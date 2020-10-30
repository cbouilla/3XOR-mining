/*
 * Copyright 2011-2014 Con Kolivas
 * Copyright 2010 Jeff Garzik
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>
#include <string.h>

#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>

#include <fcntl.h>
#include <sys/prctl.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <sched.h>

#include "miner.h"
#include "elist.h"
#include "compat.h"
#include "util.h"

#define DEFAULT_SOCKWAIT 60

bool successful_connect = false;

int no_yield(void)
{
	return 0;
}

int (*selective_yield)(void) = &no_yield;

unsigned char bit_swap_table[256] =
{
  0x00, 0x80, 0x40, 0xc0, 0x20, 0xa0, 0x60, 0xe0,
  0x10, 0x90, 0x50, 0xd0, 0x30, 0xb0, 0x70, 0xf0,
  0x08, 0x88, 0x48, 0xc8, 0x28, 0xa8, 0x68, 0xe8,
  0x18, 0x98, 0x58, 0xd8, 0x38, 0xb8, 0x78, 0xf8,
  0x04, 0x84, 0x44, 0xc4, 0x24, 0xa4, 0x64, 0xe4,
  0x14, 0x94, 0x54, 0xd4, 0x34, 0xb4, 0x74, 0xf4,
  0x0c, 0x8c, 0x4c, 0xcc, 0x2c, 0xac, 0x6c, 0xec,
  0x1c, 0x9c, 0x5c, 0xdc, 0x3c, 0xbc, 0x7c, 0xfc,
  0x02, 0x82, 0x42, 0xc2, 0x22, 0xa2, 0x62, 0xe2,
  0x12, 0x92, 0x52, 0xd2, 0x32, 0xb2, 0x72, 0xf2,
  0x0a, 0x8a, 0x4a, 0xca, 0x2a, 0xaa, 0x6a, 0xea,
  0x1a, 0x9a, 0x5a, 0xda, 0x3a, 0xba, 0x7a, 0xfa,
  0x06, 0x86, 0x46, 0xc6, 0x26, 0xa6, 0x66, 0xe6,
  0x16, 0x96, 0x56, 0xd6, 0x36, 0xb6, 0x76, 0xf6,
  0x0e, 0x8e, 0x4e, 0xce, 0x2e, 0xae, 0x6e, 0xee,
  0x1e, 0x9e, 0x5e, 0xde, 0x3e, 0xbe, 0x7e, 0xfe,
  0x01, 0x81, 0x41, 0xc1, 0x21, 0xa1, 0x61, 0xe1,
  0x11, 0x91, 0x51, 0xd1, 0x31, 0xb1, 0x71, 0xf1,
  0x09, 0x89, 0x49, 0xc9, 0x29, 0xa9, 0x69, 0xe9,
  0x19, 0x99, 0x59, 0xd9, 0x39, 0xb9, 0x79, 0xf9,
  0x05, 0x85, 0x45, 0xc5, 0x25, 0xa5, 0x65, 0xe5,
  0x15, 0x95, 0x55, 0xd5, 0x35, 0xb5, 0x75, 0xf5,
  0x0d, 0x8d, 0x4d, 0xcd, 0x2d, 0xad, 0x6d, 0xed,
  0x1d, 0x9d, 0x5d, 0xdd, 0x3d, 0xbd, 0x7d, 0xfd,
  0x03, 0x83, 0x43, 0xc3, 0x23, 0xa3, 0x63, 0xe3,
  0x13, 0x93, 0x53, 0xd3, 0x33, 0xb3, 0x73, 0xf3,
  0x0b, 0x8b, 0x4b, 0xcb, 0x2b, 0xab, 0x6b, 0xeb,
  0x1b, 0x9b, 0x5b, 0xdb, 0x3b, 0xbb, 0x7b, 0xfb,
  0x07, 0x87, 0x47, 0xc7, 0x27, 0xa7, 0x67, 0xe7,
  0x17, 0x97, 0x57, 0xd7, 0x37, 0xb7, 0x77, 0xf7,
  0x0f, 0x8f, 0x4f, 0xcf, 0x2f, 0xaf, 0x6f, 0xef,
  0x1f, 0x9f, 0x5f, 0xdf, 0x3f, 0xbf, 0x7f, 0xff,
};


struct tq_ent {
	void			*data;
	struct list_head	q_node;
};
/* Adequate size s==len*2 + 1 must be alloced to use this variant */
void __bin2hex(char *s, const unsigned char *p, size_t len)
{
	int i;
	static const char hex[16] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

	for (i = 0; i < (int)len; i++) {
		*s++ = hex[p[i] >> 4];
		*s++ = hex[p[i] & 0xF];
	}
	*s++ = '\0';
}

/* Returns a malloced array string of a binary value of arbitrary length. The
 * array is rounded up to a 4 byte size to appease architectures that need
 * aligned array  sizes */
char *bin2hex(const unsigned char *p, size_t len)
{
	ssize_t slen;
	char *s;

	slen = len * 2 + 1;
	if (slen % 4)
		slen += 4 - (slen % 4);
	s = calloc(slen, 1);
	if (unlikely(!s))
		quithere(1, "Failed to calloc");

	__bin2hex(s, p, len);

	return s;
}

static const int hex2bin_tbl[256] = {
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	 0,  1,  2,  3,  4,  5,  6,  7,  8,  9, -1, -1, -1, -1, -1, -1,
	-1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
};

/* Does the reverse of bin2hex but does not allocate any ram */
bool hex2bin(unsigned char *p, const char *hexstr, size_t len)
{
	int nibble1, nibble2;
	unsigned char idx;
	bool ret = false;

	while (*hexstr && len) {
		if (unlikely(!hexstr[1])) {
			applog(LOG_ERR, "hex2bin str truncated");
			return ret;
		}

		idx = *hexstr++;
		nibble1 = hex2bin_tbl[idx];
		idx = *hexstr++;
		nibble2 = hex2bin_tbl[idx];

		if (unlikely((nibble1 < 0) || (nibble2 < 0))) {
			applog(LOG_ERR, "hex2bin scan failed");
			return ret;
		}

		*p++ = (((unsigned char)nibble1) << 4) | ((unsigned char)nibble2);
		--len;
	}

	if (likely(len == 0 && *hexstr == 0))
		ret = true;
	return ret;
}

/* For encoding variable length strings */
unsigned char *ser_string(char *s, int *slen)
{
	size_t len = strlen(s);
	unsigned char *ret;

	ret = malloc(1 + len + 8); // Leave room for largest size
	if (unlikely(!ret))
		quit(1, "Failed to malloc ret in ser_string");
	if (len < 253) {
		ret[0] = len;
		memcpy(ret + 1, s, len);
		*slen = len + 1;
	} else if (len < 0x10000) {
		uint16_t *u16 = (uint16_t *)&ret[1];

		ret[0] = 253;
		*u16 = htobe16(len);
		memcpy(ret + 3, s, len);
		*slen = len + 3;
	} else {
		/* size_t is only 32 bit on many platforms anyway */
		uint32_t *u32 = (uint32_t *)&ret[1];

		ret[0] = 254;
		*u32 = htobe32(len);
		memcpy(ret + 5, s, len);
		*slen = len + 5;
	}
	return ret;
}

bool fulltest(const unsigned char *hash, const unsigned char *target)
{
	uint32_t *hash32 = (uint32_t *)hash;
	uint32_t *target32 = (uint32_t *)target;
	bool rc = true;
	int i;

	for (i = 28 / 4; i >= 0; i--) {
		uint32_t h32tmp = le32toh(hash32[i]);
		uint32_t t32tmp = le32toh(target32[i]);

		if (h32tmp > t32tmp) {
			rc = false;
			break;
		}
		if (h32tmp < t32tmp) {
			rc = true;
			break;
		}
	}

	if (opt_debug) {
		unsigned char hash_swap[32], target_swap[32];
		char *hash_str, *target_str;

		swab256(hash_swap, hash);
		swab256(target_swap, target);
		hash_str = bin2hex(hash_swap, 32);
		target_str = bin2hex(target_swap, 32);

		applog(LOG_DEBUG, " Proof: %s\nTarget: %s\nTrgVal? %s",
			hash_str,
			target_str,
			rc ? "YES (hash <= target)" :
			     "no (false positive; hash > target)");

		free(hash_str);
		free(target_str);
	}

	return rc;
}

struct thread_q *tq_new(void)
{
	struct thread_q *tq;

	tq = calloc(1, sizeof(*tq));
	if (!tq)
		return NULL;

	INIT_LIST_HEAD(&tq->q);
	pthread_mutex_init(&tq->mutex, NULL);
	pthread_cond_init(&tq->cond, NULL);

	return tq;
}

void tq_free(struct thread_q *tq)
{
	struct tq_ent *ent, *iter;

	if (!tq)
		return;

	list_for_each_entry_safe(ent, iter, &tq->q, q_node) {
		list_del(&ent->q_node);
		free(ent);
	}

	pthread_cond_destroy(&tq->cond);
	pthread_mutex_destroy(&tq->mutex);

	memset(tq, 0, sizeof(*tq));	/* poison */
	free(tq);
}

static void tq_freezethaw(struct thread_q *tq, bool frozen)
{
	mutex_lock(&tq->mutex);
	tq->frozen = frozen;
	pthread_cond_signal(&tq->cond);
	mutex_unlock(&tq->mutex);
}

void tq_freeze(struct thread_q *tq)
{
	tq_freezethaw(tq, true);
}

void tq_thaw(struct thread_q *tq)
{
	tq_freezethaw(tq, false);
}

bool tq_push(struct thread_q *tq, void *data)
{
	struct tq_ent *ent;
	bool rc = true;

	ent = calloc(1, sizeof(*ent));
	if (!ent)
		return false;

	ent->data = data;
	INIT_LIST_HEAD(&ent->q_node);

	mutex_lock(&tq->mutex);
	if (!tq->frozen) {
		list_add_tail(&ent->q_node, &tq->q);
	} else {
		free(ent);
		rc = false;
	}
	pthread_cond_signal(&tq->cond);
	mutex_unlock(&tq->mutex);

	return rc;
}

void *tq_pop(struct thread_q *tq, const struct timespec *abstime)
{
	struct tq_ent *ent;
	void *rval = NULL;
	int rc;

	mutex_lock(&tq->mutex);
	if (!list_empty(&tq->q))
		goto pop;

	if (abstime)
		rc = pthread_cond_timedwait(&tq->cond, &tq->mutex, abstime);
	else
		rc = pthread_cond_wait(&tq->cond, &tq->mutex);
	if (rc)
		goto out;
	if (list_empty(&tq->q))
		goto out;
pop:
	ent = list_entry(tq->q.next, struct tq_ent, q_node);
	rval = ent->data;

	list_del(&ent->q_node);
	free(ent);
out:
	mutex_unlock(&tq->mutex);

	return rval;
}

int thr_info_create(struct thr_info *thr, pthread_attr_t *attr, void *(*start) (void *), void *arg)
{
	cgsem_init(&thr->sem);

	return pthread_create(&thr->pth, attr, start, arg);
}

void thr_info_cancel(struct thr_info *thr)
{
	if (!thr)
		return;

	if (PTH(thr) != 0L) {
		pthread_cancel(thr->pth);
		PTH(thr) = 0L;
	}
	cgsem_destroy(&thr->sem);
}

void subtime(struct timeval *a, struct timeval *b)
{
	timersub(a, b, b);
}

void addtime(struct timeval *a, struct timeval *b)
{
	timeradd(a, b, b);
}

bool time_more(struct timeval *a, struct timeval *b)
{
	return timercmp(a, b, >);
}

bool time_less(struct timeval *a, struct timeval *b)
{
	return timercmp(a, b, <);
}

void copy_time(struct timeval *dest, const struct timeval *src)
{
	memcpy(dest, src, sizeof(struct timeval));
}

void timespec_to_val(struct timeval *val, const struct timespec *spec)
{
	val->tv_sec = spec->tv_sec;
	val->tv_usec = spec->tv_nsec / 1000;
}

void timeval_to_spec(struct timespec *spec, const struct timeval *val)
{
	spec->tv_sec = val->tv_sec;
	spec->tv_nsec = val->tv_usec * 1000;
}

void us_to_timeval(struct timeval *val, int64_t us)
{
	lldiv_t tvdiv = lldiv(us, 1000000);

	val->tv_sec = tvdiv.quot;
	val->tv_usec = tvdiv.rem;
}

void us_to_timespec(struct timespec *spec, int64_t us)
{
	lldiv_t tvdiv = lldiv(us, 1000000);

	spec->tv_sec = tvdiv.quot;
	spec->tv_nsec = tvdiv.rem * 1000;
}

void ms_to_timespec(struct timespec *spec, int64_t ms)
{
	lldiv_t tvdiv = lldiv(ms, 1000);

	spec->tv_sec = tvdiv.quot;
	spec->tv_nsec = tvdiv.rem * 1000000;
}

void ms_to_timeval(struct timeval *val, int64_t ms)
{
	lldiv_t tvdiv = lldiv(ms, 1000);

	val->tv_sec = tvdiv.quot;
	val->tv_usec = tvdiv.rem * 1000;
}

static void spec_nscheck(struct timespec *ts)
{
	while (ts->tv_nsec >= 1000000000) {
		ts->tv_nsec -= 1000000000;
		ts->tv_sec++;
	}
	while (ts->tv_nsec < 0) {
		ts->tv_nsec += 1000000000;
		ts->tv_sec--;
	}
}

void timeraddspec(struct timespec *a, const struct timespec *b)
{
	a->tv_sec += b->tv_sec;
	a->tv_nsec += b->tv_nsec;
	spec_nscheck(a);
}

static int __maybe_unused timespec_to_ms(struct timespec *ts)
{
	return ts->tv_sec * 1000 + ts->tv_nsec / 1000000;
}

/* Subtract b from a */
static void __maybe_unused timersubspec(struct timespec *a, const struct timespec *b)
{
	a->tv_sec -= b->tv_sec;
	a->tv_nsec -= b->tv_nsec;
	spec_nscheck(a);
}

char *Strcasestr(char *haystack, const char *needle)
{
	char *lowhay, *lowneedle, *ret;
	int hlen, nlen, i, ofs;

	if (unlikely(!haystack || !needle))
		return NULL;
	hlen = strlen(haystack);
	nlen = strlen(needle);
	if (!hlen || !nlen)
		return NULL;
	lowhay = alloca(hlen);
	lowneedle = alloca(nlen);
	for (i = 0; i < hlen; i++)
		lowhay[i] = tolower(haystack[i]);
	for (i = 0; i < nlen; i++)
		lowneedle[i] = tolower(needle[i]);
	ret = strstr(lowhay, lowneedle);
	if (!ret)
		return ret;
	ofs = ret - lowhay;
	return haystack + ofs;
}

char *Strsep(char **stringp, const char *delim)
{
	char *ret = *stringp;
	char *p;

	p = (ret != NULL) ? strpbrk(ret, delim) : NULL;

	if (p == NULL)
		*stringp = NULL;
	else {
		*p = '\0';
		*stringp = p + 1;
	}

	return ret;
}


void cgtime(struct timeval *tv)
{
	gettimeofday(tv, NULL);
}

int cgtimer_to_ms(cgtimer_t *cgt)
{
	return timespec_to_ms(cgt);
}

/* Subtracts b from a and stores it in res. */
void cgtimer_sub(cgtimer_t *a, cgtimer_t *b, cgtimer_t *res)
{
	res->tv_sec = a->tv_sec - b->tv_sec;
	res->tv_nsec = a->tv_nsec - b->tv_nsec;
	if (res->tv_nsec < 0) {
		res->tv_nsec += 1000000000;
		res->tv_sec--;
	}
}

#ifdef CLOCK_MONOTONIC /* Essentially just linux */
void cgtimer_time(cgtimer_t *ts_start)
{
	clock_gettime(CLOCK_MONOTONIC, ts_start);
}

static void nanosleep_abstime(struct timespec *ts_end)
{
	int ret;

	do {
		ret = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ts_end, NULL);
	} while (ret == EINTR);
}

/* Reentrant version of cgsleep functions allow start time to be set separately
 * from the beginning of the actual sleep, allowing scheduling delays to be
 * counted in the sleep. */
void cgsleep_ms_r(cgtimer_t *ts_start, int ms)
{
	struct timespec ts_end;

	ms_to_timespec(&ts_end, ms);
	timeraddspec(&ts_end, ts_start);
	nanosleep_abstime(&ts_end);
}

void cgsleep_us_r(cgtimer_t *ts_start, int64_t us)
{
	struct timespec ts_end;

	us_to_timespec(&ts_end, us);
	timeraddspec(&ts_end, ts_start);
	nanosleep_abstime(&ts_end);
}
#else /* CLOCK_MONOTONIC */

void cgtimer_time(cgtimer_t *ts_start)
{
	struct timeval tv;

	cgtime(&tv);
	ts_start->tv_sec = tv->tv_sec;
	ts_start->tv_nsec = tv->tv_usec * 1000;
}

static void cgsleep_spec(struct timespec *ts_diff, const struct timespec *ts_start)
{
	struct timespec now;

	timeraddspec(ts_diff, ts_start);
	cgtimer_time(&now);
	timersubspec(ts_diff, &now);
	if (unlikely(ts_diff->tv_sec < 0))
		return;
	nanosleep(ts_diff, NULL);
}

void cgsleep_ms_r(cgtimer_t *ts_start, int ms)
{
	struct timespec ts_diff;

	ms_to_timespec(&ts_diff, ms);
	cgsleep_spec(&ts_diff, ts_start);
}

void cgsleep_us_r(cgtimer_t *ts_start, int64_t us)
{
	struct timespec ts_diff;

	us_to_timespec(&ts_diff, us);
	cgsleep_spec(&ts_diff, ts_start);
}
#endif /* CLOCK_MONOTONIC */

void cgsleep_ms(int ms)
{
	cgtimer_t ts_start;

	cgsleep_prepare_r(&ts_start);
	cgsleep_ms_r(&ts_start, ms);
}

void cgsleep_us(int64_t us)
{
	cgtimer_t ts_start;

	cgsleep_prepare_r(&ts_start);
	cgsleep_us_r(&ts_start, us);
}

/* Returns the microseconds difference between end and start times as a double */
double us_tdiff(struct timeval *end, struct timeval *start)
{
	/* Sanity check. We should only be using this for small differences so
	 * limit the max to 60 seconds. */
	if (unlikely(end->tv_sec - start->tv_sec > 60))
		return 60000000;
	return (end->tv_sec - start->tv_sec) * 1000000 + (end->tv_usec - start->tv_usec);
}

/* Returns the milliseconds difference between end and start times */
int ms_tdiff(struct timeval *end, struct timeval *start)
{
	/* Like us_tdiff, limit to 1 hour. */
	if (unlikely(end->tv_sec - start->tv_sec > 3600))
		return 3600000;
	return (end->tv_sec - start->tv_sec) * 1000 + (end->tv_usec - start->tv_usec) / 1000;
}

/* Returns the seconds difference between end and start times as a double */
double tdiff(struct timeval *end, struct timeval *start)
{
	return end->tv_sec - start->tv_sec + (end->tv_usec - start->tv_usec) / 1000000.0;
}


/* Realloc memory to new size and zero any extra memory added */
void _recalloc(void **ptr, size_t old, size_t new, const char *file, const char *func, const int line)
{
	if (new == old)
		return;
	*ptr = realloc(*ptr, new);
	if (unlikely(!*ptr))
		quitfrom(1, file, func, line, "Failed to realloc");
	if (new > old)
		memset(*ptr + old, 0, new - old);
}


void dev_error(struct cgpu_info *dev, enum dev_reason reason)
{
	dev->device_last_not_well = time(NULL);
	dev->device_not_well_reason = reason;

	switch (reason) {
		case REASON_THREAD_FAIL_INIT:
			dev->thread_fail_init_count++;
			break;
		case REASON_THREAD_ZERO_HASH:
			dev->thread_zero_hash_count++;
			break;
		case REASON_THREAD_FAIL_QUEUE:
			dev->thread_fail_queue_count++;
			break;
		case REASON_DEV_SICK_IDLE_60:
			dev->dev_sick_idle_60_count++;
			break;
		case REASON_DEV_DEAD_IDLE_600:
			dev->dev_dead_idle_600_count++;
			break;
		case REASON_DEV_NOSTART:
			dev->dev_nostart_count++;
			break;
		case REASON_DEV_OVER_HEAT:
			dev->dev_over_heat_count++;
			break;
		case REASON_DEV_THERMAL_CUTOFF:
			dev->dev_thermal_cutoff_count++;
			break;
		case REASON_DEV_COMMS_ERROR:
			dev->dev_comms_error_count++;
			break;
		case REASON_DEV_THROTTLE:
			dev->dev_throttle_count++;
			break;
	}
}

/* Realloc an existing string to fit an extra string s, appending s to it. */
void *realloc_strcat(char *ptr, char *s)
{
	size_t old = 0, len = strlen(s);
	char *ret;

	if (!len)
		return ptr;
	if (ptr)
		old = strlen(ptr);

	len += old + 1;
	align_len(&len);

	ret = malloc(len);
	if (unlikely(!ret))
		quithere(1, "Failed to malloc");

	if (ptr) {
		sprintf(ret, "%s%s", ptr, s);
		free(ptr);
	} else
		sprintf(ret, "%s", s);
	return ret;
}

/* Make a text readable version of a string using 0xNN for < ' ' or > '~'
 * Including 0x00 at the end
 * You must free the result yourself */
void *str_text(char *ptr)
{
	unsigned char *uptr;
	char *ret, *txt;

	if (ptr == NULL) {
		ret = strdup("(null)");

		if (unlikely(!ret))
			quithere(1, "Failed to malloc null");
	}

	uptr = (unsigned char *)ptr;

	ret = txt = malloc(strlen(ptr)*4+5); // Guaranteed >= needed
	if (unlikely(!txt))
		quithere(1, "Failed to malloc txt");

	do {
		if (*uptr < ' ' || *uptr > '~') {
			sprintf(txt, "0x%02x", *uptr);
			txt += 4;
		} else
			*(txt++) = *uptr;
	} while (*(uptr++));

	*txt = '\0';

	return ret;
}

void RenameThread(const char* name)
{
	char buf[16];

	snprintf(buf, sizeof(buf), "cg@%s", name);
#if defined(PR_SET_NAME)
	// Only the first 15 characters are used (16 - NUL terminator)
	prctl(PR_SET_NAME, buf, 0, 0, 0);
#elif (defined(__FreeBSD__) || defined(__OpenBSD__))
	pthread_set_name_np(pthread_self(), buf);
#elif defined(MAC_OSX)
	pthread_setname_np(buf);
#else
	// Prevent warnings
	(void)buf;
#endif
}

/* cgminer specific wrappers for true unnamed semaphore usage on platforms
 * that support them and for apple which does not. We use a single byte across
 * a pipe to emulate semaphore behaviour there. */
void _cgsem_init(cgsem_t *cgsem, const char *file, const char *func, const int line)
{
	int ret;
	if ((ret = sem_init(cgsem, 0, 0)))
		quitfrom(1, file, func, line, "Failed to sem_init ret=%d errno=%d", ret, errno);
}

void _cgsem_post(cgsem_t *cgsem, const char *file, const char *func, const int line)
{
	if (unlikely(sem_post(cgsem)))
		quitfrom(1, file, func, line, "Failed to sem_post errno=%d cgsem=0x%p", errno, cgsem);
}

void _cgsem_wait(cgsem_t *cgsem, const char *file, const char *func, const int line)
{
retry:
	if (unlikely(sem_wait(cgsem))) {
		if (interrupted())
			goto retry;
		quitfrom(1, file, func, line, "Failed to sem_wait errno=%d cgsem=0x%p", errno, cgsem);
	}
}

int _cgsem_mswait(cgsem_t *cgsem, int ms, const char *file, const char *func, const int line)
{
	struct timespec abs_timeout, ts_now;
	struct timeval tv_now;
	int ret;

	cgtime(&tv_now);
	timeval_to_spec(&ts_now, &tv_now);
	ms_to_timespec(&abs_timeout, ms);
retry:
	timeraddspec(&abs_timeout, &ts_now);
	ret = sem_timedwait(cgsem, &abs_timeout);

	if (ret) {
		if (likely(sock_timeout()))
			return ETIMEDOUT;
		if (interrupted())
			goto retry;
		quitfrom(1, file, func, line, "Failed to sem_timedwait errno=%d cgsem=0x%p", errno, cgsem);
	}
	return 0;
}

void cgsem_reset(cgsem_t *cgsem)
{
	int ret;

	do {
		ret = sem_trywait(cgsem);
		if (unlikely(ret < 0 && interrupted()))
			ret = 0;
	} while (!ret);
}

void cgsem_destroy(cgsem_t *cgsem)
{
	sem_destroy(cgsem);
}

/* Provide a completion_timeout helper function for unreliable functions that
 * may die due to driver issues etc that time out if the function fails and
 * can then reliably return. */
struct cg_completion {
	cgsem_t cgsem;
	void (*fn)(void *fnarg);
	void *fnarg;
};

void *completion_thread(void *arg)
{
	struct cg_completion *cgc = (struct cg_completion *)arg;

	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
	cgc->fn(cgc->fnarg);
	cgsem_post(&cgc->cgsem);

	return NULL;
}

bool cg_completion_timeout(void *fn, void *fnarg, int timeout)
{
	struct cg_completion *cgc;
	pthread_t pthread;
	bool ret = false;

	cgc = malloc(sizeof(struct cg_completion));
	if (unlikely(!cgc))
		return ret;
	cgsem_init(&cgc->cgsem);
	cgc->fn = fn;
	cgc->fnarg = fnarg;

	pthread_create(&pthread, NULL, completion_thread, (void *)cgc);

	ret = cgsem_mswait(&cgc->cgsem, timeout);
	if (!ret) {
		pthread_join(pthread, NULL);
		free(cgc);
	} else
		pthread_cancel(pthread);
	return !ret;
}

void _cg_memcpy(void *dest, const void *src, unsigned int n, const char *file, const char *func, const int line)
{
	if (unlikely(n < 1 || n > (1ul << 31))) {
		applog(LOG_ERR, "ERR: Asked to memcpy %u bytes from %s %s():%d",
			      n, file, func, line);
		return;
	}
	memcpy(dest, src, n);
}

int cg_timeval_subtract(struct timeval* result, struct timeval* x, struct timeval* y)
{
	if(x->tv_sec > y->tv_sec)
		return -1;

	if((x->tv_sec == y->tv_sec) && (x->tv_usec > y->tv_usec))
		return -1;

	result->tv_sec = (y->tv_sec - x->tv_sec);
	result->tv_usec = (y->tv_usec - x->tv_usec);

	if(result->tv_usec < 0)
	{
		result->tv_sec--;
		result->tv_usec += 1000000;
	}
	return 0;
}

void rev(unsigned char *s, size_t l)
{
	size_t i, j;
	unsigned char t;

	for (i = 0, j = l - 1; i < j; i++, j--) {
		t = s[i];
		s[i] = s[j];
		s[j] = t;
	}
}

int check_asicnum(int asic_num, unsigned char nonce)
{
	switch(asic_num)
	{
	case 1:
		return 1;
	case 2:
		switch(nonce & 0x80)
		{
		case 0x80: return 2;
		default: return 1;
		}
	case 4:
		switch(nonce & 0xC0)
		{
		case 0xC0: return 4;
		case 0x80: return 3;
		case 0x40: return 2;
		default: return 1;
		}
	case 8:
		switch(nonce & 0xE0)
		{
		case 0xE0: return 8;
		case 0xC0: return 7;
		case 0xA0: return 6;
		case 0x80: return 5;
		case 0x60: return 4;
		case 0x40: return 3;
		case 0x20: return 2;
		default : return 1;
		}
	case 16:
		switch(nonce & 0xF0)
		{
		case 0xF0: return 16;
		case 0xE0: return 15;
		case 0xD0: return 14;
		case 0xC0: return 13;
		case 0xB0: return 12;
		case 0xA0: return 11;
		case 0x90: return 10;
		case 0x80: return 9;
		case 0x70: return 8;
		case 0x60: return 7;
		case 0x50: return 6;
		case 0x40: return 5;
		case 0x30: return 4;
		case 0x20: return 3;
		case 0x10: return 2;
		default : return 1;
		}
	case 32:
		switch(nonce & 0xF8)
		{
		case 0xF8: return 32;
		case 0xF0: return 31;
		case 0xE8: return 30;
		case 0xE0: return 29;
		case 0xD8: return 28;
		case 0xD0: return 27;
		case 0xC8: return 26;
		case 0xC0: return 25;
		case 0xB8: return 24;
		case 0xB0: return 23;
		case 0xA8: return 22;
		case 0xA0: return 21;
		case 0x98: return 20;
		case 0x90: return 19;
		case 0x88: return 18;
		case 0x80: return 17;
		case 0x78: return 16;
		case 0x70: return 15;
		case 0x68: return 14;
		case 0x60: return 13;
		case 0x58: return 12;
		case 0x50: return 11;
		case 0x48: return 10;
		case 0x40: return 9;
		case 0x38: return 8;
		case 0x30: return 7;
		case 0x28: return 6;
		case 0x20: return 5;
		case 0x18: return 4;
		case 0x10: return 3;
		case 0x08: return 2;
		default : return 1;
		}
	case 64:
		switch(nonce & 0xFC)
		{
		case 0xFC: return 64;
		case 0xF8: return 63;
		case 0xF4: return 62;
		case 0xF0: return 61;
		case 0xEC: return 60;
		case 0xE8: return 59;
		case 0xE4: return 58;
		case 0xE0: return 57;
		case 0xDC: return 56;
		case 0xD8: return 55;
		case 0xD4: return 54;
		case 0xD0: return 53;
		case 0xCC: return 52;
		case 0xC8: return 51;
		case 0xC4: return 50;
		case 0xC0: return 49;
		case 0xBC: return 48;
		case 0xB8: return 47;
		case 0xB4: return 46;
		case 0xB0: return 45;
		case 0xAC: return 44;
		case 0xA8: return 43;
		case 0xA4: return 42;
		case 0xA0: return 41;
		case 0x9C: return 40;
		case 0x98: return 39;
		case 0x94: return 38;
		case 0x90: return 37;
		case 0x8C: return 36;
		case 0x88: return 35;
		case 0x84: return 34;
		case 0x80: return 33;
		case 0x7C: return 32;
		case 0x78: return 31;
		case 0x74: return 30;
		case 0x70: return 29;
		case 0x6C: return 28;
		case 0x68: return 27;
		case 0x64: return 26;
		case 0x60: return 25;
		case 0x5C: return 24;
		case 0x58: return 23;
		case 0x54: return 22;
		case 0x50: return 21;
		case 0x4C: return 20;
		case 0x48: return 19;
		case 0x44: return 18;
		case 0x40: return 17;
		case 0x3C: return 16;
		case 0x38: return 15;
		case 0x34: return 14;
		case 0x30: return 13;
		case 0x2C: return 12;
		case 0x28: return 11;
		case 0x24: return 10;
		case 0x20: return 9;
		case 0x1C: return 8;
		case 0x18: return 7;
		case 0x14: return 6;
		case 0x10: return 5;
		case 0x0C: return 4;
		case 0x08: return 3;
		case 0x04: return 2;
		default : return 1;
		}
	default:
		return 0;
	}
}
