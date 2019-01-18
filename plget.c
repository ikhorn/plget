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

#include <netpacket/packet.h>
#include <linux/net_tstamp.h>
#include <net/ethernet.h>
#include <sys/timerfd.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <linux/sockios.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include "plget_args.h"
#include "plget.h"
#include "rx_lat.h"
#include "tx_lat.h"
#include "rtt.h"
#include "echo_lat.h"
#include <sys/mman.h>
#include "pkt_gen.h"
#include "result.h"
#include "xdp_sock.h"

#define ALIGN_ROUNDUP(x, align)\
	((align) * (((x) + align - 1) / (align)))

#define ALIGN(x) ALIGN_ROUNDUP(x, sizeof(long))

#define ALIGN_ROUNDUP_PTR(x, align)\
	((void *)ALIGN_ROUNDUP((uintptr_t)(x), (uintptr_t)(align)))

#define OFF_PTP_SEQUENCE_ID		30

struct stats tx_app_v;
struct stats *tx_sch_v;
struct stats tx_sw_v;
struct stats tx_hw_v;
struct stats rx_app_v;
struct stats rx_sw_v;
struct stats rx_hw_v;

struct stats temp;

static unsigned char ptpv2_sync_header[] = {
	0x10, 0x02, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x80, 0x63, 0xff, 0xff, 0x00, 0x09, 0xba, 0x00, 0x01,
	0x00, 0x74, 0x00, 0x00
};

#define PTP_HSIZE	sizeof(ptpv2_sync_header)

int plget_setup_timer(struct plgett *plget)
{
	int fd, ret;
	struct itimerspec tspec = { 0 };

	fd = timerfd_create(CLOCK_MONOTONIC, 0);
	if (fd < 0) {
		perror("Couldn't create timer");
		return -1;
	}

	tspec.it_value.tv_sec = 0;
	tspec.it_value.tv_nsec = 100000;

	tspec.it_interval.tv_sec = plget->interval.tv_sec;
	tspec.it_interval.tv_nsec = plget->interval.tv_nsec;

	ret = timerfd_settime(fd, 0, &tspec, NULL);
	if (ret < 0) {
		perror("Couldn't set timer");
		close(fd);
		return -1;
	}

	return fd;
}

int setup_sock_ts(int sfd, int flags)
{
	int val, err;
	unsigned int len = sizeof(val);

	val = flags;
	printf("setting ts_flags: 0x%x\n", val);
	err = setsockopt(sfd, SOL_SOCKET, SO_TIMESTAMPING, &val, len);
	if (err)
		return perror("setsockopt"), -errno;

	err = getsockopt(sfd, SOL_SOCKET, SO_TIMESTAMPING, &val, &len);
	if (err)
		return perror("getsockopt"), -errno;

	printf("new ts_flags: 0x%x\n", val);

	return 0;
}

static int enable_hw_timestamping(struct plgett *plget)
{
	struct hwtstamp_config hwconfig_requested;
	struct hwtstamp_config hwconfig;
	int need_tx_hwts, need_rx_hwts;
	struct ifreq ifreq_ts;
	int ret;

	memset(&ifreq_ts, 0, sizeof(ifreq_ts));
	memset(&hwconfig, 0, sizeof(hwconfig));

	strncpy(ifreq_ts.ifr_name, plget->if_name, sizeof(ifreq_ts.ifr_name));
	ifreq_ts.ifr_data = (void *)&hwconfig;
	ret = ioctl(plget->sfd, SIOCGHWTSTAMP, &ifreq_ts);
	printf("SIOCGHWTSTAMP: tx_type was %d; rx_filter was %d\n",
	       hwconfig.tx_type, hwconfig.rx_filter);

	memset(&ifreq_ts, 0, sizeof(ifreq_ts));
	memset(&hwconfig, 0, sizeof(hwconfig));

	strncpy(ifreq_ts.ifr_name, plget->if_name, sizeof(ifreq_ts.ifr_name));
	ifreq_ts.ifr_data = (void *)&hwconfig;

	need_tx_hwts = plget->mod == TX_LAT ||
		       plget->mod == RTT_MOD ||
		       plget->mod == ECHO_LAT;
	need_rx_hwts = plget->mod == RX_LAT ||
		       plget->mod == RTT_MOD ||
		       plget->mod == RX_RATE ||
		       plget->mod == ECHO_LAT;

	hwconfig.tx_type = need_tx_hwts ? HWTSTAMP_TX_ON :
					  HWTSTAMP_TX_OFF;
	if (!need_rx_hwts) {
		hwconfig.rx_filter = HWTSTAMP_FILTER_NONE;
	} else {
		if (!(plget->flags & PLF_PTP)) {
			hwconfig.rx_filter = HWTSTAMP_FILTER_ALL;
		} else {
			if (plget->pkt_type == PKT_UDP)
				hwconfig.rx_filter =
					HWTSTAMP_FILTER_PTP_V2_L4_SYNC;
			else
				hwconfig.rx_filter =
					HWTSTAMP_FILTER_PTP_V2_L2_SYNC;
		}
	}

	hwconfig_requested = hwconfig;
	ret = ioctl(plget->sfd, SIOCSHWTSTAMP, &ifreq_ts);
	if (ret < 0) {
		if (hwconfig_requested.tx_type == HWTSTAMP_TX_OFF &&
		    hwconfig_requested.rx_filter == HWTSTAMP_FILTER_NONE)
			printf("SIOCSHWTSTAMP: disabling h/w time stamping not possible\n");
		else
			printf("SIOCSHWTSTAMP: NIC h/w ts seems like is not supported: %d\n", ret);

		return -errno;
	}

	printf("SIOCSHWTSTAMP: tx_type %d requested, got %d; rx_filter %d requested, got %d\n",
	       hwconfig_requested.tx_type, hwconfig.tx_type,
	       hwconfig_requested.rx_filter, hwconfig.rx_filter);

	return ret;
}

static int udp_socket(struct plgett *plget)
{
	struct sockaddr_in *addr = (struct sockaddr_in *)&plget->sk_addr;
	int ip_multicast_loop = 0;
	struct ip_mreqn mreq;
	int sfd, ret;

	sfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sfd < 0)
		return perror("socket"), -errno;

	addr->sin_family = AF_INET;
	addr->sin_port = htons(plget->port);

	ret = bind(sfd, (struct sockaddr *)addr, sizeof(struct sockaddr_in));
	if (ret < 0)
		return perror("Couldn't bind"), -errno;

	addr->sin_addr = plget->iaddr;

	if (plget->flags & PLF_PRIO) {
		ret = setsockopt(sfd, SOL_SOCKET, SO_PRIORITY, &plget->prio,
				 sizeof(plget->prio));
		if (ret < 0)
			return perror("Couldn't set priority"), -errno;
	}

	if (plget->flags & PLF_BUSYPOLL) {
		ret = setsockopt(sfd, SOL_SOCKET, SO_BUSY_POLL,
				 &plget->busypoll_time,
				 sizeof(plget->busypoll_time));
		if (ret < 0)
			return perror("Couldn't set busy poll time"), -errno;
	}

	/* bind socket to the interface */
	ret = setsockopt(sfd, SOL_SOCKET, SO_BINDTODEVICE, plget->if_name,
			 sizeof(plget->if_name));
	if (ret < 0)
		return perror("Couldn't bind to the interface"), -errno;

	if (!(plget->flags & PLF_PTP))
		return sfd;

	/* set multicast group for outgoing packets */
	mreq.imr_multiaddr = plget->iaddr;
	mreq.imr_address.s_addr = htonl(INADDR_ANY);
	mreq.imr_ifindex = if_nametoindex(plget->if_name);
	ret = setsockopt(sfd, IPPROTO_IP, IP_MULTICAST_IF, &mreq,
			 sizeof(mreq));
	if (ret < 0)
		return perror("set multicast"), -errno;

	if (plget->mod == TX_LAT || plget->mod == PKT_GEN)
		return sfd;

	/* join multicast group */
	ret = setsockopt(sfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq,
			 sizeof(struct ip_mreqn));
	if (ret < 0)
		return perror("join multicast group"), -errno;

	printf("joined mcast group: %s\n", inet_ntoa(plget->iaddr));

	ret = setsockopt(sfd, IPPROTO_IP, IP_MULTICAST_LOOP,
		         &ip_multicast_loop, sizeof(ip_multicast_loop));
	if (ret < 0)
		perror("loop multicast");

	return sfd;
}

static void specify_protocol(struct plgett *plget, __u16 *protocol)
{
	if (plget->flags & PLF_AVTP)
		*protocol = htons(ETH_P_TSN);
	else if (plget->flags & PLF_PTP)
		*protocol = htons(ETH_P_1588);
	else
		*protocol = 0;
}

static int packet_socket(struct plgett *plget)
{
	struct sockaddr_ll *addr = &plget->sk_addr;
	unsigned char *mac = plget->macaddr;
	struct packet_mreq mreq;
	__u16 protocol;
	int sfd, ret;

	specify_protocol(plget, &protocol);

	sfd = socket(AF_PACKET, SOCK_DGRAM, protocol);
	if (sfd < 0)
		return perror("socket"), -errno;

	addr->sll_family = AF_PACKET;
	addr->sll_protocol = protocol;

	/* If user provided a network interface, bind() to it. */
	if (plget->if_name[0] != '\0')
		addr->sll_ifindex = if_nametoindex(plget->if_name);

	ret = bind(sfd, (struct sockaddr *)addr, sizeof(struct sockaddr_ll));
	if (ret < 0)
		return perror("Couldn't bind() to interface"), -errno;

	addr->sll_halen = ETH_ALEN;
	memcpy(addr->sll_addr, mac, ETH_ALEN);

	if (plget->flags & PLF_PRIO) {
		ret = setsockopt(sfd, SOL_SOCKET, SO_PRIORITY, &plget->prio,
				 sizeof(plget->prio));
		if (ret < 0)
			return perror("Couldn't set priority"), -errno;
	}

	if (plget->flags & PLF_BUSYPOLL) {
		ret = setsockopt(sfd, SOL_SOCKET, SO_BUSY_POLL,
				 &plget->busypoll_time,
				 sizeof(plget->busypoll_time));
		if (ret < 0)
			return perror("Couldn't set busy poll time"), -errno;
	}

	if (plget->mod == TX_LAT || plget->mod == PKT_GEN)
		return sfd;

	 /* join multicast group if address is provided */
	if (*mac == '\0')
		return sfd;

	mreq.mr_ifindex = addr->sll_ifindex;
	mreq.mr_type = PACKET_MR_MULTICAST;
	mreq.mr_alen = ETH_ALEN;
	memcpy(&mreq.mr_address, mac, ETH_ALEN);

	ret = setsockopt(sfd, SOL_PACKET,
			 PACKET_ADD_MEMBERSHIP, &mreq,
			 sizeof(struct packet_mreq));
	if (ret < 0) {
		perror("Couldn't set PACKET_ADD_MEMBERSHIP");
		return -errno;
	}

	printf("joined mcast group: %02x:%02x:%02x:%02x:%02x:%02x\n",
	       mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

	return sfd;
}

static int plget_create_socket(struct plgett *plget)
{
	if (plget->pkt_type == PKT_UDP)
		plget->sfd = udp_socket(plget);
	else if (plget->pkt_type == PKT_ETH)
		plget->sfd = packet_socket(plget);
	else if (plget->pkt_type == PKT_XDP)
		plget->sfd = xdp_socket(plget);
	else
		plget_fail("uknown packet type");

	if (plget->sfd < 0)
		return 1;

	return 0;
}

static void init_pkt_ether_header(struct plgett *plget)
{
	struct ether_header *eth;
	struct ifreq ifr;
	int ret;

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, plget->if_name, sizeof(ifr.ifr_name));

	eth = (struct ether_header *)plget->pkt;
	memcpy(eth->ether_dhost, plget->macaddr, ETH_ALEN);

	ret = ioctl(plget->sfd, SIOCGIFHWADDR, &ifr);
	if (ret) {
		memcpy(eth->ether_shost, plget->macaddr, ETH_ALEN);
		printf("cann't get interface hw address\n");
	} else {
		memcpy(eth->ether_shost, ifr.ifr_hwaddr.sa_data, ETH_ALEN);
	}

	specify_protocol(plget, &eth->ether_type);
}

static void fill_in_packets(struct plgett *plget)
{
	int ptp_payload_size;
	int n, i, j;
	char *dp;

	ptp_payload_size = plget->sk_payload_size;
	if (plget->pkt_type == PKT_XDP)
		ptp_payload_size -= ETH_HLEN;

	if (plget->flags & PLF_PTP)
		ptp_payload_size -= PTP_HSIZE;

	n = (plget->pkt_type == PKT_XDP) ? FRAME_NUM : 1;
	for (i = 0; i < n; i++) {
		if (plget->pkt_type == PKT_XDP) {
			j = FRAME_SIZE * i;
			plget->pkt = &plget->xsk->umem->frames[j];

			init_pkt_ether_header(plget);
			dp = plget->pkt + ETH_HLEN;
		} else {
			dp = plget->pkt;
		}

		if (plget->flags & PLF_PTP) {
			memcpy(dp, ptpv2_sync_header, PTP_HSIZE);
			dp += PTP_HSIZE;
		}

		if (!(plget->flags & PLF_TS_ID_ALLOWED) &&
		    plget->mod != ECHO_LAT)
			*dp++ = MAGIC;

		for (j = 0; j < ptp_payload_size; j++)
			*dp++ = (rand() % 230) + 1;
	}

	if (plget->pkt_type == PKT_XDP)
		plget->pkt = plget->xsk->umem->frames;
}

static int plget_create_packet(struct plgett *plget)
{
	int payload_size;

	/* check settings */
	if (plget->pkt_size &&
	    (plget->pkt_size < 64 || plget->pkt_size > ETH_DATA_LEN)) {
			printf("incorrect packet size: 64 <= size <= 1500\n");
			return -EINVAL;
	}

	/* adjust size and payload for packet */
	if (plget->pkt_type == PKT_UDP) {
		if (plget->flags & PLF_PTP) {
			if (plget->pkt_size &&
			    plget->pkt_size < 100) {
				printf("packet size should be > 99\n");
				return -EINVAL;
			} else if (!plget->pkt_size)
				plget->pkt_size = 100;
		} else if (!plget->pkt_size)
			plget->pkt_size = 66;

		payload_size = plget->pkt_size - 42;
	} else {
		if (plget->flags & PLF_PTP) {
			if (plget->pkt_size &&
			    plget->pkt_size < 72) {
				printf("packet size should be > 71\n");
				return -EINVAL;
			} else if (!plget->pkt_size)
				plget->pkt_size = 72;
		} else if (!plget->pkt_size)
			plget->pkt_size = 64;

		payload_size = plget->pkt_size;
		if (plget->pkt_type != PKT_XDP)
			payload_size -= ETH_HLEN;
	}

	/* allocate packet */
	plget->sk_payload_size = payload_size;
	if (plget->pkt_type != PKT_XDP) {
		plget->pkt = malloc(payload_size);
		if (!plget->pkt)
			return -ENOMEM;
	}

	fill_in_packets(plget);
	return 0;
}

static void fill_in_data_pointers(struct plgett *plget)
{
	int off = 0;

	plget->iov.iov_base = plget->data;
	plget->iov.iov_len = sizeof(plget->data);
	plget->msg.msg_iov = &plget->iov;
	plget->msg.msg_iovlen = 1;
	plget->msg.msg_control = plget->control;
	plget->msg.msg_controllen = sizeof(plget->control);

	if (plget->mod == RX_LAT || plget->mod == RX_RATE)
		return;

	if (plget->pkt_type == PKT_XDP)
		off += ETH_HLEN;

	if (plget->flags & PLF_PTP)
		plget->off_sid_wr = off + OFF_PTP_SEQUENCE_ID;
	else
		plget->off_sid_wr = 0;

	if (!(plget->flags & PLF_TS_ID_ALLOWED)) {
		if (plget->flags & PLF_PTP)
			off += PTP_HSIZE;

		if (plget->mod != ECHO_LAT)
			plget->off_tid_wr = ALIGN(off + 1);

		plget->off_magic_rd = off;
		plget->off_tid_rd = ALIGN(off + 1);

		if (plget->mod != ECHO_LAT) {
			plget->off_magic_rd -= plget->sk_payload_size;
			plget->off_tid_rd -= plget->sk_payload_size;
		}
	}
}

static int init_test(struct plgett *plget)
{
	int ts_flags = SOF_TIMESTAMPING_SOFTWARE;
	int i, ret, mod = plget->mod;

	ret = plget_create_socket(plget);
	if (ret)
		return ret;

	enable_hw_timestamping(plget);
	res_title_print(plget);

	if (mod == RTT_MOD || mod == ECHO_LAT || mod == TX_LAT ||
	    mod == RX_LAT)
		stats_reserve(&temp, plget->pkt_num);

	/* reserve stats memory and set ts flags */
	if (mod == RTT_MOD || mod == ECHO_LAT || mod == TX_LAT) {
		if (plget->flags & PLF_PRINTOUT) {
			stats_reserve(&tx_app_v, plget->pkt_num);
			stats_reserve(&tx_sw_v, plget->pkt_num);
			stats_reserve(&tx_hw_v, plget->pkt_num);
		}

		ts_flags |= SOF_TIMESTAMPING_TX_SOFTWARE;
		ts_flags |= SOF_TIMESTAMPING_TX_HARDWARE;
		if (plget->flags & PLF_TS_ID_ALLOWED) {
			ts_flags |= SOF_TIMESTAMPING_OPT_ID;
			ts_flags |= SOF_TIMESTAMPING_OPT_TSONLY;
		}

		if (plget->flags & PLF_SCHED_STAT) {
			ts_flags |= SOF_TIMESTAMPING_TX_SCHED;
			tx_sch_v = malloc(plget->dev_deep * sizeof(*tx_sch_v));
			for (i = 0; i < plget->dev_deep; i++)
				stats_reserve(&tx_sch_v[i], plget->pkt_num);
		}
	}

	if (mod == RTT_MOD || mod == ECHO_LAT || mod == RX_LAT ||
	    mod == RX_RATE) {
		if (plget->flags & PLF_PRINTOUT) {
			stats_reserve(&rx_app_v, plget->pkt_num);
			stats_reserve(&rx_sw_v, plget->pkt_num);
			stats_reserve(&rx_hw_v, plget->pkt_num);
		}

		ts_flags |= SOF_TIMESTAMPING_RX_SOFTWARE;
	}

	ts_flags |= SOF_TIMESTAMPING_RAW_HARDWARE;

	/* create and fill in packet */
	if (mod == RTT_MOD || mod == TX_LAT || mod == PKT_GEN) {
		ret = plget_create_packet(plget);
		if (ret)
			return ret;
	} else if (mod == ECHO_LAT) {
		plget->pkt = plget->data;
	}

	/* for simplicity and speed */
	fill_in_data_pointers(plget);

	ret = setup_sock_ts(plget->sfd, ts_flags);
	return ret;
}

int main(int argc, char **argv)
{
	struct plgett *plget;
	int ret;

	if (argc == 1) {
		res_print_time();
		exit(0);
	}

	plget = calloc(1, sizeof(struct plgett));
	if (!plget)
		return -ENOMEM;

	plget_args(plget, argc, argv);

	ret = init_test(plget);
	if (ret)
		return ret;

	/* lock current and future pages */
	if (mlockall(MCL_CURRENT | MCL_FUTURE))
		perror("mlockall failed");

	switch (plget->mod) {
	case RX_LAT:
		ret = rxlat(plget);
		break;
	case TX_LAT:
		ret = txlat(plget);
		break;
	case RTT_MOD:
		ret = rtt(plget);
		break;
	case ECHO_LAT:
		ret = echolat(plget);
		break;
	case PKT_GEN:
		ret = pktgen(plget);
		break;
	case RX_RATE:
		ret = rxrate(plget);
		break;
	default:
		plget_fail("didn't set mode with -m");
		break;
	}

	if (ret)
		return ret;

	res_stats_print(plget);
	exit(0);
}
