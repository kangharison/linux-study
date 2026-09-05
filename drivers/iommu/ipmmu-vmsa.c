// SPDX-License-Identifier: GPL-2.0
/*
 * IOMMU API for Renesas VMSA-compatible IPMMU
 * Author: Laurent Pinchart <laurent.pinchart@ideasonboard.com>
 *
 * Copyright (C) 2014-2020 Renesas Electronics Corporation
 */

/*
 * [한국어 설명] Renesas R-Car IPMMU(VMSA 호환) 드라이버 (ipmmu-vmsa.c)
 *
 * === 파일의 역할 ===
 * Renesas R-Car(Gen2/Gen3/Gen4)와 RZ/G2 SoC에 들어 있는 IPMMU를 리눅스
 * IOMMU 서브시스템에 붙이는 드라이버다. "VMSA 호환"이라는 이름대로
 * ARM의 VMSA(Virtual Memory System Architecture) long-descriptor 페이지
 * 테이블 형식을 그대로 쓰므로, 테이블 조작은 io-pgtable(ARM_32_LPAE_S1)에
 * 전부 위임하고 이 파일은 하드웨어 레지스터 프로그래밍만 담당한다.
 *
 * 이 드라이버를 이해하는 데 필요한 개념이 셋이다.
 *
 * (1) **루트/캐시 계층 구조**. R-Car Gen3 이후의 IPMMU는 하나가 아니라
 *     여러 개다. 중앙에 "루트"(IPMMU-MM)가 하나 있어 컨텍스트 레지스터와
 *     페이지 테이블 워크를 담당하고, 주변에 "캐시"(IPMMU-VI, IPMMU-VC 등)
 *     여러 개가 각자 담당하는 마스터들의 uTLB를 관리한다.
 *     디바이스 트리의 renesas,ipmmu-main 프로퍼티가 그 관계를 표현하며,
 *     ipmmu_ctx_write_all()이 루트와 캐시 양쪽에 같은 값을 쓰는 이유가
 *     여기 있다. Gen2에는 이 계층이 없어 IPMMU 하나가 전부를 한다.
 *
 * (2) **컨텍스트와 uTLB**. 루트 IPMMU는 여러 개의 "컨텍스트"를 갖고,
 *     각 컨텍스트가 하나의 페이지 테이블(= 하나의 도메인)을 담당한다.
 *     각 마스터는 "uTLB" 번호로 식별되며, IMUCTR 레지스터로 어느 컨텍스트에
 *     속할지 지정한다. 즉 도메인 attach는 "컨텍스트를 하나 확보하고,
 *     그 디바이스의 uTLB들을 그 컨텍스트에 연결하는" 일이다.
 *
 * (3) **디바이스 허용 목록**. R-Car Gen3/Gen4와 RZ/G2에서는 IPMMU가
 *     모든 디바이스에서 제대로 동작하지 않는다. 그래서 opt-in 방식으로
 *     검증된 디바이스(주로 eMMC)와 PCI만 허용하고, 알려진 결함이 있는
 *     실리콘 리비전은 통째로 거부한다(ipmmu_device_is_allowed 참조).
 *
 * === 전체 아키텍처에서의 위치 ===
 *
 *   [디바이스 드라이버] dma_map_*() / iommu_map()
 *        ↓
 *   [IOMMU 코어] iommu_ops 디스패치
 *        ↓
 *   [이 파일] ipmmu_map() → io-pgtable(ARM_32_LPAE_S1)에 위임
 *        ↓ tlb 콜백(ipmmu_flush_ops)
 *   [이 파일] ipmmu_tlb_invalidate() → IMCTR의 FLUSH 비트 + 완료 폴링
 *        ↓
 *   [루트 IPMMU] 컨텍스트 레지스터로 테이블 워크
 *   [캐시 IPMMU] uTLB로 각 마스터의 요청을 걸러 루트로 전달
 *
 * attach 흐름: 도메인이 처음 쓰이면 루트에서 컨텍스트를 하나 할당하고,
 * io-pgtable을 만들어 그 TTBR/MAIR을 컨텍스트 레지스터에 기록한 뒤
 * MMU를 켠다. 그다음 이 디바이스의 uTLB들을 그 컨텍스트에 연결한다.
 *
 * 실행 컨텍스트: attach/detach는 프로세스 컨텍스트(도메인 뮤텍스),
 * 컨텍스트 할당과 인터럽트 처리는 mmu->lock을 irqsave로 잡는다.
 * map/unmap은 io-pgtable에 그대로 위임하며 이 파일에는 락이 없다.
 *
 * === 타 모듈과의 연결 ===
 * - linux/io-pgtable.h: alloc_io_pgtable_ops(ARM_32_LPAE_S1, ...)로
 *   페이지 테이블 백엔드를 얻는다. cfg.arm_lpae_s1_cfg의 ttbr/mair를
 *   받아 하드웨어 레지스터에 기록한다.
 * - drivers/iommu/io-pgtable-arm.c: 실제 테이블 구현. 그쪽이 이 파일의
 *   ipmmu_flush_ops 콜백으로 TLB 무효화를 요청한다.
 * - linux/sys_soc.h: soc_device_match() — 실리콘 종류와 리비전을 보고
 *   허용/거부를 결정한다.
 * - asm/dma-iommu.h: ARM 32비트에서 CONFIG_IOMMU_DMA가 없을 때 쓰는
 *   레거시 DMA 매핑 경로. 그 외 구성에서는 스텁으로 대체된다.
 * 데이터 흐름: 디바이스 트리의 `iommus = <&ipmmu utlb_num>` → of_xlate가
 * 허용 여부를 확인하고 uTLB 번호를 fwspec에 넣는다 → attach가 컨텍스트를
 * 확보하고 그 uTLB들을 연결한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct ipmmu_features: SoC 세대별 차이를 모은 구조체. 컨텍스트 개수,
 *   uTLB 개수, 레지스터 오프셋 계산 방식 등이 여기서 갈린다.
 * - struct ipmmu_vmsa_device: IPMMU 인스턴스 하나. 루트 포인터,
 *   컨텍스트 할당 비트맵, uTLB→컨텍스트 매핑 배열.
 * - struct ipmmu_vmsa_domain: 도메인 하나. io-pgtable 핸들과 컨텍스트 번호.
 * - ipmmu_domain_init_context(): 컨텍스트를 확보하고 io-pgtable을 만들어
 *   하드웨어에 등록한다.
 * - ipmmu_domain_setup_context(): TTBR/TTBCR/MAIR을 기록하고 MMU를 켠다.
 *   리줌 시에도 그대로 재사용된다.
 * - ipmmu_utlb_enable()/disable(): 마스터를 컨텍스트에 붙이고 뗀다.
 * - ipmmu_irq(): 모든 활성 컨텍스트를 훑어 폴트를 보고한다.
 * - ipmmu_device_is_allowed(): SoC와 디바이스 허용 목록을 검사한다.
 */

/* [한국어] DECLARE_BITMAP()과 bitmap_zero() — 컨텍스트 할당 비트맵용. */
#include <linux/bitmap.h>
/* [한국어] 지연 함수. 현재는 iopoll이 대신하지만 관례상 남아 있다. */
#include <linux/delay.h>
/* [한국어] dma_set_mask_and_coherent() — 이 IPMMU가 40비트 물리 주소를
 * 다룰 수 있음을 DMA 계층에 알린다. */
#include <linux/dma-mapping.h>
/* [한국어] IS_ERR/PTR_ERR/ERR_PTR 오류 포인터 헬퍼. */
#include <linux/err.h>
/* [한국어] 심볼 내보내기 매크로. 현재 내보내는 심볼은 없다. */
#include <linux/export.h>
/* [한국어] __init 등 초기화 섹션 매크로. */
#include <linux/init.h>
/* [한국어] devm_request_irq()와 irqreturn_t — 폴트 인터럽트 처리. */
#include <linux/interrupt.h>
/* [한국어] ioread32()/iowrite32() MMIO 접근자. */
#include <linux/io.h>
/* [한국어] read_poll_timeout_atomic() — TLB 플러시 완료를 폴링한다. */
#include <linux/iopoll.h>
/* [한국어] io-pgtable 프레임워크 — 이 드라이버가 테이블을 직접 만지지
 * 않는 이유다. */
#include <linux/io-pgtable.h>
/* [한국어] IOMMU 코어 계약 — iommu_ops, report_iommu_fault 등. */
#include <linux/iommu.h>
/* [한국어] of_property_present(), of_device_get_match_data() 등 디바이스
 * 트리 접근자. */
#include <linux/of.h>
/* [한국어] of_find_device_by_node() — of_xlate가 IPMMU 플랫폼 디바이스를
 * 역추적한다. */
#include <linux/of_platform.h>
/* [한국어] dev_is_pci() — 허용 목록 검사에서 PCI 디바이스를 구분한다. */
#include <linux/pci.h>
/* [한국어] platform_driver/platform_device 정의. */
#include <linux/platform_device.h>
/* [한국어] SZ_1G, SZ_2M, SZ_4K — 지원 페이지 크기와 ARM 매핑 범위. */
#include <linux/sizes.h>
/* [한국어] kzalloc_obj()/kfree()/devm_kzalloc(). */
#include <linux/slab.h>
/* [한국어] soc_device_match()와 struct soc_device_attribute —
 * 실리콘 종류/리비전을 보고 IPMMU 사용 여부를 결정한다. */
#include <linux/sys_soc.h>

/* [한국어] ARM 32비트이면서 최신 dma-iommu 프레임워크가 없는 구성에서만
 * 레거시 ARM DMA-IOMMU 경로를 쓴다. 그 외(ARM64 등)에서는 아래 스텁으로
 * 대체되어 관련 코드가 사실상 사라진다.
 * 이 분기 때문에 device_group 선택까지 CONFIG에 의존하게 되는데,
 * 파일 아래쪽 ipmmu_ops의 FIXME가 그 점을 지적한다. */
#if defined(CONFIG_ARM) && !defined(CONFIG_IOMMU_DMA)
/* [한국어] ARM 32비트 레거시 경로: 진짜 arm_iommu_* API를 가져온다. */
#include <asm/dma-iommu.h>
#else
/* [한국어] 스텁: 매핑 생성이 항상 NULL을 반환한다. */
#define arm_iommu_create_mapping(...)	NULL
/* [한국어] 스텁: attach가 항상 실패한다. 이 경로가 쓰이지 않음을 명시한다. */
#define arm_iommu_attach_device(...)	-ENODEV
/* [한국어] 스텁: 해제는 아무것도 하지 않는다. do-while(0)으로 감싸
 * 문장 문맥에서 안전하게 쓰이도록 한다. */
#define arm_iommu_release_mapping(...)	do {} while (0)
#endif

/* [한국어] 이 드라이버가 다룰 수 있는 최대 컨텍스트 개수.
 * 실제 개수는 SoC 세대별 features가 결정하며, 이 값은 배열 크기의 상한이다. */
#define IPMMU_CTX_MAX		16U
/* [한국어] uTLB가 어느 컨텍스트에도 속하지 않음을 나타내는 표식.
 * utlb_ctx[] 배열을 이 값으로 초기화해 두고, 리줌 시 유효한 항목만
 * 되살린다. -1인 이유는 0이 유효한 컨텍스트 번호이기 때문이다. */
#define IPMMU_CTX_INVALID	-1

/* [한국어] 이 드라이버가 다룰 수 있는 최대 uTLB 개수(= 마스터 수).
 * 역시 실제 개수는 features가 정하고, 이 값은 배열 크기의 상한이다. */
#define IPMMU_UTLB_MAX		64U

/* [한국어] SoC 세대별로 달라지는 하드웨어 특성을 모은 구조체.
 * 이 드라이버가 Gen2부터 Gen4까지를 한 코드로 지원할 수 있는 이유가
 * 이 구조체다 — 차이를 전부 데이터로 뽑아 두고 코드는 그것을 참조한다.
 * 설정자: of_device_get_match_data()가 compatible에 대응하는 상수를 준다.
 * 읽는 자: 레지스터 오프셋 계산, 컨텍스트 설정, probe 전반. */
struct ipmmu_features {
	bool use_ns_alias_offset;
	/* [한국어] 레지스터 베이스에 비보안 별칭 오프셋(0x800)을 더할지 여부.
	 * Gen2에서만 참이다. probe의 주석이 설명하듯, 보안 모드 동작이
	 * 문서화되어 있지 않고 주 뱅크로는 시험이 실패해서 무조건
	 * 비보안 별칭을 쓰기로 한 결정이다.
	 * 읽는 자: ipmmu_probe(). */

	bool has_cache_leaf_nodes;
	/* [한국어] 이 SoC가 루트/캐시 계층 구조를 갖는지 여부.
	 * 참이면 IPMMU가 여러 개이고, renesas,ipmmu-main 프로퍼티로
	 * 루트를 가리키는 캐시 노드들이 존재한다. Gen3 이후가 참이다.
	 * 읽는 자: probe가 루트 판별과 IOMMU 코어 등록 여부를 정할 때. */

	unsigned int number_of_contexts;
	/* [한국어] 이 세대의 IPMMU가 지원하는 컨텍스트 개수.
	 * Gen2는 1(주석대로 소프트웨어가 하나만 시험됨), Gen3는 8, Gen4는 16.
	 * 읽는 자: probe가 num_ctx를 정할 때(IPMMU_CTX_MAX와 min을 취한다). */

	unsigned int num_utlbs;
	/* [한국어] 이 세대의 uTLB 개수. Gen2는 32, Gen3는 48, Gen4는 64.
	 * 읽는 자: probe의 utlb_ctx 초기화와 리줌의 순회 상한. */

	bool setup_imbuscr;
	/* [한국어] IMBUSCR(버스 제어) 레지스터를 설정할지 여부.
	 * Gen2에만 존재하는 레지스터라 Gen3 이후는 거짓이다.
	 * 읽는 자: ipmmu_domain_setup_context(). */

	bool twobit_imttbcr_sl0;
	/* [한국어] IMTTBCR의 SL0(시작 레벨) 필드가 2비트인지 여부.
	 * Gen3 이후가 참으로, 같은 "레벨 1 시작"을 표현하는 값이 세대마다
	 * 다르다(IMTTBCR_SL0_LVL_1 vs IMTTBCR_SL0_TWOBIT_LVL_1).
	 * 읽는 자: ipmmu_domain_setup_context(). */

	bool reserved_context;
	/* [한국어] 컨텍스트 0번을 예약해 두어야 하는지 여부.
	 * Gen3 이후가 참이며, probe가 비트맵에서 0번을 미리 점유해
	 * 할당되지 않게 만든다. 펌웨어나 보안 세계가 쓰는 것으로 보인다.
	 * 읽는 자: ipmmu_probe(). */

	bool cache_snoop;
	/* [한국어] 페이지 테이블 워크가 캐시를 스누핑하는지 여부.
	 * 참이면 IMTTBCR에 공유성/캐시 정책 비트를 세운다. Gen2만 참이다.
	 * 읽는 자: ipmmu_domain_setup_context(). */

	unsigned int ctx_offset_base;
	/* [한국어] 컨텍스트 레지스터 영역의 시작 오프셋.
	 * Gen2/Gen3는 0, Gen4는 0x10000이다 — 레지스터 배치가 크게 바뀌었다.
	 * 읽는 자: ipmmu_ctx_reg(). */

	unsigned int ctx_offset_stride;
	/* [한국어] 컨텍스트 하나가 차지하는 레지스터 영역의 크기.
	 * Gen2/Gen3는 0x40, Gen4는 0x1040이다.
	 * 읽는 자: ipmmu_ctx_reg()가 context_id에 곱해 오프셋을 만든다. */

	unsigned int utlb_offset_base;
	/* [한국어] uTLB 레지스터 영역의 시작 오프셋.
	 * Gen2/Gen3는 0, Gen4는 0x3000이다.
	 * 읽는 자: ipmmu_utlb_reg(). */
};

/* [한국어] IPMMU 인스턴스 하나의 상태(루트든 캐시든 같은 구조를 쓴다).
 * 수명: probe에서 devm_kzalloc으로 만들어져 디바이스와 함께 사라진다. */
struct ipmmu_vmsa_device {
	struct device *dev;
	/* [한국어] 이 IPMMU 자신의 device.
	 * 설정자: probe.
	 * 읽는 자: dev_err 로깅, io-pgtable의 iommu_dev(루트의 것을 쓴다). */

	void __iomem *base;
	/* [한국어] MMIO 레지스터 블록의 매핑 주소.
	 * 설정자: probe의 devm_platform_ioremap_resource(). Gen2에서는
	 *          비보안 별칭 오프셋(0x800)이 더해진 값이 된다.
	 * 읽는 자: ipmmu_read()/write()의 기준 주소. */

	struct iommu_device iommu;
	/* [한국어] IOMMU 코어에 등록되는 핸들(임베드).
	 * 설정자: probe. 다만 Gen3 이후의 루트 IPMMU는 등록하지 않는다 —
	 *          클라이언트가 붙는 것은 캐시 IPMMU이기 때문이다.
	 * 읽는 자: ipmmu_probe_device()가 담당 IOMMU로 반환한다. */

	struct ipmmu_vmsa_device *root;
	/* [한국어] 이 IPMMU가 속한 루트 IPMMU.
	 * 설정자: probe가 자기 자신(루트인 경우) 또는 ipmmu_find_root()의
	 *          결과를 넣는다.
	 * 읽는 자: 컨텍스트 레지스터 접근이 전부 루트를 거친다 —
	 *          ipmmu_ctx_read_root()/write_root()가 그 통로다.
	 * 왜 필요한가: Gen3 이후에는 컨텍스트 레지스터가 루트에만 있고,
	 *              캐시 IPMMU는 uTLB만 관리하기 때문이다. */

	const struct ipmmu_features *features;
	/* [한국어] 이 SoC 세대의 하드웨어 특성.
	 * 설정자: probe의 of_device_get_match_data().
	 * 읽는 자: 레지스터 오프셋 계산과 컨텍스트 설정 전반. */

	unsigned int num_ctx;
	/* [한국어] 실제로 쓸 컨텍스트 개수.
	 * 설정자: probe가 min(IPMMU_CTX_MAX, features->number_of_contexts).
	 * 읽는 자: 컨텍스트 할당 탐색 범위, 인터럽트 핸들러의 순회 상한,
	 *          device_reset의 순회 상한. */

	spinlock_t lock;			/* Protects ctx and domains[] */
	/* [한국어] 컨텍스트 할당 비트맵과 도메인 배열을 보호하는 스핀락.
	 * 설정자: probe가 초기화.
	 * 읽는 자: allocate/free_context와 인터럽트 핸들러.
	 * 왜 irqsave인가: 인터럽트 핸들러가 domains[]를 순회하므로
	 *                 프로세스 컨텍스트 쪽은 인터럽트를 막아야 한다. */

	DECLARE_BITMAP(ctx, IPMMU_CTX_MAX);
	/* [한국어] 컨텍스트 사용 여부 비트맵.
	 * 설정자: allocate_context가 세우고, free_context가 지운다.
	 *          probe가 reserved_context 세대에서 0번을 미리 점유한다.
	 * 읽는 자: find_first_zero_bit로 빈 컨텍스트를 찾을 때.
	 * 동기화: mmu->lock으로 보호된다. */

	struct ipmmu_vmsa_domain *domains[IPMMU_CTX_MAX];
	/* [한국어] 컨텍스트 번호 → 도메인 매핑.
	 * 설정자: allocate_context가 채우고 free_context가 NULL로 만든다.
	 * 읽는 자: 인터럽트 핸들러가 어느 도메인의 폴트인지 찾을 때,
	 *          리줌이 컨텍스트를 되살릴 때.
	 * 동기화: mmu->lock으로 보호된다. */

	s8 utlb_ctx[IPMMU_UTLB_MAX];
	/* [한국어] uTLB 번호 → 컨텍스트 번호 매핑.
	 * 설정자: utlb_enable이 컨텍스트 번호를, utlb_disable과 probe가
	 *          IPMMU_CTX_INVALID(-1)를 넣는다.
	 * 읽는 자: ipmmu_resume_noirq()가 어떤 uTLB를 어느 컨텍스트로
	 *          되살릴지 판단한다.
	 * 왜 s8인가: -1을 표현해야 하므로 부호 있는 타입이고, 컨텍스트가
	 *            최대 16개라 8비트로 충분하다. */

	struct dma_iommu_mapping *mapping;
	/* [한국어] ARM 32비트 레거시 DMA 매핑(2GB 크기, 1GB 오프셋).
	 * 설정자: ipmmu_init_arm_mapping()이 최초 1회 만든다.
	 * 읽는 자: 같은 함수가 이후 디바이스들을 여기 붙이고,
	 *          release_device와 remove가 해제한다.
	 * 값 범위: 레거시 경로가 아닌 구성에서는 항상 NULL이다
	 *          (스텁이 NULL을 반환하므로). */
};

/* [한국어] IOMMU 도메인 하나의 상태.
 * 수명: domain_alloc_paging에서 만들어져 domain_free에서 해제된다. */
struct ipmmu_vmsa_domain {
	struct ipmmu_vmsa_device *mmu;
	/* [한국어] 이 도메인이 붙어 있는 IPMMU.
	 * 설정자: 첫 attach가 기록한다.
	 * 읽는 자: 컨텍스트 레지스터 접근(항상 mmu->root를 거친다),
	 *          uTLB 조작, 폴트 로깅.
	 * 값 범위: NULL(아직 attach 전) 또는 유효한 포인터.
	 *          attach가 이 값으로 "이 도메인이 처음 쓰이는가"를 판별한다. */

	struct iommu_domain io_domain;
	/* [한국어] IOMMU 코어가 보는 도메인 부분(임베드).
	 * 설정자: domain_alloc_paging이 pgsize_bitmap을,
	 *          init_context가 geometry를 채운다.
	 * 값 범위: 페이지 크기는 1GB/2MB/4KB — LPAE가 제공하는 세 가지다. */

	struct io_pgtable_cfg cfg;
	/* [한국어] io-pgtable에 넘기는 설정이자 그것이 채워 돌려주는 결과.
	 * 특히 cfg.arm_lpae_s1_cfg의 ttbr와 mair가 중요한데,
	 * setup_context()가 그 값들을 하드웨어 레지스터에 기록한다.
	 * 설정자: init_context가 입력을, alloc_io_pgtable_ops가 출력을 채운다. */

	struct io_pgtable_ops *iop;
	/* [한국어] io-pgtable 백엔드의 연산 테이블.
	 * 설정자: init_context의 alloc_io_pgtable_ops().
	 * 읽는 자: map/unmap/iova_to_phys가 작업을 여기로 위임한다.
	 * 해제: domain_free가 free_io_pgtable_ops로 반납한다. */

	unsigned int context_id;
	/* [한국어] 루트 IPMMU에서 이 도메인에 할당된 컨텍스트 번호.
	 * 설정자: init_context가 allocate_context 결과를 저장.
	 * 읽는 자: 모든 컨텍스트 레지스터 접근의 인덱스이자,
	 *          uTLB를 이 컨텍스트에 연결할 때의 값이다. */

	struct mutex mutex;			/* Protects mappings */
	/* [한국어] 도메인 초기화를 직렬화하는 뮤텍스.
	 * 설정자: domain_alloc_paging이 초기화.
	 * 읽는 자: attach_device가 컨텍스트 초기화 구간을 감싼다.
	 * 왜 뮤텍스인가: init_context가 io-pgtable을 할당하며 잠들 수 있어
	 *                스핀락을 쓸 수 없다.
	 * 주석의 "Protects mappings"는 다소 넓은 표현으로, 실제로는
	 * 도메인 초기화(mmu 지정과 컨텍스트 확보)를 보호한다. */
};

/*
 * [한국어]
 * to_vmsa_domain - 일반 iommu_domain을 이 드라이버의 도메인으로 되돌린다
 *
 * @dom: 코어가 넘긴 일반 도메인 포인터.
 * @return: 그것을 감싸는 struct ipmmu_vmsa_domain 포인터.
 *
 * 실행 컨텍스트: 모든 도메인 콜백의 첫 줄. 순수 포인터 산술이다.
 *
 * 호출 체인:
 *   map/unmap/attach/free/iova_to_phys → [to_vmsa_domain]
 */
static struct ipmmu_vmsa_domain *to_vmsa_domain(struct iommu_domain *dom)
{
	/* [한국어] 임베드된 멤버의 주소에서 오프셋을 빼 바깥 구조체를 얻는다. */
	return container_of(dom, struct ipmmu_vmsa_domain, io_domain);
}

/*
 * [한국어]
 * to_ipmmu - 클라이언트 디바이스에서 담당 IPMMU를 얻는다
 *
 * @dev: 클라이언트 디바이스.
 * @return: of_xlate가 심어 둔 IPMMU 인스턴스, 없으면 NULL.
 *
 * NULL 반환의 의미가 중요하다: of_xlate가 허용 목록 검사에서 이 디바이스를
 * 거부했거나 아예 iommus 프로퍼티가 없다는 뜻이다. probe_device가
 * 그것을 "이 디바이스는 IOMMU를 쓰지 않는다"로 해석한다.
 *
 * 실행 컨텍스트: of_xlate, probe_device, attach 경로.
 *
 * 호출 체인:
 *   ipmmu_of_xlate() / ipmmu_probe_device() / ipmmu_attach_device()
 *   → [to_ipmmu]
 */
static struct ipmmu_vmsa_device *to_ipmmu(struct device *dev)
{
	/* [한국어] of_xlate가 저장해 둔 IPMMU 포인터를 꺼낸다. */
	return dev_iommu_priv_get(dev);
}

/* [한국어] TLB 플러시 완료를 기다리는 최대 시간(100마이크로초).
 * 이 시간을 넘으면 하드웨어가 응답하지 않는다고 보고 경고를 남긴다 —
 * 메시지가 "MMU may be deadlocked"인 것이 그 심각성을 말해 준다. */
#define TLB_LOOP_TIMEOUT		100	/* 100us */

/* -----------------------------------------------------------------------------
 * Registers Definition
 */

/* [한국어] 비보안 레지스터 별칭 영역의 오프셋.
 * probe의 주석이 설명하듯, IPMMU는 보안/비보안 두 벌의 레지스터 뱅크를
 * 갖는다. 주소 공간 앞쪽 뱅크는 CPU의 현재 모드에 대응하고, 보안 모드로
 * 돌 때는 이 오프셋에 비보안 뱅크가 나타난다. 이 드라이버는 무조건
 * 비보안 별칭을 쓴다(Gen2에서만). */
#define IM_NS_ALIAS_OFFSET		0x800

/* MMU "context" registers */
/* [한국어] IMCTR — 컨텍스트 제어 레지스터. MMU 활성화, 인터럽트 활성화,
 * TLB 플러시를 모두 이 하나로 제어한다. */
#define IMCTR				0x0000		/* R-Car Gen2/3 */
/* [한국어] 인터럽트 활성화 비트. 폴트 시 인터럽트를 내게 한다. */
#define IMCTR_INTEN			(1 << 2)	/* R-Car Gen2/3 */
/* [한국어] TLB 플러시 요청 비트. 1을 쓰면 플러시가 시작되고,
 * 완료되면 하드웨어가 스스로 0으로 되돌린다 — 그것이 폴링 대상이다. */
#define IMCTR_FLUSH			(1 << 1)	/* R-Car Gen2/3 */
/* [한국어] MMU 활성화 비트. 이 비트가 서야 이 컨텍스트의 변환이 동작한다. */
#define IMCTR_MMUEN			(1 << 0)	/* R-Car Gen2/3 */

/* [한국어] IMTTBCR — 변환 테이블 제어 레지스터. ARM의 TTBCR에 해당하며,
 * 주소 공간 분할과 시작 레벨, 캐시 정책을 정한다. */
#define IMTTBCR				0x0008		/* R-Car Gen2/3 */
/* [한국어] EAE(Extended Address Enable) — long-descriptor(LPAE) 형식을
 * 쓰겠다는 선언이다. 이 비트가 없으면 short-descriptor로 해석된다. */
#define IMTTBCR_EAE			(1 << 31)	/* R-Car Gen2/3 */
/* [한국어] TTBR0 영역을 이너 공유(inner shareable)로 지정한다.
 * cache_snoop이 참인 Gen2에서만 쓴다. */
#define IMTTBCR_SH0_INNER_SHAREABLE	(3 << 12)	/* R-Car Gen2 only */
/* [한국어] TTBR0 영역의 아우터 캐시 정책을 write-back write-allocate로. */
#define IMTTBCR_ORGN0_WB_WA		(1 << 10)	/* R-Car Gen2 only */
/* [한국어] TTBR0 영역의 이너 캐시 정책을 write-back write-allocate로. */
#define IMTTBCR_IRGN0_WB_WA		(1 << 8)	/* R-Car Gen2 only */
/* [한국어] SL0(시작 레벨)를 레벨 1로 지정하는 값(2비트 필드 버전).
 * Gen3 이후가 이 인코딩을 쓴다. */
#define IMTTBCR_SL0_TWOBIT_LVL_1	(2 << 6)	/* R-Car Gen3 only */
/* [한국어] SL0를 레벨 1로 지정하는 값(Gen2 인코딩). 같은 의미인데
 * 비트 위치와 값이 다르다 — features의 twobit_imttbcr_sl0가 어느
 * 쪽을 쓸지 결정한다. */
#define IMTTBCR_SL0_LVL_1		(1 << 4)	/* R-Car Gen2 only */

/* [한국어] IMBUSCR — 버스 제어 레지스터. Gen2에만 있다. */
#define IMBUSCR				0x000c		/* R-Car Gen2 only */
/* [한국어] DVM(Distributed Virtual Memory) 비트. setup_context가
 * 이 비트를 지워 DVM을 끈다. */
#define IMBUSCR_DVM			(1 << 2)	/* R-Car Gen2 only */
/* [한국어] 버스 선택 필드의 마스크. 역시 setup_context가 지운다. */
#define IMBUSCR_BUSSEL_MASK		(3 << 0)	/* R-Car Gen2 only */

/* [한국어] IMTTLBR0 — TTBR0의 하위 32비트(페이지 테이블 베이스). */
#define IMTTLBR0			0x0010		/* R-Car Gen2/3 */
/* [한국어] IMTTUBR0 — TTBR0의 상위 32비트. 40비트 물리 주소를 담기 위해
 * 두 레지스터로 나뉘어 있다. */
#define IMTTUBR0			0x0014		/* R-Car Gen2/3 */

/* [한국어] IMSTR — 상태 레지스터. 어떤 폴트가 났는지 알려 준다. */
#define IMSTR				0x0020		/* R-Car Gen2/3 */
/* [한국어] MHIT(Multiple Hit) — TLB에서 한 주소에 여러 엔트리가
 * 매칭됐다. 소프트웨어가 무효화 없이 매핑을 바꿨을 때 생긴다. */
#define IMSTR_MHIT			(1 << 4)	/* R-Car Gen2/3 */
/* [한국어] ABORT — 페이지 테이블 워크 자체가 실패했다(테이블 메모리
 * 접근 불가 등). */
#define IMSTR_ABORT			(1 << 2)	/* R-Car Gen2/3 */
/* [한국어] PF(Permission Fault) — 매핑은 있지만 권한이 없다. */
#define IMSTR_PF			(1 << 1)	/* R-Car Gen2/3 */
/* [한국어] TF(Translation Fault) — 매핑이 없다. 가장 흔한 폴트다. */
#define IMSTR_TF			(1 << 0)	/* R-Car Gen2/3 */

/* [한국어] IMMAIR0 — 메모리 속성 인덱스 레지스터(ARM의 MAIR0).
 * LPAE 형식에서 PTE의 AttrIndx가 이 레지스터의 바이트를 가리켜
 * 메모리 타입을 결정한다. io-pgtable이 계산한 값을 그대로 쓴다. */
#define IMMAIR0				0x0028		/* R-Car Gen2/3 */

/* [한국어] IMELAR — 폴트가 난 주소의 하위 32비트.
 * Gen2에서는 IMEAR라는 이름이었다(주소는 같다). */
#define IMELAR				0x0030		/* R-Car Gen2/3, IMEAR on R-Car Gen2 */
/* [한국어] IMEUAR — 폴트 주소의 상위 32비트. Gen3 이후에만 있다. */
#define IMEUAR				0x0034		/* R-Car Gen3 only */

/* uTLB registers */
/* [한국어] uTLB n의 제어 레지스터 오프셋. 32번을 경계로 레지스터 영역이
 * 나뉘어 있어 두 매크로로 갈라 계산한다 — Gen3에서 uTLB가 32개를
 * 넘으면서 뒤쪽 영역이 추가되었기 때문이다. */
#define IMUCTR(n)			((n) < 32 ? IMUCTR0(n) : IMUCTR32(n))
/* [한국어] uTLB 0~31의 제어 레지스터. 16바이트 간격으로 늘어선다. */
#define IMUCTR0(n)			(0x0300 + ((n) * 16))		/* R-Car Gen2/3 */
/* [한국어] uTLB 32 이상의 제어 레지스터. 별도 영역(0x600)에서 다시
 * 0번부터 세므로 (n - 32)를 쓴다. */
#define IMUCTR32(n)			(0x0600 + (((n) - 32) * 16))	/* R-Car Gen3 only */
/* [한국어] 이 uTLB가 속할 컨텍스트 번호를 지정하는 필드(비트 4~).
 * 이 한 필드가 "마스터를 도메인에 연결하는" 실체다. */
#define IMUCTR_TTSEL_MMU(n)		((n) << 4)	/* R-Car Gen2/3 */
/* [한국어] 이 uTLB를 플러시하라는 비트. enable 시 함께 세운다. */
#define IMUCTR_FLUSH			(1 << 1)	/* R-Car Gen2/3 */
/* [한국어] 이 uTLB의 변환을 활성화하는 비트. 0을 쓰면 그 마스터의
 * 요청이 IOMMU를 거치지 않게 된다(= disable). */
#define IMUCTR_MMUEN			(1 << 0)	/* R-Car Gen2/3 */

/* [한국어] uTLB n의 ASID 레지스터 오프셋. IMUCTR과 같은 방식으로
 * 32번을 경계로 갈린다. */
#define IMUASID(n)			((n) < 32 ? IMUASID0(n) : IMUASID32(n))
/* [한국어] uTLB 0~31의 ASID 레지스터. IMUCTR0에서 8바이트 떨어져 있다. */
#define IMUASID0(n)			(0x0308 + ((n) * 16))		/* R-Car Gen2/3 */
/* [한국어] uTLB 32 이상의 ASID 레지스터. */
#define IMUASID32(n)			(0x0608 + (((n) - 32) * 16))	/* R-Car Gen3 only */

/* -----------------------------------------------------------------------------
 * Root device handling
 */

/* [한국어] 플랫폼 드라이버의 전방 선언. ipmmu_find_root()가
 * driver_for_each_device()에 이 드라이버를 넘겨 등록된 IPMMU들을
 * 순회하는데, 정의가 파일 맨 끝에 있어 선언이 필요하다. */
static struct platform_driver ipmmu_driver;

/*
 * [한국어]
 * ipmmu_is_root - 이 IPMMU가 루트인지 판별한다
 *
 * @mmu: 검사할 인스턴스.
 * @return: 루트면 true.
 *
 * 판별 방식이 단순하다: root 포인터가 자기 자신을 가리키면 루트다.
 * probe가 루트인 경우 mmu->root = mmu로 설정하므로 성립한다.
 * Gen2에는 계층이 없어 모든 IPMMU가 루트다.
 *
 * 실행 컨텍스트: probe와 리줌 경로. 순수 판별이다.
 *
 * 호출 체인:
 *   __ipmmu_check_device() / ipmmu_probe() / ipmmu_resume_noirq()
 *   → [ipmmu_is_root]
 */
static bool ipmmu_is_root(struct ipmmu_vmsa_device *mmu)
{
	/* [한국어] 자기 자신을 루트로 가리키면 루트다. */
	return mmu->root == mmu;
}

/*
 * [한국어]
 * __ipmmu_check_device - 순회 콜백: 이 디바이스가 루트 IPMMU인지 확인한다
 *
 * @dev: 순회 중인 디바이스.
 * @data: 결과를 담을 포인터의 주소(struct ipmmu_vmsa_device **).
 * @return: 항상 0 — 순회를 계속하라는 뜻이다.
 *
 * 왜 항상 0인가: driver_for_each_device()는 콜백이 0이 아닌 값을 주면
 * 순회를 멈추고 그 값을 반환한다. 여기서는 끝까지 순회하며 루트를
 * 찾아 data에 담는 방식이므로 항상 0을 준다. 루트가 여럿이면
 * 마지막 것이 남지만, 시스템에 루트는 하나뿐이라는 전제다.
 *
 * 실행 컨텍스트: probe 경로(드라이버 코어의 순회 안).
 *
 * 호출 체인:
 *   ipmmu_find_root() → driver_for_each_device() → [__ipmmu_check_device]
 */
static int __ipmmu_check_device(struct device *dev, void *data)
{
	/* [한국어] 순회 중인 디바이스의 IPMMU 인스턴스. */
	struct ipmmu_vmsa_device *mmu = dev_get_drvdata(dev);
	/* [한국어] 결과를 써 넣을 포인터의 주소. */
	struct ipmmu_vmsa_device **rootp = data;

	/* [한국어] 루트를 찾으면 호출자에게 전달한다. */
	if (ipmmu_is_root(mmu))
		*rootp = mmu;

	/* [한국어] 0을 반환해 순회를 계속한다. */
	return 0;
}

/*
 * [한국어]
 * ipmmu_find_root - 이미 등록된 루트 IPMMU를 찾는다
 *
 * @return: 루트 IPMMU 포인터, 아직 없으면 NULL.
 *
 * 왜 필요한가: Gen3 이후의 캐시 IPMMU는 컨텍스트 레지스터를 갖지 않아
 * 루트를 통해야 한다. 그런데 probe 순서는 보장되지 않으므로, 캐시가
 * 먼저 probe되면 루트가 아직 없을 수 있다. 그때는 NULL이 반환되고
 * probe가 -EPROBE_DEFER로 물러나 나중에 다시 시도한다.
 *
 * 삼항 연산자의 의미: driver_for_each_device가 0을 반환하면(= 순회가
 * 끝까지 갔으면) 찾은 root를, 아니면 NULL을 준다. 콜백이 항상 0을
 * 주므로 실질적으로는 언제나 root를 반환한다.
 *
 * 실행 컨텍스트: probe 경로.
 *
 * 호출 체인:
 *   ipmmu_probe() → [ipmmu_find_root] → driver_for_each_device()
 */
static struct ipmmu_vmsa_device *ipmmu_find_root(void)
{
	/* [한국어] 찾은 루트를 담을 변수. 못 찾으면 NULL로 남는다. */
	struct ipmmu_vmsa_device *root = NULL;

	/* [한국어] 이 드라이버에 바인딩된 모든 디바이스를 순회하며 루트를
	 * 찾는다. 순회가 정상 종료(0)했을 때만 결과를 쓴다. */
	return driver_for_each_device(&ipmmu_driver.driver, NULL, &root,
				      __ipmmu_check_device) == 0 ? root : NULL;
}

/* -----------------------------------------------------------------------------
 * Read/Write Access
 */

/*
 * [한국어]
 * ipmmu_read - IPMMU 레지스터를 읽는다
 *
 * @mmu: 대상 인스턴스.
 * @offset: 레지스터 오프셋.
 * @return: 읽은 32비트 값.
 *
 * 실행 컨텍스트: 컨텍스트 설정, 폴트 처리, 플러시 폴링.
 *
 * 호출 체인:
 *   ipmmu_ctx_read() → [ipmmu_read] → ioread32()
 */
static u32 ipmmu_read(struct ipmmu_vmsa_device *mmu, unsigned int offset)
{
	/* [한국어] 매핑된 베이스에 오프셋을 더해 읽는다. */
	return ioread32(mmu->base + offset);
}

/*
 * [한국어]
 * ipmmu_write - IPMMU 레지스터에 쓴다
 *
 * @mmu: 대상 인스턴스.
 * @offset: 레지스터 오프셋.
 * @data: 쓸 값.
 * @return: 없음.
 *
 * 실행 컨텍스트: 컨텍스트 설정과 uTLB 조작.
 *
 * 호출 체인:
 *   ipmmu_ctx_write() / ipmmu_imuctr_write() → [ipmmu_write] → iowrite32()
 */
static void ipmmu_write(struct ipmmu_vmsa_device *mmu, unsigned int offset,
			u32 data)
{
	/* [한국어] 매핑된 베이스에 오프셋을 더해 쓴다. */
	iowrite32(data, mmu->base + offset);
}

/*
 * [한국어]
 * ipmmu_ctx_reg - 컨텍스트 레지스터의 실제 오프셋을 계산한다
 *
 * @mmu: 대상 인스턴스(features에서 배치 정보를 얻는다).
 * @context_id: 컨텍스트 번호.
 * @reg: 컨텍스트 안에서의 레지스터 오프셋(IMCTR 등).
 * @return: 레지스터 블록 시작으로부터의 최종 오프셋.
 *
 * 기본 계산은 base + id * stride + reg로 단순하다. 문제는 context_id가
 * 7을 넘을 때인데, 하드웨어가 8번 이후의 컨텍스트를 연속되지 않은
 * 다른 영역에 배치했다. `base += 0x800 - 8 * 0x40`이 그 보정이다 —
 * 0x800에서 앞 8개가 차지했을 크기(8 × 0x40 = 0x200)를 빼면,
 * id * stride를 그대로 더했을 때 올바른 위치가 나온다.
 * Gen4(stride 0x1040)에서도 같은 식을 쓰는데, 그 세대의 실제 배치가
 * 이 보정과 맞아떨어지도록 설계되어 있다.
 *
 * 실행 컨텍스트: 모든 컨텍스트 레지스터 접근. 순수 계산이다.
 *
 * 호출 체인:
 *   ipmmu_ctx_read() / ipmmu_ctx_write() → [ipmmu_ctx_reg]
 */
static unsigned int ipmmu_ctx_reg(struct ipmmu_vmsa_device *mmu,
				  unsigned int context_id, unsigned int reg)
{
	/* [한국어] 이 세대의 컨텍스트 레지스터 영역 시작 오프셋. */
	unsigned int base = mmu->features->ctx_offset_base;

	/* [한국어] 8번 이후의 컨텍스트는 연속되지 않은 영역에 있다.
	 * 앞 8개가 차지했을 크기를 빼 둔 보정 베이스를 쓰면, 아래의
	 * 일반 계산식이 그대로 올바른 위치를 준다. */
	if (context_id > 7)
		base += 0x800 - 8 * 0x40;

	/* [한국어] 보정된 베이스에 컨텍스트 간격과 레지스터 오프셋을 더한다. */
	return base + context_id * mmu->features->ctx_offset_stride + reg;
}

/*
 * [한국어]
 * ipmmu_ctx_read - 컨텍스트 레지스터를 읽는다
 *
 * @mmu: 대상 인스턴스.
 * @context_id: 컨텍스트 번호.
 * @reg: 컨텍스트 안에서의 레지스터 오프셋.
 * @return: 읽은 값.
 *
 * 실행 컨텍스트: 폴트 처리와 컨텍스트 설정.
 *
 * 호출 체인:
 *   ipmmu_ctx_read_root() → [ipmmu_ctx_read] → ipmmu_read()
 */
static u32 ipmmu_ctx_read(struct ipmmu_vmsa_device *mmu,
			  unsigned int context_id, unsigned int reg)
{
	/* [한국어] 오프셋을 계산해 읽는다. */
	return ipmmu_read(mmu, ipmmu_ctx_reg(mmu, context_id, reg));
}

/*
 * [한국어]
 * ipmmu_ctx_write - 컨텍스트 레지스터에 쓴다
 *
 * @mmu: 대상 인스턴스.
 * @context_id: 컨텍스트 번호.
 * @reg: 레지스터 오프셋.
 * @data: 쓸 값.
 * @return: 없음.
 *
 * 실행 컨텍스트: 컨텍스트 설정과 리셋.
 *
 * 호출 체인:
 *   ipmmu_ctx_write_root()/write_all() / ipmmu_device_reset()
 *   → [ipmmu_ctx_write] → ipmmu_write()
 */
static void ipmmu_ctx_write(struct ipmmu_vmsa_device *mmu,
			    unsigned int context_id, unsigned int reg, u32 data)
{
	/* [한국어] 오프셋을 계산해 쓴다. */
	ipmmu_write(mmu, ipmmu_ctx_reg(mmu, context_id, reg), data);
}

/*
 * [한국어]
 * ipmmu_ctx_read_root - 루트 IPMMU의 컨텍스트 레지스터를 읽는다
 *
 * @domain: 대상 도메인(mmu와 context_id를 얻는다).
 * @reg: 레지스터 오프셋.
 * @return: 읽은 값.
 *
 * 왜 항상 루트인가: Gen3 이후 컨텍스트 레지스터는 루트 IPMMU에만
 * 존재한다. 캐시 IPMMU에서 읽으면 의미 없는 값이 나온다.
 * Gen2에서는 루트가 곧 자기 자신이므로 같은 결과가 된다.
 *
 * 실행 컨텍스트: 폴트 처리, 컨텍스트 설정, 플러시 폴링.
 *
 * 호출 체인:
 *   ipmmu_tlb_sync() / ipmmu_domain_irq() / setup_context()
 *   → [ipmmu_ctx_read_root]
 */
static u32 ipmmu_ctx_read_root(struct ipmmu_vmsa_domain *domain,
			       unsigned int reg)
{
	/* [한국어] 도메인이 붙은 IPMMU의 루트에서, 이 도메인의 컨텍스트를 읽는다. */
	return ipmmu_ctx_read(domain->mmu->root, domain->context_id, reg);
}

/*
 * [한국어]
 * ipmmu_ctx_write_root - 루트 IPMMU의 컨텍스트 레지스터에 쓴다
 *
 * @domain: 대상 도메인.
 * @reg: 레지스터 오프셋.
 * @data: 쓸 값.
 * @return: 없음.
 *
 * TTBR/TTBCR/MAIR처럼 **테이블 워크에만 관계된** 레지스터가 이 함수로
 * 기록된다. 워크는 루트만 수행하므로 캐시 쪽에는 쓸 필요가 없다.
 * 반대로 IMCTR처럼 uTLB 동작에도 영향을 주는 레지스터는
 * ipmmu_ctx_write_all()로 양쪽에 쓴다.
 *
 * 실행 컨텍스트: 컨텍스트 설정과 폴트 상태 클리어.
 *
 * 호출 체인:
 *   ipmmu_domain_setup_context() / ipmmu_domain_irq()
 *   → [ipmmu_ctx_write_root]
 */
static void ipmmu_ctx_write_root(struct ipmmu_vmsa_domain *domain,
				 unsigned int reg, u32 data)
{
	/* [한국어] 루트의 해당 컨텍스트 레지스터에 기록한다. */
	ipmmu_ctx_write(domain->mmu->root, domain->context_id, reg, data);
}

/*
 * [한국어]
 * ipmmu_ctx_write_all - 루트와 캐시 IPMMU 양쪽의 컨텍스트 레지스터에 쓴다
 *
 * @domain: 대상 도메인.
 * @reg: 레지스터 오프셋.
 * @data: 쓸 값.
 * @return: 없음.
 *
 * 왜 양쪽에 쓰는가: IMCTR 같은 레지스터는 루트의 테이블 워크뿐 아니라
 * 캐시 IPMMU의 uTLB 동작에도 영향을 준다. 한쪽만 쓰면 두 계층의 상태가
 * 어긋나 TLB 플러시가 반쪽만 일어나거나 MMU 활성화가 반영되지 않는다.
 *
 * 순서에 주목: 캐시를 먼저, 루트를 나중에 쓴다. 활성화의 경우 루트가
 * 마지막에 켜지는 편이 안전하고, 플러시의 경우에도 루트 쪽 완료를
 * 폴링하므로 이 순서가 자연스럽다.
 * mmu == root인 Gen2에서는 첫 조건이 거짓이라 한 번만 쓴다.
 *
 * 실행 컨텍스트: 컨텍스트 설정, TLB 무효화, 컨텍스트 해제.
 *
 * 호출 체인:
 *   ipmmu_tlb_invalidate() / setup_context() / destroy_context()
 *   → [ipmmu_ctx_write_all]
 */
static void ipmmu_ctx_write_all(struct ipmmu_vmsa_domain *domain,
				unsigned int reg, u32 data)
{
	/* [한국어] 캐시 IPMMU가 별도로 존재하는 구성이라면 그쪽에도 쓴다.
	 * Gen2처럼 루트가 곧 자기 자신이면 이 블록을 건너뛴다. */
	if (domain->mmu != domain->mmu->root)
		ipmmu_ctx_write(domain->mmu, domain->context_id, reg, data);

	/* [한국어] 루트에는 항상 쓴다. */
	ipmmu_ctx_write(domain->mmu->root, domain->context_id, reg, data);
}

/*
 * [한국어]
 * ipmmu_utlb_reg - uTLB 레지스터의 실제 오프셋을 계산한다
 *
 * @mmu: 대상 인스턴스.
 * @reg: IMUCTR(n) 또는 IMUASID(n)이 준 상대 오프셋.
 * @return: 최종 오프셋.
 *
 * 세대별 베이스를 더하는 것이 전부다. Gen4에서 uTLB 영역이 0x3000으로
 * 옮겨 갔기 때문에 이 보정이 필요해졌다.
 *
 * 실행 컨텍스트: uTLB 조작 경로. 순수 계산이다.
 *
 * 호출 체인:
 *   ipmmu_imuasid_write() / ipmmu_imuctr_write() → [ipmmu_utlb_reg]
 */
static u32 ipmmu_utlb_reg(struct ipmmu_vmsa_device *mmu, unsigned int reg)
{
	/* [한국어] 세대별 uTLB 영역 베이스를 더한다. */
	return mmu->features->utlb_offset_base + reg;
}

/*
 * [한국어]
 * ipmmu_imuasid_write - uTLB의 ASID 레지스터에 쓴다
 *
 * @mmu: 대상 인스턴스.
 * @utlb: uTLB 번호.
 * @data: 쓸 ASID 값.
 * @return: 없음.
 *
 * 이 드라이버는 항상 0을 쓴다 — utlb_enable의 TODO 주석이 밝히듯
 * ASID를 어떻게 활용할지 정해지지 않았기 때문이다.
 *
 * 실행 컨텍스트: attach 경로.
 *
 * 호출 체인:
 *   ipmmu_utlb_enable() → [ipmmu_imuasid_write] → ipmmu_write()
 */
static void ipmmu_imuasid_write(struct ipmmu_vmsa_device *mmu,
				unsigned int utlb, u32 data)
{
	/* [한국어] 해당 uTLB의 ASID 레지스터 오프셋을 계산해 쓴다. */
	ipmmu_write(mmu, ipmmu_utlb_reg(mmu, IMUASID(utlb)), data);
}

/*
 * [한국어]
 * ipmmu_imuctr_write - uTLB의 제어 레지스터에 쓴다
 *
 * @mmu: 대상 인스턴스.
 * @utlb: uTLB 번호.
 * @data: 쓸 값(컨텍스트 번호 + 플래그, 또는 0으로 비활성화).
 * @return: 없음.
 *
 * 이 함수가 "마스터를 도메인에 붙이고 떼는" 실질적인 동작이다.
 * 0을 쓰면 그 마스터의 요청이 IOMMU를 거치지 않게 된다.
 *
 * 실행 컨텍스트: attach/detach와 release_device 경로.
 *
 * 호출 체인:
 *   ipmmu_utlb_enable()/disable() / ipmmu_release_device()
 *   → [ipmmu_imuctr_write] → ipmmu_write()
 */
static void ipmmu_imuctr_write(struct ipmmu_vmsa_device *mmu,
			       unsigned int utlb, u32 data)
{
	/* [한국어] 해당 uTLB의 제어 레지스터 오프셋을 계산해 쓴다. */
	ipmmu_write(mmu, ipmmu_utlb_reg(mmu, IMUCTR(utlb)), data);
}

/* -----------------------------------------------------------------------------
 * TLB and microTLB Management
 */

/* Wait for any pending TLB invalidations to complete */
/*
 * [한국어]
 * ipmmu_tlb_sync - 진행 중인 TLB 무효화가 끝나기를 기다린다
 *
 * @domain: 대상 도메인.
 * @return: 없음.
 *
 * IMCTR의 FLUSH 비트가 0으로 돌아올 때까지 폴링한다. 하드웨어가
 * 플러시를 마치면 스스로 그 비트를 내리기 때문이다.
 *
 * read_poll_timeout_atomic의 인자가 특이하다: 첫 인자가 값을 읽는
 * **함수**이고, 그 뒤 domain과 IMCTR이 그 함수에 넘길 인자다.
 * 단순 주소를 읽는 readl_poll_timeout과 달리, 오프셋 계산이 필요한
 * 이 드라이버에서는 함수 호출 형태가 필요하다.
 *
 * 타임아웃 시 "MMU may be deadlocked"라는 강한 표현을 쓰는 이유:
 * 플러시가 완료되지 않으면 이후 모든 변환이 옛 매핑을 쓸 수 있어
 * 시스템 전체가 잘못 동작하게 된다.
 *
 * 실행 컨텍스트: 무효화 경로. atomic 폴링이라 잠들지 않는다.
 *
 * 호출 체인:
 *   ipmmu_tlb_invalidate() / ipmmu_domain_destroy_context()
 *   → [ipmmu_tlb_sync] → read_poll_timeout_atomic()
 */
static void ipmmu_tlb_sync(struct ipmmu_vmsa_domain *domain)
{
	/* [한국어] 폴링 중 읽은 IMCTR 값을 담는 변수. */
	u32 val;

	/* [한국어] FLUSH 비트가 내려갈 때까지 1us 간격으로 최대 100us 기다린다.
	 * 첫 인자가 읽기 함수이고 마지막 두 인자가 그 함수에 전달된다.
	 * false는 "잠들지 않는다"는 뜻이다. */
	if (read_poll_timeout_atomic(ipmmu_ctx_read_root, val,
				     !(val & IMCTR_FLUSH), 1, TLB_LOOP_TIMEOUT,
				     false, domain, IMCTR))
		dev_err_ratelimited(domain->mmu->dev,
			"TLB sync timed out -- MMU may be deadlocked\n");
}

/*
 * [한국어]
 * ipmmu_tlb_invalidate - 이 컨텍스트의 TLB를 무효화하고 완료를 기다린다
 *
 * @domain: 대상 도메인.
 * @return: 없음.
 *
 * read-modify-write를 쓰는 이유: IMCTR에는 MMU 활성화(MMUEN)와 인터럽트
 * 활성화(INTEN) 비트도 함께 있다. FLUSH만 통째로 쓰면 그 설정이
 * 날아가 MMU가 꺼져 버린다.
 *
 * 읽기는 루트에서만 하고 쓰기는 양쪽에 하는 비대칭에 주목:
 * 값은 루트가 권위 있는 사본이고, 플러시 효과는 양쪽에 필요하다.
 *
 * 실행 컨텍스트: io-pgtable의 무효화 콜백 경로.
 *
 * 호출 체인:
 *   ipmmu_tlb_flush_all() → [ipmmu_tlb_invalidate] → ipmmu_tlb_sync()
 */
static void ipmmu_tlb_invalidate(struct ipmmu_vmsa_domain *domain)
{
	/* [한국어] 현재 IMCTR 값을 담을 변수. */
	u32 reg;

	/* [한국어] 루트에서 현재 값을 읽는다 — MMUEN/INTEN을 보존하기 위함이다. */
	reg = ipmmu_ctx_read_root(domain, IMCTR);
	/* [한국어] 플러시 요청 비트를 얹는다. */
	reg |= IMCTR_FLUSH;
	/* [한국어] 루트와 캐시 양쪽에 써서 두 계층의 TLB를 모두 비운다. */
	ipmmu_ctx_write_all(domain, IMCTR, reg);

	/* [한국어] 하드웨어가 플러시를 마칠 때까지 기다린다. 이것이 없으면
	 * 아직 옛 매핑이 살아 있는 상태로 호출자가 진행하게 된다. */
	ipmmu_tlb_sync(domain);
}

/*
 * Enable MMU translation for the microTLB.
 */
/*
 * [한국어]
 * ipmmu_utlb_enable - 마스터(uTLB)를 이 도메인의 컨텍스트에 연결한다
 *
 * @domain: 연결할 도메인.
 * @utlb: 마스터의 uTLB 번호(디바이스 트리의 iommus 인자).
 * @return: 없음.
 *
 * 이 함수가 "디바이스를 도메인에 붙이는" 하드웨어 동작이다.
 * IMUCTR에 컨텍스트 번호와 활성화 비트를 쓰면, 그 마스터의 DMA가
 * 해당 컨텍스트의 페이지 테이블을 거쳐 변환된다.
 *
 * 세 개의 TODO가 남아 있는데, 각각 실제 한계를 가리킨다:
 *  - 참조 카운트 없음: 여러 마스터가 같은 uTLB를 공유할 수 있는데,
 *    한쪽이 detach하면 다른 쪽도 끊긴다.
 *  - ASID를 0으로 고정: 컨텍스트 구분에 ASID를 활용하지 않는다.
 *  - uTLB 플러시 필요성 미확인: 일단 FLUSH 비트를 함께 세워 둔다.
 *
 * utlb_ctx 배열을 갱신하는 것이 중요하다 — 리줌 시 어떤 uTLB를
 * 어느 컨텍스트로 되살릴지가 여기 기록된다.
 *
 * 실행 컨텍스트: attach 경로와 리줌 경로.
 *
 * 호출 체인:
 *   ipmmu_attach_device() / ipmmu_resume_noirq() → [ipmmu_utlb_enable]
 */
static void ipmmu_utlb_enable(struct ipmmu_vmsa_domain *domain,
			      unsigned int utlb)
{
	/* [한국어] uTLB 레지스터는 그 마스터가 속한 IPMMU(캐시일 수 있다)에
	 * 있으므로, 루트가 아니라 domain->mmu를 쓴다. */
	struct ipmmu_vmsa_device *mmu = domain->mmu;

	/*
	 * TODO: Reference-count the microTLB as several bus masters can be
	 * connected to the same microTLB.
	 */

	/* TODO: What should we set the ASID to ? */
	/* [한국어] ASID를 0으로 고정한다. 원본 TODO가 밝히듯 이 값을
	 * 어떻게 활용할지 아직 정해지지 않았다. */
	ipmmu_imuasid_write(mmu, utlb, 0);
	/* TODO: Do we need to flush the microTLB ? */
	/* [한국어] 이 uTLB를 도메인의 컨텍스트에 연결하고 활성화한다.
	 * FLUSH 비트를 함께 세우는 것은 옛 엔트리를 확실히 없애려는 조치인데,
	 * 원본 TODO가 그 필요성에 의문을 표한다.
	 * 이 한 줄이 실행되는 순간부터 그 마스터의 DMA가 변환된다. */
	ipmmu_imuctr_write(mmu, utlb, IMUCTR_TTSEL_MMU(domain->context_id) |
				      IMUCTR_FLUSH | IMUCTR_MMUEN);
	/* [한국어] 리줌 시 되살릴 수 있도록 이 uTLB가 속한 컨텍스트를 기록한다. */
	mmu->utlb_ctx[utlb] = domain->context_id;
}

/*
 * Disable MMU translation for the microTLB.
 */
/*
 * [한국어]
 * ipmmu_utlb_disable - 마스터(uTLB)를 컨텍스트에서 떼어낸다
 *
 * @domain: 떼어낼 도메인.
 * @utlb: 마스터의 uTLB 번호.
 * @return: 없음.
 *
 * IMUCTR에 0을 쓰면 활성화 비트가 내려가 그 마스터의 요청이 IOMMU를
 * 거치지 않게 된다. 컨텍스트 자체는 그대로 남는다 — 다른 마스터가
 * 아직 쓰고 있을 수 있기 때문이다.
 *
 * utlb_ctx를 INVALID로 되돌려 리줌 시 이 uTLB를 되살리지 않게 한다.
 *
 * 실행 컨텍스트: detach 경로.
 *
 * 호출 체인:
 *   ipmmu_iommu_identity_attach() → [ipmmu_utlb_disable]
 */
static void ipmmu_utlb_disable(struct ipmmu_vmsa_domain *domain,
			       unsigned int utlb)
{
	/* [한국어] uTLB 레지스터를 가진 IPMMU. */
	struct ipmmu_vmsa_device *mmu = domain->mmu;

	/* [한국어] 0을 써서 이 uTLB의 변환을 끈다. */
	ipmmu_imuctr_write(mmu, utlb, 0);
	/* [한국어] 리줌 대상에서 제외하도록 표식을 되돌린다. */
	mmu->utlb_ctx[utlb] = IPMMU_CTX_INVALID;
}

/*
 * [한국어]
 * ipmmu_tlb_flush_all - io-pgtable의 전체 무효화 콜백
 *
 * @cookie: io-pgtable에 등록해 둔 쿠키 — 여기서는 도메인 포인터다.
 * @return: 없음.
 *
 * io-pgtable이 테이블을 고친 뒤 부르는 콜백이다. 이 하드웨어는
 * 범위 무효화를 지원하지 않아 항상 컨텍스트 전체를 비운다.
 *
 * 실행 컨텍스트: io-pgtable의 map/unmap 경로.
 *
 * 호출 체인:
 *   io-pgtable → ipmmu_flush_ops.tlb_flush_all → [ipmmu_tlb_flush_all]
 *   → ipmmu_tlb_invalidate()
 */
static void ipmmu_tlb_flush_all(void *cookie)
{
	/* [한국어] 쿠키가 곧 도메인이다(init_context가 그렇게 등록했다). */
	struct ipmmu_vmsa_domain *domain = cookie;

	/* [한국어] 이 컨텍스트의 TLB를 전부 비우고 완료를 기다린다. */
	ipmmu_tlb_invalidate(domain);
}

/*
 * [한국어]
 * ipmmu_tlb_flush - io-pgtable의 범위 무효화 콜백
 *
 * @iova: 무효화 시작 주소 — 이 하드웨어는 범위를 지정할 수 없어 무시된다.
 * @size: 무효화 크기 — 역시 무시된다.
 * @granule: 무효화 단위 — 역시 무시된다.
 * @cookie: 도메인 포인터.
 * @return: 없음.
 *
 * 인자를 모두 무시하고 전체를 비운다. IMCTR의 FLUSH 비트가 컨텍스트
 * 단위로만 동작하고 주소를 지정할 수단이 없기 때문이다.
 *
 * 실행 컨텍스트: io-pgtable의 테이블 변경 경로.
 *
 * 호출 체인:
 *   io-pgtable → ipmmu_flush_ops.tlb_flush_walk → [ipmmu_tlb_flush]
 *   → ipmmu_tlb_flush_all()
 */
static void ipmmu_tlb_flush(unsigned long iova, size_t size,
				size_t granule, void *cookie)
{
	/* [한국어] 범위를 지정할 수 없으므로 전체를 비운다. */
	ipmmu_tlb_flush_all(cookie);
}

/* [한국어] io-pgtable에 등록하는 TLB 무효화 콜백 묶음.
 * tlb_add_page가 없는 점에 주목: 페이지 단위 무효화를 지원하지 않으므로
 * 등록하지 않고, io-pgtable이 대신 tlb_flush_walk를 쓰게 한다. */
static const struct iommu_flush_ops ipmmu_flush_ops = {
	/* [한국어] 전체 무효화 — 컨텍스트의 TLB를 통째로 비운다. */
	.tlb_flush_all = ipmmu_tlb_flush_all,
	/* [한국어] 범위 무효화 — 인자를 무시하고 역시 전체를 비운다. */
	.tlb_flush_walk = ipmmu_tlb_flush,
};

/* -----------------------------------------------------------------------------
 * Domain/Context Management
 */

/*
 * [한국어]
 * ipmmu_domain_allocate_context - 루트에서 빈 컨텍스트를 하나 확보한다
 *
 * @mmu: 루트 IPMMU(호출자가 domain->mmu->root를 넘긴다).
 * @domain: 그 컨텍스트를 쓸 도메인.
 * @return: 확보한 컨텍스트 번호, 남은 것이 없으면 -EBUSY.
 *
 * 비트맵에서 첫 번째 0을 찾아 그 자리를 점유하고, 동시에 domains[]에
 * 도메인을 등록한다. 두 가지를 한 락 안에서 하는 것이 중요한데,
 * 인터럽트 핸들러가 domains[]를 보고 폴트를 보고하기 때문이다 —
 * 비트만 세우고 배열을 늦게 채우면 그 사이의 폴트가 NULL을 만난다.
 *
 * find_first_zero_bit이 num_ctx를 반환하면 빈 자리가 없다는 뜻이다.
 *
 * 실행 컨텍스트: attach 경로. mmu->lock을 irqsave로 잡는다.
 *
 * 호출 체인:
 *   ipmmu_domain_init_context() → [ipmmu_domain_allocate_context]
 */
static int ipmmu_domain_allocate_context(struct ipmmu_vmsa_device *mmu,
					 struct ipmmu_vmsa_domain *domain)
{
	/* [한국어] irqsave용 플래그. */
	unsigned long flags;
	/* [한국어] 찾은 컨텍스트 번호 또는 오류 코드. */
	int ret;

	/* [한국어] 비트맵과 도메인 배열을 함께 보호한다. 인터럽트 핸들러도
	 * 이 락을 잡으므로 irqsave가 필요하다. */
	spin_lock_irqsave(&mmu->lock, flags);

	/* [한국어] 첫 번째 비어 있는 컨텍스트를 찾는다. */
	ret = find_first_zero_bit(mmu->ctx, mmu->num_ctx);
	/* [한국어] 빈 자리를 찾았다면 도메인을 등록하고 비트를 세운다.
	 * 두 동작이 같은 락 안에 있어야 인터럽트 핸들러가 반쯤 등록된
	 * 상태를 보지 않는다. */
	if (ret != mmu->num_ctx) {
		mmu->domains[ret] = domain;	/* [한국어] 인터럽트 핸들러가 반쯤 등록된 상태를 보지 않도록 배열을 먼저 채운다. */
		set_bit(ret, mmu->ctx);	/* [한국어] 비트를 세워 이 컨텍스트를 점유한다. */
	} else
		/* [한국어] num_ctx가 반환됐다면 모든 컨텍스트가 사용 중이다. */
		ret = -EBUSY;

	spin_unlock_irqrestore(&mmu->lock, flags);

	/* [한국어] 컨텍스트 번호 또는 -EBUSY. */
	return ret;
}

/*
 * [한국어]
 * ipmmu_domain_free_context - 컨텍스트를 반납한다
 *
 * @mmu: 루트 IPMMU.
 * @context_id: 반납할 컨텍스트 번호.
 * @return: 없음.
 *
 * 할당의 역순으로 배열을 비우고 비트를 지운다. 순서가 배열 먼저인 것이
 * 중요한데, 비트를 먼저 지우면 다른 CPU가 그 컨텍스트를 새로 할당한 뒤
 * 이쪽이 domains[]를 NULL로 덮어쓸 수 있기 때문이다 — 다만 여기서는
 * 락 안에서 하므로 실질적인 차이는 없다.
 *
 * 실행 컨텍스트: 컨텍스트 초기화 실패와 도메인 해제 경로.
 *
 * 호출 체인:
 *   ipmmu_domain_init_context()(실패 시) / destroy_context()
 *   → [ipmmu_domain_free_context]
 */
static void ipmmu_domain_free_context(struct ipmmu_vmsa_device *mmu,
				      unsigned int context_id)
{
	/* [한국어] irqsave용 플래그. */
	unsigned long flags;

	/* [한국어] 비트맵과 배열을 함께 보호한다. */
	spin_lock_irqsave(&mmu->lock, flags);

	/* [한국어] 컨텍스트를 다시 할당 가능하게 만든다. */
	clear_bit(context_id, mmu->ctx);
	/* [한국어] 인터럽트 핸들러가 이 컨텍스트를 건너뛰도록 배열을 비운다. */
	mmu->domains[context_id] = NULL;

	spin_unlock_irqrestore(&mmu->lock, flags);	/* [한국어] 배열과 비트맵 갱신이 끝났으니 락을 푼다. */
}

/*
 * [한국어]
 * ipmmu_domain_setup_context - 컨텍스트 레지스터를 프로그래밍하고 MMU를 켠다
 *
 * @domain: 설정할 도메인. cfg에 io-pgtable의 결과가 들어 있어야 한다.
 * @return: 없음.
 *
 * io-pgtable이 만든 페이지 테이블을 하드웨어가 실제로 걷게 만드는 지점이다.
 * 설정 순서:
 *  1) TTBR0(테이블 베이스)을 상/하위 32비트로 나눠 기록.
 *  2) TTBCR — EAE로 LPAE 형식을 선언하고, 세대에 맞는 SL0 인코딩을 쓰며,
 *     cache_snoop 세대에서는 공유성/캐시 정책 비트도 세운다.
 *  3) MAIR0 — 메모리 속성 인덱스. io-pgtable이 계산한 값을 그대로 쓴다.
 *  4) IMBUSCR(Gen2만) — DVM과 버스 선택 비트를 지운다.
 *  5) IMSTR — 자기 자신을 읽어 그대로 다시 써서 모든 폴트 플래그를 지운다.
 *  6) IMCTR — 인터럽트와 MMU를 켜고 플러시를 함께 요청한다.
 *
 * 이 함수가 별도로 분리된 이유: 리줌 시 그대로 재사용된다. 서스펜드로
 * 레지스터가 날아간 뒤 같은 설정을 복원하면 되기 때문이다.
 *
 * 6번의 주석이 밝히는 결정들: long-descriptor 형식은 TEX remap을 쓰지
 * 않으므로 관련 설정이 없고, AF(Access Flag) 소프트웨어 관리도 쓸 일이
 * 없어 켜지 않는다.
 *
 * 실행 컨텍스트: attach 경로와 리줌 경로.
 *
 * 호출 체인:
 *   ipmmu_domain_init_context() / ipmmu_resume_noirq()
 *   → [ipmmu_domain_setup_context]
 */
static void ipmmu_domain_setup_context(struct ipmmu_vmsa_domain *domain)
{
	/* [한국어] io-pgtable이 만든 페이지 테이블의 물리 주소(64비트). */
	u64 ttbr;
	/* [한국어] TTBCR에 조립할 값. */
	u32 tmp;

	/* TTBR0 */
	/* [한국어] io-pgtable이 채워 준 TTBR 값을 가져온다. */
	ttbr = domain->cfg.arm_lpae_s1_cfg.ttbr;
	/* [한국어] 하위 32비트를 기록한다. */
	ipmmu_ctx_write_root(domain, IMTTLBR0, ttbr);
	/* [한국어] 상위 32비트를 기록한다. 40비트 물리 주소를 지원하므로
	 * 두 레지스터가 필요하다. */
	ipmmu_ctx_write_root(domain, IMTTUBR0, ttbr >> 32);

	/*
	 * TTBCR
	 * We use long descriptors and allocate the whole 32-bit VA space to
	 * TTBR0.
	 */
	/* [한국어] 시작 레벨(SL0)을 레벨 1로 지정한다. 같은 의미인데
	 * 세대마다 인코딩이 달라 features로 갈라 준다. */
	if (domain->mmu->features->twobit_imttbcr_sl0)
		tmp = IMTTBCR_SL0_TWOBIT_LVL_1;
	else
		tmp = IMTTBCR_SL0_LVL_1;

	/* [한국어] 테이블 워크가 캐시를 스누핑하는 세대(Gen2)라면
	 * 공유성과 캐시 정책 비트도 세운다. 그러면 워크가 캐시된 테이블을
	 * 읽을 수 있어 빨라진다. */
	if (domain->mmu->features->cache_snoop)
		tmp |= IMTTBCR_SH0_INNER_SHAREABLE | IMTTBCR_ORGN0_WB_WA |
		       IMTTBCR_IRGN0_WB_WA;

	/* [한국어] EAE를 세워 long-descriptor(LPAE) 형식을 쓴다고 선언하고,
	 * 위에서 조립한 비트들과 함께 기록한다. T0SZ 필드를 0으로 두므로
	 * 주석대로 32비트 VA 공간 전체가 TTBR0에 할당된다. */
	ipmmu_ctx_write_root(domain, IMTTBCR, IMTTBCR_EAE | tmp);

	/* MAIR0 */
	/* [한국어] 메모리 속성 인덱스 레지스터. LPAE의 PTE가 AttrIndx로
	 * 이 레지스터의 바이트를 가리켜 메모리 타입을 결정하므로,
	 * io-pgtable이 계산한 값을 그대로 써야 한다. */
	ipmmu_ctx_write_root(domain, IMMAIR0,
			     domain->cfg.arm_lpae_s1_cfg.mair);

	/* IMBUSCR */
	/* [한국어] Gen2에만 있는 버스 제어 레지스터. read-modify-write로
	 * DVM과 버스 선택 비트만 지우고 나머지는 보존한다. */
	if (domain->mmu->features->setup_imbuscr)
		ipmmu_ctx_write_root(domain, IMBUSCR,
				     ipmmu_ctx_read_root(domain, IMBUSCR) &
				     ~(IMBUSCR_DVM | IMBUSCR_BUSSEL_MASK));

	/*
	 * IMSTR
	 * Clear all interrupt flags.
	 */
	/* [한국어] 상태 레지스터를 읽어 그대로 다시 쓴다. 이 레지스터는
	 * 폴트 핸들러의 주석이 밝히듯 특이하게도 0을 써야 지워지는데,
	 * 여기서는 현재 값을 그대로 쓰는 방식으로 처리한다 — 부트로더가
	 * 남긴 옛 폴트 플래그가 있으면 그것이 지워진다. */
	ipmmu_ctx_write_root(domain, IMSTR, ipmmu_ctx_read_root(domain, IMSTR));

	/*
	 * IMCTR
	 * Enable the MMU and interrupt generation. The long-descriptor
	 * translation table format doesn't use TEX remapping. Don't enable AF
	 * software management as we have no use for it. Flush the TLB as
	 * required when modifying the context registers.
	 */
	/* [한국어] 마지막으로 인터럽트와 MMU를 켜고 TLB 플러시를 함께 요청한다.
	 * 컨텍스트 레지스터를 바꾼 뒤에는 플러시가 필수인데, 옛 설정으로
	 * 만들어진 TLB 엔트리가 남아 있을 수 있기 때문이다.
	 * 루트와 캐시 양쪽에 써야 두 계층이 함께 활성화된다. */
	ipmmu_ctx_write_all(domain, IMCTR,
			    IMCTR_INTEN | IMCTR_FLUSH | IMCTR_MMUEN);
}

/*
 * [한국어]
 * ipmmu_domain_init_context - 컨텍스트를 확보하고 페이지 테이블을 만든다
 *
 * @domain: 초기화할 도메인. domain->mmu가 이미 설정되어 있어야 한다.
 * @return: 0 성공, -EBUSY(빈 컨텍스트 없음), -EINVAL(io-pgtable 생성 실패).
 *
 * 순서가 중요하다:
 *  1) io-pgtable 설정을 채운다(ias/oas/quirks/tlb 콜백).
 *  2) 컨텍스트를 먼저 확보한다 — 실패하면 io-pgtable을 만들 이유가 없다.
 *  3) io-pgtable을 만든다. 실패하면 확보한 컨텍스트를 되돌린다.
 *  4) 하드웨어에 등록하고 MMU를 켠다.
 *
 * IO_PGTABLE_QUIRK_ARM_NS를 쓰는 이유가 원본 주석에 자세히 나온다:
 * VMSA 명세는 테이블 디스크립터의 NStable 비트가 서면 하위 엔트리들의
 * NS/NStable 비트를 무시하고 세워진 것으로 본다고 규정한다. 그런데
 * IPMMU는 그것을 따르지 않아, 비보안 모드에서 그 비트들이 하나라도
 * 없으면 보안 접근 폴트를 낸다. 그래서 모든 엔트리에 NS를 세우게 하는
 * quirk가 필요하다.
 *
 * oas가 40인데 ias는 32인 점에 주목: 입력 IOVA는 32비트지만 출력
 * 물리 주소는 40비트까지 갈 수 있다.
 *
 * coherent_walk를 false로 두는 이유도 TODO 주석에 있다: CCI와 DVM을
 * 통한 코히런트 워크를 아직 지원하지 않아, 캐시 관리를 io-pgtable에
 * 맡긴다는 뜻이다.
 *
 * 실행 컨텍스트: attach 경로, 도메인 뮤텍스 보유 상태.
 *
 * 호출 체인:
 *   ipmmu_attach_device() → [ipmmu_domain_init_context]
 *   → ipmmu_domain_allocate_context(), alloc_io_pgtable_ops(),
 *     ipmmu_domain_setup_context()
 */
static int ipmmu_domain_init_context(struct ipmmu_vmsa_domain *domain)
{
	/* [한국어] 컨텍스트 할당 결과. */
	int ret;

	/*
	 * Allocate the page table operations.
	 *
	 * VMSA states in section B3.6.3 "Control of Secure or Non-secure memory
	 * access, Long-descriptor format" that the NStable bit being set in a
	 * table descriptor will result in the NStable and NS bits of all child
	 * entries being ignored and considered as being set. The IPMMU seems
	 * not to comply with this, as it generates a secure access page fault
	 * if any of the NStable and NS bits isn't set when running in
	 * non-secure mode.
	 */
	/* [한국어] 위 주석이 설명하는 하드웨어 결함 때문에, 모든 엔트리에
	 * NS 비트를 세우게 하는 quirk가 필요하다. 명세대로라면 상위
	 * NStable 하나로 충분해야 하지만 IPMMU가 그것을 따르지 않는다. */
	domain->cfg.quirks = IO_PGTABLE_QUIRK_ARM_NS;
	/* [한국어] 도메인이 광고한 페이지 크기(1GB/2MB/4KB)를 그대로 전달한다. */
	domain->cfg.pgsize_bitmap = domain->io_domain.pgsize_bitmap;
	/* [한국어] 입력 주소 폭 32비트 — IOVA 공간이 4GB다. */
	domain->cfg.ias = 32;
	/* [한국어] 출력 주소 폭 40비트 — 물리 주소는 4GB를 넘을 수 있다.
	 * probe의 dma_set_mask_and_coherent(40)와 짝을 이룬다. */
	domain->cfg.oas = 40;
	/* [한국어] 테이블이 바뀔 때 TLB를 비울 콜백들. */
	domain->cfg.tlb = &ipmmu_flush_ops;
	/* [한국어] IOVA 공간의 끝을 4GB-1로 고정한다(ias와 일치). */
	domain->io_domain.geometry.aperture_end = DMA_BIT_MASK(32);
	/* [한국어] 코어가 이 범위를 강제하게 한다. */
	domain->io_domain.geometry.force_aperture = true;
	/*
	 * TODO: Add support for coherent walk through CCI with DVM and remove
	 * cache handling. For now, delegate it to the io-pgtable code.
	 */
	/* [한국어] 워크가 캐시 코히런트하지 않다고 선언한다. 그러면
	 * io-pgtable이 테이블을 고칠 때마다 캐시를 메모리로 밀어낸다.
	 * 원본 TODO는 CCI/DVM을 통한 코히런트 워크를 지원하면 이 처리를
	 * 없앨 수 있다고 적고 있다. */
	domain->cfg.coherent_walk = false;
	/* [한국어] 테이블 메모리의 DMA 처리에 쓸 디바이스. 워크를 수행하는
	 * 주체가 루트이므로 루트의 device를 넘긴다. */
	domain->cfg.iommu_dev = domain->mmu->root->dev;

	/*
	 * Find an unused context.
	 */
	/* [한국어] 루트에서 빈 컨텍스트를 확보한다. io-pgtable보다 먼저
	 * 시도하는 이유는, 컨텍스트가 없으면 테이블을 만들어도 쓸 수
	 * 없기 때문이다. */
	ret = ipmmu_domain_allocate_context(domain->mmu->root, domain);
	if (ret < 0)	/* [한국어] 빈 컨텍스트가 없으면 페이지 테이블을 만들 이유도 없다. */
		return ret;

	/* [한국어] 확보한 번호를 도메인에 기록한다. 이후 모든 컨텍스트
	 * 레지스터 접근이 이 번호를 쓴다. */
	domain->context_id = ret;

	/* [한국어] ARM LPAE 1단계 형식의 페이지 테이블을 만든다.
	 * 쿠키로 domain을 넘겨 TLB 콜백에서 되돌아오게 한다. */
	domain->iop = alloc_io_pgtable_ops(ARM_32_LPAE_S1, &domain->cfg,
					   domain);
	/* [한국어] 생성 실패 — 확보해 둔 컨텍스트를 반드시 되돌려야
	 * 다음 도메인이 쓸 수 있다. */
	if (!domain->iop) {
		ipmmu_domain_free_context(domain->mmu->root,	/* [한국어] io-pgtable 생성이 실패했으니 확보한 컨텍스트를 되돌린다. */
					  domain->context_id);
		return -EINVAL;	/* [한국어] 페이지 테이블 없이는 도메인을 쓸 수 없다. */
	}

	/* [한국어] 이제 cfg에 ttbr/mair가 채워졌으니 하드웨어에 등록하고
	 * MMU를 켠다. */
	ipmmu_domain_setup_context(domain);
	return 0;	/* [한국어] 컨텍스트 확보와 하드웨어 등록이 모두 끝났다. */
}

/*
 * [한국어]
 * ipmmu_domain_destroy_context - 컨텍스트를 비활성화하고 반납한다
 *
 * @domain: 정리할 도메인.
 * @return: 없음.
 *
 * IMCTR에 FLUSH만 쓰는 것이 핵심이다 — MMUEN과 INTEN이 빠지므로
 * MMU가 꺼지고 인터럽트도 멈추며, 동시에 남은 TLB 엔트리가 비워진다.
 * 세 가지를 한 번의 쓰기로 처리하는 셈이다.
 *
 * domain->mmu가 NULL이면 한 번도 attach된 적이 없는 도메인이므로
 * 정리할 것이 없다.
 *
 * 원본의 TODO("TLB flush really needed?")는 MMU를 끄는 마당에
 * 플러시가 필요한지 의문을 제기하는데, 안전을 택한 것으로 보인다.
 *
 * 실행 컨텍스트: 도메인 해제 경로.
 *
 * 호출 체인:
 *   ipmmu_domain_free() → [ipmmu_domain_destroy_context]
 *   → ipmmu_tlb_sync(), ipmmu_domain_free_context()
 */
static void ipmmu_domain_destroy_context(struct ipmmu_vmsa_domain *domain)
{
	/* [한국어] attach된 적이 없는 도메인이면 컨텍스트도 없다. */
	if (!domain->mmu)
		return;

	/*
	 * Disable the context. Flush the TLB as required when modifying the
	 * context registers.
	 *
	 * TODO: Is TLB flush really needed ?
	 */
	/* [한국어] FLUSH만 남긴 값을 써서 MMU와 인터럽트를 끄고 TLB를 비운다.
	 * MMUEN이 빠지는 것이 곧 비활성화다. */
	ipmmu_ctx_write_all(domain, IMCTR, IMCTR_FLUSH);
	/* [한국어] 플러시가 끝날 때까지 기다린다 — 곧 컨텍스트를 반납해
	 * 다른 도메인이 쓸 수 있으므로 옛 엔트리가 남으면 안 된다. */
	ipmmu_tlb_sync(domain);
	/* [한국어] 컨텍스트를 반납해 다음 도메인이 쓸 수 있게 한다. */
	ipmmu_domain_free_context(domain->mmu->root, domain->context_id);
}

/* -----------------------------------------------------------------------------
 * Fault Handling
 */

/*
 * [한국어]
 * ipmmu_domain_irq - 한 컨텍스트의 폴트를 처리한다
 *
 * @domain: 검사할 도메인.
 * @return: IRQ_HANDLED(이 컨텍스트에서 폴트를 처리함) 또는 IRQ_NONE.
 *
 * 동작 과정:
 *  1) 상태 레지스터를 읽어 폴트가 있는지 확인한다.
 *  2) 폴트 주소를 읽는다. 64비트 커널에서는 상위 32비트도 읽어 합친다.
 *  3) 상태 플래그를 지운다. **주소를 먼저 읽어야 하는** 이유가 원본
 *     주석에 있다 — 상태를 지우면 주소 레지스터가 0이 되어 버린다.
 *     또 이 레지스터는 보통과 달리 0을 써야 지워진다.
 *  4) 치명적 오류(MHIT, ABORT)를 로그로 남긴다.
 *  5) 페이지/변환 폴트가 아니면 여기서 끝낸다.
 *  6) report_iommu_fault로 상위에 알린다. 상위가 처리했으면(0 반환)
 *     조용히 끝내고, 아니면 미처리 폴트로 로그를 남긴다.
 *
 * TODO가 지적하는 한계: 폴트를 일으킨 디바이스를 IOVA로 역추적해야
 * 하는데, 그 방법이 없어 IOMMU 디바이스 자신을 넘긴다. 그래서
 * 상위 핸들러가 어느 마스터가 문제였는지 알 수 없다.
 *
 * 실행 컨텍스트: 인터럽트 핸들러 안. mmu->lock 보유 상태.
 *
 * 호출 체인:
 *   ipmmu_irq() → [ipmmu_domain_irq] → report_iommu_fault()
 */
static irqreturn_t ipmmu_domain_irq(struct ipmmu_vmsa_domain *domain)
{
	/* [한국어] 관심 있는 모든 폴트 비트를 묶은 마스크. */
	const u32 err_mask = IMSTR_MHIT | IMSTR_ABORT | IMSTR_PF | IMSTR_TF;
	/* [한국어] 로깅에 쓸 IPMMU 인스턴스. */
	struct ipmmu_vmsa_device *mmu = domain->mmu;
	/* [한국어] 폴트가 난 주소. */
	unsigned long iova;
	/* [한국어] 상태 레지스터 값. */
	u32 status;

	/* [한국어] 이 컨텍스트의 폴트 상태를 읽는다. */
	status = ipmmu_ctx_read_root(domain, IMSTR);
	/* [한국어] 폴트가 없다면 이 컨텍스트는 무관하다. */
	if (!(status & err_mask))
		return IRQ_NONE;

	/* [한국어] 폴트 주소의 하위 32비트를 읽는다. */
	iova = ipmmu_ctx_read_root(domain, IMELAR);
	/* [한국어] 64비트 커널이면 상위 32비트도 읽어 합친다.
	 * 32비트 커널에서는 unsigned long이 32비트라 의미가 없어 건너뛴다. */
	if (IS_ENABLED(CONFIG_64BIT))
		iova |= (u64)ipmmu_ctx_read_root(domain, IMEUAR) << 32;

	/*
	 * Clear the error status flags. Unlike traditional interrupt flag
	 * registers that must be cleared by writing 1, this status register
	 * seems to require 0. The error address register must be read before,
	 * otherwise its value will be 0.
	 */
	/* [한국어] 상태를 지운다. 두 가지가 특이하다 —
	 * 보통의 write-1-to-clear가 아니라 0을 써야 지워지고,
	 * 이것을 먼저 하면 주소 레지스터가 0이 되어 버린다.
	 * 그래서 위에서 주소를 먼저 읽어 두었다. */
	ipmmu_ctx_write_root(domain, IMSTR, 0);

	/* Log fatal errors. */
	/* [한국어] 다중 히트 — TLB에 중복 엔트리가 생겼다는 뜻으로,
	 * 소프트웨어가 무효화 없이 매핑을 바꿨을 때 발생한다. */
	if (status & IMSTR_MHIT)
		dev_err_ratelimited(mmu->dev, "Multiple TLB hits @0x%lx\n",
				    iova);
	/* [한국어] 워크 중단 — 페이지 테이블 자체를 읽지 못했다.
	 * 테이블 메모리가 해제됐거나 TTBR가 잘못된 경우다. */
	if (status & IMSTR_ABORT)
		dev_err_ratelimited(mmu->dev, "Page Table Walk Abort @0x%lx\n",
				    iova);

	/* [한국어] 위 두 가지는 복구할 수 없는 오류라 로그만 남긴다.
	 * 페이지/변환 폴트가 아니라면 상위에 알릴 것이 없으므로
	 * IRQ_NONE으로 물러난다 — 다만 이미 상태를 지웠으므로
	 * 인터럽트 자체는 해소된 상태다. */
	if (!(status & (IMSTR_PF | IMSTR_TF)))
		return IRQ_NONE;

	/*
	 * Try to handle page faults and translation faults.
	 *
	 * TODO: We need to look up the faulty device based on the I/O VA. Use
	 * the IOMMU device for now.
	 */
	/* [한국어] 상위 계층의 폴트 핸들러에 알린다. 0을 반환하면 처리됐다는
	 * 뜻이라 조용히 끝낸다.
	 * TODO가 지적하듯 폴트를 일으킨 디바이스를 특정하지 못해 IOMMU
	 * 자신을 넘기므로, 상위가 어느 마스터인지 알 수 없다는 한계가 있다. */
	if (!report_iommu_fault(&domain->io_domain, mmu->dev, iova, 0))
		return IRQ_HANDLED;

	/* [한국어] 아무도 처리하지 못한 폴트다. 상태와 주소를 남겨
	 * 진단할 수 있게 한다. */
	dev_err_ratelimited(mmu->dev,
			    "Unhandled fault: status 0x%08x iova 0x%lx\n",
			    status, iova);

	/* [한국어] 처리 여부와 무관하게 이 컨텍스트의 인터럽트는 해소했으므로
	 * HANDLED로 보고한다. */
	return IRQ_HANDLED;
}

/*
 * [한국어]
 * ipmmu_irq - IPMMU 인터럽트 핸들러
 *
 * @irq: 발생한 IRQ 번호(사용하지 않는다).
 * @dev: request_irq에 넘긴 IPMMU 인스턴스(루트다).
 * @return: 어느 컨텍스트든 폴트를 처리했으면 IRQ_HANDLED, 아니면 IRQ_NONE.
 *
 * IPMMU는 인터럽트 하나로 모든 컨텍스트의 폴트를 알린다. 그래서
 * 활성 컨텍스트를 전부 훑어 어느 것이 폴트를 냈는지 찾아야 한다.
 *
 * 여러 컨텍스트가 동시에 폴트를 냈을 수 있으므로 중간에 멈추지 않고
 * 끝까지 순회한다 — status를 덮어쓰지 않고 OR 하듯 갱신하는 것이
 * 그 의도를 보여 준다(HANDLED가 한 번이라도 나오면 유지된다).
 *
 * 루트에만 IRQ가 등록되는 점에 주목: probe가 ipmmu_is_root일 때만
 * IRQ를 요청한다. 캐시 IPMMU의 폴트도 루트를 통해 보고되기 때문이다.
 *
 * 실행 컨텍스트: 하드 인터럽트 컨텍스트. mmu->lock을 irqsave로 잡는다
 * (핸들러 안에서도 irqsave를 쓰는 것은 다소 과하지만 무해하다).
 *
 * 호출 체인:
 *   하드웨어 IRQ → [ipmmu_irq] → ipmmu_domain_irq()
 */
static irqreturn_t ipmmu_irq(int irq, void *dev)
{
	/* [한국어] request_irq에 넘긴 루트 IPMMU. */
	struct ipmmu_vmsa_device *mmu = dev;
	/* [한국어] 종합 결과. 하나라도 처리하면 HANDLED가 된다. */
	irqreturn_t status = IRQ_NONE;
	/* [한국어] 컨텍스트 순회 인덱스. */
	unsigned int i;
	/* [한국어] irqsave용 플래그. */
	unsigned long flags;

	/* [한국어] domains[] 배열을 보호한다 — attach/detach가 동시에
	 * 이 배열을 바꿀 수 있다. */
	spin_lock_irqsave(&mmu->lock, flags);

	/*
	 * Check interrupts for all active contexts.
	 */
	/* [한국어] 모든 컨텍스트를 훑는다. 인터럽트가 하나로 공유되므로
	 * 어느 컨텍스트가 폴트를 냈는지 찾아야 한다. */
	for (i = 0; i < mmu->num_ctx; i++) {
		/* [한국어] 쓰이지 않는 컨텍스트는 건너뛴다. */
		if (!mmu->domains[i])
			continue;
		/* [한국어] 이 컨텍스트에서 폴트를 처리했다면 종합 결과를
		 * HANDLED로 만든다. 여러 컨텍스트가 동시에 폴트를 낼 수 있어
		 * 중간에 멈추지 않는다. */
		if (ipmmu_domain_irq(mmu->domains[i]) == IRQ_HANDLED)
			status = IRQ_HANDLED;
	}

	spin_unlock_irqrestore(&mmu->lock, flags);

	/* [한국어] 하나라도 처리했으면 HANDLED, 아니면 NONE. */
	return status;
}

/* -----------------------------------------------------------------------------
 * IOMMU Operations
 */

/*
 * [한국어]
 * ipmmu_domain_alloc_paging - 페이징 도메인을 만든다
 *
 * @dev: 요청한 디바이스(사용하지 않는다).
 * @return: 새 도메인의 iommu_domain 포인터, 메모리 부족이면 NULL.
 *
 * 여기서는 껍데기만 만든다. 컨텍스트 확보와 io-pgtable 생성은
 * 첫 attach가 ipmmu_domain_init_context()에서 수행한다 — 그때가
 * 되어야 어느 IPMMU에 붙을지 알 수 있기 때문이다.
 *
 * 페이지 크기 1GB/2MB/4KB는 ARM LPAE 1단계 형식이 제공하는 세 가지다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(GFP_KERNEL).
 *
 * 호출 체인:
 *   iommu_domain_alloc() → iommu_ops->domain_alloc_paging
 *   → [ipmmu_domain_alloc_paging]
 */
static struct iommu_domain *ipmmu_domain_alloc_paging(struct device *dev)
{
	/* [한국어] 새로 만들 도메인. */
	struct ipmmu_vmsa_domain *domain;

	/* [한국어] 0으로 초기화해 할당한다. mmu와 iop이 NULL로 시작해야
	 * "아직 초기화 전"임을 판별할 수 있다. */
	domain = kzalloc_obj(*domain);
	if (!domain)	/* [한국어] 도메인 구조체를 할당하지 못했다. */
		return NULL;

	/* [한국어] 컨텍스트 초기화를 직렬화할 뮤텍스를 준비한다. */
	mutex_init(&domain->mutex);
	/* [한국어] LPAE가 제공하는 세 가지 페이지 크기를 광고한다.
	 * 코어가 큰 매핑을 1GB/2MB 블록으로 효율적으로 처리할 수 있다. */
	domain->io_domain.pgsize_bitmap = SZ_1G | SZ_2M | SZ_4K;

	/* [한국어] 코어에는 임베드된 일반 도메인 포인터를 돌려준다. */
	return &domain->io_domain;
}

/*
 * [한국어]
 * ipmmu_domain_free - 도메인을 해제한다
 *
 * @io_domain: 해제할 도메인.
 * @return: 없음.
 *
 * 순서: 컨텍스트 비활성화/반납 → io-pgtable 해제 → 구조체 반납.
 * 하드웨어를 먼저 끄는 것이 중요한데, 그렇지 않으면 해제된 페이지
 * 테이블을 하드웨어가 계속 걸을 수 있다.
 *
 * 원본 주석의 전제("모든 디바이스가 이미 detach되었다")에 기대고 있다 —
 * uTLB 연결을 여기서 끊지 않으므로, detach가 먼저 일어나지 않으면
 * 마스터가 사라진 컨텍스트를 계속 가리키게 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   iommu_domain_free() → domain_ops->free → [ipmmu_domain_free]
 *   → ipmmu_domain_destroy_context(), free_io_pgtable_ops()
 */
static void ipmmu_domain_free(struct iommu_domain *io_domain)
{
	/* [한국어] 이 드라이버의 도메인으로 복원한다. */
	struct ipmmu_vmsa_domain *domain = to_vmsa_domain(io_domain);

	/*
	 * Free the domain resources. We assume that all devices have already
	 * been detached.
	 */
	/* [한국어] 하드웨어를 먼저 끈다 — MMU 비활성화, TLB 플러시,
	 * 컨텍스트 반납이 한 번에 이뤄진다. */
	ipmmu_domain_destroy_context(domain);
	/* [한국어] 페이지 테이블 메모리를 반납한다. 하드웨어가 이미 꺼진
	 * 뒤여야 안전하다. */
	free_io_pgtable_ops(domain->iop);
	/* [한국어] 도메인 구조체를 반납한다. */
	kfree(domain);
}

/*
 * [한국어]
 * ipmmu_attach_device - 디바이스를 도메인에 붙인다
 *
 * @io_domain: 붙일 도메인.
 * @dev: 붙일 디바이스.
 * @old: 직전 도메인(사용하지 않는다).
 * @return: 0 성공, -ENXIO(담당 IPMMU 없음), -EINVAL(다른 IPMMU의 도메인),
 *          컨텍스트 초기화 실패 시 그 errno.
 *
 * 세 가지 경우를 다룬다:
 *  1) 도메인이 처음 쓰인다(domain->mmu == NULL): 이 IPMMU에 연결하고
 *     컨텍스트를 초기화한다. 실패하면 mmu를 NULL로 되돌려 다음 attach가
 *     다시 시도할 수 있게 한다.
 *  2) 이미 **다른** IPMMU에 붙어 있다: 하나의 도메인은 하나의 IPMMU에만
 *     속할 수 있으므로 거부한다. Gen3 이후 여러 캐시 IPMMU가 있는
 *     구성에서 실제로 발생할 수 있는 상황이다.
 *  3) 같은 IPMMU에 붙어 있다: 컨텍스트를 그대로 재사용한다.
 *
 * 마지막에 이 디바이스의 모든 uTLB를 그 컨텍스트에 연결한다. 하나의
 * 디바이스가 여러 uTLB를 가질 수 있어(읽기/쓰기 채널 분리 등) 순회한다.
 *
 * 뮤텍스가 컨텍스트 초기화만 감싸고 uTLB 연결은 그 밖에서 하는 점에
 * 주목: 초기화는 도메인 단위 경쟁이지만 uTLB 연결은 디바이스 단위라
 * 겹치지 않기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 뮤텍스를 쓰므로 잠들 수 있다.
 *
 * 호출 체인:
 *   iommu_attach_device() → domain_ops->attach_dev → [ipmmu_attach_device]
 *   → ipmmu_domain_init_context(), ipmmu_utlb_enable()
 */
static int ipmmu_attach_device(struct iommu_domain *io_domain,
			       struct device *dev, struct iommu_domain *old)
{
	/* [한국어] 이 디바이스의 uTLB 번호들이 담긴 펌웨어 스펙. */
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	/* [한국어] of_xlate가 심어 둔 담당 IPMMU. */
	struct ipmmu_vmsa_device *mmu = to_ipmmu(dev);
	/* [한국어] 붙일 도메인을 이 드라이버의 형태로 복원한다. */
	struct ipmmu_vmsa_domain *domain = to_vmsa_domain(io_domain);
	/* [한국어] uTLB 순회 인덱스. */
	unsigned int i;
	/* [한국어] 결과 코드. */
	int ret = 0;

	/* [한국어] of_xlate가 이 디바이스를 거부했거나 iommus 프로퍼티가
	 * 없다는 뜻이다. */
	if (!mmu) {
		dev_err(dev, "Cannot attach to IPMMU\n");	/* [한국어] of_xlate가 이 디바이스를 거부했거나 iommus 프로퍼티가 없다. */
		return -ENXIO;	/* [한국어] 담당 IPMMU가 없으면 붙을 수 없다. */
	}

	/* [한국어] 도메인 초기화를 직렬화한다. 두 디바이스가 같은 도메인에
	 * 동시에 붙으면 컨텍스트를 두 번 확보할 수 있기 때문이다. */
	mutex_lock(&domain->mutex);

	/* [한국어] 도메인이 처음 쓰이는 경우. */
	if (!domain->mmu) {
		/* The domain hasn't been used yet, initialize it. */
		/* [한국어] 이 IPMMU에 연결한다. init_context가 이 값을 읽으므로
		 * 먼저 설정해야 한다. */
		domain->mmu = mmu;
		/* [한국어] 컨텍스트를 확보하고 페이지 테이블을 만들어 하드웨어에
		 * 등록한다. */
		ret = ipmmu_domain_init_context(domain);
		/* [한국어] 실패하면 연결을 되돌려 다음 attach가 다시 시도할 수
		 * 있게 한다 — 이것이 없으면 반쯤 초기화된 도메인이 남는다. */
		if (ret < 0) {
			dev_err(dev, "Unable to initialize IPMMU context\n");	/* [한국어] 컨텍스트가 고갈되었거나 테이블을 만들지 못했다. */
			domain->mmu = NULL;	/* [한국어] 연결을 되돌려 다음 attach가 다시 시도할 수 있게 한다. */
		} else {
			/* [한국어] 어느 컨텍스트를 쓰게 됐는지 정보 로그로 남긴다.
			 * 컨텍스트가 부족한 시스템에서 디버깅에 유용하다. */
			dev_info(dev, "Using IPMMU context %u\n",
				 domain->context_id);
		}
	/* [한국어] 이미 다른 IPMMU에 붙어 있는 도메인이다. */
	} else if (domain->mmu != mmu) {
		/*
		 * Something is wrong, we can't attach two devices using
		 * different IOMMUs to the same domain.
		 */
		/* [한국어] 컨텍스트는 특정 IPMMU에 속하므로, 다른 IPMMU의
		 * 디바이스를 같은 도메인에 붙일 수 없다. */
		ret = -EINVAL;
	} else
		/* [한국어] 같은 IPMMU의 다른 디바이스가 이 도메인에 합류하는
		 * 정상적인 경우다. 컨텍스트를 그대로 공유한다. */
		dev_info(dev, "Reusing IPMMU context %u\n", domain->context_id);

	mutex_unlock(&domain->mutex);

	/* [한국어] 초기화나 검증이 실패했다면 uTLB를 연결하지 않고 끝낸다. */
	if (ret < 0)
		return ret;

	/* [한국어] 이 디바이스가 쓰는 모든 uTLB를 도메인의 컨텍스트에 연결한다.
	 * 이 루프가 끝나면 그 마스터들의 DMA가 변환되기 시작한다. */
	for (i = 0; i < fwspec->num_ids; ++i)
		ipmmu_utlb_enable(domain, fwspec->ids[i]);

	return 0;	/* [한국어] uTLB 연결까지 끝났으므로 attach 성공이다. */
}

/*
 * [한국어]
 * ipmmu_iommu_identity_attach - 항등 도메인으로 전환한다(= detach)
 *
 * @identity_domain: 정적 항등 도메인.
 * @dev: 대상 디바이스.
 * @old: 직전 도메인. 이 함수의 실제 작업 대상이다.
 * @return: 항상 0.
 *
 * 하는 일은 이 디바이스의 uTLB들을 컨텍스트에서 떼어내는 것뿐이다.
 * 컨텍스트 자체는 그대로 남는데, 원본 TODO가 지적하듯 "붙어 있는
 * 디바이스가 없을 때 컨텍스트를 끄는" 최적화가 아직 없기 때문이다.
 * 그 결과 detach 후에도 컨텍스트와 페이지 테이블이 살아 있어,
 * 도메인이 해제될 때까지 자원을 점유한다.
 *
 * 첫 검사: old가 없거나 이미 항등 도메인이면 되돌릴 것이 없다.
 * 이 검사가 없으면 to_vmsa_domain(항등 도메인)이 엉뚱한 메모리를 본다.
 *
 * 실행 컨텍스트: detach/도메인 전환 경로.
 *
 * 호출 체인:
 *   iommu_detach_device() → domain_ops->attach_dev
 *   → [ipmmu_iommu_identity_attach] → ipmmu_utlb_disable()
 */
static int ipmmu_iommu_identity_attach(struct iommu_domain *identity_domain,
				       struct device *dev,
				       struct iommu_domain *old)
{
	/* [한국어] 이 디바이스의 uTLB 번호들. */
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	/* [한국어] 떼어낼 옛 도메인. */
	struct ipmmu_vmsa_domain *domain;
	/* [한국어] uTLB 순회 인덱스. */
	unsigned int i;

	/* [한국어] 이미 항등 상태이거나 붙은 적이 없으면 할 일이 없다. */
	if (old == identity_domain || !old)
		return 0;

	/* [한국어] 옛 도메인을 이 드라이버의 형태로 복원한다. */
	domain = to_vmsa_domain(old);
	/* [한국어] 이 디바이스의 모든 uTLB를 컨텍스트에서 떼어낸다.
	 * 이후 그 마스터들의 DMA는 IOMMU를 거치지 않는다. */
	for (i = 0; i < fwspec->num_ids; ++i)
		ipmmu_utlb_disable(domain, fwspec->ids[i]);

	/*
	 * TODO: Optimize by disabling the context when no device is attached.
	 */
	/* [한국어] 원본 TODO가 밝히듯 컨텍스트는 그대로 남는다.
	 * 참조 카운트가 없어 "마지막 디바이스가 떠났는지"를 알 수 없기 때문이다. */
	return 0;
}

/* [한국어] 항등 도메인의 연산 테이블. attach_dev 하나뿐인 이유는
 * 이 도메인이 "변환 없음" 상태만 표현하기 때문이다. */
static struct iommu_domain_ops ipmmu_iommu_identity_ops = {
	/* [한국어] uTLB 연결을 끊는 콜백. */
	.attach_dev = ipmmu_iommu_identity_attach,
};

/* [한국어] 정적 항등 도메인. 상태가 없어 모든 디바이스가 공유해도 된다. */
static struct iommu_domain ipmmu_iommu_identity_domain = {
	/* [한국어] 코어가 항등 도메인임을 알아보는 종류 표시. */
	.type = IOMMU_DOMAIN_IDENTITY,
	/* [한국어] 위에서 정의한 콜백 하나짜리 테이블. */
	.ops = &ipmmu_iommu_identity_ops,
};

/*
 * [한국어]
 * ipmmu_map - 매핑 생성을 io-pgtable에 위임한다
 *
 * @io_domain: 대상 도메인.
 * @iova: 매핑 시작 IOVA.
 * @paddr: 물리 주소 시작.
 * @pgsize: 페이지 크기.
 * @pgcount: 페이지 개수.
 * @prot: 보호 플래그.
 * @gfp: 할당 플래그.
 * @mapped: 출력 인자.
 * @return: io-pgtable의 결과.
 *
 * 이 드라이버가 페이지 테이블을 직접 만지지 않는다는 사실이 이 함수
 * 하나에 압축되어 있다. 락도 없는데, 코어의 도메인 락이 직렬화를
 * 맡고 io-pgtable이 테이블 설치를 cmpxchg로 방어하기 때문이다.
 * gfp도 그대로 전달하므로 atomic 컨텍스트가 온전히 보존된다.
 *
 * 실행 컨텍스트: 매핑 경로.
 *
 * 호출 체인:
 *   iommu_map() → domain_ops->map_pages → [ipmmu_map]
 *   → io-pgtable의 map_pages
 */
static int ipmmu_map(struct iommu_domain *io_domain, unsigned long iova,
		     phys_addr_t paddr, size_t pgsize, size_t pgcount,
		     int prot, gfp_t gfp, size_t *mapped)
{
	/* [한국어] 이 드라이버의 도메인으로 복원한다. */
	struct ipmmu_vmsa_domain *domain = to_vmsa_domain(io_domain);

	/* [한국어] 인자를 그대로 io-pgtable에 넘긴다. 테이블 조작과
	 * 필요한 TLB 무효화 콜백까지 전부 그쪽이 처리한다. */
	return domain->iop->map_pages(domain->iop, iova, paddr, pgsize, pgcount,
				      prot, gfp, mapped);
}

/*
 * [한국어]
 * ipmmu_unmap - 매핑 해제를 io-pgtable에 위임한다
 *
 * @io_domain: 대상 도메인.
 * @iova: 해제 시작 IOVA.
 * @pgsize: 페이지 크기.
 * @pgcount: 페이지 개수.
 * @gather: TLB 무효화 수집 구조체.
 * @return: 해제한 바이트 수.
 *
 * map과 마찬가지로 위임만 한다. gather를 그대로 넘기지만,
 * 이 드라이버는 tlb_add_page 콜백을 등록하지 않아 io-pgtable이
 * 대신 tlb_flush_walk(= 전체 무효화)를 부르게 된다.
 *
 * 실행 컨텍스트: 해제 경로.
 *
 * 호출 체인:
 *   iommu_unmap() → domain_ops->unmap_pages → [ipmmu_unmap]
 *   → io-pgtable의 unmap_pages
 */
static size_t ipmmu_unmap(struct iommu_domain *io_domain, unsigned long iova,
			  size_t pgsize, size_t pgcount,
			  struct iommu_iotlb_gather *gather)
{
	/* [한국어] 이 드라이버의 도메인으로 복원한다. */
	struct ipmmu_vmsa_domain *domain = to_vmsa_domain(io_domain);

	/* [한국어] 해제를 io-pgtable에 위임한다. */
	return domain->iop->unmap_pages(domain->iop, iova, pgsize, pgcount, gather);
}

/*
 * [한국어]
 * ipmmu_flush_iotlb_all - 도메인 전체의 TLB를 비운다
 *
 * @io_domain: 대상 도메인.
 * @return: 없음.
 *
 * mmu가 NULL이면(= 아직 attach 전) 비울 컨텍스트가 없다. 코어가
 * attach 전에 이 콜백을 부를 수 있어 방어가 필요하다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   iommu_flush_iotlb_all() → domain_ops->flush_iotlb_all
 *   → [ipmmu_flush_iotlb_all] → ipmmu_tlb_flush_all()
 */
static void ipmmu_flush_iotlb_all(struct iommu_domain *io_domain)
{
	/* [한국어] 이 드라이버의 도메인으로 복원한다. */
	struct ipmmu_vmsa_domain *domain = to_vmsa_domain(io_domain);

	/* [한국어] attach된 도메인만 비울 컨텍스트가 있다. */
	if (domain->mmu)
		ipmmu_tlb_flush_all(domain);
}

/*
 * [한국어]
 * ipmmu_iotlb_sync - unmap 이후의 TLB 무효화 콜백
 *
 * @io_domain: 대상 도메인.
 * @gather: 무효화 범위를 모아 둔 구조체 — 이 하드웨어는 범위 무효화를
 *          지원하지 않아 무시한다.
 * @return: 없음.
 *
 * 범위를 따지지 않고 전체를 비운다. IMCTR의 FLUSH가 컨텍스트 단위로만
 * 동작하기 때문이다.
 *
 * 실행 컨텍스트: unmap 마무리 단계.
 *
 * 호출 체인:
 *   iommu_unmap()의 마무리 → domain_ops->iotlb_sync
 *   → [ipmmu_iotlb_sync] → ipmmu_flush_iotlb_all()
 */
static void ipmmu_iotlb_sync(struct iommu_domain *io_domain,
			     struct iommu_iotlb_gather *gather)
{
	/* [한국어] 범위를 무시하고 전체를 비운다. */
	ipmmu_flush_iotlb_all(io_domain);
}

/*
 * [한국어]
 * ipmmu_iova_to_phys - 주소 조회를 io-pgtable에 위임한다
 *
 * @io_domain: 대상 도메인.
 * @iova: 변환할 IOVA.
 * @return: 물리 주소, 매핑이 없으면 0.
 *
 * 원본의 TODO("Is locking needed?")가 남아 있다. io-pgtable의 워크는
 * READ_ONCE로 엔트리를 읽으므로 동시 갱신 중에도 크래시하지는 않지만,
 * 읽는 순간의 스냅숏이라 결과가 곧바로 낡을 수 있다. 조회 자체가
 * 디버깅용에 가까워 문제로 여기지 않은 것으로 보인다.
 *
 * 실행 컨텍스트: 조회 경로.
 *
 * 호출 체인:
 *   iommu_iova_to_phys() → domain_ops->iova_to_phys → [ipmmu_iova_to_phys]
 */
static phys_addr_t ipmmu_iova_to_phys(struct iommu_domain *io_domain,
				      dma_addr_t iova)
{
	/* [한국어] 이 드라이버의 도메인으로 복원한다. */
	struct ipmmu_vmsa_domain *domain = to_vmsa_domain(io_domain);

	/* TODO: Is locking needed ? */

	/* [한국어] 소프트웨어 워크를 io-pgtable에 맡긴다. */
	return domain->iop->iova_to_phys(domain->iop, iova);
}

/*
 * [한국어]
 * ipmmu_init_platform_device - 디바이스에 담당 IPMMU를 연결한다
 *
 * @dev: 클라이언트 디바이스.
 * @args: 파싱된 iommus 항목. args->np가 IPMMU 노드다.
 * @return: 0 성공, -ENODEV(그 노드의 플랫폼 디바이스가 없음).
 *
 * of_xlate가 처음 한 번만 부르는 함수다. phandle이 가리키는 IPMMU
 * 플랫폼 디바이스를 찾아 그 drvdata를 클라이언트의 priv에 심는다.
 *
 * 참조 카운트: of_find_device_by_node()가 올린 참조를 put_device로
 * 내린다. drvdata 포인터는 IPMMU가 살아 있는 한 유효하므로 참조를
 * 유지할 필요가 없다.
 *
 * 실행 컨텍스트: 디바이스 probe 경로.
 *
 * 호출 체인:
 *   ipmmu_of_xlate() → [ipmmu_init_platform_device]
 */
static int ipmmu_init_platform_device(struct device *dev,
				      const struct of_phandle_args *args)
{
	/* [한국어] phandle이 가리키는 IPMMU 플랫폼 디바이스. */
	struct platform_device *ipmmu_pdev;

	/* [한국어] 노드에 대응하는 플랫폼 디바이스를 찾는다. 참조가 하나 올라간다. */
	ipmmu_pdev = of_find_device_by_node(args->np);
	/* [한국어] 아직 그 IPMMU가 probe되지 않았다는 뜻이다. */
	if (!ipmmu_pdev)
		return -ENODEV;

	/* [한국어] IPMMU 인스턴스를 클라이언트의 priv에 심는다.
	 * 이후 to_ipmmu()가 이것을 꺼내 쓴다. */
	dev_iommu_priv_set(dev, platform_get_drvdata(ipmmu_pdev));

	/* [한국어] 올린 참조를 내린다. */
	put_device(&ipmmu_pdev->dev);

	return 0;	/* [한국어] uTLB 연결을 모두 끊었으므로 성공을 반환한다. */
}

/* [한국어] IPMMU 사용을 opt-in 방식으로 제한해야 하는 SoC 계열 목록.
 * 이 계열에 해당하면 아래 devices_allowlist에 있는 디바이스와 PCI만
 * IPMMU를 쓸 수 있다. 그 밖의 SoC(Gen2 등)는 제한 없이 허용된다.
 * 왜 이런 제한이 생겼나: Gen3 이후 IPMMU가 일부 마스터에서 제대로
 * 동작하지 않는 사례가 발견되어, 검증된 것만 열어 두는 보수적 방침을
 * 택했기 때문이다. */
static const struct soc_device_attribute soc_needs_opt_in[] = {
	/* [한국어] R-Car Gen3 계열 전체. */
	{ .family = "R-Car Gen3", },
	/* [한국어] R-Car Gen4 계열 전체. */
	{ .family = "R-Car Gen4", },
	/* [한국어] RZ/G2 계열 전체(Gen3와 같은 IP를 쓴다). */
	{ .family = "RZ/G2", },
	/* [한국어] 배열 끝을 알리는 빈 항목. */
	{ /* sentinel */ }
};

/* [한국어] IPMMU를 아예 쓸 수 없는 실리콘 목록.
 * 위 opt-in 계열에 속하면서 이 목록에도 있으면 어떤 디바이스도
 * IPMMU를 쓸 수 없다 — 알려진 하드웨어 결함 때문이다.
 * r8a7795는 리비전 ES2.*만 거부하는데, 이후 리비전에서 수정되었음을
 * 뜻한다. */
static const struct soc_device_attribute soc_denylist[] = {
	/* [한국어] RZ/G2M — 전 리비전 거부. */
	{ .soc_id = "r8a774a1", },
	/* [한국어] R-Car H3 — ES2 리비전만 거부. */
	{ .soc_id = "r8a7795", .revision = "ES2.*" },
	/* [한국어] R-Car M3-W — 전 리비전 거부. */
	{ .soc_id = "r8a7796", },
	/* [한국어] 배열 끝. */
	{ /* sentinel */ }
};

/* [한국어] opt-in 계열에서 IPMMU 사용이 검증된 디바이스 목록.
 * 전부 eMMC/SD 컨트롤러(SDHI)의 디바이스 이름이다 — 주소로 식별하므로
 * SoC마다 같은 이름이 쓰인다.
 * 이 목록이 짧다는 사실이 곧 "Gen3 이후 IPMMU는 실질적으로 스토리지에만
 * 쓰인다"는 현실을 보여 준다. */
static const char * const devices_allowlist[] = {
	/* [한국어] SDHI0. */
	"ee100000.mmc",
	/* [한국어] SDHI1. */
	"ee120000.mmc",
	/* [한국어] SDHI2. */
	"ee140000.mmc",
	/* [한국어] SDHI3. */
	"ee160000.mmc"
};

/*
 * [한국어]
 * ipmmu_device_is_allowed - 이 디바이스가 IPMMU를 써도 되는지 판단한다
 *
 * @dev: 검사할 클라이언트 디바이스.
 * @return: 허용하면 true.
 *
 * 판단 순서:
 *  1) opt-in이 필요 없는 SoC(Gen2 등)라면 무조건 허용한다.
 *  2) 그 SoC가 거부 목록에 있으면 무조건 거부한다.
 *  3) PCI 디바이스는 허용한다.
 *  4) 허용 목록(eMMC들)에 이름이 있으면 허용한다.
 *  5) 그 밖에는 거부한다.
 *
 * 이 함수가 of_xlate의 첫 관문이므로, 거부된 디바이스는 아예
 * priv에 IPMMU가 심어지지 않고 probe_device에서 -ENODEV가 된다.
 * 결과적으로 그 디바이스는 IOMMU 없이(= 물리 주소로) DMA하게 된다.
 *
 * 실행 컨텍스트: 디바이스 probe 경로.
 *
 * 호출 체인:
 *   ipmmu_of_xlate() → [ipmmu_device_is_allowed] → soc_device_match()
 */
static bool ipmmu_device_is_allowed(struct device *dev)
{
	/* [한국어] 허용 목록 순회 인덱스. */
	unsigned int i;

	/*
	 * R-Car Gen3/4 and RZ/G2 use the allow list to opt-in devices.
	 * For Other SoCs, this returns true anyway.
	 */
	/* [한국어] opt-in이 필요 없는 SoC라면 제한 없이 허용한다.
	 * Gen2가 여기 해당해, 옛 SoC의 동작이 바뀌지 않는다. */
	if (!soc_device_match(soc_needs_opt_in))
		return true;

	/* Check whether this SoC can use the IPMMU correctly or not */
	/* [한국어] 알려진 결함이 있는 실리콘이면 어떤 디바이스도 허용하지 않는다. */
	if (soc_device_match(soc_denylist))
		return false;

	/* Check whether this device is a PCI device */
	/* [한국어] PCI 디바이스는 이름으로 식별할 수 없고, 또 IPMMU와
	 * 함께 검증되었으므로 통째로 허용한다. */
	if (dev_is_pci(dev))
		return true;

	/* Check whether this device can work with the IPMMU */
	/* [한국어] 허용 목록에 이름이 있는지 확인한다. 디바이스 이름이
	 * 주소를 포함하므로("ee100000.mmc") 정확한 매칭이 가능하다. */
	for (i = 0; i < ARRAY_SIZE(devices_allowlist); i++) {
		if (!strcmp(dev_name(dev), devices_allowlist[i]))	/* [한국어] 디바이스 이름이 주소를 포함하므로 정확한 매칭이 된다. */
			return true;
	}

	/* Otherwise, do not allow use of IPMMU */
	/* [한국어] 검증되지 않은 디바이스는 IPMMU 없이 동작하게 둔다.
	 * 보수적이지만 잘못된 동작보다는 낫다는 판단이다. */
	return false;
}

/*
 * [한국어]
 * ipmmu_of_xlate - 디바이스 트리의 iommus 프로퍼티를 해석한다
 *
 * @dev: iommus 프로퍼티를 가진 클라이언트 디바이스.
 * @spec: 파싱된 항목. spec->np가 IPMMU 노드, spec->args[0]이 uTLB 번호다.
 * @return: 0 성공, -ENODEV(허용되지 않는 디바이스 또는 IPMMU 없음).
 *
 * 세 단계로 이뤄진다:
 *  1) 허용 목록 검사 — 거부되면 여기서 끝난다.
 *  2) uTLB 번호를 fwspec에 추가한다. 프로퍼티에 항목이 여럿이면
 *     이 콜백이 여러 번 불려 번호가 쌓인다.
 *  3) IPMMU 연결은 한 번만 한다 — 주석대로 xlate가 여러 번 불리므로,
 *     이미 priv가 채워져 있으면 건너뛴다.
 *
 * iommu_fwspec_add_ids의 반환값을 검사하지 않는 점은 기존 코드의
 * 허점이다(메모리 부족으로 실패할 수 있다).
 *
 * 실행 컨텍스트: 디바이스 probe 경로.
 *
 * 호출 체인:
 *   of_iommu_configure() → iommu_ops->of_xlate → [ipmmu_of_xlate]
 *   → ipmmu_device_is_allowed(), ipmmu_init_platform_device()
 */
static int ipmmu_of_xlate(struct device *dev,
			  const struct of_phandle_args *spec)
{
	/* [한국어] 허용되지 않는 디바이스는 IPMMU를 쓰지 못하게 한다.
	 * priv가 비어 있는 채로 남아 probe_device가 -ENODEV를 내게 된다. */
	if (!ipmmu_device_is_allowed(dev))
		return -ENODEV;

	/* [한국어] 이 항목의 uTLB 번호를 fwspec에 추가한다. attach가
	 * 그 배열을 순회해 uTLB들을 연결한다. */
	iommu_fwspec_add_ids(dev, spec->args, 1);

	/* Initialize once - xlate() will call multiple times */
	/* [한국어] 이미 IPMMU가 연결되어 있으면(두 번째 이후 항목) 끝낸다. */
	if (to_ipmmu(dev))
		return 0;

	/* [한국어] 첫 항목이라면 IPMMU 인스턴스를 priv에 심는다. */
	return ipmmu_init_platform_device(dev, spec);
}

/*
 * [한국어]
 * ipmmu_init_arm_mapping - ARM 32비트 레거시 DMA 매핑을 만들고 붙인다
 *
 * @dev: 대상 클라이언트 디바이스.
 * @return: 0 성공, 음수 errno(매핑 생성/attach 실패).
 *
 * CONFIG_ARM이면서 CONFIG_IOMMU_DMA가 없는 구성에서만 실제로 동작한다.
 * 그 외에서는 arm_iommu_* 스텁이 NULL/-ENODEV를 반환해 이 경로가
 * 사실상 비활성화된다.
 *
 * 매핑 범위(1GB 오프셋에서 2GB 크기)의 근거가 원본 TODO에 있다:
 * NULL 주소(IOVA 0)가 폴트를 내도록 0을 매핑 가능 범위에서 빼려는 것이다.
 * 그래서 시작을 SZ_1G로 잡았다. 다른 두 TODO는 컨텍스트마다 별도
 * 매핑을 두는 것과 크기를 설정 가능하게 하는 개선을 제안한다.
 *
 * 매핑을 IPMMU당 하나만 만들어 모든 클라이언트가 공유하는 점에 주목:
 * 그래서 같은 IPMMU에 붙는 디바이스들이 같은 IOVA 공간을 나눠 쓴다.
 *
 * 오류 경로가 다소 거칠다: attach 실패 시에도 mapping을 해제하는데,
 * 그것을 다른 디바이스가 이미 쓰고 있을 수 있다.
 *
 * 실행 컨텍스트: probe_finalize 경로.
 *
 * 호출 체인:
 *   ipmmu_probe_finalize() → [ipmmu_init_arm_mapping]
 *   → arm_iommu_create_mapping(), arm_iommu_attach_device()
 */
static int ipmmu_init_arm_mapping(struct device *dev)
{
	/* [한국어] 담당 IPMMU 인스턴스. */
	struct ipmmu_vmsa_device *mmu = to_ipmmu(dev);
	/* [한국어] 결과 코드. */
	int ret;

	/*
	 * Create the ARM mapping, used by the ARM DMA mapping core to allocate
	 * VAs. This will allocate a corresponding IOMMU domain.
	 *
	 * TODO:
	 * - Create one mapping per context (TLB).
	 * - Make the mapping size configurable ? We currently use a 2GB mapping
	 *   at a 1GB offset to ensure that NULL VAs will fault.
	 */
	/* [한국어] 이 IPMMU의 매핑이 아직 없으면 지금 만든다. 모든
	 * 클라이언트가 하나를 공유한다. */
	if (!mmu->mapping) {
		/* [한국어] 새로 만들 매핑. */
		struct dma_iommu_mapping *mapping;

		/* [한국어] 1GB 지점에서 2GB 크기의 IOVA 공간을 만든다.
		 * 0을 비워 두는 이유는 주석대로 NULL 주소 접근이 폴트를
		 * 내게 하려는 것이다 — 흔한 버그를 조기에 잡는 장치다.
		 * 이 호출 안에서 iommu_domain_alloc이 불려 도메인도 만들어진다. */
		mapping = arm_iommu_create_mapping(dev, SZ_1G, SZ_2G);
		if (IS_ERR(mapping)) {	/* [한국어] ARM 매핑 생성에 실패했다. */
			dev_err(mmu->dev, "failed to create ARM IOMMU mapping\n");	/* [한국어] 어느 IPMMU에서 실패했는지 남긴다. */
			ret = PTR_ERR(mapping);	/* [한국어] 오류 포인터에서 코드를 꺼낸다. */
			goto error;	/* [한국어] 정리 지점으로 간다. */
		}

		/* [한국어] 이후 클라이언트들이 공유하도록 보관한다. */
		mmu->mapping = mapping;
	}

	/* Attach the ARM VA mapping to the device. */
	/* [한국어] 이 디바이스를 매핑에 붙인다. 내부적으로
	 * iommu_attach_device를 거쳐 ipmmu_attach_device가 불리고,
	 * 거기서 컨텍스트 확보와 uTLB 연결이 일어난다. */
	ret = arm_iommu_attach_device(dev, mmu->mapping);
	if (ret < 0) {	/* [한국어] 디바이스를 매핑에 붙이지 못했다. */
		dev_err(dev, "Failed to attach device to VA mapping\n");	/* [한국어] DMA-OPS가 동작하지 않게 됨을 남긴다. */
		goto error;	/* [한국어] 정리 지점으로 간다. */
	}

	return 0;

/* [한국어] 매핑 생성이나 attach가 실패했을 때의 정리 지점. */
error:
	/* [한국어] 매핑을 해제한다. 다만 다른 디바이스가 이미 이 매핑을
	 * 쓰고 있을 수 있어, 이 정리가 지나칠 여지가 있다. */
	if (mmu->mapping)
		arm_iommu_release_mapping(mmu->mapping);

	return ret;	/* [한국어] 실패를 유발한 오류 코드를 반환한다. */
}

/*
 * [한국어]
 * ipmmu_probe_device - 이 디바이스를 담당할 IPMMU를 코어에 알린다
 *
 * @dev: 검사할 디바이스.
 * @return: 담당 IPMMU의 핸들, 없으면 ERR_PTR(-ENODEV).
 *
 * 원본 주석이 밝히듯, of_xlate에서 검증을 통과한 디바이스만 여기까지
 * 온다. 허용 목록에서 거부되었거나 iommus 프로퍼티가 없으면 priv가
 * 비어 있어 -ENODEV가 된다.
 *
 * 실행 컨텍스트: 디바이스 probe 경로.
 *
 * 호출 체인:
 *   iommu_probe_device() → iommu_ops->probe_device → [ipmmu_probe_device]
 */
static struct iommu_device *ipmmu_probe_device(struct device *dev)
{
	/* [한국어] of_xlate가 심어 둔 IPMMU. */
	struct ipmmu_vmsa_device *mmu = to_ipmmu(dev);

	/*
	 * Only let through devices that have been verified in xlate()
	 */
	/* [한국어] priv가 비어 있다는 것은 of_xlate가 이 디바이스를
	 * 거부했거나 아예 iommus 프로퍼티가 없었다는 뜻이다. */
	if (!mmu)
		return ERR_PTR(-ENODEV);

	/* [한국어] 담당 IPMMU의 코어 핸들을 돌려준다. */
	return &mmu->iommu;
}

/*
 * [한국어]
 * ipmmu_probe_finalize - 그룹 설정이 끝난 뒤 DMA 매핑을 붙인다
 *
 * @dev: 대상 디바이스.
 * @return: 없음(실패해도 알릴 수단이 없다).
 *
 * ARM 32비트 레거시 경로에서만 실제 작업이 있다. 최신 dma-iommu를
 * 쓰는 구성에서는 코어가 알아서 처리하므로 이 함수가 아무것도 하지 않는다.
 *
 * 실패해도 계속 진행하는 이유: DMA-OPS가 동작하지 않을 뿐 디바이스
 * 자체는 살아 있을 수 있어, 에러 메시지로 알리고 넘어간다.
 *
 * 실행 컨텍스트: 디바이스 probe 마무리 단계.
 *
 * 호출 체인:
 *   iommu_probe_device()의 마무리 → iommu_ops->probe_finalize
 *   → [ipmmu_probe_finalize] → ipmmu_init_arm_mapping()
 */
static void ipmmu_probe_finalize(struct device *dev)
{
	/* [한국어] 결과 코드. 레거시 경로가 아니면 0인 채로 남는다. */
	int ret = 0;

	/* [한국어] ARM 32비트이면서 최신 dma-iommu가 없는 구성에서만
	 * 레거시 매핑을 붙인다. IS_ENABLED를 쓰므로 다른 구성에서는
	 * 이 블록이 컴파일 단계에서 사라진다. */
	if (IS_ENABLED(CONFIG_ARM) && !IS_ENABLED(CONFIG_IOMMU_DMA))
		ret = ipmmu_init_arm_mapping(dev);

	/* [한국어] 실패하면 이 디바이스의 DMA가 IOMMU를 거치지 않게 된다.
	 * 되돌릴 방법이 없어 경고만 남긴다. */
	if (ret)
		dev_err(dev, "Can't create IOMMU mapping - DMA-OPS will not work\n");
}

/*
 * [한국어]
 * ipmmu_release_device - 디바이스가 사라질 때 uTLB 연결을 정리한다
 *
 * @dev: 제거되는 클라이언트 디바이스.
 * @return: 없음.
 *
 * 이 디바이스의 모든 uTLB를 비활성화하고 표식을 되돌린다.
 * utlb_disable()과 같은 일을 하지만 도메인을 거치지 않고 직접
 * 레지스터를 만지는데, 이 시점에는 도메인 연결이 이미 끊겼을 수
 * 있기 때문이다.
 *
 * arm_iommu_release_mapping을 부르는 점에 주목: 디바이스 하나가
 * 사라질 때마다 IPMMU 전체의 공유 매핑을 해제하는 셈이라,
 * 다른 디바이스가 아직 쓰고 있으면 문제가 될 수 있다. 실제로는
 * 참조 카운트 방식이라 마지막 사용자일 때만 해제된다.
 *
 * 실행 컨텍스트: 디바이스 제거 경로.
 *
 * 호출 체인:
 *   iommu_release_device() → iommu_ops->release_device
 *   → [ipmmu_release_device]
 */
static void ipmmu_release_device(struct device *dev)
{
	/* [한국어] 이 디바이스의 uTLB 번호들. */
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	/* [한국어] 담당 IPMMU. */
	struct ipmmu_vmsa_device *mmu = to_ipmmu(dev);
	/* [한국어] uTLB 순회 인덱스. */
	unsigned int i;

	/* [한국어] 이 디바이스가 쓰던 모든 uTLB를 정리한다. */
	for (i = 0; i < fwspec->num_ids; ++i) {
		/* [한국어] 현재 처리 중인 uTLB 번호. */
		unsigned int utlb = fwspec->ids[i];

		/* [한국어] 0을 써서 그 uTLB의 변환을 끈다. */
		ipmmu_imuctr_write(mmu, utlb, 0);
		/* [한국어] 리줌 대상에서 제외하도록 표식을 되돌린다. */
		mmu->utlb_ctx[utlb] = IPMMU_CTX_INVALID;
	}

	/* [한국어] 레거시 ARM 매핑의 참조를 내린다. 마지막 사용자면
	 * 실제로 해제되고, 아니면 참조만 줄어든다. */
	arm_iommu_release_mapping(mmu->mapping);
}

/* [한국어] IOMMU 코어에 노출하는 이 드라이버의 연산 테이블. */
static const struct iommu_ops ipmmu_ops = {
	/* [한국어] "변환 없음" 상태를 표현하는 정적 항등 도메인. */
	.identity_domain = &ipmmu_iommu_identity_domain,
	/* [한국어] 페이징 도메인 생성 — 껍데기만 만들고 컨텍스트는
	 * attach 때 확보한다. */
	.domain_alloc_paging = ipmmu_domain_alloc_paging,
	/* [한국어] 디바이스 담당 판정 — of_xlate의 검증을 통과했는지 본다. */
	.probe_device = ipmmu_probe_device,
	/* [한국어] 디바이스 제거 시 uTLB 연결과 레거시 매핑을 정리한다. */
	.release_device = ipmmu_release_device,
	/* [한국어] 그룹 설정 후 레거시 DMA 매핑을 붙이는 마무리 콜백. */
	.probe_finalize = ipmmu_probe_finalize,
	/*
	 * FIXME: The device grouping is a fixed property of the hardware's
	 * ability to isolate and control DMA, it should not depend on kconfig.
	 */
	/* [한국어] 그룹 결정 방식이 CONFIG에 따라 달라진다.
	 * 레거시 ARM 경로에서는 디바이스마다 개별 그룹(generic_device_group)을,
	 * 그 외에는 단독 그룹(generic_single_device_group)을 쓴다.
	 * 원본 FIXME가 지적하듯 그룹은 하드웨어의 격리 능력이 정하는
	 * 고정된 성질이어야 하고 빌드 설정에 좌우되어서는 안 된다 —
	 * 레거시 경로가 여러 디바이스를 한 매핑에 묶기 때문에 생긴 타협이다. */
	.device_group = IS_ENABLED(CONFIG_ARM) && !IS_ENABLED(CONFIG_IOMMU_DMA)
			? generic_device_group : generic_single_device_group,
	/* [한국어] iommus 프로퍼티 해석 — 허용 검사와 uTLB 번호 등록. */
	.of_xlate = ipmmu_of_xlate,
	/* [한국어] 페이징 도메인의 연산 테이블(익명 const 구조체). */
	.default_domain_ops = &(const struct iommu_domain_ops) {
		/* [한국어] 컨텍스트를 확보하고 uTLB를 연결한다. */
		.attach_dev	= ipmmu_attach_device,
		/* [한국어] 매핑 생성을 io-pgtable에 위임한다. */
		.map_pages	= ipmmu_map,
		/* [한국어] 매핑 해제를 io-pgtable에 위임한다. */
		.unmap_pages	= ipmmu_unmap,
		/* [한국어] 컨텍스트 전체 TLB 플러시. */
		.flush_iotlb_all = ipmmu_flush_iotlb_all,
		/* [한국어] 해제 후에도 범위를 따지지 않고 전체를 비운다. */
		.iotlb_sync	= ipmmu_iotlb_sync,
		/* [한국어] 주소 조회를 io-pgtable에 위임한다. */
		.iova_to_phys	= ipmmu_iova_to_phys,
		/* [한국어] 컨텍스트를 끄고 테이블과 구조체를 반납한다. */
		.free		= ipmmu_domain_free,
	}
};

/* -----------------------------------------------------------------------------
 * Probe/remove and init
 */

/*
 * [한국어]
 * ipmmu_device_reset - 모든 컨텍스트를 비활성화한다
 *
 * @mmu: 대상 인스턴스.
 * @return: 없음.
 *
 * IMCTR에 0을 쓰면 MMUEN과 INTEN이 모두 내려가 그 컨텍스트가 꺼진다.
 * 부트로더가 남긴 설정을 지우는 용도(probe)와 리줌 시 초기 상태를
 * 만드는 용도로 쓰인다.
 *
 * 실행 컨텍스트: probe, remove, 리줌 경로.
 *
 * 호출 체인:
 *   ipmmu_probe() / ipmmu_remove() / ipmmu_resume_noirq()
 *   → [ipmmu_device_reset]
 */
static void ipmmu_device_reset(struct ipmmu_vmsa_device *mmu)
{
	/* [한국어] 컨텍스트 순회 인덱스. */
	unsigned int i;

	/* Disable all contexts. */
	/* [한국어] 모든 컨텍스트의 제어 레지스터를 0으로 만들어 끈다. */
	for (i = 0; i < mmu->num_ctx; ++i)
		ipmmu_ctx_write(mmu, i, IMCTR, 0);
}

/* [한국어] R-Car Gen2(및 이름 없는 기본) IPMMU의 특성.
 * 계층 구조가 없어 IPMMU 하나가 전부를 하고, 컨텍스트도 하나만
 * 시험되었다는 주석이 붙어 있다. */
static const struct ipmmu_features ipmmu_features_default = {
	/* [한국어] 비보안 별칭 오프셋을 쓴다 — Gen2만 이 방식이다. */
	.use_ns_alias_offset = true,
	/* [한국어] 루트/캐시 계층이 없다. */
	.has_cache_leaf_nodes = false,
	/* [한국어] 컨텍스트 1개. 주석대로 소프트웨어가 하나만 시험되었다. */
	.number_of_contexts = 1, /* software only tested with one context */
	/* [한국어] uTLB 32개. */
	.num_utlbs = 32,
	/* [한국어] IMBUSCR 레지스터가 존재해 설정해야 한다. */
	.setup_imbuscr = true,
	/* [한국어] SL0 필드가 1비트 인코딩이다. */
	.twobit_imttbcr_sl0 = false,
	/* [한국어] 예약 컨텍스트가 없어 0번부터 쓸 수 있다. */
	.reserved_context = false,
	/* [한국어] 테이블 워크가 캐시를 스누핑하므로 공유성/캐시 비트를 켠다. */
	.cache_snoop = true,
	/* [한국어] 컨텍스트 레지스터가 오프셋 0에서 시작한다. */
	.ctx_offset_base = 0,
	/* [한국어] 컨텍스트 하나가 0x40바이트를 차지한다. */
	.ctx_offset_stride = 0x40,
	/* [한국어] uTLB 레지스터도 오프셋 0 기준이다. */
	.utlb_offset_base = 0,
};

/* [한국어] R-Car Gen3(와 RZ/G2) IPMMU의 특성.
 * Gen2와 가장 큰 차이는 루트/캐시 계층이 생겼다는 점이다. */
static const struct ipmmu_features ipmmu_features_rcar_gen3 = {
	/* [한국어] 별칭 오프셋을 쓰지 않는다 — 레지스터 배치가 달라졌다. */
	.use_ns_alias_offset = false,
	/* [한국어] 루트/캐시 계층이 있다. renesas,ipmmu-main으로 연결된다. */
	.has_cache_leaf_nodes = true,
	/* [한국어] 컨텍스트 8개 — 여러 도메인을 동시에 쓸 수 있게 되었다. */
	.number_of_contexts = 8,
	/* [한국어] uTLB 48개. 32를 넘으므로 IMUCTR32 경로가 쓰인다. */
	.num_utlbs = 48,
	/* [한국어] IMBUSCR가 없어 설정하지 않는다. */
	.setup_imbuscr = false,
	/* [한국어] SL0 필드가 2비트 인코딩으로 바뀌었다. */
	.twobit_imttbcr_sl0 = true,
	/* [한국어] 컨텍스트 0번이 예약되어 있어 할당에서 제외해야 한다. */
	.reserved_context = true,
	/* [한국어] 워크가 캐시를 스누핑하지 않아 관련 비트를 켜지 않는다. */
	.cache_snoop = false,
	/* [한국어] 컨텍스트 레지스터 베이스는 Gen2와 같다. */
	.ctx_offset_base = 0,
	/* [한국어] 컨텍스트 간격도 Gen2와 같다. */
	.ctx_offset_stride = 0x40,
	/* [한국어] uTLB 베이스도 Gen2와 같다. */
	.utlb_offset_base = 0,
};

/* [한국어] R-Car Gen4 IPMMU의 특성.
 * Gen3와 기능은 비슷하지만 레지스터 배치가 크게 바뀌어, 오프셋
 * 세 가지가 모두 다르다. */
static const struct ipmmu_features ipmmu_features_rcar_gen4 = {
	/* [한국어] 별칭 오프셋을 쓰지 않는다. */
	.use_ns_alias_offset = false,
	/* [한국어] 루트/캐시 계층이 있다. */
	.has_cache_leaf_nodes = true,
	/* [한국어] 컨텍스트 16개 — 이 드라이버의 상한(IPMMU_CTX_MAX)과 같다. */
	.number_of_contexts = 16,
	/* [한국어] uTLB 64개 — 역시 상한(IPMMU_UTLB_MAX)과 같다. */
	.num_utlbs = 64,
	/* [한국어] IMBUSCR가 없다. */
	.setup_imbuscr = false,
	/* [한국어] SL0가 2비트 인코딩이다. */
	.twobit_imttbcr_sl0 = true,
	/* [한국어] 컨텍스트 0번이 예약되어 있다. */
	.reserved_context = true,
	/* [한국어] 워크가 캐시를 스누핑하지 않는다. */
	.cache_snoop = false,
	/* [한국어] 컨텍스트 레지스터가 0x10000으로 옮겨 갔다. */
	.ctx_offset_base = 0x10000,
	/* [한국어] 컨텍스트 간격도 0x1040으로 크게 늘었다. */
	.ctx_offset_stride = 0x1040,
	/* [한국어] uTLB 레지스터도 0x3000으로 옮겨 갔다. */
	.utlb_offset_base = 0x3000,
};

/* [한국어] 디바이스 트리 compatible과 세대별 특성을 잇는 매칭 테이블.
 * SoC마다 별도 문자열을 두는 것이 Renesas의 관례라 항목이 많지만,
 * 실제로는 세 가지 features 중 하나를 가리킬 뿐이다.
 * of_device_get_match_data()가 매칭된 항목의 data를 돌려주므로,
 * probe가 그 값을 그대로 mmu->features에 넣는다. */
static const struct of_device_id ipmmu_of_ids[] = {
	{
		/* [한국어] 세대 무관 기본 바인딩 — Gen2에 해당한다. */
		.compatible = "renesas,ipmmu-vmsa",
		.data = &ipmmu_features_default,
	}, {
		/* [한국어] RZ/G2M. */
		.compatible = "renesas,ipmmu-r8a774a1",
		.data = &ipmmu_features_rcar_gen3,
	}, {
		/* [한국어] RZ/G2N. */
		.compatible = "renesas,ipmmu-r8a774b1",
		.data = &ipmmu_features_rcar_gen3,
	}, {
		/* [한국어] RZ/G2E. */
		.compatible = "renesas,ipmmu-r8a774c0",
		.data = &ipmmu_features_rcar_gen3,
	}, {
		/* [한국어] RZ/G2H. */
		.compatible = "renesas,ipmmu-r8a774e1",
		.data = &ipmmu_features_rcar_gen3,
	}, {
		/* [한국어] R-Car H3. */
		.compatible = "renesas,ipmmu-r8a7795",
		.data = &ipmmu_features_rcar_gen3,
	}, {
		/* [한국어] R-Car M3-W. */
		.compatible = "renesas,ipmmu-r8a7796",
		.data = &ipmmu_features_rcar_gen3,
	}, {
		/* [한국어] R-Car M3-W+. */
		.compatible = "renesas,ipmmu-r8a77961",
		.data = &ipmmu_features_rcar_gen3,
	}, {
		/* [한국어] R-Car M3-N. */
		.compatible = "renesas,ipmmu-r8a77965",
		.data = &ipmmu_features_rcar_gen3,
	}, {
		/* [한국어] R-Car V3M. */
		.compatible = "renesas,ipmmu-r8a77970",
		.data = &ipmmu_features_rcar_gen3,
	}, {
		/* [한국어] R-Car V3H. */
		.compatible = "renesas,ipmmu-r8a77980",
		.data = &ipmmu_features_rcar_gen3,
	}, {
		/* [한국어] R-Car E3. */
		.compatible = "renesas,ipmmu-r8a77990",
		.data = &ipmmu_features_rcar_gen3,
	}, {
		/* [한국어] R-Car D3. */
		.compatible = "renesas,ipmmu-r8a77995",
		.data = &ipmmu_features_rcar_gen3,
	}, {
		/* [한국어] R-Car V3U — Gen4 계열의 첫 칩이다. */
		.compatible = "renesas,ipmmu-r8a779a0",
		.data = &ipmmu_features_rcar_gen4,
	}, {
		/* [한국어] Gen4 계열의 일반 바인딩. 이후 칩들은 SoC별 문자열
		 * 대신 이것을 쓰도록 정리되었다. */
		.compatible = "renesas,rcar-gen4-ipmmu-vmsa",
		.data = &ipmmu_features_rcar_gen4,
	}, {
		/* [한국어] 배열 끝을 알리는 빈 항목. */
		/* Terminator */
	},
};

/*
 * [한국어]
 * ipmmu_probe - IPMMU 플랫폼 디바이스를 초기화한다
 *
 * @pdev: 디바이스 트리에서 매칭된 IPMMU 디바이스(루트이거나 캐시).
 * @return: 0 성공, -EPROBE_DEFER(루트가 아직 없음), 음수 errno(각 단계 실패).
 *
 * 이 함수의 흐름은 루트인지 캐시인지에 따라 갈린다:
 *  1) 공통: 인스턴스 할당, features 결정, DMA 마스크 설정, MMIO 매핑.
 *  2) Gen2라면 비보안 별칭 오프셋을 더한다(주석이 그 사정을 설명한다).
 *  3) 루트 판별: 계층이 없거나 renesas,ipmmu-main 프로퍼티가 없으면 루트.
 *     캐시라면 ipmmu_find_root()로 루트를 찾고, 아직 없으면 defer한다.
 *  4) 루트만: IRQ를 요청하고 모든 컨텍스트를 끄고, 예약 컨텍스트가
 *     있는 세대라면 0번을 미리 점유한다.
 *  5) IOMMU 코어 등록: Gen3 이후의 **루트는 등록하지 않는다** —
 *     클라이언트가 붙는 것은 캐시이기 때문이다. Gen2는 계층이 없어
 *     모두 등록된다.
 *
 * DMA 마스크를 40비트로 설정하는 이유: 이 IPMMU가 40비트 물리 주소를
 * 다룰 수 있어(cfg.oas = 40), 페이지 테이블도 그 범위에 있을 수 있다.
 *
 * 실행 컨텍스트: 디바이스 probe(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   플랫폼 버스 매칭 → driver->probe → [ipmmu_probe]
 *   → ipmmu_find_root(), ipmmu_device_reset(), iommu_device_register()
 */
static int ipmmu_probe(struct platform_device *pdev)
{
	/* [한국어] 만들 IPMMU 인스턴스. */
	struct ipmmu_vmsa_device *mmu;
	/* [한국어] 폴트 IRQ 번호(루트에서만 쓴다). */
	int irq;
	/* [한국어] 결과 코드. */
	int ret;

	/* [한국어] 인스턴스를 0으로 초기화해 할당한다. devm이라 자동 해제된다. */
	mmu = devm_kzalloc(&pdev->dev, sizeof(*mmu), GFP_KERNEL);
	if (!mmu) {	/* [한국어] 인스턴스를 할당하지 못했다. */
		dev_err(&pdev->dev, "cannot allocate device data\n");	/* [한국어] 메모리 부족을 남긴다. */
		return -ENOMEM;	/* [한국어] 초기화를 시작할 수 없다. */
	}

	/* [한국어] 로깅과 DMA에 쓸 device 포인터를 보관한다. */
	mmu->dev = &pdev->dev;
	/* [한국어] 컨텍스트 비트맵과 도메인 배열을 보호할 락을 초기화한다. */
	spin_lock_init(&mmu->lock);
	/* [한국어] 컨텍스트 비트맵을 비운다 — 모두 사용 가능 상태로 시작한다. */
	bitmap_zero(mmu->ctx, IPMMU_CTX_MAX);
	/* [한국어] 매칭된 compatible에 대응하는 세대별 특성을 가져온다.
	 * 이 한 줄이 세대별 차이를 모두 결정한다. */
	mmu->features = of_device_get_match_data(&pdev->dev);
	/* [한국어] uTLB→컨텍스트 매핑을 전부 INVALID(-1)로 채운다.
	 * memset이 바이트 단위로 채우는데 IPMMU_CTX_INVALID가 -1(0xff)이라
	 * s8 배열 전체가 -1이 되어 의도대로 동작한다. */
	memset(mmu->utlb_ctx, IPMMU_CTX_INVALID, mmu->features->num_utlbs);
	/* [한국어] 이 IPMMU가 40비트 물리 주소를 다룰 수 있음을 알린다.
	 * 페이지 테이블 할당이 그 범위 안에서 이뤄지게 된다. */
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(40));
	if (ret)	/* [한국어] 40비트 DMA 마스크를 설정하지 못하면 테이블 위치를 보장할 수 없다. */
		return ret;

	/* Map I/O memory and request IRQ. */
	/* [한국어] MMIO 레지스터 블록을 매핑한다. */
	mmu->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(mmu->base))	/* [한국어] MMIO 매핑 실패 — 레지스터에 접근할 수 없다. */
		return PTR_ERR(mmu->base);

	/*
	 * The IPMMU has two register banks, for secure and non-secure modes.
	 * The bank mapped at the beginning of the IPMMU address space
	 * corresponds to the running mode of the CPU. When running in secure
	 * mode the non-secure register bank is also available at an offset.
	 *
	 * Secure mode operation isn't clearly documented and is thus currently
	 * not implemented in the driver. Furthermore, preliminary tests of
	 * non-secure operation with the main register bank were not successful.
	 * Offset the registers base unconditionally to point to the non-secure
	 * alias space for now.
	 */
	/* [한국어] 위 주석이 설명하는 사정으로, Gen2에서는 무조건 비보안
	 * 별칭 영역을 가리키도록 베이스를 옮긴다. 주 뱅크로는 시험이
	 * 실패했고 보안 모드 동작은 문서화되어 있지 않기 때문이다. */
	if (mmu->features->use_ns_alias_offset)
		mmu->base += IM_NS_ALIAS_OFFSET;

	/* [한국어] 실제로 쓸 컨텍스트 개수를 정한다. 세대별 개수와
	 * 배열 상한 중 작은 쪽을 택해 배열 밖 접근을 막는다. */
	mmu->num_ctx = min(IPMMU_CTX_MAX, mmu->features->number_of_contexts);

	/*
	 * Determine if this IPMMU instance is a root device by checking for
	 * the lack of has_cache_leaf_nodes flag or renesas,ipmmu-main property.
	 */
	/* [한국어] 루트 판별. 계층이 없는 세대(Gen2)이거나,
	 * renesas,ipmmu-main 프로퍼티가 없으면(= 다른 IPMMU를 가리키지
	 * 않으면) 이 인스턴스가 루트다. */
	if (!mmu->features->has_cache_leaf_nodes ||
	    !of_property_present(pdev->dev.of_node, "renesas,ipmmu-main"))
		mmu->root = mmu;
	else
		/* [한국어] 캐시 IPMMU라면 이미 등록된 루트를 찾는다. */
		mmu->root = ipmmu_find_root();

	/*
	 * Wait until the root device has been registered for sure.
	 */
	/* [한국어] 루트가 아직 probe되지 않았다면 물러난다. 컨텍스트
	 * 레지스터가 루트에 있어 그것 없이는 아무것도 할 수 없기 때문이다.
	 * 커널이 나중에 다시 시도한다. */
	if (!mmu->root)
		return -EPROBE_DEFER;

	/* Root devices have mandatory IRQs */
	/* [한국어] 폴트 인터럽트는 루트에만 있다. 캐시 IPMMU의 폴트도
	 * 루트를 통해 보고되기 때문이다. */
	if (ipmmu_is_root(mmu)) {
		/* [한국어] 폴트 IRQ 번호를 얻는다. */
		irq = platform_get_irq(pdev, 0);
		if (irq < 0)	/* [한국어] 루트에는 폴트 인터럽트가 필수인데 얻지 못했다. */
			return irq;

		/* [한국어] 폴트 핸들러를 등록한다. 모든 컨텍스트의 폴트가
		 * 이 하나로 들어온다. */
		ret = devm_request_irq(&pdev->dev, irq, ipmmu_irq, 0,
				       dev_name(&pdev->dev), mmu);
		if (ret < 0) {	/* [한국어] IRQ 등록에 실패했다. */
			dev_err(&pdev->dev, "failed to request IRQ %d\n", irq);	/* [한국어] 어느 IRQ가 실패했는지 남긴다. */
			return ret;	/* [한국어] 폴트를 감지할 수 없는 상태로는 운용할 수 없다. */
		}

		/* [한국어] 부트로더가 남긴 컨텍스트 설정을 전부 지운다. */
		ipmmu_device_reset(mmu);

		/* [한국어] Gen3 이후는 컨텍스트 0번이 예약되어 있다.
		 * 비트맵에서 미리 점유해 할당되지 않게 만든다. */
		if (mmu->features->reserved_context) {
			dev_info(&pdev->dev, "IPMMU context 0 is reserved\n");	/* [한국어] 0번 컨텍스트가 예약되어 있음을 부팅 로그에 남긴다. */
			set_bit(0, mmu->ctx);	/* [한국어] 비트맵에서 미리 점유해 할당되지 않게 한다. */
		}
	}

	/* [한국어] of_xlate가 이 인스턴스를 찾을 수 있게 drvdata에 심는다.
	 * ipmmu_find_root()도 이 값을 읽으므로, 다른 IPMMU가 probe될 때
	 * 이 인스턴스를 발견할 수 있게 된다. */
	platform_set_drvdata(pdev, mmu);
	/*
	 * Register the IPMMU to the IOMMU subsystem in the following cases:
	 * - R-Car Gen2 IPMMU (all devices registered)
	 * - R-Car Gen3 IPMMU (leaf devices only - skip root IPMMU-MM device)
	 */
	/* [한국어] Gen3 이후의 루트(IPMMU-MM)는 IOMMU 코어에 등록하지 않는다.
	 * 클라이언트 디바이스가 실제로 붙는 것은 캐시 IPMMU이고, 루트는
	 * 컨텍스트 레지스터만 제공하는 내부 부품이기 때문이다.
	 * 여기서 0을 반환해도 probe는 성공이며, drvdata는 이미 설정되어
	 * 캐시들이 이 루트를 찾을 수 있다. */
	if (mmu->features->has_cache_leaf_nodes && ipmmu_is_root(mmu))
		return 0;

	/* [한국어] /sys/class/iommu/ 아래에 이 IPMMU를 노출한다. */
	ret = iommu_device_sysfs_add(&mmu->iommu, &pdev->dev, NULL, "%s",
				     dev_name(&pdev->dev));
	if (ret)	/* [한국어] sysfs 등록 실패 — 코어 등록으로 진행할 수 없다. */
		return ret;

	/* [한국어] IOMMU 코어에 연산 테이블을 등록한다. 이 시점부터
	 * of_xlate와 probe_device 콜백이 들어올 수 있다. */
	ret = iommu_device_register(&mmu->iommu, &ipmmu_ops, &pdev->dev);
	/* [한국어] 등록 실패 시 sysfs 노드를 되돌린다. ret이 그대로
	 * 아래에서 반환된다. */
	if (ret)
		iommu_device_sysfs_remove(&mmu->iommu);

	/* [한국어] 성공이면 0, 등록 실패면 그 오류. */
	return ret;
}

/*
 * [한국어]
 * ipmmu_remove - IPMMU 디바이스를 정리한다
 *
 * @pdev: 제거되는 플랫폼 디바이스.
 * @return: 없음.
 *
 * sysfs와 코어 등록을 해제하고, 레거시 매핑을 반납한 뒤 모든
 * 컨텍스트를 끈다. 하드웨어를 마지막에 끄는 것이 자연스러운데,
 * 코어 등록이 살아 있는 동안에는 새 attach가 들어올 수 있기 때문이다.
 *
 * Gen3 이후의 루트는 애초에 등록되지 않았는데도 unregister를 부르는
 * 점이 다소 거칠지만, 코어가 등록되지 않은 디바이스를 안전하게 다룬다.
 *
 * 실행 컨텍스트: 디바이스 제거 경로. 아래 builtin_platform_driver로
 * 등록되므로 실질적으로는 거의 실행되지 않는다.
 *
 * 호출 체인:
 *   플랫폼 버스 → driver->remove → [ipmmu_remove]
 */
static void ipmmu_remove(struct platform_device *pdev)
{
	/* [한국어] probe가 심어 둔 인스턴스를 꺼낸다. */
	struct ipmmu_vmsa_device *mmu = platform_get_drvdata(pdev);

	/* [한국어] sysfs 노드를 제거한다. */
	iommu_device_sysfs_remove(&mmu->iommu);
	/* [한국어] 코어 등록을 해제해 더 이상 콜백이 들어오지 않게 한다. */
	iommu_device_unregister(&mmu->iommu);

	/* [한국어] 레거시 ARM 매핑을 반납한다(그 경로가 아니면 스텁이라
	 * 아무 일도 하지 않는다). */
	arm_iommu_release_mapping(mmu->mapping);

	/* [한국어] 마지막으로 모든 컨텍스트를 꺼 하드웨어를 조용하게 만든다. */
	ipmmu_device_reset(mmu);
}

/*
 * [한국어]
 * ipmmu_resume_noirq - 리줌 시 IPMMU 상태를 복원한다
 *
 * @dev: IPMMU 디바이스.
 * @return: 항상 0.
 *
 * 서스펜드로 레지스터가 날아간 뒤 원래 상태를 되살린다. 두 부분이다:
 *  1) 루트라면: 모든 컨텍스트를 끈 뒤, 살아 있는 도메인마다
 *     setup_context()를 다시 불러 TTBR/MAIR/IMCTR을 복원한다.
 *     도메인 정보(domains[])는 메모리에 남아 있으므로 그것을 근거로
 *     하드웨어를 재구성하는 방식이다.
 *  2) 루트든 캐시든: utlb_ctx[]를 훑어 활성 상태였던 uTLB를 모두
 *     되살린다. 어느 컨텍스트에 속했는지가 그 배열에 기록되어 있다.
 *
 * suspend 콜백이 없는 점에 주목: 서스펜드 시 별도로 저장할 것이 없다.
 * 모든 상태가 이미 소프트웨어 자료구조에 있어, 리줌에서 그것을
 * 하드웨어에 다시 쓰기만 하면 되기 때문이다.
 *
 * noirq 단계인 이유: 다른 디바이스들이 깨어나 DMA를 시작하기 전에
 * IOMMU가 준비되어야 하므로, 인터럽트가 아직 비활성인 이른 단계에서
 * 복원한다.
 *
 * 실행 컨텍스트: PM 코어의 noirq 리줌 단계.
 *
 * 호출 체인:
 *   PM 코어 → dev_pm_ops의 noirq resume → [ipmmu_resume_noirq]
 *   → ipmmu_domain_setup_context(), ipmmu_utlb_enable()
 */
static int ipmmu_resume_noirq(struct device *dev)
{
	/* [한국어] drvdata에서 IPMMU 인스턴스를 꺼낸다. */
	struct ipmmu_vmsa_device *mmu = dev_get_drvdata(dev);
	/* [한국어] 순회 인덱스(컨텍스트와 uTLB 양쪽에 재사용된다). */
	unsigned int i;

	/* Reset root MMU and restore contexts */
	/* [한국어] 컨텍스트 레지스터는 루트에만 있으므로 루트에서만 복원한다. */
	if (ipmmu_is_root(mmu)) {
		/* [한국어] 먼저 모든 컨텍스트를 꺼 깨끗한 상태를 만든다. */
		ipmmu_device_reset(mmu);

		/* [한국어] 살아 있는 도메인마다 컨텍스트를 다시 프로그래밍한다. */
		for (i = 0; i < mmu->num_ctx; i++) {
			/* [한국어] 쓰이지 않는 컨텍스트는 건너뛴다. */
			if (!mmu->domains[i])
				continue;

			/* [한국어] TTBR/TTBCR/MAIR을 다시 쓰고 MMU를 켠다.
			 * attach 때와 같은 함수를 재사용하는 것이 이 설계의 이점이다. */
			ipmmu_domain_setup_context(mmu->domains[i]);
		}
	}

	/* Re-enable active micro-TLBs */
	/* [한국어] uTLB 레지스터는 각 IPMMU에 있으므로 루트/캐시 구분 없이
	 * 자기 것을 복원한다. */
	for (i = 0; i < mmu->features->num_utlbs; i++) {
		/* [한국어] 어느 컨텍스트에도 속하지 않던 uTLB는 건너뛴다. */
		if (mmu->utlb_ctx[i] == IPMMU_CTX_INVALID)
			continue;

		/* [한국어] 기록해 둔 컨텍스트 번호로 루트의 도메인을 찾아
		 * 그 uTLB를 다시 연결한다. 이 한 줄을 위해 utlb_ctx 배열을
		 * 유지해 온 것이다. */
		ipmmu_utlb_enable(mmu->root->domains[mmu->utlb_ctx[i]], i);
	}

	return 0;	/* [한국어] 컨텍스트와 uTLB 복원이 모두 끝났다. */
}

/* [한국어] 전원 관리 콜백 테이블.
 * NOIRQ_SYSTEM_SLEEP_PM_OPS가 suspend를 NULL로, resume을 위 함수로
 * 연결한다. suspend가 없는 이유는 저장할 상태가 소프트웨어 쪽에
 * 이미 다 있기 때문이다.
 * noirq 단계를 쓰는 것은 다른 디바이스가 DMA를 시작하기 전에
 * IOMMU가 준비되어야 하기 때문이다. */
static const struct dev_pm_ops ipmmu_pm  = {
	NOIRQ_SYSTEM_SLEEP_PM_OPS(NULL, ipmmu_resume_noirq)	/* [한국어] suspend는 없고 noirq resume만 연결한다 — 저장할 상태가 소프트웨어에 이미 있다. */
};

/* [한국어] 플랫폼 버스에 등록할 드라이버 정의. */
static struct platform_driver ipmmu_driver = {
	/* [한국어] 드라이버 공통 정보. */
	.driver = {
		/* [한국어] sysfs와 로그에 나타날 이름. */
		.name = "ipmmu-vmsa",
		/* [한국어] 세대별 특성을 담은 매칭 테이블. */
		.of_match_table = ipmmu_of_ids,
		/* [한국어] 서스펜드/리줌 콜백. pm_sleep_ptr는
		 * CONFIG_PM_SLEEP이 꺼져 있으면 NULL로 접혀,
		 * 관련 코드가 빌드에서 빠지게 해 준다. */
		.pm = pm_sleep_ptr(&ipmmu_pm),
	},
	/* [한국어] 초기화 진입점. */
	.probe = ipmmu_probe,
	/* [한국어] 정리 진입점. */
	.remove = ipmmu_remove,
};
/* [한국어] 빌트인 드라이버로 등록한다. 모듈이 아니므로 언로드가 없고,
 * IOMMU를 임의로 제거할 수 없게 된다 — 클라이언트의 DMA 매핑이
 * 통째로 무효화되는 사고를 원천 차단하는 선택이다. */
builtin_platform_driver(ipmmu_driver);
