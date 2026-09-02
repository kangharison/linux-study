/* SPDX-License-Identifier: GPL-2.0 */
/*
 * NVMe over Fabrics common host code.
 * Copyright (c) 2015-2016 HGST, a Western Digital Company.
 */
#ifndef _NVME_FABRICS_H
#define _NVME_FABRICS_H 1

/*
 * [한국어 설명] NVMe-oF 공통 호스트 API·옵션·트랜스포트 등록 (fabrics.h)
 *
 * === 파일의 역할 ===
 * PCIe 가 아닌 네트워크/패브릭 트랜스포트(TCP, RDMA, FC)가 공유하는
 * **공통 헤더**다. 사용자 공간(nvme-cli)이 /dev/nvme-fabrics 또는 configfs
 * 로 넘기는 연결 문자열을 해석한 결과(struct nvmf_ctrl_options), Host NQN/
 * Host ID 쌍(struct nvmf_host), 트랜스포트 플러그인 테이블
 * (struct nvmf_transport_ops) 및 fabrics.c 가 구현하는 레지스터 대행·
 * Connect·reconnect 헬퍼 선언을 모은다.
 *
 * PCIe 경로(pci.c)는 MMIO 로 CAP/CC/CSTS 에 직접 접근하지만, fabrics 는
 * Property Get/Set 캡슐과 Connect 명령으로 동일 의미를 원격 구현한다.
 * 이 헤더의 nvmf_reg_* / nvmf_connect_* 가 그 추상화 경계다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *   nvme-cli / systemd
 *        │  "transport=tcp,traddr=…,nqn=…"
 *        ▼
 *   fabrics.c  (옵션 파싱, transport ops 조회, create_ctrl)
 *        │
 *   tcp.c / rdma.c / fc.c  (nvmf_transport_ops.create_ctrl)
 *        │
 *   core.c  (nvme_ctrl 생명주기, admin/IO, multipath, keep-alive)
 *
 * reconnect 정책(reconnect_delay, ctrl_loss_tmo, fail_fast_tmo)과
 * 큐 개수 협상(nr_io/write/poll queues)도 옵션 구조체에 실려 트랜스포트
 * 와 core 가 공유한다.
 *
 * === 타 모듈과의 연결 ===
 * - drivers/nvme/host/fabrics.c : 이 헤더 API 의 본체 구현
 * - drivers/nvme/host/tcp.c, rdma.c, fc.c : nvmf_register_transport 소비자
 * - drivers/nvme/host/core.c : nvme_ctrl, keep-alive, reset, multipath
 * - drivers/nvme/host/auth.c : DH-HMAC-CHAP (dhchap_* 옵션)
 * - drivers/nvme/host/trace.c : Connect/Property 트레이스 디코드
 * - block/blk-mq : nvmf_map_queues / HCTX_MAX_TYPES 큐 매핑
 *
 * === 주요 심볼 ===
 * NVMF_* 기본 상수 — 큐 크기·reconnect·loss timeout 기본값
 * nvmf_host / nvmf_ctrl_options / nvmf_transport_ops
 * NVMF_OPT_* 비트 — sysfs/connect 문자열 파싱 마스크
 * nvmf_reg_*, nvmf_connect_*, nvmf_should_reconnect, nvmf_map_queues …
 *
 * === 주요 함수/구조체 요약 ===
 * - struct nvmf_ctrl_options: 연결 옵션의 정규 표현. 트랜스포트 이름, traddr/trsvcid,
 *   subsysnqn/hostnqn, 큐 개수, keep-alive 주기, 재연결 정책, TLS/인증 키를 담는다.
 *   fabrics.c 가 문자열에서 채우고 각 트랜스포트가 읽어 간다.
 * - struct nvmf_transport_ops: 트랜스포트가 fabrics 코어에 등록하는 vtable.
 *   name/module/required_opts/allowed_opts 와 create_ctrl 콜백으로 이뤄진다.
 * - nvmf_ctrl_subsysnqn: 컨트롤러의 서브시스템 NQN 을 꺼내는 인라인. opts 가 아직
 *   없는 시점에도 안전하도록 ctrl 쪽 값을 우선한다.
 * - nvmf_complete_timed_out_request: 타임아웃된 요청을 취소 상태로 완료시킨다.
 *   트랜스포트들이 자신의 timeout 콜백에서 공통으로 부른다.
 * - nvmf_nr_io_queues: 옵션에 적힌 I/O/write/poll 큐 개수를 합산한다.
 * - NVMF_OPT_* 비트: 각 옵션의 존재 여부를 나타내는 마스크. required_opts 와
 *   allowed_opts 가 이 비트로 트랜스포트별 허용 조합을 표현한다.
 */

#include <linux/in.h>		/* [한국어] IP 주소 계열 타입 — traddr 파싱/매칭 경로와 간접 연결 */
#include <linux/inet.h>		/* [한국어] in4_pton/in6_pton 등 — fabrics.c IP 옵션 비교에 사용 */

#define NVMF_MIN_QUEUE_SIZE	16	/* [한국어] SQ/CQ 깊이 하한 — 이보다 작으면 옵션 거절 */
#define NVMF_MAX_QUEUE_SIZE	1024	/* [한국어] 깊이 상한 — 메모리·타겟 한계 보호 */
#define NVMF_DEF_QUEUE_SIZE	128	/* [한국어] 사용자 미지정 시 기본 큐 깊이 */
#define NVMF_DEF_RECONNECT_DELAY	10	/* [한국어] 재연결 시도 간격 기본(초) */
/* default to 600 seconds of reconnect attempts before giving up */
#define NVMF_DEF_CTRL_LOSS_TMO		600	/* [한국어] 컨트롤러 상실 판정까지 재시도 기간(초); max_reconnects 계산에 사용 */
/* default is -1: the fail fast mechanism is disabled  */
#define NVMF_DEF_FAIL_FAST_TMO		-1	/* [한국어] Fast I/O Fail 비활성 기본값; multipath 에서 경로 빨리 내리기 */

/*
 * Define a host as seen by the target.  We allocate one at boot, but also
 * allow the override it when creating controllers.  This is both to provide
 * persistence of the Host NQN over multiple boots, and to allow using
 * multiple ones, for example in a container scenario.  Because we must not
 * use different Host NQNs with the same Host ID we generate a Host ID and
 * use this structure to keep track of the relation between the two.
 */
/*
 * [한국어] 타겟이 보는 "호스트 정체성" — Host NQN + Host ID(UUID) 쌍.
 * 부팅 시 기본 인스턴스를 하나 만들고, 연결 시 hostnqn=/hostid= 로 재정의
 * 가능. 컨테이너·멀티테넌시에서 호스트 단위 격리를 위해 여러 nvmf_host 가
 * 리스트에 공존할 수 있다. 스펙상 동일 Host ID 에 서로 다른 NQN 을 쓰면
 * 안 되므로 이 구조체가 둘의 결합을 강제한다.
 */
struct nvmf_host {
	struct kref		ref;	/* [한국어] 옵션/컨트롤러가 공유 참조; 0 이면 리스트에서 제거·해제 */
	struct list_head	list;	/* [한국어] fabrics.c 전역 host 리스트 노드 */
	char			nqn[NVMF_NQN_SIZE];	/* [한국어] Host NQN 문자열 (nqn.2014-08.org… 형식) */
	uuid_t			id;	/* [한국어] Host Identifier — Connect 데이터에 실림 */
};

/**
 * enum nvmf_parsing_opts - used to define the sysfs parsing options used.
 */
/*
 * [한국어] connect 옵션 비트마스크. fabrics.c match_table 파서가 토큰을
 * 인식할 때마다 mask 에 OR. required_opts/allowed_opts 와 AND 비교해
 * 트랜스포트별 필수·허용 키를 강제한다 (예: tcp 는 traddr 필수, fc 는
 * 다른 주소 형식).
 */
enum {
	NVMF_OPT_ERR		= 0,	/* [한국어] 파서 오류 센티널 */
	NVMF_OPT_TRANSPORT	= 1 << 0,	/* [한국어] transport=tcp|rdma|fc|loop… */
	NVMF_OPT_NQN		= 1 << 1,	/* [한국어] 서브시스템 NQN (타겟) */
	NVMF_OPT_TRADDR		= 1 << 2,	/* [한국어] 트랜스포트 주소 (IP, WWPN 등) */
	NVMF_OPT_TRSVCID	= 1 << 3,	/* [한국어] 서비스 ID (TCP/RDMA 포트 등) */
	NVMF_OPT_QUEUE_SIZE	= 1 << 4,	/* [한국어] I/O 큐 깊이 */
	NVMF_OPT_NR_IO_QUEUES	= 1 << 5,	/* [한국어] 기본 I/O 큐 개수 요청 */
	NVMF_OPT_TL_RETRY_COUNT	= 1 << 6,	/* [한국어] 트랜스포트 계층 재시도 (레거시 비트) */
	NVMF_OPT_KATO		= 1 << 7,	/* [한국어] Keep-Alive Timeout */
	NVMF_OPT_HOSTNQN	= 1 << 8,	/* [한국어] 호스트 NQN 오버라이드 */
	NVMF_OPT_RECONNECT_DELAY = 1 << 9,	/* [한국어] 재연결 백오프 초 */
	NVMF_OPT_HOST_TRADDR	= 1 << 10,	/* [한국어] 로컬 호스트 측 주소 바인딩 */
	NVMF_OPT_CTRL_LOSS_TMO	= 1 << 11,	/* [한국어] 컨트롤러 loss 타임아웃 */
	NVMF_OPT_HOST_ID	= 1 << 12,	/* [한국어] Host UUID 오버라이드 */
	NVMF_OPT_DUP_CONNECT	= 1 << 13,	/* [한국어] 동일 서브시스템 중복 연결 허용 */
	NVMF_OPT_DISABLE_SQFLOW = 1 << 14,	/* [한국어] SQ flow control 비활성 협상 */
	NVMF_OPT_HDR_DIGEST	= 1 << 15,	/* [한국어] TCP header digest (CRC) */
	NVMF_OPT_DATA_DIGEST	= 1 << 16,	/* [한국어] TCP data digest */
	NVMF_OPT_NR_WRITE_QUEUES = 1 << 17,	/* [한국어] 쓰기 전용 큐 수 (블크-mq 타입 분리) */
	NVMF_OPT_NR_POLL_QUEUES = 1 << 18,	/* [한국어] 폴링 hctx 용 큐 수 */
	NVMF_OPT_TOS		= 1 << 19,	/* [한국어] IP Type of Service / DSCP */
	NVMF_OPT_FAIL_FAST_TMO	= 1 << 20,	/* [한국어] multipath fast_io_fail 초 */
	NVMF_OPT_HOST_IFACE	= 1 << 21,	/* [한국어] 로컬 네트워크 인터페이스 이름 */
	NVMF_OPT_DISCOVERY	= 1 << 22,	/* [한국어] 디스커버리 컨트롤러 연결 표시 */
	NVMF_OPT_DHCHAP_SECRET	= 1 << 23,	/* [한국어] 호스트 DH-HMAC-CHAP 시크릿 */
	NVMF_OPT_DHCHAP_CTRL_SECRET = 1 << 24,	/* [한국어] 양방향 인증용 컨트롤러 시크릿 */
	NVMF_OPT_TLS		= 1 << 25,	/* [한국어] NVMe/TCP TLS 암호화 */
	NVMF_OPT_KEYRING	= 1 << 26,	/* [한국어] 키 조회용 keyring */
	NVMF_OPT_TLS_KEY	= 1 << 27,	/* [한국어] TLS PSK 등 키 지정 */
	NVMF_OPT_CONCAT		= 1 << 28,	/* [한국어] Secure Channel Concatenation (TCP) */
};

/**
 * struct nvmf_ctrl_options - Used to hold the options specified
 *			      with the parsing opts enum.
 * @mask:	Used by the fabrics library to parse through sysfs options
 *		on adding a NVMe controller.
 * @max_reconnects: maximum number of allowed reconnect attempts before removing
 *		the controller, (-1) means reconnect forever, zero means remove
 *		immediately;
 * @transport:	Holds the fabric transport "technology name" (for a lack of
 *		better description) that will be used by an NVMe controller
 *		being added.
 * @subsysnqn:	Hold the fully qualified NQN subsystem name (format defined
 *		in the NVMe specification, "NVMe Qualified Names").
 * @traddr:	The transport-specific TRADDR field for a port on the
 *              subsystem which is adding a controller.
 * @trsvcid:	The transport-specific TRSVCID field for a port on the
 *              subsystem which is adding a controller.
 * @host_traddr: A transport-specific field identifying the NVME host port
 *     to use for the connection to the controller.
 * @host_iface: A transport-specific field identifying the NVME host
 *     interface to use for the connection to the controller.
 * @queue_size: Number of IO queue elements.
 * @nr_io_queues: Number of controller IO queues that will be established.
 * @reconnect_delay: Time between two consecutive reconnect attempts.
 * @discovery_nqn: indicates if the subsysnqn is the well-known discovery NQN.
 * @kato:	Keep-alive timeout.
 * @host:	Virtual NVMe host, contains the NQN and Host ID.
 * @dhchap_secret: DH-HMAC-CHAP secret
 * @dhchap_ctrl_secret: DH-HMAC-CHAP controller secret for bi-directional
 *              authentication
 * @keyring:    Keyring to use for key lookups
 * @tls_key:    TLS key for encrypted connections (TCP)
 * @tls:        Start TLS encrypted connections (TCP)
 * @concat:     Enabled Secure channel concatenation (TCP)
 * @disable_sqflow: disable controller sq flow control
 * @hdr_digest: generate/verify header digest (TCP)
 * @data_digest: generate/verify data digest (TCP)
 * @nr_write_queues: number of queues for write I/O
 * @nr_poll_queues: number of queues for polling I/O
 * @tos: type of service
 * @fast_io_fail_tmo: Fast I/O fail timeout in seconds
 */
/*
 * [한국어] 한 번의 컨트롤러 connect 요청에 대한 해석 결과 전체.
 * fabrics.c 가 문자열 파싱 후 할당·채우고, create_ctrl() 에 넘겨 각
 * 트랜스포트가 소켓/QP/FC 세션을 연다. 성공 시 nvme_ctrl->opts 로
 * 수명이 이전되어 sysfs 에 주소·NQN 등이 노출된다.
 * mask 비트와 실제 필드 유효성이 짝을 이룸 — 비트 없이 필드만 보면 안 됨.
 */
struct nvmf_ctrl_options {
	unsigned		mask;	/* [한국어] 어떤 NVMF_OPT_* 가 명시되었는지 */
	int			max_reconnects;	/* [한국어] -1=무한, 0=즉시 포기, >0=횟수 제한 */
	char			*transport;	/* [한국어] "tcp"/"rdma"/"fc" … ops->name 매칭 키 */
	char			*subsysnqn;	/* [한국어] 대상 서브시스템 NQN */
	char			*traddr;	/* [한국어] 원격 주소 문자열 */
	char			*trsvcid;	/* [한국어] 포트/서비스 ID 문자열 */
	char			*host_traddr;	/* [한국어] 로컬 소스 주소 바인딩 */
	char			*host_iface;	/* [한국어] 로컬 ifname (예: eth0) */
	size_t			queue_size;	/* [한국어] 큐 깊이 (MIN~MAX 클램프 대상) */
	unsigned int		nr_io_queues;	/* [한국어] default 타입 I/O 큐 요청 수 */
	unsigned int		reconnect_delay;	/* [한국어] 재연결 대기 초 */
	bool			discovery_nqn;	/* [한국어] well-known discovery NQN 여부 */
	bool			duplicate_connect;	/* [한국어] 동일 튜플 중복 ctrl 허용 */
	unsigned int		kato;	/* [한국어] Keep Alive Timeout — Connect·keep-alive 워크와 공유 */
	struct nvmf_host	*host;	/* [한국어] 이 연결에 사용할 Host NQN/ID (ref 보유) */
	char			*dhchap_secret;	/* [한국어] 호스트 인증 시크릿 (auth.c) */
	char			*dhchap_ctrl_secret;	/* [한국어] 컨트롤러 측 시크릿 (양방향) */
	struct key		*keyring;	/* [한국어] 커널 keyring 참조 */
	struct key		*tls_key;	/* [한국어] TLS 키 객체 */
	bool			tls;	/* [한국어] TLS 사용 요청 */
	bool			concat;	/* [한국어] secure concatenation */
	bool			disable_sqflow;	/* [한국어] SQ flow control off 협상 */
	bool			hdr_digest;	/* [한국어] TCP 헤더 CRC */
	bool			data_digest;	/* [한국어] TCP 데이터 CRC */
	unsigned int		nr_write_queues;	/* [한국어] write hctx 큐 수 */
	unsigned int		nr_poll_queues;	/* [한국어] poll hctx 큐 수 */
	int			tos;	/* [한국어] 소켓 TOS; 미지정 시 음수 관례 가능 */
	int			fast_io_fail_tmo;	/* [한국어] multipath fast_io_fail (초), -1=off */
};

/*
 * struct nvmf_transport_ops - used to register a specific
 *			       fabric implementation of NVMe fabrics.
 * @entry:		Used by the fabrics library to add the new
 *			registration entry to its linked-list internal tree.
 * @module:             Transport module reference
 * @name:		Name of the NVMe fabric driver implementation.
 * @required_opts:	sysfs command-line options that must be specified
 *			when adding a new NVMe controller.
 * @allowed_opts:	sysfs command-line options that can be specified
 *			when adding a new NVMe controller.
 * @create_ctrl():	function pointer that points to a non-NVMe
 *			implementation-specific fabric technology
 *			that would go into starting up that fabric
 *			for the purpose of connection to an NVMe controller
 *			using that fabric technology.
 *
 * Notes:
 *	1. At minimum, 'required_opts' and 'allowed_opts' should
 *	   be set to the same enum parsing options defined earlier.
 *	2. create_ctrl() must be defined (even if it does nothing)
 *	3. struct nvmf_transport_ops must be statically allocated in the
 *	   modules .bss section so that a pure module_get on @module
 *	   prevents the memory from being freed.
 */
/*
 * [한국어] 트랜스포트 플러그인 디스크립터. tcp/rdma/fc 모듈 init 에서
 * nvmf_register_transport() 로 전역 리스트에 등록하고, connect 시
 * opts->transport 문자열로 name 을 찾아 create_ctrl() 을 호출한다.
 * module 필드로 모듈 참조를 올려 언로드 레이스를 막고, 정적 할당
 * 요구(원본 Notes 3)는 ops 구조체 수명을 모듈 .data/.bss 에 고정한다.
 * required_opts 에 없는 필수 키가 빠지면 fabrics 가 연결을 거절하고,
 * allowed 밖 키는 무시/에러 처리된다.
 */
struct nvmf_transport_ops {
	struct list_head	entry;	/* [한국어] fabrics 전역 transport 리스트 링크 */
	struct module		*module;	/* [한국어] THIS_MODULE — create 동안 get/put */
	const char		*name;	/* [한국어] "tcp", "rdma", "fc" 등 */
	int			required_opts;	/* [한국어] 필수 NVMF_OPT_* 비트 OR */
	int			allowed_opts;	/* [한국어] 허용 NVMF_OPT_* 비트 OR */
	struct nvme_ctrl	*(*create_ctrl)(struct device *dev,
					struct nvmf_ctrl_options *opts);
	/* [한국어] 실제 컨트롤러 할당·핸드셰이크 시작. 성공 시 살아있는
	 * nvme_ctrl *, 실패 시 ERR_PTR(-errno). dev 는 클래스 디바이스. */
};

/*
 * [한국어]
 * nvmf_ctlr_matches_baseopts - 기존 컨트롤러가 요청 옵션의 "동일 대상"인지
 *
 * @ctrl: 이미 존재하는 fabrics 컨트롤러
 * @opts: 새 connect 요청 옵션
 * @return: true 이면 같은 서브시스템·호스트 정체성으로 간주
 *
 * 중복 연결 탐지/재사용 판단에 사용. DELETING/DEAD 상태는 매칭 제외해
 * 정리 중인 객체에 새 I/O 가 붙지 않게 한다. 비교 키: subsysnqn +
 * host NQN + host UUID. traddr 등 경로 식별은 nvmf_ip_options_match 등
 * 별도 헬퍼가 담당.
 */
static inline bool
nvmf_ctlr_matches_baseopts(struct nvme_ctrl *ctrl,
			struct nvmf_ctrl_options *opts)
{
	enum nvme_ctrl_state state = nvme_ctrl_state(ctrl);	/* [한국어] 원자적 상태 스냅샷 */

	if (state == NVME_CTRL_DELETING ||
	    state == NVME_CTRL_DELETING_NOIO ||
	    state == NVME_CTRL_DEAD ||	/* [한국어] 수명 말기 — 매칭 후보에서 제외 */
	    strcmp(opts->subsysnqn, ctrl->opts->subsysnqn) ||	/* [한국어] 서브시스템 NQN */
	    strcmp(opts->host->nqn, ctrl->opts->host->nqn) ||	/* [한국어] Host NQN */
	    !uuid_equal(&opts->host->id, &ctrl->opts->host->id))	/* [한국어] Host ID */
		return false;

	return true;	/* [한국어] 동일 논리적 호스트-서브시스템 쌍 */
}

/*
 * [한국어]
 * nvmf_ctrl_subsysnqn - sysfs/로그에 쓸 서브시스템 NQN 문자열 선택
 *
 * 디스커버리 컨트롤러이거나 subsys 가 아직 없으면 connect 옵션의
 * subsysnqn 을 쓰고, 일반 서브시스템은 Identify 로 채운 subnqn 을 우선.
 * discovery 는 well-known NQN 으로 붙지만 실제 타겟 이름이 다를 수 있어
 * 옵션 쪽을 유지하는 분기가 필요.
 */
static inline char *nvmf_ctrl_subsysnqn(struct nvme_ctrl *ctrl)
{
	if (!ctrl->subsys ||
	    !strcmp(ctrl->opts->subsysnqn, NVME_DISC_SUBSYS_NAME))
		return ctrl->opts->subsysnqn;	/* [한국어] 디스커버리 또는 미바인딩 */
	return ctrl->subsys->subnqn;	/* [한국어] Identify 기반 정규 NQN */
}

/*
 * [한국어]
 * nvmf_complete_timed_out_request - fabrics 타임아웃 요청을 호스트 중단으로 완료
 *
 * @rq: blk-mq request
 *
 * 시작됐지만 아직 complete 되지 않은 요청에 NVME_SC_HOST_ABORTED_CMD 를
 * 심고 blk_mq_complete_request 로 종료. 연결 손실·reset 경로에서 inflight
 * 를 비울 때 사용. 미시작/이미 완료된 요청은 건드리지 않아 double complete
 * 를 피한다.
 */
static inline void nvmf_complete_timed_out_request(struct request *rq)
{
	if (blk_mq_request_started(rq) && !blk_mq_request_completed(rq)) {
		nvme_req(rq)->status = NVME_SC_HOST_ABORTED_CMD;	/* [한국어] 호스트 측 중단 status */
		blk_mq_complete_request(rq);	/* [한국어] 완료 소프트IRQ/원격 CPU 경로로 전달 */
	}
}

/*
 * [한국어]
 * nvmf_nr_io_queues - 옵션이 요청하는 총 I/O 큐 수 상한 계산
 *
 * default + write + poll 타입 각각 online CPU 수로 캡한 뒤 합산.
 * 실제 생성 수는 컨트롤러 협상(Set Features Number of Queues)과
 * 트랜스포트 한계로 더 줄어들 수 있음. nvmf_set_io_queues 와 함께 사용.
 */
static inline unsigned int nvmf_nr_io_queues(struct nvmf_ctrl_options *opts)
{
	return min(opts->nr_io_queues, num_online_cpus()) +
		min(opts->nr_write_queues, num_online_cpus()) +
		min(opts->nr_poll_queues, num_online_cpus());
}

/*
 * [한국어]
 * nvmf_get_virt_boundary - fabrics 요청 가상 경계 마스크
 *
 * PCIe NVMe 는 종종 페이지 경계를 DMA 제약으로 쓰지만, fabrics 는
 * 캡슐/SGL 로 페이로드를 나르므로 호스트 블록층 virt boundary 를 0
 * (제한 없음)으로 둔다. is_admin 인자는 대칭 API 용이며 현재 미사용.
 */
static inline unsigned long nvmf_get_virt_boundary(struct nvme_ctrl *ctrl,
						   bool is_admin)
{
	return 0;	/* [한국어] fabrics: 블록층 virt boundary 비적용 */
}

/* [한국어] 원격 컨트롤러 property 32/64비트 읽기 — PCIe MMIO 대용 (Property Get) */
int nvmf_reg_read32(struct nvme_ctrl *ctrl, u32 off, u32 *val);
int nvmf_reg_read64(struct nvme_ctrl *ctrl, u32 off, u64 *val);
/* [한국어] property 32비트 쓰기 (Property Set) — CC.EN 등 리셋 시퀀스 */
int nvmf_reg_write32(struct nvme_ctrl *ctrl, u32 off, u32 val);
/* [한국어] NSSR 등 서브시스템 리셋 fabrics 경로 */
int nvmf_subsystem_reset(struct nvme_ctrl *ctrl);
/* [한국어] Admin 큐 Connect 캡슐 제출·완료 대기 — 세션 수립 1단계 */
int nvmf_connect_admin_queue(struct nvme_ctrl *ctrl);
/* [한국어] 지정 qid I/O 큐 Connect — 큐별 세션/크레딧 성립 */
int nvmf_connect_io_queue(struct nvme_ctrl *ctrl, u16 qid);
/* [한국어] 트랜스포트 모듈 등록/해제 — 리스트 + 모듈 참조 */
int nvmf_register_transport(struct nvmf_transport_ops *ops);
void nvmf_unregister_transport(struct nvmf_transport_ops *ops);
/* [한국어] 파싱된 옵션 문자열·host ref·key 해제 */
void nvmf_free_options(struct nvmf_ctrl_options *opts);
/* [한국어] sysfs address 속성에 쓸 "traddr=…,trsvcid=…" 포맷 */
int nvmf_get_address(struct nvme_ctrl *ctrl, char *buf, int size);
/* [한국어] status/정책에 따라 재연결 계속 여부 — loss tmo·max_reconnects */
bool nvmf_should_reconnect(struct nvme_ctrl *ctrl, int status);
/* [한국어] IP 계열 옵션(traddr/trsvcid/host_traddr/iface/tos) 동일성 */
bool nvmf_ip_options_match(struct nvme_ctrl *ctrl,
		struct nvmf_ctrl_options *opts);
/* [한국어] 협상된 총 큐 수를 default/write/poll 배열에 분배 */
void nvmf_set_io_queues(struct nvmf_ctrl_options *opts, u32 nr_io_queues,
			u32 io_queues[HCTX_MAX_TYPES]);
/* [한국어] blk-mq tag_set 에 큐→CPU 매핑 (map_queues 콜백 헬퍼) */
void nvmf_map_queues(struct blk_mq_tag_set *set, struct nvme_ctrl *ctrl,
		     u32 io_queues[HCTX_MAX_TYPES]);

#endif /* _NVME_FABRICS_H */
