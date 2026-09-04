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
		 "nvme TLS handshake timeout in seconds (default 10)");
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

/*
 * [한국어]
 * nvme_tcp_reclassify_socket - 이 소켓의 락 클래스를 NVMe 전용으로 바꾼다
 *
 * @sock: 방금 만든 큐 소켓
 * @return: 없음
 *
 * lockdep(락 순서 검증기)만을 위한 함수다. 기능에는 아무 영향이 없고,
 * CONFIG_LOCKDEP 가 꺼진 빌드에서는 아래 빈 함수로 대체된다.
 *
 * 왜 필요한가: lockdep 은 같은 클래스의 락끼리 잡는 순서를 기억해 역전을
 * 잡아낸다. 그런데 이 드라이버의 소켓 락은 일반 소켓 락과 잡히는 문맥이
 * 다르다 -- 블록 계층의 제출 경로에서 잡히고, 그 아래로 다시 네트워크
 * 스택의 락이 이어진다. 클래스를 나누지 않으면 lockdep 이 그 두 경로를
 * 하나로 보고 있지도 않은 순환을 보고한다.
 *
 * IPv4 와 IPv6 에 서로 다른 키를 주는 것도 같은 이유다. 둘은 별개의
 * 주소 체계이고 같은 시스템에서 함께 쓰일 수 있어, 한 클래스로 묶으면
 * 역시 거짓 양성이 난다.
 *
 * 실행 컨텍스트: 큐 소켓 생성 직후. 잠들지 않는다.
 *
 * 호출 체인:
 *   nvme_tcp_alloc_queue → [이 함수] → sock_lock_init_class_and_name
 */
static void nvme_tcp_reclassify_socket(struct socket *sock)
{
	struct sock *sk = sock->sk;

	if (WARN_ON_ONCE(!sock_allow_reclassification(sk)))	/* [한국어] 이미 사용 중인 소켓은 재분류할 수 없다 — 새로 만든 것이어야 한다 */
		return;

	switch (sk->sk_family) {
	case AF_INET:
		sock_lock_init_class_and_name(sk, "slock-AF_INET-NVME",	/* [한국어] 일반 소켓과 다른 클래스를 주어 lockdep 의 거짓 양성을 막는다 */
					      &nvme_tcp_slock_key[0],
					      "sk_lock-AF_INET-NVME",
					      &nvme_tcp_sk_key[0]);
		break;
	case AF_INET6:
		sock_lock_init_class_and_name(sk, "slock-AF_INET6-NVME",	/* [한국어] IPv6 는 또 다른 키 — 같은 시스템에서 함께 쓰일 수 있어 묶으면 역시 거짓 양성이 난다 */
					      &nvme_tcp_slock_key[1],
					      "sk_lock-AF_INET6-NVME",
					      &nvme_tcp_sk_key[1]);
		break;
	default:
		WARN_ON_ONCE(1);	/* [한국어] NVMe/TCP 는 IP 소켓만 쓴다 */
	}
}
#else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
static void nvme_tcp_reclassify_socket(struct socket *sock) { }	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
#endif	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

/* [한국어] 요청 하나를 소켓으로 내보내는 동안 거쳐 가는 단계.
 * TCP 는 한 번의 write 로 다 나간다는 보장이 없다. 소켓이 받아 준 만큼만
 * 보내고 나머지는 다음 기회에 이어야 하므로, "어디까지 보냈는가"를 요청마다
 * 들고 있어야 한다. 그 위치가 이 열거값이다. */
enum nvme_tcp_send_state {
	/* [한국어] 명령 PDU 를 보내는 중. 모든 요청이 여기서 시작한다. */
	NVME_TCP_SEND_CMD_PDU = 0,
	/* [한국어] H2C(Host to Controller) 데이터 PDU 헤더를 보내는 중.
	 * 쓰기 요청에서 타겟이 R2T 로 "이제 데이터를 보내라"고 하면 이 단계가 온다. */
	NVME_TCP_SEND_H2C_PDU,
	/* [한국어] 데이터 본문을 보내는 중. 요청 크기에 따라 이 단계에 여러 번 머문다. */
	NVME_TCP_SEND_DATA,
	/* [한국어] 데이터 다이제스트(CRC32C) 4바이트를 보내는 중.
	 * data_digest 를 협상한 연결에서만 나타나는 마지막 단계다. */
	NVME_TCP_SEND_DDGST,
};

/* [한국어] 요청 하나의 TCP 측 상태.
 * blk-mq 가 태그마다 미리 잡아 두며, 하드웨어 큐가 없는 트랜스포트라
 * "지금 어디까지 보냈고 얼마나 남았는가"를 전부 여기에 들고 있어야 한다. */
struct nvme_tcp_request {
	/* [한국어] NVMe 코어가 보는 요청 부분. 반드시 맨 앞이어야 blk_mq_rq_to_pdu()
	 * 가 돌려준 주소를 코어가 struct nvme_request 로 읽을 수 있다.
	 * 설정자: nvme_tcp_init_request / nvme_setup_cmd.
	 * 읽는 자: nvme_complete_rq 의 재시도·페일오버 판정. */
	struct nvme_request	req;
	/* [한국어] 이 요청이 보낼 PDU 헤더 버퍼(명령 PDU 또는 H2C 데이터 PDU).
	 * 왜 요청마다 두나: 여러 요청이 동시에 전송 대기에 있을 수 있고, 각자
	 *   부분 전송 상태가 다르므로 헤더를 공유할 수 없다.
	 * 설정자: nvme_tcp_init_request 가 큐 생성 시 할당.
	 * 값 범위: 헤더 다이제스트를 쓰면 그만큼 더 큰 버퍼다. */
	void			*pdu;
	/* [한국어] 이 요청이 속한 큐. 완료·재큐잉 시 소켓과 상태로 돌아가는 통로다. */
	struct nvme_tcp_queue	*queue;
	/* [한국어] 이 요청이 옮겨야 할 전체 데이터 바이트 수.
	 * 설정자: nvme_tcp_setup_cmd_pdu 가 blk_rq_nr_phys_segments 로부터 계산.
	 * 읽는 자: 인라인 전송 가능 여부 판정과 남은 양 계산의 기준. */
	u32			data_len;
	/* [한국어] 지금 보내는 중인 PDU 의 전체 길이(헤더 + 인라인 데이터 + 다이제스트).
	 * 읽는 자: try_send_cmd_pdu 가 이만큼 다 나갔는지 판정한다. */
	u32			pdu_len;
	/* [한국어] 그 PDU 중 이미 소켓이 받아 간 바이트 수.
	 * 왜 필요한가: 부분 전송이 정상이므로 다음 시도에서 어디부터 이어야 할지
	 *   알아야 한다. pdu_sent == pdu_len 이 되어야 다음 단계로 넘어간다. */
	u32			pdu_sent;
	/* [한국어] 타겟이 R2T 로 요구한 데이터 중 아직 보내지 않은 바이트 수.
	 * 왜 필요한가: 쓰기 데이터는 타겟이 준비됐다고 알릴 때만 보낼 수 있고,
	 *   한 번의 R2T 가 maxh2cdata 보다 크면 여러 H2C PDU 로 쪼개 보내야 한다.
	 * 설정자: R2T PDU 를 받은 nvme_tcp_handle_r2t.
	 * 값 범위: 0 이면 이번 R2T 분을 다 보낸 것. */
	u32			h2cdata_left;
	/* [한국어] 그 데이터가 요청 전체에서 시작하는 오프셋. 쪼개 보낼 때마다 전진한다. */
	u32			h2cdata_offset;
	/* [한국어] Transfer Tag. 타겟이 R2T 에 실어 준 식별자를 그대로 H2C PDU 에 되돌려
	 * 어느 전송 요구에 대한 응답인지 짝지어 준다.
	 * 설정자: handle_r2t 가 받은 값을 저장. 읽는 자: setup_h2c_data_pdu. */
	u16			ttag;
	/* [한국어] 응답에서 꺼낸 NVMe 상태 코드. 완료 시점까지 보관한다. */
	__le16			status;
	/* [한국어] 큐의 send_list 에 매달리기 위한 고리. io_work 가 이 목록을 훑어 보낸다.
	 * 동기화: send_mutex 아래에서만 조작한다. */
	struct list_head	entry;
	/* [한국어] 잠금 없는 입력 목록(req_list)용 고리.
	 * 왜 두 개인가: queue_rq 는 어느 CPU 에서든 불릴 수 있어 잠금 없이 넣을 수
	 *   있어야 하고(llist), io_work 는 그것을 한 번에 걷어 자기 send_list 로
	 *   옮긴 뒤 순서대로 처리한다. 제출 경로와 전송 경로의 경합을 없애는 구조다. */
	struct llist_node	lentry;
	/* [한국어] 이 요청이 보낸 데이터의 CRC32C 누적값. 마지막에 4바이트로 나간다. */
	__le32			ddgst;

	/* [한국어] 지금 데이터를 읽어 내고 있는 bio.
	 * 왜 필요한가: 한 요청이 여러 bio 에 걸칠 수 있고 전송이 중간에 끊기므로,
	 *   다음에 이어갈 위치를 bio 단위로도 기억해야 한다.
	 * 설정자: nvme_tcp_init_iter 가 첫 bio 로 세우고, 소진되면 다음으로 넘긴다. */
	struct bio		*curr_bio;
	/* [한국어] 현재 bio 안에서의 읽기 위치를 담은 반복자.
	 * 소켓 전송 함수에 그대로 넘겨져, 보낸 만큼 자동으로 전진한다.
	 * 이것이 부분 전송을 이어 갈 수 있게 하는 실제 장치다. */
	struct iov_iter		iter;

	/* send state */
	/* [한국어] 현재 단계 안에서의 오프셋(헤더 몇 바이트째, 다이제스트 몇 바이트째).
	 * pdu_sent 와 달리 단계가 바뀔 때마다 0 으로 되돌아간다. */
	size_t			offset;
	/* [한국어] 이 요청에서 지금까지 보낸 데이터 본문의 총 바이트 수.
	 * 읽는 자: data_len 과 비교해 전송이 끝났는지 판정한다. */
	size_t			data_sent;
	/* [한국어] 위 enum 이 정의한 현재 전송 단계.
	 * 이 필드 하나가 "이 요청이 다음에 무엇을 보내야 하는가"의 답이다. */
	enum nvme_tcp_send_state state;
};

/* [한국어] 큐의 생애·동작 상태 비트. test_and_set/clear 로 다뤄
 * 여러 경로가 동시에 같은 큐를 조작해도 한 쪽만 통과하게 한다. */
enum nvme_tcp_queue_flags {
	/* [한국어] 소켓이 만들어지고 큐 구조체가 초기화됐다. 아직 Connect 전이라
	 * 명령을 보낼 수는 없지만, 해체 시 정리할 자원은 이미 있다는 표시다. */
	NVME_TCP_Q_ALLOCATED	= 0,
	/* [한국어] Fabrics Connect 까지 끝나 이 큐로 I/O 를 보낼 수 있다.
	 * 읽는 자: queue_rq 가 아직 준비 안 된 큐의 요청을 걸러내는 기준. */
	NVME_TCP_Q_LIVE		= 1,
	/* [한국어] 지금 blk-mq poll 경로가 이 큐를 훑고 있다.
	 * 왜 필요한가: 폴링 중에는 소켓 콜백이 io_work 를 깨우면 안 된다.
	 *   같은 큐를 두 문맥이 동시에 수신 처리하면 PDU 파싱 상태가 깨진다. */
	NVME_TCP_Q_POLLING	= 2,
	/* [한국어] 이 큐의 io_work 를 특정 CPU 에 고정했다.
	 * 왜: 큐마다 다른 CPU 에 붙여야 여러 큐의 송수신이 한 코어에 몰리지 않는다.
	 *   설정에 실패했을 수도 있어 성공 여부를 비트로 남긴다. */
	NVME_TCP_Q_IO_CPU_SET	= 3,
};

/* [한국어] 바이트 스트림에서 PDU 를 되짚어 읽는 수신 상태 기계.
 * TCP 에는 메시지 경계가 없다. 한 번의 read 가 PDU 하나의 절반일 수도,
 * 두 개 반일 수도 있다. 그래서 "지금 무엇을 모으는 중인가"를 큐가 기억하고,
 * 도착한 바이트를 그 단계에 따라 나눠 담는다. 이 열거값이 그 위치다. */
enum nvme_tcp_recv_state {
	/* [한국어] PDU 헤더를 모으는 중. 헤더가 다 모여야 그 안의 길이 필드를 읽어
	 * 본문이 어디서 끝나는지 알 수 있다. 모든 수신이 여기서 시작한다. */
	NVME_TCP_RECV_PDU = 0,
	/* [한국어] 헤더가 알려 준 길이만큼 데이터 본문을 받는 중.
	 * C2H(Controller to Host) 데이터 PDU 에서만 이 단계로 들어간다. */
	NVME_TCP_RECV_DATA,
	/* [한국어] 데이터 다이제스트 4바이트를 받는 중. 받고 나서 직접 계산한
	 * CRC32C 와 비교해 어긋나면 연결을 오류로 처리한다. */
	NVME_TCP_RECV_DDGST,
};

struct nvme_tcp_ctrl;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
/* [한국어] 큐 하나가 소유하는 소켓과 송수신 상태 전부.
 * RDMA 의 QP/CQ 자리에 여기서는 소켓 하나와 io_work 하나가 들어간다.
 * 송신과 수신을 같은 work 가 처리하므로, 그 직렬화가 곧 큐 단위 잠금이 된다. */
struct nvme_tcp_queue {
	/* [한국어] 이 큐의 TCP 소켓. 모든 PDU 가 여기를 지난다.
	 * 설정자: nvme_tcp_alloc_queue 가 sock_create 후 연결한다.
	 * 값 범위: TLS 를 쓰면 이 소켓 위에 커널 TLS 계층이 얹힌다. */
	struct socket		*sock;
	/* [한국어] 이 큐의 송신과 수신을 모두 담당하는 작업.
	 * 왜 하나인가: 둘을 나누면 같은 소켓을 두 문맥이 만지게 되어 잠금이 필요하다.
	 *   하나로 두면 work 실행 자체가 직렬화라 추가 잠금 없이 일관성이 선다. */
	struct work_struct	io_work;
	/* [한국어] 위 work 를 돌릴 CPU. 큐마다 다른 CPU 로 흩어 한 코어 쏠림을 막는다.
	 * 설정자: nvme_tcp_set_queue_io_cpu. 성공 여부는 Q_IO_CPU_SET 비트에 남는다. */
	int			io_cpu;

	/* [한국어] 큐의 시작·정지를 직렬화한다. 오류 복구와 정상 해체가 동시에
	 * 같은 큐를 내리려 할 때 한 쪽만 통과시킨다. */
	struct mutex		queue_lock;
	/* [한국어] 실제 소켓 쓰기를 직렬화한다.
	 * 왜 queue_lock 과 별개인가: 제출 경로가 직접 보내는 경우와 io_work 가
	 *   보내는 경우가 겹칠 수 있는데, 이때 필요한 것은 큐 생명주기가 아니라
	 *   쓰기 순서의 보호다. 둘을 나누어 제출이 해체를 기다리지 않게 한다. */
	struct mutex		send_mutex;
	/* [한국어] queue_rq 가 잠금 없이 요청을 밀어 넣는 입력 목록.
	 * 왜 llist 인가: 제출은 어느 CPU 에서든 들어오므로 잠금을 잡으면 그것이
	 *   곧 경합 지점이 된다. llist 는 원자적 push 만으로 넣을 수 있다. */
	struct llist_head	req_list;
	/* [한국어] io_work 가 req_list 를 통째로 걷어 옮겨 놓고 순서대로 처리하는 목록.
	 * 이 두 단계 구조 덕에 제출자와 전송자가 서로를 기다리지 않는다. */
	struct list_head	send_list;

	/* recv state */
	/* [한국어] 수신 중인 PDU 헤더를 모으는 버퍼. 큐마다 하나면 충분하다 --
	 * 스트림은 순서대로 오므로 동시에 두 PDU 를 파싱할 일이 없다. */
	void			*pdu;
	/* [한국어] 헤더에서 아직 못 받은 바이트 수. 0 이 되어야 헤더 해석이 가능하다. */
	int			pdu_remaining;
	/* [한국어] 헤더 버퍼에서 다음에 채워 넣을 위치. 부분 수신을 이어 가는 지점이다. */
	int			pdu_offset;
	/* [한국어] 데이터 본문에서 아직 못 받은 바이트 수.
	 * 설정자: 헤더를 다 받고 그 안의 길이 필드로 세운다. */
	size_t			data_remaining;
	/* [한국어] 다이제스트 4바이트 중 아직 못 받은 바이트 수. */
	size_t			ddgst_remaining;
	/* [한국어] 이번 io_work 실행에서 완료시킨 요청 수.
	 * 읽는 자: blk-mq poll 이 "몇 개를 수확했는가"로 돌려줄 값이다. */
	unsigned int		nr_cqe;

	/* send state */
	/* [한국어] 지금 전송 중인 요청. 부분 전송이라 다음 기회에 이 요청부터 이어야 한다.
	 * NULL 이면 새 요청을 send_list 에서 꺼낼 차례라는 뜻이다. */
	struct nvme_tcp_request *request;

	/* [한국어] 타겟이 한 번의 H2C PDU 로 받을 수 있는 최대 데이터 바이트.
	 * 설정자: ICResp 협상 결과. 읽는 자: R2T 분량을 이 크기로 쪼개 보낸다. */
	u32			maxh2cdata;
	/* [한국어] 명령 캡슐의 크기. 인라인 데이터를 함께 실을 수 있는지가 여기서 갈린다.
	 * admin 큐는 SQE 크기 그대로라 인라인 여유가 없다. */
	size_t			cmnd_capsule_len;
	/* [한국어] 이 큐가 속한 컨트롤러. 초기화 후 불변. */
	struct nvme_tcp_ctrl	*ctrl;
	/* [한국어] 위 enum nvme_tcp_queue_flags 의 비트 집합. */
	unsigned long		flags;
	/* [한국어] 소켓의 data_ready 콜백이 io_work 를 깨워도 되는지.
	 * 왜 필요한가: 큐를 내리는 중에는 깨우면 안 된다. 이미 정리한 자원을
	 *   io_work 가 다시 만지게 되기 때문이다. */
	bool			rd_enabled;

	/* [한국어] 헤더 다이제스트를 쓰는 연결인가. ICReq/ICResp 협상 결과다.
	 * 켜지면 모든 PDU 헤더 뒤에 CRC32C 4바이트가 붙어 길이 계산이 달라진다. */
	bool			hdr_digest;
	/* [한국어] 데이터 다이제스트를 쓰는 연결인가. 켜지면 송수신 양쪽에
	 * SEND_DDGST / RECV_DDGST 단계가 추가된다. */
	bool			data_digest;
	/* [한국어] 이 소켓 위에 커널 TLS 가 얹혀 있는가.
	 * 켜지면 평문 write 가 아니라 TLS 레코드로 나가고, 핸드셰이크는
	 * 유저스페이스 데몬이 netlink 로 수행한 뒤 키를 커널에 넘긴 것이다. */
	bool			tls_enabled;
	/* [한국어] 수신 중인 데이터의 CRC32C 누적값. 마지막에 recv_ddgst 와 비교한다. */
	u32			rcv_crc;
	/* [한국어] 송신 중인 데이터의 CRC32C 누적값. 다 보내면 ddgst 로 나간다. */
	u32			snd_crc;
	/* [한국어] 우리가 직접 계산한 기대 다이제스트. */
	__le32			exp_ddgst;
	/* [한국어] 상대가 실제로 보내 온 다이제스트. 위와 다르면 데이터가 손상된 것이다. */
	__le32			recv_ddgst;
	/* [한국어] TLS 핸드셰이크 완료를 기다리는 객체.
	 * 핸드셰이크는 유저스페이스 데몬이 하므로, 커널은 여기서 잠들어 결과를 기다린다. */
	struct completion       tls_complete;
	/* [한국어] 그 핸드셰이크의 결과. 콜백이 값을 돌려줄 수 없어 여기 남긴다. */
	int                     tls_err;
	/* [한국어] PDU 헤더 같은 작은 버퍼를 페이지 조각에서 잘라 쓰는 캐시.
	 * 요청마다 kmalloc 을 부르지 않기 위한 것이다. */
	struct page_frag_cache	pf_cache;

	/* [한국어] 원래 소켓이 갖고 있던 콜백들. 이 드라이버가 자기 것으로 바꿔 달면서
	 * 원본을 보관해 두었다가, 큐를 내릴 때 그대로 되돌려 놓는다.
	 * 그러지 않으면 소켓을 닫는 과정에서 이미 사라진 큐를 가리키는 콜백이 불린다. */
	void (*state_change)(struct sock *);
	/* [한국어] 데이터 도착 알림. 이 드라이버는 io_work 를 깨우는 것으로 바꾼다. */
	void (*data_ready)(struct sock *);
	/* [한국어] 송신 버퍼에 여유가 생겼다는 알림. 부분 전송을 이어 갈 기회다. */
	void (*write_space)(struct sock *);
};

/* [한국어] TCP 컨트롤러 하나. nvme_ctrl 을 값으로 품어
 * to_tcp_ctrl() 이 container_of 로 되찾을 수 있게 한다. */
/* [한국어] TCP 컨트롤러 하나. nvme_ctrl 을 값으로 품어
 * to_tcp_ctrl() 이 container_of 로 되찾을 수 있게 한다. */
struct nvme_tcp_ctrl {
	/* read only in the hot path */
	/* [한국어] 큐 배열. 0 번이 admin, 1 번부터 I/O 다.
	 * 위 영어 주석대로 핫패스에서는 읽기만 하므로 잠금 없이 접근한다. */
	struct nvme_tcp_queue	*queues;
	/* [한국어] I/O 큐용 태그셋. 태그 하나가 동시 처리 가능한 요청 하나이며,
	 * 태그마다 nvme_tcp_request 가 미리 붙어 있다. */
	struct blk_mq_tag_set	tag_set;

	/* other member variables */
	/* [한국어] 전역 nvme_tcp_ctrl_list 에 매달리는 고리. nvme_tcp_ctrl_mutex 가 지킨다. */
	struct list_head	list;
	/* [한국어] admin 전용 태그셋. I/O 와 분리해야 I/O 가 모두 막힌 상태에서도
	 * 리셋·삭제 같은 admin 명령이 태그를 얻어 통과할 수 있다. */
	struct blk_mq_tag_set	admin_tag_set;
	/* [한국어] 접속할 타겟 주소. 'nvme connect' 의 traddr/trsvcid 가 담긴다. */
	struct sockaddr_storage addr;
	/* [한국어] 출발지 주소. host-traddr 로 특정 로컬 인터페이스를 강제할 때 쓴다.
	 * 지정하지 않으면 0 으로 남고 커널 라우팅이 고른다. */
	struct sockaddr_storage src_addr;
	/* [한국어] 코어가 보는 컨트롤러. 상태 기계와 Identify 결과가 여기 있고,
	 * to_tcp_ctrl() 이 이 필드에서 바깥을 되찾는다. */
	struct nvme_ctrl	ctrl;

	/* [한국어] 오류 복구 작업. 소켓이 끊기거나 타임아웃이 나면 여기로 넘긴다.
	 * 워크큐인 이유: 복구는 큐를 내리고 다시 세우는 잠들 수 있는 작업이라
	 * 소켓 콜백이나 완료 문맥에서 직접 할 수 없다. */
	struct work_struct	err_work;
	/* [한국어] 재연결 시도 작업. 지연 워크라 옵션에 적힌 간격만큼 쉬었다 돈다. */
	struct delayed_work	connect_work;
	/* [한국어] 비동기 이벤트(AEN) 전용 요청.
	 * 왜 따로 두나: AEN 은 태그를 소비하지 않는 상주 명령이라 태그별 요청
	 * 구조체를 쓸 수 없다. 컨트롤러당 하나면 충분하다. */
	struct nvme_tcp_request async_req;
	/* [한국어] 기본·읽기 전용·폴링 큐 각각의 개수.
	 * 읽기 전용 큐를 두면 쓰기가 읽기 지연을 밀어내지 않고,
	 * 폴링 큐는 인터럽트 없이 blk-mq poll 로만 돈다. */
	u32			io_queues[HCTX_MAX_TYPES];
};

static LIST_HEAD(nvme_tcp_ctrl_list);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
static DEFINE_MUTEX(nvme_tcp_ctrl_mutex);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
static struct workqueue_struct *nvme_tcp_wq;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
static const struct blk_mq_ops nvme_tcp_mq_ops;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
static const struct blk_mq_ops nvme_tcp_admin_mq_ops;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
static int nvme_tcp_try_send(struct nvme_tcp_queue *queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

static inline struct nvme_tcp_ctrl *to_tcp_ctrl(struct nvme_ctrl *ctrl)	/* [한국어] 코어가 넘긴 nvme_ctrl 에서 이 트랜스포트의 바깥 구조체를 되찾는다 */
{
	/* [한국어] nvme_ctrl 이 nvme_tcp_ctrl 안에 값으로 박혀 있어 가능한 되짚기다.
	 * 코어는 늘 nvme_ctrl 포인터만 넘기므로 트랜스포트 진입점마다 이 변환이 첫 줄에 온다. */
	return container_of(ctrl, struct nvme_tcp_ctrl, ctrl);
}

static inline int nvme_tcp_queue_id(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	return queue - queue->ctrl->queues;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static inline bool nvme_tcp_recv_pdu_supported(enum nvme_tcp_pdu_type type)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	switch (type) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	case nvme_tcp_c2h_term:	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	case nvme_tcp_c2h_data:	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	case nvme_tcp_r2t:	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	case nvme_tcp_rsp:	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		return true;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	default:
		return false;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}
}

/*
 * Check if the queue is TLS encrypted
 */
static inline bool nvme_tcp_queue_tls(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	if (!IS_ENABLED(CONFIG_NVME_TCP_TLS))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	return queue->tls_enabled;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

/*
 * Check if TLS is configured for the controller.
 */
static inline bool nvme_tcp_tls_configured(struct nvme_ctrl *ctrl)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	if (!IS_ENABLED(CONFIG_NVME_TCP_TLS))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	return ctrl->opts->tls || ctrl->opts->concat;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static inline struct blk_mq_tags *nvme_tcp_tagset(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	u32 queue_idx = nvme_tcp_queue_id(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	if (queue_idx == 0)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return queue->ctrl->admin_tag_set.tags[queue_idx];	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	return queue->ctrl->tag_set.tags[queue_idx - 1];	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static inline u8 nvme_tcp_hdgst_len(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	return queue->hdr_digest ? NVME_TCP_DIGEST_LENGTH : 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static inline u8 nvme_tcp_ddgst_len(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	return queue->data_digest ? NVME_TCP_DIGEST_LENGTH : 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static inline void *nvme_tcp_req_cmd_pdu(struct nvme_tcp_request *req)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	return req->pdu;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static inline void *nvme_tcp_req_data_pdu(struct nvme_tcp_request *req)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	/* use the pdu space in the back for the data pdu */
	return req->pdu + sizeof(struct nvme_tcp_cmd_pdu) -	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		sizeof(struct nvme_tcp_data_pdu);
}

static inline size_t nvme_tcp_inline_data_size(struct nvme_tcp_request *req)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	if (nvme_is_fabrics(req->req.cmd))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return NVME_TCP_ADMIN_CCSZ;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	return req->queue->cmnd_capsule_len - sizeof(struct nvme_command);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static inline bool nvme_tcp_async_req(struct nvme_tcp_request *req)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	return req == &req->queue->ctrl->async_req;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static inline bool nvme_tcp_has_inline_data(struct nvme_tcp_request *req)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	struct request *rq;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	if (unlikely(nvme_tcp_async_req(req)))	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		return false; /* async events don't have a request */	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	rq = blk_mq_rq_from_pdu(req);	/* [한국어] blk-mq — 태그·hctx·타임아웃·맵·완료 연동 */

	return rq_data_dir(rq) == WRITE && req->data_len &&	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
		req->data_len <= nvme_tcp_inline_data_size(req);
}

static inline struct page *nvme_tcp_req_cur_page(struct nvme_tcp_request *req)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	return req->iter.bvec->bv_page;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static inline size_t nvme_tcp_req_cur_offset(struct nvme_tcp_request *req)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	return req->iter.bvec->bv_offset + req->iter.iov_offset;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static inline size_t nvme_tcp_req_cur_length(struct nvme_tcp_request *req)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	return min_t(size_t, iov_iter_single_seg_count(&req->iter),	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
			req->pdu_len - req->pdu_sent);
}

static inline size_t nvme_tcp_pdu_data_left(struct nvme_tcp_request *req)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	return rq_data_dir(blk_mq_rq_from_pdu(req)) == WRITE ?	/* [한국어] blk-mq — 태그·hctx·타임아웃·맵·완료 연동 */
			req->pdu_len - req->pdu_sent : 0;
}

static inline size_t nvme_tcp_pdu_last_send(struct nvme_tcp_request *req,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		int len)
{
	return nvme_tcp_pdu_data_left(req) <= len;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
}

static void nvme_tcp_init_iter(struct nvme_tcp_request *req,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		unsigned int dir)
{
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
	} else {
		struct bio *bio = req->curr_bio;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
		struct bvec_iter bi;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
		struct bio_vec bv;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

		vec = __bvec_iter_bvec(bio->bi_io_vec, bio->bi_iter);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nr_bvec = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		bio_for_each_bvec(bv, bio, bi) {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nr_bvec++;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		}
		size = bio->bi_iter.bi_size;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		offset = bio->bi_iter.bi_bvec_done;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}

	iov_iter_bvec(&req->iter, dir, vec, nr_bvec, size);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	req->iter.iov_offset = offset;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}

static inline void nvme_tcp_advance_req(struct nvme_tcp_request *req,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		int len)
{
	req->data_sent += len;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	req->pdu_sent += len;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	iov_iter_advance(&req->iter, len);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (!iov_iter_count(&req->iter) &&	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
	    req->data_sent < req->data_len) {
		req->curr_bio = req->curr_bio->bi_next;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_tcp_init_iter(req, ITER_SOURCE);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	}
}

static inline void nvme_tcp_send_all(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/* drain the send queue as much as we can... */
	do {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ret = nvme_tcp_try_send(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	} while (ret > 0);	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
}

static inline bool nvme_tcp_queue_has_pending(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	return !list_empty(&queue->send_list) ||	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
		!llist_empty(&queue->req_list);
}

static inline bool nvme_tcp_queue_more(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	return !nvme_tcp_queue_tls(queue) &&	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		nvme_tcp_queue_has_pending(queue);
}

static inline void nvme_tcp_queue_request(struct nvme_tcp_request *req,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		bool last)
{
	struct nvme_tcp_queue *queue = req->queue;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	bool empty;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	empty = llist_add(&req->lentry, &queue->req_list) &&	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		list_empty(&queue->send_list) && !queue->request;

	/*
	 * if we're the first on the send_list and we can try to send
	 * directly, otherwise queue io_work. Also, only do that if we
	 * are on the same cpu, so we don't introduce contention.
	 */
	if (queue->io_cpu == raw_smp_processor_id() &&	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
	    empty && mutex_trylock(&queue->send_mutex)) {	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
		nvme_tcp_send_all(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		mutex_unlock(&queue->send_mutex);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
	}

	if (last && nvme_tcp_queue_has_pending(queue))	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		queue_work_on(queue->io_cpu, nvme_tcp_wq, &queue->io_work);
}

static void nvme_tcp_process_req_list(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	struct nvme_tcp_request *req;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct llist_node *node;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	for (node = llist_del_all(&queue->req_list); node; node = node->next) {	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		req = llist_entry(node, struct nvme_tcp_request, lentry);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		list_add(&req->entry, &queue->send_list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}
}

static inline struct nvme_tcp_request *	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
nvme_tcp_fetch_request(struct nvme_tcp_queue *queue)
{
	struct nvme_tcp_request *req;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	req = list_first_entry_or_null(&queue->send_list,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			struct nvme_tcp_request, entry);
	if (!req) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_tcp_process_req_list(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		req = list_first_entry_or_null(&queue->send_list,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				struct nvme_tcp_request, entry);
		if (unlikely(!req))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			return NULL;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}

	list_del_init(&req->entry);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	init_llist_node(&req->lentry);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return req;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

#define NVME_TCP_CRC_SEED (~0)	/* [한국어] 상수/매크로 — PDU·큐·타임아웃·플래그 */

static inline void nvme_tcp_ddgst_update(u32 *crcp,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		struct page *page, size_t off, size_t len)	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
{
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
	}
}

static inline __le32 nvme_tcp_ddgst_final(u32 crc)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	return cpu_to_le32(~crc);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static inline __le32 nvme_tcp_hdgst(const void *pdu, size_t len)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	return cpu_to_le32(~crc32c(NVME_TCP_CRC_SEED, pdu, len));	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static inline void nvme_tcp_set_hdgst(void *pdu, size_t len)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	*(__le32 *)(pdu + len) = nvme_tcp_hdgst(pdu, len);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
}

static int nvme_tcp_verify_hdgst(struct nvme_tcp_queue *queue,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		void *pdu, size_t pdu_len)
{
	struct nvme_tcp_hdr *hdr = pdu;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	__le32 recv_digest;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	__le32 exp_digest;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (unlikely(!(hdr->flags & NVME_TCP_F_HDGST))) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"queue %d: header digest flag is cleared\n",
			nvme_tcp_queue_id(queue));
		return -EPROTO;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}

	recv_digest = *(__le32 *)(pdu + hdr->hlen);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	exp_digest = nvme_tcp_hdgst(pdu, pdu_len);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	if (recv_digest != exp_digest) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"header digest error: recv %#x expected %#x\n",
			le32_to_cpu(recv_digest), le32_to_cpu(exp_digest));
		return -EIO;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static int nvme_tcp_check_ddgst(struct nvme_tcp_queue *queue, void *pdu)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	struct nvme_tcp_hdr *hdr = pdu;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	u8 digest_len = nvme_tcp_hdgst_len(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	u32 len;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	len = le32_to_cpu(hdr->plen) - hdr->hlen -	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		((hdr->flags & NVME_TCP_F_HDGST) ? digest_len : 0);

	if (unlikely(len && !(hdr->flags & NVME_TCP_F_DDGST))) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"queue %d: data digest flag is cleared\n",
		nvme_tcp_queue_id(queue));
		return -EPROTO;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}
	queue->rcv_crc = NVME_TCP_CRC_SEED;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static void nvme_tcp_exit_request(struct blk_mq_tag_set *set,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		struct request *rq, unsigned int hctx_idx)	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
{
	struct nvme_tcp_request *req = blk_mq_rq_to_pdu(rq);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	page_frag_free(req->pdu);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}

static int nvme_tcp_init_request(struct blk_mq_tag_set *set,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		struct request *rq, unsigned int hctx_idx,	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
		unsigned int numa_node)
{
	struct nvme_tcp_ctrl *ctrl = to_tcp_ctrl(set->driver_data);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct nvme_tcp_request *req = blk_mq_rq_to_pdu(rq);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct nvme_tcp_cmd_pdu *pdu;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	int queue_idx = (set == &ctrl->tag_set) ? hctx_idx + 1 : 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct nvme_tcp_queue *queue = &ctrl->queues[queue_idx];	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	u8 hdgst = nvme_tcp_hdgst_len(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	req->pdu = page_frag_alloc(&queue->pf_cache,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		sizeof(struct nvme_tcp_cmd_pdu) + hdgst,
		GFP_KERNEL | __GFP_ZERO);
	if (!req->pdu)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return -ENOMEM;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	pdu = req->pdu;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	req->queue = queue;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_req(rq)->ctrl = &ctrl->ctrl;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_req(rq)->cmd = &pdu->cmd;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	init_llist_node(&req->lentry);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	INIT_LIST_HEAD(&req->entry);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static int nvme_tcp_init_hctx(struct blk_mq_hw_ctx *hctx, void *data,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		unsigned int hctx_idx)
{
	struct nvme_tcp_ctrl *ctrl = to_tcp_ctrl(data);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct nvme_tcp_queue *queue = &ctrl->queues[hctx_idx + 1];	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	hctx->driver_data = queue;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static int nvme_tcp_init_admin_hctx(struct blk_mq_hw_ctx *hctx, void *data,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		unsigned int hctx_idx)
{
	struct nvme_tcp_ctrl *ctrl = to_tcp_ctrl(data);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct nvme_tcp_queue *queue = &ctrl->queues[0];	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	hctx->driver_data = queue;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static enum nvme_tcp_recv_state	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
nvme_tcp_recv_state(struct nvme_tcp_queue *queue)
{
	return  (queue->pdu_remaining) ? NVME_TCP_RECV_PDU :	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
		(queue->ddgst_remaining) ? NVME_TCP_RECV_DDGST :	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		NVME_TCP_RECV_DATA;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}

static void nvme_tcp_init_recv_ctx(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	queue->pdu_remaining = sizeof(struct nvme_tcp_rsp_pdu) +	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
				nvme_tcp_hdgst_len(queue);
	queue->pdu_offset = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	queue->data_remaining = -1;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	queue->ddgst_remaining = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}

/*
 * [한국어]
 * nvme_tcp_error_recovery - 이 연결을 복구 대상으로 표시하고 복구 작업을 깨운다
 *
 * @ctrl: 복구할 컨트롤러
 * @return: 없음
 *
 * 이 함수 자체는 아무것도 복구하지 않는다. 상태를 RESETTING 으로 옮기고
 * 워크큐를 깨우는 것이 전부다. 실제 복구(큐 내리기, 소켓 닫기, 재연결)는
 * err_work 가 잠들 수 있는 문맥에서 한다.
 *
 * 상태 전이가 곧 중복 방지 장치다. 수신 파싱 실패, 송신 실패, 타임아웃,
 * 소켓 상태 변화 콜백 등 여러 경로가 동시에 복구를 요청할 수 있는데,
 * nvme_change_ctrl_state 는 이미 RESETTING 인 컨트롤러에 대해 false 를
 * 돌려주므로 먼저 도착한 하나만 통과한다.
 *
 * 실행 컨텍스트: 어디서든 불릴 수 있다 — 소켓 콜백, 수신 파싱(lock_sock 보유),
 * blk-mq 타임아웃 문맥. 그래서 여기서 잠드는 일을 하나도 하지 않는 것이 중요하다.
 *
 * 호출 체인:
 *   recv_skb / try_send / timeout / state_change → [이 함수] → err_work
 */
static void nvme_tcp_error_recovery(struct nvme_ctrl *ctrl)
{
	if (!nvme_change_ctrl_state(ctrl, NVME_CTRL_RESETTING))	/* [한국어] 이미 복구 중이면 false — 여러 경로가 동시에 불러도 하나만 통과한다 */
		return;

	dev_warn(ctrl->device, "starting error recovery\n");
	queue_work(nvme_reset_wq, &to_tcp_ctrl(ctrl)->err_work);	/* [한국어] 실제 복구는 잠들 수 있는 워크큐 문맥에서 한다 */
}

static int nvme_tcp_process_nvme_cqe(struct nvme_tcp_queue *queue,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		struct nvme_completion *cqe)	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
{
	struct nvme_tcp_request *req;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct request *rq;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	rq = nvme_find_rq(nvme_tcp_tagset(queue), cqe->command_id);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	if (!rq) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"got bad cqe.command_id %#x on queue %d\n",
			cqe->command_id, nvme_tcp_queue_id(queue));
		nvme_tcp_error_recovery(&queue->ctrl->ctrl);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		return -EINVAL;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}

	req = blk_mq_rq_to_pdu(rq);	/* [한국어] blk-mq — 태그·hctx·타임아웃·맵·완료 연동 */
	if (req->status == cpu_to_le16(NVME_SC_SUCCESS))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		req->status = cqe->status;

	if (!nvme_try_complete_req(rq, req->status, cqe->result))	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
		nvme_complete_rq(rq);
	queue->nr_cqe++;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static int nvme_tcp_handle_c2h_data(struct nvme_tcp_queue *queue,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		struct nvme_tcp_data_pdu *pdu)
{
	struct request *rq;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	rq = nvme_find_rq(nvme_tcp_tagset(queue), pdu->command_id);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	if (!rq) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"got bad c2hdata.command_id %#x on queue %d\n",
			pdu->command_id, nvme_tcp_queue_id(queue));
		return -ENOENT;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}

	if (!blk_rq_payload_bytes(rq)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"queue %d tag %#x unexpected data\n",
			nvme_tcp_queue_id(queue), rq->tag);
		return -EIO;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}

	queue->data_remaining = le32_to_cpu(pdu->data_length);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (pdu->hdr.flags & NVME_TCP_F_DATA_SUCCESS &&	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
	    unlikely(!(pdu->hdr.flags & NVME_TCP_F_DATA_LAST))) {
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"queue %d tag %#x SUCCESS set but not last PDU\n",
			nvme_tcp_queue_id(queue), rq->tag);
		nvme_tcp_error_recovery(&queue->ctrl->ctrl);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		return -EPROTO;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static int nvme_tcp_handle_comp(struct nvme_tcp_queue *queue,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		struct nvme_tcp_rsp_pdu *pdu)
{
	struct nvme_completion *cqe = &pdu->cqe;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	int ret = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	/*
	 * AEN requests are special as they don't time out and can
	 * survive any kind of queue freeze and often don't respond to
	 * aborts.  We don't even bother to allocate a struct request
	 * for them but rather special case them here.
	 */
	if (unlikely(nvme_is_aen_req(nvme_tcp_queue_id(queue),	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
				     cqe->command_id)))
		nvme_complete_async_event(&queue->ctrl->ctrl, cqe->status,
				&cqe->result);
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ret = nvme_tcp_process_nvme_cqe(queue, cqe);

	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

/*
 * [한국어]
 * nvme_tcp_setup_h2c_data_pdu - R2T 에 응답할 H2C 데이터 PDU 헤더를 만든다
 *
 * @req: R2T 를 받은 요청. h2cdata_left/offset 이 이미 세워져 있다.
 * @return: 없음
 *
 * 쓰기 데이터를 타겟이 요구한 만큼 보내기 위한 헤더를 짓는다. 한 번의 R2T
 * 분량이 maxh2cdata 보다 클 수 있으므로, 이 함수는 그 분량을 잘라 내는
 * 역할도 겸한다 -- 여러 번 불리며 매번 다음 조각의 헤더를 만든다.
 *
 * 첫 줄의 h2cdata_sent 가 미묘하다. pdu_len 은 아직 이전 조각의 길이를
 * 담고 있고, 그것이 곧 "직전에 보낸 양"이다. 그 값을 offset 에 더해 이번
 * 조각의 시작 위치를 구한 뒤에야 pdu_len 을 새로 덮어쓴다. 순서를 바꾸면
 * offset 이 어긋난다.
 *
 * DATA_LAST 플래그는 h2cdata_left 를 뺀 뒤에 판정한다. 즉 이번 조각으로
 * R2T 분량이 소진되는지를 보고, 그래야 타겟이 더 기다리지 않는다.
 *
 * plen 계산이 다이제스트 설정을 그대로 반영한다 -- 헤더 + 헤더 다이제스트 +
 * 데이터 + 데이터 다이제스트. 협상 결과에 따라 hdgst/ddgst 가 0 이 되므로
 * 같은 식이 두 경우를 모두 덮는다. pdo(데이터 오프셋)도 헤더 다이제스트
 * 길이를 포함해야 수신 측이 본문 시작 위치를 맞게 찾는다.
 *
 * ttag 를 되돌려 싣는 것이 R2T 와 이 PDU 를 짝짓는 유일한 수단이다.
 *
 * 실행 컨텍스트: 수신 파싱 문맥(handle_r2t) 또는 송신 문맥(try_send_data).
 *
 * 호출 체인:
 *   nvme_tcp_handle_r2t / nvme_tcp_try_send_data → [이 함수]
 */
static void nvme_tcp_setup_h2c_data_pdu(struct nvme_tcp_request *req)
{
	struct nvme_tcp_data_pdu *data = nvme_tcp_req_data_pdu(req);	/* [한국어] 요청에 딸린 PDU 헤더 버퍼 */
	struct nvme_tcp_queue *queue = req->queue;
	struct request *rq = blk_mq_rq_from_pdu(req);
	u32 h2cdata_sent = req->pdu_len;	/* [한국어] 아직 덮어쓰기 전이라 직전 조각의 길이다 — 이번 offset 을 구하는 근거 */
	u8 hdgst = nvme_tcp_hdgst_len(queue);	/* [한국어] 협상에 따라 0 또는 4 */
	u8 ddgst = nvme_tcp_ddgst_len(queue);

	req->state = NVME_TCP_SEND_H2C_PDU;	/* [한국어] 송신 상태 기계를 H2C 헤더 단계로 옮긴다 */
	req->offset = 0;	/* [한국어] 새 단계이므로 단계 내 오프셋을 되돌린다 */
	req->pdu_len = min(req->h2cdata_left, queue->maxh2cdata);	/* [한국어] 남은 R2T 분량을 타겟이 한 번에 받을 수 있는 크기로 자른다 */
	req->pdu_sent = 0;
	req->h2cdata_left -= req->pdu_len;	/* [한국어] 이번 조각만큼 남은 양을 줄인다 */
	req->h2cdata_offset += h2cdata_sent;	/* [한국어] 직전 조각 길이만큼 전진 — 위에서 미리 읽어 둔 값이 여기 쓰인다 */

	memset(data, 0, sizeof(*data));	/* [한국어] 재사용 버퍼라 이전 헤더가 남지 않게 지운다 */
	data->hdr.type = nvme_tcp_h2c_data;	/* [한국어] 호스트 → 컨트롤러 데이터 PDU */
	if (!req->h2cdata_left)	/* [한국어] 이번 조각으로 R2T 분량이 소진된다 */
		data->hdr.flags = NVME_TCP_F_DATA_LAST;	/* [한국어] 타겟이 더 기다리지 않도록 알린다 */
	if (queue->hdr_digest)
		data->hdr.flags |= NVME_TCP_F_HDGST;	/* [한국어] 헤더 뒤에 CRC 가 붙는다고 표시 */
	if (queue->data_digest)
		data->hdr.flags |= NVME_TCP_F_DDGST;	/* [한국어] 데이터 뒤에도 붙는다고 표시 */
	data->hdr.hlen = sizeof(*data);
	data->hdr.pdo = data->hdr.hlen + hdgst;	/* [한국어] 본문 시작 위치. 헤더 다이제스트 길이를 포함해야 수신 측이 맞게 찾는다 */
	data->hdr.plen =
		cpu_to_le32(data->hdr.hlen + hdgst + req->pdu_len + ddgst);	/* [한국어] 전체 길이 — 협상에 따라 hdgst/ddgst 가 0 이 되어 한 식으로 두 경우를 덮는다 */
	data->ttag = req->ttag;	/* [한국어] R2T 가 준 태그를 되돌려 어느 요구에 대한 응답인지 짝지어 준다 */
	data->command_id = nvme_cid(rq);
	data->data_offset = cpu_to_le32(req->h2cdata_offset);	/* [한국어] 요청 전체에서 이 조각이 시작하는 위치 */
	data->data_length = cpu_to_le32(req->pdu_len);
}

/*
 * [한국어]
 * nvme_tcp_handle_r2t - 타겟의 "쓰기 데이터를 보내라" 요구를 받아 송신을 예약한다
 *
 * @queue: 대상 큐
 * @pdu:   R2T(Ready to Transfer) PDU. 어느 요청의 어느 구간을 달라는지 담겨 있다.
 * @return: 0 이면 정상. 음수면 프로토콜 위반이며 연결을 끊는다.
 *
 * NVMe/TCP 의 쓰기는 두 걸음이다. 호스트가 명령을 보내면 타겟이 버퍼를 마련한
 * 뒤 R2T 로 "이 구간을 지금 보내라"고 알리고, 그제서야 호스트가 H2C 데이터
 * PDU 를 보낸다. 인라인으로 실어 보낸 분량을 뺀 나머지가 이 경로를 탄다.
 * 이 흐름 덕에 타겟이 자기 메모리 사정에 맞춰 흐름을 조절할 수 있다.
 *
 * 검사가 넷이나 있는 이유는 이 PDU 가 신뢰할 수 없는 원격 입력이기 때문이다.
 * 길이가 0 이거나, 요청 크기를 넘거나, 이미 보낸 지점보다 뒤로 돌아가는
 * 오프셋이거나, 이미 전송 목록에 올라 있는 요청에 대한 R2T 라면 모두
 * 프로토콜 위반이다. 특히 세 번째와 네 번째는 방치하면 같은 요청을 두 번
 * 전송 목록에 넣어 목록이 꼬이거나, 이미 보낸 데이터를 덮어쓰게 된다.
 *
 * 처리를 마치면 요청을 다시 입력 목록에 넣고 io_work 를 깨운다. 즉 R2T 를
 * 받은 요청은 새로 제출된 요청과 같은 경로로 다시 전송 대기에 들어간다.
 *
 * 실행 컨텍스트: 수신 파싱 문맥(lock_sock 보유). 잠들면 안 된다.
 *
 * 호출 체인:
 *   nvme_tcp_recv_pdu → [이 함수] → nvme_tcp_setup_h2c_data_pdu → io_work
 */
static int nvme_tcp_handle_r2t(struct nvme_tcp_queue *queue,
		struct nvme_tcp_r2t_pdu *pdu)
{
	struct nvme_tcp_request *req;
	struct request *rq;
	u32 r2t_length = le32_to_cpu(pdu->r2t_length);	/* [한국어] 타겟이 이번에 받겠다는 바이트 수 */
	u32 r2t_offset = le32_to_cpu(pdu->r2t_offset);	/* [한국어] 요청 전체에서 그 구간이 시작하는 위치 */

	rq = nvme_find_rq(nvme_tcp_tagset(queue), pdu->command_id);	/* [한국어] command_id 로 원래 요청을 되찾는다 */
	if (!rq) {
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 없는 태그를 가리킨다 — 타겟이 잘못 보냈거나 이미 완료된 요청이다 */
			"got bad r2t.command_id %#x on queue %d\n",
			pdu->command_id, nvme_tcp_queue_id(queue));
		return -ENOENT;
	}
	req = blk_mq_rq_to_pdu(rq);

	if (unlikely(!r2t_length)) {	/* [한국어] 0바이트를 요구하는 R2T 는 의미가 없다 */
		dev_err(queue->ctrl->ctrl.device,
			"req %d r2t len is %u, probably a bug...\n",
			rq->tag, r2t_length);
		return -EPROTO;
	}

	if (unlikely(req->data_sent + r2t_length > req->data_len)) {	/* [한국어] 요청이 가진 것보다 많이 요구한다 */
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 그대로 따르면 남의 메모리를 읽어 보내게 된다 */
			"req %d r2t len %u exceeded data len %u (%zu sent)\n",
			rq->tag, r2t_length, req->data_len, req->data_sent);
		return -EPROTO;
	}

	if (unlikely(r2t_offset < req->data_sent)) {	/* [한국어] 이미 보낸 지점보다 뒤로 돌아가라는 요구 */
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] iov_iter 는 전진만 하므로 되감을 수 없고, 따르면 데이터가 어긋난다 */
			"req %d unexpected r2t offset %u (expected %zu)\n",
			rq->tag, r2t_offset, req->data_sent);
		return -EPROTO;
	}

	if (llist_on_list(&req->lentry) ||	/* [한국어] 이 요청이 이미 입력 목록에 있거나 */
	    !list_empty(&req->entry)) {	/* [한국어] 이미 전송 목록에 올라 있다면 */
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 같은 요청을 두 번 넣게 되어 목록이 꼬인다 */
			"req %d unexpected r2t while processing request\n",
			rq->tag);
		return -EPROTO;
	}

	req->pdu_len = 0;	/* [한국어] 새 PDU 를 처음부터 조립하므로 이전 길이를 지운다 */
	req->h2cdata_left = r2t_length;	/* [한국어] 이번 R2T 분량. maxh2cdata 보다 크면 여러 PDU 로 쪼개 나간다 */
	req->h2cdata_offset = r2t_offset;
	req->ttag = pdu->ttag;	/* [한국어] 타겟이 준 전송 태그를 그대로 되돌려 어느 R2T 에 대한 응답인지 짝지어 준다 */

	nvme_tcp_setup_h2c_data_pdu(req);	/* [한국어] H2C 헤더를 만들고 전송 단계를 SEND_H2C_PDU 로 옮긴다 */

	llist_add(&req->lentry, &queue->req_list);	/* [한국어] 새 제출과 같은 경로로 전송 대기에 넣는다 */
	queue_work_on(queue->io_cpu, nvme_tcp_wq, &queue->io_work);	/* [한국어] 이 큐의 전용 CPU 에서 io_work 를 깨운다 */

	return 0;
}

static void nvme_tcp_handle_c2h_term(struct nvme_tcp_queue *queue,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		struct nvme_tcp_term_pdu *pdu)
{
	u16 fes;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	const char *msg;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	u32 plen = le32_to_cpu(pdu->hdr.plen);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	static const char * const msg_table[] = {	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		[NVME_TCP_FES_INVALID_PDU_HDR] = "Invalid PDU Header Field",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		[NVME_TCP_FES_PDU_SEQ_ERR] = "PDU Sequence Error",
		[NVME_TCP_FES_HDR_DIGEST_ERR] = "Header Digest Error",
		[NVME_TCP_FES_DATA_OUT_OF_RANGE] = "Data Transfer Out Of Range",
		[NVME_TCP_FES_DATA_LIMIT_EXCEEDED] = "Data Transfer Limit Exceeded",
		[NVME_TCP_FES_UNSUPPORTED_PARAM] = "Unsupported Parameter",
	};

	if (plen < NVME_TCP_MIN_C2HTERM_PLEN ||	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
	    plen > NVME_TCP_MAX_C2HTERM_PLEN) {
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"Received a malformed C2HTermReq PDU (plen = %u)\n",
			plen);
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}

	fes = le16_to_cpu(pdu->fes);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (fes && fes < ARRAY_SIZE(msg_table))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		msg = msg_table[fes];
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		msg = "Unknown";

	dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		"Received C2HTermReq (FES = %s)\n", msg);
}

/*
 * [한국어]
 * nvme_tcp_recv_pdu - PDU 헤더를 모으고, 다 모이면 종류에 따라 분기한다
 *
 * @queue:  대상 큐
 * @skb:    도착한 소켓 버퍼
 * @offset: skb 안 현재 위치(이 함수가 소비한 만큼 전진시킨다)
 * @len:    남은 바이트 수(마찬가지로 줄여 준다)
 * @return: 0 이면 정상. 음수면 프로토콜 위반이나 검증 실패.
 *
 * 스트림에서 메시지를 되찾는 첫 단계다. 헤더가 다 모이기 전에는 아무 판단도
 * 할 수 없다 -- 본문 길이가 헤더 안에 있기 때문이다. 그래서 pdu_remaining 이
 * 0 이 될 때까지는 모으기만 하고 0 을 돌려준다(오류가 아니라 "아직"이라는 뜻).
 *
 * 헤더가 완성되면 순서대로 검사한다:
 *   1) 헤더 길이가 기대와 맞는가 -- 알려진 타입인지 먼저 보고 메시지를 가른다.
 *   2) C2HTermReq 인가 -- 위 영어 주석대로 이것만은 다이제스트를 싣지 않으므로
 *      검증을 건너뛰어야 한다. 순서를 바꾸면 종료 통지를 다이제스트 오류로
 *      오인하게 된다.
 *   3) 헤더/데이터 다이제스트 검증.
 *   4) 타입별 처리로 분기.
 *
 * rsp 와 r2t 에서 init_recv_ctx 를 부르는 이유: 이 둘은 뒤따르는 본문이 없어
 * 곧바로 다음 PDU 헤더를 기다리는 상태로 되돌려야 한다. c2h_data 는 본문이
 * 이어지므로 그쪽 핸들러가 상태를 DATA 로 옮긴다.
 *
 * 호출 체인:
 *   nvme_tcp_recv_skb → [이 함수] → handle_c2h_data / handle_comp / handle_r2t
 */
static int nvme_tcp_recv_pdu(struct nvme_tcp_queue *queue, struct sk_buff *skb,
		unsigned int *offset, size_t *len)
{
	struct nvme_tcp_hdr *hdr;
	char *pdu = queue->pdu;	/* [한국어] 큐가 들고 있는 헤더 조립 버퍼 */
	size_t rcv_len = min_t(size_t, *len, queue->pdu_remaining);	/* [한국어] 이번에 채울 수 있는 만큼만 — 헤더를 넘겨 읽으면 다음 PDU 를 먹는다 */
	int ret;

	ret = skb_copy_bits(skb, *offset,	/* [한국어] skb 에서 조립 버퍼로 복사 */
		&pdu[queue->pdu_offset], rcv_len);	/* [한국어] 이어붙일 위치는 지금까지 모은 만큼 뒤 */
	if (unlikely(ret))
		return ret;

	queue->pdu_remaining -= rcv_len;	/* [한국어] 헤더에서 아직 못 받은 양을 줄인다 */
	queue->pdu_offset += rcv_len;	/* [한국어] 다음에 이어붙일 위치를 전진 */
	*offset += rcv_len;	/* [한국어] 호출자의 skb 위치도 함께 전진시킨다 */
	*len -= rcv_len;
	if (queue->pdu_remaining)	/* [한국어] 헤더가 아직 다 안 왔다 — 오류가 아니라 "다음 skb 를 기다린다" */
		return 0;

	hdr = queue->pdu;	/* [한국어] 이제 헤더 전체가 모였으니 해석할 수 있다 */
	if (unlikely(hdr->hlen != sizeof(struct nvme_tcp_rsp_pdu))) {	/* [한국어] 기대한 헤더 길이와 다르다 */
		if (!nvme_tcp_recv_pdu_supported(hdr->type))	/* [한국어] 아예 모르는 타입이면 길이 얘기를 할 필요가 없다 */
			goto unsupported_pdu;

		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 아는 타입인데 길이가 틀렸다 — 프로토콜 위반이다 */
			"pdu type %d has unexpected header length (%d)\n",
			hdr->type, hdr->hlen);
		return -EPROTO;
	}

	if (unlikely(hdr->type == nvme_tcp_c2h_term)) {
		/*
		 * C2HTermReq never includes Header or Data digests.
		 * Skip the checks.
		 */
		/* [한국어] 위 영어 주석대로 종료 통지에는 다이제스트가 없다. 아래 검증보다
		 * 먼저 처리해야 정상적인 종료 통지를 다이제스트 오류로 오인하지 않는다. */
		nvme_tcp_handle_c2h_term(queue, (void *)queue->pdu);
		return -EINVAL;	/* [한국어] 타겟이 연결을 끝내겠다는 뜻이므로 오류로 올려 복구 경로를 태운다 */
	}

	if (queue->hdr_digest) {	/* [한국어] 헤더 다이제스트를 협상한 연결이면 */
		ret = nvme_tcp_verify_hdgst(queue, queue->pdu, hdr->hlen);	/* [한국어] 헤더 뒤에 붙은 CRC32C 를 확인 */
		if (unlikely(ret))
			return ret;
	}


	if (queue->data_digest) {	/* [한국어] 데이터 다이제스트를 쓰는 연결이면 */
		ret = nvme_tcp_check_ddgst(queue, queue->pdu);	/* [한국어] 이 PDU 가 다이제스트를 실어야 하는 종류인지 확인하고 준비한다 */
		if (unlikely(ret))
			return ret;
	}

	switch (hdr->type) {	/* [한국어] 헤더가 온전하다 — 이제 종류별로 갈라 처리한다 */
	case nvme_tcp_c2h_data:
		return nvme_tcp_handle_c2h_data(queue, (void *)queue->pdu);	/* [한국어] 데이터가 뒤따른다. 핸들러가 상태를 RECV_DATA 로 옮긴다 */
	case nvme_tcp_rsp:
		nvme_tcp_init_recv_ctx(queue);	/* [한국어] 뒤따르는 본문이 없으므로 곧바로 다음 헤더를 기다리는 상태로 되돌린다 */
		return nvme_tcp_handle_comp(queue, (void *)queue->pdu);	/* [한국어] 응답 캡슐 — 요청을 완료시킨다 */
	case nvme_tcp_r2t:
		nvme_tcp_init_recv_ctx(queue);	/* [한국어] 마찬가지로 본문이 없다 */
		return nvme_tcp_handle_r2t(queue, (void *)queue->pdu);	/* [한국어] "쓰기 데이터를 보내라"는 요구 — 송신 쪽에 H2C 단계를 예약한다 */
	default:
		goto unsupported_pdu;
	}

unsupported_pdu:
	dev_err(queue->ctrl->ctrl.device,
		"unsupported pdu type (%d)\n", hdr->type);
	return -EINVAL;	/* [한국어] 해석할 수 없는 PDU 는 스트림 동기를 잃은 것과 같아 연결을 끊는다 */
}

static inline void nvme_tcp_end_request(struct request *rq, u16 status)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	union nvme_result res = {};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!nvme_try_complete_req(rq, cpu_to_le16(status << 1), res))	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
		nvme_complete_rq(rq);
}

/*
 * [한국어]
 * nvme_tcp_recv_data - C2H 데이터 PDU 의 본문을 요청 버퍼로 흘려 넣는다
 *
 * @queue:  대상 큐
 * @skb:    도착한 소켓 버퍼
 * @offset: skb 안 현재 위치(소비한 만큼 전진시킨다)
 * @len:    남은 바이트 수(줄여 준다)
 * @return: 0 이면 정상. 음수면 복사 실패이거나 컨트롤러가 요청보다 많이 보냈다.
 *
 * 읽기 요청의 데이터가 도착하는 자리다. 어려운 점은 두 겹의 경계가 서로
 * 맞지 않는다는 것이다 -- skb 경계와 bio 경계 어느 쪽도 데이터 길이와
 * 일치하지 않는다. 그래서 while 루프가 "이번 skb 에 남은 양"과 "지금 bio 에
 * 남은 자리" 중 작은 쪽씩 잘라 옮기고, bio 가 소진되면 다음 bio 로 넘어간다.
 *
 * curr_bio 가 NULL 이 되는 경우가 오류인 이유는 위 영어 주석이 밝힌다 --
 * 우리가 요청한 것보다 컨트롤러가 많이 보냈다는 뜻이다. 그대로 두면 요청
 * 버퍼를 넘겨 쓰게 되므로 여기서 끊는다.
 *
 * 다이제스트를 쓰면 복사와 CRC 계산을 한 번에 하는 함수를 쓴다. 데이터를
 * 두 번 훑지 않기 위해서다.
 *
 * 데이터를 다 받은 뒤가 갈림길이다. 다이제스트를 쓰면 아직 끝이 아니라
 * 4바이트를 더 기다려야 하므로 RECV_DDGST 로 넘어간다. 쓰지 않는다면
 * SUCCESS 플래그가 붙은 경우에 한해 여기서 곧바로 요청을 완료시킨다 --
 * 별도의 응답 PDU 없이 데이터 전송으로 완료를 갈음하는 최적화다.
 *
 * 실행 컨텍스트: 수신 파싱 문맥(lock_sock 보유).
 *
 * 호출 체인:
 *   nvme_tcp_recv_skb → [이 함수] → skb_copy_datagram_iter
 */
static int nvme_tcp_recv_data(struct nvme_tcp_queue *queue, struct sk_buff *skb,
			      unsigned int *offset, size_t *len)
{
	struct nvme_tcp_data_pdu *pdu = (void *)queue->pdu;	/* [한국어] 앞서 조립해 둔 C2H 데이터 헤더 */
	struct request *rq =
		nvme_cid_to_rq(nvme_tcp_tagset(queue), pdu->command_id);	/* [한국어] 헤더의 command_id 로 원래 요청을 되찾는다 */
	struct nvme_tcp_request *req = blk_mq_rq_to_pdu(rq);

	while (true) {
		int recv_len, ret;

		recv_len = min_t(size_t, *len, queue->data_remaining);	/* [한국어] 이번 skb 에 남은 양과 아직 받아야 할 양 중 작은 쪽 */
		if (!recv_len)
			break;	/* [한국어] 이 skb 를 다 썼거나 본문을 다 받았다 */

		if (!iov_iter_count(&req->iter)) {	/* [한국어] 지금 bio 에 더 쓸 자리가 없다 */
			req->curr_bio = req->curr_bio->bi_next;	/* [한국어] 다음 bio 로 넘어간다 */

			/*
			 * If we don't have any bios it means the controller
			 * sent more data than we requested, hence error
			 */
			if (!req->curr_bio) {	/* [한국어] 위 영어 주석대로 요청한 것보다 많이 보냈다는 뜻이다 */
				dev_err(queue->ctrl->ctrl.device,	/* [한국어] 그대로 두면 요청 버퍼를 넘겨 쓴다 */
					"queue %d no space in request %#x",
					nvme_tcp_queue_id(queue), rq->tag);
				nvme_tcp_init_recv_ctx(queue);	/* [한국어] 수신 상태를 헤더 대기로 되돌린다 */
				return -EIO;
			}
			nvme_tcp_init_iter(req, ITER_DEST);	/* [한국어] 새 bio 를 가리키도록 반복자를 다시 세운다 */
		}

		/* we can read only from what is left in this bio */
		recv_len = min_t(size_t, recv_len,
				iov_iter_count(&req->iter));	/* [한국어] bio 경계를 넘지 않도록 한 번 더 자른다 */

		if (queue->data_digest)	/* [한국어] 다이제스트를 쓰면 */
			ret = skb_copy_and_crc32c_datagram_iter(skb, *offset,	/* [한국어] 복사와 CRC 계산을 한 번에 — 데이터를 두 번 훑지 않는다 */
				&req->iter, recv_len, &queue->rcv_crc);
		else
			ret = skb_copy_datagram_iter(skb, *offset,	/* [한국어] 복사만. 반복자가 쓴 만큼 자동으로 전진한다 */
					&req->iter, recv_len);
		if (ret) {
			dev_err(queue->ctrl->ctrl.device,
				"queue %d failed to copy request %#x data",
				nvme_tcp_queue_id(queue), rq->tag);
			return ret;
		}

		*len -= recv_len;	/* [한국어] 호출자의 skb 잔량과 위치를 함께 갱신한다 */
		*offset += recv_len;
		queue->data_remaining -= recv_len;	/* [한국어] 이 PDU 에서 아직 받아야 할 양 */
	}

	if (!queue->data_remaining) {	/* [한국어] 본문을 다 받았다 — 여기서 갈린다 */
		if (queue->data_digest) {
			queue->exp_ddgst = nvme_tcp_ddgst_final(queue->rcv_crc);	/* [한국어] 우리가 계산한 값을 확정해 둔다 */
			queue->ddgst_remaining = NVME_TCP_DIGEST_LENGTH;	/* [한국어] 아직 끝이 아니다 — 4바이트를 더 받아 대조해야 한다 */
		} else {
			if (pdu->hdr.flags & NVME_TCP_F_DATA_SUCCESS) {	/* [한국어] 컨트롤러가 "이 데이터로 완료를 갈음한다"고 표시했다 */
				nvme_tcp_end_request(rq,	/* [한국어] 별도 응답 PDU 없이 여기서 요청을 끝낸다 — 왕복 하나를 아끼는 최적화다 */
						le16_to_cpu(req->status));
				queue->nr_cqe++;	/* [한국어] poll 이 돌려줄 완료 개수를 센다 */
			}
			nvme_tcp_init_recv_ctx(queue);	/* [한국어] 다음 PDU 헤더를 기다리는 상태로 되돌린다 */
		}
	}

	return 0;
}

static int nvme_tcp_recv_ddgst(struct nvme_tcp_queue *queue,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		struct sk_buff *skb, unsigned int *offset, size_t *len)
{
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
					pdu->command_id);
		struct nvme_tcp_request *req = blk_mq_rq_to_pdu(rq);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

		req->status = cpu_to_le16(NVME_SC_DATA_XFER_ERROR);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"data digest error: recv %#x expected %#x\n",
			le32_to_cpu(queue->recv_ddgst),
			le32_to_cpu(queue->exp_ddgst));
	}

	if (pdu->hdr.flags & NVME_TCP_F_DATA_SUCCESS) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		struct request *rq = nvme_cid_to_rq(nvme_tcp_tagset(queue),	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
					pdu->command_id);
		struct nvme_tcp_request *req = blk_mq_rq_to_pdu(rq);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

		nvme_tcp_end_request(rq, le16_to_cpu(req->status));	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		queue->nr_cqe++;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}

	nvme_tcp_init_recv_ctx(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

/*
 * [한국어]
 * nvme_tcp_recv_skb - 도착한 skb 하나를 수신 상태 기계에 밀어 넣는다
 *
 * @desc:   read_sock 이 넘긴 문맥. arg.data 에 큐가 들어 있다.
 * @skb:    도착한 소켓 버퍼
 * @offset: skb 안에서 아직 처리하지 않은 시작 위치
 * @len:    그 위치부터 남은 바이트 수
 * @return: 소비한 바이트 수. 음수면 오류이며 read_sock 이 멈춘다.
 *
 * TCP 에 메시지 경계가 없다는 사실이 이 while 루프의 존재 이유다. 한 skb 안에
 * PDU 가 몇 개 들어 있을지, 혹은 PDU 하나가 몇 개의 skb 에 걸쳐 있을지 알 수
 * 없다. 그래서 큐가 들고 있는 "지금 무엇을 모으는 중인가"(recv_state)에 따라
 * 남은 바이트를 나눠 담고, 다 쓸 때까지 반복한다. 상태는 skb 사이를 넘어
 * 유지되므로 PDU 가 잘려 도착해도 이어서 조립된다.
 *
 * 오류를 만나면 rd_enabled 를 내리는 것이 중요하다. 스트림 파싱이 어긋난
 * 시점에서 이후 바이트는 의미가 없고, 콜백이 io_work 를 다시 깨우면 깨진
 * 상태로 계속 읽게 된다. 그래서 수신을 봉인하고 복구 경로로 넘긴다.
 *
 * 실행 컨텍스트: read_sock 안, lock_sock 을 잡은 상태. 잠들면 안 된다.
 *
 * 호출 체인:
 *   nvme_tcp_try_recv → read_sock → [이 함수] → recv_pdu / recv_data / recv_ddgst
 */
static int nvme_tcp_recv_skb(read_descriptor_t *desc, struct sk_buff *skb,
			     unsigned int offset, size_t len)
{
	struct nvme_tcp_queue *queue = desc->arg.data;	/* [한국어] try_recv 가 심어 둔 큐를 되찾는다 */
	size_t consumed = len;	/* [한국어] 전부 소비하는 것이 정상 — 성공 시 이 값을 돌려준다 */
	int result;

	if (unlikely(!queue->rd_enabled))	/* [한국어] 큐가 내려가는 중이면 더 파싱하지 않는다 */
		return -EFAULT;

	while (len) {	/* [한국어] 이 skb 의 바이트를 다 쓸 때까지. PDU 여러 개가 들어 있을 수 있다 */
		switch (nvme_tcp_recv_state(queue)) {	/* [한국어] 큐가 기억하는 현재 단계에 따라 나눠 담는다 */
		case NVME_TCP_RECV_PDU:
			result = nvme_tcp_recv_pdu(queue, skb, &offset, &len);	/* [한국어] 헤더를 모은다. 다 모이면 그 안에서 다음 단계가 정해진다 */
			break;
		case NVME_TCP_RECV_DATA:
			result = nvme_tcp_recv_data(queue, skb, &offset, &len);	/* [한국어] 헤더가 알려 준 길이만큼 본문을 받는다 */
			break;
		case NVME_TCP_RECV_DDGST:
			result = nvme_tcp_recv_ddgst(queue, skb, &offset, &len);	/* [한국어] 다이제스트 4바이트를 받아 검증한다 */
			break;
		default:
			result = -EFAULT;	/* [한국어] 있을 수 없는 상태 — 방어적으로 오류 처리 */
		}
		if (result) {
			dev_err(queue->ctrl->ctrl.device,
				"receive failed:  %d\n", result);
			queue->rd_enabled = false;	/* [한국어] 파싱이 어긋난 뒤의 바이트는 의미가 없다. 수신을 봉인해 깨진 상태로 계속 읽는 것을 막는다 */
			nvme_tcp_error_recovery(&queue->ctrl->ctrl);	/* [한국어] 큐를 내리고 재연결을 시도하는 복구 경로로 넘긴다 */
			return result;
		}
	}

	return consumed;
}

static void nvme_tcp_data_ready(struct sock *sk)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	struct nvme_tcp_queue *queue;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	trace_sk_data_ready(sk);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	read_lock_bh(&sk->sk_callback_lock);	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
	queue = sk->sk_user_data;	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
	if (likely(queue && queue->rd_enabled) &&	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
	    !test_bit(NVME_TCP_Q_POLLING, &queue->flags))
		queue_work_on(queue->io_cpu, nvme_tcp_wq, &queue->io_work);
	read_unlock_bh(&sk->sk_callback_lock);	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
}

static void nvme_tcp_write_space(struct sock *sk)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	struct nvme_tcp_queue *queue;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	read_lock_bh(&sk->sk_callback_lock);	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
	queue = sk->sk_user_data;	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
	if (likely(queue && sk_stream_is_writeable(sk))) {	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
		clear_bit(SOCK_NOSPACE, &sk->sk_socket->flags);	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
		/* Ensure pending TLS partial records are retried */
		if (nvme_tcp_queue_tls(queue))	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
			queue->write_space(sk);
		queue_work_on(queue->io_cpu, nvme_tcp_wq, &queue->io_work);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	}
	read_unlock_bh(&sk->sk_callback_lock);	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
}

/*
 * [한국어]
 * nvme_tcp_state_change - 소켓 상태가 바뀌었다는 커널 통지를 받는다
 *
 * @sk: 상태가 바뀐 소켓
 * @return: 없음
 *
 * 이 드라이버가 소켓에 심어 둔 세 콜백 중 하나다. 원래 소켓이 갖고 있던
 * 콜백은 queue->state_change 에 보관해 두었다가 아래에서 반드시 이어서
 * 부른다 -- 네트워크 스택 자신의 처리를 가로채면 안 되기 때문이다.
 *
 * 여기서 잡아내는 것은 연결이 닫히는 모든 경로다. 상대가 FIN 을 보냈든,
 * 이쪽이 먼저 닫는 중이든, 어느 상태로 들어가도 이 큐로는 더 이상 NVMe 를
 * 주고받을 수 없다. 그래서 전부 같은 복구 경로로 보낸다. TCP 상태 기계의
 * 세부는 여기서 중요하지 않고, "닫히는 중인가 아닌가"만 중요하다.
 *
 * sk_user_data 가 NULL 인 경우를 먼저 거르는 이유: 큐를 내리면서 콜백을
 * 되돌리는 도중에 이 통지가 들어올 수 있다. 그때 큐를 참조하면 이미 해제된
 * 메모리를 만진다.
 *
 * read_lock_bh 로 감싸는 것은 소켓 콜백의 관례다. 콜백 포인터가 바뀌는
 * 동안(큐 해제 시) 그것을 읽는 쪽과 충돌하지 않게 한다.
 *
 * 실행 컨텍스트: 네트워크 스택의 소프트IRQ 문맥. 잠들면 안 된다 --
 * 그래서 error_recovery 가 워크큐로 넘기는 얕은 함수여야 한다.
 *
 * 호출 체인:
 *   네트워크 스택(TCP 상태 전이) → [이 함수] → nvme_tcp_error_recovery
 */
static void nvme_tcp_state_change(struct sock *sk)
{
	struct nvme_tcp_queue *queue;

	read_lock_bh(&sk->sk_callback_lock);	/* [한국어] 콜백 포인터와 user_data 가 바뀌는 동안 읽지 않도록 보호 */
	queue = sk->sk_user_data;
	if (!queue)	/* [한국어] 큐를 내리며 콜백을 되돌리는 중에 통지가 들어온 경우 */
		goto done;	/* [한국어] 참조하면 해제된 메모리를 만진다 */

	switch (sk->sk_state) {
	case TCP_CLOSE:
	case TCP_CLOSE_WAIT:
	case TCP_LAST_ACK:
	case TCP_FIN_WAIT1:
	case TCP_FIN_WAIT2:	/* [한국어] 닫히는 경로는 어느 상태든 결과가 같다 — 이 큐로는 더 이상 주고받을 수 없다 */
		nvme_tcp_error_recovery(&queue->ctrl->ctrl);	/* [한국어] 소프트IRQ 문맥이라 상태만 옮기고 워크큐로 넘긴다 */
		break;
	default:
		dev_info(queue->ctrl->ctrl.device,	/* [한국어] 그 밖의 전이는 기록만 한다 */
			"queue %d socket state %d\n",
			nvme_tcp_queue_id(queue), sk->sk_state);
	}

	queue->state_change(sk);	/* [한국어] 원래 콜백을 반드시 이어서 부른다 — 네트워크 스택 자신의 처리를 가로채면 안 된다 */
done:
	read_unlock_bh(&sk->sk_callback_lock);
}

static inline void nvme_tcp_done_send_req(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	queue->request = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}

static void nvme_tcp_fail_request(struct nvme_tcp_request *req)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	if (nvme_tcp_async_req(req)) {	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		union nvme_result res = {};	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		nvme_complete_async_event(&req->queue->ctrl->ctrl,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				cpu_to_le16(NVME_SC_HOST_PATH_ERROR), &res);
	} else {
		nvme_tcp_end_request(blk_mq_rq_from_pdu(req),	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
				NVME_SC_HOST_PATH_ERROR);
	}
}

/*
 * [한국어]
 * nvme_tcp_try_send_data - 데이터 본문을 소켓이 받아 주는 만큼 내보낸다
 *
 * @req: 전송 중인 요청. iter 가 어디까지 보냈는지 기억하고 있다.
 * @return: 1 이면 이번 PDU 의 데이터를 다 보냈다. 0 이하면 소켓이 더 받지
 *          못했거나 오류다(-EAGAIN 포함).
 *
 * 페이지 단위로 잘라 보내는 루프다. 한 번의 sock_sendmsg 가 한 페이지 조각을
 * 맡고, 소켓이 부분만 받으면 그만큼만 진행한 채 물러난다.
 *
 * MSG_SPLICE_PAGES 가 이 경로의 핵심이다. 페이지를 복사하지 않고 소켓 버퍼에
 * 참조로 붙여 제로카피로 내보낸다. 다만 모든 페이지가 그럴 수 있는 것은
 * 아니라서(슬랩 메모리처럼 참조를 붙이면 안 되는 페이지가 있다)
 * sendpages_ok 로 확인하고 안 되면 그 플래그를 떼어 복사 경로로 돌린다.
 *
 * MSG_EOR 과 MSG_MORE 의 구분은 네트워크 스택에 "이 다음에 더 보낼 것이
 * 있는가"를 알려 준다. 더 있으면 MSG_MORE 로 묶어 보내 작은 패킷이 여러 개
 * 나가는 것을 막고, 마지막이면 MSG_EOR 로 즉시 밀어낸다. 다이제스트가 남았거나
 * 큐에 다른 요청이 대기 중이면 아직 마지막이 아니다.
 *
 * 가장 미묘한 부분은 iter 를 마지막 전송에서만 갱신하지 않는 것이다. 위
 * 영어 주석이 이유를 밝힌다 -- 마지막 조각을 보내고 나면 수신 경로가 곧바로
 * 응답을 받아 요청을 완료시킬 수 있다. 그때 이쪽이 iter 를 만지면 이미
 * 반환된 요청을 건드리는 셈이 된다. 그래서 마지막만은 손대지 않는다.
 *
 * 실행 컨텍스트: io_work 또는 poll. send_mutex 는 호출자가 잡고 있다.
 *
 * 호출 체인:
 *   nvme_tcp_try_send → [이 함수] → sock_sendmsg
 */
static int nvme_tcp_try_send_data(struct nvme_tcp_request *req)
{
	struct nvme_tcp_queue *queue = req->queue;
	int req_data_len = req->data_len;	/* [한국어] 지역 복사 — 아래에서 요청이 완료돼 사라질 수 있어 미리 읽어 둔다 */
	u32 h2cdata_left = req->h2cdata_left;	/* [한국어] 같은 이유로 미리 읽는다 */

	while (true) {
		struct bio_vec bvec;
		struct msghdr msg = {
			.msg_flags = MSG_DONTWAIT | MSG_SPLICE_PAGES,	/* [한국어] 잠들지 않고, 페이지를 복사 대신 참조로 붙여 제로카피로 보낸다 */
		};
		struct page *page = nvme_tcp_req_cur_page(req);	/* [한국어] iter 가 가리키는 현재 페이지 */
		size_t offset = nvme_tcp_req_cur_offset(req);
		size_t len = nvme_tcp_req_cur_length(req);	/* [한국어] 이 페이지에서 보낼 수 있는 길이 */
		bool last = nvme_tcp_pdu_last_send(req, len);	/* [한국어] 이번 PDU 의 마지막 조각인가 */
		int req_data_sent = req->data_sent;	/* [한국어] 갱신 전 값 — 아래 판정에 쓴다 */
		int ret;

		if (last && !queue->data_digest && !nvme_tcp_queue_more(queue))	/* [한국어] 마지막이고 다이제스트도 대기 요청도 없다면 */
			msg.msg_flags |= MSG_EOR;	/* [한국어] 여기서 끝 — 즉시 밀어낸다 */
		else
			msg.msg_flags |= MSG_MORE;	/* [한국어] 더 보낼 것이 있으니 묶어라 — 작은 패킷이 여러 개 나가는 것을 막는다 */

		if (!sendpages_ok(page, len, offset))	/* [한국어] 참조로 붙이면 안 되는 페이지(슬랩 등)인지 확인 */
			msg.msg_flags &= ~MSG_SPLICE_PAGES;	/* [한국어] 그러면 제로카피를 포기하고 복사 경로로 돌린다 */

		bvec_set_page(&bvec, page, len, offset);
		iov_iter_bvec(&msg.msg_iter, ITER_SOURCE, &bvec, 1, len);
		ret = sock_sendmsg(queue->sock, &msg);	/* [한국어] 소켓이 받아 주는 만큼만 나간다 */
		if (ret <= 0)
			return ret;	/* [한국어] -EAGAIN 이면 호출자가 0 으로 바꿔 배압으로 처리한다 */

		if (queue->data_digest)	/* [한국어] 보낸 만큼 CRC 를 누적한다 */
			nvme_tcp_ddgst_update(&queue->snd_crc, page,
					offset, ret);

		/*
		 * update the request iterator except for the last payload send
		 * in the request where we don't want to modify it as we may
		 * compete with the RX path completing the request.
		 */
		/* [한국어] 위 영어 주석이 이 파일에서 가장 미묘한 경합을 설명한다.
		 * 마지막 조각을 보내고 나면 수신 경로가 곧바로 응답을 받아 요청을
		 * 완료시킬 수 있다. 그때 여기서 iter 를 만지면 이미 반환된 요청을
		 * 건드리게 되므로, 마지막만은 손대지 않는다. */
		if (req_data_sent + ret < req_data_len)	/* [한국어] 아직 마지막이 아닐 때만 */
			nvme_tcp_advance_req(req, ret);	/* [한국어] iter 와 data_sent 를 전진시킨다 */

		/* fully successful last send in current PDU */
		if (last && ret == len) {	/* [한국어] 이번 PDU 의 마지막 조각을 온전히 보냈다 */
			if (queue->data_digest) {
				req->ddgst =	/* [한국어] 누적한 CRC 를 확정하고 */
					nvme_tcp_ddgst_final(queue->snd_crc);
				req->state = NVME_TCP_SEND_DDGST;	/* [한국어] 다이제스트 4바이트를 보내는 단계로 넘어간다 */
				req->offset = 0;
			} else {
				if (h2cdata_left)	/* [한국어] R2T 분량이 아직 남았다 */
					nvme_tcp_setup_h2c_data_pdu(req);	/* [한국어] 다음 H2C PDU 헤더를 준비한다 */
				else
					nvme_tcp_done_send_req(queue);	/* [한국어] 이 요청의 전송이 끝났다 — 큐의 현재 요청 자리를 비운다 */
			}
			return 1;	/* [한국어] 진전이 있었다 — 호출자가 더 보낼 것이 있는지 다시 본다 */
		}
	}
	return -EAGAIN;	/* [한국어] 위 while(true) 때문에 도달하지 않지만, 컴파일러 경고를 막는다 */
}

/*
 * [한국어]
 * nvme_tcp_try_send_cmd_pdu - 명령 PDU 헤더를 소켓이 받아 주는 만큼 보낸다
 *
 * @req: 전송 중인 요청. offset 이 헤더의 어디까지 나갔는지 기억한다.
 * @return: 1 이면 헤더를 다 보냈다. -EAGAIN 이면 부분만 나가 다음 기회를
 *          기다린다. 0 이하의 다른 값은 소켓 오류.
 *
 * 송신 상태 기계의 첫 단계다. 요청마다 반드시 이 단계를 지나며, 여기가
 * 끝난 뒤 인라인 데이터가 있으면 DATA 로, 없으면 전송 자체가 끝난다.
 *
 * offset 이 부분 전송을 이어 가는 장치다. 남은 길이를 매번 offset 으로부터
 * 다시 계산하므로, 소켓이 절반만 받아 가도 다음 호출이 정확히 그 지점부터
 * 잇는다.
 *
 * 헤더 다이제스트를 offset 이 0 일 때만 계산하는 이유: 헤더 내용은 이미
 * 확정돼 있으므로 CRC 도 한 번만 구하면 된다. 재진입할 때마다 다시 구하면
 * 낭비일 뿐 아니라, 이미 일부가 나간 헤더를 다시 건드리게 된다.
 *
 * MSG_MORE 와 MSG_EOR 의 선택은 뒤에 더 보낼 것이 있는지로 갈린다.
 * 인라인 데이터가 이어지거나 큐에 다른 요청이 대기 중이면 아직 끝이 아니다.
 *
 * data_digest 를 쓰는 경우 여기서 CRC 시드를 심는다. 데이터 단계로 넘어가기
 * 직전이 그 초기화의 유일하게 옳은 시점이다.
 *
 * 실행 컨텍스트: io_work 또는 poll. send_mutex 는 호출자가 잡는다.
 *
 * 호출 체인:
 *   nvme_tcp_try_send → [이 함수] → sock_sendmsg
 */
static int nvme_tcp_try_send_cmd_pdu(struct nvme_tcp_request *req)
{
	struct nvme_tcp_queue *queue = req->queue;
	struct nvme_tcp_cmd_pdu *pdu = nvme_tcp_req_cmd_pdu(req);
	struct bio_vec bvec;
	struct msghdr msg = { .msg_flags = MSG_DONTWAIT | MSG_SPLICE_PAGES, };	/* [한국어] 잠들지 않고, 헤더 페이지를 참조로 붙여 보낸다 */
	bool inline_data = nvme_tcp_has_inline_data(req);	/* [한국어] 이 헤더 뒤에 데이터가 이어지는가 */
	u8 hdgst = nvme_tcp_hdgst_len(queue);	/* [한국어] 협상에 따라 0 또는 4 */
	int len = sizeof(*pdu) + hdgst - req->offset;	/* [한국어] 아직 안 나간 만큼 — offset 이 부분 전송을 이어 가는 장치다 */
	int ret;

	if (inline_data || nvme_tcp_queue_more(queue))	/* [한국어] 데이터가 이어지거나 대기 요청이 있으면 */
		msg.msg_flags |= MSG_MORE;	/* [한국어] 묶어 보내 작은 패킷이 여러 개 나가는 것을 막는다 */
	else
		msg.msg_flags |= MSG_EOR;	/* [한국어] 여기서 끝 — 즉시 밀어낸다 */

	if (queue->hdr_digest && !req->offset)	/* [한국어] offset 0 일 때만 — 헤더는 확정돼 있어 CRC 를 한 번만 구하면 된다 */
		nvme_tcp_set_hdgst(pdu, sizeof(*pdu));	/* [한국어] 재진입 때 다시 구하면 이미 나간 헤더를 건드리게 된다 */

	bvec_set_virt(&bvec, (void *)pdu + req->offset, len);	/* [한국어] 남은 구간만 가리킨다 */
	iov_iter_bvec(&msg.msg_iter, ITER_SOURCE, &bvec, 1, len);
	ret = sock_sendmsg(queue->sock, &msg);	/* [한국어] 받아 주는 만큼만 나간다 */
	if (unlikely(ret <= 0))
		return ret;

	len -= ret;
	if (!len) {	/* [한국어] 헤더를 온전히 보냈다 */
		if (inline_data) {
			req->state = NVME_TCP_SEND_DATA;	/* [한국어] 데이터 단계로 넘어간다 */
			if (queue->data_digest)
				queue->snd_crc = NVME_TCP_CRC_SEED;	/* [한국어] 데이터 CRC 누적을 여기서 시작한다 — 데이터 직전이 유일하게 옳은 시점이다 */
		} else {
			nvme_tcp_done_send_req(queue);	/* [한국어] 보낼 것이 없다 — 이 요청의 전송이 끝났다 */
		}
		return 1;	/* [한국어] 진전이 있었다 */
	}
	req->offset += ret;	/* [한국어] 나간 만큼 전진시켜 다음 호출이 이어 가게 한다 */

	return -EAGAIN;	/* [한국어] 부분만 나갔다 — 호출자가 0 으로 바꿔 배압으로 처리한다 */
}

/*
 * [한국어]
 * nvme_tcp_try_send_data_pdu - H2C 데이터 PDU 헤더를 보낸다
 *
 * @req: R2T 에 응답 중인 요청
 * @return: 1 이면 헤더를 다 보냈다. -EAGAIN 이면 부분 전송. 0 이하는 오류.
 *
 * 명령 PDU 헤더와 구조가 같지만 두 가지가 다르다.
 *
 * 첫째, MSG_EOR 이 없다. 이 헤더 뒤에는 반드시 데이터가 이어지므로 항상
 * MSG_MORE 다. 그래서 플래그 판정 분기 자체가 없다.
 *
 * 둘째, SPLICE_PAGES 를 조건부로 붙인다. h2cdata_left 가 0 이 아니면 이번
 * 조각 뒤에 또 다른 H2C PDU 가 이어진다는 뜻이고, 그 경우 헤더 버퍼를
 * 곧바로 다시 쓰게 된다. 참조로 붙여 보내면 소켓이 아직 그 페이지를 들고
 * 있을 수 있으므로, 재사용 전에 내용이 바뀌면 안 된다. 그래서 마지막
 * 조각일 때만 제로카피를 쓴다.
 *
 * 실행 컨텍스트: io_work 또는 poll. send_mutex 는 호출자가 잡는다.
 *
 * 호출 체인:
 *   nvme_tcp_try_send → [이 함수] → sock_sendmsg
 */
static int nvme_tcp_try_send_data_pdu(struct nvme_tcp_request *req)
{
	struct nvme_tcp_queue *queue = req->queue;
	struct nvme_tcp_data_pdu *pdu = nvme_tcp_req_data_pdu(req);
	struct bio_vec bvec;
	struct msghdr msg = { .msg_flags = MSG_DONTWAIT | MSG_MORE, };	/* [한국어] 데이터가 반드시 이어지므로 늘 MSG_MORE 다 */
	u8 hdgst = nvme_tcp_hdgst_len(queue);
	int len = sizeof(*pdu) - req->offset + hdgst;
	int ret;

	if (queue->hdr_digest && !req->offset)	/* [한국어] 명령 PDU 와 같은 이유로 첫 진입에서만 계산한다 */
		nvme_tcp_set_hdgst(pdu, sizeof(*pdu));

	if (!req->h2cdata_left)	/* [한국어] 이번이 마지막 조각이면 헤더 버퍼를 다시 쓸 일이 없다 */
		msg.msg_flags |= MSG_SPLICE_PAGES;	/* [한국어] 그때만 제로카피 — 이어지는 조각이 있으면 소켓이 든 페이지를 덮어쓰게 된다 */

	bvec_set_virt(&bvec, (void *)pdu + req->offset, len);
	iov_iter_bvec(&msg.msg_iter, ITER_SOURCE, &bvec, 1, len);
	ret = sock_sendmsg(queue->sock, &msg);
	if (unlikely(ret <= 0))
		return ret;

	len -= ret;
	if (!len) {
		req->state = NVME_TCP_SEND_DATA;	/* [한국어] 헤더가 끝났으니 본문으로 */
		if (queue->data_digest)
			queue->snd_crc = NVME_TCP_CRC_SEED;	/* [한국어] 이 조각의 데이터 CRC 를 새로 시작한다 */
		return 1;
	}
	req->offset += ret;

	return -EAGAIN;
}

/*
 * [한국어]
 * nvme_tcp_try_send_ddgst - 데이터 다이제스트 4바이트를 보낸다
 *
 * @req: 데이터를 다 보낸 요청
 * @return: 1 이면 다 보냈다. -EAGAIN 이면 부분 전송. 0 이하는 오류.
 *
 * 송신 상태 기계의 마지막 단계이며, data_digest 를 협상한 연결에서만 나타난다.
 * 4바이트뿐이지만 그것조차 부분 전송될 수 있어 offset 관리가 필요하다.
 *
 * kernel_sendmsg 를 쓰는 것이 다른 단계와 다르다. 보낼 것이 요청 구조체
 * 안의 작은 스칼라라 페이지 참조를 붙일 이유가 없고, kvec 으로 주소만
 * 넘기는 편이 단순하다.
 *
 * 지역 변수 두 개를 미리 읽어 두는 것이 중요하다. 마지막 바이트가 나가면
 * done_send_req 가 큐의 현재 요청 자리를 비우고, 수신 경로가 곧바로 응답을
 * 받아 요청을 완료시킬 수 있다. 그 뒤에 req 를 읽으면 이미 반환된 메모리를
 * 만지게 되므로, 판정에 필요한 값을 먼저 복사해 둔다. try_send_data 의
 * "마지막에는 iter 를 건드리지 않는다"와 같은 종류의 조심이다.
 *
 * 다 보낸 뒤 h2cdata_left 가 남았으면 다음 H2C PDU 를 준비한다 -- R2T
 * 분량이 여러 조각으로 나뉜 경우이며, 각 조각마다 다이제스트가 붙는다.
 *
 * 실행 컨텍스트: io_work 또는 poll. send_mutex 는 호출자가 잡는다.
 *
 * 호출 체인:
 *   nvme_tcp_try_send → [이 함수] → kernel_sendmsg
 */
static int nvme_tcp_try_send_ddgst(struct nvme_tcp_request *req)
{
	struct nvme_tcp_queue *queue = req->queue;
	size_t offset = req->offset;	/* [한국어] 지역 복사 — 아래에서 요청이 완료돼 사라질 수 있다 */
	u32 h2cdata_left = req->h2cdata_left;	/* [한국어] 같은 이유로 미리 읽는다 */
	int ret;
	struct msghdr msg = { .msg_flags = MSG_DONTWAIT };
	struct kvec iov = {
		.iov_base = (u8 *)&req->ddgst + req->offset,	/* [한국어] 요청 안의 작은 스칼라라 페이지 참조를 붙일 이유가 없다 */
		.iov_len = NVME_TCP_DIGEST_LENGTH - req->offset	/* [한국어] 4바이트도 부분 전송될 수 있어 offset 을 뺀다 */
	};

	if (nvme_tcp_queue_more(queue))	/* [한국어] 큐에 대기 요청이 있으면 */
		msg.msg_flags |= MSG_MORE;
	else
		msg.msg_flags |= MSG_EOR;	/* [한국어] 이것이 마지막이면 즉시 밀어낸다 */

	ret = kernel_sendmsg(queue->sock, &msg, &iov, 1, iov.iov_len);
	if (unlikely(ret <= 0))
		return ret;

	if (offset + ret == NVME_TCP_DIGEST_LENGTH) {	/* [한국어] 미리 읽어 둔 offset 을 쓴다 — req 는 이미 완료됐을 수 있다 */
		if (h2cdata_left)	/* [한국어] R2T 분량이 아직 남았다 */
			nvme_tcp_setup_h2c_data_pdu(req);	/* [한국어] 다음 조각의 헤더를 준비한다. 조각마다 다이제스트가 붙는다 */
		else
			nvme_tcp_done_send_req(queue);	/* [한국어] 이 요청의 전송이 완전히 끝났다 */
		return 1;
	}

	req->offset += ret;	/* [한국어] 부분만 나갔다 — 아직 요청이 살아 있으므로 안전하다 */
	return -EAGAIN;
}

/*
 * [한국어]
 * nvme_tcp_try_send - 요청 하나를 보낼 수 있는 데까지 보낸다
 *
 * @queue: 대상 큐
 * @return: 1 이면 진전이 있었고 더 보낼 것이 있을 수 있다. 0 이면 지금은 더
 *          보낼 수 없다(보낼 요청이 없거나 소켓이 가득 찼다). 음수는 오류.
 *
 * 이것이 송신 상태 기계의 본체다. 아래 if 들이 else 없이 연달아 오는 것이
 * 핵심 구조다 -- 한 번의 호출에서 여러 단계를 이어서 진행할 수 있어야 하기
 * 때문이다. 명령 PDU 를 다 보내고 소켓에 여유가 남아 있으면 그 자리에서
 * 데이터까지 밀어 넣는다.
 *
 * 단계 순서는 CMD_PDU → (H2C_PDU) → DATA → DDGST 다. 인라인 데이터가 없는
 * 요청은 명령 PDU 만 보내고 out 으로 빠진다 -- 데이터는 타겟이 R2T 를 보낼
 * 때까지 기다려야 하기 때문이다.
 *
 * -EAGAIN 을 0 으로 바꾸는 이유: 소켓 버퍼가 찬 것은 오류가 아니라 정상적인
 * 배압이다. 남은 분량은 write_space 콜백이 깨워 줄 때 이어서 보낸다.
 *
 * memalloc_noreclaim 으로 감싸는 이유: 이 경로에서 메모리 회수가 일어나면
 * 회수가 다시 이 블록 장치에 쓰기를 시도해 자기 자신을 기다리는 교착이 된다.
 * 전송 중에는 회수를 금지해 그 고리를 끊는다.
 *
 * 실행 컨텍스트: io_work 또는 blk-mq poll. send_mutex 는 호출자가 잡는다.
 *
 * 호출 체인:
 *   nvme_tcp_io_work → [이 함수] → try_send_cmd_pdu / _data_pdu / _data / _ddgst
 */
static int nvme_tcp_try_send(struct nvme_tcp_queue *queue)
{
	struct nvme_tcp_request *req;
	unsigned int noreclaim_flag;	/* [한국어] 회수 금지 이전 상태 — 나갈 때 되돌린다 */
	int ret = 1;

	if (!queue->request) {	/* [한국어] 이어서 보낼 요청이 없으면 새로 꺼낸다 */
		queue->request = nvme_tcp_fetch_request(queue);	/* [한국어] req_list 를 걷어 send_list 로 옮기고 하나를 집는다 */
		if (!queue->request)
			return 0;	/* [한국어] 보낼 것이 없다 — io_work 는 여기서 수신 쪽으로 넘어간다 */
	}
	req = queue->request;

	noreclaim_flag = memalloc_noreclaim_save();	/* [한국어] 전송 중 메모리 회수 금지 — 자기 장치에 쓰기를 유발하는 교착을 막는다 */
	if (req->state == NVME_TCP_SEND_CMD_PDU) {	/* [한국어] 1단계: 명령 PDU 헤더(+인라인 데이터) */
		ret = nvme_tcp_try_send_cmd_pdu(req);
		if (ret <= 0)	/* [한국어] 다 못 보냈거나 오류 — 다음 기회에 이어 간다 */
			goto done;
		if (!nvme_tcp_has_inline_data(req))	/* [한국어] 인라인 데이터가 없는 요청이면 */
			goto out;	/* [한국어] 여기서 끝. 데이터는 타겟의 R2T 를 받고 나서야 보낸다 */
	}

	if (req->state == NVME_TCP_SEND_H2C_PDU) {	/* [한국어] 2단계: R2T 에 응답하는 H2C 데이터 PDU 헤더 */
		ret = nvme_tcp_try_send_data_pdu(req);
		if (ret <= 0)
			goto done;
	}

	if (req->state == NVME_TCP_SEND_DATA) {	/* [한국어] 3단계: 데이터 본문. 크기에 따라 여러 번 머문다 */
		ret = nvme_tcp_try_send_data(req);
		if (ret <= 0)
			goto done;
	}

	if (req->state == NVME_TCP_SEND_DDGST)	/* [한국어] 4단계: 데이터 다이제스트 4바이트. data_digest 협상 시에만 */
		ret = nvme_tcp_try_send_ddgst(req);
done:
	if (ret == -EAGAIN) {	/* [한국어] 소켓 버퍼가 찼다 — 오류가 아니라 배압이다 */
		ret = 0;	/* [한국어] 0 으로 바꿔 "지금은 못 보낸다"로 보고. write_space 가 깨워 주면 이어 간다 */
	} else if (ret < 0) {	/* [한국어] 진짜 오류 — 소켓이 끊겼거나 프로토콜이 깨졌다 */
		dev_err(queue->ctrl->ctrl.device,
			"failed to send request %d\n", ret);
		nvme_tcp_fail_request(queue->request);	/* [한국어] 이 요청을 실패로 완료시킨다 */
		nvme_tcp_done_send_req(queue);	/* [한국어] 큐의 현재 요청 자리를 비워 다음 요청으로 넘어가게 한다 */
	}
out:
	memalloc_noreclaim_restore(noreclaim_flag);	/* [한국어] 회수 금지를 해제 — 반드시 짝을 맞춰야 한다 */
	return ret;
}

/*
 * [한국어]
 * nvme_tcp_try_recv - 소켓에 쌓인 바이트를 수신 상태 기계에 흘려 넣는다
 *
 * @queue: 대상 큐
 * @return: 소비한 바이트 수. 읽을 것이 없으면 0. 음수면 오류.
 *
 * 직접 recvmsg 를 부르지 않고 read_sock 을 쓰는 것이 요점이다. read_sock 은
 * 소켓에 도착한 skb 를 복사 없이 콜백(nvme_tcp_recv_skb)에 그대로 넘겨 준다.
 * 그래서 PDU 파싱이 커널 버퍼 위에서 바로 이뤄지고, 중간 복사가 한 번 줄어든다.
 *
 * nr_cqe 를 여기서 0 으로 되돌리는 이유: 이번 수신에서 몇 개의 요청을 완료
 * 시켰는지 세기 위해서다. blk-mq poll 은 그 개수를 반환값으로 요구한다.
 *
 * -EAGAIN 을 0 으로 바꾸는 것은 송신 쪽과 같은 이유다. 읽을 것이 없는 것은
 * 오류가 아니라 정상 상태이며, data_ready 콜백이 다음 도착을 알려 준다.
 *
 * lock_sock 으로 감싸는 이유: read_sock 은 소켓의 수신 큐를 직접 훑으므로,
 * 소프트IRQ 문맥의 네트워크 스택이 동시에 그것을 건드리면 안 된다.
 *
 * 실행 컨텍스트: io_work 또는 blk-mq poll. lock_sock 은 잠들 수 있다.
 *
 * 호출 체인:
 *   nvme_tcp_io_work / nvme_tcp_poll → [이 함수] → read_sock → nvme_tcp_recv_skb
 */
static int nvme_tcp_try_recv(struct nvme_tcp_queue *queue)
{
	struct socket *sock = queue->sock;
	struct sock *sk = sock->sk;
	read_descriptor_t rd_desc;	/* [한국어] read_sock 이 콜백에 함께 넘길 문맥 */
	int consumed;

	rd_desc.arg.data = queue;	/* [한국어] 콜백이 이 큐를 되찾을 수 있게 심어 둔다 */
	rd_desc.count = 1;	/* [한국어] 0 이 아니기만 하면 된다 — read_sock 이 계속 진행할지 판단하는 값이다 */
	lock_sock(sk);	/* [한국어] 수신 큐를 직접 훑으므로 네트워크 스택과의 동시 접근을 막는다 */
	queue->nr_cqe = 0;	/* [한국어] 이번 수신에서 완료시킨 요청 수를 새로 센다 — poll 이 이 값을 요구한다 */
	consumed = sock->ops->read_sock(sk, &rd_desc, nvme_tcp_recv_skb);	/* [한국어] 도착한 skb 를 복사 없이 파서에 그대로 넘긴다 */
	release_sock(sk);
	return consumed == -EAGAIN ? 0 : consumed;	/* [한국어] 읽을 것이 없는 것은 오류가 아니다 — data_ready 가 다음을 알려 준다 */
}

/*
 * [한국어]
 * nvme_tcp_io_work - 이 큐의 송신과 수신을 한 문맥에서 번갈아 처리한다
 *
 * @w: 큐에 박혀 있는 io_work
 * @return: 없음
 *
 * 이 파일의 동시성 설계가 여기 압축돼 있다. 큐마다 work 가 하나뿐이고 송신과
 * 수신을 모두 이것이 맡으므로, work 실행 자체가 직렬화 장치가 된다. 소켓을
 * 두 문맥이 동시에 만지는 일이 없어 별도 잠금이 대부분 필요 없다.
 *
 * 루프를 도는 이유: 한 번 깨어났을 때 가능한 한 많이 처리해야 재스케줄 비용을
 * 아낀다. 다만 무한정 붙잡으면 같은 CPU 의 다른 work 가 굶으므로, 1밀리초
 * 예산을 두고 그것을 넘기면 스스로를 다시 큐에 넣어 양보한다.
 *
 * send_mutex 를 trylock 으로만 잡는 이유: 제출 경로(queue_rq)도 직접 보낼 수
 * 있어 이 잠금을 들고 있을 수 있다. 여기서 기다리면 io_work 가 그 제출을
 * 막는 셈이 되므로, 실패하면 송신을 건너뛰고 수신부터 처리한다. 못 보낸
 * 분량은 다음 순회나 write_space 콜백이 처리한다.
 *
 * 수신 후 다시 송신 여지를 확인하는 것(위 영어 주석의 "did we get some space")
 * 은, 수신을 처리하는 동안 상대가 ACK 를 보내 송신 버퍼가 비었을 수 있기
 * 때문이다. 그 경우 곧바로 한 바퀴 더 돈다.
 *
 * 실행 컨텍스트: 워크큐. io_cpu 에 고정되어 큐마다 다른 코어에서 돈다.
 *
 * 호출 체인:
 *   data_ready / write_space 콜백, queue_rq → queue_work_on → [이 함수]
 *     → nvme_tcp_try_send / nvme_tcp_try_recv
 */
static void nvme_tcp_io_work(struct work_struct *w)
{
	struct nvme_tcp_queue *queue =
		container_of(w, struct nvme_tcp_queue, io_work);
	unsigned long deadline = jiffies + msecs_to_jiffies(1);	/* [한국어] 1밀리초 예산 — 이 CPU 의 다른 work 를 굶기지 않기 위한 상한 */

	do {
		bool pending = false;	/* [한국어] 이번 바퀴에 진전이 있었는가 — 없으면 더 돌 이유가 없다 */
		int result;

		if (mutex_trylock(&queue->send_mutex)) {	/* [한국어] 제출 경로가 들고 있으면 기다리지 않고 건너뛴다 */
			result = nvme_tcp_try_send(queue);
			mutex_unlock(&queue->send_mutex);
			if (result > 0)
				pending = true;	/* [한국어] 뭔가 보냈다 — 더 보낼 것이 남았을 수 있다 */
			else if (unlikely(result < 0))
				break;	/* [한국어] 송신 오류. 루프를 빠져나가 아래에서 재스케줄한다 */
		}

		result = nvme_tcp_try_recv(queue);	/* [한국어] 송신을 건너뛰었더라도 수신은 항상 시도한다 */
		if (result > 0)
			pending = true;
		else if (unlikely(result < 0))
			return;	/* [한국어] 수신 오류는 연결이 끊긴 것이므로 재스케줄하지 않고 끝낸다 */

		/* did we get some space after spending time in recv? */
		if (nvme_tcp_queue_has_pending(queue) &&	/* [한국어] 보낼 것이 남아 있고 */
		    sk_stream_is_writeable(queue->sock->sk))	/* [한국어] 수신 처리 사이에 ACK 로 송신 버퍼가 비었다면 */
			pending = true;	/* [한국어] 곧바로 한 바퀴 더 돈다 */

		if (!pending || !queue->rd_enabled)	/* [한국어] 진전이 없거나 큐가 내려가는 중이면 */
			return;	/* [한국어] 재스케줄 없이 끝낸다 — 다음 콜백이 깨워 줄 것이다 */

	} while (!time_after(jiffies, deadline)); /* quota is exhausted */

	queue_work_on(queue->io_cpu, nvme_tcp_wq, &queue->io_work);	/* [한국어] 예산을 다 썼지만 할 일이 남았다 — 자신을 다시 큐에 넣어 양보한다 */
}

static void nvme_tcp_free_async_req(struct nvme_tcp_ctrl *ctrl)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	struct nvme_tcp_request *async = &ctrl->async_req;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	page_frag_free(async->pdu);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}

static int nvme_tcp_alloc_async_req(struct nvme_tcp_ctrl *ctrl)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	struct nvme_tcp_queue *queue = &ctrl->queues[0];	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct nvme_tcp_request *async = &ctrl->async_req;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	u8 hdgst = nvme_tcp_hdgst_len(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	async->pdu = page_frag_alloc(&queue->pf_cache,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		sizeof(struct nvme_tcp_cmd_pdu) + hdgst,
		GFP_KERNEL | __GFP_ZERO);
	if (!async->pdu)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return -ENOMEM;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	async->queue = &ctrl->queues[0];	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static void nvme_tcp_free_queue(struct nvme_ctrl *nctrl, int qid)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
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
}

/*
 * [한국어]
 * nvme_tcp_init_connection - ICReq/ICResp 로 이 연결의 규약을 합의한다
 *
 * @queue: 소켓이 연결된 직후의 큐
 * @return: 0 이면 합의 성공. 음수면 상대가 규약을 어겼거나 설정이 맞지 않는다.
 *
 * TCP 소켓이 붙었다고 곧바로 NVMe 를 말할 수 있는 것은 아니다. 그 위에서
 * 무엇을 어떻게 주고받을지 먼저 정해야 한다. 이 교환이 그 첫 대화이며,
 * Fabrics Connect 명령보다도 앞선다.
 *
 * 여기서 정해지는 것이 이후 모든 PDU 의 모양을 좌우한다:
 *   - 헤더/데이터 다이제스트 사용 여부 → 모든 PDU 의 길이 계산이 달라진다.
 *   - maxh2cdata → 한 번의 H2C PDU 로 보낼 수 있는 최대 바이트. R2T 분량을
 *     이 크기로 쪼개는 근거가 된다.
 *
 * 다이제스트 검사가 "둘 다 켜졌거나 둘 다 꺼졌거나"를 요구하는 이유: 한쪽만
 * 켜면 보내는 쪽은 다이제스트를 붙이는데 받는 쪽은 그것을 데이터로 읽어
 * 스트림 동기가 통째로 어긋난다. 그래서 불일치는 곧바로 연결 실패다.
 *
 * MSG_WAITALL 로 받는 이유: ICResp 는 고정 길이이고 이 시점에는 수신 상태
 * 기계가 아직 돌지 않는다. 부분 수신을 다룰 장치가 없으므로 다 올 때까지
 * 기다린다.
 *
 * TLS 가 켜진 연결에서는 받은 것이 실제 데이터 레코드인지 확인한다.
 * 제어 레코드를 데이터로 오인해 파싱하면 안 되기 때문이다.
 *
 * maxr2t 를 0 으로 보내는 것은 "동시에 하나의 R2T 만 처리한다"는 선언이고,
 * hpda 0 은 정렬 제약을 두지 않겠다는 뜻이다. 그래서 handle_r2t 가 이미
 * 목록에 있는 요청에 대한 R2T 를 프로토콜 위반으로 처리할 수 있다.
 *
 * 실행 컨텍스트: 큐 생성 경로. kernel_sendmsg/recvmsg 로 잠들 수 있다.
 *
 * 호출 체인:
 *   nvme_tcp_alloc_queue → [이 함수] → nvmf_connect_admin_queue / _io_queue
 */
static int nvme_tcp_init_connection(struct nvme_tcp_queue *queue)
{
	struct nvme_tcp_icreq_pdu *icreq;	/* [한국어] 우리가 제안하는 규약 */
	struct nvme_tcp_icresp_pdu *icresp;	/* [한국어] 상대가 확정해 돌려주는 규약 */
	char cbuf[CMSG_LEN(sizeof(char))] = {};	/* [한국어] TLS 레코드 종류를 받아 올 제어 메시지 버퍼 */
	u8 ctype;
	struct msghdr msg = {};
	struct kvec iov;
	bool ctrl_hdgst, ctrl_ddgst;	/* [한국어] 상대가 실제로 켠 다이제스트 설정 */
	u32 maxh2cdata;
	int ret;

	icreq = kzalloc_obj(*icreq);	/* [한국어] 스택이 아니라 힙에 두는 것은 소켓에 넘길 버퍼이기 때문이다 */
	if (!icreq)
		return -ENOMEM;

	icresp = kzalloc_obj(*icresp);
	if (!icresp) {
		ret = -ENOMEM;
		goto free_icreq;
	}

	icreq->hdr.type = nvme_tcp_icreq;	/* [한국어] 연결 초기화 요청 PDU */
	icreq->hdr.hlen = sizeof(*icreq);
	icreq->hdr.pdo = 0;	/* [한국어] 데이터 오프셋 없음 — ICReq 는 본문이 없다 */
	icreq->hdr.plen = cpu_to_le32(icreq->hdr.hlen);	/* [한국어] 전체 길이가 곧 헤더 길이다 */
	icreq->pfv = cpu_to_le16(NVME_TCP_PFV_1_0);	/* [한국어] 프로토콜 버전. 상대가 다른 버전을 돌려주면 거절한다 */
	icreq->maxr2t = 0; /* single inflight r2t supported */
	/* [한국어] 위 영어 주석대로 동시에 하나의 R2T 만 다루겠다는 선언이다.
	 * handle_r2t 가 "이미 목록에 있는 요청의 R2T"를 위반으로 볼 수 있는 근거가 이것이다. */
	icreq->hpda = 0; /* no alignment constraint */
	/* [한국어] 데이터 정렬 제약을 두지 않는다 — 패딩 계산이 필요 없어진다 */
	if (queue->hdr_digest)	/* [한국어] 호스트가 원하는 다이제스트 설정을 제안한다 */
		icreq->digest |= NVME_TCP_HDR_DIGEST_ENABLE;
	if (queue->data_digest)
		icreq->digest |= NVME_TCP_DATA_DIGEST_ENABLE;

	iov.iov_base = icreq;
	iov.iov_len = sizeof(*icreq);
	ret = kernel_sendmsg(queue->sock, &msg, &iov, 1, iov.iov_len);	/* [한국어] 아직 수신 상태 기계가 돌기 전이라 직접 보낸다 */
	if (ret < 0) {
		pr_warn("queue %d: failed to send icreq, error %d\n",
			nvme_tcp_queue_id(queue), ret);
		goto free_icresp;
	}

	memset(&msg, 0, sizeof(msg));	/* [한국어] 송신에 쓴 msghdr 를 수신용으로 재사용하므로 깨끗이 지운다 */
	iov.iov_base = icresp;
	iov.iov_len = sizeof(*icresp);
	if (nvme_tcp_queue_tls(queue)) {	/* [한국어] TLS 연결이면 레코드 종류를 함께 받아야 한다 */
		msg.msg_control = cbuf;
		msg.msg_controllen = sizeof(cbuf);
	}
	msg.msg_flags = MSG_WAITALL;	/* [한국어] 고정 길이 응답이고 부분 수신을 다룰 상태 기계가 아직 없으므로 다 올 때까지 기다린다 */
	ret = kernel_recvmsg(queue->sock, &msg, &iov, 1,
			iov.iov_len, msg.msg_flags);
	if (ret >= 0 && ret < sizeof(*icresp))	/* [한국어] WAITALL 인데도 모자라게 왔다면 상대가 중간에 끊은 것이다 */
		ret = -ECONNRESET;
	if (ret < 0) {
		pr_warn("queue %d: failed to receive icresp, error %d\n",
			nvme_tcp_queue_id(queue), ret);
		goto free_icresp;
	}
	ret = -ENOTCONN;	/* [한국어] 아래 검사들이 모두 이 값으로 실패하도록 미리 세워 둔다 */
	if (nvme_tcp_queue_tls(queue)) {
		ctype = tls_get_record_type(queue->sock->sk,	/* [한국어] 받은 것이 데이터인지 TLS 제어 레코드인지 확인 */
					    (struct cmsghdr *)cbuf);
		if (ctype != TLS_RECORD_TYPE_DATA) {	/* [한국어] 제어 레코드를 ICResp 로 오인해 파싱하면 안 된다 */
			pr_err("queue %d: unhandled TLS record %d\n",
			       nvme_tcp_queue_id(queue), ctype);
			goto free_icresp;
		}
	}
	ret = -EINVAL;	/* [한국어] 여기부터는 규약 위반이므로 오류 코드를 바꾼다 */
	if (icresp->hdr.type != nvme_tcp_icresp) {	/* [한국어] 응답 종류가 다르다 — 상대가 NVMe/TCP 를 말하지 않는다 */
		pr_err("queue %d: bad type returned %d\n",
			nvme_tcp_queue_id(queue), icresp->hdr.type);
		goto free_icresp;
	}

	if (le32_to_cpu(icresp->hdr.plen) != sizeof(*icresp)) {	/* [한국어] 길이가 규격과 다르다 */
		pr_err("queue %d: bad pdu length returned %d\n",
			nvme_tcp_queue_id(queue), icresp->hdr.plen);
		goto free_icresp;
	}

	if (icresp->pfv != NVME_TCP_PFV_1_0) {	/* [한국어] 우리가 아는 프로토콜 버전이 아니다 */
		pr_err("queue %d: bad pfv returned %d\n",
			nvme_tcp_queue_id(queue), icresp->pfv);
		goto free_icresp;
	}

	ctrl_ddgst = !!(icresp->digest & NVME_TCP_DATA_DIGEST_ENABLE);	/* [한국어] 상대가 확정한 데이터 다이제스트 설정 */
	if ((queue->data_digest && !ctrl_ddgst) ||	/* [한국어] 한쪽만 켜져 있으면 */
	    (!queue->data_digest && ctrl_ddgst)) {	/* [한국어] 보내는 쪽은 붙이고 받는 쪽은 데이터로 읽어 스트림이 통째로 어긋난다 */
		pr_err("queue %d: data digest mismatch host: %s ctrl: %s\n",
			nvme_tcp_queue_id(queue),
			queue->data_digest ? "enabled" : "disabled",
			ctrl_ddgst ? "enabled" : "disabled");
		goto free_icresp;
	}

	ctrl_hdgst = !!(icresp->digest & NVME_TCP_HDR_DIGEST_ENABLE);	/* [한국어] 헤더 다이제스트도 같은 이유로 양쪽이 일치해야 한다 */
	if ((queue->hdr_digest && !ctrl_hdgst) ||
	    (!queue->hdr_digest && ctrl_hdgst)) {
		pr_err("queue %d: header digest mismatch host: %s ctrl: %s\n",
			nvme_tcp_queue_id(queue),
			queue->hdr_digest ? "enabled" : "disabled",
			ctrl_hdgst ? "enabled" : "disabled");
		goto free_icresp;
	}

	if (icresp->cpda != 0) {	/* [한국어] 컨트롤러가 데이터 정렬을 요구했다 */
		pr_err("queue %d: unsupported cpda returned %d\n",	/* [한국어] 이 드라이버는 패딩을 넣지 않으므로 지원할 수 없다 */
			nvme_tcp_queue_id(queue), icresp->cpda);
		goto free_icresp;
	}

	maxh2cdata = le32_to_cpu(icresp->maxdata);	/* [한국어] 한 번의 H2C PDU 로 보낼 수 있는 최대 바이트 */
	if ((maxh2cdata % 4) || (maxh2cdata < NVME_TCP_MIN_MAXH2CDATA)) {	/* [한국어] 4바이트 배수여야 하고 규격 최소값 이상이어야 한다 */
		pr_err("queue %d: invalid maxh2cdata returned %u\n",
		       nvme_tcp_queue_id(queue), maxh2cdata);
		goto free_icresp;
	}
	queue->maxh2cdata = maxh2cdata;	/* [한국어] R2T 분량을 이 크기로 쪼개는 근거가 된다 */

	ret = 0;
free_icresp:	/* [한국어] 두 버퍼는 협상에만 쓰이므로 성공·실패 무관하게 여기서 해제한다 */
	kfree(icresp);
free_icreq:
	kfree(icreq);
	return ret;
}

static bool nvme_tcp_admin_queue(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	return nvme_tcp_queue_id(queue) == 0;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
}

static bool nvme_tcp_default_queue(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	struct nvme_tcp_ctrl *ctrl = queue->ctrl;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	int qid = nvme_tcp_queue_id(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	return !nvme_tcp_admin_queue(queue) &&	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		qid < 1 + ctrl->io_queues[HCTX_TYPE_DEFAULT];
}

/*
 * [한국어]
 * nvme_tcp_read_queue - 이 큐가 읽기 전용 구간에 속하는가
 *
 * @queue: 판정할 큐
 * @return: 읽기 전용 큐면 true
 *
 * 큐 배열은 용도별 구간으로 나뉜다 -- admin, 기본(DEFAULT), 읽기(READ),
 * 폴링(POLL) 순이다. 별도 플래그를 두지 않고 인덱스가 어느 구간에 드는지로
 * 판정하므로, 앞 구간들을 차례로 배제한 뒤 누적 경계와 비교한다.
 *
 * 읽기 전용 큐를 따로 두는 이유: 큰 쓰기가 소켓을 오래 점유하면 그 뒤의
 * 읽기가 밀린다. 읽기를 별도 큐로 보내면 그 간섭이 사라져 읽기 지연의
 * 편차가 줄어든다.
 *
 * 호출 체인: nvme_tcp_set_queue_io_cpu / _poll_queue → [이 함수]
 */
static bool nvme_tcp_read_queue(struct nvme_tcp_queue *queue)
{
	struct nvme_tcp_ctrl *ctrl = queue->ctrl;
	int qid = nvme_tcp_queue_id(queue);

	return !nvme_tcp_admin_queue(queue) &&	/* [한국어] admin 은 별개다 */
		!nvme_tcp_default_queue(queue) &&	/* [한국어] 기본 구간도 아니어야 한다 */
		qid < 1 + ctrl->io_queues[HCTX_TYPE_DEFAULT] +	/* [한국어] admin 하나 + 기본 구간 + */
			  ctrl->io_queues[HCTX_TYPE_READ];	/* [한국어] 읽기 구간까지가 경계 */
}

/*
 * [한국어]
 * nvme_tcp_poll_queue - 이 큐가 폴링 구간에 속하는가
 *
 * @queue: 판정할 큐
 * @return: 폴링 큐면 true
 *
 * 배열의 마지막 구간이다. 앞의 세 구간(admin, 기본, 읽기)을 모두 배제한 뒤
 * 누적 경계 안에 있으면 폴링 큐다.
 *
 * 폴링 큐는 인터럽트 대신 blk_mq_poll 이 직접 수신을 돌린다. 제출한 스레드가
 * 그 자리에서 완료를 가져가므로 깨우기와 문맥 전환이 사라진다 -- 지연을
 * 최소화하려는 구성에서 쓴다.
 *
 * 호출 체인: nvme_tcp_set_queue_io_cpu 등 큐 용도 판정 경로 → [이 함수]
 */
static bool nvme_tcp_poll_queue(struct nvme_tcp_queue *queue)
{
	struct nvme_tcp_ctrl *ctrl = queue->ctrl;
	int qid = nvme_tcp_queue_id(queue);

	return !nvme_tcp_admin_queue(queue) &&
		!nvme_tcp_default_queue(queue) &&
		!nvme_tcp_read_queue(queue) &&	/* [한국어] 앞의 세 구간을 모두 배제하면 남는 것이 폴링 구간이다 */
		qid < 1 + ctrl->io_queues[HCTX_TYPE_DEFAULT] +
			  ctrl->io_queues[HCTX_TYPE_READ] +
			  ctrl->io_queues[HCTX_TYPE_POLL];	/* [한국어] 마지막 누적 경계 */
}

/*
 * Track the number of queues assigned to each cpu using a global per-cpu
 * counter and select the least used cpu from the mq_map. Our goal is to spread
 * different controllers I/O threads across different cpu cores.
 *
 * Note that the accounting is not 100% perfect, but we don't need to be, we're
 * simply putting our best effort to select the best candidate cpu core that we
 * find at any given point.
 */
/*
 * [한국어]
 * nvme_tcp_set_queue_io_cpu - 이 큐의 io_work 를 돌릴 CPU 를 고른다
 *
 * @queue: CPU 를 배정할 큐
 * @return: 없음. 배정에 성공하면 Q_IO_CPU_SET 비트가 선다.
 *
 * 큐마다 io_work 가 하나이고 그것이 송수신을 모두 맡으므로, 어느 CPU 에서
 * 도느냐가 곧 이 큐의 성능이다. 여러 큐가 한 코어에 몰리면 그 코어가
 * 병목이 되고 나머지 코어는 논다.
 *
 * 고르는 방식은 위 영어 주석이 설명한다 -- CPU 별 큐 개수를 전역 카운터로
 * 세어 두고, 후보 중 가장 적게 쓰인 CPU 를 고른다. 목표는 여러 컨트롤러의
 * I/O 스레드를 서로 다른 코어로 흩는 것이다. 정확한 회계는 아니지만
 * 그럴 필요도 없다는 것이 그 주석의 단서다.
 *
 * 후보를 mq_map 으로 좁히는 것이 요점이다. blk-mq 는 이미 CPU 와 하드웨어
 * 큐의 대응을 정해 두었으므로, 그 대응을 따르면 요청을 제출한 CPU 와
 * 그것을 보내는 io_work 가 같은 코어에 놓인다. 캐시가 따뜻한 채로 이어진다.
 * 그래서 큐 용도(기본/읽기/폴링)에 맞는 맵을 골라 그 안에서만 찾는다.
 *
 * wq_unbound 가 켜져 있으면 아무것도 하지 않고 나간다. 사용자가 워크큐를
 * 특정 CPU 에 묶지 말라고 지시한 것이므로, 스케줄러에 맡긴다.
 *
 * 실행 컨텍스트: 큐 생성 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   nvme_tcp_alloc_queue → [이 함수] → (io_work 가 이 CPU 에서 queue_work_on 된다)
 */
static void nvme_tcp_set_queue_io_cpu(struct nvme_tcp_queue *queue)
{
	struct nvme_tcp_ctrl *ctrl = queue->ctrl;
	struct blk_mq_tag_set *set = &ctrl->tag_set;
	int qid = nvme_tcp_queue_id(queue) - 1;	/* [한국어] mq_map 은 I/O 큐만 담으므로 admin 몫을 뺀 인덱스를 쓴다 */
	unsigned int *mq_map = NULL;
	int cpu, min_queues = INT_MAX, io_cpu;

	if (wq_unbound)	/* [한국어] 사용자가 CPU 고정을 원하지 않는다 */
		goto out;	/* [한국어] io_cpu 는 초기값(WORK_CPU_UNBOUND)으로 남아 스케줄러가 정한다 */

	if (nvme_tcp_default_queue(queue))	/* [한국어] 큐 용도에 맞는 맵을 골라야 blk-mq 의 CPU 대응을 따를 수 있다 */
		mq_map = set->map[HCTX_TYPE_DEFAULT].mq_map;
	else if (nvme_tcp_read_queue(queue))
		mq_map = set->map[HCTX_TYPE_READ].mq_map;
	else if (nvme_tcp_poll_queue(queue))
		mq_map = set->map[HCTX_TYPE_POLL].mq_map;

	if (WARN_ON(!mq_map))	/* [한국어] 세 구간 어디에도 속하지 않는 큐는 있을 수 없다 */
		goto out;

	/* Search for the least used cpu from the mq_map */
	io_cpu = WORK_CPU_UNBOUND;
	for_each_online_cpu(cpu) {
		int num_queues = atomic_read(&nvme_tcp_cpu_queues[cpu]);	/* [한국어] 이 CPU 에 이미 배정된 큐 수 */

		if (mq_map[cpu] != qid)	/* [한국어] 이 큐를 담당하도록 blk-mq 가 지정한 CPU 만 후보다 */
			continue;	/* [한국어] 그래야 제출한 CPU 와 전송하는 CPU 가 같아 캐시가 이어진다 */
		if (num_queues < min_queues) {	/* [한국어] 후보 중 가장 한가한 쪽을 고른다 */
			io_cpu = cpu;
			min_queues = num_queues;
		}
	}
	if (io_cpu != WORK_CPU_UNBOUND) {	/* [한국어] 후보를 찾았다 */
		queue->io_cpu = io_cpu;
		atomic_inc(&nvme_tcp_cpu_queues[io_cpu]);	/* [한국어] 전역 카운터를 올려 다음 큐가 이 CPU 를 덜 고르게 한다 */
		set_bit(NVME_TCP_Q_IO_CPU_SET, &queue->flags);	/* [한국어] 해제 시 카운터를 되돌려야 하므로 성공 여부를 남긴다 */
	}
out:
	dev_dbg(ctrl->ctrl.device, "queue %d: using cpu %d\n",
		qid, queue->io_cpu);
}

/*
 * [한국어]
 * nvme_tcp_tls_done - 유저스페이스 TLS 핸드셰이크가 끝났음을 통지받는다
 *
 * @data:   큐 포인터. 핸드셰이크를 요청할 때 넘겨 둔 것이다.
 * @status: 핸드셰이크 결과. 0 이 아니면 실패.
 * @pskid:  협상된 PSK 의 키 serial. 커널 키링에 등록돼 있다.
 * @return: 없음
 *
 * 커널은 TLS 핸드셰이크를 직접 하지 않는다. netlink handshake API 로
 * 유저스페이스 데몬(ktls-utils)에 맡기고, 그 데몬이 끝나면 이 콜백이 불린다.
 * 협상 결과는 커널 키링에 키로 남고, 여기서는 그 키 번호만 건네받는다.
 *
 * 콜백은 값을 돌려줄 수 없으므로 결과를 queue->tls_err 에 남기고 completion
 * 을 올린다. 연결을 진행하던 쪽이 그 completion 에서 잠들어 있다가 깨어나
 * 이 값을 본다. 그래서 성공 경로든 실패 경로든 마지막에 반드시 complete 를
 * 지나야 한다 -- 빠뜨리면 상대가 영원히 잠든다.
 *
 * qid 0(admin)일 때만 ctrl->tls_pskid 를 남기는 것이 중요하다. secure
 * concatenation 구성에서 setup_ctrl 이 이 값의 유무로 "PSK 가 이미
 * 파생되었는가"를 판정해 admin 큐를 다시 세울지 정하기 때문이다.
 * I/O 큐들은 그 PSK 를 재사용하므로 따로 남길 필요가 없다.
 *
 * 키를 조회한 뒤 곧바로 key_put 하는 이유: 여기서는 키가 유효한지 확인하고
 * serial 만 기록하면 된다. 실제 사용은 커널 TLS 계층이 하며 그쪽이 자기
 * 참조를 든다.
 *
 * 실행 컨텍스트: handshake 서브시스템의 완료 문맥.
 *
 * 호출 체인:
 *   유저스페이스 핸드셰이크 데몬 → netlink → handshake 코어 → [이 함수]
 *     → complete(&queue->tls_complete) → nvme_tcp_start_tls 가 깨어난다
 */
static void nvme_tcp_tls_done(void *data, int status, key_serial_t pskid)
{
	struct nvme_tcp_queue *queue = data;	/* [한국어] 요청 시 넘겨 둔 큐 */
	struct nvme_tcp_ctrl *ctrl = queue->ctrl;
	int qid = nvme_tcp_queue_id(queue);
	struct key *tls_key;

	dev_dbg(ctrl->ctrl.device, "queue %d: TLS handshake done, key %x, status %d\n",
		qid, pskid, status);

	if (status) {	/* [한국어] 데몬이 핸드셰이크에 실패했다 */
		queue->tls_err = -status;	/* [한국어] 부호를 뒤집어 errno 관례에 맞춘다 */
		goto out_complete;	/* [한국어] 실패해도 반드시 complete 를 지나야 한다 — 아니면 기다리는 쪽이 영원히 잠든다 */
	}

	tls_key = nvme_tls_key_lookup(pskid);	/* [한국어] 데몬이 키링에 넣어 둔 키가 실제로 있는지 확인 */
	if (IS_ERR(tls_key)) {
		dev_warn(ctrl->ctrl.device, "queue %d: Invalid key %x\n",
			 qid, pskid);
		queue->tls_err = -ENOKEY;
	} else {
		queue->tls_enabled = true;	/* [한국어] 이제 이 소켓의 송수신은 TLS 레코드로 나간다 */
		if (qid == 0)	/* [한국어] admin 큐에서만 컨트롤러 수준에 남긴다 */
			ctrl->ctrl.tls_pskid = key_serial(tls_key);	/* [한국어] setup_ctrl 이 이 값의 유무로 secure concatenation 재시작 여부를 판정한다 */
		key_put(tls_key);	/* [한국어] 유효성만 확인했으므로 곧바로 놓는다. 실제 사용은 커널 TLS 계층이 자기 참조로 한다 */
		queue->tls_err = 0;
	}

out_complete:
	complete(&queue->tls_complete);	/* [한국어] 연결을 진행하던 쪽을 깨운다. 모든 경로가 여기를 지나야 한다 */
}

static int nvme_tcp_start_tls(struct nvme_ctrl *nctrl,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
			      struct nvme_tcp_queue *queue,
			      key_serial_t pskid)
{
	int qid = nvme_tcp_queue_id(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	struct tls_handshake_args args;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
	unsigned long tmo = tls_handshake_timeout * HZ;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	key_serial_t keyring = nvme_keyring_id();	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	dev_dbg(nctrl->device, "queue %d: start TLS with key %x\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		qid, pskid);
	memset(&args, 0, sizeof(args));	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	args.ta_sock = queue->sock;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	args.ta_done = nvme_tcp_tls_done;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	args.ta_data = queue;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	args.ta_my_peerids[0] = pskid;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	args.ta_num_peerids = 1;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (nctrl->opts->keyring)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		keyring = key_serial(nctrl->opts->keyring);
	args.ta_keyring = keyring;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	args.ta_timeout_ms = tls_handshake_timeout * 1000;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	queue->tls_err = -EOPNOTSUPP;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	init_completion(&queue->tls_complete);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ret = tls_client_hello_psk(&args, GFP_KERNEL);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(nctrl->device, "queue %d: failed to start TLS: %d\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			qid, ret);
		return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}
	ret = wait_for_completion_interruptible_timeout(&queue->tls_complete, tmo);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret <= 0) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		if (ret == 0)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			ret = -ETIMEDOUT;

		dev_err(nctrl->device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"queue %d: TLS handshake failed, error %d\n",
			qid, ret);
		tls_handshake_cancel(queue->sock->sk);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	} else {
		if (queue->tls_err) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			dev_err(nctrl->device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				"queue %d: TLS handshake complete, error %d\n",
				qid, queue->tls_err);
		} else {
			dev_dbg(nctrl->device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				"queue %d: TLS handshake complete\n", qid);
		}
		ret = queue->tls_err;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static int nvme_tcp_alloc_queue(struct nvme_ctrl *nctrl, int qid,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
				key_serial_t pskid)
{
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
		queue->cmnd_capsule_len = nctrl->ioccsz * 16;
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		queue->cmnd_capsule_len = sizeof(struct nvme_command) +	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
						NVME_TCP_ADMIN_CCSZ;

	ret = sock_create_kern(current->nsproxy->net_ns,	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
			ctrl->addr.ss_family, SOCK_STREAM,
			IPPROTO_TCP, &queue->sock);
	if (ret) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(nctrl->device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"failed to create socket: %d\n", ret);
		goto err_destroy_mutex;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}

	sock_file = sock_alloc_file(queue->sock, O_CLOEXEC, NULL);	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
	if (IS_ERR(sock_file)) {	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
		ret = PTR_ERR(sock_file);	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
		goto err_destroy_mutex;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}

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
		sock_set_priority(queue->sock->sk, so_priority);

	/* Set socket type of service */
	if (nctrl->opts->tos >= 0)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ip_sock_set_tos(queue->sock->sk, nctrl->opts->tos);

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
			sizeof(ctrl->src_addr));
		if (ret) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			dev_err(nctrl->device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				"failed to bind queue %d socket %d\n",
				qid, ret);
			goto err_sock;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
		}
	}

	if (nctrl->opts->mask & NVMF_OPT_HOST_IFACE) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		char *iface = nctrl->opts->host_iface;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		sockptr_t optval = KERNEL_SOCKPTR(iface);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

		ret = sock_setsockopt(queue->sock, SOL_SOCKET, SO_BINDTODEVICE,	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
				      optval, strlen(iface));
		if (ret) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			dev_err(nctrl->device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			  "failed to bind to interface %s queue %d err %d\n",
			  iface, qid, ret);
			goto err_sock;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
		}
	}

	queue->hdr_digest = nctrl->opts->hdr_digest;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	queue->data_digest = nctrl->opts->data_digest;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	rcv_pdu_size = sizeof(struct nvme_tcp_rsp_pdu) +	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
			nvme_tcp_hdgst_len(queue);
	queue->pdu = kmalloc(rcv_pdu_size, GFP_KERNEL);	/* [한국어] 커널 메모리 생명주기 */
	if (!queue->pdu) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = -ENOMEM;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto err_sock;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}

	dev_dbg(nctrl->device, "connecting queue %d\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvme_tcp_queue_id(queue));

	ret = kernel_connect(queue->sock, (struct sockaddr_unsized *)&ctrl->addr,	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
		sizeof(ctrl->addr), 0);
	if (ret) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(nctrl->device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"failed to connect socket: %d\n", ret);
		goto err_rcv_pdu;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}

	/* If PSKs are configured try to start TLS */
	if (nvme_tcp_tls_configured(nctrl) && pskid) {	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		ret = nvme_tcp_start_tls(nctrl, queue, pskid);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			goto err_init_connect;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}

	ret = nvme_tcp_init_connection(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto err_init_connect;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	set_bit(NVME_TCP_Q_ALLOCATED, &queue->flags);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

err_init_connect:
	kernel_sock_shutdown(queue->sock, SHUT_RDWR);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
err_rcv_pdu:
	kfree(queue->pdu);	/* [한국어] 커널 메모리 생명주기 */
err_sock:
	/* ->sock will be released by fput() */
	fput(queue->sock->file);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	queue->sock = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
err_destroy_mutex:
	mutex_destroy(&queue->send_mutex);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
	mutex_destroy(&queue->queue_lock);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static void nvme_tcp_restore_sock_ops(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	struct socket *sock = queue->sock;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	write_lock_bh(&sock->sk->sk_callback_lock);	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
	sock->sk->sk_user_data  = NULL;	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
	sock->sk->sk_data_ready = queue->data_ready;	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
	sock->sk->sk_state_change = queue->state_change;	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
	sock->sk->sk_write_space  = queue->write_space;	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
	write_unlock_bh(&sock->sk->sk_callback_lock);	/* [한국어] 커널 소켓 송수신 — TCP 바이트 스트림 PDU 운반 */
}

static void __nvme_tcp_stop_queue(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	kernel_sock_shutdown(queue->sock, SHUT_RDWR);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_tcp_restore_sock_ops(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	cancel_work_sync(&queue->io_work);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}

static void nvme_tcp_stop_queue_nowait(struct nvme_ctrl *nctrl, int qid)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	struct nvme_tcp_ctrl *ctrl = to_tcp_ctrl(nctrl);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct nvme_tcp_queue *queue = &ctrl->queues[qid];	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	if (!test_bit(NVME_TCP_Q_ALLOCATED, &queue->flags))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	if (test_and_clear_bit(NVME_TCP_Q_IO_CPU_SET, &queue->flags))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		atomic_dec(&nvme_tcp_cpu_queues[queue->io_cpu]);

	mutex_lock(&queue->queue_lock);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
	if (test_and_clear_bit(NVME_TCP_Q_LIVE, &queue->flags))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		__nvme_tcp_stop_queue(queue);
	/* Stopping the queue will disable TLS */
	queue->tls_enabled = false;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	mutex_unlock(&queue->queue_lock);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */
}

static void nvme_tcp_wait_queue(struct nvme_ctrl *nctrl, int qid)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	struct nvme_tcp_ctrl *ctrl = to_tcp_ctrl(nctrl);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct nvme_tcp_queue *queue = &ctrl->queues[qid];	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	int timeout = 100;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	while (timeout > 0) {	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		if (!test_bit(NVME_TCP_Q_ALLOCATED, &queue->flags) ||	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		    !sk_wmem_alloc_get(queue->sock->sk))
			return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
		msleep(2);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		timeout -= 2;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}
	dev_warn(nctrl->device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		 "qid %d: timeout draining sock wmem allocation expired\n",
		 qid);
}

static void nvme_tcp_stop_queue(struct nvme_ctrl *nctrl, int qid)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	nvme_tcp_stop_queue_nowait(nctrl, qid);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	nvme_tcp_wait_queue(nctrl, qid);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
}


static void nvme_tcp_setup_sock_ops(struct nvme_tcp_queue *queue)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
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
}

/*
 * [한국어]
 * nvme_tcp_start_queue - 소켓이 열린 큐를 실제로 가동시킨다
 *
 * @nctrl: 컨트롤러
 * @idx:   큐 번호. 0 이면 admin, 1 이상이면 I/O.
 * @return: 0 이면 LIVE. 음수면 연결 실패이며 큐를 다시 멈춘다.
 *
 * alloc_queue 가 소켓을 열고 ICReq/ICResp 협상까지 마친 뒤, 이 함수가
 * 수신을 켜고 Fabrics Connect 를 보낸다. 그 둘을 나눈 이유는 재연결에서
 * 소켓은 새로 열되 태그셋은 유지해야 하고, 그 경계가 여기이기 때문이다.
 *
 * 순서가 중요하다. rd_enabled 를 세우고 수신 상태를 초기화한 뒤에야 소켓
 * 콜백을 건다(setup_sock_ops). 반대로 하면 콜백이 걸린 직후 도착한 데이터가
 * 아직 초기화되지 않은 수신 상태로 흘러든다.
 *
 * io_cpu 배정을 I/O 큐에서만 하는 이유: admin 큐는 트래픽이 적어 특정
 * 코어에 고정할 이득이 없고, 무엇보다 blk-mq 의 mq_map 은 I/O 큐만 담는다.
 *
 * 실패 시 ALLOCATED 비트를 확인하고 멈추는 것은, 소켓이 실제로 열려 있을
 * 때만 정리할 것이 있기 때문이다.
 *
 * 실행 컨텍스트: 연결 경로. Connect 응답을 기다리며 잠든다.
 *
 * 호출 체인:
 *   nvme_tcp_configure_admin_queue / _start_io_queues → [이 함수]
 *     → nvmf_connect_admin_queue / _io_queue
 */
static int nvme_tcp_start_queue(struct nvme_ctrl *nctrl, int idx)
{
	struct nvme_tcp_ctrl *ctrl = to_tcp_ctrl(nctrl);
	struct nvme_tcp_queue *queue = &ctrl->queues[idx];
	int ret;

	queue->rd_enabled = true;	/* [한국어] 이제 콜백이 io_work 를 깨워도 된다 */
	nvme_tcp_init_recv_ctx(queue);	/* [한국어] 수신 상태를 헤더 대기로 세운다 */
	nvme_tcp_setup_sock_ops(queue);	/* [한국어] 콜백은 마지막에 건다 — 먼저 걸면 초기화 전 상태로 데이터가 흘러든다 */

	if (idx) {	/* [한국어] I/O 큐 */
		nvme_tcp_set_queue_io_cpu(queue);	/* [한국어] mq_map 은 I/O 큐만 담으므로 admin 에는 적용하지 않는다 */
		ret = nvmf_connect_io_queue(nctrl, idx);
	} else
		ret = nvmf_connect_admin_queue(nctrl);

	if (!ret) {
		set_bit(NVME_TCP_Q_LIVE, &queue->flags);	/* [한국어] 이제 이 큐로 명령을 보낼 수 있다 */
	} else {
		if (test_bit(NVME_TCP_Q_ALLOCATED, &queue->flags))	/* [한국어] 소켓이 실제로 열려 있을 때만 정리할 것이 있다 */
			__nvme_tcp_stop_queue(queue);
		dev_err(nctrl->device,
			"failed to connect queue: %d ret=%d\n", idx, ret);
	}
	return ret;
}

static void nvme_tcp_free_admin_queue(struct nvme_ctrl *ctrl)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	if (to_tcp_ctrl(ctrl)->async_req.pdu) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		cancel_work_sync(&ctrl->async_event_work);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_tcp_free_async_req(to_tcp_ctrl(ctrl));	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		to_tcp_ctrl(ctrl)->async_req.pdu = NULL;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}

	nvme_tcp_free_queue(ctrl, 0);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
}

static void nvme_tcp_free_io_queues(struct nvme_ctrl *ctrl)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	int i;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	for (i = 1; i < ctrl->queue_count; i++)	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		nvme_tcp_free_queue(ctrl, i);
}

static void nvme_tcp_stop_io_queues(struct nvme_ctrl *ctrl)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	int i;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	for (i = 1; i < ctrl->queue_count; i++)	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		nvme_tcp_stop_queue_nowait(ctrl, i);
	for (i = 1; i < ctrl->queue_count; i++)	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		nvme_tcp_wait_queue(ctrl, i);
}

static int nvme_tcp_start_io_queues(struct nvme_ctrl *ctrl,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
				    int first, int last)
{
	int i, ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	for (i = first; i < last; i++) {	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		ret = nvme_tcp_start_queue(ctrl, i);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			goto out_stop_queues;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

out_stop_queues:
	for (i--; i >= first; i--)	/* [한국어] 순회 — 큐·요청·세그먼트·이벤트 처리 */
		nvme_tcp_stop_queue(ctrl, i);
	return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

/*
 * [한국어]
 * nvme_tcp_alloc_admin_queue - admin 큐 소켓을 열고 AEN 요청을 준비한다
 *
 * @ctrl: 대상 컨트롤러
 * @return: 0 이면 소켓과 AEN 버퍼가 준비됐다. -ENOKEY 면 TLS 키를 못 찾았다.
 *
 * 이 함수의 대부분은 TLS 키를 고르는 일이다. 세 갈래가 있다:
 *   - 사용자가 tls_key 를 직접 지정했으면 그것을 쓴다.
 *   - tls 만 켜져 있으면 호스트 NQN 과 서브시스템 NQN 으로 키링에서 찾는다.
 *     이름 쌍이 곧 키의 식별자다.
 *   - TLS 를 안 쓰면 pskid 가 0 으로 남고, alloc_queue 가 평문 소켓을 연다.
 *
 * 키를 못 찾으면 연결 자체를 실패시킨다. TLS 를 요구했는데 평문으로 붙는
 * 것은 사용자의 의도에 반하기 때문이다.
 *
 * AEN 요청 버퍼를 여기서 잡는 이유: 그것은 admin 큐에만 딸리고, 태그셋
 * 밖의 단일 객체라 큐 수명과 함께 관리해야 한다.
 *
 * 실행 컨텍스트: 연결 경로. 잠들 수 있다.
 *
 * 호출 체인:
 *   nvme_tcp_configure_admin_queue → [이 함수] → nvme_tcp_alloc_queue
 */
static int nvme_tcp_alloc_admin_queue(struct nvme_ctrl *ctrl)
{
	int ret;
	key_serial_t pskid = 0;	/* [한국어] 0 이면 평문 — alloc_queue 가 이 값으로 TLS 여부를 가른다 */

	if (nvme_tcp_tls_configured(ctrl)) {
		if (ctrl->opts->tls_key)	/* [한국어] 사용자가 키를 직접 지정했다 */
			pskid = key_serial(ctrl->opts->tls_key);
		else if (ctrl->opts->tls) {	/* [한국어] tls 만 켜져 있으면 키링에서 찾는다 */
			pskid = nvme_tls_psk_default(ctrl->opts->keyring,
						      ctrl->opts->host->nqn,	/* [한국어] 호스트와 서브시스템 NQN 쌍이 곧 키의 식별자다 */
						      ctrl->opts->subsysnqn);
			if (!pskid) {
				dev_err(ctrl->device, "no valid PSK found\n");
				return -ENOKEY;	/* [한국어] TLS 를 요구했는데 평문으로 붙는 것은 사용자 의도에 반한다 */
			}
		}
	}

	ret = nvme_tcp_alloc_queue(ctrl, 0, pskid);	/* [한국어] 소켓을 열고 필요하면 TLS 핸드셰이크까지 */
	if (ret)
		return ret;

	ret = nvme_tcp_alloc_async_req(to_tcp_ctrl(ctrl));	/* [한국어] AEN 은 admin 큐에만 딸리므로 큐 수명과 함께 관리한다 */
	if (ret)
		goto out_free_queue;

	return 0;

out_free_queue:
	nvme_tcp_free_queue(ctrl, 0);
	return ret;
}

/*
 * [한국어]
 * __nvme_tcp_alloc_io_queues - I/O 큐 소켓들을 연다
 *
 * @ctrl: 큐 개수가 이미 협상된 컨트롤러
 * @return: 0 이면 모두 열렸다. -ENOKEY 면 TLS 키가 준비되지 않았다.
 *
 * I/O 큐는 admin 큐가 확보한 PSK 를 재사용한다. 큐마다 핸드셰이크를 다시
 * 하지 않는 것이 요점이며, 그래서 여기서는 키를 찾는 대신 "쓸 수 있는
 * 상태인가"를 확인만 한다.
 *
 * 확인이 두 갈래인 것은 concat 여부 때문이다. secure concatenation 에서는
 * PSK 가 인증 결과로부터 파생되어 fabric 옵션에 저장되므로(위 영어 주석),
 * 그 키가 실제로 생겼는지 보고, 아울러 ctrl->tls_pskid 가 그 키와 다르면
 * 낡은 것으로 판단해 지운다 -- 재연결에서 이전 세션의 PSK 가 남아 있을 수
 * 있는데, 그것으로 붙으면 타겟이 거절한다.
 *
 * concat 이 아니면 admin 연결에서 이미 협상된 tls_pskid 가 있어야 한다.
 * 없다면 admin 은 TLS 로 붙었는데 I/O 는 평문으로 붙는 모순이 생긴다.
 *
 * 실행 컨텍스트: 연결 경로. 큐마다 핸드셰이크가 없어 admin 보다 빠르다.
 *
 * 호출 체인:
 *   nvme_tcp_alloc_io_queues → [이 함수] → nvme_tcp_alloc_queue
 */
static int __nvme_tcp_alloc_io_queues(struct nvme_ctrl *ctrl)
{
	int i, ret;

	if (nvme_tcp_tls_configured(ctrl)) {
		if (ctrl->opts->concat) {
			/*
			 * The generated PSK is stored in the
			 * fabric options
			 */
			/* [한국어] 위 영어 주석대로 인증에서 파생된 PSK 는 옵션에 저장된다 */
			if (!ctrl->opts->tls_key) {
				dev_err(ctrl->device, "no PSK generated\n");
				return -ENOKEY;	/* [한국어] 파생이 아직 안 됐다 — admin 인증이 끝나지 않았다는 뜻 */
			}
			if (ctrl->tls_pskid &&
			    ctrl->tls_pskid != key_serial(ctrl->opts->tls_key)) {	/* [한국어] 재연결에서 이전 세션의 PSK 가 남아 있을 수 있다 */
				dev_err(ctrl->device, "Stale PSK id %08x\n", ctrl->tls_pskid);
				ctrl->tls_pskid = 0;	/* [한국어] 낡은 키로 붙으면 타겟이 거절하므로 지운다 */
			}
		} else if (!ctrl->tls_pskid) {	/* [한국어] 일반 TLS — admin 연결에서 협상된 것이 있어야 한다 */
			dev_err(ctrl->device, "no PSK negotiated\n");
			return -ENOKEY;	/* [한국어] 없으면 admin 은 TLS 인데 I/O 는 평문이 되는 모순이 생긴다 */
		}
	}

	for (i = 1; i < ctrl->queue_count; i++) {	/* [한국어] 1 부터 — 0 번 admin 은 이미 열려 있다 */
		ret = nvme_tcp_alloc_queue(ctrl, i,
				ctrl->tls_pskid);	/* [한국어] admin 이 확보한 PSK 를 재사용한다 — 큐마다 핸드셰이크를 다시 하지 않는다 */
		if (ret)
			goto out_free_queues;
	}

	return 0;

out_free_queues:
	for (i--; i >= 1; i--)	/* [한국어] 실패한 것 직전부터 역순으로 되감는다 */
		nvme_tcp_free_queue(ctrl, i);

	return ret;
}

static int nvme_tcp_alloc_io_queues(struct nvme_ctrl *ctrl)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	unsigned int nr_io_queues;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	nr_io_queues = nvmf_nr_io_queues(ctrl->opts);	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
	ret = nvme_set_queue_count(ctrl, &nr_io_queues);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	if (nr_io_queues == 0) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_err(ctrl->device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"unable to set any I/O queues\n");
		return -ENOMEM;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}

	ctrl->queue_count = nr_io_queues + 1;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	dev_info(ctrl->device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		"creating %d I/O queues.\n", nr_io_queues);

	nvmf_set_io_queues(ctrl->opts, nr_io_queues,	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
			   to_tcp_ctrl(ctrl)->io_queues);
	return __nvme_tcp_alloc_io_queues(ctrl);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

/*
 * [한국어]
 * nvme_tcp_configure_io_queues - I/O 큐를 만들고 태그셋과 짝지어 가동한다
 *
 * @ctrl: 대상 컨트롤러
 * @new:  최초 생성이면 태그셋도 만든다. 재연결이면 기존 것을 쓴다.
 * @return: 0 이면 모든 큐가 LIVE. 음수면 되감았다.
 *
 * rdma.c 의 같은 이름 함수와 구조가 같고, 그 이유도 같다. 재연결에서 큐
 * 개수가 달라질 수 있는데 태그셋은 다시 만들 수 없다 -- gendisk 와 진행 중인
 * 요청이 그 위에 매달려 있기 때문이다. 그래서 개수 변경을 태그셋에 반영해야
 * 하고, 그 반영은 진행 중 I/O 가 없을 때만 안전하다. freeze 가 그 상태를
 * 만든다.
 *
 * 태그셋 맵 개수를 정하는 삼항식이 TCP 고유다. 폴링 큐를 쓰면 기본·읽기·
 * 폴링 세 종류가 모두 필요하고(HCTX_MAX_TYPES), 아니면 기본과 읽기 둘이면
 * 충분하다. 이 값이 map_queues 가 돌 맵의 개수가 된다.
 *
 * freeze 대기가 타임아웃되면 초기화를 실패시키는 것은 위 영어 주석의 판단
 * 그대로다 -- 끝나지 않는다는 것은 이미 무언가에 막혀 있다는 뜻이므로
 * 억지로 진행하지 않는다. 그 경로에서도 unfreeze 는 반드시 부른다.
 * 빠뜨리면 큐가 영구히 멈춘 채 남는다.
 *
 * 큐 가동이 두 번으로 나뉜 것도 같은 제약에서 나온다. freeze 를 기다리는
 * 동안에는 태그셋이 아는 큐만 돌아야 하고, 늘어난 몫은 갱신 뒤에 가동한다.
 *
 * 실행 컨텍스트: 연결 경로. freeze 대기로 오래 잠들 수 있다.
 *
 * 호출 체인:
 *   nvme_tcp_setup_ctrl → [이 함수] → nvme_tcp_alloc_io_queues
 *     → blk_mq_update_nr_hw_queues → nvme_tcp_start_io_queues
 */
static int nvme_tcp_configure_io_queues(struct nvme_ctrl *ctrl, bool new)
{
	int ret, nr_queues;

	ret = nvme_tcp_alloc_io_queues(ctrl);	/* [한국어] 개수 협상과 소켓 생성 */
	if (ret)
		return ret;

	if (new) {
		ret = nvme_alloc_io_tag_set(ctrl, &to_tcp_ctrl(ctrl)->tag_set,
				&nvme_tcp_mq_ops,
				ctrl->opts->nr_poll_queues ? HCTX_MAX_TYPES : 2,	/* [한국어] 폴링 큐를 쓰면 세 종류 맵이 필요하고, 아니면 기본·읽기 둘이면 된다 */
				sizeof(struct nvme_tcp_request));
		if (ret)
			goto out_free_io_queues;
	}

	/*
	 * Only start IO queues for which we have allocated the tagset
	 * and limited it to the available queues. On reconnects, the
	 * queue number might have changed.
	 */
	/* [한국어] 위 영어 주석대로 태그셋이 아는 범위만 먼저 가동한다.
	 * 아래 freeze 를 기다리는 동안 태그셋이 모르는 큐로 요청이 가면 안 된다. */
	nr_queues = min(ctrl->tagset->nr_hw_queues + 1, ctrl->queue_count);
	ret = nvme_tcp_start_io_queues(ctrl, 1, nr_queues);
	if (ret)
		goto out_cleanup_connect_q;

	if (!new) {	/* [한국어] 재연결 — 태그셋을 재사용하므로 개수 변경을 반영해야 한다 */
		nvme_start_freeze(ctrl);	/* [한국어] 새 요청을 막고 */
		nvme_unquiesce_io_queues(ctrl);	/* [한국어] 이미 들어온 것은 흘려보내 끝나게 한다 */
		if (!nvme_wait_freeze_timeout(ctrl, NVME_IO_TIMEOUT)) {
			/*
			 * If we timed out waiting for freeze we are likely to
			 * be stuck.  Fail the controller initialization just
			 * to be safe.
			 */
			/* [한국어] 위 영어 주석대로 freeze 가 끝나지 않는다는 것은 이미
			 * 막혀 있다는 뜻이다. 억지로 진행하지 않고 실패시킨다. */
			ret = -ENODEV;
			nvme_unfreeze(ctrl);	/* [한국어] 실패 경로에서도 반드시 푼다 — 빠뜨리면 큐가 영구히 멈춘다 */
			goto out_wait_freeze_timed_out;
		}
		blk_mq_update_nr_hw_queues(ctrl->tagset,	/* [한국어] 진행 중 I/O 가 없는 지금이 개수를 바꿀 수 있는 유일한 시점이다 */
			ctrl->queue_count - 1);
		nvme_unfreeze(ctrl);
	}

	/*
	 * If the number of queues has increased (reconnect case)
	 * start all new queues now.
	 */
	/* [한국어] 위 영어 주석대로 늘어난 몫은 태그셋 갱신 뒤에야 가동할 수 있다. */
	ret = nvme_tcp_start_io_queues(ctrl, nr_queues,
				       ctrl->tagset->nr_hw_queues + 1);
	if (ret)
		goto out_wait_freeze_timed_out;

	return 0;

out_wait_freeze_timed_out:
	nvme_quiesce_io_queues(ctrl);
	nvme_sync_io_queues(ctrl);
	nvme_tcp_stop_io_queues(ctrl);
out_cleanup_connect_q:
	nvme_cancel_tagset(ctrl);	/* [한국어] 남은 요청을 실패로 완료시킨다 */
	if (new)
		nvme_remove_io_tag_set(ctrl);	/* [한국어] 우리가 만든 것일 때만 해제한다 */
out_free_io_queues:
	nvme_tcp_free_io_queues(ctrl);
	return ret;
}

/*
 * [한국어]
 * nvme_tcp_configure_admin_queue - admin 큐를 세우고 컨트롤러를 초기화한다
 *
 * @ctrl: 대상 컨트롤러
 * @new:  최초 생성이면 admin 태그셋도 만든다.
 * @return: 0 이면 Identify 까지 끝났다. 음수면 역순으로 되감았다.
 *
 * 연결의 첫 관문이다. 소켓 → 태그셋 → Connect → CC.EN → Identify 순으로
 * 세우며, 여기가 끝나야 컨트롤러의 능력을 알 수 있고 I/O 큐 개수를
 * 협상할 수 있다.
 *
 * concat 조기 반환이 이 함수의 특이한 부분이다. TLS PSK 를 인증 결과로부터
 * 파생하는 구성에서는, 첫 연결이 평문으로 인증만 마치고 여기서 멈춘다.
 * PSK 가 생긴 뒤 setup_ctrl 이 이 함수를 다시 부르며, 그때 비로소
 * CC.EN 부터 이어 간다. 그래서 enable/Identify 를 건너뛰고 0 을 돌려준다.
 *
 * unquiesce 를 enable 과 Identify 사이에 두는 이유: CC.EN 이 끝나야
 * 컨트롤러가 명령을 받을 준비가 되고, Identify 는 그 큐로 나가는 첫
 * 실제 명령이다. 그전에 열면 준비되지 않은 컨트롤러로 요청이 나간다.
 *
 * 되감기 사다리가 quiesce → stop → cancel → tagset → queue 순인 것도
 * 같은 논리의 역순이다. 새 요청을 막고, 큐를 멈추고, 남은 것을 실패시킨 뒤
 * 자원을 푼다.
 *
 * 실행 컨텍스트: 연결 경로. 잠들 수 있다.
 *
 * 호출 체인:
 *   nvme_tcp_setup_ctrl → [이 함수] → nvme_tcp_alloc_admin_queue
 *     → nvme_tcp_start_queue → nvme_enable_ctrl → nvme_init_ctrl_finish
 */
static int nvme_tcp_configure_admin_queue(struct nvme_ctrl *ctrl, bool new)
{
	int error;

	error = nvme_tcp_alloc_admin_queue(ctrl);	/* [한국어] 소켓을 열고 ICReq/ICResp 협상까지 */
	if (error)
		return error;

	if (new) {
		error = nvme_alloc_admin_tag_set(ctrl,	/* [한국어] 최초 생성에서만 — 재연결은 기존 태그셋을 쓴다 */
				&to_tcp_ctrl(ctrl)->admin_tag_set,
				&nvme_tcp_admin_mq_ops,
				sizeof(struct nvme_tcp_request));
		if (error)
			goto out_free_queue;
	}

	error = nvme_tcp_start_queue(ctrl, 0);	/* [한국어] 수신을 켜고 Fabrics Connect 를 보낸다 */
	if (error)
		goto out_cleanup_tagset;

	if (ctrl->opts->concat && !ctrl->tls_pskid)	/* [한국어] secure concatenation 의 첫 연결 — 인증만 마치고 여기서 멈춘다 */
		return 0;	/* [한국어] PSK 가 생긴 뒤 setup_ctrl 이 이 함수를 다시 부르며 그때 아래를 이어 간다 */

	error = nvme_enable_ctrl(ctrl);	/* [한국어] CC.EN 을 세우고 CSTS.RDY 를 기다린다(Property Set 으로) */
	if (error)
		goto out_stop_queue;

	nvme_unquiesce_admin_queue(ctrl);	/* [한국어] CC.EN 이 끝난 지금에야 연다 — 그전에 열면 준비 안 된 컨트롤러로 요청이 나간다 */

	error = nvme_init_ctrl_finish(ctrl, false);	/* [한국어] Identify 를 읽는다. 이 큐로 나가는 첫 실제 명령이다 */
	if (error)
		goto out_quiesce_queue;

	return 0;

out_quiesce_queue:	/* [한국어] 아래는 세운 순서의 역순이다 */
	nvme_quiesce_admin_queue(ctrl);	/* [한국어] 새 요청을 막고 */
	blk_sync_queue(ctrl->admin_q);	/* [한국어] 진입한 것이 끝나기를 기다린다 */
out_stop_queue:
	nvme_tcp_stop_queue(ctrl, 0);	/* [한국어] 소켓을 닫는다 */
	nvme_cancel_admin_tagset(ctrl);	/* [한국어] 남은 요청을 실패로 완료시킨다 */
out_cleanup_tagset:
	if (new)
		nvme_remove_admin_tag_set(ctrl);	/* [한국어] 우리가 만든 것일 때만 */
out_free_queue:
	nvme_tcp_free_admin_queue(ctrl);
	return error;
}

static void nvme_tcp_teardown_admin_queue(struct nvme_ctrl *ctrl,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		bool remove)
{
	nvme_quiesce_admin_queue(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	blk_sync_queue(ctrl->admin_q);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_tcp_stop_queue(ctrl, 0);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	nvme_cancel_admin_tagset(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (remove) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_unquiesce_admin_queue(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_remove_admin_tag_set(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}
	nvme_tcp_free_admin_queue(ctrl);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	if (ctrl->tls_pskid) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		dev_dbg(ctrl->device, "Wipe negotiated TLS_PSK %08x\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			ctrl->tls_pskid);
		ctrl->tls_pskid = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}
}

static void nvme_tcp_teardown_io_queues(struct nvme_ctrl *ctrl,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		bool remove)
{
	if (ctrl->queue_count <= 1)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	nvme_quiesce_io_queues(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_sync_io_queues(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_tcp_stop_io_queues(ctrl);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	nvme_cancel_tagset(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (remove) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_unquiesce_io_queues(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_remove_io_tag_set(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	}
	nvme_tcp_free_io_queues(ctrl);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
}

static void nvme_tcp_reconnect_or_remove(struct nvme_ctrl *ctrl,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		int status)
{
	enum nvme_ctrl_state state = nvme_ctrl_state(ctrl);	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	/* If we are resetting/deleting then do nothing */
	if (state != NVME_CTRL_CONNECTING) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		WARN_ON_ONCE(state == NVME_CTRL_NEW || state == NVME_CTRL_LIVE);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		return;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}

	if (nvmf_should_reconnect(ctrl, status)) {	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
		dev_info(ctrl->device, "Reconnecting in %d seconds...\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			ctrl->opts->reconnect_delay);
		queue_delayed_work(nvme_wq, &to_tcp_ctrl(ctrl)->connect_work,	/* [한국어] 워크큐 — 송수신/재연결/복구 비동기 실행 */
				ctrl->opts->reconnect_delay * HZ);
	} else {
		dev_info(ctrl->device, "Removing controller (%d)...\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			 status);
		nvme_delete_ctrl(ctrl);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
	}
}

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
{
	return ctrl->opts->concat && ctrl->opts->tls_key && ctrl->tls_pskid;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

/*
 * [한국어]
 * nvme_tcp_setup_ctrl - 컨트롤러를 admin 부터 LIVE 까지 세운다
 *
 * @ctrl: 세울 컨트롤러
 * @new:  최초 생성인가(true), 재연결인가(false).
 *        태그셋을 만들지 재사용할지, 실패 시 어디까지 되감을지가 갈린다.
 * @return: 0 이면 LIVE. 음수면 실패이며 잡은 것은 역순으로 되돌렸다.
 *
 * 연결 절차의 뼈대다: admin 큐 → 능력 검증 → 큐 깊이 조정 → I/O 큐 → LIVE.
 * FC 의 create_association 과 같은 자리이지만 훨씬 짧다. FC 는 세션 계층을
 * 직접 세워야 하는 반면 여기서는 소켓을 열고 Connect 를 보내면 되기 때문이다.
 *
 * secure concatenation 재시작이 이 함수의 특이한 부분이다. TLS PSK 를 인증
 * 결과로부터 파생하는 구성(concat)에서는, 첫 admin 연결이 평문으로 인증을
 * 마쳐야 비로소 PSK 가 생긴다. 그래서 그 PSK 로 다시 연결하려면 admin 큐를
 * 한 번 내렸다 올려야 한다. tls_pskid 가 아직 없다는 것이 그 시점의 표시다.
 *
 * icdoff 와 SGL 검사는 FC 와 같은 이유로 여기에도 있다 -- 이 트랜스포트가
 * 지원하지 않는 구성을 미리 거른다. TCP 는 캡슐 안 데이터를 인라인으로
 * 다루므로 컨트롤러가 별도 오프셋을 요구하면 성립하지 않는다.
 *
 * 큐 깊이 조정이 두 갈래인 것에 주목할 것. 사용자 요청이 sqsize 를 넘으면
 * 경고만 하고 넘어가지만(어차피 sqsize 가 상한이라 실질 문제가 없다),
 * sqsize 자체가 maxcmd 를 넘으면 값을 줄인다 -- 그대로 두면 컨트롤러가
 * 감당 못 하는 수의 명령을 보내게 된다.
 *
 * 상태 전이 실패를 다루는 방식이 new 에 따라 갈린다. 위 영어 주석대로
 * 삭제가 시작된 경우라면 실패해도 정상이지만, 최초 생성 중이라면 그럴 리가
 * 없으므로 WARN 으로 잡는다.
 *
 * 실행 컨텍스트: connect_work / reset_work. 잠들 수 있다.
 *
 * 호출 체인:
 *   nvme_tcp_create_ctrl / _reconnect_ctrl_work → [이 함수]
 *     → nvme_tcp_configure_admin_queue → _configure_io_queues → nvme_start_ctrl
 */
static int nvme_tcp_setup_ctrl(struct nvme_ctrl *ctrl, bool new)
{
	struct nvmf_ctrl_options *opts = ctrl->opts;
	int ret;

	ret = nvme_tcp_configure_admin_queue(ctrl, new);	/* [한국어] 소켓을 열고 ICReq/ICResp 협상과 Connect 까지 끝낸다 */
	if (ret)
		return ret;

	if (ctrl->opts->concat && !ctrl->tls_pskid) {	/* [한국어] TLS PSK 를 인증 결과에서 파생하는 구성인데 아직 PSK 가 없다 */
		/* See comments for nvme_tcp_key_revoke_needed() */
		/* [한국어] 첫 연결이 평문으로 인증을 마쳐야 PSK 가 생긴다. 그 PSK 로
		 * 다시 연결하려면 admin 큐를 한 번 내렸다 올려야 한다. */
		dev_dbg(ctrl->device, "restart admin queue for secure concatenation\n");
		nvme_stop_keep_alive(ctrl);	/* [한국어] 큐를 내리는 동안 keep-alive 가 돌면 안 된다 */
		nvme_tcp_teardown_admin_queue(ctrl, false);	/* [한국어] false — 태그셋은 남긴다. 곧 다시 쓸 것이다 */
		ret = nvme_tcp_configure_admin_queue(ctrl, false);	/* [한국어] 이번에는 파생된 PSK 로 TLS 연결을 맺는다 */
		if (ret)
			goto destroy_admin;
	}

	if (ctrl->icdoff) {	/* [한국어] 컨트롤러가 캡슐 내 데이터 오프셋을 요구한다 */
		ret = -EOPNOTSUPP;
		dev_err(ctrl->device, "icdoff is not supported!\n");	/* [한국어] 이 구현은 오프셋 0 인 인라인만 다룬다 */
		goto destroy_admin;
	}

	if (!nvme_ctrl_sgl_supported(ctrl)) {	/* [한국어] Fabrics 는 PRP 를 쓰지 않으므로 SGL 지원이 필수다 */
		ret = -EOPNOTSUPP;
		dev_err(ctrl->device, "Mandatory sgls are not supported!\n");
		goto destroy_admin;
	}

	if (opts->queue_size > ctrl->sqsize + 1)	/* [한국어] 사용자가 컨트롤러 상한보다 큰 깊이를 요청했다 */
		dev_warn(ctrl->device,	/* [한국어] 경고만 한다 — 어차피 sqsize 가 실질 상한이라 동작에는 문제가 없다 */
			"queue_size %zu > ctrl sqsize %u, clamping down\n",
			opts->queue_size, ctrl->sqsize + 1);

	if (ctrl->sqsize + 1 > ctrl->maxcmd) {	/* [한국어] 이쪽은 다르다 — sqsize 자체가 컨트롤러가 감당할 수 있는 명령 수를 넘는다 */
		dev_warn(ctrl->device,
			"sqsize %u > ctrl maxcmd %u, clamping down\n",
			ctrl->sqsize + 1, ctrl->maxcmd);
		ctrl->sqsize = ctrl->maxcmd - 1;	/* [한국어] 값을 줄여야 한다. 두면 감당 못 할 수의 명령을 보내게 된다 */
	}

	if (ctrl->queue_count > 1) {	/* [한국어] I/O 큐가 있는 구성이면 */
		ret = nvme_tcp_configure_io_queues(ctrl, new);	/* [한국어] 소켓들을 열고 각각 Connect 한다 */
		if (ret)
			goto destroy_admin;
	}

	if (!nvme_change_ctrl_state(ctrl, NVME_CTRL_LIVE)) {
		/*
		 * state change failure is ok if we started ctrl delete,
		 * unless we're during creation of a new controller to
		 * avoid races with teardown flow.
		 */
		/* [한국어] 위 영어 주석대로, 그 사이 삭제가 시작됐다면 전이 실패는
		 * 정상이다. 다만 최초 생성 중이라면 삭제가 시작될 리 없으므로
		 * 아래 WARN 들이 그 가정을 검증한다. */
		enum nvme_ctrl_state state = nvme_ctrl_state(ctrl);

		WARN_ON_ONCE(state != NVME_CTRL_DELETING &&	/* [한국어] 삭제 중이 아닌데 전이가 실패했다면 상태 기계가 어긋난 것이다 */
			     state != NVME_CTRL_DELETING_NOIO);
		WARN_ON_ONCE(new);	/* [한국어] 최초 생성 중에는 삭제와 겹칠 수 없다 */
		ret = -EINVAL;
		goto destroy_io;
	}

	nvme_start_ctrl(ctrl);	/* [한국어] keep-alive 를 걸고 네임스페이스를 스캔한다 */
	return 0;

destroy_io:
	if (ctrl->queue_count > 1) {
		nvme_quiesce_io_queues(ctrl);	/* [한국어] 새 요청 유입을 멈추고 */
		nvme_sync_io_queues(ctrl);	/* [한국어] 진입한 제출이 끝나기를 기다린 뒤 */
		nvme_tcp_stop_io_queues(ctrl);	/* [한국어] 소켓을 닫는다 */
		nvme_cancel_tagset(ctrl);	/* [한국어] 남은 요청을 실패로 완료시킨다 */
		if (new)
			nvme_remove_io_tag_set(ctrl);	/* [한국어] 최초 생성이었다면 태그셋도 우리가 만든 것이므로 해제한다 */
		nvme_tcp_free_io_queues(ctrl);
	}
destroy_admin:
	nvme_stop_keep_alive(ctrl);
	nvme_tcp_teardown_admin_queue(ctrl, new);	/* [한국어] new 가 태그셋 해제 여부를 결정한다 */
	return ret;
}

/*
 * [한국어]
 * nvme_tcp_reconnect_ctrl_work - 재연결을 한 번 시도한다
 *
 * @work: connect_work(지연 워크)
 * @return: 없음
 *
 * 재연결 루프의 한 바퀴다. 실패하면 reconnect_or_remove 가 이 워크를 다시
 * 예약하므로, 둘이 서로를 부르며 시도 횟수가 소진될 때까지 반복한다.
 *
 * setup_ctrl 에 false 를 넘기는 것이 핵심이다 -- 소켓은 새로 열되 태그셋은
 * 유지한다. 그 위에 gendisk 와 진행 중인 요청이 매달려 있기 때문이다.
 *
 * 성공 시 nr_reconnects 를 0 으로 되돌려 다음 단절이 처음부터 세도록 한다.
 * 로그에 시도 횟수를 함께 남기는 것은 몇 번 만에 붙었는지가 연결 품질을
 * 가늠하는 단서가 되기 때문이다.
 *
 * 실행 컨텍스트: nvme_wq 워크큐. 연결 전체가 여기서 일어나 오래 잠든다.
 *
 * 호출 체인:
 *   nvme_tcp_reconnect_or_remove → queue_delayed_work → [이 함수]
 *     → nvme_tcp_setup_ctrl
 */
static void nvme_tcp_reconnect_ctrl_work(struct work_struct *work)
{
	struct nvme_tcp_ctrl *tcp_ctrl = container_of(to_delayed_work(work),
			struct nvme_tcp_ctrl, connect_work);
	struct nvme_ctrl *ctrl = &tcp_ctrl->ctrl;
	int ret;

	++ctrl->nr_reconnects;	/* [한국어] 시도 횟수. 성공하면 아래에서 되돌린다 */

	ret = nvme_tcp_setup_ctrl(ctrl, false);	/* [한국어] false — 소켓은 새로, 태그셋은 그대로 */
	if (ret)
		goto requeue;

	dev_info(ctrl->device, "Successfully reconnected (attempt %d/%d)\n",	/* [한국어] 몇 번 만에 붙었는지가 연결 품질의 단서다 */
		 ctrl->nr_reconnects, ctrl->opts->max_reconnects);

	ctrl->nr_reconnects = 0;	/* [한국어] 다음 단절이 처음부터 세도록 */

	return;

requeue:
	dev_info(ctrl->device, "Failed reconnect attempt %d/%d\n",
		 ctrl->nr_reconnects, ctrl->opts->max_reconnects);
	nvme_tcp_reconnect_or_remove(ctrl, ret);	/* [한국어] 다시 시도할지 포기할지는 그쪽이 정하며, 다시면 이 워크가 재예약된다 */
}

/*
 * [한국어]
 * nvme_tcp_error_recovery_work - 연결을 통째로 내리고 재연결 루프에 넘긴다
 *
 * @work: err_work
 * @return: 없음
 *
 * error_recovery 가 상태만 옮기고 깨운 워크다. 실제 복구가 여기서 일어난다.
 *
 * 해체 순서에 각각 이유가 있다:
 *   1) TLS 키 폐기 -- 인증에서 파생한 키는 세션과 함께 무효가 된다. 남겨
 *      두면 다음 연결이 낡은 키로 붙으려 하고 타겟이 거절한다.
 *   2) keep-alive 정지 -- 내리는 중에 새 admin 명령이 나가면 안 된다.
 *   3) async_event_work flush -- AEN 재무장이 진행 중일 수 있고, 큐를 내린
 *      뒤에 그것이 돌면 이미 닫힌 소켓으로 명령을 보낸다.
 *   4) 큐 해체 후 곧바로 unquiesce -- 위 영어 주석대로 대기 중인 요청을
 *      빨리 실패시키기 위해서다. 멈춘 채 두면 쌓여 기다리기만 한다.
 *   5) 인증 중단 -- 진행 중인 DH-HMAC-CHAP 협상을 끊는다.
 *
 * false 를 넘겨 태그셋은 남긴다. 재연결에서 다시 쓴다.
 *
 * 실행 컨텍스트: nvme_reset_wq 워크큐. 오래 잠들 수 있다.
 *
 * 호출 체인:
 *   nvme_tcp_error_recovery → [이 함수] → nvme_tcp_teardown_io_queues
 *     → nvme_tcp_reconnect_or_remove
 */
static void nvme_tcp_error_recovery_work(struct work_struct *work)
{
	struct nvme_tcp_ctrl *tcp_ctrl = container_of(work,
				struct nvme_tcp_ctrl, err_work);
	struct nvme_ctrl *ctrl = &tcp_ctrl->ctrl;

	if (nvme_tcp_key_revoke_needed(ctrl))	/* [한국어] 인증에서 파생한 키는 세션과 함께 무효가 된다 */
		nvme_auth_revoke_tls_key(ctrl);	/* [한국어] 남겨 두면 다음 연결이 낡은 키로 붙으려 하고 타겟이 거절한다 */
	nvme_stop_keep_alive(ctrl);	/* [한국어] 내리는 중에 새 admin 명령이 나가면 안 된다 */
	flush_work(&ctrl->async_event_work);	/* [한국어] AEN 재무장이 진행 중일 수 있다 — 큐를 내린 뒤 돌면 닫힌 소켓으로 명령을 보낸다 */
	nvme_tcp_teardown_io_queues(ctrl, false);	/* [한국어] false — 태그셋은 남긴다 */
	/* unquiesce to fail fast pending requests */
	nvme_unquiesce_io_queues(ctrl);	/* [한국어] 위 영어 주석대로 대기 요청을 빨리 실패시킨다. 멈춘 채 두면 쌓여 기다리기만 한다 */
	nvme_tcp_teardown_admin_queue(ctrl, false);
	nvme_unquiesce_admin_queue(ctrl);
	nvme_auth_stop(ctrl);	/* [한국어] 진행 중인 대역 내 인증 협상을 끊는다 */

	if (!nvme_change_ctrl_state(ctrl, NVME_CTRL_CONNECTING)) {
		/* state change failure is ok if we started ctrl delete */
		/* [한국어] 위 영어 주석대로 삭제가 시작된 경우라면 정상이다 */
		enum nvme_ctrl_state state = nvme_ctrl_state(ctrl);

		WARN_ON_ONCE(state != NVME_CTRL_DELETING &&	/* [한국어] 그 밖의 상태라면 상태 기계가 어긋난 것이다 */
			     state != NVME_CTRL_DELETING_NOIO);
		return;	/* [한국어] 삭제 경로가 정리한다 */
	}

	nvme_tcp_reconnect_or_remove(ctrl, 0);	/* [한국어] 재연결 루프에 넘긴다 */
}

static void nvme_tcp_teardown_ctrl(struct nvme_ctrl *ctrl, bool shutdown)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	nvme_tcp_teardown_io_queues(ctrl, shutdown);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	nvme_quiesce_admin_queue(ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	nvme_disable_ctrl(ctrl, shutdown);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */
	nvme_tcp_teardown_admin_queue(ctrl, shutdown);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
}

static void nvme_tcp_delete_ctrl(struct nvme_ctrl *ctrl)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	nvme_tcp_teardown_ctrl(ctrl, true);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
}

/*
 * [한국어]
 * nvme_reset_ctrl_work - 명시적 리셋 요청을 수행한다
 *
 * @work: ctrl.reset_work
 * @return: 없음
 *
 * error_recovery_work 와 하는 일이 겹치지만 진입 경로가 다르다. 이쪽은
 * 오류가 아니라 사용자나 코어의 요청으로 불린다.
 *
 * 그래서 teardown_ctrl 을 쓴다 -- I/O 큐와 admin 큐를 함께 내리면서
 * 컨트롤러에 정상 종료를 알릴 기회를 갖는다. 오류 복구가 각 큐를 따로
 * 내리며 곧바로 unquiesce 하는 것과 대비된다.
 *
 * TLS 키 폐기는 같은 이유로 여기에도 있다. 세션이 바뀌면 파생 키도 무효다.
 *
 * 상태 전이 실패를 삭제 중으로만 허용하는 것은 error_recovery_work 와 같다
 * -- rdma.c 의 reset 이 WARN_ON_ONCE(1) 로 잡는 것과는 다른데, tcp 쪽은
 * 리셋과 삭제가 겹칠 수 있는 경로가 더 있기 때문이다.
 *
 * 실행 컨텍스트: nvme_reset_wq 워크큐. 오래 잠들 수 있다.
 *
 * 호출 체인:
 *   nvme_reset_ctrl → [이 함수] → nvme_tcp_teardown_ctrl → nvme_tcp_setup_ctrl
 */
static void nvme_reset_ctrl_work(struct work_struct *work)
{
	struct nvme_ctrl *ctrl =
		container_of(work, struct nvme_ctrl, reset_work);
	int ret;

	if (nvme_tcp_key_revoke_needed(ctrl))	/* [한국어] 세션이 바뀌면 파생 키도 무효다 */
		nvme_auth_revoke_tls_key(ctrl);
	nvme_stop_ctrl(ctrl);	/* [한국어] keep-alive 와 스캔을 멈춘다 */
	nvme_tcp_teardown_ctrl(ctrl, false);	/* [한국어] 두 큐를 함께 내린다 — 정상 종료를 알릴 기회가 있는 경로다 */

	if (!nvme_change_ctrl_state(ctrl, NVME_CTRL_CONNECTING)) {
		/* state change failure is ok if we started ctrl delete */
		enum nvme_ctrl_state state = nvme_ctrl_state(ctrl);

		WARN_ON_ONCE(state != NVME_CTRL_DELETING &&	/* [한국어] tcp 는 리셋과 삭제가 겹칠 경로가 더 있어 이 실패를 정상으로 본다 */
			     state != NVME_CTRL_DELETING_NOIO);
		return;
	}

	ret = nvme_tcp_setup_ctrl(ctrl, false);	/* [한국어] 태그셋을 재사용해 다시 세운다 */
	if (ret)
		goto out_fail;

	return;

out_fail:
	++ctrl->nr_reconnects;	/* [한국어] 실패했으니 재연결 시도로 계산한다 */
	nvme_tcp_reconnect_or_remove(ctrl, ret);
}

static void nvme_tcp_stop_ctrl(struct nvme_ctrl *ctrl)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	flush_work(&to_tcp_ctrl(ctrl)->err_work);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	cancel_delayed_work_sync(&to_tcp_ctrl(ctrl)->connect_work);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}

static void nvme_tcp_free_ctrl(struct nvme_ctrl *nctrl)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	struct nvme_tcp_ctrl *ctrl = to_tcp_ctrl(nctrl);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	if (list_empty(&ctrl->list))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto free_ctrl;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	mutex_lock(&nvme_tcp_ctrl_mutex);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	list_del(&ctrl->list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	mutex_unlock(&nvme_tcp_ctrl_mutex);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	nvmf_free_options(nctrl->opts);	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
free_ctrl:
	kfree(ctrl->queues);	/* [한국어] 커널 메모리 생명주기 */
	kfree(ctrl);	/* [한국어] 커널 메모리 생명주기 */
}

static void nvme_tcp_set_sg_null(struct nvme_command *c)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	struct nvme_sgl_desc *sg = &c->common.dptr.sgl;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	sg->addr = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	sg->length = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	sg->type = (NVME_TRANSPORT_SGL_DATA_DESC << 4) |	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			NVME_SGL_FMT_TRANSPORT_A;
}

static void nvme_tcp_set_sg_inline(struct nvme_tcp_queue *queue,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		struct nvme_command *c, u32 data_len)	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
{
	struct nvme_sgl_desc *sg = &c->common.dptr.sgl;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	sg->addr = cpu_to_le64(queue->ctrl->ctrl.icdoff);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	sg->length = cpu_to_le32(data_len);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	sg->type = (NVME_SGL_FMT_DATA_DESC << 4) | NVME_SGL_FMT_OFFSET;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
}

static void nvme_tcp_set_sg_host_data(struct nvme_command *c,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		u32 data_len)
{
	struct nvme_sgl_desc *sg = &c->common.dptr.sgl;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	sg->addr = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	sg->length = cpu_to_le32(data_len);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	sg->type = (NVME_TRANSPORT_SGL_DATA_DESC << 4) |	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			NVME_SGL_FMT_TRANSPORT_A;
}

/*
 * [한국어]
 * nvme_tcp_submit_async_event - 비동기 이벤트 명령을 걸어 둔다
 *
 * @arg: 코어가 넘긴 nvme_ctrl
 * @return: 없음
 *
 * AEN 은 완료를 기다리는 명령이 아니라 통지를 예약하는 명령이다. 미완료로
 * 남아 있으므로 blk-mq 태그를 붙들면 I/O 에 쓸 태그가 영구히 하나 줄어든다.
 * 그래서 컨트롤러가 태그셋 밖에 전용 요청(async_req)을 하나 들고 있다.
 *
 * 이 함수가 다른 트랜스포트의 같은 이름 함수보다 긴 이유는 요청 상태까지
 * 직접 초기화해야 하기 때문이다. 일반 요청은 blk-mq 가 태그를 줄 때
 * 초기화해 주지만, 이것은 재사용되는 단일 객체라 매번 전송 단계(state),
 * 오프셋, bio 포인터, 두 목록 노드를 손으로 되돌려야 한다. 하나라도
 * 빠뜨리면 이전 사용분의 상태가 남아 송신 상태 기계가 엉뚱한 단계에서
 * 시작하거나 목록이 꼬인다.
 *
 * command_id 가 NVME_AQ_BLK_MQ_DEPTH 인 것은 admin 태그 범위 바로 위라
 * 실제 태그와 겹치지 않기 때문이며, 응답이 왔을 때 그 값으로 AEN 임을
 * 알아본다.
 *
 * queue_request 에 true(last)를 넘겨 곧바로 io_work 를 깨운다. 뒤에 이어질
 * 요청이 없으므로 모아 보낼 이유가 없다.
 *
 * 실행 컨텍스트: 코어의 AEN 재무장 경로.
 *
 * 호출 체인:
 *   nvme_complete_async_event → ops->submit_async_event → [이 함수]
 *     → nvme_tcp_queue_request
 */
static void nvme_tcp_submit_async_event(struct nvme_ctrl *arg)
{
	struct nvme_tcp_ctrl *ctrl = to_tcp_ctrl(arg);
	struct nvme_tcp_queue *queue = &ctrl->queues[0];	/* [한국어] AEN 은 admin 큐로만 나간다 */
	struct nvme_tcp_cmd_pdu *pdu = ctrl->async_req.pdu;	/* [한국어] 태그셋 밖의 전용 요청이 가진 PDU 버퍼 */
	struct nvme_command *cmd = &pdu->cmd;
	u8 hdgst = nvme_tcp_hdgst_len(queue);

	memset(pdu, 0, sizeof(*pdu));	/* [한국어] 재사용 버퍼라 이전 사용분을 지운다 */
	pdu->hdr.type = nvme_tcp_cmd;
	if (queue->hdr_digest)
		pdu->hdr.flags |= NVME_TCP_F_HDGST;	/* [한국어] 헤더 뒤에 CRC 가 붙는다고 표시 */
	pdu->hdr.hlen = sizeof(*pdu);
	pdu->hdr.plen = cpu_to_le32(pdu->hdr.hlen + hdgst);	/* [한국어] 데이터가 없어 헤더와 다이제스트가 전부다 */

	cmd->common.opcode = nvme_admin_async_event;
	cmd->common.command_id = NVME_AQ_BLK_MQ_DEPTH;	/* [한국어] admin 태그 범위 바로 위 — 응답에서 이 값으로 AEN 임을 알아본다 */
	cmd->common.flags |= NVME_CMD_SGL_METABUF;
	nvme_tcp_set_sg_null(cmd);	/* [한국어] 옮길 데이터가 없다 */

	ctrl->async_req.state = NVME_TCP_SEND_CMD_PDU;	/* [한국어] 아래 다섯 줄이 이 함수가 다른 트랜스포트보다 긴 이유다 */
	ctrl->async_req.offset = 0;	/* [한국어] 재사용되는 단일 객체라 blk-mq 대신 손으로 초기화해야 한다 */
	ctrl->async_req.curr_bio = NULL;	/* [한국어] 하나라도 빠뜨리면 이전 상태가 남아 상태 기계가 엉뚱한 단계에서 시작한다 */
	ctrl->async_req.data_len = 0;
	init_llist_node(&ctrl->async_req.lentry);	/* [한국어] 두 목록 노드도 되돌려야 목록이 꼬이지 않는다 */
	INIT_LIST_HEAD(&ctrl->async_req.entry);

	nvme_tcp_queue_request(&ctrl->async_req, true);	/* [한국어] true(last) — 뒤에 이어질 요청이 없으니 곧바로 io_work 를 깨운다 */
}

static void nvme_tcp_complete_timed_out(struct request *rq)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	struct nvme_tcp_request *req = blk_mq_rq_to_pdu(rq);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct nvme_ctrl *ctrl = &req->queue->ctrl->ctrl;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	nvme_tcp_stop_queue(ctrl, nvme_tcp_queue_id(req->queue));	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	nvmf_complete_timed_out_request(rq);	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
}

/*
 * [한국어]
 * nvme_tcp_timeout - 응답이 오지 않는 요청을 어떻게 처리할지 정한다
 *
 * @rq: 시한을 넘긴 요청
 * @return: BLK_EH_DONE 이면 이 자리에서 요청을 끝냈다는 뜻.
 *          BLK_EH_RESET_TIMER 면 시계를 다시 걸고 복구 경로에 맡긴다는 뜻.
 *
 * PCIe 라면 Abort 명령을 보내 개별 요청을 취소할 수 있지만, 여기서는 그런
 * 수단이 없다. 소켓이 응답하지 않는다는 것은 연결 자체의 문제이므로,
 * 처리는 "이 요청 하나"가 아니라 "이 연결 전체"의 문제로 올라간다.
 *
 * 그래서 판단은 컨트롤러 상태 하나로 갈린다.
 *
 * LIVE 가 아니면 -- 리셋·연결·삭제 중이라면 -- 여기서 즉시 완료시킨다.
 * 위 영어 주석이 그 이유를 열거한다: 이 요청들은 컨트롤러를 내리거나 세우는
 * 절차의 일부이거나 그 절차를 기다리게 만드는 것들이라, 복구 작업이
 * 정리해 주기를 기다리면 그 복구 자체가 이 요청 때문에 막힌다. 교착이다.
 *
 * LIVE 라면 정상적인 오류 복구를 촉발하고 시계만 다시 건다. 요청을 실제로
 * 끝내는 일은 복구 작업이 큐를 내리면서 일괄 처리한다. 여기서 개별적으로
 * 끝내면 복구가 아직 소켓을 만지고 있는 중에 요청이 사라져 어긋난다.
 *
 * 실행 컨텍스트: blk-mq 타임아웃 문맥. 잠들 수 없다.
 *
 * 호출 체인:
 *   blk-mq 타임아웃 → [이 함수] → nvme_tcp_error_recovery / _complete_timed_out
 */
static enum blk_eh_timer_return nvme_tcp_timeout(struct request *rq)
{
	struct nvme_tcp_request *req = blk_mq_rq_to_pdu(rq);
	struct nvme_ctrl *ctrl = &req->queue->ctrl->ctrl;
	struct nvme_tcp_cmd_pdu *pdu = nvme_tcp_req_cmd_pdu(req);	/* [한국어] 로그에 어떤 명령이었는지 남기기 위해 꺼낸다 */
	struct nvme_command *cmd = &pdu->cmd;
	int qid = nvme_tcp_queue_id(req->queue);

	dev_warn(ctrl->device,	/* [한국어] 태그·큐·opcode 를 남긴다 — 어느 큐의 어떤 명령이 막혔는지가 진단의 출발점이다 */
		 "I/O tag %d (%04x) type %d opcode %#x (%s) QID %d timeout\n",
		 rq->tag, nvme_cid(rq), pdu->hdr.type, cmd->common.opcode,
		 nvme_fabrics_opcode_str(qid, cmd), qid);

	if (nvme_ctrl_state(ctrl) != NVME_CTRL_LIVE) {
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
		/* [한국어] 위 영어 주석이 목록으로 밝힌 그대로다. 이 요청들은 컨트롤러를
		 * 내리거나 세우는 절차의 일부여서, 복구 작업이 정리해 주기를 기다리면
		 * 그 복구가 이 요청 때문에 막히는 교착이 된다. 그래서 즉시 끝낸다. */
		nvme_tcp_complete_timed_out(rq);
		return BLK_EH_DONE;	/* [한국어] 이 자리에서 완료시켰으므로 blk-mq 가 더 할 일이 없다 */
	}

	/*
	 * LIVE state should trigger the normal error recovery which will
	 * handle completing this request.
	 */
	/* [한국어] 정상 동작 중 타임아웃이면 연결 전체의 문제로 본다. 요청을 여기서
	 * 끝내지 않는 것이 중요하다 — 복구가 소켓을 만지는 중에 요청이 사라지면 어긋난다. */
	nvme_tcp_error_recovery(ctrl);
	return BLK_EH_RESET_TIMER;	/* [한국어] 시계만 다시 걸고, 실제 완료는 복구 작업이 큐를 내리며 일괄 처리한다 */
}

static blk_status_t nvme_tcp_map_data(struct nvme_tcp_queue *queue,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
			struct request *rq)	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
{
	struct nvme_tcp_request *req = blk_mq_rq_to_pdu(rq);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct nvme_tcp_cmd_pdu *pdu = nvme_tcp_req_cmd_pdu(req);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	struct nvme_command *c = &pdu->cmd;	/* [한국어] 트랜스포트 상태/요청 모델 타입 */

	c->common.flags |= NVME_CMD_SGL_METABUF;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	if (!blk_rq_nr_phys_segments(rq))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_tcp_set_sg_null(c);
	else if (rq_data_dir(rq) == WRITE &&	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
	    req->data_len <= nvme_tcp_inline_data_size(req))
		nvme_tcp_set_sg_inline(queue, c, req->data_len);
	else	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvme_tcp_set_sg_host_data(c, req->data_len);

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static blk_status_t nvme_tcp_setup_cmd_pdu(struct nvme_ns *ns,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		struct request *rq)	/* [한국어] 트랜스포트 상태/요청 모델 타입 */
{
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
				blk_rq_payload_bytes(rq) : 0;
	req->curr_bio = rq->bio;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (req->curr_bio && req->data_len)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_tcp_init_iter(req, rq_data_dir(rq));

	if (rq_data_dir(rq) == WRITE &&	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
	    req->data_len <= nvme_tcp_inline_data_size(req))
		req->pdu_len = req->data_len;

	pdu->hdr.type = nvme_tcp_cmd;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	pdu->hdr.flags = 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (queue->hdr_digest)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		pdu->hdr.flags |= NVME_TCP_F_HDGST;
	if (queue->data_digest && req->pdu_len) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		pdu->hdr.flags |= NVME_TCP_F_DDGST;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		ddgst = nvme_tcp_ddgst_len(queue);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	}
	pdu->hdr.hlen = sizeof(*pdu);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	pdu->hdr.pdo = req->pdu_len ? pdu->hdr.hlen + hdgst : 0;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	pdu->hdr.plen =	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		cpu_to_le32(pdu->hdr.hlen + hdgst + req->pdu_len + ddgst);

	ret = nvme_tcp_map_data(queue, rq);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	if (unlikely(ret)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		nvme_cleanup_cmd(rq);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		dev_err(queue->ctrl->ctrl.device,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			"Failed to map data (%d)\n", ret);
		return ret;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
	}

	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static void nvme_tcp_commit_rqs(struct blk_mq_hw_ctx *hctx)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	struct nvme_tcp_queue *queue = hctx->driver_data;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	if (!llist_empty(&queue->req_list))	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		queue_work_on(queue->io_cpu, nvme_tcp_wq, &queue->io_work);
}

/*
 * [한국어]
 * nvme_tcp_queue_rq - blk-mq 가 요청 하나를 이 트랜스포트에 넘기는 진입점
 *
 * @hctx: 이 요청이 배정된 하드웨어 큐 문맥. driver_data 에 nvme_tcp_queue 가 들어 있다.
 * @bd:   요청과 배치 힌트(last). last 가 false 면 뒤에 더 올 것이 있다는 뜻이다.
 * @return: BLK_STS_OK 면 접수. 그 밖의 값은 blk-mq 가 재큐잉하거나 실패시킨다.
 *
 * 여기서 실제로 소켓에 쓰지 않는다는 점이 RDMA/PCI 와 크게 다르다. 이 함수는
 * PDU 를 준비해 큐의 입력 목록(req_list)에 밀어 넣고 끝난다. 실제 전송은
 * io_work 가 자기 CPU 에서 한다. 그래야 제출자가 소켓 쓰기의 블로킹에
 * 붙잡히지 않고, 큐당 전송이 한 문맥으로 직렬화된다.
 *
 * nvme_check_ready 가 앞에 오는 이유: 큐가 아직 LIVE 가 아니거나 컨트롤러가
 * 재연결 중이면 요청을 보낼 수 없다. 그때 그냥 실패시킬지, 재시도로 돌릴지,
 * 다중 경로로 넘길지는 코어의 정책이라 nvme_fail_nonready_command 에 맡긴다.
 *
 * bd->last 를 넘기는 이유: 마지막 요청이면 io_work 를 즉시 깨워야 하고,
 * 뒤에 더 올 것이 있으면 모아서 한 번에 처리하는 편이 낫다. 배치 힌트가
 * 소켓 write 횟수를 줄인다.
 *
 * 실행 컨텍스트: blk-mq 제출 경로. 잠들 수 없다.
 *
 * 호출 체인:
 *   submit_bio → blk-mq → [이 함수] → nvme_tcp_setup_cmd_pdu
 *     → nvme_tcp_queue_request → (io_work 가 실제 전송)
 */
static blk_status_t nvme_tcp_queue_rq(struct blk_mq_hw_ctx *hctx,
		const struct blk_mq_queue_data *bd)
{
	struct nvme_ns *ns = hctx->queue->queuedata;	/* [한국어] 이 요청이 향하는 네임스페이스. admin 명령이면 NULL 이다 */
	struct nvme_tcp_queue *queue = hctx->driver_data;	/* [한국어] blk-mq 하드웨어 큐에 붙여 둔 우리 큐 */
	struct request *rq = bd->rq;
	struct nvme_tcp_request *req = blk_mq_rq_to_pdu(rq);	/* [한국어] 태그에 미리 붙어 있는 이 요청의 TCP 상태 */
	bool queue_ready = test_bit(NVME_TCP_Q_LIVE, &queue->flags);	/* [한국어] Connect 까지 끝나 명령을 받을 수 있는 큐인가 */
	blk_status_t ret;

	if (!nvme_check_ready(&queue->ctrl->ctrl, rq, queue_ready))	/* [한국어] 컨트롤러 상태와 큐 준비 여부를 코어 정책으로 판정 */
		return nvme_fail_nonready_command(&queue->ctrl->ctrl, rq);	/* [한국어] 재시도할지 다중 경로로 넘길지는 코어가 정한다 */

	ret = nvme_tcp_setup_cmd_pdu(ns, rq);	/* [한국어] 명령을 PDU 로 조립하고 인라인 여부·전송 단계를 정한다 */
	if (unlikely(ret))
		return ret;

	nvme_start_request(rq);	/* [한국어] 타임아웃 시계를 시작하고 통계를 연다 — 이 시점부터 요청이 '진행 중'이다 */

	nvme_tcp_queue_request(req, bd->last);	/* [한국어] 입력 목록에 넣는다. 여기서 소켓에 쓰지 않는 것이 이 트랜스포트의 특징이다 */

	return BLK_STS_OK;	/* [한국어] 접수 완료. 실제 전송과 완료는 io_work 와 수신 경로가 맡는다 */
}

static void nvme_tcp_map_queues(struct blk_mq_tag_set *set)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	struct nvme_tcp_ctrl *ctrl = to_tcp_ctrl(set->driver_data);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	nvmf_map_queues(set, &ctrl->ctrl, ctrl->io_queues);	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
}

/*
 * [한국어]
 * nvme_tcp_poll - 인터럽트 없이 이 큐의 완료를 직접 수확한다
 *
 * @hctx: 폴링 큐의 하드웨어 큐 문맥
 * @iob:  완료를 모아 일괄 처리하기 위한 배치(이 트랜스포트는 쓰지 않는다)
 * @return: 이번에 완료시킨 요청 수. 음수면 오류.
 *
 * 저지연 경로다. 소켓 콜백이 io_work 를 깨우고 워크큐가 스케줄되기를 기다리는
 * 대신, 제출한 스레드가 그 자리에서 수신을 돌려 완료를 가져간다. 깨우기와
 * 문맥 전환이 통째로 사라진다.
 *
 * POLLING 비트를 세우는 것이 이 함수의 핵심이다. 이 비트가 서 있는 동안
 * data_ready 콜백은 io_work 를 깨우지 않는다. 그러지 않으면 같은 큐를 폴링
 * 스레드와 io_work 가 동시에 수신 처리해 PDU 파싱 상태가 깨진다.
 *
 * sk_busy_loop 은 수신 큐가 비어 있을 때 NIC 를 직접 돌려 패킷을 끌어온다.
 * 인터럽트 지연조차 기다리지 않겠다는 뜻이며, 폴링 큐를 쓰는 이유와 같은 선택이다.
 *
 * nr_cqe 를 돌려주는 이유: blk-mq 는 "몇 개를 완료시켰는가"를 요구하고,
 * try_recv 가 그 개수를 세어 큐에 남겨 둔다.
 *
 * 실행 컨텍스트: 제출한 스레드가 직접 부른다. lock_sock 때문에 잠들 수 있다.
 *
 * 호출 체인:
 *   blk_mq_poll → [이 함수] → nvme_tcp_try_recv
 */
static int nvme_tcp_poll(struct blk_mq_hw_ctx *hctx, struct io_comp_batch *iob)
{
	struct nvme_tcp_queue *queue = hctx->driver_data;
	struct sock *sk = queue->sock->sk;
	int ret;

	if (!test_bit(NVME_TCP_Q_LIVE, &queue->flags))	/* [한국어] 아직 준비되지 않은 큐에서는 수확할 것이 없다 */
		return 0;

	set_bit(NVME_TCP_Q_POLLING, &queue->flags);	/* [한국어] 이 동안 data_ready 가 io_work 를 깨우지 못하게 막는다 — 두 문맥이 같은 수신 상태를 만지면 파싱이 깨진다 */
	if (sk_can_busy_loop(sk) && skb_queue_empty_lockless(&sk->sk_receive_queue))	/* [한국어] 받을 것이 아직 없다면 */
		sk_busy_loop(sk, true);	/* [한국어] NIC 를 직접 돌려 패킷을 끌어온다 — 인터럽트 지연조차 기다리지 않는다 */
	ret = nvme_tcp_try_recv(queue);	/* [한국어] io_work 와 같은 수신 경로를 이 스레드가 직접 돈다 */
	clear_bit(NVME_TCP_Q_POLLING, &queue->flags);	/* [한국어] 폴링 종료 — 이제 콜백이 다시 io_work 를 깨울 수 있다 */
	return ret < 0 ? ret : queue->nr_cqe;	/* [한국어] blk-mq 는 완료 개수를 요구한다. try_recv 가 세어 둔 값이다 */
}

static int nvme_tcp_get_address(struct nvme_ctrl *ctrl, char *buf, int size)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
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
			len--; /* strip trailing newline */
		len += scnprintf(buf + len, size - len, "%ssrc_addr=%pISc\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				(len) ? "," : "", &src_addr);
	}

	mutex_unlock(&queue->queue_lock);	/* [한국어] 동기화 — 큐/연결/상태 공유 보호 */

	return len;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static const struct blk_mq_ops nvme_tcp_mq_ops = {	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	.queue_rq	= nvme_tcp_queue_rq,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	.commit_rqs	= nvme_tcp_commit_rqs,
	.complete	= nvme_complete_rq,
	.init_request	= nvme_tcp_init_request,
	.exit_request	= nvme_tcp_exit_request,
	.init_hctx	= nvme_tcp_init_hctx,
	.timeout	= nvme_tcp_timeout,
	.map_queues	= nvme_tcp_map_queues,
	.poll		= nvme_tcp_poll,
};

static const struct blk_mq_ops nvme_tcp_admin_mq_ops = {	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	.queue_rq	= nvme_tcp_queue_rq,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	.complete	= nvme_complete_rq,
	.init_request	= nvme_tcp_init_request,
	.exit_request	= nvme_tcp_exit_request,
	.init_hctx	= nvme_tcp_init_admin_hctx,
	.timeout	= nvme_tcp_timeout,
};

static const struct nvme_ctrl_ops nvme_tcp_ctrl_ops = {	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	.name			= "tcp",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	.module			= THIS_MODULE,
	.flags			= NVME_F_FABRICS | NVME_F_BLOCKING,
	.reg_read32		= nvmf_reg_read32,
	.reg_read64		= nvmf_reg_read64,
	.reg_write32		= nvmf_reg_write32,
	.subsystem_reset	= nvmf_subsystem_reset,
	.free_ctrl		= nvme_tcp_free_ctrl,
	.submit_async_event	= nvme_tcp_submit_async_event,
	.delete_ctrl		= nvme_tcp_delete_ctrl,
	.get_address		= nvme_tcp_get_address,
	.stop_ctrl		= nvme_tcp_stop_ctrl,
	.get_virt_boundary	= nvmf_get_virt_boundary,
};

static bool
nvme_tcp_existing_controller(struct nvmf_ctrl_options *opts)
{
	struct nvme_tcp_ctrl *ctrl;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	bool found = false;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	mutex_lock(&nvme_tcp_ctrl_mutex);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	list_for_each_entry(ctrl, &nvme_tcp_ctrl_list, list) {	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		found = nvmf_ip_options_match(&ctrl->ctrl, opts);	/* [한국어] fabrics 공통(Connect/옵션/재연결) API */
		if (found)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			break;
	}
	mutex_unlock(&nvme_tcp_ctrl_mutex);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	return found;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static struct nvme_tcp_ctrl *nvme_tcp_alloc_ctrl(struct device *dev,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		struct nvmf_ctrl_options *opts)
{
	struct nvme_tcp_ctrl *ctrl;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	int ret;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	ctrl = kzalloc_obj(*ctrl);	/* [한국어] 커널 메모리 생명주기 */
	if (!ctrl)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		return ERR_PTR(-ENOMEM);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	INIT_LIST_HEAD(&ctrl->list);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ctrl->ctrl.opts = opts;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ctrl->ctrl.queue_count = opts->nr_io_queues + opts->nr_write_queues +	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
				opts->nr_poll_queues + 1;
	ctrl->ctrl.sqsize = opts->queue_size - 1;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	ctrl->ctrl.kato = opts->kato;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	INIT_DELAYED_WORK(&ctrl->connect_work,	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			nvme_tcp_reconnect_ctrl_work);
	INIT_WORK(&ctrl->err_work, nvme_tcp_error_recovery_work);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	INIT_WORK(&ctrl->ctrl.reset_work, nvme_reset_ctrl_work);	/* [한국어] NVMe core API — SQE 조립·완료·상태기계·수명 */

	if (!(opts->mask & NVMF_OPT_TRSVCID)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		opts->trsvcid =	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			kstrdup(__stringify(NVME_TCP_DISC_PORT), GFP_KERNEL);
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

	if (opts->mask & NVMF_OPT_HOST_IFACE) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		if (!__dev_get_by_name(&init_net, opts->host_iface)) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
			pr_err("invalid interface passed: %s\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			       opts->host_iface);
			ret = -ENODEV;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
			goto out_free_ctrl;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
		}
	}

	if (!opts->duplicate_connect && nvme_tcp_existing_controller(opts)) {	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		ret = -EALREADY;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_free_ctrl;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}

	ctrl->queues = kzalloc_objs(*ctrl->queues, ctrl->ctrl.queue_count);	/* [한국어] 커널 메모리 생명주기 */
	if (!ctrl->queues) {	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = -ENOMEM;	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		goto out_free_ctrl;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */
	}

	ret = nvme_init_ctrl(&ctrl->ctrl, dev, &nvme_tcp_ctrl_ops, 0);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_kfree_queues;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	return ctrl;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
out_kfree_queues:
	kfree(ctrl->queues);	/* [한국어] 커널 메모리 생명주기 */
out_free_ctrl:
	kfree(ctrl);	/* [한국어] 커널 메모리 생명주기 */
	return ERR_PTR(ret);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static struct nvme_ctrl *nvme_tcp_create_ctrl(struct device *dev,	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		struct nvmf_ctrl_options *opts)
{
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
	}

	ret = nvme_tcp_setup_ctrl(&ctrl->ctrl, true);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	if (ret)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		goto out_uninit_ctrl;	/* [한국어] 공통 정리 라벨 — 부분 할당 롤백 */

	dev_info(ctrl->ctrl.device, "new ctrl: NQN \"%s\", addr %pISp, hostnqn: %s\n",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		nvmf_ctrl_subsysnqn(&ctrl->ctrl), &ctrl->addr, opts->host->nqn);

	mutex_lock(&nvme_tcp_ctrl_mutex);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	list_add_tail(&ctrl->list, &nvme_tcp_ctrl_list);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	mutex_unlock(&nvme_tcp_ctrl_mutex);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	return &ctrl->ctrl;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

out_uninit_ctrl:
	nvme_uninit_ctrl(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
out_put_ctrl:
	nvme_put_ctrl(&ctrl->ctrl);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	if (ret > 0)	/* [한국어] 제어 분기 — 상태·에러·자원 조건 경로 */
		ret = -EIO;
	return ERR_PTR(ret);	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static struct nvmf_transport_ops nvme_tcp_transport = {	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	.name		= "tcp",	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
	.module		= THIS_MODULE,
	.required_opts	= NVMF_OPT_TRADDR,
	.allowed_opts	= NVMF_OPT_TRSVCID | NVMF_OPT_RECONNECT_DELAY |
			  NVMF_OPT_HOST_TRADDR | NVMF_OPT_CTRL_LOSS_TMO |
			  NVMF_OPT_HDR_DIGEST | NVMF_OPT_DATA_DIGEST |
			  NVMF_OPT_NR_WRITE_QUEUES | NVMF_OPT_NR_POLL_QUEUES |
			  NVMF_OPT_TOS | NVMF_OPT_HOST_IFACE | NVMF_OPT_TLS |
			  NVMF_OPT_KEYRING | NVMF_OPT_TLS_KEY | NVMF_OPT_CONCAT,
	.create_ctrl	= nvme_tcp_create_ctrl,
};

static int __init nvme_tcp_init_module(void)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
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
		wq_flags |= WQ_UNBOUND;

	nvme_tcp_wq = alloc_workqueue("nvme_tcp_wq", wq_flags, 0);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	if (!nvme_tcp_wq)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		return -ENOMEM;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */

	for_each_possible_cpu(cpu)	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */
		atomic_set(&nvme_tcp_cpu_queues[cpu], 0);

	nvmf_register_transport(&nvme_tcp_transport);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	return 0;	/* [한국어] 상위 계층으로 성공/에러/상태 반환 */
}

static void __exit nvme_tcp_cleanup_module(void)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
{
	struct nvme_tcp_ctrl *ctrl;	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	nvmf_unregister_transport(&nvme_tcp_transport);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

	mutex_lock(&nvme_tcp_ctrl_mutex);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	list_for_each_entry(ctrl, &nvme_tcp_ctrl_list, list)	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
		nvme_delete_ctrl(&ctrl->ctrl);
	mutex_unlock(&nvme_tcp_ctrl_mutex);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
	flush_workqueue(nvme_delete_wq);	/* [한국어] 트랜스포트 파이프라인 단계 — 제출/완료/연결/복구 중 한 축 */

	destroy_workqueue(nvme_tcp_wq);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
}

module_init(nvme_tcp_init_module);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */
module_exit(nvme_tcp_cleanup_module);	/* [한국어] NVMe/TCP 큐·PDU·소켓 경로 헬퍼 */

MODULE_DESCRIPTION("NVMe host TCP transport driver");	/* [한국어] 모듈 경계·파라미터·심볼 공개 */
MODULE_LICENSE("GPL v2");	/* [한국어] 모듈 경계·파라미터·심볼 공개 */
