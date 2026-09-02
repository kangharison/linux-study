/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2011-2014, Intel Corporation.
 */

/*
 * [한국어 설명] NVMe host 서브시스템 내부 공통 헤더 (drivers/nvme/host/nvme.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 host 트리(core/pci/tcp/rdma/fc/apple/multipath/ioctl/sysfs 등)가
 * 공유하는 "내부 객체 모델 + 트랜스포트 추상화 + 상태 머신 API 표면"이다.
 * include/linux/nvme.h 가 스펙 on-the-wire 구조체(Identify/SQE/CQE/로그 페이지)
 * 와 공개 상수(opcode, status, CAP/CC/CSTS 비트)를 담는다면, 본 파일은 그
 * 스펙 타입을 커널 런타임 객체(struct nvme_ctrl / nvme_ns / nvme_ns_head /
 * nvme_request / nvme_subsystem)에 매핑하고, blk-mq tag↔Command Identifier
 * (CID) 인코딩, 컨트롤러 상태 전이, multipath 경로 선택, fabrics 재연결,
 * keep-alive, AER, passthrough ioctl, ZNS, Opal, DH-HMAC-CHAP 인증 등의
 * 호스트 전용 정책을 선언한다. 즉 "무엇을 보낼지(스펙)"가 아니라
 * "어떻게 호스트 소프트웨어 스택을 구성할지"의 중심 계약서다.
 *
 * === 객체 모델 (누가 누구를 소유하는가) ===
 *   nvme_subsystem  ──1:N──► nvme_ctrl (동일 subnqn 의 컨트롤러 인스턴스들)
 *         │                      │
 *         │                      ├─ admin_q / connect_q / fabrics_q
 *         │                      ├─ tagset(IO) + admin_tagset
 *         │                      └─ namespaces 리스트 ──► nvme_ns (경로별 NS)
 *         │                                                   │
 *         └─ nsheads 리스트 ──► nvme_ns_head (서브시스템 전역 NS 앵커)
 *                                      ▲
 *                                      │ head 포인터 / siblings 리스트
 *                                      └── 여러 nvme_ns 가 동일 head 에 연결
 *                                          (shared NS + multipath 시 1:N,
 *                                           private NS 는 보통 1:1)
 *
 *   blk-mq request 의 PDU(첫 멤버) = struct nvme_request
 *     → cmd/result/status/genctr/retries/flags/ctrl 를 핫패스에서 접근
 *     → CID = (genctr 하위 4비트 << 12) | tag(12비트) 로 장치 CQE 와 매칭
 *
 * === 전체 아키텍처에서의 위치 ===
 * 상층: 블록 계층(gendisk, request_queue, blk-mq, bio, pr_ops, sed-opal)과
 *       캐릭터 디바이스(/dev/nvmeX, /dev/ngXnY) ioctl·uring 경로.
 * 중층: 본 헤더가 선언한 core API — nvme_init_ctrl/start/stop/reset/delete,
 *       nvme_setup_cmd, nvme_try_complete_req, nvme_change_ctrl_state 등.
 * 하층: 트랜스포트별 nvme_ctrl_ops 구현체
 *       - pci.c   : MMIO CAP/CC/CSTS, SQ/CQ doorbell, MSI-X, PRP/SGL
 *       - tcp.c   / rdma.c / fc.c : fabrics Connect, keep-alive, 재연결
 *       - apple.c : Apple ANS 컨트롤러 변형
 * 실행 컨텍스트 구분:
 *   - I/O 제출: blk-mq ->queue_rq (원자/softirq 가능; fabrics 는 BLOCKING 플래그)
 *   - 완료: IRQ/softirq → nvme_try_complete_req → nvme_complete_rq
 *   - 제어: nvme_wq / nvme_reset_wq / nvme_delete_wq 워커 (sleep 가능)
 *
 * === 타 모듈과의 연결 ===
 * - include/linux/nvme.h : 스펙 구조체·상수. 본 파일이 첫 include 로 끌어온다.
 * - drivers/nvme/host/core.c : 상태 머신·Identify·scan·AER·keep-alive·
 *   tag set 할당·namespace 생명주기·sync 제출 구현의 본체.
 * - drivers/nvme/host/pci.c : PCIe 프로브/리셋/큐 생성, quirks 테이블 적용.
 * - multipath.c : ANA 로그, current_path[], failover, iopolicy.
 * - fabrics.c + fabrics.h : nvmf_ctrl_options, 재연결 정책, connect 큐.
 * - ioctl.c / sysfs.c / pr.c / zns.c / hwmon.c / auth.c / fault_inject.c :
 *   본 헤더 선언의 소비자·구현자.
 * - block/blk-mq.h : tag, request PDU, complete_remote, freeze/quiesce.
 * - block/sed-opal.c : ctrl->opal_dev 와 Security Send/Receive 경로.
 * - include/linux/t10-pi.h : Protection Information (PI) 메타데이터.
 *
 * === 컨트롤러 상태 머신 (요약) ===
 *   NEW → CONNECTING → LIVE ⇄ RESETTING/CONNECTING
 *                     ↘ DELETING → DELETING_NOIO → DEAD
 * nvme_change_ctrl_state() 가 합법 전이만 허용하고 state_wq 를 깨운다.
 * LIVE 가 아니면 nvme_check_ready() 가 제출을 거절하거나 fabrics 삭제 중
 * 일부 큐잉을 허용한다. 터미널(DELETING*)에서는 다시 LIVE 로 복귀 불가.
 *
 * === 주요 심볼 요약 ===
 * struct nvme_ctrl / nvme_ctrl_ops — 컨트롤러 + 트랜스포트 vtable
 * struct nvme_ns / nvme_ns_head / nvme_subsystem — NS·멀티패스·서브시스템
 * struct nvme_request — blk-mq PDU; CID genctr 포함
 * enum nvme_quirks / nvme_ctrl_state / nvme_ctrl_flags — 비표준 보정·상태
 * nvme_wq/reset_wq/delete_wq — 스캔/리셋/삭제 직렬화 워커 큐
 * nvme_cid / nvme_find_rq / nvme_try_complete_req — 핫패스 완료 경로
 * nvme_*_tag_set, freeze/quiesce, multipath/auth/hwmon 선언
 */

#ifndef _NVME_H	/* [한국어] 인클루드 가드 — 이 내부 헤더 중복 포함 방지 */
#define _NVME_H		/* [한국어] 가드 심볼 정의 — 번역 단위당 한 번만 본문 노출 */

#include <linux/nvme.h>		/* [한국어] 스펙 on-wire 타입·opcode·status·Identify/CAP/CC 상수 — host 객체 필드가 참조하는 어휘 원천 */
#include <linux/cdev.h>		/* [한국어] struct cdev — /dev/nvmeX 컨트롤러·NS 캐릭터 노드 등록에 사용 */
#include <linux/pci.h>		/* [한국어] PCI 타입·헬퍼 — PCIe 트랜스포트·일부 quirk 판별 경로와 공유 헤더 의존 */
#include <linux/kref.h>		/* [한국어] struct kref — subsystem/ns_head/ns 수명 관리(마지막 put 시 해제) */
#include <linux/blk-mq.h>	/* [한국어] blk-mq request/tag_set/queue — I/O·admin 제출·완료의 블록 계층 기반 */
#include <linux/sed-opal.h>	/* [한국어] struct opal_dev — TCG Opal SED 세션 컨텍스트를 ctrl 에 연결 */
#include <linux/fault-inject.h>	/* [한국어] fault_attr — 디버그용 인위적 status 주입 인프라 */
#include <linux/rcupdate.h>	/* [한국어] RCU — multipath current_path[] 등 경로 포인터 읽기 측 보호 */
#include <linux/wait.h>		/* [한국어] wait_queue_head_t — state_wq 등 상태 전이 대기 */
#include <linux/t10-pi.h>	/* [한국어] T10 Protection Information — ns_head pi_type/guard 와 무결성 경로 */
#include <linux/ratelimit_types.h> /* [한국어] ratelimit_state — nuse 등 반복 경고 로그 억제 */

#include <trace/events/block.h>	/* [한국어] trace_block_bio_complete — multipath 완료 트레이스 훅 */

extern const struct pr_ops nvme_pr_ops;	/* [한국어] nvme_pr_ops 필드 — 상위 구조 작성자·동기화 참고 */
/* [한국어] 블록 계층 Persistent Reservation 콜백 테이블(pr.c 구현).
 * gendisk->fops->pr_ops 에 연결되어 RESERVE/RELEASE 등을 NVMe Reservation
 * 명령으로 변환한다. 설정: pr.c; 소비: 블록 pr ioctl 경로. */

extern unsigned int nvme_io_timeout;	/* [한국어] nvme_io_timeout 필드 — 상위 구조 작성자·동기화 참고 */
/* [한국어] I/O 명령 타임아웃(초 단위 모듈 파라미터). 핫패스에서는 HZ 환산 매크로로 사용. */
#define NVME_IO_TIMEOUT	(nvme_io_timeout * HZ)	/* [한국어] NVME_IO_TIMEOUT 매크로 — 상위 섹션 계약 참고 */
/* [한국어] jiffies 단위 I/O 타임아웃 — blk-mq 요청 타이머·abort 판단 기준. */

extern unsigned int admin_timeout;	/* [한국어] admin_timeout 필드 — 상위 구조 작성자·동기화 참고 */
/* [한국어] Admin 명령 타임아웃(초). Identify/Set Features 등 제어 평면용. */
#define NVME_ADMIN_TIMEOUT	(admin_timeout * HZ)	/* [한국어] NVME_ADMIN_TIMEOUT 매크로 — 상위 섹션 계약 참고 */
/* [한국어] jiffies 단위 admin 타임아웃 — 초기화·ioctl 동기 제출에 적용. */

#define NVME_DEFAULT_KATO	5	/* [한국어] NVME_DEFAULT_KATO 매크로 — 상위 섹션 계약 참고 */
/* [한국어] Keep Alive Timeout 기본값(초). fabrics 연결 유지 하트비트 주기 기준으로
 * KATO feature 가 명시되지 않았을 때 core 가 채택하는 스펙 권장 근처 값. */

#ifdef CONFIG_ARCH_NO_SG_CHAIN	/* [한국어] SG 체이닝 미지원 아키텍처 분기 */
#define  NVME_INLINE_SG_CNT  0	/* [한국어] NVME_INLINE_SG_CNT 매크로 — 상위 섹션 계약 참고 */
/* [한국어] SG 체이닝 불가 아키텍처: 요청 인라인에 추가 SG 슬롯을 두지 않음. */
#define  NVME_INLINE_METADATA_SG_CNT  0	/* [한국어] NVME_INLINE_METADATA_SG_CNT 매크로 — 상위 섹션 계약 참고 */
/* [한국어] 메타데이터(PI)용 인라인 SG 도 0 — 별도 할당 경로만 사용. */
#else	/* [한국어] 일반 아키텍처: 인라인 SG 로 소형 I/O 할당 절감 */
#define  NVME_INLINE_SG_CNT  2	/* [한국어] NVME_INLINE_SG_CNT 매크로 — 상위 섹션 계약 참고 */
/* [한국어] 일반 아키텍처: 요청 PDU 옆에 소형 I/O 용 인라인 SG 2개 — 작은 I/O 할당 절감. */
#define  NVME_INLINE_METADATA_SG_CNT  1	/* [한국어] NVME_INLINE_METADATA_SG_CNT 매크로 — 상위 섹션 계약 참고 */
/* [한국어] 메타데이터 디스크립터용 인라인 SG 1개 — 분리 메타데이터 전송 최적화. */
#endif	/* [한국어] CONFIG_ARCH_NO_SG_CHAIN 끝 */

/*
 * Default to a 4K page size, with the intention to update this
 * path in the future to accommodate architectures with differing
 * kernel and IO page sizes.
 */
#define NVME_CTRL_PAGE_SHIFT	12	/* [한국어] NVME_CTRL_PAGE_SHIFT 매크로 — 상위 섹션 계약 참고 */
/* [한국어] NVMe PRP/페이지 정렬 가정: 2^12=4KiB. 스펙 최소 MPS 및 호스트 기본 정렬. */
#define NVME_CTRL_PAGE_SIZE	(1 << NVME_CTRL_PAGE_SHIFT)	/* [한국어] NVME_CTRL_PAGE_SIZE 매크로 — 상위 섹션 계약 참고 */
/* [한국어] 4KiB 컨트롤러 페이지 — virt boundary, PRP 리스트 경계, DMA 매핑 정렬에 사용. */

extern struct workqueue_struct *nvme_wq;	/* [한국어] nvme_wq 필드 — 상위 구조 작성자·동기화 참고 */
/* [한국어] 일반 host 작업 큐: namespace scan, AER 처리, fw activate, ANA 갱신 등.
 * 리셋/삭제와 분리해 장시간 작업이 삭제 경로를 굶기지 않게 한다. core 모듈 초기화에서 생성. */
extern struct workqueue_struct *nvme_reset_wq;	/* [한국어] nvme_reset_wq 필드 — 상위 구조 작성자·동기화 참고 */
/* [한국어] 리셋 전용 큐: reset_work 직렬화. 일반 스캔과 분리해 리셋 중 데드락·기아 방지. */
extern struct workqueue_struct *nvme_delete_wq;	/* [한국어] nvme_delete_wq 필드 — 상위 구조 작성자·동기화 참고 */
/* [한국어] 삭제 전용 큐: delete_work. 리셋과 동시 진행 시 락 순서 문제를 줄이기 위한 분리. */
extern struct mutex nvme_subsystems_lock;	/* [한국어] nvme_subsystems_lock 필드 — 상위 구조 작성자·동기화 참고 */
/* [한국어] 전역 서브시스템 리스트 직렬화 뮤텍스. 새 subnqn 등록/해제·lookup 시 획득.
 * 개별 subsystem->lock 보다 상위 범위의 전역 락. */

/*
 * List of workarounds for devices that required behavior not specified in
 * the standard.
 */
/*
 * [한국어]
 * enum nvme_quirks - 스펙 위반·비표준 하드웨어를 보정하는 비트마스크 집합
 *
 * 각 비트는 pci.c 등 트랜스포트의 디바이스 ID 테이블에서 quirks 필드로 시드되고
 * nvme_init_ctrl() 인자 및 ctrl->quirks 에 저장된다. core/pci 경로가 Identify
 * 결과 해석, 큐 깊이, APST, suspend, CID 인코딩, DMA 마스크 등을 분기할 때
 * 검사한다. 표준 장치에서는 0. 비트 위치는 ABI 가 아니며 커널 내부 전용.
 *
 * 설정 주체: 프로브 테이블·init_ctrl 인자. 읽는 주체: enable/완료/절전/맵 핫·슬로패스.
 * 동기화: 초기화 후 사실상 불변 — 런타임에 비트를 경쟁적으로 바꾸지 않는다.
 * 진단: nvme_quirk_name() 이 단일 비트를 sysfs/dmesg 문자열로 변환한다.
 */
enum nvme_quirks {
	/*
	 * Prefers I/O aligned to a stripe size specified in a vendor
	 * specific Identify field.
	 */
	NVME_QUIRK_STRIPE_SIZE			= (1 << 0),	/* [한국어] NVME_QUIRK_STRIPE_SIZE 상수 — 상위 enum 역할 참고 */
	/* [한국어] 벤더 Identify 의 스트라이프 크기에 I/O 정렬 권장 — 큐 limits 조정. */

	/*
	 * The controller doesn't handle Identify value others than 0 or 1
	 * correctly.
	 */
	NVME_QUIRK_IDENTIFY_CNS			= (1 << 1),	/* [한국어] NVME_QUIRK_IDENTIFY_CNS 상수 — 상위 enum 역할 참고 */
	/* [한국어] CNS 값이 0/1 이외면 오동작 — 확장 Identify 시퀀스를 축소·회피. */

	/*
	 * The controller deterministically returns 0's on reads to
	 * logical blocks that deallocate was called on.
	 */
	NVME_QUIRK_DEALLOCATE_ZEROES		= (1 << 2),	/* [한국어] NVME_QUIRK_DEALLOCATE_ZEROES 상수 — 상위 enum 역할 참고 */
	/* [한국어] deallocate 후 읽기가 확정적으로 0 — discard_zeroes_data 정책에 반영. */

	/*
	 * The controller needs a delay before starts checking the device
	 * readiness, which is done by reading the NVME_CSTS_RDY bit.
	 */
	NVME_QUIRK_DELAY_BEFORE_CHK_RDY		= (1 << 3),	/* [한국어] NVME_QUIRK_DELAY_BEFORE_CHK_RDY 상수 — 상위 enum 역할 참고 */
	/* [한국어] CSTS.RDY 폴링 전 NVME_QUIRK_DELAY_AMOUNT ms 대기 — 조기 폴링 오판 방지. */

	/*
	 * APST should not be used.
	 */
	NVME_QUIRK_NO_APST			= (1 << 4),	/* [한국어] NVME_QUIRK_NO_APST 상수 — 상위 enum 역할 참고 */
	/* [한국어] Autonomous Power State Transition 비활성 — 불안정 장치에서 절전 전환 금지. */

	/*
	 * The deepest sleep state should not be used.
	 */
	NVME_QUIRK_NO_DEEPEST_PS		= (1 << 5),	/* [한국어] NVME_QUIRK_NO_DEEPEST_PS 상수 — 상위 enum 역할 참고 */
	/* [한국어] 최심부 전원 상태 제외 — 웨이크 실패·타임아웃 이슈 회피. */

	/*
	 *  Problems seen with concurrent commands
	 */
	NVME_QUIRK_QDEPTH_ONE			= (1 << 6),	/* [한국어] NVME_QUIRK_QDEPTH_ONE 상수 — 상위 enum 역할 참고 */
	/* [한국어] 동시 명령 1개로 제한 — 큐 깊이 1로 직렬화해 레이스성 펌웨어 버그 회피. */

	/*
	 * Set MEDIUM priority on SQ creation
	 */
	NVME_QUIRK_MEDIUM_PRIO_SQ		= (1 << 7),	/* [한국어] NVME_QUIRK_MEDIUM_PRIO_SQ 상수 — 상위 enum 역할 참고 */
	/* [한국어] Create SQ 시 medium 우선순위 비트 설정 — 특정 컨트롤러 스케줄 요구. */

	/*
	 * Ignore device provided subnqn.
	 */
	NVME_QUIRK_IGNORE_DEV_SUBNQN		= (1 << 8),	/* [한국어] NVME_QUIRK_IGNORE_DEV_SUBNQN 상수 — 상위 enum 역할 참고 */
	/* [한국어] 장치가 보고한 subnqn 무시 — 중복/깨진 NQN 으로 서브시스템 병합 오류 방지. */

	/*
	 * Broken Write Zeroes.
	 */
	NVME_QUIRK_DISABLE_WRITE_ZEROES		= (1 << 9),	/* [한국어] NVME_QUIRK_DISABLE_WRITE_ZEROES 상수 — 상위 enum 역할 참고 */
	/* [한국어] Write Zeroes 명령 비활성 — 블록 계층이 소프트웨어 제로필/다른 경로 사용. */

	/*
	 * Force simple suspend/resume path.
	 */
	NVME_QUIRK_SIMPLE_SUSPEND		= (1 << 10),	/* [한국어] NVME_QUIRK_SIMPLE_SUSPEND 상수 — 상위 enum 역할 참고 */
	/* [한국어] 단순 suspend: 복잡한 상태 보존 대신 재초기화에 가까운 경로 강제. */

	/*
	 * Use only one interrupt vector for all queues
	 */
	NVME_QUIRK_SINGLE_VECTOR		= (1 << 11),	/* [한국어] NVME_QUIRK_SINGLE_VECTOR 상수 — 상위 enum 역할 참고 */
	/* [한국어] 전 큐 단일 IRQ 벡터 — MSI-X 다벡터 깨진 장치용. */

	/*
	 * Use non-standard 128 bytes SQEs.
	 */
	NVME_QUIRK_128_BYTES_SQES		= (1 << 12),	/* [한국어] NVME_QUIRK_128_BYTES_SQES 상수 — 상위 enum 역할 참고 */
	/* [한국어] SQE 128바이트 비표준 포맷 — 큐 메모리 배치·명령 복사 크기 변경. */

	/*
	 * Prevent tag overlap between queues
	 */
	NVME_QUIRK_SHARED_TAGS                  = (1 << 13),	/* [한국어] NVME_QUIRK_SHARED_TAGS 상수 — 상위 enum 역할 참고 */
	/* [한국어] 큐 간 tag 공간 공유/겹침 방지 — 장치 CID 네임스페이스가 전역일 때. */

	/*
	 * Don't change the value of the temperature threshold feature
	 */
	NVME_QUIRK_NO_TEMP_THRESH_CHANGE	= (1 << 14),	/* [한국어] NVME_QUIRK_NO_TEMP_THRESH_CHANGE 상수 — 상위 enum 역할 참고 */
	/* [한국어] 온도 임계 feature 쓰기 금지 — 변경 시 오동작하는 펌웨어 보호. */

	/*
	 * The controller doesn't handle the Identify Namespace
	 * Identification Descriptor list subcommand despite claiming
	 * NVMe 1.3 compliance.
	 */
	NVME_QUIRK_NO_NS_DESC_LIST		= (1 << 15),	/* [한국어] NVME_QUIRK_NO_NS_DESC_LIST 상수 — 상위 enum 역할 참고 */
	/* [한국어] NS Identification Descriptor 리스트 CNS 미지원 — UUID 등 조회 우회. */

	/*
	 * The controller does not properly handle DMA addresses over
	 * 48 bits.
	 */
	NVME_QUIRK_DMA_ADDRESS_BITS_48		= (1 << 16),	/* [한국어] NVME_QUIRK_DMA_ADDRESS_BITS_48 상수 — 상위 enum 역할 참고 */
	/* [한국어] DMA 주소 48비트 제한 — dma_set_mask 상한을 낮춰 고주소 실패 방지. */

	/*
	 * The controller requires the command_id value be limited, so skip
	 * encoding the generation sequence number.
	 */
	NVME_QUIRK_SKIP_CID_GEN			= (1 << 17),	/* [한국어] NVME_QUIRK_SKIP_CID_GEN 상수 — 상위 enum 역할 참고 */
	/* [한국어] CID 에 genctr 상위 니블을 넣지 않음 — 완료 시 genctr++ 도 생략(아래 완료 경로).
	 * tag 재사용 혼동 감지는 약해지지만 장치 CID 폭 제한을 만족. */

	/*
	 * Reports garbage in the namespace identifiers (eui64, nguid, uuid).
	 */
	NVME_QUIRK_BOGUS_NID			= (1 << 18),	/* [한국어] NVME_QUIRK_BOGUS_NID 상수 — 상위 enum 역할 참고 */
	/* [한국어] NS 식별자 필드 신뢰 불가 — multipath 매칭·공유 NS 판정에서 무시/대체. */

	/*
	 * No temperature thresholds for channels other than 0 (Composite).
	 */
	NVME_QUIRK_NO_SECONDARY_TEMP_THRESH	= (1 << 19),	/* [한국어] NVME_QUIRK_NO_SECONDARY_TEMP_THRESH 상수 — 상위 enum 역할 참고 */
	/* [한국어] 복합 온도(채널0) 외 센서 임계 미지원 — hwmon/feature 경로 제한. */

	/*
	 * Disables simple suspend/resume path.
	 */
	NVME_QUIRK_FORCE_NO_SIMPLE_SUSPEND	= (1 << 20),	/* [한국어] NVME_QUIRK_FORCE_NO_SIMPLE_SUSPEND 상수 — 상위 enum 역할 참고 */
	/* [한국어] SIMPLE_SUSPEND 를 강제로 끔 — 단순 경로가 오히려 깨지는 장치 예외. */

	/*
	 * MSI (but not MSI-X) interrupts are broken and never fire.
	 */
	NVME_QUIRK_BROKEN_MSI			= (1 << 21),	/* [한국어] NVME_QUIRK_BROKEN_MSI 상수 — 상위 enum 역할 참고 */
	/* [한국어] 레거시 MSI 미동작 — MSI-X 만 사용하도록 인터럽트 설정 분기. */

	/*
	 * Align dma pool segment size to 512 bytes
	 */
	NVME_QUIRK_DMAPOOL_ALIGN_512		= (1 << 22),	/* [한국어] NVME_QUIRK_DMAPOOL_ALIGN_512 상수 — 상위 enum 역할 참고 */
	/* [한국어] DMA pool 세그먼트 512B 정렬 — 특정 컨트롤러 DMA 엔진 제약. */
};

/*
 * [한국어]
 * nvme_quirk_name - quirk 비트 하나를 sysfs/디버그용 문자열로 변환
 *
 * @q: 단일 비트 enum 값(복합 마스크 아님을 가정한 switch)
 * @return: 소문자 snake_case 이름 또는 "unknown"
 *
 * 왜 필요한가: quirks 비트를 dmesg·sysfs 에 사람이 읽게 노출할 때 숫자 대신
 * 이름을 쓴다. 핫패스가 아니라 진단 경로. 새 quirk 추가 시 case 누락 시
 * "unknown" 으로 떨어져 발견 가능.
 */
static inline char *nvme_quirk_name(enum nvme_quirks q)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	switch (q) {	/* [한국어] 상태/유형 디스패치 */
	case NVME_QUIRK_STRIPE_SIZE:	/* [한국어] 다중 분기 케이스 */
		return "stripe_size";		/* [한국어] 스트라이프 정렬 quirk 표시명 */
	case NVME_QUIRK_IDENTIFY_CNS:	/* [한국어] 다중 분기 케이스 */
		return "identify_cns";		/* [한국어] Identify CNS 제한 표시명 */
	case NVME_QUIRK_DEALLOCATE_ZEROES:	/* [한국어] 다중 분기 케이스 */
		return "deallocate_zeroes";	/* [한국어] dealloc 후 0 보장 표시명 */
	case NVME_QUIRK_DELAY_BEFORE_CHK_RDY:	/* [한국어] 다중 분기 케이스 */
		return "delay_before_chk_rdy";	/* [한국어] RDY 전 지연 표시명 */
	case NVME_QUIRK_NO_APST:	/* [한국어] 다중 분기 케이스 */
		return "no_apst";		/* [한국어] APST 금지 표시명 */
	case NVME_QUIRK_NO_DEEPEST_PS:	/* [한국어] 다중 분기 케이스 */
		return "no_deepest_ps";		/* [한국어] 최심 절전 금지 표시명 */
	case NVME_QUIRK_QDEPTH_ONE:	/* [한국어] 다중 분기 케이스 */
		return "qdepth_one";		/* [한국어] 큐깊이 1 표시명 */
	case NVME_QUIRK_MEDIUM_PRIO_SQ:	/* [한국어] 다중 분기 케이스 */
		return "medium_prio_sq";	/* [한국어] SQ medium 우선순위 표시명 */
	case NVME_QUIRK_IGNORE_DEV_SUBNQN:	/* [한국어] 다중 분기 케이스 */
		return "ignore_dev_subnqn";	/* [한국어] subnqn 무시 표시명 */
	case NVME_QUIRK_DISABLE_WRITE_ZEROES:	/* [한국어] 다중 분기 케이스 */
		return "disable_write_zeroes";	/* [한국어] Write Zeroes 끔 표시명 */
	case NVME_QUIRK_SIMPLE_SUSPEND:	/* [한국어] 다중 분기 케이스 */
		return "simple_suspend";	/* [한국어] 단순 suspend 표시명 */
	case NVME_QUIRK_SINGLE_VECTOR:	/* [한국어] 다중 분기 케이스 */
		return "single_vector";		/* [한국어] 단일 벡터 IRQ 표시명 */
	case NVME_QUIRK_128_BYTES_SQES:	/* [한국어] 다중 분기 케이스 */
		return "128_bytes_sqes";	/* [한국어] 128B SQE 표시명 */
	case NVME_QUIRK_SHARED_TAGS:	/* [한국어] 다중 분기 케이스 */
		return "shared_tags";		/* [한국어] 공유 태그 공간 표시명 */
	case NVME_QUIRK_NO_TEMP_THRESH_CHANGE:	/* [한국어] 다중 분기 케이스 */
		return "no_temp_thresh_change";	/* [한국어] 온도 임계 변경 금지 표시명 */
	case NVME_QUIRK_NO_NS_DESC_LIST:	/* [한국어] 다중 분기 케이스 */
		return "no_ns_desc_list";	/* [한국어] NS desc 리스트 미지원 표시명 */
	case NVME_QUIRK_DMA_ADDRESS_BITS_48:	/* [한국어] 다중 분기 케이스 */
		return "dma_address_bits_48";	/* [한국어] DMA 48bit 제한 표시명 */
	case NVME_QUIRK_SKIP_CID_GEN:	/* [한국어] 다중 분기 케이스 */
		return "skip_cid_gen";		/* [한국어] CID genctr 생략 표시명 */
	case NVME_QUIRK_BOGUS_NID:	/* [한국어] 다중 분기 케이스 */
		return "bogus_nid";		/* [한국어] 가짜 NS ID 표시명 */
	case NVME_QUIRK_NO_SECONDARY_TEMP_THRESH:	/* [한국어] 다중 분기 케이스 */
		return "no_secondary_temp_thresh"; /* [한국어] 2차 온도 임계 없음 표시명 */
	case NVME_QUIRK_FORCE_NO_SIMPLE_SUSPEND:	/* [한국어] 다중 분기 케이스 */
		return "force_no_simple_suspend"; /* [한국어] 단순 suspend 강제 해제 표시명 */
	case NVME_QUIRK_BROKEN_MSI:	/* [한국어] 다중 분기 케이스 */
		return "broken_msi";		/* [한국어] MSI 고장 표시명 */
	case NVME_QUIRK_DMAPOOL_ALIGN_512:	/* [한국어] 다중 분기 케이스 */
		return "dmapool_align_512";	/* [한국어] DMA pool 512 정렬 표시명 */
	}

	return "unknown";			/* [한국어] 미등록 비트 — 테이블 불일치 시 안전 폴백 */
}

/*
 * Common request structure for NVMe passthrough.  All drivers must have
 * this structure as the first member of their request-private data.
 */
/*
 * [한국어] blk-mq 요청 PDU 공통 헤더. tag_set.cmd_size 가 확보한 per-request
 * 메모리의 선두에 위치해야 하며, 트랜스포트 private 확장은 이 구조체 "뒤"에
 * 붙인다(container_of / 캐스팅 전제). nvme_req(req) 로 핫패스 접근.
 *
 * 데이터 흐름: nvme_setup_cmd/init_request 가 cmd 포인터·ctrl 를 채우고,
 * 트랜스포트가 SQ 에 넣은 뒤 CQE 도착 시 nvme_try_complete_req 가 status/
 * result 를 기록하고 genctr 를 증가시킨다. multipath 는 start_time 으로
 * 지연 통계를 낸다.
 */
struct nvme_request {
	struct nvme_command	*cmd;	/* [한국어] cmd 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 이 요청이 제출할 SQE 페이로드 포인터(보통 per-request 메모리 또는 외부 버퍼).
	 * 설정자: nvme_init_request / nvme_setup_cmd / passthrough ioctl 경로.
	 * 읽는 자: 트랜스포트 queue_rq 가 버스/와이어 포맷으로 복사·매핑; 트레이스·effects.
	 * 동기화: 요청 소유 컨텍스트에서만 유효 — 완료 후 재사용 전 재초기화. */

	union nvme_result	result;	/* [한국어] result 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] CQE DW0/1 결과 유니온(생성 LBA, 명령별 결과 필드).
	 * 설정자: nvme_try_complete_req (와이어 엔디안 그대로 저장하는 관례).
	 * 읽는 자: nvme_complete_rq, 동기 제출 결과 복사, ioctl 사용자 버퍼.
	 * 동기화: 완료 시점 이후 단일 소비자. */

	u8			genctr;	/* [한국어] genctr 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] CID generation 카운터. 완료 시 +1(NVME_QUIRK_SKIP_CID_GEN 제외).
	 * 역할: tag 재사용 후 도착한 stale CQE 를 genctr 불일치로 폐기.
	 * CID 상위 4비트에 인코딩(nvme_cid / nvme_find_rq). 제출·완료 핫패스 공유. */

	u8			retries;	/* [한국어] retries 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 호스트 재시도 횟수. 일시 오류·path error 시 complete_rq 가 증가.
	 * 한도 초과 시 실패 종료. usercmd 는 재시도 정책이 더 보수적일 수 있음. */

	u8			flags;	/* [한국어] flags 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] NVME_REQ_CANCELLED/USERCMD, NVME_MPATH_* 비트 집합.
	 * 설정: 취소 콜백·passthrough·mpath start. 읽기: 완료·통계·재시도 분기. */

	u16			status;	/* [한국어] status 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] NVMe status(호스트 형식: phase 제거 후 SCT+SC, 종종 >>1 저장).
	 * 설정자: nvme_try_complete_req / fault injection. 읽는 자: 에러 분류·errno·failover.
	 * 값: 성공 0, 그 외 스펙 status. DNR 비트는 재시도 억제. */

#ifdef CONFIG_NVME_MULTIPATH	/* [한국어] multipath 빌드 시 요청 시작 시각 필드 포함 */
	unsigned long		start_time;	/* [한국어] start_time 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 요청 시작 jiffies. mpath_start_request 가 기록, 지연·failover 통계에 사용. */
#endif	/* [한국어] CONFIG_NVME_MULTIPATH (nvme_request.start_time) */
	struct nvme_ctrl	*ctrl;	/* [한국어] ctrl 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 이 요청을 소유한 컨트롤러 — 완료·취소·dev_err·quirks 접근 앵커.
	 * 설정: 제출 준비(init/setup). 수명: request 완료까지 ctrl 유효 보장 필요. */
};

/*
 * Mark a bio as coming in through the mpath node.
 */
#define REQ_NVME_MPATH		REQ_DRV	/* [한국어] REQ_NVME_MPATH 매크로 — 상위 섹션 계약 참고 */
/* [한국어] multipath 상위 디스크를 통해 들어온 bio/request 표식.
 * REQ_DRV 비트를 재사용해 블록 코어 비트 공간을 늘리지 않음.
 * nvme_start_request 가 이 비트를 보고 mpath_start/end 통계를 호출. */

/*
 * [한국어] nvme_request.flags 비트 정의 — 요청 수명 동안의 호스트 측 속성.
 */
enum {
	NVME_REQ_CANCELLED		= (1 << 0),	/* [한국어] NVME_REQ_CANCELLED 상수 — 상위 enum 역할 참고 */
	/* [한국어] 호스트가 완료 전 취소함 — 타임아웃/리셋/경로 다운 시 cancel 콜백이 설정. */

	NVME_REQ_USERCMD		= (1 << 1),	/* [한국어] NVME_REQ_USERCMD 상수 — 상위 enum 역할 참고 */
	/* [한국어] 사용자 passthrough ioctl/uring 명령 — 재시도·effects 정책이 커널 I/O 와 다름. */

	NVME_MPATH_IO_STATS		= (1 << 2),	/* [한국어] NVME_MPATH_IO_STATS 상수 — 상위 enum 역할 참고 */
	/* [한국어] multipath I/O 통계 수집 대상 — start/end_request 가 카운터 갱신. */

	NVME_MPATH_CNT_ACTIVE		= (1 << 3),	/* [한국어] NVME_MPATH_CNT_ACTIVE 상수 — 상위 enum 역할 참고 */
	/* [한국어] ctrl->nr_active 에 반영된 인플라이트 — 완료 시 반드시 감소시켜 균형 유지. */
};

/*
 * [한국어]
 * nvme_req - request → nvme_request PDU 변환
 *
 * blk_mq_rq_to_pdu 는 tag 할당 시 붙여 둔 per-request 메모리를 반환.
 * 모든 트랜스포트가 nvme_request 를 선두에 두므로 캐스팅이 안전하다.
 * 컨텍스트: 제출·완료 핫패스, atomic 가능.
 */
static inline struct nvme_request *nvme_req(struct request *req)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	return blk_mq_rq_to_pdu(req);	/* [한국어] blk-mq PDU 선두 = nvme_request */
}

/*
 * [한국어]
 * nvme_req_qid - 요청이 속한 NVMe 큐 ID(qid) 계산
 *
 * admin 큐(queuedata NULL 관례/연결 전)는 0.
 * I/O 는 mq_hctx->queue_num+1 — 스펙상 qid 0=Admin, 1..=IO 큐.
 * 트랜스포트 doorbell/CID 로그·AEN 판별(nvme_is_aen_req)과 연계.
 */
static inline u16 nvme_req_qid(struct request *req)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	if (!req->q->queuedata)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		return 0;	/* [한국어] admin/미연결 큐 — 스펙 qid 0 */

	return req->mq_hctx->queue_num + 1;	/* [한국어] hctx 인덱스 → IO qid (1부터) */
}

/* The below value is the specific amount of delay needed before checking
 * readiness in case of the PCI_DEVICE(0x1c58, 0x0003), which needs the
 * NVME_QUIRK_DELAY_BEFORE_CHK_RDY quirk enabled. The value (in ms) was
 * found empirically.
 */
#define NVME_QUIRK_DELAY_AMOUNT		2300	/* [한국어] NVME_QUIRK_DELAY_AMOUNT 매크로 — 상위 섹션 계약 참고 */
/* [한국어] RDY 폴링 전  empirically 2300ms 대기 — 해당 PCI ID 장치 전용 보정 상수. */

/*
 * enum nvme_ctrl_state: Controller state
 *
 * @NVME_CTRL_NEW:		New controller just allocated, initial state
 * @NVME_CTRL_LIVE:		Controller is connected and I/O capable
 * @NVME_CTRL_RESETTING:	Controller is resetting (or scheduled reset)
 * @NVME_CTRL_CONNECTING:	Controller is disconnected, now connecting the
 *				transport
 * @NVME_CTRL_DELETING:		Controller is deleting (or scheduled deletion)
 * @NVME_CTRL_DELETING_NOIO:	Controller is deleting and I/O is not
 *				disabled/failed immediately. This state comes
 * 				after all async event processing took place and
 * 				before ns removal and the controller deletion
 * 				progress
 * @NVME_CTRL_DEAD:		Controller is non-present/unresponsive during
 *				shutdown or removal. In this case we forcibly
 *				kill all inflight I/O as they have no chance to
 *				complete
 */
/*
 * [한국어] 컨트롤러 수명 상태. ctrl->state 에 저장되며 READ_ONCE/ nvme_change_ctrl_state
 * 로만 합법 전이. I/O 제출 가드(nvme_check_ready), 리셋 직렬화, 삭제 순서
 * (AER 정리 → DELETING_NOIO → NS 제거 → DEAD) 의 중심 축.
 */
enum nvme_ctrl_state {	/* [한국어] 컨트롤러 상태 원자 스냅샷 */
	NVME_CTRL_NEW,		/* [한국어] 할당 직후 — 아직 트랜스포트 connect/enable 전 */
	NVME_CTRL_LIVE,		/* [한국어] I/O·admin 정상 서비스 — 제출 핫패스의 기대 상태 */
	NVME_CTRL_RESETTING,	/* [한국어] 리셋 예약/진행 — 신규 I/O 제한, reset_work 가 주도 */
	NVME_CTRL_CONNECTING,	/* [한국어] fabrics/PCIe 재연결·초기 연결 중 — LIVE 진입 전 단계 */
	NVME_CTRL_DELETING,	/* [한국어] 삭제 시작 — 터미널 계열 진입, 복구 불가 방향 */
	NVME_CTRL_DELETING_NOIO,/* [한국어] AER 등 비동기 처리 후 I/O 즉시 실패 전 구간 — NS 제거 직전 */
	NVME_CTRL_DEAD,		/* [한국어] 응답 불가/제거 — 인플라이트 강제 취소, 완전 종료 */
};

/*
 * [한국어] 컨트롤러·NS 별 fault injection 상태(fault_inject.c).
 * debugfs 로 probability/status/dont_retry 를 조절하고, 완료 직전
 * nvme_should_fail() 이 status 를 덮어쓴다. 운영 핫패스 기본 비활성.
 */
struct nvme_fault_inject {
#ifdef CONFIG_FAULT_INJECTION_DEBUG_FS	/* [한국어] 디버그 FS 있을 때만 실필드 — 아니면 빈 구조체 */
	struct fault_attr attr;	/* [한국어] 확률·횟수 등 generic fault-injection 속성 */
	struct dentry *parent;	/* [한국어] debugfs 디렉터리 — fini 시 recursive remove */
	bool dont_retry;	/* DNR, do not retry */
	/* [한국어] true 면 주입 status 에 DNR 성격을 부여해 호스트 재시도 억제 */
	u16 status;		/* status code */
	/* [한국어] 주입할 NVMe status 코드 — 기본 INVALID_OPCODE 등 */
#endif	/* [한국어] CONFIG_FAULT_INJECTION_DEBUG_FS (fault_inject 필드) */
};

/*
 * [한국어] ctrl->flags 비트 번호(비트 인덱스). test_bit/set_bit 대상.
 * 상태 enum 과 별도로 "일회성/동작 플래그" 를 담는다.
 */
enum nvme_ctrl_flags {
	NVME_CTRL_FAILFAST_EXPIRED	= 0,	/* [한국어] NVME_CTRL_FAILFAST_EXPIRED 상수 — 상위 enum 역할 참고 */
	/* [한국어] failfast 타이머 만료 — 경로 오류 시 빠른 실패 모드 진입 표식 */

	NVME_CTRL_ADMIN_Q_STOPPED	= 1,	/* [한국어] NVME_CTRL_ADMIN_Q_STOPPED 상수 — 상위 enum 역할 참고 */
	/* [한국어] admin 큐 정지됨 — 재개 전 admin 제출 금지 */

	NVME_CTRL_STARTED_ONCE		= 2,	/* [한국어] NVME_CTRL_STARTED_ONCE 상수 — 상위 enum 역할 참고 */
	/* [한국어] 한 번이라도 start 됨 — 재개/리셋 경로 분기(최초 vs 재시작) */

	NVME_CTRL_STOPPED		= 3,	/* [한국어] NVME_CTRL_STOPPED 상수 — 상위 enum 역할 참고 */
	/* [한국어] 컨트롤러 논리 정지 — keep-alive/스캔 등 백그라운드 억제 */

	NVME_CTRL_SKIP_ID_CNS_CS	= 4,	/* [한국어] NVME_CTRL_SKIP_ID_CNS_CS 상수 — 상위 enum 역할 참고 */
	/* [한국어] 특정 Identify CNS/CSI 조합 스킵 — 호환성 단축 경로 */

	NVME_CTRL_DIRTY_CAPABILITY	= 5,	/* [한국어] NVME_CTRL_DIRTY_CAPABILITY 상수 — 상위 enum 역할 참고 */
	/* [한국어] 큐 한계 등 capability 가 더러워짐 — 재협상/limits 갱신 필요 */

	NVME_CTRL_FROZEN		= 6,	/* [한국어] NVME_CTRL_FROZEN 상수 — 상위 enum 역할 참고 */
	/* [한국어] freeze 진행 중 표식 — wait_freeze/unfreeze 와 연동 */
};

/*
 * [한국어]
 * struct nvme_ctrl - NVMe 컨트롤러 호스트 측 인스턴스 (PCIe·fabrics·apple 공통)
 *
 * 트랜스포트가 자기 private 구조체 선두에 임베드하거나 포인터로 소유한다.
 * ops vtable 로 MMIO/연결/삭제 차이를 숨기고, core 는 이 구조체만 보고
 * 상태 머신·NS 스캔·tag set·keep-alive 를 돌린다.
 *
 * 동기화 개요:
 *   - state: nvme_change_ctrl_state + READ_ONCE; state_wq 대기
 *   - lock: 짧은 임계 구역(이벤트 비트 등)
 *   - scan_lock / namespaces_lock: 스캔·NS 리스트
 *   - srcu: NS 순회 RCU-ish 유예
 *   - ana_lock / dhchap_auth_mutex: multipath·인증
 *
 * 설정 주체: nvme_init_ctrl / 트랜스포트 probe / Identify 완료 경로.
 * 해제: uninit_ctrl + ops->free_ctrl, kref 는 device 경유 get/put.
 */
struct nvme_ctrl {
	bool comp_seen;	/* [한국어] comp_seen 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 최소 한 번 완료 인터럽트/CQE 를 관측했는지.
	 * 설정자: 트랜스포트 완료 경로(최초 CQE/IRQ). 읽는 자: 디버그·생존 진단.
	 * 동기화: 완화된 플래그(정확한 레이스 민감 아님). */

	bool identified;	/* [한국어] identified 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] Identify Controller 성공 여부 — max_ 계열/oncs/oacs 등 캐시 필드 유효성 게이트.
	 * 설정자: core Identify 완료 경로. 읽는 자: 기능 협상·sysfs·후속 초기화.
	 * 동기화: 초기화 직렬화 하에서 전이 후 LIVE 에서 읽기 위주. */

	bool passthru_err_log_enabled;	/* [한국어] passthru_err_log_enabled 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 사용자 passthrough 실패 상세 로그 토글.
	 * 설정자: sysfs. 읽는 자: ioctl/uring 완료 오류 경로. 동기화: 단순 bool. */

	enum nvme_ctrl_state state;	/* [한국어] state 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 컨트롤러 상태 머신 현재 값 — I/O 제출 가드·리셋/삭제의 단일 진실 원천.
	 * 설정자: 오직 nvme_change_ctrl_state() (합법 전이만). 읽는 자: nvme_ctrl_state()/
	 *         nvme_check_ready/핫패스. 동기화: WRITE 는 상태 전이 경로, READ 는 READ_ONCE. */

	spinlock_t lock;	/* [한국어] lock 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 짧은 임계구역용 스핀락 — events 비트 등 IRQ 와 공유 가능한 갱신.
	 * 설정/사용: core·트랜스포트 이벤트 경로. sleep 불가 구간만. scan_lock 과 중첩 순서 주의. */

	struct mutex scan_lock;	/* [한국어] scan_lock 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] namespace scan_work 및 스캔 관련 메타 직렬화.
	 * 획득: queue_scan/scan_work/리셋 연동. 목적: 동시 스캔·NS 테이블 충돌 방지. */

	const struct nvme_ctrl_ops *ops;	/* [한국어] ops 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 트랜스포트 vtable(pci/tcp/rdma/fc/apple). probe 시 고정, 수명 동안 불변 가정.
	 * 호출: enable/disable/reg_ 계열/delete/free/AER 재제출 등 core 공통 경로. */

	struct request_queue *admin_q;	/* [한국어] admin_q 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] Admin 전용 blk-mq 큐 — Identify/Features/AER/Abort/FW/Security.
	 * 설정: alloc_admin_tag_set. 제출: __nvme_submit_sync_cmd·ioctl. qid=0. */

	struct request_queue *connect_q;	/* [한국어] connect_q 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] fabrics Connect 전용 큐 — 일반 admin 과 분리해 재연결 중 데드락 완화.
	 * PCIe 에서는 보통 미사용. 설정: fabrics 연결 경로. */

	struct request_queue *fabrics_q;	/* [한국어] fabrics_q 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] fabrics 부가 제어 큐(트랜스포트 의존). PCIe 비사용 가능.
	 * 설정/해제: 해당 트랜스포트 초기화·teardown. */

	struct device *dev;	/* [한국어] dev 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 언더레이 device(예: &pdev->dev) — DMA 마스크·전원·부모 관계.
	 * 설정: nvme_init_ctrl. 읽는 자: dma_*·dev_err 등. */

	int instance;	/* [한국어] instance 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] nvmeX 번호. 설정: ida 할당(init). 읽는 자: 이름 생성·sysfs·로그. */

	int numa_node;	/* [한국어] numa_node 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 선호 NUMA 노드 — 큐 맵·할당 지역성, multipath NUMA iopolicy 힌트.
	 * 설정: 프로브/플랫폼. 읽는 자: blk-mq 맵·경로 선택. */

	struct blk_mq_tag_set *tagset;	/* [한국어] tagset 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] I/O tag set. tag 하위 12비트가 CID 축. 하드웨어 큐→CPU 맵 포함.
	 * 설정: alloc_io_tag_set. 취소: cancel_tagset. 해제: remove_io_tag_set. */

	struct blk_mq_tag_set *admin_tagset;	/* [한국어] admin_tagset 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] Admin tag set(소형). 상단 태그는 AEN 예약(nvme_is_aen_req).
	 * 설정: alloc_admin_tag_set. 해제: remove_admin_tag_set. */

	struct list_head namespaces;	/* [한국어] namespaces 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 이 컨트롤러의 nvme_ns 경로 리스트.
	 * 설정: scan 추가/remove. 순회: namespaces_lock 또는 srcu 규칙 준수. */

	struct mutex namespaces_lock;	/* [한국어] namespaces_lock 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] namespaces 리스트 쓰기 직렬화. 읽기는 srcu 와 조합.
	 * 획득: NS 추가/삭제/재검증 경로(sleep 가능). */

	struct srcu_struct srcu;	/* [한국어] srcu 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] NS 리스트/포인터 읽기 측 grace — 제출 경로에서 ns 안정성.
	 * 사용: srcu_read_lock 구간에서 list/NS 접근 후 유예 기간 대기 가능. */

	struct device ctrl_device;	/* [한국어] ctrl_device 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 클래스 모델 임베디드 device — sysfs 트리 컨트롤러 노드.
	 * 등록: add_ctrl 경로. 해제: uninit. */

	struct device *device;	/* char device */
	/* [한국어] /dev/nvmeX 캐릭터 device. get_device/put_device = nvme_get/put_ctrl.
	 * 수명 앵커: 마지막 put 이 release 로 uninit/free 연쇄. */

#ifdef CONFIG_NVME_HWMON	/* [한국어] 온도 센서 hwmon 연동 빌드 */
	struct device *hwmon_device;	/* [한국어] hwmon_device 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] hwmon 클래스 장치 — 복합 온도 등. 설정: hwmon_init. 해제: hwmon_exit. */
#endif	/* [한국어] CONFIG_NVME_HWMON (ctrl->hwmon_device) */
	struct cdev cdev;	/* [한국어] cdev 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 캐릭터 cdev — fops 가 ioctl/uring 진입. cdev_add 경유 등록. */

	struct work_struct reset_work;	/* [한국어] reset_work 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 비동기 리셋 본체. 큐: nvme_reset_wq. 스케줄: try_sched_reset/reset_ctrl.
	 * 하는 일: 상태 RESETTING→재연결/재enable→LIVE 또는 실패 처리. */

	struct work_struct delete_work;	/* [한국어] delete_work 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 비동기 삭제 본체. 큐: nvme_delete_wq. 스케줄: delete_ctrl.
	 * 순서: DELETING→AER 정리→DELETING_NOIO→NS 제거→DEAD/free. */

	wait_queue_head_t state_wq;	/* [한국어] state_wq 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] state 변경 시 웨이크. 대기자: nvme_wait_reset 등 프로세스 컨텍스트.
	 * 깨우는 쪽: nvme_change_ctrl_state. */

	struct nvme_subsystem *subsys;	/* [한국어] subsys 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 소속 서브시스템(동일 subnqn 그룹). ns_head 소유·ctrls 리스트 허브.
	 * 설정: add_ctrl 시 lookup/create. 참조는 subsystem kref 규칙. */

	struct list_head subsys_entry;	/* [한국어] subsys_entry 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] subsys->ctrls 노드. 보호: subsystem->lock / 전역 등록 경로. */

	struct opal_dev *opal_dev;	/* [한국어] opal_dev 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] TCG Opal SED 컨텍스트. Security Send/Receive→sed-opal 상태 머신.
	 * 설정: Opal 지원 시 init. 해제: 컨트롤러 teardown. */

	u16 cntlid;	/* [한국어] cntlid 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] Controller ID. 출처: Identify/Connect. fabrics multipath 경로 구분 키. */

	u16 mtfa;	/* [한국어] mtfa 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] Maximum Time for Firmware Activation. fw_act_work 대기 상한 계산에 사용. */

	u32 ctrl_config;	/* [한국어] ctrl_config 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] CC 레지스터 소프트웨어 미러(EN/CSS/SHN 등).
	 * 설정: enable/disable 경로. 읽기: nvme_multi_css 등. */

	u32 queue_count;	/* [한국어] queue_count 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 생성·협상된 큐 개수. Set Features Number of Queues 결과 반영. */

	u64 cap;	/* [한국어] cap 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] CAP 레지스터 캐시(TO/MQES/CSS 등). 초기 MMIO/속성 읽기 후 고정. */

	u32 max_hw_sectors;	/* [한국어] max_hw_sectors 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 블록 max_hw_sectors — MDTS·페이지 크기에서 유도. queue limits 반영. */

	u32 max_segments;	/* [한국어] max_segments 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 요청 당 최대 데이터 세그먼트 — PRP/SGL 한계. map 경로 제약. */

	u32 max_integrity_segments;	/* [한국어] max_integrity_segments 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 무결성 메타데이터 세그먼트 상한 — PI 경로 queue limits. */

	u32 max_zeroes_sectors;	/* [한국어] max_zeroes_sectors 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] Write Zeroes 단일 명령 섹터 상한. setup_cmd 가 분할 여부 판단. */

#ifdef CONFIG_BLK_DEV_ZONED	/* [한국어] zoned block 계층 연동 시 zone append 한계 필드 */
	u32 max_zone_append;	/* [한국어] max_zone_append 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] Zone Append 최대 크기. ZNS limits 및 명령 조립에 사용. */
#endif	/* [한국어] CONFIG_BLK_DEV_ZONED (ctrl->max_zone_append) */
	u16 crdt[3];	/* [한국어] crdt 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] Command Retry Delay Throttle 3단. 특정 status 재시도 백오프 힌트. */

	u16 oncs;	/* [한국어] oncs 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] Optional NVM Command Support 비트맵(Write Zeroes/DSM 등).
	 * 출처: Identify. 명령 지원 여부 분기에 사용. */

	u8 dmrl;	/* [한국어] dmrl 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] Dataset Management 관련 범위 제한 필드(스펙 Identify). discard 조립 참고. */

	u32 dmrsl;	/* [한국어] dmrsl 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] DSM range size limit. 너무 큰 discard 를 분할하는 근거. */

	u16 oacs;	/* [한국어] oacs 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] Optional Admin Command Support(NS mgmt/Security/FW 등).
	 * unique nsid·기능 게이트에 사용. */

	u16 sqsize;	/* [한국어] sqsize 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] SQ 크기(협상/생성 파라미터). 큐 생성·깊이 정책 입력. */

	u32 max_namespaces;	/* [한국어] max_namespaces 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 스캔 루프 상한. Identify NN 등에서 채움. */

	atomic_t abort_limit;	/* [한국어] abort_limit 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 동시 Abort 여유. 타임아웃 시 Abort 발행 가능 여부.
	 * 증감: abort 제출/완료 경로. 고갈 시 다른 복구 전략. */

	u8 vwc;	/* [한국어] vwc 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] Volatile Write Cache 특성. flush 필요·FUA 정책 결정. */

	u32 vs;	/* [한국어] vs 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 버전. 기능 네고·호환 분기. 출처: VS 레지스터/Identify. */

	u32 sgls;	/* [한국어] sgls 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] SGL 지원 비트. nvme_ctrl_sgl_supported/meta_sgl_supported 입력. */

	u16 kas;	/* [한국어] kas 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] Keep Alive Support. 0 이면 KA 불필요. fabrics 에서 중요. */

	u8 npss;	/* [한국어] npss 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 전원 상태 개수(0-based). psd[] 유효 엔트리 수 결정. */

	u8 apsta;	/* [한국어] apsta 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] APST 지원 필드. APST 구성 가능 여부. */

	u16 wctemp;	/* [한국어] wctemp 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] Warning Composite Temperature(켈빈). hwmon/임계 비교. */

	u16 cctemp;	/* [한국어] cctemp 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] Critical Composite Temperature(켈빈). 위험 온도 경계. */

	u32 oaes;	/* [한국어] oaes 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] Optional Asynchronous Events Supported. AER 구독 마스크 근거. */

	u32 aen_result;	/* [한국어] aen_result 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 최근 AER CQE 결과 스냅샷. async_event_work 가 해석·후속 로그 fetch. */

	u32 ctratt;	/* [한국어] ctratt 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] Controller Attributes(NVM Sets 등). unique nsid 조건 3번 항목. */

	unsigned int shutdown_timeout;	/* [한국어] shutdown_timeout 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] CC.SHN 후 CSTS.SHST 대기 초. 정상 종료 disable 경로. */

	unsigned int kato;	/* [한국어] kato 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] Keep Alive Timeout(초). ka_work 주기·연결 생존 기준. */

	bool subsystem;	/* [한국어] subsystem 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 서브시스템 수준 동작(NSSR 등) 가능 힌트. reset_subsystem 게이트. */

	unsigned long quirks;	/* [한국어] quirks 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] nvme_quirks 비트 OR. 프로브 시드 후 런타임 전역 검사.
	 * 예: SKIP_CID_GEN, NO_APST, DELAY_BEFORE_CHK_RDY. */

	struct nvme_id_power_state psd[32];	/* [한국어] psd 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 전원 상태 디스크립터. APST 표·레이턴시 정책 계산 입력. */

	struct nvme_effects_log *effects;	/* [한국어] effects 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] Commands Supported and Effects 로그 캐시.
	 * passthrough 전 nvme_passthru_start 가 freeze 필요 여부 판단. */

	struct xarray cels;	/* [한국어] cels 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] CSI 별 Command Effects Log. 다중 커맨드 셋(I/O set independent). */

	struct work_struct scan_work;	/* [한국어] scan_work 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] NS 열거·갱신 워커. 큐: nvme_wq. 트리거: queue_scan(AER/시작/리셋 후). */

	struct work_struct async_event_work;	/* [한국어] async_event_work 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] AER 후처리 워커 — 로그 페이지·NS 변화·에러 처리. nvme_wq. */

	struct delayed_work ka_work;	/* [한국어] ka_work 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] keep-alive 주기 전송. fabrics 연결 유지. stop_keep_alive 로 취소. */

	struct delayed_work failfast_work;	/* [한국어] failfast_work 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] failfast 만료 워크 — 경로 장애 시 빠른 실패 모드(FAILFAST_EXPIRED). */

	struct nvme_command ka_cmd;	/* [한국어] ka_cmd 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] Keep Alive SQE 템플릿 재사용 버퍼. 매 주기 재할당 회피. */

	unsigned long ka_last_check_time;	/* [한국어] ka_last_check_time 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 마지막 KA 관련 jiffies. 지연·헬스 진단. */

	struct work_struct fw_act_work;	/* [한국어] fw_act_work 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 펌웨어 활성화 대기·재개. mtfa 와 연계해 I/O 동결 시간 관리. */

	unsigned long events;	/* [한국어] events 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 대기 중 비동기 이벤트 비트마스크. lock 보호 구간에서 갱신. */

#ifdef CONFIG_NVME_MULTIPATH	/* [한국어] 컨트롤러 ANA/활성 I/O 카운터 등 multipath 전용 필드 */
	/* asymmetric namespace access: */
	/* [한국어] 비대칭 네임스페이스 접근(ANA) — multipath 경로 상태의 스펙 축 */
	u8 anacap;	/* [한국어] anacap 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] ANA 역량. Identify 에서 시드. 그룹/전이 지원 여부. */

	u8 anatt;	/* [한국어] anatt 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] ANA Transition Time. anatt_timer 만료 정책과 연계. */

	u32 anagrpmax;	/* [한국어] anagrpmax 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 최대 ANA 그룹 ID. 로그 파싱 경계. */

	u32 nanagrpid;	/* [한국어] nanagrpid 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] ANA 그룹 수. 버퍼 크기·순회에 사용. */

	struct mutex ana_lock;	/* [한국어] ana_lock 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] ANA 로그 버퍼·경로 상태 갱신 직렬화. ana_work/identify 경로. */

	struct nvme_ana_rsp_hdr *ana_log_buf;	/* [한국어] ana_log_buf 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] Get Log Page ANA 버퍼. NULL 이면 nvme_ctrl_use_ana==false.
	 * 할당: mpath_init_identify. 해제: mpath_uninit. */

	size_t ana_log_size;	/* [한국어] ana_log_size 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] ana_log_buf 바이트 크기. 재할당·파싱 한계. */

	struct timer_list anatt_timer;	/* [한국어] anatt_timer 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] ANA 전이 시간 감시 타이머. 만료 시 경로 재평가 트리거. */

	struct work_struct ana_work;	/* [한국어] ana_work 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] ANA 로그 재조회·ns ana_state 반영 워커. */

	atomic_t nr_active;	/* [한국어] nr_active 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 이 컨트롤러 경로 활성 mpath I/O 수. QD iopolicy·부하 분산 입력.
	 * 증감: mpath_start/end_request (REQ_NVME_MPATH). */
#endif	/* [한국어] CONFIG_NVME_MULTIPATH (ctrl ANA/nr_active 블록) */

#ifdef CONFIG_NVME_HOST_AUTH	/* [한국어] DH-HMAC-CHAP 호스트 인증 빌드: 키·워크·트랜잭션 */
	struct work_struct dhchap_auth_work;	/* [한국어] dhchap_auth_work 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] DH-HMAC-CHAP 협상 실행 워커. auth_negotiate 가 스케줄. */

	struct mutex dhchap_auth_mutex;	/* [한국어] dhchap_auth_mutex 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 인증 상태·키 자료 직렬화. negotiate/wait/free 경로. */

	struct nvme_dhchap_queue_context *dhchap_ctxs;	/* [한국어] dhchap_ctxs 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 큐별 인증 컨텍스트 배열. qid 인덱스. */

	struct nvme_dhchap_key *host_key;	/* [한국어] host_key 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 호스트 비밀키 자료. 설정: auth_init. 해제: auth_free. */

	struct nvme_dhchap_key *ctrl_key;	/* [한국어] ctrl_key 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 컨트롤러 측 키 자료. 협상 결과/설정 보관. */

	u16 transaction;	/* [한국어] transaction 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 인증 트랜잭션 카운터. 재협상 시 증가. */
#endif	/* [한국어] CONFIG_NVME_HOST_AUTH (ctrl auth 필드) */
	key_serial_t tls_pskid;	/* [한국어] tls_pskid 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] TLS PSK keyring 시리얼. TCP fabrics 보안 세션. revoke 시 무효화. */

	/* Power saving configuration */
	/* [한국어] 전원 절약(APST) 사용자 정책 구간 — 레이턴시 예산과 활성 플래그 */
	u64 ps_max_latency_us;	/* [한국어] ps_max_latency_us 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 허용 최대 절전 탈출 지연(µs). APST 표에서 깊은 상태 제외 기준.
	 * 설정: sysfs/정책. 읽는 자: APST 재계산 경로. */

	bool apst_enabled;	/* [한국어] apst_enabled 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 현재 APST feature 활성. Set Features 결과 반영. */

	/* PCIe only: */
	/* [한국어] PCIe Host Memory Buffer(HMB) 협상 파라미터 — fabrics 에선 보통 0/미사용 */
	u16 hmmaxd;	/* [한국어] hmmaxd 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] HMB 최대 디스크립터 수. PCIe Host Memory Buffer 협상. */

	u32 hmpre;	/* [한국어] hmpre 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] HMB preferred size. 호스트가 제공 권장 크기. */

	u32 hmmin;	/* [한국어] hmmin 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] HMB minimum size. 최소 보장 크기. */

	u32 hmminds;	/* [한국어] hmminds 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] HMB 디스크립터 엔트리 최소 크기. */

	/* Fabrics only */
	/* [한국어] NVMe-oF 전용 캡슐/재연결 필드 — PCIe 경로에서는 무시되거나 NULL */
	u32 ioccsz;	/* [한국어] ioccsz 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] I/O Capsule Command 크기. NVMe-oF 인캡슐 데이터 한계. */

	u32 iorcsz;	/* [한국어] iorcsz 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] I/O Capsule Response 크기. 응답 캡슐 한계. */

	u16 icdoff;	/* [한국어] icdoff 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] In-capsule data offset. 캡슐 레이아웃. */

	u16 maxcmd;	/* [한국어] maxcmd 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 컨트롤러 최대 동시 명령(fabrics 협상). 크레딧/깊이 상한. */

	int nr_reconnects;	/* [한국어] nr_reconnects 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 연속 재연결 시도 횟수. 백오프·포기 정책 입력. 성공 시 리셋. */

	unsigned long flags;	/* [한국어] flags 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] nvme_ctrl_flags 비트(test_bit/set_bit). FAILFAST/STOPPED/FROZEN 등.
	 * 상태 enum 과 별개 동작 플래그. */

	struct nvmf_ctrl_options *opts;	/* [한국어] opts 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] fabrics 연결 옵션(host NQN, traddr, reconn delay 등).
	 * 수명: fabrics 트리 할당/해제. PCIe 는 NULL. */

	struct page *discard_page;	/* [한국어] discard_page 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] DSM range 기술자 스크래치 페이지. discard 명령 페이로드. */

	unsigned long discard_page_busy;	/* [한국어] discard_page_busy 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] discard_page 사용 중 비트. 동시 discard 직렬화. */

	struct nvme_fault_inject fault_inject;	/* [한국어] fault_inject 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 컨트롤러 단위 fault injection(admin 등 disk 없는 요청).
	 * init/fini: 컨트롤러 등록 경로. should_fail 훅. */

	enum nvme_ctrl_type cntrltype;	/* [한국어] cntrltype 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 컨트롤러 타입(I/O vs discovery 등). 스캔·연결 정책 분기. */

	enum nvme_dctype dctype;	/* [한국어] dctype 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] Discovery 컨트롤러 세부 타입. discovery 로그 처리. */

	u16			awupf; /* 0's based value. */
	/* [한국어] Atomic Write Unit Power Fail(0-based). 원자 쓰기 한계 유도 후 limits 반영. */
};

/*
 * [한국어] 상태 읽기 단일 진입점. WRITE 는 change_ctrl_state 경로가 담당하고
 * 여기서는 READ_ONCE 로 재정렬·찢어짐 없는 스냅샷을 얻는다.
 */
static inline enum nvme_ctrl_state nvme_ctrl_state(struct nvme_ctrl *ctrl)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	return READ_ONCE(ctrl->state);	/* [한국어] 동시 전이와 레이스 없는 스냅샷 로드 */
}

/*
 * [한국어] multipath I/O 정책 — 서브시스템 단위로 ns_head 경로 선택에 적용.
 */
enum nvme_iopolicy {
	NVME_IOPOLICY_NUMA,	/* [한국어] 제출 CPU 의 NUMA 노드에 가까운 경로 선호 */
	NVME_IOPOLICY_RR,	/* [한국어] round-robin 으로 경로 분산 */
	NVME_IOPOLICY_QD,	/* [한국어] 큐 깊이/활성 I/O 수 기반 부하 분산(nr_active) */
};

/*
 * [한국어]
 * struct nvme_subsystem - 동일 NQN 을 공유하는 컨트롤러 집합 + NS head 소유자
 *
 * multipath 에서 "논리 디스크" 의 상위 정체성. device 모델 노드와 별도 kref 로
 * 마지막 put 시 unregister 한다(주석 원문 참고). ctrls/nsheads 리스트의 허브.
 */
struct nvme_subsystem {
	int			instance;	/* [한국어] instance 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] nvme-subsysX 인스턴스 번호. 설정: 서브시스템 최초 생성 시 ida/할당자.
	 * 읽는 자: sysfs 이름·로그. 전역적으로 유일. */

	struct device		dev;	/* [한국어] dev 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 서브시스템 sysfs device 노드. 마지막 kref put 시 unregister 되는 대상.
	 * 속성: iopolicy 등 subsys_attr_*. */

	/*
	 * Because we unregister the device on the last put we need
	 * a separate refcount.
	 */
	struct kref		ref;	/* [한국어] ref 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] device 모델과 분리된 참조 카운트 — 마지막 put 에서 unregister.
	 * 증가: 컨트롤러/NS head 연결. 감소: 해당 해제 경로. 0 이면 서브시스템 소멸. */

	struct list_head	entry;	/* [한국어] entry 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 전역 서브시스템 리스트 노드. 보호: nvme_subsystems_lock.
	 * lookup by subnqn 시 순회. */

	struct mutex		lock;	/* [한국어] lock 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] ctrls/nsheads 변경·식별 문자열 갱신 직렬화.
	 * 컨트롤러 add/del, ns_head 생성, iopolicy 변경 시 획득. */

	struct list_head	ctrls;	/* [한국어] ctrls 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 소속 nvme_ctrl 리스트(동일 subnqn 다중 경로/포트).
	 * 노드: ctrl->subsys_entry. multipath freeze 시 순회. */

	struct list_head	nsheads;	/* [한국어] nsheads 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 소속 nvme_ns_head 리스트 — 논리 네임스페이스 앵커 집합.
	 * 스캔이 새 NS 를 보면 head 생성 후 여기에 연결. */

	char			subnqn[NVMF_NQN_SIZE];	/* [한국어] subnqn 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] Subsystem NQN — 컨트롤러 병합 키. IGNORE_DEV_SUBNQN quirk 시 호스트 생성 값. */

	char			serial[20];	/* [한국어] serial 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] Identify 시리얼(우측 스페이스 패딩). nvme_strlen 으로 로그 출력. */

	char			model[40];	/* [한국어] model 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 모델 문자열. print_device_info 폴백 로그에 사용. */

	char			firmware_rev[8];	/* [한국어] firmware_rev 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 펌웨어 리비전. 오류 시 장치 식별 로그. */

	u8			cmic;	/* [한국어] cmic 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] CMIC — multi-path I/O·NS sharing·ANA 역량 비트.
	 * unique nsid·mpath 경고(CONFIG 꺼짐) 판단에 사용. */

	enum nvme_subsys_type	subtype;	/* [한국어] subtype 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] NVM / Discovery 등 유형. discovery 전용 처리 분기. */

	u16			vendor_id;	/* [한국어] vendor_id 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] VID. 오류 로그·quirk 힌트. Identify 에서 시드. */

	struct ida		ns_ida;	/* [한국어] ns_ida 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] ns_head->instance 할당자. 생성 시 get, 소멸 시 free. */

#ifdef CONFIG_NVME_MULTIPATH	/* [한국어] 서브시스템 단위 I/O 정책 필드 */
	enum nvme_iopolicy	iopolicy;	/* [한국어] iopolicy 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 경로 선택 정책(NUMA/RR/QD). 기본: mpath_default_iopolicy.
	 * 변경: sysfs subsys_attr_iopolicy. 사용: nvme_find_path. */
#endif	/* [한국어] CONFIG_NVME_MULTIPATH (subsys->iopolicy) */
};

/*
 * Container structure for uniqueue namespace identifiers.
 */
/*
 * [한국어] 네임스페이스 고유 식별자 묶음. multipath 에서 서로 다른 컨트롤러의
 * NS 를 동일 head 로 묶는 매칭 키. BOGUS_NID quirk 시 신뢰도 하락.
 */
struct nvme_ns_ids {
	u8	eui64[8];	/* [한국어] 64-bit EUI — 구형 장치 식별 */
	u8	nguid[16];	/* [한국어] Namespace Globally Unique Identifier */
	uuid_t	uuid;		/* [한국어] UUID 식별자(디스크립터 리스트) */
	u8	csi;		/* [한국어] Command Set Identifier — NVM/ZNS 등 */
};

/*
 * Anchor structure for namespaces.  There is one for each namespace in a
 * NVMe subsystem that any of our controllers can see, and the namespace
 * structure for each controller is chained of it.  For private namespaces
 * there is a 1:1 relation to our namespace structures, that is ->list
 * only ever has a single entry for private namespaces.
 */
/*
 * [한국어] 서브시스템 전역 네임스페이스 앵커. 공유 NS 는 여러 nvme_ns(경로)가
 * list/siblings 로 매달리고, multipath 시 head->disk 가 사용자에게 보이는
 * 단일 gendisk 가 된다. private NS 는 보통 경로 하나.
 *
 * multipath 필드: current_path[] 는 NUMA 노드별 선호 경로 RCU 포인터 배열
 * (유연 배열 멤버). requeue_* 는 경로 없음/전이 중 bio 보류.
 */
struct nvme_ns_head {
	struct list_head	list;	/* [한국어] list 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 동일 논리 NS 에 매달린 nvme_ns(컨트롤러별 경로) 리스트 헤드.
	 * 추가/삭제: 스캔·경로 제거. 순회: srcu 또는 head->lock 규칙. private NS 는 보통 1개. */

	struct srcu_struct      srcu;	/* [한국어] srcu 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] current_path[]·경로 리스트 읽기 보호. 제출 핫패스가 짧은 SRCU 임계로 경로 획득.
	 * 갱신 측은 유예 기간 후 옛 포인터 해제. */

	struct nvme_subsystem	*subsys;	/* [한국어] subsys 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 소유 서브시스템. head 수명·nsheads 리스트·iopolicy 의 상위 컨텍스트. */

	struct nvme_ns_ids	ids;	/* [한국어] ids 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] EUI/NGUID/UUID/CSI — multipath 매칭 키. BOGUS_NID 시 신뢰도 하락. */

	u8			lba_shift;	/* [한국어] lba_shift 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] LBA 크기 시프트(예: 512→9, 4K→12). nvme_sect_to_lba/lba_to_sect 핵심.
	 * 설정: Identify NS 포맷. 전 경로가 동일 기하를 공유한다고 가정. */

	u16			ms;	/* [한국어] ms 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] Metadata Size(바이트). PI 분리/확장 LBA 판단. */

	u16			pi_size;	/* [한국어] pi_size 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] PI 가 차지하는 메타 크기. nvme_ns_has_pi: pi_type && ms==pi_size. */

	u8			pi_type;	/* [한국어] pi_type 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] T10 PI 타입 0/1/2/3. 0=없음. 무결성 프로파일 설정 입력. */

	u8			guard_type;	/* [한국어] guard_type 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 가드 알고리즘(CRC16 등). 블록 integrity 프로파일과 정합. */

	struct list_head	entry;	/* [한국어] entry 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] subsys->nsheads 노드. subsystem->lock 보호. */

	struct kref		ref;	/* [한국어] ref 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] head 수명. 경로 ns·상위 disk·cdev 가 tryget/put.
	 * 0 이면 head 해제 및 ida 반환. */

	bool			shared;	/* [한국어] shared 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 공유 네임스페이스. true 면 multipath 후보·unique nsid 강제 조건. */

	bool			rotational;	/* [한국어] rotational 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 회전 매체 힌트. 큐 플래그/스케줄 정책에 반영 가능. */

	bool			passthru_err_log_enabled;	/* [한국어] passthru_err_log_enabled 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 이 head 경유 passthrough 오류 상세 로그. sysfs 토글. */

	struct nvme_effects_log *effects;	/* [한국어] effects 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] NS 스코프 effects 캐시. NS 대상 passthrough freeze 판단. */

	u64			nuse;	/* [한국어] nuse 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] Namespace Utilization. thin 사용량 관찰, 급변 시 rs_nuse 로 경고. */

	unsigned		ns_id;	/* [한국어] ns_id 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] NVMe NSID. 명령 DW1. private 비유일 가능(스펙 예외). */

	int			instance;	/* [한국어] instance 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 호스트 측 인스턴스(ng 장치 번호 등). ns_ida 할당. */

#ifdef CONFIG_BLK_DEV_ZONED	/* [한국어] head 에 존 크기 캐시 */
	u64			zsze;	/* [한국어] zsze 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 존 크기. update_zone_info 가 queue limits 에 반영. */
#endif	/* [한국어] CONFIG_BLK_DEV_ZONED (ns_head->zsze) */
	unsigned long		features;	/* [한국어] features 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] nvme_ns_features(EXT_LBAS/METADATA/DEAC). setup_cmd 분기. */

	struct ratelimit_state	rs_nuse;	/* [한국어] rs_nuse 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] nuse 경고 레이트 리미트 — 로그 폭주 방지. */

	struct cdev		cdev;	/* [한국어] cdev 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] NS 캐릭터 노드(ngXnY). 패스스루·관리. */

	struct device		cdev_device;	/* [한국어] cdev_device 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 위 cdev 의 device. nvme_cdev_add/del. */

	struct gendisk		*disk;	/* [한국어] disk 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] multipath 상위 gendisk. 비-MPATH 또는 미할당 시 NULL.
	 * nvme_ns_head_multipath / ns_head_ops 판별 근거. */

	u16			nr_plids;	/* [한국어] nr_plids 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] placement identifier 개수. */

	u16			*plids;	/* [한국어] plids 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] placement ID 배열 — 스트림/배치 힌트. 스캔 시 채움, 해제 시 free. */

#ifdef CONFIG_NVME_MULTIPATH	/* [한국어] head multipath 런타임: requeue·current_path·지연 제거 */
	struct bio_list		requeue_list;	/* [한국어] requeue_list 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 경로 없음/전이 중 보류 bio. requeue_work 가 재제출.
	 * 보호: requeue_lock. */

	spinlock_t		requeue_lock;	/* [한국어] requeue_lock 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] requeue_list 스핀락. softirq/프로세스 양쪽에서 bio 조작 시. */

	struct work_struct	requeue_work;	/* [한국어] requeue_work 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 보류 bio 재제출 워커. kick_requeue_lists / 경로 복구 시 스케줄. */

	struct work_struct	partition_scan_work;	/* [한국어] partition_scan_work 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 상위 디스크 파티션 테이블 지연 스캔. live 전환 후. */

	struct mutex		lock;	/* [한국어] lock 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] head multipath 상태(current_path, flags, disk 생명) 변경 뮤텍스. */

	unsigned long		flags;	/* [한국어] flags 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] NVME_NSHEAD_DISK_LIVE / QUEUE_IF_NO_PATH. test_bit 핫패스. */

	struct delayed_work	remove_work;	/* [한국어] remove_work 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 마지막 경로 소실 후 delayed_removal_secs 뒤 head 정리. */

	unsigned int		delayed_removal_secs;	/* [한국어] delayed_removal_secs 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 제거 유예 초. sysfs delayed_removal_secs 로 조정. */

#define NVME_NSHEAD_DISK_LIVE		0	/* [한국어] NVME_NSHEAD_DISK_LIVE 매크로 — 상위 섹션 계약 참고 */
/* [한국어] 상위 multipath 디스크 live — 사용자 제출 허용. 비트 번호 0 */
#define NVME_NSHEAD_QUEUE_IF_NO_PATH	1	/* [한국어] NVME_NSHEAD_QUEUE_IF_NO_PATH 매크로 — 상위 섹션 계약 참고 */
/* [한국어] 경로 전무 시 즉시 실패 대신 requeue. 비트 번호 1. queue_if_no_path 헬퍼 */

	struct nvme_ns __rcu	*current_path[];	/* [한국어] current_path 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] NUMA 노드별 선호 경로 RCU 유연 배열. find_path 가 갱신·반환.
	 * 읽기: rcu/srcu. 클리어: mpath_clear_* . */
#endif	/* [한국어] CONFIG_NVME_MULTIPATH (ns_head multipath 런타임) */
};

/*
 * [한국어] multipath 상위 디스크가 실제로 존재하는지 — head->disk 비NULL 이
 * CONFIG 와 함께 "멀티패스 노드" 임을 나타냄.
 */
static inline bool nvme_ns_head_multipath(struct nvme_ns_head *head)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	return IS_ENABLED(CONFIG_NVME_MULTIPATH) && head->disk;	/* [한국어] Kconfig 게이트 */
}

/*
 * [한국어] ns_head->features 비트 — LBA 포맷·메타데이터·Write Zeroes DEAC.
 */
enum nvme_ns_features {
	NVME_NS_EXT_LBAS = 1 << 0, /* support extended LBA format */ /* [한국어] 스펙 필드·상수 — 상위 블록 아키텍처 참고 */
	/* [한국어] 확장 LBA 포맷(메타데이터 포함 논리 블록) 지원 */

	NVME_NS_METADATA_SUPPORTED = 1 << 1, /* support getting generated md */ /* [한국어] 스펙 필드·상수 — 상위 블록 아키텍처 참고 */
	/* [한국어] 컨트롤러 생성/스트립 메타데이터 경로 지원 */

	NVME_NS_DEAC = 1 << 2,		/* DEAC bit in Write Zeroes supported */ /* [한국어] 스펙 필드·상수 — 상위 블록 아키텍처 참고 */
	/* [한국어] Write Zeroes 의 DEAC(deallocate) 비트 사용 가능 */
};

/*
 * [한국어]
 * struct nvme_ns - 컨트롤러 하나에 보이는 네임스페이스 "경로"
 *
 * queue/disk 는 비멀티패스에서 사용자 블록 장치, 멀티패스에서는 하위 경로
 * 장치. head 가 논리 NS, ctrl 가 물리 경로. flags 비트로 removing/ready 등
 * 수명 단계를 표시.
 */
struct nvme_ns {
	struct list_head list;	/* [한국어] list 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] head->list 상의 경로 노드. 동일 NSID/ids 를 다른 컨트롤러로 본 인스턴스.
	 * 추가: 스캔. 삭제: remove_namespaces/경로 다운. */

	struct nvme_ctrl *ctrl;	/* [한국어] ctrl 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 이 경로의 컨트롤러. 제출 시 nvme_req->ctrl, 큐 한계·상태 공유. */

	struct request_queue *queue;	/* [한국어] queue 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 경로 blk-mq 큐 — 실제 I/O SQ 제출 대상. queuedata 가 보통 이 ns.
	 * multipath 상위 disk 큐와 별개. */

	struct gendisk *disk;	/* [한국어] disk 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 경로 gendisk(nvmeXnY). 비-MPATH 에서는 사용자 가시 디스크.
	 * MPATH 에서는 하위 경로 장치(holder/slave 링크 대상). */

#ifdef CONFIG_NVME_MULTIPATH	/* [한국어] 경로별 ANA 상태·그룹 — 경로 선택 필터 입력 */
	enum nvme_ana_state ana_state;	/* [한국어] ana_state 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] ANA 상태(optimized/non-optimized/inaccessible/persistent loss 등).
	 * 설정: ANA 로그 파싱. 읽는 자: find_path 가 사용 가능 경로 필터. */

	u32 ana_grpid;	/* [한국어] ana_grpid 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] ANA 그룹 ID. 로그 항목과 ns 매칭, sysfs ana_grpid. */
#endif	/* [한국어] CONFIG_NVME_MULTIPATH (ns ANA 필드) */
	struct list_head siblings;	/* [한국어] siblings 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 동일 head 형제 경로 연결. list 와 함께 토폴로지 순회에 사용. */

	struct kref kref;	/* [한국어] kref 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 경로 참조. find_get_ns/get_ns 증가, put_ns 감소. 0 이면 구조체 free. */

	struct nvme_ns_head *head;	/* [한국어] head 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 논리 NS 앵커 — LBA 기하·PI·mpath disk·ids 공유. 필수 비NULL. */

	unsigned long flags;	/* [한국어] flags 필드 — 상위 구조 작성자·동기화 참고 */
#define NVME_NS_REMOVING		0	/* [한국어] NVME_NS_REMOVING 매크로 — 상위 섹션 계약 참고 */
/* [한국어] 제거 진행 — get_ns 실패·신규 제출 거부. 비트 0 */
#define NVME_NS_ANA_PENDING		2	/* [한국어] NVME_NS_ANA_PENDING 매크로 — 상위 섹션 계약 참고 */
/* [한국어] ANA 갱신 중 일시 사용 보류. 비트 2(1 은 역사적 공백) */
#define NVME_NS_FORCE_RO		3	/* [한국어] NVME_NS_FORCE_RO 매크로 — 상위 섹션 계약 참고 */
/* [한국어] 강제 RO — 오류/정책으로 쓰기 차단. 비트 3 */
#define NVME_NS_READY			4	/* [한국어] NVME_NS_READY 매크로 — 상위 섹션 계약 참고 */
/* [한국어] 큐·디스크 준비 완료. ctrl LIVE 와 함께 제출 가능. 비트 4 */
#define NVME_NS_SYSFS_ATTR_LINK	5	/* [한국어] NVME_NS_SYSFS_ATTR_LINK 매크로 — 상위 섹션 계약 참고 */
/* [한국어] multipath sysfs 링크 생성됨. remove 시 대칭 해제. 비트 5 */

	struct cdev		cdev;	/* [한국어] cdev 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 경로 단위 캐릭터 노드(구성 의존). ioctl 진입 가능. */

	struct device		cdev_device;	/* [한국어] cdev_device 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 위 cdev 의 device 객체. */

	struct nvme_fault_inject fault_inject;	/* [한국어] fault_inject 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] NS 단위 fault injection. should_fail 이 disk 있는 요청에서 우선 사용.
	 * admin 전용 요청은 ctrl->fault_inject. */
};

/* NVMe ns supports metadata actions by the controller (generate/strip) */
/*
 * [한국어] 컨트롤러가 PI 를 생성/검증/스트립하는 "보호 정보 내장" 모드인지.
 * pi_type 이 있고 메타데이터 크기가 PI 크기와 같을 때 true.
 */
static inline bool nvme_ns_has_pi(struct nvme_ns_head *head)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	return head->pi_type && head->ms == head->pi_size;	/* [한국어] 호출 결과 반환 */
}

/*
 * [한국어] 기본 가상 경계: 4K-1. 트랜스포트가 ops->get_virt_boundary 로 덮어쓸
 * 수 있으나 공통 폴백은 NVMe 페이지 정렬.
 */
static inline unsigned long nvme_get_virt_boundary(struct nvme_ctrl *ctrl,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
						   bool is_admin)	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
{
	return NVME_CTRL_PAGE_SIZE - 1;	/* [한국어] 4KiB 정렬 마스크 — PRP 경계와 일치 */
}

/*
 * [한국어]
 * struct nvme_ctrl_ops - 트랜스포트 드라이버가 채우는 콜백 테이블
 *
 * core 는 하드웨어 차이를 이 vtable 뒤로 숨긴다. flags 로 fabrics/blocking/
 * metadata 역량을 광고. 필수 수준: free_ctrl/delete 계열과 레지스터 접근
 * (PCIe), fabrics 는 연결 상태 기계와 결합.
 */
struct nvme_ctrl_ops {
	const char *name;	/* [한국어] name 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 트랜스포트 이름("pcie","tcp","rdma","fc","apple" 등).
	 * 로그 접두·sysfs 표시. 정적 문자열, 수명=모듈. */

	struct module *module;	/* [한국어] module 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 소유 모듈. 심볼 수명·try_module_get 으로 언로드 레이스 방지.
	 * 설정: 각 트랜스포트 모듈의 ops 정적 초기화. */

	unsigned int flags;	/* [한국어] flags 필드 — 상위 구조 작성자·동기화 참고 */
#define NVME_F_FABRICS			(1 << 0)	/* [한국어] NVME_F_FABRICS 매크로 — 상위 섹션 계약 참고 */
/* [한국어] NVMe-oF 계열 — 재연결·connect_q·KATO·DELETING 드레인 특수 규칙 활성화 */
#define NVME_F_METADATA_SUPPORTED	(1 << 1)	/* [한국어] NVME_F_METADATA_SUPPORTED 매크로 — 상위 섹션 계약 참고 */
/* [한국어] 메타데이터/PI 매핑 지원 광고. 큐 limits·명령 조립 분기 */
#define NVME_F_BLOCKING			(1 << 2)	/* [한국어] NVME_F_BLOCKING 매크로 — 상위 섹션 계약 참고 */
/* [한국어] queue_rq 가 잠들 수 있음 — blk-mq BLOCKING 태그 정책과 맞춤(tcp 등) */

	const struct attribute_group **dev_attr_groups;	/* [한국어] dev_attr_groups 필드 — 상위 구조 작성자·동기화 참고 */
	/* [한국어] 트랜스포트 전용 sysfs 속성 그룹 배열. 컨트롤러 device 에 병합. */

	int (*reg_read32)(struct nvme_ctrl *ctrl, u32 off, u32 *val);	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
	/* [한국어] 32bit 레지스터/속성 읽기. PCIe: MMIO CAP/CC/CSTS. fabrics: 속성 페이지.
	 * 실패 시 음수 errno. enable/disable·폴링 경로 핵심. */

	int (*reg_write32)(struct nvme_ctrl *ctrl, u32 off, u32 val);	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
	/* [한국어] 32bit 쓰기 — CC.EN, 셧다운 통지 등. 메모리 배리어는 구현체 책임. */

	int (*reg_read64)(struct nvme_ctrl *ctrl, u32 off, u64 *val);	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
	/* [한국어] 64bit 읽기 — CAP 전체 등. 32bit 두 번보다 원자성·편의. */

	void (*free_ctrl)(struct nvme_ctrl *ctrl);	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
	/* [한국어] 트랜스포트 private 최종 해제(container_of 로 확장 구조체 kfree).
	 * 호출: 참조 0 이후 core release 경로. 이후 ctrl 접근 금지. */

	void (*submit_async_event)(struct nvme_ctrl *ctrl);	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
	/* [한국어] AER admin 명령을 다시 슬롯에 넣기. 한 슬롯 완료 후 재무장. */

	int (*subsystem_reset)(struct nvme_ctrl *ctrl);	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
	/* [한국어] NSSR 등 서브시스템 리셋. 미지원 시 NULL → -ENOTTY.
	 * 다중 컨트롤러 토폴로지에서 광역 리셋. */

	void (*delete_ctrl)(struct nvme_ctrl *ctrl);	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
	/* [한국어] 트랜스포트 삭제 시작 — 연결 종료·하드웨어 큐 파괴 예약.
	 * core delete_work 와 협력. */

	void (*stop_ctrl)(struct nvme_ctrl *ctrl);	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
	/* [한국어] 런타임 stop — 타이머/폴링 중단 등. quiesce 전후 호출 가능. */

	int (*get_address)(struct nvme_ctrl *ctrl, char *buf, int size);	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
	/* [한국어] fabrics 주소 문자열을 buf 에 기록(sysfs address). PCIe 는 NULL 가능. */

	void (*print_device_info)(struct nvme_ctrl *ctrl);	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
	/* [한국어] 치명 오류 시 BDF 등 트랜스포트 특화 로그. NULL 이면 VID/model 폴백. */

	bool (*supports_pci_p2pdma)(struct nvme_ctrl *ctrl);	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
	/* [한국어] P2PDMA 가능 여부. PCIe 외 보통 false. 맵 경로 최적화 분기. */

	unsigned long (*get_virt_boundary)(struct nvme_ctrl *ctrl, bool is_admin);	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
	/* [한국어] 큐 virt boundary 커스터마이즈. NULL 시 nvme_get_virt_boundary(4K-1).
	 * is_admin 으로 admin/IO 다른 정렬 가능. */
};

/*
 * nvme command_id is constructed as such:
 * | xxxx | xxxxxxxxxxxx |
 *   gen    request tag
 */
/*
 * [한국어] 16-bit Command Identifier 레이아웃.
 * 하위 12비트 = blk-mq tag (동시 인플라이트 인덱스),
 * 상위 4비트 = genctr 하위 니블 — tag 재사용 후 stale CQE 탐지.
 * SKIP_CID_GEN quirk 장치는 gen 을 쓰지 않고 tag 만 사용.
 */
#define nvme_genctr_mask(gen)			(gen & 0xf)	/* [한국어] nvme_genctr_mask 매크로 — 상위 섹션 계약 참고 */
/* [한국어] genctr → CID 에 들어갈 4비트 마스크 */
#define nvme_cid_install_genctr(gen)		(nvme_genctr_mask(gen) << 12)	/* [한국어] nvme_cid_install_genctr 매크로 — 상위 섹션 계약 참고 */
/* [한국어] 4비트 gen 을 CID 상위 니블 위치로 시프트 */
#define nvme_genctr_from_cid(cid)		((cid & 0xf000) >> 12)	/* [한국어] nvme_genctr_from_cid 매크로 — 상위 섹션 계약 참고 */
/* [한국어] CQE command_id 에서 gen 니블 추출 */
#define nvme_tag_from_cid(cid)			(cid & 0xfff)	/* [한국어] nvme_tag_from_cid 매크로 — 상위 섹션 계약 참고 */
/* [한국어] CQE command_id 에서 blk-mq tag 추출 */

/*
 * [한국어] 제출 시 SQE.command_id 에 기록할 값 = gen|tag.
 * 완료 경로 nvme_find_rq 가 대칭적으로 분해·검증.
 */
static inline u16 nvme_cid(struct request *rq)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	return nvme_cid_install_genctr(nvme_req(rq)->genctr) | rq->tag;	/* [한국어] 호출 결과 반환 */
}

/*
 * [한국어]
 * nvme_find_rq - CQE 의 command_id 로 inflight request 를 안전하게 회수
 *
 * tag 로 request 를 찾은 뒤 genctr 불일치면 NULL (stale/위조 완료).
 * IRQ 완료 핫패스에서 호출. tags 는 admin 또는 I/O hctx tag 공간.
 */
static inline struct request *nvme_find_rq(struct blk_mq_tags *tags,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		u16 command_id)	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
{
	u8 genctr = nvme_genctr_from_cid(command_id);	/* [한국어] CQE 가 주장하는 세대 */
	u16 tag = nvme_tag_from_cid(command_id);	/* [한국어] 태그 인덱스 */
	struct request *rq;	/* [한국어] rq 필드 — 상위 구조 작성자·동기화 참고 */

	rq = blk_mq_tag_to_rq(tags, tag);	/* [한국어] tag → request 포인터 (인플라이트 테이블) */
	if (unlikely(!rq)) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		pr_err("could not locate request for tag %#x\n",	/* [한국어] 진단 로그 */
			tag);	/* [한국어] 이미 완료됐거나 잘못된 tag — 드롭 */
		return NULL;	/* [한국어] 대상 없음 */
	}
	if (unlikely(nvme_genctr_mask(nvme_req(rq)->genctr) != genctr)) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		dev_err(nvme_req(rq)->ctrl->device,	/* [한국어] 진단 로그 */
			"request %#x genctr mismatch (got %#x expected %#x)\n",	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
			tag, genctr, nvme_genctr_mask(nvme_req(rq)->genctr));	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
		/* [한국어] tag 재사용 후 늦은 CQE — 데이터 오염 방지를 위해 무시 */
		return NULL;	/* [한국어] 대상 없음 */
	}
	return rq;	/* [한국어] 일치: 이 요청의 정상 완료로 진행 */
}

/*
 * [한국어] genctr 검증 없이 tag 만으로 request 조회 — 취소 일괄 처리 등
 * 세대 검사 불필요/불가능한 경로용. 완료 매칭에는 find_rq 를 써야 한다.
 */
static inline struct request *nvme_cid_to_rq(struct blk_mq_tags *tags,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
                u16 command_id)	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
{
	return blk_mq_tag_to_rq(tags, nvme_tag_from_cid(command_id));	/* [한국어] blk-mq/큐 계층 API */
}

/*
 * Return the length of the string without the space padding
 */
/*
 * [한국어] NVMe Identify 문자열은 오른쪽 스페이스 패딩. 로그 출력 시 가독성을
 * 위해 유효 길이만 계산 (strnlen 계열 대체, 중간 NUL 없는 스펙 문자열).
 */
static inline int nvme_strlen(char *s, int len)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	while (s[len - 1] == ' ')	/* [한국어] 순회 루프 */
		len--;	/* [한국어] 우측 패딩 스페이스 제거 */
	return len;	/* [한국어] 호출 결과 반환 */
}

/*
 * [한국어] 치명적 오류 로그 시 장치 식별 문자열 출력.
 * 트랜스포트 print_device_info 가 있으면 위임, 없으면 VID/model/fw 공통 형식.
 */
static inline void nvme_print_device_info(struct nvme_ctrl *ctrl)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	struct nvme_subsystem *subsys = ctrl->subsys;	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */

	if (ctrl->ops->print_device_info) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		ctrl->ops->print_device_info(ctrl);	/* [한국어] 예: PCIe BDF 등 풍부한 정보 */
		return;	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
	}

	dev_err(ctrl->device,	/* [한국어] 진단 로그 */
		"VID:%04x model:%.*s firmware:%.*s\n", subsys->vendor_id,	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
		nvme_strlen(subsys->model, sizeof(subsys->model)),	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
		subsys->model, nvme_strlen(subsys->firmware_rev,	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
					   sizeof(subsys->firmware_rev)),	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
		subsys->firmware_rev);	/* [한국어] 서브시스템 Identify 기반 폴백 로그 */
}

#ifdef CONFIG_FAULT_INJECTION_DEBUG_FS
/* [한국어] fault injection 실구현 — debugfs 로 확률·status·DNR 조절 (fault_inject.c) */
void nvme_fault_inject_init(struct nvme_fault_inject *fault_inj,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
			    const char *dev_name);	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
/* [한국어] 장치별 debugfs fault_inject 트리 생성. dev_name 예: nvme0 / nvme0n1 */
void nvme_fault_inject_fini(struct nvme_fault_inject *fault_inject);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] debugfs recursive remove 및 속성 정리. 장치 해제 경로. */
void nvme_should_fail(struct request *req);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] 완료 직전 확률적 status 덮어쓰기 — multipath/재시도/에러 매핑 검증 훅 */
#else
/* [한국어] fault injection 미빌드: 빈 인라인으로 호출부 공통 유지, 핫패스 비용 0 */
static inline void nvme_fault_inject_init(struct nvme_fault_inject *fault_inj,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
					  const char *dev_name)	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
{
	/* [한국어] 설정 꺼짐: no-op — 호출부 ifdefs 최소화 */
}
static inline void nvme_fault_inject_fini(struct nvme_fault_inject *fault_inj)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	/* [한국어] 설정 꺼짐: no-op */
}
static inline void nvme_should_fail(struct request *req) {}	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
/* [한국어] 설정 꺼짐: 완료 경로 비용 0 */
#endif

bool nvme_wait_reset(struct nvme_ctrl *ctrl);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] 리셋 완료·상태 안정화까지 state_wq 대기. 프로세스 컨텍스트 전용.
 * 터미널 상태면 false 등으로 실패를 알릴 수 있음(구현 정의). */
int nvme_try_sched_reset(struct nvme_ctrl *ctrl);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] 상태가 허용하면 reset_work 를 nvme_reset_wq 에 스케줄.
 * 이미 리셋 중이거나 터미널이면 경합 실패 코드. 비동기 진입점. */

/*
 * [한국어] 서브시스템 리셋 진입. subsystem 플래그와 ops 지원이 있을 때만.
 * 미지원 시 -ENOTTY — ioctl 경로가 사용자에 전달.
 */
static inline int nvme_reset_subsystem(struct nvme_ctrl *ctrl)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	if (!ctrl->subsystem || !ctrl->ops->subsystem_reset)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		return -ENOTTY;	/* [한국어] 토폴로지/트랜스포트가 NSSR 류 미지원 */
	return ctrl->ops->subsystem_reset(ctrl);	/* [한국어] 호출 결과 반환 */
}

/*
 * Convert a 512B sector number to a device logical block number.
 */
/*
 * [한국어] 블록 계층 512B 섹터 → 장치 LBA. lba_shift 는 ns 포맷(예: 4K면 12).
 * SECTOR_SHIFT(9) 와의 차이가 시프트량.
 */
static inline u64 nvme_sect_to_lba(struct nvme_ns_head *head, sector_t sector)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	return sector >> (head->lba_shift - SECTOR_SHIFT);	/* [한국어] 호출 결과 반환 */
}

/*
 * Convert a device logical block number to a 512B sector number.
 */
static inline sector_t nvme_lba_to_sect(struct nvme_ns_head *head, u64 lba)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	return lba << (head->lba_shift - SECTOR_SHIFT);	/* [한국어] LBA → 512B 섹터 */
}

/*
 * Convert byte length to nvme's 0-based num dwords
 */
/*
 * [한국어] 바이트 길이를 NVMe "0-based DWORD 수" 로: (len/4)-1.
 * Identify/Get Log 등 전송 길이 필드 인코딩.
 */
static inline u32 nvme_bytes_to_numd(size_t len)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	return (len >> 2) - 1;	/* [한국어] 바이트→DWORD 후 0-based 인코딩 */
}

/* Decode a 2-byte "0's based"/"0-based" field */
/*
 * [한국어] 리틀엔디안 0-based 필드를 실제 개수(N+1)로 변환 — 큐 깊이 등.
 */
static inline u32 from0based(__le16 value)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	return (u32)le16_to_cpu(value) + 1;	/* [한국어] 스펙 0-based → 호스트 개수 */
}

/*
 * [한국어] ANA 관련 status 인지 — failover/경로 재선택 트리거.
 * SCT/SC 마스크로 phase 비트를 제거한 비교.
 */
static inline bool nvme_is_ana_error(u16 status)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	switch (status & NVME_SCT_SC_MASK) {	/* [한국어] phase 비트 제거 후 SCT+SC 비교 */
	case NVME_SC_ANA_TRANSITION:	/* [한국어] 다중 분기 케이스 */
		/* [한국어] 전이 중 — 잠시 후 재시도·경로 갱신 */
	case NVME_SC_ANA_INACCESSIBLE:	/* [한국어] 다중 분기 케이스 */
		/* [한국어] 이 경로에서 NS 접근 불가 — 다른 경로 필요 */
	case NVME_SC_ANA_PERSISTENT_LOSS:	/* [한국어] 다중 분기 케이스 */
		/* [한국어] 영구 손실 — 경로 제외 */
		return true;	/* [한국어] multipath failover 후보 status */
	default:	/* [한국어] 예약/미지 값 방어 */
		return false;	/* [한국어] ANA 비관련 완료 상태 */
	}
}

/*
 * [한국어] Path Related status type 전체 — fabric 경로 오류 등.
 * multipath failover 와 host_path_error 분류에 사용.
 */
static inline bool nvme_is_path_error(u16 status)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	/* check for a status code type of 'path related status' */
	return (status & NVME_SCT_MASK) == NVME_SCT_PATH;	/* [한국어] 호출 결과 반환 */
}

/*
 * Fill in the status and result information from the CQE, and then figure out
 * if blk-mq will need to use IPI magic to complete the request, and if yes do
 * so.  If not let the caller complete the request without an indirect function
 * call.
 */
/*
 * [한국어]
 * nvme_try_complete_req - 트랜스포트 완료 핫패스의 공통 종착 직전 단계
 *
 * 1) genctr++ (재사용 혼동 방지; quirk 시 생략)
 * 2) CQE status/result 를 nvme_request 에 저장 (status 는 >>1 로 phase 제거 관례)
 * 3) fault injection
 * 4) fake timeout 디버그 시 완료 억제
 * 5) blk_mq_complete_request_remote: 다른 CPU 면 IPI, 같으면 false 반환 →
 *    호출자가 직접 nvme_complete_rq 가능 (간접 호출 절약)
 *
 * @return: true 면 원격/가짜 처리로 호출자 추가 완료 불필요, false 면 로컬 완료 계속.
 */
static inline bool nvme_try_complete_req(struct request *req, __le16 status,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		union nvme_result result)	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
{
	struct nvme_request *rq = nvme_req(req);	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
	struct nvme_ctrl *ctrl = rq->ctrl;	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */

	if (!(ctrl->quirks & NVME_QUIRK_SKIP_CID_GEN))	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		rq->genctr++;	/* [한국어] 다음 제출 CID 세대 갱신 — stale CQE 차단 */

	rq->status = le16_to_cpu(status) >> 1;	/* [한국어] 호스트 status 형식(phase 제외) */
	rq->result = result;	/* [한국어] CQE 결과 DW 보존 */
	/* inject error when permitted by fault injection framework */
	nvme_should_fail(req);	/* [한국어] 디버그 빌드에서 status 덮어쓰기 가능 */
	if (unlikely(blk_should_fake_timeout(req->q)))	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		return true;	/* [한국어] 타임아웃 테스트: 정상 완료 삼킴 */
	return blk_mq_complete_request_remote(req);	/* [한국어] blk-mq/큐 계층 API */
	/* [한국어] true=IPI 로 complete 예약, false=같은 CPU 에서 호출자 완료 */
}

/*
 * [한국어] ctrl 수명 참조 +1. device 모델 get — 캐릭터 노드/파일 핸들 경로.
 */
static inline void nvme_get_ctrl(struct nvme_ctrl *ctrl)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	get_device(ctrl->device);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
}

/*
 * [한국어] 대칭 put. 마지막 참조에서 release 가 uninit/free 로 이어질 수 있음.
 */
static inline void nvme_put_ctrl(struct nvme_ctrl *ctrl)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	put_device(ctrl->device);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
}

/*
 * [한국어] Admin 큐(qid=0) 이면서 tag 가 일반 admin 깊이 이상이면
 * Asynchronous Event 예약 슬롯 요청으로 간주. 완료 경로에서 일반 I/O 와 분리.
 */
static inline bool nvme_is_aen_req(u16 qid, __u16 command_id)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	return !qid &&	/* [한국어] 호출 결과 반환 */
		nvme_tag_from_cid(command_id) >= NVME_AQ_BLK_MQ_DEPTH;	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
}

/*
 * Returns true for sink states that can't ever transition back to live.
 */
/*
 * [한국어] 터미널(싱크) 상태 판정. DELETING 계열·DEAD 는 LIVE 복귀 불가.
 * 리셋 재시도와 삭제 경로 분기, 대기 조건에 사용. 미처리 state 는 WARN 후 터미널 취급.
 */
static inline bool nvme_state_terminal(struct nvme_ctrl *ctrl)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	switch (nvme_ctrl_state(ctrl)) {	/* [한국어] 컨트롤러 상태 원자 스냅샷 */
	case NVME_CTRL_NEW:	/* [한국어] 다중 분기 케이스 */
	case NVME_CTRL_LIVE:	/* [한국어] 다중 분기 케이스 */
	case NVME_CTRL_RESETTING:	/* [한국어] 다중 분기 케이스 */
	case NVME_CTRL_CONNECTING:	/* [한국어] 다중 분기 케이스 */
		return false;	/* [한국어] 아직 회복·진행 가능 상태 */
	case NVME_CTRL_DELETING:	/* [한국어] 다중 분기 케이스 */
	case NVME_CTRL_DELETING_NOIO:	/* [한국어] 다중 분기 케이스 */
	case NVME_CTRL_DEAD:	/* [한국어] 다중 분기 케이스 */
		return true;	/* [한국어] 삭제/사망 — 재LIVE 불가 */
	default:	/* [한국어] 예약/미지 값 방어 */
		WARN_ONCE(1, "Unhandled ctrl state:%d", ctrl->state);	/* [한국어] 불변식 위반 경고 */
		return true;	/* [한국어] 알 수 없는 값은 안전하게 터미널로 취급 */
	}
}

/*
 * [한국어] === 완료·취소·상태 전이·큐 수명 API (core.c 구현) ===
 * 트랜스포트는 CQE 에서 nvme_try_complete_req 까지 담당하고, 아래 심볼이
 * status 해석·재시도·mpath failover·blk-mq endio·리셋/동결을 이어받는다.
 * 인플라이트 취소는 tagset busy iterator + cancel_* 로 리셋/삭제 시 강제 완료.
 */
void nvme_end_req(struct request *req);	/* [한국어] 요청 완료 하단 */
/* [한국어] status→blk_status 변환 후 블록 계층에 요청 종료(에러 매핑·mpath end 포함) */
void nvme_complete_rq(struct request *req);	/* [한국어] 요청 완료 하단 */
/* [한국어] 단일 요청 완료 하단: 재시도·failover·end_req 결정. softirq 또는 프로세스 */
void nvme_complete_batch_req(struct request *req);	/* [한국어] 요청 완료 하단 */
/* [한국어] 배치 완료 시 요청별 NVMe 후처리 — blk_mq_end_request_batch 직전 호출 */

/*
 * [한국어] 완료 배치 헬퍼: 각 req 에 트랜스포트 fn(예: 메타 해제) 후
 * complete_batch_req, 마지막에 blk_mq_end_request_batch.
 * 인터럽트 병합 완료 경로 최적화.
 */
static __always_inline void nvme_complete_batch(struct io_comp_batch *iob,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
						void (*fn)(struct request *rq))	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	struct request *req;	/* [한국어] req 필드 — 상위 구조 작성자·동기화 참고 */

	rq_list_for_each(&iob->req_list, req) {
		fn(req);	/* [한국어] 트랜스포트 private 정리 콜백 */
		nvme_complete_batch_req(req);	/* [한국어] NVMe 공통 완료 회계 */
	}
	blk_mq_end_request_batch(iob);	/* [한국어] 블록 계층 일괄 endio */
}

blk_status_t nvme_host_path_error(struct request *req);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] path 오류를 blk_status 로 변환하고 mpath 정책 적용 */
bool nvme_cancel_request(struct request *req, void *data);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] tagset busy iter 콜백 — 인플라이트 요청 취소 표시·완료 */
void nvme_cancel_tagset(struct nvme_ctrl *ctrl);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] I/O tagset 전체 취소 — 리셋/삭제 시 */
void nvme_cancel_admin_tagset(struct nvme_ctrl *ctrl);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] admin tagset 취소 — AER 포함 정리 */
bool nvme_change_ctrl_state(struct nvme_ctrl *ctrl,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		enum nvme_ctrl_state new_state);	/* [한국어] 컨트롤러 상태 원자 스냅샷 */
/* [한국어] 합법 전이만 수행·state_wq 웨이크. 성공 여부 반환. 상태 머신 핵심 API */
int nvme_disable_ctrl(struct nvme_ctrl *ctrl, bool shutdown);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] CC.EN=0 또는 shutdown 통지 — RDY/SHST 대기 포함 */
int nvme_enable_ctrl(struct nvme_ctrl *ctrl);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] CC 설정 후 enable — RDY 폴링( quirk 지연 포함) */
int nvme_init_ctrl(struct nvme_ctrl *ctrl, struct device *dev,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		const struct nvme_ctrl_ops *ops, unsigned long quirks);	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
/* [한국어] 공통 필드·락·워크·인스턴스 초기화. 트랜스포트 probe 초반 호출 */
int nvme_add_ctrl(struct nvme_ctrl *ctrl);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] 서브시스템 연결·sysfs 등록 등 가시화 단계 */
void nvme_uninit_ctrl(struct nvme_ctrl *ctrl);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] add 의 대칭 해제 — 아직 free_ctrl 전 */
void nvme_start_ctrl(struct nvme_ctrl *ctrl);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] LIVE 진입 후 큐 재개·스캔·KA 시작 */
void nvme_stop_ctrl(struct nvme_ctrl *ctrl);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] 백그라운드 작업 정지·큐 quiesce 방향 */
int nvme_init_ctrl_finish(struct nvme_ctrl *ctrl, bool was_suspended);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] Identify 이후 한계 적용·hwmon 등 마무리. 재개 시 was_suspended */
int nvme_alloc_admin_tag_set(struct nvme_ctrl *ctrl, struct blk_mq_tag_set *set,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		const struct blk_mq_ops *ops, unsigned int cmd_size);	/* [한국어] blk-mq/큐 계층 API */
/* [한국어] admin blk-mq tag set+큐 생성. cmd_size 에 nvme_request+private */
void nvme_remove_admin_tag_set(struct nvme_ctrl *ctrl);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] admin tag set 제거 */
int nvme_alloc_io_tag_set(struct nvme_ctrl *ctrl, struct blk_mq_tag_set *set,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		const struct blk_mq_ops *ops, unsigned int nr_maps,	/* [한국어] blk-mq/큐 계층 API */
		unsigned int cmd_size);	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
/* [한국어] I/O tag set. nr_maps 는 read/write/poll 맵 수 */
void nvme_remove_io_tag_set(struct nvme_ctrl *ctrl);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] I/O tag set 제거 */

void nvme_remove_namespaces(struct nvme_ctrl *ctrl);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] 컨트롤러의 모든 NS 경로 제거 — 삭제/리셋 경로 */

void nvme_complete_async_event(struct nvme_ctrl *ctrl, __le16 status,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		volatile union nvme_result *res);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] AER CQE 도착 처리 — aen_result 저장 후 async_event_work 스케줄 */

void nvme_quiesce_io_queues(struct nvme_ctrl *ctrl);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] 모든 I/O 큐 quiesce — 신규 디스패치 중단 */
void nvme_unquiesce_io_queues(struct nvme_ctrl *ctrl);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] I/O 큐 재개 */
void nvme_quiesce_admin_queue(struct nvme_ctrl *ctrl);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] admin 큐 quiesce */
void nvme_unquiesce_admin_queue(struct nvme_ctrl *ctrl);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] admin 큐 재개 */
void nvme_mark_namespaces_dead(struct nvme_ctrl *ctrl);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] NS 디스크를 dead 로 표시 — 상위 파일시스템 오류 전파 */
void nvme_sync_queues(struct nvme_ctrl *ctrl);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] admin+IO 인플라이트 드레인 */
void nvme_sync_io_queues(struct nvme_ctrl *ctrl);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] I/O 만 드레인 */
void nvme_unfreeze(struct nvme_ctrl *ctrl);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] blk-mq freeze 해제 — 제출 재개 */
void nvme_wait_freeze(struct nvme_ctrl *ctrl);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] freeze 완료 대기(인플라이트 소진) */
int nvme_wait_freeze_timeout(struct nvme_ctrl *ctrl, long timeout);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] 제한 시간 freeze 대기 */
void nvme_start_freeze(struct nvme_ctrl *ctrl);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] 전 큐 freeze 시작 — effects 있는 passthrough·리셋 전 */

/*
 * [한국어] passthrough 방향 → req_op. NVMe write 계열이면 DRV_OUT.
 */
static inline enum req_op nvme_req_op(struct nvme_command *cmd)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	return nvme_is_write(cmd) ? REQ_OP_DRV_OUT : REQ_OP_DRV_IN;	/* [한국어] 호출 결과 반환 */
}

#define NVME_QID_ANY -1	/* [한국어] NVME_QID_ANY 매크로 — 상위 섹션 계약 참고 */
/* [한국어] 동기 제출 시 qid 미지정 — 코어가 적절한 큐 선택 */

void nvme_init_request(struct request *req, struct nvme_command *cmd);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] PDU 에 cmd/ctrl 등 기본 필드 초기화 */
void nvme_cleanup_cmd(struct request *req);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] 명령 관련 임시 버퍼(special payload) 해제 */
blk_status_t nvme_setup_cmd(struct nvme_ns *ns, struct request *req);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] bio/request 를 NVMe SQE 로 변환(읽기/쓰기/discard/존 등). 핫패스 */
blk_status_t nvme_fail_nonready_command(struct nvme_ctrl *ctrl,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		struct request *req);	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
/* [한국어] 비LIVE 에서 실패한 명령에 적절한 blk_status 부여 */
bool __nvme_check_ready(struct nvme_ctrl *ctrl, struct request *rq,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		bool queue_live, enum nvme_ctrl_state state);	/* [한국어] 컨트롤러 상태 원자 스냅샷 */
/* [한국어] 상태별 상세 제출 허용 여부 — LIVE 빠른 경로 밖의 슬로우 패스 */

/*
 * [한국어] 제출 직전 준비 상태 가드. LIVE 면 거의 항상 true.
 * fabrics 가 DELETING 중이어도 queue_live 면 드레인 허용 등 특수 규칙.
 */
static inline bool nvme_check_ready(struct nvme_ctrl *ctrl, struct request *rq,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		bool queue_live)	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
{
	enum nvme_ctrl_state state = nvme_ctrl_state(ctrl);	/* [한국어] 컨트롤러 상태 원자 스냅샷 */

	if (likely(state == NVME_CTRL_LIVE))	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		return true;	/* [한국어] 핫패스: 정상 서비스 중 */
	if (ctrl->ops->flags & NVME_F_FABRICS && state == NVME_CTRL_DELETING)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		return queue_live;	/* [한국어] fabrics 삭제 중 잔여 큐 드레인 허용 여부 */
	return __nvme_check_ready(ctrl, rq, queue_live, state);	/* [한국어] 호출 결과 반환 */
	/* [한국어] RESETTING/CONNECTING 등 세분 규칙 */
}

/*
 * NSID shall be unique for all shared namespaces, or if at least one of the
 * following conditions is met:
 *   1. Namespace Management is supported by the controller
 *   2. ANA is supported by the controller
 *   3. NVM Set are supported by the controller
 *
 * In other case, private namespace are not required to report a unique NSID.
 */
/*
 * [한국어] NSID 유일성 보장 여부 — multipath 매칭·사용자 가시 NSID 정책.
 * 공유 NS 이거나 NS mgmt/ANA/NVM Set 지원 시 유일하다고 간주.
 */
static inline bool nvme_is_unique_nsid(struct nvme_ctrl *ctrl,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		struct nvme_ns_head *head)	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
{
	return head->shared ||	/* [한국어] 호출 결과 반환 */
		(ctrl->oacs & NVME_CTRL_OACS_NS_MNGT_SUPP) ||
		(ctrl->subsys->cmic & NVME_CTRL_CMIC_ANA) ||
		(ctrl->ctratt & NVME_CTRL_CTRATT_NVM_SETS);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
}

/*
 * Flags for __nvme_submit_sync_cmd()
 */
typedef __u32 __bitwise nvme_submit_flags_t;	/* [한국어] nvme_submit_flags_t 필드 — 상위 구조 작성자·동기화 참고 */
/* [한국어] 동기 제출 플래그 타입 — 일반 u32 와 섞임 방지용 bitwise */

enum {
	/* Insert request at the head of the queue */
	NVME_SUBMIT_AT_HEAD  = (__force nvme_submit_flags_t)(1 << 0),	/* [한국어] NVME_SUBMIT_AT_HEAD 상수 — 상위 enum 역할 참고 */
	/* [한국어] 큐 헤드 삽입 — 긴급 admin/복구 명령 */

	/* Set BLK_MQ_REQ_NOWAIT when allocating request */
	NVME_SUBMIT_NOWAIT = (__force nvme_submit_flags_t)(1 << 1),	/* [한국어] NVME_SUBMIT_NOWAIT 상수 — 상위 enum 역할 참고 */
	/* [한국어] 태그 없으면 잠들지 않고 -EAGAIN 계열 */

	/* Set BLK_MQ_REQ_RESERVED when allocating request */
	NVME_SUBMIT_RESERVED = (__force nvme_submit_flags_t)(1 << 2),	/* [한국어] NVME_SUBMIT_RESERVED 상수 — 상위 enum 역할 참고 */
	/* [한국어] 예약 태그 풀 사용 — 교착 방지용 admin */

	/* Retry command when NVME_STATUS_DNR is not set in the result */
	NVME_SUBMIT_RETRY = (__force nvme_submit_flags_t)(1 << 3),	/* [한국어] NVME_SUBMIT_RETRY 상수 — 상위 enum 역할 참고 */
	/* [한국어] DNR 아닌 실패 시 호스트 재시도 */
};

int nvme_submit_sync_cmd(struct request_queue *q, struct nvme_command *cmd,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		void *buf, unsigned bufflen);	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
/* [한국어] 간단 동기 제출 래퍼 — 버퍼 optional, 결과 유니온 무시 가능 */
int __nvme_submit_sync_cmd(struct request_queue *q, struct nvme_command *cmd,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		union nvme_result *result, void *buffer, unsigned bufflen,	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
		int qid, nvme_submit_flags_t flags);	/* [한국어] 명령 제출 경로 */
/* [한국어] 동기 제출 본체 — qid/flags/result 완전 제어. 초기화·ioctl 핵심 */
int nvme_set_features(struct nvme_ctrl *dev, unsigned int fid,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		      unsigned int dword11, void *buffer, size_t buflen,	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
		      void *result);	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
/* [한국어] Set Features 헬퍼 — 큐 수, APST, KATO, 호스트 동작 등 */
int nvme_get_features(struct nvme_ctrl *dev, unsigned int fid,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		      unsigned int dword11, void *buffer, size_t buflen,	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
		      void *result);	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
/* [한국어] Get Features 헬퍼 */
int nvme_set_queue_count(struct nvme_ctrl *ctrl, int *count);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] 원하는 IO 큐 수 협상 — 입출력 *count 갱신 */
void nvme_stop_keep_alive(struct nvme_ctrl *ctrl);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] ka_work 취소 — stop/delete 경로 */
int nvme_reset_ctrl(struct nvme_ctrl *ctrl);	/* [한국어] 비동기 리셋 워크 스케줄 */
/* [한국어] 비동기 리셋 요청 */
int nvme_reset_ctrl_sync(struct nvme_ctrl *ctrl);	/* [한국어] 동기 컨트롤러 리셋 — 상태머신·큐 재수립 완료 대기 */
/* [한국어] 리셋 스케줄 후 완료 대기 */
int nvme_delete_ctrl(struct nvme_ctrl *ctrl);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] 비동기 삭제 요청 — delete_work */
void nvme_queue_scan(struct nvme_ctrl *ctrl);	/* [한국어] 네임스페이스 스캔 워크 스케줄 */
/* [한국어] scan_work 스케줄 — NS 변화/AER/시작 후 */
int nvme_get_log(struct nvme_ctrl *ctrl, u32 nsid, u8 log_page, u8 lsp, u8 csi,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		void *log, size_t size, u64 offset);	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
/* [한국어] Get Log Page 공통 헬퍼 — SMART/ANA/effects/FW 등 */
bool nvme_tryget_ns_head(struct nvme_ns_head *head);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] head kref 시도 — 제거 레이스 시 false */
void nvme_put_ns_head(struct nvme_ns_head *head);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] head 참조 해제 */
int nvme_cdev_add(struct cdev *cdev, struct device *cdev_device,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		const struct file_operations *fops, struct module *owner);	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
/* [한국어] NVMe 캐릭터 디바이스 등록 헬퍼 */
void nvme_cdev_del(struct cdev *cdev, struct device *cdev_device);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] 대칭 해제 */
int nvme_ioctl(struct block_device *bdev, blk_mode_t mode,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		unsigned int cmd, unsigned long arg);	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
/* [한국어] 경로 디스크 ioctl — passthrough 등 (ioctl.c) */
long nvme_ns_chr_ioctl(struct file *file, unsigned int cmd, unsigned long arg);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] NS 캐릭터 노드 ioctl */
int nvme_ns_head_ioctl(struct block_device *bdev, blk_mode_t mode,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		unsigned int cmd, unsigned long arg);	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
/* [한국어] multipath 상위 디스크 ioctl */
long nvme_ns_head_chr_ioctl(struct file *file, unsigned int cmd,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		unsigned long arg);	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
/* [한국어] multipath 상위 캐릭터 ioctl */
long nvme_dev_ioctl(struct file *file, unsigned int cmd,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		unsigned long arg);	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
/* [한국어] 컨트롤러 /dev/nvmeX ioctl */
int nvme_ns_chr_uring_cmd_iopoll(struct io_uring_cmd *ioucmd,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		struct io_comp_batch *iob, unsigned int poll_flags);	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
/* [한국어] io_uring NS 명령 IOPOLL 완료 */
int nvme_ns_chr_uring_cmd(struct io_uring_cmd *ioucmd,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		unsigned int issue_flags);	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
/* [한국어] io_uring NS passthrough 제출 */
int nvme_ns_head_chr_uring_cmd(struct io_uring_cmd *ioucmd,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		unsigned int issue_flags);	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
/* [한국어] io_uring multipath head passthrough */
int nvme_identify_ns(struct nvme_ctrl *ctrl, unsigned nsid,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		struct nvme_id_ns **id);	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
/* [한국어] Identify NS 할당·조회 — 호출자가 id 해제 */
int nvme_getgeo(struct gendisk *disk, struct hd_geometry *geo);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] HDIO_GETGEO 호환 기하 정보 */
int nvme_dev_uring_cmd(struct io_uring_cmd *ioucmd, unsigned int issue_flags);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] 컨트롤러 노드 io_uring admin 명령 */

extern const struct attribute_group *nvme_ns_attr_groups[];	/* [한국어] nvme_ns_attr_groups 필드 — 상위 구조 작성자·동기화 참고 */
/* [한국어] NS sysfs 속성 그룹 배열 (sysfs.c) */
extern const struct attribute_group nvme_ns_mpath_attr_group;	/* [한국어] nvme_ns_mpath_attr_group 필드 — 상위 구조 작성자·동기화 참고 */
/* [한국어] multipath NS 추가 속성 */
extern const struct pr_ops nvme_pr_ops;	/* [한국어] nvme_pr_ops 필드 — 상위 구조 작성자·동기화 참고 */
/* [한국어] PR 콜백 — 상단 선언과 동일 심볼(호환 중복 노출) */
extern const struct block_device_operations nvme_ns_head_ops;	/* [한국어] nvme_ns_head_ops 필드 — 상위 구조 작성자·동기화 참고 */
/* [한국어] multipath 상위 gendisk fops — disk_is_ns_head 판별 키 */
extern const struct attribute_group nvme_dev_attrs_group;	/* [한국어] nvme_dev_attrs_group 필드 — 상위 구조 작성자·동기화 참고 */
/* [한국어] 컨트롤러 디바이스 속성 그룹 */
extern const struct attribute_group *nvme_subsys_attrs_groups[];	/* [한국어] nvme_subsys_attrs_groups 필드 — 상위 구조 작성자·동기화 참고 */
/* [한국어] 서브시스템 sysfs 그룹 */
extern const struct attribute_group *nvme_dev_attr_groups[];	/* [한국어] nvme_dev_attr_groups 필드 — 상위 구조 작성자·동기화 참고 */
/* [한국어] 컨트롤러 속성 그룹 배열 */
extern const struct block_device_operations nvme_bdev_ops;	/* [한국어] nvme_bdev_ops 필드 — 상위 구조 작성자·동기화 참고 */
/* [한국어] 일반 NS 경로 gendisk fops */

void nvme_delete_ctrl_sync(struct nvme_ctrl *ctrl);	/* [한국어] 컨트롤러 인스턴스 동기 삭제 */
/* [한국어] 삭제 요청 후 동기 대기 — 모듈 unload/오류 처리 */
struct nvme_ns *nvme_find_path(struct nvme_ns_head *head);	/* [한국어] multipath 경로 선택 (iopolicy·ANA) */
/* [한국어] iopolicy·ANA 에 따라 제출 경로 NS 선택 (multipath.c) */

#ifdef CONFIG_NVME_MULTIPATH
/* [한국어] === multipath/ANA 실구현 구간 (multipath.c) ===
 * 상위 gendisk(ns_head->disk) 가 사용자 진입점, 하위 nvme_ns 가 물리 경로.
 * ANA 로그·iopolicy·requeue·failover 가 이 선언들의 구현체에서 만난다. */

/*
 * [한국어] ANA 로그 버퍼가 있으면 ANA 사용 중 — 경로 상태 머신이 활성.
 * 할당 전/uninit 후는 false 로 단순 폴백 경로 선택만 한다.
 */
static inline bool nvme_ctrl_use_ana(struct nvme_ctrl *ctrl)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	return ctrl->ana_log_buf != NULL; /* [한국어] 버퍼 존재 = ANA 기능 가동 중 */
}

void nvme_mpath_unfreeze(struct nvme_subsystem *subsys);	/* [한국어] 네이티브 multipath 헬퍼 */
/* [한국어] 서브시스템 모든 mpath 디스크 unfreeze — freeze 쌍의 해제 측.
 * 호출: 정책 변경·리셋 완료 후. 대상: 각 ns_head->disk->queue. */
void nvme_mpath_wait_freeze(struct nvme_subsystem *subsys);	/* [한국어] 네이티브 multipath 헬퍼 */
/* [한국어] mpath freeze 완료 대기 — 인플라이트가 빠질 때까지 블로킹. */
void nvme_mpath_start_freeze(struct nvme_subsystem *subsys);	/* [한국어] 네이티브 multipath 헬퍼 */
/* [한국어] mpath freeze 시작 — iopolicy/테이블 변경·안전한 경로 재배치 전. */
void nvme_mpath_default_iopolicy(struct nvme_subsystem *subsys);	/* [한국어] 네이티브 multipath 헬퍼 */
/* [한국어] 모듈 파라미터 기반 기본 iopolicy 를 subsys 에 기록. 생성 직후 1회. */
void nvme_failover_req(struct request *req);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] path/ANA 오류 요청을 다른 경로로 재큐하거나 최종 실패 처리.
 * complete_rq 가 path error 판정 시 호출. REQ_NVME_MPATH 컨텍스트. */
void nvme_kick_requeue_lists(struct nvme_ctrl *ctrl);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] 이 ctrl 경로에 묶인 head 의 requeue_work 를 깨워 보류 bio 재시도. */
int nvme_mpath_alloc_disk(struct nvme_ctrl *ctrl,struct nvme_ns_head *head);	/* [한국어] 네이티브 multipath 헬퍼 */
/* [한국어] 상위 multipath gendisk 할당·fops=ns_head_ops. 공유 NS 최초 경로에서. */
void nvme_mpath_add_sysfs_link(struct nvme_ns_head *ns);	/* [한국어] 네이티브 multipath 헬퍼 */
/* [한국어] holder/slave 류 sysfs 토폴로지 링크 추가 — 사용자 공간 가시성. */
void nvme_mpath_remove_sysfs_link(struct nvme_ns *ns);	/* [한국어] 네이티브 multipath 헬퍼 */
/* [한국어] 경로 제거 시 sysfs 링크 대칭 삭제. NVME_NS_SYSFS_ATTR_LINK 와 쌍. */
void nvme_mpath_add_disk(struct nvme_ns *ns, __le32 anagrpid);	/* [한국어] 네이티브 multipath 헬퍼 */
/* [한국어] 경로를 mpath 토폴로지에 편입하고 ANA 그룹 ID 설정·current_path 후보화. */
void nvme_mpath_put_disk(struct nvme_ns_head *head);	/* [한국어] 네이티브 multipath 헬퍼 */
/* [한국어] 상위 디스크 참조 해제 — head 수명 말기. */
int nvme_mpath_init_identify(struct nvme_ctrl *ctrl, struct nvme_id_ctrl *id);	/* [한국어] 네이티브 multipath 헬퍼 */
/* [한국어] Identify 로부터 ANA 역량 읽고 로그 버퍼 할당. 실패 시 경고 후 비-ANA. */
void nvme_mpath_init_ctrl(struct nvme_ctrl *ctrl);	/* [한국어] 네이티브 multipath 헬퍼 */
/* [한국어] ctrl multipath 필드·워크·락 초기화. init_ctrl 연동. */
void nvme_mpath_update(struct nvme_ctrl *ctrl);	/* [한국어] 네이티브 multipath 헬퍼 */
/* [한국어] ANA 로그 재해석 후 각 ns->ana_state 갱신·경로 재선택 트리거. */
void nvme_mpath_uninit(struct nvme_ctrl *ctrl);	/* [한국어] 네이티브 multipath 헬퍼 */
/* [한국어] ANA 버퍼·타이머 해제. 컨트롤러 teardown. */
void nvme_mpath_stop(struct nvme_ctrl *ctrl);	/* [한국어] 네이티브 multipath 헬퍼 */
/* [한국어] ANA 타이머/워크 정지 — stop_ctrl 경로에서 백그라운드 억제. */
bool nvme_mpath_clear_current_path(struct nvme_ns *ns);	/* [한국어] 네이티브 multipath 헬퍼 */
/* [한국어] current_path[] 에서 이 ns 제거. true=실제로 비운 경우(재선택 필요). */
void nvme_mpath_revalidate_paths(struct nvme_ns *ns);	/* [한국어] 네이티브 multipath 헬퍼 */
/* [한국어] 용량/상태 변경 후 경로 재검증 — limits 동기화 포함 가능. */
void nvme_mpath_clear_ctrl_paths(struct nvme_ctrl *ctrl);	/* [한국어] 네이티브 multipath 헬퍼 */
/* [한국어] 컨트롤러 다운 시 이 ctrl 을 가리키는 모든 current_path 정리. */
void nvme_mpath_remove_disk(struct nvme_ns_head *head);	/* [한국어] 네이티브 multipath 헬퍼 */
/* [한국어] 상위 multipath 디스크 제거(del_gendisk 계열). */
void nvme_mpath_start_request(struct request *rq);	/* [한국어] 네이티브 multipath 헬퍼 */
/* [한국어] mpath I/O 시작: start_time·nr_active++·통계 비트. start_request 경유. */
void nvme_mpath_end_request(struct request *rq);	/* [한국어] 네이티브 multipath 헬퍼 */
/* [한국어] mpath I/O 종료: nr_active--·지연 통계. complete 경로 대칭. */

/*
 * [한국어] multipath 로 들어온 bio 의 block 트레이스 완료 이벤트.
 * 상위 head->disk 큐에 남겨 사용자 관측 지점을 논리 디스크에 맞춤.
 */
static inline void nvme_trace_bio_complete(struct request *req)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	struct nvme_ns *ns = req->q->queuedata;	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */

	if ((req->cmd_flags & REQ_NVME_MPATH) && req->bio)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		trace_block_bio_complete(ns->head->disk->queue, req->bio);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
}

extern bool multipath;	/* [한국어] multipath 필드 — 상위 구조 작성자·동기화 참고 */
/* [한국어] 모듈 파라미터: multipath 기능 전역 on/off */
extern struct device_attribute dev_attr_ana_grpid;	/* [한국어] dev_attr_ana_grpid 필드 — 상위 구조 작성자·동기화 참고 */
/* [한국어] sysfs ana_grpid */
extern struct device_attribute dev_attr_ana_state;	/* [한국어] dev_attr_ana_state 필드 — 상위 구조 작성자·동기화 참고 */
/* [한국어] sysfs ana_state */
extern struct device_attribute dev_attr_queue_depth;	/* [한국어] dev_attr_queue_depth 필드 — 상위 구조 작성자·동기화 참고 */
/* [한국어] 경로 큐 깊이 노출 */
extern struct device_attribute dev_attr_numa_nodes;	/* [한국어] dev_attr_numa_nodes 필드 — 상위 구조 작성자·동기화 참고 */
/* [한국어] 경로 NUMA 힌트 */
extern struct device_attribute dev_attr_delayed_removal_secs;	/* [한국어] dev_attr_delayed_removal_secs 필드 — 상위 구조 작성자·동기화 참고 */
/* [한국어] 지연 제거 초 설정 */
extern struct device_attribute subsys_attr_iopolicy;	/* [한국어] subsys_attr_iopolicy 필드 — 상위 구조 작성자·동기화 참고 */
/* [한국어] 서브시스템 iopolicy sysfs */

/*
 * [한국어] gendisk 가 multipath head 인지 fops 포인터 비교로 판별.
 * private_data 해석(ns vs head) 분기 핵심.
 */
static inline bool nvme_disk_is_ns_head(struct gendisk *disk)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	return disk->fops == &nvme_ns_head_ops;	/* [한국어] 호출 결과 반환 */
}
/*
 * [한국어] 경로 전무 시 bio 를 실패시키지 않고 requeue 할지.
 */
static inline bool nvme_mpath_queue_if_no_path(struct nvme_ns_head *head)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	if (test_bit(NVME_NSHEAD_QUEUE_IF_NO_PATH, &head->flags))	/* [한국어] 상태 플래그 비트 */
		return true;	/* [한국어] 양성 판정 */
	return false;	/* [한국어] 음성 판정 */
}
#else	/* [한국어] CONFIG_NVME_MULTIPATH 꺼짐 — 모든 mpath API 를 빈 인라인/상수로 스텁 */
#define multipath false	/* [한국어] multipath 매크로 — 상위 섹션 계약 참고 */
/* [한국어] 빌드 아웃: 전역 multipath 비활성 상수 */
static inline bool nvme_ctrl_use_ana(struct nvme_ctrl *ctrl)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	return false;	/* [한국어] ANA 코드 없음 */
}
static inline void nvme_failover_req(struct request *req)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	/* [한국어] multipath 없음: failover no-op — 호출부가 #ifdef 없이 컴파일 */
}
static inline void nvme_kick_requeue_lists(struct nvme_ctrl *ctrl)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	/* [한국어] requeue 리스트 없음 */
}
static inline int nvme_mpath_alloc_disk(struct nvme_ctrl *ctrl,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		struct nvme_ns_head *head)	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
{
	return 0;	/* [한국어] 상위 디스크 불필요 — 성공 취급 */
}
static inline void nvme_mpath_add_disk(struct nvme_ns *ns, __le32 anagrpid)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	/* [한국어] 경로 편입 불필요 */
}
static inline void nvme_mpath_put_disk(struct nvme_ns_head *head)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	/* [한국어] 상위 디스크 없음 */
}
static inline void nvme_mpath_add_sysfs_link(struct nvme_ns *ns)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	/* [한국어] 스텁: 심볼 시그니처는 비-MPATH 빌드 정합용 */
}
static inline void nvme_mpath_remove_sysfs_link(struct nvme_ns *ns)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	/* [한국어] 스텁 */
}
static inline bool nvme_mpath_clear_current_path(struct nvme_ns *ns)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	return false;	/* [한국어] current_path 없음 */
}
static inline void nvme_mpath_revalidate_paths(struct nvme_ns *ns)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	/* [한국어] 스텁 */
}
static inline void nvme_mpath_clear_ctrl_paths(struct nvme_ctrl *ctrl)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	/* [한국어] 스텁 */
}
static inline void nvme_mpath_remove_disk(struct nvme_ns_head *head)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	/* [한국어] 스텁 */
}
static inline void nvme_trace_bio_complete(struct request *req)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	/* [한국어] mpath 트레이스 없음 */
}
static inline void nvme_mpath_init_ctrl(struct nvme_ctrl *ctrl)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	/* [한국어] 스텁 */
}
static inline int nvme_mpath_init_identify(struct nvme_ctrl *ctrl,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		struct nvme_id_ctrl *id)	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
{
	if (ctrl->subsys->cmic & NVME_CTRL_CMIC_ANA)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
"Please enable CONFIG_NVME_MULTIPATH for full support of multi-port devices.\n");	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
	/* [한국어] 장치가 ANA 인데 커널 multipath 꺼짐 — 관리자에게 설정 경고 */
	return 0;	/* [한국어] 성공/no-op 완료 */
}
static inline void nvme_mpath_update(struct nvme_ctrl *ctrl)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	/* [한국어] 스텁 */
}
static inline void nvme_mpath_uninit(struct nvme_ctrl *ctrl)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	/* [한국어] 스텁 */
}
static inline void nvme_mpath_stop(struct nvme_ctrl *ctrl)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	/* [한국어] 스텁 */
}
static inline void nvme_mpath_unfreeze(struct nvme_subsystem *subsys)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	/* [한국어] 스텁 */
}
static inline void nvme_mpath_wait_freeze(struct nvme_subsystem *subsys)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	/* [한국어] 스텁 */
}
static inline void nvme_mpath_start_freeze(struct nvme_subsystem *subsys)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	/* [한국어] 스텁 */
}
static inline void nvme_mpath_default_iopolicy(struct nvme_subsystem *subsys)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	/* [한국어] 스텁 */
}
static inline void nvme_mpath_start_request(struct request *rq)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	/* [한국어] 통계 없음 */
}
static inline void nvme_mpath_end_request(struct request *rq)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	/* [한국어] 통계 없음 */
}
static inline bool nvme_disk_is_ns_head(struct gendisk *disk)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	return false;	/* [한국어] head 디스크 없음 — 모두 일반 NS */
}
static inline bool nvme_mpath_queue_if_no_path(struct nvme_ns_head *head)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	return false;	/* [한국어] 음성 판정 */
}
#endif /* CONFIG_NVME_MULTIPATH */
/* [한국어] multipath 실구현/스텁 분기 끝 — 이후 심볼은 공통 */

int nvme_ns_get_unique_id(struct nvme_ns *ns, u8 id[16],	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		enum blk_unique_id type);	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
/* [한국어] 블록 계층 unique id 콜백 — nguid/uuid 등 16바이트 복사 */

/*
 * [한국어] ZNS Identify/리포트에서 추출한 존 한계 요약 — queue_limits 반영 전 스테이징.
 */
struct nvme_zone_info {
	u64 zone_size;			/* [한국어] 존 크기 */
	unsigned int max_open_zones;	/* [한국어] 동시 open 존 상한 */
	unsigned int max_active_zones;	/* [한국어] 동시 active 존 상한 */
};

int nvme_ns_report_zones(struct nvme_ns *ns, sector_t sector,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		unsigned int nr_zones, struct blk_report_zones_args *args);	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
/* [한국어] 블록 report_zones → Zone Mgmt Receive 변환 (zns.c) */
int nvme_query_zone_info(struct nvme_ns *ns, unsigned lbaf,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		struct nvme_zone_info *zi);	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
/* [한국어] Identify 로 zone_info 채우기 */
void nvme_update_zone_info(struct nvme_ns *ns, struct queue_limits *lim,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		struct nvme_zone_info *zi);	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
/* [한국어] zone_info 를 queue_limits 에 적용 */
#ifdef CONFIG_BLK_DEV_ZONED	/* [한국어] zoned 지원 시 Zone Mgmt Send 실구현 (zns.c) */
blk_status_t nvme_setup_zone_mgmt_send(struct nvme_ns *ns, struct request *req,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
				       struct nvme_command *cmnd,	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
				       enum nvme_zone_mgmt_action action);	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
/* [한국어] open/close/finish/reset 등 Zone Mgmt Send SQE 조립 */
#else	/* [한국어] zoned 미빌드: NOTSUPP 스텁으로 setup_cmd 분기 안전 */
static inline blk_status_t nvme_setup_zone_mgmt_send(struct nvme_ns *ns,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		struct request *req, struct nvme_command *cmnd,	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
		enum nvme_zone_mgmt_action action)	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
{
	return BLK_STS_NOTSUPP;	/* [한국어] zoned 블록 미포함 빌드 */
}
#endif	/* [한국어] CONFIG_BLK_DEV_ZONED (zone_mgmt_send) */

/*
 * [한국어] sysfs device → 경로 nvme_ns. head 디스크면 WARN (private_data 타입 상이).
 */
static inline struct nvme_ns *nvme_get_ns_from_dev(struct device *dev)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	struct gendisk *disk = dev_to_disk(dev);	/* [한국어] 블록 device↔디스크 역참조 */

	WARN_ON(nvme_disk_is_ns_head(disk));	/* [한국어] head 는 이 헬퍼 대상 아님 */
	return disk->private_data;	/* [한국어] 경로 디스크 private = nvme_ns * */
}

#ifdef CONFIG_NVME_HWMON	/* [한국어] hwmon 실구현 — SMART 온도 등 (hwmon.c) */
int nvme_hwmon_init(struct nvme_ctrl *ctrl);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] 온도 센서를 hwmon 으로 등록 (hwmon.c) */
void nvme_hwmon_exit(struct nvme_ctrl *ctrl);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] hwmon 해제 */
#else	/* [한국어] hwmon 미빌드 스텁 */
static inline int nvme_hwmon_init(struct nvme_ctrl *ctrl)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	return 0;	/* [한국어] hwmon 비빌드 */
}

static inline void nvme_hwmon_exit(struct nvme_ctrl *ctrl)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	/* [한국어] no-op */
}
#endif	/* [한국어] CONFIG_NVME_HWMON (init/exit) */

/*
 * [한국어] 트랜스포트 queue_rq 가 하드웨어에 넣기 직전 호출하는 시작 훅.
 * mpath 표식이 있으면 통계 후 blk_mq_start_request 로 타이머·상태 시작.
 */
static inline void nvme_start_request(struct request *rq)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	if (rq->cmd_flags & REQ_NVME_MPATH)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		nvme_mpath_start_request(rq);	/* [한국어] 경로 활성 I/O 회계 */
	blk_mq_start_request(rq);	/* [한국어] 블록 계층 타임아웃 시계 시작 */
}

/*
 * [한국어] 데이터 SGL 지원 — byte 또는 dword 정렬 비트 중 하나.
 */
static inline bool nvme_ctrl_sgl_supported(struct nvme_ctrl *ctrl)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	return ctrl->sgls & (NVME_CTRL_SGLS_BYTE_ALIGNED |	/* [한국어] 호출 결과 반환 */
			     NVME_CTRL_SGLS_DWORD_ALIGNED);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
}

/*
 * [한국어] 메타데이터 SGL: fabrics 는 관례상 허용, PCIe 는 MSDS 비트 필요.
 */
static inline bool nvme_ctrl_meta_sgl_supported(struct nvme_ctrl *ctrl)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	if (ctrl->ops->flags & NVME_F_FABRICS)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		return true;	/* [한국어] NVMe-oF 캡슐 경로에서 메타 SGL 사용 가능 가정 */
	return ctrl->sgls & NVME_CTRL_SGLS_MSDS;	/* [한국어] 호출 결과 반환 */
}

#ifdef CONFIG_NVME_HOST_AUTH
/* [한국어] === DH-HMAC-CHAP / TLS 인증 API (auth.c) ===
 * fabrics 연결 직후 큐별 협상. 실패 시 연결을 올리지 않거나 재협상.
 * 키 자료는 ctrl 의 host_key/ctrl_key/dhchap_ctxs 에 보관. */
int __init nvme_init_auth(void);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] 모듈 로드 시 인증 서브시스템 전역 초기화 (auth.c) */
void __exit nvme_exit_auth(void);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] 모듈 언로드 시 인증 전역 정리 */
int nvme_auth_init_ctrl(struct nvme_ctrl *ctrl);	/* [한국어] DH-HMAC-CHAP 인증 API */
/* [한국어] 컨트롤러 키 자료·큐 컨텍스트 배열 준비. opts 의 비밀 참조 가능. */
void nvme_auth_stop(struct nvme_ctrl *ctrl);	/* [한국어] DH-HMAC-CHAP 인증 API */
/* [한국어] 진행 중 협상/워크 중단 — stop/delete 경로 */
int nvme_auth_negotiate(struct nvme_ctrl *ctrl, int qid);	/* [한국어] DH-HMAC-CHAP 인증 API */
/* [한국어] 지정 qid 에 대해 DH-HMAC-CHAP 협상 시작(비동기 워크 가능) */
int nvme_auth_wait(struct nvme_ctrl *ctrl, int qid);	/* [한국어] DH-HMAC-CHAP 인증 API */
/* [한국어] 협상 완료 대기 — 연결 경로에서 블로킹. 성공 0 */
void nvme_auth_free(struct nvme_ctrl *ctrl);	/* [한국어] DH-HMAC-CHAP 인증 API */
/* [한국어] 키·컨텍스트 메모리 해제. free_ctrl 직전 */
void nvme_auth_revoke_tls_key(struct nvme_ctrl *ctrl);	/* [한국어] DH-HMAC-CHAP 인증 API */
/* [한국어] TLS PSK 무효화 — 재협상 또는 삭제 시 세션 비밀 폐기 */
#else
/* [한국어] 인증 미빌드 스텁 — 연결 경로가 컴파일되게 하되 협상은 프로토콜 미지원 */
static inline int nvme_auth_init_ctrl(struct nvme_ctrl *ctrl)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	return 0;	/* [한국어] 인증 비빌드: 성공 no-op */
}
static inline int __init nvme_init_auth(void)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	return 0;	/* [한국어] 모듈 init 성공 */
}
static inline void __exit nvme_exit_auth(void)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	/* [한국어] no-op */
}
static inline void nvme_auth_stop(struct nvme_ctrl *ctrl) {};	/* [한국어] DH-HMAC-CHAP 인증 API */
/* [한국어] 빈 인라인 — 세미콜론 스타일 원본 유지 */
static inline int nvme_auth_negotiate(struct nvme_ctrl *ctrl, int qid)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	return -EPROTONOSUPPORT;	/* [한국어] 사용자에게 프로토콜 미지원 명시 */
}
static inline int nvme_auth_wait(struct nvme_ctrl *ctrl, int qid)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	return -EPROTONOSUPPORT;	/* [한국어] wait 도 동일 미지원 */
}
static inline void nvme_auth_free(struct nvme_ctrl *ctrl) {};	/* [한국어] DH-HMAC-CHAP 인증 API */
/* [한국어] free no-op */
static inline void nvme_auth_revoke_tls_key(struct nvme_ctrl *ctrl) {};	/* [한국어] DH-HMAC-CHAP 인증 API */
/* [한국어] revoke no-op */
#endif

/*
 * [한국어] === passthrough effects·NS 참조 API ===
 * 사용자 명령이 용량/포맷을 바꾸면 freeze 후 스캔이 필요하다. effects 로그가
 * 그 필요성을 opcode 단위로 알려 준다. NS 참조는 REMOVING 레이스에 안전해야 한다.
 */
u32 nvme_command_effects(struct nvme_ctrl *ctrl, struct nvme_ns *ns,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
			 u8 opcode);	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
/* [한국어] effects 로그에서 opcode 영향(CSUPP/LBCC/NCC/NIC/CCC 등) 비트 조회 */
u32 nvme_passthru_start(struct nvme_ctrl *ctrl, struct nvme_ns *ns, u8 opcode);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] passthrough 실행 전 freeze 등 사전 조치. 반환 effects 마스크를 end 에 전달 */
int nvme_execute_rq(struct request *rq, bool at_head);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] 이미 할당·초기화된 request 를 동기 실행. at_head 시 큐 헤드 삽입 */
void nvme_passthru_end(struct nvme_ctrl *ctrl, struct nvme_ns *ns, u32 effects,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		       struct nvme_command *cmd, int status);	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
/* [한국어] passthrough 후 unfreeze·필요 시 스캔 등 사후 조치. start 와 쌍 */
struct nvme_ctrl *nvme_ctrl_from_file(struct file *file);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] 캐릭터 파일 private_data → nvme_ctrl. ioctl 진입 헬퍼 */
struct nvme_ns *nvme_find_get_ns(struct nvme_ctrl *ctrl, unsigned nsid);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] NSID 로 경로 ns 를 찾아 kref get. 없거나 제거 중이면 NULL */
bool nvme_get_ns(struct nvme_ns *ns);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] REMOVING 이 아니면 참조 획득. 실패 시 제출/ioctl 포기 */
void nvme_put_ns(struct nvme_ns *ns);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
/* [한국어] ns 참조 해제 — 0 이면 경로 구조체 free 및 리스트 정리 */

/*
 * [한국어] CC.CSS 가 CSI 로 설정돼 다중 커맨드 셋(I/O Command Set Independent)
 * 모드인지. ZNS 등 비-NVM CSI 식별 경로 분기.
 */
static inline bool nvme_multi_css(struct nvme_ctrl *ctrl)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	return (ctrl->ctrl_config & NVME_CC_CSS_MASK) == NVME_CC_CSS_CSI;	/* [한국어] CSI 모드 — ZNS 등 다중 커맨드셋 */
}

#endif /* _NVME_H */
/* [한국어] include 가드 종료 — host 트리 전 번역 단위가 공유하는 내부 계약 끝 */
