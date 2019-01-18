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

#ifndef PLGET_XDP_SOCK_H
#define PLGET_XDP_SOCK_H

#include "plget.h"

#define FRAME_SHIFT	11
#define FRAME_SIZE	(1 << FRAME_SHIFT)	/* 2 frames per page */
#define FRAME_NUM	256	/* number of frames to operate on */
#define FRAME_HEADROOM	0

struct umem_queue {
	__u32 cached_prod;
	__u32 cached_cons;
	__u32 mask;
	__u32 size;
	__u32 *producer;
	__u32 *consumer;
	__u64 *ring;
	void *map;
};

struct sock_umem {
	char *frames;
	struct umem_queue fq;
	struct umem_queue cq;
	int fd;
};

struct sock_queue {
	__u32 cached_prod;
	__u32 cached_cons;
	__u32 mask;
	__u32 size;
	__u32 *producer;
	__u32 *consumer;
	struct xdp_desc *ring;
	void *map;
};

struct xsock {
	struct sock_queue rq;
	struct sock_queue tq;
	struct sock_umem *umem;
	int sfd;
};

int xdp_socket(struct plgett *plget);
int xsk_sendto(struct plgett *plget);

#endif
