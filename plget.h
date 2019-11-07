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
#include <string.h>
#include <sys/time.h>
#include "stat.h"

#ifndef XDP_RX_RING
#include "linux/if_xdp.h"
#endif

#define ts_correct(ts)			((ts)->tv_nsec || (ts)->tv_sec)
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

extern struct plgett *plget;

#define BIT(X)				(1 << (X))
#define PLF_TITLE			BIT(0)
#define PLF_PTP				BIT(1)
#define PLF_AVTP			BIT(2)
#define PLF_PLAIN_FORMAT		BIT(3)
#define PLF_HW_STAT			BIT(4)
#define PLF_IPGAP_STAT			BIT(5)
#define PLF_LATENCY_STAT		BIT(6)
#define PLF_PRIO			BIT(7)
#define PLF_BUSYPOLL			BIT(8)
#define PLF_SCHED_STAT			BIT(9)
#define PLF_QUEUE			BIT(10)
#define PLF_ZERO_COPY			BIT(11)
#define PLF_DIS_HW_TS			BIT(12)
#define PLF_ADDR_SET			BIT(13)
#define PLF_PROG_LOADED			BIT(14)
#define PLF_RT_PRINT			BIT(15)
#define PLF_SW_POLL			BIT(16)
#define PLF_RTIME			BIT(17)
#define PLF_STRICT_ID_ORDER		BIT(18)

#define PLF_PRINTOUT			(PLF_HW_STAT |\
					PLF_IPGAP_STAT |\
					PLF_LATENCY_STAT |\
					PLF_SCHED_STAT)

#define CONTROL_LEN			512

enum pkt_type {
	PKT_UDP = 1,
	PKT_ETH,
	PKT_XDP,
	PKT_RAW,
};

enum test_mod {
	RX_LAT = 1,
	TX_LAT = 2,
	RTT_MOD = 3, 	/* round trip time mode (external latency) */
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
	struct ether_addr macaddr;
	struct ether_addr if_addr;
	struct sockaddr_ll sk_addr;
	enum test_mod mod;
	struct timespec interval;
	struct timespec rtime;
	char if_name[IFNAMSIZ];
	int ifidx;
	int phc_idx;
	enum pkt_type pkt_type;
	int frame_size;
	int pkt_num;
	int sk_payload_size;	/* socket payload size */
	int sfd;
	int port;
	int flags;
	int prio;
	int queue;		/* must be used by XDP socket */
	int busypoll_time;
	int stream_id;
	int dev_deep;
	int timer_fd;
	struct xsock *xsk;	/* xdp soket info */

	/* rt print */
	unsigned long icnt; /* current iteration for progress bar */
	unsigned long inum; /* number of iterations for progress bar */

	/* packet related info */
	char *pkt;
	char *rx_pkt;
	int off_sid_wr;		/* PTP sequential id */
	int off_tid_wr;		/* wr offset for ts id for identification */
	int off_tid_rd;		/* rd offset for ts id for identification */
	int off_magic_rd;	/* rd offset for magic num for validation */

	/* rx packet related info */
	char data[ETH_DATA_LEN];
	char control[CONTROL_LEN];
	struct iovec iov;
	struct msghdr msg;
	int off_tid_rx_rd;	/* rx rd offset for ts id for identification */
	int off_magic_rx_rd;	/* rx rd offset magic num for validation */
};

int setup_sock(int sfd, int flags);

int plget_create_timer(void);
int plget_start_timer(void);
void plget_stop_timer(void);

struct stats *plget_best_rx_vect(void);
struct stats *plget_best_tx_vect(void);

static inline char *magic_rx_rd(void)
{
	return (char *)(plget->rx_pkt + plget->off_magic_rx_rd);
}

static inline char *magic_rd(void)
{
	return (char *)(plget->data + plget->off_magic_rd);
}

static inline void tid_wr(__u32 tid)
{
	char *p;

	p = (char *)(plget->off_tid_wr + plget->pkt);
	tid = htonl(tid);
	memcpy(p, &tid, sizeof(tid));
}

static inline __u32 tid_rd(void)
{
	__u32 tid;
	char *p;

	p = (char *)(plget->data + plget->off_tid_rd);

	memcpy(&tid, p, sizeof(tid));
	tid = ntohl(tid);

	return tid;
}

static inline __u32 tid_rx_rd(void)
{
	__u32 tid;
	char *p;

	p = (char *)(plget->rx_pkt + plget->off_tid_rx_rd);

	memcpy(&tid, p, sizeof(tid));
	tid = ntohl(tid);

	return tid;
}

static inline void sid_wr(__u16 sid)
{
	char *p1, *p2;

	p1 = (char *)&sid;
	p2 = plget->off_sid_wr + plget->pkt;

	*p2++ = *p1++;
	*p2 = *p1;
}

#endif
