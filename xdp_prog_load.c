/*
 * Copyright (C) 2019
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

#include <linux/if_link.h>
#include "xdp_prog_load.h"
#include <stdlib.h>
#include <libbpf.h>
#include <bpf.h>
#include "xdp_sock.h"
#include <errno.h>
#include <signal.h>
#include "plget_args.h"

const char *xdp_file_name = "xsock_dispatch.o";
#define XDP_FLAGS_DRV_MODE		(1U << 2)

void xdp_unload_prog(void)
{
	if (!(plget->flags & PLF_PROG_LOADED))
		return;

	if (bpf_set_link_xdp_fd(plget->ifidx, -1, XDP_FLAGS_DRV_MODE))
		perror("link unset xdp prog failed");
}

static void sig_exit(int sig)
{
	if (plget->pkt_type == PKT_XDP)
		xdp_unload_prog();

	printf("   exit, prog unloaded\n");
	exit(EXIT_SUCCESS);
}

int xdp_load_prog(void)
{
	struct bpf_prog_load_attr prog_attr = {
		.prog_type	= BPF_PROG_TYPE_XDP,
	};
	int qidconf_map, xsks_map;
	struct bpf_object *obj;
	struct bpf_map *map;
	int prog_fd, key = 0;
	int ret;

	if (plget->mod == TX_LAT)
		return 0;

	prog_attr.file = xdp_file_name;

	signal(SIGINT, sig_exit);
	signal(SIGTERM, sig_exit);
	signal(SIGABRT, sig_exit);

	plget->flags |= PLF_PROG_LOADED;

	if (bpf_prog_load_xattr(&prog_attr, &obj, &prog_fd))
		return perror("no program found"), -errno;

	if (prog_fd < 0)
		return perror("no program found"), -errno;

	map = bpf_object__find_map_by_name(obj, "qidconf_map");
	qidconf_map = bpf_map__fd(map);
	if (qidconf_map < 0)
		return perror("no qidconf map found"), -errno;

	map = bpf_object__find_map_by_name(obj, "xsks_map");
	xsks_map = bpf_map__fd(map);
	if (xsks_map < 0)
		return perror("no xsks map found"), -errno;

	if (bpf_set_link_xdp_fd(plget->ifidx, prog_fd, XDP_FLAGS_DRV_MODE) < 0)
		return perror("link set xdp fd failed"), -errno;

	ret = bpf_map_update_elem(qidconf_map, &key, &plget->queue, 0);
	if (ret)
		return perror("bpf_map_update_elem qidconf"), -errno;

	ret = bpf_map_update_elem(xsks_map, &key, &plget->xsk->sfd, 0);
	if (ret)
		return perror("bpf_map_update_sk elem xsks_map"), -errno;

	return 0;
}
