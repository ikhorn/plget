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

#define RATE_INERVAL			1

void rxlat_handle_ts(struct plgett *plget, struct timespec *ts)
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

	stats_push(&rx_sw_v, tss->ts);
	stats_push(&rx_hw_v, tss->ts + 2);
	stats_push(&rx_app_v, ts);
}

void rxlat_proc_packet(struct plgett *plget)
{
	struct timespec ts;
	int pkt_size, err;

	plget->msg.msg_controllen = sizeof(plget->control);
	pkt_size = recvmsg(plget->sfd, &plget->msg, 0);

	err = clock_gettime(CLOCK_REALTIME, &ts);
	if (err)
		perror("gettimeofday");

	if (pkt_size < 0)
		return perror("recvmsg");

	rxlat_handle_ts(plget, &ts);
	plget->sk_payload_size = pkt_size;
}

int rxlat(struct plgett *plget)
{
	int i;

	for (i = 0; i < plget->pkt_num; ++i)
		rxlat_proc_packet(plget);

	return 0;
}

static int rxrate_proc_packet(struct plgett *plget, struct timespec *ts)
{
	struct msghdr *msg = &plget->msg;
	struct scm_timestamping *tss = NULL;
	struct cmsghdr *cmsg;
	int pkt_size;

	msg->msg_controllen = sizeof(plget->control);
	pkt_size = recvmsg(plget->sfd, msg, 0);
	if (pkt_size < 0) {
		return perror("recvmsg"), -errno;
	}

	plget->pkt_size = pkt_size;
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

int rxrate(struct plgett *plget)
{
	struct timespec interval, first, last;
	int dsize = 0, pnum = 0, hw = 0;
	int hsize = ETH_HLEN;
	struct pollfd fds[2];
	int timer_fd, ret;
	uint64_t exps;

	if (plget->pkt_type == PKT_UDP)
		hsize += 28;

	if (!ts_correct(&plget->interval))
		plget->interval.tv_sec = 1;

	timer_fd = plget_setup_timer(plget);
	if (timer_fd < 0)
		return timer_fd;

	fds[0].fd = plget->sfd;
	fds[0].events = POLLIN;
	fds[1].fd = timer_fd;
	fds[1].events = POLLIN;

	for (;;) {
		ret = poll(fds, 2, -1);
		if (ret <= 0) {
			perror("Some error on poll()");
			ret = -errno;
			goto err;
		}

		/* receive packet */
		if (fds[0].revents & POLLIN) {
			hw = rxrate_proc_packet(plget, &last);
			if (hw >= 0) {
				dsize += plget->pkt_size;
				if (!pnum++)
					first = last;
			}
		}

		/* print speed */
		if (fds[1].revents & POLLIN) {
			ret = read(timer_fd, &exps, sizeof(uint64_t));
			if (ret < 0) {
				perror("Couldn't read timerfd");
				goto err;
			}

			if (pnum <= 1) {
				interval = plget->interval;
			} else {
				ts_sub(&last, &first, &interval);
				dsize -= plget->pkt_size;
				pnum--;
			}

			dsize = dsize + hsize * pnum;
			hw ? printf("H/W ") : printf("S/W ");
			stats_drate_print(&interval, pnum, dsize);
			dsize = 0;
			pnum = 0;
		}
	}

	ret = 0;
err:
	close(timer_fd);
	return ret;
}
