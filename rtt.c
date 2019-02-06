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
#include "rx_lat.h"
#include "tx_lat.h"
#include "rtt.h"
#include <stdio.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>

#define MAX_LATENCY			5000

static int rtt_proc(void)
{
	int sid = plget->stream_id;
	struct pollfd fds;
	unsigned int i;
	int timer, ret;
	uint64_t exps;

	timer = ts_correct(&plget->interval);
	if (timer) {
		fds.fd = plget->timer_fd;
		fds.events = POLLIN;

		ret = plget_start_timer();
		if (ret)
			return ret;
	}

	for (i = 0; i < plget->pkt_num; ++i) {
		if (plget->flags & PLF_PTP)
			sid_wr(htons((i & SEQ_ID_MASK) | sid));
		if (!(plget->flags & PLF_TS_ID_ALLOWED))
			tid_wr(i);
		txlat_proc_packet();
		rxlat_proc_packet();

		if (!timer)
			continue;

		ret = poll(&fds, 1, MAX_LATENCY);
		if (ret <= 0)
			return perror("Some error on timer poll()"), -errno;

		ret = read(plget->timer_fd, &exps, sizeof(exps));
		if (ret < 0)
			return perror("Couldn't read timerfd"), -errno;
	}

	return 0;
}

static int rtt_init(void)
{
	int ret;

	if (!ts_correct(&plget->interval))
		return 0;

	ret = plget_create_timer();
	if (ret)
		return ret;

	return 0;
}

int rtt(void)
{
	int ret;

	ret = rtt_init();
	if (ret)
		return ret;

	ret = rtt_proc();

	if (ts_correct(&plget->interval))
		close(plget->timer_fd);

	return ret;
}
