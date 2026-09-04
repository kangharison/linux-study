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
 *
 * === 전체 아키텍처에서의 위치 ===
 * NVMe over Fibre Channel 호스트 트랜스포트다. 다른 트랜스포트와 달리 이 파일은
 * 네트워크를 직접 다루지 않는다 -- FC 프레임 송수신은 HBA 의 LLDD(low-level
 * driver)가 하고, 이 파일은 그 위에서 Association 과 Connection 이라는 FC-NVME
 * 고유의 세션 계층을 관리한다.
 * 호출 체인(연결):
 *   nvme_fc_create_ctrl → nvme_fc_create_association
 *     → Create Association LS → LLDD 의 ls_req 콜백 → FC 프레임
 *       → Association ID 수신 → 큐마다 Create Connection LS
 *         → nvmf_connect_admin_queue / _io_queue (Fabrics Connect 명령)
 * 호출 체인(제출):
 *   blk-mq → nvme_fc_queue_rq → FC-NVME IU 조립 → LLDD 의 fcp_io 콜백
 *     → 완료 콜백 → nvme_fc_fcpio_done → nvme_complete_rq
 * localport(로컬 HBA 포트)와 remoteport(원격 타겟 포트)가 등록·해제되는 것을 축으로,
 * 그 위에 컨트롤러들이 매달리는 구조다.
 *
 * === 타 모듈과의 연결 ===
 * - drivers/scsi 의 FC LLDD(lpfc, qla2xxx 등): 실제 프레임 송수신 주체.
 *   nvme_fc_register_localport / _register_remoteport 로 이 파일에 자신을 알리고,
 *   nvme_fc_port_template 의 콜백으로 요청을 받는다.
 * - drivers/nvme/host/fc.h: 호스트와 타겟이 공유하는 LS PDU 포맷과 검증 루틴.
 * - drivers/nvme/target/fc: 반대편 구현. 같은 LS 를 반대 방향으로 처리한다.
 * - drivers/nvme/host/fabrics.c: 연결 옵션과 Connect 명령. "fc" 이름으로 등록한다.
 * - drivers/nvme/host/core.c: 상태 기계와 Identify 를 위임한다.
 * FC 는 포트가 사라졌다 다시 나타나는 일이 잦은 매체라, 이 파일에는 다른
 * 트랜스포트보다 정교한 참조 계수와 재연결 정책이 들어 있다.
 *
 * === 주요 함수/구조체 요약 ===
 * - nvme_fc_register_localport / _unregister_localport: HBA 의 로컬 포트 등록.
 *   LLDD 가 부팅 시 또는 포트가 올라올 때 부른다.
 * - nvme_fc_register_remoteport / _unregister_remoteport: 발견된 원격 타겟 포트 등록.
 *   포트가 사라지면 그 아래 컨트롤러들이 재연결 대기로 들어간다.
 * - nvme_fc_create_ctrl: 'nvme connect -t fc' 의 종착점. traddr/host-traddr 로
 *   local/remote 포트 짝을 찾아 컨트롤러를 만든다.
 * - nvme_fc_create_association / nvme_fc_delete_association: FC-NVME 세션의 수립과
 *   해체. Create Association LS 로 시작해 큐마다 Connection 을 세운다.
 * - nvme_fc_queue_rq: 핫패스 제출. 요청을 FC-NVME IU 로 만들어 LLDD 에 넘긴다.
 * - nvme_fc_fcpio_done: LLDD 의 완료 콜백. 응답 IU 를 해석해 요청을 완료시킨다.
 * - nvme_fc_send_ls_req / nvme_fc_handle_ls_rqst: LS 송신과 수신 처리. 후자는
 *   타겟이 보낸 Disconnect Association 같은 통지를 받는다.
 * - nvme_fc_error_recovery / nvme_fc_reset_ctrl_work: 오류 복구와 리셋. 포트가
 *   돌아오기를 기다리며 정해진 횟수만큼 재연결을 시도한다.
 * - nvme_fc_timeout: 응답 없는 요청 처리. FC 는 abort 개념이 있어 LLDD 에 abort 를
 *   요청한 뒤 그래도 안 되면 컨트롤러 리셋으로 올라간다.
 * - struct nvme_fc_ctrl: 컨트롤러 하나 -- Association ID, 큐 배열, 로컬/원격 포트 참조.
 * - struct nvme_fc_queue: 큐 하나 -- Connection ID 와 큐 번호.
 * - struct nvme_fc_fcp_op: 요청 하나의 FC 측 상태 -- IU 버퍼와 LLDD 요청 구조체.
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


/* [한국어] FC 큐의 두 단계. FC 는 세션 계층이 따로 있어 "채널이 열렸는가"와
 * "NVMe 명령을 받을 수 있는가"가 별개의 사건이다. */
enum nvme_fc_queue_flags {
	/* [한국어] Create Connection LS 가 성공해 이 큐에 대응하는 FC 채널이 열렸다.
	 * 아직 Fabrics Connect 전이라 NVMe 명령은 보낼 수 없지만, 해체 시
	 * Disconnect LS 를 보내야 할 대상은 이미 존재한다는 표시다. */
	NVME_FC_Q_CONNECTED = 0,
	/* [한국어] Fabrics Connect 까지 끝나 이 큐로 I/O 를 보낼 수 있다.
	 * 읽는 자: queue_rq 가 아직 준비 안 된 큐의 요청을 거르는 기준. */
	NVME_FC_Q_LIVE,
};

#define NVME_FC_DEFAULT_DEV_LOSS_TMO	60	/* seconds */	/* [한국어] 상수/매크로 — PDU·큐·타임아웃·플래그 */
#define NVME_FC_DEFAULT_RECONNECT_TMO	2	/* delay between reconnects
						 * when connected and a
						 * connection failure.
						 */

/* [한국어] FC 큐 하나. 다른 트랜스포트와 달리 여기에는 하드웨어 자원이 없다 --
 * 프레임 송수신은 HBA 의 LLDD 가 하고, 이 구조체는 그 위에 얹힌 세션 정보다. */
struct nvme_fc_queue {
	/* [한국어] 이 큐가 속한 컨트롤러. 초기화 후 불변. */
	struct nvme_fc_ctrl	*ctrl;
	/* [한국어] DMA 매핑의 기준이 되는 장치. LLDD 의 struct device 를 그대로 쓴다.
	 * 프레임을 실제로 옮기는 것이 HBA 이므로 매핑도 그쪽 장치 기준이어야 한다. */
	struct device		*dev;
	/* [한국어] 이 큐에 대응하는 blk-mq 하드웨어 큐 문맥. */
	struct blk_mq_hw_ctx	*hctx;
	/* [한국어] LLDD 가 이 큐를 식별하기 위해 돌려준 불투명 핸들.
	 * 왜 불투명한가: 큐 자원을 어떻게 표현할지는 HBA 마다 다르므로 이 계층은
	 *   내용을 해석하지 않고 콜백에 그대로 되돌려 준다.
	 * 설정자: LLDD 의 create_queue 콜백. 읽는 자: 모든 fcp_io 요청. */
	void			*lldd_handle;
	/* [한국어] 명령 캡슐의 크기. 인라인 데이터 여부가 여기서 갈린다. */
	size_t			cmnd_capsule_len;
	/* [한국어] 큐 번호. 0 이 admin, 1 부터 I/O 다. */
	u32			qnum;
	/* [한국어] 이 큐로 보낸 요청 수. 통계 겸 디버깅용. */
	u32			rqcnt;
	/* [한국어] FC 익스체인지 시퀀스 번호. */
	u32			seqno;

	/* [한국어] Create Connection LS 가 발급받은 Connection ID.
	 * 왜 중요한가: 이후 이 큐로 나가는 모든 IU 가 이 값을 실어야 타겟이
	 *   어느 큐의 명령인지 안다. PCIe 의 큐 ID 에 해당하지만, 값은 타겟이 정한다.
	 * 설정자: nvme_fc_connect_queue 가 LS 응답에서 꺼낸다. */
	u64			connection_id;
	/* [한국어] Command Sequence Number. 명령마다 하나씩 증가한다.
	 * 왜 원자적인가: 같은 큐에 여러 CPU 가 동시에 제출할 수 있고, 타겟은
	 *   이 번호로 순서와 유실을 판단하므로 건너뛰거나 겹치면 안 된다. */
	atomic_t		csn;

	/* [한국어] 위 enum nvme_fc_queue_flags 의 비트 집합. */
	unsigned long		flags;
} __aligned(sizeof(u64));	/* alignment for other things alloc'd with */
/* [한국어] 위 영어 주석대로, 이 구조체 뒤에 다른 것을 이어 할당하는 곳이 있어
 * 8바이트 정렬을 강제한다. 정렬이 어긋나면 뒤따르는 구조체의 u64 필드가
 * 비정렬 접근이 된다. */

/* [한국어] FCP 오퍼레이션의 성격을 나타내는 플래그. */
enum nvme_fcop_flags {
	/* [한국어] 이 오퍼레이션은 종료(termination) 처리 중이다.
	 * 왜 필요한가: 컨트롤러를 내릴 때 진행 중인 요청을 LLDD 에 abort 요청하는데,
	 *   그 완료가 정상 완료와 구분되어야 한다. 이 비트가 서 있으면 완료 경로가
	 *   재시도나 페일오버로 넘기지 않고 취소로 처리한다. */
	FCOP_FLAGS_TERMIO	= (1 << 0),
	/* [한국어] 이 오퍼레이션은 비동기 이벤트(AEN)용이다.
	 * AEN 은 태그를 소비하지 않는 상주 명령이라 blk-mq 요청이 없고,
	 * 완료 시 요청을 끝내는 대신 다시 제출해야 한다. 그 분기의 근거가 이 비트다. */
	FCOP_FLAGS_AEN		= (1 << 1),
};

/* [한국어] Link Service 요청 하나의 진행 상태.
 * LS 는 데이터 I/O 와 별개의 제어 평면 메시지다 -- Create Association,
 * Create Connection, Disconnect 가 모두 이 경로로 오간다. */
struct nvmefc_ls_req_op {
	/* [한국어] LLDD 에 넘길 요청 서술자. 반드시 맨 앞이어야 완료 콜백에서
	 * container_of 로 이 구조체를 되찾을 수 있다. */
	struct nvmefc_ls_req	ls_req;

	/* [한국어] 이 LS 를 보낼 원격 포트. 참조를 하나 들고 있어, LS 가 끝나기 전에
	 * 포트가 사라지지 않도록 한다. */
	struct nvme_fc_rport	*rport;
	/* [한국어] 이 LS 가 관련된 큐. Create/Disconnect Connection 에서 쓰인다. */
	struct nvme_fc_queue	*queue;
	/* [한국어] 이 LS 를 유발한 blk-mq 요청. 순수 제어 경로면 NULL 이다. */
	struct request		*rq;
	/* [한국어] 위 enum nvme_fcop_flags 조합. */
	u32			flags;

	/* [한국어] LS 의 결과. 완료 콜백은 값을 돌려줄 수 없어 여기 남긴다. */
	int			ls_error;
	/* [한국어] LS 완료를 기다리는 객체. 송신은 비동기지만 호출자는 대개
	 * 여기서 잠들어 응답을 기다린다 -- 세션 수립은 순서가 있는 절차이기 때문이다. */
	struct completion	ls_done;
	/* [한국어] rport->ls_req_list 에 매달리는 고리. 포트가 사라질 때
	 * 진행 중인 LS 를 찾아 취소하기 위해 목록으로 관리한다. */
	struct list_head	lsreq_list;	/* rport->ls_req_list */
	/* [한국어] 이 요청이 위 목록에 올라 있는가.
	 * 왜 별도 플래그인가: 취소 경로와 완료 경로가 동시에 목록에서 빼려 할 수
	 *   있어, 한 번만 빼도록 하는 판정이 필요하다. */
	bool			req_queued;
};

struct nvmefc_ls_rcv_op {	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	/* [한국어] 이 LS 를 보내 온 원격 포트.
	 * 설정자: nvme_fc_rcv_ls_req 가 수신 시점에 채운다.
	 * 읽는 자: 응답 전송과 취소 경로가 LLDD 핸들을 찾는 데 쓴다.
	 * 값 범위: 유효한 rport 포인터. 이 구조체는 rport 의 목록에 매달리므로
	 *   rport 보다 오래 살 수 없다.
	 * 동기화: rport->lock 아래에서 설정·해제된다. */
	struct nvme_fc_rport		*rport;

	/* [한국어] LLDD 가 넘겨준 응답 전송 핸들.
	 * 설정자: LLDD 가 nvme_fc_rcv_ls_req 인자로 준다.
	 * 읽는 자: nvme_fc_xmt_ls_rsp 가 이것을 LLDD 에 되돌려 응답을 보낸다.
	 * 값 범위: 이 LS 교환에만 유효한 불투명 핸들 — 응답 완료 후 무효.
	 * 동기화: 한 LS 는 수신부터 응답까지 한 흐름으로 처리되어 락이 없다. */
	struct nvmefc_ls_rsp		*lsrsp;

	/* [한국어] 받은 LS 요청이 담긴 버퍼. 모든 LS 종류를 덮는 공용체다.
	 * 설정자: op 를 할당할 때 뒤따르는 DMA 가능 메모리를 가리키게 한다.
	 * 읽는 자: nvme_fc_handle_ls_rqst 가 w0.ls_cmd 로 종류를 가른 뒤
	 *   해당 멤버로 해석한다.
	 * 값 범위: Create Association / Create Connection / Disconnect 중 하나.
	 * 동기화: LLDD 가 채운 뒤 넘기므로 이후에는 읽기만 한다. */
	union nvmefc_ls_requests	*rqstbuf;

	/* [한국어] 보낼 LS 응답을 조립하는 버퍼. 역시 모든 응답을 덮는 공용체.
	 * 설정자: 각 LS 처리 함수가 승인 또는 거절(RJT)을 채운다.
	 * 읽는 자: LLDD 가 DMA 로 읽어 선로에 내보낸다.
	 * 값 범위: 요청과 짝이 되는 응답 종류.
	 * 동기화: 전송이 끝날 때까지 살아 있어야 하므로 완료 콜백 전에는
	 *   해제하지 않는다. */
	union nvmefc_ls_responses	*rspbuf;

	/* [한국어] 실제로 받은 요청 바이트 수.
	 * 설정자: 수신 시 LLDD 가 알려 준 길이를 그대로 저장한다.
	 * 읽는 자: 각 LS 처리 함수가 "이 종류가 요구하는 최소 길이"와 비교해
	 *   짧으면 RJT 로 거절한다. 이 검사가 없으면 잘린 PDU 를 읽는다.
	 * 값 범위: 0 이상. 종류별 고정 크기와 비교된다. */
	u16				rqstdatalen;

	/* [한국어] 이 LS 를 이미 처리했는가.
	 * 왜 필요한가: 같은 association 에 대해 중복 LS 가 오거나 취소 경로와
	 *   처리 경로가 겹칠 때, 응답을 두 번 보내지 않도록 한 번만 통과시킨다.
	 * 설정자: 처리 경로가 rport->lock 아래에서 세운다.
	 * 읽는 자: 취소 경로가 이미 처리된 것을 건너뛰는 데 쓴다. */
	bool				handled;

	/* [한국어] rspbuf 의 장치 주소.
	 * 왜 별도로 두는가: LLDD 는 CPU 가상 주소가 아니라 DMA 주소로 읽는다.
	 * 설정자: op 준비 시 fc_dma_map_single 결과.
	 * 읽는 자: 응답 전송 시 LLDD 에 넘어가고, 해제 시 unmap 에 쓰인다.
	 * 값 범위: 매핑 성공 시 유효. 실패는 fc_dma_mapping_error 로 판정한다. */
	dma_addr_t			rspdma;

	/* [한국어] rport 의 수신 LS 목록에 매달리는 고리.
	 * 왜 목록인가: 원격 포트가 사라질 때 아직 응답하지 못한 LS 를 모두
	 *   찾아 정리해야 하기 때문이다.
	 * 동기화: rport->lock 이 목록 전체를 보호한다. */
	struct list_head		lsrcv_list;	/* rport->ls_rcv_list */
} __aligned(sizeof(u64));	/* alignment for other things alloc'd with */
/* [한국어] 위 영어 주석대로, 이 구조체 뒤에 요청·응답 버퍼를 이어 붙여
 * 한 번에 할당한다. u64 정렬이라야 뒤따르는 IU 들이 비정렬 접근을 겪지 않는다. */

/* [한국어] FCP 오퍼레이션 하나의 생애 상태.
 * atomic_t 로 다뤄지며, 상태 전이를 atomic_cmpxchg 로 시도해 "내가 먼저
 * 바꾼 쪽인가"를 판정한다. 완료와 abort 가 동시에 같은 요청에 도착할 수
 * 있는데 둘 중 하나만 처리해야 하기 때문이다. */
enum nvme_fcpop_state {
	/* [한국어] 아직 초기화되지 않았다. 큐 할당 직후의 상태. */
	FCPOP_STATE_UNINIT	= 0,
	/* [한국어] 초기화는 끝났고 제출을 기다린다. */
	FCPOP_STATE_IDLE	= 1,
	/* [한국어] LLDD 에 넘겨져 진행 중이다. 이 상태에서만 abort 가 의미를 갖는다. */
	FCPOP_STATE_ACTIVE	= 2,
	/* [한국어] abort 를 요청했다. 완료 콜백이 와도 정상 완료로 처리하지 않는다. */
	FCPOP_STATE_ABORTED	= 3,
	/* [한국어] 완료 처리를 마쳤다. 이 상태로 바꾸는 데 성공한 쪽만 요청을 끝낸다. */
	FCPOP_STATE_COMPLETE	= 4,
};

/* [한국어] 요청 하나의 FC 측 상태. blk-mq 태그마다 미리 잡혀 있다. */
struct nvme_fc_fcp_op {
	struct nvme_request	nreq;		/*
						 * nvme/host/core.c
						 * requires this to be
						 * the 1st element in the
						 * private structure
						 * associated with the
						 * request.
						 */
	/* [한국어] 위 영어 주석이 이유를 밝힌다 -- 코어가 blk_mq_rq_to_pdu() 의
	 * 결과를 그대로 struct nvme_request 로 읽으므로 반드시 첫 필드여야 한다. */
	/* [한국어] LLDD 에 넘길 FCP 요청 서술자. 명령 IU, 응답 IU, 데이터 SG 목록의
	 * 위치와 길이를 담아 HBA 가 프레임으로 옮긴다.
	 * 설정자: nvme_fc_init_request 가 버퍼 주소를 채우고, queue_rq 가 길이를 정한다.
	 * 읽는 자: LLDD 의 fcp_io 콜백. */
	struct nvmefc_fcp_req	fcp_req;

	/* [한국어] 이 요청이 속한 컨트롤러. 완료 시 통계와 오류 처리의 기준이다. */
	struct nvme_fc_ctrl	*ctrl;
	/* [한국어] 이 요청이 나가는 큐. connection_id 와 csn 을 여기서 얻는다. */
	struct nvme_fc_queue	*queue;
	/* [한국어] 대응하는 blk-mq 요청. AEN 이면 NULL 이다. */
	struct request		*rq;

	/* [한국어] 위 enum nvme_fcpop_state 의 현재 값.
	 * atomic 인 이유: 완료 콜백과 abort 요청이 동시에 도착할 수 있어,
	 *   cmpxchg 로 상태를 바꾸는 데 성공한 쪽만 처리하게 해야 한다.
	 *   그러지 않으면 요청이 두 번 완료되거나 이미 끝난 것을 abort 하게 된다. */
	atomic_t		state;
	/* [한국어] 위 enum nvme_fcop_flags 조합. TERMIO 와 AEN 여부. */
	u32			flags;
	/* [한국어] 이 큐에서 몇 번째 요청인가. 디버깅용 일련번호. */
	u32			rqno;
	/* [한국어] DMA 매핑을 마친 scatterlist 항목 수. 해제 시 같은 값으로 unmap 한다. */
	u32			nents;

	/* [한국어] 보낼 명령 IU. NVMe SQE 를 FC 프레임에 담는 형식으로 감싼 것이다.
	 * 요청마다 값으로 품는 이유는 여러 요청이 동시에 전송 중일 수 있기 때문이다. */
	struct nvme_fc_cmd_iu	cmd_iu;
	/* [한국어] 받을 확장 응답 IU. 완료 상태와 전송된 바이트 수가 들어온다.
	 * LLDD 가 여기에 직접 써 넣으므로 DMA 가능한 메모리여야 한다. */
	struct nvme_fc_ersp_iu	rsp_iu;
};

struct nvme_fcp_op_w_sgl {	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	/* [한국어] 본체. 이 구조체는 op 를 감싸 뒤에 두 영역을 덧붙인 것이다.
	 * 왜 이렇게 묶는가: 요청 하나가 필요한 세 덩어리를 blk-mq 가 요청마다
	 *   잡아 주는 PDU 한 조각 안에 모두 담기 위해서다. 그러면 I/O 경로에서
	 *   추가 할당이 전혀 없다.
	 * 설정자: nvme_fc_init_request 가 요청 생성 시 초기화.
	 * 읽는 자: blk_mq_rq_to_pdu 로 되찾아 제출·완료·abort 가 모두 쓴다. */
	struct nvme_fc_fcp_op	op;

	/* [한국어] 세그먼트가 적은 흔한 I/O 를 위한 내장 산재 목록.
	 * 왜 필요한가: 대부분의 요청은 몇 조각뿐이라 별도 할당이 낭비다.
	 *   이 배열을 넘는 요청만 mempool 에서 큰 목록을 빌린다.
	 * 값 범위: NVME_INLINE_SG_CNT 개. 초과 시 op.data_sg 가 다른 곳을 가리킨다.
	 * 동기화: 요청 하나에 전속이라 락이 없다. */
	struct scatterlist	sgl[NVME_INLINE_SG_CNT];

	/* [한국어] LLDD 전용 영역. 가변 길이 배열이라 크기는 등록 시 LLDD 가 알린
	 *   fcprqst_priv_sz 로 정해진다.
	 * 왜 여기 있는가: LLDD 가 요청마다 자기 상태(FC 익스체인지 문맥 등)를
	 *   둘 곳이 필요한데, 따로 할당하면 I/O 경로에 할당이 하나 더 생긴다.
	 * 설정자/읽는 자: 오직 LLDD. 이 드라이버는 포인터만 넘기고 내용을 보지 않는다. */
	uint8_t			priv[];
};

/* [한국어] 로컬 FC 포트 하나 -- 이 호스트에 꽂힌 HBA 포트를 나타낸다.
 * LLDD 가 등록하며, 그 아래에 발견된 원격 포트들이 매달린다. */
struct nvme_fc_lport {
	/* [한국어] LLDD 와 공유하는 공개 부분(WWNN/WWPN, 포트 ID 등).
	 * 반드시 맨 앞이어야 LLDD 가 준 포인터에서 이 구조체를 되찾을 수 있다. */
	struct nvme_fc_local_port	localport;

	/* [한국어] 이 포트 아래 원격 포트에 번호를 부여하는 ID 할당자. */
	struct ida			endp_cnt;
	/* [한국어] 전역 nvme_fc_port_list 에 매달리는 고리. */
	struct list_head		port_list;	/* nvme_fc_port_list */
	/* [한국어] 이 포트에서 발견한 원격 포트들의 목록. */
	struct list_head		endp_list;
	/* [한국어] DMA 매핑의 기준 장치. 위 영어 주석대로 물리 장치다 --
	 * 실제로 메모리를 읽고 쓰는 것이 HBA 이므로 매핑도 그쪽 기준이어야 한다. */
	struct device			*dev;	/* physical device for dma */
	/* [한국어] LLDD 가 제공하는 콜백 모음(ls_req, fcp_io, create_queue 등).
	 * 이 계층이 FC 프레임을 직접 다루지 않고 전부 여기로 위임한다. */
	struct nvme_fc_port_template	*ops;
	/* [한국어] 이 포트 객체의 참조 계수. LLDD 가 포트를 내려도 아직 이 포트를
	 * 쓰는 컨트롤러가 있으면 객체는 살아 있어야 한다. */
	struct kref			ref;
	/* [한국어] 활성 상태인 원격 포트 수.
	 * 왜 세는가: 하나라도 살아 있으면 LLDD 모듈을 언로드할 수 없다. */
	atomic_t                        act_rport_cnt;
} __aligned(sizeof(u64));	/* alignment for other things alloc'd with */
/* [한국어] 위 영어 주석대로 이 구조체 뒤에 LLDD 전용 영역을 이어 할당하므로
 * 8바이트 정렬을 강제한다. */

/* [한국어] 원격 FC 포트 하나 -- 발견된 NVMe 타겟 포트를 나타낸다.
 * FC 는 포트가 사라졌다 다시 나타나는 일이 잦은 매체라, 이 객체의 수명 관리가
 * 다른 트랜스포트보다 정교하다. */
struct nvme_fc_rport {
	/* [한국어] LLDD 와 공유하는 공개 부분(WWNN/WWPN, 포트 상태 등).
	 * 맨 앞이어야 LLDD 가 준 포인터에서 이 구조체를 되찾을 수 있다. */
	struct nvme_fc_remote_port	remoteport;

	/* [한국어] 소속 로컬 포트의 endp_list 에 매달리는 고리. */
	struct list_head		endp_list; /* for lport->endp_list */
	/* [한국어] 이 원격 포트로 연결된 컨트롤러들의 목록.
	 * 포트가 사라지면 이 목록을 훑어 모두에게 알린다. */
	struct list_head		ctrl_list;
	/* [한국어] 진행 중인 LS 요청들의 목록.
	 * 왜 목록으로 두나: 포트가 사라질 때 응답이 영영 오지 않을 LS 를 찾아
	 *   취소해야 한다. 그러지 않으면 그것을 기다리는 쪽이 영원히 잠든다. */
	struct list_head		ls_req_list;
	/* [한국어] 타겟이 보내 온, 아직 처리하지 않은 LS 들의 목록.
	 * Disconnect Association 같은 통지가 여기로 들어온다. */
	struct list_head		ls_rcv_list;
	/* [한국어] 디스커버리 중 임시로 매다는 고리. */
	struct list_head		disc_list;
	/* [한국어] DMA 매핑 기준 장치. 로컬 포트의 것과 같은 물리 HBA 다. */
	struct device			*dev;	/* physical device for dma */
	/* [한국어] 이 원격 포트를 발견한 로컬 포트. */
	struct nvme_fc_lport		*lport;
	/* [한국어] 위 목록들을 보호하는 스핀락.
	 * 스핀락인 이유: LS 수신 콜백이 인터럽트 문맥에서 들어올 수 있어
	 *   잠자는 잠금을 쓸 수 없다. */
	spinlock_t			lock;
	/* [한국어] 이 포트 객체의 참조 계수. LLDD 가 포트 제거를 알려도 아직
	 * 컨트롤러가 붙어 있으면 객체는 유지된다. */
	struct kref			ref;
	/* [한국어] 이 포트에 붙어 활성 상태인 컨트롤러 수. */
	atomic_t                        act_ctrl_cnt;
	/* [한국어] 포트가 사라진 뒤 재등장을 기다리는 시한(jiffies).
	 * 왜 필요한가: FC 는 케이블을 뽑았다 꽂거나 스위치가 재구성되면 포트가
	 *   잠시 사라진다. 곧바로 컨트롤러를 지우면 그때마다 I/O 가 실패하므로,
	 *   이 시한 안에 돌아오면 같은 세션을 이어 간다. 넘기면 포기한다. */
	unsigned long			dev_loss_end;
	/* [한국어] 수신한 LS 를 처리하는 작업. 콜백은 인터럽트 문맥일 수 있어
	 * 실제 처리를 워크큐로 넘긴다. */
	struct work_struct		lsrcv_work;
} __aligned(sizeof(u64));	/* alignment for other things alloc'd with */
/* [한국어] 로컬 포트와 같은 이유로 8바이트 정렬을 강제한다. */	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

/* fc_ctrl flags values - specified as bit positions */
#define ASSOC_ACTIVE		0	/* [한국어] 상수/매크로 — PDU·큐·타임아웃·플래그 */
#define ASSOC_FAILED		1	/* [한국어] 상수/매크로 — PDU·큐·타임아웃·플래그 */
#define FCCTRL_TERMIO		2	/* [한국어] 상수/매크로 — PDU·큐·타임아웃·플래그 */

/* [한국어] FC 컨트롤러 하나. 로컬 포트와 원격 포트의 짝 위에 세워진 세션이다. */
struct nvme_fc_ctrl {
	/* [한국어] 아래 iocnt·flags·큐 상태를 보호하는 스핀락.
	 * 스핀락인 이유: 완료 콜백이 인터럽트 문맥에서 들어올 수 있다. */
	spinlock_t		lock;
	/* [한국어] 큐 배열. 0 이 admin, 1 부터 I/O 다. */
	struct nvme_fc_queue	*queues;
	/* [한국어] DMA 매핑 기준 장치(HBA). */
	struct device		*dev;
	/* [한국어] 이 세션이 쓰는 로컬 포트. 참조를 들고 있다. */
	struct nvme_fc_lport	*lport;
	/* [한국어] 접속한 원격 포트. 참조를 들고 있다. */
	struct nvme_fc_rport	*rport;
	/* [한국어] 컨트롤러 인스턴스 번호. /dev/nvmeX 의 X 에 대응한다. */
	u32			cnum;

	/* [한국어] I/O 큐들이 살아 있는가.
	 * 왜 별도 플래그인가: 재연결 시 admin 큐만 세운 상태와 I/O 큐까지
	 *   세운 상태를 구분해야 해체 절차가 달라진다. */
	bool			ioq_live;
	/* [한국어] Create Association LS 가 발급받은 Association ID.
	 * 이 값이 곧 FC 상의 세션 식별자다. 모든 큐의 Connection 이 이 아래 매달리고,
	 * Disconnect Association 하나로 전부 정리된다. */
	u64			association_id;
	/* [한국어] 타겟이 보내 온 Disconnect Association 요청.
	 * 왜 보관하나: 그 요청에 응답(ACC)을 보내야 하는데, 세션 정리가 끝난
	 *   뒤에 보내야 순서가 맞다. 그때까지 들고 있는다. */
	struct nvmefc_ls_rcv_op	*rcv_disconn;

	/* [한국어] 원격 포트의 ctrl_list 에 매달리는 고리. */
	struct list_head	ctrl_list;	/* rport->ctrl_list */

	/* [한국어] admin 전용 태그셋. I/O 가 모두 막혀도 리셋·삭제가 통과하도록 분리한다. */
	struct blk_mq_tag_set	admin_tag_set;
	/* [한국어] I/O 큐용 태그셋. */
	struct blk_mq_tag_set	tag_set;

	/* [한국어] I/O 오류 처리 작업. 완료 콜백에서 발견한 오류를 잠들 수 있는
	 * 문맥으로 옮겨 세션 재수립을 진행한다. */
	struct work_struct	ioerr_work;
	/* [한국어] 재연결 작업. 원격 포트가 돌아오기를 기다리며 주기적으로 시도한다. */
	struct delayed_work	connect_work;

	/* [한국어] 컨트롤러 객체의 참조 계수. */
	struct kref		ref;
	/* [한국어] ASSOC_ACTIVE / ASSOC_FAILED / FCCTRL_TERMIO 비트 집합.
	 * 위에 #define 으로 비트 위치가 정의돼 있다. */
	unsigned long		flags;
	/* [한국어] 아직 완료되지 않은 abort 요청 수.
	 * 왜 세는가: 세션을 내릴 때 진행 중인 I/O 를 모두 abort 하고, 그 완료를
	 *   전부 받은 뒤에야 큐를 해제할 수 있다. 남은 개수가 0 이 되면 아래
	 *   대기열을 깨운다. */
	u32			iocnt;
	/* [한국어] 위 iocnt 가 0 이 되기를 기다리는 대기열. */
	wait_queue_head_t	ioabort_wait;

	/* [한국어] 비동기 이벤트(AEN) 전용 오퍼레이션들.
	 * 태그를 소비하지 않는 상주 명령이라 태그셋 밖에 따로 잡아 둔다. */
	struct nvme_fc_fcp_op	aen_ops[NVME_NR_AEN_COMMANDS];

	/* [한국어] 코어가 보는 컨트롤러. to_fc_ctrl() 이 이 필드에서 바깥을 되찾는다. */
	struct nvme_ctrl	ctrl;
};

static inline struct nvme_fc_ctrl *	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
to_fc_ctrl(struct nvme_ctrl *ctrl)
{
	return container_of(ctrl, struct nvme_fc_ctrl, ctrl);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
}

static inline struct nvme_fc_lport *	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
localport_to_lport(struct nvme_fc_local_port *portptr)
{
	return container_of(portptr, struct nvme_fc_lport, localport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
}

static inline struct nvme_fc_rport *	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
remoteport_to_rport(struct nvme_fc_remote_port *portptr)
{
	return container_of(portptr, struct nvme_fc_rport, remoteport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
}

static inline struct nvmefc_ls_req_op *	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
ls_req_to_lsop(struct nvmefc_ls_req *lsreq)
{
	return container_of(lsreq, struct nvmefc_ls_req_op, ls_req);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static inline struct nvme_fc_fcp_op *	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
fcp_req_to_fcp_op(struct nvmefc_fcp_req *fcpreq)
{
	return container_of(fcpreq, struct nvme_fc_fcp_op, fcp_req);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
}



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
			struct nvme_fc_queue *, unsigned int);

static void nvme_fc_handle_ls_rqst_work(struct work_struct *work);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */


/*
 * [한국어]
 * nvme_fc_free_lport - 마지막 참조가 사라진 로컬 포트 객체를 해제한다
 *
 * @ref: 0 이 된 kref
 * @return: 없음
 *
 * LLDD 가 포트를 내렸고(DELETED), 그 아래 원격 포트도 모두 사라진 뒤에야
 * 여기 도달한다. 원격 포트들이 각각 lport 참조를 들고 있으므로, 그것들이
 * free_rport 에서 참조를 놓아야 비로소 0 이 된다.
 *
 * 두 WARN 이 그 가정을 검증한다 -- 상태가 DELETED 여야 하고 endp_list 가
 * 비어 있어야 한다. 원격 포트가 남아 있는데 여기 왔다면 참조 계수 관리가
 * 어딘가 어긋난 것이다.
 *
 * ida_destroy 를 여기서 부르는 이유: 이 포트가 자기 아래 원격 포트에
 * 번호를 부여하던 할당자다. 원격 포트가 모두 사라진 지금이 그것을 정리할
 * 유일하게 안전한 시점이다.
 *
 * put_device 는 LLDD 의 물리 장치 참조를 놓는 것이다. 이것을 놓아야
 * LLDD 모듈이 언로드될 수 있다.
 *
 * 실행 컨텍스트: kref_put 이 부른다.
 *
 * 호출 체인:
 *   nvme_fc_lport_put → kref_put → [이 함수]
 */
static void
nvme_fc_free_lport(struct kref *ref)
{
	struct nvme_fc_lport *lport =
		container_of(ref, struct nvme_fc_lport, ref);
	unsigned long flags;

	WARN_ON(lport->localport.port_state != FC_OBJSTATE_DELETED);	/* [한국어] 삭제 표시 없이 참조가 0 이 될 수 없다 */
	WARN_ON(!list_empty(&lport->endp_list));	/* [한국어] 원격 포트들이 각각 참조를 들고 있으므로, 남아 있는데 0 이 됐다면 계수 관리가 어긋난 것이다 */

	/* remove from transport list */
	spin_lock_irqsave(&nvme_fc_lock, flags);
	list_del(&lport->port_list);	/* [한국어] 전역 목록에서 먼저 뺀다 — 이후 create_ctrl 의 탐색이 이 객체를 집지 않도록 */
	spin_unlock_irqrestore(&nvme_fc_lock, flags);

	ida_free(&nvme_fc_local_port_cnt, lport->localport.port_num);	/* [한국어] 이 포트의 번호를 전역 할당자에 반납 */
	ida_destroy(&lport->endp_cnt);	/* [한국어] 자기 아래 원격 포트에 번호를 주던 할당자. 그것들이 다 사라진 지금이 정리할 유일한 시점이다 */

	put_device(lport->dev);	/* [한국어] LLDD 의 물리 장치 참조를 놓는다 — 이것을 놓아야 LLDD 모듈이 언로드된다 */

	kfree(lport);
}

static void
nvme_fc_lport_put(struct nvme_fc_lport *lport)
{
	kref_put(&lport->ref, nvme_fc_free_lport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
}

static int
nvme_fc_lport_get(struct nvme_fc_lport *lport)
{
	return kref_get_unless_zero(&lport->ref);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}


static struct nvme_fc_lport *	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
nvme_fc_attach_to_unreg_lport(struct nvme_fc_port_info *pinfo,
			struct nvme_fc_port_template *ops,
			struct device *dev)	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
{
	struct nvme_fc_lport *lport;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	unsigned long flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	spin_lock_irqsave(&nvme_fc_lock, flags);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	list_for_each_entry(lport, &nvme_fc_lport_list, port_list) {	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		if (lport->localport.node_name != pinfo->node_name ||	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		    lport->localport.port_name != pinfo->port_name)
			continue;

		if (lport->dev != dev) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			lport = ERR_PTR(-EXDEV);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			goto out_done;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
		}

		if (lport->localport.port_state != FC_OBJSTATE_DELETED) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			lport = ERR_PTR(-EEXIST);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			goto out_done;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
		}

		if (!nvme_fc_lport_get(lport)) {	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
			/*
			 * fails if ref cnt already 0. If so,
			 * act as if lport already deleted
			 */
			lport = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			goto out_done;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
		}

		/* resume the lport */

		lport->ops = ops;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		lport->localport.port_role = pinfo->port_role;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		lport->localport.port_id = pinfo->port_id;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		lport->localport.port_state = FC_OBJSTATE_ONLINE;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		spin_unlock_irqrestore(&nvme_fc_lock, flags);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

		return lport;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}

	lport = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

out_done:
	spin_unlock_irqrestore(&nvme_fc_lock, flags);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	return lport;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

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
int
nvme_fc_register_localport(struct nvme_fc_port_info *pinfo,
			struct nvme_fc_port_template *template,
			struct device *dev,	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
			struct nvme_fc_local_port **portptr)
{
	struct nvme_fc_lport *newrec;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	unsigned long flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int ret, idx;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!template->localport_delete || !template->remoteport_delete ||	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
	    !template->ls_req || !template->fcp_io ||
	    !template->ls_abort || !template->fcp_abort ||
	    !template->max_hw_queues || !template->max_sgl_segments ||
	    !template->max_dif_sgl_segments || !template->dma_boundary) {
		ret = -EINVAL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_reghost_failed;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}

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
	}

	/* nothing found - allocate a new localport struct */

	newrec = kmalloc((sizeof(*newrec) + template->local_priv_sz),	/* [한국어] 커널 메모리 생명주기 */
			 GFP_KERNEL);
	if (!newrec) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = -ENOMEM;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_reghost_failed;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}

	idx = ida_alloc(&nvme_fc_local_port_cnt, GFP_KERNEL);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	if (idx < 0) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = -ENOSPC;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_fail_kfree;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}

	if (!get_device(dev) && dev) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = -ENODEV;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_ida_put;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}

	INIT_LIST_HEAD(&newrec->port_list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	INIT_LIST_HEAD(&newrec->endp_list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	kref_init(&newrec->ref);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	atomic_set(&newrec->act_rport_cnt, 0);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	newrec->ops = template;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	newrec->dev = dev;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ida_init(&newrec->endp_cnt);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (template->local_priv_sz)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		newrec->localport.private = &newrec[1];
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		newrec->localport.private = NULL;
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
		dma_set_seg_boundary(dev, template->dma_boundary);

	*portptr = &newrec->localport;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

out_ida_put:
	ida_free(&nvme_fc_local_port_cnt, idx);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
out_fail_kfree:
	kfree(newrec);	/* [한국어] 커널 메모리 생명주기 */
out_reghost_failed:
	*portptr = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}
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
/*
 * [한국어]
 * nvme_fc_unregister_localport - LLDD 가 로컬 포트(HBA) 소멸을 알려 올 때 불린다
 *
 * @portptr: 사라지는 로컬 포트
 * @return: 0 이면 접수. -EINVAL 이면 NULL 이거나 이미 삭제된 포트다.
 *
 * 원격 포트와 달리 여기에는 dev_loss_tmo 같은 유예가 없다. 우리 쪽 HBA 가
 * 사라지는 것이므로 기다릴 이유가 없기 때문이다.
 *
 * 상태를 DELETED 로 바꾸는 것이 실질적인 조치다. create_ctrl 의 포트 탐색이
 * ONLINE 만 받아들이므로, 이 순간부터 이 포트로는 새 연결이 생기지 않는다.
 *
 * 기존 컨트롤러를 여기서 직접 건드리지 않는 것에 주목할 것. 그것들은 각각
 * 원격 포트에 매달려 있고, LLDD 는 로컬 포트를 내리기 전에 그 아래 원격
 * 포트들을 먼저 내리도록 되어 있다. 그래서 여기 도달할 즈음이면
 * act_rport_cnt 가 0 으로 내려가 있는 것이 정상이다.
 *
 * 참조를 놓는 것이 마지막이며, 그것이 마지막 참조라면 free_lport 가 이어진다.
 *
 * 실행 컨텍스트: LLDD 가 부른다.
 *
 * 호출 체인:
 *   LLDD(HBA 제거) → [이 함수] → nvme_fc_lport_put → nvme_fc_free_lport
 */
int
nvme_fc_unregister_localport(struct nvme_fc_local_port *portptr)
{
	struct nvme_fc_lport *lport = localport_to_lport(portptr);
	unsigned long flags;

	if (!portptr)
		return -EINVAL;

	spin_lock_irqsave(&nvme_fc_lock, flags);

	if (portptr->port_state != FC_OBJSTATE_ONLINE) {	/* [한국어] 이미 삭제 중이면 두 번 처리하지 않는다 */
		spin_unlock_irqrestore(&nvme_fc_lock, flags);
		return -EINVAL;
	}
	portptr->port_state = FC_OBJSTATE_DELETED;	/* [한국어] create_ctrl 의 탐색이 ONLINE 만 받으므로, 이 순간부터 새 연결이 생기지 않는다 */

	spin_unlock_irqrestore(&nvme_fc_lock, flags);

	if (atomic_read(&lport->act_rport_cnt) == 0)	/* [한국어] LLDD 는 로컬 포트 전에 원격 포트를 내리므로 여기서 0 인 것이 정상이다 */
		lport->ops->localport_delete(&lport->localport);	/* [한국어] 남은 것이 없으면 LLDD 에 즉시 정리해도 된다고 알린다 */

	nvme_fc_lport_put(lport);	/* [한국어] 마지막 참조라면 free_lport 가 이어진다 */

	return 0;
}
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

static void
nvme_fc_signal_discovery_scan(struct nvme_fc_lport *lport,
		struct nvme_fc_rport *rport)
{
	char hostaddr[FCNVME_TRADDR_LENGTH];	/* NVMEFC_HOST_TRADDR=...*/	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	char tgtaddr[FCNVME_TRADDR_LENGTH];	/* NVMEFC_TRADDR=...*/	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	char *envp[4] = { "FC_EVENT=nvmediscovery", hostaddr, tgtaddr, NULL };	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!(rport->remoteport.port_role & FC_PORT_ROLE_NVME_DISCOVERY))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	snprintf(hostaddr, sizeof(hostaddr),	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		"NVMEFC_HOST_TRADDR=nn-0x%016llx:pn-0x%016llx",
		lport->localport.node_name, lport->localport.port_name);
	snprintf(tgtaddr, sizeof(tgtaddr),	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		"NVMEFC_TRADDR=nn-0x%016llx:pn-0x%016llx",
		rport->remoteport.node_name, rport->remoteport.port_name);
	kobject_uevent_env(&fc_udev_device->kobj, KOBJ_CHANGE, envp);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}

/*
 * [한국어]
 * nvme_fc_free_rport - 마지막 참조가 사라진 원격 포트 객체를 해제한다
 *
 * @ref: 0 이 된 kref
 * @return: 없음
 *
 * 여기 도달했다는 것은 이 포트를 쓰던 컨트롤러가 모두 사라졌다는 뜻이다.
 * unregister_remoteport 가 참조를 놓은 시점이 아니라, dev_loss_tmo 가 지나
 * 컨트롤러들이 실제로 지워진 뒤에야 이 함수가 불린다.
 *
 * WARN 이 다섯 개나 있는 이유는 이 시점의 가정을 하나씩 검증하기 위해서다.
 * 상태가 DELETED 여야 하고, 컨트롤러 목록과 세 LS 목록이 모두 비어 있어야
 * 한다. 하나라도 남아 있다면 그 목록에 매달린 객체가 해제된 메모리를
 * 가리키게 되므로, 조용히 넘어가지 않고 잡아낸다.
 *
 * 순서가 중요하다. 로컬 포트의 목록에서 먼저 빼야 이후 탐색이 이 객체를
 * 집지 않고, ID 를 반납한 뒤 메모리를 풀며, 마지막에 로컬 포트 참조를
 * 놓는다. 로컬 포트 참조를 먼저 놓으면 lport 가 사라진 뒤 그 ida 를
 * 만지게 된다.
 *
 * 실행 컨텍스트: kref_put 이 부른다 — 마지막 참조를 놓는 쪽의 문맥이다.
 *
 * 호출 체인:
 *   nvme_fc_rport_put → kref_put → [이 함수] → nvme_fc_lport_put
 */
static void
nvme_fc_free_rport(struct kref *ref)
{
	struct nvme_fc_rport *rport =
		container_of(ref, struct nvme_fc_rport, ref);
	struct nvme_fc_lport *lport =
			localport_to_lport(rport->remoteport.localport);	/* [한국어] 마지막에 참조를 놓을 대상 */
	unsigned long flags;

	WARN_ON(rport->remoteport.port_state != FC_OBJSTATE_DELETED);	/* [한국어] 삭제 표시 없이 참조가 0 이 될 수는 없다 */
	WARN_ON(!list_empty(&rport->ctrl_list));	/* [한국어] 컨트롤러가 남아 있으면 그것이 이 객체를 가리킨 채 해제된다 */
	WARN_ON(!list_empty(&rport->ls_req_list));	/* [한국어] 진행 중 LS 가 남아 있으면 완료 콜백이 해제된 포트를 만진다 */
	WARN_ON(!list_empty(&rport->ls_rcv_list));	/* [한국어] 수신 대기 LS 도 마찬가지 */

	/* remove from lport list */
	spin_lock_irqsave(&nvme_fc_lock, flags);
	list_del(&rport->endp_list);	/* [한국어] 먼저 목록에서 빼야 이후 탐색이 이 객체를 집지 않는다 */
	spin_unlock_irqrestore(&nvme_fc_lock, flags);

	WARN_ON(!list_empty(&rport->disc_list));	/* [한국어] 디스커버리 목록도 비어 있어야 한다 */
	ida_free(&lport->endp_cnt, rport->remoteport.port_num);	/* [한국어] 포트 번호를 반납 — lport 참조를 놓기 전에 해야 한다 */

	kfree(rport);

	nvme_fc_lport_put(lport);	/* [한국어] 마지막에 놓는다. 먼저 놓으면 lport 가 사라진 뒤 그 ida 를 만지게 된다 */
}

static void
nvme_fc_rport_put(struct nvme_fc_rport *rport)
{
	kref_put(&rport->ref, nvme_fc_free_rport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
}

static int
nvme_fc_rport_get(struct nvme_fc_rport *rport)
{
	return kref_get_unless_zero(&rport->ref);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

/*
 * [한국어]
 * nvme_fc_resume_controller - 포트가 돌아왔을 때 컨트롤러를 되살린다
 *
 * @ctrl: 연결이 끊겼던 컨트롤러
 * @return: 없음
 *
 * dev_loss_tmo 안에 포트가 다시 등록되면 불린다. 같은 컨트롤러를 살려
 * /dev/nvmeX 가 사라지지 않게 하는 것이 목적이다.
 *
 * 상태에 따라 할 일이 다르다:
 *   - NEW / CONNECTING: 포트가 없어 재연결을 억눌러 두었던 상태다.
 *     이제 시도할 수 있으므로 지연 0 으로 곧바로 연결을 건다.
 *   - RESETTING: 이미 해체가 진행 중이다. 위 영어 주석대로 그 절차가
 *     끝나면 자연스럽게 재연결 단계로 이어지므로 여기서 손댈 것이 없다.
 *     겹쳐 걸면 같은 컨트롤러에 두 개의 연결 시도가 생긴다.
 *   - 그 밖(LIVE, DELETING 등): 되살릴 필요가 없거나 지워지는 중이다.
 *
 * 실행 컨텍스트: register_remoteport(포트 재등록) 경로.
 *
 * 호출 체인:
 *   nvme_fc_register_remoteport → [이 함수] → queue_delayed_work(connect_work)
 */
static void
nvme_fc_resume_controller(struct nvme_fc_ctrl *ctrl)
{
	switch (nvme_ctrl_state(&ctrl->ctrl)) {
	case NVME_CTRL_NEW:
	case NVME_CTRL_CONNECTING:
		/*
		 * As all reconnects were suppressed, schedule a
		 * connect.
		 */
		/* [한국어] 위 영어 주석대로 포트가 없는 동안 재연결을 억눌러 두었다.
		 * 이제 시도할 수 있으므로 곧바로 건다. */
		dev_info(ctrl->ctrl.device,
			"NVME-FC{%d}: connectivity re-established. "
			"Attempting reconnect\n", ctrl->cnum);

		queue_delayed_work(nvme_wq, &ctrl->connect_work, 0);	/* [한국어] 지연 0 — 포트가 막 돌아왔으니 기다릴 이유가 없다 */
		break;

	case NVME_CTRL_RESETTING:
		/*
		 * Controller is already in the process of terminating the
		 * association. No need to do anything further. The reconnect
		 * step will naturally occur after the reset completes.
		 */
		/* [한국어] 위 영어 주석대로 리셋 절차가 끝나면 자연히 재연결로 이어진다.
		 * 여기서 겹쳐 걸면 같은 컨트롤러에 연결 시도가 두 개 생긴다. */
		break;

	default:
		/* no action to take - let it delete */
		/* [한국어] LIVE 면 되살릴 필요가 없고, DELETING 이면 지워지는 중이다 */
		break;
	}
}

static struct nvme_fc_rport *	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
nvme_fc_attach_to_suspended_rport(struct nvme_fc_lport *lport,
				struct nvme_fc_port_info *pinfo)
{
	struct nvme_fc_rport *rport;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_fc_ctrl *ctrl;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	unsigned long flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	spin_lock_irqsave(&nvme_fc_lock, flags);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	list_for_each_entry(rport, &lport->endp_list, endp_list) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (rport->remoteport.node_name != pinfo->node_name ||	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		    rport->remoteport.port_name != pinfo->port_name)
			continue;

		if (!nvme_fc_rport_get(rport)) {	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
			rport = ERR_PTR(-ENOLCK);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			goto out_done;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
		}

		spin_unlock_irqrestore(&nvme_fc_lock, flags);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

		spin_lock_irqsave(&rport->lock, flags);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */

		/* has it been unregistered */
		if (rport->remoteport.port_state != FC_OBJSTATE_DELETED) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			/* means lldd called us twice */
			spin_unlock_irqrestore(&rport->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvme_fc_rport_put(rport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
			return ERR_PTR(-ESTALE);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
		}

		rport->remoteport.port_role = pinfo->port_role;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		rport->remoteport.port_id = pinfo->port_id;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		rport->remoteport.port_state = FC_OBJSTATE_ONLINE;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		rport->dev_loss_end = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		/*
		 * kick off a reconnect attempt on all associations to the
		 * remote port. A successful reconnects will resume i/o.
		 */
		list_for_each_entry(ctrl, &rport->ctrl_list, ctrl_list)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvme_fc_resume_controller(ctrl);

		spin_unlock_irqrestore(&rport->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		return rport;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}

	rport = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

out_done:
	spin_unlock_irqrestore(&nvme_fc_lock, flags);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	return rport;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static inline void
__nvme_fc_set_dev_loss_tmo(struct nvme_fc_rport *rport,
			struct nvme_fc_port_info *pinfo)
{
	if (pinfo->dev_loss_tmo)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		rport->remoteport.dev_loss_tmo = pinfo->dev_loss_tmo;
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		rport->remoteport.dev_loss_tmo = NVME_FC_DEFAULT_DEV_LOSS_TMO;
}

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
int
nvme_fc_register_remoteport(struct nvme_fc_local_port *localport,
				struct nvme_fc_port_info *pinfo,
				struct nvme_fc_remote_port **portptr)
{
	struct nvme_fc_lport *lport = localport_to_lport(localport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_fc_rport *newrec;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	unsigned long flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int ret, idx;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!nvme_fc_lport_get(lport)) {	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		ret = -ESHUTDOWN;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_reghost_failed;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}

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
	}

	/* nothing found - allocate a new remoteport struct */

	newrec = kmalloc((sizeof(*newrec) + lport->ops->remote_priv_sz),	/* [한국어] 커널 메모리 생명주기 */
			 GFP_KERNEL);
	if (!newrec) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = -ENOMEM;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_lport_put;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}

	idx = ida_alloc(&lport->endp_cnt, GFP_KERNEL);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (idx < 0) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = -ENOSPC;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_kfree_rport;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}

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
		newrec->remoteport.private = &newrec[1];
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		newrec->remoteport.private = NULL;
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

out_kfree_rport:
	kfree(newrec);	/* [한국어] 커널 메모리 생명주기 */
out_lport_put:
	nvme_fc_lport_put(lport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
out_reghost_failed:
	*portptr = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}
EXPORT_SYMBOL_GPL(nvme_fc_register_remoteport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

/*
 * [한국어]
 * nvme_fc_abort_lsops - 이 포트에서 진행 중인 모든 LS 를 취소한다
 *
 * @rport: 사라지는 원격 포트
 * @return: 항상 0
 *
 * 포트가 없어지면 그 포트로 보낸 LS 의 응답은 영영 오지 않는다. 그런데
 * 세션 수립 절차는 ls_done 에서 잠들어 그 응답을 기다리고 있다. 취소하지
 * 않으면 그쪽이 영원히 깨어나지 못한다. 그래서 포트 소멸 경로가 반드시
 * 이 함수를 지난다.
 *
 * goto restart 구조는 handle_ls_rqst_work 와 같은 이유다. ls_abort 는
 * LLDD 호출이라 락을 들고 부를 수 없고, 놓는 순간 목록이 바뀔 수 있어
 * 순회를 이어 갈 수 없다. TERMIO 비트가 "이미 취소를 건 것"을 표시해
 * 다시 훑을 때 건너뛰게 한다.
 *
 * 그 비트를 락 안에서 세우는 것이 중요하다. 두 경로가 동시에 같은 LS 를
 * 취소하려 하면 LLDD 에 abort 가 두 번 들어간다.
 *
 * 실행 컨텍스트: unregister_remoteport. 스핀락을 놓고 LLDD 를 부른다.
 *
 * 호출 체인:
 *   nvme_fc_unregister_remoteport → [이 함수] → LLDD 의 ls_abort
 */
static int
nvme_fc_abort_lsops(struct nvme_fc_rport *rport)
{
	struct nvmefc_ls_req_op *lsop;
	unsigned long flags;

restart:	/* [한국어] 락을 놓고 취소한 뒤에는 목록이 바뀌었을 수 있어 처음부터 다시 훑는다 */
	spin_lock_irqsave(&rport->lock, flags);

	list_for_each_entry(lsop, &rport->ls_req_list, lsreq_list) {
		if (!(lsop->flags & FCOP_FLAGS_TERMIO)) {	/* [한국어] 아직 취소를 걸지 않은 것만 */
			lsop->flags |= FCOP_FLAGS_TERMIO;	/* [한국어] 락 안에서 표시해야 두 경로가 같은 LS 를 두 번 취소하지 않는다 */
			spin_unlock_irqrestore(&rport->lock, flags);	/* [한국어] LLDD 호출은 락을 들고 할 수 없다 */
			rport->lport->ops->ls_abort(&rport->lport->localport,	/* [한국어] 응답이 영영 오지 않을 LS 를 끝낸다 — 기다리던 쪽이 깨어난다 */
						&rport->remoteport,
						&lsop->ls_req);
			goto restart;	/* [한국어] 락을 놓았으므로 순회를 이어 갈 수 없다 */
		}
	}
	spin_unlock_irqrestore(&rport->lock, flags);

	return 0;
}

static void
nvme_fc_ctrl_connectivity_loss(struct nvme_fc_ctrl *ctrl)
{
	dev_info(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		"NVME-FC{%d}: controller connectivity lost. Awaiting "
		"Reconnect", ctrl->cnum);

	set_bit(ASSOC_FAILED, &ctrl->flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_reset_ctrl(&ctrl->ctrl);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
}

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
/*
 * [한국어]
 * nvme_fc_unregister_remoteport - LLDD 가 원격 포트 소멸을 알려 올 때 불린다
 *
 * @portptr: 사라진 원격 포트
 * @return: 0 이면 접수. -EINVAL 이면 NULL 이거나 이미 삭제된 포트다.
 *
 * FC 에서 포트가 사라지는 것은 흔한 일이다. 케이블을 뽑거나 스위치가
 * 재구성되거나 존 설정이 바뀌면 곧바로 이 경로가 불린다. 그래서 여기서
 * 컨트롤러를 즉시 지우지 않는 것이 이 함수의 핵심 판단이다.
 *
 * dev_loss_tmo 가 그 유예 시간이다. 0 이면 즉시 포기하고 컨트롤러를 지우지만,
 * 그 밖에는 dev_loss_end 시각을 기록해 두고 connectivity_loss 로만 알린다.
 * 그 시각 전에 포트가 돌아오면 같은 컨트롤러가 재연결로 살아나고, 넘기면
 * 그때 지운다. 이 유예가 없으면 케이블을 잠깐 건드릴 때마다 /dev/nvmeX 가
 * 사라져 상위 파일시스템이 I/O 오류를 본다.
 *
 * port_state 를 DELETED 로 먼저 바꾸는 것이 중요하다. 이 값이 queue_rq 와
 * start_fcp_op 의 첫 검사 대상이므로, 바꾸는 즉시 새 I/O 가 매체 없는
 * 경로로 나가는 것을 막는다.
 *
 * abort_lsops 를 부르는 이유: 응답이 영영 오지 않을 LS 를 취소해야 한다.
 * 그러지 않으면 ls_done 을 기다리는 쪽이 영원히 잠든다.
 *
 * 마지막 참조를 놓는 것에 대해서는 위 영어 주석이 설명한다 -- 컨트롤러들이
 * 모두 사라진 뒤에야(즉 dev_loss_tmo 가 지난 뒤에야) rport 가 실제로 해제된다.
 *
 * 실행 컨텍스트: LLDD 가 부른다. rport->lock 이 스핀락이라 그 안에서는 잠들 수 없다.
 *
 * 호출 체인:
 *   LLDD(포트 소멸 감지) → [이 함수] → nvme_fc_ctrl_connectivity_loss / nvme_delete_ctrl
 */
int
nvme_fc_unregister_remoteport(struct nvme_fc_remote_port *portptr)
{
	struct nvme_fc_rport *rport = remoteport_to_rport(portptr);	/* [한국어] 공개 부분에서 우리 객체를 되찾는다 */
	struct nvme_fc_ctrl *ctrl;
	unsigned long flags;

	if (!portptr)
		return -EINVAL;

	spin_lock_irqsave(&rport->lock, flags);	/* [한국어] LS 수신 콜백이 인터럽트 문맥일 수 있어 irqsave */

	if (portptr->port_state != FC_OBJSTATE_ONLINE) {	/* [한국어] 이미 삭제 중이면 두 번 처리하지 않는다 */
		spin_unlock_irqrestore(&rport->lock, flags);
		return -EINVAL;
	}
	portptr->port_state = FC_OBJSTATE_DELETED;	/* [한국어] 이 한 줄이 새 I/O 를 막는다 — queue_rq 와 start_fcp_op 의 첫 검사 대상이다 */

	rport->dev_loss_end = jiffies + (portptr->dev_loss_tmo * HZ);	/* [한국어] 이 시각까지는 포트가 돌아오기를 기다린다 */

	list_for_each_entry(ctrl, &rport->ctrl_list, ctrl_list) {	/* [한국어] 이 포트에 매달린 컨트롤러 모두에게 알린다 */
		/* if dev_loss_tmo==0, dev loss is immediate */
		if (!portptr->dev_loss_tmo) {	/* [한국어] 유예 없이 포기하도록 설정된 경우 */
			dev_warn(ctrl->ctrl.device,
				"NVME-FC{%d}: controller connectivity lost.\n",
				ctrl->cnum);
			nvme_delete_ctrl(&ctrl->ctrl);	/* [한국어] 곧바로 지운다 — /dev/nvmeX 가 사라진다 */
		} else
			nvme_fc_ctrl_connectivity_loss(ctrl);	/* [한국어] 유예가 있으면 재연결 대기로만 넘긴다. 돌아오면 같은 컨트롤러가 살아난다 */
	}

	spin_unlock_irqrestore(&rport->lock, flags);

	nvme_fc_abort_lsops(rport);	/* [한국어] 응답이 영영 오지 않을 LS 를 취소한다 — 없으면 기다리는 쪽이 영원히 잠든다 */

	if (atomic_read(&rport->act_ctrl_cnt) == 0)	/* [한국어] 이 포트를 쓰는 활성 컨트롤러가 하나도 없으면 */
		rport->lport->ops->remoteport_delete(portptr);	/* [한국어] LLDD 에 즉시 정리해도 된다고 알린다 */

	/*
	 * release the reference, which will allow, if all controllers
	 * go away, which should only occur after dev_loss_tmo occurs,
	 * for the rport to be torn down.
	 */
	/* [한국어] 위 영어 주석대로, 컨트롤러들이 모두 사라진 뒤에야 — 즉
	 * dev_loss_tmo 가 지난 뒤에야 — 이 참조 해제가 실제 해제로 이어진다. */
	nvme_fc_rport_put(rport);

	return 0;
}
EXPORT_SYMBOL_GPL(nvme_fc_unregister_remoteport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

/**
 * nvme_fc_rescan_remoteport - transport entry point called by an
 *                              LLDD to request a nvme device rescan.
 * @remoteport: pointer to the (registered) remote port that is to be
 *              rescanned.
 *
 * Returns: N/A
 */
void
nvme_fc_rescan_remoteport(struct nvme_fc_remote_port *remoteport)
{
	struct nvme_fc_rport *rport = remoteport_to_rport(remoteport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	nvme_fc_signal_discovery_scan(rport->lport, rport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
}
EXPORT_SYMBOL_GPL(nvme_fc_rescan_remoteport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

int
nvme_fc_set_remoteport_devloss(struct nvme_fc_remote_port *portptr,
			u32 dev_loss_tmo)
{
	struct nvme_fc_rport *rport = remoteport_to_rport(portptr);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	unsigned long flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	spin_lock_irqsave(&rport->lock, flags);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */

	if (portptr->port_state != FC_OBJSTATE_ONLINE) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		spin_unlock_irqrestore(&rport->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return -EINVAL;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}

	/* a dev_loss_tmo of 0 (immediate) is allowed to be set */
	rport->remoteport.dev_loss_tmo = dev_loss_tmo;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	spin_unlock_irqrestore(&rport->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}
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

static inline dma_addr_t
fc_dma_map_single(struct device *dev, void *ptr, size_t size,
		enum dma_data_direction dir)
{
	return dev ? dma_map_single(dev, ptr, size, dir) : (dma_addr_t)0L;	/* [한국어] DMA 매핑 — 장치가 접근할 주소 확보 */
}

static inline int
fc_dma_mapping_error(struct device *dev, dma_addr_t dma_addr)
{
	return dev ? dma_mapping_error(dev, dma_addr) : 0;	/* [한국어] DMA 매핑 — 장치가 접근할 주소 확보 */
}

static inline void
fc_dma_unmap_single(struct device *dev, dma_addr_t addr, size_t size,
	enum dma_data_direction dir)
{
	if (dev)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dma_unmap_single(dev, addr, size, dir);
}

static inline void
fc_dma_sync_single_for_cpu(struct device *dev, dma_addr_t addr, size_t size,
		enum dma_data_direction dir)
{
	if (dev)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dma_sync_single_for_cpu(dev, addr, size, dir);
}

static inline void
fc_dma_sync_single_for_device(struct device *dev, dma_addr_t addr, size_t size,
		enum dma_data_direction dir)
{
	if (dev)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dma_sync_single_for_device(dev, addr, size, dir);
}

/* pseudo dma_map_sg call */
static int
fc_map_sg(struct scatterlist *sg, int nents)	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
{
	struct scatterlist *s;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	int i;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	WARN_ON(nents == 0 || sg[0].length == 0);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	for_each_sg(sg, s, nents, i) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		s->dma_address = 0L;	/* [한국어] DMA 매핑 — 장치가 접근할 주소 확보 */
#ifdef CONFIG_NEED_SG_DMA_LENGTH	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		s->dma_length = s->length;	/* [한국어] DMA 매핑 — 장치가 접근할 주소 확보 */
#endif	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}
	return nents;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static inline int
fc_dma_map_sg(struct device *dev, struct scatterlist *sg, int nents,	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
		enum dma_data_direction dir)
{
	return dev ? dma_map_sg(dev, sg, nents, dir) : fc_map_sg(sg, nents);	/* [한국어] DMA 매핑 — 장치가 접근할 주소 확보 */
}

static inline void
fc_dma_unmap_sg(struct device *dev, struct scatterlist *sg, int nents,	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
		enum dma_data_direction dir)
{
	if (dev)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dma_unmap_sg(dev, sg, nents, dir);
}

/* *********************** FC-NVME LS Handling **************************** */

static void nvme_fc_ctrl_put(struct nvme_fc_ctrl *);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
static int nvme_fc_ctrl_get(struct nvme_fc_ctrl *);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

static void nvme_fc_error_recovery(struct nvme_fc_ctrl *ctrl, char *errmsg);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

/*
 * [한국어]
 * __nvme_fc_finish_ls_req - 보낸 LS 하나의 자원을 되돌린다
 *
 * @lsop: 완료되었거나 취소된 LS
 * @return: 없음
 *
 * send_ls_req 가 잡은 것들을 정확히 역순으로 푼다 -- 목록에서 빼고, DMA 를
 * 풀고, 포트 참조를 놓는다.
 *
 * req_queued 검사가 이 함수의 핵심이다. 완료 콜백과 취소 경로(abort_lsops
 * 뒤의 완료)가 같은 LS 에 대해 둘 다 이 함수에 도달할 수 있는데, 두 번 풀면
 * 목록이 깨지고 참조가 과도하게 내려간다. 그래서 락 안에서 그 플래그를 보고
 * 내리는 쪽만 통과시킨다. 플래그 자체가 "내가 정리 책임자인가"의 판정이다.
 *
 * 포트 참조를 DMA 해제 뒤에 놓는 것도 의도적이다. rport->dev 가 매핑을
 * 푸는 데 필요하므로, 먼저 놓으면 그 사이 포트가 사라질 수 있다.
 *
 * 실행 컨텍스트: LS 완료 콜백. 인터럽트 문맥일 수 있어 irqsave 를 쓴다.
 *
 * 호출 체인:
 *   nvme_fc_send_ls_req 의 완료 콜백 / _ls_req_done → [이 함수]
 */
static void
__nvme_fc_finish_ls_req(struct nvmefc_ls_req_op *lsop)
{
	struct nvme_fc_rport *rport = lsop->rport;
	struct nvmefc_ls_req *lsreq = &lsop->ls_req;
	unsigned long flags;

	spin_lock_irqsave(&rport->lock, flags);

	if (!lsop->req_queued) {	/* [한국어] 이미 다른 경로가 정리했다 */
		spin_unlock_irqrestore(&rport->lock, flags);
		return;	/* [한국어] 두 번 풀면 목록이 깨지고 참조가 과도하게 내려간다 */
	}

	list_del(&lsop->lsreq_list);

	lsop->req_queued = false;	/* [한국어] 이 플래그가 "내가 정리 책임자인가"의 판정 그 자체다 */

	spin_unlock_irqrestore(&rport->lock, flags);

	fc_dma_unmap_single(rport->dev, lsreq->rqstdma,	/* [한국어] 요청·응답을 한 번에 잡았으므로 한 번에 푼다 */
				  (lsreq->rqstlen + lsreq->rsplen),
				  DMA_BIDIRECTIONAL);

	nvme_fc_rport_put(rport);	/* [한국어] DMA 해제 뒤에 놓는다 — rport->dev 가 그때까지 필요하다 */
}

/*
 * [한국어]
 * __nvme_fc_send_ls_req - Link Service 요청 하나를 LLDD 에 넘긴다
 *
 * @rport: 이 LS 를 보낼 원격 포트
 * @lsop:  요청 서술자. 호출자가 요청 내용은 이미 채워 두었다.
 * @done:  LLDD 가 완료를 알릴 때 부를 콜백
 * @return: 0 이면 LLDD 가 접수. 음수면 보내지 못했고 잡은 자원은 모두 되돌렸다.
 *
 * LS 는 제어 평면 메시지다 -- Create Association, Create Connection,
 * Disconnect 가 이 경로로 오간다. 데이터 I/O 와 완전히 별개의 흐름이며,
 * 세션이 서기 전에 오가는 것이라 태그셋도 큐도 아직 없다.
 *
 * 이 함수가 신경 쓰는 것은 사실상 수명이다. LS 를 내보낸 뒤 응답이 오기까지
 * 그 사이에 원격 포트가 사라질 수 있기 때문에:
 *   - 포트 참조를 하나 올려 두고(완료 경로가 내린다),
 *   - 요청을 rport->ls_req_list 에 걸어 둔다.
 * 목록에 거는 이유는 포트가 사라질 때 "응답이 영영 오지 않을 LS"를 찾아
 * 취소하기 위해서다. 그러지 않으면 ls_done 을 기다리는 쪽이 영원히 잠든다.
 *
 * DMA 매핑이 요청과 응답을 한 번에 잡는 것에 주목할 것. 두 버퍼가 연속으로
 * 놓여 있어 한 번의 매핑으로 덮고, rspdma 는 그 안의 오프셋으로 구한다.
 * BIDIRECTIONAL 인 이유도 같다 -- 앞쪽은 우리가 써서 HBA 가 읽고, 뒤쪽은
 * HBA 가 써서 우리가 읽는다.
 *
 * 목록에 먼저 걸고 ls_req 를 부르는 순서가 중요하다. 반대로 하면 응답이
 * 목록에 걸리기 전에 도착할 수 있고, 완료 경로가 찾지 못한다.
 *
 * 실행 컨텍스트: 세션 수립·해체 경로. rport->lock 은 스핀락이라 그 안에서는
 * 잠들 수 없다.
 *
 * 호출 체인:
 *   nvme_fc_create_association / _delete_association
 *     → [이 함수] → LLDD 의 ls_req 콜백 → FC 프레임
 */
static int
__nvme_fc_send_ls_req(struct nvme_fc_rport *rport,
		struct nvmefc_ls_req_op *lsop,
		void (*done)(struct nvmefc_ls_req *req, int status))
{
	struct nvmefc_ls_req *lsreq = &lsop->ls_req;	/* [한국어] LLDD 에 넘길 공개 부분 */
	unsigned long flags;
	int ret = 0;

	if (rport->remoteport.port_state != FC_OBJSTATE_ONLINE)	/* [한국어] 포트가 사라졌으면 보낼 매체가 없다 */
		return -ECONNREFUSED;

	if (!nvme_fc_rport_get(rport))	/* [한국어] 응답이 올 때까지 포트가 살아 있도록 참조를 든다. 이미 해제 중이면 실패한다 */
		return -ESHUTDOWN;

	lsreq->done = done;	/* [한국어] LLDD 가 완료 시 부를 콜백 */
	lsop->rport = rport;
	lsop->req_queued = false;	/* [한국어] 아직 목록에 걸지 않았다 — 실패 경로가 이 값을 보고 뺄지 판단한다 */
	INIT_LIST_HEAD(&lsop->lsreq_list);
	init_completion(&lsop->ls_done);	/* [한국어] 호출자가 여기서 잠들어 응답을 기다린다 */

	lsreq->rqstdma = fc_dma_map_single(rport->dev, lsreq->rqstaddr,	/* [한국어] 요청과 응답 버퍼가 연속이라 한 번에 매핑한다 */
				  lsreq->rqstlen + lsreq->rsplen,
				  DMA_BIDIRECTIONAL);	/* [한국어] 앞쪽은 HBA 가 읽고 뒤쪽은 HBA 가 쓴다 — 방향이 둘 다 필요하다 */
	if (fc_dma_mapping_error(rport->dev, lsreq->rqstdma)) {
		ret = -EFAULT;
		goto out_putrport;
	}
	lsreq->rspdma = lsreq->rqstdma + lsreq->rqstlen;	/* [한국어] 응답 버퍼는 같은 매핑 안의 오프셋으로 구한다 */

	spin_lock_irqsave(&rport->lock, flags);	/* [한국어] LS 수신 콜백이 인터럽트 문맥일 수 있어 irqsave 가 필요하다 */

	list_add_tail(&lsop->lsreq_list, &rport->ls_req_list);	/* [한국어] 포트가 사라질 때 취소 대상으로 찾을 수 있도록 걸어 둔다 */

	lsop->req_queued = true;

	spin_unlock_irqrestore(&rport->lock, flags);

	ret = rport->lport->ops->ls_req(&rport->lport->localport,	/* [한국어] 여기서부터 프레임 송신은 LLDD 의 몫이다 */
					&rport->remoteport, lsreq);
	if (ret)
		goto out_unlink;

	return 0;

out_unlink:	/* [한국어] LLDD 가 접수를 거부했다 — 목록에서 빼고 매핑을 되돌린다 */
	lsop->ls_error = ret;
	spin_lock_irqsave(&rport->lock, flags);
	lsop->req_queued = false;	/* [한국어] 취소 경로가 두 번 빼지 않도록 표시부터 내린다 */
	list_del(&lsop->lsreq_list);
	spin_unlock_irqrestore(&rport->lock, flags);
	fc_dma_unmap_single(rport->dev, lsreq->rqstdma,
				  (lsreq->rqstlen + lsreq->rsplen),
				  DMA_BIDIRECTIONAL);
out_putrport:	/* [한국어] 맨 처음 든 포트 참조를 놓는다 */
	nvme_fc_rport_put(rport);

	return ret;
}

static void
nvme_fc_send_ls_req_done(struct nvmefc_ls_req *lsreq, int status)
{
	struct nvmefc_ls_req_op *lsop = ls_req_to_lsop(lsreq);	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	lsop->ls_error = status;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	complete(&lsop->ls_done);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}

static int
nvme_fc_send_ls_req(struct nvme_fc_rport *rport, struct nvmefc_ls_req_op *lsop)
{
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
	}

	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	/* ACC or RJT payload ? */
	if (rjt->w0.ls_cmd == FCNVME_LS_RJT)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return -ENXIO;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static int
nvme_fc_send_ls_req_async(struct nvme_fc_rport *rport,
		struct nvmefc_ls_req_op *lsop,	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
		void (*done)(struct nvmefc_ls_req *req, int status))
{
	/* don't wait for completion */

	return __nvme_fc_send_ls_req(rport, lsop, done);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

/*
 * [한국어]
 * nvme_fc_connect_admin_queue - Create Association LS 를 보내 세션을 개설한다
 *
 * @ctrl:       세션을 세울 컨트롤러
 * @queue:      admin 큐(0번). 여기에 connection_id 가 채워진다.
 * @qsize:      큐 깊이. LS 에는 sqsize(0-based)로 담긴다.
 * @ersp_ratio: 확장 응답(ERSP)을 몇 번에 한 번 받을지에 대한 힌트.
 *              타겟이 매 완료마다 전체 응답 IU 를 보내지 않아도 되게 해
 *              링크 대역폭을 아낀다.
 * @return: 0 이면 association_id 와 connection_id 를 받았다. 음수면 실패.
 *
 * FC 세션의 첫 대화다. 이 LS 하나가 두 가지를 한꺼번에 얻어 온다 --
 * association_id(세션 전체의 식별자)와 admin 큐의 connection_id. 이후
 * I/O 큐들은 Create Connection LS 로 각자의 connection_id 만 추가로 받는다.
 *
 * 할당이 한 번에 이뤄지는 구조를 눈여겨볼 것. lsop 뒤에 요청 버퍼, 응답 버퍼,
 * 그리고 LLDD 전용 영역이 차례로 붙어 하나의 kzalloc 으로 처리된다. 그래서
 * 아래에서 &lsop[1], &assoc_rqst[1] 같은 포인터 산술로 각 영역을 가른다.
 * 해제도 kfree(lsop) 하나로 끝난다.
 *
 * cntlid 에 0xffff 를 넣는 이유는 위 영어 주석이 밝힌다 -- 리눅스는 동적
 * 컨트롤러만 지원하므로, 특정 컨트롤러 ID 를 요구하지 않고 타겟이 배정하게 한다.
 *
 * 응답 검증이 열 갈래로 늘어선 이유: 이것은 원격이 보낸 데이터이고, 여기서
 * 얻는 association_id 와 connection_id 는 이후 모든 명령에 실린다. 형식이
 * 어긋난 응답을 그대로 믿으면 잘못된 식별자로 세션을 세우게 되고, 그 오류는
 * 훨씬 뒤에 엉뚱한 모습으로 드러난다. 그래서 각 서술자의 태그와 길이를
 * 하나씩 대조하고, 실패 시 어느 검사에서 걸렸는지 이름으로 남긴다.
 *
 * 실행 컨텍스트: create_association 경로. LS 응답을 기다리며 잠든다.
 *
 * 호출 체인:
 *   nvme_fc_create_association → [이 함수] → nvme_fc_send_ls_req → LLDD
 */
static int
nvme_fc_connect_admin_queue(struct nvme_fc_ctrl *ctrl,
	struct nvme_fc_queue *queue, u16 qsize, u16 ersp_ratio)
{
	struct nvmefc_ls_req_op *lsop;
	struct nvmefc_ls_req *lsreq;
	struct fcnvme_ls_cr_assoc_rqst *assoc_rqst;	/* [한국어] 보낼 요청 */
	struct fcnvme_ls_cr_assoc_acc *assoc_acc;	/* [한국어] 받을 응답 */
	unsigned long flags;
	int ret, fcret = 0;	/* [한국어] fcret 은 응답 검증 실패 지점을 가리키는 별도 코드 */

	lsop = kzalloc((sizeof(*lsop) +	/* [한국어] 서술자와 두 버퍼, LLDD 전용 영역을 한 덩어리로 잡는다 */
			 sizeof(*assoc_rqst) + sizeof(*assoc_acc) +
			 ctrl->lport->ops->lsrqst_priv_sz), GFP_KERNEL);	/* [한국어] LLDD 가 요구하는 사적 공간 크기는 드라이버마다 다르다 */
	if (!lsop) {
		dev_info(ctrl->ctrl.device,
			"NVME-FC{%d}: send Create Association failed: ENOMEM\n",
			ctrl->cnum);
		ret = -ENOMEM;
		goto out_no_memory;
	}

	assoc_rqst = (struct fcnvme_ls_cr_assoc_rqst *)&lsop[1];	/* [한국어] 서술자 바로 뒤가 요청 버퍼 */
	assoc_acc = (struct fcnvme_ls_cr_assoc_acc *)&assoc_rqst[1];	/* [한국어] 그 뒤가 응답 버퍼 */
	lsreq = &lsop->ls_req;
	if (ctrl->lport->ops->lsrqst_priv_sz)
		lsreq->private = &assoc_acc[1];	/* [한국어] 마지막 영역을 LLDD 에 넘긴다 — 내용은 이 계층이 해석하지 않는다 */
	else
		lsreq->private = NULL;

	assoc_rqst->w0.ls_cmd = FCNVME_LS_CREATE_ASSOCIATION;	/* [한국어] 세션 개설 요청 */
	assoc_rqst->desc_list_len =
			cpu_to_be32(sizeof(struct fcnvme_lsdesc_cr_assoc_cmd));	/* [한국어] FC 는 빅엔디안이다 */

	assoc_rqst->assoc_cmd.desc_tag =
			cpu_to_be32(FCNVME_LSDESC_CREATE_ASSOC_CMD);	/* [한국어] 서술자 종류 태그. 상대는 이 값으로 내용을 해석한다 */
	assoc_rqst->assoc_cmd.desc_len =
			fcnvme_lsdesc_len(
				sizeof(struct fcnvme_lsdesc_cr_assoc_cmd));

	assoc_rqst->assoc_cmd.ersp_ratio = cpu_to_be16(ersp_ratio);	/* [한국어] 전체 응답 IU 를 몇 번에 한 번 보낼지 — 링크 대역폭 절약 힌트 */
	assoc_rqst->assoc_cmd.sqsize = cpu_to_be16(qsize - 1);	/* [한국어] sqsize 는 0-based 라 하나 뺀다 */
	/* Linux supports only Dynamic controllers */
	assoc_rqst->assoc_cmd.cntlid = cpu_to_be16(0xffff);	/* [한국어] 위 영어 주석대로 동적 컨트롤러만 지원하므로 타겟이 ID 를 배정하게 한다 */
	uuid_copy(&assoc_rqst->assoc_cmd.hostid, &ctrl->ctrl.opts->host->id);	/* [한국어] 호스트 식별자 — 타겟이 접근 제어에 쓴다 */
	strscpy(assoc_rqst->assoc_cmd.hostnqn, ctrl->ctrl.opts->host->nqn,
		sizeof(assoc_rqst->assoc_cmd.hostnqn));
	strscpy(assoc_rqst->assoc_cmd.subnqn, ctrl->ctrl.opts->subsysnqn,	/* [한국어] 접속할 서브시스템 NQN */
		sizeof(assoc_rqst->assoc_cmd.subnqn));

	lsop->queue = queue;
	lsreq->rqstaddr = assoc_rqst;	/* [한국어] 아래 send_ls_req 가 이 두 버퍼를 한 번에 DMA 매핑한다 */
	lsreq->rqstlen = sizeof(*assoc_rqst);
	lsreq->rspaddr = assoc_acc;
	lsreq->rsplen = sizeof(*assoc_acc);
	lsreq->timeout = NVME_FC_LS_TIMEOUT_SEC;	/* [한국어] 응답이 없으면 이 시간 뒤 실패로 처리된다 */

	ret = nvme_fc_send_ls_req(ctrl->rport, lsop);	/* [한국어] 보내고 응답을 기다린다(내부에서 ls_done 대기) */
	if (ret)
		goto out_free_buffer;

	/* process connect LS completion */

	/* validate the ACC response */
	/* [한국어] 아래 검증이 열 갈래인 이유: 여기서 얻는 두 식별자가 이후 모든
	 * 명령에 실린다. 어긋난 응답을 믿으면 잘못된 세션이 서고, 그 오류는 훨씬
	 * 뒤에 엉뚱한 모습으로 드러난다. 그래서 서술자마다 태그와 길이를 대조한다. */
	if (assoc_acc->hdr.w0.ls_cmd != FCNVME_LS_ACC)	/* [한국어] 수락이 아니라 거절이 왔다 */
		fcret = VERR_LSACC;
	else if (assoc_acc->hdr.desc_list_len !=	/* [한국어] 전체 서술자 길이가 기대와 다르다 */
			fcnvme_lsdesc_len(
				sizeof(struct fcnvme_ls_cr_assoc_acc)))
		fcret = VERR_CR_ASSOC_ACC_LEN;
	else if (assoc_acc->hdr.rqst.desc_tag !=	/* [한국어] 응답이 어떤 요청에 대한 것인지 밝히는 서술자가 없다 */
			cpu_to_be32(FCNVME_LSDESC_RQST))
		fcret = VERR_LSDESC_RQST;
	else if (assoc_acc->hdr.rqst.desc_len !=
			fcnvme_lsdesc_len(sizeof(struct fcnvme_lsdesc_rqst)))
		fcret = VERR_LSDESC_RQST_LEN;
	else if (assoc_acc->hdr.rqst.w0.ls_cmd != FCNVME_LS_CREATE_ASSOCIATION)	/* [한국어] 우리가 보낸 것과 다른 요청에 대한 응답이다 */
		fcret = VERR_CR_ASSOC;
	else if (assoc_acc->associd.desc_tag !=	/* [한국어] association_id 서술자가 없다 — 세션 식별자를 못 받는다 */
			cpu_to_be32(FCNVME_LSDESC_ASSOC_ID))
		fcret = VERR_ASSOC_ID;
	else if (assoc_acc->associd.desc_len !=
			fcnvme_lsdesc_len(
				sizeof(struct fcnvme_lsdesc_assoc_id)))
		fcret = VERR_ASSOC_ID_LEN;
	else if (assoc_acc->connectid.desc_tag !=	/* [한국어] connection_id 서술자가 없다 — admin 큐를 쓸 수 없다 */
			cpu_to_be32(FCNVME_LSDESC_CONN_ID))
		fcret = VERR_CONN_ID;
	else if (assoc_acc->connectid.desc_len !=
			fcnvme_lsdesc_len(sizeof(struct fcnvme_lsdesc_conn_id)))
		fcret = VERR_CONN_ID_LEN;

	if (fcret) {
		ret = -EBADF;
		dev_err(ctrl->dev,	/* [한국어] 어느 검사에서 걸렸는지 이름으로 남긴다 — 상호운용 문제 추적의 출발점이다 */
			"q %d Create Association LS failed: %s\n",
			queue->qnum, validation_errors[fcret]);
	} else {
		spin_lock_irqsave(&ctrl->lock, flags);
		ctrl->association_id =	/* [한국어] 세션 전체의 식별자. 이후 모든 큐가 이 아래 매달린다 */
			be64_to_cpu(assoc_acc->associd.association_id);
		queue->connection_id =	/* [한국어] admin 큐 전용 채널 식별자 */
			be64_to_cpu(assoc_acc->connectid.connection_id);
		set_bit(NVME_FC_Q_CONNECTED, &queue->flags);	/* [한국어] FC 채널이 열렸다. NVMe Connect 는 아직이다 */
		spin_unlock_irqrestore(&ctrl->lock, flags);
	}

out_free_buffer:
	kfree(lsop);	/* [한국어] 한 덩어리로 잡았으므로 해제도 하나면 된다 */
out_no_memory:
	if (ret)
		dev_err(ctrl->dev,
			"queue %d connect admin queue failed (%d).\n",
			queue->qnum, ret);
	return ret;
}

static int
nvme_fc_connect_queue(struct nvme_fc_ctrl *ctrl, struct nvme_fc_queue *queue,
			u16 qsize, u16 ersp_ratio)
{
	struct nvmefc_ls_req_op *lsop;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct nvmefc_ls_req *lsreq;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct fcnvme_ls_cr_conn_rqst *conn_rqst;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct fcnvme_ls_cr_conn_acc *conn_acc;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	int ret, fcret = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	lsop = kzalloc((sizeof(*lsop) +	/* [한국어] 커널 메모리 생명주기 */
			 sizeof(*conn_rqst) + sizeof(*conn_acc) +
			 ctrl->lport->ops->lsrqst_priv_sz), GFP_KERNEL);
	if (!lsop) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_info(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"NVME-FC{%d}: send Create Connection failed: ENOMEM\n",
			ctrl->cnum);
		ret = -ENOMEM;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_no_memory;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}

	conn_rqst = (struct fcnvme_ls_cr_conn_rqst *)&lsop[1];	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	conn_acc = (struct fcnvme_ls_cr_conn_acc *)&conn_rqst[1];	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	lsreq = &lsop->ls_req;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ctrl->lport->ops->lsrqst_priv_sz)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		lsreq->private = (void *)&conn_acc[1];
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		lsreq->private = NULL;

	conn_rqst->w0.ls_cmd = FCNVME_LS_CREATE_CONNECTION;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	conn_rqst->desc_list_len = cpu_to_be32(	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				sizeof(struct fcnvme_lsdesc_assoc_id) +	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
				sizeof(struct fcnvme_lsdesc_cr_conn_cmd));	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	conn_rqst->associd.desc_tag = cpu_to_be32(FCNVME_LSDESC_ASSOC_ID);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	conn_rqst->associd.desc_len =	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			fcnvme_lsdesc_len(
				sizeof(struct fcnvme_lsdesc_assoc_id));	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	conn_rqst->associd.association_id = cpu_to_be64(ctrl->association_id);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	conn_rqst->connect_cmd.desc_tag =	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			cpu_to_be32(FCNVME_LSDESC_CREATE_CONN_CMD);
	conn_rqst->connect_cmd.desc_len =	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			fcnvme_lsdesc_len(
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
		fcret = VERR_LSACC;
	else if (conn_acc->hdr.desc_list_len !=	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			fcnvme_lsdesc_len(sizeof(struct fcnvme_ls_cr_conn_acc)))	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
		fcret = VERR_CR_CONN_ACC_LEN;
	else if (conn_acc->hdr.rqst.desc_tag != cpu_to_be32(FCNVME_LSDESC_RQST))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		fcret = VERR_LSDESC_RQST;
	else if (conn_acc->hdr.rqst.desc_len !=	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			fcnvme_lsdesc_len(sizeof(struct fcnvme_lsdesc_rqst)))	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
		fcret = VERR_LSDESC_RQST_LEN;
	else if (conn_acc->hdr.rqst.w0.ls_cmd != FCNVME_LS_CREATE_CONNECTION)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		fcret = VERR_CR_CONN;
	else if (conn_acc->connectid.desc_tag !=	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			cpu_to_be32(FCNVME_LSDESC_CONN_ID))
		fcret = VERR_CONN_ID;
	else if (conn_acc->connectid.desc_len !=	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			fcnvme_lsdesc_len(sizeof(struct fcnvme_lsdesc_conn_id)))	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
		fcret = VERR_CONN_ID_LEN;

	if (fcret) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = -EBADF;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		dev_err(ctrl->dev,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"q %d Create I/O Connection LS failed: %s\n",
			queue->qnum, validation_errors[fcret]);
	} else {
		queue->connection_id =	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			be64_to_cpu(conn_acc->connectid.connection_id);
		set_bit(NVME_FC_Q_CONNECTED, &queue->flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}

out_free_buffer:
	kfree(lsop);	/* [한국어] 커널 메모리 생명주기 */
out_no_memory:
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(ctrl->dev,
			"queue %d connect I/O queue failed (%d).\n",
			queue->qnum, ret);
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static void
nvme_fc_disconnect_assoc_done(struct nvmefc_ls_req *lsreq, int status)
{
	struct nvmefc_ls_req_op *lsop = ls_req_to_lsop(lsreq);	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	__nvme_fc_finish_ls_req(lsop);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/* fc-nvme initiator doesn't care about success or failure of cmd */

	kfree(lsop);	/* [한국어] 커널 메모리 생명주기 */
}

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
/*
 * [한국어]
 * nvme_fc_xmt_disconnect_assoc - 타겟에 association 종료를 알린다
 *
 * @ctrl: 종료할 세션을 가진 컨트롤러
 * @return: 없음. 실패해도 되돌릴 것이 없다.
 *
 * 이 LS 를 반드시 보내야 하는 이유는 타겟 쪽 상태 때문이다. 알리지 않고
 * 끊으면 타겟은 association 이 아직 살아 있다고 믿고 자원을 붙들고 있으며,
 * 같은 호스트가 다시 접속하려 하면 "이미 그 조합의 세션이 있다"며 거절한다
 * (create_association 의 ENOTUNIQ 가 그 경우다).
 *
 * 이 파일의 다른 LS 와 달리 비동기로 보내고 기다리지 않는다. 세션을 내리는
 * 중이라 응답을 받아도 할 일이 없고, 타겟이 응답하지 않는 상황(포트가 이미
 * 사라진 경우)에서 기다리면 해체가 그만큼 지연되기 때문이다.
 *
 * 그래서 반환값도 없다. 보내지 못했더라도 해체는 계속되어야 한다 -- 여기서
 * 실패를 이유로 멈추면 이쪽 자원까지 붙들리게 된다.
 *
 * 할당 구조는 connect_admin_queue 와 같다. 서술자·요청·응답·LLDD 사적 영역을
 * 한 덩어리로 잡고, 실패 시에만 여기서 해제한다. 성공하면 완료 콜백
 * (disconnect_assoc_done)이 해제를 맡는다.
 *
 * 실행 컨텍스트: 해체 경로(워크큐). 잠들 수 있다.
 *
 * 호출 체인:
 *   nvme_fc_delete_association / _create_association 실패 경로
 *     → [이 함수] → nvme_fc_send_ls_req_async → LLDD
 */
static void
nvme_fc_xmt_disconnect_assoc(struct nvme_fc_ctrl *ctrl)
{
	struct fcnvme_ls_disconnect_assoc_rqst *discon_rqst;
	struct fcnvme_ls_disconnect_assoc_acc *discon_acc;
	struct nvmefc_ls_req_op *lsop;
	struct nvmefc_ls_req *lsreq;
	int ret;

	lsop = kzalloc((sizeof(*lsop) +	/* [한국어] 서술자와 두 버퍼, LLDD 사적 영역을 한 덩어리로 */
			sizeof(*discon_rqst) + sizeof(*discon_acc) +
			ctrl->lport->ops->lsrqst_priv_sz), GFP_KERNEL);
	if (!lsop) {
		dev_info(ctrl->ctrl.device,	/* [한국어] 알리지 못하면 타겟이 죽은 세션을 붙들지만, 해체 자체는 계속해야 한다 */
			"NVME-FC{%d}: send Disconnect Association "
			"failed: ENOMEM\n",
			ctrl->cnum);
		return;
	}

	discon_rqst = (struct fcnvme_ls_disconnect_assoc_rqst *)&lsop[1];	/* [한국어] 서술자 뒤가 요청 버퍼 */
	discon_acc = (struct fcnvme_ls_disconnect_assoc_acc *)&discon_rqst[1];
	lsreq = &lsop->ls_req;
	if (ctrl->lport->ops->lsrqst_priv_sz)
		lsreq->private = (void *)&discon_acc[1];	/* [한국어] 마지막 영역은 LLDD 몫 */
	else
		lsreq->private = NULL;

	nvmefc_fmt_lsreq_discon_assoc(lsreq, discon_rqst, discon_acc,	/* [한국어] fc.h 의 공통 빌더 — 타겟도 같은 코드로 이 PDU 를 해석한다 */
				ctrl->association_id);	/* [한국어] 어느 세션을 끊는지 지정 */

	ret = nvme_fc_send_ls_req_async(ctrl->rport, lsop,	/* [한국어] 비동기 — 응답을 기다리지 않는다. 세션을 내리는 중이라 받아도 할 일이 없다 */
				nvme_fc_disconnect_assoc_done);	/* [한국어] 완료 시 버퍼를 해제하는 콜백 */
	if (ret)
		kfree(lsop);	/* [한국어] 접수 자체가 실패했으면 콜백이 오지 않으므로 여기서 해제한다 */
}

/*
 * [한국어]
 * nvme_fc_xmt_ls_rsp_free - 수신 LS 하나의 자원을 모두 되돌린다
 *
 * @lsop: 처리가 끝난 수신 LS
 * @return: 없음
 *
 * 응답 전송이 끝났거나 거부된 뒤 불린다. 목록에서 빼고, DMA 매핑을 풀고,
 * 버퍼 셋을 해제하고, 마지막에 포트 참조를 놓는다.
 *
 * 순서에 이유가 있다. 목록에서 먼저 빼야 다른 경로가 이 항목을 집지 않고,
 * 포트 참조는 맨 마지막에 놓아야 그전까지 lport->dev 를 안전하게 쓸 수 있다.
 * 먼저 놓으면 DMA 해제 도중 포트가 사라질 수 있다.
 *
 * sync_for_cpu 를 unmap 전에 부르는 것은 관례다. 실제로 읽을 내용은 없지만,
 * 매핑을 푸는 시점에 캐시 상태를 정합하게 맞춘다.
 *
 * 실행 컨텍스트: 전송 완료 콜백 또는 거부 경로.
 *
 * 호출 체인:
 *   nvme_fc_xmt_ls_rsp_done / _xmt_ls_rsp(거부 시) → [이 함수]
 */
static void
nvme_fc_xmt_ls_rsp_free(struct nvmefc_ls_rcv_op *lsop)
{
	struct nvme_fc_rport *rport = lsop->rport;
	struct nvme_fc_lport *lport = rport->lport;
	unsigned long flags;

	spin_lock_irqsave(&rport->lock, flags);
	list_del(&lsop->lsrcv_list);	/* [한국어] 먼저 목록에서 뺀다 — 다른 경로가 해제 중인 항목을 집지 않도록 */
	spin_unlock_irqrestore(&rport->lock, flags);

	fc_dma_sync_single_for_cpu(lport->dev, lsop->rspdma,	/* [한국어] 매핑을 푸는 시점의 캐시 정합을 맞추는 관례 */
				sizeof(*lsop->rspbuf), DMA_TO_DEVICE);
	fc_dma_unmap_single(lport->dev, lsop->rspdma,
			sizeof(*lsop->rspbuf), DMA_TO_DEVICE);

	kfree(lsop->rspbuf);	/* [한국어] 수신 LS 는 세 버퍼를 따로 잡는다 — 송신 LS 의 한 덩어리 할당과 다르다 */
	kfree(lsop->rqstbuf);
	kfree(lsop);

	nvme_fc_rport_put(rport);	/* [한국어] 맨 마지막 — 그전까지 lport->dev 를 써야 하므로 먼저 놓으면 안 된다 */
}

static void
nvme_fc_xmt_ls_rsp_done(struct nvmefc_ls_rsp *lsrsp)
{
	struct nvmefc_ls_rcv_op *lsop = lsrsp->nvme_fc_private;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	nvme_fc_xmt_ls_rsp_free(lsop);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
}

/*
 * [한국어]
 * nvme_fc_xmt_ls_rsp - 준비된 LS 응답을 LLDD 에 넘겨 내보낸다
 *
 * @lsop: 응답 버퍼가 이미 채워진 수신 LS
 * @return: 없음
 *
 * handle_ls_rqst 가 버퍼에 채워 둔 ACC 또는 RJT 를 실제로 보낸다.
 * Disconnect 수락처럼 전송을 미뤄 둔 경우에는 세션 해체가 끝난 뒤에
 * 이 함수가 불린다.
 *
 * sync_for_device 가 먼저 오는 이유: CPU 가 채운 응답 버퍼를 HBA 가 읽어야
 * 하므로, 그전에 캐시 내용을 메모리에 반영해야 한다. 방향이 TO_DEVICE 인
 * 것도 그래서다.
 *
 * LLDD 가 거부하면 완료 콜백이 오지 않는다. 그래서 여기서 직접 자원을
 * 해제한다 -- 성공 경로에서는 xmt_ls_rsp_done 이 그 일을 맡는다.
 * 두 경로가 각각 한 번씩만 해제하도록 나뉘어 있다.
 *
 * 실행 컨텍스트: lsrcv_work 또는 해체 경로.
 *
 * 호출 체인:
 *   nvme_fc_handle_ls_rqst_work / _delete_association
 *     → [이 함수] → LLDD 의 xmt_ls_rsp → nvme_fc_xmt_ls_rsp_done
 */
static void
nvme_fc_xmt_ls_rsp(struct nvmefc_ls_rcv_op *lsop)
{
	struct nvme_fc_rport *rport = lsop->rport;
	struct nvme_fc_lport *lport = rport->lport;
	struct fcnvme_ls_rqst_w0 *w0 = &lsop->rqstbuf->w0;	/* [한국어] 실패 로그에 어떤 LS 였는지 남기기 위해 */
	int ret;

	fc_dma_sync_single_for_device(lport->dev, lsop->rspdma,	/* [한국어] CPU 가 채운 응답을 HBA 가 읽기 전에 캐시를 메모리로 반영한다 */
				  sizeof(*lsop->rspbuf), DMA_TO_DEVICE);

	ret = lport->ops->xmt_ls_rsp(&lport->localport, &rport->remoteport,
				     lsop->lsrsp);
	if (ret) {
		dev_warn(lport->dev,
			"LLDD rejected LS RSP xmt: LS %d status %d\n",
			w0->ls_cmd, ret);
		nvme_fc_xmt_ls_rsp_free(lsop);	/* [한국어] 거부되면 완료 콜백이 오지 않으므로 여기서 직접 해제한다 */
		return;
	}
}

static struct nvme_fc_ctrl *	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
nvme_fc_match_disconn_ls(struct nvme_fc_rport *rport,
		      struct nvmefc_ls_rcv_op *lsop)	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
{
	struct fcnvme_ls_disconnect_assoc_rqst *rqst =	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
					&lsop->rqstbuf->rq_dis_assoc;
	struct nvme_fc_ctrl *ctrl, *tmp, *ret = NULL;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvmefc_ls_rcv_op *oldls = NULL;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	u64 association_id = be64_to_cpu(rqst->associd.association_id);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	unsigned long flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	spin_lock_irqsave(&rport->lock, flags);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */

	list_for_each_entry_safe(ctrl, tmp, &rport->ctrl_list, ctrl_list) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (!nvme_fc_ctrl_get(ctrl))	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
			continue;
		spin_lock(&ctrl->lock);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
		if (association_id == ctrl->association_id) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			oldls = ctrl->rcv_disconn;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			ctrl->rcv_disconn = lsop;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			ret = ctrl;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		}
		spin_unlock(&ctrl->lock);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			/* leave the ctrl get reference */
			break;
		spin_unlock_irqrestore(&rport->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_fc_ctrl_put(ctrl);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		spin_lock_irqsave(&rport->lock, flags);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
	}

	spin_unlock_irqrestore(&rport->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/* transmit a response for anything that was pending */
	if (oldls) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_info(rport->lport->dev,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"NVME-FC{%d}: Multiple Disconnect Association "
			"LS's received\n", ctrl->cnum);
		/* overwrite good response with bogus failure */
		oldls->lsrsp->rsplen = nvme_fc_format_rjt(oldls->rspbuf,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
						sizeof(*oldls->rspbuf),
						rqst->w0.ls_cmd,
						FCNVME_RJT_RC_UNAB,
						FCNVME_RJT_EXP_NONE, 0);
		nvme_fc_xmt_ls_rsp(oldls);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	}

	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

/*
 * returns true to mean LS handled and ls_rsp can be sent
 * returns false to defer ls_rsp xmt (will be done as part of
 *     association termination)
 */
/*
 * [한국어]
 * nvme_fc_ls_disconnect_assoc - 타겟이 보내 온 Disconnect Association 을 처리한다
 *
 * @lsop: 수신한 LS. 요청 버퍼와 응답 버퍼가 함께 들어 있다.
 * @return: true 면 지금 곧바로 응답을 보내라는 뜻. false 면 나중에 보내야 한다.
 *
 * 반환값의 의미가 이 함수의 요점이다. 거절(RJT)은 세션에 영향이 없으니 즉시
 * 보내면 되지만, 수락(ACC)은 지금 보내면 안 된다. 위 영어 주석이 그 이유를
 * 밝힌다 -- 응답 전송은 nvme_fc_delete_association() 이 이 association 의
 * 모든 익스체인지를 ABTS 로 정리한 뒤에 일어나야 한다. 먼저 응답하면 타겟이
 * 곧바로 재접속을 시도하는데, 이쪽은 아직 이전 세션의 I/O 를 취소하는 중이라
 * 두 세션이 겹친다.
 *
 * 그래서 ACC 는 여기서 버퍼에만 채워 두고 false 를 돌려준다. 실제 전송은
 * ctrl->rcv_disconn 에 보관돼 있다가 해체가 끝난 뒤 이뤄진다.
 *
 * 검증이 두 단계인 것도 눈여겨볼 만하다. 먼저 LS 자체가 형식에 맞는지 보고,
 * 그다음 이 association 이 실제로 존재하는지 찾는다. 어느 쪽이 실패했느냐에
 * 따라 거절 사유 코드가 달라진다 -- INV_ASSOC 인지 LOGIC 인지가 상대에게
 * 무엇이 잘못됐는지 알려 준다.
 *
 * 실행 컨텍스트: lsrcv_work 워크큐. LS 수신 콜백이 인터럽트 문맥일 수 있어
 * 실제 처리는 이렇게 워크로 넘어온다.
 *
 * 호출 체인:
 *   LLDD 의 LS 수신 → nvme_fc_rcv_ls_req → lsrcv_work
 *     → nvme_fc_handle_ls_rqst → [이 함수] → nvme_fc_error_recovery
 */
static bool
nvme_fc_ls_disconnect_assoc(struct nvmefc_ls_rcv_op *lsop)
{
	struct nvme_fc_rport *rport = lsop->rport;
	struct fcnvme_ls_disconnect_assoc_rqst *rqst =	/* [한국어] 타겟이 보낸 요청 */
					&lsop->rqstbuf->rq_dis_assoc;
	struct fcnvme_ls_disconnect_assoc_acc *acc =	/* [한국어] 우리가 채울 응답. ACC 든 RJT 든 같은 버퍼를 쓴다 */
					&lsop->rspbuf->rsp_dis_assoc;
	struct nvme_fc_ctrl *ctrl = NULL;
	int ret = 0;

	memset(acc, 0, sizeof(*acc));	/* [한국어] 재사용 버퍼라 이전 응답이 남지 않게 지운다 */

	ret = nvmefc_vldt_lsreq_discon_assoc(lsop->rqstdatalen, rqst);	/* [한국어] 1단계: LS 형식이 스펙에 맞는가 — 원격 입력이라 믿지 않는다 */
	if (!ret) {
		/* match an active association */
		ctrl = nvme_fc_match_disconn_ls(rport, lsop);	/* [한국어] 2단계: 그 association 이 실제로 있는가. 찾으면 참조를 하나 든다 */
		if (!ctrl)
			ret = VERR_NO_ASSOC;
	}

	if (ret) {
		dev_info(rport->lport->dev,
			"Disconnect LS failed: %s\n",
			validation_errors[ret]);
		lsop->lsrsp->rsplen = nvme_fc_format_rjt(acc,	/* [한국어] 거절 응답을 짓는다 */
					sizeof(*acc), rqst->w0.ls_cmd,
					(ret == VERR_NO_ASSOC) ?	/* [한국어] 실패 원인에 따라 사유 코드가 갈린다 — 상대가 무엇이 잘못됐는지 알 수 있게 */
						FCNVME_RJT_RC_INV_ASSOC :	/* [한국어] 그런 association 이 없다 */
						FCNVME_RJT_RC_LOGIC,	/* [한국어] 형식이 어긋났다 */
					FCNVME_RJT_EXP_NONE, 0);
		return true;	/* [한국어] 거절은 세션에 영향이 없으니 즉시 보내도 된다 */
	}

	/* format an ACCept response */

	lsop->lsrsp->rsplen = sizeof(*acc);

	nvme_fc_format_rsp_hdr(acc, FCNVME_LS_ACC,	/* [한국어] 수락 응답을 버퍼에 채워만 둔다 */
			fcnvme_lsdesc_len(
				sizeof(struct fcnvme_ls_disconnect_assoc_acc)),
			FCNVME_LS_DISCONNECT_ASSOC);

	/*
	 * the transmit of the response will occur after the exchanges
	 * for the association have been ABTS'd by
	 * nvme_fc_delete_association().
	 */
	/* [한국어] 위 영어 주석대로 지금 보내면 안 된다. 먼저 응답하면 타겟이 곧바로
	 * 재접속을 시도하는데, 이쪽은 아직 이전 세션의 I/O 를 ABTS 로 정리하는
	 * 중이라 두 세션이 겹친다. 그래서 아래 false 를 돌려 전송을 미룬다. */

	/* fail the association */
	nvme_fc_error_recovery(ctrl, "Disconnect Association LS received");	/* [한국어] 타겟이 끊겠다고 했으니 이쪽도 세션 해체를 시작한다 */

	/* release the reference taken by nvme_fc_match_disconn_ls() */
	nvme_fc_ctrl_put(ctrl);	/* [한국어] 위 match 가 든 참조를 놓는다 */

	return false;	/* [한국어] 응답은 해체가 끝난 뒤 rcv_disconn 경로로 보낸다 */
}

/*
 * Actual Processing routine for received FC-NVME LS Requests from the LLD
 * returns true if a response should be sent afterward, false if rsp will
 * be sent asynchronously.
 */
/*
 * [한국어]
 * nvme_fc_handle_ls_rqst - 타겟이 보내 온 LS 를 종류별로 분기한다
 *
 * @lsop: 수신한 LS. 요청·응답 버퍼가 함께 들어 있다.
 * @return: true 면 응답을 지금 보내라. false 면 나중에 보내야 한다.
 *
 * 호스트가 받을 수 있는 LS 는 사실상 하나뿐이다 -- Disconnect Association.
 * 나머지는 전부 거절이며, 거절 사유가 셋으로 갈리는 것이 이 함수의 내용이다:
 *
 *   - Disconnect Connection: UNSUP. 스펙에는 있으나 이 구현이 지원하지 않는다.
 *   - Create Association / Create Connection: LOGIC. 방향이 잘못됐다.
 *     세션 개설은 호스트가 요청하는 것이지 타겟이 요청하는 것이 아니다.
 *   - 그 밖: INVAL. 알 수 없는 LS 코드다.
 *
 * 사유를 구분해 두는 이유는 상대가 무엇이 잘못됐는지 알 수 있게 하기 위해서다.
 * 미지원과 논리 오류와 형식 오류는 상대가 취할 조치가 각각 다르다.
 *
 * rsplen 을 0 으로 미리 두는 것은 위 영어 주석이 밝히듯 방어적 조치다.
 * 핸들러가 나중에 올바른 길이를 채우며, 채우지 못한 경로가 있어도 길이 0 인
 * 응답이 나가지 쓰레기 값이 나가지는 않는다.
 *
 * 실행 컨텍스트: lsrcv_work 워크큐. 아래 handle_ls_rqst_work 가 락을 놓고 부른다.
 *
 * 호출 체인:
 *   nvme_fc_handle_ls_rqst_work → [이 함수] → nvme_fc_ls_disconnect_assoc
 */
static bool
nvme_fc_handle_ls_rqst(struct nvmefc_ls_rcv_op *lsop)
{
	struct fcnvme_ls_rqst_w0 *w0 = &lsop->rqstbuf->w0;	/* [한국어] LS 코드가 담긴 첫 워드 */
	bool ret = true;

	lsop->lsrsp->nvme_fc_private = lsop;	/* [한국어] 전송 완료 콜백이 이 구조체를 되찾을 실마리 */
	lsop->lsrsp->rspbuf = lsop->rspbuf;
	lsop->lsrsp->rspdma = lsop->rspdma;	/* [한국어] LLDD 가 프레임에 실을 DMA 주소 */
	lsop->lsrsp->done = nvme_fc_xmt_ls_rsp_done;	/* [한국어] 응답 전송이 끝나면 버퍼를 해제하는 콜백 */
	/* Be preventative. handlers will later set to valid length */
	lsop->lsrsp->rsplen = 0;	/* [한국어] 위 영어 주석대로 방어적 초기화. 채우지 못한 경로가 있어도 쓰레기가 나가지 않는다 */

	/*
	 * handlers:
	 *   parse request input, execute the request, and format the
	 *   LS response
	 */
	switch (w0->ls_cmd) {
	case FCNVME_LS_DISCONNECT_ASSOC:	/* [한국어] 호스트가 실제로 처리하는 유일한 LS */
		ret = nvme_fc_ls_disconnect_assoc(lsop);	/* [한국어] 수락이면 false 를 돌려 응답을 미룬다 */
		break;
	case FCNVME_LS_DISCONNECT_CONN:	/* [한국어] 스펙에는 있으나 이 구현이 지원하지 않는다 */
		lsop->lsrsp->rsplen = nvme_fc_format_rjt(lsop->rspbuf,
				sizeof(*lsop->rspbuf), w0->ls_cmd,
				FCNVME_RJT_RC_UNSUP, FCNVME_RJT_EXP_NONE, 0);	/* [한국어] 미지원 — 상대는 association 단위 해제를 쓰면 된다 */
		break;
	case FCNVME_LS_CREATE_ASSOCIATION:
	case FCNVME_LS_CREATE_CONNECTION:	/* [한국어] 방향이 잘못됐다 — 세션 개설은 호스트가 요청하는 것이다 */
		lsop->lsrsp->rsplen = nvme_fc_format_rjt(lsop->rspbuf,
				sizeof(*lsop->rspbuf), w0->ls_cmd,
				FCNVME_RJT_RC_LOGIC, FCNVME_RJT_EXP_NONE, 0);	/* [한국어] 논리 오류로 거절 */
		break;
	default:	/* [한국어] 알 수 없는 LS 코드 */
		lsop->lsrsp->rsplen = nvme_fc_format_rjt(lsop->rspbuf,
				sizeof(*lsop->rspbuf), w0->ls_cmd,
				FCNVME_RJT_RC_INVAL, FCNVME_RJT_EXP_NONE, 0);	/* [한국어] 형식 오류로 거절. 사유를 나누는 것은 상대가 취할 조치가 다르기 때문이다 */
		break;
	}

	return(ret);
}

/*
 * [한국어]
 * nvme_fc_handle_ls_rqst_work - 수신 대기 중인 LS 들을 하나씩 처리한다
 *
 * @work: rport 에 박혀 있는 lsrcv_work
 * @return: 없음
 *
 * LS 수신 콜백은 인터럽트 문맥일 수 있어 그 자리에서 처리할 수 없다. 그래서
 * 목록에 쌓아 두고 이 워크가 잠들 수 있는 문맥에서 꺼내 처리한다.
 *
 * 구조가 특이하다. 처리하려면 락을 놓아야 하는데(핸들러가 잠들 수 있다),
 * 놓는 순간 목록이 바뀔 수 있어 순회를 이어 갈 수 없다. 그래서 하나를
 * 처리할 때마다 goto restart 로 처음부터 다시 훑는다. 이미 처리한 것을
 * 건너뛰기 위해 lsop->handled 표시를 쓰고, 그 표시는 락 안에서 세운다.
 * 순회 중 안전하게 락을 놓기 위한 관용적 처리다.
 *
 * 포트가 오프라인이면 핸들러를 부르지 않고 UNAB(사용 불가)로 거절한다.
 * 세션이 이미 끊긴 상태에서 Disconnect 를 처리해 봐야 의미가 없고,
 * 그 과정에서 이미 정리된 자원을 건드릴 수 있기 때문이다.
 *
 * sendrsp 가 false 인 경우는 하나뿐이다 -- Disconnect Association 을 수락한
 * 경우로, 그때는 세션 해체가 끝난 뒤에 응답이 나간다.
 *
 * 실행 컨텍스트: 워크큐. 핸들러가 잠들 수 있어야 하므로 락을 놓고 부른다.
 *
 * 호출 체인:
 *   LLDD 의 LS 수신 → nvme_fc_rcv_ls_req → queue_work → [이 함수]
 *     → nvme_fc_handle_ls_rqst → nvme_fc_xmt_ls_rsp
 */
static void
nvme_fc_handle_ls_rqst_work(struct work_struct *work)
{
	struct nvme_fc_rport *rport =
		container_of(work, struct nvme_fc_rport, lsrcv_work);
	struct fcnvme_ls_rqst_w0 *w0;
	struct nvmefc_ls_rcv_op *lsop;
	unsigned long flags;
	bool sendrsp;

restart:	/* [한국어] 락을 놓고 처리한 뒤에는 목록이 바뀌었을 수 있어 처음부터 다시 훑는다 */
	sendrsp = true;
	spin_lock_irqsave(&rport->lock, flags);	/* [한국어] LS 수신 콜백이 인터럽트 문맥일 수 있어 irqsave */
	list_for_each_entry(lsop, &rport->ls_rcv_list, lsrcv_list) {
		if (lsop->handled)	/* [한국어] 이미 처리한 것은 건너뛴다 — restart 구조 때문에 필요한 표시다 */
			continue;

		lsop->handled = true;	/* [한국어] 락 안에서 표시해야 다른 순회가 같은 것을 두 번 잡지 않는다 */
		if (rport->remoteport.port_state == FC_OBJSTATE_ONLINE) {
			spin_unlock_irqrestore(&rport->lock, flags);	/* [한국어] 핸들러가 잠들 수 있어 락을 놓는다 */
			sendrsp = nvme_fc_handle_ls_rqst(lsop);
		} else {
			spin_unlock_irqrestore(&rport->lock, flags);
			w0 = &lsop->rqstbuf->w0;
			lsop->lsrsp->rsplen = nvme_fc_format_rjt(	/* [한국어] 포트가 이미 오프라인이면 처리해 봐야 의미가 없고, 정리된 자원을 건드릴 수 있다 */
						lsop->rspbuf,
						sizeof(*lsop->rspbuf),
						w0->ls_cmd,
						FCNVME_RJT_RC_UNAB,	/* [한국어] 사용 불가로 거절 */
						FCNVME_RJT_EXP_NONE, 0);
		}
		if (sendrsp)	/* [한국어] false 인 경우는 Disconnect 수락뿐 — 그때는 해체 후에 응답이 나간다 */
			nvme_fc_xmt_ls_rsp(lsop);
		goto restart;	/* [한국어] 락을 놓았으므로 순회를 이어 갈 수 없다. 처음부터 다시 */
	}
	spin_unlock_irqrestore(&rport->lock, flags);	/* [한국어] 처리할 것이 더 없으면 여기로 온다 */
}

static
void nvme_fc_rcv_ls_req_err_msg(struct nvme_fc_lport *lport,
				struct fcnvme_ls_rqst_w0 *w0)	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
{
	dev_info(lport->dev, "RCV %s LS failed: No memory\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		(w0->ls_cmd <= NVME_FC_LAST_LS_CMD_VALUE) ?
			nvmefc_ls_names[w0->ls_cmd] : "");
}

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
int
nvme_fc_rcv_ls_req(struct nvme_fc_remote_port *portptr,
			struct nvmefc_ls_rsp *lsrsp,	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
			void *lsreqbuf, u32 lsreqbuf_len)
{
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
			"RCV %s LS failed: no LLDD xmt_ls_rsp\n",
			(w0->ls_cmd <= NVME_FC_LAST_LS_CMD_VALUE) ?
				nvmefc_ls_names[w0->ls_cmd] : "");
		ret = -EINVAL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_put;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}

	if (lsreqbuf_len > sizeof(union nvmefc_ls_requests)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_info(lport->dev,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"RCV %s LS failed: payload too large\n",
			(w0->ls_cmd <= NVME_FC_LAST_LS_CMD_VALUE) ?
				nvmefc_ls_names[w0->ls_cmd] : "");
		ret = -E2BIG;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_put;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}

	lsop = kzalloc_obj(*lsop);	/* [한국어] 커널 메모리 생명주기 */
	if (!lsop) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_fc_rcv_ls_req_err_msg(lport, w0);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		ret = -ENOMEM;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_put;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}

	lsop->rqstbuf = kzalloc_obj(*lsop->rqstbuf);	/* [한국어] 커널 메모리 생명주기 */
	lsop->rspbuf = kzalloc_obj(*lsop->rspbuf);	/* [한국어] 커널 메모리 생명주기 */
	if (!lsop->rqstbuf || !lsop->rspbuf) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_fc_rcv_ls_req_err_msg(lport, w0);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		ret = -ENOMEM;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_free;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}

	lsop->rspdma = fc_dma_map_single(lport->dev, lsop->rspbuf,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
					sizeof(*lsop->rspbuf),
					DMA_TO_DEVICE);
	if (fc_dma_mapping_error(lport->dev, lsop->rspdma)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_info(lport->dev,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"RCV %s LS failed: DMA mapping failure\n",
			(w0->ls_cmd <= NVME_FC_LAST_LS_CMD_VALUE) ?
				nvmefc_ls_names[w0->ls_cmd] : "");
		ret = -EFAULT;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_free;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}

	lsop->rport = rport;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	lsop->lsrsp = lsrsp;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	memcpy(lsop->rqstbuf, lsreqbuf, lsreqbuf_len);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	lsop->rqstdatalen = lsreqbuf_len;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	spin_lock_irqsave(&rport->lock, flags);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
	if (rport->remoteport.port_state != FC_OBJSTATE_ONLINE) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		spin_unlock_irqrestore(&rport->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ret = -ENOTCONN;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_unmap;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}
	list_add_tail(&lsop->lsrcv_list, &rport->ls_rcv_list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	spin_unlock_irqrestore(&rport->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	schedule_work(&rport->lsrcv_work);	/* [한국어] 워크큐 — 송수신/재연결/복구 비동기 실행 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

out_unmap:
	fc_dma_unmap_single(lport->dev, lsop->rspdma,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			sizeof(*lsop->rspbuf), DMA_TO_DEVICE);
out_free:
	kfree(lsop->rspbuf);	/* [한국어] 커널 메모리 생명주기 */
	kfree(lsop->rqstbuf);	/* [한국어] 커널 메모리 생명주기 */
	kfree(lsop);	/* [한국어] 커널 메모리 생명주기 */
out_put:
	nvme_fc_rport_put(rport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}
EXPORT_SYMBOL_GPL(nvme_fc_rcv_ls_req);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */


/* *********************** NVME Ctrl Routines **************************** */

static void
__nvme_fc_exit_request(struct nvme_fc_ctrl *ctrl,
		struct nvme_fc_fcp_op *op)
{
	fc_dma_unmap_single(ctrl->lport->dev, op->fcp_req.rspdma,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				sizeof(op->rsp_iu), DMA_FROM_DEVICE);
	fc_dma_unmap_single(ctrl->lport->dev, op->fcp_req.cmddma,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				sizeof(op->cmd_iu), DMA_TO_DEVICE);

	atomic_set(&op->state, FCPOP_STATE_UNINIT);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}

static void
nvme_fc_exit_request(struct blk_mq_tag_set *set, struct request *rq,
		unsigned int hctx_idx)
{
	struct nvme_fc_fcp_op *op = blk_mq_rq_to_pdu(rq);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	return __nvme_fc_exit_request(to_fc_ctrl(set->driver_data), op);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

/*
 * [한국어]
 * __nvme_fc_abort_op - 진행 중인 오퍼레이션 하나를 LLDD 수준에서 취소한다
 *
 * @ctrl: 소속 컨트롤러
 * @op:   취소할 오퍼레이션
 * @return: 0 이면 LLDD 에 abort 를 요청했다. -ECANCELED 면 이미 끝났거나
 *          아직 시작하지 않아 취소할 것이 없다.
 *
 * 이 함수의 전부는 첫 두 줄의 경합 처리다. abort 를 걸려는 순간에 완료가
 * 도착할 수 있으므로, atomic_xchg 로 상태를 ABORTED 로 바꾸면서 이전 값을
 * 받아 온다. 이전 값이 ACTIVE 가 아니었다면 이미 다른 쪽이 처리한 것이므로
 * 바꿔 놓은 상태를 원래대로 되돌리고 물러난다. 되돌리지 않으면 이미 완료된
 * 오퍼레이션이 ABORTED 로 남아 다음 재사용 때 오작동한다.
 *
 * FCCTRL_TERMIO 를 확인하는 이유: 세션 해체 중이라면 이 abort 의 완료를
 * delete_association 이 기다린다. 그래서 iocnt 를 올려 두고, 완료 경로가
 * 내려 0 이 되면 대기하던 쪽을 깨운다. 해체 중이 아니라면(예: 단순 타임아웃)
 * 아무도 기다리지 않으므로 세지 않는다.
 *
 * 실제 취소는 LLDD 의 fcp_abort 가 한다. FC 는 I/O 마다 익스체인지가 있어
 * 하드웨어 수준에서 그것을 끝내야 자원이 풀린다.
 *
 * 실행 컨텍스트: 타임아웃 문맥과 해체 워크 양쪽에서 불린다. 스핀락을 쓰므로
 * 잠들지 않는다.
 *
 * 호출 체인:
 *   nvme_fc_timeout / nvme_fc_terminate_exchange / _abort_aen_ops
 *     → [이 함수] → LLDD 의 fcp_abort
 */
static int
__nvme_fc_abort_op(struct nvme_fc_ctrl *ctrl, struct nvme_fc_fcp_op *op)
{
	unsigned long flags;
	int opstate;

	spin_lock_irqsave(&ctrl->lock, flags);	/* [한국어] iocnt 와 flags 를 완료 콜백과 함께 보호한다 */
	opstate = atomic_xchg(&op->state, FCPOP_STATE_ABORTED);	/* [한국어] 바꾸면서 이전 값을 받는다 — 완료와 겹쳐도 한 쪽만 이긴다 */
	if (opstate != FCPOP_STATE_ACTIVE)	/* [한국어] 이미 끝났거나 시작 전이었다 */
		atomic_set(&op->state, opstate);	/* [한국어] 방금 덮어쓴 값을 되돌린다. 안 하면 완료된 것이 ABORTED 로 남아 재사용 때 오작동한다 */
	else if (test_bit(FCCTRL_TERMIO, &ctrl->flags)) {	/* [한국어] 세션 해체 중이라면 이 abort 의 완료를 기다리는 쪽이 있다 */
		op->flags |= FCOP_FLAGS_TERMIO;	/* [한국어] 완료 경로가 정상 완료와 구분할 수 있게 표시 */
		ctrl->iocnt++;	/* [한국어] 기다릴 개수를 올린다. 완료가 내려 0 이 되면 delete_association 이 깨어난다 */
	}
	spin_unlock_irqrestore(&ctrl->lock, flags);

	if (opstate != FCPOP_STATE_ACTIVE)
		return -ECANCELED;	/* [한국어] 취소할 것이 없었다 — 호출자는 이를 오류로 보지 않는다 */

	ctrl->lport->ops->fcp_abort(&ctrl->lport->localport,	/* [한국어] FC 익스체인지를 하드웨어 수준에서 끝낸다 */
					&ctrl->rport->remoteport,
					op->queue->lldd_handle,
					&op->fcp_req);

	return 0;
}

static void
nvme_fc_abort_aen_ops(struct nvme_fc_ctrl *ctrl)
{
	struct nvme_fc_fcp_op *aen_op = ctrl->aen_ops;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	int i;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/* ensure we've initialized the ops once */
	if (!(aen_op->flags & FCOP_FLAGS_AEN))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	for (i = 0; i < NVME_NR_AEN_COMMANDS; i++, aen_op++)	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		__nvme_fc_abort_op(ctrl, aen_op);
}

static inline void
__nvme_fc_fcpop_chk_teardowns(struct nvme_fc_ctrl *ctrl,
		struct nvme_fc_fcp_op *op, int opstate)
{
	unsigned long flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (opstate == FCPOP_STATE_ABORTED) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		spin_lock_irqsave(&ctrl->lock, flags);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
		if (test_bit(FCCTRL_TERMIO, &ctrl->flags) &&	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		    op->flags & FCOP_FLAGS_TERMIO) {
			if (!--ctrl->iocnt)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
				wake_up(&ctrl->ioabort_wait);
		}
		spin_unlock_irqrestore(&ctrl->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}
}

static void
nvme_fc_ctrl_ioerr_work(struct work_struct *work)
{
	struct nvme_fc_ctrl *ctrl =	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
			container_of(work, struct nvme_fc_ctrl, ioerr_work);

	nvme_fc_error_recovery(ctrl, "transport detected io error");	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
}

/*
 * nvme_fc_io_getuuid - Routine called to get the appid field
 * associated with request by the lldd
 * @req:IO request from nvme fc to driver
 * Returns: UUID if there is an appid associated with VM or
 * NULL if the user/libvirt has not set the appid to VM
 */
char *nvme_fc_io_getuuid(struct nvmefc_fcp_req *req)
{
	struct nvme_fc_fcp_op *op = fcp_req_to_fcp_op(req);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct request *rq = op->rq;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	if (!IS_ENABLED(CONFIG_BLK_CGROUP_FC_APPID) || !rq || !rq->bio)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return NULL;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	return blkcg_get_fc_appid(rq->bio);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}
EXPORT_SYMBOL_GPL(nvme_fc_io_getuuid);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

/*
 * [한국어]
 * nvme_fc_fcpio_done - LLDD 가 I/O 완료를 알려 올 때 불린다
 *
 * @req: 완료된 FCP 요청. fcp_req_to_fcp_op 으로 우리 오퍼레이션을 되찾는다.
 * @return: 없음
 *
 * FC 의 완료 경로다. 다른 트랜스포트와 결정적으로 다른 점이 둘 있다.
 *
 * 첫째, 응답의 모양이 세 가지다. 성공한 명령은 응답 페이로드가 아예 없거나
 * 12바이트 0 으로 오고(대역폭을 아끼는 최적화), 그 경우 이 함수가 CQE 를
 * 지어낸다. 완전한 ERSP IU 가 오는 경우에만 실제 CQE 가 들어 있다. 그래서
 * 아래 switch 가 rcv_rsplen 으로 갈린다.
 *
 * 둘째, 개별 I/O 실패가 세션 전체의 종료로 이어진다. 위 영어 주석의 마지막
 * 문단이 그 근거를 밝힌다 -- 전송 계층에서 감지된 실패는 이니시에이터와
 * 타겟의 SQ head/tail 인식을 어긋나게 할 수 있고, FC-NVME 스펙은 그때
 * 연결을, 따라서 association 을 끊도록 요구한다. terminate_assoc 이 기본값
 * true 이고 정상 완료에 도달해야만 false 가 되는 구조가 그 요구의 표현이다.
 *
 * atomic_xchg 로 상태를 바꾸는 것이 이 함수의 동시성 보호 전부다. abort 와
 * 완료가 같은 오퍼레이션에 동시에 도착할 수 있는데, 이전 상태를 원자적으로
 * 받아 두면 "abort 된 것이었는지"를 나중에 판단할 수 있고 처리도 한 번만 된다.
 *
 * 위 영어 주석의 앞부분은 왜 sqhd 를 무시해도 되는지 설명한다 -- 리눅스는
 * 태그셋 크기를 큐 깊이에 맞춰 잡으므로 제출 속도 조절에 sqhd 를 쓰지 않는다.
 * 그래서 CQE 를 지어낼 때 sqid·sqhd·command_id 를 채우지 않는다.
 *
 * 실행 컨텍스트: LLDD 의 완료 콜백. 인터럽트 문맥일 수 있어 잠들면 안 된다.
 *
 * 호출 체인:
 *   HBA 인터럽트 → LLDD → [이 함수] → nvme_try_complete_req / ioerr_work
 */
static void
nvme_fc_fcpio_done(struct nvmefc_fcp_req *req)
{
	struct nvme_fc_fcp_op *op = fcp_req_to_fcp_op(req);	/* [한국어] LLDD 가 준 공개 부분에서 우리 오퍼레이션을 되찾는다 */
	struct request *rq = op->rq;
	struct nvmefc_fcp_req *freq = &op->fcp_req;
	struct nvme_fc_ctrl *ctrl = op->ctrl;
	struct nvme_fc_queue *queue = op->queue;
	struct nvme_completion *cqe = &op->rsp_iu.cqe;	/* [한국어] ERSP 가 온 경우에만 유효한 실제 CQE */
	struct nvme_command *sqe = &op->cmd_iu.sqe;	/* [한국어] 보냈던 명령 — command_id 대조에 쓴다 */
	__le16 status = cpu_to_le16(NVME_SC_SUCCESS << 1);	/* [한국어] 기본은 성공. 아래 검사들이 실패를 덮어쓴다 */
	union nvme_result result;
	bool terminate_assoc = true;	/* [한국어] 기본이 '세션 종료'다. 정상 완료에 도달해야만 아래에서 false 로 바뀐다 */
	int opstate;

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
	/* [한국어] 위 영어 주석의 요지 둘. (1) 리눅스는 태그셋을 큐 깊이에 맞춰 잡아
	 * 제출 속도 조절에 sqhd 를 쓰지 않으므로, CQE 를 지어낼 때 sqid·sqhd·
	 * command_id 를 채우지 않는다. (2) 전송 계층이 감지한 개별 I/O 실패는
	 * 양쪽의 미완료 I/O 개수 인식을 어긋나게 하므로, 스펙이 연결과 association
	 * 종료를 요구한다. terminate_assoc 의 기본값이 true 인 이유가 이것이다. */

	opstate = atomic_xchg(&op->state, FCPOP_STATE_COMPLETE);	/* [한국어] 이전 상태를 원자적으로 받아 둔다 — abort 와 완료가 겹쳐도 한 번만 처리된다 */

	fc_dma_sync_single_for_cpu(ctrl->lport->dev, op->fcp_req.rspdma,	/* [한국어] HBA 가 써 넣은 응답 IU 를 CPU 가 읽기 전에 캐시를 맞춘다 */
				sizeof(op->rsp_iu), DMA_FROM_DEVICE);

	if (opstate == FCPOP_STATE_ABORTED)	/* [한국어] 우리가 취소한 것이었다 */
		status = cpu_to_le16(NVME_SC_HOST_ABORTED_CMD << 1);
	else if (freq->status) {	/* [한국어] LLDD 가 전송 자체의 실패를 보고했다 */
		status = cpu_to_le16(NVME_SC_HOST_PATH_ERROR << 1);	/* [한국어] 경로 오류로 표시 — 다중 경로가 있으면 다른 경로로 넘어간다 */
		dev_info(ctrl->ctrl.device,
			"NVME-FC{%d}: io failed due to lldd error %d\n",
			ctrl->cnum, freq->status);
	}

	/*
	 * For the linux implementation, if we have an unsuccessful
	 * status, the blk-mq layer can typically be called with the
	 * non-zero status and the content of the cqe isn't important.
	 */
	if (status)	/* [한국어] 위 영어 주석대로 실패면 CQE 내용을 볼 필요가 없다 */
		goto done;

	/*
	 * command completed successfully relative to the wire
	 * protocol. However, validate anything received and
	 * extract the status and result from the cqe (create it
	 * where necessary).
	 */

	switch (freq->rcv_rsplen) {	/* [한국어] 응답 길이가 곧 응답의 종류다 */

	case 0:
	case NVME_FC_SIZEOF_ZEROS_RSP:
		/*
		 * No response payload or 12 bytes of payload (which
		 * should all be zeros) are considered successful and
		 * no payload in the CQE by the transport.
		 */
		/* [한국어] 위 영어 주석대로 성공한 명령은 응답 페이로드를 생략할 수 있다.
		 * 대역폭을 아끼는 최적화이며, 이 경우 CQE 를 여기서 지어낸다. */
		if (freq->transferred_length !=	/* [한국어] 다만 실제로 옮긴 바이트 수는 요청한 것과 같아야 한다 */
		    be32_to_cpu(op->cmd_iu.data_len)) {
			status = cpu_to_le16(NVME_SC_HOST_PATH_ERROR << 1);	/* [한국어] 다르면 데이터가 잘렸다는 뜻이라 성공으로 볼 수 없다 */
			dev_info(ctrl->ctrl.device,
				"NVME-FC{%d}: io failed due to bad transfer "
				"length: %d vs expected %d\n",
				ctrl->cnum, freq->transferred_length,
				be32_to_cpu(op->cmd_iu.data_len));
			goto done;
		}
		result.u64 = 0;	/* [한국어] 지어낸 CQE 의 결과값은 0 */
		break;

	case sizeof(struct nvme_fc_ersp_iu):
		/*
		 * The ERSP IU contains a full completion with CQE.
		 * Validate ERSP IU and look at cqe.
		 */
		/* [한국어] 완전한 응답이 왔다. 원격이 보낸 것이므로 그대로 믿지 않고 검증한다. */
		if (unlikely(be16_to_cpu(op->rsp_iu.iu_len) !=	/* [한국어] IU 가 스스로 신고한 길이가 실제 수신 길이와 맞는가 */
					(freq->rcv_rsplen / 4) ||
			     be32_to_cpu(op->rsp_iu.xfrd_len) !=	/* [한국어] IU 가 신고한 전송량이 LLDD 가 센 것과 맞는가 */
					freq->transferred_length ||
			     op->rsp_iu.ersp_result ||	/* [한국어] FC 계층 자체의 결과 코드가 0 인가 */
			     sqe->common.command_id != cqe->command_id)) {	/* [한국어] 우리가 보낸 명령에 대한 응답이 맞는가 — 짝이 어긋나면 엉뚱한 요청을 완료시킨다 */
			status = cpu_to_le16(NVME_SC_HOST_PATH_ERROR << 1);
			dev_info(ctrl->ctrl.device,
				"NVME-FC{%d}: io failed due to bad NVMe_ERSP: "
				"iu len %d, xfr len %d vs %d, status code "
				"%d, cmdid %d vs %d\n",
				ctrl->cnum, be16_to_cpu(op->rsp_iu.iu_len),
				be32_to_cpu(op->rsp_iu.xfrd_len),
				freq->transferred_length,
				op->rsp_iu.ersp_result,
				sqe->common.command_id,
				cqe->command_id);
			goto done;
		}
		result = cqe->result;	/* [한국어] 검증을 통과했으니 실제 CQE 의 값을 쓴다 */
		status = cqe->status;
		break;

	default:	/* [한국어] 0 도 12도 전체 IU 도 아닌 길이 — 프로토콜을 벗어났다 */
		status = cpu_to_le16(NVME_SC_HOST_PATH_ERROR << 1);
		dev_info(ctrl->ctrl.device,
			"NVME-FC{%d}: io failed due to odd NVMe_xRSP iu "
			"len %d\n",
			ctrl->cnum, freq->rcv_rsplen);
		goto done;
	}

	terminate_assoc = false;	/* [한국어] 여기까지 왔다는 것은 전송 계층이 정상이었다는 뜻 — 세션을 끊을 이유가 없다 */

done:
	if (op->flags & FCOP_FLAGS_AEN) {	/* [한국어] 비동기 이벤트는 blk-mq 요청이 없어 경로가 다르다 */
		nvme_complete_async_event(&queue->ctrl->ctrl, status, &result);
		__nvme_fc_fcpop_chk_teardowns(ctrl, op, opstate);	/* [한국어] 해체를 기다리는 쪽이 있으면 iocnt 를 내려 깨운다 */
		atomic_set(&op->state, FCPOP_STATE_IDLE);	/* [한국어] AEN 오퍼레이션은 해제하지 않고 재사용한다 */
		op->flags = FCOP_FLAGS_AEN;	/* clear other flags */
		/* [한국어] 위 영어 주석대로 AEN 표시만 남기고 TERMIO 같은 나머지는 지운다 */
		nvme_fc_ctrl_put(ctrl);
		goto check_error;
	}

	__nvme_fc_fcpop_chk_teardowns(ctrl, op, opstate);
	if (!nvme_try_complete_req(rq, status, result))	/* [한국어] 코어가 재시도·페일오버로 가로채지 않았다면 */
		nvme_fc_complete_rq(rq);	/* [한국어] 매핑을 풀고 blk-mq 에 완료를 알린다 */

check_error:
	if (terminate_assoc &&	/* [한국어] 전송 계층 오류였다면 스펙상 세션을 끊어야 한다 */
	    nvme_ctrl_state(&ctrl->ctrl) != NVME_CTRL_RESETTING)	/* [한국어] 이미 리셋 중이면 중복해서 걸지 않는다 */
		queue_work(nvme_reset_wq, &ctrl->ioerr_work);	/* [한국어] 실제 종료는 잠들 수 있는 워크큐 문맥에서 한다 */
}

static int
__nvme_fc_init_request(struct nvme_fc_ctrl *ctrl,
		struct nvme_fc_queue *queue, struct nvme_fc_fcp_op *op,
		struct request *rq, u32 rqno)	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
{
	struct nvme_fcp_op_w_sgl *op_w_sgl =	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
		container_of(op, typeof(*op_w_sgl), op);
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
		cmdiu->rsv_cat = fccmnd_set_cat_css(0,
					(NVME_CC_CSS_NVM >> NVME_CC_CSS_SHIFT));
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		cmdiu->rsv_cat = fccmnd_set_cat_admin(0);

	op->fcp_req.cmddma = fc_dma_map_single(ctrl->lport->dev,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				&op->cmd_iu, sizeof(op->cmd_iu), DMA_TO_DEVICE);
	if (fc_dma_mapping_error(ctrl->lport->dev, op->fcp_req.cmddma)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(ctrl->dev,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"FCP Op failed - cmdiu dma mapping failed.\n");
		ret = -EFAULT;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_on_error;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}

	op->fcp_req.rspdma = fc_dma_map_single(ctrl->lport->dev,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				&op->rsp_iu, sizeof(op->rsp_iu),
				DMA_FROM_DEVICE);
	if (fc_dma_mapping_error(ctrl->lport->dev, op->fcp_req.rspdma)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(ctrl->dev,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"FCP Op failed - rspiu dma mapping failed.\n");
		ret = -EFAULT;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}

	atomic_set(&op->state, FCPOP_STATE_IDLE);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
out_on_error:
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static int
nvme_fc_init_request(struct blk_mq_tag_set *set, struct request *rq,
		unsigned int hctx_idx, unsigned int numa_node)
{
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
}

/*
 * [한국어]
 * nvme_fc_init_aen_ops - 비동기 이벤트(AEN)용 상주 오퍼레이션들을 준비한다
 *
 * @ctrl: 대상 컨트롤러
 * @return: 0 이면 성공. -ENOMEM 이면 할당 실패.
 *
 * AEN 은 다른 명령과 성격이 다르다. 컨트롤러가 알릴 것이 생길 때까지
 * 무한정 미완료로 남아 있는 명령이라, blk-mq 태그를 하나 붙들고 있으면
 * 그만큼 I/O 에 쓸 태그가 줄어든다. 그래서 태그셋 밖에 따로 잡아 둔다.
 *
 * command_id 를 NVME_AQ_BLK_MQ_DEPTH + i 로 주는 것이 그 분리의 표현이다.
 * admin 태그셋이 쓰는 범위 바로 위를 쓰므로 실제 태그와 절대 겹치지 않고,
 * 완료가 돌아왔을 때 이 값만 보고 "AEN 이구나" 판정할 수 있다.
 *
 * 위 영어 주석이 지적하듯 코어가 이 command_id 를 덮어쓸 수 있다. 그래도
 * 여기서 채워 두는 것은 코어가 관여하지 않는 경로에서도 값이 필요하기
 * 때문이다.
 *
 * private 영역을 요청마다 따로 할당하는 이유: LLDD 가 요청당 사적 공간을
 * 요구하는데, AEN 은 태그셋 할당을 거치지 않으므로 그 공간이 자동으로
 * 딸려 오지 않는다. 여기서 직접 잡아 붙인다.
 *
 * 실행 컨텍스트: create_association 경로. 잠들 수 있다.
 *
 * 호출 체인:
 *   nvme_fc_create_association → [이 함수] → __nvme_fc_init_request
 */
static int
nvme_fc_init_aen_ops(struct nvme_fc_ctrl *ctrl)
{
	struct nvme_fc_fcp_op *aen_op;
	struct nvme_fc_cmd_iu *cmdiu;
	struct nvme_command *sqe;
	void *private = NULL;
	int i, ret;

	aen_op = ctrl->aen_ops;	/* [한국어] 컨트롤러에 내장된 AEN 오퍼레이션 배열 */
	for (i = 0; i < NVME_NR_AEN_COMMANDS; i++, aen_op++) {
		if (ctrl->lport->ops->fcprqst_priv_sz) {
			private = kzalloc(ctrl->lport->ops->fcprqst_priv_sz,	/* [한국어] 태그셋을 거치지 않으므로 LLDD 사적 공간을 직접 잡아 준다 */
						GFP_KERNEL);
			if (!private)
				return -ENOMEM;
		}

		cmdiu = &aen_op->cmd_iu;
		sqe = &cmdiu->sqe;
		ret = __nvme_fc_init_request(ctrl, &ctrl->queues[0],	/* [한국어] AEN 은 admin 큐(0번)로만 나간다 */
				aen_op, (struct request *)NULL,	/* [한국어] 대응하는 blk-mq 요청이 없다 — AEN 의 정의 그대로다 */
				(NVME_AQ_BLK_MQ_DEPTH + i));	/* [한국어] admin 태그 범위 바로 위를 써 실제 태그와 겹치지 않게 한다 */
		if (ret) {
			kfree(private);	/* [한국어] 방금 잡은 것만 해제한다 — 앞의 것들은 호출자의 term_aen_ops 가 정리한다 */
			return ret;
		}

		aen_op->flags = FCOP_FLAGS_AEN;	/* [한국어] 완료 경로가 이 비트로 일반 요청과 다른 처리를 고른다 */
		aen_op->fcp_req.private = private;

		memset(sqe, 0, sizeof(*sqe));
		sqe->common.opcode = nvme_admin_async_event;	/* [한국어] 컨트롤러가 알릴 것이 생길 때까지 미완료로 남는 명령 */
		/* Note: core layer may overwrite the sqe.command_id value */
		sqe->common.command_id = NVME_AQ_BLK_MQ_DEPTH + i;	/* [한국어] 위 영어 주석대로 코어가 덮어쓸 수 있으나, 코어가 관여하지 않는 경로를 위해 채워 둔다 */
	}
	return 0;
}

/*
 * [한국어]
 * nvme_fc_term_aen_ops - AEN 전용 오퍼레이션들의 자원을 되돌린다
 *
 * @ctrl: 대상 컨트롤러
 * @return: 없음
 *
 * init_aen_ops 의 짝이다. 순서가 중요하다 -- 워크를 먼저 취소해야 한다.
 * async_event_work 는 AEN 을 다시 거는 일을 하므로, 자원을 푼 뒤에 그것이
 * 돌면 이미 해제된 버퍼로 명령을 보낸다. _sync 라 실행 중인 것이 끝날
 * 때까지 기다린다.
 *
 * private 를 해제한 뒤 NULL 로 지우는 것도 의도적이다. 재연결에서 다시
 * init 할 때 이전 포인터가 남아 있으면 이중 해제로 이어진다.
 *
 * 실행 컨텍스트: 세션 해체 경로. cancel_work_sync 로 잠들 수 있다.
 *
 * 호출 체인:
 *   nvme_fc_delete_association / _create_association 실패 경로 → [이 함수]
 */
static void
nvme_fc_term_aen_ops(struct nvme_fc_ctrl *ctrl)
{
	struct nvme_fc_fcp_op *aen_op;
	int i;

	cancel_work_sync(&ctrl->ctrl.async_event_work);	/* [한국어] 먼저 취소한다 — 자원을 푼 뒤 돌면 해제된 버퍼로 명령을 보낸다 */
	aen_op = ctrl->aen_ops;
	for (i = 0; i < NVME_NR_AEN_COMMANDS; i++, aen_op++) {
		__nvme_fc_exit_request(ctrl, aen_op);	/* [한국어] IU 버퍼의 DMA 매핑을 푼다 */

		kfree(aen_op->fcp_req.private);	/* [한국어] init 에서 손으로 잡은 LLDD 사적 영역 */
		aen_op->fcp_req.private = NULL;	/* [한국어] 재연결에서 다시 init 할 때 이중 해제가 되지 않도록 지운다 */
	}
}

/*
 * [한국어]
 * __nvme_fc_init_hctx - blk-mq 하드웨어 큐와 이 드라이버의 큐를 서로 연결한다
 *
 * @hctx: blk-mq 가 만든 하드웨어 큐 문맥
 * @data: 태그셋의 driver_data — 컨트롤러다
 * @qidx: 몇 번째 하드웨어 큐인가
 * @return: 항상 0
 *
 * 양방향 연결을 만드는 것이 전부다. queue_rq 는 hctx->driver_data 로
 * 우리 큐를 찾고, 반대로 큐 쪽에서 hctx 가 필요한 경우(요청 순회 등)를
 * 위해 queue->hctx 도 채운다.
 *
 * 실행 컨텍스트: 태그셋 초기화. 잠들지 않는다.
 *
 * 호출 체인:
 *   blk_mq_alloc_tag_set → ops->init_hctx → [이 함수]
 */
static inline int
__nvme_fc_init_hctx(struct blk_mq_hw_ctx *hctx, void *data, unsigned int qidx)
{
	struct nvme_fc_ctrl *ctrl = to_fc_ctrl(data);
	struct nvme_fc_queue *queue = &ctrl->queues[qidx];

	hctx->driver_data = queue;	/* [한국어] queue_rq 가 이 포인터로 우리 큐를 찾는다 */
	queue->hctx = hctx;	/* [한국어] 반대 방향도 채워 큐에서 hctx 가 필요한 경우에 대비한다 */
	return 0;
}

static int
nvme_fc_init_hctx(struct blk_mq_hw_ctx *hctx, void *data, unsigned int hctx_idx)
{
	return __nvme_fc_init_hctx(hctx, data, hctx_idx + 1);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static int
nvme_fc_init_admin_hctx(struct blk_mq_hw_ctx *hctx, void *data,
		unsigned int hctx_idx)
{
	return __nvme_fc_init_hctx(hctx, data, hctx_idx);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

/*
 * [한국어]
 * nvme_fc_init_queue - 큐 구조체 하나를 초기 상태로 세운다
 *
 * @ctrl: 소속 컨트롤러
 * @idx:  큐 번호. 0 이 admin, 1 이상이 I/O.
 * @return: 없음
 *
 * 재연결에서도 불리므로 memset 으로 시작한다. 이전 세션의 connection_id 나
 * csn 이 남아 있으면 새 세션에서 타겟이 알 수 없는 값을 받게 된다.
 *
 * cmnd_capsule_len 이 큐 번호에 따라 갈리는 것이 이 함수의 실질적 내용이다.
 * I/O 큐는 ioccsz(컨트롤러가 Identify 로 알려 준 캡슐 크기, 16바이트 단위)를
 * 쓰고, admin 큐는 SQE 크기 그대로다. 즉 admin 캡슐에는 여유 공간이 없어
 * 인라인 데이터를 실을 수 없다 -- 다른 트랜스포트에서도 반복되는 구조다.
 *
 * 아래 긴 영어 주석은 하지 않기로 한 설계를 남긴 것이다. SQE/CQE 버퍼를
 * 미리 잡아 DMA 매핑해 두는 방식을 검토했으나, LLDD 가 그 형식을 그대로
 * 쓰는 경우가 드물어(대부분 자체 메시지 API 를 쓴다) 이득이 없다는 판단이다.
 *
 * 실행 컨텍스트: 큐 구성 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   nvme_fc_init_io_queues / _create_association → [이 함수]
 */
static void
nvme_fc_init_queue(struct nvme_fc_ctrl *ctrl, int idx)
{
	struct nvme_fc_queue *queue;

	queue = &ctrl->queues[idx];
	memset(queue, 0, sizeof(*queue));	/* [한국어] 재연결에서도 불리므로 이전 세션의 connection_id·csn 을 반드시 지운다 */
	queue->ctrl = ctrl;
	queue->qnum = idx;
	atomic_set(&queue->csn, 0);	/* [한국어] 명령 순서 번호를 0 부터 다시 센다 */
	queue->dev = ctrl->dev;

	if (idx > 0)	/* [한국어] I/O 큐 */
		queue->cmnd_capsule_len = ctrl->ctrl.ioccsz * 16;	/* [한국어] Identify 가 알려 준 캡슐 크기(16바이트 단위) */
	else
		queue->cmnd_capsule_len = sizeof(struct nvme_command);	/* [한국어] admin 은 SQE 크기 그대로 — 여유가 없어 인라인을 실을 수 없다 */

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
	/* [한국어] 위 영어 주석은 채택하지 않은 설계를 남긴 것이다. SQE/CQE 버퍼를
	 * 미리 잡아 매핑해 두는 방식을 검토했으나, LLDD 가 NVMe 원형 SQE/CQE 를
	 * 그대로 쓰는 경우가 드물어 이득이 없다는 판단이다. */
}

/*
 * This routine terminates a queue at the transport level.
 * The transport has already ensured that all outstanding ios on
 * the queue have been terminated.
 * The transport will send a Disconnect LS request to terminate
 * the queue's connection. Termination of the admin queue will also
 * terminate the association at the target.
 */
static void
nvme_fc_free_queue(struct nvme_fc_queue *queue)
{
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
}

static void
__nvme_fc_delete_hw_queue(struct nvme_fc_ctrl *ctrl,
	struct nvme_fc_queue *queue, unsigned int qidx)
{
	if (ctrl->lport->ops->delete_queue)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ctrl->lport->ops->delete_queue(&ctrl->lport->localport, qidx,
				queue->lldd_handle);
	queue->lldd_handle = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}

static void
nvme_fc_free_io_queues(struct nvme_fc_ctrl *ctrl)
{
	int i;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	for (i = 1; i < ctrl->ctrl.queue_count; i++)	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		nvme_fc_free_queue(&ctrl->queues[i]);
}

static int
__nvme_fc_create_hw_queue(struct nvme_fc_ctrl *ctrl,
	struct nvme_fc_queue *queue, unsigned int qidx, u16 qsize)
{
	int ret = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	queue->lldd_handle = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ctrl->lport->ops->create_queue)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = ctrl->lport->ops->create_queue(&ctrl->lport->localport,
				qidx, qsize, &queue->lldd_handle);

	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static void
nvme_fc_delete_hw_io_queues(struct nvme_fc_ctrl *ctrl)
{
	struct nvme_fc_queue *queue = &ctrl->queues[ctrl->ctrl.queue_count - 1];	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	int i;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	for (i = ctrl->ctrl.queue_count - 1; i >= 1; i--, queue--)	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		__nvme_fc_delete_hw_queue(ctrl, queue, i);
}

static int
nvme_fc_create_hw_io_queues(struct nvme_fc_ctrl *ctrl, u16 qsize)
{
	struct nvme_fc_queue *queue = &ctrl->queues[1];	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	int i, ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	for (i = 1; i < ctrl->ctrl.queue_count; i++, queue++) {	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		ret = __nvme_fc_create_hw_queue(ctrl, queue, i, qsize);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			goto delete_queues;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

delete_queues:
	for (; i > 0; i--)	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		__nvme_fc_delete_hw_queue(ctrl, &ctrl->queues[i], i);
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

/*
 * [한국어]
 * nvme_fc_connect_io_queues - I/O 큐들을 두 층 모두에서 연결한다
 *
 * @ctrl:  대상 컨트롤러
 * @qsize: 각 큐의 깊이
 * @return: 0 이면 전부 성공. 음수면 그 지점에서 중단됐다.
 *
 * 큐마다 두 번의 연결이 필요하다는 점이 FC 의 특징이다. 먼저 Create
 * Connection LS 로 FC 채널을 열어 connection_id 를 받고, 그다음 그 채널 위로
 * Fabrics Connect 명령을 보내 NVMe 큐를 세운다. 두 층이 모두 서야 LIVE 다.
 *
 * ersp_ratio 를 qsize/5 로 주는 것에 주목할 것. 전체 응답 IU 를 다섯 번에
 * 한 번꼴로만 받겠다는 뜻이며, 나머지는 축약 응답으로 받아 링크 대역폭을
 * 아낀다. fcpio_done 이 응답 길이로 세 형태를 가르는 이유가 여기 있다.
 *
 * 실패 시 곧바로 break 하고 되감지 않는 것은 호출자의 몫이기 때문이다.
 * create_io_queues 의 goto 사다리가 이미 연 채널들을 정리한다.
 *
 * 실행 컨텍스트: 세션 수립 경로. LS 응답을 기다리며 잠든다.
 *
 * 호출 체인:
 *   nvme_fc_create_io_queues / _recreate_io_queues → [이 함수]
 *     → nvme_fc_connect_queue → nvmf_connect_io_queue
 */
static int
nvme_fc_connect_io_queues(struct nvme_fc_ctrl *ctrl, u16 qsize)
{
	int i, ret = 0;

	for (i = 1; i < ctrl->ctrl.queue_count; i++) {	/* [한국어] 1 부터 — 0 번 admin 은 이미 연결돼 있다 */
		ret = nvme_fc_connect_queue(ctrl, &ctrl->queues[i], qsize,	/* [한국어] 1층: Create Connection LS 로 FC 채널을 연다 */
					(qsize / 5));	/* [한국어] ersp_ratio — 전체 응답 IU 를 다섯 번에 한 번만 받아 대역폭을 아낀다 */
		if (ret)
			break;	/* [한국어] 되감기는 호출자의 goto 사다리가 맡는다 */
		ret = nvmf_connect_io_queue(&ctrl->ctrl, i);	/* [한국어] 2층: 그 채널 위로 NVMe Fabrics Connect */
		if (ret)
			break;

		set_bit(NVME_FC_Q_LIVE, &ctrl->queues[i].flags);	/* [한국어] 두 층이 모두 섰다 — 이제 I/O 를 받을 수 있다 */
	}

	return ret;
}

static void
nvme_fc_init_io_queues(struct nvme_fc_ctrl *ctrl)
{
	int i;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	for (i = 1; i < ctrl->ctrl.queue_count; i++)	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		nvme_fc_init_queue(ctrl, i);
}

/*
 * [한국어]
 * nvme_fc_ctrl_free - 마지막 참조가 사라진 컨트롤러 객체를 해제한다
 *
 * @ref: 0 이 된 kref
 * @return: 없음
 *
 * 여기 도달했다는 것은 세션 해체가 끝났고 진행 중인 I/O 도 모두 완료됐다는
 * 뜻이다. 큐 배열, 장치 참조, 포트 참조, 인스턴스 번호, 연결 옵션을 차례로
 * 되돌린다.
 *
 * 순서에 이유가 있다. 원격 포트의 목록에서 먼저 빼야 그 포트를 훑는 경로
 * (delete_controllers, unregister_remoteport)가 해제 중인 컨트롤러를 집지
 * 않는다. 포트 참조는 그 뒤에 놓는데, 이것이 마지막 참조라면 free_rport 가
 * 이어서 불리며 그때 ctrl_list 가 비어 있어야 그쪽 WARN 을 통과한다.
 *
 * opts 를 여기서 푸는 것은 이 컨트롤러가 그 소유자이기 때문이다. create_ctrl
 * 이 파싱한 옵션은 컨트롤러 수명과 함께 간다.
 *
 * 실행 컨텍스트: kref_put 이 부른다.
 *
 * 호출 체인:
 *   nvme_fc_ctrl_put → kref_put → [이 함수] → nvme_fc_rport_put
 */
static void
nvme_fc_ctrl_free(struct kref *ref)
{
	struct nvme_fc_ctrl *ctrl =
		container_of(ref, struct nvme_fc_ctrl, ref);
	unsigned long flags;

	/* remove from rport list */
	spin_lock_irqsave(&ctrl->rport->lock, flags);
	list_del(&ctrl->ctrl_list);	/* [한국어] 먼저 빼야 포트를 훑는 경로가 해제 중인 컨트롤러를 집지 않는다 */
	spin_unlock_irqrestore(&ctrl->rport->lock, flags);

	kfree(ctrl->queues);	/* [한국어] 큐 배열. 개별 큐 자원은 해체 때 이미 정리됐다 */

	put_device(ctrl->dev);
	nvme_fc_rport_put(ctrl->rport);	/* [한국어] 이것이 마지막 참조면 free_rport 가 이어진다 — 그때 ctrl_list 는 이미 비어 있다 */

	ida_free(&nvme_fc_ctrl_cnt, ctrl->cnum);	/* [한국어] /dev/nvmeX 의 X 를 반납해 재사용 가능하게 한다 */
	if (ctrl->ctrl.opts)
		nvmf_free_options(ctrl->ctrl.opts);	/* [한국어] 파싱된 연결 옵션은 이 컨트롤러가 소유한다 */
	kfree(ctrl);
}

static void
nvme_fc_ctrl_put(struct nvme_fc_ctrl *ctrl)
{
	kref_put(&ctrl->ref, nvme_fc_ctrl_free);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
}

static int
nvme_fc_ctrl_get(struct nvme_fc_ctrl *ctrl)
{
	return kref_get_unless_zero(&ctrl->ref);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

/*
 * All accesses from nvme core layer done - can now free the
 * controller. Called after last nvme_put_ctrl() call
 */
/*
 * [한국어]
 * nvme_fc_free_ctrl - 코어가 컨트롤러 해제를 알릴 때 참조를 하나 놓는다
 *
 * @nctrl: 해제되는 컨트롤러
 * @return: 없음
 *
 * 코어의 free_ctrl vtable 진입점이지만 실제로 해제하지는 않는다. 이
 * 드라이버는 kref 로 수명을 관리하므로, 여기서는 코어가 들고 있던 몫을
 * 놓을 뿐이다. 마지막 참조라면 nvme_fc_ctrl_free 가 이어진다.
 *
 * WARN 은 코어가 넘긴 포인터가 정말 우리 구조체 안의 것인지 확인한다.
 * container_of 로 되찾은 결과가 원래 포인터와 같아야 한다.
 *
 * 실행 컨텍스트: 코어의 컨트롤러 해제 경로.
 *
 * 호출 체인: nvme_free_ctrl → ops->free_ctrl → [이 함수] → nvme_fc_ctrl_put
 */
static void
nvme_fc_free_ctrl(struct nvme_ctrl *nctrl)
{
	struct nvme_fc_ctrl *ctrl = to_fc_ctrl(nctrl);

	WARN_ON(nctrl != &ctrl->ctrl);	/* [한국어] container_of 결과가 원래 포인터와 같아야 한다 */

	nvme_fc_ctrl_put(ctrl);	/* [한국어] 코어가 들고 있던 몫을 놓는다. 마지막이면 ctrl_free 가 이어진다 */
}

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
/*
 * [한국어]
 * nvme_fc_terminate_exchange - 바쁜 요청 하나의 FC 익스체인지를 끝낸다
 *
 * @req:  blk_mq_tagset_busy_iter 가 넘긴 진행 중 요청
 * @data: 순회 시 넘긴 컨트롤러
 * @return: 항상 true — 순회를 계속한다는 뜻
 *
 * 위 영어 주석이 이 콜백의 존재 이유를 밝힌다. FC 는 I/O 마다 익스체인지를
 * 할당하므로 요청을 실패 처리하는 것만으로는 그 하드웨어 자원이 풀리지
 * 않는다. LLDD 에 연락해 익스체인지를 끝내야 하고, 그러면 LLDD 가 평소의
 * 완료 경로를 abort 상태로 불러 준다. 그 완료가 요청을 블록 계층으로
 * 오류와 함께 돌려보낸다.
 *
 * CANCELLED 를 먼저 세우는 이유: 완료 경로가 이 비트를 보고 재시도나
 * 페일오버로 넘기지 않고 취소로 처리한다.
 *
 * 실행 컨텍스트: blk_mq_tagset_busy_iter 순회 문맥.
 *
 * 호출 체인:
 *   __nvme_fc_abort_outstanding_ios → blk_mq_tagset_busy_iter → [이 함수]
 *     → __nvme_fc_abort_op → LLDD 의 fcp_abort
 */
static bool nvme_fc_terminate_exchange(struct request *req, void *data)
{
	struct nvme_ctrl *nctrl = data;
	struct nvme_fc_ctrl *ctrl = to_fc_ctrl(nctrl);
	struct nvme_fc_fcp_op *op = blk_mq_rq_to_pdu(req);

	op->nreq.flags |= NVME_REQ_CANCELLED;	/* [한국어] 완료 경로가 재시도가 아니라 취소로 처리하게 한다 */
	__nvme_fc_abort_op(ctrl, op);	/* [한국어] 익스체인지를 하드웨어 수준에서 끝낸다 */
	return true;	/* [한국어] 순회를 계속한다 */
}

/*
 * This routine runs through all outstanding commands on the association
 * and aborts them.  This routine is typically called by the
 * delete_association routine. It is also called due to an error during
 * reconnect. In that scenario, it is most likely a command that initializes
 * the controller, including fabric Connect commands on io queues, that
 * may have timed out or failed thus the io must be killed for the connect
 * thread to see the error.
 */
/*
 * [한국어]
 * __nvme_fc_abort_outstanding_ios - 진행 중인 모든 I/O 를 LLDD 수준에서 끝낸다
 *
 * @ctrl:         대상 컨트롤러
 * @start_queues: 정리 후 큐를 다시 열지 여부. 재연결 도중이면 true 로 열어
 *                새 요청이 빨리 실패하게 하고, 완전 해체면 false 로 둔다.
 * @return: 없음
 *
 * FC 가 다른 트랜스포트와 결정적으로 다른 지점이 위 두 번째 영어 주석에
 * 적혀 있다. FC 는 I/O 하나마다 FC 익스체인지를 할당하므로, 요청을 그냥
 * 실패 처리하는 것으로 끝나지 않는다. LLDD 에 연락해 익스체인지를 실제로
 * 종료시켜야 그 자원이 풀린다. 그래서 blk_mq_tagset_busy_iter 로 바쁜 요청을
 * 훑어 각각 terminate_exchange 를 호출한다.
 *
 * 그 뒤 wait_completed_request 로 기다리는 이유: 익스체인지를 끝내면 LLDD 가
 * 평소의 완료 경로를 abort 상태로 부른다. 즉 완료는 비동기로 돌아오며,
 * 그것을 다 받기 전에 큐를 해제하면 이미 사라진 요청에 완료가 도착한다.
 *
 * 세 번째 영어 주석은 왜 정상 종료 절차(nvme_disable_ctrl)를 밟지 않는지
 * 설명한다 -- 방금 다수의 I/O 를 취소했고 그 완료를 기다릴 참인데, 종료
 * 레지스터를 쓰겠다고 새 명령을 보내면 익스체인지가 또 생긴다. 링크 상태도
 * 알 수 없으므로 그냥 keep-alive 실패로 죽게 둔다.
 *
 * 맨 앞에서 큐의 LIVE 비트를 먼저 내리는 것은, 취소하는 동안 새 요청이
 * 들어와 또 익스체인지를 만드는 것을 막기 위해서다.
 *
 * 실행 컨텍스트: 워크큐. 여러 곳에서 기다리므로 잠들 수 있어야 한다.
 *
 * 호출 체인:
 *   nvme_fc_delete_association / _error_recovery → [이 함수]
 *     → blk_mq_tagset_busy_iter → nvme_fc_terminate_exchange → LLDD
 */
static void
__nvme_fc_abort_outstanding_ios(struct nvme_fc_ctrl *ctrl, bool start_queues)
{
	int q;

	/*
	 * if aborting io, the queues are no longer good, mark them
	 * all as not live.
	 */
	/* [한국어] 위 영어 주석대로 먼저 LIVE 를 내린다. 취소하는 동안 새 요청이
	 * 들어와 또 익스체인지를 만드는 것을 막는 것이 목적이다. */
	if (ctrl->ctrl.queue_count > 1) {
		for (q = 1; q < ctrl->ctrl.queue_count; q++)
			clear_bit(NVME_FC_Q_LIVE, &ctrl->queues[q].flags);
	}
	clear_bit(NVME_FC_Q_LIVE, &ctrl->queues[0].flags);	/* [한국어] admin 큐도 마찬가지 */

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
	/* [한국어] 위 영어 주석이 FC 의 핵심 차이를 밝힌다 — I/O 하나마다 FC
	 * 익스체인지가 할당돼 있어, 요청을 실패 처리하는 것만으로는 그 자원이
	 * 풀리지 않는다. LLDD 에 연락해 익스체인지를 실제로 끝내야 한다. */
	if (ctrl->ctrl.queue_count > 1) {
		nvme_quiesce_io_queues(&ctrl->ctrl);	/* [한국어] 새 요청 유입을 멈춘다 */
		nvme_sync_io_queues(&ctrl->ctrl);	/* [한국어] 이미 진입한 제출이 끝나기를 기다린다 */
		blk_mq_tagset_busy_iter(&ctrl->tag_set,	/* [한국어] 바쁜 태그를 훑어 */
				nvme_fc_terminate_exchange, &ctrl->ctrl);	/* [한국어] 각각 LLDD 에 익스체인지 종료를 요청한다 */
		blk_mq_tagset_wait_completed_request(&ctrl->tag_set);	/* [한국어] 종료 완료가 비동기로 돌아오므로 다 받을 때까지 기다린다 */
		if (start_queues)
			nvme_unquiesce_io_queues(&ctrl->ctrl);	/* [한국어] 재연결 중이면 다시 열어 새 요청이 빨리 실패하게 한다 */
	}

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
	/* [한국어] 위 영어 주석대로 정상 종료 절차를 일부러 건너뛴다. 종료
	 * 레지스터를 쓰겠다고 명령을 보내면 익스체인지가 또 생기는데, 방금
	 * 취소한 것들의 완료를 기다릴 참이라 앞뒤가 맞지 않는다. 링크가 살아
	 * 있는지도 알 수 없으므로 keep-alive 실패로 죽게 둔다. */

	/*
	 * clean up the admin queue. Same thing as above.
	 */
	nvme_quiesce_admin_queue(&ctrl->ctrl);	/* [한국어] admin 큐도 같은 절차를 밟는다 */
	blk_sync_queue(ctrl->ctrl.admin_q);
	blk_mq_tagset_busy_iter(&ctrl->admin_tag_set,
				nvme_fc_terminate_exchange, &ctrl->ctrl);
	blk_mq_tagset_wait_completed_request(&ctrl->admin_tag_set);
	if (start_queues)
		nvme_unquiesce_admin_queue(&ctrl->ctrl);
}

/*
 * [한국어]
 * nvme_fc_error_recovery - 세션 수준 오류를 상태에 맞게 처리한다
 *
 * @ctrl:   대상 컨트롤러
 * @errmsg: 로그에 남길 원인 문자열. 호출자가 무엇 때문인지 알려 준다.
 * @return: 없음
 *
 * 같은 "오류"라도 컨트롤러가 어느 상태냐에 따라 해야 할 일이 정반대다.
 * 그래서 이 함수는 사실상 상태 판정 하나로 이뤄져 있다.
 *
 * CONNECTING 이면 -- 위 영어 주석이 열거하듯 재연결 도중의 타임아웃, 타겟이
 * 보낸 Disconnect, 컨트롤러 생성 중의 I/O 오류 -- 여기서 리셋을 걸면 안 된다.
 * 이미 create_association 이 진행 중이고 그쪽에 오류 경로가 있기 때문이다.
 * 진행 중인 I/O 만 취소해 그 절차가 실패를 인지하고 스스로 되감게 한다.
 *
 * LIVE 이면 정상 동작 중에 처음 만난 오류이므로 컨트롤러 리셋으로 간다.
 *
 * 그 밖의 상태 -- RESETTING, DELETING 등 -- 는 아무것도 하지 않는다.
 * 이미 누군가 정리하고 있다는 뜻이고, 겹쳐 걸면 같은 세션을 두 곳에서
 * 내리게 된다.
 *
 * 실행 컨텍스트: 완료 콜백, LS 수신 워크, 타임아웃 등 여러 곳. 잠들지 않는다.
 *
 * 호출 체인:
 *   fcpio_done / ls_disconnect_assoc / nvme_fc_timeout → [이 함수]
 *     → __nvme_fc_abort_outstanding_ios 또는 nvme_reset_ctrl
 */
static void
nvme_fc_error_recovery(struct nvme_fc_ctrl *ctrl, char *errmsg)
{
	enum nvme_ctrl_state state = nvme_ctrl_state(&ctrl->ctrl);	/* [한국어] 판단의 유일한 근거 */

	/*
	 * if an error (io timeout, etc) while (re)connecting, the remote
	 * port requested terminating of the association (disconnect_ls)
	 * or an error (timeout or abort) occurred on an io while creating
	 * the controller.  Abort any ios on the association and let the
	 * create_association error path resolve things.
	 */
	/* [한국어] 위 영어 주석대로, 연결 도중이면 create_association 이 이미
	 * 진행 중이고 그쪽에 되감기 경로가 있다. 여기서 리셋을 겹쳐 걸면 두 곳이
	 * 같은 세션을 내린다. I/O 만 취소해 그 절차가 실패를 인지하게 한다. */
	if (state == NVME_CTRL_CONNECTING) {
		__nvme_fc_abort_outstanding_ios(ctrl, true);	/* [한국어] true — 큐를 다시 열어 새 요청이 빨리 실패하게 한다 */
		dev_warn(ctrl->ctrl.device,
			"NVME-FC{%d}: transport error during (re)connect\n",
			ctrl->cnum);
		return;
	}

	/* Otherwise, only proceed if in LIVE state - e.g. on first error */
	if (state != NVME_CTRL_LIVE)	/* [한국어] RESETTING·DELETING 등은 이미 누가 정리 중이다 */
		return;	/* [한국어] 겹쳐 걸지 않는다 */

	dev_warn(ctrl->ctrl.device,	/* [한국어] 호출자가 준 원인을 남긴다 — 나중에 무엇 때문이었는지 추적하는 근거 */
		"NVME-FC{%d}: transport association event: %s\n",
		ctrl->cnum, errmsg);
	dev_warn(ctrl->ctrl.device,
		"NVME-FC{%d}: resetting controller\n", ctrl->cnum);

	nvme_reset_ctrl(&ctrl->ctrl);	/* [한국어] 정상 동작 중 처음 만난 오류 — 세션을 다시 세운다 */
}

/*
 * [한국어]
 * nvme_fc_timeout - 응답이 오지 않는 요청에 abort 를 걸고 시계를 다시 건다
 *
 * @rq: 시한을 넘긴 요청
 * @return: 항상 BLK_EH_RESET_TIMER
 *
 * TCP·RDMA 와 달리 FC 에는 개별 명령을 취소하는 수단이 있다. 익스체인지마다
 * ABTS 를 보낼 수 있으므로, 연결 전체를 끊지 않고 이 요청 하나만 취소를
 * 시도한다. 그래서 이 함수는 짧다.
 *
 * 항상 RESET_TIMER 를 돌려주는 것이 요점이다. 위 영어 주석이 밝히듯 abort 는
 * 이미 시작됐고, 그 완료가 곧 이 요청을 실패 상태로 끝낸다. 여기서 동기적으로
 * 기다리면 blk-mq 타임아웃 문맥이 그동안 묶이므로, 시계만 다시 걸고 물러난다.
 *
 * abort 요청 자체가 실패하면 -- LLDD 가 거부했거나 이미 끝난 상태였다면 --
 * 그때는 개별 취소로 해결되지 않는다는 뜻이므로 세션 수준 복구로 올린다.
 *
 * 로그에 opcode 와 cdw10/11 까지 남기는 이유: FC 는 링크가 길고 장비가 여러
 * 단을 거치므로, 어떤 명령이 어느 큐에서 막혔는지가 문제 구간을 좁히는
 * 첫 단서가 된다.
 *
 * 실행 컨텍스트: blk-mq 타임아웃 문맥. 잠들 수 없다.
 *
 * 호출 체인:
 *   blk-mq 타임아웃 → [이 함수] → __nvme_fc_abort_op → LLDD 의 fcp_abort
 */
static enum blk_eh_timer_return nvme_fc_timeout(struct request *rq)
{
	struct nvme_fc_fcp_op *op = blk_mq_rq_to_pdu(rq);
	struct nvme_fc_ctrl *ctrl = op->ctrl;
	u16 qnum = op->queue->qnum;
	struct nvme_fc_cmd_iu *cmdiu = &op->cmd_iu;
	struct nvme_command *sqe = &cmdiu->sqe;	/* [한국어] 로그에 어떤 명령이었는지 남기기 위해 꺼낸다 */

	/*
	 * Attempt to abort the offending command. Command completion
	 * will detect the aborted io and will fail the connection.
	 */
	/* [한국어] 위 영어 주석대로 실제 실패 처리는 abort 완료가 한다.
	 * 여기서는 요청만 걸어 두고 돌아간다. */
	dev_info(ctrl->ctrl.device,	/* [한국어] 큐 번호·opcode·cdw10/11 까지 남긴다 — 여러 단을 거치는 FC 에서 문제 구간을 좁히는 단서다 */
		"NVME-FC{%d.%d}: io timeout: opcode %d fctype %d (%s) w10/11: "
		"x%08x/x%08x\n",
		ctrl->cnum, qnum, sqe->common.opcode, sqe->fabrics.fctype,
		nvme_fabrics_opcode_str(qnum, sqe),
		sqe->common.cdw10, sqe->common.cdw11);
	if (__nvme_fc_abort_op(ctrl, op))	/* [한국어] 개별 취소가 불가능했다면 */
		nvme_fc_error_recovery(ctrl, "io timeout abort failed");	/* [한국어] 세션 수준 복구로 올린다 */

	/*
	 * the io abort has been initiated. Have the reset timer
	 * restarted and the abort completion will complete the io
	 * shortly. Avoids a synchronous wait while the abort finishes.
	 */
	/* [한국어] 위 영어 주석대로 여기서 기다리면 blk-mq 타임아웃 문맥이 묶인다.
	 * 시계만 다시 걸고 물러나면 abort 완료가 곧 요청을 끝낸다. */
	return BLK_EH_RESET_TIMER;
}

static int
nvme_fc_map_data(struct nvme_fc_ctrl *ctrl, struct request *rq,
		struct nvme_fc_fcp_op *op)
{
	struct nvmefc_fcp_req *freq = &op->fcp_req;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	freq->sg_cnt = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!blk_rq_nr_phys_segments(rq))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	freq->sg_table.sgl = freq->first_sgl;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ret = sg_alloc_table_chained(&freq->sg_table,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			blk_rq_nr_phys_segments(rq), freq->sg_table.sgl,
			NVME_INLINE_SG_CNT);
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return -ENOMEM;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	op->nents = blk_rq_map_sg(rq, freq->sg_table.sgl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	WARN_ON(op->nents > blk_rq_nr_phys_segments(rq));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	freq->sg_cnt = fc_dma_map_sg(ctrl->lport->dev, freq->sg_table.sgl,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				op->nents, rq_dma_dir(rq));
	if (unlikely(freq->sg_cnt <= 0)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		sg_free_table_chained(&freq->sg_table, NVME_INLINE_SG_CNT);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		freq->sg_cnt = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return -EFAULT;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}

	/*
	 * TODO: blk_integrity_rq(rq)  for DIF
	 */
	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static void
nvme_fc_unmap_data(struct nvme_fc_ctrl *ctrl, struct request *rq,
		struct nvme_fc_fcp_op *op)
{
	struct nvmefc_fcp_req *freq = &op->fcp_req;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	if (!freq->sg_cnt)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	fc_dma_unmap_sg(ctrl->lport->dev, freq->sg_table.sgl, op->nents,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			rq_dma_dir(rq));

	sg_free_table_chained(&freq->sg_table, NVME_INLINE_SG_CNT);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	freq->sg_cnt = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}

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
/*
 * [한국어]
 * nvme_fc_start_fcp_op - 명령 IU 를 채우고 LLDD 에 넘겨 실제 전송을 시작한다
 *
 * @ctrl:     대상 컨트롤러
 * @queue:    이 명령이 나갈 큐. connection_id 와 csn 을 여기서 얻는다.
 * @op:       태그에 붙어 있는 FC 오퍼레이션
 * @data_len: 옮길 데이터 바이트 수. 0 이면 데이터 프레임이 없다.
 * @io_dir:   전송 방향. FC 는 방향마다 프레임 흐름이 달라 명시해야 한다.
 * @return:   BLK_STS_OK 면 LLDD 가 접수. RESOURCE 면 재시도 가능,
 *            IOERR 이면 이 요청은 실패다.
 *
 * NVMe SQE 를 FC-NVME 명령 IU 로 감싸는 자리다. 눈여겨볼 것은 SGL 서술자를
 * 채우는 방식이다. 위 영어 주석이 규칙을 적어 두었듯 type 은 Transport SGL
 * Data Block, subtype 은 전송 계층 고유값, 주소는 0 이고 길이만 유효하다.
 * 주소가 0 인 이유는 FC 가 데이터를 별도 프레임으로 옮기기 때문이다 --
 * RDMA 처럼 원격에 메모리를 노출하지도, TCP 처럼 캡슐에 싣지도 않으므로
 * 명령 안에 가리킬 주소 자체가 없다. 길이만 알려 주고 나머지는 FC 프로토콜이 한다.
 *
 * csn 을 atomic_inc_return 으로 올리는 것과, LLDD 가 실패했을 때 그 번호에
 * 구멍이 생기는 문제에 대한 판단이 아래 영어 주석에 길게 적혀 있다. 요지는
 * 리눅스가 fused 명령을 쓰지 않아 타겟이 csn 순서에 의존하지 않으므로
 * 구멍이 생겨도 무해하다는 것이다.
 *
 * 상태를 ACTIVE 로 옮기는 시점이 중요하다. LLDD 에 넘기기 직전이어야 하고,
 * 넘긴 뒤에는 완료가 언제든 도착할 수 있다. 실패 시 atomic_xchg 로 되돌리는
 * 것도 그 완료와 겹치지 않기 위해서다.
 *
 * 반환값 구분: 포트가 살아 있는데도 실패했고 EBUSY 도 아니면 진짜 오류이므로
 * IOERR 이다. 그 밖에는 RESOURCE 로 돌려 blk-mq 가 재큐잉하게 한다.
 *
 * 실행 컨텍스트: blk-mq 제출 경로(또는 AEN 제출). 잠들 수 없다.
 *
 * 호출 체인:
 *   nvme_fc_queue_rq → [이 함수] → LLDD 의 fcp_io 콜백 → FC 프레임
 */
static blk_status_t
nvme_fc_start_fcp_op(struct nvme_fc_ctrl *ctrl, struct nvme_fc_queue *queue,
	struct nvme_fc_fcp_op *op, u32 data_len,
	enum nvmefc_fcp_datadir	io_dir)
{
	struct nvme_fc_cmd_iu *cmdiu = &op->cmd_iu;	/* [한국어] FC 가 이해하는 명령 봉투 */
	struct nvme_command *sqe = &cmdiu->sqe;	/* [한국어] 그 안에 담기는 NVMe 명령 본문 */
	int ret, opstate;

	/*
	 * before attempting to send the io, check to see if we believe
	 * the target device is present
	 */
	if (ctrl->rport->remoteport.port_state != FC_OBJSTATE_ONLINE)	/* [한국어] 포트가 사라졌으면 실어 보낼 매체가 없다 */
		return BLK_STS_RESOURCE;	/* [한국어] 오류가 아니라 자원 부족으로 돌려 재시도 여지를 남긴다 — 포트가 곧 돌아올 수 있다 */

	if (!nvme_fc_ctrl_get(ctrl))	/* [한국어] 전송 중 컨트롤러가 사라지지 않도록 참조를 든다 */
		return BLK_STS_IOERR;

	/* format the FC-NVME CMD IU and fcp_req */
	cmdiu->connection_id = cpu_to_be64(queue->connection_id);	/* [한국어] 타겟이 어느 큐의 명령인지 아는 유일한 근거. FC 는 빅엔디안이다 */
	cmdiu->data_len = cpu_to_be32(data_len);
	switch (io_dir) {	/* [한국어] FC 는 방향마다 프레임 흐름이 달라 명시해야 한다 */
	case NVMEFC_FCP_WRITE:
		cmdiu->flags = FCNVME_CMD_FLAGS_WRITE;
		break;
	case NVMEFC_FCP_READ:
		cmdiu->flags = FCNVME_CMD_FLAGS_READ;
		break;
	case NVMEFC_FCP_NODATA:
		cmdiu->flags = 0;	/* [한국어] 데이터 프레임 없이 명령과 응답만 오간다 */
		break;
	}
	op->fcp_req.payload_length = data_len;
	op->fcp_req.io_dir = io_dir;
	op->fcp_req.transferred_length = 0;	/* [한국어] 재사용되는 오퍼레이션이라 이전 값을 지운다 — 완료 시 검증 기준이 된다 */
	op->fcp_req.rcv_rsplen = 0;	/* [한국어] 완료 경로가 이 값으로 응답 종류를 가르므로 반드시 초기화해야 한다 */
	op->fcp_req.status = NVME_SC_SUCCESS;
	op->fcp_req.sqid = cpu_to_le16(queue->qnum);

	/*
	 * validate per fabric rules, set fields mandated by fabric spec
	 * as well as those by FC-NVME spec.
	 */
	WARN_ON_ONCE(sqe->common.metadata);	/* [한국어] FC 는 별도 메타데이터 포인터를 쓰지 않는다 — 있으면 상위가 잘못 만든 것이다 */
	sqe->common.flags |= NVME_CMD_SGL_METABUF;	/* [한국어] 데이터를 PRP 가 아니라 SGL 로 기술한다고 알린다. Fabrics 는 늘 SGL 이다 */

	/*
	 * format SQE DPTR field per FC-NVME rules:
	 *    type=0x5     Transport SGL Data Block Descriptor
	 *    subtype=0xA  Transport-specific value
	 *    address=0
	 *    length=length of the data series
	 */
	/* [한국어] 위 영어 주석의 규칙 그대로다. 주소가 0 인 것이 FC 의 특징을
	 * 드러낸다 -- 데이터는 별도 FC 프레임으로 오가므로 명령 안에 가리킬 주소가
	 * 없다. RDMA 는 rkey 를, TCP 는 캡슐 오프셋을 싣지만 FC 는 길이만 알린다. */
	sqe->rw.dptr.sgl.type = (NVME_TRANSPORT_SGL_DATA_DESC << 4) |
					NVME_SGL_FMT_TRANSPORT_A;
	sqe->rw.dptr.sgl.length = cpu_to_le32(data_len);
	sqe->rw.dptr.sgl.addr = 0;

	if (!(op->flags & FCOP_FLAGS_AEN)) {	/* [한국어] AEN 은 blk-mq 요청이 없어 매핑할 데이터도 없다 */
		ret = nvme_fc_map_data(ctrl, op->rq, op);	/* [한국어] scatterlist 를 만들고 DMA 매핑 */
		if (ret < 0) {
			nvme_cleanup_cmd(op->rq);
			nvme_fc_ctrl_put(ctrl);
			if (ret == -ENOMEM || ret == -EAGAIN)
				return BLK_STS_RESOURCE;	/* [한국어] 일시적 자원 부족 — 재시도하면 될 수 있다 */
			return BLK_STS_IOERR;
		}
	}

	fc_dma_sync_single_for_device(ctrl->lport->dev, op->fcp_req.cmddma,	/* [한국어] CPU 가 채운 명령 IU 를 HBA 가 읽기 전에 캐시를 맞춘다 */
				  sizeof(op->cmd_iu), DMA_TO_DEVICE);

	atomic_set(&op->state, FCPOP_STATE_ACTIVE);	/* [한국어] 넘기기 직전에 ACTIVE 로. 이후로는 완료가 언제든 도착할 수 있다 */

	if (!(op->flags & FCOP_FLAGS_AEN))
		nvme_start_request(op->rq);	/* [한국어] 타임아웃 시계를 시작한다 */

	cmdiu->csn = cpu_to_be32(atomic_inc_return(&queue->csn));	/* [한국어] 명령 순서 번호. 여러 CPU 가 동시에 제출해도 겹치지 않도록 원자적으로 올린다 */
	ret = ctrl->lport->ops->fcp_io(&ctrl->lport->localport,	/* [한국어] 여기서부터 프레임 전송은 HBA 의 몫이다 */
					&ctrl->rport->remoteport,
					queue->lldd_handle, &op->fcp_req);

	if (ret) {
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
		/* [한국어] 위 영어 주석의 결론: csn 을 이미 올린 뒤 전송이 실패하면
		 * 번호에 구멍이 생기지만, 리눅스는 fused 명령을 쓰지 않아 타겟이 csn
		 * 순서에 의존하지 않으므로 무해하다. 그래서 되돌리지 않는다. */
		opstate = atomic_xchg(&op->state, FCPOP_STATE_COMPLETE);	/* [한국어] 완료 콜백과 겹치지 않도록 원자적으로 상태를 되돌린다 */
		__nvme_fc_fcpop_chk_teardowns(ctrl, op, opstate);	/* [한국어] 해체를 기다리는 쪽이 있으면 iocnt 를 내려 준다 */

		if (!(op->flags & FCOP_FLAGS_AEN)) {
			nvme_fc_unmap_data(ctrl, op->rq, op);	/* [한국어] 위에서 잡은 DMA 매핑을 되돌린다 */
			nvme_cleanup_cmd(op->rq);
		}

		nvme_fc_ctrl_put(ctrl);

		if (ctrl->rport->remoteport.port_state == FC_OBJSTATE_ONLINE &&	/* [한국어] 포트는 멀쩡한데 실패했고 */
				ret != -EBUSY)	/* [한국어] 일시적 혼잡도 아니라면 */
			return BLK_STS_IOERR;	/* [한국어] 진짜 오류다 */

		return BLK_STS_RESOURCE;	/* [한국어] 그 밖에는 재시도 여지를 남긴다 */
	}

	return BLK_STS_OK;
}

/*
 * [한국어]
 * nvme_fc_queue_rq - blk-mq 가 요청 하나를 FC 트랜스포트에 넘기는 진입점
 *
 * @hctx: 배정된 하드웨어 큐 문맥. driver_data 에 nvme_fc_queue 가 들어 있다.
 * @bd:   요청과 배치 힌트
 * @return: BLK_STS_OK 면 LLDD 에 넘겼다는 뜻. 그 밖은 blk-mq 가 처리한다.
 *
 * 다른 트랜스포트와 다른 첫 줄은 port_state 검사다. FC 는 케이블·스위치·
 * 존 구성 때문에 원격 포트가 수시로 사라졌다 나타난다. 큐가 LIVE 여도 포트가
 * 오프라인이면 프레임을 실어 보낼 매체 자체가 없으므로, 큐 상태와 별개로
 * 포트 상태를 먼저 본다.
 *
 * 데이터 방향 판정에서 blk_rq_nr_phys_segments 를 쓰는 이유는 위 영어 주석이
 * 밝힌다 -- WRITE ZEROES 처럼 payload_bytes 는 0 이 아니면서 실제로 옮길
 * 데이터는 없는 명령이 있다. 그 경우까지 WRITE 로 보내면 LLDD 가 있지도 않은
 * 버퍼를 읽으려 한다. 물리 세그먼트 유무가 "진짜 옮길 것이 있는가"의 답이다.
 *
 * 실행 컨텍스트: blk-mq 제출 경로. 잠들 수 없다.
 *
 * 호출 체인:
 *   submit_bio → blk-mq → [이 함수] → nvme_setup_cmd
 *     → nvme_fc_start_fcp_op → LLDD 의 fcp_io 콜백 → FC 프레임
 */
static blk_status_t
nvme_fc_queue_rq(struct blk_mq_hw_ctx *hctx,
			const struct blk_mq_queue_data *bd)
{
	struct nvme_ns *ns = hctx->queue->queuedata;	/* [한국어] 대상 네임스페이스. admin 명령이면 NULL */
	struct nvme_fc_queue *queue = hctx->driver_data;
	struct nvme_fc_ctrl *ctrl = queue->ctrl;
	struct request *rq = bd->rq;
	struct nvme_fc_fcp_op *op = blk_mq_rq_to_pdu(rq);	/* [한국어] 태그에 미리 붙어 있는 FC 오퍼레이션 */
	enum nvmefc_fcp_datadir	io_dir;
	bool queue_ready = test_bit(NVME_FC_Q_LIVE, &queue->flags);
	u32 data_len;
	blk_status_t ret;

	if (ctrl->rport->remoteport.port_state != FC_OBJSTATE_ONLINE ||	/* [한국어] 원격 포트가 사라졌으면 실어 보낼 매체가 없다 — FC 특유의 선행 검사다 */
	    !nvme_check_ready(&queue->ctrl->ctrl, rq, queue_ready))	/* [한국어] 그리고 코어가 보는 컨트롤러·큐 준비 상태 */
		return nvme_fail_nonready_command(&queue->ctrl->ctrl, rq);	/* [한국어] 재시도할지 페일오버할지는 코어 정책에 맡긴다 */

	ret = nvme_setup_cmd(ns, rq);	/* [한국어] 블록 요청을 NVMe 명령으로 번역 — 이 부분은 트랜스포트와 무관하다 */
	if (ret)
		return ret;

	/*
	 * nvme core doesn't quite treat the rq opaquely. Commands such
	 * as WRITE ZEROES will return a non-zero rq payload_bytes yet
	 * there is no actual payload to be transferred.
	 * To get it right, key data transmission on there being 1 or
	 * more physical segments in the sg list. If there are no
	 * physical segments, there is no payload.
	 */
	/* [한국어] 위 영어 주석대로 payload_bytes 만 믿으면 WRITE ZEROES 에서
	 * 있지도 않은 버퍼를 LLDD 가 읽으려 한다. 물리 세그먼트 유무가 진짜 판정 기준이다. */
	if (blk_rq_nr_phys_segments(rq)) {	/* [한국어] 옮길 데이터가 실제로 있다 */
		data_len = blk_rq_payload_bytes(rq);
		io_dir = ((rq_data_dir(rq) == WRITE) ?	/* [한국어] FC 는 방향을 명시해야 한다 — 프레임 흐름이 방향마다 다르다 */
					NVMEFC_FCP_WRITE : NVMEFC_FCP_READ);
	} else {
		data_len = 0;
		io_dir = NVMEFC_FCP_NODATA;	/* [한국어] 데이터 프레임 없이 명령과 응답만 오간다 */
	}


	return nvme_fc_start_fcp_op(ctrl, queue, op, data_len, io_dir);	/* [한국어] IU 를 채워 LLDD 에 넘긴다. 여기서부터는 HBA 의 몫이다 */
}

/*
 * [한국어]
 * nvme_fc_submit_async_event - 비동기 이벤트 명령을 컨트롤러에 걸어 둔다
 *
 * @arg: 코어가 넘긴 nvme_ctrl
 * @return: 없음
 *
 * AEN 은 완료를 기다리는 명령이 아니라 "알릴 것이 생기면 알려 달라"고
 * 걸어 두는 명령이다. 그래서 완료가 오면 그 내용을 처리한 뒤 곧바로 다시
 * 건다 -- 이 함수가 그 재무장을 담당한다.
 *
 * TERMIO 검사가 먼저 오는 이유: 세션을 내리는 중이라면 새로 걸어 봐야
 * 곧 취소된다. 더 나쁘게는, delete_association 이 iocnt 가 0 이 되기를
 * 기다리는 중에 새 AEN 이 하나 더 늘어나 그 대기가 끝나지 않을 수 있다.
 *
 * aen_ops[0] 만 쓰는 것은 코어가 한 번에 하나의 AEN 만 요청하기 때문이다.
 * 배열이 여럿인 것은 컨트롤러가 지원하는 최대치에 맞춰 미리 잡아 둔 것이다.
 *
 * 실패해도 되돌릴 것이 없다. 로그만 남기며, 이벤트를 못 받게 될 뿐 다른
 * 동작에는 영향이 없다.
 *
 * 실행 컨텍스트: 코어의 AEN 재무장 경로.
 *
 * 호출 체인:
 *   nvme_complete_async_event → ops->submit_async_event → [이 함수]
 *     → nvme_fc_start_fcp_op
 */
static void
nvme_fc_submit_async_event(struct nvme_ctrl *arg)
{
	struct nvme_fc_ctrl *ctrl = to_fc_ctrl(arg);
	struct nvme_fc_fcp_op *aen_op;
	blk_status_t ret;

	if (test_bit(FCCTRL_TERMIO, &ctrl->flags))	/* [한국어] 해체 중에 새로 걸면 delete_association 의 iocnt 대기가 끝나지 않을 수 있다 */
		return;

	aen_op = &ctrl->aen_ops[0];	/* [한국어] 코어는 한 번에 하나만 요청한다 */

	ret = nvme_fc_start_fcp_op(ctrl, aen_op->queue, aen_op, 0,
					NVMEFC_FCP_NODATA);	/* [한국어] 데이터가 없는 명령이다 — 통지만 기다린다 */
	if (ret)
		dev_err(ctrl->ctrl.device,	/* [한국어] 되돌릴 것은 없다. 이벤트를 못 받을 뿐 다른 동작에는 영향이 없다 */
			"failed async event work\n");
}

/*
 * [한국어]
 * nvme_fc_complete_rq - 요청 하나의 트랜스포트 측 정리를 마치고 코어에 넘긴다
 *
 * @rq: 완료된 요청
 * @return: 없음
 *
 * blk-mq 의 complete 콜백이다. fcpio_done 이 상태 코드를 정한 뒤, 코어가
 * 재시도나 페일오버로 가로채지 않았을 때 여기로 온다.
 *
 * 상태를 IDLE 로 되돌리고 TERMIO 를 지우는 것이 중요하다. 이 오퍼레이션은
 * 태그와 함께 재사용되므로, 이전 요청의 흔적이 남으면 다음 요청에서
 * abort 판정이나 해체 회계가 어긋난다.
 *
 * 컨트롤러 참조를 마지막에 놓는다. start_fcp_op 이 들었던 그 참조이며,
 * 이것으로 "이 요청이 컨트롤러를 붙들고 있는 구간"이 닫힌다.
 *
 * 실행 컨텍스트: blk-mq 완료 경로.
 *
 * 호출 체인:
 *   nvme_fc_fcpio_done → nvme_try_complete_req → [이 함수] → nvme_complete_rq
 */
static void
nvme_fc_complete_rq(struct request *rq)
{
	struct nvme_fc_fcp_op *op = blk_mq_rq_to_pdu(rq);
	struct nvme_fc_ctrl *ctrl = op->ctrl;

	atomic_set(&op->state, FCPOP_STATE_IDLE);	/* [한국어] 태그와 함께 재사용되므로 다음 요청을 위해 되돌린다 */
	op->flags &= ~FCOP_FLAGS_TERMIO;	/* [한국어] 남겨 두면 다음 요청의 해체 회계가 어긋난다 */

	nvme_fc_unmap_data(ctrl, rq, op);	/* [한국어] DMA 매핑 해제 */
	nvme_complete_rq(rq);	/* [한국어] 코어가 통계와 상위 완료를 처리한다 */
	nvme_fc_ctrl_put(ctrl);	/* [한국어] start_fcp_op 이 들었던 참조를 놓아 이 요청의 구간을 닫는다 */
}

/*
 * [한국어]
 * nvme_fc_map_queues - CPU 와 하드웨어 큐의 대응을 정한다
 *
 * @set: 대응을 채울 blk-mq 태그셋
 * @return: 없음
 *
 * blk-mq 는 각 CPU 의 요청을 어느 하드웨어 큐로 보낼지 알아야 한다. 그
 * 대응이 성능을 좌우한다 -- 제출한 CPU 와 완료를 처리하는 CPU 가 같으면
 * 캐시가 이어지고 IPI 가 줄어든다.
 *
 * FC 가 특별한 점은 그 대응을 LLDD 가 더 잘 안다는 것이다. HBA 는 자기
 * 인터럽트가 어느 CPU 로 가는지 알고 있으므로, map_queues 콜백을 제공하면
 * 그쪽에 맡긴다. 없으면 blk-mq 의 범용 균등 분배로 돌아간다.
 *
 * nr_queues 가 0 인 맵을 건너뛰되 DEFAULT 만은 WARN 하는 이유: 읽기 전용이나
 * 폴링 큐는 없을 수 있지만 기본 큐가 없으면 I/O 를 보낼 곳 자체가 없다.
 *
 * 실행 컨텍스트: 태그셋 초기화. 잠들 수 있다.
 *
 * 호출 체인:
 *   blk_mq_alloc_tag_set → ops->map_queues → [이 함수] → LLDD 또는 blk_mq_map_queues
 */
static void nvme_fc_map_queues(struct blk_mq_tag_set *set)
{
	struct nvme_fc_ctrl *ctrl = to_fc_ctrl(set->driver_data);
	int i;

	for (i = 0; i < set->nr_maps; i++) {	/* [한국어] 기본·읽기·폴링 맵을 차례로 */
		struct blk_mq_queue_map *map = &set->map[i];

		if (!map->nr_queues) {	/* [한국어] 이 용도의 큐가 없다 */
			WARN_ON(i == HCTX_TYPE_DEFAULT);	/* [한국어] 읽기·폴링은 없어도 되지만 기본 큐가 없으면 I/O 를 보낼 곳이 없다 */
			continue;
		}

		/* Call LLDD map queue functionality if defined */
		if (ctrl->lport->ops->map_queues)	/* [한국어] HBA 는 자기 인터럽트가 어느 CPU 로 가는지 안다 */
			ctrl->lport->ops->map_queues(&ctrl->lport->localport,	/* [한국어] 그래서 대응을 그쪽에 맡기는 편이 낫다 */
						     map);
		else
			blk_mq_map_queues(map);	/* [한국어] 콜백이 없으면 blk-mq 의 범용 균등 분배 */
	}
}

static const struct blk_mq_ops nvme_fc_mq_ops = {	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	.queue_rq	= nvme_fc_queue_rq,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	.complete	= nvme_fc_complete_rq,
	.init_request	= nvme_fc_init_request,
	.exit_request	= nvme_fc_exit_request,
	.init_hctx	= nvme_fc_init_hctx,
	.timeout	= nvme_fc_timeout,
	.map_queues	= nvme_fc_map_queues,
};

/*
 * [한국어]
 * nvme_fc_create_io_queues - 첫 연결에서 I/O 큐 일체를 만든다
 *
 * @ctrl: admin 큐까지 세워진 컨트롤러
 * @return: 0 이면 I/O 큐가 LIVE 까지 도달. 음수면 실패이며 되감았다.
 *
 * 큐 개수가 세 값의 최솟값으로 정해지는 것이 이 함수의 첫 결정이다:
 *   - 사용자가 요청한 개수(nr_io_queues 옵션)
 *   - 온라인 CPU 수 -- 그보다 많이 만들어도 동시에 쓸 주체가 없다
 *   - HBA 가 지원하는 하드웨어 큐 수 -- 물리적 상한
 * 그렇게 고른 값을 다시 Set Features 로 컨트롤러와 협상한다. 타겟이 더 적게
 * 줄 수 있으므로 최종 개수는 그 응답이 정한다.
 *
 * 이후 순서는 태그셋 → FC 채널 → NVMe Connect 다. 태그셋을 먼저 만드는 이유는
 * 요청 구조체가 태그마다 미리 할당돼야 하고, 그 크기가 LLDD 의 사적 영역
 * 크기(fcprqst_priv_sz)에 따라 달라지기 때문이다. struct_size_t 로 그 가변
 * 길이를 계산한다.
 *
 * 실패 경로에서 tagset 을 NULL 로 되돌리는 것이 중요하다. 위 영어 주석대로
 * 해제 루틴이 이 값을 보고 "I/O 큐가 있었는가"를 판단하므로, 남겨 두면
 * 이미 정리한 것을 다시 정리하려 한다.
 *
 * 실행 컨텍스트: create_association 경로. 잠들 수 있다.
 *
 * 호출 체인:
 *   nvme_fc_create_association → [이 함수] → nvme_alloc_io_tag_set
 *     → nvme_fc_create_hw_io_queues → nvme_fc_connect_io_queues
 */
static int
nvme_fc_create_io_queues(struct nvme_fc_ctrl *ctrl)
{
	struct nvmf_ctrl_options *opts = ctrl->ctrl.opts;
	unsigned int nr_io_queues;
	int ret;

	nr_io_queues = min3(opts->nr_io_queues, num_online_cpus(),	/* [한국어] CPU 보다 많이 만들어도 동시에 쓸 주체가 없다 */
				ctrl->lport->ops->max_hw_queues);	/* [한국어] HBA 의 물리적 상한 */
	ret = nvme_set_queue_count(&ctrl->ctrl, &nr_io_queues);	/* [한국어] Set Features 로 협상 — 타겟이 더 적게 줄 수 있다 */
	if (ret) {
		dev_info(ctrl->ctrl.device,
			"set_queue_count failed: %d\n", ret);
		return ret;
	}

	ctrl->ctrl.queue_count = nr_io_queues + 1;	/* [한국어] admin 큐 하나를 더한 것이 전체 큐 수다 */
	if (!nr_io_queues)
		return 0;	/* [한국어] I/O 큐 없이 admin 만으로도 컨트롤러는 성립한다 — 관리 전용 연결 */

	nvme_fc_init_io_queues(ctrl);	/* [한국어] 큐 구조체들의 기본 필드를 세운다 */

	ret = nvme_alloc_io_tag_set(&ctrl->ctrl, &ctrl->tag_set,	/* [한국어] 태그마다 요청 구조체를 미리 할당한다 */
			&nvme_fc_mq_ops, 1,
			struct_size_t(struct nvme_fcp_op_w_sgl, priv,	/* [한국어] 요청 구조체 크기가 가변이다 — LLDD 가 요구하는 사적 영역이 뒤에 붙는다 */
				      ctrl->lport->ops->fcprqst_priv_sz));
	if (ret)
		return ret;

	ret = nvme_fc_create_hw_io_queues(ctrl, ctrl->ctrl.sqsize + 1);	/* [한국어] LLDD 에 FC 채널을 열어 달라고 한다 */
	if (ret)
		goto out_cleanup_tagset;

	ret = nvme_fc_connect_io_queues(ctrl, ctrl->ctrl.sqsize + 1);	/* [한국어] 그 위에 Create Connection LS + Fabrics Connect */
	if (ret)
		goto out_delete_hw_queues;

	ctrl->ioq_live = true;	/* [한국어] 재연결 시 이 값으로 create 와 recreate 를 가른다 */

	return 0;

out_delete_hw_queues:
	nvme_fc_delete_hw_io_queues(ctrl);
out_cleanup_tagset:
	nvme_remove_io_tag_set(&ctrl->ctrl);
	nvme_fc_free_io_queues(ctrl);

	/* force put free routine to ignore io queues */
	ctrl->ctrl.tagset = NULL;	/* [한국어] 위 영어 주석대로 해제 루틴이 이 값으로 "I/O 큐가 있었는가"를 판단한다. 남겨 두면 이미 정리한 것을 또 정리한다 */

	return ret;
}

/*
 * [한국어]
 * nvme_fc_recreate_io_queues - 재연결에서 기존 태그셋 위에 채널만 다시 연다
 *
 * @ctrl: 이전에 I/O 큐를 가졌던 컨트롤러
 * @return: 0 이면 성공. -ENOSPC 는 이전에 있던 I/O 큐가 하나도 확보되지 않은 경우.
 *
 * create_io_queues 와 나뉘어 있는 이유는 태그셋 때문이다. 재연결에서는
 * 태그셋을 다시 만들지 않는다 -- 그 위에 이미 블록 계층의 디스크와 진행 중인
 * 요청이 매달려 있어서, 새로 만들면 그 연결이 끊긴다. 그래서 FC 채널과
 * NVMe Connect 만 다시 세우고 태그셋은 재사용한다.
 *
 * 그 대신 큐 개수가 달라질 수 있다는 문제가 생긴다. 재연결 시 타겟이 이전과
 * 다른 개수를 줄 수 있는데, 태그셋은 하드웨어 큐 개수를 알고 있으므로
 * blk_mq_update_nr_hw_queues 로 알려 줘야 한다. 이것이 두 함수의 실질적 차이다.
 *
 * -ENOSPC 조건이 눈여겨볼 만하다. 이전에 I/O 큐가 있었는데 이번에 하나도
 * 못 얻었다면 재연결을 실패로 본다. admin 만으로 되살리면 디스크가 있는데
 * I/O 를 보낼 수 없는 상태가 되어, 차라리 실패시키고 다시 시도하는 편이 낫다.
 *
 * 실행 컨텍스트: create_association 경로(재연결). 잠들 수 있다.
 *
 * 호출 체인:
 *   nvme_fc_create_association → [이 함수] → blk_mq_update_nr_hw_queues
 *     → nvme_fc_create_hw_io_queues → nvme_fc_connect_io_queues
 */
static int
nvme_fc_recreate_io_queues(struct nvme_fc_ctrl *ctrl)
{
	struct nvmf_ctrl_options *opts = ctrl->ctrl.opts;
	u32 prior_ioq_cnt = ctrl->ctrl.queue_count - 1;	/* [한국어] 이전 연결에서의 I/O 큐 수. 아래 비교의 기준이다 */
	unsigned int nr_io_queues;
	int ret;

	nr_io_queues = min3(opts->nr_io_queues, num_online_cpus(),	/* [한국어] 상한 계산은 첫 연결과 같다 */
				ctrl->lport->ops->max_hw_queues);
	ret = nvme_set_queue_count(&ctrl->ctrl, &nr_io_queues);	/* [한국어] 타겟이 이번에는 다른 개수를 줄 수 있다 */
	if (ret) {
		dev_info(ctrl->ctrl.device,
			"set_queue_count failed: %d\n", ret);
		return ret;
	}

	if (!nr_io_queues && prior_ioq_cnt) {	/* [한국어] 전에는 있었는데 이번엔 하나도 못 얻었다 */
		dev_info(ctrl->ctrl.device,	/* [한국어] 디스크는 있는데 I/O 를 보낼 수 없는 상태가 되므로 */
			"Fail Reconnect: At least 1 io queue "
			"required (was %d)\n", prior_ioq_cnt);
		return -ENOSPC;	/* [한국어] 차라리 실패시키고 다시 시도하게 한다 */
	}

	ctrl->ctrl.queue_count = nr_io_queues + 1;
	/* check for io queues existing */
	if (ctrl->ctrl.queue_count == 1)
		return 0;	/* [한국어] 원래도 admin 만 있던 구성이면 할 일이 없다 */

	if (prior_ioq_cnt != nr_io_queues) {	/* [한국어] 개수가 달라졌다 */
		dev_info(ctrl->ctrl.device,
			"reconnect: revising io queue count from %d to %d\n",
			prior_ioq_cnt, nr_io_queues);
		blk_mq_update_nr_hw_queues(&ctrl->tag_set, nr_io_queues);	/* [한국어] 태그셋을 재사용하므로 하드웨어 큐 수 변경을 블록 계층에 알려야 한다. 이것이 create 와의 실질적 차이다 */
	}

	ret = nvme_fc_create_hw_io_queues(ctrl, ctrl->ctrl.sqsize + 1);	/* [한국어] FC 채널만 다시 연다 — 태그셋은 그대로 둔다 */
	if (ret)
		goto out_free_io_queues;

	ret = nvme_fc_connect_io_queues(ctrl, ctrl->ctrl.sqsize + 1);
	if (ret)
		goto out_delete_hw_queues;

	return 0;

out_delete_hw_queues:
	nvme_fc_delete_hw_io_queues(ctrl);
out_free_io_queues:
	nvme_fc_free_io_queues(ctrl);	/* [한국어] 태그셋은 건드리지 않는다 — 디스크와 진행 중 요청이 그 위에 매달려 있다 */
	return ret;
}

static void
nvme_fc_rport_active_on_lport(struct nvme_fc_rport *rport)
{
	struct nvme_fc_lport *lport = rport->lport;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	atomic_inc(&lport->act_rport_cnt);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}

/*
 * [한국어]
 * nvme_fc_rport_inactive_on_lport - 원격 포트 하나가 비활성이 됐음을 상위에 알린다
 *
 * @rport: 비활성이 된 원격 포트
 * @return: 없음
 *
 * 활성 개수를 세는 두 단계 사슬의 위쪽 절반이다. 컨트롤러가 사라지면
 * 원격 포트가 비활성이 되고, 원격 포트가 모두 비활성이 되면 로컬 포트도
 * 그렇다. 그 사슬이 끝까지 내려가야 LLDD 가 자원을 정리할 수 있다.
 *
 * 0 이 됐고 이미 삭제 표시가 있을 때만 LLDD 를 부르는 이유: 아직 살아
 * 있는 포트라면 다시 활성이 될 수 있으므로 정리하면 안 된다. 두 조건이
 * 함께여야 "더 쓸 일이 없다"가 성립한다.
 *
 * 실행 컨텍스트: 컨트롤러 해체 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   nvme_fc_ctlr_inactive_on_rport → [이 함수] → LLDD 의 localport_delete
 */
static void
nvme_fc_rport_inactive_on_lport(struct nvme_fc_rport *rport)
{
	struct nvme_fc_lport *lport = rport->lport;
	u32 cnt;

	cnt = atomic_dec_return(&lport->act_rport_cnt);	/* [한국어] 감소와 결과 읽기를 원자적으로 — 두 포트가 동시에 내려도 하나만 0 을 본다 */
	if (cnt == 0 && lport->localport.port_state == FC_OBJSTATE_DELETED)	/* [한국어] 아직 살아 있는 포트면 다시 활성이 될 수 있어 정리하면 안 된다 */
		lport->ops->localport_delete(&lport->localport);	/* [한국어] 두 조건이 함께여야 "더 쓸 일이 없다"가 성립한다 */
}

/*
 * [한국어]
 * nvme_fc_ctlr_active_on_rport - 이 컨트롤러를 활성으로 표시한다
 *
 * @ctrl: 활성화할 컨트롤러
 * @return: 0 이면 이번에 활성이 됐다. 1 이면 이미 활성이었다.
 *
 * ASSOC_ACTIVE 비트를 test_and_set 으로 다루는 것이 중복 방지의 전부다.
 * 이미 서 있었다면 1 을 돌려주고 계수를 올리지 않는다 -- 그러지 않으면
 * 재연결 때마다 활성 개수가 늘어나 영영 0 이 되지 않는다.
 *
 * 0 → 1 로 처음 올라갈 때만 상위(로컬 포트)에 알린다. 사슬이 한 단계씩
 * 올라가며 각 단계의 첫 활성만 전파되는 구조다.
 *
 * 실행 컨텍스트: 세션 수립 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   nvme_fc_create_association → [이 함수] → nvme_fc_rport_active_on_lport
 */
static int
nvme_fc_ctlr_active_on_rport(struct nvme_fc_ctrl *ctrl)
{
	struct nvme_fc_rport *rport = ctrl->rport;
	u32 cnt;

	if (test_and_set_bit(ASSOC_ACTIVE, &ctrl->flags))	/* [한국어] 이미 활성이면 계수를 올리지 않는다 — 재연결마다 늘면 영영 0 이 되지 않는다 */
		return 1;

	cnt = atomic_inc_return(&rport->act_ctrl_cnt);
	if (cnt == 1)	/* [한국어] 이 포트에서 처음 활성이 된 컨트롤러일 때만 */
		nvme_fc_rport_active_on_lport(rport);	/* [한국어] 사슬을 한 단계 위로 전파한다 */

	return 0;
}

/*
 * [한국어]
 * nvme_fc_ctlr_inactive_on_rport - 이 컨트롤러를 비활성으로 내리고 사슬을 전파한다
 *
 * @ctrl: 비활성화할 컨트롤러
 * @return: 항상 0
 *
 * active_on_rport 의 짝이다. 활성 개수가 0 이 되면 원격 포트도 비활성이
 * 되고, 그것이 다시 로컬 포트로 전파된다.
 *
 * ASSOC_ACTIVE 비트를 여기서 지우지 않는 것에 주의할 것. 위 영어 주석이
 * 밝히듯 그것은 association 삭제 경로가 담당한다. 두 곳에서 지우면
 * test_and_set 의 중복 방지가 깨진다.
 *
 * 실행 컨텍스트: 세션 해체 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   nvme_fc_delete_association → [이 함수] → nvme_fc_rport_inactive_on_lport
 */
static int
nvme_fc_ctlr_inactive_on_rport(struct nvme_fc_ctrl *ctrl)
{
	struct nvme_fc_rport *rport = ctrl->rport;
	struct nvme_fc_lport *lport = rport->lport;
	u32 cnt;

	/* clearing of ctrl->flags ASSOC_ACTIVE bit is in association delete */
	/* [한국어] 위 영어 주석대로 비트는 여기서 지우지 않는다. 두 곳에서 지우면
	 * test_and_set 이 제공하는 중복 방지가 깨진다. */

	cnt = atomic_dec_return(&rport->act_ctrl_cnt);
	if (cnt == 0) {	/* [한국어] 이 포트를 쓰던 마지막 컨트롤러였다 */
		if (rport->remoteport.port_state == FC_OBJSTATE_DELETED)	/* [한국어] 포트가 이미 삭제 표시라면 LLDD 가 정리해도 된다 */
			lport->ops->remoteport_delete(&rport->remoteport);
		nvme_fc_rport_inactive_on_lport(rport);	/* [한국어] 사슬을 한 단계 위로 전파한다 */
	}

	return 0;
}

/*
 * This routine restarts the controller on the host side, and
 * on the link side, recreates the controller association.
 */
/*
 * [한국어]
 * nvme_fc_create_association - FC 세션을 처음부터 끝까지 세운다
 *
 * @ctrl: 세션을 세울 컨트롤러
 * @return: 0 이면 LIVE 까지 도달. 음수면 실패이며 잡은 것은 역순으로 되돌렸다.
 *
 * 이 파일에서 가장 긴 절차이고, FC 가 다른 트랜스포트와 다른 지점이 모두
 * 여기 모여 있다. 순서는 이렇다:
 *
 *   포트 상태 확인 → admin 하드웨어 큐 생성 → Create Connection LS
 *     → Fabrics Connect → CC.EN → Identify → 능력 검증 → AEN 준비
 *       → I/O 큐 생성 → LIVE
 *
 * 두 층이 겹쳐 있다는 점이 핵심이다. FC 쪽 채널(하드웨어 큐 + Connection LS)을
 * 먼저 열고, 그 위에 NVMe 쪽 큐(Fabrics Connect)를 세운다. 그래서 실패 시
 * 되감기도 두 층을 각각 처리해야 하고, 아래 goto 사다리가 그만큼 길다.
 *
 * ASSOC_FAILED 를 여러 단계마다 확인하는 이유: 이 절차가 진행되는 도중에도
 * 완료 콜백이나 LS 수신이 세션 실패를 알릴 수 있다. 그 통지는 비동기로
 * 오므로, 각 단계가 성공했더라도 그 사이에 세션이 죽었을 수 있다.
 * 매번 확인하지 않으면 이미 끊어진 세션 위에 다음 단계를 쌓게 된다.
 *
 * ctrl->ctrl.max_hw_sectors 가 LLDD 의 max_sgl_segments 에서 나오는 것에
 * 주목할 것 -- HBA 가 한 번에 다룰 수 있는 SG 항목 수가 그대로 블록 계층의
 * 요청 크기 상한이 된다. 하드웨어 한도가 bio 분할 지점을 정하는 셈이다.
 *
 * icdoff 를 거부하는 이유: FC-NVME 는 캡슐에 데이터를 함께 싣지 않는다.
 * 데이터는 별도의 FC 데이터 프레임으로 오간다. 그래서 컨트롤러가 캡슐 내
 * 데이터 오프셋을 요구하면 이 트랜스포트로는 성립하지 않는다.
 *
 * 실행 컨텍스트: connect_work 또는 reset_work. 잠들 수 있다.
 *
 * 호출 체인:
 *   nvme_fc_connect_ctrl_work / _reset_ctrl_work → [이 함수]
 *     → nvme_fc_connect_admin_queue → nvmf_connect_admin_queue → nvme_enable_ctrl
 */
static int
nvme_fc_create_association(struct nvme_fc_ctrl *ctrl)
{
	struct nvmf_ctrl_options *opts = ctrl->ctrl.opts;
	struct nvmefc_ls_rcv_op *disls = NULL;	/* [한국어] 실패 시 응답해 줘야 할, 타겟이 보내 온 Disconnect */
	unsigned long flags;
	int ret;

	++ctrl->ctrl.nr_reconnects;	/* [한국어] 재시도 횟수. 성공하면 아래에서 0 으로 되돌린다 */

	spin_lock_irqsave(&ctrl->rport->lock, flags);
	if (ctrl->rport->remoteport.port_state != FC_OBJSTATE_ONLINE) {	/* [한국어] 포트가 아직 안 돌아왔다 */
		spin_unlock_irqrestore(&ctrl->rport->lock, flags);
		return -ENODEV;	/* [한국어] 재연결 작업이 나중에 다시 시도한다 */
	}

	if (nvme_fc_ctlr_active_on_rport(ctrl)) {	/* [한국어] 같은 컨트롤러가 이미 이 포트에서 활성이다 */
		spin_unlock_irqrestore(&ctrl->rport->lock, flags);
		return -ENOTUNIQ;	/* [한국어] 세션을 두 번 세우면 association_id 가 어긋난다 */
	}
	spin_unlock_irqrestore(&ctrl->rport->lock, flags);

	dev_info(ctrl->ctrl.device,	/* [한국어] WWPN 쌍과 NQN 을 남긴다 — 어느 경로의 세션인지 나중에 추적하는 근거다 */
		"NVME-FC{%d}: create association : host wwpn 0x%016llx "
		" rport wwpn 0x%016llx: NQN \"%s\"\n",
		ctrl->cnum, ctrl->lport->localport.port_name,
		ctrl->rport->remoteport.port_name, ctrl->ctrl.opts->subsysnqn);

	clear_bit(ASSOC_FAILED, &ctrl->flags);	/* [한국어] 이번 시도를 위해 실패 표시를 지운다. 아래 단계마다 다시 확인한다 */

	/*
	 * Create the admin queue
	 */

	ret = __nvme_fc_create_hw_queue(ctrl, &ctrl->queues[0], 0,	/* [한국어] 1층: LLDD 에 admin 채널을 만들어 달라고 한다 */
				NVME_AQ_DEPTH);
	if (ret)
		goto out_free_queue;

	ret = nvme_fc_connect_admin_queue(ctrl, &ctrl->queues[0],	/* [한국어] 2층: Create Association + Create Connection LS. association_id 가 여기서 발급된다 */
				NVME_AQ_DEPTH, (NVME_AQ_DEPTH / 4));
	if (ret)
		goto out_delete_hw_queue;

	ret = nvmf_connect_admin_queue(&ctrl->ctrl);	/* [한국어] 3층: NVMe Fabrics Connect 명령. 이제서야 NVMe 를 말할 수 있다 */
	if (ret)
		goto out_disconnect_admin_queue;

	set_bit(NVME_FC_Q_LIVE, &ctrl->queues[0].flags);	/* [한국어] admin 큐로 명령을 보낼 수 있다 */

	/*
	 * Check controller capabilities
	 *
	 * todo:- add code to check if ctrl attributes changed from
	 * prior connection values
	 */

	ret = nvme_enable_ctrl(&ctrl->ctrl);	/* [한국어] CC.EN 을 세우고 CSTS.RDY 를 기다린다 — Property Set 으로 이뤄진다 */
	if (!ret && test_bit(ASSOC_FAILED, &ctrl->flags))	/* [한국어] 그 사이 비동기 통지가 세션 실패를 알렸을 수 있다 */
		ret = -EIO;
	if (ret)
		goto out_disconnect_admin_queue;

	ctrl->ctrl.max_segments = ctrl->lport->ops->max_sgl_segments;	/* [한국어] HBA 가 한 번에 다룰 수 있는 SG 항목 수 */
	ctrl->ctrl.max_hw_sectors = ctrl->ctrl.max_segments <<	/* [한국어] 그 값이 그대로 요청 크기 상한이 된다 — 하드웨어 한도가 bio 분할 지점을 정한다 */
						(ilog2(SZ_4K) - 9);	/* [한국어] 세그먼트당 4K, 섹터 단위(512B)로 환산 */

	nvme_unquiesce_admin_queue(&ctrl->ctrl);	/* [한국어] 이제 admin 요청을 흘려보내도 된다 */

	ret = nvme_init_ctrl_finish(&ctrl->ctrl, false);	/* [한국어] Identify 를 읽어 컨트롤러 능력을 확정한다 */
	if (ret)
		goto out_disconnect_admin_queue;
	if (test_bit(ASSOC_FAILED, &ctrl->flags)) {	/* [한국어] 다시 확인 — 각 단계 사이가 모두 창구다 */
		ret = -EIO;
		goto out_stop_keep_alive;
	}
	/* sanity checks */

	/* FC-NVME does not have other data in the capsule */
	if (ctrl->ctrl.icdoff) {	/* [한국어] 위 영어 주석대로 FC 는 캡슐에 데이터를 싣지 않는다 */
		dev_err(ctrl->ctrl.device, "icdoff %d is not supported!\n",	/* [한국어] 데이터는 별도 FC 데이터 프레임으로 오가므로 캡슐 내 오프셋 개념이 없다 */
				ctrl->ctrl.icdoff);
		ret = NVME_SC_INVALID_FIELD | NVME_STATUS_DNR;
		goto out_stop_keep_alive;
	}

	/* FC-NVME supports normal SGL Data Block Descriptors */
	if (!nvme_ctrl_sgl_supported(&ctrl->ctrl)) {	/* [한국어] FC 는 PRP 를 쓰지 않으므로 SGL 지원이 필수다 */
		dev_err(ctrl->ctrl.device,
			"Mandatory sgls are not supported!\n");
		ret = NVME_SC_INVALID_FIELD | NVME_STATUS_DNR;
		goto out_stop_keep_alive;
	}

	if (opts->queue_size > ctrl->ctrl.maxcmd) {	/* [한국어] 사용자가 요청한 큐 깊이가 컨트롤러 한도를 넘는다 */
		/* warn if maxcmd is lower than queue_size */
		dev_warn(ctrl->ctrl.device,
			"queue_size %zu > ctrl maxcmd %u, reducing "
			"to maxcmd\n",
			opts->queue_size, ctrl->ctrl.maxcmd);
		opts->queue_size = ctrl->ctrl.maxcmd;	/* [한국어] 한도에 맞춰 줄인다 — 넘겨 보내면 컨트롤러가 거절한다 */
		ctrl->ctrl.sqsize = opts->queue_size - 1;	/* [한국어] sqsize 는 0-based 라 하나 뺀다 */
	}

	ret = nvme_fc_init_aen_ops(ctrl);	/* [한국어] 비동기 이벤트용 상주 오퍼레이션을 준비한다 */
	if (ret)
		goto out_term_aen_ops;

	/*
	 * Create the io queues
	 */

	if (ctrl->ctrl.queue_count > 1) {	/* [한국어] I/O 큐가 있는 구성이라면 */
		if (!ctrl->ioq_live)
			ret = nvme_fc_create_io_queues(ctrl);	/* [한국어] 처음이면 태그셋부터 만든다 */
		else
			ret = nvme_fc_recreate_io_queues(ctrl);	/* [한국어] 재연결이면 기존 태그셋 위에 채널만 다시 연다 */
	}
	if (!ret && test_bit(ASSOC_FAILED, &ctrl->flags))
		ret = -EIO;
	if (ret)
		goto out_term_aen_ops;

	if (!nvme_change_ctrl_state(&ctrl->ctrl, NVME_CTRL_LIVE)) {	/* [한국어] 마지막 관문 — 그 사이 삭제가 시작됐으면 실패한다 */
		ret = -EIO;
		goto out_term_aen_ops;
	}

	ctrl->ctrl.nr_reconnects = 0;	/* [한국어] 성공했으니 재시도 횟수를 되돌린다 */
	nvme_start_ctrl(&ctrl->ctrl);	/* [한국어] keep-alive 를 걸고 네임스페이스를 스캔한다 */

	return 0;	/* Success */

out_term_aen_ops:	/* [한국어] 아래부터 역순 되감기 — 세운 순서의 반대로 내린다 */
	nvme_fc_term_aen_ops(ctrl);
out_stop_keep_alive:
	nvme_stop_keep_alive(&ctrl->ctrl);
out_disconnect_admin_queue:
	dev_warn(ctrl->ctrl.device,
		"NVME-FC{%d}: create_assoc failed, assoc_id %llx ret %d\n",
		ctrl->cnum, ctrl->association_id, ret);
	/* send a Disconnect(association) LS to fc-nvme target */
	/* [한국어] 타겟 쪽에도 세션을 지우라고 알려야 한다. 알리지 않으면 타겟이
	 * 죽은 association 을 계속 들고 있어 다음 연결이 ENOTUNIQ 로 막힌다. */
	nvme_fc_xmt_disconnect_assoc(ctrl);
	spin_lock_irqsave(&ctrl->lock, flags);
	ctrl->association_id = 0;
	disls = ctrl->rcv_disconn;	/* [한국어] 타겟이 먼저 보내 온 Disconnect 가 있으면 꺼내 둔다 */
	ctrl->rcv_disconn = NULL;
	spin_unlock_irqrestore(&ctrl->lock, flags);
	if (disls)
		nvme_fc_xmt_ls_rsp(disls);	/* [한국어] 로컬 정리가 끝난 지금에야 ACC 를 보낸다 — 순서가 뒤집히면 타겟이 먼저 재접속을 시도한다 */
out_delete_hw_queue:
	__nvme_fc_delete_hw_queue(ctrl, &ctrl->queues[0], 0);
out_free_queue:
	nvme_fc_free_queue(&ctrl->queues[0]);
	clear_bit(ASSOC_ACTIVE, &ctrl->flags);
	nvme_fc_ctlr_inactive_on_rport(ctrl);	/* [한국어] 포트의 활성 컨트롤러 수를 내린다 — 0 이 되면 LLDD 모듈을 내릴 수 있다 */

	return ret;
}


/*
 * This routine stops operation of the controller on the host side.
 * On the host os stack side: Admin and IO queues are stopped,
 *   outstanding ios on them terminated via FC ABTS.
 * On the link side: the association is terminated.
 */
/*
 * [한국어]
 * nvme_fc_delete_association - FC 세션을 안전한 순서로 해체한다
 *
 * @ctrl: 해체할 컨트롤러
 * @return: 없음
 *
 * create_association 의 역순이지만, 단순한 되감기가 아니다. 진행 중인 I/O 가
 * 있고 그것들이 HBA 안에서 아직 살아 있기 때문에, "취소를 요청하고 그 완료를
 * 전부 받을 때까지 기다리는" 구간이 중간에 들어간다.
 *
 * 순서와 그 이유:
 *   1) ASSOC_ACTIVE 를 test_and_clear 로 끈다. 이 한 줄이 중복 해체 방지의
 *      전부다 -- 오류 복구와 사용자 삭제가 동시에 들어와도 하나만 통과한다.
 *   2) FCCTRL_TERMIO 를 세우고 iocnt 를 0 으로 둔다. 이제부터 abort 를 거는
 *      요청마다 iocnt 가 오르고, 완료가 돌아올 때마다 내려간다.
 *   3) 진행 중 I/O 와 AEN 에 abort 를 요청한다.
 *   4) iocnt 가 0 이 될 때까지 기다린다. 이 대기를 건너뛰면 아직 HBA 가
 *      DMA 중인 버퍼를 해제하게 된다.
 *   5) Disconnect LS 를 보낸다. 위 영어 주석이 밝히듯 맨 앞에서 보낼 수도
 *      있었지만, abort 가 모두 끝난 뒤에 보내는 편이 링크 트래픽이 깔끔하다.
 *   6) 큐와 하드웨어 채널을 해제한다.
 *   7) 큐를 다시 열어 둔다.
 *
 * 마지막 단계가 직관에 반한다. 해체하면서 큐를 unquiesce 하는 이유는 위
 * 영어 주석대로 "빨리 실패시키기" 위해서다. 큐가 멈춰 있으면 새 요청이
 * 그 안에 쌓여 기다리지만, 열어 두면 곧바로 실패해 상위(다중 경로나
 * 파일시스템)가 즉시 다른 경로를 찾거나 오류를 볼 수 있다.
 *
 * 실행 컨텍스트: 워크큐(err_work / reset_work) 또는 삭제 경로. wait_event 로
 * 잠들 수 있어야 한다.
 *
 * 호출 체인:
 *   nvme_fc_delete_ctrl / _reset_ctrl_work / _ioerr_work → [이 함수]
 */
static void
nvme_fc_delete_association(struct nvme_fc_ctrl *ctrl)
{
	struct nvmefc_ls_rcv_op *disls = NULL;
	unsigned long flags;

	if (!test_and_clear_bit(ASSOC_ACTIVE, &ctrl->flags))	/* [한국어] 비트를 끄는 데 성공한 쪽만 해체한다 — 중복 진입 방지의 전부다 */
		return;

	spin_lock_irqsave(&ctrl->lock, flags);
	set_bit(FCCTRL_TERMIO, &ctrl->flags);	/* [한국어] 종료 중임을 알린다. 완료 경로가 이 비트를 보고 iocnt 를 관리한다 */
	ctrl->iocnt = 0;	/* [한국어] 취소를 기다릴 요청 수를 새로 센다 */
	spin_unlock_irqrestore(&ctrl->lock, flags);

	__nvme_fc_abort_outstanding_ios(ctrl, false);	/* [한국어] 진행 중인 I/O 마다 LLDD 에 abort 를 건다. 거는 만큼 iocnt 가 오른다 */

	/* kill the aens as they are a separate path */
	nvme_fc_abort_aen_ops(ctrl);	/* [한국어] 위 영어 주석대로 AEN 은 태그셋 밖의 별도 경로라 따로 취소해야 한다 */

	/* wait for all io that had to be aborted */
	spin_lock_irq(&ctrl->lock);
	wait_event_lock_irq(ctrl->ioabort_wait, ctrl->iocnt == 0, ctrl->lock);	/* [한국어] 여기를 건너뛰면 HBA 가 아직 DMA 중인 버퍼를 해제하게 된다 */
	clear_bit(FCCTRL_TERMIO, &ctrl->flags);
	spin_unlock_irq(&ctrl->lock);

	nvme_fc_term_aen_ops(ctrl);	/* [한국어] 취소가 끝났으니 AEN 오퍼레이션의 버퍼를 해제한다 */

	/*
	 * send a Disconnect(association) LS to fc-nvme target
	 * Note: could have been sent at top of process, but
	 * cleaner on link traffic if after the aborts complete.
	 * Note: if association doesn't exist, association_id will be 0
	 */
	/* [한국어] 위 영어 주석대로 순서는 선택의 문제다. abort 가 오가는 중에
	 * Disconnect 를 섞어 보내는 것보다, 다 끝난 뒤 보내는 편이 링크가 깔끔하다. */
	if (ctrl->association_id)	/* [한국어] 세션이 실제로 서 있었을 때만 보낸다 */
		nvme_fc_xmt_disconnect_assoc(ctrl);

	spin_lock_irqsave(&ctrl->lock, flags);
	ctrl->association_id = 0;	/* [한국어] 이제 이 세션은 없다 */
	disls = ctrl->rcv_disconn;	/* [한국어] 타겟이 먼저 보내 온 Disconnect 가 대기 중이었나 */
	ctrl->rcv_disconn = NULL;
	spin_unlock_irqrestore(&ctrl->lock, flags);
	if (disls)
		/*
		 * if a Disconnect Request was waiting for a response, send
		 * now that all ABTS's have been issued (and are complete).
		 */
		/* [한국어] 위 영어 주석대로 ABTS 가 모두 끝난 지금에야 응답한다.
		 * 먼저 응답하면 타겟이 곧바로 재접속을 시도하는데, 이쪽은 아직
		 * 이전 세션의 I/O 를 정리하는 중이라 충돌한다. */
		nvme_fc_xmt_ls_rsp(disls);

	if (ctrl->ctrl.tagset) {	/* [한국어] I/O 큐가 있었던 구성이면 */
		nvme_fc_delete_hw_io_queues(ctrl);	/* [한국어] LLDD 쪽 채널을 닫고 */
		nvme_fc_free_io_queues(ctrl);	/* [한국어] 큐 자원을 해제한다 */
	}

	__nvme_fc_delete_hw_queue(ctrl, &ctrl->queues[0], 0);	/* [한국어] admin 채널도 닫는다 */
	nvme_fc_free_queue(&ctrl->queues[0]);

	/* re-enable the admin_q so anything new can fast fail */
	/* [한국어] 해체하면서 큐를 여는 것이 직관에 반하지만 의도적이다. 멈춰 두면
	 * 새 요청이 쌓여 기다리고, 열어 두면 곧바로 실패해 상위가 즉시 대응한다. */
	nvme_unquiesce_admin_queue(&ctrl->ctrl);

	/* resume the io queues so that things will fast fail */
	nvme_unquiesce_io_queues(&ctrl->ctrl);	/* [한국어] 같은 이유 — 다중 경로가 있으면 여기서 곧바로 다른 경로로 넘어간다 */

	nvme_fc_ctlr_inactive_on_rport(ctrl);	/* [한국어] 포트의 활성 컨트롤러 수를 내린다 */
}

/*
 * [한국어]
 * nvme_fc_delete_ctrl - 컨트롤러를 완전히 없앤다
 *
 * @nctrl: 삭제할 컨트롤러
 * @return: 없음
 *
 * 코어의 delete_ctrl vtable 진입점이다. 순서가 곧 안전성이다.
 *
 * 먼저 재연결 워크를 취소한다. 그러지 않으면 해체가 끝난 뒤 그 워크가
 * 돌아 이미 사라진 자원 위에 세션을 세우려 한다. _sync 라 이미 실행
 * 중인 것이 끝날 때까지 기다린다.
 *
 * 다음이 association 해체이며, 위 영어 주석대로 진행 중인 I/O 의 abort
 * 완료를 기다리느라 블로킹한다. 그 뒤에야 ioerr_work 를 취소할 수 있다 --
 * 해체 도중에 그 워크가 걸릴 수 있으므로 순서를 바꾸면 취소를 빠져나간다.
 *
 * 태그셋은 I/O 가 있었던 경우에만 해제하고, admin 큐는 열어 둔 채로
 * 태그셋을 없앤다. unquiesce 가 먼저인 이유는 멈춘 큐에 남은 요청이
 * 태그셋 해제를 막기 때문이다.
 *
 * 실행 컨텍스트: nvme_delete_wq 워크큐. 오래 잠들 수 있다.
 *
 * 호출 체인: nvme_delete_ctrl → ops->delete_ctrl → [이 함수]
 */
static void
nvme_fc_delete_ctrl(struct nvme_ctrl *nctrl)
{
	struct nvme_fc_ctrl *ctrl = to_fc_ctrl(nctrl);

	cancel_delayed_work_sync(&ctrl->connect_work);	/* [한국어] 해체 뒤 이 워크가 돌면 사라진 자원 위에 세션을 세우려 한다 */

	/*
	 * kill the association on the link side.  this will block
	 * waiting for io to terminate
	 */
	/* [한국어] 위 영어 주석대로 진행 중 I/O 의 abort 완료를 기다리며 블로킹한다 */
	nvme_fc_delete_association(ctrl);
	cancel_work_sync(&ctrl->ioerr_work);	/* [한국어] 해체 도중 걸릴 수 있어 그 뒤에 취소해야 빠져나가지 않는다 */

	if (ctrl->ctrl.tagset)	/* [한국어] I/O 큐가 있었던 구성에서만 */
		nvme_remove_io_tag_set(&ctrl->ctrl);

	nvme_unquiesce_admin_queue(&ctrl->ctrl);	/* [한국어] 멈춘 큐에 남은 요청이 태그셋 해제를 막으므로 먼저 연다 */
	nvme_remove_admin_tag_set(&ctrl->ctrl);
}

/*
 * [한국어]
 * nvme_fc_reconnect_or_delete - 연결 시도가 실패했을 때 다시 시도할지 포기할지 정한다
 *
 * @ctrl:   연결에 실패한 컨트롤러
 * @status: 실패 원인. 양수면 NVMe 상태 코드, 음수면 errno.
 * @return: 없음
 *
 * FC 의 재연결 정책이 여기 모여 있다. 판단에 들어가는 축이 둘이라는 점이
 * 다른 트랜스포트와 다르다:
 *
 *   1) 재연결 시도 횟수 -- 모든 Fabrics 트랜스포트가 공유하는 정책.
 *   2) dev_loss_tmo -- 포트가 사라진 뒤 얼마나 기다릴 것인가. FC 고유다.
 *
 * 그래서 "포트는 살아 있는데 연결이 안 되는" 경우와 "포트 자체가 사라진"
 * 경우가 갈린다. 전자는 시도 횟수가 소진될 때까지 계속하고, 후자는 시도
 * 횟수가 남았더라도 dev_loss_end 를 넘기면 포기한다.
 *
 * recon_delay 를 dev_loss_end 에 맞춰 줄이는 부분이 그 두 축을 잇는 지점이다.
 * 다음 시도가 유예 시간 이후로 잡히면 그 시도는 의미가 없으므로, 유예가
 * 끝나는 시각에 맞춰 마지막 한 번을 시도한다.
 *
 * DNR(Do Not Retry) 비트를 따로 보는 이유: 타겟이 "이 요청은 다시 보내도
 * 소용없다"고 명시한 것이라, 횟수가 남았어도 재시도하지 않는다. 로그도
 * 그 경우와 횟수 소진을 구분해 남긴다.
 *
 * 실행 컨텍스트: connect_work / reset_work. 잠들 수 있다.
 *
 * 호출 체인:
 *   nvme_fc_connect_ctrl_work / _reset_ctrl_work → [이 함수]
 *     → queue_delayed_work(connect_work) 또는 nvme_delete_ctrl
 */
static void
nvme_fc_reconnect_or_delete(struct nvme_fc_ctrl *ctrl, int status)
{
	struct nvme_fc_rport *rport = ctrl->rport;
	struct nvme_fc_remote_port *portptr = &rport->remoteport;
	unsigned long recon_delay = ctrl->ctrl.opts->reconnect_delay * HZ;	/* [한국어] 사용자가 지정한 시도 간격 */
	bool recon = true;	/* [한국어] 기본은 재시도. 아래에서 포기 조건에 걸리면 false 가 된다 */

	if (nvme_ctrl_state(&ctrl->ctrl) != NVME_CTRL_CONNECTING)	/* [한국어] 그 사이 삭제나 리셋이 시작됐으면 */
		return;	/* [한국어] 그쪽이 처리하도록 두고 물러난다 */

	if (portptr->port_state == FC_OBJSTATE_ONLINE) {	/* [한국어] 포트는 살아 있는데 연결이 안 되는 경우 */
		dev_info(ctrl->ctrl.device,
			"NVME-FC{%d}: reset: Reconnect attempt failed (%d)\n",
			ctrl->cnum, status);
	} else if (time_after_eq(jiffies, rport->dev_loss_end))	/* [한국어] 포트가 사라졌고 유예 시간도 지났다 */
		recon = false;	/* [한국어] 시도 횟수가 남았어도 포기한다 — FC 고유의 두 번째 축이다 */

	if (recon && nvmf_should_reconnect(&ctrl->ctrl, status)) {	/* [한국어] 유예가 남았고 Fabrics 공통 정책도 재시도를 허용하면 */
		if (portptr->port_state == FC_OBJSTATE_ONLINE)
			dev_info(ctrl->ctrl.device,
				"NVME-FC{%d}: Reconnect attempt in %ld "
				"seconds\n",
				ctrl->cnum, recon_delay / HZ);
		else if (time_after(jiffies + recon_delay, rport->dev_loss_end))	/* [한국어] 다음 시도가 유예 이후로 잡히면 그 시도는 무의미하다 */
			recon_delay = rport->dev_loss_end - jiffies;	/* [한국어] 유예가 끝나는 시각에 맞춰 마지막 한 번을 시도한다 */

		queue_delayed_work(nvme_wq, &ctrl->connect_work, recon_delay);	/* [한국어] 그 간격 뒤 다시 연결을 시도한다 */
	} else {
		if (portptr->port_state == FC_OBJSTATE_ONLINE) {
			if (status > 0 && (status & NVME_STATUS_DNR))	/* [한국어] 타겟이 "다시 보내도 소용없다"고 명시했다 */
				dev_warn(ctrl->ctrl.device,	/* [한국어] 횟수가 남았어도 재시도하지 않는다 */
					 "NVME-FC{%d}: reconnect failure\n",
					 ctrl->cnum);
			else
				dev_warn(ctrl->ctrl.device,	/* [한국어] 시도 횟수를 다 썼다 */
					 "NVME-FC{%d}: Max reconnect attempts "
					 "(%d) reached.\n",
					 ctrl->cnum, ctrl->ctrl.nr_reconnects);
		} else
			dev_warn(ctrl->ctrl.device,	/* [한국어] 포트가 끝내 돌아오지 않았다 */
				"NVME-FC{%d}: dev_loss_tmo (%d) expired "
				"while waiting for remoteport connectivity.\n",
				ctrl->cnum, min_t(int, portptr->dev_loss_tmo,	/* [한국어] 실제로 기다린 시간은 두 정책 중 짧은 쪽이다 */
					(ctrl->ctrl.opts->max_reconnects *
					 ctrl->ctrl.opts->reconnect_delay)));
		WARN_ON(nvme_delete_ctrl(&ctrl->ctrl));	/* [한국어] 포기하고 컨트롤러를 지운다. 실패하면 상태 기계가 어긋난 것이라 경고 */
	}
}

/*
 * [한국어]
 * nvme_fc_reset_ctrl_work - 컨트롤러를 완전히 내렸다가 다시 세운다
 *
 * @work: ctrl.reset_work
 * @return: 없음
 *
 * 리셋은 "세션을 버리고 처음부터 다시"다. 그래서 절차가 해체와 수립의
 * 연결이며, 이 함수는 그 둘을 잇는 얇은 껍데기에 가깝다.
 *
 * delete_association 이 블로킹이라는 점이 중요하다. 진행 중인 I/O 에 abort 를
 * 걸고 그 완료를 전부 받을 때까지 여기서 기다린다. 기다리지 않고 다음
 * 단계로 가면 이전 세션의 I/O 가 새 세션 위로 완료되어 들어온다.
 *
 * CONNECTING 으로의 전이 실패를 오류로 기록만 하고 진행하는 이유: 그 사이
 * 삭제가 시작된 경우인데, 삭제 경로가 알아서 정리하므로 여기서 되돌릴 일이
 * 없다. 다만 예상 밖의 상태 조합일 수 있어 로그는 남긴다.
 *
 * 포트가 살아 있으면 곧바로 연결을 시도하고, flush 로 그 결과까지 기다린다.
 * 리셋을 요청한 쪽(예: sysfs write)이 완료를 기다리고 있기 때문이다.
 * 포트가 없으면 시도할 것이 없으므로 재연결 정책 판단으로 바로 넘긴다.
 *
 * 실행 컨텍스트: nvme_reset_wq 워크큐. 오래 잠들 수 있다.
 *
 * 호출 체인:
 *   nvme_reset_ctrl / error_recovery → [이 함수]
 *     → nvme_fc_delete_association → nvme_fc_connect_ctrl_work
 */
static void
nvme_fc_reset_ctrl_work(struct work_struct *work)
{
	struct nvme_fc_ctrl *ctrl =
		container_of(work, struct nvme_fc_ctrl, ctrl.reset_work);

	nvme_stop_ctrl(&ctrl->ctrl);	/* [한국어] keep-alive 와 스캔을 멈추고 큐를 정지시킨다 */

	/* will block will waiting for io to terminate */
	nvme_fc_delete_association(ctrl);	/* [한국어] 위 영어 주석대로 여기서 블로킹한다 — 이전 세션의 I/O 완료를 다 받아야 새 세션과 섞이지 않는다 */

	if (!nvme_change_ctrl_state(&ctrl->ctrl, NVME_CTRL_CONNECTING))	/* [한국어] 그 사이 삭제가 시작됐을 수 있다 */
		dev_err(ctrl->ctrl.device,	/* [한국어] 삭제 경로가 정리하므로 되돌릴 것은 없지만, 예상 밖 조합일 수 있어 남긴다 */
			"NVME-FC{%d}: error_recovery: Couldn't change state "
			"to CONNECTING\n", ctrl->cnum);

	if (ctrl->rport->remoteport.port_state == FC_OBJSTATE_ONLINE) {	/* [한국어] 포트가 살아 있어야 시도할 의미가 있다 */
		if (!queue_delayed_work(nvme_wq, &ctrl->connect_work, 0)) {	/* [한국어] 지연 0 — 곧바로 연결을 시도한다 */
			dev_err(ctrl->ctrl.device,	/* [한국어] 이미 큐에 올라 있다는 뜻이라 정상 상황이 아니다 */
				"NVME-FC{%d}: failed to schedule connect "
				"after reset\n", ctrl->cnum);
		} else {
			flush_delayed_work(&ctrl->connect_work);	/* [한국어] 리셋을 요청한 쪽이 결과를 기다리므로 연결이 끝날 때까지 기다린다 */
		}
	} else {
		nvme_fc_reconnect_or_delete(ctrl, -ENOTCONN);	/* [한국어] 포트가 없으면 시도할 것이 없다 — 유예 안이면 대기, 지났으면 삭제 */
	}
}


static const struct nvme_ctrl_ops nvme_fc_ctrl_ops = {	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	.name			= "fc",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	.module			= THIS_MODULE,
	.flags			= NVME_F_FABRICS,
	.reg_read32		= nvmf_reg_read32,
	.reg_read64		= nvmf_reg_read64,
	.reg_write32		= nvmf_reg_write32,
	.subsystem_reset	= nvmf_subsystem_reset,
	.free_ctrl		= nvme_fc_free_ctrl,
	.submit_async_event	= nvme_fc_submit_async_event,
	.delete_ctrl		= nvme_fc_delete_ctrl,
	.get_address		= nvmf_get_address,
	.get_virt_boundary	= nvmf_get_virt_boundary,
};

/*
 * [한국어]
 * nvme_fc_connect_ctrl_work - 세션 수립을 시도하고 결과에 따라 다음을 정한다
 *
 * @work: connect_work(지연 워크)
 * @return: 없음
 *
 * 재연결 루프의 한 바퀴다. 실패하면 reconnect_or_delete 가 다시 이 워크를
 * 예약하므로, 두 함수가 서로를 부르며 시도 횟수나 dev_loss_tmo 가 소진될
 * 때까지 반복한다. 성공하면 로그만 남기고 루프가 끝난다.
 *
 * 지연 워크인 이유가 그 루프에 있다. 실패 후 곧바로 다시 시도하면 링크가
 * 아직 회복되지 않은 상태에서 무의미한 시도가 반복되므로, 옵션에 적힌
 * 간격만큼 쉬었다 돈다.
 *
 * 실행 컨텍스트: nvme_wq 워크큐. 세션 수립 전체가 여기서 일어나므로 오래 잠든다.
 *
 * 호출 체인:
 *   nvme_fc_init_ctrl / _reset_ctrl_work / _reconnect_or_delete
 *     → [이 함수] → nvme_fc_create_association → nvme_fc_reconnect_or_delete
 */
static void
nvme_fc_connect_ctrl_work(struct work_struct *work)
{
	int ret;

	struct nvme_fc_ctrl *ctrl =
			container_of(to_delayed_work(work),	/* [한국어] 지연 워크라 to_delayed_work 를 한 번 거쳐야 한다 */
				struct nvme_fc_ctrl, connect_work);

	ret = nvme_fc_create_association(ctrl);	/* [한국어] admin 부터 LIVE 까지의 전체 절차 */
	if (ret)
		nvme_fc_reconnect_or_delete(ctrl, ret);	/* [한국어] 실패 — 다시 시도할지 포기할지 그쪽이 정한다. 다시 시도면 이 워크가 재예약된다 */
	else
		dev_info(ctrl->ctrl.device,	/* [한국어] 성공하면 루프가 끝난다 */
			"NVME-FC{%d}: controller connect complete\n",
			ctrl->cnum);
}


static const struct blk_mq_ops nvme_fc_admin_mq_ops = {	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	.queue_rq	= nvme_fc_queue_rq,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	.complete	= nvme_fc_complete_rq,
	.init_request	= nvme_fc_init_request,
	.exit_request	= nvme_fc_exit_request,
	.init_hctx	= nvme_fc_init_admin_hctx,
	.timeout	= nvme_fc_timeout,
};


/*
 * Fails a controller request if it matches an existing controller
 * (association) with the same tuple:
 * <Host NQN, Host ID, local FC port, remote FC port, SUBSYS NQN>
 *
 * The ports don't need to be compared as they are intrinsically
 * already matched by the port pointers supplied.
 */
static bool
nvme_fc_existing_controller(struct nvme_fc_rport *rport,
		struct nvmf_ctrl_options *opts)
{
	struct nvme_fc_ctrl *ctrl;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	unsigned long flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	bool found = false;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	spin_lock_irqsave(&rport->lock, flags);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
	list_for_each_entry(ctrl, &rport->ctrl_list, ctrl_list) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		found = nvmf_ctlr_matches_baseopts(&ctrl->ctrl, opts);	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
		if (found)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			break;
	}
	spin_unlock_irqrestore(&rport->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return found;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static struct nvme_fc_ctrl *	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
nvme_fc_alloc_ctrl(struct device *dev, struct nvmf_ctrl_options *opts,
	struct nvme_fc_lport *lport, struct nvme_fc_rport *rport)
{
	struct nvme_fc_ctrl *ctrl;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	int ret, idx, ctrl_loss_tmo;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!(rport->remoteport.port_role &	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
	    (FC_PORT_ROLE_NVME_DISCOVERY | FC_PORT_ROLE_NVME_TARGET))) {
		ret = -EBADR;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_fail;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}

	if (!opts->duplicate_connect &&	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
	    nvme_fc_existing_controller(rport, opts)) {
		ret = -EALREADY;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_fail;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}

	ctrl = kzalloc_obj(*ctrl);	/* [한국어] 커널 메모리 생명주기 */
	if (!ctrl) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = -ENOMEM;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_fail;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}

	idx = ida_alloc(&nvme_fc_ctrl_cnt, GFP_KERNEL);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	if (idx < 0) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = -ENOSPC;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_free_ctrl;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}

	/*
	 * if ctrl_loss_tmo is being enforced and the default reconnect delay
	 * is being used, change to a shorter reconnect delay for FC.
	 */
	if (opts->max_reconnects != -1 &&	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
	    opts->reconnect_delay == NVMF_DEF_RECONNECT_DELAY &&
	    opts->reconnect_delay > NVME_FC_DEFAULT_RECONNECT_TMO) {
		ctrl_loss_tmo = opts->max_reconnects * opts->reconnect_delay;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		opts->reconnect_delay = NVME_FC_DEFAULT_RECONNECT_TMO;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		opts->max_reconnects = DIV_ROUND_UP(ctrl_loss_tmo,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
						opts->reconnect_delay);
	}

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
				opts->nr_io_queues,
				lport->ops->max_hw_queues);
	ctrl->ctrl.queue_count++;	/* +1 for admin queue */	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ctrl->ctrl.sqsize = opts->queue_size - 1;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ctrl->ctrl.kato = opts->kato;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ctrl->ctrl.cntlid = 0xffff;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = -ENOMEM;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ctrl->queues = kzalloc_objs(struct nvme_fc_queue,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
				    ctrl->ctrl.queue_count);
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
		ctrl->ctrl.numa_node = dev_to_node(lport->dev);

	return ctrl;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

out_free_queues:
	kfree(ctrl->queues);	/* [한국어] 커널 메모리 생명주기 */
out_free_ida:
	put_device(ctrl->dev);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ida_free(&nvme_fc_ctrl_cnt, ctrl->cnum);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
out_free_ctrl:
	kfree(ctrl);	/* [한국어] 커널 메모리 생명주기 */
out_fail:
	/* exit via here doesn't follow ctlr ref points */
	return ERR_PTR(ret);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static struct nvme_ctrl *	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
nvme_fc_init_ctrl(struct device *dev, struct nvmf_ctrl_options *opts,
	struct nvme_fc_lport *lport, struct nvme_fc_rport *rport)
{
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
			&nvme_fc_admin_mq_ops,
			struct_size_t(struct nvme_fcp_op_w_sgl, priv,	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
				      ctrl->lport->ops->fcprqst_priv_sz));
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto fail_ctrl;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	spin_lock_irqsave(&rport->lock, flags);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
	list_add_tail(&ctrl->ctrl_list, &rport->ctrl_list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	spin_unlock_irqrestore(&rport->lock, flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!nvme_change_ctrl_state(&ctrl->ctrl, NVME_CTRL_CONNECTING)) {	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
		dev_err(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"NVME-FC{%d}: failed to init ctrl state\n", ctrl->cnum);
		goto fail_ctrl;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}

	if (!queue_delayed_work(nvme_wq, &ctrl->connect_work, 0)) {	/* [한국어] 워크큐 — 송수신/재연결/복구 비동기 실행 */
		dev_err(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"NVME-FC{%d}: failed to schedule initial connect\n",
			ctrl->cnum);
		goto fail_ctrl;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}

	flush_delayed_work(&ctrl->connect_work);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	dev_info(ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		"NVME-FC{%d}: new ctrl: NQN \"%s\", hostnqn: %s\n",
		ctrl->cnum, nvmf_ctrl_subsysnqn(&ctrl->ctrl), opts->host->nqn);

	return &ctrl->ctrl;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

fail_ctrl:
	nvme_change_ctrl_state(&ctrl->ctrl, NVME_CTRL_DELETING);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
	cancel_work_sync(&ctrl->ioerr_work);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	cancel_work_sync(&ctrl->ctrl.reset_work);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	cancel_delayed_work_sync(&ctrl->connect_work);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ctrl->ctrl.opts = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (ctrl->ctrl.admin_tagset)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_remove_admin_tag_set(&ctrl->ctrl);
	/* initiate nvme ctrl ref counting teardown */
	nvme_uninit_ctrl(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

out_put_ctrl:
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
}

struct nvmet_fc_traddr {	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	/* [한국어] World Wide Node Name. FC 노드(장치 전체)를 세계적으로 유일하게
	 *   식별하는 64비트 값이다. 한 장치의 여러 포트가 같은 nn 을 공유한다.
	 * 설정자: nvme_fc_parse_traddr 가 "nn-0x..." 문자열을 파싱해 채운다.
	 * 읽는 자: 로컬/원격 포트를 찾을 때 pn 과 함께 대조된다.
	 * 값 범위: 호스트 바이트 순서의 64비트. 선로에 나갈 때 변환된다. */
	u64	nn;

	/* [한국어] World Wide Port Name. 같은 노드 안에서 포트 하나를 구별한다.
	 * 왜 둘 다 필요한가: 노드만으로는 다중 포트 HBA 의 어느 포트인지 알 수
	 *   없고, 연결은 포트 단위로 맺어진다.
	 * 설정자: 같은 파서가 "pn-0x..." 부분에서 채운다.
	 * 읽는 자: nvme connect 인자에서 온 값과 등록된 포트를 대조하는 데 쓴다. */
	u64	pn;
};

static int
__nvme_fc_parse_u64(substring_t *sstr, u64 *val)
{
	u64 token64;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (match_u64(sstr, &token64))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return -EINVAL;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	*val = token64;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

/*
 * This routine validates and extracts the WWN's from the TRADDR string.
 * As kernel parsers need the 0x to determine number base, universally
 * build string to parse with 0x prefix before parsing name strings.
 */
/*
 * [한국어]
 * nvme_fc_parse_traddr - traddr 문자열에서 WWNN/WWPN 두 개를 뽑아낸다
 *
 * @traddr: 결과를 담을 구조체. nn(node name)과 pn(port name)이 채워진다.
 * @buf:    'nvme connect' 가 넘긴 주소 문자열
 * @blen:   그 버퍼의 최대 길이
 * @return: 0 이면 성공. -EINVAL 이면 형식이 맞지 않는다.
 *
 * FC 에는 IP 주소가 없다. 포트를 가리키는 것은 64비트 World Wide Name 두
 * 개이고, 사용자는 그것을 "nn-0x...,pn-0x..." 같은 문자열로 준다. 이 함수가
 * 그 문자열을 숫자 두 개로 되돌린다.
 *
 * 허용 형식이 둘인 것이 이 함수의 대부분이다. 0x 접두가 붙은 긴 형태와
 * 붙지 않은 짧은 형태가 있고, 둘은 전체 길이와 각 필드의 오프셋이 다르다.
 * 그래서 먼저 길이와 접두사로 어느 쪽인지 판정하고, 그에 맞는 오프셋을
 * 정한 뒤 같은 코드로 두 값을 뽑는다.
 *
 * name 버퍼에 "0x" 를 미리 박아 두는 것이 요령이다. 짧은 형태에는 접두가
 * 없으므로, 숫자 부분만 복사해 넣으면 어느 형태든 "0x..." 꼴이 되어
 * 하나의 파서로 처리할 수 있다. substring_t 가 그 버퍼를 가리키고 있어
 * 복사만 바꿔 끼우면 같은 파서를 두 번 부를 수 있다.
 *
 * 실행 컨텍스트: 연결 생성 경로. 잠들 수 있다.
 *
 * 호출 체인:
 *   nvme_fc_create_ctrl → [이 함수] → __nvme_fc_parse_u64
 */
static int
nvme_fc_parse_traddr(struct nvmet_fc_traddr *traddr, char *buf, size_t blen)
{
	char name[2 + NVME_FC_TRADDR_HEXNAMELEN + 1];	/* [한국어] "0x" + 16자리 16진수 + 종료 문자 */
	substring_t wwn = { name, &name[sizeof(name)-1] };	/* [한국어] 파서가 볼 구간. 버퍼를 가리키므로 내용만 바꿔 끼우면 재사용된다 */
	int nnoffset, pnoffset;	/* [한국어] 형식에 따라 달라지는 각 이름의 시작 위치 */

	/* validate if string is one of the 2 allowed formats */
	if (strnlen(buf, blen) == NVME_FC_TRADDR_MAXLENGTH &&	/* [한국어] 긴 형태: 0x 접두가 붙는다 */
			!strncmp(buf, "nn-0x", NVME_FC_TRADDR_OXNNLEN) &&
			!strncmp(&buf[NVME_FC_TRADDR_MAX_PN_OFFSET],
				"pn-0x", NVME_FC_TRADDR_OXNNLEN)) {
		nnoffset = NVME_FC_TRADDR_OXNNLEN;	/* [한국어] "nn-0x" 뒤부터가 숫자다 */
		pnoffset = NVME_FC_TRADDR_MAX_PN_OFFSET +
						NVME_FC_TRADDR_OXNNLEN;
	} else if ((strnlen(buf, blen) == NVME_FC_TRADDR_MINLENGTH &&	/* [한국어] 짧은 형태: 0x 가 없다 */
			!strncmp(buf, "nn-", NVME_FC_TRADDR_NNLEN) &&
			!strncmp(&buf[NVME_FC_TRADDR_MIN_PN_OFFSET],
				"pn-", NVME_FC_TRADDR_NNLEN))) {
		nnoffset = NVME_FC_TRADDR_NNLEN;	/* [한국어] "nn-" 뒤부터 */
		pnoffset = NVME_FC_TRADDR_MIN_PN_OFFSET + NVME_FC_TRADDR_NNLEN;
	} else
		goto out_einval;	/* [한국어] 둘 중 어느 형태도 아니다 */

	name[0] = '0';	/* [한국어] 접두를 미리 박아 두면 짧은 형태도 "0x..." 꼴이 되어 */
	name[1] = 'x';	/* [한국어] 하나의 파서로 두 형태를 모두 처리할 수 있다 */
	name[2 + NVME_FC_TRADDR_HEXNAMELEN] = 0;	/* [한국어] 종료 문자를 미리 둔다 — 아래 memcpy 가 이 자리를 덮지 않는다 */

	memcpy(&name[2], &buf[nnoffset], NVME_FC_TRADDR_HEXNAMELEN);	/* [한국어] 숫자 부분만 접두 뒤에 채운다 */
	if (__nvme_fc_parse_u64(&wwn, &traddr->nn))	/* [한국어] node name 을 64비트로 변환 */
		goto out_einval;

	memcpy(&name[2], &buf[pnoffset], NVME_FC_TRADDR_HEXNAMELEN);	/* [한국어] 같은 버퍼에 port name 을 덮어쓰고 */
	if (__nvme_fc_parse_u64(&wwn, &traddr->pn))	/* [한국어] 같은 파서를 다시 부른다 */
		goto out_einval;

	return 0;

out_einval:
	pr_warn("%s: bad traddr string\n", __func__);
	return -EINVAL;
}

/*
 * [한국어]
 * nvme_fc_create_ctrl - 'nvme connect -t fc' 가 도달하는 종착점
 *
 * @dev:  요청을 낸 장치(보통 /dev/nvme-fabrics)
 * @opts: 파싱된 연결 옵션. traddr 과 host_traddr 이 여기 들어 있다.
 * @return: 만들어진 컨트롤러. 실패하면 ERR_PTR.
 *
 * 다른 트랜스포트가 주소로 곧장 연결을 시도하는 것과 달리, FC 는 먼저
 * "그 주소에 해당하는 포트 쌍이 이미 등록돼 있는가"를 찾는다. FC 포트는
 * LLDD 가 발견해 등록해 둔 것이고, 이 계층은 그 목록에서 짝을 고를 뿐이다.
 * 없으면 -ENOENT 이며, 이는 케이블이 빠졌거나 존 설정이 아직 안 됐다는 뜻이다.
 *
 * 이중 순회가 그 탐색이다. 바깥은 로컬 포트(우리 HBA), 안쪽은 그 아래
 * 발견된 원격 포트. 둘 다 이름이 일치하고 ONLINE 이어야 한다.
 *
 * 순서에 주의할 점이 둘 있다. 첫째, 참조를 락 안에서 든다 -- 놓은 뒤에
 * 들면 그 사이 포트가 사라질 수 있다. 둘째, 락을 놓고 나서 init_ctrl 을
 * 부른다 -- 컨트롤러 생성은 잠들 수 있는 긴 작업이라 전역 락을 들고
 * 할 수 없다.
 *
 * 참조 획득 실패 시 break 로 빠지는 것은 위 영어 주석이 밝히듯 의도된
 * 것이다. 그 포트가 해제 중이라는 뜻이므로 더 찾지 않고 오류로 나간다.
 *
 * 실행 컨텍스트: 유저스페이스 write 경로. 잠들 수 있다.
 *
 * 호출 체인:
 *   유저 'nvme connect' → nvmf_dev_write → nvmf_create_ctrl
 *     → [이 함수] → nvme_fc_init_ctrl → nvme_fc_create_association
 */
static struct nvme_ctrl *
nvme_fc_create_ctrl(struct device *dev, struct nvmf_ctrl_options *opts)
{
	struct nvme_fc_lport *lport;
	struct nvme_fc_rport *rport;
	struct nvme_ctrl *ctrl;
	struct nvmet_fc_traddr laddr = { 0L, 0L };	/* [한국어] 우리 쪽 포트 이름 */
	struct nvmet_fc_traddr raddr = { 0L, 0L };	/* [한국어] 타겟 쪽 포트 이름 */
	unsigned long flags;
	int ret;

	ret = nvme_fc_parse_traddr(&raddr, opts->traddr, NVMF_TRADDR_SIZE);	/* [한국어] 타겟 주소 문자열 → WWNN/WWPN */
	if (ret || !raddr.nn || !raddr.pn)	/* [한국어] 0 은 유효한 WWN 이 아니다 */
		return ERR_PTR(-EINVAL);

	ret = nvme_fc_parse_traddr(&laddr, opts->host_traddr, NVMF_TRADDR_SIZE);	/* [한국어] FC 는 출발 포트도 명시해야 한다 — 여러 HBA 중 어느 것을 쓸지 정해야 하기 때문 */
	if (ret || !laddr.nn || !laddr.pn)
		return ERR_PTR(-EINVAL);

	/* find the host and remote ports to connect together */
	spin_lock_irqsave(&nvme_fc_lock, flags);	/* [한국어] 전역 포트 목록 보호. LLDD 등록이 인터럽트 문맥일 수 있어 irqsave */
	list_for_each_entry(lport, &nvme_fc_lport_list, port_list) {	/* [한국어] 바깥: 등록된 로컬 포트들 */
		if (lport->localport.node_name != laddr.nn ||
		    lport->localport.port_name != laddr.pn ||
		    lport->localport.port_state != FC_OBJSTATE_ONLINE)	/* [한국어] 이름이 맞아도 살아 있어야 한다 */
			continue;

		list_for_each_entry(rport, &lport->endp_list, endp_list) {	/* [한국어] 안쪽: 그 포트에서 발견한 원격 포트들 */
			if (rport->remoteport.node_name != raddr.nn ||
			    rport->remoteport.port_name != raddr.pn ||
			    rport->remoteport.port_state != FC_OBJSTATE_ONLINE)
				continue;

			/* if fail to get reference fall through. Will error */
			if (!nvme_fc_rport_get(rport))	/* [한국어] 락 안에서 참조를 든다 — 놓은 뒤에 들면 그 사이 사라질 수 있다 */
				break;	/* [한국어] 위 영어 주석대로 해제 중인 포트다. 더 찾지 않고 오류로 나간다 */

			spin_unlock_irqrestore(&nvme_fc_lock, flags);	/* [한국어] 컨트롤러 생성은 잠들 수 있어 전역 락을 놓고 부른다 */

			ctrl = nvme_fc_init_ctrl(dev, opts, lport, rport);	/* [한국어] 여기서 세션 수립까지 이어진다 */
			if (IS_ERR(ctrl))
				nvme_fc_rport_put(rport);	/* [한국어] 실패했으면 위에서 든 참조를 놓는다 */
			return ctrl;
		}
	}
	spin_unlock_irqrestore(&nvme_fc_lock, flags);

	pr_warn("%s: %s - %s combination not found\n",	/* [한국어] 짝을 못 찾았다 — 케이블이 빠졌거나 존 설정이 아직 안 된 상태다 */
		__func__, opts->traddr, opts->host_traddr);
	return ERR_PTR(-ENOENT);
}


static struct nvmf_transport_ops nvme_fc_transport = {	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	.name		= "fc",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	.module		= THIS_MODULE,
	.required_opts	= NVMF_OPT_TRADDR | NVMF_OPT_HOST_TRADDR,
	.allowed_opts	= NVMF_OPT_RECONNECT_DELAY | NVMF_OPT_CTRL_LOSS_TMO,
	.create_ctrl	= nvme_fc_create_ctrl,
};

/* Arbitrary successive failures max. With lots of subsystems could be high */
#define DISCOVERY_MAX_FAIL	20	/* [한국어] 상수/매크로 — PDU·큐·타임아웃·플래그 */

static ssize_t nvme_fc_nvme_discovery_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned long flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	LIST_HEAD(local_disc_list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_fc_lport *lport;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	struct nvme_fc_rport *rport;	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	int failcnt = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	spin_lock_irqsave(&nvme_fc_lock, flags);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
restart:
	list_for_each_entry(lport, &nvme_fc_lport_list, port_list) {	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		list_for_each_entry(rport, &lport->endp_list, endp_list) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			if (!nvme_fc_lport_get(lport))	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
				continue;
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
				       "failures\n");
				goto process_local_list;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
			}
			if (list_empty(&rport->disc_list))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
				list_add_tail(&rport->disc_list,
					      &local_disc_list);
		}
	}

process_local_list:
	while (!list_empty(&local_disc_list)) {	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		rport = list_first_entry(&local_disc_list,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
					 struct nvme_fc_rport, disc_list);
		list_del_init(&rport->disc_list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		spin_unlock_irqrestore(&nvme_fc_lock, flags);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

		lport = rport->lport;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		/* signal discovery. Won't hurt if it repeats */
		nvme_fc_signal_discovery_scan(lport, rport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		nvme_fc_rport_put(rport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
		nvme_fc_lport_put(lport);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

		spin_lock_irqsave(&nvme_fc_lock, flags);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	}
	spin_unlock_irqrestore(&nvme_fc_lock, flags);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

	return count;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static DEVICE_ATTR(nvme_discovery, 0200, NULL, nvme_fc_nvme_discovery_store);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

#ifdef CONFIG_BLK_CGROUP_FC_APPID	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
/* Parse the cgroup id from a buf and return the length of cgrpid */
static int fc_parse_cgrpid(const char *buf, u64 *id)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{
	char cgrp_id[16+1];	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int cgrpid_len, j;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	memset(cgrp_id, 0x0, sizeof(cgrp_id));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	for (cgrpid_len = 0, j = 0; cgrpid_len < 17; cgrpid_len++) {	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		if (buf[cgrpid_len] != ':')	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			cgrp_id[cgrpid_len] = buf[cgrpid_len];
		else {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			j = 1;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		}
	}
	if (!j)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return -EINVAL;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	if (kstrtou64(cgrp_id, 16, id) < 0)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return -EINVAL;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	return cgrpid_len;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

/*
 * Parse and update the appid in the blkcg associated with the cgroupid.
 */
static ssize_t fc_appid_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	size_t orig_count = count;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	u64 cgrp_id;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int appid_len = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int cgrpid_len = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	char app_id[FC_APPID_LEN];	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int ret = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (buf[count-1] == '\n')	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		count--;

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
}
static DEVICE_ATTR(appid_store, 0200, NULL, fc_appid_store);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
#endif /* CONFIG_BLK_CGROUP_FC_APPID */	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static struct attribute *nvme_fc_attrs[] = {
	&dev_attr_nvme_discovery.attr,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
#ifdef CONFIG_BLK_CGROUP_FC_APPID	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	&dev_attr_appid_store.attr,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
#endif	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	NULL	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
};

static const struct attribute_group nvme_fc_attr_group = {	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	.attrs = nvme_fc_attrs,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
};

static const struct attribute_group *nvme_fc_attr_groups[] = {	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	&nvme_fc_attr_group,	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
	NULL
};

static struct class fc_class = {	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	.name = "fc",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	.dev_groups = nvme_fc_attr_groups,
};

/*
 * [한국어]
 * nvme_fc_init_module - nvme-fc.ko 가 올라올 때 트랜스포트를 등록한다
 *
 * @return: 0 이면 등록 완료. 음수면 실패이며 잡은 것은 역순으로 되돌렸다.
 *
 * 두 가지를 한다. 하나는 udev 이벤트를 낼 장치 클래스를 만드는 것이고,
 * 다른 하나는 fabrics 코어에 "fc" 트랜스포트를 등록하는 것이다. 등록해야
 * 'nvme connect -t fc' 가 이 파일의 create_ctrl 에 도달할 수 있다.
 *
 * 클래스를 여기서 만드는 이유는 위 영어 주석이 길게 설명한다 -- 지금은
 * SCSI 아래 흩어져 있는 FC 관련 코드와 여기 NVMe 쪽 코드가 언젠가 독립된
 * FC 클래스로 합쳐질 것이라는 전망이고, 그때까지는 NVMe probe 이벤트를
 * 유저스페이스에 알릴 자리가 필요해 임시로 여기에 둔다는 것이다.
 * 그 클래스가 옮겨 가면 이 코드도 함께 옮겨 간다.
 *
 * 순서가 등록 순서이자 해제 역순이다. 클래스 → 장치 → 트랜스포트로 세우고,
 * 실패하면 goto 사다리가 그 반대로 되감는다.
 *
 * 실행 컨텍스트: 모듈 초기화. 잠들 수 있다.
 *
 * 호출 체인: module_init → [이 함수] → nvmf_register_transport
 */
static int __init nvme_fc_init_module(void)
{
	int ret;

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
	/* [한국어] 위 영어 주석의 요지: SCSI 쪽 FC 코드와 여기 NVMe 쪽이 언젠가
	 * 독립된 FC 클래스로 합쳐질 예정이고, 그때까지 udev 이벤트를 낼 자리가
	 * 필요해 이 클래스를 임시로 여기서 만든다. */
	ret = class_register(&fc_class);
	if (ret) {
		pr_err("couldn't register class fc\n");
		return ret;
	}

	/*
	 * Create a device for the FC-centric udev events
	 */
	fc_udev_device = device_create(&fc_class, NULL, MKDEV(0, 0), NULL,	/* [한국어] 이벤트를 붙일 대상 장치. MKDEV(0,0) 은 실제 장치 번호가 없다는 뜻이다 */
				"fc_udev_device");
	if (IS_ERR(fc_udev_device)) {
		pr_err("couldn't create fc_udev device!\n");
		ret = PTR_ERR(fc_udev_device);
		goto out_destroy_class;
	}

	ret = nvmf_register_transport(&nvme_fc_transport);	/* [한국어] 이 등록이 있어야 'nvme connect -t fc' 가 이 파일에 도달한다 */
	if (ret)
		goto out_destroy_device;

	return 0;

out_destroy_device:	/* [한국어] 세운 순서의 반대로 되감는다 */
	device_destroy(&fc_class, MKDEV(0, 0));
out_destroy_class:
	class_unregister(&fc_class);

	return ret;
}

/*
 * [한국어]
 * nvme_fc_delete_controllers - 이 포트에 붙은 컨트롤러 전부에 삭제를 건다
 *
 * @rport: 대상 원격 포트
 * @return: 없음
 *
 * 모듈 언로드 경로에서 쓰인다. nvme_delete_ctrl 은 삭제를 예약만 하므로
 * 락을 들고 순회해도 안전하며, 실제 해체는 호출자가 flush_workqueue 로
 * 기다린다.
 *
 * 실행 컨텍스트: exit_module. 잠들지 않는다(예약만 한다).
 *
 * 호출 체인: nvme_fc_exit_module → [이 함수] → nvme_delete_ctrl
 */
static void
nvme_fc_delete_controllers(struct nvme_fc_rport *rport)
{
	struct nvme_fc_ctrl *ctrl;

	spin_lock(&rport->lock);
	list_for_each_entry(ctrl, &rport->ctrl_list, ctrl_list) {
		dev_warn(ctrl->ctrl.device,	/* [한국어] 사용자에게 왜 사라지는지 알린다 — 모듈 언로드는 예상 밖의 단절이다 */
			"NVME-FC{%d}: transport unloading: deleting ctrl\n",
			ctrl->cnum);
		nvme_delete_ctrl(&ctrl->ctrl);	/* [한국어] 예약만 하므로 락 안에서도 안전하다 */
	}
	spin_unlock(&rport->lock);
}

/*
 * [한국어]
 * nvme_fc_exit_module - 모듈을 내리기 전에 살아 있는 컨트롤러를 모두 지운다
 *
 * @return: 없음
 *
 * 모듈이 사라지면 이 파일의 함수 주소도 사라진다. 컨트롤러가 남아 있으면
 * 그 vtable 이 없어진 코드를 가리키게 되므로, 내리기 전에 전부 지워야 한다.
 *
 * 이중 순회로 모든 원격 포트의 컨트롤러에 삭제를 걸고, flush_workqueue 로
 * 그 삭제가 실제로 끝나기를 기다린다. delete_controllers 는 삭제를 예약만
 * 하므로 이 flush 가 없으면 아직 지워지는 중에 모듈이 내려간다.
 *
 * 락을 들고 순회하는데도 안전한 이유: delete_controllers 는 워크를 거는
 * 것뿐이라 잠들지 않는다. 실제 해체는 flush 를 기다리는 동안 워크큐에서
 * 일어나며, 그때는 이미 락을 놓은 뒤다.
 *
 * 실행 컨텍스트: 모듈 해제. flush 로 오래 잠들 수 있다.
 *
 * 호출 체인: module_exit → [이 함수] → nvme_fc_delete_controllers
 */
static void __exit nvme_fc_exit_module(void)
{
	struct nvme_fc_lport *lport;
	struct nvme_fc_rport *rport;
	unsigned long flags;

	spin_lock_irqsave(&nvme_fc_lock, flags);
	list_for_each_entry(lport, &nvme_fc_lport_list, port_list)	/* [한국어] 모든 로컬 포트의 */
		list_for_each_entry(rport, &lport->endp_list, endp_list)	/* [한국어] 모든 원격 포트의 */
			nvme_fc_delete_controllers(rport);	/* [한국어] 컨트롤러에 삭제를 건다 — 예약만 하므로 락 안에서도 안전하다 */
	spin_unlock_irqrestore(&nvme_fc_lock, flags);
	flush_workqueue(nvme_delete_wq);	/* [한국어] 예약한 삭제가 실제로 끝날 때까지 기다린다. 없으면 지워지는 중에 모듈이 내려가 함수 주소가 사라진다 */

	nvmf_unregister_transport(&nvme_fc_transport);	/* [한국어] 새 연결이 더는 이 파일로 오지 않게 한다 */

	device_destroy(&fc_class, MKDEV(0, 0));	/* [한국어] init 의 역순 */
	class_unregister(&fc_class);
}

module_init(nvme_fc_init_module);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */
module_exit(nvme_fc_exit_module);	/* [한국어] NVMe/FC LS·FCP·rport 경로 헬퍼 */

MODULE_DESCRIPTION("NVMe host FC transport driver");	/* [한국어] 모듈 경계·파라미터·심볼 공개 */
MODULE_LICENSE("GPL v2");	/* [한국어] 모듈 경계·파라미터·심볼 공개 */
