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

#include "plget_args.h"
#include <getopt.h>
#include <stdlib.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <string.h>

#define PLGET_NAME_VER			"plget v0.4"
#define PTP_EVENT_PORT			319
#define PTP_GENERAL_PORT		320
#define PTP_PRIMARY_MCAST_IPADDR	"224.0.1.129"
#define PTP_PRIMARY_MCAST_MACADDR	"01:1B:19:00:00:00"
#define PTP_FILTERED_MCAST_MACADDR	"01:80:C2:00:00:0E"

static void plget_usage(void)
{
	printf("%s\n", PLGET_NAME_VER);
	printf("Usage:\n");
	printf("plget <options>:\n");
	printf("\ts PPS \t\t--pps=PPS\t\t:packets per second, no need in "
	       "\"rx-lat\" and \"echo-lat\" modes\n");
	printf("\tu PORT\t\t--udp=PORT\t\t:udp port number for udp packet "
	       "type\n");
	printf("\t\t\t\t\t\tport 319 or 320 is special and address 224.0.1.29 "
	       "by default if not overwritten\n");
	printf("\tt TYPE\t\t--type=TYPE\t\t:type of packet, can be udp, avtp, "
	       "ptpl2, ptpl4, xdp_ptpl2\n");
	printf("\ti NAME\t\t--if=NAME\t\t:interface name\n");
	printf("\tm MODE\t\t--mode=MODE\t\t:\"rx-lat\" or \"tx-lat\" or "
	       "\"echo-lat\" or \"pkt-gen\" or \"rtt\" or \"rx-rate\" "
	       "mode\n");
	printf("\tn NUM\t\t--pkt-num=NUM\t\t:number of packets to be sent or "
	       "received\n");
	printf("\tl SIZE\t\t--pkt-size=SIZE\t\t:packet size in bytes\n");
	printf("\ta IPADDR\t--address=IPADDR\t:ip address in send mode\n");
	printf("\tc \t\t--clock-check\t\t:print title showing clocks involved "
	       "in test, no arguments\n");
	printf("\tf PRINTLINE\t--format=PRINTLINE\t:printout conf, \"hwts\" "
	       "\"ipgap\" \"plain\" \"lat\", by default \"lat\" is used\n");
	printf("\t\t\t\t\t\t\"hwts\" - print hw timesamps relatively to first "
	       "packet hw tx time\n");
	printf("\t\t\t\t\t\t\"ipgap\" - print interpacket gap, that is time "
	       "between packets, sent or received\n");
	printf("\t\t\t\t\t\t\"plain\" - format output in one row and with "
	       "space delimiter\n");
	printf("\t\t\t\t\t\t\"lat\" - print latencies\n");
	printf("\t\t\t\t\t\t\"sched\" - print latencies before packet "
	       "scheduler\n");
	printf("\tp PRIO\t\t--prio=PRIO\t\t:set priority for socket\n");
	printf("\tw TIME\t\t--busy-poll=TIME\t:set SO_BUSY_POLL option for "
	       "socket, in us\n");
	printf("\tr TIME\t\t--rel-time=TIME\t\t:use relative time for \"hwts\" "
	       "output instead of first packet timestamp, in ns\n");
	printf("\tk ID\t\t--stream-id=ID\t\t:set stream num to identify PTP "
	       "stream (with seq_id) to differ on h/w level\n");
	printf("\td NUM\t\t--dev-deep=NUM\t\t:number of devices on tx path, "
	       "used only for \"sched\" format and vlan or/and bridge is used, "
	       "by default 1 is used. That is if vlan + bridge then -d 3, and "
	       "so on, basically it's equal to number of sched timestamps "
	       "expected\n");
	printf("\tq QUEUE\t\t--queue=QUEUE\t\t:set queue for xpd socket\n");
	printf("\tz \t\t--zero-copy\t\t:force zero-copy XDP mode\n");
}

static struct option plget_options[] = {
	{"pps",		required_argument,	0, 's'},
	{"udp",		required_argument,	0, 'u'},
	{"type",	required_argument,	0, 't'},
	{"if",		required_argument,	0, 'i'},
	{"mode",	required_argument,	0, 'm'},
	{"pkt-num",	required_argument,	0, 'n'},
	{"pkt-size",	required_argument,	0, 'l'},
	{"address",	required_argument,	0, 'a'},
	{"clock-check",	no_argument,		0, 'c'},
	{"format",	required_argument,	0, 'f'},
	{"prio",	required_argument,	0, 'p'},
	{"busy-poll",	required_argument,	0, 'w'},
	{"rel-time",	required_argument,	0, 'r'},
	{"stream-id",	required_argument,	0, 'k'},
	{"dev-deep",	required_argument,	0, 'd'},
	{"queue",	required_argument,	0, 'q'},
	{"zero-copy",	no_argument,		0, 'z'},
	{NULL, 0, NULL, 0},
};

static void plget_check_args(struct plgett *plget)
{
	int mod = plget->mod;
	int need_addr;

	if (plget->flags & PLF_SCHED_STAT) {
		/* as always present some packet scheduler
		 * TODO: identify virtual interface and increase to 2
		 * TODO: identify a bridge and increase to 2 or 3 if vlan
		 */
		if (!plget->dev_deep)
			plget->dev_deep = 1;
	} else {
		plget->dev_deep = 0;
	}

	/* set default lat printout */
	if ((mod == TX_LAT || mod == RX_LAT || mod == ECHO_LAT ||
	     mod == RTT_MOD) && !(plget->flags & PLF_PRINTOUT))
		plget->flags |= PLF_LATENCY_STAT;

	if (mod == RX_RATE && plget->flags & PLF_PRINTOUT) {
		plget->flags &= ~PLF_PRINTOUT;
		printf("Latencies or timestamps cannot be printed in "
		       "this mode\n");
	}

	if (mod == RX_LAT && ts_correct(&plget->interval))
		plget_fail("pps cannot be set in rx-lat mode");

	if ((mod == RX_LAT || mod == RX_RATE) && plget->flags & PLF_PRIO)
		plget_fail("priority cannot be set int this mode");

	if ((mod == ECHO_LAT || mod == RX_LAT || mod == RX_RATE) &&
	    plget->pkt_size)
		plget_fail("packet size can't be set in this mode");

	/* check if packet type is set */
	switch (plget->pkt_type) {
	case PKT_UDP:
		if (!plget->port)
			plget_fail("Please, specify UDP port number");

		if (plget->port == PTP_EVENT_PORT ||
		    plget->port == PTP_GENERAL_PORT)
			plget->flags |= PLF_PTP;

		if (!plget->iaddr.s_addr && plget->flags & PLF_PTP) {
			inet_aton(PTP_PRIMARY_MCAST_IPADDR, &plget->iaddr);
			if (!(mod == RX_LAT && mod == RX_RATE))
				printf("Destination address is set to %s\n",
					PTP_PRIMARY_MCAST_IPADDR);
		}

		need_addr = (!plget->iaddr.s_addr) && (mod == TX_LAT ||
			    mod == ECHO_LAT || mod == RTT_MOD ||
			    mod == PKT_GEN);

		plget->flags |= PLF_TS_ID_ALLOWED;
		break;
	case PKT_ETH:
		if (plget->macaddr[0] == '\0' && (plget->flags & PLF_PTP)) {
			if (mod == TX_LAT || mod == PKT_GEN || mod == RTT_MOD ||
			    mod == ECHO_LAT) {
				sscanf(PTP_PRIMARY_MCAST_MACADDR,
				       "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
				       &plget->macaddr[0], &plget->macaddr[1],
				       &plget->macaddr[2], &plget->macaddr[3],
				       &plget->macaddr[4], &plget->macaddr[5]);

				printf("Destination address set to %s\n",
						PTP_PRIMARY_MCAST_MACADDR);
				printf("Defaults: %s or %s\n",
						PTP_PRIMARY_MCAST_MACADDR,
						PTP_FILTERED_MCAST_MACADDR);
			} else {
				printf("Default address can be specified: %s "
				       "or %s\n", PTP_PRIMARY_MCAST_MACADDR,
				       PTP_FILTERED_MCAST_MACADDR);
			}
		}

		need_addr = (plget->macaddr[0] == '\0') &&
			    (mod == TX_LAT || mod == RTT_MOD || mod == PKT_GEN);

		if (plget->port)
			printf("Cannot specify port for non UDP packets\n");
		break;
	case PKT_XDP:
		if (plget->if_name[0] == '\0')
			plget_fail("For XDP sockets, dev has to be specified");

		if (!(plget->flags & PLF_QUEUE))
			plget->queue = 0;

		need_addr = (plget->macaddr[0] == '\0') &&
			    (mod == TX_LAT || mod == RTT_MOD || mod == PKT_GEN);
		break;
	default:
		plget_fail("Please, specify packet_type");
	}

	if (need_addr)
		plget_fail("Please, specify the address with -a");
}

static void plget_set_pps(struct plgett *plget)
{
	struct timespec i;

	if (strchr(optarg, '.')) {
		double pps;

		sscanf(optarg, "%lf", &pps);

		i.tv_sec = 1 / pps;
		i.tv_nsec = i.tv_sec ? NSEC_PER_SEC * (1 / pps - i.tv_sec) :
			               NSEC_PER_SEC / pps;
	} else {
		int pps;

		pps = atoi(optarg);

		i.tv_sec = 1 / pps;
		i.tv_nsec = i.tv_sec ? 0: NSEC_PER_SEC / pps;
	}

	plget->interval = i;
}

static void plget_set_packet_type(struct plgett *plget)
{
	if (!strcmp("udp", optarg)) {
		plget->pkt_type = PKT_UDP;
	} else if (!strcmp("avtp", optarg)) {
		plget->pkt_type = PKT_ETH;
		plget->flags |= PLF_AVTP;
	} else if (!strcmp("ptpl4", optarg)) {
		plget->pkt_type = PKT_UDP;
		plget->port = PTP_EVENT_PORT;
	} else if (!strcmp("ptpl2", optarg)) {
		plget->pkt_type = PKT_ETH;
		plget->flags |= PLF_PTP;
	} else if (!strcmp("xdp_ptpl2", optarg)) {
		plget->pkt_type = PKT_XDP;
		plget->flags |= PLF_PTP;
	} else {
		plget_fail("unsupported packet type");
	}
}

static void plget_set_mode(struct plgett *plget)
{
	if (!strcmp("tx-lat", optarg))
		plget->mod = TX_LAT;
	else if (!strcmp("rx-lat", optarg))
		plget->mod = RX_LAT;
	else if (!strcmp("rtt", optarg))
		plget->mod = RTT_MOD;
	else if (!strcmp("echo-lat", optarg))
		plget->mod = ECHO_LAT;
	else if (!strcmp("pkt-gen", optarg))
		plget->mod = PKT_GEN;
	else if (!strcmp("rx-rate", optarg))
		plget->mod = RX_RATE;
	else
		plget_fail("unkown mode");
}

static void plget_set_address(struct plgett *plget)
{
	int ret;

	if (plget->pkt_type == PKT_UDP) {
		inet_aton(optarg, &plget->iaddr);
	} else if (plget->pkt_type == PKT_ETH ||
		   plget->pkt_type == PKT_XDP) {
		ret =
		sscanf(optarg, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
			&plget->macaddr[0], &plget->macaddr[1],
			&plget->macaddr[2], &plget->macaddr[3],
			&plget->macaddr[4], &plget->macaddr[5]);
		if (ret != 6) {
			printf("Invalid address\n");
			exit(EXIT_FAILURE);
		}
	} else if (!plget->pkt_type) {
		plget_fail("Set paket type first");
	} else {
		plget_fail("unkown pkt type");
	}
}


static void plget_set_output_format(struct plgett *plget)
{
	if (strstr(optarg, "hwts"))
		plget->flags |= PLF_HW_STAT;

	if (strstr(optarg, "ipgap"))
		plget->flags |= PLF_HW_GAP_STAT;

	if (strstr(optarg, "plain"))
		plget->flags |= PLF_PLAIN_FORMAT;

	if (strstr(optarg, "lat"))
		plget->flags |= PLF_LATENCY_STAT;

	if (strstr(optarg, "sched")) {
		plget->flags |= PLF_SCHED_STAT;
		plget->flags |= PLF_LATENCY_STAT;
	}
}

static void plget_set_relative_time(struct plgett *plget)
{
	__u64 ns;

	sscanf(optarg, "%llu", &ns);
	plget->rtime.tv_sec = ns / NSEC_PER_SEC;
	plget->rtime.tv_nsec = ns - (__u64)plget->rtime.tv_sec * NSEC_PER_SEC;
}


static void plget_set_stream_id(struct plgett *plget)
{
	plget->stream_id = atoi(optarg);

	if (plget->stream_id > 3)
		plget_fail("stream id has to be less then 3");

	plget->stream_id <<= STREAM_ID_SHIFT;
}

static void read_args(struct plgett *plget, int argc, char **argv)
{
	int idx, opt;

	while ((opt = getopt_long(argc, argv, "s:u:p:i:m:n:l:a:t:f:b:cw:r:k:d:q:z",
	       plget_options, &idx)) != -1) {
		switch (opt) {
		case 's':
			plget_set_pps(plget);
			break;
		case 'u':
			plget->port = atoi(optarg);
			break;
		case 't':
			plget_set_packet_type(plget);
			break;
		case 'i':
			strncpy(plget->if_name, optarg, sizeof(plget->if_name));
			break;
		case 'n':
			plget->pkt_num = atoi(optarg);
			break;
		case 'm':
			plget_set_mode(plget);
			break;
		case 'l':
			plget->pkt_size = atoi(optarg);
			break;
		case 'a':
			plget_set_address(plget);
			break;
		case 'c':
			plget->flags |= PLF_TITLE;
			break;
		case 'f':
			plget_set_output_format(plget);
			break;
		case 'p':
			plget->prio = atoi(optarg);
			plget->flags |= PLF_PRIO;
			break;
		case 'w':
			plget->busypoll_time = atoi(optarg);
			plget->flags |= PLF_BUSYPOLL;
			break;
		case 'r':
			plget_set_relative_time(plget);
			break;
		case 'k':
			plget_set_stream_id(plget);
			break;
		case 'd':
			plget->dev_deep = atoi(optarg);
			break;
		case 'q':
			plget->queue = atoi(optarg);
		case 'z':
			plget->flags |= PLF_ZERO_COPY;
		default:
			plget_fail("unknown option");
		}
	}
}

void plget_fail(char *err)
{
	if (err)
		printf("%s\n", err);

	plget_usage();
	exit(EXIT_FAILURE);
}

void plget_args(struct plgett *plget, int argc, char **argv)
{
	read_args(plget, argc, argv);
	plget_check_args(plget);
}
