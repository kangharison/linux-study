// SPDX-License-Identifier: GPL-2.0-only
/*
 * IOMMU API for Rockchip
 *
 * Module Authors:	Simon Xue <xxm@rock-chips.com>
 *			Daniel Kurtz <djkurtz@chromium.org>
 */

/*
 * [한국어 설명] Rockchip SoC의 IOMMU 드라이버 (rockchip-iommu.c)
 *
 * === 파일의 역할 ===
 * Rockchip RK3288 계열 SoC에 들어 있는 IOMMU 블록을 리눅스 IOMMU API에
 * 붙인다. 비디오 디코더, GPU, 디스플레이 컨트롤러 같은 멀티미디어
 * 블록들이 물리적으로 흩어진 버퍼를 연속된 주소 공간으로 보게 하는
 * 것이 목적이다.
 *
 * 구조는 단순하다 — 2단계 페이지 테이블 하나가 전부다. 그러나 그
 * 단순한 구조 위에 이 드라이버만의 사정이 넷 얹혀 있고, 그것이
 * 코드의 대부분을 차지한다.
 *
 * (1) **한 IOMMU 블록 안에 MMU 인스턴스가 여럿 있을 수 있다.**
 *     num_mmu와 bases[] 배열이 그것이다. 이들은 완전히 같은 상태를
 *     유지해야 하므로, 레지스터를 쓸 때는 항상 전부에 쓰고, 읽어서
 *     상태를 판정할 때는 전부가 만족해야 참으로 본다. 그래서
 *     is_stall_active 같은 함수가 &= 로 누적한다.
 *
 * (2) **stall 프로토콜.** 페이징이 켜진 상태에서는 다른 명령을 낼 수
 *     없다. 무효화든 리셋이든 먼저 stall을 걸어 변환을 멈춘 뒤에야
 *     명령이 받아들여진다. enable_stall → 작업 → disable_stall이
 *     이 드라이버의 관용구다. 각 단계는 폴링으로 완료를 확인한다.
 *
 * (3) **런타임 PM이 모든 하드웨어 접근을 감싼다.** 이 IOMMU는 자기
 *     마스터(비디오 디코더 등)와 함께 전원이 오간다. 그래서 무효화나
 *     인터럽트 처리조차 pm_runtime_get_if_in_use()로 "지금 켜져
 *     있는가"를 먼저 확인한다. 꺼져 있으면 아무 일도 하지 않는데,
 *     어차피 다시 켜질 때 rk_iommu_enable()이 전체를 다시 설정하기
 *     때문에 안전하다.
 *
 * (4) **v1과 v2의 레지스터 형식 차이를 함수 포인터로 흡수한다.**
 *     v2는 40비트 물리 주소를 지원하는데, 32비트 엔트리에 상위
 *     비트를 넣을 자리가 없어 예약 비트(11:4)에 나눠 담는다.
 *     rk_ops의 세 콜백이 그 차이를 감춘다.
 *
 * 또 하나 눈에 띄는 점은 **identity 도메인이 곧 "떼어 낸 상태"**
 * 라는 것이다. 이 하드웨어에는 통과 모드가 따로 없어서, identity에
 * 붙인다는 것은 그저 페이징을 끄는 일이다. 그래서 attach는 항상
 * "identity로 먼저 보내고 새 도메인을 켠다"는 순서를 밟는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *
 *   [디바이스 트리] iommus = <&vpu_mmu>
 *        ↓ of_xlate
 *   [이 파일] rk_iommudata를 마스터 디바이스에 매단다
 *        ↓ probe_device
 *   [이 파일] device_link로 마스터와 IOMMU의 전원을 묶는다
 *
 *   [마스터 드라이버] dma_map_*()
 *        ↓
 *   [dma-iommu] IOVA 할당
 *        ↓
 *   [이 파일] rk_iommu_map() → DTE 확보 → PTE 기록 → 캐시 플러시
 *        ↓
 *   [하드웨어] MMU가 DMA로 DT/PT를 읽어 변환
 *        ↑ 폴트
 *   [이 파일] rk_iommu_irq() → log_iova()로 테이블을 되짚어 진단 출력
 *
 * 실행 컨텍스트: map/unmap은 dt_lock(irqsave) 아래에서 돌고 atomic일
 * 수 있다. 인터럽트 핸들러는 당연히 인터럽트 문맥이며, 그 안에서
 * 클럭을 켜고 레지스터를 읽는다.
 *
 * === 타 모듈과의 연결 ===
 * - iommu-pages.h: 테이블 페이지 할당(iommu_alloc_pages_sz). 4KB
 *   정렬이 보장되어야 하드웨어가 읽을 수 있다.
 * - linux/dma-mapping.h: **테이블 자체를 DMA로 매핑한다.** 이
 *   하드웨어는 캐시 일관성이 없어, 테이블을 고칠 때마다
 *   dma_sync_single_for_device로 밀어내야 한다. 이것이 이 드라이버의
 *   가장 중요한 외부 의존이다.
 * - linux/pm_runtime.h: 전원 상태 확인과 디바이스 링크.
 * - linux/clk.h: aclk와 iface 두 클럭. 레지스터를 만지려면 켜야 한다.
 * - linux/iopoll.h: readx_poll_timeout — stall/paging 전환의 완료 확인.
 * 데이터 흐름: 마스터의 DMA 요청 → IOVA → DT 인덱스와 PT 인덱스로
 * 분해 → PTE의 물리 주소 → 실제 메모리.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct rk_iommu_domain: 도메인 하나. 디렉터리 테이블(dt)과 그
 *   DMA 주소, 그리고 이 도메인에 붙은 IOMMU들의 목록.
 * - struct rk_iommu: IOMMU 블록 하나. MMU 인스턴스들의 레지스터
 *   베이스 배열과 클럭, 그리고 현재 붙어 있는 도메인.
 * - struct rk_iommu_ops: v1/v2의 엔트리 형식 차이를 감추는 콜백 묶음.
 * - rk_iommu_enable()/disable(): stall을 걸고 DTE_ADDR을 설정하거나
 *   지우는 하드웨어 전환의 핵심.
 * - rk_iommu_map()/unmap(): 한 번에 하나의 DTE 아래에서만 동작한다 —
 *   pgsize_bitmap이 그것을 보장한다.
 * - log_iova(): 폴트가 났을 때 테이블을 소프트웨어로 되짚어 어느
 *   단계에서 끊겼는지 찍어 준다. 진단의 핵심이다.
 */

/* [한국어] 클럭 제어. 레지스터를 만지려면 aclk와 iface를 켜야 한다. */
#include <linux/clk.h>
/* [한국어] 컴파일러 배리어 등 도우미. */
#include <linux/compiler.h>
/* [한국어] 지연 관련 도우미. */
#include <linux/delay.h>
/* [한국어] struct device와 속성 읽기. */
#include <linux/device.h>
/* [한국어] 테이블 자체를 DMA 매핑한다 — 이 하드웨어는 캐시
 * 일관성이 없어 테이블 갱신마다 sync가 필요하다. */
#include <linux/dma-mapping.h>
/* [한국어] errno 상수. */
#include <linux/errno.h>
/* [한국어] 인터럽트 등록과 IRQF_SHARED. */
#include <linux/interrupt.h>
/* [한국어] readl/writel — MMU 레지스터 접근. */
#include <linux/io.h>
/* [한국어] IOMMU 코어 계약. */
#include <linux/iommu.h>
/* [한국어] readx_poll_timeout — stall과 paging 전환의 완료를
 * 폴링으로 확인한다. */
#include <linux/iopoll.h>
/* [한국어] 도메인에 붙은 IOMMU들의 연결 목록. */
#include <linux/list.h>
/* [한국어] 페이지 관련 상수와 phys_to_virt. */
#include <linux/mm.h>
/* [한국어] 초기화 매크로. */
#include <linux/init.h>
/* [한국어] 디바이스 트리 파싱(of_device_get_match_data). */
#include <linux/of.h>
/* [한국어] of_find_device_by_node — of_xlate가 IOMMU 디바이스를 찾을 때. */
#include <linux/of_platform.h>
/* [한국어] 플랫폼 드라이버 모델. */
#include <linux/platform_device.h>
/* [한국어] 런타임 PM. 이 드라이버의 모든 하드웨어 접근이 이것에 걸린다. */
#include <linux/pm_runtime.h>
/* [한국어] kzalloc/kfree. */
#include <linux/slab.h>
/* [한국어] 두 개의 스핀락(테이블용, 목록용). */
#include <linux/spinlock.h>
/* [한국어] str_write_read() — 폴트 로그의 방향 표시. */
#include <linux/string_choices.h>

/* [한국어] iommu_alloc_pages_sz/free_pages — 테이블 페이지 할당.
 * 크기 정렬이 보장되는 것이 요점이다. */
#include "iommu-pages.h"

/** MMU register offsets */
/* [한국어] 디렉터리 테이블의 물리 주소를 담는 레지스터.
 * 여기에 0을 쓰면 사실상 변환이 죽고, 리셋 완료 판정에도 쓰인다. */
#define RK_MMU_DTE_ADDR		0x00	/* Directory table address */
/* [한국어] 현재 상태(페이징 켜짐, stall 중, 폴트 발생 등)를 읽는 레지스터. */
#define RK_MMU_STATUS		0x04
/* [한국어] 명령을 써 넣는 레지스터. 아래 RK_MMU_CMD_ 계열 값을 쓴다. */
#define RK_MMU_COMMAND		0x08
/* [한국어] 마지막 폴트가 난 IOVA. 폴트 진단의 출발점이다. */
#define RK_MMU_PAGE_FAULT_ADDR	0x0C	/* IOVA of last page fault */
/* [한국어] IOTLB에서 한 줄(한 페이지)만 무효화하는 레지스터.
 * 여기에 IOVA를 쓰면 그 항목이 버려진다. */
#define RK_MMU_ZAP_ONE_LINE	0x10	/* Shootdown one IOTLB entry */
/* [한국어] 마스크와 무관한 원시 인터럽트 상태. */
#define RK_MMU_INT_RAWSTAT	0x14	/* IRQ status ignoring mask */
/* [한국어] 인터럽트를 확인하고 다시 무장시키는 레지스터. */
#define RK_MMU_INT_CLEAR	0x18	/* Acknowledge and re-arm irq */
/* [한국어] 인터럽트 활성화 마스크. enable에서 켜고 disable에서 끈다. */
#define RK_MMU_INT_MASK		0x1C	/* IRQ enable */
/* [한국어] 마스크가 적용된 인터럽트 상태. 핸들러가 이것을 읽는다. */
#define RK_MMU_INT_STATUS	0x20	/* IRQ status after masking */
/* [한국어] 자동 클럭 게이팅 제어. 이 드라이버는 쓰지 않는다. */
#define RK_MMU_AUTO_GATING	0x24

/* [한국어] 리셋 검사에 쓰는 마법 값.
 * 이 패턴을 DTE_ADDR에 써 보고 되읽어, 레지스터가 살아 있는지
 * 확인한다. 상위 니블만 되읽히는 것이 정상이라 마스킹된 값과
 * 비교한다. */
#define DTE_ADDR_DUMMY		0xCAFEBABE

/* [한국어] 상태 폴링의 간격(마이크로초). */
#define RK_MMU_POLL_PERIOD_US		100
/* [한국어] 강제 리셋 완료를 기다리는 최대 시간(0.1초).
 * 리셋은 다른 전환보다 훨씬 오래 걸린다. */
#define RK_MMU_FORCE_RESET_TIMEOUT_US	100000
/* [한국어] stall/paging 전환 완료를 기다리는 최대 시간(1ms). */
#define RK_MMU_POLL_TIMEOUT_US		1000

/* RK_MMU_STATUS fields */
/* [한국어] 페이징이 켜져 있다 — 변환이 실제로 동작 중이라는 뜻. */
#define RK_MMU_STATUS_PAGING_ENABLED       BIT(0)
/* [한국어] 폴트가 발생해 처리를 기다리는 중이다. */
#define RK_MMU_STATUS_PAGE_FAULT_ACTIVE    BIT(1)
/* [한국어] stall이 걸려 있다 — 이 상태에서만 다른 명령을 받는다. */
#define RK_MMU_STATUS_STALL_ACTIVE         BIT(2)
/* [한국어] 진행 중인 변환이 없다. */
#define RK_MMU_STATUS_IDLE                 BIT(3)
/* [한국어] 재생 버퍼가 비었다 — 폴트 후 재시도할 요청이 없다. */
#define RK_MMU_STATUS_REPLAY_BUFFER_EMPTY  BIT(4)
/* [한국어] 폴트를 낸 접근이 쓰기였다. 폴트 로그의 방향 판정에 쓴다. */
#define RK_MMU_STATUS_PAGE_FAULT_IS_WRITE  BIT(5)
/* [한국어] stall이 걸려 있지 않다(비트 2의 반대 표현). */
#define RK_MMU_STATUS_STALL_NOT_ACTIVE     BIT(31)

/* RK_MMU_COMMAND command values */
/* [한국어] 변환을 켠다. DTE_ADDR을 설정한 뒤 마지막에 낸다. */
#define RK_MMU_CMD_ENABLE_PAGING    0  /* Enable memory translation */
/* [한국어] 변환을 끈다. 이후 마스터의 DMA는 변환 없이 나간다. */
#define RK_MMU_CMD_DISABLE_PAGING   1  /* Disable memory translation */
/* [한국어] 변환을 멈춰 다른 명령을 받을 수 있게 한다.
 * 이 하드웨어의 가장 특징적인 제약이다 — 페이징 중에는 명령을
 * 낼 수 없다. */
#define RK_MMU_CMD_ENABLE_STALL     2  /* Stall paging to allow other cmds */
/* [한국어] stall을 풀면 페이징이 다시 살아난다. */
#define RK_MMU_CMD_DISABLE_STALL    3  /* Stop stall re-enables paging */
/* [한국어] IOTLB 전체를 버린다. 도메인을 새로 붙일 때 쓴다. */
#define RK_MMU_CMD_ZAP_CACHE        4  /* Shoot down entire IOTLB */
/* [한국어] 폴트를 처리했다고 알려 재시도를 재개시킨다. */
#define RK_MMU_CMD_PAGE_FAULT_DONE  5  /* Clear page fault */
/* [한국어] 모든 레지스터를 초기 상태로 되돌린다. 도메인을 붙일 때
 * 이전 상태의 잔재를 지우는 용도다. */
#define RK_MMU_CMD_FORCE_RESET      6  /* Reset all registers */

/* RK_MMU_INT_* register fields */
/* [한국어] 페이지 폴트 인터럽트 — 매핑이 없거나 권한이 모자란 접근. */
#define RK_MMU_IRQ_PAGE_FAULT    0x01  /* page fault */
/* [한국어] 버스 오류 — 테이블을 읽다 실패했다는 뜻으로, 보통
 * 테이블 주소가 잘못됐거나 메모리가 사라진 경우다. */
#define RK_MMU_IRQ_BUS_ERROR     0x02  /* bus read error */
/* [한국어] 이 드라이버가 다루는 인터럽트 전부.
 * INT_MASK에 이 값을 써 둘을 활성화하고, 상태에서 이 마스크를
 * 벗어난 비트가 보이면 예상 밖의 상황으로 로그를 남긴다. */
#define RK_MMU_IRQ_MASK          (RK_MMU_IRQ_PAGE_FAULT | RK_MMU_IRQ_BUS_ERROR)

/* [한국어] 디렉터리 테이블의 엔트리 수. 4바이트 × 1024 = 4KB 한 페이지. */
#define NUM_DT_ENTRIES 1024
/* [한국어] 페이지 테이블의 엔트리 수. 역시 4KB 한 페이지에 딱 맞는다. */
#define NUM_PT_ENTRIES 1024

/* [한국어] 이 IOMMU가 다루는 페이지 크기의 지수(2^12 = 4KB). */
#define SPAGE_ORDER 12
/* [한국어] 페이지 크기 자체. 테이블 할당과 무효화 단위로도 쓰인다. */
#define SPAGE_SIZE (1 << SPAGE_ORDER)

 /*
  * Support mapping any size that fits in one page table:
  *   4 KiB to 4 MiB
  */
/* [한국어] 지원하는 매핑 크기의 비트맵 — 4KB부터 4MB까지 모든
 * 2의 거듭제곱이다(비트 12부터 22까지).
 *
 * 이 값이 이 드라이버 전체를 단순하게 만든다. 최대 크기 4MB가
 * 곧 페이지 테이블 하나가 덮는 범위(1024 × 4KB)이므로,
 * 코어가 정렬을 보장하는 한 map/unmap은 **항상 하나의 DTE
 * 아래에서만** 일어난다. 그래서 map 함수에 DTE를 넘나드는
 * 루프가 없다. */
#define RK_IOMMU_PGSIZE_BITMAP 0x007ff000

/* [한국어] IOMMU 도메인 하나. 하나의 디렉터리 테이블과 그것을
 * 공유하는 IOMMU들의 묶음이다. */
struct rk_iommu_domain {
	struct list_head iommus;
	/* [한국어] 이 도메인에 붙어 있는 rk_iommu들의 목록.
	 * 설정자: attach가 꼬리에 추가, identity_attach가 제거.
	 * 읽는 자: rk_iommu_zap_iova()가 무효화를 모든 IOMMU에 뿌릴 때.
	 * 왜 여럿인가: 여러 마스터 블록이 같은 주소 공간을 공유할 수
	 *              있어, 그때 하나의 도메인에 여러 IOMMU가 붙는다.
	 * 동기화: iommus_lock. */

	u32 *dt; /* page directory table */
	/* [한국어] 1단계 디렉터리 테이블의 커널 가상 주소(1024개 DTE).
	 * 설정자: domain_alloc_paging이 4KB 페이지 하나를 잡아 채운다.
	 * 읽는 자: 모든 매핑/조회 경로의 출발점.
	 * 동기화: dt_lock. */

	dma_addr_t dt_dma;
	/* [한국어] 그 디렉터리 테이블의 **DMA 주소**.
	 * 설정자: domain_alloc_paging의 dma_map_single.
	 * 읽는 자: DTE_ADDR 레지스터에 쓸 값을 만들 때, 그리고
	 *          테이블 플러시의 대상 주소로.
	 * 왜 별도로 두는가: 하드웨어가 테이블을 DMA로 읽으므로
	 *                   가상 주소가 아니라 이 주소를 알려 줘야 한다. */

	spinlock_t iommus_lock; /* lock for iommus list */
	/* [한국어] iommus 목록을 보호하는 락.
	 * 설정자: domain_alloc_paging의 초기화.
	 * 읽는 자: attach/detach와 무효화 순회.
	 * dt_lock과 분리한 이유: 무효화 순회는 하드웨어를 만지느라
	 *                        오래 걸리는데, 그동안 매핑을 막을
	 *                        이유가 없기 때문이다. */

	spinlock_t dt_lock; /* lock for modifying page directory table */
	/* [한국어] 테이블(DT와 PT 모두)의 갱신을 보호하는 락.
	 * 설정자: domain_alloc_paging의 초기화.
	 * 읽는 자: map/unmap/iova_to_phys가 irqsave로 잡는다.
	 * 규약: __ 없는 이름이지만, rk_dte_get_page_table 등 몇몇
	 *       함수는 이 락을 이미 잡았다고 가정하고
	 *       assert_spin_locked로 확인한다. */

	struct device *dma_dev;
	/* [한국어] 테이블을 DMA 매핑할 때 기준이 되는 디바이스.
	 * 설정자: domain_alloc_paging이 첫 IOMMU의 dev를 쓴다.
	 * 왜 필요한가: dma_map_single과 dma_sync가 디바이스를 요구하고,
	 *              해제 시점에도 같은 디바이스를 써야 한다.
	 * 주의: 도메인이 여러 IOMMU에 붙어도 이 값은 처음 것으로 고정된다. */

	struct iommu_domain domain;
	/* [한국어] 코어가 보는 도메인 부분(임베드).
	 * 설정자: domain_alloc_paging이 pgsize_bitmap과 geometry를 채운다.
	 * 값 범위: 4KB~4MB 매핑, IOVA는 32비트 전체. */
};

/* list of clocks required by IOMMU */
/* [한국어] 이 하드웨어가 요구하는 클럭의 이름들.
 * aclk는 버스 클럭(테이블을 DMA로 읽을 때), iface는 레지스터
 * 인터페이스 클럭이다. 둘 다 켜야 레지스터가 응답한다.
 * 오래된 디바이스 트리에는 없을 수 있어 선택 사항으로 다룬다. */
static const char * const rk_iommu_clocks[] = {
	"aclk", "iface",	/* [한국어] 버스 클럭과 레지스터 인터페이스 클럭 — 둘 다 켜야 레지스터가 응답한다. */
};

/* [한국어] v1과 v2의 형식 차이를 감추는 콜백 묶음.
 * 하나의 드라이버가 두 세대의 하드웨어를 다루기 위한 장치다. */
struct rk_iommu_ops {
	phys_addr_t (*pt_address)(u32 dte);
	/* [한국어] 엔트리에서 물리 주소를 뽑아내는 함수.
	 * 설정자: iommu_data_ops_v1/v2가 컴파일 시 지정.
	 * 읽는 자: 조회, 해제, 폴트 진단 등 엔트리를 해석하는 모든 곳.
	 * 왜 다른가: v2는 40비트 주소를 32비트 엔트리에 담느라
	 *            상위 비트를 예약 필드에 나눠 넣기 때문이다.
	 * 주의: PTE에도 같은 함수를 쓴다 — 주소 필드의 배치가 같다. */

	u32 (*mk_dtentries)(dma_addr_t pt_dma);
	/* [한국어] 페이지 테이블의 DMA 주소에서 DTE 값을 만드는 함수.
	 * 설정자: 버전별 ops.
	 * 읽는 자: 새 페이지 테이블을 매달 때와 DTE_ADDR 레지스터에
	 *          쓸 값을 만들 때. */

	u32 (*mk_ptentries)(phys_addr_t page, int prot);
	/* [한국어] 물리 주소와 권한에서 PTE 값을 만드는 함수.
	 * 설정자: 버전별 ops.
	 * 읽는 자: rk_iommu_map_iova()가 PTE를 채울 때. */

	u64 dma_bit_mask;
	/* [한국어] 이 세대가 다룰 수 있는 물리 주소의 폭.
	 * 값 범위: v1은 32비트, v2는 40비트.
	 * 읽는 자: probe의 dma_set_mask_and_coherent — 테이블 페이지가
	 *          그 범위 안에 놓이도록 DMA 계층에 알린다. */

	gfp_t gfp_flags;
	/* [한국어] 테이블 페이지를 잡을 때 더할 할당 플래그.
	 * 값 범위: v1은 GFP_DMA32(4GB 아래에서만 잡아야 한다),
	 *          v2는 0(제약 없음).
	 * 왜 필요한가: v1은 엔트리에 32비트 주소만 담을 수 있어,
	 *              테이블 자체가 4GB 위에 놓이면 가리킬 수 없다. */
};

/* [한국어] IOMMU 블록 하나의 상태.
 * 수명: platform probe에서 devm으로 만들어져 드라이버 해제까지 산다. */
struct rk_iommu {
	struct device *dev;
	/* [한국어] 이 IOMMU의 플랫폼 디바이스.
	 * 설정자: rk_iommu_probe().
	 * 읽는 자: 로그, 런타임 PM, 클럭, DMA 매핑의 기준점. */

	void __iomem **bases;
	/* [한국어] MMU 인스턴스들의 레지스터 베이스 배열.
	 * 설정자: probe가 디바이스 트리의 reg 자원마다 ioremap 한다.
	 * 왜 배열인가: 한 IOMMU 블록이 여러 MMU를 품을 수 있고,
	 *              그것들은 같은 디렉터리 테이블을 공유하며
	 *              항상 같은 상태로 유지되어야 한다. */

	int num_mmu;
	/* [한국어] 실제로 매핑에 성공한 MMU 인스턴스의 수.
	 * 설정자: probe가 ioremap에 성공할 때마다 늘린다.
	 * 읽는 자: 레지스터를 쓰거나 상태를 판정하는 모든 루프의 상한.
	 * 값 범위: 0이면 probe가 실패한다. */

	int num_irq;
	/* [한국어] 이 블록이 내는 인터럽트의 수. 보통 MMU 수와 같다.
	 * 설정자: probe의 platform_irq_count.
	 * 읽는 자: 인터럽트 등록 루프와 shutdown의 해제 루프. */

	struct clk_bulk_data *clocks;
	/* [한국어] aclk와 iface 클럭 핸들의 묶음.
	 * 설정자: probe가 이름을 채우고 devm_clk_bulk_get으로 얻는다.
	 * 읽는 자: 레지스터를 만지기 직전마다 bulk_enable/disable.
	 * 왜 매번 켰다 끄는가: 이 IOMMU는 마스터가 쉴 때 함께 쉬는 것이
	 *                      전력 설계의 전제이기 때문이다. */

	int num_clocks;
	/* [한국어] 실제로 얻은 클럭의 수.
	 * 값 범위: 오래된 디바이스 트리에 클럭이 없으면 0이 되고,
	 *          그때는 bulk 함수들이 아무 일도 하지 않는다. */

	bool reset_disabled;
	/* [한국어] 강제 리셋을 건너뛸지 여부.
	 * 설정자: probe가 "rockchip,disable-mmu-reset" 속성으로 정한다.
	 * 왜 있는가: 일부 보드에서 리셋이 다른 블록에 부작용을 일으켜,
	 *            그것을 피해 갈 탈출구가 필요하다. */

	struct iommu_device iommu;
	/* [한국어] IOMMU 코어에 등록되는 부분(임베드).
	 * 설정자: probe의 sysfs_add와 register.
	 * 읽는 자: probe_device가 이 주소를 코어에 돌려준다. */

	struct list_head node; /* entry in rk_iommu_domain.iommus */
	/* [한국어] 도메인의 iommus 목록에 매다는 고리.
	 * 설정자: attach가 추가, identity_attach가 list_del_init로 제거.
	 * del_init을 쓰는 이유: 떼어 낸 뒤에도 목록 상태가 유효해야
	 *                       중복 제거가 안전하다. */

	struct iommu_domain *domain; /* domain to which iommu is attached */
	/* [한국어] 현재 붙어 있는 도메인.
	 * 설정자: attach 계열. probe는 identity 도메인으로 초기화한다.
	 * 읽는 자: enable이 어느 테이블을 설정할지, 인터럽트가 폴트를
	 *          어디에 보고할지, suspend/resume이 일을 할지 말지.
	 * 값 범위: rk_identity_domain이면 "떼어진 상태"를 뜻한다. */
};

/* [한국어] 마스터 디바이스 쪽에 매다는 연결 정보.
 * dev_iommu_priv에 걸려, 이 디바이스가 어느 IOMMU에 속하는지 알려 준다. */
struct rk_iommudata {
	struct device_link *link; /* runtime PM link from IOMMU to master */
	/* [한국어] IOMMU와 마스터의 전원을 묶는 링크.
	 * 설정자: probe_device의 device_link_add.
	 * 왜 필요한가: 마스터가 깨어날 때 IOMMU도 함께 깨어나야 한다.
	 *              이 링크가 그 순서를 보장한다. */

	struct rk_iommu *iommu;
	/* [한국어] 이 마스터를 담당하는 IOMMU.
	 * 설정자: of_xlate가 디바이스 트리 참조를 따라가 채운다.
	 * 읽는 자: rk_iommu_from_dev()를 통해 거의 모든 콜백이 쓴다. */
};

/* [한국어] 이 시스템에서 쓰이는 하드웨어 세대의 연산 묶음(v1 또는 v2).
 * 설정자: 첫 IOMMU의 probe가 디바이스 트리 매치 데이터로 정한다.
 * 왜 전역인가: 엔트리 형식은 SoC 전체에서 하나여야 하고, 엔트리를
 *              해석하는 함수들이 도메인이나 IOMMU를 인자로 받지
 *              않기 때문이다. 두 세대가 한 SoC에 섞이면 probe가
 *              WARN과 함께 실패한다. */
static const struct rk_iommu_ops *rk_ops;
/* [한국어] "어느 도메인에도 붙어 있지 않음"을 뜻하는 정적 도메인의
 * 전방 선언. 정의는 아래에 있고 인터럽트 핸들러가 그보다 앞에서
 * 참조한다.
 * 이 하드웨어에는 진짜 통과 모드가 없어, 이 도메인에 붙는다는 것은
 * 곧 페이징을 끄는 일이다. */
static struct iommu_domain rk_identity_domain;

/*
 * [한국어]
 * rk_table_flush - 고친 테이블 엔트리를 하드웨어가 볼 수 있게 밀어낸다
 *
 * @dom: 대상 도메인(DMA 디바이스를 안다).
 * @dma: 밀어낼 첫 엔트리의 DMA 주소.
 * @count: 엔트리 개수.
 * @return: 없음.
 *
 * 이 드라이버에서 가장 자주, 그리고 반드시 불려야 하는 함수다.
 * Rockchip의 MMU는 테이블을 DMA로 읽는데 캐시 일관성이 없다.
 * CPU가 엔트리를 고쳐도 그것이 캐시에만 있으면 MMU는 옛 값을
 * 본다. dma_sync_single_for_device가 그 간극을 메운다.
 *
 * 방향이 DMA_TO_DEVICE인 이유: 테이블은 CPU가 쓰고 하드웨어가
 * 읽기만 한다.
 *
 * 실행 컨텍스트: 테이블 갱신 경로. dt_lock을 잡은 상태.
 *
 * 호출 체인:
 *   rk_dte_get_page_table() / map_iova() / unmap_iova()
 *   → [rk_table_flush] → dma_sync_single_for_device()
 */
static inline void rk_table_flush(struct rk_iommu_domain *dom, dma_addr_t dma,
				  unsigned int count)
{
	/* [한국어] 엔트리 개수를 바이트로 환산한다. 모든 엔트리가 u32다. */
	size_t size = count * sizeof(u32); /* count of u32 entry */

	/* [한국어] CPU 캐시의 내용을 메모리로 밀어내 하드웨어가 볼 수
	 * 있게 한다. 이것을 빠뜨리면 MMU가 옛 엔트리를 읽어
	 * 원인을 찾기 어려운 폴트가 난다. */
	dma_sync_single_for_device(dom->dma_dev, dma, size, DMA_TO_DEVICE);
}

/*
 * [한국어]
 * to_rk_domain - iommu_domain에서 바깥 rk_iommu_domain을 복원한다
 *
 * @dom: 코어가 넘겨준 도메인.
 * @return: 그것을 품은 드라이버 쪽 도메인.
 *
 * 실행 컨텍스트: 모든 도메인 콜백의 첫 줄.
 *
 * 호출 체인:
 *   각종 iommu_domain_ops 콜백 → [to_rk_domain]
 */
static struct rk_iommu_domain *to_rk_domain(struct iommu_domain *dom)
{
	/* [한국어] 임베드 멤버의 주소에서 바깥 구조체를 역산한다. */
	return container_of(dom, struct rk_iommu_domain, domain);
}

/*
 * The Rockchip rk3288 iommu uses a 2-level page table.
 * The first level is the "Directory Table" (DT).
 * The DT consists of 1024 4-byte Directory Table Entries (DTEs), each pointing
 * to a "Page Table".
 * The second level is the 1024 Page Tables (PT).
 * Each PT consists of 1024 4-byte Page Table Entries (PTEs), each pointing to
 * a 4 KB page of physical memory.
 *
 * The DT and each PT fits in a single 4 KB page (4-bytes * 1024 entries).
 * Each iommu device has a MMU_DTE_ADDR register that contains the physical
 * address of the start of the DT page.
 *
 * The structure of the page table is as follows:
 *
 *                   DT
 * MMU_DTE_ADDR -> +-----+
 *                 |     |
 *                 +-----+     PT
 *                 | DTE | -> +-----+
 *                 +-----+    |     |     Memory
 *                 |     |    +-----+     Page
 *                 |     |    | PTE | -> +-----+
 *                 +-----+    +-----+    |     |
 *                            |     |    |     |
 *                            |     |    |     |
 *                            +-----+    |     |
 *                                       |     |
 *                                       |     |
 *                                       +-----+
 */

/*
 * [한국어] 위 그림의 요점: 2단계, 각 단계 1024 엔트리, 엔트리는
 * 4바이트, 그래서 각 테이블이 정확히 4KB 한 페이지에 들어간다.
 * 32비트 IOVA가 10비트(DTE) + 10비트(PTE) + 12비트(오프셋)로
 * 깔끔하게 나뉘는 설계다. 별도의 큰 페이지도, 가변 단계도 없다.
 */

/*
 * Each DTE has a PT address and a valid bit:
 * +---------------------+-----------+-+
 * | PT address          | Reserved  |V|
 * +---------------------+-----------+-+
 *  31:12 - PT address (PTs always starts on a 4 KB boundary)
 *  11: 1 - Reserved
 *      0 - 1 if PT @ PT address is valid
 */
/* [한국어] DTE에서 페이지 테이블 주소를 뽑는 마스크(v1).
 * 테이블이 항상 4KB 정렬이라 하위 12비트가 비고, 그 자리를
 * 유효 비트와 예약 비트가 쓴다. */
#define RK_DTE_PT_ADDRESS_MASK    0xfffff000
/* [한국어] 이 DTE가 가리키는 페이지 테이블이 유효하다는 표시. */
#define RK_DTE_PT_VALID           BIT(0)

/*
 * [한국어]
 * rk_dte_pt_address - DTE에서 페이지 테이블의 물리 주소를 뽑는다(v1)
 *
 * @dte: 디렉터리 테이블 엔트리 값.
 * @return: 페이지 테이블의 물리 주소.
 *
 * v1은 32비트 주소를 그대로 담으므로 마스킹 한 번이면 끝난다.
 * 이 함수는 PTE에도 쓰인다 — 주소 필드의 배치가 같기 때문이다.
 *
 * 실행 컨텍스트: 조회/해제/진단 경로. 순수 계산이다.
 *
 * 호출 체인:
 *   rk_ops->pt_address 를 통해 간접 호출 → [rk_dte_pt_address]
 */
static inline phys_addr_t rk_dte_pt_address(u32 dte)
{
	/* [한국어] 상위 20비트만 남기면 그것이 4KB 정렬된 주소다. */
	return (phys_addr_t)dte & RK_DTE_PT_ADDRESS_MASK;
}

/*
 * In v2:
 * 31:12 - PT address bit 31:0
 * 11: 8 - PT address bit 35:32
 *  7: 4 - PT address bit 39:36
 *  3: 1 - Reserved
 *     0 - 1 if PT @ PT address is valid
 */
/* [한국어] v2 엔트리에서 주소가 차지하는 전 구간(31:4).
 * v1의 예약 비트 자리까지 주소가 침범한 것이 v2의 핵심 변화다. */
#define RK_DTE_PT_ADDRESS_MASK_V2 GENMASK_ULL(31, 4)
/* [한국어] 엔트리 안에서 물리 주소 35:32가 들어앉은 자리. */
#define DTE_HI_MASK1	GENMASK(11, 8)
/* [한국어] 엔트리 안에서 물리 주소 39:36이 들어앉은 자리. */
#define DTE_HI_MASK2	GENMASK(7, 4)
/* [한국어] 비트 8을 비트 32로 올리는 시프트 양. */
#define DTE_HI_SHIFT1	24 /* shift bit 8 to bit 32 */
/* [한국어] 비트 4를 비트 36으로 올리는 시프트 양. */
#define DTE_HI_SHIFT2	32 /* shift bit 4 to bit 36 */
/* [한국어] 물리 주소 쪽에서 본 35:32 구간. 엔트리를 만들 때
 * 이 비트들을 DTE_HI_MASK1 자리로 내린다. */
#define PAGE_DESC_HI_MASK1	GENMASK_ULL(35, 32)
/* [한국어] 물리 주소 쪽에서 본 39:36 구간. */
#define PAGE_DESC_HI_MASK2	GENMASK_ULL(39, 36)

/*
 * [한국어]
 * rk_dte_pt_address_v2 - DTE에서 물리 주소를 복원한다(v2, 40비트)
 *
 * @dte: 엔트리 값.
 * @return: 복원된 40비트 물리 주소.
 *
 * v2가 32비트 엔트리로 40비트 주소를 표현하는 방법이 여기 있다.
 * 하위 20비트(31:12)는 그대로 두고, 남는 8비트를 예약 자리에
 * 나눠 담았다가 되돌려 올린다:
 *
 *   엔트리 [11:8] → 주소 [35:32]   (24비트 왼쪽으로)
 *   엔트리 [ 7:4] → 주소 [39:36]   (32비트 왼쪽으로)
 *
 * 그 대가로 v2 엔트리에는 유효 비트(0)와 3비트만 남는다.
 *
 * 실행 컨텍스트: 조회/해제/진단 경로. 순수 계산이다.
 *
 * 호출 체인:
 *   rk_ops->pt_address 를 통해 간접 호출 → [rk_dte_pt_address_v2]
 */
static inline phys_addr_t rk_dte_pt_address_v2(u32 dte)
{
	/* [한국어] 32비트 엔트리를 64비트로 넓혀 시프트 시 잘리지 않게 한다. */
	u64 dte_v2 = dte;

	/* [한국어] 흩어진 세 조각을 제자리로 모은다:
	 * 상위 조각 둘을 각각 올리고, 원래 자리에 있던 하위 20비트를
	 * 그대로 더한다. */
	dte_v2 = ((dte_v2 & DTE_HI_MASK2) << DTE_HI_SHIFT2) |
		 ((dte_v2 & DTE_HI_MASK1) << DTE_HI_SHIFT1) |
		 (dte_v2 & RK_DTE_PT_ADDRESS_MASK);

	return (phys_addr_t)dte_v2;	/* [한국어] 모아 붙인 40비트 값을 물리 주소 타입으로 돌려준다. */
}

/*
 * [한국어]
 * rk_dte_is_pt_valid - DTE가 유효한 페이지 테이블을 가리키는지 본다
 *
 * @dte: 엔트리 값.
 * @return: 유효하면 참.
 *
 * 유효 비트의 위치는 v1과 v2가 같아 버전 분기가 필요 없다.
 *
 * 실행 컨텍스트: 모든 워크 경로. 순수 판별이다.
 *
 * 호출 체인:
 *   map/unmap/조회/해제/진단 → [rk_dte_is_pt_valid]
 */
static inline bool rk_dte_is_pt_valid(u32 dte)
{
	/* [한국어] 최하위 비트 하나가 유효 여부를 말한다. */
	return dte & RK_DTE_PT_VALID;
}

/*
 * [한국어]
 * rk_mk_dte - 페이지 테이블 주소에서 DTE 값을 만든다(v1)
 *
 * @pt_dma: 페이지 테이블의 DMA 주소.
 * @return: 써 넣을 DTE 값.
 *
 * 실행 컨텍스트: 테이블 설치와 DTE_ADDR 레지스터 설정.
 *
 * 호출 체인:
 *   rk_ops->mk_dtentries 를 통해 간접 호출 → [rk_mk_dte]
 */
static inline u32 rk_mk_dte(dma_addr_t pt_dma)
{
	/* [한국어] 4KB 정렬된 주소에 유효 비트를 얹으면 끝이다. */
	return (pt_dma & RK_DTE_PT_ADDRESS_MASK) | RK_DTE_PT_VALID;
}

/*
 * [한국어]
 * rk_mk_dte_v2 - 페이지 테이블 주소에서 DTE 값을 만든다(v2)
 *
 * @pt_dma: 페이지 테이블의 DMA 주소(최대 40비트).
 * @return: 써 넣을 DTE 값.
 *
 * rk_dte_pt_address_v2()의 정확한 역연산이다. 주소의 상위 8비트를
 * 엔트리의 예약 자리로 **내려** 담는다.
 *
 * 이 함수가 PTE 생성에도 재사용된다(rk_mk_pte_v2). 주소 배치가
 * 같기 때문인데, 그래서 v2에서는 DTE와 PTE의 주소 인코딩이
 * 문자 그대로 동일하다.
 *
 * 실행 컨텍스트: 테이블 설치와 PTE 생성.
 *
 * 호출 체인:
 *   rk_ops->mk_dtentries / rk_mk_pte_v2() → [rk_mk_dte_v2]
 */
static inline u32 rk_mk_dte_v2(dma_addr_t pt_dma)
{
	/* [한국어] 주소의 세 구간을 엔트리 배치로 재배열한다.
	 * 하위 20비트는 제자리, 상위 두 조각은 각각 아래로 내린다. */
	pt_dma = (pt_dma & RK_DTE_PT_ADDRESS_MASK) |
		 ((pt_dma & PAGE_DESC_HI_MASK1) >> DTE_HI_SHIFT1) |
		 (pt_dma & PAGE_DESC_HI_MASK2) >> DTE_HI_SHIFT2;

	/* [한국어] 31:4 구간만 남기고 유효 비트를 얹는다. */
	return (pt_dma & RK_DTE_PT_ADDRESS_MASK_V2) | RK_DTE_PT_VALID;
}

/*
 * Each PTE has a Page address, some flags and a valid bit:
 * +---------------------+---+-------+-+
 * | Page address        |Rsv| Flags |V|
 * +---------------------+---+-------+-+
 *  31:12 - Page address (Pages always start on a 4 KB boundary)
 *  11: 9 - Reserved
 *   8: 1 - Flags
 *      8 - Read allocate - allocate cache space on read misses
 *      7 - Read cache - enable cache & prefetch of data
 *      6 - Write buffer - enable delaying writes on their way to memory
 *      5 - Write allocate - allocate cache space on write misses
 *      4 - Write cache - different writes can be merged together
 *      3 - Override cache attributes
 *          if 1, bits 4-8 control cache attributes
 *          if 0, the system bus defaults are used
 *      2 - Writable
 *      1 - Readable
 *      0 - 1 if Page @ Page address is valid
 */
/* [한국어] PTE에서 페이지의 물리 주소를 뽑는 마스크(v1). */
#define RK_PTE_PAGE_ADDRESS_MASK  0xfffff000
/* [한국어] 플래그 비트 전체(8:1). 폴트 진단에서 그대로 찍어 준다.
 * 위 표가 말하듯 캐시 정책까지 담을 수 있지만, 현재 이 드라이버는
 * 읽기/쓰기 두 비트만 쓴다. */
#define RK_PTE_PAGE_FLAGS_MASK    0x000001fe
/* [한국어] 이 페이지에 쓰기를 허용한다. */
#define RK_PTE_PAGE_WRITABLE      BIT(2)
/* [한국어] 이 페이지에서 읽기를 허용한다. */
#define RK_PTE_PAGE_READABLE      BIT(1)
/* [한국어] 이 PTE가 유효한 매핑이라는 표시. */
#define RK_PTE_PAGE_VALID         BIT(0)

/*
 * [한국어]
 * rk_pte_is_page_valid - PTE가 유효한 매핑인지 본다
 *
 * @pte: 엔트리 값.
 * @return: 유효하면 참.
 *
 * 실행 컨텍스트: 조회/매핑/해제/진단. 순수 판별이다.
 *
 * 호출 체인:
 *   map_iova() / unmap_iova() / iova_to_phys() → [rk_pte_is_page_valid]
 */
static inline bool rk_pte_is_page_valid(u32 pte)
{
	/* [한국어] 최하위 비트가 유효 여부다. */
	return pte & RK_PTE_PAGE_VALID;
}

/* TODO: set cache flags per prot IOMMU_CACHE */
/*
 * [한국어]
 * rk_mk_pte - 물리 주소와 권한에서 PTE 값을 만든다(v1)
 *
 * @page: 매핑할 물리 주소.
 * @prot: IOMMU_READ/WRITE 등 요청 권한.
 * @return: 써 넣을 PTE 값.
 *
 * 원본의 TODO가 말하듯, PTE에는 캐시 정책 비트(4~8)가 더 있지만
 * 아직 IOMMU_CACHE와 연결되어 있지 않다. 지금은 시스템 버스의
 * 기본값을 그대로 쓴다(비트 3이 0이므로).
 *
 * 실행 컨텍스트: 매핑 경로. 순수 계산이다.
 *
 * 호출 체인:
 *   rk_ops->mk_ptentries 를 통해 간접 호출 → [rk_mk_pte]
 */
static u32 rk_mk_pte(phys_addr_t page, int prot)
{
	/* [한국어] 권한 비트를 모을 자리. */
	u32 flags = 0;
	/* [한국어] 읽기를 요청했으면 읽기 비트를 세운다. */
	flags |= (prot & IOMMU_READ) ? RK_PTE_PAGE_READABLE : 0;
	/* [한국어] 쓰기를 요청했으면 쓰기 비트를 세운다.
	 * 둘 다 없으면 유효하지만 접근이 모두 막힌 매핑이 된다. */
	flags |= (prot & IOMMU_WRITE) ? RK_PTE_PAGE_WRITABLE : 0;
	/* [한국어] 물리 주소를 4KB 경계로 자른다. */
	page &= RK_PTE_PAGE_ADDRESS_MASK;
	/* [한국어] 주소, 권한, 유효 비트를 합쳐 엔트리를 완성한다. */
	return page | flags | RK_PTE_PAGE_VALID;
}

/*
 * In v2:
 * 31:12 - Page address bit 31:0
 * 11: 8 - Page address bit 35:32
 *  7: 4 - Page address bit 39:36
 *     3 - Security
 *     2 - Writable
 *     1 - Readable
 *     0 - 1 if Page @ Page address is valid
 */

/*
 * [한국어]
 * rk_mk_pte_v2 - 물리 주소와 권한에서 PTE 값을 만든다(v2)
 *
 * @page: 매핑할 물리 주소(최대 40비트).
 * @prot: 요청 권한.
 * @return: 써 넣을 PTE 값.
 *
 * v2에서는 PTE와 DTE의 주소 인코딩이 같아, 주소 부분을
 * rk_mk_dte_v2()에 그대로 맡기고 권한 비트만 얹는다.
 * 위 표가 보여 주듯 v2의 PTE에는 캐시 정책 비트가 없다 —
 * 그 자리를 주소의 상위 비트가 가져갔기 때문이다.
 *
 * 실행 컨텍스트: 매핑 경로. 순수 계산이다.
 *
 * 호출 체인:
 *   rk_ops->mk_ptentries 를 통해 간접 호출 → [rk_mk_pte_v2]
 *   → rk_mk_dte_v2()
 */
static u32 rk_mk_pte_v2(phys_addr_t page, int prot)
{
	/* [한국어] 권한 비트를 모을 자리. */
	u32 flags = 0;

	/* [한국어] 읽기 권한. */
	flags |= (prot & IOMMU_READ) ? RK_PTE_PAGE_READABLE : 0;
	/* [한국어] 쓰기 권한. */
	flags |= (prot & IOMMU_WRITE) ? RK_PTE_PAGE_WRITABLE : 0;

	/* [한국어] 주소 인코딩과 유효 비트는 DTE와 완전히 같으므로
	 * 그 함수를 그대로 빌려 쓴다. */
	return rk_mk_dte_v2(page) | flags;
}

/*
 * [한국어]
 * rk_mk_pte_invalid - PTE에서 유효 비트만 지운다
 *
 * @pte: 현재 엔트리 값.
 * @return: 무효화된 엔트리 값.
 *
 * 주소와 플래그를 남겨 두는 것이 의도적이다. 해제 후에도 옛 값이
 * 남아 있으면 폴트가 났을 때 log_iova()가 "여기 무엇이 있었는지"를
 * 보여 줄 수 있다.
 *
 * 실행 컨텍스트: 해제 경로. 순수 계산이다.
 *
 * 호출 체인:
 *   rk_iommu_unmap_iova() → [rk_mk_pte_invalid]
 */
static u32 rk_mk_pte_invalid(u32 pte)
{
	/* [한국어] 유효 비트만 떨어뜨린다 — 나머지는 진단을 위해 남긴다. */
	return pte & ~RK_PTE_PAGE_VALID;
}

/*
 * rk3288 iova (IOMMU Virtual Address) format
 *  31       22.21       12.11          0
 * +-----------+-----------+-------------+
 * | DTE index | PTE index | Page offset |
 * +-----------+-----------+-------------+
 *  31:22 - DTE index   - index of DTE in DT
 *  21:12 - PTE index   - index of PTE in PT @ DTE.pt_address
 *  11: 0 - Page offset - offset into page @ PTE.page_address
 */
/* [한국어] IOVA에서 디렉터리 인덱스가 차지하는 상위 10비트. */
#define RK_IOVA_DTE_MASK    0xffc00000
/* [한국어] 그 구간을 0으로 내리는 시프트 양. */
#define RK_IOVA_DTE_SHIFT   22
/* [한국어] IOVA에서 페이지 테이블 인덱스가 차지하는 가운데 10비트. */
#define RK_IOVA_PTE_MASK    0x003ff000
/* [한국어] 그 구간을 0으로 내리는 시프트 양. */
#define RK_IOVA_PTE_SHIFT   12
/* [한국어] 페이지 안의 오프셋(하위 12비트). */
#define RK_IOVA_PAGE_MASK   0x00000fff
/* [한국어] 오프셋은 이미 최하위라 시프트가 0이다. 대칭을 위해 정의해 둔다. */
#define RK_IOVA_PAGE_SHIFT  0

/*
 * [한국어]
 * rk_iova_dte_index - IOVA에서 디렉터리 테이블 인덱스를 뽑는다
 *
 * @iova: 대상 IOVA.
 * @return: DT 안에서의 인덱스(0~1023).
 *
 * 실행 컨텍스트: 모든 워크 경로. 순수 계산이다.
 *
 * 호출 체인:
 *   map/unmap/조회/진단 → [rk_iova_dte_index]
 */
static u32 rk_iova_dte_index(dma_addr_t iova)
{
	/* [한국어] 상위 10비트를 뽑아 인덱스로 만든다. */
	return (u32)(iova & RK_IOVA_DTE_MASK) >> RK_IOVA_DTE_SHIFT;
}

/*
 * [한국어]
 * rk_iova_pte_index - IOVA에서 페이지 테이블 인덱스를 뽑는다
 *
 * @iova: 대상 IOVA.
 * @return: PT 안에서의 인덱스(0~1023).
 *
 * 실행 컨텍스트: 모든 워크 경로. 순수 계산이다.
 *
 * 호출 체인:
 *   map/unmap/조회/진단 → [rk_iova_pte_index]
 */
static u32 rk_iova_pte_index(dma_addr_t iova)
{
	/* [한국어] 가운데 10비트를 뽑아 인덱스로 만든다. */
	return (u32)(iova & RK_IOVA_PTE_MASK) >> RK_IOVA_PTE_SHIFT;
}

/*
 * [한국어]
 * rk_iova_page_offset - IOVA에서 페이지 안의 오프셋을 뽑는다
 *
 * @iova: 대상 IOVA.
 * @return: 0~4095 사이의 오프셋.
 *
 * 실행 컨텍스트: 조회와 진단.
 *
 * 호출 체인:
 *   rk_iommu_iova_to_phys() / log_iova() → [rk_iova_page_offset]
 */
static u32 rk_iova_page_offset(dma_addr_t iova)
{
	/* [한국어] 하위 12비트가 곧 오프셋이다. 시프트는 0이지만
	 * 위 두 함수와 형태를 맞춰 둔다. */
	return (u32)(iova & RK_IOVA_PAGE_MASK) >> RK_IOVA_PAGE_SHIFT;
}

/*
 * [한국어]
 * rk_iommu_read - MMU 레지스터 하나를 읽는다
 *
 * @base: 대상 MMU 인스턴스의 레지스터 베이스.
 * @offset: 레지스터 오프셋.
 * @return: 읽은 값.
 *
 * 인스턴스별 베이스를 받는 것이 요점이다. 한 IOMMU에 MMU가 여럿이라
 * "어느 인스턴스인가"를 항상 명시해야 한다.
 *
 * 실행 컨텍스트: 클럭이 켜져 있고 전원이 들어온 상태여야 한다.
 *
 * 호출 체인:
 *   상태 판정 함수들 / 인터럽트 핸들러 → [rk_iommu_read]
 */
static u32 rk_iommu_read(void __iomem *base, u32 offset)
{
	/* [한국어] MMIO 읽기. readl은 순서 보장을 포함한다. */
	return readl(base + offset);
}

/*
 * [한국어]
 * rk_iommu_write - MMU 레지스터 하나에 쓴다
 *
 * @base: 대상 MMU 인스턴스의 레지스터 베이스.
 * @offset: 레지스터 오프셋.
 * @value: 쓸 값.
 * @return: 없음.
 *
 * 실행 컨텍스트: 클럭이 켜진 상태.
 *
 * 호출 체인:
 *   enable/disable/zap/인터럽트 처리 → [rk_iommu_write]
 */
static void rk_iommu_write(void __iomem *base, u32 offset, u32 value)
{
	/* [한국어] MMIO 쓰기. */
	writel(value, base + offset);
}

/*
 * [한국어]
 * rk_iommu_command - 모든 MMU 인스턴스에 같은 명령을 낸다
 *
 * @iommu: 대상 IOMMU 블록.
 * @command: RK_MMU_CMD_ 계열 값.
 * @return: 없음.
 *
 * 이 드라이버의 기본 원칙이 드러나는 함수다: **인스턴스들은 항상
 * 같은 상태여야 한다.** 그래서 명령은 언제나 전부에 뿌린다.
 *
 * 실행 컨텍스트: 클럭이 켜진 상태. stall 규약을 지켜야 하는
 * 명령이라면 호출자가 먼저 stall을 걸어 둔다.
 *
 * 호출 체인:
 *   enable/disable_stall(), enable/disable_paging(), force_reset()
 *   → [rk_iommu_command]
 */
static void rk_iommu_command(struct rk_iommu *iommu, u32 command)
{
	/* [한국어] 인스턴스 순회 인덱스. */
	int i;

	/* [한국어] 모든 MMU에 같은 명령을 쓴다. 하나라도 빠지면
	 * 인스턴스 간 상태가 어긋나 이후 판정이 모두 어긋난다. */
	for (i = 0; i < iommu->num_mmu; i++)
		writel(command, iommu->bases[i] + RK_MMU_COMMAND);
}

/*
 * [한국어]
 * rk_iommu_base_command - 지정한 한 인스턴스에만 명령을 낸다
 *
 * @base: 대상 MMU 인스턴스의 레지스터 베이스.
 * @command: 명령 값.
 * @return: 없음.
 *
 * 인스턴스별 처리가 필요한 두 곳에서 쓰인다: 폴트를 낸 인스턴스에만
 * 캐시를 버리고 폴트 완료를 알릴 때, 그리고 enable에서 인스턴스마다
 * DTE_ADDR을 설정한 직후 캐시를 버릴 때다.
 *
 * 실행 컨텍스트: 클럭이 켜진 상태.
 *
 * 호출 체인:
 *   rk_iommu_irq() / rk_iommu_enable() → [rk_iommu_base_command]
 */
static void rk_iommu_base_command(void __iomem *base, u32 command)
{
	/* [한국어] 이 인스턴스에만 명령을 쓴다. */
	writel(command, base + RK_MMU_COMMAND);
}
/*
 * [한국어]
 * rk_iommu_zap_lines - IOVA 범위에 해당하는 IOTLB 항목들을 버린다
 *
 * @iommu: 대상 IOMMU 블록.
 * @iova_start: 무효화할 범위의 시작.
 * @size: 범위의 크기.
 * @return: 없음.
 *
 * ZAP_ONE_LINE 레지스터는 한 번에 한 페이지만 버린다. 그래서 범위
 * 전체를 페이지 단위로 훑으며 하나씩 써 넣는다. 원본의 TODO가
 * 지적하듯, 범위가 넓으면 전체를 버리는 편이 나을 수 있는데
 * 그 경계는 아직 정해져 있지 않다.
 *
 * 바깥 루프가 인스턴스, 안쪽 루프가 페이지인 순서에 유의 —
 * 인스턴스마다 자기 IOTLB를 따로 갖기 때문이다.
 *
 * 실행 컨텍스트: 클럭이 켜지고 전원이 들어온 상태(호출자가 보장).
 *
 * 호출 체인:
 *   rk_iommu_zap_iova() → [rk_iommu_zap_lines]
 */
static void rk_iommu_zap_lines(struct rk_iommu *iommu, dma_addr_t iova_start,
			       size_t size)
{
	/* [한국어] 인스턴스 순회 인덱스. */
	int i;
	/* [한국어] 무효화할 범위의 끝(미포함). */
	dma_addr_t iova_end = iova_start + size;
	/*
	 * TODO(djkurtz): Figure out when it is more efficient to shootdown the
	 * entire iotlb rather than iterate over individual iovas.
	 */
	/* [한국어] 인스턴스마다 독립된 IOTLB가 있어 각각 버려야 한다. */
	for (i = 0; i < iommu->num_mmu; i++) {
		/* [한국어] 이 인스턴스에서 훑을 페이지 커서. */
		dma_addr_t iova;

		/* [한국어] 페이지 하나씩 무효화 레지스터에 써 넣는다.
		 * 이 레지스터는 한 번에 한 줄만 버리기 때문이다. */
		for (iova = iova_start; iova < iova_end; iova += SPAGE_SIZE)
			rk_iommu_write(iommu->bases[i], RK_MMU_ZAP_ONE_LINE, iova);
	}
}

/*
 * [한국어]
 * rk_iommu_is_stall_active - 모든 인스턴스가 stall 상태인지 본다
 *
 * @iommu: 대상 IOMMU 블록.
 * @return: 전부 stall이면 참.
 *
 * 아래 세 개의 판정 함수가 같은 형태다: **모든 인스턴스가 만족해야
 * 참**이다. 하나라도 아직 전환되지 않았으면 거짓이므로, 폴링이
 * 늦은 인스턴스까지 기다리게 된다.
 *
 * readx_poll_timeout에 함수 포인터로 넘겨지므로 부수 효과가 없어야
 * 한다 — 읽기만 한다.
 *
 * 실행 컨텍스트: 클럭이 켜진 상태.
 *
 * 호출 체인:
 *   rk_iommu_enable_stall() / disable_stall() → [rk_iommu_is_stall_active]
 */
static bool rk_iommu_is_stall_active(struct rk_iommu *iommu)
{
	/* [한국어] 전부 참이어야 하므로 참으로 시작해 누적한다. */
	bool active = true;
	/* [한국어] 인스턴스 순회 인덱스. */
	int i;

	/* [한국어] 하나라도 stall이 아니면 결과가 거짓이 된다. */
	for (i = 0; i < iommu->num_mmu; i++)
		active &= !!(rk_iommu_read(iommu->bases[i], RK_MMU_STATUS) &
					   RK_MMU_STATUS_STALL_ACTIVE);

	return active;	/* [한국어] 하나라도 멈추지 않았으면 거짓이다. */
}

/*
 * [한국어]
 * rk_iommu_is_paging_enabled - 모든 인스턴스에서 페이징이 켜져 있는지 본다
 *
 * @iommu: 대상 IOMMU 블록.
 * @return: 전부 켜져 있으면 참.
 *
 * 실행 컨텍스트: 클럭이 켜진 상태.
 *
 * 호출 체인:
 *   enable_stall() / enable_paging() / disable_paging()
 *   → [rk_iommu_is_paging_enabled]
 */
static bool rk_iommu_is_paging_enabled(struct rk_iommu *iommu)
{
	/* [한국어] 전부 참이어야 하므로 참으로 시작한다. */
	bool enable = true;
	/* [한국어] 인스턴스 순회 인덱스. */
	int i;

	/* [한국어] 하나라도 꺼져 있으면 거짓이 된다. */
	for (i = 0; i < iommu->num_mmu; i++)
		enable &= !!(rk_iommu_read(iommu->bases[i], RK_MMU_STATUS) &
					   RK_MMU_STATUS_PAGING_ENABLED);

	return enable;	/* [한국어] 하나라도 꺼져 있으면 거짓이다. */
}

/*
 * [한국어]
 * rk_iommu_is_reset_done - 강제 리셋이 끝났는지 본다
 *
 * @iommu: 대상 IOMMU 블록.
 * @return: 모든 인스턴스의 DTE_ADDR이 0이면 참.
 *
 * 리셋의 완료를 상태 비트가 아니라 **DTE_ADDR이 지워졌는지**로
 * 판정한다. force_reset이 그 직전에 더미 값을 써 두므로, 그것이
 * 0으로 돌아왔다는 것이 곧 레지스터가 초기화됐다는 증거다.
 *
 * 실행 컨텍스트: 클럭이 켜진 상태.
 *
 * 호출 체인:
 *   rk_iommu_force_reset() → [rk_iommu_is_reset_done]
 */
static bool rk_iommu_is_reset_done(struct rk_iommu *iommu)
{
	/* [한국어] 전부 참이어야 하므로 참으로 시작한다. */
	bool done = true;
	/* [한국어] 인스턴스 순회 인덱스. */
	int i;

	/* [한국어] 리셋되면 DTE_ADDR이 0으로 돌아온다 — 그것을 확인한다. */
	for (i = 0; i < iommu->num_mmu; i++)
		done &= rk_iommu_read(iommu->bases[i], RK_MMU_DTE_ADDR) == 0;

	return done;	/* [한국어] 모든 인스턴스가 초기화됐을 때만 참이다. */
}

/*
 * [한국어]
 * rk_iommu_enable_stall - 변환을 멈춰 다른 명령을 받을 수 있게 한다
 *
 * @iommu: 대상 IOMMU 블록.
 * @return: 0 성공(또는 할 일 없음), -ETIMEDOUT.
 *
 * 이 하드웨어의 가장 특징적인 제약을 다루는 함수다. 페이징이
 * 도는 중에는 리셋이나 DTE_ADDR 변경 같은 명령이 무시된다.
 * 그래서 그런 작업은 반드시 stall로 감싸야 한다.
 *
 * 두 가지 빠른 반환이 있다. 이미 stall이면 할 일이 없고,
 * **페이징이 꺼져 있어도 할 일이 없다** — stall은 페이징이
 * 켜져 있을 때만 의미가 있기 때문이다. 원본 주석이 그 점을
 * 명시한다.
 *
 * 타임아웃이 나면 각 인스턴스의 상태를 찍어 준다. 어느 인스턴스가
 * 멈추지 않았는지 알아야 진단이 되기 때문이다.
 *
 * 실행 컨텍스트: 클럭이 켜진 상태. 폴링이라 최대 1ms 잡아먹는다.
 *
 * 호출 체인:
 *   rk_iommu_enable() / rk_iommu_disable() → [rk_iommu_enable_stall]
 *   → rk_iommu_command()
 */
static int rk_iommu_enable_stall(struct rk_iommu *iommu)
{
	/* [한국어] 폴링 결과와 인스턴스 순회 인덱스. */
	int ret, i;
	/* [한국어] readx_poll_timeout이 읽은 값을 담을 변수. */
	bool val;

	/* [한국어] 이미 멈춰 있으면 명령을 낼 필요가 없다. */
	if (rk_iommu_is_stall_active(iommu))
		return 0;

	/* Stall can only be enabled if paging is enabled */
	/* [한국어] 페이징이 꺼져 있으면 멈출 변환 자체가 없다.
	 * 명령을 내도 상태가 바뀌지 않아 폴링이 타임아웃할 뿐이다. */
	if (!rk_iommu_is_paging_enabled(iommu))
		return 0;

	/* [한국어] 모든 인스턴스에 stall 명령을 낸다. */
	rk_iommu_command(iommu, RK_MMU_CMD_ENABLE_STALL);

	/* [한국어] 전부 stall 상태가 될 때까지 100us 간격으로 확인한다.
	 * 조건이 val 자체이므로 "참이 될 때까지" 기다린다. */
	ret = readx_poll_timeout(rk_iommu_is_stall_active, iommu, val,
				 val, RK_MMU_POLL_PERIOD_US,
				 RK_MMU_POLL_TIMEOUT_US);
	/* [한국어] 시간이 지나도 멈추지 않았다 — 어느 인스턴스가
	 * 문제인지 알 수 있도록 전부의 상태를 찍는다. */
	if (ret)
		for (i = 0; i < iommu->num_mmu; i++)
			dev_err(iommu->dev, "Enable stall request timed out, status: %#08x\n",
				rk_iommu_read(iommu->bases[i], RK_MMU_STATUS));

	return ret;	/* [한국어] 폴링 결과를 그대로 전한다 — 0이면 전환이 끝났다. */
}

/*
 * [한국어]
 * rk_iommu_disable_stall - stall을 풀어 변환을 재개시킨다
 *
 * @iommu: 대상 IOMMU 블록.
 * @return: 0 성공, -ETIMEDOUT.
 *
 * enable_stall의 대칭이다. 폴링 조건이 !val인 점만 다르다 —
 * "stall이 풀릴 때까지" 기다린다.
 *
 * 실행 컨텍스트: 클럭이 켜진 상태.
 *
 * 호출 체인:
 *   rk_iommu_enable() / rk_iommu_disable() → [rk_iommu_disable_stall]
 */
static int rk_iommu_disable_stall(struct rk_iommu *iommu)
{
	/* [한국어] 폴링 결과와 인스턴스 인덱스. */
	int ret, i;
	/* [한국어] 폴링이 읽은 값. */
	bool val;

	/* [한국어] 이미 풀려 있으면 할 일이 없다. */
	if (!rk_iommu_is_stall_active(iommu))
		return 0;

	/* [한국어] 모든 인스턴스에 stall 해제 명령을 낸다. */
	rk_iommu_command(iommu, RK_MMU_CMD_DISABLE_STALL);

	/* [한국어] 조건이 !val — stall이 꺼질 때까지 기다린다. */
	ret = readx_poll_timeout(rk_iommu_is_stall_active, iommu, val,
				 !val, RK_MMU_POLL_PERIOD_US,
				 RK_MMU_POLL_TIMEOUT_US);
	/* [한국어] 풀리지 않았다면 각 인스턴스의 상태를 남긴다. */
	if (ret)
		for (i = 0; i < iommu->num_mmu; i++)
			dev_err(iommu->dev, "Disable stall request timed out, status: %#08x\n",
				rk_iommu_read(iommu->bases[i], RK_MMU_STATUS));

	return ret;	/* [한국어] 폴링 결과를 그대로 전한다. */
}

/*
 * [한국어]
 * rk_iommu_enable_paging - 변환을 켠다
 *
 * @iommu: 대상 IOMMU 블록.
 * @return: 0 성공, -ETIMEDOUT.
 *
 * DTE_ADDR을 설정하고 IOTLB를 비운 뒤 마지막에 부른다. 이 순간부터
 * 마스터의 DMA가 테이블을 거쳐 나간다.
 *
 * 실행 컨텍스트: 클럭이 켜지고 stall이 걸린 상태.
 *
 * 호출 체인:
 *   rk_iommu_enable() → [rk_iommu_enable_paging]
 */
static int rk_iommu_enable_paging(struct rk_iommu *iommu)
{
	/* [한국어] 폴링 결과와 인스턴스 인덱스. */
	int ret, i;
	/* [한국어] 폴링이 읽은 값. */
	bool val;

	/* [한국어] 이미 켜져 있으면 할 일이 없다. */
	if (rk_iommu_is_paging_enabled(iommu))
		return 0;

	/* [한국어] 모든 인스턴스에 페이징 활성화 명령을 낸다. */
	rk_iommu_command(iommu, RK_MMU_CMD_ENABLE_PAGING);

	/* [한국어] 전부 켜질 때까지 기다린다. */
	ret = readx_poll_timeout(rk_iommu_is_paging_enabled, iommu, val,
				 val, RK_MMU_POLL_PERIOD_US,
				 RK_MMU_POLL_TIMEOUT_US);
	/* [한국어] 켜지지 않았다면 상태를 남긴다. */
	if (ret)
		for (i = 0; i < iommu->num_mmu; i++)
			dev_err(iommu->dev, "Enable paging request timed out, status: %#08x\n",
				rk_iommu_read(iommu->bases[i], RK_MMU_STATUS));

	return ret;	/* [한국어] 폴링 결과를 그대로 전한다. */
}

/*
 * [한국어]
 * rk_iommu_disable_paging - 변환을 끈다
 *
 * @iommu: 대상 IOMMU 블록.
 * @return: 0 성공, -ETIMEDOUT.
 *
 * 이 하드웨어에 통과 모드가 따로 없으므로, 이 함수가 사실상
 * identity 도메인의 구현이다. 페이징을 끄면 마스터의 DMA가
 * 변환 없이 나간다.
 *
 * 실행 컨텍스트: 클럭이 켜지고 stall이 걸린 상태.
 *
 * 호출 체인:
 *   rk_iommu_disable() → [rk_iommu_disable_paging]
 */
static int rk_iommu_disable_paging(struct rk_iommu *iommu)
{
	/* [한국어] 폴링 결과와 인스턴스 인덱스. */
	int ret, i;
	/* [한국어] 폴링이 읽은 값. */
	bool val;

	/* [한국어] 이미 꺼져 있으면 할 일이 없다. */
	if (!rk_iommu_is_paging_enabled(iommu))
		return 0;

	/* [한국어] 모든 인스턴스에 페이징 비활성화 명령을 낸다. */
	rk_iommu_command(iommu, RK_MMU_CMD_DISABLE_PAGING);

	/* [한국어] 조건이 !val — 전부 꺼질 때까지 기다린다. */
	ret = readx_poll_timeout(rk_iommu_is_paging_enabled, iommu, val,
				 !val, RK_MMU_POLL_PERIOD_US,
				 RK_MMU_POLL_TIMEOUT_US);
	/* [한국어] 꺼지지 않았다면 상태를 남긴다. */
	if (ret)
		for (i = 0; i < iommu->num_mmu; i++)
			dev_err(iommu->dev, "Disable paging request timed out, status: %#08x\n",
				rk_iommu_read(iommu->bases[i], RK_MMU_STATUS));

	return ret;	/* [한국어] 폴링 결과를 그대로 전한다. */
}

/*
 * [한국어]
 * rk_iommu_force_reset - 레지스터를 초기 상태로 되돌린다
 *
 * @iommu: 대상 IOMMU 블록.
 * @return: 0 성공, -EFAULT(레지스터가 죽었다), -ETIMEDOUT.
 *
 * 도메인을 붙이기 직전에 불려, 이전 설정의 잔재를 지운다.
 *
 * 리셋 전에 하는 검사가 흥미롭다. DTE_ADDR에 마법 값(0xCAFEBABE)을
 * 마스킹해 써 보고 그대로 되읽히는지 확인한다. 되읽히지 않으면
 * 레지스터 인터페이스 자체가 동작하지 않는 것이라 — 클럭이
 * 안 켜졌거나 전원이 없거나 주소가 틀렸거나 — 리셋을 시도해 봐야
 * 소용이 없다. 마스킹을 거치는 이유는 하위 비트가 주소가 아니라
 * 플래그 자리여서 그대로 되읽히지 않기 때문이다.
 *
 * reset_disabled면 통째로 건너뛴다. 일부 보드에서 리셋이 다른
 * 블록에 영향을 주기 때문이다.
 *
 * 실행 컨텍스트: 클럭이 켜지고 stall이 걸린 상태. 최대 0.1초 걸린다.
 *
 * 호출 체인:
 *   rk_iommu_enable() → [rk_iommu_force_reset] → rk_iommu_command()
 */
static int rk_iommu_force_reset(struct rk_iommu *iommu)
{
	/* [한국어] 폴링 결과와 인스턴스 인덱스. */
	int ret, i;
	/* [한국어] 검사용으로 써 볼 마법 주소 값. */
	u32 dte_addr;
	/* [한국어] 폴링이 읽은 값. */
	bool val;

	/* [한국어] 이 보드에서는 리셋이 금지되어 있다 — 그냥 성공으로 본다. */
	if (iommu->reset_disabled)
		return 0;

	/*
	 * Check if register DTE_ADDR is working by writing DTE_ADDR_DUMMY
	 * and verifying that upper 5 (v1) or 7 (v2) nybbles are read back.
	 */
	/* [한국어] 인스턴스마다 레지스터가 살아 있는지 먼저 확인한다. */
	for (i = 0; i < iommu->num_mmu; i++) {
		/* [한국어] 마법 값을 이 세대의 주소 형식으로 걸러 낸다 —
		 * 하드웨어가 되돌려 줄 값과 같은 모양으로 만드는 것이다. */
		dte_addr = rk_ops->pt_address(DTE_ADDR_DUMMY);
		rk_iommu_write(iommu->bases[i], RK_MMU_DTE_ADDR, dte_addr);

		/* [한국어] 되읽은 값이 다르면 레지스터 인터페이스가 죽어
		 * 있다는 뜻이다. 리셋을 시도할 이유가 없다. */
		if (dte_addr != rk_iommu_read(iommu->bases[i], RK_MMU_DTE_ADDR)) {
			dev_err(iommu->dev, "Error during raw reset. MMU_DTE_ADDR is not functioning\n");	/* [한국어] 레지스터가 응답하지 않는다 — 클럭이나 전원이 없다는 뜻이다. */
			return -EFAULT;	/* [한국어] 리셋을 시도해도 소용없으므로 여기서 접는다. */
		}
	}

	/* [한국어] 모든 인스턴스에 강제 리셋 명령을 낸다. */
	rk_iommu_command(iommu, RK_MMU_CMD_FORCE_RESET);

	/* [한국어] DTE_ADDR이 0으로 돌아올 때까지 기다린다.
	 * 방금 마법 값을 써 두었으므로 0이면 확실히 리셋된 것이다.
	 * 리셋은 느려서 대기 시간이 다른 전환의 100배다. */
	ret = readx_poll_timeout(rk_iommu_is_reset_done, iommu, val,
				 val, RK_MMU_FORCE_RESET_TIMEOUT_US,
				 RK_MMU_POLL_TIMEOUT_US);
	if (ret) {	/* [한국어] 시간 안에 DTE_ADDR이 0으로 돌아오지 않았다. */
		dev_err(iommu->dev, "FORCE_RESET command timed out\n");	/* [한국어] 리셋이 끝나지 않았음을 알린다. */
		return ret;	/* [한국어] 호출자가 이 IOMMU를 켜지 못하게 한다. */
	}

	return 0;	/* [한국어] 검사와 리셋이 모두 끝났다. */
}

/*
 * [한국어]
 * log_iova - 폴트가 난 IOVA를 소프트웨어로 되짚어 진단 정보를 찍는다
 *
 * @iommu: 폴트를 낸 IOMMU 블록.
 * @index: 폴트를 낸 MMU 인스턴스의 번호.
 * @iova: 폴트가 난 주소.
 * @return: 없음.
 *
 * 이 드라이버의 진단 도구다. 하드웨어가 걸었을 경로를 그대로
 * 다시 걸어, **어느 단계에서 끊겼는지**를 보여 준다.
 *
 *   DTE가 무효 → 그 4MB 영역에 매핑이 아예 없다
 *   PTE가 무효 → 영역은 있으나 그 페이지가 매핑되지 않았다
 *   둘 다 유효 → 매핑은 있는데 권한이 모자랐다(flags를 보라)
 *
 * 도메인의 dt가 아니라 **하드웨어 레지스터에서 읽은 DTE_ADDR**을
 * 출발점으로 삼는 점이 중요하다. 그래야 소프트웨어의 장부가 아니라
 * 하드웨어가 실제로 보고 있는 테이블을 확인할 수 있다. 둘이
 * 다르다면 그것 자체가 버그의 단서다.
 *
 * goto로 모이는 구조라 어느 단계에서 멈췄든 같은 두 줄이 찍히고,
 * 도달하지 못한 값들은 초깃값 0으로 남아 그 사실을 드러낸다.
 *
 * 실행 컨텍스트: 인터럽트 핸들러 안. 클럭이 켜진 상태.
 *
 * 호출 체인:
 *   rk_iommu_irq() → [log_iova]
 */
static void log_iova(struct rk_iommu *iommu, int index, dma_addr_t iova)
{
	/* [한국어] 폴트를 낸 인스턴스의 레지스터 베이스. */
	void __iomem *base = iommu->bases[index];
	/* [한국어] IOVA를 분해한 세 조각. */
	u32 dte_index, pte_index, page_offset;
	/* [한국어] 레지스터에서 읽은 DTE_ADDR의 원시 값. */
	u32 mmu_dte_addr;
	/* [한국어] 그 값에서 복원한 DT의 물리 주소와, 대상 DTE의 물리 주소. */
	phys_addr_t mmu_dte_addr_phys, dte_addr_phys;
	/* [한국어] 그 DTE의 커널 가상 주소. */
	u32 *dte_addr;
	/* [한국어] 읽어 온 DTE 값. */
	u32 dte;
	/* [한국어] PTE의 물리 주소. DTE가 무효면 0으로 남아 그 사실을 알린다. */
	phys_addr_t pte_addr_phys = 0;
	/* [한국어] PTE의 가상 주소. 도달하지 못하면 NULL로 남는다. */
	u32 *pte_addr = NULL;
	/* [한국어] 읽어 온 PTE 값. 도달하지 못하면 0. */
	u32 pte = 0;
	/* [한국어] 최종 물리 주소. 매핑이 유효할 때만 채워진다. */
	phys_addr_t page_addr_phys = 0;
	/* [한국어] PTE의 권한 플래그. 권한 문제를 진단하는 근거다. */
	u32 page_flags = 0;

	/* [한국어] 폴트 주소를 세 조각으로 분해한다. */
	dte_index = rk_iova_dte_index(iova);
	pte_index = rk_iova_pte_index(iova);	/* [한국어] 페이지 테이블 안에서의 인덱스. */
	page_offset = rk_iova_page_offset(iova);

	/* [한국어] 하드웨어가 실제로 쓰고 있는 DT의 주소를 레지스터에서
	 * 읽는다 — 소프트웨어 장부와 대조할 수 있게 하려는 것이다. */
	mmu_dte_addr = rk_iommu_read(base, RK_MMU_DTE_ADDR);
	mmu_dte_addr_phys = rk_ops->pt_address(mmu_dte_addr);

	/* [한국어] 그 DT 안에서 이 IOVA에 해당하는 DTE의 물리 주소.
	 * 엔트리가 4바이트라 인덱스에 4를 곱한다. */
	dte_addr_phys = mmu_dte_addr_phys + (4 * dte_index);
	/* [한국어] 물리 주소를 커널 가상 주소로 바꿔 직접 읽는다.
	 * 테이블이 저수준 매핑 영역에 있어 가능한 접근이다. */
	dte_addr = phys_to_virt(dte_addr_phys);
	dte = *dte_addr;

	/* [한국어] DTE가 무효면 그 4MB 영역 전체가 매핑되지 않은 것이다.
	 * 더 내려갈 수 없으니 지금까지의 정보만 찍는다. */
	if (!rk_dte_is_pt_valid(dte))
		goto print_it;

	/* [한국어] 페이지 테이블 안에서 이 IOVA의 PTE 위치를 구한다. */
	pte_addr_phys = rk_ops->pt_address(dte) + (pte_index * 4);
	pte_addr = phys_to_virt(pte_addr_phys);	/* [한국어] PTE를 커널 가상 주소로 읽는다. */
	pte = *pte_addr;

	/* [한국어] PTE가 무효면 그 페이지만 매핑되지 않은 것이다. */
	if (!rk_pte_is_page_valid(pte))
		goto print_it;

	/* [한국어] 여기까지 왔으면 매핑은 존재한다 — 그렇다면 폴트의
	 * 원인은 권한이다. 최종 주소와 플래그를 채워 그것을 보여 준다. */
	page_addr_phys = rk_ops->pt_address(pte) + page_offset;
	page_flags = pte & RK_PTE_PAGE_FLAGS_MASK;

/* [한국어] 어느 단계에서 멈췄든 이 지점으로 모여 같은 형식으로 찍는다. */
print_it:
	/* [한국어] 첫 줄: IOVA를 어떻게 분해했는지. */
	dev_err(iommu->dev, "iova = %pad: dte_index: %#03x pte_index: %#03x page_offset: %#03x\n",
		&iova, dte_index, pte_index, page_offset);
	/* [한국어] 둘째 줄: 단계별로 무엇을 읽었고 유효했는지.
	 * 도달하지 못한 단계는 0으로 찍혀 어디서 끊겼는지 드러난다. */
	dev_err(iommu->dev, "mmu_dte_addr: %pa dte@%pa: %#08x valid: %u pte@%pa: %#08x valid: %u page@%pa flags: %#03x\n",
		&mmu_dte_addr_phys, &dte_addr_phys, dte,
		rk_dte_is_pt_valid(dte), &pte_addr_phys, pte,
		rk_pte_is_page_valid(pte), &page_addr_phys, page_flags);
}

/*
 * [한국어]
 * rk_iommu_irq - 폴트와 버스 오류 인터럽트를 처리한다
 *
 * @irq: 인터럽트 번호(쓰지 않는다).
 * @dev_id: 등록 시 넘긴 rk_iommu.
 * @return: 우리 인터럽트였으면 IRQ_HANDLED, 아니면 IRQ_NONE.
 *
 * 인터럽트를 공유하므로(IRQF_SHARED) "우리 것이 아니면 IRQ_NONE"을
 * 정확히 지켜야 한다. 그래서 INT_STATUS가 0인 인스턴스는 건너뛰고,
 * 하나라도 상태가 있을 때만 HANDLED로 바꾼다.
 *
 * 시작하자마자 pm_runtime_get_if_in_use()를 부르는 것이 이 함수의
 * 핵심 방어다. 전원이 꺼진 IOMMU의 레지스터를 읽으면 시스템이
 * 멈출 수 있다. **꺼져 있으면 그냥 IRQ_NONE으로 물러난다** —
 * 꺼진 장치가 인터럽트를 냈을 리 없으므로 남의 인터럽트다.
 *
 * 폴트 처리 순서: 상태를 읽어 방향을 판정하고, 로그를 찍고,
 * 테이블을 되짚어 진단하고, 상위에 보고한 뒤, IOTLB를 버리고
 * 폴트 완료를 알린다. 마지막 두 단계가 재시도를 재개시킨다.
 *
 * 실행 컨텍스트: 인터럽트 문맥. 클럭을 직접 켜고 끈다.
 *
 * 호출 체인:
 *   커널 인터럽트 코어 → [rk_iommu_irq] → log_iova()
 *   → report_iommu_fault()
 */
static irqreturn_t rk_iommu_irq(int irq, void *dev_id)
{
	/* [한국어] 등록 시 넘긴 IOMMU 블록. */
	struct rk_iommu *iommu = dev_id;
	/* [한국어] MMU 상태 레지스터 값(폴트 방향 판정용). */
	u32 status;
	/* [한국어] 마스크가 적용된 인터럽트 상태. */
	u32 int_status;
	/* [한국어] 폴트가 난 주소. */
	dma_addr_t iova;
	/* [한국어] 반환값. 우리 인터럽트임이 확인되어야 HANDLED가 된다. */
	irqreturn_t ret = IRQ_NONE;
	/* [한국어] 인스턴스 인덱스와 PM 조회 결과. */
	int i, err;

	/* [한국어] 전원이 켜져 있을 때만 참조를 얻는다. 꺼져 있으면 0을
	 * 돌려주는데, 그때 레지스터를 읽으면 버스가 멈출 수 있다.
	 * 꺼진 장치가 인터럽트를 냈을 리 없으니 남의 것으로 본다. */
	err = pm_runtime_get_if_in_use(iommu->dev);
	if (!err || WARN_ON_ONCE(err < 0))	/* [한국어] 전원이 꺼져 있거나 PM 오류다 — 남의 인터럽트로 보고 물러난다. */
		return ret;

	/* [한국어] 레지스터를 만지려면 클럭이 필요하다. 실패하면
	 * 아무것도 할 수 없으므로 참조만 놓고 나간다. */
	if (WARN_ON(clk_bulk_enable(iommu->num_clocks, iommu->clocks)))
		goto out;

	/* [한국어] 어느 인스턴스가 인터럽트를 냈는지 하나씩 확인한다. */
	for (i = 0; i < iommu->num_mmu; i++) {
		int_status = rk_iommu_read(iommu->bases[i], RK_MMU_INT_STATUS);
		/* [한국어] 이 인스턴스는 조용하다 — 다음으로. */
		if (int_status == 0)
			continue;

		/* [한국어] 적어도 하나가 인터럽트를 냈으니 우리 것이 맞다. */
		ret = IRQ_HANDLED;
		/* [한국어] 폴트 주소를 읽어 둔다. 버스 오류 로그에도 쓰인다. */
		iova = rk_iommu_read(iommu->bases[i], RK_MMU_PAGE_FAULT_ADDR);

		/* [한국어] 페이지 폴트 처리. */
		if (int_status & RK_MMU_IRQ_PAGE_FAULT) {
			/* [한국어] 상위에 보고할 접근 방향. */
			int flags;

			/* [한국어] 상태 레지스터에서 읽기였는지 쓰기였는지 본다. */
			status = rk_iommu_read(iommu->bases[i], RK_MMU_STATUS);
			flags = (status & RK_MMU_STATUS_PAGE_FAULT_IS_WRITE) ?	/* [한국어] 상태 비트로 쓰기였는지 읽기였는지 가린다. */
					IOMMU_FAULT_WRITE : IOMMU_FAULT_READ;

			/* [한국어] 사람이 읽을 한 줄 요약을 먼저 남긴다. */
			dev_err(iommu->dev, "Page fault at %pad of type %s\n",
				&iova,
				str_write_read(flags == IOMMU_FAULT_WRITE));

			/* [한국어] 테이블을 되짚어 어느 단계에서 끊겼는지 찍는다. */
			log_iova(iommu, i, iova);

			/*
			 * Report page fault to any installed handlers.
			 * Ignore the return code, though, since we always zap cache
			 * and clear the page fault anyway.
			 */
			/* [한국어] 도메인에 붙어 있을 때만 상위 핸들러에 보고한다.
			 * 반환값을 보지 않는 이유는 아래에서 어차피 캐시를 버리고
			 * 폴트를 지우기 때문이다. */
			if (iommu->domain != &rk_identity_domain)
				report_iommu_fault(iommu->domain, iommu->dev, iova,
						   flags);
			else
				/* [한국어] 어디에도 붙지 않은 IOMMU가 폴트를 냈다 —
				 * 페이징이 꺼져 있어야 하므로 있을 수 없는 일이다. */
				dev_err(iommu->dev, "Page fault while iommu not attached to domain?\n");

			/* [한국어] 이 인스턴스의 IOTLB를 비운다. 폴트를 유발한
			 * 옛 항목이 남아 있으면 재시도가 또 실패한다. */
			rk_iommu_base_command(iommu->bases[i], RK_MMU_CMD_ZAP_CACHE);
			/* [한국어] 폴트 처리가 끝났음을 알려 재시도를 재개시킨다. */
			rk_iommu_base_command(iommu->bases[i], RK_MMU_CMD_PAGE_FAULT_DONE);
		}

		/* [한국어] 버스 오류는 테이블을 읽다 실패했다는 뜻이라
		 * 드라이버가 할 수 있는 일이 없다 — 기록만 한다. */
		if (int_status & RK_MMU_IRQ_BUS_ERROR)
			dev_err(iommu->dev, "BUS_ERROR occurred at %pad\n", &iova);

		/* [한국어] 우리가 활성화하지 않은 비트가 켜졌다 — 하드웨어가
		 * 예상 밖의 상태이거나 문서에 없는 동작이다. */
		if (int_status & ~RK_MMU_IRQ_MASK)
			dev_err(iommu->dev, "unexpected int_status: %#08x\n",
				int_status);

		/* [한국어] 확인한 비트들을 지워 인터럽트를 다시 무장시킨다.
		 * 이것을 빠뜨리면 같은 인터럽트가 무한히 반복된다. */
		rk_iommu_write(iommu->bases[i], RK_MMU_INT_CLEAR, int_status);
	}

	/* [한국어] 레지스터 작업이 끝났으니 클럭을 내린다. */
	clk_bulk_disable(iommu->num_clocks, iommu->clocks);

/* [한국어] 클럭 활성화 실패 시에도 거쳐야 하는 정리 지점. */
out:
	/* [한국어] 런타임 PM 참조를 놓아 다시 잠들 수 있게 한다. */
	pm_runtime_put(iommu->dev);
	return ret;	/* [한국어] 우리 인터럽트였는지 여부를 커널에 알린다. */
}

/*
 * [한국어]
 * rk_iommu_iova_to_phys - IOVA를 물리 주소로 변환한다(소프트웨어 워크)
 *
 * @domain: 대상 도메인.
 * @iova: 변환할 IOVA.
 * @return: 물리 주소, 매핑이 없으면 0.
 *
 * log_iova()와 같은 워크지만 이쪽은 **소프트웨어 장부**를 본다.
 * 하드웨어 레지스터를 읽지 않으므로 전원이 꺼져 있어도 답할 수
 * 있고, 클럭도 필요 없다.
 *
 * 페이지 오프셋을 더해 돌려주는 점에 주의 — 페이지 정렬되지 않은
 * IOVA도 정확히 변환한다.
 *
 * 실행 컨텍스트: 조회 경로. dt_lock을 irqsave로 잡는다.
 *
 * 호출 체인:
 *   IOMMU 코어 iova_to_phys → [rk_iommu_iova_to_phys]
 */
static phys_addr_t rk_iommu_iova_to_phys(struct iommu_domain *domain,
					 dma_addr_t iova)
{
	/* [한국어] 대상 도메인. */
	struct rk_iommu_domain *rk_domain = to_rk_domain(domain);
	/* [한국어] 인터럽트 상태 저장용. */
	unsigned long flags;
	/* [한국어] 페이지 테이블의 물리 주소와 최종 결과. */
	phys_addr_t pt_phys, phys = 0;
	/* [한국어] 읽어 온 엔트리들. */
	u32 dte, pte;
	/* [한국어] 페이지 테이블의 가상 주소. */
	u32 *page_table;

	/* [한국어] 테이블을 읽는 동안 갱신을 막는다. */
	spin_lock_irqsave(&rk_domain->dt_lock, flags);

	/* [한국어] 1단계: IOVA의 상위 10비트로 DTE를 찾는다. */
	dte = rk_domain->dt[rk_iova_dte_index(iova)];
	/* [한국어] 이 4MB 영역에 페이지 테이블이 없다 = 매핑 없음. */
	if (!rk_dte_is_pt_valid(dte))
		goto out;

	/* [한국어] DTE에서 페이지 테이블의 물리 주소를 뽑는다. */
	pt_phys = rk_ops->pt_address(dte);
	/* [한국어] 그 테이블을 커널 가상 주소로 바꿔 직접 읽는다. */
	page_table = (u32 *)phys_to_virt(pt_phys);
	/* [한국어] 2단계: 가운데 10비트로 PTE를 찾는다. */
	pte = page_table[rk_iova_pte_index(iova)];
	/* [한국어] 그 페이지가 매핑되어 있지 않다. */
	if (!rk_pte_is_page_valid(pte))
		goto out;

	/* [한국어] 페이지의 물리 주소에 페이지 안 오프셋을 더해
	 * 정확한 주소를 만든다. */
	phys = rk_ops->pt_address(pte) + rk_iova_page_offset(iova);
/* [한국어] 어느 단계에서 끊겼든 락을 풀고 나가는 공통 자리. */
out:
	spin_unlock_irqrestore(&rk_domain->dt_lock, flags);	/* [한국어] 테이블 읽기가 끝났으니 락을 놓는다. */

	return phys;	/* [한국어] 찾았으면 물리 주소, 못 찾았으면 0이다. */
}

/*
 * [한국어]
 * rk_iommu_zap_iova - 이 도메인을 쓰는 모든 IOMMU에서 IOTLB 항목을 버린다
 *
 * @rk_domain: 대상 도메인.
 * @iova: 무효화할 범위의 시작.
 * @size: 범위의 크기.
 * @return: 없음.
 *
 * 한 도메인에 여러 IOMMU가 붙어 있을 수 있으므로, 무효화는 목록을
 * 훑으며 각각에 대해 수행해야 한다.
 *
 * **전원이 켜진 IOMMU만 건드린다**는 것이 이 함수의 핵심이다.
 * 꺼진 IOMMU는 IOTLB 내용 자체가 사라졌고, 다시 켜질 때
 * rk_iommu_enable()이 ZAP_CACHE로 전체를 비우므로 지금 손댈
 * 이유가 없다. 오히려 꺼진 레지스터를 만지면 시스템이 멈춘다.
 *
 * 실행 컨텍스트: 매핑/해제 경로. iommus_lock을 잡은 채 하드웨어를
 * 만지므로 이 구간이 길어질 수 있다.
 *
 * 호출 체인:
 *   rk_iommu_unmap() / rk_iommu_zap_iova_first_last()
 *   → [rk_iommu_zap_iova] → rk_iommu_zap_lines()
 */
static void rk_iommu_zap_iova(struct rk_iommu_domain *rk_domain,
			      dma_addr_t iova, size_t size)
{
	/* [한국어] 목록 순회 커서. */
	struct list_head *pos;
	/* [한국어] 인터럽트 상태 저장용. */
	unsigned long flags;

	/* shootdown these iova from all iommus using this domain */
	/* [한국어] 순회 중 목록이 바뀌지 않도록 잠근다. */
	spin_lock_irqsave(&rk_domain->iommus_lock, flags);
	list_for_each(pos, &rk_domain->iommus) {
		/* [한국어] 이번에 처리할 IOMMU. */
		struct rk_iommu *iommu;
		/* [한국어] 전원 상태 조회 결과. */
		int ret;

		/* [한국어] 목록 노드에서 IOMMU 구조체를 복원한다. */
		iommu = list_entry(pos, struct rk_iommu, node);

		/* Only zap TLBs of IOMMUs that are powered on. */
		/* [한국어] 켜져 있을 때만 참조를 얻는다. 꺼져 있으면 0을
		 * 돌려주고, 그때는 아무것도 하지 않는 것이 옳다. */
		ret = pm_runtime_get_if_in_use(iommu->dev);
		/* [한국어] 음수는 PM 자체의 오류다 — 이 IOMMU는 건너뛴다. */
		if (WARN_ON_ONCE(ret < 0))
			continue;
		/* [한국어] 양수면 전원이 켜져 있고 참조를 얻었다. */
		if (ret) {
			/* [한국어] 레지스터를 만지려면 클럭이 필요하다. */
			WARN_ON(clk_bulk_enable(iommu->num_clocks,
						iommu->clocks));
			/* [한국어] 이 IOMMU의 모든 인스턴스에서 해당 범위를 버린다. */
			rk_iommu_zap_lines(iommu, iova, size);
			/* [한국어] 클럭을 내린다. */
			clk_bulk_disable(iommu->num_clocks, iommu->clocks);
			/* [한국어] 참조를 놓아 다시 잠들 수 있게 한다. */
			pm_runtime_put(iommu->dev);
		}
	}
	spin_unlock_irqrestore(&rk_domain->iommus_lock, flags);	/* [한국어] 목록 순회가 끝났으니 락을 놓는다. */
}

/*
 * [한국어]
 * rk_iommu_zap_iova_first_last - 범위의 첫 페이지와 마지막 페이지만 버린다
 *
 * @rk_domain: 대상 도메인.
 * @iova: 범위의 시작.
 * @size: 범위의 크기.
 * @return: 없음.
 *
 * 새 매핑을 만든 뒤에 부르는 최적화된 무효화다. 새로 매핑한
 * 범위 전체를 버릴 필요가 없는 이유는 이렇다: 방금 매핑한 페이지들은
 * IOTLB에 있을 수 없다 — 조금 전까지 매핑되지 않은 상태였으므로.
 *
 * 다만 **양 끝만은 예외**다. 이웃한 기존 매핑과 같은 캐시 줄
 * (같은 DTE나 인접 PTE)을 공유할 수 있어, 그 줄에 담긴 옛 값이
 * 남아 있을 수 있다. 그래서 첫 페이지와 마지막 페이지만 버린다.
 * 호출부의 원본 주석이 그 논리를 밝힌다.
 *
 * 실행 컨텍스트: 매핑 경로.
 *
 * 호출 체인:
 *   rk_iommu_map_iova() → [rk_iommu_zap_iova_first_last]
 */
static void rk_iommu_zap_iova_first_last(struct rk_iommu_domain *rk_domain,
					 dma_addr_t iova, size_t size)
{
	/* [한국어] 첫 페이지를 버린다 — 앞쪽 이웃과 캐시 줄을 공유할 수 있다. */
	rk_iommu_zap_iova(rk_domain, iova, SPAGE_SIZE);
	/* [한국어] 범위가 한 페이지보다 크면 마지막 페이지도 버린다.
	 * 한 페이지짜리라면 위에서 이미 처리했다. */
	if (size > SPAGE_SIZE)
		rk_iommu_zap_iova(rk_domain, iova + size - SPAGE_SIZE,
					SPAGE_SIZE);
}

/*
 * [한국어]
 * rk_dte_get_page_table - IOVA에 해당하는 페이지 테이블을 얻는다(없으면 만든다)
 *
 * @rk_domain: 대상 도메인.
 * @iova: 대상 IOVA.
 * @return: 페이지 테이블의 가상 주소, 실패하면 ERR_PTR.
 *
 * 매핑 경로에서 2단계 구조가 채워지는 유일한 지점이다.
 * 새 테이블을 만들 때 하는 일이 셋이다.
 *
 *  1) 4KB 페이지를 잡는다. 정렬이 보장되는 할당기를 쓴다.
 *  2) **그 페이지를 DMA 매핑한다.** 하드웨어가 DMA로 읽을 것이므로
 *     DMA 주소가 필요하고, 캐시 일관성도 이 매핑을 통해 관리된다.
 *  3) DTE를 채운 뒤 그 한 엔트리를 플러시한다. 플러시를 빠뜨리면
 *     하드웨어가 여전히 "무효"인 옛 DTE를 본다.
 *
 * dt_lock을 이미 잡았다고 가정하므로 GFP_ATOMIC으로 할당한다.
 *
 * 실행 컨텍스트: dt_lock을 잡은 상태. atomic 할당.
 *
 * 호출 체인:
 *   rk_iommu_map() → [rk_dte_get_page_table] → iommu_alloc_pages_sz()
 *   → dma_map_single() → rk_table_flush()
 */
static u32 *rk_dte_get_page_table(struct rk_iommu_domain *rk_domain,
				  dma_addr_t iova)
{
	/* [한국어] 얻거나 만든 페이지 테이블과, 고칠 DTE의 주소. */
	u32 *page_table, *dte_addr;
	/* [한국어] DTE의 인덱스와 값. */
	u32 dte_index, dte;
	/* [한국어] 페이지 테이블의 물리 주소. */
	phys_addr_t pt_phys;
	/* [한국어] 페이지 테이블의 DMA 주소(하드웨어가 볼 주소). */
	dma_addr_t pt_dma;

	/* [한국어] 호출자가 테이블 락을 잡았는지 확인한다. */
	assert_spin_locked(&rk_domain->dt_lock);

	/* [한국어] 이 IOVA가 속한 4MB 영역의 DTE 위치를 구한다. */
	dte_index = rk_iova_dte_index(iova);
	dte_addr = &rk_domain->dt[dte_index];	/* [한국어] 디렉터리에서 고칠 엔트리의 주소. */
	dte = *dte_addr;
	/* [한국어] 이미 페이지 테이블이 매달려 있으면 그것을 쓴다. */
	if (rk_dte_is_pt_valid(dte))
		goto done;

	/* [한국어] 새 페이지 테이블을 잡는다. 크기 지정 할당이라
	 * 4KB 정렬이 보장되고, 세대별 gfp 플래그(v1의 GFP_DMA32)가
	 * 주소 범위 제약을 반영한다. */
	page_table = iommu_alloc_pages_sz(GFP_ATOMIC | rk_ops->gfp_flags,
					  SPAGE_SIZE);
	if (!page_table)	/* [한국어] 테이블 페이지를 잡지 못했다. */
		return ERR_PTR(-ENOMEM);

	/* [한국어] 하드웨어가 이 테이블을 DMA로 읽으므로 DMA 매핑이
	 * 필요하다. 이후 rk_table_flush가 이 매핑을 통해 캐시를 민다. */
	pt_dma = dma_map_single(rk_domain->dma_dev, page_table, SPAGE_SIZE, DMA_TO_DEVICE);
	if (dma_mapping_error(rk_domain->dma_dev, pt_dma)) {	/* [한국어] DMA 매핑에 실패하면 하드웨어가 이 테이블을 읽을 수 없다. */
		dev_err(rk_domain->dma_dev, "DMA mapping error while allocating page table\n");
		/* [한국어] 매핑에 실패했으니 페이지도 되돌린다. */
		iommu_free_pages(page_table);
		return ERR_PTR(-ENOMEM);	/* [한국어] 테이블을 확보하지 못했음을 오류 포인터로 전한다. */
	}

	/* [한국어] DMA 주소로 DTE 값을 만들어 디렉터리에 써 넣는다. */
	dte = rk_ops->mk_dtentries(pt_dma);
	*dte_addr = dte;

	/* [한국어] 고친 DTE 한 개만 하드웨어에 밀어낸다.
	 * 주소는 DT의 DMA 주소에 엔트리 오프셋을 더해 구한다. */
	rk_table_flush(rk_domain,
		       rk_domain->dt_dma + dte_index * sizeof(u32), 1);
/* [한국어] 기존 테이블을 찾았든 새로 만들었든 여기로 모인다. */
done:
	/* [한국어] DTE에서 물리 주소를 뽑아 가상 주소로 바꿔 돌려준다.
	 * 새로 만든 경우에도 방금 채운 dte를 다시 해석하므로,
	 * 세대별 인코딩 차이가 자연히 흡수된다. */
	pt_phys = rk_ops->pt_address(dte);
	return (u32 *)phys_to_virt(pt_phys);	/* [한국어] 하드웨어가 볼 주소가 아니라 CPU가 접근할 가상 주소를 돌려준다. */
}

/*
 * [한국어]
 * rk_iommu_unmap_iova - 연속된 PTE들을 무효화한다
 *
 * @rk_domain: 대상 도메인.
 * @pte_addr: 첫 PTE의 가상 주소.
 * @pte_dma: 첫 PTE의 DMA 주소(플러시용).
 * @size: 해제할 크기.
 * @return: 실제로 해제한 바이트 수.
 *
 * 유효하지 않은 PTE를 만나면 **거기서 멈춘다.** 그 지점부터는
 * 애초에 매핑되어 있지 않았다는 뜻이므로, 요청받은 만큼을 다
 * 처리하지 못했더라도 그것이 정확한 결과다. 반환값이 요청보다
 * 작을 수 있는 이유가 이것이다.
 *
 * 플러시를 루프 밖에서 한 번만 하는 점에 유의 — 실제로 고친
 * 개수만큼만 밀어내면 충분하다.
 *
 * 실행 컨텍스트: dt_lock을 잡은 상태.
 *
 * 호출 체인:
 *   rk_iommu_unmap() / rk_iommu_map_iova()의 되돌리기
 *   → [rk_iommu_unmap_iova] → rk_table_flush()
 */
static size_t rk_iommu_unmap_iova(struct rk_iommu_domain *rk_domain,
				  u32 *pte_addr, dma_addr_t pte_dma,
				  size_t size)
{
	/* [한국어] 실제로 무효화한 엔트리의 수. */
	unsigned int pte_count;
	/* [한국어] 요청받은 엔트리의 수. */
	unsigned int pte_total = size / SPAGE_SIZE;

	/* [한국어] 호출자가 테이블 락을 잡았는지 확인한다. */
	assert_spin_locked(&rk_domain->dt_lock);

	/* [한국어] 요청 범위를 앞에서부터 훑는다. */
	for (pte_count = 0; pte_count < pte_total; pte_count++) {
		/* [한국어] 현재 엔트리 값을 읽는다. */
		u32 pte = pte_addr[pte_count];
		/* [한국어] 매핑되지 않은 지점에 닿았다 — 여기까지가
		 * 실제로 해제할 수 있는 범위다. */
		if (!rk_pte_is_page_valid(pte))
			break;

		/* [한국어] 유효 비트만 지운다. 주소와 플래그는 진단을
		 * 위해 남겨 둔다. */
		pte_addr[pte_count] = rk_mk_pte_invalid(pte);
	}

	/* [한국어] 실제로 고친 개수만큼만 하드웨어에 밀어낸다. */
	rk_table_flush(rk_domain, pte_dma, pte_count);

	/* [한국어] 해제한 바이트 수를 돌려준다 — 요청보다 작을 수 있다. */
	return pte_count * SPAGE_SIZE;
}

/*
 * [한국어]
 * rk_iommu_map_iova - 연속된 PTE들을 채운다
 *
 * @rk_domain: 대상 도메인.
 * @pte_addr: 첫 PTE의 가상 주소.
 * @pte_dma: 첫 PTE의 DMA 주소(플러시용).
 * @iova: 매핑할 IOVA의 시작(무효화 범위 계산에 쓴다).
 * @paddr: 물리 주소의 시작.
 * @size: 매핑할 크기.
 * @prot: 요청 권한.
 * @return: 0 성공, -EADDRINUSE(이미 매핑된 자리).
 *
 * 이미 유효한 PTE를 만나면 실패로 처리한다. 덮어쓰지 않는 것이
 * 중요한 정책인데, 그러면 상위 계층의 이중 매핑 버그가 조용히
 * 묻히기 때문이다. 대신 **지금까지 채운 것을 되돌리고**
 * 어떤 주소가 충돌했는지 자세히 찍어 준다.
 *
 * 성공 경로의 마지막에 있는 first_last 무효화가 이 함수의 또 다른
 * 요점이다. 원본 주석이 설명하듯, 새 매핑의 양 끝만 기존 매핑과
 * 캐시 줄을 공유할 수 있어 그 둘만 버리면 충분하다.
 *
 * 실행 컨텍스트: dt_lock을 잡은 상태.
 *
 * 호출 체인:
 *   rk_iommu_map() → [rk_iommu_map_iova] → rk_table_flush()
 *   → rk_iommu_zap_iova_first_last()
 */
static int rk_iommu_map_iova(struct rk_iommu_domain *rk_domain, u32 *pte_addr,
			     dma_addr_t pte_dma, dma_addr_t iova,
			     phys_addr_t paddr, size_t size, int prot)
{
	/* [한국어] 지금까지 채운 엔트리의 수. 실패 시 되돌릴 양이기도 하다. */
	unsigned int pte_count;
	/* [한국어] 채워야 할 총 엔트리 수. */
	unsigned int pte_total = size / SPAGE_SIZE;
	/* [한국어] 충돌한 자리에 이미 매핑되어 있던 물리 주소(진단용). */
	phys_addr_t page_phys;

	/* [한국어] 호출자가 테이블 락을 잡았는지 확인한다. */
	assert_spin_locked(&rk_domain->dt_lock);

	/* [한국어] 요청 범위를 앞에서부터 채운다. */
	for (pte_count = 0; pte_count < pte_total; pte_count++) {
		/* [한국어] 현재 엔트리 값을 읽어 비어 있는지 본다. */
		u32 pte = pte_addr[pte_count];

		/* [한국어] 이미 매핑된 자리다. 덮어쓰지 않고 실패로
		 * 처리해 상위의 버그가 드러나게 한다. */
		if (rk_pte_is_page_valid(pte))
			goto unwind;

		/* [한국어] 물리 주소와 권한으로 엔트리를 만들어 써 넣는다. */
		pte_addr[pte_count] = rk_ops->mk_ptentries(paddr, prot);

		/* [한국어] 다음 페이지로 물리 주소를 진행시킨다.
		 * IOVA는 PTE 배열의 인덱스가 대신하므로 따로 올리지 않는다. */
		paddr += SPAGE_SIZE;
	}

	/* [한국어] 채운 엔트리 전부를 하드웨어에 밀어낸다. */
	rk_table_flush(rk_domain, pte_dma, pte_total);

	/*
	 * Zap the first and last iova to evict from iotlb any previously
	 * mapped cachelines holding stale values for its dte and pte.
	 * We only zap the first and last iova, since only they could have
	 * dte or pte shared with an existing mapping.
	 */
	/* [한국어] 새로 매핑한 범위 자체는 IOTLB에 있을 수 없지만,
	 * 양 끝은 이웃 매핑과 같은 캐시 줄을 공유할 수 있다 —
	 * 그 둘만 버린다. */
	rk_iommu_zap_iova_first_last(rk_domain, iova, size);

	return 0;
/* [한국어] 이미 매핑된 자리를 만났다 — 지금까지 채운 것을 되돌린다. */
unwind:
	/* Unmap the range of iovas that we just mapped */
	/* [한국어] 방금 채운 pte_count개를 무효화한다. 이 함수가
	 * 플러시까지 해 주므로 별도 처리가 필요 없다. */
	rk_iommu_unmap_iova(rk_domain, pte_addr, pte_dma,
			    pte_count * SPAGE_SIZE);

	/* [한국어] 충돌한 지점의 IOVA를 계산한다(진단 출력용). */
	iova += pte_count * SPAGE_SIZE;
	/* [한국어] 그 자리에 이미 매핑되어 있던 물리 주소를 꺼낸다. */
	page_phys = rk_ops->pt_address(pte_addr[pte_count]);
	/* [한국어] 어느 IOVA가 무엇에 매핑되어 있는데 무엇으로 바꾸려
	 * 했는지를 모두 찍는다 — 이중 매핑 버그를 추적하는 데 필요한
	 * 정보가 한 줄에 다 들어 있다. */
	pr_err("iova: %pad already mapped to %pa cannot remap to phys: %pa prot: %#x\n",
	       &iova, &page_phys, &paddr, prot);

	return -EADDRINUSE;	/* [한국어] 이미 매핑된 자리를 덮어쓰지 않았음을 호출자에게 알린다. */
}

/*
 * [한국어]
 * rk_iommu_map - IOVA 범위에 물리 페이지들을 매핑한다
 *
 * @domain: 대상 도메인.
 * @_iova: 매핑할 IOVA의 시작.
 * @paddr: 물리 주소의 시작.
 * @size: 매핑 단위 크기.
 * @count: 개수(이 드라이버는 쓰지 않는다).
 * @prot: 요청 권한.
 * @gfp: 할당 플래그(이 드라이버는 GFP_ATOMIC을 강제한다).
 * @mapped: 매핑된 바이트 수를 돌려줄 곳.
 * @return: 0 성공, 음수 오류.
 *
 * 원본 주석이 이 함수의 단순함을 설명한다. pgsize_bitmap이
 * 4KB~4MB만 허용하고 코어가 정렬을 보장하므로, 요청은 **항상
 * 하나의 DTE 아래에 들어간다.** 그래서 DTE를 넘나드는 루프가 없다.
 *
 * pte_dma 계산이 눈에 띈다. 도메인의 dt에서 읽은 DTE 값을
 * 다시 pt_address로 해석해 페이지 테이블의 주소를 얻는데,
 * 이 값은 DMA 주소와 물리 주소가 같다는 전제 위에 서 있다
 * (이 SoC에서는 성립한다).
 *
 * 실행 컨텍스트: DMA 매핑 경로. dt_lock을 irqsave로 잡는다.
 *
 * 호출 체인:
 *   IOMMU 코어 map_pages → [rk_iommu_map] → rk_dte_get_page_table()
 *   → rk_iommu_map_iova()
 */
static int rk_iommu_map(struct iommu_domain *domain, unsigned long _iova,
			phys_addr_t paddr, size_t size, size_t count,
			int prot, gfp_t gfp, size_t *mapped)
{
	/* [한국어] 대상 도메인. */
	struct rk_iommu_domain *rk_domain = to_rk_domain(domain);
	/* [한국어] 인터럽트 상태 저장용. */
	unsigned long flags;
	/* [한국어] PTE의 DMA 주소와, 타입을 맞춘 IOVA. */
	dma_addr_t pte_dma, iova = (dma_addr_t)_iova;
	/* [한국어] 페이지 테이블과 그 안의 시작 PTE 위치. */
	u32 *page_table, *pte_addr;
	/* [한국어] DTE 값(이름과 달리 인덱스가 아니라 엔트리다)과 PTE 인덱스. */
	u32 dte_index, pte_index;
	/* [한국어] 매핑 결과. */
	int ret;

	/* [한국어] 테이블 갱신 구간을 잠근다. */
	spin_lock_irqsave(&rk_domain->dt_lock, flags);

	/*
	 * pgsize_bitmap specifies iova sizes that fit in one page table
	 * (1024 4-KiB pages = 4 MiB).
	 * So, size will always be 4096 <= size <= 4194304.
	 * Since iommu_map() guarantees that both iova and size will be
	 * aligned, we will always only be mapping from a single dte here.
	 */
	/* [한국어] 이 IOVA가 속한 페이지 테이블을 얻는다(없으면 만든다). */
	page_table = rk_dte_get_page_table(rk_domain, iova);
	if (IS_ERR(page_table)) {
		/* [한국어] 테이블을 확보하지 못했다 — 락을 풀고 오류를 전한다. */
		spin_unlock_irqrestore(&rk_domain->dt_lock, flags);
		return PTR_ERR(page_table);	/* [한국어] 테이블 확보 실패 이유를 그대로 전한다. */
	}

	/* [한국어] 방금 확보된 DTE **값**을 다시 읽는다. 아래에서
	 * 페이지 테이블의 주소를 계산하는 데 쓴다. */
	dte_index = rk_domain->dt[rk_iova_dte_index(iova)];
	/* [한국어] 페이지 테이블 안에서의 시작 위치. */
	pte_index = rk_iova_pte_index(iova);
	pte_addr = &page_table[pte_index];

	/* [한국어] 플러시 대상이 될 첫 PTE의 DMA 주소를 구한다.
	 * DTE에서 테이블 주소를 복원하고 엔트리 오프셋을 더한다. */
	pte_dma = rk_ops->pt_address(dte_index) + pte_index * sizeof(u32);
	/* [한국어] 실제 엔트리 채우기와 무효화를 위임한다. */
	ret = rk_iommu_map_iova(rk_domain, pte_addr, pte_dma, iova,
				paddr, size, prot);

	spin_unlock_irqrestore(&rk_domain->dt_lock, flags);
	/* [한국어] 성공했으면 요청 전량이 매핑된 것이다. */
	if (!ret)
		*mapped = size;

	return ret;
}

/*
 * [한국어]
 * rk_iommu_unmap - IOVA 범위의 매핑을 해제한다
 *
 * @domain: 대상 도메인.
 * @_iova: 해제할 IOVA의 시작.
 * @size: 해제할 크기.
 * @count: 개수(쓰지 않는다).
 * @gather: 무효화 모으기(이 드라이버는 쓰지 않는다).
 * @return: 실제로 해제한 바이트 수.
 *
 * map과 마찬가지로 하나의 DTE 아래에서만 동작한다. DTE가 무효면
 * 애초에 매핑된 적이 없으므로 0을 돌려준다.
 *
 * 무효화를 **락 밖에서** 하는 점이 중요하다. zap은 여러 IOMMU의
 * 전원을 확인하고 클럭을 켰다 끄는 느린 작업이라, 그동안 테이블
 * 락을 쥐고 있으면 다른 매핑이 모두 멈춘다.
 *
 * gather를 쓰지 않고 즉시 무효화하는 이유: 이 하드웨어에는 지연
 * 무효화를 안전하게 처리할 만한 장치가 없고, ZAP_ONE_LINE이
 * 충분히 값싸기 때문이다.
 *
 * 실행 컨텍스트: DMA 해제 경로.
 *
 * 호출 체인:
 *   IOMMU 코어 unmap_pages → [rk_iommu_unmap] → rk_iommu_unmap_iova()
 *   → rk_iommu_zap_iova()
 */
static size_t rk_iommu_unmap(struct iommu_domain *domain, unsigned long _iova,
			     size_t size, size_t count, struct iommu_iotlb_gather *gather)
{
	/* [한국어] 대상 도메인. */
	struct rk_iommu_domain *rk_domain = to_rk_domain(domain);
	/* [한국어] 인터럽트 상태 저장용. */
	unsigned long flags;
	/* [한국어] PTE의 DMA 주소와, 타입을 맞춘 IOVA. */
	dma_addr_t pte_dma, iova = (dma_addr_t)_iova;
	/* [한국어] 페이지 테이블의 물리 주소. */
	phys_addr_t pt_phys;
	/* [한국어] 읽어 온 DTE 값. */
	u32 dte;
	/* [한국어] 해제를 시작할 PTE의 주소. */
	u32 *pte_addr;
	/* [한국어] 실제로 해제된 크기. */
	size_t unmap_size;

	/* [한국어] 테이블 갱신 구간을 잠근다. */
	spin_lock_irqsave(&rk_domain->dt_lock, flags);

	/*
	 * pgsize_bitmap specifies iova sizes that fit in one page table
	 * (1024 4-KiB pages = 4 MiB).
	 * So, size will always be 4096 <= size <= 4194304.
	 * Since iommu_unmap() guarantees that both iova and size will be
	 * aligned, we will always only be unmapping from a single dte here.
	 */
	/* [한국어] 이 IOVA가 속한 DTE를 읽는다. */
	dte = rk_domain->dt[rk_iova_dte_index(iova)];
	/* Just return 0 if iova is unmapped */
	/* [한국어] 페이지 테이블이 없으면 이 4MB 영역에 매핑이 없었다. */
	if (!rk_dte_is_pt_valid(dte)) {
		spin_unlock_irqrestore(&rk_domain->dt_lock, flags);	/* [한국어] 락을 풀고 나간다. */
		return 0;	/* [한국어] 매핑된 적이 없으므로 해제한 양도 0이다. */
	}

	/* [한국어] DTE에서 페이지 테이블의 주소를 복원한다. */
	pt_phys = rk_ops->pt_address(dte);
	/* [한국어] 해제를 시작할 PTE의 가상 주소. */
	pte_addr = (u32 *)phys_to_virt(pt_phys) + rk_iova_pte_index(iova);
	/* [한국어] 같은 위치의 DMA 주소(플러시용). 이 SoC에서는 물리
	 * 주소와 DMA 주소가 같다는 전제가 성립한다. */
	pte_dma = pt_phys + rk_iova_pte_index(iova) * sizeof(u32);
	/* [한국어] 엔트리들을 무효화하고 실제 해제된 크기를 받는다. */
	unmap_size = rk_iommu_unmap_iova(rk_domain, pte_addr, pte_dma, size);

	/* [한국어] 테이블 갱신이 끝났으니 락을 놓는다. */
	spin_unlock_irqrestore(&rk_domain->dt_lock, flags);

	/* Shootdown iotlb entries for iova range that was just unmapped */
	/* [한국어] 무효화는 락 밖에서 한다 — 전원 확인과 클럭 조작이
	 * 느려서, 그동안 테이블을 잠가 두면 다른 매핑이 모두 막힌다.
	 * 실제로 해제된 범위만 버리는 점에도 유의. */
	rk_iommu_zap_iova(rk_domain, iova, unmap_size);

	return unmap_size;	/* [한국어] 실제로 해제된 바이트 수를 코어에 알린다. */
}

/*
 * [한국어]
 * rk_iommu_from_dev - 마스터 디바이스에서 담당 IOMMU를 꺼낸다
 *
 * @dev: 마스터 디바이스.
 * @return: 담당 IOMMU, 연결 정보가 없으면 NULL.
 *
 * NULL을 돌려줄 수 있다는 점이 중요하다. DRM 같은 "가상 디바이스"는
 * 자기 IOMMU가 없는데도 도메인에 붙었다 떨어지려 할 수 있어,
 * attach/detach가 이 결과로 그런 경우를 걸러 낸다.
 *
 * 실행 컨텍스트: 모든 디바이스 콜백.
 *
 * 호출 체인:
 *   attach/detach/probe_device/domain_alloc → [rk_iommu_from_dev]
 */
static struct rk_iommu *rk_iommu_from_dev(struct device *dev)
{
	/* [한국어] of_xlate가 매달아 둔 연결 정보. */
	struct rk_iommudata *data = dev_iommu_priv_get(dev);

	/* [한국어] 연결 정보가 없으면 이 디바이스에는 IOMMU가 없다. */
	return data ? data->iommu : NULL;
}

/* Must be called with iommu powered on and attached */
/*
 * [한국어]
 * rk_iommu_disable - IOMMU의 변환을 완전히 끈다
 *
 * @iommu: 대상 IOMMU 블록.
 * @return: 없음.
 *
 * 순서가 전부다: 클럭을 켜고 → stall을 걸어 명령을 받을 수 있게
 * 하고 → 페이징을 끄고 → 인터럽트와 테이블 주소를 지우고 →
 * stall을 풀고 → 클럭을 내린다.
 *
 * DTE_ADDR을 0으로 지우는 것이 특히 중요하다. 이후 이 IOMMU가
 * 우연히 켜지더라도 가리킬 테이블이 없어 아무것도 변환하지 않는다.
 * 해제된 도메인의 테이블을 계속 가리키는 상태를 남기지 않는 것이다.
 *
 * 오류를 무시하고 끝까지 진행하는 이유: 끄는 도중에 멈추면
 * 더 나쁜 상태가 남기 때문이다. 원본 주석이 그것을 명시한다.
 *
 * 실행 컨텍스트: 전원이 켜진 상태여야 한다(호출자가 보장).
 *
 * 호출 체인:
 *   rk_iommu_identity_attach() / rk_iommu_suspend() → [rk_iommu_disable]
 */
static void rk_iommu_disable(struct rk_iommu *iommu)
{
	/* [한국어] 인스턴스 순회 인덱스. */
	int i;

	/* Ignore error while disabling, just keep going */
	/* [한국어] 레지스터를 만지려면 클럭이 필요하다. 실패해도
	 * 계속 진행한다 — 끄다 만 상태가 더 위험하다. */
	WARN_ON(clk_bulk_enable(iommu->num_clocks, iommu->clocks));
	/* [한국어] 명령을 받을 수 있도록 변환을 멈춘다. */
	rk_iommu_enable_stall(iommu);
	/* [한국어] 변환을 끈다 — 이후 마스터의 DMA는 그대로 나간다. */
	rk_iommu_disable_paging(iommu);
	/* [한국어] 인스턴스마다 인터럽트와 테이블 주소를 지운다. */
	for (i = 0; i < iommu->num_mmu; i++) {
		/* [한국어] 인터럽트를 막는다 — 꺼진 IOMMU가 인터럽트를
		 * 내면 핸들러가 전원 확인에 걸려 아무 일도 못 한다. */
		rk_iommu_write(iommu->bases[i], RK_MMU_INT_MASK, 0);
		/* [한국어] 테이블 주소를 지운다. 해제될 도메인의 테이블을
		 * 계속 가리키는 상태를 남기지 않기 위함이다. */
		rk_iommu_write(iommu->bases[i], RK_MMU_DTE_ADDR, 0);
	}
	/* [한국어] stall을 푼다. 페이징이 꺼져 있으므로 변환이 되살아나지는 않는다. */
	rk_iommu_disable_stall(iommu);
	/* [한국어] 클럭을 내린다. */
	clk_bulk_disable(iommu->num_clocks, iommu->clocks);
}

/* Must be called with iommu powered on and attached */
/*
 * [한국어]
 * rk_iommu_enable - 도메인의 테이블을 설치하고 변환을 켠다
 *
 * @iommu: 대상 IOMMU 블록(iommu->domain이 이미 설정되어 있어야 한다).
 * @return: 0 성공, 음수 오류.
 *
 * disable의 역순이지만 중간에 **강제 리셋**이 끼어 있는 것이
 * 다르다. 이전 설정의 잔재를 지우고 깨끗한 상태에서 시작하려는
 * 것이다.
 *
 * 인스턴스마다 세 가지를 설정한다: 테이블 주소, IOTLB 비우기,
 * 인터럽트 활성화. 순서가 중요한데, 테이블을 걸기 전에 캐시를
 * 비우면 옛 항목이 남고, 인터럽트를 먼저 켜면 아직 준비되지 않은
 * 상태에서 폴트를 받을 수 있다.
 *
 * 오류 처리가 goto 사다리로 되어 있어, 어느 단계에서 실패하든
 * 그때까지 켠 것만 정확히 되돌린다.
 *
 * 실행 컨텍스트: 전원이 켜진 상태(호출자가 보장).
 *
 * 호출 체인:
 *   rk_iommu_attach_device() / rk_iommu_resume() → [rk_iommu_enable]
 *   → rk_iommu_force_reset() → rk_iommu_enable_paging()
 */
static int rk_iommu_enable(struct rk_iommu *iommu)
{
	/* [한국어] 붙어 있는 도메인. attach가 미리 설정해 둔다. */
	struct iommu_domain *domain = iommu->domain;
	/* [한국어] 그 도메인의 드라이버 쪽 상태(테이블 주소를 안다). */
	struct rk_iommu_domain *rk_domain = to_rk_domain(domain);
	/* [한국어] 단계별 결과와 인스턴스 인덱스. */
	int ret, i;

	/* [한국어] 레지스터를 만지려면 클럭부터 켜야 한다. */
	ret = clk_bulk_enable(iommu->num_clocks, iommu->clocks);
	if (ret)	/* [한국어] 클럭을 켜지 못하면 레지스터를 만질 수 없다. */
		return ret;

	/* [한국어] 명령을 받을 수 있도록 변환을 멈춘다.
	 * 이미 꺼져 있다면 곧바로 성공으로 돌아온다. */
	ret = rk_iommu_enable_stall(iommu);
	if (ret)	/* [한국어] 변환을 멈추지 못했으면 명령이 먹히지 않는다. */
		goto out_disable_clocks;

	/* [한국어] 이전 설정의 잔재를 지운다. 레지스터가 살아 있는지도
	 * 여기서 함께 검증된다. */
	ret = rk_iommu_force_reset(iommu);
	if (ret)	/* [한국어] 리셋이 실패했다 — 깨끗한 상태를 보장할 수 없다. */
		goto out_disable_stall;

	/* [한국어] 인스턴스마다 테이블을 걸고 준비시킨다. */
	for (i = 0; i < iommu->num_mmu; i++) {
		/* [한국어] 디렉터리 테이블의 DMA 주소를 이 세대의 엔트리
		 * 형식으로 만들어 써 넣는다. 모든 인스턴스가 같은 테이블을
		 * 공유한다. */
		rk_iommu_write(iommu->bases[i], RK_MMU_DTE_ADDR,
			       rk_ops->mk_dtentries(rk_domain->dt_dma));
		/* [한국어] IOTLB를 비운다 — 리셋 전의 항목이 남아 있으면
		 * 새 테이블과 어긋난다. */
		rk_iommu_base_command(iommu->bases[i], RK_MMU_CMD_ZAP_CACHE);
		/* [한국어] 폴트와 버스 오류 인터럽트를 켠다. 테이블이
		 * 준비된 뒤여야 안전하다. */
		rk_iommu_write(iommu->bases[i], RK_MMU_INT_MASK, RK_MMU_IRQ_MASK);
	}

	/* [한국어] 마지막으로 변환을 켠다. 이 순간부터 마스터의 DMA가
	 * 테이블을 거친다. */
	ret = rk_iommu_enable_paging(iommu);

/* [한국어] 리셋 이후의 실패는 stall부터 풀고 나간다. */
out_disable_stall:
	rk_iommu_disable_stall(iommu);
/* [한국어] stall 실패나 그 이후의 실패는 모두 클럭을 내리고 나간다. */
out_disable_clocks:
	clk_bulk_disable(iommu->num_clocks, iommu->clocks);	/* [한국어] 레지스터 작업이 끝났으니 클럭을 내린다. */
	return ret;	/* [한국어] 어느 단계에서 멈췄든 그 결과를 전한다. */
}

/*
 * [한국어]
 * rk_iommu_identity_attach - IOMMU를 도메인에서 떼어 내 변환을 끈다
 *
 * @identity_domain: 전역 identity 도메인.
 * @dev: 대상 디바이스.
 * @old: 직전 도메인(이 드라이버는 iommu->domain을 쓴다).
 * @return: 0 성공, -ENODEV(IOMMU가 없는 디바이스).
 *
 * 이 하드웨어에 통과 모드가 없으므로, identity에 붙는다는 것은
 * **변환을 끄는 일**이다. 그래서 이 함수가 곧 detach 구현이기도 하다.
 *
 * 순서가 중요하다:
 *  1) 먼저 iommu->domain을 identity로 바꾼다. 이 시점부터 인터럽트
 *     핸들러가 폴트를 옛 도메인에 보고하지 않는다.
 *  2) 도메인의 목록에서 뺀다. 이후 무효화가 이 IOMMU를 건드리지 않는다.
 *  3) **전원이 켜져 있을 때만** 하드웨어를 끈다. 꺼져 있다면
 *     이미 변환이 죽어 있고, 다시 켜질 때 resume이 identity임을
 *     보고 아무것도 하지 않으므로 안전하다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 attach_dev(identity) / rk_iommu_attach_device()
 *   → [rk_iommu_identity_attach] → rk_iommu_disable()
 */
static int rk_iommu_identity_attach(struct iommu_domain *identity_domain,
				    struct device *dev,
				    struct iommu_domain *old)
{
	/* [한국어] 대상 IOMMU. */
	struct rk_iommu *iommu;
	/* [한국어] 현재 붙어 있는 도메인의 드라이버 쪽 상태. */
	struct rk_iommu_domain *rk_domain;
	/* [한국어] 인터럽트 상태 저장용. */
	unsigned long flags;
	/* [한국어] 전원 상태 조회 결과. */
	int ret;

	/* Allow 'virtual devices' (eg drm) to detach from domain */
	/* [한국어] IOMMU가 없는 가상 디바이스라면 뗄 것도 없다. */
	iommu = rk_iommu_from_dev(dev);
	if (!iommu)	/* [한국어] IOMMU가 없는 가상 디바이스다 — 뗄 것이 없다. */
		return -ENODEV;

	/* [한국어] 지금 붙어 있는 도메인을 복원한다. identity 도메인이라면
	 * 이 변환 결과는 쓰이지 않고 아래에서 곧바로 돌아간다. */
	rk_domain = to_rk_domain(iommu->domain);

	dev_dbg(dev, "Detaching from iommu domain\n");

	/* [한국어] 이미 떼어진 상태면 할 일이 없다. */
	if (iommu->domain == identity_domain)
		return 0;

	/* [한국어] 먼저 장부를 바꾼다 — 이 순간부터 인터럽트 핸들러가
	 * 폴트를 옛 도메인에 보고하지 않는다. */
	iommu->domain = identity_domain;

	/* [한국어] 옛 도메인의 IOMMU 목록에서 뺀다. 이후 그 도메인의
	 * 무효화가 이 IOMMU를 건드리지 않는다. */
	spin_lock_irqsave(&rk_domain->iommus_lock, flags);
	list_del_init(&iommu->node);	/* [한국어] RCU가 아닌 일반 목록이라 락 아래에서 뺀다. init을 붙여 재제거가 안전하다. */
	spin_unlock_irqrestore(&rk_domain->iommus_lock, flags);

	/* [한국어] 전원이 켜져 있을 때만 하드웨어를 끈다. */
	ret = pm_runtime_get_if_in_use(iommu->dev);
	WARN_ON_ONCE(ret < 0);	/* [한국어] PM 자체의 오류는 있어서는 안 될 상황이다. */
	if (ret > 0) {
		/* [한국어] 켜져 있다 — 변환을 끄고 테이블 주소를 지운다. */
		rk_iommu_disable(iommu);
		pm_runtime_put(iommu->dev);	/* [한국어] 전원 참조를 놓아 다시 잠들 수 있게 한다. */
	}

	/* [한국어] 꺼져 있었다면 아무것도 하지 않는다. 이미 변환이 죽어
	 * 있고, 다시 켜질 때 resume이 identity를 보고 그냥 돌아간다. */
	return 0;
}

/* [한국어] identity 도메인의 연산 테이블. 붙이기 하나뿐이며,
 * 그 붙이기가 사실은 "떼어 내기"다. */
static struct iommu_domain_ops rk_identity_ops = {
	.attach_dev = rk_iommu_identity_attach,
	/* [한국어] 이 도메인으로 옮길 때 부를 콜백.
	 * 읽는 자: IOMMU 코어와, 이 파일의 attach_device. */
};

/* [한국어] "어느 도메인에도 붙어 있지 않음"을 나타내는 전역 정적 도메인.
 * 앞에서 전방 선언된 그 변수이며, probe가 모든 IOMMU를 여기에
 * 놓고 시작한다. 매핑도 테이블도 없다 — 그저 페이징이 꺼진 상태다. */
static struct iommu_domain rk_identity_domain = {
	.type = IOMMU_DOMAIN_IDENTITY,
	/* [한국어] 코어에게 통과 모드로 알리는 종류 표시.
	 * 읽는 자: 코어의 도메인 판별과, 인터럽트/suspend/resume의
	 *          "붙어 있는가" 검사. */

	.ops = &rk_identity_ops,
	/* [한국어] 위에서 정의한 연산 테이블. */
};

/*
 * [한국어]
 * rk_iommu_attach_device - 디바이스를 도메인에 붙이고 변환을 켠다
 *
 * @domain: 붙일 도메인.
 * @dev: 대상 디바이스.
 * @old: 직전 도메인.
 * @return: 0 성공, 음수 오류.
 *
 * "먼저 떼고 다시 붙인다"는 순서를 지킨다. 이 하드웨어는 한
 * IOMMU가 한 테이블만 가리킬 수 있어, 도메인 전환은 반드시
 * 완전한 정리를 거쳐야 한다.
 *
 * 목록에 먼저 넣고 하드웨어를 나중에 켜는 순서에 유의. 그 사이에
 * 무효화가 들어오면 아직 켜지지 않은 IOMMU에 zap을 시도하는데,
 * zap이 전원을 확인하고 켜져 있으면 레지스터에 쓸 뿐이라 무해하다.
 * 반대로 하드웨어를 먼저 켜면 목록에 없는 동안의 무효화를 놓친다.
 *
 * 전원이 꺼져 있으면 하드웨어 설정을 **하지 않고 그냥 성공**한다.
 * 나중에 깨어날 때 rk_iommu_resume()이 iommu->domain을 보고
 * enable을 대신 수행하기 때문이다. 이 지연 설정이 런타임 PM과
 * 맞물린 이 드라이버의 핵심 패턴이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 attach_dev → [rk_iommu_attach_device]
 *   → rk_iommu_identity_attach() → rk_iommu_enable()
 */
static int rk_iommu_attach_device(struct iommu_domain *domain,
				  struct device *dev, struct iommu_domain *old)
{
	/* [한국어] 대상 IOMMU. */
	struct rk_iommu *iommu;
	/* [한국어] 붙일 도메인의 드라이버 쪽 상태. */
	struct rk_iommu_domain *rk_domain = to_rk_domain(domain);
	/* [한국어] 인터럽트 상태 저장용. */
	unsigned long flags;
	/* [한국어] 단계별 결과. */
	int ret;

	/*
	 * Allow 'virtual devices' (e.g., drm) to attach to domain.
	 * Such a device does not belong to an iommu group.
	 */
	/* [한국어] IOMMU가 없는 가상 디바이스는 조용히 성공시킨다 —
	 * 붙일 하드웨어가 없을 뿐 오류는 아니다. */
	iommu = rk_iommu_from_dev(dev);
	if (!iommu)	/* [한국어] IOMMU가 없는 가상 디바이스는 조용히 성공시킨다. */
		return 0;

	dev_dbg(dev, "Attaching to iommu domain\n");	/* [한국어] 전환을 추적할 수 있게 흔적을 남긴다. */

	/* iommu already attached */
	/* [한국어] 같은 도메인에 다시 붙이는 요청은 무시한다. */
	if (iommu->domain == domain)
		return 0;

	/* [한국어] 먼저 완전히 떼어 낸다. 이 하드웨어는 테이블을 하나만
	 * 가리킬 수 있어, 전환은 깨끗한 상태에서 시작해야 한다. */
	ret = rk_iommu_identity_attach(&rk_identity_domain, dev, old);
	if (ret)	/* [한국어] 떼어 내기에 실패하면 새로 붙일 수도 없다. */
		return ret;

	/* [한국어] 새 도메인을 장부에 기록한다. enable이 이 값을 보고
	 * 어느 테이블을 걸지 정한다. */
	iommu->domain = domain;

	/* [한국어] 도메인의 IOMMU 목록에 넣는다. 이제부터 이 도메인의
	 * 무효화가 이 IOMMU에도 전달된다. */
	spin_lock_irqsave(&rk_domain->iommus_lock, flags);
	list_add_tail(&iommu->node, &rk_domain->iommus);	/* [한국어] 이 도메인의 무효화 대상 목록에 넣는다. */
	spin_unlock_irqrestore(&rk_domain->iommus_lock, flags);

	/* [한국어] 전원이 꺼져 있으면 지금 설정할 수 없다. 그래도 성공이다 —
	 * 깨어날 때 resume이 iommu->domain을 보고 enable을 수행한다. */
	ret = pm_runtime_get_if_in_use(iommu->dev);
	if (!ret || WARN_ON_ONCE(ret < 0))	/* [한국어] 전원이 꺼져 있으면 지금 설정하지 않는다 — resume이 대신 한다. */
		return 0;

	/* [한국어] 켜져 있으니 지금 바로 테이블을 걸고 변환을 켠다. */
	ret = rk_iommu_enable(iommu);
	if (ret) {	/* [한국어] 하드웨어를 켜는 데 실패한 경우. */
		/*
		 * Note rk_iommu_identity_attach() might fail before physically
		 * attaching the dev to iommu->domain, in which case the actual
		 * old domain for this revert should be rk_identity_domain v.s.
		 * iommu->domain. Since rk_iommu_identity_attach() does not care
		 * about the old domain argument for now, this is not a problem.
		 */
		/* [한국어] 켜기에 실패했다 — 다시 떼어 내 변환이 꺼진
		 * 안전한 상태로 되돌린다. 원본 주석이 밝히듯 old 인자가
		 * 정확하지 않을 수 있지만, 이 구현은 그것을 쓰지 않는다. */
		WARN_ON(rk_iommu_identity_attach(&rk_identity_domain, dev,
						 iommu->domain));
	}

	/* [한국어] 전원 참조를 놓는다. */
	pm_runtime_put(iommu->dev);

	return ret;	/* [한국어] 켜기 결과를 코어에 전한다. */
}

/*
 * [한국어]
 * rk_iommu_domain_alloc_paging - 페이징 도메인을 만든다
 *
 * @dev: 이 도메인을 쓸 디바이스(DMA 매핑의 기준이 된다).
 * @return: 새 도메인, 실패하면 NULL.
 *
 * 디렉터리 테이블 한 페이지를 잡고 그것을 DMA 매핑하는 것이
 * 이 함수의 전부다. 페이지 테이블은 매핑이 실제로 들어올 때
 * rk_dte_get_page_table()이 그때그때 만든다.
 *
 * dma_dev를 dev가 아니라 **IOMMU의 dev**로 잡는 점에 유의.
 * 테이블을 읽는 주체가 마스터가 아니라 IOMMU 하드웨어이기 때문이다.
 *
 * aperture가 32비트 전체인 것은 IOVA 형식이 그렇게 정의되어
 * 있어서다 — 10 + 10 + 12비트로 정확히 32비트를 채운다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(GFP_KERNEL).
 *
 * 호출 체인:
 *   IOMMU 코어 domain_alloc_paging → [rk_iommu_domain_alloc_paging]
 */
static struct iommu_domain *rk_iommu_domain_alloc_paging(struct device *dev)
{
	/* [한국어] 만들 도메인. */
	struct rk_iommu_domain *rk_domain;
	/* [한국어] DMA 매핑의 기준이 될 IOMMU. */
	struct rk_iommu *iommu;

	/* [한국어] 도메인 뼈대를 0으로 초기화해 받는다. */
	rk_domain = kzalloc_obj(*rk_domain);
	if (!rk_domain)	/* [한국어] 도메인 구조체를 잡지 못했다. */
		return NULL;

	/*
	 * rk32xx iommus use a 2 level pagetable.
	 * Each level1 (dt) and level2 (pt) table has 1024 4-byte entries.
	 * Allocate one 4 KiB page for each table.
	 */
	/* [한국어] 디렉터리 테이블 한 페이지를 잡는다. 4KB 정렬이
	 * 보장되고, v1이면 GFP_DMA32가 32비트 아래를 강제한다.
	 * 0으로 초기화되므로 모든 DTE가 무효 상태로 시작한다. */
	rk_domain->dt = iommu_alloc_pages_sz(GFP_KERNEL | rk_ops->gfp_flags,
					     SPAGE_SIZE);
	if (!rk_domain->dt)	/* [한국어] 디렉터리 테이블 페이지를 잡지 못했다. */
		goto err_free_domain;

	/* [한국어] 테이블을 읽는 주체는 IOMMU 하드웨어이므로 DMA 매핑의
	 * 기준도 IOMMU의 디바이스여야 한다. */
	iommu = rk_iommu_from_dev(dev);
	rk_domain->dma_dev = iommu->dev;
	/* [한국어] 디렉터리 테이블을 DMA 매핑한다. 여기서 얻은 주소가
	 * DTE_ADDR 레지스터에 들어가고, 플러시의 기준점도 된다. */
	rk_domain->dt_dma = dma_map_single(rk_domain->dma_dev, rk_domain->dt,
					   SPAGE_SIZE, DMA_TO_DEVICE);
	if (dma_mapping_error(rk_domain->dma_dev, rk_domain->dt_dma)) {	/* [한국어] 하드웨어가 읽을 주소를 얻지 못했다. */
		dev_err(rk_domain->dma_dev, "DMA map error for DT\n");	/* [한국어] 어느 단계에서 실패했는지 알 수 있게 남긴다. */
		goto err_free_dt;	/* [한국어] 잡아 둔 테이블 페이지를 되돌리러 간다. */
	}

	/* [한국어] IOMMU 목록을 보호할 락. */
	spin_lock_init(&rk_domain->iommus_lock);
	/* [한국어] 테이블 갱신을 보호할 락. */
	spin_lock_init(&rk_domain->dt_lock);
	/* [한국어] 아직 아무 IOMMU도 붙어 있지 않다. */
	INIT_LIST_HEAD(&rk_domain->iommus);

	/* [한국어] 4KB~4MB의 모든 2의 거듭제곱을 지원한다고 알린다.
	 * 이 값이 map/unmap을 하나의 DTE 안으로 묶어 준다. */
	rk_domain->domain.pgsize_bitmap = RK_IOMMU_PGSIZE_BITMAP;

	/* [한국어] IOVA 공간은 0부터 시작한다. */
	rk_domain->domain.geometry.aperture_start = 0;
	/* [한국어] 32비트 전체를 덮는다 — IOVA 형식이 정확히 그만큼이다. */
	rk_domain->domain.geometry.aperture_end   = DMA_BIT_MASK(32);
	/* [한국어] 코어가 이 범위 밖의 IOVA를 주지 않도록 강제한다. */
	rk_domain->domain.geometry.force_aperture = true;

	/* [한국어] 코어에는 임베드된 부분만 돌려준다. */
	return &rk_domain->domain;

/* [한국어] DMA 매핑에 실패했다 — 잡아 둔 테이블 페이지를 반납한다. */
err_free_dt:
	iommu_free_pages(rk_domain->dt);
/* [한국어] 테이블조차 못 잡았거나 위에서 내려온 경우 — 도메인을 해제한다. */
err_free_domain:
	kfree(rk_domain);	/* [한국어] 도메인 구조체를 해제한다. */

	return NULL;	/* [한국어] 생성 실패를 코어에 알린다. */
}

/*
 * [한국어]
 * rk_iommu_domain_free - 도메인과 그 아래 모든 테이블을 반납한다
 *
 * @domain: 해제할 도메인.
 * @return: 없음.
 *
 * 디렉터리의 1024개 엔트리를 훑으며 매달린 페이지 테이블을 모두
 * 반납한 뒤, 디렉터리 자신을 반납한다. 각 테이블마다 DMA 매핑도
 * 함께 풀어야 한다 — 만들 때 매핑했으므로 대칭이다.
 *
 * dma_unmap_single에 pt_phys를 넘기는 점에 유의. DMA 주소를
 * 따로 저장해 두지 않고 물리 주소를 그대로 쓰는데, 이 SoC에서
 * 둘이 같기 때문에 성립한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 free → [rk_iommu_domain_free]
 */
static void rk_iommu_domain_free(struct iommu_domain *domain)
{
	/* [한국어] 해제할 도메인. */
	struct rk_iommu_domain *rk_domain = to_rk_domain(domain);
	/* [한국어] 디렉터리 순회 인덱스. */
	int i;

	/* [한국어] 아직 IOMMU가 붙어 있다면 상위 계층이 detach를
	 * 빠뜨린 것이다 — 경고하되 해제는 진행한다. */
	WARN_ON(!list_empty(&rk_domain->iommus));

	/* [한국어] 1024개 DTE를 모두 훑는다. */
	for (i = 0; i < NUM_DT_ENTRIES; i++) {
		/* [한국어] 이번 엔트리 값. */
		u32 dte = rk_domain->dt[i];
		/* [한국어] 페이지 테이블이 매달려 있는 자리만 정리한다. */
		if (rk_dte_is_pt_valid(dte)) {
			/* [한국어] 엔트리에서 테이블의 물리 주소를 복원한다. */
			phys_addr_t pt_phys = rk_ops->pt_address(dte);
			/* [한국어] 반납할 커널 가상 주소. */
			u32 *page_table = phys_to_virt(pt_phys);
			/* [한국어] 만들 때의 DMA 매핑을 푼다. 물리 주소를
			 * 그대로 쓰는 것은 이 SoC에서 DMA 주소와 같기 때문이다. */
			dma_unmap_single(rk_domain->dma_dev, pt_phys,
					 SPAGE_SIZE, DMA_TO_DEVICE);
			/* [한국어] 페이지를 반납한다. */
			iommu_free_pages(page_table);
		}
	}

	/* [한국어] 디렉터리 테이블의 DMA 매핑을 푼다. */
	dma_unmap_single(rk_domain->dma_dev, rk_domain->dt_dma,
			 SPAGE_SIZE, DMA_TO_DEVICE);
	/* [한국어] 디렉터리 페이지를 반납한다. */
	iommu_free_pages(rk_domain->dt);

	/* [한국어] 마지막으로 도메인 구조체를 해제한다. */
	kfree(rk_domain);
}

/*
 * [한국어]
 * rk_iommu_probe_device - 디바이스를 이 IOMMU에 등록한다
 *
 * @dev: 검사할 디바이스.
 * @return: 담당 iommu_device, 담당하지 않으면 ERR_PTR(-ENODEV).
 *
 * 하는 일은 사실상 **디바이스 링크를 만드는 것** 하나다.
 * DL_FLAG_PM_RUNTIME이 핵심인데, 이것이 있어야 마스터가 깨어날 때
 * IOMMU가 먼저 깨어난다. 그 순서가 보장되지 않으면 마스터의
 * 첫 DMA가 아직 설정되지 않은 IOMMU를 만나게 된다.
 *
 * DL_FLAG_STATELESS는 링크의 수명을 드라이버 바인딩과 묶지 않고
 * 이 드라이버가 직접 관리하겠다는 뜻이다 — release_device가
 * 명시적으로 지운다.
 *
 * 실행 컨텍스트: 디바이스 probe. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 probe_device → [rk_iommu_probe_device]
 */
static struct iommu_device *rk_iommu_probe_device(struct device *dev)
{
	/* [한국어] of_xlate가 매달아 둔 연결 정보. */
	struct rk_iommudata *data;
	/* [한국어] 담당 IOMMU. */
	struct rk_iommu *iommu;

	/* [한국어] 연결 정보가 없으면 이 디바이스는 우리 담당이 아니다. */
	data = dev_iommu_priv_get(dev);
	if (!data)	/* [한국어] 연결 정보가 없으면 우리 담당이 아니다. */
		return ERR_PTR(-ENODEV);

	/* [한국어] 연결 정보에서 IOMMU를 꺼낸다. */
	iommu = rk_iommu_from_dev(dev);

	/* [한국어] 마스터가 깨어날 때 IOMMU가 먼저 깨어나도록 전원
	 * 의존을 만든다. 이 링크가 없으면 마스터의 첫 DMA가 설정되지
	 * 않은 IOMMU를 만난다. */
	data->link = device_link_add(dev, iommu->dev,
				     DL_FLAG_STATELESS | DL_FLAG_PM_RUNTIME);

	/* [한국어] 이 디바이스를 담당하는 IOMMU 인스턴스를 코어에 알린다. */
	return &iommu->iommu;
}

/*
 * [한국어]
 * rk_iommu_release_device - 디바이스를 이 IOMMU에서 걷어낸다
 *
 * @dev: 대상 디바이스.
 * @return: 없음.
 *
 * probe_device가 만든 전원 의존 링크를 지운다. rk_iommudata 자체는
 * of_xlate가 devm으로 잡았으므로 여기서 해제하지 않는다.
 *
 * 실행 컨텍스트: 디바이스 제거. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 release_device → [rk_iommu_release_device]
 */
static void rk_iommu_release_device(struct device *dev)
{
	/* [한국어] 연결 정보에서 링크를 꺼낸다. */
	struct rk_iommudata *data = dev_iommu_priv_get(dev);

	/* [한국어] 전원 의존을 끊는다. STATELESS로 만들었으므로
	 * 이렇게 명시적으로 지워야 한다. */
	device_link_del(data->link);
}

/*
 * [한국어]
 * rk_iommu_of_xlate - 디바이스 트리의 iommus 참조를 연결 정보로 바꾼다
 *
 * @dev: 마스터 디바이스.
 * @args: "iommus = <&vpu_mmu>"에서 파싱된 참조.
 * @return: 0 성공, -ENOMEM.
 *
 * 다른 드라이버들이 스트림 ID를 등록하는 자리에서, 이 드라이버는
 * **IOMMU 포인터 자체를 매단다.** Rockchip의 IOMMU는 마스터마다
 * 하나씩 붙어 있어 식별자가 필요 없기 때문이다.
 *
 * 연결 정보를 마스터가 아니라 **IOMMU 디바이스의 devm**으로 잡는
 * 점이 흥미롭다. 마스터보다 IOMMU가 오래 살기 때문에 수명 관리가
 * 단순해진다.
 *
 * 실행 컨텍스트: 디바이스 트리 파싱. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 of_xlate → [rk_iommu_of_xlate]
 */
static int rk_iommu_of_xlate(struct device *dev,
			     const struct of_phandle_args *args)
{
	/* [한국어] 참조가 가리키는 IOMMU의 플랫폼 디바이스. */
	struct platform_device *iommu_dev;
	/* [한국어] 마스터에 매달 연결 정보. */
	struct rk_iommudata *data;

	/* [한국어] 디바이스 트리 노드로 IOMMU 디바이스를 찾는다.
	 * 이 호출이 참조 계수를 올리므로 아래에서 내려야 한다. */
	iommu_dev = of_find_device_by_node(args->np);

	/* [한국어] IOMMU 디바이스의 수명에 묶어 할당한다 — 마스터보다
	 * IOMMU가 오래 살기 때문에 이쪽이 안전하다. */
	data = devm_kzalloc(&iommu_dev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)	/* [한국어] 연결 정보를 잡지 못했다. */
		return -ENOMEM;

	/* [한국어] 플랫폼 드라이버 데이터가 곧 rk_iommu다. */
	data->iommu = platform_get_drvdata(iommu_dev);
	/* [한국어] 마스터에 매달아 이후 콜백들이 꺼내 쓸 수 있게 한다. */
	dev_iommu_priv_set(dev, data);

	/* [한국어] 위에서 올린 참조 계수를 내린다. */
	platform_device_put(iommu_dev);

	return 0;	/* [한국어] 마스터에 IOMMU를 연결했다. */
}

/* [한국어] 이 드라이버가 IOMMU 코어에 제공하는 연산 테이블.
 * 무효화 콜백(iotlb_sync 등)이 없는 것이 눈에 띄는데, 이 드라이버는
 * unmap 안에서 즉시 무효화를 끝내기 때문이다. */
static const struct iommu_ops rk_iommu_ops = {
	.identity_domain = &rk_identity_domain,
	/* [한국어] 통과 모드로 쓸 정적 도메인. 실제로는 "페이징 꺼짐"
	 * 상태이며, 이 드라이버의 detach 구현이기도 하다. */

	.domain_alloc_paging = rk_iommu_domain_alloc_paging,
	/* [한국어] 페이징 도메인 생성. 디렉터리 테이블 한 페이지를 잡는다. */

	.probe_device = rk_iommu_probe_device,
	/* [한국어] 디바이스 등록. 전원 의존 링크를 만드는 것이 핵심이다. */

	.release_device = rk_iommu_release_device,
	/* [한국어] 디바이스 제거. 그 링크를 끊는다. */

	.device_group = generic_single_device_group,
	/* [한국어] 격리 단위 결정. Rockchip에서는 마스터마다 IOMMU가
	 * 하나씩 붙으므로 디바이스마다 독립 그룹이면 충분하다. */

	.of_xlate = rk_iommu_of_xlate,
	/* [한국어] 디바이스 트리의 iommus 참조를 연결 정보로 옮긴다. */

	.default_domain_ops = &(const struct iommu_domain_ops) {
		.attach_dev	= rk_iommu_attach_device,
		/* [한국어] 붙이기. 먼저 떼고 다시 붙이는 순서를 지킨다. */

		.map_pages	= rk_iommu_map,
		/* [한국어] 매핑. 항상 하나의 DTE 아래에서만 동작한다. */

		.unmap_pages	= rk_iommu_unmap,
		/* [한국어] 해제. 무효화까지 이 안에서 끝낸다. */

		.iova_to_phys	= rk_iommu_iova_to_phys,
		/* [한국어] 조회. 소프트웨어 테이블만 읽으므로 전원과 무관하다. */

		.free		= rk_iommu_domain_free,
		/* [한국어] 도메인 해제. 매달린 페이지 테이블까지 모두 반납한다. */
	}
};

/*
 * [한국어]
 * rk_iommu_probe - IOMMU 하드웨어 블록 하나를 초기화한다
 *
 * @pdev: 플랫폼 디바이스.
 * @return: 0 성공, 음수 오류.
 *
 * 순서에 이유가 있다.
 *
 *  1) 상태 구조체를 잡고 **identity 도메인으로 시작한다.** 도메인이
 *     붙기 전까지 이 IOMMU는 아무것도 변환하지 않는다.
 *  2) 세대(v1/v2)를 확정한다. 전역이므로 두 세대가 섞이면 거부한다.
 *  3) reg 자원마다 ioremap 해 MMU 인스턴스를 센다. 하나도 없으면 실패.
 *  4) 클럭을 얻는다 — 오래된 디바이스 트리에는 없을 수 있어 선택 사항.
 *  5) 런타임 PM을 켠 **뒤에** 인터럽트를 등록한다. 순서가 반대면
 *     핸들러의 pm_runtime_get_if_in_use가 아직 준비되지 않은
 *     PM 상태를 만난다.
 *  6) 마지막에 코어 등록. 이 순간부터 마스터들의 probe_device가 온다.
 *
 * 실행 컨텍스트: 플랫폼 probe. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   플랫폼 코어 → [rk_iommu_probe] → iommu_device_register()
 */
static int rk_iommu_probe(struct platform_device *pdev)
{
	/* [한국어] 로그와 devm의 기준 디바이스. */
	struct device *dev = &pdev->dev;
	/* [한국어] 만들 IOMMU 상태. */
	struct rk_iommu *iommu;
	/* [한국어] ioremap 할 레지스터 자원. */
	struct resource *res;
	/* [한국어] 디바이스 트리 매치가 알려 준 세대별 연산 묶음. */
	const struct rk_iommu_ops *ops;
	/* [한국어] 이 디바이스가 선언한 자원의 수(MMU 인스턴스 후보). */
	int num_res = pdev->num_resources;
	/* [한국어] 단계별 결과와 순회 인덱스. */
	int err, i;

	/* [한국어] 디바이스 수명에 묶어 상태를 잡는다. */
	iommu = devm_kzalloc(dev, sizeof(*iommu), GFP_KERNEL);
	if (!iommu)	/* [한국어] 상태 구조체를 잡지 못했다. */
		return -ENOMEM;

	/* [한국어] 아무 도메인에도 붙지 않은 상태로 시작한다 —
	 * 이 드라이버의 기본 자세다. */
	iommu->domain = &rk_identity_domain;

	/* [한국어] of_xlate가 이 값으로 IOMMU를 찾으므로 일찍 설정한다. */
	platform_set_drvdata(pdev, iommu);
	/* [한국어] 로그와 PM의 기준 디바이스. */
	iommu->dev = dev;
	/* [한국어] 아래 루프에서 성공한 인스턴스만 센다. */
	iommu->num_mmu = 0;

	/* [한국어] 디바이스 트리 매치에 붙은 세대별 연산 묶음을 얻는다. */
	ops = of_device_get_match_data(dev);
	/* [한국어] 첫 IOMMU가 시스템 전체의 세대를 확정한다. */
	if (!rk_ops)
		rk_ops = ops;

	/*
	 * That should not happen unless different versions of the
	 * hardware block are embedded the same SoC
	 */
	/* [한국어] 두 세대가 한 SoC에 섞여 있으면 전역 rk_ops로는
	 * 다룰 수 없다 — 엔트리 형식이 서로 다르기 때문이다. */
	if (WARN_ON(rk_ops != ops))
		return -EINVAL;

	/* [한국어] 인스턴스별 레지스터 베이스를 담을 배열. */
	iommu->bases = devm_kcalloc(dev, num_res, sizeof(*iommu->bases),
				    GFP_KERNEL);
	if (!iommu->bases)	/* [한국어] 베이스 주소 배열을 잡지 못했다. */
		return -ENOMEM;

	/* [한국어] 선언된 자원마다 매핑을 시도한다. */
	for (i = 0; i < num_res; i++) {
		/* [한국어] i번째 메모리 자원을 얻는다. */
		res = platform_get_resource(pdev, IORESOURCE_MEM, i);
		/* [한국어] 메모리 자원이 아니면 건너뛴다. */
		if (!res)
			continue;
		/* [한국어] 레지스터 영역을 매핑한다. */
		iommu->bases[i] = devm_ioremap_resource(&pdev->dev, res);
		/* [한국어] 실패한 인스턴스는 세지 않는다 — 일부만 살아
		 * 있어도 나머지로 동작할 수 있게 하려는 관용적 처리다. */
		if (IS_ERR(iommu->bases[i]))
			continue;
		iommu->num_mmu++;	/* [한국어] 매핑에 성공한 인스턴스만 센다. */
	}
	/* [한국어] 하나도 매핑되지 않았으면 이 블록을 쓸 수 없다.
	 * 첫 자원의 오류 코드를 그대로 전한다. */
	if (iommu->num_mmu == 0)
		return PTR_ERR(iommu->bases[0]);

	/* [한국어] 이 블록이 내는 인터럽트의 수를 센다. */
	iommu->num_irq = platform_irq_count(pdev);
	if (iommu->num_irq < 0)	/* [한국어] 인터럽트 수를 세지 못했다 — 그 오류를 그대로 전한다. */
		return iommu->num_irq;

	/* [한국어] 일부 보드는 강제 리셋이 다른 블록에 부작용을 일으켜
	 * 그것을 금지한다. */
	iommu->reset_disabled = device_property_read_bool(dev,
					"rockchip,disable-mmu-reset");

	/* [한국어] 필요한 클럭의 수는 이름 표의 길이로 정해진다. */
	iommu->num_clocks = ARRAY_SIZE(rk_iommu_clocks);
	/* [한국어] 클럭 핸들 배열을 잡는다. */
	iommu->clocks = devm_kcalloc(iommu->dev, iommu->num_clocks,
				     sizeof(*iommu->clocks), GFP_KERNEL);
	if (!iommu->clocks)	/* [한국어] 클럭 핸들 배열을 잡지 못했다. */
		return -ENOMEM;

	/* [한국어] bulk API가 이름으로 찾을 수 있도록 표의 이름을 채워 넣는다. */
	for (i = 0; i < iommu->num_clocks; ++i)
		iommu->clocks[i].id = rk_iommu_clocks[i];

	/*
	 * iommu clocks should be present for all new devices and devicetrees
	 * but there are older devicetrees without clocks out in the wild.
	 * So clocks as optional for the time being.
	 */
	/* [한국어] 클럭을 얻어 본다. */
	err = devm_clk_bulk_get(iommu->dev, iommu->num_clocks, iommu->clocks);
	/* [한국어] 디바이스 트리에 클럭이 없다 — 오래된 트리와의 호환을
	 * 위해 0개로 두고 진행한다. 이후 bulk 함수들이 아무 일도 하지 않는다. */
	if (err == -ENOENT)
		iommu->num_clocks = 0;
	else if (err)	/* [한국어] 클럭이 있는데 얻지 못한 것은 진짜 오류다. */
		return err;

	/* [한국어] 클럭을 미리 준비해 둔다. 이후 enable/disable은
	 * 인터럽트 문맥에서도 부를 수 있는 가벼운 연산이 된다. */
	err = clk_bulk_prepare(iommu->num_clocks, iommu->clocks);
	if (err)	/* [한국어] 클럭 준비에 실패했다. */
		return err;

	/* [한국어] 런타임 PM을 켠다. 인터럽트를 등록하기 **전에** 해야
	 * 핸들러의 전원 확인이 올바르게 동작한다. */
	pm_runtime_enable(dev);

	/* [한국어] 인터럽트마다 핸들러를 등록한다. */
	for (i = 0; i < iommu->num_irq; i++) {
		/* [한국어] i번째 인터럽트 번호. */
		int irq = platform_get_irq(pdev, i);

		/* [한국어] 인터럽트를 얻지 못했다 — PM을 되돌리고 나간다. */
		if (irq < 0) {
			err = irq;	/* [한국어] 인터럽트 번호를 얻지 못한 이유를 보관한다. */
			goto err_pm_disable;	/* [한국어] 켜 둔 런타임 PM을 되돌리러 간다. */
		}

		/* [한국어] 공유 인터럽트로 등록한다. 그래서 핸들러가
		 * "우리 것이 아니면 IRQ_NONE"을 지켜야 한다. */
		err = devm_request_irq(iommu->dev, irq, rk_iommu_irq,
				       IRQF_SHARED, dev_name(dev), iommu);
		if (err)	/* [한국어] 핸들러 등록에 실패했다. */
			goto err_pm_disable;
	}

	/* [한국어] 테이블 페이지가 이 세대가 다룰 수 있는 주소 범위 안에
	 * 놓이도록 DMA 계층에 알린다(v1은 32비트, v2는 40비트). */
	dma_set_mask_and_coherent(dev, rk_ops->dma_bit_mask);

	/* [한국어] sysfs에 IOMMU 인스턴스를 노출한다. */
	err = iommu_device_sysfs_add(&iommu->iommu, dev, NULL, dev_name(dev));
	if (err)	/* [한국어] sysfs 등록에 실패했다. */
		goto err_pm_disable;

	/* [한국어] 코어에 등록한다. 이 순간부터 마스터들의 probe_device가
	 * 불리기 시작한다. */
	err = iommu_device_register(&iommu->iommu, &rk_iommu_ops, dev);
	if (err)	/* [한국어] 코어 등록에 실패했다. */
		goto err_remove_sysfs;

	return 0;
/* [한국어] 코어 등록 실패 — sysfs 항목부터 되돌린다. */
err_remove_sysfs:
	iommu_device_sysfs_remove(&iommu->iommu);
/* [한국어] PM을 켠 이후의 실패 — PM과 클럭 준비를 되돌린다. */
err_pm_disable:
	pm_runtime_disable(dev);	/* [한국어] 켜 둔 런타임 PM을 되돌린다. */
	clk_bulk_unprepare(iommu->num_clocks, iommu->clocks);	/* [한국어] 준비해 둔 클럭을 되돌린다. */
	return err;	/* [한국어] 실패 이유를 플랫폼 코어에 전한다. */
}

/*
 * [한국어]
 * rk_iommu_shutdown - 시스템 종료 시 인터럽트를 끊고 전원을 내린다
 *
 * @pdev: 대상 플랫폼 디바이스.
 * @return: 없음.
 *
 * 종료 중에는 마스터가 이미 멈췄거나 예측할 수 없는 상태여서,
 * 폴트 인터럽트가 올라와도 처리할 의미가 없다. 먼저 인터럽트를
 * 끊고 나서 전원을 내린다 — 순서가 반대면 전원이 없는 상태에서
 * 인터럽트가 들어올 수 있다.
 *
 * pm_runtime_force_suspend()는 런타임 PM의 참조 계수와 무관하게
 * 강제로 잠재우는 함수다. 종료 경로에서는 참조를 들고 있는
 * 마스터가 있더라도 내려야 한다.
 *
 * 실행 컨텍스트: 시스템 종료. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   플랫폼 코어 shutdown → [rk_iommu_shutdown]
 */
static void rk_iommu_shutdown(struct platform_device *pdev)
{
	/* [한국어] 대상 IOMMU 상태. */
	struct rk_iommu *iommu = platform_get_drvdata(pdev);
	/* [한국어] 인터럽트 순회 인덱스. */
	int i;

	/* [한국어] 등록한 인터럽트를 모두 해제한다. */
	for (i = 0; i < iommu->num_irq; i++) {
		/* [한국어] 등록할 때와 같은 순서로 번호를 얻는다. */
		int irq = platform_get_irq(pdev, i);

		/* [한국어] 핸들러를 떼어 낸다 — 전원을 내리기 전에 해야
		 * 전원 없는 상태의 인터럽트를 막을 수 있다. */
		devm_free_irq(iommu->dev, irq, iommu);
	}

	/* [한국어] 참조 계수와 무관하게 강제로 잠재운다. */
	pm_runtime_force_suspend(&pdev->dev);
}

/*
 * [한국어]
 * rk_iommu_suspend - 런타임 서스펜드. 하드웨어 설정을 내린다
 *
 * @dev: 대상 디바이스.
 * @return: 항상 0.
 *
 * 전원이 내려가면 레지스터 내용이 사라지므로, 미리 깨끗하게
 * 끄는 것이 이 함수의 일이다. 도메인에 붙어 있지 않다면 애초에
 * 켠 것이 없어 할 일도 없다.
 *
 * 소프트웨어 상태(iommu->domain, 테이블)는 그대로 두는 점이
 * 중요하다. resume이 그것을 근거로 전부 복원한다.
 *
 * 실행 컨텍스트: 런타임 PM 콜백. 전원이 아직 살아 있다.
 *
 * 호출 체인:
 *   PM 코어 → [rk_iommu_suspend] → rk_iommu_disable()
 */
static int __maybe_unused rk_iommu_suspend(struct device *dev)
{
	/* [한국어] 대상 IOMMU 상태. */
	struct rk_iommu *iommu = dev_get_drvdata(dev);

	/* [한국어] 어느 도메인에도 붙어 있지 않으면 켠 것이 없다. */
	if (iommu->domain == &rk_identity_domain)
		return 0;

	/* [한국어] 변환을 끄고 테이블 주소와 인터럽트를 지운다.
	 * 소프트웨어 장부는 그대로 두어 resume이 복원할 수 있게 한다. */
	rk_iommu_disable(iommu);
	return 0;	/* [한국어] 서스펜드는 실패하지 않는다 — 끄는 일은 언제나 가능하다. */
}

/*
 * [한국어]
 * rk_iommu_resume - 런타임 리줌. 하드웨어 설정을 복원한다
 *
 * @dev: 대상 디바이스.
 * @return: 0 성공, 음수 오류.
 *
 * suspend의 대칭이자, **attach가 전원 꺼짐 때문에 미뤄 둔 설정을
 * 대신 수행하는 자리**이기도 하다. attach는 전원이 없으면 장부만
 * 고치고 성공을 돌려주는데, 그 뒤 처음 깨어날 때 이 함수가
 * iommu->domain을 보고 실제 하드웨어를 설정한다.
 *
 * 실행 컨텍스트: 런타임 PM 콜백. 전원이 막 들어온 상태.
 *
 * 호출 체인:
 *   PM 코어 → [rk_iommu_resume] → rk_iommu_enable()
 */
static int __maybe_unused rk_iommu_resume(struct device *dev)
{
	/* [한국어] 대상 IOMMU 상태. */
	struct rk_iommu *iommu = dev_get_drvdata(dev);

	/* [한국어] 붙어 있는 도메인이 없으면 복원할 것도 없다. */
	if (iommu->domain == &rk_identity_domain)
		return 0;

	/* [한국어] 장부에 적힌 도메인의 테이블을 걸고 변환을 켠다. */
	return rk_iommu_enable(iommu);
}

/* [한국어] 전원 관리 콜백 묶음.
 * 런타임 PM은 위의 두 함수를 쓰고, 시스템 절전은 런타임 PM을
 * 강제로 오가게 하는 표준 헬퍼에 맡긴다 — 두 경로에서 해야 할
 * 일이 같기 때문이다. */
static const struct dev_pm_ops rk_iommu_pm_ops = {
	SET_RUNTIME_PM_OPS(rk_iommu_suspend, rk_iommu_resume, NULL)	/* [한국어] 런타임 서스펜드와 리줌에 위 두 함수를 연결한다(유휴 콜백은 없다). */
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
};

/* [한국어] 1세대 하드웨어의 연산 묶음.
 * 32비트 물리 주소만 다룰 수 있어, 테이블 페이지도 4GB 아래에
 * 놓여야 한다(GFP_DMA32). */
static struct rk_iommu_ops iommu_data_ops_v1 = {
	.pt_address = &rk_dte_pt_address,
	/* [한국어] 마스킹 한 번으로 주소를 뽑는 단순한 형식. */

	.mk_dtentries = &rk_mk_dte,
	/* [한국어] 주소에 유효 비트만 얹는다. */

	.mk_ptentries = &rk_mk_pte,
	/* [한국어] 주소와 권한과 유효 비트를 합친다. */

	.dma_bit_mask = DMA_BIT_MASK(32),
	/* [한국어] 엔트리가 32비트 주소만 담을 수 있다. */

	.gfp_flags = GFP_DMA32,
	/* [한국어] 테이블 자체도 4GB 아래에 있어야 가리킬 수 있다. */
};

/* [한국어] 2세대(RK3568 등) 하드웨어의 연산 묶음.
 * 40비트 물리 주소를 지원하며, 그 상위 비트를 엔트리의 예약
 * 자리에 나눠 담는다. */
static struct rk_iommu_ops iommu_data_ops_v2 = {
	.pt_address = &rk_dte_pt_address_v2,
	/* [한국어] 흩어진 상위 비트를 모아 40비트 주소를 복원한다. */

	.mk_dtentries = &rk_mk_dte_v2,
	/* [한국어] 40비트 주소를 엔트리 배치로 흩어 담는다. */

	.mk_ptentries = &rk_mk_pte_v2,
	/* [한국어] 주소 부분은 DTE와 같아 그 함수를 재사용한다. */

	.dma_bit_mask = DMA_BIT_MASK(40),
	/* [한국어] 40비트까지의 물리 주소를 다룰 수 있다. */

	.gfp_flags = 0,
	/* [한국어] 주소 범위 제약이 없어 특별한 할당 플래그가 필요 없다. */
};

/* [한국어] 디바이스 트리 호환 문자열과 세대별 연산 묶음의 대응표.
 * 이 매치 데이터가 probe에서 rk_ops로 확정된다. */
static const struct of_device_id rk_iommu_dt_ids[] = {
	{	.compatible = "rockchip,iommu",
		/* [한국어] RK3288 등 1세대 블록. */
		.data = &iommu_data_ops_v1,
		/* [한국어] 32비트 주소 형식의 연산 묶음. */
	},
	{	.compatible = "rockchip,rk3568-iommu",
		/* [한국어] RK3568 이후의 2세대 블록. */
		.data = &iommu_data_ops_v2,
		/* [한국어] 40비트 주소 형식의 연산 묶음. */
	},
	{ /* sentinel */ }
	/* [한국어] 표의 끝을 알리는 빈 항목. */
};

/* [한국어] 플랫폼 드라이버 서술자. */
static struct platform_driver rk_iommu_driver = {
	.probe = rk_iommu_probe,
	/* [한국어] 하드웨어 초기화 진입점. */

	.shutdown = rk_iommu_shutdown,
	/* [한국어] 시스템 종료 시 인터럽트를 끊고 전원을 내린다. */

	.driver = {
		   .name = "rk_iommu",
		   /* [한국어] 드라이버 이름. */

		   .of_match_table = rk_iommu_dt_ids,
		   /* [한국어] 위의 호환 문자열 대응표. */

		   .pm = &rk_iommu_pm_ops,
		   /* [한국어] 런타임/시스템 전원 관리 콜백. */

		   .suppress_bind_attrs = true,
		   /* [한국어] sysfs로 바인딩을 풀 수 없게 막는다.
		    * IOMMU를 임의로 떼어 내면 그 아래 마스터들의 DMA가
		    * 갑자기 변환 없이 나가게 되어 위험하기 때문이다. */
	},
};
/* [한국어] 모듈이 아니라 커널에 내장되는 드라이버로 등록한다.
 * 마스터들이 부팅 초기에 IOMMU를 필요로 하기 때문이다. */
builtin_platform_driver(rk_iommu_driver);
