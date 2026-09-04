/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Definitions for the NVM Express interface
 * Copyright (c) 2011-2014, Intel Corporation.
 */

/*
 * [한국어 설명] NVM Express 온와이어/온컨트롤러 레이아웃 스펙 헤더 (include/linux/nvme.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 NVMe Base Spec 과 NVMe over Fabrics 의 **레지스터·명령·데이터 구조
 * 바이너리 레이아웃**을 커널 C 타입으로 고정한다. host 드라이버(core/pci/tcp/
 * rdma/fc)와 target(nvmet) 이 동일 정의를 공유하므로, 여기서 필드 오프셋이나
 * 엔디안 표기를 바꾸면 전 트랜스포트 온와이어 호환이 깨진다. 런타임 상태
 * 머신(struct nvme_ctrl 등)은 drivers/nvme/host/nvme.h 에 두고, 본 파일은
 * **스펙이 정의한 메모리 이미지**만 담당한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *   블록 계층(bio/blk-mq) → nvme host core → 트랜스포트(pci/tcp/rdma/fc)
 *        ↑ 본 헤더가 공급하는 SQE 64B / CQE 16B / Identify / Log / Feature
 *   PCIe BAR0 MMIO: CAP/VS/CC/CSTS/AQA/ASQ/ACQ/DBS — 컨트롤러 enable 시퀀스
 *   Fabrics: 동일 개념을 Property Get/Set 으로 에뮬, Connect/Auth 캡슐 추가
 *
 * === 호스트 소프트웨어가 반드시 아는 계약 ===
 * 1) CAP 읽기 → AQA/ASQ/ACQ 설정 → CC(EN=1,CSS,MPS,IOSQES,IOCQES) → CSTS.RDY 대기
 * 2) SQE 는 항상 64B(2^6), CQE 는 16B(2^4); LE 필드, host 작성/컨트롤러 작성 구분
 * 3) Admin opcode 와 I/O opcode 네임스페이스 분리(qid=0 vs qid>0)
 * 4) 데이터 포인터: PRP(PCIe 기본) 대 SGL(Fabrics/옵션) — PSDT 비트가 선택
 * 5) CQE status: SCT|SC|DNR|MORE — core 가 재시도/경로전환/errno 로 변환
 * 6) NQN/Discovery 상수 — fabrics 연결·디스커버리 로그 파싱 기준
 *
 * === 엔디안 규약 ===
 * 온와이어 멀티바이트 필드는 __le16/__le32/__le64. 호스트는 cpu_to_le 계열/le*_to_cpu.
 * 단일 바이트(__u8)와 char NQN 필드는 엔디안 변환 없음. command_id 는 호스트
 * 엔디안(__u16)으로 SQE/CQE 에 동일 비트 패턴 유지(컨트롤러가 그대로 반사).
 *
 * === 주요 심볼 지도 ===
 * NVME_REG_*, NVME_CAP_*, NVME_CC_*, NVME_CSTS_* — MMIO 레지스터
 * struct nvme_command / nvme_completion — SQE/CQE
 * struct nvme_id_ctrl / nvme_id_ns — Identify CNS
 * enum nvme_opcode / nvme_admin_opcode — 명령 코드
 * NVME_SC_* / NVME_SCT_* — 완료 status
 * NVMF_* / nvmf_* — Fabrics NQN·Connect·Discovery·Auth
 *
 * === 타 모듈과의 연결 ===
 * 이 헤더는 아래를 향해서만 의존한다 -- 커널의 기본 타입 외에는 아무것도 포함하지
 * 않으며, 그래서 호스트와 타겟, 그리고 트랜스포트 어디서든 안전하게 포함된다.
 * - drivers/nvme/host 아래 전부: 명령을 조립하고 응답을 해석하는 모든 코드가 이 정의를 쓴다.
 *   nvme_setup_cmd 가 채우는 struct nvme_command, Identify 결과를 읽는
 *   struct nvme_id_ctrl / nvme_id_ns 가 대표적이다.
 * - drivers/nvme/target 아래 전부: 타겟은 같은 구조체를 반대 방향으로 쓴다. 호스트가 채운
 *   필드를 읽고 응답을 채운다. 한 헤더를 공유하므로 양쪽 해석이 어긋나지 않는다.
 * - drivers/nvme/host/constants.c: 여기 정의된 opcode/status 열거를 인덱스로 삼아
 *   문자열 테이블을 만든다.
 * - drivers/nvme/host/trace.h: 추적점이 같은 열거로 명령을 해석한다.
 * - include/uapi/linux/nvme_ioctl.h: 유저스페이스로 넘어가는 부분은 그쪽에 있고,
 *   이 헤더는 커널 내부 표현이다.
 * 데이터 흐름 관점에서 이 파일은 흐르는 것이 아니라 흐름의 '모양'을 정한다.
 * 호스트가 SQE 를 채워 보내고 CQE 를 받아 읽는 그 바이트 배치가 전부 여기 있다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct nvme_command: 64바이트 SQE 의 유니온. 명령 계열마다 다른 레이아웃
 *   (rw, identify, features, create_sq/cq, dsm, zns, fabrics 등)을 한 크기로 겹쳐
 *   둔 것이라, 어느 멤버를 쓰든 하드웨어가 보는 바이트 수는 같다.
 * - struct nvme_completion: 16바이트 CQE. result, sq_head, sq_id, command_id,
 *   status 로 이뤄지며 status 의 phase 비트가 새 항목 판별의 근거다.
 * - struct nvme_id_ctrl: Identify Controller 응답 4KB. 모델·시리얼, 지원 기능,
 *   최대 전송 크기(mdts), 큐 개수 힌트, HMB/CMB 능력이 여기서 온다.
 * - struct nvme_id_ns: Identify Namespace 응답. 용량(nsze/ncap/nuse), LBA 형식
 *   목록(lbaf), 메타데이터 설정이 담긴다. 블록 계층의 queue_limits 가 이것에서 파생된다.
 * - enum nvme_opcode / nvme_admin_opcode / nvme_fabrics_type: 명령 번호의 정의처.
 * - NVME_SC_*: 상태 코드. 상위 비트가 계열(Generic/Command Specific/Media)을 가른다.
 * - NVME_REG_*: 컨트롤러 레지스터 오프셋(CAP, VS, CC, CSTS, AQA, ASQ, ACQ).
 *   PCIe 는 BAR0 에서 MMIO 로, Fabrics 는 Property Get/Set 으로 같은 값을 다룬다.
 */

#ifndef _LINUX_NVME_H
#define _LINUX_NVME_H

#include <linux/bits.h>	/* [한국어] GENMASK/BIT 등 비트필드 헬퍼 — status 마스크·feature 플래그 조립 */
#include <linux/types.h>	/* [한국어] __le16/__le32/__le64/__u8 등 고정폭·엔디안 표기 타입 */
#include <linux/uuid.h>	/* [한국어] uuid_t — Connect data 의 hostid, NIDT UUID 디스크립터 */

/*
 * [한국어] Fabrics NQN·전송 주소 필드 크기 및 디스커버리 상수
 *
 * NQN(NVMe Qualified Name)은 서브시스템/호스트를 전역 식별하는 DNS-like 문자열.
 * 명령/Connect 데이터 구조의 필드는 256B 고정 슬롯(NVMF_NQN_FIELD_LEN)이지만
 * 유효 NQN 본문 최대는 223B(NVMF_NQN_SIZE). 호스트는 복사 시 필드 길이로 버퍼를
 * 잡고, 비교·로그는 NVMF_NQN_SIZE 이내로 자른다. TRSVCID/TRADDR/TSAS 는
 * Discovery Log entry 와 연결 문자열에 재사용된다.
 */
/* NQN names in commands fields specified one size */
#define NVMF_NQN_FIELD_LEN	256	/* [한국어] Connect/Discovery 구조체 안 NQN 슬롯 고정 폭(바이트); 패딩 포함 온와이어 크기 */

/* However the max length of a qualified name is another size */
#define NVMF_NQN_SIZE		223	/* [한국어] 유효 NQN 최대 길이(스펙); 필드 256 보다 짧아 널 종료 공간 확보 */

#define NVMF_TRSVCID_SIZE	32	/* [한국어] 전송 서비스 ID(포트 문자열 등) 필드 크기 — Discovery entry trsvcid[] */
#define NVMF_TRADDR_SIZE	256	/* [한국어] 전송 주소(IP/FC WWPN 등) 필드 크기 — traddr[] 온와이어 */
#define NVMF_TSAS_SIZE		256	/* [한국어] Transport Specific Address Subtype 공통 슬롯 256B — RDMA/TCP 세부 오버레이 */

#define NVME_DISC_SUBSYS_NAME	"nqn.2014-08.org.nvmexpress.discovery"	/* [한국어] 표준 Discovery 서브시스템 NQN; 호스트가 디스커버리 컨트롤러 탐색 시 고정 사용 */

#define NVME_NSID_ALL		0xffffffff	/* [한국어] 전 네임스페이스 대상 브로드캐스트 NSID — Format/로그 등 컨트롤러 전역 연산 */

/*
 * [한국어] NSSR(NVM Subsystem Reset) 매직
 * 호스트가 NSSR 레지스터에 ASCII 'NVMe'(0x4E564D65) 를 쓰면 서브시스템 급 리셋.
 * CAP.NSSRC 가 지원을 알릴 때만 유효. PCIe 단일 기능 리셋보다 넓은 범위.
 */
/* Special NSSR value, 'NVMe' */
#define NVME_SUBSYS_RESET	0x4E564D65	/* [한국어] NSSR 에 기록하는 리틀엔디안 매직 'NVMe'; 인식 시 서브시스템 리셋 */

/*
 * [한국어] Discovery Log 의 SUBTYPE — 대상 서브시스템 종류
 * 호스트 fabrics 디스커버리 파서가 entry.subtype 으로 분기한다.
 * DISC=다른 디스커버리로 referral, NVME=실제 I/O 서브시스템, CURR=현재 디스커버리.
 */
enum nvme_subsys_type {
	/* Referral to another discovery type target subsystem */
	NVME_NQN_DISC	= 1,	/* [한국어] 다른 Discovery 타깃으로의 referral 엔트리(재귀 탐색) */

	/* NVME type target subsystem */
	NVME_NQN_NVME	= 2,	/* [한국어] 실제 NVM 서브시스템 — Connect 후 I/O 가능 */

	/* Current discovery type target subsystem */
	NVME_NQN_CURR	= 3,	/* [한국어] 현재 붙어 있는 Discovery 컨트롤러 자신 */
};

/*
 * [한국어] Identify Controller 의 CNTRLTYPE — 컨트롤러 역할
 * I/O 컨트롤러는 네임스페이스 I/O, Discovery 는 로그 페이지 전용,
 * Administrative 는 관리 전용. 호스트 core 가 타입에 따라 큐/NS 스캔 정책을 갈라 탄다.
 */
enum nvme_ctrl_type {
	NVME_CTRL_IO	= 1,		/* I/O controller */	/* [한국어] 일반 데이터 경로 컨트롤러; Identify NS·I/O 큐 생성 대상 */
	NVME_CTRL_DISC	= 2,		/* Discovery controller */	/* [한국어] Discovery 전용; I/O 큐 없이 Get Log(Discovery) 중심 */
	NVME_CTRL_ADMIN	= 3,		/* Administrative controller */	/* [한국어] 관리 전용 컨트롤러(가상화/풀); 일반 블록 I/O 대상 아님 */
};

/*
 * [한국어] Discovery Controller Type (dctype) — DDC vs CDC
 * NVMe-oF 디스커버리 계층: Direct Discovery Controller 는 직접 엔트리,
 * Central Discovery Controller 는 중앙 집중 레지스트리 역할.
 */
enum nvme_dctype {
	NVME_DCTYPE_NOT_REPORTED	= 0,	/* [한국어] 구 스펙/미보고 — 호스트는 타입 가정 없이 로그 파싱 */
	NVME_DCTYPE_DDC			= 1, /* Direct Discovery Controller */	/* [한국어] 직접 연결 가능한 디스커버리 */
	NVME_DCTYPE_CDC			= 2, /* Central Discovery Controller */	/* [한국어] 중앙 디스커버리 허브 */
};

/*
 * [한국어] Discovery Log ADRFAM — 전송 주소 패밀리
 * entry.adrfam 이 traddr 해석 방식을 결정: PCIe BDF, IPv4/IPv6 문자열,
 * InfiniBand GID, FC 주소. LOOP(254) 는 호스트 루프백/테스트 전용 예약.
 */
/* Address Family codes for Discovery Log Page entry ADRFAM field */
enum {
	NVMF_ADDR_FAMILY_PCI	= 0,	/* PCIe */	/* [한국어] PCIe 주소 패밀리 — 로컬 컨트롤러 */
	NVMF_ADDR_FAMILY_IP4	= 1,	/* IP4 */	/* [한국어] IPv4 문자열 traddr */
	NVMF_ADDR_FAMILY_IP6	= 2,	/* IP6 */	/* [한국어] IPv6 문자열 traddr */
	NVMF_ADDR_FAMILY_IB	= 3,	/* InfiniBand */	/* [한국어] InfiniBand 주소 */
	NVMF_ADDR_FAMILY_FC	= 4,	/* Fibre Channel */	/* [한국어] Fibre Channel 주소 */
	NVMF_ADDR_FAMILY_LOOP	= 254,	/* Reserved for host usage */	/* [한국어] 호스트 루프백 예약(테스트/nvme-loop) */
	NVMF_ADDR_FAMILY_MAX,	/* [한국어] 배열 경계 센티널 */
};

/*
 * [한국어] Discovery Log TRTYPE — 트랜스포트 종류
 * 호스트가 어떤 드라이버 모듈(nvme-rdma/tcp/fc/pci)로 Connect 할지 고르는 키.
 * LOOP 는 커널 루프백 타깃(nvme-loop)용 호스트 예약 값.
 */
/* Transport Type codes for Discovery Log Page entry TRTYPE field */
enum {
	NVMF_TRTYPE_PCI		= 0,	/* PCI */	/* [한국어] 로컬 PCIe NVMe */
	NVMF_TRTYPE_RDMA	= 1,	/* RDMA */	/* [한국어] NVMe/RDMA 트랜스포트 */
	NVMF_TRTYPE_FC		= 2,	/* Fibre Channel */	/* [한국어] NVMe/FC */
	NVMF_TRTYPE_TCP		= 3,	/* TCP/IP */	/* [한국어] NVMe/TCP */
	NVMF_TRTYPE_LOOP	= 254,	/* Reserved for host usage */	/* [한국어] 커널 루프백 타깃 */
	NVMF_TRTYPE_MAX,	/* [한국어] 경계 센티널 */
};

/*
 * [한국어] Discovery Log TREQ — 보안 채널·SQ flow 요구
 * 하위 2비트: TLS 등 secure channel 필요 여부. bit2 DISABLE_SQFLOW 지원 시
 * 호스트가 SQ 크레딧 플로우 컨트롤을 끌 수 있다(고성능 TCP 경로).
 */
/* Transport Requirements codes for Discovery Log Page entry TREQ field */
enum {
	NVMF_TREQ_NOT_SPECIFIED	= 0,		/* Not specified */	/* [한국어] 보안 채널 요구 미지정 */
	NVMF_TREQ_REQUIRED	= 1,		/* Required */	/* [한국어] 보안 채널(TLS 등) 필수 */
	NVMF_TREQ_NOT_REQUIRED	= 2,		/* Not Required */	/* [한국어] 보안 채널 불필요 */
#define NVME_TREQ_SECURE_CHANNEL_MASK \
	(NVMF_TREQ_REQUIRED | NVMF_TREQ_NOT_REQUIRED)	/* [한국어] TREQ 하위 보안 채널 비트 마스크 */

	NVMF_TREQ_DISABLE_SQFLOW = (1 << 2),	/* Supports SQ flow control disable */	/* [한국어] SQ flow control disable 지원 — 고성능 경로 */
};

/*
 * [한국어] TSAS.RDMA_QPTYPE — RDMA 큐페어 서비스
 * Connected(RC) 가 일반 NVMe-RDMA. Datagram 은 스펙 확장/특수. INVALID 는 파서 가드.
 */
/* RDMA QP Service Type codes for Discovery Log Page entry TSAS
 * RDMA_QPTYPE field
 */
enum {
	NVMF_RDMA_QPTYPE_CONNECTED	= 1, /* Reliable Connected */	/* [한국어] Reliable Connected QP — 일반 NVMe-RDMA */
	NVMF_RDMA_QPTYPE_DATAGRAM	= 2, /* Reliable Datagram */	/* [한국어] Reliable Datagram */
	NVMF_RDMA_QPTYPE_INVALID	= 0xff,	/* [한국어] 잘못된/미지정 QP 타입 가드 */
};

/*
 * [한국어] TSAS.RDMA_PRTYPE — RDMA 프로바이더(IB/RoCE/iWARP)
 * 호스트 RDMA 스택이 디바이스·GID 타입을 고를 때 사용. NOT_SPECIFIED 면 자동 협상.
 */
/* RDMA Provider Type codes for Discovery Log Page entry TSAS
 * RDMA_PRTYPE field
 */
enum {
	NVMF_RDMA_PRTYPE_NOT_SPECIFIED	= 1, /* No Provider Specified */	/* [한국어] 프로바이더 미지정 — 호스트 자동 */
	NVMF_RDMA_PRTYPE_IB		= 2, /* InfiniBand */	/* [한국어] InfiniBand */
	NVMF_RDMA_PRTYPE_ROCE		= 3, /* InfiniBand RoCE */	/* [한국어] RoCE */
	NVMF_RDMA_PRTYPE_ROCEV2		= 4, /* InfiniBand RoCEV2 */	/* [한국어] RoCEv2 */
	NVMF_RDMA_PRTYPE_IWARP		= 5, /* IWARP */	/* [한국어] iWARP */
};

/*
 * [한국어] TSAS.RDMA_CMS — 연결 관리 서비스
 * RDMA_CM 소켓형 엔드포인트 어드레싱이 표준. IP 기반 Connect 에 대응.
 */
/* RDMA Connection Management Service Type codes for Discovery Log Page
 * entry TSAS RDMA_CMS field
 */
enum {
	NVMF_RDMA_CMS_RDMA_CM	= 1, /* Sockets based endpoint addressing */	/* [한국어] RDMA CM 기반 엔드포인트 — 일반 nvme-rdma 연결 경로 */
};

/*
 * [한국어] TSAS.TCP SECTYPE — NVMe/TCP TLS 수준
 * NONE/TLS1.2/TLS1.3. 호스트 tcp 트랜스포트가 소켓 래핑 여부를 결정.
 * 스펙 NVMe-oF 1.1 · NVMe-TCP 보안 절과 정합.
 */
/* TSAS SECTYPE for TCP transport */
enum {
	NVMF_TCP_SECTYPE_NONE = 0, /* No Security */	/* [한국어] 평문 TCP */
	NVMF_TCP_SECTYPE_TLS12 = 1, /* TLSv1.2, NVMe-oF 1.1 and NVMe-TCP 3.6.1.1 */	/* [한국어] TLS 1.2 보안 채널 */
	NVMF_TCP_SECTYPE_TLS13 = 2, /* TLSv1.3, NVMe-oF 1.1 and NVMe-TCP 3.6.1.1 */	/* [한국어] TLS 1.3 보안 채널 */
	NVMF_TCP_SECTYPE_INVALID = 0xff,	/* [한국어] 잘못된 SECTYPE 가드 */
};

/*
 * [한국어] Admin Queue 깊이 및 blk-mq 태그 예산
 *
 * 스펙 최소 Admin SQ/CQ 깊이 관례 32. 그중 1 슬롯은 Asynchronous Event(AEN)
 * 수신용으로 상시 점유(NVME_NR_AEN_COMMANDS). 나머지가 blk-mq admin 태그.
 * 추가로 Full Queue 판별용 빈 슬롯 1 을 남겨 MQ_TAG_DEPTH = 30.
 * (NVM Express 1.2 §4.1.2 — head==tail 을 empty 로 쓰므로 full 은 한 칸 비움)
 */
#define NVME_AQ_DEPTH		32	/* [한국어] Admin SQ/CQ 엔트리 수(스펙 관례 최소 32) — AQA 에 depth-1 기록 */
#define NVME_NR_AEN_COMMANDS	1	/* [한국어] 상시 제출해 두는 Async Event 요청 개수; 컨트롤러가 이벤트 시 이 CQE 로 통지 */
#define NVME_AQ_BLK_MQ_DEPTH	(NVME_AQ_DEPTH - NVME_NR_AEN_COMMANDS)	/* [한국어] blk-mq admin 큐 깊이 = 전체 - AEN 예약 */

/*
 * Subtract one to leave an empty queue entry for 'Full Queue' condition. See
 * NVM-Express 1.2 specification, section 4.1.2.
 */
#define NVME_AQ_MQ_TAG_DEPTH	(NVME_AQ_BLK_MQ_DEPTH - 1)	/* [한국어] 실제 태그 수 = BLK_MQ_DEPTH-1; 링 full 감지용 빈 칸 1 확보 */

/*
 * [한국어] Controller Register 맵 (BAR0 / Property 공간) 오프셋
 *
 * PCIe: BAR0 MMIO. Fabrics: Property Get/Set 의 offset 과 동일 번호.
 * enable 시퀀스 핵심 순서:
 *   1. CAP(64b) 읽기 — MQES, TO, DSTRD, CSS, MPSMIN/MAX, CMBS, CRMS …
 *   2. VS 로 스펙 버전 확인
 *   3. CC.EN=0 확인 후 AQA(admin 큐 크기), ASQ/ACQ(64b 물리 주소) 기록
 *   4. CC 에 CSS/MPS/AMS/IOSQES/IOCQES 설정 후 EN=1
 *   5. CSTS.RDY=1 될 때까지 CAP.TO 단위로 폴링 (또는 CRTO 타임아웃)
 *   6. Identify + Set Features(NUM_QUEUES) 후 Create CQ/SQ, I/O 시작
 *   7. 종료 시 CC.SHN 또는 EN=0 → CSTS.SHST/RDY 대기
 * DBS(0x1000)부터 큐별 doorbell; stride = 4 << CAP.DSTRD.
 * 작성자: 대부분 호스트 기록, CAP/VS/CSTS/BPINFO 등은 컨트롤러 제공(RO).
 */
enum {
	NVME_REG_CAP	= 0x0000,	/* Controller Capabilities */	/* [한국어] CAP 64-bit RO — 큐 깊이 상한·타임아웃·CSS·페이지 크기(컨트롤러 작성) */
	NVME_REG_VS	= 0x0008,	/* Version */	/* [한국어] Version RO — major/minor/tertiary; NVME_VS()/NVME_MAJOR() 로 분해 */
	NVME_REG_INTMS	= 0x000c,	/* Interrupt Mask Set */	/* [한국어] Interrupt Mask Set — 비트 1 이 해당 벡터 마스크(호스트) */
	NVME_REG_INTMC	= 0x0010,	/* Interrupt Mask Clear */	/* [한국어] Interrupt Mask Clear — 마스크 해제(호스트) */
	NVME_REG_CC	= 0x0014,	/* Controller Configuration */	/* [한국어] CC — EN/CSS/MPS/AMS/SHN/IOSQES/IOCQES/CRIME (호스트 RW) */
	NVME_REG_CSTS	= 0x001c,	/* Controller Status */	/* [한국어] CSTS — RDY/CFS/SHST/NSSRO/PP (컨트롤러 RO; 호스트 폴링) */
	NVME_REG_NSSR	= 0x0020,	/* NVM Subsystem Reset */	/* [한국어] NSSR — NVME_SUBSYS_RESET 매직 기록 시 서브시스템 리셋 */
	NVME_REG_AQA	= 0x0024,	/* Admin Queue Attributes */	/* [한국어] AQA — ASQS/ACQS(각 depth-1); enable 전 호스트 설정 */
	NVME_REG_ASQ	= 0x0028,	/* Admin SQ Base Address */	/* [한국어] Admin SQ Base — 64b 물리 주소, 페이지 정렬; 호스트 DMA 버퍼 */
	NVME_REG_ACQ	= 0x0030,	/* Admin CQ Base Address */	/* [한국어] Admin CQ Base — 64b 물리 주소; 컨트롤러가 CQE 를 씀 */
	NVME_REG_CMBLOC	= 0x0038,	/* Controller Memory Buffer Location */	/* [한국어] CMB Location — BIR+오프셋; PCI BAR 위치 */
	NVME_REG_CMBSZ	= 0x003c,	/* Controller Memory Buffer Size */	/* [한국어] CMB Size — SQ/CQ/LIST/RDS/WDS 지원 비트 + 크기 단위 */
	NVME_REG_BPINFO	= 0x0040,	/* Boot Partition Information */	/* [한국어] Boot Partition Information — 존재/크기(컨트롤러 RO) */
	NVME_REG_BPRSEL	= 0x0044,	/* Boot Partition Read Select */	/* [한국어] Boot Partition Read Select — 호스트가 읽을 파티션 선택 */
	NVME_REG_BPMBL	= 0x0048,	/* Boot Partition Memory Buffer
					 * Location
					 */	/* [한국어] Boot Partition Memory Buffer Location — 호스트 버퍼 주소 */
	NVME_REG_CMBMSC = 0x0050,	/* Controller Memory Buffer Memory
					 * Space Control
					 */	/* [한국어] CMBMSC — CRE/CMSE 로 CMB 메모리 공간 활성화 */
	NVME_REG_CRTO	= 0x0068,	/* Controller Ready Timeouts */	/* [한국어] CRTO — CRWMT/CRIMT; CAP.CRMS 와 함께 RDY 대기 한도 */
	NVME_REG_PMRCAP	= 0x0e00,	/* Persistent Memory Capabilities */	/* [한국어] PMR Capabilities — PMR 존재 시 능력 */
	NVME_REG_PMRCTL	= 0x0e04,	/* Persistent Memory Region Control */	/* [한국어] PMR Control — 영역 enable 등 호스트 제어 */
	NVME_REG_PMRSTS	= 0x0e08,	/* Persistent Memory Region Status */	/* [한국어] PMR Status — 준비/오류 상태 컨트롤러 보고 */
	NVME_REG_PMREBS	= 0x0e0c,	/* Persistent Memory Region Elasticity
					 * Buffer Size
					 */	/* [한국어] PMR Elasticity Buffer Size */
	NVME_REG_PMRSWTP = 0x0e10,	/* Persistent Memory Region Sustained
					 * Write Throughput
					 */	/* [한국어] PMR Sustained Write Throughput */
	NVME_REG_DBS	= 0x1000,	/* SQ 0 Tail Doorbell */	/* [한국어] Doorbell 베이스(SQ0 tail); 이후 큐마다 2*stride 간격 SQ tail/CQ head */
};

/*
 * [한국어] CAP 64-bit 필드 추출 매크로 (컨트롤러→호스트, LE 해석 후)
 *
 * 호스트는 64b CAP 를 한 번 읽어 큐 생성·타임아웃·페이지 크기 협상에 쓴다.
 * MQES = 최대 큐 엔트리 수 - 1. TIMEOUT 단위 500ms. STRIDE=DSTRD → doorbell 간격.
 * NSSRC=서브시스템 리셋 지원. CSS=지원 I/O 커맨드셋 비트맵. MPSMIN/MAX=2^(12+n).
 * CMBS=Controller Memory Buffer 지원.
 */
#define NVME_CAP_MQES(cap)	((cap) & 0xffff)	/* [한국어] Maximum Queue Entries Supported — Create SQ/CQ qsize 상한은 MQES(0-based) */
#define NVME_CAP_TIMEOUT(cap)	(((cap) >> 24) & 0xff)	/* [한국어] CSTS.RDY 대기 한도 단위(500ms); CC.EN 토글 후 폴링 타임아웃 계산 */
#define NVME_CAP_STRIDE(cap)	(((cap) >> 32) & 0xf)	/* [한국어] Doorbell Stride(DSTRD); 바이트 간격 = 4 << stride */
#define NVME_CAP_NSSRC(cap)	(((cap) >> 36) & 0x1)	/* [한국어] NVM Subsystem Reset Supported — 1 이면 NSSR 매직 유효 */
#define NVME_CAP_CSS(cap)	(((cap) >> 37) & 0xff)	/* [한국어] Command Sets Supported 비트맵; CC.CSS 선택 전 교집합 확인 */
#define NVME_CAP_MPSMIN(cap)	(((cap) >> 48) & 0xf)	/* [한국어] 최소 메모리 페이지 크기 지수; 실제 바이트 = 2^(12+MPSMIN) */
#define NVME_CAP_MPSMAX(cap)	(((cap) >> 52) & 0xf)	/* [한국어] 최대 메모리 페이지 크기 지수; CC.MPS 는 MIN..MAX 내 선택 */
#define NVME_CAP_CMBS(cap)	(((cap) >> 57) & 0x1)	/* [한국어] CMB 지원 여부; 1 이면 CMBLOC/CMBSZ 로 큐·데이터 배치 가능 */

#define NVME_CMB_BIR(cmbloc)	((cmbloc) & 0x7)	/* [한국어] CMB PCI BAR 지시자 — 해당 BAR 에 CMB 매핑 */
#define NVME_CMB_OFST(cmbloc)	(((cmbloc) >> 12) & 0xfffff)	/* [한국어] BAR 내 CMB 바이트 오프셋 필드 */

#define NVME_CRTO_CRIMT(crto)	((crto) >> 16)	/* [한국어] Controller Ready Independent of Media Timeout — CRIME=1 경로 */
#define NVME_CRTO_CRWMT(crto)	((crto) & 0xffff)	/* [한국어] Controller Ready With Media Timeout — 미디어 포함 RDY 대기 */

/*
 * [한국어] CMBSZ 플래그 — CMB 에 무엇을 둘 수 있는가
 * SQS/CQS: 서브미션/컴플리션 큐 자체. LISTS: PRP 리스트. RDS/WDS: 읽기/쓰기 데이터.
 * 크기 = SZ * 단위(SZU). 호스트 pci 경로가 CMB 사용 시 DMA 매핑을 BAR 쪽으로 돌린다.
 */
enum {
	NVME_CMBSZ_SQS		= 1 << 0,	/* [한국어] CMB 에 Submission Queue 배치 가능 */
	NVME_CMBSZ_CQS		= 1 << 1,	/* [한국어] CMB 에 Completion Queue 배치 가능 */
	NVME_CMBSZ_LISTS	= 1 << 2,	/* [한국어] CMB 에 PRP 리스트 배치 가능 */
	NVME_CMBSZ_RDS		= 1 << 3,	/* [한국어] CMB 에서 읽기 데이터 버퍼 허용 */
	NVME_CMBSZ_WDS		= 1 << 4,	/* [한국어] CMB 로 쓰기 데이터 버퍼 허용 */

	NVME_CMBSZ_SZ_SHIFT	= 12,	/* [한국어] CMB 크기 값 필드 시프트 */
	NVME_CMBSZ_SZ_MASK	= 0xfffff,	/* [한국어] CMB 크기 값 마스크 — 단위와 곱해 바이트 산출 */

	NVME_CMBSZ_SZU_SHIFT	= 8,	/* [한국어] CMB 크기 단위 지수 필드 시프트 */
	NVME_CMBSZ_SZU_MASK	= 0xf,	/* [한국어] CMB 크기 단위 마스크 — 2^(12+SZU) 바이트 단위 등 */
};

/*
 * Submission and Completion Queue Entry Sizes for the NVM command set.
 * (In bytes and specified as a power of two (2^n)).
 */
/*
 * [한국어] 큐 엔트리 크기 (2^n 바이트) — CC 와 Create Queue 계약
 * SQE 고정 64B → n=6, CQE 고정 16B → n=4. Admin·NVM I/O 모두 동일.
 * 호스트는 CC.IOSQES/IOCQES 에 이 값을 기록하고, DMA 링을 64*depth / 16*depth 로 할당.
 */
#define NVME_ADM_SQES       6	/* [한국어] Submission Queue Entry 크기 = 2^6 = 64 바이트 (스펙 고정) */
#define NVME_NVM_IOSQES		6	/* [한국어] I/O SQE 크기 지수 6 → 64B */
#define NVME_NVM_IOCQES		4	/* [한국어] Completion Queue Entry 크기 = 2^4 = 16 바이트 (스펙 고정) */

/*
 * Controller Configuration (CC) register (Offset 14h)
 */
/*
 * [한국어] CC(Controller Configuration) 비트필드 — 호스트가 enable 시 씀
 *
 * EN: 1 로 올리면 컨트롤러 동작 시작, 0 은 리셋 경로. CSS: CAP.CSS 교집합에서 선택
 * (NVM=0, CSI=6 은 I/O Command Set Independent 선택). MPS: 호스트 페이지 크기 지수.
 * AMS: 중재 RR/WRRU/VS. SHN: 정상/비상 셧다운 통지 → CSTS.SHST 로 완료 확인.
 * IOSQES/IOCQES: 엔트리 크기 지수(6/4). CRIME: 미디어 무관 Ready 모드.
 * Bits 3:1, 25:31 reserved — 0 유지.
 */
enum {
	/* Enable (EN): bit 0 */
	NVME_CC_ENABLE		= 1 << 0,	/* [한국어] CC.EN — 1:컨트롤러 가동, 0:disable/reset; 토글 후 CSTS.RDY 폴링 필수 */
	NVME_CC_EN_SHIFT	= 0,	/* [한국어] EN 비트 위치 */

	/* Bits 03:01 are reserved (NVMe Base Specification rev 2.1) */

	/* I/O Command Set Selected (CSS): bits 06:04 */
	NVME_CC_CSS_SHIFT	= 4,	/* [한국어] CSS 필드 비트 오프셋 4 */
	NVME_CC_CSS_MASK	= 7 << NVME_CC_CSS_SHIFT,	/* [한국어] CSS 3비트 마스크 */
	NVME_CC_CSS_NVM		= 0 << NVME_CC_CSS_SHIFT,	/* [한국어] NVM Command Set 선택 (일반 SSD) */
	NVME_CC_CSS_CSI		= 6 << NVME_CC_CSS_SHIFT,	/* [한국어] I/O Command Set Independent 선택 (다중 셋) */

	/* Memory Page Size (MPS): bits 10:07 */
	NVME_CC_MPS_SHIFT	= 7,	/* [한국어] MPS 필드 시프트 — 호스트 페이지 크기 2^(12+MPS) */
	NVME_CC_MPS_MASK	= 0xf << NVME_CC_MPS_SHIFT,	/* [한국어] MPS 마스크; CAP MPSMIN/MAX 범위 내 */

	/* Arbitration Mechanism Selected (AMS): bits 13:11 */
	NVME_CC_AMS_SHIFT	= 11,	/* [한국어] Arbitration Mechanism 필드 위치 */
	NVME_CC_AMS_MASK	= 7 << NVME_CC_AMS_SHIFT,	/* [한국어] AMS 마스크 */
	NVME_CC_AMS_RR		= 0 << NVME_CC_AMS_SHIFT,	/* [한국어] Round Robin 중재 */
	NVME_CC_AMS_WRRU	= 1 << NVME_CC_AMS_SHIFT,	/* [한국어] Weighted Round Robin with Urgent */
	NVME_CC_AMS_VS		= 7 << NVME_CC_AMS_SHIFT,	/* [한국어] Vendor Specific 중재 */

	/* Shutdown Notification (SHN): bits 15:14 */
	NVME_CC_SHN_SHIFT	= 14,	/* [한국어] Shutdown Notification 필드 시프트 */
	NVME_CC_SHN_MASK	= 3 << NVME_CC_SHN_SHIFT,	/* [한국어] SHN 2비트 마스크 */
	NVME_CC_SHN_NONE	= 0 << NVME_CC_SHN_SHIFT,	/* [한국어] 셧다운 통지 없음 */
	NVME_CC_SHN_NORMAL	= 1 << NVME_CC_SHN_SHIFT,	/* [한국어] 정상 셧다운 — 캐시 플러시 후 SHST 완료 대기 */
	NVME_CC_SHN_ABRUPT	= 2 << NVME_CC_SHN_SHIFT,	/* [한국어] 비상 셧다운 — 최소 절차 */

	/* I/O Submission Queue Entry Size (IOSQES): bits 19:16 */
	NVME_CC_IOSQES_SHIFT	= 16,	/* [한국어] I/O SQE 크기 필드 시프트 */
	NVME_CC_IOSQES_MASK	= 0xf << NVME_CC_IOSQES_SHIFT,	/* [한국어] IOSQES 마스크 */
	NVME_CC_IOSQES		= NVME_NVM_IOSQES << NVME_CC_IOSQES_SHIFT,	/* [한국어] 값 6 을 CC 에 기록 — SQE 64B */

	/* I/O Completion Queue Entry Size (IOCQES): bits 23:20 */
	NVME_CC_IOCQES_SHIFT	= 20,	/* [한국어] I/O CQE 크기 필드 시프트 */
	NVME_CC_IOCQES_MASK	= 0xf << NVME_CC_IOCQES_SHIFT,	/* [한국어] IOCQES 마스크 */
	NVME_CC_IOCQES		= NVME_NVM_IOCQES << NVME_CC_IOCQES_SHIFT,	/* [한국어] 값 4 를 CC 에 기록 — CQE 16B */

	/* Controller Ready Independent of Media Enable (CRIME): bit 24 */
	NVME_CC_CRIME		= 1 << 24,	/* [한국어] 미디어 준비 전 Ready 허용; CRTO.CRIMT 타임아웃 경로와 연동 */

	/* Bits 25:31 are reserved (NVMe Base Specification rev 2.1) */
};

/*
 * [한국어] CSTS(Controller Status) — 컨트롤러가 쓰고 호스트가 폴링
 *
 * RDY: CC.EN 과 쌍. CFS: 치명 오류 시 1 → 호스트는 리셋 외 복구 불가 취급.
 * SHST: 셧다운 진행/완료. NSSRO: 서브시스템 리셋 발생 래치. PP: processing paused.
 */
enum {
	NVME_CSTS_RDY		= 1 << 0,	/* [한국어] Ready — EN=1 후 1 이면 Admin 제출 가능; EN=0 후 0 대기 */
	NVME_CSTS_CFS		= 1 << 1,	/* [한국어] Controller Fatal Status — 하드웨어 치명 오류; I/O 중단·리셋 필요 */
	NVME_CSTS_NSSRO		= 1 << 4,	/* [한국어] NVM Subsystem Reset Occurred — 호스트 인지 후 클리어 절차 */
	NVME_CSTS_PP		= 1 << 5,	/* [한국어] Processing Paused — 컨트롤러 처리 일시 정지 상태 */
	NVME_CSTS_SHST_NORMAL	= 0 << 2,	/* [한국어] 셧다운 통지 없음/정상 동작 */
	NVME_CSTS_SHST_OCCUR	= 1 << 2,	/* [한국어] 셧다운 처리 중 — 호스트 대기 */
	NVME_CSTS_SHST_CMPLT	= 2 << 2,	/* [한국어] 셧다운 완료 — 전원 제거 안전 */
	NVME_CSTS_SHST_MASK	= 3 << 2,	/* [한국어] SHST 2비트 마스크 */
};

/*
 * [한국어] CMBMSC — CMB 메모리 공간 제어 비트
 * CRE: CMB enable. CMSE: 메모리 공간 enable. 호스트가 CMB 를 PCI 메모리로 노출할 때 사용.
 */
enum {
	NVME_CMBMSC_CRE		= 1 << 0,	/* [한국어] CMB Enable — CMB 활성화(호스트 기록) */
	NVME_CMBMSC_CMSE	= 1 << 1,	/* [한국어] CMB Memory Space Enable — 메모리 공간 설정 */
};

/*
 * [한국어] CAP.CSS 비트 — 지원 커맨드 셋
 * bit0 NVM Command Set, bit6 I/O Command Set Independent. CC.CSS 선택 값과 대응.
 */
enum {
	NVME_CAP_CSS_NVM	= 1 << 0,	/* [한국어] NVM I/O Command Set 지원 */
	NVME_CAP_CSS_CSI	= 1 << 6,	/* [한국어] Command Set Independent 선택 가능(CC.CSS=6 경로) */
};

/*
 * [한국어] CAP.CRMS — Controller Ready Modes Supported
 * 미디어 포함/독립 Ready 타임아웃 모드 지원 비트. CRTO 와 함께 enable 대기 정책 결정.
 */
enum {
	NVME_CAP_CRMS_CRWMS	= 1ULL << 59,	/* [한국어] Ready With Media 모드 지원 */
	NVME_CAP_CRMS_CRIMS	= 1ULL << 60,	/* [한국어] Ready Independent of Media 모드 지원 */
};

/*
 * [한국어] Identify Controller 전원 상태 디스크립터 (psd[32] 원소)
 * 컨트롤러가 보고, 호스트 APST/전력 정책이 소비. max_power 단위 centiwatt.
 * entry/exit_lat 마이크로초. 플래그로 스케일·비동작 상태 표시. 전부 LE 멀티바이트.
 */
struct nvme_id_power_state {
	__le16			max_power;	/* centiwatts */	/* [한국어] 최대 전력(centiwatt) LE16 — 컨트롤러 작성 */
	__u8			rsvd2;	/* [한국어] 예약 0 */
	__u8			flags;	/* [한국어] MAX_POWER_SCALE / NON_OP_STATE 비트 */
	__le32			entry_lat;	/* microseconds */	/* [한국어] 이 상태로 진입 지연(us) LE32 */
	__le32			exit_lat;	/* microseconds */	/* [한국어] 이 상태에서 이탈 지연(us) LE32 */
	__u8			read_tput;	/* [한국어] 상대 읽기 처리량(0=최고) */
	__u8			read_lat;	/* [한국어] 상대 읽기 지연 */
	__u8			write_tput;	/* [한국어] 상대 쓰기 처리량 */
	__u8			write_lat;	/* [한국어] 상대 쓰기 지연 */
	__le16			idle_power;	/* [한국어] 유휴 전력 LE16 */
	__u8			idle_scale;	/* [한국어] 유휴 전력 스케일 */
	__u8			rsvd19;	/* [한국어] 예약 0 */
	__le16			active_power;	/* [한국어] 활성 전력 LE16 */
	__u8			active_work_scale;	/* [한국어] 활성 작업부하 스케일 */
	__u8			rsvd23[9];	/* [한국어] 예약 패딩 — 디스크립터 정렬 */
};

enum {
	NVME_PS_FLAGS_MAX_POWER_SCALE	= 1 << 0,	/* [한국어] max_power 스케일(0.0001W vs 0.01W) */
	NVME_PS_FLAGS_NON_OP_STATE	= 1 << 1,	/* [한국어] 비동작 전원 상태 — I/O 불가, 진입 시 대기 필요 */
};

/*
 * [한국어] Identify ctratt 일부 속성 비트 별칭
 * 128-bit Host ID, TBKAS(Traffic Based Keep Alive), ELBAS, RHII, FDPS 등.
 */
enum nvme_ctrl_attr {
	NVME_CTRL_ATTR_HID_128_BIT	= (1 << 0),	/* [한국어] 128-bit Host Identifier 지원 */
	NVME_CTRL_ATTR_TBKAS		= (1 << 6),	/* [한국어] Traffic Based Keep Alive — 완료 트래픽이 KA 대체 가능 */
	NVME_CTRL_ATTR_ELBAS		= (1 << 15),	/* [한국어] Extended LBA Formats Supported */
	NVME_CTRL_ATTR_RHII		= (1 << 18),	/* [한국어] Reservations and Host Identifier Interaction */
	NVME_CTRL_ATTR_FDPS		= (1 << 19),	/* [한국어] Flexible Data Placement Supported */
};

/*
 * [한국어] Identify Controller 데이터 구조 (CNS=01h, 4096B)
 *
 * 컨트롤러가 DMA 로 채움(LE). 호스트 프로브의 진실 공급원:
 *   식별(vid/sn/mn/fr/subnqn/cntlid), 능력(mdts/oncs/oacs/sgls/nn),
 *   멀티패스(cmic/anacap), 온도(wctemp), 전원(psd[]), fabrics(ioccsz…).
 * core 는 이 값으로 큐 수·SGL·keep-alive·ANA·sysfs 속성을 구성한다.
 * 작성자=컨트롤러, 소비=호스트. 온와이어 레이아웃 불변.
 */
struct nvme_id_ctrl {
	__le16			vid;	/* [한국어] PCI Vendor ID LE16 — 컨트롤러 보고 */
	__le16			ssvid;	/* [한국어] PCI Subsystem Vendor ID */
	char			sn[20];	/* [한국어] 시리얼 번호 ASCII 20B (엔디안 없음) */
	char			mn[40];	/* [한국어] 모델명 ASCII 40B */
	char			fr[8];	/* [한국어] 펌웨어 리비전 ASCII 8B */
	__u8			rab;	/* [한국어] Recommended Arbitration Burst */
	__u8			ieee[3];	/* [한국어] IEEE OUI */
	__u8			cmic;	/* [한국어] Multi-path I/O and NS Sharing Capabilities — MULTI_PORT/CTRL/ANA */
	__u8			mdts;	/* [한국어] Max Data Transfer Size — 2^mdts * CAP MPS 단위; 0=무제한 */
	__le16			cntlid;	/* [한국어] Controller ID — 멀티컨트롤러 서브시스템 내 식별 LE16 */
	__le32			ver;	/* [한국어] Version — VS 레지스터와 동일 인코딩 LE32 */
	__le32			rtd3r;	/* [한국어] RTD3 Resume Latency */
	__le32			rtd3e;	/* [한국어] RTD3 Entry Latency */
	__le32			oaes;	/* [한국어] Optional Asynchronous Events Supported 비트맵 */
	__le32			ctratt;	/* [한국어] Controller Attributes — 128b HostID/NVM Sets/UUID 등 */
	__u8			rsvd100[11];	/* [한국어] 예약 */
	__u8			cntrltype;	/* [한국어] 컨트롤러 타입 IO/DISC/ADMIN (nvme_ctrl_type) */
	__u8			fguid[16];	/* [한국어] FRU GUID */
	__le16			crdt1;	/* [한국어] Command Retry Delay Time 1 */
	__le16			crdt2;	/* [한국어] Command Retry Delay Time 2 */
	__le16			crdt3;	/* [한국어] Command Retry Delay Time 3 */
	__u8			rsvd134[122];	/* [한국어] 예약 */
	__le16			oacs;	/* [한국어] Optional Admin Command Support — Security/NS Mgmt/Directives/DBBUF */
	__u8			acl;	/* [한국어] Abort Command Limit (0-based) */
	__u8			aerl;	/* [한국어] Async Event Request Limit (0-based) — AER 슬롯 수 */
	__u8			frmw;	/* [한국어] Firmware Updates 능력 */
	__u8			lpa;	/* [한국어] Log Page Attributes — Effects 로그 등 */
	__u8			elpe;	/* [한국어] Error Log Page Entries (0-based) */
	__u8			npss;	/* [한국어] Number of Power States Support (0-based) — psd[] 유효 개수 */
	__u8			avscc;	/* [한국어] Admin Vendor Specific Command Configuration */
	__u8			apsta;	/* [한국어] Autonomous Power State Transition Attributes */
	__le16			wctemp;	/* [한국어] Warning Composite Temperature Threshold (Kelvin) */
	__le16			cctemp;	/* [한국어] Critical Composite Temperature Threshold */
	__le16			mtfa;	/* [한국어] Maximum Time for Firmware Activation */
	__le32			hmpre;	/* [한국어] Host Memory Buffer Preferred Size */
	__le32			hmmin;	/* [한국어] Host Memory Buffer Minimum Size */
	__u8			tnvmcap[16];	/* [한국어] Total NVM Capacity 128-bit LE */
	__u8			unvmcap[16];	/* [한국어] Unallocated NVM Capacity 128-bit LE */
	__le32			rpmbs;	/* [한국어] Replay Protected Memory Block Support */
	__le16			edstt;	/* [한국어] Extended Device Self-test Time */
	__u8			dsto;	/* [한국어] Device Self-test Options */
	__u8			fwug;	/* [한국어] Firmware Update Granularity */
	__le16			kas;	/* [한국어] Keep Alive Support — 0 이면 KA 미지원 */
	__le16			hctma;	/* [한국어] Host Controlled Thermal Management Attributes */
	__le16			mntmt;	/* [한국어] Minimum Thermal Management Temperature */
	__le16			mxtmt;	/* [한국어] Maximum Thermal Management Temperature */
	__le32			sanicap;	/* [한국어] Sanitize Capabilities */
	__le32			hmminds;	/* [한국어] Host Memory Buffer Minimum Descriptor Entry Size */
	__le16			hmmaxd;	/* [한국어] Host Memory Maximum Descriptors Entries */
	__le16			nvmsetidmax;	/* [한국어] NVM Set Identifier Maximum */
	__le16			endgidmax;	/* [한국어] Endurance Group Identifier Maximum */
	__u8			anatt;	/* [한국어] ANA Transition Time */
	__u8			anacap;	/* [한국어] ANA Capabilities — multipath 경로 상태 보고 */
	__le32			anagrpmax;	/* [한국어] ANA Group ID Maximum */
	__le32			nanagrpid;	/* [한국어] Number of ANA Group IDs */
	__u8			rsvd352[160];	/* [한국어] 예약 */
	__u8			sqes;	/* [한국어] SQ Entry Size 지원 (min/max nibble) — 보통 6/6 */
	__u8			cqes;	/* [한국어] CQ Entry Size 지원 — 보통 4/4 */
	__le16			maxcmd;	/* [한국어] Maximum Outstanding Commands */
	__le32			nn;	/* [한국어] Number of Namespaces — 스캔 상한/목록 크기 근거 */
	__le16			oncs;	/* [한국어] Optional NVM Command Support — Compare/DSM/WZ/Reservations */
	__le16			fuses;	/* [한국어] Fused Operation Support */
	__u8			fna;	/* [한국어] Format NVM Attributes */
	__u8			vwc;	/* [한국어] Volatile Write Cache — PRESENT 비트로 플러시 정책 */
	__le16			awun;	/* [한국어] Atomic Write Unit Normal (0-based LBA) */
	__le16			awupf;	/* [한국어] Atomic Write Unit Power Fail */
	__u8			nvscc;	/* [한국어] NVM Vendor Specific Command Configuration */
	__u8			nwpc;	/* [한국어] Namespace Write Protection Capabilities */
	__le16			acwu;	/* [한국어] Atomic Compare & Write Unit */
	__u8			rsvd534[2];	/* [한국어] 예약 */
	__le32			sgls;	/* [한국어] SGL Support 비트맵 — PRP 대 SGL 선택·정렬·키드 SGL 근거 */
	__le32			mnan;	/* [한국어] Maximum Number of Allowed Namespaces */
	__u8			rsvd544[224];	/* [한국어] 예약 */
	char			subnqn[256];	/* [한국어] 서브시스템 NQN 문자열 256B 필드 — fabrics Connect 대상 */
	__u8			rsvd1024[768];	/* [한국어] 예약(직후 fabrics I/O 큐 속성) */
	__le32			ioccsz;	/* [한국어] I/O Queue Command Capsule Size (fabrics, 16B 단위) */
	__le32			iorcsz;	/* [한국어] I/O Queue Response Capsule Size (fabrics) */
	__le16			icdoff;	/* [한국어] In Capsule Data Offset */
	__u8			ctrattr;	/* [한국어] Controller Attributes (fabrics 쪽 추가 속성 바이트) */
	__u8			msdbd;	/* [한국어] Maximum SGL Data Block Descriptors */
	__u8			rsvd1804[2];	/* [한국어] 예약 */
	__u8			dctype;	/* [한국어] Discovery Controller Type (nvme_dctype) */
	__u8			rsvd1807[241];	/* [한국어] 예약 — psd 앞 정렬 */
	struct nvme_id_power_state	psd[32];	/* [한국어] 전원 상태 디스크립터 최대 32개 — npss+1 유효 */
	__u8			vs[1024];	/* [한국어] Vendor Specific 영역 1024B */
};

/*
 * [한국어] Identify Controller 능력 비트 — cmic/oncs/oacs/sgls/ctratt 해석용
 * 호스트가 선택 기능(discard, SGL, multipath, PR, HMB…)을 켤지 판단하는 근거.
 */
enum {
	NVME_CTRL_CMIC_MULTI_PORT		= 1 << 0,	/* [한국어] 멀티 포트 서브시스템 */
	NVME_CTRL_CMIC_MULTI_CTRL		= 1 << 1,	/* [한국어] 멀티 컨트롤러 */
	NVME_CTRL_CMIC_ANA			= 1 << 3,	/* [한국어] ANA 보고 지원 — multipath */
	NVME_CTRL_ONCS_COMPARE			= 1 << 0,	/* [한국어] Compare 명령 지원 */
	NVME_CTRL_ONCS_WRITE_UNCORRECTABLE	= 1 << 1,	/* [한국어] Write Uncorrectable 지원 */
	NVME_CTRL_ONCS_DSM			= 1 << 2,	/* [한국어] Dataset Management 지원 — discard 경로 */
	NVME_CTRL_ONCS_WRITE_ZEROES		= 1 << 3,	/* [한국어] Write Zeroes 지원 */
	NVME_CTRL_ONCS_RESERVATIONS		= 1 << 5,	/* [한국어] Reservations 지원 — pr.c */
	NVME_CTRL_ONCS_TIMESTAMP		= 1 << 6,	/* [한국어] Timestamp feature 지원 */
	NVME_CTRL_VWC_PRESENT			= 1 << 0,	/* [한국어] 휘발 쓰기 캐시 존재 — flush 의미 */
	NVME_CTRL_OACS_SEC_SUPP                 = 1 << 0,	/* [한국어] Security Send/Recv 지원 — Opal 등 */
	NVME_CTRL_OACS_NS_MNGT_SUPP		= 1 << 3,	/* [한국어] NS Management/Attach 지원 */
	NVME_CTRL_OACS_DIRECTIVES		= 1 << 5,	/* [한국어] Directives 지원 */
	NVME_CTRL_OACS_DBBUF_SUPP		= 1 << 8,	/* [한국어] Doorbell Buffer Config 지원 */
	NVME_CTRL_LPA_CMD_EFFECTS_LOG		= 1 << 1,	/* [한국어] Commands Supported and Effects 로그 */
	NVME_CTRL_CTRATT_128_ID			= 1 << 0,	/* [한국어] 128-bit Host Identifier */
	NVME_CTRL_CTRATT_NON_OP_PSP		= 1 << 1,	/* [한국어] Non-Operational Power State Permissive */
	NVME_CTRL_CTRATT_NVM_SETS		= 1 << 2,	/* [한국어] NVM Sets */
	NVME_CTRL_CTRATT_READ_RECV_LVLS		= 1 << 3,	/* [한국어] Read Recovery Levels */
	NVME_CTRL_CTRATT_ENDURANCE_GROUPS	= 1 << 4,	/* [한국어] Endurance Groups */
	NVME_CTRL_CTRATT_PREDICTABLE_LAT	= 1 << 5,	/* [한국어] Predictable Latency Mode */
	NVME_CTRL_CTRATT_NAMESPACE_GRANULARITY	= 1 << 7,	/* [한국어] Namespace Granularity */
	NVME_CTRL_CTRATT_UUID_LIST		= 1 << 9,	/* [한국어] UUID List */
	NVME_CTRL_SGLS_BYTE_ALIGNED             = 1,	/* [한국어] SGL 지원·바이트 정렬 데이터 블록 */
	NVME_CTRL_SGLS_DWORD_ALIGNED            = 2,	/* [한국어] SGL 지원·DWORD 정렬 요구 */
	NVME_CTRL_SGLS_KSDBDS			= 1 << 2,	/* [한국어] Keyed SGL Data Block Descriptor 지원 */
	NVME_CTRL_SGLS_MSDS                     = 1 << 19,	/* [한국어] Metadata SGL 지원 */
	NVME_CTRL_SGLS_SAOS                     = 1 << 20,	/* [한국어] SGL Attribute Overriding 등 */
};

/*
 * [한국어] LBA Format 디스크립터 — id_ns.lbaf[] 원소
 * ds=데이터 크기 2^n 바이트, ms=메타데이터, rp=상대 성능. Format/flbas 선택 기준.
 */
struct nvme_lbaf {
	__le16			ms;	/* [한국어] Metadata Size(바이트) LE16 — PI 포함 가능 */
	__u8			ds;	/* [한국어] LBA Data Size 지수 — 실제 크기 2^ds (예: 9=512, 12=4096) */
	__u8			rp;	/* [한국어] Relative Performance — Best…Degraded */
};

/*
 * [한국어] Identify Namespace (CNS=00h) — 블록 장치 geometry 원천
 *
 * nsze/ncap/nuse → 용량. flbas+lbaf[] → 논리 블록 크기. nmic/rescap → 공유·예약.
 * 호스트가 gendisk 용량·PI·discard 정렬(npwg 등)을 여기서 파생. 컨트롤러 작성 LE.
 */
struct nvme_id_ns {
	__le64			nsze;	/* [한국어] Namespace Size (LBA 수) LE64 — 보고 용량 */
	__le64			ncap;	/* [한국어] Namespace Capacity — 할당 가능 용량 */
	__le64			nuse;	/* [한국어] Namespace Utilization — 실제 사용량 */
	__u8			nsfeat;	/* [한국어] NS 특징 — thin/atomics/optperf 비트 */
	__u8			nlbaf;	/* [한국어] LBAF 개수-1 — lbaf[] 유효 엔트리 */
	__u8			flbas;	/* [한국어] Formatted LBA Size — 활성 LBAF 인덱스+메타 위치 */
	__u8			mc;	/* [한국어] Metadata Capabilities — extended LBA / 별도 포인터 */
	__u8			dpc;	/* [한국어] End-to-end Data Protection Capabilities */
	__u8			dps;	/* [한국어] End-to-end Data Protection Type Settings (활성 PI) */
	__u8			nmic;	/* [한국어] Namespace Multi-path I/O and Namespace Sharing */
	__u8			rescap;	/* [한국어] Reservation Capabilities 비트맵 */
	__u8			fpi;	/* [한국어] Format Progress Indicator */
	__u8			dlfeat;	/* [한국어] Deallocate Logical Block Features */
	__le16			nawun;	/* [한국어] Namespace Atomic Write Unit Normal */
	__le16			nawupf;	/* [한국어] Namespace Atomic Write Unit Power Fail */
	__le16			nacwu;	/* [한국어] Namespace Atomic Compare & Write Unit */
	__le16			nabsn;	/* [한국어] Namespace Atomic Boundary Size Normal */
	__le16			nabo;	/* [한국어] Namespace Atomic Boundary Offset */
	__le16			nabspf;	/* [한국어] Namespace Atomic Boundary Size Power Fail */
	__le16			noiob;	/* [한국어] Namespace Optimal I/O Boundary — 병합/정렬 힌트 */
	__u8			nvmcap[16];	/* [한국어] NVM Capacity 128-bit */
	__le16			npwg;	/* [한국어] Namespace Preferred Write Granularity — discard/쓰기 정렬 */
	__le16			npwa;	/* [한국어] Namespace Preferred Write Alignment */
	__le16			npdg;	/* [한국어] Namespace Preferred Deallocate Granularity */
	__le16			npda;	/* [한국어] Namespace Preferred Deallocate Alignment */
	__le16			nows;	/* [한국어] Namespace Optimal Write Size */
	__u8			rsvd74[18];	/* [한국어] 예약 */
	__le32			anagrpid;	/* [한국어] ANA Group Identifier — multipath 그룹 */
	__u8			rsvd96[3];	/* [한국어] 예약 */
	__u8			nsattr;	/* [한국어] Namespace Attributes — RO 등 */
	__le16			nvmsetid;	/* [한국어] NVM Set Identifier */
	__le16			endgid;	/* [한국어] Endurance Group Identifier */
	__u8			nguid[16];	/* [한국어] Namespace Globally Unique Identifier 16B */
	__u8			eui64[8];	/* [한국어] IEEE EUI-64 */
	struct nvme_lbaf	lbaf[64];	/* [한국어] LBA Format 테이블 최대 64 — flbas 가 활성 선택 */
	__u8			vs[3712];	/* [한국어] Vendor Specific — 4096B 구조 패딩 */
};

/*
 * [한국어] I/O Command Set Independent Identify Namespace (CNS=08h)
 * 커맨드셋 무관 NS 속성. nstat 로 Not Ready 등. ZNS/NVM 공통 경로가 소비.
 * 컨트롤러 작성, 호스트 프로브/재스캔이 읽음. LE 멀티바이트.
 */
/* I/O Command Set Independent Identify Namespace Data Structure */
struct nvme_id_ns_cs_indep {
	__u8			nsfeat;	/* [한국어] NS 특징 비트 — thin/atomics/optperf */
	__u8			nmic;	/* [한국어] 멀티패스/공유 NS 능력 */
	__u8			rescap;	/* [한국어] Reservation Capabilities */
	__u8			fpi;	/* [한국어] Format Progress Indicator */
	__le32			anagrpid;	/* [한국어] ANA Group ID LE32 */
	__u8			nsattr;	/* [한국어] NS 속성 — RO 등 */
	__u8			rsvd9;	/* [한국어] 예약 0 */
	__le16			nvmsetid;	/* [한국어] NVM Set Identifier */
	__le16			endgid;	/* [한국어] Endurance Group Identifier */
	__u8			nstat;	/* [한국어] NS 상태 — NRDY 등 즉시 I/O 가능 여부 */
	__u8			rsvd15[4081];	/* [한국어] 예약 — 4096B 페이지 패딩 */
};

/*
 * [한국어] ZNS LBA Format Extension — 존 크기·디스크립터 확장
 * zsze=존 크기(LBA), zdes=존 디스크립터 확장 크기. zns.c 가 존 모델 구성.
 */
struct nvme_zns_lbafe {
	__le64			zsze;	/* [한국어] Zone Size (LBA 수) LE64 */
	__u8			zdes;	/* [한국어] Zone Descriptor Extension Size */
	__u8			rsvd9[7];	/* [한국어] 예약 패딩 */
};

/*
 * [한국어] ZNS Identify Namespace (CNS=05h, CSI=ZNS)
 * 존 연산 특성 zoc/ozcs, active/open 자원 한도 mar/mor. 호스트 ZNS 정책.
 */
struct nvme_id_ns_zns {
	__le16			zoc;	/* [한국어] Zone Operation Characteristics */
	__le16			ozcs;	/* [한국어] Optional Zoned Command Support */
	__le32			mar;	/* [한국어] Maximum Active Resources (0-based) */
	__le32			mor;	/* [한국어] Maximum Open Resources (0-based) */
	__le32			rrl;	/* [한국어] Reset Recommended Limit */
	__le32			frl;	/* [한국어] Finish Recommended Limit */
	__u8			rsvd20[2796];	/* [한국어] 예약 — lbafe 앞 정렬 */
	struct nvme_zns_lbafe	lbafe[64];	/* [한국어] LBAF별 존 확장 포맷 테이블 */
	__u8			vs[256];	/* [한국어] Vendor Specific */
};

/*
 * [한국어] ZNS Identify Controller — zasl 등 Zone Append 크기 한계
 */
struct nvme_id_ctrl_zns {
	__u8	zasl;	/* [한국어] Zone Append Size Limit — 2^zasl * MPS 단위 */
	__u8	rsvd1[4095];	/* [한국어] 예약 — 4096B */
};

/*
 * [한국어] NVM Command Set Identify Namespace 확장 (ELBAF·가드 PI)
 * sizeof==4096 정적 검증. 2.0+ NVM 확장 필드. 컨트롤러 작성.
 */
struct nvme_id_ns_nvm {
	__le64	lbstm;	/* [한국어] Logical Block Storage Tag Mask */
	__u8	pic;	/* [한국어] Protection Information Capabilities */
	__u8	rsvd9[3];	/* [한국어] 예약 */
	__le32	elbaf[64];	/* [한국어] Extended LBA Format — STS/guard/QPIF 팩 */
	__le32	npdgl;	/* [한국어] Namespace Preferred Deallocate Granularity Large */
	__le32	nprg;	/* [한국어] Namespace Preferred Read Granularity */
	__le32	npra;	/* [한국어] Namespace Preferred Read Alignment */
	__le32	nors;	/* [한국어] Namespace Optimal Read Size */
	__le32	npdal;	/* [한국어] Namespace Preferred Deallocate Alignment Large */
	__u8	rsvd288[3808];	/* [한국어] 예약 — 4096B 패딩 */
};

static_assert(sizeof(struct nvme_id_ns_nvm) == 4096);	/* [한국어] 온와이어 Identify 페이지 크기 불변식 — 스펙 4096B */

enum {
	NVME_ID_NS_NVM_STS_MASK		= 0x7f,	/* [한국어] ELBAF Storage Tag Size 마스크 */
	NVME_ID_NS_NVM_GUARD_SHIFT	= 7,	/* [한국어] guard 타입 필드 시프트 */
	NVME_ID_NS_NVM_GUARD_MASK	= 0x3,	/* [한국어] guard 타입 마스크 — 16/32/64b */
	NVME_ID_NS_NVM_QPIF_SHIFT	= 9,	/* [한국어] Qualified PI Format 시프트 */
	NVME_ID_NS_NVM_QPIF_MASK	= 0xf,	/* [한국어] QPIF 마스크 */
	NVME_ID_NS_NVM_QPIFS		= 1 << 3,	/* [한국어] Qualified PI Formats Supported 비트 */
};

/*
 * [한국어] nvme_elbaf_sts() — ELBAF 에서 Storage Tag Size 비트 추출 (PI 확장 포맷)
 */
static inline __u8 nvme_elbaf_sts(__u32 elbaf)
{
	return elbaf & NVME_ID_NS_NVM_STS_MASK;	/* [한국어] STS 하위 7비트 */
}

/*
 * [한국어] nvme_elbaf_guard_type() — ELBAF guard 타입 필드 — 16/32/64b 가드
 */
static inline __u8 nvme_elbaf_guard_type(__u32 elbaf)
{
	return (elbaf >> NVME_ID_NS_NVM_GUARD_SHIFT) & NVME_ID_NS_NVM_GUARD_MASK;	/* [한국어] guard 타입 추출 */
}

/*
 * [한국어] nvme_elbaf_qualified_guard_type() — Qualified PI format 인덱스 추출
 */
static inline __u8 nvme_elbaf_qualified_guard_type(__u32 elbaf)
{
	return (elbaf >> NVME_ID_NS_NVM_QPIF_SHIFT) & NVME_ID_NS_NVM_QPIF_MASK;	/* [한국어] QPIF 인덱스 */
}

/*
 * [한국어] NVM Command Set Identify Controller 확장
 * Verify/Write Zeroes/DSM 크기 한계 등 NVM 셋 전용 컨트롤러 능력.
 */
struct nvme_id_ctrl_nvm {
	__u8	vsl;	/* [한국어] Verify Size Limit */
	__u8	wzsl;	/* [한국어] Write Zeroes Size Limit */
	__u8	wusl;	/* [한국어] Write Uncorrectable Size Limit */
	__u8	dmrl;	/* [한국어] Dataset Management Ranges Limit */
	__le32	dmrsl;	/* [한국어] Dataset Management Range Size Limit LE32 */
	__le64	dmsl;	/* [한국어] Dataset Management Size Limit LE64 */
	__u8	rsvd16[4080];	/* [한국어] 예약 — 4096B */
};

/*
 * [한국어] Identify CNS (CDW10) — 어떤 Identify 데이터 구조를 읽을지
 * 호스트 프로브: CNS=CTRL 로 nvme_id_ctrl, CNS=NS 로 nvme_id_ns,
 * ACTIVE_LIST 로 NSID 목록. CSI 결합 CNS 는 ZNS/NVM 확장 Identify.
 */
enum {
	NVME_ID_CNS_NS			= 0x00,	/* [한국어] Identify Namespace — nvme_id_ns */
	NVME_ID_CNS_CTRL		= 0x01,	/* [한국어] Identify Controller — nvme_id_ctrl */
	NVME_ID_CNS_NS_ACTIVE_LIST	= 0x02,	/* [한국어] 활성 NSID 목록 — 스캔 시작점 */
	NVME_ID_CNS_NS_DESC_LIST	= 0x03,	/* [한국어] NS 식별 디스크립터 리스트 */
	NVME_ID_CNS_CS_NS		= 0x05,	/* [한국어] I/O CS 특정 NS Identify (ZNS 등) */
	NVME_ID_CNS_CS_CTRL		= 0x06,	/* [한국어] I/O CS 특정 CTRL Identify */
	NVME_ID_CNS_NS_ACTIVE_LIST_CS	= 0x07,	/* [한국어] 활성 NS 목록 (커맨드셋 한정) */
	NVME_ID_CNS_NS_CS_INDEP		= 0x08,	/* [한국어] CS-independent NS Identify */
	NVME_ID_CNS_NS_PRESENT_LIST	= 0x10,	/* [한국어] 존재 NS 목록(미첨부 포함) */
	NVME_ID_CNS_NS_PRESENT		= 0x11,	/* [한국어] 특정 NS present Identify */
	NVME_ID_CNS_CTRL_NS_LIST	= 0x12,	/* [한국어] 컨트롤러에 attach 된 NS 목록 */
	NVME_ID_CNS_CTRL_LIST		= 0x13,	/* [한국어] 서브시스템 컨트롤러 목록 */
	NVME_ID_CNS_SCNDRY_CTRL_LIST	= 0x15,	/* [한국어] 2차 컨트롤러 목록 */
	NVME_ID_CNS_NS_GRANULARITY	= 0x16,	/* [한국어] NS 단위 세분성 */
	NVME_ID_CNS_UUID_LIST		= 0x17,	/* [한국어] UUID 목록 */
	NVME_ID_CNS_ENDGRP_LIST		= 0x19,	/* [한국어] Endurance Group 목록 */
};

enum {
	NVME_CSI_NVM			= 0,	/* [한국어] Command Set Identifier NVM=0 */
	NVME_CSI_ZNS			= 2,	/* [한국어] Command Set Identifier ZNS=2 */
};

/*
 * [한국어] Directive 타입/연산 코드 — Streams 등 쓰기 분류 힌트 프레임워크
 */
enum {
	NVME_DIR_IDENTIFY		= 0x00,	/* [한국어] Identify Directive */
	NVME_DIR_STREAMS		= 0x01,	/* [한국어] Streams Directive */
	NVME_DIR_SND_ID_OP_ENABLE	= 0x01,	/* [한국어] Directive Send: Identify enable */
	NVME_DIR_SND_ST_OP_REL_ID	= 0x01,	/* [한국어] Streams: release identifier */
	NVME_DIR_SND_ST_OP_REL_RSC	= 0x02,	/* [한국어] Streams: release resources */
	NVME_DIR_RCV_ID_OP_PARAM	= 0x01,	/* [한국어] Directive Receive: Identify parameters */
	NVME_DIR_RCV_ST_OP_PARAM	= 0x01,	/* [한국어] Streams: receive parameters */
	NVME_DIR_RCV_ST_OP_STATUS	= 0x02,	/* [한국어] Streams: receive status */
	NVME_DIR_RCV_ST_OP_RESOURCE	= 0x03,	/* [한국어] Streams: allocate resources */
	NVME_DIR_ENDIR			= 0x01,	/* [한국어] Enable Directive 비트 */
};

/*
 * [한국어] Namespace Identify 플래그·LBAF·PI 비트 — geometry/보호 정보 해석
 * flbas 에서 LBAF 인덱스 재조립, DPC/DPS 로 T10 PI 타입/위치 결정.
 */
enum {
	NVME_NS_FEAT_THIN	= 1 << 0,	/* [한국어] 씬 프로비저닝 NS */
	NVME_NS_FEAT_ATOMICS	= 1 << 1,	/* [한국어] NS 원자성 특화 필드 유효 */
	NVME_NS_FEAT_OPTPERF_SHIFT = 4,	/* [한국어] OPTPERF 비트 시작 위치 */
	/* In NVMe version 2.0 and below, OPTPERF is only bit 4 of NSFEAT */
	NVME_NS_FEAT_OPTPERF_MASK = 0x1,	/* [한국어] 2.0 이하 OPTPERF 1비트 */
	/* Since version 2.1, OPTPERF is bits 4 and 5 of NSFEAT */
	NVME_NS_FEAT_OPTPERF_MASK_2_1 = 0x3,	/* [한국어] 2.1+ OPTPERF 2비트 */
	NVME_NS_ATTR_RO		= 1 << 0,	/* [한국어] NS 읽기 전용 속성 */
	NVME_NS_FLBAS_LBA_MASK	= 0xf,	/* [한국어] flbas 하위 활성 LBAF 인덱스 마스크 */
	NVME_NS_FLBAS_LBA_UMASK	= 0x60,	/* [한국어] flbas 확장 인덱스 비트 */
	NVME_NS_FLBAS_LBA_SHIFT	= 1,	/* [한국어] 확장 인덱스 정렬 시프트 */
	NVME_NS_FLBAS_META_EXT	= 0x10,	/* [한국어] 메타데이터가 extended LBA 에 포함 */
	NVME_NS_NMIC_SHARED	= 1 << 0,	/* [한국어] 다중 컨트롤러 공유 NS */
	NVME_NS_ROTATIONAL	= 1 << 4,	/* [한국어] 회전 매체 힌트 */
	NVME_NS_VWC_NOT_PRESENT = 1 << 5,	/* [한국어] 휘발 캐시 없음 */
	NVME_LBAF_RP_BEST	= 0,	/* [한국어] LBAF 상대 성능 Best */
	NVME_LBAF_RP_BETTER	= 1,	/* [한국어] Better */
	NVME_LBAF_RP_GOOD	= 2,	/* [한국어] Good */
	NVME_LBAF_RP_DEGRADED	= 3,	/* [한국어] Degraded */
	NVME_NS_DPC_PI_LAST	= 1 << 4,	/* [한국어] PI 를 데이터 마지막에 둘 수 있음 */
	NVME_NS_DPC_PI_FIRST	= 1 << 3,	/* [한국어] PI 를 데이터 앞에 둘 수 있음 */
	NVME_NS_DPC_PI_TYPE3	= 1 << 2,	/* [한국어] PI Type 3 능력 */
	NVME_NS_DPC_PI_TYPE2	= 1 << 1,	/* [한국어] PI Type 2 능력 */
	NVME_NS_DPC_PI_TYPE1	= 1 << 0,	/* [한국어] PI Type 1 능력 */
	NVME_NS_DPS_PI_FIRST	= 1 << 3,	/* [한국어] 활성 설정: PI first */
	NVME_NS_DPS_PI_MASK	= 0x7,	/* [한국어] 활성 PI 타입 마스크 */
	NVME_NS_DPS_PI_TYPE1	= 1,	/* [한국어] 활성 PI Type 1 */
	NVME_NS_DPS_PI_TYPE2	= 2,	/* [한국어] 활성 PI Type 2 */
	NVME_NS_DPS_PI_TYPE3	= 3,	/* [한국어] 활성 PI Type 3 */
};

enum {
	NVME_NSTAT_NRDY		= 1 << 0,	/* [한국어] NS Not Ready — I/O 전 대기/재시도 */
};

enum {
	NVME_NVM_NS_16B_GUARD	= 0,	/* [한국어] PI Guard 16비트 */
	NVME_NVM_NS_32B_GUARD	= 1,	/* [한국어] PI Guard 32비트 */
	NVME_NVM_NS_64B_GUARD	= 2,	/* [한국어] PI Guard 64비트 */
	NVME_NVM_NS_QTYPE_GUARD	= 3,	/* [한국어] Qualified guard 타입 */
};

/*
 * [한국어] nvme_lbaf_index() — flbas 에서 활성 LBAF 인덱스 재조립
 * 하위 4비트 + 상위 확장 비트를 합쳐 lbaf[] 인덱스를 만든다.
 */
static inline __u8 nvme_lbaf_index(__u8 flbas)
{
	return (flbas & NVME_NS_FLBAS_LBA_MASK) |	/* [한국어] 하위 LBAF 인덱스 */
		((flbas & NVME_NS_FLBAS_LBA_UMASK) >> NVME_NS_FLBAS_LBA_SHIFT);	/* [한국어] 확장 비트 정렬 후 합 */
}

/* Identify Namespace Metadata Capabilities (MC): */
enum {
	NVME_MC_EXTENDED_LBA	= (1 << 0),	/* [한국어] 메타데이터가 extended LBA 에 포함 가능 */
	NVME_MC_METADATA_PTR	= (1 << 1),	/* [한국어] 별도 메타데이터 포인터 가능 */
};

/*
 * [한국어] Namespace Identification Descriptor 헤더
 * 리스트로 EUI64/NGUID/UUID/CSI 를 가변 연결. nidt+nidl 로 파싱.
 */
struct nvme_ns_id_desc {
	__u8 nidt;	/* [한국어] Namespace ID Descriptor Type — NVME_NIDT_* */
	__u8 nidl;	/* [한국어] Descriptor Length */
	__le16 reserved;	/* [한국어] 예약 0 */
};

#define NVME_NIDT_EUI64_LEN	8	/* [한국어] EUI64 디스크립터 페이로드 8B */
#define NVME_NIDT_NGUID_LEN	16	/* [한국어] NGUID 디스크립터 페이로드 16B */
#define NVME_NIDT_UUID_LEN	16	/* [한국어] UUID 디스크립터 페이로드 16B */
#define NVME_NIDT_CSI_LEN	1	/* [한국어] CSI 디스크립터 페이로드 1B */

enum {
	NVME_NIDT_EUI64		= 0x01,	/* [한국어] IEEE EUI-64 식별자 타입 */
	NVME_NIDT_NGUID		= 0x02,	/* [한국어] Namespace GUID 타입 */
	NVME_NIDT_UUID		= 0x03,	/* [한국어] UUID 타입 */
	NVME_NIDT_CSI		= 0x04,	/* [한국어] Command Set Identifier 디스크립터 */
};

/*
 * [한국어] Endurance Group 로그 페이지 — 수명·미디어 마모·용량 통계
 * 컨트롤러 작성, 관리/SMART 확장 경로가 소비.
 */
struct nvme_endurance_group_log {
	__u8	egcw;	/* [한국어] Endurance Group Critical Warning */
	__u8	egfeat;	/* [한국어] Endurance Group Features */
	__u8	rsvd2;	/* [한국어] 예약 */
	__u8	avsp;	/* [한국어] Available Spare */
	__u8	avspt;	/* [한국어] Available Spare Threshold */
	__u8	pused;	/* [한국어] Percentage Used */
	__le16	did;	/* [한국어] Domain Identifier */
	__u8	rsvd8[24];	/* [한국어] 예약 */
	__u8	ee[16];	/* [한국어] Endurance Estimate 128b */
	__u8	dur[16];	/* [한국어] Data Units Read 128b */
	__u8	duw[16];	/* [한국어] Data Units Written 128b */
	__u8	muw[16];	/* [한국어] Media Units Written 128b */
	__u8	hrc[16];	/* [한국어] Host Read Commands 128b */
	__u8	hwc[16];	/* [한국어] Host Write Commands 128b */
	__u8	mdie[16];	/* [한국어] Media and Data Integrity Errors 128b */
	__u8	neile[16];	/* [한국어] Number of Error Information Log Entries 128b */
	__u8	tegcap[16];	/* [한국어] Total Endurance Group Capacity 128b */
	__u8	uegcap[16];	/* [한국어] Unallocated Endurance Group Capacity 128b */
	__u8	rsvd192[320];	/* [한국어] 예약 패딩 */
};

/*
 * [한국어] Rotational Media 로그 — 회전 매체 스핀/로드 카운트 (HDD-like NVMe)
 */
struct nvme_rotational_media_log {
	__le16	endgid;	/* [한국어] Endurance Group ID */
	__le16	numa;	/* [한국어] Number of Actuators */
	__le16	nrs;	/* [한국어] Number of Rotational Speeds */
	__u8	rsvd6[2];	/* [한국어] 예약 */
	__le32	spinc;	/* [한국어] Spinup Count */
	__le32	fspinc;	/* [한국어] Failed Spinup Count */
	__le32	ldc;	/* [한국어] Load Count */
	__le32	fldc;	/* [한국어] Failed Load Count */
	__u8	rsvd24[488];	/* [한국어] 예약 */
};

struct nvme_fdp_config {
	__u8			flags;	/* [한국어] flags 필드 — 상위 구조 작성자·동기화 참고 */
#define FDPCFG_FDPE	(1U << 0)	/* [한국어] FDPCFG_FDPE 매크로 — 상위 섹션 계약 참고 */
	__u8			fdpcidx;	/* [한국어] fdpcidx 필드 — 상위 구조 작성자·동기화 참고 */
	__le16			reserved;	/* [한국어] 스펙 예약 — 호스트 0 */
};

struct nvme_fdp_ruh_desc {
	__u8			ruht;	/* [한국어] ruht 필드 — 상위 구조 작성자·동기화 참고 */
	__u8			reserved[3];	/* [한국어] 스펙 예약 — 호스트 0 */
};

struct nvme_fdp_config_desc {
	__le16			dsze;	/* [한국어] dsze 필드 — 상위 구조 작성자·동기화 참고 */
	__u8			fdpa;	/* [한국어] fdpa 필드 — 상위 구조 작성자·동기화 참고 */
	__u8			vss;	/* [한국어] vss 필드 — 상위 구조 작성자·동기화 참고 */
	__le32			nrg;	/* [한국어] nrg 필드 — 상위 구조 작성자·동기화 참고 */
	__le16			nruh;	/* [한국어] nruh 필드 — 상위 구조 작성자·동기화 참고 */
	__le16			maxpids;	/* [한국어] maxpids 필드 — 상위 구조 작성자·동기화 참고 */
	__le32			nns;	/* [한국어] nns 필드 — 상위 구조 작성자·동기화 참고 */
	__le64			runs;	/* [한국어] runs 필드 — 상위 구조 작성자·동기화 참고 */
	__le32			erutl;	/* [한국어] erutl 필드 — 상위 구조 작성자·동기화 참고 */
	__u8			rsvd28[36];	/* [한국어] 스펙 예약 — 호스트 0 */
	struct nvme_fdp_ruh_desc ruhs[];	/* [한국어] ruhs 필드 — 상위 구조 작성자·동기화 참고 */
};

struct nvme_fdp_config_log {
	__le16			numfdpc;	/* [한국어] numfdpc 필드 — 상위 구조 작성자·동기화 참고 */
	__u8			ver;	/* [한국어] ver 필드 — 상위 구조 작성자·동기화 참고 */
	__u8			rsvd3;	/* [한국어] 스펙 예약 — 호스트 0 */
	__le32			sze;	/* [한국어] sze 필드 — 상위 구조 작성자·동기화 참고 */
	__u8			rsvd8[8];	/* [한국어] 스펙 예약 — 호스트 0 */
	/*
	 * This is followed by variable number of nvme_fdp_config_desc
	 * structures, but sparse doesn't like nested variable sized arrays.
	 */
};

/*
 * [한국어] SMART / Health Information 로그 (LID=02h)
 * critical_warning·온도·spare·percent_used·단위 읽기/쓰기 카운터.
 * 호스트 hwmon·AER SMART 이벤트·관리 도구가 소비. 16B 카운터는 128-bit LE.
 * 작성자=컨트롤러, Get Log Page 로 호스트 수신.
 */
struct nvme_smart_log {
	__u8			critical_warning;	/* [한국어] 치명 경고 비트 — spare/temp/media 등 */
	__u8			temperature[2];	/* [한국어] 복합 온도(Kelvin) 2B LE */
	__u8			avail_spare;	/* [한국어] 가용 예비 공간 퍼센트 */
	__u8			spare_thresh;	/* [한국어] 예비 공간 임계 */
	__u8			percent_used;	/* [한국어] 수명 사용률 추정 % */
	__u8			endu_grp_crit_warn_sumry;	/* [한국어] Endurance Group 치명 경고 요약 */
	__u8			rsvd7[25];	/* [한국어] 예약 */
	__u8			data_units_read[16];	/* [한국어] 읽기 데이터 단위 카운터 128b LE */
	__u8			data_units_written[16];	/* [한국어] 쓰기 데이터 단위 카운터 128b LE */
	__u8			host_reads[16];	/* [한국어] 호스트 읽기 명령 수 128b */
	__u8			host_writes[16];	/* [한국어] 호스트 쓰기 명령 수 128b */
	__u8			ctrl_busy_time[16];	/* [한국어] 컨트롤러 바쁨 시간 128b */
	__u8			power_cycles[16];	/* [한국어] 전원 사이클 128b */
	__u8			power_on_hours[16];	/* [한국어] 전원 인가 시간 128b */
	__u8			unsafe_shutdowns[16];	/* [한국어] 비정상 종료 128b */
	__u8			media_errors[16];	/* [한국어] 미디어 오류 128b */
	__u8			num_err_log_entries[16];	/* [한국어] 에러 로그 엔트리 수 128b */
	__le32			warning_temp_time;	/* [한국어] 경고 온도 체류 시간 LE32 */
	__le32			critical_comp_time;	/* [한국어] 위험 온도 체류 시간 LE32 */
	__le16			temp_sensor[8];	/* [한국어] 개별 온도 센서 배열 */
	__le32			thm_temp1_trans_count;	/* [한국어] 열 관리 온도1 전이 횟수 */
	__le32			thm_temp2_trans_count;	/* [한국어] 열 관리 온도2 전이 횟수 */
	__le32			thm_temp1_total_time;	/* [한국어] 열 관리 온도1 총 시간 */
	__le32			thm_temp2_total_time;	/* [한국어] 열 관리 온도2 총 시간 */
	__u8			rsvd232[280];	/* [한국어] 예약 — 512B 로그 패딩 */
};

/*
 * [한국어] Firmware Slot Information 로그 — afi 활성 슬롯, frs[7] 리비전
 */
struct nvme_fw_slot_info_log {
	__u8			afi;	/* [한국어] Active Firmware Info — 활성/다음 슬롯 */
	__u8			rsvd1[7];	/* [한국어] 예약 */
	__le64			frs[7];	/* [한국어] 슬롯 1..7 펌웨어 리비전 문자열 LE64 팩 */
	__u8			rsvd64[448];	/* [한국어] 예약 */
};

enum {
	NVME_CMD_EFFECTS_CSUPP		= 1 << 0,	/* [한국어] 명령 지원 */
	NVME_CMD_EFFECTS_LBCC		= 1 << 1,	/* [한국어] Logical Block Content 변경 */
	NVME_CMD_EFFECTS_NCC		= 1 << 2,	/* [한국어] Namespace Capability 변경 */
	NVME_CMD_EFFECTS_NIC		= 1 << 3,	/* [한국어] Namespace Inventory 변경 — 재스캔 */
	NVME_CMD_EFFECTS_CCC		= 1 << 4,	/* [한국어] Controller Capability 변경 */
	NVME_CMD_EFFECTS_CSER_MASK	= GENMASK(15, 14),	/* [한국어] Command Submission and Execution Rule */
	NVME_CMD_EFFECTS_CSE_MASK	= GENMASK(18, 16),	/* [한국어] Command Scope Effects */
	NVME_CMD_EFFECTS_UUID_SEL	= 1 << 19,	/* [한국어] UUID 선택 가능 */
	NVME_CMD_EFFECTS_SCOPE_MASK	= GENMASK(31, 20),	/* [한국어] 영향 범위 마스크 */
};

/*
 * [한국어] Commands Supported and Effects 로그
 * acs[256]/iocs[256] — 제출 전 CCC/NIC 등 부작용. 패스스루·차단 정책.
 */
struct nvme_effects_log {
	__le32 acs[256];	/* [한국어] Admin Commands Supported and Effects */
	__le32 iocs[256];	/* [한국어] I/O Commands Supported and Effects */
	__u8   resv[2048];	/* [한국어] 예약 */
};

/*
 * [한국어] ANA 그룹 상태 — multipath 가 경로 선택·failover 에 사용
 * OPTIMIZED 우선, NONOPTIMIZED 차선, INACCESSIBLE/PERSISTENT_LOSS 는 경로 회피.
 */
enum nvme_ana_state {
	NVME_ANA_OPTIMIZED		= 0x01,	/* [한국어] 최적 경로 — multipath 우선 선택 */
	NVME_ANA_NONOPTIMIZED		= 0x02,	/* [한국어] 비최적 — 가능하나 성능/선호 낮음 */
	NVME_ANA_INACCESSIBLE		= 0x03,	/* [한국어] 접근 불가 — 즉시 다른 경로 */
	NVME_ANA_PERSISTENT_LOSS	= 0x04,	/* [한국어] 영구 손실 — 경로 제거 */
	NVME_ANA_CHANGE			= 0x0f,	/* [한국어] 전이 중 — 잠시 재시도 */
};

/*
 * [한국어] ANA 그룹 디스크립터 — multipath.c 핵심
 * grpid/state/chgcnt + nsids[] flex. 컨트롤러 작성 LE.
 */
struct nvme_ana_group_desc {
	__le32	grpid;	/* [한국어] ANA 그룹 ID LE32 */
	__le32	nnsids;	/* [한국어] 그룹 소속 NS 수 */
	__le64	chgcnt;	/* [한국어] 변경 카운터 — 로그 일관성 */
	__u8	state;	/* [한국어] nvme_ana_state 값 */
	__u8	rsvd17[15];	/* [한국어] 예약 */
	__le32	nsids[];	/* [한국어] NSID 배열 flex — nnsids 개 */
};

/* flag for the log specific field of the ANA log */
#define NVME_ANA_LOG_RGO	(1 << 0)	/* [한국어] ANA 로그 RGO — 그룹 전용 보고 요청 플래그 */

struct nvme_ana_rsp_hdr {
	__le64	chgcnt;	/* [한국어] 전체 변경 카운터 LE64 */
	__le16	ngrps;	/* [한국어] 후속 그룹 디스크립터 개수 */
	__le16	rsvd10[3];	/* [한국어] 예약 */
};

/*
 * [한국어] 존 디스크립터 — Zone Report 엔트리
 * zt/zs/za, zcap/zslba/wp. 파일시스템/zns 가 쓰기 가능 영역 판단.
 */
struct nvme_zone_descriptor {
	__u8		zt;	/* [한국어] Zone Type — 순차 쓰기 필수 등 */
	__u8		zs;	/* [한국어] Zone State — Empty/Open/Full/… */
	__u8		za;	/* [한국어] Zone Attributes */
	__u8		rsvd3[5];	/* [한국어] 예약 */
	__le64		zcap;	/* [한국어] Zone Capacity (LBA) LE64 */
	__le64		zslba;	/* [한국어] Zone Start LBA LE64 */
	__le64		wp;	/* [한국어] Write Pointer LBA LE64 */
	__u8		rsvd32[32];	/* [한국어] 예약 */
};

enum {
	NVME_ZONE_TYPE_SEQWRITE_REQ	= 0x2,	/* [한국어] 순차 쓰기 필수 존 타입 */
};

/*
 * [한국어] Zone Report 헤더 + entries[] — Zone Mgmt Receive 출력
 */
struct nvme_zone_report {
	__le64		nr_zones;	/* [한국어] 보고된 존 개수 LE64 */
	__u8		resv8[56];	/* [한국어] 예약 */
	struct nvme_zone_descriptor entries[];	/* [한국어] 존 디스크립터 flex 배열 */
};

enum {
	NVME_SMART_CRIT_SPARE		= 1 << 0,	/* [한국어] 예비 공간 임계 미달 */
	NVME_SMART_CRIT_TEMPERATURE	= 1 << 1,	/* [한국어] 온도 임계 초과 */
	NVME_SMART_CRIT_RELIABILITY	= 1 << 2,	/* [한국어] 신뢰성 저하 */
	NVME_SMART_CRIT_MEDIA		= 1 << 3,	/* [한국어] 미디어 오류 */
	NVME_SMART_CRIT_VOLATILE_MEMORY	= 1 << 4,	/* [한국어] 휘발 메모리 백업 실패 */
};

/*
 * [한국어] Asynchronous Event 타입/코드 — Admin AER 슬롯 완료 시 통지
 * 호스트 core 가 NS 변경·FW·ANA·Discovery 를 분기 처리. Set Features(ASYNC_EVENT)로 마스크.
 */
enum {
	NVME_AER_ERROR			= 0,	/* [한국어] 에러 클래스 비동기 이벤트 */
	NVME_AER_SMART			= 1,	/* [한국어] SMART/Health 이벤트 */
	NVME_AER_NOTICE			= 2,	/* [한국어] Notice 클래스 — NS 변경/FW/ANA/Discovery */
	NVME_AER_CSS			= 6,	/* [한국어] I/O Command Set Specific 이벤트 */
	NVME_AER_VS			= 7,	/* [한국어] Vendor Specific AER */
};

enum {
	NVME_AER_ERROR_PERSIST_INT_ERR	= 0x03,	/* [한국어] 지속 내부 오류 AER */
};

enum {
	NVME_AER_NOTICE_NS_CHANGED	= 0x00,	/* [한국어] NS 속성 변경 Notice — 재Identify */
	NVME_AER_NOTICE_FW_ACT_STARTING = 0x01,	/* [한국어] FW 활성화 시작 Notice */
	NVME_AER_NOTICE_ANA		= 0x03,	/* [한국어] ANA 변경 Notice — multipath 로그 재읽기 */
	NVME_AER_NOTICE_DISC_CHANGED	= 0xf0,	/* [한국어] Discovery 로그 변경 — fabrics 재조회 */
};

enum {
	NVME_AEN_BIT_NS_ATTR		= 8,	/* [한국어] AEN 설정 비트 위치: NS 속성 */
	NVME_AEN_BIT_FW_ACT		= 9,	/* [한국어] AEN 비트: FW 활성 */
	NVME_AEN_BIT_ANA_CHANGE		= 11,	/* [한국어] AEN 비트: ANA 변경 */
	NVME_AEN_BIT_DISC_CHANGE	= 31,	/* [한국어] AEN 비트: Discovery 변경 */
};

enum {
	NVME_AEN_CFG_NS_ATTR		= 1 << NVME_AEN_BIT_NS_ATTR,	/* [한국어] NS 속성 AEN 허용 마스크 */
	NVME_AEN_CFG_FW_ACT		= 1 << NVME_AEN_BIT_FW_ACT,	/* [한국어] FW 활성 AEN 허용 */
	NVME_AEN_CFG_ANA_CHANGE		= 1 << NVME_AEN_BIT_ANA_CHANGE,	/* [한국어] ANA 변경 AEN 허용 */
	NVME_AEN_CFG_DISC_CHANGE	= 1 << NVME_AEN_BIT_DISC_CHANGE,	/* [한국어] Discovery 변경 AEN 허용 */
};

struct nvme_lba_range_type {
	__u8			type;	/* [한국어] type 필드 — 상위 구조 작성자·동기화 참고 */
	__u8			attributes;	/* [한국어] attributes 필드 — 상위 구조 작성자·동기화 참고 */
	__u8			rsvd2[14];	/* [한국어] 스펙 예약 — 호스트 0 */
	__le64			slba;	/* [한국어] slba 필드 — 상위 구조 작성자·동기화 참고 */
	__le64			nlb;	/* [한국어] nlb 필드 — 상위 구조 작성자·동기화 참고 */
	__u8			guid[16];	/* [한국어] guid 필드 — 상위 구조 작성자·동기화 참고 */
	__u8			rsvd48[16];	/* [한국어] 스펙 예약 — 호스트 0 */
};

enum {
	NVME_LBART_TYPE_FS	= 0x01,	/* [한국어] 파일시스템 구간 힌트 */
	NVME_LBART_TYPE_RAID	= 0x02,	/* [한국어] RAID 구간 힌트 */
	NVME_LBART_TYPE_CACHE	= 0x03,	/* [한국어] 캐시 구간 힌트 */
	NVME_LBART_TYPE_SWAP	= 0x04,	/* [한국어] 스왑 구간 힌트 */

	NVME_LBART_ATTRIB_TEMP	= 1 << 0,	/* [한국어] 임시 구간 속성 */
	NVME_LBART_ATTRIB_HIDE	= 1 << 1,	/* [한국어] 숨김 구간 속성 */
};

/*
 * [한국어] Persistent Reservation 타입 — SCSI PR 유사 배타/등록자 only 모델
 * pr.c 가 블록 pr_ops 를 이 값으로 변환.
 */
enum nvme_pr_type {
	NVME_PR_WRITE_EXCLUSIVE			= 1,	/* [한국어] 쓰기 배타 */
	NVME_PR_EXCLUSIVE_ACCESS		= 2,	/* [한국어] 완전 배타 접근 */
	NVME_PR_WRITE_EXCLUSIVE_REG_ONLY	= 3,	/* [한국어] 쓰기 배타(등록자만) */
	NVME_PR_EXCLUSIVE_ACCESS_REG_ONLY	= 4,	/* [한국어] 완전 배타(등록자만) */
	NVME_PR_WRITE_EXCLUSIVE_ALL_REGS	= 5,	/* [한국어] 쓰기 배타(전체 등록자) */
	NVME_PR_EXCLUSIVE_ACCESS_ALL_REGS	= 6,	/* [한국어] 완전 배타(전체 등록자) */
};

enum nvme_eds {
	NVME_EXTENDED_DATA_STRUCT	= 0x1,	/* [한국어] Reservation Report 확장 형식 */
};

struct nvme_registered_ctrl {
	__le16	cntlid;	/* [한국어] cntlid 필드 — 상위 구조 작성자·동기화 참고 */
	__u8	rcsts;	/* [한국어] rcsts 필드 — 상위 구조 작성자·동기화 참고 */
	__u8	rsvd3[5];	/* [한국어] 스펙 예약 — 호스트 0 */
	__le64	hostid;	/* [한국어] hostid 필드 — 상위 구조 작성자·동기화 참고 */
	__le64	rkey;	/* [한국어] rkey 필드 — 상위 구조 작성자·동기화 참고 */
};

struct nvme_reservation_status {
	__le32	gen;	/* [한국어] gen 필드 — 상위 구조 작성자·동기화 참고 */
	__u8	rtype;	/* [한국어] rtype 필드 — 상위 구조 작성자·동기화 참고 */
	__u8	regctl[2];	/* [한국어] regctl 필드 — 상위 구조 작성자·동기화 참고 */
	__u8	resv5[2];	/* [한국어] 스펙 예약 — 호스트 0 */
	__u8	ptpls;	/* [한국어] ptpls 필드 — 상위 구조 작성자·동기화 참고 */
	__u8	resv10[14];	/* [한국어] 스펙 예약 — 호스트 0 */
	struct nvme_registered_ctrl regctl_ds[];	/* [한국어] regctl_ds 필드 — 상위 구조 작성자·동기화 참고 */
};

struct nvme_registered_ctrl_ext {
	__le16	cntlid;	/* [한국어] cntlid 필드 — 상위 구조 작성자·동기화 참고 */
	__u8	rcsts;	/* [한국어] rcsts 필드 — 상위 구조 작성자·동기화 참고 */
	__u8	rsvd3[5];	/* [한국어] 스펙 예약 — 호스트 0 */
	__le64	rkey;	/* [한국어] rkey 필드 — 상위 구조 작성자·동기화 참고 */
	__u8	hostid[16];	/* [한국어] hostid 필드 — 상위 구조 작성자·동기화 참고 */
	__u8	rsvd32[32];	/* [한국어] 스펙 예약 — 호스트 0 */
};

struct nvme_reservation_status_ext {
	__le32	gen;	/* [한국어] gen 필드 — 상위 구조 작성자·동기화 참고 */
	__u8	rtype;	/* [한국어] rtype 필드 — 상위 구조 작성자·동기화 참고 */
	__u8	regctl[2];	/* [한국어] regctl 필드 — 상위 구조 작성자·동기화 참고 */
	__u8	resv5[2];	/* [한국어] 스펙 예약 — 호스트 0 */
	__u8	ptpls;	/* [한국어] ptpls 필드 — 상위 구조 작성자·동기화 참고 */
	__u8	resv10[14];	/* [한국어] 스펙 예약 — 호스트 0 */
	__u8	rsvd24[40];	/* [한국어] 스펙 예약 — 호스트 0 */
	struct nvme_registered_ctrl_ext regctl_eds[];	/* [한국어] regctl_eds 필드 — 상위 구조 작성자·동기화 참고 */
};

/*
 * [한국어] NVM I/O Command Set opcode (SQE byte0, qid>0)
 * 데이터 평면 명령. 홀수 opcode 는 쓰기 계열 관례(nvme_is_write 가 LSB —
 * Fabrics 는 예외). 호스트 blk-mq 가 bio 를 이 코드로 변환. 0x80+ 벤더 고유.
 */
/* I/O commands */

enum nvme_opcode {
	nvme_cmd_flush		= 0x00,	/* [한국어] 0x00 Flush — 휘발 캐시→미디어; 내구성 장벽 */
	nvme_cmd_write		= 0x01,	/* [한국어] 0x01 Write — 호스트→미디어 핫패스 */
	nvme_cmd_read		= 0x02,	/* [한국어] 0x02 Read — 미디어→호스트 핫패스 */
	nvme_cmd_write_uncor	= 0x04,	/* [한국어] 0x04 Write Uncorrectable — LBA 의도적 오류 표시 */
	nvme_cmd_compare	= 0x05,	/* [한국어] 0x05 Compare — 버퍼와 미디어 비교 */
	nvme_cmd_write_zeroes	= 0x08,	/* [한국어] 0x08 Write Zeroes — 범위 0/deallocate */
	nvme_cmd_dsm		= 0x09,	/* [한국어] 0x09 Dataset Management — discard/힌트 */
	nvme_cmd_verify		= 0x0c,	/* [한국어] 0x0c Verify — 전송 없이 무결성 검증 */
	nvme_cmd_resv_register	= 0x0d,	/* [한국어] 0x0d Reservation Register — pr.c */
	nvme_cmd_resv_report	= 0x0e,	/* [한국어] 0x0e Reservation Report */
	nvme_cmd_resv_acquire	= 0x11,	/* [한국어] 0x11 Reservation Acquire/Preempt */
	nvme_cmd_io_mgmt_recv	= 0x12,	/* [한국어] 0x12 I/O Mgmt Receive — FDP 등 */
	nvme_cmd_resv_release	= 0x15,	/* [한국어] 0x15 Reservation Release */
	nvme_cmd_zone_mgmt_send	= 0x79,	/* [한국어] 0x79 Zone Mgmt Send — 존 상태 전이 */
	nvme_cmd_zone_mgmt_recv	= 0x7a,	/* [한국어] 0x7a Zone Mgmt Receive — zone report */
	nvme_cmd_zone_append	= 0x7d,	/* [한국어] 0x7d Zone Append — 존 끝 자동 오프셋 쓰기 */
	nvme_cmd_vendor_start	= 0x80,	/* [한국어] 0x80+ 벤더 고유 I/O opcode 시작 */
};

#define nvme_opcode_name(opcode)	{ opcode, #opcode }	/* [한국어] nvme_opcode_name 매크로 — 상위 섹션 계약 참고 */
#define show_nvm_opcode_name(val)				\
	__print_symbolic(val,					\
		nvme_opcode_name(nvme_cmd_flush),		\
		nvme_opcode_name(nvme_cmd_write),		\
		nvme_opcode_name(nvme_cmd_read),		\
		nvme_opcode_name(nvme_cmd_write_uncor),		\
		nvme_opcode_name(nvme_cmd_compare),		\
		nvme_opcode_name(nvme_cmd_write_zeroes),	\
		nvme_opcode_name(nvme_cmd_dsm),			\
		nvme_opcode_name(nvme_cmd_verify),		\
		nvme_opcode_name(nvme_cmd_resv_register),	\
		nvme_opcode_name(nvme_cmd_resv_report),		\
		nvme_opcode_name(nvme_cmd_resv_acquire),	\
		nvme_opcode_name(nvme_cmd_io_mgmt_recv),	\
		nvme_opcode_name(nvme_cmd_resv_release),	\
		nvme_opcode_name(nvme_cmd_zone_mgmt_send),	\
		nvme_opcode_name(nvme_cmd_zone_mgmt_recv),	\
		nvme_opcode_name(nvme_cmd_zone_append))	/* [한국어] 인자/선언 연속행 */



/*
 * [한국어] SGL 디스크립터 subtype/type 및 PRP 대 SGL 선택
 *
 * PRP 는 PCIe 호스트 기본(물리 페이지 리스트). SGL 은 주소+길이 디스크립터
 * 체인으로 Fabrics 와 일부 PCIe 컨트롤러가 사용. PSDT 비트(명령 flags 상위)가
 * PRP vs SGL 선택. ADDRESS=절대, OFFSET=캡슐 내 상대, TRANSPORT/INVALIDATE=전송 특화.
 * type 상위 니블=디스크립터 종류, 하위=subtype. 호스트가 작성, 컨트롤러가 소비.
 */
/*
 * Descriptor subtype - lower 4 bits of nvme_(keyed_)sgl_desc identifier
 *
 * @NVME_SGL_FMT_ADDRESS:     absolute address of the data block
 * @NVME_SGL_FMT_OFFSET:      relative offset of the in-capsule data block
 * @NVME_SGL_FMT_TRANSPORT_A: transport defined format, value 0xA
 * @NVME_SGL_FMT_INVALIDATE:  RDMA transport specific remote invalidation
 *                            request subtype
 */
enum {
	NVME_SGL_FMT_ADDRESS		= 0x00,	/* [한국어] 절대 데이터 주소 subtype */
	NVME_SGL_FMT_OFFSET		= 0x01,	/* [한국어] 캡슐 내 상대 오프셋 subtype */
	NVME_SGL_FMT_TRANSPORT_A	= 0x0A,	/* [한국어] 전송 정의 포맷 0xA */
	NVME_SGL_FMT_INVALIDATE		= 0x0f,	/* [한국어] RDMA remote invalidate 요청 subtype */
};

/*
 * Descriptor type - upper 4 bits of nvme_(keyed_)sgl_desc identifier
 *
 * For struct nvme_sgl_desc:
 *   @NVME_SGL_FMT_DATA_DESC:		data block descriptor
 *   @NVME_SGL_FMT_SEG_DESC:		sgl segment descriptor
 *   @NVME_SGL_FMT_LAST_SEG_DESC:	last sgl segment descriptor
 *
 * For struct nvme_keyed_sgl_desc:
 *   @NVME_KEY_SGL_FMT_DATA_DESC:	keyed data block descriptor
 *
 * Transport-specific SGL types:
 *   @NVME_TRANSPORT_SGL_DATA_DESC:	Transport SGL data dlock descriptor
 */
enum {
	NVME_SGL_FMT_DATA_DESC		= 0x00,	/* [한국어] 데이터 블록 디스크립터 type */
	NVME_SGL_FMT_SEG_DESC		= 0x02,	/* [한국어] SGL 세그먼트(다음 리스트) type */
	NVME_SGL_FMT_LAST_SEG_DESC	= 0x03,	/* [한국어] 마지막 세그먼트 type */
	NVME_KEY_SGL_FMT_DATA_DESC	= 0x04,	/* [한국어] Keyed 데이터 디스크립터 type */
	NVME_TRANSPORT_SGL_DATA_DESC	= 0x05,	/* [한국어] 전송 특화 데이터 디스크립터 */
};

/*
 * [한국어] SGL Data Block/Segment 디스크립터 16B
 * addr+length+type. type 상위=디스크립터 종류, 하위=subtype. LE. 호스트 작성.
 */
struct nvme_sgl_desc {
	__le64	addr;	/* [한국어] 데이터/세그먼트 물리·IOVA 주소 LE64 */
	__le32	length;	/* [한국어] 바이트 길이 LE32 */
	__u8	rsvd[3];	/* [한국어] 예약 0 */
	__u8	type;	/* [한국어] identifier — 상위 type | 하위 subtype */
};

/*
 * [한국어] Keyed SGL — RDMA rkey 포함
 * length 24b + key 32b + type. 원격 메모리 등록 키로 전송.
 */
struct nvme_keyed_sgl_desc {
	__le64	addr;	/* [한국어] 원격/로컬 데이터 주소 LE64 */
	__u8	length[3];	/* [한국어] 24-bit 길이 (리틀엔디안 3바이트) */
	__u8	key[4];	/* [한국어] RDMA rkey 등 4바이트 키 */
	__u8	type;	/* [한국어] KEY_SGL_FMT_DATA_DESC 등 */
};

/*
 * [한국어] SQE 데이터 포인터 유니온 (16B) — PRP 대 SGL
 * PSDT=00: prp1/prp2 (PCIe 기본). SGL 모드: sgl 또는 ksgl.
 * 호스트 매핑 계층이 채움. 컨트롤러가 읽어 데이터 이동. LE 주소.
 */
union nvme_data_ptr {
	struct {	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
		__le64	prp1;	/* [한국어] PRP1 — 첫 데이터 페이지 또는 버퍼 물리 주소 */
		__le64	prp2;	/* [한국어] PRP2 — 다음 페이지 또는 PRP 리스트 포인터 */
	};
	struct nvme_sgl_desc	sgl;	/* [한국어] 일반 SGL 디스크립터 뷰 */
	struct nvme_keyed_sgl_desc ksgl;	/* [한국어] Keyed SGL 뷰 (RDMA 등) */
};

/*
 * Lowest two bits of our flags field (FUSE field in the spec):
 *
 * @NVME_CMD_FUSE_FIRST:   Fused Operation, first command
 * @NVME_CMD_FUSE_SECOND:  Fused Operation, second command
 *
 * Highest two bits in our flags field (PSDT field in the spec):
 *
 * @NVME_CMD_PSDT_SGL_METABUF:	Use SGLS for this transfer,
 *	If used, MPTR contains addr of single physical buffer (byte aligned).
 * @NVME_CMD_PSDT_SGL_METASEG:	Use SGLS for this transfer,
 *	If used, MPTR contains an address of an SGL segment containing
 *	exactly 1 SGL descriptor (qword aligned).
 */
/*
 * [한국어] SQE flags: 하위 FUSE, 상위 PSDT(SGL vs PRP)
 * PSDT=00 → PRP, 01/10 → SGL+메타 포인터 해석이 달라짐. SGL_ALL 마스크로 판별.
 */
enum {
	NVME_CMD_FUSE_FIRST	= (1 << 0),	/* [한국어] 퓨즈 연산 첫 명령 — 다음 SECOND 와 원자 쌍 */
	NVME_CMD_FUSE_SECOND	= (1 << 1),	/* [한국어] 퓨즈 연산 둘째 명령 */

	NVME_CMD_SGL_METABUF	= (1 << 6),	/* [한국어] PSDT: SGL 사용, MPTR=단일 메타 버퍼 */
	NVME_CMD_SGL_METASEG	= (1 << 7),	/* [한국어] PSDT: SGL 사용, MPTR=메타 SGL 세그먼트 */
	NVME_CMD_SGL_ALL	= NVME_CMD_SGL_METABUF | NVME_CMD_SGL_METASEG,	/* [한국어] SGL PSDT 마스크 — 0 이면 PRP */
};

/*
 * [한국어] 공통 64B SQE 레이아웃
 * 모든 명령의 골격: opcode/flags/cid/nsid | cdw2-3 | metadata | dptr | cdw10-15.
 * 호스트가 작성, 컨트롤러가 소비. 구체 명령은 동일 메모리를 다른 struct 로 오버레이.
 */
struct nvme_common_command {
	__u8			opcode;	/* [한국어] SQE byte0 명령 코드 — qid·0x7f 로 네임스페이스 구분 (호스트) */
	__u8			flags;	/* [한국어] FUSE·PSDT 등 SQE flags (호스트) */
	__u16			command_id;	/* [한국어] 호스트 태그 CID — CQE 가 그대로 반사; blk-mq 상관 */
	__le32			nsid;	/* [한국어] Namespace ID LE32 — 0=비NS, 0xffffffff=전체 */
	__le32			cdw2[2];	/* [한국어] SQE DW2-3 — 명령별/예약 */
	__le64			metadata;	/* [한국어] MPTR — 메타데이터 버퍼 주소 (호스트) */
	union nvme_data_ptr	dptr;	/* [한국어] 데이터 포인터 PRP 또는 SGL (호스트) */
	struct_group(cdws,	/* [한국어] CDW10-15 그룹 — 일괄 접근용 */
	__le32			cdw10;	/* [한국어] 명령 dword10 LE32 (호스트) */
	__le32			cdw11;	/* [한국어] 명령 dword11 */
	__le32			cdw12;	/* [한국어] 명령 dword12 */
	__le32			cdw13;	/* [한국어] 명령 dword13 */
	__le32			cdw14;	/* [한국어] 명령 dword14 */
	__le32			cdw15;	/* [한국어] 명령 dword15 */
	);
};

/*
 * [한국어] Read/Write/Compare/Verify 등 LBA I/O SQE
 * slba+length+control(FUA/LR/PI)+dsmgmt+PI 태그. 핫패스 최다 사용 포맷.
 * 작성자=호스트, 64B 고정.
 */
struct nvme_rw_command {
	__u8			opcode;	/* [한국어] read/write/compare/verify 등 I/O opcode */
	__u8			flags;	/* [한국어] FUSE/PSDT */
	__u16			command_id;	/* [한국어] CID — 완료 매칭 */
	__le32			nsid;	/* [한국어] 대상 NSID LE32 */
	__le32			cdw2;	/* [한국어] 예약/확장 DW2 */
	__le32			cdw3;	/* [한국어] 예약/확장 DW3 */
	__le64			metadata;	/* [한국어] PI/메타 버퍼 포인터 */
	union nvme_data_ptr	dptr;	/* [한국어] 페이로드 PRP/SGL */
	__le64			slba;	/* [한국어] 시작 LBA LE64 */
	__le16			length;	/* [한국어] LBA 개수 - 1 LE16 */
	__le16			control;	/* [한국어] FUA/LR/PRINFO 등 제어 비트 */
	__le32			dsmgmt;	/* [한국어] 접근 빈도·지연 힌트 / directive 타입 */
	__le32			reftag;	/* [한국어] PI Reference Tag */
	__le16			lbat;	/* [한국어] PI Application Tag */
	__le16			lbatm;	/* [한국어] PI Application Tag Mask */
};

/*
 * [한국어] RW control/dsmgmt 비트 — FUA/LR/PRINFO/접근 힌트
 * 호스트가 캐시 우회·PI 검사·프리페치 힌트를 SQE 에 실어 보낸다.
 */
enum {
	NVME_RW_LR			= 1 << 15,	/* [한국어] Limited Retry — 컨트롤러 재시도 제한 */
	NVME_RW_FUA			= 1 << 14,	/* [한국어] Force Unit Access — 캐시 우회 내구성 */
	NVME_RW_APPEND_PIREMAP		= 1 << 9,	/* [한국어] Zone Append 시 PI remap */
	NVME_RW_DSM_FREQ_UNSPEC		= 0,	/* [한국어] 접근 빈도 미지정 */
	NVME_RW_DSM_FREQ_TYPICAL	= 1,	/* [한국어] 전형적 빈도 */
	NVME_RW_DSM_FREQ_RARE		= 2,	/* [한국어] 드묾 */
	NVME_RW_DSM_FREQ_READS		= 3,	/* [한국어] 읽기 위주 */
	NVME_RW_DSM_FREQ_WRITES		= 4,	/* [한국어] 쓰기 위주 */
	NVME_RW_DSM_FREQ_RW		= 5,	/* [한국어] 읽기/쓰기 혼합 */
	NVME_RW_DSM_FREQ_ONCE		= 6,	/* [한국어] 일회성 */
	NVME_RW_DSM_FREQ_PREFETCH	= 7,	/* [한국어] 프리페치 */
	NVME_RW_DSM_FREQ_TEMP		= 8,	/* [한국어] 임시 데이터 */
	NVME_RW_DSM_LATENCY_NONE	= 0 << 4,	/* [한국어] 지연 힌트 없음 */
	NVME_RW_DSM_LATENCY_IDLE	= 1 << 4,	/* [한국어] 유휴 허용 지연 */
	NVME_RW_DSM_LATENCY_NORM	= 2 << 4,	/* [한국어] 보통 지연 */
	NVME_RW_DSM_LATENCY_LOW		= 3 << 4,	/* [한국어] 저지연 */
	NVME_RW_DSM_SEQ_REQ		= 1 << 6,	/* [한국어] 순차 접근 힌트 */
	NVME_RW_DSM_COMPRESSED		= 1 << 7,	/* [한국어] 압축 데이터 힌트 */
	NVME_RW_PRINFO_PRCHK_REF	= 1 << 10,	/* [한국어] PI Reference Tag 검사 */
	NVME_RW_PRINFO_PRCHK_APP	= 1 << 11,	/* [한국어] PI App Tag 검사 */
	NVME_RW_PRINFO_PRCHK_GUARD	= 1 << 12,	/* [한국어] PI Guard 검사 */
	NVME_RW_PRINFO_PRACT		= 1 << 13,	/* [한국어] PI 동작(PRACT) — 컨트롤러 생성/검사 */
	NVME_RW_DTYPE_STREAMS		= 1 << 4,	/* [한국어] Directive 타입 Streams */
	NVME_RW_DTYPE_DPLCMT		= 2 << 4,	/* [한국어] Directive 타입 Data Placement */
	NVME_WZ_DEAC			= 1 << 9,	/* [한국어] Write Zeroes DEAC — 범위 deallocate 힌트 */
};

/*
 * [한국어] Dataset Management SQE — nr=범위수-1, attributes=AD/IDR/IDW
 * dptr → nvme_dsm_range 배열. 파일시스템 discard 핫 경로. 호스트 작성.
 */
struct nvme_dsm_cmd {
	__u8			opcode;	/* [한국어] nvme_cmd_dsm */
	__u8			flags;	/* [한국어] FUSE/PSDT */
	__u16			command_id;	/* [한국어] CID */
	__le32			nsid;	/* [한국어] 대상 NSID */
	__u64			rsvd2[2];	/* [한국어] 예약 */
	union nvme_data_ptr	dptr;	/* [한국어] DSM 범위 배열 버퍼 */
	__le32			nr;	/* [한국어] 범위 수 - 1 */
	__le32			attributes;	/* [한국어] AD/IDR/IDW 비트 */
	__u32			rsvd12[4];	/* [한국어] 예약 */
};

enum {
	NVME_DSMGMT_IDR		= 1 << 0,	/* [한국어] Integral Dataset for Read 힌트 */
	NVME_DSMGMT_IDW		= 1 << 1,	/* [한국어] Integral Dataset for Write 힌트 */
	NVME_DSMGMT_AD		= 1 << 2,	/* [한국어] Attribute Deallocate — discard */
};

#define NVME_DSM_MAX_RANGES	256	/* [한국어] 단일 DSM 명령 최대 범위 수 */

/*
 * [한국어] DSM 범위 디스크립터 — slba+nlb+cattr. 최대 NVME_DSM_MAX_RANGES 개
 */
struct nvme_dsm_range {
	__le32			cattr;	/* [한국어] 범위 속성 */
	__le32			nlb;	/* [한국어] 논리 블록 수 */
	__le64			slba;	/* [한국어] 시작 LBA LE64 */
};

/*
 * [한국어] Write Zeroes SQE — 범위 0 채우기; control.DEAC 로 deallocate
 */
struct nvme_write_zeroes_cmd {
	__u8			opcode;	/* [한국어] nvme_cmd_write_zeroes */
	__u8			flags;	/* [한국어] FUSE/PSDT */
	__u16			command_id;	/* [한국어] CID */
	__le32			nsid;	/* [한국어] 대상 NSID */
	__u64			rsvd2;	/* [한국어] 예약 */
	__le64			metadata;	/* [한국어] 메타 포인터(보통 미사용) */
	union nvme_data_ptr	dptr;	/* [한국어] 데이터 포인터(보통 미사용) */
	__le64			slba;	/* [한국어] 시작 LBA */
	__le16			length;	/* [한국어] LBA 개수 - 1 */
	__le16			control;	/* [한국어] DEAC 등 제어 비트 */
	__le32			dsmgmt;	/* [한국어] DSM 힌트 */
	__le32			reftag;	/* [한국어] PI Ref Tag */
	__le16			lbat;	/* [한국어] PI App Tag */
	__le16			lbatm;	/* [한국어] PI App Tag Mask */
};

/*
 * [한국어] Zone Send Action — 존 상태 머신 전이 명령 코드
 */
enum nvme_zone_mgmt_action {
	NVME_ZONE_CLOSE		= 0x1,	/* [한국어] 존 Close */
	NVME_ZONE_FINISH	= 0x2,	/* [한국어] 존 Finish → Full */
	NVME_ZONE_OPEN		= 0x3,	/* [한국어] 존 Explicit Open */
	NVME_ZONE_RESET		= 0x4,	/* [한국어] 존 Reset → Empty */
	NVME_ZONE_OFFLINE	= 0x5,	/* [한국어] 존 Offline */
	NVME_ZONE_SET_DESC_EXT	= 0x10,	/* [한국어] 존 디스크립터 확장 설정 */
};

/*
 * [한국어] Zone Management Send SQE — zsa=close/finish/open/reset/offline
 */
struct nvme_zone_mgmt_send_cmd {
	__u8			opcode;	/* [한국어] nvme_cmd_zone_mgmt_send */
	__u8			flags;	/* [한국어] FUSE/PSDT */
	__u16			command_id;	/* [한국어] CID */
	__le32			nsid;	/* [한국어] 대상 NSID */
	__le32			cdw2[2];	/* [한국어] 예약/확장 */
	__le64			metadata;	/* [한국어] 메타 포인터 */
	union nvme_data_ptr	dptr;	/* [한국어] 선택적 데이터 */
	__le64			slba;	/* [한국어] 대상 존 식별 LBA */
	__le32			cdw12;	/* [한국어] 추가 제어 */
	__u8			zsa;	/* [한국어] Zone Send Action — nvme_zone_mgmt_action */
	__u8			select_all;	/* [한국어] 전체 존 선택 */
	__u8			rsvd13[2];	/* [한국어] 예약 */
	__le32			cdw14[2];	/* [한국어] 예약 */
};

/*
 * [한국어] Zone Management Receive SQE — zra=report, zrasf=상태 필터
 */
struct nvme_zone_mgmt_recv_cmd {
	__u8			opcode;	/* [한국어] nvme_cmd_zone_mgmt_recv */
	__u8			flags;	/* [한국어] FUSE/PSDT */
	__u16			command_id;	/* [한국어] CID */
	__le32			nsid;	/* [한국어] 대상 NSID */
	__le64			rsvd2[2];	/* [한국어] 예약 */
	union nvme_data_ptr	dptr;	/* [한국어] zone_report 수신 버퍼 */
	__le64			slba;	/* [한국어] 보고 시작 LBA */
	__le32			numd;	/* [한국어] Number of Dwords */
	__u8			zra;	/* [한국어] Zone Receive Action */
	__u8			zrasf;	/* [한국어] 상태 필터 ZRASF_* */
	__u8			pr;	/* [한국어] Partial Report 비트 */
	__u8			rsvd13;	/* [한국어] 예약 */
	__le32			cdw14[2];	/* [한국어] 예약 */
};

struct nvme_io_mgmt_recv_cmd {
	__u8			opcode;	/* [한국어] opcode 필드 — 상위 구조 작성자·동기화 참고 */
	__u8			flags;	/* [한국어] flags 필드 — 상위 구조 작성자·동기화 참고 */
	__u16			command_id;	/* [한국어] command_id 필드 — 상위 구조 작성자·동기화 참고 */
	__le32			nsid;	/* [한국어] nsid 필드 — 상위 구조 작성자·동기화 참고 */
	__le64			rsvd2[2];	/* [한국어] 스펙 예약 — 호스트 0 */
	union nvme_data_ptr	dptr;	/* [한국어] dptr 필드 — 상위 구조 작성자·동기화 참고 */
	__u8			mo;	/* [한국어] mo 필드 — 상위 구조 작성자·동기화 참고 */
	__u8			rsvd11;	/* [한국어] 스펙 예약 — 호스트 0 */
	__u16			mos;	/* [한국어] mos 필드 — 상위 구조 작성자·동기화 참고 */
	__le32			numd;	/* [한국어] numd 필드 — 상위 구조 작성자·동기화 참고 */
	__le32			cdw12[4];	/* [한국어] cdw12 필드 — 상위 구조 작성자·동기화 참고 */
};

enum {
	NVME_IO_MGMT_RECV_MO_RUHS	= 1,	/* [한국어] NVME_IO_MGMT_RECV_MO_RUHS 상수 — 상위 enum 역할 참고 */
};

struct nvme_fdp_ruh_status_desc {
	__le16			pid;	/* [한국어] pid 필드 — 상위 구조 작성자·동기화 참고 */
	__le16			ruhid;	/* [한국어] ruhid 필드 — 상위 구조 작성자·동기화 참고 */
	__le32			earutr;	/* [한국어] earutr 필드 — 상위 구조 작성자·동기화 참고 */
	__le64			ruamw;	/* [한국어] ruamw 필드 — 상위 구조 작성자·동기화 참고 */
	__u8			reserved[16];	/* [한국어] 스펙 예약 — 호스트 0 */
};

struct nvme_fdp_ruh_status {
	__u8			rsvd0[14];	/* [한국어] 스펙 예약 — 호스트 0 */
	__le16			nruhsd;	/* [한국어] nruhsd 필드 — 상위 구조 작성자·동기화 참고 */
	struct nvme_fdp_ruh_status_desc ruhsd[];	/* [한국어] ruhsd 필드 — 상위 구조 작성자·동기화 참고 */
};

/*
 * [한국어] Zone Receive Action / 상태 필터 — Zone Report 요청 시 사용
 */
enum {
	NVME_ZRA_ZONE_REPORT		= 0,	/* [한국어] Zone Report 수신 동작 */
	NVME_ZRASF_ZONE_REPORT_ALL	= 0,	/* [한국어] 모든 상태 존 보고 */
	NVME_ZRASF_ZONE_STATE_EMPTY	= 0x01,	/* [한국어] Empty 상태 필터 */
	NVME_ZRASF_ZONE_STATE_IMP_OPEN	= 0x02,	/* [한국어] Implicit Open 필터 */
	NVME_ZRASF_ZONE_STATE_EXP_OPEN	= 0x03,	/* [한국어] Explicit Open 필터 */
	NVME_ZRASF_ZONE_STATE_CLOSED	= 0x04,	/* [한국어] Closed 필터 */
	NVME_ZRASF_ZONE_STATE_READONLY	= 0x05,	/* [한국어] Read Only 필터 */
	NVME_ZRASF_ZONE_STATE_FULL	= 0x06,	/* [한국어] Full 필터 */
	NVME_ZRASF_ZONE_STATE_OFFLINE	= 0x07,	/* [한국어] Offline 필터 */
	NVME_REPORT_ZONE_PARTIAL	= 1,	/* [한국어] Partial report 비트 */
};

/* Features */

enum {
	NVME_TEMP_THRESH_MASK		= 0xffff,	/* [한국어] NVME_TEMP_THRESH_MASK 상수 — 상위 enum 역할 참고 */
	NVME_TEMP_THRESH_SELECT_SHIFT	= 16,	/* [한국어] NVME_TEMP_THRESH_SELECT_SHIFT 상수 — 상위 enum 역할 참고 */
	NVME_TEMP_THRESH_TYPE_UNDER	= 0x100000,	/* [한국어] NVME_TEMP_THRESH_TYPE_UNDER 상수 — 상위 enum 역할 참고 */
};

struct nvme_feat_auto_pst {
	__le64 entries[32];	/* [한국어] entries 필드 — 상위 구조 작성자·동기화 참고 */
};

enum {
	NVME_HOST_MEM_ENABLE	= (1 << 0),	/* [한국어] NVME_HOST_MEM_ENABLE 상수 — 상위 enum 역할 참고 */
	NVME_HOST_MEM_RETURN	= (1 << 1),	/* [한국어] NVME_HOST_MEM_RETURN 상수 — 상위 enum 역할 참고 */
};

struct nvme_feat_host_behavior {
	__u8 acre;	/* [한국어] acre 필드 — 상위 구조 작성자·동기화 참고 */
	__u8 etdas;	/* [한국어] etdas 필드 — 상위 구조 작성자·동기화 참고 */
	__u8 lbafee;	/* [한국어] lbafee 필드 — 상위 구조 작성자·동기화 참고 */
	__u8 resv1[509];	/* [한국어] 스펙 예약 — 호스트 0 */
};

enum {
	NVME_ENABLE_ACRE	= 1,	/* [한국어] NVME_ENABLE_ACRE 상수 — 상위 enum 역할 참고 */
	NVME_ENABLE_LBAFEE	= 1,	/* [한국어] NVME_ENABLE_LBAFEE 상수 — 상위 enum 역할 참고 */
};

/*
 * [한국어] Admin Command Set opcode (SQE byte0, Admin SQ qid=0)
 * 제어 평면: 큐 생명주기, Identify, Feature, 로그, FW, Security, Sanitize.
 * 초기화·ioctl 패스스루·keep-alive·AER 가 주요 생산자. I/O opcode 와 숫자 공간이
 * 겹칠 수 있어 반드시 qid 로 해석 네임스페이스를 분리한다.
 */
/* Admin commands */

enum nvme_admin_opcode {
	nvme_admin_delete_sq		= 0x00,	/* [한국어] 0x00 Delete SQ */
	nvme_admin_create_sq		= 0x01,	/* [한국어] 0x01 Create SQ — 대상 CQ 선행 필요 */
	nvme_admin_get_log_page		= 0x02,	/* [한국어] 0x02 Get Log Page — SMART/ANA/에러/Discovery */
	nvme_admin_delete_cq		= 0x04,	/* [한국어] 0x04 Delete CQ */
	nvme_admin_create_cq		= 0x05,	/* [한국어] 0x05 Create CQ + IRQ 벡터 */
	nvme_admin_identify		= 0x06,	/* [한국어] 0x06 Identify — 프로브 핵심 */
	nvme_admin_abort_cmd		= 0x08,	/* [한국어] 0x08 Abort */
	nvme_admin_set_features		= 0x09,	/* [한국어] 0x09 Set Features — 큐수/IRQ/KATO 등 */
	nvme_admin_get_features		= 0x0a,	/* [한국어] 0x0a Get Features */
	nvme_admin_async_event		= 0x0c,	/* [한국어] 0x0c Async Event 요청 슬롯 */
	nvme_admin_ns_mgmt		= 0x0d,	/* [한국어] 0x0d Namespace Management */
	nvme_admin_activate_fw		= 0x10,	/* [한국어] 0x10 Firmware Commit/Activate */
	nvme_admin_download_fw		= 0x11,	/* [한국어] 0x11 Firmware Image Download */
	nvme_admin_dev_self_test	= 0x14,	/* [한국어] 0x14 Device Self-test */
	nvme_admin_ns_attach		= 0x15,	/* [한국어] 0x15 Namespace Attach */
	nvme_admin_keep_alive		= 0x18,	/* [한국어] 0x18 Keep Alive — fabrics 생존 */
	nvme_admin_directive_send	= 0x19,	/* [한국어] 0x19 Directive Send */
	nvme_admin_directive_recv	= 0x1a,	/* [한국어] 0x1a Directive Receive */
	nvme_admin_virtual_mgmt		= 0x1c,	/* [한국어] 0x1c Virtualization Management */
	nvme_admin_nvme_mi_send		= 0x1d,	/* [한국어] 0x1d NVMe-MI Send */
	nvme_admin_nvme_mi_recv		= 0x1e,	/* [한국어] 0x1e NVMe-MI Receive */
	nvme_admin_dbbuf		= 0x7C,	/* [한국어] 0x7C Doorbell Buffer Config */
	nvme_admin_format_nvm		= 0x80,	/* [한국어] 0x80 Format NVM */
	nvme_admin_security_send	= 0x81,	/* [한국어] 0x81 Security Send — Opal 등 */
	nvme_admin_security_recv	= 0x82,	/* [한국어] 0x82 Security Receive */
	nvme_admin_sanitize_nvm		= 0x84,	/* [한국어] 0x84 Sanitize */
	nvme_admin_get_lba_status	= 0x86,	/* [한국어] 0x86 Get LBA Status */
	nvme_admin_vendor_start		= 0xC0,	/* [한국어] 0xC0+ 벤더 Admin 시작 */
};

#define nvme_admin_opcode_name(opcode)	{ opcode, #opcode }	/* [한국어] nvme_admin_opcode_name 매크로 — 상위 섹션 계약 참고 */
#define show_admin_opcode_name(val)					\
	__print_symbolic(val,						\
		nvme_admin_opcode_name(nvme_admin_delete_sq),		\
		nvme_admin_opcode_name(nvme_admin_create_sq),		\
		nvme_admin_opcode_name(nvme_admin_get_log_page),	\
		nvme_admin_opcode_name(nvme_admin_delete_cq),		\
		nvme_admin_opcode_name(nvme_admin_create_cq),		\
		nvme_admin_opcode_name(nvme_admin_identify),		\
		nvme_admin_opcode_name(nvme_admin_abort_cmd),		\
		nvme_admin_opcode_name(nvme_admin_set_features),	\
		nvme_admin_opcode_name(nvme_admin_get_features),	\
		nvme_admin_opcode_name(nvme_admin_async_event),		\
		nvme_admin_opcode_name(nvme_admin_ns_mgmt),		\
		nvme_admin_opcode_name(nvme_admin_activate_fw),		\
		nvme_admin_opcode_name(nvme_admin_download_fw),		\
		nvme_admin_opcode_name(nvme_admin_dev_self_test),	\
		nvme_admin_opcode_name(nvme_admin_ns_attach),		\
		nvme_admin_opcode_name(nvme_admin_keep_alive),		\
		nvme_admin_opcode_name(nvme_admin_directive_send),	\
		nvme_admin_opcode_name(nvme_admin_directive_recv),	\
		nvme_admin_opcode_name(nvme_admin_virtual_mgmt),	\
		nvme_admin_opcode_name(nvme_admin_nvme_mi_send),	\
		nvme_admin_opcode_name(nvme_admin_nvme_mi_recv),	\
		nvme_admin_opcode_name(nvme_admin_dbbuf),		\
		nvme_admin_opcode_name(nvme_admin_format_nvm),		\
		nvme_admin_opcode_name(nvme_admin_security_send),	\
		nvme_admin_opcode_name(nvme_admin_security_recv),	\
		nvme_admin_opcode_name(nvme_admin_sanitize_nvm),	\
		nvme_admin_opcode_name(nvme_admin_get_lba_status))	/* [한국어] 인자/선언 연속행 */

/*
 * [한국어] Create Queue 플래그 + Feature ID + Log Page ID + FW 활성 동작
 * NUM_QUEUES 는 초기화 필수. SMART/ANA/Discovery 로그는 런타임 관측 핵심.
 */
enum {
	NVME_QUEUE_PHYS_CONTIG	= (1 << 0),	/* [한국어] 큐 메모리가 물리 연속 — PRP 단일 페이지 가능 */
	NVME_CQ_IRQ_ENABLED	= (1 << 1),	/* [한국어] CQ 인터럽트 사용 (vs 폴링) */
	NVME_SQ_PRIO_URGENT	= (0 << 1),	/* [한국어] SQ 우선순위 Urgent */
	NVME_SQ_PRIO_HIGH	= (1 << 1),	/* [한국어] SQ 우선순위 High */
	NVME_SQ_PRIO_MEDIUM	= (2 << 1),	/* [한국어] SQ 우선순위 Medium */
	NVME_SQ_PRIO_LOW	= (3 << 1),	/* [한국어] SQ 우선순위 Low */
	NVME_FEAT_ARBITRATION	= 0x01,	/* [한국어] 중재 버스트/가중치 Feature */
	NVME_FEAT_POWER_MGMT	= 0x02,	/* [한국어] 전원 상태 선택 Feature */
	NVME_FEAT_LBA_RANGE	= 0x03,	/* [한국어] LBA Range Type Feature */
	NVME_FEAT_TEMP_THRESH	= 0x04,	/* [한국어] 온도 임계 — hwmon 연동 */
	NVME_FEAT_ERR_RECOVERY	= 0x05,	/* [한국어] 오류 복구 시간 Feature */
	NVME_FEAT_VOLATILE_WC	= 0x06,	/* [한국어] 휘발 쓰기 캐시 on/off */
	NVME_FEAT_NUM_QUEUES	= 0x07,	/* [한국어] I/O 큐 쌍 수 협상 — 초기화 필수 */
	NVME_FEAT_IRQ_COALESCE	= 0x08,	/* [한국어] 인터럽트 병합 Feature */
	NVME_FEAT_IRQ_CONFIG	= 0x09,	/* [한국어] 벡터별 IRQ 설정 */
	NVME_FEAT_WRITE_ATOMIC	= 0x0a,	/* [한국어] 원자 쓰기 단위 제어 */
	NVME_FEAT_ASYNC_EVENT	= 0x0b,	/* [한국어] AEN 마스크 설정 */
	NVME_FEAT_AUTO_PST	= 0x0c,	/* [한국어] 자율 전원 전이 테이블 */
	NVME_FEAT_HOST_MEM_BUF	= 0x0d,	/* [한국어] Host Memory Buffer */
	NVME_FEAT_TIMESTAMP	= 0x0e,	/* [한국어] 컨트롤러 시계 */
	NVME_FEAT_KATO		= 0x0f,	/* [한국어] Keep Alive Timeout */
	NVME_FEAT_HCTM		= 0x10,	/* [한국어] Host Controlled Thermal */
	NVME_FEAT_NOPSC		= 0x11,	/* [한국어] Non-Operational Power State Config */
	NVME_FEAT_RRL		= 0x12,	/* [한국어] Read Recovery Level */
	NVME_FEAT_PLM_CONFIG	= 0x13,	/* [한국어] Predictable Latency Mode 설정 */
	NVME_FEAT_PLM_WINDOW	= 0x14,	/* [한국어] PLM 윈도 */
	NVME_FEAT_HOST_BEHAVIOR	= 0x16,	/* [한국어] 호스트 동작 지원 비트 */
	NVME_FEAT_SANITIZE	= 0x17,	/* [한국어] Sanitize 설정 */
	NVME_FEAT_FDP		= 0x1d,	/* [한국어] Flexible Data Placement */
	NVME_FEAT_SW_PROGRESS	= 0x80,	/* [한국어] 소프트웨어 진행 마커 */
	NVME_FEAT_HOST_ID	= 0x81,	/* [한국어] Host Identifier 설정 */
	NVME_FEAT_RESV_MASK	= 0x82,	/* [한국어] 예약 알림 마스크 */
	NVME_FEAT_RESV_PERSIST	= 0x83,	/* [한국어] 예약 지속성 */
	NVME_FEAT_WRITE_PROTECT	= 0x84,	/* [한국어] NS 쓰기 보호 상태 */
	NVME_FEAT_VENDOR_START	= 0xC0,	/* [한국어] 벤더 Feature 시작 */
	NVME_FEAT_VENDOR_END	= 0xFF,	/* [한국어] 벤더 Feature 끝 */
	NVME_LOG_SUPPORTED	= 0x00,	/* [한국어] 지원 로그 페이지 목록 */
	NVME_LOG_ERROR		= 0x01,	/* [한국어] 에러 정보 로그 */
	NVME_LOG_SMART		= 0x02,	/* [한국어] SMART/Health */
	NVME_LOG_FW_SLOT	= 0x03,	/* [한국어] FW 슬롯 정보 */
	NVME_LOG_CHANGED_NS	= 0x04,	/* [한국어] 변경된 NS 목록 — 재스캔 트리거 */
	NVME_LOG_CMD_EFFECTS	= 0x05,	/* [한국어] Commands Supported and Effects */
	NVME_LOG_DEVICE_SELF_TEST = 0x06,	/* [한국어] 셀프테스트 로그 */
	NVME_LOG_TELEMETRY_HOST = 0x07,	/* [한국어] 호스트 개시 텔레메트리 */
	NVME_LOG_TELEMETRY_CTRL = 0x08,	/* [한국어] 컨트롤러 개시 텔레메트리 */
	NVME_LOG_ENDURANCE_GROUP = 0x09,	/* [한국어] Endurance Group 로그 */
	NVME_LOG_ANA		= 0x0c,	/* [한국어] Asymmetric Namespace Access — multipath */
	NVME_LOG_FEATURES	= 0x12,	/* [한국어] 지원 Feature 로그 */
	NVME_LOG_RMI		= 0x16,	/* [한국어] Rotational Media Information */
	NVME_LOG_FDP_CONFIGS	= 0x20,	/* [한국어] FDP 구성 로그 */
	NVME_LOG_DISC		= 0x70,	/* [한국어] Discovery 로그 0x70 — fabrics */
	NVME_LOG_RESERVATION	= 0x80,	/* [한국어] Reservation Notification 로그 */
	NVME_FWACT_REPL		= (0 << 3),	/* [한국어] Firmware Commit: 교체만 */
	NVME_FWACT_REPL_ACTV	= (1 << 3),	/* [한국어] Firmware Commit: 교체 후 활성화 */
	NVME_FWACT_ACTV		= (2 << 3),	/* [한국어] Firmware Commit: 활성화만 */
};

struct nvme_supported_log {
	__le32	lids[256];	/* [한국어] LID별 지원 비트 — LSUPP 등 (컨트롤러 작성) */
};

enum {
	NVME_LIDS_LSUPP	= 1 << 0,	/* [한국어] 해당 로그 페이지 지원 */
};

struct nvme_supported_features_log {
	__le32	fis[256];	/* [한국어] FID별 지원/범위 변경 가능 비트 */
};

enum {
	NVME_FIS_FSUPP	= 1 << 0,	/* [한국어] Feature 지원 */
	NVME_FIS_NSCPE	= 1 << 20,	/* [한국어] NS 범위 변경 가능 */
	NVME_FIS_CSCPE	= 1 << 21,	/* [한국어] 컨트롤러 범위 변경 가능 */
};

/* NVMe Namespace Write Protect State */
enum {
	NVME_NS_NO_WRITE_PROTECT = 0,	/* [한국어] 쓰기 보호 없음 */
	NVME_NS_WRITE_PROTECT,	/* [한국어] 쓰기 보호 활성 */
	NVME_NS_WRITE_PROTECT_POWER_CYCLE,	/* [한국어] 전원 사이클까지 쓰기 보호 */
	NVME_NS_WRITE_PROTECT_PERMANENT,	/* [한국어] 영구 쓰기 보호 */
};

#define NVME_MAX_CHANGED_NAMESPACES	1024	/* [한국어] Changed NS 로그 최대 NSID 개수 — 재스캔 버퍼 상한 */

/*
 * [한국어] Identify Admin SQE (opcode 06h)
 * cns+ctrlid+csi+cnssid 로 출력 구조 선택. dptr → 4096B 버퍼. 프로브 핵심.
 * 호스트 작성, 컨트롤러가 데이터 버퍼를 채움.
 */
struct nvme_identify {
	__u8			opcode;	/* [한국어] nvme_admin_identify */
	__u8			flags;	/* [한국어] FUSE/PSDT */
	__u16			command_id;	/* [한국어] CID */
	__le32			nsid;	/* [한국어] 대상 NSID (CNS에 따라) */
	__u64			rsvd2[2];	/* [한국어] 예약 */
	union nvme_data_ptr	dptr;	/* [한국어] 4096B Identify 출력 버퍼 */
	__u8			cns;	/* [한국어] CNS — NVME_ID_CNS_* */
	__u8			rsvd3;	/* [한국어] 예약 */
	__le16			ctrlid;	/* [한국어] 대상 Controller ID */
	__le16			cnssid;	/* [한국어] Command Set 관련 NSID */
	__u8			rsvd11;	/* [한국어] 예약 */
	__u8			csi;	/* [한국어] Command Set Identifier — NVM/ZNS */
	__u32			rsvd12[4];	/* [한국어] 예약 */
};

#define NVME_IDENTIFY_DATA_SIZE 4096	/* [한국어] Identify 데이터 구조 기본 전송 크기 4096B — DMA 버퍼 할당 기준 */

/*
 * [한국어] Get/Set Features SQE — fid + dword11..15, 선택적 dptr
 * 큐 수·IRQ·KATO·온도 등 런타임 정책. 호스트 작성.
 */
struct nvme_features {
	__u8			opcode;	/* [한국어] get_features 또는 set_features */
	__u8			flags;	/* [한국어] FUSE/PSDT */
	__u16			command_id;	/* [한국어] CID */
	__le32			nsid;	/* [한국어] NS 범위 Feature 시 NSID */
	__u64			rsvd2[2];	/* [한국어] 예약 */
	union nvme_data_ptr	dptr;	/* [한국어] 선택적 Feature 데이터 버퍼 */
	__le32			fid;	/* [한국어] Feature Identifier — NVME_FEAT_* */
	__le32			dword11;	/* [한국어] Feature 값/선택 CDW11 */
	__le32                  dword12;	/* [한국어] Feature CDW12 */
	__le32                  dword13;	/* [한국어] Feature CDW13 */
	__le32                  dword14;	/* [한국어] Feature CDW14 */
	__le32                  dword15;	/* [한국어] Feature CDW15 */
};

/*
 * [한국어] Host Memory Buffer 디스크립터 — 컨트롤러 내부 버퍼 대신 호스트 메모리 청크
 */
struct nvme_host_mem_buf_desc {
	__le64			addr;	/* [한국어] 호스트 버퍼 물리 주소 LE64 */
	__le32			size;	/* [한국어] 바이트 크기 LE32 */
	__u32			rsvd;	/* [한국어] 예약 */
};

/*
 * [한국어] Create I/O Completion Queue Admin SQE
 * prp1=CQ 버퍼, cqid/qsize, cq_flags(연속·IRQ), irq_vector. SQ 보다 먼저 생성.
 */
struct nvme_create_cq {
	__u8			opcode;	/* [한국어] nvme_admin_create_cq */
	__u8			flags;	/* [한국어] flags */
	__u16			command_id;	/* [한국어] CID */
	__u32			rsvd1[5];	/* [한국어] 예약 */
	__le64			prp1;	/* [한국어] CQ 메모리 물리 주소 */
	__u64			rsvd8;	/* [한국어] 예약 */
	__le16			cqid;	/* [한국어] 생성할 CQ ID */
	__le16			qsize;	/* [한국어] 큐 크기 0-based; CAP.MQES 이하 */
	__le16			cq_flags;	/* [한국어] PHYS_CONTIG | IRQ_ENABLED */
	__le16			irq_vector;	/* [한국어] MSI-X 벡터 인덱스 */
	__u32			rsvd12[4];	/* [한국어] 예약 */
};

/*
 * [한국어] Create I/O Submission Queue Admin SQE
 * prp1=SQ 버퍼, sqid/qsize/prio, cqid=연결 CQ. 생성 후 doorbell 로 제출 시작.
 */
struct nvme_create_sq {
	__u8			opcode;	/* [한국어] nvme_admin_create_sq */
	__u8			flags;	/* [한국어] flags */
	__u16			command_id;	/* [한국어] CID */
	__u32			rsvd1[5];	/* [한국어] 예약 */
	__le64			prp1;	/* [한국어] SQ 메모리 물리 주소 */
	__u64			rsvd8;	/* [한국어] 예약 */
	__le16			sqid;	/* [한국어] 생성할 SQ ID */
	__le16			qsize;	/* [한국어] 큐 크기 0-based */
	__le16			sq_flags;	/* [한국어] 연속·우선순위 (URGENT/HIGH/MED/LOW) */
	__le16			cqid;	/* [한국어] 연결할 CQ ID — 선행 Create CQ 필요 */
	__u32			rsvd12[4];	/* [한국어] 예약 */
};

/*
 * [한국어] Delete SQ/CQ Admin SQE — qid 만 지정. 잔여 명령은 abort/flush 정책
 */
struct nvme_delete_queue {
	__u8			opcode;	/* [한국어] delete_sq 또는 delete_cq */
	__u8			flags;	/* [한국어] flags */
	__u16			command_id;	/* [한국어] CID */
	__u32			rsvd1[9];	/* [한국어] 예약 */
	__le16			qid;	/* [한국어] 삭제 대상 큐 ID */
	__u16			rsvd10;	/* [한국어] 예약 */
	__u32			rsvd11[5];	/* [한국어] 예약 */
};

/*
 * [한국어] Abort Command Admin SQE — sqid+cid 대상 중단 요청
 */
struct nvme_abort_cmd {
	__u8			opcode;	/* [한국어] nvme_admin_abort_cmd */
	__u8			flags;	/* [한국어] flags */
	__u16			command_id;	/* [한국어] 이 Abort 자체의 CID */
	__u32			rsvd1[9];	/* [한국어] 예약 */
	__le16			sqid;	/* [한국어] 대상 SQ ID */
	__u16			cid;	/* [한국어] 대상 command_id */
	__u32			rsvd11[5];	/* [한국어] 예약 */
};

/*
 * [한국어] Firmware Image Download Admin SQE — numd+offset 로 이미지 청크
 */
struct nvme_download_firmware {
	__u8			opcode;	/* [한국어] nvme_admin_download_fw */
	__u8			flags;	/* [한국어] flags */
	__u16			command_id;	/* [한국어] CID */
	__u32			rsvd1[5];	/* [한국어] 예약 */
	union nvme_data_ptr	dptr;	/* [한국어] FW 이미지 청크 버퍼 */
	__le32			numd;	/* [한국어] Number of Dwords */
	__le32			offset;	/* [한국어] 이미지 내 오프셋 */
	__u32			rsvd12[4];	/* [한국어] 예약 */
};

/*
 * [한국어] Format NVM Admin SQE — cdw10 에 LBAF/PI/메타/세션 범위. NS 데이터 파괴
 */
struct nvme_format_cmd {
	__u8			opcode;	/* [한국어] nvme_admin_format_nvm */
	__u8			flags;	/* [한국어] flags */
	__u16			command_id;	/* [한국어] CID */
	__le32			nsid;	/* [한국어] 대상 NS 또는 NVME_NSID_ALL */
	__u64			rsvd2[4];	/* [한국어] 예약 */
	__le32			cdw10;	/* [한국어] LBAF/MS/PI/SES 인코딩 */
	__u32			rsvd11[5];	/* [한국어] 예약 */
};

/*
 * [한국어] Get Log Page Admin SQE
 * lid/lsp/numd/lpo/lsi/csi. SMART·에러·ANA·Discovery·Effects 등 전부 이 명령.
 */
struct nvme_get_log_page_command {
	__u8			opcode;	/* [한국어] nvme_admin_get_log_page */
	__u8			flags;	/* [한국어] flags */
	__u16			command_id;	/* [한국어] CID */
	__le32			nsid;	/* [한국어] 로그 범위 NSID */
	__u64			rsvd2[2];	/* [한국어] 예약 */
	union nvme_data_ptr	dptr;	/* [한국어] 로그 수신 버퍼 */
	__u8			lid;	/* [한국어] Log Page Identifier — NVME_LOG_* */
	__u8			lsp; /* upper 4 bits reserved */	/* [한국어] Log Specific Parameter */
	__le16			numdl;	/* [한국어] Number of Dwords Lower */
	__le16			numdu;	/* [한국어] Number of Dwords Upper */
	__le16			lsi;	/* [한국어] Log Specific Identifier */
	union {	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
		struct {	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
			__le32 lpol;	/* [한국어] Log Page Offset lower */
			__le32 lpou;	/* [한국어] Log Page Offset upper */
		};
		__le64 lpo;	/* [한국어] Log Page Offset 64b 통합 뷰 */
	};
	__u8			rsvd14[3];	/* [한국어] 예약 */
	__u8			csi;	/* [한국어] Command Set Identifier */
	__u32			rsvd15;	/* [한국어] 예약 */
};

/*
 * [한국어] Directive Send/Receive SQE — streams 등 directive 프레임워크
 */
struct nvme_directive_cmd {
	__u8			opcode;	/* [한국어] directive_send 또는 directive_recv */
	__u8			flags;	/* [한국어] flags */
	__u16			command_id;	/* [한국어] CID */
	__le32			nsid;	/* [한국어] 대상 NSID */
	__u64			rsvd2[2];	/* [한국어] 예약 */
	union nvme_data_ptr	dptr;	/* [한국어] directive 데이터 */
	__le32			numd;	/* [한국어] Number of Dwords */
	__u8			doper;	/* [한국어] Directive Operation */
	__u8			dtype;	/* [한국어] Directive Type — Identify/Streams */
	__le16			dspec;	/* [한국어] Directive Specific */
	__u8			endir;	/* [한국어] Enable Directive */
	__u8			tdtype;	/* [한국어] Target Directive Type */
	__u16			rsvd15;	/* [한국어] 예약 */

	__u32			rsvd16[3];	/* [한국어] 예약 */
};

/*
 * Fabrics subcommands.
 */
/*
 * [한국어] Fabrics 공통 opcode 0x7f — fctype 로 서브명령 구분
 * SQE.opcode=0x7f 이면 Admin/I/O 가 아니라 Fabrics capsule.
 * fctype: Connect / Property Get·Set / Auth. 트랜스포트가 연결·레지스터 에뮬·인증에 사용.
 */
enum nvmf_fabrics_opcode {
	nvme_fabrics_command		= 0x7f,	/* [한국어] 모든 Fabrics 명령의 opcode — nvme_is_fabrics() 판정 기준 */
};

enum nvmf_capsule_command {
	nvme_fabrics_type_property_set	= 0x00,	/* [한국어] fctype 00h Property Set — CC 등 기록 */
	nvme_fabrics_type_connect	= 0x01,	/* [한국어] fctype 01h Connect — 큐 연결 수립 */
	nvme_fabrics_type_property_get	= 0x04,	/* [한국어] fctype 04h Property Get — CAP/CSTS 읽기 */
	nvme_fabrics_type_auth_send	= 0x05,	/* [한국어] fctype 05h Authentication Send */
	nvme_fabrics_type_auth_receive	= 0x06,	/* [한국어] fctype 06h Authentication Receive */
};

#define nvme_fabrics_type_name(type)   { type, #type }	/* [한국어] nvme_fabrics_type_name 매크로 — 상위 섹션 계약 참고 */
#define show_fabrics_type_name(type)					\
	__print_symbolic(type,						\
		nvme_fabrics_type_name(nvme_fabrics_type_property_set),	\
		nvme_fabrics_type_name(nvme_fabrics_type_connect),	\
		nvme_fabrics_type_name(nvme_fabrics_type_property_get), \
		nvme_fabrics_type_name(nvme_fabrics_type_auth_send),	\
		nvme_fabrics_type_name(nvme_fabrics_type_auth_receive))	/* [한국어] 인자/선언 연속행 */

/*
 * If not fabrics command, fctype will be ignored.
 */
#define show_opcode_name(qid, opcode, fctype)			\
	((opcode) == nvme_fabrics_command ?			\
	 show_fabrics_type_name(fctype) :			\
	((qid) ?						\
	 show_nvm_opcode_name(opcode) :				\
	 show_admin_opcode_name(opcode)))	/* [한국어] 인자/선언 연속행 */

struct nvmf_common_command {
	__u8	opcode;	/* [한국어] opcode 필드 — 상위 구조 작성자·동기화 참고 */
	__u8	resv1;	/* [한국어] 스펙 예약 — 호스트 0 */
	__u16	command_id;	/* [한국어] command_id 필드 — 상위 구조 작성자·동기화 참고 */
	__u8	fctype;	/* [한국어] fctype 필드 — 상위 구조 작성자·동기화 참고 */
	__u8	resv2[35];	/* [한국어] 스펙 예약 — 호스트 0 */
	__u8	ts[24];	/* [한국어] ts 필드 — 상위 구조 작성자·동기화 참고 */
};

/*
 * The legal cntlid range a NVMe Target will provide.
 * Note that cntlid of value 0 is considered illegal in the fabrics world.
 * Devices based on earlier specs did not have the subsystem concept;
 * therefore, those devices had their cntlid value set to 0 as a result.
 */
#define NVME_CNTLID_MIN		1	/* [한국어] Fabrics 유효 cntlid 하한 — 0 은 레거시/불법 취급 */
#define NVME_CNTLID_MAX		0xffef	/* [한국어] 정적 cntlid 상한 */
#define NVME_CNTLID_DYNAMIC	0xffff	/* [한국어] Connect 시 컨트롤러가 동적 할당하는 cntlid 요청 값 */

#define MAX_DISC_LOGS	255	/* [한국어] 디스커버리 로그 페이지 처리 상한 — genctr 루프 방어 */

/* Discovery log page entry flags (EFLAGS): */
enum {
	NVME_DISC_EFLAGS_EPCSD		= (1 << 1),	/* [한국어] Explicit Persistent Connection 지원 등 */
	NVME_DISC_EFLAGS_DUPRETINFO	= (1 << 0),	/* [한국어] 중복 반환 정보 플래그 */
};

/*
 * [한국어] Discovery Log Page Entry — 연결 가능한 타깃 한 줄
 * trtype/adrfam/subtype/treq + portid/cntlid/asqsz + trsvcid/subnqn/traddr + tsas.
 * 호스트 fabrics 가 이를 파싱해 nvme connect 인자 생성. 디스커버리 컨트롤러 작성.
 */
/* Discovery log page entry */
struct nvmf_disc_rsp_page_entry {
	__u8		trtype;	/* [한국어] 전송 타입 — PCI/RDMA/FC/TCP (NVMF_TRTYPE_*) */
	__u8		adrfam;	/* [한국어] 주소 패밀리 — traddr 해석 방식 */
	__u8		subtype;	/* [한국어] 서브시스템 타입 — DISC/NVME/CURR */
	__u8		treq;	/* [한국어] 전송 요구 — 보안 채널·SQ flow */
	__le16		portid;	/* [한국어] 포트 ID LE16 */
	__le16		cntlid;	/* [한국어] 권고 Controller ID */
	__le16		asqsz;	/* [한국어] Admin SQ 크기 권고 */
	__le16		eflags;	/* [한국어] 엔트리 플래그 — 중복 정보 등 */
	__u8		resv10[20];	/* [한국어] 예약 */
	char		trsvcid[NVMF_TRSVCID_SIZE];	/* [한국어] 서비스 ID 문자열(포트 번호 등) */
	__u8		resv64[192];	/* [한국어] 예약 */
	char		subnqn[NVMF_NQN_FIELD_LEN];	/* [한국어] 대상 서브시스템 NQN 256B 필드 */
	char		traddr[NVMF_TRADDR_SIZE];	/* [한국어] 전송 주소 문자열(IP/WWPN 등) */
	union tsas {	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
		char		common[NVMF_TSAS_SIZE];	/* [한국어] 전송 특화 공통 256B 슬롯 */
		struct rdma {	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
			__u8	qptype;	/* [한국어] RDMA QP 타입 — Connected/Datagram */
			__u8	prtype;	/* [한국어] RDMA 프로바이더 — IB/RoCE/iWARP */
			__u8	cms;	/* [한국어] 연결 관리 서비스 — RDMA_CM */
			__u8	resv3[5];	/* [한국어] 예약 */
			__u16	pkey;	/* [한국어] Partition Key */
			__u8	resv10[246];	/* [한국어] 예약 패딩 */
		} rdma;	/* [한국어] RDMA TSAS 오버레이 */
		struct tcp {	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
			__u8	sectype;	/* [한국어] TCP SECTYPE — NONE/TLS12/TLS13 */
		} tcp;	/* [한국어] TCP TSAS 오버레이 */
	} tsas;	/* [한국어] Transport Specific Address Subtype 유니온 */
};

/*
 * [한국어] Discovery Log Page 헤더
 * genctr 변경 시 재읽기, numrec 만큼 entries[] 반복. LID=70h.
 */
/* Discovery log page header */
struct nvmf_disc_rsp_page_hdr {
	__le64		genctr;	/* [한국어] generation counter — 변경 감지 LE64 */
	__le64		numrec;	/* [한국어] 레코드 수 LE64 */
	__le16		recfmt;	/* [한국어] 레코드 포맷 버전 */
	__u8		resv14[1006];	/* [한국어] 예약 — entries 앞 정렬 */
	struct nvmf_disc_rsp_page_entry entries[];	/* [한국어] 가변 길이 디스커버리 엔트리 배열 */
};

enum {
	NVME_CONNECT_DISABLE_SQFLOW	= (1 << 2),	/* [한국어] Connect cattr: SQ flow control disable */
};

/*
 * [한국어] Fabrics Connect SQE
 * qid=0 Admin 큐 연결 후 속성 Identify, qid>0 I/O 큐.
 * sqsize/kato/cattr, dptr → nvmf_connect_data (hostid, NQN 쌍).
 * 연결 성공 CQE 가 cntlid 등 result 반환. 호스트 작성.
 */
struct nvmf_connect_command {
	__u8		opcode;	/* [한국어] 0x7f fabrics */
	__u8		resv1;	/* [한국어] 예약 */
	__u16		command_id;	/* [한국어] CID */
	__u8		fctype;	/* [한국어] connect = 0x01 */
	__u8		resv2[19];	/* [한국어] 예약 */
	union nvme_data_ptr dptr;	/* [한국어] → nvmf_connect_data 버퍼 */
	__le16		recfmt;	/* [한국어] 기록 형식 버전 */
	__le16		qid;	/* [한국어] 연결할 큐 ID — 0=Admin, >0=I/O */
	__le16		sqsize;	/* [한국어] SQ 크기 0-based */
	__u8		cattr;	/* [한국어] Connect 속성 — DISABLE_SQFLOW 등 */
	__u8		resv3;	/* [한국어] 예약 */
	__le32		kato;	/* [한국어] Keep Alive Timeout (ms) LE32 */
	__u8		resv4[12];	/* [한국어] 예약 */
};

enum {
	NVME_CONNECT_AUTHREQ_ASCR	= (1U << 18),	/* [한국어] Secure Channel concatenation 요구 */
	NVME_CONNECT_AUTHREQ_ATR	= (1U << 17),	/* [한국어] Authentication 요구 */
};

/*
 * [한국어] Connect 데이터 페이로드 (호스트→컨트롤러)
 * hostid UUID, cntlid(동적 0xffff 가능), subsysnqn, hostnqn. 인증·권한의 정체성.
 */
struct nvmf_connect_data {
	uuid_t		hostid;	/* [한국어] Host Identifier UUID */
	__le16		cntlid;	/* [한국어] 요청 cntlid — DYNAMIC=0xffff 면 할당 요청 */
	char		resv4[238];	/* [한국어] 예약 */
	char		subsysnqn[NVMF_NQN_FIELD_LEN];	/* [한국어] 대상 Subsystem NQN */
	char		hostnqn[NVMF_NQN_FIELD_LEN];	/* [한국어] 이 호스트의 NQN */
	char		resv5[256];	/* [한국어] 예약 */
};

/*
 * [한국어] Property Set — Fabrics 에서 CC 등 레지스터 에뮬 기록
 * offset+value. PCIe MMIO 대신 캡슐로 CAP/CC/CSTS 공간 접근.
 */
struct nvmf_property_set_command {
	__u8		opcode;	/* [한국어] 0x7f */
	__u8		resv1;	/* [한국어] 예약 */
	__u16		command_id;	/* [한국어] CID */
	__u8		fctype;	/* [한국어] property_set */
	__u8		resv2[35];	/* [한국어] 예약 */
	__u8		attrib;	/* [한국어] 접근 폭 등 속성 */
	__u8		resv3[3];	/* [한국어] 예약 */
	__le32		offset;	/* [한국어] 레지스터 오프셋 — NVME_REG_* 와 동일 번호 */
	__le64		value;	/* [한국어] 기록 값 LE64 */
	__u8		resv4[8];	/* [한국어] 예약 */
};

/*
 * [한국어] Property Get — 레지스터 읽기 에뮬
 * offset 지정, 결과는 CQE.result. enable 시퀀스를 fabrics 에서도 동일 개념으로 수행.
 */
struct nvmf_property_get_command {
	__u8		opcode;	/* [한국어] 0x7f */
	__u8		resv1;	/* [한국어] 예약 */
	__u16		command_id;	/* [한국어] CID */
	__u8		fctype;	/* [한국어] property_get */
	__u8		resv2[35];	/* [한국어] 예약 */
	__u8		attrib;	/* [한국어] 접근 폭 등 */
	__u8		resv3[3];	/* [한국어] 예약 */
	__le32		offset;	/* [한국어] 읽을 레지스터 오프셋 */
	__u8		resv4[16];	/* [한국어] 예약 */
};

/*
 * [한국어] Fabrics Authentication 공통 SQE 레이아웃 — Auth Send/Receive 의 상위 뷰.
 * opcode=Fabrics(0x7f), fctype 로 Send vs Receive 구분. spsp0/1·secp 가 보안
 * 프로토콜 선택(DH-HMAC-CHAP=0xE9). dptr 가 인증 메시지 버퍼를 가리키고
 * al_tl 은 Send 의 tl / Receive 의 al 오버레이(호스트가 길이 계약).
 */
struct nvmf_auth_common_command {
	__u8		opcode;	/* [한국어] 항상 nvme_fabrics_command (0x7f) */
	__u8		resv1;	/* [한국어] 스펙 예약 — 호스트 0 */
	__u16		command_id;	/* [한국어] 호스트 CID — CQE 매칭 */
	__u8		fctype;	/* [한국어] auth send / auth receive fabrics 타입 */
	__u8		resv2[19];	/* [한국어] 예약 패딩 — SQE 오프셋 정렬 */
	union nvme_data_ptr dptr;	/* [한국어] 인증 페이로드 SGL/PRP — 호스트 버퍼 */
	__u8		resv3;	/* [한국어] 예약 */
	__u8		spsp0;	/* [한국어] Security Protocol Specific 0 — CHAP 상수 */
	__u8		spsp1;	/* [한국어] Security Protocol Specific 1 */
	__u8		secp;	/* [한국어] Security Protocol — DHCHAP_PROTOCOL_IDENTIFIER */
	__le32		al_tl;	/* [한국어] Send=tl / Receive=al 길이 오버레이 필드 */
	__u8		resv4[16];	/* [한국어] SQE 64B 맞춤 예약 */
};

/*
 * [한국어] Authentication Send — 호스트→컨트롤러 메시지 제출(Negotiate/Reply/Success2).
 * tl 은 전송 바이트 수. auth.c 가 chap->buf 를 dptr 에 실어 동기 제출.
 */
struct nvmf_auth_send_command {
	__u8		opcode;	/* [한국어] Fabrics opcode */
	__u8		resv1;	/* [한국어] 예약 */
	__u16		command_id;	/* [한국어] CID */
	__u8		fctype;	/* [한국어] auth send fctype */
	__u8		resv2[19];	/* [한국어] 예약 */
	union nvme_data_ptr dptr;	/* [한국어] 송신 메시지 버퍼 */
	__u8		resv3;	/* [한국어] 예약 */
	__u8		spsp0;	/* [한국어] SPSP0 */
	__u8		spsp1;	/* [한국어] SPSP1 */
	__u8		secp;	/* [한국어] 보안 프로토콜 ID */
	__le32		tl;	/* [한국어] Transfer Length — 송신 페이로드 바이트 */
	__u8		resv4[16];	/* [한국어] 예약 */
};

/*
 * [한국어] Authentication Receive — 컨트롤러→호스트 메시지 수신(Challenge/Success1/Failure).
 * al 은 할당 버퍼 크기. 호스트가 수신 후 auth_type/auth_id 로 단계 전이.
 */
struct nvmf_auth_receive_command {
	__u8		opcode;	/* [한국어] Fabrics opcode */
	__u8		resv1;	/* [한국어] 예약 */
	__u16		command_id;	/* [한국어] CID */
	__u8		fctype;	/* [한국어] auth receive fctype */
	__u8		resv2[19];	/* [한국어] 예약 */
	union nvme_data_ptr dptr;	/* [한국어] 수신 메시지 버퍼 */
	__u8		resv3;	/* [한국어] 예약 */
	__u8		spsp0;	/* [한국어] SPSP0 */
	__u8		spsp1;	/* [한국어] SPSP1 */
	__u8		secp;	/* [한국어] 보안 프로토콜 ID */
	__le32		al;	/* [한국어] Allocation Length — 수신 버퍼 용량 */
	__u8		resv4[16];	/* [한국어] 예약 */
};

/*
 * [한국어] DH-HMAC-CHAP 인증 상수 — Fabrics Auth Send/Receive 페이로드
 * auth.c 가 Negotiate→Challenge→Reply→Success1/2 시퀀스를 구동.
 */
/* Value for secp */
enum {
	NVME_AUTH_DHCHAP_PROTOCOL_IDENTIFIER	= 0xe9,	/* [한국어] DH-HMAC-CHAP security protocol 식별자 0xE9 */
};

/* Defined value for auth_type */
enum {
	NVME_AUTH_COMMON_MESSAGES	= 0x00,	/* [한국어] 공통 인증 메시지 클래스 */
	NVME_AUTH_DHCHAP_MESSAGES	= 0x01,	/* [한국어] DH-HMAC-CHAP 메시지 클래스 */
};

/* Defined messages for auth_id */
enum {
	NVME_AUTH_DHCHAP_MESSAGE_NEGOTIATE	= 0x00,	/* [한국어] 협상 메시지 */
	NVME_AUTH_DHCHAP_MESSAGE_CHALLENGE	= 0x01,	/* [한국어] 챌린지 (컨트롤러→호스트) */
	NVME_AUTH_DHCHAP_MESSAGE_REPLY		= 0x02,	/* [한국어] 응답 (호스트→컨트롤러) */
	NVME_AUTH_DHCHAP_MESSAGE_SUCCESS1	= 0x03,	/* [한국어] 1차 성공·상호 인증 */
	NVME_AUTH_DHCHAP_MESSAGE_SUCCESS2	= 0x04,	/* [한국어] 최종 성공 */
	NVME_AUTH_DHCHAP_MESSAGE_FAILURE2	= 0xf0,	/* [한국어] 인증 실패 메시지 2 */
	NVME_AUTH_DHCHAP_MESSAGE_FAILURE1	= 0xf1,	/* [한국어] 인증 실패 메시지 1 */
};

/*
 * [한국어] Negotiate 에 실리는 프로토콜 디스크립터 — 지원 hash/DH 목록.
 * idlist 앞 halen 바이트가 해시 ID, 이어 dhlen 바이트가 DH 그룹 ID.
 */
struct nvmf_auth_dhchap_protocol_descriptor {
	__u8		authid;	/* [한국어] NVME_AUTH_DHCHAP_AUTH_ID */
	__u8		rsvd;	/* [한국어] 예약 */
	__u8		halen;	/* [한국어] 해시 ID 목록 길이 */
	__u8		dhlen;	/* [한국어] DH 그룹 ID 목록 길이 */
	__u8		idlist[60];	/* [한국어] hash IDs 이어 dh group IDs 가변 목록 */
};

enum {
	NVME_AUTH_DHCHAP_AUTH_ID	= 0x01,	/* [한국어] DH-HMAC-CHAP 인증 방식 ID */
};

/* Defined hash functions for DH-HMAC-CHAP authentication */
enum {
	NVME_AUTH_HASH_SHA256	= 0x01,	/* [한국어] SHA-256 해시 */
	NVME_AUTH_HASH_SHA384	= 0x02,	/* [한국어] SHA-384 해시 */
	NVME_AUTH_HASH_SHA512	= 0x03,	/* [한국어] SHA-512 해시 */
	NVME_AUTH_HASH_INVALID	= 0xff,	/* [한국어] 무효 해시 ID 가드 */
};

/* Maximum digest size for any NVME_AUTH_HASH_* value */
enum {
	NVME_AUTH_MAX_DIGEST_SIZE = 64,	/* [한국어] 다이제스트 버퍼 최대 64B (SHA-512) */
};

/* Defined Diffie-Hellman group identifiers for DH-HMAC-CHAP authentication */
enum {
	NVME_AUTH_DHGROUP_NULL		= 0x00,	/* [한국어] DH 없음 */
	NVME_AUTH_DHGROUP_2048		= 0x01,	/* [한국어] 2048-bit DH 그룹 */
	NVME_AUTH_DHGROUP_3072		= 0x02,	/* [한국어] 3072-bit DH 그룹 */
	NVME_AUTH_DHGROUP_4096		= 0x03,	/* [한국어] 4096-bit DH 그룹 */
	NVME_AUTH_DHGROUP_6144		= 0x04,	/* [한국어] 6144-bit DH 그룹 */
	NVME_AUTH_DHGROUP_8192		= 0x05,	/* [한국어] 8192-bit DH 그룹 */
	NVME_AUTH_DHGROUP_INVALID	= 0xff,	/* [한국어] 무효 DH 그룹 가드 */
};

enum {
	NVME_AUTH_SECP_NOSC		= 0x00,	/* [한국어] Secure channel 없음 */
	NVME_AUTH_SECP_SC		= 0x01,	/* [한국어] Secure channel 사용 */
	NVME_AUTH_SECP_NEWTLSPSK	= 0x02,	/* [한국어] 새 TLS PSK 유도 */
	NVME_AUTH_SECP_REPLACETLSPSK	= 0x03,	/* [한국어] TLS PSK 교체 */
};

/* [한국어] Negotiate 가변 프로토콜 목록 엔트리 유니온 — 현재 DH-CHAP 만 */
union nvmf_auth_protocol {
	struct nvmf_auth_dhchap_protocol_descriptor dhchap;	/* [한국어] DH-HMAC-CHAP 디스크립터 */
};

/*
 * [한국어] Host→Ctrl Negotiate — 지원 해시/DH·secure channel 선택(sc_c)·트랜잭션.
 * napd 개수만큼 auth_protocol[] 가 이어짐. auth.c setup_negotiate 작성.
 */
struct nvmf_auth_dhchap_negotiate_data {
	__u8		auth_type;	/* [한국어] NVME_AUTH_DHCHAP_MESSAGES */
	__u8		auth_id;	/* [한국어] MESSAGE_NEGOTIATE */
	__le16		rsvd;	/* [한국어] 예약 */
	__le16		t_id;	/* [한국어] 트랜잭션 ID — 핸드셰이크 상관 */
	__u8		sc_c;	/* [한국어] secure channel 선택(concat/TLS 등) */
	__u8		napd;	/* [한국어] 이어지는 protocol descriptor 개수 */
	union nvmf_auth_protocol auth_protocol[];	/* [한국어] 가변 프로토콜 목록 */
};

/*
 * [한국어] Ctrl→Host Challenge — 선택 해시/DH, c1 챌린지, 시퀀스, 선택적 DH 공개키.
 * hl=해시 길이. cval[hl] 뒤 dhvlen 바이트 DH 값. 호스트가 Reply HMAC 입력으로 사용.
 */
struct nvmf_auth_dhchap_challenge_data {
	__u8		auth_type;	/* [한국어] DHCHAP 메시지 클래스 */
	__u8		auth_id;	/* [한국어] MESSAGE_CHALLENGE */
	__u16		rsvd1;	/* [한국어] 예약 */
	__le16		t_id;	/* [한국어] 트랜잭션 ID 에코 */
	__u8		hl;	/* [한국어] 해시 출력 길이(바이트) */
	__u8		rsvd2;	/* [한국어] 예약 */
	__u8		hashid;	/* [한국어] 선택된 해시 알고리즘 ID */
	__u8		dhgid;	/* [한국어] 선택된 DH 그룹 ID (NULL=DH 없음) */
	__le16		dhvlen;	/* [한국어] 후속 DH 공개값 길이 */
	__le32		seqnum;	/* [한국어] 컨트롤러 시퀀스 s1 — HMAC 입력 */
	/* 'hl' bytes of challenge value */
	__u8		cval[];	/* [한국어] c1 챌린지 값; 뒤이어 DH 공개키 */
	/* followed by 'dhvlen' bytes of DH value */
};

/*
 * [한국어] Host→Ctrl Reply — 호스트 HMAC 응답, 선택적 상호 c2/s2, 호스트 DH 공개키.
 * cvalid 비트로 컨트롤러 챌린지 포함 여부. rval 뒤에 c2·DH 가변 영역.
 */
struct nvmf_auth_dhchap_reply_data {
	__u8		auth_type;	/* [한국어] DHCHAP 메시지 클래스 */
	__u8		auth_id;	/* [한국어] MESSAGE_REPLY */
	__le16		rsvd1;	/* [한국어] 예약 */
	__le16		t_id;	/* [한국어] 트랜잭션 ID */
	__u8		hl;	/* [한국어] 응답 해시 길이 */
	__u8		rsvd2;	/* [한국어] 예약 */
	__u8		cvalid;	/* [한국어] RESPONSE_VALID — 상호 인증 챌린지 포함 */
	__u8		rsvd3;	/* [한국어] 예약 */
	__le16		dhvlen;	/* [한국어] 호스트 DH 공개값 길이 */
	__le32		seqnum;	/* [한국어] 호스트 시퀀스 s2 (상호 인증 시) */
	/* 'hl' bytes of response data */
	__u8		rval[];	/* [한국어] 호스트 응답 HMAC; 뒤 c2·DH */
	/* followed by 'hl' bytes of Challenge value */
	/* followed by 'dhvlen' bytes of DH value */
};

enum {
	NVME_AUTH_DHCHAP_RESPONSE_VALID	= (1 << 0),	/* [한국어] Reply/Success1 에 응답 페이로드 유효 */
};

/*
 * [한국어] Ctrl→Host Success1 — 단방향 완료 또는 상호 인증 시 컨트롤러 응답.
 * rvalid 가 설정되면 rval[hl] 에 컨트롤러 HMAC. 이후 호스트 Success2 가능.
 */
struct nvmf_auth_dhchap_success1_data {
	__u8		auth_type;	/* [한국어] DHCHAP 메시지 클래스 */
	__u8		auth_id;	/* [한국어] MESSAGE_SUCCESS1 */
	__le16		rsvd1;	/* [한국어] 예약 */
	__le16		t_id;	/* [한국어] 트랜잭션 ID */
	__u8		hl;	/* [한국어] 응답 길이 */
	__u8		rsvd2;	/* [한국어] 예약 */
	__u8		rvalid;	/* [한국어] 컨트롤러 응답 포함 여부 */
	__u8		rsvd3[7];	/* [한국어] 예약 패딩 */
	/* 'hl' bytes of response value */
	__u8		rval[];	/* [한국어] 컨트롤러 HMAC (양방향 시) */
};

/*
 * [한국어] Host→Ctrl Success2 — 양방향 인증 최종 ACK. 페이로드 최소 헤더만.
 * Success1 검증 후 호스트가 보내 핸드셰이크를 닫는다.
 */
struct nvmf_auth_dhchap_success2_data {
	__u8		auth_type;	/* [한국어] DHCHAP 메시지 클래스 */
	__u8		auth_id;	/* [한국어] MESSAGE_SUCCESS2 */
	__le16		rsvd1;	/* [한국어] 예약 */
	__le16		t_id;	/* [한국어] 트랜잭션 ID */
	__u8		rsvd2[10];	/* [한국어] 고정 헤더 패딩 */
};

/*
 * [한국어] Failure1/2 — 인증 실패 사유. rescode/rescode_exp 로 해시·DH·concat
 * 불일치 등을 전달. auth.c 가 Failure2 를 구성해 보낸 뒤 연결을 중단.
 */
struct nvmf_auth_dhchap_failure_data {
	__u8		auth_type;	/* [한국어] COMMON 또는 DHCHAP 실패 클래스 */
	__u8		auth_id;	/* [한국어] FAILURE1/FAILURE2 */
	__le16		rsvd1;	/* [한국어] 예약 */
	__le16		t_id;	/* [한국어] 실패한 트랜잭션 ID */
	__u8		rescode;	/* [한국어] 실패 대분류 (FAILED 등) */
	__u8		rescode_exp;	/* [한국어] 상세 사유 (HASH_UNUSABLE 등) */
};

enum {
	NVME_AUTH_DHCHAP_FAILURE_REASON_FAILED	= 0x01,	/* [한국어] 일반 실패 대분류 코드 */
};

/* [한국어] Failure rescode_exp — 프로토콜/암호 협상 실패 상세 (auth.c 매핑) */
enum {
	NVME_AUTH_DHCHAP_FAILURE_FAILED			= 0x01,	/* [한국어] 일반 인증 실패 */
	NVME_AUTH_DHCHAP_FAILURE_NOT_USABLE		= 0x02,	/* [한국어] 프로토콜 사용 불가 */
	NVME_AUTH_DHCHAP_FAILURE_CONCAT_MISMATCH	= 0x03,	/* [한국어] secure concat 불일치 */
	NVME_AUTH_DHCHAP_FAILURE_HASH_UNUSABLE		= 0x04,	/* [한국어] 해시 알고리즘 거부 */
	NVME_AUTH_DHCHAP_FAILURE_DHGROUP_UNUSABLE	= 0x05,	/* [한국어] DH 그룹 거부 */
	NVME_AUTH_DHCHAP_FAILURE_INCORRECT_PAYLOAD	= 0x06,	/* [한국어] 페이로드 형식 오류 */
	NVME_AUTH_DHCHAP_FAILURE_INCORRECT_MESSAGE	= 0x07,	/* [한국어] 메시지 순서/타입 오류 */
};

/*
 * [한국어] Doorbell Buffer Config — 호스트 메모리 doorbell shadow.
 * prp1/prp2 가 SQ/CQ doorbell 미러 영역. PCIe 최적화 경로에서 MMIO 감소.
 */
struct nvme_dbbuf {
	__u8			opcode;	/* [한국어] Admin doorbell buffer config opcode */
	__u8			flags;	/* [한국어] PSDT 등 공통 플래그 */
	__u16			command_id;	/* [한국어] CID */
	__u32			rsvd1[5];	/* [한국어] 예약 */
	__le64			prp1;	/* [한국어] SQ doorbell 버퍼 PRP */
	__le64			prp2;	/* [한국어] CQ doorbell 버퍼 PRP */
	__u32			rsvd12[6];	/* [한국어] SQE 잔여 예약 */
};

/*
 * [한국어] Streams Directive 파라미터 — 디렉티브 기반 스트림 자원.
 * 호스트가 Identify/Directive 로 읽어 스트림 할당 정책을 맞춘다.
 */
struct streams_directive_params {
	__le16	msl;	/* [한국어] Max Streams Limit */
	__le16	nssa;	/* [한국어] NVM Subsystem Streams Available */
	__le16	nsso;	/* [한국어] NVM Subsystem Streams Open */
	__u8	rsvd[10];	/* [한국어] 예약 */
	__le32	sws;	/* [한국어] Stream Write Size */
	__le16	sgs;	/* [한국어] Stream Granularity Size */
	__le16	nsa;	/* [한국어] Namespace Streams Allocated */
	__le16	nso;	/* [한국어] Namespace Streams Open */
	__u8	rsvd2[6];	/* [한국어] 예약 */
};

/*
 * [한국어] 64바이트 Submission Queue Entry 전체 유니온
 * DMA 링 슬롯 타입. 동일 64B 를 common/rw/identify/…/fabrics 뷰로 해석.
 * 호스트 제출 경로가 채우고 SQ tail doorbell 로 컨트롤러에 알림.
 * 반드시 64B 정렬·크기 — CC.IOSQES 와 일치.
 */
struct nvme_command {
	union {	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
		struct nvme_common_command common;	/* [한국어] 공통 레이아웃 뷰 */
		struct nvme_rw_command rw;	/* [한국어] Read/Write/Compare 등 LBA 명령 뷰 */
		struct nvme_identify identify;	/* [한국어] Identify Admin 뷰 */
		struct nvme_features features;	/* [한국어] Get/Set Features 뷰 */
		struct nvme_create_cq create_cq;	/* [한국어] Create I/O CQ */
		struct nvme_create_sq create_sq;	/* [한국어] Create I/O SQ */
		struct nvme_delete_queue delete_queue;	/* [한국어] Delete SQ/CQ */
		struct nvme_download_firmware dlfw;	/* [한국어] Firmware Image Download */
		struct nvme_format_cmd format;	/* [한국어] Format NVM */
		struct nvme_dsm_cmd dsm;	/* [한국어] Dataset Management */
		struct nvme_write_zeroes_cmd write_zeroes;	/* [한국어] Write Zeroes */
		struct nvme_zone_mgmt_send_cmd zms;	/* [한국어] Zone Mgmt Send */
		struct nvme_zone_mgmt_recv_cmd zmr;	/* [한국어] Zone Mgmt Receive */
		struct nvme_abort_cmd abort;	/* [한국어] Abort */
		struct nvme_get_log_page_command get_log_page;	/* [한국어] Get Log Page */
		struct nvmf_common_command fabrics;	/* [한국어] Fabrics 공통 */
		struct nvmf_connect_command connect;	/* [한국어] Fabrics Connect */
		struct nvmf_property_set_command prop_set;	/* [한국어] Property Set */
		struct nvmf_property_get_command prop_get;	/* [한국어] Property Get */
		struct nvmf_auth_common_command auth_common;	/* [한국어] Auth 공통 헤더 */
		struct nvmf_auth_send_command auth_send;	/* [한국어] Authentication Send */
		struct nvmf_auth_receive_command auth_receive;	/* [한국어] Authentication Receive */
		struct nvme_dbbuf dbbuf;	/* [한국어] Doorbell Buffer Config */
		struct nvme_directive_cmd directive;	/* [한국어] Directive Send/Recv */
		struct nvme_io_mgmt_recv_cmd imr;	/* [한국어] I/O Management Receive */
	};
};

/*
 * [한국어] nvme_is_fabrics() — opcode==0x7f 이면 Fabrics capsule
 * 쓰기 판정·트레이스·opcode 문자열 분기에서 사용.
 */
static inline bool nvme_is_fabrics(const struct nvme_command *cmd)
{
	return cmd->common.opcode == nvme_fabrics_command;	/* [한국어] Fabrics 단일 opcode 판정 */
}

#ifdef CONFIG_NVME_VERBOSE_ERRORS
const char *nvme_get_error_status_str(u16 status);	/* [한국어] SCT/SC → 상세 에러 문자열 (constants.c) */
const char *nvme_get_opcode_str(u8 opcode);	/* [한국어] I/O opcode 이름 */
const char *nvme_get_admin_opcode_str(u8 opcode);	/* [한국어] Admin opcode 이름 */
const char *nvme_get_fabrics_opcode_str(u8 opcode);	/* [한국어] Fabrics fctype 이름 */
#else /* CONFIG_NVME_VERBOSE_ERRORS */
/* [한국어] verbose 비활성 빌드: 고정 짧은 문자열 — 바이너리 크기 절약 */
static inline const char *nvme_get_error_status_str(u16 status)
{
	return "I/O Error";	/* [한국어] 상태 코드 비해석 폴백 */
}
static inline const char *nvme_get_opcode_str(u8 opcode)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	return "I/O Cmd";	/* [한국어] I/O 명령 총칭 */
}
static inline const char *nvme_get_admin_opcode_str(u8 opcode)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	return "Admin Cmd";	/* [한국어] Admin 명령 총칭 */
}

static inline const char *nvme_get_fabrics_opcode_str(u8 opcode)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	return "Fabrics Cmd";	/* [한국어] Fabrics 명령 총칭 */
}
#endif /* CONFIG_NVME_VERBOSE_ERRORS */

/*
 * [한국어] nvme_opcode_str() — qid? I/O : Admin 문자열 선택 — 로그 공통 헬퍼
 */
static inline const char *nvme_opcode_str(int qid, u8 opcode)
{
	return qid ? nvme_get_opcode_str(opcode) :	/* [한국어] qid>0 → I/O opcode 이름 */
		nvme_get_admin_opcode_str(opcode);	/* [한국어] qid==0 → Admin opcode 이름 */
}

/*
 * [한국어] nvme_fabrics_opcode_str() — Fabrics 이면 fctype 이름, 아니면 opcode_str
 */
static inline const char *nvme_fabrics_opcode_str(
		int qid, const struct nvme_command *cmd)
{
	if (nvme_is_fabrics(cmd))	/* [한국어] 0x7f 이면 fctype 문자열 */
		return nvme_get_fabrics_opcode_str(cmd->fabrics.fctype);	/* [한국어] Connect/Property/Auth 이름 */

	return nvme_opcode_str(qid, cmd->common.opcode);	/* [한국어] 일반 Admin/I/O 이름 */
}

/*
 * [한국어] Error Information 로그 슬롯 — 실패 명령의 sqid/cid/status/lba/nsid
 * Get Log LID=01h. 진단·텔레메트리. 컨트롤러 작성 LE.
 */
struct nvme_error_slot {
	__le64		error_count;	/* [한국어] 에러 로그 슬롯 순번 LE64 */
	__le16		sqid;	/* [한국어] 실패 명령의 SQ ID */
	__le16		cmdid;	/* [한국어] 실패 명령의 CID */
	__le16		status_field;	/* [한국어] 실패한 명령 status */
	__le16		param_error_location;	/* [한국어] 잘못된 파라미터 바이트 오프셋 힌트 */
	__le64		lba;	/* [한국어] 관련 LBA */
	__le32		nsid;	/* [한국어] 관련 NSID */
	__u8		vs;	/* [한국어] Vendor Specific */
	__u8		resv[3];	/* [한국어] 예약 */
	__le64		cs;	/* [한국어] Command Specific 정보 */
	__u8		resv2[24];	/* [한국어] 예약 */
};

/*
 * [한국어] nvme_is_write() — 데이터 방향 추정
 * Fabrics 는 fctype LSB, 그 외 opcode LSB. DMA 매핑 방향·권한 판단에 사용.
 * 원본 영문 주석대로 Fabrics 가 방향 비트를 opcode 와 공유하지 않아 특수 분기.
 */
static inline bool nvme_is_write(const struct nvme_command *cmd)
{
	/*
	 * What a mess...
	 *
	 * Why can't we simply have a Fabrics In and Fabrics out command?
	 */
	if (unlikely(nvme_is_fabrics(cmd)))	/* [한국어] Fabrics 는 fctype 홀수/짝수로 방향 */
		return cmd->fabrics.fctype & 1;	/* [한국어] fctype LSB = 쓰기 계열 관례 */
	return cmd->common.opcode & 1;	/* [한국어] I/O/Admin opcode LSB = 쓰기 관례 */
}

/*
 * [한국어] CQE status 필드 해석 — SCT(타입) + SC(코드) + 플래그
 *
 * CQE.status 는 LE16. 호스트는 le16_to_cpu 후:
 *   SC  = bits 7:0   (NVME_SC_MASK)
 *   SCT = bits 10:8  (NVME_SCT_MASK) — Generic / Command Specific / Media / Path
 *   CRD = Command Retry Delay, MORE, DNR(Do Not Retry)
 * core 의 nvme_end_req / 재시도 로직: DNR=1 이면 재시도 금지, Path SCT 면 multipath
 * failover, Media 는 EIO 계열. 동일 숫자 SC 가 SCT 에 따라 의미가 다르고,
 * Fabrics Connect 오류는 Command Specific 공간에서 NVM 과 숫자가 겹치므로
 * 명령 문맥과 함께 해석해야 한다. fault_inject 도 이 코드/DNR 을 주입한다.
 *
 * 값 인코딩: 아래 enum 은 SCT 를 상위 비트에 합친 호스트 내부 표기(0x100=CS 등)를
 * 섞어 쓴다. 실제 온와이어 status 워드에서는 SCT/SC 가 분리 필드다.
 */
enum {
	/*
	 * Generic Command Status:
	 */
	NVME_SCT_GENERIC		= 0x0,	/* [한국어] SCT=0 Generic — 공통 명령 오류 클래스 */
	NVME_SC_SUCCESS			= 0x0,	/* [한국어] 성공 — 호스트는 데이터/결과를 신뢰 */
	NVME_SC_INVALID_OPCODE		= 0x1,	/* [한국어] 미지원/잘못된 opcode */
	NVME_SC_INVALID_FIELD		= 0x2,	/* [한국어] SQE 필드 값 오류 — param_error_location 참고 */
	NVME_SC_CMDID_CONFLICT		= 0x3,	/* [한국어] 동일 SQ 에 CID 충돌 */
	NVME_SC_DATA_XFER_ERROR		= 0x4,	/* [한국어] 데이터 전송 실패 */
	NVME_SC_POWER_LOSS		= 0x5,	/* [한국어] 전력 손실 관련 중단 */
	NVME_SC_INTERNAL		= 0x6,	/* [한국어] 컨트롤러 내부 오류 */
	NVME_SC_ABORT_REQ		= 0x7,	/* [한국어] Abort 명령으로 중단됨 */
	NVME_SC_ABORT_QUEUE		= 0x8,	/* [한국어] 큐 삭제 등으로 일괄 중단 */
	NVME_SC_FUSED_FAIL		= 0x9,	/* [한국어] 퓨즈 쌍 중 하나 실패 */
	NVME_SC_FUSED_MISSING		= 0xa,	/* [한국어] 퓨즈 쌍 누락 */
	NVME_SC_INVALID_NS		= 0xb,	/* [한국어] 잘못된 NSID */
	NVME_SC_CMD_SEQ_ERROR		= 0xc,	/* [한국어] 명령 시퀀스 위반 */
	NVME_SC_SGL_INVALID_LAST	= 0xd,	/* [한국어] SGL last 세그먼트 오류 */
	NVME_SC_SGL_INVALID_COUNT	= 0xe,	/* [한국어] SGL 디스크립터 개수 오류 */
	NVME_SC_SGL_INVALID_DATA	= 0xf,	/* [한국어] SGL 데이터 디스크립터 오류 */
	NVME_SC_SGL_INVALID_METADATA	= 0x10,	/* [한국어] SGL 메타데이터 오류 */
	NVME_SC_SGL_INVALID_TYPE	= 0x11,	/* [한국어] SGL 타입 오류 */
	NVME_SC_CMB_INVALID_USE		= 0x12,	/* [한국어] CMB 사용 위반 */
	NVME_SC_PRP_INVALID_OFFSET	= 0x13,	/* [한국어] PRP 오프셋 정렬/범위 오류 */
	NVME_SC_ATOMIC_WU_EXCEEDED	= 0x14,	/* [한국어] 원자 쓰기 단위 초과 */
	NVME_SC_OP_DENIED		= 0x15,	/* [한국어] 연산 거부(권한/정책) */
	NVME_SC_SGL_INVALID_OFFSET	= 0x16,	/* [한국어] SGL 오프셋 오류 */
	NVME_SC_RESERVED		= 0x17,	/* [한국어] 예약 코드 */
	NVME_SC_HOST_ID_INCONSIST	= 0x18,	/* [한국어] Host ID 불일치 */
	NVME_SC_KA_TIMEOUT_EXPIRED	= 0x19,	/* [한국어] Keep Alive 만료 — 연결 위험 */
	NVME_SC_KA_TIMEOUT_INVALID	= 0x1A,	/* [한국어] KATO 값 부당 */
	NVME_SC_ABORTED_PREEMPT_ABORT	= 0x1B,	/* [한국어] Preempt and Abort 로 중단 */
	NVME_SC_SANITIZE_FAILED		= 0x1C,	/* [한국어] Sanitize 실패 상태 고착 가능 */
	NVME_SC_SANITIZE_IN_PROGRESS	= 0x1D,	/* [한국어] Sanitize 진행 중 */
	NVME_SC_SGL_INVALID_GRANULARITY	= 0x1E,	/* [한국어] SGL 정렬 granularity 위반 */
	NVME_SC_CMD_NOT_SUP_CMB_QUEUE	= 0x1F,	/* [한국어] CMB 큐에서 미지원 명령 */
	NVME_SC_NS_WRITE_PROTECTED	= 0x20,	/* [한국어] NS 쓰기 보호 */
	NVME_SC_CMD_INTERRUPTED		= 0x21,	/* [한국어] 명령 인터럽트됨 */
	NVME_SC_TRANSIENT_TR_ERR	= 0x22,	/* [한국어] 일시 전송 오류 — 재시도 후보 */
	NVME_SC_ADMIN_COMMAND_MEDIA_NOT_READY = 0x24,	/* [한국어] 미디어 미준비 — CRIME 경로 관련 */
	NVME_SC_INVALID_IO_CMD_SET	= 0x2C,	/* [한국어] 선택된 I/O 커맨드셋 부당 */

	NVME_SC_LBA_RANGE		= 0x80,	/* [한국어] LBA 범위 초과 */
	NVME_SC_CAP_EXCEEDED		= 0x81,	/* [한국어] 용량 초과 */
	NVME_SC_NS_NOT_READY		= 0x82,	/* [한국어] NS 미준비 — 재시도 후보 */
	NVME_SC_RESERVATION_CONFLICT	= 0x83,	/* [한국어] 예약 충돌 — EBADE 계열 */
	NVME_SC_FORMAT_IN_PROGRESS	= 0x84,	/* [한국어] Format 진행 중 */

	/*
	 * Command Specific Status:
	 */
	NVME_SCT_COMMAND_SPECIFIC	= 0x100,	/* [한국어] SCT=1 Command Specific 베이스 */
	NVME_SC_CQ_INVALID		= 0x100,	/* [한국어] CQ ID 무효 */
	NVME_SC_QID_INVALID		= 0x101,	/* [한국어] 큐 ID 무효 */
	NVME_SC_QUEUE_SIZE		= 0x102,	/* [한국어] 큐 크기 부당 */
	NVME_SC_ABORT_LIMIT		= 0x103,	/* [한국어] Abort 한도 초과 */
	NVME_SC_ABORT_MISSING		= 0x104,	/* [한국어] Abort 대상 없음 */
	NVME_SC_ASYNC_LIMIT		= 0x105,	/* [한국어] AER 한도 초과 */
	NVME_SC_FIRMWARE_SLOT		= 0x106,	/* [한국어] FW 슬롯 오류 */
	NVME_SC_FIRMWARE_IMAGE		= 0x107,	/* [한국어] FW 이미지 오류 */
	NVME_SC_INVALID_VECTOR		= 0x108,	/* [한국어] IRQ 벡터 부당 */
	NVME_SC_INVALID_LOG_PAGE	= 0x109,	/* [한국어] 로그 페이지 무효 */
	NVME_SC_INVALID_FORMAT		= 0x10a,	/* [한국어] Format 인자 부당 */
	NVME_SC_FW_NEEDS_CONV_RESET	= 0x10b,	/* [한국어] FW 활성에 전통 리셋 필요 */
	NVME_SC_INVALID_QUEUE		= 0x10c,	/* [한국어] 큐 상태 무효 */
	NVME_SC_FEATURE_NOT_SAVEABLE	= 0x10d,	/* [한국어] Feature 저장 불가 */
	NVME_SC_FEATURE_NOT_CHANGEABLE	= 0x10e,	/* [한국어] Feature 변경 불가 */
	NVME_SC_FEATURE_NOT_PER_NS	= 0x10f,	/* [한국어] NS 단위 Feature 아님 */
	NVME_SC_FW_NEEDS_SUBSYS_RESET	= 0x110,	/* [한국어] 서브시스템 리셋 필요 */
	NVME_SC_FW_NEEDS_RESET		= 0x111,	/* [한국어] 컨트롤러 리셋 필요 */
	NVME_SC_FW_NEEDS_MAX_TIME	= 0x112,	/* [한국어] FW 활성 시간 초과 위험 */
	NVME_SC_FW_ACTIVATE_PROHIBITED	= 0x113,	/* [한국어] FW 활성 금지 상태 */
	NVME_SC_OVERLAPPING_RANGE	= 0x114,	/* [한국어] 범위 겹침 */
	NVME_SC_NS_INSUFFICIENT_CAP	= 0x115,	/* [한국어] NS 용량 부족 */
	NVME_SC_NS_ID_UNAVAILABLE	= 0x116,	/* [한국어] NSID 고갈 */
	NVME_SC_NS_ALREADY_ATTACHED	= 0x118,	/* [한국어] 이미 attach 됨 */
	NVME_SC_NS_IS_PRIVATE		= 0x119,	/* [한국어] 사설 NS — attach 거부 */
	NVME_SC_NS_NOT_ATTACHED		= 0x11a,	/* [한국어] attach 되지 않음 */
	NVME_SC_THIN_PROV_NOT_SUPP	= 0x11b,	/* [한국어] 씬 프로비저닝 미지원 */
	NVME_SC_CTRL_LIST_INVALID	= 0x11c,	/* [한국어] 컨트롤러 리스트 무효 */
	NVME_SC_SELF_TEST_IN_PROGRESS	= 0x11d,	/* [한국어] 셀프테스트 진행 중 */
	NVME_SC_BP_WRITE_PROHIBITED	= 0x11e,	/* [한국어] Boot Partition 쓰기 금지 */
	NVME_SC_CTRL_ID_INVALID		= 0x11f,	/* [한국어] Controller ID 무효 */
	NVME_SC_SEC_CTRL_STATE_INVALID	= 0x120,	/* [한국어] Secondary Controller 상태 무효 */
	NVME_SC_CTRL_RES_NUM_INVALID	= 0x121,	/* [한국어] 컨트롤러 자원 수 무효 */
	NVME_SC_RES_ID_INVALID		= 0x122,	/* [한국어] 자원 ID 무효 */
	NVME_SC_PMR_SAN_PROHIBITED	= 0x123,	/* [한국어] PMR sanitize 금지 */
	NVME_SC_ANA_GROUP_ID_INVALID	= 0x124,	/* [한국어] ANA 그룹 ID 무효 */
	NVME_SC_ANA_ATTACH_FAILED	= 0x125,	/* [한국어] ANA attach 실패 */

	/*
	 * I/O Command Set Specific - NVM commands:
	 */
	NVME_SC_BAD_ATTRIBUTES		= 0x180,	/* [한국어] NVM 속성 오류 (동일 숫자 Fabrics 와 문맥 분리) */
	NVME_SC_INVALID_PI		= 0x181,	/* [한국어] PI 설정 오류 */
	NVME_SC_READ_ONLY		= 0x182,	/* [한국어] 읽기 전용 위반 */
	NVME_SC_CMD_SIZE_LIM_EXCEEDED	= 0x183,	/* [한국어] 명령 크기 한도 초과 */

	/*
	 * I/O Command Set Specific - Fabrics commands:
	 */
	NVME_SC_CONNECT_FORMAT		= 0x180,	/* [한국어] Connect 기록 형식 오류 (Fabrics 문맥) */
	NVME_SC_CONNECT_CTRL_BUSY	= 0x181,	/* [한국어] Connect 시 컨트롤러 바쁨 */
	NVME_SC_CONNECT_INVALID_PARAM	= 0x182,	/* [한국어] Connect 파라미터 무효 */
	NVME_SC_CONNECT_RESTART_DISC	= 0x183,	/* [한국어] 디스커버리 재시작 필요 */
	NVME_SC_CONNECT_INVALID_HOST	= 0x184,	/* [한국어] Host NQN/권한 거부 */

	NVME_SC_DISCOVERY_RESTART	= 0x190,	/* [한국어] Discovery 로그 재시작 */
	NVME_SC_AUTH_REQUIRED		= 0x191,	/* [한국어] 인증 필요 — Auth 시퀀스 선행 */

	/*
	 * I/O Command Set Specific - Zoned commands:
	 */
	NVME_SC_ZONE_BOUNDARY_ERROR	= 0x1b8,	/* [한국어] 존 경계 위반 */
	NVME_SC_ZONE_FULL		= 0x1b9,	/* [한국어] 존 Full — append/write 거부 */
	NVME_SC_ZONE_READ_ONLY		= 0x1ba,	/* [한국어] 존 읽기 전용 */
	NVME_SC_ZONE_OFFLINE		= 0x1bb,	/* [한국어] 존 Offline */
	NVME_SC_ZONE_INVALID_WRITE	= 0x1bc,	/* [한국어] 존 쓰기 규칙 위반(순차 등) */
	NVME_SC_ZONE_TOO_MANY_ACTIVE	= 0x1bd,	/* [한국어] Active 존 자원 초과 */
	NVME_SC_ZONE_TOO_MANY_OPEN	= 0x1be,	/* [한국어] Open 존 자원 초과 */
	NVME_SC_ZONE_INVALID_TRANSITION	= 0x1bf,	/* [한국어] 불법 존 상태 전이 */

	/*
	 * Media and Data Integrity Errors:
	 */
	NVME_SCT_MEDIA_ERROR		= 0x200,	/* [한국어] SCT=2 Media and Data Integrity */
	NVME_SC_WRITE_FAULT		= 0x280,	/* [한국어] 미디어 쓰기 고장 */
	NVME_SC_READ_ERROR		= 0x281,	/* [한국어] 미디어 읽기 오류 */
	NVME_SC_GUARD_CHECK		= 0x282,	/* [한국어] PI Guard 불일치 */
	NVME_SC_APPTAG_CHECK		= 0x283,	/* [한국어] PI App Tag 불일치 */
	NVME_SC_REFTAG_CHECK		= 0x284,	/* [한국어] PI Ref Tag 불일치 */
	NVME_SC_COMPARE_FAILED		= 0x285,	/* [한국어] Compare 불일치 */
	NVME_SC_ACCESS_DENIED		= 0x286,	/* [한국어] 접근 거부 */
	NVME_SC_UNWRITTEN_BLOCK		= 0x287,	/* [한국어] 미기록 블록 읽기 */

	/*
	 * Path-related Errors:
	 */
	NVME_SCT_PATH			= 0x300,	/* [한국어] SCT=3 Path 관련 — multipath failover 힌트 */
	NVME_SC_INTERNAL_PATH_ERROR	= 0x300,	/* [한국어] 경로 내부 오류 */
	NVME_SC_ANA_PERSISTENT_LOSS	= 0x301,	/* [한국어] ANA 영구 손실 — 경로 폐기 */
	NVME_SC_ANA_INACCESSIBLE	= 0x302,	/* [한국어] ANA 접근 불가 — 다른 경로 사용 */
	NVME_SC_ANA_TRANSITION		= 0x303,	/* [한국어] ANA 전이 중 — 재시도/대기 */
	NVME_SC_CTRL_PATH_ERROR		= 0x360,	/* [한국어] 컨트롤러 경로 오류 */
	NVME_SC_HOST_PATH_ERROR		= 0x370,	/* [한국어] 호스트 측 경로 오류 */
	NVME_SC_HOST_ABORTED_CMD	= 0x371,	/* [한국어] 호스트가 명령 중단 */

	NVME_SC_MASK			= 0x00ff, /* Status Code */	/* [한국어] status 하위 8비트 SC 마스크 */
	NVME_SCT_MASK			= 0x0700, /* Status Code Type */	/* [한국어] SCT 비트 마스크 (status 내) */
	NVME_SCT_SC_MASK		= NVME_SCT_MASK | NVME_SC_MASK,	/* [한국어] SCT|SC 결합 마스크 */

	NVME_STATUS_CRD			= 0x1800, /* Command Retry Delayed */	/* [한국어] Command Retry Delay — 지연 후 재시도 힌트 */
	NVME_STATUS_MORE		= 0x2000,	/* [한국어] More 비트 — 추가 상태/로그 존재 */
	NVME_STATUS_DNR			= 0x4000, /* Do Not Retry */	/* [한국어] Do Not Retry — 재시도 금지; 즉시 실패 처리 */
};

#define NVME_SCT(status) ((status) >> 8 & 7)	/* [한국어] status 워드에서 SCT 3비트 추출 — 에러 클래스 분기용 */

/*
 * [한국어] 16바이트 Completion Queue Entry (CQE)
 *
 * 컨트롤러가 작성, 호스트 ISR/폴링이 소비. 레이아웃:
 *   result(8B) | sq_head(2) | sq_id(2) | command_id(2) | status(2)
 * phase 비트는 status 의 bit0 로 토글되어 링 랩 감지.
 * 호스트는 command_id 로 요청 매칭, status 로 성공/재시도/경로오류 분기,
 * sq_head 로 SQ 슬롯 reclaim. doorbell CQ head 갱신으로 소비 완료 통지.
 */
struct nvme_completion {
	/*
	 * Used by Admin and Fabrics commands to return data:
	 */
	union nvme_result {	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
		__le16	u16;	/* [한국어] 결과 16비트 뷰 — Get Features 등 */
		__le32	u32;	/* [한국어] 결과 32비트 뷰 */
		__le64	u64;	/* [한국어] 결과 64비트 뷰 — Connect cntlid 등 */
	} result;	/* [한국어] 명령 결과 — Admin/Fabrics 가 값 반환 (컨트롤러 LE) */
	__le16	sq_head;	/* how much of this queue may be reclaimed */	/* [한국어] SQ Head — 호스트 reclaim 한계 (컨트롤러) */
	__le16	sq_id;		/* submission queue that generated this entry */	/* [한국어] 완료를 생성한 SQ ID (컨트롤러) */
	__u16	command_id;	/* of the command which completed */	/* [한국어] 원 명령 CID 반사 — 요청 매칭 */
	__le16	status;		/* did the command fail, and if so, why? */	/* [한국어] SCT|SC|phase|DNR|MORE (컨트롤러 LE16) */
};

/*
 * [한국어] VS 레지스터 버전 팩/언팩
 * 컨트롤러 VS 와 Identify.ver 이 동일 인코딩. 호스트 호환성 분기·sysfs 표시에 사용.
 */
#define NVME_VS(major, minor, tertiary) \
	(((major) << 16) | ((minor) << 8) | (tertiary))	/* [한국어] major<<16 | minor<<8 | tertiary */

#define NVME_MAJOR(ver)		((ver) >> 16)	/* [한국어] 버전 major 추출 */
#define NVME_MINOR(ver)		(((ver) >> 8) & 0xff)	/* [한국어] 버전 minor 추출 */
#define NVME_TERTIARY(ver)	((ver) & 0xff)	/* [한국어] 버전 tertiary 추출 */

enum {
	NVME_AEN_RESV_LOG_PAGE_AVALIABLE	= 0x00,	/* [한국어] NVME_AEN_RESV_LOG_PAGE_AVALIABLE 상수 — 상위 enum 역할 참고 */
};

enum {
	NVME_PR_LOG_EMPTY_LOG_PAGE			= 0x00,	/* [한국어] NVME_PR_LOG_EMPTY_LOG_PAGE 상수 — 상위 enum 역할 참고 */
	NVME_PR_LOG_REGISTRATION_PREEMPTED		= 0x01,	/* [한국어] NVME_PR_LOG_REGISTRATION_PREEMPTED 상수 — 상위 enum 역할 참고 */
	NVME_PR_LOG_RESERVATION_RELEASED		= 0x02,	/* [한국어] NVME_PR_LOG_RESERVATION_RELEASED 상수 — 상위 enum 역할 참고 */
	NVME_PR_LOG_RESERVATOIN_PREEMPTED		= 0x03,	/* [한국어] NVME_PR_LOG_RESERVATOIN_PREEMPTED 상수 — 상위 enum 역할 참고 */
};

enum {
	NVME_PR_NOTIFY_BIT_REG_PREEMPTED		= 1,	/* [한국어] NVME_PR_NOTIFY_BIT_REG_PREEMPTED 상수 — 상위 enum 역할 참고 */
	NVME_PR_NOTIFY_BIT_RESV_RELEASED		= 2,	/* [한국어] NVME_PR_NOTIFY_BIT_RESV_RELEASED 상수 — 상위 enum 역할 참고 */
	NVME_PR_NOTIFY_BIT_RESV_PREEMPTED		= 3,	/* [한국어] NVME_PR_NOTIFY_BIT_RESV_PREEMPTED 상수 — 상위 enum 역할 참고 */
};

struct nvme_pr_log {
	__le64			count;	/* [한국어] count 필드 — 상위 구조 작성자·동기화 참고 */
	__u8			type;	/* [한국어] type 필드 — 상위 구조 작성자·동기화 참고 */
	__u8			nr_pages;	/* [한국어] nr_pages 필드 — 상위 구조 작성자·동기화 참고 */
	__u8			rsvd1[2];	/* [한국어] 스펙 예약 — 호스트 0 */
	__le32			nsid;	/* [한국어] nsid 필드 — 상위 구조 작성자·동기화 참고 */
	__u8			rsvd2[48];	/* [한국어] 스펙 예약 — 호스트 0 */
};

struct nvmet_pr_register_data {
	__le64	crkey;	/* [한국어] crkey 필드 — 상위 구조 작성자·동기화 참고 */
	__le64	nrkey;	/* [한국어] nrkey 필드 — 상위 구조 작성자·동기화 참고 */
};

struct nvmet_pr_acquire_data {
	__le64	crkey;	/* [한국어] crkey 필드 — 상위 구조 작성자·동기화 참고 */
	__le64	prkey;	/* [한국어] prkey 필드 — 상위 구조 작성자·동기화 참고 */
};

struct nvmet_pr_release_data {
	__le64	crkey;	/* [한국어] crkey 필드 — 상위 구조 작성자·동기화 참고 */
};

/*
 * [한국어] Identify rescap 비트 — 지원 PR 타입. 호스트가 지원 예약 유형 해석.
 */
enum nvme_pr_capabilities {
	NVME_PR_SUPPORT_PTPL				= 1,	/* [한국어] Persist Through Power Loss 지원 */
	NVME_PR_SUPPORT_WRITE_EXCLUSIVE			= 1 << 1,	/* [한국어] 쓰기 배타 지원 */
	NVME_PR_SUPPORT_EXCLUSIVE_ACCESS		= 1 << 2,	/* [한국어] 완전 배타 지원 */
	NVME_PR_SUPPORT_WRITE_EXCLUSIVE_REG_ONLY	= 1 << 3,	/* [한국어] 쓰기 배타(등록자만) 지원 */
	NVME_PR_SUPPORT_EXCLUSIVE_ACCESS_REG_ONLY	= 1 << 4,	/* [한국어] 완전 배타(등록자만) 지원 */
	NVME_PR_SUPPORT_WRITE_EXCLUSIVE_ALL_REGS	= 1 << 5,	/* [한국어] 쓰기 배타(전체 등록자) 지원 */
	NVME_PR_SUPPORT_EXCLUSIVE_ACCESS_ALL_REGS	= 1 << 6,	/* [한국어] 완전 배타(전체 등록자) 지원 */
	NVME_PR_SUPPORT_IEKEY_VER_1_3_DEF		= 1 << 7,	/* [한국어] Ignore Existing Key 지원(스펙 1.3) */
};

enum nvme_pr_register_action {
	NVME_PR_REGISTER_ACT_REG		= 0,	/* [한국어] 키 등록 */
	NVME_PR_REGISTER_ACT_UNREG		= 1,	/* [한국어] 키 해제 */
	NVME_PR_REGISTER_ACT_REPLACE		= 1 << 1,	/* [한국어] 키 교체 */
};

enum nvme_pr_acquire_action {
	NVME_PR_ACQUIRE_ACT_ACQUIRE		= 0,	/* [한국어] 예약 획득 */
	NVME_PR_ACQUIRE_ACT_PREEMPT		= 1,	/* [한국어] 선점 */
	NVME_PR_ACQUIRE_ACT_PREEMPT_AND_ABORT	= 1 << 1,	/* [한국어] 선점+명령 abort */
};

enum nvme_pr_release_action {
	NVME_PR_RELEASE_ACT_RELEASE		= 0,	/* [한국어] 예약 해제 */
	NVME_PR_RELEASE_ACT_CLEAR		= 1,	/* [한국어] 예약/등록 clear */
};

enum nvme_pr_change_ptpl {
	NVME_PR_CPTPL_NO_CHANGE			= 0,	/* [한국어] PTPL 상태 변경 없음 */
	NVME_PR_CPTPL_RESV			= 1 << 30,	/* [한국어] 예약을 PTPL 로 */
	NVME_PR_CPTPL_CLEARED			= 2 << 30,	/* [한국어] PTPL 클리어 */
	NVME_PR_CPTPL_PERSIST			= 3 << 30,	/* [한국어] 전원 손실 후에도 유지 */
};

#define NVME_PR_IGNORE_KEY (1 << 3)	/* [한국어] 예약 명령에서 기존 키 검사 생략 — 관리 경로 주의해 사용 */

/* Section 8.3.4.5.2 of the NVMe 2.1 */
#define NVME_AUTH_DHCHAP_MAX_HASH_IDS 30	/* [한국어] DH-HMAC-CHAP 해시 ID 목록 최대 개수 */
#define NVME_AUTH_DHCHAP_MAX_DH_IDS 30	/* [한국어] DH-HMAC-CHAP DH 그룹 ID 목록 최대 개수 */

/*
 * [한국어] 헤더 가드 종료
 * 본 파일의 모든 레이아웃은 스펙 온와이어 계약이다. 필드 추가·패딩 변경 금지.
 * 런타임 정책·큐 상태·요청 수명은 drivers/nvme/host/nvme.h 와 core.c 를 본다.
 *
 * 학습 체크리스트 (이 헤더만으로 검증 가능):
 *  - CAP→AQA/ASQ/ACQ→CC.EN→CSTS.RDY enable 시퀀스 필드가 모두 있는가
 *  - SQE 64B / CQE 16B (IOSQES=6, IOCQES=4) 계약
 *  - Admin vs I/O opcode 분리, Fabrics 0x7f+fctype
 *  - Identify CNS/CSI 와 id_ctrl/id_ns 레이아웃
 *  - PRP(prp1/prp2) vs SGL(PSDT) 선택
 *  - status SCT/SC/DNR 와 호스트 재시도/multipath 분기
 *  - NQN/Discovery 상수와 Connect 데이터
 */
#endif /* _LINUX_NVME_H */
