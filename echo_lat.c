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
#include <stdio.h>
#include <unistd.h>
#include "plget.h"
#include "stat.h"
#include <errno.h>
#include "tx_lat.h"
#include "rx_lat.h"
#include "echo_lat.h"
#include <poll.h>
#include <errno.h>

#define MAX_LATENCY			5000

static int echolat_proc(void)
{
	struct ether_addr *dst_addr, *src_addr;
	int type = plget->pkt_type;
	int swap_addr, timer, ret;
	struct ether_header *eth;
	struct pollfd fds;
	uint64_t exps;

	timer = ts_correct(&plget->interval);
	if (timer) {
		fds.fd = plget->timer_fd;
		fds.events = POLLIN;

		ret = plget_start_timer();
		if (ret)
			return ret;
	}

	swap_addr = type == PKT_XDP || type == PKT_RAW;

	plget->inum = plget->pkt_num;
	for (plget->icnt = 0; plget->icnt < plget->pkt_num; ++plget->icnt) {
		rxlat_proc_packet();

		plget->pkt = plget->rx_pkt;

		if (swap_addr) {
			eth = (struct ether_header *)plget->pkt;
			dst_addr = (struct ether_addr *)&eth->ether_dhost;
			src_addr = (struct ether_addr *)&eth->ether_shost;

			if (plget->flags & PLF_ADDR_SET)
				*dst_addr = plget->macaddr;
			else
				*dst_addr = *src_addr;

			*src_addr = plget->if_addr;
		}

		if (timer) {
			ret = poll(&fds, 1, MAX_LATENCY);
			if (ret <= 0)
				return perror("Some error on timer poll()"), -errno;

			ret = read(plget->timer_fd, &exps, sizeof(exps));
			if (ret < 0)
				return perror("Couldn't read timerfd"), -errno;
		}

		txlat_proc_packet();
	}

	return 0;
}

static int echolat_init(void)
{
	int ret;

	if (!ts_correct(&plget->interval))
		return 0;

	ret = plget_create_timer();
	if (ret)
		return ret;

	return 0;
}

int echolat(void)
{
	int ret;

	ret = echolat_init();
	if (ret)
		return ret;

	ret = echolat_proc();

	if (ts_correct(&plget->interval))
		close(plget->timer_fd);

	return ret;
}
