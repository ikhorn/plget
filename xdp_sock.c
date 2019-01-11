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

#include "xdp_sock.h"
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
//#include <linux/if_xdp.h>

#ifndef XDP_RX_RING
#include "linux/if_xdp.h"
#endif

#ifndef AF_XDP
#define AF_XDP 44
#endif

#ifndef SOL_XDP
#define SOL_XDP 283
#endif

#define RQ_DESC_NUM	1024
#define TQ_DESC_NUM	1024
#define FQ_DESC_NUM	1024
#define CQ_DESC_NUM	1024

#define FRAME_NUM	256	/* number of frames to operate on */
#define FRAME_SIZE	2048	/* 2 frames per page */
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
	struct sock_queue rx;
	struct sock_queue tx;
	int sfd;
	struct xdp_umem *umem;
	__u32 outstanding_tx;
	unsigned long rx_npkts;
	unsigned long tx_npkts;
	unsigned long prev_rx_npkts;
	unsigned long prev_tx_npkts;
};

static void *xdpsk_allocate_frames_memory(int sfd)
{
	struct xdp_umem_reg mr;
	void *bufs;
	int ret;

	ret = posix_memalign(&bufs, getpagesize(), FRAME_NUM * FRAME_SIZE);
	if (ret)
		return perror("cannot allocate frames memory"), NULL;

	/* register/map user memory for payload/frames */
	mr.addr = (unsigned long)bufs;
	mr.len = FRAME_NUM * FRAME_SIZE;
	mr.chunk_size = FRAME_SIZE;
	mr.headroom = FRAME_HEADROOM;

	ret = setsockopt(sfd, SOL_XDP, XDP_UMEM_REG, &mr, sizeof(mr));
	if (ret)
		return perror("cannot register umem for frames"), NULL;

	return bufs;
}

static struct sock_umem *umem_allocate(int sfd)
{
	struct xdp_mmap_offsets offsets;
	struct sock_umem *umem;
	int desc_num, ret;
	socklen_t opt_len;
	void *bufs;

	umem = calloc(1, sizeof(struct sock_umem));
	if (!umem)
		return perror("cannot allocate umem shell"), NULL;

	bufs = xdpsk_allocate_frames_memory(sfd);
	if (!bufs)
		return perror("cannot allocate umem shell"), NULL;

	/* set size of fill and completion queues */
	desc_num = FQ_DESC_NUM;
	ret = setsockopt(sfd, SOL_XDP, XDP_UMEM_FILL_RING, &desc_num,
			 sizeof(int));
	if (ret)
		return perror("cannot set size for fill queue"), NULL;

	desc_num = CQ_DESC_NUM;
	ret = setsockopt(sfd, SOL_XDP, XDP_UMEM_COMPLETION_RING, &desc_num,
			 sizeof(int));
	if (ret)
		return perror("cannot set size for completion queue"), NULL;

	/* get offsets for the rings */
	opt_len = sizeof(offsets);
	ret = getsockopt(sfd, SOL_XDP, XDP_MMAP_OFFSETS, &offsets, &opt_len);
	if (ret)
		return perror("cannot get xdp mmap offsets"), NULL;

	/* initialize fill queue */
	umem->fq.map = mmap(0, offsets.fr.desc + FQ_DESC_NUM * sizeof(__u64),
			    PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
			    sfd, XDP_UMEM_PGOFF_FILL_RING);
	if (umem->fq.map == MAP_FAILED)
		return perror("cannot map fill queue memory"), NULL;

	umem->fq.mask = FQ_DESC_NUM - 1;
	umem->fq.size = FQ_DESC_NUM;
	umem->fq.producer = umem->fq.map + offsets.fr.producer;
	umem->fq.consumer = umem->fq.map + offsets.fr.consumer;
	umem->fq.ring = umem->fq.map + offsets.fr.desc;
	umem->fq.cached_cons = FQ_DESC_NUM;

	/* initialize completion queue */
	umem->cq.map = mmap(0, offsets.cr.desc + CQ_DESC_NUM * sizeof(__u64),
			     PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
			     sfd, XDP_UMEM_PGOFF_COMPLETION_RING);
	if (umem->cq.map == MAP_FAILED)
		return perror("cannot map completion queue memory"), NULL;

	umem->cq.mask = CQ_DESC_NUM - 1;
	umem->cq.size = CQ_DESC_NUM;
	umem->cq.producer = umem->cq.map + offsets.cr.producer;
	umem->cq.consumer = umem->cq.map + offsets.cr.consumer;
	umem->cq.ring = umem->cq.map + offsets.cr.desc;

	umem->frames = bufs;
	umem->fd = sfd;

	return umem;
}

int xdp_socket(struct plgett *plget)
{
	struct sock_umem *umem;
	struct xsock *xsk;
	int sfd;

	sfd = socket(AF_XDP, SOCK_RAW, 0);
	if (sfd < 0)
		return perror("xdp socket"), -errno;

	plget->sfd = sfd;

	umem = umem_allocate(sfd);

	return sfd;
}
