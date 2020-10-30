// #define LOCAL
/*
 * Version courte. la fonction main() invoque gen_stratum_work() dans une boucle.
 * Ceci prépare un objet work. Il est ensuite enfilé dans une queue avec stage_work(),
 * qui invoque hash_push(). La table de hachage est nommée staged_work (cf. uthash.h).
 *
 * Parallèlement, le thread de minage exécute hash_queued_work(). Là, une boucle infinie
 * exécute fill_queue(). Là, get_work() est appelé tant que nécessaire. Cela invoque hash_pop()
 * pour récupérer un work dans la table de hachage staged_work. Le processus est répété
 * tant que driver-bitmain/bitmain_fill() décide que c'est suffisant.
 *
 * Apparement, le device gère un FIFO, dont il communique la taille lors de l'initialisation.
 *
 * bitmain_fill gère aussi une AUTRE queue, planquée dans cgpu. Tant qu'elle n'est pas pleine,
 * la fonction get_queued() est invoquée. Celle-ci récupère un éventuel work dans cgpu->unqueued_work,
 * et s'il est présent il est ajouté à la file d'attente dans cgpu->queued_work.
 *
 * Enfin, un thread est lancé lors de l'initialisation du device et exécute bitmain_get_results.
 * Cette fonction essaye en permanence de lire le device. Si assez de données ont été accumulées,
 * alors bitmain_parse_results est invoqué. Il peut s'agir d'info de status ou bien... de nonces.
 * Dans ce cas, submit_nonce() est invoqué, et à terme submit_tested_work, submit_work_async()
 *
 * Côté CGPU, la queue "logicielle" est de taille 4000, la queue matérielle de taille 16000, apparemment.
 *
 * Pour la pool stratum : 2 threads (un qui lit les notice et update le staged_work, un qui pousse les résultats)
 * Pour chaque device : 1 thread qui pousse le staged_work dans la queue du device, et...
 *
 * Plan A
 * ======
 *    Ignorer le fait qu'il y a zéro pool
 *    Générer les works en boucle dans main()
 *    Modifier send_work_async pour sauvegarder les résultats
 *
 * Suite du Plan A
 * ===============
 *
 * Rapprocher la production du work de sa consommation. Eviter les queue et les context switches.
 *
 * Flot normal du work. 
 * 1) main -> Il est créé.
 * 2) main -> Il est staged. work->work_block := work_block (variable globale)
 *
 * 3) get_work -> Il est extrait de la file "staged". Son attribut "mined" passe à true. Son attribut "device_diff" est fixé.
 * 4) fill_queue -> Il devient gpu->unqueued_work
 * 5) __add_queued() -> il est ajouté à cgpu->queued_work
 * 5.1) Il est renvoyé à bitmain_fill. Là, un numéro (slot) lui est affecté. S'il y a ddéjà quelque chose, 
	   c'est que ce quelque chose est un ancien work terminé. Cans ce cas-là : 
		work_completed --> retire de cgpu->queued_work, et free_work() sur l'ancien work.
	   le work est envoyé au FPGA
 * 
 * 6) De retour du FPGA, on a un work_id. --> clone_queued_work_byid() renvoie une copie fraiche du work.
 *
 *
 * Copyright 2011-2014 Con Kolivas
 * Copyright 2011-2012 Luke Dashjr
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
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <math.h>
#include <stdarg.h>
#include <assert.h>
#include <signal.h>
#include <limits.h>
#include <semaphore.h>

#include <sys/stat.h>
#include <sys/types.h>

#include <sys/resource.h>
#include <ccan/opt/opt.h>
#include <jansson.h>
char *curly = ":D";

#include <libgen.h>
#include <sha2.h>

#include "compat.h"
#include "miner.h"
#include "usbutils.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>

#include <err.h>
#include <nanomsg/nn.h>
#include <nanomsg/reqrep.h>
#include <nanomsg/pipeline.h>

#include "driver-bitmain.h"
#define USE_FPGA

#include <inttypes.h>

struct strategies strategies[] = {
	{ "Failover" },
	{ "Round Robin" },
	{ "Rotate" },
	{ "Load Balance" },
	{ "Balance" },
};

static char packagename[256];

FILE * g_logwork_file = NULL;
FILE * g_logwork_files[65] = {0};
FILE * g_logwork_diffs[65] = {0};
int g_logwork_asicnum = 0;

bool opt_work_update;
bool opt_protocol;

// CB stats
int staged_work_items = 0;
int nonce_reported = 0;

bool have_longpoll;
bool want_per_device_stats;
bool use_syslog;
bool opt_quiet;
bool opt_realquiet;
bool opt_loginput;
bool opt_compact;
const int opt_cutofftemp = 95;
int opt_log_interval = 5;
int opt_queue = 1;
static int max_queue = 2;
int opt_scantime = -1;
int opt_expiry = 120;
unsigned long long global_hashrate;
unsigned long global_quota_gcd = 1;
time_t last_getwork;


int nDevs;
bool opt_restart = true;
bool opt_nogpu;

struct list_head scan_devices;
int total_devices;
int zombie_devs;
static int most_devices;
struct cgpu_info **devices;
int mining_threads;
int num_processors;

bool use_curses = false;

static bool opt_widescreen;
static bool alt_status;
static bool switch_status;
bool opt_fail_only;
bool opt_lowmem;
bool opt_autofan;
bool opt_autoengine;
bool opt_noadl;
char *opt_version_path = NULL;
char *opt_logfile_path = NULL;
const char *opt_logfile_openflag = NULL;
const char *opt_logwork_path = NULL;
const char *opt_logwork_asicnum = NULL;
const bool opt_logwork_diff = false;

const bool opt_delaynet;
const bool opt_disable_pool;
static bool no_work;

bool opt_worktime;

char *opt_bitmain_options = NULL;
char *opt_bitmain_freq = NULL;
char *opt_bitmain_voltage = NULL;

char *opt_usb_select = NULL;
int opt_usbdump = -1;
bool opt_usb_list_all;
cgsem_t usb_resource_sem;


char *opt_kernel_path;
char *cgminer_path;

#define QUIET	(opt_quiet || opt_realquiet)

struct thr_info *control_thr;
struct thr_info **mining_thr;
static int gwsched_thr_id;
static int watchpool_thr_id;
static int watchdog_thr_id;

int gpur_thr_id;
// static int api_thr_id;


static int hotplug_thr_id;

static int total_control_threads;
bool hotplug_mode;
static int new_devices;
static int new_threads;
int hotplug_time = 5;

#if LOCK_TRACKING
pthread_mutex_t lockstat_lock;
#endif

pthread_mutex_t hash_lock;
static pthread_mutex_t *stgd_lock;
pthread_mutex_t console_lock;
cglock_t ch_lock;
static pthread_rwlock_t blk_lock;
static pthread_mutex_t sshare_lock;

pthread_rwlock_t netacc_lock;
pthread_rwlock_t mining_thr_lock;
pthread_rwlock_t devices_lock;

static pthread_mutex_t lp_lock;
static pthread_cond_t lp_cond;

pthread_mutex_t restart_lock;
pthread_cond_t restart_cond;

pthread_cond_t gws_cond;

#define CG_LOCAL_MHASHES_MAX_NUM 12
double g_local_mhashes_dones[CG_LOCAL_MHASHES_MAX_NUM] = {0};
int g_local_mhashes_index = 0;
double g_displayed_rolling = 0;
char g_miner_version[256] = {0};
char g_miner_compiletime[256] = {0};
char g_miner_type[256] = {0};

double rolling1, rolling5, rolling15;
double total_rolling;
double total_mhashes_done;
static struct timeval total_tv_start, total_tv_end;
static struct timeval restart_tv_start, update_tv_start;

cglock_t control_lock;
pthread_mutex_t stats_lock;

int hw_errors;
int g_max_fan, g_max_temp;
int64_t total_accepted, total_rejected, total_diff1;
int64_t total_getworks, total_stale, total_discarded;
double total_diff_accepted, total_diff_rejected, total_diff_stale;
static int staged_rollable;
unsigned int new_blocks;
static unsigned int work_block = 0;
unsigned int found_blocks;

unsigned int local_work;
unsigned int local_work_last = 0;
long local_work_lasttime = 0;
unsigned int total_go, total_ro;
int opt_rotate_period;

static const bool curses_active;

/* Protected by ch_lock */
char current_hash[68];



static char datestamp[40];

struct timeval block_timeval;
static char best_share[8] = "0";
double current_diff = 0xFFFFFFFFFFFFFFFFULL;
uint64_t best_diff = 0;

struct block {
	char hash[68];
	UT_hash_handle hh;
	int block_no;
};

static struct block *blocks = NULL;


int swork_id;


const char *opt_socks_proxy = NULL;
int opt_suggest_diff;
static int forkpid;


struct sigaction termhandler, inthandler;

struct thread_q *getq;

static uint32_t total_work;
struct work *staged_work = NULL;

struct schedtime {
	bool enable;
	struct tm tm;
};

struct schedtime schedstart;
struct schedtime schedstop;
bool sched_paused;

// nanomsg adresses
char *nn_req_address;
char *nn_push_address;


void nn_fatal(const char *func)
{
	fprintf(stderr, "%s: %s\n", func, nn_strerror(nn_errno()));
	exit(1);
}

void get_datestamp(char *f, size_t fsiz, struct timeval *tv)
{
	struct tm *tm;

	const time_t tmp_time = tv->tv_sec;
	tm = localtime(&tmp_time);
	snprintf(f, fsiz, "[%d-%02d-%02d %02d:%02d:%02d]",
		tm->tm_year + 1900,
		tm->tm_mon + 1,
		tm->tm_mday,
		tm->tm_hour,
		tm->tm_min,
		tm->tm_sec);
}


static char exit_buf[512];

static void applog_and_exit(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(exit_buf, sizeof(exit_buf), fmt, ap);
	va_end(ap);
	_applog(LOG_ERR, exit_buf, true);
	exit(1);
}

static pthread_mutex_t sharelog_lock;


static struct thr_info *__get_thread(int thr_id)
{
	return mining_thr[thr_id];
}

struct thr_info *get_thread(int thr_id)
{
	struct thr_info *thr;

	rd_lock(&mining_thr_lock);
	thr = __get_thread(thr_id);
	rd_unlock(&mining_thr_lock);

	return thr;
}

static struct cgpu_info *get_thr_cgpu(int thr_id)
{
	struct thr_info *thr = get_thread(thr_id);

	return thr->cgpu;
}

struct cgpu_info *get_devices(int id)
{
	struct cgpu_info *cgpu;

	rd_lock(&devices_lock);
	cgpu = devices[id];
	rd_unlock(&devices_lock);

	return cgpu;
}


char *set_int_range(const char *arg, int *i, int min, int max)
{
	char *err = opt_set_intval(arg, i);

	if (err)
		return err;

	if (*i < min || *i > max)
		return "Value out of range";

	return NULL;
}

static char *set_int_0_to_9999(const char *arg, int *i)
{
	return set_int_range(arg, i, 0, 9999);
}

static char *set_int_0_to_100(const char *arg, int *i)
{
	return set_int_range(arg, i, 0, 100);
}

static char __maybe_unused *set_int_0_to_4(const char *arg, int *i)
{
	return set_int_range(arg, i, 0, 4);
}

#ifdef USE_FPGA_SERIAL
static char *opt_add_serial;
static char *add_serial(char *arg)
{
	string_elist_add(arg, &scan_devices);
	return NULL;
}
#endif

void get_intrange(char *arg, int *val1, int *val2)
{
	if (sscanf(arg, "%d-%d", val1, val2) == 1)
		*val2 = *val1;
}



static char *enable_debug(bool *flag)
{
	*flag = true;
	/* Turn on verbose output, too. */
	opt_log_output = true;
	return NULL;
}

static char *temp_cutoff_str = NULL;
static char __maybe_unused *opt_set_temp_cutoff;

char *set_temp_cutoff(char *arg)
{
	int val;

	if (!(arg && arg[0]))
		return "Invalid parameters for set temp cutoff";
	val = atoi(arg);
	if (val < 0 || val > 200)
		return "Invalid value passed to set temp cutoff";
	temp_cutoff_str = arg;

	return NULL;
}

static void load_temp_cutoffs()
{
	int i, val = 0, device = 0;
	char *nextptr;

	if (temp_cutoff_str) {
		for (device = 0, nextptr = strtok(temp_cutoff_str, ","); nextptr; ++device, nextptr = strtok(NULL, ",")) {
			if (device >= total_devices)
				quit(1, "Too many values passed to set temp cutoff");
			val = atoi(nextptr);
			if (val < 0 || val > 200)
				quit(1, "Invalid value passed to set temp cutoff");

			rd_lock(&devices_lock);
			devices[device]->cutofftemp = val;
			rd_unlock(&devices_lock);
		}
	} else {
		rd_lock(&devices_lock);
		for (i = device; i < total_devices; ++i) {
			if (!devices[i]->cutofftemp)
				devices[i]->cutofftemp = opt_cutofftemp;
		}
		rd_unlock(&devices_lock);

		return;
	}
	if (device <= 1) {
		rd_lock(&devices_lock);
		for (i = device; i < total_devices; ++i)
			devices[i]->cutofftemp = val;
		rd_unlock(&devices_lock);
	}
}

static char *set_logfile_path(const char *arg)
{
	opt_set_charp(arg, &opt_logfile_path);

	return NULL;
}


static char *set_bitmain_options(const char *arg)
{
	opt_set_charp(arg, &opt_bitmain_options);

	return NULL;
}
static char *set_bitmain_freq(const char *arg)
{
	opt_set_charp(arg, &opt_bitmain_freq);

	return NULL;
}
static char *set_bitmain_voltage(const char *arg)
{
	opt_set_charp(arg, &opt_bitmain_voltage);

	return NULL;
}



/* These options are available from config file or commandline */
static struct opt_table opt_config_table[] = {
	OPT_WITH_ARG("--logfile",
				set_logfile_path, NULL, opt_hidden,
				"Set log file, default: cgminer.log"),	
	
// bitmain stuff
	OPT_WITH_ARG("--bitmain-dev",
			set_bitmain_dev, NULL, NULL,
			"Set bitmain device (default: usb mode, other windows: COM1 or linux: /dev/bitmain-asic)"),
	OPT_WITHOUT_ARG("--bitmain-hwerror",
			opt_set_bool, &opt_bitmain_hwerror,
			"Set bitmain device detect hardware error"),
	OPT_WITHOUT_ARG("--bitmain-checkall",
			opt_set_bool, &opt_bitmain_checkall,
			"Set bitmain check all"),
	OPT_WITHOUT_ARG("--bitmain-checkn2diff",
			opt_set_bool, &opt_bitmain_checkn2diff,
			"Set bitmain check not 2 pow diff"),
	OPT_WITHOUT_ARG("--bitmain-nobeeper",
			opt_set_bool, &opt_bitmain_nobeeper,
			"Set bitmain beeper no ringing"),
	OPT_WITHOUT_ARG("--bitmain-notempoverctrl",
			opt_set_bool, &opt_bitmain_notempoverctrl,
			"Set bitmain not stop runing when temprerature is over 80 degree Celsius"),
	OPT_WITHOUT_ARG("--bitmain-auto",
			opt_set_bool, &opt_bitmain_auto,
			"Adjust bitmain overclock frequency dynamically for best hashrate"),
	OPT_WITHOUT_ARG("--bitmain-homemode",
			opt_set_bool, &opt_bitmain_homemode,
			"Set bitmain miner to home mode"),
	OPT_WITHOUT_ARG("--bitmain-use-vil",
			opt_set_bool, &opt_bitmain_new_cmd_type_vil,
			"Set bitmain miner use vil mode"),
	OPT_WITH_ARG("--bitmain-cutoff",
		     set_int_0_to_100, opt_show_intval, &opt_bitmain_overheat,
		     "Set bitmain overheat cut off temperature"),
	OPT_WITH_ARG("--bitmain-fan",
		     set_bitmain_fan, NULL, NULL,
		     "Set fanspeed percentage for bitmain, single value or range (default: 20-100)"),
	OPT_WITH_ARG("--bitmain-freq",
		     set_bitmain_freq, NULL, NULL,
		     "Set frequency"),
	OPT_WITH_ARG("--bitmain-voltage",
		     set_bitmain_voltage, NULL, NULL,
		     "Set voltage"),
	OPT_WITH_ARG("--bitmain-options",
		     set_bitmain_options, NULL, NULL,
		     "Set bitmain options baud:miners:asic:timeout:freq"),
	OPT_WITH_ARG("--bitmain-temp",
		     set_int_0_to_100, opt_show_intval, &opt_bitmain_temp,
		     "Set bitmain target temperature"),


	OPT_WITH_ARG("--queue|-Q",
		     set_int_0_to_9999, opt_show_intval, &opt_queue,
		     "Maximum number of work items to have queued"),
	OPT_WITHOUT_ARG("--quiet|-q",
			opt_set_bool, &opt_quiet,
			"Disable logging output, display status and errors"),
	OPT_WITHOUT_ARG("--real-quiet",
			opt_set_bool, &opt_realquiet,
			"Disable all output"),
	OPT_WITHOUT_ARG("--debug|-D",
		     enable_debug, &opt_debug,
		     "Enable debug output"),
	OPT_WITHOUT_ARG("--text-only|-T",
			opt_set_invbool, &use_curses,
			opt_hidden
	),
	OPT_WITHOUT_ARG("--verbose",
			opt_set_bool, &opt_log_output,
			"Log verbose output to stderr as well as status output"),
	OPT_WITHOUT_ARG("--widescreen",
			opt_set_bool, &opt_widescreen,
			"Use extra wide display without toggling"),
	OPT_WITHOUT_ARG("--worktime",
			opt_set_bool, &opt_worktime,
			"Display extra work time debug information"),
	OPT_ENDTABLE
};

static int fileconf_load;
char *cnfbuf = NULL;

extern const char *opt_argv0;

static char *opt_verusage_and_exit(const char *extra)
{
	printf("%s\nCustom Built by C. Bouillaguet for Bitmain miners\n", packagename);
	printf("%s", opt_usage(opt_argv0, extra));
	fflush(stdout);
	exit(0);
}

char *display_devs(int *ndevs)
{
	exit(*ndevs);
}


/* These options are available from commandline only */
static struct opt_table opt_cmdline_table[] = {
	OPT_WITHOUT_ARG("--help|-h",
			opt_verusage_and_exit, NULL,
			"Print this message"),
	OPT_WITHOUT_ARG("--version|-V",
			opt_version_and_exit, packagename,
			"Display version and exit"),
	OPT_ENDTABLE
};



static void calc_midstate(struct work *work)
{
	unsigned char data[64];
	uint32_t *data32 = (uint32_t *)data;
	sha256_ctx ctx;

	flip64(data32, work->data);
	sha256_init(&ctx);
	sha256_update(&ctx, data, 64);
	memcpy(work->midstate, ctx.h, 32);
	endian_flip32(work->midstate, work->midstate);
}

/* Returns the current value of total_work and increments it */
static int total_work_inc(void)
{
	int ret;

	cg_wlock(&control_lock);
	ret = total_work++;
	cg_wunlock(&control_lock);

	return ret;
}

static struct work *make_work(void)
{
	struct work *work = calloc(1, sizeof(struct work));

	if (unlikely(!work))
		quit(1, "Failed to calloc work in make_work");

	work->id = total_work_inc();

	return work;
}

/* This is the central place all work that is about to be retired should be
 * cleaned to remove any dynamically allocated arrays within the struct */
void clean_work(struct work *work)
{
	free(work->job_id);
	free(work->ntime);
	free(work->coinbase);
	free(work->nonce1);
	memset(work, 0, sizeof(struct work));
}

/* All dynamically allocated work structs should be freed here to not leak any
 * ram from arrays allocated within the work struct */
void _free_work(struct work *work)
{
	clean_work(work);
	free(work);
}

static void calc_diff(struct work *work, double known);
char *workpadding = "000000800000000000000000000000000000000000000000000000000000000000000000000000000000000080020000";


/* Always true with stratum */
#define pool_localgen(pool) (true)
#define json_rpc_call(curl, url, userpass, rpc_req, probe, longpoll, rolltime, pool, share) (NULL)
#define work_decode(pool, work, val) (false)
#define gen_gbt_work(pool, work) {}

int dev_from_id(int thr_id)
{
	struct cgpu_info *cgpu = get_thr_cgpu(thr_id);

	return cgpu->device_id;
}

/* Create an exponentially decaying average over the opt_log_interval */
void decay_time(double *f, double fadd, double fsecs, double interval)
{
	double ftotal, fprop;

	if (fsecs <= 0)
		return;
	fprop = 1.0 - 1 / (exp(fsecs / interval));
	ftotal = 1.0 + fprop;
	*f += (fadd / fsecs * fprop);
	*f /= ftotal;
}

static int __total_staged(void)
{
	return HASH_COUNT(staged_work);
}


double total_secs = 1.0;
double last_total_secs = 1.0;
static char statusline[256];
/* logstart is where the log window should start */
static int devcursor, logstart, logcursor;


/* Convert a uint64_t value into a truncated string for displaying with its
 * associated suitable for Mega, Giga etc. Buf array needs to be long enough */
static void suffix_string(uint64_t val, char *buf, size_t bufsiz, int sigdigits)
{
	const double  dkilo = 1000.0;
	const uint64_t kilo = 1000ull;
	const uint64_t mega = 1000000ull;
	const uint64_t giga = 1000000000ull;
	const uint64_t tera = 1000000000000ull;
	const uint64_t peta = 1000000000000000ull;
	const uint64_t exa  = 1000000000000000000ull;
	char suffix[2] = "";
	bool decimal = true;
	double dval;

	if (val >= exa) {
		val /= peta;
		dval = (double)val / dkilo;
		strcpy(suffix, "E");
	} else if (val >= peta) {
		val /= tera;
		dval = (double)val / dkilo;
		strcpy(suffix, "P");
	} else if (val >= tera) {
		val /= giga;
		dval = (double)val / dkilo;
		strcpy(suffix, "T");
	} else if (val >= giga) {
		val /= mega;
		dval = (double)val / dkilo;
		strcpy(suffix, "G");
	} else if (val >= mega) {
		val /= kilo;
		dval = (double)val / dkilo;
		strcpy(suffix, "M");
	} else if (val >= kilo) {
		dval = (double)val / dkilo;
		strcpy(suffix, "K");
	} else {
		dval = val;
		decimal = false;
	}

	if (!sigdigits) {
		if (decimal)
			snprintf(buf, bufsiz, "%.3g%s", dval, suffix);
		else
			snprintf(buf, bufsiz, "%d%s", (unsigned int)dval, suffix);
	} else {
		/* Always show sigdigits + 1, padded on right with zeroes
		 * followed by suffix */
		int ndigits = sigdigits - 1 - (dval > 0.0 ? floor(log10(dval)) : 0);

		snprintf(buf, bufsiz, "%*.*f%s", sigdigits + 1, ndigits, dval, suffix);
	}
}

double cgpu_runtime(struct cgpu_info *cgpu)
{
	struct timeval now;
	double dev_runtime;

	if (cgpu->dev_start_tv.tv_sec == 0)
		dev_runtime = total_secs;
	else {
		cgtime(&now);
		dev_runtime = tdiff(&now, &(cgpu->dev_start_tv));
	}

	if (dev_runtime < 1.0)
		dev_runtime = 1.0;
	return dev_runtime;
}

double tsince_restart(void)
{
	struct timeval now;

	cgtime(&now);
	return tdiff(&now, &restart_tv_start);
}

double tsince_update(void)
{
	struct timeval now;

	cgtime(&now);
	return tdiff(&now, &update_tv_start);
}

static void get_statline(char *buf, size_t bufsiz, struct cgpu_info *cgpu)
{
	char displayed_hashes[16], displayed_rolling[16];
	double dev_runtime, wu;
	uint64_t dh64, dr64;

	dev_runtime = cgpu_runtime(cgpu);

	wu = cgpu->diff1 / dev_runtime * 60.0;

	dh64 = (double)cgpu->total_mhashes / dev_runtime * 1000000ull;
	dr64 = (double)cgpu->rolling * 1000000ull;
	suffix_string(dh64, displayed_hashes, sizeof(displayed_hashes), 4);
	suffix_string(dr64, displayed_rolling, sizeof(displayed_rolling), 4);

	snprintf(buf, bufsiz, "%s%d ", cgpu->drv->name, cgpu->device_id);
	cgpu->drv->get_statline_before(buf, bufsiz, cgpu);
	tailsprintf(buf, bufsiz, "(%ds):%s (avg):%sh/s | A:%.0f R:%.0f HW:%d WU:%.1f/m",
		opt_log_interval,
		displayed_rolling,
		displayed_hashes,
		cgpu->diff_accepted,
		cgpu->diff_rejected,
		cgpu->hw_errors,
		wu);
	cgpu->drv->get_statline(buf, bufsiz, cgpu);
}



/* truediffone == 0x00000000FFFF0000000000000000000000000000000000000000000000000000
 * Generate a 256 bit binary LE target by cutting up diff into 64 bit sized
 * portions or vice versa. */
static const double truediffone = 26959535291011309493156476344723991336010898738574164086137773096960.0;
static const double bits192 = 6277101735386680763835789423207666416102355444464034512896.0;
static const double bits128 = 340282366920938463463374607431768211456.0;
static const double bits64 = 18446744073709551616.0;

/* Converts a little endian 256 bit value to a double */
static double le256todouble(const void *target)
{
	uint64_t *data64;
	double dcut64;

	data64 = (uint64_t *)(target + 24);
	dcut64 = le64toh(*data64) * bits192;

	data64 = (uint64_t *)(target + 16);
	dcut64 += le64toh(*data64) * bits128;

	data64 = (uint64_t *)(target + 8);
	dcut64 += le64toh(*data64) * bits64;

	data64 = (uint64_t *)(target);
	dcut64 += le64toh(*data64);

	return dcut64;
}

static double diff_from_target(void *target)
{
	double d64, dcut64;

	d64 = truediffone;
	dcut64 = le256todouble(target);
	if (unlikely(!dcut64))
		dcut64 = 1;
	return d64 / dcut64;
}

/*
 * Calculate the work->work_difficulty based on the work->target
 */
static void calc_diff(struct work *work, double known)
{
	if (known)
		work->work_difficulty = known;
	else
		work->work_difficulty = diff_from_target(work->target);
}



static void kill_timeout(struct thr_info *thr)
{
	cg_completion_timeout(&thr_info_cancel, thr, 1000);
}

static void kill_mining(void)
{
	struct thr_info *thr;
	int i;

	forcelog(LOG_DEBUG, "Killing off mining threads");
	/* Kill the mining threads*/
	for (i = 0; i < mining_threads; i++) {
		pthread_t *pth = NULL;

		thr = get_thread(i);
		if (thr && PTH(thr) != 0L)
			pth = &thr->pth;
		thr_info_cancel(thr);

		if (pth && *pth)
			pthread_join(*pth, NULL);
	}
}

static void __kill_work(void)
{
	struct thr_info *thr;
	int i;

	if (!successful_connect)
		return;

	forcelog(LOG_INFO, "Received kill message");

	/* Best to get rid of it first so it doesn't
	 * try to create any new devices */
	forcelog(LOG_DEBUG, "Killing off HotPlug thread");
	thr = &control_thr[hotplug_thr_id];
	kill_timeout(thr);

	forcelog(LOG_DEBUG, "Killing off watchpool thread");
	/* Kill the watchpool thread */
	thr = &control_thr[watchpool_thr_id];
	kill_timeout(thr);

	forcelog(LOG_DEBUG, "Killing off watchdog thread");
	/* Kill the watchdog thread */
	thr = &control_thr[watchdog_thr_id];
	kill_timeout(thr);

	forcelog(LOG_DEBUG, "Shutting down mining threads");
	for (i = 0; i < mining_threads; i++) {
		struct cgpu_info *cgpu;

		thr = get_thread(i);
		if (!thr)
			continue;
		cgpu = thr->cgpu;
		if (!cgpu)
			continue;

		cgpu->shutdown = true;
	}

	sleep(1);

	cg_completion_timeout(&kill_mining, NULL, 3000);
}

/* This should be the common exit path */
void kill_work(void)
{
	cg_completion_timeout(&__kill_work, NULL, 5000);

	quit(0, "Shutdown signal received.");
}

static char **initial_args;

static void clean_up(bool restarting);

void app_restart(void)
{
	applog(LOG_WARNING, "Attempting to restart %s", packagename);

	cg_completion_timeout(&__kill_work, NULL, 5000);
	clean_up(true);
	execv(initial_args[0], (EXECV_2ND_ARG_TYPE)initial_args);
	applog(LOG_WARNING, "Failed to restart application");
}

static void sighandler(int __maybe_unused sig)
{
	/* Restore signal handlers so we can still quit if kill_work fails */
	sigaction(SIGTERM, &termhandler, NULL);
	sigaction(SIGINT, &inthandler, NULL);
	kill_work();
}

static void _stage_work(struct work *work);

#define stage_work(WORK) do { \
	_stage_work(WORK); \
	WORK = NULL; \
} while (0)



struct work *make_clone(struct work *work)
{
	struct work *work_clone = copy_work(work);

	work_clone->clone = true;
	cgtime((struct timeval *)&(work_clone->tv_cloned));
	work_clone->longpoll = false;
	work_clone->mandatory = false;
	/* Make cloned work appear slightly older to bias towards keeping the
	 * master work item which can be further rolled */
	work_clone->tv_staged.tv_sec -= 1;

	return work_clone;
}



/* Return an adjusted ntime if we're submitting work that a device has
 * internally offset the ntime. */
static char *offset_ntime(const char *ntime, int noffset)
{
	unsigned char bin[4];
	uint32_t h32, *be32 = (uint32_t *)bin;

	hex2bin(bin, ntime, 4);
	h32 = be32toh(*be32) + noffset;
	*be32 = htobe32(h32);

	return bin2hex(bin, 4);
}

/* Duplicates any dynamically allocated arrays within the work struct to
 * prevent a copied work struct from freeing ram belonging to another struct */
static void _copy_work(struct work *work, const struct work *base_work, int noffset)
{
	uint32_t id = work->id;

	clean_work(work);
	memcpy(work, base_work, sizeof(struct work));
	/* Keep the unique new id assigned during make_work to prevent copied
	 * work from having the same id. */
	work->id = id;
	if (base_work->job_id)
		work->job_id = strdup(base_work->job_id);
	if (base_work->nonce1)
		work->nonce1 = strdup(base_work->nonce1);
	if (base_work->ntime) {
		/* If we are passed an noffset the binary work->data ntime and
		 * the work->ntime hex string need to be adjusted. */
		if (noffset) {
			uint32_t *work_ntime = (uint32_t *)(work->data + 68);
			uint32_t ntime = be32toh(*work_ntime);

			ntime += noffset;
			*work_ntime = htobe32(ntime);
			work->ntime = offset_ntime(base_work->ntime, noffset);
		} else
			work->ntime = strdup(base_work->ntime);
	} else if (noffset) {
		uint32_t *work_ntime = (uint32_t *)(work->data + 68);
		uint32_t ntime = be32toh(*work_ntime);

		ntime += noffset;
		*work_ntime = htobe32(ntime);
	}
	if (base_work->coinbase)
		work->coinbase = strdup(base_work->coinbase);
}

void set_work_ntime(struct work *work, int ntime)
{
	uint32_t *work_ntime = (uint32_t *)(work->data + 68);

	*work_ntime = htobe32(ntime);
	if (work->ntime) {
		free(work->ntime);
		work->ntime = bin2hex((unsigned char *)work_ntime, 4);
	}
}

/* Generates a copy of an existing work struct, creating fresh heap allocations
 * for all dynamically allocated arrays within the struct. noffset is used for
 * when a driver has internally rolled the ntime, noffset is a relative value.
 * The macro copy_work() calls this function with an noffset of 0. */
struct work *copy_work_noffset(struct work *base_work, int noffset)
{
	struct work *work = make_work();

	_copy_work(work, base_work, noffset);

	return work;
}


uint64_t share_diff(const struct work *work)
{
	bool new_best = false;
	double d64, s64;
	uint64_t ret;

	d64 = truediffone;
	s64 = le256todouble(work->hash);
	if (unlikely(!s64))
		s64 = 0;

	ret = round(d64 / s64);

	cg_wlock(&control_lock);
	if (unlikely(ret > best_diff)) {
		new_best = true;
		best_diff = ret;
		suffix_string(best_diff, best_share, sizeof(best_share), 0);
	}
	// if (unlikely(ret > work->pool->best_diff))
	//	work->pool->best_diff = ret;
	cg_wunlock(&control_lock);

	if (unlikely(new_best))
		applog(LOG_INFO, "New best share: %s", best_share);

	return ret;
}

uint64_t share_ndiff(const struct work *work)
{
	double d64, s64;
	uint64_t ret = 0;

	if(work != NULL) {
		d64 = truediffone;
		s64 = le256todouble(work->hash);
		if (unlikely(!s64)) {
			ret = 0;
		} else {
			ret = (d64 / s64);
		}
	}
	return ret;
}

void _discard_work(struct work *work)
{
	if (!work->clone && !work->rolls && !work->mined) {
		if (work->pool) {
			work->pool->discarded_work++;
			work->pool->quota_used--;
			work->pool->works--;
		}
		total_discarded++;
		applog(LOG_DEBUG, "Discarded work");
	} else
		applog(LOG_DEBUG, "Discarded cloned or rolled work");
	free_work(work);
}

/* A generic wait function for threads that poll that will wait a specified
 * time tdiff waiting on the pthread conditional that is broadcast when a
 * work restart is required. Returns the value of pthread_cond_timedwait
 * which is zero if the condition was met or ETIMEDOUT if not.
 */
int restart_wait(struct thr_info *thr, unsigned int mstime)
{
	struct timeval now, then, tdiff;
	struct timespec abstime;
	int rc;

	tdiff.tv_sec = mstime / 1000;
	tdiff.tv_usec = mstime * 1000 - (tdiff.tv_sec * 1000000);
	cgtime(&now);
	timeradd(&now, &tdiff, &then);
	abstime.tv_sec = then.tv_sec;
	abstime.tv_nsec = then.tv_usec * 1000;

	mutex_lock(&restart_lock);
	if (thr->work_restart)
		rc = 0;
	else
		rc = pthread_cond_timedwait(&restart_cond, &restart_lock, &abstime);
	mutex_unlock(&restart_lock);

	return rc;
}

static int tv_sort(struct work *worka, struct work *workb)
{
	return worka->tv_staged.tv_sec - workb->tv_staged.tv_sec;
}

static bool work_rollable(struct work *work)
{
	return (!work->clone && work->rolltime);
}

static bool hash_push(struct work *work)
{
	bool rc = true;

	mutex_lock(stgd_lock);
	staged_work_items++;
	if (likely(!getq->frozen)) {
		HASH_ADD_INT(staged_work, id, work);
		HASH_SORT(staged_work, tv_sort);
	} else
		rc = false;
	pthread_cond_broadcast(&getq->cond);
	mutex_unlock(stgd_lock);

	return rc;
}

static void _stage_work(struct work *work)
{
	applog(LOG_DEBUG, "Pushing work from pool [FOOBAR] to hash queue");
	work->work_block = work_block;
	hash_push(work);
}




void zero_bestshare(void)
{
	best_diff = 0;
	memset(best_share, 0, 8);
	suffix_string(best_diff, best_share, sizeof(best_share), 0);

}

static struct timeval tv_hashmeter;
static time_t hashdisplay_t;

void zero_stats(void)
{
	int i;

	cgtime(&total_tv_start);
	copy_time(&tv_hashmeter, &total_tv_start);
	total_rolling = 0;
	rolling1 = 0;
	rolling5 = 0;
	rolling15 = 0;
	total_mhashes_done = 0;

	for(i = 0; i < CG_LOCAL_MHASHES_MAX_NUM; i++) {
		g_local_mhashes_dones[i] = 0;
	}
	g_local_mhashes_index = 0;
	g_max_fan = 0;
	g_max_temp = 0;
	total_getworks = 0;
	total_accepted = 0;
	total_rejected = 0;
	hw_errors = 0;
	total_stale = 0;
	total_discarded = 0;
	local_work = 0;
	total_go = 0;
	total_ro = 0;
	total_secs = 1.0;
	last_total_secs = 1.0;
	total_diff1 = 0;
	found_blocks = 0;
	total_diff_accepted = 0;
	total_diff_rejected = 0;
	total_diff_stale = 0;


	zero_bestshare();

	for (i = 0; i < total_devices; ++i) {
		struct cgpu_info *cgpu = get_devices(i);

		copy_time(&cgpu->dev_start_tv, &total_tv_start);

		mutex_lock(&hash_lock);
		cgpu->total_mhashes = 0;
		cgpu->accepted = 0;
		cgpu->rejected = 0;
		cgpu->hw_errors = 0;
		cgpu->utility = 0.0;
		cgpu->last_share_pool_time = 0;
		cgpu->diff1 = 0;
		cgpu->diff_accepted = 0;
		cgpu->diff_rejected = 0;
		cgpu->last_share_diff = 0;
		mutex_unlock(&hash_lock);

		/* Don't take any locks in the driver zero stats function, as
		 * it's called async from everything else and we don't want to
		 * deadlock. */
		cgpu->drv->zero_stats(cgpu);
	}
}

static void set_highprio(void)
{

	int ret = nice(-10);

	if (!ret)
		applog(LOG_DEBUG, "Unable to set thread to high priority");
}

static void set_lowprio(void)
{
	int ret = nice(10);

	if (!ret)
		applog(LOG_INFO, "Unable to set thread to low priority");
}


static void thread_reportin(struct thr_info *thr)
{
	thr->getwork = false;
	cgtime(&thr->last);
	thr->cgpu->status = LIFE_WELL;
	thr->cgpu->device_last_well = time(NULL);
}

/* Tell the watchdog thread this thread is waiting on get work and should not
 * be restarted */
static void thread_reportout(struct thr_info *thr)
{
	thr->getwork = true;
	cgtime(&thr->last);
	thr->cgpu->status = LIFE_WELL;
	thr->cgpu->device_last_well = time(NULL);
}

static void hashmeter(int thr_id, uint64_t hashes_done)
{
	bool showlog = false;
	double tv_tdiff;
	time_t now_t;
	int diff_t;

	uint64_t local_mhashes_done = 0;
	uint64_t local_mhashes_done_avg = 0;
	int local_mhashes_done_count = 0;
	int i = 0;

	cgtime(&total_tv_end);
	tv_tdiff = tdiff(&total_tv_end, &tv_hashmeter);
	now_t = total_tv_end.tv_sec;
	diff_t = now_t - hashdisplay_t;
	if (diff_t >= opt_log_interval) {
		alt_status ^= switch_status;
		hashdisplay_t = now_t;
		showlog = true;
	} else if (thr_id < 0) {
		/* hashmeter is called by non-mining threads in case nothing
		 * has reported in to allow hashrate to converge to zero , but
		 * we only update if it has been more than opt_log_interval */
		return;
	}
	copy_time(&tv_hashmeter, &total_tv_end);

	if (thr_id >= 0) {
		struct thr_info *thr = get_thread(thr_id);
		struct cgpu_info *cgpu = thr->cgpu;
		double device_tdiff, thr_mhs;

		/* Update the last time this thread reported in */
		copy_time(&thr->last, &total_tv_end);
		cgpu->device_last_well = now_t;
		device_tdiff = tdiff(&total_tv_end, &cgpu->last_message_tv);
		copy_time(&cgpu->last_message_tv, &total_tv_end);
		thr_mhs = (double)hashes_done / device_tdiff / 1000000;
		applog(LOG_DEBUG, "[thread %d: %"PRIu64" hashes, %.1f mhash/sec]",
		       thr_id, hashes_done, thr_mhs);
		hashes_done /= 1000000;

		mutex_lock(&hash_lock);
		cgpu->total_mhashes += hashes_done;
		decay_time(&cgpu->rolling, hashes_done, device_tdiff, opt_log_interval);
		decay_time(&cgpu->rolling1, hashes_done, device_tdiff, 60.0);
		decay_time(&cgpu->rolling5, hashes_done, device_tdiff, 300.0);
		decay_time(&cgpu->rolling15, hashes_done, device_tdiff, 900.0);
		mutex_unlock(&hash_lock);

		if (want_per_device_stats && showlog) {
			char logline[256];

			get_statline(logline, sizeof(logline), cgpu);
			if (!curses_active) {
				printf("%s          \r", logline);
				fflush(stdout);
			} else
				applog(LOG_INFO, "%s", logline);
		}
	} else {
		/* No device has reported in, we have been called from the
		 * watchdog thread so decay all the hashrates */
		mutex_lock(&hash_lock);
		for (thr_id = 0; thr_id < mining_threads; thr_id++) {
			struct thr_info *thr = get_thread(thr_id);
			struct cgpu_info *cgpu = thr->cgpu;
			double device_tdiff  = tdiff(&total_tv_end, &cgpu->last_message_tv);

			copy_time(&cgpu->last_message_tv, &total_tv_end);
			decay_time(&cgpu->rolling, 0, device_tdiff, opt_log_interval);
			decay_time(&cgpu->rolling1, 0, device_tdiff, 60.0);
			decay_time(&cgpu->rolling5, 0, device_tdiff, 300.0);
			decay_time(&cgpu->rolling15, 0, device_tdiff, 900.0);
		}
		mutex_unlock(&hash_lock);
	}

	mutex_lock(&hash_lock);
	total_mhashes_done += hashes_done;
	if(showlog){		
		g_local_mhashes_index++;
		if(g_local_mhashes_index >= CG_LOCAL_MHASHES_MAX_NUM)
			g_local_mhashes_index = 0;

		for(i = 0; i < CG_LOCAL_MHASHES_MAX_NUM; i++) {
			if(g_local_mhashes_dones[i] >= 0) {
				local_mhashes_done_avg += g_local_mhashes_dones[i];
					//applog(LOG_DEBUG, "g_local_mhashes_dones[%d] = %f,%d", i, g_local_mhashes_dones[i],g_local_mhashes_index);
				local_mhashes_done_count++;
			}
		}
			
			if(local_mhashes_done_count > 0) {
			local_mhashes_done = local_mhashes_done_avg / local_mhashes_done_count;
		} else {
			local_mhashes_done = hashes_done;
		}
			
		decay_time(&total_rolling, local_mhashes_done, opt_log_interval, opt_log_interval);
		decay_time(&rolling1, hashes_done, tv_tdiff, 60.0);
		decay_time(&rolling5, hashes_done,tv_tdiff, 300.0);
		decay_time(&rolling15, hashes_done, tv_tdiff, 900.0);
		global_hashrate = llround(total_rolling) * 1000000;
		g_local_mhashes_dones[g_local_mhashes_index] = 0;
	}
		g_local_mhashes_dones[g_local_mhashes_index] += hashes_done;
	total_secs = tdiff(&total_tv_end, &total_tv_start);

	if (total_secs - last_total_secs > 86400) {
		applog(LOG_ERR, "cgminer time error total_secs = %.0f last_total_secs = %.0f", total_secs, last_total_secs);
		mutex_unlock(&hash_lock);
		zero_stats();
		mutex_lock(&hash_lock);
	} else {
		last_total_secs = total_secs;
	}
	if (showlog) {
		char displayed_hashes[16], displayed_rolling[16];
		char displayed_r1[16], displayed_r5[16], displayed_r15[16];
		uint64_t d64;

		d64 = (double)total_mhashes_done / total_secs * 1000000ull;
		suffix_string(d64, displayed_hashes, sizeof(displayed_hashes), 4);
		d64 = (double)total_rolling * 1000000ull;
		g_displayed_rolling = total_rolling / 1000.0;
		suffix_string(d64, displayed_rolling, sizeof(displayed_rolling), 4);
		d64 = (double)rolling1 * 1000000ull;
		suffix_string(d64, displayed_r1, sizeof(displayed_rolling), 4);
		d64 = (double)rolling5 * 1000000ull;
		suffix_string(d64, displayed_r5, sizeof(displayed_rolling), 4);
		d64 = (double)rolling15 * 1000000ull;
		suffix_string(d64, displayed_r15, sizeof(displayed_rolling), 4);

		snprintf(statusline, sizeof(statusline),
			"(%ds):%s (1m):%s (5m):%s (15m):%s (avg):%sh/s, %d work items staged, %d nonces found",
			opt_log_interval, displayed_rolling, displayed_r1, displayed_r5,
			displayed_r15, displayed_hashes, staged_work_items, nonce_reported);
	}
	mutex_unlock(&hash_lock);

	if (showlog) {
		if (!curses_active) {
			printf("%s          \r", statusline);
			fflush(stdout);
		} else
			applog(LOG_INFO, "%s", statusline);
	}
}


static bool work_filled;
static bool work_emptied;

/* If this is called non_blocking, it will return NULL for work so that must
 * be handled. */
static struct work *hash_pop(bool blocking)
{
	struct work *work = NULL, *tmp;
	int hc;

	mutex_lock(stgd_lock);
	if (!HASH_COUNT(staged_work)) {
		/* Increase the queue if we reach zero and we know we can reach
		 * the maximum we're asking for. */
		if (work_filled && max_queue < opt_queue) {
			max_queue++;
			work_filled = false;
		}
		work_emptied = true;
		if (!blocking)
			goto out_unlock;
		do {
			struct timespec then;
			struct timeval now;
			int rc;

			cgtime(&now);
			then.tv_sec = now.tv_sec + 10;
			then.tv_nsec = now.tv_usec * 1000;
			pthread_cond_signal(&gws_cond);
			rc = pthread_cond_timedwait(&getq->cond, stgd_lock, &then);
			/* Check again for !no_work as multiple threads may be
				* waiting on this condition and another may set the
				* bool separately. */
			if (rc && !no_work) {
				no_work = true;
				applog(LOG_WARNING, "Waiting for work to be available from pools.");
			}
		} while (!HASH_COUNT(staged_work));
	}

	if (no_work) {
		applog(LOG_WARNING, "Work available from pools, resuming.");
		no_work = false;
	}

	hc = HASH_COUNT(staged_work);
	/* Find clone work if possible, to allow masters to be reused */
	if (hc > staged_rollable) {
		HASH_ITER(hh, staged_work, work, tmp) {
			if (!work_rollable(work))
				break;
		}
	} else
		work = staged_work;
	HASH_DEL(staged_work, work);
	
	/* Signal the getwork scheduler to look for more work */
	pthread_cond_signal(&gws_cond);

	/* Signal hash_pop again in case there are mutliple hash_pop waiters */
	pthread_cond_signal(&getq->cond);

	/* Keep track of last getwork grabbed */
	last_getwork = time(NULL);
out_unlock:
	mutex_unlock(stgd_lock);

	return work;
}

void set_target(unsigned char *dest_target, double diff)
{
	unsigned char target[32];
	uint64_t *data64, h64;
	double d64, dcut64;

	if (unlikely(diff == 0.0)) {
		/* This shouldn't happen but best we check to prevent a crash */
		applog(LOG_ERR, "Diff zero passed to set_target");
		diff = 1.0;
	}

	d64 = truediffone;
	d64 /= diff;

	dcut64 = d64 / bits192;
	h64 = dcut64;
	data64 = (uint64_t *)(target + 24);
	*data64 = htole64(h64);
	dcut64 = h64;
	dcut64 *= bits192;
	d64 -= dcut64;

	dcut64 = d64 / bits128;
	h64 = dcut64;
	data64 = (uint64_t *)(target + 16);
	*data64 = htole64(h64);
	dcut64 = h64;
	dcut64 *= bits128;
	d64 -= dcut64;

	dcut64 = d64 / bits64;
	h64 = dcut64;
	data64 = (uint64_t *)(target + 8);
	*data64 = htole64(h64);
	dcut64 = h64;
	dcut64 *= bits64;
	d64 -= dcut64;

	h64 = d64;
	data64 = (uint64_t *)(target);
	*data64 = htole64(h64);

	if (opt_debug) {
		char *htarget = bin2hex(target, 32);

		applog(LOG_DEBUG, "Generated target %s", htarget);
		free(htarget);
	}
	memcpy(dest_target, target, 32);
}




static const char *PREFIXES[3] = {
		     "FOO-0x0000000000000000                                                          ",
		     "BAR-0x0000000000000000                                                          ",
		     "FOOBAR-0x0000000000000000                                                       "
		};

static const char NIBBLE[16] = {48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 65, 66, 67, 68, 69, 70};


static void gen_foobar_work(int kind, int64_t *_counter, struct work *work)
{
	int64_t counter = *_counter;
	work->foobar_counter = counter;
	*_counter = counter + 1;

	char buffer[80];
	memcpy(buffer, PREFIXES[kind], 80);

	int j = (kind == 2) ? 25 : 22;
	while (counter > 0) {
		int nibble = counter & 0x000f;
		counter >>= 4;
		buffer[j] = NIBBLE[nibble];
		j--;
	}
	flip80(work->data, buffer);

	if (opt_debug) {
		char *block;

		block = bin2hex(work->data, 80);
		applog(LOG_DEBUG, "Generated block %s", block);
		free(block);
	}

	calc_midstate(work);
	work->sdiff = 2; 
	local_work++;

	work->nonce = 0;
	calc_diff(work, work->sdiff);
	cgtime(&work->tv_staged);
}






/* The time difference in seconds between when this device last got work via
 * get_work() and generated a valid share. */
int share_work_tdiff(struct cgpu_info *cgpu)
{
	return last_getwork - cgpu->last_device_valid_work;
}

struct work *get_work(struct thr_info *thr, const int thr_id)
{
	struct cgpu_info *cgpu = thr->cgpu;
	struct work *work = NULL;
	time_t diff_t;

	thread_reportout(thr);
	applog(LOG_DEBUG, "Popping work from get queue to get work");
	diff_t = time(NULL);
	while (!work) {
		work = hash_pop(true);
	}
	diff_t = time(NULL) - diff_t;
	/* Since this is a blocking function, we need to add grace time to
	 * the device's last valid work to not make outages appear to be
	 * device failures. */
	if (diff_t > 0) {
		applog(LOG_DEBUG, "Get work blocked for %d seconds", (int)diff_t);
		cgpu->last_device_valid_work += diff_t;
	}
	applog(LOG_DEBUG, "Got work from get queue to get work for thread %d", thr_id);

	work->thr_id = thr_id;
	
	thread_reportin(thr);
	work->mined = true;
	work->device_diff = MIN(cgpu->drv->max_diff, work->work_difficulty);
	work->device_diff = MAX(cgpu->drv->min_diff, work->device_diff);
	return work;
}


void inc_hw_errors(struct thr_info *thr)
{
	applog(LOG_ERR, "%s%d: invalid nonce - HW error", thr->cgpu->drv->name,
	       thr->cgpu->device_id);

	mutex_lock(&stats_lock);
	hw_errors++;
	thr->cgpu->hw_errors++;
	mutex_unlock(&stats_lock);

	thr->cgpu->drv->hw_error(thr);
}

void inc_dev_status(int max_fan, int max_temp)
{
	mutex_lock(&stats_lock);
	g_max_fan = max_fan;
	g_max_temp = max_temp;
	mutex_unlock(&stats_lock);
}



void inc_work_stats(struct thr_info *thr, struct pool *pool, int diff1)
{
	(void) pool;
	mutex_lock(&stats_lock);
	total_diff1 += diff1;
	thr->cgpu->diff1 += diff1;
	thr->cgpu->last_device_valid_work = time(NULL);
	mutex_unlock(&stats_lock);
}


bool submit_nonce_direct(struct thr_info *thr, struct work *work, uint32_t nonce)
{
	struct nonce_msg_t msg;
	msg.counter = work->foobar_counter;
	msg.nonce = htole32(nonce);
	
	// printf("found counter = %" PRIx64 " nonce = %" PRIx32 "\n", msg.counter, msg.nonce);

	if (opt_debug) {
		uint32_t *work_nonce = (uint32_t *)(work->data + 64 + 12);
		*work_nonce = htole32(nonce);

		uint32_t space[20];
		flip80(space, work->data);
		char *block = bin2hex((const unsigned char *) space, 80);
		applog(LOG_DEBUG, "Solved block %s", block);
		free(block);
	}

	assert(sizeof(msg) == 12);

	struct cgpu_info *bitmain = thr->cgpu;
	/* Create the nanomsg socket if not alreday there. It must always be created in
	   the thread that uses it. */
	if (bitmain->nn_socket < 0) {
		bitmain->nn_socket = nn_socket(AF_SP, NN_PUSH);
		if (bitmain->nn_socket < 0) 
                	nn_fatal("nn_socket (PUSH)");
        
        	if (nn_connect(bitmain->nn_socket, nn_push_address) < 0)
                	nn_fatal("nn_connect (PUSH)");
               	applog(LOG_WARNING, "BITMAIN : created socket and connecting to %s", nn_push_address);
        }

	if (nn_send(bitmain->nn_socket, &msg, sizeof(msg), NN_DONTWAIT) < 0) {
		if (nn_errno() == EAGAIN) {
			applog(LOG_WARNING, "Cannot PUSH (blocked). Trying to re-connect");
			// if (0 != zmq_close(bitmain->zmq_socket))
			// 	errx(1, "zmq_close: %s", zmq_strerror(errno));
			// bitmain->zmq_socket = NULL;
		} else {
			nn_fatal("nn_send (PUSH)");
		}
	}
	nonce_reported++;
	return true;
}


static inline bool abandon_work(struct work *work, struct timeval *wdiff, uint64_t hashes)
{
	(void) work;
	if (wdiff->tv_sec > opt_scantime || hashes >= 0xfffffffe)
		return true;
	return false;
}

static void mt_disable(struct thr_info *mythr, const int thr_id,
		       struct device_drv *drv)
{
	applog(LOG_WARNING, "Thread %d being disabled", thr_id);
	mythr->cgpu->rolling = 0;
	applog(LOG_DEBUG, "Waiting on sem in miner thread");
	cgsem_wait(&mythr->sem);
	applog(LOG_WARNING, "Thread %d being re-enabled", thr_id);
	drv->thread_enable(mythr);
}

/* Put a new unqueued work item in cgpu->unqueued_work under cgpu->qlock till
 * the driver tells us it's full so that it may extract the work item using
 * the get_queued() function which adds it to the hashtable on
 * cgpu->queued_work. */
static void fill_queue(struct thr_info *mythr, struct cgpu_info *cgpu, struct device_drv *drv, const int thr_id)
{
	do {
		bool need_work;

		/* Do this lockless just to know if we need more unqueued work. */
		need_work = (!cgpu->unqueued_work);

		/* get_work is a blocking function so do it outside of lock
		 * to prevent deadlocks with other locks. */
		if (need_work) {
			struct work *work = get_work(mythr, thr_id);

			wr_lock(&cgpu->qlock);
			/* Check we haven't grabbed work somehow between
			 * checking and picking up the lock. */
			if (likely(!cgpu->unqueued_work))
				cgpu->unqueued_work = work;
			else
				need_work = false;
			wr_unlock(&cgpu->qlock);

			if (unlikely(!need_work))
				discard_work(work);
		}
		/* The queue_full function should be used by the driver to
		 * actually place work items on the physical device if it
		 * does have a queue. */
	} while (!drv->queue_full(cgpu));
}

/* Add a work item to a cgpu's queued hashlist */
void __add_queued(struct cgpu_info *cgpu, struct work *work)
{
	cgpu->queued_count++;
	HASH_ADD_INT(cgpu->queued_work, id, work);
}

struct work *__get_queued(struct cgpu_info *cgpu)
{
	struct work *work = NULL;

	if (cgpu->unqueued_work) {
		work = cgpu->unqueued_work;
		__add_queued(cgpu, work);
		cgpu->unqueued_work = NULL;
	}

	return work;
}

/* This function is for retrieving one work item from the unqueued pointer and
 * adding it to the hashtable of queued work. Code using this function must be
 * able to handle NULL as a return which implies there is no work available. */
struct work *get_queued(struct cgpu_info *cgpu)
{
	struct work *work;

	wr_lock(&cgpu->qlock);
	work = __get_queued(cgpu);
	wr_unlock(&cgpu->qlock);

	return work;
}

void add_queued(struct cgpu_info *cgpu, struct work *work)
{
	wr_lock(&cgpu->qlock);
	__add_queued(cgpu, work);
	wr_unlock(&cgpu->qlock);
}

/* Get fresh work and add it to cgpu's queued hashlist */
struct work *get_queue_work(struct thr_info *thr, struct cgpu_info *cgpu, int thr_id)
{
	struct work *work = get_work(thr, thr_id);

	add_queued(cgpu, work);
	return work;
}

/* This function is for finding an already queued work item in the
 * given que hashtable. Code using this function must be able
 * to handle NULL as a return which implies there is no matching work.
 * The calling function must lock access to the que if it is required. */
struct work *__find_work_byid(struct work *queue, uint32_t id)
{
	struct work *ret = NULL;
	HASH_FIND_INT(queue, &id, ret);
	return ret;
}

struct work *find_queued_work_byid(struct cgpu_info *cgpu, uint32_t id)
{
	struct work *ret;

	rd_lock(&cgpu->qlock);
	ret = __find_work_byid(cgpu->queued_work, id);
	rd_unlock(&cgpu->qlock);

	return ret;
}

struct work *clone_queued_work_byid(struct cgpu_info *cgpu, uint32_t id)
{
	struct work *work, *ret = NULL;

	rd_lock(&cgpu->qlock);
	work = __find_work_byid(cgpu->queued_work, id);
	if (work)
		ret = copy_work(work);
	rd_unlock(&cgpu->qlock);

	return ret;
}

void __work_completed(struct cgpu_info *cgpu, struct work *work)
{
	cgpu->queued_count--;
	HASH_DEL(cgpu->queued_work, work);
}


/* This function should be used by queued device drivers when they're sure
 * the work struct is no longer in use. */
void work_completed(struct cgpu_info *cgpu, struct work *work)
{
	wr_lock(&cgpu->qlock);
	__work_completed(cgpu, work);
	wr_unlock(&cgpu->qlock);

	free_work(work);
}


/* This version of hash work is for devices that are fast enough to always
 * perform a full nonce range and need a queue to maintain the device busy.
 * Work creation and destruction is not done from within this function
 * directly. */
void hash_queued_work(struct thr_info *mythr)
{
	// charles : here is where the actual fun takes place.

	struct timeval tv_start = {0, 0}, tv_end;
	struct cgpu_info *cgpu = mythr->cgpu;
	struct device_drv *drv = cgpu->drv;
	const int thr_id = mythr->id;
	int64_t hashes_done = 0;

	while (likely(!cgpu->shutdown)) {
		struct timeval diff;
		int64_t hashes;

		mythr->work_update = false;

		fill_queue(mythr, cgpu, drv, thr_id);

		hashes = drv->scanwork(mythr);

		/* Reset the bool here in case the driver looks for it
		 * synchronously in the scanwork loop. */
		mythr->work_restart = false;

		if (unlikely(hashes == -1 )) {
			applog(LOG_ERR, "%s %d failure, disabling!", drv->name, cgpu->device_id);
			cgpu->deven = DEV_DISABLED;
			dev_error(cgpu, REASON_THREAD_ZERO_HASH);
			break;
		}

		hashes_done += hashes;
		cgtime(&tv_end);
		timersub(&tv_end, &tv_start, &diff);
		/* Update the hashmeter at most 5 times per second */
		if ((hashes_done && (diff.tv_sec > 0 || diff.tv_usec > 200000)) ||
		    diff.tv_sec >= opt_log_interval) {
			hashmeter(thr_id, hashes_done);
			hashes_done = 0;
			copy_time(&tv_start, &tv_end);
		}

		if (unlikely(mythr->pause || cgpu->deven != DEV_ENABLED))
			mt_disable(mythr, thr_id, drv);

		if (mythr->work_update)
			drv->update_work(cgpu);
	}
	cgpu->deven = DEV_DISABLED;
}


void *miner_thread(void *userdata)
{
	struct thr_info *mythr = userdata;
	const int thr_id = mythr->id;
	struct cgpu_info *cgpu = mythr->cgpu;
	struct device_drv *drv = cgpu->drv;
	char threadname[16];

	snprintf(threadname, sizeof(threadname), "%d/Miner", thr_id);
	RenameThread(threadname);

	thread_reportout(mythr);
	if (!drv->thread_init(mythr)) {
		dev_error(cgpu, REASON_THREAD_FAIL_INIT);
		goto out;
	}

	applog(LOG_DEBUG, "Waiting on sem in miner thread");
	cgsem_wait(&mythr->sem);

	cgpu->last_device_valid_work = time(NULL);
	drv->hash_work(mythr);
	drv->thread_shutdown(mythr);
out:
	return NULL;
}

enum {
	STAT_SLEEP_INTERVAL		= 1,
	STAT_CTR_INTERVAL		= 10000000,
	FAILURE_INTERVAL		= 30,
};



void reinit_device(struct cgpu_info *cgpu)
{
	if (cgpu->deven == DEV_DISABLED)
		return;
	cgpu->drv->reinit_device(cgpu);
}

static struct timeval rotate_tv;


/* Makes sure the hashmeter keeps going even if mining threads stall, updates
 * the screen at regular intervals, and restarts threads if they appear to have
 * died. */
#define WATCHDOG_INTERVAL		2
#define WATCHDOG_SICK_TIME		120
#define WATCHDOG_DEAD_TIME		600
#define WATCHDOG_SICK_COUNT		(WATCHDOG_SICK_TIME/WATCHDOG_INTERVAL)
#define WATCHDOG_DEAD_COUNT		(WATCHDOG_DEAD_TIME/WATCHDOG_INTERVAL)

static void *watchdog_thread(void __maybe_unused *userdata)
{
	const unsigned int interval = WATCHDOG_INTERVAL;
	struct timeval zero_tv;

	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

	RenameThread("Watchdog");

	set_lowprio();
	memset(&zero_tv, 0, sizeof(struct timeval));
	cgtime(&rotate_tv);

	while (1) {
		int i;
		struct timeval now;

		sleep(interval);

		// discard_stale();

		hashmeter(-1, 0);
		cgtime(&now);


		for (i = 0; i < total_devices; ++i) {
			struct cgpu_info *cgpu = get_devices(i);
			struct thr_info *thr = cgpu->thr[0];
			enum dev_enable *denable;
			char dev_str[8];
			int gpu;

			if (!thr)
				continue;

			cgpu->drv->get_stats(cgpu);

			gpu = cgpu->device_id;
			denable = &cgpu->deven;
			snprintf(dev_str, sizeof(dev_str), "%s%d", cgpu->drv->name, gpu);

			/* Thread is waiting on getwork or disabled */
			if (thr->getwork || *denable == DEV_DISABLED)
				continue;

			if (cgpu->status != LIFE_WELL && (now.tv_sec - thr->last.tv_sec < WATCHDOG_SICK_TIME)) {
				if (cgpu->status != LIFE_INIT)
				applog(LOG_ERR, "%s: Recovered, declaring WELL!", dev_str);
				cgpu->status = LIFE_WELL;
				cgpu->device_last_well = time(NULL);
			} else if (cgpu->status == LIFE_WELL && (now.tv_sec - thr->last.tv_sec > WATCHDOG_SICK_TIME)) {
				cgpu->rolling = 0;
				cgpu->status = LIFE_SICK;
				applog(LOG_ERR, "%s: Idle for more than 60 seconds, declaring SICK!", dev_str);
				cgtime(&thr->sick);

				dev_error(cgpu, REASON_DEV_SICK_IDLE_60);
				if (opt_restart) {
					applog(LOG_ERR, "%s: Attempting to restart", dev_str);
					reinit_device(cgpu);
				}
			} else if (cgpu->status == LIFE_SICK && (now.tv_sec - thr->last.tv_sec > WATCHDOG_DEAD_TIME)) {
				cgpu->status = LIFE_DEAD;
				applog(LOG_ERR, "%s: Not responded for more than 10 minutes, declaring DEAD!", dev_str);
				cgtime(&thr->sick);

				dev_error(cgpu, REASON_DEV_DEAD_IDLE_600);
			} else if (now.tv_sec - thr->sick.tv_sec > 60 &&
				   (cgpu->status == LIFE_SICK || cgpu->status == LIFE_DEAD)) {
				/* Attempt to restart a GPU that's sick or dead once every minute */
				cgtime(&thr->sick);
				if (opt_restart)
					reinit_device(cgpu);
			}
		}
	}

	return NULL;
}


static void noop_get_statline(char __maybe_unused *buf, size_t __maybe_unused bufsiz, struct cgpu_info __maybe_unused *cgpu);
void blank_get_statline_before(char *buf, size_t bufsiz, struct cgpu_info __maybe_unused *cgpu);


static void clean_up(bool restarting)
{
	(void) restarting;
	cgtime(&total_tv_end);
	curl_global_cleanup();
}

/* Should all else fail and we're unable to clean up threads due to locking
 * issues etc, just silently exit. */
static void *killall_thread(void __maybe_unused *arg)
{
	pthread_detach(pthread_self());
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
	sleep(5);
	exit(1);
	return NULL;
}

void __quit(int status, bool clean)
{
	pthread_t killall_t;

	if (unlikely(pthread_create(&killall_t, NULL, killall_thread, NULL)))
		exit(1);

	if (clean)
		clean_up(false);

	if (forkpid > 0) {
		kill(forkpid, SIGTERM);
		forkpid = 0;
	}
	pthread_cancel(killall_t);
	exit(status);
}

void _quit(int status)
{
	__quit(status, true);
}




static int cgminer_id_count = 0;

/* Various noop functions for drivers that don't support or need their
 * variants. */
static void noop_reinit_device(struct cgpu_info __maybe_unused *cgpu)
{
}

void blank_get_statline_before(char __maybe_unused *buf,size_t __maybe_unused bufsiz, struct cgpu_info __maybe_unused *cgpu)
{
}

static void noop_get_statline(char __maybe_unused *buf, size_t __maybe_unused bufsiz, struct cgpu_info __maybe_unused *cgpu)
{
}

static bool noop_get_stats(struct cgpu_info __maybe_unused *cgpu)
{
	return true;
}

static bool noop_thread_prepare(struct thr_info __maybe_unused *thr)
{
	return true;
}

static uint64_t noop_can_limit_work(struct thr_info __maybe_unused *thr)
{
	return 0xffffffff;
}

static bool noop_thread_init(struct thr_info __maybe_unused *thr)
{
	return true;
}

static bool noop_prepare_work(struct thr_info __maybe_unused *thr, struct work __maybe_unused *work)
{
	return true;
}

static void noop_hw_error(struct thr_info __maybe_unused *thr)
{
}

static void noop_thread_shutdown(struct thr_info __maybe_unused *thr)
{
}

static void noop_thread_enable(struct thr_info __maybe_unused *thr)
{
}

static void noop_detect(bool __maybe_unused hotplug)
{
}

static struct api_data *noop_get_api_stats(struct cgpu_info __maybe_unused *cgpu)
{
	return NULL;
}

static void noop_hash_work(struct thr_info __maybe_unused *thr)
{
}

#define noop_flush_work noop_reinit_device
#define noop_update_work noop_reinit_device
#define noop_queue_full noop_get_stats
#define noop_zero_stats noop_reinit_device
#define noop_identify_device noop_reinit_device

/* Fill missing driver drv functions with noops */
void fill_device_drv(struct device_drv *drv)
{
	//if (!drv->drv_detect)
//		drv->drv_detect = &noop_detect;
//	if (!drv->reinit_device)
//		drv->reinit_device = &noop_reinit_device;
//	if (!drv->get_statline_before)
//		drv->get_statline_before = &blank_get_statline_before;
//	if (!drv->get_statline)
//		drv->get_statline = &noop_get_statline;
	if (!drv->get_stats)
		drv->get_stats = &noop_get_stats;
//	if (!drv->thread_prepare)
//		drv->thread_prepare = &noop_thread_prepare;
	if (!drv->can_limit_work)
		drv->can_limit_work = &noop_can_limit_work;
	if (!drv->thread_init)
		drv->thread_init = &noop_thread_init;
	if (!drv->prepare_work)
		drv->prepare_work = &noop_prepare_work;
	if (!drv->hw_error)
		drv->hw_error = &noop_hw_error;
	//if (!drv->thread_shutdown)
	//	drv->thread_shutdown = &noop_thread_shutdown;
	if (!drv->thread_enable)
		drv->thread_enable = &noop_thread_enable;
	//if (!drv->hash_work)
	//	drv->hash_work = &hash_sole_work;
	//if (!drv->flush_work)
	//	drv->flush_work = &noop_flush_work;
	if (!drv->update_work)
		drv->update_work = &noop_update_work;
	//if (!drv->queue_full)
	//	drv->queue_full = &noop_queue_full;
	if (!drv->zero_stats)
		drv->zero_stats = &noop_zero_stats;
	/* If drivers support internal diff they should set a max_diff or
	 * we will assume they don't and set max to 1. */
	if (!drv->max_diff)
		drv->max_diff = 1;
}

void null_device_drv(struct device_drv *drv)
{
	drv->drv_detect = &noop_detect;
	drv->reinit_device = &noop_reinit_device;
	drv->get_statline_before = &blank_get_statline_before;
	drv->get_statline = &noop_get_statline;
	drv->get_api_stats = &noop_get_api_stats;
	drv->get_stats = &noop_get_stats;
	drv->identify_device = &noop_identify_device;
	drv->set_device = NULL;

	drv->thread_prepare = &noop_thread_prepare;
	drv->can_limit_work = &noop_can_limit_work;
	drv->thread_init = &noop_thread_init;
	drv->prepare_work = &noop_prepare_work;

	/* This should make the miner thread just exit */
	drv->hash_work = &noop_hash_work;

	drv->hw_error = &noop_hw_error;
	drv->thread_shutdown = &noop_thread_shutdown;
	drv->thread_enable = &noop_thread_enable;

	drv->zero_stats = &noop_zero_stats;

	drv->hash_work = &noop_hash_work;

	drv->queue_full = &noop_queue_full;
	drv->flush_work = &noop_flush_work;
	drv->update_work = &noop_update_work;

	drv->zero_stats = &noop_zero_stats;
	drv->max_diff = 1;
	drv->min_diff = 1;
}

void enable_device(struct cgpu_info *cgpu)
{
	cgpu->deven = DEV_ENABLED;

	wr_lock(&devices_lock);
	devices[cgpu->cgminer_id = cgminer_id_count++] = cgpu;
	wr_unlock(&devices_lock);

	if (hotplug_mode)
		new_threads += cgpu->threads;
	else
		mining_threads += cgpu->threads;

	rwlock_init(&cgpu->qlock);
	cgpu->queued_work = NULL;
}

struct _cgpu_devid_counter {
	char name[4];
	int lastid;
	UT_hash_handle hh;
};

static void adjust_mostdevs(void)
{
	if (total_devices - zombie_devs > most_devices)
		most_devices = total_devices - zombie_devs;
}


bool add_cgpu(struct cgpu_info *cgpu)
{
	static struct _cgpu_devid_counter *devids = NULL;
	struct _cgpu_devid_counter *d;
	
	HASH_FIND_STR(devids, cgpu->drv->name, d);
	if (d)
		cgpu->device_id = ++d->lastid;
	else {
		d = malloc(sizeof(*d));
		memcpy(d->name, cgpu->drv->name, sizeof(d->name));
		cgpu->device_id = d->lastid = 0;
		HASH_ADD_STR(devids, name, d);
	}

	wr_lock(&devices_lock);
	devices = realloc(devices, sizeof(struct cgpu_info *) * (total_devices + new_devices + 2));
	wr_unlock(&devices_lock);

	mutex_lock(&stats_lock);
	cgpu->last_device_valid_work = time(NULL);
	mutex_unlock(&stats_lock);

	if (hotplug_mode)
		devices[total_devices + new_devices++] = cgpu;
	else
		devices[total_devices++] = cgpu;

	adjust_mostdevs();
	return true;
}

struct device_drv *copy_drv(struct device_drv *drv)
{
	struct device_drv *copy;

	if (unlikely(!(copy = malloc(sizeof(*copy))))) {
		quit(1, "Failed to allocate device_drv copy of %s (%s)",
				drv->name, drv->copy ? "copy" : "original");
	}
	memcpy(copy, drv, sizeof(*copy));
	copy->copy = true;
	return copy;
}



int main(int argc, char *argv[])
{
	struct sigaction handler;
	struct work *work = NULL;
	struct thr_info *thr;
	struct block *block;
	int i;
	char *s;

	assert(sizeof(struct nonce_msg_t) == 12);

	g_logfile_enable = false;
	strcpy(g_logfile_path, "cgminer.log");
	strcpy(g_logfile_openflag, "a+");
	

	/* If we're on a small lowspec platform with only one CPU, we should
	 * yield after dropping a lock to allow a thread waiting for it to be
	 * able to get CPU time to grab the lock. */
	if (sysconf(_SC_NPROCESSORS_ONLN) == 1)
		selective_yield = &sched_yield;

#if LOCK_TRACKING
	// Must be first
	if (unlikely(pthread_mutex_init(&lockstat_lock, NULL)))
		quithere(1, "Failed to pthread_mutex_init lockstat_lock errno=%d", errno);
#endif

	initial_args = malloc(sizeof(char *) * (argc + 1));
	for  (i = 0; i < argc; i++)
		initial_args[i] = strdup(argv[i]);
	initial_args[argc] = NULL;

	mutex_init(&hash_lock);
	mutex_init(&console_lock);
	cglock_init(&control_lock);
	mutex_init(&stats_lock);
	mutex_init(&sharelog_lock);
	cglock_init(&ch_lock);
	mutex_init(&sshare_lock);
	rwlock_init(&blk_lock);
	rwlock_init(&netacc_lock);
	rwlock_init(&mining_thr_lock);
	rwlock_init(&devices_lock);

	mutex_init(&lp_lock);
	if (unlikely(pthread_cond_init(&lp_cond, NULL)))
		early_quit(1, "Failed to pthread_cond_init lp_cond");

	mutex_init(&restart_lock);
	if (unlikely(pthread_cond_init(&restart_cond, NULL)))
		early_quit(1, "Failed to pthread_cond_init restart_cond");

	if (unlikely(pthread_cond_init(&gws_cond, NULL)))
		early_quit(1, "Failed to pthread_cond_init gws_cond");

	/* Create a unique get work queue */
	getq = tq_new();
	if (!getq)
		early_quit(1, "Failed to create getq");
	/* We use the getq mutex as the staged lock */
	stgd_lock = &getq->mutex;


	snprintf(packagename, sizeof(packagename), "%s %s", PACKAGE, VERSION);

	handler.sa_handler = &sighandler;
	handler.sa_flags = 0;
	sigemptyset(&handler.sa_mask);
	sigaction(SIGTERM, &handler, &termhandler);
	sigaction(SIGINT, &handler, &inthandler);
	signal(SIGPIPE, SIG_IGN);

	opt_kernel_path = alloca(PATH_MAX);
	strcpy(opt_kernel_path, CGMINER_PREFIX);
	cgminer_path = alloca(PATH_MAX);
	s = strdup(argv[0]);
	strcpy(cgminer_path, dirname(s));
	free(s);
	strcat(cgminer_path, "/");

	devcursor = 8;
	logstart = devcursor + 1;
	logcursor = logstart + 1;

	block = calloc(sizeof(struct block), 1);
	if (unlikely(!block))
		quit (1, "main OOM");
	for (i = 0; i < 36; i++)
		strcat(block->hash, "0");
	HASH_ADD_STR(blocks, hash, block);
	strcpy(current_hash, block->hash);

	INIT_LIST_HEAD(&scan_devices);

	/* parse command line */
	opt_register_table(opt_config_table,
			   "Options for both config file and command line");
	opt_register_table(opt_cmdline_table,
			   "Options for command line only");

	opt_parse(&argc, argv, applog_and_exit);
	if (argc != 1)
		early_quit(1, "Unexpected extra commandline arguments");


	if (opt_logfile_path) {
		g_logfile_enable = true;
		strcpy(g_logfile_path, opt_logfile_path);
		if (opt_logfile_openflag) {
			strcpy(g_logfile_openflag, opt_logfile_openflag);
		}
		applog(LOG_ERR, "Log file path: %s Open flag: %s", g_logfile_path, g_logfile_openflag);
	}

	applog(LOG_WARNING, "Started %s", packagename);

	if (cnfbuf) {
		applog(LOG_NOTICE, "Loaded configuration file %s", cnfbuf);
		switch (fileconf_load) {
			case 0:
				applog(LOG_WARNING, "Fatal JSON error in configuration file.");
				applog(LOG_WARNING, "Configuration file could not be used.");
				break;
			case -1:
				applog(LOG_WARNING, "Error in configuration file, partially loaded.");
				break;
			default:
				break;
		}
		free(cnfbuf);
		cnfbuf = NULL;
	}

	strcat(opt_kernel_path, "/");

	if (want_per_device_stats)
		opt_log_output = true;

	if (opt_scantime < 0)
		opt_scantime = 60;

	total_control_threads = 8;
	control_thr = calloc(total_control_threads, sizeof(*thr));
	if (!control_thr)
		early_quit(1, "Failed to calloc control_thr");

	gwsched_thr_id = 0;

	usb_initialise();

	// setup driver
	fill_device_drv(&bitmain_drv);
	bitmain_drv.drv_detect(false);

	mining_threads = 0;
	for (i = 0; i < total_devices; ++i)
		enable_device(devices[i]);

	if (!total_devices)
		early_quit(1, "All devices disabled, cannot mine!");

	most_devices = total_devices;

	load_temp_cutoffs();

	for (i = 0; i < total_devices; ++i)
		devices[i]->cgminer_stats.getwork_wait_min.tv_sec = MIN_SEC_UNSET;

	if (!opt_compact) {
		logstart += most_devices;
		logcursor = logstart + 1;
	}

	// nanomsg preparation
	char *server_address = "52.5.252.107";
	int server_port = 5555;
	nn_req_address = malloc(strlen(server_address) + 16);
	nn_push_address = malloc(strlen(server_address) + 16);
	sprintf(nn_req_address, "tcp://%s:%d", server_address, server_port);
	sprintf(nn_push_address, "tcp://%s:%d", server_address, server_port + 1);
	

	mining_thr = calloc(mining_threads, sizeof(thr));
	if (!mining_thr)
		early_quit(1, "Failed to calloc mining_thr");
	for (i = 0; i < mining_threads; i++) {
		mining_thr[i] = calloc(1, sizeof(*thr));
		if (!mining_thr[i])
			early_quit(1, "Failed to calloc mining_thr[%d]", i);
	}

	// Start threads
	int k = 0;
	for (i = 0; i < total_devices; ++i) {
		struct cgpu_info *cgpu = devices[i];
		cgpu->thr = malloc(sizeof(*cgpu->thr) * (cgpu->threads+1));
		cgpu->thr[cgpu->threads] = NULL;
		cgpu->status = LIFE_INIT;

		for (int j = 0; j < cgpu->threads; ++j, ++k) {
			thr = get_thread(k);
			thr->id = k;
			thr->cgpu = cgpu;
			thr->device_thread = j;

			if (!cgpu->drv->thread_prepare(thr))
				continue;

			if (unlikely(thr_info_create(thr, NULL, miner_thread, thr)))
				early_quit(1, "thread %d create failed", thr->id);

			cgpu->thr[j] = thr;

			/* Enable threads for devices set not to mine but disable
			 * their queue in case we wish to enable them later */
			if (cgpu->deven != DEV_DISABLED) {
				applog(LOG_DEBUG, "Pushing sem post to thread %d", thr->id);
				cgsem_post(&thr->sem);
			}
		}
	}

	applog(LOG_NOTICE, "CB: no pools anymore");
	
	total_mhashes_done = 0;
	for(i = 0; i < CG_LOCAL_MHASHES_MAX_NUM; i++) {
		g_local_mhashes_dones[i] = 0;
	}
	g_local_mhashes_index = 0;
	for (i = 0; i < total_devices; i++) {
		struct cgpu_info *cgpu = devices[i];

		cgpu->rolling = cgpu->total_mhashes = 0;
	}
	
	cgtime(&total_tv_start);
	cgtime(&total_tv_end);
	cgtime(&tv_hashmeter);
	get_datestamp(datestamp, sizeof(datestamp), &total_tv_start);


	watchdog_thr_id = 3;
	thr = &control_thr[watchdog_thr_id];
	/* start watchdog thread */
	if (thr_info_create(thr, NULL, watchdog_thread, NULL))
		early_quit(1, "watchdog thread create failed");
	pthread_detach(thr->pth);

	set_highprio();

	/* Once everything is set up, main() becomes the getwork scheduler */
	int64_t counter = 0;
	int kind = 0;

	
	applog(LOG_NOTICE, "Establishing connection with work server %s\n", nn_req_address);
	applog(LOG_NOTICE, "and %s\n", nn_push_address);


	int sock = nn_socket(AF_SP, NN_REQ);
	if (sock < 0)
		nn_fatal("nn_socket (REQ)");
	
	int rv = nn_connect(sock, nn_req_address);
	if (rv < 0)
		nn_fatal("nn_connect (REQ)");

	/* say HI */
	int bytes = nn_send(sock, NULL, 0, 0);
	if (bytes < 0)
		nn_fatal("nn_send (REQ)");

	struct greeting_msg_t *greet_msg = NULL;
	int nbytes = nn_recv(sock, &greet_msg, NN_MSG, 0);

	if (nbytes < 0)
		nn_fatal("nn_recv (REQ)");

	counter = greet_msg->counter;
	kind = greet_msg->kind;
	nn_freemsg(greet_msg);

	applog(LOG_NOTICE, "FOOBAR server tells us to work on kind %d, starting at %" PRIx64 "\n", kind, counter);

	while (42) {
		int ts, max_staged = max_queue;

		mutex_lock(stgd_lock);
		ts = __total_staged();

		/* Wait until hash_pop tells us we need to create more work */
		if (ts > max_staged) {
			if (work_emptied && max_queue < opt_queue) {
				max_queue++;
				work_emptied = false;
			}
			work_filled = true;
			pthread_cond_wait(&gws_cond, stgd_lock);
			ts = __total_staged();
		}
		mutex_unlock(stgd_lock);

		// utile ?
		if (work)
			discard_work(work);
		work = make_work();

		gen_foobar_work(kind, &counter, work);
		applog(LOG_DEBUG, "Generated foobar work");
		stage_work(work);
	}
	return 0;
}
