// SPDX-License-Identifier: GPL-2.0
/*
 * NVMe over Fabrics RDMA host code.
 * Copyright (c) 2015-2016 HGST, a Western Digital Company.
 */

/*
 * [한국어 설명] NVMe over RDMA 호스트 트랜스포트 (rdma.c)
 *
 * === 파일의 역할 ===
 * InfiniBand/RoCE/iWARP RDMA CM·QP 위에서 NVMe-oF 를 구현한다. CM 연결 후
 * QP 생성, RECV 링 게시, fabrics Connect, SEND/WRITE/READ 로 캡슐·데이터를
 * 제로카피 전송한다. MR 등록(FastReg)과 CQ 폴링/인터럽트가 완료 경로다.
 *
 * === 아키텍처 위치 ===
 * nvmf create_ctrl → rdma_resolve/connect → QP/CQ/PD → Connect → LIVE
 * 제출: queue_rq → map SG → post_send → CQ → complete
 * 재연결: CM 이벤트/ WC 에러 → reconnect 상태기계(fabrics 옵션)
 *
 * === 주요 구조 ===
 * nvme_rdma_ctrl/queue/request/device — CM id, QP, MR, SGE, tagset
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt	/* [한국어] 상수/매크로 — PDU·큐·타임아웃·플래그 */
#include <linux/module.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include <linux/init.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include <linux/slab.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include <rdma/mr_pool.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include <linux/err.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include <linux/string.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include <linux/atomic.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include <linux/blk-mq.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include <linux/blk-integrity.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include <linux/types.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include <linux/list.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include <linux/mutex.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include <linux/scatterlist.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include <linux/nvme.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include <linux/unaligned.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */

#include <rdma/ib_verbs.h>	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
#include <rdma/rdma_cm.h>	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
#include <linux/nvme-rdma.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */

#include "nvme.h"	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include "fabrics.h"	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */


#define NVME_RDMA_CM_TIMEOUT_MS		3000		/* 3 second */	/* [한국어] 상수/매크로 — PDU·큐·타임아웃·플래그 */

#define NVME_RDMA_MAX_SEGMENTS		256	/* [한국어] 상수/매크로 — PDU·큐·타임아웃·플래그 */

#define NVME_RDMA_MAX_INLINE_SEGMENTS	4	/* [한국어] 상수/매크로 — PDU·큐·타임아웃·플래그 */

#define NVME_RDMA_DATA_SGL_SIZE \
	(sizeof(struct scatterlist) * NVME_INLINE_SG_CNT)	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
#define NVME_RDMA_METADATA_SGL_SIZE \
	(sizeof(struct scatterlist) * NVME_INLINE_METADATA_SG_CNT)	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

struct nvme_rdma_device {	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct ib_device	*dev;	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	struct ib_pd		*pd;	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	struct kref		ref;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct list_head	entry;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	unsigned int		num_inline_segments;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

struct nvme_rdma_qe {	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct ib_cqe		cqe;	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	void			*data;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	u64			dma;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

struct nvme_rdma_sgl {	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	int			nents;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct sg_table		sg_table;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

struct nvme_rdma_queue;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
struct nvme_rdma_request {	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct nvme_request	req;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct ib_mr		*mr;	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	struct nvme_rdma_qe	sqe;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	union nvme_result	result;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	__le16			status;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	refcount_t		ref;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct ib_sge		sge[1 + NVME_RDMA_MAX_INLINE_SEGMENTS];	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	u32			num_sge;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct ib_reg_wr	reg_wr;	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	struct ib_cqe		reg_cqe;	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	struct nvme_rdma_queue  *queue;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct nvme_rdma_sgl	data_sgl;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct nvme_rdma_sgl	*metadata_sgl;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	bool			use_sig_mr;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

enum nvme_rdma_queue_flags {	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	NVME_RDMA_Q_ALLOCATED		= 0,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	NVME_RDMA_Q_LIVE		= 1,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	NVME_RDMA_Q_TR_READY		= 2,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

struct nvme_rdma_queue {	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct nvme_rdma_qe	*rsp_ring;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	int			queue_size;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	size_t			cmnd_capsule_len;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_ctrl	*ctrl;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct nvme_rdma_device	*device;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct ib_cq		*ib_cq;	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	struct ib_qp		*qp;	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */

	unsigned long		flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct rdma_cm_id	*cm_id;	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	int			cm_error;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct completion	cm_done;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	bool			pi_support;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int			cq_size;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct mutex		queue_lock;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

struct nvme_rdma_ctrl {	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	/* read only in the hot path */
	struct nvme_rdma_queue	*queues;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	/* other member variables */
	struct blk_mq_tag_set	tag_set;	/* [한국어] blk-mq — 태그·hctx·타임아웃·맵·완료 연동 */
	struct work_struct	err_work;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	struct nvme_rdma_qe	async_event_sqe;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	struct delayed_work	reconnect_work;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	struct list_head	list;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	struct blk_mq_tag_set	admin_tag_set;	/* [한국어] blk-mq — 태그·hctx·타임아웃·맵·완료 연동 */
	struct nvme_rdma_device	*device;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	u32			max_fr_pages;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	struct sockaddr_storage addr;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct sockaddr_storage src_addr;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	struct nvme_ctrl	ctrl;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	bool			use_inline_data;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	u32			io_queues[HCTX_MAX_TYPES];	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline struct nvme_rdma_ctrl *to_rdma_ctrl(struct nvme_ctrl *ctrl)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return container_of(ctrl, struct nvme_rdma_ctrl, ctrl);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static LIST_HEAD(device_list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
static DEFINE_MUTEX(device_list_mutex);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static LIST_HEAD(nvme_rdma_ctrl_list);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
static DEFINE_MUTEX(nvme_rdma_ctrl_mutex);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

/*
 * Disabling this option makes small I/O goes faster, but is fundamentally
 * unsafe.  With it turned off we will have to register a global rkey that
 * allows read and write access to all physical memory.
 */
static bool register_always = true;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
module_param(register_always, bool, 0444);	/* [한국어] 모듈 경계·파라미터·심볼 공개 */
MODULE_PARM_DESC(register_always,	/* [한국어] 모듈 경계·파라미터·심볼 공개 */
	 "Use memory registration even for contiguous memory regions");	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_rdma_cm_handler(struct rdma_cm_id *cm_id,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		struct rdma_cm_event *event);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
static void nvme_rdma_recv_done(struct ib_cq *cq, struct ib_wc *wc);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
static void nvme_rdma_complete_rq(struct request *rq);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

static const struct blk_mq_ops nvme_rdma_mq_ops;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
static const struct blk_mq_ops nvme_rdma_admin_mq_ops;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

static inline int nvme_rdma_queue_idx(struct nvme_rdma_queue *queue)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return queue - queue->ctrl->queues;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static bool nvme_rdma_poll_queue(struct nvme_rdma_queue *queue)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return nvme_rdma_queue_idx(queue) >	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		queue->ctrl->io_queues[HCTX_TYPE_DEFAULT] +	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		queue->ctrl->io_queues[HCTX_TYPE_READ];	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline size_t nvme_rdma_inline_data_size(struct nvme_rdma_queue *queue)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return queue->cmnd_capsule_len - sizeof(struct nvme_command);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_free_qe(struct ib_device *ibdev, struct nvme_rdma_qe *qe,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		size_t capsule_size, enum dma_data_direction dir)	/* [한국어] DMA 매핑 — 장치가 접근할 주소 확보 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ib_dma_unmap_single(ibdev, qe->dma, capsule_size, dir);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	kfree(qe->data);	/* [한국어] 커널 메모리 생명주기 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_rdma_alloc_qe(struct ib_device *ibdev, struct nvme_rdma_qe *qe,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		size_t capsule_size, enum dma_data_direction dir)	/* [한국어] DMA 매핑 — 장치가 접근할 주소 확보 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	qe->data = kzalloc(capsule_size, GFP_KERNEL);	/* [한국어] 커널 메모리 생명주기 */
	if (!qe->data)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return -ENOMEM;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	qe->dma = ib_dma_map_single(ibdev, qe->data, capsule_size, dir);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	if (ib_dma_mapping_error(ibdev, qe->dma)) {	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
		kfree(qe->data);	/* [한국어] 커널 메모리 생명주기 */
		qe->data = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return -ENOMEM;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_free_ring(struct ib_device *ibdev,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		struct nvme_rdma_qe *ring, size_t ib_queue_size,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		size_t capsule_size, enum dma_data_direction dir)	/* [한국어] DMA 매핑 — 장치가 접근할 주소 확보 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int i;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	for (i = 0; i < ib_queue_size; i++)	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
		nvme_rdma_free_qe(ibdev, &ring[i], capsule_size, dir);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	kfree(ring);	/* [한국어] 커널 메모리 생명주기 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static struct nvme_rdma_qe *nvme_rdma_alloc_ring(struct ib_device *ibdev,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		size_t ib_queue_size, size_t capsule_size,	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
		enum dma_data_direction dir)	/* [한국어] DMA 매핑 — 장치가 접근할 주소 확보 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_qe *ring;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	int i;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ring = kzalloc_objs(struct nvme_rdma_qe, ib_queue_size);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	if (!ring)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return NULL;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	/*
	 * Bind the CQEs (post recv buffers) DMA mapping to the RDMA queue
	 * lifetime. It's safe, since any change in the underlying RDMA device
	 * will issue error recovery and queue re-creation.
	 */
	for (i = 0; i < ib_queue_size; i++) {	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
		if (nvme_rdma_alloc_qe(ibdev, &ring[i], capsule_size, dir))	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
			goto out_free_ring;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return ring;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

out_free_ring:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_rdma_free_ring(ibdev, ring, i, capsule_size, dir);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	return NULL;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_qp_event(struct ib_event *event, void *context)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	pr_debug("QP event %s (%d)\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		 ib_event_msg(event->event), event->event);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */

}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_rdma_wait_for_cm(struct nvme_rdma_queue *queue)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = wait_for_completion_interruptible(&queue->cm_done);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	WARN_ON_ONCE(queue->cm_error > 0);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return queue->cm_error;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_rdma_create_qp(struct nvme_rdma_queue *queue, const int factor)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_device *dev = queue->device;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct ib_qp_init_attr init_attr;	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	memset(&init_attr, 0, sizeof(init_attr));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	init_attr.event_handler = nvme_rdma_qp_event;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	/* +1 for drain */
	init_attr.cap.max_send_wr = factor * queue->queue_size + 1;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	/* +1 for drain */
	init_attr.cap.max_recv_wr = queue->queue_size + 1;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	init_attr.cap.max_recv_sge = 1;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	init_attr.cap.max_send_sge = 1 + dev->num_inline_segments;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	init_attr.sq_sig_type = IB_SIGNAL_REQ_WR;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	init_attr.qp_type = IB_QPT_RC;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	init_attr.send_cq = queue->ib_cq;	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	init_attr.recv_cq = queue->ib_cq;	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	if (queue->pi_support)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		init_attr.create_flags |= IB_QP_CREATE_INTEGRITY_EN;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	init_attr.qp_context = queue;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = rdma_create_qp(queue->cm_id, dev->pd, &init_attr);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */

	queue->qp = queue->cm_id->qp;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_exit_request(struct blk_mq_tag_set *set,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		struct request *rq, unsigned int hctx_idx)	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_request *req = blk_mq_rq_to_pdu(rq);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	kfree(req->sqe.data);	/* [한국어] 커널 메모리 생명주기 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_rdma_init_request(struct blk_mq_tag_set *set,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		struct request *rq, unsigned int hctx_idx,	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
		unsigned int numa_node)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_ctrl *ctrl = to_rdma_ctrl(set->driver_data);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct nvme_rdma_request *req = blk_mq_rq_to_pdu(rq);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	int queue_idx = (set == &ctrl->tag_set) ? hctx_idx + 1 : 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_queue *queue = &ctrl->queues[queue_idx];	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	nvme_req(rq)->ctrl = &ctrl->ctrl;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	req->sqe.data = kzalloc_obj(struct nvme_command);	/* [한국어] 커널 메모리 생명주기 */
	if (!req->sqe.data)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return -ENOMEM;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	/* metadata nvme_rdma_sgl struct is located after command's data SGL */
	if (queue->pi_support)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		req->metadata_sgl = (void *)nvme_req(rq) +	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			sizeof(struct nvme_rdma_request) +	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
			NVME_RDMA_DATA_SGL_SIZE;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	req->queue = queue;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_req(rq)->cmd = req->sqe.data;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_rdma_init_hctx(struct blk_mq_hw_ctx *hctx, void *data,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		unsigned int hctx_idx)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_ctrl *ctrl = to_rdma_ctrl(data);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct nvme_rdma_queue *queue = &ctrl->queues[hctx_idx + 1];	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	BUG_ON(hctx_idx >= ctrl->ctrl.queue_count);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	hctx->driver_data = queue;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_rdma_init_admin_hctx(struct blk_mq_hw_ctx *hctx, void *data,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		unsigned int hctx_idx)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_ctrl *ctrl = to_rdma_ctrl(data);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct nvme_rdma_queue *queue = &ctrl->queues[0];	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	BUG_ON(hctx_idx != 0);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	hctx->driver_data = queue;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_free_dev(struct kref *ref)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_device *ndev =	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		container_of(ref, struct nvme_rdma_device, ref);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	mutex_lock(&device_list_mutex);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
	list_del(&ndev->entry);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	mutex_unlock(&device_list_mutex);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */

	ib_dealloc_pd(ndev->pd);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	kfree(ndev);	/* [한국어] 커널 메모리 생명주기 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_dev_put(struct nvme_rdma_device *dev)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	kref_put(&dev->ref, nvme_rdma_free_dev);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_rdma_dev_get(struct nvme_rdma_device *dev)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return kref_get_unless_zero(&dev->ref);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static struct nvme_rdma_device *	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
nvme_rdma_find_get_device(struct rdma_cm_id *cm_id)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_device *ndev;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	mutex_lock(&device_list_mutex);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
	list_for_each_entry(ndev, &device_list, entry) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (ndev->dev->node_guid == cm_id->device->node_guid &&	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		    nvme_rdma_dev_get(ndev))	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
			goto out_unlock;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ndev = kzalloc_obj(*ndev);	/* [한국어] 커널 메모리 생명주기 */
	if (!ndev)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_err;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	ndev->dev = cm_id->device;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	kref_init(&ndev->ref);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ndev->pd = ib_alloc_pd(ndev->dev,	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
		register_always ? 0 : IB_PD_UNSAFE_GLOBAL_RKEY);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (IS_ERR(ndev->pd))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_free_dev;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	if (!(ndev->dev->attrs.device_cap_flags &	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
	      IB_DEVICE_MEM_MGT_EXTENSIONS)) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		dev_err(&ndev->dev->dev,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"Memory registrations not supported.\n");	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_free_pd;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ndev->num_inline_segments = min(NVME_RDMA_MAX_INLINE_SEGMENTS,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
					ndev->dev->attrs.max_send_sge - 1);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	list_add(&ndev->entry, &device_list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
out_unlock:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	mutex_unlock(&device_list_mutex);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
	return ndev;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

out_free_pd:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ib_dealloc_pd(ndev->pd);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
out_free_dev:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	kfree(ndev);	/* [한국어] 커널 메모리 생명주기 */
out_err:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	mutex_unlock(&device_list_mutex);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
	return NULL;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_free_cq(struct nvme_rdma_queue *queue)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (nvme_rdma_poll_queue(queue))	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		ib_free_cq(queue->ib_cq);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ib_cq_pool_put(queue->ib_cq, queue->cq_size);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_destroy_queue_ib(struct nvme_rdma_queue *queue)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_device *dev;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct ib_device *ibdev;	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */

	if (!test_and_clear_bit(NVME_RDMA_Q_TR_READY, &queue->flags))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	dev = queue->device;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ibdev = dev->dev;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (queue->pi_support)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ib_mr_pool_destroy(queue->qp, &queue->qp->sig_mrs);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	ib_mr_pool_destroy(queue->qp, &queue->qp->rdma_mrs);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */

	/*
	 * The cm_id object might have been destroyed during RDMA connection
	 * establishment error flow to avoid getting other cma events, thus
	 * the destruction of the QP shouldn't use rdma_cm API.
	 */
	ib_destroy_qp(queue->qp);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	nvme_rdma_free_cq(queue);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	nvme_rdma_free_ring(ibdev, queue->rsp_ring, queue->queue_size,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
			sizeof(struct nvme_completion), DMA_FROM_DEVICE);	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	nvme_rdma_dev_put(dev);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_rdma_get_max_fr_pages(struct ib_device *ibdev, bool pi_support)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	u32 max_page_list_len;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (pi_support)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		max_page_list_len = ibdev->attrs.max_pi_fast_reg_page_list_len;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		max_page_list_len = ibdev->attrs.max_fast_reg_page_list_len;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return min_t(u32, NVME_RDMA_MAX_SEGMENTS, max_page_list_len - 1);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_rdma_create_cq(struct ib_device *ibdev,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		struct nvme_rdma_queue *queue)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int ret, comp_vector, idx = nvme_rdma_queue_idx(queue);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	/*
	 * Spread I/O queues completion vectors according their queue index.
	 * Admin queues can always go on completion vector 0.
	 */
	comp_vector = (idx == 0 ? idx : idx - 1) % ibdev->num_comp_vectors;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/* Polling queues need direct cq polling context */
	if (nvme_rdma_poll_queue(queue))	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		queue->ib_cq = ib_alloc_cq(ibdev, queue, queue->cq_size,	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
					   comp_vector, IB_POLL_DIRECT);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		queue->ib_cq = ib_cq_pool_get(ibdev, queue->cq_size,	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
					      comp_vector, IB_POLL_SOFTIRQ);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (IS_ERR(queue->ib_cq)) {	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
		ret = PTR_ERR(queue->ib_cq);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
		return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_rdma_create_queue_ib(struct nvme_rdma_queue *queue)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct ib_device *ibdev;	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	const int send_wr_factor = 3;			/* MR, SEND, INV */	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	const int cq_factor = send_wr_factor + 1;	/* + RECV */	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int ret, pages_per_mr;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	queue->device = nvme_rdma_find_get_device(queue->cm_id);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	if (!queue->device) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(queue->cm_id->device->dev.parent,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"no client data found!\n");	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return -ECONNREFUSED;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ibdev = queue->device->dev;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/* +1 for ib_drain_qp */
	queue->cq_size = cq_factor * queue->queue_size + 1;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = nvme_rdma_create_cq(ibdev, queue);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_put_dev;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	ret = nvme_rdma_create_qp(queue, send_wr_factor);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_destroy_ib_cq;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	queue->rsp_ring = nvme_rdma_alloc_ring(ibdev, queue->queue_size,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
			sizeof(struct nvme_completion), DMA_FROM_DEVICE);	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	if (!queue->rsp_ring) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = -ENOMEM;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_destroy_qp;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/*
	 * Currently we don't use SG_GAPS MR's so if the first entry is
	 * misaligned we'll end up using two entries for a single data page,
	 * so one additional entry is required.
	 */
	pages_per_mr = nvme_rdma_get_max_fr_pages(ibdev, queue->pi_support) + 1;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	ret = ib_mr_pool_init(queue->qp, &queue->qp->rdma_mrs,	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
			      queue->queue_size,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			      IB_MR_TYPE_MEM_REG,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			      pages_per_mr, 0);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"failed to initialize MR pool sized %d for QID %d\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			queue->queue_size, nvme_rdma_queue_idx(queue));	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		goto out_destroy_ring;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (queue->pi_support) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = ib_mr_pool_init(queue->qp, &queue->qp->sig_mrs,	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
				      queue->queue_size, IB_MR_TYPE_INTEGRITY,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				      pages_per_mr, pages_per_mr);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (ret) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				"failed to initialize PI MR pool sized %d for QID %d\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				queue->queue_size, nvme_rdma_queue_idx(queue));	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
			goto out_destroy_mr_pool;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	set_bit(NVME_RDMA_Q_TR_READY, &queue->flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

out_destroy_mr_pool:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ib_mr_pool_destroy(queue->qp, &queue->qp->rdma_mrs);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
out_destroy_ring:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_rdma_free_ring(ibdev, queue->rsp_ring, queue->queue_size,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
			    sizeof(struct nvme_completion), DMA_FROM_DEVICE);	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
out_destroy_qp:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	rdma_destroy_qp(queue->cm_id);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
out_destroy_ib_cq:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_rdma_free_cq(queue);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
out_put_dev:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_rdma_dev_put(queue->device);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_rdma_alloc_queue(struct nvme_rdma_ctrl *ctrl,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		int idx, size_t queue_size)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_queue *queue;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct sockaddr *src_addr = NULL;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	queue = &ctrl->queues[idx];	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	mutex_init(&queue->queue_lock);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
	queue->ctrl = ctrl;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (idx && ctrl->ctrl.max_integrity_segments)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		queue->pi_support = true;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		queue->pi_support = false;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	init_completion(&queue->cm_done);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (idx > 0)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		queue->cmnd_capsule_len = ctrl->ctrl.ioccsz * 16;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		queue->cmnd_capsule_len = sizeof(struct nvme_command);	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	queue->queue_size = queue_size;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	queue->cm_id = rdma_create_id(&init_net, nvme_rdma_cm_handler, queue,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
			RDMA_PS_TCP, IB_QPT_RC);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (IS_ERR(queue->cm_id)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_info(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"failed to create CM ID: %ld\n", PTR_ERR(queue->cm_id));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ret = PTR_ERR(queue->cm_id);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_destroy_mutex;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (ctrl->ctrl.opts->mask & NVMF_OPT_HOST_TRADDR)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		src_addr = (struct sockaddr *)&ctrl->src_addr;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	queue->cm_error = -ETIMEDOUT;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ret = rdma_resolve_addr(queue->cm_id, src_addr,	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
			(struct sockaddr *)&ctrl->addr,	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
			NVME_RDMA_CM_TIMEOUT_MS);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_info(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"rdma_resolve_addr failed (%d).\n", ret);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
		goto out_destroy_cm_id;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = nvme_rdma_wait_for_cm(queue);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	if (ret) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_info(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"rdma connection establishment failed (%d)\n", ret);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_destroy_cm_id;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	set_bit(NVME_RDMA_Q_ALLOCATED, &queue->flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

out_destroy_cm_id:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	rdma_destroy_id(queue->cm_id);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	nvme_rdma_destroy_queue_ib(queue);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
out_destroy_mutex:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	mutex_destroy(&queue->queue_lock);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void __nvme_rdma_stop_queue(struct nvme_rdma_queue *queue)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	rdma_disconnect(queue->cm_id);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	ib_drain_qp(queue->qp);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_stop_queue(struct nvme_rdma_queue *queue)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (!test_bit(NVME_RDMA_Q_ALLOCATED, &queue->flags))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	mutex_lock(&queue->queue_lock);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
	if (test_and_clear_bit(NVME_RDMA_Q_LIVE, &queue->flags))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		__nvme_rdma_stop_queue(queue);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	mutex_unlock(&queue->queue_lock);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_free_queue(struct nvme_rdma_queue *queue)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (!test_and_clear_bit(NVME_RDMA_Q_ALLOCATED, &queue->flags))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	rdma_destroy_id(queue->cm_id);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	nvme_rdma_destroy_queue_ib(queue);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	mutex_destroy(&queue->queue_lock);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_free_io_queues(struct nvme_rdma_ctrl *ctrl)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int i;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	for (i = 1; i < ctrl->ctrl.queue_count; i++)	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		nvme_rdma_free_queue(&ctrl->queues[i]);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_stop_io_queues(struct nvme_rdma_ctrl *ctrl)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int i;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	for (i = 1; i < ctrl->ctrl.queue_count; i++)	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		nvme_rdma_stop_queue(&ctrl->queues[i]);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_rdma_start_queue(struct nvme_rdma_ctrl *ctrl, int idx)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_queue *queue = &ctrl->queues[idx];	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (idx)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = nvmf_connect_io_queue(&ctrl->ctrl, idx);	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ret = nvmf_connect_admin_queue(&ctrl->ctrl);	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */

	if (!ret) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		set_bit(NVME_RDMA_Q_LIVE, &queue->flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	} else {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (test_bit(NVME_RDMA_Q_ALLOCATED, &queue->flags))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			__nvme_rdma_stop_queue(queue);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		dev_info(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"failed to connect queue: %d ret=%d\n", idx, ret);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_rdma_start_io_queues(struct nvme_rdma_ctrl *ctrl,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
				     int first, int last)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int i, ret = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	for (i = first; i < last; i++) {	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		ret = nvme_rdma_start_queue(ctrl, i);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			goto out_stop_queues;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

out_stop_queues:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	for (i--; i >= first; i--)	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		nvme_rdma_stop_queue(&ctrl->queues[i]);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_rdma_alloc_io_queues(struct nvme_rdma_ctrl *ctrl)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvmf_ctrl_options *opts = ctrl->ctrl.opts;	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
	unsigned int nr_io_queues;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int i, ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	nr_io_queues = nvmf_nr_io_queues(opts);	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
	ret = nvme_set_queue_count(&ctrl->ctrl, &nr_io_queues);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	if (nr_io_queues == 0) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"unable to set any I/O queues\n");	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return -ENOMEM;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ctrl->ctrl.queue_count = nr_io_queues + 1;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	dev_info(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		"creating %d I/O queues.\n", nr_io_queues);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	nvmf_set_io_queues(opts, nr_io_queues, ctrl->io_queues);	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
	for (i = 1; i < ctrl->ctrl.queue_count; i++) {	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		ret = nvme_rdma_alloc_queue(ctrl, i,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
				ctrl->ctrl.sqsize + 1);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			goto out_free_queues;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

out_free_queues:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	for (i--; i >= 1; i--)	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		nvme_rdma_free_queue(&ctrl->queues[i]);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_rdma_alloc_tag_set(struct nvme_ctrl *ctrl)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	unsigned int cmd_size = sizeof(struct nvme_rdma_request) +	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
				NVME_RDMA_DATA_SGL_SIZE;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (ctrl->max_integrity_segments)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		cmd_size += sizeof(struct nvme_rdma_sgl) +	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
			    NVME_RDMA_METADATA_SGL_SIZE;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return nvme_alloc_io_tag_set(ctrl, &to_rdma_ctrl(ctrl)->tag_set,	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
			&nvme_rdma_mq_ops,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
			ctrl->opts->nr_poll_queues ? HCTX_MAX_TYPES : 2,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			cmd_size);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_destroy_admin_queue(struct nvme_rdma_ctrl *ctrl)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ctrl->async_event_sqe.data) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		cancel_work_sync(&ctrl->ctrl.async_event_work);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_rdma_free_qe(ctrl->device->dev, &ctrl->async_event_sqe,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
				sizeof(struct nvme_command), DMA_TO_DEVICE);	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
		ctrl->async_event_sqe.data = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_rdma_free_queue(&ctrl->queues[0]);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_rdma_configure_admin_queue(struct nvme_rdma_ctrl *ctrl,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		bool new)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	bool pi_capable = false;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int error;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	error = nvme_rdma_alloc_queue(ctrl, 0, NVME_AQ_DEPTH);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	if (error)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return error;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	ctrl->device = ctrl->queues[0].device;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ctrl->ctrl.numa_node = ibdev_to_node(ctrl->device->dev);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/* T10-PI support */
	if (ctrl->device->dev->attrs.kernel_cap_flags &	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
	    IBK_INTEGRITY_HANDOVER)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		pi_capable = true;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ctrl->max_fr_pages = nvme_rdma_get_max_fr_pages(ctrl->device->dev,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
							pi_capable);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/*
	 * Bind the async event SQE DMA mapping to the admin queue lifetime.
	 * It's safe, since any change in the underlying RDMA device will issue
	 * error recovery and queue re-creation.
	 */
	error = nvme_rdma_alloc_qe(ctrl->device->dev, &ctrl->async_event_sqe,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
			sizeof(struct nvme_command), DMA_TO_DEVICE);	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	if (error)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_free_queue;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	if (new) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		error = nvme_alloc_admin_tag_set(&ctrl->ctrl,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				&ctrl->admin_tag_set, &nvme_rdma_admin_mq_ops,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
				sizeof(struct nvme_rdma_request) +	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
				NVME_RDMA_DATA_SGL_SIZE);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (error)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			goto out_free_async_qe;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	error = nvme_rdma_start_queue(ctrl, 0);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	if (error)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_remove_admin_tag_set;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	error = nvme_enable_ctrl(&ctrl->ctrl);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
	if (error)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_stop_queue;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	ctrl->ctrl.max_segments = ctrl->max_fr_pages;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ctrl->ctrl.max_hw_sectors = ctrl->max_fr_pages << (ilog2(SZ_4K) - 9);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (pi_capable)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ctrl->ctrl.max_integrity_segments = ctrl->max_fr_pages;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ctrl->ctrl.max_integrity_segments = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	nvme_unquiesce_admin_queue(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	error = nvme_init_ctrl_finish(&ctrl->ctrl, false);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
	if (error)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_quiesce_queue;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

out_quiesce_queue:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_quiesce_admin_queue(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	blk_sync_queue(ctrl->ctrl.admin_q);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
out_stop_queue:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_rdma_stop_queue(&ctrl->queues[0]);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	nvme_cancel_admin_tagset(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
out_remove_admin_tag_set:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (new)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_remove_admin_tag_set(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
out_free_async_qe:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ctrl->async_event_sqe.data) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_rdma_free_qe(ctrl->device->dev, &ctrl->async_event_sqe,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
			sizeof(struct nvme_command), DMA_TO_DEVICE);	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
		ctrl->async_event_sqe.data = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
out_free_queue:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_rdma_free_queue(&ctrl->queues[0]);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	return error;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_rdma_configure_io_queues(struct nvme_rdma_ctrl *ctrl, bool new)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int ret, nr_queues;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = nvme_rdma_alloc_io_queues(ctrl);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	if (new) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = nvme_rdma_alloc_tag_set(&ctrl->ctrl);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			goto out_free_io_queues;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/*
	 * Only start IO queues for which we have allocated the tagset
	 * and limited it to the available queues. On reconnects, the
	 * queue number might have changed.
	 */
	nr_queues = min(ctrl->tag_set.nr_hw_queues + 1, ctrl->ctrl.queue_count);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ret = nvme_rdma_start_io_queues(ctrl, 1, nr_queues);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_cleanup_tagset;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	if (!new) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_start_freeze(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_unquiesce_io_queues(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (!nvme_wait_freeze_timeout(&ctrl->ctrl, NVME_IO_TIMEOUT)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			/*
			 * If we timed out waiting for freeze we are likely to
			 * be stuck.  Fail the controller initialization just
			 * to be safe.
			 */
			ret = -ENODEV;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvme_unfreeze(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			goto out_wait_freeze_timed_out;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		blk_mq_update_nr_hw_queues(ctrl->ctrl.tagset,	/* [한국어] blk-mq — 태그·hctx·타임아웃·맵·완료 연동 */
			ctrl->ctrl.queue_count - 1);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_unfreeze(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/*
	 * If the number of queues has increased (reconnect case)
	 * start all new queues now.
	 */
	ret = nvme_rdma_start_io_queues(ctrl, nr_queues,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
					ctrl->tag_set.nr_hw_queues + 1);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_wait_freeze_timed_out;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

out_wait_freeze_timed_out:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_quiesce_io_queues(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_sync_io_queues(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_rdma_stop_io_queues(ctrl);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
out_cleanup_tagset:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_cancel_tagset(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (new)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_remove_io_tag_set(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
out_free_io_queues:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_rdma_free_io_queues(ctrl);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_teardown_admin_queue(struct nvme_rdma_ctrl *ctrl,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		bool remove)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_quiesce_admin_queue(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	blk_sync_queue(ctrl->ctrl.admin_q);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_rdma_stop_queue(&ctrl->queues[0]);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	nvme_cancel_admin_tagset(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (remove) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_unquiesce_admin_queue(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_remove_admin_tag_set(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_rdma_destroy_admin_queue(ctrl);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_teardown_io_queues(struct nvme_rdma_ctrl *ctrl,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		bool remove)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ctrl->ctrl.queue_count > 1) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_quiesce_io_queues(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_sync_io_queues(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_rdma_stop_io_queues(ctrl);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		nvme_cancel_tagset(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (remove) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			nvme_unquiesce_io_queues(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvme_remove_io_tag_set(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_rdma_free_io_queues(ctrl);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_stop_ctrl(struct nvme_ctrl *nctrl)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_ctrl *ctrl = to_rdma_ctrl(nctrl);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	flush_work(&ctrl->err_work);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	cancel_delayed_work_sync(&ctrl->reconnect_work);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_free_ctrl(struct nvme_ctrl *nctrl)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_ctrl *ctrl = to_rdma_ctrl(nctrl);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	if (list_empty(&ctrl->list))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto free_ctrl;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	mutex_lock(&nvme_rdma_ctrl_mutex);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	list_del(&ctrl->list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	mutex_unlock(&nvme_rdma_ctrl_mutex);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	nvmf_free_options(nctrl->opts);	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
free_ctrl:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	kfree(ctrl->queues);	/* [한국어] 커널 메모리 생명주기 */
	kfree(ctrl);	/* [한국어] 커널 메모리 생명주기 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_reconnect_or_remove(struct nvme_rdma_ctrl *ctrl,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
					  int status)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	enum nvme_ctrl_state state = nvme_ctrl_state(&ctrl->ctrl);	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	/* If we are resetting/deleting then do nothing */
	if (state != NVME_CTRL_CONNECTING) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		WARN_ON_ONCE(state == NVME_CTRL_NEW || state == NVME_CTRL_LIVE);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (nvmf_should_reconnect(&ctrl->ctrl, status)) {	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
		dev_info(ctrl->ctrl.device, "Reconnecting in %d seconds...\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			ctrl->ctrl.opts->reconnect_delay);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		queue_delayed_work(nvme_wq, &ctrl->reconnect_work,	/* [한국어] 워크큐 — 송수신/재연결/복구 비동기 실행 */
				ctrl->ctrl.opts->reconnect_delay * HZ);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	} else {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_delete_ctrl(&ctrl->ctrl);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_rdma_setup_ctrl(struct nvme_rdma_ctrl *ctrl, bool new)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	bool changed;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	u16 max_queue_size;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = nvme_rdma_configure_admin_queue(ctrl, new);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	if (ctrl->ctrl.icdoff) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = -EOPNOTSUPP;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		dev_err(ctrl->ctrl.device, "icdoff is not supported!\n");	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto destroy_admin;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!(ctrl->ctrl.sgls & NVME_CTRL_SGLS_KSDBDS)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = -EOPNOTSUPP;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		dev_err(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"Mandatory keyed sgls are not supported!\n");	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto destroy_admin;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (ctrl->ctrl.opts->queue_size > ctrl->ctrl.sqsize + 1) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_warn(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"queue_size %zu > ctrl sqsize %u, clamping down\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			ctrl->ctrl.opts->queue_size, ctrl->ctrl.sqsize + 1);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (ctrl->ctrl.max_integrity_segments)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		max_queue_size = NVME_RDMA_MAX_METADATA_QUEUE_SIZE;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		max_queue_size = NVME_RDMA_MAX_QUEUE_SIZE;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (ctrl->ctrl.sqsize + 1 > max_queue_size) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_warn(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			 "ctrl sqsize %u > max queue size %u, clamping down\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			 ctrl->ctrl.sqsize + 1, max_queue_size);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ctrl->ctrl.sqsize = max_queue_size - 1;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (ctrl->ctrl.sqsize + 1 > ctrl->ctrl.maxcmd) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_warn(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"sqsize %u > ctrl maxcmd %u, clamping down\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			ctrl->ctrl.sqsize + 1, ctrl->ctrl.maxcmd);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ctrl->ctrl.sqsize = ctrl->ctrl.maxcmd - 1;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (ctrl->ctrl.sgls & NVME_CTRL_SGLS_SAOS)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ctrl->use_inline_data = true;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (ctrl->ctrl.queue_count > 1) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = nvme_rdma_configure_io_queues(ctrl, new);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			goto destroy_admin;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	changed = nvme_change_ctrl_state(&ctrl->ctrl, NVME_CTRL_LIVE);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
	if (!changed) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		/*
		 * state change failure is ok if we started ctrl delete,
		 * unless we're during creation of a new controller to
		 * avoid races with teardown flow.
		 */
		enum nvme_ctrl_state state = nvme_ctrl_state(&ctrl->ctrl);	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

		WARN_ON_ONCE(state != NVME_CTRL_DELETING &&	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			     state != NVME_CTRL_DELETING_NOIO);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		WARN_ON_ONCE(new);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ret = -EINVAL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto destroy_io;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	nvme_start_ctrl(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

destroy_io:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ctrl->ctrl.queue_count > 1) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_quiesce_io_queues(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_sync_io_queues(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_rdma_stop_io_queues(ctrl);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		nvme_cancel_tagset(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (new)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			nvme_remove_io_tag_set(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_rdma_free_io_queues(ctrl);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
destroy_admin:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_stop_keep_alive(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_rdma_teardown_admin_queue(ctrl, new);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_reconnect_ctrl_work(struct work_struct *work)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_ctrl *ctrl = container_of(to_delayed_work(work),	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
			struct nvme_rdma_ctrl, reconnect_work);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	++ctrl->ctrl.nr_reconnects;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = nvme_rdma_setup_ctrl(ctrl, false);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto requeue;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	dev_info(ctrl->ctrl.device, "Successfully reconnected (%d attempts)\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			ctrl->ctrl.nr_reconnects);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ctrl->ctrl.nr_reconnects = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

requeue:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	dev_info(ctrl->ctrl.device, "Failed reconnect attempt %d/%d\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		 ctrl->ctrl.nr_reconnects, ctrl->ctrl.opts->max_reconnects);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_rdma_reconnect_or_remove(ctrl, ret);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_error_recovery_work(struct work_struct *work)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_ctrl *ctrl = container_of(work,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
			struct nvme_rdma_ctrl, err_work);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	nvme_stop_keep_alive(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	flush_work(&ctrl->ctrl.async_event_work);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_rdma_teardown_io_queues(ctrl, false);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	nvme_unquiesce_io_queues(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_rdma_teardown_admin_queue(ctrl, false);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	nvme_unquiesce_admin_queue(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_auth_stop(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!nvme_change_ctrl_state(&ctrl->ctrl, NVME_CTRL_CONNECTING)) {	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
		/* state change failure is ok if we started ctrl delete */
		enum nvme_ctrl_state state = nvme_ctrl_state(&ctrl->ctrl);	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

		WARN_ON_ONCE(state != NVME_CTRL_DELETING &&	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			     state != NVME_CTRL_DELETING_NOIO);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	nvme_rdma_reconnect_or_remove(ctrl, 0);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_error_recovery(struct nvme_rdma_ctrl *ctrl)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (!nvme_change_ctrl_state(&ctrl->ctrl, NVME_CTRL_RESETTING))	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	dev_warn(ctrl->ctrl.device, "starting error recovery\n");	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	queue_work(nvme_reset_wq, &ctrl->err_work);	/* [한국어] 워크큐 — 송수신/재연결/복구 비동기 실행 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_end_request(struct nvme_rdma_request *req)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct request *rq = blk_mq_rq_from_pdu(req);	/* [한국어] blk-mq — 태그·hctx·타임아웃·맵·완료 연동 */

	if (!refcount_dec_and_test(&req->ref))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	if (!nvme_try_complete_req(rq, req->status, req->result))	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
		nvme_rdma_complete_rq(rq);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_wr_error(struct ib_cq *cq, struct ib_wc *wc,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		const char *op)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_queue *queue = wc->qp->qp_context;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct nvme_rdma_ctrl *ctrl = queue->ctrl;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	if (nvme_ctrl_state(&ctrl->ctrl) == NVME_CTRL_LIVE)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_info(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			     "%s for CQE 0x%p failed with status %s (%d)\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			     op, wc->wr_cqe,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			     ib_wc_status_msg(wc->status), wc->status);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	nvme_rdma_error_recovery(ctrl);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_memreg_done(struct ib_cq *cq, struct ib_wc *wc)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (unlikely(wc->status != IB_WC_SUCCESS))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_rdma_wr_error(cq, wc, "MEMREG");	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_inv_rkey_done(struct ib_cq *cq, struct ib_wc *wc)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_request *req =	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		container_of(wc->wr_cqe, struct nvme_rdma_request, reg_cqe);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	if (unlikely(wc->status != IB_WC_SUCCESS))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_rdma_wr_error(cq, wc, "LOCAL_INV");	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_rdma_end_request(req);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_rdma_inv_rkey(struct nvme_rdma_queue *queue,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		struct nvme_rdma_request *req)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct ib_send_wr wr = {	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
		.opcode		    = IB_WR_LOCAL_INV,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		.next		    = NULL,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		.num_sge	    = 0,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		.send_flags	    = IB_SEND_SIGNALED,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		.ex.invalidate_rkey = req->mr->rkey,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	req->reg_cqe.done = nvme_rdma_inv_rkey_done;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	wr.wr_cqe = &req->reg_cqe;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return ib_post_send(queue->qp, &wr, NULL);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_dma_unmap_req(struct ib_device *ibdev, struct request *rq)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_request *req = blk_mq_rq_to_pdu(rq);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	if (blk_integrity_rq(rq)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ib_dma_unmap_sg(ibdev, req->metadata_sgl->sg_table.sgl,	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
				req->metadata_sgl->nents, rq_dma_dir(rq));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		sg_free_table_chained(&req->metadata_sgl->sg_table,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				      NVME_INLINE_METADATA_SG_CNT);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ib_dma_unmap_sg(ibdev, req->data_sgl.sg_table.sgl, req->data_sgl.nents,	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
			rq_dma_dir(rq));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	sg_free_table_chained(&req->data_sgl.sg_table, NVME_INLINE_SG_CNT);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_unmap_data(struct nvme_rdma_queue *queue,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		struct request *rq)	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_request *req = blk_mq_rq_to_pdu(rq);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct nvme_rdma_device *dev = queue->device;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct ib_device *ibdev = dev->dev;	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	struct list_head *pool = &queue->qp->rdma_mrs;	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */

	if (!blk_rq_nr_phys_segments(rq))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	if (req->use_sig_mr)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		pool = &queue->qp->sig_mrs;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (req->mr) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ib_mr_pool_put(queue->qp, pool, req->mr);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
		req->mr = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	nvme_rdma_dma_unmap_req(ibdev, rq);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_rdma_set_sg_null(struct nvme_command *c)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_keyed_sgl_desc *sg = &c->common.dptr.ksgl;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	sg->addr = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	put_unaligned_le24(0, sg->length);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	put_unaligned_le32(0, sg->key);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	sg->type = NVME_KEY_SGL_FMT_DATA_DESC << 4;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_rdma_map_sg_inline(struct nvme_rdma_queue *queue,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		struct nvme_rdma_request *req, struct nvme_command *c,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		int count)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_sgl_desc *sg = &c->common.dptr.sgl;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct ib_sge *sge = &req->sge[1];	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	struct scatterlist *sgl;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	u32 len = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int i;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	for_each_sg(req->data_sgl.sg_table.sgl, sgl, count, i) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		sge->addr = sg_dma_address(sgl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		sge->length = sg_dma_len(sgl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		sge->lkey = queue->device->pd->local_dma_lkey;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		len += sge->length;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		sge++;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	sg->addr = cpu_to_le64(queue->ctrl->ctrl.icdoff);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	sg->length = cpu_to_le32(len);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	sg->type = (NVME_SGL_FMT_DATA_DESC << 4) | NVME_SGL_FMT_OFFSET;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	req->num_sge += count;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_rdma_map_sg_single(struct nvme_rdma_queue *queue,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		struct nvme_rdma_request *req, struct nvme_command *c)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_keyed_sgl_desc *sg = &c->common.dptr.ksgl;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	sg->addr = cpu_to_le64(sg_dma_address(req->data_sgl.sg_table.sgl));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	put_unaligned_le24(sg_dma_len(req->data_sgl.sg_table.sgl), sg->length);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	put_unaligned_le32(queue->device->pd->unsafe_global_rkey, sg->key);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	sg->type = NVME_KEY_SGL_FMT_DATA_DESC << 4;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_rdma_map_sg_fr(struct nvme_rdma_queue *queue,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		struct nvme_rdma_request *req, struct nvme_command *c,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		int count)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_keyed_sgl_desc *sg = &c->common.dptr.ksgl;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	int nr;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	req->mr = ib_mr_pool_get(queue->qp, &queue->qp->rdma_mrs);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	if (WARN_ON_ONCE(!req->mr))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return -EAGAIN;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	/*
	 * Align the MR to a 4K page size to match the ctrl page size and
	 * the block virtual boundary.
	 */
	nr = ib_map_mr_sg(req->mr, req->data_sgl.sg_table.sgl, count, NULL,	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
			  SZ_4K);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (unlikely(nr < count)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ib_mr_pool_put(queue->qp, &queue->qp->rdma_mrs, req->mr);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
		req->mr = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (nr < 0)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			return nr;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
		return -EINVAL;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ib_update_fast_reg_key(req->mr, ib_inc_rkey(req->mr->rkey));	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */

	req->reg_cqe.done = nvme_rdma_memreg_done;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	memset(&req->reg_wr, 0, sizeof(req->reg_wr));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	req->reg_wr.wr.opcode = IB_WR_REG_MR;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	req->reg_wr.wr.wr_cqe = &req->reg_cqe;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	req->reg_wr.wr.num_sge = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	req->reg_wr.mr = req->mr;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	req->reg_wr.key = req->mr->rkey;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	req->reg_wr.access = IB_ACCESS_LOCAL_WRITE |	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			     IB_ACCESS_REMOTE_READ |	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			     IB_ACCESS_REMOTE_WRITE;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	sg->addr = cpu_to_le64(req->mr->iova);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	put_unaligned_le24(req->mr->length, sg->length);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	put_unaligned_le32(req->mr->rkey, sg->key);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	sg->type = (NVME_KEY_SGL_FMT_DATA_DESC << 4) |	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			NVME_SGL_FMT_INVALIDATE;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_set_sig_domain(struct blk_integrity *bi,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		struct nvme_command *cmd, struct ib_sig_domain *domain,	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
		u16 control, u8 pi_type)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	domain->sig_type = IB_SIG_TYPE_T10_DIF;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	domain->sig.dif.bg_type = IB_T10DIF_CRC;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	domain->sig.dif.pi_interval = 1 << bi->interval_exp;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	domain->sig.dif.ref_tag = le32_to_cpu(cmd->rw.reftag);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (control & NVME_RW_PRINFO_PRCHK_REF)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		domain->sig.dif.ref_remap = true;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	domain->sig.dif.app_tag = le16_to_cpu(cmd->rw.lbat);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	domain->sig.dif.apptag_check_mask = le16_to_cpu(cmd->rw.lbatm);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	domain->sig.dif.app_escape = true;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (pi_type == NVME_NS_DPS_PI_TYPE3)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		domain->sig.dif.ref_escape = true;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_set_sig_attrs(struct blk_integrity *bi,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		struct nvme_command *cmd, struct ib_sig_attrs *sig_attrs,	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
		u8 pi_type)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	u16 control = le16_to_cpu(cmd->rw.control);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	memset(sig_attrs, 0, sizeof(*sig_attrs));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (control & NVME_RW_PRINFO_PRACT) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		/* for WRITE_INSERT/READ_STRIP no memory domain */
		sig_attrs->mem.sig_type = IB_SIG_TYPE_NONE;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_rdma_set_sig_domain(bi, cmd, &sig_attrs->wire, control,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
					 pi_type);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		/* Clear the PRACT bit since HCA will generate/verify the PI */
		control &= ~NVME_RW_PRINFO_PRACT;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		cmd->rw.control = cpu_to_le16(control);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	} else {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		/* for WRITE_PASS/READ_PASS both wire/memory domains exist */
		nvme_rdma_set_sig_domain(bi, cmd, &sig_attrs->wire, control,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
					 pi_type);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_rdma_set_sig_domain(bi, cmd, &sig_attrs->mem, control,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
					 pi_type);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_set_prot_checks(struct nvme_command *cmd, u8 *mask)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	*mask = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (le16_to_cpu(cmd->rw.control) & NVME_RW_PRINFO_PRCHK_REF)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		*mask |= IB_SIG_CHECK_REFTAG;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (le16_to_cpu(cmd->rw.control) & NVME_RW_PRINFO_PRCHK_GUARD)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		*mask |= IB_SIG_CHECK_GUARD;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_sig_done(struct ib_cq *cq, struct ib_wc *wc)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (unlikely(wc->status != IB_WC_SUCCESS))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_rdma_wr_error(cq, wc, "SIG");	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_rdma_map_sg_pi(struct nvme_rdma_queue *queue,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		struct nvme_rdma_request *req, struct nvme_command *c,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		int count, int pi_count)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_sgl *sgl = &req->data_sgl;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct ib_reg_wr *wr = &req->reg_wr;	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	struct request *rq = blk_mq_rq_from_pdu(req);	/* [한국어] blk-mq — 태그·hctx·타임아웃·맵·완료 연동 */
	struct nvme_ns *ns = rq->q->queuedata;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct bio *bio = rq->bio;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct nvme_keyed_sgl_desc *sg = &c->common.dptr.ksgl;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct blk_integrity *bi = blk_get_integrity(bio->bi_bdev->bd_disk);	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	u32 xfer_len;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int nr;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	req->mr = ib_mr_pool_get(queue->qp, &queue->qp->sig_mrs);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	if (WARN_ON_ONCE(!req->mr))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return -EAGAIN;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	nr = ib_map_mr_sg_pi(req->mr, sgl->sg_table.sgl, count, NULL,	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
			     req->metadata_sgl->sg_table.sgl, pi_count, NULL,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			     SZ_4K);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (unlikely(nr))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto mr_put;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	nvme_rdma_set_sig_attrs(bi, c, req->mr->sig_attrs, ns->head->pi_type);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	nvme_rdma_set_prot_checks(c, &req->mr->sig_attrs->check_mask);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	ib_update_fast_reg_key(req->mr, ib_inc_rkey(req->mr->rkey));	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */

	req->reg_cqe.done = nvme_rdma_sig_done;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	memset(wr, 0, sizeof(*wr));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	wr->wr.opcode = IB_WR_REG_MR_INTEGRITY;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	wr->wr.wr_cqe = &req->reg_cqe;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	wr->wr.num_sge = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	wr->wr.send_flags = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	wr->mr = req->mr;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	wr->key = req->mr->rkey;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	wr->access = IB_ACCESS_LOCAL_WRITE |	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		     IB_ACCESS_REMOTE_READ |	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		     IB_ACCESS_REMOTE_WRITE;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	sg->addr = cpu_to_le64(req->mr->iova);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	xfer_len = req->mr->length;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	/* Check if PI is added by the HW */
	if (!pi_count)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		xfer_len += (xfer_len >> bi->interval_exp) * ns->head->pi_size;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	put_unaligned_le24(xfer_len, sg->length);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	put_unaligned_le32(req->mr->rkey, sg->key);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	sg->type = NVME_KEY_SGL_FMT_DATA_DESC << 4;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

mr_put:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ib_mr_pool_put(queue->qp, &queue->qp->sig_mrs, req->mr);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	req->mr = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (nr < 0)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return nr;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	return -EINVAL;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_rdma_dma_map_req(struct ib_device *ibdev, struct request *rq,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		int *count, int *pi_count)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_request *req = blk_mq_rq_to_pdu(rq);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	req->data_sgl.sg_table.sgl = (struct scatterlist *)(req + 1);	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	ret = sg_alloc_table_chained(&req->data_sgl.sg_table,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			blk_rq_nr_phys_segments(rq), req->data_sgl.sg_table.sgl,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			NVME_INLINE_SG_CNT);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return -ENOMEM;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	req->data_sgl.nents = blk_rq_map_sg(rq, req->data_sgl.sg_table.sgl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	*count = ib_dma_map_sg(ibdev, req->data_sgl.sg_table.sgl,	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
			       req->data_sgl.nents, rq_dma_dir(rq));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (unlikely(*count <= 0)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = -EIO;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_free_table;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (blk_integrity_rq(rq)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		req->metadata_sgl->sg_table.sgl =	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			(struct scatterlist *)(req->metadata_sgl + 1);	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
		ret = sg_alloc_table_chained(&req->metadata_sgl->sg_table,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				rq->nr_integrity_segments,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				req->metadata_sgl->sg_table.sgl,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				NVME_INLINE_METADATA_SG_CNT);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (unlikely(ret)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			ret = -ENOMEM;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			goto out_unmap_sg;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		req->metadata_sgl->nents = blk_rq_map_integrity_sg(rq,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				req->metadata_sgl->sg_table.sgl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		*pi_count = ib_dma_map_sg(ibdev,	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
					  req->metadata_sgl->sg_table.sgl,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
					  req->metadata_sgl->nents,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
					  rq_dma_dir(rq));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (unlikely(*pi_count <= 0)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			ret = -EIO;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			goto out_free_pi_table;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

out_free_pi_table:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	sg_free_table_chained(&req->metadata_sgl->sg_table,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			      NVME_INLINE_METADATA_SG_CNT);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
out_unmap_sg:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ib_dma_unmap_sg(ibdev, req->data_sgl.sg_table.sgl, req->data_sgl.nents,	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
			rq_dma_dir(rq));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
out_free_table:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	sg_free_table_chained(&req->data_sgl.sg_table, NVME_INLINE_SG_CNT);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_rdma_map_data(struct nvme_rdma_queue *queue,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		struct request *rq, struct nvme_command *c)	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_request *req = blk_mq_rq_to_pdu(rq);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct nvme_rdma_device *dev = queue->device;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct ib_device *ibdev = dev->dev;	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	int pi_count = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int count, ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	req->num_sge = 1;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	refcount_set(&req->ref, 2); /* send and recv completions */	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	c->common.flags |= NVME_CMD_SGL_METABUF;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!blk_rq_nr_phys_segments(rq))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return nvme_rdma_set_sg_null(c);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	ret = nvme_rdma_dma_map_req(ibdev, rq, &count, &pi_count);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	if (unlikely(ret))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	if (req->use_sig_mr) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = nvme_rdma_map_sg_pi(queue, req, c, count, pi_count);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		goto out;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (count <= dev->num_inline_segments) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		if (rq_data_dir(rq) == WRITE && nvme_rdma_queue_idx(queue) &&	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		    queue->ctrl->use_inline_data &&	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		    blk_rq_payload_bytes(rq) <=	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				nvme_rdma_inline_data_size(queue)) {	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
			ret = nvme_rdma_map_sg_inline(queue, req, c, count);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
			goto out;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		if (count == 1 && dev->pd->flags & IB_PD_UNSAFE_GLOBAL_RKEY) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			ret = nvme_rdma_map_sg_single(queue, req, c);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
			goto out;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = nvme_rdma_map_sg_fr(queue, req, c, count);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
out:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (unlikely(ret))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_dma_unmap_req;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

out_dma_unmap_req:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_rdma_dma_unmap_req(ibdev, rq);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_send_done(struct ib_cq *cq, struct ib_wc *wc)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_qe *qe =	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		container_of(wc->wr_cqe, struct nvme_rdma_qe, cqe);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct nvme_rdma_request *req =	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		container_of(qe, struct nvme_rdma_request, sqe);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	if (unlikely(wc->status != IB_WC_SUCCESS))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_rdma_wr_error(cq, wc, "SEND");	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_rdma_end_request(req);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_rdma_post_send(struct nvme_rdma_queue *queue,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		struct nvme_rdma_qe *qe, struct ib_sge *sge, u32 num_sge,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		struct ib_send_wr *first)	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct ib_send_wr wr;	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	sge->addr   = qe->dma;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	sge->length = sizeof(struct nvme_command);	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	sge->lkey   = queue->device->pd->local_dma_lkey;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	wr.next       = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	wr.wr_cqe     = &qe->cqe;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	wr.sg_list    = sge;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	wr.num_sge    = num_sge;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	wr.opcode     = IB_WR_SEND;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	wr.send_flags = IB_SEND_SIGNALED;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (first)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		first->next = &wr;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		first = &wr;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = ib_post_send(queue->qp, first, NULL);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	if (unlikely(ret)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			     "%s failed with error code %d\n", __func__, ret);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_rdma_post_recv(struct nvme_rdma_queue *queue,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		struct nvme_rdma_qe *qe)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct ib_recv_wr wr;	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	struct ib_sge list;	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	list.addr   = qe->dma;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	list.length = sizeof(struct nvme_completion);	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	list.lkey   = queue->device->pd->local_dma_lkey;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	qe->cqe.done = nvme_rdma_recv_done;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	wr.next     = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	wr.wr_cqe   = &qe->cqe;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	wr.sg_list  = &list;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	wr.num_sge  = 1;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = ib_post_recv(queue->qp, &wr, NULL);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	if (unlikely(ret)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"%s failed with error code %d\n", __func__, ret);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static struct blk_mq_tags *nvme_rdma_tagset(struct nvme_rdma_queue *queue)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	u32 queue_idx = nvme_rdma_queue_idx(queue);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	if (queue_idx == 0)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return queue->ctrl->admin_tag_set.tags[queue_idx];	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	return queue->ctrl->tag_set.tags[queue_idx - 1];	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_async_done(struct ib_cq *cq, struct ib_wc *wc)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (unlikely(wc->status != IB_WC_SUCCESS))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_rdma_wr_error(cq, wc, "ASYNC");	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_submit_async_event(struct nvme_ctrl *arg)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_ctrl *ctrl = to_rdma_ctrl(arg);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct nvme_rdma_queue *queue = &ctrl->queues[0];	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct ib_device *dev = queue->device->dev;	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	struct nvme_rdma_qe *sqe = &ctrl->async_event_sqe;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct nvme_command *cmd = sqe->data;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct ib_sge sge;	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ib_dma_sync_single_for_cpu(dev, sqe->dma, sizeof(*cmd), DMA_TO_DEVICE);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */

	memset(cmd, 0, sizeof(*cmd));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	cmd->common.opcode = nvme_admin_async_event;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	cmd->common.command_id = NVME_AQ_BLK_MQ_DEPTH;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	cmd->common.flags |= NVME_CMD_SGL_METABUF;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_rdma_set_sg_null(cmd);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	sqe->cqe.done = nvme_rdma_async_done;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	ib_dma_sync_single_for_device(dev, sqe->dma, sizeof(*cmd),	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
			DMA_TO_DEVICE);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = nvme_rdma_post_send(queue, sqe, &sge, 1, NULL);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	WARN_ON_ONCE(ret);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_process_nvme_rsp(struct nvme_rdma_queue *queue,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		struct nvme_completion *cqe, struct ib_wc *wc)	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct request *rq;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct nvme_rdma_request *req;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	rq = nvme_find_rq(nvme_rdma_tagset(queue), cqe->command_id);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	if (!rq) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"got bad command_id %#x on QP %#x\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			cqe->command_id, queue->qp->qp_num);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_rdma_error_recovery(queue->ctrl);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	req = blk_mq_rq_to_pdu(rq);	/* [한국어] blk-mq — 태그·hctx·타임아웃·맵·완료 연동 */

	req->status = cqe->status;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	req->result = cqe->result;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (wc->wc_flags & IB_WC_WITH_INVALIDATE) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		if (unlikely(!req->mr ||	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			     wc->ex.invalidate_rkey != req->mr->rkey)) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				"Bogus remote invalidation for rkey %#x\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				req->mr ? req->mr->rkey : 0);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvme_rdma_error_recovery(queue->ctrl);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	} else if (req->mr) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		ret = nvme_rdma_inv_rkey(queue, req);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		if (unlikely(ret < 0)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				"Queueing INV WR for rkey %#x failed (%d)\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				req->mr->rkey, ret);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvme_rdma_error_recovery(queue->ctrl);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		/* the local invalidation completion will end the request */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	nvme_rdma_end_request(req);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_recv_done(struct ib_cq *cq, struct ib_wc *wc)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_qe *qe =	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		container_of(wc->wr_cqe, struct nvme_rdma_qe, cqe);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct nvme_rdma_queue *queue = wc->qp->qp_context;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct ib_device *ibdev = queue->device->dev;	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	struct nvme_completion *cqe = qe->data;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	const size_t len = sizeof(struct nvme_completion);	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	if (unlikely(wc->status != IB_WC_SUCCESS)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_rdma_wr_error(cq, wc, "RECV");	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/* sanity checking for received data length */
	if (unlikely(wc->byte_len < len)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"Unexpected nvme completion length(%d)\n", wc->byte_len);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_rdma_error_recovery(queue->ctrl);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ib_dma_sync_single_for_cpu(ibdev, qe->dma, len, DMA_FROM_DEVICE);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	/*
	 * AEN requests are special as they don't time out and can
	 * survive any kind of queue freeze and often don't respond to
	 * aborts.  We don't even bother to allocate a struct request
	 * for them but rather special case them here.
	 */
	if (unlikely(nvme_is_aen_req(nvme_rdma_queue_idx(queue),	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
				     cqe->command_id)))	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_complete_async_event(&queue->ctrl->ctrl, cqe->status,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				&cqe->result);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_rdma_process_nvme_rsp(queue, cqe, wc);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	ib_dma_sync_single_for_device(ibdev, qe->dma, len, DMA_FROM_DEVICE);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */

	nvme_rdma_post_recv(queue, qe);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_rdma_conn_established(struct nvme_rdma_queue *queue)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int ret, i;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	for (i = 0; i < queue->queue_size; i++) {	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		ret = nvme_rdma_post_recv(queue, &queue->rsp_ring[i]);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_rdma_conn_rejected(struct nvme_rdma_queue *queue,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		struct rdma_cm_event *ev)	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct rdma_cm_id *cm_id = queue->cm_id;	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	int status = ev->status;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	const char *rej_msg;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	const struct nvme_rdma_cm_rej *rej_data;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	u8 rej_data_len;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	rej_msg = rdma_reject_msg(cm_id, status);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	rej_data = rdma_consumer_reject_data(cm_id, ev, &rej_data_len);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */

	if (rej_data && rej_data_len >= sizeof(u16)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		u16 sts = le16_to_cpu(rej_data->sts);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		      "Connect rejected: status %d (%s) nvme status %d (%s).\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		      status, rej_msg, sts, nvme_rdma_cm_msg(sts));	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	} else {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"Connect rejected: status %d (%s).\n", status, rej_msg);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return -ECONNRESET;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_rdma_addr_resolved(struct nvme_rdma_queue *queue)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_ctrl *ctrl = &queue->ctrl->ctrl;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = nvme_rdma_create_queue_ib(queue);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	if (ctrl->opts->tos >= 0)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		rdma_set_service_type(queue->cm_id, ctrl->opts->tos);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	ret = rdma_resolve_route(queue->cm_id, NVME_RDMA_CM_TIMEOUT_MS);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	if (ret) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(ctrl->device, "rdma_resolve_route failed (%d).\n",	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
			queue->cm_error);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_destroy_queue;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

out_destroy_queue:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_rdma_destroy_queue_ib(queue);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_rdma_route_resolved(struct nvme_rdma_queue *queue)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_ctrl *ctrl = queue->ctrl;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct rdma_conn_param param = { };	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	struct nvme_rdma_cm_req priv = { };	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	param.qp_num = queue->qp->qp_num;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	param.flow_control = 1;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	param.responder_resources = queue->device->dev->attrs.max_qp_rd_atom;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	/* maximum retry count */
	param.retry_count = 7;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	param.rnr_retry_count = 7;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	param.private_data = &priv;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	param.private_data_len = sizeof(priv);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	priv.recfmt = cpu_to_le16(NVME_RDMA_CM_FMT_1_0);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	priv.qid = cpu_to_le16(nvme_rdma_queue_idx(queue));	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	/*
	 * set the admin queue depth to the minimum size
	 * specified by the Fabrics standard.
	 */
	if (priv.qid == 0) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		priv.hrqsize = cpu_to_le16(NVME_AQ_DEPTH);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		priv.hsqsize = cpu_to_le16(NVME_AQ_DEPTH - 1);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	} else {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		/*
		 * current interpretation of the fabrics spec
		 * is at minimum you make hrqsize sqsize+1, or a
		 * 1's based representation of sqsize.
		 */
		priv.hrqsize = cpu_to_le16(queue->queue_size);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		priv.hsqsize = cpu_to_le16(queue->ctrl->ctrl.sqsize);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		/* cntlid should only be set when creating an I/O queue */
		priv.cntlid = cpu_to_le16(ctrl->ctrl.cntlid);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = rdma_connect_locked(queue->cm_id, &param);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	if (ret) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"rdma_connect_locked failed (%d).\n", ret);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
		return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_rdma_cm_handler(struct rdma_cm_id *cm_id,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		struct rdma_cm_event *ev)	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_queue *queue = cm_id->context;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	int cm_error = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	dev_dbg(queue->ctrl->ctrl.device, "%s (%d): status %d id %p\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		rdma_event_msg(ev->event), ev->event,	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
		ev->status, cm_id);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	switch (ev->event) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	case RDMA_CM_EVENT_ADDR_RESOLVED:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		cm_error = nvme_rdma_addr_resolved(queue);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	case RDMA_CM_EVENT_ROUTE_RESOLVED:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		cm_error = nvme_rdma_route_resolved(queue);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	case RDMA_CM_EVENT_ESTABLISHED:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		queue->cm_error = nvme_rdma_conn_established(queue);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		/* complete cm_done regardless of success/failure */
		complete(&queue->cm_done);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	case RDMA_CM_EVENT_REJECTED:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		cm_error = nvme_rdma_conn_rejected(queue, ev);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	case RDMA_CM_EVENT_ROUTE_ERROR:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	case RDMA_CM_EVENT_CONNECT_ERROR:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	case RDMA_CM_EVENT_UNREACHABLE:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	case RDMA_CM_EVENT_ADDR_ERROR:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		dev_dbg(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"CM error event %d\n", ev->event);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		cm_error = -ECONNRESET;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	case RDMA_CM_EVENT_DISCONNECTED:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	case RDMA_CM_EVENT_ADDR_CHANGE:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	case RDMA_CM_EVENT_TIMEWAIT_EXIT:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		dev_dbg(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"disconnect received - connection closed\n");	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_rdma_error_recovery(queue->ctrl);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	case RDMA_CM_EVENT_DEVICE_REMOVAL:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		/* device removal is handled via the ib_client API */
		break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	default:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"Unexpected RDMA CM event (%d)\n", ev->event);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_rdma_error_recovery(queue->ctrl);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (cm_error) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		queue->cm_error = cm_error;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		complete(&queue->cm_done);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_complete_timed_out(struct request *rq)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_request *req = blk_mq_rq_to_pdu(rq);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct nvme_rdma_queue *queue = req->queue;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	nvme_rdma_stop_queue(queue);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	nvmf_complete_timed_out_request(rq);	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static enum blk_eh_timer_return nvme_rdma_timeout(struct request *rq)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_request *req = blk_mq_rq_to_pdu(rq);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct nvme_rdma_queue *queue = req->queue;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct nvme_rdma_ctrl *ctrl = queue->ctrl;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct nvme_command *cmd = req->req.cmd;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	int qid = nvme_rdma_queue_idx(queue);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	dev_warn(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		 "I/O tag %d (%04x) opcode %#x (%s) QID %d timeout\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		 rq->tag, nvme_cid(rq), cmd->common.opcode,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		 nvme_fabrics_opcode_str(qid, cmd), qid);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (nvme_ctrl_state(&ctrl->ctrl) != NVME_CTRL_LIVE) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		/*
		 * If we are resetting, connecting or deleting we should
		 * complete immediately because we may block controller
		 * teardown or setup sequence
		 * - ctrl disable/shutdown fabrics requests
		 * - connect requests
		 * - initialization admin requests
		 * - I/O requests that entered after unquiescing and
		 *   the controller stopped responding
		 *
		 * All other requests should be cancelled by the error
		 * recovery work, so it's fine that we fail it here.
		 */
		nvme_rdma_complete_timed_out(rq);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		return BLK_EH_DONE;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/*
	 * LIVE state should trigger the normal error recovery which will
	 * handle completing this request.
	 */
	nvme_rdma_error_recovery(ctrl);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	return BLK_EH_RESET_TIMER;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static blk_status_t nvme_rdma_queue_rq(struct blk_mq_hw_ctx *hctx,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		const struct blk_mq_queue_data *bd)	/* [한국어] blk-mq — 태그·hctx·타임아웃·맵·완료 연동 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_ns *ns = hctx->queue->queuedata;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct nvme_rdma_queue *queue = hctx->driver_data;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct request *rq = bd->rq;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct nvme_rdma_request *req = blk_mq_rq_to_pdu(rq);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct nvme_rdma_qe *sqe = &req->sqe;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct nvme_command *c = nvme_req(rq)->cmd;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct ib_device *dev;	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	bool queue_ready = test_bit(NVME_RDMA_Q_LIVE, &queue->flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	blk_status_t ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int err;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	WARN_ON_ONCE(rq->tag < 0);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!nvme_check_ready(&queue->ctrl->ctrl, rq, queue_ready))	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
		return nvme_fail_nonready_command(&queue->ctrl->ctrl, rq);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */

	dev = queue->device->dev;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	req->sqe.dma = ib_dma_map_single(dev, req->sqe.data,	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
					 sizeof(struct nvme_command),	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
					 DMA_TO_DEVICE);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	err = ib_dma_mapping_error(dev, req->sqe.dma);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	if (unlikely(err))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return BLK_STS_RESOURCE;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	ib_dma_sync_single_for_cpu(dev, sqe->dma,	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
			sizeof(struct nvme_command), DMA_TO_DEVICE);	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	ret = nvme_setup_cmd(ns, rq);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto unmap_qe;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	nvme_start_request(rq);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */

	if (IS_ENABLED(CONFIG_BLK_DEV_INTEGRITY) &&	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
	    queue->pi_support &&	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	    (c->common.opcode == nvme_cmd_write ||	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	     c->common.opcode == nvme_cmd_read) &&	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	    nvme_ns_has_pi(ns->head))	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		req->use_sig_mr = true;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		req->use_sig_mr = false;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	err = nvme_rdma_map_data(queue, rq, c);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	if (unlikely(err < 0)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			     "Failed to map data (%d)\n", err);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto err;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	sqe->cqe.done = nvme_rdma_send_done;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	ib_dma_sync_single_for_device(dev, sqe->dma,	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
			sizeof(struct nvme_command), DMA_TO_DEVICE);	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	err = nvme_rdma_post_send(queue, sqe, req->sge, req->num_sge,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
			req->mr ? &req->reg_wr.wr : NULL);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (unlikely(err))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto err_unmap;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	return BLK_STS_OK;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

err_unmap:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_rdma_unmap_data(queue, rq);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
err:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (err == -EIO)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = nvme_host_path_error(rq);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else if (err == -ENOMEM || err == -EAGAIN)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = BLK_STS_RESOURCE;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ret = BLK_STS_IOERR;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_cleanup_cmd(rq);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
unmap_qe:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ib_dma_unmap_single(dev, req->sqe.dma, sizeof(struct nvme_command),	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
			    DMA_TO_DEVICE);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_rdma_poll(struct blk_mq_hw_ctx *hctx, struct io_comp_batch *iob)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_queue *queue = hctx->driver_data;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	return ib_process_cq_direct(queue->ib_cq, -1);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_check_pi_status(struct nvme_rdma_request *req)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct request *rq = blk_mq_rq_from_pdu(req);	/* [한국어] blk-mq — 태그·hctx·타임아웃·맵·완료 연동 */
	struct ib_mr_status mr_status;	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = ib_check_mr_status(req->mr, IB_MR_CHECK_SIG_STATUS, &mr_status);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	if (ret) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		pr_err("ib_check_mr_status failed, ret %d\n", ret);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
		nvme_req(rq)->status = NVME_SC_INVALID_PI;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (mr_status.fail_status & IB_MR_CHECK_SIG_STATUS) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		switch (mr_status.sig_err.err_type) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		case IB_SIG_BAD_GUARD:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvme_req(rq)->status = NVME_SC_GUARD_CHECK;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		case IB_SIG_BAD_REFTAG:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvme_req(rq)->status = NVME_SC_REFTAG_CHECK;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		case IB_SIG_BAD_APPTAG:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvme_req(rq)->status = NVME_SC_APPTAG_CHECK;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		pr_err("PI error found type %d expected 0x%x vs actual 0x%x\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		       mr_status.sig_err.err_type, mr_status.sig_err.expected,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		       mr_status.sig_err.actual);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_complete_rq(struct request *rq)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_request *req = blk_mq_rq_to_pdu(rq);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct nvme_rdma_queue *queue = req->queue;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct ib_device *ibdev = queue->device->dev;	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */

	if (req->use_sig_mr)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_rdma_check_pi_status(req);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	nvme_rdma_unmap_data(queue, rq);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	ib_dma_unmap_single(ibdev, req->sqe.dma, sizeof(struct nvme_command),	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
			    DMA_TO_DEVICE);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_complete_rq(rq);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_map_queues(struct blk_mq_tag_set *set)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_ctrl *ctrl = to_rdma_ctrl(set->driver_data);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	nvmf_map_queues(set, &ctrl->ctrl, ctrl->io_queues);	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static const struct blk_mq_ops nvme_rdma_mq_ops = {	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	.queue_rq	= nvme_rdma_queue_rq,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	.complete	= nvme_rdma_complete_rq,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	.init_request	= nvme_rdma_init_request,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	.exit_request	= nvme_rdma_exit_request,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	.init_hctx	= nvme_rdma_init_hctx,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	.timeout	= nvme_rdma_timeout,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	.map_queues	= nvme_rdma_map_queues,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	.poll		= nvme_rdma_poll,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static const struct blk_mq_ops nvme_rdma_admin_mq_ops = {	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	.queue_rq	= nvme_rdma_queue_rq,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	.complete	= nvme_rdma_complete_rq,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	.init_request	= nvme_rdma_init_request,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	.exit_request	= nvme_rdma_exit_request,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	.init_hctx	= nvme_rdma_init_admin_hctx,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	.timeout	= nvme_rdma_timeout,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_shutdown_ctrl(struct nvme_rdma_ctrl *ctrl, bool shutdown)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_rdma_teardown_io_queues(ctrl, shutdown);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	nvme_quiesce_admin_queue(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_disable_ctrl(&ctrl->ctrl, shutdown);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
	nvme_rdma_teardown_admin_queue(ctrl, shutdown);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_delete_ctrl(struct nvme_ctrl *ctrl)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_rdma_shutdown_ctrl(to_rdma_ctrl(ctrl), true);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_reset_ctrl_work(struct work_struct *work)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_ctrl *ctrl =	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		container_of(work, struct nvme_rdma_ctrl, ctrl.reset_work);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	nvme_stop_ctrl(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_rdma_shutdown_ctrl(ctrl, false);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	if (!nvme_change_ctrl_state(&ctrl->ctrl, NVME_CTRL_CONNECTING)) {	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
		/* state change failure should never happen */
		WARN_ON_ONCE(1);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = nvme_rdma_setup_ctrl(ctrl, false);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_fail;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

out_fail:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	++ctrl->ctrl.nr_reconnects;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_rdma_reconnect_or_remove(ctrl, ret);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static const struct nvme_ctrl_ops nvme_rdma_ctrl_ops = {	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	.name			= "rdma",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	.module			= THIS_MODULE,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	.flags			= NVME_F_FABRICS | NVME_F_METADATA_SUPPORTED,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	.reg_read32		= nvmf_reg_read32,	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
	.reg_read64		= nvmf_reg_read64,	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
	.reg_write32		= nvmf_reg_write32,	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
	.subsystem_reset	= nvmf_subsystem_reset,	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
	.free_ctrl		= nvme_rdma_free_ctrl,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	.submit_async_event	= nvme_rdma_submit_async_event,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	.delete_ctrl		= nvme_rdma_delete_ctrl,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	.get_address		= nvmf_get_address,	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
	.stop_ctrl		= nvme_rdma_stop_ctrl,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	.get_virt_boundary	= nvme_get_virt_boundary,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

/*
 * Fails a connection request if it matches an existing controller
 * (association) with the same tuple:
 * <Host NQN, Host ID, local address, remote address, remote port, SUBSYS NQN>
 *
 * if local address is not specified in the request, it will match an
 * existing controller with all the other parameters the same and no
 * local port address specified as well.
 *
 * The ports don't need to be compared as they are intrinsically
 * already matched by the port pointers supplied.
 */
static bool	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_rdma_existing_controller(struct nvmf_ctrl_options *opts)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_ctrl *ctrl;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	bool found = false;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	mutex_lock(&nvme_rdma_ctrl_mutex);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	list_for_each_entry(ctrl, &nvme_rdma_ctrl_list, list) {	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		found = nvmf_ip_options_match(&ctrl->ctrl, opts);	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
		if (found)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	mutex_unlock(&nvme_rdma_ctrl_mutex);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	return found;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static struct nvme_rdma_ctrl *nvme_rdma_alloc_ctrl(struct device *dev,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		struct nvmf_ctrl_options *opts)	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_ctrl *ctrl;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ctrl = kzalloc_obj(*ctrl);	/* [한국어] 커널 메모리 생명주기 */
	if (!ctrl)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return ERR_PTR(-ENOMEM);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	ctrl->ctrl.opts = opts;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	INIT_LIST_HEAD(&ctrl->list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!(opts->mask & NVMF_OPT_TRSVCID)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		opts->trsvcid =	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			kstrdup(__stringify(NVME_RDMA_IP_PORT), GFP_KERNEL);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (!opts->trsvcid) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			ret = -ENOMEM;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			goto out_free_ctrl;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		opts->mask |= NVMF_OPT_TRSVCID;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = inet_pton_with_scope(&init_net, AF_UNSPEC,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			opts->traddr, opts->trsvcid, &ctrl->addr);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		pr_err("malformed address passed: %s:%s\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			opts->traddr, opts->trsvcid);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_free_ctrl;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (opts->mask & NVMF_OPT_HOST_TRADDR) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = inet_pton_with_scope(&init_net, AF_UNSPEC,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			opts->host_traddr, NULL, &ctrl->src_addr);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (ret) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			pr_err("malformed src address passed: %s\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			       opts->host_traddr);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			goto out_free_ctrl;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!opts->duplicate_connect && nvme_rdma_existing_controller(opts)) {	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		ret = -EALREADY;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_free_ctrl;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	INIT_DELAYED_WORK(&ctrl->reconnect_work,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvme_rdma_reconnect_ctrl_work);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	INIT_WORK(&ctrl->err_work, nvme_rdma_error_recovery_work);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	INIT_WORK(&ctrl->ctrl.reset_work, nvme_rdma_reset_ctrl_work);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	ctrl->ctrl.queue_count = opts->nr_io_queues + opts->nr_write_queues +	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				opts->nr_poll_queues + 1;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ctrl->ctrl.sqsize = opts->queue_size - 1;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ctrl->ctrl.kato = opts->kato;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = -ENOMEM;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ctrl->queues = kzalloc_objs(*ctrl->queues, ctrl->ctrl.queue_count);	/* [한국어] 커널 메모리 생명주기 */
	if (!ctrl->queues)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_free_ctrl;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	ret = nvme_init_ctrl(&ctrl->ctrl, dev, &nvme_rdma_ctrl_ops,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
				0 /* no quirks, we're perfect! */);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_kfree_queues;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	return ctrl;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

out_kfree_queues:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	kfree(ctrl->queues);	/* [한국어] 커널 메모리 생명주기 */
out_free_ctrl:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	kfree(ctrl);	/* [한국어] 커널 메모리 생명주기 */
	return ERR_PTR(ret);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static struct nvme_ctrl *nvme_rdma_create_ctrl(struct device *dev,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		struct nvmf_ctrl_options *opts)	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_ctrl *ctrl;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	bool changed;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ctrl = nvme_rdma_alloc_ctrl(dev, opts);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	if (IS_ERR(ctrl))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return ERR_CAST(ctrl);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	ret = nvme_add_ctrl(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_put_ctrl;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	changed = nvme_change_ctrl_state(&ctrl->ctrl, NVME_CTRL_CONNECTING);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
	WARN_ON_ONCE(!changed);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = nvme_rdma_setup_ctrl(ctrl, true);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_uninit_ctrl;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	dev_info(ctrl->ctrl.device, "new ctrl: NQN \"%s\", addr %pISpcs, hostnqn: %s\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvmf_ctrl_subsysnqn(&ctrl->ctrl), &ctrl->addr, opts->host->nqn);	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */

	mutex_lock(&nvme_rdma_ctrl_mutex);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	list_add_tail(&ctrl->list, &nvme_rdma_ctrl_list);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	mutex_unlock(&nvme_rdma_ctrl_mutex);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	return &ctrl->ctrl;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

out_uninit_ctrl:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_uninit_ctrl(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
out_put_ctrl:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_put_ctrl(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret > 0)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = -EIO;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return ERR_PTR(ret);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static struct nvmf_transport_ops nvme_rdma_transport = {	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	.name		= "rdma",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	.module		= THIS_MODULE,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	.required_opts	= NVMF_OPT_TRADDR,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	.allowed_opts	= NVMF_OPT_TRSVCID | NVMF_OPT_RECONNECT_DELAY |	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			  NVMF_OPT_HOST_TRADDR | NVMF_OPT_CTRL_LOSS_TMO |	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			  NVMF_OPT_NR_WRITE_QUEUES | NVMF_OPT_NR_POLL_QUEUES |	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			  NVMF_OPT_TOS,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	.create_ctrl	= nvme_rdma_create_ctrl,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_rdma_remove_one(struct ib_device *ib_device, void *client_data)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_ctrl *ctrl;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct nvme_rdma_device *ndev;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	bool found = false;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	mutex_lock(&device_list_mutex);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
	list_for_each_entry(ndev, &device_list, entry) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (ndev->dev == ib_device) {	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
			found = true;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	mutex_unlock(&device_list_mutex);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */

	if (!found)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	/* Delete all controllers using this device */
	mutex_lock(&nvme_rdma_ctrl_mutex);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	list_for_each_entry(ctrl, &nvme_rdma_ctrl_list, list) {	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		if (ctrl->device->dev != ib_device)	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
			continue;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_delete_ctrl(&ctrl->ctrl);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	mutex_unlock(&nvme_rdma_ctrl_mutex);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	flush_workqueue(nvme_delete_wq);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static struct ib_client nvme_rdma_ib_client = {	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	.name   = "nvme_rdma",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	.remove = nvme_rdma_remove_one	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int __init nvme_rdma_init_module(void)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = ib_register_client(&nvme_rdma_ib_client);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	ret = nvmf_register_transport(&nvme_rdma_transport);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto err_unreg_client;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

err_unreg_client:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ib_unregister_client(&nvme_rdma_ib_client);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void __exit nvme_rdma_cleanup_module(void)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_rdma_ctrl *ctrl;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	nvmf_unregister_transport(&nvme_rdma_transport);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	ib_unregister_client(&nvme_rdma_ib_client);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	mutex_lock(&nvme_rdma_ctrl_mutex);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	list_for_each_entry(ctrl, &nvme_rdma_ctrl_list, list)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		nvme_delete_ctrl(&ctrl->ctrl);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
	mutex_unlock(&nvme_rdma_ctrl_mutex);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	flush_workqueue(nvme_delete_wq);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

module_init(nvme_rdma_init_module);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
module_exit(nvme_rdma_cleanup_module);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

MODULE_DESCRIPTION("NVMe host RDMA transport driver");	/* [한국어] 모듈 경계·파라미터·심볼 공개 */
MODULE_LICENSE("GPL v2");	/* [한국어] 모듈 경계·파라미터·심볼 공개 */
