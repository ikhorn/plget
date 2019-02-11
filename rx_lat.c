/*
 * Copyright (C) 2017
 * Authors:	Ivan Khoronzhuk <ivan.khoronzhuk@linaro.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/net_tstamp.h>
#include <time.h>
#include <linux/errqueue.h>
#include <net/ethernet.h>
#include <unistd.h>
#include <stdio.h>
#include "plget.h"
#include "stat.h"
#include "rx_lat.h"
#include <errno.h>
#include <poll.h>
#include "xdp_sock.h"
#include <string.h>

#define RATE_INERVAL			1

static void rxlat_handle_ts(struct timespec *ts, __u32 ts_id)
{
	struct scm_timestamping *tss = NULL;
	struct msghdr *msg = &plget->msg;
	struct cmsghdr *cmsg;

	for (cmsg = CMSG_FIRSTHDR(msg); cmsg; cmsg = CMSG_NXTHDR(msg, cmsg)) {
		if (cmsg->cmsg_level != SOL_SOCKET ||
			cmsg->cmsg_type != SCM_TIMESTAMPING)
			continue;

		tss = (struct scm_timestamping *) CMSG_DATA(cmsg);
		break;
	}

	if (!tss) {
		fprintf(stderr, "SCM_TIMESTAMPING not found!\n");
		return;
	}

	if (plget->flags & PLF_TS_ID_ALLOWED) {
		stats_push(&rx_sw_v, tss->ts);
		stats_push(&rx_hw_v, tss->ts + 2);
		stats_push(&rx_app_v, ts);
	} else {
		stats_push_id(&rx_sw_v, tss->ts, ts_id);
		stats_push_id(&rx_hw_v, tss->ts + 2, ts_id);
		stats_push_id(&rx_app_v, ts, ts_id);
	}
}

static int rxlat_recvmsg_raw(struct timespec *ts)
{
	int psize, err;
	__u16 proto;

	do {
		psize = recvmsg(plget->sfd, &plget->msg, 0);
		err = clock_gettime(CLOCK_REALTIME, ts);
		if (err)
			return -1;

		if (psize < 0)
			return -errno;

		memcpy(&proto, plget->data + ETH_ALEN * 2, sizeof(proto));
		if ((plget->flags & PLF_PTP) && proto == htons(ETH_P_1588))
			break;
	} while (0);

	return psize;
}

static int rxlat_recvmsg_xdp(struct timespec *ts)
{
	int psize;
	__u16 proto;

	do {
		psize = xsk_recvmsg(&plget->msg, ts);

		if (psize < 0)
			return -errno;

		memcpy(&proto, plget->rx_pkt + ETH_ALEN * 2, sizeof(proto));
		if ((plget->flags & PLF_PTP) && proto == htons(ETH_P_1588))
			break;
	} while (0);

	return psize;
}

static int rxlat_recvmsg(struct timespec *ts, __u32 *ts_id)
{
	int psize, err;
	char *magic;

	for (;;) {
		if (plget->pkt_type == PKT_XDP)
			psize = rxlat_recvmsg_xdp(ts);
		else if (plget->pkt_type == PKT_RAW) {
			psize = rxlat_recvmsg_raw(ts);
		} else {
			psize = recvmsg(plget->sfd, &plget->msg, 0);
			err = clock_gettime(CLOCK_REALTIME, ts);
			if (err)
				return -1;
		}

		if (plget->flags & PLF_TS_ID_ALLOWED)
			break;

		/* check magic number */
		magic = magic_rx_rd();
		if (*magic == MAGIC) {
			*ts_id = tid_rx_rd();
			break;
		}

		printf("incorrect MAGIC number 0x%x\n", *magic);
	}

	return psize;
}

void rxlat_proc_packet(void)
{
	struct timespec ts;
	__u32 ts_id;
	int psize;

	plget->msg.msg_controllen = sizeof(plget->control);
	psize = rxlat_recvmsg(&ts, &ts_id);

	if (psize < 0)
		return perror("recvmsg");

	rxlat_handle_ts(&ts, ts_id);
	plget->sk_payload_size = psize;
}

int rxlat(void)
{
	plget->inum = plget->pkt_num;
	for (plget->icnt = 0; plget->icnt < plget->pkt_num; ++plget->icnt)
		rxlat_proc_packet();

	return 0;
}

static int rxrate_proc_packet(struct timespec *ts)
{
	struct msghdr *msg = &plget->msg;
	struct scm_timestamping *tss = NULL;
	struct cmsghdr *cmsg;
	int psize;

	msg->msg_controllen = sizeof(plget->control);
	psize = recvmsg(plget->sfd, msg, 0);
	if (psize < 0) {
		return perror("recvmsg"), -errno;
	}

	plget->frame_size = psize;
	for (cmsg = CMSG_FIRSTHDR(msg); cmsg; cmsg = CMSG_NXTHDR(msg, cmsg)) {
		if (cmsg->cmsg_level != SOL_SOCKET ||
			cmsg->cmsg_type != SCM_TIMESTAMPING)
			continue;

		tss = (struct scm_timestamping *) CMSG_DATA(cmsg);
		break;
	}

	if (!tss) {
		fprintf(stderr, "SCM_TIMESTAMPING not found!\n");
		return -1;
	}

	if (ts_correct(tss->ts + 2)) {
		ts->tv_sec = (tss->ts + 2)->tv_sec;
		ts->tv_nsec = (tss->ts + 2)->tv_nsec;
		return 1;
	}

	ts->tv_sec = tss->ts->tv_sec;
	ts->tv_nsec = tss->ts->tv_nsec;
	return 0;
}

int rxrate_proc(void)
{
	struct timespec interval, first, last;
	int dsize = 0, pnum = 0, hw = 0;
	int hsize = ETH_HLEN;
	struct pollfd fds[2];
	uint64_t exps;
	int ret;

	if (plget->pkt_type == PKT_UDP)
		hsize += 28;

	ret = plget_start_timer();
	if (ret)
		return ret;

	fds[0].fd = plget->sfd;
	fds[0].events = POLLIN;
	fds[1].fd = plget->timer_fd;
	fds[1].events = POLLIN;

	for (;;) {
		ret = poll(fds, 2, -1);
		if (ret <= 0)
			return perror("Some error on poll()"), -errno;

		/* receive packet */
		if (fds[0].revents & POLLIN) {
			hw = rxrate_proc_packet(&last);
			if (hw >= 0) {
				plget->frame_size += hsize;
				dsize += plget->frame_size;
				if (!pnum++)
					first = last;
			}
		}

		/* print speed */
		if (fds[1].revents & POLLIN) {
			ret = read(plget->timer_fd, &exps, sizeof(uint64_t));
			if (ret < 0)
				return perror("Couldn't read timerfd"), -errno;

			if (pnum <= 1) {
				interval = plget->interval;
			} else {
				ts_sub(&last, &first, &interval);
				dsize -= plget->frame_size;
				pnum--;
			}

			hw ? printf("H/W ") : printf("S/W ");
			stats_drate_print(&interval, pnum, dsize);
			dsize = 0;
			pnum = 0;
		}
	}

	return 0;
}

static int init_rxrate(void)
{
	if (!ts_correct(&plget->interval))
		plget->interval.tv_sec = 1;

	return plget_create_timer();
}

int rxrate(void)
{
	int ret;

	ret = init_rxrate();
	if (ret)
		return ret;

	ret = rxrate_proc();

	close(plget->timer_fd);
	return ret;
}
