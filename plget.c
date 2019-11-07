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
#include "xdp_prog_load.h"
#include <pthread.h>
#include "rtprint.h"
#include <linux/ethtool.h>

#define ALIGN_ROUNDUP(x, align)\
	((align) * (((x) + align - 1) / (align)))

#define ALIGN(x) ALIGN_ROUNDUP(x, sizeof(long))

#define ALIGN_ROUNDUP_PTR(x, align)\
	((void *)ALIGN_ROUNDUP((uintptr_t)(x), (uintptr_t)(align)))

#define OFF_PTP_SEQUENCE_ID		30
#define UDP_HLEN	42

struct plgett *plget;

struct stats tx_app_v;
struct stats *tx_sch_v;
struct stats tx_sw_v;
struct stats tx_hw_v;
struct stats rx_app_v;
struct stats rx_sw_v;
struct stats rx_hw_v;

struct stats temp;

static unsigned char ptpv2_sync_pkt[] = {
	0x10, 0x02, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x80, 0x63, 0xff, 0xff, 0x00, 0x09, 0xba, 0x00, 0x01,
	0x00, 0x74, 0x00, 0x00
};

#define PTP_HSIZE	sizeof(ptpv2_sync_pkt)

int plget_create_timer(void)
{
	plget->timer_fd = timerfd_create(CLOCK_MONOTONIC, 0);
	if (plget->timer_fd < 0)
		return perror("Couldn't create timer"), -1;

	return 0;
}

int plget_start_timer(void)
{
	struct itimerspec tspec = { 0 };
	int ret;

	tspec.it_value.tv_sec = 0;
	tspec.it_value.tv_nsec = 100000;

	tspec.it_interval.tv_sec = plget->interval.tv_sec;
	tspec.it_interval.tv_nsec = plget->interval.tv_nsec;

	ret = timerfd_settime(plget->timer_fd, 0, &tspec, NULL);
	if (ret < 0) {
		perror("Couldn't set timer");
		return -1;
	}

	return 0;
}

void plget_stop_timer(void)
{
	struct itimerspec tspec = { 0 };

	timerfd_settime(plget->timer_fd, 0, &tspec, NULL);
}

static int setup_sock_ts(int sfd, int flags)
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

static int get_timestamp_info(struct ethtool_ts_info *info)
{
	struct ifreq ifr;
	int ret;

	memset(&ifr, 0, sizeof(ifr));
	memset(info, 0, sizeof(struct ethtool_ts_info));

	strncpy(ifr.ifr_name, plget->if_name, sizeof(ifr.ifr_name));
	ifr.ifr_data = (char *)info;

	info->cmd = ETHTOOL_GET_TS_INFO;

	ret = ioctl(plget->sfd, SIOCETHTOOL, &ifr);
	if (ret < 0) {
		printf("SIOCETHTOOL failed");
		return -1;
	}

	return 0;
}

static void print_timestamp_info(struct ethtool_ts_info *info)
{
	printf("\n\"%s\" timestamp capabilities: \n", plget->if_name);
	printf("RX filters: \n");

	if (info->rx_filters & BIT(HWTSTAMP_FILTER_NONE))
		printf("\tHWTSTAMP_FILTER_NONE\n");
	if (info->rx_filters & BIT(HWTSTAMP_FILTER_ALL))
		printf("\tHWTSTAMP_FILTER_ALL\n");
	if (info->rx_filters & BIT(HWTSTAMP_FILTER_SOME))
		printf("\tHWTSTAMP_FILTER_SOME\n");
	if (info->rx_filters & BIT(HWTSTAMP_FILTER_PTP_V1_L4_EVENT))
		printf("\tHWTSTAMP_FILTER_PTP_V1_L4_EVENT\n");
	if (info->rx_filters & BIT(HWTSTAMP_FILTER_PTP_V1_L4_SYNC))
		printf("\tHWTSTAMP_FILTER_PTP_V1_L4_SYNC\n");
	if (info->rx_filters & BIT(HWTSTAMP_FILTER_PTP_V1_L4_DELAY_REQ))
		printf("\tHWTSTAMP_FILTER_PTP_V1_L4_DELAY_REQ\n");
	if (info->rx_filters & BIT(HWTSTAMP_FILTER_PTP_V2_L4_EVENT))
		printf("\tHWTSTAMP_FILTER_PTP_V2_L4_EVENT\n");
	if (info->rx_filters & BIT(HWTSTAMP_FILTER_PTP_V2_L4_SYNC))
		printf("\tHWTSTAMP_FILTER_PTP_V2_L4_SYNC\n");
	if (info->rx_filters & BIT(HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ))
		printf("\tHWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ\n");
	if (info->rx_filters & BIT(HWTSTAMP_FILTER_PTP_V2_L2_EVENT))
		printf("\tHWTSTAMP_FILTER_PTP_V2_L2_EVENT\n");
	if (info->rx_filters & BIT(HWTSTAMP_FILTER_PTP_V2_L2_SYNC))
		printf("\tHWTSTAMP_FILTER_PTP_V2_L2_SYNC\n");
	if (info->rx_filters & BIT(HWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ))
		printf("\tHWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ\n");
	if (info->rx_filters & BIT(HWTSTAMP_FILTER_PTP_V2_EVENT))
		printf("\tHWTSTAMP_FILTER_PTP_V2_EVENT\n");
	if (info->rx_filters & BIT(HWTSTAMP_FILTER_PTP_V2_SYNC))
		printf("\tHWTSTAMP_FILTER_PTP_V2_SYNC\n");
	if (info->rx_filters & BIT(HWTSTAMP_FILTER_PTP_V2_DELAY_REQ))
		printf("\tHWTSTAMP_FILTER_PTP_V2_DELAY_REQ\n");
	if (info->rx_filters & BIT(HWTSTAMP_FILTER_NTP_ALL))
		printf("\tHWTSTAMP_FILTER_NTP_ALL\n");

	printf("TX types: \n");

	if (info->tx_types & BIT(HWTSTAMP_TX_OFF))
		printf("\tHWTSTAMP_TX_OFF\n");
	if (info->tx_types & BIT(HWTSTAMP_TX_ON))
		printf("\tHWTSTAMP_TX_ON\n");
	if (info->tx_types & BIT(HWTSTAMP_TX_ONESTEP_SYNC))
		printf("\tHWTSTAMP_TX_ONESTEP_SYNC\n");

	printf("TS cofigurations: \n");

	if (info->so_timestamping & SOF_TIMESTAMPING_TX_HARDWARE)
		printf("\tSOF_TIMESTAMPING_TX_HARDWARE\n");
	if (info->so_timestamping & SOF_TIMESTAMPING_TX_SOFTWARE)
		printf("\tSOF_TIMESTAMPING_TX_SOFTWARE\n");
	if (info->so_timestamping & SOF_TIMESTAMPING_RX_HARDWARE)
		printf("\tSOF_TIMESTAMPING_RX_HARDWARE\n");
	if (info->so_timestamping & SOF_TIMESTAMPING_RX_SOFTWARE)
		printf("\tSOF_TIMESTAMPING_RX_SOFTWARE\n");
	if (info->so_timestamping & SOF_TIMESTAMPING_SOFTWARE)
		printf("\tSOF_TIMESTAMPING_SOFTWARE\n");
	if (info->so_timestamping & SOF_TIMESTAMPING_RAW_HARDWARE)
		printf("\tSOF_TIMESTAMPING_RAW_HARDWARE\n");

	printf("\t\n");
}

static void print_hwts_configuration(struct hwtstamp_config *hwcnf, char *sfx)
{
	printf("\n\"%s\" HWTS configuration %s: \n", plget->if_name,
	       sfx);

	printf("TX type: \n");

	if (hwcnf->tx_type == HWTSTAMP_TX_OFF)
		printf("\tHWTSTAMP_TX_OFF\n");
	if (hwcnf->tx_type == HWTSTAMP_TX_ON)
		printf("\tHWTSTAMP_TX_ON\n");
	if (hwcnf->tx_type == HWTSTAMP_TX_ONESTEP_SYNC)
		printf("\tHWTSTAMP_TX_ONESTEP_SYNC\n");

	printf("RX filter: \n");
	if (hwcnf->rx_filter == HWTSTAMP_FILTER_NONE)
		printf("\tHWTSTAMP_FILTER_NONE\n");
	if (hwcnf->rx_filter == HWTSTAMP_FILTER_ALL)
		printf("\tHWTSTAMP_FILTER_ALL\n");
	if (hwcnf->rx_filter == HWTSTAMP_FILTER_SOME)
		printf("\tHWTSTAMP_FILTER_SOME\n");
	if (hwcnf->rx_filter == HWTSTAMP_FILTER_PTP_V1_L4_EVENT)
		printf("\tHWTSTAMP_FILTER_PTP_V1_L4_EVENT\n");
	if (hwcnf->rx_filter == HWTSTAMP_FILTER_PTP_V1_L4_SYNC)
		printf("\tHWTSTAMP_FILTER_PTP_V1_L4_SYNC\n");
	if (hwcnf->rx_filter == HWTSTAMP_FILTER_PTP_V1_L4_DELAY_REQ)
		printf("\tHWTSTAMP_FILTER_PTP_V1_L4_DELAY_REQ\n");
	if (hwcnf->rx_filter == HWTSTAMP_FILTER_PTP_V2_L4_EVENT)
		printf("\tHWTSTAMP_FILTER_PTP_V2_L4_EVENT\n");
	if (hwcnf->rx_filter == HWTSTAMP_FILTER_PTP_V2_L4_SYNC)
		printf("\tHWTSTAMP_FILTER_PTP_V2_L4_SYNC\n");
	if (hwcnf->rx_filter == HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ)
		printf("\tHWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ\n");
	if (hwcnf->rx_filter == HWTSTAMP_FILTER_PTP_V2_L2_EVENT)
		printf("\tHWTSTAMP_FILTER_PTP_V2_L2_EVENT\n");
	if (hwcnf->rx_filter == HWTSTAMP_FILTER_PTP_V2_L2_SYNC)
		printf("\tHWTSTAMP_FILTER_PTP_V2_L2_SYNC\n");
	if (hwcnf->rx_filter == HWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ)
		printf("\tHWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ\n");
	if (hwcnf->rx_filter == HWTSTAMP_FILTER_PTP_V2_EVENT)
		printf("\tHWTSTAMP_FILTER_PTP_V2_EVENT\n");
	if (hwcnf->rx_filter == HWTSTAMP_FILTER_PTP_V2_SYNC)
		printf("\tHWTSTAMP_FILTER_PTP_V2_SYNC\n");
	if (hwcnf->rx_filter == HWTSTAMP_FILTER_PTP_V2_DELAY_REQ)
		printf("\tHWTSTAMP_FILTER_PTP_V2_DELAY_REQ\n");
	if (hwcnf->rx_filter == HWTSTAMP_FILTER_NTP_ALL)
		printf("\tHWTSTAMP_FILTER_NTP_ALL\n");

	printf("\t\n");
}

void get_inf_addr(void)
{
	int type = plget->pkt_type;
	struct ifreq ifr;
	int ok;

	ok = type == PKT_ETH || type == PKT_RAW || type == PKT_XDP;
	if (!ok)
		return;

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, plget->if_name, sizeof(ifr.ifr_name));

	if (ioctl(plget->sfd, SIOCGIFHWADDR, &ifr))
		plget_fail("cann't get interface hw address");

	plget->if_addr = *((struct ether_addr *)ifr.ifr_hwaddr.sa_data);
}

static void get_inf_info(void)
{
	struct ethtool_ts_info info;
	int ret;

	get_inf_addr();

	/* timestamping capabilities */
	ret = get_timestamp_info(&info);
	if (!ret) {
		plget->phc_idx = info.phc_index;
		print_timestamp_info(&info);
	}

	res_title_print();
}

static int enable_hw_timestamping(void)
{
	struct hwtstamp_config hwconfig_requested;
	struct hwtstamp_config hwconfig;
	int need_tx_hwts, need_rx_hwts;
	struct ifreq ifreq_ts;
	int ret;


	/* current timestamping configuration */
	memset(&ifreq_ts, 0, sizeof(ifreq_ts));
	memset(&hwconfig, 0, sizeof(hwconfig));
	strncpy(ifreq_ts.ifr_name, plget->if_name, sizeof(ifreq_ts.ifr_name));
	ifreq_ts.ifr_data = (void *)&hwconfig;
	ret = ioctl(plget->sfd, SIOCGHWTSTAMP, &ifreq_ts);
	print_hwts_configuration(&hwconfig, "before");

	/* configure timestamping */
	memset(&ifreq_ts, 0, sizeof(ifreq_ts));
	memset(&hwconfig, 0, sizeof(hwconfig));
	strncpy(ifreq_ts.ifr_name, plget->if_name, sizeof(ifreq_ts.ifr_name));
	ifreq_ts.ifr_data = (void *)&hwconfig;

	if (plget->flags & PLF_DIS_HW_TS) {
		need_tx_hwts = 0;
		need_rx_hwts = 0;
	} else {
		need_tx_hwts = plget->mod == TX_LAT ||
			       plget->mod == RTT_MOD ||
			       plget->mod == ECHO_LAT;
		need_rx_hwts = plget->mod == RX_LAT ||
			       plget->mod == RTT_MOD ||
			       plget->mod == RX_RATE ||
			       plget->mod == ECHO_LAT;
	}

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

	print_hwts_configuration(&hwconfig, "after");

	return ret;
}

static int udp_socket(void)
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
	mreq.imr_ifindex = plget->ifidx;
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

static void specify_protocol(__u16 *protocol)
{
	if (plget->flags & PLF_AVTP)
		*protocol = htons(ETH_P_TSN);
	else if (plget->flags & PLF_PTP)
		*protocol = htons(ETH_P_1588);
	else
		*protocol = 0;
}

static int mac_is_mcast(__u8 *mac)
{
	return 0x01 & mac[0];
}

static int plget_mcast(int sfd)
{
	__u8 *mac = (__u8 *)&plget->macaddr;
	struct packet_mreq mreq;
	int ret;

	if (plget->mod == TX_LAT || plget->mod == PKT_GEN)
		return 0;

	 /* join multicast group if mcast address is provided */
	if (!(plget->flags & PLF_ADDR_SET))
		return 0;

	if (!mac_is_mcast(mac))
		return 0;

	mreq.mr_ifindex = plget->ifidx;
	mreq.mr_type = PACKET_MR_MULTICAST;
	mreq.mr_alen = ETH_ALEN;
	memcpy(&mreq.mr_address, mac, ETH_ALEN);

	ret = setsockopt(sfd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mreq,
			 sizeof(struct packet_mreq));
	if (ret)
		return perror("Cannot set PACKET_MEMBERSHIP"), -errno;

	printf("joined mcast group: %02x:%02x:%02x:%02x:%02x:%02x\n",
	       mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

	return 0;
}

static int packet_socket(void)
{
	struct sockaddr_ll *addr = &plget->sk_addr;
	__u16 protocol;
	int sfd, ret;

	specify_protocol(&protocol);

	if (plget->pkt_type == PKT_RAW) {
		protocol = htons(ETH_P_ALL);
		sfd = socket(AF_PACKET, SOCK_RAW, protocol);
	} else {
		sfd = socket(AF_PACKET, SOCK_DGRAM, protocol);
	}

	if (sfd < 0)
		return perror("socket"), -errno;

	addr->sll_family = AF_PACKET;
	addr->sll_protocol = protocol;

	/* If user provided a network interface, bind() to it. */
	if (plget->if_name[0] != '\0')
		addr->sll_ifindex = plget->ifidx;

	ret = bind(sfd, (struct sockaddr *)addr, sizeof(struct sockaddr_ll));
	if (ret < 0)
		return perror("Couldn't bind() to interface"), -errno;

	addr->sll_halen = ETH_ALEN;
	memcpy(addr->sll_addr, (__u8 *)&plget->macaddr, ETH_ALEN);

	if (plget_mcast(sfd))
		return -errno;

	return sfd;
}


static int plget_more_sock_options(void)
{
	int sfd = plget->sfd;
	int ret;

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

	return 0;
}

static int plget_create_socket(void)
{
	if (plget->pkt_type == PKT_UDP)
		plget->sfd = udp_socket();
	else if (plget->pkt_type == PKT_ETH || plget->pkt_type == PKT_RAW)
		plget->sfd = packet_socket();
	else if (plget->pkt_type == PKT_XDP)
		plget->sfd = xdp_socket();
	else
		plget_fail("uknown packet type");

	if (plget->sfd < 0)
		return 1;

	/* set more socket options */
	if (plget_more_sock_options())
		return -errno;

	return 0;
}

static void init_pkt_ether_header(void)
{
	struct ether_header *eth = (struct ether_header *)plget->pkt;
	struct ether_addr *dst_addr, *src_addr;

	dst_addr = (struct ether_addr *)eth->ether_dhost;
	src_addr = (struct ether_addr *)eth->ether_shost;

	*dst_addr = plget->macaddr;
	*src_addr = plget->if_addr;

	specify_protocol(&eth->ether_type);
}

static void fill_in_packets(void)
{
	int ptp_payload_size;
	int n, i, j;
	char *dp;

	ptp_payload_size = plget->sk_payload_size;
	if (plget->pkt_type == PKT_XDP || plget->pkt_type == PKT_RAW)
		ptp_payload_size -= ETH_HLEN;

	if (plget->flags & PLF_PTP)
		ptp_payload_size -= PTP_HSIZE;

	n = (plget->pkt_type == PKT_XDP) ? FRAME_NUM : 1;
	for (i = 0; i < n; i++) {
		if (plget->pkt_type == PKT_XDP) {
			j = FRAME_SIZE * i;
			plget->pkt = &plget->xsk->umem->frames[j];
		}

		if (plget->pkt_type == PKT_RAW || plget->pkt_type == PKT_XDP) {
			init_pkt_ether_header();
			dp = plget->pkt + ETH_HLEN;
		} else {
			dp = plget->pkt;
		}

		if (plget->flags & PLF_PTP) {
			memcpy(dp, ptpv2_sync_pkt, PTP_HSIZE);
			dp += PTP_HSIZE;
		}

		*dp++ = MAGIC;

		for (j = 0; j < ptp_payload_size; j++)
			*dp++ = (rand() % 230) + 1;
	}

	if (plget->pkt_type == PKT_XDP)
		plget->pkt = plget->xsk->umem->frames;
}

static int plget_create_packet(void)
{
	int payload_size;

	/* check settings */
	if (plget->frame_size &&
	    (plget->frame_size < 64 || plget->frame_size > ETH_DATA_LEN)) {
			printf("incorrect packet size: 64 <= size <= 1500\n");
			return -EINVAL;
	}

	/* adjust size and payload for packet */
	if (plget->pkt_type == PKT_UDP) {
		if (plget->flags & PLF_PTP) {
			if (plget->frame_size &&
			    plget->frame_size < 100) {
				printf("packet size should be > 99\n");
				return -EINVAL;
			} else if (!plget->frame_size)
				plget->frame_size = 100;
		} else if (!plget->frame_size)
			plget->frame_size = 66;

		payload_size = plget->frame_size - UDP_HLEN;
	} else {
		if (plget->flags & PLF_PTP) {
			if (plget->frame_size &&
			    plget->frame_size < 72) {
				printf("packet size should be > 71\n");
				return -EINVAL;
			} else if (!plget->frame_size)
				plget->frame_size = 72;
		} else if (!plget->frame_size)
			plget->frame_size = 64;

		payload_size = plget->frame_size;
		if (plget->pkt_type != PKT_XDP && plget->pkt_type != PKT_RAW)
			payload_size -= ETH_HLEN;
	}

	/* allocate packet */
	plget->sk_payload_size = payload_size;
	if (plget->pkt_type != PKT_XDP) {
		plget->pkt = malloc(payload_size);
		if (!plget->pkt)
			return -ENOMEM;
	}

	fill_in_packets();
	return 0;
}

static void fill_in_data_pointers(void)
{
	int off = 0;

	plget->iov.iov_base = plget->data;
	plget->iov.iov_len = sizeof(plget->data);
	plget->msg.msg_iov = &plget->iov;
	plget->msg.msg_iovlen = 1;
	plget->msg.msg_control = plget->control;
	plget->msg.msg_controllen = sizeof(plget->control);

	plget->rx_pkt = plget->data;

	if (plget->pkt_type == PKT_XDP || plget->pkt_type == PKT_RAW)
		off += ETH_HLEN;

	if (plget->flags & PLF_PTP)
		plget->off_sid_wr = off + OFF_PTP_SEQUENCE_ID;

	if (plget->flags & PLF_PTP)
		off += PTP_HSIZE;

	if (plget->mod != ECHO_LAT)
		plget->off_tid_wr = off + 1;

	plget->off_magic_rd = off;
	plget->off_tid_rd = off + 1;

	plget->off_magic_rx_rd = off;
	plget->off_tid_rx_rd = off + 1;

	/* add sent_payload - sk_payload */
	if (plget->pkt_type == PKT_ETH) {
		plget->off_magic_rd += ETH_HLEN;
		plget->off_tid_rd += ETH_HLEN;
	} else if (plget->pkt_type == PKT_UDP) {
		plget->off_magic_rd += UDP_HLEN;
		plget->off_tid_rd += UDP_HLEN;
	}
}

static int init_test(void)
{
	int ts_flags = SOF_TIMESTAMPING_SOFTWARE;
	int i, ret, mod = plget->mod;

	ret = plget_create_socket();
	if (ret)
		return ret;

	get_inf_info();

	/* if requested some info only and mode is not set then no need to
	 * continue
	 */
	if (!plget->mod)
		return 0;

	enable_hw_timestamping();

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
		ret = plget_create_packet();
		if (ret)
			return ret;
	}

	/* for simplicity and speed */
	fill_in_data_pointers();

	ret = setup_sock_ts(plget->sfd, ts_flags);
	return ret;
}

int main(int argc, char **argv)
{
	int ret;
	pthread_t rt_thd;

	if (argc == 1) {
		res_print_time();
		exit(0);
	}

	plget = calloc(1, sizeof(struct plgett));
	if (!plget)
		return -ENOMEM;

	plget_args(argc, argv);

	ret = init_test();
	if (ret)
		return ret;

	if (!plget->mod)
		return 0;

	/* lock current and future pages */
	if (mlockall(MCL_CURRENT | MCL_FUTURE))
		perror("mlockall failed");

	if (plget->flags & PLF_RT_PRINT)
		ret = pthread_create(&rt_thd, NULL, rtprint, NULL);

	switch (plget->mod) {
	case RX_LAT:
		ret = rxlat();
		break;
	case TX_LAT:
		ret = txlat();
		break;
	case RTT_MOD:
		ret = rtt();
		break;
	case ECHO_LAT:
		ret = echolat();
		break;
	case PKT_GEN:
		ret = pktgen();
		break;
	case RX_RATE:
		ret = rxrate();
		break;
	default:
		plget_fail("provide mode with -m");
		break;
	}

	xdp_unload_prog();

	if (plget->flags & PLF_RT_PRINT) {
		plget->icnt = plget->inum;
		pthread_join(rt_thd, NULL);
	}

	res_stats_print();
	free(plget);

	if (ret)
		printf("exit with error\n");

	exit(ret);
}
