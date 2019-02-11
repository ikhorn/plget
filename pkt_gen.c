/*
 * Copyright (C) 2018
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

#include <poll.h>
#include <stdio.h>
#include "pkt_gen.h"
#include <unistd.h>
#include <errno.h>

#define MAX_LATENCY			5000

static int fast_pktgen(void)
{
	struct sockaddr *addr = (struct sockaddr *)&plget->sk_addr;
	int dsize = plget->sk_payload_size;
	char *packet = plget->pkt;
	int sid = plget->stream_id;
	int sfd = plget->sfd;
	int ret;

	plget->inum = plget->pkt_num ? plget->pkt_num : ~0;
	for (plget->icnt = 0; plget->icnt < plget->inum; plget->icnt++) {
		if (plget->flags & PLF_PTP)
			sid_wr(htons((plget->icnt & SEQ_ID_MASK) | sid));

		tid_wr(plget->icnt);
		ret = sendto(sfd, packet, dsize, 0, addr,
			     sizeof(plget->sk_addr));
		if (ret != dsize) {
			if (ret < 0)
				perror("sendto");
			else
				perror("cannot send whole packet\n");

			break;
		}
	}

	plget->pkt_num = plget->icnt;
	return !(plget->icnt == plget->inum);
}

int pktgen_proc(void)
{
	struct sockaddr *addr = (struct sockaddr *)&plget->sk_addr;
	int dsize = plget->sk_payload_size;
	int sid = plget->stream_id;
	char *packet = plget->pkt;
	int sfd = plget->sfd;
	struct pollfd fds[1];
	uint64_t exps;
	int ret;

	ret = plget_start_timer();
	if (ret)
		return ret;

	fds[0].fd = plget->timer_fd;
	fds[0].events = POLLIN;

	plget->inum = plget->pkt_num ? plget->pkt_num : ~0;
	tid_wr(0);

	for (plget->icnt = 0; plget->icnt < plget->inum;) {
		ret = poll(fds, 1, MAX_LATENCY);
		if (ret <= 0) {
			if (!ret) {
				printf("Timed out\n");
				break;
			}

			perror("Some error on poll()");
			break;
		}

		/* time to send new packet */
		if (fds[0].revents & POLLIN) {
			ret = read(plget->timer_fd, &exps, sizeof(exps));
			if (ret < 0)
				return perror("Couldn't read timerfd"), -errno;

			ret = sendto(sfd, packet, dsize, 0, addr,
				     sizeof(plget->sk_addr));
			if (ret != dsize) {
				if (ret < 0)
					perror("sendto");
				else
					perror("cannot send whole packet\n");

				break;
			}

			if (plget->flags & PLF_PTP)
				sid_wr(htons((++plget->icnt & SEQ_ID_MASK) |
					      sid));
			tid_wr(plget->icnt);
		}
	}

	plget->pkt_num = plget->icnt;
	return !(plget->icnt == plget->inum);
}

int pktgen(void)
{
	int ret;

	if (!ts_correct(&plget->interval))
		return fast_pktgen();

	ret = plget_create_timer();
	if (ret)
		return ret;

	ret = pktgen_proc();

	close(plget->timer_fd);
	return ret;
}
