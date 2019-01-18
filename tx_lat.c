#include <linux/net_tstamp.h>
#include <time.h>
#include <sys/timerfd.h>
#include <linux/errqueue.h>
#include <net/ethernet.h>
#include "plget_args.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "plget.h"
#include "stat.h"
#include "tx_lat.h"
#include "xdp_sock.h"
#include <poll.h>
#include <unistd.h>
#include <errno.h>

#define MAX_LATENCY			5000

static int init_tx_test(struct plgett *plget)
{
	if (ts_correct(&plget->interval))
		return 0;

	plget->interval.tv_sec = 1;
	return 0;
}

static int get_tx_tstamps(struct plgett *plget)
{
	struct scm_timestamping *tss = NULL;
	struct msghdr *msg = &plget->msg;
	struct sock_extended_err *serr;
	int i, ts_type, psize;
	struct cmsghdr *cmsg;
	struct timespec *ts;
	unsigned int ts_id;
	struct stats *v;
	char *magic;

	plget->msg.msg_controllen = sizeof(plget->control);
	psize = recvmsg(plget->sfd, msg, MSG_ERRQUEUE);
	if (psize < 0) {
		perror("recvmsg error occured");
		return -1;
	}
	plget->pkt_size = psize;

	/* get end timestamps */
	for (cmsg = CMSG_FIRSTHDR(msg); cmsg; cmsg = CMSG_NXTHDR(msg, cmsg)) {
		if (cmsg->cmsg_level == SOL_SOCKET &&
		    cmsg->cmsg_type == SCM_TIMESTAMPING) {
			tss = (struct scm_timestamping *) CMSG_DATA(cmsg);
			continue;
		} else if (!((cmsg->cmsg_level == SOL_IP &&
			      cmsg->cmsg_type == IP_RECVERR) ||
			     (cmsg->cmsg_level == SOL_PACKET &&
			      cmsg->cmsg_type == PACKET_TX_TIMESTAMP))) {
			continue;
		}

		serr = (void *) CMSG_DATA(cmsg);
		if (serr->ee_errno != ENOMSG ||
		    serr->ee_origin != SO_EE_ORIGIN_TIMESTAMPING) {
			continue;
		}

		ts_type = serr->ee_info;
	}

	if (!(plget->flags & PLF_TS_ID_ALLOWED)) {
		/* check MAGIC number and get timestamp id */
		magic = magic_rd(plget, psize);
		if (*magic != MAGIC) {
			printf("incorrect MAGIC number 0x%x\n", *magic);
			return -1;
		}

		ts_id = tid_rd(plget, psize);
	} else {
		ts_id = serr->ee_data;
	}

	if (!tss)
		return plget->mod == RTT_MOD ? 0 : -1;

	ts = tss->ts;
	if (ts_type == SCM_TSTAMP_SCHED) {
		for (i = 0; i < plget->dev_deep; i++) {
			v = &tx_sch_v[i];
			if (!stats_correct_id(v, ts_id)) {
				stats_push_id(v, ts, ts_id);
				return 0;
			}
		}

		return -1;
	}

	if (ts_correct(ts))
		stats_push_id(&tx_sw_v, ts, ts_id);
	else
		stats_push_id(&tx_hw_v, ts + 2, ts_id);
	return 0;
}

static void txlat_stop_timer(int fd)
{
	struct itimerspec tspec = { 0 };

	timerfd_settime(fd, 0, &tspec, NULL);
}

static int txlat_sendto(struct plgett *plget)
{
	int ret;

	if (plget->pkt_type != PKT_XDP) {
		ret = sendto(plget->sfd, plget->pkt, plget->sk_payload_size, 0,
				(struct sockaddr *)&plget->sk_addr,
				sizeof(plget->sk_addr));
		return ret;
	}

	ret = xsk_sendto(plget);
	return ret;
}

static int txlat_proc_packets(struct plgett *plget, int pkt_num)
{
	int tx_cnt = 0, rx_cnt = 0;
	int sid = plget->stream_id;
	struct pollfd fds[2];
	struct timespec ts;
	int timer_fd, ret;
	uint64_t exps;
	int ts_num;

	ts_num = pkt_num * (plget->dev_deep + 1);
	timer_fd = plget_setup_timer(plget);
	if (timer_fd < 0)
		return 1;

	fds[0].fd = plget->sfd;
	fds[0].events = POLLERR;
	fds[1].fd = timer_fd;
	fds[1].events = POLLIN;

	for (;;) {
		ret = poll(fds, 2, MAX_LATENCY);
		if (ret <= 0) {
			if (!ret) {
				ret = -1;
				printf("Timed out, tx packets: %d, "
				       "ts num: %d\n", tx_cnt, rx_cnt);
				goto err;
			}

			perror("Some error on poll()");
			ret = -errno;
			goto err;
		}

		/* time to send new packet */
		if (fds[1].revents & POLLIN) {
			ret = read(timer_fd, &exps, sizeof(exps));
			if (ret < 0) {
				perror("Couldn't read timerfd");
				goto err;
			}

			sid_wr(plget, htons((tx_cnt & SEQ_ID_MASK) | sid));
			if (!(plget->flags & PLF_TS_ID_ALLOWED))
				tid_wr(plget, tx_cnt);
			if (++tx_cnt >= pkt_num)
				txlat_stop_timer(timer_fd);

			/* send packet */
			clock_gettime(CLOCK_REALTIME, &ts);
			ret = txlat_sendto(plget);

			stats_push(&tx_app_v, &ts);
			if (ret != plget->sk_payload_size) {
				if (ret < 0)
					perror("sendto");
				else
					perror("sendto: cannot send whole packet\n");
			}
		}

		/* receive timestamps */
		if (fds[0].revents & POLLERR) {
			ret = get_tx_tstamps(plget);
			if (ret < 0)
				continue;

			if (++rx_cnt >= ts_num)
				break;
		}
	}

	ret = 0;
err:
	close(timer_fd);
	return ret;
}

/*
 * txlat_proc_packet - send packet and receive hw or sw ts
 * @ plget - pointer on shared data
 */
void txlat_proc_packet(struct plgett *plget)
{
	int ts_num, rx_cnt = 0;
	struct pollfd fds[1];
	struct timespec ts;
	int ret;

	fds[0].fd = plget->sfd;
	fds[0].events = POLLERR;
	ts_num = plget->dev_deep + 1;

	/* send packet */
	clock_gettime(CLOCK_REALTIME, &ts);
	ret = sendto(plget->sfd, plget->pkt,
			plget->sk_payload_size, 0,
			(struct sockaddr *)&plget->sk_addr,
			sizeof(plget->sk_addr));

	stats_push(&tx_app_v, &ts);
	if (ret != plget->sk_payload_size) {
		if (ret < 0)
			perror("sendto");
		else
			perror("sendto: cannot send whole packet\n");
	}

	for (;;) {
		ret = poll(fds, 1, MAX_LATENCY);
		if (ret <= 0) {
			if (ret == 0) {
				printf("Timed out get tx timestamp\n");
				return;
			}

			perror("Some error on poll()");
			return;
		}

		/* receive timestamps */
		if (fds[0].revents & POLLERR) {
			ret = get_tx_tstamps(plget);
			if (ret < 0)
				printf("Can't get tx timestamp\n");

			if (++rx_cnt >= ts_num)
				break;
		}
	}
}

int txlat(struct plgett *plget)
{
	int ret;

	ret = init_tx_test(plget);
	if (ret)
		return ret;

	ret = txlat_proc_packets(plget, plget->pkt_num);
	return ret;
}
