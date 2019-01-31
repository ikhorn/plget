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
#include "xdp_prog_load.h"
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <linux/errqueue.h>
#include <poll.h>
#include <string.h>

#ifndef AF_XDP
#define AF_XDP 44
#endif

#ifndef SOL_XDP
#define SOL_XDP 283
#endif

#define RQ_DESC_NUM	256
#define TQ_DESC_NUM	256
#define FQ_DESC_NUM	256
#define CQ_DESC_NUM	256

#define FRAME_HEADROOM	0

#define barrier() __asm__ __volatile__("": : :"memory")

#ifdef __aarch64__
#define __smp_rmb() __asm__ __volatile__("dmb ishld": : :"memory")
#define __smp_wmb() __asm__ __volatile__("dmb ishst": : :"memory")
#else
#define __smp_rmb() barrier()
#define __smp_wmb() barrier()
#endif

static inline __u32 queue_get_num(struct queue *q, __u32 num)
{
	__u32 entries = q->cached_prod - q->cached_cons;

	if (entries == 0) {
		q->cached_prod = *q->producer;
		entries = q->cached_prod - q->cached_cons;
	}

	return (entries > num) ? num : entries;
}

static inline __u32 queue_get_free_num(struct queue *q, __u32 num)
{
	__u32 entries = q->cached_cons - q->cached_prod;

	if (entries >= num)
		return entries;

	/* Refresh the local tail pointer */
	q->cached_cons = *q->consumer + q->size;
	return q->cached_cons - q->cached_prod;
}

static int get_ring_offsets(int sfd, struct xdp_mmap_offsets *offsets)
{
	socklen_t opt_len;
	int ret;

	opt_len = sizeof(struct xdp_mmap_offsets);
	ret = getsockopt(sfd, SOL_XDP, XDP_MMAP_OFFSETS, offsets, &opt_len);
	if (ret)
		return perror("cannot get xdp mmap offsets"), -errno;

	return 0;
}

static void *frames_allocate(int sfd)
{
	struct xdp_umem_reg mr;
	void *bufs;
	int ret;

	ret = posix_memalign(&bufs, getpagesize(), FRAME_NUM * FRAME_SIZE);
	if (ret)
		return perror("cannot allocate frames memory"), NULL;

	/* register/map user memory for frames */
	mr.addr = (unsigned long)bufs;
	mr.len = FRAME_NUM * FRAME_SIZE;
	mr.chunk_size = FRAME_SIZE;
	mr.headroom = FRAME_HEADROOM;

	ret = setsockopt(sfd, SOL_XDP, XDP_UMEM_REG, &mr, sizeof(mr));
	if (ret)
		return perror("cannot register umem for frames"), NULL;

	return bufs;
}

static int fill_ring_allocate(struct sock_umem *umem)
{
	struct xdp_mmap_offsets offsets;
	int sfd = umem->fd;
	int desc_num, ret;

	desc_num = FQ_DESC_NUM;
	ret = setsockopt(sfd, SOL_XDP, XDP_UMEM_FILL_RING, &desc_num,
			 sizeof(int));
	if (ret)
		return perror("cannot set size for fill queue"), -errno;

	ret = get_ring_offsets(sfd, &offsets);
	if (ret)
		return ret;

	umem->fq.map = mmap(0, offsets.fr.desc + FQ_DESC_NUM * sizeof(__u64),
			    PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
			    sfd, XDP_UMEM_PGOFF_FILL_RING);
	if (umem->fq.map == MAP_FAILED)
		return perror("cannot map fill queue memory"), -errno;

	umem->fq.mask = FQ_DESC_NUM - 1;
	umem->fq.size = FQ_DESC_NUM;
	umem->fq.producer = umem->fq.map + offsets.fr.producer;
	umem->fq.consumer = umem->fq.map + offsets.fr.consumer;
	umem->fq.ring = umem->fq.map + offsets.fr.desc;
	umem->fq.cached_cons = FQ_DESC_NUM;

	return 0;
}

static int completion_ring_allocate(struct sock_umem *umem)
{
	struct xdp_mmap_offsets offsets;
	int sfd = umem->fd;
	int desc_num, ret;

	desc_num = CQ_DESC_NUM;
	ret = setsockopt(sfd, SOL_XDP, XDP_UMEM_COMPLETION_RING, &desc_num,
			 sizeof(int));
	if (ret)
		return perror("cannot set size for completion queue"), -errno;

	ret = get_ring_offsets(sfd, &offsets);
	if (ret)
		return ret;

	umem->cq.map = mmap(0, offsets.cr.desc + CQ_DESC_NUM * sizeof(__u64),
			     PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
			     sfd, XDP_UMEM_PGOFF_COMPLETION_RING);
	if (umem->cq.map == MAP_FAILED)
		return perror("cannot map completion queue memory"), -errno;

	umem->cq.mask = CQ_DESC_NUM - 1;
	umem->cq.size = CQ_DESC_NUM;
	umem->cq.producer = umem->cq.map + offsets.cr.producer;
	umem->cq.consumer = umem->cq.map + offsets.cr.consumer;
	umem->cq.ring = umem->cq.map + offsets.cr.desc;

	return 0;
}

static int rx_ring_allocate(struct xsock *xsk)
{
	struct xdp_mmap_offsets offsets;
	int sfd = xsk->sfd;
	int desc_num, ret;

	/* set number of descriptors for tx and rx queues */
	desc_num = RQ_DESC_NUM;
	ret = setsockopt(sfd, SOL_XDP, XDP_RX_RING, &desc_num, sizeof(int));
	if (ret)
		return perror("xdp socket rx ring desc num"), -errno;

	ret = get_ring_offsets(sfd, &offsets);
	if (ret)
		return ret;

	xsk->rq.map = mmap(0, offsets.rx.desc + RQ_DESC_NUM * sizeof(struct xdp_desc),
			   PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, sfd,
			   XDP_PGOFF_RX_RING);
	if (xsk->rq.map == MAP_FAILED)
		return perror("cannot map rx ring memory"), -errno;

	xsk->rq.mask = RQ_DESC_NUM - 1;
	xsk->rq.size = RQ_DESC_NUM;
	xsk->rq.producer = xsk->rq.map + offsets.rx.producer;
	xsk->rq.consumer = xsk->rq.map + offsets.rx.consumer;
	xsk->rq.ring = xsk->rq.map + offsets.rx.desc;

	return 0;
}

static int tx_ring_allocate(struct xsock *xsk)
{
	struct xdp_mmap_offsets offsets;
	int sfd = xsk->sfd;
	int desc_num, ret;

	desc_num = TQ_DESC_NUM;
	ret = setsockopt(sfd, SOL_XDP, XDP_TX_RING, &desc_num, sizeof(int));
	if (ret)
		return perror("xdp socket tx ring desc num"), -errno;

	ret = get_ring_offsets(sfd, &offsets);
	if (ret)
		return ret;

	xsk->tq.map = mmap(0, offsets.tx.desc + TQ_DESC_NUM * sizeof(struct xdp_desc),
			   PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, sfd,
			   XDP_PGOFF_TX_RING);
	if (xsk->tq.map == MAP_FAILED)
		return perror("cannot map rx ring memory"), -errno;

	xsk->tq.mask = TQ_DESC_NUM - 1;
	xsk->tq.size = TQ_DESC_NUM;
	xsk->tq.producer = xsk->tq.map + offsets.tx.producer;
	xsk->tq.consumer = xsk->tq.map + offsets.tx.consumer;
	xsk->tq.ring = xsk->tq.map + offsets.tx.desc;
	xsk->tq.cached_cons = TQ_DESC_NUM;

	return 0;
}

static inline int umem_fq_enqueue(struct queue *fq, struct xdp_desc *d,
				  __u32 num)
{
	__u32 i, idx;
	umem_desc *desc = fq->ring;

	if (queue_get_free_num(fq, num) < num)
		return -ENOSPC;

	for (i = 0; i < num; i++) {
		idx = fq->cached_prod++ & fq->mask;
		desc[idx] = d[i].addr;
	}

	__smp_wmb();

	*fq->producer = fq->cached_prod;
	return 0;
}

static int fq_populate(struct queue *fq)
{
	struct xdp_desc desc;
	__u64 max_addr, *addr;

	addr = &desc.addr;
	max_addr = FQ_DESC_NUM * FRAME_SIZE;

	for (*addr = 0; *addr < max_addr; *addr += FRAME_SIZE)
		if (umem_fq_enqueue(fq, &desc, 1))
			return perror("cannot populate fill queue"), -errno;

	return 0;
}

static struct sock_umem *umem_allocate(int sfd)
{
	struct sock_umem *umem;
	int ret;

	umem = calloc(1, sizeof(struct sock_umem));
	if (!umem)
		return perror("cannot allocate umem shell"), NULL;

	umem->frames = frames_allocate(sfd);
	if (!umem->frames)
		return perror("cannot allocate umem shell"), NULL;

	umem->fd = sfd;
	ret = fill_ring_allocate(umem);
	if (ret)
		return perror("cannot fill ring"), NULL;

	/* populate fill queue */
	fq_populate(&umem->fq);

	ret = completion_ring_allocate(umem);
	if (ret)
		return perror("cannot fill ring"), NULL;

	return umem;
}

int xdp_socket(struct plgett *plget)
{
	struct sockaddr_xdp *addr = (struct sockaddr_xdp *)&plget->sk_addr;
	struct rlimit r = {RLIM_INFINITY, RLIM_INFINITY};
	struct xsock *xsk;
	int ret, sfd;

	if (setrlimit(RLIMIT_MEMLOCK, &r))
		return perror("setting rlimit err"), -errno;

	xsk = calloc(1, sizeof(*xsk));
	if (!xsk)
		return -errno;

	sfd = socket(AF_XDP, SOCK_RAW, 0);
	if (sfd < 0)
		return perror("xdp socket"), -errno;

	xsk->sfd = sfd;
	xsk->umem = umem_allocate(sfd);
	if (!xsk->umem)
		return perror("cannot allocate umem"), -errno;

	ret = rx_ring_allocate(xsk);
	if (ret)
		return perror("cannot allocate rx ring"), -errno;

	ret = tx_ring_allocate(xsk);
	if (ret)
		return perror("cannot allocate tx ring"), -errno;

	/* bind socket with interface and queue */
	addr->sxdp_family = AF_XDP;
	addr->sxdp_ifindex = plget->ifidx;
	addr->sxdp_queue_id = plget->queue;

	if (plget->flags & PLF_ZERO_COPY)
		addr->sxdp_flags = XDP_ZEROCOPY;
	else
		addr->sxdp_flags = XDP_COPY;

	ret = bind(sfd, (struct sockaddr *)addr, sizeof(struct sockaddr_xdp));
	if (ret)
		return perror("cannot bind dev and queue with socket"), -errno;

	plget->xsk = xsk;

	ret = xdp_load_prog(plget);
	if (ret)
		return perror("cannot load xdp prog"), -errno;

	return sfd;
}

static inline __u32 cq_deq(struct queue *q, __u64 *descs, __u32 num)
{
	umem_desc *r = q->ring;
	__u32 idx, i, entries;

	entries = queue_get_num(q, num);

	__smp_rmb();

	for (i = 0; i < entries; i++) {
		idx = q->cached_cons++ & q->mask;
		descs[i] = r[idx];
	}

	if (entries > 0) {
		__smp_wmb();

		*q->consumer = q->cached_cons;
	}

	return entries;
}

static inline __u32 rq_deq(struct queue *q, struct xdp_desc *descs, int num)
{
	struct xdp_desc *r = q->ring;
	__u32 idx, i, entries;

	entries = queue_get_num(q, num);

	__smp_rmb();

	for (i = 0; i < entries; i++) {
		idx = q->cached_cons++ & q->mask;
		descs[i] = r[idx];
	}

	if (entries > 0) {
		__smp_wmb();

		*q->consumer = q->cached_cons;
	}

	return entries;
}

static int xsk_tx_complete(struct xsock *xsk, __u32 num)
{
	int ret;
	__u64 desc;

	ret = sendto(xsk->sfd, NULL, 0, MSG_DONTWAIT, NULL, 0);
	if (ret < 0)
		return ret;

	ret = cq_deq(&xsk->umem->cq, &desc, num);
	return ret;
}

int xsk_sendto(struct plgett *plget)
{
	struct xsock *xsk = plget->xsk;
	struct queue *tq = &xsk->tq;
	struct xdp_desc *ring = tq->ring;
	unsigned int frame_idx;
	__u32 ret, desc_idx;

	/* prepare frame */
	ret = queue_get_free_num(tq, 1);
	if (!ret)
		return 0;

	/* enqueue */
	frame_idx = (plget->pkt - xsk->umem->frames) >> FRAME_SHIFT;

	desc_idx = tq->cached_prod++ & tq->mask;
	ring[desc_idx].addr = frame_idx << FRAME_SHIFT;
	ring[desc_idx].len = plget->pkt_size;

	__smp_wmb();

	*tq->producer = tq->cached_prod;

	/* kick */
	ret = xsk_tx_complete(xsk, 1);
	if (ret < 0)
		return perror("sendto"), ret;

	if (frame_idx >= FRAME_NUM - 1)
		plget->pkt = xsk->umem->frames;
	else
		plget->pkt += FRAME_SIZE;

	return plget->sk_payload_size;
}

/* Rx part */
static inline void *xq_get_data(struct xsock *xsk, __u64 addr)

{
	return &xsk->umem->frames[addr];
}

int xsk_poll(struct xdp_desc *desc, struct timespec *ts2)
{
	struct xsock *xsk = plget->xsk;
	struct pollfd fds;
	char *data, *temp;
	unsigned int ret;
	__u16 proto;

	fds.fd = plget->sfd;
	fds.events = POLLIN;

	ret = poll(&fds, 1, -1);
	if (ret <= 0)
		return perror("Some error on poll()"), -errno;

	ret = rq_deq(&xsk->rq, desc, 1);
	if (!ret)
		return -1;

	data = xq_get_data(xsk, desc->addr - 2 * sizeof(__u64));
	clock_gettime(CLOCK_REALTIME, ts2);

	/* to be sure, drop not compatible packets */
	if (desc->len < ETH_HLEN)
		goto err;

	temp = data + 2 * sizeof(__u64) + ETH_ALEN * 2;
	proto = *temp | (*(temp + 1) << 8);
	if (plget->flags & PLF_PTP && proto != htons(ETH_P_1588))
		goto err;

	plget->pkt = data;

	return desc->len;

err:
	umem_fq_enqueue(&xsk->umem->fq, desc, 1);
	return -1;
}

int xsk_recvmsg(struct plgett *plget, struct msghdr *msg, struct timespec *ts2)
{
	struct scm_timestamping *tss;
	struct cmsghdr *cmsg;
	struct timespec *ts1;
	struct xdp_desc desc;
	char *data;
	__u64 ns;
	int len;

	do {
		len = xsk_poll(&desc, ts2);
	} while (len <= 0);

	data = plget->pkt;

	cmsg = msg->msg_control;
	cmsg->cmsg_len =
		sizeof(struct cmsghdr) + sizeof(struct scm_timestamping);

	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_TIMESTAMPING;
	tss = (struct scm_timestamping *)CMSG_DATA(cmsg);

	memcpy(&ns, data, sizeof(ns));
	ts1 = &tss->ts[2];
	ts1->tv_sec = ns / NSEC_PER_SEC;
	ts1->tv_nsec = ns - ts1->tv_sec * NSEC_PER_SEC;

	memcpy(&ns, data + sizeof(ns), sizeof(ns));
	ts1 = tss->ts;
	ts1->tv_sec = ns / NSEC_PER_SEC;
	ts1->tv_nsec = ns - ts1->tv_sec * NSEC_PER_SEC;

	plget->pkt += 2 * sizeof(ns);

	umem_fq_enqueue(&plget->xsk->umem->fq, &desc, 1);

	msg->msg_controllen = CMSG_ALIGN(cmsg->cmsg_len);

	return len;
}
