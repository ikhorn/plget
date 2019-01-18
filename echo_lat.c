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

#include <linux/net_tstamp.h>
#include <time.h>
#include <linux/errqueue.h>
#include <net/ethernet.h>
#include <stdio.h>
#include "plget.h"
#include "stat.h"
#include <errno.h>
#include "tx_lat.h"
#include "rx_lat.h"
#include "echo_lat.h"

int echolat(struct plgett *plget)
{
	int i = 0;
	int off_magic_rd_base = plget->off_magic_rd;

	for (; i < plget->pkt_num; ++i) {
		rxlat_proc_packet(plget);
		if (!(plget->flags & PLF_TS_ID_ALLOWED))
			plget->off_magic_rd =
				off_magic_rd_base - plget->sk_payload_size;
		txlat_proc_packet(plget);
	}

	return 0;
}
