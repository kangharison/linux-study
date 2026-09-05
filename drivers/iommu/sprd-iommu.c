// SPDX-License-Identifier: GPL-2.0-only
/*
 * Unisoc IOMMU driver
 *
 * Copyright (C) 2020 Unisoc, Inc.
 * Author: Chunyan Zhang <chunyan.zhang@unisoc.com>
 */

/*
 * [한국어 설명] Unisoc(구 Spreadtrum) SoC용 IOMMU 드라이버 (sprd-iommu.c)
 *
 * === 파일의 역할 ===
 * Unisoc 모바일 SoC에 들어 있는 단순한 IOMMU IP를 리눅스 IOMMU 서브시스템에
 * 붙이는 드라이버다. 이 하드웨어의 가장 큰 특징은 **페이지 테이블이 1단계
 * 평면 배열(flat array)**이라는 점이다. 다단계 워크가 없고, IOVA에서
 * aperture 시작을 뺀 뒤 페이지 시프트만 하면 곧바로 배열 인덱스가 된다.
 * 각 엔트리는 u32 하나로 물리 페이지 번호(PFN)만 담으며, 권한 비트조차 없다.
 * 그래서 map/unmap이 각각 "배열에 PFN 쓰기"와 "memset(0)"으로 끝난다.
 * 아래 두 가지가 이 설계를 가능하게 한다.
 *  - IOVA 공간이 256MB로 고정되어 있어, 4KB 페이지 기준 엔트리 65536개
 *    (256KB 메모리)면 전체를 덮는다. 다단계로 절약할 이유가 없다.
 *  - IOMMU 하나가 클라이언트 디바이스 하나만 담당한다(카메라, 비디오 코덱
 *    같은 멀티미디어 IP마다 전용 IOMMU가 하나씩 붙는다). 그래서 도메인과
 *    디바이스가 사실상 1:1이고, 그룹도 generic_single_device_group이다.
 * 이 IP에는 두 세대가 있다 — EX와 VAU. 차이는 레지스터 오프셋과, VAU가
 * 읽기/쓰기용 기본 PPN 레지스터를 따로 둔다는 점 정도다. 드라이버는 버전
 * 레지스터를 읽어 판별한 뒤 오프셋만 갈아 끼운다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *
 *   [멀티미디어 드라이버] dma_map_*() / iommu_map()
 *        ↓
 *   [IOMMU 코어] iommu_ops 콜백 디스패치
 *        ↓
 *   [이 파일] sprd_iommu_map() → pgt_va[] 배열에 PFN 기록
 *        ↓ iotlb_sync_map 콜백
 *   [이 파일] SPRD_*_UPDATE 레지스터에 0xffffffff 기록 = TLB 전체 무효화
 *        ↓
 *   [SPRD IOMMU 하드웨어] FIRST_PPN 레지스터가 가리키는 평면 테이블을 직접 인덱싱
 *
 * attach 시점의 하드웨어 설정 순서가 중요하다:
 *   IOMMU 끄기 → FIRST_PPN(테이블 위치) → FIRST_VPN(IOVA 시작)
 *   → VPN_RANGE(IOVA 범위) → DEFAULT_PPN(폴트 시 보호 페이지) → IOMMU 켜기.
 * 끄고 설정한 뒤 켜는 이유는, 이전 클라이언트의 테이블이 남아 있는 상태에서
 * 새 설정이 섞여 들어가는 것을 막기 위해서다(원본 주석이 지적하는 그대로).
 *
 * 실행 컨텍스트: probe/attach는 프로세스 컨텍스트. map/unmap/iova_to_phys는
 * DMA API 경로라 atomic 컨텍스트일 수 있어, 페이지 테이블 접근을
 * spin_lock_irqsave로 보호한다.
 *
 * === 타 모듈과의 연결 ===
 * - linux/iommu.h: iommu_ops/iommu_domain_ops 계약. 이 드라이버는
 *   io-pgtable 프레임워크를 쓰지 않고 테이블을 직접 관리한다.
 * - linux/dma-mapping.h: dma_alloc_coherent(). 페이지 테이블 자체와 폴트용
 *   보호 페이지를 코히런트 DMA 메모리로 잡는다 — 하드웨어가 직접 읽는
 *   메모리라 캐시 일관성이 필요하기 때문이다.
 * - linux/clk.h: 일부 IOMMU 인스턴스는 게이트 클록을 켜야 레지스터에
 *   접근할 수 있다. devm_clk_get_optional로 "있으면 켜고 없으면 넘어간다".
 * - linux/of_platform.h: of_xlate 경로에서 디바이스 트리의 iommus 프로퍼티가
 *   가리키는 IOMMU 플랫폼 디바이스를 찾아 클라이언트의 priv에 심는다.
 * 데이터 흐름: 디바이스 트리의 `iommus = <&iommu ...>` → of_xlate가
 * sprd_iommu_device를 클라이언트의 dev_iommu_priv에 저장 → probe_device가
 * 그것으로 iommu_device를 반환 → attach 때 도메인과 하드웨어를 연결.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct sprd_iommu_device: IOMMU 인스턴스 하나. 레지스터 베이스, 버전,
 *   폴트용 보호 페이지, 그리고 현재 붙어 있는 도메인 포인터.
 * - struct sprd_iommu_domain: 도메인 하나. 평면 페이지 테이블의 가상/물리
 *   주소와 그것을 보호하는 스핀락.
 * - sprd_iommu_attach_device(): 테이블을 할당하고(최초 1회) 하드웨어
 *   레지스터를 순서대로 프로그래밍한다.
 * - sprd_iommu_map()/unmap(): 평면 배열에 PFN을 쓰거나 0으로 지운다.
 * - sprd_iommu_sync_map(): UPDATE 레지스터에 0xffffffff를 써 TLB를 통째로 비운다.
 * - sprd_iommu_probe(): MMIO 매핑, 보호 페이지 할당, 코어 등록, 클록,
 *   버전 판별을 차례로 수행하고 실패 시 역순으로 되감는다.
 */

/* [한국어] 게이트 클록 제어 API — 일부 IOMMU는 클록을 켜야 레지스터가 살아난다. */
#include <linux/clk.h>
/* [한국어] struct device와 dev_err/dev_iommu_priv_* 접근자. */
#include <linux/device.h>
/* [한국어] dma_alloc_coherent()/dma_free_coherent(). 페이지 테이블과 보호
 * 페이지를 하드웨어가 직접 읽으므로 코히런트 메모리로 잡아야 한다. */
#include <linux/dma-mapping.h>
/* [한국어] -EINVAL, -ENOMEM 등 표준 오류 코드. */
#include <linux/errno.h>
/* [한국어] IOMMU 코어 계약 — iommu_ops, iommu_domain, iommu_device 등. */
#include <linux/iommu.h>
/* [한국어] syscon regmap 헤더. 현재 코드에서 직접 쓰이지는 않지만,
 * 일부 SoC 변형이 시스템 컨트롤러를 통해 IOMMU를 제어하던 흔적이다. */
#include <linux/mfd/syscon.h>
/* [한국어] MODULE_* 매크로와 THIS_MODULE — 이 드라이버는 모듈로 빌드될 수 있다. */
#include <linux/module.h>
/* [한국어] of_find_device_by_node() — of_xlate에서 디바이스 트리 노드로부터
 * 플랫폼 디바이스를 역추적하는 데 쓴다. */
#include <linux/of_platform.h>
/* [한국어] platform_driver/platform_device와 devm_platform_ioremap_resource(). */
#include <linux/platform_device.h>
/* [한국어] regmap API. syscon과 마찬가지로 현재는 직접 쓰이지 않는다. */
#include <linux/regmap.h>
/* [한국어] kzalloc_obj()/kfree()/devm_kzalloc() — 도메인과 디바이스 구조체 할당. */
#include <linux/slab.h>

/* [한국어] 페이지 크기의 로그값. IOVA를 페이지 테이블 인덱스로 바꾸는 시프트량이자,
 * 물리 주소를 PFN으로 바꾸는 시프트량이다. 이 드라이버 전체에서 가장 자주
 * 등장하는 상수다. */
#define SPRD_IOMMU_PAGE_SHIFT	12
/* [한국어] 페이지 크기 4KB. 이 하드웨어는 단일 페이지 크기만 지원하므로
 * domain->pgsize_bitmap에 이 값 하나만 실린다. */
#define SPRD_IOMMU_PAGE_SIZE	SZ_4K

/* [한국어] 아래는 1세대(EX) IP의 레지스터 오프셋들이다.
 * SPRD_EX_CFG — 설정 레지스터. IOMMU 활성화와 폴트 동작을 제어한다. */
#define SPRD_EX_CFG		0x0
/* [한국어] VAOR(Virtual Address Out of Range) 바이패스 비트. 범위를 벗어난
 * 주소 접근을 폴트로 처리할지 그냥 통과시킬지 결정한다.
 * 이 드라이버는 이 비트를 건드리지 않고 하드웨어 기본값에 맡긴다. */
#define SPRD_IOMMU_VAOR_BYPASS	BIT(4)
/* [한국어] 게이트 클록 활성화 비트. IOMMU 내부 클록 게이팅을 켠다 —
 * 전력 절감을 위해 유휴 시 클록을 멈출 수 있게 하는 설정이다. */
#define SPRD_IOMMU_GATE_EN	BIT(1)
/* [한국어] IOMMU 활성화 비트. 이 비트가 0이면 주소 변환 없이 통과한다.
 * sprd_iommu_hw_en()이 GATE_EN과 함께 세우거나 내린다. */
#define SPRD_IOMMU_EN		BIT(0)
/* [한국어] TLB 무효화 레지스터(EX). 여기에 0xffffffff를 쓰면 TLB 전체가
 * 비워진다. 범위 지정 무효화가 없어 항상 통째로 비운다. */
#define SPRD_EX_UPDATE		0x4
/* [한국어] 이 IOMMU가 담당하는 IOVA 영역의 시작 VPN(가상 페이지 번호). */
#define SPRD_EX_FIRST_VPN	0x8
/* [한국어] IOVA 영역의 크기를 페이지 수로 표현한 레지스터.
 * FIRST_VPN과 함께 "어느 IOVA 구간을 변환할지"를 정한다. */
#define SPRD_EX_VPN_RANGE	0xc
/* [한국어] 페이지 테이블 자체가 놓인 물리 페이지 번호. 하드웨어는 이 값에
 * IOVA 인덱스를 더해 곧바로 엔트리를 읽는다(다단계 워크 없음). */
#define SPRD_EX_FIRST_PPN	0x10
/* [한국어] 변환 실패 시 접근을 대신 보낼 물리 페이지 번호.
 * 폴트가 나도 시스템이 죽지 않도록 쓰레기 데이터를 흘려보낼 "보호 페이지"를
 * 가리킨다 — 이 IP에는 폴트 인터럽트 처리 경로가 없기 때문이다. */
#define SPRD_EX_DEFAULT_PPN	0x14

/* [한국어] 아래는 2세대(VAU) IP의 레지스터 오프셋들이다. EX와 배치가 달라
 * 모든 접근이 버전 분기를 거친다.
 * SPRD_IOMMU_VERSION — 버전 레지스터. probe에서 세대를 판별하는 근거다.
 * 오프셋 0x0이 EX의 CFG와 겹치는데, VAU에서는 그 자리가 버전 정보다. */
#define SPRD_IOMMU_VERSION	0x0
/* [한국어] 버전 레지스터에서 실제 버전 번호가 실린 비트 8~15. */
#define SPRD_VERSION_MASK	GENMASK(15, 8)
/* [한국어] 위 필드를 오른쪽 끝으로 내리는 시프트량. 결과값이
 * enum sprd_iommu_version(0=EX, 1=VAU)과 직접 비교된다. */
#define SPRD_VERSION_SHIFT	0x8
/* [한국어] VAU의 설정 레지스터. EX_CFG와 같은 활성화 비트 배치를 쓴다. */
#define SPRD_VAU_CFG		0x4
/* [한국어] VAU의 TLB 무효화 레지스터. */
#define SPRD_VAU_UPDATE		0x8
/* [한국어] VAU의 접근 권한(authority) 설정 레지스터. 이 드라이버는 사용하지
 * 않고 하드웨어 기본값을 그대로 둔다. */
#define SPRD_VAU_AUTH_CFG	0xc
/* [한국어] VAU의 페이지 테이블 물리 페이지 번호 레지스터. */
#define SPRD_VAU_FIRST_PPN	0x10
/* [한국어] VAU의 읽기 폴트용 기본 PPN. EX와 달리 읽기/쓰기를 나눠 둬서,
 * 폴트 종류별로 다른 페이지로 보낼 수 있다(이 드라이버는 같은 값을 쓴다). */
#define SPRD_VAU_DEFAULT_PPN_RD	0x14
/* [한국어] VAU의 쓰기 폴트용 기본 PPN. */
#define SPRD_VAU_DEFAULT_PPN_WR	0x18
/* [한국어] VAU의 IOVA 시작 VPN 레지스터. */
#define SPRD_VAU_FIRST_VPN	0x1c
/* [한국어] VAU의 IOVA 범위(페이지 수) 레지스터. */
#define SPRD_VAU_VPN_RANGE	0x20

/* [한국어] IP 세대 구분. 값 자체가 버전 레지스터에서 읽은 숫자와 일치하도록
 * 정의되어 있어(EX=0, VAU=1), sprd_iommu_get_version()이 읽은 값을 그대로
 * 이 enum으로 쓸 수 있다. 그 밖의 값이 나오면 지원하지 않는 IP다. */
enum sprd_iommu_version {
	SPRD_IOMMU_EX,
	/* [한국어] 1세대 IP. 레지스터가 SPRD_EX_* 오프셋에 배치되고,
	 * 폴트용 기본 PPN이 읽기/쓰기 공용으로 하나뿐이다.
	 * 설정자: sprd_iommu_get_version()이 버전 레지스터에서 읽어 판별.
	 * 읽는 자: 레지스터 오프셋을 고르는 모든 함수의 분기 조건. */

	SPRD_IOMMU_VAU,
	/* [한국어] 2세대 IP. SPRD_VAU_* 오프셋을 쓰고, 읽기/쓰기 폴트용
	 * 기본 PPN 레지스터가 분리되어 있으며 권한 설정 레지스터가 추가됐다.
	 * 설정자/읽는 자: 위와 동일. */
};

/*
 * struct sprd_iommu_device - high-level sprd IOMMU device representation,
 * including hardware information and configuration, also driver data, etc
 *
 * @ver: sprd IOMMU IP version
 * @prot_page_va: protect page base virtual address
 * @prot_page_pa: protect page base physical address, data would be
 *		  written to here while translation fault
 * @base: mapped base address for accessing registers
 * @dev: pointer to basic device structure
 * @iommu: IOMMU core representation
 * @group: IOMMU group
 * @eb: gate clock which controls IOMMU access
 */
/* [한국어] IOMMU 인스턴스 하나를 표현하는 구조체.
 * 수명: probe에서 devm_kzalloc으로 만들어져 디바이스와 함께 사라진다.
 * 소유자: 플랫폼 디바이스의 drvdata이자, 클라이언트 디바이스들의
 *         dev_iommu_priv가 가리키는 대상이다.
 * 참고: 위 kernel-doc은 @group을 언급하지만 실제 필드는 없다 —
 *       generic_single_device_group()으로 대체되면서 제거된 흔적이다.
 *       반대로 첫 필드 @dom은 문서에 빠져 있다. */
struct sprd_iommu_device {
	struct sprd_iommu_domain	*dom;
	/* [한국어] 현재 이 IOMMU에 붙어 있는 도메인.
	 * 설정자: sprd_iommu_attach_device()가 attach 시 기록.
	 * 읽는 자: 같은 함수가 "이미 이 도메인이 붙어 있는가"를 판별해
	 *          중복 설정을 건너뛰는 데 쓴다.
	 * 값 범위: NULL(아무 도메인도 붙지 않음) 또는 유효한 도메인 포인터.
	 * 왜 하나뿐인가: 이 IP는 클라이언트 디바이스 하나만 담당하므로
	 *                동시에 여러 도메인이 붙을 일이 없다.
	 * 동기화: attach는 IOMMU 코어가 그룹 뮤텍스 아래에서 직렬화한다. */

	enum sprd_iommu_version	ver;
	/* [한국어] 이 IP의 세대(EX 또는 VAU).
	 * 설정자: sprd_iommu_probe()가 sprd_iommu_get_version() 결과를 저장.
	 * 읽는 자: 레지스터 오프셋을 고르는 모든 함수
	 *          (first_vpn/vpn_range/first_ppn/default_ppn/hw_en/sync_map).
	 * 값 범위: SPRD_IOMMU_EX 또는 SPRD_IOMMU_VAU.
	 * 동기화: probe에서 한 번 정해지고 이후 읽기 전용. */

	u32			*prot_page_va;
	/* [한국어] 변환 실패 시 접근이 흘러갈 "보호 페이지"의 커널 가상 주소.
	 * 설정자: probe가 dma_alloc_coherent()로 4KB를 할당.
	 * 읽는 자: 해제 경로(probe 실패, remove)가 dma_free_coherent()에 넘긴다.
	 *          내용 자체는 아무도 읽지 않는다 — 존재만으로 역할을 한다.
	 * 값 범위: 유효한 커널 가상 주소.
	 * 왜 필요한가: 이 IP에는 폴트 인터럽트 처리 경로가 없어, 잘못된 DMA를
	 *              막을 수단이 "쓰레기를 버릴 곳"뿐이다. 실제 메모리를
	 *              오염시키는 대신 이 페이지로 보낸다.
	 * 동기화: probe/remove에서만 다루므로 락이 없다. */

	dma_addr_t		prot_page_pa;
	/* [한국어] 위 보호 페이지의 DMA(물리) 주소.
	 * 설정자: dma_alloc_coherent()가 출력 인자로 채운다.
	 * 읽는 자: sprd_iommu_default_ppn()이 페이지 시프트해 DEFAULT_PPN
	 *          레지스터에 기록한다 — 하드웨어가 폴트 시 이 주소로 접근을 돌린다.
	 * 값 범위: 4KB 정렬된 DMA 주소.
	 * 동기화: probe 이후 불변. */

	void __iomem		*base;
	/* [한국어] 이 IOMMU의 MMIO 레지스터 블록이 매핑된 커널 주소.
	 * 설정자: probe의 devm_platform_ioremap_resource().
	 * 읽는 자: sprd_iommu_read()/write()가 여기에 오프셋을 더해 접근한다.
	 * 값 범위: ioremap된 __iomem 포인터 — 반드시 readl/writel 계열로만 접근해야 한다.
	 * 동기화: devm 관리라 디바이스 해제 시 자동 언매핑된다. */

	struct device		*dev;
	/* [한국어] 이 IOMMU 자신의 struct device.
	 * 설정자: probe가 &pdev->dev를 저장.
	 * 읽는 자: dma_alloc_coherent()/dma_free_coherent()의 첫 인자(DMA 마스터가
	 *          IOMMU 자신이므로)와 dev_err() 로깅.
	 * 값 범위: 유효한 플랫폼 디바이스 포인터.
	 * 동기화: probe 이후 불변. */

	struct iommu_device	iommu;
	/* [한국어] IOMMU 코어에 등록되는 핸들(임베드된 구조체).
	 * 설정자: probe의 iommu_device_sysfs_add()/iommu_device_register().
	 * 읽는 자: sprd_iommu_probe_device()가 "이 디바이스는 이 IOMMU 소관"이라는
	 *          뜻으로 그 주소를 반환한다.
	 * 값 범위: 등록 후 유효.
	 * 동기화: 등록/해제는 probe/remove에서만 일어난다. */

	struct clk		*eb;
	/* [한국어] 이 IOMMU의 접근을 제어하는 게이트 클록(enable bit clock).
	 * 설정자: sprd_iommu_clk_enable()이 devm_clk_get_optional() 결과를 저장.
	 * 읽는 자: sprd_iommu_clk_disable()이 해제 시 확인한다.
	 * 값 범위: NULL(이 인스턴스는 클록이 필요 없음) 또는 유효한 클록 포인터.
	 * 왜 optional인가: 같은 드라이버가 담당하는 여러 IOMMU 인스턴스 중
	 *                  일부만 별도 게이트 클록을 갖기 때문이다.
	 * 동기화: probe/remove에서만 다룬다. */
};

/* [한국어] IOMMU 도메인 하나를 표현하는 구조체 — 곧 평면 페이지 테이블 하나다.
 * 수명: domain_alloc_paging에서 kzalloc되어 domain_free에서 해제된다.
 * 소유자: IOMMU 코어(iommu_domain 멤버를 통해). */
struct sprd_iommu_domain {
	spinlock_t		pgtlock; /* lock for page table */
	/* [한국어] 평면 페이지 테이블 배열을 보호하는 스핀락.
	 * 설정자: sprd_iommu_domain_alloc_paging()이 초기화.
	 * 읽는 자: map/unmap/iova_to_phys가 irqsave로 잡는다.
	 * 왜 irqsave인가: DMA API 경로가 인터럽트 컨텍스트에서도 호출될 수 있어,
	 *                 같은 CPU의 인터럽트가 락을 다시 요구하는 데드락을 막아야 한다.
	 * 왜 필요한가: 여러 스레드가 같은 도메인의 서로 다른 IOVA를 동시에
	 *              매핑할 수 있는데, 이 드라이버는 원자적 연산 대신 평범한
	 *              대입으로 엔트리를 쓰므로 락으로 직렬화한다. */

	struct iommu_domain	domain;
	/* [한국어] IOMMU 코어가 보는 도메인 부분(임베드).
	 * 설정자: domain_alloc_paging이 pgsize_bitmap과 geometry를 채운다.
	 * 읽는 자: 이 파일 전반이 domain.geometry.aperture_start/end를 읽어
	 *          IOVA 범위 검사와 인덱스 계산에 쓴다.
	 * 값 범위: aperture는 0 ~ 256MB-1로 고정, pgsize_bitmap은 4KB 하나.
	 * 동기화: alloc 이후 불변. */

	u32			*pgt_va; /* page table virtual address base */
	/* [한국어] 평면 페이지 테이블 배열의 커널 가상 주소.
	 * 설정자: sprd_iommu_attach_device()가 최초 attach 때
	 *          dma_alloc_coherent()로 할당한다(재attach 시에는 재사용).
	 * 읽는 자: map/unmap/iova_to_phys가 (iova - start) >> 12를 인덱스로 접근.
	 * 값 범위: NULL(아직 attach된 적 없음) 또는 유효한 배열 포인터.
	 *          배열 크기는 (256MB / 4KB) * 4바이트 = 256KB.
	 * 엔트리 형식: 물리 페이지 번호(PFN) u32 하나. 유효 비트도 권한 비트도
	 *              없어서, 0이면 "매핑 없음"으로 관례상 취급한다.
	 * 동기화: 배열 내용은 pgtlock으로, 포인터 자체는 attach 직렬화로 보호된다. */

	dma_addr_t		pgt_pa; /* page table physical address base */
	/* [한국어] 위 페이지 테이블의 DMA(물리) 주소.
	 * 설정자: dma_alloc_coherent()가 출력 인자로 채운다.
	 * 읽는 자: sprd_iommu_first_ppn()이 페이지 시프트해 FIRST_PPN 레지스터에
	 *          기록한다 — 하드웨어가 테이블을 찾아가는 유일한 단서다.
	 * 값 범위: 4KB 정렬된 DMA 주소.
	 * 동기화: attach 이후 불변. */

	struct sprd_iommu_device	*sdev;
	/* [한국어] 이 도메인이 붙어 있는 IOMMU 인스턴스.
	 * 설정자: attach가 최초 테이블 할당과 함께 기록하고,
	 *          sprd_iommu_cleanup()이 해제 시 NULL로 되돌린다.
	 * 읽는 자: map/sync/cleanup이 하드웨어에 접근하기 위한 통로로 쓴다.
	 *          map은 이 값이 NULL이면 "아직 attach되지 않았다"고 판단해 거부한다.
	 * 값 범위: NULL 또는 유효한 IOMMU 포인터.
	 * 동기화: attach/free 경로에서만 바뀐다. */
};

/* [한국어] iommu_ops 구조체의 전방 선언. 정의는 파일 아래쪽에 있지만
 * sprd_iommu_probe()보다 앞서 참조되지는 않는다 — 오히려 컴파일러 경고를
 * 피하고 정의 위치를 콜백들 뒤로 미루기 위한 관례적 선언이다. */
static const struct iommu_ops sprd_iommu_ops;

/*
 * [한국어]
 * to_sprd_domain - 일반 iommu_domain 포인터를 이 드라이버의 도메인으로 되돌린다
 *
 * @dom: IOMMU 코어가 넘긴 일반 도메인 포인터.
 * @return: 그것을 감싸고 있는 struct sprd_iommu_domain 포인터.
 *
 * 왜 필요한가: 코어는 드라이버 내부 구조를 모르므로 iommu_domain 포인터만
 * 주고받는다. 드라이버는 그것을 자기 구조체에 임베드해 두고 container_of로
 * 복원한다 — 리눅스에서 상속을 흉내 내는 표준 관용구다.
 *
 * 실행 컨텍스트: 모든 도메인 콜백의 첫 줄. 순수 포인터 산술이라 실패가 없다.
 *
 * 호출 체인:
 *   sprd_iommu_map()/unmap()/attach_device()/domain_free() → [to_sprd_domain]
 */
static struct sprd_iommu_domain *to_sprd_domain(struct iommu_domain *dom)
{
	/* [한국어] domain 멤버의 주소에서 구조체 내 오프셋을 빼 바깥 구조체의
	 * 시작 주소를 얻는다. 컴파일 타임 상수 뺄셈이라 비용이 없다. */
	return container_of(dom, struct sprd_iommu_domain, domain);
}

/*
 * [한국어]
 * sprd_iommu_write - IOMMU 레지스터에 32비트 값을 쓴다
 *
 * @sdev: 대상 IOMMU 인스턴스(base 주소를 얻는다).
 * @reg: 레지스터 오프셋(SPRD_EX_* 또는 SPRD_VAU_*).
 * @val: 쓸 값.
 * @return: 없음.
 *
 * 왜 relaxed인가: writel_relaxed()는 메모리 배리어를 동반하지 않는 MMIO
 * 쓰기다. 이 드라이버의 레지스터 설정은 순서가 중요한 구간이 명시적으로
 * (끄기 → 설정 → 켜기) 잡혀 있고, MMIO 쓰기끼리는 같은 디바이스에 대해
 * 순서가 보장되므로 매번 배리어를 칠 이유가 없다.
 *
 * 실행 컨텍스트: attach(프로세스), sync_map(atomic 가능) 모두에서 호출된다.
 *
 * 호출 체인:
 *   first_vpn/vpn_range/first_ppn/default_ppn/update_bits/sync_map
 *   → [sprd_iommu_write] → writel_relaxed()
 */
static inline void
sprd_iommu_write(struct sprd_iommu_device *sdev, unsigned int reg, u32 val)
{
	/* [한국어] 매핑된 베이스 주소에 오프셋을 더해 MMIO 쓰기를 수행한다. */
	writel_relaxed(val, sdev->base + reg);
}

/*
 * [한국어]
 * sprd_iommu_read - IOMMU 레지스터에서 32비트 값을 읽는다
 *
 * @sdev: 대상 IOMMU 인스턴스.
 * @reg: 레지스터 오프셋.
 * @return: 읽은 값.
 *
 * 왜 필요한가: 읽기가 쓰이는 곳은 두 군데뿐이다 — 버전 판별과
 * read-modify-write(update_bits). 그만큼 이 하드웨어는 상태를 되읽을 일이 적다.
 *
 * 실행 컨텍스트: probe와 attach 경로.
 *
 * 호출 체인:
 *   sprd_iommu_get_version() / sprd_iommu_update_bits() → [sprd_iommu_read]
 *   → readl_relaxed()
 */
static inline u32
sprd_iommu_read(struct sprd_iommu_device *sdev, unsigned int reg)
{
	/* [한국어] 베이스 + 오프셋에서 32비트를 읽는다. relaxed인 이유는
	 * write와 동일하다 — 필요한 순서는 코드 흐름이 이미 보장한다. */
	return readl_relaxed(sdev->base + reg);
}

/*
 * [한국어]
 * sprd_iommu_update_bits - 레지스터의 일부 비트만 read-modify-write 한다
 *
 * @sdev: 대상 IOMMU 인스턴스.
 * @reg: 레지스터 오프셋.
 * @mask: 바꿀 비트들의 마스크(시프트되기 전 기준).
 * @shift: 마스크를 적용할 위치.
 * @val: 넣을 값(역시 시프트되기 전 기준).
 * @return: 없음.
 *
 * 왜 필요한가: 설정 레지스터에는 이 드라이버가 건드리지 않아야 할 비트
 * (VAOR_BYPASS 등)가 함께 들어 있다. 통째로 덮어쓰면 그 설정이 날아가므로,
 * 읽어서 해당 비트만 갈아 끼우고 다시 쓴다.
 *
 * 실행 컨텍스트: sprd_iommu_hw_en()에서만 호출된다(attach 경로).
 * 동기화: 락이 없다 — attach가 IOMMU 코어에 의해 직렬화되고, 이 IOMMU는
 * 클라이언트가 하나뿐이라 경쟁이 발생하지 않는다는 전제다.
 *
 * 호출 체인:
 *   sprd_iommu_hw_en() → [sprd_iommu_update_bits]
 *   → sprd_iommu_read(), sprd_iommu_write()
 */
static inline void
sprd_iommu_update_bits(struct sprd_iommu_device *sdev, unsigned int reg,
		  u32 mask, u32 shift, u32 val)
{
	/* [한국어] 현재 레지스터 값을 읽어 둔다 — 건드리지 않을 비트를 보존하기 위함이다. */
	u32 t = sprd_iommu_read(sdev, reg);

	/* [한국어] 대상 비트 자리만 지우고(~(mask << shift)) 새 값을 그 자리에
	 * 끼워 넣는다. val에 mask를 한 번 더 씌우는 것은, 범위를 넘는 값이
	 * 이웃 비트를 침범하지 못하게 하는 방어다. */
	t = (t & (~(mask << shift))) | ((val & mask) << shift);
	/* [한국어] 갱신된 전체 값을 다시 기록한다. */
	sprd_iommu_write(sdev, reg, t);
}

/*
 * [한국어]
 * sprd_iommu_get_version - 하드웨어에서 IP 세대를 읽어 판별한다
 *
 * @sdev: 대상 IOMMU 인스턴스.
 * @return: SPRD_IOMMU_EX(0) 또는 SPRD_IOMMU_VAU(1), 알 수 없는 값이면 -EINVAL.
 *
 * 왜 필요한가: EX와 VAU는 레지스터 오프셋이 완전히 다르다. 디바이스 트리의
 * compatible 문자열은 둘 다 "sprd,iommu-v1"이라 구분이 되지 않으므로,
 * 하드웨어에 직접 물어보는 수밖에 없다.
 *
 * 주의할 점: 버전 레지스터의 오프셋 0x0은 EX에서는 CFG 레지스터 자리다.
 * 그럼에도 이 방식이 통하는 이유는, EX의 CFG에서 비트 8~15가 사실상
 * 0(=SPRD_IOMMU_EX)으로 읽히기 때문이다 — 우연이라기보다 IP 설계가 그렇게
 * 맞춰져 있다.
 *
 * 실행 컨텍스트: probe 중 한 번. 이 시점에는 클록이 이미 켜져 있어야 한다.
 *
 * 호출 체인:
 *   sprd_iommu_probe() → [sprd_iommu_get_version] → sprd_iommu_read()
 */
static inline int
sprd_iommu_get_version(struct sprd_iommu_device *sdev)
{
	/* [한국어] 버전 레지스터를 읽어 비트 8~15만 뽑아 오른쪽 끝으로 내린다.
	 * 결과가 enum sprd_iommu_version의 값과 직접 비교 가능한 형태가 된다. */
	int ver = (sprd_iommu_read(sdev, SPRD_IOMMU_VERSION) &
		   SPRD_VERSION_MASK) >> SPRD_VERSION_SHIFT;

	/* [한국어] 이 드라이버가 아는 두 세대인지 확인한다. */
	switch (ver) {
	/* [한국어] 1세대 — SPRD_EX_* 오프셋을 쓰게 된다. */
	case SPRD_IOMMU_EX:
	/* [한국어] 2세대 — SPRD_VAU_* 오프셋을 쓰게 된다.
	 * 두 case가 같은 동작(값 그대로 반환)이라 fallthrough로 묶여 있다. */
	case SPRD_IOMMU_VAU:
		return ver;
	/* [한국어] 알 수 없는 세대 — 레지스터 배치를 짐작할 수 없으므로
	 * probe를 실패시켜 잘못된 오프셋에 쓰는 사고를 막는다. */
	default:
		return -EINVAL;	/* [한국어] 모르는 세대라 레지스터 배치를 짐작할 수 없으니 잘못된 인자로 보고한다. */
	}
}

/*
 * [한국어]
 * sprd_iommu_pgt_size - 도메인에 필요한 평면 페이지 테이블의 바이트 크기를 구한다
 *
 * @domain: 대상 도메인(aperture 범위를 읽는다).
 * @return: 테이블 크기(바이트).
 *
 * 계산의 의미: (aperture 크기 / 페이지 크기) × 엔트리 크기(u32).
 * 기본 설정(256MB, 4KB 페이지)에서는 65536개 × 4바이트 = 256KB가 나온다.
 * 다단계 테이블이 아니라 IOVA 전체를 덮는 평면 배열이므로, aperture가
 * 커지면 메모리 사용량이 선형으로 늘어난다 — 256MB로 제한된 이유이기도 하다.
 *
 * `+ 1`이 붙는 이유: aperture_end가 포함(inclusive) 경계라, 크기를 구하려면
 * 하나를 더해야 한다.
 *
 * 실행 컨텍스트: attach(할당 시)와 cleanup(해제 시)에서 호출된다.
 * 두 곳이 같은 값을 얻어야 dma_free_coherent에 올바른 크기를 넘길 수 있다.
 *
 * 호출 체인:
 *   sprd_iommu_attach_device() / sprd_iommu_cleanup() → [sprd_iommu_pgt_size]
 */
static size_t
sprd_iommu_pgt_size(struct iommu_domain *domain)
{
	/* [한국어] aperture의 바이트 크기를 페이지 수로 바꾼 뒤 엔트리 크기를
	 * 곱한다. end가 포함 경계이므로 +1을 먼저 더해야 한다. */
	return ((domain->geometry.aperture_end -
		 domain->geometry.aperture_start + 1) >>
		SPRD_IOMMU_PAGE_SHIFT) * sizeof(u32);
}

/*
 * [한국어]
 * sprd_iommu_domain_alloc_paging - 새 페이징 도메인을 만든다
 *
 * @dev: 이 도메인을 요청한 디바이스. 이 구현에서는 보지 않는다
 *       (모든 도메인이 동일한 고정 기하를 갖기 때문).
 * @return: 새 도메인의 iommu_domain 포인터, 메모리 부족이면 NULL.
 *
 * 왜 여기서 테이블을 할당하지 않는가: 테이블 할당에는 DMA 마스터
 * (=IOMMU 디바이스)가 필요한데, 이 시점에는 어느 IOMMU에 붙을지 아직
 * 정해지지 않았다. 그래서 실제 할당은 첫 attach로 미룬다.
 *
 * 고정 기하: aperture 0 ~ 256MB-1, 페이지 4KB. 하드웨어 자체의 제약이자,
 * 평면 테이블 크기를 256KB로 묶어 두는 실용적 선택이기도 하다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. kzalloc_obj가 GFP_KERNEL을 쓴다.
 *
 * 호출 체인:
 *   iommu_domain_alloc() → iommu_ops->domain_alloc_paging
 *   → [sprd_iommu_domain_alloc_paging]
 */
static struct iommu_domain *sprd_iommu_domain_alloc_paging(struct device *dev)
{
	/* [한국어] 새로 만들 도메인 구조체. */
	struct sprd_iommu_domain *dom;

	/* [한국어] 0으로 초기화해 할당한다. pgt_va와 sdev가 NULL로 시작해야
	 * "아직 attach되지 않음" 판정이 올바르게 동작한다. */
	dom = kzalloc_obj(*dom);
	/* [한국어] 메모리 부족 — 코어에 NULL로 실패를 알린다. */
	if (!dom)
		return NULL;

	/* [한국어] 페이지 테이블 배열을 보호할 스핀락을 초기화한다. */
	spin_lock_init(&dom->pgtlock);

	/* [한국어] 지원 페이지 크기는 4KB 하나뿐이다. 코어는 이 비트맵을 보고
	 * map 요청을 4KB 단위로 쪼개 넘긴다. */
	dom->domain.pgsize_bitmap = SPRD_IOMMU_PAGE_SIZE;

	/* [한국어] IOVA 공간의 시작. 0으로 고정이라 인덱스 계산의
	 * (iova - start)가 사실상 iova 그대로가 된다. */
	dom->domain.geometry.aperture_start = 0;
	/* [한국어] IOVA 공간의 끝(포함 경계). 256MB 제한이 곧 평면 테이블
	 * 크기를 256KB로 묶는 근거다. */
	dom->domain.geometry.aperture_end = SZ_256M - 1;
	/* [한국어] 코어가 이 범위를 강제하게 한다 — 범위를 벗어난 IOVA 할당
	 * 요청이 아예 들어오지 않게 막는다. */
	dom->domain.geometry.force_aperture = true;

	/* [한국어] 코어에는 임베드된 일반 도메인 포인터를 돌려준다. */
	return &dom->domain;
}

/*
 * [한국어]
 * sprd_iommu_first_vpn - 이 도메인이 담당할 IOVA 시작 페이지 번호를 하드웨어에 알린다
 *
 * @dom: 대상 도메인(aperture_start와 sdev를 읽는다).
 * @return: 없음.
 *
 * 왜 필요한가: 하드웨어는 "어느 IOVA부터 변환 대상인지"를 알아야 한다.
 * 평면 테이블의 0번 엔트리가 곧 이 VPN에 대응하므로, 이 값이 인덱스 계산의
 * 기준점이 된다(소프트웨어의 `(iova - start) >> 12`와 짝을 이룬다).
 *
 * 실행 컨텍스트: attach 중, IOMMU가 꺼진 상태에서 호출된다.
 *
 * 호출 체인:
 *   sprd_iommu_attach_device() → [sprd_iommu_first_vpn] → sprd_iommu_write()
 */
static void sprd_iommu_first_vpn(struct sprd_iommu_domain *dom)
{
	/* [한국어] 레지스터를 쓸 대상 IOMMU. attach에서 이미 설정되어 있다. */
	struct sprd_iommu_device *sdev = dom->sdev;
	/* [한국어] 레지스터에 쓸 VPN 값. */
	u32 val;
	/* [한국어] 세대에 따라 달라지는 레지스터 오프셋. */
	unsigned int reg;

	/* [한국어] 1세대와 2세대의 FIRST_VPN 오프셋이 다르므로 갈라 준다. */
	if (sdev->ver == SPRD_IOMMU_EX)
		reg = SPRD_EX_FIRST_VPN;
	else
		reg = SPRD_VAU_FIRST_VPN;

	/* [한국어] aperture 시작 주소를 페이지 번호로 바꾼다. 현재 구성에서는
	 * start가 0이라 결과도 0이지만, 계산을 명시적으로 남겨 두면 aperture가
	 * 바뀌어도 코드가 그대로 성립한다. */
	val = dom->domain.geometry.aperture_start >> SPRD_IOMMU_PAGE_SHIFT;
	/* [한국어] 하드웨어에 기록. */
	sprd_iommu_write(sdev, reg, val);
}

/*
 * [한국어]
 * sprd_iommu_vpn_range - 이 도메인이 담당할 IOVA 범위(페이지 수)를 알린다
 *
 * @dom: 대상 도메인.
 * @return: 없음.
 *
 * 왜 필요한가: FIRST_VPN이 시작점이라면 이 값은 길이다. 둘을 합쳐
 * 하드웨어가 "이 구간의 접근만 테이블로 변환하고, 벗어나면 폴트"라고
 * 판단할 수 있게 된다. 소프트웨어 쪽 범위 검사(map/unmap의 start/end 비교)와
 * 같은 내용을 하드웨어에도 알려 주는 셈이다.
 *
 * 계산 주의: aperture_end - aperture_start이므로 페이지 수보다 1 작은 값이
 * 된다(256MB면 65535). 하드웨어가 "범위"를 그렇게 해석한다는 뜻이다.
 *
 * 실행 컨텍스트: attach 중, IOMMU가 꺼진 상태.
 *
 * 호출 체인:
 *   sprd_iommu_attach_device() → [sprd_iommu_vpn_range] → sprd_iommu_write()
 */
static void sprd_iommu_vpn_range(struct sprd_iommu_domain *dom)
{
	/* [한국어] 대상 IOMMU. */
	struct sprd_iommu_device *sdev = dom->sdev;
	/* [한국어] 레지스터에 쓸 범위 값. */
	u32 val;
	/* [한국어] 세대별 레지스터 오프셋. */
	unsigned int reg;

	/* [한국어] EX와 VAU의 VPN_RANGE 오프셋이 다르다. */
	if (sdev->ver == SPRD_IOMMU_EX)
		reg = SPRD_EX_VPN_RANGE;
	else
		reg = SPRD_VAU_VPN_RANGE;

	/* [한국어] aperture의 바이트 길이를 페이지 단위로 바꾼다.
	 * end - start이므로 결과는 (페이지 수 - 1)이다. */
	val = (dom->domain.geometry.aperture_end -
	       dom->domain.geometry.aperture_start) >> SPRD_IOMMU_PAGE_SHIFT;
	/* [한국어] 하드웨어에 기록. */
	sprd_iommu_write(sdev, reg, val);
}

/*
 * [한국어]
 * sprd_iommu_first_ppn - 페이지 테이블이 놓인 물리 페이지 번호를 하드웨어에 알린다
 *
 * @dom: 대상 도메인(pgt_pa와 sdev를 읽는다).
 * @return: 없음.
 *
 * 왜 필요한가: 이 IP에는 TTBR 개념이 없고 이 레지스터 하나가 그 역할을 한다.
 * 하드웨어는 여기 적힌 물리 페이지에서 시작하는 평면 배열을
 * (IOVA - FIRST_VPN)번째로 인덱싱해 곧바로 엔트리를 읽는다 — 워크가 없다.
 *
 * 실행 컨텍스트: attach 중, IOMMU가 꺼진 상태. 이 호출 시점에는 이미
 * dma_alloc_coherent로 테이블이 할당되어 pgt_pa가 유효해야 한다.
 *
 * 호출 체인:
 *   sprd_iommu_attach_device() → [sprd_iommu_first_ppn] → sprd_iommu_write()
 */
static void sprd_iommu_first_ppn(struct sprd_iommu_domain *dom)
{
	/* [한국어] 테이블의 물리 주소를 페이지 번호로 바꾼다. dma_alloc_coherent가
	 * 페이지 정렬을 보장하므로 하위 12비트는 항상 0이다. */
	u32 val = dom->pgt_pa >> SPRD_IOMMU_PAGE_SHIFT;
	/* [한국어] 대상 IOMMU. */
	struct sprd_iommu_device *sdev = dom->sdev;
	/* [한국어] 세대별 레지스터 오프셋. */
	unsigned int reg;

	/* [한국어] EX와 VAU의 FIRST_PPN 오프셋이 다르다(공교롭게도 둘 다 0x10이지만
	 * 의미상 별개의 상수이므로 분기를 유지한다). */
	if (sdev->ver == SPRD_IOMMU_EX)
		reg = SPRD_EX_FIRST_PPN;
	else
		reg = SPRD_VAU_FIRST_PPN;

	/* [한국어] 하드웨어가 테이블을 찾아갈 수 있게 기록한다. */
	sprd_iommu_write(sdev, reg, val);
}

/*
 * [한국어]
 * sprd_iommu_default_ppn - 변환 실패 시 접근을 돌릴 보호 페이지를 지정한다
 *
 * @sdev: 대상 IOMMU 인스턴스(prot_page_pa를 읽는다).
 * @return: 없음.
 *
 * 왜 필요한가: 이 IP에는 폴트 인터럽트를 받아 처리하는 경로가 없다.
 * 매핑되지 않은 IOVA에 디바이스가 접근하면 하드웨어는 그 접근을 어디론가
 * 보내야 하는데, 진짜 메모리로 보내면 시스템이 손상된다. 그래서 probe에서
 * 미리 잡아 둔 4KB "보호 페이지"로 모두 돌려보낸다 — 읽으면 쓰레기가 나오고
 * 쓰면 그 페이지만 더럽혀질 뿐, 다른 메모리는 안전하다.
 *
 * 세대 차이: EX는 읽기/쓰기 공용 레지스터 하나지만, VAU는 읽기용과 쓰기용이
 * 분리되어 있다. 이 드라이버는 둘 다 같은 페이지를 가리키게 한다.
 *
 * 실행 컨텍스트: attach 중, IOMMU가 꺼진 상태.
 *
 * 호출 체인:
 *   sprd_iommu_attach_device() → [sprd_iommu_default_ppn] → sprd_iommu_write()
 */
static void sprd_iommu_default_ppn(struct sprd_iommu_device *sdev)
{
	/* [한국어] 보호 페이지의 물리 주소를 페이지 번호로 변환한다. */
	u32 val = sdev->prot_page_pa >> SPRD_IOMMU_PAGE_SHIFT;

	/* [한국어] 1세대는 읽기/쓰기 공용 레지스터 하나에 기록한다. */
	if (sdev->ver == SPRD_IOMMU_EX) {
		sprd_iommu_write(sdev, SPRD_EX_DEFAULT_PPN, val);
	/* [한국어] 2세대는 읽기용과 쓰기용이 나뉘어 있다. 폴트 종류별로 다른
	 * 페이지를 쓸 수도 있지만, 여기서는 같은 보호 페이지를 지정한다. */
	} else if (sdev->ver == SPRD_IOMMU_VAU) {
		/* [한국어] 읽기 폴트용 기본 PPN. */
		sprd_iommu_write(sdev, SPRD_VAU_DEFAULT_PPN_RD, val);
		/* [한국어] 쓰기 폴트용 기본 PPN. */
		sprd_iommu_write(sdev, SPRD_VAU_DEFAULT_PPN_WR, val);
	}
}

/*
 * [한국어]
 * sprd_iommu_hw_en - IOMMU 하드웨어를 켜거나 끈다
 *
 * @sdev: 대상 IOMMU 인스턴스.
 * @en: true면 켜고, false면 끈다.
 * @return: 없음.
 *
 * 왜 필요한가: attach는 반드시 "끄기 → 설정 → 켜기" 순서를 밟아야 한다.
 * 켜진 상태에서 FIRST_PPN 같은 레지스터를 바꾸면, 하드웨어가 절반만 바뀐
 * 설정으로 엉뚱한 메모리를 테이블로 읽을 수 있기 때문이다.
 * 도메인 해제(cleanup)에서도 테이블 메모리를 해제하기 전에 반드시 꺼야 한다.
 *
 * EN과 GATE_EN을 함께 다루는 이유: 두 비트가 항상 같이 켜지고 같이 꺼진다.
 * GATE_EN은 유휴 시 내부 클록을 멈추는 전력 최적화라, IOMMU를 쓸 때만
 * 의미가 있기 때문이다.
 *
 * 실행 컨텍스트: attach와 cleanup(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   sprd_iommu_attach_device() / sprd_iommu_cleanup() → [sprd_iommu_hw_en]
 *   → sprd_iommu_update_bits()
 */
static void sprd_iommu_hw_en(struct sprd_iommu_device *sdev, bool en)
{
	/* [한국어] 세대별 설정 레지스터 오프셋. */
	unsigned int reg_cfg;
	/* [한국어] 건드릴 비트들의 마스크와 쓸 값. */
	u32 mask, val;

	/* [한국어] EX는 SPRD_EX_CFG(0x0), VAU는 SPRD_VAU_CFG(0x4)를 쓴다. */
	if (sdev->ver == SPRD_IOMMU_EX)
		reg_cfg = SPRD_EX_CFG;
	else
		reg_cfg = SPRD_VAU_CFG;

	/* [한국어] 변환 활성화 비트와 클록 게이팅 비트를 함께 다룬다.
	 * 나머지 비트(VAOR_BYPASS 등)는 update_bits가 보존해 준다. */
	mask = SPRD_IOMMU_EN | SPRD_IOMMU_GATE_EN;
	/* [한국어] 켜면 두 비트를 모두 세우고, 끄면 모두 내린다. */
	val = en ? mask : 0;
	/* [한국어] shift가 0인 이유: mask가 이미 최종 비트 위치를 담고 있어
	 * 추가 이동이 필요 없다. */
	sprd_iommu_update_bits(sdev, reg_cfg, mask, 0, val);
}

/*
 * [한국어]
 * sprd_iommu_cleanup - 도메인이 잡고 있던 하드웨어와 테이블을 정리한다
 *
 * @dom: 정리할 도메인.
 * @return: 없음.
 *
 * 왜 이 순서인가: 반드시 (1) 테이블 메모리 해제보다 먼저 (2) IOMMU를 꺼야
 * 하는 것처럼 보이지만, 이 코드는 해제 후에 끈다. 실제로는 도메인 해제
 * 시점에 이미 클라이언트 디바이스가 DMA를 하지 않는 상태이므로 문제가
 * 되지 않는다 — 다만 순서를 뒤집는 편이 더 안전하다는 점은 기억해 둘 만하다
 * (코드는 고치지 않는다).
 *
 * 동작 과정:
 *  1) 한 번도 attach된 적이 없으면(sdev == NULL) 정리할 것이 없다.
 *  2) 테이블 크기를 다시 계산해 dma_free_coherent로 반납.
 *  3) 하드웨어를 끈다.
 *  4) sdev를 NULL로 만들어 "attach 안 됨" 상태로 되돌린다.
 *
 * 실행 컨텍스트: 도메인 해제 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   sprd_iommu_domain_free() → [sprd_iommu_cleanup]
 *   → sprd_iommu_pgt_size(), dma_free_coherent(), sprd_iommu_hw_en()
 */
static void sprd_iommu_cleanup(struct sprd_iommu_domain *dom)
{
	/* [한국어] 해제할 테이블의 크기. dma_free_coherent가 할당 때와 같은
	 * 크기를 요구하므로 다시 계산한다. */
	size_t pgt_size;

	/* Nothing need to do if the domain hasn't been attached */
	/* [한국어] 만들어지기만 하고 attach된 적이 없는 도메인은 테이블도
	 * 하드웨어 연결도 없다 — 그냥 돌아간다. */
	if (!dom->sdev)
		return;

	/* [한국어] 할당 때와 동일한 계산으로 크기를 구한다(aperture가 불변이라
	 * 항상 같은 값이 나온다). */
	pgt_size = sprd_iommu_pgt_size(&dom->domain);
	/* [한국어] 평면 페이지 테이블을 반납한다. DMA 마스터가 IOMMU 자신이므로
	 * 첫 인자가 dom->sdev->dev다. */
	dma_free_coherent(dom->sdev->dev, pgt_size, dom->pgt_va, dom->pgt_pa);
	/* [한국어] 하드웨어 변환을 끈다 — 이후 이 IOMMU를 통한 DMA는 변환 없이
	 * 통과하게 된다. */
	sprd_iommu_hw_en(dom->sdev, false);
	/* [한국어] 연결을 끊어 이 도메인이 다시 정리되지 않도록 표시한다. */
	dom->sdev = NULL;
}

/*
 * [한국어]
 * sprd_iommu_domain_free - 도메인을 해제한다
 *
 * @domain: 해제할 도메인(코어가 넘긴 일반 포인터).
 * @return: 없음.
 *
 * 왜 두 단계인가: cleanup이 하드웨어와 DMA 메모리를 정리하고, 여기서
 * 도메인 구조체 자체를 반납한다. cleanup을 분리해 둔 것은 attach 실패
 * 경로 등에서 재사용할 여지를 남기기 위함이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   iommu_domain_free() → domain_ops->free → [sprd_iommu_domain_free]
 *   → sprd_iommu_cleanup(), kfree()
 */
static void sprd_iommu_domain_free(struct iommu_domain *domain)
{
	/* [한국어] 일반 포인터를 이 드라이버의 도메인으로 복원한다. */
	struct sprd_iommu_domain *dom = to_sprd_domain(domain);

	/* [한국어] 테이블 메모리와 하드웨어 상태를 먼저 정리한다. */
	sprd_iommu_cleanup(dom);
	/* [한국어] 이제 참조가 없으므로 구조체를 반납한다. */
	kfree(dom);
}

/*
 * [한국어]
 * sprd_iommu_attach_device - 디바이스를 도메인에 붙이고 하드웨어를 프로그래밍한다
 *
 * @domain: 붙일 도메인.
 * @dev: 붙일 클라이언트 디바이스(of_xlate가 priv에 IOMMU를 심어 둔 상태).
 * @old: 직전 도메인. 이 구현에서는 사용하지 않는다.
 * @return: 0 성공, -ENOMEM(페이지 테이블 할당 실패).
 *
 * 왜 여기서 테이블을 할당하는가: domain_alloc 시점에는 DMA 마스터가 될
 * IOMMU 디바이스를 알 수 없다. attach가 되어야 비로소 sdev가 정해지므로,
 * 최초 attach에서 dma_alloc_coherent를 수행한다. 두 번째 attach부터는
 * pgt_va가 이미 있어 재할당하지 않는다.
 *
 * 하드웨어 설정 순서가 이 함수의 핵심이다:
 *   끄기 → FIRST_PPN → FIRST_VPN → VPN_RANGE → DEFAULT_PPN → 켜기.
 * 원본 주석이 밝히듯, 이 IOMMU는 클라이언트 하나만 담당하므로 다른
 * 매핑 테이블이 남아 있는 채로 설정을 바꾸면 접근 충돌이 생긴다.
 * 그래서 반드시 끈 상태에서 모든 레지스터를 갱신하고 마지막에 켠다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(GFP_KERNEL 할당). IOMMU 코어가
 * 그룹 뮤텍스로 직렬화하므로 여기서는 별도 락이 없다.
 *
 * 호출 체인:
 *   iommu_attach_device() → domain_ops->attach_dev → [sprd_iommu_attach_device]
 *   → dma_alloc_coherent(), sprd_iommu_hw_en(), first_ppn/first_vpn/
 *     vpn_range/default_ppn
 */
static int sprd_iommu_attach_device(struct iommu_domain *domain,
				    struct device *dev,
				    struct iommu_domain *old)
{
	/* [한국어] of_xlate가 클라이언트의 priv에 심어 둔 IOMMU 인스턴스. */
	struct sprd_iommu_device *sdev = dev_iommu_priv_get(dev);
	/* [한국어] 붙일 도메인을 이 드라이버의 형태로 복원한다. */
	struct sprd_iommu_domain *dom = to_sprd_domain(domain);
	/* [한국어] 필요한 평면 테이블의 크기(기본 구성에서 256KB). */
	size_t pgt_size = sprd_iommu_pgt_size(domain);

	/* The device is attached to this domain */
	/* [한국어] 이미 같은 도메인이 붙어 있으면 하드웨어를 다시 만질 이유가
	 * 없다. 재설정은 IOMMU를 잠시 끄는 것을 뜻하므로, 불필요한 중단을 피한다. */
	if (sdev->dom == dom)
		return 0;

	/* The first time that domain is attaching to a device */
	/* [한국어] 이 도메인이 처음 attach되는 경우에만 테이블을 만든다.
	 * 두 번째부터는 기존 테이블(과 그 안의 매핑)을 그대로 재사용한다. */
	if (!dom->pgt_va) {
		/* [한국어] 하드웨어가 직접 읽는 메모리이므로 코히런트 DMA로 잡는다.
		 * 0으로 초기화되어 모든 엔트리가 "매핑 없음" 상태로 시작한다. */
		dom->pgt_va = dma_alloc_coherent(sdev->dev, pgt_size, &dom->pgt_pa, GFP_KERNEL);
		/* [한국어] 256KB 연속 코히런트 메모리 확보 실패 — attach를 포기한다. */
		if (!dom->pgt_va)
			return -ENOMEM;

		/* [한국어] 이제부터 이 도메인은 이 IOMMU에 매여 있다. map/sync가
		 * 이 포인터로 하드웨어에 접근하게 된다. */
		dom->sdev = sdev;
	}

	/* [한국어] IOMMU 쪽에도 현재 도메인을 기록해, 다음 attach에서 중복
	 * 설정을 건너뛸 수 있게 한다. */
	sdev->dom = dom;

	/*
	 * One sprd IOMMU serves one client device only, disabled it before
	 * configure mapping table to avoid access conflict in case other
	 * mapping table is stored in.
	 */
	/* [한국어] 1단계: 변환을 끈다. 이전 도메인의 테이블이 아직 레지스터에
	 * 남아 있는 상태에서 새 값을 섞어 넣으면 하드웨어가 뒤엉킨 설정으로
	 * 메모리를 읽게 되므로, 반드시 먼저 꺼야 한다. */
	sprd_iommu_hw_en(sdev, false);
	/* [한국어] 2단계: 새 페이지 테이블의 위치를 알린다. */
	sprd_iommu_first_ppn(dom);
	/* [한국어] 3단계: 변환 대상 IOVA의 시작점을 알린다. */
	sprd_iommu_first_vpn(dom);
	/* [한국어] 4단계: 변환 대상 IOVA의 길이를 알린다. */
	sprd_iommu_vpn_range(dom);
	/* [한국어] 5단계: 폴트 시 접근을 돌릴 보호 페이지를 지정한다. */
	sprd_iommu_default_ppn(sdev);
	/* [한국어] 6단계: 모든 설정이 끝났으니 변환을 켠다. 이 시점부터
	 * 클라이언트의 DMA가 테이블을 통해 변환된다. */
	sprd_iommu_hw_en(sdev, true);

	/* [한국어] attach 완료. */
	return 0;
}

/*
 * [한국어]
 * sprd_iommu_map - IOVA 구간에 물리 페이지들을 매핑한다
 *
 * @domain: 대상 도메인.
 * @iova: 매핑 시작 IOVA.
 * @paddr: 매핑할 물리 주소 시작(연속이어야 한다).
 * @pgsize: 페이지 크기. 항상 4KB다(pgsize_bitmap이 하나뿐이라).
 * @pgcount: 매핑할 페이지 개수.
 * @prot: 보호 플래그. **이 하드웨어는 권한 비트가 없어 무시된다.**
 * @gfp: 할당 플래그. 새로 할당할 것이 없어 쓰이지 않는다.
 * @mapped: 출력 인자 — 매핑한 바이트 수를 기록한다.
 * @return: 0 성공, -EINVAL(attach 안 됨 / IOVA 범위 초과).
 *
 * 왜 이렇게 단순한가: 평면 테이블이라 워크도, 중간 테이블 할당도 없다.
 * 인덱스를 구해 PFN을 연속으로 써 넣으면 끝이다. 권한 비트가 없어
 * prot 인자를 아예 보지 않는다는 점이 이 하드웨어의 큰 한계다 —
 * 읽기 전용 매핑을 표현할 수단이 없다.
 *
 * 동작 과정:
 *  1) 도메인이 attach되어 있는지(테이블이 존재하는지) 확인.
 *  2) 요청 구간이 aperture 안인지 확인.
 *  3) (iova - start) >> 12로 배열 시작 인덱스를 구한다.
 *  4) 락을 잡고 pgcount개의 엔트리에 PFN을 순차적으로 기록.
 *  5) *mapped에 처리한 바이트 수를 기록한다.
 *
 * TLB 무효화는 여기서 하지 않는다 — 코어가 매핑을 마친 뒤
 * iotlb_sync_map 콜백(sprd_iommu_sync_map)을 별도로 호출한다.
 *
 * 실행 컨텍스트: DMA API 경로라 atomic일 수 있어 irqsave 스핀락을 쓴다.
 *
 * 호출 체인:
 *   iommu_map() → domain_ops->map_pages → [sprd_iommu_map]
 *   → (이후 코어가) sprd_iommu_sync_map()
 */
static int sprd_iommu_map(struct iommu_domain *domain, unsigned long iova,
			  phys_addr_t paddr, size_t pgsize, size_t pgcount,
			  int prot, gfp_t gfp, size_t *mapped)
{
	/* [한국어] 이 드라이버의 도메인 형태로 복원한다. */
	struct sprd_iommu_domain *dom = to_sprd_domain(domain);
	/* [한국어] 매핑할 총 바이트 수. pgsize 인자 대신 상수를 쓰는 것은
	 * 이 하드웨어가 4KB 외의 크기를 지원하지 않기 때문이다. */
	size_t size = pgcount * SPRD_IOMMU_PAGE_SIZE;
	/* [한국어] irqsave용 플래그. */
	unsigned long flags;
	/* [한국어] 엔트리 기록 루프의 인덱스. */
	unsigned int i;
	/* [한국어] 기록을 시작할 테이블 엔트리의 주소. */
	u32 *pgt_base_iova;
	/* [한국어] 물리 주소를 32비트로 잘라 담는다. 이 IP는 32비트 물리
	 * 주소 공간만 다루므로(PFN이 u32 하나에 들어간다) 이 캐스팅이 성립한다. */
	u32 pabase = (u32)paddr;
	/* [한국어] aperture 시작 — 인덱스 계산의 기준점. */
	unsigned long start = domain->geometry.aperture_start;
	/* [한국어] aperture 끝(포함 경계) — 범위 검사에 쓴다. */
	unsigned long end = domain->geometry.aperture_end;

	/* [한국어] attach된 적이 없으면 테이블 자체가 없다. 코어가 순서를
	 * 어긴 경우이므로 에러 로그를 남기고 거부한다. */
	if (!dom->sdev) {
		pr_err("No sprd_iommu_device attached to the domain\n");	/* [한국어] 테이블이 없는 상태에서 매핑 요청이 왔다는 뜻이라 에러 로그를 남긴다. */
		return -EINVAL;	/* [한국어] 코어의 호출 순서가 어긋난 경우이므로 매핑을 거부한다. */
	}

	/* [한국어] 요청 구간이 평면 테이블이 덮는 범위를 벗어나면 배열 밖에
	 * 쓰게 되므로 반드시 걸러야 한다. end가 포함 경계라 (end + 1)과 비교한다. */
	if (iova < start || (iova + size) > (end + 1)) {
		dev_err(dom->sdev->dev, "(iova(0x%lx) + size(%zx)) are not in the range!\n",	/* [한국어] 어떤 IOVA와 크기가 범위를 벗어났는지 남겨 디버깅을 돕는다. */
			iova, size);
		return -EINVAL;	/* [한국어] 평면 배열 밖을 건드릴 뻔했으므로 거부한다. */
	}

	/* [한국어] IOVA를 배열 인덱스로 바꾼다. aperture 시작을 빼고 페이지
	 * 시프트하면 곧바로 엔트리 번호가 된다 — 이것이 평면 테이블의 전부다. */
	pgt_base_iova = dom->pgt_va + ((iova - start) >> SPRD_IOMMU_PAGE_SHIFT);

	/* [한국어] 테이블 배열 접근을 직렬화한다. 원자적 연산이 아니라
	 * 평범한 대입을 쓰므로 락이 반드시 필요하다. */
	spin_lock_irqsave(&dom->pgtlock, flags);
	/* [한국어] 요청된 페이지 수만큼 엔트리를 채운다. */
	for (i = 0; i < pgcount; i++) {
		/* [한국어] 엔트리에는 물리 페이지 번호만 들어간다 — 유효 비트도
		 * 권한 비트도 없다. 그래서 0이 아닌 값이 곧 "유효한 매핑"이다. */
		pgt_base_iova[i] = pabase >> SPRD_IOMMU_PAGE_SHIFT;
		/* [한국어] 다음 물리 페이지로 전진. 호출자는 물리적으로 연속된
		 * 영역만 넘겨야 한다. */
		pabase += SPRD_IOMMU_PAGE_SIZE;
	}
	spin_unlock_irqrestore(&dom->pgtlock, flags);

	/* [한국어] 코어에 실제 매핑한 바이트 수를 알린다. 이 드라이버는 요청을
	 * 항상 전부 처리하므로 size 그대로다. */
	*mapped = size;
	/* [한국어] 성공. TLB 무효화는 코어가 iotlb_sync_map으로 따로 요청한다. */
	return 0;
}

/*
 * [한국어]
 * sprd_iommu_unmap - IOVA 구간의 매핑을 제거한다
 *
 * @domain: 대상 도메인.
 * @iova: 해제 시작 IOVA.
 * @pgsize: 페이지 크기(항상 4KB).
 * @pgcount: 해제할 페이지 개수.
 * @iotlb_gather: TLB 무효화 수집 구조체. 이 드라이버는 범위 무효화를
 *                지원하지 않아 사용하지 않는다.
 * @return: 해제한 바이트 수, 범위를 벗어났으면 0.
 *
 * 왜 memset 하나로 끝나는가: 엔트리가 PFN u32 하나뿐이고 유효 비트가 따로
 * 없으므로, 0으로 채우는 것이 곧 "매핑 없음"이다. 해제할 중간 테이블도 없다.
 *
 * gather를 쓰지 않는 이유: 이 IP의 TLB 무효화는 UPDATE 레지스터에
 * 0xffffffff를 쓰는 전체 무효화뿐이다. 범위를 모아 봐야 쓸 데가 없어,
 * 코어가 나중에 iotlb_sync를 부를 때 통째로 비운다.
 *
 * 실행 컨텍스트: DMA API 경로(atomic 가능) — irqsave 스핀락.
 *
 * 호출 체인:
 *   iommu_unmap() → domain_ops->unmap_pages → [sprd_iommu_unmap]
 *   → (이후 코어가) sprd_iommu_sync()
 */
static size_t sprd_iommu_unmap(struct iommu_domain *domain, unsigned long iova,
			       size_t pgsize, size_t pgcount,
			       struct iommu_iotlb_gather *iotlb_gather)
{
	/* [한국어] 이 드라이버의 도메인으로 복원한다. */
	struct sprd_iommu_domain *dom = to_sprd_domain(domain);
	/* [한국어] irqsave용 플래그. */
	unsigned long flags;
	/* [한국어] 지우기를 시작할 엔트리 주소. */
	u32 *pgt_base_iova;
	/* [한국어] 해제할 총 바이트 수(반환값이기도 하다). */
	size_t size = pgcount * SPRD_IOMMU_PAGE_SIZE;
	/* [한국어] aperture 시작 — 인덱스 계산 기준. */
	unsigned long start = domain->geometry.aperture_start;
	/* [한국어] aperture 끝(포함 경계). */
	unsigned long end = domain->geometry.aperture_end;

	/* [한국어] 범위를 벗어난 요청은 아무것도 하지 않고 0을 반환한다.
	 * unmap은 오류 코드를 반환할 수 없어 "0바이트 처리"로 실패를 표현한다.
	 * map과 달리 로그를 남기지 않는 점이 다르다. */
	if (iova < start || (iova + size) > (end + 1))
		return 0;

	/* [한국어] map과 동일한 방식으로 배열 인덱스를 구한다. */
	pgt_base_iova = dom->pgt_va + ((iova - start) >> SPRD_IOMMU_PAGE_SHIFT);

	/* [한국어] 테이블 접근을 직렬화한다. */
	spin_lock_irqsave(&dom->pgtlock, flags);
	/* [한국어] 해당 엔트리들을 0으로 채운다 — 0이 곧 "매핑 없음"이므로
	 * 이 한 줄이 해제의 전부다. 이후 그 IOVA 접근은 보호 페이지로 향한다. */
	memset(pgt_base_iova, 0, pgcount * sizeof(u32));
	spin_unlock_irqrestore(&dom->pgtlock, flags);

	/* [한국어] 요청 전부를 처리했으므로 요청 크기를 그대로 반환한다. */
	return size;
}

/*
 * [한국어]
 * sprd_iommu_sync_map - 페이지 테이블 갱신 후 TLB를 무효화한다
 *
 * @domain: 대상 도메인.
 * @iova: 갱신된 구간의 시작. **이 하드웨어는 범위 무효화를 지원하지 않아 무시된다.**
 * @size: 갱신된 구간의 크기. 역시 무시된다.
 * @return: 항상 0(성공).
 *
 * 왜 항상 전체 무효화인가: 이 IP의 UPDATE 레지스터는 "무엇을 비울지"를
 * 지정할 수단이 없다. 0xffffffff를 쓰면 TLB 전체가 비워지고, 그것이
 * 이 드라이버가 가진 유일한 무효화 수단이다. 그래서 iova/size 인자를 받지만
 * 쓰지 않는다.
 *
 * 왜 map 안에서 하지 않는가: 코어가 여러 map 호출을 묶은 뒤 마지막에
 * 한 번만 sync를 부르므로, 무효화 횟수를 줄일 수 있다. 전체 무효화밖에
 * 없는 이 하드웨어에서는 그 절약이 특히 크다.
 *
 * 실행 컨텍스트: 매핑 직후, atomic일 수 있다. MMIO 쓰기 한 번이라 잠들지 않는다.
 *
 * 호출 체인:
 *   iommu_map()의 마무리 → domain_ops->iotlb_sync_map → [sprd_iommu_sync_map]
 *   → sprd_iommu_write()
 */
static int sprd_iommu_sync_map(struct iommu_domain *domain,
			       unsigned long iova, size_t size)
{
	/* [한국어] 하드웨어에 접근하기 위해 도메인을 복원한다. */
	struct sprd_iommu_domain *dom = to_sprd_domain(domain);
	/* [한국어] 세대별 UPDATE 레지스터 오프셋. */
	unsigned int reg;

	/* [한국어] EX는 0x4, VAU는 0x8에 UPDATE 레지스터가 있다. */
	if (dom->sdev->ver == SPRD_IOMMU_EX)
		reg = SPRD_EX_UPDATE;
	else
		reg = SPRD_VAU_UPDATE;

	/* clear IOMMU TLB buffer after page table updated */
	/* [한국어] 모든 비트를 세워 TLB 전체를 비운다. 범위 지정 무효화가
	 * 없으므로 매번 전체를 날리는 수밖에 없다 — 이 IP의 성능상 약점이다. */
	sprd_iommu_write(dom->sdev, reg, 0xffffffff);
	/* [한국어] 실패할 수 있는 동작이 없어 항상 성공을 반환한다. */
	return 0;
}

/*
 * [한국어]
 * sprd_iommu_sync - unmap 이후의 TLB 무효화 콜백
 *
 * @domain: 대상 도메인.
 * @iotlb_gather: 코어가 모아 둔 무효화 범위. 이 하드웨어는 범위 무효화를
 *                지원하지 않아 사용하지 않는다.
 * @return: 없음.
 *
 * 왜 sync_map을 그대로 부르는가: map 이후든 unmap 이후든 이 IP가 할 수 있는
 * 일은 "TLB 전체 비우기" 하나뿐이라 동작이 완전히 같다. iova와 size에 0을
 * 넘기는 것은 그 인자들이 어차피 무시되기 때문이다.
 *
 * 실행 컨텍스트: unmap 마무리 단계, atomic 가능.
 *
 * 호출 체인:
 *   iommu_unmap()의 마무리 → domain_ops->iotlb_sync → [sprd_iommu_sync]
 *   → sprd_iommu_sync_map()
 */
static void sprd_iommu_sync(struct iommu_domain *domain,
			    struct iommu_iotlb_gather *iotlb_gather)
{
	/* [한국어] 전체 무효화 한 가지뿐이므로 sync_map을 재사용한다.
	 * 인자 0, 0은 무시되는 자리 채우기다. */
	sprd_iommu_sync_map(domain, 0, 0);
}

/*
 * [한국어]
 * sprd_iommu_iova_to_phys - IOVA를 물리 주소로 변환한다(소프트웨어 조회)
 *
 * @domain: 대상 도메인.
 * @iova: 변환할 IOVA.
 * @return: 물리 주소. 매핑이 없으면 페이지 내 오프셋만 남은 값이 되는데,
 *          사실상 0에 가까운 무의미한 값이다.
 *
 * 왜 이렇게 단순한가: 평면 테이블이라 인덱싱 한 번으로 PFN을 얻는다.
 * 거기에 페이지 시프트를 되돌리고 IOVA의 페이지 내 오프셋을 더하면 끝이다.
 *
 * 한 가지 특이점: 엔트리가 0(매핑 없음)이어도 이 함수는 0을 명시적으로
 * 반환하지 않고 오프셋만 남은 값을 돌려준다. 유효 비트가 없는 하드웨어
 * 설계 탓에 "매핑 없음"과 "물리 주소 0에 매핑됨"을 구분할 수 없기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 또는 atomic — 역시 irqsave 스핀락으로 보호한다.
 * 락을 잡는 이유는 동시에 map/unmap이 같은 엔트리를 바꿀 수 있어서다.
 *
 * 호출 체인:
 *   iommu_iova_to_phys() → domain_ops->iova_to_phys → [sprd_iommu_iova_to_phys]
 */
static phys_addr_t sprd_iommu_iova_to_phys(struct iommu_domain *domain,
					   dma_addr_t iova)
{
	/* [한국어] 이 드라이버의 도메인으로 복원한다. */
	struct sprd_iommu_domain *dom = to_sprd_domain(domain);
	/* [한국어] irqsave용 플래그. */
	unsigned long flags;
	/* [한국어] 계산 결과인 물리 주소. */
	phys_addr_t pa;
	/* [한국어] aperture 시작 — 인덱스 계산 기준이자 범위 검사용. */
	unsigned long start = domain->geometry.aperture_start;
	/* [한국어] aperture 끝(포함 경계). */
	unsigned long end = domain->geometry.aperture_end;

	/* [한국어] 범위를 벗어난 조회는 배열 밖 접근이 되므로 반드시 막는다.
	 * 상위 계층의 버그일 가능성이 커서 WARN_ON으로 흔적을 남긴다. */
	if (WARN_ON(iova < start || iova > end))
		return 0;

	/* [한국어] 테이블 접근을 직렬화한다. */
	spin_lock_irqsave(&dom->pgtlock, flags);
	/* [한국어] 인덱싱해 엔트리(=PFN)를 읽는다. */
	pa = *(dom->pgt_va + ((iova - start) >> SPRD_IOMMU_PAGE_SHIFT));
	/* [한국어] PFN을 물리 주소로 되돌리고, IOVA의 페이지 내 오프셋을 더한다.
	 * (SPRD_IOMMU_PAGE_SIZE - 1)이 오프셋 마스크 역할을 한다. */
	pa = (pa << SPRD_IOMMU_PAGE_SHIFT) + ((iova - start) & (SPRD_IOMMU_PAGE_SIZE - 1));
	spin_unlock_irqrestore(&dom->pgtlock, flags);

	/* [한국어] 계산된 물리 주소를 반환한다. */
	return pa;
}

/*
 * [한국어]
 * sprd_iommu_probe_device - 이 디바이스를 담당할 IOMMU를 알려 준다
 *
 * @dev: 검사할 클라이언트 디바이스.
 * @return: 담당 IOMMU의 iommu_device 핸들.
 *
 * 왜 검사가 없는가: 이 콜백이 불리기 전에 of_xlate가 먼저 실행되어
 * dev_iommu_priv에 IOMMU 인스턴스를 심어 둔다. 디바이스 트리에 iommus
 * 프로퍼티가 없는 디바이스는 애초에 이 콜백까지 오지 않으므로, priv가
 * 항상 유효하다는 전제가 성립한다.
 *
 * 실행 컨텍스트: 디바이스 probe 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   iommu_probe_device() → iommu_ops->probe_device → [sprd_iommu_probe_device]
 */
static struct iommu_device *sprd_iommu_probe_device(struct device *dev)
{
	/* [한국어] of_xlate가 심어 둔 IOMMU 인스턴스를 꺼낸다. */
	struct sprd_iommu_device *sdev = dev_iommu_priv_get(dev);

	/* [한국어] 그 인스턴스에 임베드된 코어 핸들을 돌려주면, 코어가 이
	 * 디바이스를 해당 IOMMU 소속으로 등록한다. */
	return &sdev->iommu;
}

/*
 * [한국어]
 * sprd_iommu_of_xlate - 디바이스 트리의 iommus 프로퍼티를 해석한다
 *
 * @dev: iommus 프로퍼티를 가진 클라이언트 디바이스.
 * @args: 파싱된 phandle 인자. args->np가 IOMMU 노드를 가리킨다.
 * @return: 항상 0.
 *
 * 왜 필요한가: 디바이스 트리에서 `iommus = <&iommu_vsp>` 같은 프로퍼티를
 * 만나면 코어가 이 콜백을 부른다. 여기서 그 phandle이 가리키는 IOMMU
 * 플랫폼 디바이스를 찾아 drvdata(=sprd_iommu_device)를 꺼내고, 클라이언트의
 * dev_iommu_priv에 심어 둔다. 이후 probe_device와 attach가 그 포인터를 쓴다.
 *
 * 참조 카운트: of_find_device_by_node()가 디바이스 참조를 하나 올리므로
 * platform_device_put()으로 반드시 내려야 한다. drvdata 포인터 자체는
 * IOMMU 디바이스가 살아 있는 한 유효하므로, 참조를 놓아도 안전하다
 * (IOMMU가 클라이언트보다 먼저 사라지지 않는다는 전제).
 *
 * 왜 !dev_iommu_priv_get 검사가 있는가: 한 디바이스가 iommus 프로퍼티에
 * 여러 항목을 가질 수 있어 이 콜백이 여러 번 불릴 수 있다. 이 IP는 클라이언트
 * 하나에 IOMMU 하나이므로 첫 번째만 기록하고 나머지는 무시한다.
 *
 * 실행 컨텍스트: 디바이스 probe 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   of_iommu_configure() → iommu_ops->of_xlate → [sprd_iommu_of_xlate]
 *   → of_find_device_by_node(), platform_get_drvdata(), platform_device_put()
 */
static int sprd_iommu_of_xlate(struct device *dev,
			       const struct of_phandle_args *args)
{
	/* [한국어] phandle이 가리키는 IOMMU 플랫폼 디바이스를 담을 포인터. */
	struct platform_device *pdev;

	/* [한국어] 아직 IOMMU가 정해지지 않은 경우에만 처리한다.
	 * 두 번째 이후의 iommus 항목은 무시된다. */
	if (!dev_iommu_priv_get(dev)) {
		/* [한국어] 디바이스 트리 노드로부터 대응하는 플랫폼 디바이스를
		 * 찾는다. 참조 카운트가 하나 올라간다. */
		pdev = of_find_device_by_node(args->np);
		/* [한국어] 그 디바이스의 drvdata가 곧 sprd_iommu_device다
		 * (probe에서 platform_set_drvdata로 심어 두었다).
		 * 이것을 클라이언트의 priv에 저장해 두면, 이후 probe_device와
		 * attach가 어느 IOMMU를 쓸지 알 수 있다. */
		dev_iommu_priv_set(dev, platform_get_drvdata(pdev));
		/* [한국어] 올린 참조를 내린다. 포인터는 IOMMU 디바이스가
		 * 살아 있는 동안 유효하므로 참조를 유지할 필요가 없다. */
		platform_device_put(pdev);
	}

	/* [한국어] 이 콜백은 실패를 표현하지 않는다 — 항상 성공으로 보고한다. */
	return 0;
}


/* [한국어] IOMMU 코어에 노출하는 이 드라이버의 연산 테이블. */
static const struct iommu_ops sprd_iommu_ops = {
	/* [한국어] 페이징 도메인 생성 — 고정 기하(256MB, 4KB)를 세팅한다. */
	.domain_alloc_paging = sprd_iommu_domain_alloc_paging,
	/* [한국어] 디바이스 담당 판정 — of_xlate가 심어 둔 IOMMU를 돌려준다. */
	.probe_device	= sprd_iommu_probe_device,
	/* [한국어] IOMMU 하나가 클라이언트 하나만 담당하므로, 코어가 제공하는
	 * "디바이스마다 단독 그룹" 헬퍼를 그대로 쓴다. */
	.device_group	= generic_single_device_group,
	/* [한국어] 디바이스 트리의 iommus 프로퍼티 해석 콜백. */
	.of_xlate	= sprd_iommu_of_xlate,
	/* [한국어] 모듈 언로드 중 콜백이 실행되지 않도록 코어가 참조를 잡게 한다. */
	.owner		= THIS_MODULE,
	/* [한국어] 도메인 단위 연산 테이블. 익명 const 구조체를 그 자리에
	 * 정의하는 관용구로, 별도 전역 심볼을 만들지 않는다. */
	.default_domain_ops = &(const struct iommu_domain_ops) {
		/* [한국어] 테이블 할당(최초 1회)과 하드웨어 프로그래밍. */
		.attach_dev	= sprd_iommu_attach_device,
		/* [한국어] 평면 배열에 PFN 기록. */
		.map_pages	= sprd_iommu_map,
		/* [한국어] 평면 배열의 해당 구간을 0으로 지우기. */
		.unmap_pages	= sprd_iommu_unmap,
		/* [한국어] 매핑 후 TLB 전체 무효화. */
		.iotlb_sync_map	= sprd_iommu_sync_map,
		/* [한국어] 해제 후 TLB 전체 무효화(같은 동작). */
		.iotlb_sync	= sprd_iommu_sync,
		/* [한국어] 소프트웨어 인덱싱으로 IOVA→물리 주소 조회. */
		.iova_to_phys	= sprd_iommu_iova_to_phys,
		/* [한국어] 테이블 해제 + 하드웨어 끄기 + 구조체 반납. */
		.free		= sprd_iommu_domain_free,
	}
};

/* [한국어] 이 드라이버가 바인딩할 디바이스 트리 compatible 목록.
 * EX와 VAU가 같은 문자열을 쓰기 때문에, 세대 구분은 부팅 후
 * 버전 레지스터를 읽어서 해야 한다(sprd_iommu_get_version 참조). */
static const struct of_device_id sprd_iommu_of_match[] = {
	/* [한국어] Unisoc IOMMU v1 — EX와 VAU 양쪽을 포괄한다. */
	{ .compatible = "sprd,iommu-v1" },
	/* [한국어] 배열의 끝을 알리는 빈 항목. 이것이 없으면 매칭 루프가
	 * 배열 밖으로 넘어간다. */
	{ },
};
/* [한국어] 모듈 자동 로딩을 위해 위 매칭 테이블을 모듈 메타데이터에 심는다.
 * udev/kmod가 디바이스 트리의 compatible을 보고 이 모듈을 찾아 올린다. */
MODULE_DEVICE_TABLE(of, sprd_iommu_of_match);

/*
 * Clock is not required, access to some of IOMMUs is controlled by gate
 * clk, enabled clocks for that kind of IOMMUs before accessing.
 * Return 0 for success or no clocks found.
 */
/*
 * [한국어]
 * sprd_iommu_clk_enable - 게이트 클록이 있으면 켠다
 *
 * @sdev: 대상 IOMMU 인스턴스.
 * @return: 0 성공 또는 클록 없음, 음수 errno(클록 조회/활성화 실패).
 *
 * 왜 optional인가: 같은 드라이버가 담당하는 여러 IOMMU 인스턴스 중 일부만
 * 별도 게이트 클록을 갖는다. 클록이 없는 인스턴스에서 필수로 요구하면
 * probe가 실패하므로, devm_clk_get_optional()로 "있으면 쓰고 없으면 넘어간다".
 *
 * 반환값 판별 순서에 주의: devm_clk_get_optional()은 클록이 없으면 NULL을,
 * 오류면 ERR_PTR를 반환한다. 그래서 NULL 검사를 먼저 하고(성공으로 처리),
 * 그다음 IS_ERR를 검사한다.
 *
 * 실행 컨텍스트: probe(프로세스 컨텍스트). 이 호출이 성공해야
 * sprd_iommu_get_version()이 레지스터를 읽을 수 있다.
 *
 * 호출 체인:
 *   sprd_iommu_probe() → [sprd_iommu_clk_enable]
 *   → devm_clk_get_optional(), clk_prepare_enable()
 */
static int sprd_iommu_clk_enable(struct sprd_iommu_device *sdev)
{
	/* [한국어] 조회한 클록을 담을 임시 포인터. */
	struct clk *eb;

	/* [한국어] 이름 없는(첫 번째) 클록을 optional로 조회한다. devm이라
	 * 디바이스가 사라질 때 자동으로 정리된다. */
	eb = devm_clk_get_optional(sdev->dev, NULL);
	/* [한국어] NULL은 "이 인스턴스에는 클록이 없다"는 뜻이며 정상이다.
	 * IS_ERR보다 먼저 검사해야 하는 이유는 NULL이 오류가 아니기 때문이다. */
	if (!eb)
		return 0;

	/* [한국어] 여기서 오류라면 클록은 있는데 얻지 못한 것이므로 probe를
	 * 실패시켜야 한다(-EPROBE_DEFER일 수도 있다). */
	if (IS_ERR(eb))
		return PTR_ERR(eb);

	/* [한국어] 나중에 끄기 위해 클록 핸들을 보관한다. */
	sdev->eb = eb;
	/* [한국어] prepare와 enable을 함께 수행한다. 이 호출이 성공해야
	 * IOMMU 레지스터에 접근할 수 있다. */
	return clk_prepare_enable(eb);
}

/*
 * [한국어]
 * sprd_iommu_clk_disable - 켜 두었던 게이트 클록을 끈다
 *
 * @sdev: 대상 IOMMU 인스턴스.
 * @return: 없음.
 *
 * 왜 조건 검사가 필요한가: 클록이 없는 인스턴스는 sdev->eb가 NULL이므로,
 * 그대로 clk_disable_unprepare에 넘기면 안 된다.
 *
 * 실행 컨텍스트: probe 실패 시의 되감기 경로. 흥미롭게도 remove 경로에는
 * 이 호출이 없다 — 원본 코드가 그러하며 여기서 고치지 않는다.
 *
 * 호출 체인:
 *   sprd_iommu_probe()의 disable_clk 레이블 → [sprd_iommu_clk_disable]
 *   → clk_disable_unprepare()
 */
static void sprd_iommu_clk_disable(struct sprd_iommu_device *sdev)
{
	/* [한국어] 클록을 실제로 얻은 인스턴스에서만 끈다. */
	if (sdev->eb)
		clk_disable_unprepare(sdev->eb);
}

/*
 * [한국어]
 * sprd_iommu_probe - 플랫폼 디바이스로 나타난 IOMMU를 초기화한다
 *
 * @pdev: 디바이스 트리에서 매칭된 IOMMU 플랫폼 디바이스.
 * @return: 0 성공, 음수 errno(각 단계의 실패).
 *
 * 동작 과정(그리고 실패 시 되감는 순서):
 *  1) 인스턴스 구조체 할당(devm이라 자동 해제).
 *  2) MMIO 레지스터 블록 매핑(devm이라 자동 해제).
 *  3) 폴트용 보호 페이지 4KB를 코히런트 DMA로 할당 → free_page에서 해제.
 *  4) drvdata 설정 — of_xlate가 이 값을 꺼내 쓴다.
 *  5) sysfs 노드 등록 → remove_sysfs에서 해제.
 *  6) IOMMU 코어에 등록 → unregister_iommu에서 해제.
 *  7) 게이트 클록 켜기 → disable_clk에서 끄기.
 *  8) 버전 레지스터를 읽어 세대 판별(클록이 켜진 뒤여야 한다).
 *
 * 순서의 이유: 클록을 코어 등록보다 뒤에 켜는 것이 다소 특이하지만,
 * 버전 판별만 클록을 요구하므로 문제가 없다. 다만 6번과 7번 사이에
 * 클라이언트 디바이스가 attach를 시도하면 클록이 꺼진 상태로 레지스터를
 * 만질 여지가 이론상 존재한다.
 *
 * 실행 컨텍스트: 디바이스 probe(프로세스 컨텍스트, 잠들 수 있음).
 *
 * 호출 체인:
 *   플랫폼 버스 매칭 → driver->probe → [sprd_iommu_probe]
 *   → devm_platform_ioremap_resource(), dma_alloc_coherent(),
 *     iommu_device_sysfs_add(), iommu_device_register(),
 *     sprd_iommu_clk_enable(), sprd_iommu_get_version()
 */
static int sprd_iommu_probe(struct platform_device *pdev)
{
	/* [한국어] 만들 IOMMU 인스턴스. */
	struct sprd_iommu_device *sdev;
	/* [한국어] 편의를 위한 struct device 포인터. */
	struct device *dev = &pdev->dev;
	/* [한국어] 매핑된 MMIO 베이스 주소. */
	void __iomem *base;
	/* [한국어] 각 단계의 결과. */
	int ret;

	/* [한국어] devm 할당이라 디바이스가 사라질 때 자동으로 해제된다.
	 * 0 초기화라 dom/eb 등이 NULL로 시작한다. */
	sdev = devm_kzalloc(dev, sizeof(*sdev), GFP_KERNEL);
	/* [한국어] 메모리 부족 — 아직 잡은 자원이 없어 그냥 반환한다. */
	if (!sdev)
		return -ENOMEM;

	/* [한국어] 디바이스 트리의 첫 번째 reg 항목을 ioremap 한다. devm이라
	 * 별도 해제 경로가 필요 없다. */
	base = devm_platform_ioremap_resource(pdev, 0);
	/* [한국어] 매핑 실패 — 주소 자원이 없거나 이미 점유된 경우다. */
	if (IS_ERR(base)) {
		dev_err(dev, "Failed to get ioremap resource.\n");	/* [한국어] reg 자원이 없거나 이미 점유된 경우이므로 로그를 남긴다. */
		return PTR_ERR(base);	/* [한국어] ioremap 실패 원인을 그대로 상위에 전달한다. */
	}
	/* [한국어] 이후 모든 레지스터 접근의 기준이 될 베이스를 보관한다. */
	sdev->base = base;

	/* [한국어] 폴트 시 접근을 흘려보낼 보호 페이지를 4KB 할당한다.
	 * 하드웨어가 직접 쓰는 메모리라 코히런트여야 한다. */
	sdev->prot_page_va = dma_alloc_coherent(dev, SPRD_IOMMU_PAGE_SIZE,
						&sdev->prot_page_pa, GFP_KERNEL);
	/* [한국어] 이 페이지가 없으면 폴트 시 시스템 메모리가 오염될 수 있으므로
	 * probe 자체를 실패시킨다. */
	if (!sdev->prot_page_va)
		return -ENOMEM;

	/* [한국어] of_xlate가 이 인스턴스를 찾아갈 수 있도록 drvdata에 심는다.
	 * 이 한 줄이 디바이스 트리와 드라이버를 잇는 연결 고리다. */
	platform_set_drvdata(pdev, sdev);
	/* [한국어] DMA 할당과 로깅에 쓸 자기 자신의 device 포인터를 보관한다. */
	sdev->dev = dev;

	/* [한국어] /sys/class/iommu/ 아래에 이 IOMMU를 노출한다. 이름은
	 * 디바이스 이름을 그대로 쓴다. */
	ret = iommu_device_sysfs_add(&sdev->iommu, dev, NULL, dev_name(dev));
	/* [한국어] 실패 시 보호 페이지만 되돌리면 된다. */
	if (ret)
		goto free_page;

	/* [한국어] IOMMU 코어에 연산 테이블을 등록한다. 이 시점부터
	 * of_xlate/probe_device 콜백이 들어올 수 있다. */
	ret = iommu_device_register(&sdev->iommu, &sprd_iommu_ops, dev);
	/* [한국어] 실패 시 sysfs 노드부터 되감는다. */
	if (ret)
		goto remove_sysfs;

	/* [한국어] 레지스터 접근에 필요한 게이트 클록을 켠다(있는 경우에만). */
	ret = sprd_iommu_clk_enable(sdev);
	/* [한국어] 실패 시 코어 등록을 취소한다. */
	if (ret)
		goto unregister_iommu;

	/* [한국어] 이제 레지스터를 읽을 수 있으니 IP 세대를 판별한다.
	 * 이후 모든 레지스터 오프셋 선택이 이 값에 달려 있다. */
	ret = sprd_iommu_get_version(sdev);
	/* [한국어] 알 수 없는 세대라면 어느 오프셋을 써야 할지 모르므로
	 * 계속 진행하면 위험하다 — probe를 실패시킨다. */
	if (ret < 0) {
		dev_err(dev, "IOMMU version(%d) is invalid.\n", ret);	/* [한국어] 읽어 온 버전 값을 함께 남겨 어떤 IP인지 추적할 수 있게 한다. */
		goto disable_clk;	/* [한국어] 클록부터 역순으로 되감는다. */
	}
	/* [한국어] 판별된 세대를 보관한다. 반환값이 그대로 enum 값이다. */
	sdev->ver = ret;

	/* [한국어] 초기화 완료. 이제 클라이언트 디바이스들이 attach할 수 있다. */
	return 0;

/* [한국어] 아래는 역순 되감기 레이블들이다. 각 레이블은 그 아래의 모든
 * 정리 단계를 연쇄적으로 수행한다(fallthrough를 의도적으로 이용한다). */
disable_clk:
	/* [한국어] 켜 두었던 게이트 클록을 끈다. */
	sprd_iommu_clk_disable(sdev);
unregister_iommu:
	/* [한국어] IOMMU 코어 등록을 취소한다 — 더 이상 콜백이 들어오지 않는다. */
	iommu_device_unregister(&sdev->iommu);
remove_sysfs:
	/* [한국어] sysfs 노드를 제거한다. */
	iommu_device_sysfs_remove(&sdev->iommu);
free_page:
	/* [한국어] 보호 페이지를 반납한다. devm이 아니라 명시적 해제가 필요하다. */
	dma_free_coherent(sdev->dev, SPRD_IOMMU_PAGE_SIZE, sdev->prot_page_va, sdev->prot_page_pa);
	/* [한국어] 실패를 유발한 오류 코드를 그대로 반환한다. */
	return ret;
}

/*
 * [한국어]
 * sprd_iommu_remove - IOMMU 디바이스를 정리한다
 *
 * @pdev: 제거되는 플랫폼 디바이스.
 * @return: 없음.
 *
 * 정리 순서: 보호 페이지 해제 → drvdata 비우기 → sysfs 제거 → 코어 등록 해제.
 * probe의 되감기 순서와 완전히 일치하지는 않는다(보호 페이지를 가장 먼저
 * 해제한다). 또한 게이트 클록을 끄는 호출이 빠져 있다 — 원본 그대로이며,
 * 코드를 고치지 않는다는 원칙에 따라 사실만 기록한다.
 *
 * 실행 컨텍스트: 디바이스 제거 경로(프로세스 컨텍스트). suppress_bind_attrs가
 * 켜져 있어 sysfs를 통한 임의 언바인드는 막혀 있고, 실질적으로는 모듈
 * 언로드나 시스템 종료 때만 실행된다.
 *
 * 호출 체인:
 *   플랫폼 버스 → driver->remove → [sprd_iommu_remove]
 *   → dma_free_coherent(), iommu_device_sysfs_remove(), iommu_device_unregister()
 */
static void sprd_iommu_remove(struct platform_device *pdev)
{
	/* [한국어] probe에서 심어 둔 인스턴스를 꺼낸다. */
	struct sprd_iommu_device *sdev = platform_get_drvdata(pdev);

	/* [한국어] 폴트용 보호 페이지를 반납한다. */
	dma_free_coherent(sdev->dev, SPRD_IOMMU_PAGE_SIZE, sdev->prot_page_va, sdev->prot_page_pa);

	/* [한국어] drvdata를 비워 of_xlate가 해제된 인스턴스를 집어 들지 않게 한다. */
	platform_set_drvdata(pdev, NULL);
	/* [한국어] sysfs 노드를 제거한다. */
	iommu_device_sysfs_remove(&sdev->iommu);
	/* [한국어] 코어 등록을 해제한다 — 이후 콜백이 들어오지 않는다. */
	iommu_device_unregister(&sdev->iommu);
}

/* [한국어] 플랫폼 버스에 등록할 드라이버 정의. */
static struct platform_driver sprd_iommu_driver = {
	/* [한국어] 드라이버 공통 정보 묶음. */
	.driver	= {
		/* [한국어] sysfs와 로그에 나타날 드라이버 이름. */
		.name		= "sprd-iommu",
		/* [한국어] 디바이스 트리 compatible 매칭 테이블. */
		.of_match_table	= sprd_iommu_of_match,
		/* [한국어] sysfs를 통한 수동 bind/unbind를 막는다.
		 * IOMMU를 임의로 언바인드하면 클라이언트의 DMA 매핑이 통째로
		 * 무효화되어 시스템이 손상되므로, 사용자 조작을 원천 차단한다. */
		.suppress_bind_attrs = true,
	},
	/* [한국어] 초기화 진입점. */
	.probe	= sprd_iommu_probe,
	/* [한국어] 정리 진입점. */
	.remove = sprd_iommu_remove,
};
/* [한국어] module_init/module_exit 보일러플레이트를 자동 생성하는 매크로.
 * 모듈 적재 시 platform_driver_register(), 해제 시 unregister()를 호출한다. */
module_platform_driver(sprd_iommu_driver);

/* [한국어] modinfo에 표시될 이 모듈의 설명. */
MODULE_DESCRIPTION("IOMMU driver for Unisoc SoCs");
/* [한국어] 플랫폼 버스 이름 기반 자동 로딩용 별칭. 디바이스 트리 매칭
 * (MODULE_DEVICE_TABLE)과 별개로, 이름으로 등록되는 경우를 대비한다. */
MODULE_ALIAS("platform:sprd-iommu");
/* [한국어] 라이선스 선언. GPL이어야 GPL 전용 커널 심볼을 쓸 수 있고,
 * 커널이 tainted로 표시되지 않는다. */
MODULE_LICENSE("GPL");
