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
 *
 * === 전체 아키텍처에서의 위치 ===
 * NVMe over RDMA 트랜스포트다. TCP 와 달리 데이터가 CPU 를 거치지 않는다 -- 원격
 * 장치가 메모리 영역에 직접 읽고 쓴다. 그래서 이 파일의 일은 '전송'보다 '등록'에
 * 가깝다. 요청마다 메모리 영역을 등록해 원격에 rkey 를 넘기고, 완료 후 무효화한다.
 * 호출 체인(제출):
 *   blk-mq → nvme_rdma_queue_rq → nvme_rdma_map_data (MR 등록)
 *     → ib_post_send (명령 캡슐 전송)
 *       → 원격이 RDMA Read/Write 로 데이터를 직접 옮김
 * 호출 체인(완료):
 *   CQ 이벤트 → nvme_rdma_recv_done / _send_done → MR 무효화 확인
 *     → nvme_complete_rq
 * 큐마다 QP(Queue Pair) 하나와 CQ 하나가 대응한다. 완료는 ib_cq 의 폴링 문맥에서
 * 처리되며, poll 큐는 blk-mq 의 poll 경로에서 같은 CQ 를 직접 훑는다.
 *
 * === 타 모듈과의 연결 ===
 * - drivers/infiniband/core: ib_verbs API(QP/CQ/MR/PD)와 RDMA CM. 연결 수립부터
 *   메모리 등록까지 모두 이쪽 인터페이스를 쓴다.
 * - drivers/nvme/host/fabrics.c: 연결 옵션과 Connect 명령. "rdma" 이름으로 등록한다.
 * - drivers/nvme/host/core.c: 상태 기계와 Identify 를 위임한다.
 * - block layer: MR 등록 가능한 세그먼트 수가 queue_limits 의 max_segments 를
 *   좌우한다. RDMA 는 한 요청이 하나의 MR 로 표현되는 것이 이상적이라,
 *   세그먼트가 많으면 등록 비용이 올라간다.
 * 데이터 흐름에서 이 파일이 실제로 옮기는 것은 명령 캡슐과 완료뿐이고,
 * 페이로드는 하드웨어가 직접 옮긴다는 점이 다른 트랜스포트와의 가장 큰 차이다.
 *
 * === 주요 함수/구조체 요약 ===
 * - nvme_rdma_queue_rq: 핫패스 제출. 명령을 캡슐에 담고 필요하면 MR 을 등록한 뒤
 *   ib_post_send 로 내보낸다.
 * - nvme_rdma_map_data / nvme_rdma_unmap_data: 요청 데이터를 RDMA 가 접근할 수 있는
 *   형태로 만든다. 인라인으로 보낼지, 단일 SGE 로 보낼지, MR 을 등록할지를 크기와
 *   세그먼트 수를 보고 고른다.
 * - nvme_rdma_recv_done / nvme_rdma_send_done: CQ 완료 콜백. 응답 캡슐을 해석하고
 *   MR 무효화가 끝났는지 확인한 뒤 요청을 완료시킨다.
 * - nvme_rdma_create_queue_ib / nvme_rdma_destroy_queue_ib / nvme_rdma_create_qp:
 *   큐별 RDMA 자원(PD, CQ, QP, MR 풀)의 생성과 해제.
 * - nvme_rdma_cm_handler: RDMA CM 이벤트 처리. ADDR_RESOLVED → ROUTE_RESOLVED →
 *   ESTABLISHED 로 이어지는 연결 수립과, DISCONNECTED 같은 단절 통지를 받는다.
 * - nvme_rdma_setup_ctrl / nvme_rdma_teardown_io_queues: 컨트롤러 단위 큐 구성과 해체.
 * - nvme_rdma_error_recovery / _error_recovery_work / _reconnect_ctrl_work:
 *   링크 단절 후 복구 절차.
 * - struct nvme_rdma_queue: 큐 하나의 RDMA 자원 -- QP, CQ, CM ID, MR 풀, 플래그.
 * - struct nvme_rdma_request: 요청별 등록 상태 -- 사용한 MR, sg 테이블, 캡슐 버퍼.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt	/* [한국어] pr_fmt 재정의 — 이 파일의 모든 pr_* 로그 앞에 "nvme_rdma: " 가 붙는다 */
#include <linux/module.h>	/* [한국어] module_init/exit, MODULE_* 매크로 — 이 파일은 nvme-rdma.ko 로 빌드된다 */
#include <linux/init.h>	/* [한국어] __init/__exit 섹션 표시 */
#include <linux/slab.h>	/* [한국어] kmalloc/kfree — 큐 배열과 응답 링 할당 */
#include <rdma/mr_pool.h>	/* [한국어] ib_mr_pool_* — QP 마다 MR 을 미리 잡아 두는 풀. 요청마다 MR 을 새로 만들면 등록 비용이 감당되지 않는다 */
#include <linux/err.h>	/* [한국어] IS_ERR/PTR_ERR — RDMA API 는 오류를 포인터에 실어 돌려주는 관례를 쓴다 */
#include <linux/string.h>	/* [한국어] memcpy/strcmp 등 문자열·메모리 헬퍼 */
#include <linux/atomic.h>	/* [한국어] atomic_t — 큐 상태 플래그와 참조 계수 */
#include <linux/blk-mq.h>	/* [한국어] blk-mq tagset/hctx/request — 이 트랜스포트가 블록 계층에 붙는 접점 */
#include <linux/blk-integrity.h>	/* [한국어] blk_integrity — T10-PI 보호 정보. RDMA 는 signature MR 로 이를 오프로드한다 */
#include <linux/types.h>	/* [한국어] u8/u16/__le16 등 고정폭 타입 */
#include <linux/list.h>	/* [한국어] list_head — 장치 목록과 컨트롤러 목록 */
#include <linux/mutex.h>	/* [한국어] mutex — 장치 목록과 컨트롤러 목록의 보호 */
#include <linux/scatterlist.h>	/* [한국어] scatterlist — 요청 데이터를 페이지 조각 목록으로 표현 */
#include <linux/nvme.h>	/* [한국어] NVMe 스펙 구조체와 상수 (nvme_command, NVME_SC_* 등) */
#include <linux/unaligned.h>	/* [한국어] get_unaligned_le* — 캡슐 안 정렬되지 않은 필드 읽기 */

#include <rdma/ib_verbs.h>	/* [한국어] ib_verbs 핵심 API — QP/CQ/MR/PD 생성과 WR 게시. 이 파일의 실질적 하위 계층 */
#include <rdma/rdma_cm.h>	/* [한국어] RDMA Connection Manager — 주소 해석, 라우팅, 연결 수립/단절 이벤트 */
#include <linux/nvme-rdma.h>	/* [한국어] NVMe-over-RDMA 와이어 포맷 — 명령 캡슐과 SGL 서술자 레이아웃 */

#include "nvme.h"	/* [한국어] nvme_ctrl / nvme_ctrl_ops 등 코어 계약. 이 파일이 채워 등록한다 */
#include "fabrics.h"	/* [한국어] Fabrics 공통부 — 연결 옵션 파싱과 Connect/Property 명령 */


#define NVME_RDMA_CM_TIMEOUT_MS		3000		/* 3 second */	/* [한국어] RDMA CM 연산의 타임아웃(ms). 주소 해석과 라우팅 각 단계에 적용된다 */

#define NVME_RDMA_MAX_SEGMENTS		256	/* [한국어] 한 요청이 가질 수 있는 최대 세그먼트 수. 이 값이 queue_limits 의 max_segments 가 되어 블록 계층의 bio 분할 단위를 정한다 */

#define NVME_RDMA_MAX_INLINE_SEGMENTS	4	/* [한국어] 인라인으로 실을 수 있는 최대 세그먼트 수. 이보다 작은 전송은 MR 등록 없이 명령 캡슐에 데이터를 함께 담아 왕복을 줄인다 */

/* [한국어] 요청에 미리 박아 두는 데이터 scatterlist 의 바이트 크기.
 * 이만큼은 별도 할당 없이 요청 구조체 안에서 해결되므로, 세그먼트가 적은
 * 흔한 요청은 추가 할당 없이 처리된다. 줄 연속(\) 뒤에는 주석을 둘 수 없어
 * 매크로 위에 적는다. */
#define NVME_RDMA_DATA_SGL_SIZE \
	(sizeof(struct scatterlist) * NVME_INLINE_SG_CNT)
/* [한국어] 메타데이터(보호 정보) scatterlist 의 바이트 크기. 데이터와 별도
 * SGL 을 쓰기 때문에 따로 잡는다. T10-PI 가 꺼진 네임스페이스에서는 쓰이지 않는다. */
#define NVME_RDMA_METADATA_SGL_SIZE \
	(sizeof(struct scatterlist) * NVME_INLINE_METADATA_SG_CNT)

/* [한국어] 하나의 RDMA 장치(HCA)를 나타내는 공유 객체.
 * 여러 컨트롤러가 같은 HCA 를 쓸 수 있으므로 장치별로 하나만 두고 참조 계수로
 * 공유한다. PD 는 장치 단위 자원이라 컨트롤러마다 새로 만들 이유가 없다. */
struct nvme_rdma_device {
	/* [한국어] ib_verbs 가 보는 실제 HCA.
	 * 설정자: nvme_rdma_find_get_device() 가 CM ID 의 verbs 포인터에서 얻는다.
	 * 읽는 자: DMA 매핑, MR 등록, QP 생성 등 모든 verbs 호출의 첫 인자.
	 * 값 범위: 유효한 포인터. 장치가 제거되면 이 객체 전체가 해제된다.
	 * 동기화: device_list_mutex 가 목록을, ref 가 수명을 지킨다. */
	struct ib_device	*dev;
	/* [한국어] Protection Domain — 이 장치에서 만드는 MR 과 QP 가 속하는 보호 영역.
	 * 같은 PD 안의 자원끼리만 서로를 참조할 수 있어, 컨트롤러 사이의 격리 경계가 된다.
	 * 설정자: nvme_rdma_find_get_device() 가 ib_alloc_pd() 로 한 번 만든다.
	 * 읽는 자: MR 풀 초기화와 QP 생성.
	 * 동기화: 생성 후 불변. */
	struct ib_pd		*pd;
	/* [한국어] 이 장치 객체의 참조 계수.
	 * 왜 필요한가: 여러 컨트롤러가 같은 HCA 를 공유하므로, 마지막 사용자가
	 *   사라질 때만 PD 를 풀어야 한다.
	 * 설정자/읽는 자: nvme_rdma_find_get_device 가 올리고
	 *   nvme_rdma_dev_put 이 내린다. 0 이 되면 nvme_rdma_free_dev 가 불린다.
	 * 동기화: kref 자체가 원자적이지만, 목록에서 빼는 일은 device_list_mutex 아래서 한다. */
	struct kref		ref;
	/* [한국어] 전역 device_list 에 매달리기 위한 연결 고리.
	 * 설정자: 장치를 처음 만들 때 list_add.
	 * 읽는 자: nvme_rdma_find_get_device 가 같은 HCA 가 이미 있는지 훑을 때.
	 * 동기화: device_list_mutex. */
	struct list_head	entry;
	/* [한국어] 이 HCA 가 한 WR 에 실을 수 있는 인라인 세그먼트 수.
	 * 왜 중요한가: 인라인으로 보낼 수 있으면 MR 등록 없이 명령 캡슐에 데이터를
	 *   함께 담아 왕복을 줄일 수 있다. 작은 I/O 의 지연이 여기서 갈린다.
	 * 설정자: 장치 생성 시 HCA 의 max_send_sge 와 NVME_RDMA_MAX_INLINE_SEGMENTS
	 *   중 작은 값으로 정한다.
	 * 읽는 자: nvme_rdma_map_sg_inline 이 인라인 가능 여부를 판정할 때.
	 * 동기화: 생성 후 불변. */
	unsigned int		num_inline_segments;
};

/* [한국어] Queue Element — DMA 로 주고받는 버퍼 하나와 그 완료 콜백을 묶은 것.
 * 명령 캡슐(송신)과 응답 캡슐(수신) 양쪽에 같은 모양이 쓰인다. */
struct nvme_rdma_qe {
	/* [한국어] 이 버퍼에 대한 완료가 올라올 때 불릴 콜백.
	 * 왜 구조체 안에 두나: ib_verbs 는 완료 큐 항목에 ib_cqe 포인터만 실어 주므로,
	 *   콜백에서 container_of 로 이 qe 를 되찾는 것이 유일한 연결 수단이다.
	 * 설정자: 송신은 nvme_rdma_send_done, 수신은 nvme_rdma_recv_done 을 건다.
	 * 읽는 자: ib_poll_cq 가 완료를 꺼내며 이 함수를 부른다.
	 * 동기화: 완료 처리 문맥에서만 다뤄진다. */
	struct ib_cqe		cqe;
	/* [한국어] 캡슐 내용이 담긴 커널 가상 주소.
	 * 설정자: nvme_rdma_alloc_qe 가 kzalloc 으로 잡는다.
	 * 읽는 자: 송신 전 명령을 채울 때, 수신 후 응답을 읽을 때.
	 * 값 범위: 캡슐 크기(보통 명령 64B, 응답 16B + 여유)만큼 유효.
	 * 동기화: 요청 하나에 묶여 있어 공유되지 않는다. */
	void			*data;
	/* [한국어] 위 버퍼를 HCA 가 접근할 수 있게 매핑한 DMA 주소.
	 * 왜 따로 두나: HCA 는 CPU 가상 주소를 모르므로 WR 의 SGE 에는 이 값을 실어야 한다.
	 * 설정자: nvme_rdma_alloc_qe 의 ib_dma_map_single.
	 * 읽는 자: WR 을 조립할 때, 그리고 해제 시 ib_dma_unmap_single.
	 * 동기화: 매핑 이후 불변. */
	u64			dma;
};

/* [한국어] 요청 데이터를 표현하는 scatterlist 와 그 유효 항목 수를 묶은 것.
 * 데이터용과 메타데이터용으로 각각 하나씩 쓰인다. */
struct nvme_rdma_sgl {
	/* [한국어] DMA 매핑을 마친 뒤 실제로 유효한 항목 수.
	 * 왜 sg_table.nents 와 따로 두나: ib_dma_map_sg 는 인접한 조각을 합칠 수 있어
	 *   매핑 후 개수가 매핑 전보다 줄어들 수 있다. 이 값이 매핑 후 개수이고,
	 *   WR 을 조립하거나 MR 을 등록할 때 기준이 되는 쪽이다.
	 * 설정자: nvme_rdma_map_data 의 ib_dma_map_sg 반환값.
	 * 읽는 자: 인라인/단일 SGE/MR 등록 중 무엇을 쓸지 고르는 판정과 해제 경로.
	 * 동기화: 요청 단위. */
	int			nents;
	/* [한국어] 요청의 데이터 조각 목록. 블록 계층의 bio 를 훑어 채운다.
	 * 설정자: nvme_rdma_map_data 가 sg_alloc_table_chained 로 잡고
	 *   blk_rq_map_sg 로 채운다.
	 * 읽는 자: DMA 매핑, MR 등록, 인라인 복사 경로.
	 * 값 범위: 항목 수는 NVME_RDMA_MAX_SEGMENTS 이하. 작은 요청은 요청 구조체
	 *   안의 예약 공간(NVME_RDMA_DATA_SGL_SIZE)으로 해결돼 별도 할당이 없다.
	 * 동기화: 요청 단위. */
	struct sg_table		sg_table;
};

struct nvme_rdma_queue;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
/* [한국어] 요청 하나의 RDMA 측 상태 전부.
 * blk-mq 가 태그마다 이 구조체를 하나씩 미리 잡아 두고(요청당 동적 할당 없음),
 * nvme_rdma_queue_rq 가 채워 하드웨어에 넘긴 뒤 완료에서 되찾는다. */
struct nvme_rdma_request {
	/* [한국어] NVMe 코어가 보는 요청 부분. 반드시 맨 앞에 있어야 한다 --
	 * blk_mq_rq_to_pdu() 가 돌려주는 주소가 곧 이 필드의 주소이고,
	 * 코어는 그것을 struct nvme_request 로 읽기 때문이다.
	 * 설정자: nvme_rdma_init_request / nvme_setup_cmd.
	 * 읽는 자: nvme_complete_rq 등 코어의 완료·재시도 판정.
	 * 동기화: 요청 하나에 묶여 있어 공유되지 않는다. */
	struct nvme_request	req;
	/* [한국어] 이 요청의 데이터를 원격에 노출하기 위해 등록한 메모리 영역.
	 * 왜 필요한가: RDMA 는 원격이 직접 읽고 쓰므로, 대상 메모리를 HCA 에
	 *   등록해 rkey 를 받아야 한다. 그 rkey 를 명령 캡슐의 SGL 에 실어 보낸다.
	 * 설정자: nvme_rdma_map_sg_fr 이 MR 풀(ib_mr_pool_get)에서 하나 꺼내 온다.
	 * 읽는 자: 완료 후 ib_mr_pool_put 으로 반납하는 경로.
	 * 값 범위: 인라인이나 단일 SGE 로 처리된 요청에서는 NULL 이다.
	 * 동기화: 요청 단위. 반납 전에 무효화가 끝났는지 ref 로 확인한다. */
	struct ib_mr		*mr;
	/* [한국어] 이 요청의 명령 캡슐(SQE)을 담아 보낼 송신 버퍼.
	 * 설정자: nvme_rdma_init_request 가 큐 생성 시 미리 할당·매핑한다.
	 * 읽는 자: nvme_rdma_queue_rq 가 명령을 채워 ib_post_send 로 내보낸다.
	 * 동기화: 요청 단위. 완료 전까지 HCA 가 읽으므로 건드리면 안 된다. */
	struct nvme_rdma_qe	sqe;
	/* [한국어] 응답 캡슐에서 꺼낸 명령 결과(cdw0 등).
	 * 왜 따로 보관하나: 응답 수신과 MR 무효화 완료가 순서 없이 도착할 수 있어,
	 *   둘 다 끝난 시점에 요청을 완료시켜야 한다. 그때까지 결과를 들고 있는다.
	 * 설정자: nvme_rdma_recv_done.
	 * 읽는 자: nvme_rdma_end_request 가 blk-mq 에 넘길 때.
	 * 동기화: 요청 단위. ref 가 0 이 되는 시점이 경계다. */
	union nvme_result	result;
	/* [한국어] 응답 캡슐의 NVMe 상태 코드. result 와 같은 이유로 보관된다.
	 * 값 범위: NVME_SC_* . 0 이면 성공.
	 * 동기화: 요청 단위. */
	__le16			status;
	/* [한국어] 이 요청을 완료시키기 위해 아직 기다려야 할 이벤트 수.
	 * 왜 필요한가: 응답 수신과 MR 무효화(local invalidate) 완료가 각각
	 *   독립적으로 도착한다. 둘 다 끝나야 메모리 노출이 닫힌 것이므로,
	 *   먼저 온 쪽이 요청을 완료시켜 버리면 원격이 아직 접근 가능한 상태에서
	 *   버퍼가 반환될 수 있다.
	 * 설정자: 제출 시 1 또는 2 로 세운다(MR 을 썼으면 2).
	 * 읽는 자: nvme_rdma_end_request 가 0 으로 내려간 순간에만 완료시킨다.
	 * 동기화: refcount_t 자체가 원자적이다. */
	refcount_t		ref;
	/* [한국어] 송신 WR 에 실을 scatter/gather 항목들.
	 * 첫 칸은 늘 명령 캡슐(sqe)이고, 뒤따르는 칸은 인라인으로 함께 보내는
	 * 데이터 조각이다. 그래서 크기가 1 + 최대 인라인 세그먼트 수다.
	 * 설정자: nvme_rdma_map_sg_inline 이 데이터 칸을 채운다.
	 * 읽는 자: ib_post_send.
	 * 동기화: 요청 단위. */
	struct ib_sge		sge[1 + NVME_RDMA_MAX_INLINE_SEGMENTS];
	/* [한국어] 위 sge 중 실제로 채워진 개수. 인라인을 쓰지 않으면 1(캡슐만)이다.
	 * 설정자/읽는 자: map 경로가 세우고 ib_post_send 가 읽는다.
	 * 동기화: 요청 단위. */
	u32			num_sge;
	/* [한국어] MR 등록을 요청하는 Work Request.
	 * 왜 요청 안에 두나: 등록도 WR 로 큐에 올려야 하므로, 명령 전송 WR 과
	 *   체인으로 묶어 한 번에 게시한다. 별도 왕복 없이 등록과 제출이 함께 간다.
	 * 설정자: nvme_rdma_map_sg_fr.
	 * 읽는 자: ib_post_send.
	 * 동기화: 요청 단위. */
	struct ib_reg_wr	reg_wr;
	/* [한국어] 위 등록 WR 의 완료 콜백.
	 * 설정자: nvme_rdma_memreg_done 을 건다.
	 * 읽는 자: CQ 폴링이 등록 완료를 꺼낼 때.
	 * 동기화: 완료 처리 문맥. */
	struct ib_cqe		reg_cqe;
	/* [한국어] 이 요청이 속한 큐로 돌아가는 포인터.
	 * 왜 필요한가: 완료 콜백은 ib_cqe 만 받으므로, 거기서 요청을 되찾은 뒤
	 *   큐 자원(장치, QP, MR 풀)에 닿으려면 이 링크가 있어야 한다.
	 * 설정자: nvme_rdma_init_request.
	 * 동기화: 초기화 후 불변. */
	struct nvme_rdma_queue  *queue;
	/* [한국어] 요청 데이터의 scatterlist. 요청 구조체 안에 값으로 품어
	 * 흔한 크기의 요청은 추가 할당 없이 처리된다.
	 * 설정자/읽는 자: nvme_rdma_map_data / _unmap_data.
	 * 동기화: 요청 단위. */
	struct nvme_rdma_sgl	data_sgl;
	/* [한국어] 보호 정보(T10-PI) 메타데이터용 scatterlist.
	 * 왜 포인터인가: PI 를 쓰는 네임스페이스에서만 필요하므로, 쓰지 않는
	 *   구성에서 요청 구조체를 키우지 않으려고 별도 할당해 매단다.
	 * 값 범위: PI 가 없으면 NULL.
	 * 동기화: 요청 단위. */
	struct nvme_rdma_sgl	*metadata_sgl;
	/* [한국어] 이 요청이 signature MR 을 쓰는지.
	 * 무엇인가: signature MR 은 등록과 동시에 HCA 가 PI 를 생성·검증하게 하는
	 *   특수 MR 이다. 켜지면 CPU 가 체크섬을 계산하지 않고 HCA 에 맡긴다.
	 * 설정자: nvme_rdma_queue_rq 가 요청의 무결성 여부를 보고 정한다.
	 * 읽는 자: map/unmap 경로가 일반 MR 과 다른 절차를 탈지 가른다.
	 * 동기화: 요청 단위. */
	bool			use_sig_mr;
};

/* [한국어] 큐의 생애 단계를 나타내는 비트 위치. 세 단계가 순서대로 켜지고
 * 해체할 때 역순으로 꺼진다. 비트로 둔 것은 test_and_set/clear 로 경합 없이
 * "내가 처음 끄는 쪽인가"를 판정하기 위해서다. */
enum nvme_rdma_queue_flags {
	/* [한국어] 큐 구조체와 CM ID 가 만들어졌다. 아직 QP 도 연결도 없다.
	 * 이 비트가 서 있어야 해체 경로가 무엇이든 정리할 대상이 있다고 본다. */
	NVME_RDMA_Q_ALLOCATED		= 0,
	/* [한국어] Fabrics Connect 까지 끝나 이 큐로 명령을 보낼 수 있다.
	 * 설정자: nvme_rdma_start_queue 가 연결 성공 후 세운다.
	 * 읽는 자: queue_rq 가 이 비트를 보고 아직 준비 안 된 큐의 요청을 거른다. */
	NVME_RDMA_Q_LIVE		= 1,
	/* [한국어] 트랜스포트 자원(QP, CQ, MR 풀)이 준비됐다.
	 * 왜 LIVE 와 나누나: 자원은 섰지만 Connect 는 아직인 구간이 존재하고,
	 *   해체할 때 그 구간에서 들어오면 QP 만 정리해야 하기 때문이다. */
	NVME_RDMA_Q_TR_READY		= 2,
};

/* [한국어] 큐 하나가 소유하는 RDMA 자원 일체.
 * NVMe 의 SQ/CQ 쌍에 대응하지만, 여기서는 QP 하나와 CQ 하나가 그 역할을 한다. */
struct nvme_rdma_queue {
	/* [한국어] 미리 게시해 두는 응답 수신 버퍼의 링.
	 * 왜 미리 게시하나: RDMA 는 수신 버퍼가 이미 올라와 있어야 상대가 보낼 수
	 *   있다. 응답이 올 때 버퍼를 준비하는 것은 늦다.
	 * 설정자: nvme_rdma_alloc_qe 로 큐 크기만큼 잡고 ib_post_recv 로 올린다.
	 * 읽는 자: nvme_rdma_recv_done 이 도착한 응답을 여기서 읽는다.
	 * 동기화: 완료 처리 문맥에서 소비하고 즉시 다시 게시한다. */
	struct nvme_rdma_qe	*rsp_ring;
	/* [한국어] 이 큐가 동시에 처리할 수 있는 요청 수. blk-mq 태그 깊이와 같다.
	 * 설정자: 컨트롤러 설정에서 admin 은 작게, I/O 는 옵션 값으로 정해진다.
	 * 읽는 자: 수신 링 크기와 QP 의 WR 개수를 정하는 근거. */
	int			queue_size;
	/* [한국어] 명령 캡슐의 크기. 인라인 데이터를 함께 실으면 그만큼 커진다.
	 * 왜 큐마다 다른가: admin 큐는 인라인을 쓰지 않아 SQE 크기 그대로이고,
	 *   I/O 큐는 인라인 여유를 더한 크기다.
	 * 읽는 자: 송신 버퍼 할당과 인라인 가능 여부 판정. */
	size_t			cmnd_capsule_len;
	/* [한국어] 이 큐가 속한 컨트롤러로 돌아가는 포인터.
	 * 동기화: 초기화 후 불변. */
	struct nvme_rdma_ctrl	*ctrl;
	/* [한국어] 이 큐가 쓰는 HCA. 여러 큐가 같은 장치를 공유하며 참조 계수로 관리된다.
	 * 설정자: nvme_rdma_create_queue_ib 가 CM ID 에서 찾아 참조를 올린다.
	 * 동기화: 큐 수명 동안 유지. */
	struct nvme_rdma_device	*device;
	/* [한국어] 이 큐의 완료 큐. 송신·수신·MR 등록 완료가 모두 여기로 올라온다.
	 * 설정자: ib_alloc_cq 로 만든다. poll 큐면 IB_POLL_DIRECT 로,
	 *   아니면 소프트IRQ 폴링 모드로 만든다 -- 그 차이가 인터럽트 유무다.
	 * 동기화: ib_verbs 코어가 폴링을 직렬화한다. */
	struct ib_cq		*ib_cq;
	/* [한국어] Queue Pair -- 송신 큐와 수신 큐의 쌍. WR 은 전부 여기에 게시된다.
	 * 설정자: nvme_rdma_create_qp.
	 * 동기화: ib_post_send 는 자체적으로 안전하지만, 이 드라이버는 요청마다
	 *   독립된 WR 을 올리므로 추가 잠금을 두지 않는다. */
	struct ib_qp		*qp;

	/* [한국어] 위 enum nvme_rdma_queue_flags 의 비트 집합.
	 * 동기화: test_and_set_bit / test_and_clear_bit 으로만 다뤄 경합을 피한다. */
	unsigned long		flags;
	/* [한국어] RDMA Connection Manager 식별자. 주소 해석부터 연결 수립까지의
	 * 모든 CM 이벤트가 이것에 묶여 온다.
	 * 설정자: rdma_create_id.
	 * 읽는 자: nvme_rdma_cm_handler 가 이벤트에서 큐를 되찾는 통로이기도 하다. */
	struct rdma_cm_id	*cm_id;
	/* [한국어] CM 이벤트 처리 결과. 콜백은 값을 돌려줄 수 없으므로 여기에 남긴다.
	 * 설정자: nvme_rdma_cm_handler.
	 * 읽는 자: 아래 cm_done 을 기다리던 쪽이 깨어나 이 값을 본다.
	 * 값 범위: 0 이면 성공, 음수면 errno. */
	int			cm_error;
	/* [한국어] CM 단계 하나가 끝났음을 알리는 완료 객체.
	 * 왜 필요한가: 주소 해석 → 라우팅 해석 → 연결 수립은 각각 비동기 콜백으로
	 *   끝난다. 호출자는 단계마다 여기서 기다린다.
	 * 동기화: completion 자체가 동기화 수단이다. */
	struct completion	cm_done;
	/* [한국어] 이 큐가 보호 정보(T10-PI) 오프로드를 쓸 수 있는지.
	 * 설정자: 장치 능력과 네임스페이스 설정을 보고 큐 생성 시 정한다.
	 * 읽는 자: signature MR 을 쓸지 가르는 판정. */
	bool			pi_support;
	/* [한국어] CQ 에 잡아 둔 항목 수.
	 * 왜 별도로 기억하나: 큐 깊이와 1:1 이 아니다. 요청마다 송신·수신·등록·
	 *   무효화 완료가 올라올 수 있어 그만큼 여유를 두고 잡는다. 재연결 시
	 *   같은 크기로 다시 만들기 위해 값을 남긴다. */
	int			cq_size;
	/* [한국어] 이 큐의 시작/정지를 직렬화하는 뮤텍스.
	 * 왜 필요한가: 오류 복구와 정상 해체가 동시에 같은 큐를 내리려 할 수 있다.
	 *   QP 파괴는 두 번 하면 안 되므로 한 쪽만 통과시켜야 한다.
	 * 동기화: 잠자는 잠금이라 완료 콜백 문맥에서는 잡을 수 없다. */
	struct mutex		queue_lock;
};

/* [한국어] RDMA 컨트롤러 하나. nvme_ctrl 을 값으로 품어
 * to_rdma_ctrl() 이 container_of 로 되찾을 수 있게 한다. */
struct nvme_rdma_ctrl {
	/* read only in the hot path */
	/* [한국어] 큐 배열. 0 번이 admin, 1 번부터 I/O 큐다.
	 * 위 영어 주석대로 핫패스에서는 읽기만 하므로 잠금 없이 접근한다.
	 * 설정자: nvme_rdma_alloc_io_queues 가 협상된 개수만큼 잡는다. */
	struct nvme_rdma_queue	*queues;

	/* other member variables */
	/* [한국어] I/O 큐용 blk-mq 태그셋. 태그 하나가 곧 동시 처리 가능한 요청
	 * 하나이며, 태그마다 nvme_rdma_request 가 미리 붙어 있다. */
	struct blk_mq_tag_set	tag_set;
	/* [한국어] 오류 복구 작업. 링크가 끊기거나 타임아웃이 나면 여기로 넘긴다.
	 * 왜 워크큐인가: 복구는 큐를 내리고 다시 세우는 잠들 수 있는 작업이라
	 *   완료 콜백이나 인터럽트 문맥에서 직접 할 수 없다. */
	struct work_struct	err_work;

	/* [한국어] 비동기 이벤트(AEN) 전용 명령 캡슐 버퍼.
	 * 왜 따로 두나: AEN 은 태그를 소비하지 않는 상주 명령이라 일반 요청의
	 *   태그별 버퍼를 쓸 수 없다. 컨트롤러당 하나만 있으면 된다. */
	struct nvme_rdma_qe	async_event_sqe;

	/* [한국어] 재연결 시도 작업. 지연 워크라 옵션에 적힌 간격만큼 쉬었다 돈다.
	 * 읽는 자: nvme_rdma_reconnect_ctrl_work. 시도 횟수가 소진되면 삭제로 간다. */
	struct delayed_work	reconnect_work;

	/* [한국어] 전역 nvme_rdma_ctrl_list 에 매달리기 위한 고리.
	 * 동기화: nvme_rdma_ctrl_mutex. */
	struct list_head	list;

	/* [한국어] admin 큐 전용 태그셋. I/O 와 분리해 두어야 I/O 큐가 모두
	 * 막힌 상태에서도 리셋·삭제 같은 admin 명령이 통과할 수 있다. */
	struct blk_mq_tag_set	admin_tag_set;
	/* [한국어] 이 컨트롤러가 쓰는 HCA. 큐들이 공유하는 것과 같은 객체다. */
	struct nvme_rdma_device	*device;

	/* [한국어] 하나의 MR 이 등록할 수 있는 최대 페이지 수.
	 * 왜 중요한가: 이 값이 한 요청이 MR 하나로 표현 가능한 크기의 상한이고,
	 *   따라서 queue_limits 의 max_hw_sectors 를 좌우한다. 장치의
	 *   max_fast_reg_page_list_len 에서 온다. */
	u32			max_fr_pages;

	/* [한국어] 접속할 타겟의 주소. 'nvme connect' 의 traddr/trsvcid 가 여기 담긴다.
	 * 읽는 자: rdma_resolve_addr 이 이 주소로 경로를 찾는다. */
	struct sockaddr_storage addr;
	/* [한국어] 출발지 주소. host-traddr 로 지정하면 특정 로컬 포트를 강제한다.
	 * 값 범위: 지정하지 않으면 0 으로 남고 커널이 라우팅으로 고른다. */
	struct sockaddr_storage src_addr;

	/* [한국어] 코어가 보는 컨트롤러. 상태 기계와 Identify 결과가 여기 있다.
	 * 위치가 중요하다 -- to_rdma_ctrl() 이 이 필드에서 바깥을 되찾는다. */
	struct nvme_ctrl	ctrl;
	/* [한국어] 이 연결에서 인라인 데이터 전송을 쓸 수 있는지.
	 * 설정자: 타겟이 광고한 인라인 크기와 장치 능력을 보고 정한다.
	 * 읽는 자: map 경로가 인라인/단일 SGE/MR 중 무엇을 고를지 판정할 때. */
	bool			use_inline_data;
	/* [한국어] 기본·읽기 전용·폴링 큐 각각의 개수.
	 * 왜 나누나: blk-mq 는 큐를 용도별 맵으로 나눠 쓴다. 읽기 전용 큐를 두면
	 *   쓰기가 읽기 지연을 밀어내지 않고, 폴링 큐는 인터럽트 없이 돈다.
	 * 설정자: nvme_rdma_alloc_io_queues 가 옵션과 협상 결과로 채운다. */
	u32			io_queues[HCTX_MAX_TYPES];
};

static inline struct nvme_rdma_ctrl *to_rdma_ctrl(struct nvme_ctrl *ctrl)	/* [한국어] 코어가 넘겨준 nvme_ctrl 에서 이 트랜스포트의 바깥 구조체를 되찾는다 */
{
	/* [한국어] nvme_ctrl 은 nvme_rdma_ctrl 안에 값으로 박혀 있으므로,
	 * 그 필드 주소에서 바깥 구조체 시작으로 되돌아갈 수 있다. 코어는 항상
	 * nvme_ctrl 포인터만 넘겨주기 때문에 트랜스포트 진입점마다 이 변환이 첫 줄에 온다. */
	return container_of(ctrl, struct nvme_rdma_ctrl, ctrl);
}

static LIST_HEAD(device_list);	/* [한국어] 같은 IB 장치를 여러 컨트롤러가 공유한다 — PD 와 MR 풀을 한 번만 만들기 위한 목록 */
static DEFINE_MUTEX(device_list_mutex);	/* [한국어] 그 목록의 조회와 등록을 직렬화한다 */

static LIST_HEAD(nvme_rdma_ctrl_list);	/* [한국어] 중복 연결 검사와 모듈 언로드 시 일괄 삭제에 쓰인다 */
static DEFINE_MUTEX(nvme_rdma_ctrl_mutex);	/* [한국어] 그 목록을 보호한다 */

/*
 * Disabling this option makes small I/O goes faster, but is fundamentally
 * unsafe.  With it turned off we will have to register a global rkey that
 * allows read and write access to all physical memory.
 */
static bool register_always = true;	/* [한국어] 기본은 항상 등록 — 위 영어 주석대로, 등록을 생략하면 원격이 전체 물리 메모리를 읽고 쓸 수 있는 키를 쓰게 된다 */
module_param(register_always, bool, 0444);	/* [한국어] 성능을 위해 끌 수 있지만 보안 대가가 위 주석에 있다 */
MODULE_PARM_DESC(register_always,	/* [한국어] 연속 메모리에도 MR 등록을 강제한다는 뜻 */
	 "Use memory registration even for contiguous memory regions");

static int nvme_rdma_cm_handler(struct rdma_cm_id *cm_id,	/* [한국어] CM 이벤트 콜백 — 주소 해석·경로 해석·연결 수립이 모두 여기로 온다 */
		struct rdma_cm_event *event);
static void nvme_rdma_recv_done(struct ib_cq *cq, struct ib_wc *wc);	/* [한국어] 수신 완료 — 응답 캡슐이 도착했을 때 */
static void nvme_rdma_complete_rq(struct request *rq);	/* [한국어] blk-mq 완료 콜백. 아래에서 정의된다 */

static const struct blk_mq_ops nvme_rdma_mq_ops;	/* [한국어] 아래 정의를 앞당겨 참조하기 위한 전방 선언 */
static const struct blk_mq_ops nvme_rdma_admin_mq_ops;

static inline int nvme_rdma_queue_idx(struct nvme_rdma_queue *queue)	/* [한국어] 이 큐가 배열에서 몇 번째인가 — 0 이면 admin, 1 이상이면 I/O */
{
	/* [한국어] 큐들은 하나의 연속 배열이므로 포인터 차이가 곧 인덱스다.
	 * 별도 필드를 두지 않은 것은 인덱스가 배열 위치와 늘 일치하기 때문이다. */
	return queue - queue->ctrl->queues;
}

/*
 * [한국어]
 * nvme_rdma_poll_queue - 이 큐가 폴링 전용 큐인가
 *
 * @queue: 판정할 큐
 * @return: 폴링 큐면 true
 *
 * 큐 배열은 용도별로 구간이 나뉘어 있다. 앞쪽부터 기본(DEFAULT), 읽기 전용
 * (READ), 그리고 마지막이 폴링(POLL) 구간이다. 그래서 "기본 개수 + 읽기 개수"
 * 보다 뒤에 있으면 폴링 큐라는 판정이 성립한다. 별도 플래그가 없는 이유이기도 하다.
 *
 * 왜 구분이 필요한가: 폴링 큐는 CQ 를 IB_POLL_DIRECT 로 만들어 인터럽트를 달지
 * 않는다. 완료는 blk-mq 의 poll 경로가 직접 훑어 가져간다. 그래서 큐 생성과
 * 완료 수확 양쪽에서 이 판정이 갈림길이 된다.
 *
 * 호출 체인:
 *   nvme_rdma_create_queue_ib / nvme_rdma_post_send → [이 함수]
 */
static bool nvme_rdma_poll_queue(struct nvme_rdma_queue *queue)
{
	/* [한국어] 인덱스가 기본 구간과 읽기 구간의 합보다 크면 폴링 구간이다. */
	return nvme_rdma_queue_idx(queue) >	/* [한국어] 이 큐의 배열 인덱스 */
		queue->ctrl->io_queues[HCTX_TYPE_DEFAULT] +	/* [한국어] 기본 큐 개수 — 구간의 첫 경계 */
		queue->ctrl->io_queues[HCTX_TYPE_READ];	/* [한국어] 읽기 전용 큐 개수 — 두 번째 경계. 그 뒤가 폴링 구간 */
}

static inline size_t nvme_rdma_inline_data_size(struct nvme_rdma_queue *queue)	/* [한국어] 이 큐에서 명령 캡슐에 함께 실을 수 있는 데이터 바이트 수 */
{
	/* [한국어] 캡슐 전체 길이에서 SQE 가 차지하는 64바이트를 뺀 나머지가
	 * 인라인 데이터에 쓸 수 있는 공간이다. admin 큐는 캡슐이 SQE 크기와 같아
	 * 이 값이 0 이 되고, 그래서 admin 은 인라인을 쓰지 않는다. */
	return queue->cmnd_capsule_len - sizeof(struct nvme_command);
}

/*
 * [한국어]
 * nvme_rdma_free_qe - 큐 항목 하나의 버퍼와 DMA 매핑을 되돌린다
 *
 * @ibdev:        이 매핑을 만든 IB 장치
 * @qe:           해제할 항목
 * @capsule_size: 매핑할 때 쓴 크기. 언매핑에도 같은 값이 필요하다
 * @dir:          매핑 방향. 이것도 매핑 때와 같아야 한다
 * @return: 없음
 *
 * 순서가 중요하다. 언매핑이 먼저이고 kfree 가 나중이다 -- 반대로 하면
 * HCA 가 아직 접근할 수 있는 메모리를 반납하는 셈이 된다.
 *
 * 실행 컨텍스트: 큐 해체 경로. 잠들 수 있다.
 *
 * 호출 체인: nvme_rdma_free_ring / 큐 해체 → [이 함수] → ib_dma_unmap_single
 */
static void nvme_rdma_free_qe(struct ib_device *ibdev, struct nvme_rdma_qe *qe,
		size_t capsule_size, enum dma_data_direction dir)
{
	ib_dma_unmap_single(ibdev, qe->dma, capsule_size, dir);	/* [한국어] 언매핑이 먼저 — 반대로 하면 HCA 가 접근할 수 있는 메모리를 반납하게 된다 */
	kfree(qe->data);	/* [한국어] 그다음에야 버퍼를 놓는다 */
}

/*
 * [한국어]
 * nvme_rdma_alloc_qe - 큐 항목 하나의 버퍼를 잡고 HCA 가 볼 수 있게 매핑한다
 *
 * @ibdev:        매핑할 IB 장치
 * @qe:           채울 항목
 * @capsule_size: 이 항목이 담을 캡슐 크기(명령 또는 완료)
 * @dir:          어느 방향으로 쓸지. 송신이면 TO_DEVICE, 수신이면 FROM_DEVICE
 * @return: 0 이면 성공, -ENOMEM 이면 할당 또는 매핑 실패
 *
 * RDMA 에서는 커널 가상 주소를 HCA 에게 줄 수 없다. 버퍼를 잡은 뒤 반드시
 * DMA 주소로 바꿔야 하고, 그 주소를 work request 에 싣는다.
 *
 * 매핑 실패 시 버퍼를 해제하고 포인터를 NULL 로 지우는 것이 중요하다.
 * 링 할당 실패 경로가 "data 가 NULL 이 아니면 해제"로 판단하기 때문에,
 * 지우지 않으면 이중 해제가 된다.
 *
 * 실행 컨텍스트: 큐 생성 경로. GFP_KERNEL 이라 잠들 수 있다.
 *
 * 호출 체인: nvme_rdma_alloc_ring / 큐 초기화 → [이 함수] → ib_dma_map_single
 */
static int nvme_rdma_alloc_qe(struct ib_device *ibdev, struct nvme_rdma_qe *qe,
		size_t capsule_size, enum dma_data_direction dir)
{
	qe->data = kzalloc(capsule_size, GFP_KERNEL);	/* [한국어] 캡슐 하나를 담을 버퍼 */
	if (!qe->data)
		return -ENOMEM;

	qe->dma = ib_dma_map_single(ibdev, qe->data, capsule_size, dir);	/* [한국어] HCA 는 커널 가상 주소를 모른다 — DMA 주소로 바꿔야 work request 에 실을 수 있다 */
	if (ib_dma_mapping_error(ibdev, qe->dma)) {	/* [한국어] IOMMU 자원이 부족하거나 주소가 장치 범위를 넘었다 */
		kfree(qe->data);	/* [한국어] 매핑이 없으므로 버퍼만 되돌린다 */
		qe->data = NULL;	/* [한국어] 링 정리 경로가 "NULL 이 아니면 해제"로 판단하므로 반드시 지운다 */
		return -ENOMEM;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}

	return 0;
}

/*
 * [한국어]
 * nvme_rdma_free_ring - 큐 항목 링 전체를 되돌린다
 *
 * @ibdev:        매핑을 만든 IB 장치
 * @ring:         해제할 링
 * @ib_queue_size: 실제로 초기화된 항목 수
 * @capsule_size: 항목 하나의 크기
 * @dir:          매핑 방향
 * @return: 없음
 *
 * 인자로 개수를 받는 것이 요점이다. 할당 중간에 실패한 경우 링 전체가
 * 아니라 "여기까지 성공한" 개수만 되돌려야 하고, 그 값을 호출자가 안다.
 *
 * 실행 컨텍스트: 큐 해체 또는 할당 실패 경로. 잠들 수 있다.
 *
 * 호출 체인: 큐 해체 / nvme_rdma_alloc_ring(실패) → [이 함수]
 */
static void nvme_rdma_free_ring(struct ib_device *ibdev,
		struct nvme_rdma_qe *ring, size_t ib_queue_size,
		size_t capsule_size, enum dma_data_direction dir)
{
	int i;	/* [한국어] 되돌릴 개수만큼만 도는 인덱스 */

	for (i = 0; i < ib_queue_size; i++)	/* [한국어] 호출자가 넘긴 개수까지만 — 부분 실패를 정확히 되감기 위한 인자다 */
		nvme_rdma_free_qe(ibdev, &ring[i], capsule_size, dir);
	kfree(ring);	/* [한국어] 항목을 모두 푼 뒤 배열 자체를 놓는다 */
}

/*
 * [한국어]
 * nvme_rdma_alloc_ring - 큐 항목 링을 통째로 잡는다
 *
 * @ibdev:        매핑할 IB 장치
 * @ib_queue_size: 만들 항목 수
 * @capsule_size: 항목 하나의 크기
 * @dir:          매핑 방향
 * @return: 링 포인터. 실패하면 NULL.
 *
 * RDMA 는 수신 버퍼를 미리 걸어 두어야 하므로, 큐를 만들 때 받을 수 있는
 * 최대 개수만큼의 버퍼를 한꺼번에 준비한다. 송신 쪽도 같은 구조로
 * 미리 잡아 두어 제출 경로에서 할당이 일어나지 않게 한다.
 *
 * 실패 시 i 를 그대로 넘겨 되돌리는 것에 주의할 것. 아래 영어 주석이
 * 밝히듯, 부분 실패를 그 자리에서 정확히 되감아야 오류 복구가 큐를
 * 다시 만들 때 이전 잔재와 부딪히지 않는다.
 *
 * 실행 컨텍스트: 큐 생성 경로. 잠들 수 있다.
 *
 * 호출 체인: nvme_rdma_alloc_queue → [이 함수] → nvme_rdma_alloc_qe
 */
static struct nvme_rdma_qe *nvme_rdma_alloc_ring(struct ib_device *ibdev,
		size_t ib_queue_size, size_t capsule_size,
		enum dma_data_direction dir)
{
	struct nvme_rdma_qe *ring;	/* [한국어] 항목 배열 */
	int i;	/* [한국어] 실패 시 여기까지의 개수를 그대로 되돌리기에 쓴다 */

	ring = kzalloc_objs(struct nvme_rdma_qe, ib_queue_size);	/* [한국어] 항목 서술자 배열 — 버퍼는 아래에서 하나씩 붙인다 */
	if (!ring)
		return NULL;

	/*
	 * Bind the CQEs (post recv buffers) DMA mapping to the RDMA queue
	 * lifetime. It's safe, since any change in the underlying RDMA device
	 * will issue error recovery and queue re-creation.
	 */
	for (i = 0; i < ib_queue_size; i++) {	/* [한국어] RDMA 는 수신 버퍼를 미리 걸어 두어야 해서 최대 개수만큼 한꺼번에 준비한다 */
		if (nvme_rdma_alloc_qe(ibdev, &ring[i], capsule_size, dir))	/* [한국어] 하나라도 실패하면 여기까지를 정확히 되감는다 */
			goto out_free_ring;
	}

	return ring;	/* [한국어] 제출 경로에서 할당이 일어나지 않도록 여기서 다 잡아 둔다 */

out_free_ring:
	nvme_rdma_free_ring(ibdev, ring, i, capsule_size, dir);	/* [한국어] i 가 성공한 개수 — 전체가 아니라 그만큼만 되돌린다 */
	return NULL;
}

/*
 * [한국어]
 * nvme_rdma_qp_event - QP 에서 올라온 비동기 이벤트를 기록만 한다
 *
 * @event: ib_verbs 가 전달한 QP 이벤트
 * @context: QP 생성 시 넘긴 컨텍스트(이 드라이버는 큐 포인터를 넣는다)
 * @return: 없음
 *
 * QP 수준의 치명적 오류는 별도로 처리하지 않고 디버그 로그만 남긴다. 실제
 * 복구는 이 경로가 아니라, 완료 큐에 실린 오류 상태(IB_WC_*)와 CM 의
 * DISCONNECTED 이벤트가 촉발하는 nvme_rdma_error_recovery 가 담당하기 때문이다.
 * 여기서 겹쳐 복구를 걸면 같은 큐를 두 곳에서 내리게 된다.
 *
 * 실행 컨텍스트: ib_verbs 코어의 이벤트 처리 문맥. 잠들면 안 된다.
 */
static void nvme_rdma_qp_event(struct ib_event *event, void *context)
{
	/* [한국어] 이벤트 이름과 번호만 남긴다 — 복구 판단은 다른 경로의 몫이다. */
	pr_debug("QP event %s (%d)\n",
		 ib_event_msg(event->event), event->event);

}

/*
 * [한국어]
 * nvme_rdma_wait_for_cm - CM 단계 하나가 끝나기를 기다리고 그 결과를 돌려준다
 *
 * @queue: 대상 큐
 * @return: 0 이면 그 단계 성공. 음수면 CM 이 남긴 오류이거나 대기 중 시그널.
 *
 * 왜 필요한가: 주소 해석 → 라우팅 해석 → 연결 수립은 각각 비동기 콜백으로
 * 끝난다. 콜백은 값을 돌려줄 수 없으므로 결과를 queue->cm_error 에 남기고
 * cm_done 을 완료시킨다. 이 함수가 그 둘을 하나의 동기 호출처럼 묶어 준다.
 *
 * interruptible 로 기다리는 이유: 연결이 영영 오지 않을 수 있어(타겟이
 * 응답하지 않는 경우) 사용자가 'nvme connect' 를 끊을 수 있어야 한다.
 *
 * 호출 체인:
 *   nvme_rdma_alloc_queue → rdma_resolve_addr → [이 함수] → (콜백이 깨움)
 */
static int nvme_rdma_wait_for_cm(struct nvme_rdma_queue *queue)
{
	int ret;	/* [한국어] 대기 자체의 결과 — CM 결과와는 별개다 */

	ret = wait_for_completion_interruptible(&queue->cm_done);	/* [한국어] 콜백이 complete() 할 때까지 잠든다. 시그널을 받으면 음수로 깨어난다 */
	if (ret)	/* [한국어] 시그널로 깨어난 경우 — CM 결과를 보기 전에 그대로 돌아간다 */
		return ret;
	WARN_ON_ONCE(queue->cm_error > 0);	/* [한국어] cm_error 는 0 또는 음수 errno 여야 한다. 양수면 콜백이 규약을 어긴 것 */
	return queue->cm_error;	/* [한국어] 콜백이 남긴 그 단계의 실제 결과 */
}

/*
 * [한국어]
 * nvme_rdma_create_qp - 이 큐의 Queue Pair 를 만든다
 *
 * @queue:  QP 를 붙일 큐. cm_id 와 ib_cq 가 이미 준비돼 있어야 한다.
 * @factor: 요청 하나가 송신 큐에서 소비할 수 있는 WR 개수의 배수.
 *          MR 등록과 무효화까지 세면 요청당 여러 WR 이 나가므로,
 *          호출자가 그만큼 곱해서 넘긴다.
 * @return: 0 이면 성공. 음수면 rdma_create_qp 실패.
 *
 * QP 는 송신 큐와 수신 큐의 쌍이며, 이 드라이버의 모든 WR 이 여기에 게시된다.
 * 크기를 잘못 잡으면 요청이 몰릴 때 ib_post_send 가 ENOMEM 으로 실패하므로,
 * 아래 cap 계산이 이 함수의 실질적인 내용이다.
 *
 * 호출 체인:
 *   nvme_rdma_create_queue_ib → [이 함수] → rdma_create_qp
 */
static int nvme_rdma_create_qp(struct nvme_rdma_queue *queue, const int factor)
{
	struct nvme_rdma_device *dev = queue->device;	/* [한국어] PD 와 인라인 세그먼트 한도를 가진 장치 객체 */
	struct ib_qp_init_attr init_attr;	/* [한국어] QP 생성 파라미터 — 아래에서 하나씩 채운다 */
	int ret;

	memset(&init_attr, 0, sizeof(init_attr));	/* [한국어] 지정하지 않는 필드가 쓰레기 값이 되지 않도록 먼저 0 으로 채운다 */
	init_attr.event_handler = nvme_rdma_qp_event;	/* [한국어] QP 비동기 이벤트는 로그만 남긴다 — 복구는 CQ/CM 경로가 한다 */
	/* +1 for drain */
	init_attr.cap.max_send_wr = factor * queue->queue_size + 1;	/* [한국어] 요청당 factor 개 WR 에 더해 1 — 위 영어 주석대로 해체 시 drain WR 자리 */
	/* +1 for drain */
	init_attr.cap.max_recv_wr = queue->queue_size + 1;	/* [한국어] 응답 수신은 요청당 하나. 여기도 drain 용 한 칸을 더한다 */
	init_attr.cap.max_recv_sge = 1;	/* [한국어] 응답 캡슐은 연속된 버퍼 하나라 SGE 가 하나면 충분하다 */
	init_attr.cap.max_send_sge = 1 + dev->num_inline_segments;	/* [한국어] 명령 캡슐 한 칸 + 인라인 데이터 조각들. 장치가 허용하는 만큼만 잡는다 */
	init_attr.sq_sig_type = IB_SIGNAL_REQ_WR;	/* [한국어] 모든 송신에 완료를 올리지 않고, WR 이 요청한 것만 올린다 — CQ 부담을 줄인다 */
	init_attr.qp_type = IB_QPT_RC;	/* [한국어] Reliable Connection — 순서와 도달을 하드웨어가 보장한다. NVMe 의 명령/응답 짝맞춤이 이를 전제한다 */
	init_attr.send_cq = queue->ib_cq;	/* [한국어] 송신과 수신이 같은 CQ 를 공유한다 — 큐당 폴링 지점을 하나로 유지하기 위해서다 */
	init_attr.recv_cq = queue->ib_cq;
	if (queue->pi_support)	/* [한국어] 보호 정보 오프로드를 쓰는 큐라면 */
		init_attr.create_flags |= IB_QP_CREATE_INTEGRITY_EN;	/* [한국어] signature MR 을 쓸 수 있도록 무결성 기능을 켜고 QP 를 만든다 */
	init_attr.qp_context = queue;	/* [한국어] 이벤트 콜백이 큐를 되찾을 수 있게 컨텍스트로 심어 둔다 */

	ret = rdma_create_qp(queue->cm_id, dev->pd, &init_attr);	/* [한국어] CM ID 에 QP 를 붙인다 — PD 는 장치 단위로 공유하는 것을 쓴다 */

	queue->qp = queue->cm_id->qp;	/* [한국어] 성공했으면 CM 이 채워 준 QP 를 큐에 캐시한다. 실패면 NULL 이 들어가고 아래 ret 로 걸러진다 */
	return ret;
}

/*
 * [한국어]
 * nvme_rdma_exit_request - 요청 하나가 들고 있던 SQE 버퍼를 반납한다
 *
 * @set:      이 요청이 속한 태그셋
 * @rq:       해제할 요청
 * @hctx_idx: 하드웨어 큐 번호(쓰지 않는다)
 * @return: 없음
 *
 * init_request 의 짝이다. 매핑을 풀지 않는 것에 주의할 것 -- 이 SQE 는
 * 제출할 때마다 매핑하고 완료할 때 푸는 방식이라, 여기 도달한 시점에는
 * 이미 매핑이 없다. 남은 것은 버퍼뿐이다.
 *
 * 실행 컨텍스트: 태그셋 해제. 잠들 수 있다.
 *
 * 호출 체인: blk_mq_free_tag_set → ops->exit_request → [이 함수]
 */
static void nvme_rdma_exit_request(struct blk_mq_tag_set *set,
		struct request *rq, unsigned int hctx_idx)
{
	struct nvme_rdma_request *req = blk_mq_rq_to_pdu(rq);	/* [한국어] 요청 뒤에 붙은 드라이버 전용 영역 */

	kfree(req->sqe.data);	/* [한국어] 매핑은 제출마다 걸고 완료에서 풀므로 여기 남은 것은 버퍼뿐이다 */
}

/*
 * [한국어]
 * nvme_rdma_init_request - 요청 하나의 고정 상태를 한 번만 세운다
 *
 * @set:       이 요청이 속한 태그셋(admin 인지 I/O 인지 구별에 쓴다)
 * @rq:        초기화할 요청
 * @hctx_idx:  하드웨어 큐 번호
 * @numa_node: 할당된 NUMA 노드(쓰지 않는다)
 * @return: 0 이면 성공, -ENOMEM 이면 SQE 버퍼 할당 실패
 *
 * 태그셋을 만들 때 요청마다 한 번씩 불린다. 여기서 채우는 것은 요청의
 * 수명 내내 변하지 않는 것들이고, I/O 마다 달라지는 값은 queue_rq 가
 * 채운다.
 *
 * 큐 번호 계산이 이 함수의 요령이다. admin 태그셋이면 0번 큐, I/O
 * 태그셋이면 hctx_idx + 1 -- I/O 큐는 admin 다음부터 번호가 붙는다.
 * 태그셋 포인터를 비교해 어느 쪽인지 가른다.
 *
 * SQE 버퍼를 따로 잡는 이유는 RDMA 가 명령을 캡슐로 보내기 때문이다.
 * PCIe 처럼 큐 메모리에 써 넣는 것이 아니라, 매 제출마다 이 버퍼를
 * 매핑해 work request 에 싣는다.
 *
 * PI 를 쓰는 큐에서는 메타데이터용 SGL 이 데이터 SGL 뒤에 이어 붙는다.
 * 그 오프셋을 여기서 계산해 두어 제출 경로가 다시 세지 않게 한다.
 *
 * 실행 컨텍스트: 태그셋 초기화. 잠들 수 있다.
 *
 * 호출 체인: blk_mq_alloc_tag_set → ops->init_request → [이 함수]
 */
static int nvme_rdma_init_request(struct blk_mq_tag_set *set,
		struct request *rq, unsigned int hctx_idx,
		unsigned int numa_node)
{
	struct nvme_rdma_ctrl *ctrl = to_rdma_ctrl(set->driver_data);	/* [한국어] 태그셋에 새겨 둔 컨트롤러 */
	struct nvme_rdma_request *req = blk_mq_rq_to_pdu(rq);	/* [한국어] 채울 드라이버 전용 영역 */
	int queue_idx = (set == &ctrl->tag_set) ? hctx_idx + 1 : 0;	/* [한국어] 태그셋 포인터로 admin 인지 I/O 인지 가른다 — I/O 는 0번 admin 다음부터다 */
	struct nvme_rdma_queue *queue = &ctrl->queues[queue_idx];	/* [한국어] 이 요청이 늘 갈 큐 */

	nvme_req(rq)->ctrl = &ctrl->ctrl;	/* [한국어] 코어가 오류 정책을 판단할 때 쓴다 */
	req->sqe.data = kzalloc_obj(struct nvme_command);	/* [한국어] RDMA 는 명령을 캡슐로 보낸다 — 큐 메모리에 쓰는 것이 아니라 매 제출마다 이 버퍼를 매핑한다 */
	if (!req->sqe.data)
		return -ENOMEM;

	/* metadata nvme_rdma_sgl struct is located after command's data SGL */
	if (queue->pi_support)	/* [한국어] 위 영어 주석대로 메타데이터 SGL 은 데이터 SGL 뒤에 이어 붙는다 */
		req->metadata_sgl = (void *)nvme_req(rq) +
			sizeof(struct nvme_rdma_request) +
			NVME_RDMA_DATA_SGL_SIZE;

	req->queue = queue;	/* [한국어] 완료 경로가 이 포인터로 큐를 되찾는다 */
	nvme_req(rq)->cmd = req->sqe.data;	/* [한국어] 코어가 조립할 SQE 의 자리를 방금 잡은 버퍼로 고정한다 */

	return 0;
}

/*
 * [한국어]
 * nvme_rdma_init_hctx - blk-mq I/O 하드웨어 큐를 이 드라이버의 큐에 잇는다
 *
 * @hctx:     blk-mq 가 만든 하드웨어 큐 문맥
 * @data:     태그셋의 driver_data — 컨트롤러다
 * @hctx_idx: 몇 번째 I/O 하드웨어 큐인가
 * @return: 항상 0
 *
 * +1 이 admin 큐 자리를 건너뛴다. 큐 배열은 0번이 admin 이고 I/O 는
 * 1번부터이므로, blk-mq 의 0-based 인덱스를 그대로 쓰면 admin 큐로
 * I/O 를 보내게 된다.
 *
 * BUG_ON 은 그 계산이 배열 밖을 가리키지 않는지 확인한다. 여기서 넘치면
 * 이후 제출 경로가 엉뚱한 메모리를 큐로 다루므로, 조용히 진행시키는
 * 것보다 즉시 멈추는 편이 낫다.
 *
 * 실행 컨텍스트: 태그셋 초기화. 잠들 수 있다.
 *
 * 호출 체인: blk_mq_alloc_tag_set → ops->init_hctx → [이 함수]
 */
static int nvme_rdma_init_hctx(struct blk_mq_hw_ctx *hctx, void *data,
		unsigned int hctx_idx)
{
	struct nvme_rdma_ctrl *ctrl = to_rdma_ctrl(data);	/* [한국어] 태그셋의 driver_data */
	struct nvme_rdma_queue *queue = &ctrl->queues[hctx_idx + 1];	/* [한국어] +1 이 admin 큐 자리를 건너뛴다 — 안 하면 I/O 가 admin 큐로 간다 */

	BUG_ON(hctx_idx >= ctrl->ctrl.queue_count);	/* [한국어] 배열 밖을 가리키면 이후 제출이 엉뚱한 메모리를 큐로 다룬다 — 조용히 진행시키지 않는다 */

	hctx->driver_data = queue;	/* [한국어] queue_rq 가 이 포인터로 큐를 찾는다 */
	return 0;
}

/*
 * [한국어]
 * nvme_rdma_init_admin_hctx - admin 하드웨어 큐를 0번 큐에 잇는다
 *
 * @hctx:     blk-mq 가 만든 하드웨어 큐 문맥
 * @data:     태그셋의 driver_data — 컨트롤러다
 * @hctx_idx: 항상 0이어야 한다
 * @return: 항상 0
 *
 * admin 태그셋은 하드웨어 큐가 하나뿐이므로 인덱스 계산이 필요 없다.
 * I/O 쪽과 콜백을 나눈 것도 그 때문이다 -- 조건 분기 대신 태그셋마다
 * 맞는 함수를 걸어 둔다.
 *
 * 실행 컨텍스트: 태그셋 초기화. 잠들 수 있다.
 *
 * 호출 체인: blk_mq_alloc_tag_set → ops->init_hctx → [이 함수]
 */
static int nvme_rdma_init_admin_hctx(struct blk_mq_hw_ctx *hctx, void *data,
		unsigned int hctx_idx)
{
	struct nvme_rdma_ctrl *ctrl = to_rdma_ctrl(data);
	struct nvme_rdma_queue *queue = &ctrl->queues[0];	/* [한국어] admin 은 늘 0번이라 계산이 필요 없다 */

	BUG_ON(hctx_idx != 0);	/* [한국어] admin 태그셋은 하드웨어 큐가 하나뿐이다 */

	hctx->driver_data = queue;
	return 0;
}

/*
 * [한국어]
 * nvme_rdma_free_dev - 마지막 사용자가 사라진 장치 객체를 해제한다
 *
 * @ref: 0 이 된 kref. 바깥 nvme_rdma_device 로 되돌린다.
 * @return: 없음
 *
 * kref_put 이 계수를 0 으로 내렸을 때만 불린다. 여러 컨트롤러가 같은 HCA 를
 * 공유하므로, PD 는 마지막 하나가 떠날 때 풀어야 한다.
 *
 * 순서가 중요하다: 목록에서 먼저 빼고 나서 PD 를 푼다. 반대로 하면 목록을
 * 훑던 nvme_rdma_find_get_device 가 이미 해제된 PD 를 가진 객체를 집어 갈 수 있다.
 *
 * 호출 체인:
 *   nvme_rdma_dev_put → kref_put → [이 함수] → ib_dealloc_pd
 */
static void nvme_rdma_free_dev(struct kref *ref)
{
	struct nvme_rdma_device *ndev =	/* [한국어] kref 필드 주소에서 바깥 장치 객체를 되찾는다 */
		container_of(ref, struct nvme_rdma_device, ref);

	mutex_lock(&device_list_mutex);	/* [한국어] 목록 조작 보호 — 동시에 같은 HCA 를 찾는 쪽과 경합한다 */
	list_del(&ndev->entry);	/* [한국어] 먼저 목록에서 빼야 이후 탐색이 이 객체를 집지 않는다 */
	mutex_unlock(&device_list_mutex);

	ib_dealloc_pd(ndev->pd);	/* [한국어] 이 장치에서 만든 MR·QP 가 모두 사라진 뒤에야 PD 를 푼다 */
	kfree(ndev);	/* [한국어] 장치 객체 자체를 반환 */
}

static void nvme_rdma_dev_put(struct nvme_rdma_device *dev)	/* [한국어] 장치 참조를 하나 놓는다 — 0 이 되면 free_dev 가 불린다 */
{
	kref_put(&dev->ref, nvme_rdma_free_dev);	/* [한국어] 감소와 0 판정이 원자적으로 일어난다 */
}

static int nvme_rdma_dev_get(struct nvme_rdma_device *dev)	/* [한국어] 장치 참조를 하나 얻는다. 이미 해제 중이면 실패 */
{
	/* [한국어] _unless_zero 인 것이 핵심이다. 목록을 훑는 도중 다른 쪽이
	 * 계수를 0 으로 내려 해제를 시작했을 수 있는데, 그 객체를 되살려 쓰면
	 * 곧 해제될 PD 를 잡게 된다. 0 이면 실패시켜 탐색을 계속하게 한다. */
	return kref_get_unless_zero(&dev->ref);
}

/*
 * [한국어]
 * nvme_rdma_find_get_device - 이 CM ID 가 붙은 HCA 의 공유 장치 객체를 얻는다
 *
 * @cm_id: 연결이 해석된 CM 식별자. 여기에 실제 ib_device 포인터가 실려 있다.
 * @return: 참조가 하나 올라간 장치 객체. 실패하면 NULL.
 *
 * 왜 공유하나: PD 는 HCA 단위 자원이라 컨트롤러마다 새로 만들 이유가 없다.
 * 같은 HCA 를 쓰는 큐와 컨트롤러가 하나의 PD 를 나눠 쓰고, 참조 계수로 수명을 맞춘다.
 * 동일성 판정은 node_guid 로 한다 -- 포인터 비교로는 같은 장치의 다른 표현을
 * 놓칠 수 있기 때문이다.
 *
 * 여기서 장치 능력 두 가지가 결정된다:
 *   - MEM_MGT_EXTENSIONS 가 없으면 fast registration 을 쓸 수 없어 아예 거절한다.
 *   - num_inline_segments 는 인라인으로 실을 수 있는 조각 수의 상한이 되고,
 *     이후 작은 I/O 가 MR 등록 없이 나갈 수 있는지를 가른다.
 *
 * register_always 가 꺼져 있으면 PD 를 UNSAFE_GLOBAL_RKEY 로 만든다. 위 모듈
 * 파라미터 주석이 밝히듯 작은 I/O 는 빨라지지만 물리 메모리 전체를 원격에
 * 노출하는 rkey 가 생기므로 근본적으로 안전하지 않다.
 *
 * 실행 컨텍스트: 큐 생성 경로. device_list_mutex 를 잡으므로 잠들 수 있다.
 *
 * 호출 체인:
 *   nvme_rdma_create_queue_ib → [이 함수] → ib_alloc_pd
 */
static struct nvme_rdma_device *
nvme_rdma_find_get_device(struct rdma_cm_id *cm_id)
{
	struct nvme_rdma_device *ndev;

	mutex_lock(&device_list_mutex);	/* [한국어] 탐색과 등록이 하나의 임계구역이어야 같은 HCA 객체가 둘 생기지 않는다 */
	list_for_each_entry(ndev, &device_list, entry) {	/* [한국어] 이미 만들어 둔 장치가 있는지 훑는다 */
		if (ndev->dev->node_guid == cm_id->device->node_guid &&	/* [한국어] GUID 가 같으면 같은 HCA 다 */
		    nvme_rdma_dev_get(ndev))	/* [한국어] 해제 중이 아닐 때만 채택 — 실패하면 계속 훑는다 */
			goto out_unlock;	/* [한국어] 기존 객체 재사용 */
	}

	ndev = kzalloc_obj(*ndev);	/* [한국어] 처음 보는 HCA — 새 객체를 만든다 */
	if (!ndev)
		goto out_err;

	ndev->dev = cm_id->device;	/* [한국어] 실제 HCA 를 기억한다 */
	kref_init(&ndev->ref);	/* [한국어] 참조 1 로 시작 — 이 호출자가 그 하나를 들고 나간다 */

	ndev->pd = ib_alloc_pd(ndev->dev,	/* [한국어] 이 HCA 의 보호 영역을 만든다 */
		register_always ? 0 : IB_PD_UNSAFE_GLOBAL_RKEY);	/* [한국어] 등록을 생략하는 모드면 전역 rkey 를 요구한다 — 위 모듈 파라미터 주석의 위험이 여기서 실현된다 */
	if (IS_ERR(ndev->pd))	/* [한국어] RDMA API 는 오류를 포인터에 실어 준다 */
		goto out_free_dev;

	if (!(ndev->dev->attrs.device_cap_flags &	/* [한국어] fast registration(FRWR) 지원 여부 확인 */
	      IB_DEVICE_MEM_MGT_EXTENSIONS)) {
		dev_err(&ndev->dev->dev,	/* [한국어] 이 기능이 없으면 요청마다 MR 을 등록할 수 없어 이 드라이버가 성립하지 않는다 */
			"Memory registrations not supported.\n");
		goto out_free_pd;
	}

	ndev->num_inline_segments = min(NVME_RDMA_MAX_INLINE_SEGMENTS,	/* [한국어] 드라이버 상한과 */
					ndev->dev->attrs.max_send_sge - 1);	/* [한국어] 장치가 허용하는 송신 SGE 수 중 작은 쪽. -1 은 명령 캡슐이 첫 칸을 쓰기 때문 */
	list_add(&ndev->entry, &device_list);	/* [한국어] 다음 큐가 재사용할 수 있도록 목록에 등록 */
out_unlock:
	mutex_unlock(&device_list_mutex);
	return ndev;

out_free_pd:	/* [한국어] PD 는 만들었으나 능력 검사에서 탈락한 경우 */
	ib_dealloc_pd(ndev->pd);
out_free_dev:	/* [한국어] 객체는 잡았으나 PD 생성에 실패한 경우 */
	kfree(ndev);
out_err:	/* [한국어] 객체 할당부터 실패한 경우 — 풀 것이 없다 */
	mutex_unlock(&device_list_mutex);
	return NULL;
}

/*
 * [한국어]
 * nvme_rdma_free_cq - 완료 큐를 해제한다. 만든 방식에 맞춰 푸는 방식도 갈린다
 *
 * @queue: 대상 큐
 * @return: 없음
 *
 * 폴링 큐의 CQ 는 ib_alloc_cq 로 이 큐만을 위해 만들었으므로 ib_free_cq 로 푼다.
 * 그 밖의 큐는 ib_cq_pool_get 으로 공유 풀에서 빌려 온 것이라 반납해야 한다.
 * 만든 경로와 푸는 경로가 짝이 맞지 않으면 풀의 회계가 어긋난다.
 */
static void nvme_rdma_free_cq(struct nvme_rdma_queue *queue)
{
	if (nvme_rdma_poll_queue(queue))	/* [한국어] 폴링 큐는 전용 CQ 를 따로 만들었다 */
		ib_free_cq(queue->ib_cq);	/* [한국어] 전용이므로 그대로 해제 */
	else
		ib_cq_pool_put(queue->ib_cq, queue->cq_size);	/* [한국어] 공유 풀에서 빌린 것이라 빌린 크기와 함께 반납한다 */
}

/*
 * [한국어]
 * nvme_rdma_destroy_queue_ib - 큐의 RDMA 자원(QP, CQ, MR 풀, 수신 링)을 되돌린다
 *
 * @queue: 해체할 큐
 * @return: 없음
 *
 * 진입 자체를 test_and_clear_bit 으로 가른다. 오류 복구와 정상 해체가 동시에
 * 같은 큐를 내리려 할 수 있는데, QP 파괴는 두 번 하면 안 되므로 비트를 먼저
 * 끄는 쪽 하나만 통과시킨다. 이 한 줄이 이 함수의 동시성 보호 전부다.
 *
 * 해제 순서에도 이유가 있다. MR 풀을 먼저 비워야 QP 를 파괴할 때 아직 등록된
 * 영역이 남아 있지 않고, 수신 링은 QP 가 사라진 뒤에 풀어야 하드웨어가 더 이상
 * 그 버퍼에 쓰지 않는다. 장치 참조는 그 모든 것을 쓰고 난 마지막에 놓는다.
 *
 * 호출 체인:
 *   nvme_rdma_free_queue / 오류 경로 → [이 함수] → ib_destroy_qp
 */
static void nvme_rdma_destroy_queue_ib(struct nvme_rdma_queue *queue)
{
	struct nvme_rdma_device *dev;
	struct ib_device *ibdev;

	if (!test_and_clear_bit(NVME_RDMA_Q_TR_READY, &queue->flags))	/* [한국어] 비트를 끄는 데 성공한 쪽만 해체를 수행한다 — 중복 파괴 방지 */
		return;

	dev = queue->device;	/* [한국어] 마지막에 참조를 놓기 위해 미리 붙잡아 둔다 */
	ibdev = dev->dev;

	if (queue->pi_support)	/* [한국어] 보호 정보용 signature MR 풀이 따로 있는 큐라면 */
		ib_mr_pool_destroy(queue->qp, &queue->qp->sig_mrs);	/* [한국어] 그 풀부터 비운다 */
	ib_mr_pool_destroy(queue->qp, &queue->qp->rdma_mrs);	/* [한국어] 일반 MR 풀도 비운다 — QP 파괴 전에 등록된 영역이 남으면 안 된다 */

	/*
	 * The cm_id object might have been destroyed during RDMA connection
	 * establishment error flow to avoid getting other cma events, thus
	 * the destruction of the QP shouldn't use rdma_cm API.
	 */
	/* [한국어] 위 영어 주석대로, 연결 수립 실패 경로에서 추가 CM 이벤트를 막으려고
	 * cm_id 를 먼저 없앴을 수 있다. 그래서 rdma_destroy_qp 가 아니라 ib_destroy_qp 를
	 * 직접 부른다 — 전자는 cm_id 를 경유하므로 그 경우 쓸 수 없다. */
	ib_destroy_qp(queue->qp);
	nvme_rdma_free_cq(queue);	/* [한국어] QP 가 사라진 뒤에야 CQ 를 푼다 — 남은 완료가 올라올 곳이 없어야 한다 */

	nvme_rdma_free_ring(ibdev, queue->rsp_ring, queue->queue_size,	/* [한국어] 미리 게시해 둔 응답 수신 버퍼들을 매핑 해제하고 반환 */
			sizeof(struct nvme_completion), DMA_FROM_DEVICE);

	nvme_rdma_dev_put(dev);	/* [한국어] 이 큐가 들고 있던 장치 참조를 놓는다. 마지막이면 PD 까지 풀린다 */
}

/*
 * [한국어]
 * nvme_rdma_get_max_fr_pages - MR 하나가 등록할 수 있는 페이지 수의 상한
 *
 * @ibdev:      능력을 물어볼 HCA
 * @pi_support: 보호 정보(T10-PI) 오프로드를 쓰는 큐인가
 * @return:     한 MR 이 담을 수 있는 최대 페이지 수
 *
 * 이 값이 왜 중요한가: 한 요청은 원칙적으로 MR 하나로 표현돼야 한다. 그래서
 * 이 상한이 곧 한 요청이 옮길 수 있는 최대 크기가 되고, 호출자는 여기에
 * 페이지 크기를 곱해 queue_limits 의 max_hw_sectors 를 정한다. 블록 계층이
 * bio 를 어디서 자를지가 결국 이 하드웨어 값에서 나온다.
 *
 * signature MR 은 데이터와 보호 정보를 함께 기술해야 해서 일반 MR 과 한도가
 * 다르다. 그래서 pi_support 에 따라 다른 능력 필드를 본다.
 *
 * -1 을 하는 이유는 호출자 쪽 주석이 설명한다 -- 첫 항목이 정렬되지 않으면
 * 한 페이지가 두 칸을 쓰게 되어 한 칸의 여유가 필요하다.
 *
 * 호출 체인:
 *   nvme_rdma_create_queue_ib / nvme_rdma_setup_ctrl → [이 함수]
 */
static int nvme_rdma_get_max_fr_pages(struct ib_device *ibdev, bool pi_support)
{
	u32 max_page_list_len;	/* [한국어] 장치가 광고한 fast-registration 페이지 목록 길이 */

	if (pi_support)	/* [한국어] 보호 정보를 함께 등록하는 signature MR 이라면 */
		max_page_list_len = ibdev->attrs.max_pi_fast_reg_page_list_len;	/* [한국어] PI 전용 한도를 쓴다 — 데이터와 PI 를 함께 담아야 해 일반보다 작다 */
	else
		max_page_list_len = ibdev->attrs.max_fast_reg_page_list_len;	/* [한국어] 일반 MR 한도 */

	return min_t(u32, NVME_RDMA_MAX_SEGMENTS, max_page_list_len - 1);	/* [한국어] 드라이버 상한과 장치 한도 중 작은 쪽. -1 은 비정렬 첫 조각을 위한 여유 */
}

/*
 * [한국어]
 * nvme_rdma_create_cq - 이 큐의 완료 큐를 만든다
 *
 * @ibdev: 대상 HCA
 * @queue: CQ 를 붙일 큐. cq_size 가 이미 정해져 있어야 한다.
 * @return: 0 이면 성공, 음수면 실패
 *
 * 두 가지가 여기서 갈린다.
 *
 * 첫째, 완료 벡터 선택. 위 영어 주석대로 I/O 큐는 자기 인덱스에 따라 벡터를
 * 나눠 갖는다. 그래야 여러 큐의 완료 인터럽트가 서로 다른 CPU 로 흩어져
 * 한 코어에 몰리지 않는다. admin 큐(인덱스 0)는 트래픽이 적어 늘 0번을 쓰고,
 * I/O 큐는 idx-1 로 0부터 다시 세어 벡터를 고르게 분배한다.
 *
 * 둘째, 폴링 여부. 폴링 큐는 인터럽트를 달지 않고 blk-mq 의 poll 경로가 직접
 * CQ 를 훑어야 하므로 IB_POLL_DIRECT 로 전용 CQ 를 만든다. 나머지는 공유 CQ
 * 풀에서 빌려 소프트IRQ 문맥에서 완료를 처리한다. 해제 경로(nvme_rdma_free_cq)가
 * 이 선택을 그대로 되짚어야 풀의 회계가 맞는다.
 *
 * 호출 체인:
 *   nvme_rdma_create_queue_ib → [이 함수] → ib_alloc_cq / ib_cq_pool_get
 */
static int nvme_rdma_create_cq(struct ib_device *ibdev,
		struct nvme_rdma_queue *queue)
{
	int ret, comp_vector, idx = nvme_rdma_queue_idx(queue);	/* [한국어] 벡터 분배의 기준이 되는 큐 인덱스 */

	/*
	 * Spread I/O queues completion vectors according their queue index.
	 * Admin queues can always go on completion vector 0.
	 */
	comp_vector = (idx == 0 ? idx : idx - 1) % ibdev->num_comp_vectors;	/* [한국어] admin 은 0번 고정, I/O 는 0부터 다시 세어 장치가 가진 벡터 수로 감싼다 */

	/* Polling queues need direct cq polling context */
	if (nvme_rdma_poll_queue(queue))	/* [한국어] 폴링 큐는 인터럽트가 없어야 한다 */
		queue->ib_cq = ib_alloc_cq(ibdev, queue, queue->cq_size,	/* [한국어] 전용 CQ 를 직접 만든다 */
					   comp_vector, IB_POLL_DIRECT);	/* [한국어] DIRECT — 호출자가 부를 때만 폴링. blk-mq poll 이 그 호출자다 */
	else
		queue->ib_cq = ib_cq_pool_get(ibdev, queue->cq_size,	/* [한국어] 공유 풀에서 빌린다 — 큐가 많아도 CQ 자원을 아낀다 */
					      comp_vector, IB_POLL_SOFTIRQ);	/* [한국어] SOFTIRQ — 인터럽트가 뜨면 소프트IRQ 문맥에서 완료를 수확한다 */

	if (IS_ERR(queue->ib_cq)) {	/* [한국어] RDMA API 는 오류를 포인터에 실어 준다 */
		ret = PTR_ERR(queue->ib_cq);
		return ret;
	}

	return 0;
}

/*
 * [한국어]
 * nvme_rdma_create_queue_ib - 큐의 RDMA 자원 일체를 세운다
 *
 * @queue: cm_id 가 이미 해석된 큐
 * @return: 0 이면 성공. 음수면 실패이며 그때까지 잡은 것은 모두 되돌린다.
 *
 * 세우는 순서가 곧 의존 순서다:
 *   장치(PD) → CQ → QP → 응답 수신 링 → MR 풀 → (PI 면 signature MR 풀)
 * 마지막에 TR_READY 비트를 세워야 해체 경로가 "정리할 자원이 있다"고 인식한다.
 * 그 전에 실패하면 아래 goto 사다리가 역순으로 되돌린다.
 *
 * 크기 계산이 이 함수의 핵심이다. 요청 하나가 송신 큐에서 소비하는 WR 은 셋이다
 * -- MR 등록, SEND, 그리고 무효화(INV). 그래서 send_wr_factor 가 3 이다. CQ 는
 * 여기에 수신 완료까지 올라오므로 하나를 더해 4배로 잡고, drain 용 한 칸을 더한다.
 * 이 계산이 모자라면 부하가 몰릴 때 ib_post_send 가 ENOMEM 으로 실패한다.
 *
 * 실행 컨텍스트: 연결 수립 경로. 잠들 수 있다.
 *
 * 호출 체인:
 *   nvme_rdma_cm_handler(ADDR_RESOLVED) → [이 함수] → ib_mr_pool_init
 */
static int nvme_rdma_create_queue_ib(struct nvme_rdma_queue *queue)
{
	struct ib_device *ibdev;
	const int send_wr_factor = 3;			/* MR, SEND, INV */
	/* [한국어] 위 영어 주석대로 요청 하나가 송신 큐에서 쓰는 WR 은 등록·전송·무효화 셋이다 */
	const int cq_factor = send_wr_factor + 1;	/* + RECV */
	/* [한국어] CQ 에는 수신 완료도 올라오므로 하나를 더한다 */
	int ret, pages_per_mr;

	queue->device = nvme_rdma_find_get_device(queue->cm_id);	/* [한국어] 이 HCA 의 공유 장치 객체를 얻는다(참조 +1) */
	if (!queue->device) {
		dev_err(queue->cm_id->device->dev.parent,	/* [한국어] 장치 능력이 모자라거나 할당에 실패한 경우 */
			"no client data found!\n");
		return -ECONNREFUSED;	/* [한국어] 연결 자체를 거절한다 — 이 HCA 로는 NVMe/RDMA 를 할 수 없다 */
	}
	ibdev = queue->device->dev;

	/* +1 for ib_drain_qp */
	queue->cq_size = cq_factor * queue->queue_size + 1;	/* [한국어] 요청당 4개 완료 + 해체 시 drain 한 칸. 이 값은 해제 때 반납 크기로도 쓰인다 */

	ret = nvme_rdma_create_cq(ibdev, queue);	/* [한국어] 완료 큐 먼저 — QP 가 생성 시 CQ 를 요구한다 */
	if (ret)
		goto out_put_dev;

	ret = nvme_rdma_create_qp(queue, send_wr_factor);	/* [한국어] 송수신 큐 쌍. 위에서 만든 CQ 를 양쪽에 붙인다 */
	if (ret)
		goto out_destroy_ib_cq;

	queue->rsp_ring = nvme_rdma_alloc_ring(ibdev, queue->queue_size,	/* [한국어] 응답 수신 버퍼를 큐 깊이만큼 미리 잡는다 */
			sizeof(struct nvme_completion), DMA_FROM_DEVICE);	/* [한국어] 방향은 장치→호스트. 응답 캡슐 하나가 nvme_completion 크기다 */
	if (!queue->rsp_ring) {
		ret = -ENOMEM;
		goto out_destroy_qp;
	}

	/*
	 * Currently we don't use SG_GAPS MR's so if the first entry is
	 * misaligned we'll end up using two entries for a single data page,
	 * so one additional entry is required.
	 */
	pages_per_mr = nvme_rdma_get_max_fr_pages(ibdev, queue->pi_support) + 1;	/* [한국어] 위 영어 주석대로 비정렬 첫 조각이 두 칸을 쓸 수 있어 한 칸을 더한다 */
	ret = ib_mr_pool_init(queue->qp, &queue->qp->rdma_mrs,	/* [한국어] MR 을 미리 만들어 풀에 채워 둔다 — 요청마다 만들면 감당이 안 된다 */
			      queue->queue_size,	/* [한국어] 동시 처리 가능한 요청 수만큼 있으면 충분하다 */
			      IB_MR_TYPE_MEM_REG,	/* [한국어] 일반 메모리 등록용 */
			      pages_per_mr, 0);
	if (ret) {
		dev_err(queue->ctrl->ctrl.device,
			"failed to initialize MR pool sized %d for QID %d\n",
			queue->queue_size, nvme_rdma_queue_idx(queue));
		goto out_destroy_ring;
	}

	if (queue->pi_support) {	/* [한국어] 보호 정보 오프로드를 쓰는 큐라면 별도 풀이 하나 더 필요하다 */
		ret = ib_mr_pool_init(queue->qp, &queue->qp->sig_mrs,	/* [한국어] signature MR — 등록과 동시에 HCA 가 PI 를 생성·검증한다 */
				      queue->queue_size, IB_MR_TYPE_INTEGRITY,
				      pages_per_mr, pages_per_mr);	/* [한국어] 데이터와 메타데이터 양쪽에 같은 페이지 수를 잡는다 */
		if (ret) {
			dev_err(queue->ctrl->ctrl.device,
				"failed to initialize PI MR pool sized %d for QID %d\n",
				queue->queue_size, nvme_rdma_queue_idx(queue));
			goto out_destroy_mr_pool;
		}
	}

	set_bit(NVME_RDMA_Q_TR_READY, &queue->flags);	/* [한국어] 여기서부터 해체 경로가 이 자원들을 정리할 책임을 진다 */

	return 0;

out_destroy_mr_pool:	/* [한국어] 아래부터는 역순 되감기 — 잡은 순서의 반대로 푼다 */
	ib_mr_pool_destroy(queue->qp, &queue->qp->rdma_mrs);
out_destroy_ring:
	nvme_rdma_free_ring(ibdev, queue->rsp_ring, queue->queue_size,
			    sizeof(struct nvme_completion), DMA_FROM_DEVICE);
out_destroy_qp:
	rdma_destroy_qp(queue->cm_id);	/* [한국어] 여기서는 cm_id 가 살아 있으므로 rdma_ 계열을 쓸 수 있다 — 해체 경로와 다른 점이다 */
out_destroy_ib_cq:
	nvme_rdma_free_cq(queue);
out_put_dev:
	nvme_rdma_dev_put(queue->device);	/* [한국어] 맨 처음 얻은 장치 참조를 놓는다 */
	return ret;
}

/*
 * [한국어]
 * nvme_rdma_alloc_queue - 큐 하나의 RDMA 연결을 주소 해석까지 세운다
 *
 * @ctrl:       대상 컨트롤러
 * @idx:        큐 번호. 0이면 admin.
 * @queue_size: 이 큐의 깊이
 * @return: 0 이면 성공, 음수 errno
 *
 * RDMA 연결은 세 단계다. 주소 해석(로컬 장치 찾기) → 경로 해석 → 연결.
 * 이 함수는 첫 단계를 걸고 CM 콜백이 나머지를 이어 가며, 마지막에
 * wait_for_cm 이 전 과정의 결과를 기다린다. 그래서 rdma_resolve_addr
 * 한 줄이 성공해도 아직 연결된 것이 아니다.
 *
 * 캡슐 길이가 큐 종류마다 다른 것이 이 함수의 결정적 분기다. admin 은
 * SQE 크기 그대로지만, I/O 큐는 컨트롤러가 알린 ioccsz(16바이트 단위)를
 * 쓴다. 그 길이가 데이터를 명령과 함께 인라인으로 보낼 수 있는지를
 * 정하므로, 제출 경로의 매핑 선택이 여기서 갈린다.
 *
 * cm_error 를 -ETIMEDOUT 으로 미리 채워 두는 것에 주의할 것. CM 콜백이
 * 한 번도 불리지 않고 타임아웃이 나는 경우, 그 값이 그대로 결과가 된다.
 *
 * 실행 컨텍스트: 연결/재연결 경로. 잠들 수 있다.
 *
 * 호출 체인:
 *   nvme_rdma_configure_admin_queue / _io_queues → [이 함수]
 *     → rdma_resolve_addr → nvme_rdma_cm_handler → nvme_rdma_wait_for_cm
 */
static int nvme_rdma_alloc_queue(struct nvme_rdma_ctrl *ctrl,
		int idx, size_t queue_size)
{
	struct nvme_rdma_queue *queue;	/* [한국어] 채울 큐. 배열에서 꺼내 쓴다 */
	struct sockaddr *src_addr = NULL;	/* [한국어] 로컬 주소를 고정하지 않으면 NULL — 커널이 라우팅으로 고른다 */
	int ret;

	queue = &ctrl->queues[idx];	/* [한국어] 큐 배열은 컨트롤러 할당 때 통째로 잡혀 있다 */
	mutex_init(&queue->queue_lock);	/* [한국어] stop/start 가 겹치는 것을 막는 큐별 락 */
	queue->ctrl = ctrl;	/* [한국어] CM 콜백이 이 포인터로 컨트롤러를 되찾는다 */
	if (idx && ctrl->ctrl.max_integrity_segments)	/* [한국어] admin 큐에는 T10-PI 를 쓸 일이 없다 */
		queue->pi_support = true;
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		queue->pi_support = false;
	init_completion(&queue->cm_done);	/* [한국어] CM 콜백이 이것으로 결과를 알린다 */

	if (idx > 0)	/* [한국어] I/O 큐는 컨트롤러가 알린 캡슐 길이를 쓴다 */
		queue->cmnd_capsule_len = ctrl->ctrl.ioccsz * 16;
	else
		queue->cmnd_capsule_len = sizeof(struct nvme_command);	/* [한국어] admin 은 SQE 크기 그대로 — 인라인 데이터를 쓰지 않는다 */

	queue->queue_size = queue_size;	/* [한국어] 수신 링 크기와 Connect 에 실을 값이 여기서 정해진다 */

	queue->cm_id = rdma_create_id(&init_net, nvme_rdma_cm_handler, queue,	/* [한국어] CM ID 를 만든다. RDMA_PS_TCP + RC 는 신뢰성 있는 연결형 QP 를 뜻한다 */
			RDMA_PS_TCP, IB_QPT_RC);
	if (IS_ERR(queue->cm_id)) {	/* [한국어] CM 자원 부족이거나 모듈이 준비되지 않았다 */
		dev_info(ctrl->ctrl.device,	/* [한국어] 사용자에게 어느 단계에서 막혔는지 알린다 */
			"failed to create CM ID: %ld\n", PTR_ERR(queue->cm_id));
		ret = PTR_ERR(queue->cm_id);
		goto out_destroy_mutex;	/* [한국어] 아직 CM ID 가 없으므로 락만 되돌린다 */
	}

	if (ctrl->ctrl.opts->mask & NVMF_OPT_HOST_TRADDR)	/* [한국어] 사용자가 출구 주소를 고정했다 */
		src_addr = (struct sockaddr *)&ctrl->src_addr;	/* [한국어] 다중 경로 구성에서 어느 HCA 로 나갈지 정한다 */

	queue->cm_error = -ETIMEDOUT;	/* [한국어] 콜백이 한 번도 안 불리면 이 값이 그대로 결과가 된다 */
	ret = rdma_resolve_addr(queue->cm_id, src_addr,	/* [한국어] 비동기다 — 성공 반환은 "요청을 걸었다"는 뜻뿐이다 */
			(struct sockaddr *)&ctrl->addr,	/* [한국어] 사용자가 준 대상 주소 */
			NVME_RDMA_CM_TIMEOUT_MS);
	if (ret) {	/* [한국어] 요청 자체를 걸지 못했다 */
		dev_info(ctrl->ctrl.device,
			"rdma_resolve_addr failed (%d).\n", ret);
		goto out_destroy_cm_id;
	}

	ret = nvme_rdma_wait_for_cm(queue);	/* [한국어] 주소 해석 → 경로 해석 → 연결까지 전 과정의 결과를 여기서 기다린다 */
	if (ret) {	/* [한국어] 어느 단계든 실패하면 cm_error 로 전달된다 */
		dev_info(ctrl->ctrl.device,
			"rdma connection establishment failed (%d)\n", ret);
		goto out_destroy_cm_id;
	}

	set_bit(NVME_RDMA_Q_ALLOCATED, &queue->flags);	/* [한국어] 이 비트가 서야 해체 경로가 이 큐를 정리 대상으로 본다 */

	return 0;	/* [한국어] RDMA 연결까지다. NVMe Connect 는 start_queue 가 보낸다 */

out_destroy_cm_id:
	rdma_destroy_id(queue->cm_id);
	nvme_rdma_destroy_queue_ib(queue);	/* [한국어] CM 콜백이 이미 QP 를 만들었을 수 있다 */
out_destroy_mutex:
	mutex_destroy(&queue->queue_lock);
	return ret;
}

/*
 * [한국어]
 * __nvme_rdma_stop_queue - 큐 하나를 실제로 끊고 진행 중 작업을 배출시킨다
 *
 * @queue: 대상 큐
 * @return: 없음
 *
 * 두 줄이지만 순서가 전부다. 먼저 끊어야 새 작업이 들어오지 않고,
 * 그다음 drain 이 이미 하드웨어에 걸린 work request 가 모두 완료
 * 통지를 낼 때까지 기다린다. drain 없이 QP 를 없애면 완료 콜백이
 * 이미 해제된 자료구조를 건드린다.
 *
 * ib_drain_qp 는 자체적으로 더미 work request 를 걸어 그것이 완료되는
 * 것으로 앞선 것들이 모두 끝났음을 확인한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 오래 잠들 수 있다.
 *
 * 호출 체인: nvme_rdma_stop_queue / _start_queue(실패) → [이 함수]
 */
static void __nvme_rdma_stop_queue(struct nvme_rdma_queue *queue)
{
	rdma_disconnect(queue->cm_id);	/* [한국어] 먼저 끊어야 새 작업이 들어오지 않는다 */
	ib_drain_qp(queue->qp);	/* [한국어] 걸린 work request 가 모두 완료 통지를 낼 때까지 — 없으면 완료가 해제된 메모리로 온다 */
}

/*
 * [한국어]
 * nvme_rdma_stop_queue - 살아 있는 큐만 한 번씩 끊는다
 *
 * @queue: 대상 큐
 * @return: 없음
 *
 * __nvme_rdma_stop_queue 를 두 겹으로 감싼다. 바깥은 큐가 아예 할당되지
 * 않았는지 보고, 안쪽은 LIVE 비트를 test_and_clear 로 다뤄 두 경로가
 * 동시에 들어와도 실제 종료는 한 번만 일어나게 한다.
 *
 * 오류 복구와 삭제가 겹치는 것이 흔한 상황이라 이 중복 방지가 필요하다.
 * ib_drain_qp 를 두 번 부르면 이미 없어진 QP 를 건드린다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 잠들 수 있다.
 *
 * 호출 체인: 큐 해체 / 오류 복구 → [이 함수] → __nvme_rdma_stop_queue
 */
static void nvme_rdma_stop_queue(struct nvme_rdma_queue *queue)
{
	if (!test_bit(NVME_RDMA_Q_ALLOCATED, &queue->flags))	/* [한국어] 아예 만들어지지 않은 큐다 */
		return;

	mutex_lock(&queue->queue_lock);	/* [한국어] 오류 복구와 삭제가 동시에 들어올 수 있다 */
	if (test_and_clear_bit(NVME_RDMA_Q_LIVE, &queue->flags))	/* [한국어] 실제 종료는 한 번만 — drain 을 두 번 부르면 없어진 QP 를 건드린다 */
		__nvme_rdma_stop_queue(queue);
	mutex_unlock(&queue->queue_lock);
}

/*
 * [한국어]
 * nvme_rdma_free_queue - 큐가 잡은 CM ID 와 IB 자원을 반납한다
 *
 * @queue: 대상 큐
 * @return: 없음
 *
 * ALLOCATED 비트를 test_and_clear 로 다뤄 두 번 해제되지 않게 한다.
 * stop 과 마찬가지로 오류 복구와 삭제가 겹칠 수 있기 때문이다.
 *
 * 호출자가 먼저 stop 을 부른 뒤에만 여기 와야 한다. 진행 중 work
 * request 가 남은 채로 QP 를 없애면 완료가 해제된 메모리로 온다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 잠들 수 있다.
 *
 * 호출 체인: 큐 해체 경로 → [이 함수] → rdma_destroy_id
 */
static void nvme_rdma_free_queue(struct nvme_rdma_queue *queue)
{
	if (!test_and_clear_bit(NVME_RDMA_Q_ALLOCATED, &queue->flags))	/* [한국어] 같은 이유로 해제도 한 번만 */
		return;

	rdma_destroy_id(queue->cm_id);	/* [한국어] CM ID 를 없앤다 */
	nvme_rdma_destroy_queue_ib(queue);	/* [한국어] QP·CQ·MR 풀을 반납한다 */
	mutex_destroy(&queue->queue_lock);	/* [한국어] 마지막에 락 — 위 두 단계가 이 락 규약 아래 끝난 뒤여야 한다 */
}

/*
 * [한국어]
 * nvme_rdma_free_io_queues - I/O 큐를 모두 반납한다
 *
 * @ctrl: 대상 컨트롤러
 * @return: 없음
 *
 * 1부터 도는 이유는 0번이 admin 이기 때문이다. admin 큐는 수명이 달라
 * 별도 경로가 다룬다.
 *
 * 실행 컨텍스트: 세션 해체. 잠들 수 있다.
 *
 * 호출 체인: nvme_rdma_teardown_io_queues → [이 함수] → nvme_rdma_free_queue
 */
static void nvme_rdma_free_io_queues(struct nvme_rdma_ctrl *ctrl)
{
	int i;	/* [한국어] 큐 인덱스 */

	for (i = 1; i < ctrl->ctrl.queue_count; i++)	/* [한국어] 0번은 admin 이라 건너뛴다 — 수명이 다르다 */
		nvme_rdma_free_queue(&ctrl->queues[i]);
}

/*
 * [한국어]
 * nvme_rdma_stop_io_queues - I/O 큐를 모두 끊는다
 *
 * @ctrl: 대상 컨트롤러
 * @return: 없음
 *
 * free 의 짝이며 반드시 먼저 온다. 여기서 각 큐의 진행 중 작업이
 * 배출되고, 그 뒤에야 자원을 반납할 수 있다. 역시 admin 을 건너뛴다.
 *
 * 실행 컨텍스트: 세션 해체. 오래 잠들 수 있다.
 *
 * 호출 체인: nvme_rdma_teardown_io_queues → [이 함수] → nvme_rdma_stop_queue
 */
static void nvme_rdma_stop_io_queues(struct nvme_rdma_ctrl *ctrl)
{
	int i;

	for (i = 1; i < ctrl->ctrl.queue_count; i++)	/* [한국어] 마찬가지로 I/O 큐만 */
		nvme_rdma_stop_queue(&ctrl->queues[i]);
}

/*
 * [한국어]
 * nvme_rdma_start_queue - 큐에 Fabrics Connect 를 보내 실제로 쓸 수 있게 한다
 *
 * @ctrl: 대상 컨트롤러
 * @idx:  큐 번호. 0이면 admin.
 * @return: 0 이면 성공, 음수 errno 또는 NVMe 상태
 *
 * alloc_queue 가 RDMA 연결까지 세웠다면 이 함수가 NVMe 수준의 연결을
 * 맺는다. 그 둘은 별개다 -- QP 가 붙었어도 Connect 를 보내기 전에는
 * 컨트롤러가 이 큐를 인정하지 않는다.
 *
 * 실패 시 곧바로 큐를 끊는 것이 요점이다. Connect 가 실패한 큐를
 * 열어 두면 오류 복구가 그것을 살아 있는 큐로 오인한다.
 *
 * 실행 컨텍스트: 연결/재연결 경로. 잠들 수 있다.
 *
 * 호출 체인:
 *   nvme_rdma_start_io_queues / _configure_admin_queue → [이 함수]
 *     → nvmf_connect_io_queue / nvmf_connect_admin_queue
 */
static int nvme_rdma_start_queue(struct nvme_rdma_ctrl *ctrl, int idx)
{
	struct nvme_rdma_queue *queue = &ctrl->queues[idx];	/* [한국어] LIVE 비트를 세울 대상 */
	int ret;

	if (idx)	/* [한국어] admin 과 I/O 는 Connect 명령이 다르다 */
		ret = nvmf_connect_io_queue(&ctrl->ctrl, idx);
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ret = nvmf_connect_admin_queue(&ctrl->ctrl);

	if (!ret) {	/* [한국어] Connect 성공 — 이제 이 큐로 명령을 보낼 수 있다 */
		set_bit(NVME_RDMA_Q_LIVE, &queue->flags);	/* [한국어] 제출 경로가 이 비트를 보고 큐를 쓴다 */
	} else {
		if (test_bit(NVME_RDMA_Q_ALLOCATED, &queue->flags))	/* [한국어] 실패한 큐를 열어 두면 복구가 살아 있는 큐로 오인한다 */
			__nvme_rdma_stop_queue(queue);
		dev_info(ctrl->ctrl.device,	/* [한국어] 어느 큐가 왜 실패했는지 남긴다 */
			"failed to connect queue: %d ret=%d\n", idx, ret);
	}
	return ret;
}

/*
 * [한국어]
 * nvme_rdma_start_io_queues - 지정한 구간의 I/O 큐에 Connect 를 보낸다
 *
 * @ctrl:  대상 컨트롤러
 * @first: 시작 큐 번호(포함)
 * @last:  끝 큐 번호(제외)
 * @return: 0 이면 전부 성공, 아니면 실패한 큐의 오류
 *
 * 구간을 인자로 받는 이유는 큐 수를 늘리는 경우가 있기 때문이다.
 * 재연결에서 컨트롤러가 더 많은 큐를 허락하면 새로 늘어난 구간만
 * Connect 하면 된다.
 *
 * 실패 시 i-- 부터 거꾸로 되돌리는 것에 주의할 것. 실패한 큐 자신은
 * start_queue 가 이미 끊었으므로 그 앞의 것들만 되감는다.
 *
 * 실행 컨텍스트: 연결/재연결 경로. 잠들 수 있다.
 *
 * 호출 체인: nvme_rdma_configure_io_queues → [이 함수] → nvme_rdma_start_queue
 */
static int nvme_rdma_start_io_queues(struct nvme_rdma_ctrl *ctrl,
				     int first, int last)
{
	int i, ret = 0;	/* [한국어] 실패 시 되돌릴 위치를 i 가 기억한다 */

	for (i = first; i < last; i++) {	/* [한국어] 구간을 받는 이유: 재연결에서 큐가 늘면 새 구간만 Connect 한다 */
		ret = nvme_rdma_start_queue(ctrl, i);
		if (ret)
			goto out_stop_queues;
	}

	return 0;	/* [한국어] 구간 전체가 살아났다 */

out_stop_queues:
	for (i--; i >= first; i--)	/* [한국어] 실패한 큐 자신은 start_queue 가 이미 끊었으므로 그 앞만 되감는다 */
		nvme_rdma_stop_queue(&ctrl->queues[i]);
	return ret;
}

/*
 * [한국어]
 * nvme_rdma_alloc_io_queues - I/O 큐 개수를 협상하고 각 큐를 만든다
 *
 * @ctrl: 대상 컨트롤러
 * @return: 0 이면 모두 생성. 음수면 그 지점까지 되감았다.
 *
 * 개수는 두 단계로 정해진다. 먼저 옵션에 적힌 기본·읽기·폴링 큐 수를 합쳐
 * 요청할 값을 만들고(nvmf_nr_io_queues), 그것을 Set Features 로 컨트롤러와
 * 협상한다. 타겟이 더 적게 줄 수 있으므로 최종 개수는 그 응답이 정한다.
 *
 * 하나도 못 얻으면 -ENOMEM 으로 실패시킨다. RDMA 는 admin 만으로 쓸 이유가
 * 거의 없고, 디스크를 붙였는데 I/O 를 보낼 수 없는 상태가 되기 때문이다.
 *
 * nvmf_set_io_queues 가 협상된 개수를 세 용도로 다시 배분한다. 그 결과가
 * ctrl->io_queues[] 이고, poll_queue 판정과 map_queues 가 그 값을 쓴다.
 *
 * 큐 생성은 1 부터 시작한다 -- 0 번 admin 은 이미 세워져 있다. 실패하면
 * 그때까지 만든 것을 역순으로 되감는다.
 *
 * 실행 컨텍스트: 연결 경로. CM 이벤트를 기다리며 잠든다.
 *
 * 호출 체인:
 *   nvme_rdma_configure_io_queues → [이 함수] → nvme_rdma_alloc_queue
 */
static int nvme_rdma_alloc_io_queues(struct nvme_rdma_ctrl *ctrl)
{
	struct nvmf_ctrl_options *opts = ctrl->ctrl.opts;
	unsigned int nr_io_queues;
	int i, ret;

	nr_io_queues = nvmf_nr_io_queues(opts);	/* [한국어] 기본·읽기·폴링 큐 수의 합 — 사용자가 요청한 값이다 */
	ret = nvme_set_queue_count(&ctrl->ctrl, &nr_io_queues);	/* [한국어] Set Features 로 협상. 타겟이 더 적게 줄 수 있다 */
	if (ret)
		return ret;

	if (nr_io_queues == 0) {	/* [한국어] 하나도 못 얻었다 */
		dev_err(ctrl->ctrl.device,	/* [한국어] 디스크는 붙는데 I/O 를 보낼 수 없는 상태가 되므로 실패로 본다 */
			"unable to set any I/O queues\n");
		return -ENOMEM;
	}

	ctrl->ctrl.queue_count = nr_io_queues + 1;	/* [한국어] admin 하나를 더한 것이 전체 큐 수 */
	dev_info(ctrl->ctrl.device,
		"creating %d I/O queues.\n", nr_io_queues);

	nvmf_set_io_queues(opts, nr_io_queues, ctrl->io_queues);	/* [한국어] 협상된 개수를 세 용도로 다시 배분한다 — poll_queue 판정과 map_queues 가 이 값을 쓴다 */
	for (i = 1; i < ctrl->ctrl.queue_count; i++) {	/* [한국어] 1 부터 — 0 번 admin 은 이미 있다 */
		ret = nvme_rdma_alloc_queue(ctrl, i,
				ctrl->ctrl.sqsize + 1);	/* [한국어] CM 3단계를 거쳐 QP 까지 세운다 */
		if (ret)
			goto out_free_queues;
	}

	return 0;

out_free_queues:
	for (i--; i >= 1; i--)	/* [한국어] 실패한 것 직전부터 역순으로 되감는다 */
		nvme_rdma_free_queue(&ctrl->queues[i]);

	return ret;
}

/*
 * [한국어]
 * nvme_rdma_alloc_tag_set - I/O 태그셋을 만든다. 요청 하나의 크기를 여기서 정한다
 *
 * @ctrl: 대상 컨트롤러
 * @return: 0 이면 성공, 음수 errno
 *
 * cmd_size 계산이 이 함수의 전부다. blk-mq 는 요청마다 이 크기의 영역을
 * 붙여 주므로, 요청 구조체와 그 뒤에 오는 SGL 들이 모두 들어가야 한다.
 * 데이터 SGL 은 항상이고, PI 를 쓰는 구성에서는 메타데이터 SGL 이
 * 하나 더 붙는다.
 *
 * 이 크기를 여기서 한 번 정해 두면 제출 경로에서 SGL 을 따로 할당할
 * 일이 없다 -- RDMA I/O 경로에 할당이 없는 이유다.
 *
 * 실행 컨텍스트: 연결 경로. 잠들 수 있다.
 *
 * 호출 체인: nvme_rdma_configure_io_queues → [이 함수] → nvme_alloc_io_tag_set
 */
static int nvme_rdma_alloc_tag_set(struct nvme_ctrl *ctrl)
{
	unsigned int cmd_size = sizeof(struct nvme_rdma_request) +	/* [한국어] 요청 구조체 뒤에 데이터 SGL 이 이어 붙는다 */
				NVME_RDMA_DATA_SGL_SIZE;

	if (ctrl->max_integrity_segments)	/* [한국어] PI 를 쓰면 메타데이터 SGL 이 하나 더 */
		cmd_size += sizeof(struct nvme_rdma_sgl) +
			    NVME_RDMA_METADATA_SGL_SIZE;

	return nvme_alloc_io_tag_set(ctrl, &to_rdma_ctrl(ctrl)->tag_set,	/* [한국어] 이 크기를 미리 정해 두어 제출 경로에 할당이 없다 */
			&nvme_rdma_mq_ops,
			ctrl->opts->nr_poll_queues ? HCTX_MAX_TYPES : 2,
			cmd_size);
}

/*
 * [한국어]
 * nvme_rdma_destroy_admin_queue - admin 큐와 AEN 전용 SQE 를 되돌린다
 *
 * @ctrl: 대상 컨트롤러
 * @return: 없음
 *
 * 순서가 안전성이다. async_event_work 를 먼저 취소해야 한다 -- 그 워크가
 * 하는 일이 이 SQE 로 AEN 을 다시 거는 것이므로, 버퍼를 푼 뒤에 돌면
 * 해제된 메모리를 매핑해 보낸다. _sync 라 실행 중인 것이 끝날 때까지
 * 기다린다.
 *
 * 포인터를 NULL 로 지우는 것도 필수다. 재연결에서 다시 잡을 때 이 값이
 * 남아 있으면 이 블록을 또 지나 이중 해제가 된다.
 *
 * 실행 컨텍스트: 세션 해체. 잠들 수 있다.
 *
 * 호출 체인: nvme_rdma_teardown_admin_queue → [이 함수] → nvme_rdma_free_queue
 */
static void nvme_rdma_destroy_admin_queue(struct nvme_rdma_ctrl *ctrl)
{
	if (ctrl->async_event_sqe.data) {	/* [한국어] AEN 용 SQE 를 잡아 둔 구성에서만 */
		cancel_work_sync(&ctrl->ctrl.async_event_work);	/* [한국어] 먼저 취소해야 한다 — 이 워크가 하는 일이 아래에서 풀 버퍼로 AEN 을 거는 것이다 */
		nvme_rdma_free_qe(ctrl->device->dev, &ctrl->async_event_sqe,	/* [한국어] 매핑을 풀고 버퍼를 놓는다 */
				sizeof(struct nvme_command), DMA_TO_DEVICE);	/* [한국어] 매핑할 때와 같은 크기·방향이어야 한다 */
		ctrl->async_event_sqe.data = NULL;	/* [한국어] 재연결에서 이 블록을 또 지나 이중 해제가 되지 않도록 */
	}
	nvme_rdma_free_queue(&ctrl->queues[0]);	/* [한국어] admin 큐 자체를 반납한다 */
}

/*
 * [한국어]
 * nvme_rdma_configure_admin_queue - admin 큐를 세우고 컨트롤러 능력을 읽는다
 *
 * @ctrl: 대상 컨트롤러
 * @new:  처음 연결인가. 재연결이면 태그셋을 다시 만들지 않는다.
 * @return: 0 이면 성공, 음수 errno
 *
 * 세션 수립의 첫 단계이며, 여기가 끝나야 컨트롤러가 무엇을 할 수 있는지
 * 알게 된다. 순서가 곧 의존 관계다: RDMA 연결 → 태그셋 → NVMe Connect →
 * CC.EN → Identify.
 *
 * new 인자로 재연결을 구분하는 이유는 태그셋의 수명이 세션보다 길기
 * 때문이다. 재연결에서 태그셋을 새로 만들면 이미 큐에 있던 요청이
 * 갈 곳을 잃는다.
 *
 * 한계값을 enable 뒤에 정하는 것에 주의할 것. max_fr_pages 는 HCA 가
 * 한 번에 등록할 수 있는 페이지 수이고, 그것이 곧 이 트랜스포트의
 * 최대 전송 크기다. PI 지원 여부도 여기서 코어에 알린다 -- 이 값이
 * 0 이면 코어가 메타데이터 있는 네임스페이스를 다르게 다룬다.
 *
 * 실패 라벨이 다섯 단계인 것은 그만큼 되돌릴 것이 층층이 쌓이기
 * 때문이다. 각 라벨은 그 지점까지 성공한 것만 정확히 되감는다.
 *
 * 실행 컨텍스트: 연결/재연결 워크. 오래 잠든다.
 *
 * 호출 체인:
 *   nvme_rdma_setup_ctrl → [이 함수]
 *     → nvme_rdma_alloc_queue → nvme_rdma_start_queue → nvme_enable_ctrl
 */
static int nvme_rdma_configure_admin_queue(struct nvme_rdma_ctrl *ctrl,
		bool new)
{
	bool pi_capable = false;	/* [한국어] HCA 가 T10-PI 오프로드를 지원하는지. 아래에서 코어에 전달된다 */
	int error;

	error = nvme_rdma_alloc_queue(ctrl, 0, NVME_AQ_DEPTH);	/* [한국어] admin 큐는 깊이가 스펙 고정값이다 */
	if (error)
		return error;	/* [한국어] 아직 아무것도 잡지 않았다 */

	ctrl->device = ctrl->queues[0].device;	/* [한국어] CM 이 고른 IB 장치를 컨트롤러 수준으로 올린다 */
	ctrl->ctrl.numa_node = ibdev_to_node(ctrl->device->dev);	/* [한국어] HCA 가 붙은 노드에서 메모리를 잡아야 DMA 가 원격 노드를 넘지 않는다 */

	/* T10-PI support */
	if (ctrl->device->dev->attrs.kernel_cap_flags &	/* [한국어] 시그니처 MR 을 지원하는 HCA 만 PI 를 하드웨어로 검사할 수 있다 */
	    IBK_INTEGRITY_HANDOVER)
		pi_capable = true;

	ctrl->max_fr_pages = nvme_rdma_get_max_fr_pages(ctrl->device->dev,	/* [한국어] 한 번의 빠른 등록으로 덮을 수 있는 페이지 수 — 이것이 최대 전송 크기를 정한다 */
							pi_capable);

	/*
	 * Bind the async event SQE DMA mapping to the admin queue lifetime.
	 * It's safe, since any change in the underlying RDMA device will issue
	 * error recovery and queue re-creation.
	 */
	error = nvme_rdma_alloc_qe(ctrl->device->dev, &ctrl->async_event_sqe,	/* [한국어] AEN 은 태그를 쓰지 않으므로 전용 SQE 를 따로 잡아 둔다 */
			sizeof(struct nvme_command), DMA_TO_DEVICE);	/* [한국어] 호스트가 쓰고 HCA 가 읽는 방향 */
	if (error)
		goto out_free_queue;

	if (new) {	/* [한국어] 재연결이면 태그셋이 이미 있다 — 다시 만들면 대기 중 요청이 갈 곳을 잃는다 */
		error = nvme_alloc_admin_tag_set(&ctrl->ctrl,	/* [한국어] 요청 하나에 SQE 구조체와 데이터 SGL 이 붙는다 */
				&ctrl->admin_tag_set, &nvme_rdma_admin_mq_ops,
				sizeof(struct nvme_rdma_request) +
				NVME_RDMA_DATA_SGL_SIZE);
		if (error)
			goto out_free_async_qe;

	}

	error = nvme_rdma_start_queue(ctrl, 0);	/* [한국어] RDMA 연결 위에 NVMe Connect 를 얹는다 */
	if (error)
		goto out_remove_admin_tag_set;

	error = nvme_enable_ctrl(&ctrl->ctrl);	/* [한국어] CC.EN 을 세우고 CSTS.RDY 를 기다린다 */
	if (error)
		goto out_stop_queue;

	ctrl->ctrl.max_segments = ctrl->max_fr_pages;	/* [한국어] 한 번에 등록 가능한 페이지 수가 곧 세그먼트 상한이다 */
	ctrl->ctrl.max_hw_sectors = ctrl->max_fr_pages << (ilog2(SZ_4K) - 9);	/* [한국어] 페이지 수를 512바이트 섹터 수로 — 4K/512 = 8, 즉 3비트 시프트 */
	if (pi_capable)	/* [한국어] HCA 가 지원할 때만 PI 세그먼트를 허용한다 */
		ctrl->ctrl.max_integrity_segments = ctrl->max_fr_pages;
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ctrl->ctrl.max_integrity_segments = 0;

	nvme_unquiesce_admin_queue(&ctrl->ctrl);	/* [한국어] 이제 Identify 를 보낼 수 있다 */

	error = nvme_init_ctrl_finish(&ctrl->ctrl, false);	/* [한국어] Identify 로 능력을 읽어 코어 구성을 완성한다 */
	if (error)
		goto out_quiesce_queue;

	return 0;	/* [한국어] admin 큐가 살아났다 — 다음은 I/O 큐다 */

out_quiesce_queue:
	nvme_quiesce_admin_queue(&ctrl->ctrl);	/* [한국어] 열었던 큐를 다시 멈춘다 */
	blk_sync_queue(ctrl->ctrl.admin_q);	/* [한국어] 진행 중 완료 처리가 끝나기를 기다린다 */
out_stop_queue:
	nvme_rdma_stop_queue(&ctrl->queues[0]);	/* [한국어] RDMA 연결을 끊고 배출시킨다 */
	nvme_cancel_admin_tagset(&ctrl->ctrl);	/* [한국어] 남은 요청을 취소로 완료시켜 태그를 반납한다 */
out_remove_admin_tag_set:
	if (new)	/* [한국어] 이번에 만든 태그셋만 되돌린다 */
		nvme_remove_admin_tag_set(&ctrl->ctrl);
out_free_async_qe:
	if (ctrl->async_event_sqe.data) {	/* [한국어] 잡혔을 때만 */
		nvme_rdma_free_qe(ctrl->device->dev, &ctrl->async_event_sqe,
			sizeof(struct nvme_command), DMA_TO_DEVICE);
		ctrl->async_event_sqe.data = NULL;	/* [한국어] 다음 시도가 이중 해제하지 않도록 */
	}
out_free_queue:
	nvme_rdma_free_queue(&ctrl->queues[0]);
	return error;
}

/*
 * [한국어]
 * nvme_rdma_configure_io_queues - I/O 큐를 만들고 태그셋과 짝지어 가동한다
 *
 * @ctrl: 대상 컨트롤러
 * @new:  최초 생성이면 태그셋도 만든다. 재연결이면 기존 것을 쓴다.
 * @return: 0 이면 모든 큐가 LIVE. 음수면 되감았다.
 *
 * 재연결에서 큐 개수가 달라질 수 있다는 사실이 이 함수를 복잡하게 만든다.
 * 태그셋은 하드웨어 큐 수를 알고 있고, 그 위에 gendisk 와 진행 중인 요청이
 * 매달려 있어 다시 만들 수 없다. 그래서 개수 변경을 태그셋에 반영해야 한다.
 *
 * 그 반영이 안전하려면 진행 중인 I/O 가 없어야 한다. freeze 가 그 장치다 --
 * 새 요청을 막고 이미 들어온 것이 끝나기를 기다린다. 그런데 그 대기 자체가
 * 영원히 끝나지 않을 수 있다. 위 영어 주석이 그 경우를 다룬다: 타임아웃이
 * 나면 이미 무언가에 막혀 있을 가능성이 크므로 안전을 택해 초기화를 실패시킨다.
 *
 * 큐 가동이 두 번으로 나뉜 이유도 여기 있다. 위 영어 주석대로, 먼저 태그셋이
 * 아는 범위 안의 큐만 가동한다 -- 그래야 freeze 를 기다리는 동안 태그셋이
 * 모르는 큐로 요청이 가지 않는다. 개수가 늘어난 경우 나머지는 태그셋을
 * 갱신한 뒤에 가동한다.
 *
 * 실행 컨텍스트: 연결 경로. freeze 대기로 오래 잠들 수 있다.
 *
 * 호출 체인:
 *   nvme_rdma_setup_ctrl → [이 함수] → nvme_rdma_alloc_io_queues
 *     → blk_mq_update_nr_hw_queues → nvme_rdma_start_io_queues
 */
static int nvme_rdma_configure_io_queues(struct nvme_rdma_ctrl *ctrl, bool new)
{
	int ret, nr_queues;

	ret = nvme_rdma_alloc_io_queues(ctrl);	/* [한국어] 개수 협상과 큐 생성 */
	if (ret)
		return ret;

	if (new) {
		ret = nvme_rdma_alloc_tag_set(&ctrl->ctrl);	/* [한국어] 최초 생성에서만 태그셋을 만든다 */
		if (ret)
			goto out_free_io_queues;
	}

	/*
	 * Only start IO queues for which we have allocated the tagset
	 * and limited it to the available queues. On reconnects, the
	 * queue number might have changed.
	 */
	/* [한국어] 위 영어 주석대로 태그셋이 아는 범위 안의 큐만 먼저 가동한다.
	 * 그래야 아래 freeze 를 기다리는 동안 태그셋이 모르는 큐로 요청이 가지 않는다. */
	nr_queues = min(ctrl->tag_set.nr_hw_queues + 1, ctrl->ctrl.queue_count);
	ret = nvme_rdma_start_io_queues(ctrl, 1, nr_queues);	/* [한국어] Fabrics Connect 를 보내 LIVE 로 만든다 */
	if (ret)
		goto out_cleanup_tagset;

	if (!new) {	/* [한국어] 재연결 — 태그셋을 재사용하므로 개수 변경을 반영해야 한다 */
		nvme_start_freeze(&ctrl->ctrl);	/* [한국어] 새 요청을 막는다 */
		nvme_unquiesce_io_queues(&ctrl->ctrl);	/* [한국어] 이미 들어온 것은 흘려보내 끝나게 한다 */
		if (!nvme_wait_freeze_timeout(&ctrl->ctrl, NVME_IO_TIMEOUT)) {
			/*
			 * If we timed out waiting for freeze we are likely to
			 * be stuck.  Fail the controller initialization just
			 * to be safe.
			 */
			/* [한국어] 위 영어 주석대로 freeze 가 끝나지 않는다는 것은 이미
			 * 무언가에 막혀 있다는 뜻이다. 억지로 진행하지 않고 실패시킨다. */
			ret = -ENODEV;
			nvme_unfreeze(&ctrl->ctrl);	/* [한국어] 실패해도 freeze 는 반드시 풀어야 한다 — 아니면 큐가 영구히 멈춘다 */
			goto out_wait_freeze_timed_out;
		}
		blk_mq_update_nr_hw_queues(ctrl->ctrl.tagset,	/* [한국어] 진행 중 I/O 가 없는 지금이 개수를 바꿀 수 있는 유일한 시점이다 */
			ctrl->ctrl.queue_count - 1);
		nvme_unfreeze(&ctrl->ctrl);
	}

	/*
	 * If the number of queues has increased (reconnect case)
	 * start all new queues now.
	 */
	/* [한국어] 위 영어 주석대로, 개수가 늘어난 경우 태그셋 갱신 뒤에야
	 * 나머지 큐를 가동할 수 있다. */
	ret = nvme_rdma_start_io_queues(ctrl, nr_queues,
					ctrl->tag_set.nr_hw_queues + 1);
	if (ret)
		goto out_wait_freeze_timed_out;

	return 0;

out_wait_freeze_timed_out:
	nvme_quiesce_io_queues(&ctrl->ctrl);
	nvme_sync_io_queues(&ctrl->ctrl);
	nvme_rdma_stop_io_queues(ctrl);
out_cleanup_tagset:
	nvme_cancel_tagset(&ctrl->ctrl);	/* [한국어] 남은 요청을 실패로 완료시킨다 */
	if (new)
		nvme_remove_io_tag_set(&ctrl->ctrl);	/* [한국어] 우리가 만든 것일 때만 해제한다 */
out_free_io_queues:
	nvme_rdma_free_io_queues(ctrl);
	return ret;
}

/*
 * [한국어]
 * nvme_rdma_teardown_admin_queue - admin 큐를 접는다
 *
 * @ctrl:   대상 컨트롤러
 * @remove: 태그셋까지 없앨 것인가. 재연결이면 false 로 남겨 둔다.
 * @return: 없음
 *
 * 순서가 전부다. 먼저 큐를 멈춰 새 명령을 막고, blk_sync_queue 로
 * 진행 중 완료 처리를 배출시키고, 그다음에야 RDMA 연결을 끊는다.
 * 그 뒤 남은 요청을 취소로 완료시켜야 태그가 반납된다.
 *
 * remove 가 참일 때만 unquiesce 하는 것이 요점이다. 태그셋을 없애려면
 * 멈춘 큐에 남은 요청이 빠져나가야 하므로 먼저 열어야 한다. 재연결에서는
 * 멈춘 채로 두어야 새 세션이 설 때까지 요청이 기다린다.
 *
 * 실행 컨텍스트: 오류 복구/삭제 경로. 오래 잠든다.
 *
 * 호출 체인:
 *   nvme_rdma_error_recovery_work / _delete_ctrl → [이 함수]
 *     → nvme_rdma_stop_queue → nvme_rdma_destroy_admin_queue
 */
static void nvme_rdma_teardown_admin_queue(struct nvme_rdma_ctrl *ctrl,
		bool remove)
{
	nvme_quiesce_admin_queue(&ctrl->ctrl);	/* [한국어] 새 명령을 막는 것이 먼저다 */
	blk_sync_queue(ctrl->ctrl.admin_q);	/* [한국어] 진행 중 완료 처리를 배출시킨다 */
	nvme_rdma_stop_queue(&ctrl->queues[0]);	/* [한국어] 그 뒤에야 RDMA 연결을 끊는다 */
	nvme_cancel_admin_tagset(&ctrl->ctrl);	/* [한국어] 남은 요청을 취소로 완료시켜야 태그가 돌아온다 */
	if (remove) {	/* [한국어] 태그셋까지 없앨 때만 */
		nvme_unquiesce_admin_queue(&ctrl->ctrl);	/* [한국어] 멈춘 큐에 남은 요청이 태그셋 해제를 막으므로 먼저 연다 */
		nvme_remove_admin_tag_set(&ctrl->ctrl);
	}
	nvme_rdma_destroy_admin_queue(ctrl);	/* [한국어] AEN SQE 와 큐 자원을 반납한다 */
}

/*
 * [한국어]
 * nvme_rdma_teardown_io_queues - I/O 큐를 모두 접는다
 *
 * @ctrl:   대상 컨트롤러
 * @remove: 태그셋까지 없앨 것인가
 * @return: 없음
 *
 * admin 쪽과 같은 순서를 I/O 큐 전체에 적용한다. queue_count > 1 검사가
 * 앞에 있는 이유는 admin 만 세운 상태에서도 이 경로가 불릴 수 있기
 * 때문이다 -- I/O 큐가 없으면 할 일도 없다.
 *
 * sync_io_queues 가 quiesce 다음인 것이 중요하다. 멈추기 전에 기다리면
 * 그 사이 들어온 요청 때문에 영원히 끝나지 않는다.
 *
 * 실행 컨텍스트: 오류 복구/삭제 경로. 오래 잠든다.
 *
 * 호출 체인:
 *   nvme_rdma_error_recovery_work / _delete_ctrl → [이 함수]
 *     → nvme_rdma_stop_io_queues → nvme_rdma_free_io_queues
 */
static void nvme_rdma_teardown_io_queues(struct nvme_rdma_ctrl *ctrl,
		bool remove)
{
	if (ctrl->ctrl.queue_count > 1) {	/* [한국어] admin 만 세운 상태에서도 불리므로 확인한다 */
		nvme_quiesce_io_queues(&ctrl->ctrl);	/* [한국어] 새 I/O 를 막는다 */
		nvme_sync_io_queues(&ctrl->ctrl);	/* [한국어] 멈춘 뒤에 기다려야 끝난다 — 순서를 바꾸면 새 요청이 계속 들어온다 */
		nvme_rdma_stop_io_queues(ctrl);	/* [한국어] 각 큐를 끊고 배출시킨다 */
		nvme_cancel_tagset(&ctrl->ctrl);	/* [한국어] 남은 요청을 취소로 완료시킨다 */
		if (remove) {
			nvme_unquiesce_io_queues(&ctrl->ctrl);	/* [한국어] 태그셋을 없애려면 큐를 열어 남은 요청을 빼야 한다 */
			nvme_remove_io_tag_set(&ctrl->ctrl);
		}
		nvme_rdma_free_io_queues(ctrl);	/* [한국어] 큐 자원을 반납한다 */
	}
}

/*
 * [한국어]
 * nvme_rdma_stop_ctrl - 이 트랜스포트의 백그라운드 워크를 멈춘다
 *
 * @nctrl: 대상 컨트롤러
 * @return: 없음
 *
 * 코어의 stop_ctrl vtable 진입점이다. 두 워크를 다루는 방식이 다른
 * 것에 주의할 것 -- err_work 는 flush 하고 reconnect_work 는 cancel 한다.
 * 진행 중인 오류 복구는 끝까지 가는 편이 안전하지만(중간에 끊으면
 * 자원이 어정쩡하게 남는다), 아직 시작하지 않은 재연결은 시작할
 * 이유가 없기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 오래 잠든다.
 *
 * 호출 체인: nvme_stop_ctrl → ops->stop_ctrl → [이 함수]
 */
static void nvme_rdma_stop_ctrl(struct nvme_ctrl *nctrl)
{
	struct nvme_rdma_ctrl *ctrl = to_rdma_ctrl(nctrl);

	flush_work(&ctrl->err_work);	/* [한국어] 진행 중 복구는 끝까지 가는 편이 안전하다 — 중간에 끊으면 자원이 어정쩡하게 남는다 */
	cancel_delayed_work_sync(&ctrl->reconnect_work);	/* [한국어] 아직 시작 안 한 재연결은 시작할 이유가 없다 */
}

/*
 * [한국어]
 * nvme_rdma_free_ctrl - 컨트롤러 구조체와 큐 배열을 반납한다
 *
 * @nctrl: 해제되는 컨트롤러
 * @return: 없음
 *
 * 코어가 마지막 참조를 놓을 때 불린다. 전역 목록에서 빼는 일이 먼저이며,
 * list_empty 검사는 초기화 도중 실패해 목록에 오르지 못한 컨트롤러를
 * 위한 것이다. 그런 경우 옵션도 아직 이 컨트롤러 소유가 아니라
 * 해제하면 안 된다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 잠들 수 있다.
 *
 * 호출 체인: nvme_free_ctrl → ops->free_ctrl → [이 함수]
 */
static void nvme_rdma_free_ctrl(struct nvme_ctrl *nctrl)
{
	struct nvme_rdma_ctrl *ctrl = to_rdma_ctrl(nctrl);

	if (list_empty(&ctrl->list))	/* [한국어] 초기화 도중 실패해 전역 목록에 오르지 못한 컨트롤러다 */
		goto free_ctrl;	/* [한국어] 옵션도 아직 이 컨트롤러 소유가 아니라 해제하면 안 된다 */

	mutex_lock(&nvme_rdma_ctrl_mutex);	/* [한국어] 중복 연결 검사가 이 목록을 훑는다 */
	list_del(&ctrl->list);
	mutex_unlock(&nvme_rdma_ctrl_mutex);

	nvmf_free_options(nctrl->opts);	/* [한국어] 목록에 올랐다면 옵션 소유권도 이 컨트롤러에 있다 */
free_ctrl:
	kfree(ctrl->queues);	/* [한국어] 큐 배열 */
	kfree(ctrl);
}

/*
 * [한국어]
 * nvme_rdma_reconnect_or_remove - 다시 시도할지 포기할지 정한다
 *
 * @ctrl:   대상 컨트롤러
 * @status: 직전 시도가 남긴 오류. 재시도 가치 판단의 입력이다.
 * @return: 없음
 *
 * 연결 시도가 실패할 때마다 불리는 갈림길이다. 상태가 CONNECTING 이
 * 아니면 아무것도 하지 않는다 -- 리셋이나 삭제가 이미 이 컨트롤러를
 * 가져갔다는 뜻이므로, 여기서 재연결을 걸면 그 경로와 부딪힌다.
 *
 * WARN 이 NEW 와 LIVE 만 잡는 것에 주의할 것. 그 두 상태로 여기 오는
 * 것은 상태 기계가 깨졌다는 뜻이지만, DELETING 이나 RESETTING 은
 * 정상적인 경쟁이라 경고 없이 조용히 물러난다.
 *
 * 실행 컨텍스트: 연결/복구 워크. 잠들지 않는다(예약만 한다).
 *
 * 호출 체인:
 *   nvme_rdma_reconnect_ctrl_work / _error_recovery_work → [이 함수]
 *     → queue_delayed_work 또는 nvme_delete_ctrl
 */
static void nvme_rdma_reconnect_or_remove(struct nvme_rdma_ctrl *ctrl,
					  int status)
{
	enum nvme_ctrl_state state = nvme_ctrl_state(&ctrl->ctrl);	/* [한국어] 한 번만 읽는다 — 아래 판단이 일관되게 같은 값을 봐야 한다 */

	/* If we are resetting/deleting then do nothing */
	if (state != NVME_CTRL_CONNECTING) {	/* [한국어] 위 영어 주석대로 리셋이나 삭제가 이미 이 컨트롤러를 가져갔다 */
		WARN_ON_ONCE(state == NVME_CTRL_NEW || state == NVME_CTRL_LIVE);	/* [한국어] 그 둘로 여기 오는 것은 상태 기계가 깨진 것이다. DELETING·RESETTING 은 정상 경쟁이라 조용히 물러난다 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}

	if (nvmf_should_reconnect(&ctrl->ctrl, status)) {	/* [한국어] ctrl_loss_tmo 안이고 오류가 재시도할 만한 것인지 묻는다 */
		dev_info(ctrl->ctrl.device, "Reconnecting in %d seconds...\n",	/* [한국어] 사용자가 장치가 왜 안 보이는지 알 수 있게 남긴다 */
			ctrl->ctrl.opts->reconnect_delay);
		queue_delayed_work(nvme_wq, &ctrl->reconnect_work,	/* [한국어] 간격을 두고 다시 시도한다 — 즉시 재시도는 대상을 두드린다 */
				ctrl->ctrl.opts->reconnect_delay * HZ);
	} else {
		nvme_delete_ctrl(&ctrl->ctrl);	/* [한국어] 더 시도할 가치가 없다 — 컨트롤러를 지운다 */
	}
}

/*
 * [한국어]
 * nvme_rdma_setup_ctrl - 컨트롤러를 admin 부터 LIVE 까지 세운다
 *
 * @ctrl: 세울 컨트롤러
 * @new:  최초 생성인가(true), 재연결인가(false). 태그셋을 만들지 재사용할지와
 *        실패 시 어디까지 되감을지가 갈린다.
 * @return: 0 이면 LIVE. 음수면 실패이며 역순으로 되돌렸다.
 *
 * tcp.c 의 같은 이름 함수와 뼈대가 같다 -- admin → 능력 검증 → 큐 깊이 조정
 * → I/O 큐 → LIVE. 다른 것은 검증하는 능력과 깊이 상한의 근거다.
 *
 * keyed SGL 을 요구하는 것이 RDMA 만의 조건이다. 원격이 rkey 로 우리 메모리에
 * 직접 접근하는 것이 이 트랜스포트의 전제인데, 키를 실을 서술자 형식을
 * 지원하지 않으면 그 방식 자체가 성립하지 않는다.
 *
 * 큐 깊이 상한이 세 겹이라는 점도 이 트랜스포트 고유다:
 *   - 사용자 요청 vs sqsize: 경고만. sqsize 가 어차피 실질 상한이다.
 *   - sqsize vs RDMA 최대 큐 크기: 값을 줄인다. 이 상한은 QP 에 올릴 수 있는
 *     WR 수에서 나오며, 보호 정보를 쓰면 요청당 WR 이 더 필요해 상한이 낮아진다.
 *     그래서 max_integrity_segments 유무로 두 값이 갈린다.
 *   - sqsize vs maxcmd: 값을 줄인다. 컨트롤러가 감당할 수 있는 명령 수다.
 *
 * SAOS(SGL Address as Offset Supported)를 확인해 인라인 사용 여부를 정하는
 * 것도 여기다. 그 능력이 있어야 캡슐 안 오프셋으로 데이터를 가리킬 수 있고,
 * map_data 의 인라인 경로가 열린다.
 *
 * 실행 컨텍스트: reconnect_ctrl_work / reset_ctrl_work. 잠들 수 있다.
 *
 * 호출 체인:
 *   nvme_rdma_create_ctrl / _reconnect_ctrl_work → [이 함수]
 *     → nvme_rdma_configure_admin_queue → _configure_io_queues → nvme_start_ctrl
 */
static int nvme_rdma_setup_ctrl(struct nvme_rdma_ctrl *ctrl, bool new)
{
	int ret;
	bool changed;
	u16 max_queue_size;

	ret = nvme_rdma_configure_admin_queue(ctrl, new);	/* [한국어] CM 3단계 연결과 Fabrics Connect 까지 끝낸다 */
	if (ret)
		return ret;

	if (ctrl->ctrl.icdoff) {	/* [한국어] 캡슐 내 데이터 오프셋을 요구하는 컨트롤러 */
		ret = -EOPNOTSUPP;
		dev_err(ctrl->ctrl.device, "icdoff is not supported!\n");
		goto destroy_admin;
	}

	if (!(ctrl->ctrl.sgls & NVME_CTRL_SGLS_KSDBDS)) {	/* [한국어] 키 있는 SGL 서술자 지원 여부 */
		ret = -EOPNOTSUPP;
		dev_err(ctrl->ctrl.device,	/* [한국어] rkey 를 실을 수 없으면 원격 직접 접근이라는 이 트랜스포트의 전제가 무너진다 */
			"Mandatory keyed sgls are not supported!\n");
		goto destroy_admin;
	}

	if (ctrl->ctrl.opts->queue_size > ctrl->ctrl.sqsize + 1) {	/* [한국어] 사용자 요청이 컨트롤러 상한을 넘는다 */
		dev_warn(ctrl->ctrl.device,	/* [한국어] 경고만 — sqsize 가 어차피 실질 상한이라 동작에는 문제가 없다 */
			"queue_size %zu > ctrl sqsize %u, clamping down\n",
			ctrl->ctrl.opts->queue_size, ctrl->ctrl.sqsize + 1);
	}

	if (ctrl->ctrl.max_integrity_segments)	/* [한국어] 보호 정보를 쓰면 요청당 WR 이 더 필요하다 */
		max_queue_size = NVME_RDMA_MAX_METADATA_QUEUE_SIZE;	/* [한국어] 그만큼 큐 깊이 상한이 낮아진다 */
	else
		max_queue_size = NVME_RDMA_MAX_QUEUE_SIZE;

	if (ctrl->ctrl.sqsize + 1 > max_queue_size) {	/* [한국어] QP 에 올릴 수 있는 WR 수에서 나오는 상한이다 */
		dev_warn(ctrl->ctrl.device,
			 "ctrl sqsize %u > max queue size %u, clamping down\n",
			 ctrl->ctrl.sqsize + 1, max_queue_size);
		ctrl->ctrl.sqsize = max_queue_size - 1;	/* [한국어] 넘기면 ib_post_send 가 실패하므로 반드시 줄여야 한다 */
	}

	if (ctrl->ctrl.sqsize + 1 > ctrl->ctrl.maxcmd) {	/* [한국어] 컨트롤러가 감당할 수 있는 명령 수 */
		dev_warn(ctrl->ctrl.device,
			"sqsize %u > ctrl maxcmd %u, clamping down\n",
			ctrl->ctrl.sqsize + 1, ctrl->ctrl.maxcmd);
		ctrl->ctrl.sqsize = ctrl->ctrl.maxcmd - 1;
	}

	if (ctrl->ctrl.sgls & NVME_CTRL_SGLS_SAOS)	/* [한국어] 캡슐 내 오프셋으로 데이터를 가리킬 수 있는가 */
		ctrl->use_inline_data = true;	/* [한국어] 그래야 map_data 의 인라인 경로가 열린다 */

	if (ctrl->ctrl.queue_count > 1) {
		ret = nvme_rdma_configure_io_queues(ctrl, new);	/* [한국어] I/O 큐마다 QP 를 세우고 Connect 한다 */
		if (ret)
			goto destroy_admin;
	}

	changed = nvme_change_ctrl_state(&ctrl->ctrl, NVME_CTRL_LIVE);
	if (!changed) {
		/*
		 * state change failure is ok if we started ctrl delete,
		 * unless we're during creation of a new controller to
		 * avoid races with teardown flow.
		 */
		/* [한국어] 위 영어 주석대로 삭제가 시작된 경우라면 전이 실패는 정상이다.
		 * 최초 생성 중이라면 삭제가 시작될 리 없으므로 아래 WARN 이 그 가정을 검증한다. */
		enum nvme_ctrl_state state = nvme_ctrl_state(&ctrl->ctrl);

		WARN_ON_ONCE(state != NVME_CTRL_DELETING &&
			     state != NVME_CTRL_DELETING_NOIO);
		WARN_ON_ONCE(new);
		ret = -EINVAL;
		goto destroy_io;
	}

	nvme_start_ctrl(&ctrl->ctrl);	/* [한국어] keep-alive 를 걸고 네임스페이스를 스캔한다 */
	return 0;

destroy_io:
	if (ctrl->ctrl.queue_count > 1) {
		nvme_quiesce_io_queues(&ctrl->ctrl);	/* [한국어] 새 요청 유입을 멈추고 */
		nvme_sync_io_queues(&ctrl->ctrl);	/* [한국어] 진입한 제출이 끝나기를 기다린 뒤 */
		nvme_rdma_stop_io_queues(ctrl);	/* [한국어] QP 를 내린다 */
		nvme_cancel_tagset(&ctrl->ctrl);	/* [한국어] 남은 요청을 실패로 완료시킨다 */
		if (new)
			nvme_remove_io_tag_set(&ctrl->ctrl);	/* [한국어] 최초 생성이었다면 태그셋도 우리 것이므로 해제한다 */
		nvme_rdma_free_io_queues(ctrl);
	}
destroy_admin:
	nvme_stop_keep_alive(&ctrl->ctrl);
	nvme_rdma_teardown_admin_queue(ctrl, new);	/* [한국어] new 가 태그셋 해제 여부를 결정한다 */
	return ret;
}

/*
 * [한국어]
 * nvme_rdma_reconnect_ctrl_work - 재연결을 한 번 시도하고 결과를 넘긴다
 *
 * @work: reconnect_work(지연 워크)
 * @return: 없음
 *
 * 재연결 루프의 한 바퀴다. 실패하면 reconnect_or_remove 가 다시 이 워크를
 * 예약하므로, 둘이 서로를 부르며 시도 횟수가 소진될 때까지 반복한다.
 *
 * nr_reconnects 를 여기서 올리고 성공 시 0 으로 되돌리는 것이 그 횟수 관리의
 * 전부다. 로그에 시도 횟수를 함께 남기는 것은, 몇 번 만에 붙었는지가
 * 링크 품질을 가늠하는 단서가 되기 때문이다.
 *
 * setup_ctrl 에 false 를 넘기는 것이 중요하다 -- 재연결이므로 태그셋을
 * 다시 만들지 않고 기존 것을 쓴다. 그 위에 gendisk 와 진행 중 요청이
 * 매달려 있기 때문이다.
 *
 * 실행 컨텍스트: nvme_wq 워크큐. 연결 전체가 여기서 일어나 오래 잠든다.
 *
 * 호출 체인:
 *   nvme_rdma_reconnect_or_remove → queue_delayed_work → [이 함수]
 *     → nvme_rdma_setup_ctrl
 */
static void nvme_rdma_reconnect_ctrl_work(struct work_struct *work)
{
	struct nvme_rdma_ctrl *ctrl = container_of(to_delayed_work(work),
			struct nvme_rdma_ctrl, reconnect_work);
	int ret;

	++ctrl->ctrl.nr_reconnects;	/* [한국어] 시도 횟수. 성공하면 아래에서 0 으로 되돌린다 */

	ret = nvme_rdma_setup_ctrl(ctrl, false);	/* [한국어] false — 재연결이므로 태그셋을 재사용한다 */
	if (ret)
		goto requeue;

	dev_info(ctrl->ctrl.device, "Successfully reconnected (%d attempts)\n",	/* [한국어] 몇 번 만에 붙었는지가 링크 품질을 가늠하는 단서다 */
			ctrl->ctrl.nr_reconnects);

	ctrl->ctrl.nr_reconnects = 0;	/* [한국어] 다음 단절을 위해 초기화 */

	return;

requeue:
	dev_info(ctrl->ctrl.device, "Failed reconnect attempt %d/%d\n",
		 ctrl->ctrl.nr_reconnects, ctrl->ctrl.opts->max_reconnects);
	nvme_rdma_reconnect_or_remove(ctrl, ret);	/* [한국어] 다시 시도할지 포기할지는 그쪽이 정한다. 다시면 이 워크가 재예약된다 */
}

/*
 * [한국어]
 * nvme_rdma_error_recovery_work - 연결을 통째로 내리고 재연결 루프에 넘긴다
 *
 * @work: err_work
 * @return: 없음
 *
 * error_recovery 가 상태만 옮기고 깨운 워크다. 실제 복구가 여기서 일어난다.
 *
 * 해체 순서에 이유가 있다:
 *   1) keep-alive 정지 -- 내리는 중에 새 admin 명령이 나가면 안 된다.
 *   2) async_event_work flush -- AEN 재무장이 진행 중일 수 있고, 큐를 내린
 *      뒤에 그것이 돌면 사라진 큐로 명령을 보낸다.
 *   3) I/O 큐 해체 후 곧바로 unquiesce -- 멈춘 채 두면 새 요청이 쌓여
 *      기다리지만, 열어 두면 즉시 실패해 상위가 대응할 수 있다.
 *   4) admin 큐도 같은 순서로.
 *   5) 인증 중단 -- 진행 중인 DH-HMAC-CHAP 협상을 끊는다.
 *
 * false 를 넘기는 것은 태그셋을 남긴다는 뜻이다. 재연결에서 다시 쓴다.
 *
 * CONNECTING 전이 실패는 삭제가 시작된 경우이며, 그때는 그쪽이 정리하므로
 * 여기서 그대로 돌아간다.
 *
 * 실행 컨텍스트: nvme_reset_wq 워크큐. 오래 잠들 수 있다.
 *
 * 호출 체인:
 *   nvme_rdma_error_recovery → [이 함수] → nvme_rdma_teardown_io_queues
 *     → nvme_rdma_reconnect_or_remove
 */
static void nvme_rdma_error_recovery_work(struct work_struct *work)
{
	struct nvme_rdma_ctrl *ctrl = container_of(work,
			struct nvme_rdma_ctrl, err_work);

	nvme_stop_keep_alive(&ctrl->ctrl);	/* [한국어] 내리는 중에 새 admin 명령이 나가면 안 된다 */
	flush_work(&ctrl->ctrl.async_event_work);	/* [한국어] AEN 재무장이 진행 중일 수 있다 — 큐를 내린 뒤 돌면 사라진 큐로 명령을 보낸다 */
	nvme_rdma_teardown_io_queues(ctrl, false);	/* [한국어] false — 태그셋은 남긴다. 재연결에서 다시 쓴다 */
	nvme_unquiesce_io_queues(&ctrl->ctrl);	/* [한국어] 해체하면서 여는 것이 의도적이다 — 새 요청이 쌓이는 대신 즉시 실패해 상위가 대응한다 */
	nvme_rdma_teardown_admin_queue(ctrl, false);
	nvme_unquiesce_admin_queue(&ctrl->ctrl);
	nvme_auth_stop(&ctrl->ctrl);	/* [한국어] 진행 중인 대역 내 인증 협상을 끊는다 */

	if (!nvme_change_ctrl_state(&ctrl->ctrl, NVME_CTRL_CONNECTING)) {
		/* state change failure is ok if we started ctrl delete */
		/* [한국어] 위 영어 주석대로 삭제가 시작된 경우라면 정상이다 */
		enum nvme_ctrl_state state = nvme_ctrl_state(&ctrl->ctrl);

		WARN_ON_ONCE(state != NVME_CTRL_DELETING &&	/* [한국어] 그 밖의 상태라면 상태 기계가 어긋난 것이다 */
			     state != NVME_CTRL_DELETING_NOIO);
		return;	/* [한국어] 삭제 경로가 정리하므로 여기서 물러난다 */
	}

	nvme_rdma_reconnect_or_remove(ctrl, 0);	/* [한국어] 재연결 루프에 넘긴다 */
}

static void nvme_rdma_error_recovery(struct nvme_rdma_ctrl *ctrl)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{
	if (!nvme_change_ctrl_state(&ctrl->ctrl, NVME_CTRL_RESETTING))	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	dev_warn(ctrl->ctrl.device, "starting error recovery\n");	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	queue_work(nvme_reset_wq, &ctrl->err_work);	/* [한국어] 워크큐 — 송수신/재연결/복구 비동기 실행 */
}

static void nvme_rdma_end_request(struct nvme_rdma_request *req)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{
	struct request *rq = blk_mq_rq_from_pdu(req);	/* [한국어] blk-mq — 태그·hctx·타임아웃·맵·완료 연동 */

	if (!refcount_dec_and_test(&req->ref))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	if (!nvme_try_complete_req(rq, req->status, req->result))	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
		nvme_rdma_complete_rq(rq);
}

static void nvme_rdma_wr_error(struct ib_cq *cq, struct ib_wc *wc,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		const char *op)
{
	struct nvme_rdma_queue *queue = wc->qp->qp_context;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct nvme_rdma_ctrl *ctrl = queue->ctrl;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	if (nvme_ctrl_state(&ctrl->ctrl) == NVME_CTRL_LIVE)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_info(ctrl->ctrl.device,
			     "%s for CQE 0x%p failed with status %s (%d)\n",
			     op, wc->wr_cqe,
			     ib_wc_status_msg(wc->status), wc->status);
	nvme_rdma_error_recovery(ctrl);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
}

static void nvme_rdma_memreg_done(struct ib_cq *cq, struct ib_wc *wc)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{
	if (unlikely(wc->status != IB_WC_SUCCESS))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_rdma_wr_error(cq, wc, "MEMREG");
}

static void nvme_rdma_inv_rkey_done(struct ib_cq *cq, struct ib_wc *wc)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{
	struct nvme_rdma_request *req =	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		container_of(wc->wr_cqe, struct nvme_rdma_request, reg_cqe);

	if (unlikely(wc->status != IB_WC_SUCCESS))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_rdma_wr_error(cq, wc, "LOCAL_INV");
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_rdma_end_request(req);
}

static int nvme_rdma_inv_rkey(struct nvme_rdma_queue *queue,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		struct nvme_rdma_request *req)
{
	struct ib_send_wr wr = {
		.opcode		    = IB_WR_LOCAL_INV,	/* [한국어] LOCAL_INV — 원격이 아니라 로컬 HCA 에게 이 rkey 를 무효화하라고 시킨다 */
		.next		    = NULL,	/* [한국어] 작업 하나만 올린다. 체인 없음 */
		.num_sge	    = 0,	/* [한국어] 데이터 전송이 없는 제어 작업이라 산재 목록이 필요 없다 */
		.send_flags	    = IB_SEND_SIGNALED,	/* [한국어] 완료 통지를 반드시 받아야 한다 — 무효화가 끝나야 MR 을 재사용할 수 있다 */
		.ex.invalidate_rkey = req->mr->rkey,	/* [한국어] 무효화할 키. 이것이 살아 있는 동안은 원격이 이 메모리에 접근할 수 있다 */
	};

	req->reg_cqe.done = nvme_rdma_inv_rkey_done;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	wr.wr_cqe = &req->reg_cqe;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return ib_post_send(queue->qp, &wr, NULL);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
}

static void nvme_rdma_dma_unmap_req(struct ib_device *ibdev, struct request *rq)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{
	struct nvme_rdma_request *req = blk_mq_rq_to_pdu(rq);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	if (blk_integrity_rq(rq)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ib_dma_unmap_sg(ibdev, req->metadata_sgl->sg_table.sgl,	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
				req->metadata_sgl->nents, rq_dma_dir(rq));
		sg_free_table_chained(&req->metadata_sgl->sg_table,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				      NVME_INLINE_METADATA_SG_CNT);
	}

	ib_dma_unmap_sg(ibdev, req->data_sgl.sg_table.sgl, req->data_sgl.nents,	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
			rq_dma_dir(rq));
	sg_free_table_chained(&req->data_sgl.sg_table, NVME_INLINE_SG_CNT);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}

static void nvme_rdma_unmap_data(struct nvme_rdma_queue *queue,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		struct request *rq)
{
	struct nvme_rdma_request *req = blk_mq_rq_to_pdu(rq);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct nvme_rdma_device *dev = queue->device;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct ib_device *ibdev = dev->dev;	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	struct list_head *pool = &queue->qp->rdma_mrs;	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */

	if (!blk_rq_nr_phys_segments(rq))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	if (req->use_sig_mr)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		pool = &queue->qp->sig_mrs;

	if (req->mr) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ib_mr_pool_put(queue->qp, pool, req->mr);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
		req->mr = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}

	nvme_rdma_dma_unmap_req(ibdev, rq);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
}

static int nvme_rdma_set_sg_null(struct nvme_command *c)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{
	struct nvme_keyed_sgl_desc *sg = &c->common.dptr.ksgl;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	sg->addr = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	put_unaligned_le24(0, sg->length);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	put_unaligned_le32(0, sg->key);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	sg->type = NVME_KEY_SGL_FMT_DATA_DESC << 4;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

/*
 * [한국어]
 * nvme_rdma_map_sg_inline - 데이터를 명령 캡슐에 함께 실어 보낸다
 *
 * @queue: 대상 큐
 * @req:   요청. 이 함수가 req->sge 의 두 번째 칸부터 채운다.
 * @c:     보낼 명령. dptr.sgl 에 "데이터가 캡슐 안에 있다"고 표시한다.
 * @count: DMA 매핑을 마친 조각 수
 * @return: 항상 0 (실패할 여지가 없다)
 *
 * 세 가지 전송 방식 중 가장 빠른 길이다. MR 등록도, 원격의 RDMA Read 도 없이
 * 명령과 데이터가 한 번에 건너간다. 작은 쓰기의 지연이 여기서 결정된다.
 *
 * 대신 조건이 까다롭다 -- 데이터가 캡슐 여유 공간(icdoff 이후)에 들어갈 만큼
 * 작아야 하고, 조각 수가 장치의 인라인 SGE 한도 안이어야 한다. 그 판정은
 * 호출자 nvme_rdma_map_data 가 미리 한다.
 *
 * lkey 를 쓰는 것에 주목할 것: 이 데이터는 원격이 읽어 가는 것이 아니라
 * 로컬 HCA 가 읽어 패킷에 실어 보내는 것이므로, 원격 접근용 rkey 가 필요 없다.
 * 노출이 없으니 등록도 무효화도 없다.
 *
 * 호출 체인:
 *   nvme_rdma_map_data → [이 함수] → (ib_post_send 가 sge 를 읽어 전송)
 */
static int nvme_rdma_map_sg_inline(struct nvme_rdma_queue *queue,
		struct nvme_rdma_request *req, struct nvme_command *c,
		int count)
{
	struct nvme_sgl_desc *sg = &c->common.dptr.sgl;	/* [한국어] 명령의 데이터 포인터 자리 — 키 없는 SGL 서술자로 쓴다 */
	struct ib_sge *sge = &req->sge[1];	/* [한국어] 0번 칸은 명령 캡슐이 쓰므로 1번부터 데이터를 채운다 */
	struct scatterlist *sgl;
	u32 len = 0;	/* [한국어] 인라인으로 실은 총 바이트 — 아래에서 서술자 길이로 쓴다 */
	int i;

	for_each_sg(req->data_sgl.sg_table.sgl, sgl, count, i) {	/* [한국어] 매핑된 조각을 순서대로 송신 SGE 로 옮긴다 */
		sge->addr = sg_dma_address(sgl);	/* [한국어] HCA 가 읽을 DMA 주소 */
		sge->length = sg_dma_len(sgl);
		sge->lkey = queue->device->pd->local_dma_lkey;	/* [한국어] 로컬 키 — 이 데이터는 우리 HCA 가 읽어 보낼 뿐 원격에 노출되지 않는다 */
		len += sge->length;
		sge++;
	}

	sg->addr = cpu_to_le64(queue->ctrl->ctrl.icdoff);	/* [한국어] 캡슐 안에서 데이터가 시작하는 오프셋. 타겟이 Identify 로 알려 준 값이다 */
	sg->length = cpu_to_le32(len);	/* [한국어] 캡슐에 실린 데이터의 총 길이 */
	sg->type = (NVME_SGL_FMT_DATA_DESC << 4) | NVME_SGL_FMT_OFFSET;	/* [한국어] OFFSET 형식 — 주소가 아니라 캡슐 내 오프셋이라는 뜻이다 */

	req->num_sge += count;	/* [한국어] 명령 캡슐 한 칸에 데이터 조각 수를 더한 것이 최종 SGE 개수 */
	return 0;
}

/*
 * [한국어]
 * nvme_rdma_map_sg_single - 전역 rkey 로 조각 하나를 그대로 노출한다
 *
 * @queue: 대상 큐
 * @req:   요청. 데이터가 물리적으로 연속이라 조각이 하나뿐인 경우.
 * @c:     보낼 명령. dptr.ksgl 에 주소·길이·키를 싣는다.
 * @return: 항상 0
 *
 * register_always 를 끈 구성에서만 쓰이는 지름길이다. 그 모드에서는 PD 를
 * UNSAFE_GLOBAL_RKEY 로 만들어 두었기 때문에, 별도 등록 없이 그 전역 키로
 * 어떤 물리 주소든 원격이 접근할 수 있다. MR 등록 왕복이 사라져 작은 I/O 가 빨라진다.
 *
 * 대가는 파일 상단 register_always 주석이 밝힌 그대로다 -- 물리 메모리 전체를
 * 읽고 쓸 수 있는 키가 상대에게 건네지므로 근본적으로 안전하지 않다. 그래서
 * 기본값은 켜짐이고, 이 경로는 성능을 위해 안전을 포기한 선택지로만 남아 있다.
 *
 * 조각이 하나여야 하는 이유: 키 있는 SGL 서술자 하나는 연속된 한 구간만
 * 가리킬 수 있다. 흩어져 있으면 MR 을 등록해 하나로 묶어야 한다.
 *
 * 호출 체인:
 *   nvme_rdma_map_data → [이 함수]
 */
static int nvme_rdma_map_sg_single(struct nvme_rdma_queue *queue,
		struct nvme_rdma_request *req, struct nvme_command *c)
{
	struct nvme_keyed_sgl_desc *sg = &c->common.dptr.ksgl;	/* [한국어] 키 있는 SGL — 원격이 이 키로 접근한다 */

	sg->addr = cpu_to_le64(sg_dma_address(req->data_sgl.sg_table.sgl));	/* [한국어] 유일한 조각의 DMA 주소를 그대로 노출 */
	put_unaligned_le24(sg_dma_len(req->data_sgl.sg_table.sgl), sg->length);	/* [한국어] 길이 필드가 24비트라 정렬되지 않은 쓰기 헬퍼를 쓴다 */
	put_unaligned_le32(queue->device->pd->unsafe_global_rkey, sg->key);	/* [한국어] 전역 rkey — 등록 없이 쓰는 대신 물리 메모리 전체가 노출된다 */
	sg->type = NVME_KEY_SGL_FMT_DATA_DESC << 4;	/* [한국어] INVALIDATE 비트가 없다 — 등록한 것이 아니므로 무효화할 것도 없다 */
	return 0;
}

/*
 * [한국어]
 * nvme_rdma_map_sg_fr - 요청 데이터를 MR 하나로 등록해 원격에 노출한다
 *
 * @queue: 대상 큐
 * @req:   요청. req->mr 과 req->reg_wr 을 이 함수가 채운다.
 * @c:     보낼 명령. dptr.ksgl 에 iova·길이·rkey 를 싣는다.
 * @count: 매핑된 조각 수
 * @return: 0 이면 성공. -EAGAIN 이면 풀이 비어 재시도, -EINVAL 이면 조각을
 *          MR 하나로 담을 수 없는 경우.
 *
 * 일반적인 경로다. 흩어진 여러 조각을 MR 하나로 묶어 연속된 가상 구간(iova)처럼
 * 보이게 만들고, 그 rkey 를 명령에 실어 보낸다. 그러면 원격은 조각의 존재를
 * 모른 채 하나의 구간을 RDMA Read/Write 로 접근한다.
 *
 * 등록도 WR 이라는 점이 중요하다. 여기서 만든 reg_wr 은 호출자가 명령 전송 WR
 * 앞에 체인으로 묶어 한 번에 게시한다. 그래서 등록을 위한 별도 왕복이 없다.
 *
 * rkey 를 매번 증가시키는 이유(ib_inc_rkey): MR 은 풀에서 재사용되므로, 이전
 * 요청이 쓰던 키가 그대로면 늦게 도착한 원격 접근이 새 요청의 메모리를 건드릴
 * 수 있다. 키를 바꿔 과거의 키를 무효로 만든다.
 *
 * type 에 INVALIDATE 비트를 세우는 이유: 타겟이 데이터 전송을 마치면서 이 MR 을
 * 무효화해 달라는 요청이다. 그러면 호스트가 별도 무효화 WR 을 보내지 않아도 되고,
 * 그 완료가 req->ref 를 내려 요청이 끝난다.
 *
 * 호출 체인:
 *   nvme_rdma_map_data → [이 함수] → ib_map_mr_sg
 */
static int nvme_rdma_map_sg_fr(struct nvme_rdma_queue *queue,
		struct nvme_rdma_request *req, struct nvme_command *c,
		int count)
{
	struct nvme_keyed_sgl_desc *sg = &c->common.dptr.ksgl;	/* [한국어] 원격이 rkey 로 접근할 서술자 */
	int nr;

	req->mr = ib_mr_pool_get(queue->qp, &queue->qp->rdma_mrs);	/* [한국어] 미리 만들어 둔 풀에서 MR 하나를 꺼낸다 */
	if (WARN_ON_ONCE(!req->mr))	/* [한국어] 풀 크기를 큐 깊이에 맞췄으므로 비는 것은 설계상 있을 수 없다 */
		return -EAGAIN;	/* [한국어] 그래도 비었으면 blk-mq 에 재큐잉을 맡긴다 */

	/*
	 * Align the MR to a 4K page size to match the ctrl page size and
	 * the block virtual boundary.
	 */
	nr = ib_map_mr_sg(req->mr, req->data_sgl.sg_table.sgl, count, NULL,	/* [한국어] 흩어진 조각들을 MR 하나의 연속 구간으로 묶는다 */
			  SZ_4K);	/* [한국어] 위 영어 주석대로 컨트롤러 페이지 크기·블록 가상 경계와 맞추기 위해 4K 정렬 */
	if (unlikely(nr < count)) {	/* [한국어] 조각을 다 담지 못했다 — 이 MR 로는 이 요청을 표현할 수 없다 */
		ib_mr_pool_put(queue->qp, &queue->qp->rdma_mrs, req->mr);	/* [한국어] 쓰지 못한 MR 을 즉시 반납 */
		req->mr = NULL;	/* [한국어] 해제 경로가 이미 반납한 MR 을 또 놓지 않도록 지운다 */
		if (nr < 0)
			return nr;	/* [한국어] 음수면 ib 계층이 준 오류를 그대로 올린다 */
		return -EINVAL;	/* [한국어] 담긴 개수가 모자란 경우 — 세그먼트 제약이 어긋난 것이라 재시도해도 같다 */
	}

	ib_update_fast_reg_key(req->mr, ib_inc_rkey(req->mr->rkey));	/* [한국어] 키를 증가시켜 이 MR 의 이전 사용분을 무효로 만든다 — 지연 도착한 원격 접근 차단 */

	req->reg_cqe.done = nvme_rdma_memreg_done;	/* [한국어] 등록 완료 콜백 */
	memset(&req->reg_wr, 0, sizeof(req->reg_wr));	/* [한국어] 재사용되는 요청이므로 이전 값이 남지 않게 지운다 */
	req->reg_wr.wr.opcode = IB_WR_REG_MR;	/* [한국어] 이 WR 은 "MR 을 등록하라"는 명령이다 */
	req->reg_wr.wr.wr_cqe = &req->reg_cqe;
	req->reg_wr.wr.num_sge = 0;	/* [한국어] 등록 WR 은 옮길 데이터가 없어 SGE 가 필요 없다 */
	req->reg_wr.mr = req->mr;
	req->reg_wr.key = req->mr->rkey;
	req->reg_wr.access = IB_ACCESS_LOCAL_WRITE |	/* [한국어] 읽기 명령이면 HCA 가 이 메모리에 써 넣어야 한다 */
			     IB_ACCESS_REMOTE_READ |	/* [한국어] 쓰기 명령이면 원격이 읽어 간다 */
			     IB_ACCESS_REMOTE_WRITE;	/* [한국어] 읽기 명령이면 원격이 써 넣는다. 방향을 나누지 않고 둘 다 허용한다 */

	sg->addr = cpu_to_le64(req->mr->iova);	/* [한국어] MR 이 만들어 낸 연속 가상 주소 — 원격은 조각의 존재를 모른다 */
	put_unaligned_le24(req->mr->length, sg->length);
	put_unaligned_le32(req->mr->rkey, sg->key);	/* [한국어] 방금 갱신한 키를 실어 보낸다 */
	sg->type = (NVME_KEY_SGL_FMT_DATA_DESC << 4) |
			NVME_SGL_FMT_INVALIDATE;	/* [한국어] 타겟이 전송을 마치며 이 MR 을 무효화해 달라는 표시 — 호스트가 별도 무효화 WR 을 보내지 않아도 된다 */

	return 0;
}

static void nvme_rdma_set_sig_domain(struct blk_integrity *bi,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		struct nvme_command *cmd, struct ib_sig_domain *domain,
		u16 control, u8 pi_type)
{
	domain->sig_type = IB_SIG_TYPE_T10_DIF;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	domain->sig.dif.bg_type = IB_T10DIF_CRC;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	domain->sig.dif.pi_interval = 1 << bi->interval_exp;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	domain->sig.dif.ref_tag = le32_to_cpu(cmd->rw.reftag);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (control & NVME_RW_PRINFO_PRCHK_REF)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		domain->sig.dif.ref_remap = true;

	domain->sig.dif.app_tag = le16_to_cpu(cmd->rw.lbat);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	domain->sig.dif.apptag_check_mask = le16_to_cpu(cmd->rw.lbatm);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	domain->sig.dif.app_escape = true;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (pi_type == NVME_NS_DPS_PI_TYPE3)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		domain->sig.dif.ref_escape = true;
}

static void nvme_rdma_set_sig_attrs(struct blk_integrity *bi,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		struct nvme_command *cmd, struct ib_sig_attrs *sig_attrs,
		u8 pi_type)
{
	u16 control = le16_to_cpu(cmd->rw.control);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	memset(sig_attrs, 0, sizeof(*sig_attrs));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (control & NVME_RW_PRINFO_PRACT) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		/* for WRITE_INSERT/READ_STRIP no memory domain */
		sig_attrs->mem.sig_type = IB_SIG_TYPE_NONE;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_rdma_set_sig_domain(bi, cmd, &sig_attrs->wire, control,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
					 pi_type);
		/* Clear the PRACT bit since HCA will generate/verify the PI */
		control &= ~NVME_RW_PRINFO_PRACT;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		cmd->rw.control = cpu_to_le16(control);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	} else {
		/* for WRITE_PASS/READ_PASS both wire/memory domains exist */
		nvme_rdma_set_sig_domain(bi, cmd, &sig_attrs->wire, control,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
					 pi_type);
		nvme_rdma_set_sig_domain(bi, cmd, &sig_attrs->mem, control,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
					 pi_type);
	}
}

static void nvme_rdma_set_prot_checks(struct nvme_command *cmd, u8 *mask)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{
	*mask = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (le16_to_cpu(cmd->rw.control) & NVME_RW_PRINFO_PRCHK_REF)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		*mask |= IB_SIG_CHECK_REFTAG;
	if (le16_to_cpu(cmd->rw.control) & NVME_RW_PRINFO_PRCHK_GUARD)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		*mask |= IB_SIG_CHECK_GUARD;
}

static void nvme_rdma_sig_done(struct ib_cq *cq, struct ib_wc *wc)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{
	if (unlikely(wc->status != IB_WC_SUCCESS))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_rdma_wr_error(cq, wc, "SIG");
}

static int nvme_rdma_map_sg_pi(struct nvme_rdma_queue *queue,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		struct nvme_rdma_request *req, struct nvme_command *c,
		int count, int pi_count)
{
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
			     req->metadata_sgl->sg_table.sgl, pi_count, NULL,
			     SZ_4K);
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
		     IB_ACCESS_REMOTE_READ |
		     IB_ACCESS_REMOTE_WRITE;

	sg->addr = cpu_to_le64(req->mr->iova);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	xfer_len = req->mr->length;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	/* Check if PI is added by the HW */
	if (!pi_count)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		xfer_len += (xfer_len >> bi->interval_exp) * ns->head->pi_size;
	put_unaligned_le24(xfer_len, sg->length);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	put_unaligned_le32(req->mr->rkey, sg->key);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	sg->type = NVME_KEY_SGL_FMT_DATA_DESC << 4;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

mr_put:
	ib_mr_pool_put(queue->qp, &queue->qp->sig_mrs, req->mr);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
	req->mr = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (nr < 0)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return nr;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	return -EINVAL;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static int nvme_rdma_dma_map_req(struct ib_device *ibdev, struct request *rq,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		int *count, int *pi_count)
{
	struct nvme_rdma_request *req = blk_mq_rq_to_pdu(rq);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	req->data_sgl.sg_table.sgl = (struct scatterlist *)(req + 1);	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	ret = sg_alloc_table_chained(&req->data_sgl.sg_table,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			blk_rq_nr_phys_segments(rq), req->data_sgl.sg_table.sgl,
			NVME_INLINE_SG_CNT);
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return -ENOMEM;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	req->data_sgl.nents = blk_rq_map_sg(rq, req->data_sgl.sg_table.sgl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	*count = ib_dma_map_sg(ibdev, req->data_sgl.sg_table.sgl,	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
			       req->data_sgl.nents, rq_dma_dir(rq));
	if (unlikely(*count <= 0)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = -EIO;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_free_table;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}

	if (blk_integrity_rq(rq)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		req->metadata_sgl->sg_table.sgl =	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			(struct scatterlist *)(req->metadata_sgl + 1);	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
		ret = sg_alloc_table_chained(&req->metadata_sgl->sg_table,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				rq->nr_integrity_segments,
				req->metadata_sgl->sg_table.sgl,
				NVME_INLINE_METADATA_SG_CNT);
		if (unlikely(ret)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			ret = -ENOMEM;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			goto out_unmap_sg;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
		}

		req->metadata_sgl->nents = blk_rq_map_integrity_sg(rq,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				req->metadata_sgl->sg_table.sgl);
		*pi_count = ib_dma_map_sg(ibdev,	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
					  req->metadata_sgl->sg_table.sgl,
					  req->metadata_sgl->nents,
					  rq_dma_dir(rq));
		if (unlikely(*pi_count <= 0)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			ret = -EIO;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			goto out_free_pi_table;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
		}
	}

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

out_free_pi_table:
	sg_free_table_chained(&req->metadata_sgl->sg_table,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			      NVME_INLINE_METADATA_SG_CNT);
out_unmap_sg:
	ib_dma_unmap_sg(ibdev, req->data_sgl.sg_table.sgl, req->data_sgl.nents,	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
			rq_dma_dir(rq));
out_free_table:
	sg_free_table_chained(&req->data_sgl.sg_table, NVME_INLINE_SG_CNT);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

/*
 * [한국어]
 * nvme_rdma_map_data - 요청 데이터를 어떻게 건넬지 정하고 명령에 서술자를 채운다
 *
 * @queue: 대상 큐
 * @rq:    블록 계층이 넘긴 요청
 * @c:     채울 명령. 이 함수가 c->common.dptr 에 데이터 서술자를 쓴다.
 * @return: 0 이면 성공. 음수면 매핑 실패이며 잡은 DMA 는 되돌린다.
 *
 * 이 파일에서 가장 중요한 판단 지점이다. 같은 요청이라도 크기와 조각 모양,
 * 방향, 장치 설정에 따라 네 갈래 중 하나로 나간다. 성능 차이가 여기서 난다:
 *
 *   1) 데이터 없음      → set_sg_null. Flush 같은 명령.
 *   2) 인라인           → map_sg_inline. MR 등록도 원격 왕복도 없이 캡슐에 실어
 *                         한 번에 보낸다. 가장 빠르지만 조건이 가장 까다롭다.
 *   3) 전역 rkey 단일   → map_sg_single. 등록을 건너뛰지만 물리 메모리 전체가
 *                         노출되는 unsafe 모드에서만 가능하다.
 *   4) MR 등록          → map_sg_fr. 일반적인 경로. 흩어진 조각을 하나로 묶는다.
 *
 * 인라인 조건이 넷인 이유를 하나씩 보면:
 *   - WRITE 여야 한다. 읽기는 데이터가 돌아오는 방향이라 미리 실을 것이 없다.
 *   - queue_idx 가 0 이 아니어야 한다. admin 큐는 캡슐 여유가 없어
 *     inline_data_size 가 0 이다.
 *   - 컨트롤러가 인라인을 협상했어야 한다.
 *   - 페이로드가 캡슐 여유 공간 안에 들어가야 한다.
 *
 * refcount 를 2 로 세우는 것에 주의: 송신 완료와 수신 완료가 각각 독립적으로
 * 도착하고, 둘 다 끝나야 요청을 완료시킬 수 있다. MR 을 쓰는 경로는 무효화
 * 완료가 더해져 호출자 쪽에서 하나 더 올린다.
 *
 * 실행 컨텍스트: blk-mq 제출 경로. 잠들 수 없다.
 *
 * 호출 체인:
 *   nvme_rdma_queue_rq → [이 함수] → map_sg_inline / _single / _fr / _pi
 */
static int nvme_rdma_map_data(struct nvme_rdma_queue *queue,
		struct request *rq, struct nvme_command *c)
{
	struct nvme_rdma_request *req = blk_mq_rq_to_pdu(rq);	/* [한국어] 태그에 붙어 있는 이 요청의 RDMA 상태 */
	struct nvme_rdma_device *dev = queue->device;
	struct ib_device *ibdev = dev->dev;
	int pi_count = 0;	/* [한국어] 보호 정보 조각 수 — PI 를 쓰지 않으면 0 으로 남는다 */
	int count, ret;

	req->num_sge = 1;	/* [한국어] 0번 칸은 명령 캡슐이 차지한다. 인라인이면 아래에서 더 늘어난다 */
	refcount_set(&req->ref, 2); /* send and recv completions */
	/* [한국어] 위 영어 주석대로 송신 완료와 수신 완료 둘을 기다린다. 둘 다 와야 요청이 끝난다 */

	c->common.flags |= NVME_CMD_SGL_METABUF;	/* [한국어] 이 명령은 PRP 가 아니라 SGL 로 데이터를 기술한다고 알린다. Fabrics 는 늘 SGL 이다 */

	if (!blk_rq_nr_phys_segments(rq))	/* [한국어] 옮길 데이터가 없는 명령(Flush 등) */
		return nvme_rdma_set_sg_null(c);	/* [한국어] 길이 0 서술자를 넣고 끝낸다 — DMA 매핑도 필요 없다 */

	ret = nvme_rdma_dma_map_req(ibdev, rq, &count, &pi_count);	/* [한국어] scatterlist 를 만들고 DMA 매핑 — count 가 매핑 후 조각 수다 */
	if (unlikely(ret))
		return ret;

	if (req->use_sig_mr) {	/* [한국어] 보호 정보를 HCA 에 맡기는 요청이면 전용 경로로 간다 */
		ret = nvme_rdma_map_sg_pi(queue, req, c, count, pi_count);	/* [한국어] signature MR — 등록과 동시에 PI 생성·검증이 이뤄진다 */
		goto out;
	}

	if (count <= dev->num_inline_segments) {	/* [한국어] 조각 수가 장치의 인라인 SGE 한도 안이면 두 지름길을 검토한다 */
		if (rq_data_dir(rq) == WRITE && nvme_rdma_queue_idx(queue) &&	/* [한국어] 쓰기여야 하고(읽기는 실을 데이터가 없다), admin 큐가 아니어야 한다 */
		    queue->ctrl->use_inline_data &&	/* [한국어] 연결 협상에서 인라인이 허용됐어야 한다 */
		    blk_rq_payload_bytes(rq) <=	/* [한국어] 그리고 페이로드가 */
				nvme_rdma_inline_data_size(queue)) {	/* [한국어] 캡슐 여유 공간에 들어가야 한다. admin 은 이 값이 0 이라 위 idx 검사와 중복 방어가 된다 */
			ret = nvme_rdma_map_sg_inline(queue, req, c, count);	/* [한국어] 가장 빠른 길 — 명령과 데이터가 한 번에 건너간다 */
			goto out;
		}

		if (count == 1 && dev->pd->flags & IB_PD_UNSAFE_GLOBAL_RKEY) {	/* [한국어] 조각이 하나이고 unsafe 전역 rkey 모드라면 */
			ret = nvme_rdma_map_sg_single(queue, req, c);	/* [한국어] 등록을 건너뛴다 — 대가는 물리 메모리 전체 노출이다 */
			goto out;
		}
	}

	ret = nvme_rdma_map_sg_fr(queue, req, c, count);	/* [한국어] 일반 경로 — 조각들을 MR 하나로 묶어 rkey 를 넘긴다 */
out:
	if (unlikely(ret))
		goto out_dma_unmap_req;

	return 0;

out_dma_unmap_req:	/* [한국어] 서술자 구성에 실패했으면 위에서 잡은 DMA 매핑을 되돌려야 한다 */
	nvme_rdma_dma_unmap_req(ibdev, rq);
	return ret;
}

/*
 * [한국어]
 * nvme_rdma_send_done - 명령 캡슐 송신이 끝났을 때 불린다
 *
 * @cq: 완료가 올라온 완료 큐
 * @wc: 완료 항목. status 가 성공 여부를 담는다.
 * @return: 없음
 *
 * 송신 완료는 "하드웨어가 캡슐을 다 읽어 보냈다"는 뜻이지 "타겟이 처리했다"는
 * 뜻이 아니다. 그래서 여기서 요청을 완료시키지 않고 참조만 하나 내린다.
 * 실제 완료는 응답 수신(recv_done)까지 와서 참조가 0 이 될 때 일어난다.
 *
 * container_of 를 두 번 타는 이유: 완료 항목은 ib_cqe 포인터만 실어 주므로,
 * 먼저 그것을 감싼 qe 를 찾고, 다시 그 qe 를 품은 요청으로 올라가야 한다.
 * 송신 버퍼가 요청 안에 값으로 박혀 있어 가능한 되짚기다.
 *
 * 실행 컨텍스트: CQ 폴링 문맥(소프트IRQ 또는 blk-mq poll). 잠들 수 없다.
 *
 * 호출 체인:
 *   ib_poll_cq → [이 함수] → nvme_rdma_end_request
 */
static void nvme_rdma_send_done(struct ib_cq *cq, struct ib_wc *wc)
{
	struct nvme_rdma_qe *qe =	/* [한국어] 완료가 가리키는 ib_cqe 에서 그것을 품은 버퍼 객체로 */
		container_of(wc->wr_cqe, struct nvme_rdma_qe, cqe);
	struct nvme_rdma_request *req =	/* [한국어] 다시 그 버퍼를 품은 요청으로 — sqe 는 요청 안에 값으로 있다 */
		container_of(qe, struct nvme_rdma_request, sqe);

	if (unlikely(wc->status != IB_WC_SUCCESS))	/* [한국어] 송신 자체가 실패했다면 링크가 깨진 것이다 */
		nvme_rdma_wr_error(cq, wc, "SEND");	/* [한국어] 로그를 남기고 오류 복구를 촉발한다 — 여기서 요청을 개별 처리하지 않는다 */
	else
		nvme_rdma_end_request(req);	/* [한국어] 참조를 하나 내린다. 응답까지 와서 0 이 되어야 실제로 완료된다 */
}

/*
 * [한국어]
 * nvme_rdma_post_send - 명령 캡슐을 송신 큐에 올린다
 *
 * @queue:   대상 큐
 * @qe:      보낼 캡슐이 담긴 버퍼
 * @sge:     이 함수가 채울 첫 SGE(캡슐 자리). 인라인이면 뒤에 데이터 칸이 이어진다.
 * @num_sge: 총 SGE 개수. 캡슐만이면 1, 인라인이면 1 + 데이터 조각 수.
 * @first:   앞에 체인으로 붙일 WR. MR 등록 WR 이 여기로 들어온다. 없으면 NULL.
 * @return:  0 이면 게시 성공. 음수면 송신 큐가 가득 찼거나 QP 가 오류 상태다.
 *
 * first 인자가 이 함수의 핵심이다. MR 을 쓰는 요청은 등록 WR 과 전송 WR 을
 * 체인으로 묶어 한 번에 게시한다. 그래야 등록을 기다렸다 보내는 왕복이 없어지고,
 * 하드웨어가 순서를 보장해 등록이 끝난 뒤 전송이 나간다.
 *
 * IB_SEND_SIGNALED 를 세우는 이유: QP 를 IB_SIGNAL_REQ_WR 로 만들었기 때문에
 * 요청한 WR 만 완료가 올라온다. 송신 완료는 참조 계수를 내리는 데 필요하므로
 * 반드시 신호를 요구해야 한다.
 *
 * 실행 컨텍스트: blk-mq 제출 경로. 잠들 수 없다.
 *
 * 호출 체인:
 *   nvme_rdma_queue_rq → [이 함수] → ib_post_send
 */
static int nvme_rdma_post_send(struct nvme_rdma_queue *queue,
		struct nvme_rdma_qe *qe, struct ib_sge *sge, u32 num_sge,
		struct ib_send_wr *first)
{
	struct ib_send_wr wr;	/* [한국어] 스택 위에 만든다 — ib_post_send 가 게시하며 내용을 복사해 가므로 반환 후 사라져도 된다 */
	int ret;

	sge->addr   = qe->dma;	/* [한국어] 캡슐 버퍼의 DMA 주소 */
	sge->length = sizeof(struct nvme_command);	/* [한국어] 캡슐의 명령 부분은 늘 64바이트다 */
	sge->lkey   = queue->device->pd->local_dma_lkey;	/* [한국어] 로컬 키 — 우리 HCA 가 읽어 보낼 뿐이다 */

	wr.next       = NULL;	/* [한국어] 체인의 끝. 앞에 등록 WR 이 붙으면 그쪽이 이것을 가리킨다 */
	wr.wr_cqe     = &qe->cqe;	/* [한국어] 완료 시 send_done 으로 되돌아올 실마리 */
	wr.sg_list    = sge;
	wr.num_sge    = num_sge;	/* [한국어] 인라인이면 캡슐 뒤로 데이터 조각들이 함께 나간다 */
	wr.opcode     = IB_WR_SEND;	/* [한국어] 상대의 미리 게시된 수신 버퍼로 보내는 일반 SEND */
	wr.send_flags = IB_SEND_SIGNALED;	/* [한국어] 완료를 반드시 올려 달라 — 참조 계수를 내리려면 이 완료가 필요하다 */

	if (first)	/* [한국어] MR 등록 WR 이 앞에 있으면 */
		first->next = &wr;	/* [한국어] 그 뒤에 전송을 잇는다. 하드웨어가 등록 → 전송 순서를 지킨다 */
	else
		first = &wr;	/* [한국어] 없으면 전송이 곧 체인의 시작이다 */

	ret = ib_post_send(queue->qp, first, NULL);	/* [한국어] 체인 전체를 한 번에 송신 큐에 올린다 */
	if (unlikely(ret)) {
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 큐가 가득 찼거나 QP 가 오류 상태 — 크기 계산이 맞으면 나오지 않아야 한다 */
			     "%s failed with error code %d\n", __func__, ret);
	}
	return ret;
}

/*
 * [한국어]
 * nvme_rdma_post_recv - 응답 캡슐을 받을 버퍼를 수신 큐에 미리 올린다
 *
 * @queue: 대상 큐
 * @qe:    수신 링에서 꺼낸 버퍼
 * @return: 0 이면 게시 성공
 *
 * RDMA 는 상대가 보내기 전에 수신 버퍼가 이미 올라와 있어야 한다. 응답이
 * 도착한 뒤 버퍼를 준비하는 것은 늦다 -- 그 사이에 온 패킷은 버려지고 링크가
 * 오류 상태로 간다. 그래서 큐를 세울 때 깊이만큼 미리 게시하고, 완료를 처리할
 * 때마다 같은 버퍼를 즉시 다시 올린다.
 *
 * 실행 컨텍스트: 큐 초기화 시점과 CQ 폴링 문맥 양쪽에서 불린다.
 *
 * 호출 체인:
 *   nvme_rdma_start_queue / nvme_rdma_recv_done → [이 함수] → ib_post_recv
 */
static int nvme_rdma_post_recv(struct nvme_rdma_queue *queue,
		struct nvme_rdma_qe *qe)
{
	struct ib_recv_wr wr;	/* [한국어] 게시하며 복사되므로 스택에 두어도 된다 */
	struct ib_sge list;
	int ret;

	list.addr   = qe->dma;	/* [한국어] 응답이 써 넣을 버퍼의 DMA 주소 */
	list.length = sizeof(struct nvme_completion);	/* [한국어] 응답 캡슐 하나 크기 */
	list.lkey   = queue->device->pd->local_dma_lkey;	/* [한국어] 로컬 키 — 우리 HCA 가 이 버퍼에 써 넣는다 */

	qe->cqe.done = nvme_rdma_recv_done;	/* [한국어] 도착 시 불릴 콜백. 송신용 버퍼와 달리 수신은 매번 여기서 건다 */

	wr.next     = NULL;
	wr.wr_cqe   = &qe->cqe;	/* [한국어] 완료에서 이 버퍼를 되찾을 실마리 */
	wr.sg_list  = &list;
	wr.num_sge  = 1;	/* [한국어] 응답 캡슐은 연속된 버퍼 하나다 */

	ret = ib_post_recv(queue->qp, &wr, NULL);	/* [한국어] 수신 큐에 올린다 — 이제 상대가 보낼 수 있다 */
	if (unlikely(ret)) {
		dev_err(queue->ctrl->ctrl.device,
			"%s failed with error code %d\n", __func__, ret);
	}
	return ret;
}

/*
 * [한국어]
 * nvme_rdma_tagset - 이 큐의 요청들이 속한 blk-mq 태그 집합을 돌려준다
 *
 * @queue: 대상 큐
 * @return: admin 큐면 admin 태그셋, I/O 큐면 해당 hctx 의 태그셋
 *
 * 왜 나뉘어 있나: admin 과 I/O 는 태그셋이 별개다. 그래야 I/O 큐가 모두
 * 막힌 상태에서도 리셋·삭제 같은 admin 명령이 태그를 얻어 통과할 수 있다.
 * 인덱스가 1 밀리는 것은 큐 배열에서는 0번이 admin 이지만 I/O 태그셋의
 * hctx 배열에서는 첫 I/O 큐가 0번이기 때문이다.
 *
 * 호출 체인:
 *   nvme_rdma_recv_done → [이 함수] → blk_mq_tag_to_rq (응답의 command_id 로 요청 복원)
 */
static struct blk_mq_tags *nvme_rdma_tagset(struct nvme_rdma_queue *queue)
{
	u32 queue_idx = nvme_rdma_queue_idx(queue);

	if (queue_idx == 0)	/* [한국어] 0번은 admin 큐 */
		return queue->ctrl->admin_tag_set.tags[queue_idx];
	return queue->ctrl->tag_set.tags[queue_idx - 1];	/* [한국어] I/O 큐 — admin 몫을 빼고 0부터 다시 센다 */
}

/*
 * [한국어]
 * nvme_rdma_async_done - 비동기 이벤트(AEN) 명령의 송신 완료
 *
 * @cq: 완료 큐
 * @wc: 완료 항목
 * @return: 없음
 *
 * AEN 은 태그를 소비하지 않는 상주 명령이라 blk-mq 요청이 없다. 그래서
 * 완료시킬 요청도, 내릴 참조도 없고 오류만 확인하면 된다. 실제 이벤트 도착은
 * 응답 수신 경로(recv_done)가 처리한다.
 */
static void nvme_rdma_async_done(struct ib_cq *cq, struct ib_wc *wc)
{
	if (unlikely(wc->status != IB_WC_SUCCESS))	/* [한국어] 송신 실패면 링크 문제이므로 복구를 촉발한다 */
		nvme_rdma_wr_error(cq, wc, "ASYNC");
}

/*
 * [한국어]
 * nvme_rdma_submit_async_event - 비동기 이벤트 명령을 걸어 둔다
 *
 * @arg: 코어가 넘긴 nvme_ctrl
 * @return: 없음
 *
 * AEN 은 완료를 기다리는 명령이 아니라 통지를 예약하는 명령이다. 컨트롤러가
 * 알릴 것이 생길 때까지 미완료로 남으므로, blk-mq 태그를 붙들면 I/O 에 쓸
 * 태그가 영구히 하나 줄어든다. 그래서 태그셋 밖의 전용 버퍼(async_event_sqe)
 * 하나를 컨트롤러가 들고 있고, 이 함수가 그것을 채워 보낸다.
 *
 * command_id 가 NVME_AQ_BLK_MQ_DEPTH 인 것이 그 분리의 표현이다. admin 태그
 * 범위 바로 위라 실제 태그와 겹치지 않고, 응답이 왔을 때 recv_done 이
 * nvme_is_aen_req 로 이 값을 알아보고 코어의 AEN 경로로 넘긴다.
 *
 * DMA sync 가 앞뒤로 한 쌍인 이유: 이 버퍼는 컨트롤러 수명 내내 재사용된다.
 * 앞의 for_cpu 로 이전 사용분의 캐시 상태를 정리한 뒤 새 명령을 채우고,
 * 뒤의 for_device 로 HCA 가 읽을 수 있게 되돌린다.
 *
 * set_sg_null 은 데이터가 없는 명령임을 서술자에 표시한다. AEN 은 통지만
 * 받으므로 옮길 페이로드가 없다.
 *
 * 실패를 WARN 으로만 잡는 것은 되돌릴 것이 없기 때문이다. 이벤트를 못 받게
 * 될 뿐 다른 동작에는 영향이 없다.
 *
 * 실행 컨텍스트: 코어의 AEN 재무장 경로.
 *
 * 호출 체인:
 *   nvme_complete_async_event → ops->submit_async_event → [이 함수]
 *     → nvme_rdma_post_send
 */
static void nvme_rdma_submit_async_event(struct nvme_ctrl *arg)
{
	struct nvme_rdma_ctrl *ctrl = to_rdma_ctrl(arg);
	struct nvme_rdma_queue *queue = &ctrl->queues[0];	/* [한국어] AEN 은 admin 큐로만 나간다 */
	struct ib_device *dev = queue->device->dev;
	struct nvme_rdma_qe *sqe = &ctrl->async_event_sqe;	/* [한국어] 태그셋 밖의 전용 버퍼 — 컨트롤러당 하나면 충분하다 */
	struct nvme_command *cmd = sqe->data;
	struct ib_sge sge;
	int ret;

	ib_dma_sync_single_for_cpu(dev, sqe->dma, sizeof(*cmd), DMA_TO_DEVICE);	/* [한국어] 재사용 버퍼라 CPU 가 새로 채우기 전에 캐시를 맞춘다 */

	memset(cmd, 0, sizeof(*cmd));	/* [한국어] 이전 사용분이 남지 않게 */
	cmd->common.opcode = nvme_admin_async_event;
	cmd->common.command_id = NVME_AQ_BLK_MQ_DEPTH;	/* [한국어] admin 태그 범위 바로 위 — recv_done 이 이 값으로 AEN 을 알아본다 */
	cmd->common.flags |= NVME_CMD_SGL_METABUF;	/* [한국어] Fabrics 는 늘 SGL 로 데이터를 기술한다 */
	nvme_rdma_set_sg_null(cmd);	/* [한국어] 옮길 데이터가 없다 — 통지만 기다리는 명령이다 */

	sqe->cqe.done = nvme_rdma_async_done;	/* [한국어] 송신 완료 시 오류만 확인하는 콜백 */

	ib_dma_sync_single_for_device(dev, sqe->dma, sizeof(*cmd),	/* [한국어] HCA 가 읽을 수 있게 되돌린다 */
			DMA_TO_DEVICE);

	ret = nvme_rdma_post_send(queue, sqe, &sge, 1, NULL);	/* [한국어] 캡슐만 보낸다(SGE 하나), 앞에 붙일 등록 WR 도 없다 */
	WARN_ON_ONCE(ret);	/* [한국어] 되돌릴 것이 없다 — 이벤트를 못 받을 뿐이다 */
}

static void nvme_rdma_process_nvme_rsp(struct nvme_rdma_queue *queue,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		struct nvme_completion *cqe, struct ib_wc *wc)
{
	struct request *rq;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct nvme_rdma_request *req;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	rq = nvme_find_rq(nvme_rdma_tagset(queue), cqe->command_id);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	if (!rq) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"got bad command_id %#x on QP %#x\n",
			cqe->command_id, queue->qp->qp_num);
		nvme_rdma_error_recovery(queue->ctrl);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}
	req = blk_mq_rq_to_pdu(rq);	/* [한국어] blk-mq — 태그·hctx·타임아웃·맵·완료 연동 */

	req->status = cqe->status;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	req->result = cqe->result;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (wc->wc_flags & IB_WC_WITH_INVALIDATE) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		if (unlikely(!req->mr ||	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			     wc->ex.invalidate_rkey != req->mr->rkey)) {
			dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				"Bogus remote invalidation for rkey %#x\n",
				req->mr ? req->mr->rkey : 0);
			nvme_rdma_error_recovery(queue->ctrl);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		}
	} else if (req->mr) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		ret = nvme_rdma_inv_rkey(queue, req);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		if (unlikely(ret < 0)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				"Queueing INV WR for rkey %#x failed (%d)\n",
				req->mr->rkey, ret);
			nvme_rdma_error_recovery(queue->ctrl);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		}
		/* the local invalidation completion will end the request */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}

	nvme_rdma_end_request(req);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
}

/*
 * [한국어]
 * nvme_rdma_recv_done - 응답 캡슐이 도착했을 때 불린다
 *
 * @cq: 완료가 올라온 완료 큐
 * @wc: 완료 항목. byte_len 에 실제 수신 길이가 들어 있다.
 * @return: 없음
 *
 * RDMA 완료 경로의 종착점이다. 응답 캡슐을 해석해 요청을 완료시키고,
 * 마지막에 같은 버퍼를 수신 큐에 다시 올린다. 다시 올리지 않으면 수신
 * 버퍼가 하나씩 줄어들어 결국 상대가 보낼 곳이 없어진다.
 *
 * byte_len 검사가 필요한 이유: 이것은 원격이 보낸 길이다. 기대보다 짧으면
 * 아직 초기화되지 않은 버퍼 부분을 CQE 로 읽게 되므로, 해석하기 전에 막는다.
 *
 * DMA sync 가 앞뒤로 한 쌍인 것에 주목할 것. 앞의 for_cpu 는 HCA 가 써 넣은
 * 내용을 CPU 가 읽기 전에 캐시를 맞추고, 뒤의 for_device 는 같은 버퍼를 다시
 * 수신용으로 넘기기 전에 되돌린다. 버퍼를 재사용하기 때문에 두 방향이 모두 필요하다.
 *
 * AEN 을 특별 취급하는 이유는 위 영어 주석이 밝힌다 -- 타임아웃이 없고 큐
 * 정지를 견디며 abort 에도 응답하지 않는다. blk-mq 요청을 아예 할당하지
 * 않으므로 command_id 로 구분해 코어의 AEN 경로로 직접 넘긴다.
 *
 * 실행 컨텍스트: CQ 폴링(소프트IRQ 또는 blk-mq poll). 잠들 수 없다.
 *
 * 호출 체인:
 *   ib_poll_cq → [이 함수] → nvme_rdma_process_nvme_rsp → nvme_rdma_post_recv
 */
static void nvme_rdma_recv_done(struct ib_cq *cq, struct ib_wc *wc)
{
	struct nvme_rdma_qe *qe =
		container_of(wc->wr_cqe, struct nvme_rdma_qe, cqe);	/* [한국어] 완료가 실어 준 cqe 에서 수신 버퍼를 되찾는다 */
	struct nvme_rdma_queue *queue = wc->qp->qp_context;	/* [한국어] QP 생성 시 심어 둔 큐 포인터 */
	struct ib_device *ibdev = queue->device->dev;
	struct nvme_completion *cqe = qe->data;	/* [한국어] 버퍼 내용이 곧 NVMe 완료 항목이다 */
	const size_t len = sizeof(struct nvme_completion);

	if (unlikely(wc->status != IB_WC_SUCCESS)) {	/* [한국어] 수신 자체가 실패했다 — 링크 문제다 */
		nvme_rdma_wr_error(cq, wc, "RECV");
		return;
	}

	/* sanity checking for received data length */
	if (unlikely(wc->byte_len < len)) {	/* [한국어] 원격이 보낸 길이다 — 짧으면 초기화되지 않은 부분을 CQE 로 읽게 된다 */
		dev_err(queue->ctrl->ctrl.device,
			"Unexpected nvme completion length(%d)\n", wc->byte_len);
		nvme_rdma_error_recovery(queue->ctrl);	/* [한국어] 프로토콜이 깨진 것이므로 연결을 다시 세운다 */
		return;
	}

	ib_dma_sync_single_for_cpu(ibdev, qe->dma, len, DMA_FROM_DEVICE);	/* [한국어] HCA 가 써 넣은 내용을 CPU 가 읽기 전에 캐시를 맞춘다 */
	/*
	 * AEN requests are special as they don't time out and can
	 * survive any kind of queue freeze and often don't respond to
	 * aborts.  We don't even bother to allocate a struct request
	 * for them but rather special case them here.
	 */
	/* [한국어] 위 영어 주석대로 AEN 은 타임아웃도 없고 큐 정지를 견디며 abort 에도
	 * 응답하지 않는다. blk-mq 요청을 아예 만들지 않으므로 여기서 갈라 처리한다. */
	if (unlikely(nvme_is_aen_req(nvme_rdma_queue_idx(queue),
				     cqe->command_id)))	/* [한국어] command_id 가 태그 범위 위쪽이면 AEN 이다 */
		nvme_complete_async_event(&queue->ctrl->ctrl, cqe->status,
				&cqe->result);
	else
		nvme_rdma_process_nvme_rsp(queue, cqe, wc);	/* [한국어] 일반 요청 — 태그로 되찾아 완료시킨다 */
	ib_dma_sync_single_for_device(ibdev, qe->dma, len, DMA_FROM_DEVICE);	/* [한국어] 같은 버퍼를 다시 수신용으로 넘기기 전에 되돌린다 */

	nvme_rdma_post_recv(queue, qe);	/* [한국어] 반드시 다시 올린다 — 없으면 수신 버퍼가 줄어 상대가 보낼 곳이 사라진다 */
}

static int nvme_rdma_conn_established(struct nvme_rdma_queue *queue)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{
	int ret, i;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	for (i = 0; i < queue->queue_size; i++) {	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		ret = nvme_rdma_post_recv(queue, &queue->rsp_ring[i]);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static int nvme_rdma_conn_rejected(struct nvme_rdma_queue *queue,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		struct rdma_cm_event *ev)
{
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
		      "Connect rejected: status %d (%s) nvme status %d (%s).\n",
		      status, rej_msg, sts, nvme_rdma_cm_msg(sts));
	} else {
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"Connect rejected: status %d (%s).\n", status, rej_msg);
	}

	return -ECONNRESET;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

/*
 * [한국어]
 * nvme_rdma_addr_resolved - 주소 해석이 끝났으니 QP 를 만들고 경로를 찾는다
 *
 * @queue: 주소 해석이 완료된 큐
 * @return: 0 이면 라우팅 해석을 시작했다. 음수면 실패이며 되감았다.
 *
 * RDMA 연결은 세 단계를 거친다: 주소 해석 → 라우팅 해석 → 연결 수립.
 * 각 단계가 비동기 CM 이벤트로 끝나며, 이 함수는 첫 단계가 끝났을 때
 * 불려 다음 단계를 시작한다.
 *
 * QP 를 여기서 만드는 이유: 주소가 해석돼야 어느 HCA 를 쓸지 정해지고,
 * 그래야 그 장치의 PD 로 QP 를 만들 수 있다. 그전에는 만들 수 없다.
 *
 * ToS 를 설정하는 것은 라우팅 해석 직전이어야 한다. 경로를 찾을 때
 * 서비스 등급이 반영되어야 하기 때문이다. 옵션이 음수면(지정 안 함)
 * 건드리지 않는다.
 *
 * 실행 컨텍스트: CM 이벤트 핸들러. 잠들면 안 된다.
 *
 * 호출 체인:
 *   nvme_rdma_cm_handler(ADDR_RESOLVED) → [이 함수] → rdma_resolve_route
 *     → (ROUTE_RESOLVED 이벤트) → nvme_rdma_route_resolved
 */
static int nvme_rdma_addr_resolved(struct nvme_rdma_queue *queue)
{
	struct nvme_ctrl *ctrl = &queue->ctrl->ctrl;
	int ret;

	ret = nvme_rdma_create_queue_ib(queue);	/* [한국어] 주소가 풀려 HCA 가 정해졌으니 이제 QP·CQ·MR 풀을 만들 수 있다 */
	if (ret)
		return ret;

	if (ctrl->opts->tos >= 0)	/* [한국어] 사용자가 서비스 등급을 지정했다면 */
		rdma_set_service_type(queue->cm_id, ctrl->opts->tos);	/* [한국어] 경로 탐색 전에 설정해야 라우팅에 반영된다 */
	ret = rdma_resolve_route(queue->cm_id, NVME_RDMA_CM_TIMEOUT_MS);	/* [한국어] 두 번째 단계 시작 — 결과는 다시 CM 이벤트로 온다 */
	if (ret) {
		dev_err(ctrl->device, "rdma_resolve_route failed (%d).\n",
			queue->cm_error);
		goto out_destroy_queue;
	}

	return 0;

out_destroy_queue:
	nvme_rdma_destroy_queue_ib(queue);	/* [한국어] 방금 만든 자원을 되돌린다 */
	return ret;
}

/*
 * [한국어]
 * nvme_rdma_route_resolved - 경로가 정해졌으니 연결을 요청한다
 *
 * @queue: 라우팅 해석이 끝난 큐
 * @return: 0 이면 연결 요청을 보냈다. 음수면 실패.
 *
 * 세 단계 중 마지막이다. rdma_connect 에 실어 보내는 private_data 가
 * NVMe-oF 의 연결 파라미터이며, 타겟은 그것을 보고 큐를 준비한다.
 *
 * responder_resources 를 HCA 의 max_qp_rd_atom 으로 주는 이유: 상대가
 * 동시에 몇 개의 RDMA Read 를 우리에게 걸 수 있는지를 알리는 값이고,
 * 우리 HCA 가 감당할 수 있는 최대치가 곧 그 한도다.
 *
 * retry_count 와 rnr_retry_count 를 7(최대)로 두는 것은 링크 수준 재시도를
 * 최대한 허용한다는 뜻이다. 일시적인 수신 자원 부족(RNR)으로 연결이 끊기는
 * 것보다 하드웨어가 재시도하는 편이 낫다.
 *
 * admin 큐와 I/O 큐의 큐 크기 계산이 다른 것이 이 함수의 미묘한 부분이고,
 * 두 영어 주석이 각각 그 근거를 밝힌다. admin 은 Fabrics 규격이 정한 최소
 * 크기를 그대로 쓰고, I/O 는 hrqsize 를 sqsize+1 로 둔다 -- 규격 해석상
 * hrqsize 는 1-based 이고 hsqsize 는 0-based 이기 때문이다.
 *
 * cntlid 를 I/O 큐에서만 싣는 이유도 명시돼 있다. admin 연결 시점에는
 * 아직 컨트롤러 ID 를 받지 못했고, 그것을 받아 오는 것이 admin 연결의
 * 목적 중 하나다.
 *
 * 실행 컨텍스트: CM 이벤트 핸들러. rdma_connect_locked 는 CM 락을 이미
 * 들고 있는 문맥에서 부르는 변형이다.
 *
 * 호출 체인:
 *   nvme_rdma_cm_handler(ROUTE_RESOLVED) → [이 함수] → rdma_connect_locked
 *     → (ESTABLISHED 이벤트)
 */
static int nvme_rdma_route_resolved(struct nvme_rdma_queue *queue)
{
	struct nvme_rdma_ctrl *ctrl = queue->ctrl;
	struct rdma_conn_param param = { };	/* [한국어] RDMA 수준 연결 파라미터 */
	struct nvme_rdma_cm_req priv = { };	/* [한국어] 그 안에 실어 보낼 NVMe-oF 수준 파라미터 */
	int ret;

	param.qp_num = queue->qp->qp_num;
	param.flow_control = 1;

	param.responder_resources = queue->device->dev->attrs.max_qp_rd_atom;	/* [한국어] 상대가 우리에게 동시에 걸 수 있는 RDMA Read 수 — 우리 HCA 의 한도가 그 상한이다 */
	/* maximum retry count */
	param.retry_count = 7;	/* [한국어] 최대값. 일시적 오류로 연결이 끊기는 것보다 하드웨어가 재시도하는 편이 낫다 */
	param.rnr_retry_count = 7;	/* [한국어] 상대의 수신 자원이 잠시 부족할 때(RNR)도 마찬가지 */
	param.private_data = &priv;	/* [한국어] 타겟은 이 데이터를 보고 큐를 준비한다 */
	param.private_data_len = sizeof(priv);

	priv.recfmt = cpu_to_le16(NVME_RDMA_CM_FMT_1_0);	/* [한국어] 이 private_data 의 형식 버전 */
	priv.qid = cpu_to_le16(nvme_rdma_queue_idx(queue));	/* [한국어] 몇 번 큐를 여는지 */
	/*
	 * set the admin queue depth to the minimum size
	 * specified by the Fabrics standard.
	 */
	if (priv.qid == 0) {	/* [한국어] admin 큐 */
		priv.hrqsize = cpu_to_le16(NVME_AQ_DEPTH);	/* [한국어] 위 영어 주석대로 Fabrics 규격이 정한 최소 크기를 그대로 쓴다 */
		priv.hsqsize = cpu_to_le16(NVME_AQ_DEPTH - 1);	/* [한국어] hsqsize 는 0-based 라 하나 뺀다 */
	} else {
		/*
		 * current interpretation of the fabrics spec
		 * is at minimum you make hrqsize sqsize+1, or a
		 * 1's based representation of sqsize.
		 */
		/* [한국어] 위 영어 주석대로 규격 해석상 hrqsize 는 1-based,
		 * hsqsize 는 0-based 이므로 둘의 값이 하나 차이가 난다. */
		priv.hrqsize = cpu_to_le16(queue->queue_size);
		priv.hsqsize = cpu_to_le16(queue->ctrl->ctrl.sqsize);
		/* cntlid should only be set when creating an I/O queue */
		priv.cntlid = cpu_to_le16(ctrl->ctrl.cntlid);	/* [한국어] 위 영어 주석대로 I/O 큐에서만 싣는다 — admin 연결 시점에는 아직 이 ID 를 받지 못했다 */
	}

	ret = rdma_connect_locked(queue->cm_id, &param);	/* [한국어] _locked 변형 — CM 락을 이미 들고 있는 이벤트 핸들러 문맥이다 */
	if (ret) {
		dev_err(ctrl->ctrl.device,
			"rdma_connect_locked failed (%d).\n", ret);
		return ret;
	}

	return 0;
}

static int nvme_rdma_cm_handler(struct rdma_cm_id *cm_id,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		struct rdma_cm_event *ev)
{
	struct nvme_rdma_queue *queue = cm_id->context;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	int cm_error = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	dev_dbg(queue->ctrl->ctrl.device, "%s (%d): status %d id %p\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		rdma_event_msg(ev->event), ev->event,
		ev->status, cm_id);

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
			"CM error event %d\n", ev->event);
		cm_error = -ECONNRESET;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	case RDMA_CM_EVENT_DISCONNECTED:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	case RDMA_CM_EVENT_ADDR_CHANGE:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	case RDMA_CM_EVENT_TIMEWAIT_EXIT:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		dev_dbg(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"disconnect received - connection closed\n");
		nvme_rdma_error_recovery(queue->ctrl);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	case RDMA_CM_EVENT_DEVICE_REMOVAL:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		/* device removal is handled via the ib_client API */
		break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	default:
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"Unexpected RDMA CM event (%d)\n", ev->event);
		nvme_rdma_error_recovery(queue->ctrl);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}

	if (cm_error) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		queue->cm_error = cm_error;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		complete(&queue->cm_done);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static void nvme_rdma_complete_timed_out(struct request *rq)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{
	struct nvme_rdma_request *req = blk_mq_rq_to_pdu(rq);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct nvme_rdma_queue *queue = req->queue;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	nvme_rdma_stop_queue(queue);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	nvmf_complete_timed_out_request(rq);	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
}

static enum blk_eh_timer_return nvme_rdma_timeout(struct request *rq)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{
	struct nvme_rdma_request *req = blk_mq_rq_to_pdu(rq);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct nvme_rdma_queue *queue = req->queue;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct nvme_rdma_ctrl *ctrl = queue->ctrl;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct nvme_command *cmd = req->req.cmd;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	int qid = nvme_rdma_queue_idx(queue);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	dev_warn(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		 "I/O tag %d (%04x) opcode %#x (%s) QID %d timeout\n",
		 rq->tag, nvme_cid(rq), cmd->common.opcode,
		 nvme_fabrics_opcode_str(qid, cmd), qid);

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
	}

	/*
	 * LIVE state should trigger the normal error recovery which will
	 * handle completing this request.
	 */
	nvme_rdma_error_recovery(ctrl);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	return BLK_EH_RESET_TIMER;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static blk_status_t nvme_rdma_queue_rq(struct blk_mq_hw_ctx *hctx,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		const struct blk_mq_queue_data *bd)
{
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
		return nvme_fail_nonready_command(&queue->ctrl->ctrl, rq);

	dev = queue->device->dev;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	req->sqe.dma = ib_dma_map_single(dev, req->sqe.data,	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
					 sizeof(struct nvme_command),	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
					 DMA_TO_DEVICE);
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
	    queue->pi_support &&
	    (c->common.opcode == nvme_cmd_write ||
	     c->common.opcode == nvme_cmd_read) &&
	    nvme_ns_has_pi(ns->head))
		req->use_sig_mr = true;
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		req->use_sig_mr = false;

	err = nvme_rdma_map_data(queue, rq, c);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	if (unlikely(err < 0)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			     "Failed to map data (%d)\n", err);
		goto err;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}

	sqe->cqe.done = nvme_rdma_send_done;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	ib_dma_sync_single_for_device(dev, sqe->dma,	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
			sizeof(struct nvme_command), DMA_TO_DEVICE);	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	err = nvme_rdma_post_send(queue, sqe, req->sge, req->num_sge,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
			req->mr ? &req->reg_wr.wr : NULL);
	if (unlikely(err))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto err_unmap;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	return BLK_STS_OK;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

err_unmap:
	nvme_rdma_unmap_data(queue, rq);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
err:
	if (err == -EIO)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = nvme_host_path_error(rq);
	else if (err == -ENOMEM || err == -EAGAIN)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = BLK_STS_RESOURCE;
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ret = BLK_STS_IOERR;
	nvme_cleanup_cmd(rq);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
unmap_qe:
	ib_dma_unmap_single(dev, req->sqe.dma, sizeof(struct nvme_command),	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
			    DMA_TO_DEVICE);
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static int nvme_rdma_poll(struct blk_mq_hw_ctx *hctx, struct io_comp_batch *iob)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{
	struct nvme_rdma_queue *queue = hctx->driver_data;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	return ib_process_cq_direct(queue->ib_cq, -1);	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
}

/*
 * [한국어]
 * nvme_rdma_check_pi_status - signature MR 이 검출한 보호 정보 오류를 확인한다
 *
 * @req: signature MR 을 썼던 요청
 * @return: 없음. 결과는 nvme_req(rq)->status 에 기록한다.
 *
 * T10-PI 검증을 HCA 에 맡긴 경우의 뒤처리다. signature MR 은 데이터를 옮기면서
 * 보호 정보를 함께 검사하는데, 어긋나도 그 자리에서 오류를 내지 않고 MR 에
 * 기록해 둔다. 그래서 완료 후 이렇게 따로 물어봐야 한다.
 *
 * CPU 가 검증하는 경우와 비교하면 이 구조의 이유가 드러난다. 소프트웨어
 * 검증은 데이터를 한 번 더 읽어야 하지만, 오프로드하면 HCA 가 전송 중에
 * 함께 처리하므로 추가 메모리 접근이 없다. 대신 결과를 나중에 회수한다.
 *
 * 오류 종류를 세 가지로 나누어 각각 다른 NVMe 상태 코드로 옮기는 것이
 * 중요하다. Guard 는 데이터 자체의 CRC 불일치, Reference Tag 는 블록 주소
 * 불일치(엉뚱한 블록을 읽었다는 뜻), Application Tag 는 상위가 붙인 표식
 * 불일치다. 상위 계층이 원인을 구분해야 대응이 달라진다.
 *
 * ib_check_mr_status 자체가 실패하면 검증 결과를 알 수 없으므로, 안전한
 * 쪽으로 INVALID_PI 를 기록한다 -- 모르는 것을 성공으로 넘기지 않는다.
 *
 * 실행 컨텍스트: 완료 처리 경로.
 *
 * 호출 체인:
 *   nvme_rdma_process_nvme_rsp → [이 함수] → ib_check_mr_status
 */
static void nvme_rdma_check_pi_status(struct nvme_rdma_request *req)
{
	struct request *rq = blk_mq_rq_from_pdu(req);
	struct ib_mr_status mr_status;
	int ret;

	ret = ib_check_mr_status(req->mr, IB_MR_CHECK_SIG_STATUS, &mr_status);	/* [한국어] 전송 중 검사 결과를 MR 에서 회수한다 */
	if (ret) {
		pr_err("ib_check_mr_status failed, ret %d\n", ret);
		nvme_req(rq)->status = NVME_SC_INVALID_PI;	/* [한국어] 결과를 알 수 없다 — 모르는 것을 성공으로 넘기지 않는다 */
		return;
	}

	if (mr_status.fail_status & IB_MR_CHECK_SIG_STATUS) {	/* [한국어] 실제로 불일치가 있었다 */
		switch (mr_status.sig_err.err_type) {
		case IB_SIG_BAD_GUARD:	/* [한국어] 데이터 자체의 CRC 가 어긋났다 — 전송 중 손상 */
			nvme_req(rq)->status = NVME_SC_GUARD_CHECK;
			break;
		case IB_SIG_BAD_REFTAG:	/* [한국어] 블록 주소가 어긋났다 — 엉뚱한 블록을 읽어 온 것이다 */
			nvme_req(rq)->status = NVME_SC_REFTAG_CHECK;
			break;
		case IB_SIG_BAD_APPTAG:	/* [한국어] 상위가 붙인 표식이 어긋났다 */
			nvme_req(rq)->status = NVME_SC_APPTAG_CHECK;
			break;
		}
		pr_err("PI error found type %d expected 0x%x vs actual 0x%x\n",	/* [한국어] 기대값과 실제값을 함께 남긴다 — 원인 추적의 근거다 */
		       mr_status.sig_err.err_type, mr_status.sig_err.expected,
		       mr_status.sig_err.actual);
	}
}

static void nvme_rdma_complete_rq(struct request *rq)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{
	struct nvme_rdma_request *req = blk_mq_rq_to_pdu(rq);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct nvme_rdma_queue *queue = req->queue;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	struct ib_device *ibdev = queue->device->dev;	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */

	if (req->use_sig_mr)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_rdma_check_pi_status(req);

	nvme_rdma_unmap_data(queue, rq);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	ib_dma_unmap_single(ibdev, req->sqe.dma, sizeof(struct nvme_command),	/* [한국어] RDMA CM/IB verbs — 연결·QP·CQ·MR·WR */
			    DMA_TO_DEVICE);
	nvme_complete_rq(rq);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
}

static void nvme_rdma_map_queues(struct blk_mq_tag_set *set)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{
	struct nvme_rdma_ctrl *ctrl = to_rdma_ctrl(set->driver_data);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	nvmf_map_queues(set, &ctrl->ctrl, ctrl->io_queues);	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
}

static const struct blk_mq_ops nvme_rdma_mq_ops = {	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	.queue_rq	= nvme_rdma_queue_rq,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	.complete	= nvme_rdma_complete_rq,
	.init_request	= nvme_rdma_init_request,
	.exit_request	= nvme_rdma_exit_request,
	.init_hctx	= nvme_rdma_init_hctx,
	.timeout	= nvme_rdma_timeout,
	.map_queues	= nvme_rdma_map_queues,
	.poll		= nvme_rdma_poll,
};

static const struct blk_mq_ops nvme_rdma_admin_mq_ops = {	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	.queue_rq	= nvme_rdma_queue_rq,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	.complete	= nvme_rdma_complete_rq,
	.init_request	= nvme_rdma_init_request,
	.exit_request	= nvme_rdma_exit_request,
	.init_hctx	= nvme_rdma_init_admin_hctx,
	.timeout	= nvme_rdma_timeout,
};

static void nvme_rdma_shutdown_ctrl(struct nvme_rdma_ctrl *ctrl, bool shutdown)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{
	nvme_rdma_teardown_io_queues(ctrl, shutdown);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	nvme_quiesce_admin_queue(&ctrl->ctrl);	/* [한국어] 새 명령을 막는 것이 먼저다 */
	nvme_disable_ctrl(&ctrl->ctrl, shutdown);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
	nvme_rdma_teardown_admin_queue(ctrl, shutdown);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
}

static void nvme_rdma_delete_ctrl(struct nvme_ctrl *ctrl)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{
	nvme_rdma_shutdown_ctrl(to_rdma_ctrl(ctrl), true);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
}

/*
 * [한국어]
 * nvme_rdma_reset_ctrl_work - 사용자나 코어가 요청한 리셋을 수행한다
 *
 * @work: ctrl.reset_work
 * @return: 없음
 *
 * error_recovery_work 와 목적이 비슷하지만 진입 경로가 다르다. 이쪽은
 * 오류가 아니라 명시적 요청(sysfs write, 코어의 복구 정책)으로 불린다.
 *
 * 그 차이가 상태 전이 처리에 드러난다. 여기서는 CONNECTING 전이 실패를
 * WARN_ON_ONCE(1) 로 잡는다 -- 리셋은 코어가 이미 상태를 RESETTING 으로
 * 옮긴 뒤에 부르므로, 그 상태에서 CONNECTING 으로 못 가는 경우가 없어야
 * 한다. 반면 error_recovery_work 는 삭제와 겹칠 수 있어 그 경우를 정상으로 본다.
 *
 * shutdown_ctrl 이 teardown 대신 쓰이는 것도 다르다. 리셋에서는 컨트롤러에
 * 정상 종료를 알릴 기회가 있으므로 그 절차를 밟는다.
 *
 * 실행 컨텍스트: nvme_reset_wq 워크큐. 오래 잠들 수 있다.
 *
 * 호출 체인:
 *   nvme_reset_ctrl → [이 함수] → nvme_rdma_shutdown_ctrl → nvme_rdma_setup_ctrl
 */
static void nvme_rdma_reset_ctrl_work(struct work_struct *work)
{
	struct nvme_rdma_ctrl *ctrl =
		container_of(work, struct nvme_rdma_ctrl, ctrl.reset_work);
	int ret;

	nvme_stop_ctrl(&ctrl->ctrl);	/* [한국어] keep-alive 와 스캔을 멈춘다 */
	nvme_rdma_shutdown_ctrl(ctrl, false);	/* [한국어] 정상 종료 절차를 밟는다 — 오류 복구와 달리 알릴 기회가 있다 */

	if (!nvme_change_ctrl_state(&ctrl->ctrl, NVME_CTRL_CONNECTING)) {
		/* state change failure should never happen */
		/* [한국어] 위 영어 주석대로 여기서는 실패할 수 없다 — 코어가 이미
		 * RESETTING 으로 옮긴 뒤에 부르기 때문이다. 오류 복구 경로가
		 * 같은 실패를 정상으로 보는 것과 대비된다. */
		WARN_ON_ONCE(1);
		return;
	}

	ret = nvme_rdma_setup_ctrl(ctrl, false);	/* [한국어] 태그셋을 재사용해 다시 세운다 */
	if (ret)
		goto out_fail;

	return;

out_fail:
	++ctrl->ctrl.nr_reconnects;	/* [한국어] 실패했으니 재연결 시도로 계산한다 */
	nvme_rdma_reconnect_or_remove(ctrl, ret);
}

static const struct nvme_ctrl_ops nvme_rdma_ctrl_ops = {	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	.name			= "rdma",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	.module			= THIS_MODULE,
	.flags			= NVME_F_FABRICS | NVME_F_METADATA_SUPPORTED,
	.reg_read32		= nvmf_reg_read32,
	.reg_read64		= nvmf_reg_read64,
	.reg_write32		= nvmf_reg_write32,
	.subsystem_reset	= nvmf_subsystem_reset,
	.free_ctrl		= nvme_rdma_free_ctrl,
	.submit_async_event	= nvme_rdma_submit_async_event,
	.delete_ctrl		= nvme_rdma_delete_ctrl,
	.get_address		= nvmf_get_address,
	.stop_ctrl		= nvme_rdma_stop_ctrl,
	.get_virt_boundary	= nvme_get_virt_boundary,
};

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
static bool
nvme_rdma_existing_controller(struct nvmf_ctrl_options *opts)
{
	struct nvme_rdma_ctrl *ctrl;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	bool found = false;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	mutex_lock(&nvme_rdma_ctrl_mutex);	/* [한국어] 중복 연결 검사가 이 목록을 훑는다 */
	list_for_each_entry(ctrl, &nvme_rdma_ctrl_list, list) {	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		found = nvmf_ip_options_match(&ctrl->ctrl, opts);	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
		if (found)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			break;
	}
	mutex_unlock(&nvme_rdma_ctrl_mutex);

	return found;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static struct nvme_rdma_ctrl *nvme_rdma_alloc_ctrl(struct device *dev,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		struct nvmf_ctrl_options *opts)
{
	struct nvme_rdma_ctrl *ctrl;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ctrl = kzalloc_obj(*ctrl);	/* [한국어] 커널 메모리 생명주기 */
	if (!ctrl)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return ERR_PTR(-ENOMEM);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	ctrl->ctrl.opts = opts;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	INIT_LIST_HEAD(&ctrl->list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!(opts->mask & NVMF_OPT_TRSVCID)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		opts->trsvcid =	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			kstrdup(__stringify(NVME_RDMA_IP_PORT), GFP_KERNEL);
		if (!opts->trsvcid) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			ret = -ENOMEM;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			goto out_free_ctrl;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
		}
		opts->mask |= NVMF_OPT_TRSVCID;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}

	ret = inet_pton_with_scope(&init_net, AF_UNSPEC,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			opts->traddr, opts->trsvcid, &ctrl->addr);
	if (ret) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		pr_err("malformed address passed: %s:%s\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			opts->traddr, opts->trsvcid);
		goto out_free_ctrl;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}

	if (opts->mask & NVMF_OPT_HOST_TRADDR) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = inet_pton_with_scope(&init_net, AF_UNSPEC,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			opts->host_traddr, NULL, &ctrl->src_addr);
		if (ret) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			pr_err("malformed src address passed: %s\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			       opts->host_traddr);
			goto out_free_ctrl;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
		}
	}

	if (!opts->duplicate_connect && nvme_rdma_existing_controller(opts)) {	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		ret = -EALREADY;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_free_ctrl;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}

	INIT_DELAYED_WORK(&ctrl->reconnect_work,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvme_rdma_reconnect_ctrl_work);
	INIT_WORK(&ctrl->err_work, nvme_rdma_error_recovery_work);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	INIT_WORK(&ctrl->ctrl.reset_work, nvme_rdma_reset_ctrl_work);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	ctrl->ctrl.queue_count = opts->nr_io_queues + opts->nr_write_queues +	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				opts->nr_poll_queues + 1;
	ctrl->ctrl.sqsize = opts->queue_size - 1;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ctrl->ctrl.kato = opts->kato;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = -ENOMEM;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ctrl->queues = kzalloc_objs(*ctrl->queues, ctrl->ctrl.queue_count);	/* [한국어] 커널 메모리 생명주기 */
	if (!ctrl->queues)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_free_ctrl;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	ret = nvme_init_ctrl(&ctrl->ctrl, dev, &nvme_rdma_ctrl_ops,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
				0 /* no quirks, we're perfect! */);
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_kfree_queues;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	return ctrl;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

out_kfree_queues:
	kfree(ctrl->queues);	/* [한국어] 큐 배열 */
out_free_ctrl:
	kfree(ctrl);
	return ERR_PTR(ret);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static struct nvme_ctrl *nvme_rdma_create_ctrl(struct device *dev,	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		struct nvmf_ctrl_options *opts)
{
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
		nvmf_ctrl_subsysnqn(&ctrl->ctrl), &ctrl->addr, opts->host->nqn);

	mutex_lock(&nvme_rdma_ctrl_mutex);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	list_add_tail(&ctrl->list, &nvme_rdma_ctrl_list);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	mutex_unlock(&nvme_rdma_ctrl_mutex);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	return &ctrl->ctrl;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

out_uninit_ctrl:
	nvme_uninit_ctrl(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
out_put_ctrl:
	nvme_put_ctrl(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret > 0)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = -EIO;
	return ERR_PTR(ret);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static struct nvmf_transport_ops nvme_rdma_transport = {	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	.name		= "rdma",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	.module		= THIS_MODULE,
	.required_opts	= NVMF_OPT_TRADDR,
	.allowed_opts	= NVMF_OPT_TRSVCID | NVMF_OPT_RECONNECT_DELAY |
			  NVMF_OPT_HOST_TRADDR | NVMF_OPT_CTRL_LOSS_TMO |
			  NVMF_OPT_NR_WRITE_QUEUES | NVMF_OPT_NR_POLL_QUEUES |
			  NVMF_OPT_TOS,
	.create_ctrl	= nvme_rdma_create_ctrl,
};

/*
 * [한국어]
 * nvme_rdma_remove_one - HCA 가 제거될 때 그것을 쓰던 컨트롤러를 모두 지운다
 *
 * @ib_device:   사라지는 RDMA 장치
 * @client_data: ib_client 등록 시 넘긴 데이터(이 드라이버는 쓰지 않는다)
 * @return: 없음
 *
 * ib_client 콜백이다. HCA 가 뽑히거나 그 드라이버가 언로드되면 불린다.
 * 장치가 사라진 뒤에도 컨트롤러가 남아 있으면 이미 없는 QP 와 PD 를
 * 가리키게 되므로, 여기서 전부 지워야 한다.
 *
 * 먼저 device_list 를 훑어 우리가 이 장치를 쓰고 있었는지 확인한다.
 * 쓰지 않았다면 할 일이 없다 -- 시스템에 여러 HCA 가 있고 그중 우리와
 * 무관한 것이 빠지는 경우다.
 *
 * flush_workqueue 가 마지막에 오는 것이 중요하다. nvme_delete_ctrl 은
 * 삭제를 예약만 하므로, 기다리지 않으면 아직 지워지는 중에 이 콜백이
 * 반환되고 ib 코어가 장치 구조체를 해제한다. 그러면 해체 도중의 코드가
 * 사라진 장치를 만진다.
 *
 * 두 락을 따로 잡는 것은 잠금 순서를 지키기 위해서다. 장치 목록을 놓은 뒤
 * 컨트롤러 목록을 잡으므로, 두 락을 동시에 들지 않는다.
 *
 * 실행 컨텍스트: ib_client 제거 콜백. flush 로 오래 잠들 수 있다.
 *
 * 호출 체인:
 *   ib 코어(장치 제거) → [이 함수] → nvme_delete_ctrl
 */
static void nvme_rdma_remove_one(struct ib_device *ib_device, void *client_data)
{
	struct nvme_rdma_ctrl *ctrl;
	struct nvme_rdma_device *ndev;
	bool found = false;

	mutex_lock(&device_list_mutex);
	list_for_each_entry(ndev, &device_list, entry) {	/* [한국어] 우리가 이 HCA 를 쓰고 있었는가 */
		if (ndev->dev == ib_device) {
			found = true;
			break;
		}
	}
	mutex_unlock(&device_list_mutex);	/* [한국어] 컨트롤러 목록을 잡기 전에 놓는다 — 두 락을 동시에 들지 않는다 */

	if (!found)
		return;	/* [한국어] 무관한 HCA 가 빠진 경우 — 할 일이 없다 */

	/* Delete all controllers using this device */
	mutex_lock(&nvme_rdma_ctrl_mutex);
	list_for_each_entry(ctrl, &nvme_rdma_ctrl_list, list) {
		if (ctrl->device->dev != ib_device)	/* [한국어] 다른 HCA 를 쓰는 컨트롤러는 건드리지 않는다 */
			continue;
		nvme_delete_ctrl(&ctrl->ctrl);	/* [한국어] 예약만 한다 — 실제 삭제는 워크큐에서 일어난다 */
	}
	mutex_unlock(&nvme_rdma_ctrl_mutex);

	flush_workqueue(nvme_delete_wq);	/* [한국어] 반드시 기다린다. 안 그러면 삭제 중에 ib 코어가 장치를 해제해 사라진 장치를 만지게 된다 */
}

static struct ib_client nvme_rdma_ib_client = {	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	.name   = "nvme_rdma",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	.remove = nvme_rdma_remove_one
};

static int __init nvme_rdma_init_module(void)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = ib_register_client(&nvme_rdma_ib_client);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	ret = nvmf_register_transport(&nvme_rdma_transport);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto err_unreg_client;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

err_unreg_client:
	ib_unregister_client(&nvme_rdma_ib_client);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static void __exit nvme_rdma_cleanup_module(void)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
{
	struct nvme_rdma_ctrl *ctrl;	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	nvmf_unregister_transport(&nvme_rdma_transport);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	ib_unregister_client(&nvme_rdma_ib_client);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

	mutex_lock(&nvme_rdma_ctrl_mutex);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	list_for_each_entry(ctrl, &nvme_rdma_ctrl_list, list)	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
		nvme_delete_ctrl(&ctrl->ctrl);
	mutex_unlock(&nvme_rdma_ctrl_mutex);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
	flush_workqueue(nvme_delete_wq);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}

module_init(nvme_rdma_init_module);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */
module_exit(nvme_rdma_cleanup_module);	/* [한국어] NVMe/RDMA QP·CM·MR 경로 헬퍼 */

MODULE_DESCRIPTION("NVMe host RDMA transport driver");	/* [한국어] 모듈 경계·파라미터·심볼 공개 */
MODULE_LICENSE("GPL v2");	/* [한국어] 모듈 경계·파라미터·심볼 공개 */
