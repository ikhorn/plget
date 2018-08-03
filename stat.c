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

#include <stdlib.h>
#include <stdio.h>
#include "stat.h"
#include "plget.h"
#include <math.h>
#include <string.h>

#define LOG_ENTRY_SIZE		18
#define LOG_BASE		8
#define LOG_LINE_SIZE		(LOG_BASE * LOG_ENTRY_SIZE + 1)

/* res = a - b; */
void ts_sub(struct timespec *a, struct timespec *b, struct timespec *res)
{
	typeof(res->tv_sec) sec;
	typeof(res->tv_nsec) nsec;

	sec = a->tv_sec - b->tv_sec;
	if (a->tv_nsec < b->tv_nsec) {
		nsec = NSEC_PER_SEC + a->tv_nsec - b->tv_nsec;
		sec--;
	} else {
		nsec = a->tv_nsec - b->tv_nsec;
	}

	res->tv_sec = sec;
	res->tv_nsec = nsec;
}

static inline __u64 stat_num(struct stats *ss)
{
	return ss->next_ts - ss->start_ts;
}

static inline __u64 to_num(struct stats *ss, struct timespec *ts)
{
	return ts - ss->start_ts;
}

static inline __u64 to_val(struct timespec *ts)
{
	return NSEC_PER_SEC * ts->tv_sec + ts->tv_nsec;
}

void stats_push(struct stats *ss, struct timespec *ts)
{
	ss->next_ts->tv_sec = ts->tv_sec;
	ss->next_ts->tv_nsec = ts->tv_nsec;
	ss->next_ts++;
}

void stats_push_id(struct stats *ss, struct timespec *ts, __u32 id)
{
	if (id == ss->id) {
		ss->next_ts->tv_sec = ts->tv_sec;
		ss->next_ts->tv_nsec = ts->tv_nsec;
		ss->next_ts++;
		ss->id++;
		return;
	}

	if (id > ss->id) {
		ss->id = id + 1;
		ss->next_ts += id - ss->id + 1;
	}

	(ss->start_ts + id)->tv_sec = ts->tv_sec;
	(ss->start_ts + id)->tv_nsec = ts->tv_nsec;
}

int stats_correct_id(struct stats *ss, __u32 id)
{
	return ts_correct(ss->start_ts + id);
}

static double stats_mean(struct stats *ss)
{
	struct timespec *ts;
	double mean = 0;
	__u64 val;
	__u64 n;

	n = stat_num(ss);
	for (ts = ss->start_ts; ts < ss->next_ts; ts++) {
		val = to_val(ts);
		mean += val / (n * 1000.0);
	}

	return mean;
}

static double stats_gap_mean(struct stats *ss)
{
	struct timespec *ts, temp;
	double mean = 0;
	__u64 n;

	n = stat_num(ss);
	for (ts = ss->start_ts + 1; ts < ss->next_ts; ts++) {
		ts_sub(ts, ts - 1, &temp);
		mean += to_val(&temp) / ((n - 1) * 1000.0);
	}

	return mean;
}

static double stats_gap_dev(struct stats *ss, double mean)
{
	struct timespec *ts, temp;
	double dev = 0;
	double mdif;
	__u64 n;

	n = stat_num(ss);
	for (ts = ss->start_ts + 1; ts < ss->next_ts; ts++) {
		ts_sub(ts, ts - 1, &temp);
		mdif = mean - to_val(&temp) / 1000.0;
		mdif *= mdif;
		dev += mdif / (n - 1);
	}

	dev = sqrt(dev);
	return dev;
}

static double stats_dev(struct stats *ss, double mean)
{
	struct timespec *ts;
	double dev = 0;
	double mdif;
	__u64 n;

	n = stat_num(ss);
	for (ts = ss->start_ts; ts < ss->next_ts; ts++) {
		mdif = mean - to_val(ts) / 1000.0;
		mdif *= mdif;
		dev += mdif / n;
	}

	dev = sqrt(dev);
	return dev;
}

void stats_diff(struct stats *a, struct stats *b, struct stats *res)
{
	struct timespec *tsa, *tsb;

	tsa = a->start_ts;
	tsb = b->start_ts;
	res->next_ts = res->start_ts;

	while (tsa < a->next_ts && tsb < b->next_ts) {
		if (ts_correct(tsa) && ts_correct(tsb)) {
			ts_sub(tsa++, tsb++, res->next_ts++);
		} else
			break;
	}
}

static void stats_print_log(struct stats *ss, int flags, struct timespec *rtime)
{
	double min_val = 1000000, max_val = 0;
	struct timespec *ts, temp;
	char line[LOG_LINE_SIZE];
	int max_n = 0, min_n = 0;
	__u64 n, start;
	double val;
	int pad;

	if (flags & STATS_LIN_DATA) {
		start = to_val(rtime);
		printf("relative abs time %llu ns\n", start);
		start = to_val(ss->start_ts);
		printf("first packet abs time %llu ns\n", start);
	}

	memset(line, '-', LOG_LINE_SIZE - 1);
	line[LOG_LINE_SIZE - 1] = 0;
	printf("%s", line);

	if (flags & STATS_PLAIN_OUTPUT)
		printf("\n");

	n = stat_num(ss);
	for (ts = ss->start_ts; ts < ss->next_ts; ts++) {
		if (flags & STATS_LIN_DATA) {
			ts_sub(ts, rtime, &temp);
			val = to_val(&temp) / 1000.0;
		} else if (flags & STATS_GAP_DATA) {
			if (!to_num(ss, ts)) {
				val = 0;
			} else {
				ts_sub(ts, ts - 1, &temp);
				val = to_val(&temp) / 1000.0;
			}
		} else {
			val = to_val(ts) / 1000.0;
		}

		if (flags & STATS_PLAIN_OUTPUT)
			printf("%g\n", val);
		else {
			if (!(to_num(ss, ts) % LOG_BASE))
				printf("\n");

			printf(" %15g |", val);
		}

		if (flags & STATS_GAP_DATA) {
			if (!to_num(ss, ts)) {
				if (n == 1)
					min_val = 0;
				continue;
			}
		}

		if (max_val < val) {
			max_n = to_num(ss, ts);
			max_val = val;
		}

		if (min_val > val) {
			min_n = to_num(ss, ts);
			min_val = val;
		}
	}

	int pad_needed = !(flags & STATS_PLAIN_OUTPUT) &&
			 (to_num(ss, ts) % LOG_BASE);
	if (pad_needed) {
		pad = (LOG_BASE - to_num(ss, ts) % LOG_BASE) * LOG_ENTRY_SIZE;
		printf("%*c", pad, '|');
	}

	printf("\n%s\n", line);

	if (flags & STATS_LIN_DATA)
		return;

	printf("max val(#%d) = %.2fus\n", max_n, max_val);
	printf("min val(#%d) = %.2fus\n", min_n, min_val);
	printf("peak-to-peak = %.2fus\n", max_val - min_val);
}

int stats_reserve(struct stats *ss, int entry_num)
{
	struct timespec *ts;

	ts = malloc(entry_num * sizeof(*ts));
	if (ts == NULL)
		return -1;

	ss->start_ts = ts;
	ss->next_ts = ts;

	return 0;
}

void stats_drate_print(struct timespec *interval, int pkt_num, int data_size)
{
	double rate, pps;

	pps = (double)pkt_num * NSEC_PER_SEC / to_val(interval);
	rate = (double)data_size * 8 * USEC_PER_SEC / to_val(interval);

	printf("RATE = %.2fkbps, PPS = %.1f\n", rate, pps);
}

void stats_rate_print(struct timespec *interval, int pkt_num, int pkt_size)
{
	int dsize;

	dsize = pkt_size * pkt_num;
	stats_drate_print(interval, pkt_num, dsize);
}

void stats_vrate_print(struct stats *ss, int pkt_size)
{
	struct timespec interval;
	unsigned int pkt_num;

	pkt_num = ss->next_ts - ss->start_ts;
	if (pkt_num-- < 2 || !ts_correct(ss->start_ts))
		return;

	ts_sub(ss->next_ts - 1, ss->start_ts, &interval);
	stats_rate_print(&interval, pkt_num, pkt_size);
}

int stats_print(char *str, struct stats *ss, int flags, struct timespec *rtime)
{
	double mean, dev;
	__u64 n;

	/* don't print if first entry is incorrect or no entries */
	n = stat_num(ss);
	if (!n || !ts_correct(ss->start_ts))
		return 0;

	printf("%s: packets %llu:\n", str, n);

	if (rtime)
		flags |= STATS_LIN_DATA;

	stats_print_log(ss, flags, rtime);

	if (flags & STATS_LIN_DATA)
		goto out;

	if (flags & STATS_GAP_DATA) {
		mean = stats_gap_mean(ss);
		dev = stats_gap_dev(ss, mean);
	} else {
		mean = stats_mean(ss);
		dev = stats_dev(ss, mean);
	}

	printf("mean +- RMS = %.2f +- %.2f us\n", mean, dev);
out:
	printf("\n");
	return n;
}
