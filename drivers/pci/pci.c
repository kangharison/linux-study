// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Bus Services, see include/linux/pci.h for further explanation.
 *
 * Copyright 1993 -- 1997 Drew Eckhardt, Frederic Potter,
 * David Mosberger-Tang
 *
 * Copyright 1997 -- 2000 Martin Mares <mj@ucw.cz>
 */

/*
 * [한국어 설명] PCI 버스 서비스의 핵심 구현 (drivers/pci/pci.c)
 *
 * === 파일의 역할 ===
 * PCI 장치 하나를 "쓸 수 있는 상태"로 만들고 유지하는 데 필요한 조작을 모아 둔
 * 파일이다. 장치 활성화(pci_enable_device 계열), 버스 마스터 권한 부여,
 * BAR 리소스 예약, config space 접근 헬퍼, capability 탐색, 전원 상태 전이,
 * 함수 리셋(FLR 등), 링크 파라미터 조회가 모두 여기 있다.
 * 버스를 훑어 장치를 발견하는 일은 probe.c 가, 리소스 주소를 실제로 배정하는 일은
 * setup-bus.c/setup-res.c 가, 인터럽트 할당은 msi/ 가 맡는다. 이 파일은 그렇게
 * 만들어진 struct pci_dev 를 대상으로 "무엇을 켜고 끄고 되돌릴 것인가"를 다룬다.
 * 7700여 줄로 PCI 서브시스템에서 가장 큰 파일이며, 드라이버가 pci_* API 를 부르면
 * 대부분 이 파일 어딘가에 도달한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 부팅 시 흐름은 대략 다음과 같다.
 *   PCI 호스트 컨트롤러 드라이버(controller/) 가 버스를 등록
 *     → probe.c 가 config space 를 훑어 장치를 발견하고 pci_dev 를 만든다
 *     → setup-bus.c 가 BAR 에 실제 주소를 배정한다
 *     → pci-driver.c 가 vendor/device ID 로 드라이버를 짝지어 .probe 를 부른다
 *     → [이 파일] 드라이버가 pci_enable_device_mem(), pci_set_master() 등을 호출해
 *       장치를 깨우고 DMA 권한을 준다
 * 실행 컨텍스트는 대부분 프로세스 컨텍스트다(잠들 수 있다). 다만
 * pci_channel_offline() 이나 config 접근 헬퍼처럼 오류 처리 경로에서 불리는
 * 것들은 더 제한적인 문맥에서도 호출될 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * 아래로는 각 호스트 컨트롤러 드라이버의 config space 접근 콜백(access.c 가 중개)에
 * 의존하고, 위로는 모든 PCI 드라이버가 이 파일의 API 를 쓴다. 전원 관리는
 * pci-acpi.c(ACPI _PS0/_PS3, _DSM)와 협력하고, 링크 절전은 pcie/aspm.c,
 * 오류 복구는 pcie/aer.c, 인터럽트는 msi/ 가 각각 나눠 맡는다.
 * 공유 상태는 struct pci_dev 자체와, 그 안의 saved_config_space(전원 전이 전후로
 * config space 를 통째로 저장·복원하는 버퍼)다.
 *
 * === NVMe 드라이버가 실제로 쓰는 것 (drivers/nvme/ 전수 확인) ===
 * 주석을 제거한 drivers/nvme/ 전체를 검색해 실제 호출을 확인했다. NVMe 가 부르는
 * pci* 함수는 33개이며, 그중 이 파일에 정의된 것은 다음 8개다:
 *
 *     pci_enable_device_mem()   — 장치를 깨우고 MEM 공간 디코딩을 켠다.
 *                                 NVMe 는 IO 공간을 쓰지 않아 _mem 판을 쓴다.
 *     pci_set_master()          — PCI_COMMAND 의 Bus Master Enable 비트를 세운다.
 *                                 이것이 없으면 장치가 DMA 를 시작할 수 없어,
 *                                 NVMe 의 SQ/CQ 링과 PRP/SGL 전송이 전부 불가능하다.
 *     pci_disable_device()      — 위 둘의 반대. 제거·리셋 경로에서 부른다.
 *     pci_save_state()          — config space 를 saved_config_space 에 통째로 보관.
 *     pci_restore_state()       — 그것을 되돌린다. D3 전환이나 FLR 후에는 BAR·
 *                                 Command 레지스터가 초기화되므로 반드시 필요하다.
 *     pci_load_saved_state()    — 미리 만들어 둔 상태 묶음을 적용한다.
 *     pci_device_is_present()   — Vendor ID 를 읽어 0xFFFF 가 아닌지 본다.
 *                                 표면 제거(surprise removal) 판정의 근거다.
 *     pcie_reset_flr()          — PCIe Device Control 의 Initiate Function Level
 *                                 Reset 비트를 눌러 함수 하나만 리셋한다.
 *                                 drivers/nvme/host/pci.c 의 컨트롤러 리셋 경로가
 *                                 직접 호출한다.
 *
 * 나머지 25개는 다른 파일에 있다 — 예를 들어 pci_alloc_irq_vectors_affinity() 는
 * msi/ 에, pci_p2pdma_add_resource()/pci_alloc_p2pmem() 은 p2pdma.c 에,
 * pci_request_mem_regions() 는 인라인 래퍼다.
 *
 * (이전 주석은 pci_iomap, pci_find_capability, pcie_get_mps, pci_set_power_state,
 *  pci_reset_function, pci_enable_acs 등을 "NVMe 가 호출한다"고 적어 두었으나
 *  실제 호출은 없었다. 위 검증 결과로 대체했다.)
 *
 * === 주요 함수/구조체 요약 ===
 * pci_enable_device_mem()/pci_disable_device() : 장치 활성화·비활성화의 짝.
 *   내부적으로 참조 계수를 세므로 중복 호출이 안전하다.
 * pci_set_master()/pci_clear_master()          : Bus Master Enable 비트 제어.
 * pci_save_state()/pci_restore_state()         : config space 스냅숏과 복원.
 *   전원 전이·리셋을 넘나들 때 BAR 와 Command 를 잃지 않게 하는 장치다.
 * pci_set_power_state()                        : D0~D3cold 전이. ACPI 협력 포함.
 * pci_find_capability()/pci_find_ext_capability() : capability 리스트 순회.
 *   전자는 config space 0x34 에서 시작하는 표준 리스트, 후자는 0x100 부터의
 *   확장 리스트를 훑는다.
 * pci_reset_function() 계열                    : FLR 을 비롯한 리셋 방법 선택.
 */

#include <linux/acpi.h>	/* [한국어] ACPI 전원 관리 연동 — _PS0/_PS3 로 D-state 를 바꾸거나 _DSM 으로 장치별 기능을 묻는다 */
#include <linux/kernel.h>	/* [한국어] min/max, WARN_ON 등 커널 기본 매크로 */
#include <linux/delay.h>	/* [한국어] msleep/udelay — 리셋 후 장치가 응답하기까지 스펙이 정한 대기 시간을 지킬 때 */
#include <linux/dmi.h>	/* [한국어] DMI(SMBIOS) 조회 — 특정 메인보드에서만 필요한 예외 처리를 판별한다 */
#include <linux/init.h>	/* [한국어] __init 등 초기화 섹션 표시 */
#include <linux/iommu.h>	/* [한국어] IOMMU 연동 — DMA 를 켜기 전에 장치가 어느 IOMMU 그룹에 속하는지 확인한다 */
#include <linux/lockdep.h>	/* [한국어] 락 순서 검증 어노테이션. 이 파일은 여러 전역 락(pci_bus_sem 등)을 다뤄 순서가 중요하다 */
#include <linux/msi.h>	/* [한국어] struct msi_desc — 인터럽트 해제 시 남은 MSI 기술자를 정리하는 경로에서 참조 */
#include <linux/of.h>	/* [한국어] DeviceTree 노드 조회 — ACPI 가 없는 임베디드 플랫폼의 전원/리셋 정보를 읽는다 */
#include <linux/pci.h>	/* [한국어] struct pci_dev, pci_* 공개 API 선언. 이 파일이 구현하는 인터페이스의 정의처 */
#include <linux/pm.h>	/* [한국어] 전원 관리 공통 타입(pm_message_t 등) */
#include <linux/slab.h>	/* [한국어] kmalloc/kfree — saved_config_space 버퍼 등을 동적 할당한다 */
#include <linux/module.h>	/* [한국어] EXPORT_SYMBOL — 이 파일의 API 를 모든 PCI 드라이버에 공개한다 */
#include <linux/spinlock.h>	/* [한국어] pci_lock 등 config space 접근 직렬화용 스핀락 */
#include <linux/string.h>	/* [한국어] memcpy/strcmp — 파라미터 문자열 파싱과 상태 버퍼 복사 */
#include <linux/log2.h>	/* [한국어] ilog2/roundup_pow_of_two — BAR 크기와 정렬이 2의 거듭제곱이라 시프트 계산에 쓴다 */
#include <linux/logic_pio.h>	/* [한국어] 논리 PIO — MMIO 만 있는 아키텍처에서 IO 공간 접근을 흉내 내는 계층 */
#include <linux/device.h>	/* [한국어] 드라이버 모델(struct device) — pci_dev 가 그 위에 얹혀 있다 */
#include <linux/pm_runtime.h>	/* [한국어] 런타임 PM — 유휴 시 D3 로 내려보내고 접근 시 깨우는 자동 전원 관리 */
#include <linux/pci-ats.h>	/* [한국어] ATS/PASID — 장치가 IOMMU 변환을 캐시하는 기능. 활성화 순서 제약이 있다 */
#include <linux/pci_hotplug.h>	/* [한국어] 핫플러그 슬롯 관련 타입 */
#include <linux/vmalloc.h>	/* [한국어] vmalloc — 큰 상태 버퍼를 물리 연속 없이 잡을 때 */
#include <asm/dma.h>	/* [한국어] 아키텍처별 DMA 상수(MAX_DMA_ADDRESS 등) */
#include <linux/aer.h>	/* [한국어] AER(Advanced Error Reporting) 타입 — 오류 복구 경로와 리셋이 맞물린다 */
#include <linux/bitfield.h>	/* [한국어] FIELD_GET/FIELD_PREP — config space 레지스터의 비트 필드를 안전하게 뽑고 넣는다 */
#include "pci.h"	/* [한국어] PCI 서브시스템 내부 전용 API(pci_bus_sem, 내부 헬퍼). 외부에 노출하지 않는 것들 */

DEFINE_MUTEX(pci_slot_mutex); /* [한국어] PCI 슬롯 리스트(struct pci_slot)를 보호하는 전역 뮤텍스. 슬롯 등록/해제와 슬롯 단위 리셋이 서로를 밟지 않게 한다 */

/*
 * [한국어] pci_power_t 값(PCI_UNKNOWN..PCI_D3cold)을 사람이 읽는 문자열로 바꾸는 표.
 * 인덱스가 곧 D-state 번호이며, 로그에 "D0" "D3hot" 처럼 찍을 때 쓴다.
 * PCI PM 규격의 전원 상태는 D0(완전 동작) → D1 → D2 → D3hot(버스 전원은 살아 있음)
 * → D3cold(전원 자체가 끊김) 순으로 깊어진다.
 */
const char *pci_power_names[] = {
	"error", "D0", "D1", "D2", "D3hot", "D3cold", "unknown", /* [한국어] 앞의 두 칸은 D-state 가 아닌 특수값 — "error"(PCI_ERROR_RESPONSE 상태), "unknown"(아직 모름) */
};
EXPORT_SYMBOL_GPL(pci_power_names);

#ifdef CONFIG_X86_32 /* [한국어] x86 32비트에서만 존재하는 ISA DMA 브리지 결함 플래그. 다른 아키텍처에는 ISA 브리지가 없어 컴파일하지 않는다 */
int isa_dma_bridge_buggy; /* [한국어] 특정 ISA/PCI 브리지가 DMA 중 버스를 제대로 넘기지 못하는 결함이 있으면 1. quirk 코드가 세우고, 사운드/플로피 같은 ISA DMA 드라이버가 읽어 우회한다 */
EXPORT_SYMBOL(isa_dma_bridge_buggy);
#endif

int pci_pci_problems; /* [한국어] PCI_PCI_PROBLEMS_* 비트 모음 — 칩셋 자체의 알려진 결함(쓰기 결합 불가, MWI 금지 등)을 quirk 가 기록해 두는 전역 */
EXPORT_SYMBOL(pci_pci_problems);

unsigned int pci_pm_d3hot_delay; /* [한국어] D3hot→D0 복귀 후 추가로 기다릴 밀리초. pci=d3hot_delay= 로 관리자가 늘릴 수 있고, pci_dev_d3_sleep() 이 장치별 값과 max 를 취한다 */

static void pci_pme_list_scan(struct work_struct *work); /* [한국어] 아래에서 DECLARE_DELAYED_WORK 가 이 함수를 참조하므로 미리 선언해 둔다 */

static LIST_HEAD(pci_pme_list); /* [한국어] PME(Power Management Event) 를 폴링으로 확인해야 하는 장치들의 리스트 머리. 인터럽트로 PME 를 못 받는 장치를 주기적으로 훑는다 */
static DEFINE_MUTEX(pci_pme_list_mutex); /* [한국어] 위 리스트를 보호하는 뮤텍스. 등록/해제는 프로세스 문맥이고 스캔도 워크큐(프로세스 문맥)라 잠들 수 있는 뮤텍스로 충분하다 */
static DECLARE_DELAYED_WORK(pci_pme_work, pci_pme_list_scan); /* [한국어] PME_TIMEOUT(1초) 마다 pci_pme_list_scan() 을 돌리는 지연 워크. 워크큐 문맥이라 config space 접근이 잠들어도 안전하다 */

/*
 * [한국어] PME 폴링 대상 하나를 나타내는 항목.
 * pci_pme_active() 가 "PME 를 켰지만 이 장치는 pme_poll 이다" 라고 판단하면
 * 이 구조체를 만들어 pci_pme_list 에 넣고, pci_pme_list_scan() 이 1초마다 훑는다.
 */
struct pci_pme_device {
	struct list_head list; /* [한국어] pci_pme_list 에 꿰이는 링크. 설정자: pci_pme_active(). 읽는 자: pci_pme_list_scan(). 동기화: pci_pme_list_mutex */
	struct pci_dev *dev; /* [한국어] 폴링할 장치. 이 포인터를 들고 있는 동안 장치가 사라지면 안 되므로 pci_pme_active() 가 리스트에서 빼는 것으로 수명을 맞춘다 */
};

#define PME_TIMEOUT 1000 /* How long between PME checks */

/*
 * Following exit from Conventional Reset, devices must be ready within 1 sec
 * (PCIe r6.0 sec 6.6.1).  A D3cold to D0 transition implies a Conventional
 * Reset (PCIe r6.0 sec 5.8).
 */
/* [한국어] Conventional Reset 을 벗어난 장치가 config 요청에 응답해야 하는 제한 시간(ms).
 * D3cold→D0 전이도 Conventional Reset 과 같은 효과라 이 대기가 적용된다.
 * pci_dev_wait() 이 이 값을 상한으로 삼아 장치가 살아나기를 기다린다. */
#define PCI_RESET_WAIT 1000 /* msec */

/*
 * Devices may extend the 1 sec period through Request Retry Status
 * completions (PCIe r6.0 sec 2.3.1).  The spec does not provide an upper
 * limit, but 60 sec ought to be enough for any device to become
 * responsive.
 */
/* [한국어] 장치가 Request Retry Status(CRS)로 "아직 준비 안 됨"을 계속 답할 때의 상한(ms).
 * 스펙에 상한이 없어 커널이 60초로 못 박았다 — 이보다 오래 끌면 고장으로 본다. */
#define PCIE_RESET_READY_POLL_MS 60000 /* msec */

/*
 * [한국어]
 * pci_dev_d3_sleep - D3 계열 전원 상태를 오갈 때 규정된 안정화 시간만큼 잠든다.
 *
 * @dev: 전원 상태를 막 바꾼(또는 바꾸려는) PCI 장치.
 * @return: 없음 — 지연만 준다.
 *
 * PCI PM 규격은 D3hot 을 드나든 뒤 장치가 config 접근에 응답하기까지 일정 시간이
 * 필요하다고 정한다. 그 시간을 지키지 않고 config space 를 읽으면 응답이 없어
 * all-ones(0xFFFF)가 돌아오고, 상위 코드가 장치가 사라졌다고 오인한다.
 * 실제 지연은 "장치별 요구치 dev->d3hot_delay" 와 "커널 파라미터
 * pci_pm_d3hot_delay" 중 큰 쪽이다. 후자는 스펙보다 굼뜬 하드웨어를 위해
 * pci=d3hot_delay= 로 전역 하한을 올릴 수 있게 열어 둔 것이다.
 * 둘 다 0 이면 아무 지연도 주지 않는다(지연이 필요 없다고 선언된 장치).
 *
 * 실행 문맥: usleep_range() 로 잠들기 때문에 반드시 프로세스 문맥이어야 한다.
 * 인터럽트/atomic 문맥에서 부르면 안 된다.
 *
 * 호출 체인:
 *   pci_power_up() / pci_set_low_power_state() / pci_pm_reset()
 *     → [이 함수] → usleep_range()
 */
static void pci_dev_d3_sleep(struct pci_dev *dev)
{
	unsigned int delay_ms = max(dev->d3hot_delay, pci_pm_d3hot_delay); /* [한국어] 두 요구치 중 큰 값을 택한다 — 어느 쪽도 위반하지 않으려면 더 긴 쪽을 지켜야 한다 */
	unsigned int upper; /* [한국어] 실제 sleep 구간의 위쪽 경계. 아래에서 delay_ms 의 20% 로 잡는다 */

	if (delay_ms) { /* [한국어] 요구 지연이 0 이면 잠들 이유가 없다 — 그대로 빠져나간다 */
		/* Use a 20% upper bound, 1ms minimum */
		upper = max(DIV_ROUND_CLOSEST(delay_ms, 5), 1U); /* [한국어] 허용 오차를 delay_ms 의 20%(1/5)로 잡되 최소 1ms. usleep_range 에 폭을 주면 커널이 다른 타이머와 묶어 깨울 수 있어 인터럽트 횟수가 준다 */
		usleep_range(delay_ms * USEC_PER_MSEC, /* [한국어] [delay_ms, delay_ms+upper] 구간에서 깨어난다. 하한은 반드시 지키므로 스펙 위반은 없다 */
			     (delay_ms + upper) * USEC_PER_MSEC);
	}
}

/*
 * [한국어]
 * pci_reset_supported - 이 장치에 시도해 볼 수 있는 리셋 방법이 하나라도 있는가.
 *
 * @dev: 검사 대상 PCI 장치.
 * @return: 리셋 방법이 하나라도 등록돼 있으면 true, 없으면 false.
 *
 * dev->reset_methods[] 는 pci_init_reset_methods() 가 채우는 배열로,
 * 쓸 수 있는 리셋 방법의 인덱스를 우선순위 순으로 담고 0 으로 끝난다.
 * 따라서 첫 칸이 0 이면 후보가 하나도 없다는 뜻이고, 그때 리셋 요청은
 * -ENOTTY 로 거절되어야 한다.
 *
 * 실행 문맥: 필드 하나를 읽을 뿐이라 어떤 문맥에서도 안전하다. 락도 잡지 않는다.
 *
 * 호출 체인:
 *   pci_reset_function() / pci_reset_function_locked() / pci_try_reset_function()
 *   그리고 pci-sysfs.c 의 reset_store(), quirks.c
 *     → [이 함수]
 */
bool pci_reset_supported(struct pci_dev *dev)
{
	return dev->reset_methods[0] != 0; /* [한국어] 배열이 0 으로 끝나므로 첫 칸이 0 이 아니면 최소 한 가지 방법이 있다 */
}

#ifdef CONFIG_PCI_DOMAINS /* [한국어] PCI 도메인(세그먼트)을 여러 개 둘 수 있는 아키텍처에서만 컴파일한다. 도메인은 버스 번호 0~255 를 통째로 갖는 독립 공간이다 */
int pci_domains_supported = 1; /* [한국어] 런타임에 도메인이 실제로 지원되는지. pci_no_domains() 가 0 으로 내리면 도메인 번호를 모두 0 으로 취급한다 */
#endif

#define DEFAULT_HOTPLUG_IO_SIZE		(256) /* [한국어] 핫플러그 브리지 뒤에 미리 잡아 둘 IO 공간 크기(바이트). 나중에 꽂힐 장치를 위한 예약분이다 */
#define DEFAULT_HOTPLUG_MMIO_SIZE	(2*1024*1024) /* [한국어] 핫플러그용 non-prefetchable MMIO 예약 크기 — 2MB */
#define DEFAULT_HOTPLUG_MMIO_PREF_SIZE	(2*1024*1024) /* [한국어] 핫플러그용 prefetchable MMIO 예약 크기 — 2MB. prefetchable 은 읽기에 부작용이 없어 브리지가 미리 읽어 와도 되는 영역이다 */
/* hpiosize=nn can override this */
unsigned long pci_hotplug_io_size  = DEFAULT_HOTPLUG_IO_SIZE; /* [한국어] 위 기본값을 담는 전역. pci=hpiosize= 로 덮어쓸 수 있다 */
/*
 * pci=hpmmiosize=nnM overrides non-prefetchable MMIO size,
 * pci=hpmmioprefsize=nnM overrides prefetchable MMIO size;
 * pci=hpmemsize=nnM overrides both
 */
unsigned long pci_hotplug_mmio_size = DEFAULT_HOTPLUG_MMIO_SIZE; /* [한국어] 핫플러그 non-prefetchable MMIO 예약 크기 전역 */
unsigned long pci_hotplug_mmio_pref_size = DEFAULT_HOTPLUG_MMIO_PREF_SIZE; /* [한국어] 핫플러그 prefetchable MMIO 예약 크기 전역 */

#define DEFAULT_HOTPLUG_BUS_SIZE	1 /* [한국어] 핫플러그 브리지 뒤에 예약할 버스 번호 개수. 1 이면 바로 아래 버스 하나만 미리 확보한다 */
unsigned long pci_hotplug_bus_size = DEFAULT_HOTPLUG_BUS_SIZE; /* [한국어] 그 값을 담는 전역. pci=hpbussize= 로 덮어쓸 수 있다 */


/* PCIe MPS/MRRS strategy; can be overridden by kernel command-line param */
#ifdef CONFIG_PCIE_BUS_TUNE_OFF /* [한국어] 빌드 설정으로 기본 MPS/MRRS 정책을 고른다. MPS(Max Payload Size)는 TLP 하나가 실어 나를 수 있는 데이터 바이트 수다 */
enum pcie_bus_config_types pcie_bus_config = PCIE_BUS_TUNE_OFF; /* [한국어] TUNE_OFF — 커널이 MPS 를 건드리지 않고 펌웨어가 설정한 값을 그대로 쓴다 */
#elif defined CONFIG_PCIE_BUS_SAFE /* [한국어] SAFE — 경로상 가장 작은 MPS 에 모두를 맞춘다. 가장 보수적이다 */
enum pcie_bus_config_types pcie_bus_config = PCIE_BUS_SAFE; /* [한국어] SAFE 정책 값 */
#elif defined CONFIG_PCIE_BUS_PERFORMANCE /* [한국어] PERFORMANCE — 각 링크가 감당 가능한 최대 MPS 를 쓴다. TLP 헤더 오버헤드가 줄어 대역폭이 오르지만, 경로가 섞이면 위험하다 */
enum pcie_bus_config_types pcie_bus_config = PCIE_BUS_PERFORMANCE; /* [한국어] PERFORMANCE 정책 값 */
#elif defined CONFIG_PCIE_BUS_PEER2PEER /* [한국어] PEER2PEER — 장치끼리 직접 DMA 할 수 있게 전 구간 MPS 를 128B 로 통일한다. P2P 트래픽은 어느 경로로 갈지 모르기 때문이다 */
enum pcie_bus_config_types pcie_bus_config = PCIE_BUS_PEER2PEER; /* [한국어] PEER2PEER 정책 값 */
#else /* [한국어] 아무 것도 고르지 않았을 때 */
enum pcie_bus_config_types pcie_bus_config = PCIE_BUS_DEFAULT; /* [한국어] DEFAULT — 펌웨어 값을 존중하되 필요한 경우에만 조정한다 */
#endif

/*
 * The default CLS is used if arch didn't set CLS explicitly and not
 * all pci devices agree on the same value.  Arch can override either
 * the dfl or actual value as it sees fit.  Don't forget this is
 * measured in 32-bit words, not bytes.
 */
u8 pci_dfl_cache_line_size __ro_after_init = L1_CACHE_BYTES >> 2; /* [한국어] 아키텍처가 명시하지 않았을 때 쓸 기본 캐시라인 크기. 단위가 32비트 워드라서 L1_CACHE_BYTES 를 4로 나눈다(>>2) */
u8 pci_cache_line_size __ro_after_init ; /* [한국어] 실제로 장치에 써 넣을 값. 0 이면 아직 정해지지 않은 것으로 보고 pci_set_cacheline_size() 가 기본값을 쓴다 */

/*
 * If we set up a device for bus mastering, we need to check the latency
 * timer as certain BIOSes forget to set it properly.
 */
unsigned int pcibios_max_latency = 255; /* [한국어] Latency Timer 에 써 넣을 상한(PCI 클럭 단위, 최대 255). 일부 BIOS 가 이 값을 0 으로 두고 넘겨 버스를 독점하는 문제가 있어 pcibios_set_master() 가 보정한다 */

/* If set, the PCIe ARI capability will not be used. */
static bool pcie_ari_disabled; /* [한국어] pci=noari 로 세워지는 플래그. ARI(Alternative Routing-ID)는 하나의 장치 번호 아래 함수 번호를 8비트로 확장하는 기능이다 */

/* If set, the PCIe ATS capability will not be used. */
static bool pcie_ats_disabled; /* [한국어] pci=noats 로 세워지는 플래그. ATS 를 끄면 장치가 IOMMU 변환 결과를 캐시하지 못한다 */

/* If set, the PCI config space of each device is printed during boot. */
bool pci_early_dump; /* [한국어] pci=earlydump 으로 세워지는 플래그. 부팅 초기에 각 장치의 config space 를 통째로 로그에 찍는다 */

/*
 * [한국어]
 * pci_ats_disabled - ATS(Address Translation Services) 사용이 금지돼 있는지 알려준다.
 *
 * @return: pci=noats 로 꺼 두었으면 true, 아니면 false.
 *
 * ATS 는 장치가 IOMMU 의 주소 변환 결과를 스스로 캐시(Device TLB)해 두고
 * 이미 변환된 주소로 DMA 를 내보내는 기능이다. 변환 왕복이 줄어 성능에는
 * 이롭지만, 장치를 신뢰할 수 없으면 변환을 우회하는 공격 통로가 되므로
 * 관리자가 통째로 끌 수 있어야 한다. 그 스위치를 읽는 접근자다.
 * 내부 변수 pcie_ats_disabled 는 static 이라 다른 파일에서 직접 볼 수 없어
 * 이 함수를 EXPORT 해 둔다.
 *
 * 실행 문맥: bool 하나를 읽을 뿐이며 락이 필요 없다. 부팅 후에는 값이 바뀌지 않는다.
 *
 * 호출 체인:
 *   drivers/pci/ats.c 의 pci_ats_init(), 이 파일의 pci_enable_acs(),
 *   drivers/pci/quirks.c → [이 함수]
 */
bool pci_ats_disabled(void)
{
	return pcie_ats_disabled; /* [한국어] static 변수를 그대로 돌려준다 */
}
EXPORT_SYMBOL_GPL(pci_ats_disabled);

/* Disable bridge_d3 for all PCIe ports */
static bool pci_bridge_d3_disable; /* [한국어] pcie_port_pm=off 로 세워진다. 모든 PCIe 포트의 D3 진입을 막는다 */
/* Force bridge_d3 for all PCIe ports */
static bool pci_bridge_d3_force; /* [한국어] pcie_port_pm=force 로 세워진다. 화이트리스트를 무시하고 D3 를 강제한다 */

/*
 * [한국어]
 * pcie_port_pm_setup - 커널 커맨드라인 "pcie_port_pm=" 값을 해석한다.
 *
 * @str: "=" 뒤에 붙은 문자열. "off" 또는 "force" 를 기대한다.
 * @return: 항상 1 — __setup 규약에서 1 은 "이 파라미터를 내가 처리했다"는 뜻이다.
 *
 * PCIe 포트(브리지)를 D3 로 내려보내면 그 아래 장치까지 전원이 끊겨 절전이
 * 크지만, 일부 플랫폼에서는 다시 깨우지 못하는 사고가 난다. 그래서 커널은
 * 브리지 D3 를 조건부로만 허용하고, 이 파라미터로 양쪽 극단을 강제할 수 있게 했다.
 *   off   → pci_bridge_d3_disable = true  (절대 D3 로 내리지 않는다)
 *   force → pci_bridge_d3_force   = true  (조건 검사 없이 허용한다)
 * 두 플래그는 pci_bridge_d3_possible() 이 읽는다.
 *
 * 실행 문맥: __init 이며 부팅 초기 파라미터 파싱 단계에서 한 번만 불린다.
 * 아직 장치가 하나도 없으므로 동기화가 필요 없다.
 *
 * 호출 체인:
 *   커널 파라미터 파서(__setup 테이블) → [이 함수]
 */
static int __init pcie_port_pm_setup(char *str)
{
	if (!strcmp(str, "off")) /* [한국어] "off" 이면 브리지 D3 를 전면 금지한다 */
		pci_bridge_d3_disable = true; /* [한국어] 금지 플래그를 세운다 */
	else if (!strcmp(str, "force")) /* [한국어] "force" 이면 반대로 조건 검사를 건너뛰고 허용한다 */
		pci_bridge_d3_force = true; /* [한국어] 강제 플래그를 세운다. 둘 다 아니면 아무 것도 바꾸지 않고 기본 정책을 쓴다 */
	return 1; /* [한국어] __setup 콜백은 1 을 돌려 "처리했음"을 알린다. 0 이면 알 수 없는 파라미터로 취급돼 init 환경변수로 넘어간다 */
}
__setup("pcie_port_pm=", pcie_port_pm_setup); /* [한국어] 이 함수를 "pcie_port_pm=" 파라미터의 처리기로 등록한다. 링크 타임에 __setup 섹션에 항목이 박힌다 */

/**
 * pci_bus_max_busnr - returns maximum PCI bus number of given bus' children
 * @bus: pointer to PCI bus structure to search
 *
 * Given a PCI bus, returns the highest PCI bus number present in the set
 * including the given PCI bus and its list of child PCI buses.
 */
/*
 * [한국어]
 * pci_bus_max_busnr - 이 버스와 그 아래 모든 자식 버스 중 가장 큰 버스 번호를 구한다.
 *
 * @bus: 탐색을 시작할 버스.
 * @return: 자기 자신을 포함한 서브트리에서 가장 큰 버스 번호(0~255).
 *
 * PCI 버스 번호는 8비트라 한 도메인에 256개뿐이다. 브리지 아래에 버스를 새로
 * 만들 때(핫플러그 등) 어디까지 이미 쓰였는지 알아야 다음 번호를 고를 수 있어
 * 이 값이 필요하다.
 * bus->busn_res 는 그 버스가 차지한 번호 구간을 담은 resource 이고, .end 가
 * 그 구간의 마지막 번호다. 자식 버스를 재귀로 훑어 최댓값을 갱신한다.
 *
 * 실행 문맥: bus->children 리스트를 락 없이 순회하므로, 호출자가 버스 구조가
 * 바뀌지 않는 문맥(핫플러그 경로 안 등)에 있어야 한다. 재귀 깊이는 PCI 계층
 * 깊이만큼이라 스택 위험은 없다.
 *
 * 호출 체인:
 *   drivers/pci/hotplug/acpiphp_glue.c → [이 함수] → [이 함수](재귀)
 */
unsigned char pci_bus_max_busnr(struct pci_bus *bus)
{
	struct pci_bus *tmp; /* [한국어] 자식 버스를 훑는 커서 */
	unsigned char max, n; /* [한국어] max 는 지금까지의 최댓값, n 은 자식 서브트리의 최댓값 */

	max = bus->busn_res.end; /* [한국어] 우선 자기 구간의 마지막 번호로 시작한다 */
	list_for_each_entry(tmp, &bus->children, node) { /* [한국어] 직속 자식 버스를 모두 순회한다. node 가 부모의 children 리스트에 꿰이는 링크다 */
		n = pci_bus_max_busnr(tmp); /* [한국어] 자식 서브트리의 최댓값을 재귀로 구한다 */
		if (n > max) /* [한국어] 더 큰 번호를 만나면 */
			max = n; /* [한국어] 최댓값을 갱신한다 */
	}
	return max; /* [한국어] 서브트리 전체의 최댓값을 돌려준다 */
}
EXPORT_SYMBOL_GPL(pci_bus_max_busnr);

/**
 * pci_status_get_and_clear_errors - return and clear error bits in PCI_STATUS
 * @pdev: the PCI device
 *
 * Returns error bits set in PCI_STATUS and clears them.
 */
/*
 * [한국어]
 * pci_status_get_and_clear_errors - PCI_STATUS 의 오류 비트를 읽어 돌려주고 지운다.
 *
 * @pdev: 대상 PCI 장치.
 * @return: 읽은 시점에 켜져 있던 오류 비트들(PCI_STATUS_ERROR_BITS 로 걸러낸 값).
 *          config 읽기 자체가 실패하면 -EIO.
 *
 * config space 오프셋 0x06 의 Status 레지스터에는 패리티 오류, Signaled Target
 * Abort, Received Target/Master Abort, Signaled System Error 같은 사건이
 * 비트로 남는다. 이 비트들은 W1C(Write-1-to-Clear) 라서, 읽은 값을 그대로 다시
 * 쓰면 켜져 있던 비트만 지워지고 그 사이 새로 켜진 비트는 살아남는다.
 * 그래서 "읽고 지우기"를 이 한 함수로 묶어 둔 것이다.
 *
 * 실행 문맥: config 접근이므로 잠들 수 있는 문맥이 안전하다. 별도 락은 잡지 않고
 * config 접근 직렬화는 access.c 의 pci_lock 이 맡는다.
 *
 * 호출 체인:
 *   각 드라이버의 오류 처리 경로 → [이 함수]
 *     → pci_read_config_word() / pci_write_config_word()
 */
int pci_status_get_and_clear_errors(struct pci_dev *pdev)
{
	u16 status; /* [한국어] Status 레지스터 값(16비트)을 받을 곳 */
	int ret; /* [한국어] config 접근 결과 코드 */

	ret = pci_read_config_word(pdev, PCI_STATUS, &status); /* [한국어] config space 오프셋 0x06(PCI_STATUS)에서 2바이트를 읽는다 */
	if (ret != PCIBIOS_SUCCESSFUL) /* [한국어] PCIBIOS_SUCCESSFUL 이 아니면 장치가 응답하지 않았거나 접근이 막힌 것이다 */
		return -EIO; /* [한국어] config 읽기 실패는 -EIO 로 알린다. 상태 비트가 아니라 접근 자체가 실패한 경우다 */

	status &= PCI_STATUS_ERROR_BITS; /* [한국어] 관심 있는 오류 비트만 남긴다. PCI_STATUS_ERROR_BITS 는 패리티/어보트/SERR 계열 비트의 묶음이다 */
	if (status) /* [한국어] 켜진 오류가 하나라도 있을 때만 쓰기를 한다 */
		pci_write_config_word(pdev, PCI_STATUS, status); /* [한국어] 읽은 값을 그대로 되쓴다 — W1C 규약이라 1 인 비트만 지워진다. 0 인 비트에 0 을 써도 아무 일도 없어 안전하다 */

	return status; /* [한국어] 지우기 전에 읽어 둔 오류 비트를 호출자에게 돌려준다 */
}
EXPORT_SYMBOL_GPL(pci_status_get_and_clear_errors);

#ifdef CONFIG_HAS_IOMEM /* [한국어] ioremap 이 있는 아키텍처에서만 컴파일한다. MMIO 창을 커널 가상 주소로 못 잡는 구성에서는 아래 세 함수가 존재하지 않는다 */
/*
 * [한국어]
 * __pci_ioremap_resource - BAR 하나가 가리키는 MMIO 영역을 커널 가상 주소로 잡는다.
 *
 * @pdev: 대상 PCI 장치.
 * @bar: BAR 번호(0~5). PCI 헤더 타입 0 의 BAR 는 config space 0x10 부터 4바이트씩 6개다.
 * @write_combine: true 면 Write-Combining 속성으로, false 면 일반(uncached) 속성으로 매핑.
 * @return: 매핑된 __iomem 포인터. BAR 가 MEM 이 아니거나 주소가 배정되지 않았으면 NULL.
 *
 * 열거 단계에서 pci_dev->resource[bar] 에는 그 BAR 가 차지하는 CPU 물리 주소
 * 구간이 이미 기록돼 있다. 이 함수는 그 구간을 그대로 ioremap 해 준다.
 * 두 가지를 먼저 확인하는 것이 요점이다.
 *   - IORESOURCE_UNSET: 주소가 아직 배정되지 않은 BAR. 매핑하면 엉뚱한 물리
 *     주소를 잡게 되므로 거절한다.
 *   - IORESOURCE_MEM 없음: IO 공간 BAR 다. IO 공간은 inb/outb 로 접근하는
 *     별도 주소 공간이라 ioremap 대상이 아니다.
 * write_combine 은 쓰기를 모아 한 번에 내보내도 되는 영역(프레임버퍼 등)에만
 * 쓴다. 레지스터처럼 쓰기 순서와 시점이 의미를 갖는 영역에 쓰면 안 된다.
 *
 * 실행 문맥: ioremap 은 페이지 테이블을 건드리므로 프로세스 문맥에서 부른다.
 *
 * 호출 체인:
 *   pci_ioremap_bar() / pci_ioremap_wc_bar() → [이 함수] → ioremap() / ioremap_wc()
 */
static void __iomem *__pci_ioremap_resource(struct pci_dev *pdev, int bar,
					    bool write_combine)
{
	struct resource *res = &pdev->resource[bar]; /* [한국어] BAR 번호로 resource 배열을 인덱싱한다. 열거 때 setup-res.c 가 채워 둔 항목이다 */
	resource_size_t start = res->start; /* [한국어] 그 BAR 가 차지하는 CPU 물리 주소의 시작 */
	resource_size_t size = resource_size(res); /* [한국어] end - start + 1. BAR 크기는 항상 2의 거듭제곱이며, 열거 때 "BAR 에 전부 1 을 쓰고 되읽어" 알아낸 값이다 */

	/*
	 * Make sure the BAR is actually a memory resource, not an IO resource
	 */
	if (res->flags & IORESOURCE_UNSET || !(res->flags & IORESOURCE_MEM)) { /* [한국어] IORESOURCE_UNSET 이면 주소가 아직 배정되지 않은 BAR 이고, IORESOURCE_MEM 이 없으면 IO 공간 BAR 다. 둘 다 ioremap 대상이 아니다 */
		pci_err(pdev, "can't ioremap BAR %d: %pR\n", bar, res); /* [한국어] %pR 은 resource 를 "[mem 0x... - 0x...]" 꼴로 찍어 주는 커널 전용 포맷이다 */
		return NULL; /* [한국어] 매핑 실패는 NULL 로 알린다. 호출자는 이 값을 그대로 상위에 전달한다 */
	}

	if (write_combine) /* [한국어] WC 매핑 요청이면 */
		return ioremap_wc(start, size); /* [한국어] 쓰기 결합을 허용하는 속성으로 매핑한다. 쓰기가 버퍼에 모였다 한꺼번에 나가므로 레지스터에는 쓰면 안 된다 */

	return ioremap(start, size); /* [한국어] 기본은 uncached 매핑 — 매 readl/writel 이 실제 버스 트랜잭션이 되어 레지스터 접근의 순서와 시점이 보장된다 */
}

/*
 * [한국어]
 * pci_ioremap_bar - BAR 하나를 일반(uncached) 속성으로 ioremap 하는 공개 래퍼.
 *
 * @pdev: 대상 PCI 장치.
 * @bar: BAR 번호(0~5).
 * @return: 매핑된 __iomem 포인터, 실패 시 NULL.
 *
 * 장치 레지스터 창을 잡는 가장 흔한 방법이다. write_combine=false 로 고정하므로
 * 매 readl/writel 이 실제 버스 트랜잭션이 되어 순서가 보장된다.
 *
 * 참고 — NVMe 드라이버는 이 함수를 쓰지 않는다. drivers/nvme/host/pci.c 의
 * nvme_remap_bar() 가 pci_resource_start(pdev, 0) 로 BAR0 시작 주소만 얻은 뒤
 * ioremap() 을 직접 부른다. 도어벨 개수가 큐 수에 따라 달라져 매핑 크기를
 * 나중에 늘려야 하기 때문에, 크기를 스스로 정하는 쪽을 택한 것이다.
 *
 * 실행 문맥: 프로세스 문맥.
 *
 * 호출 체인:
 *   각 드라이버의 probe → [이 함수] → __pci_ioremap_resource() → ioremap()
 */
void __iomem *pci_ioremap_bar(struct pci_dev *pdev, int bar)
{
	return __pci_ioremap_resource(pdev, bar, false); /* [한국어] write_combine=false — 레지스터 접근용 일반 매핑 */
}
EXPORT_SYMBOL_GPL(pci_ioremap_bar);

/*
 * [한국어]
 * pci_ioremap_wc_bar - BAR 하나를 Write-Combining 속성으로 ioremap 하는 공개 래퍼.
 *
 * @pdev: 대상 PCI 장치.
 * @bar: BAR 번호(0~5).
 * @return: 매핑된 __iomem 포인터, 실패 시 NULL.
 *
 * WC 매핑은 연속된 쓰기를 CPU 가 모아 한 번의 큰 버스트로 내보내게 한다.
 * 프레임버퍼처럼 "많이 쓰고, 쓰기 순서가 중요하지 않은" 영역에서 대역폭이
 * 크게 오른다. 반대로 레지스터에 쓰면 순서가 뒤집혀 장치가 오동작한다.
 *
 * 실행 문맥: 프로세스 문맥.
 *
 * 호출 체인:
 *   각 드라이버의 probe → [이 함수] → __pci_ioremap_resource() → ioremap_wc()
 */
void __iomem *pci_ioremap_wc_bar(struct pci_dev *pdev, int bar)
{
	return __pci_ioremap_resource(pdev, bar, true); /* [한국어] write_combine=true — 쓰기 결합 매핑 */
}
EXPORT_SYMBOL_GPL(pci_ioremap_wc_bar);
#endif

/**
 * pci_dev_str_match_path - test if a path string matches a device
 * @dev: the PCI device to test
 * @path: string to match the device against
 * @endptr: pointer to the string after the match
 *
 * Test if a string (typically from a kernel parameter) formatted as a
 * path of device/function addresses matches a PCI device. The string must
 * be of the form:
 *
 *   [<domain>:]<bus>:<device>.<func>[/<device>.<func>]*
 *
 * A path for a device can be obtained using 'lspci -t'.  Using a path
 * is more robust against bus renumbering than using only a single bus,
 * device and function address.
 *
 * Returns 1 if the string matches the device, 0 if it does not and
 * a negative error code if it fails to parse the string.
 */
/*
 * [한국어]
 * pci_dev_str_match_path - "경로" 형식 문자열이 이 장치를 가리키는지 판정한다.
 *
 * @dev: 검사할 PCI 장치.
 * @path: "[<domain>:]<bus>:<dev>.<func>[/<dev>.<func>]*" 형식 문자열.
 * @endptr: 매치에 쓴 부분 바로 뒤(다음 ';' 위치)를 돌려줄 곳.
 * @return: 일치하면 1, 불일치면 0, 파싱 실패면 음수 오류(-EINVAL/-ENOMEM).
 *
 * 커널 파라미터로 특정 장치를 지목할 때 "0000:01:00.0" 처럼 주소만 쓰면
 * 하드웨어를 추가하거나 펌웨어가 바뀔 때 버스 번호가 밀려 엉뚱한 장치를
 * 가리키게 된다. 그래서 루트부터의 브리지 경로를 함께 적는 형식을 지원한다
 * (lspci -t 로 얻을 수 있다). 경로는 번호 재배정에 훨씬 강하다.
 *
 * 판정은 뒤에서 앞으로 진행한다. 문자열 끝의 "/<dev>.<func>" 를 하나 떼어
 * 지금 장치의 devfn 과 맞춰 보고, 맞으면 상위 브리지로 올라가며 반복한다.
 * 마지막에 "/" 가 없어지면 남은 앞부분이 루트 쪽 "[domain:]bus:dev.func" 이다.
 *
 * 실행 문맥: GFP_ATOMIC 으로 복사본을 뜬다 — 이 함수가 잠들 수 없는 문맥에서도
 * 불릴 수 있기 때문이다. 상위 브리지로 올라갈 때 참조 계수를 따로 올리지 않는
 * 이유는 원본 주석대로, 맨 아래 장치의 참조가 위쪽 브리지들의 수명을
 * 이미 붙들고 있기 때문이다.
 *
 * 호출 체인:
 *   pci_dev_str_match() → [이 함수] → pci_upstream_bridge(), sscanf()
 */
static int pci_dev_str_match_path(struct pci_dev *dev, const char *path,
				  const char **endptr)
{
	int ret; /* [한국어] 중간 반환값과 최종 판정값을 함께 담는다 */
	unsigned int seg, bus, slot, func; /* [한국어] 파싱해 낼 도메인/버스/장치/함수 번호 */
	char *wpath, *p; /* [한국어] wpath 는 잘라 쓰기 위한 복사본, p 는 마지막 "/" 위치 */
	char end; /* [한국어] sscanf 가 형식 뒤에 남는 문자를 잡아내려고 두는 자리 — 여기에 뭔가 들어오면 형식이 어긋난 것이다 */

	*endptr = strchrnul(path, ';'); /* [한국어] 이 항목은 ';' 에서 끝난다. strchrnul 은 못 찾으면 NUL 위치를 주므로 별도 분기가 필요 없다 */

	wpath = kmemdup_nul(path, *endptr - path, GFP_ATOMIC); /* [한국어] ';' 앞까지만 NUL 로 끝나게 복사한다. GFP_ATOMIC 은 잠들 수 없는 문맥에서도 부를 수 있게 하려는 선택이다 */
	if (!wpath) /* [한국어] 할당 실패 */
		return -ENOMEM; /* [한국어] -ENOMEM 으로 알린다. 여기서는 아직 해제할 것이 없다 */

	while (1) { /* [한국어] 경로 성분이 남아 있는 동안 뒤에서부터 하나씩 떼어 낸다 */
		p = strrchr(wpath, '/'); /* [한국어] 가장 마지막 "/" — 즉 가장 아래쪽(장치에 가장 가까운) 성분의 시작 */
		if (!p) /* [한국어] "/" 가 없으면 남은 것은 루트 쪽 "bus:dev.func" 뿐이다 */
			break; /* [한국어] 루프를 빠져나가 아래에서 그 부분을 파싱한다 */
		ret = sscanf(p, "/%x.%x%c", &slot, &func, &end); /* [한국어] "/<slot>.<func>" 를 16진수로 읽는다. %c 로 잉여 문자를 잡는다 */
		if (ret != 2) { /* [한국어] 정확히 두 항목만 읽혀야 정상 — 세 개가 읽혔다면 뒤에 군더더기가 붙은 것이다 */
			ret = -EINVAL; /* [한국어] 형식 오류 */
			goto free_and_exit; /* [한국어] 복사본을 해제하고 나간다 */
		}

		if (dev->devfn != PCI_DEVFN(slot, func)) { /* [한국어] PCI_DEVFN(slot, func) 는 (slot << 3) | func — devfn 은 상위 5비트가 장치 번호, 하위 3비트가 함수 번호다 */
			ret = 0; /* [한국어] 이 성분에서 이미 어긋났으므로 "불일치"다. 오류가 아니라 정상적인 0 */
			goto free_and_exit; /* [한국어] 해제 후 0 반환 */
		}

		/*
		 * Note: we don't need to get a reference to the upstream
		 * bridge because we hold a reference to the top level
		 * device which should hold a reference to the bridge,
		 * and so on.
		 */
		dev = pci_upstream_bridge(dev); /* [한국어] 한 단계 위 브리지로 올라간다. 경로를 뒤에서 앞으로 훑는 중이다 */
		if (!dev) { /* [한국어] 루트에 닿았는데 경로 성분이 아직 남아 있다면 경로가 더 길다는 뜻이므로 */
			ret = 0; /* [한국어] 불일치 */
			goto free_and_exit; /* [한국어] 해제 후 0 반환 */
		}

		*p = 0; /* [한국어] 방금 처리한 "/..." 성분을 잘라 내 문자열을 짧게 만든다. 다음 반복에서 그 앞 성분이 마지막이 된다 */
	}

	ret = sscanf(wpath, "%x:%x:%x.%x%c", &seg, &bus, &slot, /* [한국어] 남은 앞부분을 "<domain>:<bus>:<dev>.<func>" 로 읽어 본다 */
		     &func, &end); /* [한국어] end 는 잉여 문자 검출용 — 네 항목만 읽혀야 정상이다 */
	if (ret != 4) { /* [한국어] 네 개가 아니면 도메인이 생략된 형식일 수 있다 */
		seg = 0; /* [한국어] 도메인 생략 시 0 으로 본다(원본 주석대로의 규약) */
		ret = sscanf(wpath, "%x:%x.%x%c", &bus, &slot, &func, &end); /* [한국어] "<bus>:<dev>.<func>" 로 다시 읽는다 */
		if (ret != 3) { /* [한국어] 이번에도 어긋나면 파싱 실패다 */
			ret = -EINVAL; /* [한국어] 형식 오류 */
			goto free_and_exit; /* [한국어] 해제 후 나간다 */
		}
	}

	ret = (seg == pci_domain_nr(dev->bus) && /* [한국어] 도메인(세그먼트) 번호 비교. 도메인은 버스 번호 공간을 통째로 나누는 상위 개념이다 */
	       bus == dev->bus->number && /* [한국어] 버스 번호 비교 */
	       dev->devfn == PCI_DEVFN(slot, func)); /* [한국어] devfn 비교. 셋이 모두 같아야 1(일치)이 된다 */

free_and_exit: /* [한국어] 성공·실패 모두 이 지점으로 모여 복사본을 반드시 해제한다 */
	kfree(wpath); /* [한국어] kmemdup_nul 로 잡은 작업용 복사본을 돌려준다 */
	return ret; /* [한국어] 1(일치) / 0(불일치) / 음수(오류) 중 하나 */
}

/**
 * pci_dev_str_match - test if a string matches a device
 * @dev: the PCI device to test
 * @p: string to match the device against
 * @endptr: pointer to the string after the match
 *
 * Test if a string (typically from a kernel parameter) matches a specified
 * PCI device. The string may be of one of the following formats:
 *
 *   [<domain>:]<bus>:<device>.<func>[/<device>.<func>]*
 *   pci:<vendor>:<device>[:<subvendor>:<subdevice>]
 *
 * The first format specifies a PCI bus/device/function address which
 * may change if new hardware is inserted, if motherboard firmware changes,
 * or due to changes caused in kernel parameters. If the domain is
 * left unspecified, it is taken to be 0.  In order to be robust against
 * bus renumbering issues, a path of PCI device/function numbers may be used
 * to address the specific device.  The path for a device can be determined
 * through the use of 'lspci -t'.
 *
 * The second format matches devices using IDs in the configuration
 * space which may match multiple devices in the system. A value of 0
 * for any field will match all devices. (Note: this differs from
 * in-kernel code that uses PCI_ANY_ID which is ~0; this is for
 * legacy reasons and convenience so users don't have to specify
 * FFFFFFFFs on the command line.)
 *
 * Returns 1 if the string matches the device, 0 if it does not and
 * a negative error code if the string cannot be parsed.
 */
/*
 * [한국어]
 * pci_dev_str_match - 커널 파라미터 문자열 하나가 이 장치를 가리키는지 판정한다.
 *
 * @dev: 검사할 PCI 장치.
 * @p: 판정할 문자열. 두 가지 형식을 받는다.
 *     1) "[<domain>:]<bus>:<dev>.<func>[/<dev>.<func>]*"  — 주소(또는 경로)
 *     2) "pci:<vendor>:<device>[:<subvendor>:<subdevice>]" — ID
 * @endptr: 이 항목을 소비하고 난 뒤의 위치를 돌려줄 곳. 호출자가 ';' 로 이어진
 *          목록을 계속 훑을 수 있게 한다.
 * @return: 일치 1, 불일치 0, 파싱 실패 시 음수 오류.
 *
 * pci=resource_alignment=, pci=disable_acs_redir= 처럼 "특정 장치들에만" 적용할
 * 파라미터를 해석하는 공통 판정기다. 주소 형식은 한 장치를 정확히 집지만 버스
 * 번호 재배정에 약하고, ID 형식은 같은 모델 전부를 한꺼번에 집는다.
 * ID 형식에서 0 은 "아무 값이나"를 뜻한다 — 커널 내부의 PCI_ANY_ID(~0)와
 * 다른데, 사용자가 명령줄에 FFFF 를 적지 않아도 되게 한 편의다(원본 주석).
 *
 * 실행 문맥: 문자열 파싱뿐이고 config space 접근이 없다. 하위 호출
 * pci_dev_str_match_path() 가 GFP_ATOMIC 을 쓰므로 잠들 수 없는 문맥도 허용한다.
 *
 * 호출 체인:
 *   pci_specified_resource_alignment(), pci_disable_acs_redir 처리 경로
 *     → [이 함수] → pci_dev_str_match_path()
 */
static int pci_dev_str_match(struct pci_dev *dev, const char *p,
			     const char **endptr)
{
	int ret; /* [한국어] 하위 호출 결과와 최종 판정값 */
	int count; /* [한국어] sscanf 의 %n 이 돌려줄 "소비한 문자 수" — 이 문자열을 어디까지 먹었는지 알아야 endptr 을 옮길 수 있다 */
	unsigned short vendor, device, subsystem_vendor, subsystem_device; /* [한국어] ID 형식에서 뽑아낼 네 가지 식별자. PCI config space 의 0x00(Vendor), 0x02(Device), 0x2c(Subsystem Vendor), 0x2e(Subsystem Device)에 대응한다 */

	if (strncmp(p, "pci:", 4) == 0) { /* [한국어] "pci:" 접두가 붙었으면 ID 형식이다 */
		/* PCI vendor/device (subvendor/subdevice) IDs are specified */
		p += 4; /* [한국어] 접두 4글자를 건너뛴다 */
		ret = sscanf(p, "%hx:%hx:%hx:%hx%n", &vendor, &device, /* [한국어] 네 항목 형식을 먼저 시도한다. %hx 는 unsigned short 16진수 */
			     &subsystem_vendor, &subsystem_device, &count); /* [한국어] %n 은 값을 소비하지 않고 지금까지 읽은 문자 수만 count 에 넣는다 */
		if (ret != 4) { /* [한국어] 네 개가 다 읽히지 않았으면 subsystem 을 생략한 두 항목 형식일 수 있다 */
			ret = sscanf(p, "%hx:%hx%n", &vendor, &device, &count); /* [한국어] vendor:device 만 읽어 본다 */
			if (ret != 2) /* [한국어] 그것마저 실패하면 형식 오류다 */
				return -EINVAL; /* [한국어] -EINVAL. 이 경로에서는 아직 할당한 자원이 없어 바로 반환해도 된다 */

			subsystem_vendor = 0; /* [한국어] 생략된 subsystem 은 0 으로 둔다 — 아래 비교에서 0 은 "아무 값이나 허용"으로 쓰인다 */
			subsystem_device = 0; /* [한국어] 같은 이유로 subsystem device 도 0 */
		}

		p += count; /* [한국어] 소비한 만큼 커서를 앞으로 옮긴다. 호출자에게 돌려줄 endptr 의 기준이 된다 */

		if ((!vendor || vendor == dev->vendor) && /* [한국어] vendor 가 0 이면 무조건 통과, 아니면 실제 Vendor ID 와 같아야 한다 */
		    (!device || device == dev->device) && /* [한국어] device 도 같은 규칙 */
		    (!subsystem_vendor || /* [한국어] subsystem vendor 가 0 이면 검사를 건너뛰고 */
			    subsystem_vendor == dev->subsystem_vendor) && /* [한국어] 0 이 아니면 값이 일치해야 한다 */
		    (!subsystem_device || /* [한국어] subsystem device 도 마찬가지로 0 이면 무시하고 */
			    subsystem_device == dev->subsystem_device)) /* [한국어] 0 이 아니면 일치해야 한다 */
			goto found; /* [한국어] 네 조건이 모두 통과하면 일치 처리 지점으로 간다 */
	} else {
		/*
		 * PCI Bus, Device, Function IDs are specified
		 * (optionally, may include a path of devfns following it)
		 */
		ret = pci_dev_str_match_path(dev, p, &p); /* [한국어] 경로 판정기에 넘긴다. 세 번째 인자로 &p 를 주어 소비한 끝 위치를 돌려받는다 */
		if (ret < 0) /* [한국어] 음수는 파싱 실패 */
			return ret; /* [한국어] 그대로 상위에 전달한다 */
		else if (ret) /* [한국어] 1 이면 일치 */
			goto found; /* [한국어] 일치 처리 지점으로 */
	}

	*endptr = p; /* [한국어] 불일치로 끝나더라도 이 항목을 어디까지 읽었는지는 알려 줘야 호출자가 다음 항목으로 넘어갈 수 있다 */
	return 0; /* [한국어] 불일치 */

found: /* [한국어] 일치했을 때만 오는 지점 */
	*endptr = p; /* [한국어] 여기서도 소비 위치를 알려 준다 */
	return 1; /* [한국어] 일치 */
}

/*
 * [한국어]
 * __pci_find_next_cap - capability 연결 리스트를 순회해 원하는 ID 를 찾는다.
 *
 * @bus: 대상 장치가 붙은 버스. pci_dev 없이도 쓸 수 있게 bus/devfn 을 따로 받는다.
 * @devfn: 장치·함수 번호((device << 3) | function).
 * @pos: 순회를 시작할 config space 오프셋. "다음 포인터가 들어 있는 위치"를 준다.
 * @cap: 찾을 capability ID(PCI_CAP_ID_PM=0x01, PCI_CAP_ID_MSI=0x05,
 *       PCI_CAP_ID_PCIX=0x07, PCI_CAP_ID_EXP=0x10, PCI_CAP_ID_MSIX=0x11 등).
 * @return: 찾은 capability 구조체의 시작 오프셋, 없으면 0.
 *
 * PCI 표준 capability 는 config space 안에 "다음 항목 오프셋"으로 이어진
 * 단방향 연결 리스트다. 각 항목의 첫 바이트가 ID, 둘째 바이트가 다음 항목의
 * 오프셋이며, 다음 오프셋 0 이 리스트의 끝이다.
 * 실제 순회는 drivers/pci/pci.h 의 PCI_FIND_NEXT_CAP 매크로가 한다. 그 매크로가
 * 지키는 규약은 세 가지다.
 *   - TTL 48회 제한: 링크가 자기 자신을 가리키는 고장난 하드웨어에서
 *     무한 루프에 빠지지 않게 끊는다.
 *   - PCI_STD_HEADER_SIZEOF(0x40) 미만이면 중단: capability 는 표준 헤더
 *     64바이트 뒤쪽에만 놓일 수 있으므로, 그보다 작은 오프셋은 잘못된 링크다.
 *   - ALIGN_DOWN(pos, 4): capability 는 DWORD 경계에 정렬돼 있어야 한다.
 *   - 읽은 ID 가 0xff 면 중단: 응답 없는 config 읽기가 all-ones 를 돌려주므로,
 *     장치가 사라진 경우를 리스트 끝과 함께 걸러 낸다.
 * 매크로에 pci_bus_read_config 를 넘기므로 pci_dev 가 아직 없어도 동작한다.
 *
 * 실행 문맥: config space 를 여러 번 읽는다. 프로세스 문맥에서 부르는 것이 안전하다.
 *
 * 호출 체인:
 *   pci_find_capability() / pci_bus_find_capability() / pci_find_next_capability()
 *     → [이 함수] → PCI_FIND_NEXT_CAP → pci_bus_read_config_byte/_word()
 */
static u8 __pci_find_next_cap(struct pci_bus *bus, unsigned int devfn,
			      u8 pos, int cap)
{
	return PCI_FIND_NEXT_CAP(pci_bus_read_config, pos, cap, NULL, bus, devfn); /* [한국어] prev_ptr 에 NULL 을 넘겨 "직전 항목 위치는 필요 없다"고 알린다. args 로 bus/devfn 이 매크로 안의 pci_bus_read_config_byte/_word 호출에 전달된다 */
}

/*
 * [한국어]
 * pci_find_next_capability - 이미 찾은 capability 다음부터 같은 ID 를 이어서 찾는다.
 *
 * @dev: 대상 PCI 장치.
 * @pos: 직전에 찾은 capability 의 시작 오프셋.
 * @cap: 찾을 capability ID.
 * @return: 다음에 나오는 같은 ID 의 오프셋, 더 없으면 0.
 *
 * 같은 ID 의 capability 가 여러 개 있을 수 있는 경우(Vendor-Specific 등)를 위해
 * 존재한다. 시작점을 pos + PCI_CAP_LIST_NEXT 로 주는 것이 핵심이다 —
 * PCI_CAP_LIST_NEXT 는 capability 헤더 안에서 "다음 포인터" 필드의 오프셋(1)이고,
 * 매크로는 시작 위치의 바이트를 먼저 읽어 그것을 첫 후보 오프셋으로 삼기 때문이다.
 * 즉 "현재 항목의 next 필드부터 이어서 훑어라"는 뜻이 된다.
 *
 * 실행 문맥: config space 읽기. 프로세스 문맥.
 *
 * 참고 — drivers/nvme/ 전체를 검색해도 이 함수 호출은 없다.
 *
 * 호출 체인:
 *   각 드라이버/quirk → [이 함수] → __pci_find_next_cap()
 */
u8 pci_find_next_capability(struct pci_dev *dev, u8 pos, int cap)
{
	return __pci_find_next_cap(dev->bus, dev->devfn, /* [한국어] bus/devfn 을 풀어 하위 헬퍼에 넘긴다 */
				   pos + PCI_CAP_LIST_NEXT, cap); /* [한국어] 현재 capability 의 next 필드 위치에서 시작하게 한다 */
}
EXPORT_SYMBOL_GPL(pci_find_next_capability);

/*
 * [한국어]
 * __pci_bus_find_cap_start - capability 리스트의 머리 포인터가 놓인 오프셋을 구한다.
 *
 * @bus: 대상 버스.
 * @devfn: 장치·함수 번호.
 * @hdr_type: config space 0x0e 의 Header Type 하위 7비트.
 * @return: 리스트 머리 포인터가 들어 있는 config 오프셋, capability 가 없으면 0.
 *
 * capability 리스트를 훑기 전에 "어디서 시작하는가"를 먼저 알아야 한다.
 * 두 단계로 판단한다.
 *   1) Status 레지스터(0x06)의 Capabilities List 비트를 본다. 이 비트가 0 이면
 *      이 장치에는 capability 가 하나도 없다 — 더 볼 것도 없이 0.
 *   2) 머리 포인터의 위치는 헤더 타입마다 다르다.
 *      일반 장치(타입 0)와 브리지(타입 1)는 0x34(PCI_CAPABILITY_LIST),
 *      CardBus 브리지(타입 2)는 0x14(PCI_CB_CAPABILITY_LIST) 다.
 *      헤더 타입 0/1 은 헤더 레이아웃이 달라도 이 필드 위치만은 같다.
 * NVMe SSD 는 헤더 타입 0 이므로 언제나 0x34 를 쓴다.
 *
 * 실행 문맥: config space 읽기 한 번. 프로세스 문맥이 안전하다.
 *
 * 호출 체인:
 *   pci_find_capability() / pci_bus_find_capability()
 *     → [이 함수] → pci_bus_read_config_word()
 */
static u8 __pci_bus_find_cap_start(struct pci_bus *bus,
				    unsigned int devfn, u8 hdr_type)
{
	u16 status; /* [한국어] Status 레지스터 값 */

	pci_bus_read_config_word(bus, devfn, PCI_STATUS, &status); /* [한국어] config space 오프셋 0x06 에서 Status 를 읽는다 */
	if (!(status & PCI_STATUS_CAP_LIST)) /* [한국어] PCI_STATUS_CAP_LIST(비트 4)가 0 이면 이 장치에는 capability 리스트 자체가 없다 */
		return 0; /* [한국어] 0 은 "머리 포인터 없음"을 뜻한다 */

	switch (hdr_type) { /* [한국어] 헤더 타입에 따라 머리 포인터의 위치가 다르다 */
	case PCI_HEADER_TYPE_NORMAL: /* [한국어] 타입 0 — 일반 장치(NVMe SSD 가 여기 해당) */
	case PCI_HEADER_TYPE_BRIDGE: /* [한국어] 타입 1 — PCI-to-PCI 브리지. 레이아웃은 달라도 capability 포인터 위치는 같다 */
		return PCI_CAPABILITY_LIST; /* [한국어] PCI_CAPABILITY_LIST = config space 오프셋 0x34 */
	case PCI_HEADER_TYPE_CARDBUS: /* [한국어] 타입 2 — CardBus 브리지 */
		return PCI_CB_CAPABILITY_LIST; /* [한국어] PCI_CB_CAPABILITY_LIST = config space 오프셋 0x14. CardBus 헤더는 0x34 를 다른 용도로 쓴다 */
	}

	return 0; /* [한국어] 알 수 없는 헤더 타입이면 안전하게 "없음"으로 처리한다 */
}

/**
 * pci_find_capability - query for devices' capabilities
 * @dev: PCI device to query
 * @cap: capability code
 *
 * Tell if a device supports a given PCI capability.
 * Returns the address of the requested capability structure within the
 * device's PCI configuration space or 0 in case the device does not
 * support it.  Possible values for @cap include:
 *
 *  %PCI_CAP_ID_PM           Power Management
 *  %PCI_CAP_ID_AGP          Accelerated Graphics Port
 *  %PCI_CAP_ID_VPD          Vital Product Data
 *  %PCI_CAP_ID_SLOTID       Slot Identification
 *  %PCI_CAP_ID_MSI          Message Signalled Interrupts
 *  %PCI_CAP_ID_CHSWP        CompactPCI HotSwap
 *  %PCI_CAP_ID_PCIX         PCI-X
 *  %PCI_CAP_ID_EXP          PCI Express
 */
/*
 * [한국어]
 * pci_find_capability - 이 장치가 특정 capability 를 갖고 있는지 보고 그 오프셋을 준다.
 *
 * @dev: 대상 PCI 장치.
 * @cap: 찾을 capability ID.
 * @return: capability 구조체가 시작하는 config space 오프셋, 없으면 0.
 *
 * PCI 장치의 선택 기능은 config space 안에 "capability 구조체"로 붙어 있고,
 * 그 목록을 훑어야 지원 여부와 레지스터 위치를 알 수 있다. 전원 관리(PM),
 * MSI, MSI-X, PCI Express capability 가 모두 이 방식으로 노출된다.
 * 반환값 0 은 "지원하지 않음"이며, capability 는 0x40 이후에만 놓일 수 있으므로
 * 0 을 유효한 오프셋과 혼동할 일이 없다.
 *
 * 이 파일 안에서는 pci_pm_init() 이 PCI_CAP_ID_PM 을, pci_save_pcix_state() 가
 * PCI_CAP_ID_PCIX 를 찾는 데 쓴다.
 *
 * 참고 — drivers/nvme/ 전체를 검색해도 이 함수를 직접 부르는 곳은 없다.
 * NVMe 는 MSI-X 를 pci_alloc_irq_vectors_affinity() 로 요청하고, 그 안쪽
 * drivers/pci/msi/ 코드가 대신 이 계열 함수로 capability 를 찾는다.
 * 또 PCIe capability 위치는 열거 때 dev->pcie_cap 에 이미 캐시돼 있어
 * pcie_capability_read_word() 같은 헬퍼가 리스트를 다시 훑지 않는다.
 *
 * 실행 문맥: config space 를 여러 번 읽으므로 프로세스 문맥이 안전하다. 락은 없다.
 *
 * 호출 체인:
 *   드라이버/PCI 코어 → [이 함수]
 *     → __pci_bus_find_cap_start() → __pci_find_next_cap()
 */
u8 pci_find_capability(struct pci_dev *dev, int cap)
{
	u8 pos; /* [한국어] 찾은 오프셋을 담는다 */

	pos = __pci_bus_find_cap_start(dev->bus, dev->devfn, dev->hdr_type); /* [한국어] 먼저 리스트 머리 포인터가 놓인 위치를 구한다 */
	if (pos) /* [한국어] 0 이면 capability 가 없는 장치이므로 순회할 것도 없다 */
		pos = __pci_find_next_cap(dev->bus, dev->devfn, pos, cap); /* [한국어] 머리부터 리스트를 훑어 원하는 ID 를 찾는다 */

	return pos; /* [한국어] 찾은 오프셋 또는 0 */
}
EXPORT_SYMBOL(pci_find_capability);

/**
 * pci_bus_find_capability - query for devices' capabilities
 * @bus: the PCI bus to query
 * @devfn: PCI device to query
 * @cap: capability code
 *
 * Like pci_find_capability() but works for PCI devices that do not have a
 * pci_dev structure set up yet.
 *
 * Returns the address of the requested capability structure within the
 * device's PCI configuration space or 0 in case the device does not
 * support it.
 */
/*
 * [한국어]
 * pci_bus_find_capability - pci_dev 가 아직 없는 장치에서 capability 를 찾는다.
 *
 * @bus: 장치가 붙은 버스.
 * @devfn: 장치·함수 번호.
 * @cap: 찾을 capability ID.
 * @return: capability 오프셋, 없으면 0.
 *
 * pci_find_capability() 와 하는 일은 같지만, struct pci_dev 가 만들어지기 전
 * 단계에서도 쓸 수 있다. 열거(probe.c) 도중에는 아직 pci_dev 가 없어
 * bus + devfn 만으로 config space 를 두드려야 하기 때문이다.
 * 그래서 캐시된 dev->hdr_type 대신 config space 0x0e 를 직접 읽는다.
 *
 * 실행 문맥: config space 읽기. 프로세스 문맥.
 *
 * 호출 체인:
 *   probe 이전 단계의 PCI 코어 코드 → [이 함수]
 *     → __pci_bus_find_cap_start() → __pci_find_next_cap()
 */
u8 pci_bus_find_capability(struct pci_bus *bus, unsigned int devfn, int cap)
{
	u8 hdr_type, pos; /* [한국어] 헤더 타입과 찾은 오프셋 */

	pci_bus_read_config_byte(bus, devfn, PCI_HEADER_TYPE, &hdr_type); /* [한국어] config space 오프셋 0x0e 에서 Header Type 을 1바이트 읽는다 */

	pos = __pci_bus_find_cap_start(bus, devfn, hdr_type & PCI_HEADER_TYPE_MASK); /* [한국어] PCI_HEADER_TYPE_MASK(0x7f)로 하위 7비트만 남긴다 — 최상위 비트는 헤더 타입이 아니라 "멀티펑션 장치"를 뜻하는 별개 플래그다 */
	if (pos) /* [한국어] 머리 포인터가 없으면 순회하지 않는다 */
		pos = __pci_find_next_cap(bus, devfn, pos, cap); /* [한국어] 리스트를 훑는다 */

	return pos; /* [한국어] 찾은 오프셋 또는 0 */
}
EXPORT_SYMBOL(pci_bus_find_capability);

/**
 * pci_find_next_ext_capability - Find an extended capability
 * @dev: PCI device to query
 * @start: address at which to start looking (0 to start at beginning of list)
 * @cap: capability code
 *
 * Returns the address of the next matching extended capability structure
 * within the device's PCI configuration space or 0 if the device does
 * not support it.  Some capabilities can occur several times, e.g., the
 * vendor-specific capability, and this provides a way to find them all.
 */
/*
 * [한국어]
 * pci_find_next_ext_capability - PCIe 확장 capability 리스트를 이어서 훑는다.
 *
 * @dev: 대상 PCI 장치.
 * @start: 순회를 시작할 확장 config 오프셋. 0 이면 리스트 처음(0x100)부터.
 * @cap: 찾을 확장 capability ID(PCI_EXT_CAP_ID_ERR=AER, _ACS, _ATS, _SRIOV, _DSN 등).
 * @return: 찾은 확장 capability 의 오프셋, 없으면 0.
 *
 * PCIe 는 기존 256바이트 config space 로는 모자라 4096바이트로 확장했고,
 * 0x100 부터 "확장 capability" 리스트를 따로 둔다. 표준 capability 리스트와
 * 구조가 다르다.
 *   - 헤더가 4바이트다: 하위 16비트가 ID, 상위 4비트가 버전, 그 사이 12비트가
 *     다음 항목 오프셋. 표준 쪽 8비트 링크보다 넓어 4KB 전체를 가리킬 수 있다.
 *   - 리스트는 항상 0x100 에서 시작한다. 별도의 머리 포인터가 없다.
 * 같은 ID 가 여러 번 나올 수 있어(Vendor-Specific 등) start 로 이어 찾게 했다.
 *
 * 확장 config space 는 접근 방법 자체가 다르다. x86 의 전통적인 0xCF8/0xCFC
 * IO 포트 방식으로는 256바이트까지만 닿고, 0x100 이후는 ECAM(메모리 매핑
 * config)이 있어야 읽을 수 있다. dev->cfg_size 가 그 가능 범위를 담고 있어
 * 맨 먼저 확인한다.
 *
 * 실행 문맥: config space 를 여러 번 읽는다. 프로세스 문맥이 안전하다.
 *
 * 호출 체인:
 *   pci_find_ext_capability(), pci_find_vsec_capability(), AER/ACS/SR-IOV 코드
 *     → [이 함수] → PCI_FIND_NEXT_EXT_CAP → pci_bus_read_config_dword()
 */
u16 pci_find_next_ext_capability(struct pci_dev *dev, u16 start, int cap)
{
	if (dev->cfg_size <= PCI_CFG_SPACE_SIZE) /* [한국어] PCI_CFG_SPACE_SIZE 는 256. 확장 config 에 닿을 수 없는 플랫폼/장치면 확장 capability 도 있을 수 없다 */
		return 0; /* [한국어] "없음"을 뜻하는 0 */

	return PCI_FIND_NEXT_EXT_CAP(pci_bus_read_config, start, cap, /* [한국어] 매크로가 0x100 부터(또는 start 부터) 12비트 링크를 따라간다. 여기서도 TTL 로 무한 루프를 막는다 */
				     NULL, dev->bus, dev->devfn); /* [한국어] prev_ptr 은 NULL — 직전 항목 위치는 필요 없다. bus/devfn 이 config 읽기 인자로 전달된다 */
}
EXPORT_SYMBOL_GPL(pci_find_next_ext_capability);

/**
 * pci_find_ext_capability - Find an extended capability
 * @dev: PCI device to query
 * @cap: capability code
 *
 * Returns the address of the requested extended capability structure
 * within the device's PCI configuration space or 0 if the device does
 * not support it.  Possible values for @cap include:
 *
 *  %PCI_EXT_CAP_ID_ERR		Advanced Error Reporting
 *  %PCI_EXT_CAP_ID_VC		Virtual Channel
 *  %PCI_EXT_CAP_ID_DSN		Device Serial Number
 *  %PCI_EXT_CAP_ID_PWR		Power Budgeting
 */
/*
 * [한국어]
 * pci_find_ext_capability - 확장 capability 를 리스트 처음부터 찾는다.
 *
 * @dev: 대상 PCI 장치.
 * @cap: 찾을 확장 capability ID.
 * @return: 오프셋(0x100 이상), 없으면 0.
 *
 * pci_find_next_ext_capability(dev, 0, cap) 의 얇은 래퍼다. start=0 은
 * "리스트 처음(0x100)부터"를 뜻한다.
 * AER(오류 보고), ACS(피어 간 접근 제어), ATS, SR-IOV, DSN(일련번호)이 모두
 * 확장 capability 이므로, PCI 코어의 오류 복구·가상화 코드가 자주 부른다.
 *
 * 참고 — drivers/nvme/ 전체를 검색해도 이 함수 호출은 없다. NVMe 장치의 AER
 * 처리는 NVMe 드라이버가 아니라 drivers/pci/pcie/aer.c 가 맡고, 드라이버는
 * pci_error_handlers 콜백으로 통보만 받는다.
 *
 * 실행 문맥: config space 읽기. 프로세스 문맥.
 *
 * 호출 체인:
 *   AER/ACS/SR-IOV/DSN 코드 → [이 함수] → pci_find_next_ext_capability()
 */
u16 pci_find_ext_capability(struct pci_dev *dev, int cap)
{
	return pci_find_next_ext_capability(dev, 0, cap); /* [한국어] start=0 으로 넘겨 리스트 처음부터 훑게 한다 */
}
EXPORT_SYMBOL_GPL(pci_find_ext_capability);

/**
 * pci_get_dsn - Read and return the 8-byte Device Serial Number
 * @dev: PCI device to query
 *
 * Looks up the PCI_EXT_CAP_ID_DSN and reads the 8 bytes of the Device Serial
 * Number.
 *
 * Returns the DSN, or zero if the capability does not exist.
 */
/*
 * [한국어]
 * pci_get_dsn - Device Serial Number 확장 capability 에서 64비트 일련번호를 읽는다.
 *
 * @dev: 대상 PCI 장치.
 * @return: 8바이트 일련번호. DSN capability 가 없으면 0.
 *
 * DSN 은 장치마다 유일한 64비트 값으로, 같은 모델이 여러 개 꽂혔을 때 각각을
 * 구별하는 데 쓴다. 버스 번호나 슬롯과 달리 재배치·재부팅에도 변하지 않는다.
 * 값의 배치는 스펙이 정해 두었다 — capability 시작에서 4바이트 떨어진 곳에
 * DWORD 두 개가 놓이며, 앞이 하위 32비트, 뒤가 상위 32비트다.
 * 반환값 0 은 "capability 없음"이다. 이론상 일련번호 0 과 구분되지 않지만,
 * 스펙상 0 은 유효한 DSN 이 아니다.
 *
 * 실행 문맥: config space 를 두 번 읽는다. 프로세스 문맥.
 *
 * 호출 체인:
 *   장치를 고유 식별해야 하는 드라이버/서브시스템 → [이 함수]
 *     → pci_find_ext_capability() → pci_read_config_dword()
 */
u64 pci_get_dsn(struct pci_dev *dev)
{
	u32 dword; /* [한국어] config 읽기 한 번당 32비트를 담는 임시 */
	u64 dsn; /* [한국어] 두 DWORD 를 합쳐 만들 64비트 결과 */
	int pos; /* [한국어] DSN capability 의 시작 오프셋 */

	pos = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_DSN); /* [한국어] 확장 capability 리스트에서 DSN(ID 0x0003)을 찾는다 */
	if (!pos) /* [한국어] 이 장치는 DSN 을 노출하지 않는다 */
		return 0; /* [한국어] "없음"을 0 으로 알린다 */

	/*
	 * The Device Serial Number is two dwords offset 4 bytes from the
	 * capability position. The specification says that the first dword is
	 * the lower half, and the second dword is the upper half.
	 */
	pos += 4; /* [한국어] capability 헤더 4바이트를 건너뛴다 — 그 뒤부터가 실제 일련번호다 */
	pci_read_config_dword(dev, pos, &dword); /* [한국어] 앞 DWORD 를 읽는다 */
	dsn = (u64)dword; /* [한국어] 스펙상 앞 DWORD 가 하위 32비트다 */
	pci_read_config_dword(dev, pos + 4, &dword); /* [한국어] 그 다음 DWORD 를 읽는다 */
	dsn |= ((u64)dword) << 32; /* [한국어] 뒤 DWORD 를 상위 32비트로 올려 합친다. (u64) 캐스트를 먼저 하지 않으면 32비트 시프트가 정의되지 않은 동작이 된다 */

	return dsn; /* [한국어] 완성된 64비트 일련번호 */
}
EXPORT_SYMBOL_GPL(pci_get_dsn);

/*
 * [한국어]
 * __pci_find_next_ht_cap - HyperTransport capability 중 원하는 하위 타입을 찾는다.
 *
 * @dev: 대상 PCI 장치.
 * @pos: 순회를 시작할 config 오프셋.
 * @ht_cap: 찾을 HT 하위 타입(HT_CAPTYPE_SLAVE, HT_CAPTYPE_HOST, HT_CAPTYPE_IRQ 등).
 * @return: 조건에 맞는 HT capability 의 오프셋, 없으면 0.
 *
 * HyperTransport 는 AMD 계열 플랫폼이 쓰던 상호연결 규격이다. HT 관련 기능이
 * 여러 개라도 PCI capability ID 는 모두 PCI_CAP_ID_HT(0x08) 하나로 같고,
 * 실제 종류는 capability 안쪽 바이트(오프셋 +3)의 상위 비트로 구분한다.
 * 그래서 "ID 로 찾은 뒤 하위 타입을 다시 확인"하는 두 겹 탐색이 된다.
 *
 * 마스크가 둘인 이유가 이 구조 때문이다. SLAVE/HOST 타입은 상위 3비트로,
 * 나머지 타입은 상위 5비트로 식별한다. 3비트 타입의 나머지 비트에는 다른
 * 의미가 실려 있어 5비트로 비교하면 어긋난다.
 *
 * 실행 문맥: config space 를 반복해 읽는다. 프로세스 문맥.
 * 오류가 나면(장치가 사라졌거나 접근 실패) 즉시 0 을 돌려 루프를 끝낸다.
 *
 * 호출 체인:
 *   pci_find_ht_capability() / pci_find_next_ht_capability()
 *     → [이 함수] → PCI_FIND_NEXT_CAP, pci_read_config_byte()
 */
static u8 __pci_find_next_ht_cap(struct pci_dev *dev, u8 pos, int ht_cap)
{
	int rc; /* [한국어] config 읽기 결과 코드 */
	u8 cap, mask; /* [한국어] cap 은 읽어 온 하위 타입 바이트, mask 는 그중 비교할 비트 범위 */

	if (ht_cap == HT_CAPTYPE_SLAVE || ht_cap == HT_CAPTYPE_HOST) /* [한국어] SLAVE/HOST 는 상위 3비트만으로 구분되는 타입이다 */
		mask = HT_3BIT_CAP_MASK; /* [한국어] 상위 3비트 마스크 */
	else
		mask = HT_5BIT_CAP_MASK; /* [한국어] 상위 5비트로 구분한다 */

	pos = PCI_FIND_NEXT_CAP(pci_bus_read_config, pos, /* [한국어] 먼저 PCI_CAP_ID_HT(0x08) 인 capability 를 찾는다 */
				PCI_CAP_ID_HT, NULL, dev->bus, dev->devfn); /* [한국어] prev_ptr 은 NULL. bus/devfn 이 config 읽기 인자로 넘어간다 */
	while (pos) { /* [한국어] HT capability 를 하나 찾을 때마다 하위 타입을 확인한다 */
		rc = pci_read_config_byte(dev, pos + 3, &cap); /* [한국어] capability 시작에서 3바이트 뒤가 HT 하위 타입 바이트다 */
		if (rc != PCIBIOS_SUCCESSFUL) /* [한국어] 읽기가 실패하면 장치가 응답하지 않는 것이므로 더 훑을 의미가 없다 */
			return 0; /* [한국어] 실패를 "못 찾음"으로 처리한다 */

		if ((cap & mask) == ht_cap) /* [한국어] 마스크로 걸러 낸 상위 비트가 원하는 타입과 같으면 */
			return pos; /* [한국어] 그 오프셋이 답이다 */

		pos = PCI_FIND_NEXT_CAP(pci_bus_read_config, /* [한국어] 아니면 다음 HT capability 로 넘어간다 */
					pos + PCI_CAP_LIST_NEXT, /* [한국어] 현재 항목의 next 필드 위치에서 이어 찾는다 */
					PCI_CAP_ID_HT, NULL, dev->bus, /* [한국어] 같은 ID(PCI_CAP_ID_HT)로 계속 검색한다 */
					dev->devfn); /* [한국어] config 접근에 쓸 devfn */
	}

	return 0; /* [한국어] 리스트를 다 훑도록 맞는 하위 타입이 없었다 */
}

/**
 * pci_find_next_ht_capability - query a device's HyperTransport capabilities
 * @dev: PCI device to query
 * @pos: Position from which to continue searching
 * @ht_cap: HyperTransport capability code
 *
 * To be used in conjunction with pci_find_ht_capability() to search for
 * all capabilities matching @ht_cap. @pos should always be a value returned
 * from pci_find_ht_capability().
 *
 * NB. To be 100% safe against broken PCI devices, the caller should take
 * steps to avoid an infinite loop.
 */
/*
 * [한국어]
 * pci_find_next_ht_capability - 직전 HT capability 다음부터 같은 하위 타입을 찾는다.
 *
 * @dev: 대상 PCI 장치.
 * @pos: pci_find_ht_capability() 가 돌려준 직전 위치.
 * @ht_cap: 찾을 HT 하위 타입.
 * @return: 다음에 나오는 오프셋, 더 없으면 0.
 *
 * HT capability 는 한 장치에 여러 개 있을 수 있어 전부 훑을 방법이 필요하다.
 * pos + PCI_CAP_LIST_NEXT 로 "현재 항목의 next 필드"를 시작점으로 준다.
 * 원본 주석의 경고대로, 링크가 깨진 장치에서 완전히 안전하려면 호출자가
 * 스스로 반복 횟수를 제한해야 한다.
 *
 * 실행 문맥: config space 읽기. 프로세스 문맥.
 *
 * 호출 체인:
 *   HT 관련 quirk/드라이버 → [이 함수] → __pci_find_next_ht_cap()
 */
u8 pci_find_next_ht_capability(struct pci_dev *dev, u8 pos, int ht_cap)
{
	return __pci_find_next_ht_cap(dev, pos + PCI_CAP_LIST_NEXT, ht_cap); /* [한국어] 현재 항목의 next 필드 위치부터 이어 찾게 한다 */
}
EXPORT_SYMBOL_GPL(pci_find_next_ht_capability);

/**
 * pci_find_ht_capability - query a device's HyperTransport capabilities
 * @dev: PCI device to query
 * @ht_cap: HyperTransport capability code
 *
 * Tell if a device supports a given HyperTransport capability.
 * Returns an address within the device's PCI configuration space
 * or 0 in case the device does not support the request capability.
 * The address points to the PCI capability, of type PCI_CAP_ID_HT,
 * which has a HyperTransport capability matching @ht_cap.
 */
/*
 * [한국어]
 * pci_find_ht_capability - HyperTransport 하위 타입을 리스트 처음부터 찾는다.
 *
 * @dev: 대상 PCI 장치.
 * @ht_cap: 찾을 HT 하위 타입.
 * @return: 오프셋, 없으면 0.
 *
 * pci_find_capability() 와 같은 구조지만, ID 만 보는 대신 HT 하위 타입까지
 * 확인하는 __pci_find_next_ht_cap() 을 쓴다.
 * HyperTransport 는 AMD 플랫폼의 상호연결 규격이라 PCIe 엔드포인트인
 * NVMe SSD 자체와는 관계가 없다. 이 경로를 쓰는 것은 HT 를 노출하는
 * 칩셋/브리지 쪽 코드다.
 *
 * 실행 문맥: config space 읽기. 프로세스 문맥.
 *
 * 호출 체인:
 *   HT 관련 quirk/드라이버 → [이 함수]
 *     → __pci_bus_find_cap_start() → __pci_find_next_ht_cap()
 */
u8 pci_find_ht_capability(struct pci_dev *dev, int ht_cap)
{
	u8 pos; /* [한국어] 찾은 오프셋 */

	pos = __pci_bus_find_cap_start(dev->bus, dev->devfn, dev->hdr_type); /* [한국어] 리스트 머리 포인터 위치를 먼저 구한다 */
	if (pos) /* [한국어] capability 리스트가 없으면 건너뛴다 */
		pos = __pci_find_next_ht_cap(dev, pos, ht_cap); /* [한국어] HT 하위 타입까지 확인하며 훑는다 */

	return pos; /* [한국어] 오프셋 또는 0 */
}
EXPORT_SYMBOL_GPL(pci_find_ht_capability);

/**
 * pci_find_vsec_capability - Find a vendor-specific extended capability
 * @dev: PCI device to query
 * @vendor: Vendor ID for which capability is defined
 * @cap: Vendor-specific capability ID
 *
 * If @dev has Vendor ID @vendor, search for a VSEC capability with
 * VSEC ID @cap. If found, return the capability offset in
 * config space; otherwise return 0.
 */
/*
 * [한국어]
 * pci_find_vsec_capability - 특정 벤더의 VSEC(Vendor-Specific Extended Capability)을 찾는다.
 *
 * @dev: 대상 PCI 장치.
 * @vendor: 이 VSEC 을 정의한 벤더 ID. 장치의 Vendor ID 와 다르면 볼 필요가 없다.
 * @cap: 그 벤더가 정한 VSEC ID.
 * @return: VSEC 의 config space 오프셋, 없으면 0.
 *
 * VSEC 은 벤더가 스펙 밖의 기능을 노출하려고 쓰는 확장 capability 다. 확장
 * capability ID 는 모두 PCI_EXT_CAP_ID_VNDR(0x000b) 로 같고, 실제 구분은
 * capability 안쪽 VSEC 헤더의 ID 필드로 한다. 그래서 HT 와 마찬가지로
 * "ID 로 훑고, 안쪽 필드로 다시 거른다"는 두 겹 구조다.
 * 같은 벤더가 VSEC 을 여러 개 둘 수 있어 리스트 전체를 순회한다.
 * 벤더 ID 부터 비교하는 이유는 명확하다 — VSEC 의 의미는 벤더가 정하므로,
 * 다른 벤더의 장치에서 같은 VSEC ID 를 찾아 봤자 전혀 다른 것을 가리킨다.
 *
 * 실행 문맥: config space 를 반복해 읽는다. 프로세스 문맥.
 *
 * 호출 체인:
 *   벤더별 기능 코드(예: probe.c 의 Thunderbolt 식별) → [이 함수]
 *     → pci_find_next_ext_capability() → pci_read_config_dword()
 */
u16 pci_find_vsec_capability(struct pci_dev *dev, u16 vendor, int cap)
{
	u16 vsec = 0; /* [한국어] 순회 커서. 0 으로 시작해 리스트 처음부터 훑는다 */
	u32 header; /* [한국어] VSEC 헤더 DWORD 를 받을 곳 */
	int ret; /* [한국어] config 읽기 결과 코드 */

	if (vendor != dev->vendor) /* [한국어] 요청한 벤더의 장치가 아니면 그 벤더의 VSEC 이 있을 리 없다 */
		return 0; /* [한국어] "없음" */

	while ((vsec = pci_find_next_ext_capability(dev, vsec, /* [한국어] VNDR 확장 capability 를 하나씩 이어 찾는다. 반환값을 다시 vsec 에 담아 다음 검색의 시작점으로 삼는다 */
						     PCI_EXT_CAP_ID_VNDR))) { /* [한국어] PCI_EXT_CAP_ID_VNDR — 모든 VSEC 이 공유하는 확장 capability ID */
		ret = pci_read_config_dword(dev, vsec + PCI_VNDR_HEADER, &header); /* [한국어] VSEC 헤더(capability 시작 + PCI_VNDR_HEADER)를 읽는다. 이 DWORD 안에 벤더가 정한 VSEC ID 와 리비전, 길이가 들어 있다 */
		if (ret != PCIBIOS_SUCCESSFUL) /* [한국어] 읽기 실패 */
			continue; /* [한국어] 이 항목만 건너뛰고 다음 VSEC 을 계속 본다 — 하나 실패했다고 나머지를 포기할 이유는 없다 */

		if (PCI_VNDR_HEADER_ID(header) == cap) /* [한국어] 헤더에서 VSEC ID 필드를 뽑아 찾는 값과 비교한다 */
			return vsec; /* [한국어] 일치하면 이 VSEC 의 시작 오프셋이 답이다 */
	}

	return 0; /* [한국어] 리스트를 다 훑도록 맞는 VSEC 이 없었다 */
}
EXPORT_SYMBOL_GPL(pci_find_vsec_capability);

/**
 * pci_find_dvsec_capability - Find DVSEC for vendor
 * @dev: PCI device to query
 * @vendor: Vendor ID to match for the DVSEC
 * @dvsec: Designated Vendor-specific capability ID
 *
 * If DVSEC has Vendor ID @vendor and DVSEC ID @dvsec return the capability
 * offset in config space; otherwise return 0.
 */
/*
 * [한국어]
 * pci_find_dvsec_capability - 특정 벤더의 DVSEC 을 찾는다.
 *
 * @dev: 대상 PCI 장치.
 * @vendor: DVSEC 헤더에 적힌 벤더 ID.
 * @dvsec: 그 벤더가 정한 DVSEC ID.
 * @return: DVSEC 의 config space 오프셋, 없으면 0.
 *
 * DVSEC(Designated VSEC)은 VSEC 과 목적은 같지만 결정적 차이가 있다 —
 * 헤더 안에 벤더 ID 를 직접 적어 두기 때문에, 장치 자신의 Vendor ID 와
 * 다른 벤더가 정의한 기능도 실을 수 있다. CXL 이 대표적이다. CXL 기능은
 * PCI-SIG 벤더 ID 로 정의돼 있어 어느 회사의 장치에 붙어 있든 같은 DVSEC 으로
 * 식별된다. 이 파일의 cxl_port_dvsec() 이 바로 그렇게 쓴다.
 * 그래서 pci_find_vsec_capability() 와 달리 dev->vendor 를 먼저 보지 않고,
 * 리스트를 훑으며 각 DVSEC 헤더의 벤더 ID 를 직접 비교한다.
 *
 * 실행 문맥: config space 를 반복해 읽는다. 프로세스 문맥.
 *
 * 호출 체인:
 *   cxl_port_dvsec() 등 → [이 함수]
 *     → pci_find_ext_capability() / pci_find_next_ext_capability()
 */
u16 pci_find_dvsec_capability(struct pci_dev *dev, u16 vendor, u16 dvsec)
{
	int pos; /* [한국어] 순회 커서 */

	pos = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_DVSEC); /* [한국어] DVSEC 확장 capability(ID 0x0023)를 리스트 처음부터 찾는다 */
	if (!pos) /* [한국어] 하나도 없으면 */
		return 0; /* [한국어] "없음" */

	while (pos) { /* [한국어] DVSEC 은 여러 개 있을 수 있으므로 벤더/ID 가 맞을 때까지 훑는다 */
		u16 v, id; /* [한국어] v 는 헤더에 적힌 벤더 ID, id 는 DVSEC ID */

		pci_read_config_word(dev, pos + PCI_DVSEC_HEADER1, &v); /* [한국어] DVSEC Header1 에서 벤더 ID 를 읽는다 */
		pci_read_config_word(dev, pos + PCI_DVSEC_HEADER2, &id); /* [한국어] DVSEC Header2 에서 DVSEC ID 를 읽는다 */
		if (vendor == v && dvsec == id) /* [한국어] 둘 다 맞아야 우리가 찾는 DVSEC 이다 */
			return pos; /* [한국어] 그 시작 오프셋이 답이다 */

		pos = pci_find_next_ext_capability(dev, pos, PCI_EXT_CAP_ID_DVSEC); /* [한국어] 아니면 다음 DVSEC 으로 넘어간다 */
	}

	return 0; /* [한국어] 끝까지 훑도록 맞는 것이 없었다 */
}
EXPORT_SYMBOL_GPL(pci_find_dvsec_capability);

/**
 * pci_find_parent_resource - return resource region of parent bus of given
 *			      region
 * @dev: PCI device structure contains resources to be searched
 * @res: child resource record for which parent is sought
 *
 * For given resource region of given device, return the resource region of
 * parent bus the given region is contained in.
 */
/*
 * [한국어]
 * pci_find_parent_resource - 이 BAR 를 품고 있는 상위 버스의 주소 창을 찾는다.
 *
 * @dev: 자원을 가진 PCI 장치.
 * @res: 부모 창을 찾고 싶은 자식 자원(보통 이 장치의 BAR 하나).
 * @return: @res 를 포함하는 부모 버스의 resource, 없으면 NULL.
 *
 * PCI 주소 공간은 계층적이다. 브리지는 자기 아래로 내려보낼 주소 구간(윈도)을
 * 갖고, 그 아래 장치의 BAR 는 반드시 그 윈도 안에 들어와야 한다. 그렇지 않으면
 * 브리지가 그 주소의 트랜잭션을 아래로 흘려보내지 않아 장치에 닿지 못한다.
 * 자원을 새로 배정하거나 옮길 때 "어느 윈도 안에서 자리를 잡아야 하는가"를
 * 알아야 해서 이 조회가 필요하다.
 *
 * 두 가지 미묘한 점이 있다.
 *   - prefetchable 윈도 안에 non-prefetchable BAR 가 놓이면 안 된다.
 *     prefetchable 영역은 브리지가 미리 읽어 와도 되는 곳인데, 읽기에 부작용이
 *     있는 레지스터를 그런 창에 두면 오동작한다. 이 경우는 배정기의 버그라
 *     NULL 을 돌려 잘못을 드러낸다.
 *   - transparent 브리지(subtractive decode) 아래에서는 한 주소가 두 창에
 *     모두 걸릴 수 있다. positively-decoded 쪽이 정답이고, 원본 주석대로
 *     pci_bus_for_each_resource() 가 그 순서를 보장해 주므로 첫 일치를 쓴다.
 *
 * 실행 문맥: 부모 버스의 resource 배열을 훑을 뿐 config 접근이 없다.
 *
 * 호출 체인:
 *   setup-res.c 의 자원 배정 코드, pci_reassigndev_resource_alignment()
 *     → [이 함수]
 */
struct resource *pci_find_parent_resource(const struct pci_dev *dev,
					  struct resource *res)
{
	const struct pci_bus *bus = dev->bus; /* [한국어] 이 장치가 붙은 버스 — 그 버스의 창들이 후보다 */
	struct resource *r; /* [한국어] 후보 창을 훑는 커서 */

	pci_bus_for_each_resource(bus, r) { /* [한국어] 버스에 등록된 주소 창을 순서대로 본다. 이 매크로는 positively-decoded 창을 먼저 준다 */
		if (!r) /* [한국어] 빈 슬롯은 건너뛴다 — 창 배열에는 쓰이지 않는 칸이 있을 수 있다 */
			continue; /* [한국어] 다음 창으로 */
		if (resource_contains(r, res)) { /* [한국어] 이 창이 자식 자원을 완전히 포함하는가 */

			/*
			 * If the window is prefetchable but the BAR is
			 * not, the allocator made a mistake.
			 */
			if (r->flags & IORESOURCE_PREFETCH && /* [한국어] 창은 prefetchable 인데 */
			    !(res->flags & IORESOURCE_PREFETCH)) /* [한국어] BAR 는 아니라면 — 읽기 부작용이 있는 영역을 미리 읽어도 되는 창에 넣은 셈이다 */
				return NULL; /* [한국어] 배정기의 실수이므로 부모를 찾지 못한 것으로 처리한다 */

			/*
			 * If we're below a transparent bridge, there may
			 * be both a positively-decoded aperture and a
			 * subtractively-decoded region that contain the BAR.
			 * We want the positively-decoded one, so this depends
			 * on pci_bus_for_each_resource() giving us those
			 * first.
			 */
			return r; /* [한국어] 첫 일치를 그대로 쓴다. 순서 보장 덕분에 이것이 positively-decoded 창이다 */
		}
	}
	return NULL; /* [한국어] 어느 창에도 들어가지 않는다 — 이 자원은 부모 아래로 라우팅되지 않는다 */
}
EXPORT_SYMBOL(pci_find_parent_resource);

/**
 * pci_find_resource - Return matching PCI device resource
 * @dev: PCI device to query
 * @res: Resource to look for
 *
 * Goes over standard PCI resources (BARs) and checks if the given resource
 * is partially or fully contained in any of them. In that case the
 * matching resource is returned, %NULL otherwise.
 */
/*
 * [한국어]
 * pci_find_resource - 주어진 주소 구간이 이 장치의 어느 BAR 에 속하는지 찾는다.
 *
 * @dev: 대상 PCI 장치.
 * @res: 찾고자 하는 주소 구간.
 * @return: 그 구간을 포함하는 BAR 의 resource, 없으면 NULL.
 *
 * 방향이 pci_find_parent_resource() 와 반대다. 저쪽은 "이 BAR 의 부모 창"을
 * 찾고, 이쪽은 "이 주소가 내 어느 BAR 인지"를 찾는다.
 * 표준 BAR 6개(PCI_STD_NUM_BARS)만 본다 — ROM 이나 VF BAR, 브리지 윈도는
 * 같은 resource 배열의 뒤쪽에 있지만 여기서는 대상이 아니다.
 * r->start 가 0 인 항목을 건너뛰는 것은 그 BAR 가 구현되지 않았거나 주소가
 * 배정되지 않았다는 뜻이기 때문이다.
 *
 * 실행 문맥: 배열 순회뿐. config 접근이 없어 어느 문맥에서도 안전하다.
 *
 * 호출 체인:
 *   주소로부터 BAR 를 역추적해야 하는 코드 → [이 함수]
 */
struct resource *pci_find_resource(struct pci_dev *dev, struct resource *res)
{
	int i; /* [한국어] BAR 인덱스 */

	for (i = 0; i < PCI_STD_NUM_BARS; i++) { /* [한국어] PCI_STD_NUM_BARS 는 6 — 헤더 타입 0 의 표준 BAR 개수(config space 0x10~0x24) */
		struct resource *r = &dev->resource[i]; /* [한국어] i 번 BAR 의 resource */

		if (r->start && resource_contains(r, res)) /* [한국어] 주소가 배정된 BAR 중에서 이 구간을 포함하는 것을 찾는다 */
			return r; /* [한국어] 그 BAR 가 답이다 */
	}

	return NULL; /* [한국어] 어느 BAR 에도 속하지 않는다 */
}
EXPORT_SYMBOL(pci_find_resource);

/**
 * pci_resource_name - Return the name of the PCI resource
 * @dev: PCI device to query
 * @i: index of the resource
 *
 * Return the standard PCI resource (BAR) name according to their index.
 */
/*
 * [한국어]
 * pci_resource_name - resource 배열의 인덱스를 사람이 읽는 이름으로 바꾼다.
 *
 * @dev: 대상 PCI 장치(헤더 타입에 따라 이름표가 달라진다).
 * @i: dev->resource[] 의 인덱스.
 * @return: "BAR 0", "ROM", "bridge window" 같은 정적 문자열. 범위를 벗어나면 "unknown".
 *
 * struct pci_dev 의 resource 배열에는 성격이 다른 것들이 한 줄로 섞여 있다 —
 * 표준 BAR 6개, Expansion ROM, (CONFIG_PCI_IOV 일 때) VF BAR 6개, 그리고
 * 브리지 윈도 3개. 인덱스만으로는 무엇인지 알 수 없어 로그와 sysfs 출력에
 * 쓸 이름표가 필요하다.
 * 배열의 순서가 곧 resource 배열의 배치와 일치해야 하므로, CONFIG_PCI_IOV 가
 * 꺼지면 VF BAR 칸이 통째로 빠져 뒤쪽 인덱스가 당겨진다. #ifdef 를 이름표
 * 배열 안에 둔 이유가 이것이다.
 * CardBus 브리지(헤더 타입 2)는 배치가 아예 달라 별도 표를 쓴다.
 *
 * 실행 문맥: 정적 표 조회뿐이라 어느 문맥에서도 안전하다. 반환 문자열은
 * static const 이므로 호출자가 해제하면 안 된다.
 *
 * 호출 체인:
 *   pci_request_region()/pci_release_region() 의 로그, pci-sysfs.c → [이 함수]
 */
const char *pci_resource_name(struct pci_dev *dev, unsigned int i)
{
	/* [한국어] 헤더 타입 0/1 용 이름표. 인덱스가 dev->resource[] 의 인덱스와 일대일로 대응해야 한다. */
	static const char * const bar_name[] = {
		"BAR 0", /* [한국어] 인덱스 0 — config space 0x10 의 BAR0. NVMe 는 여기에 컨트롤러 레지스터와 도어벨이 매핑된다 */
		"BAR 1", /* [한국어] 인덱스 1 — config space 0x14. BAR0 가 64비트면 이 칸은 BAR0 의 상위 32비트로 소모된다 */
		"BAR 2", /* [한국어] 인덱스 2 — config space 0x18 */
		"BAR 3", /* [한국어] 인덱스 3 — config space 0x1c */
		"BAR 4", /* [한국어] 인덱스 4 — config space 0x20. NVMe 의 CMB/PMR 가 여기 놓이는 경우가 있다 */
		"BAR 5", /* [한국어] 인덱스 5 — config space 0x24 */
		"ROM", /* [한국어] 인덱스 6 — Expansion ROM Base Address(config space 0x30). 옵션 ROM 이 있는 장치만 쓴다 */
#ifdef CONFIG_PCI_IOV /* [한국어] SR-IOV 를 빌드했을 때만 VF BAR 칸이 resource 배열에 존재한다 */
		"VF BAR 0", /* [한국어] VF BAR 0 — SR-IOV capability 안에 따로 있는, 가상 함수용 BAR */
		"VF BAR 1", /* [한국어] VF BAR 1 */
		"VF BAR 2", /* [한국어] VF BAR 2 */
		"VF BAR 3", /* [한국어] VF BAR 3 */
		"VF BAR 4", /* [한국어] VF BAR 4 */
		"VF BAR 5", /* [한국어] VF BAR 5 */
#endif
		"bridge window",	/* "io" included in %pR */
		"bridge window",	/* "mem" included in %pR */
		"bridge window",	/* "mem pref" included in %pR */
	};
	/* [한국어] CardBus 브리지(헤더 타입 2)용 이름표. config space 배치가 달라 표를 따로 둔다. */
	static const char * const cardbus_name[] = {
		"BAR 1", /* [한국어] CardBus 는 BAR 를 하나만 쓴다 */
		"unknown", /* [한국어] 나머지 칸은 CardBus 에서 의미가 없다 */
		"unknown", /* [한국어] 미사용 칸 */
		"unknown", /* [한국어] 미사용 칸 */
		"unknown", /* [한국어] 미사용 칸 */
		"unknown", /* [한국어] 미사용 칸 */
#ifdef CONFIG_PCI_IOV /* [한국어] 인덱스를 위 표와 맞추려면 여기서도 VF BAR 칸 수를 똑같이 채워야 한다 */
		"unknown", /* [한국어] 인덱스 정렬용 자리 */
		"unknown", /* [한국어] 인덱스 정렬용 자리 */
		"unknown", /* [한국어] 인덱스 정렬용 자리 */
		"unknown", /* [한국어] 인덱스 정렬용 자리 */
		"unknown", /* [한국어] 인덱스 정렬용 자리 */
		"unknown", /* [한국어] 인덱스 정렬용 자리 */
#endif
		"CardBus bridge window 0",	/* I/O */
		"CardBus bridge window 1",	/* I/O */
		"CardBus bridge window 0",	/* mem */
		"CardBus bridge window 1",	/* mem */
	};

	if (dev->hdr_type == PCI_HEADER_TYPE_CARDBUS && /* [한국어] CardBus 브리지이고 */
	    i < ARRAY_SIZE(cardbus_name)) /* [한국어] 인덱스가 CardBus 표 안이면 */
		return cardbus_name[i]; /* [한국어] CardBus 이름표를 쓴다 */

	if (i < ARRAY_SIZE(bar_name)) /* [한국어] 그 밖에는 일반 표를 본다 */
		return bar_name[i]; /* [한국어] 해당 이름표 */

	return "unknown"; /* [한국어] 표 범위를 벗어난 인덱스 — 이름을 알 수 없다 */
}

/**
 * pci_wait_for_pending - wait for @mask bit(s) to clear in status word @pos
 * @dev: the PCI device to operate on
 * @pos: config space offset of status word
 * @mask: mask of bit(s) to care about in status word
 *
 * Return 1 when mask bit(s) in status word clear, 0 otherwise.
 */
/*
 * [한국어]
 * pci_wait_for_pending - config space 상태 워드의 특정 비트가 내려갈 때까지 기다린다.
 *
 * @dev: 대상 PCI 장치.
 * @pos: 상태 워드가 놓인 config space 오프셋.
 * @mask: 지켜볼 비트 마스크.
 * @return: 비트가 내려갔으면 1, 끝까지 안 내려갔으면 0.
 *
 * 리셋을 걸기 전에는 장치가 이미 내보낸 트랜잭션이 모두 끝났는지 확인해야
 * 한다. 아직 완료를 기다리는 트랜잭션이 남은 채로 리셋하면, 뒤늦게 돌아온
 * 완료 패킷이 갈 곳을 잃어 시스템 오류가 된다. 그 확인이 PCIe 의
 * Device Status 레지스터에 있는 Transaction Pending 비트다.
 *
 * 대기 방식은 지수적 백오프다. 첫 번째는 곧바로 확인하고, 이후 100ms, 200ms,
 * 400ms 를 자며 총 4번 본다. 최악의 경우 약 700ms 를 쓴다.
 * msleep 을 쓰므로 반드시 프로세스 문맥이어야 한다.
 *
 * 참고 — NVMe 컨트롤러의 큐 소진(SQ/CQ 비우기)과는 무관하다. 여기서 보는
 * 것은 PCIe 링크 위의 미완료 트랜잭션이지 NVMe 명령 큐가 아니다.
 *
 * 호출 체인:
 *   pci_wait_for_pending_transaction() / pci_af_flr() → [이 함수]
 *     → pci_read_config_word(), msleep()
 */
int pci_wait_for_pending(struct pci_dev *dev, int pos, u16 mask)
{
	int i; /* [한국어] 시도 횟수 */

	/* Wait for Transaction Pending bit clean */
	for (i = 0; i < 4; i++) { /* [한국어] 최대 4번 확인한다 */
		u16 status; /* [한국어] 매 시도마다 새로 읽을 상태 워드 */
		if (i) /* [한국어] 첫 시도(i==0)는 자지 않고 바로 확인한다 — 이미 끝나 있으면 지연을 줄 이유가 없다 */
			msleep((1 << (i - 1)) * 100); /* [한국어] 100ms → 200ms → 400ms 로 두 배씩 늘려 잔다. 합계 700ms 가 상한이다 */

		pci_read_config_word(dev, pos, &status); /* [한국어] 상태 워드를 다시 읽는다 */
		if (!(status & mask)) /* [한국어] 관심 비트가 모두 내려갔으면 */
			return 1; /* [한국어] 성공(1) */
	}

	return 0; /* [한국어] 700ms 를 기다려도 안 내려갔다 — 호출자는 이를 알고도 리셋을 강행할지 결정한다 */
}

static int pci_acs_enable; /* [한국어] pci_request_acs() 가 세우는 전역 플래그. 1 이면 ACS 를 지원하는 장치마다 켠다 */

/**
 * pci_request_acs - ask for ACS to be enabled if supported
 */
/*
 * [한국어]
 * pci_request_acs - 이후 열거되는 장치에서 ACS 를 켜 달라고 표시해 둔다.
 *
 * @return: 없음.
 *
 * ACS(Access Control Services)는 스위치나 멀티펑션 장치가 "아래 장치끼리
 * 자기들끼리만 주고받는 트랜잭션"을 그대로 통과시키지 않고 위로 올려보내도록
 * 강제하는 기능이다. 그래야 IOMMU 가 모든 DMA 를 볼 수 있어 장치 간 격리가
 * 성립한다. ACS 없이 P2P 트래픽이 스위치 안에서 되돌아가면 IOMMU 를 우회한다.
 *
 * IOMMU 드라이버가 초기화될 때 이 함수를 불러 의사만 남기고, 실제로 켜는 일은
 * 각 장치가 열거될 때 pci_enable_acs() 가 한다. 순서가 이렇게 나뉜 이유는
 * IOMMU 초기화가 PCI 열거보다 먼저이거나 뒤일 수 있기 때문이다.
 *
 * 실행 문맥: 전역 int 하나를 1 로 놓을 뿐이다. 부팅 초기에 불려 경쟁이 없다.
 *
 * 호출 체인:
 *   IOMMU 드라이버 초기화 → [이 함수]  (나중에 pci_enable_acs() 가 이 값을 읽는다)
 */
void pci_request_acs(void)
{
	pci_acs_enable = 1; /* [한국어] 의사 표시만 남긴다. 실제 레지스터 조작은 열거 시점의 pci_enable_acs() 몫이다 */
}

static const char *disable_acs_redir_param; /* [한국어] pci=disable_acs_redir= 로 받은 문자열. 지정된 장치들에서 ACS 재지정 제어 비트를 끈다 */
static const char *config_acs_param; /* [한국어] pci=config_acs= 로 받은 문자열. 장치별로 ACS 제어 비트를 임의 조합으로 지정한다 */

/*
 * [한국어] 한 장치의 ACS 설정을 계산하는 동안 들고 다니는 작업 상태.
 * pci_acs_init() 이 스택에 잡아 pci_std_enable_acs() 와 __pci_config_acs() 에
 * 넘기고, 최종값을 config space 의 ACS Control 레지스터에 한 번에 쓴다.
 */
struct pci_acs {
	u16 ctrl; /* [한국어] 최종적으로 ACS Control 레지스터에 쓸 값. 설정자: pci_std_enable_acs()/__pci_config_acs(). 읽는 자: pci_acs_init(). 값 범위: PCI_ACS_SV/TB/RR/CR/UF/EC/DT 비트 조합 */
	u16 fw_ctrl; /* [한국어] 펌웨어가 이미 켜 두었던 값. 커널 파라미터로 비트를 지울 때도 펌웨어가 켠 것은 존중하려고 따로 보관한다 */
};

/*
 * [한국어]
 * __pci_config_acs - 커널 커맨드라인이 지정한 ACS 설정을 이 장치에 반영한다.
 *
 * @dev: 대상 PCI 장치.
 * @caps: 계산 중인 ACS 설정 상태(ctrl 을 여기서 고쳐 쓴다).
 * @p: 파라미터 문자열. 두 형태로 불린다.
 *     - disable_acs_redir 용: "<장치>[;<장치>]*"  (마스크·플래그는 인자로 고정)
 *     - config_acs 용:       "<비트열>@<장치>[;...]"  (비트열도 문자열에서 읽는다)
 * @acs_mask: 건드릴 비트의 마스크. 0 이면 문자열에서 직접 파싱하라는 신호다.
 * @acs_flags: 그 비트에 넣을 값.
 *
 * @return: 없음. 결과는 caps->ctrl 에 남는다.
 *
 * 특정 장치의 ACS 를 관리자가 손으로 조정할 수 있게 하는 경로다. 보통은
 * IOMMU 격리를 위해 ACS 를 켜는 것이 맞지만, GPU 나 NVMe 사이의 P2P DMA 를
 * 쓰려면 오히려 재지정(redirect) 비트를 꺼야 성능이 나온다. 그런 판단은
 * 커널이 대신할 수 없어 관리자에게 맡긴 것이다.
 *
 * config_acs 형식의 비트열은 "@" 앞에 오고, 오른쪽 끝이 비트 0 이다.
 * 각 자리는 '1'(켬), '0'(끔), 'x'/'X'(건드리지 않음) 셋 중 하나다.
 * 마스크는 1 과 0 자리에만 세워지고, x 자리는 마스크에서 빠져 펌웨어 값이
 * 그대로 유지된다.
 *
 * 실행 문맥: 열거 중 pci_acs_init() 에서 불린다. 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_enable_acs() → [이 함수]
 *     → pci_dev_str_match(), pci_dev_specific_disable_acs_redir()
 */
static void __pci_config_acs(struct pci_dev *dev, struct pci_acs *caps,
			     const char *p, const u16 acs_mask, const u16 acs_flags)
{
	u16 flags = acs_flags; /* [한국어] 인자로 받은 기본 플래그. config_acs 경로에서는 문자열에서 다시 계산된다 */
	u16 mask = acs_mask; /* [한국어] 인자로 받은 기본 마스크. 마찬가지로 덮어써질 수 있다 */
	char *delimit; /* [한국어] 비트열과 장치 지정자를 가르는 "@" 의 위치 */
	int ret = 0; /* [한국어] pci_dev_str_match() 의 결과. 루프를 빠져나온 뒤 "일치했는가"를 판단하는 데 쓰므로 0 으로 초기화해 둔다 */

	if (!p) /* [한국어] 해당 커맨드라인 파라미터가 아예 주어지지 않았으면 */
		return; /* [한국어] 할 일이 없다 */

	while (*p) { /* [한국어] ';' 로 이어진 장치 지정자를 하나씩 훑는다 */
		if (!acs_mask) { /* [한국어] 마스크가 인자로 고정되지 않았다면 config_acs 형식이므로 비트열부터 읽어야 한다 */
			/* Check for ACS flags */
			delimit = strstr(p, "@"); /* [한국어] 비트열과 장치 지정자의 경계 */
			if (delimit) { /* [한국어] "@" 가 있어야 형식이 맞다 */
				int end; /* [한국어] 비트열의 마지막 문자 인덱스 */
				u32 shift = 0; /* [한국어] 지금 해석 중인 비트 위치. 오른쪽 끝이 비트 0 이다 */

				end = delimit - p - 1; /* [한국어] "@" 바로 앞이 비트열의 끝이다 */
				mask = 0; /* [한국어] 문자열에서 새로 계산하므로 0 에서 시작한다 */
				flags = 0; /* [한국어] 플래그도 마찬가지 */

				while (end > -1) { /* [한국어] 오른쪽부터 왼쪽으로 한 글자씩 해석한다 */
					if (*(p + end) == '0') { /* [한국어] '0' — 이 비트를 명시적으로 끈다 */
						mask |= 1 << shift; /* [한국어] 마스크에는 세우고(건드린다) 플래그에는 세우지 않는다(0 을 넣는다) */
						shift++; /* [한국어] 다음 비트 자리로 */
						end--; /* [한국어] 왼쪽 글자로 */
					} else if (*(p + end) == '1') { /* [한국어] '1' — 이 비트를 켠다 */
						mask |= 1 << shift; /* [한국어] 건드릴 비트로 표시하고 */
						flags |= 1 << shift; /* [한국어] 값 1 을 넣는다 */
						shift++; /* [한국어] 다음 비트 자리로 */
						end--; /* [한국어] 왼쪽 글자로 */
					} else if ((*(p + end) == 'x') || (*(p + end) == 'X')) { /* [한국어] 'x'/'X' — 이 비트는 건드리지 않는다 */
						shift++; /* [한국어] 마스크를 세우지 않으므로 아래 합성식에서 펌웨어 값이 그대로 남는다 */
						end--; /* [한국어] 왼쪽 글자로 */
					} else {
						pci_err(dev, "Invalid ACS flags... Ignoring\n"); /* [한국어] 형식 오류를 알린다 */
						return; /* [한국어] 잘못된 파라미터로 ACS 를 건드리느니 아무 것도 하지 않는다 */
					}
				}
				p = delimit + 1; /* [한국어] 비트열을 다 읽었으니 "@" 다음의 장치 지정자로 커서를 옮긴다 */
			} else {
				pci_err(dev, "ACS Flags missing\n"); /* [한국어] 형식 오류를 알리고 */
				return; /* [한국어] 중단한다 */
			}
		}

		if (mask & ~(PCI_ACS_SV | PCI_ACS_TB | PCI_ACS_RR | PCI_ACS_CR | /* [한국어] ACS Control 에 실제로 존재하는 비트(Source Validation, Translation Blocking, P2P Request/Completion Redirect, Upstream Forwarding, Egress Control, Direct Translated P2P) 이외의 자리를 지정했는지 본다 */
			    PCI_ACS_UF | PCI_ACS_EC | PCI_ACS_DT)) { /* [한국어] 이 일곱 비트가 ACS Control 레지스터에 정의된 전부다 */
			pci_err(dev, "Invalid ACS flags specified\n"); /* [한국어] 정의되지 않은 비트를 건드리라는 요구는 거절한다 */
			return; /* [한국어] 아무 것도 바꾸지 않고 나간다 */
		}

		ret = pci_dev_str_match(dev, p, &p); /* [한국어] 남은 문자열이 이 장치를 가리키는지 판정한다. 소비한 위치를 p 로 돌려받아 다음 항목으로 이어 간다 */
		if (ret < 0) { /* [한국어] 파싱 실패 */
			pr_warn_once("PCI: Can't parse ACS command line parameter\n"); /* [한국어] 같은 경고가 부팅 로그를 덮지 않도록 한 번만 찍는다 */
			break; /* [한국어] 더 볼 것이 없다 */
		} else if (ret == 1) { /* [한국어] 이 장치와 일치했다 */
			/* Found a match */
			break; /* [한국어] 루프를 끝내고 아래에서 설정을 적용한다 */
		}

		if (*p != ';' && *p != ',') { /* [한국어] 항목 구분자가 아니면 문자열의 끝이거나 형식이 깨진 것이다 */
			/* End of param or invalid format */
			break; /* [한국어] 더 훑지 않는다 */
		}
		p++; /* [한국어] 구분자를 건너뛰고 다음 장치 지정자로 */
	}

	if (ret != 1) /* [한국어] 끝까지 훑도록 이 장치와 일치한 항목이 없었다면 */
		return; /* [한국어] 이 장치에는 적용할 설정이 없다 */

	if (!pci_dev_specific_disable_acs_redir(dev)) /* [한국어] 장치별 quirk 가 "이 하드웨어에서는 ACS 재지정을 끄면 안 된다"고 거부할 수 있다 */
		return; /* [한국어] 거부되면 커맨드라인보다 quirk 를 우선한다 */

	pci_dbg(dev, "ACS mask  = %#06x\n", mask); /* [한국어] 어떤 비트를 건드리는지 */
	pci_dbg(dev, "ACS flags = %#06x\n", flags); /* [한국어] 그 비트에 무엇을 넣는지 */
	pci_dbg(dev, "ACS control = %#06x\n", caps->ctrl); /* [한국어] 지금까지 계산된 제어값 */
	pci_dbg(dev, "ACS fw_ctrl = %#06x\n", caps->fw_ctrl); /* [한국어] 펌웨어가 켜 두었던 원래 값 */

	/*
	 * For mask bits that are 0, copy them from the firmware setting
	 * and apply flags for all the mask bits that are 1.
	 */
	caps->ctrl = (caps->fw_ctrl & ~mask) | (flags & mask); /* [한국어] 마스크가 0 인 자리는 펌웨어 값을 그대로 두고, 1 인 자리만 flags 로 덮는다. 앞서 pci_std_enable_acs() 가 채운 caps->ctrl 이 아니라 fw_ctrl 을 바탕으로 삼는 점이 중요하다 — 관리자의 지정이 커널 기본값보다 우선한다 */

	pci_info(dev, "Configured ACS to %#06x\n", caps->ctrl); /* [한국어] 최종 결과를 로그에 남긴다. ACS 는 격리 정책이라 무엇이 적용됐는지 흔적을 남길 가치가 있다 */
}

/**
 * pci_std_enable_acs - enable ACS on devices using standard ACS capabilities
 * @dev: the PCI device
 * @caps: default ACS controls
 */
/*
 * [한국어]
 * pci_std_enable_acs - 표준 ACS 기능 중 격리에 필요한 비트를 켠다.
 *
 * @dev: 대상 PCI 장치.
 * @caps: 계산 중인 ACS 설정. ctrl 에 비트를 얹는다.
 * @return: 없음.
 *
 * IOMMU 가 모든 DMA 를 볼 수 있게 하려면 스위치/멀티펑션 장치가 트래픽을
 * 자기 안에서 되돌리지 못하게 막아야 한다. 켜는 비트마다 이유가 다르다.
 *   - SV(Source Validation): 아래 장치가 자기 것이 아닌 Requester ID 를
 *     사칭하지 못하게 검사한다.
 *   - RR(P2P Request Redirect) / CR(P2P Completion Redirect): 아래 장치끼리
 *     주고받는 요청·완료를 스위치 안에서 처리하지 않고 위로 올려보낸다.
 *     이것이 켜져야 IOMMU 가 그 트래픽을 본다.
 *   - UF(Upstream Forwarding): 위로 올려보내야 할 트래픽을 실제로 올린다.
 *   - TB(Translation Blocking): 이미 변환된 주소로 온 요청을 막는다.
 * dev->acs_capabilities 와 AND 하는 이유는, 하드웨어가 지원하지 않는 비트를
 * 켜 봐야 무시되거나 오동작하기 때문이다. 지원하는 것만 켠다.
 *
 * TB 만 조건부다. ATS 를 쓰는 장치는 변환된 주소로 DMA 하는 것이 정상 동작인데
 * TB 를 켜면 그 정상 경로가 막힌다. 그래서 ATS 가 꺼져 있거나(pci=noats),
 * 외부에 노출된 포트(Thunderbolt 등)이거나, 신뢰할 수 없다고 표시된 장치일
 * 때만 켠다 — 그런 경우에는 변환 우회 자체를 위험으로 본다.
 *
 * 실행 문맥: 열거 중 pci_enable_acs() 에서 불린다. 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_enable_acs() → [이 함수] → pci_ats_disabled()
 */
static void pci_std_enable_acs(struct pci_dev *dev, struct pci_acs *caps)
{
	/* Source Validation */
	caps->ctrl |= (dev->acs_capabilities & PCI_ACS_SV); /* [한국어] 하드웨어가 SV 를 지원할 때만 켠다 */

	/* P2P Request Redirect */
	caps->ctrl |= (dev->acs_capabilities & PCI_ACS_RR); /* [한국어] P2P 요청을 위로 올려보내게 한다 */

	/* P2P Completion Redirect */
	caps->ctrl |= (dev->acs_capabilities & PCI_ACS_CR); /* [한국어] P2P 완료도 위로 올려보내게 한다 */

	/* Upstream Forwarding */
	caps->ctrl |= (dev->acs_capabilities & PCI_ACS_UF); /* [한국어] 올려보낼 트래픽을 실제로 상위로 전달하게 한다 */

	/* Enable Translation Blocking for external devices and noats */
	if (pci_ats_disabled() || dev->external_facing || dev->untrusted) /* [한국어] ATS 를 아예 못 쓰게 했거나, 외부 노출 포트이거나, 신뢰할 수 없는 장치일 때만 */
		caps->ctrl |= (dev->acs_capabilities & PCI_ACS_TB); /* [한국어] 변환된 주소로 오는 요청을 차단한다. ATS 를 정상적으로 쓰는 장치에 켜면 그 DMA 가 막힌다 */
}

/**
 * pci_enable_acs - enable ACS if hardware support it
 * @dev: the PCI device
 */
/*
 * [한국어]
 * pci_enable_acs - 이 장치의 ACS Control 레지스터를 최종 계산해 써 넣는다.
 *
 * @dev: 대상 PCI 장치.
 * @return: 없음.
 *
 * ACS 설정은 세 곳에서 온다. 이 함수가 그 셋을 순서대로 겹쳐 쌓는다.
 *   1) 펌웨어가 이미 써 둔 값 — 현재 레지스터를 읽어 바탕으로 삼는다.
 *   2) 커널 기본 정책 — IOMMU 가 있고(pci_acs_enable) 장치별 quirk 가
 *      허락하면 pci_std_enable_acs() 가 격리 비트를 얹는다.
 *   3) 커맨드라인 지정 — disable_acs_redir 와 config_acs 를 차례로 적용한다.
 *      원본 주석대로 이것은 IOMMU 유무와 무관하게 언제나 적용된다.
 *      관리자가 바꾸려는 데는 이유가 있다고 보는 것이다.
 * 계산이 끝난 뒤에야 config space 에 한 번 쓴다 — 중간 상태를 하드웨어에
 * 노출하지 않기 위해서다.
 *
 * dev->acs_cap 은 열거 때 pci_acs_init() 이 캐시해 둔 ACS capability 오프셋이다.
 * 0 이면 이 장치에는 ACS 자체가 없어 할 일이 없다.
 *
 * 참고 — drivers/nvme/ 에는 이 함수 호출이 없다. ACS 는 PCI 코어가 열거 중에
 * 처리하며, NVMe 드라이버가 개입하지 않는다.
 *
 * 실행 문맥: 열거 중 프로세스 문맥. config space 를 읽고 쓴다.
 *
 * 호출 체인:
 *   pci_acs_init()(probe.c 경로) → [이 함수]
 *     → pci_std_enable_acs(), __pci_config_acs(), pci_write_config_word()
 */
void pci_enable_acs(struct pci_dev *dev)
{
	struct pci_acs caps; /* [한국어] 계산 중간값을 담을 작업 구조체. 스택에 두므로 다른 CPU 와 공유되지 않는다 */
	bool enable_acs = false; /* [한국어] 커널 기본 정책을 얹을지 여부 */
	int pos; /* [한국어] ACS capability 오프셋 */

	/* If an iommu is present we start with kernel default caps */
	if (pci_acs_enable) { /* [한국어] IOMMU 드라이버가 pci_request_acs() 로 의사를 남겼다면 */
		if (pci_dev_specific_enable_acs(dev)) /* [한국어] 장치별 quirk 에 물어본다. 일부 하드웨어는 표준 ACS 대신 자체 방식을 쓰거나 표준 비트가 오동작한다 */
			enable_acs = true; /* [한국어] quirk 가 허락하면 기본 정책을 적용한다 */
	}

	pos = dev->acs_cap; /* [한국어] 열거 때 캐시해 둔 ACS capability 오프셋 */
	if (!pos) /* [한국어] ACS capability 가 없는 장치 */
		return; /* [한국어] 할 일이 없다 */

	pci_read_config_word(dev, pos + PCI_ACS_CTRL, &caps.ctrl); /* [한국어] 현재 ACS Control 값을 읽는다 — 펌웨어가 설정해 둔 상태다 */
	caps.fw_ctrl = caps.ctrl; /* [한국어] 그 원본을 따로 보관한다. 아래 __pci_config_acs() 가 "건드리지 않는 비트"의 출처로 쓴다 */

	if (enable_acs) /* [한국어] 커널 기본 정책을 얹을 차례면 */
		pci_std_enable_acs(dev, &caps); /* [한국어] 격리에 필요한 비트를 caps.ctrl 에 켠다 */

	/*
	 * Always apply caps from the command line, even if there is no iommu.
	 * Trust that the admin has a reason to change the ACS settings.
	 */
	__pci_config_acs(dev, &caps, disable_acs_redir_param, /* [한국어] disable_acs_redir 파라미터 적용 — 지정된 장치에서 재지정 비트를 끈다 */
			 PCI_ACS_RR | PCI_ACS_CR | PCI_ACS_EC, /* [한국어] 건드릴 비트: P2P Request Redirect, P2P Completion Redirect, Egress Control */
			 ~(PCI_ACS_RR | PCI_ACS_CR | PCI_ACS_EC)); /* [한국어] 넣을 값: 그 세 비트를 모두 0 으로 (~ 로 뒤집어 해당 자리를 0 으로 만든다). P2P DMA 를 쓰려는 관리자가 격리를 일부러 푸는 경로다 */
	__pci_config_acs(dev, &caps, config_acs_param, 0, 0); /* [한국어] config_acs 파라미터 적용 — 마스크/플래그에 0 을 넘겨 "비트열도 문자열에서 파싱하라"고 알린다 */

	pci_write_config_word(dev, pos + PCI_ACS_CTRL, caps.ctrl); /* [한국어] 계산이 끝난 값을 ACS Control 레지스터에 한 번에 쓴다 */
}

/**
 * pci_restore_bars - restore a device's BAR values (e.g. after wake-up)
 * @dev: PCI device to have its BARs restored
 *
 * Restore the BAR values for a given device, so as to make it
 * accessible by its driver.
 */
/*
 * [한국어]
 * pci_restore_bars - 소프트웨어가 기억하는 자원 주소를 BAR 에 다시 써 넣는다.
 *
 * @dev: 대상 PCI 장치.
 * @return: 없음.
 *
 * D3cold 를 거치거나 전원이 끊기면 config space 가 초기값으로 돌아가 BAR 가
 * 모두 0 이 된다. 그 상태에서는 CPU 가 장치 레지스터에 닿을 주소가 없어
 * 드라이버가 아무 것도 할 수 없다.
 * 이 함수는 pci_dev->resource[] 에 남아 있는 "이 장치는 여기에 있어야 한다"는
 * 정보를 근거로 BAR 를 다시 쓴다. 저장해 둔 config space 사본을 되돌리는
 * pci_restore_state() 와는 출처가 다르다 — 이쪽은 자원 배정 결과가 근거다.
 *
 * PCI_BRIDGE_RESOURCES 까지만 도는 것이 요점이다. 그 인덱스부터는 브리지
 * 윈도라 BAR 가 아니고, 되돌리는 방식도 다르다. 따라서 표준 BAR 와 ROM 까지만
 * 갱신한다.
 *
 * 실행 문맥: 전원 복귀 경로에서 프로세스 문맥으로 불린다.
 *
 * 호출 체인:
 *   pci_set_full_power_state()(D3cold 에서 돌아온 경우) → [이 함수]
 *     → pci_update_resource()
 */
static void pci_restore_bars(struct pci_dev *dev)
{
	int i; /* [한국어] resource 인덱스 */

	for (i = 0; i < PCI_BRIDGE_RESOURCES; i++) /* [한국어] 표준 BAR 와 ROM 까지만 — PCI_BRIDGE_RESOURCES 부터는 브리지 윈도라 대상이 아니다 */
		pci_update_resource(dev, i); /* [한국어] resource[i] 의 주소를 해당 BAR 레지스터에 다시 써 넣는다 */
}

/*
 * [한국어] 아래 platform_pci_* 묶음은 "PCI 규격이 아니라 플랫폼 펌웨어가 하는
 * 전원 관리"로 가는 창구다. 장치의 PM capability 만으로는 D3cold 를 만들 수 없다 —
 * 전원 자체를 끊는 것은 슬롯의 전원 스위치를 쥔 펌웨어의 일이기 때문이다.
 * 그 펌웨어 창구가 둘이라 매번 갈래를 탄다.
 *   - ACPI(acpi_pci_*): x86/ARM 서버·데스크톱의 표준 경로. _PS0/_PS3 메서드로
 *     장치 전원을 켜고 끄며, _PR3 로 전원 자원 유무를 알린다.
 *   - Intel MID(mid_pci_*): CONFIG_X86_INTEL_MID 인 구형 임베디드 플랫폼 전용.
 *     ACPI 대신 전용 전원 관리 유닛을 쓴다. 그 옵션이 꺼져 있으면
 *     drivers/pci/pci.h 의 pci_use_mid_pm() 스텁이 항상 false 를 돌려주어
 *     컴파일러가 MID 갈래를 통째로 걷어 낸다. 오늘날 NVMe 를 쓰는 환경은
 *     예외 없이 ACPI 갈래로 간다.
 */
/*
 * [한국어]
 * platform_pci_power_manageable - 플랫폼 펌웨어가 이 장치의 전원을 다룰 수 있는가.
 *
 * @dev: 대상 PCI 장치.
 * @return: 펌웨어가 전원 상태를 바꿔 줄 수 있으면 true.
 *
 * 이 값이 false 면 커널은 장치 자신의 PM capability 로만 전원을 다룰 수 있고,
 * 그 범위는 D0~D3hot 까지다. 전원을 아예 끊는 D3cold 는 펌웨어의 협조가 있어야
 * 가능하다. pci_set_power_state() 와 pci_target_state() 가 목표 상태를 정할 때
 * 맨 먼저 확인하는 조건이다.
 *
 * 실행 문맥: ACPI 갈래는 ACPI 객체를 조회하므로 프로세스 문맥에서 부른다.
 *
 * 호출 체인:
 *   pci_set_power_state() / pci_target_state() / pci_pm_init() → [이 함수]
 *     → acpi_pci_power_manageable()
 */
static inline bool platform_pci_power_manageable(struct pci_dev *dev)
{
	if (pci_use_mid_pm()) /* [한국어] MID 플랫폼이면 */
		return true; /* [한국어] MID 전원 유닛이 모든 PCI 장치를 관리하므로 무조건 true */

	return acpi_pci_power_manageable(dev); /* [한국어] 표준 경로 — ACPI 에 이 장치의 전원 자원(_PR0/_PR3)이 정의돼 있는지 묻는다 */
}

/*
 * [한국어]
 * platform_pci_set_power_state - 플랫폼 펌웨어에 전원 상태 전이를 요청한다.
 *
 * @dev: 대상 PCI 장치.
 * @t: 목표 D-state.
 * @return: 0 성공, 음수 오류.
 *
 * PMCSR(장치의 PM Control/Status 레지스터)을 직접 쓰는 것과 다르다. 이쪽은
 * 슬롯에 공급되는 전원 자원 자체를 켜고 끈다. ACPI 갈래에서는 _PS0/_PS3
 * 메서드가 호출되어 전원 레일이 실제로 끊기거나 살아난다.
 * D3cold 로 내려가고 D0 로 돌아오는 일이 이 경로 없이는 불가능하다.
 *
 * 실행 문맥: ACPI 메서드 실행은 잠들 수 있다. 반드시 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_platform_power_transition() → [이 함수] → acpi_pci_set_power_state()
 */
static inline int platform_pci_set_power_state(struct pci_dev *dev,
					       pci_power_t t)
{
	if (pci_use_mid_pm()) /* [한국어] MID 플랫폼이면 */
		return mid_pci_set_power_state(dev, t); /* [한국어] MID 전용 전원 유닛에 요청한다 */

	return acpi_pci_set_power_state(dev, t); /* [한국어] 표준 경로 — ACPI 의 _PS0/_PS3 를 실행해 전원 자원을 조작한다 */
}

/*
 * [한국어]
 * platform_pci_get_power_state - 플랫폼 펌웨어가 보는 현재 전원 상태를 묻는다.
 *
 * @dev: 대상 PCI 장치.
 * @return: 펌웨어가 보고하는 D-state. 알 수 없으면 PCI_UNKNOWN.
 *
 * D3cold 는 장치의 PMCSR 을 읽어서는 알 수 없다. 전원이 없으니 config 접근
 * 자체가 응답하지 않기 때문이다. 그래서 "지금 D3cold 인가"는 전원 레일을 쥔
 * 펌웨어에게 물어야 한다. pci_update_current_state() 가 맨 먼저 이 함수를
 * 부르는 이유가 바로 그것이다.
 *
 * 실행 문맥: ACPI 조회. 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_update_current_state() / pci_power_up() → [이 함수]
 *     → acpi_pci_get_power_state()
 */
static inline pci_power_t platform_pci_get_power_state(struct pci_dev *dev)
{
	if (pci_use_mid_pm()) /* [한국어] MID 플랫폼이면 */
		return mid_pci_get_power_state(dev); /* [한국어] MID 전원 유닛에 묻는다 */

	return acpi_pci_get_power_state(dev); /* [한국어] 표준 경로 — ACPI 가 보고하는 전원 상태 */
}

/*
 * [한국어]
 * platform_pci_refresh_power_state - 펌웨어가 캐시한 전원 상태를 다시 읽게 한다.
 *
 * @dev: 대상 PCI 장치.
 * @return: 없음.
 *
 * ACPI 계층은 전원 자원의 상태를 캐시해 둔다. 시스템 전체가 절전에서 돌아온
 * 직후처럼 캐시를 믿을 수 없는 시점에는 실제 하드웨어를 다시 읽어 캐시를
 * 갱신해야 한다. 그 요청을 보내는 것이 전부다.
 * MID 갈래에는 대응하는 개념이 없어 아무 것도 하지 않는다.
 *
 * 실행 문맥: ACPI 조회. 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_refresh_power_state() → [이 함수] → acpi_pci_refresh_power_state()
 */
static inline void platform_pci_refresh_power_state(struct pci_dev *dev)
{
	if (!pci_use_mid_pm()) /* [한국어] MID 가 아닐 때만 — MID 에는 대응하는 캐시가 없다 */
		acpi_pci_refresh_power_state(dev); /* [한국어] ACPI 전원 자원 상태를 하드웨어에서 다시 읽게 한다 */
}

/*
 * [한국어]
 * platform_pci_choose_state - 시스템 절전 시 이 장치를 어느 D-state 로 둘지 펌웨어에 묻는다.
 *
 * @dev: 대상 PCI 장치.
 * @return: 펌웨어가 권하는 D-state. 판단할 수 없으면 PCI_POWER_ERROR.
 *
 * ACPI 는 장치마다 _SxD 메서드로 "시스템이 S3 일 때 이 장치는 D3 로 두어라"
 * 같은 지시를 담을 수 있다. 펌웨어가 하드웨어의 사정을 아는 경우가 있어
 * 커널이 임의로 정하기보다 먼저 물어보는 것이다.
 * 반환값 PCI_POWER_ERROR 는 오류가 아니라 "지시가 없으니 커널이 알아서 정하라"는
 * 뜻으로 pci_choose_state() 가 해석한다.
 *
 * 실행 문맥: ACPI 메서드 조회. 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_choose_state() / pci_target_state() → [이 함수] → acpi_pci_choose_state()
 */
static inline pci_power_t platform_pci_choose_state(struct pci_dev *dev)
{
	if (pci_use_mid_pm()) /* [한국어] MID 갈래에는 이런 지시 메서드가 없다 */
		return PCI_POWER_ERROR; /* [한국어] "지시 없음"을 뜻한다. 호출자는 자체 규칙으로 상태를 정한다 */

	return acpi_pci_choose_state(dev); /* [한국어] 표준 경로 — ACPI 의 _SxD 를 조회한다 */
}

/*
 * [한국어]
 * platform_pci_set_wakeup - 이 장치의 원격 깨우기(wakeup) 를 펌웨어 수준에서 켜고 끈다.
 *
 * @dev: 대상 PCI 장치.
 * @enable: true 면 깨우기 허용, false 면 금지.
 * @return: 0 성공, 음수 오류.
 *
 * PME(Power Management Event) 신호는 장치가 낸다고 끝이 아니다. 그 신호를
 * 받아 시스템을 깨우려면 GPE(General Purpose Event) 같은 플랫폼 쪽 경로가
 * 열려 있어야 한다. ACPI 갈래에서 그것을 여닫는 것이 이 함수다.
 * 장치 쪽 PME 활성화는 별개이며 __pci_pme_active() 가 담당한다 — 둘 다
 * 켜져야 실제로 깨어난다.
 *
 * 실행 문맥: ACPI 조작. 프로세스 문맥.
 *
 * 호출 체인:
 *   __pci_enable_wake() → [이 함수] → acpi_pci_wakeup()
 */
static inline int platform_pci_set_wakeup(struct pci_dev *dev, bool enable)
{
	if (pci_use_mid_pm()) /* [한국어] MID 갈래에는 대응 경로가 없다 */
		return PCI_POWER_ERROR; /* [한국어] 실패를 알린다. 이 반환값은 호출자에서 0 이 아니므로 오류로 취급된다 */

	return acpi_pci_wakeup(dev, enable); /* [한국어] 표준 경로 — ACPI 의 깨우기 경로(GPE 등)를 켜거나 끈다 */
}

/*
 * [한국어]
 * platform_pci_need_resume - 펌웨어가 이 장치를 반드시 깨워야 한다고 보는가.
 *
 * @dev: 대상 PCI 장치.
 * @return: 강제로 깨워야 하면 true.
 *
 * 시스템 절전에서 돌아올 때, 장치를 굳이 D0 로 올리지 않고 절전 상태 그대로
 * 두는 최적화(direct-complete)가 있다. 그러나 펌웨어가 절전 중에 그 장치의
 * 전원 상태를 바꿔 버렸다면 소프트웨어가 기억하는 상태와 어긋나므로
 * 반드시 깨워 상태를 다시 맞춰야 한다. 그 판단을 펌웨어에 묻는다.
 *
 * 실행 문맥: ACPI 조회. 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_dev_need_resume() → [이 함수] → acpi_pci_need_resume()
 */
static inline bool platform_pci_need_resume(struct pci_dev *dev)
{
	if (pci_use_mid_pm()) /* [한국어] MID 갈래에서는 */
		return false; /* [한국어] 강제 깨우기를 요구하지 않는다 */

	return acpi_pci_need_resume(dev); /* [한국어] 표준 경로 — ACPI 가 상태 불일치를 알리면 true */
}

/*
 * [한국어]
 * platform_pci_bridge_d3 - 이 브리지를 D3 로 내려도 되는지 펌웨어에 묻는다.
 *
 * @dev: 대상 브리지(Root Port 또는 Switch Port).
 * @return: 허용되면 true.
 *
 * 브리지를 D3 로 내리면 그 아래 모든 장치의 전원이 함께 끊긴다. 절전 효과는
 * 크지만, 다시 깨울 때 링크를 재훈련하고 아래 장치를 모두 복원해야 해서
 * 플랫폼이 제대로 지원하지 않으면 장치를 잃는다.
 * ACPI 는 _PR3(전원 자원)나 핫플러그 관련 정보로 그 가부를 알려 준다.
 * NVMe SSD 가 붙은 Root Port 가 D3 로 갈 수 있는지가 이 판단에 걸린다 —
 * 갈 수 있으면 유휴 시 SSD 전원까지 완전히 끊어 노트북 대기 전력이 크게 준다.
 *
 * 실행 문맥: ACPI 조회. 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_bridge_d3_possible() → [이 함수] → acpi_pci_bridge_d3()
 */
static inline bool platform_pci_bridge_d3(struct pci_dev *dev)
{
	if (pci_use_mid_pm()) /* [한국어] MID 갈래에서는 */
		return false; /* [한국어] 브리지 D3 를 허용하지 않는다 */

	return acpi_pci_bridge_d3(dev); /* [한국어] 표준 경로 — ACPI 에 물어본다 */
}

/**
 * pci_update_current_state - Read power state of given device and cache it
 * @dev: PCI device to handle.
 * @state: State to cache in case the device doesn't have the PM capability
 *
 * The power state is read from the PMCSR register, which however is
 * inaccessible in D3cold.  The platform firmware is therefore queried first
 * to detect accessibility of the register.  In case the platform firmware
 * reports an incorrect state or the device isn't power manageable by the
 * platform at all, we try to detect D3cold by testing accessibility of the
 * vendor ID in config space.
 */
/*
 * [한국어]
 * pci_update_current_state - 하드웨어의 실제 전원 상태를 읽어 dev->current_state 에 캐시한다.
 *
 * @dev: 대상 PCI 장치.
 * @state: PM capability 가 없어 읽을 방법이 없을 때 그냥 믿고 기록할 값.
 * @return: 없음.
 *
 * 커널은 각 장치의 D-state 를 dev->current_state 에 들고 다니는데, 이 값이
 * 실제와 어긋나면 이미 잠든 장치에 명령을 내리거나 깨어 있는 장치를 다시
 * 깨우는 일이 생긴다. 그래서 전원 전이 전후와 resume 직후에 실물을 다시 읽는다.
 *
 * 읽는 순서에 이유가 있다.
 *   1) 먼저 플랫폼에 묻는다. D3cold 는 전원이 없어 PMCSR 을 읽을 수 없으므로,
 *      레지스터를 읽기 전에 펌웨어 답부터 확인해야 한다.
 *   2) PM capability 가 있으면 PMCSR 의 하위 2비트를 읽는다. 이 두 비트가
 *      곧 D0~D3hot 을 나타낸다.
 *   3) 읽은 값이 all-ones 면(PCI_POSSIBLE_ERROR) 장치가 응답하지 않은 것이다.
 *      원본 주석대로, 펌웨어가 상태를 잘못 보고했거나 애초에 플랫폼이 이
 *      장치의 전원을 관리하지 않는 경우를 여기서 걸러 D3cold 로 판정한다.
 *   4) PM capability 조차 없으면 읽을 곳이 없으니 호출자가 넘긴 @state 를 믿는다.
 *
 * 실행 문맥: config space 를 읽는다. 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_power_up(), pci_set_full_power_state(), pci_refresh_power_state(),
 *   pci_enable_device_flags() → [이 함수]
 *     → platform_pci_get_power_state(), pci_read_config_word()
 */
void pci_update_current_state(struct pci_dev *dev, pci_power_t state)
{
	if (platform_pci_get_power_state(dev) == PCI_D3cold) { /* [한국어] 전원 자체가 끊긴 상태인지는 펌웨어만 안다. 레지스터를 읽기 전에 먼저 확인한다 */
		dev->current_state = PCI_D3cold; /* [한국어] D3cold 로 확정. 이 상태에서는 config 접근이 무의미하다 */
	} else if (dev->pm_cap) { /* [한국어] 전원은 있고 PM capability 도 있으면 장치 레지스터로 정확히 알 수 있다 */
		u16 pmcsr; /* [한국어] PM Control/Status 레지스터 값 */

		pci_read_config_word(dev, dev->pm_cap + PCI_PM_CTRL, &pmcsr); /* [한국어] PM capability 시작 + PCI_PM_CTRL(4) 위치가 PMCSR 이다 */
		if (PCI_POSSIBLE_ERROR(pmcsr)) { /* [한국어] all-ones 가 읽혔다 — 응답 없는 config 읽기의 표식이다. 장치가 사라졌거나 이미 전원이 끊겼다 */
			dev->current_state = PCI_D3cold; /* [한국어] 전원이 없는 것으로 본다 */
			return; /* [한국어] 더 볼 것이 없다 */
		}
		dev->current_state = pmcsr & PCI_PM_CTRL_STATE_MASK; /* [한국어] PMCSR 하위 2비트(PCI_PM_CTRL_STATE_MASK)가 곧 현재 D-state 다. 0=D0, 1=D1, 2=D2, 3=D3hot */
	} else {
		dev->current_state = state; /* [한국어] 호출자가 알려 준 값을 그대로 믿는다 */
	}
}

/**
 * pci_refresh_power_state - Refresh the given device's power state data
 * @dev: Target PCI device.
 *
 * Ask the platform to refresh the devices power state information and invoke
 * pci_update_current_state() to update its current PCI power state.
 */
/*
 * [한국어]
 * pci_refresh_power_state - 펌웨어 캐시와 소프트웨어 캐시를 모두 실물에 맞춘다.
 *
 * @dev: 대상 PCI 장치.
 * @return: 없음.
 *
 * 전원 상태는 두 겹으로 캐시돼 있다 — ACPI 계층이 전원 자원 상태를,
 * 커널이 dev->current_state 를 들고 있다. 시스템 절전에서 돌아온 직후처럼
 * 그 사이 펌웨어가 몰래 상태를 바꿨을 수 있는 시점에는 둘 다 못 믿는다.
 * 그래서 아래쪽(ACPI) 캐시를 먼저 갱신하고, 그 결과를 근거로 위쪽
 * (dev->current_state) 을 갱신한다. 순서가 반대면 갱신되지 않은 ACPI 답을
 * 그대로 믿게 되어 의미가 없다.
 *
 * 실행 문맥: ACPI 조회 + config 읽기. 프로세스 문맥.
 *
 * 호출 체인:
 *   pci-driver.c 의 시스템 resume 경로 → [이 함수]
 *     → platform_pci_refresh_power_state() → pci_update_current_state()
 */
void pci_refresh_power_state(struct pci_dev *dev)
{
	platform_pci_refresh_power_state(dev); /* [한국어] 먼저 ACPI 쪽 캐시를 실물에서 다시 읽게 한다 */
	pci_update_current_state(dev, dev->current_state); /* [한국어] 그 갱신된 답을 바탕으로 dev->current_state 를 맞춘다. 두 번째 인자는 PM capability 가 없을 때만 쓰이는 대체값이라 현재 값을 그대로 넘긴다 */
}

/**
 * pci_platform_power_transition - Use platform to change device power state
 * @dev: PCI device to handle.
 * @state: State to put the device into.
 */
/*
 * [한국어]
 * pci_platform_power_transition - 펌웨어를 통해 전원 상태를 바꾸고 캐시를 맞춘다.
 *
 * @dev: 대상 PCI 장치.
 * @state: 목표 D-state.
 * @return: 0 성공, 음수면 펌웨어가 전이를 수행하지 못했다.
 *
 * 장치 자신의 PMCSR 로는 D0~D3hot 까지만 갈 수 있다. 전원 레일을 끊는
 * D3cold 는 펌웨어만 할 수 있어 이 경로가 따로 있다.
 * 전이에 성공하면 실물을 다시 읽어 캐시를 맞춘다.
 *
 * 실패했을 때의 처리가 미묘하다. PM capability 조차 없는 장치는 전원 상태를
 * 읽거나 바꿀 수단이 아예 없으므로, 펌웨어가 실패했다면 그 장치는 계속
 * 켜져 있다고 보는 것이 안전하다. 그래서 D0 로 기록한다. PM capability 가
 * 있는 장치는 나중에 PMCSR 을 읽어 정확한 값을 알 수 있으므로 손대지 않는다.
 *
 * 실행 문맥: ACPI 메서드 실행. 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_power_up() / pci_set_low_power_state() 등 → [이 함수]
 *     → platform_pci_set_power_state() → pci_update_current_state()
 */
int pci_platform_power_transition(struct pci_dev *dev, pci_power_t state)
{
	int error; /* [한국어] 펌웨어 전이 결과 */

	error = platform_pci_set_power_state(dev, state); /* [한국어] ACPI 의 _PS0/_PS3 등을 실행해 전원 자원을 조작한다 */
	if (!error) /* [한국어] 전이에 성공했으면 */
		pci_update_current_state(dev, state); /* [한국어] 실물을 다시 읽어 dev->current_state 를 맞춘다 */
	else if (!dev->pm_cap) /* Fall back to PCI_D0 */
		dev->current_state = PCI_D0; /* [한국어] PM capability 도 없고 펌웨어 전이도 실패했다 — 상태를 알아낼 방법이 전혀 없으므로 켜져 있다고 보는 쪽이 안전하다 */

	return error; /* [한국어] 호출자는 이 값으로 다음 단계(PMCSR 직접 조작 등)를 결정한다 */
}
EXPORT_SYMBOL_GPL(pci_platform_power_transition);

/*
 * [한국어]
 * pci_resume_one - 장치 하나에 런타임 resume 을 요청하는 pci_walk_bus 콜백.
 *
 * @pci_dev: 순회 중 만난 장치.
 * @ign: pci_walk_bus 가 넘겨 주는 사용자 데이터. 여기서는 쓰지 않는다.
 * @return: 항상 0 — pci_walk_bus 규약에서 0 이 아닌 값은 "순회를 중단하라"는
 *          뜻이므로, 모든 장치를 빠짐없이 훑기 위해 언제나 0 을 돌려준다.
 *
 * pm_request_resume() 은 요청만 큐에 넣고 즉시 돌아온다. 실제 깨우기는 PM
 * 워크큐가 나중에 수행하므로 이 콜백은 잠들지 않는다. pci_walk_bus 가
 * 버스 락을 쥔 채 콜백을 부르기 때문에 이 성질이 중요하다.
 *
 * 호출 체인:
 *   pci_resume_bus() → pci_walk_bus() → [이 함수] → pm_request_resume()
 */
static int pci_resume_one(struct pci_dev *pci_dev, void *ign)
{
	pm_request_resume(&pci_dev->dev); /* [한국어] 비동기 resume 요청 — 큐에 넣기만 하고 바로 돌아온다 */
	return 0; /* [한국어] 0 을 돌려 순회를 계속하게 한다 */
}

/**
 * pci_resume_bus - Walk given bus and runtime resume devices on it
 * @bus: Top bus of the subtree to walk.
 */
/*
 * [한국어]
 * pci_resume_bus - 버스 아래 모든 장치에 런타임 resume 을 요청한다.
 *
 * @bus: 훑을 서브트리의 꼭대기 버스. NULL 이면 아무 것도 하지 않는다.
 * @return: 없음.
 *
 * 브리지를 D3 에서 D0 로 올렸다면 그 아래 장치들도 다시 쓸 수 있는 상태가
 * 됐다는 뜻이다. 하지만 런타임 PM 은 각 장치가 스스로 상태를 관리하므로,
 * 아래 장치들에게 "너희도 깨어나라"고 알려 주어야 한다.
 * 요청만 걸어 두고 실제 깨우기는 각 장치의 PM 코어가 수행한다.
 *
 * 실행 문맥: pci_walk_bus 가 pci_bus_sem 을 쥐고 순회한다. 콜백이 잠들지
 * 않도록 비동기 요청만 거는 이유가 그것이다.
 *
 * 호출 체인:
 *   pci_bridge_d3_update() 등 브리지 전원 경로 → [이 함수]
 *     → pci_walk_bus() → pci_resume_one()
 */
void pci_resume_bus(struct pci_bus *bus)
{
	if (bus) /* [한국어] NULL 버스 방어 — 호출자가 브리지의 subordinate 버스를 그대로 넘겨 NULL 일 수 있다 */
		pci_walk_bus(bus, pci_resume_one, NULL); /* [한국어] 서브트리 전체를 훑으며 각 장치에 pci_resume_one() 을 적용한다 */
}

/*
 * [한국어]
 * pci_dev_wait - 리셋 뒤 장치가 config 요청에 정상 응답할 때까지 기다린다.
 *
 * @dev: 기다릴 대상 장치.
 * @reset_type: 로그에 찍을 리셋 이름("FLR", "bus reset" 등).
 * @timeout: 총 대기 상한(ms).
 * @return: 0 준비 완료. -ENOTTY 는 장치가 분리됐거나 시간 안에 살아나지 못한 경우.
 *
 * 리셋 직후의 장치는 살아 있어도 아직 초기화 중일 수 있다. 그 상태에서
 * config 요청을 받으면 PCIe 는 RRS(Request Retry Status)로 "나중에 다시
 * 물어라"라고 답한다. 문제는 그 답이 소프트웨어에 어떻게 보이느냐가
 * 상위 Root Port 설정에 따라 달라진다는 점이다.
 *
 *   - Root Port 가 Configuration RRS Software Visibility 를 켰다면,
 *     Vendor ID 읽기가 RRS 를 나타내는 특별한 값(0x0001)을 돌려준다.
 *     그래서 Vendor ID 를 읽어 그 값이 아니게 될 때까지 기다린다.
 *   - 그 기능이 없으면 하드웨어가 알아서 재시도하고, 끝내 실패하면
 *     ~0(all-ones)을 합성해 돌려준다. 그런데 존재하지 않는 장치와 VF 도
 *     Vendor ID 로 ~0 을 주기 때문에 구분이 되지 않는다. 그래서 이쪽
 *     갈래에서는 Vendor ID 대신 Command 레지스터를 읽는다. Command 는
 *     정상 장치라면 결코 all-ones 가 아니다.
 *
 * 대기는 1ms 에서 시작해 두 배씩 늘린다(1, 2, 4, ... ms). PCI_RESET_WAIT(1초)를
 * 넘기면 로그를 남기고, 그 시점에 링크 자체가 죽은 경우를 대비해 상위
 * 브리지의 링크 재훈련을 한 번만 시도한다 — 성공하면 대기를 처음부터 다시 센다.
 *
 * 실행 문맥: msleep 으로 잔다. 반드시 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_reset_secondary_bus(), pcie_flr(), pci_af_flr(), pci_pm_reset(),
 *   pci_bridge_wait_for_secondary_bus() → [이 함수]
 *     → pci_read_config_dword(), pcie_failed_link_retrain(), msleep()
 */
static int pci_dev_wait(struct pci_dev *dev, char *reset_type, int timeout)
{
	int delay = 1; /* [한국어] 다음 대기 길이(ms). 1 에서 시작해 두 배씩 늘린다 */
	bool retrain = false; /* [한국어] 링크 재훈련을 아직 시도하지 않았는지. 한 번만 시도하려고 플래그로 둔다 */
	struct pci_dev *root, *bridge; /* [한국어] root 는 RRS 가시성 설정을 확인할 Root Port, bridge 는 재훈련을 걸 상위 브리지 */

	root = pcie_find_root_port(dev); /* [한국어] 이 장치 위쪽의 Root Port 를 찾는다. RRS Software Visibility 는 Root Port 의 설정이다 */

	if (pci_is_pcie(dev)) { /* [한국어] PCIe 장치일 때만 링크 재훈련이 의미가 있다. 재래식 PCI 에는 훈련할 링크가 없다 */
		bridge = pci_upstream_bridge(dev); /* [한국어] 재훈련을 걸 대상은 바로 위 브리지다 */
		if (bridge) /* [한국어] 루트 버스에 직접 붙은 장치라면 상위 브리지가 없다 */
			retrain = true; /* [한국어] 브리지가 있을 때만 재훈련 시도를 예약한다 */
	}

	/*
	 * The caller has already waited long enough after a reset that the
	 * device should respond to config requests, but it may respond
	 * with Request Retry Status (RRS) if it needs more time to
	 * initialize.
	 *
	 * If the device is below a Root Port with Configuration RRS
	 * Software Visibility enabled, reading the Vendor ID returns a
	 * special data value if the device responded with RRS.  Read the
	 * Vendor ID until we get non-RRS status.
	 *
	 * If there's no Root Port or Configuration RRS Software Visibility
	 * is not enabled, the device may still respond with RRS, but
	 * hardware may retry the config request.  If no retries receive
	 * Successful Completion, hardware generally synthesizes ~0
	 * (PCI_ERROR_RESPONSE) data to complete the read.  Reading Vendor
	 * ID for VFs and non-existent devices also returns ~0, so read the
	 * Command register until it returns something other than ~0.
	 */
	for (;;) { /* [한국어] 준비될 때까지, 또는 시간이 다할 때까지 반복한다 */
		u32 id; /* [한국어] 읽어 온 config 값(Vendor ID 또는 Command) */

		if (pci_dev_is_disconnected(dev)) { /* [한국어] 그 사이 장치가 뽑혔거나 오류로 끊겼다면 기다려도 소용없다 */
			pci_dbg(dev, "disconnected; not waiting\n"); /* [한국어] 이유를 남긴다 */
			return -ENOTTY; /* [한국어] -ENOTTY 로 "이 방법으로는 안 된다"를 알린다. 상위 리셋 코드는 다른 방법을 시도한다 */
		}

		if (root && root->config_rrs_sv) { /* [한국어] Root Port 가 RRS 를 소프트웨어에 보여 주도록 설정돼 있다면 */
			pci_read_config_dword(dev, PCI_VENDOR_ID, &id); /* [한국어] Vendor ID(오프셋 0x00)를 32비트로 읽는다 — Device ID 까지 함께 온다 */
			if (!pci_bus_rrs_vendor_id(id)) /* [한국어] RRS 를 뜻하는 특별한 Vendor ID 값이 아니면 장치가 준비된 것이다 */
				break; /* [한국어] 대기 종료 */
		} else {
			pci_read_config_dword(dev, PCI_COMMAND, &id); /* [한국어] 그래서 Command 레지스터(오프셋 0x04)를 읽는다. 정상 장치라면 all-ones 일 수 없다 */
			if (!PCI_POSSIBLE_ERROR(id)) /* [한국어] all-ones 가 아니면 정상 응답이다 */
				break; /* [한국어] 대기 종료 */
		}

		if (delay > timeout) { /* [한국어] 누적 대기가 상한을 넘었다 */
			pci_warn(dev, "not ready %dms after %s; giving up\n", /* [한국어] 포기 사실을 경고로 남긴다 */
				 delay - 1, reset_type); /* [한국어] delay 는 이미 다음 값으로 두 배가 돼 있으므로 실제 경과는 delay-1 이다(1+2+4+...+n = 2n-1) */
			return -ENOTTY; /* [한국어] -ENOTTY 로 실패를 알린다 */
		}

		if (delay > PCI_RESET_WAIT) { /* [한국어] 1초를 넘겨도 안 살아난다 — 스펙이 보장하는 시간을 이미 지났다 */
			if (retrain) { /* [한국어] 아직 링크 재훈련을 시도하지 않았다면 */
				retrain = false; /* [한국어] 한 번만 시도하도록 플래그를 내린다 */
				if (pcie_failed_link_retrain(bridge) == 0) { /* [한국어] 링크가 죽어 config 가 닿지 않는 경우일 수 있다. 재훈련에 성공하면 */
					delay = 1; /* [한국어] 장치는 이제 막 링크를 얻은 셈이므로 대기 시간을 처음부터 다시 센다 */
					continue; /* [한국어] 곧바로 다시 확인한다 */
				}
			}
			pci_info(dev, "not ready %dms after %s; waiting\n", /* [한국어] 재훈련으로도 해결되지 않았으면 계속 기다린다는 사실만 알린다 */
				 delay - 1, reset_type); /* [한국어] 지금까지 실제로 기다린 시간 */
		}

		msleep(delay); /* [한국어] 현재 간격만큼 잔다 */
		delay *= 2; /* [한국어] 지수 백오프 — 다음 대기는 두 배로 늘린다. 준비가 빠른 장치는 몇 ms 만에 끝나고, 느린 장치도 폴링 횟수가 로그 규모로 억제된다 */
	}

	if (delay > PCI_RESET_WAIT) /* [한국어] 1초를 넘겨 걸렸다면 눈에 띄게 남길 가치가 있다 */
		pci_info(dev, "ready %dms after %s\n", delay - 1, /* [한국어] 정보 수준으로 기록한다 */
			 reset_type); /* [한국어] 어떤 리셋 뒤였는지도 함께 */
	else
		pci_dbg(dev, "ready %dms after %s\n", delay - 1, /* [한국어] 디버그 수준으로만 남긴다 */
			reset_type); /* [한국어] 어떤 리셋 뒤였는지 */

	return 0; /* [한국어] 장치가 config 요청에 정상 응답한다 */
}

/**
 * pci_power_up - Put the given device into D0
 * @dev: PCI device to power up
 *
 * On success, return 0 or 1, depending on whether or not it is necessary to
 * restore the device's BARs subsequently (1 is returned in that case).
 *
 * On failure, return a negative error code.  Always return failure if @dev
 * lacks a Power Management Capability, even if the platform was able to
 * put the device in D0 via non-PCI means.
 */
/*
 * [한국어]
 * pci_power_up - 장치를 D0 로 올리고, BAR 를 다시 써야 하는지 알려 준다.
 *
 * @dev: 깨울 PCI 장치.
 * @return: 0 = D0 로 올렸고 BAR 는 그대로다.
 *          1 = D0 로 올렸지만 내부 리셋이 있었을 수 있어 BAR 복원이 필요하다.
 *          음수 = 실패. PM capability 가 없으면 플랫폼이 D0 로 올려 주었더라도
 *                 언제나 -EIO 다(원본 주석 참고).
 *
 * 전원을 올리는 일은 두 층에서 이뤄진다. 먼저 플랫폼(ACPI)에 전원 자원을
 * 켜 달라고 하고, 그 다음 장치의 PMCSR 로 D-state 를 D0 로 바꾼다. 순서가
 * 중요하다 — 전원이 없으면 PMCSR 을 쓸 수조차 없다.
 *
 * 반환값 1 의 의미가 이 함수의 핵심이다. PCI PM 규격은 D3hot 에서 D0 로
 * 돌아올 때 장치가 내부 리셋을 수행해도 된다고 허용한다(원본 주석의 5.4.1절).
 * 그러면 BAR 를 비롯한 config 가 초기값으로 돌아가 버린다. 장치가
 * PMCSR 의 No_Soft_Reset 비트로 "나는 리셋하지 않는다"고 선언했다면 안전하지만,
 * 그렇지 않으면 호출자가 BAR 를 다시 써야 한다. 그 판단 결과를 1 로 알린다.
 *
 * PM capability 가 없을 때 굳이 -EIO 를 돌려주는 이유는, 이 함수의 계약이
 * "PCI PM 으로 D0 를 만들었다"이기 때문이다. 플랫폼이 전원을 켜 주었을 수는
 * 있어도 D-state 를 PCI 방식으로 확인·설정할 수단이 없으므로 성공이라
 * 말할 수 없다. 대신 current_state 는 최선을 다해 기록해 둔다.
 *
 * 실행 문맥: ACPI 조작과 config 접근, 그리고 전이 지연 동안 잠든다.
 * 반드시 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_set_full_power_state(), pci_restore_state() 경로, pci-driver.c 의 resume
 *     → [이 함수] → platform_pci_set_power_state(), pci_read/write_config_word(),
 *        pci_dev_d3_sleep()
 */
int pci_power_up(struct pci_dev *dev)
{
	bool need_restore; /* [한국어] D0 도착 후 호출자가 BAR 를 다시 써야 하는지 */
	pci_power_t state; /* [한국어] PMCSR 에서 읽어 낸, 이 함수에 들어오기 전의 실제 D-state */
	u16 pmcsr; /* [한국어] PM Control/Status 레지스터 값 */

	platform_pci_set_power_state(dev, PCI_D0); /* [한국어] 먼저 플랫폼에 전원 자원을 켜 달라고 한다. D3cold 였다면 이것 없이는 config 접근 자체가 불가능하다 */

	if (!dev->pm_cap) { /* [한국어] PM capability 가 없는 장치 — PCI 방식으로 D-state 를 다룰 수 없다 */
		state = platform_pci_get_power_state(dev); /* [한국어] 그래도 플랫폼이 아는 상태는 기록해 둔다 */
		if (state == PCI_UNKNOWN) /* [한국어] 플랫폼도 모른다면 */
			dev->current_state = PCI_D0; /* [한국어] 전원이 켜져 있다고 보는 쪽이 안전하다 */
		else
			dev->current_state = state; /* [한국어] 그 값을 그대로 기록한다 */

		return -EIO; /* [한국어] 상태 기록과 별개로, PCI PM 으로 D0 를 만들지는 못했으므로 실패로 보고한다 */
	}

	if (pci_dev_is_disconnected(dev)) { /* [한국어] 그 사이 장치가 뽑혔거나 오류로 끊겼다면 config 를 써 봐야 소용없다 */
		dev->current_state = PCI_D3cold; /* [한국어] 응답하지 않는 장치는 전원이 없는 것과 같다고 기록한다 */
		return -EIO; /* [한국어] 실패 */
	}

	pci_read_config_word(dev, dev->pm_cap + PCI_PM_CTRL, &pmcsr); /* [한국어] PMCSR 을 읽어 지금 어느 D-state 인지 확인한다 */
	if (PCI_POSSIBLE_ERROR(pmcsr)) { /* [한국어] all-ones 가 읽혔다 — 응답 없는 config 읽기의 표식이다 */
		pci_err(dev, "Unable to change power state from %s to D0, device inaccessible\n", /* [한국어] 어느 상태에서 D0 로 못 갔는지 남긴다 */
			pci_power_name(dev->current_state)); /* [한국어] pci_power_names[] 표를 통해 "D3hot" 같은 문자열로 바꾼다 */
		dev->current_state = PCI_D3cold; /* [한국어] 접근할 수 없는 장치는 D3cold 로 기록한다 */
		return -EIO; /* [한국어] 실패 */
	}

	state = pmcsr & PCI_PM_CTRL_STATE_MASK; /* [한국어] PMCSR 하위 2비트가 현재 D-state 다 */

	need_restore = (state == PCI_D3hot || dev->current_state >= PCI_D3hot) && /* [한국어] 지금 D3hot 이거나, 소프트웨어가 D3hot 이상(D3cold 포함)으로 알고 있었다면 내부 리셋이 일어났을 수 있고 */
			!(pmcsr & PCI_PM_CTRL_NO_SOFT_RESET); /* [한국어] No_Soft_Reset 비트가 0 이면 실제로 리셋한다는 선언이다 — 그때만 BAR 복원이 필요하다. 이 비트가 1 이면 장치가 D3hot 을 오가도 config 를 유지한다고 약속한 것이다 */

	if (state == PCI_D0) /* [한국어] 이미 D0 라면 전이 자체가 필요 없다 */
		goto end; /* [한국어] 전이와 지연을 건너뛰고 마무리로 간다. need_restore 계산은 이미 끝나 있다 */

	/*
	 * Force the entire word to 0. This doesn't affect PME_Status, disables
	 * PME_En, and sets PowerState to 0.
	 */
	pci_write_config_word(dev, dev->pm_cap + PCI_PM_CTRL, 0); /* [한국어] 워드 전체를 0 으로 쓴다. PowerState 는 0(D0)이 되고 PME_En 은 꺼진다. PME_Status 는 W1C 라서 0 을 써도 지워지지 않아 보존된다 — 아직 처리하지 않은 깨우기 사건을 잃지 않기 위해서다 */

	/* Mandatory transition delays; see PCI PM 1.2. */
	if (state == PCI_D3hot) /* [한국어] D3hot 에서 올라온 경우 */
		pci_dev_d3_sleep(dev); /* [한국어] 규격이 정한 D3hot→D0 안정화 시간을 지킨다 */
	else if (state == PCI_D2) /* [한국어] D2 에서 올라온 경우 */
		udelay(PCI_PM_D2_DELAY); /* [한국어] D2→D0 은 훨씬 짧아 마이크로초 단위 지연으로 충분하다 */

end: /* [한국어] 이미 D0 였을 때 건너뛰어 오는 지점 */
	dev->current_state = PCI_D0; /* [한국어] D0 로 확정 기록한다 */
	if (need_restore) /* [한국어] 내부 리셋 가능성이 있었으면 */
		return 1; /* [한국어] 1 을 돌려 호출자가 BAR 를 복원하게 한다 */

	return 0; /* [한국어] BAR 가 살아 있으므로 할 일이 없다 */
}

/**
 * pci_set_full_power_state - Put a PCI device into D0 and update its state
 * @dev: PCI device to power up
 * @locked: whether pci_bus_sem is held
 *
 * Call pci_power_up() to put @dev into D0, read from its PCI_PM_CTRL register
 * to confirm the state change, restore its BARs if they might be lost and
 * reconfigure ASPM in accordance with the new power state.
 *
 * If pci_restore_state() is going to be called right after a power state change
 * to D0, it is more efficient to use pci_power_up() directly instead of this
 * function.
 */
/*
 * [한국어]
 * pci_set_full_power_state - D0 로 올린 뒤 확인·BAR 복원·ASPM 재설정까지 마친다.
 *
 * @dev: 깨울 PCI 장치.
 * @locked: 호출자가 이미 pci_bus_sem 을 쥐고 있는지. 아래 ASPM 코드가 그 락을
 *          다시 잡을지 말지 결정하는 데 쓴다.
 * @return: 0 성공(또는 이미 D0). 음수면 실패.
 *
 * pci_power_up() 이 하는 일에 세 가지를 더한다.
 *   1) PMCSR 을 다시 읽어 정말 D0 가 됐는지 확인한다. 장치가 전이를 거부하는
 *      경우가 실제로 있어, 성공했다고 가정하면 이후 접근이 모두 어긋난다.
 *   2) pci_power_up() 이 1 을 돌려주면 BAR 를 복원한다. 원본 주석이 드는
 *      예처럼, D3hot→D0 에서 내부 리셋을 하는 장치와 부팅 시 장치를 D3hot 에
 *      둔 채 넘기는 BIOS 가 실제로 존재한다.
 *   3) 상위 브리지의 ASPM 설정을 다시 계산한다. 링크 절전 정책은 양쪽 끝의
 *      전원 상태에 따라 달라지기 때문이다.
 *
 * 원본 주석이 일러 두듯, 곧바로 pci_restore_state() 를 부를 참이라면 이
 * 함수 대신 pci_power_up() 을 직접 쓰는 편이 낫다 — BAR 복원이 두 번 되기
 * 때문이다.
 *
 * 실행 문맥: 잠들 수 있다. 프로세스 문맥.
 *
 * 호출 체인:
 *   __pci_set_power_state() → [이 함수]
 *     → pci_power_up(), pci_restore_bars(), pcie_aspm_pm_state_change()
 */
static int pci_set_full_power_state(struct pci_dev *dev, bool locked)
{
	u16 pmcsr; /* [한국어] 확인용으로 다시 읽을 PMCSR */
	int ret; /* [한국어] pci_power_up() 의 결과(0/1/음수) */

	ret = pci_power_up(dev); /* [한국어] 실제 D0 전이를 맡긴다 */
	if (ret < 0) { /* [한국어] 실패한 경우 */
		if (dev->current_state == PCI_D0) /* [한국어] PM capability 가 없어 -EIO 를 받았더라도 플랫폼이 D0 로 올려 두었다면 */
			return 0; /* [한국어] 목적은 이룬 것이므로 성공으로 본다 */

		return ret; /* [한국어] 그 밖의 실패는 그대로 전달한다 */
	}

	pci_read_config_word(dev, dev->pm_cap + PCI_PM_CTRL, &pmcsr); /* [한국어] 전이 결과를 하드웨어에서 다시 확인한다 */
	dev->current_state = pmcsr & PCI_PM_CTRL_STATE_MASK; /* [한국어] 읽은 값으로 캐시를 갱신한다 */
	if (dev->current_state != PCI_D0) { /* [한국어] D0 가 아니면 장치가 전이를 거부한 것이다 */
		pci_info_ratelimited(dev, "Refused to change power state from %s to D0\n", /* [한국어] 로그가 넘치지 않도록 빈도를 제한해 알린다. 실패로 반환하지는 않는다 — 이후 접근이 실제로 되는 경우도 있어 진행을 막지는 않는다 */
				     pci_power_name(dev->current_state)); /* [한국어] 거부된 뒤 남아 있는 상태 이름 */
	} else if (ret > 0) { /* [한국어] D0 로 잘 갔고, pci_power_up() 이 BAR 복원이 필요하다고 알렸다면 */
		/*
		 * According to section 5.4.1 of the "PCI BUS POWER MANAGEMENT
		 * INTERFACE SPECIFICATION, REV. 1.2", a device transitioning
		 * from D3hot to D0 _may_ perform an internal reset, thereby
		 * going to "D0 Uninitialized" rather than "D0 Initialized".
		 * For example, at least some versions of the 3c905B and the
		 * 3c556B exhibit this behaviour.
		 *
		 * At least some laptop BIOSen (e.g. the Thinkpad T21) leave
		 * devices in a D3hot state at boot.  Consequently, we need to
		 * restore at least the BARs so that the device will be
		 * accessible to its driver.
		 */
		pci_restore_bars(dev); /* [한국어] resource[] 에 남아 있는 배정 결과를 BAR 에 다시 써 넣는다 */
	}

	if (dev->bus->self) /* [한국어] 루트 버스가 아니라 브리지 아래에 있다면 */
		pcie_aspm_pm_state_change(dev->bus->self, locked); /* [한국어] 그 브리지 링크의 ASPM 정책을 다시 계산한다. locked 는 pci_bus_sem 중복 획득을 피하려고 전달한다 */

	return 0; /* [한국어] D0 도달(또는 이미 D0) */
}

/**
 * __pci_dev_set_current_state - Set current state of a PCI device
 * @dev: Device to handle
 * @data: pointer to state to be set
 */
/*
 * [한국어]
 * __pci_dev_set_current_state - 장치 하나의 current_state 만 바꾸는 pci_walk_bus 콜백.
 *
 * @dev: 순회 중 만난 장치.
 * @data: pci_power_t 하나를 가리키는 포인터. pci_walk_bus 의 콜백 시그니처가
 *        void * 라서 이렇게 감싸 넘긴다.
 * @return: 항상 0 — pci_walk_bus 에서 0 이 아니면 순회가 중단되므로,
 *          모든 장치에 적용하기 위해 언제나 0 이다.
 *
 * 하드웨어를 건드리지 않고 소프트웨어 캐시만 고친다. 브리지가 D3cold 로
 * 내려가면 그 아래 장치는 config 접근조차 불가능해져 실물을 읽을 수 없으므로,
 * "논리적으로 이 상태가 됐다"고 기록만 해 두는 것이다.
 *
 * 호출 체인:
 *   pci_bus_set_current_state() / __pci_bus_set_current_state()
 *     → pci_walk_bus() → [이 함수]
 */
static int __pci_dev_set_current_state(struct pci_dev *dev, void *data)
{
	pci_power_t state = *(pci_power_t *)data; /* [한국어] void * 로 넘어온 값을 pci_power_t 로 되돌린다 */

	dev->current_state = state; /* [한국어] 하드웨어는 건드리지 않고 캐시만 갱신한다 */
	return 0; /* [한국어] 0 을 돌려 순회를 계속하게 한다 */
}

/**
 * pci_bus_set_current_state - Walk given bus and set current state of devices
 * @bus: Top bus of the subtree to walk.
 * @state: state to be set
 */
/*
 * [한국어]
 * pci_bus_set_current_state - 서브트리 전체의 전원 상태 캐시를 한 값으로 맞춘다.
 *
 * @bus: 훑을 서브트리의 꼭대기 버스. NULL 이면 아무 것도 하지 않는다.
 * @state: 기록할 D-state.
 * @return: 없음.
 *
 * 브리지 하나의 전원 상태를 바꾸면 그 아래 장치들의 실제 상태도 함께 바뀐다.
 * 예를 들어 브리지가 D3cold 로 내려가면 아래 장치도 전원을 잃는다. 그러나
 * 그 장치들의 config 에는 이제 닿을 수 없어 읽어서 확인할 방법이 없다.
 * 그래서 소프트웨어 캐시만 일괄로 맞춰 둔다.
 *
 * 실행 문맥: pci_walk_bus 가 pci_bus_sem 을 잡고 순회한다. 그 락을 이미
 * 쥐고 있는 호출자는 이 함수 대신 __pci_bus_set_current_state() 를 써야 한다.
 *
 * 호출 체인:
 *   전원 전이 경로 → [이 함수] → pci_walk_bus() → __pci_dev_set_current_state()
 */
void pci_bus_set_current_state(struct pci_bus *bus, pci_power_t state)
{
	if (bus) /* [한국어] NULL 버스 방어 — 브리지의 subordinate 버스가 없을 수 있다 */
		pci_walk_bus(bus, __pci_dev_set_current_state, &state); /* [한국어] 서브트리의 모든 장치에 같은 상태를 기록한다 */
}

/*
 * [한국어]
 * __pci_bus_set_current_state - 락 보유 여부에 따라 순회 방법을 골라 상태 캐시를 맞춘다.
 *
 * @bus: 훑을 서브트리의 꼭대기 버스.
 * @state: 기록할 D-state.
 * @locked: 호출자가 이미 pci_bus_sem 을 쥐고 있는가.
 * @return: 없음.
 *
 * pci_walk_bus() 는 내부에서 pci_bus_sem 을 잡는다. 이미 그 락을 쥔 호출자가
 * 부르면 자기 자신과 교착하므로, 그런 경우를 위해 락을 잡지 않는
 * pci_walk_bus_locked() 를 대신 쓴다. 전원 전이 경로는 락을 쥔 채 들어오는
 * 경우와 아닌 경우가 모두 있어 이 갈래가 필요하다.
 *
 * 실행 문맥: locked=true 면 pci_bus_sem 을 이미 쥔 상태여야 한다.
 *
 * 호출 체인:
 *   __pci_set_power_state() → [이 함수]
 *     → pci_walk_bus_locked() 또는 pci_walk_bus() → __pci_dev_set_current_state()
 */
static void __pci_bus_set_current_state(struct pci_bus *bus, pci_power_t state, bool locked)
{
	if (!bus) /* [한국어] 브리지에 하위 버스가 없으면 */
		return; /* [한국어] 훑을 것이 없다 */

	if (locked) /* [한국어] 호출자가 이미 pci_bus_sem 을 쥐고 있으면 */
		pci_walk_bus_locked(bus, __pci_dev_set_current_state, &state); /* [한국어] 락을 다시 잡지 않는 판을 쓴다 — 그러지 않으면 자기 자신과 교착한다 */
	else
		pci_walk_bus(bus, __pci_dev_set_current_state, &state); /* [한국어] 스스로 pci_bus_sem 을 잡는 일반 순회를 쓴다 */
}

/**
 * pci_set_low_power_state - Put a PCI device into a low-power state.
 * @dev: PCI device to handle.
 * @state: PCI power state (D1, D2, D3hot) to put the device into.
 * @locked: whether pci_bus_sem is held
 *
 * Use the device's PCI_PM_CTRL register to put it into a low-power state.
 *
 * RETURN VALUE:
 * -EINVAL if the requested state is invalid.
 * -EIO if device does not support PCI PM or its PM capabilities register has a
 * wrong version, or device doesn't support the requested state.
 * 0 if device already is in the requested state.
 * 0 if device's power state has been successfully changed.
 */
/*
 * [한국어]
 * pci_set_low_power_state - 장치의 PMCSR 로 D1/D2/D3hot 에 들어간다.
 *
 * @dev: 대상 PCI 장치.
 * @state: 목표 저전원 상태(PCI_D1, PCI_D2, PCI_D3hot).
 * @locked: pci_bus_sem 보유 여부. ASPM 재설정에 그대로 전달한다.
 * @return: 0 성공(또는 이미 그 상태). -EINVAL 잘못된 전이. -EIO PM capability
 *          없음 또는 장치가 그 상태를 지원하지 않음.
 *
 * 전원을 "내리는" 쪽은 올리는 쪽과 규칙이 다르다. PCI PM 규격상 D0 로는
 * 어느 상태에서든 갈 수 있지만, 저전원 상태끼리는 더 깊은 쪽으로만 갈 수 있다.
 * D1 에서 D3 는 되지만 D3 에서 D1 은 안 되고, 굳이 가려면 D0 를 거쳐야 한다.
 * 그래서 맨 먼저 전이 방향을 검사한다.
 *
 * D1 과 D2 는 선택 기능이라 장치가 지원하지 않을 수 있다. 지원 여부는 열거
 * 때 PM capability 에서 읽어 dev->d1_support / d2_support 에 담아 둔다.
 * D3hot 은 PM capability 가 있으면 언제나 지원된다.
 *
 * 상태를 쓴 뒤 규격이 정한 안정화 시간을 지키고, 다시 읽어 실제로 전이됐는지
 * 확인한다. 장치가 거부하는 경우가 있어 확인 없이는 믿을 수 없다.
 *
 * 참고 — NVMe 의 APST(Autonomous Power State Transition) 와는 다른 층이다.
 * APST 는 NVMe 컨트롤러가 스스로 자기 전력 상태를 바꾸는 NVMe 규격의 기능이고,
 * 여기서 다루는 D-state 는 PCI 링크·슬롯 수준의 전원 상태다.
 *
 * 실행 문맥: 전이 지연 동안 잠든다. 프로세스 문맥.
 *
 * 호출 체인:
 *   __pci_set_power_state() → [이 함수]
 *     → pci_read/write_config_word(), pci_dev_d3_sleep(), pcie_aspm_pm_state_change()
 */
static int pci_set_low_power_state(struct pci_dev *dev, pci_power_t state, bool locked)
{
	u16 pmcsr; /* [한국어] PM Control/Status 레지스터 값 */

	if (!dev->pm_cap) /* [한국어] PM capability 가 없으면 D-state 를 바꿀 레지스터 자체가 없다 */
		return -EIO; /* [한국어] -EIO */

	/*
	 * Validate transition: We can enter D0 from any state, but if
	 * we're already in a low-power state, we can only go deeper.  E.g.,
	 * we can go from D1 to D3, but we can't go directly from D3 to D1;
	 * we'd have to go from D3 to D0, then to D1.
	 */
	if (dev->current_state <= PCI_D3cold && dev->current_state > state) { /* [한국어] 현재 상태가 유효 범위 안이면서 목표보다 깊다면 얕은 쪽으로 되돌아가려는 것이다 — 규격이 금지한다 */
		pci_dbg(dev, "Invalid power transition (from %s to %s)\n", /* [한국어] 어떤 전이가 거부됐는지 남긴다 */
			pci_power_name(dev->current_state), /* [한국어] 현재 상태 이름 */
			pci_power_name(state)); /* [한국어] 목표 상태 이름 */
		return -EINVAL; /* [한국어] 잘못된 요청 */
	}

	/* Check if this device supports the desired state */
	if ((state == PCI_D1 && !dev->d1_support) /* [한국어] D1 을 요청했는데 장치가 D1 을 지원하지 않거나 */
	   || (state == PCI_D2 && !dev->d2_support)) /* [한국어] D2 를 요청했는데 D2 를 지원하지 않으면. 두 상태는 PM capability 에서 선택 사항이다 */
		return -EIO; /* [한국어] -EIO */

	if (dev->current_state == state) /* [한국어] 이미 목표 상태면 */
		return 0; /* [한국어] 할 일이 없다 */

	pci_read_config_word(dev, dev->pm_cap + PCI_PM_CTRL, &pmcsr); /* [한국어] 현재 PMCSR 을 읽는다. 다른 비트(PME_En, PME_Status)를 보존한 채 상태 필드만 바꾸기 위해서다 */
	if (PCI_POSSIBLE_ERROR(pmcsr)) { /* [한국어] all-ones 면 장치가 응답하지 않는다 */
		pci_err(dev, "Unable to change power state from %s to %s, device inaccessible\n", /* [한국어] 실패를 남긴다 */
			pci_power_name(dev->current_state), /* [한국어] 현재 상태 이름 */
			pci_power_name(state)); /* [한국어] 목표 상태 이름 */
		dev->current_state = PCI_D3cold; /* [한국어] 응답하지 않는 장치는 전원이 없는 것으로 본다 */
		return -EIO; /* [한국어] -EIO */
	}

	pmcsr &= ~PCI_PM_CTRL_STATE_MASK; /* [한국어] 상태 필드(하위 2비트)만 지운다. PME_En 과 PME_Status 는 건드리지 않는다 — 저전원으로 내려가면서 깨우기 설정을 잃으면 안 된다 */
	pmcsr |= state; /* [한국어] 목표 D-state 값을 그 자리에 넣는다. PCI_D1=1, PCI_D2=2, PCI_D3hot=3 이 그대로 비트 값이다 */

	/* Enter specified state */
	pci_write_config_word(dev, dev->pm_cap + PCI_PM_CTRL, pmcsr); /* [한국어] PMCSR 에 쓰는 순간 장치가 그 상태로 들어간다 */

	/* Mandatory power management transition delays; see PCI PM 1.2. */
	if (state == PCI_D3hot) /* [한국어] D3hot 로 내려가는 경우 */
		pci_dev_d3_sleep(dev); /* [한국어] 규격이 정한 안정화 시간을 지킨다. 이 지연 없이 곧바로 config 를 건드리면 응답하지 않는다 */
	else if (state == PCI_D2) /* [한국어] D2 로 내려가는 경우 */
		udelay(PCI_PM_D2_DELAY); /* [한국어] D2 는 훨씬 짧아 마이크로초 단위로 충분하다. D1 은 지연이 필요 없다 */

	pci_read_config_word(dev, dev->pm_cap + PCI_PM_CTRL, &pmcsr); /* [한국어] 실제로 전이됐는지 다시 읽어 확인한다 */
	dev->current_state = pmcsr & PCI_PM_CTRL_STATE_MASK; /* [한국어] 읽은 값으로 캐시를 갱신한다 */
	if (dev->current_state != state) /* [한국어] 목표와 다르면 장치가 전이를 거부한 것이다 */
		pci_info_ratelimited(dev, "Refused to change power state from %s to %s\n", /* [한국어] 로그 빈도를 제한해 알린다. 실패로 반환하지는 않는다 — 상태 캐시는 이미 실물에 맞춰 두었으므로 상위 로직은 정확한 값으로 동작한다 */
				     pci_power_name(dev->current_state), /* [한국어] 실제로 머무른 상태 */
				     pci_power_name(state)); /* [한국어] 가려던 상태 */

	if (dev->bus->self) /* [한국어] 루트 버스가 아니라 브리지 아래에 있다면 */
		pcie_aspm_pm_state_change(dev->bus->self, locked); /* [한국어] 링크 절전(ASPM) 정책을 새 전원 상태에 맞춰 다시 계산한다 */

	return 0; /* [한국어] 전이 시도를 마쳤다 */
}

/*
 * [한국어]
 * __pci_set_power_state - 전원 상태 전이의 공통 본체. 유효성 검사와 경로 선택을 맡는다.
 *
 * @dev: 대상 PCI 장치.
 * @state: 목표 D-state.
 * @locked: pci_bus_sem 보유 여부.
 * @return: 0 성공 또는 "할 필요 없음". 음수면 실패.
 *
 * 요청을 그대로 하드웨어에 넘기지 않고 여러 겹으로 거른다.
 *   1) 범위를 강제로 맞춘다(클램프). 잘못된 값으로 레지스터를 쓰는 것보다
 *      가장 가까운 유효 상태로 조정하는 편이 안전하다.
 *   2) 장치나 상위 브리지가 PCI PM 을 지원하지 않으면 D1/D2 요청을 조용히
 *      무시한다. 원본 주석대로, 이런 경우 D0 로 가는 요청만 의미가 있다.
 *   3) 이미 목표 상태면 아무 것도 하지 않는다.
 *   4) D3 진입을 금지하는 quirk 가 걸린 장치는 요청을 무시한다. 실제로 D3 에서
 *      돌아오지 못하는 하드웨어가 있어 목록으로 관리한다.
 *
 * 그 뒤 목적지에 따라 경로가 갈린다.
 *   - D0 → pci_set_full_power_state()
 *   - D3cold → 먼저 장치의 PMCSR 로 D3hot 까지 내려간 뒤, 플랫폼에 전원을
 *     끊게 한다. 전원을 끊기 전에 장치를 정리된 상태로 두어야 하기 때문이다.
 *     브리지를 껐다면 그 아래 계층 전체가 전원을 잃으므로 캐시를 함께 맞춘다.
 *   - 그 밖(D1/D2/D3hot) → PMCSR 전이 후 플랫폼에도 알린다.
 *
 * 반환값 규약이 미묘하다. 플랫폼 전이가 실패하면 그때서야 앞 단계(PMCSR)의
 * 결과를 돌려준다 — 플랫폼이 성공했다면 PMCSR 단계의 실패는 결과적으로
 * 문제가 되지 않기 때문이다.
 *
 * 실행 문맥: 잠들 수 있다. 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_set_power_state() / pci_set_power_state_locked() → [이 함수]
 *     → pci_set_full_power_state(), pci_set_low_power_state(),
 *        pci_platform_power_transition(), __pci_bus_set_current_state()
 */
static int __pci_set_power_state(struct pci_dev *dev, pci_power_t state, bool locked)
{
	int error; /* [한국어] 하위 전이 함수들의 반환값을 모아 두는 곳 */

	/* Bound the state we're entering */
	if (state > PCI_D3cold) /* [한국어] D3cold 보다 깊은 상태는 없다 */
		state = PCI_D3cold; /* [한국어] 가장 깊은 유효 상태로 맞춘다 */
	else if (state < PCI_D0) /* [한국어] D0 보다 얕은 상태도 없다 */
		state = PCI_D0; /* [한국어] 가장 얕은 유효 상태로 맞춘다 */
	else if ((state == PCI_D1 || state == PCI_D2) && pci_no_d1d2(dev)) /* [한국어] D1/D2 를 요청했는데 장치나 상위 브리지가 PCI PM 을 제대로 지원하지 않으면 */

		/*
		 * If the device or the parent bridge do not support PCI
		 * PM, ignore the request if we're doing anything other
		 * than putting it into D0 (which would only happen on
		 * boot).
		 */
		return 0; /* [한국어] 조용히 성공으로 처리한다. 전원을 못 내렸을 뿐 오류는 아니다 */

	/* Check if we're already there */
	if (dev->current_state == state) /* [한국어] 이미 목표 상태면 */
		return 0; /* [한국어] 건드릴 것이 없다 */

	if (state == PCI_D0) /* [한국어] D0 로 올리는 경우 */
		return pci_set_full_power_state(dev, locked); /* [한국어] 전용 경로로 넘긴다 — 확인과 BAR 복원, ASPM 재설정이 함께 필요하다 */

	/*
	 * This device is quirked not to be put into D3, so don't put it in
	 * D3
	 */
	if (state >= PCI_D3hot && (dev->dev_flags & PCI_DEV_FLAGS_NO_D3)) /* [한국어] D3 계열을 요청했지만 이 장치에 NO_D3 quirk 가 걸려 있으면 — D3 에서 돌아오지 못하는 하드웨어다 */
		return 0; /* [한국어] 요청을 무시하고 성공으로 처리한다 */

	if (state == PCI_D3cold) { /* [한국어] D3cold 는 두 단계를 밟는다 */
		/*
		 * To put the device in D3cold, put it into D3hot in the native
		 * way, then put it into D3cold using platform ops.
		 */
		error = pci_set_low_power_state(dev, PCI_D3hot, locked); /* [한국어] 먼저 장치 스스로 D3hot 까지 내려가게 한다. 결과는 보관만 해 둔다 */

		if (pci_platform_power_transition(dev, PCI_D3cold)) /* [한국어] 그 다음 플랫폼에 전원 자원을 끊게 한다. 실패하면 */
			return error; /* [한국어] 앞 단계의 결과를 돌려준다 — D3hot 까지는 갔을 수도 있다 */

		/* Powering off a bridge may power off the whole hierarchy */
		if (dev->current_state == PCI_D3cold) /* [한국어] 브리지 자신이 D3cold 에 들어갔다면 */
			__pci_bus_set_current_state(dev->subordinate, PCI_D3cold, locked); /* [한국어] 그 아래 모든 장치도 전원을 잃었다. config 로 확인할 수 없으니 캐시만 일괄로 맞춘다 */
	} else {
		error = pci_set_low_power_state(dev, state, locked); /* [한국어] 장치의 PMCSR 로 전이한다 */

		if (pci_platform_power_transition(dev, state)) /* [한국어] 플랫폼에도 같은 상태를 알린다. 실패하면 */
			return error; /* [한국어] 앞 단계의 결과를 돌려준다 */
	}

	return 0; /* [한국어] 여기까지 왔으면 목표 상태(또는 그에 준하는 상태)에 도달했다 */
}

/**
 * pci_set_power_state - Set the power state of a PCI device
 * @dev: PCI device to handle.
 * @state: PCI power state (D0, D1, D2, D3hot) to put the device into.
 *
 * Transition a device to a new power state, using the platform firmware and/or
 * the device's PCI PM registers.
 *
 * RETURN VALUE:
 * -EINVAL if the requested state is invalid.
 * -EIO if device does not support PCI PM or its PM capabilities register has a
 * wrong version, or device doesn't support the requested state.
 * 0 if the transition is to D1 or D2 but D1 and D2 are not supported.
 * 0 if device already is in the requested state.
 * 0 if the transition is to D3 but D3 is not supported.
 * 0 if device's power state has been successfully changed.
 */
/*
 * [한국어]
 * pci_set_power_state - 전원 상태를 바꾸는 공개 API(락을 잡지 않은 호출자용).
 *
 * @dev: 대상 PCI 장치.
 * @state: 목표 D-state.
 * @return: __pci_set_power_state() 의 반환값 그대로. 0 이면 성공이거나
 *          "할 필요/방법이 없어 무시"이고, 음수면 실패다.
 *
 * locked=false 로 본체에 넘긴다 — 즉 아래에서 pci_bus_sem 이 필요하면
 * 스스로 잡는다. 대부분의 드라이버가 쓰는 진입점이다.
 *
 * 참고 — drivers/nvme/ 전체를 검색해도 이 함수 호출은 없다. NVMe 장치의
 * D-state 전이는 drivers/pci/pci-driver.c 의 PM 콜백(pci_pm_suspend 계열)이
 * 대신 수행하고, NVMe 드라이버는 컨트롤러를 끄고 켜는 자기 일만 한다.
 *
 * 실행 문맥: 잠들 수 있다. 프로세스 문맥.
 *
 * 호출 체인:
 *   드라이버, pci-driver.c 의 PM 콜백 → [이 함수] → __pci_set_power_state()
 */
int pci_set_power_state(struct pci_dev *dev, pci_power_t state)
{
	return __pci_set_power_state(dev, state, false); /* [한국어] locked=false — 필요하면 하위에서 pci_bus_sem 을 직접 잡는다 */
}
EXPORT_SYMBOL(pci_set_power_state);

/*
 * [한국어]
 * pci_set_power_state_locked - pci_bus_sem 을 이미 쥔 호출자를 위한 진입점.
 *
 * @dev: 대상 PCI 장치.
 * @state: 목표 D-state.
 * @return: __pci_set_power_state() 의 반환값 그대로.
 *
 * 하는 일은 pci_set_power_state() 와 같지만 locked=true 로 넘긴다. 그래야
 * 아래쪽 버스 순회가 pci_bus_sem 을 다시 잡으려다 교착하는 일을 피한다.
 * 진입 시 lockdep 로 "정말 그 락을 쥐고 있는가"를 검사해, 잘못 쓴 호출자를
 * 조용한 교착 대신 즉시 경고로 드러낸다.
 *
 * 실행 문맥: pci_bus_sem 을 쥔 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_bus_sem 을 쥔 채 전원을 다루는 코드 → [이 함수] → __pci_set_power_state()
 */
int pci_set_power_state_locked(struct pci_dev *dev, pci_power_t state)
{
	lockdep_assert_held(&pci_bus_sem); /* [한국어] 계약 위반을 조기에 드러낸다 — 이 락 없이 들어오면 아래 순회가 교착한다 */

	return __pci_set_power_state(dev, state, true); /* [한국어] PCIe capability 에서 저장할 레지스터 개수. 아래 pci_save_pcie_state() 가 DEVCTL/LNKCTL/SLTCTL/RTCTL/DEVCTL2/LNKCTL2/SLTCTL2 일곱 개를 u16 으로 담으므로 버퍼 크기도 7 * sizeof(u16) 로 잡힌다 */
}
EXPORT_SYMBOL(pci_set_power_state_locked);

/* [한국어] PCIe capability 에서 저장/복원해야 하는 레지스터의 개수.
 * 저장 버퍼(struct pci_cap_saved_data)의 크기를 정하는 데 쓴다.
 * 7 이라는 값은 DEVCTL, LNKCTL, SLTCTL, RTCTL, DEVCTL2, LNKCTL2, SLTCTL2 —
 * 즉 "제어" 레지스터들의 수에서 온다. 상태나 능력 레지스터는 하드웨어가
 * 스스로 채우므로 복원할 필요가 없다.
 * 새 제어 레지스터가 스펙에 추가되면 이 값도 함께 늘려야 하며, 그렇지
 * 않으면 저장 버퍼가 모자라 전원 복귀 후 설정이 유실된다. */
#define PCI_EXP_SAVE_REGS	7

/*
 * [한국어]
 * _pci_find_saved_cap - 이 장치에 붙어 있는 capability 저장 버퍼를 찾는다.
 *
 * @pci_dev: 대상 PCI 장치.
 * @cap: 찾을 capability 번호.
 * @extended: false 면 표준 capability, true 면 확장 capability.
 * @return: 해당 버퍼(struct pci_cap_saved_state), 없으면 NULL.
 *
 * config space 를 통째로 뜨는 것만으로는 전원 전이를 넘길 수 없다.
 * 표준 헤더 64바이트 밖의 capability 레지스터들은 saved_config_space 에
 * 담기지 않기 때문이다. 그래서 capability 마다 따로 버퍼를 잡아
 * dev->saved_cap_space 해시 리스트에 매달아 둔다. 그 버퍼를 찾는 조회다.
 *
 * 버퍼는 pci_allocate_cap_save_buffers() 가 열거 시점에 미리 잡는다.
 * 저장 시점(pci_save_state)에 할당하지 않는 이유는, 그 경로가 메모리 할당이
 * 실패하면 안 되는 상황에서도 불릴 수 있기 때문이다.
 *
 * 표준과 확장 capability 는 번호 공간이 겹친다(둘 다 작은 정수). 그래서
 * cap_nr 만으로는 구분되지 않고 cap_extended 를 함께 비교해야 한다.
 *
 * 실행 문맥: 리스트 순회뿐이고 config 접근이 없다. 리스트는 열거 시점에
 * 구성돼 이후 바뀌지 않으므로 별도 락이 없다.
 *
 * 호출 체인:
 *   pci_find_saved_cap() / pci_find_saved_ext_cap() → [이 함수]
 */
static struct pci_cap_saved_state *_pci_find_saved_cap(struct pci_dev *pci_dev,
						       u16 cap, bool extended)
{
	struct pci_cap_saved_state *tmp; /* [한국어] 해시 리스트 순회 커서 */

	hlist_for_each_entry(tmp, &pci_dev->saved_cap_space, next) { /* [한국어] 이 장치에 매달린 저장 버퍼들을 훑는다 */
		if (tmp->cap.cap_extended == extended && tmp->cap.cap_nr == cap) /* [한국어] 번호와 "표준/확장" 구분이 둘 다 맞아야 같은 capability 다 */
			return tmp; /* [한국어] 찾은 버퍼 */
	}
	return NULL; /* [한국어] 이 capability 용 버퍼는 할당되지 않았다 */
}

/*
 * [한국어]
 * pci_find_saved_cap - 표준 capability 의 저장 버퍼를 찾는다.
 *
 * @dev: 대상 PCI 장치.
 * @cap: 표준 capability ID(PCI_CAP_ID_EXP, PCI_CAP_ID_PCIX 등).
 * @return: 저장 버퍼, 없으면 NULL.
 *
 * extended=false 로 고정한 얇은 래퍼다. 저장·복원 코드가 매번 false 를 적는
 * 대신 의도가 드러나는 이름을 쓰게 한다.
 *
 * 호출 체인:
 *   pci_save_pcie_state(), pci_restore_pcie_state(), pci_save_pcix_state() 등
 *     → [이 함수] → _pci_find_saved_cap()
 */
struct pci_cap_saved_state *pci_find_saved_cap(struct pci_dev *dev, char cap)
{
	return _pci_find_saved_cap(dev, cap, false); /* [한국어] extended=false — 표준 capability 공간에서 찾는다 */
}

/*
 * [한국어]
 * pci_find_saved_ext_cap - 확장 capability 의 저장 버퍼를 찾는다.
 *
 * @dev: 대상 PCI 장치.
 * @cap: 확장 capability ID(PCI_EXT_CAP_ID_ERR, _DPC, _PTM 등).
 * @return: 저장 버퍼, 없으면 NULL.
 *
 * extended=true 로 고정한 래퍼다. AER/DPC/PTM/VC 처럼 0x100 이후에 있는
 * capability 의 상태를 저장·복원하는 코드가 쓴다.
 *
 * 호출 체인:
 *   pci_save_aer_state(), pci_save_dpc_state() 등(다른 파일) → [이 함수]
 *     → _pci_find_saved_cap()
 */
struct pci_cap_saved_state *pci_find_saved_ext_cap(struct pci_dev *dev, u16 cap)
{
	return _pci_find_saved_cap(dev, cap, true); /* [한국어] extended=true — 확장 capability 공간에서 찾는다 */
}

/*
 * [한국어]
 * pci_save_pcie_state - PCIe capability 의 제어 레지스터 일곱 개를 떠 둔다.
 *
 * @dev: 대상 PCI 장치.
 * @return: 0 성공(또는 PCIe 장치가 아니라 할 일 없음), -ENOMEM 이면 저장 버퍼가 없다.
 *
 * saved_config_space 는 config space 앞쪽 64바이트만 담는다. PCIe capability 는
 * 그보다 뒤에 있어 별도로 떠야 한다. 여기서 저장하는 레지스터들이 잃으면
 * 곤란한 이유는 각각 분명하다.
 *   - DEVCTL: Max Payload Size, Max Read Request Size, 오류 보고 활성화.
 *     MPS/MRRS 를 잃으면 링크 성능이 기본값으로 떨어지고, 경로상 불일치가
 *     생기면 오동작한다. NVMe 의 대역폭에 직접 영향을 준다.
 *   - LNKCTL: ASPM 정책, Common Clock 설정.
 *   - SLTCTL / SLTCTL2: 슬롯 제어(핫플러그 표시등, 전원 제어).
 *   - RTCTL: Root Port 의 오류 보고 제어.
 *   - DEVCTL2: LTR 활성화, Completion Timeout 설정, ARI Forwarding.
 *   - LNKCTL2: 목표 링크 속도.
 * ASPM L1 substate 와 LTR 은 별도 확장 capability 라 전용 함수가 따로 뜬다.
 *
 * 저장 버퍼가 없으면 -ENOMEM 을 돌려준다. 실제로 할당에 실패했다기보다
 * pci_allocate_cap_save_buffers() 가 미리 잡아 두지 못한 상황이므로,
 * 이 시점에는 되돌릴 방법이 없어 오류로 알린다.
 *
 * 실행 문맥: config space 읽기. 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_save_state() → [이 함수] → pcie_capability_read_word()
 */
static int pci_save_pcie_state(struct pci_dev *dev)
{
	int i = 0; /* [한국어] 버퍼에 채워 넣을 위치. 아래에서 i++ 로 하나씩 전진한다 — 복원 함수가 같은 순서로 읽으므로 순서 자체가 형식이다 */
	struct pci_cap_saved_state *save_state; /* [한국어] PCIe capability 전용 저장 버퍼 */
	u16 *cap; /* [한국어] 그 버퍼를 u16 배열로 보는 포인터 */

	if (!pci_is_pcie(dev)) /* [한국어] PCIe capability 가 없는 재래식 PCI 장치면 */
		return 0; /* [한국어] 저장할 것이 없다 */

	save_state = pci_find_saved_cap(dev, PCI_CAP_ID_EXP); /* [한국어] PCI_CAP_ID_EXP 용 버퍼를 찾는다 */
	if (!save_state) { /* [한국어] 열거 때 미리 잡혀 있어야 한다 */
		pci_err(dev, "buffer not found in %s\n", __func__); /* [한국어] 버퍼가 없다는 것은 초기화 단계에서 이미 문제가 있었다는 뜻이다 */
		return -ENOMEM; /* [한국어] 저장에 실패했음을 알린다 */
	}

	cap = (u16 *)&save_state->cap.data[0]; /* [한국어] 버퍼의 데이터 영역을 u16 배열로 다룬다. 이 capability 의 레지스터가 모두 16비트이기 때문이다 */
	pcie_capability_read_word(dev, PCI_EXP_DEVCTL, &cap[i++]); /* [한국어] Device Control — MPS, MRRS, 오류 보고 활성화 비트가 들어 있다 */
	pcie_capability_read_word(dev, PCI_EXP_LNKCTL, &cap[i++]); /* [한국어] Link Control — ASPM 정책과 Common Clock 설정 */
	pcie_capability_read_word(dev, PCI_EXP_SLTCTL, &cap[i++]); /* [한국어] Slot Control — 슬롯 표시등·전원·핫플러그 인터럽트 제어 */
	pcie_capability_read_word(dev, PCI_EXP_RTCTL,  &cap[i++]); /* [한국어] Root Control — Root Port 가 어떤 오류 메시지에 인터럽트를 낼지 */
	pcie_capability_read_word(dev, PCI_EXP_DEVCTL2, &cap[i++]); /* [한국어] Device Control 2 — LTR 활성화, Completion Timeout, ARI Forwarding */
	pcie_capability_read_word(dev, PCI_EXP_LNKCTL2, &cap[i++]); /* [한국어] Link Control 2 — 목표 링크 속도(Gen1~Gen5 등) */
	pcie_capability_read_word(dev, PCI_EXP_SLTCTL2, &cap[i++]); /* [한국어] Slot Control 2 */

	pci_save_aspm_l1ss_state(dev); /* [한국어] ASPM L1 substate 설정은 별도 확장 capability 라 따로 뜬다 */
	pci_save_ltr_state(dev); /* [한국어] LTR(Latency Tolerance Reporting)의 최대 지연 값도 별도 capability 다 */

	return 0; /* [한국어] 저장 완료 */
}

/*
 * [한국어]
 * pci_restore_pcie_state - 떠 두었던 PCIe capability 레지스터를 되돌린다.
 *
 * @dev: 대상 PCI 장치.
 * @return: 없음. 저장 버퍼가 없으면 조용히 아무 것도 하지 않는다.
 *
 * 순서가 이 함수의 전부라 해도 좋다.
 *   1) LTR 값을 먼저 복원한다. DEVCTL2 의 LTR Enable 비트를 켜기 전에
 *      최대 지연 값이 제자리에 있어야 한다 — 원본 주석이 짚는 요점이다.
 *      순서가 반대면 아직 쓰레기 값인 지연 한도로 LTR 이 동작한다.
 *   2) ASPM L1 substate 를 복원한다.
 *   3) 다운스트림 포트는 링크가 끊길 때 LTR Enable 비트를 스스로 지운다
 *      (PCIe r5.0 7.5.3.16). 그래서 장치 레지스터를 복원하기 전에 상위
 *      브리지의 LTR 을 먼저 되살려야 한다.
 *   4) 그제서야 저장 순서 그대로 일곱 레지스터를 되쓴다.
 *
 * 저장 순서와 복원 순서가 정확히 같아야 한다는 점이 중요하다. 버퍼에는
 * 레지스터 이름이 아니라 값만 들어 있어, 순서가 어긋나면 엉뚱한 레지스터에
 * 값을 쓴다.
 *
 * 실행 문맥: config space 쓰기. 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_restore_state() → [이 함수]
 *     → pci_restore_ltr_state(), pci_bridge_reconfigure_ltr(),
 *        pcie_capability_write_word()
 */
static void pci_restore_pcie_state(struct pci_dev *dev)
{
	int i = 0; /* [한국어] 버퍼에서 꺼낼 위치. 저장 때와 같은 순서로 전진한다 */
	struct pci_cap_saved_state *save_state; /* [한국어] PCIe capability 저장 버퍼 */
	u16 *cap; /* [한국어] 그 버퍼를 u16 배열로 보는 포인터 */

	/*
	 * Restore max latencies (in the LTR capability) before enabling
	 * LTR itself in PCI_EXP_DEVCTL2.
	 */
	pci_restore_ltr_state(dev); /* [한국어] LTR 최대 지연 값을 먼저 되돌린다. 아래에서 DEVCTL2 의 LTR Enable 을 켜기 전이어야 한다 */
	pci_restore_aspm_l1ss_state(dev); /* [한국어] ASPM L1 substate 설정 복원 */

	save_state = pci_find_saved_cap(dev, PCI_CAP_ID_EXP); /* [한국어] PCIe capability 저장 버퍼를 찾는다 */
	if (!save_state) /* [한국어] 저장된 적이 없으면 */
		return; /* [한국어] 되돌릴 것이 없다 */

	/*
	 * Downstream ports reset the LTR enable bit when link goes down.
	 * Check and re-configure the bit here before restoring device.
	 * PCIe r5.0, sec 7.5.3.16.
	 */
	pci_bridge_reconfigure_ltr(dev); /* [한국어] 상위 다운스트림 포트가 링크 다운 때 스스로 지운 LTR Enable 을 되살린다. 장치 쪽을 복원하기 전에 해야 한다 */

	cap = (u16 *)&save_state->cap.data[0]; /* [한국어] 저장 버퍼의 데이터 영역 */
	pcie_capability_write_word(dev, PCI_EXP_DEVCTL, cap[i++]); /* [한국어] Device Control 복원 — MPS/MRRS 가 여기서 되살아난다 */
	pcie_capability_write_word(dev, PCI_EXP_LNKCTL, cap[i++]); /* [한국어] Link Control 복원 */
	pcie_capability_write_word(dev, PCI_EXP_SLTCTL, cap[i++]); /* [한국어] Slot Control 복원 */
	pcie_capability_write_word(dev, PCI_EXP_RTCTL, cap[i++]); /* [한국어] Root Control 복원 */
	pcie_capability_write_word(dev, PCI_EXP_DEVCTL2, cap[i++]); /* [한국어] Device Control 2 복원 — LTR Enable 이 여기서 켜진다 */
	pcie_capability_write_word(dev, PCI_EXP_LNKCTL2, cap[i++]); /* [한국어] Link Control 2 복원 */
	pcie_capability_write_word(dev, PCI_EXP_SLTCTL2, cap[i++]); /* [한국어] Slot Control 2 복원 */
}

/*
 * [한국어]
 * pci_save_pcix_state - PCI-X capability 의 Command 레지스터를 떠 둔다.
 *
 * @dev: 대상 PCI 장치.
 * @return: 0 성공(또는 PCI-X 가 아니라 할 일 없음), -ENOMEM 이면 저장 버퍼가 없다.
 *
 * PCI-X 는 PCIe 이전의 고속 병렬 버스 규격이다. Command 레지스터에
 * 분할 트랜잭션 개수와 최대 읽기 바이트 수 설정이 들어 있어, 잃으면
 * 성능이 기본값으로 떨어진다.
 * PCIe 엔드포인트인 NVMe SSD 에는 PCI-X capability 가 없으므로 이 함수는
 * 곧바로 0 을 돌려주고 끝난다. 오래된 하드웨어를 위해 남아 있는 경로다.
 *
 * 실행 문맥: config space 읽기. 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_save_state() → [이 함수] → pci_find_capability(), pci_read_config_word()
 */
static int pci_save_pcix_state(struct pci_dev *dev)
{
	int pos; /* [한국어] PCI-X capability 의 오프셋 */
	struct pci_cap_saved_state *save_state; /* [한국어] 저장 버퍼 */

	pos = pci_find_capability(dev, PCI_CAP_ID_PCIX); /* [한국어] 이 장치에 PCI-X capability 가 있는지 본다 */
	if (!pos) /* [한국어] 없으면(PCIe 장치는 모두 여기 해당) */
		return 0; /* [한국어] 저장할 것이 없다 */

	save_state = pci_find_saved_cap(dev, PCI_CAP_ID_PCIX); /* [한국어] PCI-X 용 저장 버퍼를 찾는다 */
	if (!save_state) { /* [한국어] capability 는 있는데 버퍼가 없다면 초기화 단계의 문제다 */
		pci_err(dev, "buffer not found in %s\n", __func__); /* [한국어] 어느 함수에서 났는지 __func__ 로 남긴다 */
		return -ENOMEM; /* [한국어] 저장 실패 */
	}

	pci_read_config_word(dev, pos + PCI_X_CMD, /* [한국어] PCI-X Command 레지스터를 읽어 */
			     (u16 *)save_state->cap.data); /* [한국어] 저장 버퍼에 그대로 담는다. 레지스터가 하나뿐이라 인덱스가 필요 없다 */

	return 0; /* [한국어] 저장 완료 */
}

/*
 * [한국어]
 * pci_restore_pcix_state - PCI-X Command 레지스터를 되돌린다.
 *
 * @dev: 대상 PCI 장치.
 * @return: 없음.
 *
 * pci_save_pcix_state() 의 짝이다. 저장 버퍼와 capability 오프셋이 둘 다
 * 있어야 복원할 수 있다. capability 오프셋을 다시 찾는 이유는, 오프셋 자체는
 * 저장해 두지 않고 필요할 때마다 config space 를 훑기 때문이다.
 * PCIe 엔드포인트인 NVMe SSD 에는 해당 사항이 없다.
 *
 * 실행 문맥: config space 쓰기. 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_restore_state() → [이 함수] → pci_write_config_word()
 */
static void pci_restore_pcix_state(struct pci_dev *dev)
{
	int i = 0, pos; /* [한국어] i 는 버퍼 인덱스, pos 는 capability 오프셋 */
	struct pci_cap_saved_state *save_state; /* [한국어] 저장 버퍼 */
	u16 *cap; /* [한국어] 버퍼를 u16 배열로 보는 포인터 */

	save_state = pci_find_saved_cap(dev, PCI_CAP_ID_PCIX); /* [한국어] PCI-X 저장 버퍼를 찾는다 */
	pos = pci_find_capability(dev, PCI_CAP_ID_PCIX); /* [한국어] capability 오프셋을 다시 구한다 — 오프셋은 저장해 두지 않는다 */
	if (!save_state || !pos) /* [한국어] 둘 중 하나라도 없으면 */
		return; /* [한국어] 복원할 수 없다 */
	cap = (u16 *)&save_state->cap.data[0]; /* [한국어] 버퍼의 데이터 영역 */

	pci_write_config_word(dev, pos + PCI_X_CMD, cap[i++]); /* [한국어] Command 레지스터를 되쓴다. 레지스터가 하나뿐이지만 저장 쪽과 형태를 맞추려고 i++ 를 쓴다 */
}

/**
 * pci_save_state - save the PCI configuration space of a device before
 *		    suspending
 * @dev: PCI device that we're dealing with
 */
/*
 * [한국어]
 * pci_save_state - config space 앞 64바이트와 capability 상태를 통째로 떠 둔다.
 *
 * @dev: 대상 PCI 장치.
 * @return: 0 성공. 음수면 어느 capability 저장 단계에서 실패했다.
 *
 * 이 함수가 필요한 이유는 하나다 — D3cold 전이나 리셋(FLR/보조 버스 리셋)을
 * 거치면 장치의 config space 가 초기값으로 돌아간다. BAR 는 모두 0 이 되고
 * Command 레지스터의 MEM/IO 디코딩과 Bus Master Enable 도 꺼진다. 그 상태의
 * 장치는 CPU 가 닿을 주소도 없고 DMA 도 못 한다. 즉 시스템에서 사라진 것과
 * 같다. 그래서 잃기 전에 떠 두었다가 pci_restore_state() 로 되돌린다.
 *
 * 뜨는 대상은 두 갈래다.
 *   1) 표준 헤더 64바이트 = DWORD 16개. dev->saved_config_space[] 에 담는다.
 *      여기에 Vendor/Device ID, Command, Status, BAR 6개, 인터럽트 라인 등
 *      장치를 다시 찾고 켜는 데 필요한 모든 것이 들어 있다.
 *   2) 그 밖의 capability 들. 각각 전용 버퍼에 따로 뜬다 — PCIe, PCI-X, DPC,
 *      AER, PTM, TPH, VC.
 *
 * MSI/MSI-X 는 여기서 저장하지 않는다. 복원은 pci_restore_state() 안의
 * pci_restore_msi_state() 가 하는데, 그 값의 출처는 config space 사본이
 * 아니라 커널이 들고 있는 인터럽트 기술자다.
 *
 * NVMe 와의 관계: drivers/nvme/host/pci.c 가 이 함수를 직접 부른다.
 * 컨트롤러 리셋과 절전 진입 직전에 상태를 떠 두고, 돌아온 뒤
 * pci_restore_state() 로 되돌린다. 이것이 없으면 리셋 후 BAR0 가 0 이 되어
 * 도어벨과 컨트롤러 레지스터에 접근할 수 없다.
 *
 * 실행 문맥: config space 를 여러 번 읽는다. 프로세스 문맥.
 *
 * 호출 체인:
 *   drivers/nvme/host/pci.c, pci_dev_save_and_disable(), pci-driver.c 의 PM 콜백
 *     → [이 함수] → pci_read_config_dword(), pci_save_pcie_state(),
 *        pci_save_pcix_state(), pci_save_dpc/aer/ptm/tph/vc_state()
 */
int pci_save_state(struct pci_dev *dev)
{
	int i; /* [한국어] DWORD 인덱스 겸, 아래에서는 하위 저장 함수의 반환값을 담는 임시로도 쓰인다 */
	/* XXX: 100% dword access ok here? */
	for (i = 0; i < 16; i++) { /* [한국어] DWORD 16개 = 64바이트. PCI 표준 헤더의 크기이자 헤더 타입에 상관없이 존재하는 영역이다 */
		pci_read_config_dword(dev, i * 4, &dev->saved_config_space[i]); /* [한국어] i 번째 DWORD 는 오프셋 i*4 다. 32비트 단위로 읽는 이유는 BAR 처럼 32비트로 다뤄야 하는 필드가 있기 때문이다 */
		pci_dbg(dev, "save config %#04x: %#010x\n", /* [한국어] 무엇을 떴는지 디버그 로그로 남긴다. 복원이 어긋났을 때 이 로그와 대조한다 */
			i * 4, dev->saved_config_space[i]); /* [한국어] 오프셋과 값 */
	}
	dev->state_saved = true; /* [한국어] 복원해도 되는 사본이 있다는 표시. pci_restore_state() 가 이 플래그를 보고 복원 여부를 정하고, 복원 후 다시 내린다 */

	i = pci_save_pcie_state(dev); /* [한국어] PCIe capability 의 제어 레지스터들을 뜬다 */
	if (i != 0) /* [한국어] 저장 버퍼가 없어 실패하면 */
		return i; /* [한국어] 그 오류를 그대로 올린다. 이후 capability 는 시도하지 않는다 */

	i = pci_save_pcix_state(dev); /* [한국어] PCI-X capability 를 뜬다(PCIe 장치에는 없어 곧바로 0) */
	if (i != 0) /* [한국어] 실패하면 */
		return i; /* [한국어] 오류를 올린다 */

	pci_save_dpc_state(dev); /* [한국어] DPC(Downstream Port Containment) — 오류 발생 시 하위 포트를 격리하는 기능의 설정 */
	pci_save_aer_state(dev); /* [한국어] AER — 어떤 오류를 보고할지 정한 마스크와 심각도 설정 */
	pci_save_ptm_state(dev); /* [한국어] PTM(Precision Time Measurement) — 장치와 호스트의 시각 동기 설정 */
	pci_save_tph_state(dev); /* [한국어] TPH(TLP Processing Hints) — 어느 캐시에 데이터를 놓을지 힌트를 주는 기능의 설정 */
	return pci_save_vc_state(dev); /* [한국어] VC(Virtual Channel) — 트래픽 클래스와 가상 채널 매핑. 마지막이라 그 결과가 곧 이 함수의 결과다 */
}
EXPORT_SYMBOL(pci_save_state);

/*
 * [한국어]
 * pci_restore_config_dword - config space DWORD 하나를 되쓰되, 필요하면 재시도한다.
 *
 * @pdev: 대상 PCI 장치.
 * @offset: config space 오프셋(바이트).
 * @saved_val: 되돌릴 값.
 * @retry: 값이 곧바로 반영되지 않을 때 다시 시도할 횟수.
 * @force: true 면 현재 값이 이미 같아도 무조건 쓴다.
 * @return: 없음.
 *
 * 단순한 쓰기가 아니라 세 가지 배려가 들어 있다.
 *   - 이미 값이 같으면 쓰지 않는다. 불필요한 config 쓰기는 느릴 뿐 아니라
 *     일부 레지스터에서는 부작용이 있다.
 *   - force 는 그 최적화를 끄는 스위치다. 값이 같아 보여도 반드시 다시 써야
 *     하는 레지스터가 있어(아래 브리지 prefetch 창) 예외를 열어 둔다.
 *   - retry 는 BAR 처럼 "썼는데 곧바로 반영되지 않는" 경우를 위한 것이다.
 *     1ms 씩 쉬며 되읽어 확인하고, 횟수를 다 쓰면 마지막 쓰기만 하고 포기한다.
 *
 * 실행 문맥: mdelay 로 바쁜 대기를 한다. retry 가 0 이 아닌 호출은 프로세스
 * 문맥에서만 하는 것이 안전하다.
 *
 * 호출 체인:
 *   pci_restore_config_space_range() → [이 함수]
 *     → pci_read_config_dword(), pci_write_config_dword(), mdelay()
 */
static void pci_restore_config_dword(struct pci_dev *pdev, int offset,
				     u32 saved_val, int retry, bool force)
{
	u32 val; /* [한국어] 현재 레지스터 값 */

	pci_read_config_dword(pdev, offset, &val); /* [한국어] 되쓰기 전에 지금 값을 읽는다 */
	if (!force && val == saved_val) /* [한국어] 강제가 아니고 이미 같은 값이면 */
		return; /* [한국어] 쓸 이유가 없다 */

	for (;;) { /* [한국어] 반영될 때까지, 또는 재시도 횟수를 다 쓸 때까지 */
		pci_dbg(pdev, "restore config %#04x: %#010x -> %#010x\n", /* [한국어] 무엇을 무엇으로 바꾸는지 남긴다 */
			offset, val, saved_val); /* [한국어] 오프셋, 현재 값, 되돌릴 값 */
		pci_write_config_dword(pdev, offset, saved_val); /* [한국어] 저장해 둔 값을 써 넣는다 */
		if (retry-- <= 0) /* [한국어] 재시도 횟수를 소진했으면 확인 없이 끝낸다 — 이미 쓰기는 했다 */
			return; /* [한국어] 끝 */

		pci_read_config_dword(pdev, offset, &val); /* [한국어] 정말 반영됐는지 되읽어 확인한다 */
		if (val == saved_val) /* [한국어] 값이 맞으면 */
			return; /* [한국어] 끝 */

		mdelay(1); /* [한국어] 하드웨어가 반영할 시간을 준다. udelay 가 아닌 mdelay 라 바쁜 대기 1ms 다 */
	}
}

/*
 * [한국어]
 * pci_restore_config_space_range - config space 의 DWORD 구간을 뒤에서 앞으로 되돌린다.
 *
 * @pdev: 대상 PCI 장치.
 * @start: 시작 DWORD 인덱스(포함).
 * @end: 끝 DWORD 인덱스(포함).
 * @retry: 각 DWORD 에 적용할 재시도 횟수.
 * @force: 값이 같아도 강제로 쓸지.
 * @return: 없음.
 *
 * 인덱스가 큰 쪽에서 작은 쪽으로 내려가며 쓴다. 이 방향이 곧 복원 순서
 * 정책의 도구다 — 호출자(pci_restore_config_space)가 구간을 나누어 부르면서
 * "BAR 를 Command 보다 먼저" 같은 순서를 만들어 낸다.
 * 인덱스 i 의 config 오프셋은 4 * i 다.
 *
 * 실행 문맥: 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_restore_config_space() → [이 함수] → pci_restore_config_dword()
 */
static void pci_restore_config_space_range(struct pci_dev *pdev,
					   int start, int end, int retry,
					   bool force)
{
	int index; /* [한국어] DWORD 인덱스 */

	for (index = end; index >= start; index--) /* [한국어] 큰 인덱스에서 작은 인덱스로 내려간다 */
		pci_restore_config_dword(pdev, 4 * index, /* [한국어] 인덱스를 바이트 오프셋으로 바꿔 넘긴다 */
					 pdev->saved_config_space[index], /* [한국어] 그 자리에 되돌릴 값 */
					 retry, force); /* [한국어] 재시도 횟수와 강제 여부는 그대로 전달한다 */
}

/*
 * [한국어]
 * pci_restore_config_space - 헤더 타입에 맞는 순서로 표준 헤더 64바이트를 되돌린다.
 *
 * @pdev: 대상 PCI 장치.
 * @return: 없음.
 *
 * 이 함수의 핵심은 "무엇을 쓰느냐"가 아니라 "어떤 순서로 쓰느냐"다.
 *
 * 헤더 타입 0(일반 장치, NVMe SSD 가 여기 해당):
 *   DWORD 10~15 → 4~9 → 0~3 순으로 되돌린다.
 *   4~9 가 BAR 6개이고, 0~3 안에 Command 레지스터(오프셋 0x04)가 있다.
 *   원본 주석이 짚듯 BAR 를 Command 보다 먼저 써야 한다. 순서가 반대라면
 *   BAR 가 아직 0 인 상태에서 MEM 디코딩이 켜지고, 장치가 주소 0 부근을
 *   자기 것이라 주장하는 위험한 창이 열린다.
 *   BAR 구간에만 retry=10 을 주는 이유는, BAR 쓰기가 곧바로 반영되지 않는
 *   하드웨어가 있어 되읽어 확인하며 최대 10번까지 다시 쓰기 위해서다.
 *
 * 헤더 타입 1(브리지):
 *   DWORD 12~15 → 9~11 → 0~8 순이다. 9~11 에 force=true 를 주는 것이 특징인데,
 *   원본 주석대로 일부 Intel 브리지가 prefetchable 창 레지스터를 명시적으로
 *   다시 쓰지 않으면 S3 복귀 후 오동작하기 때문이다. 값이 같아 보여도
 *   반드시 쓰게 하는 quirk 성 처리다.
 *
 * 그 밖의 헤더 타입은 순서를 따질 근거가 없어 0~15 를 한 번에 되돌린다.
 *
 * 실행 문맥: 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_restore_state() → [이 함수] → pci_restore_config_space_range()
 */
static void pci_restore_config_space(struct pci_dev *pdev)
{
	if (pdev->hdr_type == PCI_HEADER_TYPE_NORMAL) { /* [한국어] 헤더 타입 0 — 일반 엔드포인트. NVMe SSD 가 여기 해당한다 */
		pci_restore_config_space_range(pdev, 10, 15, 0, false); /* [한국어] DWORD 10~15(오프셋 0x28~0x3f) — Subsystem ID, Expansion ROM, Capabilities Pointer, 인터럽트 라인/핀 등 */
		/* Restore BARs before the command register. */
		pci_restore_config_space_range(pdev, 4, 9, 10, false); /* [한국어] DWORD 4~9 = BAR 0~5(오프셋 0x10~0x27). retry=10 은 쓰기가 곧바로 반영되지 않는 하드웨어를 위한 확인 재시도다 */
		pci_restore_config_space_range(pdev, 0, 3, 0, false); /* [한국어] DWORD 0~3(오프셋 0x00~0x0f) — 여기에 Command 레지스터가 있다. BAR 가 모두 제자리에 들어간 뒤에야 디코딩을 켠다 */
	} else if (pdev->hdr_type == PCI_HEADER_TYPE_BRIDGE) { /* [한국어] 헤더 타입 1 — PCI-to-PCI 브리지 */
		pci_restore_config_space_range(pdev, 12, 15, 0, false); /* [한국어] DWORD 12~15(오프셋 0x30~0x3f) */

		/*
		 * Force rewriting of prefetch registers to avoid S3 resume
		 * issues on Intel PCI bridges that occur when these
		 * registers are not explicitly written.
		 */
		pci_restore_config_space_range(pdev, 9, 11, 0, true); /* [한국어] DWORD 9~11 — prefetchable 메모리 창의 상위 32비트 레지스터들. force=true 로 값이 같아도 반드시 다시 쓴다 */
		pci_restore_config_space_range(pdev, 0, 8, 0, false); /* [한국어] DWORD 0~8 — Command 와 나머지 브리지 창 설정 */
	} else {
		pci_restore_config_space_range(pdev, 0, 15, 0, false); /* [한국어] 순서를 따질 근거가 없어 전 구간을 한 번에 되돌린다 */
	}
}

/**
 * pci_restore_state - Restore the saved state of a PCI device
 * @dev: PCI device that we're dealing with
 */
/*
 * [한국어]
 * pci_restore_state - pci_save_state() 로 떠 둔 상태를 장치에 되돌린다.
 *
 * @dev: 대상 PCI 장치.
 * @return: 없음.
 *
 * 이 함수는 dev->state_saved 를 검사하지 않는다. 사본이 유효한지 판단하는 일은
 * 호출자 몫이며(pci-driver.c 의 PM 콜백이 그 플래그를 보고 부를지 정한다),
 * 여기서는 복원을 마친 뒤 플래그를 내리기만 한다.
 *
 * D3cold 나 리셋을 거치면 config space 가 초기값으로 돌아간다. 이 함수가
 * 그것을 되돌려 장치를 "다시 찾을 수 있고 다시 쓸 수 있는" 상태로 만든다.
 * BAR 가 복원되어야 CPU 가 레지스터에 닿고, Command 의 Bus Master Enable 이
 * 복원되어야 장치가 DMA 를 다시 시작할 수 있다.
 *
 * 복원 순서에 뚜렷한 이유가 있다.
 *   1) PCIe capability 를 먼저 되돌린다 — MPS/MRRS 같은 링크 파라미터는
 *      트래픽이 흐르기 전에 제자리에 있어야 한다.
 *   2) PASID → PRI → ATS 순서로 되돌린다. 이 셋은 의존 관계가 있어
 *      순서를 지켜야 한다.
 *   3) AER 상태를 지우고 나서 AER 설정을 복원한다. 순서가 반대면 복원 직후
 *      리셋 과정에서 쌓인 오류 비트가 그대로 남아 곧바로 오류로 보고된다.
 *   4) 그 다음에야 표준 헤더(BAR·Command 등)를 되돌린다.
 *   5) 마지막으로 MSI/MSI-X 를 복원한다. 인터럽트는 장치가 다시 동작할 수
 *      있게 된 뒤에 되살리는 것이 안전하다.
 *
 * NVMe 와의 관계: drivers/nvme/host/pci.c 가 리셋과 재초기화 경로에서
 * 직접 부른다. FLR 뒤에 이 호출이 빠지면 BAR0 가 0 이 되어 컨트롤러
 * 레지스터와 도어벨에 접근할 수 없고, Bus Master Enable 도 꺼진 채라
 * SQ/CQ 를 읽지 못한다.
 *
 * 실행 문맥: config space 를 많이 쓴다. 프로세스 문맥.
 *
 * 호출 체인:
 *   drivers/nvme/host/pci.c, pci_dev_restore(), pci-driver.c 의 resume
 *     → [이 함수] → pci_restore_pcie_state(), pci_restore_config_space(),
 *        pci_restore_msi_state(), pci_enable_acs(), pci_restore_iov_state()
 */
void pci_restore_state(struct pci_dev *dev)
{
	pci_restore_pcie_state(dev); /* [한국어] 링크 파라미터(MPS/MRRS)와 ASPM/LTR 을 먼저 되돌린다 — 트래픽이 흐르기 전이어야 한다 */
	pci_restore_pasid_state(dev); /* [한국어] PASID(Process Address Space ID) 복원. 아래 PRI/ATS 가 이 설정에 의존한다 */
	pci_restore_pri_state(dev); /* [한국어] PRI(Page Request Interface) 복원 — 장치가 페이지 폴트를 요청하는 기능 */
	pci_restore_ats_state(dev); /* [한국어] ATS 복원 — 주소 변환 캐시 기능. PASID/PRI 뒤에 와야 한다 */
	pci_restore_vc_state(dev); /* [한국어] Virtual Channel 복원 — 트래픽 클래스 매핑 */
	pci_restore_rebar_state(dev); /* [한국어] Resizable BAR 복원. BAR 크기 자체를 되돌리는 것이라 아래 BAR 주소 복원보다 먼저다 */
	pci_restore_dpc_state(dev); /* [한국어] DPC 복원 */
	pci_restore_ptm_state(dev); /* [한국어] PTM 복원 */
	pci_restore_tph_state(dev); /* [한국어] TPH 복원 */

	pci_aer_clear_status(dev); /* [한국어] 리셋 과정에서 쌓인 오류 상태 비트를 먼저 지운다 */
	pci_restore_aer_state(dev); /* [한국어] 그 다음 AER 설정(마스크·심각도)을 되돌린다. 순서가 반대면 복원 직후 묵은 오류가 보고된다 */

	pci_restore_config_space(dev); /* [한국어] 표준 헤더 64바이트를 되돌린다 — BAR 와 Command 가 여기서 살아난다 */

	pci_restore_pcix_state(dev); /* [한국어] PCI-X Command 복원(PCIe 장치에는 해당 없음) */
	pci_restore_msi_state(dev); /* [한국어] MSI/MSI-X 복원. 장치가 다시 동작 가능해진 뒤에 인터럽트를 되살린다 */

	/* Restore ACS and IOV configuration state */
	pci_enable_acs(dev); /* [한국어] ACS 설정을 다시 계산해 적용한다 — 리셋으로 지워졌을 격리 설정을 되살린다 */
	pci_restore_iov_state(dev); /* [한국어] SR-IOV 설정(VF 개수 등)을 복원한다 */

	dev->state_saved = false; /* [한국어] 사본을 다 썼다는 표시. 다시 복원하려면 pci_save_state() 를 새로 불러야 한다 */
}
EXPORT_SYMBOL(pci_restore_state);

/*
 * [한국어] 장치 밖으로 꺼내 들고 다닐 수 있는 "상태 사본" 한 벌.
 *
 * pci_dev 안의 saved_config_space + saved_cap_space 리스트는 장치에 매여 있어
 * 다음 pci_save_state() 가 덮어써 버린다. 그와 달리 이 구조체는 kzalloc 으로
 * 잡은 독립 메모리 한 덩어리라, 원하는 시점까지 보관했다가 되돌릴 수 있다.
 * 소유권은 pci_store_saved_state() 를 부른 쪽에 있고 kfree 로 해제한다.
 *
 * 메모리 배치가 특이하다. 고정 길이 config_space 뒤에, 가변 길이 capability
 * 사본들이 연달아 이어 붙는다. 각 capability 사본은 헤더 + 데이터라 크기가
 * 제각각이므로 배열 인덱싱이 불가능하고, 바이트 단위 포인터 산술로 훑는다.
 * 마지막에는 size 가 0 인 빈 항목이 끝 표시로 놓인다(원본 주석의
 * "Empty cap_save terminates list").
 */
struct pci_saved_state {
	u32 config_space[16]; /* [한국어] 표준 헤더 64바이트의 사본. 설정자: pci_store_saved_state(). 읽는 자: pci_load_saved_state(). 값 범위: pci_dev->saved_config_space 를 그대로 복사한 값. 동기화: 이 구조체는 호출자 소유라 공유되지 않는다 */
	struct pci_cap_saved_data cap[]; /* [한국어] 가변 길이 배열 — 뒤에 이어 붙은 capability 사본들의 시작점. 각 항목의 실제 크기가 달라 (u8 *) 로 캐스팅해 바이트 단위로 전진하며 읽는다. size 가 0 인 항목이 끝 표시다 */
};

/**
 * pci_store_saved_state - Allocate and return an opaque struct containing
 *			   the device saved state.
 * @dev: PCI device that we're dealing with
 *
 * Return NULL if no state or error.
 */
/*
 * [한국어]
 * pci_store_saved_state - 장치에 붙어 있는 저장 상태를 독립 메모리로 복사해 낸다.
 *
 * @dev: 대상 PCI 장치.
 * @return: 새로 할당한 상태 사본. 저장된 상태가 없거나 할당에 실패하면 NULL.
 *          호출자가 kfree 로 해제해야 한다.
 *
 * pci_save_state() 가 뜬 사본은 pci_dev 안에 있어 다음 저장이 덮어쓴다.
 * "지금 이 상태를 나중에 되돌리고 싶다"면 장치 밖으로 빼내야 한다.
 * VFIO 처럼 장치를 사용자 공간에 넘겼다 되찾는 코드가 이 짝을 쓴다.
 *
 * 크기 계산이 두 번에 걸친다. 먼저 고정 부분과 끝 표시용 빈 항목의 크기를
 * 잡고, capability 사본을 하나씩 훑으며 그 크기를 더한다. 그 다음 한 번에
 * 할당하고 같은 순서로 채운다. kzalloc 으로 잡으므로 마지막 빈 항목의
 * size 는 0 으로 초기화돼 끝 표시가 저절로 만들어진다.
 *
 * 실행 문맥: GFP_KERNEL 로 할당한다. 반드시 프로세스 문맥.
 *
 * 호출 체인:
 *   VFIO 등 장치 상태를 보관해야 하는 코드 → [이 함수] → kzalloc(), memcpy()
 */
struct pci_saved_state *pci_store_saved_state(struct pci_dev *dev)
{
	struct pci_saved_state *state; /* [한국어] 새로 만들 사본 */
	struct pci_cap_saved_state *tmp; /* [한국어] 장치에 매달린 capability 저장 버퍼를 훑는 커서 */
	struct pci_cap_saved_data *cap; /* [한국어] 사본 안에서 다음 capability 를 쓸 위치 */
	size_t size; /* [한국어] 필요한 총 바이트 수 */

	if (!dev->state_saved) /* [한국어] pci_save_state() 를 부른 적이 없으면 복사할 것이 없다 */
		return NULL; /* [한국어] NULL 로 "상태 없음"을 알린다 */

	size = sizeof(*state) + sizeof(struct pci_cap_saved_data); /* [한국어] 고정 부분 + 끝 표시용 빈 항목 하나. 이 빈 항목이 있어야 읽는 쪽이 리스트의 끝을 안다 */

	hlist_for_each_entry(tmp, &dev->saved_cap_space, next) /* [한국어] 장치에 매달린 capability 버퍼를 모두 훑으며 */
		size += sizeof(struct pci_cap_saved_data) + tmp->cap.size; /* [한국어] 각 항목의 헤더 크기와 데이터 크기를 더한다 */

	state = kzalloc(size, GFP_KERNEL); /* [한국어] 계산한 크기로 한 번에 잡는다. kzalloc 이라 끝 표시 항목의 size 가 0 으로 초기화된다 */
	if (!state) /* [한국어] 할당 실패 */
		return NULL; /* [한국어] NULL */

	memcpy(state->config_space, dev->saved_config_space, /* [한국어] 표준 헤더 64바이트 사본을 그대로 옮긴다 */
	       sizeof(state->config_space)); /* [한국어] 배열 크기 자체를 쓰므로 양쪽 크기가 어긋날 수 없다 */

	cap = state->cap; /* [한국어] 가변 부분의 시작 위치 */
	hlist_for_each_entry(tmp, &dev->saved_cap_space, next) { /* [한국어] 같은 순서로 capability 사본을 이어 붙인다 */
		size_t len = sizeof(struct pci_cap_saved_data) + tmp->cap.size; /* [한국어] 이 항목이 차지할 바이트 수 = 헤더 + 데이터 */
		memcpy(cap, &tmp->cap, len); /* [한국어] 헤더와 데이터를 통째로 복사한다 */
		cap = (struct pci_cap_saved_data *)((u8 *)cap + len); /* [한국어] 다음 항목 자리로 전진한다. 항목 크기가 제각각이라 (u8 *) 로 캐스팅해 바이트 단위로 더한다 */
	}
	/* Empty cap_save terminates list */

	return state; /* [한국어] 호출자가 소유하고 kfree 로 해제한다 */
}
EXPORT_SYMBOL_GPL(pci_store_saved_state);

/**
 * pci_load_saved_state - Reload the provided save state into struct pci_dev.
 * @dev: PCI device that we're dealing with
 * @state: Saved state returned from pci_store_saved_state()
 */
/*
 * [한국어]
 * pci_load_saved_state - 밖에서 들고 온 상태 사본을 장치의 저장 버퍼에 되싣는다.
 *
 * @dev: 대상 PCI 장치.
 * @state: pci_store_saved_state() 가 돌려준 사본. NULL 이면 "저장 상태를 지워라"는 뜻이다.
 * @return: 0 성공. -EINVAL 이면 사본이 이 장치와 맞지 않는다.
 *
 * 주의할 점은 이 함수가 하드웨어를 전혀 건드리지 않는다는 것이다. 하는 일은
 * pci_dev 안의 saved_config_space 와 capability 버퍼들을 채우는 것뿐이고,
 * 실제로 장치에 써 넣는 것은 그 뒤에 부르는 pci_restore_state() 다.
 *
 * @state 가 NULL 인 호출이 중요한 관용구다. dev->state_saved 를 false 로
 * 내려 "복원할 사본이 없다"고 표시하는 효과만 남는다.
 * drivers/nvme/host/pci.c 가 정확히 이렇게 쓴다 — NVMe 자체 전력 상태
 * 제어(프로토콜 PS)를 시도했다가 실패하면, PCI PM 이 평소대로 D-state 를
 * 다루도록 저장 표시를 지워 되돌린다.
 *
 * 사본 검증이 -EINVAL 의 근거다. 사본 안의 capability 마다 이 장치에 같은
 * 번호·같은 크기의 버퍼가 있어야 한다. 다른 장치의 사본을 실으면 크기가
 * 어긋나 거절된다.
 *
 * 실행 문맥: memcpy 뿐이라 잠들지 않는다.
 *
 * 호출 체인:
 *   drivers/nvme/host/pci.c(NULL 로), pci_load_and_free_saved_state(), VFIO
 *     → [이 함수] → _pci_find_saved_cap(), memcpy()
 */
int pci_load_saved_state(struct pci_dev *dev,
			 struct pci_saved_state *state)
{
	struct pci_cap_saved_data *cap; /* [한국어] 사본 안에서 읽어 나갈 위치 */

	dev->state_saved = false; /* [한국어] 먼저 내려 둔다. 아래에서 성공적으로 다 실었을 때만 다시 올린다 — 중간에 실패하면 반쯤 채워진 사본을 복원하지 않게 하려는 것이다 */

	if (!state) /* [한국어] NULL 이면 "지워라"는 뜻이다 */
		return 0; /* [한국어] 플래그만 내린 채 성공으로 끝낸다 */

	memcpy(dev->saved_config_space, state->config_space, /* [한국어] 표준 헤더 64바이트 사본을 장치의 버퍼로 옮긴다 */
	       sizeof(state->config_space)); /* [한국어] 양쪽 배열 크기가 같으므로 이 크기로 안전하다 */

	cap = state->cap; /* [한국어] 가변 부분의 시작 */
	while (cap->size) { /* [한국어] size 가 0 인 끝 표시 항목을 만날 때까지 */
		struct pci_cap_saved_state *tmp; /* [한국어] 이 장치에서 대응하는 저장 버퍼 */

		tmp = _pci_find_saved_cap(dev, cap->cap_nr, cap->cap_extended); /* [한국어] 번호와 "표준/확장" 구분으로 짝을 찾는다 */
		if (!tmp || tmp->cap.size != cap->size) /* [한국어] 대응 버퍼가 없거나 크기가 다르면 다른 장치의 사본이다 */
			return -EINVAL; /* [한국어] 실을 수 없다 */

		memcpy(tmp->cap.data, cap->data, tmp->cap.size); /* [한국어] 데이터만 옮긴다. 헤더(번호·크기)는 이미 같음을 확인했다 */
		cap = (struct pci_cap_saved_data *)((u8 *)cap + /* [한국어] 다음 항목으로 전진한다 */
		       sizeof(struct pci_cap_saved_data) + cap->size); /* [한국어] 헤더 크기 + 이 항목의 데이터 크기만큼 */
	}

	dev->state_saved = true; /* [한국어] 끝까지 문제없이 실었으므로 복원 가능 표시를 올린다 */
	return 0; /* [한국어] 성공 */
}
EXPORT_SYMBOL_GPL(pci_load_saved_state);

/**
 * pci_load_and_free_saved_state - Reload the save state pointed to by state,
 *				   and free the memory allocated for it.
 * @dev: PCI device that we're dealing with
 * @state: Pointer to saved state returned from pci_store_saved_state()
 */
/*
 * [한국어]
 * pci_load_and_free_saved_state - 사본을 싣고 그 메모리까지 해제한다.
 *
 * @dev: 대상 PCI 장치.
 * @state: 사본 포인터를 담은 변수의 주소. 해제 후 NULL 로 덮어써 준다.
 * @return: pci_load_saved_state() 의 결과.
 *
 * "한 번 쓰고 버리는" 사본을 위한 편의 함수다. 포인터의 주소를 받는 이유는
 * 해제한 뒤 호출자의 변수까지 NULL 로 만들어, 이미 해제된 메모리를 다시
 * 쓰는 사고를 막기 위해서다.
 * 싣기가 실패해도 메모리는 해제한다 — 실패한 사본을 계속 들고 있을 이유가 없다.
 *
 * 실행 문맥: kfree 를 부른다. 프로세스 문맥.
 *
 * 호출 체인:
 *   VFIO 등 → [이 함수] → pci_load_saved_state(), kfree()
 */
int pci_load_and_free_saved_state(struct pci_dev *dev,
				  struct pci_saved_state **state)
{
	int ret = pci_load_saved_state(dev, *state); /* [한국어] 사본을 장치 버퍼에 싣는다. 결과는 성공·실패와 무관하게 보관해 둔다 */
	kfree(*state); /* [한국어] 싣기 성공 여부와 관계없이 사본 메모리를 해제한다 */
	*state = NULL; /* [한국어] 호출자의 포인터까지 NULL 로 만들어 해제 후 재사용을 막는다 */
	return ret; /* [한국어] 싣기 결과를 그대로 전달한다 */
}
EXPORT_SYMBOL_GPL(pci_load_and_free_saved_state);

/*
 * [한국어]
 * pcibios_enable_device - 아키텍처가 가로챌 수 있는 장치 활성화 훅(기본 구현).
 *
 * @dev: 활성화할 PCI 장치.
 * @bars: 활성화할 resource 인덱스의 비트마스크. 비트 i 가 1 이면 resource[i] 를 켠다.
 * @return: 0 성공, 음수 오류.
 *
 * __weak 이라 아키텍처가 같은 이름의 함수를 정의하면 그쪽이 링크된다.
 * 기본 구현은 공통 코드인 pci_enable_resources() 를 그대로 부른다.
 * 그 함수는 요청한 BAR 들이 실제로 주소를 배정받았는지 확인한 뒤 Command
 * 레지스터의 MEM/IO 디코딩 비트를 켠다 — 즉 "이 주소는 내 것"이라고 장치가
 * 응답하기 시작하는 지점이다.
 * x86 처럼 펌웨어와 협조가 필요한 아키텍처는 이 훅을 덮어써 추가 처리를 넣는다.
 *
 * 실행 문맥: config space 를 읽고 쓴다. 프로세스 문맥.
 *
 * 호출 체인:
 *   do_pci_enable_device() → [이 함수] → pci_enable_resources()
 */
int __weak pcibios_enable_device(struct pci_dev *dev, int bars)
{
	return pci_enable_resources(dev, bars); /* [한국어] 요청한 BAR 들의 배정 상태를 확인하고 Command 의 MEM/IO 디코딩을 켠다 */
}

/*
 * [한국어]
 * pci_host_bridge_enable_device - 호스트 브리지 드라이버에 활성화 사실을 알린다.
 *
 * @dev: 활성화 중인 PCI 장치.
 * @return: 0 성공(훅이 없으면 그냥 0), 음수면 브리지 드라이버가 거부했다.
 *
 * 일부 호스트 컨트롤러는 장치가 켜지기 전에 자기 쪽 설정을 손봐야 한다 —
 * 아웃바운드 주소 창을 열거나, 도메인별 자원을 잡거나 하는 일이다.
 * 그런 컨트롤러 드라이버가 host_bridge->enable_device 훅을 채워 두면
 * 여기서 불린다. 훅이 없으면(대부분의 경우) 아무 일도 하지 않는다.
 *
 * 실행 문맥: 컨트롤러 드라이버의 훅을 부른다. 프로세스 문맥.
 *
 * 호출 체인:
 *   do_pci_enable_device() → [이 함수] → host_bridge->enable_device()
 */
static int pci_host_bridge_enable_device(struct pci_dev *dev)
{
	struct pci_host_bridge *host_bridge = pci_find_host_bridge(dev->bus); /* [한국어] 이 장치가 속한 도메인의 호스트 브리지를 찾는다. 버스 계층을 루트까지 거슬러 올라간다 */
	int err; /* [한국어] 훅의 반환값 */

	if (host_bridge && host_bridge->enable_device) { /* [한국어] 브리지가 있고 활성화 훅을 채워 두었으면 */
		err = host_bridge->enable_device(host_bridge, dev); /* [한국어] 컨트롤러 드라이버에게 처리를 넘긴다 */
		if (err) /* [한국어] 거부하면 */
			return err; /* [한국어] 그 오류를 그대로 올려 활성화를 중단시킨다 */
	}

	return 0; /* [한국어] 훅이 없거나 성공했다 */
}

/*
 * [한국어]
 * pci_host_bridge_disable_device - 호스트 브리지 드라이버에 비활성화 사실을 알린다.
 *
 * @dev: 비활성화 중인 PCI 장치.
 * @return: 없음.
 *
 * pci_host_bridge_enable_device() 의 짝이다. 활성화 때 잡아 둔 컨트롤러 쪽
 * 자원을 돌려주도록 훅을 부른다. 반환값이 없는 이유는 정리 경로라서
 * 실패해도 되돌릴 것이 없기 때문이다.
 *
 * 실행 문맥: 프로세스 문맥.
 *
 * 호출 체인:
 *   do_pci_enable_device() 의 오류 경로, pci_disable_device()
 *     → [이 함수] → host_bridge->disable_device()
 */
static void pci_host_bridge_disable_device(struct pci_dev *dev)
{
	struct pci_host_bridge *host_bridge = pci_find_host_bridge(dev->bus); /* [한국어] 이 장치가 속한 도메인의 호스트 브리지 */

	if (host_bridge && host_bridge->disable_device) /* [한국어] 브리지가 있고 비활성화 훅이 있으면 */
		host_bridge->disable_device(host_bridge, dev); /* [한국어] 컨트롤러 드라이버에게 정리를 맡긴다 */
}

/*
 * [한국어]
 * do_pci_enable_device - 장치를 실제로 켜는 본체. 전원, 디코딩, 인터럽트 순으로 준비한다.
 *
 * @dev: 활성화할 PCI 장치.
 * @bars: 켤 resource 인덱스의 비트마스크.
 * @return: 0 성공, 음수 오류.
 *
 * 참조 계수 관리는 호출자(pci_enable_device_flags)가 하고, 이 함수는 순수하게
 * "켜는 절차"만 밟는다. 순서에 각각 이유가 있다.
 *
 *   1) D0 로 올린다. 전원이 없으면 그 뒤의 config 쓰기가 모두 무의미하다.
 *      -EIO 를 예외로 허용하는 것이 중요한 세부다 — pci_power_up() 은 PM
 *      capability 가 없는 장치에 언제나 -EIO 를 돌려주지만, 그런 장치는
 *      애초에 늘 켜져 있으므로 계속 진행해도 된다.
 *   2) 상위 브리지의 ASPM 링크 설정을 다시 계산한다. 곧 트래픽이 흐를 참이므로
 *      절전 상태를 새 상황에 맞춘다.
 *   3) 호스트 브리지 드라이버에 알린다.
 *   4) pcibios_enable_device() 로 Command 의 MEM/IO 디코딩을 켠다.
 *      여기서 실패하면 3)에서 잡은 것을 되돌려야 해서 goto 정리 경로가 있다.
 *   5) enable 단계 quirk 를 적용한다.
 *   6) INTx(레거시 핀 인터럽트)를 다시 살린다.
 *
 * 6)의 조건이 미묘하다. 이미 MSI/MSI-X 를 쓰는 장치는 INTx 를 켜면 안 된다 —
 * 두 방식은 배타적이고, MSI 를 쓰는 중에 INTx 가 열려 있으면 가짜 인터럽트가
 * 생긴다. 그래서 msi_enabled/msix_enabled 이면 그대로 끝낸다.
 * NVMe 는 MSI-X 를 쓰지만, pci_enable_device_mem() 을 부르는 시점에는 아직
 * MSI-X 를 요청하기 전이라 이 갈래로 오지 않는다. 대신 Interrupt Pin 이
 * 0 이 아닌 경우에만 INTx 를 여는 두 번째 조건에 걸린다.
 *
 * 실행 문맥: 전원 전이 지연 동안 잠들 수 있다. 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_enable_device_flags(), pci_reenable_device() → [이 함수]
 *     → pci_set_power_state(), pcibios_enable_device(), pci_fixup_device()
 */
static int do_pci_enable_device(struct pci_dev *dev, int bars)
{
	int err; /* [한국어] 각 단계의 반환값 */
	struct pci_dev *bridge; /* [한국어] 상위 브리지. ASPM 재설정 대상이다 */
	u16 cmd; /* [한국어] Command 레지스터 값 */
	u8 pin; /* [한국어] Interrupt Pin 레지스터 값 — 0 이면 이 함수는 INTx 를 쓰지 않는다 */

	err = pci_set_power_state(dev, PCI_D0); /* [한국어] 무엇보다 먼저 전원을 D0 로 올린다 */
	if (err < 0 && err != -EIO) /* [한국어] -EIO 는 예외로 넘긴다. PM capability 가 없는 장치는 늘 -EIO 를 받지만 애초에 항상 켜져 있어 문제가 없다 */
		return err; /* [한국어] 그 밖의 오류는 활성화를 포기한다 */

	bridge = pci_upstream_bridge(dev); /* [한국어] 상위 브리지를 찾는다 */
	if (bridge) /* [한국어] 루트 버스 직결이 아니면 */
		pcie_aspm_powersave_config_link(bridge); /* [한국어] 곧 트래픽이 흐를 것이므로 그 링크의 ASPM 절전 설정을 다시 계산한다 */

	err = pci_host_bridge_enable_device(dev); /* [한국어] 호스트 컨트롤러 드라이버에 활성화를 알린다 */
	if (err) /* [한국어] 컨트롤러가 거부하면 */
		return err; /* [한국어] 여기서 중단한다. 아직 되돌릴 것이 없다 */

	err = pcibios_enable_device(dev, bars); /* [한국어] 요청한 BAR 들에 대해 Command 의 MEM/IO 디코딩을 켠다. 이 순간부터 장치가 그 주소에 응답한다 */
	if (err < 0) /* [한국어] 실패하면 */
		goto err_enable; /* [한국어] 앞서 알린 호스트 브리지 쪽을 되돌려야 한다 */
	pci_fixup_device(pci_fixup_enable, dev); /* [한국어] 활성화 단계에 걸린 quirk 를 적용한다. 켜진 직후에만 손봐야 하는 하드웨어가 있다 */

	if (dev->msi_enabled || dev->msix_enabled) /* [한국어] 이미 MSI 또는 MSI-X 를 쓰고 있으면 */
		return 0; /* [한국어] INTx 를 건드리지 않고 끝낸다. 두 방식은 배타적이라 함께 열면 가짜 인터럽트가 생긴다 */

	pci_read_config_byte(dev, PCI_INTERRUPT_PIN, &pin); /* [한국어] config space 오프셋 0x3d 의 Interrupt Pin. 0 이면 이 함수가 INTx 를 전혀 쓰지 않는다는 뜻이다 */
	if (pin) { /* [한국어] INTx 를 쓰는 장치라면 */
		pci_read_config_word(dev, PCI_COMMAND, &cmd); /* [한국어] Command 레지스터를 읽어 */
		if (cmd & PCI_COMMAND_INTX_DISABLE) /* [한국어] INTx Disable 비트(비트 10)가 켜져 있으면 — 리셋이나 이전 사용의 잔재다 */
			pci_write_config_word(dev, PCI_COMMAND, /* [한국어] 그 비트를 지워 INTx 를 다시 연다 */
					      cmd & ~PCI_COMMAND_INTX_DISABLE); /* [한국어] ~ 로 해당 비트만 0 으로 만든다. 다른 비트는 그대로 둔다 */
	}

	return 0; /* [한국어] 활성화 완료 */

err_enable: /* [한국어] pcibios_enable_device() 가 실패했을 때만 오는 정리 지점 */
	pci_host_bridge_disable_device(dev); /* [한국어] 호스트 브리지 드라이버에 알렸던 것을 되돌린다 */

	return err; /* [한국어] 실패 원인을 그대로 올린다 */

}

/**
 * pci_reenable_device - Resume abandoned device
 * @dev: PCI device to be resumed
 *
 * NOTE: This function is a backend of pci_default_resume() and is not supposed
 * to be called by normal code, write proper resume handler and use it instead.
 */
/*
 * [한국어]
 * pci_reenable_device - 이미 enable 된 장치의 활성화 절차를 다시 밟는다.
 *
 * @dev: 대상 PCI 장치.
 * @return: 0 성공(또는 애초에 enable 상태가 아니어서 할 일 없음), 음수 오류.
 *
 * 절전에서 돌아오면 config space 가 초기화돼 Command 의 디코딩 비트가 꺼져
 * 있을 수 있다. 그런데 참조 계수(enable_cnt)는 그대로라 pci_enable_device()
 * 를 다시 불러도 "이미 켜져 있다"며 곧바로 돌아온다. 그 틈을 메우는 함수다 —
 * 참조 계수를 건드리지 않고 do_pci_enable_device() 만 다시 밟는다.
 *
 * 모든 resource 비트를 켜는 마스크를 넘기는 이유는, 원래 어떤 BAR 로 켰는지
 * 기억해 두지 않기 때문이다. 이미 배정되지 않은 BAR 는 하위에서 걸러진다.
 *
 * 원본 주석의 경고대로 드라이버가 직접 부를 함수가 아니다 —
 * 제대로 된 resume 핸들러를 작성해 쓰라는 뜻이다.
 *
 * 실행 문맥: 프로세스 문맥.
 *
 * 호출 체인:
 *   pci-driver.c 의 pci_default_resume 계열 → [이 함수] → do_pci_enable_device()
 */
int pci_reenable_device(struct pci_dev *dev)
{
	if (pci_is_enabled(dev)) /* [한국어] enable_cnt 가 0 이면 원래 꺼져 있던 장치이므로 되살릴 것이 없다 */
		return do_pci_enable_device(dev, (1 << PCI_NUM_RESOURCES) - 1); /* [한국어] 모든 resource 비트를 세운 마스크. 어떤 BAR 로 켰었는지 기억해 두지 않아 전부 시도한다 */
	return 0; /* [한국어] 꺼져 있던 장치는 그대로 둔다 */
}
EXPORT_SYMBOL(pci_reenable_device);

/*
 * [한국어]
 * pci_enable_bridge - 이 장치까지 이르는 브리지 경로를 루트 쪽부터 모두 켠다.
 *
 * @dev: 켜야 할 브리지(또는 그 아래 장치).
 * @return: 없음. 실패해도 로그만 남기고 계속 진행한다.
 *
 * 엔드포인트를 켜는 것만으로는 부족하다. CPU 가 그 BAR 주소로 보낸 트랜잭션이
 * 중간 브리지를 통과하려면, 경로상 모든 브리지가 켜져 있고 Bus Master 여야
 * 한다. 브리지의 Bus Master Enable 은 "아래에서 올라온 DMA 를 위로 전달한다"는
 * 뜻이라, 이것이 꺼져 있으면 NVMe 가 아무리 DMA 를 내보내도 메모리에 닿지 못한다.
 *
 * 재귀로 루트 쪽부터 처리한다 — 위쪽 브리지가 먼저 켜져야 아래쪽 브리지에
 * 접근할 수 있기 때문이다.
 *
 * 실패해도 중단하지 않고 로그만 남기는 이유는, 여기서 포기하면 아래 장치의
 * 활성화가 통째로 실패해 상황이 더 나빠지기 때문이다. 켤 수 있는 데까지
 * 켜 보고 결과는 상위 장치의 접근 실패로 드러나게 둔다.
 *
 * 실행 문맥: 프로세스 문맥. 재귀 깊이는 PCI 계층 깊이만큼이다.
 *
 * 호출 체인:
 *   pci_enable_device_flags() → [이 함수] → [이 함수](재귀),
 *     pci_enable_device(), pci_set_master()
 */
static void pci_enable_bridge(struct pci_dev *dev)
{
	struct pci_dev *bridge; /* [한국어] 한 단계 위 브리지 */
	int retval; /* [한국어] pci_enable_device() 의 결과 */

	bridge = pci_upstream_bridge(dev); /* [한국어] 상위 브리지를 찾는다 */
	if (bridge) /* [한국어] 루트에 닿지 않았으면 */
		pci_enable_bridge(bridge); /* [한국어] 위쪽부터 먼저 켠다 — 위가 꺼져 있으면 아래에 접근할 수 없다 */

	if (pci_is_enabled(dev)) { /* [한국어] 이 브리지가 이미 켜져 있으면 */
		if (!dev->is_busmaster) /* [한국어] Bus Master 만 확인한다. 켜져 있어도 BME 가 꺼져 있으면 아래 DMA 가 위로 못 간다 */
			pci_set_master(dev); /* [한국어] Bus Master Enable 을 세운다 */
		return; /* [한국어] 이미 켜져 있으므로 더 할 일이 없다 */
	}

	retval = pci_enable_device(dev); /* [한국어] 꺼져 있으면 켠다. MEM 과 IO 를 모두 요청한다 — 브리지는 양쪽 창을 다 가질 수 있다 */
	if (retval) /* [한국어] 실패해도 */
		pci_err(dev, "Error enabling bridge (%d), continuing\n", /* [한국어] 로그만 남기고 계속한다. 여기서 포기하면 아래 장치가 통째로 못 켜진다 */
			retval); /* [한국어] 실패 코드 */
	pci_set_master(dev); /* [한국어] 브리지의 Bus Master 는 반드시 켠다 — 아래에서 올라오는 DMA 를 위로 전달하는 스위치다 */
}

/*
 * [한국어]
 * pci_enable_device_flags - 활성화의 공통 본체. 참조 계수와 BAR 선택을 담당한다.
 *
 * @dev: 활성화할 PCI 장치.
 * @flags: 켤 자원의 종류. IORESOURCE_MEM 이면 메모리 BAR 만,
 *         IORESOURCE_MEM | IORESOURCE_IO 면 둘 다.
 * @return: 0 성공, 음수 오류.
 *
 * 두 가지가 이 함수의 핵심이다.
 *
 * 하나는 참조 계수다. dev->enable_cnt 를 원자적으로 올려, 그 결과가 1 보다
 * 크면 이미 다른 곳에서 켜 둔 것이므로 곧바로 성공을 돌려준다. 덕분에
 * pci_enable_device() 를 여러 번 불러도 안전하고, pci_disable_device() 와
 * 짝을 맞춰 부르기만 하면 마지막 하나가 실제로 끈다. atomic_inc_return 을
 * 쓰므로 여러 CPU 가 동시에 불러도 정확히 한 쪽만 실제 활성화를 수행한다.
 * 실패하면 올렸던 계수를 다시 내려 균형을 맞춘다.
 *
 * 다른 하나는 BAR 선택이다. resource[] 를 훑어 @flags 와 성격이 맞는 것만
 * 비트마스크로 모은다. 두 루프로 나뉜 이유는 원본 주석대로 SR-IOV VF BAR
 * 구간을 건너뛰기 위해서다 — VF BAR 는 PF 를 켜는 것과 다른 절차로 다룬다.
 *
 * 맨 앞에서 전원 상태를 새로 읽는 것도 이유가 있다. 부팅 직후나 장치 제거
 * 직후에는 캐시가 실물과 다를 수 있고, 그 상태에서 MSI 메시지를 쓰면
 * 엉뚱하게 동작한다(원본 주석).
 *
 * NVMe 와의 관계: drivers/nvme/host/pci.c 가 pci_enable_device_mem() 을 통해
 * 이 경로로 들어온다. NVMe 는 IO 공간 BAR 를 쓰지 않아 MEM 만 요청한다.
 *
 * 실행 문맥: 잠들 수 있다. 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_enable_device_mem() / pci_enable_device() → [이 함수]
 *     → pci_update_current_state(), pci_enable_bridge(), do_pci_enable_device()
 */
static int pci_enable_device_flags(struct pci_dev *dev, unsigned long flags)
{
	struct pci_dev *bridge; /* [한국어] 상위 브리지 */
	int err; /* [한국어] do_pci_enable_device() 의 결과 */
	int i, bars = 0; /* [한국어] i 는 resource 인덱스, bars 는 켤 resource 들의 비트마스크 */

	/*
	 * Power state could be unknown at this point, either due to a fresh
	 * boot or a device removal call.  So get the current power state
	 * so that things like MSI message writing will behave as expected
	 * (e.g. if the device really is in D0 at enable time).
	 */
	pci_update_current_state(dev, dev->current_state); /* [한국어] 캐시된 전원 상태를 실물에 맞춘다. 두 번째 인자는 PM capability 가 없을 때만 쓰이는 대체값이라 현재 값을 그대로 넘긴다 */

	if (atomic_inc_return(&dev->enable_cnt) > 1) /* [한국어] 참조 계수를 원자적으로 올린다. 결과가 1 보다 크면 이미 누군가 켜 둔 것이다. 여러 CPU 가 동시에 불러도 정확히 한 쪽만 아래로 내려간다 */
		return 0;		/* already enabled */

	bridge = pci_upstream_bridge(dev); /* [한국어] 상위 브리지를 찾는다 */
	if (bridge) /* [한국어] 루트 직결이 아니면 */
		pci_enable_bridge(bridge); /* [한국어] 경로상 브리지를 루트 쪽부터 모두 켠다. 그러지 않으면 이 장치의 BAR 에 트랜잭션이 닿지 못한다 */

	/* only skip sriov related */
	for (i = 0; i <= PCI_ROM_RESOURCE; i++) /* [한국어] 표준 BAR 0~5 와 ROM(PCI_ROM_RESOURCE)까지 */
		if (dev->resource[i].flags & flags) /* [한국어] 그 자원의 성격이 요청한 flags 와 겹치는지 본다. MEM 만 요청하면 IO BAR 는 걸러진다 */
			bars |= (1 << i); /* [한국어] 해당 인덱스 비트를 세운다 */
	for (i = PCI_BRIDGE_RESOURCES; i < DEVICE_COUNT_RESOURCE; i++) /* [한국어] 브리지 윈도 구간. 중간의 SR-IOV VF BAR 구간을 건너뛰려고 루프를 둘로 나눈 것이다 */
		if (dev->resource[i].flags & flags) /* [한국어] 같은 기준으로 거른다 */
			bars |= (1 << i); /* [한국어] 해당 인덱스 비트를 세운다 */

	err = do_pci_enable_device(dev, bars); /* [한국어] 고른 자원들에 대해 실제 활성화 절차를 밟는다 */
	if (err < 0) /* [한국어] 실패하면 */
		atomic_dec(&dev->enable_cnt); /* [한국어] 앞서 올린 참조 계수를 되돌린다. 그러지 않으면 영영 끌 수 없는 장치가 된다 */
	return err; /* [한국어] 활성화 결과 */
}

/**
 * pci_enable_device_mem - Initialize a device for use with Memory space
 * @dev: PCI device to be initialized
 *
 * Initialize device before it's used by a driver. Ask low-level code
 * to enable Memory resources. Wake up the device if it was suspended.
 * Beware, this function can fail.
 */
/*
 * [한국어]
 * pci_enable_device_mem - 메모리 공간만 쓰는 장치를 활성화한다.
 *
 * @dev: 활성화할 PCI 장치.
 * @return: 0 성공, 음수 오류.
 *
 * pci_enable_device_flags(dev, IORESOURCE_MEM) 의 얇은 래퍼다. IO 공간 BAR 는
 * 켜지 않는다는 점이 pci_enable_device() 와 유일한 차이다.
 *
 * 그 차이가 실질적인 이유는 이렇다. IO 공간은 x86 의 64KB 짜리 별도 주소
 * 공간으로, 시스템 전체에서 극히 부족한 자원이다. 쓰지도 않을 IO BAR 를
 * 켜면 그 자원을 붙들고 있게 되고, 배정에 실패하면 활성화 자체가 실패한다.
 * 그래서 메모리 BAR 만 쓰는 현대적인 장치는 이 판을 쓰는 것이 맞다.
 *
 * NVMe 와의 관계: drivers/nvme/host/pci.c 의 nvme_pci_enable() 이 이 함수를
 * 부른다. NVMe 규격은 컨트롤러 레지스터와 도어벨을 BAR0 의 메모리 공간에
 * 매핑하도록 정하고 있고 IO 공간은 쓰지 않으므로, _mem 판이 정확한 선택이다.
 * 이 호출이 성공해야 Command 의 Memory Space Enable 이 켜져 BAR0 매핑을 통한
 * 레지스터 접근이 실제로 동작한다.
 *
 * 실행 문맥: 잠들 수 있다. 프로세스 문맥.
 *
 * 호출 체인:
 *   drivers/nvme/host/pci.c 의 nvme_pci_enable() → [이 함수]
 *     → pci_enable_device_flags() → do_pci_enable_device()
 */
int pci_enable_device_mem(struct pci_dev *dev)
{
	return pci_enable_device_flags(dev, IORESOURCE_MEM); /* [한국어] IORESOURCE_MEM 만 넘겨 메모리 BAR 만 켠다. IO BAR 는 걸러진다 */
}
EXPORT_SYMBOL(pci_enable_device_mem);

/**
 * pci_enable_device - Initialize device before it's used by a driver.
 * @dev: PCI device to be initialized
 *
 * Initialize device before it's used by a driver. Ask low-level code
 * to enable I/O and memory. Wake up the device if it was suspended.
 * Beware, this function can fail.
 *
 * Note we don't actually enable the device many times if we call
 * this function repeatedly (we just increment the count).
 */
/*
 * [한국어]
 * pci_enable_device - 메모리와 IO 공간을 모두 쓰는 장치를 활성화한다.
 *
 * @dev: 활성화할 PCI 장치.
 * @return: 0 성공, 음수 오류.
 *
 * pci_enable_device_flags(dev, IORESOURCE_MEM | IORESOURCE_IO) 의 래퍼다.
 * 두 종류의 BAR 를 모두 켜므로, IO 공간 BAR 가 배정되지 않았다면 활성화가
 * 실패할 수 있다. 메모리 BAR 만 쓰는 장치라면 pci_enable_device_mem() 이 낫다.
 *
 * 원본 주석이 짚듯 여러 번 불러도 안전하다 — 참조 계수만 올라간다.
 * 다만 pci_disable_device() 를 같은 횟수만큼 불러야 실제로 꺼진다.
 *
 * 실행 문맥: 잠들 수 있다. 프로세스 문맥.
 *
 * 호출 체인:
 *   각 드라이버의 probe, pci_enable_bridge() → [이 함수]
 *     → pci_enable_device_flags() → do_pci_enable_device()
 */
int pci_enable_device(struct pci_dev *dev)
{
	return pci_enable_device_flags(dev, IORESOURCE_MEM | IORESOURCE_IO); /* [한국어] MEM 과 IO 를 모두 요청한다 */
}
EXPORT_SYMBOL(pci_enable_device);

/*
 * pcibios_device_add - provide arch specific hooks when adding device dev
 * @dev: the PCI device being added
 *
 * Permits the platform to provide architecture specific functionality when
 * devices are added. This is the default implementation. Architecture
 * implementations can override this.
 */
/*
 * [한국어]
 * pcibios_device_add - 장치가 시스템에 등록될 때 아키텍처가 끼어들 수 있는 훅(기본 구현).
 *
 * @dev: 막 등록되는 PCI 장치.
 * @return: 0 성공. 음수를 돌려주면 장치 등록이 중단된다.
 *
 * __weak 이라 아키텍처가 같은 이름의 함수를 정의하면 그쪽이 링크된다.
 * 공통 코드에는 특별히 할 일이 없어 기본 구현은 0 만 돌려준다.
 * 아키텍처별로 DMA 마스크 조정이나 IOMMU 그룹 배정 같은 처리를 넣을 자리다.
 *
 * 실행 문맥: 열거 중 프로세스 문맥.
 *
 * 호출 체인:
 *   probe.c 의 pci_device_add() → [이 함수]
 */
int __weak pcibios_device_add(struct pci_dev *dev)
{
	return 0; /* [한국어] 공통 코드에는 할 일이 없다 */
}

/**
 * pcibios_release_device - provide arch specific hooks when releasing
 *			    device dev
 * @dev: the PCI device being released
 *
 * Permits the platform to provide architecture specific functionality when
 * devices are released. This is the default implementation. Architecture
 * implementations can override this.
 */
/*
 * [한국어]
 * pcibios_release_device - 장치가 해제될 때의 아키텍처 훅(기본 구현, 빈 함수).
 *
 * @dev: 해제되는 PCI 장치.
 * @return: 없음.
 *
 * __weak 이며 본문이 비어 있다. 아키텍처가 장치 등록 때 잡아 둔 자원이
 * 있다면 여기서 돌려주도록 덮어쓴다.
 *
 * 호출 체인:
 *   probe.c 의 장치 해제 경로 → [이 함수]
 */
void __weak pcibios_release_device(struct pci_dev *dev) {}

/**
 * pcibios_disable_device - disable arch specific PCI resources for device dev
 * @dev: the PCI device to disable
 *
 * Disables architecture specific PCI resources for the device. This
 * is the default implementation. Architecture implementations can
 * override this.
 */
/*
 * [한국어]
 * pcibios_disable_device - 장치 비활성화 시의 아키텍처 훅(기본 구현, 빈 함수).
 *
 * @dev: 비활성화되는 PCI 장치.
 * @return: 없음.
 *
 * __weak 이며 본문이 비어 있다. pcibios_enable_device() 가 아키텍처별로 켠
 * 것이 있다면 그 짝으로 여기서 끈다.
 *
 * 호출 체인:
 *   do_pci_disable_device() → [이 함수]
 */
void __weak pcibios_disable_device(struct pci_dev *dev) {}

/*
 * [한국어]
 * do_pci_disable_device - Bus Master 를 끄고 아키텍처 훅을 부른다.
 *
 * @dev: 비활성화할 PCI 장치.
 * @return: 없음.
 *
 * "장치를 끈다"고 할 때 실제로 하는 일은 Command 레지스터의 Bus Master Enable
 * 비트를 지우는 것뿐이다. 이 비트가 내려가면 장치는 더 이상 스스로 트랜잭션을
 * 시작할 수 없다 — 즉 DMA 가 멈춘다. MEM/IO 디코딩 비트는 건드리지 않는다.
 *
 * 이것이 정리 경로에서 결정적으로 중요하다. 드라이버를 내리는데 DMA 가
 * 멈추지 않으면, 이미 해제한 메모리에 장치가 계속 써 넣어 메모리를
 * 망가뜨린다. NVMe 로 치면 컨트롤러가 SQ/CQ 를 계속 읽고 PRP 가 가리키는
 * 버퍼에 데이터를 쓰는 상태가 이어지는 것이다.
 *
 * 이미 꺼져 있으면 쓰지 않는다 — 불필요한 config 쓰기를 피한다.
 *
 * 실행 문맥: config space 를 읽고 쓴다. 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_disable_device(), pci_disable_enabled_device() → [이 함수]
 *     → pci_read/write_config_word(), pcibios_disable_device()
 */
static void do_pci_disable_device(struct pci_dev *dev)
{
	u16 pci_command; /* [한국어] Command 레지스터 값 */

	pci_read_config_word(dev, PCI_COMMAND, &pci_command); /* [한국어] config space 오프셋 0x04 의 Command 를 읽는다 */
	if (pci_command & PCI_COMMAND_MASTER) { /* [한국어] Bus Master Enable(비트 2)이 켜져 있을 때만 손댄다 */
		pci_command &= ~PCI_COMMAND_MASTER; /* [한국어] 그 비트만 지운다. 다른 비트(MEM/IO 디코딩 등)는 그대로 둔다 */
		pci_write_config_word(dev, PCI_COMMAND, pci_command); /* [한국어] 되쓰는 순간 장치는 새 DMA 트랜잭션을 시작할 수 없게 된다 */
	}

	pcibios_disable_device(dev); /* [한국어] 아키텍처가 켜 두었던 것이 있으면 여기서 끈다 */
}

/**
 * pci_disable_enabled_device - Disable device without updating enable_cnt
 * @dev: PCI device to disable
 *
 * NOTE: This function is a backend of PCI power management routines and is
 * not supposed to be called drivers.
 */
/*
 * [한국어]
 * pci_disable_enabled_device - 참조 계수를 건드리지 않고 장치만 끈다.
 *
 * @dev: 대상 PCI 장치.
 * @return: 없음.
 *
 * pci_disable_device() 와 달리 enable_cnt 를 내리지 않는다. 절전으로 들어갈
 * 때처럼 "지금은 끄지만 나중에 그대로 되살릴 것"인 상황을 위한 함수다.
 * 계수를 유지해 두면 돌아온 뒤 pci_reenable_device() 로 같은 상태를
 * 복구할 수 있다.
 *
 * 원본 주석대로 드라이버가 부를 함수가 아니다 — PM 코어 내부용이다.
 *
 * 실행 문맥: 프로세스 문맥.
 *
 * 호출 체인:
 *   PCI 전원 관리 경로 → [이 함수] → do_pci_disable_device()
 */
void pci_disable_enabled_device(struct pci_dev *dev)
{
	if (pci_is_enabled(dev)) /* [한국어] 애초에 켜져 있지 않으면 끌 것이 없다 */
		do_pci_disable_device(dev); /* [한국어] Bus Master 만 끈다. enable_cnt 는 그대로 둔다 */
}

/**
 * pci_disable_device - Disable PCI device after use
 * @dev: PCI device to be disabled
 *
 * Signal to the system that the PCI device is not in use by the system
 * anymore.  This only involves disabling PCI bus-mastering, if active.
 *
 * Note we don't actually disable the device until all callers of
 * pci_enable_device() have called pci_disable_device().
 */
/*
 * [한국어]
 * pci_disable_device - 장치를 다 썼음을 알린다(참조 계수 감소 포함).
 *
 * @dev: 비활성화할 PCI 장치.
 * @return: 없음.
 *
 * pci_enable_device() 계열의 정확한 짝이다. enable_cnt 를 원자적으로 내리고,
 * 0 이 되었을 때만 실제로 끈다. 원본 주석이 짚듯 pci_enable_device() 를
 * 부른 모든 주체가 이 함수를 불러야 비로소 장치가 꺼진다.
 *
 * 맨 앞의 dev_WARN_ONCE 는 짝이 맞지 않는 호출을 잡는 장치다. 이미 0 인데
 * 또 내리면 계수가 음수가 되어, 이후의 pci_enable_device() 가 "이미 켜져
 * 있다"고 오판하게 된다. 그런 버그는 원인을 찾기 어려워 즉시 경고로 드러낸다.
 *
 * 실제로 끄는 일은 Bus Master 비트를 지우는 것뿐이다(원본 주석). MEM/IO
 * 디코딩은 그대로 남으므로, 이 호출 뒤에도 config 와 BAR 는 여전히 읽을 수 있다.
 *
 * NVMe 와의 관계: drivers/nvme/host/pci.c 가 제거·정리 경로에서 부른다.
 * 이 호출로 Bus Master 가 꺼져야 컨트롤러가 SQ/CQ 를 더 읽지 않고 DMA 가 멎는다.
 *
 * 실행 문맥: 프로세스 문맥.
 *
 * 호출 체인:
 *   drivers/nvme/host/pci.c, 각 드라이버의 remove → [이 함수]
 *     → pci_host_bridge_disable_device(), do_pci_disable_device()
 */
void pci_disable_device(struct pci_dev *dev)
{
	dev_WARN_ONCE(&dev->dev, atomic_read(&dev->enable_cnt) <= 0, /* [한국어] 계수가 이미 0 이하인데 또 내리려 한다면 enable/disable 짝이 맞지 않는 버그다 */
		      "disabling already-disabled device"); /* [한국어] 한 번만 경고해 로그를 덮지 않게 한다 */

	if (atomic_dec_return(&dev->enable_cnt) != 0) /* [한국어] 원자적으로 내린다. 0 이 아니면 아직 다른 사용자가 남아 있다 */
		return; /* [한국어] 실제로 끄지 않고 돌아간다 */

	pci_host_bridge_disable_device(dev); /* [한국어] 마지막 사용자였으므로 호스트 브리지 드라이버에 먼저 알린다 */

	do_pci_disable_device(dev); /* [한국어] Bus Master 를 끄고 아키텍처 훅을 부른다 */

	dev->is_busmaster = 0; /* [한국어] 소프트웨어 쪽 표시도 내린다. pci_set_master()/pci_clear_master() 가 이 필드로 현재 상태를 판단한다 */
}
EXPORT_SYMBOL(pci_disable_device);

/**
 * pcibios_set_pcie_reset_state - set reset state for device dev
 * @dev: the PCIe device reset
 * @state: Reset state to enter into
 *
 * Set the PCIe reset state for the device. This is the default
 * implementation. Architecture implementations can override this.
 */
/*
 * [한국어]
 * pcibios_set_pcie_reset_state - PCIe 리셋 상태를 설정하는 아키텍처 훅(기본 구현).
 *
 * @dev: 대상 PCIe 장치.
 * @state: 요청하는 리셋 상태(pcie_reset_state_t).
 * @return: 언제나 -EINVAL — 기본 구현은 이 기능을 지원하지 않는다.
 *
 * __weak 이라 아키텍처(대표적으로 powerpc 의 EEH)가 덮어쓸 수 있다. 공통
 * 코드에는 링크 리셋을 거는 표준 방법이 없어 기본값은 거절이다.
 * 호출자는 -EINVAL 을 "이 플랫폼에는 그런 수단이 없다"로 읽는다.
 *
 * 호출 체인:
 *   pci_set_pcie_reset_state() → [이 함수]
 */
int __weak pcibios_set_pcie_reset_state(struct pci_dev *dev,
					enum pcie_reset_state state)
{
	return -EINVAL; /* [한국어] 지원하지 않음을 알린다. 아키텍처가 덮어쓰지 않았다면 이 경로가 전부다 */
}

/**
 * pci_set_pcie_reset_state - set reset state for device dev
 * @dev: the PCIe device reset
 * @state: Reset state to enter into
 *
 * Sets the PCI reset state for the device.
 */
/*
 * [한국어]
 * pci_set_pcie_reset_state - 아키텍처 훅을 통해 PCIe 리셋 상태를 설정한다.
 *
 * @dev: 대상 PCIe 장치.
 * @state: 요청하는 리셋 상태.
 * @return: 아키텍처 훅의 결과. 기본 구현에서는 -EINVAL.
 *
 * 공개 API 이지만 실제 내용은 전적으로 아키텍처 훅에 달려 있다. powerpc 의
 * EEH(Enhanced Error Handling)처럼 플랫폼이 링크 리셋을 직접 제어할 수 있는
 * 환경에서만 의미가 있다.
 *
 * 실행 문맥: 아키텍처 구현에 달렸다.
 *
 * 호출 체인:
 *   EEH 등 플랫폼 오류 복구 코드 → [이 함수] → pcibios_set_pcie_reset_state()
 */
int pci_set_pcie_reset_state(struct pci_dev *dev, enum pcie_reset_state state)
{
	return pcibios_set_pcie_reset_state(dev, state); /* [한국어] 아키텍처 구현에 그대로 넘긴다 */
}
EXPORT_SYMBOL_GPL(pci_set_pcie_reset_state);

#ifdef CONFIG_PCIEAER /* [한국어] AER 을 빌드했을 때만 필요한 함수다. AER 코드가 오류 처리 후 상태를 지우는 데 쓴다 */
/*
 * [한국어]
 * pcie_clear_device_status - PCIe Device Status 의 오류 비트를 지운다.
 *
 * @dev: 대상 PCIe 장치.
 * @return: 없음.
 *
 * PCIe capability 의 Device Status 레지스터에는 이 장치가 겪은 오류가 비트로
 * 남는다. 네 가지를 한 번에 지운다.
 *   - CED(Correctable Error Detected): 하드웨어가 스스로 복구한 오류.
 *   - NFED(Non-Fatal Error Detected): 해당 트랜잭션만 실패한 오류.
 *   - FED(Fatal Error Detected): 링크가 신뢰할 수 없게 된 오류.
 *   - URD(Unsupported Request Detected): 장치가 처리할 수 없는 요청.
 * 이 비트들은 W1C(Write-1-to-Clear) 라서 1 을 써야 지워진다. 그래서 "쓰기"가
 * 곧 "지우기"다.
 * 오류 복구를 마친 뒤 이 흔적을 지우지 않으면, 다음 점검에서 이미 처리한
 * 오류를 새 오류로 다시 보고하게 된다.
 *
 * 실행 문맥: config space 쓰기. 프로세스 문맥.
 *
 * 호출 체인:
 *   drivers/pci/pcie/aer.c 의 복구 경로 → [이 함수] → pcie_capability_write_word()
 */
void pcie_clear_device_status(struct pci_dev *dev)
{
	pcie_capability_write_word(dev, PCI_EXP_DEVSTA, /* [한국어] Device Status 레지스터에 쓴다 — W1C 라 1 인 비트가 지워진다 */
				   PCI_EXP_DEVSTA_CED | PCI_EXP_DEVSTA_NFED | /* [한국어] Correctable / Non-Fatal 오류 검출 비트 */
				   PCI_EXP_DEVSTA_FED | PCI_EXP_DEVSTA_URD); /* [한국어] Fatal / Unsupported Request 검출 비트 */
}
#endif

/**
 * pcie_clear_root_pme_status - Clear root port PME interrupt status.
 * @dev: PCIe root port or event collector.
 */
/*
 * [한국어]
 * pcie_clear_root_pme_status - Root Port 의 PME 인터럽트 상태를 지운다.
 *
 * @dev: Root Port 또는 Root Complex Event Collector.
 * @return: 없음.
 *
 * 아래 장치가 PME 를 보내면 Root Port 의 Root Status 레지스터에 PME 비트가
 * 서고 인터럽트가 난다. 그 사건을 처리한 뒤 이 비트를 지우지 않으면
 * 같은 인터럽트가 계속 다시 발생한다.
 * PCI_EXP_RTSTA_PME 역시 W1C 라 1 을 쓰는 것이 지우는 동작이다.
 *
 * 실행 문맥: config space 쓰기. 프로세스 문맥.
 *
 * 호출 체인:
 *   drivers/pci/pcie/pme.c → [이 함수] → pcie_capability_write_dword()
 */
void pcie_clear_root_pme_status(struct pci_dev *dev)
{
	pcie_capability_write_dword(dev, PCI_EXP_RTSTA, PCI_EXP_RTSTA_PME); /* [한국어] Root Status 의 PME 비트에 1 을 써서 지운다. W1C 이므로 다른 비트는 0 을 써도 영향이 없다 */
}

/**
 * pci_check_pme_status - Check if given device has generated PME.
 * @dev: Device to check.
 *
 * Check the PME status of the device and if set, clear it and clear PME enable
 * (if set).  Return 'true' if PME status and PME enable were both set or
 * 'false' otherwise.
 */
/*
 * [한국어]
 * pci_check_pme_status - 이 장치가 PME 를 냈는지 확인하고, 냈다면 흔적을 지운다.
 *
 * @dev: 검사할 PCI 장치.
 * @return: PME Status 와 PME Enable 이 둘 다 켜져 있었으면 true, 아니면 false.
 *
 * PME(Power Management Event)는 절전 중인 장치가 "나를 깨워 달라"고 보내는
 * 신호다. 장치의 PMCSR 안에 두 비트가 있다.
 *   - PME_Status: 실제로 사건이 났다는 표시(W1C).
 *   - PME_Enable: 그 사건을 신호로 내보내도 되는지.
 *
 * 반환값이 true 이려면 둘 다 켜져 있어야 한다. Status 만 켜져 있고 Enable 이
 * 꺼져 있다면, 장치가 사건을 기록만 했을 뿐 깨우기를 요청한 것이 아니므로
 * 흔적만 지우고 false 를 돌려준다.
 *
 * 확인과 동시에 PME_Enable 을 끄는 것이 중요하다(원본 주석의 "interrupt
 * flood"). PME 는 레벨 신호라, Enable 을 켠 채로 두면 장치를 깨워 처리할
 * 때까지 같은 인터럽트가 끝없이 반복된다.
 *
 * 쓰기가 한 번뿐인 것도 요점이다. PME_Status 에 1(지우기)을, PME_Enable 에
 * 0(끄기)을 담아 한 번에 쓴다.
 *
 * 실행 문맥: config space 를 읽고 쓴다. 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_pme_wakeup(), drivers/pci/pcie/pme.c → [이 함수]
 *     → pci_read/write_config_word()
 */
bool pci_check_pme_status(struct pci_dev *dev)
{
	int pmcsr_pos; /* [한국어] PMCSR 의 config space 오프셋 */
	u16 pmcsr; /* [한국어] PMCSR 값 */
	bool ret = false; /* [한국어] "깨우기 요청이었다"의 판정 결과 */

	if (!dev->pm_cap) /* [한국어] PM capability 가 없으면 PME 자체가 불가능하다 */
		return false; /* [한국어] false */

	pmcsr_pos = dev->pm_cap + PCI_PM_CTRL; /* [한국어] PM capability 시작 + PCI_PM_CTRL(4) = PMCSR 위치 */
	pci_read_config_word(dev, pmcsr_pos, &pmcsr); /* [한국어] 현재 값을 읽는다 */
	if (!(pmcsr & PCI_PM_CTRL_PME_STATUS)) /* [한국어] PME_Status 가 꺼져 있으면 사건 자체가 없었다 */
		return false; /* [한국어] false. 아무 것도 쓰지 않고 돌아간다 */

	/* Clear PME status. */
	pmcsr |= PCI_PM_CTRL_PME_STATUS; /* [한국어] W1C 이므로 1 을 세워 두면 아래 쓰기에서 이 비트가 지워진다 */
	if (pmcsr & PCI_PM_CTRL_PME_ENABLE) { /* [한국어] PME_Enable 도 켜져 있었다면 이것은 진짜 깨우기 요청이다 */
		/* Disable PME to avoid interrupt flood. */
		pmcsr &= ~PCI_PM_CTRL_PME_ENABLE; /* [한국어] Enable 을 끈다. PME 는 레벨 신호라 켜 둔 채로는 같은 인터럽트가 끝없이 반복된다 */
		ret = true; /* [한국어] 호출자에게 "깨워야 한다"고 알린다 */
	}

	pci_write_config_word(dev, pmcsr_pos, pmcsr); /* [한국어] 한 번의 쓰기로 Status 를 지우고 Enable 을 끈다 */

	return ret; /* [한국어] true 면 호출자가 장치를 깨운다 */
}

/**
 * pci_pme_wakeup - Wake up a PCI device if its PME Status bit is set.
 * @dev: Device to handle.
 * @pme_poll_reset: Whether or not to reset the device's pme_poll flag.
 *
 * Check if @dev has generated PME and queue a resume request for it in that
 * case.
 */
/*
 * [한국어]
 * pci_pme_wakeup - PME 를 낸 장치에 resume 요청을 거는 pci_walk_bus 콜백.
 *
 * @dev: 검사할 장치.
 * @pme_poll_reset: NULL 이 아니면 이 장치의 pme_poll 플래그를 내린다.
 *                  포인터로 받는 것은 pci_walk_bus 콜백의 시그니처가 void * 이기 때문이다.
 * @return: 항상 0 — pci_walk_bus 규약상 0 이 아니면 순회가 중단되므로,
 *          모든 장치를 훑기 위해 언제나 0 이다.
 *
 * pme_poll 은 "이 장치의 PME 인터럽트를 믿을 수 없어 1초마다 폴링해야 한다"는
 * 표시다. 그런데 실제로 인터럽트가 도착했다면 폴링이 필요 없다는 것이
 * 증명된 셈이므로, 그때 이 플래그를 내려 불필요한 폴링을 그만둔다.
 * pme_poll_reset 이 참인 호출은 인터럽트 경로에서 오는 것이라 그 정리를 함께 한다.
 *
 * 실제 깨우기는 pm_request_resume() 으로 요청만 걸고 즉시 돌아온다 —
 * pci_walk_bus 가 버스 락을 쥔 채 콜백을 부르므로 여기서 잠들면 안 된다.
 *
 * 실행 문맥: pci_walk_bus 콜백. 잠들지 않는다.
 *
 * 호출 체인:
 *   pci_pme_wakeup_bus() → pci_walk_bus() → [이 함수],
 *   pci_pme_list_scan() → [이 함수]
 *     → pci_check_pme_status(), pci_wakeup_event(), pm_request_resume()
 */
static int pci_pme_wakeup(struct pci_dev *dev, void *pme_poll_reset)
{
	if (pme_poll_reset && dev->pme_poll) /* [한국어] 인터럽트로 PME 를 받은 경로라면 폴링이 필요 없다는 증거다 */
		dev->pme_poll = false; /* [한국어] 폴링 플래그를 내려 pci_pme_list_scan() 이 이 장치를 리스트에서 빼게 한다 */

	if (pci_check_pme_status(dev)) { /* [한국어] PME Status 와 Enable 이 둘 다 켜져 있었으면 진짜 깨우기 요청이다 */
		pci_wakeup_event(dev); /* [한국어] 전원 관리 코어에 깨우기 사건을 보고한다 — 이 계수가 시스템 절전을 취소시키기도 한다 */
		pm_request_resume(&dev->dev); /* [한국어] 비동기 resume 을 요청한다. 여기서 직접 깨우면 버스 락을 쥔 채 잠들게 된다 */
	}
	return 0; /* [한국어] 0 을 돌려 순회를 계속하게 한다 */
}

/**
 * pci_pme_wakeup_bus - Walk given bus and wake up devices on it, if necessary.
 * @bus: Top bus of the subtree to walk.
 */
/*
 * [한국어]
 * pci_pme_wakeup_bus - 서브트리 전체에서 PME 를 낸 장치를 찾아 깨운다.
 *
 * @bus: 훑을 서브트리의 꼭대기 버스. NULL 이면 아무 것도 하지 않는다.
 * @return: 없음.
 *
 * Root Port 가 PME 인터럽트를 받아도 "어느 장치가 보냈는지"는 알려 주지
 * 않는다. PME 는 여러 장치가 공유하는 신호이기 때문이다. 그래서 그 아래
 * 서브트리를 모두 훑으며 각 장치의 PME Status 를 직접 확인한다.
 *
 * 콜백에 (void *)true 를 넘겨 pme_poll 플래그를 내리게 한다 — 인터럽트가
 * 실제로 왔다는 것은 이 장치에 폴링이 필요 없다는 증거이기 때문이다.
 *
 * 실행 문맥: pci_walk_bus 가 pci_bus_sem 을 잡고 순회한다. 프로세스 문맥.
 *
 * 호출 체인:
 *   drivers/pci/pcie/pme.c 의 인터럽트 처리 → [이 함수]
 *     → pci_walk_bus() → pci_pme_wakeup()
 */
void pci_pme_wakeup_bus(struct pci_bus *bus)
{
	if (bus) /* [한국어] NULL 버스 방어 */
		pci_walk_bus(bus, pci_pme_wakeup, (void *)true); /* [한국어] (void *)true 는 "pme_poll 플래그를 내려라"는 신호다. 콜백 시그니처가 void * 라 이렇게 실어 보낸다 */
}


/**
 * pci_pme_capable - check the capability of PCI device to generate PME#
 * @dev: PCI device to handle.
 * @state: PCI state from which device will issue PME#.
 */
/*
 * [한국어]
 * pci_pme_capable - 이 장치가 지정한 D-state 에서 PME 를 낼 수 있는가.
 *
 * @dev: 검사할 장치.
 * @state: 확인할 D-state(PCI_D0 ~ PCI_D3cold).
 * @return: 그 상태에서 PME 를 낼 수 있으면 true.
 *
 * 장치마다 "어느 전원 상태에서 PME 를 낼 수 있는가"가 다르다. PM capability 의
 * PMC 레지스터에 상태별 지원 비트가 있고, 열거 때 pci_pm_init() 이 그것을
 * dev->pme_support 비트맵으로 옮겨 둔다. 비트 위치가 곧 D-state 번호다.
 *
 * 이 판정이 중요한 이유는, PME 를 낼 수 없는 깊은 상태로 장치를 내려보내면
 * 그 장치로는 시스템을 깨울 수 없게 되기 때문이다. pci_target_state() 가
 * 목표 상태를 고를 때 이 함수로 후보를 거른다.
 *
 * 실행 문맥: 캐시된 비트맵만 읽는다. 어느 문맥에서도 안전하다.
 *
 * 호출 체인:
 *   pci_target_state(), pci_enable_wake() 경로 → [이 함수]
 */
bool pci_pme_capable(struct pci_dev *dev, pci_power_t state)
{
	if (!dev->pm_cap) /* [한국어] PM capability 가 없으면 PME 자체가 불가능하다 */
		return false; /* [한국어] false */

	return !!(dev->pme_support & (1 << state)); /* [한국어] 비트 위치가 곧 D-state 번호다. !! 로 0/1 의 bool 로 정규화한다 */
}
EXPORT_SYMBOL(pci_pme_capable);

/*
 * [한국어]
 * pci_pme_list_scan - PME 폴링 대상 장치들을 1초마다 훑는 지연 워크 핸들러.
 *
 * @work: 워크큐가 넘겨 주는 work_struct. 여기서는 쓰지 않는다.
 * @return: 없음.
 *
 * PME 신호를 제대로 내지 않는 하드웨어를 위한 대비책이다. 인터럽트를 기다리는
 * 대신 절전 중인 장치들의 PMCSR 을 직접 읽어 PME Status 를 확인한다.
 *
 * config space 를 안전하게 읽으려면 두 조건이 갖춰져야 한다.
 *   - 상위 브리지가 D0 여야 한다. 브리지가 자고 있으면 그 아래 config 접근이
 *     닿지 않거나 값이 불안정하다(원본 주석).
 *   - 장치 자신은 절전 중이되 D3cold 는 아니어야 한다. D3cold 는 전원이 없어
 *     config 를 읽을 수 없고, 깨어 있는 장치는 폴링할 이유가 없다.
 *
 * 브리지 참조를 pm_runtime_get_if_active() 로 잡는 것이 요점이다. "이미
 * 깨어 있으면 참조를 올리고, 자고 있으면 0 을 돌려준다" — 폴링하자고
 * 브리지를 깨우면 절전이 무의미해지므로, 자고 있으면 그냥 건너뛴다.
 *
 * pme_poll 이 내려간 항목은 리스트에서 빼고 해제한다. 실제 인터럽트가 온
 * 적이 있어 폴링이 필요 없다고 판명된 장치다. 그래서 순회에
 * list_for_each_entry_safe 를 써야 한다 — 순회 도중 항목을 지우기 때문이다.
 *
 * 리스트가 비지 않았으면 자기 자신을 1초 뒤로 다시 예약한다.
 * system_freezable_wq 를 쓰는 이유는 시스템 절전에 들어갈 때 이 워크가
 * 함께 멈춰야 하기 때문이다.
 *
 * 실행 문맥: 워크큐(프로세스 문맥). pci_pme_list_mutex 를 쥔 채 config 를
 * 읽으므로 잠들 수 있는 문맥이어야 한다.
 *
 * 호출 체인:
 *   워크큐 → [이 함수] → pm_runtime_get_if_active(), pci_pme_wakeup()
 */
static void pci_pme_list_scan(struct work_struct *work)
{
	struct pci_pme_device *pme_dev, *n; /* [한국어] 순회 커서와, 순회 중 삭제를 견디기 위한 예비 포인터 */

	mutex_lock(&pci_pme_list_mutex); /* [한국어] 리스트를 보호한다. 아래에서 config 접근으로 잠들 수 있어 스핀락이 아닌 뮤텍스다 */
	list_for_each_entry_safe(pme_dev, n, &pci_pme_list, list) { /* [한국어] _safe 판을 쓰는 이유는 아래에서 항목을 지우기 때문이다 */
		struct pci_dev *pdev = pme_dev->dev; /* [한국어] 폴링할 실제 장치 */

		if (pdev->pme_poll) { /* [한국어] 아직 폴링이 필요한 장치인가 */
			struct pci_dev *bridge = pdev->bus->self; /* [한국어] 상위 브리지(루트 버스 직결이면 NULL) */
			struct device *dev = &pdev->dev; /* [한국어] 런타임 PM API 에 넘길 struct device */
			struct device *bdev = bridge ? &bridge->dev : NULL; /* [한국어] 브리지의 struct device. 브리지가 없으면 NULL */
			int bref = 0; /* [한국어] 브리지 참조를 잡았는지 기록. 0 이면 잡지 않았다는 뜻이라 나중에 놓지 않는다 */

			/*
			 * If we have a bridge, it should be in an active/D0
			 * state or the configuration space of subordinate
			 * devices may not be accessible or stable over the
			 * course of the call.
			 */
			if (bdev) { /* [한국어] 브리지가 있으면 그 상태부터 확인한다 */
				bref = pm_runtime_get_if_active(bdev); /* [한국어] 이미 깨어 있을 때만 참조를 올린다. 폴링하자고 브리지를 깨우면 절전이 무의미해진다 */
				if (!bref) /* [한국어] 브리지가 자고 있으면 */
					continue; /* [한국어] 이 장치는 이번 회차에 건너뛴다 */

				if (bridge->current_state != PCI_D0) /* [한국어] 참조는 잡혔지만 PCI 관점의 상태가 D0 가 아니면 config 접근이 불안정하다 */
					goto put_bridge; /* [한국어] 참조를 놓고 다음 장치로 */
			}

			/*
			 * The device itself should be suspended but config
			 * space must be accessible, therefore it cannot be in
			 * D3cold.
			 */
			if (pm_runtime_suspended(dev) && /* [한국어] 장치가 런타임 절전 중이고 — 깨어 있으면 폴링할 이유가 없다 */
			    pdev->current_state != PCI_D3cold) /* [한국어] D3cold 가 아니어야 한다. 전원이 없으면 PMCSR 을 읽을 수 없다 */
				pci_pme_wakeup(pdev, NULL); /* [한국어] PME Status 를 확인하고, 났다면 깨우기를 요청한다. NULL 은 "pme_poll 플래그는 건드리지 말라"는 뜻이다 */

put_bridge: /* [한국어] 브리지 참조를 놓기 위한 공통 출구 */
			if (bref > 0) /* [한국어] 참조를 실제로 잡았을 때만 */
				pm_runtime_put(bdev); /* [한국어] 놓는다. 그래야 브리지가 다시 절전에 들어갈 수 있다 */
		} else {
			list_del(&pme_dev->list); /* [한국어] 폴링 리스트에서 뺀다 */
			kfree(pme_dev); /* [한국어] 항목 메모리를 해제한다. _safe 순회라 삭제해도 안전하다 */
		}
	}
	if (!list_empty(&pci_pme_list)) /* [한국어] 폴링할 장치가 남아 있으면 */
		queue_delayed_work(system_freezable_wq, &pci_pme_work, /* [한국어] 1초 뒤 자기 자신을 다시 예약한다. freezable 워크큐라 시스템 절전 시 함께 멈춘다 */
				   msecs_to_jiffies(PME_TIMEOUT)); /* [한국어] PME_TIMEOUT(1000ms)을 jiffies 로 바꾼다 */
	mutex_unlock(&pci_pme_list_mutex); /* [한국어] 리스트 보호를 푼다 */
}

/*
 * [한국어]
 * __pci_pme_active - 장치의 PMCSR 에서 PME# 발생을 켜거나 끈다.
 *
 * @dev: 대상 PCI 장치.
 * @enable: true 면 PME 발생 허용, false 면 금지.
 * @return: 없음.
 *
 * 하드웨어 레지스터만 만지는 저수준 부분이다. 폴링 리스트 관리는 상위의
 * pci_pme_active() 가 맡는다.
 *
 * 쓰기 한 번에 두 가지를 한다는 점이 요점이다. PME_Status 비트는 W1C 라
 * 언제나 1 을 세워 둬 묵은 상태를 지우고, PME_Enable 은 @enable 에 따라
 * 켜거나 끈다. 켜기 전에 상태를 지우지 않으면, 예전에 남은 Status 때문에
 * 켜자마자 가짜 PME 로 판정될 수 있다.
 *
 * 실행 문맥: config space 를 읽고 쓴다. 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_pme_active() → [이 함수] → pci_read/write_config_word()
 */
static void __pci_pme_active(struct pci_dev *dev, bool enable)
{
	u16 pmcsr; /* [한국어] PMCSR 값 */

	if (!dev->pme_support) /* [한국어] 어느 상태에서도 PME 를 못 내는 장치면 건드릴 것이 없다 */
		return; /* [한국어] 끝 */

	pci_read_config_word(dev, dev->pm_cap + PCI_PM_CTRL, &pmcsr); /* [한국어] 현재 PMCSR 을 읽는다 — 전원 상태 비트 등 다른 필드를 보존하기 위해서다 */
	/* Clear PME_Status by writing 1 to it and enable PME# */
	pmcsr |= PCI_PM_CTRL_PME_STATUS | PCI_PM_CTRL_PME_ENABLE; /* [한국어] Status 는 언제나 1 을 세워 묵은 흔적을 지우고, Enable 은 일단 켠 값으로 둔다 */
	if (!enable) /* [한국어] 끄라는 요청이면 */
		pmcsr &= ~PCI_PM_CTRL_PME_ENABLE; /* [한국어] Enable 만 다시 내린다. Status 지우기는 그대로 남는다 */

	pci_write_config_word(dev, dev->pm_cap + PCI_PM_CTRL, pmcsr); /* [한국어] 한 번의 쓰기로 상태 지우기와 활성/비활성을 함께 적용한다 */
}

/**
 * pci_pme_restore - Restore PME configuration after config space restore.
 * @dev: PCI device to update.
 */
/*
 * [한국어]
 * pci_pme_restore - config space 복원 뒤 PME 설정을 소프트웨어 기대값에 맞춘다.
 *
 * @dev: 대상 PCI 장치.
 * @return: 없음.
 *
 * pci_restore_state() 는 config space 사본을 그대로 되쓰는데, 그 사본에 담긴
 * PME 설정이 "지금 원하는" 설정과 다를 수 있다. 사본을 뜬 뒤에 깨우기 설정이
 * 바뀌었을 수 있기 때문이다. 그래서 복원 후 dev->wakeup_prepared 를 기준으로
 * 다시 맞춘다.
 *
 * 두 갈래의 처리가 대칭적이다.
 *   - 깨우기를 원하는 상태(wakeup_prepared): Enable 을 켜고 Status 는 건드리지
 *     않는다(W1C 라 0 을 쓰면 보존된다). 복원 도중 실제로 도착한 PME 를
 *     잃지 않기 위해서다.
 *   - 원하지 않는 상태: Enable 을 끄고 Status 에 1 을 써 지운다. 어차피
 *     깨우지 않을 것이므로 묵은 흔적을 남길 이유가 없다.
 *
 * 실행 문맥: config space 를 읽고 쓴다. 프로세스 문맥.
 *
 * 호출 체인:
 *   pci-driver.c 의 resume 경로 → [이 함수] → pci_read/write_config_word()
 */
void pci_pme_restore(struct pci_dev *dev)
{
	u16 pmcsr; /* [한국어] PMCSR 값 */

	if (!dev->pme_support) /* [한국어] PME 를 낼 수 없는 장치면 맞출 것이 없다 */
		return; /* [한국어] 끝 */

	pci_read_config_word(dev, dev->pm_cap + PCI_PM_CTRL, &pmcsr); /* [한국어] 복원 직후의 PMCSR 을 읽는다 */
	if (dev->wakeup_prepared) { /* [한국어] 소프트웨어가 "이 장치로 깨어날 것"이라 기대하는 상태라면 */
		pmcsr |= PCI_PM_CTRL_PME_ENABLE; /* [한국어] PME 발생을 켠다 */
		pmcsr &= ~PCI_PM_CTRL_PME_STATUS; /* [한국어] Status 비트는 0 으로 남긴다 — W1C 라 0 을 쓰면 지워지지 않아, 그 사이 실제로 도착한 PME 를 잃지 않는다 */
	} else {
		pmcsr &= ~PCI_PM_CTRL_PME_ENABLE; /* [한국어] PME 발생을 끄고 */
		pmcsr |= PCI_PM_CTRL_PME_STATUS; /* [한국어] Status 에 1 을 세워 묵은 흔적을 지운다 */
	}
	pci_write_config_word(dev, dev->pm_cap + PCI_PM_CTRL, pmcsr); /* [한국어] 맞춘 값을 써 넣는다 */
}

/**
 * pci_pme_active - enable or disable PCI device's PME# function
 * @dev: PCI device to handle.
 * @enable: 'true' to enable PME# generation; 'false' to disable it.
 *
 * The caller must verify that the device is capable of generating PME# before
 * calling this function with @enable equal to 'true'.
 */
/*
 * [한국어]
 * pci_pme_active - PME# 발생을 켜거나 끄고, 필요하면 폴링 리스트도 관리한다.
 *
 * @dev: 대상 PCI 장치.
 * @enable: true 면 PME 발생 허용, false 면 금지.
 * @return: 없음.
 *
 * 하드웨어 설정(__pci_pme_active)과 소프트웨어 폴링 리스트 관리를 함께 한다.
 *
 * 폴링이 필요한 이유는 원본 주석이 길게 설명한다. 재래식 PCI 는 PME# 라는
 * 물리적 신호선이 제대로 배선돼 있어야 하는데 그러지 않은 보드가 있고,
 * PCIe 는 신호선 대신 메시지를 쓰지만 Status 비트만 세우고 메시지를 보내지
 * 않는 장치나, 메시지를 받고도 인터럽트를 내지 않는 Root Port 가 실재한다.
 * 그래서 PCIe 에서도 폴링이 남아 있다. 어차피 시스템은 다른 이유로도 자주
 * 깨어나므로 1초 주기 폴링의 비용보다 절전 이득이 크다는 판단이다.
 *
 * dev->pme_poll 이 참인 장치만 리스트에 넣는다. 켤 때는 항목을 만들어 리스트
 * 앞에 붙이고, 그것이 리스트의 유일한 항목이라면(= 방금 비어 있던 리스트가
 * 살아났다면) 폴링 워크를 시작한다. 끌 때는 리스트에서 찾아 빼고 해제한다.
 *
 * 원본 주석의 경고대로, enable=true 로 부르기 전에 호출자가
 * pci_pme_capable() 로 지원 여부를 확인해야 한다.
 *
 * 실행 문맥: kmalloc 과 뮤텍스를 쓴다. 프로세스 문맥.
 *
 * 호출 체인:
 *   __pci_enable_wake() → [이 함수]
 *     → __pci_pme_active(), kmalloc_obj(), queue_delayed_work()
 */
void pci_pme_active(struct pci_dev *dev, bool enable)
{
	__pci_pme_active(dev, enable); /* [한국어] 먼저 하드웨어 쪽 PME Enable 비트를 맞춘다 */

	/*
	 * PCI (as opposed to PCIe) PME requires that the device have
	 * its PME# line hooked up correctly. Not all hardware vendors
	 * do this, so the PME never gets delivered and the device
	 * remains asleep. The easiest way around this is to
	 * periodically walk the list of suspended devices and check
	 * whether any have their PME flag set. The assumption is that
	 * we'll wake up often enough anyway that this won't be a huge
	 * hit, and the power savings from the devices will still be a
	 * win.
	 *
	 * Although PCIe uses in-band PME message instead of PME# line
	 * to report PME, PME does not work for some PCIe devices in
	 * reality.  For example, there are devices that set their PME
	 * status bits, but don't really bother to send a PME message;
	 * there are PCI Express Root Ports that don't bother to
	 * trigger interrupts when they receive PME messages from the
	 * devices below.  So PME poll is used for PCIe devices too.
	 */

	if (dev->pme_poll) { /* [한국어] 인터럽트를 믿을 수 없어 폴링이 필요하다고 표시된 장치만 리스트로 관리한다 */
		struct pci_pme_device *pme_dev; /* [한국어] 리스트에 넣을 항목 */
		if (enable) { /* [한국어] 켜는 경우 */
			pme_dev = kmalloc_obj(struct pci_pme_device); /* [한국어] 항목 하나를 잡는다 */
			if (!pme_dev) { /* [한국어] 할당에 실패하면 */
				pci_warn(dev, "can't enable PME#\n"); /* [한국어] 폴링을 못 하므로 PME 가 동작하지 않을 수 있음을 알린다 */
				return; /* [한국어] 하드웨어 설정은 이미 했으므로 그대로 돌아간다 */
			}
			pme_dev->dev = dev; /* [한국어] 어느 장치를 폴링할지 기록한다 */
			mutex_lock(&pci_pme_list_mutex); /* [한국어] 리스트를 보호한다 */
			list_add(&pme_dev->list, &pci_pme_list); /* [한국어] 리스트 앞에 붙인다 */
			if (list_is_singular(&pci_pme_list)) /* [한국어] 이 항목이 유일하다면 방금 전까지 리스트가 비어 있었다는 뜻이다 */
				queue_delayed_work(system_freezable_wq, /* [한국어] 폴링 워크를 새로 시작한다. 이미 항목이 있었다면 워크가 이미 돌고 있어 다시 예약하지 않는다 */
						   &pci_pme_work, /* [한국어] 예약할 지연 워크 */
						   msecs_to_jiffies(PME_TIMEOUT)); /* [한국어] 1초 뒤 */
			mutex_unlock(&pci_pme_list_mutex); /* [한국어] 리스트 보호를 푼다 */
		} else {
			mutex_lock(&pci_pme_list_mutex); /* [한국어] 리스트를 보호한다 */
			list_for_each_entry(pme_dev, &pci_pme_list, list) { /* [한국어] 해당 장치의 항목을 찾는다 */
				if (pme_dev->dev == dev) { /* [한국어] 같은 장치를 가리키는 항목이면 */
					list_del(&pme_dev->list); /* [한국어] 리스트에서 뺀다 */
					kfree(pme_dev); /* [한국어] 항목 메모리를 해제한다 */
					break; /* [한국어] 한 장치에 항목은 하나뿐이므로 더 찾지 않는다 */
				}
			}
			mutex_unlock(&pci_pme_list_mutex); /* [한국어] 리스트 보호를 푼다 */
		}
	}

	pci_dbg(dev, "PME# %s\n", enable ? "enabled" : "disabled"); /* [한국어] 최종 상태를 디버그 로그로 남긴다 */
}
EXPORT_SYMBOL(pci_pme_active);

/**
 * __pci_enable_wake - enable PCI device as wakeup event source
 * @dev: PCI device affected
 * @state: PCI state from which device will issue wakeup events
 * @enable: True to enable event generation; false to disable
 *
 * This enables the device as a wakeup event source, or disables it.
 * When such events involves platform-specific hooks, those hooks are
 * called automatically by this routine.
 *
 * Devices with legacy power management (no standard PCI PM capabilities)
 * always require such platform hooks.
 *
 * RETURN VALUE:
 * 0 is returned on success
 * -EINVAL is returned if device is not supposed to wake up the system
 * Error code depending on the platform is returned if both the platform and
 * the native mechanism fail to enable the generation of wake-up events
 */
/*
 * [한국어]
 * __pci_enable_wake - 이 장치를 깨우기 이벤트 원천으로 켜거나 끈다.
 *
 * @dev: 대상 PCI 장치.
 * @state: 어느 D-state 에서 깨우기 신호를 낼 것인지.
 * @enable: true 면 허용, false 면 금지.
 * @return: 0 성공. 장치도 플랫폼도 깨우기를 켜지 못하면 그 오류 코드.
 *
 * 깨우기는 두 층이 모두 열려야 성립한다.
 *   - 장치 쪽: PMCSR 의 PME Enable(pci_pme_active).
 *   - 플랫폼 쪽: ACPI 의 깨우기 경로(platform_pci_set_wakeup).
 * 원본 주석이 인용하는 "PCI System Architecture" 의 지침대로, 켤 때는
 * PME# 를 먼저 켜고 ACPI 를 나중에 켠다. 끌 때는 대칭이 되도록 반대 순서로
 * 플랫폼을 먼저 끈다.
 *
 * D3cold 를 함께 보는 조건이 미묘하다. 목표 상태에서 PME 를 못 내더라도
 * D3cold 에서 낼 수 있다면 PME 를 켜 둔다 — 원본 주석대로, 위쪽 계층이
 * D3cold 로 내려가면 이 장치도 덩달아 D3cold 가 되고, 그때 깨울 수 있어야
 * 하기 때문이다.
 *
 * 반환값 계산이 특이하다. 장치 쪽을 켜지 못했을 때만(ret=1) 플랫폼 쪽
 * 결과를 최종 결과로 삼는다. 즉 "둘 중 하나라도 성공하면 성공"이다.
 *
 * 실행 문맥: ACPI 조작과 config 접근. 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_enable_wake(), pci_finish_runtime_suspend() → [이 함수]
 *     → pci_pme_capable(), pci_pme_active(), platform_pci_set_wakeup()
 */
static int __pci_enable_wake(struct pci_dev *dev, pci_power_t state, bool enable)
{
	int ret = 0; /* [한국어] 반환값. 0 이 성공이며, 아래에서 장치 쪽 실패를 1 로 표시했다가 플랫폼 결과로 덮는다 */

	/*
	 * Bridges that are not power-manageable directly only signal
	 * wakeup on behalf of subordinate devices which is set up
	 * elsewhere, so skip them. However, bridges that are
	 * power-manageable may signal wakeup for themselves (for example,
	 * on a hotplug event) and they need to be covered here.
	 */
	if (!pci_power_manageable(dev)) /* [한국어] 전원 관리가 불가능한 브리지는 자기 자신이 아니라 아래 장치를 대신해 신호할 뿐이고, 그 설정은 다른 곳에서 한다(원본 주석) */
		return 0; /* [한국어] 할 일이 없으므로 성공으로 끝낸다 */

	/* Don't do the same thing twice in a row for one device. */
	if (!!enable == !!dev->wakeup_prepared) /* [한국어] 요청과 현재 상태가 같으면 — !! 로 양쪽을 0/1 로 정규화해 비교한다 */
		return 0; /* [한국어] 중복 호출이므로 아무 것도 하지 않는다. ACPI 쪽에는 참조 계수가 있어 두 번 켜면 균형이 깨진다 */

	/*
	 * According to "PCI System Architecture" 4th ed. by Tom Shanley & Don
	 * Anderson we should be doing PME# wake enable followed by ACPI wake
	 * enable.  To disable wake-up we call the platform first, for symmetry.
	 */

	if (enable) { /* [한국어] 켜는 경우 */
		int error; /* [한국어] 플랫폼 쪽 결과를 따로 받는다 */

		/*
		 * Enable PME signaling if the device can signal PME from
		 * D3cold regardless of whether or not it can signal PME from
		 * the current target state, because that will allow it to
		 * signal PME when the hierarchy above it goes into D3cold and
		 * the device itself ends up in D3cold as a result of that.
		 */
		if (pci_pme_capable(dev, state) || pci_pme_capable(dev, PCI_D3cold)) /* [한국어] 목표 상태에서 PME 를 낼 수 있거나, D3cold 에서 낼 수 있으면 — 후자는 위 계층이 D3cold 로 내려갈 때를 대비한 것이다 */
			pci_pme_active(dev, true); /* [한국어] 장치 쪽 PME 를 켠다 */
		else
			ret = 1; /* [한국어] 장치 쪽은 실패로 표시해 둔다. 플랫폼 쪽이 성공하면 아래에서 덮인다 */
		error = platform_pci_set_wakeup(dev, true); /* [한국어] 플랫폼 쪽 깨우기 경로를 연다 */
		if (ret) /* [한국어] 장치 쪽이 실패했을 때만 */
			ret = error; /* [한국어] 플랫폼 쪽 결과를 최종 결과로 삼는다. 둘 중 하나라도 되면 깨울 수 있다 */
		if (!ret) /* [한국어] 어느 한쪽이라도 성공했으면 */
			dev->wakeup_prepared = true; /* [한국어] 깨우기 준비 완료로 표시한다. pci_pme_restore() 가 이 플래그를 기준으로 복원한다 */
	} else {
		platform_pci_set_wakeup(dev, false); /* [한국어] 대칭을 위해 플랫폼 쪽을 먼저 끈다 */
		pci_pme_active(dev, false); /* [한국어] 그 다음 장치 쪽 PME 를 끈다 */
		dev->wakeup_prepared = false; /* [한국어] 표시를 내린다 */
	}

	return ret; /* [한국어] 0 이면 깨우기 설정이 성립했다 */
}

/**
 * pci_enable_wake - change wakeup settings for a PCI device
 * @pci_dev: Target device
 * @state: PCI state from which device will issue wakeup events
 * @enable: Whether or not to enable event generation
 *
 * If @enable is set, check device_may_wakeup() for the device before calling
 * __pci_enable_wake() for it.
 */
/*
 * [한국어]
 * pci_enable_wake - 사용자 정책(device_may_wakeup)을 확인한 뒤 깨우기를 설정한다.
 *
 * @pci_dev: 대상 PCI 장치.
 * @state: 어느 D-state 에서 깨우기 신호를 낼 것인지.
 * @enable: true 면 허용, false 면 금지.
 * @return: 0 성공, -EINVAL 이면 이 장치로 깨우는 것이 허용돼 있지 않다.
 *
 * __pci_enable_wake() 앞에 정책 검사를 한 겹 두른 공개 API 다.
 * device_may_wakeup() 은 사용자가 sysfs 의 power/wakeup 로 켜고 끄는 설정을
 * 본다. 하드웨어가 할 수 있어도 사용자가 원하지 않으면 켜지 않는다.
 * 끄는 요청에는 검사를 하지 않는다 — 끄는 것은 언제나 허용된다.
 *
 * 실행 문맥: 프로세스 문맥.
 *
 * 호출 체인:
 *   각 드라이버, pci_prepare_to_sleep(), pci_wake_from_d3() → [이 함수]
 *     → device_may_wakeup(), __pci_enable_wake()
 */
int pci_enable_wake(struct pci_dev *pci_dev, pci_power_t state, bool enable)
{
	if (enable && !device_may_wakeup(&pci_dev->dev)) /* [한국어] 켜려는데 사용자 정책이 이 장치의 깨우기를 허용하지 않으면 */
		return -EINVAL; /* [한국어] 거절한다. 끄는 요청에는 이 검사가 적용되지 않는다 */

	return __pci_enable_wake(pci_dev, state, enable); /* [한국어] 실제 설정은 본체에 맡긴다 */
}
EXPORT_SYMBOL(pci_enable_wake);

/**
 * pci_wake_from_d3 - enable/disable device to wake up from D3_hot or D3_cold
 * @dev: PCI device to prepare
 * @enable: True to enable wake-up event generation; false to disable
 *
 * Many drivers want the device to wake up the system from D3_hot or D3_cold
 * and this function allows them to set that up cleanly - pci_enable_wake()
 * should not be called twice in a row to enable wake-up due to PCI PM vs ACPI
 * ordering constraints.
 *
 * This function only returns error code if the device is not allowed to wake
 * up the system from sleep or it is not capable of generating PME# from both
 * D3_hot and D3_cold and the platform is unable to enable wake-up power for it.
 */
/*
 * [한국어]
 * pci_wake_from_d3 - D3hot 또는 D3cold 에서 깨어나도록 설정한다.
 *
 * @dev: 대상 PCI 장치.
 * @enable: true 면 허용, false 면 금지.
 * @return: 0 성공, 음수면 깨우기를 설정할 수 없다.
 *
 * 드라이버가 흔히 원하는 것은 "가장 깊은 절전에서도 깨어나게 해 달라"인데,
 * 그 깊이가 장치마다 다르다. D3cold 에서 PME 를 낼 수 있으면 D3cold 를,
 * 아니면 D3hot 을 기준으로 설정한다.
 *
 * 원본 주석이 경고하듯 pci_enable_wake() 를 연달아 두 번 불러 두 상태를
 * 모두 켜려 해서는 안 된다 — PCI PM 과 ACPI 사이의 순서 제약 때문이다.
 * 이 함수는 둘 중 하나만 고르게 해 그 실수를 막는다.
 *
 * 실행 문맥: 프로세스 문맥.
 *
 * 호출 체인:
 *   각 드라이버의 suspend 경로 → [이 함수]
 *     → pci_pme_capable(), pci_enable_wake()
 */
int pci_wake_from_d3(struct pci_dev *dev, bool enable)
{
	return pci_pme_capable(dev, PCI_D3cold) ? /* [한국어] D3cold 에서 PME 를 낼 수 있는가에 따라 */
			pci_enable_wake(dev, PCI_D3cold, enable) : /* [한국어] 낼 수 있으면 D3cold 기준으로 설정하고 */
			pci_enable_wake(dev, PCI_D3hot, enable); /* [한국어] 없으면 D3hot 기준으로 설정한다 */
}
EXPORT_SYMBOL(pci_wake_from_d3);

/**
 * pci_target_state - find an appropriate low power state for a given PCI dev
 * @dev: PCI device
 * @wakeup: Whether or not wakeup functionality will be enabled for the device.
 *
 * Use underlying platform code to find a supported low power state for @dev.
 * If the platform can't manage @dev, return the deepest state from which it
 * can generate wake events, based on any available PME info.
 */
/*
 * [한국어]
 * pci_target_state - 이 장치를 내려보낼 적절한 저전원 상태를 고른다.
 *
 * @dev: 대상 PCI 장치.
 * @wakeup: 이 장치로 시스템을 깨울 계획인지. 참이면 PME 를 낼 수 있는 상태만 고른다.
 * @return: 목표 D-state.
 *
 * 결정 순서가 곧 우선순위다.
 *
 *   1) 플랫폼이 전원을 관리할 수 있으면 플랫폼에 물어본다. 답을 알 수 없거나
 *      (PCI_POWER_ERROR/PCI_UNKNOWN) 지원하지 않는 D1/D2 를 답하면 D3hot 으로
 *      되돌린다.
 *   2) 플랫폼이 관리하지 않는데 이미 D3cold 라면, 표준 밖의 방법으로 전원이
 *      끊긴 것이다. 원본 주석대로 그대로 재우는 편이 낫다.
 *   3) PM capability 조차 없으면 저전원 상태로 갈 방법이 없어 D0 다.
 *   4) 깨우기가 필요하면, PME 를 낼 수 있는 가장 깊은 상태를 찾는다.
 *      D3hot 부터 하나씩 얕아지며 pme_support 비트맵을 확인한다.
 *      D0 에서만 PME 를 낼 수 있다면 D0 를 돌려준다 — 깨울 수 있는 것이
 *      깊이보다 중요하기 때문이다.
 *   5) 아무 제약이 없으면 D3hot.
 *
 * NVMe 와의 관계: NVMe 컨트롤러가 D3hot 으로 내려갈지, 아니면 자체 APST 로
 * 얕은 상태에 머무를지는 drivers/nvme/host/pci.c 가 따로 판단한다. 이 함수는
 * PCI 층의 D-state 만 고른다.
 *
 * 실행 문맥: ACPI 조회를 포함한다. 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_prepare_to_sleep(), pci_finish_runtime_suspend(), pci_dev_run_wake()
 *     → [이 함수] → platform_pci_choose_state(), pci_no_d1d2()
 */
static pci_power_t pci_target_state(struct pci_dev *dev, bool wakeup)
{
	if (platform_pci_power_manageable(dev)) { /* [한국어] 플랫폼이 이 장치의 전원을 다룰 수 있으면 플랫폼의 판단을 우선한다 */
		/*
		 * Call the platform to find the target state for the device.
		 */
		pci_power_t state = platform_pci_choose_state(dev); /* [한국어] ACPI 의 _SxD 등을 조회해 권고 상태를 받는다 */

		switch (state) { /* [한국어] 받은 답을 검증한다 */
		case PCI_POWER_ERROR: /* [한국어] 플랫폼이 판단하지 못했거나 */
		case PCI_UNKNOWN: /* [한국어] 상태를 알 수 없다고 답한 경우 */
			return PCI_D3hot; /* [한국어] 표준적으로 안전한 D3hot 을 쓴다 */

		case PCI_D1: /* [한국어] D1 을 권했지만 */
		case PCI_D2: /* [한국어] D2 를 권했지만 */
			if (pci_no_d1d2(dev)) /* [한국어] 장치나 상위 브리지가 D1/D2 를 지원하지 않으면 */
				return PCI_D3hot; /* [한국어] D3hot 으로 대체한다 */
		}

		return state; /* [한국어] 플랫폼의 권고를 그대로 쓴다 */
	}

	/*
	 * If the device is in D3cold even though it's not power-manageable by
	 * the platform, it may have been powered down by non-standard means.
	 * Best to let it slumber.
	 */
	if (dev->current_state == PCI_D3cold) /* [한국어] 플랫폼이 관리하지 않는데 이미 D3cold 라면 표준 밖의 방법으로 전원이 끊긴 것이다 */
		return PCI_D3cold; /* [한국어] 그대로 재워 둔다 */
	else if (!dev->pm_cap) /* [한국어] PM capability 가 없으면 */
		return PCI_D0; /* [한국어] 저전원 상태로 갈 수단 자체가 없다 */

	if (wakeup && dev->pme_support) { /* [한국어] 깨우기가 필요하고 PME 를 낼 수 있는 상태가 하나라도 있으면 */
		pci_power_t state = PCI_D3hot; /* [한국어] 가장 깊은 후보인 D3hot 부터 시작한다 */

		/*
		 * Find the deepest state from which the device can generate
		 * PME#.
		 */
		while (state && !(dev->pme_support & (1 << state))) /* [한국어] pme_support 비트맵에서 해당 상태 비트를 확인하며 */
			state--; /* [한국어] 점점 얕은 상태로 내려온다. 비트 위치가 곧 D-state 번호다 */

		if (state) /* [한국어] D0 보다 깊은 상태에서 PME 를 낼 수 있으면 */
			return state; /* [한국어] 그 상태를 쓴다 */
		else if (dev->pme_support & 1) /* [한국어] D0(비트 0)에서만 PME 를 낼 수 있다면 */
			return PCI_D0; /* [한국어] 절전을 포기하고 D0 에 머문다. 깨울 수 있는 것이 절전보다 중요하다 */
	}

	return PCI_D3hot; /* [한국어] 깨우기가 필요 없으면 가장 깊은 표준 상태로 내려간다 */
}

/**
 * pci_prepare_to_sleep - prepare PCI device for system-wide transition
 *			  into a sleep state
 * @dev: Device to handle.
 *
 * Choose the power state appropriate for the device depending on whether
 * it can wake up the system and/or is power manageable by the platform
 * (PCI_D3hot is the default) and put the device into that state.
 */
/*
 * [한국어]
 * pci_prepare_to_sleep - 시스템 절전 진입 전에 장치를 목표 상태로 내려보낸다.
 *
 * @dev: 대상 PCI 장치.
 * @return: 0 성공, -EIO 면 목표 상태를 정할 수 없었고, 그 밖의 음수는 전이 실패.
 *
 * 순서가 중요하다. 깨우기를 먼저 설정하고 그 다음 전원을 내린다 —
 * 이미 잠든 장치의 PMCSR 을 건드릴 수는 없기 때문이다.
 * 전이에 실패하면 깨우기 설정을 되돌린다. 켜지도 못한 장치를 깨우기 원천으로
 * 남겨 두면 ACPI 쪽 참조 계수가 어긋난다.
 *
 * 실행 문맥: 시스템 절전 경로. 프로세스 문맥.
 *
 * 호출 체인:
 *   pci-driver.c 의 시스템 suspend 콜백 → [이 함수]
 *     → pci_target_state(), pci_enable_wake(), pci_set_power_state()
 */
int pci_prepare_to_sleep(struct pci_dev *dev)
{
	bool wakeup = device_may_wakeup(&dev->dev); /* [한국어] 사용자가 이 장치로 깨우는 것을 허용했는가 */
	pci_power_t target_state = pci_target_state(dev, wakeup); /* [한국어] 그 정책까지 반영해 목표 상태를 고른다 */
	int error; /* [한국어] 전이 결과 */

	if (target_state == PCI_POWER_ERROR) /* [한국어] 목표 상태를 정할 수 없었다면 */
		return -EIO; /* [한국어] 절전 진입을 실패로 알린다 */

	pci_enable_wake(dev, target_state, wakeup); /* [한국어] 전원을 내리기 전에 깨우기를 설정한다 — 잠든 뒤에는 PMCSR 을 건드릴 수 없다 */

	error = pci_set_power_state(dev, target_state); /* [한국어] 목표 상태로 내려보낸다 */

	if (error) /* [한국어] 전이에 실패했으면 */
		pci_enable_wake(dev, target_state, false); /* [한국어] 깨우기 설정을 되돌린다. 그러지 않으면 ACPI 쪽 참조 계수가 어긋난다 */

	return error; /* [한국어] 전이 결과를 그대로 올린다 */
}
EXPORT_SYMBOL(pci_prepare_to_sleep);

/**
 * pci_back_from_sleep - turn PCI device on during system-wide transition
 *			 into working state
 * @dev: Device to handle.
 *
 * Disable device's system wake-up capability and put it into D0.
 */
/*
 * [한국어]
 * pci_back_from_sleep - 절전에서 돌아와 D0 로 올리고 깨우기 설정을 정리한다.
 *
 * @dev: 대상 PCI 장치.
 * @return: 0 성공, 음수면 D0 전이 실패.
 *
 * pci_prepare_to_sleep() 의 짝이다. 순서가 반대인 것에 이유가 있다 —
 * 먼저 D0 로 올려야 PMCSR 에 접근할 수 있고, 그래야 깨우기 설정을 끌 수 있다.
 * 깨어난 뒤에는 더 이상 깨우기 원천일 필요가 없으므로 끄는 것이 맞다.
 *
 * 실행 문맥: 프로세스 문맥.
 *
 * 호출 체인:
 *   pci-driver.c 의 시스템 resume 콜백 → [이 함수]
 *     → pci_set_power_state(), pci_enable_wake()
 */
int pci_back_from_sleep(struct pci_dev *dev)
{
	int ret = pci_set_power_state(dev, PCI_D0); /* [한국어] 먼저 D0 로 올린다. 그래야 PMCSR 을 건드릴 수 있다 */

	if (ret) /* [한국어] 전이에 실패하면 */
		return ret; /* [한국어] 깨우기 설정을 건드릴 수 없으므로 그대로 오류를 올린다 */

	pci_enable_wake(dev, PCI_D0, false); /* [한국어] 깨어났으므로 깨우기 원천 설정을 끈다 */
	return 0; /* [한국어] 복귀 완료 */
}
EXPORT_SYMBOL(pci_back_from_sleep);

/**
 * pci_finish_runtime_suspend - Carry out PCI-specific part of runtime suspend.
 * @dev: PCI device being suspended.
 *
 * Prepare @dev to generate wake-up events at run time and put it into a low
 * power state.
 */
/*
 * [한국어]
 * pci_finish_runtime_suspend - 런타임 절전의 PCI 쪽 마무리를 수행한다.
 *
 * @dev: 절전에 들어가는 PCI 장치.
 * @return: 0 성공, -EIO 면 목표 상태를 정할 수 없었고, 그 밖의 음수는 전이 실패.
 *
 * pci_prepare_to_sleep() 과 구조는 같지만 깨우기 판단 기준이 다르다.
 * 시스템 절전은 사용자 정책(device_may_wakeup)을 따르지만, 런타임 절전은
 * 그것과 무관하게 "장치가 스스로 깨어나 일을 계속할 수 있어야" 한다.
 * 그래서 목표 상태는 device_can_wakeup()(하드웨어 능력)으로 고르고,
 * 실제 깨우기 설정은 pci_dev_run_wake()(런타임 깨우기가 실제로 가능한가)로 정한다.
 * 사용자 정책 검사를 건너뛰려고 pci_enable_wake() 대신 __pci_enable_wake() 를
 * 직접 부르는 것도 같은 이유다.
 *
 * 실행 문맥: 런타임 PM 경로. 프로세스 문맥.
 *
 * 호출 체인:
 *   pci-driver.c 의 런타임 suspend 콜백 → [이 함수]
 *     → pci_target_state(), __pci_enable_wake(), pci_set_power_state()
 */
int pci_finish_runtime_suspend(struct pci_dev *dev)
{
	pci_power_t target_state; /* [한국어] 고를 목표 D-state */
	int error; /* [한국어] 전이 결과 */

	target_state = pci_target_state(dev, device_can_wakeup(&dev->dev)); /* [한국어] 하드웨어가 깨우기를 지원하는지를 기준으로 상태를 고른다. 사용자 정책이 아니라 능력이 기준이다 */
	if (target_state == PCI_POWER_ERROR) /* [한국어] 상태를 정할 수 없으면 */
		return -EIO; /* [한국어] 절전을 포기한다 */

	__pci_enable_wake(dev, target_state, pci_dev_run_wake(dev)); /* [한국어] 런타임 깨우기가 실제로 가능한 경우에만 켠다. 사용자 정책 검사를 건너뛰려고 __ 판을 직접 부른다 */

	error = pci_set_power_state(dev, target_state); /* [한국어] 목표 상태로 내려보낸다 */

	if (error) /* [한국어] 실패하면 */
		pci_enable_wake(dev, target_state, false); /* [한국어] 깨우기 설정을 되돌린다 */

	return error; /* [한국어] 전이 결과 */
}

/**
 * pci_dev_run_wake - Check if device can generate run-time wake-up events.
 * @dev: Device to check.
 *
 * Return true if the device itself is capable of generating wake-up events
 * (through the platform or using the native PCIe PME) or if the device supports
 * PME and one of its upstream bridges can generate wake-up events.
 */
/*
 * [한국어]
 * pci_dev_run_wake - 런타임 절전 중에도 이 장치가 시스템을 깨울 수 있는가.
 *
 * @dev: 검사할 장치.
 * @return: 깨울 수 있으면 true.
 *
 * 조건을 세 겹으로 확인한다.
 *   1) 장치가 PME 를 낼 수 있어야 한다(pme_support).
 *   2) 실제로 내려갈 목표 상태에서 낼 수 있어야 한다. 원칙적으로 PME 가
 *      가능해도 그 깊이에서 못 내면 소용없다(원본 주석).
 *   3) 그 PME 신호를 받아 줄 주체가 있어야 한다. 장치 자신이 깨우기 원천으로
 *      허용돼 있거나, 위쪽 브리지 중 하나가 그 역할을 할 수 있어야 한다.
 *      브리지가 대신 깨울 수 있는 이유는, PCIe 의 PME 가 상위로 전파되는
 *      메시지이기 때문이다.
 *
 * 3)의 순회는 루트 버스까지 올라간다. 루트에 닿으면 마지막으로 호스트
 * 브리지의 struct device 를 확인한다 — ACPI 가 깨우기 능력을 그쪽에 붙여
 * 두는 경우가 있다.
 *
 * 실행 문맥: 버스 계층을 락 없이 거슬러 올라간다. 계층 구조가 안정된
 * 문맥에서 불러야 한다.
 *
 * 호출 체인:
 *   pci_finish_runtime_suspend(), pci_dev_complete_resume(), pci-driver.c
 *     → [이 함수] → pci_pme_capable(), pci_target_state(), device_can_wakeup()
 */
bool pci_dev_run_wake(struct pci_dev *dev)
{
	struct pci_bus *bus = dev->bus; /* [한국어] 위로 거슬러 올라갈 버스 커서 */

	if (!dev->pme_support) /* [한국어] 어느 상태에서도 PME 를 못 내면 */
		return false; /* [한국어] 깨울 수 없다 */

	/* PME-capable in principle, but not from the target power state */
	if (!pci_pme_capable(dev, pci_target_state(dev, true))) /* [한국어] 런타임 절전에서 실제로 내려갈 상태를 계산해, 그 상태에서 PME 를 낼 수 있는지 본다 */
		return false; /* [한국어] 그 깊이에서 못 내면 소용없다 */

	if (device_can_wakeup(&dev->dev)) /* [한국어] 장치 자신이 깨우기 원천으로 허용돼 있으면 */
		return true; /* [한국어] 그것으로 충분하다 */

	while (bus->parent) { /* [한국어] 아니면 위쪽 브리지 중 대신 깨워 줄 수 있는 것을 찾는다. parent 가 없으면 루트 버스다 */
		struct pci_dev *bridge = bus->self; /* [한국어] 이 버스를 만든 브리지 */

		if (device_can_wakeup(&bridge->dev)) /* [한국어] 그 브리지가 깨우기 원천이 될 수 있으면 */
			return true; /* [한국어] PME 메시지가 그 브리지까지 전파되므로 깨울 수 있다 */

		bus = bus->parent; /* [한국어] 한 단계 위로 올라간다 */
	}

	/* We have reached the root bus. */
	if (bus->bridge) /* [한국어] 루트 버스의 호스트 브리지 device — ACPI 가 깨우기 능력을 여기 붙여 두는 경우가 있다 */
		return device_can_wakeup(bus->bridge); /* [한국어] 그 능력을 그대로 답으로 삼는다 */

	return false; /* [한국어] 루트까지 올라가도 깨워 줄 주체가 없다 */
}
EXPORT_SYMBOL_GPL(pci_dev_run_wake);

/**
 * pci_dev_need_resume - Check if it is necessary to resume the device.
 * @pci_dev: Device to check.
 *
 * Return 'true' if the device is not runtime-suspended or it has to be
 * reconfigured due to wakeup settings difference between system and runtime
 * suspend, or the current power state of it is not suitable for the upcoming
 * (system-wide) transition.
 */
/*
 * [한국어]
 * pci_dev_need_resume - 시스템 절전에 들어가기 전에 이 장치를 반드시 깨워야 하는가.
 *
 * @pci_dev: 검사할 장치.
 * @return: 깨워야 하면 true.
 *
 * 런타임 절전 중인 장치를 시스템 절전에 들어갈 때 굳이 깨우지 않고 그대로
 * 두는 최적화(direct-complete)가 있다. 이 함수는 그 최적화를 적용해도
 * 되는지 판정한다.
 *
 * 깨워야 하는 경우는 세 가지다.
 *   1) 애초에 런타임 절전 중이 아니다 — 깨울 것도 없이 이미 깨어 있다.
 *   2) 플랫폼이 상태 불일치를 알린다(platform_pci_need_resume).
 *   3) 지금 상태가 시스템 절전에 적합하지 않다. 런타임 절전과 시스템 절전은
 *      깨우기 정책이 달라 목표 상태가 다를 수 있기 때문이다.
 *
 * 3)의 판정에서 D3cold 와 D3hot 을 예외로 두는 것이 요점이다. 원본 주석대로
 * D3cold 는 D3hot 위에 전원 차단이 더해진 것뿐이라, 앞의 플랫폼 검사가
 * 걸리지 않았다면 굳이 깨워 다시 설정할 필요가 없다.
 *
 * 실행 문맥: 시스템 절전 준비 단계. 프로세스 문맥.
 *
 * 호출 체인:
 *   pci-driver.c 의 pci_pm_prepare 계열 → [이 함수]
 *     → pm_runtime_suspended(), platform_pci_need_resume(), pci_target_state()
 */
bool pci_dev_need_resume(struct pci_dev *pci_dev)
{
	struct device *dev = &pci_dev->dev; /* [한국어] 런타임 PM API 에 넘길 struct device */
	pci_power_t target_state; /* [한국어] 시스템 절전에서 목표로 삼을 상태 */

	if (!pm_runtime_suspended(dev) || platform_pci_need_resume(pci_dev)) /* [한국어] 런타임 절전 중이 아니거나, 플랫폼이 상태 불일치를 알리면 */
		return true; /* [한국어] 깨워야 한다 */

	target_state = pci_target_state(pci_dev, device_may_wakeup(dev)); /* [한국어] 시스템 절전 기준(사용자 정책 포함)으로 목표 상태를 다시 계산한다 */

	/*
	 * If the earlier platform check has not triggered, D3cold is just power
	 * removal on top of D3hot, so no need to resume the device in that
	 * case.
	 */
	return target_state != pci_dev->current_state && /* [한국어] 목표가 현재와 다르고 */
		target_state != PCI_D3cold && /* [한국어] 목표가 D3cold 가 아니며 — D3cold 는 D3hot 위에 전원 차단만 더한 것이라 다시 설정할 필요가 없다 */
		pci_dev->current_state != PCI_D3hot; /* [한국어] 현재가 D3hot 도 아닐 때만 깨운다 */
}

/**
 * pci_dev_adjust_pme - Adjust PME setting for a suspended device.
 * @pci_dev: Device to check.
 *
 * If the device is suspended and it is not configured for system wakeup,
 * disable PME for it to prevent it from waking up the system unnecessarily.
 *
 * Note that if the device's power state is D3cold and the platform check in
 * pci_dev_need_resume() has not triggered, the device's configuration need not
 * be changed.
 */
/*
 * [한국어]
 * pci_dev_adjust_pme - 시스템을 깨울 필요가 없는 절전 장치의 PME 를 꺼 둔다.
 *
 * @pci_dev: 대상 장치.
 * @return: 없음.
 *
 * 런타임 절전 중인 장치는 자기 일을 재개하려고 PME 를 켜 두었을 수 있다.
 * 그러나 시스템 절전에 들어갈 때 사용자가 이 장치로 시스템을 깨우기를
 * 원하지 않는다면, 그 PME 가 시스템을 불필요하게 깨우게 된다. 그래서 꺼 둔다.
 *
 * D3cold 를 제외하는 이유는 두 가지다. 전원이 없어 PMCSR 을 쓸 수 없고,
 * 원본 주석대로 pci_dev_need_resume() 의 플랫폼 검사가 걸리지 않았다면
 * 설정을 바꿀 필요도 없다.
 *
 * 실행 문맥: dev->power.lock 스핀락을 인터럽트를 막고 잡는다. 그 락 안에서
 * 런타임 절전 상태가 바뀌지 않도록 보장하면서 PMCSR 을 만진다.
 *
 * 호출 체인:
 *   pci-driver.c 의 시스템 suspend 준비 단계 → [이 함수] → __pci_pme_active()
 */
void pci_dev_adjust_pme(struct pci_dev *pci_dev)
{
	struct device *dev = &pci_dev->dev; /* [한국어] 런타임 PM 상태를 볼 struct device */

	spin_lock_irq(&dev->power.lock); /* [한국어] 상태 확인과 PMCSR 조작 사이에 런타임 절전 상태가 바뀌지 않도록 막는다. 인터럽트 문맥에서도 이 락을 잡으므로 _irq 판이 필요하다 */

	if (pm_runtime_suspended(dev) && !device_may_wakeup(dev) && /* [한국어] 런타임 절전 중이고, 사용자가 이 장치로 시스템을 깨우기를 원하지 않으며 */
	    pci_dev->current_state < PCI_D3cold) /* [한국어] D3cold 가 아니어야 한다 — 전원이 없으면 PMCSR 을 쓸 수 없다 */
		__pci_pme_active(pci_dev, false); /* [한국어] PME 를 꺼 시스템이 불필요하게 깨어나지 않게 한다 */

	spin_unlock_irq(&dev->power.lock); /* [한국어] 락을 푼다 */
}

/**
 * pci_dev_complete_resume - Finalize resume from system sleep for a device.
 * @pci_dev: Device to handle.
 *
 * If the device is runtime suspended and wakeup-capable, enable PME for it as
 * it might have been disabled during the prepare phase of system suspend if
 * the device was not configured for system wakeup.
 */
/*
 * [한국어]
 * pci_dev_complete_resume - 시스템 절전에서 돌아온 뒤 꺼 두었던 PME 를 되살린다.
 *
 * @pci_dev: 대상 장치.
 * @return: 없음.
 *
 * pci_dev_adjust_pme() 의 짝이다. 시스템 절전 준비 단계에서 껐던 PME 를
 * 다시 켜, 런타임 절전 중인 장치가 스스로 깨어날 수 있게 되돌린다.
 * 그러지 않으면 그 장치는 영영 깨어나지 못한 채 남는다.
 *
 * 먼저 pci_dev_run_wake() 로 "런타임 깨우기가 원래 가능한 장치인가"를
 * 확인한다. 애초에 불가능했다면 켤 이유가 없다.
 *
 * 실행 문맥: dev->power.lock 을 인터럽트를 막고 잡는다.
 *
 * 호출 체인:
 *   pci-driver.c 의 시스템 resume 완료 단계 → [이 함수]
 *     → pci_dev_run_wake(), __pci_pme_active()
 */
void pci_dev_complete_resume(struct pci_dev *pci_dev)
{
	struct device *dev = &pci_dev->dev; /* [한국어] 런타임 PM 상태를 볼 struct device */

	if (!pci_dev_run_wake(pci_dev)) /* [한국어] 애초에 런타임 깨우기가 불가능한 장치면 */
		return; /* [한국어] 되살릴 것이 없다 */

	spin_lock_irq(&dev->power.lock); /* [한국어] 상태 확인과 PMCSR 조작 사이에 상태가 바뀌지 않도록 막는다 */

	if (pm_runtime_suspended(dev) && pci_dev->current_state < PCI_D3cold) /* [한국어] 여전히 런타임 절전 중이고 D3cold 가 아니면 — 전원이 있어야 PMCSR 을 쓸 수 있다 */
		__pci_pme_active(pci_dev, true); /* [한국어] PME 를 다시 켜 스스로 깨어날 수 있게 한다 */

	spin_unlock_irq(&dev->power.lock); /* [한국어] 락을 푼다 */
}

/**
 * pci_choose_state - Choose the power state of a PCI device.
 * @dev: Target PCI device.
 * @state: Target state for the whole system.
 *
 * Returns PCI power state suitable for @dev and @state.
 */
/*
 * [한국어]
 * pci_choose_state - 시스템 절전 이벤트에 대응하는 이 장치의 D-state 를 고른다.
 *
 * @dev: 대상 PCI 장치.
 * @state: 시스템 전체의 목표 상태를 담은 pm_message_t.
 * @return: 이 장치에 적합한 D-state.
 *
 * PM_EVENT_ON 은 "시스템을 켜진 상태로 되돌린다"는 뜻이므로 장치도 D0 다.
 * 그 밖의 이벤트(SUSPEND, HIBERNATE 등)는 모두 절전이라 pci_target_state() 에
 * 판단을 맡긴다.
 *
 * wakeup=false 로 넘기는 것이 특징이다. 즉 이 함수가 고르는 상태는 깨우기
 * 능력을 고려하지 않은, 순수한 절전 관점의 목표다. 깨우기까지 감안한 선택은
 * pci_prepare_to_sleep() 이 따로 한다.
 *
 * 실행 문맥: 프로세스 문맥.
 *
 * 호출 체인:
 *   각 드라이버의 suspend 콜백 → [이 함수] → pci_target_state()
 */
pci_power_t pci_choose_state(struct pci_dev *dev, pm_message_t state)
{
	if (state.event == PM_EVENT_ON) /* [한국어] 시스템을 다시 켜는 이벤트면 */
		return PCI_D0; /* [한국어] 장치도 완전 동작 상태여야 한다 */

	return pci_target_state(dev, false); /* [한국어] 그 밖의 절전 이벤트는 목표 상태 계산에 맡긴다. wakeup=false 라 깨우기 능력은 고려하지 않는다 */
}
EXPORT_SYMBOL(pci_choose_state);

/*
 * [한국어]
 * pci_config_pm_runtime_get - config space 를 건드리는 동안 장치가 잠들지 않게 붙든다.
 *
 * @pdev: 대상 PCI 장치.
 * @return: 없음.
 *
 * sysfs 등으로 config space 를 읽고 쓰는 동안 장치가 런타임 절전에 들어가면
 * 접근이 실패하거나 엉뚱한 값을 읽는다. 그래서 참조를 잡아 둔다.
 *
 * 처리 순서에 이유가 있다.
 *   1) 부모(대개 상위 브리지)를 먼저 동기적으로 깨운다. 부모가 자고 있으면
 *      자식의 config 에 아예 닿을 수 없기 때문이다.
 *   2) 자기 자신은 참조만 올리고 깨우지는 않는다(_noresume). 아래에서 보듯
 *      D3cold 가 아니면 자고 있어도 config 는 읽히므로, 굳이 깨워
 *      절전 이득을 버릴 이유가 없다.
 *   3) pm_runtime_barrier() 로 진행 중인 절전 작업이 끝나기를 기다린다.
 *      원본 주석대로 current_state 는 절전 도중에 D3cold 로 바뀌므로,
 *      그 작업이 끝나야 아래 검사가 올바른 값을 본다.
 *   4) 그래도 D3cold 라면 그때만 실제로 깨운다.
 *
 * 실행 문맥: pm_runtime_get_sync/resume 이 잠들 수 있다. 프로세스 문맥.
 *
 * 호출 체인:
 *   drivers/pci/pci-sysfs.c 의 config 접근 → [이 함수]
 *     → pm_runtime_get_sync(), pm_runtime_barrier(), pm_runtime_resume()
 */
void pci_config_pm_runtime_get(struct pci_dev *pdev)
{
	struct device *dev = &pdev->dev; /* [한국어] 이 장치의 struct device */
	struct device *parent = dev->parent; /* [한국어] 부모 device — 대개 상위 브리지다 */

	if (parent) /* [한국어] 부모가 있으면 */
		pm_runtime_get_sync(parent); /* [한국어] 동기적으로 깨운다. 부모가 자고 있으면 자식의 config 에 닿을 수 없다 */
	pm_runtime_get_noresume(dev); /* [한국어] 자기 자신은 참조만 올린다. 깨우지 않는 이유는 아래에서 D3cold 인 경우에만 깨우면 되기 때문이다 */
	/*
	 * pdev->current_state is set to PCI_D3cold during suspending,
	 * so wait until suspending completes
	 */
	pm_runtime_barrier(dev); /* [한국어] 진행 중인 절전 작업이 끝나기를 기다린다. 그래야 아래에서 읽는 current_state 가 확정된 값이 된다 */
	/*
	 * Only need to resume devices in D3cold, because config
	 * registers are still accessible for devices suspended but
	 * not in D3cold.
	 */
	if (pdev->current_state == PCI_D3cold) /* [한국어] D3cold 는 전원이 없어 config 에 닿을 수 없다 */
		pm_runtime_resume(dev); /* [한국어] 이때만 실제로 깨운다 */
}

/*
 * [한국어]
 * pci_config_pm_runtime_put - config space 접근이 끝나 참조를 돌려준다.
 *
 * @pdev: 대상 PCI 장치.
 * @return: 없음.
 *
 * pci_config_pm_runtime_get() 의 짝이다. 잡을 때와 반대 순서로 놓는다 —
 * 자식을 먼저 놓고 부모를 나중에 놓는다. 부모를 먼저 놓으면 자식이 아직
 * 참조를 쥔 채로 부모가 잠들려 시도하는 어색한 순간이 생긴다.
 *
 * 부모에만 _sync 를 쓰는 것도 대칭이다. 잡을 때 부모를 동기적으로 깨웠으니
 * 놓을 때도 동기적으로 돌려준다.
 *
 * 실행 문맥: pm_runtime_put_sync 가 잠들 수 있다. 프로세스 문맥.
 *
 * 호출 체인:
 *   drivers/pci/pci-sysfs.c 의 config 접근 → [이 함수]
 *     → pm_runtime_put(), pm_runtime_put_sync()
 */
void pci_config_pm_runtime_put(struct pci_dev *pdev)
{
	struct device *dev = &pdev->dev; /* [한국어] 이 장치의 struct device */
	struct device *parent = dev->parent; /* [한국어] 부모 device */

	pm_runtime_put(dev); /* [한국어] 자식 참조를 먼저 놓는다 */
	if (parent) /* [한국어] 부모가 있으면 */
		pm_runtime_put_sync(parent); /* [한국어] 부모 참조를 동기적으로 돌려준다. 잡을 때 _sync 로 깨운 것과 대칭이다 */
}

/*
 * [한국어] 브리지를 D3 로 내리면 안 되는 것으로 알려진 메인보드 목록(quirk).
 *
 * 스펙상으로는 D3 가 가능해 보여도 실제 하드웨어·펌웨어가 그 전이를 견디지
 * 못하는 보드가 있다. 그런 보드를 DMI(SMBIOS)의 제조사·모델 문자열로 식별해
 * 브리지 전원 관리를 통째로 막는다. pci_bridge_d3_possible() 이 이 표를 본다.
 *
 * 표에 담긴 세 사례가 각각 다른 종류의 고장을 보여 준다.
 *   - Gigabyte X299: Root Port 가 핫플러그 가능으로 표시돼 있지 않아 커널이
 *     전원을 관리해도 된다고 판단하는데, 그러면 BIOS 의 SMI 핸들러가 혼란에
 *     빠진다.
 *   - Elo Continental Z2: Root Port 를 D3cold 로 내렸다 D0 로 되돌리면 그
 *     아래 장치에 더 이상 접근할 수 없다.
 *   - HP Pavilion 17: dGPU 가 붙은 Root Port 의 전원 상태 변경이 실패한다.
 *
 * 표 전체가 CONFIG_X86 안에 있는 이유는 이 보드들이 모두 x86 이기 때문이고,
 * 마지막의 빈 항목 { } 는 dmi_check_system() 이 표의 끝을 알아보는 표식이다.
 */
static const struct dmi_system_id bridge_d3_blacklist[] = {
#ifdef CONFIG_X86 /* [한국어] 아래 항목들이 모두 x86 보드라 그 아키텍처에서만 컴파일한다 */
	{
		/*
		 * Gigabyte X299 root port is not marked as hotplug capable
		 * which allows Linux to power manage it.  However, this
		 * confuses the BIOS SMI handler so don't power manage root
		 * ports on that system.
		 */
		.ident = "X299 DESIGNARE EX-CF", /* [한국어] 로그에 찍힐 이 항목의 이름 */
		.matches = { /* [한국어] DMI 필드가 모두 일치해야 이 보드로 판정한다 */
			DMI_MATCH(DMI_BOARD_VENDOR, "Gigabyte Technology Co., Ltd."), /* [한국어] 보드 제조사 문자열 */
			DMI_MATCH(DMI_BOARD_NAME, "X299 DESIGNARE EX-CF"), /* [한국어] 보드 모델 문자열 */
		},
	},
	{
		/*
		 * Downstream device is not accessible after putting a root port
		 * into D3cold and back into D0 on Elo Continental Z2 board
		 */
		.ident = "Elo Continental Z2", /* [한국어] 로그에 찍힐 이 항목의 이름 */
		.matches = { /* [한국어] 세 필드가 모두 맞아야 한다 */
			DMI_MATCH(DMI_BOARD_VENDOR, "Elo Touch Solutions"), /* [한국어] 보드 제조사 */
			DMI_MATCH(DMI_BOARD_NAME, "Geminilake"), /* [한국어] 보드 모델 */
			DMI_MATCH(DMI_BOARD_VERSION, "Continental Z2"), /* [한국어] 보드 리비전까지 확인한다 — 같은 모델의 다른 리비전은 문제가 없을 수 있다 */
		},
	},
	{
		/*
		 * Changing power state of root port dGPU is connected fails
		 * https://gitlab.freedesktop.org/drm/amd/-/issues/3229
		 */
		.ident = "Hewlett-Packard HP Pavilion 17 Notebook PC/1972", /* [한국어] 로그에 찍힐 이 항목의 이름 */
		.matches = { /* [한국어] 세 필드가 모두 맞아야 한다 */
			DMI_MATCH(DMI_BOARD_VENDOR, "Hewlett-Packard"), /* [한국어] 보드 제조사 */
			DMI_MATCH(DMI_BOARD_NAME, "1972"), /* [한국어] 보드 모델 */
			DMI_MATCH(DMI_BOARD_VERSION, "95.33"), /* [한국어] BIOS/보드 버전 */
		},
	},
#endif
	{ } /* [한국어] 빈 항목이 표의 끝 표식이다. dmi_check_system() 이 이것을 만나면 순회를 멈춘다 */
};

/**
 * pci_bridge_d3_possible - Is it possible to put the bridge into D3
 * @bridge: Bridge to check
 *
 * Currently we only allow D3 for some PCIe ports and for Thunderbolt.
 *
 * Return: Whether it is possible to move the bridge to D3.
 *
 * The return value is guaranteed to be constant across the entire lifetime
 * of the bridge, including its hot-removal.
 */
/*
 * [한국어]
 * pci_bridge_d3_possible - 이 브리지를 런타임에 D3 로 내려도 되는가.
 *
 * @bridge: 검사할 브리지.
 * @return: D3 로 내릴 수 있으면 true.
 *
 * 브리지를 D3 로 내리면 그 아래 계층 전체의 전원이 끊긴다. 절전 효과가
 * 크지만 되살리지 못하면 장치를 통째로 잃으므로, 허용 조건을 여러 겹으로
 * 따진다. 위에서부터 순서대로 판정한다.
 *
 *   - PCIe 포트가 아니면 불가. 재래식 PCI 브리지는 대상이 아니다.
 *   - Root Port / Upstream / Downstream 포트만 후보다.
 *   - pcie_port_pm=off 로 전면 금지했으면 불가.
 *   - 펌웨어가 다루는 핫플러그 포트(ACPI 슬롯 등)는 OS 가 함부로 D3 로
 *     내리면 안 된다.
 *   - PCIe 핫플러그 포트인데 OS 가 직접 다루지 않는 경우(펌웨어가 쥐고 있는
 *     경우)도 마찬가지다.
 *   - pcie_port_pm=force 면 그 뒤 검사를 건너뛰고 허용한다.
 *   - Thunderbolt 는 가장 오래된 2010년 컨트롤러도 D3 를 지원한다(원본 주석).
 *   - 플랫폼(ACPI)이 지원한다고 하면 그 판단을 믿는다.
 *   - OS 가 직접 다루는 PCIe 핫플러그 포트는 불가. 원본 주석대로 2018년까지
 *     OS 지원이 없어 벤더가 런타임 D3 를 검증한 적이 없다.
 *   - DMI 블랙리스트에 걸린 보드는 불가.
 *   - x86 에서는 BIOS 연도가 2015년 이상일 때만 허용한다. 오래된 보드의
 *     펌웨어를 믿을 수 없다는 보수적 판단이다. x86 이 아니면 이 제한이 없다.
 *
 * 원본 주석이 강조하듯 반환값은 브리지의 수명 내내 변하지 않는다.
 * 그래서 열거 시점에 한 번 계산해 두고 쓸 수 있다.
 *
 * NVMe 와의 관계: NVMe SSD 가 붙은 Root Port 가 여기서 true 를 받아야
 * 유휴 시 SSD 전원까지 완전히 끊을 수 있다. 노트북 대기 전력에 직접 영향을 준다.
 *
 * 실행 문맥: DMI 조회와 ACPI 조회를 한다. 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_bridge_d3_update(), probe.c → [이 함수]
 *     → platform_pci_bridge_d3(), dmi_check_system(), dmi_get_bios_year()
 */
bool pci_bridge_d3_possible(struct pci_dev *bridge)
{
	if (!pci_is_pcie(bridge)) /* [한국어] 재래식 PCI 브리지에는 이 정책이 적용되지 않는다 */
		return false; /* [한국어] 불가 */

	switch (pci_pcie_type(bridge)) { /* [한국어] PCIe 포트의 종류에 따라 갈린다 */
	case PCI_EXP_TYPE_ROOT_PORT: /* [한국어] Root Complex 에서 내려오는 첫 포트 */
	case PCI_EXP_TYPE_UPSTREAM: /* [한국어] 스위치의 업스트림 포트 */
	case PCI_EXP_TYPE_DOWNSTREAM: /* [한국어] 스위치의 다운스트림 포트. 이 셋만 후보이며 엔드포인트는 대상이 아니다 */
		if (pci_bridge_d3_disable) /* [한국어] pcie_port_pm=off 로 전면 금지했으면 */
			return false; /* [한국어] 불가 */

		/*
		 * Hotplug ports handled by platform firmware may not be put
		 * into D3 by the OS, e.g. ACPI slots ...
		 */
		if (bridge->is_hotplug_bridge && !bridge->is_pciehp) /* [한국어] 핫플러그 브리지인데 PCIe 네이티브 핫플러그가 아니면 — 펌웨어(ACPI 슬롯 등)가 다루는 포트다 */
			return false; /* [한국어] OS 가 함부로 전원을 끊으면 안 되므로 불가 */

		/* ... or PCIe hotplug ports not handled natively by the OS. */
		if (bridge->is_pciehp && !pciehp_is_native(bridge)) /* [한국어] PCIe 핫플러그 포트이지만 OS 가 직접 다루지 않는(펌웨어가 쥔) 경우 */
			return false; /* [한국어] 역시 불가 */

		if (pci_bridge_d3_force) /* [한국어] pcie_port_pm=force 면 */
			return true; /* [한국어] 뒤의 보수적 검사를 모두 건너뛰고 허용한다 */

		/* Even the oldest 2010 Thunderbolt controller supports D3. */
		if (bridge->is_thunderbolt) /* [한국어] Thunderbolt 컨트롤러라면 */
			return true; /* [한국어] 허용. 가장 오래된 세대도 D3 를 지원한다 */

		/* Platform might know better if the bridge supports D3 */
		if (platform_pci_bridge_d3(bridge)) /* [한국어] 플랫폼 펌웨어가 지원한다고 답하면 */
			return true; /* [한국어] 그 판단을 믿고 허용한다 */

		/*
		 * Hotplug ports handled natively by the OS were not validated
		 * by vendors for runtime D3 at least until 2018 because there
		 * was no OS support.
		 */
		if (bridge->is_pciehp) /* [한국어] OS 가 직접 다루는 PCIe 핫플러그 포트는 — 벤더 검증이 없었다 */
			return false; /* [한국어] 보수적으로 불가 */

		if (dmi_check_system(bridge_d3_blacklist)) /* [한국어] 알려진 문제 보드 목록에 걸리면 */
			return false; /* [한국어] 불가 */

		/*
		 * Out of caution, we only allow PCIe ports from 2015 or newer
		 * into D3 on x86.
		 */
		if (!IS_ENABLED(CONFIG_X86) || dmi_get_bios_year() >= 2015) /* [한국어] x86 이 아니면 이 연도 제한을 적용하지 않고, x86 이면 BIOS 가 2015년 이후일 때만 */
			return true; /* [한국어] 허용한다 */
		break; /* [한국어] 연도 조건을 만족하지 못했으면 switch 를 빠져나가 아래에서 불가로 끝난다 */
	}

	return false; /* [한국어] 위 조건을 통과하지 못했거나 대상 포트 종류가 아니다 */
}

/*
 * [한국어]
 * pci_dev_check_d3cold - 장치 하나가 D3cold 를 막고 있는지 보는 pci_walk_bus 콜백.
 *
 * @dev: 검사할 장치.
 * @data: bool 하나를 가리키는 포인터. 하나라도 막으면 false 로 내린다.
 * @return: 0 이면 순회 계속, 0 이 아니면 순회 중단. 즉 !*d3cold_ok 를 돌려주어
 *          "이미 불가로 판정됐으면 더 볼 것 없이 멈춰라"가 된다.
 *
 * 브리지를 D3cold 로 내리려면 그 아래 모든 장치가 전원 차단을 견뎌야 한다.
 * 하나라도 반대하면 전체가 불가다. 반대 사유는 세 가지다.
 *   - no_d3cold 또는 !d3cold_allowed: 드라이버나 사용자가 이 장치의 D3cold 를
 *     막아 두었다.
 *   - 깨우기 원천으로 설정돼 있는데 D3cold 에서는 PME 를 못 낸다: 전원을 끊으면
 *     깨울 수 없게 되므로 안 된다.
 *   - 전원 관리가 불가능하다: 상태를 다룰 수단이 없다.
 *
 * 값을 조기 반환으로 전파하는 구조라, 반대하는 장치를 하나 만나는 즉시
 * 순회가 끝난다.
 *
 * 실행 문맥: pci_walk_bus 콜백. 잠들지 않는다.
 *
 * 호출 체인:
 *   pci_bridge_d3_update() → pci_walk_bus() → [이 함수]
 *     → pci_pme_capable(), pci_power_manageable()
 */
static int pci_dev_check_d3cold(struct pci_dev *dev, void *data)
{
	bool *d3cold_ok = data; /* [한국어] void * 로 넘어온 판정 결과 포인터 */

	if (/* The device needs to be allowed to go D3cold ... */
	    dev->no_d3cold || !dev->d3cold_allowed || /* [한국어] 드라이버나 사용자가 이 장치의 D3cold 를 막아 두었거나 */

	    /* ... and if it is wakeup capable to do so from D3cold. */
	    (device_may_wakeup(&dev->dev) && /* [한국어] 깨우기 원천으로 설정돼 있는데 */
	     !pci_pme_capable(dev, PCI_D3cold)) || /* [한국어] D3cold 에서는 PME 를 낼 수 없거나 — 전원을 끊으면 깨울 수 없게 된다 */

	    /* If it is a bridge it must be allowed to go to D3. */
	    !pci_power_manageable(dev)) /* [한국어] 전원 관리 자체가 불가능하면 */

		*d3cold_ok = false; /* [한국어] 이 서브트리는 D3cold 로 내릴 수 없다 */

	return !*d3cold_ok; /* [한국어] 불가로 판정됐으면 0 이 아닌 값이 되어 순회가 즉시 멈춘다 */
}

/*
 * pci_bridge_d3_update - Update bridge D3 capabilities
 * @dev: PCI device which is changed
 *
 * Update upstream bridge PM capabilities accordingly depending on if the
 * device PM configuration was changed or the device is being removed.  The
 * change is also propagated upstream.
 */
/*
 * [한국어]
 * pci_bridge_d3_update - 자식의 변화에 맞춰 상위 브리지의 D3 허용 여부를 다시 계산한다.
 *
 * @dev: 설정이 바뀌었거나 제거되는 장치.
 * @return: 없음.
 *
 * 브리지의 D3 허용 여부는 아래 모든 장치의 사정에 달려 있다. 자식 하나가
 * 추가·변경·제거될 때마다 다시 계산해야 한다. 계산 비용을 아끼려고 세 가지
 * 지름길을 쓴다.
 *
 *   1) 제거인데 브리지가 이미 D3 허용 상태라면 다시 계산할 필요가 없다.
 *      장치를 빼는 것은 제약을 없애는 방향이라 결과가 바뀔 수 없다.
 *   2) 추가·변경이면 그 장치 하나만 먼저 본다. 이미 허용 상태였다면
 *      불허로 바뀌는 원인은 방금 바뀐 그 장치뿐이기 때문이다.
 *   3) 그래도 판정이 나지 않고 브리지가 현재 불허 상태라면, 형제들 중
 *      누가 막고 있는지 알 수 없으므로 서브트리 전체를 훑는다.
 *
 * 결과가 달라졌으면 브리지의 플래그를 갱신하고 그 위쪽으로도 같은 계산을
 * 재귀로 전파한다 — 손자의 변화가 할아버지 브리지의 판정까지 바꿀 수 있다.
 *
 * 실행 문맥: pci_walk_bus 를 부르므로 프로세스 문맥. 재귀 깊이는 계층 깊이만큼이다.
 *
 * 호출 체인:
 *   pci_d3cold_enable(), pci_d3cold_disable(), 장치 추가·제거 경로
 *     → [이 함수] → pci_dev_check_d3cold(), pci_walk_bus(), [이 함수](재귀)
 */
void pci_bridge_d3_update(struct pci_dev *dev)
{
	bool remove = !device_is_registered(&dev->dev); /* [한국어] device_is_registered() 가 거짓이면 이 장치는 제거되는 중이다 */
	struct pci_dev *bridge; /* [한국어] 다시 계산할 대상인 상위 브리지 */
	bool d3cold_ok = true; /* [한국어] 낙관적으로 시작해, 반대하는 장치를 만나면 false 로 내린다 */

	bridge = pci_upstream_bridge(dev); /* [한국어] 상위 브리지를 찾는다 */
	if (!bridge || !pci_bridge_d3_possible(bridge)) /* [한국어] 루트 직결이거나 그 브리지가 애초에 D3 대상이 아니면 */
		return; /* [한국어] 계산할 것이 없다 */

	/*
	 * If D3 is currently allowed for the bridge, removing one of its
	 * children won't change that.
	 */
	if (remove && bridge->bridge_d3) /* [한국어] 제거인데 이미 허용 상태라면 — 제약이 하나 사라지는 방향이라 결과가 바뀔 수 없다 */
		return; /* [한국어] 다시 계산하지 않는다 */

	/*
	 * If D3 is currently allowed for the bridge and a child is added or
	 * changed, disallowance of D3 can only be caused by that child, so
	 * we only need to check that single device, not any of its siblings.
	 *
	 * If D3 is currently not allowed for the bridge, checking the device
	 * first may allow us to skip checking its siblings.
	 */
	if (!remove) /* [한국어] 추가·변경이면 */
		pci_dev_check_d3cold(dev, &d3cold_ok); /* [한국어] 바뀐 장치 하나만 먼저 확인한다. 이것만으로 불허가 나면 형제를 훑을 필요가 없다 */

	/*
	 * If D3 is currently not allowed for the bridge, this may be caused
	 * either by the device being changed/removed or any of its siblings,
	 * so we need to go through all children to find out if one of them
	 * continues to block D3.
	 */
	if (d3cold_ok && !bridge->bridge_d3) /* [한국어] 그 장치는 문제없는데 브리지가 아직 불허 상태라면, 다른 형제가 막고 있는지 확인해야 한다 */
		pci_walk_bus(bridge->subordinate, pci_dev_check_d3cold, /* [한국어] 서브트리 전체를 훑는다. 콜백이 0 이 아닌 값을 돌려주면 그 지점에서 순회가 멈춘다 */
			     &d3cold_ok); /* [한국어] 판정 결과를 받을 변수의 주소 */

	if (bridge->bridge_d3 != d3cold_ok) { /* [한국어] 계산 결과가 지금 값과 다르면 */
		bridge->bridge_d3 = d3cold_ok; /* [한국어] 브리지의 플래그를 갱신하고 */
		/* Propagate change to upstream bridges */
		pci_bridge_d3_update(bridge); /* [한국어] 위쪽 브리지에도 같은 계산을 전파한다. 손자의 변화가 할아버지의 판정까지 바꿀 수 있다 */
	}
}

/**
 * pci_d3cold_enable - Enable D3cold for device
 * @dev: PCI device to handle
 *
 * This function can be used in drivers to enable D3cold from the device
 * they handle.  It also updates upstream PCI bridge PM capabilities
 * accordingly.
 */
/*
 * [한국어]
 * pci_d3cold_enable - 이 장치의 D3cold 진입 금지를 푼다.
 *
 * @dev: 대상 PCI 장치.
 * @return: 없음.
 *
 * 드라이버가 "이제 이 장치는 전원이 끊겨도 괜찮다"고 알리는 창구다.
 * 플래그를 바꾸는 것만으로는 부족하다 — 상위 브리지의 D3 허용 여부가
 * 이 장치의 사정에 달려 있었을 수 있으므로 그것도 다시 계산해야 한다.
 *
 * 이미 허용 상태면 아무 것도 하지 않는다. 불필요한 재계산과 재귀 전파를
 * 피하기 위해서다.
 *
 * 실행 문맥: pci_bridge_d3_update() 가 버스를 순회한다. 프로세스 문맥.
 *
 * 호출 체인:
 *   각 드라이버 → [이 함수] → pci_bridge_d3_update()
 */
void pci_d3cold_enable(struct pci_dev *dev)
{
	if (dev->no_d3cold) { /* [한국어] 지금 금지돼 있을 때만 손댄다 */
		dev->no_d3cold = false; /* [한국어] 금지를 푼다 */
		pci_bridge_d3_update(dev); /* [한국어] 이 변화가 상위 브리지의 판정을 바꿀 수 있으므로 다시 계산한다 */
	}
}
EXPORT_SYMBOL_GPL(pci_d3cold_enable);

/**
 * pci_d3cold_disable - Disable D3cold for device
 * @dev: PCI device to handle
 *
 * This function can be used in drivers to disable D3cold from the device
 * they handle.  It also updates upstream PCI bridge PM capabilities
 * accordingly.
 */
/*
 * [한국어]
 * pci_d3cold_disable - 이 장치의 D3cold 진입을 금지한다.
 *
 * @dev: 대상 PCI 장치.
 * @return: 없음.
 *
 * pci_d3cold_enable() 의 짝이다. 전원이 끊기면 곤란한 상태(예: 장치가
 * 유지해야 할 컨텍스트가 있는 동안)에서 드라이버가 부른다.
 * 이 장치가 막으면 상위 브리지도 D3cold 로 갈 수 없으므로 그 판정을 다시
 * 계산해 위로 전파한다.
 *
 * 실행 문맥: 프로세스 문맥.
 *
 * 호출 체인:
 *   각 드라이버 → [이 함수] → pci_bridge_d3_update()
 */
void pci_d3cold_disable(struct pci_dev *dev)
{
	if (!dev->no_d3cold) { /* [한국어] 지금 허용돼 있을 때만 손댄다 */
		dev->no_d3cold = true; /* [한국어] 금지 플래그를 세운다 */
		pci_bridge_d3_update(dev); /* [한국어] 상위 브리지의 판정을 다시 계산한다 */
	}
}
EXPORT_SYMBOL_GPL(pci_d3cold_disable);

/*
 * [한국어]
 * pci_pm_power_up_and_verify_state - D0 로 올린 뒤 실제 상태를 다시 읽어 캐시를 맞춘다.
 *
 * @pci_dev: 대상 PCI 장치.
 * @return: 없음. pci_power_up() 의 반환값을 일부러 무시한다.
 *
 * pci_power_up() 이 실패하더라도 그 뒤의 pci_update_current_state() 가
 * 실물을 읽어 정확한 상태를 기록한다. 즉 "올릴 수 있으면 올리고, 어쨌든
 * 캐시는 실제와 맞춘다"가 이 함수의 계약이다. 그래서 반환값이 없다.
 *
 * 두 번째 인자 PCI_D0 는 PM capability 가 없어 읽을 방법이 없을 때만 쓰이는
 * 대체값이다. 그런 장치는 늘 켜져 있으므로 D0 로 기록하는 것이 맞다.
 *
 * 실행 문맥: 전원 전이 지연 동안 잠들 수 있다. 프로세스 문맥.
 *
 * 호출 체인:
 *   pci-driver.c 의 resume 경로 → [이 함수]
 *     → pci_power_up(), pci_update_current_state()
 */
void pci_pm_power_up_and_verify_state(struct pci_dev *pci_dev)
{
	pci_power_up(pci_dev); /* [한국어] D0 로 올린다. 반환값은 일부러 무시한다 — 실패해도 아래에서 실제 상태를 읽어 맞춘다 */
	pci_update_current_state(pci_dev, PCI_D0); /* [한국어] 실물을 읽어 캐시를 갱신한다. PCI_D0 는 PM capability 가 없을 때만 쓰이는 대체값이다 */
}

/**
 * pci_pm_init - Initialize PM functions of given PCI device
 * @dev: PCI device to handle.
 */
/*
 * [한국어]
 * pci_pm_init - 열거 중 이 장치의 전원 관리 능력을 조사해 pci_dev 에 기록한다.
 *
 * @dev: 막 열거된 PCI 장치.
 * @return: 없음.
 *
 * PM capability(config space 안의 ID 0x01 구조체)를 찾아 그 안의 PMC
 * (Power Management Capabilities) 레지스터를 읽고, 이후 모든 전원 관리
 * 판단의 근거가 되는 필드들을 채운다.
 *   - dev->pm_cap: PM capability 의 오프셋. 이후 PMCSR 접근이 모두 이 값 기준이다.
 *     0 이면 "PCI PM 을 쓸 수 없는 장치"를 뜻한다.
 *   - dev->d1_support / d2_support: 선택 상태인 D1/D2 의 지원 여부.
 *   - dev->pme_support: 어느 D-state 에서 PME 를 낼 수 있는지의 비트맵.
 *   - dev->d3hot_delay / d3cold_delay: 전이 후 지켜야 할 기본 대기 시간.
 *   - dev->bridge_d3: 이 장치가 브리지라면 D3 로 내릴 수 있는지.
 *
 * PMC 버전 검사가 앞에 있다. 이 코드가 아는 것은 버전 3까지이며, 그보다
 * 높으면 레지스터 배치를 신뢰할 수 없어 PM capability 자체를 없는 것으로
 * 취급하고 poweron 으로 건너뛴다.
 *
 * 마지막의 세 호출이 초기 상태를 정한다.
 *   - pci_pm_power_up_and_verify_state(): 일단 D0 로 올려 놓는다. 부팅 시
 *     펌웨어가 장치를 D3hot 에 둔 채 넘기는 경우가 있기 때문이다.
 *   - pm_runtime_forbid(): 런타임 절전을 기본적으로 막는다. 사용자가
 *     sysfs 의 power/control 로 열어 주어야 켜진다.
 *   - pm_runtime_set_active(): 원본 주석대로, 아직 런타임 PM 을 켜지 않았어도
 *     부모와 공급자가 그 사이 잠들어 버리지 않도록 "동작 중"으로 표시해 둔다.
 *
 * PME 를 발견하면 device_set_wakeup_capable() 로 능력만 알리고 실제 PME 는
 * 꺼 둔다 — 원본 주석대로 켜는 판단은 사용자 공간에 맡긴다.
 *
 * 실행 문맥: 열거 중 프로세스 문맥.
 *
 * 호출 체인:
 *   probe.c 의 pci_device_add() → [이 함수]
 *     → pci_find_capability(), pci_bridge_d3_possible(), pci_pme_active()
 */
void pci_pm_init(struct pci_dev *dev)
{
	int pm; /* [한국어] PM capability 의 오프셋 */
	u16 pmc; /* [한국어] PMC(Power Management Capabilities) 레지스터 값 */

	device_enable_async_suspend(&dev->dev); /* [한국어] 이 장치의 suspend/resume 을 다른 장치와 병렬로 처리해도 된다고 알린다. 절전 진입·복귀 시간이 줄어든다 */
	dev->wakeup_prepared = false; /* [한국어] 아직 깨우기 설정을 하지 않았다 */

	dev->pm_cap = 0; /* [한국어] PM capability 를 못 찾을 경우를 대비해 0 으로 초기화한다. 0 은 "PCI PM 불가"를 뜻한다 */
	dev->pme_support = 0; /* [한국어] PME 지원 비트맵도 비워 둔다 */

	/* find PCI PM capability in list */
	pm = pci_find_capability(dev, PCI_CAP_ID_PM); /* [한국어] 표준 capability 리스트에서 PM(ID 0x01)을 찾는다 */
	if (!pm) /* [한국어] 없으면 이 장치는 PCI PM 을 쓸 수 없다 */
		goto poweron; /* [한국어] 조사할 것이 없으니 전원만 켜고 끝낸다 */
	/* Check device's ability to generate PME# */
	pci_read_config_word(dev, pm + PCI_PM_PMC, &pmc); /* [한국어] PM capability 시작 + PCI_PM_PMC(2) 위치의 PMC 레지스터를 읽는다 */

	if ((pmc & PCI_PM_CAP_VER_MASK) > 3) { /* [한국어] PMC 하위 3비트가 규격 버전이다. 이 코드는 3까지만 안다 */
		pci_err(dev, "unsupported PM cap regs version (%u)\n", /* [한국어] 모르는 버전은 레지스터 배치를 신뢰할 수 없다 */
			pmc & PCI_PM_CAP_VER_MASK); /* [한국어] 몇 번 버전인지 남긴다 */
		goto poweron; /* [한국어] PM capability 가 없는 것처럼 취급하고 건너뛴다 */
	}

	dev->pm_cap = pm; /* [한국어] 이제부터 이 값이 PMCSR 접근의 기준점이 된다. 0 이 아니면 "PCI PM 가능"의 표식이기도 하다 */
	dev->d3hot_delay = PCI_PM_D3HOT_WAIT; /* [한국어] D3hot 을 드나든 뒤 지켜야 할 기본 대기(ms). quirk 가 장치별로 늘릴 수 있다 */
	dev->d3cold_delay = PCI_PM_D3COLD_WAIT; /* [한국어] D3cold 에서 돌아온 뒤의 기본 대기(ms). 전원이 다시 들어오는 것이라 더 길다 */
	dev->bridge_d3 = pci_bridge_d3_possible(dev); /* [한국어] 브리지라면 D3 로 내릴 수 있는지 지금 한 번 계산해 둔다. 이 판정은 수명 내내 바뀌지 않는다 */
	dev->d3cold_allowed = true; /* [한국어] 기본적으로 D3cold 를 허용한다. 드라이버가 pci_d3cold_disable() 로 막을 수 있다 */

	dev->d1_support = false; /* [한국어] D1 은 선택 상태라 기본은 미지원 */
	dev->d2_support = false; /* [한국어] D2 도 마찬가지 */
	if (!pci_no_d1d2(dev)) { /* [한국어] 장치나 상위 브리지에 D1/D2 금지 quirk 가 없을 때만 조사한다 */
		if (pmc & PCI_PM_CAP_D1) /* [한국어] PMC 의 D1 지원 비트 */
			dev->d1_support = true; /* [한국어] 지원 표시 */
		if (pmc & PCI_PM_CAP_D2) /* [한국어] PMC 의 D2 지원 비트 */
			dev->d2_support = true; /* [한국어] 지원 표시 */

		if (dev->d1_support || dev->d2_support) /* [한국어] 둘 중 하나라도 지원하면 */
			pci_info(dev, "supports%s%s\n", /* [한국어] 어느 것을 지원하는지 로그에 남긴다 */
				   dev->d1_support ? " D1" : "", /* [한국어] D1 지원 여부에 따라 문자열을 붙이거나 뺀다 */
				   dev->d2_support ? " D2" : ""); /* [한국어] D2 도 같은 방식 */
	}

	pmc &= PCI_PM_CAP_PME_MASK; /* [한국어] PMC 상위 5비트가 "어느 D-state 에서 PME 를 낼 수 있는가"의 비트맵이다 */
	if (pmc) { /* [한국어] 하나라도 낼 수 있으면 */
		pci_info(dev, "PME# supported from%s%s%s%s%s\n", /* [한국어] 어느 상태에서 가능한지 로그에 남긴다 */
			 (pmc & PCI_PM_CAP_PME_D0) ? " D0" : "", /* [한국어] D0 에서 PME 가능 */
			 (pmc & PCI_PM_CAP_PME_D1) ? " D1" : "", /* [한국어] D1 에서 PME 가능 */
			 (pmc & PCI_PM_CAP_PME_D2) ? " D2" : "", /* [한국어] D2 에서 PME 가능 */
			 (pmc & PCI_PM_CAP_PME_D3hot) ? " D3hot" : "", /* [한국어] D3hot 에서 PME 가능 */
			 (pmc & PCI_PM_CAP_PME_D3cold) ? " D3cold" : ""); /* [한국어] D3cold 에서 PME 가능 — 전원이 끊긴 상태에서도 보조 전원으로 신호를 낸다 */
		dev->pme_support = FIELD_GET(PCI_PM_CAP_PME_MASK, pmc); /* [한국어] 마스크를 제자리로 시프트해 비트 위치가 곧 D-state 번호가 되게 만든다. pci_pme_capable() 이 이 형태를 전제로 조회한다 */
		dev->pme_poll = true; /* [한국어] 기본적으로 폴링 대상으로 표시한다. PME 신호를 제대로 내지 않는 하드웨어가 많아 보수적으로 켜 두고, 실제 인터럽트를 받으면 그때 내린다 */
		/*
		 * Make device's PM flags reflect the wake-up capability, but
		 * let the user space enable it to wake up the system as needed.
		 */
		device_set_wakeup_capable(&dev->dev, true); /* [한국어] "이 장치는 깨울 능력이 있다"고 드라이버 모델에 알린다. 실제 허용 여부는 사용자 공간이 정한다 */
		/* Disable the PME# generation functionality */
		pci_pme_active(dev, false); /* [한국어] 능력만 알리고 실제 PME 발생은 꺼 둔다 — 쓰지도 않을 깨우기로 시스템이 깨어나지 않게 */
	}

poweron: /* [한국어] PM capability 가 없거나 버전이 맞지 않아 조사를 건너뛴 경우도 여기로 온다 */
	pci_pm_power_up_and_verify_state(dev); /* [한국어] 일단 D0 로 올리고 실제 상태를 확인한다. 펌웨어가 장치를 D3hot 에 둔 채 넘기는 경우가 있다 */
	pm_runtime_forbid(&dev->dev); /* [한국어] 런타임 절전을 기본적으로 금지한다. sysfs 의 power/control 로 사용자가 열어 주어야 켜진다 */

	/*
	 * Runtime PM will be enabled for the device when it has been fully
	 * configured, but since its parent and suppliers may suspend in
	 * the meantime, prevent them from doing so by changing the
	 * device's runtime PM status to "active".
	 */
	pm_runtime_set_active(&dev->dev); /* [한국어] 런타임 PM 을 아직 켜지 않았더라도 "동작 중"으로 표시해 둔다. 그러지 않으면 부모나 공급자가 그 사이 잠들어 버릴 수 있다 */
}

/*
 * [한국어]
 * pci_ea_flags - EA 엔트리의 Property 값을 resource 플래그로 옮긴다.
 *
 * @dev: 대상 PCI 장치(현재 구현은 쓰지 않지만 시그니처를 맞춰 둔다).
 * @prop: EA 엔트리의 Property 필드 값.
 * @return: IORESOURCE_* 플래그 조합. 알 수 없는 Property 면 0.
 *
 * EA(Enhanced Allocation)는 BAR 대신 쓰는 자원 기술 방식이다. 보통의 BAR 는
 * "전부 1 을 쓰고 되읽어" 크기를 알아내고 커널이 주소를 배정하지만, EA 는
 * 주소와 크기를 config space 에 직접 적어 둔다. 재배치가 불가능한 대신
 * 크기 탐색이 필요 없다. 그래서 반환 플래그에 IORESOURCE_PCI_FIXED 가 항상
 * 들어간다 — "이 자원은 옮길 수 없다"는 뜻이며, 자원 재배정 코드가 이 표시를
 * 보고 건드리지 않는다.
 *
 * IORESOURCE_PCI_EA_BEI 는 "이 자원의 출처가 EA 다"라는 표식이다.
 *
 * Property 값이 자원의 종류를 정한다 — 메모리인지 IO 인지, prefetchable 인지,
 * PF 용인지 VF 용인지. 정의되지 않은 값에는 0 을 돌려주어 호출자가 그
 * 엔트리를 버리게 한다.
 *
 * 실행 문맥: 값 변환뿐이라 어느 문맥에서도 안전하다.
 *
 * 호출 체인:
 *   pci_ea_read() → [이 함수]
 */
static unsigned long pci_ea_flags(struct pci_dev *dev, u8 prop)
{
	unsigned long flags = IORESOURCE_PCI_FIXED | IORESOURCE_PCI_EA_BEI; /* [한국어] 모든 EA 자원에 공통인 두 표식 — 재배치 불가(FIXED)와 EA 출처(EA_BEI) */

	switch (prop) { /* [한국어] Property 값에 따라 자원의 종류가 갈린다 */
	case PCI_EA_P_MEM: /* [한국어] 일반 함수용 non-prefetchable 메모리 */
	case PCI_EA_P_VF_MEM: /* [한국어] VF 용 non-prefetchable 메모리 */
		flags |= IORESOURCE_MEM; /* [한국어] 메모리 공간으로 표시한다 */
		break; /* [한국어] 분기 종료 */
	case PCI_EA_P_MEM_PREFETCH: /* [한국어] 일반 함수용 prefetchable 메모리 */
	case PCI_EA_P_VF_MEM_PREFETCH: /* [한국어] VF 용 prefetchable 메모리 */
		flags |= IORESOURCE_MEM | IORESOURCE_PREFETCH; /* [한국어] prefetchable 표시를 함께 세운다 — 읽기에 부작용이 없어 브리지가 미리 읽어도 되는 영역이다 */
		break; /* [한국어] 분기 종료 */
	case PCI_EA_P_IO: /* [한국어] IO 공간 */
		flags |= IORESOURCE_IO; /* [한국어] IO 공간으로 표시한다 */
		break; /* [한국어] 분기 종료 */
	default: /* [한국어] 정의되지 않았거나 이 코드가 다루지 않는 Property */
		return 0; /* [한국어] 0 을 돌려 호출자가 이 엔트리를 버리게 한다 */
	}

	return flags; /* [한국어] 완성된 플래그 조합 */
}

/*
 * [한국어]
 * pci_ea_get_resource - EA 엔트리의 BEI 값이 가리키는 resource 슬롯을 찾는다.
 *
 * @dev: 대상 PCI 장치.
 * @bei: BAR Equivalent Indicator — 이 엔트리가 어느 BAR 에 해당하는지를 나타내는 번호.
 * @prop: EA 엔트리의 Property. VF 자원인지 확인하는 데 함께 쓴다.
 * @return: dev->resource[] 안의 해당 슬롯, 대응하는 것이 없으면 NULL.
 *
 * BEI 는 EA 가 쓰는 자체 번호 체계라 pci_dev->resource[] 의 인덱스와 다르다.
 * 그 사이를 옮기는 것이 이 함수의 일이다.
 *   - BEI 0~5 → 표준 BAR 0~5. 인덱스가 그대로 같다.
 *   - BEI 9~14(VF BAR 0~5) → PCI_IOV_RESOURCES 부터 시작하는 구간.
 *     CONFIG_PCI_IOV 를 껐다면 그 슬롯이 없으므로 갈래 자체가 컴파일되지 않는다.
 *   - BEI ROM → PCI_ROM_RESOURCE.
 *   - 그 밖(브리지 윈도 등)은 이 코드가 다루지 않아 NULL.
 *
 * Property 를 함께 보는 이유는 일관성 검사다. VF BAR 를 가리키는 BEI 인데
 * Property 가 VF 용이 아니면 앞뒤가 맞지 않는 엔트리이므로 받아들이지 않는다.
 *
 * 실행 문맥: 배열 인덱싱뿐이다.
 *
 * 호출 체인:
 *   pci_ea_read() → [이 함수]
 */
static struct resource *pci_ea_get_resource(struct pci_dev *dev, u8 bei,
					    u8 prop)
{
	if (bei <= PCI_EA_BEI_BAR5 && prop <= PCI_EA_P_IO) /* [한국어] BEI 가 표준 BAR 범위이고 Property 도 일반 함수용이면 */
		return &dev->resource[bei]; /* [한국어] 인덱스가 그대로 대응한다 */
#ifdef CONFIG_PCI_IOV /* [한국어] VF BAR 슬롯은 SR-IOV 를 빌드했을 때만 존재한다 */
	else if (bei >= PCI_EA_BEI_VF_BAR0 && bei <= PCI_EA_BEI_VF_BAR5 && /* [한국어] BEI 가 VF BAR 범위이고 */
		 (prop == PCI_EA_P_VF_MEM || prop == PCI_EA_P_VF_MEM_PREFETCH)) /* [한국어] Property 도 VF 용이어야 앞뒤가 맞는 엔트리다 */
		return &dev->resource[PCI_IOV_RESOURCES + /* [한국어] VF 자원 구간의 시작에서 */
				      bei - PCI_EA_BEI_VF_BAR0]; /* [한국어] BEI 의 상대 위치만큼 떨어진 슬롯이다 */
#endif
	else if (bei == PCI_EA_BEI_ROM) /* [한국어] Expansion ROM 을 가리키는 BEI 면 */
		return &dev->resource[PCI_ROM_RESOURCE]; /* [한국어] ROM 전용 슬롯을 준다 */
	else
		return NULL; /* [한국어] 이 코드가 다루지 않는다 */
}

/* Read an Enhanced Allocation (EA) entry */
/*
 * [한국어]
 * pci_ea_read - EA 엔트리 하나를 읽어 대응하는 resource 를 채운다.
 *
 * @dev: 대상 PCI 장치.
 * @offset: 이 엔트리가 시작하는 config space 오프셋.
 * @return: 다음 엔트리의 오프셋(= offset + 이 엔트리의 크기).
 *
 * EA 엔트리는 가변 길이다. 첫 DWORD 의 Entry Size 필드가 "첫 DWORD 이후
 * 몇 개가 더 있는지"를 말해 주므로, 실제 크기는 (ES + 1) * 4 바이트다.
 * 오류가 나든 성공하든 이 크기만큼 건너뛴 오프셋을 돌려주어야 다음 엔트리를
 * 제대로 읽을 수 있다. 그래서 모든 오류 경로가 out 레이블로 모인다.
 *
 * 엔트리는 최대 다섯 DWORD 로 이뤄진다.
 *   [0] 헤더 — Entry Size, BEI, Property, Secondary Property, Enable 비트
 *   [1] Base 하위 32비트. 최하위 비트가 "64비트 엔트리인가"의 표시다.
 *   [2] MaxOffset 하위 32비트. 역시 최하위 비트가 64비트 표시다.
 *   [3] Base 상위 32비트 (64비트일 때만)
 *   [4] MaxOffset 상위 32비트 (64비트일 때만)
 *
 * 주소 계산에 두 가지 요령이 있다.
 *   - Base 와 MaxOffset 의 하위 2비트는 값이 아니라 플래그 자리이므로
 *     PCI_EA_FIELD_MASK 로 걸러낸다.
 *   - end = start + (max_offset | 0x03) 에서 | 0x03 은 걸러낸 하위 2비트를
 *     다시 1 로 채우는 것이다. EA 의 구간은 항상 DWORD 단위이므로 마지막
 *     주소는 반드시 ...11 로 끝난다.
 *
 * 32비트 아키텍처(resource_size_t 가 4바이트)에서 상위 32비트가 0 이 아닌
 * 엔트리는 표현할 수 없어 버린다.
 *
 * 실행 문맥: config space 를 여러 번 읽는다. 열거 중 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_ea_init() → [이 함수] → pci_ea_get_resource(), pci_ea_flags()
 */
static int pci_ea_read(struct pci_dev *dev, int offset)
{
	struct resource *res; /* [한국어] 채울 대상 resource 슬롯 */
	const char *res_name; /* [한국어] 로그에 쓸 자원 이름("BAR 0" 등) */
	int ent_size, ent_offset = offset; /* [한국어] ent_size 는 이 엔트리의 총 바이트 수, ent_offset 은 지금까지 읽은 위치 */
	resource_size_t start, end; /* [한국어] 계산해 낼 주소 구간 */
	unsigned long flags; /* [한국어] resource 에 넣을 IORESOURCE_* 플래그 */
	u32 dw0, bei, base, max_offset; /* [한국어] dw0 은 헤더, 나머지는 헤더에서 뽑아낸 필드와 주소 값 */
	u8 prop; /* [한국어] Property 필드 */
	bool support_64 = (sizeof(resource_size_t) >= 8); /* [한국어] 이 아키텍처의 resource_size_t 가 64비트를 담을 수 있는가 — 32비트라면 상위 주소를 표현할 수 없다 */

	pci_read_config_dword(dev, ent_offset, &dw0); /* [한국어] 첫 DWORD(헤더)를 읽는다 */
	ent_offset += 4; /* [한국어] 다음 DWORD 로 커서를 옮긴다 */

	/* Entry size field indicates DWORDs after 1st */
	ent_size = (FIELD_GET(PCI_EA_ES, dw0) + 1) << 2; /* [한국어] Entry Size 는 "첫 DWORD 이후의 개수"라 +1 하고, DWORD 를 바이트로 바꾸려 <<2(=×4) 한다 */

	if (!(dw0 & PCI_EA_ENABLE)) /* Entry not enabled */
		goto out; /* [한국어] Enable 비트가 꺼진 엔트리는 내용이 무효다. 크기만큼 건너뛰도록 out 으로 간다 */

	bei = FIELD_GET(PCI_EA_BEI, dw0); /* [한국어] 이 엔트리가 어느 BAR 에 해당하는지 */
	prop = FIELD_GET(PCI_EA_PP, dw0); /* [한국어] 자원의 종류(메모리/IO, prefetchable 여부, PF/VF) */

	/*
	 * If the Property is in the reserved range, try the Secondary
	 * Property instead.
	 */
	if (prop > PCI_EA_P_BRIDGE_IO && prop < PCI_EA_P_MEM_RESERVED) /* [한국어] Primary Property 가 예약 범위에 들어가면 규격이 Secondary Property 를 보라고 정한다 */
		prop = FIELD_GET(PCI_EA_SP, dw0); /* [한국어] 같은 헤더의 Secondary Property 필드로 대체한다 */
	if (prop > PCI_EA_P_BRIDGE_IO) /* [한국어] 그래도 아는 범위를 벗어나면 */
		goto out; /* [한국어] 해석할 수 없는 엔트리다 */

	res = pci_ea_get_resource(dev, bei, prop); /* [한국어] BEI 를 resource 슬롯으로 옮긴다 */
	res_name = pci_resource_name(dev, bei); /* [한국어] 로그용 이름을 미리 구해 둔다 */
	if (!res) { /* [한국어] 대응하는 슬롯이 없으면 */
		pci_err(dev, "Unsupported EA entry BEI: %u\n", bei); /* [한국어] 어떤 BEI 였는지 남기고 */
		goto out; /* [한국어] 이 엔트리를 버린다 */
	}

	flags = pci_ea_flags(dev, prop); /* [한국어] Property 를 resource 플래그로 옮긴다 */
	if (!flags) { /* [한국어] 0 이면 다룰 수 없는 Property 다 */
		pci_err(dev, "Unsupported EA properties: %#x\n", prop); /* [한국어] 어떤 값이었는지 남기고 */
		goto out; /* [한국어] 버린다 */
	}

	/* Read Base */
	pci_read_config_dword(dev, ent_offset, &base); /* [한국어] Base 하위 32비트를 읽는다 */
	start = (base & PCI_EA_FIELD_MASK); /* [한국어] 하위 2비트는 값이 아니라 플래그 자리이므로 마스크로 걸러낸다 */
	ent_offset += 4; /* [한국어] 다음 DWORD 로 */

	/* Read MaxOffset */
	pci_read_config_dword(dev, ent_offset, &max_offset); /* [한국어] MaxOffset 하위 32비트를 읽는다 */
	ent_offset += 4; /* [한국어] 다음 DWORD 로 */

	/* Read Base MSBs (if 64-bit entry) */
	if (base & PCI_EA_IS_64) { /* [한국어] Base 의 최하위 비트가 "64비트 엔트리"를 뜻한다 */
		u32 base_upper; /* [한국어] 상위 32비트를 담을 임시 */

		pci_read_config_dword(dev, ent_offset, &base_upper); /* [한국어] Base 상위 32비트를 읽는다 */
		ent_offset += 4; /* [한국어] 다음 DWORD 로 */

		flags |= IORESOURCE_MEM_64; /* [한국어] 64비트 메모리 자원임을 표시한다 */

		/* entry starts above 32-bit boundary, can't use */
		if (!support_64 && base_upper) /* [한국어] 32비트 아키텍처인데 상위 주소가 0 이 아니면 표현할 방법이 없다 */
			goto out; /* [한국어] 이 엔트리를 버린다 */

		if (support_64) /* [한국어] 64비트를 담을 수 있으면 */
			start |= ((u64)base_upper << 32); /* [한국어] 상위 32비트를 올려 붙인다 */
	}

	end = start + (max_offset | 0x03); /* [한국어] | 0x03 은 앞서 마스크로 걸러낸 하위 2비트를 다시 1 로 채우는 것이다. EA 구간은 DWORD 단위라 마지막 주소가 반드시 ...11 로 끝난다 */

	/* Read MaxOffset MSBs (if 64-bit entry) */
	if (max_offset & PCI_EA_IS_64) { /* [한국어] MaxOffset 의 최하위 비트도 64비트 여부를 뜻한다 */
		u32 max_offset_upper; /* [한국어] 상위 32비트를 담을 임시 */

		pci_read_config_dword(dev, ent_offset, &max_offset_upper); /* [한국어] MaxOffset 상위 32비트를 읽는다 */
		ent_offset += 4; /* [한국어] 다음 DWORD 로 */

		flags |= IORESOURCE_MEM_64; /* [한국어] 64비트 자원 표시 */

		/* entry too big, can't use */
		if (!support_64 && max_offset_upper) /* [한국어] 32비트 아키텍처에서는 이만한 크기를 표현할 수 없다 */
			goto out; /* [한국어] 버린다 */

		if (support_64) /* [한국어] 64비트를 담을 수 있으면 */
			end += ((u64)max_offset_upper << 32); /* [한국어] 상위 32비트만큼 끝 주소를 늘린다 */
	}

	if (end < start) { /* [한국어] 끝이 시작보다 앞이면 32비트 경계를 넘어가며 값이 뒤집힌 것이다 */
		pci_err(dev, "EA Entry crosses address boundary\n"); /* [한국어] 잘못된 엔트리임을 알리고 */
		goto out; /* [한국어] 버린다 */
	}

	if (ent_size != ent_offset - offset) { /* [한국어] 헤더가 말한 크기와 실제로 읽은 바이트 수가 맞아야 한다 */
		pci_err(dev, "EA Entry Size (%d) does not match length read (%d)\n", /* [한국어] 어긋나면 엔트리 형식이 깨진 것이다 */
			ent_size, ent_offset - offset); /* [한국어] 양쪽 값을 함께 남긴다 */
		goto out; /* [한국어] 버린다 */
	}

	res->name = pci_name(dev); /* [한국어] /proc/iomem 등에 표시될 이름 */
	res->start = start; /* [한국어] 계산해 낸 시작 주소 */
	res->end = end; /* [한국어] 계산해 낸 끝 주소 */
	res->flags = flags; /* [한국어] 종류와 IORESOURCE_PCI_FIXED 등 표식. FIXED 가 있어 자원 재배정 코드가 이 자원을 옮기지 않는다 */

	if (bei <= PCI_EA_BEI_BAR5) /* [한국어] 표준 BAR 에 해당하면 */
		pci_info(dev, "%s %pR: from Enhanced Allocation, properties %#02x\n", /* [한국어] 이름표와 함께 남긴다 */
			 res_name, res, prop); /* [한국어] 자원 이름, 구간, Property */
	else if (bei == PCI_EA_BEI_ROM) /* [한국어] ROM 이면 */
		pci_info(dev, "%s %pR: from Enhanced Allocation, properties %#02x\n", /* [한국어] 같은 형식으로 남긴다 */
			 res_name, res, prop); /* [한국어] 자원 이름, 구간, Property */
	else if (bei >= PCI_EA_BEI_VF_BAR0 && bei <= PCI_EA_BEI_VF_BAR5) /* [한국어] VF BAR 범위면 */
		pci_info(dev, "%s %pR: from Enhanced Allocation, properties %#02x\n", /* [한국어] 역시 같은 형식으로 남긴다 */
			 res_name, res, prop); /* [한국어] 자원 이름, 구간, Property */
	else
		pci_info(dev, "BEI %d %pR: from Enhanced Allocation, properties %#02x\n", /* [한국어] BEI 번호를 그대로 찍는다 */
			   bei, res, prop); /* [한국어] BEI 번호, 구간, Property */

out: /* [한국어] 성공·실패가 모두 모이는 지점 */
	return offset + ent_size; /* [한국어] 실패했더라도 이 엔트리의 크기만큼은 건너뛰어야 다음 엔트리를 제대로 읽는다 */
}

/* Enhanced Allocation Initialization */
/*
 * [한국어]
 * pci_ea_init - EA capability 를 찾아 모든 엔트리를 읽어 들인다.
 *
 * @dev: 대상 PCI 장치.
 * @return: 없음. EA capability 가 없으면 아무 것도 하지 않는다.
 *
 * EA 가 있는 장치는 BAR 탐색 대신 이 경로로 자원을 얻는다. 엔트리 개수는
 * capability 헤더의 NumEntries 필드에 있고, 첫 엔트리의 위치는
 * PCI_EA_FIRST_ENT 로 정해져 있다.
 *
 * 헤더 타입 1(브리지)에서 4바이트를 더 건너뛰는 이유는, 브리지용 EA
 * capability 에는 첫 엔트리 앞에 DWORD 가 하나 더 놓이기 때문이다
 * (원본 주석의 "Skip DWORD 2 for type 1 functions").
 *
 * 각 엔트리는 길이가 달라, pci_ea_read() 가 돌려주는 다음 오프셋을 그대로
 * 이어받는 방식으로 순회한다.
 *
 * 실행 문맥: 열거 중 프로세스 문맥.
 *
 * 호출 체인:
 *   probe.c → [이 함수] → pci_find_capability(), pci_ea_read()
 */
void pci_ea_init(struct pci_dev *dev)
{
	int ea; /* [한국어] EA capability 의 오프셋 */
	u8 num_ent; /* [한국어] 엔트리 개수 */
	int offset; /* [한국어] 지금 읽을 엔트리의 오프셋 */
	int i; /* [한국어] 엔트리 순회 인덱스 */

	/* find PCI EA capability in list */
	ea = pci_find_capability(dev, PCI_CAP_ID_EA); /* [한국어] 표준 capability 리스트에서 EA 를 찾는다 */
	if (!ea) /* [한국어] 없으면 이 장치는 EA 를 쓰지 않는다 */
		return; /* [한국어] 할 일이 없다 */

	/* determine the number of entries */
	pci_bus_read_config_byte(dev->bus, dev->devfn, ea + PCI_EA_NUM_ENT, /* [한국어] capability 헤더의 NumEntries 필드를 읽는다 */
					&num_ent); /* [한국어] 읽은 값을 담을 곳 */
	num_ent &= PCI_EA_NUM_ENT_MASK; /* [한국어] 상위 비트는 다른 용도라 개수 필드만 남긴다 */

	offset = ea + PCI_EA_FIRST_ENT; /* [한국어] 첫 엔트리의 위치 */

	/* Skip DWORD 2 for type 1 functions */
	if (dev->hdr_type == PCI_HEADER_TYPE_BRIDGE) /* [한국어] 브리지용 EA 는 첫 엔트리 앞에 DWORD 가 하나 더 있다 */
		offset += 4; /* [한국어] 그만큼 건너뛴다 */

	/* parse each EA entry */
	for (i = 0; i < num_ent; ++i) /* [한국어] 선언된 개수만큼 반복한다 */
		offset = pci_ea_read(dev, offset); /* [한국어] 각 엔트리를 읽고, 돌려받은 다음 오프셋을 이어받는다. 엔트리 길이가 제각각이라 이 방식이어야 한다 */
}

/*
 * [한국어]
 * pci_add_saved_cap - capability 저장 버퍼를 장치의 해시 리스트에 매단다.
 *
 * @pci_dev: 대상 PCI 장치.
 * @new_cap: 새로 할당한 저장 버퍼.
 * @return: 없음.
 *
 * 저장 버퍼는 dev->saved_cap_space 해시 리스트에 매달리고,
 * _pci_find_saved_cap() 이 그 리스트를 훑어 찾는다. 리스트 앞에 붙이므로
 * 순서는 등록의 역순이 되지만, 조회가 번호로 이뤄져 순서는 의미가 없다.
 *
 * 실행 문맥: 열거 중 프로세스 문맥. 이 시점에는 다른 주체가 리스트를 보지
 * 않으므로 별도 락이 없다.
 *
 * 호출 체인:
 *   _pci_add_cap_save_buffer() → [이 함수] → hlist_add_head()
 */
static void pci_add_saved_cap(struct pci_dev *pci_dev,
	struct pci_cap_saved_state *new_cap)
{
	hlist_add_head(&new_cap->next, &pci_dev->saved_cap_space); /* [한국어] 해시 리스트 앞에 붙인다. 조회가 번호로 이뤄져 순서는 의미가 없다 */
}

/**
 * _pci_add_cap_save_buffer - allocate buffer for saving given
 *			      capability registers
 * @dev: the PCI device
 * @cap: the capability to allocate the buffer for
 * @extended: Standard or Extended capability ID
 * @size: requested size of the buffer
 */
/*
 * [한국어]
 * _pci_add_cap_save_buffer - capability 하나를 위한 저장 버퍼를 미리 잡아 둔다.
 *
 * @dev: 대상 PCI 장치.
 * @cap: 버퍼를 잡을 capability 번호.
 * @extended: 확장 capability 면 true.
 * @size: 그 capability 의 레지스터를 담는 데 필요한 바이트 수.
 * @return: 0 성공 또는 "해당 capability 가 없어 잡을 필요 없음". -ENOMEM 은 할당 실패.
 *
 * 저장 버퍼를 왜 미리 잡아 두는가가 핵심이다. pci_save_state() 는 절전
 * 진입이나 리셋 직전처럼 실패가 곤란한 시점에 불린다. 그때 메모리 할당이
 * 실패하면 상태를 뜨지 못하고, 결국 복원도 못 해 장치를 잃는다. 그래서
 * 열거 시점에 미리 잡아 두고, 저장 경로는 이미 있는 버퍼에 쓰기만 한다.
 *
 * capability 가 없으면 0 을 돌려준다. 오류가 아니라 "잡을 필요가 없다"는
 * 뜻이므로 호출자가 실패로 취급하면 안 된다.
 *
 * cap_nr 과 cap_extended 를 함께 기록해 두어야 _pci_find_saved_cap() 이
 * 나중에 이 버퍼를 정확히 찾아낼 수 있다. 두 번호 공간이 겹치기 때문이다.
 *
 * 실행 문맥: GFP_KERNEL 할당. 열거 중 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_add_cap_save_buffer() / pci_add_ext_cap_save_buffer() → [이 함수]
 *     → pci_find_capability(), kzalloc(), pci_add_saved_cap()
 */
static int _pci_add_cap_save_buffer(struct pci_dev *dev, u16 cap,
				    bool extended, unsigned int size)
{
	int pos; /* [한국어] capability 의 오프셋 — 존재 여부만 확인하는 데 쓴다 */
	struct pci_cap_saved_state *save_state; /* [한국어] 새로 잡을 저장 버퍼 */

	if (extended) /* [한국어] 확장 capability 면 */
		pos = pci_find_ext_capability(dev, cap); /* [한국어] 0x100 이후 리스트에서 찾고 */
	else
		pos = pci_find_capability(dev, cap); /* [한국어] 0x34 에서 시작하는 리스트에서 찾는다 */

	if (!pos) /* [한국어] 이 장치에 해당 capability 가 없으면 */
		return 0; /* [한국어] 잡을 필요가 없다. 오류가 아니라 정상적인 0 이다 */

	save_state = kzalloc(sizeof(*save_state) + size, GFP_KERNEL); /* [한국어] 관리용 헤더 + 요청한 데이터 크기만큼 잡는다. kzalloc 이라 데이터 영역이 0 으로 시작한다 */
	if (!save_state) /* [한국어] 할당 실패 */
		return -ENOMEM; /* [한국어] 저장 경로에서 이 버퍼가 없으면 상태를 뜰 수 없으므로 오류로 알린다 */

	save_state->cap.cap_nr = cap; /* [한국어] 어느 capability 의 버퍼인지 기록한다 */
	save_state->cap.cap_extended = extended; /* [한국어] 표준/확장 구분도 기록한다. 두 공간의 번호가 겹쳐 이 필드가 없으면 구분되지 않는다 */
	save_state->cap.size = size; /* [한국어] 데이터 영역의 크기. 사본을 실을 때 크기가 맞는지 검증하는 근거가 된다 */
	pci_add_saved_cap(dev, save_state); /* [한국어] 장치의 해시 리스트에 매단다 */

	return 0; /* [한국어] 준비 완료 */
}

/*
 * [한국어]
 * pci_add_cap_save_buffer - 표준 capability 용 저장 버퍼를 잡는다.
 *
 * @dev: 대상 PCI 장치.
 * @cap: 표준 capability ID.
 * @size: 필요한 바이트 수.
 * @return: 0 성공 또는 "capability 없음", -ENOMEM 은 할당 실패.
 *
 * extended=false 로 고정한 래퍼다.
 *
 * 호출 체인:
 *   pci_allocate_cap_save_buffers() 등 → [이 함수] → _pci_add_cap_save_buffer()
 */
int pci_add_cap_save_buffer(struct pci_dev *dev, char cap, unsigned int size)
{
	return _pci_add_cap_save_buffer(dev, cap, false, size); /* [한국어] extended=false — 표준 capability 공간 */
}

/*
 * [한국어]
 * pci_add_ext_cap_save_buffer - 확장 capability 용 저장 버퍼를 잡는다.
 *
 * @dev: 대상 PCI 장치.
 * @cap: 확장 capability ID.
 * @size: 필요한 바이트 수.
 * @return: 0 성공 또는 "capability 없음", -ENOMEM 은 할당 실패.
 *
 * extended=true 로 고정한 래퍼다. AER/DPC/PTM/LTR 처럼 0x100 이후에 있는
 * capability 의 상태를 뜨려는 코드가 쓴다.
 *
 * 호출 체인:
 *   pci_allocate_cap_save_buffers(), aer.c/dpc.c 등 → [이 함수]
 *     → _pci_add_cap_save_buffer()
 */
int pci_add_ext_cap_save_buffer(struct pci_dev *dev, u16 cap, unsigned int size)
{
	return _pci_add_cap_save_buffer(dev, cap, true, size); /* [한국어] extended=true — 확장 capability 공간 */
}

/**
 * pci_allocate_cap_save_buffers - allocate buffers for saving capabilities
 * @dev: the PCI device
 */
/*
 * [한국어]
 * pci_allocate_cap_save_buffers - 열거 시점에 필요한 저장 버퍼를 한꺼번에 잡는다.
 *
 * @dev: 대상 PCI 장치.
 * @return: 없음. 개별 실패는 로그만 남기고 계속 진행한다.
 *
 * pci_save_state() 가 실패하면 안 되는 시점에 불리므로, 그때 필요한 버퍼를
 * 여기서 미리 다 잡아 둔다. 잡는 대상과 크기가 각각 저장 함수와 짝을 이룬다.
 *   - PCIe: PCI_EXP_SAVE_REGS(7) 개의 u16 — pci_save_pcie_state() 가 딱 그만큼 쓴다.
 *   - PCI-X: u16 하나 — Command 레지스터 한 개.
 *   - LTR: u16 두 개 — 최대 지연 값 두 개.
 * VC(Virtual Channel)는 크기가 장치마다 달라 전용 함수가 따로 계산한다.
 *
 * 실패해도 중단하지 않고 로그만 남기는 이유는, 한 capability 의 버퍼를 잡지
 * 못했다고 나머지까지 포기하면 절전 시 잃는 것이 더 많기 때문이다.
 * 잡지 못한 capability 는 나중에 저장 단계에서 -ENOMEM 으로 드러난다.
 *
 * 실행 문맥: GFP_KERNEL 할당. 열거 중 프로세스 문맥.
 *
 * 호출 체인:
 *   probe.c 의 pci_device_add() → [이 함수]
 *     → pci_add_cap_save_buffer(), pci_add_ext_cap_save_buffer(),
 *        pci_allocate_vc_save_buffers()
 */
void pci_allocate_cap_save_buffers(struct pci_dev *dev)
{
	int error; /* [한국어] 각 할당의 결과 */

	error = pci_add_cap_save_buffer(dev, PCI_CAP_ID_EXP, /* [한국어] PCIe capability 용 버퍼 */
					PCI_EXP_SAVE_REGS * sizeof(u16)); /* [한국어] pci_save_pcie_state() 가 저장하는 레지스터 개수와 정확히 일치해야 한다 */
	if (error) /* [한국어] 실패하면 */
		pci_err(dev, "unable to preallocate PCI Express save buffer\n"); /* [한국어] 로그만 남기고 다음 것을 계속 잡는다 */

	error = pci_add_cap_save_buffer(dev, PCI_CAP_ID_PCIX, sizeof(u16)); /* [한국어] PCI-X 는 Command 레지스터 하나뿐이라 u16 하나면 된다 */
	if (error) /* [한국어] 실패하면 */
		pci_err(dev, "unable to preallocate PCI-X save buffer\n"); /* [한국어] 로그만 남긴다 */

	error = pci_add_ext_cap_save_buffer(dev, PCI_EXT_CAP_ID_LTR, /* [한국어] LTR 확장 capability 용 버퍼 */
					    2 * sizeof(u16)); /* [한국어] 최대 지연 값 두 개를 담는다 */
	if (error) /* [한국어] 실패하면 */
		pci_err(dev, "unable to allocate suspend buffer for LTR\n"); /* [한국어] 로그만 남긴다 */

	pci_allocate_vc_save_buffers(dev); /* [한국어] VC 는 장치마다 채널 수가 달라 크기를 전용 함수가 직접 계산한다 */
}

/*
 * [한국어]
 * pci_free_cap_save_buffers - 장치에 매달린 저장 버퍼를 모두 해제한다.
 *
 * @dev: 대상 PCI 장치.
 * @return: 없음.
 *
 * pci_allocate_cap_save_buffers() 와 그 밖의 경로가 잡아 둔 버퍼를 장치 해제
 * 시점에 한꺼번에 돌려준다. 순회하며 지우므로 _safe 판이 필요하다 —
 * 지운 항목의 next 를 읽을 수 없기 때문이다.
 *
 * 리스트 머리를 비우지 않는 이유는 이 함수 뒤에 pci_dev 자체가 해제되어
 * 리스트를 다시 볼 일이 없기 때문이다.
 *
 * 실행 문맥: 장치 해제 경로. 프로세스 문맥.
 *
 * 호출 체인:
 *   probe.c 의 장치 해제 경로 → [이 함수] → kfree()
 */
void pci_free_cap_save_buffers(struct pci_dev *dev)
{
	struct pci_cap_saved_state *tmp; /* [한국어] 순회 커서 */
	struct hlist_node *n; /* [한국어] 지운 항목의 next 를 미리 붙들어 두는 예비 포인터 */

	hlist_for_each_entry_safe(tmp, n, &dev->saved_cap_space, next) /* [한국어] 순회 중 항목을 지우므로 _safe 판을 쓴다 */
		kfree(tmp); /* [한국어] 버퍼 하나를 해제한다 */
}

/**
 * pci_configure_ari - enable or disable ARI forwarding
 * @dev: the PCI device
 *
 * If @dev and its upstream bridge both support ARI, enable ARI in the
 * bridge.  Otherwise, disable ARI in the bridge.
 */
/*
 * [한국어]
 * pci_configure_ari - 상위 브리지의 ARI Forwarding 을 켜거나 끈다.
 *
 * @dev: 방금 열거된 PCI 장치.
 * @return: 없음.
 *
 * ARI(Alternative Routing-ID Interpretation)는 devfn 8비트를 해석하는 방식을
 * 바꾸는 기능이다. 원래는 상위 5비트가 device 번호, 하위 3비트가 function
 * 번호라 한 장치에 함수가 8개뿐이지만, ARI 를 켜면 8비트 전체를 함수 번호로
 * 읽어 한 장치가 최대 256개 함수를 가질 수 있다. SR-IOV 로 VF 를 많이 만드는
 * 장치에 꼭 필요하다.
 *
 * 켜야 할 곳이 장치가 아니라 그 위 브리지라는 점이 요점이다. devfn 을
 * 해석해 트래픽을 내려보내는 주체가 브리지이기 때문이다. 그래서 조건도
 * 양쪽을 본다 — 브리지의 DEVCAP2 에 ARI 지원 비트가 있어야 하고,
 * 장치에는 ARI 확장 capability 가 있어야 한다.
 *
 * dev->devfn 이 0 인 장치(함수 0)에서만 동작한다. 한 장치의 여러 함수가
 * 저마다 브리지 설정을 뒤집는 것을 막기 위해, 대표 함수 하나만 판단한다.
 *
 * 실행 문맥: 열거 중 프로세스 문맥. config space 를 읽고 쓴다.
 *
 * 호출 체인:
 *   probe.c 의 pci_configure_device() 경로 → [이 함수]
 *     → pcie_capability_read_dword(), pci_find_ext_capability(),
 *        pcie_capability_set_word()/clear_word()
 */
void pci_configure_ari(struct pci_dev *dev)
{
	u32 cap; /* [한국어] 브리지의 Device Capabilities 2 레지스터 값 */
	struct pci_dev *bridge; /* [한국어] 설정을 실제로 바꿀 상위 브리지 */

	if (pcie_ari_disabled || !pci_is_pcie(dev) || dev->devfn) /* [한국어] pci=noari 로 껐거나, PCIe 가 아니거나, 함수 0 이 아니면 — 마지막 조건은 한 장치의 여러 함수가 브리지 설정을 뒤집는 것을 막는다 */
		return; /* [한국어] 건드리지 않는다 */

	bridge = dev->bus->self; /* [한국어] devfn 을 해석하는 주체인 상위 브리지 */
	if (!bridge) /* [한국어] 루트 버스 직결이면 조정할 브리지가 없다 */
		return; /* [한국어] 할 일이 없다 */

	pcie_capability_read_dword(bridge, PCI_EXP_DEVCAP2, &cap); /* [한국어] 브리지가 ARI Forwarding 을 지원하는지 확인한다 */
	if (!(cap & PCI_EXP_DEVCAP2_ARI)) /* [한국어] 지원하지 않으면 */
		return; /* [한국어] 켤 수도 없다 */

	if (pci_find_ext_capability(dev, PCI_EXT_CAP_ID_ARI)) { /* [한국어] 장치 쪽에 ARI 확장 capability 가 있으면 — 즉 8비트 함수 번호를 쓰는 장치면 */
		pcie_capability_set_word(bridge, PCI_EXP_DEVCTL2, /* [한국어] 브리지의 ARI Forwarding 을 켠다 */
					 PCI_EXP_DEVCTL2_ARI); /* [한국어] Device Control 2 의 ARI Forwarding Enable 비트 */
		bridge->ari_enabled = 1; /* [한국어] 소프트웨어 쪽 표시도 세운다. SR-IOV 코드가 VF 의 devfn 을 계산할 때 이 값을 본다 */
	} else {
		pcie_capability_clear_word(bridge, PCI_EXP_DEVCTL2, /* [한국어] 브리지의 ARI Forwarding 을 끈다. 켜 둔 채로 두면 devfn 해석이 어긋난다 */
					   PCI_EXP_DEVCTL2_ARI); /* [한국어] 같은 비트를 지운다 */
		bridge->ari_enabled = 0; /* [한국어] 소프트웨어 표시도 내린다 */
	}
}

/*
 * [한국어]
 * pci_acs_flags_enabled - 요구한 ACS 비트가 실제로 켜져 있는지 확인한다.
 *
 * @pdev: 검사할 장치.
 * @acs_flags: 확인할 ACS 비트 조합.
 * @return: 요구한 비트가 모두 켜져 있으면 true.
 *
 * 단순 비트 비교가 아니라 한 겹의 해석이 들어간다. 원본 주석이 설명하듯,
 * ACS 기능 대부분은 규격상 "필수" 이거나 "제어 가능한 경우에만 필수"다.
 * 따라서 Capability 필드에 그 기능이 없다는 것은 "지원하지 않는다"가 아니라
 * "제어할 수 없게 하드와이어로 켜져 있다"는 뜻으로 읽어야 한다.
 * 그래서 요구 비트를 Capability 와 AND 해 "제어 가능한 것만" 남긴 뒤
 * Control 레지스터와 비교한다. 제어할 수 없는 비트는 이미 켜진 것으로 본다.
 *
 * Egress Control(EC)만 예외라 마스크에 언제나 포함시킨다 — 이 기능은
 * 하드와이어로 켜져 있다고 가정할 수 없기 때문이다.
 *
 * 실행 문맥: config space 읽기. 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_acs_enabled() → [이 함수] → pci_read_config_word()
 */
static bool pci_acs_flags_enabled(struct pci_dev *pdev, u16 acs_flags)
{
	int pos; /* [한국어] ACS capability 의 오프셋 */
	u16 ctrl; /* [한국어] ACS Control 레지스터 값 */

	pos = pdev->acs_cap; /* [한국어] 열거 때 캐시해 둔 ACS capability 위치 */
	if (!pos) /* [한국어] ACS capability 자체가 없으면 */
		return false; /* [한국어] 요구를 만족한다고 말할 수 없다 */

	/*
	 * Except for egress control, capabilities are either required
	 * or only required if controllable.  Features missing from the
	 * capability field can therefore be assumed as hard-wired enabled.
	 */
	acs_flags &= (pdev->acs_capabilities | PCI_ACS_EC); /* [한국어] 제어 가능한 비트만 남긴다. Capability 에 없는 기능은 하드와이어로 켜진 것으로 보아 검사에서 뺀다. EC 만 예외라 언제나 포함시킨다 */

	pci_read_config_word(pdev, pos + PCI_ACS_CTRL, &ctrl); /* [한국어] ACS Control 레지스터를 읽는다 */
	return (ctrl & acs_flags) == acs_flags; /* [한국어] 남은 요구 비트가 모두 켜져 있어야 true 다 */
}

/**
 * pci_acs_enabled - test ACS against required flags for a given device
 * @pdev: device to test
 * @acs_flags: required PCI ACS flags
 *
 * Return true if the device supports the provided flags.  Automatically
 * filters out flags that are not implemented on multifunction devices.
 *
 * Note that this interface checks the effective ACS capabilities of the
 * device rather than the actual capabilities.  For instance, most single
 * function endpoints are not required to support ACS because they have no
 * opportunity for peer-to-peer access.  We therefore return 'true'
 * regardless of whether the device exposes an ACS capability.  This makes
 * it much easier for callers of this function to ignore the actual type
 * or topology of the device when testing ACS support.
 */
/*
 * [한국어]
 * pci_acs_enabled - 이 장치가 요구한 ACS 격리를 제공하는가(실효적 판정).
 *
 * @pdev: 검사할 장치.
 * @acs_flags: 요구하는 ACS 비트 조합.
 * @return: 요구를 만족하면 true.
 *
 * 원본 주석이 강조하듯, 이것은 "실제 capability" 가 아니라 "실효적
 * capability" 를 본다. 즉 "ACS 를 구현했는가"가 아니라 "이 장치를 지나는
 * P2P 트래픽이 IOMMU 를 우회할 수 있는가"를 묻는다.
 * 그래서 단일 함수 엔드포인트처럼 애초에 P2P 를 할 기회가 없는 장치는
 * ACS capability 가 없어도 true 를 돌려준다. 덕분에 호출자는 장치의 종류나
 * 위상을 신경 쓰지 않고 이 함수 하나로 판정할 수 있다.
 *
 * 판정은 PCIe 타입별로 갈리며 각각 규격 근거가 있다(원본 주석에 절 번호가 있다).
 *   - quirk 가 먼저다. 하드웨어별 예외는 pci_dev_specific_acs_enabled() 가 답한다.
 *   - 재래식 PCI/PCI-X: 공유 버스라 어느 장치든 남의 DMA 를 엿보거나 받을 수
 *     있다. 격리가 성립할 수 없으므로 false.
 *   - PCIe-to-PCI/X 브리지, RC Event Collector: 규격이 ACS 를 배제하거나
 *     언급하지 않아 보수적으로 false.
 *   - Downstream Port / Root Port: 단일·다중 함수와 무관하게 ACS 를 구현해야
 *     하므로 실제 비트를 확인한다.
 *   - Endpoint / Upstream / Legacy End / RC End: 다중 함수일 때만 P2P 기회가
 *     있으므로, 다중 함수면 확인하고 단일 함수면 아래로 빠져 true.
 *
 * NVMe 와의 관계: NVMe SSD 는 보통 단일 함수 엔드포인트라 이 함수가 true 를
 * 돌려준다. P2PDMA 를 쓸 때는 SSD 자체가 아니라 경로상의 스위치 포트가
 * 관건이며, 그 판정은 pci_acs_path_enabled() 가 한다.
 *
 * 실행 문맥: config space 읽기. 프로세스 문맥.
 *
 * 호출 체인:
 *   IOMMU 그룹 구성 코드, p2pdma.c → [이 함수]
 *     → pci_dev_specific_acs_enabled(), pci_acs_flags_enabled()
 */
bool pci_acs_enabled(struct pci_dev *pdev, u16 acs_flags)
{
	int ret; /* [한국어] quirk 의 답. 0 이상이면 quirk 가 판정을 끝냈다는 뜻이다 */

	ret = pci_dev_specific_acs_enabled(pdev, acs_flags); /* [한국어] 하드웨어별 예외를 먼저 물어본다. 표준 ACS 대신 자체 방식으로 격리하는 장치가 있다 */
	if (ret >= 0) /* [한국어] quirk 가 판정했으면(음수가 아니면) */
		return ret > 0; /* [한국어] 그 답을 그대로 쓴다. 0 은 false, 양수는 true */

	/*
	 * Conventional PCI and PCI-X devices never support ACS, either
	 * effectively or actually.  The shared bus topology implies that
	 * any device on the bus can receive or snoop DMA.
	 */
	if (!pci_is_pcie(pdev)) /* [한국어] 재래식 PCI/PCI-X 는 공유 버스라 격리 자체가 성립하지 않는다 */
		return false; /* [한국어] 격리 없음 */

	switch (pci_pcie_type(pdev)) { /* [한국어] PCIe 타입별로 규격이 요구하는 바가 다르다 */
	/*
	 * PCI/X-to-PCIe bridges are not specifically mentioned by the spec,
	 * but since their primary interface is PCI/X, we conservatively
	 * handle them as we would a non-PCIe device.
	 */
	case PCI_EXP_TYPE_PCIE_BRIDGE: /* [한국어] PCIe-to-PCI/X 브리지 — 주 인터페이스가 PCI/X 라 보수적으로 다룬다 */
	/*
	 * PCIe 3.0, 6.12.1 excludes ACS on these devices.  "ACS is never
	 * applicable... must never implement an ACS Extended Capability...".
	 * This seems arbitrary, but we take a conservative interpretation
	 * of this statement.
	 */
	case PCI_EXP_TYPE_PCI_BRIDGE: /* [한국어] PCI-to-PCIe 브리지 */
	case PCI_EXP_TYPE_RC_EC: /* [한국어] Root Complex Event Collector — 규격이 ACS 를 배제한다 */
		return false; /* [한국어] 세 종류 모두 격리를 보장할 수 없다 */
	/*
	 * PCIe 3.0, 6.12.1.1 specifies that downstream and root ports should
	 * implement ACS in order to indicate their peer-to-peer capabilities,
	 * regardless of whether they are single- or multi-function devices.
	 */
	case PCI_EXP_TYPE_DOWNSTREAM: /* [한국어] 스위치의 다운스트림 포트 */
	case PCI_EXP_TYPE_ROOT_PORT: /* [한국어] Root Port — 이 둘은 단일·다중 함수와 무관하게 ACS 를 구현해야 한다 */
		return pci_acs_flags_enabled(pdev, acs_flags); /* [한국어] 실제 비트를 확인한다 */
	/*
	 * PCIe 3.0, 6.12.1.2 specifies ACS capabilities that should be
	 * implemented by the remaining PCIe types to indicate peer-to-peer
	 * capabilities, but only when they are part of a multifunction
	 * device.  The footnote for section 6.12 indicates the specific
	 * PCIe types included here.
	 */
	case PCI_EXP_TYPE_ENDPOINT: /* [한국어] 엔드포인트 */
	case PCI_EXP_TYPE_UPSTREAM: /* [한국어] 스위치 업스트림 포트 */
	case PCI_EXP_TYPE_LEG_END: /* [한국어] 레거시 엔드포인트 */
	case PCI_EXP_TYPE_RC_END: /* [한국어] Root Complex 통합 엔드포인트 */
		if (!pdev->multifunction) /* [한국어] 단일 함수 장치라면 P2P 를 할 기회가 없다 */
			break; /* [한국어] switch 를 빠져나가 아래에서 true 로 끝난다 */

		return pci_acs_flags_enabled(pdev, acs_flags); /* [한국어] 다중 함수라면 함수끼리 P2P 가 가능하므로 실제 비트를 확인한다 */
	}

	/*
	 * PCIe 3.0, 6.12.1.3 specifies no ACS capabilities are applicable
	 * to single function devices with the exception of downstream ports.
	 */
	return true; /* [한국어] 단일 함수 장치(다운스트림 포트 제외)에는 ACS 가 적용되지 않는다. 격리를 깰 방법이 없으므로 요구를 만족한 것으로 본다 */
}

/**
 * pci_acs_path_enabled - test ACS flags from start to end in a hierarchy
 * @start: starting downstream device
 * @end: ending upstream device or NULL to search to the root bus
 * @acs_flags: required flags
 *
 * Walk up a device tree from start to end testing PCI ACS support.  If
 * any step along the way does not support the required flags, return false.
 */
/*
 * [한국어]
 * pci_acs_path_enabled - 두 장치 사이의 경로 전 구간에서 ACS 격리가 성립하는지 본다.
 *
 * @start: 아래쪽 시작 장치.
 * @end: 위쪽 끝 장치. NULL 이면 루트 버스까지 훑는다.
 * @acs_flags: 요구하는 ACS 비트 조합.
 * @return: 경로상 모든 장치가 요구를 만족하면 true.
 *
 * 격리는 경로에서 가장 약한 고리로 결정된다. 중간의 스위치 포트 하나가
 * P2P 트래픽을 위로 올려보내지 않고 자기 안에서 되돌리면, 그 지점에서
 * IOMMU 를 우회하게 되어 아래위 모두의 격리가 무너진다. 그래서 한 곳이라도
 * 어긋나면 즉시 false 다.
 *
 * @end 가 NULL 인 경우의 처리가 미묘하다. 루트 버스에 닿으면 (end == NULL) 을
 * 돌려준다 — end 를 지정했는데 루트까지 왔다는 것은 그 end 가 이 경로 위에
 * 없었다는 뜻이므로 false 여야 한다.
 *
 * do-while 인 이유는 start 자신도 검사 대상이기 때문이다. start == end 인
 * 경우에도 최소 한 번은 돌아 그 장치를 확인한다.
 *
 * NVMe 와의 관계: P2PDMA 로 NVMe 의 CMB 와 다른 장치가 직접 데이터를 주고받을
 * 때, 그 경로에서 ACS 재지정이 켜져 있으면 트래픽이 루트까지 올라갔다
 * 내려와 성능이 크게 떨어진다. p2pdma.c 가 이 함수로 경로를 검사한다.
 *
 * 실행 문맥: 버스 계층을 거슬러 올라가며 config 를 읽는다. 프로세스 문맥.
 *
 * 호출 체인:
 *   drivers/pci/p2pdma.c, IOMMU 그룹 구성 코드 → [이 함수] → pci_acs_enabled()
 */
bool pci_acs_path_enabled(struct pci_dev *start,
			  struct pci_dev *end, u16 acs_flags)
{
	struct pci_dev *pdev, *parent = start; /* [한국어] pdev 는 지금 검사 중인 장치, parent 는 다음에 올라갈 장치. start 부터 시작한다 */

	do { /* [한국어] start 자신도 검사해야 하므로 do-while 이다 */
		pdev = parent; /* [한국어] 이번 회차에 검사할 장치 */

		if (!pci_acs_enabled(pdev, acs_flags)) /* [한국어] 한 곳이라도 격리를 제공하지 않으면 */
			return false; /* [한국어] 경로 전체가 무너진다 */

		if (pci_is_root_bus(pdev->bus)) /* [한국어] 루트 버스에 닿았다면 더 올라갈 곳이 없다 */
			return (end == NULL); /* [한국어] end 를 지정하지 않았다면 루트까지 온 것이 정상이라 true, 지정했는데 여기까지 왔다면 그 end 가 경로 위에 없었다는 뜻이라 false 다 */

		parent = pdev->bus->self; /* [한국어] 한 단계 위 브리지로 */
	} while (pdev != end); /* [한국어] end 에 닿으면 멈춘다 */

	return true; /* [한국어] 경로 전 구간이 요구를 만족했다 */
}

/**
 * pci_acs_init - Initialize ACS if hardware supports it
 * @dev: the PCI device
 */
/*
 * [한국어]
 * pci_acs_init - 열거 중 ACS capability 위치와 능력 비트를 캐시해 둔다.
 *
 * @dev: 막 열거된 PCI 장치.
 * @return: 없음.
 *
 * 이후 ACS 관련 코드가 매번 확장 capability 리스트를 훑지 않도록,
 * 오프셋과 Capability 레지스터 값을 pci_dev 에 담아 둔다.
 *   - dev->acs_cap: ACS capability 의 오프셋. 0 이면 이 장치에는 ACS 가 없다.
 *   - dev->acs_capabilities: 어떤 ACS 기능을 제어할 수 있는지의 비트맵.
 *
 * 마지막의 pci_disable_broken_acs_cap() 은 quirk 다. ACS capability 를
 * 내보이지만 실제로는 동작하지 않는 하드웨어가 있어, 그런 장치의 능력
 * 비트를 지워 커널이 잘못 믿지 않게 한다.
 *
 * 이름과 달리 여기서 ACS 를 켜지는 않는다. 켜는 일은 pci_enable_acs() 가
 * 나중에 한다.
 *
 * 실행 문맥: 열거 중 프로세스 문맥.
 *
 * 호출 체인:
 *   probe.c 의 pci_init_capabilities() → [이 함수]
 *     → pci_find_ext_capability(), pci_disable_broken_acs_cap()
 */
void pci_acs_init(struct pci_dev *dev)
{
	int pos; /* [한국어] 지역 사본. 아래에서 두 번 쓰므로 한 번만 읽어 둔다 */

	dev->acs_cap = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_ACS); /* [한국어] ACS 확장 capability 의 오프셋을 캐시한다. 이후 조회가 리스트를 다시 훑지 않는다 */
	pos = dev->acs_cap; /* [한국어] 지역 변수로 옮긴다 */
	if (!pos) /* [한국어] ACS capability 가 없는 장치면 */
		return; /* [한국어] 캐시할 것이 없다 */

	pci_read_config_word(dev, pos + PCI_ACS_CAP, &dev->acs_capabilities); /* [한국어] ACS Capability 레지스터를 읽어 "어떤 기능을 제어할 수 있는가"를 캐시한다 */
	pci_disable_broken_acs_cap(dev); /* [한국어] ACS 를 내보이지만 실제로는 동작하지 않는 하드웨어의 능력 비트를 지우는 quirk */
}

/**
 * pci_enable_atomic_ops_to_root - enable AtomicOp requests to root port
 * @dev: the PCI device
 * @cap_mask: mask of desired AtomicOp sizes, including one or more of:
 *	PCI_EXP_DEVCAP2_ATOMIC_COMP32
 *	PCI_EXP_DEVCAP2_ATOMIC_COMP64
 *	PCI_EXP_DEVCAP2_ATOMIC_COMP128
 *
 * Return 0 if all upstream bridges support AtomicOp routing, egress
 * blocking is disabled on all upstream ports, and the root port supports
 * the requested completion capabilities (32-bit, 64-bit and/or 128-bit
 * AtomicOp completion), or negative otherwise.
 */
/*
 * [한국어]
 * pci_enable_atomic_ops_to_root - 이 장치가 Root Port 로 AtomicOp 을 보낼 수 있게 한다.
 *
 * @dev: AtomicOp 를 보내려는 엔드포인트.
 * @cap_mask: 필요한 완료 크기 조합(32/64/128비트).
 * @return: 0 성공. 경로상 어느 조건이라도 맞지 않으면 -EINVAL.
 *
 * PCIe AtomicOp 는 FetchAdd, Swap, CAS 를 메모리에 대해 원자적으로 수행하는
 * 트랜잭션이다. 이것이 성립하려면 요청자(엔드포인트)부터 완료자(Root Port)
 * 까지 경로 전체가 협조해야 하므로, 검사가 여러 겹이다.
 *
 *   1) VF 는 안 된다. 규격상 Device Control 2 의 AtomicOp Requester Enable
 *      비트가 VF 에서는 예약이고 PF 의 값이 모든 VF 에 적용된다(원본 주석).
 *   2) PCIe 장치여야 한다.
 *   3) 요청자가 될 수 있는 것은 엔드포인트(및 레거시 엔드포인트)뿐이다.
 *      원본 주석대로 이 구현은 완료자를 Root Port 로 한정하고 P2P 는 다루지 않는다.
 *   4) Root Port 가 요청한 크기의 완료를 지원해야 한다.
 *   5) 경로상 모든 스위치 포트가 AtomicOp 라우팅을 지원해야 하고,
 *      업스트림 포트는 Egress Blocking 을 켜 두지 않아야 한다.
 *
 * 5)의 순회에서 fallthrough 가 중요하다. 업스트림 포트는 Egress Blocking 을
 * 확인한 뒤 그대로 아래로 흘러 라우팅 지원까지 검사한다 — 두 조건이 모두
 * 필요하기 때문이다.
 *
 * 모든 검사를 통과한 뒤에야 장치의 AtomicOp Requester Enable 을 켠다.
 *
 * 실행 문맥: config space 를 여러 번 읽는다. 프로세스 문맥.
 *
 * 호출 체인:
 *   AtomicOp 을 쓰려는 드라이버 → [이 함수]
 *     → pcie_find_root_port(), pcie_capability_read_dword(),
 *        pcie_capability_set_word()
 */
int pci_enable_atomic_ops_to_root(struct pci_dev *dev, u32 cap_mask)
{
	struct pci_dev *root, *bridge; /* [한국어] root 는 완료자가 될 Root Port, bridge 는 경로를 훑는 커서 */
	u32 cap, ctl2; /* [한국어] cap 은 읽어 온 Device Capabilities 2, ctl2 는 Device Control 2 */

	/*
	 * Per PCIe r7.0, sec 7.5.3.16, the AtomicOp Requester Enable bit
	 * in Device Control 2 is reserved in VFs and the PF value applies
	 * to all associated VFs.
	 */
	if (dev->is_virtfn) /* [한국어] VF 는 이 비트를 스스로 제어할 수 없다 — PF 의 설정이 모든 VF 에 적용된다 */
		return -EINVAL; /* [한국어] 거절 */

	if (!pci_is_pcie(dev)) /* [한국어] 재래식 PCI 에는 AtomicOp 자체가 없다 */
		return -EINVAL; /* [한국어] 거절 */

	/*
	 * Per PCIe r7.0, sec 6.15, endpoints and root ports may be
	 * AtomicOp requesters.  For now, we only support (legacy) endpoints
	 * as requesters and root ports as completers.  No endpoints as
	 * completers, and no peer-to-peer.
	 */

	switch (pci_pcie_type(dev)) { /* [한국어] 요청자가 될 수 있는 종류인지 확인한다 */
	case PCI_EXP_TYPE_ENDPOINT: /* [한국어] 엔드포인트 */
	case PCI_EXP_TYPE_LEG_END: /* [한국어] 레거시 엔드포인트 — 이 둘만 요청자가 될 수 있다 */
		break; /* [한국어] 검사를 통과했으니 계속 진행한다 */
	default: /* [한국어] 그 밖의 종류(브리지, 스위치 포트 등)는 */
		return -EINVAL; /* [한국어] 이 구현에서 요청자가 될 수 없다 */
	}

	root = pcie_find_root_port(dev); /* [한국어] 완료자가 될 Root Port 를 찾는다 */
	if (!root) /* [한국어] 찾지 못하면 */
		return -EINVAL; /* [한국어] 완료를 받아 줄 곳이 없다 */

	pcie_capability_read_dword(root, PCI_EXP_DEVCAP2, &cap); /* [한국어] Root Port 의 Device Capabilities 2 를 읽는다 */
	if ((cap & cap_mask) != cap_mask) /* [한국어] 요청한 완료 크기를 모두 지원해야 한다. 하나라도 빠지면 */
		return -EINVAL; /* [한국어] 거절 */

	bridge = pci_upstream_bridge(dev); /* [한국어] 경로 순회를 바로 위 브리지에서 시작한다 */
	while (bridge != root) { /* [한국어] Root Port 에 닿을 때까지 중간 스위치 포트를 모두 확인한다 */
		switch (pci_pcie_type(bridge)) { /* [한국어] 포트 종류에 따라 확인할 것이 다르다 */
		case PCI_EXP_TYPE_UPSTREAM: /* [한국어] 스위치의 업스트림 포트 */
			/* Upstream ports must not block AtomicOps on egress */
			pcie_capability_read_dword(bridge, PCI_EXP_DEVCTL2, /* [한국어] Device Control 2 를 읽어 */
						   &ctl2); /* [한국어] 값을 담는다 */
			if (ctl2 & PCI_EXP_DEVCTL2_ATOMIC_EGRESS_BLOCK) /* [한국어] AtomicOp Egress Blocking 이 켜져 있으면 이 포트에서 막힌다 */
				return -EINVAL; /* [한국어] 거절 */
			fallthrough; /* [한국어] 막히지 않았으면 아래 라우팅 검사로 그대로 이어진다 — 업스트림 포트는 두 조건을 모두 만족해야 한다 */

		/* All switch ports need to route AtomicOps */
		case PCI_EXP_TYPE_DOWNSTREAM: /* [한국어] 스위치의 다운스트림 포트(그리고 위에서 흘러온 업스트림 포트) */
			pcie_capability_read_dword(bridge, PCI_EXP_DEVCAP2, /* [한국어] Device Capabilities 2 를 읽어 */
						   &cap); /* [한국어] 값을 담는다 */
			if (!(cap & PCI_EXP_DEVCAP2_ATOMIC_ROUTE)) /* [한국어] AtomicOp Routing 지원 비트가 없으면 이 포트가 AtomicOp 를 전달하지 못한다 */
				return -EINVAL; /* [한국어] 거절 */
			break; /* [한국어] 이 포트는 통과 */
		}

		bridge = pci_upstream_bridge(bridge); /* [한국어] 한 단계 위로 올라간다 */
	}

	pcie_capability_set_word(dev, PCI_EXP_DEVCTL2, /* [한국어] 경로 전체가 조건을 만족했으므로 이제 장치의 요청 기능을 켠다 */
				 PCI_EXP_DEVCTL2_ATOMIC_REQ); /* [한국어] Device Control 2 의 AtomicOp Requester Enable 비트 */
	return 0; /* [한국어] 설정 완료 */
}
EXPORT_SYMBOL(pci_enable_atomic_ops_to_root);

/**
 * pci_release_region - Release a PCI bar
 * @pdev: PCI device whose resources were previously reserved by
 *	  pci_request_region()
 * @bar: BAR to release
 *
 * Releases the PCI I/O and memory resources previously reserved by a
 * successful call to pci_request_region().  Call this function only
 * after all use of the PCI regions has ceased.
 */
/*
 * [한국어]
 * pci_release_region - BAR 하나에 대해 잡아 두었던 자원 예약을 돌려준다.
 *
 * @pdev: 대상 PCI 장치.
 * @bar: 해제할 BAR 번호.
 * @return: 없음.
 *
 * "자원 예약"은 하드웨어를 건드리는 일이 아니라 커널의 자원 트리
 * (/proc/iomem, /proc/ioports 에 보이는 그것)에 이 구간의 주인을 등록하는
 * 일이다. 두 드라이버가 같은 MMIO 창을 동시에 잡는 사고를 막는 장치이며,
 * 등록해 두면 /proc/iomem 에 드라이버 이름이 표시돼 진단에도 쓰인다.
 *
 * IO 공간과 메모리 공간은 커널 안에서 서로 다른 자원 트리에 속하므로
 * 해제 함수도 다르다. 그래서 resource 플래그를 보고 갈래를 탄다.
 *
 * 길이가 0 인 BAR 는 애초에 예약된 적이 없어 그냥 넘어간다.
 *
 * 실행 문맥: 자원 트리를 조작한다. 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_release_selected_regions(), 각 드라이버의 remove → [이 함수]
 *     → release_region() / release_mem_region()
 */
void pci_release_region(struct pci_dev *pdev, int bar)
{
	if (!pci_bar_index_is_valid(bar)) /* [한국어] BAR 번호가 유효 범위 밖이면 인덱싱 자체가 위험하다 */
		return; /* [한국어] 조용히 무시한다 */

	if (pci_resource_len(pdev, bar) == 0) /* [한국어] 구현되지 않았거나 크기가 0 인 BAR 는 예약된 적이 없다 */
		return; /* [한국어] 해제할 것이 없다 */
	if (pci_resource_flags(pdev, bar) & IORESOURCE_IO) /* [한국어] IO 공간 BAR 면 */
		release_region(pci_resource_start(pdev, bar), /* [한국어] IO 자원 트리에서 이 구간을 놓는다 */
				pci_resource_len(pdev, bar)); /* [한국어] 예약했던 것과 같은 시작 주소와 길이를 넘겨야 정확히 그 구간이 풀린다 */
	else if (pci_resource_flags(pdev, bar) & IORESOURCE_MEM) /* [한국어] 메모리 공간 BAR 면 */
		release_mem_region(pci_resource_start(pdev, bar), /* [한국어] 메모리 자원 트리에서 놓는다. IO 와 메모리는 별개의 트리라 함수가 다르다 */
				pci_resource_len(pdev, bar)); /* [한국어] 같은 구간을 지정한다 */
}
EXPORT_SYMBOL(pci_release_region);

/**
 * __pci_request_region - Reserved PCI I/O and memory resource
 * @pdev: PCI device whose resources are to be reserved
 * @bar: BAR to be reserved
 * @name: name of the driver requesting the resource
 * @exclusive: whether the region access is exclusive or not
 *
 * Returns: 0 on success, negative error code on failure.
 *
 * Mark the PCI region associated with PCI device @pdev BAR @bar as being
 * reserved by owner @name. Do not access any address inside the PCI regions
 * unless this call returns successfully.
 *
 * If @exclusive is set, then the region is marked so that userspace
 * is explicitly not allowed to map the resource via /dev/mem or
 * sysfs MMIO access.
 *
 * Returns 0 on success, or %EBUSY on error.  A warning
 * message is also printed on failure.
 */
/*
 * [한국어]
 * __pci_request_region - BAR 하나의 주소 구간을 커널 자원 트리에 예약한다.
 *
 * @pdev: 대상 PCI 장치.
 * @bar: 예약할 BAR 번호.
 * @name: 예약자 이름. /proc/iomem 에 이 이름이 표시된다.
 * @exclusive: 0 이면 보통 예약, IORESOURCE_EXCLUSIVE 면 사용자 공간의 접근까지 막는다.
 * @return: 0 성공(길이 0 인 BAR 도 성공), -EINVAL 잘못된 BAR 번호, -EBUSY 이미 남이 잡고 있음.
 *
 * 이 예약이 하는 일은 하드웨어 설정이 아니라 소유권 등록이다. 예약이 성공해야
 * 그 주소를 ioremap 해 접근해도 안전하다 — 원본 주석이 "이 호출이 성공하기
 * 전에는 그 구간의 어떤 주소도 건드리지 말라"고 못 박는 이유다.
 *
 * @exclusive 는 한 걸음 더 나간다. IORESOURCE_EXCLUSIVE 로 표시된 구간은
 * /dev/mem 이나 sysfs 의 MMIO 통로로도 매핑할 수 없게 된다. 사용자 공간이
 * 드라이버 몰래 레지스터를 건드려 장치를 망가뜨리는 것을 막는 장치다.
 *
 * 길이가 0 인 BAR 에 0(성공)을 돌려주는 것이 중요하다. 구현되지 않은 BAR 를
 * 요청 목록에 넣었다는 이유로 전체 요청이 실패하면 곤란하기 때문이다.
 *
 * 실행 문맥: 자원 트리를 조작한다. 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_request_region(), __pci_request_selected_regions() → [이 함수]
 *     → request_region() / __request_mem_region()
 */
static int __pci_request_region(struct pci_dev *pdev, int bar,
				const char *name, int exclusive)
{
	if (!pci_bar_index_is_valid(bar)) /* [한국어] BAR 번호가 유효 범위 밖이면 */
		return -EINVAL; /* [한국어] 잘못된 요청이다 */

	if (pci_resource_len(pdev, bar) == 0) /* [한국어] 구현되지 않았거나 크기가 0 인 BAR 는 */
		return 0; /* [한국어] 예약할 것이 없으므로 성공으로 처리한다. 여기서 실패로 돌리면 요청 목록에 든 다른 BAR 까지 함께 실패한다 */

	if (pci_resource_flags(pdev, bar) & IORESOURCE_IO) { /* [한국어] IO 공간 BAR 면 */
		if (!request_region(pci_resource_start(pdev, bar), /* [한국어] IO 자원 트리에 등록을 시도한다 */
			    pci_resource_len(pdev, bar), name)) /* [한국어] 시작 주소와 길이, 그리고 소유자 이름 */
			goto err_out; /* [한국어] 이미 다른 주인이 있으면 실패 경로로 */
	} else if (pci_resource_flags(pdev, bar) & IORESOURCE_MEM) { /* [한국어] 메모리 공간 BAR 면 */
		if (!__request_mem_region(pci_resource_start(pdev, bar), /* [한국어] 메모리 자원 트리에 등록을 시도한다 */
					pci_resource_len(pdev, bar), name, /* [한국어] 시작 주소와 길이, 소유자 이름 */
					exclusive)) /* [한국어] exclusive 플래그를 그대로 넘겨 사용자 공간 매핑 차단 여부를 정한다 */
			goto err_out; /* [한국어] 이미 다른 주인이 있으면 실패 경로로 */
	}

	return 0; /* [한국어] 예약 성공 */

err_out: /* [한국어] 두 갈래의 실패가 모이는 지점 */
	pci_warn(pdev, "BAR %d: can't reserve %pR\n", bar, /* [한국어] 어느 BAR 가 왜 실패했는지 남긴다. 자원 충돌은 원인을 찾기 어려워 로그가 중요하다 */
		 &pdev->resource[bar]); /* [한국어] %pR 로 "[mem 0x... - 0x...]" 형태로 구간을 찍는다 */
	return -EBUSY; /* [한국어] 이미 남이 쓰고 있다는 뜻의 -EBUSY */
}

/**
 * pci_request_region - Reserve PCI I/O and memory resource
 * @pdev: PCI device whose resources are to be reserved
 * @bar: BAR to be reserved
 * @name: name of the driver requesting the resource
 *
 * Returns: 0 on success, negative error code on failure.
 *
 * Mark the PCI region associated with PCI device @pdev BAR @bar as being
 * reserved by owner @name. Do not access any address inside the PCI regions
 * unless this call returns successfully.
 *
 * Returns 0 on success, or %EBUSY on error.  A warning
 * message is also printed on failure.
 */
/*
 * [한국어]
 * pci_request_region - BAR 하나를 보통(비독점) 방식으로 예약한다.
 *
 * @pdev: 대상 PCI 장치.
 * @bar: 예약할 BAR 번호.
 * @name: 예약자 이름.
 * @return: 0 성공, -EBUSY 충돌, -EINVAL 잘못된 BAR 번호.
 *
 * exclusive=0 으로 고정한 래퍼다. 사용자 공간이 /dev/mem 등으로 이 구간을
 * 매핑하는 것을 막지 않는다.
 *
 * 실행 문맥: 프로세스 문맥.
 *
 * 호출 체인:
 *   각 드라이버의 probe → [이 함수] → __pci_request_region()
 */
int pci_request_region(struct pci_dev *pdev, int bar, const char *name)
{
	return __pci_request_region(pdev, bar, name, 0); /* [한국어] exclusive=0 — 사용자 공간 매핑을 막지 않는 보통 예약 */
}
EXPORT_SYMBOL(pci_request_region);

/**
 * pci_release_selected_regions - Release selected PCI I/O and memory resources
 * @pdev: PCI device whose resources were previously reserved
 * @bars: Bitmask of BARs to be released
 *
 * Release selected PCI I/O and memory resources previously reserved.
 * Call this function only after all use of the PCI regions has ceased.
 */
/*
 * [한국어]
 * pci_release_selected_regions - 비트마스크로 고른 BAR 들의 예약을 한꺼번에 푼다.
 *
 * @pdev: 대상 PCI 장치.
 * @bars: 해제할 BAR 들의 비트마스크. 비트 i 가 1 이면 BAR i 를 푼다.
 * @return: 없음.
 *
 * 예약할 때 쓴 비트마스크를 그대로 넘겨 대칭을 맞추는 것이 보통이다.
 * 표준 BAR 6개만 대상이며 ROM 이나 VF BAR 는 다루지 않는다.
 *
 * NVMe 와의 관계: drivers/nvme/host/pci.c 가 부르는 pci_release_mem_regions()
 * 는 <linux/pci.h> 의 인라인 함수로, 결국 이 함수에 도달한다.
 * (그 헤더는 이 작업 트리에 포함돼 있지 않아 트리 안에서는 확인할 수 없다.)
 *
 * 실행 문맥: 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_release_regions(), pci_release_mem_regions()(인라인) → [이 함수]
 *     → pci_release_region()
 */
void pci_release_selected_regions(struct pci_dev *pdev, int bars)
{
	int i; /* [한국어] BAR 인덱스 */

	for (i = 0; i < PCI_STD_NUM_BARS; i++) /* [한국어] 표준 BAR 6개만 대상이다 */
		if (bars & (1 << i)) /* [한국어] 비트마스크에 표시된 BAR 만 */
			pci_release_region(pdev, i); /* [한국어] 하나씩 예약을 푼다 */
}
EXPORT_SYMBOL(pci_release_selected_regions);

/*
 * [한국어]
 * __pci_request_selected_regions - 고른 BAR 들을 예약하되, 실패하면 전부 되돌린다.
 *
 * @pdev: 대상 PCI 장치.
 * @bars: 예약할 BAR 들의 비트마스크.
 * @name: 예약자 이름.
 * @excl: 0 이면 보통 예약, IORESOURCE_EXCLUSIVE 면 독점 예약.
 * @return: 0 성공, -EBUSY 이면 하나라도 충돌해 아무 것도 예약되지 않은 상태다.
 *
 * "전부 성공하거나 전부 없던 일이 되거나" 를 보장하는 것이 이 함수의 요점이다.
 * 중간에 실패하면 이미 예약한 BAR 들을 역순으로 풀어, 호출자가 부분적으로
 * 예약된 어정쩡한 상태를 떠안지 않게 한다.
 *
 * 되돌리기 루프의 --i 가 그 장치다. 실패한 시점의 i 는 아직 예약하지 못한
 * BAR 를 가리키므로, 먼저 감소시켜 직전에 성공한 BAR 부터 푼다.
 *
 * 실행 문맥: 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_request_selected_regions(), pci_request_selected_regions_exclusive(),
 *   pci_request_regions() → [이 함수]
 *     → __pci_request_region(), pci_release_region()
 */
static int __pci_request_selected_regions(struct pci_dev *pdev, int bars,
					  const char *name, int excl)
{
	int i; /* [한국어] BAR 인덱스. 실패 시 되돌리기의 시작점으로도 쓰인다 */

	for (i = 0; i < PCI_STD_NUM_BARS; i++) /* [한국어] 표준 BAR 6개만 대상이다 */
		if (bars & (1 << i)) /* [한국어] 비트마스크에 표시된 BAR 만 */
			if (__pci_request_region(pdev, i, name, excl)) /* [한국어] 하나씩 예약을 시도하고, 실패하면 */
				goto err_out; /* [한국어] 이미 예약한 것들을 되돌리러 간다 */
	return 0; /* [한국어] 모두 성공 */

err_out: /* [한국어] 부분 실패를 되돌리는 지점 */
	while (--i >= 0) /* [한국어] i 는 실패한 BAR 를 가리키므로 먼저 감소시켜 직전에 성공한 것부터 푼다 */
		if (bars & (1 << i)) /* [한국어] 예약을 시도했던 BAR 만 */
			pci_release_region(pdev, i); /* [한국어] 되돌린다 */

	return -EBUSY; /* [한국어] 아무 것도 예약되지 않은 상태로 -EBUSY 를 알린다 */
}


/**
 * pci_request_selected_regions - Reserve selected PCI I/O and memory resources
 * @pdev: PCI device whose resources are to be reserved
 * @bars: Bitmask of BARs to be requested
 * @name: Name of the driver requesting the resources
 *
 * Returns: 0 on success, negative error code on failure.
 */
/*
 * [한국어]
 * pci_request_selected_regions - 고른 BAR 들을 보통(비독점) 방식으로 예약한다.
 *
 * @pdev: 대상 PCI 장치.
 * @bars: 예약할 BAR 들의 비트마스크.
 * @name: 예약자 이름.
 * @return: 0 성공, -EBUSY 충돌.
 *
 * excl=0 으로 고정한 래퍼다. 전부 성공하거나 전부 되돌려지는 성질은
 * 하위 함수에서 보장된다.
 *
 * NVMe 와의 관계: drivers/nvme/host/pci.c 의 nvme_dev_map() 이 부르는
 * pci_request_mem_regions() 는 <linux/pci.h> 의 인라인 함수로,
 * pci_select_bars(pdev, IORESOURCE_MEM) 로 메모리 BAR 만 골라 이 함수에
 * 넘긴다. 그 헤더는 이 작업 트리에 없어 트리 안에서는 확인할 수 없다.
 *
 * 실행 문맥: 프로세스 문맥.
 *
 * 호출 체인:
 *   각 드라이버, pci_request_regions(), pci_request_mem_regions()(인라인)
 *     → [이 함수] → __pci_request_selected_regions()
 */
int pci_request_selected_regions(struct pci_dev *pdev, int bars,
				 const char *name)
{
	return __pci_request_selected_regions(pdev, bars, name, 0); /* [한국어] excl=0 — 사용자 공간 매핑을 막지 않는 보통 예약 */
}
EXPORT_SYMBOL(pci_request_selected_regions);

/**
 * pci_request_selected_regions_exclusive - Request regions exclusively
 * @pdev: PCI device to request regions from
 * @bars: bit mask of BARs to request
 * @name: name of the driver requesting the resources
 *
 * Returns: 0 on success, negative error code on failure.
 */
/*
 * [한국어]
 * pci_request_selected_regions_exclusive - 고른 BAR 들을 독점 예약한다.
 *
 * @pdev: 대상 PCI 장치.
 * @bars: 예약할 BAR 들의 비트마스크.
 * @name: 예약자 이름.
 * @return: 0 성공, -EBUSY 충돌.
 *
 * IORESOURCE_EXCLUSIVE 를 넘겨, 사용자 공간이 /dev/mem 이나 sysfs 의 MMIO
 * 통로로 이 구간을 매핑하는 것까지 막는다. 드라이버 몰래 레지스터를 건드려
 * 장치를 망가뜨리는 일을 방지하려는 것이다.
 *
 * 실행 문맥: 프로세스 문맥.
 *
 * 호출 체인:
 *   독점이 필요한 드라이버, pci_request_regions_exclusive() → [이 함수]
 *     → __pci_request_selected_regions()
 */
int pci_request_selected_regions_exclusive(struct pci_dev *pdev, int bars,
					   const char *name)
{
	return __pci_request_selected_regions(pdev, bars, name, /* [한국어] 독점 플래그를 실어 보낸다 */
			IORESOURCE_EXCLUSIVE); /* [한국어] IORESOURCE_EXCLUSIVE — 사용자 공간의 매핑까지 차단한다 */
}
EXPORT_SYMBOL(pci_request_selected_regions_exclusive);

/**
 * pci_release_regions - Release reserved PCI I/O and memory resources
 * @pdev: PCI device whose resources were previously reserved by
 *	  pci_request_regions()
 *
 * Releases all PCI I/O and memory resources previously reserved by a
 * successful call to pci_request_regions().  Call this function only
 * after all use of the PCI regions has ceased.
 */
/*
 * [한국어]
 * pci_release_regions - 표준 BAR 6개의 예약을 모두 푼다.
 *
 * @pdev: 대상 PCI 장치.
 * @return: 없음.
 *
 * (1 << PCI_STD_NUM_BARS) - 1 은 하위 6비트가 모두 1 인 마스크, 즉 BAR 0~5
 * 전부를 뜻한다. pci_request_regions() 의 짝이다.
 *
 * 실행 문맥: 프로세스 문맥.
 *
 * 호출 체인:
 *   각 드라이버의 remove → [이 함수] → pci_release_selected_regions()
 */
void pci_release_regions(struct pci_dev *pdev)
{
	pci_release_selected_regions(pdev, (1 << PCI_STD_NUM_BARS) - 1); /* [한국어] 하위 6비트가 모두 1 인 마스크 — 표준 BAR 전부를 가리킨다 */
}
EXPORT_SYMBOL(pci_release_regions);

/**
 * pci_request_regions - Reserve PCI I/O and memory resources
 * @pdev: PCI device whose resources are to be reserved
 * @name: name of the driver requesting the resources
 *
 * Mark all PCI regions associated with PCI device @pdev as being reserved by
 * owner @name. Do not access any address inside the PCI regions unless this
 * call returns successfully.
 *
 * Returns 0 on success, or %EBUSY on error.  A warning
 * message is also printed on failure.
 */
/*
 * [한국어]
 * pci_request_regions - 표준 BAR 6개를 모두 보통 방식으로 예약한다.
 *
 * @pdev: 대상 PCI 장치.
 * @name: 예약자 이름.
 * @return: 0 성공, -EBUSY 충돌.
 *
 * 구현되지 않은 BAR 는 길이가 0 이라 하위에서 성공으로 처리되므로, 6개를
 * 모두 요청해도 문제가 없다. 하나라도 충돌하면 전부 되돌려진다.
 *
 * 메모리 BAR 만 쓰는 장치라면 IO 자원까지 붙들지 않도록
 * pci_request_mem_regions()(헤더의 인라인 함수)를 쓰는 편이 낫다.
 *
 * 실행 문맥: 프로세스 문맥.
 *
 * 호출 체인:
 *   각 드라이버의 probe → [이 함수] → pci_request_selected_regions()
 */
int pci_request_regions(struct pci_dev *pdev, const char *name)
{
	return pci_request_selected_regions(pdev, /* [한국어] 표준 BAR 전부를 대상으로 */
			((1 << PCI_STD_NUM_BARS) - 1), name); /* [한국어] 하위 6비트가 1 인 마스크와 소유자 이름을 넘긴다 */
}
EXPORT_SYMBOL(pci_request_regions);

/**
 * pci_request_regions_exclusive - Reserve PCI I/O and memory resources
 * @pdev: PCI device whose resources are to be reserved
 * @name: name of the driver requesting the resources
 *
 * Returns: 0 on success, negative error code on failure.
 *
 * Mark all PCI regions associated with PCI device @pdev as being reserved
 * by owner @name. Do not access any address inside the PCI regions
 * unless this call returns successfully.
 *
 * pci_request_regions_exclusive() will mark the region so that /dev/mem
 * and the sysfs MMIO access will not be allowed.
 *
 * Returns 0 on success, or %EBUSY on error.  A warning message is also
 * printed on failure.
 */
/*
 * [한국어]
 * pci_request_regions_exclusive - 표준 BAR 6개를 모두 독점 예약한다.
 *
 * @pdev: 대상 PCI 장치.
 * @name: 예약자 이름.
 * @return: 0 성공, -EBUSY 충돌.
 *
 * pci_request_regions() 와 같지만 IORESOURCE_EXCLUSIVE 가 붙는다. 원본
 * 주석대로 /dev/mem 과 sysfs 의 MMIO 접근이 모두 막힌다.
 *
 * 실행 문맥: 프로세스 문맥.
 *
 * 호출 체인:
 *   독점이 필요한 드라이버 → [이 함수]
 *     → pci_request_selected_regions_exclusive()
 */
int pci_request_regions_exclusive(struct pci_dev *pdev, const char *name)
{
	return pci_request_selected_regions_exclusive(pdev, /* [한국어] 표준 BAR 전부를 독점으로 요청한다 */
				((1 << PCI_STD_NUM_BARS) - 1), name); /* [한국어] 하위 6비트가 1 인 마스크와 소유자 이름 */
}
EXPORT_SYMBOL(pci_request_regions_exclusive);

/*
 * Record the PCI IO range (expressed as CPU physical address + size).
 * Return a negative value if an error has occurred, zero otherwise
 */
/*
 * [한국어]
 * pci_register_io_range - 호스트 브리지가 제공하는 IO 창을 논리 PIO 계층에 등록한다.
 *
 * @fwnode: 이 IO 창을 기술한 펌웨어 노드(DeviceTree/ACPI). 나중에 이 노드로 창을 찾는다.
 * @addr: 그 창이 놓인 CPU 물리 주소.
 * @size: 창의 크기.
 * @return: 0 성공, -EINVAL 잘못된 인자, -ENOMEM 할당 실패.
 *
 * x86 은 IO 공간을 inb/outb 전용 명령으로 접근하지만, ARM 같은 아키텍처에는
 * 그런 명령이 없다. 대신 IO 공간을 메모리 창에 매핑해 두고 그 창에 대한
 * 읽기·쓰기로 흉내 낸다. 그 대응 관계를 관리하는 것이 논리 PIO(logic_pio)
 * 계층이고, 이 함수는 거기에 창 하나를 등록한다.
 *
 * PCI_IOBASE 가 정의되지 않은 아키텍처(x86 등)에서는 본문이 통째로 사라지고
 * 0 만 돌려준다.
 *
 * addr + size < addr 검사는 정수 넘침을 잡는 것이다. 넘치면 창의 끝이
 * 시작보다 앞에 오는 말이 안 되는 구간이 된다.
 *
 * -EEXIST 를 성공으로 바꾸는 것이 미묘하다. 원본 주석대로 지연 프로브
 * (deferred probing) 때문에 같은 창이 두 번 등록될 수 있는데, 이미 등록돼
 * 있다는 것은 목적이 이뤄진 상태이므로 실패로 볼 이유가 없다.
 *
 * 실행 문맥: GFP_ATOMIC 으로 할당한다 — 잠들 수 없는 문맥에서도 부를 수 있게
 * 한 선택이다.
 *
 * 호출 체인:
 *   호스트 브리지 드라이버(controller/), of/acpi 자원 파싱 → [이 함수]
 *     → logic_pio_register_range()
 */
int pci_register_io_range(const struct fwnode_handle *fwnode, phys_addr_t addr,
			resource_size_t	size)
{
	int ret = 0; /* [한국어] PCI_IOBASE 가 없는 아키텍처에서는 아래가 통째로 사라지므로 여기서 0 으로 초기화해 두어야 한다 */
#ifdef PCI_IOBASE /* [한국어] IO 공간을 메모리 창으로 흉내 내는 아키텍처에서만 실제 등록을 한다 */
	struct logic_pio_hwaddr *range; /* [한국어] 논리 PIO 계층에 등록할 창 기술자 */

	if (!size || addr + size < addr) /* [한국어] 크기가 0 이거나, 더했을 때 넘쳐 끝이 시작보다 앞에 오면 */
		return -EINVAL; /* [한국어] 말이 되지 않는 구간이다 */

	range = kzalloc_obj(*range, GFP_ATOMIC); /* [한국어] GFP_ATOMIC 은 잠들 수 없는 문맥에서도 부를 수 있게 하려는 선택이다 */
	if (!range) /* [한국어] 할당 실패 */
		return -ENOMEM; /* [한국어] -ENOMEM */

	range->fwnode = fwnode; /* [한국어] 나중에 이 창을 되찾을 열쇠가 되는 펌웨어 노드 */
	range->size = size; /* [한국어] 창의 크기 */
	range->hw_start = addr; /* [한국어] 창이 놓인 CPU 물리 주소 */
	range->flags = LOGIC_PIO_CPU_MMIO; /* [한국어] "CPU 의 MMIO 로 접근하는 IO 창"이라는 종류 표시 */

	ret = logic_pio_register_range(range); /* [한국어] 논리 PIO 계층에 등록한다 */
	if (ret) /* [한국어] 실패하면 */
		kfree(range); /* [한국어] 기술자를 해제한다. 등록에 성공했다면 소유권이 그쪽으로 넘어가므로 해제하면 안 된다 */

	/* Ignore duplicates due to deferred probing */
	if (ret == -EEXIST) /* [한국어] 이미 같은 창이 등록돼 있다면 — 지연 프로브로 같은 등록이 두 번 오는 경우다 */
		ret = 0; /* [한국어] 목적은 이미 이뤄졌으므로 성공으로 처리한다 */
#endif

	return ret; /* [한국어] PCI_IOBASE 가 없으면 언제나 0 이다 */
}

/*
 * [한국어]
 * pci_pio_to_address - 논리 IO 포트 번호를 CPU 물리 주소로 되돌린다.
 *
 * @pio: 논리 PIO 계층이 부여한 포트 번호.
 * @return: 대응하는 CPU 물리 주소. 변환할 수 없으면 OF_BAD_ADDR.
 *
 * pci_register_io_range() 로 등록해 둔 대응 관계를 거꾸로 조회한다.
 * "IO 포트 0x1000 이 실제로는 어느 물리 주소인가"를 알아야 하는 코드가 쓴다.
 *
 * MMIO_UPPER_LIMIT 는 논리 PIO 가 관리하는 번호 공간의 상한이다. 그보다 큰
 * 값은 애초에 이 계층이 부여한 번호가 아니므로 변환할 수 없다.
 * PCI_IOBASE 가 없는 아키텍처에서는 언제나 OF_BAD_ADDR 다.
 *
 * 실행 문맥: 조회뿐이다.
 *
 * 호출 체인:
 *   IO 자원을 물리 주소로 되돌려야 하는 코드 → [이 함수] → logic_pio_to_hwaddr()
 */
phys_addr_t pci_pio_to_address(unsigned long pio)
{
#ifdef PCI_IOBASE /* [한국어] 논리 PIO 계층이 있는 아키텍처에서만 실제 변환이 가능하다 */
	if (pio < MMIO_UPPER_LIMIT) /* [한국어] 논리 PIO 가 관리하는 번호 공간 안이면 */
		return logic_pio_to_hwaddr(pio); /* [한국어] 등록된 대응 관계로 물리 주소를 찾는다 */
#endif

	return (phys_addr_t) OF_BAD_ADDR; /* [한국어] 변환할 수 없음을 뜻하는 관례적인 값 */
}
EXPORT_SYMBOL_GPL(pci_pio_to_address);

/*
 * [한국어]
 * pci_address_to_pio - CPU 물리 주소를 IO 포트 번호로 옮긴다.
 *
 * @address: 변환할 CPU 물리 주소.
 * @return: 대응하는 IO 포트 번호. 변환할 수 없으면 (unsigned long)-1.
 *
 * pci_pio_to_address() 의 반대 방향이다. __weak 이라 아키텍처가 덮어쓸 수 있다.
 *
 * 두 갈래의 성격이 다르다.
 *   - 논리 PIO 가 있는 아키텍처: 등록된 대응 관계를 조회한다.
 *   - 없는 아키텍처(x86 등): IO 주소 공간과 포트 번호가 같은 값이므로
 *     상한만 확인하고 그대로 캐스팅한다.
 *
 * 실행 문맥: 조회 또는 단순 변환.
 *
 * 호출 체인:
 *   of/acpi 자원 파싱 코드 → [이 함수] → logic_pio_trans_cpuaddr()
 */
unsigned long __weak pci_address_to_pio(phys_addr_t address)
{
#ifdef PCI_IOBASE /* [한국어] 논리 PIO 계층이 있는 아키텍처면 */
	return logic_pio_trans_cpuaddr(address); /* [한국어] 등록된 대응 관계로 포트 번호를 찾는다 */
#else /* [한국어] 없는 아키텍처면 주소와 포트 번호가 같은 값이다 */
	if (address > IO_SPACE_LIMIT) /* [한국어] IO 공간의 상한을 넘으면 */
		return (unsigned long)-1; /* [한국어] 변환할 수 없음을 뜻하는 전부 1 인 값 */

	return (unsigned long) address; /* [한국어] 상한 안이면 값 그대로가 포트 번호다 */
#endif
}

/**
 * pci_remap_iospace - Remap the memory mapped I/O space
 * @res: Resource describing the I/O space
 * @phys_addr: physical address of range to be mapped
 *
 * Remap the memory mapped I/O space described by the @res and the CPU
 * physical address @phys_addr into virtual address space.  Only
 * architectures that have memory mapped IO functions defined (and the
 * PCI_IOBASE value defined) should call this function.
 */
#ifndef pci_remap_iospace /* [한국어] 아키텍처가 같은 이름의 매크로/함수를 정의하지 않았을 때만 이 기본 구현을 쓴다 */
/*
 * [한국어]
 * pci_remap_iospace - IO 공간 창을 커널 가상 주소 공간에 매핑한다.
 *
 * @res: 매핑할 IO 자원(포트 번호 구간).
 * @phys_addr: 그 구간이 놓인 CPU 물리 주소.
 * @return: 0 성공, -EINVAL 이면 IO 자원이 아니거나 범위를 벗어났다.
 *          IO 공간을 메모리로 흉내 내지 않는 아키텍처에서는 -ENODEV.
 *
 * PCI_IOBASE 는 커널이 IO 공간을 통째로 얹어 두는 가상 주소의 기준점이다.
 * 포트 번호 N 은 PCI_IOBASE + N 번지에 대응하며, inb(N) 은 결국 그 주소를
 * 읽는 것이 된다. 이 함수는 그 가상 주소 구간에 실제 물리 주소를 연결한다.
 *
 * pgprot_device(PAGE_KERNEL) 로 매핑하는 것이 중요하다. 장치 메모리 속성이라
 * 캐시되지 않고 쓰기가 재배열되지 않는다 — IO 접근은 순서와 시점이 곧
 * 의미이기 때문이다.
 *
 * IO 공간을 메모리로 흉내 낼 수 없는 아키텍처에서는 애초에 불릴 일이 없어
 * WARN_ONCE 로 호출 자체를 버그로 드러낸다.
 *
 * 실행 문맥: 페이지 테이블을 건드린다. 프로세스 문맥.
 *
 * 호출 체인:
 *   호스트 브리지 드라이버(controller/) → [이 함수] → vmap_page_range()
 */
int pci_remap_iospace(const struct resource *res, phys_addr_t phys_addr)
{
#if defined(PCI_IOBASE) /* [한국어] IO 공간을 메모리 창으로 흉내 내는 아키텍처에서만 실제 매핑이 가능하다 */
	unsigned long vaddr = (unsigned long)PCI_IOBASE + res->start; /* [한국어] 포트 번호 res->start 가 대응하는 커널 가상 주소. PCI_IOBASE 가 IO 공간 전체의 기준점이다 */

	if (!(res->flags & IORESOURCE_IO)) /* [한국어] IO 자원이 아니면 이 경로의 대상이 아니다 */
		return -EINVAL; /* [한국어] 거절 */

	if (res->end > IO_SPACE_LIMIT) /* [한국어] IO 공간의 상한을 넘는 구간은 매핑할 수 없다 */
		return -EINVAL; /* [한국어] 거절 */

	return vmap_page_range(vaddr, vaddr + resource_size(res), phys_addr, /* [한국어] 가상 주소 구간에 물리 주소를 연결한다 */
			       pgprot_device(PAGE_KERNEL)); /* [한국어] 장치 메모리 속성 — 캐시되지 않고 쓰기가 재배열되지 않는다. IO 접근은 순서와 시점이 곧 의미다 */
#else /* [한국어] IO 공간을 메모리로 흉내 내지 않는 아키텍처 */
	/*
	 * This architecture does not have memory mapped I/O space,
	 * so this function should never be called
	 */
	WARN_ONCE(1, "This architecture does not support memory mapped I/O\n"); /* [한국어] 여기에 도달했다는 것 자체가 호출자의 버그다. 한 번만 경고한다 */
	return -ENODEV; /* [한국어] 지원하지 않음을 알린다 */
#endif
}
EXPORT_SYMBOL(pci_remap_iospace);
#endif

/**
 * pci_unmap_iospace - Unmap the memory mapped I/O space
 * @res: resource to be unmapped
 *
 * Unmap the CPU virtual address @res from virtual address space.  Only
 * architectures that have memory mapped IO functions defined (and the
 * PCI_IOBASE value defined) should call this function.
 */
/*
 * [한국어]
 * pci_unmap_iospace - pci_remap_iospace() 로 만든 매핑을 되돌린다.
 *
 * @res: 해제할 IO 자원.
 * @return: 없음.
 *
 * 같은 규칙으로 가상 주소를 계산해 그 구간의 페이지 매핑을 푼다.
 * IO 공간을 메모리로 흉내 내지 않는 아키텍처에서는 본문이 통째로 사라져
 * 아무 일도 하지 않는다 — 그런 곳에서는 매핑을 만든 적도 없기 때문이다.
 *
 * 실행 문맥: 페이지 테이블을 건드린다. 프로세스 문맥.
 *
 * 호출 체인:
 *   호스트 브리지 드라이버의 해제 경로 → [이 함수] → vunmap_range()
 */
void pci_unmap_iospace(struct resource *res)
{
#if defined(PCI_IOBASE) /* [한국어] 매핑을 만든 아키텍처에서만 해제할 것이 있다 */
	unsigned long vaddr = (unsigned long)PCI_IOBASE + res->start; /* [한국어] 매핑할 때와 같은 규칙으로 가상 주소를 계산한다 */

	vunmap_range(vaddr, vaddr + resource_size(res)); /* [한국어] 그 구간의 페이지 매핑을 푼다 */
#endif
}
EXPORT_SYMBOL(pci_unmap_iospace);

/*
 * [한국어]
 * __pci_set_master - Command 레지스터의 Bus Master Enable 비트를 세우거나 지운다.
 *
 * @dev: 대상 PCI 장치.
 * @enable: true 면 BME 를 세우고, false 면 지운다.
 * @return: 없음.
 *
 * 하는 일은 config space 오프셋 0x04(Command)의 비트 2 하나를 바꾸는 것이
 * 전부다. 그러나 그 비트 하나가 "이 장치가 스스로 버스 트랜잭션을 시작할 수
 * 있는가"를 결정한다. 꺼져 있으면 장치는 오직 응답만 할 수 있다 — CPU 가
 * 보낸 읽기·쓰기에 답할 뿐, 자기가 먼저 메모리를 읽거나 쓸 수 없다.
 * 즉 DMA 가 통째로 불가능해진다.
 *
 * NVMe 로 옮겨 보면 이 비트가 왜 결정적인지 분명해진다. NVMe 컨트롤러의
 * 동작은 거의 전부가 장치가 시작하는 DMA 다.
 *   - 호스트가 도어벨을 울리면 컨트롤러가 호스트 메모리의 SQ(제출 큐)를
 *     읽어 명령을 가져온다.  ← 장치가 시작하는 읽기
 *   - PRP 목록이나 SGL 이 가리키는 버퍼에 데이터를 쓰거나 읽는다.
 *     ← 장치가 시작하는 전송
 *   - 완료를 CQ(완료 큐)에 써 넣고 MSI-X 메시지를 보낸다.
 *     ← 장치가 시작하는 쓰기 (MSI-X 도 결국 메모리 쓰기 트랜잭션이다)
 * BME 가 꺼져 있으면 이 셋이 모두 막힌다. 도어벨을 울려도 컨트롤러는 SQ 를
 * 읽어 오지 못하고, 명령은 영원히 완료되지 않는다.
 *
 * 반대로 정리 경로에서는 이 비트를 지우는 것이 안전의 핵심이다. 드라이버가
 * 버퍼를 해제한 뒤에도 장치가 DMA 를 계속하면 남의 메모리를 덮어쓰게 된다.
 *
 * 값이 바뀔 때만 쓰는 것은 불필요한 config 쓰기를 피하려는 것이다. 다만
 * dev->is_busmaster 는 조건과 무관하게 언제나 갱신한다 — 소프트웨어가 보는
 * 상태와 하드웨어가 어긋나면 안 되기 때문이다.
 *
 * 실행 문맥: config space 를 읽고 쓴다. 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_set_master() / pci_clear_master() → [이 함수]
 *     → pci_read_config_word(), pci_write_config_word()
 */
static void __pci_set_master(struct pci_dev *dev, bool enable)
{
	u16 old_cmd, cmd; /* [한국어] old_cmd 는 읽어 온 원래 값, cmd 는 비트를 조작한 새 값 */

	pci_read_config_word(dev, PCI_COMMAND, &old_cmd); /* [한국어] config space 오프셋 0x04 의 Command 레지스터를 읽는다. 다른 비트(MEM/IO 디코딩 등)를 보존하려면 먼저 읽어야 한다 */
	if (enable) /* [한국어] 켜는 경우 */
		cmd = old_cmd | PCI_COMMAND_MASTER; /* [한국어] PCI_COMMAND_MASTER(비트 2)를 세운다. 이 비트가 서야 장치가 스스로 트랜잭션을 시작할 수 있다 */
	else
		cmd = old_cmd & ~PCI_COMMAND_MASTER; /* [한국어] 같은 비트를 지운다. 이 순간부터 장치는 새 DMA 를 시작할 수 없다 */
	if (cmd != old_cmd) { /* [한국어] 값이 실제로 달라졌을 때만 쓴다 — 불필요한 config 쓰기를 피한다 */
		pci_dbg(dev, "%s bus mastering\n", /* [한국어] 상태 변화를 디버그 로그로 남긴다 */
			enable ? "enabling" : "disabling"); /* [한국어] 켜는지 끄는지 */
		pci_write_config_word(dev, PCI_COMMAND, cmd); /* [한국어] 바뀐 Command 값을 써 넣는다. 이 쓰기가 완료되는 순간 하드웨어의 동작이 바뀐다 */
	}
	dev->is_busmaster = enable; /* [한국어] 값이 바뀌지 않았더라도 소프트웨어 표시는 언제나 갱신한다. pci_enable_bridge() 등이 이 필드로 현재 상태를 판단한다 */
}

/**
 * pcibios_setup - process "pci=" kernel boot arguments
 * @str: string used to pass in "pci=" kernel boot arguments
 *
 * Process kernel boot arguments.  This is the default implementation.
 * Architecture specific implementations can override this as necessary.
 */
/*
 * [한국어]
 * pcibios_setup - "pci=" 커널 파라미터 중 아키텍처가 처리할 것을 가로챈다(기본 구현).
 *
 * @str: 아직 아무도 처리하지 않은 "pci=" 옵션 문자열.
 * @return: 처리하지 않고 그대로 돌려주는 문자열. 공통 코드는 이 값을 보고
 *          "알 수 없는 옵션"으로 경고한다.
 *
 * __weak 이라 아키텍처가 덮어쓸 수 있다. x86 처럼 자기만의 pci= 옵션이 있는
 * 아키텍처가 이 훅에서 그것들을 처리하고, 남은 문자열만 돌려준다.
 * 기본 구현은 아무 것도 처리하지 않으므로 받은 문자열을 그대로 돌려준다.
 *
 * 실행 문맥: __init. 부팅 초기 파라미터 파싱 단계.
 *
 * 호출 체인:
 *   pci_setup() → [이 함수]
 */
char * __weak __init pcibios_setup(char *str)
{
	return str; /* [한국어] 아무 것도 처리하지 않았음을 뜻한다. 호출자가 이 문자열을 보고 알 수 없는 옵션으로 경고한다 */
}

/**
 * pcibios_set_master - enable PCI bus-mastering for device dev
 * @dev: the PCI device to enable
 *
 * Enables PCI bus-mastering for the device.  This is the default
 * implementation.  Architecture specific implementations can override
 * this if necessary.
 */
/*
 * [한국어]
 * pcibios_set_master - Bus Master 를 켠 뒤의 아키텍처별 마무리(기본 구현).
 *
 * @dev: 방금 Bus Master 가 된 PCI 장치.
 * @return: 없음.
 *
 * 기본 구현이 하는 일은 Latency Timer 보정 하나다. Latency Timer 는 재래식
 * PCI 의 공유 버스에서 "한 마스터가 버스를 몇 클럭까지 붙들 수 있는가"를
 * 정하는 값이다. 이 값이 0 이면 다른 장치가 버스를 요구하는 즉시 놓아야 해
 * 전송 효율이 크게 떨어지고, 반대로 너무 크면 한 장치가 버스를 독점해
 * 다른 장치의 지연이 커진다.
 * 일부 BIOS 가 이 값을 제대로 설정하지 않은 채 넘기기 때문에 커널이 보정한다.
 *   - 16보다 작으면 64(또는 관리자가 정한 상한)로 올린다.
 *   - 상한을 넘으면 상한으로 낮춘다.
 *   - 그 사이면 손대지 않는다.
 *
 * PCIe 장치는 곧바로 돌아간다. PCIe 는 점대점 링크라 버스를 공유하지 않고,
 * 원본 주석대로 Latency Timer 자체가 적용되지 않는다. NVMe SSD 는 모두
 * 여기에 해당해 이 함수는 사실상 아무 일도 하지 않는다.
 *
 * 실행 문맥: config space 를 읽고 쓴다. 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_set_master() → [이 함수]
 */
void __weak pcibios_set_master(struct pci_dev *dev)
{
	u8 lat; /* [한국어] Latency Timer 레지스터 값 */

	/* The latency timer doesn't apply to PCIe (either Type 0 or Type 1) */
	if (pci_is_pcie(dev)) /* [한국어] PCIe 는 점대점 링크라 버스를 공유하지 않으므로 Latency Timer 가 의미가 없다. NVMe SSD 는 모두 여기서 돌아간다 */
		return; /* [한국어] 할 일이 없다 */

	pci_read_config_byte(dev, PCI_LATENCY_TIMER, &lat); /* [한국어] config space 오프셋 0x0d 의 Latency Timer 를 읽는다 */
	if (lat < 16) /* [한국어] 16 미만이면 버스를 너무 빨리 놓아 전송 효율이 나쁘다 */
		lat = (64 <= pcibios_max_latency) ? 64 : pcibios_max_latency; /* [한국어] 64 로 올리되, 관리자가 정한 상한(pcibios_max_latency)을 넘지 않게 한다 */
	else if (lat > pcibios_max_latency) /* [한국어] 상한보다 크면 */
		lat = pcibios_max_latency; /* [한국어] 상한으로 낮춘다. 한 장치가 버스를 독점하지 않게 하는 것이다 */
	else
		return; /* [한국어] 손대지 않고 그대로 둔다. 아래 쓰기도 건너뛴다 */

	pci_write_config_byte(dev, PCI_LATENCY_TIMER, lat); /* [한국어] 보정한 값을 써 넣는다 */
}

/**
 * pci_set_master - enables bus-mastering for device dev
 * @dev: the PCI device to enable
 *
 * Enables bus-mastering on the device and calls pcibios_set_master()
 * to do the needed arch specific settings.
 */
/*
 * [한국어]
 * pci_set_master - 이 장치가 DMA 를 시작할 수 있게 한다.
 *
 * @dev: Bus Master 로 만들 PCI 장치.
 * @return: 없음.
 *
 * 두 단계로 이뤄진다. 먼저 Command 레지스터의 Bus Master Enable 비트를
 * 세우고, 그 다음 아키텍처별 마무리를 부른다.
 *
 * 이 함수 하나가 장치를 "응답만 하는 존재"에서 "스스로 버스 트랜잭션을
 * 시작하는 존재"로 바꾼다. BAR 를 매핑해 레지스터를 읽고 쓸 수 있게 되는
 * 것과는 전혀 다른 권한이다 — 그쪽은 CPU 가 장치를 향해 가는 방향이고,
 * 이쪽은 장치가 메모리를 향해 가는 방향이다.
 *
 * NVMe 와의 관계: drivers/nvme/host/pci.c 의 nvme_pci_enable() 이 부른다.
 * 이 호출이 없으면 컨트롤러는 도어벨을 받아도 SQ 를 읽어 오지 못하고,
 * PRP/SGL 이 가리키는 버퍼로 데이터를 옮기지도, CQ 에 완료를 써 넣지도
 * 못한다. 즉 NVMe 의 동작 전체가 성립하지 않는다.
 *
 * 참고로 이 함수는 되돌릴 수 있는 참조 계수를 두지 않는다. 여러 번 불러도
 * 같은 상태이며, 끄려면 pci_clear_master() 나 pci_disable_device() 를 쓴다.
 *
 * 실행 문맥: config space 를 읽고 쓴다. 프로세스 문맥.
 *
 * 호출 체인:
 *   drivers/nvme/host/pci.c 의 nvme_pci_enable(), 각 드라이버의 probe,
 *   pci_enable_bridge() → [이 함수]
 *     → __pci_set_master(), pcibios_set_master()
 */
void pci_set_master(struct pci_dev *dev)
{
	__pci_set_master(dev, true); /* [한국어] Command 의 Bus Master Enable 비트를 세운다. 이 순간부터 장치가 DMA 를 시작할 수 있다 */
	pcibios_set_master(dev); /* [한국어] 아키텍처별 마무리. 기본 구현은 재래식 PCI 의 Latency Timer 를 보정하며, PCIe 장치에서는 곧바로 돌아간다 */
}
EXPORT_SYMBOL(pci_set_master);

/**
 * pci_clear_master - disables bus-mastering for device dev
 * @dev: the PCI device to disable
 */
/*
 * [한국어]
 * pci_clear_master - 이 장치가 더 이상 DMA 를 시작하지 못하게 한다.
 *
 * @dev: 대상 PCI 장치.
 * @return: 없음.
 *
 * pci_set_master() 의 짝이다. Command 의 Bus Master Enable 비트를 지운다.
 * 이 순간 이후 장치는 새로운 버스 트랜잭션을 시작할 수 없다.
 *
 * 정리·리셋 경로에서 중요하다. 드라이버가 DMA 버퍼를 해제했는데 장치가
 * 여전히 그 주소로 쓰고 있으면 이미 다른 용도로 재사용된 메모리를
 * 덮어쓰게 되고, 원인을 찾기 매우 어려운 손상이 된다.
 *
 * 아키텍처 마무리(pcibios_set_master)를 부르지 않는 것이 pci_set_master()
 * 와의 차이다. Latency Timer 보정은 켤 때만 의미가 있기 때문이다.
 *
 * 실행 문맥: config space 를 읽고 쓴다. 프로세스 문맥.
 *
 * 호출 체인:
 *   각 드라이버의 remove/오류 경로 → [이 함수] → __pci_set_master()
 */
void pci_clear_master(struct pci_dev *dev)
{
	__pci_set_master(dev, false); /* [한국어] Bus Master Enable 비트를 지운다. 이 순간 이후 장치는 새 DMA 를 시작할 수 없다 */
}
EXPORT_SYMBOL(pci_clear_master);

/**
 * pci_set_cacheline_size - ensure the CACHE_LINE_SIZE register is programmed
 * @dev: the PCI device for which MWI is to be enabled
 *
 * Helper function for pci_set_mwi.
 * Originally copied from drivers/net/acenic.c.
 * Copyright 1998-2001 by Jes Sorensen, <jes@trained-monkey.org>.
 *
 * RETURNS: An appropriate -ERRNO error value on error, or zero for success.
 */
/*
 * [한국어]
 * pci_set_cacheline_size - Cache Line Size 레지스터에 올바른 값을 넣는다.
 *
 * @dev: 대상 PCI 장치.
 * @return: 0 성공, -EINVAL 이면 값을 정할 수 없거나 장치가 그 값을 받지 않는다.
 *
 * config space 오프셋 0x0c 의 Cache Line Size 는 이름 그대로 이 시스템의
 * 캐시라인 크기를 장치에 알려 주는 값이다. 단위가 바이트가 아니라 32비트
 * 워드라는 점이 함정이다 — 64바이트 캐시라인이면 16 을 넣는다. 로그에서
 * << 2(×4)로 되돌려 바이트로 찍는 것이 그 때문이다.
 *
 * 이 값이 필요한 이유는 MWI(Memory Write and Invalidate) 때문이다. MWI 는
 * "캐시라인 하나를 통째로 덮어쓸 테니 읽어 올 필요 없이 무효화하라"는
 * 트랜잭션이라, 장치가 캐시라인 크기를 정확히 알아야 성립한다.
 *
 * 이미 들어 있는 값이 올바른 값의 배수라면 그대로 둔다 — 더 큰 배수도
 * 캐시라인 경계를 침범하지 않아 안전하기 때문이다.
 *
 * 써 넣은 뒤 되읽어 확인하는 이유는 이 레지스터가 읽기 전용이거나 특정
 * 값만 받는 장치가 있기 때문이다. 그런 장치에는 MWI 를 켤 수 없다.
 *
 * 실행 문맥: config space 를 읽고 쓴다. 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_set_mwi() → [이 함수] → pci_read/write_config_byte()
 */
int pci_set_cacheline_size(struct pci_dev *dev)
{
	u8 cacheline_size; /* [한국어] 현재 레지스터 값 */

	if (!pci_cache_line_size) /* [한국어] 아키텍처가 캐시라인 크기를 알려 주지 않았으면 넣을 값이 없다 */
		return -EINVAL; /* [한국어] 설정할 수 없다 */

	/* Validate current setting: the PCI_CACHE_LINE_SIZE must be
	   equal to or multiple of the right value. */
	/* [한국어] 위 영어 주석대로, 이미 설정된 값이 올바른 값의 배수이기만 하면
	 * 그대로 둔다. 캐시라인보다 큰 값은 성능을 조금 손해 볼 뿐 정확성에는
	 * 문제가 없기 때문이다. 펌웨어가 더 큰 값을 넣어 둔 경우를 존중한다. */
	pci_read_config_byte(dev, PCI_CACHE_LINE_SIZE, &cacheline_size); /* [한국어] 현재 값을 읽는다 */
	if (cacheline_size >= pci_cache_line_size && /* [한국어] 올바른 값 이상이면서 */
	    (cacheline_size % pci_cache_line_size) == 0) /* [한국어] 그 배수이면 캐시라인 경계를 침범하지 않아 그대로 두어도 안전하다 */
		return 0; /* [한국어] 손대지 않고 성공으로 끝낸다 */

	/* Write the correct value. */
	pci_write_config_byte(dev, PCI_CACHE_LINE_SIZE, pci_cache_line_size); /* [한국어] 올바른 값을 써 넣는다 */
	/* Read it back. */
	pci_read_config_byte(dev, PCI_CACHE_LINE_SIZE, &cacheline_size); /* [한국어] 되읽어 실제로 반영됐는지 확인한다 — 이 레지스터를 읽기 전용으로 둔 장치가 있다 */
	if (cacheline_size == pci_cache_line_size) /* [한국어] 값이 그대로 들어갔으면 */
		return 0; /* [한국어] 성공 */

	pci_dbg(dev, "cache line size of %d is not supported\n", /* [한국어] 장치가 이 값을 받지 않는다는 사실을 남긴다 */
		   pci_cache_line_size << 2); /* [한국어] 저장 단위가 32비트 워드라 << 2(×4)로 바이트 단위로 되돌려 찍는다 */

	return -EINVAL; /* [한국어] MWI 를 켤 수 없다 */
}
EXPORT_SYMBOL_GPL(pci_set_cacheline_size);

/**
 * pci_set_mwi - enables memory-write-invalidate PCI transaction
 * @dev: the PCI device for which MWI is enabled
 *
 * Enables the Memory-Write-Invalidate transaction in %PCI_COMMAND.
 *
 * RETURNS: An appropriate -ERRNO error value on error, or zero for success.
 */
/*
 * [한국어]
 * pci_set_mwi - Memory Write and Invalidate 트랜잭션을 켠다.
 *
 * @dev: 대상 PCI 장치.
 * @return: 0 성공, 음수면 켤 수 없다.
 *
 * MWI 는 재래식 PCI 의 성능 기능이다. 보통의 메모리 쓰기는 캐시라인의 일부만
 * 바꿀 수 있어 캐시가 원본을 먼저 읽어 와야 하지만(read-for-ownership),
 * MWI 는 "이 캐시라인을 통째로 덮어쓴다"고 선언하므로 그 읽기를 건너뛸 수
 * 있다. 큰 DMA 쓰기에서 메모리 대역폭이 눈에 띄게 줄어든다.
 *
 * 성립 조건이 Cache Line Size 레지스터라, 그것을 먼저 맞춘 뒤에야 Command 의
 * MWI 비트를 켠다.
 *
 * PCIe 에는 MWI 가 없다. PCIe 트랜잭션에는 이 개념 자체가 없어 Command 의
 * 해당 비트는 무시된다. NVMe SSD 를 포함한 모든 PCIe 장치에서 이 함수는
 * 의미가 없으며, 실제로 drivers/nvme/ 에는 호출이 없다.
 * 일부 아키텍처는 PCI_DISABLE_MWI 를 정의해 이 기능을 통째로 무력화한다.
 *
 * 실행 문맥: config space 를 읽고 쓴다. 프로세스 문맥.
 *
 * 호출 체인:
 *   재래식 PCI 드라이버, pci_try_set_mwi() → [이 함수]
 *     → pci_set_cacheline_size(), pci_write_config_word()
 */
int pci_set_mwi(struct pci_dev *dev)
{
#ifdef PCI_DISABLE_MWI /* [한국어] 아키텍처가 MWI 를 쓰지 않기로 했으면 */
	return 0; /* [한국어] 아무 일도 하지 않고 성공으로 처리한다 */
#else /* [한국어] MWI 를 쓰는 아키텍처 */
	int rc; /* [한국어] Cache Line Size 설정 결과 */
	u16 cmd; /* [한국어] Command 레지스터 값 */

	rc = pci_set_cacheline_size(dev); /* [한국어] MWI 는 캐시라인 크기를 장치가 알아야 성립하므로 먼저 맞춘다 */
	if (rc) /* [한국어] 맞추지 못했으면 */
		return rc; /* [한국어] MWI 를 켤 수 없다 */

	pci_read_config_word(dev, PCI_COMMAND, &cmd); /* [한국어] Command 레지스터를 읽는다 */
	if (!(cmd & PCI_COMMAND_INVALIDATE)) { /* [한국어] MWI 비트가 아직 꺼져 있으면 */
		pci_dbg(dev, "enabling Mem-Wr-Inval\n"); /* [한국어] 켠다는 사실을 남기고 */
		cmd |= PCI_COMMAND_INVALIDATE; /* [한국어] PCI_COMMAND_INVALIDATE(비트 4)를 세운다 */
		pci_write_config_word(dev, PCI_COMMAND, cmd); /* [한국어] 써 넣는다 */
	}
	return 0; /* [한국어] 성공 */
#endif
}
EXPORT_SYMBOL(pci_set_mwi);

/**
 * pci_try_set_mwi - enables memory-write-invalidate PCI transaction
 * @dev: the PCI device for which MWI is enabled
 *
 * Enables the Memory-Write-Invalidate transaction in %PCI_COMMAND.
 * Callers are not required to check the return value.
 *
 * RETURNS: An appropriate -ERRNO error value on error, or zero for success.
 */
/*
 * [한국어]
 * pci_try_set_mwi - MWI 를 켜 보되, 실패해도 호출자가 신경 쓰지 않아도 되는 판.
 *
 * @dev: 대상 PCI 장치.
 * @return: pci_set_mwi() 의 결과. 원본 주석대로 호출자가 확인하지 않아도 된다.
 *
 * 현재 구현은 pci_set_mwi() 를 그대로 부른다. 반환값을 무시해도 좋다는
 * 계약을 이름으로 드러내는 것이 존재 이유이며, 그래서 MWI 를 켜지 못해도
 * 동작에 지장이 없는 드라이버가 이 쪽을 쓴다.
 *
 * 실행 문맥: 프로세스 문맥.
 *
 * 호출 체인:
 *   MWI 가 없어도 무방한 드라이버 → [이 함수] → pci_set_mwi()
 */
int pci_try_set_mwi(struct pci_dev *dev)
{
#ifdef PCI_DISABLE_MWI /* [한국어] 아키텍처가 MWI 를 쓰지 않기로 했으면 */
	return 0; /* [한국어] 아무 일도 하지 않는다 */
#else /* [한국어] MWI 를 쓰는 아키텍처면 */
	return pci_set_mwi(dev); /* [한국어] 평범하게 켜기를 시도한다 */
#endif
}
EXPORT_SYMBOL(pci_try_set_mwi);

/**
 * pci_clear_mwi - disables Memory-Write-Invalidate for device dev
 * @dev: the PCI device to disable
 *
 * Disables PCI Memory-Write-Invalidate transaction on the device
 */
/*
 * [한국어]
 * pci_clear_mwi - Memory Write and Invalidate 를 끈다.
 *
 * @dev: 대상 PCI 장치.
 * @return: 없음.
 *
 * pci_set_mwi() 의 짝이다. Command 의 MWI 비트만 지운다. Cache Line Size 는
 * 되돌리지 않는데, 그 값은 MWI 와 무관하게 시스템의 사실을 담고 있어
 * 남겨 두어도 해가 없기 때문이다.
 *
 * PCI_DISABLE_MWI 인 아키텍처에서는 본문이 통째로 사라진다 — 켠 적이 없으니
 * 끌 것도 없다.
 *
 * 실행 문맥: config space 를 읽고 쓴다. 프로세스 문맥.
 *
 * 호출 체인:
 *   재래식 PCI 드라이버의 정리 경로 → [이 함수] → pci_write_config_word()
 */
void pci_clear_mwi(struct pci_dev *dev)
{
#ifndef PCI_DISABLE_MWI /* [한국어] MWI 를 쓰는 아키텍처에서만 끌 것이 있다 */
	u16 cmd; /* [한국어] Command 레지스터 값 */

	pci_read_config_word(dev, PCI_COMMAND, &cmd); /* [한국어] 현재 값을 읽는다 */
	if (cmd & PCI_COMMAND_INVALIDATE) { /* [한국어] MWI 비트가 켜져 있을 때만 손댄다 */
		cmd &= ~PCI_COMMAND_INVALIDATE; /* [한국어] 그 비트만 지운다. 다른 비트는 보존한다 */
		pci_write_config_word(dev, PCI_COMMAND, cmd); /* [한국어] 써 넣는다 */
	}
#endif
}
EXPORT_SYMBOL(pci_clear_mwi);

/**
 * pci_disable_parity - disable parity checking for device
 * @dev: the PCI device to operate on
 *
 * Disable parity checking for device @dev
 */
/*
 * [한국어]
 * pci_disable_parity - 이 장치의 패리티 오류 응답을 끈다.
 *
 * @dev: 대상 PCI 장치.
 * @return: 없음.
 *
 * Command 레지스터의 Parity Error Response(비트 6)를 지운다. 이 비트가
 * 켜져 있으면 장치가 패리티 오류를 감지했을 때 그것을 오류로 보고하고,
 * 꺼져 있으면 감지는 하되 보고하지 않는다.
 *
 * 끄는 이유는 성능이 아니라 오작동 회피다. 패리티 신호를 잘못 내는 하드웨어
 * 때문에 끝없는 오류 보고가 발생하는 경우가 있어, quirk 가 그런 장치의
 * 보고를 막는다.
 *
 * 이미 꺼져 있으면 쓰지 않는다.
 *
 * 실행 문맥: config space 를 읽고 쓴다. 프로세스 문맥.
 *
 * 호출 체인:
 *   drivers/pci/quirks.c → [이 함수] → pci_read/write_config_word()
 */
void pci_disable_parity(struct pci_dev *dev)
{
	u16 cmd; /* [한국어] Command 레지스터 값 */

	pci_read_config_word(dev, PCI_COMMAND, &cmd); /* [한국어] 현재 값을 읽는다 */
	if (cmd & PCI_COMMAND_PARITY) { /* [한국어] 패리티 오류 응답이 켜져 있을 때만 손댄다 */
		cmd &= ~PCI_COMMAND_PARITY; /* [한국어] 그 비트만 지운다 */
		pci_write_config_word(dev, PCI_COMMAND, cmd); /* [한국어] 써 넣는다 */
	}
}

/**
 * pci_intx - enables/disables PCI INTx for device dev
 * @pdev: the PCI device to operate on
 * @enable: boolean: whether to enable or disable PCI INTx
 *
 * Enables/disables PCI INTx for device @pdev
 */
/*
 * [한국어]
 * pci_intx - 레거시 INTx 인터럽트를 켜거나 끈다.
 *
 * @pdev: 대상 PCI 장치.
 * @enable: 0 이 아니면 INTx 허용, 0 이면 금지.
 * @return: 없음.
 *
 * 비트의 뜻이 뒤집혀 있다는 점이 요점이다. Command 레지스터의 비트 10 은
 * "INTx Enable" 이 아니라 "INTx Disable" 이다. 그래서 켜려면 비트를 지우고,
 * 끄려면 비트를 세운다. 이 반전 때문에 코드를 잘못 읽기 쉽다.
 *
 * INTx 는 PCIe 에서도 메시지로 흉내 내어 남아 있지만, MSI/MSI-X 와 함께
 * 쓸 수는 없다. MSI 를 쓰는 중에 INTx 가 열려 있으면 가짜 인터럽트가 생기고,
 * 반대로 INTx 를 쓰려는데 막혀 있으면 인터럽트가 오지 않는다.
 *
 * 값이 바뀌지 않으면 쓰지 않는다.
 *
 * NVMe 와의 관계: NVMe 는 MSI-X 를 쓰므로 정상 경로에서 INTx 를 열지 않는다.
 * do_pci_enable_device() 가 msi_enabled/msix_enabled 를 먼저 보고 INTx 를
 * 건드리지 않는 것이 그 때문이다.
 *
 * 실행 문맥: config space 를 읽고 쓴다. 프로세스 문맥.
 *
 * 호출 체인:
 *   각 드라이버, MSI 코드 → [이 함수] → pci_read/write_config_word()
 */
void pci_intx(struct pci_dev *pdev, int enable)
{
	u16 pci_command, new; /* [한국어] pci_command 는 읽어 온 원래 값, new 는 비트를 조작한 새 값 */

	pci_read_config_word(pdev, PCI_COMMAND, &pci_command); /* [한국어] Command 레지스터를 읽는다 */

	if (enable) /* [한국어] INTx 를 허용하려면 */
		new = pci_command & ~PCI_COMMAND_INTX_DISABLE; /* [한국어] INTx Disable 비트를 지운다 — 이름이 Disable 이라 켜는 쪽이 "지우기"다 */
	else
		new = pci_command | PCI_COMMAND_INTX_DISABLE; /* [한국어] 같은 비트를 세운다 */

	if (new == pci_command) /* [한국어] 값이 그대로면 */
		return; /* [한국어] 불필요한 config 쓰기를 하지 않는다 */

	pci_write_config_word(pdev, PCI_COMMAND, new); /* [한국어] 바뀐 값을 써 넣는다 */
}
EXPORT_SYMBOL_GPL(pci_intx);

/**
 * pci_wait_for_pending_transaction - wait for pending transaction
 * @dev: the PCI device to operate on
 *
 * Return 0 if transaction is pending 1 otherwise.
 */
/*
 * [한국어]
 * pci_wait_for_pending_transaction - 미완료 PCIe 트랜잭션이 없어질 때까지 기다린다.
 *
 * @dev: 대상 PCI 장치.
 * @return: 미완료 트랜잭션이 없어졌으면 1, 시간 안에 없어지지 않으면 0.
 *          PCIe 가 아니면 확인할 방법이 없어 1(문제 없음)로 본다.
 *
 * 리셋을 걸기 전 반드시 밟아야 하는 단계다. 장치가 이미 내보낸 읽기 요청이
 * 남아 있는데 리셋해 버리면, 뒤늦게 돌아온 완료 패킷이 갈 곳을 잃는다.
 * 그 결과는 보통 Unexpected Completion 오류이며, 심하면 시스템 오류가 된다.
 *
 * 확인 대상은 PCIe capability 의 Device Status 레지스터에 있는
 * Transaction Pending 비트다. 이 비트는 "이 함수가 발행했고 아직 완료를
 * 받지 못한 요청이 있다"를 뜻한다.
 *
 * NVMe 로 옮기면, 컨트롤러가 SQ 를 읽거나 데이터 버퍼를 읽는 요청이 아직
 * 공중에 떠 있는 상태를 말한다. 다만 이것은 PCIe 링크 위의 트랜잭션이지
 * NVMe 명령 큐의 상태가 아니다 — 큐를 비우는 것은 NVMe 드라이버가 따로 한다.
 *
 * 실행 문맥: 하위에서 msleep 으로 잔다. 프로세스 문맥.
 *
 * 호출 체인:
 *   pcie_flr() → [이 함수] → pci_wait_for_pending()
 */
int pci_wait_for_pending_transaction(struct pci_dev *dev)
{
	if (!pci_is_pcie(dev)) /* [한국어] 재래식 PCI 에는 Transaction Pending 비트가 없다 */
		return 1; /* [한국어] 확인할 방법이 없으므로 문제 없음(1)으로 본다 */

	return pci_wait_for_pending(dev, pci_pcie_cap(dev) + PCI_EXP_DEVSTA, /* [한국어] PCIe capability 시작 + PCI_EXP_DEVSTA 위치의 Device Status 를 지켜본다 */
				    PCI_EXP_DEVSTA_TRPND); /* [한국어] 그중 Transaction Pending 비트가 내려갈 때까지 기다린다 */
}
EXPORT_SYMBOL(pci_wait_for_pending_transaction);

/**
 * pcie_flr - initiate a PCIe function level reset
 * @dev: device to reset
 *
 * Initiate a function level reset unconditionally on @dev without
 * checking any flags and DEVCAP
 */
/*
 * [한국어]
 * pcie_flr - PCIe Function Level Reset 을 조건 없이 실행한다.
 *
 * @dev: 리셋할 PCIe 장치.
 * @return: 0 성공. 음수면 IOMMU 준비 실패이거나, 리셋 후 장치가 되살아나지 않았다.
 *
 * FLR 은 "이 함수 하나만" 초기 상태로 되돌리는 리셋이다. 링크는 살아 있고
 * 같은 장치의 다른 함수나 형제 장치에는 영향이 없다. 그래서 한 장치만
 * 되살리고 싶을 때 가장 먼저 시도하는 방법이다.
 *
 * FLR 이 지우는 것과 남기는 것을 구분하는 것이 중요하다.
 *   - 지워지는 것: BAR, Command 레지스터, MSI/MSI-X 설정, 장치 내부 상태.
 *     그래서 리셋 전에 pci_save_state() 로 떠 두고 나중에
 *     pci_restore_state() 로 되돌려야 한다.
 *   - 남는 것: 링크 자체와 링크 파라미터. 재훈련이 필요 없다.
 *
 * 절차의 각 단계에 이유가 있다.
 *   1) 미완료 트랜잭션이 없어지기를 기다린다. 시간이 지나도 남아 있으면
 *      로그를 남기고 그대로 진행한다 — 리셋을 포기하는 것보다 낫다는 판단이다.
 *   2) IOMMU 를 준비시킨다. 원본 주석대로 이 순서여야 한다. DMA 가 멎기
 *      전에 IOMMU 매핑을 걷으면 아직 떠 있는 요청이 변환에 실패한다.
 *   3) Device Control 의 Initiate Function Level Reset 비트를 쓴다.
 *      쓰는 순간 리셋이 시작된다.
 *   4) 100ms 기다린다. 원본 주석대로 규격은 100ms 안에 FLR 을 끝내라고
 *      요구하되, 그동안 요청을 조용히 버려도 된다고 허용한다. 그래서 그
 *      시간 동안은 아예 건드리지 않는다.
 *   5) 장치가 config 요청에 응답할 때까지 최대 60초까지 기다린다.
 *   dev->imm_ready 는 "리셋 직후 즉시 응답한다"고 선언한 장치의 표시로,
 *   그런 장치는 4)와 5)를 건너뛴다.
 *
 * NVMe 와의 관계: drivers/nvme/host/pci.c 가 컨트롤러 리셋 경로에서
 * pcie_reset_flr() 를 통해 이 함수에 도달한다. 컨트롤러가 응답하지 않을 때
 * 소프트웨어 수준의 리셋(CC.EN 조작)으로 되살아나지 않으면 PCI 층의 FLR 로
 * 더 강하게 초기화하는 것이다.
 *
 * 실행 문맥: msleep 으로 잔다. 반드시 프로세스 문맥.
 *
 * 호출 체인:
 *   pcie_reset_flr(), drivers/nvme/host/pci.c → [이 함수]
 *     → pci_wait_for_pending_transaction(), pci_dev_reset_iommu_prepare(),
 *        pcie_capability_set_word(), pci_dev_wait()
 */
int pcie_flr(struct pci_dev *dev)
{
	int ret; /* [한국어] pci_dev_wait() 와 IOMMU 준비의 결과 */

	if (!pci_wait_for_pending_transaction(dev)) /* [한국어] 미완료 트랜잭션이 시간 안에 사라지지 않으면 */
		pci_err(dev, "timed out waiting for pending transaction; performing function level reset anyway\n"); /* [한국어] 사실만 남기고 그대로 진행한다. 리셋을 포기하는 것보다 낫다는 판단이다 */

	/* Have to call it after waiting for pending DMA transaction */
	ret = pci_dev_reset_iommu_prepare(dev); /* [한국어] IOMMU 쪽 준비. 원본 주석대로 DMA 대기 뒤에 와야 한다 — 아직 떠 있는 요청이 변환에 실패하지 않도록 */
	if (ret) { /* [한국어] 준비에 실패하면 */
		pci_err(dev, "failed to stop IOMMU for a PCI reset: %d\n", ret); /* [한국어] 이유를 남기고 */
		return ret; /* [한국어] 리셋을 포기한다. 여기서는 아직 아무 것도 되돌릴 것이 없다 */
	}

	pcie_capability_set_word(dev, PCI_EXP_DEVCTL, PCI_EXP_DEVCTL_BCR_FLR); /* [한국어] Device Control 의 Initiate Function Level Reset 비트를 쓴다. 이 쓰기가 곧 리셋의 시작이다 */

	if (dev->imm_ready) /* [한국어] "리셋 직후 즉시 응답한다"고 선언한 장치라면 */
		goto done; /* [한국어] 대기를 건너뛴다 */

	/*
	 * Per PCIe r4.0, sec 6.6.2, a device must complete an FLR within
	 * 100ms, but may silently discard requests while the FLR is in
	 * progress.  Wait 100ms before trying to access the device.
	 */
	msleep(100); /* [한국어] 규격이 정한 100ms. 이 시간 동안 장치는 요청을 조용히 버려도 되므로 아예 건드리지 않는다 */

	ret = pci_dev_wait(dev, "FLR", PCIE_RESET_READY_POLL_MS); /* [한국어] 그 뒤 장치가 config 요청에 정상 응답할 때까지 최대 60초까지 기다린다 */
done: /* [한국어] imm_ready 장치가 건너뛰어 오는 지점 */
	pci_dev_reset_iommu_done(dev); /* [한국어] IOMMU 쪽 준비를 되돌린다. 성공·실패 모두 반드시 거쳐야 짝이 맞는다 */
	return ret; /* [한국어] pci_dev_wait() 의 결과가 곧 FLR 의 결과다 */
}
EXPORT_SYMBOL_GPL(pcie_flr);

/**
 * pcie_reset_flr - initiate a PCIe function level reset
 * @dev: device to reset
 * @probe: if true, return 0 if device can be reset this way
 *
 * Initiate a function level reset on @dev.
 */
/*
 * [한국어]
 * pcie_reset_flr - FLR 가능 여부를 확인하거나 실제로 FLR 을 실행한다.
 *
 * @dev: 대상 PCIe 장치.
 * @probe: true 면 "이 방법을 쓸 수 있는가"만 판정하고 실제 리셋은 하지 않는다.
 * @return: 0 이면 가능(probe) 또는 성공(실행). -ENOTTY 는 이 방법을 쓸 수 없다는 뜻이다.
 *
 * 이 파일의 리셋 방법들은 모두 이 "probe 겸 실행" 형태를 따른다.
 * pci_init_reset_methods() 가 probe=true 로 불러 쓸 수 있는 방법 목록을
 * 만들고, 실제 리셋 때는 그 목록을 순서대로 probe=false 로 부른다.
 * 덕분에 판정 조건과 실행 코드가 한곳에 있어 둘이 어긋날 수 없다.
 *
 * -ENOTTY 를 쓰는 것도 규약이다. 상위 코드는 이 값을 "실패"가 아니라
 * "이 방법은 해당 없음"으로 읽고 다음 방법으로 넘어간다.
 *
 * 두 가지 조건을 본다.
 *   - PCI_DEV_FLAGS_NO_FLR_RESET: FLR 이 오히려 장치를 망가뜨리는 하드웨어에
 *     quirk 가 붙여 두는 표시.
 *   - devcap 의 FLR 지원 비트: 열거 때 캐시해 둔 Device Capabilities 값.
 *
 * NVMe 와의 관계: drivers/nvme/host/pci.c 가 이 함수를 probe=false 로 직접
 * 부른다 — 리셋 방법 목록을 거치지 않고 FLR 만 콕 집어 쓰는 경로다.
 *
 * 실행 문맥: probe=false 면 잠든다. 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_init_reset_methods(), __pci_reset_function_locked(),
 *   drivers/nvme/host/pci.c → [이 함수] → pcie_flr()
 */
int pcie_reset_flr(struct pci_dev *dev, bool probe)
{
	if (dev->dev_flags & PCI_DEV_FLAGS_NO_FLR_RESET) /* [한국어] FLR 이 오히려 장치를 망가뜨린다고 quirk 가 표시해 두었으면 */
		return -ENOTTY; /* [한국어] "이 방법은 해당 없음"을 뜻하는 -ENOTTY */

	if (!(dev->devcap & PCI_EXP_DEVCAP_FLR)) /* [한국어] 열거 때 캐시해 둔 Device Capabilities 에 FLR 지원 비트가 없으면 */
		return -ENOTTY; /* [한국어] 역시 쓸 수 없는 방법이다 */

	if (probe) /* [한국어] 가능 여부만 묻는 호출이면 */
		return 0; /* [한국어] "쓸 수 있다"는 뜻의 0 을 돌려주고 실제 리셋은 하지 않는다 */

	return pcie_flr(dev); /* [한국어] 실제로 FLR 을 건다 */
}
EXPORT_SYMBOL_GPL(pcie_reset_flr);

/*
 * [한국어]
 * pci_af_flr - Advanced Features capability 를 통한 FLR 을 판정하거나 실행한다.
 *
 * @dev: 대상 PCI 장치.
 * @probe: true 면 가능 여부만 판정한다.
 * @return: 0 가능/성공, -ENOTTY 이 방법을 쓸 수 없음, 그 밖의 음수는 실패.
 *
 * PCIe 가 아닌 재래식 PCI 장치도 FLR 을 쓸 수 있게 하려고 나중에 추가된
 * 규격이 AF(Advanced Features) capability 다. 하는 일은 pcie_flr() 과 같지만
 * 조작하는 레지스터가 PCIe capability 가 아니라 AF capability 안에 있다.
 *
 * 두 비트를 모두 확인해야 한다.
 *   - PCI_AF_CAP_TP: Transaction Pending 비트를 구현했는가. 미완료 트랜잭션을
 *     확인할 수 없으면 안전하게 리셋할 수 없다.
 *   - PCI_AF_CAP_FLR: FLR 자체를 지원하는가.
 *
 * 대기 코드의 << 8 이 요령이다. pci_wait_for_pending() 은 워드(16비트) 단위로
 * 읽는데 AF Status 와 AF Control 이 인접한 바이트라, Control 오프셋에서
 * 워드로 읽으면 상위 바이트가 Status 가 된다. 그래서 Status 의 비트를
 * 8칸 올려 맞춘다(원본 주석의 "word-aligned test").
 *
 * NVMe SSD 는 PCIe 장치라 AF capability 가 없어 이 방법은 곧바로 -ENOTTY 다.
 *
 * 실행 문맥: msleep 으로 잔다. 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_init_reset_methods(), __pci_reset_function_locked() → [이 함수]
 *     → pci_wait_for_pending(), pci_dev_reset_iommu_prepare(), pci_dev_wait()
 */
static int pci_af_flr(struct pci_dev *dev, bool probe)
{
	int ret; /* [한국어] pci_dev_wait() 와 IOMMU 준비의 결과 */
	int pos; /* [한국어] AF capability 의 오프셋 */
	u8 cap; /* [한국어] AF Capabilities 레지스터 값 */

	pos = pci_find_capability(dev, PCI_CAP_ID_AF); /* [한국어] 표준 capability 리스트에서 AF 를 찾는다 */
	if (!pos) /* [한국어] 없으면 이 방법을 쓸 수 없다 */
		return -ENOTTY; /* [한국어] "해당 없음"을 뜻하는 -ENOTTY */

	if (dev->dev_flags & PCI_DEV_FLAGS_NO_FLR_RESET) /* [한국어] FLR 이 오히려 장치를 망가뜨린다고 quirk 가 표시했으면 */
		return -ENOTTY; /* [한국어] 역시 해당 없음 */

	pci_read_config_byte(dev, pos + PCI_AF_CAP, &cap); /* [한국어] AF Capabilities 를 읽는다 */
	if (!(cap & PCI_AF_CAP_TP) || !(cap & PCI_AF_CAP_FLR)) /* [한국어] Transaction Pending 비트를 구현하지 않았거나 FLR 을 지원하지 않으면 — 전자가 없으면 미완료 DMA 를 확인할 수 없어 안전하게 리셋할 수 없다 */
		return -ENOTTY; /* [한국어] 해당 없음 */

	if (probe) /* [한국어] 가능 여부만 묻는 호출이면 */
		return 0; /* [한국어] "쓸 수 있다"만 알리고 끝낸다 */

	/*
	 * Wait for Transaction Pending bit to clear.  A word-aligned test
	 * is used, so we use the control offset rather than status and shift
	 * the test bit to match.
	 */
	if (!pci_wait_for_pending(dev, pos + PCI_AF_CTRL, /* [한국어] AF Control 오프셋에서 워드로 읽는다. 인접한 상위 바이트가 AF Status 이므로 */
				 PCI_AF_STATUS_TP << 8)) /* [한국어] Status 의 Transaction Pending 비트를 8칸 올려 위치를 맞춘다 */
		pci_err(dev, "timed out waiting for pending transaction; performing AF function level reset anyway\n"); /* [한국어] 시간 안에 내려가지 않아도 사실만 남기고 리셋을 강행한다 */

	/* Have to call it after waiting for pending DMA transaction */
	ret = pci_dev_reset_iommu_prepare(dev); /* [한국어] IOMMU 쪽 준비. DMA 대기 뒤에 와야 한다 */
	if (ret) { /* [한국어] 실패하면 */
		pci_err(dev, "failed to stop IOMMU for a PCI reset: %d\n", ret); /* [한국어] 이유를 남기고 */
		return ret; /* [한국어] 리셋을 포기한다 */
	}

	pci_write_config_byte(dev, pos + PCI_AF_CTRL, PCI_AF_CTRL_FLR); /* [한국어] AF Control 의 FLR 비트를 쓴다. 이 쓰기가 리셋의 시작이다 */

	if (dev->imm_ready) /* [한국어] 즉시 응답한다고 선언한 장치면 */
		goto done; /* [한국어] 대기를 건너뛴다 */

	/*
	 * Per Advanced Capabilities for Conventional PCI ECN, 13 April 2006,
	 * updated 27 July 2006; a device must complete an FLR within
	 * 100ms, but may silently discard requests while the FLR is in
	 * progress.  Wait 100ms before trying to access the device.
	 */
	msleep(100); /* [한국어] AF FLR 도 100ms 안에 끝내되 그동안 요청을 버려도 된다는 규정이라, 그 시간 동안 건드리지 않는다 */

	ret = pci_dev_wait(dev, "AF_FLR", PCIE_RESET_READY_POLL_MS); /* [한국어] 그 뒤 장치가 응답할 때까지 기다린다 */
done: /* [한국어] imm_ready 장치가 건너뛰어 오는 지점 */
	pci_dev_reset_iommu_done(dev); /* [한국어] IOMMU 준비를 되돌린다. 성공·실패 모두 거쳐야 짝이 맞는다 */
	return ret; /* [한국어] 대기 결과가 곧 리셋 결과다 */
}

/**
 * pci_pm_reset - Put device into PCI_D3 and back into PCI_D0.
 * @dev: Device to reset.
 * @probe: if true, return 0 if the device can be reset this way.
 *
 * If @dev supports native PCI PM and its PCI_PM_CTRL_NO_SOFT_RESET flag is
 * unset, it will be reinitialized internally when going from PCI_D3hot to
 * PCI_D0.  If that's the case and the device is not in a low-power state
 * already, force it into PCI_D3hot and back to PCI_D0, causing it to be reset.
 *
 * NOTE: This causes the caller to sleep for twice the device power transition
 * cooldown period, which for the D0->D3hot and D3hot->D0 transitions is 10 ms
 * by default (i.e. unless the @dev's d3hot_delay field has a different value).
 * Moreover, only devices in D0 can be reset by this function.
 */
/*
 * [한국어]
 * pci_pm_reset - D0 → D3hot → D0 왕복으로 장치를 리셋한다.
 *
 * @dev: 대상 PCI 장치.
 * @probe: true 면 가능 여부만 판정한다.
 * @return: 0 가능/성공, -ENOTTY 해당 없음, -EINVAL 이면 장치가 D0 가 아니다.
 *
 * FLR 이 없는 장치를 위한 대안이다. 근거는 PCI PM 규격의 성질 하나다 —
 * No_Soft_Reset 비트가 0 인 장치는 D3hot 에서 D0 로 돌아올 때 내부 리셋을
 * 수행해도 된다. 그 성질을 리셋 수단으로 역이용하는 것이다.
 *
 * 그래서 판정 조건이 뒤집혀 보인다. No_Soft_Reset 이 켜져 있으면(= 리셋하지
 * 않겠다고 선언한 장치면) 이 방법을 쓸 수 없어 -ENOTTY 다.
 *
 * D0 가 아니면 -EINVAL 인 이유는, 이미 저전원 상태인 장치를 D3hot 으로
 * 내려 봐야 "D3hot 에서 D0 로 올라오는" 전이가 성립하지 않기 때문이다
 * (원본 주석의 "only devices in D0 can be reset by this function").
 *
 * 비용도 원본 주석에 적혀 있다. 전이 지연을 두 번 겪으므로 기본값 기준
 * 최소 20ms 를 잔다. FLR 이 있으면 그쪽이 훨씬 빠르고 확실하다.
 *
 * 이 방법이 지우는 것은 장치 내부 상태이며 config space 도 초기화될 수
 * 있으므로, 다른 리셋과 마찬가지로 상태 저장·복원이 필요하다.
 *
 * 실행 문맥: 전이 지연 동안 두 번 잔다. 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_init_reset_methods(), __pci_reset_function_locked() → [이 함수]
 *     → pci_dev_d3_sleep(), pci_dev_wait()
 */
static int pci_pm_reset(struct pci_dev *dev, bool probe)
{
	u16 csr; /* [한국어] PMCSR 값. 상태 비트를 바꿔 가며 두 번 쓴다 */
	int ret; /* [한국어] pci_dev_wait() 와 IOMMU 준비의 결과 */

	if (!dev->pm_cap || dev->dev_flags & PCI_DEV_FLAGS_NO_PM_RESET) /* [한국어] PM capability 가 없거나, PM 리셋이 위험하다고 quirk 가 표시했으면 */
		return -ENOTTY; /* [한국어] 이 방법을 쓸 수 없다 */

	pci_read_config_word(dev, dev->pm_cap + PCI_PM_CTRL, &csr); /* [한국어] PMCSR 을 읽는다 */
	if (csr & PCI_PM_CTRL_NO_SOFT_RESET) /* [한국어] No_Soft_Reset 이 켜져 있으면 "D3hot 을 오가도 리셋하지 않는다"는 선언이라 */
		return -ENOTTY; /* [한국어] 리셋 수단으로 쓸 수 없다 */

	if (probe) /* [한국어] 가능 여부만 묻는 호출이면 */
		return 0; /* [한국어] "쓸 수 있다"만 알린다 */

	if (dev->current_state != PCI_D0) /* [한국어] 이미 저전원 상태면 "D3hot 에서 D0 로 올라오는" 전이를 만들 수 없다 */
		return -EINVAL; /* [한국어] 잘못된 요청으로 거절한다 */

	ret = pci_dev_reset_iommu_prepare(dev); /* [한국어] IOMMU 쪽 준비 */
	if (ret) { /* [한국어] 실패하면 */
		pci_err(dev, "failed to stop IOMMU for a PCI reset: %d\n", ret); /* [한국어] 이유를 남기고 */
		return ret; /* [한국어] 리셋을 포기한다 */
	}

	csr &= ~PCI_PM_CTRL_STATE_MASK; /* [한국어] 상태 필드만 지운다. PME 관련 비트는 보존한다 */
	csr |= PCI_D3hot; /* [한국어] D3hot 값을 넣는다 */
	pci_write_config_word(dev, dev->pm_cap + PCI_PM_CTRL, csr); /* [한국어] 써 넣는 순간 장치가 D3hot 으로 내려간다 */
	pci_dev_d3_sleep(dev); /* [한국어] 규격이 정한 D3hot 전이 안정화 시간을 지킨다 */

	csr &= ~PCI_PM_CTRL_STATE_MASK; /* [한국어] 다시 상태 필드만 지우고 */
	csr |= PCI_D0; /* [한국어] D0 값을 넣는다 */
	pci_write_config_word(dev, dev->pm_cap + PCI_PM_CTRL, csr); /* [한국어] 써 넣는 순간 장치가 D0 로 올라오며, No_Soft_Reset 이 0 이므로 내부 리셋이 일어난다 */
	pci_dev_d3_sleep(dev); /* [한국어] D3hot→D0 전이 안정화 시간을 다시 지킨다 */

	ret = pci_dev_wait(dev, "PM D3hot->D0", PCIE_RESET_READY_POLL_MS); /* [한국어] 그 뒤 장치가 config 요청에 응답할 때까지 기다린다 */
	pci_dev_reset_iommu_done(dev); /* [한국어] IOMMU 준비를 되돌린다 */
	return ret; /* [한국어] 대기 결과가 곧 리셋 결과다 */
}

/**
 * pcie_wait_for_link_status - Wait for link status change
 * @pdev: Device whose link to wait for.
 * @use_lt: Use the LT bit if TRUE, or the DLLLA bit if FALSE.
 * @active: Waiting for active or inactive?
 *
 * Return 0 if successful, or -ETIMEDOUT if status has not changed within
 * PCIE_LINK_RETRAIN_TIMEOUT_MS milliseconds.
 */
/*
 * [한국어]
 * pcie_wait_for_link_status - 링크 상태 비트가 원하는 값이 될 때까지 기다린다.
 *
 * @pdev: 링크를 지켜볼 장치(보통 브리지).
 * @use_lt: true 면 LT(Link Training) 비트를, false 면 DLLLA(Data Link Layer
 *          Link Active) 비트를 본다.
 * @active: true 면 그 비트가 서기를, false 면 내려가기를 기다린다.
 * @return: 0 원하는 상태 도달, -ETIMEDOUT 이면 1초 안에 그렇게 되지 않았다.
 *
 * 두 비트의 뜻이 서로 다르다는 것이 이 함수를 이해하는 열쇠다.
 *   - LT(Link Training): 지금 링크 훈련이 진행 중이다. 훈련이 끝나기를
 *     기다리려면 이 비트가 내려가기를 기다린다.
 *   - DLLLA(Data Link Layer Link Active): 데이터 링크 계층이 살아 있다.
 *     링크가 쓸 수 있게 되기를 기다리려면 이 비트가 서기를 기다린다.
 * 그래서 호출자는 보통 (use_lt=true, active=false) 또는
 * (use_lt=false, active=true) 조합으로 부른다.
 *
 * 1ms 씩 자며 폴링하고 상한은 PCIE_LINK_RETRAIN_TIMEOUT_MS(1초)다.
 * do-while 이라 먼저 한 번 확인하므로, 이미 원하는 상태면 자지 않는다.
 *
 * 실행 문맥: msleep 으로 잔다. 프로세스 문맥.
 *
 * 호출 체인:
 *   pcie_retrain_link() → [이 함수] → pcie_capability_read_word()
 */
static int pcie_wait_for_link_status(struct pci_dev *pdev,
				     bool use_lt, bool active)
{
	u16 lnksta_mask, lnksta_match; /* [한국어] lnksta_mask 는 지켜볼 비트, lnksta_match 는 그 비트가 가져야 할 값 */
	unsigned long end_jiffies; /* [한국어] 폴링을 멈출 시각 */
	u16 lnksta; /* [한국어] 매번 읽어 올 Link Status 값 */

	lnksta_mask = use_lt ? PCI_EXP_LNKSTA_LT : PCI_EXP_LNKSTA_DLLLA; /* [한국어] LT 는 "훈련 중", DLLLA 는 "링크 살아 있음"을 뜻한다. 목적에 따라 볼 비트가 다르다 */
	lnksta_match = active ? lnksta_mask : 0; /* [한국어] active 면 그 비트가 서야 하고, 아니면 내려가야 한다 */

	end_jiffies = jiffies + msecs_to_jiffies(PCIE_LINK_RETRAIN_TIMEOUT_MS); /* [한국어] 1초 뒤의 시각을 만료 기준으로 잡는다 */
	do { /* [한국어] 먼저 한 번 확인하고 시작한다 — 이미 원하는 상태면 자지 않는다 */
		pcie_capability_read_word(pdev, PCI_EXP_LNKSTA, &lnksta); /* [한국어] Link Status 레지스터를 읽는다 */
		if ((lnksta & lnksta_mask) == lnksta_match) /* [한국어] 지켜보는 비트가 원하는 값이 됐으면 */
			return 0; /* [한국어] 대기 종료 */
		msleep(1); /* [한국어] 1ms 자고 다시 본다 */
	} while (time_before(jiffies, end_jiffies)); /* [한국어] 만료 시각 전이면 계속 반복한다 */

	return -ETIMEDOUT; /* [한국어] 1초 안에 원하는 상태가 되지 않았다 */
}

/**
 * pcie_retrain_link - Request a link retrain and wait for it to complete
 * @pdev: Device whose link to retrain.
 * @use_lt: Use the LT bit if TRUE, or the DLLLA bit if FALSE, for status.
 *
 * Trigger retraining of the PCIe Link and wait for the completion of the
 * retraining. As link retraining is known to asserts LBMS and may change
 * the Link Speed, LBMS is cleared after the retraining and the Link Speed
 * of the subordinate bus is updated.
 *
 * Retrain completion status is retrieved from the Link Status Register
 * according to @use_lt.  It is not verified whether the use of the DLLLA
 * bit is valid.
 *
 * Return 0 if successful, or -ETIMEDOUT if training has not completed
 * within PCIE_LINK_RETRAIN_TIMEOUT_MS milliseconds.
 */
/*
 * [한국어]
 * pcie_retrain_link - 링크 재훈련을 요청하고 끝날 때까지 기다린다.
 *
 * @pdev: 재훈련을 걸 장치(보통 브리지).
 * @use_lt: 완료 판정에 LT 비트를 쓸지, DLLLA 비트를 쓸지.
 * @return: 0 성공, -ETIMEDOUT 이면 1초 안에 훈련이 끝나지 않았다.
 *
 * 링크 재훈련은 링크 속도나 폭을 바꾸거나, 불안정한 링크를 되살릴 때 쓴다.
 * Link Control 의 Retrain Link 비트를 쓰면 하드웨어가 훈련을 시작한다.
 *
 * 앞뒤로 붙은 처리에 각각 이유가 있다.
 *   1) 시작 전에 "훈련 중이 아님"을 먼저 확인한다. 원본 주석이 인용하는
 *      PCIe r6.1 7.5.3.7 의 구현 노트대로, 파라미터를 바꾸기 전에 시작된
 *      훈련이 아직 돌고 있으면 새 파라미터가 반영되지 않는 LTSSM 경쟁이 생긴다.
 *   2) clear_retrain_link quirk: 일부 장치는 Retrain Link 비트를 소프트웨어가
 *      직접 지워 주어야 훈련이 성공한다는 결함이 있다.
 *   3) 훈련 뒤 LBMS(Link Bandwidth Management Status)를 지운다. 재훈련 자체가
 *      이 비트를 세우기 때문에, 지워 두어야 이후 이 비트를 "하드웨어가 스스로
 *      속도·폭을 바꿨다"는 신호로 쓸 수 있다.
 *   4) 하위 버스의 링크 속도를 갱신한다. 원본 주석대로 LBMS 를 지우는 것이
 *      대역폭 제어 IRQ 핸들러와 경쟁해, 핸들러가 속도를 갱신하지 못한 채
 *      일찍 돌아갈 수 있기 때문이다.
 *
 * NVMe 와의 관계: NVMe SSD 가 붙은 링크가 낮은 속도로 훈련됐거나 오류로
 * 불안정할 때 이 경로로 되살린다. 링크 속도는 곧 SSD 의 최대 대역폭이다.
 *
 * 실행 문맥: 최대 2초까지 잘 수 있다. 프로세스 문맥.
 *
 * 호출 체인:
 *   링크 속도 조정 코드, pcie_failed_link_retrain() → [이 함수]
 *     → pcie_wait_for_link_status(), pcie_reset_lbms(), pcie_update_link_speed()
 */
int pcie_retrain_link(struct pci_dev *pdev, bool use_lt)
{
	int rc; /* [한국어] 각 단계의 결과 */

	/*
	 * Ensure the updated LNKCTL parameters are used during link
	 * training by checking that there is no ongoing link training that
	 * may have started before link parameters were changed, so as to
	 * avoid LTSSM race as recommended in Implementation Note at the end
	 * of PCIe r6.1 sec 7.5.3.7.
	 */
	rc = pcie_wait_for_link_status(pdev, true, false); /* [한국어] 훈련이 이미 돌고 있지 않은지 먼저 확인한다(LT 비트가 내려가기를 기다린다). 이것을 건너뛰면 새 파라미터가 반영되지 않는 경쟁이 생긴다 */
	if (rc) /* [한국어] 이미 돌고 있는 훈련이 1초 안에 끝나지 않으면 */
		return rc; /* [한국어] 재훈련을 걸지 않고 물러난다 */

	pcie_capability_set_word(pdev, PCI_EXP_LNKCTL, PCI_EXP_LNKCTL_RL); /* [한국어] Link Control 의 Retrain Link 비트를 세운다. 이 쓰기가 훈련을 시작시킨다 */
	if (pdev->clear_retrain_link) { /* [한국어] 비트를 소프트웨어가 직접 지워야 훈련이 성공하는 결함이 있는 장치라면 */
		/*
		 * Due to an erratum in some devices the Retrain Link bit
		 * needs to be cleared again manually to allow the link
		 * training to succeed.
		 */
		pcie_capability_clear_word(pdev, PCI_EXP_LNKCTL, PCI_EXP_LNKCTL_RL); /* [한국어] quirk 대로 다시 지워 준다 */
	}

	rc = pcie_wait_for_link_status(pdev, use_lt, !use_lt); /* [한국어] 훈련이 끝나기를 기다린다. use_lt 에 따라 "LT 가 내려감" 또는 "DLLLA 가 섬"을 기준으로 삼는다 */

	/*
	 * Clear LBMS after a manual retrain so that the bit can be used
	 * to track link speed or width changes made by hardware itself
	 * in attempt to correct unreliable link operation.
	 */
	pcie_reset_lbms(pdev); /* [한국어] 재훈련이 세워 놓은 LBMS 를 지운다. 그래야 이후 이 비트를 "하드웨어가 스스로 속도·폭을 바꿨다"는 신호로 쓸 수 있다 */

	/*
	 * Ensure the Link Speed updates after retraining in case the Link
	 * Speed was changed because of the retraining. While the bwctrl's
	 * IRQ handler normally picks up the new Link Speed, clearing LBMS
	 * races with the IRQ handler reading the Link Status register and
	 * can result in the handler returning early without updating the
	 * Link Speed.
	 */
	if (pdev->subordinate) /* [한국어] 이 장치가 브리지라면 */
		pcie_update_link_speed(pdev->subordinate, PCIE_LINK_RETRAIN); /* [한국어] 하위 버스의 링크 속도 기록을 갱신한다. LBMS 를 지우는 것이 IRQ 핸들러와 경쟁해 핸들러가 갱신을 놓칠 수 있기 때문이다 */

	return rc; /* [한국어] 훈련 대기의 결과가 곧 이 함수의 결과다 */
}

/**
 * pcie_wait_for_link_delay - Wait until link is active or inactive
 * @pdev: Bridge device
 * @active: waiting for active or inactive?
 * @delay: Delay to wait after link has become active (in ms)
 *
 * Use this to wait till link becomes active or inactive.
 */
/*
 * [한국어]
 * pcie_wait_for_link_delay - 링크가 살아나거나(또는 죽거나) 할 때까지 기다린다
 *
 * @pdev:   하류 포트(브리지). 이 포트 아래 링크의 상태를 본다.
 * @active: true 면 "링크가 올라올 때까지", false 면 "내려갈 때까지" 기다린다.
 * @delay:  링크가 올라온 뒤 추가로 더 기다릴 시간(ms). 링크가 붙었다고 해서
 *          하위 장치가 곧바로 config 요청을 받을 수 있는 것은 아니기 때문이다.
 * @return: true = 원하는 상태에 도달, false = 시간 안에 도달하지 못함.
 *
 * 리셋이나 D3cold 복귀 뒤에 반드시 거쳐야 하는 관문이다. 링크가 아직
 * 훈련 중인데 config 요청을 내려보내면 응답이 오지 않아 all-ones 가 읽히고,
 * 커널은 장치가 사라졌다고 오판한다.
 *
 * 세 갈래로 갈린다.
 *   1) 링크 상태 보고 기능이 없는 컨트롤러 - 물어볼 방법이 없으니 그냥
 *      최대 시간만큼 자고 성공했다고 친다.
 *   2) active 요청 - 20ms 먼저 자고(스펙이 정한 LTSSM Detect 진입 시간),
 *      Data Link Layer Link Active 비트를 폴링한다. 실패하면 재훈련을
 *      한 번 시도해 본다.
 *   3) inactive 요청 - 폴링만 한다. 추가 대기가 필요 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용. msleep 으로 잠든다.
 * 호출자: pcie_wait_for_link(), pci_bridge_wait_for_secondary_bus().
 * 피호출자: pcie_wait_for_link_status(), pcie_failed_link_retrain().
 *
 * 호출 체인:
 *   pci_bridge_wait_for_secondary_bus -> [pcie_wait_for_link_delay]
 *     -> pcie_wait_for_link_status -> pcie_capability_read_word(LNKSTA)
 */
static bool pcie_wait_for_link_delay(struct pci_dev *pdev, bool active,
				     int delay)
{
	int rc;		/* [한국어] pcie_wait_for_link_status 의 결과. 0 이 성공이다 */

	/*
	 * Some controllers might not implement link active reporting. In this
	 * case, we wait for 1000 ms + any delay requested by the caller.
	 */
	/* [한국어] Data Link Layer Link Active Reporting 을 구현하지 않은 포트다.
	 * 상태를 물어볼 수단이 없으므로 "충분히 오래" 자는 것 말고는 방법이 없다.
	 * 최대 훈련 시간에 호출자가 요구한 추가 지연을 더해 한 번에 잔다.
	 * 조건과 무관하게 true 를 돌려주는 것은, 실패를 알 방법이 없는 이상
	 * 성공했다고 가정하는 편이 낫기 때문이다(아니면 멀쩡한 장치도 못 쓴다). */
	if (!pdev->link_active_reporting) {
		msleep(PCIE_LINK_RETRAIN_TIMEOUT_MS + delay);
		return true;
	}

	/*
	 * PCIe r4.0 sec 6.6.1, a component must enter LTSSM Detect within 20ms,
	 * after which we should expect the link to be active if the reset was
	 * successful. If so, software must wait a minimum 100ms before sending
	 * configuration requests to devices downstream this port.
	 *
	 * If the link fails to activate, either the device was physically
	 * removed or the link is permanently failed.
	 */
	/* [한국어] 링크가 올라오기를 기다리는 경우에만 20ms 를 먼저 잔다.
	 * 스펙이 "리셋 해제 후 20ms 안에 LTSSM 이 Detect 상태로 들어간다" 고
	 * 정했으므로, 그 전에 폴링해 봐야 아직 아무 일도 일어나지 않았다. */
	if (active)
		msleep(20);
	/* [한국어] LNKSTA 의 Data Link Layer Link Active 비트를 원하는 값이 될 때까지
	 * 폴링한다. 두 번째 인자 false 는 "링크 훈련 중(LT) 비트는 보지 않는다" 는 뜻.
	 * 0 = 도달, 음수 = 시간 초과. */
	rc = pcie_wait_for_link_status(pdev, false, active);
	if (active) {
		/* [한국어] 링크가 안 올라왔다. 일부 장치는 첫 훈련에 실패하지만
		 * 속도를 낮춰 다시 시도하면 붙는다. 그 우회를 한 번 시도한다. */
		if (rc)
			rc = pcie_failed_link_retrain(pdev);
		/* [한국어] 재훈련도 실패했다면 물리적으로 뽑혔거나 링크가 영구 고장이다. */
		if (rc)
			return false;

		/* [한국어] 링크는 붙었지만 하위 장치가 아직 config 요청을 받을 준비가
		 * 안 됐을 수 있다. 스펙이 요구하는 최소 100ms(호출자가 delay 로 전달)를
		 * 여기서 채운다. 이 대기를 건너뛰면 첫 config 읽기가 all-ones 로 돌아온다. */
		msleep(delay);
		return true;
	}

	/* [한국어] 링크가 내려가기를 기다린 경우. 시간 안에 안 내려갔으면 실패다. */
	if (rc)
		return false;

	return true;	/* [한국어] 원하는 상태에 도달했다 */
}

/**
 * pcie_wait_for_link - Wait until link is active or inactive
 * @pdev: Bridge device
 * @active: waiting for active or inactive?
 *
 * Use this to wait till link becomes active or inactive.
 */
/*
 * [한국어]
 * pcie_wait_for_link - 링크 상태 변화를 기다린다 (표준 100ms 판)
 *
 * @pdev:   하류 포트(브리지)
 * @active: true 면 링크가 올라오기를, false 면 내려가기를 기다린다.
 * @return: true = 도달, false = 시간 초과.
 *
 * pcie_wait_for_link_delay() 에 delay=100 을 넣은 것이 전부다. 100ms 는
 * PCIe 스펙이 정한 값으로, 링크가 붙은 뒤 하위 장치에 첫 config 요청을
 * 보내기까지 반드시 기다려야 하는 최소 시간이다(PCIe Base Spec 6.6.1).
 * 대부분의 호출자는 이 표준 값이면 충분하므로 이 짧은 판을 쓴다.
 * 펌웨어가 더 긴 시간을 요구하는 특수한 경우에만 _delay 판을 직접 부른다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용.
 * 호출자: hotplug 드라이버(pciehp), 리셋 경로, 전원 복귀 경로.
 */
bool pcie_wait_for_link(struct pci_dev *pdev, bool active)
{
	/* [한국어] 100 = PCIe 스펙이 요구하는 링크 활성 후 최소 대기(ms). */
	return pcie_wait_for_link_delay(pdev, active, 100);
}

/*
 * Find maximum D3cold delay required by all the devices on the bus.  The
 * spec says 100 ms, but firmware can lower it and we allow drivers to
 * increase it as well.
 *
 * Context: Called with @pci_bus_sem locked for reading.
 */
/*
 * [한국어]
 * pci_bus_max_d3cold_delay - 이 버스의 장치들이 요구하는 D3cold 복귀 지연의 최대값
 *
 * @bus:    검사할 버스
 * @return: 밀리초 단위 지연. 버스가 비어 있으면 100.
 *
 * D3cold 는 장치에서 전원을 완전히 끊은 상태다. 전원을 다시 넣으면 장치가
 * 스스로 초기화를 마칠 때까지 config 접근을 하면 안 되고, 그 시간이 장치마다
 * 다르다. 브리지 하나 아래에 여러 장치가 있으면 그중 가장 오래 걸리는 것에
 * 맞춰야 하므로 최대값을 구한다.
 *
 * 코드가 눈에 띄게 이상해 보인다 — min_delay 를 100 에서 시작해 더 작은 값을
 * 찾아 내려가고, max_delay 는 0 에서 시작해 올라간 뒤, 마지막에 둘 중 큰 것을
 * 돌려준다. 결과적으로:
 *   - 어떤 장치가 100 보다 큰 지연을 요구하면 그 값이 이긴다(max_delay).
 *   - 모든 장치가 100 이하를 요구하면 min_delay 는 그중 최소값이 되고,
 *     max_delay 는 그중 최대값이 되므로 max() 는 최대값을 고른다.
 * 즉 실질적으로는 "모든 장치의 요구 중 최대값" 이며, min_delay 의 초기값 100 은
 * 아무 장치도 없을 때의 스펙 기본값 역할만 한다.
 *
 * 실행 컨텍스트: 호출자가 pci_bus_sem 을 읽기 잠금으로 쥐고 들어와야 한다.
 *   lockdep_assert_held 가 그것을 강제한다(디버그 빌드에서 경고).
 * 호출자: pci_bridge_wait_for_secondary_bus().
 *
 * 호출 체인:
 *   pci_bridge_wait_for_secondary_bus -> [pci_bus_max_d3cold_delay]
 */
static int pci_bus_max_d3cold_delay(const struct pci_bus *bus)
{
	const struct pci_dev *pdev;	/* [한국어] 순회 커서 */
	/* [한국어] 스펙 기본값 100ms 에서 시작해 더 작은 값을 찾아 내려간다. */
	int min_delay = 100;
	/* [한국어] 0 에서 시작해 더 큰 값을 찾아 올라간다. */
	int max_delay = 0;

	/* [한국어] 버스의 devices 목록을 순회하려면 pci_bus_sem 이 필요하다.
	 * 잠금 없이 돌면 그사이 장치가 제거되어 목록이 끊길 수 있다.
	 * 이 매크로는 CONFIG_LOCKDEP 빌드에서만 실제로 검사한다. */
	lockdep_assert_held(&pci_bus_sem);

	/* [한국어] 이 버스에 직접 붙은 장치들만 본다(하위 브리지 아래는 재귀하지 않는다). */
	list_for_each_entry(pdev, &bus->devices, bus_list) {
		/* [한국어] d3cold_delay 는 열거 시 스펙 기본값으로 채워지고,
		 * quirk 나 펌웨어(_DSM)가 장치별로 조정한다. */
		if (pdev->d3cold_delay < min_delay)
			min_delay = pdev->d3cold_delay;
		if (pdev->d3cold_delay > max_delay)
			max_delay = pdev->d3cold_delay;
	}

	/* [한국어] 둘 중 큰 값. 위 주석에서 설명한 대로 사실상 최대값이다. */
	return max(min_delay, max_delay);
}

/**
 * pci_bridge_wait_for_secondary_bus - Wait for secondary bus to be accessible
 * @dev: PCI bridge
 * @reset_type: reset type in human-readable form
 *
 * Handle necessary delays before access to the devices on the secondary
 * side of the bridge are permitted after D3cold to D0 transition
 * or Conventional Reset.
 *
 * For PCIe this means the delays in PCIe 5.0 section 6.6.1. For
 * conventional PCI it means Tpvrh + Trhfa specified in PCI 3.0 section
 * 4.3.2.
 *
 * Return 0 on success or -ENOTTY if the first device on the secondary bus
 * failed to become accessible.
 */
/*
 * [한국어]
 * pci_bridge_wait_for_secondary_bus - 리셋 후 하위 버스가 접근 가능해질 때까지 기다린다
 *
 * @dev:        하류 브리지(Root Port 또는 스위치의 Downstream Port)
 * @reset_type: 로그에 찍을 리셋 종류 이름("bus reset", "FLR" 등). 사람이 읽을
 *              문자열이며 동작에는 영향을 주지 않는다.
 * @return: 0 = 하위 장치에 접근 가능해졌음(또는 기다릴 필요가 없었음),
 *          -ENOTTY = 하위 버스의 첫 장치가 끝내 응답하지 않음.
 *
 * 리셋이나 D3cold -> D0 전환 뒤에 반드시 거쳐야 하는 대기다. 이 대기를
 * 건너뛰고 config 요청을 내려보내면 응답이 없어 all-ones 가 읽히고, 커널은
 * 멀쩡한 장치를 "사라졌다" 고 판단해 제거해 버린다.
 *
 * 필요한 대기 시간이 상황마다 달라서 함수가 여러 갈래로 갈린다.
 *   - 재래식 PCI/PCI-X: Tpvrh + Trhfa = 1000ms + 100ms 를 그냥 잔다.
 *     상태를 물어볼 수단이 없기 때문이다(PCI 3.0 spec 4.3.2).
 *   - PCIe 5 GT/s 이하: 100ms 자고 나서 장치가 응답하는지 폴링한다.
 *     이 속도대에서는 링크가 빨리 붙으므로 링크 상태를 따로 기다릴 필요가 없다.
 *   - PCIe Gen3 이상: 먼저 Data Link Layer Link Active 가 서기를 기다린 뒤,
 *     그다음에 장치 응답을 폴링한다. 고속 링크는 훈련이 오래 걸려서
 *     시간만 재서는 판정할 수 없다(PCIe 5.0 spec 6.6.1).
 *
 * 폴링을 하는 이유가 하나 더 있다. 장치는 아직 준비되지 않았을 때
 * Request Retry Status(CRS)로 완료를 돌려줄 수 있고, 그러면 정해진 시간보다
 * 더 오래 걸린다. 스펙은 "1초까지는 고장으로 단정하지 말라" 고 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용(msleep 과 폴링). 하위 버스 목록을
 *   읽는 짧은 구간에서만 pci_bus_sem 을 읽기 잠금으로 잡고, 잠들기 전에 놓는다.
 * 호출자: pci_reset_bus_function(), pci_parent_bus_reset(), 전원 복귀 경로,
 *   그리고 AER/DPC 복구.
 * 피호출자: pci_bus_max_d3cold_delay(), pcie_wait_for_link_delay(), pci_dev_wait().
 *
 * 호출 체인:
 *   pci_reset_bus_function -> pci_bridge_secondary_bus_reset
 *     -> [pci_bridge_wait_for_secondary_bus] -> pci_dev_wait
 */
int pci_bridge_wait_for_secondary_bus(struct pci_dev *dev, char *reset_type)
{
	/* [한국어] 응답 여부를 확인할 대표 장치(하위 버스의 첫 장치).
	 * __free(pci_dev_put) 은 이 변수가 스코프를 벗어날 때 자동으로
	 * pci_dev_put() 을 부르게 하는 정리(cleanup) 속성이다. 아래에 return 이
	 * 여러 개라 손으로 put 을 챙기면 빠뜨리기 쉬운데, 그 실수를 원천 차단한다.
	 * NULL 로 초기화해 두어야 참조를 잡기 전에 반환해도 안전하다. */
	struct pci_dev *child __free(pci_dev_put) = NULL;
	int delay;	/* [한국어] 하위 장치들이 요구하는 D3cold 복귀 지연(ms) */

	/* [한국어] 이미 사라진 장치라면 기다릴 것이 없다. 오히려 config 접근을
	 * 시도하면 위험하므로 즉시 성공으로 돌아간다. */
	if (pci_dev_is_disconnected(dev))
		return 0;

	/* [한국어] 브리지가 아니면 "하위 버스" 자체가 없다. */
	if (!pci_is_bridge(dev))
		return 0;

	/* [한국어] 하위 버스의 장치 목록을 읽기 위한 잠금. 아래에서 목록을
	 * 확인하고 첫 장치의 참조를 잡은 뒤 곧바로 놓는다 — 이 세마포어를 쥔 채
	 * msleep 으로 잠들면 버스 트리 전체가 그동안 멈춘다. */
	down_read(&pci_bus_sem);

	/*
	 * We only deal with devices that are present currently on the bus.
	 * For any hot-added devices the access delay is handled in pciehp
	 * board_added(). In case of ACPI hotplug the firmware is expected
	 * to configure the devices before OS is notified.
	 */
	/* [한국어] 하위 버스가 없거나 비어 있으면 기다릴 대상이 없다.
	 * 위 영어 주석이 설명하듯, 핫플러그로 새로 꽂힌 장치의 대기는 여기가 아니라
	 * pciehp 의 board_added() 가 담당한다 — 그쪽은 아직 열거도 안 된 상태라
	 * 이 함수가 볼 수 있는 자식이 없다. */
	if (!dev->subordinate || list_empty(&dev->subordinate->devices)) {
		up_read(&pci_bus_sem);
		return 0;
	}

	/* Take d3cold_delay requirements into account */
	/* [한국어] 하위 장치들이 요구하는 지연 중 최대값. 이 값이 아래 모든
	 * 대기의 기준이 된다. 반드시 pci_bus_sem 을 쥔 상태에서 불러야 한다. */
	delay = pci_bus_max_d3cold_delay(dev->subordinate);
	/* [한국어] 0 이면 모든 하위 장치가 "기다릴 필요 없다" 고 선언한 것이다
	 * (quirk 나 펌웨어가 d3cold_delay 를 0 으로 낮춘 경우). 그대로 돌아간다. */
	if (!delay) {
		up_read(&pci_bus_sem);
		return 0;
	}

	/* [한국어] 하위 버스의 첫 장치를 대표로 삼아 참조를 올린다. 이 장치가
	 * config 요청에 응답하면 버스 전체가 살아난 것으로 본다 — 모든 장치를
	 * 하나씩 확인할 필요는 없다는 판단이다.
	 * 참조를 올리는 이유: 아래에서 잠금을 놓고 최대 1초까지 자는데, 그 사이
	 * 이 장치가 제거되면 포인터가 무효해진다. 참조가 있으면 구조체 자체는
	 * 유지된다(내용은 disconnected 로 바뀔 수 있고, pci_dev_wait 이 그것을 본다). */
	child = pci_dev_get(list_first_entry(&dev->subordinate->devices,
					     struct pci_dev, bus_list));
	/* [한국어] 여기서 잠금을 놓는다. 이후는 전부 잠들 수 있는 대기다. */
	up_read(&pci_bus_sem);

	/*
	 * Conventional PCI and PCI-X we need to wait Tpvrh + Trhfa before
	 * accessing the device after reset (that is 1000 ms + 100 ms).
	 */
	/* [한국어] 재래식 PCI/PCI-X 경로. 링크라는 개념도, 상태를 물어볼 레지스터도
	 * 없으므로 스펙이 정한 시간을 그대로 자는 수밖에 없다.
	 * 1000 = Tpvrh(전원/리셋 안정화), delay = 하위 장치가 요구한 추가 시간. */
	if (!pci_is_pcie(dev)) {
		pci_dbg(dev, "waiting %d ms for secondary bus\n", 1000 + delay);
		msleep(1000 + delay);
		return 0;
	}

	/*
	 * For PCIe downstream and root ports that do not support speeds
	 * greater than 5 GT/s need to wait minimum 100 ms. For higher
	 * speeds (gen3) we need to wait first for the data link layer to
	 * become active.
	 *
	 * However, 100 ms is the minimum and the PCIe spec says the
	 * software must allow at least 1s before it can determine that the
	 * device that did not respond is a broken device. Also device can
	 * take longer than that to respond if it indicates so through Request
	 * Retry Status completions.
	 *
	 * Therefore we wait for 100 ms and check for the device presence
	 * until the timeout expires.
	 */
	/* [한국어] 상류 포트나 엔드포인트에는 "하위 버스" 라는 것이 없다.
	 * 아래 로직은 전부 하류 포트를 전제로 하므로 여기서 빠져나간다. */
	if (!pcie_downstream_port(dev))
		return 0;

	/* [한국어] Gen1(2.5GT/s) / Gen2(5GT/s) 경로. 이 속도대는 링크 훈련이
	 * 짧아서 링크 상태를 따로 기다리지 않고 곧바로 장치 응답을 본다. */
	if (pcie_get_speed_cap(dev) <= PCIE_SPEED_5_0GT) {
		u16 status;	/* [한국어] LNKSTA 값을 담을 임시 변수 */

		pci_dbg(dev, "waiting %d ms for downstream link\n", delay);
		/* [한국어] 하위 장치가 요구한 최소 시간을 먼저 채운다. */
		msleep(delay);

		/* [한국어] 그다음 실제로 응답하는지 확인한다. pci_dev_wait 은 Vendor ID 를
		 * 반복해 읽으며 all-ones 가 아닌 값이 나오기를 기다린다. 0 이면 성공.
		 * 남은 예산에서 이미 잔 delay 를 빼서 넘긴다 — 전체 대기 상한이
		 * PCI_RESET_WAIT 을 넘지 않게 하기 위해서다. */
		if (!pci_dev_wait(child, reset_type, PCI_RESET_WAIT - delay))
			return 0;

		/*
		 * If the port supports active link reporting we now check
		 * whether the link is active and if not bail out early with
		 * the assumption that the device is not present anymore.
		 */
		/* [한국어] 여기까지 왔다는 것은 1차 대기 안에 응답이 없었다는 뜻이다.
		 * 더 기다릴지 포기할지 정해야 하는데, 링크 상태를 물어볼 수 없는
		 * 포트라면 판단 근거가 없으므로 고장으로 처리한다. */
		if (!dev->link_active_reporting)
			return -ENOTTY;

		/* [한국어] 링크 상태를 직접 확인한다. DLLLA(Data Link Layer Link Active)가
		 * 서 있지 않다면 물리적으로 뽑혔거나 링크가 죽은 것이므로, 더 기다려 봐야
		 * 시간만 버린다. 반대로 서 있다면 장치가 아직 초기화 중일 뿐이므로
		 * 아래에서 더 기다려 준다. */
		pcie_capability_read_word(dev, PCI_EXP_LNKSTA, &status);
		if (!(status & PCI_EXP_LNKSTA_DLLLA))
			return -ENOTTY;

		/* [한국어] 링크는 살아 있으니 남은 예산만큼 더 폴링한다.
		 * PCIE_RESET_READY_POLL_MS 가 전체 상한이고 그중 PCI_RESET_WAIT 은
		 * 위에서 이미 썼다. */
		return pci_dev_wait(child, reset_type,
				    PCIE_RESET_READY_POLL_MS - PCI_RESET_WAIT);
	}

	/* [한국어] Gen3 이상 경로. 고속 링크는 훈련이 오래 걸리고 시간이 일정하지
	 * 않아서, 시간만 재는 방식이 통하지 않는다. 링크가 실제로 붙었는지를
	 * 먼저 확인해야 한다. */
	pci_dbg(dev, "waiting %d ms for downstream link, after activation\n",
		delay);
	/* [한국어] DLLLA 가 설 때까지 기다린 뒤, 추가로 delay 만큼 더 잔다.
	 * 두 번째 인자 true 가 "활성화를 기다린다" 는 뜻이다. */
	if (!pcie_wait_for_link_delay(dev, true, delay)) {
		/* Did not train, no need to wait any further */
		/* [한국어] 훈련 자체가 안 됐다. 장치 응답을 폴링해 봐야 의미가 없으므로
		 * 바로 포기한다. dbg 가 아니라 info 로 찍는 것은, 이것이 실제로
		 * 무언가 잘못됐다는 신호이기 때문이다. */
		pci_info(dev, "Data Link Layer Link Active not set in %d msec\n", delay);
		return -ENOTTY;
	}

	/* [한국어] 링크가 붙었으니 이제 장치가 config 요청에 응답하기를 기다린다.
	 * 전체 상한에서 이미 쓴 delay 를 뺀 만큼이 남은 예산이다. */
	return pci_dev_wait(child, reset_type,
			    PCIE_RESET_READY_POLL_MS - delay);
}

/*
 * [한국어]
 * pci_reset_secondary_bus - 브리지의 하위 버스에 리셋 신호를 넣었다 뺀다
 *
 * @dev: 브리지 장치
 * @return: 없음. 이 동작은 실패할 수 없다(레지스터 쓰기가 전부다).
 *
 * Secondary Bus Reset(SBR)이라고 부르는 동작이다. 브리지의 Bridge Control
 * 레지스터에 있는 Secondary Bus Reset 비트를 세우면, 그 브리지 아래로
 * 리셋 신호가 나가 하위의 모든 장치가 초기화된다.
 *
 * 주의할 점이 있다. 이 함수는 "리셋 신호를 넣었다 빼는" 것까지만 한다.
 * 리셋이 풀린 뒤 장치가 다시 응답하기까지 기다리는 일은 하지 않으므로,
 * 호출자가 반드시 pci_bridge_wait_for_secondary_bus() 를 이어서 불러야 한다.
 * 그것을 묶어 둔 것이 pci_bridge_secondary_bus_reset() 이고, 대부분의 코드는
 * 그쪽을 쓴다. 이 함수를 직접 부르는 것은 대기 시점을 스스로 제어해야 하는
 * 특수한 경우뿐이다.
 *
 * FLR 과의 차이: FLR 은 PCIe function 하나만 리셋하지만, SBR 은 그 브리지
 * 아래 전부를 리셋한다. NVMe 컨트롤러가 FLR 에 응답하지 않을 때 마지막
 * 수단으로 쓰지만, 같은 스위치 포트에 다른 드라이브가 붙어 있으면 그것들도
 * 함께 리셋된다는 부작용이 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용(msleep).
 * 호출자: pci_bridge_secondary_bus_reset(), 그리고 일부 quirk.
 *
 * 호출 체인:
 *   pci_parent_bus_reset -> pci_bridge_secondary_bus_reset
 *     -> [pci_reset_secondary_bus] -> pci_write_config_word
 */
void pci_reset_secondary_bus(struct pci_dev *dev)
{
	u16 ctrl;	/* [한국어] Bridge Control 레지스터(오프셋 0x3E)의 현재 값 */

	/* [한국어] 읽고-고쳐-쓰기. 이 레지스터에는 VGA Enable, ISA Enable,
	 * Master Abort Mode 같은 다른 설정도 함께 들어 있어서, 통째로 덮어쓰면
	 * 그것들이 지워진다. */
	pci_read_config_word(dev, PCI_BRIDGE_CONTROL, &ctrl);
	/* [한국어] Secondary Bus Reset 비트(0x40)를 세운다. 이 비트가 1인 동안
	 * 하위 버스는 리셋 상태로 유지된다. */
	ctrl |= PCI_BRIDGE_CTL_BUS_RESET;
	pci_write_config_word(dev, PCI_BRIDGE_CONTROL, ctrl);

	/*
	 * PCI spec v3.0 7.6.4.2 requires minimum Trst of 1ms.  Double
	 * this to 2ms to ensure that we meet the minimum requirement.
	 */
	/* [한국어] 리셋 신호를 최소 1ms 는 유지해야 한다는 스펙 요구를,
	 * 여유를 두어 2ms 로 잡았다. msleep 은 요청한 시간보다 더 잘 수는 있어도
	 * 덜 자지는 않으므로 최소 요구를 확실히 넘긴다. */
	msleep(2);

	/* [한국어] 비트를 내려 리셋을 해제한다. 이 순간부터 하위 장치들이
	 * 초기화를 시작하며, 그것이 끝날 때까지는 config 요청에 응답하지 않는다. */
	ctrl &= ~PCI_BRIDGE_CTL_BUS_RESET;
	pci_write_config_word(dev, PCI_BRIDGE_CONTROL, ctrl);
}

/*
 * [한국어]
 * pcibios_reset_secondary_bus - 아키텍처가 가로챌 수 있는 하위 버스 리셋 훅
 *
 * @dev: 브리지 장치
 * @return: 없음.
 *
 * __weak 로 선언돼 있다. 즉 아키텍처 코드가 같은 이름의 함수를 정의하면
 * 링커가 그쪽을 쓰고, 정의하지 않으면 이 기본 구현이 쓰인다. 기본 구현은
 * 표준 방식인 pci_reset_secondary_bus() 를 그대로 부를 뿐이다.
 *
 * 훅을 둔 이유: 일부 플랫폼은 Bridge Control 레지스터의 SBR 비트를 쓰는
 * 표준 방식으로 리셋이 되지 않거나, 리셋 전후에 SoC 고유의 처리(클럭 게이팅,
 * PHY 재설정 등)를 함께 해야 한다. 그런 플랫폼이 이 함수를 덮어쓴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(기본 구현이 msleep 한다).
 * 호출자: pci_bridge_secondary_bus_reset() 하나뿐이다.
 */
void __weak pcibios_reset_secondary_bus(struct pci_dev *dev)
{
	/* [한국어] 표준 동작. 아키텍처가 덮어쓰지 않았다면 이것이 전부다. */
	pci_reset_secondary_bus(dev);
}

/**
 * pci_bridge_secondary_bus_reset - Reset the secondary bus on a PCI bridge.
 * @dev: Bridge device
 *
 * Use the bridge control register to assert reset on the secondary bus.
 * Devices on the secondary bus are left in power-on state.
 */
/*
 * [한국어]
 * pci_bridge_secondary_bus_reset - 하위 버스를 리셋하고, 다시 살아날 때까지 기다린다
 *
 * @dev: 브리지 장치
 * @return: 0 = 리셋 후 하위 장치에 접근 가능,
 *          -ENOTTY = 하위 장치가 끝내 응답하지 않음.
 *
 * "리셋 신호를 넣었다 빼는 것"(pcibios_reset_secondary_bus)과 "다시 응답할
 * 때까지 기다리는 것"(pci_bridge_wait_for_secondary_bus)을 한 쌍으로 묶은
 * 함수다. 리셋만 걸고 기다리지 않으면 곧바로 이어지는 config 접근이 전부
 * 실패하므로, 이 둘은 사실상 항상 함께 쓰인다.
 *
 * 앞부분의 경고가 중요하다. dev->block_cfg_access 가 서 있지 않다는 것은
 * 호출자가 pci_cfg_access_lock() 을 걸지 않았다는 뜻이고, 그러면 리셋이
 * 진행되는 동안 userspace(lspci 등)나 다른 커널 코드가 config 를 읽을 수
 * 있다. 리셋 중인 장치는 아무 값이나 돌려주므로 그 값을 믿은 코드가
 * 오작동한다. 그래서 잠금 없이 부르는 호출자를 찾아내 고치도록
 * 호출 지점(%pS 로 반환 주소를 심볼 이름으로 출력)을 남긴다.
 * _once 판이라 같은 지점에 대해 한 번만 찍는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용.
 * 호출자: pci_parent_bus_reset(), pci_reset_bus_function(), AER/DPC 복구.
 *
 * 호출 체인:
 *   pci_reset_bus -> pci_bus_reset -> [pci_bridge_secondary_bus_reset]
 *     -> pcibios_reset_secondary_bus -> pci_bridge_wait_for_secondary_bus
 */
int pci_bridge_secondary_bus_reset(struct pci_dev *dev)
{
	/* [한국어] config 접근 차단이 안 걸린 채로 들어왔다. 버그이지만 리셋
	 * 자체는 진행한다 — 여기서 실패시키면 기존 호출자들이 전부 깨진다.
	 * 대신 어디서 불렀는지 남겨 고칠 수 있게 한다. */
	if (!dev->block_cfg_access)
		pci_warn_once(dev, "unlocked secondary bus reset via: %pS\n",
			      __builtin_return_address(0));
	/* [한국어] 실제 리셋. 아키텍처가 훅을 덮어썼다면 그쪽으로 간다. */
	pcibios_reset_secondary_bus(dev);

	/* [한국어] 리셋이 풀린 뒤 하위 장치가 config 요청에 응답할 때까지 기다린다.
	 * "bus reset" 은 로그에 찍힐 이름일 뿐이다. 이 함수의 반환값이 곧
	 * 이 함수의 결과가 된다. */
	return pci_bridge_wait_for_secondary_bus(dev, "bus reset");
}
EXPORT_SYMBOL_GPL(pci_bridge_secondary_bus_reset);

/*
 * [한국어]
 * pci_parent_bus_reset - 상위 브리지의 버스 리셋으로 이 장치 하나를 리셋한다
 *
 * @dev:   리셋할 장치
 * @probe: true 면 "이 방법을 쓸 수 있는가" 만 확인하고 실제로는 리셋하지 않는다.
 *         false 면 실제로 리셋한다.
 * @return: 0 = 가능(probe) 또는 성공, -ENOTTY = 이 방법을 쓸 수 없음.
 *
 * 리셋 방법 후보 중 하나다. 커널은 장치를 리셋할 때 여러 방법을 정해진 순서로
 * 시도하는데(pci_dev_reset_methods 배열), 이것은 그중 "상위 버스를 통째로
 * 리셋한다" 는 방법이다. FLR 처럼 장치가 스스로 지원해야 하는 방법이 없을 때
 * 쓰는 최후 수단에 가깝다.
 *
 * probe 인자가 이 함수의 구조를 결정한다. 커널은 먼저 모든 방법에 대해
 * probe=true 로 물어 "쓸 수 있는 것" 목록을 만들어 두고, 실제 리셋 때는
 * 그 목록을 순서대로 probe=false 로 부른다. 그래서 조건 검사와 실행이
 * 한 함수 안에 함께 있고, 검사를 통과한 뒤 probe 이면 거기서 멈춘다.
 *
 * 쓸 수 없는 조건이 네 가지다.
 *   1) 루트 버스에 직접 붙은 장치 - 위에 브리지가 없어 리셋할 대상이 없다.
 *   2) 이 장치 자신이 브리지 - 자기 아래를 리셋하는 것과 혼동되고,
 *      상위 버스를 리셋하면 자기 아래 전부가 함께 날아간다.
 *   3) bus->self 가 없음 - 상위 브리지 구조체를 찾을 수 없다.
 *   4) PCI_DEV_FLAGS_NO_BUS_RESET quirk - 버스 리셋을 받으면 망가지는
 *      것으로 알려진 장치다.
 * 그리고 한 가지 더 — 같은 버스에 다른 장치가 있으면 안 된다. 버스 리셋은
 * 그 버스 전체를 리셋하므로, 목표가 아닌 장치까지 함께 리셋되기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용.
 * 호출자: __pci_reset_function_locked() 가 pci_dev_reset_methods 순서대로 부른다.
 *
 * 호출 체인:
 *   pci_reset_function -> __pci_reset_function_locked
 *     -> [pci_parent_bus_reset] -> pci_bridge_secondary_bus_reset
 */
static int pci_parent_bus_reset(struct pci_dev *dev, bool probe)
{
	struct pci_dev *pdev;	/* [한국어] 같은 버스의 다른 장치를 찾는 순회 커서 */

	/* [한국어] 위 네 가지 불가 조건을 한 번에 검사한다.
	 * pci_is_root_bus: 상위 브리지 없음 / dev->subordinate: 자신이 브리지 /
	 * !dev->bus->self: 상위 브리지 구조체 없음 /
	 * PCI_DEV_FLAGS_NO_BUS_RESET: quirk 로 금지된 장치. */
	if (pci_is_root_bus(dev->bus) || dev->subordinate ||
	    !dev->bus->self || dev->dev_flags & PCI_DEV_FLAGS_NO_BUS_RESET)
		return -ENOTTY;

	/* [한국어] 같은 버스에 다른 장치가 하나라도 있으면 쓸 수 없다.
	 * 루프처럼 보이지만 실제로는 "자기 자신이 아닌 것을 처음 만나면 즉시 실패" 라
	 * 사실상 조건 검사다. 버스 리셋은 대상을 고를 수 없기 때문에,
	 * 이 버스에 이 장치 하나뿐일 때만 안전하다. */
	list_for_each_entry(pdev, &dev->bus->devices, bus_list)
		if (pdev != dev)
			return -ENOTTY;

	/* [한국어] 가능 여부만 묻는 호출이었다면 여기서 "가능" 을 알리고 끝낸다. */
	if (probe)
		return 0;

	/* [한국어] 실제 리셋. bus->self 가 이 버스를 만든 상위 브리지다. */
	return pci_bridge_secondary_bus_reset(dev->bus->self);
}

/*
 * [한국어]
 * pci_reset_hotplug_slot - 핫플러그 드라이버에게 슬롯 리셋을 요청한다
 *
 * @hotplug: 슬롯을 관리하는 핫플러그 컨트롤러. NULL 일 수 있다.
 * @probe:   true 면 가능 여부만 확인, false 면 실제 리셋.
 * @return:  0 = 가능/성공, -ENOTTY = 이 방법을 쓸 수 없음, 그 외 = 드라이버가 준 오류.
 *
 * 슬롯 리셋은 핫플러그 컨트롤러(pciehp, acpiphp, shpchp 등)가 슬롯의 전원을
 * 껐다 켜거나 리셋 신호를 넣는 방식이다. 방법이 컨트롤러마다 다르므로
 * 커널은 콜백으로 위임하고, 이 함수는 그 위임을 안전하게 감싸는 일만 한다.
 *
 * try_module_get / module_put 쌍이 이 함수의 핵심이다. 핫플러그 드라이버는
 * 모듈이라 언제든 언로드될 수 있는데, 콜백을 부르는 도중에 모듈이 사라지면
 * 이미 해제된 코드로 점프하게 된다. try_module_get 이 참조를 잡아 그 사이
 * 언로드를 막고, 콜백이 끝난 뒤 놓는다. try_ 접두사는 "이미 언로드가
 * 진행 중이면 실패한다" 는 뜻이라, 실패하면 이 방법을 포기한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용(콜백이 잠들 수 있다).
 * 호출자: pci_dev_reset_slot_function().
 *
 * 호출 체인:
 *   __pci_reset_function_locked -> pci_dev_reset_slot_function
 *     -> [pci_reset_hotplug_slot] -> hotplug->ops->reset_slot (pciehp_reset_slot 등)
 */
static int pci_reset_hotplug_slot(struct hotplug_slot *hotplug, bool probe)
{
	/* [한국어] "쓸 수 없음" 을 기본값으로 두고, 콜백이 있을 때만 덮어쓴다. */
	int rc = -ENOTTY;

	/* [한국어] 두 가지를 한 번에 확인한다. 슬롯에 핫플러그 컨트롤러가 없거나,
	 * 있어도 그 모듈이 언로드 중이면 이 방법을 쓸 수 없다.
	 * && 가 아니라 || 로 묶여 있으므로 hotplug 가 NULL 이면 try_module_get 은
	 * 아예 평가되지 않는다(단축 평가) — NULL 역참조를 막는다. */
	if (!hotplug || !try_module_get(hotplug->owner))
		return rc;

	/* [한국어] 콜백이 없는 컨트롤러도 있다. 그러면 rc 는 -ENOTTY 로 남는다.
	 * probe 인자를 그대로 넘겨 "가능 여부만 묻는 것" 인지도 함께 전달한다. */
	if (hotplug->ops->reset_slot)
		rc = hotplug->ops->reset_slot(hotplug, probe);

	/* [한국어] 콜백이 끝났으니 모듈 참조를 놓는다. 위 if 를 타지 않았더라도
	 * try_module_get 은 이미 성공했으므로 반드시 짝을 맞춰야 한다. */
	module_put(hotplug->owner);

	return rc;
}

/*
 * [한국어]
 * pci_dev_reset_slot_function - 슬롯 리셋으로 이 장치를 리셋한다
 *
 * @dev:   리셋할 장치
 * @probe: true 면 가능 여부만 확인.
 * @return: 0 = 가능/성공, -ENOTTY = 이 방법을 쓸 수 없음.
 *
 * 리셋 방법 후보 중 하나. 앞의 pci_parent_bus_reset() 과 성격이 같다 —
 * 대상을 고를 수 없는 "광범위한" 리셋이라, 부작용이 없을 때만 허용한다.
 *
 * 쓸 수 없는 조건 네 가지:
 *   1) multifunction - 슬롯 리셋은 그 슬롯의 모든 function 을 리셋한다.
 *      대상 외의 function 까지 날아가므로 허용하지 않는다.
 *   2) 자신이 브리지 - 아래 달린 것들이 전부 함께 리셋된다.
 *   3) 슬롯이 없음 - 납땜된 장치는 슬롯이라는 개념 자체가 없다.
 *   4) PCI_DEV_FLAGS_NO_BUS_RESET quirk - 광범위 리셋이 금지된 장치.
 *
 * NVMe 학습 관점: U.2 백플레인의 NVMe 드라이브는 슬롯이 있고 단일 function 인
 * 경우가 많아 이 조건을 통과한다. 하지만 리셋 순서상 FLR 이 먼저 시도되므로
 * (pci_dev_reset_methods 배열 순서), 실제로 이 경로까지 오는 것은 FLR 을
 * 지원하지 않거나 FLR 이 실패한 드라이브다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용.
 * 호출자: __pci_reset_function_locked().
 */
static int pci_dev_reset_slot_function(struct pci_dev *dev, bool probe)
{
	/* [한국어] 위 네 가지 불가 조건. 하나라도 걸리면 이 방법은 쓸 수 없다. */
	if (dev->multifunction || dev->subordinate || !dev->slot ||
	    dev->dev_flags & PCI_DEV_FLAGS_NO_BUS_RESET)
		return -ENOTTY;

	/* [한국어] 이 슬롯을 관리하는 핫플러그 컨트롤러에게 위임한다.
	 * dev->slot->hotplug 가 NULL 이면 그쪽에서 -ENOTTY 로 걸러진다. */
	return pci_reset_hotplug_slot(dev->slot->hotplug, probe);
}

/*
 * [한국어]
 * cxl_port_dvsec - 이 장치의 CXL Port DVSEC 오프셋을 찾는다
 *
 * @dev:    검사할 장치(보통 CXL 을 지원하는 브리지)
 * @return: config space 안의 오프셋. 없으면 0.
 *
 * DVSEC(Designated Vendor-Specific Extended Capability)는 PCIe 확장 capability
 * 중 하나로, 벤더가 자기만의 레지스터 묶음을 정의할 수 있게 해 준다.
 * CXL 컨소시엄은 자기 벤더 ID(PCI_VENDOR_ID_CXL)로 여러 DVSEC 을 정의했고,
 * 그중 PCI_DVSEC_CXL_PORT 가 포트 제어용이다.
 *
 * 이 오프셋이 필요한 이유는 바로 아래 cxl_sbr_masked() 때문이다. CXL 포트는
 * Secondary Bus Reset 을 소프트웨어가 마음대로 걸지 못하게 막아 둘 수 있는데,
 * 그 마스크 비트가 이 DVSEC 안에 있다.
 *
 * 실행 컨텍스트: 제약 없음. config 읽기뿐이다.
 * 호출자: cxl_sbr_masked(), cxl_reset_bus_function().
 */
static u16 cxl_port_dvsec(struct pci_dev *dev)
{
	/* [한국어] 확장 capability 목록을 훑어 (벤더=CXL, ID=PORT) 인 DVSEC 을 찾는다.
	 * 없으면 0 을 돌려주며, 0 은 유효한 확장 capability 오프셋이 아니므로
	 * (확장 capability 는 0x100 부터 시작) "없음" 을 나타내는 값으로 안전하다. */
	return pci_find_dvsec_capability(dev, PCI_VENDOR_ID_CXL,
					 PCI_DVSEC_CXL_PORT);
}

/*
 * [한국어]
 * cxl_sbr_masked - 이 CXL 포트에서 Secondary Bus Reset 이 무력화돼 있는가
 *
 * @dev:    CXL 포트(브리지)
 * @return: true = SBR 을 걸어도 아무 일도 일어나지 않는다(마스크됨),
 *          false = SBR 이 정상 동작하거나, 판단할 수 없다.
 *
 * CXL 포트는 일반 PCIe 브리지와 달리 SBR 을 기본적으로 막아 둔다. CXL 링크
 * 뒤에는 시스템 메모리로 쓰이는 장치가 붙을 수 있고, 그것을 예고 없이
 * 리셋하면 커널이 쓰던 메모리가 통째로 사라져 즉시 죽기 때문이다.
 * 그래서 CXL 스펙은 "Unmask SBR" 비트를 두고, 이 비트가 1 일 때만 Bridge
 * Control 의 SBR 비트가 실제로 동작하게 했다(CXL spec r3.1, 8.1.5.2).
 *
 * 반환값의 의미가 미묘하다. "판단할 수 없는 경우" 를 모두 false 로 돌려준다 —
 * DVSEC 이 없거나, config 읽기가 실패했거나, all-ones 가 읽힌 경우다.
 * 이는 "마스크돼 있지 않다" 고 낙관하는 것이 아니라, "여기서 막을 근거가
 * 없으니 상위 로직에게 판단을 넘긴다" 는 뜻이다. 실제로 SBR 이 안 먹히면
 * 그다음 단계인 pci_bridge_wait_for_secondary_bus() 에서 실패로 드러난다.
 *
 * 실행 컨텍스트: 제약 없음(config 읽기).
 * 호출자: pci_reset_bus_function().
 */
static bool cxl_sbr_masked(struct pci_dev *dev)
{
	u16 dvsec, reg;	/* [한국어] dvsec = DVSEC 오프셋, reg = Port Control 레지스터 값 */
	int rc;		/* [한국어] config 읽기 결과 */

	/* [한국어] CXL Port DVSEC 이 없다면 이 포트는 CXL 포트가 아니거나
	 * 포트 제어 기능을 노출하지 않는다. 판단 근거가 없으므로 false. */
	dvsec = cxl_port_dvsec(dev);
	if (!dvsec)
		return false;

	/* [한국어] DVSEC 시작 위치 + Port Control 레지스터의 상대 오프셋. */
	rc = pci_read_config_word(dev, dvsec + PCI_DVSEC_CXL_PORT_CTL, &reg);
	/* [한국어] 읽기 실패이거나 all-ones 가 돌아온 경우. 후자는 장치가
	 * 응답하지 않았다는 뜻이고, PCI_POSSIBLE_ERROR 가 그 값을 판정한다.
	 * 둘 다 "모르겠다" 이므로 false. */
	if (rc || PCI_POSSIBLE_ERROR(reg))
		return false;

	/*
	 * Per CXL spec r3.1, sec 8.1.5.2, when "Unmask SBR" is 0, the SBR
	 * bit in Bridge Control has no effect.  When 1, the Port generates
	 * hot reset when the SBR bit is set to 1.
	 */
	/* [한국어] Unmask SBR 이 1 이면 SBR 이 정상 동작한다 -> 마스크돼 있지 않다. */
	if (reg & PCI_DVSEC_CXL_PORT_CTL_UNMASK_SBR)
		return false;

	/* [한국어] Unmask SBR 이 0 이다. SBR 을 걸어도 하드웨어가 무시한다. */
	return true;
}

/*
 * [한국어]
 * pci_reset_bus_function - 슬롯 리셋과 상위 버스 리셋을 차례로 시도한다
 *
 * @dev:   리셋할 장치
 * @probe: true 면 가능 여부만 확인, false 면 실제 리셋.
 * @return: 0 = 가능/성공, -ENOTTY = 두 방법 모두 쓸 수 없음, 그 외 = 오류.
 *
 * "광범위 리셋"(장치 하나가 아니라 그 주변까지 함께 리셋하는 방식) 두 가지를
 * 묶은 함수다. 먼저 슬롯 리셋을, 안 되면 상위 버스 리셋을 시도한다.
 * 두 방법 모두 성립 조건이 까다로워서 대부분의 장치에서는 -ENOTTY 가 난다.
 *
 * 이 함수의 세 가지 공통 작업이 개별 방법에는 없다.
 *   1) CXL 포트의 SBR 마스크 확인 - 막혀 있으면 시도 자체를 하지 않는다.
 *   2) 리셋 전후의 IOMMU 정지/복구 - 리셋으로 장치 상태가 초기화되므로
 *      IOMMU 매핑도 함께 무효화해야 한다.
 *   3) goto done 으로 정리 경로를 하나로 모으기.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용.
 * 호출자: __pci_reset_function_locked() 가 pci_dev_reset_methods 배열의
 *   순서대로 리셋 방법을 시도하며 이것을 부른다. FLR 등 장치 단위 방법이
 *   먼저 오고 이것이 뒤에 온다 — 부작용이 큰 방법일수록 나중이다.
 * 피호출자: cxl_sbr_masked(), pci_dev_reset_iommu_prepare/done(),
 *   pci_dev_reset_slot_function(), pci_parent_bus_reset().
 *
 * 호출 체인:
 *   pci_reset_function -> __pci_reset_function_locked -> [pci_reset_bus_function]
 *     -> pci_dev_reset_slot_function 또는 pci_parent_bus_reset
 */
static int pci_reset_bus_function(struct pci_dev *dev, bool probe)
{
	/* [한국어] 이 장치의 바로 위 브리지. 광범위 리셋은 결국 이 브리지가 수행한다. */
	struct pci_dev *bridge = pci_upstream_bridge(dev);
	int rc;	/* [한국어] 하위 리셋 방법들의 결과 */

	/*
	 * If "dev" is below a CXL port that has SBR control masked, SBR
	 * won't do anything, so return error.
	 */
	/* [한국어] 상위가 CXL 포트이고 그 포트가 SBR 을 막아 두었다면, 리셋을 걸어도
	 * 하드웨어가 무시한다. 그런데도 시도하면 "리셋했다" 고 착각한 채
	 * 응답을 기다리다 타임아웃만 낭비하므로, 미리 이 방법을 포기한다.
	 * 세 조건을 && 로 이어 단축 평가하므로 브리지가 없으면 나머지는
	 * 평가되지 않는다. */
	if (bridge && pcie_is_cxl(bridge) && cxl_sbr_masked(bridge))
		return -ENOTTY;

	/* [한국어] 리셋 전에 IOMMU 를 멈춘다. 리셋이 걸리면 장치의 requester ID 와
	 * 진행 중이던 DMA 상태가 초기화되는데, IOMMU 가 그것을 모른 채 계속
	 * 매핑을 유지하면 리셋 직후의 엉뚱한 DMA 가 통과해 버린다.
	 * probe 인 경우에도 부르는 이유는 아래 방법들이 probe 여부와 무관하게
	 * 같은 준비 상태를 전제하기 때문이다. */
	rc = pci_dev_reset_iommu_prepare(dev);
	if (rc) {
		/* [한국어] IOMMU 를 멈출 수 없으면 리셋을 시도해서는 안 된다.
		 * -ENOTTY 가 아니라 실제 오류를 그대로 올려 보내 상위가
		 * "이 방법이 없다" 가 아니라 "실패했다" 로 구분하게 한다. */
		pci_err(dev, "failed to stop IOMMU for a PCI reset: %d\n", rc);
		return rc;
	}

	/* [한국어] 첫 번째 후보 — 슬롯 리셋. 핫플러그 컨트롤러가 있고 단일
	 * function 인 경우에만 성립한다. */
	rc = pci_dev_reset_slot_function(dev, probe);
	/* [한국어] -ENOTTY 는 "이 방법은 쓸 수 없다" 는 뜻이므로 다음 후보로 넘어간다.
	 * 그 외의 값(성공 0, 또는 실제 오류)은 결론이므로 정리 후 그대로 반환한다. */
	if (rc != -ENOTTY)
		goto done;

	/* [한국어] 두 번째 후보 — 상위 버스 리셋. 이 버스에 이 장치 하나뿐일 때만 성립. */
	rc = pci_parent_bus_reset(dev, probe);
done:
	/* [한국어] 어느 경로로 왔든 IOMMU 를 반드시 되살린다. goto 로 모아 둔 이유가
	 * 이것이다 — 중간에 빠져나가는 경로가 여럿이라 각자 정리하면 빠뜨리기 쉽다. */
	pci_dev_reset_iommu_done(dev);
	return rc;
}

/*
 * [한국어]
 * cxl_reset_bus_function - CXL 포트의 SBR 마스크를 잠시 풀고 버스 리셋을 건다
 *
 * @dev:   리셋할 장치(CXL 포트 아래에 있어야 한다)
 * @probe: true 면 가능 여부만 확인.
 * @return: 0 = 가능/성공, -ENOTTY = 이 방법을 쓸 수 없음, 그 외 = 오류.
 *
 * 바로 위 pci_reset_bus_function() 은 CXL 포트가 SBR 을 막아 두었으면
 * 포기한다. 이 함수는 그 반대 전략이다 — 마스크를 잠시 풀고, 리셋하고,
 * 원래대로 되돌린다.
 *
 * 두 함수가 따로 있는 이유는 위험도가 다르기 때문이다. CXL 포트가 SBR 을
 * 막아 둔 데에는 이유가 있다(그 아래 장치의 메모리를 시스템이 쓰고 있을 수
 * 있다). 그래서 커널은 안전한 방법들을 먼저 다 시도하고, 이 방법은
 * pci_dev_reset_methods 배열의 마지막에 두어 최후 수단으로만 쓴다.
 *
 * 되돌리기(restore)가 이 함수의 핵심이다. 우리가 마스크를 푼 경우에만
 * 다시 씌운다 — 원래부터 풀려 있었다면 건드리지 않는다. reg(원래 값)와
 * val(우리가 쓴 값)을 비교하는 것이 그 판단이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용.
 * 호출자: __pci_reset_function_locked() 가 pci_dev_reset_methods 순서대로 부른다.
 *
 * 호출 체인:
 *   pci_reset_function -> __pci_reset_function_locked
 *     -> [cxl_reset_bus_function] -> pci_reset_bus_function -> pci_parent_bus_reset
 */
static int cxl_reset_bus_function(struct pci_dev *dev, bool probe)
{
	struct pci_dev *bridge;	/* [한국어] 상위 CXL 포트 */
	/* [한국어] dvsec = DVSEC 오프셋, reg = 원래 Port Control 값,
	 * val = 우리가 실제로 써 넣은 값. 마지막에 둘을 비교해 되돌릴지 정한다. */
	u16 dvsec, reg, val;
	int rc;

	/* [한국어] 상위 브리지가 없으면 버스 리셋 자체가 불가능하다. */
	bridge = pci_upstream_bridge(dev);
	if (!bridge)
		return -ENOTTY;

	/* [한국어] 상위가 CXL 포트가 아니면 이 방법은 해당 사항이 없다. */
	dvsec = cxl_port_dvsec(bridge);
	if (!dvsec)
		return -ENOTTY;

	/* [한국어] 가능 여부만 묻는 호출이었다면 여기까지가 답이다.
	 * 아래는 전부 실제로 하드웨어를 바꾸는 동작이므로 probe 에서는 하면 안 된다. */
	if (probe)
		return 0;

	/* [한국어] 원래 값을 읽어 둔다. 마지막에 되돌리기 위해 반드시 필요하다.
	 * 여기서 실패하면 되돌릴 수 없게 되므로 아예 시작하지 않는다. */
	rc = pci_read_config_word(bridge, dvsec + PCI_DVSEC_CXL_PORT_CTL, &reg);
	if (rc)
		return -ENOTTY;

	/* [한국어] 리셋 전 IOMMU 정지. 아래 pci_reset_bus_function() 도 같은 일을
	 * 하지만, 그 함수는 probe=false 로 불릴 때 자기 안에서 다시 prepare/done
	 * 쌍을 수행한다. 중첩 호출을 견디도록 설계된 함수들이다. */
	rc = pci_dev_reset_iommu_prepare(dev);
	if (rc) {
		pci_err(dev, "failed to stop IOMMU for a PCI reset: %d\n", rc);
		return rc;
	}

	/* [한국어] 이미 마스크가 풀려 있으면 아무것도 쓰지 않는다. val 을 reg 와
	 * 같게 두면 아래 "reg != val" 이 거짓이 되어 되돌리기도 건너뛴다. */
	if (reg & PCI_DVSEC_CXL_PORT_CTL_UNMASK_SBR) {
		val = reg;
	} else {
		/* [한국어] 마스크를 푼다. 이 순간부터 Bridge Control 의 SBR 비트가
		 * 실제로 동작한다. 원래 값 reg 는 그대로 보존해 둔다. */
		val = reg | PCI_DVSEC_CXL_PORT_CTL_UNMASK_SBR;
		pci_write_config_word(bridge, dvsec + PCI_DVSEC_CXL_PORT_CTL,
				      val);
	}

	/* [한국어] 이제 일반 버스 리셋 경로를 탄다. 마스크가 풀렸으므로
	 * 그 안의 cxl_sbr_masked() 검사도 통과한다. */
	rc = pci_reset_bus_function(dev, probe);

	/* [한국어] 우리가 바꿨을 때만 되돌린다. 원래 풀려 있던 포트를 우리가
	 * 임의로 막아 버리면 다른 코드의 전제가 깨진다. */
	if (reg != val)
		pci_write_config_word(bridge, dvsec + PCI_DVSEC_CXL_PORT_CTL,
				      reg);

	/* [한국어] IOMMU 복구. 위 prepare 와 짝을 이룬다. */
	pci_dev_reset_iommu_done(dev);
	return rc;
}

/*
 * [한국어]
 * pci_dev_lock - 장치를 "아무도 건드리지 못하는" 상태로 만든다
 *
 * @dev: 잠글 장치
 * @return: 없음. 얻을 때까지 기다린다.
 *
 * 리셋처럼 장치를 통째로 흔드는 동작 전에 두 겹의 잠금을 건다.
 *   1) device_lock() - 드라이버 코어의 장치 뮤텍스. 이것을 쥐면 probe,
 *      remove, 전원 관리 콜백이 전부 대기한다. 즉 드라이버가 리셋 도중에
 *      바인딩되거나 떨어져 나가는 일이 없다.
 *   2) pci_cfg_access_lock() - config 접근 차단 플래그. userspace(sysfs,
 *      /proc/bus/pci, VFIO)의 config 읽기·쓰기를 재운다.
 *
 * 두 잠금의 성격이 다르다는 점이 중요하다. device_lock 은 커널 내부 경로를
 * 막고, cfg_access_lock 은 바깥 경로를 막는다. 리셋 중인 장치의 config 는
 * 아무 값이나 돌려주므로, 그 값을 읽고 판단하는 코드가 하나라도 있으면
 * 오작동한다.
 *
 * 순서가 정해져 있다. 항상 device_lock 을 먼저 잡는다. 반대 순서로 잡는
 * 코드가 생기면 교착이 나므로, 이 함수 하나로 순서를 고정해 둔 것이다.
 * 해제는 pci_dev_unlock() 이 역순으로 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용. 두 잠금 모두 잠들 수 있다.
 * 호출자: pci_reset_function(), pci_try_reset_function(), VFIO.
 *
 * 호출 체인:
 *   pci_reset_function -> [pci_dev_lock] -> device_lock + pci_cfg_access_lock
 */
void pci_dev_lock(struct pci_dev *dev)
{
	/* block PM suspend, driver probe, etc. */
	/* [한국어] 커널 내부 경로 차단. 이 뮤텍스가 probe/remove/PM 콜백의
	 * 직렬화 지점이라, 쥐고 있는 동안 드라이버 상태가 바뀌지 않는다. */
	device_lock(&dev->dev);
	/* [한국어] userspace 경로 차단. dev->block_cfg_access 를 세워
	 * pci_user_read/write_config_* 를 재운다. 반드시 device_lock 뒤에
	 * 와야 한다 — 순서를 뒤집으면 다른 경로와 교착할 수 있다. */
	pci_cfg_access_lock(dev);
}
EXPORT_SYMBOL_GPL(pci_dev_lock);

/* Return 1 on successful lock, 0 on contention */
/*
 * [한국어]
 * pci_dev_trylock - 기다리지 않고 두 잠금을 모두 얻어 본다
 *
 * @dev: 잠글 장치
 * @return: 1 = 두 잠금 모두 획득(반드시 pci_dev_unlock 해야 한다),
 *          0 = 둘 중 하나라도 얻지 못함(아무것도 잡지 않은 상태로 돌아간다).
 *
 * pci_dev_lock() 의 기다리지 않는 판이다. 잠들 수 없는 문맥이나, 잠금을
 * 얻지 못했을 때 다른 방법으로 대응해야 하는 곳에서 쓴다.
 *
 * 두 잠금을 원자적으로 얻어야 한다는 점이 이 함수의 어려운 부분이다.
 * 첫 번째는 얻었는데 두 번째를 못 얻으면, 첫 번째를 반드시 되돌려 놓아야
 * 한다. 그러지 않으면 "실패했다" 고 알리면서 잠금은 쥐고 있는 상태가 되어,
 * 호출자가 unlock 을 부르지 않으니 그 잠금이 영원히 풀리지 않는다.
 * 중간의 device_unlock() 이 그 되돌리기다.
 *
 * 반환값 규약이 커널의 다른 trylock 들과 같다 — 성공이 1, 실패가 0 이다.
 * 함수 대부분이 0 을 성공으로 쓰는 것과 반대이므로 주의해야 한다.
 * (원문 주석 "Return 1 on successful lock, 0 on contention" 이 그것을 못박는다.)
 *
 * 실행 컨텍스트: 잠들지 않는다. 인터럽트 문맥에서도 부를 수 있다.
 * 호출자: VFIO 의 리셋 경로, AER 복구처럼 기다릴 수 없는 곳.
 */
int pci_dev_trylock(struct pci_dev *dev)
{
	/* [한국어] 첫 번째 잠금. 이미 누가 쥐고 있으면 0 을 돌려주고 끝난다. */
	if (device_trylock(&dev->dev)) {
		/* [한국어] 두 번째 잠금. 성공하면 둘 다 쥔 상태로 1 을 반환한다. */
		if (pci_cfg_access_trylock(dev))
			return 1;
		/* [한국어] 두 번째를 못 얻었다. 첫 번째를 반드시 되돌린다 —
		 * 이 한 줄이 없으면 장치 뮤텍스가 영구히 잠긴다. */
		device_unlock(&dev->dev);
	}

	/* [한국어] 실패. 잠금을 하나도 쥐지 않은 상태로 돌아간다. */
	return 0;
}
EXPORT_SYMBOL_GPL(pci_dev_trylock);

/*
 * [한국어]
 * pci_dev_unlock - pci_dev_lock() 이 건 두 잠금을 푼다
 *
 * @dev: 풀 장치
 * @return: 없음.
 *
 * 해제 순서가 획득의 역순이라는 점이 전부다. 잠금은
 * device_lock -> cfg_access_lock 순으로 잡았으므로,
 * cfg_access_unlock -> device_unlock 순으로 푼다.
 *
 * 역순을 지키는 이유: cfg_access_unlock() 은 잠들어 있던 userspace 태스크를
 * 깨우는데, 그들이 깨어나자마자 device_lock 을 기다리게 되면 불필요한
 * 경쟁이 생긴다. 순서를 지키면 깨어난 태스크가 곧바로 진행한다.
 * 더 중요하게는, 잠금 순서를 일관되게 유지해야 교착 분석(lockdep)이
 * 성립한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용.
 * 호출자: pci_dev_lock() 또는 1 을 돌려받은 pci_dev_trylock() 과 짝을 이루는 코드.
 */
void pci_dev_unlock(struct pci_dev *dev)
{
	/* [한국어] 바깥 경로부터 연다. 기다리던 userspace 접근이 깨어난다. */
	pci_cfg_access_unlock(dev);
	/* [한국어] 그다음 커널 내부 경로. probe/remove/PM 이 다시 진행된다. */
	device_unlock(&dev->dev);
}
EXPORT_SYMBOL_GPL(pci_dev_unlock);

/*
 * [한국어]
 * pci_dev_save_and_disable - 리셋 직전에 상태를 보관하고 장치를 완전히 잠재운다
 *
 * @dev: 리셋할 장치
 * @return: 없음.
 *
 * 리셋은 config space 를 초기화하므로, 리셋 후 장치를 다시 쓰려면 지금 값을
 * 어딘가 적어 두어야 한다. 그리고 리셋이 진행되는 동안 장치가 DMA 를
 * 계속하거나 인터럽트를 쏘면 안 되므로 모든 활동을 멈춰야 한다.
 * 이 함수가 그 둘을 순서대로 한다.
 *
 * 네 단계다.
 *   1) 드라이버에게 알린다(reset_prepare). 드라이버는 진행 중인 요청을
 *      정리하고 하드웨어에 더 이상 접근하지 않을 준비를 한다.
 *   2) 장치를 D0 로 깨운다. D3 상태에서는 config space 접근이 제한되고,
 *      리셋 후 어차피 D0 가 되므로 지금 맞춰 두는 편이 낫다.
 *   3) config space 를 통째로 저장한다.
 *   4) Command 레지스터를 0(+INTx Disable)으로 만들어 장치를 무력화한다.
 *
 * 4단계가 이 함수에서 가장 중요하다. Command 를 지우면 세 가지가 동시에
 * 꺼진다 — MMIO/IO 디코딩(BAR 접근 차단), Bus Master(DMA 차단),
 * 그리고 INTx Disable 비트를 세워 레거시 인터럽트까지 막는다.
 * Bus Master 를 끄는 것이 특히 중요하다. NVMe 처럼 스스로 메모리를 읽고
 * 쓰는 장치가 리셋 도중에 DMA 를 하면, 커널이 이미 해제한 메모리를
 * 덮어쓸 수 있다. MSI/MSI-X 도 결국 메모리 쓰기이므로 함께 차단된다.
 *
 * 실행 컨텍스트: 호출자가 device lock 을 쥐고 있어야 한다.
 *   device_lock_assert 가 그것을 강제한다. 이 잠금이 있어야 reset_prepare
 *   콜백을 부르는 도중에 드라이버가 언바인딩되지 않는다.
 * 호출자: pci_reset_function(), pci_try_reset_function(), 버스 단위 리셋.
 * 짝: pci_dev_restore() 가 이것을 되돌린다.
 *
 * 호출 체인:
 *   pci_reset_function -> [pci_dev_save_and_disable]
 *     -> err_handler->reset_prepare (nvme_reset_prepare 등)
 *     -> pci_save_state -> pci_write_config_word(PCI_COMMAND)
 */
static void pci_dev_save_and_disable(struct pci_dev *dev)
{
	/* [한국어] 드라이버가 등록한 오류 처리 콜백 묶음. 드라이버가 바인딩돼
	 * 있지 않거나 err_handler 를 제공하지 않으면 NULL 이다. */
	const struct pci_error_handlers *err_handler =
			dev->driver ? dev->driver->err_handler : NULL;

	/*
	 * dev->driver->err_handler->reset_prepare() is protected against
	 * races with ->remove() by the device lock, which must be held by
	 * the caller.
	 */
	/* [한국어] 호출자가 device lock 을 쥐고 있는지 확인한다. 이 잠금이
	 * reset_prepare 호출과 드라이버 언바인딩(->remove) 사이의 경쟁을 막는다.
	 * CONFIG_LOCKDEP 빌드에서만 실제로 검사한다. */
	device_lock_assert(&dev->dev);
	/* [한국어] 1단계 — 드라이버에게 "곧 리셋한다" 고 알린다. */
	if (err_handler && err_handler->reset_prepare)
		err_handler->reset_prepare(dev);
	/* [한국어] 드라이버는 붙어 있는데 준비 콜백이 없는 경우. 알릴 방법이
	 * 없으므로 로그만 남긴다. 드라이버 입장에서는 예고 없이 하드웨어가
	 * 초기화되는 셈이라, 이 경고가 문제 추적의 실마리가 된다. */
	else if (dev->driver)
		pci_warn(dev, "resetting");

	/*
	 * Wake-up device prior to save.  PM registers default to D0 after
	 * reset and a simple register restore doesn't reliably return
	 * to a non-D0 state anyway.
	 */
	/* [한국어] 2단계 — D0 로 깨운다. 두 가지 이유가 있다.
	 * (1) D3 에서는 config space 의 일부만 접근 가능해 저장이 불완전해진다.
	 * (2) 리셋 후 전원 상태는 어차피 D0 가 되므로, 저장해 둔 상태와
	 *     실제 상태를 맞춰 두는 편이 복원할 때 어긋나지 않는다. */
	pci_set_power_state(dev, PCI_D0);

	/* [한국어] 3단계 — config space 전체를 dev->saved_config_space 에 복사한다.
	 * BAR, Command, Interrupt Line, 그리고 capability 들의 상태가 모두 담긴다. */
	pci_save_state(dev);
	/*
	 * Disable the device by clearing the Command register, except for
	 * INTx-disable which is set.  This not only disables MMIO and I/O port
	 * BARs, but also prevents the device from being Bus Master, preventing
	 * DMA from the device including MSI/MSI-X interrupts.  For PCI 2.3
	 * compliant devices, INTx-disable prevents legacy interrupts.
	 */
	/* [한국어] 4단계 — Command 레지스터를 INTx Disable 하나만 남기고 전부 0 으로.
	 * 값 자체가 곧 의미다: Memory Space Enable=0(BAR 접근 차단),
	 * I/O Space Enable=0, Bus Master Enable=0(DMA 차단),
	 * INTx Disable=1(레거시 인터럽트 차단).
	 * 저장을 먼저 하고 이 쓰기를 나중에 하는 순서가 중요하다 —
	 * 순서가 바뀌면 지워진 Command 값이 저장돼 복원이 무의미해진다. */
	pci_write_config_word(dev, PCI_COMMAND, PCI_COMMAND_INTX_DISABLE);
}

/*
 * [한국어]
 * pci_dev_restore - 리셋 후 상태를 되돌리고 드라이버에게 알린다
 *
 * @dev: 리셋을 마친 장치
 * @return: 없음.
 *
 * pci_dev_save_and_disable() 의 짝이다. 순서가 그것의 역순이라는 점이 핵심이다.
 *   save 쪽: 드라이버에게 알림 -> 상태 저장 -> 장치 무력화
 *   restore 쪽: 상태 복원 -> 드라이버에게 알림
 *
 * 왜 복원이 먼저이고 알림이 나중인가. reset_done 콜백을 받은 드라이버는
 * 곧바로 하드웨어를 만지기 시작한다 — 큐를 다시 만들고, 도어벨을 두드리고,
 * 인터럽트를 등록한다. 그러려면 BAR 가 이미 제자리에 있고 Bus Master 가
 * 켜져 있어야 한다. 알림이 먼저 가면 드라이버가 아직 복원되지 않은
 * 하드웨어를 건드리게 된다.
 *
 * pci_restore_state() 가 Command 레지스터를 되살리므로, save 단계에서 지웠던
 * Memory Space Enable 과 Bus Master Enable 이 여기서 다시 켜진다.
 *
 * NVMe 관점: nvme_reset_done() 이 이 콜백으로 불려 컨트롤러 재초기화를
 * 시작한다(nvme_reset_ctrl 을 큐잉). 그래서 이 시점에 BAR0 매핑이
 * 유효해야 CC/CSTS 레지스터에 접근할 수 있다.
 *
 * 실행 컨텍스트: 호출자가 device lock 을 쥐고 있어야 한다(위 원문 주석).
 * 호출자: pci_reset_function() 등, pci_dev_save_and_disable() 을 부른 그 코드.
 *
 * 호출 체인:
 *   pci_reset_function -> [pci_dev_restore] -> pci_restore_state
 *     -> err_handler->reset_done (nvme_reset_done 등)
 */
static void pci_dev_restore(struct pci_dev *dev)
{
	/* [한국어] save 쪽과 같은 방식으로 콜백 묶음을 꺼낸다. 그 사이 드라이버가
	 * 바뀌지 않았음은 호출자가 쥔 device lock 이 보장한다. */
	const struct pci_error_handlers *err_handler =
			dev->driver ? dev->driver->err_handler : NULL;

	/* [한국어] 저장해 둔 config space 를 하드웨어에 다시 써 넣는다.
	 * BAR 주소, Command(Memory/Bus Master Enable 포함), MSI/MSI-X 설정이
	 * 이 한 번의 호출로 되살아난다. 반드시 아래 알림보다 먼저 와야 한다. */
	pci_restore_state(dev);

	/*
	 * dev->driver->err_handler->reset_done() is protected against
	 * races with ->remove() by the device lock, which must be held by
	 * the caller.
	 */
	/* [한국어] 드라이버에게 "리셋이 끝났고 하드웨어는 복원됐다" 고 알린다.
	 * 드라이버는 여기서 자기 자료구조를 재초기화한다. */
	if (err_handler && err_handler->reset_done)
		err_handler->reset_done(dev);
	/* [한국어] 콜백이 없는 드라이버. 알릴 방법이 없으니 로그만 남긴다.
	 * 이 드라이버는 자기 하드웨어가 리셋됐다는 사실을 영영 모르므로,
	 * 대개 다음 I/O 에서 오류를 만나 스스로 복구를 시작하게 된다. */
	else if (dev->driver)
		pci_warn(dev, "reset done");
}

/* dev->reset_methods[] is a 0-terminated list of indices into this array */
/* [한국어] 커널이 알고 있는 모든 리셋 방법의 목록. 순서가 곧 우선순위다 —
 * 위에 있을수록 먼저 시도하며, 부작용이 작은 방법일수록 위에 온다.
 *
 * 0번 항목이 빈 칸인 것이 이 배열의 설계다. dev->reset_methods[] 는 이
 * 배열의 인덱스를 담은 0-종료 목록인데, 0 을 "끝" 표시로 쓰려면 0번이
 * 실제 방법이어서는 안 되기 때문이다. 그래서 자리 하나를 비워 둔다.
 *
 * 각 방법과 그 범위:
 *   device_specific - quirks.c 의 pci_dev_reset_methods[] 에 등록된 장치별
 *                     맞춤 절차. 삼성 SM961/PM961 의 nvme_disable_and_flr()
 *                     이 여기 해당한다.
 *   acpi            - 펌웨어가 제공하는 _RST 메서드. 플랫폼이 방법을 안다.
 *   flr             - PCIe Function Level Reset. 이 function 하나만 리셋하며
 *                     이웃에 영향이 없어, 표준 방법 중 가장 안전하다.
 *                     NVMe 드라이버가 pcie_reset_flr() 로 직접 부르기도 한다.
 *   af_flr          - PCI-X/AF(Advanced Features) 방식의 FLR. PCIe 이전 세대용.
 *   pm              - D3hot 으로 갔다가 D0 로 돌아오게 해 리셋을 유도한다.
 *                     스펙상 D3hot -> D0 전환이 장치를 초기화하기 때문이다.
 *   bus             - 슬롯 리셋 또는 상위 버스 리셋. 주변까지 함께 리셋된다.
 *   cxl_bus         - CXL 포트의 SBR 마스크를 풀고 버스 리셋. 가장 위험해서
 *                     마지막이다.
 *
 * 이 배열은 sysfs 의 reset_method 속성에도 쓰인다 — 사용자가 이름으로
 * 특정 방법만 허용하거나 순서를 바꿀 수 있고, .name 필드가 그때의 이름이다. */
const struct pci_reset_fn_method pci_reset_fn_methods[] = {
	{ },	/* [한국어] 0번은 "목록의 끝" 을 나타내는 자리. 실제 방법이 아니다 */
	{ pci_dev_specific_reset, .name = "device_specific" },	/* [한국어] quirks.c 의 장치별 절차 */
	{ pci_dev_acpi_reset, .name = "acpi" },			/* [한국어] 펌웨어 _RST */
	{ pcie_reset_flr, .name = "flr" },			/* [한국어] PCIe Function Level Reset */
	{ pci_af_flr, .name = "af_flr" },			/* [한국어] PCI-X AF 방식 FLR */
	{ pci_pm_reset, .name = "pm" },				/* [한국어] D3hot -> D0 전환 유도 */
	{ pci_reset_bus_function, .name = "bus" },		/* [한국어] 슬롯/상위 버스 리셋 */
	{ cxl_reset_bus_function, .name = "cxl_bus" },		/* [한국어] CXL 마스크 해제 후 버스 리셋 */
};

/**
 * __pci_reset_function_locked - reset a PCI device function while holding
 * the @dev mutex lock.
 * @dev: PCI device to reset
 *
 * Some devices allow an individual function to be reset without affecting
 * other functions in the same device.  The PCI device must be responsive
 * to PCI config space in order to use this function.
 *
 * The device function is presumed to be unused and the caller is holding
 * the device mutex lock when this function is called.
 *
 * Resetting the device will make the contents of PCI configuration space
 * random, so any caller of this must be prepared to reinitialise the
 * device including MSI, bus mastering, BARs, decoding IO and memory spaces,
 * etc.
 *
 * Context: The caller must hold the device lock.
 *
 * Return: 0 if the device function was successfully reset or negative if the
 * device doesn't support resetting a single function.
 */
/*
 * [한국어]
 * __pci_reset_function_locked - 준비된 리셋 방법들을 순서대로 시도한다
 *
 * @dev: 리셋할 장치
 * @return: 0 = 어느 한 방법으로 리셋 성공,
 *          -ENOTTY = 쓸 수 있는 방법이 하나도 없거나 모두 "해당 없음",
 *          그 외 음수 = 어떤 방법이 실제로 실패했다.
 *
 * 리셋의 실제 실행부다. dev->reset_methods[] 에 미리 담아 둔 방법 인덱스를
 * 앞에서부터 하나씩 꺼내 실행하고, 하나라도 성공하면 즉시 끝낸다.
 *
 * 반환값의 세 갈래를 구분하는 것이 이 함수의 핵심 로직이다.
 *   0        - 성공. 더 시도하지 않는다.
 *   -ENOTTY  - "이 방법은 이 장치에 해당하지 않는다." 다음 방법으로 넘어간다.
 *   그 외    - 실제 실패. 여기서 멈춘다. 원문 주석이 그 이유를 밝힌다 —
 *              한 방법이 진짜로 실패했다면 장치가 이상한 상태일 가능성이
 *              높고, 그 위에 다른 리셋을 더 거는 것은 상황을 악화시킨다.
 *
 * 이름 앞의 __ 는 "잠금과 상태 저장/복원을 호출자가 책임진다" 는 뜻이다.
 * 그것까지 해 주는 판이 pci_reset_function() 이다. 리셋은 config space 를
 * 임의의 값으로 만들므로, 이 함수만 부르고 복원을 하지 않으면 장치는
 * BAR 도 Bus Master 도 없는 상태로 남는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용(might_sleep). 호출자가 device lock 을
 *   쥐고 있어야 한다(device_lock_assert).
 * 호출자: pci_reset_function(), pci_reset_function_locked(), VFIO.
 * 피호출자: pci_reset_fn_methods[] 의 각 reset_fn.
 *
 * 호출 체인:
 *   pci_reset_function -> [__pci_reset_function_locked]
 *     -> pcie_reset_flr / pci_pm_reset / pci_reset_bus_function ...
 */
int __pci_reset_function_locked(struct pci_dev *dev)
{
	/* [한국어] i = reset_methods 배열의 위치, m = 그 자리에 담긴 방법 인덱스,
	 * rc = 방법이 돌려준 결과. i 와 m 이 다른 것이 중요하다 — reset_methods 는
	 * "쓸 수 있는 방법만" 앞에서부터 채워 둔 압축 목록이라 인덱스가 어긋난다. */
	int i, m, rc;
	const struct pci_reset_fn_method *method;	/* [한국어] 현재 시도할 방법 */

	/* [한국어] 리셋 방법들이 msleep 을 쓰므로 잠들 수 있다. 원자 문맥에서
	 * 불렸다면 디버그 빌드가 여기서 경고한다. */
	might_sleep();
	/* [한국어] device lock 이 있어야 리셋 도중 드라이버가 바뀌지 않는다. */
	device_lock_assert(&dev->dev);

	/*
	 * A reset method returns -ENOTTY if it doesn't support this device and
	 * we should try the next method.
	 *
	 * If it returns 0 (success), we're finished.  If it returns any other
	 * error, we're also finished: this indicates that further reset
	 * mechanisms might be broken on the device.
	 */
	for (i = 0; i < PCI_NUM_RESET_METHODS; i++) {
		m = dev->reset_methods[i];	/* [한국어] 이 자리에 담긴 방법 인덱스 */
		/* [한국어] 0 은 목록의 끝 표시다(pci_reset_fn_methods[0] 이 빈 칸인 이유).
		 * 여기까지 왔다는 것은 모든 방법이 "해당 없음" 이었다는 뜻이다. */
		if (!m)
			return -ENOTTY;

		method = &pci_reset_fn_methods[m];	/* [한국어] 인덱스로 실제 방법을 찾는다 */
		/* [한국어] 어떤 방법을 시도하는지 로그로 남긴다. 리셋이 왜 실패했는지
		 * 추적할 때 이 순서가 결정적인 단서가 된다. */
		pci_dbg(dev, "reset via %s\n", method->name);
		/* [한국어] PCI_RESET_DO_RESET(=false) 을 넘겨 "실제로 리셋하라" 고 지시한다.
		 * PCI_RESET_PROBE(=true) 였다면 가능 여부만 확인하고 끝난다. */
		rc = method->reset_fn(dev, PCI_RESET_DO_RESET);
		if (!rc)
			return 0;	/* [한국어] 성공. 더 시도할 이유가 없다 */

		pci_dbg(dev, "%s failed with %d\n", method->name, rc);
		/* [한국어] -ENOTTY 가 아니라면 실제 실패다. 위 원문 주석대로,
		 * 이런 상태에서 다음 방법을 더 시도하면 장치가 더 망가질 수 있으므로
		 * 그 오류를 그대로 올려 보낸다. */
		if (rc != -ENOTTY)
			return rc;
		/* [한국어] -ENOTTY 였다면 루프가 계속되어 다음 방법으로 넘어간다. */
	}

	/* [한국어] 배열을 끝까지 돌았는데 0 표시를 못 만난 경우.
	 * reset_methods 가 가득 찼고 전부 -ENOTTY 였다는 뜻이다. */
	return -ENOTTY;
}
EXPORT_SYMBOL_GPL(__pci_reset_function_locked);

/**
 * pci_init_reset_methods - check whether device can be safely reset
 * and store supported reset mechanisms.
 * @dev: PCI device to check for reset mechanisms
 *
 * Some devices allow an individual function to be reset without affecting
 * other functions in the same device.  The PCI device must be in D0-D3hot
 * state.
 *
 * Stores reset mechanisms supported by device in reset_methods byte array
 * which is a member of struct pci_dev.
 */
/*
 * [한국어]
 * pci_init_reset_methods - 이 장치에 쓸 수 있는 리셋 방법을 미리 조사해 둔다
 *
 * @dev: 조사할 장치
 * @return: 없음. 결과는 dev->reset_methods[] 에 담긴다.
 *
 * 장치를 열거할 때 한 번 실행되어, 모든 리셋 방법에 "이 장치에 쓸 수 있는가"
 * 를 물어보고 쓸 수 있는 것들의 인덱스만 앞에서부터 채워 넣는다.
 * 실제 리셋 때마다 매번 물어보지 않기 위한 캐시다.
 *
 * 결과 배열의 모양이 특이하다. 예컨대 FLR(3번)과 버스 리셋(6번)만 가능한
 * 장치라면 reset_methods = {3, 6, 0, ...} 이 된다. 값은 pci_reset_fn_methods
 * 배열의 인덱스이고, 0 이 목록의 끝이다. 그래서 그 배열의 0번을 비워 둔 것이다.
 *
 * 루프가 m=1 부터 시작하는 것도 같은 이유다 — 0번은 실제 방법이 아니다.
 *
 * 도중에 break 하는 조건이 미묘하다. 어떤 방법이 -ENOTTY 가 아닌 오류를
 * 돌려주면 거기서 조사를 멈춘다. probe 단계에서조차 오류가 났다는 것은
 * 장치가 config 접근에 제대로 응답하지 않는다는 뜻이고, 그 상태에서 뒤쪽
 * 방법들을 계속 물어봐야 결과를 믿을 수 없기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용(might_sleep). 열거 중에 불린다.
 * 호출자: probe.c 의 pci_init_capabilities() 경로.
 * 결과를 쓰는 곳: __pci_reset_function_locked(), sysfs 의 reset_method 속성.
 *
 * 호출 체인:
 *   pci_device_add -> pci_init_capabilities -> [pci_init_reset_methods]
 *     -> 각 reset_fn(dev, PCI_RESET_PROBE)
 */
void pci_init_reset_methods(struct pci_dev *dev)
{
	/* [한국어] m = 조사 중인 방법 인덱스, i = 결과 배열에 채워 넣을 위치.
	 * 둘이 다르게 움직이는 것이 핵심이다 — 쓸 수 있는 방법만 i 를 전진시킨다. */
	int m, i, rc;

	/* [한국어] 컴파일 시점 검사. 방법 배열의 크기와 상수가 어긋나면 빌드가
	 * 실패한다. 새 리셋 방법을 추가하면서 PCI_NUM_RESET_METHODS 를 올리는 것을
	 * 잊으면 아래 루프가 배열 밖을 읽게 되는데, 그것을 원천 차단한다. */
	BUILD_BUG_ON(ARRAY_SIZE(pci_reset_fn_methods) != PCI_NUM_RESET_METHODS);

	/* [한국어] probe 단계에서도 일부 방법(ACPI 등)은 잠들 수 있다. */
	might_sleep();

	i = 0;	/* [한국어] 결과 배열의 첫 자리부터 채운다 */
	/* [한국어] 1부터 시작 — 0번은 "끝" 표시 전용 자리다.
	 * 순서대로 조사하므로 결과 배열도 우선순위 순서를 유지한다. */
	for (m = 1; m < PCI_NUM_RESET_METHODS; m++) {
		/* [한국어] PCI_RESET_PROBE(=true)를 넘겨 "실제로 하지 말고 가능한지만
		 * 답하라" 고 요청한다. 각 방법이 이 인자를 보고 조건 검사만 하고 돌아온다. */
		rc = pci_reset_fn_methods[m].reset_fn(dev, PCI_RESET_PROBE);
		/* [한국어] 0 = 쓸 수 있다. 결과 배열에 인덱스를 기록하고 i 를 전진. */
		if (!rc)
			dev->reset_methods[i++] = m;
		/* [한국어] -ENOTTY 가 아닌 오류 = 장치 상태가 이상하다. 뒤쪽 방법의
		 * 조사 결과도 믿을 수 없으므로 여기서 멈춘다. */
		else if (rc != -ENOTTY)
			break;
		/* [한국어] -ENOTTY 였다면 i 를 그대로 두고 다음 방법으로 넘어간다. */
	}

	/* [한국어] 목록의 끝 표시. break 로 빠져나왔든 끝까지 돌았든 반드시
	 * 여기서 0 을 넣어야 __pci_reset_function_locked 가 멈출 지점을 안다.
	 * 배열 크기가 PCI_NUM_RESET_METHODS 이고 i 는 최대 그보다 1 작으므로
	 * 이 쓰기는 항상 배열 안이다. */
	dev->reset_methods[i] = 0;
}

/**
 * pci_reset_function - quiesce and reset a PCI device function
 * @dev: PCI device to reset
 *
 * Some devices allow an individual function to be reset without affecting
 * other functions in the same device.  The PCI device must be responsive
 * to PCI config space in order to use this function.
 *
 * This function does not just reset the PCI portion of a device, but
 * clears all the state associated with the device.  This function differs
 * from __pci_reset_function_locked() in that it saves and restores device state
 * over the reset and takes the PCI device lock.
 *
 * Returns 0 if the device function was successfully reset or negative if the
 * device doesn't support resetting a single function.
 */
/*
 * [한국어]
 * pci_reset_function - 장치를 잠그고, 상태를 보관하고, 리셋하고, 되돌린다
 *
 * @dev: 리셋할 장치
 * @return: 0 = 성공, 음수 = 쓸 수 있는 방법이 없거나 리셋이 실패했다.
 *
 * 리셋 절차 전체를 한 번에 처리하는 "완성품" 진입점이다.
 * __pci_reset_function_locked() 가 리셋 자체만 한다면, 이 함수는 그 앞뒤에
 * 필요한 모든 것을 붙인다.
 *
 *   잠금(상위 브리지 -> 자신) -> 상태 저장 및 무력화 -> 리셋
 *     -> 상태 복원 -> 잠금 해제(자신 -> 상위 브리지)
 *
 * 상위 브리지까지 잠그는 이유가 이 함수에서 가장 설명이 필요한 부분이다.
 * 리셋 방법 중에는 상위 브리지를 건드리는 것들이 있다 — 버스 리셋은
 * 브리지의 Bridge Control 레지스터를 쓰고, CXL 리셋은 브리지의 DVSEC 을
 * 건드린다. 그 사이에 브리지 자신이 절전에 들어가거나 드라이버가 떨어지면
 * 리셋 절차가 어긋난다. 그래서 브리지도 함께 얼려 둔다.
 * 브리지가 없는 경우(루트 버스 직결)에는 그럴 대상이 없으므로 건너뛴다.
 *
 * 잠금 순서 — 위에서 아래로 잡고, 아래에서 위로 푼다. 이 순서를 모든
 * 코드가 지켜야 교착이 나지 않는다.
 *
 * NVMe 관점: drivers/nvme/ 에는 이 함수 호출이 없다(전수 확인).
 * NVMe 컨트롤러 리셋은 nvme_reset_ctrl() 이 NVMe 스펙의 CC.EN 절차로
 * 직접 수행하며, PCI 리셋이 필요한 경우에도 pcie_reset_flr() 을 직접 부른다.
 * 이 함수를 타는 경로는 sysfs 의 reset 속성이나 VFIO 장치 할당 쪽이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용. 잠금과 msleep 이 있다.
 * 호출자: pci-sysfs.c 의 reset 속성, VFIO, 일부 드라이버.
 *
 * 호출 체인:
 *   echo 1 > /sys/bus/pci/devices/.../reset -> [pci_reset_function]
 *     -> pci_dev_save_and_disable -> __pci_reset_function_locked -> pci_dev_restore
 */
int pci_reset_function(struct pci_dev *dev)
{
	struct pci_dev *bridge;	/* [한국어] 상위 브리지. 없을 수 있다 */
	int rc;

	/* [한국어] pci_init_reset_methods() 가 채워 둔 목록이 비어 있으면
	 * 시도할 방법이 없다. 잠금을 잡기 전에 걸러 낸다. */
	if (!pci_reset_supported(dev))
		return -ENOTTY;

	/*
	 * If there's no upstream bridge, no locking is needed since there is
	 * no upstream bridge configuration to hold consistent.
	 */
	/* [한국어] 상위 브리지를 먼저 잠근다. 위에서 아래 순서를 지켜야
	 * 다른 경로와 교착하지 않는다. 루트 버스 직결 장치면 NULL 이고,
	 * 그때는 지킬 브리지 설정이 없으므로 잠글 필요도 없다. */
	bridge = pci_upstream_bridge(dev);
	if (bridge)
		pci_dev_lock(bridge);

	/* [한국어] 대상 장치를 잠근다. 이제 드라이버도 userspace 도 건드릴 수 없다. */
	pci_dev_lock(dev);
	/* [한국어] 드라이버에게 알리고, config 를 저장하고, Command 를 지워
	 * DMA 와 인터럽트를 멈춘다. */
	pci_dev_save_and_disable(dev);

	/* [한국어] 실제 리셋. 준비된 방법들을 순서대로 시도한다. */
	rc = __pci_reset_function_locked(dev);

	/* [한국어] 성공했든 실패했든 반드시 복원한다. 실패한 경우에도 저장해 둔
	 * 상태를 되돌려야 장치가 최소한 리셋 전과 같은 모습으로 남는다 —
	 * 복원을 건너뛰면 Command 가 0 인 채로 남아 장치가 완전히 죽는다. */
	pci_dev_restore(dev);
	pci_dev_unlock(dev);

	/* [한국어] 잡은 역순으로 푼다. */
	if (bridge)
		pci_dev_unlock(bridge);

	/* [한국어] __pci_reset_function_locked 의 결과가 그대로 이 함수의 결과다.
	 * 복원은 성공/실패와 무관하게 이미 마쳤다. */
	return rc;
}
EXPORT_SYMBOL_GPL(pci_reset_function);

/**
 * pci_reset_function_locked - quiesce and reset a PCI device function
 * @dev: PCI device to reset
 *
 * Some devices allow an individual function to be reset without affecting
 * other functions in the same device.  The PCI device must be responsive
 * to PCI config space in order to use this function.
 *
 * This function does not just reset the PCI portion of a device, but
 * clears all the state associated with the device.  This function differs
 * from __pci_reset_function_locked() in that it saves and restores device state
 * over the reset.  It also differs from pci_reset_function() in that it
 * requires the PCI device lock to be held.
 *
 * Context: The caller must hold the device lock.
 *
 * Return: 0 if the device function was successfully reset or negative if the
 * device doesn't support resetting a single function.
 */
/*
 * [한국어]
 * pci_reset_function_locked - 호출자가 이미 잠금을 쥔 경우의 리셋
 *
 * @dev: 리셋할 장치
 * @return: 0 = 성공, 음수 = 실패.
 *
 * pci_reset_function() 에서 잠금 획득/해제만 뺀 판이다. 저장과 복원은 그대로 한다.
 * 이름이 비슷한 세 함수의 관계를 정리하면:
 *
 *   pci_reset_function()          잠금 O, 저장/복원 O   <- 가장 완전한 판
 *   pci_reset_function_locked()   잠금 X, 저장/복원 O   <- 이 함수
 *   __pci_reset_function_locked() 잠금 X, 저장/복원 X   <- 리셋만
 *
 * 이 판이 필요한 이유: 드라이버의 probe 나 remove 안에서 리셋을 걸어야 하는
 * 경우가 있는데, 그 경로는 이미 드라이버 코어가 device lock 을 쥐고 들어온
 * 상태다. 거기서 pci_reset_function() 을 부르면 자기가 쥔 잠금을 다시
 * 잡으려다 교착한다.
 *
 * 다만 상위 브리지는 잠그지 않는다는 점에 주의해야 한다. 버스 리셋 계열이
 * 선택되면 브리지를 건드리게 되는데, 그 보호가 없다. 그래서 이 함수는
 * FLR 처럼 장치 안에서 끝나는 방법이 있는 장치에 주로 쓴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 호출자가 device lock 을 쥐고 있어야 한다.
 * 호출자: 드라이버의 probe/remove 안, 그리고 일부 오류 복구 경로.
 */
int pci_reset_function_locked(struct pci_dev *dev)
{
	int rc;

	/* [한국어] 이 함수의 전제 조건. 호출자가 잠금을 안 쥐고 부르면
	 * 리셋 도중 드라이버가 언바인딩될 수 있다. */
	device_lock_assert(&dev->dev);

	/* [한국어] 쓸 수 있는 리셋 방법이 하나도 없으면 시작하지 않는다.
	 * 저장/무력화를 해 놓고 실패하면 되돌리는 비용만 든다. */
	if (!pci_reset_supported(dev))
		return -ENOTTY;

	/* [한국어] 상태 저장 + 장치 무력화. */
	pci_dev_save_and_disable(dev);

	rc = __pci_reset_function_locked(dev);	/* [한국어] 실제 리셋 */

	/* [한국어] 성공/실패와 무관하게 복원한다. */
	pci_dev_restore(dev);

	return rc;
}
EXPORT_SYMBOL_GPL(pci_reset_function_locked);

/**
 * pci_try_reset_function - quiesce and reset a PCI device function
 * @dev: PCI device to reset
 *
 * Same as above, except return -EAGAIN if unable to lock device.
 */
/*
 * [한국어]
 * pci_try_reset_function - 잠금을 기다리지 않는 리셋
 *
 * @dev: 리셋할 장치
 * @return: 0 = 성공, -EAGAIN = 잠금을 얻지 못해 아무것도 하지 않았다,
 *          -ENOTTY = 쓸 수 있는 방법이 없다, 그 외 = 리셋 실패.
 *
 * pci_reset_function() 과 같지만 잠금을 기다리지 않는다. 잠금이 이미 잡혀
 * 있으면 -EAGAIN 을 돌려주고 즉시 물러난다.
 *
 * -EAGAIN 이라는 반환값이 의도를 말해 준다. "지금은 안 되니 나중에 다시
 * 시도하라" 는 뜻이지 "이 장치는 리셋할 수 없다" 가 아니다. 호출자는
 * 워크큐에 다시 넣거나 잠시 뒤 재시도하는 식으로 대응한다.
 *
 * 상위 브리지를 잠그지 않는 것은 pci_reset_function_locked() 와 같은
 * 한계다. 두 장치의 잠금을 기다리지 않고 모두 얻으려면 한쪽만 얻는
 * 어중간한 상태를 처리해야 해서 복잡해지기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(리셋 방법들이 msleep 한다).
 *   잠금 획득만 기다리지 않을 뿐, 리셋 자체는 시간이 걸린다.
 * 호출자: VFIO 의 장치 반납 경로처럼 실패해도 되는 곳.
 */
int pci_try_reset_function(struct pci_dev *dev)
{
	int rc;

	/* [한국어] 방법이 없으면 잠금을 건드릴 필요도 없다. */
	if (!pci_reset_supported(dev))
		return -ENOTTY;

	/* [한국어] 두 잠금을 기다리지 않고 얻어 본다. 실패하면 아무것도
	 * 잡지 않은 상태이므로 그냥 돌아가면 된다. */
	if (!pci_dev_trylock(dev))
		return -EAGAIN;

	pci_dev_save_and_disable(dev);		/* [한국어] 상태 저장 + 무력화 */
	rc = __pci_reset_function_locked(dev);	/* [한국어] 실제 리셋 */
	pci_dev_restore(dev);			/* [한국어] 상태 복원 */
	pci_dev_unlock(dev);			/* [한국어] trylock 이 잡은 두 잠금 해제 */

	return rc;
}
EXPORT_SYMBOL_GPL(pci_try_reset_function);

/* Do any devices on or below this bus prevent a bus reset? */
/*
 * [한국어]
 * pci_bus_resettable - 이 버스 아래 전부를 리셋해도 되는가
 *
 * @bus:    검사할 버스
 * @return: true = 버스 리셋 가능, false = 이 아래 어딘가에 리셋을 견디지
 *          못하는 장치가 있다.
 *
 * 버스 리셋은 그 버스 아래 모든 장치를 함께 리셋한다. 그중 하나라도
 * "나는 버스 리셋을 받으면 망가진다"(PCI_DEV_FLAGS_NO_BUS_RESET quirk)고
 * 표시돼 있으면 전체를 포기해야 한다. 이 함수가 그 판정을 한다.
 *
 * 재귀 구조인 것이 요점이다. 하위에 브리지가 있으면 그 아래까지 전부
 * 확인해야 한다 — 버스 리셋의 효과가 계층 전체로 퍼지기 때문이다.
 * 깊이가 깊어도 PCIe 계층은 실제로 몇 단계뿐이라 스택 사용이 문제되지 않는다.
 *
 * 브리지 자신도 검사한다는 점에 주의. bus->self 는 이 버스를 만든 상위
 * 브리지이고, 버스 리셋은 그 브리지의 레지스터를 통해 걸리므로 브리지가
 * 리셋을 거부하면 애초에 방법이 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 목록 순회에 pci_bus_sem 이 필요하지만
 *   이 함수는 잡지 않는다 — 호출자(pci_probe_reset_bus 등)가 쥐고 들어온다.
 * 호출자: pci_probe_reset_slot(), pci_probe_reset_bus().
 */
static bool pci_bus_resettable(struct pci_bus *bus)
{
	struct pci_dev *dev;	/* [한국어] 순회 커서 */


	/* [한국어] 이 버스를 만든 상위 브리지가 버스 리셋을 거부하면 끝이다.
	 * 실제 리셋이 그 브리지의 Bridge Control 레지스터로 걸리기 때문이다.
	 * 루트 버스면 bus->self 가 NULL 이라 이 검사를 건너뛴다. */
	if (bus->self && (bus->self->dev_flags & PCI_DEV_FLAGS_NO_BUS_RESET))
		return false;

	/* [한국어] 이 버스의 모든 장치를 확인한다. */
	list_for_each_entry(dev, &bus->devices, bus_list) {
		/* [한국어] 두 조건 중 하나라도 걸리면 전체가 불가다.
		 * (1) 이 장치 자신이 버스 리셋을 거부한다.
		 * (2) 이 장치가 브리지이고, 그 아래에 거부하는 장치가 있다(재귀). */
		if (dev->dev_flags & PCI_DEV_FLAGS_NO_BUS_RESET ||
		    (dev->subordinate && !pci_bus_resettable(dev->subordinate)))
			return false;
	}

	/* [한국어] 서브트리 전체가 버스 리셋을 견딘다. */
	return true;
}

/* [한국어] 전방 선언 세 개. 아래 __pci_bus_lock() 계열이 하위 버스를 만나면
 * 이 함수들을 재귀 호출하는데, 정의가 더 뒤에 있어서 미리 선언해 둔다.
 * __pci_bus_lock 과 pci_bus_lock 이 짝을 이루는 상호 재귀 구조다 —
 * pci_bus_lock(bus) 이 __pci_bus_lock(bus, NULL) 을 부르고, 그것이 하위
 * 버스에 대해 다시 pci_bus_lock 을 부른다. */
static void pci_bus_lock(struct pci_bus *bus);
static void pci_bus_unlock(struct pci_bus *bus);
static int pci_bus_trylock(struct pci_bus *bus);

/* Lock devices from the top of the tree down */
/*
 * [한국어]
 * __pci_bus_lock - 서브트리 전체를 위에서 아래로 잠근다
 *
 * @bus:  잠글 버스
 * @slot: NULL 이면 이 버스의 모든 장치, 지정하면 그 슬롯에 속한 장치만.
 *        슬롯 단위 리셋과 버스 단위 리셋이 같은 코드를 쓰기 위한 필터다.
 * @return: 없음. 전부 얻을 때까지 기다린다.
 *
 * 버스 리셋은 서브트리 전체에 영향을 주므로, 그 안의 모든 장치를 얼려 두어야
 * 한다. 하나라도 빠뜨리면 그 장치의 드라이버가 리셋 도중 하드웨어를 건드린다.
 *
 * 잠금 순서가 "위에서 아래로" 라는 점이 이 함수와 __pci_bus_unlock() 의
 * 대칭을 만든다. 브리지를 먼저 잡고, 그다음 자식들을, 자식이 브리지면
 * 재귀해서 그 아래까지. 해제는 정확히 반대 순서다. 이 규칙을 모든 경로가
 * 지켜야 두 리셋이 동시에 일어날 때 교착하지 않는다.
 *
 * slot 필터의 동작: 슬롯 리셋은 같은 버스에 있어도 다른 슬롯의 장치는
 * 건드리지 않는다. 그래서 dev->slot 이 목표와 다르면 건너뛴다.
 * 다만 브리지(bus->self)는 필터와 무관하게 항상 잠근다 — 리셋이 그 브리지의
 * 레지스터를 통해 걸리기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용(pci_dev_lock 이 잠들 수 있다).
 * 호출자: pci_bus_lock(), pci_slot_lock().
 * 짝: __pci_bus_unlock().
 *
 * 호출 체인:
 *   pci_reset_bus -> pci_bus_lock -> [__pci_bus_lock] -> pci_dev_lock (재귀)
 */
static void __pci_bus_lock(struct pci_bus *bus, struct pci_slot *slot)
{
	/* [한국어] bridge = 이 버스를 만든 상위 브리지. 루트 버스면 NULL. */
	struct pci_dev *dev, *bridge = bus->self;

	/* [한국어] 위에서 아래로 — 브리지를 먼저 잠근다. 리셋이 이 브리지의
	 * 레지스터로 걸리므로 반드시 포함해야 한다. */
	if (bridge)
		pci_dev_lock(bridge);

	list_for_each_entry(dev, &bus->devices, bus_list) {
		/* [한국어] 슬롯 필터. slot 이 지정됐는데 이 장치가 그 슬롯 소속이
		 * 아니면 건드리지 않는다. slot 이 NULL 이면 이 조건이 항상 거짓이라
		 * 모든 장치가 대상이 된다. */
		if (slot && (!dev->slot || dev->slot != slot))
			continue;
		/* [한국어] 이 장치가 브리지면 그 아래 전체를 재귀적으로 잠근다.
		 * pci_bus_lock 이 다시 __pci_bus_lock(sub, NULL) 을 부르므로,
		 * 하위 트리에서는 슬롯 필터가 풀린다 — 하위는 전부 대상이기 때문이다. */
		if (dev->subordinate)
			pci_bus_lock(dev->subordinate);
		else
			pci_dev_lock(dev);	/* [한국어] 잎 장치는 그냥 잠근다 */
	}
}

/* Unlock devices from the bottom of the tree up */
/*
 * [한국어]
 * __pci_bus_unlock - 서브트리 전체를 아래에서 위로 푼다
 *
 * @bus:  풀 버스
 * @slot: NULL 이면 전부, 지정하면 그 슬롯 소속만. 잠글 때와 같은 값이어야 한다.
 * @return: 없음.
 *
 * __pci_bus_lock() 의 정확한 역순이다. 자식들을 먼저 풀고, 브리지를 마지막에
 * 푼다. 이 순서를 지켜야 잠금 획득 순서와 해제 순서가 서로의 거울이 되어
 * lockdep 이 순환을 오탐하지 않는다.
 *
 * 브리지를 마지막에 푸는 데는 실질적 이유도 있다. 브리지 잠금이 먼저 풀리면
 * 그 순간 다른 코드가 브리지를 잡고 하위 버스 설정을 바꿀 수 있는데,
 * 아직 자식들이 잠긴 채라 어중간한 상태가 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용.
 * 호출자: pci_bus_unlock(), pci_slot_unlock().
 */
static void __pci_bus_unlock(struct pci_bus *bus, struct pci_slot *slot)
{
	struct pci_dev *dev, *bridge = bus->self;

	/* [한국어] 아래에서 위로 — 자식들을 먼저 푼다. */
	list_for_each_entry(dev, &bus->devices, bus_list) {
		/* [한국어] 잠글 때와 동일한 필터를 써야 짝이 맞는다. */
		if (slot && (!dev->slot || dev->slot != slot))
			continue;
		if (dev->subordinate)
			pci_bus_unlock(dev->subordinate);	/* [한국어] 하위 트리 재귀 해제 */
		else
			pci_dev_unlock(dev);
	}

	/* [한국어] 브리지는 맨 마지막. 잠글 때 처음이었던 것의 역순이다. */
	if (bridge)
		pci_dev_unlock(bridge);
}

/* Return 1 on successful lock, 0 on contention */
/*
 * [한국어]
 * __pci_bus_trylock - 서브트리 전체를 기다리지 않고 잠근다 (전부 아니면 전무)
 *
 * @bus:  잠글 버스
 * @slot: NULL 이면 전부, 지정하면 그 슬롯 소속만.
 * @return: 1 = 서브트리 전체를 잠갔다, 0 = 하나라도 실패해 아무것도 잡지 않았다.
 *
 * 이 함수의 어려움은 "전부 아니면 전무" 를 보장하는 데 있다. 장치가 수십 개인
 * 서브트리에서 마지막 하나를 못 얻으면, 앞서 얻은 것을 전부 정확히 되돌려
 * 놓아야 한다. 하나라도 남기면 그 잠금은 영원히 풀리지 않는다.
 *
 * 되돌리기의 핵심이 list_for_each_entry_continue_reverse 다. 이 매크로는
 * "현재 dev 위치에서 뒤로(역방향) 계속" 순회한다. 실패한 그 장치는 건너뛰고
 * (아직 잠그지 못했으니 풀 것도 없다) 그 앞의 것들만 역순으로 푼다.
 * 보통의 for 루프로는 "어디까지 잠갔는지" 를 따로 기억해야 하는데,
 * 이 매크로가 루프 변수 dev 를 그대로 활용해 그 기억을 대신한다.
 *
 * 역순으로 푸는 이유는 __pci_bus_unlock() 과 같다 — 잠금 순서의 거울이어야
 * 한다. 브리지를 맨 마지막에 푸는 것도 마찬가지다.
 *
 * 반환값이 1=성공, 0=실패로 커널 trylock 관례를 따른다.
 *
 * 실행 컨텍스트: 잠들지 않는다.
 * 호출자: pci_bus_trylock(), pci_slot_trylock().
 */
static int __pci_bus_trylock(struct pci_bus *bus, struct pci_slot *slot)
{
	struct pci_dev *dev, *bridge = bus->self;

	/* [한국어] 브리지부터. 여기서 실패하면 아직 아무것도 안 잡았으므로
	 * 되돌릴 것 없이 그냥 0 을 돌려준다. */
	if (bridge && !pci_dev_trylock(bridge))
		return 0;

	/* [한국어] 이 버스의 장치들을 차례로 잠근다. 하나라도 실패하면
	 * unlock 레이블로 가서 여기까지 잠근 것을 되돌린다. */
	list_for_each_entry(dev, &bus->devices, bus_list) {
		if (slot && (!dev->slot || dev->slot != slot))
			continue;
		if (dev->subordinate) {
			/* [한국어] 하위 트리를 통째로 시도한다. 그 안에서 실패하면
			 * 하위 함수가 자기 몫을 이미 되돌려 놓고 0 을 준다. */
			if (!pci_bus_trylock(dev->subordinate))
				goto unlock;
		} else if (!pci_dev_trylock(dev))
			goto unlock;	/* [한국어] 잎 장치 실패 */
	}
	return 1;	/* [한국어] 서브트리 전체 획득 성공 */

unlock:
	/* [한국어] 실패 지점(dev)에서 뒤로 되짚어 가며, 앞서 잠근 것들만 푼다.
	 * _continue_reverse 는 dev 자신은 건드리지 않고 그 이전 항목부터
	 * 역방향으로 순회한다 — 실패한 장치는 잠그지 못했으니 풀면 안 되고,
	 * 그 앞의 것들은 전부 잠겨 있으므로 정확히 맞아떨어진다. */
	list_for_each_entry_continue_reverse(dev, &bus->devices, bus_list) {
		/* [한국어] 잠글 때와 같은 필터를 써야 짝이 맞는다. */
		if (slot && (!dev->slot || dev->slot != slot))
			continue;
		if (dev->subordinate)
			pci_bus_unlock(dev->subordinate);
		else
			pci_dev_unlock(dev);
	}

	/* [한국어] 마지막으로 브리지. 잠글 때 처음이었으므로 풀 때는 마지막이다. */
	if (bridge)
		pci_dev_unlock(bridge);
	return 0;	/* [한국어] 실패. 잠금을 하나도 쥐지 않은 상태로 돌아간다 */
}

/* Lock devices from the top of the tree down */
/*
 * [한국어]
 * pci_bus_lock - 버스 서브트리 전체를 잠근다
 *
 * @bus: 잠글 버스
 * @return: 없음. 전부 얻을 때까지 기다린다.
 *
 * __pci_bus_lock(bus, NULL) 을 부르는 얇은 래퍼다. slot 에 NULL 을 넘겨
 * "이 버스의 모든 장치" 를 대상으로 삼는다.
 *
 * 래퍼를 따로 두는 이유는 __pci_bus_lock 이 하위 버스에 대해 이 함수를
 * 재귀 호출하기 때문이다. 재귀 지점에서는 슬롯 필터가 풀려 있어야 하므로
 * (하위 트리는 통째로 대상이다), NULL 을 고정한 판을 만들어 두면
 * 재귀할 때마다 실수할 여지가 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용.
 * 호출자: pci_bus_reset(), 그리고 __pci_bus_lock 자신(재귀).
 */
static void pci_bus_lock(struct pci_bus *bus)
{
	/* [한국어] slot=NULL — 이 버스의 모든 장치가 대상이다. */
	__pci_bus_lock(bus, NULL);
}

/* Unlock devices from the bottom of the tree up */
/*
 * [한국어]
 * pci_bus_unlock - 버스 서브트리 전체를 푼다
 *
 * @bus: 풀 버스
 * @return: 없음.
 *
 * pci_bus_lock() 의 짝. 잠글 때와 같은 필터(NULL)를 넘겨야 짝이 맞는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용.
 * 호출자: pci_bus_reset(), __pci_bus_unlock 과 __pci_bus_trylock 의
 *   되돌리기 경로(재귀).
 */
static void pci_bus_unlock(struct pci_bus *bus)
{
	/* [한국어] 잠글 때와 동일하게 slot=NULL. */
	__pci_bus_unlock(bus, NULL);
}

/* Return 1 on successful lock, 0 on contention */
/*
 * [한국어]
 * pci_bus_trylock - 버스 서브트리 전체를 기다리지 않고 잠근다
 *
 * @bus: 잠글 버스
 * @return: 1 = 전부 획득, 0 = 하나라도 실패(아무것도 잡지 않은 상태).
 *
 * __pci_bus_trylock(bus, NULL) 의 래퍼. "전부 아니면 전무" 규약을 그대로
 * 이어받으므로, 0 을 받으면 unlock 을 부르면 안 된다.
 *
 * 실행 컨텍스트: 잠들지 않는다.
 * 호출자: pci_try_reset_bus() 경로, __pci_bus_trylock 자신(재귀).
 */
static int pci_bus_trylock(struct pci_bus *bus)
{
	/* [한국어] slot=NULL — 버스 전체 대상. */
	return __pci_bus_trylock(bus, NULL);
}

/* Do any devices on or below this slot prevent a bus reset? */
/*
 * [한국어]
 * pci_slot_resettable - 이 슬롯을 리셋해도 되는가
 *
 * @slot:   검사할 슬롯
 * @return: true = 슬롯 리셋 가능, false = 이 슬롯 안에 리셋을 견디지 못하는
 *          장치가 있다.
 *
 * pci_bus_resettable() 의 슬롯 판이다. 차이는 검사 범위뿐 — 이쪽은 같은
 * 버스에 있어도 다른 슬롯의 장치는 건너뛴다. 슬롯 리셋이 그 슬롯에만
 * 영향을 주기 때문이다.
 *
 * 브리지 검사는 그대로 남아 있다. 슬롯 리셋도 결국 상위 브리지를 통해
 * 걸리므로, 그 브리지가 리셋을 거부하면 방법이 없다.
 *
 * 하위 트리는 pci_bus_resettable() 로 넘긴다 — 슬롯 안의 장치가 브리지라면
 * 그 아래는 전부 이 슬롯에 속하므로 필터 없이 전체를 검사하는 것이 맞다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 호출자가 pci_bus_sem 을 쥐고 들어온다.
 * 호출자: pci_probe_reset_slot().
 */
static bool pci_slot_resettable(struct pci_slot *slot)
{
	/* [한국어] 슬롯이 속한 버스의 상위 브리지. */
	struct pci_dev *dev, *bridge = slot->bus->self;

	/* [한국어] 브리지가 버스 리셋을 거부하면 슬롯 리셋도 불가능하다. */
	if (bridge && (bridge->dev_flags & PCI_DEV_FLAGS_NO_BUS_RESET))
		return false;

	list_for_each_entry(dev, &slot->bus->devices, bus_list) {
		/* [한국어] 이 슬롯 소속이 아닌 장치는 리셋 대상이 아니므로 검사하지 않는다.
		 * pci_bus_resettable() 과 이 함수의 유일한 차이가 이 필터다. */
		if (!dev->slot || dev->slot != slot)
			continue;
		/* [한국어] 이 장치가 거부하거나, 이 장치 아래 서브트리에 거부하는
		 * 장치가 있으면 전체가 불가다. 하위는 전부 이 슬롯에 속하므로
		 * 필터 없는 pci_bus_resettable 을 쓴다. */
		if (dev->dev_flags & PCI_DEV_FLAGS_NO_BUS_RESET ||
		    (dev->subordinate && !pci_bus_resettable(dev->subordinate)))
			return false;
	}

	return true;
}

/* Lock devices from the top of the tree down */
/*
 * [한국어]
 * pci_slot_lock - 이 슬롯에 속한 장치들만 잠근다
 *
 * @slot: 잠글 슬롯
 * @return: 없음.
 *
 * __pci_bus_lock 에 slot 을 필터로 넘긴다. 같은 버스에 다른 슬롯의 장치가
 * 있어도 그것들은 건드리지 않는다 — 슬롯 리셋의 영향 범위가 그 슬롯뿐이므로
 * 불필요하게 남의 장치를 얼릴 이유가 없다.
 *
 * 다만 상위 브리지는 필터와 무관하게 잠긴다(__pci_bus_lock 의 동작).
 * 리셋이 그 브리지를 통해 걸리기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용.
 * 호출자: pci_slot_reset().
 */
static void pci_slot_lock(struct pci_slot *slot)
{
	/* [한국어] slot 을 필터로 넘겨 이 슬롯 소속만 대상으로 삼는다. */
	__pci_bus_lock(slot->bus, slot);
}

/* Unlock devices from the bottom of the tree up */
/*
 * [한국어]
 * pci_slot_unlock - pci_slot_lock() 이 잠근 것을 푼다
 *
 * @slot: 풀 슬롯
 * @return: 없음.
 *
 * 잠글 때와 같은 슬롯 필터를 넘겨야 정확히 같은 집합이 풀린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용.
 * 호출자: pci_slot_reset().
 */
static void pci_slot_unlock(struct pci_slot *slot)
{
	/* [한국어] 잠글 때와 동일한 필터. */
	__pci_bus_unlock(slot->bus, slot);
}

/* Return 1 on successful lock, 0 on contention */
/*
 * [한국어]
 * pci_slot_trylock - 이 슬롯의 장치들을 기다리지 않고 잠근다
 *
 * @slot: 잠글 슬롯
 * @return: 1 = 전부 획득, 0 = 하나라도 실패(아무것도 잡지 않았다).
 *
 * "전부 아니면 전무" 규약은 __pci_bus_trylock 과 같다.
 *
 * 실행 컨텍스트: 잠들지 않는다.
 * 호출자: pci_try_reset_slot().
 */
static int pci_slot_trylock(struct pci_slot *slot)
{
	/* [한국어] slot 필터를 넘긴 trylock. */
	return __pci_bus_trylock(slot->bus, slot);
}

/*
 * Save and disable devices from the top of the tree down while holding
 * the @dev mutex lock for the entire tree.
 */
/*
 * [한국어]
 * pci_bus_save_and_disable_locked - 서브트리의 모든 장치를 저장하고 무력화한다
 *
 * @bus: 대상 버스
 * @return: 없음.
 *
 * pci_dev_save_and_disable() 을 서브트리 전체에 재귀로 적용한다.
 * 버스 리셋은 그 아래 전부를 리셋하므로, 전부의 config 를 저장해 두고
 * 전부의 DMA 를 멈춰야 한다. 하나라도 빠뜨리면 그 장치가 리셋 도중
 * DMA 를 계속해 메모리를 오염시키거나, 리셋 후 복원되지 않아 죽는다.
 *
 * 함수 이름 끝의 _locked 가 전제를 말한다 — 호출자가 이미 서브트리 전체를
 * pci_bus_lock() 으로 잠가 둔 상태여야 한다. pci_dev_save_and_disable() 안의
 * device_lock_assert() 가 각 장치마다 그것을 확인한다.
 *
 * 순회 순서가 위에서 아래인 것도 의도적이다. 부모를 먼저 무력화하면
 * 그 아래로 가는 트래픽이 먼저 끊기므로, 자식들이 무력화되는 사이에
 * 새 요청이 들어올 여지가 줄어든다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용.
 * 호출자: pci_bus_reset() 계열.
 * 짝: pci_bus_restore_locked().
 */
static void pci_bus_save_and_disable_locked(struct pci_bus *bus)
{
	struct pci_dev *dev;	/* [한국어] 순회 커서 */

	list_for_each_entry(dev, &bus->devices, bus_list) {
		/* [한국어] 이 장치의 config 를 저장하고 Command 를 지운다. */
		pci_dev_save_and_disable(dev);
		/* [한국어] 브리지면 그 아래 전체에 대해 같은 일을 반복한다.
		 * 부모를 먼저 처리한 뒤 자식으로 내려가는 위에서 아래 순서다. */
		if (dev->subordinate)
			pci_bus_save_and_disable_locked(dev->subordinate);
	}
}

/*
 * Restore devices from top of the tree down while holding @dev mutex lock
 * for the entire tree.  Parent bridges need to be restored before we can
 * get to subordinate devices.
 */
/*
 * [한국어]
 * pci_bus_restore_locked - 서브트리의 모든 장치를 위에서 아래로 복원한다
 *
 * @bus: 대상 버스
 * @return: 없음.
 *
 * pci_bus_save_and_disable_locked() 의 짝이다. 저장 쪽과 달리 이쪽에는
 * 반드시 위에서 아래여야 하는 강한 이유가 있고, 원문 주석이 그것을 밝힌다 —
 * 하위 장치에 닿으려면 그 앞의 부모 브리지들이 먼저 복원되어 있어야 한다는 것이다.
 *
 * 왜 그런가. 브리지의 config 에는 그 아래 버스로 어떤 주소 범위를 통과시킬지
 * 정하는 윈도우 레지스터(Memory Base/Limit 등)와 버스 번호가 들어 있다.
 * 리셋으로 그 값이 날아간 상태에서 하위 장치에 config 접근을 시도하면,
 * 브리지가 그 요청을 어디로 보낼지 몰라 응답이 오지 않는다. 그래서 부모를
 * 먼저 되살려 길을 열어 두어야 자식에게 닿을 수 있다.
 *
 * 그리고 재귀로 내려가기 직전에 pci_bridge_wait_for_secondary_bus() 를
 * 부르는 것도 같은 맥락이다. 브리지 설정을 복원했다고 해서 하위 링크가
 * 곧바로 살아나는 것은 아니므로, 링크가 올라오고 하위 장치가 응답할
 * 준비를 마칠 때까지 기다린 뒤에 내려간다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용(대기가 있다). 호출자가 서브트리
 *   전체를 잠가 둔 상태여야 한다.
 * 호출자: pci_bus_reset() 계열, pci_slot_restore_locked().
 *
 * 호출 체인:
 *   pci_try_reset_bus -> [pci_bus_restore_locked]
 *     -> pci_dev_restore -> pci_bridge_wait_for_secondary_bus -> (재귀)
 */
static void pci_bus_restore_locked(struct pci_bus *bus)
{
	struct pci_dev *dev;	/* [한국어] 순회 커서 */

	list_for_each_entry(dev, &bus->devices, bus_list) {
		/* [한국어] 이 장치의 config 를 되돌리고 드라이버에게 알린다. */
		pci_dev_restore(dev);
		if (dev->subordinate) {
			/* [한국어] 브리지를 복원했으니 이제 그 아래 링크가 살아나기를
			 * 기다린다. 이 대기 없이 곧바로 재귀하면 하위 장치의 config
			 * 접근이 all-ones 로 돌아와 복원이 전부 실패한다. */
			pci_bridge_wait_for_secondary_bus(dev, "bus reset");
			/* [한국어] 길이 열렸으니 아래로 내려간다. */
			pci_bus_restore_locked(dev->subordinate);
		}
	}
}

/*
 * Save and disable devices from the top of the tree down while holding
 * the @dev mutex lock for the entire tree.
 */
/*
 * [한국어]
 * pci_slot_save_and_disable_locked - 이 슬롯의 장치들을 저장하고 무력화한다
 *
 * @slot: 대상 슬롯
 * @return: 없음.
 *
 * pci_bus_save_and_disable_locked() 의 슬롯 판. 차이는 슬롯 필터뿐이다 —
 * 같은 버스에 있어도 다른 슬롯의 장치는 건드리지 않는다. 슬롯 리셋이
 * 그 슬롯에만 영향을 주기 때문이다.
 *
 * 하위 트리로 내려갈 때는 필터 없는 버스 판을 쓴다. 슬롯 안의 장치가
 * 브리지라면 그 아래는 전부 이 슬롯에 딸린 것이므로 통째로 대상이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 호출자가 pci_slot_lock() 으로
 *   해당 장치들을 잠가 둔 상태여야 한다.
 * 호출자: pci_try_reset_slot().
 * 짝: pci_slot_restore_locked().
 */
static void pci_slot_save_and_disable_locked(struct pci_slot *slot)
{
	struct pci_dev *dev;	/* [한국어] 순회 커서 */

	list_for_each_entry(dev, &slot->bus->devices, bus_list) {
		/* [한국어] 이 슬롯 소속이 아닌 장치는 리셋 대상이 아니므로 건너뛴다. */
		if (!dev->slot || dev->slot != slot)
			continue;
		pci_dev_save_and_disable(dev);
		/* [한국어] 하위 트리는 전부 이 슬롯에 딸린 것이므로 필터 없이 전체 처리. */
		if (dev->subordinate)
			pci_bus_save_and_disable_locked(dev->subordinate);
	}
}

/*
 * Restore devices from top of the tree down while holding @dev mutex lock
 * for the entire tree.  Parent bridges need to be restored before we can
 * get to subordinate devices.
 */
/*
 * [한국어]
 * pci_slot_restore_locked - 이 슬롯의 장치들을 위에서 아래로 복원한다
 *
 * @slot: 대상 슬롯
 * @return: 없음.
 *
 * pci_bus_restore_locked() 의 슬롯 판. 필터가 추가된 것 외에는 동일하며,
 * "부모 브리지를 먼저 복원해야 자식에게 닿을 수 있다" 는 순서 제약도 같다.
 *
 * pci_bridge_wait_for_secondary_bus() 에 넘기는 문자열만 "slot reset" 으로
 * 다르다. 로그에 어떤 종류의 리셋이었는지 남기기 위한 것이며 동작에는
 * 영향이 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용(대기 있음).
 * 호출자: pci_try_reset_slot().
 */
static void pci_slot_restore_locked(struct pci_slot *slot)
{
	struct pci_dev *dev;	/* [한국어] 순회 커서 */

	list_for_each_entry(dev, &slot->bus->devices, bus_list) {
		/* [한국어] 저장할 때와 같은 필터. 짝이 맞아야 한다. */
		if (!dev->slot || dev->slot != slot)
			continue;
		pci_dev_restore(dev);
		if (dev->subordinate) {
			/* [한국어] 브리지 복원 후 하위 링크가 살아나기를 기다린 뒤 내려간다. */
			pci_bridge_wait_for_secondary_bus(dev, "slot reset");
			pci_bus_restore_locked(dev->subordinate);
		}
	}
}

/*
 * [한국어]
 * pci_slot_reset - 슬롯을 잠그고 핫플러그 컨트롤러에게 리셋을 시킨다
 *
 * @slot:  리셋할 슬롯
 * @probe: true 면 가능 여부만 확인(잠그지 않는다), false 면 실제 리셋.
 * @return: 0 = 가능/성공, -ENOTTY = 슬롯이 없거나 리셋할 수 없음.
 *
 * probe 값에 따라 잠금 여부가 달라지는 것이 이 함수의 특징이다.
 * 가능 여부만 묻는 경우에는 하드웨어를 건드리지 않으므로 잠글 필요가 없고,
 * 오히려 잠그면 불필요하게 다른 작업을 막는다.
 *
 * 주의: 이 함수는 저장/복원을 하지 않는다. probe 용도로 쓰이거나
 * (pci_probe_reset_slot), 상위에서 저장/복원을 직접 처리하는 경우에만
 * 쓴다. 실제 리셋에 저장/복원까지 필요한 경로는 pci_try_reset_slot() 이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용(might_sleep).
 * 호출자: pci_probe_reset_slot(), pci_reset_bus 계열.
 */
static int pci_slot_reset(struct pci_slot *slot, bool probe)
{
	int rc;

	/* [한국어] 슬롯 자체가 없거나, 그 안에 리셋을 거부하는 장치가 있으면 불가. */
	if (!slot || !pci_slot_resettable(slot))
		return -ENOTTY;

	/* [한국어] 실제 리셋일 때만 잠근다. probe 는 읽기만 하므로 필요 없다. */
	if (!probe)
		pci_slot_lock(slot);

	/* [한국어] 핫플러그 드라이버의 reset_slot 콜백이 잠들 수 있다. */
	might_sleep();

	/* [한국어] 실제 동작은 핫플러그 컨트롤러에게 위임한다. probe 값을
	 * 그대로 넘겨 "가능한지만 답하라" 인지도 전달한다. */
	rc = pci_reset_hotplug_slot(slot->hotplug, probe);

	/* [한국어] 잠갔던 경우에만 푼다. */
	if (!probe)
		pci_slot_unlock(slot);

	/* [한국어] 핫플러그 컨트롤러가 준 결과를 그대로 올려 보낸다.
	 * probe 였다면 "가능 여부", 아니었다면 "리셋 성공 여부"다. */
	return rc;
}

/**
 * pci_probe_reset_slot - probe whether a PCI slot can be reset
 * @slot: PCI slot to probe
 *
 * Return 0 if slot can be reset, negative if a slot reset is not supported.
 */
/*
 * [한국어]
 * pci_probe_reset_slot - 이 슬롯을 리셋할 수 있는지만 확인한다
 *
 * @slot:   확인할 슬롯
 * @return: 0 = 리셋 가능, 음수 = 불가능.
 *
 * pci_slot_reset(slot, PCI_RESET_PROBE) 의 래퍼. 하드웨어를 전혀 건드리지
 * 않고 조건만 확인한다.
 *
 * VFIO 처럼 장치를 사용자 공간에 넘기기 전에 "이 장치를 안전하게 되돌릴
 * 수단이 있는가" 를 확인해야 하는 코드가 쓴다. 되돌릴 방법이 없으면
 * 장치를 넘겨줘서는 안 되기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: VFIO, 그리고 리셋 가능 여부를 미리 알아야 하는 코드.
 */
int pci_probe_reset_slot(struct pci_slot *slot)
{
	/* [한국어] PCI_RESET_PROBE(=true) — 확인만 하고 실제로는 리셋하지 않는다. */
	return pci_slot_reset(slot, PCI_RESET_PROBE);
}
EXPORT_SYMBOL_GPL(pci_probe_reset_slot);

/**
 * pci_try_reset_slot - Try to reset a PCI slot
 * @slot: PCI slot to reset
 *
 * A PCI bus may host multiple slots, each slot may support a reset mechanism
 * independent of other slots.  For instance, some slots may support slot power
 * control.  In the case of a 1:1 bus to slot architecture, this function may
 * wrap the bus reset to avoid spurious slot related events such as hotplug.
 * Generally a slot reset should be attempted before a bus reset.  All of the
 * function of the slot and any subordinate buses behind the slot are reset
 * through this function.  PCI config space of all devices in the slot and
 * behind the slot is saved before and restored after reset.
 *
 * Same as above except return -EAGAIN if the slot cannot be locked
 */
/*
 * [한국어]
 * pci_try_reset_slot - 슬롯 전체를 저장 -> 리셋 -> 복원한다 (기다리지 않는 판)
 *
 * @slot:   리셋할 슬롯
 * @return: 0 = 성공, -EAGAIN = 잠금을 얻지 못함, 그 외 음수 = 불가능하거나 실패.
 *
 * 슬롯 리셋의 완전판이다. 위 원문 주석이 설명하듯, 이 함수는 슬롯 안의
 * 모든 장치와 그 아래 서브트리 전체를 대상으로 config 저장과 복원까지
 * 책임진다.
 *
 * 순서:
 *   1) probe 로 가능한지 먼저 확인 - 불가능하면 잠금조차 시도하지 않는다.
 *   2) 슬롯 전체를 trylock - 실패하면 -EAGAIN.
 *   3) 저장 + 무력화 -> 실제 리셋 -> 복원 -> 잠금 해제.
 *
 * 1단계를 따로 두는 이유: 잠금은 비싸고 다른 작업을 막는다. 어차피 안 될
 * 일이라면 잠그기 전에 알아내는 편이 낫다.
 *
 * 원문 주석이 밝히는 정책도 중요하다 — "Generally a slot reset should be
 * attempted before a bus reset." 슬롯 리셋이 버스 리셋보다 영향 범위가
 * 좁으므로 먼저 시도한다. 1:1 버스-슬롯 구조에서는 이 함수가 사실상
 * 버스 리셋을 감싸는 셈인데, 그렇게 하면 hotplug 관련 가짜 이벤트가
 * 생기지 않는다는 이점이 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용(리셋과 복원이 잠든다).
 *   잠금만 기다리지 않을 뿐이다.
 * 호출자: pci_reset_bus() 경로.
 */
static int pci_try_reset_slot(struct pci_slot *slot)
{
	int rc;

	/* [한국어] 1단계 — 가능 여부 확인. 여기서 실패하면 잠금을 건드리지 않는다. */
	rc = pci_slot_reset(slot, PCI_RESET_PROBE);
	if (rc)
		return rc;

	/* [한국어] 2단계 — 슬롯 전체를 기다리지 않고 잠근다. */
	if (pci_slot_trylock(slot)) {
		/* [한국어] 3단계 — 서브트리 전체의 config 저장 + DMA 차단. */
		pci_slot_save_and_disable_locked(slot);
		might_sleep();
		/* [한국어] PCI_RESET_DO_RESET(=false) — 이번에는 실제로 리셋한다. */
		rc = pci_reset_hotplug_slot(slot->hotplug, PCI_RESET_DO_RESET);
		/* [한국어] 위에서 아래로 복원. 브리지를 먼저 되살려야 자식에게 닿는다. */
		pci_slot_restore_locked(slot);
		pci_slot_unlock(slot);
	} else
		/* [한국어] 잠금 실패. 아무것도 하지 않았으므로 되돌릴 것도 없다.
		 * -EAGAIN 은 "지금은 안 되니 나중에" 라는 뜻이다. */
		rc = -EAGAIN;

	return rc;
}

/*
 * [한국어]
 * pci_bus_reset - 버스를 잠그고 상위 브리지로 하위 버스를 리셋한다
 *
 * @bus:   리셋할 버스
 * @probe: true 면 가능 여부만 확인, false 면 실제 리셋.
 * @return: 0 = 가능/성공, -ENOTTY = 루트 버스이거나 리셋할 수 없음.
 *
 * pci_slot_reset() 의 버스 판이다. 슬롯 리셋이 핫플러그 컨트롤러에게
 * 위임했다면, 이쪽은 상위 브리지의 Secondary Bus Reset 비트를 직접 쓴다.
 *
 * 루트 버스에는 쓸 수 없다(!bus->self). 루트 버스를 리셋한다는 것은
 * 호스트 브리지를 리셋한다는 뜻이고, 그 위에 CPU 가 붙어 있으므로
 * 애초에 성립하지 않는다.
 *
 * 이 함수도 저장/복원을 하지 않는다. probe 용이거나 상위가 직접 처리하는
 * 경우에 쓴다. 완전판은 pci_reset_bus() / pci_try_reset_bus() 다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용.
 * 호출자: pci_probe_reset_bus(), pci_reset_bus() 경로.
 */
static int pci_bus_reset(struct pci_bus *bus, bool probe)
{
	int ret;

	/* [한국어] 루트 버스이거나(리셋을 걸 브리지가 없다), 서브트리에
	 * 리셋을 거부하는 장치가 있으면 불가능하다. */
	if (!bus->self || !pci_bus_resettable(bus))
		return -ENOTTY;

	/* [한국어] 가능 여부만 묻는 호출이었다면 여기까지가 답이다.
	 * pci_slot_reset 과 달리 아예 일찍 빠져나간다 — 아래에 확인만으로
	 * 끝낼 수 있는 부분이 없기 때문이다. */
	if (probe)
		return 0;

	/* [한국어] 서브트리 전체를 잠근다. 브리지부터 위에서 아래로. */
	pci_bus_lock(bus);

	might_sleep();

	/* [한국어] 상위 브리지의 SBR 비트를 눌렀다 떼고, 하위가 다시
	 * 응답할 때까지 기다린다. */
	ret = pci_bridge_secondary_bus_reset(bus->self);

	/* [한국어] 아래에서 위로 해제. */
	pci_bus_unlock(bus);

	return ret;
}

/**
 * pci_try_reset_bus - Try to reset a PCI bus
 * @bus: top level PCI bus to reset
 *
 * Same as above except return -EAGAIN if the bus cannot be locked
 */
/*
 * [한국어]
 * pci_try_reset_bus - 버스 전체를 저장 -> 리셋 -> 복원한다 (기다리지 않는 판)
 *
 * @bus:    리셋할 버스
 * @return: 0 = 성공, -EAGAIN = 잠금을 얻지 못함, 그 외 음수 = 불가능하거나 실패.
 *
 * pci_try_reset_slot() 의 버스 판이다. 구조가 완전히 같다 —
 * probe 로 먼저 확인하고, trylock 으로 서브트리를 잠그고,
 * 저장 -> 리셋 -> 복원 -> 해제.
 *
 * 다른 점은 리셋 수단뿐이다. 슬롯 판은 핫플러그 컨트롤러에게 위임하지만,
 * 이쪽은 상위 브리지의 Secondary Bus Reset 을 직접 쓴다.
 *
 * 복원 단계가 특히 무겁다. pci_bus_restore_locked() 가 서브트리를 위에서
 * 아래로 돌면서 브리지마다 하위 링크가 살아나기를 기다리므로, 장치가
 * 많으면 수백 밀리초가 걸릴 수 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용. 잠금만 기다리지 않는다.
 * 호출자: pci_reset_bus(), pci_reset_bridge() 의 fallback 경로.
 */
static int pci_try_reset_bus(struct pci_bus *bus)
{
	int rc;

	/* [한국어] 1단계 — 가능 여부 확인. 실패하면 잠금을 건드리지 않는다. */
	rc = pci_bus_reset(bus, PCI_RESET_PROBE);
	if (rc)
		return rc;

	/* [한국어] 2단계 — 서브트리 전체를 기다리지 않고 잠근다. */
	if (pci_bus_trylock(bus)) {
		/* [한국어] 3단계 — 전체 config 저장 + DMA 차단. */
		pci_bus_save_and_disable_locked(bus);
		might_sleep();
		/* [한국어] 상위 브리지의 SBR 로 실제 리셋. */
		rc = pci_bridge_secondary_bus_reset(bus->self);
		/* [한국어] 위에서 아래로 복원. 브리지마다 링크 복구를 기다린다. */
		pci_bus_restore_locked(bus);
		pci_bus_unlock(bus);
	} else
		/* [한국어] 잠금 실패. 아무것도 하지 않았다. */
		rc = -EAGAIN;

	return rc;
}

/* [한국어] pci_reset_bridge() 의 restore 인자에 넘길 이름 붙인 상수.
 * bool 인자를 true/false 로 직접 넘기면 호출부만 보고는 무슨 뜻인지 알 수 없다.
 * pci_reset_bridge(bridge, true) 보다 pci_reset_bridge(bridge, PCI_RESET_RESTORE)
 * 가 읽기 쉬우므로 이렇게 이름을 붙였다.
 *   RESTORE    - 리셋 전후로 config 저장/복원까지 PCI 코어가 처리한다.
 *   NO_RESTORE - 리셋만 한다. 오류 복구 경로가 자기 절차로 복구하므로
 *                코어가 끼어들면 안 되는 경우다. */
#define PCI_RESET_RESTORE true
#define PCI_RESET_NO_RESTORE false
/**
 * pci_reset_bridge - reset a bridge's subordinate bus
 * @bridge: bridge that connects to the bus to reset
 * @restore: when true use a reset method that invokes pci_dev_restore() post
 *           reset for affected devices
 *
 * This function will first try to reset the slots on this bus if the method is
 * available. If slot reset fails or is not available, this will fall back to a
 * secondary bus reset.
 */
/*
 * [한국어]
 * pci_reset_bridge - 슬롯 리셋을 먼저 시도하고, 안 되면 버스 리셋으로 물러난다
 *
 * @bridge:  하위 버스를 가진 브리지
 * @restore: true 면 리셋 전후로 config 저장/복원까지 하는 방법을 쓴다.
 *           false 면 리셋만 하고 복원은 호출자가 알아서 한다.
 * @return: 0 = 성공, 음수 = 실패.
 *
 * 리셋 정책이 담긴 함수다. 영향 범위가 좁은 방법부터 시도한다는 원칙에 따라
 * 슬롯 리셋을 먼저 보고, 하나라도 안 되면 버스 리셋으로 내려간다.
 *
 * "하나라도 안 되면" 이라는 점이 중요하다. 이 버스에 슬롯이 여러 개 있을 때,
 * 그중 하나라도 슬롯 리셋을 지원하지 않으면 부분적으로만 리셋된 상태가 되어
 * 오히려 위험하다. 그래서 먼저 전부 확인하고(첫 번째 루프), 전부 가능할 때만
 * 실제로 실행한다(두 번째 루프). 실행 도중 실패해도 버스 리셋으로 넘어간다.
 *
 * restore 인자로 두 가지 쓰임을 구분한다.
 *   PCI_RESET_NO_RESTORE - AER/DPC 오류 복구 경로. 그쪽은 자기만의 복구
 *     절차(err_handler 콜백 시퀀스)를 갖고 있어서, 여기서 config 를 복원해
 *     버리면 그 절차와 충돌한다.
 *   PCI_RESET_RESTORE - 일반 리셋 경로. 저장/복원까지 여기서 처리한다.
 *
 * pci_slot_mutex 는 bus->slots 목록을 보호한다. 슬롯은 핫플러그로 등록/해제될
 * 수 있어서, 순회 중에 목록이 바뀌면 안 된다. 다만 실제 리셋은 시간이 오래
 * 걸리므로 bus_reset 레이블에서 먼저 풀고 나서 버스 리셋을 시작한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용.
 * 호출자: pci_bus_error_reset(), pci_try_reset_bridge().
 *
 * 호출 체인:
 *   pcie_do_recovery -> pci_bus_error_reset -> [pci_reset_bridge]
 *     -> pci_slot_reset 또는 pci_bus_reset
 */
static int pci_reset_bridge(struct pci_dev *bridge, bool restore)
{
	/* [한국어] 이 브리지가 만든 하위 버스. 리셋 대상은 이 버스 아래 전부다. */
	struct pci_bus *bus = bridge->subordinate;
	struct pci_slot *slot;	/* [한국어] 슬롯 순회 커서 */

	/* [한국어] 하위 버스가 없으면 브리지가 아니거나 아직 열거되지 않았다. */
	if (!bus)
		return -ENOTTY;

	/* [한국어] bus->slots 목록을 보호한다. 핫플러그가 슬롯을 추가/제거할 수 있다. */
	mutex_lock(&pci_slot_mutex);
	/* [한국어] 슬롯이 하나도 없으면 슬롯 리셋이라는 선택지 자체가 없다. */
	if (list_empty(&bus->slots))
		goto bus_reset;

	/* [한국어] 1차 확인 — 모든 슬롯이 리셋 가능한지 먼저 본다.
	 * 하나라도 불가능하면 부분 리셋이 되므로 전체를 포기하고 버스 리셋으로 간다. */
	list_for_each_entry(slot, &bus->slots, list)
		if (pci_probe_reset_slot(slot))
			goto bus_reset;

	/* [한국어] 2차 실행 — 확인을 통과했으니 실제로 리셋한다. */
	list_for_each_entry(slot, &bus->slots, list) {
		int ret;

		/* [한국어] restore 에 따라 저장/복원 포함 여부가 갈린다.
		 * pci_try_reset_slot 은 저장 -> 리셋 -> 복원을 모두 하고,
		 * pci_slot_reset 은 리셋만 한다. */
		if (restore)
			ret = pci_try_reset_slot(slot);
		else
			ret = pci_slot_reset(slot, PCI_RESET_DO_RESET);

		/* [한국어] 실행 중 실패도 버스 리셋으로 물러난다. 이미 리셋된
		 * 슬롯이 있더라도, 버스 리셋이 그 위에 다시 걸리므로 최종
		 * 상태는 일관된다. */
		if (ret)
			goto bus_reset;
	}

	/* [한국어] 모든 슬롯을 성공적으로 리셋했다. */
	mutex_unlock(&pci_slot_mutex);
	return 0;
bus_reset:
	/* [한국어] 버스 리셋은 오래 걸리므로 슬롯 목록 잠금을 먼저 놓는다.
	 * 이 시점 이후에는 slot 포인터를 쓰지 않으므로 안전하다. */
	mutex_unlock(&pci_slot_mutex);

	/* [한국어] 슬롯 리셋과 같은 기준으로 저장/복원 포함 여부를 정한다. */
	if (restore)
		return pci_try_reset_bus(bus);
	return pci_bus_reset(bridge->subordinate, PCI_RESET_DO_RESET);
}

/**
 * pci_bus_error_reset - reset the bridge's subordinate bus
 * @bridge: The parent device that connects to the bus to reset
 */
/*
 * [한국어]
 * pci_bus_error_reset - 오류 복구 중에 하위 버스를 리셋한다 (복원은 하지 않는다)
 *
 * @bridge: 리셋할 버스를 가진 브리지
 * @return: 0 = 성공, 음수 = 실패.
 *
 * AER 이나 DPC 가 치명적 오류를 감지했을 때 부르는 진입점이다.
 * PCI_RESET_NO_RESTORE 를 넘긴다는 점이 이 함수의 전부다.
 *
 * 왜 복원을 하지 않는가. 오류 복구는 정해진 순서로 진행된다 —
 * error_detected -> (리셋) -> slot_reset -> resume 순으로 각 드라이버의
 * 콜백이 불리고, 드라이버는 slot_reset 콜백에서 자기 하드웨어를 직접
 * 재설정한다. 그 사이에 PCI 코어가 저장해 둔 config 를 되돌려 버리면
 * 드라이버가 방금 설정한 값을 덮어써서 복구가 어긋난다.
 *
 * NVMe 관점: NVMe 컨트롤러에서 치명적 오류가 나면 이 경로를 탄다.
 * nvme_error_detected 가 먼저 불려 I/O 를 멈추고, 여기서 링크가 리셋되고,
 * nvme_slot_reset 이 컨트롤러를 다시 초기화한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(오류 복구 워커).
 * 호출자: pcie/err.c 의 pcie_do_recovery().
 */
int pci_bus_error_reset(struct pci_dev *bridge)
{
	/* [한국어] PCI_RESET_NO_RESTORE — 복원은 오류 복구 절차가 직접 한다. */
	return pci_reset_bridge(bridge, PCI_RESET_NO_RESTORE);
}

/*
 * [한국어]
 * pci_try_reset_bridge - 하위 버스를 리셋하고 config 까지 복원한다
 *
 * @bridge: 리셋할 버스를 가진 브리지
 * @return: 0 = 성공, -EAGAIN = 잠금을 얻지 못함, 그 외 음수 = 실패.
 *
 * pci_bus_error_reset() 의 짝이다. 차이는 PCI_RESET_RESTORE 를 넘긴다는
 * 것뿐 — 즉 저장/복원까지 PCI 코어가 처리한다.
 *
 * 오류 복구가 아닌 일반적인 리셋 요청에 쓴다. 드라이버가 스스로
 * 하드웨어를 재설정할 준비가 돼 있지 않은 상황이므로, 코어가 리셋 전의
 * config 를 그대로 되돌려 놓아야 장치가 계속 동작한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용.
 * 호출자: 브리지 아래를 통째로 리셋해야 하는 코드.
 */
int pci_try_reset_bridge(struct pci_dev *bridge)
{
	/* [한국어] PCI_RESET_RESTORE — 저장/복원을 포함한 방법을 쓴다. */
	return pci_reset_bridge(bridge, PCI_RESET_RESTORE);
}

/**
 * pci_probe_reset_bus - probe whether a PCI bus can be reset
 * @bus: PCI bus to probe
 *
 * Return 0 if bus can be reset, negative if a bus reset is not supported.
 */
/*
 * [한국어]
 * pci_probe_reset_bus - 이 버스를 리셋할 수 있는지만 확인한다
 *
 * @bus:    확인할 버스
 * @return: 0 = 가능, 음수 = 불가능(루트 버스이거나 거부 장치가 있다).
 *
 * pci_bus_reset(bus, PCI_RESET_PROBE) 의 래퍼. 하드웨어를 건드리지 않는다.
 * pci_probe_reset_slot() 과 같은 용도 — 장치를 사용자 공간에 넘기기 전에
 * 되돌릴 수단이 있는지 미리 확인하는 데 쓴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: VFIO.
 */
int pci_probe_reset_bus(struct pci_bus *bus)
{
	/* [한국어] PCI_RESET_PROBE(=true) — 확인만 한다. */
	return pci_bus_reset(bus, PCI_RESET_PROBE);
}
EXPORT_SYMBOL_GPL(pci_probe_reset_bus);

/**
 * pci_reset_bus - Try to reset a PCI bus
 * @pdev: top level PCI device to reset via slot/bus
 *
 * Same as above except return -EAGAIN if the bus cannot be locked
 */
/*
 * [한국어]
 * pci_reset_bus - 이 장치가 속한 슬롯이나 버스를 리셋한다
 *
 * @pdev:   기준이 되는 장치. 이 장치의 슬롯 또는 버스가 대상이 된다.
 * @return: 0 = 성공, -EAGAIN = 잠금을 얻지 못함, 그 외 음수 = 실패.
 *
 * 장치 단위 리셋(pci_reset_function)이 통하지 않을 때 쓰는 더 넓은 범위의
 * 리셋이다. 이름은 "bus" 지만 실제로는 슬롯 리셋을 먼저 시도한다 —
 * 영향 범위가 좁은 쪽이 우선이라는 이 파일의 일관된 원칙이다.
 *
 * 삼항 연산자 한 줄에 그 정책이 담겨 있다.
 *   pci_probe_reset_slot(pdev->slot) 이 0(가능)을 돌려주면 -> 슬롯 리셋
 *   그 외(불가능)이면                                      -> 버스 리셋
 *
 * pdev->slot 이 NULL 인 경우(납땜된 장치)에도 안전하다.
 * pci_probe_reset_slot -> pci_slot_reset 이 !slot 을 먼저 확인해
 * -ENOTTY 를 돌려주므로 자연스럽게 버스 리셋으로 간다.
 *
 * NVMe 관점: drivers/nvme/ 에는 이 함수 호출이 없다(전수 확인).
 * NVMe SSD 를 이 방식으로 리셋하면 같은 슬롯/버스의 다른 장치까지
 * 함께 리셋되므로, 드라이버가 스스로 쓸 만한 방법이 아니다.
 * VFIO 가 장치를 게스트에게 넘기기 전후로 부르는 것이 주된 용례다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용.
 * 호출자: VFIO, 일부 드라이버의 최후 복구 경로.
 */
int pci_reset_bus(struct pci_dev *pdev)
{
	/* [한국어] 슬롯 리셋이 가능하면 그쪽을, 아니면 버스 리셋을 쓴다.
	 * probe 가 0 을 성공으로 쓰므로 ! 로 뒤집어 조건을 만든다. */
	return (!pci_probe_reset_slot(pdev->slot)) ?
	    pci_try_reset_slot(pdev->slot) : pci_try_reset_bus(pdev->bus);
}
EXPORT_SYMBOL_GPL(pci_reset_bus);

/**
 * pcix_get_max_mmrbc - get PCI-X maximum designed memory read byte count
 * @dev: PCI device to query
 *
 * Returns mmrbc: maximum designed memory read count in bytes or
 * appropriate error value.
 */
/*
 * [한국어]
 * pcix_get_max_mmrbc - 이 PCI-X 장치가 지원하는 최대 Memory Read Byte Count
 *
 * @dev:    조회할 장치
 * @return: 바이트 단위 최대값(512/1024/2048/4096), 또는 -EINVAL.
 *
 * MMRBC(Maximum Memory Read Byte Count)는 PCI-X 에서 장치가 한 번의 읽기
 * 트랜잭션으로 요청할 수 있는 최대 바이트 수다. 값이 클수록 버스 효율이
 * 좋지만, 한 장치가 버스를 오래 점유해 다른 장치의 지연이 커진다.
 *
 * 이 함수는 "하드웨어가 지원하는 상한" 을 알려 준다. 현재 설정값은
 * pcix_get_mmrbc() 로 따로 읽고, 바꾸는 것은 pcix_set_mmrbc() 다.
 *
 * 인코딩이 특이하다. Status 레지스터의 필드에는 0~3 이 들어 있고,
 * 실제 값은 512 << n 이다. 0->512, 1->1024, 2->2048, 3->4096.
 * 그래서 512 를 왼쪽으로 필드값만큼 밀어 계산한다.
 *
 * NVMe 관점: PCIe 는 PCI-X 의 후속이지만 MMRBC 라는 개념을 그대로 쓰지
 * 않는다. PCIe 의 대응물은 Device Control 레지스터의 Max Read Request Size 이고,
 * 그것을 다루는 함수가 아래 pcie_get_readrq()/pcie_set_readrq() 다.
 * NVMe SSD 는 PCIe 장치이므로 이 함수와 무관하다.
 *
 * 실행 컨텍스트: 제약 없음(config 읽기).
 * 호출자: PCI-X 장치를 다루는 옛 드라이버들(e1000, tg3 등).
 */
int pcix_get_max_mmrbc(struct pci_dev *dev)
{
	int cap;	/* [한국어] PCI-X capability 의 config space 오프셋 */
	u32 stat;	/* [한국어] PCI-X Status 레지스터 값 */

	/* [한국어] PCI-X capability(ID 0x07)를 찾는다. PCIe 장치에는 없으므로
	 * NVMe SSD 에서는 여기서 -EINVAL 이 난다. */
	cap = pci_find_capability(dev, PCI_CAP_ID_PCIX);
	if (!cap)
		return -EINVAL;

	/* [한국어] Status 레지스터(capability + 0x04)를 dword 로 읽는다. */
	if (pci_read_config_dword(dev, cap + PCI_X_STATUS, &stat))
		return -EINVAL;

	/* [한국어] 필드값 n 을 실제 바이트 수로 변환. 512 << n 이므로
	 * n=0 이면 512, n=3 이면 4096 이다. FIELD_GET 이 마스크와 시프트를
	 * 한 번에 처리해 준다. */
	return 512 << FIELD_GET(PCI_X_STATUS_MAX_READ, stat);
}
EXPORT_SYMBOL(pcix_get_max_mmrbc);

/**
 * pcix_get_mmrbc - get PCI-X maximum memory read byte count
 * @dev: PCI device to query
 *
 * Returns mmrbc: maximum memory read count in bytes or appropriate error
 * value.
 */
/*
 * [한국어]
 * pcix_get_mmrbc - 현재 설정된 Memory Read Byte Count 를 읽는다
 *
 * @dev:    조회할 장치
 * @return: 바이트 단위 현재값, 또는 -EINVAL.
 *
 * 위 pcix_get_max_mmrbc() 가 하드웨어 상한을 알려 준다면, 이쪽은 지금
 * 실제로 설정된 값을 알려 준다. 읽는 레지스터가 Status 가 아니라
 * Command 라는 점이 그 차이다 — Status 는 하드웨어가 고정으로 알려 주는
 * 능력이고, Command 는 소프트웨어가 쓰는 설정이다.
 *
 * 인코딩(512 << n)은 max 판과 같다.
 *
 * 실행 컨텍스트: 제약 없음(config 읽기).
 * 호출자: PCI-X 장치 드라이버.
 */
int pcix_get_mmrbc(struct pci_dev *dev)
{
	int cap;	/* [한국어] PCI-X capability 오프셋 */
	u16 cmd;	/* [한국어] PCI-X Command 레지스터 값(설정) */

	cap = pci_find_capability(dev, PCI_CAP_ID_PCIX);
	if (!cap)
		return -EINVAL;

	/* [한국어] Command 레지스터(capability + 0x02)는 16비트다.
	 * Status 와 달리 word 로 읽는 것에 주의. */
	if (pci_read_config_word(dev, cap + PCI_X_CMD, &cmd))
		return -EINVAL;

	/* [한국어] max 판과 같은 인코딩. */
	return 512 << FIELD_GET(PCI_X_CMD_MAX_READ, cmd);
}
EXPORT_SYMBOL(pcix_get_mmrbc);

/**
 * pcix_set_mmrbc - set PCI-X maximum memory read byte count
 * @dev: PCI device to query
 * @mmrbc: maximum memory read count in bytes
 *    valid values are 512, 1024, 2048, 4096
 *
 * If possible sets maximum memory read byte count, some bridges have errata
 * that prevent this.
 */
/*
 * [한국어]
 * pcix_set_mmrbc - Memory Read Byte Count 를 바꾼다
 *
 * @dev:   대상 장치
 * @mmrbc: 설정할 바이트 수. 512 / 1024 / 2048 / 4096 만 유효하다.
 * @return: 0 = 성공(또는 이미 같은 값이라 할 일 없음),
 *          -EINVAL = 잘못된 값이거나 PCI-X 장치가 아님,
 *          -E2BIG = 하드웨어 상한을 넘음,
 *          -EIO = 버스가 이 변경을 금지하거나 쓰기 실패.
 *
 * 검증을 여러 겹으로 하는 것이 이 함수의 특징이다.
 *   1) 값 자체가 유효한가 - 512~4096 사이의 2의 거듭제곱.
 *   2) 하드웨어가 그만큼 지원하는가 - Status 의 상한과 비교.
 *   3) 이 버스에서 값을 올려도 되는가 - 일부 브리지에 에라타가 있어
 *      MMRBC 를 키우면 오동작한다(PCI_BUS_FLAGS_NO_MMRBC quirk).
 *      값을 낮추는 것은 언제나 안전하므로 v > o 일 때만 막는다.
 *
 * ffs(mmrbc) - 10 이라는 변환이 눈에 띈다. ffs 는 "가장 낮은 1 비트의 위치"
 * 를 1-기반으로 돌려준다. mmrbc 가 2의 거듭제곱이므로 그 비트가 유일하다.
 *   512  = 2^9  -> ffs = 10 -> v = 0
 *   1024 = 2^10 -> ffs = 11 -> v = 1
 *   2048 = 2^11 -> ffs = 12 -> v = 2
 *   4096 = 2^12 -> ffs = 13 -> v = 3
 * 즉 앞의 get 함수들이 쓰는 "512 << n" 인코딩의 역변환이다.
 *
 * 값이 이미 같으면 아무것도 쓰지 않는다(o != v 검사). 불필요한 config
 * 쓰기를 피하는 것도 있지만, 위의 NO_MMRBC 검사에 걸리지 않게 하려는
 * 목적도 있다 — 같은 값을 다시 쓰는 것은 변경이 아니기 때문이다.
 *
 * 실행 컨텍스트: 제약 없음(config 접근).
 * 호출자: PCI-X 장치 드라이버가 성능 조정 시.
 */
int pcix_set_mmrbc(struct pci_dev *dev, int mmrbc)
{
	int cap;		/* [한국어] PCI-X capability 오프셋 */
	/* [한국어] stat = Status 레지스터(하드웨어 상한),
	 * v = 새로 설정할 인코딩 값(0~3), o = 현재 인코딩 값 */
	u32 stat, v, o;
	u16 cmd;		/* [한국어] Command 레지스터 값 */

	/* [한국어] 1차 검증 — 스펙이 허용하는 네 값(512/1024/2048/4096)인가.
	 * 범위와 2의 거듭제곱 여부를 함께 보면 그 넷만 통과한다. */
	if (mmrbc < 512 || mmrbc > 4096 || !is_power_of_2(mmrbc))
		return -EINVAL;

	/* [한국어] 바이트 수를 레지스터 인코딩(0~3)으로 변환.
	 * ffs(512)=10 이므로 10 을 빼면 0 부터 시작한다. */
	v = ffs(mmrbc) - 10;

	cap = pci_find_capability(dev, PCI_CAP_ID_PCIX);
	if (!cap)
		return -EINVAL;

	/* [한국어] 2차 검증 준비 — 하드웨어 상한을 읽는다. */
	if (pci_read_config_dword(dev, cap + PCI_X_STATUS, &stat))
		return -EINVAL;

	/* [한국어] 하드웨어가 지원하지 않는 값이면 -E2BIG.
	 * -EINVAL 과 구분하는 이유는 호출자가 "값 자체가 틀렸다" 와
	 * "이 장치에서는 너무 크다" 를 다르게 다룰 수 있게 하기 위해서다. */
	if (v > FIELD_GET(PCI_X_STATUS_MAX_READ, stat))
		return -E2BIG;

	/* [한국어] 현재 설정값을 읽는다. read-modify-write 를 위해서다. */
	if (pci_read_config_word(dev, cap + PCI_X_CMD, &cmd))
		return -EINVAL;

	o = FIELD_GET(PCI_X_CMD_MAX_READ, cmd);	/* [한국어] 현재 인코딩 값 */
	/* [한국어] 이미 원하는 값이면 아무것도 하지 않는다. */
	if (o != v) {
		/* [한국어] 3차 검증 — 값을 올리는 경우에만 버스 quirk 를 확인한다.
		 * 일부 브리지가 큰 MMRBC 를 제대로 처리하지 못하는 에라타 때문이다.
		 * 낮추는 것은 항상 안전하므로 v > o 조건이 붙는다. */
		if (v > o && (dev->bus->bus_flags & PCI_BUS_FLAGS_NO_MMRBC))
			return -EIO;

		/* [한국어] 해당 필드만 지우고 새 값을 끼워 넣는다. Command 레지스터에는
		 * 다른 설정(Relaxed Ordering, Max Outstanding Split 등)도 있어
		 * 통째로 덮어쓰면 안 된다. */
		cmd &= ~PCI_X_CMD_MAX_READ;
		cmd |= FIELD_PREP(PCI_X_CMD_MAX_READ, v);
		if (pci_write_config_word(dev, cap + PCI_X_CMD, cmd))
			return -EIO;
	}
	return 0;
}
EXPORT_SYMBOL(pcix_set_mmrbc);

/**
 * pcie_get_readrq - get PCI Express read request size
 * @dev: PCI device to query
 *
 * Returns maximum memory read request in bytes or appropriate error value.
 */
/*
 * [한국어]
 * pcie_get_readrq - 현재 설정된 Max Read Request Size 를 읽는다
 *
 * @dev:    조회할 장치
 * @return: 바이트 단위 값(128~4096).
 *
 * MRRS(Max Read Request Size)는 이 장치가 한 번의 Memory Read Request 로
 * 요청할 수 있는 최대 바이트 수다. PCI-X 의 MMRBC 에 대응하는 PCIe 개념이다.
 *
 * NVMe 학습 관점에서 중요한 파라미터다. NVMe 컨트롤러는 호스트 메모리에서
 * 데이터를 읽어 올 때(쓰기 명령의 데이터, PRP 리스트, SQ 엔트리) 이 크기로
 * 요청을 쪼갠다. 값이 작으면 요청 개수가 늘어 헤더 오버헤드와 완료 처리
 * 부담이 커지고, 값이 크면 한 요청이 링크를 오래 점유해 다른 트래픽의
 * 지연이 커진다. 기본값은 보통 512바이트다.
 *
 * MPS(Max Payload Size)와 혼동하기 쉬운데 방향이 다르다.
 *   MRRS - "내가 한 번에 얼마나 요청하는가"(읽기 요청의 크기)
 *   MPS  - "한 TLP 에 데이터를 얼마나 담는가"(실제 전송 단위)
 * 4KB 를 MRRS 로 요청해도 MPS 가 256이면 응답은 256바이트짜리 TLP 16개로
 * 나뉘어 온다.
 *
 * 인코딩은 128 << n 이다(n = 0~5 -> 128~4096).
 * 반환값 검사가 없다는 점에 주의 — pcie_capability_read_word 가 실패하면
 * ctl 은 0 이 되고, 결과는 128 이 된다. 최소값이라 안전한 실패다.
 *
 * 실행 컨텍스트: 제약 없음(config 읽기).
 * 호출자: pcie_set_readrq() 자신, sysfs, 그리고 성능을 조정하는 드라이버들.
 *   drivers/nvme/ 에는 직접 호출이 없다 — 기본값을 그대로 쓴다.
 */
int pcie_get_readrq(struct pci_dev *dev)
{
	u16 ctl;	/* [한국어] Device Control 레지스터(PCIe capability + 0x08) */

	/* [한국어] 실패하면 ctl 이 0 으로 채워진다(pcie_capability_read_word 의 규약).
	 * 그 경우 아래 계산이 128 을 돌려주는데, 최소값이므로 무해하다. */
	pcie_capability_read_word(dev, PCI_EXP_DEVCTL, &ctl);

	/* [한국어] READRQ 필드(비트 14:12)의 값 n 을 128 << n 으로 환산.
	 * n=0 -> 128, n=1 -> 256, ... n=5 -> 4096. */
	return 128 << FIELD_GET(PCI_EXP_DEVCTL_READRQ, ctl);
}
EXPORT_SYMBOL(pcie_get_readrq);

/**
 * pcie_set_readrq - set PCI Express maximum memory read request
 * @dev: PCI device to query
 * @rq: maximum memory read count in bytes
 *    valid values are 128, 256, 512, 1024, 2048, 4096
 *
 * If possible sets maximum memory read request in bytes
 */
/*
 * [한국어]
 * pcie_set_readrq - Max Read Request Size 를 바꾼다
 *
 * @dev:    대상 장치
 * @rq:     설정할 바이트 수. 128/256/512/1024/2048/4096 만 유효하다.
 * @return: 0 = 성공, -EINVAL = 잘못된 값이거나 브리지가 허용하지 않음,
 *          그 외 = config 쓰기 실패의 errno.
 *
 * 값을 그대로 쓰지 않고 두 번 깎일 수 있다는 점이 이 함수의 요점이다.
 *
 *   1) "performance" 버스 설정에서는 MRRS 를 MPS 이하로 낮춘다.
 *      원문 주석이 이유를 밝힌다 — 호스트 브리지가 감당할 수 없는 큰 요청을
 *      만들어 내지 않게 하기 위해서다. MRRS 가 MPS 보다 크면 하나의 읽기
 *      요청에 대한 응답이 여러 TLP 로 쪼개져 오는데, 일부 브리지가 그
 *      재조립을 제대로 못 한다.
 *
 *   2) no_inc_mrrs 플래그가 선 호스트 브리지에서는 값을 "올리는" 것 자체를
 *      금지한다. 펌웨어가 정해 둔 값보다 크게 만들면 오동작하는 플랫폼용
 *      안전장치다. 흥미로운 점은 여기서 상한으로 쓰는 max_mrrs 가
 *      pcie_get_readrq(dev), 즉 지금 설정된 값이라는 것이다 — "현재보다
 *      키우지 말라" 는 뜻이지 하드웨어 능력과는 무관하다.
 *
 * ffs(rq) - 8 변환은 pcix_set_mmrbc 의 -10 과 같은 원리다.
 *   128 = 2^7 -> ffs = 8 -> 인코딩 0
 *   4096 = 2^12 -> ffs = 13 -> 인코딩 5
 * firstbit < 8 검사는 128 미만을 거른다. 위에서 이미 rq >= 128 을 확인했지만,
 * MPS 클램프가 rq 를 낮춘 뒤라서 다시 확인해야 한다 — MPS 가 128 미만인
 * 이상한 장치가 있다면 여기서 걸린다.
 *
 * pcie_capability_clear_and_set_word 를 쓰는 이유: DEVCTL 에는 MPS, Relaxed
 * Ordering, No Snoop, Extended Tag 등 다른 설정이 함께 있어 통째로 덮어쓰면
 * 안 된다. READRQ 필드만 골라 갈아끼운다.
 *
 * 실행 컨텍스트: 제약 없음(config 접근).
 * 호출자: sysfs 의 max_read_request_size 속성, 성능을 조정하는 드라이버.
 *   drivers/nvme/ 에는 직접 호출이 없다.
 */
int pcie_set_readrq(struct pci_dev *dev, int rq)
{
	u16 v;			/* [한국어] DEVCTL 의 READRQ 필드 자리에 맞춘 값 */
	int ret;		/* [한국어] config 쓰기 결과(PCIBIOS_*) */
	unsigned int firstbit;	/* [한국어] rq 의 유일한 1 비트 위치(1-기반) */
	/* [한국어] 이 장치가 속한 호스트 브리지. no_inc_mrrs 정책을 읽기 위해 필요하다. */
	struct pci_host_bridge *bridge = pci_find_host_bridge(dev->bus);

	/* [한국어] 스펙이 허용하는 여섯 값(128~4096의 2의 거듭제곱)인지 확인. */
	if (rq < 128 || rq > 4096 || !is_power_of_2(rq))
		return -EINVAL;

	/*
	 * If using the "performance" PCIe config, we clamp the read rq
	 * size to the max packet size to keep the host bridge from
	 * generating requests larger than we can cope with.
	 */
	/* [한국어] 1차 깎임 — performance 모드에서는 MRRS 를 MPS 이하로 제한한다.
	 * pcie_bus_config 는 "pci=pcie_bus_perf" 같은 부팅 인자로 정해지는
	 * 전역 정책이다. 요청한 값보다 작아질 수 있으므로 rq 자체를 덮어쓴다. */
	if (pcie_bus_config == PCIE_BUS_PERFORMANCE) {
		int mps = pcie_get_mps(dev);

		/* [한국어] MPS 보다 큰 MRRS 를 요청하면 응답이 여러 TLP 로 쪼개져
		 * 오는데, 일부 호스트 브리지가 그 재조립을 감당하지 못한다.
		 * 그래서 MPS 이하로 깎는다. 요청값보다 작아질 수 있다. */
		if (mps < rq)
			rq = mps;
	}

	/* [한국어] 바이트 수를 레지스터 인코딩으로 변환. ffs(128)=8 이므로 8 을 뺀다. */
	firstbit = ffs(rq);
	/* [한국어] 클램프 뒤 다시 확인한다. 위 검사는 클램프 전 값에 대한 것이라
	 * MPS 가 비정상적으로 작은 장치에서는 여기서만 걸린다. */
	if (firstbit < 8)
		return -EINVAL;
	v = FIELD_PREP(PCI_EXP_DEVCTL_READRQ, firstbit - 8);

	/* [한국어] 2차 검증 — 이 호스트 브리지가 MRRS 증가를 금지하는가.
	 * 펌웨어가 설정해 둔 값보다 키우면 오동작하는 플랫폼을 위한 quirk 다. */
	if (bridge->no_inc_mrrs) {
		/* [한국어] 상한은 "현재 설정값" 이다. 하드웨어 능력이 아니라
		 * 펌웨어가 정해 둔 값을 그대로 지키라는 뜻이다. */
		int max_mrrs = pcie_get_readrq(dev);

		if (rq > max_mrrs) {
			/* [한국어] 조용히 실패하지 않고 로그를 남긴다. 성능이 기대만큼
			 * 나오지 않을 때 원인을 찾을 단서가 된다. */
			pci_info(dev, "can't set Max_Read_Request_Size to %d; max is %d\n", rq, max_mrrs);
			return -EINVAL;
		}
	}

	/* [한국어] READRQ 필드만 지우고 새 값을 끼워 넣는다. DEVCTL 의 다른
	 * 설정(MPS, Relaxed Ordering 등)은 그대로 보존된다. */
	ret = pcie_capability_clear_and_set_word(dev, PCI_EXP_DEVCTL,
						  PCI_EXP_DEVCTL_READRQ, v);

	/* [한국어] PCIBIOS_* 를 음수 errno 로 변환. 이 함수는 드라이버와 sysfs 가
	 * 부르므로 커널 표준 errno 여야 한다. */
	return pcibios_err_to_errno(ret);
}
EXPORT_SYMBOL(pcie_set_readrq);

/**
 * pcie_get_mps - get PCI Express maximum payload size
 * @dev: PCI device to query
 *
 * Returns maximum payload size in bytes
 */
/*
 * [한국어]
 * pcie_get_mps - 현재 설정된 Max Payload Size 를 읽는다
 *
 * @dev:    조회할 장치
 * @return: 바이트 단위 값(128~4096).
 *
 * MPS(Max Payload Size)는 하나의 TLP(Transaction Layer Packet)에 담을 수 있는
 * 데이터의 최대 바이트 수다. TLP 헤더는 크기가 고정이므로, MPS 가 클수록
 * 헤더 오버헤드 비율이 줄어 실효 대역폭이 올라간다.
 *
 * 중요한 제약이 있다. MPS 는 한 계층(링크의 양 끝) 전체가 같은 값을 써야
 * 한다 — 정확히는 송신자가 수신자의 MPS 보다 큰 TLP 를 보내면 안 된다.
 * 그래서 커널은 계층 전체를 훑어 최소 공통값을 찾아 모두에게 설정한다
 * (pcie_bus_configure_settings). 개별 장치가 자기 마음대로 올릴 수 없다.
 *
 * NVMe 학습 관점: NVMe 의 데이터 전송이 이 크기로 쪼개진다. 4KB 블록 하나를
 * 쓰는 명령이라면, MPS 가 128 이면 32개의 TLP 로, 512 면 8개로 나뉜다.
 * TLP 개수가 곧 링크 오버헤드이므로 MPS 는 NVMe 처리량에 직접 영향을 준다.
 * 다만 계층의 최소값에 묶이므로, 중간에 MPS 128 짜리 옛 브리지가 하나
 * 끼어 있으면 최신 SSD 도 128 로 내려간다.
 *
 * MRRS 와의 차이는 pcie_get_readrq() 주석 참고.
 *
 * 실행 컨텍스트: 제약 없음(config 읽기).
 * 호출자: pcie_set_readrq() 의 클램프, pcie_bus_configure_settings(), sysfs.
 */
int pcie_get_mps(struct pci_dev *dev)
{
	u16 ctl;	/* [한국어] Device Control 레지스터 값 */

	/* [한국어] 실패 시 ctl=0 -> 결과 128(최소값). MRRS 판과 같은 안전한 실패다. */
	pcie_capability_read_word(dev, PCI_EXP_DEVCTL, &ctl);

	/* [한국어] PAYLOAD 필드(비트 7:5)를 128 << n 으로 환산. */
	return 128 << FIELD_GET(PCI_EXP_DEVCTL_PAYLOAD, ctl);
}
EXPORT_SYMBOL(pcie_get_mps);

/**
 * pcie_set_mps - set PCI Express maximum payload size
 * @dev: PCI device to query
 * @mps: maximum payload size in bytes
 *    valid values are 128, 256, 512, 1024, 2048, 4096
 *
 * If possible sets maximum payload size
 */
/*
 * [한국어]
 * pcie_set_mps - Max Payload Size 를 바꾼다
 *
 * @dev:    대상 장치
 * @mps:    설정할 바이트 수. 128/256/512/1024/2048/4096 만 유효하다.
 * @return: 0 = 성공, -EINVAL = 잘못된 값이거나 하드웨어 능력을 넘음,
 *          그 외 = config 쓰기 실패의 errno.
 *
 * MRRS 설정과 구조가 비슷하지만 검증이 하나 다르다. 여기서는
 * dev->pcie_mpss 와 비교한다 — MPSS(Max Payload Size Supported)는 Device
 * Capability 레지스터에 하드웨어가 적어 둔 능력치이고, 열거 시 캐시해 둔
 * 값이다. 매번 config 를 읽지 않기 위해서다.
 *
 * 이 함수는 개별 장치의 값만 바꾼다. 계층 전체의 일관성(송신자가 수신자의
 * MPS 를 넘지 않아야 한다)은 호출자인 pcie_bus_configure_settings() 가
 * 책임진다. 그쪽이 계층을 두 번 훑어 공통값을 정한 뒤 이 함수로 하나씩
 * 설정한다. 그래서 이 함수를 직접 부르면 계층 일관성이 깨질 수 있다.
 *
 * ffs(mps) - 8 변환은 MRRS 와 같다(128 = 2^7 -> ffs 8 -> 인코딩 0).
 *
 * 실행 컨텍스트: 제약 없음(config 접근).
 * 호출자: pcie_bus_configure_settings() 경로, sysfs.
 *   drivers/nvme/ 에는 직접 호출이 없다.
 */
int pcie_set_mps(struct pci_dev *dev, int mps)
{
	u16 v;		/* [한국어] PAYLOAD 필드 자리에 맞춘 값 */
	int ret;	/* [한국어] config 쓰기 결과 */

	/* [한국어] 스펙이 허용하는 여섯 값인지 확인. */
	if (mps < 128 || mps > 4096 || !is_power_of_2(mps))
		return -EINVAL;

	/* [한국어] 바이트 수를 인코딩(0~5)으로 변환. */
	v = ffs(mps) - 8;
	/* [한국어] 하드웨어 능력 검사. pcie_mpss 는 Device Capability 의
	 * Max Payload Size Supported 필드를 열거 시 캐시해 둔 값이다.
	 * 능력을 넘는 값을 쓰면 장치가 받아들이지 않거나 오동작한다. */
	if (v > dev->pcie_mpss)
		return -EINVAL;
	/* [한국어] 인코딩을 레지스터의 해당 비트 자리로 옮긴다. */
	v = FIELD_PREP(PCI_EXP_DEVCTL_PAYLOAD, v);

	/* [한국어] PAYLOAD 필드만 갈아끼운다. READRQ 등 다른 설정은 보존. */
	ret = pcie_capability_clear_and_set_word(dev, PCI_EXP_DEVCTL,
						  PCI_EXP_DEVCTL_PAYLOAD, v);

	return pcibios_err_to_errno(ret);	/* [한국어] 커널 errno 로 변환 */
}
EXPORT_SYMBOL(pcie_set_mps);

/*
 * [한국어]
 * to_pcie_link_speed - LNKSTA 레지스터 값에서 현재 링크 속도를 뽑아낸다
 *
 * @lnksta: Link Status 레지스터(PCIe capability + 0x12)의 값
 * @return: enum pci_bus_speed. PCIE_SPEED_2_5GT ~ PCIE_SPEED_64_0GT 또는
 *          PCI_SPEED_UNKNOWN.
 *
 * LNKSTA 의 Current Link Speed 필드(비트 3:0)는 1~6 같은 작은 정수다.
 * 그 값을 그대로 쓰면 코드가 매직 넘버로 뒤덮이므로, pcie_link_speed[]
 * 배열로 enum 값에 대응시킨다. 배열은 이 파일 앞쪽에 정의돼 있다.
 *
 * "Current" 라는 점이 중요하다 — 하드웨어가 낼 수 있는 최대 속도가 아니라
 * 지금 실제로 협상된 속도다. 링크가 불안정하면 Gen4 장치도 Gen1 으로
 * 떨어져 동작할 수 있고, 그때 이 함수는 2.5GT/s 를 돌려준다.
 *
 * NVMe 학습 관점: NVMe SSD 의 실효 대역폭 상한이 여기서 결정된다.
 * lspci 나 sysfs 의 current_link_speed 가 보여 주는 값이 이것이며,
 * 기대보다 낮다면 슬롯 배선, 케이블, 또는 링크 훈련 실패를 의심해야 한다.
 *
 * 실행 컨텍스트: 제약 없음. 순수 계산이다.
 * 호출자: pcie_bandwidth_available(), pcie_link_speed_mbps(),
 *   pcie_update_link_speed().
 */
static enum pci_bus_speed to_pcie_link_speed(u16 lnksta)
{
	/* [한국어] Current Link Speed 필드를 배열 인덱스로 써서 enum 으로 변환.
	 * 정의되지 않은 인덱스가 들어와도 배열이 그만큼 크고 미정의 자리는
	 * PCI_SPEED_UNKNOWN 으로 채워져 있어 안전하다. */
	return pcie_link_speed[FIELD_GET(PCI_EXP_LNKSTA_CLS, lnksta)];
}

/*
 * [한국어]
 * pcie_link_speed_mbps - 현재 링크 속도를 레인당 Mb/s 로 돌려준다
 *
 * @pdev:   조회할 장치
 * @return: 양수 = 레인 하나당 Mb/s, 음수 = config 읽기 실패의 errno.
 *
 * to_pcie_link_speed() 가 enum 을 주는 데 반해 이 함수는 숫자를 준다.
 * 인코딩을 해석해야 하는 부담 없이 곧바로 계산에 쓸 수 있는 형태다.
 *
 * 주의할 점: 반환값은 "레인 하나당" 속도이고, 인코딩 오버헤드를 뺀
 * 실효값이다. Gen1/Gen2 는 8b/10b 인코딩이라 2.5GT/s -> 2000Mb/s,
 * 5GT/s -> 4000Mb/s 이고, Gen3 이상은 128b/130b 라 손실이 훨씬 적어
 * 8GT/s -> 7877Mb/s 가 된다. 전체 대역폭을 구하려면 레인 수를 곱해야 한다.
 *
 * 반환값이 int 인데 오류도 같은 자리로 돌려준다는 점에 주의해야 한다.
 * 속도 값은 항상 양수이므로 음수 여부로 오류를 판정할 수 있다.
 *
 * 실행 컨텍스트: 제약 없음(config 읽기).
 * 호출자: 링크 대역폭을 숫자로 필요로 하는 드라이버들.
 */
int pcie_link_speed_mbps(struct pci_dev *pdev)
{
	u16 lnksta;	/* [한국어] Link Status 레지스터 값 */
	int err;

	/* [한국어] 실패하면 그대로 errno 를 올려 보낸다. 여기서는 0 으로
	 * 채워진 값을 해석하지 않는다 — 속도는 잘못 알면 계산이 전부 틀어지므로
	 * "모른다" 를 명확히 알리는 편이 낫다. */
	err = pcie_capability_read_word(pdev, PCI_EXP_LNKSTA, &lnksta);
	if (err)
		return err;

	/* [한국어] LNKSTA -> enum -> Mb/s 두 단계 변환.
	 * pcie_dev_speed_mbps 가 인코딩 오버헤드를 반영한 실효값을 준다. */
	return pcie_dev_speed_mbps(to_pcie_link_speed(lnksta));
}
EXPORT_SYMBOL(pcie_link_speed_mbps);

/**
 * pcie_bandwidth_available - determine minimum link settings of a PCIe
 *			      device and its bandwidth limitation
 * @dev: PCI device to query
 * @limiting_dev: storage for device causing the bandwidth limitation
 * @speed: storage for speed of limiting device
 * @width: storage for width of limiting device
 *
 * Walk up the PCI device chain and find the point where the minimum
 * bandwidth is available.  Return the bandwidth available there and (if
 * limiting_dev, speed, and width pointers are supplied) information about
 * that point.  The bandwidth returned is in Mb/s, i.e., megabits/second of
 * raw bandwidth.
 */
/*
 * [한국어]
 * pcie_bandwidth_available - 이 장치까지 오는 경로에서 가장 좁은 구간을 찾는다
 *
 * @dev:          기준 장치(보통 엔드포인트)
 * @limiting_dev: 병목이 되는 장치를 담아 돌려줄 곳. 필요 없으면 NULL.
 * @speed:        그 지점의 링크 속도. 필요 없으면 NULL.
 * @width:        그 지점의 링크 폭. 필요 없으면 NULL.
 * @return:       Mb/s 단위 가용 대역폭. 0 이면 판정 불가.
 *
 * 대역폭은 경로 전체에서 가장 좁은 구간에 의해 결정된다. 이 함수는 장치에서
 * 루트까지 거슬러 올라가며 각 링크의 (폭 x 레인당 속도)를 계산하고, 그중
 * 최소값과 그것을 만든 장치를 함께 돌려준다.
 *
 * NVMe 학습 관점에서 매우 유용한 함수다. Gen4 x4 NVMe SSD 를 꽂았는데
 * 기대 성능이 안 나오는 경우, 원인이 SSD 자신이 아니라 중간 스위치나
 * 슬롯 배선일 수 있다. 이 함수는 그 병목이 정확히 어느 장치인지 짚어 준다.
 * 실제로 커널이 부팅 로그에 "N Mb/s available PCIe bandwidth, limited by
 * ... " 를 찍는 것이 이 결과다(__pcie_print_link_status).
 *
 * 조건 "!bw || next_bw <= bw" 가 미묘하다.
 *   !bw       - 첫 번째 반복. 비교할 기준이 없으므로 무조건 채택한다.
 *   <= (미만이 아니라 이하) - 같은 대역폭 구간이 여럿이면 더 위쪽(루트에
 *               가까운) 것을 병목으로 기록한다. 상류일수록 더 많은 장치가
 *               공유하므로 실제 경합이 심한 지점이기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(config 읽기를 여러 번 한다).
 * 호출자: __pcie_print_link_status(), 드라이버의 성능 진단 코드.
 *
 * 호출 체인:
 *   pcie_print_link_status -> __pcie_print_link_status
 *     -> [pcie_bandwidth_available] -> pcie_capability_read_word (계층마다)
 */
u32 pcie_bandwidth_available(struct pci_dev *dev, struct pci_dev **limiting_dev,
			     enum pci_bus_speed *speed,
			     enum pcie_link_width *width)
{
	u16 lnksta;			/* [한국어] 현재 보고 있는 장치의 LNKSTA */
	enum pci_bus_speed next_speed;	/* [한국어] 이 구간의 링크 속도 */
	enum pcie_link_width next_width;/* [한국어] 이 구간의 링크 폭(레인 수) */
	u32 bw, next_bw;		/* [한국어] bw = 지금까지의 최소, next_bw = 이 구간의 값 */

	/* [한국어] 출력 인자를 먼저 "모름" 으로 초기화한다. 루프가 한 번도 돌지
	 * 않거나(dev 가 NULL) 모든 읽기가 실패해도 호출자가 쓰레기를 보지 않는다. */
	if (speed)
		*speed = PCI_SPEED_UNKNOWN;
	if (width)
		*width = PCIE_LNK_WIDTH_UNKNOWN;

	bw = 0;	/* [한국어] 0 은 "아직 아무것도 못 봤다" 는 표시로도 쓰인다 */

	/* [한국어] 장치에서 루트까지 한 단계씩 거슬러 올라간다. */
	while (dev) {
		/* [한국어] 이 장치의 상류 링크 상태를 읽는다. 실패하면 lnksta 가 0 이 되어
		 * 속도/폭이 0 으로 나오고, 그러면 next_bw 도 0 이라 이 구간이 병목으로
		 * 기록된다 — 읽을 수 없는 링크를 "무제한" 으로 낙관하는 것보다 안전하다. */
		pcie_capability_read_word(dev, PCI_EXP_LNKSTA, &lnksta);

		next_speed = to_pcie_link_speed(lnksta);		/* [한국어] Current Link Speed */
		next_width = FIELD_GET(PCI_EXP_LNKSTA_NLW, lnksta);	/* [한국어] Negotiated Link Width */

		/* [한국어] 이 구간의 대역폭 = 레인 수 x 레인당 실효 Mb/s.
		 * PCIE_SPEED2MBS_ENC 가 인코딩 오버헤드(8b/10b vs 128b/130b)를
		 * 반영한 값을 준다. */
		next_bw = next_width * PCIE_SPEED2MBS_ENC(next_speed);

		/* Check if current device limits the total bandwidth */
		/* [한국어] 첫 반복이거나, 이 구간이 지금까지의 최소 이하이면 갱신한다.
		 * '이하'(<=)인 것이 의도적이다 — 같은 값이면 더 상류를 병목으로 본다. */
		if (!bw || next_bw <= bw) {
			bw = next_bw;

			/* [한국어] 호출자가 요청한 정보만 채운다. */
			if (limiting_dev)
				*limiting_dev = dev;
			if (speed)
				*speed = next_speed;
			if (width)
				*width = next_width;
		}

		/* [한국어] 한 단계 위로. 루트 버스에 닿으면 NULL 이 되어 루프가 끝난다. */
		dev = pci_upstream_bridge(dev);
	}

	return bw;	/* [한국어] 경로 전체의 최소 대역폭 */
}
EXPORT_SYMBOL(pcie_bandwidth_available);

/**
 * pcie_get_supported_speeds - query Supported Link Speed Vector
 * @dev: PCI device to query
 *
 * Query @dev supported link speeds.
 *
 * Implementation Note in PCIe r6.0 sec 7.5.3.18 recommends determining
 * supported link speeds using the Supported Link Speeds Vector in the Link
 * Capabilities 2 Register (when available).
 *
 * Link Capabilities 2 was added in PCIe r3.0, sec 7.8.18.
 *
 * Without Link Capabilities 2, i.e., prior to PCIe r3.0, Supported Link
 * Speeds field in Link Capabilities is used and only 2.5 GT/s and 5.0 GT/s
 * speeds were defined.
 *
 * For @dev without Supported Link Speed Vector, the field is synthesized
 * from the Max Link Speed field in the Link Capabilities Register.
 *
 * Return: Supported Link Speeds Vector (+ reserved 0 at LSB).
 */
/*
 * [한국어]
 * pcie_get_supported_speeds - 이 장치가 지원하는 링크 속도들을 비트맵으로 모은다
 *
 * @dev:    조회할 장치
 * @return: Supported Link Speeds Vector. 각 비트가 하나의 속도를 뜻한다
 *          (비트1=2.5GT/s, 비트2=5.0GT/s, 비트3=8.0GT/s, ...).
 *          비트 0 은 예약이라 항상 0 이다.
 *
 * 왜 "최대 속도" 하나가 아니라 비트맵인가. 하드웨어가 중간 속도를 건너뛸 수
 * 있기 때문이다. 예컨대 2.5/5.0/16.0 은 지원하는데 8.0 은 지원하지 않는
 * 장치가 존재한다. 최대값만 알면 그런 구멍을 표현할 수 없다.
 *
 * 정보를 얻는 경로가 세 갈래다.
 *   1) LNKCAP2 의 Supported Link Speeds Vector - PCIe r3.0 에서 추가된
 *      정식 수단이다. 스펙이 이것을 우선 쓰라고 권고한다(r6.0 7.5.3.18).
 *   2) LNKCAP 의 Max Link Speed 로 상한을 걸기 - 두 레지스터가 어긋난
 *      장치가 있어서, 벡터에 있어도 최대 속도를 넘는 비트는 지운다.
 *   3) LNKCAP2 가 없는 옛 장치(PCIe r3.0 이전) - 최대 속도로부터 벡터를
 *      "합성" 한다. 그 시절에는 2.5 와 5.0 뿐이었고 건너뛰기도 없었으므로,
 *      최대가 5.0 이면 {2.5, 5.0} 이라고 단정할 수 있다.
 *
 * 원문 주석이 밝히는 설계 하나 — 비트 0 을 예약으로 비워 두는 덕분에
 * PCI_EXP_LNKCAP2_SLS_* 상수를 변환 없이 그대로 쓸 수 있다. 인덱스와
 * 비트 위치를 맞추기 위한 의도적인 낭비다.
 *
 * NVMe 학습 관점: NVMe SSD 의 "Gen4 지원" 같은 사양이 이 벡터에 들어 있다.
 * 결과는 dev->supported_speeds 에 캐시되고, pcie_get_speed_cap() 이
 * 그것을 읽어 최대 속도를 알려 준다.
 *
 * 실행 컨텍스트: 제약 없음(config 읽기 2회).
 * 호출자: probe.c 의 열거 경로가 한 번 불러 dev->supported_speeds 에 저장.
 */
u8 pcie_get_supported_speeds(struct pci_dev *dev)
{
	u32 lnkcap2, lnkcap;	/* [한국어] Link Capabilities 2 와 Link Capabilities */
	u8 speeds;		/* [한국어] 결과 비트맵 */

	/*
	 * Speeds retain the reserved 0 at LSB before PCIe Supported Link
	 * Speeds Vector to allow using SLS Vector bit defines directly.
	 */
	/* [한국어] 1단계 — 정식 벡터를 읽는다. LNKCAP2 가 없는 장치에서는
	 * pcie_capability_read_dword 가 0 을 채워 주므로 speeds 도 0 이 된다. */
	pcie_capability_read_dword(dev, PCI_EXP_LNKCAP2, &lnkcap2);
	speeds = lnkcap2 & PCI_EXP_LNKCAP2_SLS;

	/* Ignore speeds higher than Max Link Speed */
	/* [한국어] 2단계 — 최대 속도를 넘는 비트를 지운다.
	 * GENMASK(n, 0) 은 비트 0..n 이 1인 마스크다. Max Link Speed 값이 곧
	 * 그 속도의 비트 번호이므로(2.5GT/s=1, 5.0=2, 8.0=3 ...), 그 위 비트가
	 * 모두 잘려 나간다. 두 레지스터가 서로 어긋나게 구현된 장치를 위한 방어다. */
	pcie_capability_read_dword(dev, PCI_EXP_LNKCAP, &lnkcap);
	speeds &= GENMASK(lnkcap & PCI_EXP_LNKCAP_SLS, 0);

	/* PCIe r3.0-compliant */
	/* [한국어] 벡터를 얻었으면 여기서 끝. r3.0 이후 장치의 정상 경로다. */
	if (speeds)
		return speeds;

	/* Synthesize from the Max Link Speed field */
	/* [한국어] 3단계 — LNKCAP2 가 없는 옛 장치. 최대 속도로부터 벡터를 만든다.
	 * 그 시절 정의된 속도가 2.5 와 5.0 둘뿐이고 건너뛰기도 없었으므로
	 * 최대값만으로 전체 집합이 결정된다. */
	if ((lnkcap & PCI_EXP_LNKCAP_SLS) == PCI_EXP_LNKCAP_SLS_5_0GB)
		speeds = PCI_EXP_LNKCAP2_SLS_5_0GB | PCI_EXP_LNKCAP2_SLS_2_5GB;
	else if ((lnkcap & PCI_EXP_LNKCAP_SLS) == PCI_EXP_LNKCAP_SLS_2_5GB)
		speeds = PCI_EXP_LNKCAP2_SLS_2_5GB;
	/* [한국어] 둘 다 아니면 speeds 는 0 으로 남는다 — 링크 capability 자체가
	 * 없는 장치(RCiEP 등)이거나 config 읽기가 실패한 경우다. */

	return speeds;
}

/**
 * pcie_get_speed_cap - query for the PCI device's link speed capability
 * @dev: PCI device to query
 *
 * Query the PCI device speed capability.
 *
 * Return: the maximum link speed supported by the device.
 */
/*
 * [한국어]
 * pcie_get_speed_cap - 이 장치가 낼 수 있는 최대 링크 속도
 *
 * @dev:    조회할 장치
 * @return: enum pci_bus_speed. 지원 정보가 없으면 PCI_SPEED_UNKNOWN.
 *
 * dev->supported_speeds 는 열거 시 pcie_get_supported_speeds() 가 채워 둔
 * 비트맵이다. PCIE_LNKCAP2_SLS2SPEED 매크로가 그중 가장 높은 비트를 찾아
 * enum 으로 바꿔 준다.
 *
 * 하드웨어를 읽지 않고 캐시된 값만 쓰므로 매우 가볍다. 그래서 리셋 대기
 * 시간을 정할 때처럼 자주 불리는 곳에서도 부담 없이 쓸 수 있다
 * (pci_bridge_wait_for_secondary_bus 가 5GT/s 이하인지 판정하는 데 쓴다).
 *
 * "현재 속도" 가 아니라 "낼 수 있는 최대" 라는 점에 주의. 실제로 협상된
 * 속도는 to_pcie_link_speed(LNKSTA) 로 읽어야 한다.
 *
 * 실행 컨텍스트: 제약 없음. 하드웨어 접근이 없다.
 * 호출자: pci_bridge_wait_for_secondary_bus(), pcie_bandwidth_capable(), sysfs.
 */
enum pci_bus_speed pcie_get_speed_cap(struct pci_dev *dev)
{
	/* [한국어] 비트맵에서 최상위 비트를 골라 enum 으로 변환. */
	return PCIE_LNKCAP2_SLS2SPEED(dev->supported_speeds);
}
EXPORT_SYMBOL(pcie_get_speed_cap);

/**
 * pcie_get_width_cap - query for the PCI device's link width capability
 * @dev: PCI device to query
 *
 * Query the PCI device width capability.  Return the maximum link width
 * supported by the device.
 */
/*
 * [한국어]
 * pcie_get_width_cap - 이 장치가 지원하는 최대 링크 폭(레인 수)
 *
 * @dev:    조회할 장치
 * @return: 레인 수(1, 2, 4, 8, 16, 32) 또는 PCIE_LNK_WIDTH_UNKNOWN.
 *
 * 속도와 달리 폭은 벡터가 필요 없다. 폭은 항상 연속적으로 축소 가능해서
 * "최대 x4" 라면 x1, x2, x4 가 모두 가능하다고 단정할 수 있기 때문이다.
 * 그래서 LNKCAP 의 Max Link Width 필드 하나만 읽으면 된다.
 *
 * lnkcap 이 0 인지 확인하는 것은 실패 감지다. pcie_capability_read_dword 는
 * 실패하거나 레지스터가 없으면 0 을 채우는데, 유효한 LNKCAP 은 최소한
 * Max Link Speed 필드가 0 이 아니므로 전체가 0 일 수 없다. 그래서 0 이면
 * "읽지 못했다" 로 판정하고 UNKNOWN 을 돌려준다.
 *
 * NVMe 학습 관점: NVMe SSD 의 "x4" 사양이 이 값이다. M.2 슬롯이 x2 로만
 * 배선된 보드에 x4 SSD 를 꽂으면, 이 함수는 여전히 4 를 돌려주지만
 * 실제 협상된 폭(LNKSTA 의 Negotiated Link Width)은 2 가 된다.
 * 그 차이를 보여 주는 것이 __pcie_print_link_status 의 경고다.
 *
 * 실행 컨텍스트: 제약 없음(config 읽기).
 * 호출자: pcie_bandwidth_capable(), sysfs 의 max_link_width.
 */
enum pcie_link_width pcie_get_width_cap(struct pci_dev *dev)
{
	u32 lnkcap;	/* [한국어] Link Capabilities 레지스터 값 */

	pcie_capability_read_dword(dev, PCI_EXP_LNKCAP, &lnkcap);
	/* [한국어] 0 은 읽기 실패나 레지스터 부재를 뜻한다. 유효한 LNKCAP 은
	 * 절대 0 이 될 수 없으므로 이 검사로 구분할 수 있다. */
	if (lnkcap)
		return FIELD_GET(PCI_EXP_LNKCAP_MLW, lnkcap);	/* [한국어] Max Link Width 필드 */

	return PCIE_LNK_WIDTH_UNKNOWN;
}
EXPORT_SYMBOL(pcie_get_width_cap);

/**
 * pcie_bandwidth_capable - calculate a PCI device's link bandwidth capability
 * @dev: PCI device
 * @speed: storage for link speed
 * @width: storage for link width
 *
 * Calculate a PCI device's link bandwidth by querying for its link speed
 * and width, multiplying them, and applying encoding overhead.  The result
 * is in Mb/s, i.e., megabits/second of raw bandwidth.
 */
/*
 * [한국어]
 * pcie_bandwidth_capable - 이 장치가 낼 수 있는 최대 대역폭
 *
 * @dev:    조회할 장치
 * @speed:  최대 링크 속도를 담아 돌려줄 곳(필수 — NULL 을 허용하지 않는다)
 * @width:  최대 링크 폭을 담아 돌려줄 곳(필수)
 * @return: Mb/s 단위 최대 대역폭. 속도나 폭을 알 수 없으면 0.
 *
 * pcie_bandwidth_available() 과 짝을 이루는 함수다. 그쪽이 "경로 전체에서
 * 실제로 쓸 수 있는 대역폭" 을 구한다면, 이쪽은 "이 장치 하나가 이론상
 * 낼 수 있는 대역폭" 을 구한다. 둘을 비교하면 병목이 있는지 알 수 있고,
 * 그것이 __pcie_print_link_status() 가 하는 일이다.
 *
 * 계산은 단순하다 — 레인 수 x 레인당 실효 속도. PCIE_SPEED2MBS_ENC 가
 * 인코딩 오버헤드를 이미 반영한 값을 준다(Gen1/2 는 8b/10b 라 20% 손실,
 * Gen3 이상은 128b/130b 라 약 1.5% 손실).
 *
 * 출력 인자가 NULL 을 허용하지 않는다는 점이 pcie_bandwidth_available() 과
 * 다르다. static 함수이고 호출자가 하나뿐이라 그 검사를 생략했다.
 *
 * 실행 컨텍스트: 제약 없음.
 * 호출자: __pcie_print_link_status() 하나뿐이다.
 */
static u32 pcie_bandwidth_capable(struct pci_dev *dev,
				  enum pci_bus_speed *speed,
				  enum pcie_link_width *width)
{
	/* [한국어] 캐시된 supported_speeds 에서 최대 속도를 꺼낸다(하드웨어 접근 없음). */
	*speed = pcie_get_speed_cap(dev);
	/* [한국어] LNKCAP 에서 최대 폭을 읽는다(config 읽기 1회). */
	*width = pcie_get_width_cap(dev);

	/* [한국어] 둘 중 하나라도 모르면 곱셈이 무의미하다. 0 은 "판정 불가" 를
	 * 뜻하며, 호출자가 그것을 보고 출력을 생략한다. */
	if (*speed == PCI_SPEED_UNKNOWN || *width == PCIE_LNK_WIDTH_UNKNOWN)
		return 0;

	/* [한국어] 레인 수 x 레인당 실효 Mb/s. */
	return *width * PCIE_SPEED2MBS_ENC(*speed);
}

/**
 * __pcie_print_link_status - Report the PCI device's link speed and width
 * @dev: PCI device to query
 * @verbose: Print info even when enough bandwidth is available
 *
 * If the available bandwidth at the device is less than the device is
 * capable of, report the device's maximum possible bandwidth and the
 * upstream link that limits its performance.  If @verbose, always print
 * the available bandwidth, even if the device isn't constrained.
 */
/*
 * [한국어]
 * __pcie_print_link_status - 링크 대역폭을 로그에 남긴다 (병목이 있으면 지목한다)
 *
 * @dev:     대상 장치
 * @verbose: true 면 병목이 없어도 항상 출력, false 면 병목이 있을 때만 출력.
 * @return:  없음.
 *
 * 이 장치가 낼 수 있는 대역폭(capable)과 경로상 실제로 쓸 수 있는
 * 대역폭(available)을 비교해, 후자가 작으면 원인이 되는 장치와 함께 알린다.
 *
 * NVMe 학습에서 매우 실용적인 함수다. 드라이버가 부팅 중에 이것을 부르면
 * dmesg 에 다음과 같은 줄이 남는다:
 *
 *   "8.000 Gb/s available PCIe bandwidth, limited by 5.0 GT/s PCIe x2 link
 *    at 0000:00:1c.0 (capable of 31.504 Gb/s with 8.0 GT/s PCIe x4 link)"
 *
 * 즉 "이 SSD 는 Gen3 x4 (31.5 Gb/s)를 낼 수 있는데, 상위 포트가 Gen2 x2 라
 * 8 Gb/s 밖에 못 쓴다" 는 진단이다. 성능이 기대에 못 미칠 때 슬롯 배선이나
 * 스위치를 의심할 근거가 된다. (다만 drivers/nvme/ 는 이 함수를 부르지
 * 않는다 — 주로 네트워크 드라이버가 쓴다.)
 *
 * 출력 형식의 "%u.%03u Gb/s" 가 눈에 띈다. 값이 Mb/s 단위 정수인데
 * Gb/s 로 보여 주려면 1000 으로 나눠야 하지만, 커널에는 부동소수점이
 * 없다. 그래서 몫과 나머지를 각각 정수로 출력해 소수점을 흉내 낸다 —
 * 31504 -> "31.504". %03u 로 자리를 채워야 31504 가 "31.504" 가 되고
 * 31040 이 "31.4" 가 아니라 "31.040" 이 된다.
 *
 * Flit mode 는 PCIe 6.0 에서 도입된 새 패킷 형식이다. 오버헤드 계산이
 * 달라지므로 로그에 표시해 둔다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(config 를 여러 번 읽는다).
 * 호출자: pcie_print_link_status(), 그리고 verbose=false 로 부르는 드라이버들.
 */
void __pcie_print_link_status(struct pci_dev *dev, bool verbose)
{
	/* [한국어] _cap 이 붙은 것이 "이 장치의 능력", 안 붙은 것이 "경로상 실제". */
	enum pcie_link_width width, width_cap;
	enum pci_bus_speed speed, speed_cap;
	/* [한국어] 병목 지점의 장치. pcie_bandwidth_available 이 채워 준다. */
	struct pci_dev *limiting_dev = NULL;
	u32 bw_avail, bw_cap;	/* [한국어] Mb/s 단위 대역폭 */
	/* [한국어] 로그 끝에 덧붙일 문자열. 기본은 빈 문자열이라 아무것도 붙지 않는다. */
	char *flit_mode = "";

	/* [한국어] 이 장치 자신의 능력. */
	bw_cap = pcie_bandwidth_capable(dev, &speed_cap, &width_cap);
	/* [한국어] 루트까지 거슬러 올라가며 찾은 최소 대역폭과 그 지점. */
	bw_avail = pcie_bandwidth_available(dev, &limiting_dev, &speed, &width);

	/* [한국어] PCIe 6.0 Flit mode 여부. 오버헤드 계산이 달라 값 해석에
	 * 영향을 주므로 로그에 명시한다. */
	if (dev->bus && dev->bus->flit_mode)
		flit_mode = ", in Flit mode";

	/* [한국어] 병목이 없는 경우. verbose 일 때만 출력한다 — 정상 상태를
	 * 매번 찍으면 부팅 로그가 불필요하게 길어진다. */
	if (bw_avail >= bw_cap && verbose)
		pci_info(dev, "%u.%03u Gb/s available PCIe bandwidth (%s x%d link)%s\n",
			 /* [한국어] Mb/s 정수를 Gb/s "정수.소수3자리" 로. 커널에 부동소수점이
			  * 없어 몫과 나머지를 따로 출력한다. */
			 bw_cap / 1000, bw_cap % 1000,
			 pci_speed_string(speed_cap), width_cap, flit_mode);
	/* [한국어] 병목이 있는 경우. verbose 와 무관하게 항상 출력한다 —
	 * 이것은 사용자가 알아야 할 정보이기 때문이다. */
	else if (bw_avail < bw_cap)
		pci_info(dev, "%u.%03u Gb/s available PCIe bandwidth, limited by %s x%d link at %s (capable of %u.%03u Gb/s with %s x%d link)%s\n",
			 /* [한국어] 실제 대역폭과 그 지점의 속도/폭 */
			 bw_avail / 1000, bw_avail % 1000,
			 pci_speed_string(speed), width,
			 /* [한국어] 병목 장치의 주소(0000:00:1c.0 형식).
			  * NULL 이면 "<unknown>" — 모든 링크 읽기가 실패한 경우다. */
			 limiting_dev ? pci_name(limiting_dev) : "<unknown>",
			 /* [한국어] 대비를 위해 이 장치의 능력도 함께 보여 준다. */
			 bw_cap / 1000, bw_cap % 1000,
			 pci_speed_string(speed_cap), width_cap, flit_mode);
}

/**
 * pcie_print_link_status - Report the PCI device's link speed and width
 * @dev: PCI device to query
 *
 * Report the available bandwidth at the device.
 */
/*
 * [한국어]
 * pcie_print_link_status - 링크 대역폭을 항상 로그에 남긴다
 *
 * @dev: 대상 장치
 * @return: 없음.
 *
 * __pcie_print_link_status(dev, true) 의 래퍼. verbose=true 이므로 병목이
 * 없어도 출력한다.
 *
 * 드라이버가 probe 에서 이것을 부르면 "내가 얼마짜리 링크를 쓰고 있는지"
 * 가 dmesg 에 항상 남는다. 나중에 성능 문제를 조사할 때 그 줄 하나가
 * 결정적인 단서가 되므로, 대역폭이 중요한 장치의 드라이버는 대부분 이것을
 * 부른다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: 주로 고성능 네트워크 드라이버(mlx5, ice 등).
 *   drivers/nvme/ 에는 호출이 없다.
 */
void pcie_print_link_status(struct pci_dev *dev)
{
	/* [한국어] true = 병목이 없어도 출력. */
	__pcie_print_link_status(dev, true);
}
EXPORT_SYMBOL(pcie_print_link_status);

/**
 * pci_select_bars - Make BAR mask from the type of resource
 * @dev: the PCI device for which BAR mask is made
 * @flags: resource type mask to be selected
 *
 * This helper routine makes bar mask from the type of resource.
 */
/*
 * [한국어]
 * pci_select_bars - 조건에 맞는 BAR 들을 비트마스크로 모은다
 *
 * @dev:    대상 장치
 * @flags:  고를 기준. IORESOURCE_MEM, IORESOURCE_IO 등을 OR 로 조합한다.
 * @return: 비트 i 가 1 이면 i 번 자원이 조건에 맞는다는 뜻.
 *
 * pci_request_selected_regions()(:7343)처럼 "여러 BAR 을
 * 한꺼번에" 다루는 함수들이 인자로 받는 마스크를 만들어 준다. BAR 번호를
 * 손으로 세어 (1<<0)|(1<<4) 같은 상수를 쓰는 대신, "메모리 자원 전부" 라는
 * 의도를 그대로 표현할 수 있다.
 *
 * 루프가 PCI_NUM_RESOURCES 까지 도는 것에 주의. 이 값은 6(표준 BAR 개수)이
 * 아니라 그보다 크다 — ROM BAR, 브리지 윈도우, IOV BAR 까지 포함하기
 * 때문이다. 그래서 결과 마스크에는 BAR 0~5 외의 비트도 설 수 있다.
 *
 * NVMe 학습 관점: NVMe 드라이버는 이 함수를 직접 부르지 않는다(전수 확인).
 * pci_request_mem_regions(pdev) 라는 인라인 래퍼를 쓰는데, 그것이 내부에서
 * pci_select_bars(pdev, IORESOURCE_MEM) 를 부른다. NVMe 는 BAR0 만 쓰지만
 * (컨트롤러 레지스터와 도어벨), CMB 를 지원하는 컨트롤러는 BAR2/4 도 갖는다.
 *
 * 실행 컨텍스트: 제약 없음. 캐시된 resource[] 배열만 읽는다.
 * 호출자: pci_request_mem_regions() 계열 인라인 래퍼, 일부 드라이버.
 */
int pci_select_bars(struct pci_dev *dev, unsigned long flags)
{
	int i, bars = 0;	/* [한국어] bars = 만들어 갈 비트마스크 */
	/* [한국어] 표준 BAR 6개뿐 아니라 ROM/브리지 윈도우/IOV BAR 까지 모두 훑는다. */
	for (i = 0; i < PCI_NUM_RESOURCES; i++)
		/* [한국어] 이 자원의 플래그에 요청한 비트가 하나라도 있으면 채택.
		 * & 이므로 flags 에 여러 종류를 넣으면 그중 아무거나 맞으면 된다. */
		if (pci_resource_flags(dev, i) & flags)
			bars |= (1 << i);	/* [한국어] i 번 비트를 세운다 */
	return bars;
}
EXPORT_SYMBOL(pci_select_bars);

/* Some architectures require additional programming to enable VGA */
/* [한국어] 아키텍처가 등록해 둘 VGA 설정 콜백. 전역 함수 포인터 하나다.
 * 설정자: pci_register_set_vga_state() — 부팅 초기에 아키텍처 코드가 부른다.
 * 읽는 자: pci_set_vga_state_arch().
 * 값 범위: NULL(아키텍처가 추가 작업을 요구하지 않음) 또는 유효한 함수 포인터.
 * 동기화: __init 시점에 한 번 설정되고 이후 바뀌지 않아 보호가 필요 없다.
 * 왜 필요한가: 일부 아키텍처는 config space 비트만 바꿔서는 VGA 디코딩이
 *   켜지지 않고, 칩셋 고유의 레지스터를 함께 건드려야 한다. 그 부분을
 *   PCI 코어가 알 수 없으므로 훅으로 뺐다. */
static arch_set_vga_state_t arch_set_vga_state;

/*
 * [한국어]
 * pci_register_set_vga_state - 아키텍처별 VGA 설정 훅을 등록한다
 *
 * @func: 등록할 콜백. NULL 을 넘기면 훅이 해제된다.
 * @return: 없음.
 *
 * __init 이 붙어 있어 부팅이 끝나면 이 함수의 코드가 메모리에서 해제된다.
 * 즉 런타임에는 부를 수 없고, 아키텍처 초기화 시점에만 쓸 수 있다.
 *
 * 실행 컨텍스트: 부팅 초기, 단일 스레드.
 * 호출자: 아키텍처별 PCI 초기화 코드.
 */
void __init pci_register_set_vga_state(arch_set_vga_state_t func)
{
	arch_set_vga_state = func;	/* NULL disables */
}

/*
 * [한국어]
 * pci_set_vga_state_arch - 등록된 아키텍처 훅이 있으면 부른다
 *
 * @dev:          대상 장치
 * @decode:       true 면 VGA 디코딩 활성화, false 면 비활성화
 * @command_bits: PCI_COMMAND_IO / PCI_COMMAND_MEMORY 조합
 * @flags:        상위 브리지까지 함께 바꿀지 등의 옵션
 * @return: 0 = 성공(훅이 없는 경우 포함), 음수 = 아키텍처가 실패를 알림.
 *
 * 훅이 등록돼 있지 않으면 아무것도 하지 않고 0 을 돌려준다. "아키텍처가
 * 추가 작업을 요구하지 않는다" 는 것은 오류가 아니라 정상이기 때문이다.
 * 이 판정을 여기 한 곳에 모아 두어 호출자가 NULL 검사를 반복하지 않게 한다.
 *
 * 실행 컨텍스트: 훅 구현에 달렸지만, 호출자가 프로세스 컨텍스트다.
 * 호출자: pci_set_vga_state().
 */
static int pci_set_vga_state_arch(struct pci_dev *dev, bool decode,
				  unsigned int command_bits, u32 flags)
{
	/* [한국어] 훅이 있으면 위임하고, 없으면 성공으로 처리한다. */
	if (arch_set_vga_state)
		return arch_set_vga_state(dev, decode, command_bits,
						flags);
	return 0;
}

/**
 * pci_set_vga_state - set VGA decode state on device and parents if requested
 * @dev: the PCI device
 * @decode: true = enable decoding, false = disable decoding
 * @command_bits: PCI_COMMAND_IO and/or PCI_COMMAND_MEMORY
 * @flags: traverse ancestors and change bridges
 * CHANGE_BRIDGE_ONLY / CHANGE_BRIDGE
 */
/*
 * [한국어]
 * pci_set_vga_state - 이 장치를 향해 VGA 레거시 주소가 흐르도록 경로를 연다
 *
 * @dev:          VGA 장치
 * @decode:       true = 이 장치가 VGA 주소를 받게 한다, false = 막는다
 * @command_bits: PCI_COMMAND_IO / PCI_COMMAND_MEMORY 중 켤 것
 * @flags:        PCI_VGA_STATE_CHANGE_DECODES(장치 자신을 바꿀지),
 *                PCI_VGA_STATE_CHANGE_BRIDGE(경로상 브리지들도 바꿀지)
 * @return: 0 = 성공, -EIO = 브리지가 VGA Enable 을 지원하지 않음,
 *          그 외 = 아키텍처 훅의 오류.
 *
 * VGA 는 PCI 이전 시대의 유산이다. 그래픽 카드가 쓰는 고정 주소 대역
 * (I/O 0x3B0~0x3DF, 메모리 0xA0000~0xBFFFF)이 BAR 와 무관하게 하드와이어돼
 * 있어서, 그 주소로 가는 트랜잭션을 "누구에게 보낼지" 를 경로상의 모든
 * 브리지가 알아야 한다. 그 스위치가 Bridge Control 의 VGA Enable 비트다.
 *
 * 그래서 이 함수는 두 가지를 한다.
 *   1) 장치 자신의 Command 레지스터에서 IO/Memory 디코딩을 켠다.
 *   2) 루트까지 올라가며 모든 브리지의 VGA Enable 비트를 켠다.
 * 하나라도 빠지면 그래픽 출력이 나오지 않는다. 그래서 계층 전체를 훑는다.
 *
 * 화면에 무언가를 그리는 카드는 시스템에 하나만 활성일 수 있으므로,
 * 여러 GPU 가 있는 시스템에서 vgaarb(VGA arbiter)가 이 함수로 경로를
 * 이쪽저쪽으로 바꿔 준다.
 *
 * 되읽기 검사가 있는 이유: Bridge Control 의 VGA Enable 은 선택 기능이라
 * 지원하지 않는 브리지에서는 쓰기가 무시된다. 그런 브리지가 경로에 있으면
 * VGA 주소가 이 장치까지 도달하지 못하므로, 실패를 조기에 알려야 한다.
 * 켜는 방향(decode=true)에서만 확인하는 것은, 끄는 것은 실패해도
 * 치명적이지 않기 때문이다.
 *
 * NVMe 관점: NVMe 와는 무관한 함수다. 다만 "경로상의 모든 브리지를 설정해야
 * 한다" 는 구조는 MPS 설정이나 ACS 설정과 같은 패턴이라, PCIe 계층 구조를
 * 이해하는 데 좋은 예다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(config 접근).
 * 호출자: drivers/gpu/vga/vgaarb.c, GPU 드라이버.
 */
int pci_set_vga_state(struct pci_dev *dev, bool decode,
		      unsigned int command_bits, u32 flags)
{
	struct pci_bus *bus;		/* [한국어] 루트까지 올라갈 순회 커서 */
	struct pci_dev *bridge;		/* [한국어] 현재 버스를 만든 브리지 */
	u16 cmd;			/* [한국어] Command 또는 Bridge Control 값 */
	int rc;				/* [한국어] 아키텍처 훅의 결과 */

	/* [한국어] 디코딩을 바꾸겠다면서 IO/Memory 가 아닌 비트를 넘긴 것은
	 * 호출자의 버그다. Command 레지스터의 다른 비트(Bus Master 등)를
	 * 실수로 건드리면 장치가 오동작하므로 경고를 남긴다.
	 * ~(IO|MEMORY) 마스크에 걸리는 비트가 있는지 보는 방식이다. */
	WARN_ON((flags & PCI_VGA_STATE_CHANGE_DECODES) && (command_bits & ~(PCI_COMMAND_IO|PCI_COMMAND_MEMORY)));

	/* ARCH specific VGA enables */
	/* [한국어] 아키텍처가 추가로 할 일이 있으면 먼저 처리한다. 여기서
	 * 실패하면 config 를 건드리지 않고 물러난다 — 절반만 설정된 상태를
	 * 만들지 않기 위해서다. */
	rc = pci_set_vga_state_arch(dev, decode, command_bits, flags);
	if (rc)
		return rc;

	/* [한국어] 1단계 — 장치 자신의 디코딩 비트. 호출자가 요청한 경우에만. */
	if (flags & PCI_VGA_STATE_CHANGE_DECODES) {
		/* [한국어] read-modify-write. Command 에는 Bus Master 등 다른
		 * 설정도 있어 통째로 덮어쓰면 안 된다. */
		pci_read_config_word(dev, PCI_COMMAND, &cmd);
		if (decode)
			cmd |= command_bits;	/* [한국어] 요청한 디코딩을 켠다 */
		else
			cmd &= ~command_bits;	/* [한국어] 끈다 */
		pci_write_config_word(dev, PCI_COMMAND, cmd);
	}

	/* [한국어] 브리지까지 바꾸라는 요청이 없으면 여기서 끝. */
	if (!(flags & PCI_VGA_STATE_CHANGE_BRIDGE))
		return 0;

	/* [한국어] 2단계 — 이 장치가 붙은 버스에서 루트까지 올라가며
	 * 모든 브리지의 VGA Enable 을 설정한다. 하나라도 빠지면 VGA 주소가
	 * 중간에서 끊긴다. */
	bus = dev->bus;
	while (bus) {
		bridge = bus->self;	/* [한국어] 루트 버스면 NULL */
		if (bridge) {
			/* [한국어] Bridge Control(오프셋 0x3E)의 read-modify-write.
			 * 이 레지스터에는 Secondary Bus Reset 비트도 있어서
			 * 통째로 덮어쓰면 하위 버스가 리셋된다 — 반드시 읽고 고쳐야 한다. */
			pci_read_config_word(bridge, PCI_BRIDGE_CONTROL,
					     &cmd);
			if (decode)
				/* [한국어] VGA Enable(0x08). 이 브리지가 VGA 주소 범위를
				 * 하위 버스로 통과시킨다. */
				cmd |= PCI_BRIDGE_CTL_VGA;
			else
				cmd &= ~PCI_BRIDGE_CTL_VGA;
			/* [한국어] 고친 값을 되쓴다. Secondary Bus Reset 비트를
			 * 건드리지 않도록 반드시 읽은 값을 기반으로 해야 한다. */
			pci_write_config_word(bridge, PCI_BRIDGE_CONTROL,
					      cmd);


			/*
			 * VGA Enable may not be writable if bridge doesn't
			 * support it.
			 */
			/* [한국어] 되읽어 확인한다. VGA Enable 은 선택 기능이라
			 * 지원하지 않는 브리지에서는 쓰기가 조용히 무시된다.
			 * 그러면 경로가 끊긴 것이므로 -EIO 로 알린다.
			 * 끄는 방향은 실패해도 무해하므로 확인하지 않는다. */
			if (decode) {
				pci_read_config_word(bridge, PCI_BRIDGE_CONTROL,
						     &cmd);
				/* [한국어] 방금 쓴 비트가 실제로 서지 않았다면 이 브리지는
				 * VGA Enable 을 구현하지 않은 것이다. 경로가 끊겼으므로
				 * 더 올라가 봐야 소용없어 즉시 실패를 알린다. */
				if (!(cmd & PCI_BRIDGE_CTL_VGA))
					return -EIO;
			}
		}
		bus = bus->parent;	/* [한국어] 한 단계 위로. 루트에서 NULL 이 되어 끝난다 */
	}
	return 0;
}

/* [한국어] 아래 pci_pr3_present() 는 ACPI 테이블을 조회하므로 ACPI 지원이
 * 빌드에 포함된 커널에서만 존재한다. DT 시스템에는 _PR3 라는 개념 자체가 없다. */
#ifdef CONFIG_ACPI
/*
 * [한국어]
 * pci_pr3_present - 이 장치를 D3cold 로 보낼 수 있다고 펌웨어가 알려 주는가
 *
 * @pdev:   확인할 장치
 * @return: true = ACPI 가 _PR3 메서드를 제공한다(D3cold 가능),
 *          false = 없거나 ACPI 자체가 꺼져 있다.
 *
 * _PR3 는 ACPI 의 "Power Resources for D3hot" 메서드로, 이 장치를 D3 로
 * 보낼 때 어떤 전원 자원을 꺼야 하는지 펌웨어가 알려 주는 것이다.
 * 이것이 있어야 커널이 장치의 전원을 실제로 끊는 D3cold 까지 갈 수 있다.
 * 없으면 D3hot(장치 안의 로직만 꺼짐)이 한계다.
 *
 * 두 조건을 모두 확인한다.
 *   power_resources - ACPI 장치가 전원 자원을 갖고 있다고 선언했는가.
 *   _PR3 메서드     - 실제로 그 메서드가 존재하는가.
 * 둘 중 하나만 있으면 안 되므로 && 로 묶었다.
 *
 * PR3 를 "Power Reduction 3" 로 읽으면 안 된다 — Power Resources for D3 다.
 *
 * NVMe 학습 관점: 노트북의 NVMe SSD 를 D3cold 로 보내 배터리를 아끼는
 * 동작이 이 조건에 달려 있다. 다만 D3cold 에서 깨어나려면 전원을 다시
 * 넣고 링크를 재훈련해야 해서 수백 밀리초가 걸리므로, NVMe 는
 * 그 지연을 감수할 만큼 오래 쉴 때만 그리로 간다.
 * (drivers/nvme/ 에 이 함수 직접 호출은 없다 — PCI 코어의 전원 관리가
 *  판단에 쓴다.)
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. ACPI 조회가 잠들 수 있다.
 * 호출자: 전원 관리 정책 코드, 일부 GPU 드라이버.
 */
bool pci_pr3_present(struct pci_dev *pdev)
{
	struct acpi_device *adev;	/* [한국어] 이 PCI 장치에 대응하는 ACPI 노드 */

	/* [한국어] "acpi=off" 로 부팅했다면 물어볼 곳이 없다. */
	if (acpi_disabled)
		return false;

	/* [한국어] PCI 장치와 ACPI 노드를 잇는 매크로. 펌웨어가 이 장치를
	 * 기술하지 않았으면 NULL 이다(핫플러그로 꽂힌 장치 등). */
	adev = ACPI_COMPANION(&pdev->dev);
	if (!adev)
		return false;

	/* [한국어] 두 조건을 모두 만족해야 한다. 앞의 플래그는 "전원 자원을
	 * 선언했는가", 뒤는 "D3 용 메서드가 실제로 있는가". */
	return adev->power.flags.power_resources &&
		acpi_has_method(adev->handle, "_PR3");
}
EXPORT_SYMBOL_GPL(pci_pr3_present);
#endif

/**
 * pci_add_dma_alias - Add a DMA devfn alias for a device
 * @dev: the PCI device for which alias is added
 * @devfn_from: alias slot and function
 * @nr_devfns: number of subsequent devfns to alias
 *
 * This helper encodes an 8-bit devfn as a bit number in dma_alias_mask
 * which is used to program permissible bus-devfn source addresses for DMA
 * requests in an IOMMU.  These aliases factor into IOMMU group creation
 * and are useful for devices generating DMA requests beyond or different
 * from their logical bus-devfn.  Examples include device quirks where the
 * device simply uses the wrong devfn, as well as non-transparent bridges
 * where the alias may be a proxy for devices in another domain.
 *
 * IOMMU group creation is performed during device discovery or addition,
 * prior to any potential DMA mapping and therefore prior to driver probing
 * (especially for userspace assigned devices where IOMMU group definition
 * cannot be left as a userspace activity).  DMA aliases should therefore
 * be configured via quirks, such as the PCI fixup header quirk.
 */
/*
 * [한국어]
 * pci_add_dma_alias - 이 장치가 다른 devfn 으로도 DMA 를 낸다고 등록한다
 *
 * @dev:        문제의 장치
 * @devfn_from: 별칭 범위의 시작 devfn
 * @nr_devfns:  범위의 개수. 1 이면 devfn 하나만 추가한다.
 * @return: 없음. 실패해도 경고만 남기고 넘어간다.
 *
 * DMA 트랜잭션에는 발신자를 나타내는 requester ID(RID)가 붙고, IOMMU 는
 * 그것으로 어느 주소 공간을 쓸지 정한다. 그런데 일부 장치는 자기 devfn 이
 * 아닌 엉뚱한 값을 쓴다 — 하드웨어 버그이거나, 내부적으로 여러 function 이
 * 하나의 DMA 엔진을 공유하는 설계다.
 *
 * 그런 장치를 IOMMU 뒤에서 쓰면 DMA 가 전부 차단된다. IOMMU 는 등록되지
 * 않은 RID 에서 오는 접근을 막기 때문이다. 그래서 "이 장치는 이런 RID 로도
 * 나타난다" 를 미리 알려 줘야 하고, 그 등록이 이 함수다.
 *
 * 원문 주석이 중요한 제약을 밝힌다 — IOMMU 그룹은 장치 발견 시점에
 * 만들어지므로, 별칭 등록은 그보다 먼저 이뤄져야 한다. 그래서 드라이버가
 * probe 에서 부르면 이미 늦고, quirks.c 의 DECLARE_PCI_FIXUP_HEADER 로
 * 등록해야 한다.
 *
 * 마스크는 MAX_NR_DEVFNS(256) 비트짜리 비트맵이고, 필요할 때 처음 한 번만
 * 할당한다. 대부분의 장치는 별칭이 없어 이 포인터가 NULL 로 남는다.
 *
 * 할당 실패를 치명적으로 다루지 않는다는 점에 주의. 경고만 남기고 돌아간다.
 * 부팅 초기에 이만큼도 할당하지 못하는 상황이면 어차피 다른 데서 먼저
 * 죽을 것이고, 여기서 실패를 전파해 봐야 quirk 호출자가 할 수 있는 일이 없다.
 *
 * NVMe 학습 관점: NVMe SSD 자체에 이런 quirk 가 걸린 사례는 없다. 하지만
 * NVMe 가 PCIe-to-PCI 브리지 뒤에 있으면 브리지의 RID 로 바뀌는데,
 * 그것은 이 함수가 아니라 pci_for_each_dma_alias() 가 계층을 훑으며
 * 자동으로 처리한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(GFP_KERNEL 할당). 열거 중에 불린다.
 * 호출자: quirks.c 의 DMA alias fixup 들.
 */
void pci_add_dma_alias(struct pci_dev *dev, u8 devfn_from,
		       unsigned int nr_devfns)
{
	int devfn_to;	/* [한국어] 범위의 끝. 로그 출력에만 쓴다 */

	/* [한국어] 범위가 비트맵 밖으로 나가지 않게 자른다. devfn 은 8비트라
	 * 최대 256개이고, devfn_from 부터 남은 칸이 그보다 적을 수 있다.
	 * 이 clamp 가 없으면 bitmap_set 이 배열 밖을 건드린다. */
	nr_devfns = min(nr_devfns, (unsigned int)MAX_NR_DEVFNS - devfn_from);
	devfn_to = devfn_from + nr_devfns - 1;

	/* [한국어] 비트맵을 처음 쓰는 경우에만 할당한다. 별칭이 없는 장치는
	 * 이 포인터가 NULL 로 남아 메모리를 쓰지 않는다. zalloc 이라 0 으로 채워진다. */
	if (!dev->dma_alias_mask)
		dev->dma_alias_mask = bitmap_zalloc(MAX_NR_DEVFNS, GFP_KERNEL);
	/* [한국어] 방금 할당했든 원래 있었든, 여기서 NULL 이면 쓸 수 없다.
	 * 실패를 전파하지 않고 경고만 남기는 이유는 위 함수 주석 참고. */
	if (!dev->dma_alias_mask) {
		pci_warn(dev, "Unable to allocate DMA alias mask\n");
		return;
	}

	/* [한국어] devfn_from 부터 nr_devfns 개의 비트를 한 번에 세운다. */
	bitmap_set(dev->dma_alias_mask, devfn_from, nr_devfns);

	/* [한국어] 로그를 단수/복수로 나눠 찍는다. IOMMU 문제를 추적할 때
	 * 어떤 별칭이 등록됐는지가 결정적 단서라 반드시 남긴다.
	 * %02x.%d 는 슬롯.함수 표기다(devfn 을 PCI_SLOT/PCI_FUNC 로 쪼갠 것). */
	if (nr_devfns == 1)
		pci_info(dev, "Enabling fixed DMA alias to %02x.%d\n",
				PCI_SLOT(devfn_from), PCI_FUNC(devfn_from));
	/* [한국어] 범위 형태. 시작과 끝을 함께 보여 준다. */
	else if (nr_devfns > 1)
		pci_info(dev, "Enabling fixed DMA alias for devfn range from %02x.%d to %02x.%d\n",
				PCI_SLOT(devfn_from), PCI_FUNC(devfn_from),
				PCI_SLOT(devfn_to), PCI_FUNC(devfn_to));
	/* [한국어] nr_devfns 가 0 이면 아무것도 찍지 않는다. 위 clamp 로
	 * 0 이 될 수 있다(devfn_from 이 이미 경계인 경우). */
}

/*
 * [한국어]
 * pci_devs_are_dma_aliases - 두 장치가 DMA 관점에서 서로를 가리는 관계인가
 *
 * @dev1, @dev2: 비교할 두 장치
 * @return: true = 하나가 다른 하나의 RID 로 DMA 를 낼 수 있다.
 *
 * IOMMU 는 서로 별칭 관계인 장치들을 같은 그룹으로 묶어야 한다. 하나가
 * 다른 하나의 RID 를 쓸 수 있다면 두 장치를 서로 격리할 방법이 없기 때문이다.
 * 그 판정을 이 함수가 한다.
 *
 * 네 가지 경우를 OR 로 확인한다. 관계가 대칭적이지 않을 수 있어서
 * 양쪽 방향을 모두 봐야 한다.
 *   1) dev1 의 별칭 목록에 dev2 의 devfn 이 있는가
 *   2) dev2 의 별칭 목록에 dev1 의 devfn 이 있는가
 *   3) dev1 의 실제 DMA 주체가 dev2 인가
 *   4) dev2 의 실제 DMA 주체가 dev1 인가
 *
 * 1,2 는 quirk 로 등록한 devfn 별칭(pci_add_dma_alias)이고,
 * 3,4 는 아키텍처가 제공하는 다른 버스로의 별칭(pci_real_dma_dev)이다.
 * 두 메커니즘이 다르므로 둘 다 확인해야 한다.
 *
 * 마스크 검사에서 && 로 NULL 을 먼저 거르는 것에 주의. 대부분의 장치는
 * dma_alias_mask 가 NULL 이라 test_bit 이 아예 평가되지 않는다.
 *
 * 실행 컨텍스트: 제약 없음.
 * 호출자: IOMMU 코드의 그룹 구성(drivers/iommu/iommu.c).
 */
bool pci_devs_are_dma_aliases(struct pci_dev *dev1, struct pci_dev *dev2)
{
	return (dev1->dma_alias_mask &&
		/* [한국어] dev1 이 dev2 의 devfn 으로도 DMA 를 낸다고 등록돼 있는가.
		 * NULL 검사를 && 앞에 두어 마스크 없는 장치에서 역참조를 막는다. */
		test_bit(dev2->devfn, dev1->dma_alias_mask)) ||
	       (dev2->dma_alias_mask &&
		/* [한국어] 반대 방향. 관계가 한쪽에만 등록돼 있을 수 있다. */
		test_bit(dev1->devfn, dev2->dma_alias_mask)) ||
	       /* [한국어] 아키텍처가 정의한 "실제 DMA 주체" 관계. 다른 버스에
		* 걸친 별칭은 devfn 비트맵으로 표현할 수 없어 이 경로를 쓴다. */
	       pci_real_dma_dev(dev1) == dev2 ||
	       pci_real_dma_dev(dev2) == dev1;
}

/*
 * [한국어]
 * pci_device_is_present - 이 장치가 아직 거기 있는가
 *
 * @pdev:   확인할 장치
 * @return: true = 응답한다, false = 사라졌거나 이미 제거 표시가 됐다.
 *
 * 장치의 Vendor ID 를 읽어 all-ones 가 아닌지 본다. 응답 없는 장치를 읽으면
 * 버스가 all-ones 를 돌려주므로, 그것이 "없음" 의 신호가 된다
 * (access.c 의 PCI_SET_ERROR_RESPONSE 주석 참고).
 *
 * 두 단계로 확인한다.
 *   1) pci_dev_is_disconnected() — 커널이 이미 "제거됨" 으로 표시해 둔
 *      장치인가. 이 플래그는 hotplug 나 오류 처리 경로가 세운다.
 *      하드웨어를 건드리지 않고 즉시 판정할 수 있어 먼저 본다.
 *   2) 실제로 Vendor ID 를 읽어 본다.
 *
 * VF 를 PF 로 바꿔치기하는 첫 줄이 중요하다. SR-IOV 가상 함수(VF)의
 * Vendor/Device ID 는 스펙상 항상 0xFFFF 로 하드와이어돼 있다 — VF 는
 * PF 의 ID 를 물려받아 쓰므로 자기 ID 를 가질 필요가 없기 때문이다.
 * 그래서 VF 를 그대로 읽으면 살아 있는 장치도 "없음" 으로 판정된다.
 * pci_physfn() 이 VF 를 그 PF 로 바꿔 주고, PF 가 있으면 VF 도 있다.
 *
 * NVMe 학습 관점: NVMe 드라이버가 이 파일에서 직접 부르는 8개 함수 중
 * 하나다(전수 확인). 명령이 타임아웃됐을 때 "컨트롤러가 뽑혔는가,
 * 아니면 살아 있는데 응답만 늦는가" 를 가르는 데 쓴다. 뽑혔다면 복구를
 * 시도할 이유가 없으므로 곧바로 모든 I/O 를 실패 처리한다.
 *
 * 실행 컨텍스트: 제약 없음(config 읽기).
 * 호출자: drivers/nvme/host/pci.c 를 비롯한 여러 드라이버.
 */
bool pci_device_is_present(struct pci_dev *pdev)
{
	u32 v;	/* [한국어] 읽은 Vendor/Device ID. 값 자체는 쓰지 않는다 */

	/* Check PF if pdev is a VF, since VF Vendor/Device IDs are 0xffff */
	/* [한국어] VF 라면 PF 로 바꾼다. VF 의 ID 는 스펙상 항상 0xFFFF 라
	 * 그대로 읽으면 살아 있어도 "없음" 이 된다. PF 가 아니면 그대로 둔다. */
	pdev = pci_physfn(pdev);
	/* [한국어] 이미 제거 표시가 된 장치. 하드웨어를 건드리지 않고 판정한다 —
	 * 사라진 장치에 접근하는 것 자체가 위험하기 때문이다. */
	if (pci_dev_is_disconnected(pdev))
		return false;
	/* [한국어] 실제로 읽어 본다. 이 함수는 all-ones 인지 판정해
	 * true/false 를 돌려주므로 그대로 반환하면 된다. 마지막 인자 0 은
	 * "재시도하지 않음"(CRS 대기 없음)을 뜻한다. */
	return pci_bus_read_dev_vendor_id(pdev->bus, pdev->devfn, &v, 0);
}
EXPORT_SYMBOL_GPL(pci_device_is_present);

/*
 * [한국어]
 * pci_ignore_hotplug - 이 장치의 링크가 끊겨도 제거로 해석하지 말라고 표시한다
 *
 * @dev: 대상 장치
 * @return: 없음.
 *
 * 드라이버가 스스로 장치의 전원을 껐다 켜는 경우가 있다. 그러면 링크가
 * 잠시 끊기는데, 그것을 본 핫플러그 컨트롤러는 "카드가 뽑혔다" 고 판단해
 * 장치를 제거해 버린다. 드라이버 입장에서는 잠깐 재우려 했을 뿐인데
 * 장치 자체가 사라지는 것이다.
 *
 * 이 함수는 그 오해를 막는다. 플래그를 보고 pciehp 가 Presence Detect /
 * Data Link Layer State Changed 이벤트를 무시한다.
 *
 * 상위 브리지에도 함께 세우는 것이 중요하다. 링크 끊김을 실제로 감지하는
 * 주체가 하위 장치가 아니라 상위 포트(하류 포트)이기 때문이다. 그 포트의
 * 핫플러그 서비스가 이 플래그를 확인한다.
 *
 * 이 플래그는 한번 세우면 내려가지 않는다(해제 함수가 없다). 드라이버가
 * 바인딩된 동안 계속 유효하다는 뜻이며, 그래서 진짜 뽑힘도 함께 무시된다 —
 * 그 대가를 감수할 만한 드라이버만 이것을 쓴다.
 *
 * NVMe 관점: drivers/nvme/ 에는 호출이 없다(전수 확인). NVMe 는 D3 전환에
 * 링크를 끊지 않으므로 필요가 없다. 주로 GPU 드라이버가 런타임 절전으로
 * 외장 GPU 전원을 끊을 때 쓴다.
 *
 * 실행 컨텍스트: 제약 없음. 플래그 대입뿐이다.
 * 호출자: nouveau, radeon 등 GPU 드라이버.
 */
void pci_ignore_hotplug(struct pci_dev *dev)
{
	/* [한국어] 링크 끊김을 실제로 감지하는 상위 포트. 루트 버스 직결이면 NULL. */
	struct pci_dev *bridge = dev->bus->self;

	dev->ignore_hotplug = 1;	/* [한국어] 장치 자신에게 표시 */
	/* Propagate the "ignore hotplug" setting to the parent bridge. */
	/* [한국어] 상위 브리지에도 전파. 실제로 이벤트를 받는 쪽이 여기라
	 * 이 줄이 없으면 위 표시가 아무 효과를 내지 못한다. */
	if (bridge)
		bridge->ignore_hotplug = 1;
}
EXPORT_SYMBOL_GPL(pci_ignore_hotplug);

/**
 * pci_real_dma_dev - Get PCI DMA device for PCI device
 * @dev: the PCI device that may have a PCI DMA alias
 *
 * Permits the platform to provide architecture-specific functionality to
 * devices needing to alias DMA to another PCI device on another PCI bus. If
 * the PCI device is on the same bus, it is recommended to use
 * pci_add_dma_alias(). This is the default implementation. Architecture
 * implementations can override this.
 */
/*
 * [한국어]
 * pci_real_dma_dev - 이 장치를 대신해 DMA 를 내는 장치를 알려 준다
 *
 * @dev:    조회할 장치
 * @return: 실제 DMA 주체. 기본 구현은 자기 자신을 그대로 돌려준다.
 *
 * __weak 훅이다. 기본 동작은 항등 함수 — 대부분의 장치는 자기 이름으로
 * DMA 를 내기 때문이다.
 *
 * 아키텍처가 이것을 덮어쓰는 경우는 "다른 버스에 있는 장치가 대신 DMA 를
 * 내는" 구조일 때다. pci_add_dma_alias() 는 같은 버스 안의 devfn 별칭만
 * 표현할 수 있어서(비트맵의 인덱스가 devfn 이다), 버스를 건너뛰는 관계는
 * 이 훅으로 표현한다. 원문 주석이 그 구분을 밝힌다 — 같은 버스면
 * pci_add_dma_alias() 를 쓰라고 권한다.
 *
 * 실제로 이것을 덮어쓰는 곳은 Intel VMD 드라이버다. VMD 뒤의 장치들은
 * VMD 컨트롤러의 RID 로 DMA 를 내므로, 그 매핑을 여기서 알려 준다.
 *
 * NVMe 학습 관점: VMD 는 인텔 플랫폼에서 NVMe SSD 여러 개를 하나의
 * 엔드포인트 뒤에 숨기는 기능이다. VMD 가 켜진 시스템에서는 NVMe 가
 * 별도의 PCI 도메인에 나타나고, 그 DMA 의 실제 주체는 VMD 컨트롤러다.
 * IOMMU 가 그것을 알아야 매핑을 올바른 RID 에 걸 수 있다.
 *
 * 실행 컨텍스트: 제약 없음.
 * 호출자: pci_devs_are_dma_aliases(), IOMMU 코드.
 */
struct pci_dev __weak *pci_real_dma_dev(struct pci_dev *dev)
{
	/* [한국어] 기본 동작 — 자기 자신이 DMA 주체다. */
	return dev;
}

/*
 * [한국어]
 * pcibios_default_alignment - 아키텍처가 요구하는 BAR 정렬 하한
 *
 * @return: 바이트 단위 정렬값. 0 = 추가 요구 없음(기본).
 *
 * __weak 훅이다. 기본은 0 이며, 그러면 BAR 는 자기 크기만큼만 정렬된다
 * (PCI 스펙의 요구). 아키텍처가 그보다 큰 정렬을 요구하면 이것을 덮어쓴다.
 *
 * 왜 더 큰 정렬이 필요한가. IOMMU 가 페이지 단위로만 매핑을 걸 수 있는
 * 플랫폼에서는, 한 페이지 안에 두 장치의 BAR 가 섞이면 그 둘을 서로
 * 격리할 수 없다. VFIO 로 장치를 게스트에 넘길 때 치명적인 문제가 되므로,
 * 그런 플랫폼은 BAR 를 페이지 경계에 맞춘다.
 *
 * 실행 컨텍스트: 제약 없음.
 * 호출자: pci_specified_resource_alignment() 가 사용자 지정 정렬과 비교할 때.
 */
resource_size_t __weak pcibios_default_alignment(void)
{
	/* [한국어] 0 = "아키텍처가 추가로 요구하는 정렬이 없다". */
	return 0;
}

/*
 * Arches that don't want to expose struct resource to userland as-is in
 * sysfs and /proc can implement their own pci_resource_to_user().
 */
/*
 * [한국어]
 * pci_resource_to_user - 자원의 시작/끝 주소를 userspace 에 보일 형태로 바꾼다
 *
 * @dev:   대상 장치
 * @bar:   자원 번호
 * @rsrc:  커널이 들고 있는 자원
 * @start: 사용자에게 보일 시작 주소를 담을 곳
 * @end:   사용자에게 보일 끝 주소를 담을 곳
 * @return: 없음.
 *
 * __weak 훅이며, 기본 구현은 그대로 복사한다. 즉 커널이 보는 주소와
 * userspace 가 보는 주소가 같다.
 *
 * 위 원문 주석이 존재 이유를 밝힌다 — struct resource 의 값을 그대로
 * 노출하고 싶지 않은 아키텍처가 자기 판을 제공할 수 있게 한 것이다.
 * 예컨대 sparc 는 sysfs 와 /proc/bus/pci 에 CPU 물리 주소가 아니라
 * PCI 버스 주소를 보여 준다. 그 플랫폼의 userspace 도구들이 그렇게
 * 기대하도록 오래전에 굳어졌기 때문이다.
 *
 * 실행 컨텍스트: 제약 없음.
 * 호출자: pci-sysfs.c 의 resource 속성, proc.c 의 /proc/bus/pci.
 */
void __weak pci_resource_to_user(const struct pci_dev *dev, int bar,
				 const struct resource *rsrc,
				 resource_size_t *start, resource_size_t *end)
{
	/* [한국어] 기본 동작 — 변환 없이 그대로 보여 준다. */
	*start = rsrc->start;
	*end = rsrc->end;
}

/* [한국어] "pci=resource_alignment=..." 부팅 인자 또는 sysfs 로 들어온 문자열.
 * 설정자: pci_setup() 이 부팅 인자를 파싱할 때, 그리고 sysfs 의
 *   resource_alignment 속성 쓰기(resource_alignment_store).
 * 읽는 자: pci_specified_resource_alignment().
 * 값 범위: NULL 또는 "12@0000:01:00.0" 형식의 문자열. 앞의 숫자가 정렬
 *   차수(2^n), @ 뒤가 장치 지정자다. 여러 개를 세미콜론으로 이을 수 있다.
 * 동기화: 아래 spinlock 이 보호한다. sysfs 로 런타임에 바뀔 수 있어서
 *   읽는 쪽과 쓰는 쪽이 경쟁하기 때문이다. */
static char *resource_alignment_param;
/* [한국어] 위 문자열 포인터를 보호하는 스핀락.
 * sysfs 쓰기가 옛 문자열을 kfree 하고 새것을 대입하는 사이에,
 * 다른 CPU 가 그 포인터를 따라가면 해제된 메모리를 읽는다.
 * 뮤텍스가 아니라 스핀락인 것은 임계 구역이 문자열 파싱뿐이라 짧고,
 * 그 안에서 잠들 일이 없기 때문이다. */
static DEFINE_SPINLOCK(resource_alignment_lock);

/**
 * pci_specified_resource_alignment - get resource alignment specified by user.
 * @dev: the PCI device to get
 * @resize: whether or not to change resources' size when reassigning alignment
 *
 * RETURNS: Resource alignment if it is specified.
 *          Zero if it is not specified.
 */
/*
 * [한국어]
 * pci_specified_resource_alignment - 사용자가 이 장치에 지정한 BAR 정렬을 찾는다
 *
 * @dev:    조회할 장치
 * @resize: 사용자 지정이 발견되면 true 로 세워 돌려준다. 즉 "크기를 키워서라도
 *          정렬을 맞추라" 는 신호다.
 * @return: 바이트 단위 정렬값. 지정이 없으면 아키텍처 기본값(대개 0).
 *
 * "pci=resource_alignment=12@0000:01:00.0" 같은 부팅 인자를 해석한다.
 * 이 기능이 왜 필요한가 — VFIO 로 장치를 게스트에 넘길 때, BAR 가 페이지
 * 경계에 맞지 않으면 한 페이지에 두 장치의 레지스터가 섞여 격리가 깨진다.
 * 그런 장치를 강제로 페이지 정렬시켜 통과시키는 것이 이 옵션의 목적이다.
 *
 * 문자열 형식: "<차수>@<장치지정자>" 를 세미콜론으로 이은 것.
 *   차수는 2의 지수다 — 12 면 2^12 = 4096바이트(x86 의 페이지 크기).
 *   @ 앞의 숫자를 생략하면 PAGE_SHIFT 가 기본값이 된다.
 *   장치 지정자는 "0000:01:00.0" 또는 "pci:8086:1234" 형식이며,
 *   그 해석은 pci_dev_str_match() 가 담당한다.
 *
 * 파싱 실패를 다루는 방식이 관대하다. 차수가 63 을 넘으면(1ULL 시프트가
 * 정의되지 않는 범위) 오류를 찍되 PAGE_SHIFT 로 대체하고 계속 진행한다.
 * 부팅 인자 하나 잘못 썼다고 부팅을 막을 이유가 없기 때문이다.
 *
 * PCI_PROBE_ONLY 플래그가 있으면 아예 무시한다. 그 모드는 "펌웨어가 정한
 * 자원 배치를 그대로 쓰고 커널은 재배치하지 않는다" 는 뜻이라, 정렬을
 * 바꾸겠다는 요청 자체가 성립하지 않는다.
 *
 * 실행 컨텍스트: 스핀락을 쥔 채 문자열을 파싱한다. 잠들 수 없다.
 * 호출자: pci_reassigndev_resource_alignment().
 */
static resource_size_t pci_specified_resource_alignment(struct pci_dev *dev,
							bool *resize)
{
	/* [한국어] align_order = 파싱한 2의 지수, count = sscanf 가 소비한 문자 수 */
	int align_order, count;
	/* [한국어] 기본값은 아키텍처가 요구하는 정렬. 사용자 지정이 없으면 이 값이 그대로 나간다. */
	resource_size_t align = pcibios_default_alignment();
	const char *p;	/* [한국어] 파싱 커서 */
	int ret;	/* [한국어] pci_dev_str_match 의 결과 */

	/* [한국어] resource_alignment_param 포인터를 읽는 동안 sysfs 쓰기가
	 * 그것을 해제하지 못하게 막는다. 문자열 전체를 다 쓸 때까지 쥐고 있어야 한다. */
	spin_lock(&resource_alignment_lock);
	p = resource_alignment_param;
	/* [한국어] NULL 이거나 빈 문자열이면 지정이 없다. 기본값으로 나간다. */
	if (!p || !*p)
		goto out;
	/* [한국어] 펌웨어 배치를 그대로 쓰는 모드에서는 재정렬이 성립하지 않는다.
	 * 0 을 돌려 "정렬 요구 없음" 으로 만들고, 왜 무시했는지 한 번만 알린다. */
	if (pci_has_flag(PCI_PROBE_ONLY)) {
		align = 0;
		pr_info_once("PCI: Ignoring requested alignments (PCI_PROBE_ONLY)\n");
		goto out;
	}

	/* [한국어] 세미콜론으로 이어진 항목들을 앞에서부터 훑는다. */
	while (*p) {
		count = 0;
		/* [한국어] "<숫자>@" 형식인지 본다. %n 은 여기까지 읽은 문자 수를
		 * count 에 담는 지시자로, 그 다음 글자가 '@' 인지 확인하는 데 쓴다.
		 * 두 조건을 && 로 묶어야 "12abc" 같은 것이 통과하지 않는다. */
		if (sscanf(p, "%d%n", &align_order, &count) == 1 &&
		    p[count] == '@') {
			p += count + 1;	/* [한국어] 숫자와 '@' 를 건너뛴다 */
			/* [한국어] 1ULL << 64 이상은 정의되지 않은 동작이다.
			 * 오류를 알리되 부팅을 막지 않고 페이지 크기로 대체한다. */
			if (align_order > 63) {
				pr_err("PCI: Invalid requested alignment (order %d)\n",
				       align_order);
				align_order = PAGE_SHIFT;
			}
		} else {
			/* [한국어] 차수를 생략한 형식("0000:01:00.0" 만 쓴 경우).
			 * 기본값은 페이지 크기다 — VFIO 용도가 대부분이라 그것이 자연스럽다. */
			align_order = PAGE_SHIFT;
		}

		/* [한국어] 남은 문자열이 이 장치를 가리키는지 확인하고, 커서를
		 * 다음 항목으로 옮긴다(p 를 갱신해 준다).
		 * 반환: 1 = 일치, 0 = 불일치, 음수 = 형식 오류. */
		ret = pci_dev_str_match(dev, p, &p);
		if (ret == 1) {
			/* [한국어] 이 장치에 대한 지정을 찾았다. 크기 확장을 허용하고
			 * 차수를 실제 바이트 수로 바꾼다. 1ULL 인 것이 중요하다 —
			 * 1 << 32 는 32비트에서 넘치지만 1ULL << 32 는 안전하다. */
			*resize = true;
			align = 1ULL << align_order;
			break;
		} else if (ret < 0) {
			/* [한국어] 형식이 깨졌다. 뒤쪽도 신뢰할 수 없으므로 중단한다. */
			pr_err("PCI: Can't parse resource_alignment parameter: %s\n",
			       p);
			break;
		}

		/* [한국어] 항목 구분자 확인. pci_dev_str_match() 가 커서를 이 항목의
		 * 끝까지 옮겨 놓았으므로, 여기 있어야 할 것은 다음 항목을 잇는
		 * 세미콜론이나 쉼표, 아니면 문자열의 끝(널)이다.
		 * 그 셋 중 아무것도 아니면 형식이 깨진 것이고, 널이어도 여기서
		 * 걸려 루프가 끝난다 — 두 경우를 한 조건으로 처리하는 것이 요령이다. */
		if (*p != ';' && *p != ',') {
			/* End of param or invalid format */
			break;
		}
		p++;	/* [한국어] 구분자를 건너뛰어 다음 항목의 첫 글자로 */
	}
out:
	/* [한국어] 문자열 사용이 끝났으니 락을 놓는다. 이 시점 이후로는
	 * resource_alignment_param 이 해제되어도 상관없다. */
	spin_unlock(&resource_alignment_lock);
	/* [한국어] 일치를 찾았으면 지정된 정렬, 아니면 진입 시의 기본값
	 * (pcibios_default_alignment(), 보통 0)이 그대로 나간다. */
	return align;
}

/*
 * [한국어]
 * pci_request_resource_alignment - BAR 하나가 원하는 정렬을 갖도록 자원을 손본다
 *
 * @dev:    대상 장치
 * @bar:    자원 번호
 * @align:  원하는 정렬(바이트)
 * @resize: true 면 크기를 키우는 방법, false 면 시작 주소를 지정하는 방법.
 * @return: 없음.
 *
 * 아직 주소를 배정하지 않은 상태에서 struct resource 를 고쳐, 나중에
 * setup-bus.c 가 자리를 정할 때 원하는 정렬로 배치되게 만든다.
 * 실제 배치는 이 함수가 하지 않는다 — 요청만 기록해 두는 것이다.
 *
 * 아래 긴 영어 주석이 두 방법의 장단점을 설명한다. 요약하면:
 *   방법 1(크기 확장) - BAR 는 자기 크기만큼 정렬되므로, 크기를 정렬값까지
 *     키우면 자동으로 그 정렬을 얻는다. 그 구간을 통째로 차지하므로 다른
 *     BAR 가 같은 페이지에 들어오지 못한다. 단점은 자원 크기가 실제
 *     하드웨어 BAR 보다 커져서, 크기를 기준으로 계산하는 드라이버가
 *     깨질 수 있다는 것이다.
 *   방법 2(시작 주소 지정) - 크기는 그대로 두고 IORESOURCE_STARTALIGN 으로
 *     정렬만 요구한다. 그 자체로는 다른 BAR 가 끼어드는 것을 막지 못하지만,
 *     시스템의 모든 자원을 같은 방식으로 정렬하면 결과적으로 아무도 겹치지
 *     않는다.
 *
 * 그래서 선택 기준이 resize 다. 사용자가 특정 장치만 지정했다면(부팅 인자에
 * 장치 지정자가 있었다면) 그 장치만 확실히 격리해야 하므로 방법 1 을 쓰고,
 * 아키텍처 기본값으로 전부 정렬하는 경우라면 방법 2 로 충분하다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 열거 중에 불린다.
 * 호출자: pci_reassigndev_resource_alignment().
 */
static void pci_request_resource_alignment(struct pci_dev *dev, int bar,
					   resource_size_t align, bool resize)
{
	struct resource *r = &dev->resource[bar];	/* [한국어] 손볼 자원 */
	/* [한국어] 로그에 쓸 자원 이름("BAR 0", "ROM" 등). */
	const char *r_name = pci_resource_name(dev, bar);
	resource_size_t size;	/* [한국어] 현재 자원 크기 */

	/* [한국어] 메모리 자원만 대상이다. I/O 포트 공간은 정렬 개념이 다르고
	 * IOMMU 격리와도 무관하다. 빈 자원(flags==0)도 여기서 걸러진다. */
	if (!(r->flags & IORESOURCE_MEM))
		return;

	/* [한국어] IORESOURCE_PCI_FIXED 는 "이 자원은 옮길 수 없다" 는 표시다.
	 * 펌웨어가 고정했거나 quirk 가 못박은 자원이라 재배치 자체가 불가능하다.
	 * 조용히 넘어가지 않고 로그를 남기는 것은, 사용자가 부팅 인자로
	 * 요청했는데 왜 안 먹히는지 알 수 있게 하기 위해서다. */
	if (r->flags & IORESOURCE_PCI_FIXED) {
		pci_info(dev, "%s %pR: ignoring requested alignment %#llx\n",
			 r_name, r, (unsigned long long)align);
		return;
	}

	size = resource_size(r);
	/* [한국어] 이미 요청보다 크면 할 일이 없다. BAR 는 자기 크기만큼
	 * 정렬되므로, 크기가 align 이상이면 정렬도 자동으로 그 이상이다.
	 * (BAR 크기는 항상 2의 거듭제곱이라 이 논리가 성립한다.) */
	if (size >= align)
		return;

	/*
	 * Increase the alignment of the resource.  There are two ways we
	 * can do this:
	 *
	 * 1) Increase the size of the resource.  BARs are aligned on their
	 *    size, so when we reallocate space for this resource, we'll
	 *    allocate it with the larger alignment.  This also prevents
	 *    assignment of any other BARs inside the alignment region, so
	 *    if we're requesting page alignment, this means no other BARs
	 *    will share the page.
	 *
	 *    The disadvantage is that this makes the resource larger than
	 *    the hardware BAR, which may break drivers that compute things
	 *    based on the resource size, e.g., to find registers at a
	 *    fixed offset before the end of the BAR.
	 *
	 * 2) Retain the resource size, but use IORESOURCE_STARTALIGN and
	 *    set r->start to the desired alignment.  By itself this
	 *    doesn't prevent other BARs being put inside the alignment
	 *    region, but if we realign *every* resource of every device in
	 *    the system, none of them will share an alignment region.
	 *
	 * When the user has requested alignment for only some devices via
	 * the "pci=resource_alignment" argument, "resize" is true and we
	 * use the first method.  Otherwise we assume we're aligning all
	 * devices and we use the second.
	 */

	/* [한국어] 무엇을 왜 바꾸는지 남긴다. 자원 크기가 하드웨어 BAR 와
	 * 달라질 수 있으므로, 나중에 이상해 보일 때 이 줄이 설명이 된다. */
	pci_info(dev, "%s %pR: requesting alignment to %#llx\n",
		 r_name, r, (unsigned long long)align);

	if (resize) {
		/* [한국어] 방법 1 — 크기를 정렬값까지 키운다.
		 * start=0, end=align-1 로 두면 크기가 정확히 align 이 된다.
		 * start 를 0 으로 만드는 것은 "아직 주소 미정" 의 관용적 표현이고,
		 * 아래 IORESOURCE_UNSET 과 함께 그 뜻이 확정된다.
		 * BAR 가 자기 크기만큼 정렬되는 성질 덕분에, 이 크기로 배치하면
		 * 자동으로 align 경계에 놓이고 그 구간을 독점한다. */
		r->start = 0;
		r->end = align - 1;
	} else {
		/* [한국어] 방법 2 — 크기는 유지하고 정렬만 요구한다.
		 * SIZEALIGN("크기만큼 정렬")을 끄고 STARTALIGN("start 필드에
		 * 적힌 값만큼 정렬")로 바꾼다. 두 플래그는 배타적이라 반드시
		 * 하나를 끄고 다른 하나를 켜야 한다. */
		r->flags &= ~IORESOURCE_SIZEALIGN;
		r->flags |= IORESOURCE_STARTALIGN;
		/* [한국어] STARTALIGN 모드에서는 start 필드가 주소가 아니라
		 * "요구 정렬" 을 담는다. 그래서 align 을 start 자리에 넣는다.
		 * 크기는 원래 값을 그대로 유지한다. */
		resource_set_range(r, align, size);
	}
	/* [한국어] "아직 실제 주소가 배정되지 않았다" 는 표시.
	 * 두 방법 모두 자원의 내용을 요청으로 바꿔 놓았으므로, 이 플래그가
	 * 있어야 setup-bus.c 가 이것을 배치 대상으로 인식한다.
	 * 이 줄이 없으면 위에서 넣은 값이 실제 주소로 오해된다. */
	r->flags |= IORESOURCE_UNSET;
}

/*
 * This function disables memory decoding and releases memory resources
 * of the device specified by kernel's boot parameter 'pci=resource_alignment='.
 * It also rounds up size to specified alignment.
 * Later on, the kernel will assign page-aligned memory resource back
 * to the device.
 */
/*
 * [한국어]
 * pci_reassigndev_resource_alignment - 이 장치의 BAR 들을 재배치 대상으로 표시한다
 *
 * @dev: 대상 장치
 * @return: 없음.
 *
 * "pci=resource_alignment=" 로 지정된 장치의 메모리 자원을 모두 놓아 주고,
 * 원하는 정렬을 요구 사항으로 기록해 둔다. 실제 재배치는 나중에
 * setup-bus.c 가 한다.
 *
 * 위 원문 주석이 흐름을 요약한다 — 메모리 디코딩을 끄고, 자원을 놓고,
 * 크기를 정렬값까지 올림한 뒤, 커널이 나중에 정렬된 주소를 다시 배정한다.
 *
 * 메모리 디코딩을 먼저 끄는 것이 순서상 중요하다. 자원을 놓아 준 뒤에도
 * 하드웨어 BAR 에는 옛 주소가 남아 있는데, 디코딩이 켜져 있으면 그 주소로
 * 가는 접근을 이 장치가 계속 받아들인다. 그 구간이 곧 다른 장치에게
 * 배정되면 두 장치가 같은 주소에 응답하는 충돌이 생긴다.
 *
 * 세 가지 예외를 걸러 낸다.
 *   1) VF — SR-IOV 가상 함수의 BAR 는 스펙상 읽기 전용 0 이고, 실제 주소는
 *      PF 의 SR-IOV capability 안 VF BAR 로 정해진다. 여기서 손댈 수 없다.
 *   2) 호스트 브리지 — 자원을 놓아 주면 시스템 전체의 주소 공간이 사라진다.
 *   3) 정렬 요구가 없는 장치(align == 0).
 *
 * 브리지라면 윈도우 자원까지 함께 놓아 준다. 하위 장치의 BAR 가 커지거나
 * 위치가 바뀌면 그것을 감싸는 브리지 윈도우도 다시 계산되어야 하기 때문이다.
 * 윈도우를 그대로 두면 새 주소가 그 창 밖으로 나가 접근이 닿지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 열거 직후에 불린다.
 * 호출자: probe.c 의 pci_device_add() 경로.
 */
void pci_reassigndev_resource_alignment(struct pci_dev *dev)
{
	int i;			/* [한국어] 자원 인덱스 */
	struct resource *r;	/* [한국어] 브리지 윈도우를 가리킬 포인터 */
	resource_size_t align;	/* [한국어] 요구 정렬. 0 이면 요구 없음 */
	u16 command;		/* [한국어] Command 레지스터 값 */
	/* [한국어] pci_specified_resource_alignment 가 채워 줄 값.
	 * true 면 "크기를 키워서라도 정렬하라"(방법 1), false 면 방법 2. */
	bool resize = false;

	/*
	 * VF BARs are read-only zero according to SR-IOV spec r1.1, sec
	 * 3.4.1.11.  Their resources are allocated from the space
	 * described by the VF BARx register in the PF's SR-IOV capability.
	 * We can't influence their alignment here.
	 */
	/* [한국어] VF 는 자기 BAR 를 갖지 않는다. 주소가 PF 의 SR-IOV
	 * capability 로 결정되므로 여기서 바꿀 수단이 없다. */
	if (dev->is_virtfn)
		return;

	/* check if specified PCI is target device to reassign */
	/* [한국어] 사용자가 이 장치를 지정했는지 확인한다. 0 이면 지정이 없거나
	 * 아키텍처 기본 요구도 없다는 뜻이라 아무것도 하지 않는다. */
	align = pci_specified_resource_alignment(dev, &resize);
	if (!align)
		return;

	/* [한국어] 호스트 브리지는 시스템 전체 주소 공간의 소유자다. 그 자원을
	 * 놓아 주면 그 아래 모든 장치가 갈 곳을 잃는다. 사용자가 실수로
	 * 지정했을 가능성이 높으므로 경고를 남긴다.
	 * hdr_type 이 NORMAL 인데 class 가 HOST 브리지인 특이한 조합을 보는
	 * 이유는, 호스트 브리지가 브리지 헤더가 아니라 일반 헤더를 쓰기 때문이다. */
	if (dev->hdr_type == PCI_HEADER_TYPE_NORMAL &&
	    (dev->class >> 8) == PCI_CLASS_BRIDGE_HOST) {
		pci_warn(dev, "Can't reassign resources to host bridge\n");
		return;
	}

	/* [한국어] 메모리 디코딩을 끈다. 자원을 놓아 준 뒤에도 하드웨어 BAR 에는
	 * 옛 주소가 남으므로, 디코딩이 켜져 있으면 그 구간을 물려받은 다른
	 * 장치와 충돌한다. read-modify-write 로 다른 비트는 보존한다. */
	pci_read_config_word(dev, PCI_COMMAND, &command);
	command &= ~PCI_COMMAND_MEMORY;
	pci_write_config_word(dev, PCI_COMMAND, command);

	/* [한국어] 표준 BAR 6개와 ROM BAR 까지(PCI_ROM_RESOURCE 가 6이라 <= 로
	 * 7개를 돈다) 정렬 요구를 기록한다. 메모리가 아닌 자원은 피호출자가
	 * 알아서 걸러 낸다. */
	for (i = 0; i <= PCI_ROM_RESOURCE; i++)
		pci_request_resource_alignment(dev, i, align, resize);

	/*
	 * Need to disable bridge's resource window,
	 * to enable the kernel to reassign new resource
	 * window later on.
	 */
	/* [한국어] 이 장치가 브리지라면 윈도우 자원도 놓아 준다.
	 * 하위 BAR 가 재배치되면 그것을 감싸는 창도 다시 계산되어야 하는데,
	 * 옛 창을 그대로 두면 새 주소가 창 밖으로 나가 접근이 닿지 않는다. */
	if (dev->hdr_type == PCI_HEADER_TYPE_BRIDGE) {
		/* [한국어] 브리지 윈도우 자원들(PCI_BRIDGE_RESOURCES 이후)을 훑는다. */
		for (i = PCI_BRIDGE_RESOURCES; i < PCI_NUM_RESOURCES; i++) {
			r = &dev->resource[i];
			/* [한국어] I/O 윈도우는 대상이 아니다(메모리 정렬만 다룬다). */
			if (!(r->flags & IORESOURCE_MEM))
				continue;
			/* [한국어] "주소 미정" 으로 표시하고, 크기는 유지한 채
			 * start 를 0 으로 되돌린다. end 를 먼저 계산하는 순서가
			 * 중요하다 — resource_size(r) 는 start 를 쓰므로, start 를
			 * 먼저 0 으로 만들면 크기가 틀어진다. */
			r->flags |= IORESOURCE_UNSET;
			r->end = resource_size(r) - 1;
			r->start = 0;
		}
		/* [한국어] 하드웨어의 윈도우 레지스터도 무효화한다. 자원 구조체만
		 * 놓아 주고 하드웨어를 그대로 두면, 브리지가 옛 주소 범위를
		 * 계속 하위로 통과시켜 충돌이 난다. */
		pci_disable_bridge_window(dev);
	}
}

/*
 * [한국어]
 * resource_alignment_show - 현재 설정된 정렬 파라미터를 sysfs 로 보여 준다
 *
 * @bus:    pci_bus_type. 버스 단위 속성이라 장치가 아니라 버스를 받는다.
 * @buf:    출력 버퍼(PAGE_SIZE 크기)
 * @return: 쓴 바이트 수. 설정이 없으면 0.
 *
 * /sys/bus/pci/resource_alignment 를 읽으면 이 함수가 불린다.
 *
 * 스핀락으로 문자열을 보호하는 것이 요점이다. 읽는 도중에
 * resource_alignment_store() 가 옛 문자열을 kfree 하면 해제된 메모리를
 * 복사하게 된다. sysfs_emit 이 락 안에서 복사를 마치므로 안전하다.
 *
 * sysfs_emit 은 sprintf 대신 쓰는 sysfs 전용 헬퍼로, 버퍼 경계를 자동으로
 * 지키고 PAGE_SIZE 를 넘지 않도록 잘라 준다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(sysfs 읽기). 스핀락 안에서는 잠들지 않는다.
 * 호출자: sysfs 코어.
 */
static ssize_t resource_alignment_show(const struct bus_type *bus, char *buf)
{
	size_t count = 0;	/* [한국어] 설정이 없으면 0 바이트를 쓴 것으로 남는다 */

	/* [한국어] 문자열을 복사하는 동안 store 가 그것을 해제하지 못하게 막는다. */
	spin_lock(&resource_alignment_lock);
	if (resource_alignment_param)
		count = sysfs_emit(buf, "%s\n", resource_alignment_param);
	spin_unlock(&resource_alignment_lock);

	/* [한국어] 설정이 없으면 0 이 그대로 나가 빈 파일로 보인다. */
	return count;
}

/*
 * [한국어]
 * resource_alignment_store - sysfs 로 정렬 파라미터를 바꾼다
 *
 * @bus:    pci_bus_type
 * @buf:    사용자가 쓴 문자열
 * @count:  그 길이
 * @return: 소비한 바이트 수(=count), 또는 음수 errno.
 *
 * 부팅 인자로 주던 값을 런타임에 바꿀 수 있게 해 준다. 다만 이미 배치가
 * 끝난 장치에는 소급 적용되지 않는다 — 이후 새로 발견되거나 재스캔되는
 * 장치부터 적용된다.
 *
 * 메모리 수명 관리가 이 함수의 핵심이다. 순서가 정확해야 한다.
 *   1) 락 밖에서 새 문자열을 복사한다(GFP_KERNEL 할당은 잠들 수 있으므로
 *      락 안에서 하면 안 된다).
 *   2) 락 안에서 옛 포인터를 old 에 챙기고 새 포인터로 교체한다.
 *   3) 락 밖에서 옛 문자열을 해제한다.
 * 3번을 락 안에서 해도 되지만, 락 구간을 짧게 유지하는 편이 낫다.
 * 반대로 2번 없이 곧바로 kfree 하면, 그 순간 show 나
 * pci_specified_resource_alignment 가 그 포인터를 읽고 있을 수 있다.
 *
 * 빈 문자열을 쓰면 설정이 해제된다. 그때는 방금 할당한 param 을 바로
 * 해제하고 NULL 을 넣는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(sysfs 쓰기).
 * 호출자: sysfs 코어.
 */
static ssize_t resource_alignment_store(const struct bus_type *bus,
					const char *buf, size_t count)
{
	/* [한국어] param = 새로 만들 문자열, old = 교체 전 것(나중에 해제),
	 * end = 개행 위치 */
	char *param, *old, *end;

	/* [한국어] sysfs 버퍼는 한 페이지다. 널 종료 자리를 남겨야 하므로
	 * PAGE_SIZE - 1 을 넘으면 거절한다. */
	if (count >= (PAGE_SIZE - 1))
		return -EINVAL;

	/* [한국어] 락을 잡기 전에 할당한다. GFP_KERNEL 은 잠들 수 있어
	 * 스핀락 안에서 쓰면 안 된다. kstrndup 은 복사 후 널 종료까지 해 준다. */
	param = kstrndup(buf, count, GFP_KERNEL);
	if (!param)
		return -ENOMEM;

	/* [한국어] echo 로 쓰면 끝에 개행이 붙는다. 파서가 그것을 장치 지정자의
	 * 일부로 오해하지 않도록 잘라 낸다. */
	end = strchr(param, '\n');
	if (end)
		*end = '\0';

	/* [한국어] 포인터 교체 구간만 락으로 보호한다. */
	spin_lock(&resource_alignment_lock);
	old = resource_alignment_param;	/* [한국어] 옛것을 챙겨 둔다 */
	if (strlen(param)) {
		resource_alignment_param = param;	/* [한국어] 교체 */
	} else {
		/* [한국어] 빈 문자열 = 설정 해제. 방금 만든 것은 쓸 데가 없으므로
		 * 여기서 바로 해제한다(락 안이지만 이 포인터는 아직 공개되지
		 * 않아 아무도 보고 있지 않다). */
		kfree(param);
		resource_alignment_param = NULL;
	}
	spin_unlock(&resource_alignment_lock);

	/* [한국어] 옛 문자열 해제. 락을 놓은 뒤에 하는 이유는 락 구간을
	 * 짧게 유지하기 위해서다. 이 시점에는 전역 포인터가 이미 바뀌었으므로
	 * 아무도 old 를 새로 읽지 않는다. */
	kfree(old);

	return count;	/* [한국어] 전부 소비했다고 알린다 */
}

/* [한국어] 위 show/store 를 /sys/bus/pci/resource_alignment 속성으로 묶는다.
 * BUS_ATTR_RW 는 이름으로부터 resource_alignment_show/_store 를 찾아
 * struct bus_attribute bus_attr_resource_alignment 를 만드는 매크로다.
 * 그래서 함수 이름이 정확히 <속성명>_show/_store 여야 한다. */
static BUS_ATTR_RW(resource_alignment);

/*
 * [한국어]
 * pci_resource_alignment_sysfs_init - resource_alignment sysfs 파일을 만든다
 *
 * @return: 0 = 성공, 음수 = sysfs 파일 생성 실패.
 *
 * /sys/bus/pci/resource_alignment 를 만드는 초기화 함수다.
 *
 * late_initcall 로 등록한 것에 이유가 있다. 이 파일을 만들려면
 * pci_bus_type 이 이미 등록돼 있어야 하는데, 그것은 postcore_initcall
 * 단계에서 이뤄진다. late_initcall 은 그보다 한참 뒤라 순서가 보장된다.
 *
 * __init 이 붙어 있어 부팅이 끝나면 이 함수의 코드는 해제된다.
 *
 * 실행 컨텍스트: 부팅 중, 단일 스레드.
 */
static int __init pci_resource_alignment_sysfs_init(void)
{
	/* [한국어] pci_bus_type 아래에 속성 파일 하나를 만든다. */
	return bus_create_file(&pci_bus_type,
					&bus_attr_resource_alignment);
}
/* [한국어] 부팅 후반부에 위 함수를 실행하도록 등록한다. pci_bus_type 등록
 * (postcore_initcall)보다 뒤여야 하므로 late 단계를 골랐다. */
late_initcall(pci_resource_alignment_sysfs_init);

/*
 * [한국어]
 * pci_no_domains - PCI 도메인 지원을 런타임에 끈다
 *
 * @return: 없음.
 *
 * "pci=nodomains" 부팅 인자가 있을 때 불린다. 도메인(세그먼트)은 서로
 * 독립된 PCI 버스 번호 공간이며, 한 시스템에 256개 버스로 부족할 때
 * 여러 벌을 두는 방식이다. 장치 주소가 "0000:01:00.0" 처럼 네 자리
 * 도메인으로 시작하는 것이 그것이다.
 *
 * 왜 끄고 싶은가. 도메인 번호가 붙으면 옛 userspace 도구나 스크립트가
 * 장치 이름을 파싱하지 못하는 경우가 있었다. 그런 호환성 문제를
 * 우회하려고 남겨 둔 스위치다. 도메인이 실제로 여러 개인 시스템에서
 * 이것을 쓰면 장치를 구분할 수 없게 되므로 위험하다.
 *
 * #ifdef 로 감싼 것에 주의. CONFIG_PCI_DOMAINS 가 꺼진 커널에는
 * pci_domains_supported 변수 자체가 없다. 그래도 함수는 정의되어야
 * 호출부(pci_setup)를 #ifdef 로 지저분하게 만들지 않는다 —
 * 그 경우 이 함수는 빈 함수가 되어 컴파일러가 지운다.
 *
 * 실행 컨텍스트: 부팅 중 인자 파싱 시점, 단일 스레드.
 * 호출자: pci_setup().
 */
static void pci_no_domains(void)
{
#ifdef CONFIG_PCI_DOMAINS
	/* [한국어] 전역 플래그를 내린다. pci_domain_nr() 이 이 값을 보고
	 * 항상 0 을 돌려주게 되어, 모든 장치가 도메인 0 에 있는 것처럼 보인다. */
	pci_domains_supported = 0;
#endif
}

#ifdef CONFIG_PCI_DOMAINS
/* [한국어] 동적으로 배정하는 도메인 번호의 할당자.
 * IDA(ID Allocator)는 "쓰이지 않는 가장 작은 정수" 를 찾아 주는 커널
 * 자료구조로, 비트맵을 기수 트리로 관리해 희소한 범위도 효율적으로 다룬다.
 * 설정자/읽는 자: pci_bus_find_emul_domain_nr(), of_pci_bus_find_domain_nr(),
 *   pci_bus_release_emul_domain_nr().
 * 동기화: IDA 자체가 내부 락을 갖고 있어 별도 보호가 필요 없다.
 * 왜 필요한가: 도메인 번호는 시스템 전체에서 유일해야 한다. 호스트 브리지
 *   드라이버들이 각자 번호를 정하면 충돌하므로, 한곳에서 발급한다. */
static DEFINE_IDA(pci_domain_nr_dynamic_ida);

/**
 * pci_bus_find_emul_domain_nr() - allocate a PCI domain number per constraints
 * @hint: desired domain, 0 if any ID in the range of @min to @max is acceptable
 * @min: minimum allowable domain
 * @max: maximum allowable domain, no IDs higher than INT_MAX will be returned
 */
/*
 * [한국어]
 * pci_bus_find_emul_domain_nr - 범위 안에서 빈 도메인 번호를 하나 얻는다
 *
 * @hint: 이 번호를 우선 원한다는 힌트. 0 이면 아무거나 상관없다.
 * @min:  허용 하한
 * @max:  허용 상한(INT_MAX 를 넘는 값은 나오지 않는다)
 * @return: 배정된 도메인 번호, 또는 음수 errno(범위에 빈 번호가 없거나 할당 실패).
 *
 * 실제 하드웨어가 아니라 소프트웨어가 만들어 낸 PCI 계층 — 하이퍼바이저의
 * 가상 버스, Thunderbolt 터널, Intel VMD 뒤의 내부 도메인 등 — 에 번호를
 * 발급한다. 함수 이름의 emul 이 그 뜻이다.
 *
 * hint 를 min 으로 승격시키는 max(hint, min) 이 이 함수의 요령이다.
 * ida_alloc_range 는 하한부터 위로 훑으며 빈 번호를 찾으므로, 하한을
 * hint 로 올려 두면 "가능하면 hint, 이미 쓰였으면 그 다음 빈 번호" 가 된다.
 * hint 가 0(요청 없음)이면 max 가 min 을 고르므로 원래 하한이 유지된다.
 *
 * NVMe 학습 관점: Intel VMD 가 켜진 시스템에서 NVMe SSD 들이 별도 도메인에
 * 나타나는데, 그 도메인 번호를 VMD 드라이버가 이 함수로 받아 온다.
 * lspci 에 "10000:00:00.0" 처럼 큰 도메인 번호가 보이는 이유다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(GFP_KERNEL 할당).
 * 호출자: controller/vmd.c, 하이퍼바이저용 가상 버스 드라이버.
 */
int pci_bus_find_emul_domain_nr(u32 hint, u32 min, u32 max)
{
	/* [한국어] max(hint, min) 으로 하한을 올려 hint 를 우선 시도하게 만든다.
	 * IDA 가 하한부터 위로 훑으므로, hint 가 비어 있으면 그것이 배정된다. */
	return ida_alloc_range(&pci_domain_nr_dynamic_ida, max(hint, min), max,
			       GFP_KERNEL);
}
EXPORT_SYMBOL_GPL(pci_bus_find_emul_domain_nr);

/*
 * [한국어]
 * pci_bus_release_emul_domain_nr - 배정받은 도메인 번호를 돌려준다
 *
 * @domain_nr: 반납할 번호. pci_bus_find_emul_domain_nr() 이 돌려준 값이어야 한다.
 * @return: 없음.
 *
 * 가상 PCI 계층이 사라질 때(드라이버 언로드, Thunderbolt 장치 분리 등)
 * 번호를 풀어 다음 사용자가 쓸 수 있게 한다. 반납하지 않으면 번호가
 * 영구히 소진되어, 장치를 반복해서 꽂았다 뺐다 하면 결국 고갈된다.
 *
 * 배정받지 않은 번호를 반납하면 IDA 가 경고를 띄운다 — 짝을 맞추는 것은
 * 호출자의 책임이다.
 *
 * 실행 컨텍스트: 제약 없음(ida_free 는 잠들지 않는다).
 * 호출자: pci_bus_find_emul_domain_nr() 을 부른 그 드라이버.
 */
void pci_bus_release_emul_domain_nr(int domain_nr)
{
	/* [한국어] IDA 에서 해당 번호의 비트를 지운다. 다음 할당에서 재사용된다. */
	ida_free(&pci_domain_nr_dynamic_ida, domain_nr);
}
EXPORT_SYMBOL_GPL(pci_bus_release_emul_domain_nr);
#endif

#ifdef CONFIG_PCI_DOMAINS_GENERIC
/* [한국어] DeviceTree 가 명시적으로 지정한 도메인 번호의 할당자.
 * 동적 IDA 와 별도로 두는 이유는 두 번호 공간의 성격이 다르기 때문이다 —
 * 이쪽은 "DT 가 이미 정해 둔 번호" 라 우리가 고를 여지가 없고,
 * 같은 번호가 DT 에 두 번 나오는 오류를 잡아내는 것이 목적이다.
 * 동기화: IDA 내부 락. */
static DEFINE_IDA(pci_domain_nr_static_ida);

/*
 * [한국어]
 * of_pci_reserve_static_domain_nr - DT 에 적힌 도메인 번호들을 동적 풀에서 빼 둔다
 *
 * @return: 없음.
 *
 * DeviceTree 전체를 훑어 "pci" 타입 노드들의 linux,pci-domain 속성을 읽고,
 * 거기 적힌 번호를 동적 IDA 에 영구 할당해 버린다. 반납하지 않으므로
 * 그 번호는 동적 배정 후보에서 영원히 제외된다.
 *
 * 왜 이렇게 하는가. DT 에 도메인 0 과 2 가 적혀 있고 3 번째 호스트 브리지가
 * DT 지정 없이 등장했다고 하자. 동적 배정이 아무 대비 없이 가장 작은
 * 빈 번호를 고르면 0 을 줄 텐데, 그것은 DT 가 이미 다른 브리지에 준
 * 번호다. 그 브리지가 아직 등록되지 않았을 뿐이라 충돌을 알아채지도
 * 못한다. 미리 빼 두면 그런 사고가 원천적으로 없어진다.
 *
 * 부팅 중 한 번만 실행되며, 그 보장은 호출자(of_pci_bus_find_domain_nr)의
 * static 플래그가 한다.
 *
 * ida_alloc_range 의 반환값을 확인하지 않는 것이 눈에 띄는데, 실패해도
 * 할 수 있는 일이 없기 때문이다 — DT 에 중복된 번호가 있으면 두 번째
 * 할당이 실패하지만, 그것은 DT 자체의 오류라 커널이 고칠 수 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(GFP_KERNEL). 부팅 중 첫 브리지 등록 시.
 * 호출자: of_pci_bus_find_domain_nr() — 첫 호출에서 한 번만.
 */
static void of_pci_reserve_static_domain_nr(void)
{
	struct device_node *np;	/* [한국어] DT 노드 순회 커서 */
	int domain_nr;

	/* [한국어] device_type = "pci" 인 모든 DT 노드를 훑는다. */
	for_each_node_by_type(np, "pci") {
		/* [한국어] linux,pci-domain 속성을 읽는다. 없으면 음수. */
		domain_nr = of_get_pci_domain_nr(np);
		/* [한국어] 도메인을 지정하지 않은 노드는 동적 배정 대상이므로 건너뛴다. */
		if (domain_nr < 0)
			continue;
		/*
		 * Permanently allocate domain_nr in dynamic_ida
		 * to prevent it from dynamic allocation.
		 */
		/* [한국어] min=max=domain_nr 로 그 번호 하나만 콕 집어 할당한다.
		 * 반납하지 않으므로 동적 배정에서 영구히 제외된다.
		 * 반환값을 보지 않는 이유는 위 함수 주석 참고. */
		ida_alloc_range(&pci_domain_nr_dynamic_ida,
				domain_nr, domain_nr, GFP_KERNEL);
	}
}

/*
 * [한국어]
 * of_pci_bus_find_domain_nr - DeviceTree 기반 시스템에서 도메인 번호를 정한다
 *
 * @parent: 호스트 브리지의 struct device. DT 노드를 여기서 얻는다. NULL 가능.
 * @return: 배정된 도메인 번호, 또는 음수 errno.
 *
 * 두 갈래로 갈린다.
 *   DT 에 linux,pci-domain 이 적혀 있으면 - 그 번호를 static IDA 에 할당한다.
 *     번호는 이미 정해져 있으므로 고르는 것이 아니라 "중복이 없는지 확인" 하는
 *     것이 목적이다. DT 에 같은 번호가 두 번 나오면 두 번째가 실패해 오류를
 *     알린다(원문 주석의 "duplicate static allocations in case of errors in DT").
 *   지정이 없으면 - dynamic IDA 에서 빈 번호를 고른다. 이때 DT 가 쓰는 번호는
 *     of_pci_reserve_static_domain_nr() 이 미리 빼 두었으므로 충돌하지 않는다.
 *
 * 첫 호출에서만 DT 전체를 훑는다. static 지역 변수로 그 한 번을 기억하는데,
 * 부팅 중 호스트 브리지 등록은 직렬화되어 있어 이 플래그에 락이 없어도 된다.
 *
 * 두 IDA 를 나눠 쓰는 구조가 처음에는 헷갈린다. 정리하면 —
 *   dynamic IDA: 실제 배정 풀. DT 번호도 여기에 "예약" 되어 빠져 있다.
 *   static IDA:  DT 번호의 중복 검사 전용. 배정과는 무관하다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(GFP_KERNEL).
 * 호출자: pci_bus_find_domain_nr().
 */
static int of_pci_bus_find_domain_nr(struct device *parent)
{
	/* [한국어] DT 스캔을 한 번만 하기 위한 표시. 부팅 중 브리지 등록이
	 * 직렬화되어 있어 락 없이도 안전하다. */
	static bool static_domains_reserved = false;
	int domain_nr;

	/* On the first call scan device tree for static allocations. */
	/* [한국어] 첫 호출에서 DT 전체를 훑어 지정된 번호들을 동적 풀에서 뺀다.
	 * 이것을 먼저 해야 아래 동적 배정이 DT 번호와 충돌하지 않는다. */
	if (!static_domains_reserved) {
		of_pci_reserve_static_domain_nr();
		static_domains_reserved = true;
	}

	if (parent) {
		/*
		 * If domain is in DT, allocate it in static IDA.  This
		 * prevents duplicate static allocations in case of errors
		 * in DT.
		 */
		/* [한국어] 이 브리지의 DT 노드에 도메인이 적혀 있는가. */
		domain_nr = of_get_pci_domain_nr(parent->of_node);
		if (domain_nr >= 0)
			/* [한국어] 적힌 번호를 그대로 쓴다. static IDA 에 할당하는 것은
			 * 배정이 아니라 중복 검사가 목적이다 — 같은 번호가 두 번
			 * 요청되면 여기서 -ENOSPC 가 나 DT 오류를 드러낸다. */
			return ida_alloc_range(&pci_domain_nr_static_ida,
					       domain_nr, domain_nr,
					       GFP_KERNEL);
	}

	/*
	 * If domain was not specified in DT, choose a free ID from dynamic
	 * allocations. All domain numbers from DT are permanently in
	 * dynamic allocations to prevent assigning them to other DT nodes
	 * without static domain.
	 */
	/* [한국어] DT 지정이 없다. 동적 풀에서 가장 작은 빈 번호를 받는다.
	 * DT 가 쓰는 번호는 이미 빠져 있으므로 안전하다. */
	return ida_alloc(&pci_domain_nr_dynamic_ida, GFP_KERNEL);
}

/*
 * [한국어]
 * of_pci_bus_release_domain_nr - 도메인 번호를 원래 받은 IDA 로 돌려준다
 *
 * @parent:     호스트 브리지의 struct device. 어느 IDA 에서 받았는지 판정하는 근거.
 * @domain_nr:  반납할 번호. 음수면 아무것도 하지 않는다.
 * @return: 없음.
 *
 * 어려운 점은 "이 번호를 어느 IDA 에서 받았는가" 를 알아내는 것이다.
 * 번호만 봐서는 알 수 없으므로, 할당 때와 같은 판정을 다시 한다 —
 * DT 에 이 번호가 적혀 있었다면 static IDA 에서 받았고, 아니면 dynamic 이다.
 * 엉뚱한 IDA 에 반납하면 그쪽 번호 공간이 오염되고, 원래 쪽은 영원히
 * 그 번호를 잃는다.
 *
 * domain_nr < 0 검사는 할당 실패 후의 정리 경로를 위한 것이다.
 * 할당이 실패하면 음수 errno 가 domain_nr 에 남는데, 호출자가 성공 여부를
 * 따지지 않고 그냥 반납을 부를 수 있게 여기서 걸러 준다.
 *
 * 실행 컨텍스트: 제약 없음.
 * 호출자: pci_bus_release_domain_nr().
 */
static void of_pci_bus_release_domain_nr(struct device *parent, int domain_nr)
{
	/* [한국어] 할당이 실패했던 경우(음수 errno). 반납할 것이 없다. */
	if (domain_nr < 0)
		return;

	/* Release domain from IDA where it was allocated. */
	/* [한국어] 할당 때와 같은 판정을 반복해 어느 IDA 인지 알아낸다.
	 * DT 에 이 번호가 적혀 있었다면 static, 아니면 dynamic 이다. */
	if (parent && of_get_pci_domain_nr(parent->of_node) == domain_nr)
		ida_free(&pci_domain_nr_static_ida, domain_nr);
	else
		/* [한국어] DT 지정이 없었으므로 동적 풀에서 받은 번호다. */
		ida_free(&pci_domain_nr_dynamic_ida, domain_nr);
}

/*
 * [한국어]
 * pci_bus_find_domain_nr - 이 버스가 쓸 도메인 번호를 정한다 (펌웨어 종류별 분기)
 *
 * @bus:    대상 버스. ACPI 경로에서 쓴다.
 * @parent: 호스트 브리지의 struct device. DT 경로에서 쓴다.
 * @return: 도메인 번호, 또는 음수 errno.
 *
 * 시스템이 어떤 펌웨어를 쓰느냐에 따라 도메인 번호의 출처가 다르다.
 *   ACPI 시스템(x86 서버 대부분) - MCFG 테이블의 PCI Segment Group 번호를
 *     쓴다. 펌웨어가 이미 정해 둔 값이라 커널이 고를 여지가 없다.
 *   DT 시스템(ARM 임베디드 대부분) - linux,pci-domain 속성이 있으면 그것을,
 *     없으면 커널이 빈 번호를 고른다.
 *
 * acpi_disabled 하나로 갈라지는 것이 조금 거칠어 보이지만, 한 시스템이
 * 두 방식을 동시에 쓰는 경우가 없어서 이것으로 충분하다.
 *
 * 두 인자 중 하나씩만 쓰인다는 점이 특이하다 — ACPI 경로는 bus 를,
 * DT 경로는 parent 를 본다. 호출자는 둘 다 넘기고 어느 쪽이 쓰일지는
 * 이 함수가 정한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: probe.c 의 pci_create_root_bus() 경로.
 */
int pci_bus_find_domain_nr(struct pci_bus *bus, struct device *parent)
{
	/* [한국어] ACPI 가 꺼져 있으면 DT 시스템으로 본다. 삼항 연산자로
	 * 두 경로 중 하나만 실행된다. */
	return acpi_disabled ? of_pci_bus_find_domain_nr(parent) :
			       acpi_pci_bus_find_domain_nr(bus);
}

/*
 * [한국어]
 * pci_bus_release_domain_nr - 도메인 번호를 반납한다 (DT 시스템에서만)
 *
 * @parent:    호스트 브리지의 struct device
 * @domain_nr: 반납할 번호
 * @return: 없음.
 *
 * pci_bus_find_domain_nr() 의 짝이지만 대칭적이지 않다. ACPI 시스템에서는
 * 아무것도 하지 않고 돌아간다.
 *
 * 이유는 ACPI 쪽 번호가 애초에 "할당" 이 아니기 때문이다. MCFG 테이블에
 * 펌웨어가 적어 둔 Segment Group 번호를 읽어 온 것뿐이라, 커널이 관리하는
 * 자원이 아니고 따라서 반납할 대상도 없다. 반면 DT 시스템에서는 커널이
 * IDA 로 번호를 발급했으므로 반드시 돌려줘야 한다.
 *
 * 실행 컨텍스트: 제약 없음.
 * 호출자: 호스트 브리지 제거 경로(pci_remove_root_bus 등).
 */
void pci_bus_release_domain_nr(struct device *parent, int domain_nr)
{
	/* [한국어] ACPI 시스템이면 반납할 것이 없다. 펌웨어가 정한 번호를
	 * 읽어 왔을 뿐 커널이 발급한 것이 아니기 때문이다. */
	if (!acpi_disabled)
		return;
	/* [한국어] DT 시스템 — IDA 로 발급받은 번호이므로 돌려준다. */
	of_pci_bus_release_domain_nr(parent, domain_nr);
}
#endif

/**
 * pci_ext_cfg_avail - can we access extended PCI config space?
 *
 * Returns 1 if we can access PCI extended config space (offsets
 * greater than 0xff). This is the default implementation. Architecture
 * implementations can override this.
 */
/*
 * [한국어]
 * pci_ext_cfg_avail - 확장 config space(오프셋 0x100 이상)에 접근할 수 있는가
 *
 * @return: 1 = 접근 가능(기본), 0 = 불가능.
 *
 * PCI 시절 config space 는 256바이트였고, PCIe 가 그것을 4096바이트로
 * 늘렸다. 그 뒤쪽 3840바이트에 확장 capability 들이 놓인다 — AER, VC,
 * ACS, SR-IOV, ATS, DPC, PASID 가 모두 거기 있다.
 *
 * 문제는 접근 방법이 다르다는 것이다. 레거시 0xCF8/0xCFC 포트 방식은
 * 오프셋을 8비트로만 표현할 수 있어 256바이트까지밖에 못 간다.
 * ECAM(메모리 매핑) 방식이어야 4096바이트 전체에 닿는다. 그래서 ECAM 을
 * 설정하지 못한 시스템에서는 확장 capability 가 아예 보이지 않는다.
 *
 * __weak 기본 구현은 1(가능)을 돌려준다. 대부분의 현대 시스템이 ECAM 을
 * 쓰기 때문이다. x86 은 이것을 덮어써서 MMCONFIG 설정 여부를 실제로 확인한다.
 *
 * NVMe 학습 관점: 이것이 0 이면 NVMe 의 여러 기능이 조용히 사라진다.
 * SR-IOV(확장 capability 0x0010)로 VF 를 만들 수 없고, AER 로 오류를
 * 보고받지 못하며, ATS/PASID 기반 기능도 쓸 수 없다. NVMe 자체는
 * BAR0 로 동작하므로 기본 I/O 는 되지만, 고급 기능이 전부 빠진 상태가 된다.
 *
 * 실행 컨텍스트: 제약 없음.
 * 호출자: probe.c 가 확장 capability 를 훑기 전에 확인한다.
 */
int __weak pci_ext_cfg_avail(void)
{
	/* [한국어] 기본은 "가능". ECAM 을 못 쓰는 아키텍처가 덮어쓴다. */
	return 1;
}

/*
 * [한국어]
 * pci_setup - "pci=" 커널 부팅 인자를 해석한다
 *
 * @str: "pci=" 뒤에 붙은 문자열 전체. 쉼표로 여러 옵션을 이을 수 있다.
 *       예: "pci=nomsi,realloc,pcie_bus_perf"
 * @return: 항상 0. early_param 규약상 0 이 "처리했다" 는 뜻이다.
 *
 * PCI 서브시스템의 거의 모든 전역 정책이 여기서 정해진다. 파일 여기저기의
 * 전역 변수(pcie_bus_config, pci_hotplug_*_size, resource_alignment_param
 * 등)를 실제로 세팅하는 유일한 지점이라, 그 변수들의 출처를 찾을 때
 * 반드시 들르게 되는 함수다.
 *
 * 구조는 단순한 문자열 매칭 사슬이다. 다만 몇 가지 요령이 있다.
 *
 *   1) 쉼표 자르기 — strchr 로 쉼표를 찾아 그 자리를 '\0' 으로 바꾸고,
 *      다음 항목의 시작 주소를 k 에 챙긴다. 원본 문자열을 직접 고치므로
 *      추가 할당이 없다. 부팅 초기에는 메모리 할당기가 아직 없을 수 있어
 *      이런 제자리 처리가 필요하다.
 *
 *   2) 아키텍처 우선 처리 — pcibios_setup(str) 을 먼저 부른다. 아키텍처가
 *      그 옵션을 알아보면 NULL 을 돌려주고, 모르면 문자열을 그대로 돌려준다.
 *      즉 아키텍처 고유 옵션이 공통 옵션보다 우선한다.
 *      `(str = pcibios_setup(str))` 로 대입과 검사를 한 식에 넣은 것이
 *      처음에는 눈에 걸리지만, str 을 갱신해야 아래 비교가 맞아서 그렇다.
 *
 *   3) strcmp vs strncmp — 정확히 일치해야 하는 옵션(nomsi, noaer)은
 *      strcmp 를, 뒤에 값이 붙는 옵션(realloc=, hpiosize=)은 strncmp 를 쓴다.
 *      "realloc=" 를 "realloc" 보다 먼저 검사하는 순서도 그래서 중요하다 —
 *      순서가 반대면 "realloc=off" 가 "realloc" 에 먼저 걸려 값이 무시된다.
 *
 * NVMe 학습에 직접 관계있는 옵션들:
 *   nomsi           - MSI/MSI-X 를 전역으로 끈다. NVMe 가 INTx 로 떨어져
 *                     큐당 인터럽트를 못 쓰게 되므로 성능이 크게 나빠진다.
 *                     문제 격리용 디버그 옵션이다.
 *   pcie_bus_perf   - MPS 를 최대한 크게 맞춘다. NVMe 의 TLP 개수가 줄어
 *                     처리량이 올라가지만, 계층에 낡은 장치가 있으면 위험하다.
 *   pcie_bus_safe   - 계층 전체의 공통 최소값으로 MPS 를 맞춘다(보수적).
 *   realloc         - 펌웨어가 배정한 BAR 주소를 버리고 커널이 다시 배치한다.
 *                     펌웨어가 NVMe 의 BAR 를 제대로 잡아 주지 못한 보드에서
 *                     쓴다.
 *   noaer           - AER 을 끈다. NVMe 오류가 보고되지 않는다.
 *   noats           - ATS 를 끈다. IOMMU 변환 캐시를 장치가 갖지 못한다.
 *
 * 실행 컨텍스트: 부팅 아주 초기(early_param). 메모리 할당기도, printk
 *   버퍼도 제한적으로만 동작한다. 그래서 문자열을 복사하지 않고
 *   __initdata 를 그대로 가리키며, 그 뒤처리를 아래
 *   pci_realloc_setup_params() 가 맡는다.
 * 호출자: 커널 부팅 인자 파서.
 */
static int __init pci_setup(char *str)
{
	/* [한국어] 쉼표로 이어진 항목들을 하나씩 처리한다. */
	while (str) {
		char *k = strchr(str, ',');	/* [한국어] 다음 항목의 위치 */
		if (k)
			/* [한국어] 쉼표를 널로 바꿔 현재 항목을 끊고, k 를 다음
			 * 항목의 첫 글자로 옮긴다. 후위 증가라 두 일이 한 줄에 담긴다. */
			*k++ = 0;
		/* [한국어] 세 조건을 && 로 잇는다.
		 * *str      : 빈 항목("pci=,foo" 같은 경우)을 건너뛴다.
		 * pcibios_setup(str) : 아키텍처에게 먼저 기회를 준다. 처리했으면
		 *                      NULL 을 돌려줘 아래 사슬을 건너뛰게 한다.
		 *                      돌려준 값을 str 에 다시 담는 것에 주의.
		 * *str      : 아키텍처가 문자열을 소비해 빈 것으로 만들었을 수 있다. */
		if (*str && (str = pcibios_setup(str)) && *str) {
			/* [한국어] CardBus 관련 옵션. 0 을 돌려주면 처리한 것이다
			 * (다른 비교들과 반환값 규약이 반대라 헷갈리기 쉽다). */
			if (!pci_setup_cardbus(str)) {
				/* Function handled the parameters */
			} else if (!strcmp(str, "nomsi")) {
				/* [한국어] MSI/MSI-X 전역 비활성화. msi/msi.c 의
				 * pci_msi_enable 을 false 로 만든다. */
				pci_no_msi();
			} else if (!strncmp(str, "noats", 5)) {
				/* [한국어] Address Translation Services 비활성화.
				 * 장치가 IOMMU 변환 결과를 캐시하지 못하게 한다. */
				pr_info("PCIe: ATS is disabled\n");
				pcie_ats_disabled = true;
			} else if (!strcmp(str, "noaer")) {
				/* [한국어] Advanced Error Reporting 비활성화. */
				pci_no_aer();
			} else if (!strcmp(str, "earlydump")) {
				/* [한국어] 열거 중 각 장치의 config space 를 통째로
				 * 로그에 찍는다. 부팅이 매우 느려지므로 디버그 전용. */
				pci_early_dump = true;
			} else if (!strncmp(str, "realloc=", 8)) {
				/* [한국어] 값이 붙은 형태("realloc=off"). 반드시
				 * 아래 "realloc" 보다 먼저 검사해야 한다. */
				pci_realloc_get_opt(str + 8);
			} else if (!strncmp(str, "realloc", 7)) {
				/* [한국어] 값 없는 형태 — "on" 으로 간주한다. */
				pci_realloc_get_opt("on");
			} else if (!strcmp(str, "nodomains")) {
				pci_no_domains();	/* [한국어] 도메인 번호 표기 끄기 */
			} else if (!strncmp(str, "noari", 5)) {
				/* [한국어] ARI(Alternative Routing-ID) 비활성화.
				 * 한 장치가 8개 넘는 function 을 갖는 확장 방식이며,
				 * SR-IOV 의 VF 다수 배치에 쓰인다. */
				pcie_ari_disabled = true;
			} else if (!strncmp(str, "notph", 5)) {
				/* [한국어] TPH(TLP Processing Hints) 비활성화.
				 * 장치가 "이 데이터는 어느 캐시에 넣어 달라" 고
				 * 힌트를 주는 기능이다. */
				pci_no_tph();
			} else if (!strncmp(str, "resource_alignment=", 19)) {
				/* [한국어] 포인터만 저장한다. 복사하지 않는 이유는
				 * 이 시점에 할당기가 없을 수 있어서이고, 그 뒷수습을
				 * pci_realloc_setup_params() 가 한다. */
				resource_alignment_param = str + 19;
			} else if (!strncmp(str, "ecrc=", 5)) {
				/* [한국어] ECRC(End-to-end CRC) 정책. TLP 에 CRC 를
				 * 붙여 경로 전체의 데이터 무결성을 검사한다. */
				pcie_ecrc_get_policy(str + 5);
			} else if (!strncmp(str, "hpiosize=", 9)) {
				/* [한국어] 핫플러그 슬롯에 미리 예약해 둘 I/O 공간 크기.
				 * memparse 는 "1M", "256K" 같은 접미사를 해석하고,
				 * &str 로 파싱이 끝난 위치를 돌려준다. */
				pci_hotplug_io_size = memparse(str + 9, &str);
			} else if (!strncmp(str, "hpmmiosize=", 11)) {
				/* [한국어] 비프리페치 메모리 예약 크기. */
				pci_hotplug_mmio_size = memparse(str + 11, &str);
			} else if (!strncmp(str, "hpmmioprefsize=", 15)) {
				/* [한국어] 프리페치 가능 메모리 예약 크기. */
				pci_hotplug_mmio_pref_size = memparse(str + 15, &str);
			} else if (!strncmp(str, "hpmemsize=", 10)) {
				/* [한국어] 위 둘을 같은 값으로 한 번에 지정하는 단축 형태. */
				pci_hotplug_mmio_size = memparse(str + 10, &str);
				pci_hotplug_mmio_pref_size = pci_hotplug_mmio_size;
			} else if (!strncmp(str, "hpbussize=", 10)) {
				/* [한국어] 핫플러그 브리지에 예약할 버스 번호 개수. */
				pci_hotplug_bus_size =
					simple_strtoul(str + 10, &str, 0);
				/* [한국어] 버스 번호는 8비트라 255 를 넘을 수 없다.
				 * 범위를 벗어나면 조용히 기본값으로 되돌린다. */
				if (pci_hotplug_bus_size > 0xff)
					pci_hotplug_bus_size = DEFAULT_HOTPLUG_BUS_SIZE;
			} else if (!strncmp(str, "pcie_bus_tune_off", 17)) {
				/* [한국어] MPS 를 건드리지 않는다. 펌웨어 설정 그대로. */
				pcie_bus_config = PCIE_BUS_TUNE_OFF;
			} else if (!strncmp(str, "pcie_bus_safe", 13)) {
				/* [한국어] 계층의 공통 최소 MPS 로 통일. 보수적이지만
				 * 어떤 조합에서도 안전하다. */
				pcie_bus_config = PCIE_BUS_SAFE;
			} else if (!strncmp(str, "pcie_bus_perf", 13)) {
				/* [한국어] MPS 를 최대한 크게. NVMe 처리량에 유리하지만
				 * MRRS 를 MPS 로 클램프하는 부작용이 따른다
				 * (pcie_set_readrq 주석 참고). */
				pcie_bus_config = PCIE_BUS_PERFORMANCE;
			} else if (!strncmp(str, "pcie_bus_peer2peer", 18)) {
				/* [한국어] MPS 를 128 로 통일. 어떤 두 장치끼리도
				 * 직접 통신(P2PDMA)할 수 있게 하는 최소 공통값이다. */
				pcie_bus_config = PCIE_BUS_PEER2PEER;
			} else if (!strncmp(str, "pcie_scan_all", 13)) {
				/* [한국어] PCIe 링크 아래 장치 0 만 스캔한다는 규칙을
				 * 무시하고 전부 훑는다. 규칙을 어기는 하드웨어용. */
				pci_add_flags(PCI_SCAN_ALL_PCIE_DEVS);
			} else if (!strncmp(str, "disable_acs_redir=", 18)) {
				/* [한국어] 지정한 장치의 ACS 재지향 차단을 끈다.
				 * P2PDMA 를 쓰려면 필요할 수 있으나 격리가 약해진다. */
				disable_acs_redir_param = str + 18;
			} else if (!strncmp(str, "config_acs=", 11)) {
				/* [한국어] ACS 설정을 직접 지정한다. */
				config_acs_param = str + 11;
			} else {
				/* [한국어] 모르는 옵션. 부팅을 막지 않고 로그만 남긴다 —
				 * 오타 하나로 부팅이 안 되면 복구가 어렵기 때문이다. */
				pr_err("PCI: Unknown option `%s'\n", str);
			}
		}
		str = k;	/* [한국어] 다음 항목으로. 마지막이면 k 가 NULL 이라 루프가 끝난다 */
	}
	return 0;	/* [한국어] early_param 규약상 0 = 처리 완료 */
}
/* [한국어] "pci=" 인자를 위 함수가 처리하도록 등록한다. early_param 은
 * 메모리 관리자가 준비되기도 전에 불리는 아주 이른 단계다 — 그래서
 * 이 함수 안에서 kmalloc 을 쓸 수 없다. */
early_param("pci", pci_setup);

/*
 * 'resource_alignment_param' and 'disable_acs_redir_param' are initialized
 * in pci_setup(), above, to point to data in the __initdata section which
 * will be freed after the init sequence is complete. We can't allocate memory
 * in pci_setup() because some architectures do not have any memory allocation
 * service available during an early_param() call. So we allocate memory and
 * copy the variable here before the init section is freed.
 *
 */
/*
 * [한국어]
 * pci_realloc_setup_params - 부팅 인자 문자열을 해제되지 않을 메모리로 옮긴다
 *
 * @return: 항상 0.
 *
 * 위 원문 주석이 문제와 해법을 모두 설명한다. 요약하면 —
 * pci_setup() 은 early_param 단계라 메모리를 할당할 수 없어서, 문자열을
 * 복사하지 않고 __initdata 영역을 그대로 가리키게 해 두었다. 그런데
 * __initdata 는 부팅이 끝나면 통째로 해제된다. 그 전에 힙으로 옮겨
 * 놓아야 나중에 sysfs 로 읽거나 장치 열거에서 참조할 때 살아 있다.
 *
 * kstrdup 이 NULL 을 받으면 NULL 을 돌려준다는 성질을 이용해, 인자가
 * 지정되지 않은 경우(포인터가 NULL)를 따로 검사하지 않는다.
 *
 * 할당 실패를 확인하지 않는 것이 눈에 띈다. 실패하면 포인터가 NULL 이 되어
 * "그 옵션이 지정되지 않은 것" 과 같은 상태가 된다. 부팅 인자 하나를
 * 잃는 것뿐이라 치명적이지 않고, 이 시점에 그만큼도 할당하지 못하는
 * 시스템이면 어차피 곧 다른 데서 실패한다.
 *
 * pure_initcall 단계를 고른 이유: __initdata 해제(free_initmem)보다는
 * 이르고, 장치 열거(subsys_initcall 이후)보다는 빨라야 한다.
 * pure_initcall 은 initcall 중 가장 이른 단계라 두 조건을 모두 만족한다.
 *
 * 실행 컨텍스트: 부팅 중, 단일 스레드. 이 시점에는 할당기가 준비돼 있다.
 */
static int __init pci_realloc_setup_params(void)
{
	/* [한국어] 세 문자열을 힙으로 복사한다. kstrdup 은 NULL 을 받으면
	 * NULL 을 돌려주므로, 지정되지 않은 옵션도 그대로 통과한다.
	 * 반환값을 원래 변수에 다시 담아 포인터를 갈아끼운다 — 옛 포인터는
	 * __initdata 를 가리키므로 해제하면 안 된다(애초에 할당한 것이 아니다). */
	resource_alignment_param = kstrdup(resource_alignment_param,
					   GFP_KERNEL);
	disable_acs_redir_param = kstrdup(disable_acs_redir_param, GFP_KERNEL);
	config_acs_param = kstrdup(config_acs_param, GFP_KERNEL);

	return 0;	/* [한국어] 실패해도 0. 위 함수 주석 참고 */
}
/* [한국어] initcall 중 가장 이른 단계에 등록한다. __initdata 가 해제되기
 * 전이면서 장치 열거보다는 앞서야 하기 때문이다. */
pure_initcall(pci_realloc_setup_params);
