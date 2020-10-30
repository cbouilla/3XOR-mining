/*
 * Copyright 2012-2013 Andrew Smith
 * Copyright 2013-2014 Con Kolivas <kernel@kolivas.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#ifndef USBUTILS_H
#define USBUTILS_H

#include "util.h"

struct usb_find_devices {};
struct libusb_device {};

struct cg_usb_info {
	uint8_t bus_number;
	uint8_t device_address;
	int usbstat;
	bool nodev;
	bool initialised;
	int nodev_count;
	struct timeval last_nodev;
	uint32_t ioerr_count;
	uint32_t continuous_ioerr_count;

	/*
	 * for nodev and cgusb access (read and write)
	 * it's a pointer so MMQ can have it in multiple devices
	 *
	 * N.B. general mining code doesn't need to use the read
	 * lock for 'nodev' if it calls a usb_read/write/etc function
	 * that uses the lock - however, all usbutils code MUST use it
	 * to avoid devices disappearing while in use by multiple threads
	 */
	cglock_t devlock;

	time_t last_pipe;
	uint64_t pipe_count;
	uint64_t clear_err_count;
	uint64_t retry_err_count;
	uint64_t clear_fail_count;

	uint64_t read_delay_count;
	double total_read_delay;
	uint64_t write_delay_count;
	double total_write_delay;

	uint64_t tmo_count;
	//struct cg_usb_tmo usb_tmo[USB_TMOS];
};

#define ENUMERATION(a,b) a,
#define JUMPTABLE(a,b) b,

#define USB_PARSE_COMMANDS(USB_ADD_COMMAND) \
	USB_ADD_COMMAND(C_BITMAIN_SEND, "BitmainSend") \
	USB_ADD_COMMAND(C_BITMAIN_READ, "BitmainRead") \
	USB_ADD_COMMAND(C_BITMAIN_TOKEN_TXCONFIG, "BitmainTokenTxConfig") \
	USB_ADD_COMMAND(C_BITMAIN_TOKEN_TXTASK, "BitmainTokenTxTask") \
	USB_ADD_COMMAND(C_BITMAIN_TOKEN_RXSTATUS, "BitmainTokenRxStatus") \
	USB_ADD_COMMAND(C_BITMAIN_DATA_RXSTATUS, "BitmainDataRxStatus") \
	USB_ADD_COMMAND(C_BITMAIN_DATA_RXNONCE, "BitmainDataRxNonce")

/* Create usb_cmds enum from USB_PARSE_COMMANDS macro */
enum usb_cmds {
	USB_PARSE_COMMANDS(ENUMERATION)
	C_MAX
};

struct device_drv;
struct cgpu_info;


struct cgpu_info *btm_alloc_cgpu(struct device_drv *drv, int threads);
struct cgpu_info *btm_free_cgpu(struct cgpu_info *cgpu);
void btm_uninit(struct cgpu_info *cgpu);
bool btm_init(struct cgpu_info *cgpu, const char * devpath);
void btm_detect(struct device_drv *drv, bool (*device_detect)(const char*));
int btm_read(struct cgpu_info *cgpu, char *buf, size_t bufsize);
int btm_write(struct cgpu_info *cgpu, char *buf, size_t bufsize);






bool async_usb_transfers(void);
void cancel_usb_transfers(void);
void usb_all(int level);
void usb_list(void);
const char *usb_cmdname(enum usb_cmds cmd);
void usb_applog(struct cgpu_info *cgpu, enum usb_cmds cmd, char *msg, int amount, int err);
void blacklist_cgpu(struct cgpu_info *cgpu);
void whitelist_cgpu(struct cgpu_info *cgpu);
void usb_nodev(struct cgpu_info *cgpu);
struct cgpu_info *usb_copy_cgpu(struct cgpu_info *orig);
struct cgpu_info *usb_alloc_cgpu(struct device_drv *drv, int threads);
struct cgpu_info *usb_free_cgpu(struct cgpu_info *cgpu);
void usb_uninit(struct cgpu_info *cgpu);
bool usb_init(struct cgpu_info *cgpu, struct libusb_device *dev, struct usb_find_devices *found);
void __usb_detect(struct device_drv *drv, struct cgpu_info *(*device_detect)(struct libusb_device *, struct usb_find_devices *),
		  bool single);
#define usb_detect(drv, cgpu) __usb_detect(drv, cgpu, false)
#define usb_detect_one(drv, cgpu) __usb_detect(drv, cgpu, true)
struct api_data *api_usb_stats(int *count);
void update_usb_stats(struct cgpu_info *cgpu);
void usb_reset(struct cgpu_info *cgpu);
int _usb_read(struct cgpu_info *cgpu, int intinfo, int epinfo, char *buf, size_t bufsiz, int *processed, int timeout, const char *end, enum usb_cmds cmd, bool readonce, bool cancellable);
int _usb_write(struct cgpu_info *cgpu, int intinfo, int epinfo, char *buf, size_t bufsiz, int *processed, int timeout, enum usb_cmds);
int _usb_transfer(struct cgpu_info *cgpu, uint8_t request_type, uint8_t bRequest, uint16_t wValue, uint16_t wIndex, uint32_t *data, int siz, unsigned int timeout, enum usb_cmds cmd);
int _usb_transfer_read(struct cgpu_info *cgpu, uint8_t request_type, uint8_t bRequest, uint16_t wValue, uint16_t wIndex, char *buf, int bufsiz, int *amount, unsigned int timeout, enum usb_cmds cmd);
int usb_ftdi_cts(struct cgpu_info *cgpu);
int _usb_ftdi_set_latency(struct cgpu_info *cgpu, int intinfo);
#define usb_ftdi_set_latency(_cgpu) _usb_ftdi_set_latency(_cgpu, DEFAULT_INTINFO)
void usb_buffer_clear(struct cgpu_info *cgpu);
uint32_t usb_buffer_size(struct cgpu_info *cgpu);
int _usb_interface(struct cgpu_info *cgpu, int intinfo);
#define usb_interface(_cgpu) _usb_interface(_cgpu, DEFAULT_INTINFO)
enum sub_ident usb_ident(struct cgpu_info *cgpu);
void usb_set_dev_start(struct cgpu_info *cgpu);
void usb_cleanup();
void usb_initialise();
void *usb_resource_thread(void *userdata);
void initialise_usblocks(void);

#define usb_read(cgpu, buf, bufsiz, read, cmd) \
	_usb_read(cgpu, DEFAULT_INTINFO, DEFAULT_EP_IN, buf, bufsiz, read, DEVTIMEOUT, NULL, cmd, false, false)

#define usb_read_cancellable(cgpu, buf, bufsiz, read, cmd) \
	_usb_read(cgpu, DEFAULT_INTINFO, DEFAULT_EP_IN, buf, bufsiz, read, DEVTIMEOUT, NULL, cmd, false, true)

#define usb_read_ii(cgpu, intinfo, buf, bufsiz, read, cmd) \
	_usb_read(cgpu, intinfo, DEFAULT_EP_IN, buf, bufsiz, read, DEVTIMEOUT, NULL, cmd, false, false)

#define usb_read_once(cgpu, buf, bufsiz, read, cmd) \
	_usb_read(cgpu, DEFAULT_INTINFO, DEFAULT_EP_IN, buf, bufsiz, read, DEVTIMEOUT, NULL, cmd, true, false)

#define usb_read_ii_once(cgpu, intinfo, buf, bufsiz, read, cmd) \
	_usb_read(cgpu, intinfo, DEFAULT_EP_IN, buf, bufsiz, read, DEVTIMEOUT, NULL, cmd, true, false)

#define usb_read_once_timeout(cgpu, buf, bufsiz, read, timeout, cmd) \
	_usb_read(cgpu, DEFAULT_INTINFO, DEFAULT_EP_IN, buf, bufsiz, read, timeout, NULL, cmd, true, false)

#define usb_read_once_timeout_cancellable(cgpu, buf, bufsiz, read, timeout, cmd) \
	_usb_read(cgpu, DEFAULT_INTINFO, DEFAULT_EP_IN, buf, bufsiz, read, timeout, NULL, cmd, true, true)

#define usb_read_ii_once_timeout(cgpu, intinfo, buf, bufsiz, read, timeout, cmd) \
	_usb_read(cgpu, intinfo, DEFAULT_EP_IN, buf, bufsiz, read, timeout, NULL, cmd, true, false)

#define usb_read_nl(cgpu, buf, bufsiz, read, cmd) \
	_usb_read(cgpu, DEFAULT_INTINFO, DEFAULT_EP_IN, buf, bufsiz, read, DEVTIMEOUT, "\n", cmd, false, false)

#define usb_read_nl_timeout(cgpu, buf, bufsiz, read, timeout, cmd) \
	_usb_read(cgpu, DEFAULT_INTINFO, DEFAULT_EP_IN, buf, bufsiz, read, timeout, "\n", cmd, false, false)

#define usb_read_ok(cgpu, buf, bufsiz, read, cmd) \
	_usb_read(cgpu, DEFAULT_INTINFO, DEFAULT_EP_IN, buf, bufsiz, read, DEVTIMEOUT, "OK\n", cmd, false, false)

#define usb_read_ok_timeout(cgpu, buf, bufsiz, read, timeout, cmd) \
	_usb_read(cgpu, DEFAULT_INTINFO, DEFAULT_EP_IN, buf, bufsiz, read, timeout, "OK\n", cmd, false, false)

#define usb_read_ep(cgpu, ep, buf, bufsiz, read, cmd) \
	_usb_read(cgpu, DEFAULT_INTINFO, ep, buf, bufsiz, read, DEVTIMEOUT, NULL, cmd, false, false)

#define usb_read_timeout(cgpu, buf, bufsiz, read, timeout, cmd) \
	_usb_read(cgpu, DEFAULT_INTINFO, DEFAULT_EP_IN, buf, bufsiz, read, timeout, NULL, cmd, false, false)

#define usb_read_timeout_cancellable(cgpu, buf, bufsiz, read, timeout, cmd) \
	_usb_read(cgpu, DEFAULT_INTINFO, DEFAULT_EP_IN, buf, bufsiz, read, timeout, NULL, cmd, false, true)

#define usb_read_ii_timeout(cgpu, intinfo, buf, bufsiz, read, timeout, cmd) \
	_usb_read(cgpu, intinfo, DEFAULT_EP_IN, buf, bufsiz, read, timeout, NULL, cmd, false, false)

#define usb_read_ii_timeout_cancellable(cgpu, intinfo, buf, bufsiz, read, timeout, cmd) \
	_usb_read(cgpu, intinfo, DEFAULT_EP_IN, buf, bufsiz, read, timeout, NULL, cmd, false, true)

#define usb_read_ep_timeout(cgpu, ep, buf, bufsiz, read, timeout, cmd) \
	_usb_read(cgpu, DEFAULT_INTINFO, ep, buf, bufsiz, read, timeout, NULL, cmd, false, false)

#define usb_write(cgpu, buf, bufsiz, wrote, cmd) \
	_usb_write(cgpu, DEFAULT_INTINFO, DEFAULT_EP_OUT, buf, bufsiz, wrote, DEVTIMEOUT, cmd)

#define usb_write_ii(cgpu, intinfo, buf, bufsiz, wrote, cmd) \
	_usb_write(cgpu, intinfo, DEFAULT_EP_OUT, buf, bufsiz, wrote, DEVTIMEOUT, cmd)

#define usb_write_ep(cgpu, ep, buf, bufsiz, wrote, cmd) \
	_usb_write(cgpu, DEFAULT_INTINFO, ep, buf, bufsiz, wrote, DEVTIMEOUT, cmd)

#define usb_write_timeout(cgpu, buf, bufsiz, wrote, timeout, cmd) \
	_usb_write(cgpu, DEFAULT_INTINFO, DEFAULT_EP_OUT, buf, bufsiz, wrote, timeout, cmd)

#define usb_write_ep_timeout(cgpu, ep, buf, bufsiz, wrote, timeout, cmd) \
	_usb_write(cgpu, DEFAULT_INTINFO, ep, buf, bufsiz, wrote, timeout, cmd)

#define usb_transfer(cgpu, typ, req, val, idx, cmd) \
	_usb_transfer(cgpu, typ, req, val, idx, NULL, 0, DEVTIMEOUT, cmd)

#define usb_transfer_data(cgpu, typ, req, val, idx, data, len, cmd) \
	_usb_transfer(cgpu, typ, req, val, idx, data, len, DEVTIMEOUT, cmd)

#define usb_transfer_read(cgpu, typ, req, val, idx, buf, bufsiz, read, cmd) \
	_usb_transfer_read(cgpu, typ, req, val, idx, buf, bufsiz, read, DEVTIMEOUT, cmd)

#endif
