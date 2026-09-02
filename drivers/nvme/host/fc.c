// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2016 Avago Technologies.  All rights reserved.
 */

/*
 * [한국어 설명] NVMe over Fibre Channel 호스트 트랜스포트 (fc.c)
 *
 * === 파일의 역할 ===
 * FC-NVME 바인딩: LLDD(lpfc 등) 콜백과 협력해 Association/Connection LS,
 * FCP_CMD/RSP 교환으로 Admin/I/O 큐를 구현한다. rport 단위 컨트롤러, 큐별
 * exchange, LS 타임아웃·재연결, blk-mq 연동이 핵심이다.
 *
 * === 아키텍처 위치 ===
 * LLDD 가 lport/rport 등록 → 호스트가 CREATE_ASSOC/CONN LS → Connect-like
 * 큐 성립 → FCP 로 SQE/CQE 운반. fabrics 옵션·core 상태기계와 결합.
 * 실패 시 LS disconnect / rport 다운 → failfast·재연결.
 *
 * === 주요 구조 ===
 * nvme_fc_ctrl/queue/fcp_op/ls_op/lport/rport — exchange, LS 상태, ops 벡터
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt	/* [한국어] 상수/매크로 — PDU·큐·타임아웃·플래그 */
#include <linux/module.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include <linux/parser.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include <uapi/scsi/fc/fc_fs.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include <uapi/scsi/fc/fc_els.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include <linux/delay.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include <linux/overflow.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include <linux/blk-cgroup.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include "nvme.h"	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include "fabrics.h"	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include <linux/nvme-fc-driver.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include <linux/nvme-fc.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include "fc.h"	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include <scsi/scsi_transport_fc.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */

/* *************************** Data Structures/Defines ****************** */


enum nvme_fc_queue_flags {	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	NVME_FC_Q_CONNECTED = 0,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	NVME_FC_Q_LIVE,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

#define NVME_FC_DEFAULT_DEV_LOSS_TMO	60	/* seconds */	/* [한국어] 상수/매크로 — PDU·큐·타임아웃·플래그 */
#define NVME_FC_DEFAULT_RECONNECT_TMO	2	/* delay between reconnects
						 * when connected and a
						 * connection failure.
						 */

struct nvme_fc_queue {	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_fc_ctrl	*ctrl;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct device		*dev;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct blk_mq_hw_ctx	*hctx;	/* [한국어] blk-mq — 태그·hctx·타임아웃·맵·완료 연동 */
	void			*lldd_handle;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	size_t			cmnd_capsule_len;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	u32			qnum;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	u32			rqcnt;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	u32			seqno;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	u64			connection_id;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	atomic_t		csn;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	unsigned long		flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
} __aligned(sizeof(u64));	/* alignment for other things alloc'd with */	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

enum nvme_fcop_flags {	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	FCOP_FLAGS_TERMIO	= (1 << 0),	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	FCOP_FLAGS_AEN		= (1 << 1),	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

struct nvmefc_ls_req_op {	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct nvmefc_ls_req	ls_req;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	struct nvme_fc_rport	*rport;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_fc_queue	*queue;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct request		*rq;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	u32			flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	int			ls_error;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct completion	ls_done;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct list_head	lsreq_list;	/* rport->ls_req_list */	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	bool			req_queued;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

struct nvmefc_ls_rcv_op {	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct nvme_fc_rport		*rport;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvmefc_ls_rsp		*lsrsp;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	union nvmefc_ls_requests	*rqstbuf;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	union nvmefc_ls_responses	*rspbuf;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	u16				rqstdatalen;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	bool				handled;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	dma_addr_t			rspdma;	/* [한국어] DMA 매핑 — 장치가 접근할 주소 확보 */
	struct list_head		lsrcv_list;	/* rport->ls_rcv_list */	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
} __aligned(sizeof(u64));	/* alignment for other things alloc'd with */	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

enum nvme_fcpop_state {	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	FCPOP_STATE_UNINIT	= 0,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	FCPOP_STATE_IDLE	= 1,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	FCPOP_STATE_ACTIVE	= 2,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	FCPOP_STATE_ABORTED	= 3,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	FCPOP_STATE_COMPLETE	= 4,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

struct nvme_fc_fcp_op {	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_request	nreq;		/*
						 * nvme/host/core.c
						 * requires this to be
						 * the 1st element in the
						 * private structure
						 * associated with the
						 * request.
						 */
	struct nvmefc_fcp_req	fcp_req;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	struct nvme_fc_ctrl	*ctrl;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_fc_queue	*queue;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct request		*rq;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	atomic_t		state;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	u32			flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	u32			rqno;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	u32			nents;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	struct nvme_fc_cmd_iu	cmd_iu;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_fc_ersp_iu	rsp_iu;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

struct nvme_fcp_op_w_sgl {	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct nvme_fc_fcp_op	op;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct scatterlist	sgl[NVME_INLINE_SG_CNT];	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	uint8_t			priv[];	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

struct nvme_fc_lport {	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_fc_local_port	localport;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	struct ida			endp_cnt;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct list_head		port_list;	/* nvme_fc_port_list */	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct list_head		endp_list;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct device			*dev;	/* physical device for dma */	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct nvme_fc_port_template	*ops;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct kref			ref;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	atomic_t                        act_rport_cnt;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
} __aligned(sizeof(u64));	/* alignment for other things alloc'd with */	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

struct nvme_fc_rport {	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_fc_remote_port	remoteport;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	struct list_head		endp_list; /* for lport->endp_list */	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct list_head		ctrl_list;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct list_head		ls_req_list;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct list_head		ls_rcv_list;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct list_head		disc_list;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct device			*dev;	/* physical device for dma */	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct nvme_fc_lport		*lport;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	spinlock_t			lock;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct kref			ref;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	atomic_t                        act_ctrl_cnt;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	unsigned long			dev_loss_end;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct work_struct		lsrcv_work;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
} __aligned(sizeof(u64));	/* alignment for other things alloc'd with */	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

/* fc_ctrl flags values - specified as bit positions */
#define ASSOC_ACTIVE		0	/* [한국어] 상수/매크로 — PDU·큐·타임아웃·플래그 */
#define ASSOC_FAILED		1	/* [한국어] 상수/매크로 — PDU·큐·타임아웃·플래그 */
#define FCCTRL_TERMIO		2	/* [한국어] 상수/매크로 — PDU·큐·타임아웃·플래그 */

struct nvme_fc_ctrl {	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	spinlock_t		lock;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_queue	*queues;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct device		*dev;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct nvme_fc_lport	*lport;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_fc_rport	*rport;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	u32			cnum;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	bool			ioq_live;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	u64			association_id;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvmefc_ls_rcv_op	*rcv_disconn;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	struct list_head	ctrl_list;	/* rport->ctrl_list */	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	struct blk_mq_tag_set	admin_tag_set;	/* [한국어] blk-mq — 태그·hctx·타임아웃·맵·완료 연동 */
	struct blk_mq_tag_set	tag_set;	/* [한국어] blk-mq — 태그·hctx·타임아웃·맵·완료 연동 */

	struct work_struct	ioerr_work;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct delayed_work	connect_work;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	struct kref		ref;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	unsigned long		flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	u32			iocnt;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	wait_queue_head_t	ioabort_wait;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	struct nvme_fc_fcp_op	aen_ops[NVME_NR_AEN_COMMANDS];	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	struct nvme_ctrl	ctrl;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline struct nvme_fc_ctrl *	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
to_fc_ctrl(struct nvme_ctrl *ctrl)	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return container_of(ctrl, struct nvme_fc_ctrl, ctrl);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline struct nvme_fc_lport *	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
localport_to_lport(struct nvme_fc_local_port *portptr)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return container_of(portptr, struct nvme_fc_lport, localport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline struct nvme_fc_rport *	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
remoteport_to_rport(struct nvme_fc_remote_port *portptr)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return container_of(portptr, struct nvme_fc_rport, remoteport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline struct nvmefc_ls_req_op *	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
ls_req_to_lsop(struct nvmefc_ls_req *lsreq)	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return container_of(lsreq, struct nvmefc_ls_req_op, ls_req);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline struct nvme_fc_fcp_op *	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
fcp_req_to_fcp_op(struct nvmefc_fcp_req *fcpreq)	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return container_of(fcpreq, struct nvme_fc_fcp_op, fcp_req);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */



/* *************************** Globals **************************** */


static DEFINE_SPINLOCK(nvme_fc_lock);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

static LIST_HEAD(nvme_fc_lport_list);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
static DEFINE_IDA(nvme_fc_local_port_cnt);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
static DEFINE_IDA(nvme_fc_ctrl_cnt);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

/*
 * These items are short-term. They will eventually be moved into
 * a generic FC class. See comments in module init.
 */
static struct device *fc_udev_device;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

static void nvme_fc_complete_rq(struct request *rq);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

/* *********************** FC-NVME Port Management ************************ */

static void __nvme_fc_delete_hw_queue(struct nvme_fc_ctrl *,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
			struct nvme_fc_queue *, unsigned int);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

static void nvme_fc_handle_ls_rqst_work(struct work_struct *work);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */


static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_free_lport(struct kref *ref)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_lport *lport =	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		container_of(ref, struct nvme_fc_lport, ref);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	unsigned long flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	WARN_ON(lport->localport.port_state != FC_OBJSTATE_DELETED);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	WARN_ON(!list_empty(&lport->endp_list));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/* remove from transport list */
	spin_lock_irqsave(&nvme_fc_lock, flags);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	list_del(&lport->port_list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	spin_unlock_irqrestore(&nvme_fc_lock, flags);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	ida_free(&nvme_fc_local_port_cnt, lport->localport.port_num);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	ida_destroy(&lport->endp_cnt);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	put_device(lport->dev);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	kfree(lport);	/* [한국어] 커널 메모리 생명주기 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_lport_put(struct nvme_fc_lport *lport)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	kref_put(&lport->ref, nvme_fc_free_lport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_lport_get(struct nvme_fc_lport *lport)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return kref_get_unless_zero(&lport->ref);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */


static struct nvme_fc_lport *	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
nvme_fc_attach_to_unreg_lport(struct nvme_fc_port_info *pinfo,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
			struct nvme_fc_port_template *ops,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
			struct device *dev)	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_lport *lport;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	unsigned long flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	spin_lock_irqsave(&nvme_fc_lock, flags);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	list_for_each_entry(lport, &nvme_fc_lport_list, port_list) {	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		if (lport->localport.node_name != pinfo->node_name ||	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		    lport->localport.port_name != pinfo->port_name)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			continue;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		if (lport->dev != dev) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			lport = ERR_PTR(-EXDEV);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			goto out_done;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		if (lport->localport.port_state != FC_OBJSTATE_DELETED) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			lport = ERR_PTR(-EEXIST);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			goto out_done;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		if (!nvme_fc_lport_get(lport)) {	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
			/*
			 * fails if ref cnt already 0. If so,
			 * act as if lport already deleted
			 */
			lport = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			goto out_done;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		/* resume the lport */

		lport->ops = ops;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		lport->localport.port_role = pinfo->port_role;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		lport->localport.port_id = pinfo->port_id;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		lport->localport.port_state = FC_OBJSTATE_ONLINE;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		spin_unlock_irqrestore(&nvme_fc_lock, flags);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

		return lport;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	lport = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

out_done:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	spin_unlock_irqrestore(&nvme_fc_lock, flags);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	return lport;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

/**
 * nvme_fc_register_localport - transport entry point called by an
 *                              LLDD to register the existence of a NVME
 *                              host FC port.
 * @pinfo:     pointer to information about the port to be registered
 * @template:  LLDD entrypoints and operational parameters for the port
 * @dev:       physical hardware device node port corresponds to. Will be
 *             used for DMA mappings
 * @portptr:   pointer to a local port pointer. Upon success, the routine
 *             will allocate a nvme_fc_local_port structure and place its
 *             address in the local port pointer. Upon failure, local port
 *             pointer will be set to 0.
 *
 * Returns:
 * a completion status. Must be 0 upon success; a negative errno
 * (ex: -ENXIO) upon failure.
 */
int	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_register_localport(struct nvme_fc_port_info *pinfo,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
			struct nvme_fc_port_template *template,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
			struct device *dev,	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
			struct nvme_fc_local_port **portptr)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_lport *newrec;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	unsigned long flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int ret, idx;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!template->localport_delete || !template->remoteport_delete ||	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
	    !template->ls_req || !template->fcp_io ||	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	    !template->ls_abort || !template->fcp_abort ||	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	    !template->max_hw_queues || !template->max_sgl_segments ||	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	    !template->max_dif_sgl_segments || !template->dma_boundary) {	/* [한국어] DMA 매핑 — 장치가 접근할 주소 확보 */
		ret = -EINVAL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_reghost_failed;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/*
	 * look to see if there is already a localport that had been
	 * deregistered and in the process of waiting for all the
	 * references to fully be removed.  If the references haven't
	 * expired, we can simply re-enable the localport. Remoteports
	 * and controller reconnections should resume naturally.
	 */
	newrec = nvme_fc_attach_to_unreg_lport(pinfo, template, dev);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	/* found an lport, but something about its state is bad */
	if (IS_ERR(newrec)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = PTR_ERR(newrec);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_reghost_failed;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	/* found existing lport, which was resumed */
	} else if (newrec) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		*portptr = &newrec->localport;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/* nothing found - allocate a new localport struct */

	newrec = kmalloc((sizeof(*newrec) + template->local_priv_sz),	/* [한국어] 커널 메모리 생명주기 */
			 GFP_KERNEL);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (!newrec) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = -ENOMEM;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_reghost_failed;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	idx = ida_alloc(&nvme_fc_local_port_cnt, GFP_KERNEL);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	if (idx < 0) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = -ENOSPC;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_fail_kfree;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!get_device(dev) && dev) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = -ENODEV;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_ida_put;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	INIT_LIST_HEAD(&newrec->port_list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	INIT_LIST_HEAD(&newrec->endp_list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	kref_init(&newrec->ref);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	atomic_set(&newrec->act_rport_cnt, 0);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	newrec->ops = template;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	newrec->dev = dev;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ida_init(&newrec->endp_cnt);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (template->local_priv_sz)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		newrec->localport.private = &newrec[1];	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		newrec->localport.private = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	newrec->localport.node_name = pinfo->node_name;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	newrec->localport.port_name = pinfo->port_name;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	newrec->localport.port_role = pinfo->port_role;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	newrec->localport.port_id = pinfo->port_id;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	newrec->localport.port_state = FC_OBJSTATE_ONLINE;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	newrec->localport.port_num = idx;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	spin_lock_irqsave(&nvme_fc_lock, flags);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	list_add_tail(&newrec->port_list, &nvme_fc_lport_list);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	spin_unlock_irqrestore(&nvme_fc_lock, flags);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	if (dev)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dma_set_seg_boundary(dev, template->dma_boundary);	/* [한국어] DMA 매핑 — 장치가 접근할 주소 확보 */

	*portptr = &newrec->localport;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

out_ida_put:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ida_free(&nvme_fc_local_port_cnt, idx);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
out_fail_kfree:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	kfree(newrec);	/* [한국어] 커널 메모리 생명주기 */
out_reghost_failed:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	*portptr = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
EXPORT_SYMBOL_GPL(nvme_fc_register_localport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

/**
 * nvme_fc_unregister_localport - transport entry point called by an
 *                              LLDD to deregister/remove a previously
 *                              registered a NVME host FC port.
 * @portptr: pointer to the (registered) local port that is to be deregistered.
 *
 * Returns:
 * a completion status. Must be 0 upon success; a negative errno
 * (ex: -ENXIO) upon failure.
 */
int	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_unregister_localport(struct nvme_fc_local_port *portptr)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_lport *lport = localport_to_lport(portptr);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	unsigned long flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!portptr)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return -EINVAL;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	spin_lock_irqsave(&nvme_fc_lock, flags);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	if (portptr->port_state != FC_OBJSTATE_ONLINE) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		spin_unlock_irqrestore(&nvme_fc_lock, flags);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		return -EINVAL;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	portptr->port_state = FC_OBJSTATE_DELETED;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	spin_unlock_irqrestore(&nvme_fc_lock, flags);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	if (atomic_read(&lport->act_rport_cnt) == 0)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		lport->ops->localport_delete(&lport->localport);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	nvme_fc_lport_put(lport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
EXPORT_SYMBOL_GPL(nvme_fc_unregister_localport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

/*
 * TRADDR strings, per FC-NVME are fixed format:
 *   "nn-0x<16hexdigits>:pn-0x<16hexdigits>" - 43 characters
 * udev event will only differ by prefix of what field is
 * being specified:
 *    "NVMEFC_HOST_TRADDR=" or "NVMEFC_TRADDR=" - 19 max characters
 *  19 + 43 + null_fudge = 64 characters
 */
#define FCNVME_TRADDR_LENGTH		64	/* [한국어] 상수/매크로 — PDU·큐·타임아웃·플래그 */

static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_signal_discovery_scan(struct nvme_fc_lport *lport,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		struct nvme_fc_rport *rport)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	char hostaddr[FCNVME_TRADDR_LENGTH];	/* NVMEFC_HOST_TRADDR=...*/	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	char tgtaddr[FCNVME_TRADDR_LENGTH];	/* NVMEFC_TRADDR=...*/	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	char *envp[4] = { "FC_EVENT=nvmediscovery", hostaddr, tgtaddr, NULL };	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!(rport->remoteport.port_role & FC_PORT_ROLE_NVME_DISCOVERY))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	snprintf(hostaddr, sizeof(hostaddr),	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		"NVMEFC_HOST_TRADDR=nn-0x%016llx:pn-0x%016llx",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		lport->localport.node_name, lport->localport.port_name);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	snprintf(tgtaddr, sizeof(tgtaddr),	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		"NVMEFC_TRADDR=nn-0x%016llx:pn-0x%016llx",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		rport->remoteport.node_name, rport->remoteport.port_name);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	kobject_uevent_env(&fc_udev_device->kobj, KOBJ_CHANGE, envp);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_free_rport(struct kref *ref)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_rport *rport =	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		container_of(ref, struct nvme_fc_rport, ref);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_fc_lport *lport =	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
			localport_to_lport(rport->remoteport.localport);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	unsigned long flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	WARN_ON(rport->remoteport.port_state != FC_OBJSTATE_DELETED);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	WARN_ON(!list_empty(&rport->ctrl_list));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	WARN_ON(!list_empty(&rport->ls_req_list));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	WARN_ON(!list_empty(&rport->ls_rcv_list));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/* remove from lport list */
	spin_lock_irqsave(&nvme_fc_lock, flags);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	list_del(&rport->endp_list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	spin_unlock_irqrestore(&nvme_fc_lock, flags);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	WARN_ON(!list_empty(&rport->disc_list));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ida_free(&lport->endp_cnt, rport->remoteport.port_num);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	kfree(rport);	/* [한국어] 커널 메모리 생명주기 */

	nvme_fc_lport_put(lport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_rport_put(struct nvme_fc_rport *rport)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	kref_put(&rport->ref, nvme_fc_free_rport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_rport_get(struct nvme_fc_rport *rport)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return kref_get_unless_zero(&rport->ref);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_resume_controller(struct nvme_fc_ctrl *ctrl)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	switch (nvme_ctrl_state(&ctrl->ctrl)) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	case NVME_CTRL_NEW:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	case NVME_CTRL_CONNECTING:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		/*
		 * As all reconnects were suppressed, schedule a
		 * connect.
		 */
		dev_info(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"NVME-FC{%d}: connectivity re-established. "	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"Attempting reconnect\n", ctrl->cnum);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		queue_delayed_work(nvme_wq, &ctrl->connect_work, 0);	/* [한국어] 워크큐 — 송수신/재연결/복구 비동기 실행 */
		break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	case NVME_CTRL_RESETTING:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		/*
		 * Controller is already in the process of terminating the
		 * association. No need to do anything further. The reconnect
		 * step will naturally occur after the reset completes.
		 */
		break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	default:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		/* no action to take - let it delete */
		break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static struct nvme_fc_rport *	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
nvme_fc_attach_to_suspended_rport(struct nvme_fc_lport *lport,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
				struct nvme_fc_port_info *pinfo)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_rport *rport;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_fc_ctrl *ctrl;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	unsigned long flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	spin_lock_irqsave(&nvme_fc_lock, flags);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	list_for_each_entry(rport, &lport->endp_list, endp_list) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (rport->remoteport.node_name != pinfo->node_name ||	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		    rport->remoteport.port_name != pinfo->port_name)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			continue;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		if (!nvme_fc_rport_get(rport)) {	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
			rport = ERR_PTR(-ENOLCK);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			goto out_done;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		spin_unlock_irqrestore(&nvme_fc_lock, flags);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

		spin_lock_irqsave(&rport->lock, flags);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */

		/* has it been unregistered */
		if (rport->remoteport.port_state != FC_OBJSTATE_DELETED) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			/* means lldd called us twice */
			spin_unlock_irqrestore(&rport->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvme_fc_rport_put(rport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
			return ERR_PTR(-ESTALE);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		rport->remoteport.port_role = pinfo->port_role;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		rport->remoteport.port_id = pinfo->port_id;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		rport->remoteport.port_state = FC_OBJSTATE_ONLINE;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		rport->dev_loss_end = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		/*
		 * kick off a reconnect attempt on all associations to the
		 * remote port. A successful reconnects will resume i/o.
		 */
		list_for_each_entry(ctrl, &rport->ctrl_list, ctrl_list)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvme_fc_resume_controller(ctrl);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

		spin_unlock_irqrestore(&rport->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		return rport;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	rport = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

out_done:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	spin_unlock_irqrestore(&nvme_fc_lock, flags);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	return rport;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
__nvme_fc_set_dev_loss_tmo(struct nvme_fc_rport *rport,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
			struct nvme_fc_port_info *pinfo)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (pinfo->dev_loss_tmo)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		rport->remoteport.dev_loss_tmo = pinfo->dev_loss_tmo;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		rport->remoteport.dev_loss_tmo = NVME_FC_DEFAULT_DEV_LOSS_TMO;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

/**
 * nvme_fc_register_remoteport - transport entry point called by an
 *                              LLDD to register the existence of a NVME
 *                              subsystem FC port on its fabric.
 * @localport: pointer to the (registered) local port that the remote
 *             subsystem port is connected to.
 * @pinfo:     pointer to information about the port to be registered
 * @portptr:   pointer to a remote port pointer. Upon success, the routine
 *             will allocate a nvme_fc_remote_port structure and place its
 *             address in the remote port pointer. Upon failure, remote port
 *             pointer will be set to 0.
 *
 * Returns:
 * a completion status. Must be 0 upon success; a negative errno
 * (ex: -ENXIO) upon failure.
 */
int	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_register_remoteport(struct nvme_fc_local_port *localport,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
				struct nvme_fc_port_info *pinfo,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
				struct nvme_fc_remote_port **portptr)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_lport *lport = localport_to_lport(localport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_fc_rport *newrec;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	unsigned long flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int ret, idx;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!nvme_fc_lport_get(lport)) {	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		ret = -ESHUTDOWN;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_reghost_failed;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/*
	 * look to see if there is already a remoteport that is waiting
	 * for a reconnect (within dev_loss_tmo) with the same WWN's.
	 * If so, transition to it and reconnect.
	 */
	newrec = nvme_fc_attach_to_suspended_rport(lport, pinfo);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	/* found an rport, but something about its state is bad */
	if (IS_ERR(newrec)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = PTR_ERR(newrec);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_lport_put;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	/* found existing rport, which was resumed */
	} else if (newrec) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_fc_lport_put(lport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		__nvme_fc_set_dev_loss_tmo(newrec, pinfo);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_fc_signal_discovery_scan(lport, newrec);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		*portptr = &newrec->remoteport;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/* nothing found - allocate a new remoteport struct */

	newrec = kmalloc((sizeof(*newrec) + lport->ops->remote_priv_sz),	/* [한국어] 커널 메모리 생명주기 */
			 GFP_KERNEL);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (!newrec) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = -ENOMEM;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_lport_put;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	idx = ida_alloc(&lport->endp_cnt, GFP_KERNEL);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (idx < 0) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = -ENOSPC;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_kfree_rport;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	INIT_LIST_HEAD(&newrec->endp_list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	INIT_LIST_HEAD(&newrec->ctrl_list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	INIT_LIST_HEAD(&newrec->ls_req_list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	INIT_LIST_HEAD(&newrec->disc_list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	kref_init(&newrec->ref);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	atomic_set(&newrec->act_ctrl_cnt, 0);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	spin_lock_init(&newrec->lock);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
	newrec->remoteport.localport = &lport->localport;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	INIT_LIST_HEAD(&newrec->ls_rcv_list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	newrec->dev = lport->dev;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	newrec->lport = lport;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (lport->ops->remote_priv_sz)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		newrec->remoteport.private = &newrec[1];	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		newrec->remoteport.private = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	newrec->remoteport.port_role = pinfo->port_role;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	newrec->remoteport.node_name = pinfo->node_name;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	newrec->remoteport.port_name = pinfo->port_name;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	newrec->remoteport.port_id = pinfo->port_id;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	newrec->remoteport.port_state = FC_OBJSTATE_ONLINE;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	newrec->remoteport.port_num = idx;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	__nvme_fc_set_dev_loss_tmo(newrec, pinfo);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	INIT_WORK(&newrec->lsrcv_work, nvme_fc_handle_ls_rqst_work);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	spin_lock_irqsave(&nvme_fc_lock, flags);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	list_add_tail(&newrec->endp_list, &lport->endp_list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	spin_unlock_irqrestore(&nvme_fc_lock, flags);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	nvme_fc_signal_discovery_scan(lport, newrec);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	*portptr = &newrec->remoteport;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

out_kfree_rport:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	kfree(newrec);	/* [한국어] 커널 메모리 생명주기 */
out_lport_put:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_fc_lport_put(lport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
out_reghost_failed:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	*portptr = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
EXPORT_SYMBOL_GPL(nvme_fc_register_remoteport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

static int	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_abort_lsops(struct nvme_fc_rport *rport)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvmefc_ls_req_op *lsop;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	unsigned long flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

restart:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	spin_lock_irqsave(&rport->lock, flags);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */

	list_for_each_entry(lsop, &rport->ls_req_list, lsreq_list) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (!(lsop->flags & FCOP_FLAGS_TERMIO)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			lsop->flags |= FCOP_FLAGS_TERMIO;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			spin_unlock_irqrestore(&rport->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			rport->lport->ops->ls_abort(&rport->lport->localport,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
						&rport->remoteport,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
						&lsop->ls_req);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			goto restart;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	spin_unlock_irqrestore(&rport->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_ctrl_connectivity_loss(struct nvme_fc_ctrl *ctrl)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	dev_info(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		"NVME-FC{%d}: controller connectivity lost. Awaiting "	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		"Reconnect", ctrl->cnum);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	set_bit(ASSOC_FAILED, &ctrl->flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_reset_ctrl(&ctrl->ctrl);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

/**
 * nvme_fc_unregister_remoteport - transport entry point called by an
 *                              LLDD to deregister/remove a previously
 *                              registered a NVME subsystem FC port.
 * @portptr: pointer to the (registered) remote port that is to be
 *           deregistered.
 *
 * Returns:
 * a completion status. Must be 0 upon success; a negative errno
 * (ex: -ENXIO) upon failure.
 */
int	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_unregister_remoteport(struct nvme_fc_remote_port *portptr)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_rport *rport = remoteport_to_rport(portptr);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_fc_ctrl *ctrl;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	unsigned long flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!portptr)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return -EINVAL;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	spin_lock_irqsave(&rport->lock, flags);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */

	if (portptr->port_state != FC_OBJSTATE_ONLINE) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		spin_unlock_irqrestore(&rport->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return -EINVAL;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	portptr->port_state = FC_OBJSTATE_DELETED;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	rport->dev_loss_end = jiffies + (portptr->dev_loss_tmo * HZ);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	list_for_each_entry(ctrl, &rport->ctrl_list, ctrl_list) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		/* if dev_loss_tmo==0, dev loss is immediate */
		if (!portptr->dev_loss_tmo) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			dev_warn(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				"NVME-FC{%d}: controller connectivity lost.\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				ctrl->cnum);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvme_delete_ctrl(&ctrl->ctrl);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
		} else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvme_fc_ctrl_connectivity_loss(ctrl);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	spin_unlock_irqrestore(&rport->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	nvme_fc_abort_lsops(rport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	if (atomic_read(&rport->act_ctrl_cnt) == 0)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		rport->lport->ops->remoteport_delete(portptr);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/*
	 * release the reference, which will allow, if all controllers
	 * go away, which should only occur after dev_loss_tmo occurs,
	 * for the rport to be torn down.
	 */
	nvme_fc_rport_put(rport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
EXPORT_SYMBOL_GPL(nvme_fc_unregister_remoteport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

/**
 * nvme_fc_rescan_remoteport - transport entry point called by an
 *                              LLDD to request a nvme device rescan.
 * @remoteport: pointer to the (registered) remote port that is to be
 *              rescanned.
 *
 * Returns: N/A
 */
void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_rescan_remoteport(struct nvme_fc_remote_port *remoteport)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_rport *rport = remoteport_to_rport(remoteport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	nvme_fc_signal_discovery_scan(rport->lport, rport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
EXPORT_SYMBOL_GPL(nvme_fc_rescan_remoteport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

int	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_set_remoteport_devloss(struct nvme_fc_remote_port *portptr,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
			u32 dev_loss_tmo)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_rport *rport = remoteport_to_rport(portptr);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	unsigned long flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	spin_lock_irqsave(&rport->lock, flags);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */

	if (portptr->port_state != FC_OBJSTATE_ONLINE) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		spin_unlock_irqrestore(&rport->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return -EINVAL;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/* a dev_loss_tmo of 0 (immediate) is allowed to be set */
	rport->remoteport.dev_loss_tmo = dev_loss_tmo;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	spin_unlock_irqrestore(&rport->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
EXPORT_SYMBOL_GPL(nvme_fc_set_remoteport_devloss);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */


/* *********************** FC-NVME DMA Handling **************************** */

/*
 * The fcloop device passes in a NULL device pointer. Real LLD's will
 * pass in a valid device pointer. If NULL is passed to the dma mapping
 * routines, depending on the platform, it may or may not succeed, and
 * may crash.
 *
 * As such:
 * Wrap all the dma routines and check the dev pointer.
 *
 * If simple mappings (return just a dma address, we'll noop them,
 * returning a dma address of 0.
 *
 * On more complex mappings (dma_map_sg), a pseudo routine fills
 * in the scatter list, setting all dma addresses to 0.
 */

static inline dma_addr_t	/* [한국어] DMA 매핑 — 장치가 접근할 주소 확보 */
fc_dma_map_single(struct device *dev, void *ptr, size_t size,	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
		enum dma_data_direction dir)	/* [한국어] DMA 매핑 — 장치가 접근할 주소 확보 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return dev ? dma_map_single(dev, ptr, size, dir) : (dma_addr_t)0L;	/* [한국어] DMA 매핑 — 장치가 접근할 주소 확보 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline int	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
fc_dma_mapping_error(struct device *dev, dma_addr_t dma_addr)	/* [한국어] DMA 매핑 — 장치가 접근할 주소 확보 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return dev ? dma_mapping_error(dev, dma_addr) : 0;	/* [한국어] DMA 매핑 — 장치가 접근할 주소 확보 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
fc_dma_unmap_single(struct device *dev, dma_addr_t addr, size_t size,	/* [한국어] DMA 매핑 — 장치가 접근할 주소 확보 */
	enum dma_data_direction dir)	/* [한국어] DMA 매핑 — 장치가 접근할 주소 확보 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (dev)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dma_unmap_single(dev, addr, size, dir);	/* [한국어] DMA 매핑 — 장치가 접근할 주소 확보 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
fc_dma_sync_single_for_cpu(struct device *dev, dma_addr_t addr, size_t size,	/* [한국어] DMA 매핑 — 장치가 접근할 주소 확보 */
		enum dma_data_direction dir)	/* [한국어] DMA 매핑 — 장치가 접근할 주소 확보 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (dev)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dma_sync_single_for_cpu(dev, addr, size, dir);	/* [한국어] DMA 매핑 — 장치가 접근할 주소 확보 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
fc_dma_sync_single_for_device(struct device *dev, dma_addr_t addr, size_t size,	/* [한국어] DMA 매핑 — 장치가 접근할 주소 확보 */
		enum dma_data_direction dir)	/* [한국어] DMA 매핑 — 장치가 접근할 주소 확보 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (dev)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dma_sync_single_for_device(dev, addr, size, dir);	/* [한국어] DMA 매핑 — 장치가 접근할 주소 확보 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

/* pseudo dma_map_sg call */
static int	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
fc_map_sg(struct scatterlist *sg, int nents)	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct scatterlist *s;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	int i;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	WARN_ON(nents == 0 || sg[0].length == 0);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	for_each_sg(sg, s, nents, i) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		s->dma_address = 0L;	/* [한국어] DMA 매핑 — 장치가 접근할 주소 확보 */
#ifdef CONFIG_NEED_SG_DMA_LENGTH	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		s->dma_length = s->length;	/* [한국어] DMA 매핑 — 장치가 접근할 주소 확보 */
#endif	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return nents;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline int	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
fc_dma_map_sg(struct device *dev, struct scatterlist *sg, int nents,	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
		enum dma_data_direction dir)	/* [한국어] DMA 매핑 — 장치가 접근할 주소 확보 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return dev ? dma_map_sg(dev, sg, nents, dir) : fc_map_sg(sg, nents);	/* [한국어] DMA 매핑 — 장치가 접근할 주소 확보 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
fc_dma_unmap_sg(struct device *dev, struct scatterlist *sg, int nents,	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
		enum dma_data_direction dir)	/* [한국어] DMA 매핑 — 장치가 접근할 주소 확보 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (dev)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dma_unmap_sg(dev, sg, nents, dir);	/* [한국어] DMA 매핑 — 장치가 접근할 주소 확보 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

/* *********************** FC-NVME LS Handling **************************** */

static void nvme_fc_ctrl_put(struct nvme_fc_ctrl *);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
static int nvme_fc_ctrl_get(struct nvme_fc_ctrl *);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

static void nvme_fc_error_recovery(struct nvme_fc_ctrl *ctrl, char *errmsg);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
__nvme_fc_finish_ls_req(struct nvmefc_ls_req_op *lsop)	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_rport *rport = lsop->rport;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvmefc_ls_req *lsreq = &lsop->ls_req;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	unsigned long flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	spin_lock_irqsave(&rport->lock, flags);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */

	if (!lsop->req_queued) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		spin_unlock_irqrestore(&rport->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	list_del(&lsop->lsreq_list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	lsop->req_queued = false;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	spin_unlock_irqrestore(&rport->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	fc_dma_unmap_single(rport->dev, lsreq->rqstdma,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				  (lsreq->rqstlen + lsreq->rsplen),	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				  DMA_BIDIRECTIONAL);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	nvme_fc_rport_put(rport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
__nvme_fc_send_ls_req(struct nvme_fc_rport *rport,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		struct nvmefc_ls_req_op *lsop,	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
		void (*done)(struct nvmefc_ls_req *req, int status))	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvmefc_ls_req *lsreq = &lsop->ls_req;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	unsigned long flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int ret = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (rport->remoteport.port_state != FC_OBJSTATE_ONLINE)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return -ECONNREFUSED;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	if (!nvme_fc_rport_get(rport))	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		return -ESHUTDOWN;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	lsreq->done = done;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	lsop->rport = rport;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	lsop->req_queued = false;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	INIT_LIST_HEAD(&lsop->lsreq_list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	init_completion(&lsop->ls_done);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	lsreq->rqstdma = fc_dma_map_single(rport->dev, lsreq->rqstaddr,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				  lsreq->rqstlen + lsreq->rsplen,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				  DMA_BIDIRECTIONAL);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (fc_dma_mapping_error(rport->dev, lsreq->rqstdma)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = -EFAULT;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_putrport;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	lsreq->rspdma = lsreq->rqstdma + lsreq->rqstlen;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	spin_lock_irqsave(&rport->lock, flags);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */

	list_add_tail(&lsop->lsreq_list, &rport->ls_req_list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	lsop->req_queued = true;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	spin_unlock_irqrestore(&rport->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = rport->lport->ops->ls_req(&rport->lport->localport,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
					&rport->remoteport, lsreq);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_unlink;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

out_unlink:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	lsop->ls_error = ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	spin_lock_irqsave(&rport->lock, flags);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
	lsop->req_queued = false;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	list_del(&lsop->lsreq_list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	spin_unlock_irqrestore(&rport->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	fc_dma_unmap_single(rport->dev, lsreq->rqstdma,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				  (lsreq->rqstlen + lsreq->rsplen),	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				  DMA_BIDIRECTIONAL);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
out_putrport:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_fc_rport_put(rport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_send_ls_req_done(struct nvmefc_ls_req *lsreq, int status)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvmefc_ls_req_op *lsop = ls_req_to_lsop(lsreq);	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	lsop->ls_error = status;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	complete(&lsop->ls_done);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_send_ls_req(struct nvme_fc_rport *rport, struct nvmefc_ls_req_op *lsop)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvmefc_ls_req *lsreq = &lsop->ls_req;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct fcnvme_ls_rjt *rjt = lsreq->rspaddr;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = __nvme_fc_send_ls_req(rport, lsop, nvme_fc_send_ls_req_done);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	if (!ret) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		/*
		 * No timeout/not interruptible as we need the struct
		 * to exist until the lldd calls us back. Thus mandate
		 * wait until driver calls back. lldd responsible for
		 * the timeout action
		 */
		wait_for_completion(&lsop->ls_done);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		__nvme_fc_finish_ls_req(lsop);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		ret = lsop->ls_error;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	/* ACC or RJT payload ? */
	if (rjt->w0.ls_cmd == FCNVME_LS_RJT)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return -ENXIO;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_send_ls_req_async(struct nvme_fc_rport *rport,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		struct nvmefc_ls_req_op *lsop,	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
		void (*done)(struct nvmefc_ls_req *req, int status))	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	/* don't wait for completion */

	return __nvme_fc_send_ls_req(rport, lsop, done);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_connect_admin_queue(struct nvme_fc_ctrl *ctrl,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_fc_queue *queue, u16 qsize, u16 ersp_ratio)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvmefc_ls_req_op *lsop;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct nvmefc_ls_req *lsreq;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct fcnvme_ls_cr_assoc_rqst *assoc_rqst;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct fcnvme_ls_cr_assoc_acc *assoc_acc;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	unsigned long flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int ret, fcret = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	lsop = kzalloc((sizeof(*lsop) +	/* [한국어] 커널 메모리 생명주기 */
			 sizeof(*assoc_rqst) + sizeof(*assoc_acc) +	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			 ctrl->lport->ops->lsrqst_priv_sz), GFP_KERNEL);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (!lsop) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_info(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"NVME-FC{%d}: send Create Association failed: ENOMEM\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			ctrl->cnum);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ret = -ENOMEM;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_no_memory;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	assoc_rqst = (struct fcnvme_ls_cr_assoc_rqst *)&lsop[1];	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	assoc_acc = (struct fcnvme_ls_cr_assoc_acc *)&assoc_rqst[1];	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	lsreq = &lsop->ls_req;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ctrl->lport->ops->lsrqst_priv_sz)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		lsreq->private = &assoc_acc[1];	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		lsreq->private = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	assoc_rqst->w0.ls_cmd = FCNVME_LS_CREATE_ASSOCIATION;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	assoc_rqst->desc_list_len =	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			cpu_to_be32(sizeof(struct fcnvme_lsdesc_cr_assoc_cmd));	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	assoc_rqst->assoc_cmd.desc_tag =	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			cpu_to_be32(FCNVME_LSDESC_CREATE_ASSOC_CMD);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	assoc_rqst->assoc_cmd.desc_len =	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			fcnvme_lsdesc_len(	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				sizeof(struct fcnvme_lsdesc_cr_assoc_cmd));	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	assoc_rqst->assoc_cmd.ersp_ratio = cpu_to_be16(ersp_ratio);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	assoc_rqst->assoc_cmd.sqsize = cpu_to_be16(qsize - 1);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	/* Linux supports only Dynamic controllers */
	assoc_rqst->assoc_cmd.cntlid = cpu_to_be16(0xffff);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	uuid_copy(&assoc_rqst->assoc_cmd.hostid, &ctrl->ctrl.opts->host->id);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	strscpy(assoc_rqst->assoc_cmd.hostnqn, ctrl->ctrl.opts->host->nqn,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		sizeof(assoc_rqst->assoc_cmd.hostnqn));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	strscpy(assoc_rqst->assoc_cmd.subnqn, ctrl->ctrl.opts->subsysnqn,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		sizeof(assoc_rqst->assoc_cmd.subnqn));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	lsop->queue = queue;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	lsreq->rqstaddr = assoc_rqst;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	lsreq->rqstlen = sizeof(*assoc_rqst);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	lsreq->rspaddr = assoc_acc;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	lsreq->rsplen = sizeof(*assoc_acc);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	lsreq->timeout = NVME_FC_LS_TIMEOUT_SEC;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = nvme_fc_send_ls_req(ctrl->rport, lsop);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_free_buffer;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	/* process connect LS completion */

	/* validate the ACC response */
	if (assoc_acc->hdr.w0.ls_cmd != FCNVME_LS_ACC)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		fcret = VERR_LSACC;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else if (assoc_acc->hdr.desc_list_len !=	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			fcnvme_lsdesc_len(	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				sizeof(struct fcnvme_ls_cr_assoc_acc)))	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
		fcret = VERR_CR_ASSOC_ACC_LEN;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else if (assoc_acc->hdr.rqst.desc_tag !=	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			cpu_to_be32(FCNVME_LSDESC_RQST))	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		fcret = VERR_LSDESC_RQST;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else if (assoc_acc->hdr.rqst.desc_len !=	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			fcnvme_lsdesc_len(sizeof(struct fcnvme_lsdesc_rqst)))	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
		fcret = VERR_LSDESC_RQST_LEN;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else if (assoc_acc->hdr.rqst.w0.ls_cmd != FCNVME_LS_CREATE_ASSOCIATION)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		fcret = VERR_CR_ASSOC;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else if (assoc_acc->associd.desc_tag !=	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			cpu_to_be32(FCNVME_LSDESC_ASSOC_ID))	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		fcret = VERR_ASSOC_ID;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else if (assoc_acc->associd.desc_len !=	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			fcnvme_lsdesc_len(	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				sizeof(struct fcnvme_lsdesc_assoc_id)))	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
		fcret = VERR_ASSOC_ID_LEN;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else if (assoc_acc->connectid.desc_tag !=	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			cpu_to_be32(FCNVME_LSDESC_CONN_ID))	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		fcret = VERR_CONN_ID;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else if (assoc_acc->connectid.desc_len !=	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			fcnvme_lsdesc_len(sizeof(struct fcnvme_lsdesc_conn_id)))	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
		fcret = VERR_CONN_ID_LEN;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (fcret) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = -EBADF;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		dev_err(ctrl->dev,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"q %d Create Association LS failed: %s\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			queue->qnum, validation_errors[fcret]);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	} else {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		spin_lock_irqsave(&ctrl->lock, flags);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
		ctrl->association_id =	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			be64_to_cpu(assoc_acc->associd.association_id);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		queue->connection_id =	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			be64_to_cpu(assoc_acc->connectid.connection_id);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		set_bit(NVME_FC_Q_CONNECTED, &queue->flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		spin_unlock_irqrestore(&ctrl->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

out_free_buffer:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	kfree(lsop);	/* [한국어] 커널 메모리 생명주기 */
out_no_memory:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(ctrl->dev,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"queue %d connect admin queue failed (%d).\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			queue->qnum, ret);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_connect_queue(struct nvme_fc_ctrl *ctrl, struct nvme_fc_queue *queue,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
			u16 qsize, u16 ersp_ratio)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvmefc_ls_req_op *lsop;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct nvmefc_ls_req *lsreq;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct fcnvme_ls_cr_conn_rqst *conn_rqst;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct fcnvme_ls_cr_conn_acc *conn_acc;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	int ret, fcret = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	lsop = kzalloc((sizeof(*lsop) +	/* [한국어] 커널 메모리 생명주기 */
			 sizeof(*conn_rqst) + sizeof(*conn_acc) +	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			 ctrl->lport->ops->lsrqst_priv_sz), GFP_KERNEL);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (!lsop) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_info(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"NVME-FC{%d}: send Create Connection failed: ENOMEM\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			ctrl->cnum);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ret = -ENOMEM;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_no_memory;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	conn_rqst = (struct fcnvme_ls_cr_conn_rqst *)&lsop[1];	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	conn_acc = (struct fcnvme_ls_cr_conn_acc *)&conn_rqst[1];	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	lsreq = &lsop->ls_req;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ctrl->lport->ops->lsrqst_priv_sz)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		lsreq->private = (void *)&conn_acc[1];	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		lsreq->private = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	conn_rqst->w0.ls_cmd = FCNVME_LS_CREATE_CONNECTION;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	conn_rqst->desc_list_len = cpu_to_be32(	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				sizeof(struct fcnvme_lsdesc_assoc_id) +	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
				sizeof(struct fcnvme_lsdesc_cr_conn_cmd));	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	conn_rqst->associd.desc_tag = cpu_to_be32(FCNVME_LSDESC_ASSOC_ID);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	conn_rqst->associd.desc_len =	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			fcnvme_lsdesc_len(	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				sizeof(struct fcnvme_lsdesc_assoc_id));	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	conn_rqst->associd.association_id = cpu_to_be64(ctrl->association_id);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	conn_rqst->connect_cmd.desc_tag =	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			cpu_to_be32(FCNVME_LSDESC_CREATE_CONN_CMD);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	conn_rqst->connect_cmd.desc_len =	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			fcnvme_lsdesc_len(	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				sizeof(struct fcnvme_lsdesc_cr_conn_cmd));	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	conn_rqst->connect_cmd.ersp_ratio = cpu_to_be16(ersp_ratio);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	conn_rqst->connect_cmd.qid  = cpu_to_be16(queue->qnum);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	conn_rqst->connect_cmd.sqsize = cpu_to_be16(qsize - 1);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	lsop->queue = queue;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	lsreq->rqstaddr = conn_rqst;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	lsreq->rqstlen = sizeof(*conn_rqst);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	lsreq->rspaddr = conn_acc;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	lsreq->rsplen = sizeof(*conn_acc);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	lsreq->timeout = NVME_FC_LS_TIMEOUT_SEC;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = nvme_fc_send_ls_req(ctrl->rport, lsop);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_free_buffer;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	/* process connect LS completion */

	/* validate the ACC response */
	if (conn_acc->hdr.w0.ls_cmd != FCNVME_LS_ACC)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		fcret = VERR_LSACC;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else if (conn_acc->hdr.desc_list_len !=	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			fcnvme_lsdesc_len(sizeof(struct fcnvme_ls_cr_conn_acc)))	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
		fcret = VERR_CR_CONN_ACC_LEN;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else if (conn_acc->hdr.rqst.desc_tag != cpu_to_be32(FCNVME_LSDESC_RQST))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		fcret = VERR_LSDESC_RQST;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else if (conn_acc->hdr.rqst.desc_len !=	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			fcnvme_lsdesc_len(sizeof(struct fcnvme_lsdesc_rqst)))	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
		fcret = VERR_LSDESC_RQST_LEN;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else if (conn_acc->hdr.rqst.w0.ls_cmd != FCNVME_LS_CREATE_CONNECTION)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		fcret = VERR_CR_CONN;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else if (conn_acc->connectid.desc_tag !=	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			cpu_to_be32(FCNVME_LSDESC_CONN_ID))	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		fcret = VERR_CONN_ID;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else if (conn_acc->connectid.desc_len !=	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			fcnvme_lsdesc_len(sizeof(struct fcnvme_lsdesc_conn_id)))	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
		fcret = VERR_CONN_ID_LEN;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (fcret) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = -EBADF;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		dev_err(ctrl->dev,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"q %d Create I/O Connection LS failed: %s\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			queue->qnum, validation_errors[fcret]);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	} else {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		queue->connection_id =	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			be64_to_cpu(conn_acc->connectid.connection_id);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		set_bit(NVME_FC_Q_CONNECTED, &queue->flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

out_free_buffer:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	kfree(lsop);	/* [한국어] 커널 메모리 생명주기 */
out_no_memory:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(ctrl->dev,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"queue %d connect I/O queue failed (%d).\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			queue->qnum, ret);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_disconnect_assoc_done(struct nvmefc_ls_req *lsreq, int status)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvmefc_ls_req_op *lsop = ls_req_to_lsop(lsreq);	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	__nvme_fc_finish_ls_req(lsop);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/* fc-nvme initiator doesn't care about success or failure of cmd */

	kfree(lsop);	/* [한국어] 커널 메모리 생명주기 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

/*
 * This routine sends a FC-NVME LS to disconnect (aka terminate)
 * the FC-NVME Association.  Terminating the association also
 * terminates the FC-NVME connections (per queue, both admin and io
 * queues) that are part of the association. E.g. things are torn
 * down, and the related FC-NVME Association ID and Connection IDs
 * become invalid.
 *
 * The behavior of the fc-nvme initiator is such that its
 * understanding of the association and connections will implicitly
 * be torn down. The action is implicit as it may be due to a loss of
 * connectivity with the fc-nvme target, so you may never get a
 * response even if you tried.  As such, the action of this routine
 * is to asynchronously send the LS, ignore any results of the LS, and
 * continue on with terminating the association. If the fc-nvme target
 * is present and receives the LS, it too can tear down.
 */
static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_xmt_disconnect_assoc(struct nvme_fc_ctrl *ctrl)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct fcnvme_ls_disconnect_assoc_rqst *discon_rqst;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct fcnvme_ls_disconnect_assoc_acc *discon_acc;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct nvmefc_ls_req_op *lsop;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct nvmefc_ls_req *lsreq;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	lsop = kzalloc((sizeof(*lsop) +	/* [한국어] 커널 메모리 생명주기 */
			sizeof(*discon_rqst) + sizeof(*discon_acc) +	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			ctrl->lport->ops->lsrqst_priv_sz), GFP_KERNEL);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (!lsop) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_info(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"NVME-FC{%d}: send Disconnect Association "	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"failed: ENOMEM\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			ctrl->cnum);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	discon_rqst = (struct fcnvme_ls_disconnect_assoc_rqst *)&lsop[1];	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	discon_acc = (struct fcnvme_ls_disconnect_assoc_acc *)&discon_rqst[1];	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	lsreq = &lsop->ls_req;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ctrl->lport->ops->lsrqst_priv_sz)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		lsreq->private = (void *)&discon_acc[1];	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		lsreq->private = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	nvmefc_fmt_lsreq_discon_assoc(lsreq, discon_rqst, discon_acc,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				ctrl->association_id);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = nvme_fc_send_ls_req_async(ctrl->rport, lsop,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
				nvme_fc_disconnect_assoc_done);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		kfree(lsop);	/* [한국어] 커널 메모리 생명주기 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_xmt_ls_rsp_free(struct nvmefc_ls_rcv_op *lsop)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_rport *rport = lsop->rport;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_fc_lport *lport = rport->lport;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	unsigned long flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	spin_lock_irqsave(&rport->lock, flags);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
	list_del(&lsop->lsrcv_list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	spin_unlock_irqrestore(&rport->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	fc_dma_sync_single_for_cpu(lport->dev, lsop->rspdma,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				sizeof(*lsop->rspbuf), DMA_TO_DEVICE);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	fc_dma_unmap_single(lport->dev, lsop->rspdma,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			sizeof(*lsop->rspbuf), DMA_TO_DEVICE);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	kfree(lsop->rspbuf);	/* [한국어] 커널 메모리 생명주기 */
	kfree(lsop->rqstbuf);	/* [한국어] 커널 메모리 생명주기 */
	kfree(lsop);	/* [한국어] 커널 메모리 생명주기 */

	nvme_fc_rport_put(rport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_xmt_ls_rsp_done(struct nvmefc_ls_rsp *lsrsp)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvmefc_ls_rcv_op *lsop = lsrsp->nvme_fc_private;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	nvme_fc_xmt_ls_rsp_free(lsop);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_xmt_ls_rsp(struct nvmefc_ls_rcv_op *lsop)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_rport *rport = lsop->rport;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_fc_lport *lport = rport->lport;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct fcnvme_ls_rqst_w0 *w0 = &lsop->rqstbuf->w0;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	fc_dma_sync_single_for_device(lport->dev, lsop->rspdma,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				  sizeof(*lsop->rspbuf), DMA_TO_DEVICE);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = lport->ops->xmt_ls_rsp(&lport->localport, &rport->remoteport,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				     lsop->lsrsp);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_warn(lport->dev,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"LLDD rejected LS RSP xmt: LS %d status %d\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			w0->ls_cmd, ret);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_fc_xmt_ls_rsp_free(lsop);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static struct nvme_fc_ctrl *	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
nvme_fc_match_disconn_ls(struct nvme_fc_rport *rport,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		      struct nvmefc_ls_rcv_op *lsop)	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct fcnvme_ls_disconnect_assoc_rqst *rqst =	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
					&lsop->rqstbuf->rq_dis_assoc;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_ctrl *ctrl, *tmp, *ret = NULL;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvmefc_ls_rcv_op *oldls = NULL;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	u64 association_id = be64_to_cpu(rqst->associd.association_id);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	unsigned long flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	spin_lock_irqsave(&rport->lock, flags);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */

	list_for_each_entry_safe(ctrl, tmp, &rport->ctrl_list, ctrl_list) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (!nvme_fc_ctrl_get(ctrl))	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
			continue;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		spin_lock(&ctrl->lock);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
		if (association_id == ctrl->association_id) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			oldls = ctrl->rcv_disconn;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			ctrl->rcv_disconn = lsop;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			ret = ctrl;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		spin_unlock(&ctrl->lock);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			/* leave the ctrl get reference */
			break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		spin_unlock_irqrestore(&rport->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_fc_ctrl_put(ctrl);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		spin_lock_irqsave(&rport->lock, flags);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	spin_unlock_irqrestore(&rport->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/* transmit a response for anything that was pending */
	if (oldls) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_info(rport->lport->dev,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"NVME-FC{%d}: Multiple Disconnect Association "	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"LS's received\n", ctrl->cnum);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		/* overwrite good response with bogus failure */
		oldls->lsrsp->rsplen = nvme_fc_format_rjt(oldls->rspbuf,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
						sizeof(*oldls->rspbuf),	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
						rqst->w0.ls_cmd,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
						FCNVME_RJT_RC_UNAB,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
						FCNVME_RJT_EXP_NONE, 0);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_fc_xmt_ls_rsp(oldls);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

/*
 * returns true to mean LS handled and ls_rsp can be sent
 * returns false to defer ls_rsp xmt (will be done as part of
 *     association termination)
 */
static bool	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_ls_disconnect_assoc(struct nvmefc_ls_rcv_op *lsop)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_rport *rport = lsop->rport;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct fcnvme_ls_disconnect_assoc_rqst *rqst =	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
					&lsop->rqstbuf->rq_dis_assoc;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct fcnvme_ls_disconnect_assoc_acc *acc =	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
					&lsop->rspbuf->rsp_dis_assoc;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_ctrl *ctrl = NULL;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	int ret = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	memset(acc, 0, sizeof(*acc));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = nvmefc_vldt_lsreq_discon_assoc(lsop->rqstdatalen, rqst);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (!ret) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		/* match an active association */
		ctrl = nvme_fc_match_disconn_ls(rport, lsop);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		if (!ctrl)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			ret = VERR_NO_ASSOC;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (ret) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_info(rport->lport->dev,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"Disconnect LS failed: %s\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			validation_errors[ret]);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		lsop->lsrsp->rsplen = nvme_fc_format_rjt(acc,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
					sizeof(*acc), rqst->w0.ls_cmd,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
					(ret == VERR_NO_ASSOC) ?	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
						FCNVME_RJT_RC_INV_ASSOC :	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
						FCNVME_RJT_RC_LOGIC,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
					FCNVME_RJT_EXP_NONE, 0);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return true;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/* format an ACCept response */

	lsop->lsrsp->rsplen = sizeof(*acc);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	nvme_fc_format_rsp_hdr(acc, FCNVME_LS_ACC,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
			fcnvme_lsdesc_len(	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				sizeof(struct fcnvme_ls_disconnect_assoc_acc)),	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
			FCNVME_LS_DISCONNECT_ASSOC);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/*
	 * the transmit of the response will occur after the exchanges
	 * for the association have been ABTS'd by
	 * nvme_fc_delete_association().
	 */

	/* fail the association */
	nvme_fc_error_recovery(ctrl, "Disconnect Association LS received");	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	/* release the reference taken by nvme_fc_match_disconn_ls() */
	nvme_fc_ctrl_put(ctrl);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	return false;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

/*
 * Actual Processing routine for received FC-NVME LS Requests from the LLD
 * returns true if a response should be sent afterward, false if rsp will
 * be sent asynchronously.
 */
static bool	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_handle_ls_rqst(struct nvmefc_ls_rcv_op *lsop)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct fcnvme_ls_rqst_w0 *w0 = &lsop->rqstbuf->w0;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	bool ret = true;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	lsop->lsrsp->nvme_fc_private = lsop;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	lsop->lsrsp->rspbuf = lsop->rspbuf;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	lsop->lsrsp->rspdma = lsop->rspdma;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	lsop->lsrsp->done = nvme_fc_xmt_ls_rsp_done;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	/* Be preventative. handlers will later set to valid length */
	lsop->lsrsp->rsplen = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/*
	 * handlers:
	 *   parse request input, execute the request, and format the
	 *   LS response
	 */
	switch (w0->ls_cmd) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	case FCNVME_LS_DISCONNECT_ASSOC:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ret = nvme_fc_ls_disconnect_assoc(lsop);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	case FCNVME_LS_DISCONNECT_CONN:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		lsop->lsrsp->rsplen = nvme_fc_format_rjt(lsop->rspbuf,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
				sizeof(*lsop->rspbuf), w0->ls_cmd,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				FCNVME_RJT_RC_UNSUP, FCNVME_RJT_EXP_NONE, 0);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	case FCNVME_LS_CREATE_ASSOCIATION:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	case FCNVME_LS_CREATE_CONNECTION:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		lsop->lsrsp->rsplen = nvme_fc_format_rjt(lsop->rspbuf,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
				sizeof(*lsop->rspbuf), w0->ls_cmd,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				FCNVME_RJT_RC_LOGIC, FCNVME_RJT_EXP_NONE, 0);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	default:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		lsop->lsrsp->rsplen = nvme_fc_format_rjt(lsop->rspbuf,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
				sizeof(*lsop->rspbuf), w0->ls_cmd,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				FCNVME_RJT_RC_INVAL, FCNVME_RJT_EXP_NONE, 0);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return(ret);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_handle_ls_rqst_work(struct work_struct *work)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_rport *rport =	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		container_of(work, struct nvme_fc_rport, lsrcv_work);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct fcnvme_ls_rqst_w0 *w0;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct nvmefc_ls_rcv_op *lsop;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	unsigned long flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	bool sendrsp;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

restart:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	sendrsp = true;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	spin_lock_irqsave(&rport->lock, flags);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
	list_for_each_entry(lsop, &rport->ls_rcv_list, lsrcv_list) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (lsop->handled)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			continue;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		lsop->handled = true;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (rport->remoteport.port_state == FC_OBJSTATE_ONLINE) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			spin_unlock_irqrestore(&rport->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			sendrsp = nvme_fc_handle_ls_rqst(lsop);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		} else {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			spin_unlock_irqrestore(&rport->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			w0 = &lsop->rqstbuf->w0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			lsop->lsrsp->rsplen = nvme_fc_format_rjt(	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
						lsop->rspbuf,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
						sizeof(*lsop->rspbuf),	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
						w0->ls_cmd,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
						FCNVME_RJT_RC_UNAB,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
						FCNVME_RJT_EXP_NONE, 0);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (sendrsp)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			nvme_fc_xmt_ls_rsp(lsop);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		goto restart;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	spin_unlock_irqrestore(&rport->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
void nvme_fc_rcv_ls_req_err_msg(struct nvme_fc_lport *lport,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
				struct fcnvme_ls_rqst_w0 *w0)	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	dev_info(lport->dev, "RCV %s LS failed: No memory\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		(w0->ls_cmd <= NVME_FC_LAST_LS_CMD_VALUE) ?	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvmefc_ls_names[w0->ls_cmd] : "");	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

/**
 * nvme_fc_rcv_ls_req - transport entry point called by an LLDD
 *                       upon the reception of a NVME LS request.
 *
 * The nvme-fc layer will copy payload to an internal structure for
 * processing.  As such, upon completion of the routine, the LLDD may
 * immediately free/reuse the LS request buffer passed in the call.
 *
 * If this routine returns error, the LLDD should abort the exchange.
 *
 * @portptr:    pointer to the (registered) remote port that the LS
 *              was received from. The remoteport is associated with
 *              a specific localport.
 * @lsrsp:      pointer to a nvmefc_ls_rsp response structure to be
 *              used to reference the exchange corresponding to the LS
 *              when issuing an ls response.
 * @lsreqbuf:   pointer to the buffer containing the LS Request
 * @lsreqbuf_len: length, in bytes, of the received LS request
 */
int	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_rcv_ls_req(struct nvme_fc_remote_port *portptr,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
			struct nvmefc_ls_rsp *lsrsp,	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
			void *lsreqbuf, u32 lsreqbuf_len)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_rport *rport = remoteport_to_rport(portptr);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_fc_lport *lport = rport->lport;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct fcnvme_ls_rqst_w0 *w0 = (struct fcnvme_ls_rqst_w0 *)lsreqbuf;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct nvmefc_ls_rcv_op *lsop;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	unsigned long flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	nvme_fc_rport_get(rport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	/* validate there's a routine to transmit a response */
	if (!lport->ops->xmt_ls_rsp) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_info(lport->dev,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"RCV %s LS failed: no LLDD xmt_ls_rsp\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			(w0->ls_cmd <= NVME_FC_LAST_LS_CMD_VALUE) ?	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				nvmefc_ls_names[w0->ls_cmd] : "");	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ret = -EINVAL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_put;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (lsreqbuf_len > sizeof(union nvmefc_ls_requests)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_info(lport->dev,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"RCV %s LS failed: payload too large\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			(w0->ls_cmd <= NVME_FC_LAST_LS_CMD_VALUE) ?	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				nvmefc_ls_names[w0->ls_cmd] : "");	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ret = -E2BIG;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_put;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	lsop = kzalloc_obj(*lsop);	/* [한국어] 커널 메모리 생명주기 */
	if (!lsop) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_fc_rcv_ls_req_err_msg(lport, w0);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		ret = -ENOMEM;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_put;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	lsop->rqstbuf = kzalloc_obj(*lsop->rqstbuf);	/* [한국어] 커널 메모리 생명주기 */
	lsop->rspbuf = kzalloc_obj(*lsop->rspbuf);	/* [한국어] 커널 메모리 생명주기 */
	if (!lsop->rqstbuf || !lsop->rspbuf) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_fc_rcv_ls_req_err_msg(lport, w0);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		ret = -ENOMEM;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_free;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	lsop->rspdma = fc_dma_map_single(lport->dev, lsop->rspbuf,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
					sizeof(*lsop->rspbuf),	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
					DMA_TO_DEVICE);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (fc_dma_mapping_error(lport->dev, lsop->rspdma)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_info(lport->dev,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"RCV %s LS failed: DMA mapping failure\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			(w0->ls_cmd <= NVME_FC_LAST_LS_CMD_VALUE) ?	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				nvmefc_ls_names[w0->ls_cmd] : "");	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ret = -EFAULT;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_free;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	lsop->rport = rport;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	lsop->lsrsp = lsrsp;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	memcpy(lsop->rqstbuf, lsreqbuf, lsreqbuf_len);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	lsop->rqstdatalen = lsreqbuf_len;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	spin_lock_irqsave(&rport->lock, flags);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
	if (rport->remoteport.port_state != FC_OBJSTATE_ONLINE) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		spin_unlock_irqrestore(&rport->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ret = -ENOTCONN;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_unmap;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	list_add_tail(&lsop->lsrcv_list, &rport->ls_rcv_list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	spin_unlock_irqrestore(&rport->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	schedule_work(&rport->lsrcv_work);	/* [한국어] 워크큐 — 송수신/재연결/복구 비동기 실행 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

out_unmap:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	fc_dma_unmap_single(lport->dev, lsop->rspdma,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			sizeof(*lsop->rspbuf), DMA_TO_DEVICE);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
out_free:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	kfree(lsop->rspbuf);	/* [한국어] 커널 메모리 생명주기 */
	kfree(lsop->rqstbuf);	/* [한국어] 커널 메모리 생명주기 */
	kfree(lsop);	/* [한국어] 커널 메모리 생명주기 */
out_put:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_fc_rport_put(rport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
EXPORT_SYMBOL_GPL(nvme_fc_rcv_ls_req);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */


/* *********************** NVME Ctrl Routines **************************** */

static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
__nvme_fc_exit_request(struct nvme_fc_ctrl *ctrl,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		struct nvme_fc_fcp_op *op)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	fc_dma_unmap_single(ctrl->lport->dev, op->fcp_req.rspdma,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				sizeof(op->rsp_iu), DMA_FROM_DEVICE);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	fc_dma_unmap_single(ctrl->lport->dev, op->fcp_req.cmddma,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				sizeof(op->cmd_iu), DMA_TO_DEVICE);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	atomic_set(&op->state, FCPOP_STATE_UNINIT);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_exit_request(struct blk_mq_tag_set *set, struct request *rq,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		unsigned int hctx_idx)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_fcp_op *op = blk_mq_rq_to_pdu(rq);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	return __nvme_fc_exit_request(to_fc_ctrl(set->driver_data), op);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
__nvme_fc_abort_op(struct nvme_fc_ctrl *ctrl, struct nvme_fc_fcp_op *op)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	unsigned long flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int opstate;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	spin_lock_irqsave(&ctrl->lock, flags);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
	opstate = atomic_xchg(&op->state, FCPOP_STATE_ABORTED);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (opstate != FCPOP_STATE_ACTIVE)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		atomic_set(&op->state, opstate);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else if (test_bit(FCCTRL_TERMIO, &ctrl->flags)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		op->flags |= FCOP_FLAGS_TERMIO;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ctrl->iocnt++;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	spin_unlock_irqrestore(&ctrl->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (opstate != FCPOP_STATE_ACTIVE)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return -ECANCELED;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	ctrl->lport->ops->fcp_abort(&ctrl->lport->localport,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
					&ctrl->rport->remoteport,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
					op->queue->lldd_handle,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
					&op->fcp_req);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_abort_aen_ops(struct nvme_fc_ctrl *ctrl)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_fcp_op *aen_op = ctrl->aen_ops;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	int i;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/* ensure we've initialized the ops once */
	if (!(aen_op->flags & FCOP_FLAGS_AEN))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	for (i = 0; i < NVME_NR_AEN_COMMANDS; i++, aen_op++)	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		__nvme_fc_abort_op(ctrl, aen_op);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
__nvme_fc_fcpop_chk_teardowns(struct nvme_fc_ctrl *ctrl,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		struct nvme_fc_fcp_op *op, int opstate)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	unsigned long flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (opstate == FCPOP_STATE_ABORTED) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		spin_lock_irqsave(&ctrl->lock, flags);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
		if (test_bit(FCCTRL_TERMIO, &ctrl->flags) &&	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		    op->flags & FCOP_FLAGS_TERMIO) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			if (!--ctrl->iocnt)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
				wake_up(&ctrl->ioabort_wait);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		spin_unlock_irqrestore(&ctrl->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_ctrl_ioerr_work(struct work_struct *work)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_ctrl *ctrl =	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
			container_of(work, struct nvme_fc_ctrl, ioerr_work);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	nvme_fc_error_recovery(ctrl, "transport detected io error");	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

/*
 * nvme_fc_io_getuuid - Routine called to get the appid field
 * associated with request by the lldd
 * @req:IO request from nvme fc to driver
 * Returns: UUID if there is an appid associated with VM or
 * NULL if the user/libvirt has not set the appid to VM
 */
char *nvme_fc_io_getuuid(struct nvmefc_fcp_req *req)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_fcp_op *op = fcp_req_to_fcp_op(req);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct request *rq = op->rq;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	if (!IS_ENABLED(CONFIG_BLK_CGROUP_FC_APPID) || !rq || !rq->bio)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return NULL;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	return blkcg_get_fc_appid(rq->bio);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
EXPORT_SYMBOL_GPL(nvme_fc_io_getuuid);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_fcpio_done(struct nvmefc_fcp_req *req)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_fcp_op *op = fcp_req_to_fcp_op(req);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct request *rq = op->rq;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct nvmefc_fcp_req *freq = &op->fcp_req;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct nvme_fc_ctrl *ctrl = op->ctrl;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_fc_queue *queue = op->queue;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_completion *cqe = &op->rsp_iu.cqe;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct nvme_command *sqe = &op->cmd_iu.sqe;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	__le16 status = cpu_to_le16(NVME_SC_SUCCESS << 1);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	union nvme_result result;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	bool terminate_assoc = true;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int opstate;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/*
	 * WARNING:
	 * The current linux implementation of a nvme controller
	 * allocates a single tag set for all io queues and sizes
	 * the io queues to fully hold all possible tags. Thus, the
	 * implementation does not reference or care about the sqhd
	 * value as it never needs to use the sqhd/sqtail pointers
	 * for submission pacing.
	 *
	 * This affects the FC-NVME implementation in two ways:
	 * 1) As the value doesn't matter, we don't need to waste
	 *    cycles extracting it from ERSPs and stamping it in the
	 *    cases where the transport fabricates CQEs on successful
	 *    completions.
	 * 2) The FC-NVME implementation requires that delivery of
	 *    ERSP completions are to go back to the nvme layer in order
	 *    relative to the rsn, such that the sqhd value will always
	 *    be "in order" for the nvme layer. As the nvme layer in
	 *    linux doesn't care about sqhd, there's no need to return
	 *    them in order.
	 *
	 * Additionally:
	 * As the core nvme layer in linux currently does not look at
	 * every field in the cqe - in cases where the FC transport must
	 * fabricate a CQE, the following fields will not be set as they
	 * are not referenced:
	 *      cqe.sqid,  cqe.sqhd,  cqe.command_id
	 *
	 * Failure or error of an individual i/o, in a transport
	 * detected fashion unrelated to the nvme completion status,
	 * potentially cause the initiator and target sides to get out
	 * of sync on SQ head/tail (aka outstanding io count allowed).
	 * Per FC-NVME spec, failure of an individual command requires
	 * the connection to be terminated, which in turn requires the
	 * association to be terminated.
	 */

	opstate = atomic_xchg(&op->state, FCPOP_STATE_COMPLETE);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	fc_dma_sync_single_for_cpu(ctrl->lport->dev, op->fcp_req.rspdma,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				sizeof(op->rsp_iu), DMA_FROM_DEVICE);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (opstate == FCPOP_STATE_ABORTED)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		status = cpu_to_le16(NVME_SC_HOST_ABORTED_CMD << 1);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else if (freq->status) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		status = cpu_to_le16(NVME_SC_HOST_PATH_ERROR << 1);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		dev_info(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"NVME-FC{%d}: io failed due to lldd error %d\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			ctrl->cnum, freq->status);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/*
	 * For the linux implementation, if we have an unsuccessful
	 * status, the blk-mq layer can typically be called with the
	 * non-zero status and the content of the cqe isn't important.
	 */
	if (status)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto done;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	/*
	 * command completed successfully relative to the wire
	 * protocol. However, validate anything received and
	 * extract the status and result from the cqe (create it
	 * where necessary).
	 */

	switch (freq->rcv_rsplen) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	case 0:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	case NVME_FC_SIZEOF_ZEROS_RSP:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		/*
		 * No response payload or 12 bytes of payload (which
		 * should all be zeros) are considered successful and
		 * no payload in the CQE by the transport.
		 */
		if (freq->transferred_length !=	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		    be32_to_cpu(op->cmd_iu.data_len)) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			status = cpu_to_le16(NVME_SC_HOST_PATH_ERROR << 1);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			dev_info(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				"NVME-FC{%d}: io failed due to bad transfer "	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				"length: %d vs expected %d\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				ctrl->cnum, freq->transferred_length,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				be32_to_cpu(op->cmd_iu.data_len));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			goto done;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		result.u64 = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	case sizeof(struct nvme_fc_ersp_iu):	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		/*
		 * The ERSP IU contains a full completion with CQE.
		 * Validate ERSP IU and look at cqe.
		 */
		if (unlikely(be16_to_cpu(op->rsp_iu.iu_len) !=	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
					(freq->rcv_rsplen / 4) ||	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			     be32_to_cpu(op->rsp_iu.xfrd_len) !=	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
					freq->transferred_length ||	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			     op->rsp_iu.ersp_result ||	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			     sqe->common.command_id != cqe->command_id)) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			status = cpu_to_le16(NVME_SC_HOST_PATH_ERROR << 1);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			dev_info(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				"NVME-FC{%d}: io failed due to bad NVMe_ERSP: "	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				"iu len %d, xfr len %d vs %d, status code "	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				"%d, cmdid %d vs %d\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				ctrl->cnum, be16_to_cpu(op->rsp_iu.iu_len),	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				be32_to_cpu(op->rsp_iu.xfrd_len),	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				freq->transferred_length,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				op->rsp_iu.ersp_result,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				sqe->common.command_id,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				cqe->command_id);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			goto done;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		result = cqe->result;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		status = cqe->status;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	default:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		status = cpu_to_le16(NVME_SC_HOST_PATH_ERROR << 1);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		dev_info(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"NVME-FC{%d}: io failed due to odd NVMe_xRSP iu "	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"len %d\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			ctrl->cnum, freq->rcv_rsplen);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto done;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	terminate_assoc = false;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

done:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (op->flags & FCOP_FLAGS_AEN) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_complete_async_event(&queue->ctrl->ctrl, status, &result);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		__nvme_fc_fcpop_chk_teardowns(ctrl, op, opstate);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		atomic_set(&op->state, FCPOP_STATE_IDLE);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		op->flags = FCOP_FLAGS_AEN;	/* clear other flags */	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_fc_ctrl_put(ctrl);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		goto check_error;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	__nvme_fc_fcpop_chk_teardowns(ctrl, op, opstate);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (!nvme_try_complete_req(rq, status, result))	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
		nvme_fc_complete_rq(rq);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

check_error:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (terminate_assoc &&	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
	    nvme_ctrl_state(&ctrl->ctrl) != NVME_CTRL_RESETTING)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		queue_work(nvme_reset_wq, &ctrl->ioerr_work);	/* [한국어] 워크큐 — 송수신/재연결/복구 비동기 실행 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
__nvme_fc_init_request(struct nvme_fc_ctrl *ctrl,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		struct nvme_fc_queue *queue, struct nvme_fc_fcp_op *op,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		struct request *rq, u32 rqno)	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fcp_op_w_sgl *op_w_sgl =	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
		container_of(op, typeof(*op_w_sgl), op);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_cmd_iu *cmdiu = &op->cmd_iu;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	int ret = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	memset(op, 0, sizeof(*op));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	op->fcp_req.cmdaddr = &op->cmd_iu;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	op->fcp_req.cmdlen = sizeof(op->cmd_iu);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	op->fcp_req.rspaddr = &op->rsp_iu;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	op->fcp_req.rsplen = sizeof(op->rsp_iu);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	op->fcp_req.done = nvme_fc_fcpio_done;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	op->ctrl = ctrl;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	op->queue = queue;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	op->rq = rq;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	op->rqno = rqno;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	cmdiu->format_id = NVME_CMD_FORMAT_ID;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	cmdiu->fc_id = NVME_CMD_FC_ID;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	cmdiu->iu_len = cpu_to_be16(sizeof(*cmdiu) / sizeof(u32));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (queue->qnum)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		cmdiu->rsv_cat = fccmnd_set_cat_css(0,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
					(NVME_CC_CSS_NVM >> NVME_CC_CSS_SHIFT));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		cmdiu->rsv_cat = fccmnd_set_cat_admin(0);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	op->fcp_req.cmddma = fc_dma_map_single(ctrl->lport->dev,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				&op->cmd_iu, sizeof(op->cmd_iu), DMA_TO_DEVICE);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (fc_dma_mapping_error(ctrl->lport->dev, op->fcp_req.cmddma)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(ctrl->dev,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"FCP Op failed - cmdiu dma mapping failed.\n");	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ret = -EFAULT;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_on_error;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	op->fcp_req.rspdma = fc_dma_map_single(ctrl->lport->dev,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				&op->rsp_iu, sizeof(op->rsp_iu),	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				DMA_FROM_DEVICE);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (fc_dma_mapping_error(ctrl->lport->dev, op->fcp_req.rspdma)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(ctrl->dev,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"FCP Op failed - rspiu dma mapping failed.\n");	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ret = -EFAULT;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	atomic_set(&op->state, FCPOP_STATE_IDLE);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
out_on_error:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_init_request(struct blk_mq_tag_set *set, struct request *rq,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		unsigned int hctx_idx, unsigned int numa_node)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_ctrl *ctrl = to_fc_ctrl(set->driver_data);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_fcp_op_w_sgl *op = blk_mq_rq_to_pdu(rq);	/* [한국어] blk-mq — 태그·hctx·타임아웃·맵·완료 연동 */
	int queue_idx = (set == &ctrl->tag_set) ? hctx_idx + 1 : 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_queue *queue = &ctrl->queues[queue_idx];	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	int res;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	res = __nvme_fc_init_request(ctrl, queue, &op->op, rq, queue->rqcnt++);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (res)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return res;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	op->op.fcp_req.first_sgl = op->sgl;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	op->op.fcp_req.private = &op->priv[0];	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_req(rq)->ctrl = &ctrl->ctrl;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_req(rq)->cmd = &op->op.cmd_iu.sqe;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return res;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_init_aen_ops(struct nvme_fc_ctrl *ctrl)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_fcp_op *aen_op;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_fc_cmd_iu *cmdiu;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_command *sqe;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	void *private = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int i, ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	aen_op = ctrl->aen_ops;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	for (i = 0; i < NVME_NR_AEN_COMMANDS; i++, aen_op++) {	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		if (ctrl->lport->ops->fcprqst_priv_sz) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			private = kzalloc(ctrl->lport->ops->fcprqst_priv_sz,	/* [한국어] 커널 메모리 생명주기 */
						GFP_KERNEL);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			if (!private)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
				return -ENOMEM;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		cmdiu = &aen_op->cmd_iu;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		sqe = &cmdiu->sqe;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ret = __nvme_fc_init_request(ctrl, &ctrl->queues[0],	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				aen_op, (struct request *)NULL,	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
				(NVME_AQ_BLK_MQ_DEPTH + i));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (ret) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			kfree(private);	/* [한국어] 커널 메모리 생명주기 */
			return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		aen_op->flags = FCOP_FLAGS_AEN;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		aen_op->fcp_req.private = private;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		memset(sqe, 0, sizeof(*sqe));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		sqe->common.opcode = nvme_admin_async_event;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		/* Note: core layer may overwrite the sqe.command_id value */
		sqe->common.command_id = NVME_AQ_BLK_MQ_DEPTH + i;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_term_aen_ops(struct nvme_fc_ctrl *ctrl)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_fcp_op *aen_op;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	int i;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	cancel_work_sync(&ctrl->ctrl.async_event_work);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	aen_op = ctrl->aen_ops;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	for (i = 0; i < NVME_NR_AEN_COMMANDS; i++, aen_op++) {	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		__nvme_fc_exit_request(ctrl, aen_op);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		kfree(aen_op->fcp_req.private);	/* [한국어] 커널 메모리 생명주기 */
		aen_op->fcp_req.private = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline int	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
__nvme_fc_init_hctx(struct blk_mq_hw_ctx *hctx, void *data, unsigned int qidx)	/* [한국어] blk-mq — 태그·hctx·타임아웃·맵·완료 연동 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_ctrl *ctrl = to_fc_ctrl(data);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_fc_queue *queue = &ctrl->queues[qidx];	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	hctx->driver_data = queue;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	queue->hctx = hctx;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_init_hctx(struct blk_mq_hw_ctx *hctx, void *data, unsigned int hctx_idx)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return __nvme_fc_init_hctx(hctx, data, hctx_idx + 1);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_init_admin_hctx(struct blk_mq_hw_ctx *hctx, void *data,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		unsigned int hctx_idx)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return __nvme_fc_init_hctx(hctx, data, hctx_idx);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_init_queue(struct nvme_fc_ctrl *ctrl, int idx)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_queue *queue;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	queue = &ctrl->queues[idx];	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	memset(queue, 0, sizeof(*queue));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	queue->ctrl = ctrl;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	queue->qnum = idx;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	atomic_set(&queue->csn, 0);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	queue->dev = ctrl->dev;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (idx > 0)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		queue->cmnd_capsule_len = ctrl->ctrl.ioccsz * 16;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		queue->cmnd_capsule_len = sizeof(struct nvme_command);	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	/*
	 * Considered whether we should allocate buffers for all SQEs
	 * and CQEs and dma map them - mapping their respective entries
	 * into the request structures (kernel vm addr and dma address)
	 * thus the driver could use the buffers/mappings directly.
	 * It only makes sense if the LLDD would use them for its
	 * messaging api. It's very unlikely most adapter api's would use
	 * a native NVME sqe/cqe. More reasonable if FC-NVME IU payload
	 * structures were used instead.
	 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

/*
 * This routine terminates a queue at the transport level.
 * The transport has already ensured that all outstanding ios on
 * the queue have been terminated.
 * The transport will send a Disconnect LS request to terminate
 * the queue's connection. Termination of the admin queue will also
 * terminate the association at the target.
 */
static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_free_queue(struct nvme_fc_queue *queue)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (!test_and_clear_bit(NVME_FC_Q_CONNECTED, &queue->flags))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	clear_bit(NVME_FC_Q_LIVE, &queue->flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	/*
	 * Current implementation never disconnects a single queue.
	 * It always terminates a whole association. So there is never
	 * a disconnect(queue) LS sent to the target.
	 */

	queue->connection_id = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	atomic_set(&queue->csn, 0);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
__nvme_fc_delete_hw_queue(struct nvme_fc_ctrl *ctrl,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_fc_queue *queue, unsigned int qidx)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ctrl->lport->ops->delete_queue)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ctrl->lport->ops->delete_queue(&ctrl->lport->localport, qidx,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				queue->lldd_handle);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	queue->lldd_handle = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_free_io_queues(struct nvme_fc_ctrl *ctrl)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int i;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	for (i = 1; i < ctrl->ctrl.queue_count; i++)	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		nvme_fc_free_queue(&ctrl->queues[i]);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
__nvme_fc_create_hw_queue(struct nvme_fc_ctrl *ctrl,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_fc_queue *queue, unsigned int qidx, u16 qsize)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int ret = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	queue->lldd_handle = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ctrl->lport->ops->create_queue)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = ctrl->lport->ops->create_queue(&ctrl->lport->localport,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				qidx, qsize, &queue->lldd_handle);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_delete_hw_io_queues(struct nvme_fc_ctrl *ctrl)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_queue *queue = &ctrl->queues[ctrl->ctrl.queue_count - 1];	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	int i;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	for (i = ctrl->ctrl.queue_count - 1; i >= 1; i--, queue--)	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		__nvme_fc_delete_hw_queue(ctrl, queue, i);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_create_hw_io_queues(struct nvme_fc_ctrl *ctrl, u16 qsize)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_queue *queue = &ctrl->queues[1];	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	int i, ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	for (i = 1; i < ctrl->ctrl.queue_count; i++, queue++) {	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		ret = __nvme_fc_create_hw_queue(ctrl, queue, i, qsize);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			goto delete_queues;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

delete_queues:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	for (; i > 0; i--)	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		__nvme_fc_delete_hw_queue(ctrl, &ctrl->queues[i], i);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_connect_io_queues(struct nvme_fc_ctrl *ctrl, u16 qsize)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int i, ret = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	for (i = 1; i < ctrl->ctrl.queue_count; i++) {	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		ret = nvme_fc_connect_queue(ctrl, &ctrl->queues[i], qsize,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
					(qsize / 5));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ret = nvmf_connect_io_queue(&ctrl->ctrl, i);	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
		if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		set_bit(NVME_FC_Q_LIVE, &ctrl->queues[i].flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_init_io_queues(struct nvme_fc_ctrl *ctrl)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int i;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	for (i = 1; i < ctrl->ctrl.queue_count; i++)	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		nvme_fc_init_queue(ctrl, i);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_ctrl_free(struct kref *ref)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_ctrl *ctrl =	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		container_of(ref, struct nvme_fc_ctrl, ref);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	unsigned long flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/* remove from rport list */
	spin_lock_irqsave(&ctrl->rport->lock, flags);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
	list_del(&ctrl->ctrl_list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	spin_unlock_irqrestore(&ctrl->rport->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	kfree(ctrl->queues);	/* [한국어] 커널 메모리 생명주기 */

	put_device(ctrl->dev);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_fc_rport_put(ctrl->rport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	ida_free(&nvme_fc_ctrl_cnt, ctrl->cnum);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	if (ctrl->ctrl.opts)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvmf_free_options(ctrl->ctrl.opts);	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
	kfree(ctrl);	/* [한국어] 커널 메모리 생명주기 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_ctrl_put(struct nvme_fc_ctrl *ctrl)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	kref_put(&ctrl->ref, nvme_fc_ctrl_free);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_ctrl_get(struct nvme_fc_ctrl *ctrl)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return kref_get_unless_zero(&ctrl->ref);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

/*
 * All accesses from nvme core layer done - can now free the
 * controller. Called after last nvme_put_ctrl() call
 */
static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_free_ctrl(struct nvme_ctrl *nctrl)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_ctrl *ctrl = to_fc_ctrl(nctrl);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	WARN_ON(nctrl != &ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	nvme_fc_ctrl_put(ctrl);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

/*
 * This routine is used by the transport when it needs to find active
 * io on a queue that is to be terminated. The transport uses
 * blk_mq_tagset_busy_itr() to find the busy requests, which then invoke
 * this routine to kill them on a 1 by 1 basis.
 *
 * As FC allocates FC exchange for each io, the transport must contact
 * the LLDD to terminate the exchange, thus releasing the FC exchange.
 * After terminating the exchange the LLDD will call the transport's
 * normal io done path for the request, but it will have an aborted
 * status. The done path will return the io request back to the block
 * layer with an error status.
 */
static bool nvme_fc_terminate_exchange(struct request *req, void *data)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_ctrl *nctrl = data;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct nvme_fc_ctrl *ctrl = to_fc_ctrl(nctrl);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_fc_fcp_op *op = blk_mq_rq_to_pdu(req);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	op->nreq.flags |= NVME_REQ_CANCELLED;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	__nvme_fc_abort_op(ctrl, op);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return true;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

/*
 * This routine runs through all outstanding commands on the association
 * and aborts them.  This routine is typically called by the
 * delete_association routine. It is also called due to an error during
 * reconnect. In that scenario, it is most likely a command that initializes
 * the controller, including fabric Connect commands on io queues, that
 * may have timed out or failed thus the io must be killed for the connect
 * thread to see the error.
 */
static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
__nvme_fc_abort_outstanding_ios(struct nvme_fc_ctrl *ctrl, bool start_queues)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int q;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/*
	 * if aborting io, the queues are no longer good, mark them
	 * all as not live.
	 */
	if (ctrl->ctrl.queue_count > 1) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		for (q = 1; q < ctrl->ctrl.queue_count; q++)	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
			clear_bit(NVME_FC_Q_LIVE, &ctrl->queues[q].flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	clear_bit(NVME_FC_Q_LIVE, &ctrl->queues[0].flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/*
	 * If io queues are present, stop them and terminate all outstanding
	 * ios on them. As FC allocates FC exchange for each io, the
	 * transport must contact the LLDD to terminate the exchange,
	 * thus releasing the FC exchange. We use blk_mq_tagset_busy_itr()
	 * to tell us what io's are busy and invoke a transport routine
	 * to kill them with the LLDD.  After terminating the exchange
	 * the LLDD will call the transport's normal io done path, but it
	 * will have an aborted status. The done path will return the
	 * io requests back to the block layer as part of normal completions
	 * (but with error status).
	 */
	if (ctrl->ctrl.queue_count > 1) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_quiesce_io_queues(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_sync_io_queues(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		blk_mq_tagset_busy_iter(&ctrl->tag_set,	/* [한국어] blk-mq — 태그·hctx·타임아웃·맵·완료 연동 */
				nvme_fc_terminate_exchange, &ctrl->ctrl);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		blk_mq_tagset_wait_completed_request(&ctrl->tag_set);	/* [한국어] blk-mq — 태그·hctx·타임아웃·맵·완료 연동 */
		if (start_queues)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			nvme_unquiesce_io_queues(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/*
	 * Other transports, which don't have link-level contexts bound
	 * to sqe's, would try to gracefully shutdown the controller by
	 * writing the registers for shutdown and polling (call
	 * nvme_disable_ctrl()). Given a bunch of i/o was potentially
	 * just aborted and we will wait on those contexts, and given
	 * there was no indication of how live the controller is on the
	 * link, don't send more io to create more contexts for the
	 * shutdown. Let the controller fail via keepalive failure if
	 * its still present.
	 */

	/*
	 * clean up the admin queue. Same thing as above.
	 */
	nvme_quiesce_admin_queue(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	blk_sync_queue(ctrl->ctrl.admin_q);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	blk_mq_tagset_busy_iter(&ctrl->admin_tag_set,	/* [한국어] blk-mq — 태그·hctx·타임아웃·맵·완료 연동 */
				nvme_fc_terminate_exchange, &ctrl->ctrl);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	blk_mq_tagset_wait_completed_request(&ctrl->admin_tag_set);	/* [한국어] blk-mq — 태그·hctx·타임아웃·맵·완료 연동 */
	if (start_queues)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_unquiesce_admin_queue(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_error_recovery(struct nvme_fc_ctrl *ctrl, char *errmsg)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	enum nvme_ctrl_state state = nvme_ctrl_state(&ctrl->ctrl);	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	/*
	 * if an error (io timeout, etc) while (re)connecting, the remote
	 * port requested terminating of the association (disconnect_ls)
	 * or an error (timeout or abort) occurred on an io while creating
	 * the controller.  Abort any ios on the association and let the
	 * create_association error path resolve things.
	 */
	if (state == NVME_CTRL_CONNECTING) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		__nvme_fc_abort_outstanding_ios(ctrl, true);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		dev_warn(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"NVME-FC{%d}: transport error during (re)connect\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			ctrl->cnum);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/* Otherwise, only proceed if in LIVE state - e.g. on first error */
	if (state != NVME_CTRL_LIVE)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	dev_warn(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		"NVME-FC{%d}: transport association event: %s\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ctrl->cnum, errmsg);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	dev_warn(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		"NVME-FC{%d}: resetting controller\n", ctrl->cnum);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	nvme_reset_ctrl(&ctrl->ctrl);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static enum blk_eh_timer_return nvme_fc_timeout(struct request *rq)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_fcp_op *op = blk_mq_rq_to_pdu(rq);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_fc_ctrl *ctrl = op->ctrl;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	u16 qnum = op->queue->qnum;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_cmd_iu *cmdiu = &op->cmd_iu;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_command *sqe = &cmdiu->sqe;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	/*
	 * Attempt to abort the offending command. Command completion
	 * will detect the aborted io and will fail the connection.
	 */
	dev_info(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		"NVME-FC{%d.%d}: io timeout: opcode %d fctype %d (%s) w10/11: "	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		"x%08x/x%08x\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ctrl->cnum, qnum, sqe->common.opcode, sqe->fabrics.fctype,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_fabrics_opcode_str(qnum, sqe),	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		sqe->common.cdw10, sqe->common.cdw11);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (__nvme_fc_abort_op(ctrl, op))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_fc_error_recovery(ctrl, "io timeout abort failed");	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	/*
	 * the io abort has been initiated. Have the reset timer
	 * restarted and the abort completion will complete the io
	 * shortly. Avoids a synchronous wait while the abort finishes.
	 */
	return BLK_EH_RESET_TIMER;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_map_data(struct nvme_fc_ctrl *ctrl, struct request *rq,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		struct nvme_fc_fcp_op *op)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvmefc_fcp_req *freq = &op->fcp_req;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	freq->sg_cnt = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!blk_rq_nr_phys_segments(rq))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	freq->sg_table.sgl = freq->first_sgl;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ret = sg_alloc_table_chained(&freq->sg_table,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			blk_rq_nr_phys_segments(rq), freq->sg_table.sgl,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			NVME_INLINE_SG_CNT);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return -ENOMEM;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	op->nents = blk_rq_map_sg(rq, freq->sg_table.sgl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	WARN_ON(op->nents > blk_rq_nr_phys_segments(rq));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	freq->sg_cnt = fc_dma_map_sg(ctrl->lport->dev, freq->sg_table.sgl,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				op->nents, rq_dma_dir(rq));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (unlikely(freq->sg_cnt <= 0)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		sg_free_table_chained(&freq->sg_table, NVME_INLINE_SG_CNT);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		freq->sg_cnt = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return -EFAULT;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/*
	 * TODO: blk_integrity_rq(rq)  for DIF
	 */
	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_unmap_data(struct nvme_fc_ctrl *ctrl, struct request *rq,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		struct nvme_fc_fcp_op *op)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvmefc_fcp_req *freq = &op->fcp_req;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	if (!freq->sg_cnt)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	fc_dma_unmap_sg(ctrl->lport->dev, freq->sg_table.sgl, op->nents,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			rq_dma_dir(rq));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	sg_free_table_chained(&freq->sg_table, NVME_INLINE_SG_CNT);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	freq->sg_cnt = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

/*
 * In FC, the queue is a logical thing. At transport connect, the target
 * creates its "queue" and returns a handle that is to be given to the
 * target whenever it posts something to the corresponding SQ.  When an
 * SQE is sent on a SQ, FC effectively considers the SQE, or rather the
 * command contained within the SQE, an io, and assigns a FC exchange
 * to it. The SQE and the associated SQ handle are sent in the initial
 * CMD IU sents on the exchange. All transfers relative to the io occur
 * as part of the exchange.  The CQE is the last thing for the io,
 * which is transferred (explicitly or implicitly) with the RSP IU
 * sent on the exchange. After the CQE is received, the FC exchange is
 * terminated and the Exchange may be used on a different io.
 *
 * The transport to LLDD api has the transport making a request for a
 * new fcp io request to the LLDD. The LLDD then allocates a FC exchange
 * resource and transfers the command. The LLDD will then process all
 * steps to complete the io. Upon completion, the transport done routine
 * is called.
 *
 * So - while the operation is outstanding to the LLDD, there is a link
 * level FC exchange resource that is also outstanding. This must be
 * considered in all cleanup operations.
 */
static blk_status_t	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_start_fcp_op(struct nvme_fc_ctrl *ctrl, struct nvme_fc_queue *queue,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_fc_fcp_op *op, u32 data_len,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	enum nvmefc_fcp_datadir	io_dir)	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_cmd_iu *cmdiu = &op->cmd_iu;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_command *sqe = &cmdiu->sqe;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	int ret, opstate;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/*
	 * before attempting to send the io, check to see if we believe
	 * the target device is present
	 */
	if (ctrl->rport->remoteport.port_state != FC_OBJSTATE_ONLINE)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return BLK_STS_RESOURCE;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	if (!nvme_fc_ctrl_get(ctrl))	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		return BLK_STS_IOERR;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	/* format the FC-NVME CMD IU and fcp_req */
	cmdiu->connection_id = cpu_to_be64(queue->connection_id);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	cmdiu->data_len = cpu_to_be32(data_len);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	switch (io_dir) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	case NVMEFC_FCP_WRITE:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		cmdiu->flags = FCNVME_CMD_FLAGS_WRITE;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	case NVMEFC_FCP_READ:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		cmdiu->flags = FCNVME_CMD_FLAGS_READ;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	case NVMEFC_FCP_NODATA:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		cmdiu->flags = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	op->fcp_req.payload_length = data_len;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	op->fcp_req.io_dir = io_dir;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	op->fcp_req.transferred_length = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	op->fcp_req.rcv_rsplen = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	op->fcp_req.status = NVME_SC_SUCCESS;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	op->fcp_req.sqid = cpu_to_le16(queue->qnum);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/*
	 * validate per fabric rules, set fields mandated by fabric spec
	 * as well as those by FC-NVME spec.
	 */
	WARN_ON_ONCE(sqe->common.metadata);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	sqe->common.flags |= NVME_CMD_SGL_METABUF;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/*
	 * format SQE DPTR field per FC-NVME rules:
	 *    type=0x5     Transport SGL Data Block Descriptor
	 *    subtype=0xA  Transport-specific value
	 *    address=0
	 *    length=length of the data series
	 */
	sqe->rw.dptr.sgl.type = (NVME_TRANSPORT_SGL_DATA_DESC << 4) |	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
					NVME_SGL_FMT_TRANSPORT_A;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	sqe->rw.dptr.sgl.length = cpu_to_le32(data_len);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	sqe->rw.dptr.sgl.addr = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!(op->flags & FCOP_FLAGS_AEN)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = nvme_fc_map_data(ctrl, op->rq, op);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		if (ret < 0) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			nvme_cleanup_cmd(op->rq);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvme_fc_ctrl_put(ctrl);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
			if (ret == -ENOMEM || ret == -EAGAIN)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
				return BLK_STS_RESOURCE;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
			return BLK_STS_IOERR;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	fc_dma_sync_single_for_device(ctrl->lport->dev, op->fcp_req.cmddma,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				  sizeof(op->cmd_iu), DMA_TO_DEVICE);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	atomic_set(&op->state, FCPOP_STATE_ACTIVE);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!(op->flags & FCOP_FLAGS_AEN))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_start_request(op->rq);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */

	cmdiu->csn = cpu_to_be32(atomic_inc_return(&queue->csn));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ret = ctrl->lport->ops->fcp_io(&ctrl->lport->localport,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
					&ctrl->rport->remoteport,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
					queue->lldd_handle, &op->fcp_req);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (ret) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		/*
		 * If the lld fails to send the command is there an issue with
		 * the csn value?  If the command that fails is the Connect,
		 * no - as the connection won't be live.  If it is a command
		 * post-connect, it's possible a gap in csn may be created.
		 * Does this matter?  As Linux initiators don't send fused
		 * commands, no.  The gap would exist, but as there's nothing
		 * that depends on csn order to be delivered on the target
		 * side, it shouldn't hurt.  It would be difficult for a
		 * target to even detect the csn gap as it has no idea when the
		 * cmd with the csn was supposed to arrive.
		 */
		opstate = atomic_xchg(&op->state, FCPOP_STATE_COMPLETE);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		__nvme_fc_fcpop_chk_teardowns(ctrl, op, opstate);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		if (!(op->flags & FCOP_FLAGS_AEN)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			nvme_fc_unmap_data(ctrl, op->rq, op);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
			nvme_cleanup_cmd(op->rq);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		nvme_fc_ctrl_put(ctrl);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

		if (ctrl->rport->remoteport.port_state == FC_OBJSTATE_ONLINE &&	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
				ret != -EBUSY)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			return BLK_STS_IOERR;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

		return BLK_STS_RESOURCE;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return BLK_STS_OK;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static blk_status_t	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_queue_rq(struct blk_mq_hw_ctx *hctx,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
			const struct blk_mq_queue_data *bd)	/* [한국어] blk-mq — 태그·hctx·타임아웃·맵·완료 연동 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_ns *ns = hctx->queue->queuedata;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct nvme_fc_queue *queue = hctx->driver_data;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_fc_ctrl *ctrl = queue->ctrl;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct request *rq = bd->rq;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct nvme_fc_fcp_op *op = blk_mq_rq_to_pdu(rq);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	enum nvmefc_fcp_datadir	io_dir;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	bool queue_ready = test_bit(NVME_FC_Q_LIVE, &queue->flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	u32 data_len;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	blk_status_t ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (ctrl->rport->remoteport.port_state != FC_OBJSTATE_ONLINE ||	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
	    !nvme_check_ready(&queue->ctrl->ctrl, rq, queue_ready))	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
		return nvme_fail_nonready_command(&queue->ctrl->ctrl, rq);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */

	ret = nvme_setup_cmd(ns, rq);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	/*
	 * nvme core doesn't quite treat the rq opaquely. Commands such
	 * as WRITE ZEROES will return a non-zero rq payload_bytes yet
	 * there is no actual payload to be transferred.
	 * To get it right, key data transmission on there being 1 or
	 * more physical segments in the sg list. If there are no
	 * physical segments, there is no payload.
	 */
	if (blk_rq_nr_phys_segments(rq)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		data_len = blk_rq_payload_bytes(rq);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		io_dir = ((rq_data_dir(rq) == WRITE) ?	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
					NVMEFC_FCP_WRITE : NVMEFC_FCP_READ);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	} else {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		data_len = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		io_dir = NVMEFC_FCP_NODATA;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */


	return nvme_fc_start_fcp_op(ctrl, queue, op, data_len, io_dir);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_submit_async_event(struct nvme_ctrl *arg)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_ctrl *ctrl = to_fc_ctrl(arg);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_fc_fcp_op *aen_op;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	blk_status_t ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (test_bit(FCCTRL_TERMIO, &ctrl->flags))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	aen_op = &ctrl->aen_ops[0];	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = nvme_fc_start_fcp_op(ctrl, aen_op->queue, aen_op, 0,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
					NVMEFC_FCP_NODATA);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"failed async event work\n");	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_complete_rq(struct request *rq)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_fcp_op *op = blk_mq_rq_to_pdu(rq);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_fc_ctrl *ctrl = op->ctrl;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	atomic_set(&op->state, FCPOP_STATE_IDLE);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	op->flags &= ~FCOP_FLAGS_TERMIO;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	nvme_fc_unmap_data(ctrl, rq, op);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	nvme_complete_rq(rq);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
	nvme_fc_ctrl_put(ctrl);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_fc_map_queues(struct blk_mq_tag_set *set)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_ctrl *ctrl = to_fc_ctrl(set->driver_data);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	int i;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	for (i = 0; i < set->nr_maps; i++) {	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		struct blk_mq_queue_map *map = &set->map[i];	/* [한국어] blk-mq — 태그·hctx·타임아웃·맵·완료 연동 */

		if (!map->nr_queues) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			WARN_ON(i == HCTX_TYPE_DEFAULT);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			continue;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		/* Call LLDD map queue functionality if defined */
		if (ctrl->lport->ops->map_queues)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			ctrl->lport->ops->map_queues(&ctrl->lport->localport,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
						     map);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			blk_mq_map_queues(map);	/* [한국어] blk-mq — 태그·hctx·타임아웃·맵·완료 연동 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static const struct blk_mq_ops nvme_fc_mq_ops = {	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	.queue_rq	= nvme_fc_queue_rq,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	.complete	= nvme_fc_complete_rq,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	.init_request	= nvme_fc_init_request,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	.exit_request	= nvme_fc_exit_request,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	.init_hctx	= nvme_fc_init_hctx,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	.timeout	= nvme_fc_timeout,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	.map_queues	= nvme_fc_map_queues,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_create_io_queues(struct nvme_fc_ctrl *ctrl)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvmf_ctrl_options *opts = ctrl->ctrl.opts;	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
	unsigned int nr_io_queues;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	nr_io_queues = min3(opts->nr_io_queues, num_online_cpus(),	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				ctrl->lport->ops->max_hw_queues);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ret = nvme_set_queue_count(&ctrl->ctrl, &nr_io_queues);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_info(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"set_queue_count failed: %d\n", ret);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ctrl->ctrl.queue_count = nr_io_queues + 1;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (!nr_io_queues)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	nvme_fc_init_io_queues(ctrl);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	ret = nvme_alloc_io_tag_set(&ctrl->ctrl, &ctrl->tag_set,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			&nvme_fc_mq_ops, 1,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
			struct_size_t(struct nvme_fcp_op_w_sgl, priv,	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
				      ctrl->lport->ops->fcprqst_priv_sz));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	ret = nvme_fc_create_hw_io_queues(ctrl, ctrl->ctrl.sqsize + 1);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_cleanup_tagset;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	ret = nvme_fc_connect_io_queues(ctrl, ctrl->ctrl.sqsize + 1);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_delete_hw_queues;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	ctrl->ioq_live = true;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

out_delete_hw_queues:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_fc_delete_hw_io_queues(ctrl);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
out_cleanup_tagset:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_remove_io_tag_set(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_fc_free_io_queues(ctrl);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	/* force put free routine to ignore io queues */
	ctrl->ctrl.tagset = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_recreate_io_queues(struct nvme_fc_ctrl *ctrl)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvmf_ctrl_options *opts = ctrl->ctrl.opts;	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
	u32 prior_ioq_cnt = ctrl->ctrl.queue_count - 1;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	unsigned int nr_io_queues;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	nr_io_queues = min3(opts->nr_io_queues, num_online_cpus(),	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				ctrl->lport->ops->max_hw_queues);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ret = nvme_set_queue_count(&ctrl->ctrl, &nr_io_queues);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_info(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"set_queue_count failed: %d\n", ret);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!nr_io_queues && prior_ioq_cnt) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_info(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"Fail Reconnect: At least 1 io queue "	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"required (was %d)\n", prior_ioq_cnt);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return -ENOSPC;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ctrl->ctrl.queue_count = nr_io_queues + 1;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	/* check for io queues existing */
	if (ctrl->ctrl.queue_count == 1)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	if (prior_ioq_cnt != nr_io_queues) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_info(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"reconnect: revising io queue count from %d to %d\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			prior_ioq_cnt, nr_io_queues);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		blk_mq_update_nr_hw_queues(&ctrl->tag_set, nr_io_queues);	/* [한국어] blk-mq — 태그·hctx·타임아웃·맵·완료 연동 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = nvme_fc_create_hw_io_queues(ctrl, ctrl->ctrl.sqsize + 1);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_free_io_queues;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	ret = nvme_fc_connect_io_queues(ctrl, ctrl->ctrl.sqsize + 1);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_delete_hw_queues;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

out_delete_hw_queues:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_fc_delete_hw_io_queues(ctrl);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
out_free_io_queues:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_fc_free_io_queues(ctrl);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_rport_active_on_lport(struct nvme_fc_rport *rport)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_lport *lport = rport->lport;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	atomic_inc(&lport->act_rport_cnt);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_rport_inactive_on_lport(struct nvme_fc_rport *rport)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_lport *lport = rport->lport;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	u32 cnt;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	cnt = atomic_dec_return(&lport->act_rport_cnt);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (cnt == 0 && lport->localport.port_state == FC_OBJSTATE_DELETED)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		lport->ops->localport_delete(&lport->localport);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_ctlr_active_on_rport(struct nvme_fc_ctrl *ctrl)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_rport *rport = ctrl->rport;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	u32 cnt;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (test_and_set_bit(ASSOC_ACTIVE, &ctrl->flags))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return 1;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	cnt = atomic_inc_return(&rport->act_ctrl_cnt);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (cnt == 1)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_fc_rport_active_on_lport(rport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_ctlr_inactive_on_rport(struct nvme_fc_ctrl *ctrl)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_rport *rport = ctrl->rport;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_fc_lport *lport = rport->lport;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	u32 cnt;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/* clearing of ctrl->flags ASSOC_ACTIVE bit is in association delete */

	cnt = atomic_dec_return(&rport->act_ctrl_cnt);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (cnt == 0) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		if (rport->remoteport.port_state == FC_OBJSTATE_DELETED)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			lport->ops->remoteport_delete(&rport->remoteport);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_fc_rport_inactive_on_lport(rport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

/*
 * This routine restarts the controller on the host side, and
 * on the link side, recreates the controller association.
 */
static int	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_create_association(struct nvme_fc_ctrl *ctrl)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvmf_ctrl_options *opts = ctrl->ctrl.opts;	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
	struct nvmefc_ls_rcv_op *disls = NULL;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	unsigned long flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	++ctrl->ctrl.nr_reconnects;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	spin_lock_irqsave(&ctrl->rport->lock, flags);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
	if (ctrl->rport->remoteport.port_state != FC_OBJSTATE_ONLINE) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		spin_unlock_irqrestore(&ctrl->rport->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return -ENODEV;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (nvme_fc_ctlr_active_on_rport(ctrl)) {	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		spin_unlock_irqrestore(&ctrl->rport->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return -ENOTUNIQ;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	spin_unlock_irqrestore(&ctrl->rport->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	dev_info(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		"NVME-FC{%d}: create association : host wwpn 0x%016llx "	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		" rport wwpn 0x%016llx: NQN \"%s\"\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ctrl->cnum, ctrl->lport->localport.port_name,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ctrl->rport->remoteport.port_name, ctrl->ctrl.opts->subsysnqn);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	clear_bit(ASSOC_FAILED, &ctrl->flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/*
	 * Create the admin queue
	 */

	ret = __nvme_fc_create_hw_queue(ctrl, &ctrl->queues[0], 0,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				NVME_AQ_DEPTH);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_free_queue;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	ret = nvme_fc_connect_admin_queue(ctrl, &ctrl->queues[0],	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
				NVME_AQ_DEPTH, (NVME_AQ_DEPTH / 4));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_delete_hw_queue;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	ret = nvmf_connect_admin_queue(&ctrl->ctrl);	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_disconnect_admin_queue;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	set_bit(NVME_FC_Q_LIVE, &ctrl->queues[0].flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/*
	 * Check controller capabilities
	 *
	 * todo:- add code to check if ctrl attributes changed from
	 * prior connection values
	 */

	ret = nvme_enable_ctrl(&ctrl->ctrl);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
	if (!ret && test_bit(ASSOC_FAILED, &ctrl->flags))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = -EIO;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_disconnect_admin_queue;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	ctrl->ctrl.max_segments = ctrl->lport->ops->max_sgl_segments;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ctrl->ctrl.max_hw_sectors = ctrl->ctrl.max_segments <<	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
						(ilog2(SZ_4K) - 9);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	nvme_unquiesce_admin_queue(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = nvme_init_ctrl_finish(&ctrl->ctrl, false);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_disconnect_admin_queue;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	if (test_bit(ASSOC_FAILED, &ctrl->flags)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = -EIO;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_stop_keep_alive;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	/* sanity checks */

	/* FC-NVME does not have other data in the capsule */
	if (ctrl->ctrl.icdoff) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(ctrl->ctrl.device, "icdoff %d is not supported!\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				ctrl->ctrl.icdoff);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ret = NVME_SC_INVALID_FIELD | NVME_STATUS_DNR;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_stop_keep_alive;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/* FC-NVME supports normal SGL Data Block Descriptors */
	if (!nvme_ctrl_sgl_supported(&ctrl->ctrl)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"Mandatory sgls are not supported!\n");	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ret = NVME_SC_INVALID_FIELD | NVME_STATUS_DNR;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_stop_keep_alive;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (opts->queue_size > ctrl->ctrl.maxcmd) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		/* warn if maxcmd is lower than queue_size */
		dev_warn(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"queue_size %zu > ctrl maxcmd %u, reducing "	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"to maxcmd\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			opts->queue_size, ctrl->ctrl.maxcmd);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		opts->queue_size = ctrl->ctrl.maxcmd;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ctrl->ctrl.sqsize = opts->queue_size - 1;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = nvme_fc_init_aen_ops(ctrl);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_term_aen_ops;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	/*
	 * Create the io queues
	 */

	if (ctrl->ctrl.queue_count > 1) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		if (!ctrl->ioq_live)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			ret = nvme_fc_create_io_queues(ctrl);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			ret = nvme_fc_recreate_io_queues(ctrl);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (!ret && test_bit(ASSOC_FAILED, &ctrl->flags))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = -EIO;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_term_aen_ops;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	if (!nvme_change_ctrl_state(&ctrl->ctrl, NVME_CTRL_LIVE)) {	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
		ret = -EIO;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_term_aen_ops;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ctrl->ctrl.nr_reconnects = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_start_ctrl(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return 0;	/* Success */	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

out_term_aen_ops:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_fc_term_aen_ops(ctrl);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
out_stop_keep_alive:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_stop_keep_alive(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
out_disconnect_admin_queue:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	dev_warn(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		"NVME-FC{%d}: create_assoc failed, assoc_id %llx ret %d\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ctrl->cnum, ctrl->association_id, ret);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	/* send a Disconnect(association) LS to fc-nvme target */
	nvme_fc_xmt_disconnect_assoc(ctrl);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	spin_lock_irqsave(&ctrl->lock, flags);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
	ctrl->association_id = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	disls = ctrl->rcv_disconn;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ctrl->rcv_disconn = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	spin_unlock_irqrestore(&ctrl->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (disls)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_fc_xmt_ls_rsp(disls);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
out_delete_hw_queue:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	__nvme_fc_delete_hw_queue(ctrl, &ctrl->queues[0], 0);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
out_free_queue:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_fc_free_queue(&ctrl->queues[0]);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	clear_bit(ASSOC_ACTIVE, &ctrl->flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_fc_ctlr_inactive_on_rport(ctrl);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */


/*
 * This routine stops operation of the controller on the host side.
 * On the host os stack side: Admin and IO queues are stopped,
 *   outstanding ios on them terminated via FC ABTS.
 * On the link side: the association is terminated.
 */
static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_delete_association(struct nvme_fc_ctrl *ctrl)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvmefc_ls_rcv_op *disls = NULL;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	unsigned long flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!test_and_clear_bit(ASSOC_ACTIVE, &ctrl->flags))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	spin_lock_irqsave(&ctrl->lock, flags);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
	set_bit(FCCTRL_TERMIO, &ctrl->flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ctrl->iocnt = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	spin_unlock_irqrestore(&ctrl->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	__nvme_fc_abort_outstanding_ios(ctrl, false);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/* kill the aens as they are a separate path */
	nvme_fc_abort_aen_ops(ctrl);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	/* wait for all io that had to be aborted */
	spin_lock_irq(&ctrl->lock);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
	wait_event_lock_irq(ctrl->ioabort_wait, ctrl->iocnt == 0, ctrl->lock);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	clear_bit(FCCTRL_TERMIO, &ctrl->flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	spin_unlock_irq(&ctrl->lock);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	nvme_fc_term_aen_ops(ctrl);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	/*
	 * send a Disconnect(association) LS to fc-nvme target
	 * Note: could have been sent at top of process, but
	 * cleaner on link traffic if after the aborts complete.
	 * Note: if association doesn't exist, association_id will be 0
	 */
	if (ctrl->association_id)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_fc_xmt_disconnect_assoc(ctrl);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	spin_lock_irqsave(&ctrl->lock, flags);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
	ctrl->association_id = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	disls = ctrl->rcv_disconn;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ctrl->rcv_disconn = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	spin_unlock_irqrestore(&ctrl->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (disls)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		/*
		 * if a Disconnect Request was waiting for a response, send
		 * now that all ABTS's have been issued (and are complete).
		 */
		nvme_fc_xmt_ls_rsp(disls);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	if (ctrl->ctrl.tagset) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_fc_delete_hw_io_queues(ctrl);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		nvme_fc_free_io_queues(ctrl);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	__nvme_fc_delete_hw_queue(ctrl, &ctrl->queues[0], 0);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_fc_free_queue(&ctrl->queues[0]);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	/* re-enable the admin_q so anything new can fast fail */
	nvme_unquiesce_admin_queue(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/* resume the io queues so that things will fast fail */
	nvme_unquiesce_io_queues(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	nvme_fc_ctlr_inactive_on_rport(ctrl);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_delete_ctrl(struct nvme_ctrl *nctrl)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_ctrl *ctrl = to_fc_ctrl(nctrl);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	cancel_delayed_work_sync(&ctrl->connect_work);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/*
	 * kill the association on the link side.  this will block
	 * waiting for io to terminate
	 */
	nvme_fc_delete_association(ctrl);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	cancel_work_sync(&ctrl->ioerr_work);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (ctrl->ctrl.tagset)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_remove_io_tag_set(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	nvme_unquiesce_admin_queue(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_remove_admin_tag_set(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_reconnect_or_delete(struct nvme_fc_ctrl *ctrl, int status)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_rport *rport = ctrl->rport;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_fc_remote_port *portptr = &rport->remoteport;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	unsigned long recon_delay = ctrl->ctrl.opts->reconnect_delay * HZ;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	bool recon = true;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (nvme_ctrl_state(&ctrl->ctrl) != NVME_CTRL_CONNECTING)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	if (portptr->port_state == FC_OBJSTATE_ONLINE) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_info(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"NVME-FC{%d}: reset: Reconnect attempt failed (%d)\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			ctrl->cnum, status);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	} else if (time_after_eq(jiffies, rport->dev_loss_end))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		recon = false;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (recon && nvmf_should_reconnect(&ctrl->ctrl, status)) {	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
		if (portptr->port_state == FC_OBJSTATE_ONLINE)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			dev_info(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				"NVME-FC{%d}: Reconnect attempt in %ld "	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				"seconds\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				ctrl->cnum, recon_delay / HZ);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		else if (time_after(jiffies + recon_delay, rport->dev_loss_end))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			recon_delay = rport->dev_loss_end - jiffies;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		queue_delayed_work(nvme_wq, &ctrl->connect_work, recon_delay);	/* [한국어] 워크큐 — 송수신/재연결/복구 비동기 실행 */
	} else {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (portptr->port_state == FC_OBJSTATE_ONLINE) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			if (status > 0 && (status & NVME_STATUS_DNR))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
				dev_warn(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
					 "NVME-FC{%d}: reconnect failure\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
					 ctrl->cnum);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				dev_warn(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
					 "NVME-FC{%d}: Max reconnect attempts "	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
					 "(%d) reached.\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
					 ctrl->cnum, ctrl->ctrl.nr_reconnects);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		} else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			dev_warn(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				"NVME-FC{%d}: dev_loss_tmo (%d) expired "	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				"while waiting for remoteport connectivity.\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				ctrl->cnum, min_t(int, portptr->dev_loss_tmo,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
					(ctrl->ctrl.opts->max_reconnects *	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
					 ctrl->ctrl.opts->reconnect_delay)));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		WARN_ON(nvme_delete_ctrl(&ctrl->ctrl));	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_reset_ctrl_work(struct work_struct *work)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_ctrl *ctrl =	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		container_of(work, struct nvme_fc_ctrl, ctrl.reset_work);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	nvme_stop_ctrl(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/* will block will waiting for io to terminate */
	nvme_fc_delete_association(ctrl);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	if (!nvme_change_ctrl_state(&ctrl->ctrl, NVME_CTRL_CONNECTING))	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
		dev_err(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"NVME-FC{%d}: error_recovery: Couldn't change state "	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"to CONNECTING\n", ctrl->cnum);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (ctrl->rport->remoteport.port_state == FC_OBJSTATE_ONLINE) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		if (!queue_delayed_work(nvme_wq, &ctrl->connect_work, 0)) {	/* [한국어] 워크큐 — 송수신/재연결/복구 비동기 실행 */
			dev_err(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				"NVME-FC{%d}: failed to schedule connect "	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				"after reset\n", ctrl->cnum);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		} else {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			flush_delayed_work(&ctrl->connect_work);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	} else {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_fc_reconnect_or_delete(ctrl, -ENOTCONN);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */


static const struct nvme_ctrl_ops nvme_fc_ctrl_ops = {	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	.name			= "fc",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	.module			= THIS_MODULE,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	.flags			= NVME_F_FABRICS,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	.reg_read32		= nvmf_reg_read32,	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
	.reg_read64		= nvmf_reg_read64,	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
	.reg_write32		= nvmf_reg_write32,	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
	.subsystem_reset	= nvmf_subsystem_reset,	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
	.free_ctrl		= nvme_fc_free_ctrl,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	.submit_async_event	= nvme_fc_submit_async_event,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	.delete_ctrl		= nvme_fc_delete_ctrl,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	.get_address		= nvmf_get_address,	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
	.get_virt_boundary	= nvmf_get_virt_boundary,	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_connect_ctrl_work(struct work_struct *work)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	struct nvme_fc_ctrl *ctrl =	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
			container_of(to_delayed_work(work),	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				struct nvme_fc_ctrl, connect_work);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	ret = nvme_fc_create_association(ctrl);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_fc_reconnect_or_delete(ctrl, ret);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		dev_info(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"NVME-FC{%d}: controller connect complete\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			ctrl->cnum);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */


static const struct blk_mq_ops nvme_fc_admin_mq_ops = {	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	.queue_rq	= nvme_fc_queue_rq,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	.complete	= nvme_fc_complete_rq,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	.init_request	= nvme_fc_init_request,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	.exit_request	= nvme_fc_exit_request,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	.init_hctx	= nvme_fc_init_admin_hctx,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	.timeout	= nvme_fc_timeout,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */


/*
 * Fails a controller request if it matches an existing controller
 * (association) with the same tuple:
 * <Host NQN, Host ID, local FC port, remote FC port, SUBSYS NQN>
 *
 * The ports don't need to be compared as they are intrinsically
 * already matched by the port pointers supplied.
 */
static bool	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_existing_controller(struct nvme_fc_rport *rport,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		struct nvmf_ctrl_options *opts)	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_ctrl *ctrl;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	unsigned long flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	bool found = false;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	spin_lock_irqsave(&rport->lock, flags);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
	list_for_each_entry(ctrl, &rport->ctrl_list, ctrl_list) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		found = nvmf_ctlr_matches_baseopts(&ctrl->ctrl, opts);	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
		if (found)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	spin_unlock_irqrestore(&rport->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return found;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static struct nvme_fc_ctrl *	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
nvme_fc_alloc_ctrl(struct device *dev, struct nvmf_ctrl_options *opts,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_fc_lport *lport, struct nvme_fc_rport *rport)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_ctrl *ctrl;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	int ret, idx, ctrl_loss_tmo;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!(rport->remoteport.port_role &	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
	    (FC_PORT_ROLE_NVME_DISCOVERY | FC_PORT_ROLE_NVME_TARGET))) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ret = -EBADR;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_fail;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!opts->duplicate_connect &&	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
	    nvme_fc_existing_controller(rport, opts)) {	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		ret = -EALREADY;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_fail;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ctrl = kzalloc_obj(*ctrl);	/* [한국어] 커널 메모리 생명주기 */
	if (!ctrl) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = -ENOMEM;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_fail;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	idx = ida_alloc(&nvme_fc_ctrl_cnt, GFP_KERNEL);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	if (idx < 0) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = -ENOSPC;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_free_ctrl;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/*
	 * if ctrl_loss_tmo is being enforced and the default reconnect delay
	 * is being used, change to a shorter reconnect delay for FC.
	 */
	if (opts->max_reconnects != -1 &&	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
	    opts->reconnect_delay == NVMF_DEF_RECONNECT_DELAY &&	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	    opts->reconnect_delay > NVME_FC_DEFAULT_RECONNECT_TMO) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ctrl_loss_tmo = opts->max_reconnects * opts->reconnect_delay;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		opts->reconnect_delay = NVME_FC_DEFAULT_RECONNECT_TMO;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		opts->max_reconnects = DIV_ROUND_UP(ctrl_loss_tmo,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
						opts->reconnect_delay);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ctrl->ctrl.opts = opts;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ctrl->ctrl.nr_reconnects = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	INIT_LIST_HEAD(&ctrl->ctrl_list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ctrl->lport = lport;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ctrl->rport = rport;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ctrl->dev = lport->dev;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ctrl->cnum = idx;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ctrl->ioq_live = false;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	init_waitqueue_head(&ctrl->ioabort_wait);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	get_device(ctrl->dev);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	kref_init(&ctrl->ref);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	INIT_WORK(&ctrl->ctrl.reset_work, nvme_fc_reset_ctrl_work);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	INIT_DELAYED_WORK(&ctrl->connect_work, nvme_fc_connect_ctrl_work);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	INIT_WORK(&ctrl->ioerr_work, nvme_fc_ctrl_ioerr_work);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	spin_lock_init(&ctrl->lock);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */

	/* io queue count */
	ctrl->ctrl.queue_count = min_t(unsigned int,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				opts->nr_io_queues,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				lport->ops->max_hw_queues);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ctrl->ctrl.queue_count++;	/* +1 for admin queue */	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ctrl->ctrl.sqsize = opts->queue_size - 1;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ctrl->ctrl.kato = opts->kato;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ctrl->ctrl.cntlid = 0xffff;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = -ENOMEM;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ctrl->queues = kzalloc_objs(struct nvme_fc_queue,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
				    ctrl->ctrl.queue_count);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (!ctrl->queues)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_free_ida;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	nvme_fc_init_queue(ctrl, 0);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	/*
	 * Would have been nice to init io queues tag set as well.
	 * However, we require interaction from the controller
	 * for max io queue count before we can do so.
	 * Defer this to the connect path.
	 */

	ret = nvme_init_ctrl(&ctrl->ctrl, dev, &nvme_fc_ctrl_ops, 0);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_free_queues;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	if (lport->dev)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ctrl->ctrl.numa_node = dev_to_node(lport->dev);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return ctrl;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

out_free_queues:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	kfree(ctrl->queues);	/* [한국어] 커널 메모리 생명주기 */
out_free_ida:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	put_device(ctrl->dev);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ida_free(&nvme_fc_ctrl_cnt, ctrl->cnum);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
out_free_ctrl:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	kfree(ctrl);	/* [한국어] 커널 메모리 생명주기 */
out_fail:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	/* exit via here doesn't follow ctlr ref points */
	return ERR_PTR(ret);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static struct nvme_ctrl *	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
nvme_fc_init_ctrl(struct device *dev, struct nvmf_ctrl_options *opts,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_fc_lport *lport, struct nvme_fc_rport *rport)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_ctrl *ctrl;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	unsigned long flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ctrl = nvme_fc_alloc_ctrl(dev, opts, lport, rport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	if (IS_ERR(ctrl))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return ERR_CAST(ctrl);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	ret = nvme_add_ctrl(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_put_ctrl;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	ret = nvme_alloc_admin_tag_set(&ctrl->ctrl, &ctrl->admin_tag_set,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			&nvme_fc_admin_mq_ops,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
			struct_size_t(struct nvme_fcp_op_w_sgl, priv,	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
				      ctrl->lport->ops->fcprqst_priv_sz));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto fail_ctrl;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	spin_lock_irqsave(&rport->lock, flags);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
	list_add_tail(&ctrl->ctrl_list, &rport->ctrl_list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	spin_unlock_irqrestore(&rport->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!nvme_change_ctrl_state(&ctrl->ctrl, NVME_CTRL_CONNECTING)) {	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
		dev_err(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"NVME-FC{%d}: failed to init ctrl state\n", ctrl->cnum);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto fail_ctrl;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!queue_delayed_work(nvme_wq, &ctrl->connect_work, 0)) {	/* [한국어] 워크큐 — 송수신/재연결/복구 비동기 실행 */
		dev_err(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"NVME-FC{%d}: failed to schedule initial connect\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			ctrl->cnum);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto fail_ctrl;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	flush_delayed_work(&ctrl->connect_work);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	dev_info(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		"NVME-FC{%d}: new ctrl: NQN \"%s\", hostnqn: %s\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ctrl->cnum, nvmf_ctrl_subsysnqn(&ctrl->ctrl), opts->host->nqn);	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */

	return &ctrl->ctrl;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

fail_ctrl:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_change_ctrl_state(&ctrl->ctrl, NVME_CTRL_DELETING);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
	cancel_work_sync(&ctrl->ioerr_work);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	cancel_work_sync(&ctrl->ctrl.reset_work);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	cancel_delayed_work_sync(&ctrl->connect_work);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ctrl->ctrl.opts = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (ctrl->ctrl.admin_tagset)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_remove_admin_tag_set(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	/* initiate nvme ctrl ref counting teardown */
	nvme_uninit_ctrl(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

out_put_ctrl:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	/* Remove core ctrl ref. */
	nvme_put_ctrl(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/* as we're past the point where we transition to the ref
	 * counting teardown path, if we return a bad pointer here,
	 * the calling routine, thinking it's prior to the
	 * transition, will do an rport put. Since the teardown
	 * path also does a rport put, we do an extra get here to
	 * so proper order/teardown happens.
	 */
	nvme_fc_rport_get(rport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	return ERR_PTR(-EIO);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

struct nvmet_fc_traddr {	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	u64	nn;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	u64	pn;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
__nvme_fc_parse_u64(substring_t *sstr, u64 *val)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	u64 token64;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (match_u64(sstr, &token64))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return -EINVAL;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	*val = token64;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

/*
 * This routine validates and extracts the WWN's from the TRADDR string.
 * As kernel parsers need the 0x to determine number base, universally
 * build string to parse with 0x prefix before parsing name strings.
 */
static int	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_parse_traddr(struct nvmet_fc_traddr *traddr, char *buf, size_t blen)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	char name[2 + NVME_FC_TRADDR_HEXNAMELEN + 1];	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	substring_t wwn = { name, &name[sizeof(name)-1] };	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int nnoffset, pnoffset;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/* validate if string is one of the 2 allowed formats */
	if (strnlen(buf, blen) == NVME_FC_TRADDR_MAXLENGTH &&	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			!strncmp(buf, "nn-0x", NVME_FC_TRADDR_OXNNLEN) &&	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			!strncmp(&buf[NVME_FC_TRADDR_MAX_PN_OFFSET],	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				"pn-0x", NVME_FC_TRADDR_OXNNLEN)) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nnoffset = NVME_FC_TRADDR_OXNNLEN;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		pnoffset = NVME_FC_TRADDR_MAX_PN_OFFSET +	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
						NVME_FC_TRADDR_OXNNLEN;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	} else if ((strnlen(buf, blen) == NVME_FC_TRADDR_MINLENGTH &&	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			!strncmp(buf, "nn-", NVME_FC_TRADDR_NNLEN) &&	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			!strncmp(&buf[NVME_FC_TRADDR_MIN_PN_OFFSET],	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				"pn-", NVME_FC_TRADDR_NNLEN))) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nnoffset = NVME_FC_TRADDR_NNLEN;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		pnoffset = NVME_FC_TRADDR_MIN_PN_OFFSET + NVME_FC_TRADDR_NNLEN;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	} else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_einval;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	name[0] = '0';	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	name[1] = 'x';	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	name[2 + NVME_FC_TRADDR_HEXNAMELEN] = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	memcpy(&name[2], &buf[nnoffset], NVME_FC_TRADDR_HEXNAMELEN);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (__nvme_fc_parse_u64(&wwn, &traddr->nn))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_einval;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	memcpy(&name[2], &buf[pnoffset], NVME_FC_TRADDR_HEXNAMELEN);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (__nvme_fc_parse_u64(&wwn, &traddr->pn))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_einval;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

out_einval:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	pr_warn("%s: bad traddr string\n", __func__);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return -EINVAL;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static struct nvme_ctrl *	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
nvme_fc_create_ctrl(struct device *dev, struct nvmf_ctrl_options *opts)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_lport *lport;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_fc_rport *rport;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_ctrl *ctrl;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct nvmet_fc_traddr laddr = { 0L, 0L };	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct nvmet_fc_traddr raddr = { 0L, 0L };	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	unsigned long flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = nvme_fc_parse_traddr(&raddr, opts->traddr, NVMF_TRADDR_SIZE);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	if (ret || !raddr.nn || !raddr.pn)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return ERR_PTR(-EINVAL);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	ret = nvme_fc_parse_traddr(&laddr, opts->host_traddr, NVMF_TRADDR_SIZE);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	if (ret || !laddr.nn || !laddr.pn)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return ERR_PTR(-EINVAL);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	/* find the host and remote ports to connect together */
	spin_lock_irqsave(&nvme_fc_lock, flags);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	list_for_each_entry(lport, &nvme_fc_lport_list, port_list) {	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		if (lport->localport.node_name != laddr.nn ||	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		    lport->localport.port_name != laddr.pn ||	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		    lport->localport.port_state != FC_OBJSTATE_ONLINE)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			continue;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		list_for_each_entry(rport, &lport->endp_list, endp_list) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			if (rport->remoteport.node_name != raddr.nn ||	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			    rport->remoteport.port_name != raddr.pn ||	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			    rport->remoteport.port_state != FC_OBJSTATE_ONLINE)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				continue;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

			/* if fail to get reference fall through. Will error */
			if (!nvme_fc_rport_get(rport))	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
				break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

			spin_unlock_irqrestore(&nvme_fc_lock, flags);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

			ctrl = nvme_fc_init_ctrl(dev, opts, lport, rport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
			if (IS_ERR(ctrl))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
				nvme_fc_rport_put(rport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
			return ctrl;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	spin_unlock_irqrestore(&nvme_fc_lock, flags);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	pr_warn("%s: %s - %s combination not found\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		__func__, opts->traddr, opts->host_traddr);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return ERR_PTR(-ENOENT);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */


static struct nvmf_transport_ops nvme_fc_transport = {	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	.name		= "fc",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	.module		= THIS_MODULE,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	.required_opts	= NVMF_OPT_TRADDR | NVMF_OPT_HOST_TRADDR,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	.allowed_opts	= NVMF_OPT_RECONNECT_DELAY | NVMF_OPT_CTRL_LOSS_TMO,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	.create_ctrl	= nvme_fc_create_ctrl,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

/* Arbitrary successive failures max. With lots of subsystems could be high */
#define DISCOVERY_MAX_FAIL	20	/* [한국어] 상수/매크로 — PDU·큐·타임아웃·플래그 */

static ssize_t nvme_fc_nvme_discovery_store(struct device *dev,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		struct device_attribute *attr, const char *buf, size_t count)	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	unsigned long flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	LIST_HEAD(local_disc_list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_lport *lport;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_fc_rport *rport;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	int failcnt = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	spin_lock_irqsave(&nvme_fc_lock, flags);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
restart:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	list_for_each_entry(lport, &nvme_fc_lport_list, port_list) {	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		list_for_each_entry(rport, &lport->endp_list, endp_list) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			if (!nvme_fc_lport_get(lport))	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
				continue;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			if (!nvme_fc_rport_get(rport)) {	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
				/*
				 * This is a temporary condition. Upon restart
				 * this rport will be gone from the list.
				 *
				 * Revert the lport put and retry.  Anything
				 * added to the list already will be skipped (as
				 * they are no longer list_empty).  Loops should
				 * resume at rports that were not yet seen.
				 */
				nvme_fc_lport_put(lport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

				if (failcnt++ < DISCOVERY_MAX_FAIL)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
					goto restart;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

				pr_err("nvme_discovery: too many reference "	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				       "failures\n");	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				goto process_local_list;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
			}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			if (list_empty(&rport->disc_list))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
				list_add_tail(&rport->disc_list,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
					      &local_disc_list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

process_local_list:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	while (!list_empty(&local_disc_list)) {	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		rport = list_first_entry(&local_disc_list,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
					 struct nvme_fc_rport, disc_list);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		list_del_init(&rport->disc_list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		spin_unlock_irqrestore(&nvme_fc_lock, flags);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

		lport = rport->lport;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		/* signal discovery. Won't hurt if it repeats */
		nvme_fc_signal_discovery_scan(lport, rport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		nvme_fc_rport_put(rport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		nvme_fc_lport_put(lport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

		spin_lock_irqsave(&nvme_fc_lock, flags);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	spin_unlock_irqrestore(&nvme_fc_lock, flags);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	return count;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static DEVICE_ATTR(nvme_discovery, 0200, NULL, nvme_fc_nvme_discovery_store);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

#ifdef CONFIG_BLK_CGROUP_FC_APPID	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
/* Parse the cgroup id from a buf and return the length of cgrpid */
static int fc_parse_cgrpid(const char *buf, u64 *id)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	char cgrp_id[16+1];	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int cgrpid_len, j;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	memset(cgrp_id, 0x0, sizeof(cgrp_id));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	for (cgrpid_len = 0, j = 0; cgrpid_len < 17; cgrpid_len++) {	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		if (buf[cgrpid_len] != ':')	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			cgrp_id[cgrpid_len] = buf[cgrpid_len];	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		else {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			j = 1;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (!j)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return -EINVAL;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	if (kstrtou64(cgrp_id, 16, id) < 0)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return -EINVAL;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	return cgrpid_len;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

/*
 * Parse and update the appid in the blkcg associated with the cgroupid.
 */
static ssize_t fc_appid_store(struct device *dev,	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
		struct device_attribute *attr, const char *buf, size_t count)	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	size_t orig_count = count;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	u64 cgrp_id;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int appid_len = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int cgrpid_len = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	char app_id[FC_APPID_LEN];	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int ret = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (buf[count-1] == '\n')	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		count--;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if ((count > (16+1+FC_APPID_LEN)) || (!strchr(buf, ':')))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return -EINVAL;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	cgrpid_len = fc_parse_cgrpid(buf, &cgrp_id);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (cgrpid_len < 0)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return -EINVAL;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	appid_len = count - cgrpid_len - 1;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (appid_len > FC_APPID_LEN)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return -EINVAL;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	memset(app_id, 0x0, sizeof(app_id));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	memcpy(app_id, &buf[cgrpid_len+1], appid_len);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ret = blkcg_set_fc_appid(app_id, cgrp_id, sizeof(app_id));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret < 0)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	return orig_count;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
static DEVICE_ATTR(appid_store, 0200, NULL, fc_appid_store);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
#endif /* CONFIG_BLK_CGROUP_FC_APPID */	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static struct attribute *nvme_fc_attrs[] = {	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	&dev_attr_nvme_discovery.attr,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
#ifdef CONFIG_BLK_CGROUP_FC_APPID	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	&dev_attr_appid_store.attr,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
#endif	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	NULL	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static const struct attribute_group nvme_fc_attr_group = {	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	.attrs = nvme_fc_attrs,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static const struct attribute_group *nvme_fc_attr_groups[] = {	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	&nvme_fc_attr_group,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	NULL	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static struct class fc_class = {	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	.name = "fc",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	.dev_groups = nvme_fc_attr_groups,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int __init nvme_fc_init_module(void)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/*
	 * NOTE:
	 * It is expected that in the future the kernel will combine
	 * the FC-isms that are currently under scsi and now being
	 * added to by NVME into a new standalone FC class. The SCSI
	 * and NVME protocols and their devices would be under this
	 * new FC class.
	 *
	 * As we need something to post FC-specific udev events to,
	 * specifically for nvme probe events, start by creating the
	 * new device class.  When the new standalone FC class is
	 * put in place, this code will move to a more generic
	 * location for the class.
	 */
	ret = class_register(&fc_class);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		pr_err("couldn't register class fc\n");	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/*
	 * Create a device for the FC-centric udev events
	 */
	fc_udev_device = device_create(&fc_class, NULL, MKDEV(0, 0), NULL,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				"fc_udev_device");	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (IS_ERR(fc_udev_device)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		pr_err("couldn't create fc_udev device!\n");	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ret = PTR_ERR(fc_udev_device);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_destroy_class;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = nvmf_register_transport(&nvme_fc_transport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_destroy_device;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

out_destroy_device:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	device_destroy(&fc_class, MKDEV(0, 0));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
out_destroy_class:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	class_unregister(&fc_class);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_fc_delete_controllers(struct nvme_fc_rport *rport)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_ctrl *ctrl;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	spin_lock(&rport->lock);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
	list_for_each_entry(ctrl, &rport->ctrl_list, ctrl_list) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		dev_warn(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"NVME-FC{%d}: transport unloading: deleting ctrl\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			ctrl->cnum);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_delete_ctrl(&ctrl->ctrl);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	spin_unlock(&rport->lock);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void __exit nvme_fc_exit_module(void)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_lport *lport;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_fc_rport *rport;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	unsigned long flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	spin_lock_irqsave(&nvme_fc_lock, flags);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	list_for_each_entry(lport, &nvme_fc_lport_list, port_list)	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		list_for_each_entry(rport, &lport->endp_list, endp_list)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvme_fc_delete_controllers(rport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	spin_unlock_irqrestore(&nvme_fc_lock, flags);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	flush_workqueue(nvme_delete_wq);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	nvmf_unregister_transport(&nvme_fc_transport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	device_destroy(&fc_class, MKDEV(0, 0));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	class_unregister(&fc_class);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

module_init(nvme_fc_init_module);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
module_exit(nvme_fc_exit_module);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

MODULE_DESCRIPTION("NVMe host FC transport driver");	/* [한국어] 모듈 경계·파라미터·심볼 공개 */
MODULE_LICENSE("GPL v2");	/* [한국어] 모듈 경계·파라미터·심볼 공개 */
