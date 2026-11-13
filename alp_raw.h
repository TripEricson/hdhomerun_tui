/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/*
 * /dev/alpraw%d - raw ATSC 3.0 ALP packets and L1 signalling from alp.ko.
 * Shared by the kernel module and userspace tools.
 *
 * read():  each call returns one record: struct alp_raw_hdr followed by
 *          hdr.len bytes of one complete, unmodified ALP packet. The buffer
 *          must hold the whole record (65536 + sizeof(hdr) always does);
 *          a smaller buffer gets -EMSGSIZE and the record stays queued.
 *          The first read() (or poll()) on an open file claims the device as
 *          its reader and starts the tuner's ALP feed; only one reader at a
 *          time (-EBUSY otherwise). Closing it stops the feed if no one else
 *          (e.g. the alp%d network interface) needs it.
 * ioctl(): ALP_RAW_IOC_GET_L1 reads L1 signalling; it never claims the
 *          reader, so it can be used alongside a running capture.
 *          ALP_RAW_IOC_SET_XFER sets the USB transfer size (see below).
 *
 * The device node is 0660 root:root unless a udev rule assigns a group;
 * 99-alpraw.rules gives it to group "video" like /dev/dvb.
 */
#ifndef _ALP_RAW_H_
#define _ALP_RAW_H_

#include <linux/types.h>
#include <linux/ioctl.h>

struct alp_raw_hdr {
	__u64 tstamp_ns;	/* CLOCK_REALTIME when the packet was delimited */
	__u32 len;		/* ALP packet bytes that follow */
	__u32 drops;		/* packets dropped (reader too slow) just before */
};

#define ALP_RAW_L1_SNAPSHOT	1	/* demod register snapshot ("CXA3") */
#define ALP_RAW_L1_DETAIL	2	/* raw A/322 L1-Detail bytes */

struct alp_raw_l1 {
	__u32 what;		/* in: ALP_RAW_L1_* */
	__u32 len;		/* in: buffer size; out: bytes written */
	__u64 buf;		/* in: user pointer */
};

/*
 * Errors: ENODATA not tuned to ATSC 3.0, EAGAIN L1-Detail not decoded yet,
 * ENOSPC buffer too small, EOPNOTSUPP bridge can't read L1, ENODEV unplugged.
 */
#define ALP_RAW_IOC_GET_L1	_IOWR(0xA3, 0x40, struct alp_raw_l1)

/*
 * Bridges hand over TS only in whole transfers, so a low-bitrate PLP decoded
 * on its own can take minutes to fill a default (64 KB) one. SET_XFER asks
 * for a smaller transfer, in 188-byte TS packets; 0 returns to the driver
 * default. In: packets requested. Out: packets now in effect (the bridge
 * clamps to its range, and with several tuners on one bridge the smallest
 * request wins). Set it before the first read()/poll() starts the stream;
 * a running stream takes a smaller size at once but a larger one only from
 * its next start. The request lasts until this file is closed or makes
 * another. GET_XFER only reports the size in effect.
 * EOPNOTSUPP: the bridge has no adjustable transfer size.
 */
#define ALP_RAW_IOC_SET_XFER	_IOWR(0xA3, 0x41, __u32)
#define ALP_RAW_IOC_GET_XFER	_IOR(0xA3, 0x42, __u32)

#endif /* _ALP_RAW_H_ */
