// SPDX-License-Identifier: GPL-2.0
/*
 * NVMe over Fabrics TCP host.
 * Copyright (c) 2018 Lightbits Labs. All rights reserved.
 */

/*
 * [한국어 설명] NVMe over TCP 호스트 트랜스포트 (tcp.c)
 *
 * === 파일의 역할 ===
 * NVMe-oF TCP 바이트 스트림 위에서 Admin/I/O 큐를 구현한다. 커널 소켓으로
 * Capsule(SQE)·Data·CQE PDU 를 송수신하고, 선택적 TLS·Digest(CRC32C)·ICReq/ICResp
 * 협상 후 fabrics Connect 로 큐를 성립시킨다. blk-mq queue_rq 가 요청을 큐잉하면
 * io_work 가 소켓에 직렬 송신하고, 수신 경로는 skb 를 파싱해 complete 한다.
 *
 * === 아키텍처 위치 ===
 * 사용자/FS → blk-mq → nvme_tcp_queue_rq → 요청 리스트 → nvme_tcp_io_work
 *   → sock_sendmsg(PDU+data) → 네트워크 → 타깃
 * 수신: sk_data_ready/read_sock → recv 상태기계 → CQE → nvme_complete_rq
 * fabrics.c 가 create_ctrl/옵션 파싱을 담당하고, core.c 가 상태기계·NS 스캔을 담당.
 *
 * === 주요 구조 ===
 * nvme_tcp_ctrl/queue/request — 소켓, send/recv 상태, PDU 스크래치, TLS 키
 *
 * === 전체 아키텍처에서의 위치 ===
 * NVMe/TCP 트랜스포트다. 하드웨어 큐 대신 커널 소켓이 있고, 도어벨 대신 PDU 를
 * 바이트 스트림으로 흘려보낸다. 그래서 다른 트랜스포트에 없는 두 가지 일을 한다 --
 * 스트림에서 PDU 경계를 되찾는 수신 상태 기계와, 부분 전송을 이어 가는 송신 상태 기계다.
 * 호출 체인(제출):
 *   blk-mq → nvme_tcp_queue_rq → 큐의 송신 목록에 걸기
 *     → io_work → nvme_tcp_try_send → try_send_cmd_pdu / try_send_data /
 *       try_send_ddgst → kernel_sendmsg
 * 호출 체인(완료):
 *   소켓 데이터 도착 → data_ready 콜백 → io_work → nvme_tcp_try_recv
 *     → recv_pdu → recv_data / recv_ddgst → nvme_complete_rq
 * 큐마다 io_work 하나가 송신과 수신을 모두 담당하며, 그 직렬화가 곧 큐 단위 잠금
 * 역할을 한다. poll 큐는 인터럽트 대신 nvme_tcp_poll 로 같은 경로를 돈다.
 *
 * === 타 모듈과의 연결 ===
 * - net/ipv4, net/socket.c: 실제 전송 수단. kernel_sendmsg / kernel_recvmsg 와
 *   소켓 콜백(data_ready, write_space, state_change)이 접점이다.
 * - net/tls: TLS 가 켜진 연결에서는 소켓 위에 커널 TLS 가 얹힌다. 핸드셰이크는
 *   유저스페이스 도우미와 협조해 이뤄진다.
 * - drivers/nvme/host/fabrics.c: 연결 옵션 파싱과 Connect 명령. 이 파일은
 *   nvmf_transport_ops 로 "tcp" 이름을 등록한다.
 * - drivers/nvme/host/core.c: nvme_ctrl_ops 를 통해 상태 기계와 Identify 를 위임한다.
 * - crypto: 데이터 다이제스트(ddgst)와 헤더 다이제스트의 CRC32C 계산.
 * 데이터 흐름은 요청 → PDU(헤더+데이터+다이제스트) → 소켓 → 네트워크이고,
 * 반대 방향은 그 역순으로 되짚어 요청을 되찾는다.
 *
 * === 주요 함수/구조체 요약 ===
 * - nvme_tcp_queue_rq: 핫패스 제출 진입점. 명령 PDU 를 준비하고 큐의 송신 목록에
 *   걸어 io_work 를 깨운다. 실제 write 는 여기서 하지 않는다.
 * - nvme_tcp_try_send / _try_send_cmd_pdu / _try_send_data / _try_send_ddgst:
 *   송신 상태 기계. 소켓이 받아 주는 만큼만 보내고 남으면 다음 기회에 이어 간다.
 * - nvme_tcp_try_recv / _recv_pdu / _recv_data / _recv_ddgst: 수신 상태 기계.
 *   스트림에서 PDU 헤더를 모으고, 길이를 읽어 본문 경계를 정하고, 다이제스트를 검증한다.
 * - nvme_tcp_init_connection: 소켓을 연 뒤 ICReq/ICResp 를 주고받아 최대 데이터 길이,
 *   다이제스트 사용 여부 같은 연결 파라미터를 합의한다.
 * - nvme_tcp_setup_ctrl / nvme_tcp_teardown_ctrl: 컨트롤러 전체의 큐 구성과 해체.
 * - nvme_tcp_error_recovery / _error_recovery_work / _reconnect_ctrl_work:
 *   연결이 끊겼을 때의 복구. 큐를 내리고 일정 간격으로 재연결을 시도한다.
 * - nvme_tcp_timeout: 응답 없는 요청 처리. 네트워크 트랜스포트라 abort 대신
 *   오류 복구 경로로 넘긴다.
 * - struct nvme_tcp_queue: 큐 하나의 전부 -- 소켓, io_work, 송수신 상태, 다이제스트
 *   컨텍스트, 송신 대기 목록.
 * - struct nvme_tcp_request: 요청별 전송 상태. 남은 바이트, 현재 스캐터리스트 위치,
 *   PDU 버퍼를 추적한다.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt	/* [한국어] 상수/매크로 — PDU·큐·타임아웃·플래그 */
#include <linux/module.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include <linux/init.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include <linux/slab.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include <linux/err.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include <linux/crc32.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include <linux/nvme-tcp.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include <linux/nvme-keyring.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include <net/sock.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include <net/tcp.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include <net/tls.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include <net/tls_prot.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include <net/handshake.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include <linux/blk-mq.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include <net/busy_poll.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include <trace/events/sock.h>	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */

#include "nvme.h"	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */
#include "fabrics.h"	/* [한국어] 의존 헤더 — 스펙·블록·네트/RDMA/FC API */

struct nvme_tcp_queue;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

/*
 * Define the socket priority to use for connections where it is desirable
 * that the NIC consider performing optimized packet processing or filtering.
 * A non-zero value being sufficient to indicate general consideration of any
 * possible optimization.  Making it a module param allows for alternative
 * values that may be unique for some NIC implementations.
 */
static int so_priority;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
module_param(so_priority, int, 0644);	/* [한국어] 모듈 경계·파라미터·심볼 공개 */
MODULE_PARM_DESC(so_priority, "nvme tcp socket optimize priority");	/* [한국어] 모듈 경계·파라미터·심볼 공개 */

/*
 * Use the unbound workqueue for nvme_tcp_wq, then we can set the cpu affinity
 * from sysfs.
 */
static bool wq_unbound;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
module_param(wq_unbound, bool, 0644);	/* [한국어] 모듈 경계·파라미터·심볼 공개 */
MODULE_PARM_DESC(wq_unbound, "Use unbound workqueue for nvme-tcp IO context (default false)");	/* [한국어] 모듈 경계·파라미터·심볼 공개 */

/*
 * TLS handshake timeout
 */
static int tls_handshake_timeout = 10;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
#ifdef CONFIG_NVME_TCP_TLS	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
module_param(tls_handshake_timeout, int, 0644);	/* [한국어] 모듈 경계·파라미터·심볼 공개 */
MODULE_PARM_DESC(tls_handshake_timeout,	/* [한국어] 모듈 경계·파라미터·심볼 공개 */
		 "nvme TLS handshake timeout in seconds (default 10)");	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
#endif	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static atomic_t nvme_tcp_cpu_queues[NR_CPUS];	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

#ifdef CONFIG_DEBUG_LOCK_ALLOC	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
/* lockdep can detect a circular dependency of the form
 *   sk_lock -> mmap_lock (page fault) -> fs locks -> sk_lock
 * because dependencies are tracked for both nvme-tcp and user contexts. Using
 * a separate class prevents lockdep from conflating nvme-tcp socket use with
 * user-space socket API use.
 */
static struct lock_class_key nvme_tcp_sk_key[2];	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
static struct lock_class_key nvme_tcp_slock_key[2];	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

static void nvme_tcp_reclassify_socket(struct socket *sock)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct sock *sk = sock->sk;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	if (WARN_ON_ONCE(!sock_allow_reclassification(sk)))	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	switch (sk->sk_family) {	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
	case AF_INET:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		sock_lock_init_class_and_name(sk, "slock-AF_INET-NVME",	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
					      &nvme_tcp_slock_key[0],	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
					      "sk_lock-AF_INET-NVME",	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
					      &nvme_tcp_sk_key[0]);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	case AF_INET6:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		sock_lock_init_class_and_name(sk, "slock-AF_INET6-NVME",	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
					      &nvme_tcp_slock_key[1],	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
					      "sk_lock-AF_INET6-NVME",	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
					      &nvme_tcp_sk_key[1]);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	default:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		WARN_ON_ONCE(1);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
#else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
static void nvme_tcp_reclassify_socket(struct socket *sock) { }	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
#endif	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

enum nvme_tcp_send_state {	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	NVME_TCP_SEND_CMD_PDU = 0,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	NVME_TCP_SEND_H2C_PDU,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	NVME_TCP_SEND_DATA,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	NVME_TCP_SEND_DDGST,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

struct nvme_tcp_request {	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct nvme_request	req;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	void			*pdu;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_queue	*queue;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	u32			data_len;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	u32			pdu_len;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	u32			pdu_sent;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	u32			h2cdata_left;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	u32			h2cdata_offset;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	u16			ttag;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	__le16			status;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct list_head	entry;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct llist_node	lentry;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	__le32			ddgst;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	struct bio		*curr_bio;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct iov_iter		iter;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	/* send state */
	size_t			offset;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	size_t			data_sent;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	enum nvme_tcp_send_state state;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

enum nvme_tcp_queue_flags {	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	NVME_TCP_Q_ALLOCATED	= 0,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	NVME_TCP_Q_LIVE		= 1,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	NVME_TCP_Q_POLLING	= 2,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	NVME_TCP_Q_IO_CPU_SET	= 3,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

enum nvme_tcp_recv_state {	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	NVME_TCP_RECV_PDU = 0,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	NVME_TCP_RECV_DATA,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	NVME_TCP_RECV_DDGST,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

struct nvme_tcp_ctrl;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
struct nvme_tcp_queue {	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct socket		*sock;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct work_struct	io_work;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	int			io_cpu;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	struct mutex		queue_lock;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct mutex		send_mutex;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct llist_head	req_list;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct list_head	send_list;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	/* recv state */
	void			*pdu;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int			pdu_remaining;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int			pdu_offset;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	size_t			data_remaining;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	size_t			ddgst_remaining;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	unsigned int		nr_cqe;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/* send state */
	struct nvme_tcp_request *request;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	u32			maxh2cdata;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	size_t			cmnd_capsule_len;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_ctrl	*ctrl;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	unsigned long		flags;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	bool			rd_enabled;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	bool			hdr_digest;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	bool			data_digest;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	bool			tls_enabled;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	u32			rcv_crc;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	u32			snd_crc;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	__le32			exp_ddgst;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	__le32			recv_ddgst;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct completion       tls_complete;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	int                     tls_err;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct page_frag_cache	pf_cache;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	void (*state_change)(struct sock *);	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	void (*data_ready)(struct sock *);	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	void (*write_space)(struct sock *);	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

struct nvme_tcp_ctrl {	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	/* read only in the hot path */
	struct nvme_tcp_queue	*queues;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct blk_mq_tag_set	tag_set;	/* [한국어] blk-mq — 태그·hctx·타임아웃·맵·완료 연동 */

	/* other member variables */
	struct list_head	list;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct blk_mq_tag_set	admin_tag_set;	/* [한국어] blk-mq — 태그·hctx·타임아웃·맵·완료 연동 */
	struct sockaddr_storage addr;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct sockaddr_storage src_addr;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct nvme_ctrl	ctrl;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	struct work_struct	err_work;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct delayed_work	connect_work;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct nvme_tcp_request async_req;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	u32			io_queues[HCTX_MAX_TYPES];	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static LIST_HEAD(nvme_tcp_ctrl_list);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
static DEFINE_MUTEX(nvme_tcp_ctrl_mutex);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
static struct workqueue_struct *nvme_tcp_wq;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
static const struct blk_mq_ops nvme_tcp_mq_ops;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
static const struct blk_mq_ops nvme_tcp_admin_mq_ops;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
static int nvme_tcp_try_send(struct nvme_tcp_queue *queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

static inline struct nvme_tcp_ctrl *to_tcp_ctrl(struct nvme_ctrl *ctrl)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return container_of(ctrl, struct nvme_tcp_ctrl, ctrl);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline int nvme_tcp_queue_id(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return queue - queue->ctrl->queues;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline bool nvme_tcp_recv_pdu_supported(enum nvme_tcp_pdu_type type)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	switch (type) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	case nvme_tcp_c2h_term:	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	case nvme_tcp_c2h_data:	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	case nvme_tcp_r2t:	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	case nvme_tcp_rsp:	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		return true;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	default:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return false;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

/*
 * Check if the queue is TLS encrypted
 */
static inline bool nvme_tcp_queue_tls(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (!IS_ENABLED(CONFIG_NVME_TCP_TLS))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	return queue->tls_enabled;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

/*
 * Check if TLS is configured for the controller.
 */
static inline bool nvme_tcp_tls_configured(struct nvme_ctrl *ctrl)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (!IS_ENABLED(CONFIG_NVME_TCP_TLS))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	return ctrl->opts->tls || ctrl->opts->concat;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline struct blk_mq_tags *nvme_tcp_tagset(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	u32 queue_idx = nvme_tcp_queue_id(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	if (queue_idx == 0)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return queue->ctrl->admin_tag_set.tags[queue_idx];	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	return queue->ctrl->tag_set.tags[queue_idx - 1];	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline u8 nvme_tcp_hdgst_len(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return queue->hdr_digest ? NVME_TCP_DIGEST_LENGTH : 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline u8 nvme_tcp_ddgst_len(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return queue->data_digest ? NVME_TCP_DIGEST_LENGTH : 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline void *nvme_tcp_req_cmd_pdu(struct nvme_tcp_request *req)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return req->pdu;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline void *nvme_tcp_req_data_pdu(struct nvme_tcp_request *req)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	/* use the pdu space in the back for the data pdu */
	return req->pdu + sizeof(struct nvme_tcp_cmd_pdu) -	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		sizeof(struct nvme_tcp_data_pdu);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline size_t nvme_tcp_inline_data_size(struct nvme_tcp_request *req)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (nvme_is_fabrics(req->req.cmd))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return NVME_TCP_ADMIN_CCSZ;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	return req->queue->cmnd_capsule_len - sizeof(struct nvme_command);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline bool nvme_tcp_async_req(struct nvme_tcp_request *req)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return req == &req->queue->ctrl->async_req;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline bool nvme_tcp_has_inline_data(struct nvme_tcp_request *req)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct request *rq;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	if (unlikely(nvme_tcp_async_req(req)))	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		return false; /* async events don't have a request */	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	rq = blk_mq_rq_from_pdu(req);	/* [한국어] blk-mq — 태그·hctx·타임아웃·맵·완료 연동 */

	return rq_data_dir(rq) == WRITE && req->data_len &&	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
		req->data_len <= nvme_tcp_inline_data_size(req);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline struct page *nvme_tcp_req_cur_page(struct nvme_tcp_request *req)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return req->iter.bvec->bv_page;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline size_t nvme_tcp_req_cur_offset(struct nvme_tcp_request *req)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return req->iter.bvec->bv_offset + req->iter.iov_offset;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline size_t nvme_tcp_req_cur_length(struct nvme_tcp_request *req)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return min_t(size_t, iov_iter_single_seg_count(&req->iter),	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
			req->pdu_len - req->pdu_sent);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline size_t nvme_tcp_pdu_data_left(struct nvme_tcp_request *req)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return rq_data_dir(blk_mq_rq_from_pdu(req)) == WRITE ?	/* [한국어] blk-mq — 태그·hctx·타임아웃·맵·완료 연동 */
			req->pdu_len - req->pdu_sent : 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline size_t nvme_tcp_pdu_last_send(struct nvme_tcp_request *req,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		int len)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return nvme_tcp_pdu_data_left(req) <= len;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_tcp_init_iter(struct nvme_tcp_request *req,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		unsigned int dir)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct request *rq = blk_mq_rq_from_pdu(req);	/* [한국어] blk-mq — 태그·hctx·타임아웃·맵·완료 연동 */
	struct bio_vec *vec;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	unsigned int size;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int nr_bvec;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	size_t offset;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (rq->rq_flags & RQF_SPECIAL_PAYLOAD) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		vec = &rq->special_vec;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nr_bvec = 1;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		size = blk_rq_payload_bytes(rq);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		offset = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	} else {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		struct bio *bio = req->curr_bio;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
		struct bvec_iter bi;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
		struct bio_vec bv;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

		vec = __bvec_iter_bvec(bio->bi_io_vec, bio->bi_iter);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nr_bvec = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		bio_for_each_bvec(bv, bio, bi) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nr_bvec++;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		size = bio->bi_iter.bi_size;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		offset = bio->bi_iter.bi_bvec_done;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	iov_iter_bvec(&req->iter, dir, vec, nr_bvec, size);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	req->iter.iov_offset = offset;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline void nvme_tcp_advance_req(struct nvme_tcp_request *req,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		int len)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	req->data_sent += len;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	req->pdu_sent += len;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	iov_iter_advance(&req->iter, len);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (!iov_iter_count(&req->iter) &&	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
	    req->data_sent < req->data_len) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		req->curr_bio = req->curr_bio->bi_next;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_tcp_init_iter(req, ITER_SOURCE);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline void nvme_tcp_send_all(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/* drain the send queue as much as we can... */
	do {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ret = nvme_tcp_try_send(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	} while (ret > 0);	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline bool nvme_tcp_queue_has_pending(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return !list_empty(&queue->send_list) ||	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
		!llist_empty(&queue->req_list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline bool nvme_tcp_queue_more(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return !nvme_tcp_queue_tls(queue) &&	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		nvme_tcp_queue_has_pending(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline void nvme_tcp_queue_request(struct nvme_tcp_request *req,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		bool last)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_queue *queue = req->queue;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	bool empty;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	empty = llist_add(&req->lentry, &queue->req_list) &&	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		list_empty(&queue->send_list) && !queue->request;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/*
	 * if we're the first on the send_list and we can try to send
	 * directly, otherwise queue io_work. Also, only do that if we
	 * are on the same cpu, so we don't introduce contention.
	 */
	if (queue->io_cpu == raw_smp_processor_id() &&	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
	    empty && mutex_trylock(&queue->send_mutex)) {	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
		nvme_tcp_send_all(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		mutex_unlock(&queue->send_mutex);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (last && nvme_tcp_queue_has_pending(queue))	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		queue_work_on(queue->io_cpu, nvme_tcp_wq, &queue->io_work);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_tcp_process_req_list(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_request *req;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct llist_node *node;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	for (node = llist_del_all(&queue->req_list); node; node = node->next) {	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		req = llist_entry(node, struct nvme_tcp_request, lentry);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		list_add(&req->entry, &queue->send_list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline struct nvme_tcp_request *	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
nvme_tcp_fetch_request(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_request *req;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	req = list_first_entry_or_null(&queue->send_list,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			struct nvme_tcp_request, entry);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	if (!req) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_tcp_process_req_list(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		req = list_first_entry_or_null(&queue->send_list,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				struct nvme_tcp_request, entry);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		if (unlikely(!req))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			return NULL;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	list_del_init(&req->entry);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	init_llist_node(&req->lentry);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return req;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

#define NVME_TCP_CRC_SEED (~0)	/* [한국어] 상수/매크로 — PDU·큐·타임아웃·플래그 */

static inline void nvme_tcp_ddgst_update(u32 *crcp,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		struct page *page, size_t off, size_t len)	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	page += off / PAGE_SIZE;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	off %= PAGE_SIZE;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	while (len) {	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		const void *vaddr = kmap_local_page(page);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		size_t n = min(len, (size_t)PAGE_SIZE - off);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		*crcp = crc32c(*crcp, vaddr + off, n);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		kunmap_local(vaddr);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		page++;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		off = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		len -= n;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline __le32 nvme_tcp_ddgst_final(u32 crc)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return cpu_to_le32(~crc);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline __le32 nvme_tcp_hdgst(const void *pdu, size_t len)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return cpu_to_le32(~crc32c(NVME_TCP_CRC_SEED, pdu, len));	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline void nvme_tcp_set_hdgst(void *pdu, size_t len)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	*(__le32 *)(pdu + len) = nvme_tcp_hdgst(pdu, len);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_tcp_verify_hdgst(struct nvme_tcp_queue *queue,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		void *pdu, size_t pdu_len)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_hdr *hdr = pdu;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	__le32 recv_digest;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	__le32 exp_digest;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (unlikely(!(hdr->flags & NVME_TCP_F_HDGST))) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"queue %d: header digest flag is cleared\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvme_tcp_queue_id(queue));	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		return -EPROTO;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	recv_digest = *(__le32 *)(pdu + hdr->hlen);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	exp_digest = nvme_tcp_hdgst(pdu, pdu_len);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	if (recv_digest != exp_digest) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"header digest error: recv %#x expected %#x\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			le32_to_cpu(recv_digest), le32_to_cpu(exp_digest));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return -EIO;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_tcp_check_ddgst(struct nvme_tcp_queue *queue, void *pdu)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_hdr *hdr = pdu;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	u8 digest_len = nvme_tcp_hdgst_len(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	u32 len;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	len = le32_to_cpu(hdr->plen) - hdr->hlen -	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		((hdr->flags & NVME_TCP_F_HDGST) ? digest_len : 0);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (unlikely(len && !(hdr->flags & NVME_TCP_F_DDGST))) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"queue %d: data digest flag is cleared\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_tcp_queue_id(queue));	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		return -EPROTO;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	queue->rcv_crc = NVME_TCP_CRC_SEED;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_tcp_exit_request(struct blk_mq_tag_set *set,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		struct request *rq, unsigned int hctx_idx)	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_request *req = blk_mq_rq_to_pdu(rq);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	page_frag_free(req->pdu);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_tcp_init_request(struct blk_mq_tag_set *set,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		struct request *rq, unsigned int hctx_idx,	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
		unsigned int numa_node)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_ctrl *ctrl = to_tcp_ctrl(set->driver_data);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct nvme_tcp_request *req = blk_mq_rq_to_pdu(rq);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct nvme_tcp_cmd_pdu *pdu;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	int queue_idx = (set == &ctrl->tag_set) ? hctx_idx + 1 : 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_queue *queue = &ctrl->queues[queue_idx];	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	u8 hdgst = nvme_tcp_hdgst_len(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	req->pdu = page_frag_alloc(&queue->pf_cache,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		sizeof(struct nvme_tcp_cmd_pdu) + hdgst,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		GFP_KERNEL | __GFP_ZERO);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (!req->pdu)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return -ENOMEM;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	pdu = req->pdu;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	req->queue = queue;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_req(rq)->ctrl = &ctrl->ctrl;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_req(rq)->cmd = &pdu->cmd;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	init_llist_node(&req->lentry);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	INIT_LIST_HEAD(&req->entry);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_tcp_init_hctx(struct blk_mq_hw_ctx *hctx, void *data,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		unsigned int hctx_idx)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_ctrl *ctrl = to_tcp_ctrl(data);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct nvme_tcp_queue *queue = &ctrl->queues[hctx_idx + 1];	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	hctx->driver_data = queue;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_tcp_init_admin_hctx(struct blk_mq_hw_ctx *hctx, void *data,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		unsigned int hctx_idx)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_ctrl *ctrl = to_tcp_ctrl(data);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct nvme_tcp_queue *queue = &ctrl->queues[0];	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	hctx->driver_data = queue;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static enum nvme_tcp_recv_state	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
nvme_tcp_recv_state(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return  (queue->pdu_remaining) ? NVME_TCP_RECV_PDU :	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
		(queue->ddgst_remaining) ? NVME_TCP_RECV_DDGST :	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		NVME_TCP_RECV_DATA;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_tcp_init_recv_ctx(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	queue->pdu_remaining = sizeof(struct nvme_tcp_rsp_pdu) +	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
				nvme_tcp_hdgst_len(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	queue->pdu_offset = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	queue->data_remaining = -1;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	queue->ddgst_remaining = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_tcp_error_recovery(struct nvme_ctrl *ctrl)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (!nvme_change_ctrl_state(ctrl, NVME_CTRL_RESETTING))	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	dev_warn(ctrl->device, "starting error recovery\n");	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	queue_work(nvme_reset_wq, &to_tcp_ctrl(ctrl)->err_work);	/* [한국어] 워크큐 — 송수신/재연결/복구 비동기 실행 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_tcp_process_nvme_cqe(struct nvme_tcp_queue *queue,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		struct nvme_completion *cqe)	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_request *req;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct request *rq;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	rq = nvme_find_rq(nvme_tcp_tagset(queue), cqe->command_id);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	if (!rq) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"got bad cqe.command_id %#x on queue %d\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			cqe->command_id, nvme_tcp_queue_id(queue));	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		nvme_tcp_error_recovery(&queue->ctrl->ctrl);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		return -EINVAL;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	req = blk_mq_rq_to_pdu(rq);	/* [한국어] blk-mq — 태그·hctx·타임아웃·맵·완료 연동 */
	if (req->status == cpu_to_le16(NVME_SC_SUCCESS))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		req->status = cqe->status;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!nvme_try_complete_req(rq, req->status, cqe->result))	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
		nvme_complete_rq(rq);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
	queue->nr_cqe++;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_tcp_handle_c2h_data(struct nvme_tcp_queue *queue,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		struct nvme_tcp_data_pdu *pdu)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct request *rq;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	rq = nvme_find_rq(nvme_tcp_tagset(queue), pdu->command_id);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	if (!rq) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"got bad c2hdata.command_id %#x on queue %d\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			pdu->command_id, nvme_tcp_queue_id(queue));	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		return -ENOENT;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!blk_rq_payload_bytes(rq)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"queue %d tag %#x unexpected data\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvme_tcp_queue_id(queue), rq->tag);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		return -EIO;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	queue->data_remaining = le32_to_cpu(pdu->data_length);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (pdu->hdr.flags & NVME_TCP_F_DATA_SUCCESS &&	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
	    unlikely(!(pdu->hdr.flags & NVME_TCP_F_DATA_LAST))) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"queue %d tag %#x SUCCESS set but not last PDU\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvme_tcp_queue_id(queue), rq->tag);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		nvme_tcp_error_recovery(&queue->ctrl->ctrl);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		return -EPROTO;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_tcp_handle_comp(struct nvme_tcp_queue *queue,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		struct nvme_tcp_rsp_pdu *pdu)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_completion *cqe = &pdu->cqe;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	int ret = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/*
	 * AEN requests are special as they don't time out and can
	 * survive any kind of queue freeze and often don't respond to
	 * aborts.  We don't even bother to allocate a struct request
	 * for them but rather special case them here.
	 */
	if (unlikely(nvme_is_aen_req(nvme_tcp_queue_id(queue),	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
				     cqe->command_id)))	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_complete_async_event(&queue->ctrl->ctrl, cqe->status,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				&cqe->result);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ret = nvme_tcp_process_nvme_cqe(queue, cqe);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_tcp_setup_h2c_data_pdu(struct nvme_tcp_request *req)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_data_pdu *data = nvme_tcp_req_data_pdu(req);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct nvme_tcp_queue *queue = req->queue;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct request *rq = blk_mq_rq_from_pdu(req);	/* [한국어] blk-mq — 태그·hctx·타임아웃·맵·완료 연동 */
	u32 h2cdata_sent = req->pdu_len;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	u8 hdgst = nvme_tcp_hdgst_len(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	u8 ddgst = nvme_tcp_ddgst_len(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	req->state = NVME_TCP_SEND_H2C_PDU;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	req->offset = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	req->pdu_len = min(req->h2cdata_left, queue->maxh2cdata);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	req->pdu_sent = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	req->h2cdata_left -= req->pdu_len;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	req->h2cdata_offset += h2cdata_sent;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	memset(data, 0, sizeof(*data));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	data->hdr.type = nvme_tcp_h2c_data;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	if (!req->h2cdata_left)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		data->hdr.flags = NVME_TCP_F_DATA_LAST;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (queue->hdr_digest)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		data->hdr.flags |= NVME_TCP_F_HDGST;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (queue->data_digest)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		data->hdr.flags |= NVME_TCP_F_DDGST;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	data->hdr.hlen = sizeof(*data);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	data->hdr.pdo = data->hdr.hlen + hdgst;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	data->hdr.plen =	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		cpu_to_le32(data->hdr.hlen + hdgst + req->pdu_len + ddgst);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	data->ttag = req->ttag;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	data->command_id = nvme_cid(rq);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	data->data_offset = cpu_to_le32(req->h2cdata_offset);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	data->data_length = cpu_to_le32(req->pdu_len);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_tcp_handle_r2t(struct nvme_tcp_queue *queue,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		struct nvme_tcp_r2t_pdu *pdu)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_request *req;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct request *rq;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	u32 r2t_length = le32_to_cpu(pdu->r2t_length);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	u32 r2t_offset = le32_to_cpu(pdu->r2t_offset);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	rq = nvme_find_rq(nvme_tcp_tagset(queue), pdu->command_id);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	if (!rq) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"got bad r2t.command_id %#x on queue %d\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			pdu->command_id, nvme_tcp_queue_id(queue));	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		return -ENOENT;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	req = blk_mq_rq_to_pdu(rq);	/* [한국어] blk-mq — 태그·hctx·타임아웃·맵·완료 연동 */

	if (unlikely(!r2t_length)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"req %d r2t len is %u, probably a bug...\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			rq->tag, r2t_length);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return -EPROTO;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (unlikely(req->data_sent + r2t_length > req->data_len)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"req %d r2t len %u exceeded data len %u (%zu sent)\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			rq->tag, r2t_length, req->data_len, req->data_sent);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return -EPROTO;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (unlikely(r2t_offset < req->data_sent)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"req %d unexpected r2t offset %u (expected %zu)\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			rq->tag, r2t_offset, req->data_sent);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return -EPROTO;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (llist_on_list(&req->lentry) ||	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
	    !list_empty(&req->entry)) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"req %d unexpected r2t while processing request\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			rq->tag);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return -EPROTO;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	req->pdu_len = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	req->h2cdata_left = r2t_length;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	req->h2cdata_offset = r2t_offset;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	req->ttag = pdu->ttag;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	nvme_tcp_setup_h2c_data_pdu(req);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	llist_add(&req->lentry, &queue->req_list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	queue_work_on(queue->io_cpu, nvme_tcp_wq, &queue->io_work);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_tcp_handle_c2h_term(struct nvme_tcp_queue *queue,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		struct nvme_tcp_term_pdu *pdu)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	u16 fes;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	const char *msg;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	u32 plen = le32_to_cpu(pdu->hdr.plen);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	static const char * const msg_table[] = {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		[NVME_TCP_FES_INVALID_PDU_HDR] = "Invalid PDU Header Field",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		[NVME_TCP_FES_PDU_SEQ_ERR] = "PDU Sequence Error",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		[NVME_TCP_FES_HDR_DIGEST_ERR] = "Header Digest Error",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		[NVME_TCP_FES_DATA_OUT_OF_RANGE] = "Data Transfer Out Of Range",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		[NVME_TCP_FES_DATA_LIMIT_EXCEEDED] = "Data Transfer Limit Exceeded",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		[NVME_TCP_FES_UNSUPPORTED_PARAM] = "Unsupported Parameter",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (plen < NVME_TCP_MIN_C2HTERM_PLEN ||	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
	    plen > NVME_TCP_MAX_C2HTERM_PLEN) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"Received a malformed C2HTermReq PDU (plen = %u)\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			plen);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	fes = le16_to_cpu(pdu->fes);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (fes && fes < ARRAY_SIZE(msg_table))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		msg = msg_table[fes];	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		msg = "Unknown";	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		"Received C2HTermReq (FES = %s)\n", msg);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_tcp_recv_pdu(struct nvme_tcp_queue *queue, struct sk_buff *skb,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		unsigned int *offset, size_t *len)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_hdr *hdr;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	char *pdu = queue->pdu;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	size_t rcv_len = min_t(size_t, *len, queue->pdu_remaining);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = skb_copy_bits(skb, *offset,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		&pdu[queue->pdu_offset], rcv_len);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (unlikely(ret))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	queue->pdu_remaining -= rcv_len;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	queue->pdu_offset += rcv_len;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	*offset += rcv_len;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	*len -= rcv_len;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (queue->pdu_remaining)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	hdr = queue->pdu;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (unlikely(hdr->hlen != sizeof(struct nvme_tcp_rsp_pdu))) {	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		if (!nvme_tcp_recv_pdu_supported(hdr->type))	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
			goto unsupported_pdu;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"pdu type %d has unexpected header length (%d)\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			hdr->type, hdr->hlen);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return -EPROTO;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (unlikely(hdr->type == nvme_tcp_c2h_term)) {	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		/*
		 * C2HTermReq never includes Header or Data digests.
		 * Skip the checks.
		 */
		nvme_tcp_handle_c2h_term(queue, (void *)queue->pdu);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		return -EINVAL;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (queue->hdr_digest) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = nvme_tcp_verify_hdgst(queue, queue->pdu, hdr->hlen);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		if (unlikely(ret))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */


	if (queue->data_digest) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = nvme_tcp_check_ddgst(queue, queue->pdu);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		if (unlikely(ret))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	switch (hdr->type) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	case nvme_tcp_c2h_data:	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		return nvme_tcp_handle_c2h_data(queue, (void *)queue->pdu);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	case nvme_tcp_rsp:	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		nvme_tcp_init_recv_ctx(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		return nvme_tcp_handle_comp(queue, (void *)queue->pdu);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	case nvme_tcp_r2t:	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		nvme_tcp_init_recv_ctx(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		return nvme_tcp_handle_r2t(queue, (void *)queue->pdu);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	default:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto unsupported_pdu;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

unsupported_pdu:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		"unsupported pdu type (%d)\n", hdr->type);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return -EINVAL;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline void nvme_tcp_end_request(struct request *rq, u16 status)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	union nvme_result res = {};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!nvme_try_complete_req(rq, cpu_to_le16(status << 1), res))	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
		nvme_complete_rq(rq);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_tcp_recv_data(struct nvme_tcp_queue *queue, struct sk_buff *skb,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
			      unsigned int *offset, size_t *len)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_data_pdu *pdu = (void *)queue->pdu;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct request *rq =	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
		nvme_cid_to_rq(nvme_tcp_tagset(queue), pdu->command_id);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct nvme_tcp_request *req = blk_mq_rq_to_pdu(rq);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	while (true) {	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		int recv_len, ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		recv_len = min_t(size_t, *len, queue->data_remaining);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (!recv_len)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		if (!iov_iter_count(&req->iter)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			req->curr_bio = req->curr_bio->bi_next;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

			/*
			 * If we don't have any bios it means the controller
			 * sent more data than we requested, hence error
			 */
			if (!req->curr_bio) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
				dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
					"queue %d no space in request %#x",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
					nvme_tcp_queue_id(queue), rq->tag);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
				nvme_tcp_init_recv_ctx(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
				return -EIO;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
			}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvme_tcp_init_iter(req, ITER_DEST);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		/* we can read only from what is left in this bio */
		recv_len = min_t(size_t, recv_len,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				iov_iter_count(&req->iter));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		if (queue->data_digest)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			ret = skb_copy_and_crc32c_datagram_iter(skb, *offset,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				&req->iter, recv_len, &queue->rcv_crc);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			ret = skb_copy_datagram_iter(skb, *offset,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
					&req->iter, recv_len);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (ret) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				"queue %d failed to copy request %#x data",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				nvme_tcp_queue_id(queue), rq->tag);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
			return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		*len -= recv_len;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		*offset += recv_len;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		queue->data_remaining -= recv_len;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!queue->data_remaining) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		if (queue->data_digest) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			queue->exp_ddgst = nvme_tcp_ddgst_final(queue->rcv_crc);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
			queue->ddgst_remaining = NVME_TCP_DIGEST_LENGTH;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		} else {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			if (pdu->hdr.flags & NVME_TCP_F_DATA_SUCCESS) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
				nvme_tcp_end_request(rq,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
						le16_to_cpu(req->status));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				queue->nr_cqe++;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvme_tcp_init_recv_ctx(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_tcp_recv_ddgst(struct nvme_tcp_queue *queue,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		struct sk_buff *skb, unsigned int *offset, size_t *len)	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_data_pdu *pdu = (void *)queue->pdu;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	char *ddgst = (char *)&queue->recv_ddgst;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	size_t recv_len = min_t(size_t, *len, queue->ddgst_remaining);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	off_t off = NVME_TCP_DIGEST_LENGTH - queue->ddgst_remaining;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = skb_copy_bits(skb, *offset, &ddgst[off], recv_len);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (unlikely(ret))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	queue->ddgst_remaining -= recv_len;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	*offset += recv_len;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	*len -= recv_len;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (queue->ddgst_remaining)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	if (queue->recv_ddgst != queue->exp_ddgst) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		struct request *rq = nvme_cid_to_rq(nvme_tcp_tagset(queue),	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
					pdu->command_id);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		struct nvme_tcp_request *req = blk_mq_rq_to_pdu(rq);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

		req->status = cpu_to_le16(NVME_SC_DATA_XFER_ERROR);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"data digest error: recv %#x expected %#x\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			le32_to_cpu(queue->recv_ddgst),	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			le32_to_cpu(queue->exp_ddgst));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (pdu->hdr.flags & NVME_TCP_F_DATA_SUCCESS) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		struct request *rq = nvme_cid_to_rq(nvme_tcp_tagset(queue),	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
					pdu->command_id);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		struct nvme_tcp_request *req = blk_mq_rq_to_pdu(rq);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

		nvme_tcp_end_request(rq, le16_to_cpu(req->status));	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		queue->nr_cqe++;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	nvme_tcp_init_recv_ctx(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_tcp_recv_skb(read_descriptor_t *desc, struct sk_buff *skb,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
			     unsigned int offset, size_t len)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_queue *queue = desc->arg.data;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	size_t consumed = len;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int result;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (unlikely(!queue->rd_enabled))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return -EFAULT;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	while (len) {	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		switch (nvme_tcp_recv_state(queue)) {	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		case NVME_TCP_RECV_PDU:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			result = nvme_tcp_recv_pdu(queue, skb, &offset, &len);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
			break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		case NVME_TCP_RECV_DATA:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			result = nvme_tcp_recv_data(queue, skb, &offset, &len);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
			break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		case NVME_TCP_RECV_DDGST:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			result = nvme_tcp_recv_ddgst(queue, skb, &offset, &len);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
			break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		default:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			result = -EFAULT;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (result) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				"receive failed:  %d\n", result);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			queue->rd_enabled = false;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvme_tcp_error_recovery(&queue->ctrl->ctrl);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
			return result;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return consumed;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_tcp_data_ready(struct sock *sk)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_queue *queue;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	trace_sk_data_ready(sk);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	read_lock_bh(&sk->sk_callback_lock);	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
	queue = sk->sk_user_data;	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
	if (likely(queue && queue->rd_enabled) &&	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
	    !test_bit(NVME_TCP_Q_POLLING, &queue->flags))	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		queue_work_on(queue->io_cpu, nvme_tcp_wq, &queue->io_work);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	read_unlock_bh(&sk->sk_callback_lock);	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_tcp_write_space(struct sock *sk)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_queue *queue;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	read_lock_bh(&sk->sk_callback_lock);	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
	queue = sk->sk_user_data;	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
	if (likely(queue && sk_stream_is_writeable(sk))) {	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
		clear_bit(SOCK_NOSPACE, &sk->sk_socket->flags);	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
		/* Ensure pending TLS partial records are retried */
		if (nvme_tcp_queue_tls(queue))	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
			queue->write_space(sk);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		queue_work_on(queue->io_cpu, nvme_tcp_wq, &queue->io_work);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	read_unlock_bh(&sk->sk_callback_lock);	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_tcp_state_change(struct sock *sk)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_queue *queue;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	read_lock_bh(&sk->sk_callback_lock);	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
	queue = sk->sk_user_data;	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
	if (!queue)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto done;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	switch (sk->sk_state) {	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
	case TCP_CLOSE:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	case TCP_CLOSE_WAIT:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	case TCP_LAST_ACK:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	case TCP_FIN_WAIT1:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	case TCP_FIN_WAIT2:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_tcp_error_recovery(&queue->ctrl->ctrl);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	default:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		dev_info(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"queue %d socket state %d\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvme_tcp_queue_id(queue), sk->sk_state);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	queue->state_change(sk);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
done:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	read_unlock_bh(&sk->sk_callback_lock);	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static inline void nvme_tcp_done_send_req(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	queue->request = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_tcp_fail_request(struct nvme_tcp_request *req)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (nvme_tcp_async_req(req)) {	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		union nvme_result res = {};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		nvme_complete_async_event(&req->queue->ctrl->ctrl,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				cpu_to_le16(NVME_SC_HOST_PATH_ERROR), &res);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	} else {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_tcp_end_request(blk_mq_rq_from_pdu(req),	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
				NVME_SC_HOST_PATH_ERROR);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_tcp_try_send_data(struct nvme_tcp_request *req)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_queue *queue = req->queue;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	int req_data_len = req->data_len;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	u32 h2cdata_left = req->h2cdata_left;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	while (true) {	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		struct bio_vec bvec;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
		struct msghdr msg = {	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
			.msg_flags = MSG_DONTWAIT | MSG_SPLICE_PAGES,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		struct page *page = nvme_tcp_req_cur_page(req);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		size_t offset = nvme_tcp_req_cur_offset(req);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		size_t len = nvme_tcp_req_cur_length(req);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		bool last = nvme_tcp_pdu_last_send(req, len);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		int req_data_sent = req->data_sent;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		if (last && !queue->data_digest && !nvme_tcp_queue_more(queue))	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
			msg.msg_flags |= MSG_EOR;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			msg.msg_flags |= MSG_MORE;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		if (!sendpages_ok(page, len, offset))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			msg.msg_flags &= ~MSG_SPLICE_PAGES;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		bvec_set_page(&bvec, page, len, offset);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		iov_iter_bvec(&msg.msg_iter, ITER_SOURCE, &bvec, 1, len);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ret = sock_sendmsg(queue->sock, &msg);	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
		if (ret <= 0)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

		if (queue->data_digest)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			nvme_tcp_ddgst_update(&queue->snd_crc, page,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
					offset, ret);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		/*
		 * update the request iterator except for the last payload send
		 * in the request where we don't want to modify it as we may
		 * compete with the RX path completing the request.
		 */
		if (req_data_sent + ret < req_data_len)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			nvme_tcp_advance_req(req, ret);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

		/* fully successful last send in current PDU */
		if (last && ret == len) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			if (queue->data_digest) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
				req->ddgst =	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
					nvme_tcp_ddgst_final(queue->snd_crc);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
				req->state = NVME_TCP_SEND_DDGST;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				req->offset = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			} else {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				if (h2cdata_left)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
					nvme_tcp_setup_h2c_data_pdu(req);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
				else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
					nvme_tcp_done_send_req(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
			}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			return 1;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return -EAGAIN;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_tcp_try_send_cmd_pdu(struct nvme_tcp_request *req)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_queue *queue = req->queue;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct nvme_tcp_cmd_pdu *pdu = nvme_tcp_req_cmd_pdu(req);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct bio_vec bvec;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct msghdr msg = { .msg_flags = MSG_DONTWAIT | MSG_SPLICE_PAGES, };	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	bool inline_data = nvme_tcp_has_inline_data(req);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	u8 hdgst = nvme_tcp_hdgst_len(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	int len = sizeof(*pdu) + hdgst - req->offset;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (inline_data || nvme_tcp_queue_more(queue))	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		msg.msg_flags |= MSG_MORE;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		msg.msg_flags |= MSG_EOR;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (queue->hdr_digest && !req->offset)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_tcp_set_hdgst(pdu, sizeof(*pdu));	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	bvec_set_virt(&bvec, (void *)pdu + req->offset, len);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	iov_iter_bvec(&msg.msg_iter, ITER_SOURCE, &bvec, 1, len);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ret = sock_sendmsg(queue->sock, &msg);	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
	if (unlikely(ret <= 0))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	len -= ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (!len) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		if (inline_data) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			req->state = NVME_TCP_SEND_DATA;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			if (queue->data_digest)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
				queue->snd_crc = NVME_TCP_CRC_SEED;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		} else {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvme_tcp_done_send_req(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return 1;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	req->offset += ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return -EAGAIN;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_tcp_try_send_data_pdu(struct nvme_tcp_request *req)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_queue *queue = req->queue;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct nvme_tcp_data_pdu *pdu = nvme_tcp_req_data_pdu(req);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct bio_vec bvec;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct msghdr msg = { .msg_flags = MSG_DONTWAIT | MSG_MORE, };	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	u8 hdgst = nvme_tcp_hdgst_len(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	int len = sizeof(*pdu) - req->offset + hdgst;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (queue->hdr_digest && !req->offset)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_tcp_set_hdgst(pdu, sizeof(*pdu));	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	if (!req->h2cdata_left)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		msg.msg_flags |= MSG_SPLICE_PAGES;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	bvec_set_virt(&bvec, (void *)pdu + req->offset, len);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	iov_iter_bvec(&msg.msg_iter, ITER_SOURCE, &bvec, 1, len);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ret = sock_sendmsg(queue->sock, &msg);	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
	if (unlikely(ret <= 0))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	len -= ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (!len) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		req->state = NVME_TCP_SEND_DATA;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (queue->data_digest)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			queue->snd_crc = NVME_TCP_CRC_SEED;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return 1;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	req->offset += ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return -EAGAIN;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_tcp_try_send_ddgst(struct nvme_tcp_request *req)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_queue *queue = req->queue;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	size_t offset = req->offset;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	u32 h2cdata_left = req->h2cdata_left;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct msghdr msg = { .msg_flags = MSG_DONTWAIT };	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct kvec iov = {	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
		.iov_base = (u8 *)&req->ddgst + req->offset,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		.iov_len = NVME_TCP_DIGEST_LENGTH - req->offset	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (nvme_tcp_queue_more(queue))	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		msg.msg_flags |= MSG_MORE;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		msg.msg_flags |= MSG_EOR;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = kernel_sendmsg(queue->sock, &msg, &iov, 1, iov.iov_len);	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
	if (unlikely(ret <= 0))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	if (offset + ret == NVME_TCP_DIGEST_LENGTH) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		if (h2cdata_left)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			nvme_tcp_setup_h2c_data_pdu(req);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvme_tcp_done_send_req(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		return 1;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	req->offset += ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return -EAGAIN;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_tcp_try_send(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_request *req;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	unsigned int noreclaim_flag;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int ret = 1;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!queue->request) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		queue->request = nvme_tcp_fetch_request(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		if (!queue->request)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	req = queue->request;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	noreclaim_flag = memalloc_noreclaim_save();	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (req->state == NVME_TCP_SEND_CMD_PDU) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = nvme_tcp_try_send_cmd_pdu(req);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		if (ret <= 0)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			goto done;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
		if (!nvme_tcp_has_inline_data(req))	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
			goto out;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (req->state == NVME_TCP_SEND_H2C_PDU) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = nvme_tcp_try_send_data_pdu(req);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		if (ret <= 0)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			goto done;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (req->state == NVME_TCP_SEND_DATA) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = nvme_tcp_try_send_data(req);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		if (ret <= 0)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			goto done;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (req->state == NVME_TCP_SEND_DDGST)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = nvme_tcp_try_send_ddgst(req);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
done:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret == -EAGAIN) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	} else if (ret < 0) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"failed to send request %d\n", ret);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_tcp_fail_request(queue->request);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		nvme_tcp_done_send_req(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
out:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	memalloc_noreclaim_restore(noreclaim_flag);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_tcp_try_recv(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct socket *sock = queue->sock;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct sock *sk = sock->sk;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	read_descriptor_t rd_desc;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int consumed;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	rd_desc.arg.data = queue;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	rd_desc.count = 1;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	lock_sock(sk);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	queue->nr_cqe = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	consumed = sock->ops->read_sock(sk, &rd_desc, nvme_tcp_recv_skb);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	release_sock(sk);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return consumed == -EAGAIN ? 0 : consumed;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_tcp_io_work(struct work_struct *w)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_queue *queue =	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		container_of(w, struct nvme_tcp_queue, io_work);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	unsigned long deadline = jiffies + msecs_to_jiffies(1);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	do {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		bool pending = false;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		int result;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		if (mutex_trylock(&queue->send_mutex)) {	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
			result = nvme_tcp_try_send(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
			mutex_unlock(&queue->send_mutex);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
			if (result > 0)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
				pending = true;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			else if (unlikely(result < 0))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
				break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		result = nvme_tcp_try_recv(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		if (result > 0)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			pending = true;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		else if (unlikely(result < 0))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

		/* did we get some space after spending time in recv? */
		if (nvme_tcp_queue_has_pending(queue) &&	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		    sk_stream_is_writeable(queue->sock->sk))	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
			pending = true;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		if (!pending || !queue->rd_enabled)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	} while (!time_after(jiffies, deadline)); /* quota is exhausted */	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */

	queue_work_on(queue->io_cpu, nvme_tcp_wq, &queue->io_work);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_tcp_free_async_req(struct nvme_tcp_ctrl *ctrl)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_request *async = &ctrl->async_req;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	page_frag_free(async->pdu);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_tcp_alloc_async_req(struct nvme_tcp_ctrl *ctrl)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_queue *queue = &ctrl->queues[0];	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct nvme_tcp_request *async = &ctrl->async_req;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	u8 hdgst = nvme_tcp_hdgst_len(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	async->pdu = page_frag_alloc(&queue->pf_cache,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		sizeof(struct nvme_tcp_cmd_pdu) + hdgst,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		GFP_KERNEL | __GFP_ZERO);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (!async->pdu)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return -ENOMEM;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	async->queue = &ctrl->queues[0];	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_tcp_free_queue(struct nvme_ctrl *nctrl, int qid)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_ctrl *ctrl = to_tcp_ctrl(nctrl);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct nvme_tcp_queue *queue = &ctrl->queues[qid];	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	unsigned int noreclaim_flag;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!test_and_clear_bit(NVME_TCP_Q_ALLOCATED, &queue->flags))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	page_frag_cache_drain(&queue->pf_cache);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	noreclaim_flag = memalloc_noreclaim_save();	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	/* ->sock will be released by fput() */
	fput(queue->sock->file);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	queue->sock = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	memalloc_noreclaim_restore(noreclaim_flag);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	kfree(queue->pdu);	/* [한국어] 커널 메모리 생명주기 */
	mutex_destroy(&queue->send_mutex);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
	mutex_destroy(&queue->queue_lock);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_tcp_init_connection(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_icreq_pdu *icreq;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct nvme_tcp_icresp_pdu *icresp;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	char cbuf[CMSG_LEN(sizeof(char))] = {};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	u8 ctype;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct msghdr msg = {};	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct kvec iov;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	bool ctrl_hdgst, ctrl_ddgst;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	u32 maxh2cdata;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	icreq = kzalloc_obj(*icreq);	/* [한국어] 커널 메모리 생명주기 */
	if (!icreq)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return -ENOMEM;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	icresp = kzalloc_obj(*icresp);	/* [한국어] 커널 메모리 생명주기 */
	if (!icresp) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = -ENOMEM;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto free_icreq;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	icreq->hdr.type = nvme_tcp_icreq;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	icreq->hdr.hlen = sizeof(*icreq);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	icreq->hdr.pdo = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	icreq->hdr.plen = cpu_to_le32(icreq->hdr.hlen);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	icreq->pfv = cpu_to_le16(NVME_TCP_PFV_1_0);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	icreq->maxr2t = 0; /* single inflight r2t supported */	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	icreq->hpda = 0; /* no alignment constraint */	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (queue->hdr_digest)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		icreq->digest |= NVME_TCP_HDR_DIGEST_ENABLE;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (queue->data_digest)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		icreq->digest |= NVME_TCP_DATA_DIGEST_ENABLE;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	iov.iov_base = icreq;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	iov.iov_len = sizeof(*icreq);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ret = kernel_sendmsg(queue->sock, &msg, &iov, 1, iov.iov_len);	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
	if (ret < 0) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		pr_warn("queue %d: failed to send icreq, error %d\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvme_tcp_queue_id(queue), ret);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		goto free_icresp;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	memset(&msg, 0, sizeof(msg));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	iov.iov_base = icresp;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	iov.iov_len = sizeof(*icresp);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (nvme_tcp_queue_tls(queue)) {	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		msg.msg_control = cbuf;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		msg.msg_controllen = sizeof(cbuf);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	msg.msg_flags = MSG_WAITALL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ret = kernel_recvmsg(queue->sock, &msg, &iov, 1,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			iov.iov_len, msg.msg_flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret >= 0 && ret < sizeof(*icresp))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = -ECONNRESET;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret < 0) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		pr_warn("queue %d: failed to receive icresp, error %d\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvme_tcp_queue_id(queue), ret);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		goto free_icresp;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ret = -ENOTCONN;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (nvme_tcp_queue_tls(queue)) {	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		ctype = tls_get_record_type(queue->sock->sk,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
					    (struct cmsghdr *)cbuf);	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
		if (ctype != TLS_RECORD_TYPE_DATA) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			pr_err("queue %d: unhandled TLS record %d\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			       nvme_tcp_queue_id(queue), ctype);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
			goto free_icresp;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ret = -EINVAL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (icresp->hdr.type != nvme_tcp_icresp) {	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		pr_err("queue %d: bad type returned %d\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvme_tcp_queue_id(queue), icresp->hdr.type);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		goto free_icresp;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (le32_to_cpu(icresp->hdr.plen) != sizeof(*icresp)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		pr_err("queue %d: bad pdu length returned %d\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvme_tcp_queue_id(queue), icresp->hdr.plen);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		goto free_icresp;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (icresp->pfv != NVME_TCP_PFV_1_0) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		pr_err("queue %d: bad pfv returned %d\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvme_tcp_queue_id(queue), icresp->pfv);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		goto free_icresp;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ctrl_ddgst = !!(icresp->digest & NVME_TCP_DATA_DIGEST_ENABLE);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if ((queue->data_digest && !ctrl_ddgst) ||	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
	    (!queue->data_digest && ctrl_ddgst)) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		pr_err("queue %d: data digest mismatch host: %s ctrl: %s\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvme_tcp_queue_id(queue),	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
			queue->data_digest ? "enabled" : "disabled",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			ctrl_ddgst ? "enabled" : "disabled");	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto free_icresp;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ctrl_hdgst = !!(icresp->digest & NVME_TCP_HDR_DIGEST_ENABLE);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if ((queue->hdr_digest && !ctrl_hdgst) ||	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
	    (!queue->hdr_digest && ctrl_hdgst)) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		pr_err("queue %d: header digest mismatch host: %s ctrl: %s\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvme_tcp_queue_id(queue),	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
			queue->hdr_digest ? "enabled" : "disabled",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			ctrl_hdgst ? "enabled" : "disabled");	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto free_icresp;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (icresp->cpda != 0) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		pr_err("queue %d: unsupported cpda returned %d\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvme_tcp_queue_id(queue), icresp->cpda);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		goto free_icresp;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	maxh2cdata = le32_to_cpu(icresp->maxdata);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if ((maxh2cdata % 4) || (maxh2cdata < NVME_TCP_MIN_MAXH2CDATA)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		pr_err("queue %d: invalid maxh2cdata returned %u\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		       nvme_tcp_queue_id(queue), maxh2cdata);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		goto free_icresp;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	queue->maxh2cdata = maxh2cdata;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
free_icresp:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	kfree(icresp);	/* [한국어] 커널 메모리 생명주기 */
free_icreq:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	kfree(icreq);	/* [한국어] 커널 메모리 생명주기 */
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static bool nvme_tcp_admin_queue(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return nvme_tcp_queue_id(queue) == 0;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static bool nvme_tcp_default_queue(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_ctrl *ctrl = queue->ctrl;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	int qid = nvme_tcp_queue_id(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	return !nvme_tcp_admin_queue(queue) &&	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		qid < 1 + ctrl->io_queues[HCTX_TYPE_DEFAULT];	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static bool nvme_tcp_read_queue(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_ctrl *ctrl = queue->ctrl;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	int qid = nvme_tcp_queue_id(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	return !nvme_tcp_admin_queue(queue) &&	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		!nvme_tcp_default_queue(queue) &&	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		qid < 1 + ctrl->io_queues[HCTX_TYPE_DEFAULT] +	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			  ctrl->io_queues[HCTX_TYPE_READ];	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static bool nvme_tcp_poll_queue(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_ctrl *ctrl = queue->ctrl;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	int qid = nvme_tcp_queue_id(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	return !nvme_tcp_admin_queue(queue) &&	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		!nvme_tcp_default_queue(queue) &&	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		!nvme_tcp_read_queue(queue) &&	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		qid < 1 + ctrl->io_queues[HCTX_TYPE_DEFAULT] +	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			  ctrl->io_queues[HCTX_TYPE_READ] +	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			  ctrl->io_queues[HCTX_TYPE_POLL];	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

/*
 * Track the number of queues assigned to each cpu using a global per-cpu
 * counter and select the least used cpu from the mq_map. Our goal is to spread
 * different controllers I/O threads across different cpu cores.
 *
 * Note that the accounting is not 100% perfect, but we don't need to be, we're
 * simply putting our best effort to select the best candidate cpu core that we
 * find at any given point.
 */
static void nvme_tcp_set_queue_io_cpu(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_ctrl *ctrl = queue->ctrl;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct blk_mq_tag_set *set = &ctrl->tag_set;	/* [한국어] blk-mq — 태그·hctx·타임아웃·맵·완료 연동 */
	int qid = nvme_tcp_queue_id(queue) - 1;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	unsigned int *mq_map = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int cpu, min_queues = INT_MAX, io_cpu;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (wq_unbound)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	if (nvme_tcp_default_queue(queue))	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		mq_map = set->map[HCTX_TYPE_DEFAULT].mq_map;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else if (nvme_tcp_read_queue(queue))	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		mq_map = set->map[HCTX_TYPE_READ].mq_map;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else if (nvme_tcp_poll_queue(queue))	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		mq_map = set->map[HCTX_TYPE_POLL].mq_map;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (WARN_ON(!mq_map))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	/* Search for the least used cpu from the mq_map */
	io_cpu = WORK_CPU_UNBOUND;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	for_each_online_cpu(cpu) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		int num_queues = atomic_read(&nvme_tcp_cpu_queues[cpu]);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

		if (mq_map[cpu] != qid)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			continue;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (num_queues < min_queues) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			io_cpu = cpu;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			min_queues = num_queues;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (io_cpu != WORK_CPU_UNBOUND) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		queue->io_cpu = io_cpu;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		atomic_inc(&nvme_tcp_cpu_queues[io_cpu]);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		set_bit(NVME_TCP_Q_IO_CPU_SET, &queue->flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
out:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	dev_dbg(ctrl->ctrl.device, "queue %d: using cpu %d\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		qid, queue->io_cpu);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_tcp_tls_done(void *data, int status, key_serial_t pskid)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_queue *queue = data;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct nvme_tcp_ctrl *ctrl = queue->ctrl;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	int qid = nvme_tcp_queue_id(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct key *tls_key;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	dev_dbg(ctrl->ctrl.device, "queue %d: TLS handshake done, key %x, status %d\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		qid, pskid, status);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (status) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		queue->tls_err = -status;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_complete;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	tls_key = nvme_tls_key_lookup(pskid);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (IS_ERR(tls_key)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_warn(ctrl->ctrl.device, "queue %d: Invalid key %x\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			 qid, pskid);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		queue->tls_err = -ENOKEY;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	} else {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		queue->tls_enabled = true;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (qid == 0)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			ctrl->ctrl.tls_pskid = key_serial(tls_key);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		key_put(tls_key);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		queue->tls_err = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

out_complete:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	complete(&queue->tls_complete);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_tcp_start_tls(struct nvme_ctrl *nctrl,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
			      struct nvme_tcp_queue *queue,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
			      key_serial_t pskid)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int qid = nvme_tcp_queue_id(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct tls_handshake_args args;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	unsigned long tmo = tls_handshake_timeout * HZ;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	key_serial_t keyring = nvme_keyring_id();	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	dev_dbg(nctrl->device, "queue %d: start TLS with key %x\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		qid, pskid);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	memset(&args, 0, sizeof(args));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	args.ta_sock = queue->sock;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	args.ta_done = nvme_tcp_tls_done;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	args.ta_data = queue;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	args.ta_my_peerids[0] = pskid;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	args.ta_num_peerids = 1;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (nctrl->opts->keyring)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		keyring = key_serial(nctrl->opts->keyring);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	args.ta_keyring = keyring;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	args.ta_timeout_ms = tls_handshake_timeout * 1000;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	queue->tls_err = -EOPNOTSUPP;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	init_completion(&queue->tls_complete);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ret = tls_client_hello_psk(&args, GFP_KERNEL);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(nctrl->device, "queue %d: failed to start TLS: %d\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			qid, ret);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ret = wait_for_completion_interruptible_timeout(&queue->tls_complete, tmo);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret <= 0) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		if (ret == 0)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			ret = -ETIMEDOUT;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		dev_err(nctrl->device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"queue %d: TLS handshake failed, error %d\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			qid, ret);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		tls_handshake_cancel(queue->sock->sk);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	} else {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (queue->tls_err) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			dev_err(nctrl->device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				"queue %d: TLS handshake complete, error %d\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				qid, queue->tls_err);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		} else {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			dev_dbg(nctrl->device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				"queue %d: TLS handshake complete\n", qid);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ret = queue->tls_err;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_tcp_alloc_queue(struct nvme_ctrl *nctrl, int qid,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
				key_serial_t pskid)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_ctrl *ctrl = to_tcp_ctrl(nctrl);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct nvme_tcp_queue *queue = &ctrl->queues[qid];	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	int ret, rcv_pdu_size;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct file *sock_file;	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */

	mutex_init(&queue->queue_lock);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
	queue->ctrl = ctrl;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	init_llist_head(&queue->req_list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	INIT_LIST_HEAD(&queue->send_list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	mutex_init(&queue->send_mutex);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
	INIT_WORK(&queue->io_work, nvme_tcp_io_work);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	if (qid > 0)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		queue->cmnd_capsule_len = nctrl->ioccsz * 16;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		queue->cmnd_capsule_len = sizeof(struct nvme_command) +	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
						NVME_TCP_ADMIN_CCSZ;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = sock_create_kern(current->nsproxy->net_ns,	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
			ctrl->addr.ss_family, SOCK_STREAM,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			IPPROTO_TCP, &queue->sock);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(nctrl->device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"failed to create socket: %d\n", ret);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto err_destroy_mutex;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	sock_file = sock_alloc_file(queue->sock, O_CLOEXEC, NULL);	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
	if (IS_ERR(sock_file)) {	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
		ret = PTR_ERR(sock_file);	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
		goto err_destroy_mutex;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	sk_net_refcnt_upgrade(queue->sock->sk);	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
	nvme_tcp_reclassify_socket(queue->sock);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	/* Single syn retry */
	tcp_sock_set_syncnt(queue->sock->sk, 1);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/* Set TCP no delay */
	tcp_sock_set_nodelay(queue->sock->sk);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/*
	 * Cleanup whatever is sitting in the TCP transmit queue on socket
	 * close. This is done to prevent stale data from being sent should
	 * the network connection be restored before TCP times out.
	 */
	sock_no_linger(queue->sock->sk);	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */

	if (so_priority > 0)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		sock_set_priority(queue->sock->sk, so_priority);	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */

	/* Set socket type of service */
	if (nctrl->opts->tos >= 0)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ip_sock_set_tos(queue->sock->sk, nctrl->opts->tos);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/* Set 10 seconds timeout for icresp recvmsg */
	queue->sock->sk->sk_rcvtimeo = 10 * HZ;	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */

	queue->sock->sk->sk_allocation = GFP_ATOMIC;	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
	queue->sock->sk->sk_use_task_frag = false;	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
	queue->io_cpu = WORK_CPU_UNBOUND;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	queue->request = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	queue->data_remaining = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	queue->ddgst_remaining = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	queue->pdu_remaining = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	queue->pdu_offset = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	sk_set_memalloc(queue->sock->sk);	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */

	if (nctrl->opts->mask & NVMF_OPT_HOST_TRADDR) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = kernel_bind(queue->sock, (struct sockaddr_unsized *)&ctrl->src_addr,	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
			sizeof(ctrl->src_addr));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (ret) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			dev_err(nctrl->device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				"failed to bind queue %d socket %d\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				qid, ret);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			goto err_sock;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (nctrl->opts->mask & NVMF_OPT_HOST_IFACE) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		char *iface = nctrl->opts->host_iface;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		sockptr_t optval = KERNEL_SOCKPTR(iface);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		ret = sock_setsockopt(queue->sock, SOL_SOCKET, SO_BINDTODEVICE,	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
				      optval, strlen(iface));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (ret) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			dev_err(nctrl->device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			  "failed to bind to interface %s queue %d err %d\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			  iface, qid, ret);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			goto err_sock;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	queue->hdr_digest = nctrl->opts->hdr_digest;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	queue->data_digest = nctrl->opts->data_digest;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	rcv_pdu_size = sizeof(struct nvme_tcp_rsp_pdu) +	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
			nvme_tcp_hdgst_len(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	queue->pdu = kmalloc(rcv_pdu_size, GFP_KERNEL);	/* [한국어] 커널 메모리 생명주기 */
	if (!queue->pdu) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = -ENOMEM;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto err_sock;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	dev_dbg(nctrl->device, "connecting queue %d\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvme_tcp_queue_id(queue));	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	ret = kernel_connect(queue->sock, (struct sockaddr_unsized *)&ctrl->addr,	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
		sizeof(ctrl->addr), 0);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(nctrl->device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"failed to connect socket: %d\n", ret);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto err_rcv_pdu;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/* If PSKs are configured try to start TLS */
	if (nvme_tcp_tls_configured(nctrl) && pskid) {	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		ret = nvme_tcp_start_tls(nctrl, queue, pskid);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			goto err_init_connect;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = nvme_tcp_init_connection(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto err_init_connect;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	set_bit(NVME_TCP_Q_ALLOCATED, &queue->flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

err_init_connect:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	kernel_sock_shutdown(queue->sock, SHUT_RDWR);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
err_rcv_pdu:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	kfree(queue->pdu);	/* [한국어] 커널 메모리 생명주기 */
err_sock:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	/* ->sock will be released by fput() */
	fput(queue->sock->file);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	queue->sock = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
err_destroy_mutex:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	mutex_destroy(&queue->send_mutex);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
	mutex_destroy(&queue->queue_lock);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_tcp_restore_sock_ops(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct socket *sock = queue->sock;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	write_lock_bh(&sock->sk->sk_callback_lock);	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
	sock->sk->sk_user_data  = NULL;	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
	sock->sk->sk_data_ready = queue->data_ready;	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
	sock->sk->sk_state_change = queue->state_change;	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
	sock->sk->sk_write_space  = queue->write_space;	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
	write_unlock_bh(&sock->sk->sk_callback_lock);	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void __nvme_tcp_stop_queue(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	kernel_sock_shutdown(queue->sock, SHUT_RDWR);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_tcp_restore_sock_ops(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	cancel_work_sync(&queue->io_work);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_tcp_stop_queue_nowait(struct nvme_ctrl *nctrl, int qid)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_ctrl *ctrl = to_tcp_ctrl(nctrl);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct nvme_tcp_queue *queue = &ctrl->queues[qid];	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	if (!test_bit(NVME_TCP_Q_ALLOCATED, &queue->flags))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	if (test_and_clear_bit(NVME_TCP_Q_IO_CPU_SET, &queue->flags))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		atomic_dec(&nvme_tcp_cpu_queues[queue->io_cpu]);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	mutex_lock(&queue->queue_lock);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
	if (test_and_clear_bit(NVME_TCP_Q_LIVE, &queue->flags))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		__nvme_tcp_stop_queue(queue);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	/* Stopping the queue will disable TLS */
	queue->tls_enabled = false;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	mutex_unlock(&queue->queue_lock);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_tcp_wait_queue(struct nvme_ctrl *nctrl, int qid)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_ctrl *ctrl = to_tcp_ctrl(nctrl);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct nvme_tcp_queue *queue = &ctrl->queues[qid];	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	int timeout = 100;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	while (timeout > 0) {	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		if (!test_bit(NVME_TCP_Q_ALLOCATED, &queue->flags) ||	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		    !sk_wmem_alloc_get(queue->sock->sk))	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
			return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
		msleep(2);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		timeout -= 2;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	dev_warn(nctrl->device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		 "qid %d: timeout draining sock wmem allocation expired\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		 qid);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_tcp_stop_queue(struct nvme_ctrl *nctrl, int qid)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_tcp_stop_queue_nowait(nctrl, qid);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	nvme_tcp_wait_queue(nctrl, qid);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */


static void nvme_tcp_setup_sock_ops(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	write_lock_bh(&queue->sock->sk->sk_callback_lock);	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
	queue->sock->sk->sk_user_data = queue;	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
	queue->state_change = queue->sock->sk->sk_state_change;	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
	queue->data_ready = queue->sock->sk->sk_data_ready;	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
	queue->write_space = queue->sock->sk->sk_write_space;	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
	queue->sock->sk->sk_data_ready = nvme_tcp_data_ready;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	queue->sock->sk->sk_state_change = nvme_tcp_state_change;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	queue->sock->sk->sk_write_space = nvme_tcp_write_space;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
#ifdef CONFIG_NET_RX_BUSY_POLL	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	queue->sock->sk->sk_ll_usec = 1;	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
#endif	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	write_unlock_bh(&queue->sock->sk->sk_callback_lock);	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_tcp_start_queue(struct nvme_ctrl *nctrl, int idx)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_ctrl *ctrl = to_tcp_ctrl(nctrl);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct nvme_tcp_queue *queue = &ctrl->queues[idx];	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	queue->rd_enabled = true;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_tcp_init_recv_ctx(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	nvme_tcp_setup_sock_ops(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	if (idx) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_tcp_set_queue_io_cpu(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		ret = nvmf_connect_io_queue(nctrl, idx);	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
	} else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ret = nvmf_connect_admin_queue(nctrl);	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */

	if (!ret) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		set_bit(NVME_TCP_Q_LIVE, &queue->flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	} else {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (test_bit(NVME_TCP_Q_ALLOCATED, &queue->flags))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			__nvme_tcp_stop_queue(queue);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		dev_err(nctrl->device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"failed to connect queue: %d ret=%d\n", idx, ret);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_tcp_free_admin_queue(struct nvme_ctrl *ctrl)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (to_tcp_ctrl(ctrl)->async_req.pdu) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		cancel_work_sync(&ctrl->async_event_work);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_tcp_free_async_req(to_tcp_ctrl(ctrl));	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		to_tcp_ctrl(ctrl)->async_req.pdu = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	nvme_tcp_free_queue(ctrl, 0);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_tcp_free_io_queues(struct nvme_ctrl *ctrl)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int i;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	for (i = 1; i < ctrl->queue_count; i++)	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		nvme_tcp_free_queue(ctrl, i);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_tcp_stop_io_queues(struct nvme_ctrl *ctrl)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int i;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	for (i = 1; i < ctrl->queue_count; i++)	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		nvme_tcp_stop_queue_nowait(ctrl, i);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	for (i = 1; i < ctrl->queue_count; i++)	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		nvme_tcp_wait_queue(ctrl, i);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_tcp_start_io_queues(struct nvme_ctrl *ctrl,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
				    int first, int last)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int i, ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	for (i = first; i < last; i++) {	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		ret = nvme_tcp_start_queue(ctrl, i);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			goto out_stop_queues;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

out_stop_queues:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	for (i--; i >= first; i--)	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		nvme_tcp_stop_queue(ctrl, i);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_tcp_alloc_admin_queue(struct nvme_ctrl *ctrl)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	key_serial_t pskid = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (nvme_tcp_tls_configured(ctrl)) {	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		if (ctrl->opts->tls_key)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			pskid = key_serial(ctrl->opts->tls_key);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		else if (ctrl->opts->tls) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			pskid = nvme_tls_psk_default(ctrl->opts->keyring,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
						      ctrl->opts->host->nqn,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
						      ctrl->opts->subsysnqn);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			if (!pskid) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
				dev_err(ctrl->device, "no valid PSK found\n");	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				return -ENOKEY;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
			}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = nvme_tcp_alloc_queue(ctrl, 0, pskid);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	ret = nvme_tcp_alloc_async_req(to_tcp_ctrl(ctrl));	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_free_queue;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

out_free_queue:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_tcp_free_queue(ctrl, 0);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int __nvme_tcp_alloc_io_queues(struct nvme_ctrl *ctrl)	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int i, ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (nvme_tcp_tls_configured(ctrl)) {	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		if (ctrl->opts->concat) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			/*
			 * The generated PSK is stored in the
			 * fabric options
			 */
			if (!ctrl->opts->tls_key) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
				dev_err(ctrl->device, "no PSK generated\n");	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				return -ENOKEY;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
			}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			if (ctrl->tls_pskid &&	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			    ctrl->tls_pskid != key_serial(ctrl->opts->tls_key)) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				dev_err(ctrl->device, "Stale PSK id %08x\n", ctrl->tls_pskid);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				ctrl->tls_pskid = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		} else if (!ctrl->tls_pskid) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			dev_err(ctrl->device, "no PSK negotiated\n");	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			return -ENOKEY;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	for (i = 1; i < ctrl->queue_count; i++) {	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		ret = nvme_tcp_alloc_queue(ctrl, i,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
				ctrl->tls_pskid);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			goto out_free_queues;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

out_free_queues:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	for (i--; i >= 1; i--)	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		nvme_tcp_free_queue(ctrl, i);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_tcp_alloc_io_queues(struct nvme_ctrl *ctrl)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	unsigned int nr_io_queues;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	nr_io_queues = nvmf_nr_io_queues(ctrl->opts);	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
	ret = nvme_set_queue_count(ctrl, &nr_io_queues);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	if (nr_io_queues == 0) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(ctrl->device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"unable to set any I/O queues\n");	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return -ENOMEM;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ctrl->queue_count = nr_io_queues + 1;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	dev_info(ctrl->device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		"creating %d I/O queues.\n", nr_io_queues);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	nvmf_set_io_queues(ctrl->opts, nr_io_queues,	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
			   to_tcp_ctrl(ctrl)->io_queues);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return __nvme_tcp_alloc_io_queues(ctrl);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_tcp_configure_io_queues(struct nvme_ctrl *ctrl, bool new)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int ret, nr_queues;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = nvme_tcp_alloc_io_queues(ctrl);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	if (new) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = nvme_alloc_io_tag_set(ctrl, &to_tcp_ctrl(ctrl)->tag_set,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				&nvme_tcp_mq_ops,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
				ctrl->opts->nr_poll_queues ? HCTX_MAX_TYPES : 2,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				sizeof(struct nvme_tcp_request));	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			goto out_free_io_queues;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/*
	 * Only start IO queues for which we have allocated the tagset
	 * and limited it to the available queues. On reconnects, the
	 * queue number might have changed.
	 */
	nr_queues = min(ctrl->tagset->nr_hw_queues + 1, ctrl->queue_count);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ret = nvme_tcp_start_io_queues(ctrl, 1, nr_queues);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_cleanup_connect_q;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	if (!new) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_start_freeze(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_unquiesce_io_queues(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (!nvme_wait_freeze_timeout(ctrl, NVME_IO_TIMEOUT)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			/*
			 * If we timed out waiting for freeze we are likely to
			 * be stuck.  Fail the controller initialization just
			 * to be safe.
			 */
			ret = -ENODEV;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvme_unfreeze(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			goto out_wait_freeze_timed_out;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		blk_mq_update_nr_hw_queues(ctrl->tagset,	/* [한국어] blk-mq — 태그·hctx·타임아웃·맵·완료 연동 */
			ctrl->queue_count - 1);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_unfreeze(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/*
	 * If the number of queues has increased (reconnect case)
	 * start all new queues now.
	 */
	ret = nvme_tcp_start_io_queues(ctrl, nr_queues,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
				       ctrl->tagset->nr_hw_queues + 1);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_wait_freeze_timed_out;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

out_wait_freeze_timed_out:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_quiesce_io_queues(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_sync_io_queues(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_tcp_stop_io_queues(ctrl);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
out_cleanup_connect_q:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_cancel_tagset(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (new)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_remove_io_tag_set(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
out_free_io_queues:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_tcp_free_io_queues(ctrl);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_tcp_configure_admin_queue(struct nvme_ctrl *ctrl, bool new)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int error;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	error = nvme_tcp_alloc_admin_queue(ctrl);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	if (error)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return error;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	if (new) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		error = nvme_alloc_admin_tag_set(ctrl,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				&to_tcp_ctrl(ctrl)->admin_tag_set,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				&nvme_tcp_admin_mq_ops,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
				sizeof(struct nvme_tcp_request));	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		if (error)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			goto out_free_queue;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	error = nvme_tcp_start_queue(ctrl, 0);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	if (error)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_cleanup_tagset;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	if (ctrl->opts->concat && !ctrl->tls_pskid)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	error = nvme_enable_ctrl(ctrl);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
	if (error)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_stop_queue;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	nvme_unquiesce_admin_queue(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	error = nvme_init_ctrl_finish(ctrl, false);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
	if (error)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_quiesce_queue;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

out_quiesce_queue:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_quiesce_admin_queue(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	blk_sync_queue(ctrl->admin_q);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
out_stop_queue:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_tcp_stop_queue(ctrl, 0);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	nvme_cancel_admin_tagset(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
out_cleanup_tagset:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (new)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_remove_admin_tag_set(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
out_free_queue:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_tcp_free_admin_queue(ctrl);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	return error;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_tcp_teardown_admin_queue(struct nvme_ctrl *ctrl,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		bool remove)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_quiesce_admin_queue(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	blk_sync_queue(ctrl->admin_q);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_tcp_stop_queue(ctrl, 0);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	nvme_cancel_admin_tagset(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (remove) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_unquiesce_admin_queue(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_remove_admin_tag_set(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_tcp_free_admin_queue(ctrl);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	if (ctrl->tls_pskid) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_dbg(ctrl->device, "Wipe negotiated TLS_PSK %08x\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			ctrl->tls_pskid);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ctrl->tls_pskid = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_tcp_teardown_io_queues(struct nvme_ctrl *ctrl,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		bool remove)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ctrl->queue_count <= 1)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	nvme_quiesce_io_queues(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_sync_io_queues(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_tcp_stop_io_queues(ctrl);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	nvme_cancel_tagset(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (remove) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_unquiesce_io_queues(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_remove_io_tag_set(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_tcp_free_io_queues(ctrl);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_tcp_reconnect_or_remove(struct nvme_ctrl *ctrl,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		int status)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	enum nvme_ctrl_state state = nvme_ctrl_state(ctrl);	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	/* If we are resetting/deleting then do nothing */
	if (state != NVME_CTRL_CONNECTING) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		WARN_ON_ONCE(state == NVME_CTRL_NEW || state == NVME_CTRL_LIVE);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (nvmf_should_reconnect(ctrl, status)) {	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
		dev_info(ctrl->device, "Reconnecting in %d seconds...\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			ctrl->opts->reconnect_delay);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		queue_delayed_work(nvme_wq, &to_tcp_ctrl(ctrl)->connect_work,	/* [한국어] 워크큐 — 송수신/재연결/복구 비동기 실행 */
				ctrl->opts->reconnect_delay * HZ);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	} else {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		dev_info(ctrl->device, "Removing controller (%d)...\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			 status);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_delete_ctrl(ctrl);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

/*
 * The TLS key is set by secure concatenation after negotiation has been
 * completed on the admin queue. We need to revoke the key when:
 * - concatenation is enabled (otherwise it's a static key set by the user)
 * and
 * - the generated key is present in ctrl->tls_key (otherwise there's nothing
 *   to revoke)
 * and
 * - a valid PSK key ID has been set in ctrl->tls_pskid (otherwise TLS
 *   negotiation has not run).
 *
 * We cannot always revoke the key as nvme_tcp_alloc_admin_queue() is called
 * twice during secure concatenation, once on a 'normal' connection to run the
 * DH-HMAC-CHAP negotiation (which generates the key, so it _must not_ be set),
 * and once after the negotiation (which uses the key, so it _must_ be set).
 */
static bool nvme_tcp_key_revoke_needed(struct nvme_ctrl *ctrl)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return ctrl->opts->concat && ctrl->opts->tls_key && ctrl->tls_pskid;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_tcp_setup_ctrl(struct nvme_ctrl *ctrl, bool new)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvmf_ctrl_options *opts = ctrl->opts;	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = nvme_tcp_configure_admin_queue(ctrl, new);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	if (ctrl->opts->concat && !ctrl->tls_pskid) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		/* See comments for nvme_tcp_key_revoke_needed() */
		dev_dbg(ctrl->device, "restart admin queue for secure concatenation\n");	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_stop_keep_alive(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_tcp_teardown_admin_queue(ctrl, false);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		ret = nvme_tcp_configure_admin_queue(ctrl, false);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			goto destroy_admin;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (ctrl->icdoff) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = -EOPNOTSUPP;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		dev_err(ctrl->device, "icdoff is not supported!\n");	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto destroy_admin;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!nvme_ctrl_sgl_supported(ctrl)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = -EOPNOTSUPP;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		dev_err(ctrl->device, "Mandatory sgls are not supported!\n");	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto destroy_admin;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (opts->queue_size > ctrl->sqsize + 1)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_warn(ctrl->device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"queue_size %zu > ctrl sqsize %u, clamping down\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			opts->queue_size, ctrl->sqsize + 1);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (ctrl->sqsize + 1 > ctrl->maxcmd) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_warn(ctrl->device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"sqsize %u > ctrl maxcmd %u, clamping down\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			ctrl->sqsize + 1, ctrl->maxcmd);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ctrl->sqsize = ctrl->maxcmd - 1;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (ctrl->queue_count > 1) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = nvme_tcp_configure_io_queues(ctrl, new);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			goto destroy_admin;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!nvme_change_ctrl_state(ctrl, NVME_CTRL_LIVE)) {	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
		/*
		 * state change failure is ok if we started ctrl delete,
		 * unless we're during creation of a new controller to
		 * avoid races with teardown flow.
		 */
		enum nvme_ctrl_state state = nvme_ctrl_state(ctrl);	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

		WARN_ON_ONCE(state != NVME_CTRL_DELETING &&	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			     state != NVME_CTRL_DELETING_NOIO);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		WARN_ON_ONCE(new);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ret = -EINVAL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto destroy_io;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	nvme_start_ctrl(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

destroy_io:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ctrl->queue_count > 1) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_quiesce_io_queues(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_sync_io_queues(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_tcp_stop_io_queues(ctrl);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		nvme_cancel_tagset(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		if (new)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			nvme_remove_io_tag_set(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_tcp_free_io_queues(ctrl);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
destroy_admin:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_stop_keep_alive(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_tcp_teardown_admin_queue(ctrl, new);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_tcp_reconnect_ctrl_work(struct work_struct *work)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_ctrl *tcp_ctrl = container_of(to_delayed_work(work),	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
			struct nvme_tcp_ctrl, connect_work);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct nvme_ctrl *ctrl = &tcp_ctrl->ctrl;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	++ctrl->nr_reconnects;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = nvme_tcp_setup_ctrl(ctrl, false);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto requeue;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	dev_info(ctrl->device, "Successfully reconnected (attempt %d/%d)\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		 ctrl->nr_reconnects, ctrl->opts->max_reconnects);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ctrl->nr_reconnects = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

requeue:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	dev_info(ctrl->device, "Failed reconnect attempt %d/%d\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		 ctrl->nr_reconnects, ctrl->opts->max_reconnects);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_tcp_reconnect_or_remove(ctrl, ret);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_tcp_error_recovery_work(struct work_struct *work)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_ctrl *tcp_ctrl = container_of(work,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
				struct nvme_tcp_ctrl, err_work);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct nvme_ctrl *ctrl = &tcp_ctrl->ctrl;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	if (nvme_tcp_key_revoke_needed(ctrl))	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		nvme_auth_revoke_tls_key(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_stop_keep_alive(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	flush_work(&ctrl->async_event_work);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_tcp_teardown_io_queues(ctrl, false);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	/* unquiesce to fail fast pending requests */
	nvme_unquiesce_io_queues(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_tcp_teardown_admin_queue(ctrl, false);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	nvme_unquiesce_admin_queue(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_auth_stop(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!nvme_change_ctrl_state(ctrl, NVME_CTRL_CONNECTING)) {	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
		/* state change failure is ok if we started ctrl delete */
		enum nvme_ctrl_state state = nvme_ctrl_state(ctrl);	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

		WARN_ON_ONCE(state != NVME_CTRL_DELETING &&	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			     state != NVME_CTRL_DELETING_NOIO);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	nvme_tcp_reconnect_or_remove(ctrl, 0);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_tcp_teardown_ctrl(struct nvme_ctrl *ctrl, bool shutdown)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_tcp_teardown_io_queues(ctrl, shutdown);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	nvme_quiesce_admin_queue(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_disable_ctrl(ctrl, shutdown);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
	nvme_tcp_teardown_admin_queue(ctrl, shutdown);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_tcp_delete_ctrl(struct nvme_ctrl *ctrl)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_tcp_teardown_ctrl(ctrl, true);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_reset_ctrl_work(struct work_struct *work)	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_ctrl *ctrl =	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
		container_of(work, struct nvme_ctrl, reset_work);	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (nvme_tcp_key_revoke_needed(ctrl))	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		nvme_auth_revoke_tls_key(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_stop_ctrl(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_tcp_teardown_ctrl(ctrl, false);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	if (!nvme_change_ctrl_state(ctrl, NVME_CTRL_CONNECTING)) {	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
		/* state change failure is ok if we started ctrl delete */
		enum nvme_ctrl_state state = nvme_ctrl_state(ctrl);	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

		WARN_ON_ONCE(state != NVME_CTRL_DELETING &&	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			     state != NVME_CTRL_DELETING_NOIO);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = nvme_tcp_setup_ctrl(ctrl, false);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_fail;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

out_fail:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	++ctrl->nr_reconnects;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_tcp_reconnect_or_remove(ctrl, ret);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_tcp_stop_ctrl(struct nvme_ctrl *ctrl)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	flush_work(&to_tcp_ctrl(ctrl)->err_work);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	cancel_delayed_work_sync(&to_tcp_ctrl(ctrl)->connect_work);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_tcp_free_ctrl(struct nvme_ctrl *nctrl)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_ctrl *ctrl = to_tcp_ctrl(nctrl);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	if (list_empty(&ctrl->list))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto free_ctrl;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	mutex_lock(&nvme_tcp_ctrl_mutex);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	list_del(&ctrl->list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	mutex_unlock(&nvme_tcp_ctrl_mutex);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	nvmf_free_options(nctrl->opts);	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
free_ctrl:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	kfree(ctrl->queues);	/* [한국어] 커널 메모리 생명주기 */
	kfree(ctrl);	/* [한국어] 커널 메모리 생명주기 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_tcp_set_sg_null(struct nvme_command *c)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_sgl_desc *sg = &c->common.dptr.sgl;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	sg->addr = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	sg->length = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	sg->type = (NVME_TRANSPORT_SGL_DATA_DESC << 4) |	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			NVME_SGL_FMT_TRANSPORT_A;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_tcp_set_sg_inline(struct nvme_tcp_queue *queue,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		struct nvme_command *c, u32 data_len)	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_sgl_desc *sg = &c->common.dptr.sgl;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	sg->addr = cpu_to_le64(queue->ctrl->ctrl.icdoff);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	sg->length = cpu_to_le32(data_len);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	sg->type = (NVME_SGL_FMT_DATA_DESC << 4) | NVME_SGL_FMT_OFFSET;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_tcp_set_sg_host_data(struct nvme_command *c,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		u32 data_len)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_sgl_desc *sg = &c->common.dptr.sgl;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	sg->addr = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	sg->length = cpu_to_le32(data_len);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	sg->type = (NVME_TRANSPORT_SGL_DATA_DESC << 4) |	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			NVME_SGL_FMT_TRANSPORT_A;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_tcp_submit_async_event(struct nvme_ctrl *arg)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_ctrl *ctrl = to_tcp_ctrl(arg);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct nvme_tcp_queue *queue = &ctrl->queues[0];	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct nvme_tcp_cmd_pdu *pdu = ctrl->async_req.pdu;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct nvme_command *cmd = &pdu->cmd;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	u8 hdgst = nvme_tcp_hdgst_len(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	memset(pdu, 0, sizeof(*pdu));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	pdu->hdr.type = nvme_tcp_cmd;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	if (queue->hdr_digest)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		pdu->hdr.flags |= NVME_TCP_F_HDGST;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	pdu->hdr.hlen = sizeof(*pdu);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	pdu->hdr.plen = cpu_to_le32(pdu->hdr.hlen + hdgst);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	cmd->common.opcode = nvme_admin_async_event;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	cmd->common.command_id = NVME_AQ_BLK_MQ_DEPTH;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	cmd->common.flags |= NVME_CMD_SGL_METABUF;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_tcp_set_sg_null(cmd);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	ctrl->async_req.state = NVME_TCP_SEND_CMD_PDU;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ctrl->async_req.offset = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ctrl->async_req.curr_bio = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ctrl->async_req.data_len = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	init_llist_node(&ctrl->async_req.lentry);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	INIT_LIST_HEAD(&ctrl->async_req.entry);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	nvme_tcp_queue_request(&ctrl->async_req, true);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_tcp_complete_timed_out(struct request *rq)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_request *req = blk_mq_rq_to_pdu(rq);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct nvme_ctrl *ctrl = &req->queue->ctrl->ctrl;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	nvme_tcp_stop_queue(ctrl, nvme_tcp_queue_id(req->queue));	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	nvmf_complete_timed_out_request(rq);	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static enum blk_eh_timer_return nvme_tcp_timeout(struct request *rq)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_request *req = blk_mq_rq_to_pdu(rq);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct nvme_ctrl *ctrl = &req->queue->ctrl->ctrl;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct nvme_tcp_cmd_pdu *pdu = nvme_tcp_req_cmd_pdu(req);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct nvme_command *cmd = &pdu->cmd;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	int qid = nvme_tcp_queue_id(req->queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	dev_warn(ctrl->device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		 "I/O tag %d (%04x) type %d opcode %#x (%s) QID %d timeout\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		 rq->tag, nvme_cid(rq), pdu->hdr.type, cmd->common.opcode,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		 nvme_fabrics_opcode_str(qid, cmd), qid);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (nvme_ctrl_state(ctrl) != NVME_CTRL_LIVE) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
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
		nvme_tcp_complete_timed_out(rq);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		return BLK_EH_DONE;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/*
	 * LIVE state should trigger the normal error recovery which will
	 * handle completing this request.
	 */
	nvme_tcp_error_recovery(ctrl);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	return BLK_EH_RESET_TIMER;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static blk_status_t nvme_tcp_map_data(struct nvme_tcp_queue *queue,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
			struct request *rq)	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_request *req = blk_mq_rq_to_pdu(rq);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct nvme_tcp_cmd_pdu *pdu = nvme_tcp_req_cmd_pdu(req);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct nvme_command *c = &pdu->cmd;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	c->common.flags |= NVME_CMD_SGL_METABUF;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!blk_rq_nr_phys_segments(rq))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_tcp_set_sg_null(c);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	else if (rq_data_dir(rq) == WRITE &&	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
	    req->data_len <= nvme_tcp_inline_data_size(req))	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		nvme_tcp_set_sg_inline(queue, c, req->data_len);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_tcp_set_sg_host_data(c, req->data_len);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static blk_status_t nvme_tcp_setup_cmd_pdu(struct nvme_ns *ns,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		struct request *rq)	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_request *req = blk_mq_rq_to_pdu(rq);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct nvme_tcp_cmd_pdu *pdu = nvme_tcp_req_cmd_pdu(req);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct nvme_tcp_queue *queue = req->queue;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	u8 hdgst = nvme_tcp_hdgst_len(queue), ddgst = 0;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	blk_status_t ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = nvme_setup_cmd(ns, rq);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	req->state = NVME_TCP_SEND_CMD_PDU;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	req->status = cpu_to_le16(NVME_SC_SUCCESS);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	req->offset = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	req->data_sent = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	req->pdu_len = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	req->pdu_sent = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	req->h2cdata_left = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	req->data_len = blk_rq_nr_phys_segments(rq) ?	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				blk_rq_payload_bytes(rq) : 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	req->curr_bio = rq->bio;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (req->curr_bio && req->data_len)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_tcp_init_iter(req, rq_data_dir(rq));	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	if (rq_data_dir(rq) == WRITE &&	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
	    req->data_len <= nvme_tcp_inline_data_size(req))	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		req->pdu_len = req->data_len;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	pdu->hdr.type = nvme_tcp_cmd;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	pdu->hdr.flags = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (queue->hdr_digest)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		pdu->hdr.flags |= NVME_TCP_F_HDGST;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (queue->data_digest && req->pdu_len) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		pdu->hdr.flags |= NVME_TCP_F_DDGST;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ddgst = nvme_tcp_ddgst_len(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	pdu->hdr.hlen = sizeof(*pdu);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	pdu->hdr.pdo = req->pdu_len ? pdu->hdr.hlen + hdgst : 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	pdu->hdr.plen =	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		cpu_to_le32(pdu->hdr.hlen + hdgst + req->pdu_len + ddgst);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = nvme_tcp_map_data(queue, rq);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	if (unlikely(ret)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_cleanup_cmd(rq);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"Failed to map data (%d)\n", ret);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_tcp_commit_rqs(struct blk_mq_hw_ctx *hctx)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_queue *queue = hctx->driver_data;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	if (!llist_empty(&queue->req_list))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		queue_work_on(queue->io_cpu, nvme_tcp_wq, &queue->io_work);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static blk_status_t nvme_tcp_queue_rq(struct blk_mq_hw_ctx *hctx,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		const struct blk_mq_queue_data *bd)	/* [한국어] blk-mq — 태그·hctx·타임아웃·맵·완료 연동 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_ns *ns = hctx->queue->queuedata;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct nvme_tcp_queue *queue = hctx->driver_data;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct request *rq = bd->rq;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	struct nvme_tcp_request *req = blk_mq_rq_to_pdu(rq);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	bool queue_ready = test_bit(NVME_TCP_Q_LIVE, &queue->flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	blk_status_t ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!nvme_check_ready(&queue->ctrl->ctrl, rq, queue_ready))	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
		return nvme_fail_nonready_command(&queue->ctrl->ctrl, rq);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */

	ret = nvme_tcp_setup_cmd_pdu(ns, rq);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	if (unlikely(ret))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	nvme_start_request(rq);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */

	nvme_tcp_queue_request(req, bd->last);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	return BLK_STS_OK;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void nvme_tcp_map_queues(struct blk_mq_tag_set *set)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_ctrl *ctrl = to_tcp_ctrl(set->driver_data);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	nvmf_map_queues(set, &ctrl->ctrl, ctrl->io_queues);	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_tcp_poll(struct blk_mq_hw_ctx *hctx, struct io_comp_batch *iob)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_queue *queue = hctx->driver_data;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct sock *sk = queue->sock->sk;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!test_bit(NVME_TCP_Q_LIVE, &queue->flags))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	set_bit(NVME_TCP_Q_POLLING, &queue->flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (sk_can_busy_loop(sk) && skb_queue_empty_lockless(&sk->sk_receive_queue))	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
		sk_busy_loop(sk, true);	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
	ret = nvme_tcp_try_recv(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	clear_bit(NVME_TCP_Q_POLLING, &queue->flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return ret < 0 ? ret : queue->nr_cqe;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int nvme_tcp_get_address(struct nvme_ctrl *ctrl, char *buf, int size)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_queue *queue = &to_tcp_ctrl(ctrl)->queues[0];	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct sockaddr_storage src_addr;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	int ret, len;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	len = nvmf_get_address(ctrl, buf, size);	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */

	if (!test_bit(NVME_TCP_Q_LIVE, &queue->flags))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return len;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	mutex_lock(&queue->queue_lock);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */

	ret = kernel_getsockname(queue->sock, (struct sockaddr *)&src_addr);	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	if (ret > 0) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		if (len > 0)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			len--; /* strip trailing newline */	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		len += scnprintf(buf + len, size - len, "%ssrc_addr=%pISc\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				(len) ? "," : "", &src_addr);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	mutex_unlock(&queue->queue_lock);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */

	return len;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static const struct blk_mq_ops nvme_tcp_mq_ops = {	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	.queue_rq	= nvme_tcp_queue_rq,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	.commit_rqs	= nvme_tcp_commit_rqs,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	.complete	= nvme_complete_rq,	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
	.init_request	= nvme_tcp_init_request,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	.exit_request	= nvme_tcp_exit_request,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	.init_hctx	= nvme_tcp_init_hctx,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	.timeout	= nvme_tcp_timeout,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	.map_queues	= nvme_tcp_map_queues,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	.poll		= nvme_tcp_poll,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static const struct blk_mq_ops nvme_tcp_admin_mq_ops = {	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	.queue_rq	= nvme_tcp_queue_rq,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	.complete	= nvme_complete_rq,	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
	.init_request	= nvme_tcp_init_request,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	.exit_request	= nvme_tcp_exit_request,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	.init_hctx	= nvme_tcp_init_admin_hctx,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	.timeout	= nvme_tcp_timeout,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static const struct nvme_ctrl_ops nvme_tcp_ctrl_ops = {	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	.name			= "tcp",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	.module			= THIS_MODULE,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	.flags			= NVME_F_FABRICS | NVME_F_BLOCKING,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	.reg_read32		= nvmf_reg_read32,	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
	.reg_read64		= nvmf_reg_read64,	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
	.reg_write32		= nvmf_reg_write32,	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
	.subsystem_reset	= nvmf_subsystem_reset,	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
	.free_ctrl		= nvme_tcp_free_ctrl,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	.submit_async_event	= nvme_tcp_submit_async_event,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	.delete_ctrl		= nvme_tcp_delete_ctrl,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	.get_address		= nvme_tcp_get_address,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	.stop_ctrl		= nvme_tcp_stop_ctrl,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	.get_virt_boundary	= nvmf_get_virt_boundary,	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static bool	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
nvme_tcp_existing_controller(struct nvmf_ctrl_options *opts)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_ctrl *ctrl;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	bool found = false;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	mutex_lock(&nvme_tcp_ctrl_mutex);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	list_for_each_entry(ctrl, &nvme_tcp_ctrl_list, list) {	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		found = nvmf_ip_options_match(&ctrl->ctrl, opts);	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
		if (found)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			break;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	mutex_unlock(&nvme_tcp_ctrl_mutex);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	return found;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static struct nvme_tcp_ctrl *nvme_tcp_alloc_ctrl(struct device *dev,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		struct nvmf_ctrl_options *opts)	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_ctrl *ctrl;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ctrl = kzalloc_obj(*ctrl);	/* [한국어] 커널 메모리 생명주기 */
	if (!ctrl)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return ERR_PTR(-ENOMEM);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	INIT_LIST_HEAD(&ctrl->list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ctrl->ctrl.opts = opts;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ctrl->ctrl.queue_count = opts->nr_io_queues + opts->nr_write_queues +	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				opts->nr_poll_queues + 1;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ctrl->ctrl.sqsize = opts->queue_size - 1;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ctrl->ctrl.kato = opts->kato;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	INIT_DELAYED_WORK(&ctrl->connect_work,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvme_tcp_reconnect_ctrl_work);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	INIT_WORK(&ctrl->err_work, nvme_tcp_error_recovery_work);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	INIT_WORK(&ctrl->ctrl.reset_work, nvme_reset_ctrl_work);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */

	if (!(opts->mask & NVMF_OPT_TRSVCID)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		opts->trsvcid =	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			kstrdup(__stringify(NVME_TCP_DISC_PORT), GFP_KERNEL);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
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

	if (opts->mask & NVMF_OPT_HOST_IFACE) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		if (!__dev_get_by_name(&init_net, opts->host_iface)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			pr_err("invalid interface passed: %s\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			       opts->host_iface);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			ret = -ENODEV;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			goto out_free_ctrl;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
		}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!opts->duplicate_connect && nvme_tcp_existing_controller(opts)) {	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		ret = -EALREADY;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_free_ctrl;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ctrl->queues = kzalloc_objs(*ctrl->queues, ctrl->ctrl.queue_count);	/* [한국어] 커널 메모리 생명주기 */
	if (!ctrl->queues) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = -ENOMEM;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_free_ctrl;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = nvme_init_ctrl(&ctrl->ctrl, dev, &nvme_tcp_ctrl_ops, 0);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_kfree_queues;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	return ctrl;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
out_kfree_queues:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	kfree(ctrl->queues);	/* [한국어] 커널 메모리 생명주기 */
out_free_ctrl:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	kfree(ctrl);	/* [한국어] 커널 메모리 생명주기 */
	return ERR_PTR(ret);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static struct nvme_ctrl *nvme_tcp_create_ctrl(struct device *dev,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		struct nvmf_ctrl_options *opts)	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_ctrl *ctrl;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ctrl = nvme_tcp_alloc_ctrl(dev, opts);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	if (IS_ERR(ctrl))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return ERR_CAST(ctrl);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	ret = nvme_add_ctrl(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_put_ctrl;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	if (!nvme_change_ctrl_state(&ctrl->ctrl, NVME_CTRL_CONNECTING)) {	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
		WARN_ON_ONCE(1);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ret = -EINTR;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_uninit_ctrl;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ret = nvme_tcp_setup_ctrl(&ctrl->ctrl, true);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_uninit_ctrl;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	dev_info(ctrl->ctrl.device, "new ctrl: NQN \"%s\", addr %pISp, hostnqn: %s\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvmf_ctrl_subsysnqn(&ctrl->ctrl), &ctrl->addr, opts->host->nqn);	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */

	mutex_lock(&nvme_tcp_ctrl_mutex);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	list_add_tail(&ctrl->list, &nvme_tcp_ctrl_list);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	mutex_unlock(&nvme_tcp_ctrl_mutex);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	return &ctrl->ctrl;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

out_uninit_ctrl:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_uninit_ctrl(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
out_put_ctrl:	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_put_ctrl(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret > 0)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = -EIO;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return ERR_PTR(ret);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static struct nvmf_transport_ops nvme_tcp_transport = {	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	.name		= "tcp",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	.module		= THIS_MODULE,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	.required_opts	= NVMF_OPT_TRADDR,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	.allowed_opts	= NVMF_OPT_TRSVCID | NVMF_OPT_RECONNECT_DELAY |	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			  NVMF_OPT_HOST_TRADDR | NVMF_OPT_CTRL_LOSS_TMO |	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			  NVMF_OPT_HDR_DIGEST | NVMF_OPT_DATA_DIGEST |	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			  NVMF_OPT_NR_WRITE_QUEUES | NVMF_OPT_NR_POLL_QUEUES |	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			  NVMF_OPT_TOS | NVMF_OPT_HOST_IFACE | NVMF_OPT_TLS |	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			  NVMF_OPT_KEYRING | NVMF_OPT_TLS_KEY | NVMF_OPT_CONCAT,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	.create_ctrl	= nvme_tcp_create_ctrl,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static int __init nvme_tcp_init_module(void)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	unsigned int wq_flags = WQ_MEM_RECLAIM | WQ_HIGHPRI | WQ_SYSFS;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int cpu;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	BUILD_BUG_ON(sizeof(struct nvme_tcp_hdr) != 8);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	BUILD_BUG_ON(sizeof(struct nvme_tcp_cmd_pdu) != 72);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	BUILD_BUG_ON(sizeof(struct nvme_tcp_data_pdu) != 24);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	BUILD_BUG_ON(sizeof(struct nvme_tcp_rsp_pdu) != 24);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	BUILD_BUG_ON(sizeof(struct nvme_tcp_r2t_pdu) != 24);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	BUILD_BUG_ON(sizeof(struct nvme_tcp_icreq_pdu) != 128);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	BUILD_BUG_ON(sizeof(struct nvme_tcp_icresp_pdu) != 128);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	BUILD_BUG_ON(sizeof(struct nvme_tcp_term_pdu) != 24);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	if (wq_unbound)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		wq_flags |= WQ_UNBOUND;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	nvme_tcp_wq = alloc_workqueue("nvme_tcp_wq", wq_flags, 0);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	if (!nvme_tcp_wq)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		return -ENOMEM;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	for_each_possible_cpu(cpu)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		atomic_set(&nvme_tcp_cpu_queues[cpu], 0);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	nvmf_register_transport(&nvme_tcp_transport);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

static void __exit nvme_tcp_cleanup_module(void)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_ctrl *ctrl;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	nvmf_unregister_transport(&nvme_tcp_transport);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	mutex_lock(&nvme_tcp_ctrl_mutex);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	list_for_each_entry(ctrl, &nvme_tcp_ctrl_list, list)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		nvme_delete_ctrl(&ctrl->ctrl);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
	mutex_unlock(&nvme_tcp_ctrl_mutex);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	flush_workqueue(nvme_delete_wq);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	destroy_workqueue(nvme_tcp_wq);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
}	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

module_init(nvme_tcp_init_module);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
module_exit(nvme_tcp_cleanup_module);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

MODULE_DESCRIPTION("NVMe host TCP transport driver");	/* [한국어] 모듈 경계·파라미터·심볼 공개 */
MODULE_LICENSE("GPL v2");	/* [한국어] 모듈 경계·파라미터·심볼 공개 */
