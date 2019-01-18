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

#define MAX_LATENCY			5000

int fast_pktgen(struct plgett *plget)
{
	struct sockaddr *addr = (struct sockaddr *)&plget->sk_addr;
	int dsize = plget->sk_payload_size;
	char *packet = plget->pkt;
	int sid = plget->stream_id;
	int sfd = plget->sfd;
	int pnum, ret, i = 0;

	pnum = plget->pkt_num ? plget->pkt_num : ~0;
	for (i = 0; i < pnum; i++) {
		sid_wr(plget, htons((i & SEQ_ID_MASK) | sid));
		ret = sendto(sfd, packet, dsize, 0, addr,
			     sizeof(plget->sk_addr));
		if (ret != dsize) {
			if (ret < 0)
				perror("sendto");
			else
				perror("cannot send whole packet\n");

			return -1;
		}
	}

	plget->pkt_num = i;
	return 0;
}

int pktgen(struct plgett *plget)
{
	struct sockaddr *addr = (struct sockaddr *)&plget->sk_addr;
	int dsize = plget->sk_payload_size;
	int timer_fd, pnum, ret, i = 0;
	char *packet = plget->pkt;
	int sid = plget->stream_id;
	int sfd = plget->sfd;
	struct pollfd fds[1];
	uint64_t exps;

	if (!ts_correct(&plget->interval))
		return fast_pktgen(plget);

	timer_fd = plget_setup_timer(plget);
	if (timer_fd < 0)
		goto err;

	fds[0].fd = timer_fd;
	fds[0].events = POLLIN;
	pnum = plget->pkt_num ? plget->pkt_num : ~0;
	for (i = 0;;) {
		ret = poll(fds, 1, MAX_LATENCY);
		if (ret <= 0) {
			if (!ret) {
				printf("Timed out\n");
				goto err;
			}

			perror("Some error on poll()");
			goto err;
		}

		/* time to send new packet */
		if (fds[0].revents & POLLIN) {
			ret = read(timer_fd, &exps, sizeof(exps));
			if (ret < 0) {
				perror("Couldn't read timerfd");
				goto err;
			}

			ret = sendto(sfd, packet, dsize, 0, addr,
				     sizeof(plget->sk_addr));
			if (ret != dsize) {
				if (ret < 0)
					perror("sendto");
				else
					perror("cannot send whole packet\n");

				goto err;
			}

			if (++i >= pnum) {
				close(timer_fd);
				break;
			}

			*(__u16 *)(plget->off_sid_wr + plget->pkt) =
				htons((i & SEQ_ID_MASK) | sid);
		}
	}

	plget->pkt_num = i;
	return 0;
err:
	plget->pkt_num = i;
	return -1;
}
