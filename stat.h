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

#include <time.h>
#include <linux/types.h>

#define STATS_PLAIN_OUTPUT	0x01
#define STATS_LIN_DATA		0x02
#define STATS_GAP_DATA		0x04

#ifndef LAT_STAT_H
#define LAT_STAT_H

struct stats {
	struct timespec *next_ts;
	struct timespec *start_ts;
	__u32 id;
};

void ts_sub(struct timespec *a, struct timespec *b, struct timespec *res);
void stats_push(struct stats *ss, struct timespec *ts);
void stats_push_id(struct stats *ss, struct timespec *ts, __u32 id);
int stats_print(char *str, struct stats *ss, int flags, struct timespec *rtime);
int stats_reserve(struct stats *ss, int entry_num);
void stats_diff(struct stats *a, struct stats *b, struct stats *res);
int stats_correct_id(struct stats *ss, __u32 id);

void stats_vrate_print(struct stats *ss, int pkt_size);
void stats_rate_print(struct timespec *interval, int pkt_num, int pkt_size);
void stats_drate_print(struct timespec *interval, int pkt_num, int data_size);

#endif
