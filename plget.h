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

#ifndef PLGET_H
#define PLGET_H

#include <netpacket/packet.h>
#include <net/ethernet.h>
#include <netinet/in.h>
#include <net/if.h>
#include <sys/time.h>
#include "stat.h"

#ifndef XDP_RX_RING
#include "linux/if_xdp.h"
#endif

#define ts_correct(ts)			((ts)->tv_sec || (ts)->tv_nsec)
#define NSEC_PER_SEC			1000000000ULL
#define USEC_PER_SEC			1000000ULL
#define MAGIC				0x34
#define SEQ_ID_MASK			0x3fff
#define STREAM_ID_SHIFT			14

extern struct stats tx_app_v;
extern struct stats *tx_sch_v;
extern struct stats tx_sw_v;
extern struct stats tx_hw_v;
extern struct stats rx_app_v;
extern struct stats rx_sw_v;
extern struct stats rx_hw_v;

extern struct stats temp;

#define BIT(X)				(1 << (X))
#define PLF_TITLE			BIT(0)
#define PLF_PTP				BIT(2)
#define PLF_AVTP			BIT(3)
#define PLF_PLAIN_FORMAT		BIT(4)
#define PLF_HW_STAT			BIT(5)
#define PLF_HW_GAP_STAT			BIT(6)
#define PLF_LATENCY_STAT		BIT(7)
#define PLF_PRIO			BIT(8)
#define PLF_BUSYPOLL			BIT(9)
#define PLF_TS_ID_ALLOWED		BIT(10)
#define PLF_SCHED_STAT			BIT(11)
#define PLF_QUEUE			BIT(12)
#define PLF_ZERO_COPY			BIT(13)

#define PLF_PRINTOUT			(PLF_HW_STAT |\
					PLF_HW_GAP_STAT |\
					PLF_LATENCY_STAT |\
					PLF_SCHED_STAT)

#define CONTROL_LEN			512

enum pkt_type {
	PKT_UDP,
	PKT_ETH,
	PKT_XDP_ETH,
};

enum test_mod {
	RX_LAT = 1,
	TX_LAT = 2,
	EXT_LAT = 3,
	ECHO_LAT = 4,
	PKT_GEN = 5,
	RX_RATE = 6,
};

struct plgett {
	union {
		struct in_addr iaddr;
		struct sockaddr_ll iaddr2;
		struct sockaddr_xdp iaddr3;
	};
	unsigned char macaddr[8];
	struct sockaddr_ll sk_addr;
	enum test_mod mod;
	struct timespec interval;
	struct timespec rtime;
	char if_name[IFNAMSIZ];
	enum pkt_type pkt_type;
	int pkt_size;
	int pkt_num;
	int payload_size;
	int sfd;
	int port;
	int flags;
	int prio;
	int queue; /* must be used by XDP socket */
	int busypoll_time;
	int stream_id;
	int dev_deep;

	/* packet related info */
	char *packet;
	__u16 *seq_id_wr;
	unsigned int *packet_id_wr;
	char *magic_rd;

	/* rx packet related info */
	char data[ETH_DATA_LEN];
	char control[CONTROL_LEN];
	struct iovec iov;
	struct msghdr msg;
};

int setup_sock(int sfd, int flags);
int plget_setup_timer(struct plgett *plget);
struct stats *plget_best_rx_vect(void);
struct stats *plget_best_tx_vect(void);

#endif
