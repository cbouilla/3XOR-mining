/*
 * Copyright 2012-2013 Andrew Smith
 * Copyright 2013-2014 Con Kolivas <kernel@kolivas.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include "config.h"

#include <ctype.h>
#include <stdint.h>
#include <stdbool.h>

#include "logging.h"
#include "miner.h"
#include "usbutils.h"

#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>



// Total device limit
static int total_count = 0;
static int total_limit = 999999;

// For device limits by driver
static struct driver_count {
	int count;
	int limit;
} drv_count[DRIVER_MAX];

struct cgpu_info *usb_free_cgpu(struct cgpu_info *cgpu)
{
	if (cgpu->drv->copy)
		free(cgpu->drv);

	free(cgpu->device_path);

	free(cgpu);

	return NULL;
}

void usb_initialise(void)
{
	for (int i = 0; i < DRIVER_MAX; i++) {
		drv_count[i].count = 0;
		drv_count[i].limit = 999999;
	}
}

struct cgpu_info *btm_alloc_cgpu(struct device_drv *drv, int threads)
{
	struct cgpu_info *cgpu = calloc(1, sizeof(*cgpu));

	if (unlikely(!cgpu))
		quit(1, "Failed to calloc cgpu for %s in usb_alloc_cgpu", drv->dname);

	cgpu->drv = drv;
	cgpu->deven = DEV_ENABLED;
	cgpu->threads = threads;

	cgpu->usbinfo.nodev = true;
	cgpu->device_fd = -1;

	cglock_init(&cgpu->usbinfo.devlock);

	return cgpu;
}

struct cgpu_info *btm_free_cgpu(struct cgpu_info *cgpu)
{
	if (cgpu->drv->copy)
		free(cgpu->drv);

	if(cgpu->device_path) {
		free(cgpu->device_path);
	}

	free(cgpu);

	return NULL;
}

bool btm_init(struct cgpu_info *cgpu, const char * devpath)
{
	int fd = -1;
	if(cgpu->device_fd >= 0) {
		return false;
	}
	fd = open(devpath, O_RDWR|O_EXCL|O_NONBLOCK);

	if(fd == -1) {
		applog(LOG_DEBUG, "%s open %s error %d",
				cgpu->drv->dname, devpath, errno);
		return false;
	}
	cgpu->device_path = strdup(devpath);
	cgpu->device_fd = fd;
	cgpu->usbinfo.nodev = false;
	applog(LOG_DEBUG, "btm_init open device fd = %d", cgpu->device_fd);
	return true;
}

void btm_uninit(struct cgpu_info *cgpu)
{
	applog(LOG_DEBUG, "BTM uninit %s%i", cgpu->drv->name, cgpu->device_fd);

	// May have happened already during a failed initialisation
	//  if release_cgpu() was called due to a USB NODEV(err)
	close(cgpu->device_fd);
	if(cgpu->device_path) {
		free(cgpu->device_path);
		cgpu->device_path = NULL;
	}
}

void btm_detect(struct device_drv *drv, bool (*device_detect)(const char*))
{
	applog(LOG_DEBUG, "BTM scan devices: checking for %s devices", drv->name);

	if (total_count >= total_limit) {
		applog(LOG_DEBUG, "BTM scan devices: total limit %d reached", total_limit);
		return;
	}

	if (drv_count[drv->drv_id].count >= drv_count[drv->drv_id].limit) {
		applog(LOG_DEBUG,
			"BTM scan devices: %s limit %d reached",
			drv->dname, drv_count[drv->drv_id].limit);
		return;
	}
	device_detect("asic");
}

int btm_read(struct cgpu_info *cgpu, char *buf, size_t bufsize)
{
	return read(cgpu->device_fd, buf, bufsize);
}

int btm_write(struct cgpu_info *cgpu, char *buf, size_t bufsize)
{
	return write(cgpu->device_fd, buf, bufsize);
}

