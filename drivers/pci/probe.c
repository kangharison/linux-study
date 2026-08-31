// SPDX-License-Identifier: GPL-2.0
/*
 * PCI detection and setup code
 */
/*
 * [한국어] PCI 버스 열거(enumeration)와 장치 설정의 핵심 (drivers/pci/probe.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 "이 PCI 버스에 어떤 장치가 꽂혀 있는가"를 알아내고, 발견한 각
 * 장치마다 struct pci_dev 를 하나씩 만들어 커널 드라이버 모델에 등록하는
 * 일을 한다. 시스템에는 장치 목록을 알려 주는 별도의 명부가 없으므로,
 * 커널은 가능한 모든 (bus, device, function) 좌표에 대해 config space 를
 * 실제로 읽어 보고 응답이 오는지로 존재 여부를 판정한다 —
 * 이것이 pci_scan_slot() → pci_scan_single_device() → pci_scan_device() 가
 * 하는 일이며, 응답이 없으면 호스트 브리지가 모두 1(0xFFFFFFFF)을 돌려주므로
 * Vendor ID 가 0xFFFF 인 것을 "장치 없음"으로 해석한다.
 * 존재가 확인되면 pci_setup_device() 가 헤더 타입에 따라 config space 를
 * 해석해 BAR·IRQ·클래스·서브시스템 ID 를 pci_dev 에 채우고,
 * pci_device_add() 가 device_add() 를 불러 드라이버 매칭을 개시한다.
 * 브리지를 만나면 secondary/subordinate 버스 번호를 배정하고 그 아래로
 * 재귀 하강해(pci_scan_bridge_extend ↔ pci_scan_child_bus_extend) 트리
 * 전체를 훑는다. 부팅 시의 최초 스캔과 핫플러그/rescan 이 모두 이 코드를
 * 공유한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 실행 컨텍스트는 항상 프로세스 문맥의 커널 코드다(인터럽트 문맥 아님).
 * 부팅 경로에서는 아키텍처/펌웨어 코드(ACPI, DT, 각 호스트 컨트롤러
 * 드라이버 drivers/pci/controller/ 아래)가 struct pci_host_bridge 를 만들어
 * pci_host_probe() 또는 pci_scan_root_bus_bridge() 를 부르는 것으로
 * 시작한다. 그 아래 흐름은 다음과 같다.
 *   pci_host_probe/pci_scan_root_bus_bridge
 *     → pci_register_host_bridge()   : 루트 버스 생성 + device_add
 *     → pci_scan_child_bus()         : 루트 버스부터 재귀 스캔
 *        → pci_scan_child_bus_extend()
 *           → pci_scan_slot()        : 한 슬롯의 function 0..7
 *              → pci_scan_single_device() → pci_scan_device()
 *                 → pci_bus_read_dev_vendor_id()  : 존재 판정(0xFFFF)
 *                 → pci_setup_device()            : 헤더 해석 + BAR 크기 측정
 *                 → pci_device_add()              : 드라이버 모델 등록
 *           → pci_scan_bridge_extend()            : 브리지 아래로 재귀
 * 이 파일이 끝나면 리소스는 "크기와 정렬만 아는 미할당 상태"일 수 있고,
 * 실제 주소 배치는 setup-bus.c(pci_assign_unassigned_bus_resources 등)가
 * 이어받는다. 드라이버 바인딩은 pci-driver.c 가 담당한다.
 *
 * === 타 모듈과의 연결 ===
 * - drivers/pci/access.c : pci_bus_read_config_byte/word/dword 및 대응하는
 *   write 함수로 실제 config 접근을 수행한다. 이 파일은 그 위에서 오프셋의
 *   의미만 해석한다.
 * - drivers/pci/pci.h : pci_bus_read_dev_vendor_id(), __pci_read_base(),
 *   pci_read_bridge_bases() 등 내부 API 선언과 pci_flags 를 공유한다.
 * - drivers/pci/setup-bus.c, setup-res.c : 여기서 측정한 res->start/end/
 *   flags(IORESOURCE_UNSET 포함)를 받아 실제 주소를 배정한다.
 * - drivers/pci/msi/, aer, aspm, iov, pm : pci_init_capabilities() 와
 *   pci_configure_device() 가 각 하위 시스템의 초기화 훅을 불러 준다.
 * - drivers/nvme/host/pci.c : 이 파일이 만들어 준 pci_dev 를 받아
 *   pci_enable_device_mem(), pci_request_mem_regions(pdev, "nvme") 로
 *   BAR 영역을 확보한 뒤 nvme_remap_bar() 안에서
 *   ioremap(pci_resource_start(pdev, 0), size) 로 BAR0 을 매핑한다
 *   (drivers/nvme/host/pci.c:2265, 크기 상한은 pci_resource_len(pdev, 0)).
 *   그 BAR0 창 안에 NVMe 컨트롤러 레지스터 CAP(0x00)/CC(0x14)/CSTS(0x1c)와
 *   0x1000 부터 시작하는 doorbell 배열(dev->dbs = dev->bar + NVME_REG_DBS)이
 *   놓인다. 즉 __pci_read_base() 가 잰 BAR0 의 크기·타입(64bit/prefetch)이
 *   그대로 NVMe MMIO 창의 크기가 된다. MSI-X 벡터는
 *   pci_alloc_irq_vectors_affinity() 로 별도로 받는다.
 * - 데이터 흐름: config space(하드웨어) → pci_dev 필드/struct resource →
 *   sysfs 및 리소스 할당기 → 드라이버(probe) → NVMe 의 MMIO 매핑.
 *
 * === 주요 함수/구조체 요약 ===
 * - __pci_read_base()  : BAR 에 모두 1(0xFFFFFFFF)을 써 넣고 되읽어,
 *   하드와이어된 하위 0 비트 수로 영역 크기를 역산하는 고전 기법. 32/64비트
 *   BAR, prefetchable, IO/MEM 마스크 차이를 모두 여기서 처리한다.
 * - pci_setup_device() : header type(0=일반 장치, 1=PCI-to-PCI 브리지,
 *   2=CardBus 브리지)에 따라 서로 다른 레이아웃으로 config space 를 해석.
 * - pci_scan_slot()/pci_scan_single_device()/pci_scan_device() : 장치 존재
 *   판정과 pci_dev 생성.
 * - pci_device_add()   : pci_dev 를 버스 목록과 sysfs 에 올리고 device_add()
 *   로 드라이버 매칭을 개시하는 지점.
 * - pci_scan_bridge_extend()/pci_scan_child_bus_extend() : 브리지의
 *   primary/secondary/subordinate 버스 번호 배정과 재귀 하강.
 * - pci_register_host_bridge()/pci_create_root_bus() : 루트 버스 생성.
 * - struct pci_bus, struct pci_dev, struct pci_host_bridge (include/linux/pci.h)
 *   가 이 파일이 채우는 핵심 자료구조다.
 */

#include <linux/array_size.h>	/* [한국어] ARRAY_SIZE() — pci_speed_string() 의 속도 문자열 배열 크기 계산에 필요 */
#include <linux/kernel.h>	/* [한국어] 커널 기본 매크로(min/max, container_of 등) */
#include <linux/delay.h>	/* [한국어] msleep()/udelay() — RRS(Configuration Request Retry Status) 재시도 대기와 브리지 리셋 후 정착 시간에 사용 */
#include <linux/init.h>	/* [한국어] __init/__weak 등 초기화 섹션 지정자 — pcibus_class_init(), pci_sort_breadthfirst() 가 부팅 전용 코드임을 표시 */
#include <linux/pci.h>	/* [한국어] struct pci_dev/pci_bus/pci_host_bridge 와 PCI_* config space 오프셋 상수(PCI_VENDOR_ID, PCI_BASE_ADDRESS_0 …)의 원천 */
#include <linux/msi.h>	/* [한국어] MSI/MSI-X irq domain 타입 — pci_dev_msi_domain(), pci_set_msi_domain() 이 장치별 인터럽트 도메인을 물릴 때 필요. NVMe 의 큐당 MSI-X 벡터도 결국 이 도메인에서 나온다 */
#include <linux/of_pci.h>	/* [한국어] Device Tree 기반 PCI 헬퍼(pci_set_of_node 등) — DT 시스템에서 노드를 pci_dev 에 연결 */
#include <linux/of_platform.h>	/* [한국어] of_platform_populate() — 호스트 브리지 아래 DT 자식 노드를 플랫폼 장치로 만들 때 */
#include <linux/platform_device.h>	/* [한국어] 호스트 브리지가 platform_device 로 표현되는 경우의 타입 정의 */
#include <linux/pci_hotplug.h>	/* [한국어] 핫플러그 슬롯 관련 정의 — pci_hp_add_bridge() 와 removable 판정에 사용 */
#include <linux/slab.h>	/* [한국어] kzalloc/kfree — pci_dev, pci_bus, host bridge 객체 동적 할당 */
#include <linux/sprintf.h>	/* [한국어] sprintf() — 버스/장치 이름 문자열(dev_set_name) 구성 */
#include <linux/module.h>	/* [한국어] EXPORT_SYMBOL() — pci_root_buses 등 심볼을 모듈(예: nvme, 각종 호스트 드라이버)에 공개 */
#include <linux/cpumask.h>	/* [한국어] cpumask — irq affinity 계산 경로에서 참조 */
#include <linux/aer.h>	/* [한국어] AER(Advanced Error Reporting) 초기화/해제 API — pci_aer_init()/pci_aer_exit(). NVMe 컨트롤러의 PCIe 오류 복구가 이 위에서 동작 */
#include <linux/acpi.h>	/* [한국어] ACPI 펌웨어 연동 — pci_acpi_program_hp_params(), ACPI 기반 removable/슬롯 정보 */
#include <linux/hypervisor.h>	/* [한국어] hypervisor_isolated_pci_functions() — 가상화 환경에서 function 0 만 보이는 경우의 스캔 최적화 */
#include <linux/irqdomain.h>	/* [한국어] struct irq_domain — MSI 도메인 조회/설정에 필요 */
#include <linux/pm_runtime.h>	/* [한국어] runtime PM — 새 pci_dev 의 PM 상태 초기화(pm_runtime_*)에 사용 */
#include <linux/bitfield.h>	/* [한국어] FIELD_GET/FIELD_PREP — capability 레지스터의 비트필드를 마스크·시프트 없이 뽑아낼 때 */
#include <trace/events/pci.h>	/* [한국어] trace_pci_hp_event() 등 PCI tracepoint 정의 */
#include "pci.h"	/* [한국어] drivers/pci 내부 전용 선언 — pci_bus_read_dev_vendor_id(), __pci_read_base(), pci_flags, 각 하위 시스템 init/exit 훅 */

/*
 * [한국어] busn_resource - PCI "버스 번호" 를 하나의 리소스 축으로 표현한 루트
 * 리소스. PCI config 트랜잭션의 주소는 (bus, device, function, offset) 인데,
 * 이 중 bus 번호는 8비트라 0~255 만 존재한다. 커널은 이 번호 공간도
 * 메모리/IO 처럼 struct resource 트리로 관리해, 브리지마다 배정한
 * secondary~subordinate 구간이 서로 겹치지 않도록 검사한다.
 * 설정자: 이 정적 초기화가 전부(런타임 변경 없음).
 * 읽는 자: pci_bus_insert_busn_res() 가 자식 버스 번호 구간을 이 리소스의
 * 하위로 insert_resource() 할 때 부모로 사용한다.
 */
static struct resource busn_resource = {
	.name	= "PCI busn",
	/* [한국어] /proc/iomem 류 출력과 디버깅에서 이 리소스를 식별할 이름 */
	.start	= 0,
	/* [한국어] 버스 번호 하한 — PCI 버스 번호는 0 부터 시작 */
	.end	= 255,
	/* [한국어] 버스 번호 상한 — config 주소의 bus 필드가 8비트이므로 최대 255 */
	.flags	= IORESOURCE_BUS,
	/* [한국어] 이 리소스가 메모리도 IO 도 아닌 "버스 번호" 축임을 표시하는 플래그.
	 * IORESOURCE_MEM/IO 와 달리 CPU 주소 공간이 아니라 PCI 토폴로지 좌표를 뜻한다. */
};

/* Ugh.  Need to stop exporting this to modules. */
/*
 * [한국어] pci_root_buses - 시스템의 모든 "루트 버스"(호스트 브리지 바로 아래
 * 버스) 를 잇는 전역 연결 리스트의 머리. 각 원소는 struct pci_bus 의 node
 * 필드로 매달린다.
 * 설정자: pci_register_host_bridge() 가 새 루트 버스를 list_add_tail() 한다.
 * 읽는 자: pci_find_next_bus() 등 버스 전체를 훑는 코드.
 * 동기화: 갱신은 pci_bus_sem 을 잡은 상태에서 이루어진다.
 */
LIST_HEAD(pci_root_buses);
EXPORT_SYMBOL(pci_root_buses);
/* [한국어] 모듈에서 참조 가능하도록 심볼 공개. 위 영어 주석("Ugh.")은
 * 이 export 를 언젠가 없애고 싶다는 상류 개발자의 메모다. */

static LIST_HEAD(pci_domain_busn_res_list);
/* [한국어] PCI 도메인(세그먼트) 번호별 버스 번호 리소스를 캐시해 두는 리스트.
 * 한 시스템에 루트 컴플렉스가 여럿이면 각자 0~255 의 버스 번호 공간을 따로
 * 가지므로, 도메인마다 별도의 0~0xff 리소스가 필요하다.
 * 설정자/읽는 자: 아래 get_pci_domain_busn_res() 하나뿐이다.
 * 동기화: 호출 경로가 버스 스캔(직렬화된 초기화/rescan 락 아래)이라 별도 락 없음. */

/*
 * [한국어] struct pci_domain_busn_res - "도메인 번호 → 그 도메인의 버스 번호
 * 리소스" 매핑 한 건. get_pci_domain_busn_res() 전용의 내부 자료구조다.
 */
struct pci_domain_busn_res {
	struct list_head list;
	/* [한국어] pci_domain_busn_res_list 에 자신을 매다는 링크.
	 * 설정자: get_pci_domain_busn_res() 의 list_add_tail().
	 * 읽는 자: 같은 함수의 list_for_each_entry() 순회.
	 * 값 범위: 항상 초기화된 리스트 노드.
	 * 동기화: 위 리스트와 동일(별도 락 없음). */

	struct resource res;
	/* [한국어] 이 도메인이 쓸 수 있는 버스 번호 구간(0~0xff)을 나타내는 리소스.
	 * 설정자: 아래 함수에서 start=0, end=0xff, flags=IORESOURCE_BUS|
	 *   IORESOURCE_PCI_FIXED 로 한 번 초기화된다.
	 * 읽는 자: pci_bus_insert_busn_res() 가 자식 버스 구간을 넣을 부모로 사용.
	 * 값 범위: 고정(0~255).
	 * 동기화: 리소스 트리 조작은 커널 resource 락이 보호한다. */

	int domain_nr;
	/* [한국어] 이 항목이 담당하는 PCI 도메인(세그먼트) 번호.
	 * 설정자: get_pci_domain_busn_res() 가 새로 만들 때 인자값을 저장.
	 * 읽는 자: 같은 함수의 조회 루프에서 비교 키로 사용.
	 * 값 범위: 0 이상. 단일 세그먼트 시스템은 항상 0.
	 * 동기화: 생성 후 불변. */
};

/*
 * [한국어]
 * get_pci_domain_busn_res - 주어진 PCI 도메인의 "버스 번호 리소스"를 얻는다
 *
 * @domain_nr: PCI 도메인(세그먼트) 번호. 단일 루트 컴플렉스 시스템은 0.
 * @return: 해당 도메인의 0~0xff 버스 번호 루트 리소스 포인터. 메모리 할당에
 *          실패하면 NULL — 호출자(pci_bus_insert_busn_res)는 이때 -ENOMEM 을
 *          돌려주고 버스 번호 등록을 포기한다.
 *
 * 왜 필요한가: 버스 번호는 도메인마다 독립된 0~255 공간이다. 도메인이 여럿인
 * 시스템(예: 여러 세그먼트를 쓰는 서버, VMD 로 묶인 NVMe 그룹)에서 한 개의
 * 전역 0~255 리소스만 쓰면 서로 다른 도메인의 같은 버스 번호가 충돌한 것으로
 * 오판된다. 그래서 도메인별로 루트 리소스를 하나씩 만들어 캐시한다.
 *
 * 동작: 리스트를 훑어 이미 만들어 둔 항목이 있으면 그것을 돌려주고, 없으면
 * kzalloc 으로 새로 만들어 0~0xff 로 초기화한 뒤 리스트에 매단다(lazy 생성).
 * 한 번 만든 항목은 해제하지 않는다(도메인 수만큼만 생기므로 누수가 아니다).
 *
 * 실행 컨텍스트: 버스 스캔/핫플러그 경로의 프로세스 문맥. 호출자가 이미
 * 직렬화되어 있어 별도 락을 잡지 않는다.
 *
 * 호출 체인:
 *   pci_bus_insert_busn_res() → [get_pci_domain_busn_res] → kzalloc_obj/list_add_tail
 */
static struct resource *get_pci_domain_busn_res(int domain_nr)
{
	struct pci_domain_busn_res *r;
	/* [한국어] 리스트 순회 커서 겸, 새로 만들 항목을 담을 포인터 */

	/* [한국어] 이미 이 도메인의 리소스를 만들어 둔 적이 있는지 선형 탐색.
	 * 도메인 수는 보통 한 자릿수라 선형 탐색으로 충분하다. */
	list_for_each_entry(r, &pci_domain_busn_res_list, list)
		if (r->domain_nr == domain_nr)
			/* [한국어] 캐시 적중 — 기존 리소스를 그대로 재사용한다 */
			return &r->res;

	/* [한국어] 없으면 새로 만든다. kzalloc 이므로 list/res 필드가 0 으로 시작한다 */
	r = kzalloc_obj(*r);
	if (!r)
		/* [한국어] 할당 실패 — 호출자가 -ENOMEM 으로 변환해 상위로 전파한다 */
		return NULL;

	r->domain_nr = domain_nr;
	/* [한국어] 조회 키가 될 도메인 번호 기록 */
	r->res.start = 0;
	/* [한국어] 이 도메인에서 쓸 수 있는 최소 버스 번호 */
	r->res.end = 0xff;
	/* [한국어] 최대 버스 번호 255 — config 주소의 bus 필드가 8비트이기 때문 */
	r->res.flags = IORESOURCE_BUS | IORESOURCE_PCI_FIXED;
	/* [한국어] IORESOURCE_BUS = 버스 번호 축의 리소스,
	 * IORESOURCE_PCI_FIXED = 이 구간은 재배치(리밸런싱) 대상이 아니라
	 * 고정되어 있음을 리소스 할당기에 알리는 표시. */

	/* [한국어] 다음 호출부터 캐시 적중하도록 전역 리스트에 등록 */
	list_add_tail(&r->list, &pci_domain_busn_res_list);

	return &r->res;
	/* [한국어] 새로 만든 루트 리소스를 호출자에게 넘긴다 */
}

/*
 * PCI Bus Class
 */
/*
 * [한국어]
 * release_pcibus_dev - struct pci_bus 의 마지막 참조가 사라졌을 때 호출되는 소멸자
 *
 * @dev: 해제 대상 pci_bus 안에 박혀 있는 struct device. to_pci_bus() 로 바깥
 *       pci_bus 를 복원한다.
 * @return: 없음.
 *
 * 왜 필요한가: pci_bus 는 드라이버 모델의 refcount(struct device) 로 수명이
 * 관리된다. 커널은 put_device() 로 참조가 0 이 되는 순간 class->dev_release
 * 콜백을 부르는데, 그 콜백이 이 함수다. 여기서 kfree 를 해야만 버스 객체가
 * 실제로 반납된다 — 아무도 임의 시점에 kfree(pci_bus) 를 하지 않는다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 마지막 put_device() 를 호출한 쪽(버스 제거,
 * 핫플러그 언플러그, 초기화 실패 경로)에서 동기적으로 실행된다.
 *
 * 호출 체인:
 *   put_device(&bus->dev) → device_release() → [release_pcibus_dev] → kfree
 */
static void release_pcibus_dev(struct device *dev)
{
	struct pci_bus *pci_bus = to_pci_bus(dev);
	/* [한국어] container_of 기반 역변환 — 내장된 device 주소에서 바깥 pci_bus 복원 */

	put_device(pci_bus->bridge);
	/* [한국어] 이 버스를 만든 부모(호스트 브리지 또는 상위 브리지 pci_dev)의
	 * 참조를 놓는다. pci_alloc_child_bus()/pci_register_host_bridge() 에서
	 * get_device() 로 잡아 둔 것과 짝을 이룬다. */
	pci_bus_remove_resources(pci_bus);
	/* [한국어] 이 버스에 등록해 둔 리소스 목록(bridge window 등)을 모두 떼어내
	 * 리소스 트리에서 제거한다. 남겨 두면 다음 스캔에서 유령 리소스가 된다. */
	pci_release_bus_of_node(pci_bus);
	/* [한국어] DT/OF 노드에 잡아 둔 참조를 놓는다(CONFIG_OF 가 없으면 빈 함수) */
	kfree(pci_bus);
	/* [한국어] 마지막으로 pci_bus 자체 메모리 반납. 이 시점 이후 dev 접근 금지 */
}

/*
 * [한국어] pcibus_class - PCI 버스 객체들이 속하는 드라이버 모델 클래스.
 * 이 클래스가 있어야 /sys/class/pci_bus/ 아래에 각 버스가 노출되고,
 * 버스 객체의 소멸자와 공통 sysfs 속성이 커널에 등록된다.
 */
static const struct class pcibus_class = {
	.name		= "pci_bus",
	/* [한국어] sysfs 상의 클래스 디렉터리 이름 — /sys/class/pci_bus 로 보인다 */
	.dev_release	= &release_pcibus_dev,
	/* [한국어] 이 클래스에 속한 device 의 refcount 가 0 이 될 때 불릴 소멸자.
	 * 위 release_pcibus_dev() 를 가리키며, pci_bus 의 kfree 책임을 진다. */
	.dev_groups	= pcibus_groups,
	/* [한국어] 모든 pci_bus 에 자동으로 생성될 sysfs 속성 그룹
	 * (drivers/pci/pci-sysfs.c 에 정의: cpuaffinity, cpulistaffinity 등). */
};

/*
 * [한국어]
 * pcibus_class_init - PCI 버스 클래스를 드라이버 모델에 등록하는 초기화 함수
 *
 * @return: class_register() 의 결과. 0 이면 성공, 음수면 errno.
 *
 * 왜 필요한가: 어떤 pci_bus 도 만들어지기 전에 클래스가 먼저 존재해야
 * device_register() 가 성공한다. 그래서 postcore_initcall 단계(드라이버
 * initcall 보다 훨씬 이른 시점)에 등록해 둔다.
 *
 * 실행 컨텍스트: 부팅 시 단 한 번, 단일 스레드 초기화 문맥(__init).
 *
 * 호출 체인:
 *   do_initcalls() → [pcibus_class_init] → class_register()
 */
static int __init pcibus_class_init(void)
{
	return class_register(&pcibus_class);
	/* [한국어] 실패해도 부팅은 계속되지만 이후 버스 등록이 줄줄이 실패한다 */
}
postcore_initcall(pcibus_class_init);
/* [한국어] initcall 레벨 2(postcore). PCI 호스트 컨트롤러 드라이버들이 도는
 * subsys/device initcall 보다 앞서 실행되도록 이 레벨을 고른 것이다. */

/*
 * [한국어]
 * pci_size - "모두 1 쓰고 되읽기" 결과에서 BAR 가 디코딩하는 영역 크기를 역산
 *
 * @base:    BAR 의 원래 값(펌웨어가 넣어 둔 주소). 주소 비트만 남긴 상태.
 * @maxbase: BAR 에 0xFFFFFFFF(또는 ROM 마스크)를 쓰고 되읽은 값. 주소 비트만
 *           남긴 상태. 장치가 디코딩하지 않는 하위 비트는 0 으로 고정되어 있다.
 * @mask:    이 BAR 종류에서 "주소로 쓰이는 비트"를 나타내는 마스크
 *           (MEM 은 ~0x0f, IO 는 ~0x03, ROM 은 ~0x7ff, 64비트는 상위까지 확장).
 * @return: BAR 가 차지하는 바이트 크기(항상 2의 거듭제곱). 미구현 BAR 이거나
 *          결과가 말이 안 되면 0 — 호출자 __pci_read_base() 는 0 을 받으면
 *          그 BAR 를 없는 것으로 처리하고 res->flags 를 0 으로 지운다.
 *
 * 왜 이 방법인가: PCI 스펙은 BAR 의 크기를 알려 주는 별도 레지스터를 두지
 * 않는다. 대신 "BAR 에 모두 1 을 써 넣고 되읽으면, 장치가 디코딩하는 상위
 * 비트만 1 로 남고 크기보다 낮은 하위 비트는 0 으로 하드와이어되어 읽힌다"고
 * 규정한다(pci_regs.h 의 영어 주석: "Decoded size can be determined by writing
 * a value of 0xffffffff to the register, and reading it back. Only 1 bits are
 * decoded."). 예를 들어 16KB(0x4000) 를 디코딩하는 MEM BAR 는 되읽었을 때
 * 0xFFFFC000 이 되고, 이때 최하위 1 비트의 자리값이 곧 크기 0x4000 이다.
 *
 * 동작 단계:
 *  1) mask & maxbase 로 "1 로 남은 주소 비트"만 추린다.
 *  2) size & ~(size-1) 로 그 중 최하위 1 비트만 남긴다 → 이것이 곧 크기.
 *  3) base == maxbase 인 이상한 경우를 걸러낸다(아래 인라인 주석 참조).
 *
 * 실행 컨텍스트: 프로세스 문맥. 순수 계산 함수라 하드웨어 접근도 락도 없다.
 *
 * 호출 체인:
 *   __pci_read_base() → [pci_size]
 */
static u64 pci_size(u64 base, u64 maxbase, u64 mask)
{
	u64 size = mask & maxbase;	/* Find the significant bits */
	/* [한국어] 되읽은 값에서 주소 비트만 남긴다. 하위 플래그 비트(IO/MEM,
	 * 64비트, prefetch)는 마스크로 잘려 나가고, 장치가 디코딩하는 범위의
	 * 비트만 1 로 남는다. */
	if (!size)
		/* [한국어] 남은 비트가 하나도 없다 = 이 BAR 는 구현되지 않았다.
		 * 미구현 BAR 는 쓰기를 무시하고 항상 0 을 반환하도록 스펙이 요구한다. */
		return 0;

	/*
	 * Get the lowest of them to find the decode size, and from that
	 * the extent.
	 */
	size = size & ~(size-1);
	/* [한국어] 최하위 1 비트만 남기는 관용구(two's complement 트릭).
	 * 예: size=0xFFFFC000 → size-1=0xFFFFBFFF → ~(size-1)=0x00004000 →
	 * AND 결과 0x00004000 = 16KB. 이 자리값이 곧 BAR 가 요구하는 크기이자
	 * 정렬 요구치다(PCI BAR 는 크기 단위로 자연 정렬되어야 한다). */

	/*
	 * base == maxbase can be valid only if the BAR has already been
	 * programmed with all 1s.
	 */
	if (base == maxbase && ((base | (size - 1)) & mask) != mask)
		/* [한국어] 원래 값과 되읽은 값이 같다는 것은 두 가지 뜻일 수 있다.
		 * (가) 펌웨어가 이미 BAR 에 all-1s 를 써 둔 정상적인 경우 —
		 *      이때는 base 에 (size-1) 를 OR 하면 마스크 전체가 1 이 된다.
		 * (나) config 접근이 실패해 쓰레기 값이 계속 읽히는 경우 —
		 *      이때는 위 등식이 성립하지 않으므로 0 을 돌려 이 BAR 를 버린다.
		 * 즉 (나) 를 걸러내기 위한 정합성 검사다. */
		return 0;

	return size;
	/* [한국어] 2의 거듭제곱 크기. 호출자는 res->end = res->start + size - 1 로 쓴다 */
}

/*
 * [한국어]
 * decode_bar - BAR 하위 플래그 비트를 커널 리소스 플래그로 번역한다
 *
 * @dev: 대상 장치(현재 구현에서는 참조하지 않지만 시그니처 통일을 위해 유지).
 * @bar: BAR 레지스터에서 읽은 32비트 원본 값(하위 플래그 비트가 살아 있는 상태).
 * @return: IORESOURCE_IO / IORESOURCE_MEM / IORESOURCE_PREFETCH /
 *          IORESOURCE_MEM_64 조합에, BAR 의 하위 플래그 비트 자체를 그대로
 *          섞어 넣은 값. 호출자 __pci_read_base() 가 res->flags 의 초기값으로 쓴다.
 *
 * 왜 필요한가: BAR 의 최하위 몇 비트는 주소가 아니라 속성 부호다. PCI 스펙상
 *   bit0 : 0 = 메모리 공간, 1 = I/O 공간 (PCI_BASE_ADDRESS_SPACE)
 *   메모리일 때 bit2:1 : 00 = 32비트, 10 = 1MB 이하(구식), 01 = 64비트
 *                        (PCI_BASE_ADDRESS_MEM_TYPE_MASK = 0x06)
 *   메모리일 때 bit3    : prefetchable (PCI_BASE_ADDRESS_MEM_PREFETCH = 0x08)
 *   I/O 일 때 bit1      : reserved
 * 이 비트들을 커널 공통 리소스 어휘로 바꿔야 이후 리소스 할당기가 어느 창에
 * 넣을지 판단할 수 있다.
 *
 * IO 와 MEM 의 마스크 차이가 핵심이다. IO BAR 는 하위 2비트만 플래그라
 * PCI_BASE_ADDRESS_IO_MASK = ~0x03 이고, MEM BAR 는 하위 4비트가 플래그라
 * PCI_BASE_ADDRESS_MEM_MASK = ~0x0f 이다. 따라서 최소 정렬도 각각 4바이트,
 * 16바이트다.
 *
 * NVMe 접점: NVMe 컨트롤러의 BAR0 은 메모리 공간 BAR 이며 보통 64비트로
 * 선언된다(그러면 BAR0/BAR1 두 칸을 함께 쓴다). prefetchable 은 아니어야
 * 한다 — 컨트롤러 레지스터는 읽기에 부작용이 있을 수 있는 MMIO 이기 때문.
 *
 * 실행 컨텍스트: 프로세스 문맥의 순수 함수. 하드웨어 접근 없음.
 *
 * 호출 체인:
 *   __pci_read_base() → [decode_bar]
 */
static inline unsigned long decode_bar(struct pci_dev *dev, u32 bar)
{
	u32 mem_type;
	/* [한국어] 메모리 BAR 의 타입 필드(bit2:1)를 담을 임시 변수 */
	unsigned long flags;
	/* [한국어] 조립해서 돌려줄 커널 리소스 플래그 */

	if ((bar & PCI_BASE_ADDRESS_SPACE) == PCI_BASE_ADDRESS_SPACE_IO) {
		/* [한국어] bit0 == 1 → I/O 공간 BAR. x86 의 in/out 명령으로 접근하는
		 * 포트 주소 공간이며, MMIO 와 완전히 별개의 주소 축이다. */
		flags = bar & ~PCI_BASE_ADDRESS_IO_MASK;
		/* [한국어] ~(~0x03) = 0x03 — 즉 하위 2비트(플래그 비트)만 추출해
		 * 리소스 플래그의 하위에 그대로 보존한다. 주소 비트는 버린다. */
		flags |= IORESOURCE_IO;
		/* [한국어] 커널 리소스 트리에서 I/O 포트 축임을 표시 */
		return flags;
		/* [한국어] I/O BAR 는 prefetch/64비트 개념이 없으므로 여기서 끝 */
	}

	flags = bar & ~PCI_BASE_ADDRESS_MEM_MASK;
	/* [한국어] ~(~0x0f) = 0x0f — 메모리 BAR 의 하위 4비트 플래그를 추출.
	 * IO 와 마스크 폭이 다른 것이 두 공간의 결정적 차이다. */
	flags |= IORESOURCE_MEM;
	/* [한국어] 커널 리소스 트리에서 MMIO 축임을 표시 */
	if (flags & PCI_BASE_ADDRESS_MEM_PREFETCH)
		/* [한국어] bit3 = prefetchable. "읽어도 부작용이 없고 쓰기 병합이
		 * 허용된다"는 뜻이라, 브리지의 prefetchable window(64비트 확장이
		 * 가능한 창) 로 배치할 수 있다. 프레임버퍼 같은 것이 대표적이다. */
		flags |= IORESOURCE_PREFETCH;

	mem_type = bar & PCI_BASE_ADDRESS_MEM_TYPE_MASK;
	/* [한국어] bit2:1 만 추출(마스크 0x06) — 메모리 BAR 의 주소 폭 종류 */
	switch (mem_type) {
	case PCI_BASE_ADDRESS_MEM_TYPE_32:
		/* [한국어] 값 0x00 — 32비트 주소 BAR. 추가 플래그가 필요 없다 */
		break;
	case PCI_BASE_ADDRESS_MEM_TYPE_1M:
		/* 1M mem BAR treated as 32-bit BAR */
		/* [한국어] 값 0x02 — 원래 "1MB 미만 주소에만 배치 가능"을 뜻하던
		 * 구식 인코딩. 현대 하드웨어에는 사실상 없으므로 32비트와 같게 다룬다. */
		break;
	case PCI_BASE_ADDRESS_MEM_TYPE_64:
		flags |= IORESOURCE_MEM_64;
		/* [한국어] 값 0x04 — 64비트 주소 BAR. 이 경우 바로 다음 BAR 칸이
		 * 상위 32비트를 담는 확장으로 소비되므로, __pci_read_base() 는
		 * 1 을 반환해 호출자에게 "BAR 두 칸을 썼다"고 알린다.
		 * NVMe BAR0 이 보통 이 형태다. */
		break;
	default:
		/* mem unknown type treated as 32-bit BAR */
		/* [한국어] 값 0x06 은 스펙상 예약(reserved). 스펙을 어긴 장치를
		 * 만나도 열거가 죽지 않도록 32비트로 간주하고 넘어간다. */
		break;
	}
	return flags;
	/* [한국어] 조립된 플래그. 호출자는 여기에 IORESOURCE_UNSET/SIZEALIGN 등을
	 * 추가로 얹는다. */
}

#define PCI_COMMAND_DECODE_ENABLE	(PCI_COMMAND_MEMORY | PCI_COMMAND_IO)
/* [한국어] Command 레지스터(config offset 0x04)에서 "이 장치가 주소를
 * 디코딩(응답)하도록 켜는" 두 비트를 묶은 편의 매크로.
 * PCI_COMMAND_IO(0x1) = I/O 공간 응답 허용, PCI_COMMAND_MEMORY(0x2) = 메모리
 * 공간 응답 허용.
 * 왜 필요한가: BAR 크기를 재려면 BAR 에 임시로 all-1s 를 써야 하는데, 그
 * 순간 장치가 엉뚱한 주소 범위를 디코딩하게 되어 다른 장치와 충돌할 수 있다.
 * 그래서 pci_read_bases()/pci_setup_device() 는 이 두 비트를 잠시 끄고
 * 크기를 잰 뒤 원래 값으로 되돌린다. */

/**
 * __pci_size_bars - Read the raw BAR mask for a range of PCI BARs
 * @dev: the PCI device
 * @count: number of BARs to size
 * @pos: starting config space position
 * @sizes: array to store mask values
 * @rom: indicate whether to use ROM mask, which avoids enabling ROM BARs
 *
 * Provided @sizes array must be sufficiently sized to store results for
 * @count u32 BARs.  Caller is responsible for disabling decode to specified
 * BAR range around calling this function.  This function is intended to avoid
 * disabling decode around sizing each BAR individually, which can result in
 * non-trivial overhead in virtualized environments with very large PCI BARs.
 */
/*
 * [한국어]
 * __pci_size_bars - 연속한 여러 BAR 에 대해 "모두 1 쓰고 되읽기"를 수행한다
 *
 * @dev:   대상 장치.
 * @count: 처리할 BAR 개수(표준 헤더는 최대 6, 브리지는 2, CardBus 는 1, ROM 은 1).
 * @pos:   첫 BAR 의 config space 오프셋(예: PCI_BASE_ADDRESS_0 = 0x10).
 * @sizes: 되읽은 마스크 값을 담을 u32 배열. 최소 @count 칸이 있어야 한다.
 * @rom:   true 면 옵션 ROM BAR 용 마스크를 쓴다.
 * @return: 없음(결과는 @sizes 로 나간다).
 *
 * 왜 필요한가: BAR 크기 측정 자체는 "쓰고-읽고-복원" 3 단계지만, 그 사이에는
 * 장치의 주소 디코딩을 꺼 두어야 안전하다. 예전 코드는 BAR 하나마다 디코딩을
 * 껐다 켰다 했는데, 위 영어 주석이 말하듯 BAR 가 아주 큰 가상화 환경에서는
 * 그 왕복 비용이 무시할 수 없었다. 그래서 "디코딩 끄기"는 호출자에게 맡기고
 * 이 함수는 마스크 읽기만 연속으로 처리한다.
 *
 * 중요한 계약: 호출자가 반드시 Command 레지스터의 PCI_COMMAND_DECODE_ENABLE
 * 비트를 끈 상태에서 불러야 한다. 켜진 채로 all-1s 를 쓰면 장치가 주소 공간
 * 거의 전체를 디코딩하게 되어 다른 장치의 트랜잭션을 가로챌 수 있다.
 *
 * rom=true 일 때 마스크가 다른 이유: ROM BAR(0x30 또는 0x38)의 bit0 은
 * 주소가 아니라 "ROM 디코딩 활성화"(PCI_ROM_ADDRESS_ENABLE) 비트다. ~0 을
 * 쓰면 그 비트까지 1 이 되어 원치 않게 ROM 을 켜 버리므로,
 * PCI_ROM_ADDRESS_MASK(= ~0x7ff, 주소 비트 31..11) 만 쓴다.
 *
 * 실행 컨텍스트: 프로세스 문맥. config 접근은 pci_read/write_config_dword 를
 * 통하며 이들이 내부적으로 버스 락을 처리한다. 인터럽트 문맥에서는 불리지 않는다.
 *
 * 호출 체인:
 *   pci_read_bases()/pci_setup_device() → __pci_size_stdbars()/__pci_size_rom()
 *     → [__pci_size_bars] → pci_read_config_dword()/pci_write_config_dword()
 */
static void __pci_size_bars(struct pci_dev *dev, int count,
			    unsigned int pos, u32 *sizes, bool rom)
{
	u32 orig, mask = rom ? PCI_ROM_ADDRESS_MASK : ~0;
	/* [한국어] orig = 원래 BAR 값 백업용.
	 * mask = BAR 에 써 넣을 "모두 1" 패턴. 일반 BAR 는 ~0 = 0xFFFFFFFF 를
	 * 그대로 쓰지만, ROM BAR 는 PCI_ROM_ADDRESS_MASK(~0x7ff)만 써서
	 * bit0 의 ROM Enable 을 건드리지 않는다. */
	int i;
	/* [한국어] BAR 순회 인덱스 */

	for (i = 0; i < count; i++, pos += 4, sizes++) {
		/* [한국어] BAR 는 config space 에 4바이트씩 연속으로 놓이므로
		 * pos 를 4 씩 전진시키고 결과 포인터도 한 칸씩 민다. */
		pci_read_config_dword(dev, pos, &orig);
		/* [한국어] 1단계: 원래 값 백업. 펌웨어가 배정해 둔 주소를 잃지 않기 위함 */
		pci_write_config_dword(dev, pos, mask);
		/* [한국어] 2단계: 모두 1 을 써 넣는다. 장치는 자신이 디코딩하는
		 * 상위 비트만 1 로 받아들이고, 크기보다 낮은 하위 비트는 0 으로
		 * 고정해 둔다(하드와이어). */
		pci_read_config_dword(dev, pos, sizes);
		/* [한국어] 3단계: 되읽는다. 얻은 값에서 최하위 1 비트의 자리값이
		 * 곧 BAR 크기다 — 실제 계산은 pci_size() 가 한다. */
		pci_write_config_dword(dev, pos, orig);
		/* [한국어] 4단계: 원래 값 복원. 이 복원을 빼먹으면 장치가 all-1s
		 * 주소를 디코딩한 채로 남아 시스템이 불안정해진다. */
	}
}

/*
 * [한국어]
 * __pci_size_stdbars - 표준(비 ROM) BAR 들의 크기 마스크를 읽는 래퍼
 *
 * @dev:   대상 장치.
 * @count: BAR 개수. header type 0 이면 PCI_STD_NUM_BARS(6), type 1 브리지면 2,
 *         type 2 CardBus 면 1 이 넘어온다.
 * @pos:   첫 BAR 오프셋. 세 헤더 타입 모두 0x10(PCI_BASE_ADDRESS_0)에서 시작한다.
 * @sizes: 결과 마스크 배열.
 * @return: 없음.
 *
 * 왜 필요한가: __pci_size_bars() 의 rom 인자를 false 로 고정해 주는 얇은
 * 래퍼다. static 이 아니라 외부에 노출되어 있는 이유는 이 파일 밖에서도
 * 쓰이기 때문이다(drivers/pci/pci.h 에 선언).
 *
 * 실행 컨텍스트: 프로세스 문맥. 호출자가 디코딩을 꺼 둔 상태여야 한다는
 * __pci_size_bars() 의 계약이 그대로 적용된다.
 *
 * 호출 체인:
 *   pci_setup_device()/pci_read_bases() → [__pci_size_stdbars] → __pci_size_bars()
 */
void __pci_size_stdbars(struct pci_dev *dev, int count,
			unsigned int pos, u32 *sizes)
{
	__pci_size_bars(dev, count, pos, sizes, false);
	/* [한국어] rom=false — ~0(0xFFFFFFFF) 전체를 써 넣는 일반 BAR 방식 */
}

/*
 * [한국어]
 * __pci_size_rom - 옵션 ROM BAR 하나의 크기 마스크를 읽는 래퍼
 *
 * @dev:   대상 장치.
 * @pos:   ROM BAR 오프셋. header type 0 은 PCI_ROM_ADDRESS(0x30),
 *         header type 1(브리지) 은 PCI_ROM_ADDRESS1(0x38) 이다.
 * @sizes: 결과 마스크를 담을 u32 한 칸.
 * @return: 없음.
 *
 * 왜 별도 함수인가: ROM BAR 는 bit0 이 주소가 아니라 ROM Enable
 * (PCI_ROM_ADDRESS_ENABLE) 이라, 일반 BAR 처럼 ~0 을 쓰면 ROM 디코딩이
 * 켜져 버린다. 그래서 rom=true 로 PCI_ROM_ADDRESS_MASK(~0x7ff, 주소 비트
 * 31..11)만 써 넣도록 분기한다. ROM 은 최소 2KB 단위라 하위 11비트가
 * 주소로 쓰이지 않는다.
 *
 * 실행 컨텍스트: 프로세스 문맥, __pci_size_bars() 와 동일한 계약.
 *
 * 호출 체인:
 *   pci_read_bases() → [__pci_size_rom] → __pci_size_bars(rom=true)
 */
static void __pci_size_rom(struct pci_dev *dev, unsigned int pos, u32 *sizes)
{
	__pci_size_bars(dev, 1, pos, sizes, true);
	/* [한국어] count=1 — ROM BAR 는 언제나 32비트 한 칸이다(64비트 확장 없음) */
}

/**
 * __pci_read_base - Read a PCI BAR
 * @dev: the PCI device
 * @type: type of the BAR
 * @res: resource buffer to be filled in
 * @pos: BAR position in the config space
 * @sizes: array of one or more pre-read BAR masks
 *
 * Returns 1 if the BAR is 64-bit, or 0 if 32-bit.
 */
/*
 * [한국어]
 * __pci_read_base - BAR 한 칸(또는 64비트 BAR 두 칸)을 해석해 struct resource 로 만든다
 *
 * @dev:   대상 PCI 장치.
 * @type:  BAR 종류. pci_bar_unknown 이면 하위 비트를 보고 스스로 판별하는
 *         일반 BAR, pci_bar_mem32 등 ROM 계열이면 ROM BAR 로 취급한다.
 * @res:   결과를 채울 struct resource. dev->resource[] 배열의 한 칸이다.
 * @pos:   이 BAR 의 config space 오프셋(0x10, 0x14, … 또는 ROM 의 0x30/0x38).
 * @sizes: 이미 __pci_size_stdbars()/__pci_size_rom() 으로 읽어 둔 "모두 1 쓰고
 *         되읽은" 마스크 배열. 64비트 BAR 면 sizes[0](하위), sizes[1](상위) 두
 *         칸을 쓴다.
 * @return: 이 BAR 가 64비트라 다음 칸까지 소비했으면 1, 32비트면 0.
 *         호출자 pci_read_bases() 는 이 값으로 다음 BAR 인덱스를 얼마나 건너뛸지
 *         결정한다(pos += (rc == 1) ? 8 : 4 형태).
 *
 * 왜 필요한가: BAR 원본 값만으로는 "어디부터 얼마만큼"인지 알 수 없다. 시작
 * 주소는 BAR 값에서 하위 플래그 비트를 지우면 나오지만, 크기는 앞서 잰
 * 마스크로 역산해야 하고, 64비트 BAR 는 두 칸을 합쳐야 하며, 얻은 것은 CPU
 * 주소가 아니라 "버스 주소"라 변환이 한 번 더 필요하다. 이 함수가 그 전부를
 * 처리해 커널이 다룰 수 있는 struct resource 로 정규화한다.
 *
 * 동작 단계:
 *  1) BAR 원본 값 l 을 읽고, 미리 잰 마스크 sz 를 가져온다.
 *  2) 둘 다 all-ones(PCI_POSSIBLE_ERROR) 면 config 접근 실패로 보고 0 으로 만든다.
 *  3) type 에 따라 IO/MEM/ROM 별로 주소 마스크를 골라 l64/sz64/mask64 를 만든다.
 *  4) 64비트 BAR 면 pos+4 칸을 읽어 상위 32비트를 이어 붙인다.
 *  5) pci_size() 로 크기를 역산한다.
 *  6) 32비트 커널에서 4GB 초과/4GB 위 배치는 다룰 수 없으므로 UNSET 처리한다.
 *  7) 버스 주소 → CPU 리소스 주소 변환 후, 되돌려 변환해 왕복 일치를 검증한다.
 *
 * 실행 컨텍스트: 프로세스 문맥(부팅 스캔 또는 핫플러그/rescan). config 접근을
 * 하므로 인터럽트 문맥 금지. 호출자가 Command 의 디코딩 비트를 끈 상태에서
 * 마스크를 미리 읽어 두었다는 전제가 있다.
 *
 * NVMe 접점: NVMe 컨트롤러의 BAR0(대개 64비트 MEM BAR)이 여기서 크기와 타입이
 * 확정되어 dev->resource[0] 에 들어간다. 이후 drivers/nvme/host/pci.c 는
 * pci_resource_start(pdev, 0)/pci_resource_len(pdev, 0) 로 바로 이 resource 를
 * 읽어 ioremap 한다(nvme_remap_bar()). 즉 이 함수가 잰 크기가 NVMe 의 CAP/CC/
 * CSTS 와 0x1000 부터의 doorbell 배열을 담는 MMIO 창의 크기가 된다.
 *
 * 호출 체인:
 *   pci_setup_device() → pci_read_bases() → [__pci_read_base]
 *     → pci_read_config_dword(), decode_bar(), pci_size(),
 *       pcibios_bus_to_resource(), pcibios_resource_to_bus()
 */
int __pci_read_base(struct pci_dev *dev, enum pci_bar_type type,
		    struct resource *res, unsigned int pos, u32 *sizes)
{
	u32 l = 0, sz;
	/* [한국어] l = BAR 에서 읽은 원본 32비트 값(주소 + 하위 플래그).
	 * sz = 미리 재 둔 "모두 1 쓰고 되읽은" 마스크. */
	u64 l64, sz64, mask64;
	/* [한국어] 64비트 BAR 까지 담기 위해 64비트로 확장한 값들.
	 * l64 = 시작 주소, sz64 = 크기 마스크→크기, mask64 = 유효 주소 비트 마스크. */
	struct pci_bus_region region, inverted_region;
	/* [한국어] region      = BAR 가 말하는 "버스 주소" 구간.
	 * inverted_region = CPU 리소스 주소로 바꿨다가 다시 버스 주소로 되돌린 값.
	 * 둘을 비교해 주소 변환이 왕복 일치하는지 검증한다. */
	const char *res_name = pci_resource_name(dev, res - dev->resource);
	/* [한국어] res 포인터와 dev->resource 배열 시작의 차 = BAR 인덱스.
	 * 그 인덱스로 "BAR 0" 같은 사람이 읽을 이름을 얻어 로그에 쓴다. */

	res->name = pci_name(dev);
	/* [한국어] 리소스 이름을 "0000:01:00.0" 형식의 BDF 문자열로 지정.
	 * /proc/iomem 에 이 이름으로 나타난다. */

	pci_read_config_dword(dev, pos, &l);
	/* [한국어] BAR 의 현재 값을 읽는다. 펌웨어가 이미 주소를 배정해 두었다면
	 * 그 주소가, 아니면 0 이 들어 있다. */
	sz = sizes[0];
	/* [한국어] 호출자가 __pci_size_stdbars()/__pci_size_rom() 으로 미리
	 * 읽어 둔 크기 마스크. 이 함수는 다시 하드웨어를 건드리지 않는다. */

	/*
	 * All bits set in sz means the device isn't working properly.
	 * If the BAR isn't implemented, all bits must be 0.  If it's a
	 * memory BAR or a ROM, bit 0 must be clear; if it's an io BAR, bit
	 * 1 must be clear.
	 */
	if (PCI_POSSIBLE_ERROR(sz))
		sz = 0;
	/* [한국어] PCI_POSSIBLE_ERROR(x) 는 x == 0xFFFFFFFF 인지 보는 매크로다.
	 * 마스크가 전부 1 이라는 것은 스펙상 불가능하다 — 미구현 BAR 는 전부 0
	 * 이어야 하고, MEM/ROM 은 bit0 이, IO 는 bit1 이 반드시 0 이기 때문이다.
	 * 따라서 all-ones 는 "config 읽기가 응답을 못 받았다"는 신호이며,
	 * 장치를 뽑았거나 오류 상태다. 0 으로 만들어 아래에서 fail 로 보낸다.
	 * 이는 Vendor ID 0xFFFF 를 "장치 없음"으로 읽는 것과 정확히 같은 원리다. */

	/*
	 * I don't know how l can have all bits set.  Copied from old code.
	 * Maybe it fixes a bug on some ancient platform.
	 */
	if (PCI_POSSIBLE_ERROR(l))
		l = 0;
	/* [한국어] BAR 원본 값도 all-ones 면 0 으로 간주. 위 영어 주석이 솔직히
	 * 밝히듯 상류 개발자도 어떤 하드웨어를 위한 방어인지 모른 채 옛 코드에서
	 * 그대로 가져온 것이다. 근거 없는 설명을 붙이지 않고 사실만 적어 둔다. */

	if (type == pci_bar_unknown) {
		/* [한국어] 일반 BAR — 하위 플래그 비트를 보고 종류를 스스로 판별한다 */
		res->flags = decode_bar(dev, l);
		/* [한국어] IO/MEM, prefetch, 64비트 여부를 리소스 플래그로 번역 */
		res->flags |= IORESOURCE_SIZEALIGN;
		/* [한국어] "이 리소스의 정렬 요구치는 곧 크기와 같다"는 표시.
		 * PCI BAR 는 자기 크기 단위로 자연 정렬되어야 하므로(크기보다 낮은
		 * 주소 비트가 하드와이어 0), 리소스 할당기가 이 규칙을 지키도록 알린다. */
		if (res->flags & IORESOURCE_IO) {
			/* [한국어] I/O 공간 BAR — 하위 2비트만 플래그 */
			l64 = l & PCI_BASE_ADDRESS_IO_MASK;
			/* [한국어] ~0x03 로 하위 2비트를 지워 시작 주소만 남긴다 */
			sz64 = sz & PCI_BASE_ADDRESS_IO_MASK;
			/* [한국어] 마스크에서도 같은 방식으로 플래그 비트 제거 */
			mask64 = PCI_BASE_ADDRESS_IO_MASK & (u32)IO_SPACE_LIMIT;
			/* [한국어] I/O 공간은 아키텍처마다 상한이 다르다(x86 은 0xFFFF).
			 * IO_SPACE_LIMIT 로 유효 주소 비트를 더 좁혀 pci_size() 가
			 * 존재하지 않는 상위 비트를 크기로 오해하지 않게 한다. */
		} else {
			/* [한국어] 메모리 공간 BAR — 하위 4비트가 플래그 */
			l64 = l & PCI_BASE_ADDRESS_MEM_MASK;
			/* [한국어] ~0x0f 로 하위 4비트를 지워 시작 주소만 남긴다.
			 * IO 의 ~0x03 과 다른 이 마스크 폭이 두 공간의 핵심 차이다. */
			sz64 = sz & PCI_BASE_ADDRESS_MEM_MASK;
			/* [한국어] 마스크도 동일하게 정리 */
			mask64 = (u32)PCI_BASE_ADDRESS_MEM_MASK;
			/* [한국어] 우선 하위 32비트만 유효 비트로 둔다. 64비트 BAR 면
			 * 아래에서 상위 32비트를 추가로 켠다. */
		}
	} else {
		/* [한국어] ROM BAR 경로 — 호출자가 type 으로 명시해 준 경우다 */
		if (l & PCI_ROM_ADDRESS_ENABLE)
			/* [한국어] ROM BAR 의 bit0 은 주소가 아니라 "ROM 디코딩 활성"
			 * 비트다. 현재 켜져 있으면 리소스에도 그 사실을 기록해 둔다. */
			res->flags |= IORESOURCE_ROM_ENABLE;
		l64 = l & PCI_ROM_ADDRESS_MASK;
		/* [한국어] ~0x7ff — ROM 은 2KB 단위이므로 하위 11비트가 주소가 아니다 */
		sz64 = sz & PCI_ROM_ADDRESS_MASK;
		/* [한국어] 마스크도 같은 폭으로 정리 */
		mask64 = PCI_ROM_ADDRESS_MASK;
		/* [한국어] ROM 은 언제나 32비트이므로 유효 비트도 여기까지다 */
	}

	if (res->flags & IORESOURCE_MEM_64) {
		/* [한국어] decode_bar() 가 64비트 MEM BAR 로 판별한 경우.
		 * PCI 는 64비트 BAR 를 "연속한 두 개의 32비트 칸"으로 표현하며,
		 * 다음 칸 전체가 주소 상위 32비트다(플래그 비트 없음). */
		pci_read_config_dword(dev, pos + 4, &l);
		/* [한국어] pos+4 = 짝이 되는 다음 BAR 칸. 예: BAR0(0x10) 의 짝은
		 * BAR1(0x14). NVMe 컨트롤러의 BAR0 이 전형적으로 이 형태다. */
		sz = sizes[1];
		/* [한국어] 그 칸의 크기 마스크도 미리 읽어 둔 배열의 두 번째 칸에 있다 */

		l64 |= ((u64)l << 32);
		/* [한국어] 상위 32비트를 왼쪽으로 밀어 시작 주소를 64비트로 완성 */
		sz64 |= ((u64)sz << 32);
		/* [한국어] 크기 마스크도 같은 방식으로 64비트로 확장 */
		mask64 |= ((u64)~0 << 32);
		/* [한국어] 유효 주소 비트 마스크의 상위 32비트를 모두 켠다 —
		 * 이제 상위 절반도 주소로 쓰인다는 뜻. */
	}

	if (!sz64)
		goto fail;
	/* [한국어] 마스크가 전부 0 = 이 BAR 는 구현되어 있지 않다(스펙상 미구현
	 * BAR 는 쓰기를 무시하고 0 만 반환한다). 정상적인 상황이므로 에러 로그
	 * 없이 res->flags 를 0 으로 지우고 끝낸다. */

	sz64 = pci_size(l64, sz64, mask64);
	/* [한국어] 마스크에서 실제 바이트 크기를 역산하는 핵심 호출.
	 * 여기서 나온 값이 그대로 res 의 길이가 된다. */
	if (!sz64) {
		/* [한국어] 마스크는 0 이 아니었는데 크기를 못 구했다 = 펌웨어/하드웨어가
		 * 스펙을 어긴 것이다. FW_BUG 접두어로 펌웨어 결함임을 표시해 보고한다. */
		pci_info(dev, FW_BUG "%s: invalid; can't size\n", res_name);
		goto fail;
	}

	if (res->flags & IORESOURCE_MEM_64) {
		/* [한국어] 64비트 BAR 인데 커널이 32비트 주소만 다룰 수 있는 경우의 방어 */
		if ((sizeof(pci_bus_addr_t) < 8 || sizeof(resource_size_t) < 8)
		    && sz64 > 0x100000000ULL) {
			/* [한국어] 버스 주소 타입이나 리소스 주소 타입이 32비트인데
			 * BAR 크기가 4GB 를 넘으면 표현 자체가 불가능하다. */
			res->flags |= IORESOURCE_UNSET | IORESOURCE_DISABLED;
			/* [한국어] UNSET = 주소가 배정되지 않음, DISABLED = 쓰지 않음.
			 * 리소스 할당기가 이 BAR 를 아예 건너뛰게 만든다. */
			resource_set_range(res, 0, 0);
			/* [한국어] start/end 를 0 으로 만들어 빈 리소스로 둔다 */
			pci_err(dev, "%s: can't handle BAR larger than 4GB (size %#010llx)\n",
				res_name, (unsigned long long)sz64);
			/* [한국어] 이 장치의 해당 BAR 는 사용할 수 없다고 오류로 보고 */
			goto out;
		}

		if ((sizeof(pci_bus_addr_t) < 8) && l) {
			/* Above 32-bit boundary; try to reallocate */
			/* [한국어] 크기는 4GB 이하지만 펌웨어가 배정한 시작 주소가
			 * 4GB 위(상위 32비트 l 이 0 이 아님)인 경우. 32비트 버스 주소
			 * 타입으로는 그 주소를 담을 수 없다. */
			res->flags |= IORESOURCE_UNSET;
			/* [한국어] "주소 미배정" 표시 — DISABLED 는 붙이지 않는다.
			 * 크기는 유효하므로 리소스 할당기가 4GB 아래로 재배치를 시도할 수 있다. */
			resource_set_range(res, 0, sz64);
			/* [한국어] 시작은 0(미정), 길이는 측정한 크기로 남겨 둔다 */
			pci_info(dev, "%s: can't handle BAR above 4GB (bus address %#010llx)\n",
				 res_name, (unsigned long long)l64);
			/* [한국어] 오류가 아니라 재배치 예정이므로 info 수준으로 알린다 */
			goto out;
		}
	}

	region.start = l64;
	/* [한국어] BAR 가 말하는 시작 "버스 주소" */
	region.end = l64 + sz64 - 1;
	/* [한국어] 끝 주소는 포함(inclusive) 표기 — 그래서 -1 */

	pcibios_bus_to_resource(dev->bus, res, &region);
	/* [한국어] 버스 주소 → CPU 물리 주소 변환. 호스트 브리지가 주소를
	 * 오프셋만큼 옮겨 매핑하는 플랫폼(임베디드, 일부 서버)이 있어서
	 * BAR 값이 곧 CPU 주소인 것은 아니다. 변환 결과가 res->start/end 에 들어간다. */
	pcibios_resource_to_bus(dev->bus, &inverted_region, res);
	/* [한국어] 방금 얻은 CPU 주소를 다시 버스 주소로 되돌려 본다(왕복 검증용) */

	/*
	 * If "A" is a BAR value (a bus address), "bus_to_resource(A)" is
	 * the corresponding resource address (the physical address used by
	 * the CPU.  Converting that resource address back to a bus address
	 * should yield the original BAR value:
	 *
	 *     resource_to_bus(bus_to_resource(A)) == A
	 *
	 * If it doesn't, CPU accesses to "bus_to_resource(A)" will not
	 * be claimed by the device.
	 */
	if (inverted_region.start != region.start) {
		/* [한국어] 왕복이 일치하지 않는다 = 펌웨어가 넣어 둔 BAR 값이 이
		 * 호스트 브리지의 주소 변환 창 밖에 있다는 뜻. 그 주소로 CPU 가
		 * 접근해도 장치가 응답하지 않으므로 초기값을 버려야 한다. */
		res->flags |= IORESOURCE_UNSET;
		/* [한국어] 주소 미배정으로 표시해 리소스 할당기가 새로 배정하게 한다 */
		res->start = 0;
		/* [한국어] 시작 주소는 미정 */
		res->end = region.end - region.start;
		/* [한국어] 길이만 보존한다(= sz64 - 1). 크기와 정렬 정보는 유효하다 */
		pci_info(dev, "%s: initial BAR value %#010llx invalid\n",
			 res_name, (unsigned long long)region.start);
		/* [한국어] 어떤 초기값이 버려졌는지 로그로 남긴다 */
	}

	goto out;
	/* [한국어] 정상 경로 종료 — 아래 fail 블록을 건너뛴다 */


fail:
	res->flags = 0;
	/* [한국어] 실패 경로: 플래그를 모두 지워 "이 BAR 는 없는 것"으로 만든다.
	 * 호출자는 flags 가 0 인 리소스를 무시한다. */
out:
	if (res->flags)
		pci_info(dev, "%s %pR\n", res_name, res);
	/* [한국어] 유효한 BAR 만 부팅 로그에 한 줄씩 남긴다. %pR 은 리소스를
	 * "[mem 0xf7e00000-0xf7e03fff 64bit]" 형태로 찍는 커널 포맷 지정자다. */

	return (res->flags & IORESOURCE_MEM_64) ? 1 : 0;
	/* [한국어] 64비트 BAR 였으면 1 — 호출자에게 "다음 칸도 내가 썼다"고 알려
	 * BAR 인덱스를 두 칸 전진시키게 한다. 32비트면 0. */
}

/*
 * [한국어]
 * pci_read_bases - 한 장치의 모든 BAR(및 옵션 ROM BAR)를 읽어 리소스로 만든다
 *
 * @dev:     대상 장치.
 * @howmany: 이 헤더 타입에서 유효한 BAR 칸 수.
 *           header type 0(일반 장치)=PCI_STD_NUM_BARS(6),
 *           header type 1(브리지)=2, header type 2(CardBus)=1.
 * @rom:     옵션 ROM BAR 의 config 오프셋. 0 이면 ROM BAR 를 읽지 않는다.
 *           type 0 은 PCI_ROM_ADDRESS(0x30), type 1 은 PCI_ROM_ADDRESS1(0x38).
 * @return: 없음. 결과는 dev->resource[] 배열에 채워진다.
 *
 * 왜 필요한가: BAR 크기 측정은 BAR 에 임시로 all-1s 를 써야 하는 파괴적
 * 동작이라, 그동안 장치가 주소를 디코딩하고 있으면 위험하다. 이 함수는
 * "디코딩 끄기 → 모든 BAR 마스크 한꺼번에 읽기 → 디코딩 복원 → 해석" 순서로
 * 그 위험 구간을 최소화한다. 해석 단계에서는 하드웨어를 건드리지 않는다.
 *
 * 실행 컨텍스트: 프로세스 문맥. pci_setup_device() 안에서만 불리며, 그 시점의
 * 장치는 아직 어떤 드라이버에도 바인딩되지 않았으므로 BAR 를 흔들어도 된다.
 *
 * 호출 체인:
 *   pci_setup_device() → [pci_read_bases]
 *     → __pci_size_stdbars()/__pci_size_rom() → __pci_read_base()
 */
static __always_inline void pci_read_bases(struct pci_dev *dev,
					   unsigned int howmany, int rom)
{
	u32 rombar, stdbars[PCI_STD_NUM_BARS];
	/* [한국어] stdbars = 표준 BAR 0~5 의 크기 마스크를 담을 배열.
	 * rombar  = 옵션 ROM BAR 의 크기 마스크.
	 * 디코딩을 끈 짧은 구간 안에서 이 값들을 모두 확보한 뒤,
	 * 디코딩을 되살린 다음에 천천히 해석한다. */
	unsigned int pos, reg;
	/* [한국어] pos = BAR 인덱스 겸 루프 커서. 64비트 BAR 를 만나면 한 칸 더 전진한다.
	 * reg = 그 인덱스에 대응하는 config space 오프셋. */
	u16 orig_cmd;
	/* [한국어] 디코딩을 끄기 전의 Command 레지스터 원본. 나중에 그대로 복원한다.
	 * dev->mmio_always_on 인 장치에서는 대입되지 않으므로, 아래에서 반드시
	 * !dev->mmio_always_on 조건과 함께 검사한다. */

	BUILD_BUG_ON(statically_true(howmany > PCI_STD_NUM_BARS));
	/* [한국어] 컴파일 타임 검사. 이 함수는 __always_inline 이라 howmany 가
	 * 호출 지점의 상수로 접혀 들어온다. 그 상수가 배열 크기를 넘으면
	 * stdbars[] 스택 오버플로가 나므로 빌드 자체를 실패시킨다. */

	if (dev->non_compliant_bars)
		return;
	/* [한국어] 일부 장치(주로 quirk 로 표시)는 BAR 가 스펙을 지키지 않아
	 * 크기 측정을 시도하면 오작동한다. 아예 건너뛴다. */

	/* Per PCIe r4.0, sec 9.3.4.1.11, the VF BARs are all RO Zero */
	if (dev->is_virtfn)
		return;
	/* [한국어] SR-IOV 가상 함수(VF)의 BAR 는 스펙상 읽기 전용 0 이다.
	 * VF 의 실제 주소 창은 PF 의 SR-IOV capability 안 VF BAR 로 정해지며
	 * drivers/pci/iov.c 가 따로 처리한다. 여기서 재려 해 봐야 의미가 없다. */

	/* No printks while decoding is disabled! */
	if (!dev->mmio_always_on) {
		/* [한국어] mmio_always_on 은 "이 장치의 디코딩을 절대 끄면 안 된다"는
		 * 표시(예: 콘솔 출력에 쓰이는 장치, 호스트 브리지 자신). 그런 장치가
		 * 아닐 때만 디코딩을 잠시 끈다. */
		pci_read_config_word(dev, PCI_COMMAND, &orig_cmd);
		/* [한국어] Command 레지스터(offset 0x04) 원본 백업 */
		if (orig_cmd & PCI_COMMAND_DECODE_ENABLE) {
			/* [한국어] IO 또는 MEM 디코딩이 켜져 있을 때만 끌 필요가 있다 */
			pci_write_config_word(dev, PCI_COMMAND,
				orig_cmd & ~PCI_COMMAND_DECODE_ENABLE);
			/* [한국어] 두 비트를 지워 장치가 어떤 주소에도 응답하지 않게 만든다.
			 * 이 구간에서 BAR 에 all-1s 를 써도 다른 장치와 충돌하지 않는다.
			 * 위 영어 주석("No printks while decoding is disabled!")은
			 * 이 구간에서 printk 를 하면 안 된다는 경고다 — 콘솔이 PCI 장치라면
			 * 그 장치의 디코딩이 꺼진 상태라 출력이 멈추거나 행 걸릴 수 있다. */
		}
	}

	__pci_size_stdbars(dev, howmany, PCI_BASE_ADDRESS_0, stdbars);
	/* [한국어] 표준 BAR 들의 크기 마스크를 0x10 부터 howmany 칸 만큼 한꺼번에
	 * 읽어 둔다. 여기서만 하드웨어를 파괴적으로 건드린다. */
	if (rom)
		__pci_size_rom(dev, rom, &rombar);
	/* [한국어] ROM BAR 가 있는 헤더 타입이면 그것도 같은 구간에서 재 둔다.
	 * ROM 은 bit0(Enable) 을 건드리지 않는 별도 마스크를 쓴다. */

	if (!dev->mmio_always_on &&
	    (orig_cmd & PCI_COMMAND_DECODE_ENABLE))
		pci_write_config_word(dev, PCI_COMMAND, orig_cmd);
	/* [한국어] 위험 구간 종료 — Command 를 원래대로 되돌린다.
	 * 두 조건 순서가 중요하다. mmio_always_on 인 장치는 orig_cmd 를 읽은 적이
	 * 없어 값이 미정이므로, 단축 평가로 그 경우를 먼저 걸러 낸다.
	 * 이 시점 이후부터는 printk 를 해도 안전하다. */

	for (pos = 0; pos < howmany; pos++) {
		/* [한국어] BAR 칸을 순회하며 해석한다. 하드웨어 접근은 __pci_read_base()
		 * 안의 읽기뿐이라(쓰기는 없다) 디코딩이 켜진 상태로 해도 안전하다. */
		struct resource *res = &dev->resource[pos];
		/* [한국어] BAR n 은 dev->resource[n] 에 1:1 대응한다.
		 * NVMe 드라이버가 보는 pci_resource_start(pdev, 0) 이 곧 resource[0]. */
		reg = PCI_BASE_ADDRESS_0 + (pos << 2);
		/* [한국어] 인덱스 → config 오프셋 변환. BAR 는 4바이트 간격이므로
		 * pos << 2 (= pos * 4) 를 0x10 에 더한다. */
		pos += __pci_read_base(dev, pci_bar_unknown,
				       res, reg, &stdbars[pos]);
		/* [한국어] 반환값이 1 이면 64비트 BAR 라 다음 칸까지 소비된 것이므로
		 * pos 를 한 칸 더 밀어 그 칸을 별도 BAR 로 다시 해석하지 않게 한다
		 * (루프의 pos++ 와 합쳐 두 칸 전진). NVMe BAR0 이 64비트면 BAR1 칸이
		 * 이렇게 건너뛰어진다. */
	}

	if (rom) {
		/* [한국어] 옵션 ROM BAR 는 표준 BAR 배열 밖의 전용 칸에 저장한다 */
		struct resource *res = &dev->resource[PCI_ROM_RESOURCE];
		/* [한국어] PCI_ROM_RESOURCE = dev->resource[] 에서 ROM 용으로 예약된 인덱스 */
		dev->rom_base_reg = rom;
		/* [한국어] ROM BAR 의 config 오프셋을 기억해 둔다. 나중에
		 * pci_enable_rom()/pci_map_rom() 이 이 오프셋으로 Enable 비트를 켠다. */
		res->flags = IORESOURCE_MEM | IORESOURCE_PREFETCH |
				IORESOURCE_READONLY | IORESOURCE_SIZEALIGN;
		/* [한국어] ROM 은 종류가 이미 정해져 있어 decode_bar() 를 쓰지 않고
		 * 플래그를 직접 세운다. MEM(메모리 공간), PREFETCH(읽기에 부작용 없음),
		 * READONLY(쓰기 불가), SIZEALIGN(크기 단위 정렬). */
		__pci_read_base(dev, pci_bar_mem32, res, rom, &rombar);
		/* [한국어] type 을 pci_bar_mem32 로 넘겨 __pci_read_base() 가
		 * ROM 전용 마스크 경로(PCI_ROM_ADDRESS_MASK)를 타게 한다. */
	}
}

/*
 * [한국어]
 * pci_read_bridge_io - PCI-to-PCI 브리지의 I/O 포워딩 창(window)을 읽어 리소스로 만든다
 *
 * @dev: 브리지 장치(header type 1). 이 브리지의 config space 를 읽는다.
 * @res: 결과를 담을 리소스. 보통 브리지 pci_dev 의 resource[PCI_BRIDGE_IO_WINDOW].
 * @log: true 면 "bridge window ..." 한 줄을 부팅 로그에 남긴다. 같은 창을 여러
 *       번 읽는 경로에서 중복 출력을 막기 위한 스위치다.
 * @return: 없음.
 *
 * 왜 필요한가: 브리지는 BAR 로 자기 주소를 갖는 대신, "이 주소 구간은 내
 * 아래쪽(secondary) 버스로 넘긴다"는 창을 base/limit 쌍으로 표현한다. 커널은
 * 그 창을 알아야 아래 버스에 붙은 장치들의 BAR 를 창 안에 배치할 수 있다.
 * 창 밖에 배치하면 브리지가 트랜잭션을 전달하지 않아 장치가 응답하지 않는다.
 *
 * config space 레이아웃(header type 1 전용):
 *   0x1c PCI_IO_BASE (8bit), 0x1d PCI_IO_LIMIT (8bit)
 *        상위 4비트 = 주소 비트 15:12, 하위 4비트 = 타입(16/32비트 주소)
 *   0x30 PCI_IO_BASE_UPPER16, 0x32 PCI_IO_LIMIT_UPPER16 (32비트 I/O 창일 때만)
 * 즉 I/O 창의 기본 단위(granularity)는 4KB 이고, 주소 하위 12비트는 창에
 * 표현되지 않는다(base 는 0 으로, limit 는 0xfff 로 암묵 확장).
 *
 * 실행 컨텍스트: 프로세스 문맥. config 읽기만 하므로 부작용이 없다.
 *
 * 호출 체인:
 *   pci_read_bridge_windows()/pci_read_bridge_bases() → [pci_read_bridge_io]
 *     → pci_read_config_byte/word(), pcibios_bus_to_resource()
 */
static void pci_read_bridge_io(struct pci_dev *dev, struct resource *res,
			       bool log)
{
	u8 io_base_lo, io_limit_lo;
	/* [한국어] 0x1c/0x1d 에서 읽은 8비트 base/limit 원본(타입 비트 포함) */
	unsigned long io_mask, io_granularity, base, limit;
	/* [한국어] io_mask = 주소 비트만 남기는 마스크,
	 * io_granularity = 창의 최소 단위(4KB 또는 1KB),
	 * base/limit = 조립된 시작/끝 버스 주소. */
	struct pci_bus_region region;
	/* [한국어] 버스 주소 구간. CPU 주소로 변환하기 전의 중간 표현. */

	if (!dev->io_window)
		return;
	/* [한국어] 이 브리지가 I/O 창을 아예 구현하지 않으면(base/limit 가 RO 0)
	 * 읽을 것이 없다. pci_setup_device() 가 미리 판정해 둔 플래그다. */

	io_mask = PCI_IO_RANGE_MASK;
	/* [한국어] ~0x0f — 하위 4비트(타입 필드)를 제외한 주소 비트만 남긴다.
	 * 이 마스크를 쓰면 창이 4KB 단위로 정렬된다. */
	io_granularity = 0x1000;
	/* [한국어] 4KB. 표준 I/O 창의 최소 단위이자 limit 을 확장할 폭이다. */
	if (dev->io_window_1k) {
		/* Support 1K I/O space granularity */
		/* [한국어] 일부 인텔 브리지는 1KB 단위 I/O 창을 지원한다(스펙 확장).
		 * quirk 가 이 플래그를 세워 주면 더 촘촘한 마스크를 쓴다. */
		io_mask = PCI_IO_1K_RANGE_MASK;
		/* [한국어] ~0x03 — 주소 비트를 두 칸 더 살려 1KB 정렬을 가능하게 한다 */
		io_granularity = 0x400;
		/* [한국어] 1KB */
	}

	pci_read_config_byte(dev, PCI_IO_BASE, &io_base_lo);
	/* [한국어] offset 0x1c — I/O 창 시작 주소의 비트 15:12 와 타입 비트 */
	pci_read_config_byte(dev, PCI_IO_LIMIT, &io_limit_lo);
	/* [한국어] offset 0x1d — I/O 창 끝 주소의 비트 15:12 와 타입 비트 */
	base = (io_base_lo & io_mask) << 8;
	/* [한국어] 상위 4비트가 주소 비트 15:12 이므로 8비트 왼쪽 시프트하면
	 * 비트 15:12 자리에 놓인다. 하위 12비트는 0 으로 암묵 확장된다. */
	limit = (io_limit_lo & io_mask) << 8;
	/* [한국어] limit 도 같은 방식. 실제 끝 주소는 아래에서 granularity-1 을 더한다 */

	if ((io_base_lo & PCI_IO_RANGE_TYPE_MASK) == PCI_IO_RANGE_TYPE_32) {
		/* [한국어] 하위 4비트가 0x1 이면 32비트 I/O 주소 창(0x0 이면 16비트).
		 * 32비트면 상위 16비트가 별도 레지스터에 들어 있다. */
		u16 io_base_hi, io_limit_hi;
		/* [한국어] 주소 비트 31:16 을 담는 확장 레지스터 값 */

		pci_read_config_word(dev, PCI_IO_BASE_UPPER16, &io_base_hi);
		/* [한국어] offset 0x30 — I/O base 의 상위 16비트 */
		pci_read_config_word(dev, PCI_IO_LIMIT_UPPER16, &io_limit_hi);
		/* [한국어] offset 0x32 — I/O limit 의 상위 16비트 */
		base |= ((unsigned long) io_base_hi << 16);
		/* [한국어] 상위 16비트를 제자리로 밀어 넣어 32비트 주소 완성 */
		limit |= ((unsigned long) io_limit_hi << 16);
		/* [한국어] limit 도 동일 */
	}

	res->flags = (io_base_lo & PCI_IO_RANGE_TYPE_MASK) | IORESOURCE_IO;
	/* [한국어] 타입 비트(16/32비트)를 그대로 보존한 채 IORESOURCE_IO 를 얹는다.
	 * 보존하는 이유는 나중에 setup-bus.c 가 창을 다시 프로그래밍할 때 같은
	 * 타입으로 되써야 하기 때문이다. */

	if (base <= limit) {
		/* [한국어] base <= limit 이면 창이 실제로 열려 있다는 뜻.
		 * 반대면 "창 없음"을 나타내는 스펙상의 관용 표현이다. */
		region.start = base;
		/* [한국어] 창의 시작 버스 주소 */
		region.end = limit + io_granularity - 1;
		/* [한국어] limit 은 마지막 granule 의 시작 주소만 담고 있으므로,
		 * 실제 끝 주소는 granule 하나(4KB 또는 1KB)를 더해 -1 한 값이다. */
		pcibios_bus_to_resource(dev->bus, res, &region);
		/* [한국어] 버스 주소 → CPU 주소 변환 후 res->start/end 에 기록 */
		if (log)
			pci_info(dev, "  bridge window %pR\n", res);
		/* [한국어] 부팅 로그에 창 범위를 남긴다(중복 방지 위해 log 로 제어) */
	} else {
		/* [한국어] 창이 닫혀 있거나 펌웨어가 설정하지 않은 경우 */
		resource_set_range(res, 0, 0);
		/* [한국어] 빈 리소스로 만든다 */
		res->flags |= IORESOURCE_UNSET | IORESOURCE_DISABLED;
		/* [한국어] UNSET = 주소 미배정(필요하면 setup-bus.c 가 새로 배정),
		 * DISABLED = 현재는 쓰이지 않는 창임을 표시. */
	}
}

/*
 * [한국어]
 * pci_read_bridge_mmio - 브리지의 non-prefetchable 32비트 메모리 창을 읽는다
 *
 * @dev: 브리지 장치(header type 1).
 * @res: 결과를 담을 리소스(보통 resource[PCI_BRIDGE_MEM_WINDOW]).
 * @log: true 면 부팅 로그에 창 범위를 남긴다.
 * @return: 없음.
 *
 * 왜 필요한가: 브리지 아래에 붙은 장치의 MMIO BAR 는 이 창 안에 배치되어야
 * 브리지가 트랜잭션을 아래로 전달한다. NVMe SSD 가 스위치나 Root Port 아래에
 * 있다면, 그 BAR0 은 상위 브리지들의 메모리 창 안에 들어가야 한다.
 *
 * config space 레이아웃(header type 1):
 *   0x20 PCI_MEMORY_BASE (16bit), 0x22 PCI_MEMORY_LIMIT (16bit)
 *        상위 12비트 = 주소 비트 31:20, 하위 4비트 = 예약(0)
 * 따라서 이 창은 언제나 1MB 단위로 정렬되고 크기도 1MB 배수다. 주소 하위
 * 20비트는 표현되지 않아 base 는 0 으로, limit 은 0xfffff 로 암묵 확장된다.
 * 이 창은 32비트 주소 전용이며 prefetchable 이 아니다 — 읽기에 부작용이 있는
 * 일반 MMIO 레지스터(NVMe 의 CAP/CC/CSTS/doorbell 같은 것)가 여기에 놓인다.
 *
 * 실행 컨텍스트: 프로세스 문맥, config 읽기만 수행.
 *
 * 호출 체인:
 *   pci_read_bridge_windows()/pci_read_bridge_bases() → [pci_read_bridge_mmio]
 */
static void pci_read_bridge_mmio(struct pci_dev *dev, struct resource *res,
				 bool log)
{
	u16 mem_base_lo, mem_limit_lo;
	/* [한국어] 0x20/0x22 에서 읽은 16비트 base/limit 원본 */
	unsigned long base, limit;
	/* [한국어] 조립된 시작/끝 버스 주소 */
	struct pci_bus_region region;
	/* [한국어] 버스 주소 구간 — CPU 주소 변환 전의 중간 표현 */

	pci_read_config_word(dev, PCI_MEMORY_BASE, &mem_base_lo);
	/* [한국어] offset 0x20 — 메모리 창 시작 주소의 비트 31:20 */
	pci_read_config_word(dev, PCI_MEMORY_LIMIT, &mem_limit_lo);
	/* [한국어] offset 0x22 — 메모리 창 끝 주소의 비트 31:20 */
	base = ((unsigned long) mem_base_lo & PCI_MEMORY_RANGE_MASK) << 16;
	/* [한국어] ~0x0f 로 예약 비트를 지운 뒤 16비트 왼쪽 시프트 →
	 * 값이 주소 비트 31:20 자리에 놓인다. 하위 20비트는 0. */
	limit = ((unsigned long) mem_limit_lo & PCI_MEMORY_RANGE_MASK) << 16;
	/* [한국어] limit 도 동일하게 조립. 실제 끝은 아래에서 0xfffff 를 더한다 */

	res->flags = (mem_base_lo & PCI_MEMORY_RANGE_TYPE_MASK) | IORESOURCE_MEM;
	/* [한국어] 하위 4비트(스펙상 이 창에서는 예약이라 0)를 보존한 채
	 * IORESOURCE_MEM 을 얹는다. prefetchable 플래그는 붙지 않는다. */

	if (base <= limit) {
		/* [한국어] 창이 실제로 열려 있는 경우 */
		region.start = base;
		/* [한국어] 창의 시작 버스 주소 */
		region.end = limit + 0xfffff;
		/* [한국어] limit 은 마지막 1MB 블록의 시작만 담으므로,
		 * 1MB - 1 = 0xfffff 를 더해야 실제 끝 주소가 된다. */
		pcibios_bus_to_resource(dev->bus, res, &region);
		/* [한국어] 버스 주소 → CPU 물리 주소 변환 후 res 에 기록 */
		if (log)
			pci_info(dev, "  bridge window %pR\n", res);
		/* [한국어] 부팅 로그에 창 범위 기록 */
	} else {
		/* [한국어] base > limit = 창이 닫혀 있다는 스펙상의 표현 */
		resource_set_range(res, 0, 0);
		/* [한국어] 빈 리소스로 만든다 */
		res->flags |= IORESOURCE_UNSET | IORESOURCE_DISABLED;
		/* [한국어] 미배정/비활성 표시 — 나중에 필요하면 setup-bus.c 가
		 * 이 창을 새로 계산해 프로그래밍한다. */
	}
}

/*
 * [한국어]
 * pci_read_bridge_mmio_pref - 브리지의 prefetchable 메모리 창을 읽는다
 *
 * @dev: 브리지 장치(header type 1).
 * @res: 결과를 담을 리소스(보통 resource[PCI_BRIDGE_PREF_MEM_WINDOW]).
 * @log: true 면 부팅 로그에 창 범위를 남긴다.
 * @return: 없음.
 *
 * 왜 별도의 창인가: prefetchable 메모리는 "읽어도 부작용이 없고, 브리지가
 * 미리 읽거나 쓰기를 병합해도 안전한" 영역이다. 그래서 브리지는 이 영역을
 * 일반 MMIO 창과 분리해 관리하며, 이 창만 64비트 주소로 확장할 수 있다.
 * IORESOURCE_PREFETCH 가 붙은 BAR 만 이 창에 배치된다.
 *
 * config space 레이아웃(header type 1):
 *   0x24 PCI_PREF_MEMORY_BASE (16bit), 0x26 PCI_PREF_MEMORY_LIMIT (16bit)
 *        상위 12비트 = 주소 비트 31:20, 하위 4비트 = 타입(0=32비트, 1=64비트)
 *   0x28 PCI_PREF_BASE_UPPER32, 0x2c PCI_PREF_LIMIT_UPPER32 (64비트 창일 때)
 * 창 단위는 1MB 로, non-prefetchable 창과 같다.
 *
 * 실행 컨텍스트: 프로세스 문맥, config 읽기만 수행.
 *
 * 호출 체인:
 *   pci_read_bridge_windows()/pci_read_bridge_bases() → [pci_read_bridge_mmio_pref]
 */
static void pci_read_bridge_mmio_pref(struct pci_dev *dev, struct resource *res,
				      bool log)
{
	u16 mem_base_lo, mem_limit_lo;
	/* [한국어] 0x24/0x26 에서 읽은 16비트 base/limit 원본(타입 비트 포함) */
	u64 base64, limit64;
	/* [한국어] 64비트로 조립한 시작/끝 버스 주소 */
	pci_bus_addr_t base, limit;
	/* [한국어] 플랫폼의 실제 버스 주소 타입으로 좁힌 값.
	 * 32비트 커널에서는 이 타입이 32비트라 잘림이 생길 수 있어 아래에서 검사한다. */
	struct pci_bus_region region;
	/* [한국어] 버스 주소 구간 — CPU 주소 변환 전의 중간 표현 */

	if (!dev->pref_window)
		return;
	/* [한국어] 이 브리지가 prefetchable 창을 구현하지 않으면 읽을 것이 없다.
	 * pci_setup_device() 가 base/limit 에 값을 써 보고 미리 판정해 둔 플래그다. */

	pci_read_config_word(dev, PCI_PREF_MEMORY_BASE, &mem_base_lo);
	/* [한국어] offset 0x24 — prefetchable 창 시작 주소의 비트 31:20 + 타입 */
	pci_read_config_word(dev, PCI_PREF_MEMORY_LIMIT, &mem_limit_lo);
	/* [한국어] offset 0x26 — prefetchable 창 끝 주소의 비트 31:20 + 타입 */
	base64 = (mem_base_lo & PCI_PREF_RANGE_MASK) << 16;
	/* [한국어] ~0x0f 로 타입 비트를 지우고 16비트 시프트 → 주소 비트 31:20 */
	limit64 = (mem_limit_lo & PCI_PREF_RANGE_MASK) << 16;
	/* [한국어] limit 도 동일하게 하위 32비트분을 조립 */

	if ((mem_base_lo & PCI_PREF_RANGE_TYPE_MASK) == PCI_PREF_RANGE_TYPE_64) {
		/* [한국어] 하위 4비트가 0x1 이면 64비트 prefetchable 창.
		 * 이때만 0x28/0x2c 의 상위 32비트 레지스터가 유효하다. */
		u32 mem_base_hi, mem_limit_hi;
		/* [한국어] 주소 비트 63:32 를 담는 확장 레지스터 값 */

		pci_read_config_dword(dev, PCI_PREF_BASE_UPPER32, &mem_base_hi);
		/* [한국어] offset 0x28 — prefetchable base 의 상위 32비트 */
		pci_read_config_dword(dev, PCI_PREF_LIMIT_UPPER32, &mem_limit_hi);
		/* [한국어] offset 0x2c — prefetchable limit 의 상위 32비트 */

		/*
		 * Some bridges set the base > limit by default, and some
		 * (broken) BIOSes do not initialize them.  If we find
		 * this, just assume they are not being used.
		 */
		if (mem_base_hi <= mem_limit_hi) {
			/* [한국어] 상위 32비트가 base <= limit 로 앞뒤가 맞을 때만
			 * 합성한다. 위 영어 주석대로 일부 브리지/BIOS 는 이 값을
			 * 초기화하지 않아 base > limit 인 쓰레기가 들어 있는데,
			 * 그대로 합성하면 엉뚱한 4GB 위 주소가 만들어진다.
			 * 그 경우에는 하위 32비트만 쓴 채 지나간다. */
			base64 |= (u64) mem_base_hi << 32;
			/* [한국어] 상위 32비트를 제자리로 밀어 64비트 시작 주소 완성 */
			limit64 |= (u64) mem_limit_hi << 32;
			/* [한국어] limit 도 동일 */
		}
	}

	base = (pci_bus_addr_t) base64;
	/* [한국어] 플랫폼 버스 주소 타입으로 축소. 32비트 커널이면 상위가 잘린다 */
	limit = (pci_bus_addr_t) limit64;
	/* [한국어] limit 도 동일하게 축소 */

	if (base != base64) {
		/* [한국어] 축소 전후가 다르다 = 실제로 잘렸다 = 이 플랫폼의 버스
		 * 주소 타입으로는 4GB 위의 창을 표현할 수 없다. */
		pci_err(dev, "can't handle bridge window above 4GB (bus address %#010llx)\n",
			(unsigned long long) base64);
		/* [한국어] 오류를 보고하고 창을 설정하지 않은 채 반환한다.
		 * res 는 호출자가 준 상태 그대로 남는다. */
		return;
	}

	res->flags = (mem_base_lo & PCI_PREF_RANGE_TYPE_MASK) | IORESOURCE_MEM |
		     IORESOURCE_PREFETCH;
	/* [한국어] 타입 비트(32/64비트)를 보존하고 MEM + PREFETCH 를 얹는다.
	 * PREFETCH 가 붙어야 prefetchable BAR 들이 이 창에 배치 대상이 된다. */
	if (res->flags & PCI_PREF_RANGE_TYPE_64)
		res->flags |= IORESOURCE_MEM_64;
	/* [한국어] 보존해 둔 타입 비트가 64비트를 뜻하면 커널 어휘의
	 * IORESOURCE_MEM_64 도 함께 세운다. 이 창만이 64비트 BAR 를 4GB 위에
	 * 배치할 수 있는 유일한 통로다. */

	if (base <= limit) {
		/* [한국어] 창이 실제로 열려 있는 경우 */
		region.start = base;
		/* [한국어] 창의 시작 버스 주소 */
		region.end = limit + 0xfffff;
		/* [한국어] 1MB 단위 창이므로 마지막 블록의 끝(0xfffff)까지 포함 */
		pcibios_bus_to_resource(dev->bus, res, &region);
		/* [한국어] 버스 주소 → CPU 물리 주소 변환 후 res 에 기록 */
		if (log)
			pci_info(dev, "  bridge window %pR\n", res);
		/* [한국어] 부팅 로그에 창 범위 기록 */
	} else {
		/* [한국어] base > limit = 창이 닫혀 있음 */
		resource_set_range(res, 0, 0);
		/* [한국어] 빈 리소스로 만든다 */
		res->flags |= IORESOURCE_UNSET | IORESOURCE_DISABLED;
		/* [한국어] 미배정/비활성 표시 */
	}
}

/*
 * [한국어]
 * pci_read_bridge_windows - 브리지가 실제로 어떤 창을 구현하는지 알아내고 읽는다
 *
 * @bridge: 검사할 브리지 장치(header type 1).
 * @return: 없음. 결과는 bridge->io_window / pref_window / pref_64_window
 *          플래그와 로그 출력으로 남는다.
 *
 * 왜 필요한가: 브리지의 I/O 창과 prefetchable 창은 "선택 사항"이다. 구현하지
 * 않은 브리지는 해당 레지스터가 읽기 전용 0 이다. 그런데 구현했더라도 아직
 * 펌웨어가 설정하지 않아 0 으로 읽힐 수 있으므로, 값이 0 이라는 것만으로는
 * 두 경우를 구별할 수 없다. 그래서 이 함수는 **시험 삼아 값을 써 보고 되읽어**
 * 쓰기가 먹히는지로 구현 여부를 판정한다(write-readback probe). 판정 결과는
 * 플래그로 남아, 나중에 pci_read_bridge_io()/mmio_pref() 가 조기 반환할지
 * 결정하고 setup-bus.c 가 창을 배정할지 판단하는 근거가 된다.
 *
 * 주의: 시험 쓰기 후에는 반드시 0 으로 되돌린다. 이 함수는 브리지가 아직
 * 활성화되기 전(pci_setup_device 시점)에 불리므로 이 조작이 안전하다.
 *
 * 실행 컨텍스트: 프로세스 문맥. config 읽기/쓰기를 모두 수행한다.
 *
 * 호출 체인:
 *   pci_setup_device() → [pci_read_bridge_windows]
 *     → pci_read_bridge_io/mmio/mmio_pref()
 */
static void pci_read_bridge_windows(struct pci_dev *bridge)
{
	u32 buses;
	/* [한국어] offset 0x18 부터의 4바이트 — primary/secondary/subordinate
	 * 버스 번호와 secondary latency timer 를 한꺼번에 담는다. */
	u16 io;
	/* [한국어] I/O base/limit(0x1c, 0x1d)을 한 워드로 읽은 값 */
	u32 pmem, tmp;
	/* [한국어] pmem = prefetchable base/limit(0x24, 0x26)을 한 dword 로 읽은 값,
	 * 겸 상위 32비트 시험 때 원본 백업용. tmp = 시험 쓰기 후 되읽은 값. */
	struct resource res;
	/* [한국어] 로그 출력과 각 창 읽기에 재사용하는 임시 리소스.
	 * 여기 담긴 값은 저장되지 않고 출력용으로만 쓰인다. */

	pci_read_config_dword(bridge, PCI_PRIMARY_BUS, &buses);
	/* [한국어] offset 0x18 — 버스 번호 3종을 한 번에 읽는다.
	 * 바이트별로: 0x18=primary, 0x19=secondary, 0x1a=subordinate. */
	res.flags = IORESOURCE_BUS;
	/* [한국어] 이 임시 리소스는 "버스 번호 구간"임을 표시(로그 포맷용) */
	res.start = FIELD_GET(PCI_SECONDARY_BUS_MASK, buses);
	/* [한국어] 마스크 0x0000ff00 — 브리지 바로 아래 버스의 번호.
	 * 이 브리지 아래 트리의 시작점이다. */
	res.end = FIELD_GET(PCI_SUBORDINATE_BUS_MASK, buses);
	/* [한국어] 마스크 0x00ff0000 — 이 브리지 아래에 존재하는 가장 큰 버스 번호.
	 * secondary~subordinate 구간이 이 브리지가 책임지는 버스 번호 범위이며,
	 * 브리지는 이 범위의 config 트랜잭션만 아래로 전달한다. */
	pci_info(bridge, "PCI bridge to %pR%s\n", &res,
		 bridge->transparent ? " (subtractive decode)" : "");
	/* [한국어] "PCI bridge to [bus 02-05]" 형태로 담당 버스 범위를 남긴다.
	 * transparent(subtractive decode) 브리지는 자기 창에 해당하지 않는
	 * 주소도 아래로 흘려보내는 종류라, 그 사실을 함께 표시한다. */

	pci_read_config_word(bridge, PCI_IO_BASE, &io);
	/* [한국어] I/O base/limit 을 한 워드(0x1c~0x1d)로 읽는다 */
	if (!io) {
		/* [한국어] 0 이면 두 가지 가능성이 있다: (가) I/O 창 미구현,
		 * (나) 구현했지만 아직 설정 전. 아래 시험 쓰기로 구별한다. */
		pci_write_config_word(bridge, PCI_IO_BASE, 0xe0f0);
		/* [한국어] 시험 패턴. base 의 상위 니블에 0xe, limit 의 상위 니블에
		 * 0xf 를 넣어 base < limit 인 형태를 만든다(창이 열린 모양).
		 * 구현되지 않은 레지스터라면 이 쓰기가 무시된다. */
		pci_read_config_word(bridge, PCI_IO_BASE, &io);
		/* [한국어] 되읽어 값이 남아 있으면 쓰기 가능한 실제 레지스터다 */
		pci_write_config_word(bridge, PCI_IO_BASE, 0x0);
		/* [한국어] 반드시 원래대로(0) 되돌린다. 시험 값을 남기면 브리지가
		 * 엉뚱한 I/O 범위를 아래로 전달하게 된다. */
	}
	if (io) {
		/* [한국어] 원래 값이 있었거나 시험 쓰기가 먹혔다 = I/O 창 존재 */
		bridge->io_window = 1;
		/* [한국어] 플래그 기록. pci_read_bridge_io() 가 이 값을 보고
		 * 조기 반환할지 결정한다. */
		pci_read_bridge_io(bridge, &res, true);
		/* [한국어] 실제 I/O 창 범위를 파싱해 로그로 남긴다 */
	}

	pci_read_bridge_mmio(bridge, &res, true);
	/* [한국어] non-prefetchable 32비트 메모리 창은 스펙상 필수라
	 * 존재 여부를 시험할 필요 없이 바로 읽는다. */

	/*
	 * DECchip 21050 pass 2 errata: the bridge may miss an address
	 * disconnect boundary by one PCI data phase.  Workaround: do not
	 * use prefetching on this device.
	 */
	if (bridge->vendor == PCI_VENDOR_ID_DEC && bridge->device == 0x0001)
		return;
	/* [한국어] quirk. DEC 21050 pass 2 브리지는 주소 disconnect 경계를
	 * 한 데이터 페이즈 놓치는 하드웨어 결함이 있다. prefetch 를 쓰면 그
	 * 결함이 드러나므로 이 칩에서는 prefetchable 창을 아예 쓰지 않는다.
	 * 즉 pref_window 플래그를 세우지 않은 채 반환한다. */

	pci_read_config_dword(bridge, PCI_PREF_MEMORY_BASE, &pmem);
	/* [한국어] prefetchable base/limit(0x24~0x27)을 한 dword 로 읽는다 */
	if (!pmem) {
		/* [한국어] I/O 창과 같은 이유로 시험 쓰기가 필요하다 */
		pci_write_config_dword(bridge, PCI_PREF_MEMORY_BASE,
					       0xffe0fff0);
		/* [한국어] 시험 패턴. 하위 워드(base) 0xfff0, 상위 워드(limit) 0xffe0
		 * 형태로, 타입 니블까지 1 을 섞어 64비트 지원 여부도 함께 드러나게 한다. */
		pci_read_config_dword(bridge, PCI_PREF_MEMORY_BASE, &pmem);
		/* [한국어] 되읽기 — 남은 비트가 있으면 실제 레지스터가 존재한다 */
		pci_write_config_dword(bridge, PCI_PREF_MEMORY_BASE, 0x0);
		/* [한국어] 원래대로 0 으로 복원 */
	}
	if (!pmem)
		return;
	/* [한국어] 시험 쓰기도 먹히지 않았다 = prefetchable 창 미구현.
	 * pref_window 를 세우지 않고 끝낸다. */

	bridge->pref_window = 1;
	/* [한국어] prefetchable 창이 존재함을 기록.
	 * pci_read_bridge_mmio_pref() 가 이 플래그를 보고 동작한다. */

	if ((pmem & PCI_PREF_RANGE_TYPE_MASK) == PCI_PREF_RANGE_TYPE_64) {
		/* [한국어] 타입 니블이 0x1 = 브리지가 "64비트 prefetchable 창을
		 * 지원한다"고 주장하는 경우. 주장만으로는 믿지 않고 검증한다. */

		/*
		 * Bridge claims to have a 64-bit prefetchable memory
		 * window; verify that the upper bits are actually
		 * writable.
		 */
		pci_read_config_dword(bridge, PCI_PREF_BASE_UPPER32, &pmem);
		/* [한국어] offset 0x28 의 현재 값을 백업(변수 pmem 을 재사용) */
		pci_write_config_dword(bridge, PCI_PREF_BASE_UPPER32,
				       0xffffffff);
		/* [한국어] 상위 32비트에 모두 1 을 써 본다 */
		pci_read_config_dword(bridge, PCI_PREF_BASE_UPPER32, &tmp);
		/* [한국어] 되읽어 하나라도 1 이 남았는지 확인 */
		pci_write_config_dword(bridge, PCI_PREF_BASE_UPPER32, pmem);
		/* [한국어] 백업해 둔 원래 값으로 복원 */
		if (tmp)
			bridge->pref_64_window = 1;
		/* [한국어] 실제로 쓰기가 먹혔을 때만 64비트 창으로 인정한다.
		 * 타입 니블만 64비트라고 해 놓고 상위 레지스터는 RO 0 인
		 * 스펙 위반 브리지가 존재하기 때문이다. 이 플래그가 있어야
		 * 리소스 할당기가 4GB 위에 창을 배치할 수 있다. */
	}

	pci_read_bridge_mmio_pref(bridge, &res, true);
	/* [한국어] 마지막으로 prefetchable 창의 실제 범위를 파싱해 로그로 남긴다 */
}

/*
 * [한국어]
 * pci_read_bridge_bases - 자식 버스의 리소스 배열을 부모 브리지의 창에 연결한다
 *
 * @child: 브리지 아래에 새로 만들어진 자식 버스. child->self 가 그 브리지다.
 * @return: 없음.
 *
 * 왜 필요한가: 버스에 붙은 장치의 BAR 를 배정하려면 "이 버스에서 쓸 수 있는
 * 주소 범위"를 알아야 한다. 그 범위는 곧 부모 브리지의 포워딩 창이다. 이
 * 함수는 자식 버스의 resource[] 슬롯이 부모 브리지 pci_dev 의
 * resource[PCI_BRIDGE_RESOURCES + i] 를 **가리키게** 만든다. 복사가 아니라
 * 포인터 공유이므로, 나중에 setup-bus.c 가 브리지 창을 다시 계산하면 그
 * 변경이 자식 버스에도 즉시 반영된다.
 *
 * NVMe 접점: NVMe SSD 가 Root Port 나 스위치 아래에 있으면, 그 SSD 의 BAR0 은
 * 여기서 연결된 창 안에서만 주소를 받을 수 있다.
 *
 * subtractive decode(transparent) 브리지: 자기 창에 해당하지 않는 주소도
 * 아래로 흘려보내는 종류다. 그런 브리지 아래 버스는 부모 버스의 리소스를
 * 그대로 물려받으므로, 마지막에 부모의 리소스를 추가로 등록한다.
 *
 * 실행 컨텍스트: 프로세스 문맥의 버스 스캔 경로.
 *
 * 호출 체인:
 *   pci_scan_bridge_extend() → pci_alloc_child_bus() → [pci_read_bridge_bases]
 *     → pci_read_bridge_io/mmio/mmio_pref(), pci_bus_add_resource()
 */
void pci_read_bridge_bases(struct pci_bus *child)
{
	struct pci_dev *dev = child->self;
	/* [한국어] 이 자식 버스를 만들어 낸 상위 브리지 장치.
	 * 루트 버스에서는 NULL 이라 아래에서 먼저 걸러 낸다. */
	struct resource *res;
	/* [한국어] subtractive decode 경로에서 부모 버스 리소스를 순회할 커서 */
	int i;
	/* [한국어] 브리지 창 슬롯 인덱스 */

	if (pci_is_root_bus(child))	/* It's a host bus, nothing to read */
		return;
	/* [한국어] 루트 버스는 위에 브리지가 없다. 그 리소스는 호스트 브리지
	 * 드라이버가 pci_add_resource_offset() 등으로 이미 등록해 두었다. */

	pci_info(dev, "PCI bridge to %pR%s\n",
		 &child->busn_res,
		 dev->transparent ? " (subtractive decode)" : "");
	/* [한국어] 이 브리지가 담당하는 자식 버스 번호 범위를 로그로 남긴다.
	 * busn_res 는 pci_alloc_child_bus() 가 채운 secondary~subordinate 구간. */

	pci_bus_remove_resources(child);
	/* [한국어] 재스캔(rescan) 시 이전에 등록된 리소스가 남아 있을 수 있으므로
	 * 먼저 모두 떼어 낸다. 이렇게 해야 아래 재연결이 중복되지 않는다. */
	for (i = 0; i < PCI_BRIDGE_RESOURCE_NUM; i++)
		child->resource[i] = &dev->resource[PCI_BRIDGE_RESOURCES+i];
	/* [한국어] 핵심 연결. 자식 버스의 창 슬롯 i 가 브리지 pci_dev 의
	 * 창 리소스를 직접 가리키게 한다(포인터 공유).
	 * PCI_BRIDGE_RESOURCES 는 pci_dev->resource[] 에서 브리지 창이 시작하는
	 * 인덱스이고, PCI_BRIDGE_RESOURCE_NUM 은 그 개수다.
	 * 순서는 0=I/O 창, 1=MEM 창, 2=prefetchable MEM 창. */

	pci_read_bridge_io(child->self,
			   child->resource[PCI_BUS_BRIDGE_IO_WINDOW], false);
	/* [한국어] I/O 창을 읽어 방금 연결한 리소스에 채운다.
	 * log=false — 같은 창을 pci_read_bridge_windows() 가 이미 찍었으므로
	 * 부팅 로그에 두 번 나오지 않게 한다. */
	pci_read_bridge_mmio(child->self,
			     child->resource[PCI_BUS_BRIDGE_MEM_WINDOW], false);
	/* [한국어] non-prefetchable 32비트 메모리 창을 채운다.
	 * NVMe 컨트롤러의 BAR0 이 배치될 수 있는 대표적인 창이다. */
	pci_read_bridge_mmio_pref(child->self,
				  child->resource[PCI_BUS_BRIDGE_PREF_MEM_WINDOW],
				  false);
	/* [한국어] prefetchable 메모리 창을 채운다. 브리지가 pref_window 를
	 * 세우지 않았다면 이 호출은 즉시 반환한다. */

	if (!dev->transparent)
		return;
	/* [한국어] 일반(positive decode) 브리지는 자기 창 안의 주소만 아래로
	 * 전달하므로 여기서 끝. 아래는 transparent 브리지 전용 처리다. */

	pci_bus_for_each_resource(child->parent, res) {
		/* [한국어] transparent 브리지는 창에 해당하지 않는 주소도 아래로
		 * 흘려보내므로, 부모 버스가 쓸 수 있는 모든 리소스를 이 버스도
		 * 쓸 수 있다. 부모의 리소스를 하나씩 상속시킨다. */
		if (!res || !res->flags)
			continue;
		/* [한국어] 비어 있거나 유효하지 않은 슬롯은 건너뛴다 */

		pci_bus_add_resource(child, res);
		/* [한국어] 부모의 리소스를 자식 버스의 추가 리소스 목록에 등록.
		 * 위의 resource[] 슬롯 연결과 달리 이쪽은 목록에 덧붙이는 방식이다. */
		pci_info(dev, "  bridge window %pR (subtractive decode)\n", res);
		/* [한국어] 상속된 창임을 명시해 로그로 남긴다 */
	}
}

/*
 * [한국어]
 * pci_alloc_bus - 새 PCI 버스를 나타내는 struct pci_bus 를 할당하고 초기화한다
 *
 * @parent: 부모 버스. 루트 버스를 만들 때는 NULL 이다.
 * @return: 0 으로 초기화되고 리스트 헤드가 준비된 pci_bus. 할당 실패 시 NULL.
 *
 * 왜 필요한가: 버스 객체는 두 군데(루트 버스 생성, 브리지 아래 자식 버스 생성)
 * 에서 만들어지는데, 리스트 헤드 초기화 같은 공통 준비를 한곳에 모아 둔
 * 하위 헬퍼다. 여기서는 메모리와 리스트만 준비하고, 버스 번호·부모 연결·
 * device 등록 같은 나머지는 호출자가 채운다.
 *
 * 실행 컨텍스트: 프로세스 문맥. GFP_KERNEL 할당을 하므로 잠들 수 있다.
 *
 * 호출 체인:
 *   pci_register_host_bridge()/pci_alloc_child_bus() → [pci_alloc_bus] → kzalloc_obj
 */
static struct pci_bus *pci_alloc_bus(struct pci_bus *parent)
{
	struct pci_bus *b;
	/* [한국어] 새로 만들 버스 객체 */

	b = kzalloc_obj(*b);
	/* [한국어] 0 초기화 할당. 아래에서 명시적으로 채우지 않는 필드는 모두 0/NULL 이다 */
	if (!b)
		return NULL;
	/* [한국어] 할당 실패 — 호출자가 스캔을 포기한다 */

	INIT_LIST_HEAD(&b->node);
	/* [한국어] 이 버스를 부모의 children 목록(또는 루트면 pci_root_buses)에
	 * 매달 때 쓰는 링크 */
	INIT_LIST_HEAD(&b->children);
	/* [한국어] 이 버스 아래 브리지로 이어지는 자식 버스들의 목록 머리 */
	INIT_LIST_HEAD(&b->devices);
	/* [한국어] 이 버스에 붙은 pci_dev 들의 목록 머리.
	 * pci_device_add() 가 새 장치를 여기에 매단다. NVMe 컨트롤러의 pci_dev 도
	 * 발견되면 자신이 붙은 버스의 이 목록에 들어간다. */
	INIT_LIST_HEAD(&b->slots);
	/* [한국어] 이 버스의 물리 슬롯(struct pci_slot) 목록 머리 — 핫플러그용 */
	INIT_LIST_HEAD(&b->resources);
	/* [한국어] 이 버스가 쓸 수 있는 추가 리소스 목록.
	 * subtractive decode 상속분과 루트 버스의 호스트 창이 여기 들어간다. */
	b->max_bus_speed = PCI_SPEED_UNKNOWN;
	/* [한국어] 이 버스가 낼 수 있는 최대 속도. 아직 모르는 상태로 시작해
	 * pci_set_bus_speed() 가 링크 capability 를 읽어 채운다. */
	b->cur_bus_speed = PCI_SPEED_UNKNOWN;
	/* [한국어] 현재 협상된 속도. 마찬가지로 나중에 채워진다.
	 * NVMe SSD 가 Gen4 x4 로 붙었는지 등은 결국 이 값들로 드러난다. */
#ifdef CONFIG_PCI_DOMAINS_GENERIC
	/* [한국어] 도메인 번호를 커널 공통 로직으로 관리하는 구성일 때만.
	 * (x86 처럼 아키텍처가 자체 관리하는 경우 pci_bus 에 domain_nr 필드가
	 *  다르게 다뤄지므로 이 대입이 필요 없다.) */
	if (parent)
		b->domain_nr = parent->domain_nr;
	/* [한국어] 자식 버스는 부모와 같은 도메인에 속한다. 루트 버스(parent 가
	 * NULL)의 도메인은 호출자가 따로 정해 준다. */
#endif
	return b;
	/* [한국어] 리스트만 준비된 빈 버스. 나머지 필드는 호출자 몫이다 */
}

/*
 * [한국어]
 * pci_release_host_bridge_dev - 호스트 브리지 객체의 마지막 참조가 사라졌을 때의 소멸자
 *
 * @dev: pci_host_bridge 안에 박혀 있는 struct device.
 * @return: 없음.
 *
 * 왜 필요한가: 호스트 브리지도 드라이버 모델 객체라 refcount 로 수명이
 * 관리된다. 여기서 브리지가 들고 있던 주소 변환 정보(windows, dma_ranges)와
 * 에뮬레이션 도메인 번호를 반납해야 재사용/언로드 시 누수가 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 마지막 put_device() 시점에 동기적으로 실행된다.
 *
 * 호출 체인:
 *   put_device(&bridge->dev) → device_release() → [pci_release_host_bridge_dev]
 */
static void pci_release_host_bridge_dev(struct device *dev)
{
	struct pci_host_bridge *bridge = to_pci_host_bridge(dev);
	/* [한국어] 내장 device 주소에서 바깥 host bridge 구조체 복원 */

	if (bridge->release_fn)
		bridge->release_fn(bridge);
	/* [한국어] 호스트 컨트롤러 드라이버가 등록해 둔 자체 정리 콜백이 있으면
	 * 먼저 호출한다(ACPI 정보 해제 등 플랫폼별 처리). */

	pci_free_resource_list(&bridge->windows);
	/* [한국어] 이 브리지의 CPU 주소 ↔ 버스 주소 변환 창 목록 해제.
	 * pcibios_bus_to_resource() 가 참조하던 정보가 여기 있었다. */
	pci_free_resource_list(&bridge->dma_ranges);
	/* [한국어] 장치 → 메모리 방향(DMA)의 주소 변환 범위 목록 해제.
	 * 장치가 DMA 로 접근할 수 있는 주소 창을 기술하던 것이다. */

	/* Host bridges only have domain_nr set in the emulation case */
	if (bridge->domain_nr != PCI_DOMAIN_NR_NOT_SET)
		pci_bus_release_emul_domain_nr(bridge->domain_nr);
	/* [한국어] 에뮬레이션(가상) 도메인 번호를 동적으로 받아 쓴 경우에만
	 * 반납한다. 위 영어 주석대로 그 경우에만 domain_nr 이 설정된다. */

	kfree(bridge);
	/* [한국어] 마지막으로 구조체 자체 해제. 이후 dev 접근 금지 */
}

/*
 * [한국어] pci_host_bridge_groups - 모든 호스트 브리지 device 에 자동 생성되는
 * sysfs 속성 그룹 배열. NULL 로 끝나는 가변 길이 배열이다.
 */
static const struct attribute_group *pci_host_bridge_groups[] = {
#ifdef CONFIG_PCI_IDE
	/* [한국어] PCI IDE(Integrity and Data Encryption, TEE 관련 링크 암호화)
	 * 기능이 빌드에 포함될 때만 그 속성 그룹을 노출한다. */
	&pci_ide_attr_group,
#endif
	NULL
	/* [한국어] 배열 종료 표시. 커널은 NULL 을 만날 때까지 순회한다 */
};

/*
 * [한국어] pci_host_bridge_type - 호스트 브리지 device 의 타입 서술자.
 * 드라이버 모델은 device_type 으로 "이 device 는 어떤 부류인가"를 구분하고,
 * 그 부류에 공통인 sysfs 속성과 소멸자를 붙인다.
 */
static const struct device_type pci_host_bridge_type = {
	.groups = pci_host_bridge_groups,
	/* [한국어] 위에서 정의한 속성 그룹 목록 */
	.release = pci_release_host_bridge_dev,
	/* [한국어] refcount 0 시 호출될 소멸자. pcibus_class 의 dev_release 와
	 * 같은 역할을 device_type 수준에서 한다. */
};

/*
 * [한국어]
 * pci_init_host_bridge - 갓 할당한 호스트 브리지의 기본 필드를 채운다
 *
 * @bridge: 0 으로 초기화된 pci_host_bridge.
 * @return: 없음.
 *
 * 왜 필요한가: 호스트 브리지는 PCI 트리의 뿌리로서 두 가지를 들고 있다.
 * (1) CPU 주소 ↔ 버스 주소 변환 창(windows)과 DMA 범위(dma_ranges) 목록,
 * (2) "이 PCIe 기능을 OS 가 직접 관리해도 되는가"를 나타내는 native_* 플래그.
 * 여기서는 목록을 빈 상태로 만들고 native_* 를 모두 1(=OS 가 관리)로 낙관적
 * 초기화한다. 그다음 ACPI _OSC 협상(drivers/acpi/pci_root.c)이 펌웨어가
 * 소유권을 주장하는 기능에 대해 이 플래그를 0 으로 되돌린다.
 *
 * 왜 낙관적 기본값인가: ACPI 가 없는 플랫폼(DT 기반 임베디드 등)에서는
 * 협상 단계 자체가 없으므로, 기본값이 곧 최종값이다. 그런 시스템에서는
 * OS 가 모든 것을 관리하는 것이 맞다.
 *
 * 실행 컨텍스트: 프로세스 문맥의 호스트 컨트롤러 probe 경로.
 *
 * 호출 체인:
 *   pci_alloc_host_bridge()/devm_pci_alloc_host_bridge() → [pci_init_host_bridge]
 *     → device_initialize()
 */
static void pci_init_host_bridge(struct pci_host_bridge *bridge)
{
	INIT_LIST_HEAD(&bridge->windows);
	/* [한국어] CPU 물리 주소 ↔ PCI 버스 주소 변환 창 목록. 호스트 컨트롤러
	 * 드라이버가 pci_add_resource_offset() 으로 채우며,
	 * pcibios_bus_to_resource()/resource_to_bus() 가 이것을 참조한다. */
	INIT_LIST_HEAD(&bridge->dma_ranges);
	/* [한국어] 장치가 DMA 로 접근 가능한 주소 범위 목록(DT 의 dma-ranges).
	 * NVMe 가 PRP/SGL 로 지정하는 호스트 메모리 주소가 이 범위 안에 있어야
	 * 하드웨어가 실제로 접근할 수 있다. */

	/*
	 * We assume we can manage these PCIe features.  Some systems may
	 * reserve these for use by the platform itself, e.g., an ACPI BIOS
	 * may implement its own AER handling and use _OSC to prevent the
	 * OS from interfering.
	 */
	bridge->native_aer = 1;
	/* [한국어] AER(Advanced Error Reporting)을 OS 가 직접 처리한다는 기본값.
	 * 0 이 되면 drivers/pci/pcie/aer.c 가 서비스를 붙이지 않고, PCIe 오류가
	 * 나도 커널이 아니라 펌웨어가 처리한다. NVMe 컨트롤러의 링크 오류 복구
	 * 경로(err_handler)가 이 플래그에 달려 있다. */
	bridge->native_pcie_hotplug = 1;
	/* [한국어] PCIe native 핫플러그(pciehp)를 OS 가 처리 */
	bridge->native_shpc_hotplug = 1;
	/* [한국어] 구식 SHPC(Standard Hot-Plug Controller) 핫플러그를 OS 가 처리 */
	bridge->native_pme = 1;
	/* [한국어] PME(Power Management Event) 를 OS 가 처리.
	 * 절전 상태의 장치가 깨어나겠다고 알리는 신호다. */
	bridge->native_ltr = 1;
	/* [한국어] LTR(Latency Tolerance Reporting)을 OS 가 설정.
	 * 장치가 "나는 이만큼의 지연은 견딘다"고 알려 플랫폼이 더 깊은 절전
	 * 상태로 갈 수 있게 하는 기능이다. 아래 pci_configure_ltr() 이 쓴다. */
	bridge->native_dpc = 1;
	/* [한국어] DPC(Downstream Port Containment)를 OS 가 처리. 오류가 난
	 * 링크를 즉시 차단해 오염 전파를 막는 기능으로, 오류 발생 시 아래
	 * 장치를 제거·재스캔하는 복구 흐름과 맞물린다. */
	bridge->domain_nr = PCI_DOMAIN_NR_NOT_SET;
	/* [한국어] 도메인 번호 미지정 표시. 에뮬레이션 경로에서만 실제 값이
	 * 들어가며, 소멸자가 이 값으로 반납 여부를 판단한다. */
	bridge->native_cxl_error = 1;
	/* [한국어] CXL 프로토콜 오류 보고를 OS 가 처리 */
	bridge->dev.type = &pci_host_bridge_type;
	/* [한국어] device_type 지정 — 소멸자와 sysfs 속성 그룹이 여기서 온다.
	 * device_initialize() 보다 먼저 세워야 한다. */
	pci_ide_init_host_bridge(bridge);
	/* [한국어] PCI IDE(링크 암호화) 관련 브리지 상태 초기화.
	 * CONFIG_PCI_IDE 가 꺼져 있으면 빈 함수다. */

	device_initialize(&bridge->dev);
	/* [한국어] 드라이버 모델 객체 초기화(kobject refcount 1, 락 준비).
	 * 이 시점 이후로는 kfree 가 아니라 put_device() 로 해제해야 한다. */
}

/*
 * [한국어]
 * pci_alloc_host_bridge - 호스트 브리지 객체를 할당한다
 *
 * @priv: 호출자(호스트 컨트롤러 드라이버)가 구조체 뒤에 덧붙여 쓸 전용
 *        데이터의 바이트 수. 0 이면 덧붙이지 않는다.
 * @return: 초기화된 pci_host_bridge, 실패 시 NULL.
 *
 * 왜 priv 를 뒤에 붙이나: 컨트롤러 드라이버마다 필요한 상태가 달라서
 * 별도 할당을 하면 해제 시점 관리가 번거롭다. 한 덩어리로 할당하면
 * 브리지가 해제될 때 전용 데이터도 함께 사라진다.
 * pci_host_bridge_priv() 가 그 뒤쪽 영역의 포인터를 돌려준다.
 *
 * 실행 컨텍스트: 프로세스 문맥(GFP_KERNEL).
 *
 * 호출 체인:
 *   호스트 컨트롤러 드라이버 probe → [pci_alloc_host_bridge]
 *     → kzalloc(), pci_init_host_bridge()
 */
struct pci_host_bridge *pci_alloc_host_bridge(size_t priv)
{
	struct pci_host_bridge *bridge;
	/* [한국어] 만들 브리지 객체 */

	bridge = kzalloc(sizeof(*bridge) + priv, GFP_KERNEL);
	/* [한국어] 구조체 + 드라이버 전용 영역을 한 덩어리로 할당 */
	if (!bridge)
		return NULL;
	/* [한국어] 메모리 부족 — 호출자는 -ENOMEM 으로 probe 를 실패시킨다 */

	pci_init_host_bridge(bridge);
	/* [한국어] native_* 기본값과 리스트 초기화, device_initialize 수행 */

	return bridge;
}
EXPORT_SYMBOL(pci_alloc_host_bridge);
/* [한국어] 각 호스트 컨트롤러 드라이버(모듈)에서 쓰므로 심볼 공개 */

/*
 * [한국어]
 * devm_pci_alloc_host_bridge_release - devres 가 자동 호출하는 해제 래퍼
 *
 * @data: devm_add_action_or_reset() 에 등록해 둔 pci_host_bridge 포인터.
 * @return: 없음.
 *
 * 왜 필요한가: devres 콜백의 시그니처는 void (*)(void *) 라
 * pci_free_host_bridge() 를 그대로 등록할 수 없다. 타입만 맞춰 주는 얇은 래퍼다.
 *
 * 실행 컨텍스트: 부모 device 가 사라질 때 devres 정리 경로에서 호출된다.
 *
 * 호출 체인:
 *   devres_release_all() → [devm_pci_alloc_host_bridge_release] → pci_free_host_bridge()
 */
static void devm_pci_alloc_host_bridge_release(void *data)
{
	pci_free_host_bridge(data);
	/* [한국어] 참조를 놓아 refcount 가 0 이면 소멸자가 돈다 */
}

/*
 * [한국어]
 * devm_pci_alloc_host_bridge - device 수명에 묶인 호스트 브리지 할당
 *
 * @dev:  이 브리지를 소유할 부모 device(보통 platform_device).
 * @priv: 드라이버 전용 데이터 크기.
 * @return: 초기화된 브리지, 실패 시 NULL.
 *
 * 왜 필요한가: 컨트롤러 드라이버의 probe 가 중간에 실패하거나 remove 될 때
 * 브리지를 잊지 않고 해제하도록, devres(device-managed resource)에 해제
 * 동작을 등록해 둔다. 실패 경로마다 수동으로 free 를 부르지 않아도 된다.
 *
 * 실행 컨텍스트: 프로세스 문맥의 드라이버 probe.
 *
 * 호출 체인:
 *   호스트 컨트롤러 드라이버 probe → [devm_pci_alloc_host_bridge]
 *     → pci_alloc_host_bridge(), devm_add_action_or_reset(), devm_of_pci_bridge_init()
 */
struct pci_host_bridge *devm_pci_alloc_host_bridge(struct device *dev,
						   size_t priv)
{
	int ret;
	/* [한국어] 하위 호출의 성공/실패 코드 */
	struct pci_host_bridge *bridge;
	/* [한국어] 만들 브리지 객체 */

	bridge = pci_alloc_host_bridge(priv);
	/* [한국어] 먼저 일반 할당 */
	if (!bridge)
		return NULL;
	/* [한국어] 메모리 부족 */

	bridge->dev.parent = dev;
	/* [한국어] sysfs 계층에서 이 브리지를 컨트롤러 device 아래에 놓는다 */

	ret = devm_add_action_or_reset(dev, devm_pci_alloc_host_bridge_release,
				       bridge);
	/* [한국어] "dev 가 사라지면 이 브리지를 해제하라"를 devres 에 등록.
	 * _or_reset 접미사는 등록 자체가 실패하면 지금 즉시 해제 동작을
	 * 한 번 실행해 준다는 뜻이라, 아래에서 따로 free 할 필요가 없다. */
	if (ret)
		return NULL;
	/* [한국어] 등록 실패 — bridge 는 위 _or_reset 규칙에 따라 이미 해제되었다 */

	ret = devm_of_pci_bridge_init(dev, bridge);
	/* [한국어] Device Tree 에서 이 브리지의 창(ranges)/dma-ranges 등을 읽어
	 * windows/dma_ranges 목록을 채운다. ACPI 시스템에서는 해당 없음. */
	if (ret)
		return NULL;
	/* [한국어] DT 파싱 실패 — 브리지는 devres 가 나중에 정리한다 */

	return bridge;
}
EXPORT_SYMBOL(devm_pci_alloc_host_bridge);
/* [한국어] 컨트롤러 드라이버 모듈에서 쓰므로 공개 */

/*
 * [한국어]
 * pci_free_host_bridge - 호스트 브리지 참조를 놓는다
 *
 * @bridge: 해제할 브리지.
 * @return: 없음.
 *
 * 왜 kfree 가 아닌가: 브리지는 device refcount 로 관리되므로, 다른 곳에서
 * 아직 참조 중일 수 있다. put_device() 로 참조만 놓고, 마지막 참조였다면
 * 드라이버 모델이 pci_release_host_bridge_dev() 를 불러 실제 해제한다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   드라이버 오류 경로/devm 콜백 → [pci_free_host_bridge] → put_device()
 */
void pci_free_host_bridge(struct pci_host_bridge *bridge)
{
	put_device(&bridge->dev);
	/* [한국어] refcount 감소. 0 이면 device_type->release 가 실행된다 */
}
EXPORT_SYMBOL(pci_free_host_bridge);
/* [한국어] 컨트롤러 드라이버 모듈에서 쓰므로 공개 */

/* Indexed by PCI_X_SSTATUS_FREQ (secondary bus mode and frequency) */
/*
 * [한국어] pcix_bus_speed - PCI-X Secondary Status 레지스터의 4비트 주파수
 * 코드(PCI_X_SSTATUS_FREQ)를 커널의 enum pci_bus_speed 로 옮기는 표.
 * 인덱스가 곧 하드웨어가 보고한 코드값이라 룩업 한 번으로 변환된다.
 * 설정자: 정적 초기화(불변). 읽는 자: pci_set_bus_speed().
 * PCI-X 는 PCIe 이전의 병렬 버스 규격이라 NVMe 와는 직접 관계가 없다.
 */
static const unsigned char pcix_bus_speed[] = {
	PCI_SPEED_UNKNOWN,		/* 0 */
	PCI_SPEED_66MHz_PCIX,		/* 1 */
	PCI_SPEED_100MHz_PCIX,		/* 2 */
	PCI_SPEED_133MHz_PCIX,		/* 3 */
	PCI_SPEED_UNKNOWN,		/* 4 */
	PCI_SPEED_66MHz_PCIX_ECC,	/* 5 */
	PCI_SPEED_100MHz_PCIX_ECC,	/* 6 */
	PCI_SPEED_133MHz_PCIX_ECC,	/* 7 */
	PCI_SPEED_UNKNOWN,		/* 8 */
	PCI_SPEED_66MHz_PCIX_266,	/* 9 */
	PCI_SPEED_100MHz_PCIX_266,	/* A */
	PCI_SPEED_133MHz_PCIX_266,	/* B */
	PCI_SPEED_UNKNOWN,		/* C */
	PCI_SPEED_66MHz_PCIX_533,	/* D */
	PCI_SPEED_100MHz_PCIX_533,	/* E */
	PCI_SPEED_133MHz_PCIX_533	/* F */
	/* [한국어] 값 4/8/C 는 스펙상 예약이라 UNKNOWN 으로 둔다.
	 * 표 크기를 16 으로 채워 두면 4비트 코드에 대해 범위 검사가 필요 없다. */
};

/* Indexed by PCI_EXP_LNKCAP_SLS, PCI_EXP_LNKSTA_CLS */
/*
 * [한국어] pcie_link_speed - PCIe Link Capabilities 의 Supported Link Speeds
 * (LNKCAP.SLS) 또는 Link Status 의 Current Link Speed(LNKSTA.CLS) 4비트 코드를
 * 커널의 속도 enum 으로 옮기는 표. 코드 1 부터 세대가 시작한다:
 *   1 = Gen1 2.5 GT/s, 2 = Gen2 5.0, 3 = Gen3 8.0, 4 = Gen4 16.0,
 *   5 = Gen5 32.0, 6 = Gen6 64.0 GT/s.
 * 설정자: 정적 초기화(불변). 읽는 자: pcie_get_link_speed(),
 * pci_set_bus_speed(), 그리고 다른 파일들(EXPORT 되어 있다).
 * NVMe 접점: NVMe SSD 가 Gen3 x4 로 붙었는지 Gen4 x4 인지가 이 표를 거쳐
 * 사람이 읽는 속도로 바뀌며, 대역폭 부족 경고(pcie_report_downtraining)의
 * 근거가 된다.
 */
const unsigned char pcie_link_speed[] = {
	PCI_SPEED_UNKNOWN,		/* 0 */
	PCIE_SPEED_2_5GT,		/* 1 */
	PCIE_SPEED_5_0GT,		/* 2 */
	PCIE_SPEED_8_0GT,		/* 3 */
	PCIE_SPEED_16_0GT,		/* 4 */
	PCIE_SPEED_32_0GT,		/* 5 */
	PCIE_SPEED_64_0GT,		/* 6 */
	PCI_SPEED_UNKNOWN,		/* 7 */
	PCI_SPEED_UNKNOWN,		/* 8 */
	PCI_SPEED_UNKNOWN,		/* 9 */
	PCI_SPEED_UNKNOWN,		/* A */
	PCI_SPEED_UNKNOWN,		/* B */
	PCI_SPEED_UNKNOWN,		/* C */
	PCI_SPEED_UNKNOWN,		/* D */
	PCI_SPEED_UNKNOWN,		/* E */
	PCI_SPEED_UNKNOWN		/* F */
	/* [한국어] 7~F 는 아직 정의되지 않은 미래 세대 자리. 새 세대가 나오면
	 * 여기에 채워진다. 미리 16칸을 채워 두어 범위 검사를 단순화했다. */
};
EXPORT_SYMBOL_GPL(pcie_link_speed);
/* [한국어] 다른 PCI 하위 모듈(모듈로 빌드될 수 있는 것 포함)에서 참조하므로 공개 */

/**
 * pcie_get_link_speed - Get speed value from PCIe generation number
 * @speed: PCIe speed (1-based: 1 = 2.5GT, 2 = 5GT, ...)
 *
 * Returns the speed value (e.g., PCIE_SPEED_2_5GT) if @speed is valid,
 * otherwise returns PCI_SPEED_UNKNOWN.
 */
/*
 * [한국어]
 * pcie_get_link_speed - PCIe 세대 코드(1-based)를 속도 enum 으로 변환한다
 *
 * @speed: 하드웨어가 보고한 4비트 링크 속도 코드(1 = Gen1 …).
 * @return: 대응하는 PCIE_SPEED_* 값. 표 범위를 벗어나면 PCI_SPEED_UNKNOWN.
 *
 * 왜 필요한가: pcie_link_speed[] 를 직접 인덱싱하면 배열 범위를 넘는 코드에
 * 대해 커널이 잘못된 메모리를 읽는다. 경계 검사를 한곳에 모아 둔 접근자다.
 *
 * 실행 컨텍스트: 어느 문맥에서나 안전한 순수 함수.
 *
 * 호출 체인:
 *   링크 속도를 다루는 여러 호출자 → [pcie_get_link_speed] → pcie_link_speed[]
 */
unsigned char pcie_get_link_speed(unsigned int speed)
{
	if (speed >= ARRAY_SIZE(pcie_link_speed))
		return PCI_SPEED_UNKNOWN;
	/* [한국어] 경계 검사. 인자가 unsigned 라 음수 검사는 필요 없다 */

	return pcie_link_speed[speed];
	/* [한국어] 표 룩업 */
}
EXPORT_SYMBOL_GPL(pcie_get_link_speed);
/* [한국어] 다른 PCI 모듈에서 쓰므로 공개 */

/*
 * [한국어]
 * pci_speed_string - 버스 속도 enum 을 사람이 읽는 문자열로 바꾼다
 *
 * @speed: enum pci_bus_speed 값.
 * @return: "8.0 GT/s PCIe" 같은 정적 문자열. 알 수 없으면 "Unknown".
 *          반환된 포인터는 정적 저장 기간이라 해제하면 안 된다.
 *
 * 왜 필요한가: 부팅 로그와 sysfs 의 current_link_speed 같은 곳에 속도를
 * 표시하려면 사람이 읽는 형태가 필요하다. enum 값이 곧 인덱스가 되도록
 * 표를 배열해 두어 변환이 룩업 한 번이다.
 *
 * 실행 컨텍스트: 순수 함수.
 *
 * 호출 체인:
 *   pci_set_bus_speed()/sysfs 속성 → [pci_speed_string]
 */
const char *pci_speed_string(enum pci_bus_speed speed)
{
	/* Indexed by the pci_bus_speed enum */
	/*
	 * [한국어] 인덱스가 곧 enum pci_bus_speed 값인 문자열 표.
	 * 0x05~0x08 이 NULL 인 이유는 그 자리에 대응하는 enum 값이 정의되어
	 * 있지 않기 때문이다(표의 정렬을 유지하려고 빈칸으로 둔 것).
	 * static 이라 함수를 나가도 유효하다.
	 */
	static const char *speed_strings[] = {
	    "33 MHz PCI",		/* 0x00 */
	    "66 MHz PCI",		/* 0x01 */
	    "66 MHz PCI-X",		/* 0x02 */
	    "100 MHz PCI-X",		/* 0x03 */
	    "133 MHz PCI-X",		/* 0x04 */
	    NULL,			/* 0x05 */
	    NULL,			/* 0x06 */
	    NULL,			/* 0x07 */
	    NULL,			/* 0x08 */
	    "66 MHz PCI-X 266",		/* 0x09 */
	    "100 MHz PCI-X 266",	/* 0x0a */
	    "133 MHz PCI-X 266",	/* 0x0b */
	    "Unknown AGP",		/* 0x0c */
	    "1x AGP",			/* 0x0d */
	    "2x AGP",			/* 0x0e */
	    "4x AGP",			/* 0x0f */
	    "8x AGP",			/* 0x10 */
	    "66 MHz PCI-X 533",		/* 0x11 */
	    "100 MHz PCI-X 533",	/* 0x12 */
	    "133 MHz PCI-X 533",	/* 0x13 */
	    "2.5 GT/s PCIe",		/* 0x14 */
	    "5.0 GT/s PCIe",		/* 0x15 */
	    "8.0 GT/s PCIe",		/* 0x16 */
	    "16.0 GT/s PCIe",		/* 0x17 */
	    "32.0 GT/s PCIe",		/* 0x18 */
	    "64.0 GT/s PCIe",		/* 0x19 */
	    /* [한국어] 0x14 부터가 PCIe 세대들이다. NVMe SSD 의 링크 속도는
	     * 여기서 문자열이 되어 dmesg 와 sysfs 에 나타난다. */
	};

	if (speed < ARRAY_SIZE(speed_strings))
		return speed_strings[speed];
	/* [한국어] 표 안이면 그대로 반환. NULL 칸이 나올 수 있는데, 그 값은
	 * 실제로는 쓰이지 않는 enum 자리라 호출자에 도달하지 않는다. */
	return "Unknown";
	/* [한국어] 표 밖의 값(정의되지 않은 미래 세대 등) */
}
EXPORT_SYMBOL_GPL(pci_speed_string);
/* [한국어] sysfs/핫플러그 등 다른 모듈에서 쓰므로 공개 */

/*
 * [한국어]
 * pcie_update_link_speed - 링크 상태 레지스터를 다시 읽어 버스 속도 정보를 갱신
 *
 * @bus:    갱신할 버스. bus->self 가 이 버스의 상위(upstream) 브리지다.
 * @reason: 왜 갱신하는지(링크 대역폭 변경 알림, 재학습 등). 하위 함수가
 *          로그와 통보 여부를 결정하는 데 쓴다.
 * @return: 없음.
 *
 * 왜 필요한가: 링크 속도/폭은 고정이 아니다. 전원 관리, 오류로 인한 강등,
 * 핫플러그 후 재협상 등으로 실행 중에 바뀐다. 그때마다 커널이 캐시해 둔
 * cur_bus_speed 를 실제 하드웨어 상태로 맞춰 주어야 sysfs 값이 거짓말을
 * 하지 않는다.
 *
 * NVMe 접점: 링크가 Gen4 에서 Gen1 로 강등되면 NVMe 의 실효 대역폭이 그대로
 * 떨어진다. 사용자가 성능 저하를 진단할 때 이 값이 근거가 된다.
 *
 * 실행 컨텍스트: 프로세스 문맥. config 읽기를 수행한다.
 *
 * 호출 체인:
 *   링크 대역폭 변경 처리/핫플러그 → [pcie_update_link_speed]
 *     → pcie_capability_read_word(), __pcie_update_link_speed()
 */
void pcie_update_link_speed(struct pci_bus *bus,
			    enum pcie_link_change_reason reason)
{
	struct pci_dev *bridge = bus->self;
	/* [한국어] 이 버스로 내려오는 링크의 반대편 포트. 링크 상태 레지스터는
	 * 그 브리지의 PCIe capability 안에 있다. */
	u16 linksta, linksta2;
	/* [한국어] Link Status(현재 속도/폭)와 Link Status 2(추가 상태) 값 */

	pcie_capability_read_word(bridge, PCI_EXP_LNKSTA, &linksta);
	/* [한국어] PCIe capability 내 Link Status 레지스터.
	 * CLS(Current Link Speed)와 NLW(Negotiated Link Width)가 들어 있다. */
	pcie_capability_read_word(bridge, PCI_EXP_LNKSTA2, &linksta2);
	/* [한국어] Link Status 2. 신형 세대에서 추가된 상태 비트를 담는다 */

	__pcie_update_link_speed(bus, reason, linksta, linksta2);
	/* [한국어] 읽은 원본 값을 넘겨 bus->cur_bus_speed 등을 실제로 갱신한다.
	 * 레지스터 읽기와 해석을 분리해 둔 이유는, 이미 값을 가진 호출자가
	 * 중복 읽기 없이 밑줄 버전을 직접 부를 수 있게 하기 위해서다. */
}
EXPORT_SYMBOL_GPL(pcie_update_link_speed);
/* [한국어] pciehp 등 다른 모듈에서 쓰므로 공개 */

/*
 * [한국어] agp_speeds - AGP Status 레지스터의 속도 비트를 AGP_* 값으로 옮기는 표.
 * AGP 는 PCIe 이전의 그래픽 전용 버스라 NVMe 와는 무관하며, 오래된 하드웨어
 * 지원을 위해 남아 있는 코드다.
 * 설정자: 정적 초기화. 읽는 자: agp_speed().
 */
static unsigned char agp_speeds[] = {
	AGP_UNKNOWN,
	/* [한국어] 인덱스 0 — 속도를 알 수 없음 */
	AGP_1X,
	/* [한국어] 인덱스 1 — 1배속 */
	AGP_2X,
	/* [한국어] 인덱스 2 — 2배속 */
	AGP_4X,
	/* [한국어] 인덱스 3 — 4배속 */
	AGP_8X
	/* [한국어] 인덱스 4 — 8배속(AGP 3.0) */
};

/*
 * [한국어]
 * agp_speed - AGP 상태 비트 조합을 AGP 속도 등급으로 변환한다
 *
 * @agp3:    AGP 3.0 모드 비트(0 이 아니면 3.0). 호출자가 agpstat & 8 을 넘긴다.
 * @agpstat: 속도 지원 비트 3개(하위 3비트).
 * @return: AGP_UNKNOWN/AGP_1X/2X/4X/8X 중 하나.
 *
 * 왜 필요한가: AGP 는 같은 비트가 1.0 모드와 3.0 모드에서 다른 배속을 뜻한다.
 * 예를 들어 비트 2 는 1.0 에서 4x, 3.0 에서 8x 다. 그 이중 의미를 인덱스
 * 보정으로 처리한다. AGP 는 PCIe 이전 그래픽 버스라 NVMe 와 무관하며,
 * 오래된 시스템 지원을 위해 남은 코드다.
 *
 * 실행 컨텍스트: 순수 함수.
 *
 * 호출 체인:
 *   pci_set_bus_speed() → [agp_speed] → agp_speeds[]
 */
static enum pci_bus_speed agp_speed(int agp3, int agpstat)
{
	int index = 0;
	/* [한국어] agp_speeds[] 표의 인덱스. 0 = AGP_UNKNOWN 이 기본값 */

	if (agpstat & 4)
		index = 3;
	/* [한국어] 최상위 지원 비트가 켜져 있으면 가장 빠른 등급.
	 * 1.0 모드에서 4x, 3.0 모드에서는 아래 보정으로 8x 가 된다. */
	else if (agpstat & 2)
		index = 2;
	/* [한국어] 중간 등급 — 1.0 에서 2x, 3.0 에서 4x */
	else if (agpstat & 1)
		index = 1;
	/* [한국어] 최저 등급 — 1.0 에서 1x */
	else
		goto out;
	/* [한국어] 어떤 속도 비트도 켜져 있지 않다 = 알 수 없음(index 0 유지) */

	if (agp3) {
		/* [한국어] AGP 3.0 모드에서는 같은 비트가 두 배속을 뜻한다 */
		index += 2;
		/* [한국어] 등급을 두 칸 올려 1x→4x, 2x→8x 로 보정 */
		if (index == 5)
			index = 0;
		/* [한국어] index 3(=4x 자리) + 2 = 5 는 표 범위(0~4) 밖이다.
		 * 스펙상 나올 수 없는 조합이므로 UNKNOWN 으로 되돌린다. */
	}

 out:
	return agp_speeds[index];
	/* [한국어] 표 룩업 결과 반환 */
}

/*
 * [한국어]
 * pci_set_bus_speed - 버스의 최대/현재 동작 속도를 상위 브리지에서 알아낸다
 *
 * @bus: 속도를 채울 버스. bus->self 가 이 버스로 내려오는 브리지다.
 * @return: 없음. 결과는 bus->max_bus_speed / bus->cur_bus_speed 에 기록된다.
 *
 * 왜 필요한가: 버스의 속도는 버스 자신이 아니라 그 버스로 이어지는 링크의
 * 성질이다. 그래서 상위 브리지의 capability(AGP / PCI-X / PCIe)를 순서대로
 * 찾아보고 처음 맞는 것으로 속도를 결정한다. 결과는 sysfs 의
 * max_bus_speed/cur_bus_speed 로 노출된다.
 *
 * capability 를 찾는 방식: pci_find_capability() 는 Status 레지스터(0x06)의
 * CAP_LIST 비트를 확인한 뒤 Capabilities Pointer(header type 0/1 은 0x34,
 * CardBus 는 0x14)에서 시작해, 각 항목의 [ID, Next] 2바이트를 읽으며 링크를
 * 따라간다. 무한 루프 방지는 TTL 48회 제한이며,
 * drivers/pci/pci.h 의 PCI_FIND_NEXT_CAP 매크로에 구현되어 있다.
 *
 * NVMe 접점: NVMe SSD 가 붙은 버스라면 PCIe 분기를 타며, LNKCAP 의
 * Supported Link Speeds 가 최대 속도, LNKSTA 의 Current Link Speed 가
 * 현재 속도가 된다. 이 값이 SSD 의 실효 대역폭 상한을 결정한다.
 *
 * 실행 컨텍스트: 프로세스 문맥의 버스 스캔 경로. config 읽기만 수행한다.
 *
 * 호출 체인:
 *   pci_alloc_child_bus() → [pci_set_bus_speed]
 *     → pci_find_capability(), pcie_capability_read_dword(), pcie_update_link_speed()
 */
static void pci_set_bus_speed(struct pci_bus *bus)
{
	struct pci_dev *bridge = bus->self;
	/* [한국어] 이 버스의 상위 브리지. 링크 속도 정보는 여기에 있다 */
	int pos;
	/* [한국어] 찾은 capability 의 config space 오프셋. 0 이면 없음 */

	pos = pci_find_capability(bridge, PCI_CAP_ID_AGP);
	/* [한국어] AGP capability(ID 0x02)를 capability 링크에서 찾는다 */
	if (!pos)
		pos = pci_find_capability(bridge, PCI_CAP_ID_AGP3);
	/* [한국어] 없으면 AGP 3.0 전용 capability(ID 0x0e)도 찾아본다 */
	if (pos) {
		/* [한국어] AGP 브리지인 경우 */
		u32 agpstat, agpcmd;
		/* [한국어] agpstat = 하드웨어가 지원하는 속도, agpcmd = 현재 설정된 속도 */

		pci_read_config_dword(bridge, pos + PCI_AGP_STATUS, &agpstat);
		/* [한국어] capability 시작 오프셋 + PCI_AGP_STATUS = AGP Status 레지스터.
		 * capability 안의 필드는 항상 "capability 시작 + 상대 오프셋"으로 접근한다. */
		bus->max_bus_speed = agp_speed(agpstat & 8, agpstat & 7);
		/* [한국어] 비트3 = AGP 3.0 모드 여부, 하위 3비트 = 지원 속도.
		 * 지원하는 최고 속도가 곧 최대 버스 속도다. */

		pci_read_config_dword(bridge, pos + PCI_AGP_COMMAND, &agpcmd);
		/* [한국어] AGP Command 레지스터 — 실제로 협상된 동작 속도 */
		bus->cur_bus_speed = agp_speed(agpstat & 8, agpcmd & 7);
		/* [한국어] 모드 비트는 status 쪽 것을 그대로 쓰고, 속도 비트만
		 * command 쪽에서 가져와 현재 속도를 구한다. */
	}

	pos = pci_find_capability(bridge, PCI_CAP_ID_PCIX);
	/* [한국어] PCI-X capability(ID 0x07) 탐색 */
	if (pos) {
		/* [한국어] PCI-X 브리지인 경우 */
		u16 status;
		/* [한국어] PCI-X Bridge Secondary Status 레지스터 값 */
		enum pci_bus_speed max;
		/* [한국어] 계산한 최대 속도 */

		pci_read_config_word(bridge, pos + PCI_X_BRIDGE_SSTATUS,
				     &status);
		/* [한국어] secondary(아래쪽) 버스의 상태 — 이 버스의 성질이다.
		 * primary 쪽이 아니라 secondary 를 읽는 것이 핵심이다. */

		if (status & PCI_X_SSTATUS_533MHZ) {
			max = PCI_SPEED_133MHz_PCIX_533;
			/* [한국어] 533MHz 등급 지원 비트 */
		} else if (status & PCI_X_SSTATUS_266MHZ) {
			max = PCI_SPEED_133MHz_PCIX_266;
			/* [한국어] 266MHz 등급 */
		} else if (status & PCI_X_SSTATUS_133MHZ) {
			/* [한국어] 133MHz 등급 — PCI-X 버전에 따라 ECC 유무가 갈린다 */
			if ((status & PCI_X_SSTATUS_VERS) == PCI_X_SSTATUS_V2)
				max = PCI_SPEED_133MHz_PCIX_ECC;
			/* [한국어] PCI-X 2.0 은 ECC 를 쓴다 */
			else
				max = PCI_SPEED_133MHz_PCIX;
			/* [한국어] PCI-X 1.0 */
		} else {
			max = PCI_SPEED_66MHz_PCIX;
			/* [한국어] 어떤 고속 비트도 없으면 최저 등급 66MHz */
		}

		bus->max_bus_speed = max;
		/* [한국어] 최대 속도 기록 */
		bus->cur_bus_speed =
			pcix_bus_speed[FIELD_GET(PCI_X_SSTATUS_FREQ, status)];
		/* [한국어] 현재 동작 주파수는 상태 레지스터의 FREQ 필드에 코드로
		 * 들어 있으므로, 앞서 정의한 pcix_bus_speed[] 표로 변환한다. */

		return;
		/* [한국어] PCI-X 이면 PCIe 일 수 없으므로 여기서 끝낸다 */
	}

	if (pci_is_pcie(bridge)) {
		/* [한국어] PCIe 브리지 — NVMe SSD 가 붙는 현대적인 경로다.
		 * pci_is_pcie() 는 set_pcie_port_type() 이 채워 둔
		 * dev->pcie_cap 이 0 이 아닌지를 본다. */
		u32 linkcap;
		/* [한국어] Link Capabilities 레지스터 값 */

		pcie_capability_read_dword(bridge, PCI_EXP_LNKCAP, &linkcap);
		/* [한국어] PCIe capability 안의 Link Capabilities.
		 * pcie_capability_* 계열은 dev->pcie_cap 을 기준으로 오프셋을
		 * 자동 계산해 주는 접근자다. */
		bus->max_bus_speed = pcie_link_speed[linkcap & PCI_EXP_LNKCAP_SLS];
		/* [한국어] SLS(Supported Link Speeds) 필드는 LNKCAP 의 하위 4비트다.
		 * 그 코드를 pcie_link_speed[] 표로 옮겨 최대 속도를 얻는다.
		 * NVMe SSD 라면 이 값이 Gen3(8 GT/s)나 Gen4(16 GT/s) 등이 된다. */

		pcie_update_link_speed(bus, PCIE_ADD_BUS);
		/* [한국어] 현재 협상된 속도는 LNKSTA 를 읽어야 알 수 있으므로
		 * 전용 함수에 맡긴다. PCIE_ADD_BUS 는 "버스를 새로 추가하며
		 * 처음 읽는 것"이라는 이유 표시로, 불필요한 변경 알림을 막는다. */
	}
}

/*
 * [한국어]
 * pci_host_bridge_msi_domain - 이 버스가 쓸 MSI irq_domain 을 여러 경로로 찾는다
 *
 * @bus: 루트 버스(또는 도메인을 물려받을 버스).
 * @return: 찾은 irq_domain. 못 찾으면 NULL(그 경우 MSI 를 쓸 수 없다).
 *
 * 왜 필요한가: MSI/MSI-X 는 "장치가 특정 주소에 특정 값을 써서 인터럽트를
 * 발생시키는" 방식이라, 그 주소/값을 만들어 주고 리눅스 IRQ 번호로 이어 주는
 * irq_domain 이 필요하다. 그 도메인이 어디서 오는지는 플랫폼마다 달라서
 * 네 가지 경로를 순서대로 시도한다:
 *   1) 호스트 컨트롤러 드라이버가 직접 설정해 둔 것
 *   2) Device Tree 의 msi-parent 연결
 *   3) ACPI(IORT 등)가 기술한 매핑
 *   4) firmware node 로 직접 조회
 *
 * NVMe 접점: NVMe 는 큐마다 MSI-X 벡터를 하나씩 쓰려 하며
 * (drivers/nvme/host/pci.c 의 pci_alloc_irq_vectors_affinity 호출),
 * 그 벡터들은 결국 여기서 찾은 도메인에서 할당된다. 도메인이 없으면
 * 레거시 INTx 로 떨어진다.
 *
 * 실행 컨텍스트: 프로세스 문맥의 루트 버스 등록 경로.
 *
 * 호출 체인:
 *   pci_set_bus_msi_domain() → [pci_host_bridge_msi_domain]
 *     → dev_get_msi_domain(), pci_host_bridge_of/acpi_msi_domain(),
 *       irq_find_matching_fwnode()
 */
static struct irq_domain *pci_host_bridge_msi_domain(struct pci_bus *bus)
{
	struct irq_domain *d;
	/* [한국어] 찾은 도메인을 담을 변수. 못 찾으면 NULL 그대로 반환된다 */

	/* If the host bridge driver sets a MSI domain of the bridge, use it */
	d = dev_get_msi_domain(bus->bridge);
	/* [한국어] 1순위: 호스트 컨트롤러 드라이버가 자신의 device 에 직접
	 * 붙여 둔 도메인. 드라이버가 명시했다면 그것이 가장 정확하다. */

	/*
	 * Any firmware interface that can resolve the msi_domain
	 * should be called from here.
	 */
	if (!d)
		d = pci_host_bridge_of_msi_domain(bus);
	/* [한국어] 2순위: Device Tree 의 msi-parent 프로퍼티를 따라간다.
	 * CONFIG_OF 가 없으면 NULL 을 돌려주는 빈 함수다. */
	if (!d)
		d = pci_host_bridge_acpi_msi_domain(bus);
	/* [한국어] 3순위: ACPI 기반 시스템(IORT 등)의 매핑에서 찾는다 */

	/*
	 * If no IRQ domain was found via the OF tree, try looking it up
	 * directly through the fwnode_handle.
	 */
	if (!d) {
		/* [한국어] 4순위: 위 경로가 모두 실패한 경우의 마지막 시도 */
		struct fwnode_handle *fwnode = pci_root_bus_fwnode(bus);
		/* [한국어] 루트 버스에 대응하는 펌웨어 노드(DT 노드든 ACPI 핸들이든
		 * 공통 추상화). 아키텍처가 제공하지 않으면 NULL. */

		if (fwnode)
			d = irq_find_matching_fwnode(fwnode,
						     DOMAIN_BUS_PCI_MSI);
		/* [한국어] 그 펌웨어 노드에 등록된 도메인 중 "PCI MSI 용"
		 * (DOMAIN_BUS_PCI_MSI 토큰)인 것을 찾는다. 같은 노드에 여러
		 * 종류의 도메인이 붙을 수 있어 토큰으로 구분한다. */
	}

	return d;
	/* [한국어] 네 경로 모두 실패하면 NULL — 이 버스에서는 MSI 를 못 쓴다 */
}

/*
 * [한국어]
 * pci_set_bus_msi_domain - 버스에 MSI 도메인을 정해 붙인다
 *
 * @bus: 도메인을 정할 버스.
 * @return: 없음. 결과는 bus->dev 의 msi_domain 필드에 기록된다.
 *
 * 왜 필요한가: MSI 도메인은 트리를 따라 상속된다. 어떤 브리지가 자기만의
 * 도메인을 갖고 있으면(예: 인터럽트 리매핑을 하는 스위치, VMD 같은 장치)
 * 그 아래 버스는 그 도메인을 써야 한다. 그래서 위로 올라가며 처음 만나는
 * 브리지의 도메인을 쓰고, 끝까지 없으면 루트에서 새로 찾는다.
 *
 * 실행 컨텍스트: 프로세스 문맥의 버스 생성 경로.
 *
 * 호출 체인:
 *   pci_register_host_bridge()/pci_alloc_child_bus() → [pci_set_bus_msi_domain]
 *     → dev_get_msi_domain(), pci_host_bridge_msi_domain(), dev_set_msi_domain()
 */
static void pci_set_bus_msi_domain(struct pci_bus *bus)
{
	struct irq_domain *d;
	/* [한국어] 결정된 도메인 */
	struct pci_bus *b;
	/* [한국어] 위로 거슬러 올라가는 커서. 루프 종료 후에도 쓰이므로
	 * 루프 밖에 선언되어 있다(루트 버스를 가리킨 채 끝난다). */

	/*
	 * The bus can be a root bus, a subordinate bus, or a virtual bus
	 * created by an SR-IOV device.  Walk up to the first bridge device
	 * found or derive the domain from the host bridge.
	 */
	for (b = bus, d = NULL; !d && !pci_is_root_bus(b); b = b->parent) {
		/* [한국어] 도메인을 찾았거나 루트 버스에 닿으면 멈춘다.
		 * 위 영어 주석대로 대상 버스는 루트, 하위 버스, SR-IOV 가 만든
		 * 가상 버스 중 어느 것이든 될 수 있어 일반적인 상향 순회가 필요하다. */
		if (b->self)
			d = dev_get_msi_domain(&b->self->dev);
		/* [한국어] b->self 는 이 버스로 내려오는 브리지 장치.
		 * 그 장치에 개별 MSI 도메인이 붙어 있으면 그것을 쓴다.
		 * SR-IOV 가상 버스처럼 self 가 없는 경우를 위해 NULL 검사가 있다. */
	}

	if (!d)
		d = pci_host_bridge_msi_domain(b);
	/* [한국어] 중간 브리지에서 못 찾았다 — 이때 b 는 루트 버스다.
	 * 루트에서 펌웨어/드라이버 경로로 도메인을 찾는다. */

	dev_set_msi_domain(&bus->dev, d);
	/* [한국어] 버스 device 에 도메인을 기록한다. 이후 이 버스에 붙는 장치들이
	 * pci_dev_msi_domain() 을 통해 이 값을 물려받는다.
	 * NVMe 컨트롤러의 MSI-X 벡터도 이 도메인에서 나온다. */
}

/*
 * [한국어]
 * pci_preserve_config - 펌웨어가 배치해 둔 리소스 설정을 그대로 둘지 판단한다
 *
 * @host_bridge: 판단 대상 호스트 브리지.
 * @return: true 면 커널이 BAR/브리지 창을 재배정하지 않고 펌웨어 설정을
 *          존중한다. false 면 필요에 따라 재배정할 수 있다.
 *
 * 왜 필요한가: 보통 커널은 리소스를 자유롭게 재배정하지만, 그러면 안 되는
 * 경우가 있다. 예를 들어 펌웨어가 이미 특정 주소를 전제로 다른 설정을 해
 * 두었거나, 재배정 도중 화면 출력이 끊기면 곤란한 상황이다. 그런 요구는
 * ACPI 의 _DSM 이나 DT 프로퍼티로 전달되며, 이 함수가 두 경로를 확인한다.
 *
 * NVMe 접점: true 이면 NVMe 컨트롤러의 BAR0 주소가 부팅 시 펌웨어가 정한
 * 값 그대로 유지되므로, 드라이버가 보는 pci_resource_start(pdev, 0) 이
 * 펌웨어 배치와 일치한다.
 *
 * 실행 컨텍스트: 프로세스 문맥의 루트 버스 등록 경로.
 *
 * 호출 체인:
 *   pci_register_host_bridge() → [pci_preserve_config]
 *     → pci_acpi_preserve_config(), of_pci_preserve_config()
 */
static bool pci_preserve_config(struct pci_host_bridge *host_bridge)
{
	if (pci_acpi_preserve_config(host_bridge))
		return true;
	/* [한국어] ACPI 경로에서 "설정을 보존하라"고 지시했으면 즉시 true.
	 * CONFIG_ACPI 가 없으면 false 를 돌려주는 빈 함수다. */

	if (host_bridge->dev.parent && host_bridge->dev.parent->of_node)
		return of_pci_preserve_config(host_bridge->dev.parent->of_node);
	/* [한국어] DT 시스템: 부모 device 에 연결된 DT 노드가 있으면 그 노드의
	 * 프로퍼티를 확인한다. 두 조건을 모두 검사하는 것은 부모가 없거나
	 * DT 노드가 없는 구성(순수 ACPI 등)에서 NULL 역참조를 피하기 위해서다. */

	return false;
	/* [한국어] 아무 지시도 없으면 기본값 — 커널이 필요하면 재배정한다 */
}

/*
 * [한국어]
 * pci_register_host_bridge - 호스트 브리지를 등록하고 그 아래 루트 버스를 만든다
 *
 * @bridge: 호스트 컨트롤러 드라이버가 채워 놓은 pci_host_bridge.
 *          busnr(루트 버스 번호), ops(config 접근 함수), windows(주소 창 목록),
 *          sysdata 가 이미 설정되어 있어야 한다.
 * @return: 0 이면 성공. -ENOMEM(메모리 부족), -EEXIST(같은 도메인/버스 번호가
 *          이미 등록됨), 그 밖에 device_add/register 가 준 음수 errno.
 *
 * 왜 필요한가: PCI 열거의 출발점이다. 이 함수가 끝나야 비로소
 *  - struct pci_bus 하나가 만들어져 sysfs 에 /sys/class/pci_bus/0000:00 로 보이고
 *  - 호스트 브리지가 기술한 주소 창들이 그 버스의 리소스 목록에 올라가며
 *  - MSI 도메인과 NUMA 노드가 정해지고
 *  - 전역 pci_root_buses 목록에 들어간다.
 * 그다음 호출자가 pci_scan_child_bus() 로 실제 장치 스캔을 시작한다.
 *
 * 창(window) 목록을 임시로 옮기는 이유: device_add() 가 실패하면 되돌려야 하고,
 * 성공한 뒤에는 인접한 창을 병합해서 다시 넣기 때문이다. 그 사이 구간에서
 * bridge->windows 는 비어 있다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 마지막에 pci_bus_sem 을 쓰기 모드로 잡아
 * 전역 루트 버스 목록을 갱신한다.
 *
 * 호출 체인:
 *   pci_host_probe()/pci_scan_root_bus_bridge() → [pci_register_host_bridge]
 *     → pci_alloc_bus(), device_add(), device_register(),
 *       pci_bus_add_resource(), pci_bus_insert_busn_res()
 */
static int pci_register_host_bridge(struct pci_host_bridge *bridge)
{
	struct device *parent = bridge->dev.parent;
	/* [한국어] 호스트 컨트롤러 device(플랫폼 장치 등). 없을 수도 있다 */
	struct resource_entry *window, *next, *n;
	/* [한국어] 창 목록 순회용 커서들. n 은 _safe 순회에서 다음 항목을 미리
	 * 보관해 현재 항목을 지워도 안전하게 하는 용도다. */
	struct pci_bus *bus, *b;
	/* [한국어] bus = 새로 만들 루트 버스, b = 중복 검사에서 찾은 기존 버스 */
	resource_size_t offset, next_offset;
	/* [한국어] 창의 CPU 주소 - 버스 주소 차이(오프셋). 병합 가능 판정에 쓴다 */
	LIST_HEAD(resources);
	/* [한국어] bridge->windows 를 잠시 옮겨 둘 지역 리스트 머리 */
	struct resource *res, *next_res;
	/* [한국어] 병합 검사에서 비교할 두 창의 리소스 */
	bool bus_registered = false;
	/* [한국어] bus->dev 를 device_register() 까지 진행했는지 표시.
	 * 오류 경로에서 put_device 를 쓸지 kfree 를 쓸지 가르는 기준이다. */
	char addr[64], *fmt;
	/* [한국어] 로그에 덧붙일 "(bus address [...])" 문자열과 그 포맷 */
	const char *name;
	/* [한국어] 등록된 버스 device 의 이름("0000:00" 형태) */
	int err;
	/* [한국어] 오류 코드 */

	bus = pci_alloc_bus(NULL);
	/* [한국어] 루트 버스이므로 부모 버스는 NULL */
	if (!bus)
		return -ENOMEM;
	/* [한국어] 메모리 부족 — 아직 아무것도 등록하지 않았으므로 그냥 반환 */

	bridge->bus = bus;
	/* [한국어] 브리지가 자기 루트 버스를 가리키게 한다(양방향 연결의 한쪽) */

	bus->sysdata = bridge->sysdata;
	/* [한국어] 아키텍처/컨트롤러 전용 데이터 포인터. config 접근 함수가
	 * 이것을 통해 하드웨어에 접근한다. */
	bus->ops = bridge->ops;
	/* [한국어] 이 버스의 config space 읽기/쓰기 함수 집합(struct pci_ops).
	 * 이후 모든 pci_read_config_* 가 결국 여기로 내려간다. */
	bus->number = bus->busn_res.start = bridge->busnr;
	/* [한국어] 루트 버스 번호를 두 곳에 동시에 설정. number 는 config 주소에
	 * 쓰는 값이고, busn_res.start 는 버스 번호 리소스 구간의 시작이다. */
#ifdef CONFIG_PCI_DOMAINS_GENERIC
	/* [한국어] 도메인 번호를 커널 공통 로직이 관리하는 구성일 때만 */
	if (bridge->domain_nr == PCI_DOMAIN_NR_NOT_SET)
		bus->domain_nr = pci_bus_find_domain_nr(bus, parent);
	/* [한국어] 컨트롤러가 도메인 번호를 지정하지 않았으면 커널이 찾아/할당한다 */
	else
		bus->domain_nr = bridge->domain_nr;
	/* [한국어] 지정했으면 그대로 사용 */
	if (bus->domain_nr < 0) {
		/* [한국어] 음수는 오류 코드다(도메인 번호 고갈 등) */
		err = bus->domain_nr;
		goto free;
		/* [한국어] 아직 아무것도 등록 전이므로 free 경로로 */
	}
#endif

	b = pci_find_bus(pci_domain_nr(bus), bridge->busnr);
	/* [한국어] 같은 도메인에 같은 번호의 버스가 이미 있는지 확인 */
	if (b) {
		/* Ignore it if we already got here via a different bridge */
		dev_dbg(&b->dev, "bus already known\n");
		/* [한국어] 같은 루트 버스를 서로 다른 펌웨어 서술(ACPI 와 DT 등)로
		 * 두 번 등록하려는 경우다. 두 번째 시도는 조용히 거절한다. */
		err = -EEXIST;
		goto free;
	}

	dev_set_name(&bridge->dev, "pci%04x:%02x", pci_domain_nr(bus),
		     bridge->busnr);
	/* [한국어] 호스트 브리지 device 이름 — "pci0000:00" 형태.
	 * device_add() 전에 이름이 정해져 있어야 한다. */

	err = pcibios_root_bridge_prepare(bridge);
	/* [한국어] 아키텍처별 사전 준비 훅(기본은 0 을 돌려주는 __weak 함수).
	 * x86/ACPI 는 여기서 NUMA 노드 등을 채운다. */
	if (err)
		goto free;
	/* [한국어] 준비 실패 — 아직 등록 전이므로 free 로 */

	/* Temporarily move resources off the list */
	list_splice_init(&bridge->windows, &resources);
	/* [한국어] 창 목록을 통째로 지역 리스트로 옮기고 원본은 비운다.
	 * 아래에서 병합·검사한 뒤 유효한 것만 되돌려 놓는다. */
	err = device_add(&bridge->dev);
	/* [한국어] 호스트 브리지를 드라이버 모델에 등록. 이 시점부터 sysfs 에
	 * 보이고, 실패 시에는 put_device 가 아니라 free 경로로 간다
	 * (device_add 가 실패하면 참조가 늘지 않았기 때문). */
	if (err)
		goto free;

	bus->bridge = get_device(&bridge->dev);
	/* [한국어] 버스가 브리지 device 참조를 하나 잡는다.
	 * release_pcibus_dev() 의 put_device(pci_bus->bridge) 와 짝이다. */
	device_enable_async_suspend(bus->bridge);
	/* [한국어] 시스템 절전 시 이 device 의 suspend/resume 을 다른 device 와
	 * 병렬로 수행하도록 허용해 절전 진입/복귀 시간을 줄인다. */
	pci_set_bus_of_node(bus);
	/* [한국어] DT 노드를 버스에 연결(CONFIG_OF 없으면 빈 함수) */
	pci_set_bus_msi_domain(bus);
	/* [한국어] 이 루트 버스의 MSI 도메인을 결정한다. 아래에 붙을 모든 장치,
	 * NVMe 컨트롤러를 포함해, 이 도메인에서 MSI-X 벡터를 받게 된다. */
	if (bridge->msi_domain && !dev_get_msi_domain(&bus->dev) &&
	    !pci_host_of_has_msi_map(parent))
		bus->bus_flags |= PCI_BUS_FLAGS_NO_MSI;
	/* [한국어] 세 조건이 모두 참일 때만 "이 버스는 MSI 불가"로 표시한다.
	 * (1) 컨트롤러가 MSI 도메인이 필요하다고 선언했는데,
	 * (2) 방금 결정한 도메인이 실제로는 NULL 이고,
	 * (3) DT 의 msi-map 으로도 도메인을 찾을 수 없다면
	 * MSI 를 쓸 방법이 없다. 이 플래그가 서면 그 아래 장치는 MSI/MSI-X 를
	 * 할당받지 못하고 레거시 INTx 로 떨어진다. */

	if (!parent)
		set_dev_node(bus->bridge, pcibus_to_node(bus));
	/* [한국어] 부모 device 가 없으면 NUMA 노드를 상속받을 곳이 없으므로,
	 * 아키텍처가 제공하는 pcibus_to_node() 로 직접 설정한다. */

	bus->dev.class = &pcibus_class;
	/* [한국어] 파일 앞부분에서 정의한 클래스에 소속시킨다.
	 * 소멸자(release_pcibus_dev)와 sysfs 속성이 여기서 온다. */
	bus->dev.parent = bus->bridge;
	/* [한국어] sysfs 상에서 버스를 호스트 브리지 아래에 놓는다 */

	dev_set_name(&bus->dev, "%04x:%02x", pci_domain_nr(bus), bus->number);
	/* [한국어] "0000:00" 형태의 버스 이름 */
	name = dev_name(&bus->dev);
	/* [한국어] 아래 로그에서 재사용할 이름 포인터 */

	err = device_register(&bus->dev);
	/* [한국어] 버스 device 등록(initialize + add). 여기서부터는 실패해도
	 * 객체가 refcount 로 관리되므로 kfree 가 아니라 put_device 로 정리한다. */
	bus_registered = true;
	/* [한국어] err 와 무관하게 세운다 — device_register 는 실패해도 내부적으로
	 * device_initialize 를 마친 상태라, 정리는 반드시 put_device 여야 한다. */
	if (err)
		goto unregister;

	pcibios_add_bus(bus);
	/* [한국어] 아키텍처별 버스 추가 훅(기본은 빈 __weak 함수) */

	if (bus->ops->add_bus) {
		/* [한국어] 컨트롤러 드라이버가 버스 추가 시 할 일을 등록해 두었으면 */
		err = bus->ops->add_bus(bus);
		if (WARN_ON(err < 0))
			dev_err(&bus->dev, "failed to add bus: %d\n", err);
		/* [한국어] 실패해도 등록을 되돌리지는 않는다. 심각한 문제이므로
		 * WARN_ON 으로 스택 트레이스를 남겨 원인 추적을 돕는다. */
	}

	/* Create legacy_io and legacy_mem files for this bus */
	pci_create_legacy_files(bus);
	/* [한국어] /sys/.../legacy_io, legacy_mem 파일 생성. 사용자 공간이
	 * 레거시 ISA 영역에 접근할 수 있게 하는 구식 인터페이스다. */

	if (parent)
		dev_info(parent, "PCI host bridge to bus %s\n", name);
	/* [한국어] 부모 device 문맥으로 로그 — 어느 컨트롤러인지 함께 보인다 */
	else
		pr_info("PCI host bridge to bus %s\n", name);
	/* [한국어] 부모가 없으면 일반 printk 로 */

	if (nr_node_ids > 1 && pcibus_to_node(bus) == NUMA_NO_NODE)
		dev_warn(&bus->dev, "Unknown NUMA node; performance will be reduced\n");
	/* [한국어] NUMA 시스템인데 이 버스가 어느 노드에 속하는지 모르는 경우.
	 * 그러면 커널이 DMA 버퍼와 인터럽트 처리 CPU 를 장치와 같은 노드에
	 * 몰아 줄 수 없어, 원격 노드 접근으로 지연이 늘어난다. NVMe 처럼
	 * DMA 량이 많은 장치에서 특히 체감되므로 경고한다. */

	/* Check if the boot configuration by FW needs to be preserved */
	bridge->preserve_config = pci_preserve_config(bridge);
	/* [한국어] 펌웨어가 배치한 BAR/창 설정을 보존할지 여부를 기록.
	 * setup-bus.c 가 재배정 여부를 판단할 때 이 값을 본다. */

	/* Coalesce contiguous windows */
	resource_list_for_each_entry_safe(window, n, &resources) {
		/* [한국어] 인접하고 성질이 같은 창들을 하나로 합친다. 컨트롤러
		 * 드라이버나 펌웨어가 같은 영역을 여러 조각으로 기술하는 경우가
		 * 있는데, 합쳐 두면 큰 BAR 를 배치할 여지가 생긴다. */
		if (list_is_last(&window->node, &resources))
			break;
		/* [한국어] 마지막 항목은 뒤에 이어 붙일 것이 없다 */

		next = list_next_entry(window, node);
		/* [한국어] 바로 다음 창 */
		offset = window->offset;
		/* [한국어] 현재 창의 CPU-버스 주소 오프셋 */
		res = window->res;
		/* [한국어] 현재 창의 리소스 */
		next_offset = next->offset;
		/* [한국어] 다음 창의 오프셋 */
		next_res = next->res;
		/* [한국어] 다음 창의 리소스 */

		if (res->flags != next_res->flags || offset != next_offset)
			continue;
		/* [한국어] 종류(IO/MEM/prefetch)나 주소 변환 오프셋이 다르면
		 * 합칠 수 없다. 합치면 주소 변환이 깨진다. */

		if (res->end + 1 == next_res->start) {
			/* [한국어] 현재 창의 끝 바로 다음이 다음 창의 시작 = 딱 붙어 있다 */
			next_res->start = res->start;
			/* [한국어] 다음 창을 앞으로 늘려 현재 창을 흡수한다 */
			res->flags = res->start = res->end = 0;
			/* [한국어] 흡수된 창은 전부 0 으로 만들어 "빈 항목" 표시.
			 * 실제 제거는 아래 두 번째 순회에서 한다. */
		}
	}

	/* Add initial resources to the bus */
	resource_list_for_each_entry_safe(window, n, &resources) {
		/* [한국어] 이제 살아남은 창들을 버스의 리소스로 등록한다 */
		offset = window->offset;
		/* [한국어] 주소 변환 오프셋 */
		res = window->res;
		/* [한국어] 창의 리소스 */
		if (!res->flags && !res->start && !res->end) {
			/* [한국어] 위 병합 단계에서 0 으로 만든 빈 항목 */
			release_resource(res);
			/* [한국어] 리소스 트리에서 떼어 낸다 */
			resource_list_destroy_entry(window);
			/* [한국어] 목록 항목과 리소스 메모리를 해제 */
			continue;
		}

		list_move_tail(&window->node, &bridge->windows);
		/* [한국어] 유효한 창은 임시 리스트에서 bridge->windows 로 되돌린다.
		 * 브리지가 해제될 때 pci_free_resource_list 로 정리된다. */

		if (res->flags & IORESOURCE_BUS)
			pci_bus_insert_busn_res(bus, bus->number, res->end);
		/* [한국어] 버스 번호 축의 리소스는 별도 처리 — 이 루트 버스가
		 * 담당할 버스 번호 구간을 도메인 리소스 트리에 등록한다. */
		else
			pci_bus_add_resource(bus, res);
		/* [한국어] MEM/IO 창은 버스의 사용 가능 리소스 목록에 넣는다.
		 * 이 목록이 곧 "이 버스 아래 장치의 BAR 를 배치할 수 있는 범위"이며,
		 * NVMe 컨트롤러의 BAR0 도 결국 이 안에서 주소를 받는다. */

		if (offset) {
			/* [한국어] CPU 주소와 버스 주소가 다른 플랫폼이면, 로그에
			 * 버스 주소도 함께 보여 주는 것이 진단에 도움이 된다. */
			if (resource_type(res) == IORESOURCE_IO)
				fmt = " (bus address [%#06llx-%#06llx])";
			/* [한국어] I/O 는 보통 16비트라 6자리로 충분 */
			else
				fmt = " (bus address [%#010llx-%#010llx])";
			/* [한국어] MEM 은 32비트 폭에 맞춰 10자리 */

			snprintf(addr, sizeof(addr), fmt,
				 (unsigned long long)(res->start - offset),
				 (unsigned long long)(res->end - offset));
			/* [한국어] CPU 주소에서 오프셋을 빼면 버스 주소가 된다 */
		} else
			addr[0] = '\0';
		/* [한국어] 오프셋이 0 이면 두 주소가 같으므로 덧붙일 문자열 없음 */

		dev_info(&bus->dev, "root bus resource %pR%s\n", res, addr);
		/* [한국어] "root bus resource [mem 0x...-0x...]" 형태로 로그 */
	}

	of_pci_make_host_bridge_node(bridge);
	/* [한국어] DT 에 이 호스트 브리지 노드가 없으면 동적으로 만들어 준다
	 * (ACPI 시스템에서 DT 소비자와 호환을 맞추는 경우 등). */

	down_write(&pci_bus_sem);
	/* [한국어] 전역 루트 버스 목록을 보호하는 읽기/쓰기 세마포어를 쓰기
	 * 모드로 잡는다. 다른 CPU 가 동시에 목록을 순회/수정하는 것을 막는다. */
	list_add_tail(&bus->node, &pci_root_buses);
	/* [한국어] 이제 이 루트 버스가 공식적으로 시스템의 일부가 된다 */
	up_write(&pci_bus_sem);
	/* [한국어] 락 해제 */

	return 0;
	/* [한국어] 성공. 호출자는 이어서 pci_scan_child_bus() 로 장치를 훑는다 */

unregister:
	/* [한국어] device_register(&bus->dev) 이후에 실패한 경우의 정리 경로 */
	put_device(&bridge->dev);
	/* [한국어] 위에서 get_device 로 잡은 브리지 참조를 놓는다 */
	device_del(&bridge->dev);
	/* [한국어] device_add 로 올려 둔 브리지를 sysfs 에서 내린다 */
free:
	/* [한국어] 브리지 등록 전(또는 위 unregister 를 거친 뒤)의 공통 정리 */
#ifdef CONFIG_PCI_DOMAINS_GENERIC
	if (bridge->domain_nr == PCI_DOMAIN_NR_NOT_SET)
		pci_bus_release_domain_nr(parent, bus->domain_nr);
	/* [한국어] 우리가 동적으로 받은 도메인 번호였다면 반납한다.
	 * 컨트롤러가 지정해 준 번호였다면 우리 것이 아니므로 건드리지 않는다. */
#endif
	if (bus_registered)
		put_device(&bus->dev);
	/* [한국어] device_register 를 거쳤으면 refcount 로 관리되므로
	 * put_device 를 써야 한다(소멸자가 kfree 를 대신 해 준다). */
	else
		kfree(bus);
	/* [한국어] 아직 등록 전이면 드라이버 모델이 모르는 순수 메모리라
	 * 직접 kfree 한다. 이 둘을 혼동하면 이중 해제나 누수가 난다. */

	return err;
	/* [한국어] 실패 코드를 그대로 상위(pci_host_probe 등)로 전달 */
}

/*
 * [한국어]
 * pci_bridge_child_ext_cfg_accessible - 브리지 아래 버스에서 확장 config 접근이 되는가
 *
 * @bridge: 검사할 브리지 장치.
 * @return: true 면 자식 버스의 장치들도 4KB 확장 config space(오프셋
 *          0x100~0xfff)에 접근할 수 있다. false 면 표준 256바이트만 가능하다.
 *
 * 왜 필요한가: PCI 의 원래 config space 는 256바이트였고, PCIe 가 4KB 로
 * 확장했다. 그런데 확장 영역에 접근하려면 경로 전체가 그 방식을 지원해야
 * 한다. 중간에 구식 PCI 브리지가 끼어 있으면, 그 아래 장치의 확장 영역은
 * 읽을 수 없다. 확장 영역에는 AER, LTR, SR-IOV, ARI, DEV3 같은
 * "Extended Capability" 들이 놓이므로, 접근 가능 여부가 그 기능들의
 * 사용 가능 여부를 결정한다.
 * (주의: MSI-X 는 확장 capability 가 아니라 표준 capability ID 0x11 이므로
 *  256바이트 영역 안에 있다. 확장 접근이 막혀도 MSI-X 자체는 쓸 수 있다.)
 *
 * 판정 순서:
 *  1) 상위 버스에서 이미 막혀 있으면 아래도 당연히 막혀 있다.
 *  2) 양쪽이 모두 PCIe 인 포트(Root Port, 스위치 상/하향 포트)면 통과.
 *  3) 그 밖의 브리지는 PCI-X Mode 2(266/533MHz)를 지원할 때만 가능하다.
 *
 * 실행 컨텍스트: 프로세스 문맥, config 읽기만 수행.
 *
 * 호출 체인:
 *   pci_alloc_child_bus() → [pci_bridge_child_ext_cfg_accessible]
 *     → pci_find_capability(), pci_read_config_dword()
 */
static bool pci_bridge_child_ext_cfg_accessible(struct pci_dev *bridge)
{
	int pos;
	/* [한국어] PCI-X capability 의 config 오프셋. 0 이면 없음 */
	u32 status;
	/* [한국어] PCI-X Status 레지스터 값 */

	/*
	 * If extended config space isn't accessible on a bridge's primary
	 * bus, we certainly can't access it on the secondary bus.
	 */
	if (bridge->bus->bus_flags & PCI_BUS_FLAGS_NO_EXTCFG)
		return false;
	/* [한국어] 이 브리지가 붙어 있는 버스(primary) 자체가 확장 접근 불가면,
	 * 브리지에 도달하는 트랜잭션부터 확장 오프셋을 전달할 수 없다.
	 * 따라서 그 아래는 볼 것도 없이 불가다. */

	/*
	 * PCIe Root Ports and switch ports are PCIe on both sides, so if
	 * extended config space is accessible on the primary, it's also
	 * accessible on the secondary.
	 */
	if (pci_is_pcie(bridge) &&
	    (pci_pcie_type(bridge) == PCI_EXP_TYPE_ROOT_PORT ||
	     pci_pcie_type(bridge) == PCI_EXP_TYPE_UPSTREAM ||
	     pci_pcie_type(bridge) == PCI_EXP_TYPE_DOWNSTREAM))
		return true;
	/* [한국어] 이 세 종류는 위아래가 모두 PCIe 링크다. PCIe 는 config 요청에
	 * 12비트 레지스터 번호를 실어 보내므로 4KB 전체가 자연스럽게 전달된다.
	 * NVMe SSD 는 보통 Root Port 나 스위치 하향 포트 바로 아래에 붙으므로
	 * 이 분기에서 true 가 되어, AER/LTR 같은 확장 capability 를 쓸 수 있다. */

	/*
	 * For the other bridge types:
	 *   - PCI-to-PCI bridges
	 *   - PCIe-to-PCI/PCI-X forward bridges
	 *   - PCI/PCI-X-to-PCIe reverse bridges
	 * extended config space on the secondary side is only accessible
	 * if the bridge supports PCI-X Mode 2.
	 */
	pos = pci_find_capability(bridge, PCI_CAP_ID_PCIX);
	/* [한국어] PCI-X capability(ID 0x07)를 찾는다. 없으면 순수 구식 PCI
	 * 브리지이므로 확장 접근이 불가능하다. */
	if (!pos)
		return false;

	pci_read_config_dword(bridge, pos + PCI_X_STATUS, &status);
	/* [한국어] PCI-X Status 레지스터 읽기 */
	return status & (PCI_X_STATUS_266MHZ | PCI_X_STATUS_533MHZ);
	/* [한국어] 266MHz 또는 533MHz 지원 = PCI-X Mode 2 지원이라는 뜻이고,
	 * Mode 2 부터 확장 config 전달이 규정되어 있다. 둘 중 하나라도 켜져
	 * 있으면 0 이 아닌 값이 되어 true 로 해석된다. */
}

/*
 * [한국어]
 * pci_alloc_child_bus - 브리지 아래에 새 자식 버스를 만들어 등록한다
 *
 * @parent: 브리지가 붙어 있는 상위 버스.
 * @bridge: 이 자식 버스를 만들어 내는 브리지 장치. SR-IOV 가상 버스처럼
 *          실제 브리지가 없는 경우 NULL 이 올 수 있다.
 * @busnr:  이 자식 버스에 배정할 secondary 버스 번호.
 * @return: 등록까지 마친 pci_bus. 실패하면 NULL.
 *
 * 왜 필요한가: 브리지를 발견해 그 아래로 재귀 하강하려면 먼저 "아래 버스"를
 * 표현하는 객체가 있어야 한다. 이 함수는 부모로부터 상속할 것(config 접근
 * 함수, 버스 플래그, sysdata)을 물려주고, 브리지로부터 알아낼 것(속도,
 * 확장 config 가능 여부, 창 리소스)을 채운 뒤 드라이버 모델에 등록한다.
 *
 * 상속되는 것 중 중요한 것: bus_flags 는 PCI_BUS_FLAGS_NO_MSI /
 * NO_EXTCFG 같은 제약을 아래로 전파한다. 상위에서 막힌 기능은 아래에서도
 * 쓸 수 없기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥의 버스 스캔 경로.
 *
 * 호출 체인:
 *   pci_scan_bridge_extend() → pci_add_new_bus() → [pci_alloc_child_bus]
 *     → pci_alloc_bus(), pci_set_bus_speed(), device_register()
 */
static struct pci_bus *pci_alloc_child_bus(struct pci_bus *parent,
					   struct pci_dev *bridge, int busnr)
{
	struct pci_bus *child;
	/* [한국어] 만들 자식 버스 */
	struct pci_host_bridge *host;
	/* [한국어] 이 트리의 루트 호스트 브리지 — child_ops 확인용 */
	int i;
	/* [한국어] 브리지 창 슬롯 순회 인덱스 */
	int ret;
	/* [한국어] 하위 호출의 성공/실패 코드 */

	/* Allocate a new bus and inherit stuff from the parent */
	child = pci_alloc_bus(parent);
	/* [한국어] 리스트 초기화와 도메인 번호 상속까지 수행 */
	if (!child)
		return NULL;
	/* [한국어] 메모리 부족 — 호출자가 스캔을 포기한다 */

	child->parent = parent;
	/* [한국어] 트리에서 위로 올라갈 수 있는 링크 */
	child->sysdata = parent->sysdata;
	/* [한국어] config 접근 함수가 쓰는 컨트롤러 전용 데이터를 그대로 물려받는다 */
	child->bus_flags = parent->bus_flags;
	/* [한국어] NO_MSI / NO_EXTCFG 같은 제약을 아래로 전파.
	 * 상위에서 MSI 를 못 쓰면 아래 NVMe 도 MSI-X 를 못 쓴다. */

	host = pci_find_host_bridge(parent);
	/* [한국어] 트리를 거슬러 올라가 루트의 호스트 브리지를 찾는다 */
	if (host->child_ops)
		child->ops = host->child_ops;
	/* [한국어] 일부 컨트롤러는 루트 버스와 그 아래 버스에서 config 접근
	 * 방식이 다르다(예: 루트는 전용 레지스터, 아래는 ECAM). 그런 경우
	 * 호스트 브리지가 child_ops 를 따로 지정해 둔다. */
	else
		child->ops = parent->ops;
	/* [한국어] 지정이 없으면 부모와 같은 접근 방식을 쓴다 */

	/*
	 * Initialize some portions of the bus device, but don't register
	 * it now as the parent is not properly set up yet.
	 */
	child->dev.class = &pcibus_class;
	/* [한국어] 버스 클래스 소속 — 소멸자와 sysfs 속성이 여기서 온다 */
	dev_set_name(&child->dev, "%04x:%02x", pci_domain_nr(child), busnr);
	/* [한국어] "0000:01" 형태의 이름. 등록 전에 정해 두어야 한다 */

	/* Set up the primary, secondary and subordinate bus numbers */
	child->number = child->busn_res.start = busnr;
	/* [한국어] 이 버스의 번호 = 브리지의 secondary 버스 번호.
	 * config 주소에 쓰는 값과 버스 번호 리소스의 시작을 함께 설정한다. */
	child->primary = parent->busn_res.start;
	/* [한국어] 브리지의 primary(윗쪽) 버스 번호 = 부모 버스의 번호 */
	child->busn_res.end = 0xff;
	/* [한국어] subordinate 는 아직 모른다. 일단 최대값으로 열어 두고,
	 * 아래를 다 훑은 뒤 pci_scan_bridge_extend() 가 실제 최대 버스 번호로
	 * 좁힌다. 이렇게 해야 스캔 도중 아래쪽 버스로 가는 config 요청이
	 * 브리지에서 막히지 않는다. */

	if (!bridge) {
		/* [한국어] 브리지 장치 없이 만들어지는 버스(SR-IOV 가상 버스 등).
		 * 창이나 링크 속도 같은 개념이 없으므로 아래 처리를 모두 건너뛴다. */
		child->dev.parent = parent->bridge;
		/* [한국어] sysfs 부모는 부모 버스의 브리지 device 로 둔다 */
		goto add_dev;
	}

	child->self = bridge;
	/* [한국어] 이 버스로 내려오는 브리지 장치를 기억.
	 * 이후 pci_read_bridge_bases() 등이 이 포인터로 브리지에 접근한다. */
	child->bridge = get_device(&bridge->dev);
	/* [한국어] 브리지 device 참조를 하나 잡는다. release_pcibus_dev() 의
	 * put_device(pci_bus->bridge) 와 짝을 이룬다. */
	child->dev.parent = child->bridge;
	/* [한국어] sysfs 계층에서 이 버스를 브리지 아래에 놓는다 */
	pci_set_bus_of_node(child);
	/* [한국어] DT 노드 연결(CONFIG_OF 없으면 빈 함수) */
	pci_set_bus_speed(child);
	/* [한국어] 브리지의 링크 capability 를 읽어 이 버스의 최대/현재 속도를
	 * 채운다. NVMe SSD 가 붙을 버스라면 여기서 Gen 세대가 정해진다. */

	/*
	 * Check whether extended config space is accessible on the child
	 * bus.  Note that we currently assume it is always accessible on
	 * the root bus.
	 */
	if (!pci_bridge_child_ext_cfg_accessible(bridge)) {
		/* [한국어] 이 아래로는 4KB 확장 config 를 볼 수 없는 경우 */
		child->bus_flags |= PCI_BUS_FLAGS_NO_EXTCFG;
		/* [한국어] 플래그를 세워 두면 pci_cfg_space_size() 가 이 아래
		 * 장치들의 cfg_size 를 256 으로 제한하고, 확장 capability 탐색이
		 * 시도조차 되지 않는다. */
		pci_info(child, "extended config space not accessible\n");
		/* [한국어] 왜 AER 등이 안 붙는지 진단할 수 있도록 로그를 남긴다 */
	}

	/* Set up default resource pointers and names */
	for (i = 0; i < PCI_BRIDGE_RESOURCE_NUM; i++) {
		/* [한국어] 자식 버스의 창 슬롯을 브리지의 창 리소스에 연결한다.
		 * pci_read_bridge_bases() 가 하는 것과 같은 포인터 공유이며,
		 * 여기서는 이름까지 붙여 준다. */
		child->resource[i] = &bridge->resource[PCI_BRIDGE_RESOURCES+i];
		/* [한국어] 브리지 pci_dev 의 창 리소스를 그대로 가리킨다 */
		child->resource[i]->name = child->name;
		/* [한국어] /proc/iomem 등에 버스 이름으로 표시되게 한다 */
	}
	bridge->subordinate = child;
	/* [한국어] 브리지가 자기 아래 버스를 가리키게 한다. 이 필드가 0 이 아닌
	 * 것이 곧 "이 장치는 브리지이고 아래 버스가 이미 만들어졌다"는 표시라,
	 * pci_scan_bridge_extend() 가 재스캔 여부를 판단할 때 확인한다. */

add_dev:
	/* [한국어] 브리지 유무와 무관한 공통 등록 절차 */
	pci_set_bus_msi_domain(child);
	/* [한국어] MSI 도메인 결정 — 위로 올라가며 찾거나 루트에서 상속.
	 * 이 버스에 붙을 NVMe 컨트롤러의 MSI-X 벡터가 이 도메인에서 나온다. */
	ret = device_register(&child->dev);
	/* [한국어] 드라이버 모델에 버스 device 등록. 이 시점부터 sysfs 에 보인다 */
	if (WARN_ON(ret < 0)) {
		/* [한국어] 등록 실패는 정상 동작에서 일어나지 않으므로 경고와 함께
		 * 스택 트레이스를 남긴다 */
		put_device(&child->dev);
		/* [한국어] device_register 는 실패해도 초기화는 끝난 상태라
		 * kfree 가 아니라 put_device 로 정리해야 한다(소멸자가 kfree 수행) */
		return NULL;
	}

	pcibios_add_bus(child);
	/* [한국어] 아키텍처별 버스 추가 훅 */

	if (child->ops->add_bus) {
		/* [한국어] 컨트롤러 드라이버가 버스 추가 콜백을 등록했으면 호출 */
		ret = child->ops->add_bus(child);
		if (WARN_ON(ret < 0))
			dev_err(&child->dev, "failed to add bus: %d\n", ret);
		/* [한국어] 실패해도 등록을 되돌리지는 않고 경고만 남긴다 */
	}

	/* Create legacy_io and legacy_mem files for this bus */
	pci_create_legacy_files(child);
	/* [한국어] 레거시 I/O/MEM sysfs 파일 생성 */

	return child;
	/* [한국어] 이제 이 버스에 대해 pci_scan_child_bus_extend() 를 돌릴 수 있다 */
}

/*
 * [한국어]
 * pci_add_new_bus - 자식 버스를 만들고 부모의 자식 목록에 매단다
 *
 * @parent: 상위 버스.
 * @dev:    이 버스를 만드는 브리지 장치(없으면 NULL).
 * @busnr:  배정할 secondary 버스 번호.
 * @return: 만들어진 자식 버스, 실패 시 NULL.
 *
 * 왜 필요한가: pci_alloc_child_bus() 는 객체를 만들어 등록만 하고 트리에
 * 연결하지는 않는다. 트리 연결은 전역적으로 보이는 변경이라 락이 필요한데,
 * 그 락 구간을 이 얇은 래퍼가 담당한다. 외부(핫플러그 드라이버 등)에서
 * 쓰는 공개 API 이기도 하다.
 *
 * 실행 컨텍스트: 프로세스 문맥. pci_bus_sem 을 쓰기 모드로 잠시 잡는다.
 *
 * 호출 체인:
 *   pci_scan_bridge_extend() → [pci_add_new_bus] → pci_alloc_child_bus()
 */
struct pci_bus *pci_add_new_bus(struct pci_bus *parent, struct pci_dev *dev,
				int busnr)
{
	struct pci_bus *child;
	/* [한국어] 만들어진 자식 버스 */

	child = pci_alloc_child_bus(parent, dev, busnr);
	/* [한국어] 할당 + 초기화 + device 등록까지 수행 */
	if (child) {
		/* [한국어] 성공했을 때만 트리에 연결한다 */
		down_write(&pci_bus_sem);
		/* [한국어] 버스 트리를 보호하는 세마포어를 쓰기 모드로 획득.
		 * 목록을 순회하는 다른 코드(sysfs, 전원 관리)와의 경쟁을 막는다. */
		list_add_tail(&child->node, &parent->children);
		/* [한국어] 부모의 자식 목록 끝에 매단다. 버스 번호 순서대로
		 * 스캔하므로 tail 에 붙이면 자연히 번호 순으로 정렬된다. */
		up_write(&pci_bus_sem);
		/* [한국어] 락 해제 */
	}
	return child;
	/* [한국어] 실패했으면 NULL 그대로 — 호출자가 브리지 아래 스캔을 포기한다 */
}
EXPORT_SYMBOL(pci_add_new_bus);
/* [한국어] 일부 핫플러그/컨트롤러 드라이버가 모듈에서 호출하므로 공개 */

/*
 * [한국어]
 * pci_enable_rrs_sv - Root Port 의 "RRS 소프트웨어 가시성"을 켠다
 *
 * @pdev: Root Port 장치.
 * @return: 없음. 성공하면 pdev->config_rrs_sv 가 1 이 된다.
 *
 * RRS(Configuration Request Retry Status)란: 전원이 막 들어온 장치는 아직
 * config 요청에 답할 준비가 되지 않을 수 있다. 그때 장치는 "지금은 안 되니
 * 나중에 다시 요청하라"는 RRS 응답을 보낸다. 기본 동작에서는 Root Port 가
 * 이 재시도를 하드웨어에서 알아서 반복하므로 소프트웨어는 그저 오래 걸린다고
 * 느낄 뿐이다.
 *
 * 소프트웨어 가시성을 켜면: Root Port 가 재시도를 감추는 대신, Vendor ID
 * 읽기에 대해 0x0001 이라는 특별한 값을 돌려준다. 그러면 커널이
 * "장치는 있는데 아직 준비가 안 됐다"를 직접 구별할 수 있어,
 * pci_bus_wait_rrs() 가 적절히 기다렸다가 재시도할 수 있다.
 *
 * 왜 중요한가: 이 값이 없으면 커널은 "응답 없음(0xFFFF)"과 "아직 준비 안 됨"을
 * 구별하지 못한다. 특히 부팅 직후나 핫플러그 직후의 NVMe SSD 처럼 초기화에
 * 시간이 걸리는 장치가 열거에서 누락되는 것을 막아 준다.
 *
 * 실행 컨텍스트: 프로세스 문맥. Root Port 를 발견한 직후 한 번 실행된다.
 *
 * 호출 체인:
 *   pci_setup_device()(Root Port 판정 후) → [pci_enable_rrs_sv]
 *     → pcie_capability_read_word()/set_word()
 */
static void pci_enable_rrs_sv(struct pci_dev *pdev)
{
	u16 root_cap = 0;
	/* [한국어] Root Capabilities 레지스터 값. 읽기 실패 시에도 0 이 남아
	 * 아래 검사가 자연히 거짓이 되도록 0 으로 초기화한다. */

	/* Enable Configuration RRS Software Visibility if supported */
	pcie_capability_read_word(pdev, PCI_EXP_RTCAP, &root_cap);
	/* [한국어] PCIe capability 의 Root Capabilities 레지스터.
	 * Root Port 에만 존재하는 레지스터다. */
	if (root_cap & PCI_EXP_RTCAP_RRS_SV) {
		/* [한국어] 이 Root Port 가 RRS 소프트웨어 가시성을 지원하는 경우 */
		pcie_capability_set_word(pdev, PCI_EXP_RTCTL,
					 PCI_EXP_RTCTL_RRS_SVE);
		/* [한국어] Root Control 레지스터의 활성화 비트를 켠다.
		 * set_word 는 읽기-수정-쓰기를 원자적으로 처리해 다른 비트를
		 * 건드리지 않는다. */
		pdev->config_rrs_sv = 1;
		/* [한국어] 커널 측에도 기록. pci_bus_generic_read_dev_vendor_id()
		 * 가 이 값을 보고 0x0001 을 "준비 중"으로 해석할지 결정한다. */
	}
}

static unsigned int pci_scan_child_bus_extend(struct pci_bus *bus,
					      unsigned int available_buses);
/* [한국어] 전방 선언. pci_scan_bridge_extend() 와 서로를 호출하는 상호
 * 재귀 구조라, 한쪽을 먼저 선언해 두어야 컴파일이 된다.
 * 이 상호 재귀가 곧 PCI 트리의 깊이 우선 하강이다:
 *   버스 스캔 → 브리지 발견 → 브리지 아래 버스 스캔 → … */

/*
 * [한국어]
 * pbus_validate_busn - 이 버스의 번호 구간이 상위 브리지들의 구간 안에 있는지 검증
 *
 * @bus: 검증할 버스.
 * @return: 없음. 문제가 있으면 로그로 경고만 하고 넘어간다.
 *
 * 왜 필요한가: config 트랜잭션은 브리지들을 거쳐 내려간다. 각 브리지는
 * 자기 secondary~subordinate 구간에 속하는 버스 번호만 아래로 전달한다.
 * 따라서 어떤 버스의 번호 구간이 조상 브리지 중 하나의 구간을 벗어나면,
 * 그 버스의 장치에는 config 요청이 도달하지 못해 완전히 접근 불가가 된다.
 * 이 함수는 뿌리까지 올라가며 그 포함 관계를 확인한다.
 *
 * 왜 경고만 하는가: 이 상황은 버스 번호가 모자라거나 펌웨어 설정이 이상할 때
 * 생긴다. 커널이 여기서 할 수 있는 복구가 없으므로, 왜 장치가 안 보이는지
 * 진단할 단서를 남기는 것이 목적이다.
 *
 * 실행 컨텍스트: 프로세스 문맥의 스캔 경로.
 *
 * 호출 체인:
 *   pci_scan_bridge_extend() → [pbus_validate_busn]
 */
void pbus_validate_busn(struct pci_bus *bus)
{
	struct pci_bus *upstream = bus->parent;
	/* [한국어] 한 단계 위 버스부터 검사를 시작한다 */
	struct pci_dev *bridge = bus->self;
	/* [한국어] 경고 로그를 어느 브리지 문맥으로 남길지 정하는 데 쓴다 */

	/* Check that all devices are accessible */
	while (upstream->parent) {
		/* [한국어] 루트 버스(parent 가 NULL)에 닿을 때까지 거슬러 올라간다.
		 * 루트 버스 자신은 위에 브리지가 없어 검사 대상이 아니다. */
		if ((bus->busn_res.end > upstream->busn_res.end) ||
		    (bus->number > upstream->busn_res.end) ||
		    (bus->number < upstream->number) ||
		    (bus->busn_res.end < upstream->number)) {
			/* [한국어] 네 조건은 "[bus.number, bus.busn_res.end] 구간이
			 * [upstream.number, upstream.busn_res.end] 안에 완전히 들어가는가"를
			 * 부정한 것이다. 순서대로:
			 *  - 내 subordinate 가 상위 subordinate 를 넘음
			 *  - 내 secondary 가 상위 subordinate 를 넘음
			 *  - 내 secondary 가 상위 secondary 보다 작음
			 *  - 내 subordinate 가 상위 secondary 보다 작음
			 * 하나라도 참이면 포함 관계가 깨진 것이다. */
			pci_info(bridge, "devices behind bridge are unusable because %pR cannot be assigned for them\n",
				 &bus->busn_res);
			/* [한국어] 이 브리지 아래 장치(NVMe SSD 포함)에 config 요청이
			 * 도달하지 못한다는 경고. 어떤 구간이 문제인지 함께 찍는다. */
			break;
			/* [한국어] 한 번 어긋나면 더 위를 봐도 의미가 없다 */
		}
		upstream = upstream->parent;
		/* [한국어] 다음 조상으로 이동 */
	}
}

/**
 * pci_ea_fixed_busnrs() - Read fixed Secondary and Subordinate bus
 * numbers from EA capability.
 * @dev: Bridge
 * @sec: updated with secondary bus number from EA
 * @sub: updated with subordinate bus number from EA
 *
 * If @dev is a bridge with EA capability that specifies valid secondary
 * and subordinate bus numbers, return true with the bus numbers in @sec
 * and @sub.  Otherwise return false.
 */
/*
 * [한국어]
 * pci_ea_fixed_busnrs - EA capability 가 지정한 고정 버스 번호를 읽는다
 *
 * @dev: 검사할 브리지.
 * @sec: 성공 시 EA 가 지정한 secondary 버스 번호가 기록된다.
 * @sub: 성공 시 EA 가 지정한 subordinate 버스 번호가 기록된다.
 * @return: 유효한 고정 번호를 찾으면 true, 아니면 false(그 경우 @sec/@sub 는
 *          건드리지 않는다).
 *
 * EA(Enhanced Allocation)란: 보통 BAR 와 버스 번호는 소프트웨어가 자유롭게
 * 배정하지만, 일부 하드웨어(특히 SoC 내장 장치)는 주소와 버스 번호가 물리적으로
 * 고정되어 있어 바꿀 수 없다. EA capability(ID 0x14)는 그런 고정값을
 * 소프트웨어에 알려 주는 수단이다.
 *
 * 왜 필요한가: 브리지의 secondary/subordinate 를 커널이 임의로 배정해 버리면
 * 고정 하드웨어와 어긋나 그 아래가 전부 접근 불가가 된다. 그래서
 * pci_scan_bridge_extend() 는 번호를 배정하기 전에 이 함수로 고정값이 있는지
 * 먼저 확인한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, config 읽기만 수행.
 *
 * 호출 체인:
 *   pci_scan_bridge_extend() → [pci_ea_fixed_busnrs] → pci_find_capability()
 */
bool pci_ea_fixed_busnrs(struct pci_dev *dev, u8 *sec, u8 *sub)
{
	int ea, offset;
	/* [한국어] ea = EA capability 시작 오프셋, offset = 첫 엔트리 위치 */
	u32 dw;
	/* [한국어] 버스 번호 필드가 들어 있는 4바이트 */
	u8 ea_sec, ea_sub;
	/* [한국어] 추출한 secondary/subordinate 번호. 유효성 검사를 통과한
	 * 뒤에만 호출자 버퍼에 기록한다. */

	if (dev->hdr_type != PCI_HEADER_TYPE_BRIDGE)
		return false;
	/* [한국어] header type 1(브리지)이 아니면 secondary/subordinate 개념 자체가
	 * 없다. 일반 장치(type 0)나 CardBus(type 2)는 대상이 아니다. */

	/* find PCI EA capability in list */
	ea = pci_find_capability(dev, PCI_CAP_ID_EA);
	/* [한국어] capability 링크를 따라가 EA(ID 0x14)를 찾는다 */
	if (!ea)
		return false;
	/* [한국어] EA 가 없으면 고정 번호도 없다 — 커널이 자유롭게 배정하면 된다 */

	offset = ea + PCI_EA_FIRST_ENT;
	/* [한국어] EA capability 헤더 다음의 첫 엔트리 위치.
	 * 브리지용 EA 는 첫 엔트리의 dword 에 버스 번호를 담는다. */
	pci_read_config_dword(dev, offset, &dw);
	/* [한국어] 그 dword 를 읽는다 */
	ea_sec = FIELD_GET(PCI_EA_SEC_BUS_MASK, dw);
	/* [한국어] 마스크로 secondary 버스 번호 필드만 추출 */
	ea_sub = FIELD_GET(PCI_EA_SUB_BUS_MASK, dw);
	/* [한국어] subordinate 버스 번호 필드 추출 */
	if (ea_sec  == 0 || ea_sub < ea_sec)
		return false;
	/* [한국어] 정합성 검사. secondary 가 0 이면(= 루트 버스 번호) 브리지 아래
	 * 버스로는 있을 수 없는 값이고, subordinate < secondary 는 빈 구간이라
	 * 역시 무효다. 둘 중 하나라도 걸리면 EA 값을 믿지 않는다. */

	*sec = ea_sec;
	/* [한국어] 검증을 통과했으므로 호출자에게 전달 */
	*sub = ea_sub;
	/* [한국어] 마찬가지 */
	return true;
	/* [한국어] 호출자는 이 값을 그대로 브리지에 프로그래밍한다 */
}

/*
 * pci_scan_bridge_extend() - Scan buses behind a bridge
 * @bus: Parent bus the bridge is on
 * @dev: Bridge itself
 * @max: Starting subordinate number of buses behind this bridge
 * @available_buses: Total number of buses available for this bridge and
 *		     the devices below. After the minimal bus space has
 *		     been allocated the remaining buses will be
 *		     distributed equally between hotplug-capable bridges.
 * @pass: Either %0 (scan already configured bridges) or %1 (scan bridges
 *        that need to be reconfigured.
 *
 * If it's a bridge, configure it and scan the bus behind it.
 * For CardBus bridges, we don't scan behind as the devices will
 * be handled by the bridge driver itself.
 *
 * We need to process bridges in two passes -- first we scan those
 * already configured by the BIOS and after we are done with all of
 * them, we proceed to assigning numbers to the remaining buses in
 * order to avoid overlaps between old and new bus numbers.
 *
 * Return: New subordinate number covering all buses behind this bridge.
 */
/*
 * [한국어]
 * pci_scan_bridge_extend - 브리지에 버스 번호를 배정하고 그 아래를 재귀 스캔한다
 *
 * @bus:             이 브리지가 붙어 있는 부모 버스.
 * @dev:             브리지 장치 자신.
 * @max:             지금까지 사용된 가장 큰 버스 번호.
 * @available_buses: 이 브리지와 그 아래에 쓸 수 있는 여분 버스 개수.
 *                   핫플러그로 나중에 장치가 더 꽂힐 것에 대비해 미리 남겨 두는
 *                   예비분이다. 0 이면 최소한만 쓴다.
 * @pass:            0 = 펌웨어가 이미 설정해 둔 브리지만 처리, 1 = 번호를 새로
 *                   배정해야 하는 브리지 처리.
 * @return: 이 브리지 아래 전체를 포함하는 새로운 최대 버스 번호(subordinate).
 *
 * 왜 두 번 도는가(2-pass): 버스 번호는 시스템 전체에서 유일해야 한다. 펌웨어가
 * 이미 번호를 넣어 둔 브리지와 커널이 새로 번호를 매길 브리지가 섞여 있을 때,
 * 순서 없이 처리하면 새로 매긴 번호가 기존 번호와 겹칠 수 있다. 그래서
 * pass 0 에서 기존 설정을 모두 파악해 max 를 확정한 뒤, pass 1 에서 그 위쪽
 * 번호부터 새로 배정한다. 위 영어 주석이 설명하는 그대로다.
 *
 * 재귀 구조: 이 함수는 자식 버스를 만든 뒤 pci_scan_child_bus_extend() 를
 * 부르고, 그 함수는 다시 브리지를 만날 때마다 이 함수를 부른다. 이 상호
 * 재귀가 PCI 트리 전체의 깊이 우선 순회를 이룬다.
 *
 * subordinate 를 두 번 쓰는 이유: 자식 버스를 스캔하기 전에는 아래에 버스가
 * 몇 개나 있을지 모른다. 그래서 일단 0xff(최대)로 열어 두어 config 요청이
 * 아래로 전달되게 하고, 스캔이 끝난 뒤 실제 최대 번호로 좁혀 쓴다.
 *
 * NVMe 접점: NVMe SSD 가 Root Port 나 PCIe 스위치 아래에 있으면, 그 포트에
 * 대해 이 함수가 실행되어야 SSD 가 붙은 버스가 존재하게 되고 비로소
 * pci_scan_slot() 이 그 SSD 를 발견할 수 있다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 시작과 끝에서 runtime PM 참조를 잡고 놓아
 * 스캔 동안 브리지가 절전 상태로 들어가지 않게 한다.
 *
 * 호출 체인:
 *   pci_scan_child_bus_extend() → [pci_scan_bridge_extend]
 *     → pci_add_new_bus() → pci_scan_child_bus_extend() → …(재귀)
 */
static int pci_scan_bridge_extend(struct pci_bus *bus, struct pci_dev *dev,
				  int max, unsigned int available_buses,
				  int pass)
{
	struct pci_bus *child;
	/* [한국어] 이 브리지 아래의 자식 버스 */
	u32 buses;
	/* [한국어] offset 0x18 의 4바이트 — primary/secondary/subordinate/
	 * secondary latency timer 를 한꺼번에 담는다. */
	u16 bctl;
	/* [한국어] Bridge Control 레지스터(0x3e) 원본. 스캔 중 잠시 바꿨다가
	 * out 레이블에서 반드시 복원한다. */
	u8 primary, secondary, subordinate;
	/* [한국어] 브리지에서 읽어 낸 현재 버스 번호 3종 */
	int broken = 0;
	/* [한국어] 펌웨어가 넣어 둔 설정이 앞뒤가 맞지 않아 무시해야 하는지 표시 */
	bool fixed_buses;
	/* [한국어] EA capability 가 버스 번호를 고정해 두었는지 */
	u8 fixed_sec, fixed_sub;
	/* [한국어] EA 가 지정한 고정 secondary/subordinate 번호 */
	int next_busnr;
	/* [한국어] 이 브리지 아래 버스에 새로 배정할 번호 */

	/*
	 * Make sure the bridge is powered on to be able to access config
	 * space of devices below it.
	 */
	pm_runtime_get_sync(&dev->dev);
	/* [한국어] 브리지를 D0(동작 상태)로 올리고 그 상태를 유지시킨다.
	 * 절전 상태의 브리지는 아래로 config 요청을 전달하지 않으므로,
	 * 이것을 빼먹으면 아래 장치가 통째로 안 보인다. _sync 는 전원이
	 * 실제로 올라올 때까지 기다린다는 뜻이다. */

	pci_read_config_dword(dev, PCI_PRIMARY_BUS, &buses);
	/* [한국어] offset 0x18 — 버스 번호 3종을 한 번에 읽는다 */
	primary = FIELD_GET(PCI_PRIMARY_BUS_MASK, buses);
	/* [한국어] 마스크 0x000000ff — 브리지의 윗쪽 버스 번호 */
	secondary = FIELD_GET(PCI_SECONDARY_BUS_MASK, buses);
	/* [한국어] 마스크 0x0000ff00 — 브리지 바로 아래 버스 번호 */
	subordinate = FIELD_GET(PCI_SUBORDINATE_BUS_MASK, buses);
	/* [한국어] 마스크 0x00ff0000 — 이 브리지 아래에 존재하는 최대 버스 번호 */

	pci_dbg(dev, "scanning [bus %02x-%02x] behind bridge, pass %d\n",
		secondary, subordinate, pass);
	/* [한국어] 현재 브리지가 주장하는 구간과 몇 번째 패스인지 디버그 로그 */

	if (!primary && (primary != bus->number) && secondary && subordinate) {
		pci_warn(dev, "Primary bus is hard wired to 0\n");
		primary = bus->number;
		/* [한국어] primary 가 0 으로 하드와이어된 브리지 quirk.
		 * 실제 부모 버스 번호가 0 이 아닌데도 브리지는 0 만 보고한다.
		 * secondary/subordinate 는 정상값이 들어 있으므로, primary 만
		 * 실제 부모 버스 번호로 바로잡아 아래 정합성 검사를 통과시킨다. */
	}

	/* Check if setup is sensible at all */
	if (!pass &&
	    (primary != bus->number || secondary <= bus->number ||
	     secondary > subordinate)) {
		/* [한국어] pass 0(기존 설정 존중)에서만 검사한다. 세 조건은
		 * 펌웨어 설정이 물리적으로 말이 되는지 보는 것이다:
		 *  - primary 가 실제 부모 버스 번호와 다르다
		 *  - secondary 가 부모 번호보다 크지 않다(아래 버스는 반드시 큰 번호)
		 *  - secondary 가 subordinate 보다 크다(빈 구간)
		 * 하나라도 걸리면 그 설정을 믿을 수 없다. */
		pci_info(dev, "bridge configuration invalid ([bus %02x-%02x]), reconfiguring\n",
			 secondary, subordinate);
		broken = 1;
		/* [한국어] 이 표시가 서면 아래에서 "펌웨어 설정 무시, 새로 배정"
		 * 경로(else 분기)로 간다. */
	}

	/*
	 * Disable Master-Abort Mode during probing to avoid reporting of
	 * bus errors in some architectures.
	 */
	pci_read_config_word(dev, PCI_BRIDGE_CONTROL, &bctl);
	/* [한국어] Bridge Control(0x3e) 원본 백업 — out 에서 복원한다 */
	pci_write_config_word(dev, PCI_BRIDGE_CONTROL,
			      bctl & ~PCI_BRIDGE_CTL_MASTER_ABORT);
	/* [한국어] Master Abort 보고 비트(0x20)를 끈다. 스캔은 본질적으로
	 * "없는 장치에 말을 걸어 보는" 일이라 master abort 가 대량으로 발생하는데,
	 * 그것을 오류로 보고하면 아키텍처에 따라 머신 체크나 NMI 로 이어질 수 있다.
	 * 스캔이 끝나면 원래대로 되돌린다. */

	if (pci_is_cardbus_bridge(dev)) {
		/* [한국어] header type 2(CardBus)는 버스 번호 필드 위치와 창 구조가
		 * 완전히 달라 전용 함수가 처리한다. 위 영어 주석대로 CardBus
		 * 아래는 여기서 스캔하지 않고 CardBus 드라이버가 담당한다. */
		max = pci_cardbus_scan_bridge_extend(bus, dev, buses, max,
						     available_buses,
						     pass);
		goto out;
		/* [한국어] 공통 마무리(레지스터 복원, PM 반납)로 */
	}

	if ((secondary || subordinate) &&
	    !pcibios_assign_all_busses() && !broken) {
		/* [한국어] 경로 A: 펌웨어 설정을 그대로 존중한다.
		 * 세 조건이 모두 필요하다:
		 *  - 버스 번호가 실제로 설정되어 있고(둘 중 하나라도 0 이 아님)
		 *  - 아키텍처가 "모든 버스 번호를 커널이 다시 매긴다"고 하지 않았고
		 *  - 위 정합성 검사를 통과했다. */
		unsigned int cmax, buses;
		/* [한국어] cmax = 자식 스캔이 돌려준 실제 최대 버스 번호,
		 * buses = 이 브리지에 허용된 버스 개수(바깥 buses 를 가린다). */

		/*
		 * Bus already configured by firmware, process it in the
		 * first pass and just note the configuration.
		 */
		if (pass)
			goto out;
		/* [한국어] 이미 설정된 브리지는 pass 0 에서만 다룬다.
		 * pass 1 에 또 들어오면 중복 처리이므로 그냥 나간다. */

		/*
		 * The bus might already exist for two reasons: Either we
		 * are rescanning the bus or the bus is reachable through
		 * more than one bridge. The second case can happen with
		 * the i450NX chipset.
		 */
		child = pci_find_bus(pci_domain_nr(bus), secondary);
		/* [한국어] 그 번호의 버스가 이미 있는지 확인. 위 영어 주석이 든
		 * 두 가지 이유(재스캔, i450NX 처럼 한 버스가 두 브리지로 도달)가
		 * 있어 중복 생성을 피해야 한다. */
		if (!child) {
			/* [한국어] 없으면 새로 만든다 */
			child = pci_add_new_bus(bus, dev, secondary);
			if (!child)
				goto out;
			/* [한국어] 생성 실패(메모리 부족) — 아래를 포기한다 */
			child->primary = primary;
			/* [한국어] 위에서 보정한 primary 를 기록 */
			pci_bus_insert_busn_res(child, secondary, subordinate);
			/* [한국어] 펌웨어가 정한 구간을 그대로 버스 번호 리소스로 등록 */
			child->bridge_ctl = bctl;
			/* [한국어] 원본 Bridge Control 을 버스에 기억시킨다.
			 * 나중에 secondary bus reset 등에서 기준값으로 쓴다. */
		}

		buses = subordinate - secondary;
		/* [한국어] 펌웨어가 이 브리지 아래에 허용한 여분 버스 개수 */
		cmax = pci_scan_child_bus_extend(child, buses);
		/* [한국어] ★재귀 하강★ — 자식 버스를 훑는다. 그 안에서 또 브리지를
		 * 만나면 이 함수가 다시 불린다. 반환값은 실제로 쓰인 최대 버스 번호. */
		if (cmax > subordinate)
			pci_warn(dev, "bridge has subordinate %02x but max busn %02x\n",
				 subordinate, cmax);
		/* [한국어] 실제로 필요한 번호가 펌웨어가 허용한 구간을 넘었다.
		 * 그러면 넘어간 버스의 장치들은 접근 불가다(브리지가 전달하지 않는다).
		 * 펌웨어 설정을 존중하는 경로라 고치지 않고 경고만 남긴다. */

		/* Subordinate should equal child->busn_res.end */
		if (subordinate > max)
			max = subordinate;
		/* [한국어] 전체 최대 버스 번호를 갱신. 펌웨어가 정한 subordinate 을
		 * 그대로 인정하므로 cmax 가 아니라 subordinate 을 쓴다. */
	} else {
		/* [한국어] 경로 B: 커널이 버스 번호를 새로 배정한다.
		 * (설정이 없거나, 아키텍처가 전부 재배정하라고 했거나, broken 이거나) */

		/*
		 * We need to assign a number to this bus which we always
		 * do in the second pass.
		 */
		if (!pass) {
			/* [한국어] pass 0 에서는 배정하지 않는다. 기존 설정을 모두
			 * 파악한 뒤(= max 확정 후) pass 1 에서 그 위 번호를 쓴다. */
			if (pcibios_assign_all_busses() || broken)

				/*
				 * Temporarily disable forwarding of the
				 * configuration cycles on all bridges in
				 * this bus segment to avoid possible
				 * conflicts in the second pass between two
				 * bridges programmed with overlapping bus
				 * ranges.
				 */
				pci_write_config_dword(dev, PCI_PRIMARY_BUS,
						       buses & PCI_SEC_LATENCY_TIMER_MASK);
			/* [한국어] 버스 번호 3종을 모두 0 으로 만들고 latency timer
			 * 필드(0xff000000)만 보존한다. 번호가 0 이면 브리지가 어떤
			 * config 요청도 아래로 전달하지 않는다. 위 영어 주석대로,
			 * pass 1 에서 새 번호를 배정하는 동안 기존의 겹치는 설정이
			 * 남아 있어 충돌하는 것을 막기 위한 조치다. */
			goto out;
		}

		/* Clear errors */
		pci_write_config_word(dev, PCI_STATUS, 0xffff);
		/* [한국어] Status 레지스터(0x06)의 오류 비트를 지운다.
		 * PCI 의 오류 상태 비트는 W1C(1 을 쓰면 지워짐)라, 0xffff 를 쓰면
		 * 켜져 있던 모든 오류 비트가 정리된다. 지금까지의 탐색 과정에서
		 * 쌓인 master abort 흔적을 없애는 것이다. */

		/* Read bus numbers from EA Capability (if present) */
		fixed_buses = pci_ea_fixed_busnrs(dev, &fixed_sec, &fixed_sub);
		/* [한국어] 하드웨어가 버스 번호를 고정해 두었는지 먼저 확인 */
		if (fixed_buses)
			next_busnr = fixed_sec;
		/* [한국어] 고정값이 있으면 반드시 그것을 써야 한다 */
		else
			next_busnr = max + 1;
		/* [한국어] 없으면 지금까지 쓰인 최대 번호 다음 번호를 쓴다 */

		/*
		 * Prevent assigning a bus number that already exists.
		 * This can happen when a bridge is hot-plugged, so in this
		 * case we only re-scan this bus.
		 */
		child = pci_find_bus(pci_domain_nr(bus), next_busnr);
		/* [한국어] 그 번호가 이미 쓰이고 있는지 확인.
		 * 위 영어 주석대로 핫플러그 상황에서 생길 수 있다. */
		if (!child) {
			child = pci_add_new_bus(bus, dev, next_busnr);
			if (!child)
				goto out;
			/* [한국어] 생성 실패 시 아래 스캔 포기 */
			pci_bus_insert_busn_res(child, next_busnr,
						bus->busn_res.end);
			/* [한국어] 끝을 부모의 subordinate 까지 최대로 열어 둔다.
			 * 아래에 버스가 몇 개나 있을지 아직 모르기 때문이다.
			 * 스캔 후 pci_bus_update_busn_res_end() 로 좁힌다. */
		}
		max++;
		/* [한국어] 방금 번호 하나를 소비했다 */
		if (available_buses)
			available_buses--;
		/* [한국어] 예비분에서도 하나 차감. 0 이면 더 뺄 것이 없다 */

		buses = (buses & PCI_SEC_LATENCY_TIMER_MASK) |
			FIELD_PREP(PCI_PRIMARY_BUS_MASK, child->primary) |
			FIELD_PREP(PCI_SECONDARY_BUS_MASK, child->busn_res.start) |
			FIELD_PREP(PCI_SUBORDINATE_BUS_MASK, child->busn_res.end);
		/* [한국어] 새 버스 번호 3종을 하나의 dword 로 조립한다.
		 * latency timer 필드는 원본에서 보존하고, 나머지 세 바이트만
		 * FIELD_PREP 으로 각자 자리에 채운다.
		 * subordinate 에는 아직 넉넉한 값(부모의 끝)이 들어간다 — 아래를
		 * 훑는 동안 config 요청이 막히지 않게 하기 위함이다. */

		/* We need to blast all three values with a single write */
		pci_write_config_dword(dev, PCI_PRIMARY_BUS, buses);
		/* [한국어] 세 값을 한 번의 dword 쓰기로 동시에 반영한다.
		 * 바이트 단위로 나눠 쓰면 중간 상태에서 브리지가 모순된 구간을
		 * 갖게 되어(예: secondary > subordinate) 그 순간의 config 요청이
		 * 엉뚱하게 처리될 수 있다. 위 영어 주석의 "blast ... with a single
		 * write" 가 그 뜻이다. */

		child->bridge_ctl = bctl;
		/* [한국어] 원본 Bridge Control 을 버스에 기억시킨다 */
		max = pci_scan_child_bus_extend(child, available_buses);
		/* [한국어] ★재귀 하강★ — 새로 만든 버스를 훑는다. 남은 예비 버스
		 * 개수를 함께 넘겨 아래 핫플러그 브리지들이 나눠 갖게 한다.
		 * 반환값이 이 아래 전체를 포함하는 실제 최대 버스 번호다. */

		/*
		 * Set subordinate bus number to its real value.
		 * If fixed subordinate bus number exists from EA
		 * capability then use it.
		 */
		if (fixed_buses)
			max = fixed_sub;
		/* [한국어] EA 고정값이 있으면 실제 스캔 결과보다 그것이 우선이다.
		 * 하드웨어가 그렇게 배선되어 있기 때문이다. */
		pci_bus_update_busn_res_end(child, max);
		/* [한국어] 커널 쪽 버스 번호 리소스의 끝을 실제 값으로 좁힌다.
		 * 위에서 최대로 열어 두었던 것을 여기서 확정한다. */
		pci_write_config_byte(dev, PCI_SUBORDINATE_BUS, max);
		/* [한국어] 하드웨어 쪽 subordinate(0x1a)도 실제 값으로 좁힌다.
		 * 이제 이 브리지는 딱 필요한 구간의 요청만 아래로 전달한다. */
	}
	scnprintf(child->name, sizeof(child->name), "PCI Bus %04x:%02x",
		  pci_domain_nr(bus), child->number);
	/* [한국어] 버스 이름을 "PCI Bus 0000:01" 형태로 만든다.
	 * 이 이름은 앞서 pci_alloc_child_bus() 에서 창 리소스의 이름으로도
	 * 연결해 두었으므로 /proc/iomem 에 그대로 나타난다. */

	pbus_validate_busn(child);
	/* [한국어] 배정 결과가 조상 브리지들의 구간 안에 들어가는지 확인.
	 * 어긋나면 경고 로그를 남긴다. */

out:
	/* Clear errors in the Secondary Status Register */
	pci_write_config_word(dev, PCI_SEC_STATUS, 0xffff);
	/* [한국어] Secondary Status(0x1e)의 오류 비트를 W1C 로 모두 지운다.
	 * 아래쪽 버스를 훑는 동안 없는 장치에 말을 걸어 쌓인 흔적을 치운다. */

	pci_write_config_word(dev, PCI_BRIDGE_CONTROL, bctl);
	/* [한국어] 앞에서 Master Abort 보고를 끄려고 바꿨던 Bridge Control 을
	 * 원본으로 되돌린다. 모든 반환 경로가 이 레이블을 지나므로 복원이 보장된다. */

	pm_runtime_put(&dev->dev);
	/* [한국어] 스캔 시작에서 잡은 runtime PM 참조를 놓는다. 이제 브리지는
	 * 유휴 상태가 되면 절전으로 내려갈 수 있다. */

	return max;
	/* [한국어] 이 브리지 아래 전체를 포함하는 최대 버스 번호.
	 * 호출자 pci_scan_child_bus_extend() 가 다음 브리지 처리에 이어 쓴다. */
}

/*
 * pci_scan_bridge() - Scan buses behind a bridge
 * @bus: Parent bus the bridge is on
 * @dev: Bridge itself
 * @max: Starting subordinate number of buses behind this bridge
 * @pass: Either %0 (scan already configured bridges) or %1 (scan bridges
 *        that need to be reconfigured.
 *
 * If it's a bridge, configure it and scan the bus behind it.
 * For CardBus bridges, we don't scan behind as the devices will
 * be handled by the bridge driver itself.
 *
 * We need to process bridges in two passes -- first we scan those
 * already configured by the BIOS and after we are done with all of
 * them, we proceed to assigning numbers to the remaining buses in
 * order to avoid overlaps between old and new bus numbers.
 *
 * Return: New subordinate number covering all buses behind this bridge.
 */
/*
 * [한국어]
 * pci_scan_bridge - 브리지 아래를 스캔하는 공개 API (예비 버스 없이)
 *
 * @bus:  브리지가 붙어 있는 부모 버스.
 * @dev:  브리지 장치.
 * @max:  지금까지 쓰인 최대 버스 번호.
 * @pass: 0 = 이미 설정된 브리지 처리, 1 = 번호를 새로 배정할 브리지 처리.
 * @return: 이 브리지 아래를 포함하는 새 최대 버스 번호.
 *
 * 왜 필요한가: pci_scan_bridge_extend() 의 available_buses 인자를 0 으로
 * 고정한 래퍼다. 핫플러그용 예비 버스 번호를 남기지 않는, 가장 단순한 형태의
 * 스캔이며 외부 모듈에 공개된 인터페이스이기도 하다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   외부 호출자 → [pci_scan_bridge] → pci_scan_bridge_extend()
 */
int pci_scan_bridge(struct pci_bus *bus, struct pci_dev *dev, int max, int pass)
{
	return pci_scan_bridge_extend(bus, dev, max, 0, pass);
	/* [한국어] available_buses = 0 — 예비분을 남기지 않는다 */
}
EXPORT_SYMBOL(pci_scan_bridge);
/* [한국어] 모듈에서 호출할 수 있도록 공개 */

/*
 * Read interrupt line and base address registers.
 * The architecture-dependent code can tweak these, of course.
 */
/*
 * [한국어]
 * pci_read_irq - 레거시 INTx 인터럽트 정보를 config space 에서 읽는다
 *
 * @dev: 대상 장치.
 * @return: 없음. dev->pin 과 dev->irq 가 채워진다.
 *
 * 왜 필요한가: MSI/MSI-X 이전의 PCI 는 INTA#~INTD# 라는 네 개의 물리 신호선을
 * 공유해 인터럽트를 전달했다. 그 정보가 config space 의 두 바이트에 있다:
 *   0x3d PCI_INTERRUPT_PIN  : 이 함수(function)가 어느 핀을 쓰는가.
 *                             0 = 인터럽트 안 씀, 1 = INTA#, 2 = INTB#, 3 = INTC#, 4 = INTD#.
 *                             하드웨어가 배선한 값이라 읽기 전용이다.
 *   0x3c PCI_INTERRUPT_LINE : 그 핀이 어느 인터럽트 컨트롤러 입력에 연결되는가.
 *                             하드웨어가 아니라 펌웨어가 써 넣는 "메모"에 가깝다.
 *
 * NVMe 접점: NVMe 컨트롤러는 실제로는 MSI-X 를 쓰지만(그 벡터는
 * pci_alloc_irq_vectors_affinity() 가 따로 할당한다), 스펙상 INTx 도 지원해야
 * 하므로 여기서 읽은 값이 dev->irq 의 초기값이 된다. MSI-X 를 활성화하면
 * 드라이버는 pci_irq_vector() 로 얻은 벡터를 쓰고 이 값은 쓰이지 않는다.
 *
 * 실행 컨텍스트: 프로세스 문맥, pci_setup_device() 안에서 호출.
 *
 * 호출 체인:
 *   pci_setup_device() → [pci_read_irq] → pci_read_config_byte()
 */
static void pci_read_irq(struct pci_dev *dev)
{
	unsigned char irq;
	/* [한국어] 핀 번호와 라인 번호를 차례로 담는 임시 변수(재사용) */

	/* VFs are not allowed to use INTx, so skip the config reads */
	if (dev->is_virtfn) {
		/* [한국어] SR-IOV 가상 함수는 스펙상 INTx 를 쓸 수 없다(MSI/MSI-X 전용).
		 * config 를 읽어 봐야 의미 없는 값이므로 0 으로 확정한다. */
		dev->pin = 0;
		/* [한국어] 인터럽트 핀 없음 */
		dev->irq = 0;
		/* [한국어] 레거시 IRQ 번호 없음 */
		return;
	}

	pci_read_config_byte(dev, PCI_INTERRUPT_PIN, &irq);
	/* [한국어] offset 0x3d — 이 함수가 쓰는 INTx 핀(0~4). 읽기 전용 */
	dev->pin = irq;
	/* [한국어] pci_dev 에 기록. 0 이면 INTx 를 쓰지 않는 장치다 */
	if (irq)
		pci_read_config_byte(dev, PCI_INTERRUPT_LINE, &irq);
	/* [한국어] 핀을 쓰는 장치일 때만 라인 번호(0x3c)를 읽는다.
	 * 핀이 0 이면 라인 값에 의미가 없어 읽지 않고, 위에서 읽은 0 이
	 * 그대로 아래 대입에 쓰인다. */
	dev->irq = irq;
	/* [한국어] 라인 번호(또는 핀이 0 일 때는 0)를 초기 IRQ 로 기록한다.
	 * 아키텍처 코드가 나중에 이 값을 실제 리눅스 IRQ 번호로 바꿔 놓는다. */
}

/*
 * [한국어]
 * set_pcie_port_type - PCIe capability 를 찾아 장치의 포트 종류와 성질을 확정한다
 *
 * @pdev: 대상 장치.
 * @return: 없음. pdev->pcie_cap, pcie_flags_reg, devcap, pcie_mpss,
 *          link_active_reporting, aspm_*_support 가 채워진다.
 *
 * 왜 필요한가: 이후 PCI 코드 전체가 "이 장치가 PCIe 인가, 그렇다면 어떤
 * 종류의 포트인가"를 끊임없이 묻는다(pci_is_pcie(), pci_pcie_type()).
 * 그 판단의 근거가 되는 pdev->pcie_cap(PCIe capability 의 config 오프셋)과
 * pcie_flags_reg 를 여기서 한 번 읽어 캐시해 둔다. pcie_cap 이 0 이 아니면
 * PCIe 장치라는 뜻이다.
 *
 * 포트 종류(PCI_EXP_TYPE_*): Endpoint, Root Port, Switch Upstream/Downstream,
 * PCIe-to-PCI 브리지, RC Event Collector 등. NVMe SSD 는 Endpoint 다.
 *
 * capability 를 찾는 방식: pci_find_capability(pdev, PCI_CAP_ID_EXP) 는
 * Status 레지스터의 CAP_LIST 비트를 확인한 뒤 Capabilities Pointer(0x34)에서
 * 시작해 각 항목의 [ID, Next] 를 따라가며 ID 0x10(PCI_CAP_ID_EXP)을 찾는다.
 * 무한 루프는 TTL 48회로 끊는다(drivers/pci/pci.h 의 PCI_FIND_NEXT_CAP).
 *
 * 마지막의 타입 보정: 일부 하드웨어가 자신을 상향/하향 포트로 잘못 보고한다.
 * 부모와의 관계로 그 모순을 잡아낸다.
 *
 * 실행 컨텍스트: 프로세스 문맥, pci_setup_device() 안에서 호출.
 *
 * 호출 체인:
 *   pci_setup_device() → [set_pcie_port_type]
 *     → pci_find_capability(), pci_enable_rrs_sv(), pci_upstream_bridge()
 */
void set_pcie_port_type(struct pci_dev *pdev)
{
	int pos;
	/* [한국어] PCIe capability 의 config space 오프셋. 0 이면 PCIe 가 아니다 */
	u16 reg16;
	/* [한국어] PCIe Capabilities 레지스터(포트 타입과 버전이 들어 있다) */
	u32 reg32;
	/* [한국어] Link Capabilities 레지스터 값 */
	int type;
	/* [한국어] 이 장치가 보고한 포트 종류 */
	struct pci_dev *parent;
	/* [한국어] 상위 브리지 — 타입 모순 검사에 쓴다 */

	pos = pci_find_capability(pdev, PCI_CAP_ID_EXP);
	/* [한국어] PCIe capability(ID 0x10)를 capability 링크에서 찾는다 */
	if (!pos)
		return;
	/* [한국어] 없으면 구식 PCI 장치다. pcie_cap 이 0 으로 남아
	 * pci_is_pcie() 가 false 를 돌려주게 된다. */

	pdev->pcie_cap = pos;
	/* [한국어] 오프셋을 캐시. 이후 pcie_capability_read_*() 가 이 값을
	 * 기준으로 레지스터 위치를 계산하므로, 매번 링크를 다시 걷지 않아도 된다. */
	pci_read_config_word(pdev, pos + PCI_EXP_FLAGS, &reg16);
	/* [한국어] capability 시작 + PCI_EXP_FLAGS = PCIe Capabilities 레지스터.
	 * 하위 4비트가 capability 버전, 그 위 4비트가 Device/Port Type 이다. */
	pdev->pcie_flags_reg = reg16;
	/* [한국어] 캐시. pci_pcie_type() 이 이 값에서 타입 필드를 뽑아 쓴다 */

	type = pci_pcie_type(pdev);
	/* [한국어] 방금 캐시한 값에서 포트 종류를 추출 */
	if (type == PCI_EXP_TYPE_ROOT_PORT)
		pci_enable_rrs_sv(pdev);
	/* [한국어] Root Port 에만 존재하는 Root Capabilities/Control 을 다루므로
	 * 여기서만 RRS 소프트웨어 가시성을 켠다. 이것이 켜져야 아래 장치가
	 * "준비 중"인지 "없는" 것인지 구별할 수 있다. */

	pci_read_config_dword(pdev, pos + PCI_EXP_DEVCAP, &pdev->devcap);
	/* [한국어] Device Capabilities — 최대 페이로드, FLR 지원 여부 등이 들어 있다.
	 * 통째로 캐시해 두고 필요한 비트를 그때그때 뽑아 쓴다. */
	pdev->pcie_mpss = FIELD_GET(PCI_EXP_DEVCAP_PAYLOAD, pdev->devcap);
	/* [한국어] MPSS(Max Payload Size Supported). 이 장치가 감당할 수 있는
	 * TLP 페이로드 크기 코드(0=128B, 1=256B, … 각 단계마다 2배).
	 * pci_configure_mps() 가 트리 전체의 최솟값에 맞춰 실제 MPS 를 정할 때
	 * 이 값을 근거로 쓴다. NVMe 의 DMA 전송 효율에 직접 영향을 준다. */

	pcie_capability_read_dword(pdev, PCI_EXP_LNKCAP, &reg32);
	/* [한국어] Link Capabilities — 속도, 폭, ASPM 지원 여부가 들어 있다 */
	if (reg32 & PCI_EXP_LNKCAP_DLLLARC)
		pdev->link_active_reporting = 1;
	/* [한국어] DLL Link Active Reporting Capable 비트.
	 * 이 기능이 있으면 링크가 실제로 살아났는지를 Link Status 의
	 * Data Link Layer Link Active 비트로 확인할 수 있다. 핫플러그나 리셋 후
	 * "언제까지 기다려야 하는가"를 추측이 아니라 사실로 알 수 있게 해 준다. */

#ifdef CONFIG_PCIEASPM
	/* [한국어] ASPM(Active State Power Management) 지원이 빌드에 포함될 때만.
	 * ASPM 은 링크가 유휴일 때 저전력 상태(L0s/L1)로 내리는 기능으로,
	 * NVMe 의 APST(장치 내부 전력 상태 전환)와는 다른 층위다 —
	 * ASPM 은 "링크"의 절전, APST 는 "컨트롤러"의 절전이다. */
	if (reg32 & PCI_EXP_LNKCAP_ASPM_L0S)
		pdev->aspm_l0s_support = 1;
	/* [한국어] L0s(빠른 복귀, 얕은 절전) 지원 여부 기록 */
	if (reg32 & PCI_EXP_LNKCAP_ASPM_L1)
		pdev->aspm_l1_support = 1;
	/* [한국어] L1(더 깊은 절전, 복귀 지연 큼) 지원 여부 기록.
	 * NVMe 지연에 민감한 환경에서 L1 이 문제가 되는 경우가 있어
	 * 이 정보가 정책 결정의 근거가 된다. */
#endif

	parent = pci_upstream_bridge(pdev);
	/* [한국어] 이 장치의 상위 브리지(루트 버스의 장치라면 NULL) */
	if (!parent)
		return;
	/* [한국어] 부모가 없으면 아래의 모순 검사를 할 수 없다 */

	/*
	 * Some systems do not identify their upstream/downstream ports
	 * correctly so detect impossible configurations here and correct
	 * the port type accordingly.
	 */
	if (type == PCI_EXP_TYPE_DOWNSTREAM) {
		/*
		 * If pdev claims to be downstream port but the parent
		 * device is also downstream port assume pdev is actually
		 * upstream port.
		 */
		if (pcie_downstream_port(parent)) {
			/* [한국어] 하향 포트 바로 아래에 또 하향 포트가 올 수는 없다.
			 * 스위치 구조는 반드시 "하향 포트 → 상향 포트 → 하향 포트들"
			 * 순서이므로, 이 장치는 사실 상향 포트다. */
			pci_info(pdev, "claims to be downstream port but is acting as upstream port, correcting type\n");
			pdev->pcie_flags_reg &= ~PCI_EXP_FLAGS_TYPE;
			/* [한국어] 캐시한 값에서 타입 필드만 지운다.
			 * 하드웨어 레지스터가 아니라 커널 캐시만 고치는 것이다. */
			pdev->pcie_flags_reg |= PCI_EXP_TYPE_UPSTREAM;
			/* [한국어] 올바른 타입으로 덮어쓴다 */
		}
	} else if (type == PCI_EXP_TYPE_UPSTREAM) {
		/*
		 * If pdev claims to be upstream port but the parent
		 * device is also upstream port assume pdev is actually
		 * downstream port.
		 */
		if (pci_pcie_type(parent) == PCI_EXP_TYPE_UPSTREAM) {
			/* [한국어] 상향 포트 아래에 또 상향 포트가 올 수도 없다.
			 * 상향 포트의 자식은 하향 포트여야 한다. */
			pci_info(pdev, "claims to be upstream port but is acting as downstream port, correcting type\n");
			pdev->pcie_flags_reg &= ~PCI_EXP_FLAGS_TYPE;
			/* [한국어] 타입 필드 제거 */
			pdev->pcie_flags_reg |= PCI_EXP_TYPE_DOWNSTREAM;
			/* [한국어] 하향 포트로 정정. 이 보정이 없으면 아래에서
			 * 확장 config 접근 판정이나 ASPM 정책이 어긋난다. */
		}
	}
}

/*
 * [한국어]
 * set_pcie_hotplug_bridge - 이 포트가 핫플러그 슬롯을 가졌는지 표시한다
 *
 * @pdev: 검사할 포트(보통 Root Port 나 스위치 하향 포트).
 * @return: 없음. 조건이 맞으면 pdev->is_hotplug_bridge 와 is_pciehp 가 1 이 된다.
 *
 * 왜 필요한가: 핫플러그 가능한 포트는 지금 아래가 비어 있어도 나중에 장치가
 * 꽂힐 수 있다. 그래서 버스 번호와 주소 창을 여유 있게 남겨 두어야 하며
 * (pci_scan_child_bus_extend 의 available_buses 분배), pciehp 드라이버가
 * 그 포트에 붙어 슬롯 이벤트를 처리해야 한다. 이 플래그가 그 판단의 근거다.
 *
 * NVMe 접점: U.2/U.3 백플레인의 NVMe 베이가 대표적인 핫플러그 슬롯이다.
 * 이 플래그가 서야 SSD 를 뽑고 꽂는 것이 커널에 이벤트로 전달된다.
 *
 * 실행 컨텍스트: 프로세스 문맥, pci_setup_device() 안에서 호출.
 *
 * 호출 체인:
 *   pci_setup_device() → [set_pcie_hotplug_bridge]
 *     → pcie_capability_read_dword()
 */
void set_pcie_hotplug_bridge(struct pci_dev *pdev)
{
	u32 reg32;
	/* [한국어] Slot Capabilities 레지스터 값 */

	pcie_capability_read_dword(pdev, PCI_EXP_SLTCAP, &reg32);
	/* [한국어] PCIe capability 안의 Slot Capabilities.
	 * 슬롯이 있는 포트에만 의미가 있다. */
	if (reg32 & PCI_EXP_SLTCAP_HPC)
		pdev->is_hotplug_bridge = pdev->is_pciehp = 1;
	/* [한국어] HPC(Hot-Plug Capable) 비트가 서 있으면 두 플래그를 함께 세운다.
	 * is_hotplug_bridge = PCI 코어가 리소스를 여유 있게 잡을 근거,
	 * is_pciehp        = pciehp 드라이버가 이 포트를 맡을 근거. */
}

/*
 * [한국어]
 * set_pcie_thunderbolt - 이 장치가 Thunderbolt 컨트롤러의 일부인지 표시한다
 *
 * @dev: 대상 장치.
 * @return: 없음. 해당하면 dev->is_thunderbolt 가 1 이 된다.
 *
 * 왜 필요한가: Thunderbolt 는 PCIe 를 케이블 밖으로 터널링하는 기술이라,
 * 사용자가 임의의 장치를 물리적으로 꽂을 수 있다. 그런 경로의 장치는
 * DMA 공격 위험이 있어 IOMMU 정책을 더 엄격히 적용해야 한다. 그 판단의
 * 재료로 쓰인다.
 *
 * 검출 방법: Intel 이 정의한 Vendor-Specific capability(VSEC)의 존재로
 * 판별한다. 표준 필드로는 Thunderbolt 여부를 알 수 없어 벤더 확장을 본다.
 *
 * 실행 컨텍스트: 프로세스 문맥, pci_setup_device() 안에서 호출.
 *
 * 호출 체인:
 *   pci_setup_device() → [set_pcie_thunderbolt] → pci_find_vsec_capability()
 */
static void set_pcie_thunderbolt(struct pci_dev *dev)
{
	u16 vsec;
	/* [한국어] 찾은 VSEC 의 config 오프셋. 0 이면 없음 */

	/* Is the device part of a Thunderbolt controller? */
	vsec = pci_find_vsec_capability(dev, PCI_VENDOR_ID_INTEL, PCI_VSEC_ID_INTEL_TBT);
	/* [한국어] 벤더 ID(Intel)와 VSEC ID(Thunderbolt) 조합으로 찾는다.
	 * VSEC 는 확장 capability 영역에 있으므로 확장 config 접근이 가능해야 한다. */
	if (vsec)
		dev->is_thunderbolt = 1;
	/* [한국어] 존재하면 Thunderbolt 계열로 표시.
	 * IOMMU/보안 정책과 아래 set_pcie_untrusted() 판단에 쓰인다. */
}

/*
 * [한국어]
 * set_pcie_cxl - 이 포트/장치가 CXL 프로토콜로 동작 중인지 표시한다
 *
 * @dev: 대상 장치.
 * @return: 없음. dev->is_cxl 이 갱신된다.
 *
 * CXL 이란: 같은 PCIe 물리 링크 위에서 다른 프로토콜(CXL.cache/CXL.mem)을
 * 흘려보내는 기술이다. 링크가 어떤 프로토콜로 학습(training)되었는지는
 * 부팅 때 정해지므로, config 로 물어봐야 알 수 있다.
 *
 * 왜 부모부터 갱신하는가: 대체 프로토콜 학습 결과는 링크 단위로 정해지고
 * 나중에 바뀔 수 있다. 자식의 상태를 판단하기 전에 위쪽 링크 상태를 먼저
 * 최신화해야 트리 전체가 일관된다. 그래서 상위로 재귀한다(재귀 깊이는
 * PCI 트리 깊이만큼으로, 실제 하드웨어에서는 몇 단계에 불과하다).
 *
 * 실행 컨텍스트: 프로세스 문맥, config 읽기만 수행.
 *
 * 호출 체인:
 *   pci_setup_device() → [set_pcie_cxl] → (상위로 재귀) → pci_find_dvsec_capability()
 */
static void set_pcie_cxl(struct pci_dev *dev)
{
	struct pci_dev *bridge;
	/* [한국어] 상위 브리지 — 먼저 갱신할 대상 */
	u16 dvsec, cap;
	/* [한국어] dvsec = CXL DVSEC 의 오프셋, cap = Flexbus Port Status 값 */

	if (!pci_is_pcie(dev))
		return;
	/* [한국어] CXL 은 PCIe 링크 위에서만 성립한다. 구식 PCI 장치는 대상 아님 */

	/*
	 * Update parent's CXL state because alternate protocol training
	 * may have changed
	 */
	bridge = pci_upstream_bridge(dev);
	/* [한국어] 상위 브리지 획득(루트 버스 장치면 NULL) */
	if (bridge)
		set_pcie_cxl(bridge);
	/* [한국어] 위로 재귀해 조상들의 CXL 상태를 먼저 최신화한다.
	 * 루트에 닿으면 bridge 가 NULL 이 되어 재귀가 끝난다. */

	dvsec = pci_find_dvsec_capability(dev, PCI_VENDOR_ID_CXL,
					  PCI_DVSEC_CXL_FLEXBUS_PORT);
	/* [한국어] DVSEC(Designated Vendor-Specific Extended Capability) 중
	 * CXL 컨소시엄의 Flexbus Port 항목을 찾는다. DVSEC 는 확장 config
	 * 영역(0x100 이상)에 있다. */
	if (!dvsec)
		return;
	/* [한국어] 없으면 CXL 장치가 아니다. is_cxl 은 기존 값 그대로 둔다 */

	pci_read_config_word(dev, dvsec + PCI_DVSEC_CXL_FLEXBUS_PORT_STATUS,
			     &cap);
	/* [한국어] Flexbus Port Status — 이 링크가 실제로 어떤 CXL 하위
	 * 프로토콜로 동작 중인지를 알려 준다 */

	dev->is_cxl = FIELD_GET(PCI_DVSEC_CXL_FLEXBUS_PORT_STATUS_CACHE, cap) ||
		FIELD_GET(PCI_DVSEC_CXL_FLEXBUS_PORT_STATUS_MEM, cap);
	/* [한국어] CXL.cache 또는 CXL.mem 중 하나라도 활성이면 CXL 로 판정한다.
	 * (CXL.io 만 쓰는 경우는 사실상 일반 PCIe 와 같으므로 제외한다.)
	 * 매번 새로 계산해 덮어쓰므로, 프로토콜이 재학습되면 값이 갱신된다. */

}

/*
 * [한국어]
 * set_pcie_untrusted - 외부에서 꽂힌 장치를 "신뢰할 수 없음"으로 표시한다
 *
 * @dev: 대상 장치.
 * @return: 없음. 해당하면 dev->untrusted 가 true 가 된다.
 *
 * 왜 필요한가: PCIe 장치는 DMA 로 시스템 메모리에 직접 접근할 수 있다.
 * 사용자가 임의로 꽂을 수 있는 포트(Thunderbolt 등)에 악의적 장치를 연결하면
 * 메모리를 통째로 읽어 갈 수 있다(DMA 공격). untrusted 로 표시된 장치는
 * IOMMU 가 엄격한 매핑을 강제하고, ATS(Address Translation Services) 같은
 * "장치가 주소 변환을 캐시하는" 기능을 허용하지 않는다.
 *
 * 상속 규칙: 신뢰할 수 없는 브리지 아래는 전부 신뢰할 수 없다. 중간에
 * 안전한 장치를 끼워 넣어 우회할 수 없게 하기 위함이다.
 *
 * 실행 컨텍스트: 프로세스 문맥, pci_setup_device() 안에서 호출.
 *
 * 호출 체인:
 *   pci_setup_device() → [set_pcie_untrusted] → arch_pci_dev_is_removable()
 */
static void set_pcie_untrusted(struct pci_dev *dev)
{
	struct pci_dev *parent = pci_upstream_bridge(dev);
	/* [한국어] 상위 브리지 — 신뢰 여부를 물려받을 대상 */

	if (!parent)
		return;
	/* [한국어] 루트 버스에 직접 붙은 장치는 메인보드 온보드로 간주해
	 * 여기서 untrusted 로 만들지 않는다 */
	/*
	 * If the upstream bridge is untrusted we treat this device as
	 * untrusted as well.
	 */
	if (parent->untrusted) {
		/* [한국어] 부모가 신뢰 불가면 그 아래는 무조건 신뢰 불가.
		 * 이 상속이 트리를 따라 전파되므로, 외부 포트 아래 전체가
		 * 자동으로 표시된다. */
		dev->untrusted = true;
		return;
		/* [한국어] 이미 결론이 났으므로 아키텍처 판정은 볼 필요 없다 */
	}

	if (arch_pci_dev_is_removable(dev)) {
		/* [한국어] 아키텍처/펌웨어가 "이 포트는 외부 노출(external facing)"
		 * 이라고 알려 준 경우. x86 에서는 ACPI 의 ExternalFacingPort 속성이
		 * 근거가 된다. */
		pci_dbg(dev, "marking as untrusted\n");
		dev->untrusted = true;
		/* [한국어] 표시. 이후 IOMMU 설정과 ATS 허용 여부가 달라진다 */
	}
}

/*
 * [한국어]
 * pci_set_removable - 사용자가 뽑을 수 있는 장치로 표시한다
 *
 * @dev: 대상 장치.
 * @return: 없음. 해당하면 device 의 removable 속성이 DEVICE_REMOVABLE 이 된다.
 *
 * 왜 필요한가: 사용자 공간(udev, 데스크톱 환경)이 "이 장치는 사용자가 뽑을 수
 * 있다"를 알아야 안전 제거 UI 를 보여 주거나 마운트 정책을 달리할 수 있다.
 * sysfs 의 removable 속성으로 노출된다.
 *
 * untrusted 와의 차이: untrusted 는 보안 정책(DMA 제한)용이고, removable 은
 * 사용자 경험용이다. 판단 근거는 비슷하지만 쓰임이 다르다.
 *
 * 아래 영어 주석이 설명하는 정책이 중요하다: 전통적인 핫플러그 슬롯(서버의
 * U.2 NVMe 베이 같은 것)은 기술적으로는 뽑을 수 있지만, 케이스를 열어야
 * 접근할 수 있어 일반 사용자가 뽑는 대상으로 보지 않는다. 그래서
 * external_facing 으로 표시된 포트 아래만 removable 로 노출한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, pci_setup_device() 안에서 호출.
 *
 * 호출 체인:
 *   pci_setup_device() → [pci_set_removable] → dev_set_removable()
 */
static void pci_set_removable(struct pci_dev *dev)
{
	struct pci_dev *parent = pci_upstream_bridge(dev);
	/* [한국어] 상위 브리지 — removable 성질을 물려받을 대상 */

	if (!parent)
		return;
	/* [한국어] 루트 버스 직결 장치는 온보드로 간주 */
	/*
	 * We (only) consider everything tunneled below an external_facing
	 * device to be removable by the user. We're mainly concerned with
	 * consumer platforms with user accessible thunderbolt ports that are
	 * vulnerable to DMA attacks, and we expect those ports to be marked by
	 * the firmware as external_facing. Devices in traditional hotplug
	 * slots can technically be removed, but the expectation is that unless
	 * the port is marked with external_facing, such devices are less
	 * accessible to user / may not be removed by end user, and thus not
	 * exposed as "removable" to userspace.
	 */
	if (dev_is_removable(&parent->dev)) {
		/* [한국어] 부모가 removable 이면 그 아래도 removable — 상속 규칙.
		 * 외부 포트에 도킹을 물리고 거기에 또 장치를 다는 구조가 흔하다. */
		dev_set_removable(&dev->dev, DEVICE_REMOVABLE);
		return;
	}

	if (arch_pci_dev_is_removable(dev)) {
		/* [한국어] 아키텍처/펌웨어가 external_facing 포트라고 알려 준 경우.
		 * 위 영어 주석대로 서버의 일반 핫플러그 슬롯은 여기에 해당하지
		 * 않으므로, 백플레인의 NVMe 는 보통 removable 로 표시되지 않는다. */
		pci_dbg(dev, "marking as removable\n");
		dev_set_removable(&dev->dev, DEVICE_REMOVABLE);
		/* [한국어] sysfs 의 removable 속성이 "removable" 로 바뀐다 */
	}
}

/**
 * pci_ext_cfg_is_aliased - Is ext config space just an alias of std config?
 * @dev: PCI device
 *
 * PCI Express to PCI/PCI-X Bridge Specification, rev 1.0, 4.1.4 says that
 * when forwarding a type1 configuration request the bridge must check that
 * the extended register address field is zero.  The bridge is not permitted
 * to forward the transactions and must handle it as an Unsupported Request.
 * Some bridges do not follow this rule and simply drop the extended register
 * bits, resulting in the standard config space being aliased, every 256
 * bytes across the entire configuration space.  Test for this condition by
 * comparing the first dword of each potential alias to the vendor/device ID.
 * Known offenders:
 *   ASM1083/1085 PCIe-to-PCI Reversible Bridge (1b21:1080, rev 01 & 03)
 *   AMD/ATI SBx00 PCI to PCI Bridge (1002:4384, rev 40)
 */
/*
 * [한국어]
 * pci_ext_cfg_is_aliased - 확장 config 영역이 사실은 표준 영역의 그림자인지 검사
 *
 * @dev: 검사할 장치.
 * @return: true 면 0x100 이상이 0x00 의 반복(alias)일 뿐이므로 확장 config 를
 *          쓰면 안 된다. false 면 진짜 확장 영역이거나 판단 불가.
 *
 * 무엇이 문제인가(위 영어 주석의 quirk): 스펙상 PCIe-to-PCI 브리지는 확장
 * 레지스터 주소 비트가 0 이 아닌 config 요청을 아래로 전달하면 안 되고,
 * Unsupported Request 로 처리해야 한다. 그런데 일부 브리지가 그 비트를 그냥
 * 버리고 전달해 버린다. 그 결과 0x100 을 읽으면 0x00 이, 0x200 을 읽으면
 * 다시 0x00 이 읽히는 식으로 256바이트마다 같은 내용이 되풀이된다.
 * 알려진 문제 하드웨어는 영어 주석에 적힌 ASM1083/1085 와 AMD/ATI SBx00 이다.
 *
 * 검출 방법: 확장 영역의 각 256바이트 경계에서 첫 dword 를 읽어, 그것이
 * 표준 영역 0x00 의 Vendor/Device ID 와 같은지 본다. 모든 경계에서 같으면
 * 그림자다. 진짜 확장 capability 헤더가 우연히 Vendor ID 와 같을 확률은
 * 사실상 없다.
 *
 * 왜 CONFIG_PCI_QUIRKS 로 감싸는가: 이 검사는 15번의 config 읽기를 하므로
 * 공짜가 아니다. quirk 지원을 뺀 구성(임베디드 등)에서는 이런 고장난 브리지가
 * 없다고 보고 검사를 생략한다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_cfg_space_size_ext() → [pci_ext_cfg_is_aliased] → pci_read_config_dword()
 */
static bool pci_ext_cfg_is_aliased(struct pci_dev *dev)
{
#ifdef CONFIG_PCI_QUIRKS
	/* [한국어] quirk 지원이 켜진 구성에서만 실제 검사를 수행한다 */
	int pos, ret;
	/* [한국어] pos = 검사할 확장 영역 오프셋, ret = config 읽기 결과 코드 */
	u32 header, tmp;
	/* [한국어] header = 표준 영역 0x00 의 Vendor/Device ID(비교 기준),
	 * tmp = 확장 영역에서 읽은 값 */

	pci_read_config_dword(dev, PCI_VENDOR_ID, &header);
	/* [한국어] offset 0x00 — 하위 16비트가 Vendor ID, 상위 16비트가 Device ID.
	 * 이 4바이트가 그림자 검사의 지문 역할을 한다. */

	for (pos = PCI_CFG_SPACE_SIZE;
	     pos < PCI_CFG_SPACE_EXP_SIZE; pos += PCI_CFG_SPACE_SIZE) {
		/* [한국어] 0x100 부터 0xf00 까지 256바이트 간격으로 15군데를 본다.
		 * PCI_CFG_SPACE_SIZE = 256, PCI_CFG_SPACE_EXP_SIZE = 4096. */
		ret = pci_read_config_dword(dev, pos, &tmp);
		/* [한국어] 그 경계의 첫 dword 를 읽는다 */
		if ((ret != PCIBIOS_SUCCESSFUL) || (header != tmp))
			return false;
		/* [한국어] 읽기 자체가 실패했거나 값이 Vendor ID 와 다르면
		 * 그림자가 아니다(진짜 확장 영역이거나 판단 불가). 한 군데만
		 * 달라도 즉시 false 다. */
	}

	return true;
	/* [한국어] 15군데가 모두 Vendor ID 와 같았다 = 표준 영역이 반복되고 있다.
	 * 호출자는 이 장치의 config space 를 256바이트로 제한한다. */
#else
	return false;
	/* [한국어] quirk 지원이 없는 구성 — 검사 비용을 아끼고 정상으로 간주 */
#endif
}

/**
 * pci_cfg_space_size_ext - Get the configuration space size of the PCI device
 * @dev: PCI device
 *
 * Regular PCI devices have 256 bytes, but PCI-X 2 and PCI Express devices
 * have 4096 bytes.  Even if the device is capable, that doesn't mean we can
 * access it.  Maybe we don't have a way to generate extended config space
 * accesses, or the device is behind a reverse Express bridge.  So we try
 * reading the dword at 0x100 which must either be 0 or a valid extended
 * capability header.
 */
/*
 * [한국어]
 * pci_cfg_space_size_ext - 확장 config 영역에 실제로 접근되는지 시험한다
 *
 * @dev: 검사할 장치.
 * @return: 접근 가능하면 PCI_CFG_SPACE_EXP_SIZE(4096), 아니면
 *          PCI_CFG_SPACE_SIZE(256).
 *
 * 왜 "지원"과 "접근 가능"이 다른가: 장치가 PCIe 라서 4KB config 를 갖고
 * 있어도, 그 사이의 경로가 확장 주소를 전달하지 못하면 읽을 수 없다.
 * 위 영어 주석이 드는 예가 그것이다 — 확장 접근을 만들어 낼 방법이 없는
 * 플랫폼이거나, reverse Express 브리지 뒤에 있는 경우.
 *
 * 판정 근거: 0x100 의 dword 는 스펙상 "0(확장 capability 없음)이거나
 * 유효한 확장 capability 헤더"여야 한다. all-ones 가 나오면 응답이 없다는
 * 뜻이고, 표준 영역의 그림자면 Vendor ID 가 나온다. 둘 다 아니어야 통과다.
 *
 * NVMe 접점: AER, LTR, SR-IOV, ARI 같은 확장 capability 는 0x100 이상에
 * 있으므로, 여기서 4096 이 나와야 NVMe 컨트롤러의 오류 보고(AER)와
 * 지연 보고(LTR)를 쓸 수 있다. (MSI-X 는 표준 capability ID 0x11 이라
 * 256바이트 영역 안에 있어, 확장 접근이 막혀도 영향받지 않는다.)
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_cfg_space_size() → [pci_cfg_space_size_ext] → pci_ext_cfg_is_aliased()
 */
static int pci_cfg_space_size_ext(struct pci_dev *dev)
{
	u32 status;
	/* [한국어] 0x100 에서 읽은 값 */
	int pos = PCI_CFG_SPACE_SIZE;
	/* [한국어] 확장 영역의 시작 오프셋 = 0x100 */

	if (pci_read_config_dword(dev, pos, &status) != PCIBIOS_SUCCESSFUL)
		return PCI_CFG_SPACE_SIZE;
	/* [한국어] 읽기 자체가 실패 — 이 플랫폼/경로에서는 확장 접근을 만들 수
	 * 없다는 뜻이므로 256바이트로 제한한다. */
	if (PCI_POSSIBLE_ERROR(status) || pci_ext_cfg_is_aliased(dev))
		return PCI_CFG_SPACE_SIZE;
	/* [한국어] 두 가지 실패 방식을 함께 거른다.
	 * (1) all-ones = 응답 없음(요청이 어딘가에서 버려졌다).
	 * (2) 그림자(alias) = 고장난 브리지가 확장 주소 비트를 버리고 있다.
	 * 어느 쪽이든 확장 영역을 신뢰할 수 없다. */

	return PCI_CFG_SPACE_EXP_SIZE;
	/* [한국어] 4096바이트 전체를 쓸 수 있다. 이 값이 dev->cfg_size 가 되고,
	 * pci_find_ext_capability() 가 그 안에서 확장 capability 를 찾는다. */
}

/*
 * [한국어]
 * pci_cfg_space_size - 이 장치의 config space 크기를 확정한다
 *
 * @dev: 대상 장치. dev->class 와 dev->pcie_cap 이 이미 채워져 있어야 한다.
 * @return: 256 또는 4096.
 *
 * 왜 필요한가: dev->cfg_size 는 이후 모든 config 접근의 상한이 된다.
 * sysfs 의 config 파일 크기도 이 값이고, pci_find_ext_capability() 는
 * cfg_size 가 256 이면 아예 탐색을 시작하지 않는다. 즉 이 판정 하나가
 * 그 장치에서 쓸 수 있는 확장 기능 전체를 좌우한다.
 *
 * 판정 순서:
 *  1) SR-IOV VF 는 무조건 4096(아래 영어 주석의 근거 참조).
 *  2) 상위 버스가 확장 접근 불가로 표시되어 있으면 256.
 *  3) 호스트 브리지이거나 PCIe 장치면 실제 접근을 시험해 본다.
 *  4) 구식 PCI 라도 PCI-X Mode 2 면 시험해 본다.
 *  5) 그 밖에는 256.
 *
 * NVMe 접점: NVMe 컨트롤러는 PCIe 장치이므로 3)번 경로를 타고,
 * 정상이라면 4096 이 되어 AER/LTR/ARI 같은 확장 capability 를 쓸 수 있다.
 *
 * 실행 컨텍스트: 프로세스 문맥, pci_setup_device() 안에서 호출.
 *
 * 호출 체인:
 *   pci_setup_device() → [pci_cfg_space_size] → pci_cfg_space_size_ext()
 */
int pci_cfg_space_size(struct pci_dev *dev)
{
	int pos;
	/* [한국어] PCI-X capability 오프셋 */
	u32 status;
	/* [한국어] PCI-X Status 레지스터 값 */
	u16 class;
	/* [한국어] 상위 3바이트로 줄인 클래스 코드 */

#ifdef CONFIG_PCI_IOV
	/*
	 * Per the SR-IOV specification (rev 1.1, sec 3.5), VFs are required to
	 * implement a PCIe capability and therefore must implement extended
	 * config space.  We can skip the NO_EXTCFG test below and the
	 * reachability/aliasing test in pci_cfg_space_size_ext() by virtue of
	 * the fact that the SR-IOV capability on the PF resides in extended
	 * config space and must be accessible and non-aliased to have enabled
	 * support for this VF.  This is a micro performance optimization for
	 * systems supporting many VFs.
	 */
	if (dev->is_virtfn)
		return PCI_CFG_SPACE_EXP_SIZE;
	/* [한국어] 위 영어 주석의 논리가 정연하다: VF 가 존재한다는 것 자체가
	 * PF 의 SR-IOV capability 를 읽을 수 있었다는 뜻이고, 그 capability 는
	 * 확장 영역에 있다. 즉 확장 접근이 되고 그림자도 아님이 이미 증명된
	 * 상태다. VF 가 수백 개인 시스템에서 검사를 반복하지 않으려는
	 * 성능 최적화다. */
#endif

	if (dev->bus->bus_flags & PCI_BUS_FLAGS_NO_EXTCFG)
		return PCI_CFG_SPACE_SIZE;
	/* [한국어] pci_alloc_child_bus() 가 이 버스에 확장 접근 불가 표시를
	 * 해 두었다면 시험해 볼 것도 없이 256 이다. */

	class = dev->class >> 8;
	/* [한국어] dev->class 는 하위 8비트가 revision 이고 그 위 24비트가
	 * 클래스/서브클래스/프로그래밍 인터페이스다. 8비트 내려 클래스 부분만
	 * 취한다(u16 이라 상위 8비트는 잘리고 class/subclass 만 남는다). */
	if (class == PCI_CLASS_BRIDGE_HOST)
		return pci_cfg_space_size_ext(dev);
	/* [한국어] 호스트 브리지는 PCIe capability 를 갖지 않는 경우가 많지만
	 * 확장 config 는 가질 수 있다. 그래서 별도로 먼저 시험한다. */

	if (pci_is_pcie(dev))
		return pci_cfg_space_size_ext(dev);
	/* [한국어] PCIe 장치는 4KB config 를 갖는다. NVMe 컨트롤러가 여기로 온다 */

	pos = pci_find_capability(dev, PCI_CAP_ID_PCIX);
	/* [한국어] 남은 것은 구식 PCI/PCI-X 장치. PCI-X capability 를 찾아본다 */
	if (!pos)
		return PCI_CFG_SPACE_SIZE;
	/* [한국어] 순수 구식 PCI 장치 — 256바이트가 전부다 */

	pci_read_config_dword(dev, pos + PCI_X_STATUS, &status);
	/* [한국어] PCI-X Status 를 읽어 Mode 2 지원 여부를 본다 */
	if (status & (PCI_X_STATUS_266MHZ | PCI_X_STATUS_533MHZ))
		return pci_cfg_space_size_ext(dev);
	/* [한국어] 266/533MHz 지원 = PCI-X Mode 2. Mode 2 부터 4KB config 가
	 * 규정되어 있으므로 실제 접근을 시험해 본다. */

	return PCI_CFG_SPACE_SIZE;
	/* [한국어] PCI-X Mode 1 이하 — 256바이트 */
}

/*
 * [한국어]
 * pci_class - 장치의 클래스 코드와 리비전을 얻는다
 *
 * @dev: 대상 장치.
 * @return: offset 0x08 의 4바이트. 하위 8비트 = Revision ID,
 *          그 위 24비트 = Class Code(base class / sub-class / prog-if).
 *
 * 왜 필요한가: 클래스 코드는 "이 장치가 무엇인가"를 벤더와 무관하게 나타내는
 * 표준 분류다. 드라이버가 특정 벤더가 아니라 종류로 매칭할 수 있게 해 준다.
 *
 * NVMe 접점: NVMe 컨트롤러의 클래스 코드는 0x010802 다
 * (base 0x01 = Mass Storage, sub 0x08 = Non-Volatile Memory,
 *  prog-if 0x02 = NVM Express). include/linux/pci_ids.h 의
 * PCI_CLASS_STORAGE_EXPRESS 가 그 값이며, drivers/nvme/host/pci.c:4137 의
 * nvme_id_table 이 PCI_DEVICE_CLASS(PCI_CLASS_STORAGE_EXPRESS, 0xffffff) 로
 * 이 클래스 전체를 잡는다. 즉 여기서 읽은 값이 dev->class 에 들어가고,
 * pci_device_add() → device_add() 이후의 드라이버 매칭에서 nvme 드라이버가
 * 그 장치를 자기 것으로 인식하는 근거가 된다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_setup_device() → [pci_class] → pci_read_config_dword()
 */
static u32 pci_class(struct pci_dev *dev)
{
	u32 class;
	/* [한국어] 읽어 온 클래스+리비전 4바이트 */

#ifdef CONFIG_PCI_IOV
	if (dev->is_virtfn)
		return dev->physfn->sriov->class;
	/* [한국어] SR-IOV 가상 함수는 자기 config 에 클래스 코드를 갖지 않는다.
	 * PF(물리 함수)의 SR-IOV capability 가 VF 들의 공통 클래스를 기술하므로,
	 * PF 쪽에 저장해 둔 값을 그대로 쓴다. */
#endif
	pci_read_config_dword(dev, PCI_CLASS_REVISION, &class);
	/* [한국어] offset 0x08 — Revision(0x08)과 Class Code(0x09~0x0b)가
	 * 연속해 있어 한 번의 dword 읽기로 둘 다 얻는다. */
	return class;
	/* [한국어] 호출자가 dev->class 에 그대로 저장한다 */
}

/*
 * [한국어]
 * pci_subsystem_ids - 서브시스템 벤더/장치 ID 를 얻는다
 *
 * @dev:    대상 장치.
 * @vendor: 서브시스템 벤더 ID 를 받을 곳.
 * @device: 서브시스템 장치 ID 를 받을 곳.
 * @return: 없음.
 *
 * 왜 필요한가: Vendor/Device ID 는 칩 자체를 식별하지만, 같은 칩을 여러 회사가
 * 서로 다른 보드에 얹어 판다. 서브시스템 ID 는 "그 칩을 누가 어떤 제품으로
 * 만들었는가"를 나타내며, 보드마다 다른 quirk 를 적용할 때 결정적인 단서가 된다.
 *
 * config 위치(header type 0 전용):
 *   0x2c PCI_SUBSYSTEM_VENDOR_ID (16bit)
 *   0x2e PCI_SUBSYSTEM_ID        (16bit)
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_setup_device() → [pci_subsystem_ids] → pci_read_config_word()
 */
static void pci_subsystem_ids(struct pci_dev *dev, u16 *vendor, u16 *device)
{
#ifdef CONFIG_PCI_IOV
	if (dev->is_virtfn) {
		/* [한국어] VF 는 서브시스템 ID 도 자기 config 에 갖지 않는다.
		 * PF 의 SR-IOV capability 에 기록된 값을 물려받는다. */
		*vendor = dev->physfn->sriov->subsystem_vendor;
		/* [한국어] PF 가 기억해 둔 서브시스템 벤더 ID */
		*device = dev->physfn->sriov->subsystem_device;
		/* [한국어] PF 가 기억해 둔 서브시스템 장치 ID */
		return;
	}
#endif
	pci_read_config_word(dev, PCI_SUBSYSTEM_VENDOR_ID, vendor);
	/* [한국어] offset 0x2c */
	pci_read_config_word(dev, PCI_SUBSYSTEM_ID, device);
	/* [한국어] offset 0x2e */
}

/*
 * [한국어]
 * pci_hdr_type - 헤더 타입 바이트를 읽는다
 *
 * @dev: 대상 장치.
 * @return: offset 0x0e 의 8비트 원본. 하위 7비트가 헤더 타입,
 *          최상위 비트(0x80, PCI_HEADER_TYPE_MFD)가 멀티펑션 표시다.
 *
 * 헤더 타입이란: config space 의 앞 64바이트 중 뒤쪽 48바이트의 레이아웃이
 * 장치 종류마다 다르다. 그 레이아웃을 고르는 값이다.
 *   0 (PCI_HEADER_TYPE_NORMAL)  : 일반 장치. BAR 6개(0x10~0x27),
 *                                 서브시스템 ID(0x2c), ROM(0x30).
 *   1 (PCI_HEADER_TYPE_BRIDGE)  : PCI-to-PCI 브리지. BAR 2개(0x10~0x17),
 *                                 버스 번호(0x18~0x1a), I/O 창(0x1c),
 *                                 MEM 창(0x20), prefetch 창(0x24), ROM(0x38).
 *   2 (PCI_HEADER_TYPE_CARDBUS) : CardBus 브리지. BAR 1개, 전혀 다른 배치.
 * 앞 16바이트(Vendor/Device/Command/Status/Class/헤더 타입 등)는 세 타입이
 * 공통이라, 헤더 타입 자체는 어느 장치에서든 같은 자리에서 읽을 수 있다.
 *
 * 멀티펑션 비트: function 0 의 헤더 타입 최상위 비트가 1 이면 그 슬롯에
 * function 1~7 이 더 있을 수 있다는 뜻이다. pci_scan_slot() 이 이 비트를
 * 보고 나머지 function 을 훑을지 결정한다.
 *
 * NVMe 접점: NVMe 컨트롤러는 header type 0(일반 장치)이므로 BAR 6칸을 갖고,
 * 그중 BAR0(+64비트면 BAR1)이 컨트롤러 레지스터 창이 된다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_setup_device() → [pci_hdr_type] → pci_read_config_byte()
 */
static u8 pci_hdr_type(struct pci_dev *dev)
{
	u8 hdr_type;
	/* [한국어] 읽어 온 헤더 타입 바이트(멀티펑션 비트 포함) */

#ifdef CONFIG_PCI_IOV
	if (dev->is_virtfn)
		return dev->physfn->sriov->hdr_type;
	/* [한국어] VF 의 헤더 타입도 PF 의 SR-IOV capability 가 기술한다 */
#endif
	pci_read_config_byte(dev, PCI_HEADER_TYPE, &hdr_type);
	/* [한국어] offset 0x0e. 세 헤더 타입 모두 같은 자리에 있어,
	 * 레이아웃을 알기 전에도 안전하게 읽을 수 있다. */
	return hdr_type;
	/* [한국어] 호출자가 PCI_HEADER_TYPE_MASK(0x7f)로 타입만 뽑아 쓰고,
	 * 0x80 비트는 멀티펑션 판정에 따로 쓴다. */
}

#define LEGACY_IO_RESOURCE	(IORESOURCE_IO | IORESOURCE_PCI_FIXED)
/* [한국어] IDE 컨트롤러의 레거시 고정 I/O 포트(0x1f0, 0x3f6 등)를 나타낼 때
 * 쓰는 플래그 조합. IORESOURCE_IO = I/O 포트 공간,
 * IORESOURCE_PCI_FIXED = 주소가 하드웨어에 고정되어 재배치할 수 없음.
 * 아래 pci_setup_device() 의 IDE 처리에서 사용한다. */

/**
 * pci_intx_mask_broken - Test PCI_COMMAND_INTX_DISABLE writability
 * @dev: PCI device
 *
 * Test whether PCI_COMMAND_INTX_DISABLE is writable for @dev.  Check this
 * at enumeration-time to avoid modifying PCI_COMMAND at run-time.
 */
/*
 * [한국어]
 * pci_intx_mask_broken - INTx 차단 비트가 실제로 쓰기 가능한지 시험한다
 *
 * @dev: 대상 장치.
 * @return: 1 이면 쓰기가 안 된다(= INTx 를 소프트웨어로 막을 수 없다), 0 이면 정상.
 *
 * 무엇을 시험하나: Command 레지스터(0x04)의 bit 10(PCI_COMMAND_INTX_DISABLE)은
 * "이 장치의 레거시 INTx 인터럽트를 내보내지 마라"는 스위치다. PCI 2.3 에서
 * 추가된 것이라, 그 이전 장치에서는 예약(읽기 전용)이다.
 *
 * 왜 열거 시점에 시험하나: 위 영어 주석이 이유를 밝힌다 — 이 시험은
 * Command 레지스터를 잠시 바꾸는 파괴적 동작이므로, 드라이버가 장치를
 * 쓰고 있는 런타임에 하면 위험하다. 아직 아무도 쓰지 않는 열거 단계에
 * 미리 해 두고 결과만 플래그로 남긴다.
 *
 * 이름이 부정확하다는 점: 영어 주석대로, PCI 2.3 이전 장치가 이 비트를
 * 지원하지 않는 것은 "고장"이 아니라 규격대로다. 그래도 커널 입장에서는
 * 소프트웨어로 인터럽트를 막을 수 없다는 결과가 같아 broken 으로 부른다.
 *
 * NVMe 접점: NVMe 컨트롤러는 실제로는 MSI-X 를 쓰지만, VFIO 로 장치를
 * 사용자 공간에 넘길 때처럼 INTx 를 소프트웨어로 마스킹해야 하는 경우가
 * 있어 이 정보가 필요하다.
 *
 * 실행 컨텍스트: 프로세스 문맥, pci_setup_device() 안에서 호출.
 *
 * 호출 체인:
 *   pci_setup_device() → [pci_intx_mask_broken] → pci_read/write_config_word()
 */
static int pci_intx_mask_broken(struct pci_dev *dev)
{
	u16 orig, toggle, new;
	/* [한국어] orig = 원본 Command 값, toggle = 해당 비트만 뒤집은 값,
	 * new = 되읽은 값. */

	pci_read_config_word(dev, PCI_COMMAND, &orig);
	/* [한국어] 원본 백업 — 시험이 끝나면 반드시 되돌린다 */
	toggle = orig ^ PCI_COMMAND_INTX_DISABLE;
	/* [한국어] XOR 로 INTx Disable 비트 하나만 반전시킨다. 다른 비트는
	 * 그대로 두어야 장치의 현재 동작(디코딩 등)을 건드리지 않는다. */
	pci_write_config_word(dev, PCI_COMMAND, toggle);
	/* [한국어] 반전된 값을 써 본다 */
	pci_read_config_word(dev, PCI_COMMAND, &new);
	/* [한국어] 되읽어 그 비트가 실제로 바뀌었는지 확인 */

	pci_write_config_word(dev, PCI_COMMAND, orig);
	/* [한국어] 결과와 무관하게 원본 복원. 판정보다 먼저 복원해 두어야
	 * 어떤 경로로도 장치를 이상한 상태로 남기지 않는다. */

	/*
	 * PCI_COMMAND_INTX_DISABLE was reserved and read-only prior to PCI
	 * r2.3, so strictly speaking, a device is not *broken* if it's not
	 * writable.  But we'll live with the misnomer for now.
	 */
	if (new != toggle)
		return 1;
	/* [한국어] 쓴 값이 그대로 읽히지 않았다 = 그 비트가 읽기 전용이다.
	 * 호출자가 dev->broken_intx_masking 을 세운다. */
	return 0;
	/* [한국어] 정상적으로 쓰기가 되는 장치 */
}

/*
 * [한국어]
 * early_dump_pci_device - 장치의 표준 config space 256바이트를 통째로 로그에 찍는다
 *
 * @pdev: 대상 장치.
 * @return: 없음.
 *
 * 왜 필요한가: 하드웨어 브링업이나 열거 단계의 버그를 쫓을 때, 커널이 무엇을
 * 보고 판단했는지 원본 그대로 확인해야 할 때가 있다. pci_early_dump 커널
 * 파라미터가 켜져 있을 때만 pci_setup_device() 가 이 함수를 부른다.
 *
 * 왜 256바이트만인가: 이 시점에는 dev->cfg_size 판정(pci_cfg_space_size)이
 * 아직 이루어지지 않아 확장 영역 접근 가능 여부를 모른다. 어느 장치에나
 * 있는 표준 영역만 안전하게 덤프한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 배열 64칸(256바이트)을 스택에 잡는다.
 *
 * 호출 체인:
 *   pci_setup_device() → [early_dump_pci_device] → print_hex_dump()
 */
static void early_dump_pci_device(struct pci_dev *pdev)
{
	u32 value[PCI_CFG_SPACE_SIZE / sizeof(u32)];
	/* [한국어] 256 / 4 = 64칸. config 를 dword 단위로 모아 담을 버퍼 */
	int i;
	/* [한국어] 순회 인덱스 */

	pci_info(pdev, "config space:\n");
	/* [한국어] 덤프 시작을 알리는 머리글 */

	for (i = 0; i < ARRAY_SIZE(value); i++)
		pci_read_config_dword(pdev, i * sizeof(u32), &value[i]);
	/* [한국어] 오프셋 0, 4, 8 … 252 를 차례로 읽는다. config 접근은
	 * dword 단위가 기본이라 4의 배수 오프셋으로 접근한다. */

	print_hex_dump(KERN_INFO, "", DUMP_PREFIX_OFFSET, 16, 1,
		       value, ARRAY_SIZE(value) * sizeof(u32), false);
	/* [한국어] 커널 표준 16진 덤프. 한 줄에 16바이트씩, 1바이트 단위로
	 * 끊어 오프셋을 앞에 붙여 출력한다. 마지막 false 는 ASCII 병기를
	 * 하지 않는다는 뜻이다. */
}

/*
 * [한국어]
 * pci_type_str - 장치 종류를 사람이 읽는 문자열로 바꾼다
 *
 * @dev: 대상 장치. hdr_type 과 pcie_flags_reg 가 이미 채워져 있어야 한다.
 * @return: "PCIe Endpoint" 같은 정적 문자열(해제 금지).
 *
 * 왜 필요한가: 부팅 로그에서 각 장치가 무엇으로 인식되었는지 한눈에 보이게
 * 하려는 목적이다. 특히 상향/하향 포트 타입 보정이 있었는지, 장치가 PCIe 로
 * 잡혔는지 구식 PCI 로 잡혔는지 진단할 때 쓰인다.
 *
 * NVMe 접점: NVMe SSD 는 PCIe Endpoint 이므로 로그에 "PCIe Endpoint" 로
 * 나타난다. 만약 다르게 나온다면 PCIe capability 를 못 찾았거나 타입 필드가
 * 이상하다는 신호다.
 *
 * 실행 컨텍스트: 순수 함수.
 *
 * 호출 체인:
 *   pci_setup_device() → [pci_type_str]
 */
static const char *pci_type_str(struct pci_dev *dev)
{
	/*
	 * [한국어] 인덱스가 곧 PCI_EXP_TYPE_* 값인 문자열 표.
	 * 0x0 Endpoint, 0x1 Legacy Endpoint, 0x4 Root Port,
	 * 0x5 Switch Upstream, 0x6 Switch Downstream,
	 * 0x7 PCIe-to-PCI/PCI-X bridge, 0x8 PCI/PCI-X-to-PCIe bridge,
	 * 0x9 Root Complex Integrated Endpoint, 0xa Root Complex Event Collector.
	 * 0x2, 0x3 은 정의되지 않은 자리라 "unknown" 으로 채워 표의 정렬을 맞춘다.
	 */
	static const char * const str[] = {
		"PCIe Endpoint",
		/* [한국어] 타입 0x0 — 일반 PCIe 엔드포인트. NVMe SSD 가 여기다 */
		"PCIe Legacy Endpoint",
		/* [한국어] 타입 0x1 — I/O 공간을 쓰는 등 구식 동작을 남긴 엔드포인트 */
		"PCIe unknown",
		/* [한국어] 타입 0x2 — 스펙상 정의되지 않은 값 */
		"PCIe unknown",
		/* [한국어] 타입 0x3 — 스펙상 정의되지 않은 값 */
		"PCIe Root Port",
		/* [한국어] 타입 0x4 — 루트 컴플렉스에서 나오는 하향 포트 */
		"PCIe Switch Upstream Port",
		/* [한국어] 타입 0x5 — 스위치의 위쪽 포트 */
		"PCIe Switch Downstream Port",
		/* [한국어] 타입 0x6 — 스위치의 아래쪽 포트들 */
		"PCIe to PCI/PCI-X bridge",
		/* [한국어] 타입 0x7 — PCIe 아래에 구식 PCI 를 붙이는 정방향 브리지 */
		"PCI/PCI-X to PCIe bridge",
		/* [한국어] 타입 0x8 — 구식 PCI 아래에 PCIe 를 붙이는 역방향 브리지 */
		"PCIe Root Complex Integrated Endpoint",
		/* [한국어] 타입 0x9 — 링크 없이 루트 컴플렉스에 내장된 장치 */
		"PCIe Root Complex Event Collector",
		/* [한국어] 타입 0xa — 내장 엔드포인트들의 오류를 모아 보고하는 장치 */
	};
	int type;
	/* [한국어] 표에서 찾을 PCIe 포트 타입 */

	if (pci_is_pcie(dev)) {
		/* [한국어] pcie_cap 이 0 이 아니면 PCIe 장치다 */
		type = pci_pcie_type(dev);
		/* [한국어] 캐시해 둔 pcie_flags_reg 에서 타입 필드를 뽑는다.
		 * set_pcie_port_type() 이 보정한 값이 반영된다. */
		if (type < ARRAY_SIZE(str))
			return str[type];
		/* [한국어] 표 안이면 그대로 반환 */

		return "PCIe unknown";
		/* [한국어] 표 밖의 값 — 미래 스펙이거나 고장난 장치 */
	}

	switch (dev->hdr_type) {
		/* [한국어] PCIe 가 아니면 헤더 타입으로 분류할 수밖에 없다.
		 * 구식 PCI 에는 포트 타입 개념 자체가 없기 때문이다. */
	case PCI_HEADER_TYPE_NORMAL:
		return "conventional PCI endpoint";
		/* [한국어] 헤더 타입 0 — 일반 장치 */
	case PCI_HEADER_TYPE_BRIDGE:
		return "conventional PCI bridge";
		/* [한국어] 헤더 타입 1 — PCI-to-PCI 브리지 */
	case PCI_HEADER_TYPE_CARDBUS:
		return "CardBus bridge";
		/* [한국어] 헤더 타입 2 — CardBus 브리지 */
	default:
		return "conventional PCI";
		/* [한국어] 스펙에 없는 헤더 타입. 이 경우 pci_setup_device() 가
		 * 아래에서 -EIO 로 거절하므로 실제로는 거의 보이지 않는다. */
	}
}

/**
 * pci_setup_device - Fill in class and map information of a device
 * @dev: the device structure to fill
 *
 * Initialize the device structure with information about the device's
 * vendor,class,memory and IO-space addresses, IRQ lines etc.
 * Called at initialisation of the PCI subsystem and by CardBus services.
 * Returns 0 on success and negative if unknown type of device (not normal,
 * bridge or CardBus).
 */
/*
 * [한국어]
 * pci_setup_device - config space 를 해석해 pci_dev 를 채운다 (헤더 타입 분기의 중심)
 *
 * @dev: 이미 vendor/device/devfn/bus 가 채워진 갓 만들어진 pci_dev.
 * @return: 0 이면 성공. 알 수 없는 헤더 타입이면 -EIO(호출자가 이 장치를 버린다).
 *          pci_set_of_node() 실패 시 그 오류를 그대로 전달한다.
 *
 * 왜 필요한가: pci_scan_device() 가 "여기 장치가 있다"까지 확인했다면, 이
 * 함수가 "그 장치가 무엇이고 어떤 자원을 요구하는가"를 알아낸다. 클래스,
 * 리비전, 서브시스템 ID, 헤더 타입, BAR, IRQ, 그리고 각종 성질 플래그가
 * 여기서 확정된다. 이 함수가 끝나야 pci_device_add() 로 넘어갈 수 있다.
 *
 * ★ 헤더 타입 분기 ★ — 이 함수의 핵심이다. config space 의 앞 16바이트
 * (Vendor/Device/Command/Status/Class/CacheLine/헤더 타입/BIST)는 모든 장치가
 * 같지만, 0x10 이후는 헤더 타입에 따라 완전히 다르다:
 *
 *   type 0 (일반 장치, NVMe SSD 가 여기):
 *     0x10~0x27 BAR 0~5 (6칸)
 *     0x28      CardBus CIS 포인터
 *     0x2c/0x2e 서브시스템 벤더/장치 ID
 *     0x30      확장 ROM 주소
 *     0x34      Capabilities 포인터
 *     0x3c/0x3d Interrupt Line / Pin
 *
 *   type 1 (PCI-to-PCI 브리지):
 *     0x10~0x17 BAR 0~1 (2칸뿐 — 브리지는 자기 자원이 거의 없다)
 *     0x18~0x1b primary/secondary/subordinate 버스 번호 + latency
 *     0x1c~0x1d I/O 창 base/limit
 *     0x20~0x23 메모리 창 base/limit
 *     0x24~0x2f prefetchable 창 base/limit (+ 상위 32비트)
 *     0x30~0x33 I/O 창 상위 16비트
 *     0x34      Capabilities 포인터 (type 0 과 같은 자리)
 *     0x38      확장 ROM 주소 (type 0 의 0x30 과 다른 자리!)
 *     0x3e      Bridge Control
 *     서브시스템 ID 는 표준 필드가 없어 SSVID capability 로 얻는다.
 *
 *   type 2 (CardBus 브리지):
 *     BAR 1칸, 0x14 에 Capabilities 포인터(다른 두 타입은 0x34),
 *     0x18~0x1a 버스 번호, 메모리/IO 창이 각각 2쌍씩, 서브시스템 ID 는 0x40/0x42.
 *
 * 따라서 pci_read_bases() 에 넘기는 BAR 개수(6/2/1)와 ROM 오프셋
 * (PCI_ROM_ADDRESS/PCI_ROM_ADDRESS1/없음)이 타입마다 다르고, 서브시스템 ID 를
 * 얻는 방법도 셋 다 다르다.
 *
 * 정합성 검사: 헤더 타입과 클래스 코드는 서로 맞아야 한다. 예를 들어 헤더
 * 타입 0(일반 장치)인데 클래스가 PCI-to-PCI 브리지라면 모순이다. 그런 장치는
 * bad 레이블로 가서 클래스를 "정의되지 않음"으로 바꾸고 계속 진행한다
 * (장치를 버리지는 않는다 — BAR 정보는 여전히 유효할 수 있으므로).
 *
 * NVMe 접점: NVMe SSD 는 header type 0 이고 클래스는 0x010802 다. 이 함수의
 * type 0 분기에서 pci_read_bases(dev, PCI_STD_NUM_BARS, PCI_ROM_ADDRESS) 가
 * 실행되어 BAR0 의 크기와 타입이 dev->resource[0] 에 확정된다. 나중에
 * drivers/nvme/host/pci.c 가 pci_resource_start/len(pdev, 0) 으로 읽어 가는
 * 바로 그 값이다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 장치가 아직 어떤 드라이버에도 바인딩되지
 * 않은 상태라, BAR 를 흔들거나 Command 를 바꾸는 조작이 안전하다.
 *
 * 호출 체인:
 *   pci_scan_device() → [pci_setup_device]
 *     → pci_hdr_type(), set_pcie_port_type(), pci_class(),
 *       pci_cfg_space_size(), pci_read_irq(), pci_read_bases(),
 *       pci_read_bridge_windows()
 */
int pci_setup_device(struct pci_dev *dev)
{
	u32 class;
	/* [한국어] 클래스+리비전 4바이트, 나중에는 클래스만 담는 용도로 재사용 */
	u16 cmd;
	/* [한국어] Command 레지스터 값(비정상 BAR 장치의 디코딩을 끌 때 사용) */
	u8 hdr_type;
	/* [한국어] 헤더 타입 원본(멀티펑션 비트 포함) */
	int err, pos = 0;
	/* [한국어] err = 오류 코드, pos = capability 오프셋(브리지의 SSVID 용) */
	struct pci_bus_region region;
	/* [한국어] 레거시 IDE quirk 에서 고정 포트 주소를 리소스로 바꿀 때 쓰는 버퍼 */
	struct resource *res;
	/* [한국어] 그 quirk 에서 채울 리소스 슬롯 포인터 */

	hdr_type = pci_hdr_type(dev);
	/* [한국어] offset 0x0e — 이후 모든 파싱의 분기 기준이 되는 값 */

	dev->sysdata = dev->bus->sysdata;
	/* [한국어] 버스로부터 컨트롤러 전용 데이터 포인터를 물려받는다 */
	dev->dev.parent = dev->bus->bridge;
	/* [한국어] sysfs 계층에서 이 장치를 브리지 아래에 놓는다 */
	dev->dev.bus = &pci_bus_type;
	/* [한국어] 드라이버 모델의 "PCI 버스 타입"에 소속시킨다. 이 값이 있어야
	 * device_add() 이후 pci_bus_type.match(= pci_bus_match) 가 불려
	 * id_table 기반 드라이버 매칭이 일어난다. NVMe 드라이버가 이 장치를
	 * 자기 것으로 인식하게 되는 출발점이다. */
	dev->hdr_type = FIELD_GET(PCI_HEADER_TYPE_MASK, hdr_type);
	/* [한국어] 마스크 0x7f — 멀티펑션 비트를 제외한 순수 헤더 타입(0/1/2) */
	dev->multifunction = FIELD_GET(PCI_HEADER_TYPE_MFD, hdr_type);
	/* [한국어] 마스크 0x80 — 이 슬롯에 function 이 더 있을 수 있다는 표시.
	 * pci_scan_slot() 이 function 1~7 을 훑을지 결정할 때 본다. */
	dev->error_state = pci_channel_io_normal;
	/* [한국어] AER/오류 복구용 채널 상태의 초기값 = 정상. 오류가 나면
	 * pci_channel_io_frozen 등으로 바뀌고, 드라이버의 err_handler 가
	 * 그 상태를 보고 대응한다(NVMe 도 err_handler 를 등록한다). */
	set_pcie_port_type(dev);
	/* [한국어] PCIe capability 를 찾아 pcie_cap/포트 타입/MPSS/ASPM 지원을
	 * 확정한다. 이후 pci_is_pcie() 가 의미를 갖는다. */

	err = pci_set_of_node(dev);
	/* [한국어] Device Tree 에서 이 BDF 에 대응하는 노드를 찾아 연결한다 */
	if (err)
		return err;
	/* [한국어] DT 파싱 오류 — 이 장치를 만들 수 없다 */
	pci_set_acpi_fwnode(dev);
	/* [한국어] ACPI 시스템이면 대응하는 ACPI 핸들을 연결한다.
	 * 전원 관리(_PS0/_PS3)와 핫플러그 정보가 여기서 온다. */

	pci_dev_assign_slot(dev);
	/* [한국어] 이 장치가 속한 물리 슬롯(struct pci_slot)이 이미 등록되어
	 * 있으면 연결한다. sysfs 의 slot 링크와 핫플러그에 쓰인다. */

	/*
	 * Assume 32-bit PCI; let 64-bit PCI cards (which are far rarer)
	 * set this higher, assuming the system even supports it.
	 */
	dev->dma_mask = 0xffffffff;
	/* [한국어] DMA 주소 마스크의 보수적 기본값 = 32비트.
	 * 64비트 DMA 가 가능한 장치는 드라이버가 dma_set_mask() 로 올린다.
	 * NVMe 드라이버도 probe 에서 64비트로 올린다. 기본값을 낮게 두는 이유는,
	 * 능력을 과대평가해 4GB 위 버퍼를 넘겨 주면 데이터가 조용히 깨지기
	 * 때문이다(반대로 과소평가하면 성능만 손해다). */

	/*
	 * Assume 64-bit addresses for MSI initially. Will be changed to 32-bit
	 * if MSI (rather than MSI-X) capability does not have
	 * PCI_MSI_FLAGS_64BIT. Can also be overridden by driver.
	 */
	dev->msi_addr_mask = DMA_BIT_MASK(64);
	/* [한국어] MSI 메시지를 쓸 주소의 폭. 여기서는 낙관적으로 64비트로 두고,
	 * MSI capability 에 64비트 지원 비트가 없으면 나중에 32비트로 낮춘다.
	 * MSI-X 는 언제나 64비트 주소를 지원하므로 그대로 유지된다 —
	 * NVMe 는 MSI-X 를 쓰므로 이 값이 유지된다. */

	dev_set_name(&dev->dev, "%04x:%02x:%02x.%d", pci_domain_nr(dev->bus),
		     dev->bus->number, PCI_SLOT(dev->devfn),
		     PCI_FUNC(dev->devfn));
	/* [한국어] 익숙한 "0000:01:00.0" 형태의 BDF 이름을 만든다.
	 * devfn 은 (device << 3) | function 으로 압축된 8비트라,
	 * PCI_SLOT 이 상위 5비트(장치 번호), PCI_FUNC 이 하위 3비트(함수 번호)를
	 * 뽑아낸다. lspci 와 sysfs 에 이 이름이 그대로 나온다. */

	class = pci_class(dev);
	/* [한국어] offset 0x08 의 클래스+리비전 */

	dev->revision = class & 0xff;
	/* [한국어] 하위 8비트가 Revision ID — 같은 칩의 판올림 구분 */
	dev->class = class >> 8;		    /* upper 3 bytes */
	/* [한국어] 상위 24비트가 Class Code. NVMe 라면 0x010802 가 된다.
	 * 이 값이 드라이버 매칭의 기준이다. */

	if (pci_early_dump)
		early_dump_pci_device(dev);
	/* [한국어] pci_early_dump 커널 파라미터가 켜져 있을 때만 config 덤프 */

	/* Need to have dev->class ready */
	dev->cfg_size = pci_cfg_space_size(dev);
	/* [한국어] 위 영어 주석대로 pci_cfg_space_size() 가 클래스 코드를
	 * 참조하므로(호스트 브리지 판정), dev->class 를 채운 뒤에 불러야 한다.
	 * 결과가 4096 이어야 확장 capability(AER/LTR 등)를 쓸 수 있다. */

	/* Need to have dev->cfg_size ready */
	set_pcie_thunderbolt(dev);
	/* [한국어] Thunderbolt VSEC 는 확장 영역에 있으므로 cfg_size 확정 후에
	 * 불러야 한다(영어 주석의 순서 요구). */

	set_pcie_cxl(dev);
	/* [한국어] CXL DVSEC 도 확장 영역에 있다. 상위로 재귀하며 링크 상태 갱신 */

	set_pcie_untrusted(dev);
	/* [한국어] 위 두 판정과 부모의 상태를 종합해 신뢰 여부를 정한다.
	 * IOMMU 정책과 ATS 허용에 영향을 준다. */

	if (pci_is_pcie(dev))
		dev->supported_speeds = pcie_get_supported_speeds(dev);
	/* [한국어] 이 장치가 지원하는 링크 속도 집합을 비트맵으로 캐시한다.
	 * 링크 속도 강등 진단(pcie_report_downtraining)과 sysfs 표시에 쓰인다. */

	/* "Unknown power state" */
	dev->current_state = PCI_UNKNOWN;
	/* [한국어] 전원 상태(D0~D3) 를 아직 확인하지 않았다는 표시.
	 * pci_pm_init() 이 PM capability 를 읽어 실제 상태로 갱신한다. */

	/* Early fixups, before probing the BARs */
	pci_fixup_device(pci_fixup_early, dev);
	/* [한국어] BAR 를 건드리기 전에 적용해야 하는 quirk 를 실행한다.
	 * 예: BAR 가 쓰레기 값인 장치에 non_compliant_bars 를 세우는 quirk.
	 * 이 시점에 해야 아래 pci_read_bases() 가 그 표시를 존중할 수 있다. */

	pci_set_removable(dev);
	/* [한국어] 사용자 제거 가능 여부를 sysfs 속성으로 표시 */

	pci_info(dev, "[%04x:%04x] type %02x class %#08x %s\n",
		 dev->vendor, dev->device, dev->hdr_type, dev->class,
		 pci_type_str(dev));
	/* [한국어] 열거 결과 한 줄 요약. NVMe SSD 라면
	 * "[144d:a80a] type 00 class 0x010802 PCIe Endpoint" 같은 형태가 된다.
	 * 부팅 로그에서 SSD 가 제대로 인식되었는지 확인하는 첫 단서다. */

	/* Device class may be changed after fixup */
	class = dev->class >> 8;
	/* [한국어] quirk 가 클래스를 바꿨을 수 있으므로 다시 읽어 온다.
	 * 8비트 더 내려 base class + sub-class 만 남긴다(prog-if 제외) —
	 * 아래 비교 상수 PCI_CLASS_BRIDGE_PCI 등이 그 형식이다. */

	if (dev->non_compliant_bars && !dev->mmio_always_on) {
		/* [한국어] quirk 가 "이 장치의 BAR 는 믿을 수 없다"고 표시했고,
		 * 디코딩을 꺼도 되는 장치인 경우 */
		pci_read_config_word(dev, PCI_COMMAND, &cmd);
		/* [한국어] 현재 Command 값 확인 */
		if (cmd & (PCI_COMMAND_IO | PCI_COMMAND_MEMORY)) {
			/* [한국어] 디코딩이 켜져 있다면 위험하다 — BAR 값이 쓰레기인데
			 * 그 주소를 디코딩하고 있으면 다른 장치의 트랜잭션을 가로챌 수 있다 */
			pci_info(dev, "device has non-compliant BARs; disabling IO/MEM decoding\n");
			cmd &= ~PCI_COMMAND_IO;
			/* [한국어] I/O 공간 응답 비활성화 */
			cmd &= ~PCI_COMMAND_MEMORY;
			/* [한국어] 메모리 공간 응답 비활성화 */
			pci_write_config_word(dev, PCI_COMMAND, cmd);
			/* [한국어] 반영. 이 장치는 이후 주소 디코딩을 하지 않는다 */
		}
	}

	dev->broken_intx_masking = pci_intx_mask_broken(dev);
	/* [한국어] INTx 차단 비트가 쓰기 가능한지 지금(아무도 장치를 쓰지 않을 때)
	 * 시험해 결과만 남긴다 */

	switch (dev->hdr_type) {		    /* header type */
		/* [한국어] ★여기서부터가 헤더 타입별 레이아웃 분기다★ */
	case PCI_HEADER_TYPE_NORMAL:		    /* standard header */
		/* [한국어] type 0 — 일반 장치. NVMe 컨트롤러가 여기로 온다 */
		if (class == PCI_CLASS_BRIDGE_PCI)
			goto bad;
		/* [한국어] 헤더는 일반 장치인데 클래스는 PCI 브리지라고 주장한다 =
		 * 모순. 브리지 레이아웃으로 읽을 수도, 클래스를 믿을 수도 없다. */
		pci_read_irq(dev);
		/* [한국어] 0x3c/0x3d 의 INTx 정보를 읽는다 */
		pci_read_bases(dev, PCI_STD_NUM_BARS, PCI_ROM_ADDRESS);
		/* [한국어] ★핵심★ BAR 6칸(0x10~0x27)과 ROM BAR(0x30)를 읽어
		 * dev->resource[] 를 채운다. NVMe 의 BAR0(대개 64비트 MEM BAR)이
		 * 여기서 크기와 타입이 확정되어 resource[0] 에 들어간다. */

		pci_subsystem_ids(dev, &dev->subsystem_vendor, &dev->subsystem_device);
		/* [한국어] type 0 은 0x2c/0x2e 에 서브시스템 ID 표준 필드가 있다 */

		/*
		 * Do the ugly legacy mode stuff here rather than broken chip
		 * quirk code. Legacy mode ATA controllers have fixed
		 * addresses. These are not always echoed in BAR0-3, and
		 * BAR0-3 in a few cases contain junk!
		 */
		if (class == PCI_CLASS_STORAGE_IDE) {
			/* [한국어] IDE 컨트롤러 전용 예외 처리. 레거시 모드로 동작하는
			 * IDE 는 BAR 대신 PC 시절부터 고정된 I/O 포트를 쓴다.
			 * 영어 주석대로 그 주소가 BAR 에 반영되지 않거나 아예 쓰레기가
			 * 들어 있는 칩이 있어, BAR 를 무시하고 고정 주소를 박아 넣는다. */
			u8 progif;
			/* [한국어] 클래스 코드의 프로그래밍 인터페이스 바이트(0x09).
			 * IDE 에서는 각 채널이 레거시 모드인지 네이티브 모드인지를 나타낸다. */
			pci_read_config_byte(dev, PCI_CLASS_PROG, &progif);
			/* [한국어] offset 0x09 */
			if ((progif & 1) == 0) {
				/* [한국어] bit0 == 0 → primary 채널이 레거시 모드.
				 * 즉 BAR0/BAR1 이 아니라 고정 포트를 쓴다. */
				region.start = 0x1F0;
				/* [한국어] PC 표준 primary IDE 명령 포트 시작 */
				region.end = 0x1F7;
				/* [한국어] 8바이트 범위(0x1F0~0x1F7) */
				res = &dev->resource[0];
				/* [한국어] BAR0 슬롯을 이 고정 포트로 덮어쓴다 */
				res->flags = LEGACY_IO_RESOURCE;
				/* [한국어] IORESOURCE_IO | IORESOURCE_PCI_FIXED —
				 * I/O 포트이며 재배치 불가 */
				pcibios_bus_to_resource(dev->bus, res, &region);
				/* [한국어] 버스 주소를 CPU 주소로 변환해 리소스에 기록 */
				pci_info(dev, "BAR 0 %pR: legacy IDE quirk\n",
					 res);
				/* [한국어] BAR 를 덮어썼다는 사실을 로그로 남긴다 */
				region.start = 0x3F6;
				/* [한국어] primary 채널의 제어 포트 */
				region.end = 0x3F6;
				/* [한국어] 1바이트뿐 */
				res = &dev->resource[1];
				/* [한국어] BAR1 슬롯 */
				res->flags = LEGACY_IO_RESOURCE;
				/* [한국어] 동일한 고정 I/O 플래그 */
				pcibios_bus_to_resource(dev->bus, res, &region);
				/* [한국어] 주소 변환 후 기록 */
				pci_info(dev, "BAR 1 %pR: legacy IDE quirk\n",
					 res);
				/* [한국어] 로그 */
			}
			if ((progif & 4) == 0) {
				/* [한국어] bit2 == 0 → secondary 채널이 레거시 모드.
				 * 두 채널이 독립적이라 각각 검사한다. */
				region.start = 0x170;
				/* [한국어] PC 표준 secondary IDE 명령 포트 */
				region.end = 0x177;
				/* [한국어] 8바이트 범위 */
				res = &dev->resource[2];
				/* [한국어] BAR2 슬롯 */
				res->flags = LEGACY_IO_RESOURCE;
				/* [한국어] 고정 I/O 리소스 */
				pcibios_bus_to_resource(dev->bus, res, &region);
				/* [한국어] 주소 변환 후 기록 */
				pci_info(dev, "BAR 2 %pR: legacy IDE quirk\n",
					 res);
				/* [한국어] 로그 */
				region.start = 0x376;
				/* [한국어] secondary 채널의 제어 포트 */
				region.end = 0x376;
				/* [한국어] 1바이트 */
				res = &dev->resource[3];
				/* [한국어] BAR3 슬롯 */
				res->flags = LEGACY_IO_RESOURCE;
				/* [한국어] 고정 I/O 리소스 */
				pcibios_bus_to_resource(dev->bus, res, &region);
				/* [한국어] 주소 변환 후 기록 */
				pci_info(dev, "BAR 3 %pR: legacy IDE quirk\n",
					 res);
				/* [한국어] 로그 */
			}
		}
		break;

	case PCI_HEADER_TYPE_BRIDGE:		    /* bridge header */
		/* [한국어] type 1 — PCI-to-PCI 브리지. 레이아웃이 완전히 다르다.
		 * 여기서는 클래스 정합성 검사를 하지 않는데, PCIe 포트가 스스로를
		 * 브리지 헤더로 표현하면서 클래스는 다양하게 보고하는 경우가 있기
		 * 때문이다. */
		/*
		 * The PCI-to-PCI bridge spec requires that subtractive
		 * decoding (i.e. transparent) bridge must have programming
		 * interface code of 0x01.
		 */
		pci_read_irq(dev);
		/* [한국어] 브리지도 자기 INTx 를 가질 수 있다 */
		dev->transparent = ((dev->class & 0xff) == 1);
		/* [한국어] 클래스 코드의 최하위 바이트 = prog-if.
		 * 위 영어 주석대로 값 0x01 이 subtractive decode(transparent)
		 * 브리지를 뜻한다. 그런 브리지는 자기 창 밖의 주소도 아래로
		 * 흘려보내므로, pci_read_bridge_bases() 가 부모 리소스를 상속시킨다. */
		pci_read_bases(dev, 2, PCI_ROM_ADDRESS1);
		/* [한국어] ★type 0 과의 차이★ BAR 는 2칸뿐이고(0x18 부터는 버스
		 * 번호가 차지한다), ROM BAR 는 0x30 이 아니라 0x38 에 있다. */
		pci_read_bridge_windows(dev);
		/* [한국어] I/O / MEM / prefetchable 창의 구현 여부를 시험 쓰기로
		 * 판정하고 범위를 읽는다. type 1 에만 존재하는 단계다. */
		set_pcie_hotplug_bridge(dev);
		/* [한국어] Slot Capabilities 를 보고 핫플러그 가능 포트인지 표시 */
		pos = pci_find_capability(dev, PCI_CAP_ID_SSVID);
		/* [한국어] ★type 0 과의 차이★ 브리지 헤더에는 서브시스템 ID 표준
		 * 필드가 없다. 대신 SSVID capability(ID 0x0d)가 있으면 거기서 읽는다. */
		if (pos) {
			/* [한국어] capability 가 존재할 때만 */
			pci_read_config_word(dev, pos + PCI_SSVID_VENDOR_ID, &dev->subsystem_vendor);
			/* [한국어] capability 시작 + 상대 오프셋 = 서브시스템 벤더 ID */
			pci_read_config_word(dev, pos + PCI_SSVID_DEVICE_ID, &dev->subsystem_device);
			/* [한국어] 서브시스템 장치 ID */
		}
		break;

	case PCI_HEADER_TYPE_CARDBUS:		    /* CardBus bridge header */
		/* [한국어] type 2 — CardBus 브리지(PCMCIA 후속). 노트북용 구식 규격 */
		if (class != PCI_CLASS_BRIDGE_CARDBUS)
			goto bad;
		/* [한국어] CardBus 헤더라면 클래스도 반드시 CardBus 브리지여야 한다 */
		pci_read_irq(dev);
		/* [한국어] INTx 정보 */
		pci_read_bases(dev, 1, 0);
		/* [한국어] ★세 타입 중 가장 적다★ BAR 1칸뿐이고 ROM BAR 는 없다
		 * (세 번째 인자 0 = ROM 읽지 않음). 나머지 공간은 CardBus 전용
		 * 창 레지스터들이 차지한다. */
		pci_read_config_word(dev, PCI_CB_SUBSYSTEM_VENDOR_ID, &dev->subsystem_vendor);
		/* [한국어] ★또 다른 자리★ CardBus 의 서브시스템 벤더 ID 는 0x40 */
		pci_read_config_word(dev, PCI_CB_SUBSYSTEM_ID, &dev->subsystem_device);
		/* [한국어] 서브시스템 장치 ID 는 0x42 */
		break;

	default:				    /* unknown header */
		/* [한국어] 스펙에 없는 헤더 타입(3~7). 레이아웃을 모르므로 어떤
		 * 필드도 안전하게 해석할 수 없다. */
		pci_err(dev, "unknown header type %02x, ignoring device\n",
			dev->hdr_type);
		/* [한국어] 오류 보고 */
		pci_release_of_node(dev);
		/* [한국어] 위에서 pci_set_of_node() 로 잡은 DT 참조를 되돌린다.
		 * 이 경로만 실패로 빠져나가므로 여기서만 해제가 필요하다. */
		return -EIO;
		/* [한국어] 호출자 pci_scan_device() 가 이 값을 보고 pci_dev 를
		 * 해제하고 장치를 없는 것으로 취급한다. */

	bad:
		/* [한국어] 헤더 타입과 클래스 코드가 모순인 경우의 공통 처리.
		 * switch 문 안의 레이블이라 위 case 들에서 goto 로 뛰어든다. */
		pci_err(dev, "ignoring class %#08x (doesn't match header type %02x)\n",
			dev->class, dev->hdr_type);
		/* [한국어] 어떤 모순인지 로그로 남긴다 */
		dev->class = PCI_CLASS_NOT_DEFINED << 8;
		/* [한국어] 클래스를 "정의되지 않음"으로 바꾼다. 장치를 버리지는
		 * 않는다 — 이렇게 하면 클래스 기반 드라이버 매칭에 걸리지 않아
		 * 잘못된 드라이버가 붙는 것을 막으면서, 장치 자체는 sysfs 에
		 * 보이게 되어 사용자가 상황을 확인할 수 있다. */
	}

	/* We found a fine healthy device, go go go... */
	return 0;
	/* [한국어] 성공. 호출자가 이어서 pci_device_add() 를 부른다 */
}

/*
 * [한국어]
 * pci_configure_mps - 장치의 Max Payload Size 를 상위 브리지와 맞춘다
 *
 * @dev: 대상 장치.
 * @return: 없음.
 *
 * MPS 란: PCIe 트랜잭션 하나가 실어 나를 수 있는 데이터의 최대 바이트 수
 * (128, 256, 512 … 4096). 클수록 헤더 오버헤드 비율이 줄어 대역폭 효율이
 * 좋아진다. NVMe 처럼 큰 DMA 전송을 하는 장치에서 체감 차이가 크다.
 *
 * 왜 맞춰야 하는가: PCIe 스펙상 한 계층 구조 안에서 MPS 는 일치해야 한다.
 * 어떤 장치가 상위 브리지보다 큰 MPS 로 TLP 를 보내면 브리지가 그것을
 * Malformed TLP 로 판정해 버린다. 그래서 커널은 기본적으로 자식의 MPS 를
 * 부모(상위 브리지) 값에 맞춘다.
 *
 * pcie_bus_config 정책(커널 파라미터 pci=pcie_bus_*):
 *   PCIE_BUS_TUNE_OFF  : 아무것도 건드리지 않고 불일치만 경고.
 *   PCIE_BUS_DEFAULT   : 이 함수가 부모 값에 맞춘다(아래 로직).
 *   PCIE_BUS_PEER2PEER : 모든 장치를 128 로 통일. 어떤 두 장치끼리도 직접
 *                        통신(peer-to-peer)할 수 있게 최소 공통값을 쓴다.
 *   그 밖(safe/performance) : 나중에 pcie_bus_configure_settings() 가 처리.
 *
 * 실행 컨텍스트: 프로세스 문맥, pci_configure_device() 안에서 호출.
 *
 * 호출 체인:
 *   pci_device_add() → pci_configure_device() → [pci_configure_mps]
 *     → pcie_get_mps()/pcie_set_mps()
 */
static void pci_configure_mps(struct pci_dev *dev)
{
	struct pci_dev *bridge = pci_upstream_bridge(dev);
	/* [한국어] 맞출 기준이 될 상위 브리지 */
	int mps, mpss, p_mps, rc;
	/* [한국어] mps   = 이 장치의 현재 MPS(바이트),
	 * mpss  = 이 장치가 지원하는 최대 MPS(바이트),
	 * p_mps = 상위 브리지의 현재 MPS,
	 * rc    = pcie_set_mps() 결과. */

	if (!pci_is_pcie(dev))
		return;
	/* [한국어] MPS 는 PCIe 개념이다. 구식 PCI 장치는 해당 없음 */

	/* MPS and MRRS fields are of type 'RsvdP' for VFs, short-circuit out */
	if (dev->is_virtfn)
		return;
	/* [한국어] SR-IOV VF 의 MPS/MRRS 필드는 예약(RsvdP)이라 쓸 수 없다.
	 * 실제 값은 PF 가 정하며 VF 는 그것을 따른다. */

	/*
	 * For Root Complex Integrated Endpoints, program the maximum
	 * supported value unless limited by the PCIE_BUS_PEER2PEER case.
	 */
	if (pci_pcie_type(dev) == PCI_EXP_TYPE_RC_END) {
		/* [한국어] Root Complex Integrated Endpoint 는 링크 없이 루트
		 * 컴플렉스에 내장된 장치라 "맞출 상위 브리지"가 없다.
		 * 그래서 맞추는 대신 자기가 낼 수 있는 최대값을 그냥 쓴다. */
		if (pcie_bus_config == PCIE_BUS_PEER2PEER)
			mps = 128;
		/* [한국어] peer-to-peer 정책에서는 모든 장치가 서로 통신할 수
		 * 있어야 하므로 최소 공통값 128 로 통일한다 */
		else
			mps = 128 << dev->pcie_mpss;
		/* [한국어] MPSS 는 지수 코드다(0=128B, 1=256B, 2=512B …).
		 * 128 을 그만큼 왼쪽으로 밀면 실제 바이트 수가 된다. */
		rc = pcie_set_mps(dev, mps);
		/* [한국어] Device Control 레지스터의 MPS 필드에 반영 */
		if (rc) {
			pci_warn(dev, "can't set Max Payload Size to %d; if necessary, use \"pci=pcie_bus_safe\" and report a bug\n",
				 mps);
			/* [한국어] 설정 실패 — 사용자에게 우회 방법(pcie_bus_safe)을
			 * 알리고 버그 보고를 요청한다. 치명적이지는 않다. */
		}
		return;
		/* [한국어] 내장 엔드포인트 처리 끝 */
	}

	if (!bridge || !pci_is_pcie(bridge))
		return;
	/* [한국어] 맞출 대상이 없거나 상위가 PCIe 가 아니면 할 일이 없다 */

	mps = pcie_get_mps(dev);
	/* [한국어] 이 장치의 현재 MPS */
	p_mps = pcie_get_mps(bridge);
	/* [한국어] 상위 브리지의 현재 MPS — 맞춰야 할 목표값 */

	if (mps == p_mps)
		return;
	/* [한국어] 이미 일치하면 건드릴 필요 없다(가장 흔한 경우) */

	if (pcie_bus_config == PCIE_BUS_TUNE_OFF) {
		/* [한국어] 사용자가 "손대지 마라"고 지시한 경우 */
		pci_warn(dev, "Max Payload Size %d, but upstream %s set to %d; if necessary, use \"pci=pcie_bus_safe\" and report a bug\n",
			 mps, pci_name(bridge), p_mps);
		/* [한국어] 불일치 사실만 알리고 고치지 않는다. 이 상태로는
		 * Malformed TLP 가 날 수 있음을 사용자가 알아야 한다. */
		return;
	}

	/*
	 * Fancier MPS configuration is done later by
	 * pcie_bus_configure_settings()
	 */
	if (pcie_bus_config != PCIE_BUS_DEFAULT)
		return;
	/* [한국어] safe/performance 같은 정교한 정책은 트리 전체를 훑어야
	 * 정할 수 있으므로, 열거가 끝난 뒤 pcie_bus_configure_settings() 가
	 * 따로 처리한다. 여기서는 기본 정책만 다룬다. */

	mpss = 128 << dev->pcie_mpss;
	/* [한국어] 이 장치가 지원하는 최대 MPS(바이트) */
	if (mpss < p_mps && pci_pcie_type(bridge) == PCI_EXP_TYPE_ROOT_PORT) {
		/* [한국어] 이 장치가 부모만큼 큰 MPS 를 낼 수 없는 경우.
		 * 부모가 Root Port 라면 그 아래에는 이 장치 하나뿐이므로
		 * (Root Port 는 하나의 링크만 가진다), 부모 쪽을 낮춰도
		 * 다른 장치에 피해가 없다. 그래서 부모를 낮춘다. */
		pcie_set_mps(bridge, mpss);
		/* [한국어] 부모의 MPS 를 자식이 감당 가능한 값으로 낮춘다 */
		pci_info(dev, "Upstream bridge's Max Payload Size set to %d (was %d, max %d)\n",
			 mpss, p_mps, 128 << bridge->pcie_mpss);
		/* [한국어] 부모를 건드렸다는 사실을 명시적으로 로그에 남긴다 */
		p_mps = pcie_get_mps(bridge);
		/* [한국어] 실제로 반영된 값을 다시 읽는다. 하드웨어가 요청한
		 * 값을 그대로 받아들이지 않을 수도 있기 때문이다. */
	}

	rc = pcie_set_mps(dev, p_mps);
	/* [한국어] 이 장치의 MPS 를 부모 값에 맞춘다 */
	if (rc) {
		pci_warn(dev, "can't set Max Payload Size to %d; if necessary, use \"pci=pcie_bus_safe\" and report a bug\n",
			 p_mps);
		/* [한국어] 설정 실패 — 불일치가 남지만 열거는 계속한다 */
		return;
	}

	pci_info(dev, "Max Payload Size set to %d (was %d, max %d)\n",
		 p_mps, mps, mpss);
	/* [한국어] 무엇을 무엇으로 바꿨고 최대 가능치는 얼마인지 남긴다.
	 * NVMe 성능이 기대에 못 미칠 때 MPS 가 128 로 묶여 있는지 확인하는
	 * 근거가 되는 로그다. */
}

/*
 * [한국어]
 * pci_configure_extended_tags - PCIe Extended Tag(8비트 태그)를 켜거나 끈다
 *
 * @dev: 대상 장치.
 * @ign: 무시되는 인자. 이 함수는 pci_walk_bus() 류의 콜백으로도 쓰이도록
 *       (struct pci_dev *, void *) 시그니처를 맞춰 두었다.
 * @return: 항상 0. 콜백 규약상 0 이 아니면 순회가 중단되므로, 어떤 실패가
 *          있어도 0 을 돌려 열거가 계속되게 한다.
 *
 * Tag 란: PCIe 에서 완료(completion)를 기다리는 읽기 요청 하나하나를
 * 구별하는 번호다. 기본은 5비트라 동시에 32개까지만 미완료(outstanding)
 * 요청을 가질 수 있다. Extended Tag 를 켜면 8비트가 되어 256개까지 늘어난다.
 *
 * 왜 중요한가: 지연이 큰 링크에서는 "동시에 얼마나 많은 요청을 띄울 수
 * 있는가"가 곧 대역폭 상한이다(Little's law). NVMe 처럼 큐 깊이를 깊게
 * 가져가는 장치는 태그가 32개로 묶이면 링크를 다 채우지 못한다.
 *
 * no_ext_tags quirk: 계층 어딘가에 8비트 태그를 제대로 처리하지 못하는
 * 장치가 있으면 호스트 브리지에 no_ext_tags 가 세워진다. 그 경우 성능보다
 * 정확성이 우선이므로 아래 모든 장치에서 Extended Tag 를 끈다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_device_add() → pci_configure_device() → [pci_configure_extended_tags]
 *     → pcie_capability_read/set/clear_word()
 */
int pci_configure_extended_tags(struct pci_dev *dev, void *ign)
{
	struct pci_host_bridge *host;
	/* [한국어] no_ext_tags 정책을 들고 있는 루트 호스트 브리지 */
	u32 cap;
	/* [한국어] Device Capabilities — Extended Tag 지원 여부가 들어 있다 */
	u16 ctl;
	/* [한국어] Device Control — Extended Tag 활성화 비트가 들어 있다 */
	int ret;
	/* [한국어] config 접근 결과 */

	/* PCI_EXP_DEVCTL_EXT_TAG is RsvdP in VFs */
	if (!pci_is_pcie(dev) || dev->is_virtfn)
		return 0;
	/* [한국어] PCIe 가 아니면 개념 자체가 없고, VF 는 이 비트가 예약이라
	 * 건드릴 수 없다(실제 값은 PF 가 정한다). */

	ret = pcie_capability_read_dword(dev, PCI_EXP_DEVCAP, &cap);
	if (ret)
		return 0;
	/* [한국어] 읽기 실패 — 조용히 포기한다. 여기서 오류를 올려도 할 수 있는
	 * 일이 없고, 열거를 멈추게 하면 손해가 더 크다. */

	if (!(cap & PCI_EXP_DEVCAP_EXT_TAG))
		return 0;
	/* [한국어] 이 장치가 8비트 태그를 지원하지 않는다 — 켤 수도 끌 수도 없다 */

	ret = pcie_capability_read_word(dev, PCI_EXP_DEVCTL, &ctl);
	if (ret)
		return 0;
	/* [한국어] 현재 활성화 상태를 못 읽었다 — 포기 */

	host = pci_find_host_bridge(dev->bus);
	if (!host)
		return 0;
	/* [한국어] 정책을 물어볼 곳이 없다 — 포기 */

	/*
	 * If some device in the hierarchy doesn't handle Extended Tags
	 * correctly, make sure they're disabled.
	 */
	if (host->no_ext_tags) {
		/* [한국어] 이 계층에 8비트 태그를 오해하는 장치가 있다는 표시.
		 * quirk 가 세워 준다. 성능보다 정확성이 우선이다. */
		if (ctl & PCI_EXP_DEVCTL_EXT_TAG) {
			/* [한국어] 지금 켜져 있으면(펌웨어가 켰거나) 꺼야 한다 */
			pci_info(dev, "disabling Extended Tags\n");
			pcie_capability_clear_word(dev, PCI_EXP_DEVCTL,
						   PCI_EXP_DEVCTL_EXT_TAG);
			/* [한국어] 읽기-수정-쓰기로 그 비트만 지운다.
			 * 다른 제어 비트(MPS, MRRS 등)는 그대로 유지된다. */
		}
		return 0;
		/* [한국어] 끄는 것이 목적이었으므로 여기서 끝 */
	}

	if (!(ctl & PCI_EXP_DEVCTL_EXT_TAG)) {
		/* [한국어] 지원하는데 꺼져 있으면 켜 준다 */
		pci_info(dev, "enabling Extended Tags\n");
		pcie_capability_set_word(dev, PCI_EXP_DEVCTL,
					 PCI_EXP_DEVCTL_EXT_TAG);
		/* [한국어] 그 비트만 세운다. 이제 이 장치는 최대 256개의 미완료
		 * 읽기 요청을 띄울 수 있다. */
	}
	return 0;
	/* [한국어] 콜백 규약상 항상 0 */
}

/*
 * [한국어]
 * pci_dev3_init - Device 3 확장 capability 를 읽어 flit 모드 여부를 기록한다
 *
 * @pdev: 대상 장치.
 * @return: 없음. pdev->fm_enabled 가 설정된다.
 *
 * Device 3 capability 란: 확장 capability ID 0x2F.
 * (include/uapi/linux/pci_regs.h:763 PCI_EXT_CAP_ID_DEV3)
 * 그 안의 Status 레지스터(오프셋 0x0c) bit3 이
 * PCI_DEV3_STA_SEGMENT 이며, 헤더의 영어 주석은 이 비트를
 * "Segment Captured (end-to-end flit-mode detected)" 라고 설명한다.
 *
 * 이 트리에서 확인되는 쓰임: pdev->fm_enabled 를 읽는 곳은
 * drivers/pci/ide.c:480 의 pci_ide_domain() 한 곳이며, 이 값이 참이면
 * pci_domain_nr(pdev->bus) 를, 거짓이면 0 을 IDE 스트림의 도메인 번호로
 * 쓴다. 그 밖의 용도는 이 트리에서 확인되지 않는다.
 *
 * 실행 컨텍스트: 프로세스 문맥, pci_init_capabilities() 안에서 호출.
 *
 * 호출 체인:
 *   pci_device_add() → pci_init_capabilities() → [pci_dev3_init]
 *     → pci_find_ext_capability()
 */
static void pci_dev3_init(struct pci_dev *pdev)
{
	u16 cap = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_DEV3);
	/* [한국어] 확장 capability 링크(0x100 부터 시작하는 별도 리스트)에서
	 * ID 0x2F 를 찾는다. dev->cfg_size 가 256 이면 즉시 0 을 돌려준다. */
	u32 val = 0;
	/* [한국어] Status 레지스터 값. 읽기가 실패해도 0 이 남아 아래 판정이
	 * 자연히 거짓이 되도록 0 으로 초기화한다. */

	if (!cap)
		return;
	/* [한국어] Device 3 capability 가 없는 장치 — 대부분이 여기에 해당 */
	pci_read_config_dword(pdev, cap + PCI_DEV3_STA, &val);
	/* [한국어] capability 시작 + 0x0c = Device 3 Status */
	pdev->fm_enabled = !!(val & PCI_DEV3_STA_SEGMENT);
	/* [한국어] bit3(값 0x8)을 뽑아 0/1 로 정규화해 저장한다.
	 * !! 는 비트 위치와 무관하게 참/거짓만 남기는 관용구다. */
}

/**
 * pcie_relaxed_ordering_enabled - Probe for PCIe relaxed ordering enable
 * @dev: PCI device to query
 *
 * Returns true if the device has enabled relaxed ordering attribute.
 */
/*
 * [한국어]
 * pcie_relaxed_ordering_enabled - Relaxed Ordering 이 켜져 있는지 알려 준다
 *
 * @dev: 조회할 장치.
 * @return: RO 가 활성화되어 있으면 true.
 *
 * Relaxed Ordering 이란: PCIe 는 기본적으로 같은 방향의 트랜잭션 순서를
 * 지킨다(strict ordering). RO 를 켜면 하드웨어가 순서를 바꿔 처리할 수 있어
 * 링크와 메모리 컨트롤러가 더 효율적으로 동작한다. 순서에 의존하지 않는
 * 데이터 전송(예: DMA 로 옮기는 데이터 블록)에서 이득이 있다.
 *
 * 왜 조회 함수가 따로 있나: 드라이버가 자기 DMA 설정을 결정할 때 현재 RO
 * 상태를 알아야 하는 경우가 있어 외부에 공개(EXPORT)되어 있다.
 *
 * 실행 컨텍스트: 어느 문맥에서나 config 읽기가 가능한 곳.
 *
 * 호출 체인:
 *   드라이버/quirk → [pcie_relaxed_ordering_enabled] → pcie_capability_read_word()
 */
bool pcie_relaxed_ordering_enabled(struct pci_dev *dev)
{
	u16 v;
	/* [한국어] Device Control 레지스터 값 */

	pcie_capability_read_word(dev, PCI_EXP_DEVCTL, &v);
	/* [한국어] PCIe capability 의 Device Control */

	return !!(v & PCI_EXP_DEVCTL_RELAX_EN);
	/* [한국어] Relaxed Ordering Enable 비트를 뽑아 bool 로 정규화 */
}
EXPORT_SYMBOL(pcie_relaxed_ordering_enabled);
/* [한국어] 드라이버 모듈에서 조회할 수 있도록 공개 */

/*
 * [한국어]
 * pci_configure_relaxed_ordering - Root Port 가 못 견디면 장치의 RO 를 끈다
 *
 * @dev: 대상 장치.
 * @return: 없음.
 *
 * 왜 필요한가: Relaxed Ordering 은 성능 최적화지만, 일부 Root Port 는 순서가
 * 뒤바뀐 트랜잭션을 제대로 처리하지 못해 데이터가 잘못 배치되는 결함이 있다.
 * 그런 Root Port 는 quirk 가 PCI_DEV_FLAGS_NO_RELAXED_ORDERING 을 세워 두고,
 * 이 함수가 그 아래 장치들의 RO 를 강제로 끈다. 성능보다 정확성이 우선인 판단이다.
 *
 * 왜 Root Port 만 보는가: 아래 영어 주석이 밝히듯, 지금은 Root Port 로 향하는
 * DMA 만 다룬다. 장치끼리 직접 주고받는 peer-to-peer DMA 는 경로가 달라
 * 별도의 문제이며 여기서 다루지 않는다.
 *
 * 실행 컨텍스트: 프로세스 문맥, pci_configure_device() 안에서 호출.
 *
 * 호출 체인:
 *   pci_device_add() → pci_configure_device() → [pci_configure_relaxed_ordering]
 *     → pcie_find_root_port(), pcie_capability_clear_word()
 */
static void pci_configure_relaxed_ordering(struct pci_dev *dev)
{
	struct pci_dev *root;
	/* [한국어] 이 장치의 DMA 가 최종적으로 도달하는 Root Port */

	/* PCI_EXP_DEVCTL_RELAX_EN is RsvdP in VFs */
	if (dev->is_virtfn)
		return;
	/* [한국어] VF 의 RO 비트는 예약(RsvdP)이라 쓸 수 없다.
	 * PF 의 설정이 VF 에도 적용된다. */

	if (!pcie_relaxed_ordering_enabled(dev))
		return;
	/* [한국어] 이미 꺼져 있으면 할 일이 없다.
	 * 이 함수는 켜는 일은 하지 않고 끄기만 한다. */

	/*
	 * For now, we only deal with Relaxed Ordering issues with Root
	 * Ports. Peer-to-Peer DMA is another can of worms.
	 */
	root = pcie_find_root_port(dev);
	/* [한국어] 트리를 거슬러 올라가 Root Port 를 찾는다 */
	if (!root)
		return;
	/* [한국어] 가상화 게스트 등에서는 Root Port 가 보이지 않을 수 있다.
	 * 그러면 판단 근거가 없으므로 손대지 않는다. */

	if (root->dev_flags & PCI_DEV_FLAGS_NO_RELAXED_ORDERING) {
		/* [한국어] 이 Root Port 는 RO 를 제대로 처리하지 못한다고
		 * quirk 가 표시해 두었다(drivers/pci/quirks.c 가 벤더/장치 ID 로 설정). */
		pcie_capability_clear_word(dev, PCI_EXP_DEVCTL,
					   PCI_EXP_DEVCTL_RELAX_EN);
		/* [한국어] 장치 쪽 RO Enable 비트를 끈다. 이제 이 장치는
		 * 순서를 지키는 트랜잭션만 보낸다 — 느리지만 안전하다. */
		pci_info(dev, "Relaxed Ordering disabled because the Root Port didn't support it\n");
		/* [한국어] 성능 저하의 이유를 진단할 수 있도록 로그로 남긴다 */
	}
}

/*
 * [한국어]
 * pci_configure_eetlp_prefix - End-to-End TLP Prefix 최대 개수를 기록한다
 *
 * @dev: 대상 장치.
 * @return: 없음. dev->eetlp_prefix_max 가 설정될 수 있다.
 *
 * EETLP Prefix 란: TLP(Transaction Layer Packet) 앞에 붙이는 추가 헤더로,
 * 종단 간(end-to-end)에 전달되는 부가 정보를 담는다. PASID(프로세스 주소
 * 공간 식별자) 같은 확장이 이 방식을 쓴다. 경로 중간의 스위치들이 이
 * prefix 를 그대로 전달할 수 있어야 하므로, "이 장치가 지원한다"만으로는
 * 부족하고 경로 전체가 지원해야 한다.
 *
 * 그래서 상위 브리지를 확인한다: Root Port 와 RC 내장 엔드포인트는 자기가
 * 경로의 끝이므로 자기 값을 그대로 쓰고, 그 밖의 장치는 상위 브리지가
 * prefix 를 지원할 때에만(bridge->eetlp_prefix_max 가 0 이 아닐 때) 자기
 * 값을 기록한다. 열거가 위에서 아래로 진행하므로 부모가 이미 처리되어 있다.
 *
 * 실행 컨텍스트: 프로세스 문맥, pci_configure_device() 안에서 호출.
 *
 * 호출 체인:
 *   pci_device_add() → pci_configure_device() → [pci_configure_eetlp_prefix]
 */
static void pci_configure_eetlp_prefix(struct pci_dev *dev)
{
	struct pci_dev *bridge;
	/* [한국어] 상위 브리지 — 경로가 prefix 를 전달할 수 있는지 확인용 */
	unsigned int eetlp_max;
	/* [한국어] 이 장치가 지원하는 prefix 최대 개수 */
	int pcie_type;
	/* [한국어] 이 장치의 PCIe 포트 종류 */
	u32 cap;
	/* [한국어] Device Capabilities 2 레지스터 값 */

	if (!pci_is_pcie(dev))
		return;
	/* [한국어] PCIe 전용 기능 */

	pcie_capability_read_dword(dev, PCI_EXP_DEVCAP2, &cap);
	/* [한국어] Device Capabilities 2 — PCIe 2.1 이후에 추가된 기능들이 모여 있다 */
	if (!(cap & PCI_EXP_DEVCAP2_EE_PREFIX))
		return;
	/* [한국어] 이 장치가 EETLP prefix 를 지원하지 않는다 */

	pcie_type = pci_pcie_type(dev);
	/* [한국어] Root Port/RC 내장 엔드포인트인지 판단하기 위해 종류를 얻는다 */

	eetlp_max = FIELD_GET(PCI_EXP_DEVCAP2_EE_PREFIX_MAX, cap);
	/* [한국어] 지원하는 최대 prefix 개수 필드(2비트) 추출 */
	/* 00b means 4 */
	eetlp_max = eetlp_max ?: 4;
	/* [한국어] 스펙상 인코딩이 특이하다. 1/2/3 은 그대로 1/2/3 개지만
	 * 0(00b)은 "0개"가 아니라 "4개"를 뜻한다. GCC 확장인 ?: 로
	 * "0 이면 4, 아니면 그대로"를 간결하게 쓴 것이다. */

	if (pcie_type == PCI_EXP_TYPE_ROOT_PORT ||
	    pcie_type == PCI_EXP_TYPE_RC_END)
		dev->eetlp_prefix_max = eetlp_max;
	/* [한국어] Root Port 와 RC 내장 엔드포인트는 경로의 시작점이라
	 * 위쪽 제약을 볼 필요가 없다. 자기 지원값을 그대로 기록한다. */
	else {
		/* [한국어] 그 밖의 장치는 위쪽 경로가 prefix 를 전달할 수 있어야 한다 */
		bridge = pci_upstream_bridge(dev);
		/* [한국어] 상위 브리지 획득 */
		if (bridge && bridge->eetlp_prefix_max)
			dev->eetlp_prefix_max = eetlp_max;
		/* [한국어] 부모가 prefix 를 지원할 때에만 기록한다.
		 * 열거는 위에서 아래로 내려오므로 부모의 값은 이미 확정되어 있다.
		 * 부모가 0 이면 경로가 끊긴 것이라 이 장치도 쓸 수 없다. */
	}
}

/*
 * [한국어]
 * pci_configure_serr - 브리지의 SERR# 포워딩을 켠다
 *
 * @dev: 대상 장치(브리지가 아니면 아무 일도 하지 않는다).
 * @return: 없음.
 *
 * 왜 필요한가: 아래 영어 주석대로, 브리지는 SERR# 포워딩이 꺼져 있으면
 * 아래쪽 엔드포인트가 보낸 오류 메시지(ERR_COR / ERR_NONFATAL / ERR_FATAL)를
 * 위로 전달하지 않는다. 그러면 오류가 Root Complex 에 도달하지 못해
 * AER 이 아무 것도 보고하지 못한다. 오류가 없는 것이 아니라 보이지 않게 되는
 * 것이라 더 위험하다.
 *
 * NVMe 접점: NVMe 컨트롤러에서 링크 오류가 나면 그 오류 메시지가 이 경로를
 * 타고 올라가야 AER 이 err_handler 를 호출하고 컨트롤러 리셋/복구가 시작된다.
 * 중간 브리지에서 이 비트가 꺼져 있으면 그 복구가 시작되지 않는다.
 *
 * 실행 컨텍스트: 프로세스 문맥, pci_configure_device() 안에서 호출.
 *
 * 호출 체인:
 *   pci_device_add() → pci_configure_device() → [pci_configure_serr]
 */
static void pci_configure_serr(struct pci_dev *dev)
{
	u16 control;
	/* [한국어] Bridge Control 레지스터(0x3e) 값 */

	if (dev->hdr_type == PCI_HEADER_TYPE_BRIDGE) {
		/* [한국어] Bridge Control 은 header type 1 에만 존재하는 레지스터다.
		 * 다른 타입에서 0x3e 를 건드리면 엉뚱한 필드를 망가뜨린다. */

		/*
		 * A bridge will not forward ERR_ messages coming from an
		 * endpoint unless SERR# forwarding is enabled.
		 */
		pci_read_config_word(dev, PCI_BRIDGE_CONTROL, &control);
		/* [한국어] 현재 값을 읽어 다른 비트를 보존한다 */
		if (!(control & PCI_BRIDGE_CTL_SERR)) {
			/* [한국어] 꺼져 있을 때만 쓴다 — 불필요한 config 쓰기를 아낀다 */
			control |= PCI_BRIDGE_CTL_SERR;
			/* [한국어] 값 0x02 — SERR# 포워딩 활성화 비트 */
			pci_write_config_word(dev, PCI_BRIDGE_CONTROL, control);
			/* [한국어] 반영. 이제 아래에서 온 오류 메시지가 위로 전달된다 */
		}
	}
}

/*
 * [한국어]
 * pci_configure_rcb - Read Completion Boundary 를 Root Port 와 일치시킨다
 *
 * @dev: 대상 장치.
 * @return: 없음.
 *
 * RCB 란: 메모리 읽기 요청 하나에 대한 응답(completion)을 여러 개로 쪼갤 때,
 * 그 경계를 어느 단위에 맞출지 정하는 값이다. 64바이트 또는 128바이트다.
 * 경로 양끝의 값이 다르면 completion 조각의 경계가 어긋나 비효율이 생긴다.
 *
 * 왜 Root Port 값을 따르는가: 아래 영어 주석이 인용하는 PCIe r7.0 sec 7.5.3.7
 * 에 따르면, Root Port 의 RCB 는 읽기 전용이고, 엔드포인트와 브리지는
 * Root Port 에 그 값이 설정되어 있을 때에만 자기 RCB 를 설정할 수 있다.
 * 즉 Root Port 가 기준이고 나머지가 따라가는 구조다.
 *
 * 실행 컨텍스트: 프로세스 문맥, pci_configure_device() 안에서 호출.
 *
 * 호출 체인:
 *   pci_device_add() → pci_configure_device() → [pci_configure_rcb]
 *     → pcie_find_root_port(), pcie_capability_clear_and_set_word()
 */
static void pci_configure_rcb(struct pci_dev *dev)
{
	struct pci_dev *rp;
	/* [한국어] 기준이 될 Root Port */
	u16 rp_lnkctl;
	/* [한국어] 그 Root Port 의 Link Control 값 */

	/*
	 * Per PCIe r7.0, sec 7.5.3.7, RCB is only meaningful in Root Ports
	 * (where it is read-only), Endpoints, and Bridges.  It may only be
	 * set for Endpoints and Bridges if it is set in the Root Port. For
	 * Endpoints, it is 'RsvdP' for Virtual Functions.
	 */
	if (!pci_is_pcie(dev) ||
	    pci_pcie_type(dev) == PCI_EXP_TYPE_ROOT_PORT ||
	    pci_pcie_type(dev) == PCI_EXP_TYPE_UPSTREAM ||
	    pci_pcie_type(dev) == PCI_EXP_TYPE_DOWNSTREAM ||
	    pci_pcie_type(dev) == PCI_EXP_TYPE_RC_EC ||
	    dev->is_virtfn)
		return;
	/* [한국어] 설정 대상이 아닌 경우를 한꺼번에 걸러 낸다.
	 * - PCIe 가 아니면 RCB 개념이 없다.
	 * - Root Port 는 자기 RCB 가 읽기 전용이라 쓸 수 없다(기준일 뿐).
	 * - 스위치 상/하향 포트와 RC Event Collector 는 위 스펙 인용에서
	 *   설정 대상으로 열거되지 않았다.
	 * - VF 는 이 비트가 예약(RsvdP)이다.
	 * 남는 것은 엔드포인트와 (PCIe-to-PCI 등) 브리지다. NVMe SSD 는
	 * 엔드포인트이므로 여기를 통과한다. */

	/* Root Port often not visible to virtualized guests */
	rp = pcie_find_root_port(dev);
	/* [한국어] 기준값을 읽어 올 Root Port 를 찾는다 */
	if (!rp)
		return;
	/* [한국어] 위 영어 주석대로 가상화 게스트에서는 Root Port 가 보이지
	 * 않는 경우가 흔하다. 기준이 없으면 설정하지 않는다. */

	pcie_capability_read_word(rp, PCI_EXP_LNKCTL, &rp_lnkctl);
	/* [한국어] Root Port 의 Link Control — RCB 비트가 여기 있다 */
	pcie_capability_clear_and_set_word(dev, PCI_EXP_LNKCTL,
					   PCI_EXP_LNKCTL_RCB,
					   (rp_lnkctl & PCI_EXP_LNKCTL_RCB) ?
					   PCI_EXP_LNKCTL_RCB : 0);
	/* [한국어] clear_and_set 은 "지정한 비트를 먼저 지우고, 지정한 값으로
	 * 다시 세운다"를 한 번의 읽기-수정-쓰기로 처리한다. 세 번째 인자가
	 * 지울 마스크, 네 번째가 세울 값이다.
	 * 삼항 연산은 "Root Port 에 RCB 가 서 있으면 나도 세우고, 아니면 지운다"
	 * 즉 Root Port 값을 그대로 복사하는 것이다. 다른 Link Control 비트
	 * (ASPM 설정 등)는 건드리지 않는다. */
}

/*
 * [한국어]
 * pci_configure_device - 새로 발견한 장치에 대한 링크/전송 계층 설정을 모아 실행
 *
 * @dev: 방금 열거된 장치.
 * @return: 없음.
 *
 * 왜 필요한가: pci_setup_device() 가 "장치가 무엇인지" 읽어 냈다면, 이
 * 함수는 "그 장치가 이 시스템에서 어떻게 동작해야 하는지"를 써 넣는다.
 * 드라이버가 붙기 전에 끝나야 하므로 pci_device_add() 안에서, device_add()
 * 보다 앞서 호출된다.
 *
 * 순서의 의미: MPS/Extended Tag 처럼 성능에 관한 것부터, RO/SERR/RCB 처럼
 * 정확성과 오류 보고에 관한 것까지 차례로 적용하고, 마지막에 ACPI 펌웨어가
 * 지정한 핫플러그 파라미터를 덮어쓴다.
 *
 * NVMe 접점: NVMe SSD 가 나타나면 이 함수가 그 SSD 의 MPS, Extended Tag,
 * LTR, ASPM L1 substates 를 확정한다. 이 설정들이 이후 SSD 의 실효 대역폭과
 * 유휴 시 절전 동작을 좌우한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 드라이버 바인딩 전.
 *
 * 호출 체인:
 *   pci_device_add() → [pci_configure_device] → 각 pci_configure_* 함수
 */
static void pci_configure_device(struct pci_dev *dev)
{
	pci_configure_mps(dev);
	/* [한국어] 최대 페이로드 크기를 상위 브리지와 맞춘다(대역폭 효율) */
	pci_configure_extended_tags(dev, NULL);
	/* [한국어] 8비트 태그를 켜 미완료 요청 수를 32 → 256 으로 늘린다.
	 * 두 번째 인자는 콜백 시그니처를 맞추기 위한 더미다. */
	pci_configure_relaxed_ordering(dev);
	/* [한국어] Root Port 가 RO 를 못 견디면 장치 쪽 RO 를 끈다(정확성) */
	pci_configure_ltr(dev);
	/* [한국어] LTR(Latency Tolerance Reporting) 설정. 장치가 "이만큼의
	 * 지연은 견딘다"고 플랫폼에 알려, 플랫폼이 더 깊은 절전으로 갈 수
	 * 있게 한다. drivers/pci/pci.c 에 구현되어 있다. */
	pci_configure_aspm_l1ss(dev);
	/* [한국어] ASPM L1 Substates(L1.1/L1.2) 설정. L1 보다 더 깊은 링크
	 * 절전 상태로, 복귀 지연이 더 크다. drivers/pci/pcie/aspm.c 소관.
	 * NVMe 의 APST(컨트롤러 자체 전력 상태)와는 별개의 층위다. */
	pci_configure_eetlp_prefix(dev);
	/* [한국어] End-to-End TLP Prefix 최대 개수를 경로 제약과 함께 기록 */
	pci_configure_serr(dev);
	/* [한국어] 브리지면 SERR# 포워딩을 켜 오류 메시지가 위로 전달되게 한다 */
	pci_configure_rcb(dev);
	/* [한국어] Read Completion Boundary 를 Root Port 값에 맞춘다 */

	pci_acpi_program_hp_params(dev);
	/* [한국어] 마지막으로 ACPI 펌웨어가 _DSM 으로 지정한 핫플러그 파라미터를
	 * 적용한다. 위에서 커널이 정한 값을 펌웨어가 덮어쓸 수 있으므로 맨 끝이다.
	 * CONFIG_ACPI 가 없으면 빈 함수다. */
}

/*
 * [한국어]
 * pci_release_capabilities - 장치가 해제될 때 각 capability 하위 시스템을 정리한다
 *
 * @dev: 해제 중인 장치.
 * @return: 없음.
 *
 * 왜 필요한가: pci_init_capabilities() 가 장치 등록 시 여러 하위 시스템의
 * 초기화 훅을 불렀다면, 그 짝이 되는 정리 훅을 여기서 부른다. 이것을
 * 빼먹으면 장치를 뽑았다 꽂을 때마다 메모리와 등록 항목이 쌓인다.
 *
 * NVMe 접점: NVMe SSD 를 핫플러그로 뽑거나 드라이버를 언로드할 때, 이
 * 함수가 그 장치의 AER 등록과 SR-IOV 자원을 정리한다. MSI-X 벡터는 여기가
 * 아니라 드라이버가 pci_free_irq_vectors() 로 따로 반납한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, pci_release_dev() 안(참조 카운트 0 시점).
 *
 * 호출 체인:
 *   put_device(&dev->dev) → pci_release_dev() → [pci_release_capabilities]
 */
static void pci_release_capabilities(struct pci_dev *dev)
{
	pci_aer_exit(dev);
	/* [한국어] AER 관련 상태와 등록을 해제한다. 이후 이 장치의 PCIe 오류는
	 * 커널이 보고하지 않는다. */
	pci_rcec_exit(dev);
	/* [한국어] Root Complex Event Collector 와의 연결을 끊는다.
	 * RCEC 는 내장 엔드포인트들의 오류를 모아 보고하는 장치라,
	 * 그 목록에서 이 장치를 빼야 한다. */
	pci_iov_release(dev);
	/* [한국어] SR-IOV 자원을 반납한다. PF 라면 VF 관련 상태가 정리된다 */
	pci_free_cap_save_buffers(dev);
	/* [한국어] suspend 시 capability 레지스터 내용을 저장해 두던 버퍼를
	 * 해제한다. 절전 복귀 시 하드웨어가 초기화되므로 커널이 값을 기억해
	 * 두었다가 되쓰는데, 그 버퍼다. */
}

/**
 * pci_release_dev - Free a PCI device structure when all users of it are
 *		     finished
 * @dev: device that's been disconnected
 *
 * Will be called only by the device core when all users of this PCI device are
 * done.
 */
/*
 * [한국어]
 * pci_release_dev - pci_dev 의 마지막 참조가 사라졌을 때의 소멸자
 *
 * @dev: pci_dev 안에 박혀 있는 struct device.
 * @return: 없음.
 *
 * 왜 필요한가: pci_dev 는 refcount 로 관리되므로 아무도 임의로 kfree 하면
 * 안 된다. 커널이 참조가 0 이 되는 순간 device_type->release 로 이 함수를
 * 부르고, 여기서만 실제 해제가 일어난다.
 *
 * 해제 순서가 중요하다: 하위 시스템 정리 → 외부 참조 반납 → 메모리 해제.
 * 순서를 바꾸면 이미 해제된 필드를 참조하게 된다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 마지막 put_device() 호출자 문맥에서 동기 실행.
 *
 * 호출 체인:
 *   put_device(&pdev->dev) → device_release() → [pci_release_dev] → kfree
 */
static void pci_release_dev(struct device *dev)
{
	struct pci_dev *pci_dev;
	/* [한국어] 내장 device 에서 복원한 바깥 pci_dev */

	pci_dev = to_pci_dev(dev);
	/* [한국어] container_of 기반 역변환 */
	pci_release_capabilities(pci_dev);
	/* [한국어] AER/RCEC/SR-IOV/저장 버퍼 등 하위 시스템 정리 */
	pci_release_of_node(pci_dev);
	/* [한국어] pci_set_of_node() 로 잡은 DT 노드 참조 반납 */
	pcibios_release_device(pci_dev);
	/* [한국어] 아키텍처별 정리 훅(기본은 빈 __weak 함수) */
	pci_bus_put(pci_dev->bus);
	/* [한국어] pci_alloc_dev() 의 pci_bus_get() 과 짝. 버스 참조를 놓는다.
	 * 이 장치가 버스의 마지막 사용자였다면 버스도 해제될 수 있다. */
	bitmap_free(pci_dev->dma_alias_mask);
	/* [한국어] DMA alias 비트맵 해제. 일부 장치는 자기 것이 아닌 다른
	 * BDF 로 DMA 요청을 보내는데(브리지 뒤의 구식 장치 등), IOMMU 가
	 * 그 별칭들도 함께 매핑해야 해서 커널이 비트맵으로 기억해 둔 것이다.
	 * NULL 이어도 안전하다. */
	dev_dbg(dev, "device released\n");
	/* [한국어] 해제 완료 디버그 로그. kfree 직전이라 dev 를 쓸 수 있는 마지막 시점 */
	kfree(pci_dev);
	/* [한국어] 구조체 자체 해제. 이후 어떤 필드도 접근 불가 */
}

/*
 * [한국어] pci_dev_type - PCI 장치 device 의 타입 서술자.
 * release 콜백을 여기 두지 않는 것에 유의 — pci_dev 의 소멸자
 * pci_release_dev() 는 pci_device_add() 에서 dev->dev.release 에 직접
 * 대입되며, 이 device_type 은 sysfs 속성 그룹만 제공한다.
 */
static const struct device_type pci_dev_type = {
	.groups = pci_dev_attr_groups,
	/* [한국어] 모든 PCI 장치에 자동 생성되는 sysfs 속성 그룹들
	 * (drivers/pci/pci-sysfs.c: vendor, device, class, resource,
	 *  config, enable, numa_node 등). */
};

/*
 * [한국어]
 * pci_alloc_dev - 빈 pci_dev 를 할당하고 최소한의 초기화를 한다
 *
 * @bus: 이 장치가 붙을 버스.
 * @return: 0 으로 초기화되고 락과 버스 참조가 준비된 pci_dev. 실패 시 NULL.
 *
 * 왜 필요한가: 장치를 발견했을 때(pci_scan_device)와 SR-IOV VF 를 만들 때
 * (drivers/pci/iov.c) 모두 이 함수를 쓴다. 공통 초기화를 한곳에 모아 둔 것이며,
 * 이 시점에는 아직 vendor/device 도 채워지지 않은 빈 껍데기다.
 *
 * 실행 컨텍스트: 프로세스 문맥(GFP_KERNEL).
 *
 * 호출 체인:
 *   pci_scan_device()/pci_iov_add_virtfn() → [pci_alloc_dev] → kzalloc_obj
 */
struct pci_dev *pci_alloc_dev(struct pci_bus *bus)
{
	struct pci_dev *dev;
	/* [한국어] 만들 장치 객체 */

	dev = kzalloc_obj(struct pci_dev);
	/* [한국어] 0 초기화 할당. 명시적으로 채우지 않는 필드는 모두 0/NULL 이다 */
	if (!dev)
		return NULL;
	/* [한국어] 메모리 부족 */

	INIT_LIST_HEAD(&dev->bus_list);
	/* [한국어] 이 장치를 버스의 devices 목록에 매달 링크.
	 * 실제 연결은 pci_device_add() 가 한다. */
	dev->dev.type = &pci_dev_type;
	/* [한국어] sysfs 속성 그룹을 제공할 device_type 지정 */
	dev->bus = pci_bus_get(bus);
	/* [한국어] 소속 버스를 가리키면서 참조를 하나 잡는다.
	 * 이 장치가 살아 있는 동안 버스가 사라지지 않도록 보장한다.
	 * pci_release_dev() 의 pci_bus_put() 과 짝이다. */
	dev->driver_exclusive_resource = (struct resource) {
		/* [한국어] 드라이버가 "이 영역은 나만 쓰겠다"고 예약할 때 부모로
		 * 쓰이는 리소스. 복합 리터럴로 한 번에 초기화한다. */
		.name = "PCI Exclusive",
		/* [한국어] /proc/iomem 등에서 보일 이름 */
		.start = 0,
		/* [한국어] 시작을 0 으로, */
		.end = -1,
		/* [한국어] 끝을 -1(= 부호 없는 최대값)로 두어 주소 공간 전체를
		 * 덮는 열린 범위로 만든다. 실제 제한은 자식 리소스가 가진다. */
	};

	spin_lock_init(&dev->pcie_cap_lock);
	/* [한국어] PCIe capability 레지스터의 읽기-수정-쓰기를 보호하는 락.
	 * pcie_capability_clear_and_set_word() 같은 함수가 이 락을 잡아,
	 * 두 CPU 가 동시에 같은 레지스터를 고쳐 한쪽 변경이 사라지는 것을 막는다. */
#ifdef CONFIG_PCI_MSI
	raw_spin_lock_init(&dev->msi_lock);
	/* [한국어] MSI/MSI-X 마스킹 비트를 보호하는 락. raw_spinlock 인 이유는
	 * 인터럽트 마스킹 경로가 RT 커널에서도 잠들면 안 되기 때문이다
	 * (일반 spinlock 은 PREEMPT_RT 에서 잠들 수 있는 뮤텍스가 된다).
	 * NVMe 처럼 큐마다 벡터를 갖는 장치에서 자주 쓰인다. */
#endif
	return dev;
	/* [한국어] 호출자가 devfn/vendor/device 를 채우고 pci_setup_device() 로 넘긴다 */
}
EXPORT_SYMBOL(pci_alloc_dev);
/* [한국어] 일부 컨트롤러/가상화 코드가 모듈에서 쓰므로 공개 */

/*
 * [한국어]
 * pci_bus_wait_rrs - 장치가 "준비 중"이라고 답하는 동안 기다린다
 *
 * @bus:     대상 버스.
 * @devfn:   대상 (device, function) 좌표.
 * @l:       입출력. 현재 읽은 Vendor/Device ID dword 이며, 재시도 성공 시
 *           유효한 값으로 갱신된다.
 * @timeout: 최대 대기 시간(ms). 0 이면 기다리지 않는다.
 * @return: 유효한 Vendor ID 를 얻었으면 true. 시간 초과나 읽기 실패면 false.
 *
 * RRS 란: Configuration Request Retry Status(예전 이름 CRS). 리셋이나 전원
 * 인가 직후의 장치는 config 요청에 답할 준비가 되지 않아 "나중에 다시
 * 요청하라"고 응답한다. Root Port 가 RRS Software Visibility 를 켜 두었으면
 * (pci_enable_rrs_sv 참조) 그 상태가 Vendor ID 0x0001 이라는 예약값으로
 * 커널에 보인다. drivers/pci/pci.h 의 pci_bus_rrs_vendor_id() 가 그 판정을 한다.
 *
 * 왜 필요한가: 이것이 없으면 커널은 아직 준비 중인 장치를 "없는 장치"로
 * 오해하고 지나쳐 버린다. 초기화에 시간이 걸리는 장치(전원이 막 들어온
 * NVMe SSD 등)가 열거에서 통째로 누락될 수 있다.
 *
 * 지수 백오프: 1ms 부터 시작해 매번 두 배로 늘린다. 대부분의 장치는 몇 ms 만에
 * 준비되므로 빠르게 잡아내고, 오래 걸리는 장치에 대해서는 config 읽기 횟수를
 * 로그 스케일로 억제한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. msleep() 으로 잠들므로 인터럽트 문맥 금지.
 *
 * 호출 체인:
 *   pci_bus_generic_read_dev_vendor_id() → [pci_bus_wait_rrs]
 *     → msleep(), pci_bus_read_config_dword()
 */
static bool pci_bus_wait_rrs(struct pci_bus *bus, int devfn, u32 *l,
			     int timeout)
{
	int delay = 1;
	/* [한국어] 다음에 기다릴 시간(ms). 1 → 2 → 4 → … 로 두 배씩 늘어난다.
	 * 로그에 찍는 "누적 경과 시간"은 delay - 1 인데, 1+2+4+…+2^(n-1) = 2^n - 1
	 * 이므로 현재 delay 에서 1 을 뺀 값이 지금까지 잔 총 시간이 된다. */

	if (!pci_bus_rrs_vendor_id(*l))
		return true;	/* not a Configuration RRS completion */
	/* [한국어] 애초에 RRS 표식(Vendor ID 0x0001)이 아니었다 — 이미 유효한
	 * 값이므로 기다릴 필요 없이 성공이다. */

	if (!timeout)
		return false;	/* RRS, but caller doesn't want to wait */
	/* [한국어] RRS 이긴 한데 호출자가 대기를 원하지 않는다(timeout 0).
	 * 예를 들어 빠른 존재 확인만 하려는 경로가 그렇다. */

	/*
	 * We got the reserved Vendor ID that indicates a completion with
	 * Configuration Request Retry Status (RRS).  Retry until we get a
	 * valid Vendor ID or we time out.
	 */
	while (pci_bus_rrs_vendor_id(*l)) {
		/* [한국어] 여전히 RRS 인 동안 반복 */
		if (delay > timeout) {
			/* [한국어] 다음 대기 시간이 남은 예산을 넘었다 = 시간 초과 */
			pr_warn("pci %04x:%02x:%02x.%d: not ready after %dms; giving up\n",
				pci_domain_nr(bus), bus->number,
				PCI_SLOT(devfn), PCI_FUNC(devfn), delay - 1);
			/* [한국어] 어느 BDF 가 얼마나 기다린 끝에 포기되었는지 남긴다.
			 * pci_dev 가 아직 없으므로 pci_warn 이 아니라 pr_warn 을 쓴다. */

			return false;
			/* [한국어] 호출자는 이 자리에 장치가 없는 것으로 처리한다 */
		}
		if (delay >= 1000)
			pr_info("pci %04x:%02x:%02x.%d: not ready after %dms; waiting\n",
				pci_domain_nr(bus), bus->number,
				PCI_SLOT(devfn), PCI_FUNC(devfn), delay - 1);
		/* [한국어] 1초 이상 걸리기 시작하면 사용자에게 알린다. 부팅이
		 * 멈춘 것처럼 보이는 상황의 원인을 로그로 설명해 주는 것이다. */

		msleep(delay);
		/* [한국어] 잠든다. 이 함수가 프로세스 문맥 전용인 이유다 */
		delay *= 2;
		/* [한국어] 지수 백오프 — 다음 대기는 두 배 */

		if (pci_bus_read_config_dword(bus, devfn, PCI_VENDOR_ID, l))
			return false;
		/* [한국어] 다시 읽는다. 0 이 아닌 반환값은 config 접근 자체의
		 * 실패를 뜻하므로 더 기다릴 의미가 없다. 성공하면 갱신된 *l 로
		 * 루프 조건을 다시 판정한다. */
	}

	if (delay >= 1000)
		pr_info("pci %04x:%02x:%02x.%d: ready after %dms\n",
			pci_domain_nr(bus), bus->number,
			PCI_SLOT(devfn), PCI_FUNC(devfn), delay - 1);
	/* [한국어] 오래 기다렸던 장치가 마침내 응답했다는 사실을 남긴다.
	 * 위의 "waiting" 로그와 짝을 이룬다. */

	return true;
	/* [한국어] 이제 *l 에는 유효한 Vendor/Device ID 가 들어 있다 */
}

/*
 * [한국어]
 * pci_bus_generic_read_dev_vendor_id - 어떤 좌표에 장치가 있는지 판정한다
 *
 * @bus:     대상 버스.
 * @devfn:   대상 (device, function) 좌표.
 * @l:       읽은 Vendor/Device ID dword 를 받을 곳.
 * @timeout: RRS 상태일 때 기다릴 최대 ms.
 * @return: 그 자리에 장치가 있으면 true, 없으면 false.
 *
 * ★ 이 함수가 PCI 열거의 근본 원리를 구현한다 ★
 * 시스템에는 "어떤 장치가 어디에 있다"는 명부가 없다. 그래서 커널은 가능한
 * 모든 좌표에 대해 config 오프셋 0x00(Vendor/Device ID)을 실제로 읽어 보고,
 * 응답이 오는지로 존재를 판정한다.
 *
 * 왜 0xFFFF 가 "없음"인가: config 요청의 목적지에 아무도 없으면 응답이
 * 오지 않는다. 그때 호스트 브리지/Root Complex 는 마스터 어보트를 감지하고,
 * 읽기 결과로 모든 비트가 1 인 값(0xFFFFFFFF)을 CPU 에 돌려준다. 이것은
 * 버스가 아무도 구동하지 않을 때 풀업 저항 때문에 모두 1 로 읽히는
 * 물리적 성질에서 유래한 규약이며, PCI 스펙은 유효한 Vendor ID 로 0xFFFF 를
 * 쓰지 못하도록 예약해 두어 이 판정이 모호해지지 않게 했다.
 * 같은 원리가 이 파일 곳곳에서 쓰인다 — __pci_read_base() 의
 * PCI_POSSIBLE_ERROR(sz), capability 순회에서 ID 0xff 를 만나면 중단하는 것,
 * 그리고 drivers/nvme/host/pci.c:3118 에서
 * readl(dev->bar + NVME_REG_CSTS) == -1 로 컨트롤러가 사라졌는지 보는 것까지
 * 모두 "all-ones = 응답 없음"이라는 같은 규약이다.
 *
 * 0x0000 도 걸러 내는 이유: 아래 영어 주석대로, 빈 슬롯에 대해 0xFFFFFFFF 가
 * 아니라 0 을 돌려주는 결함 보드가 있다. Vendor ID 0x0000 역시 유효한 값이
 * 아니므로 함께 없는 것으로 처리한다. 0x0000ffff/0xffff0000 은 상위 또는
 * 하위 절반만 제대로 읽힌 어중간한 응답으로, 역시 신뢰할 수 없다.
 *
 * NVMe 접점: 부팅 시 NVMe SSD 가 발견되는 최초의 순간이 바로 이 함수가
 * 그 SSD 의 Vendor ID(예: 삼성 0x144d)를 읽어 내는 시점이다.
 *
 * 실행 컨텍스트: 프로세스 문맥(RRS 대기에서 잠들 수 있다).
 *
 * 호출 체인:
 *   pci_scan_device() → pci_bus_read_dev_vendor_id()
 *     → [pci_bus_generic_read_dev_vendor_id] → pci_bus_wait_rrs()
 */
bool pci_bus_generic_read_dev_vendor_id(struct pci_bus *bus, int devfn, u32 *l,
					int timeout)
{
	if (pci_bus_read_config_dword(bus, devfn, PCI_VENDOR_ID, l))
		return false;
	/* [한국어] config 오프셋 0x00 을 dword 로 읽는다. 하위 16비트가
	 * Vendor ID, 상위 16비트가 Device ID 라 한 번에 둘 다 얻는다.
	 * 0 이 아닌 반환값은 버스 접근 자체가 실패했다는 뜻(해당 버스 번호가
	 * 유효하지 않거나 컨트롤러가 거부)이므로 장치 없음으로 처리한다. */

	/* Some broken boards return 0 or ~0 (PCI_ERROR_RESPONSE) if a slot is empty: */
	if (PCI_POSSIBLE_ERROR(*l) || *l == 0x00000000 ||
	    *l == 0x0000ffff || *l == 0xffff0000)
		return false;
	/* [한국어] 네 가지 "장치 없음" 패턴을 한꺼번에 거른다.
	 *  0xFFFFFFFF : 표준적인 응답 없음(마스터 어보트). PCI_POSSIBLE_ERROR 가 판정.
	 *  0x00000000 : 영어 주석이 말하는 결함 보드의 빈 슬롯 응답.
	 *  0x0000ffff : Vendor ID 만 0xffff — 절반만 응답 없음.
	 *  0xffff0000 : Device ID 만 0xffff — 역시 절반만 응답 없음.
	 * 어느 쪽이든 유효한 장치일 수 없으므로 이 좌표는 비어 있다고 본다. */

	if (pci_bus_rrs_vendor_id(*l))
		return pci_bus_wait_rrs(bus, devfn, l, timeout);
	/* [한국어] Vendor ID 가 예약값 0x0001 이면 "장치는 있는데 아직 준비
	 * 중"이라는 뜻이다(RRS). 없는 것으로 처리하면 안 되므로 기다린다. */

	return true;
	/* [한국어] 유효한 Vendor/Device ID 를 얻었다 = 이 좌표에 장치가 있다.
	 * 호출자가 *l 에서 vendor 와 device 를 뽑아 pci_dev 에 채운다. */
}

/*
 * [한국어]
 * pci_bus_read_dev_vendor_id - 장치 존재 판정의 공개 진입점
 *
 * @bus:     대상 버스.
 * @devfn:   대상 좌표.
 * @l:       읽은 Vendor/Device ID 를 받을 곳.
 * @timeout: RRS 대기 최대 ms.
 * @return: 장치가 있으면 true.
 *
 * 왜 한 겹 더 감싸는가: 아키텍처나 가상화 계층이 존재 판정을 가로채야 할
 * 때를 대비한 분리 지점이다. 현재 이 구현은 generic 버전을 그대로 부른다.
 * 외부 모듈(핫플러그 드라이버 등)이 쓰는 공개 API 이기도 하다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_scan_device() → [pci_bus_read_dev_vendor_id]
 *     → pci_bus_generic_read_dev_vendor_id()
 */
bool pci_bus_read_dev_vendor_id(struct pci_bus *bus, int devfn, u32 *l,
				int timeout)
{
	return pci_bus_generic_read_dev_vendor_id(bus, devfn, l, timeout);
	/* [한국어] 일반 구현으로 그대로 위임 */
}
EXPORT_SYMBOL(pci_bus_read_dev_vendor_id);
/* [한국어] 핫플러그 등 모듈에서 쓰므로 공개 */

/*
 * Read the config data for a PCI device, sanity-check it,
 * and fill in the dev structure.
 */
/*
 * [한국어]
 * pci_scan_device - 한 좌표를 조사해 장치가 있으면 pci_dev 를 만든다
 *
 * @bus:   대상 버스.
 * @devfn: 대상 (device, function) 좌표.
 * @return: 완성된 pci_dev(아직 등록 전). 장치가 없거나 해석에 실패하면 NULL.
 *
 * 왜 필요한가: "존재 판정 → 객체 할당 → config 해석"의 세 단계를 묶은
 * 함수다. 이 함수가 NULL 이 아닌 것을 돌려주어야 비로소 그 좌표에 장치가
 * 있다고 확정되고, 호출자 pci_scan_single_device() 가 pci_device_add() 로
 * 드라이버 모델에 올린다.
 *
 * 60초 타임아웃: 첫 인자로 60*1000ms 를 넘긴다. PCIe 스펙이 요구하는
 * 준비 시간보다 훨씬 넉넉한 값으로, 느린 장치도 놓치지 않으려는 선택이다.
 * 실제로는 대부분 수 ms 안에 끝난다.
 *
 * 실패 시 정리: pci_setup_device() 가 실패하면(알 수 없는 헤더 타입 등)
 * 아직 드라이버 모델에 등록되지 않은 상태이므로, put_device 가 아니라
 * 버스 참조를 직접 놓고 kfree 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. RRS 대기에서 최대 60초까지 잠들 수 있다.
 *
 * 호출 체인:
 *   pci_scan_slot() → pci_scan_single_device() → [pci_scan_device]
 *     → pci_bus_read_dev_vendor_id(), pci_alloc_dev(), pci_setup_device()
 */
static struct pci_dev *pci_scan_device(struct pci_bus *bus, int devfn)
{
	struct pci_dev *dev;
	/* [한국어] 만들 장치 객체 */
	u32 l;
	/* [한국어] config 0x00 에서 읽은 Vendor/Device ID dword */

	if (!pci_bus_read_dev_vendor_id(bus, devfn, &l, 60*1000))
		return NULL;
	/* [한국어] ★존재 판정★ 응답이 all-ones 등이면 이 좌표는 비어 있다.
	 * 60초까지 RRS 재시도를 허용한다. */

	dev = pci_alloc_dev(bus);
	if (!dev)
		return NULL;
	/* [한국어] 메모리 부족 — 장치는 있지만 표현할 객체를 못 만든다 */

	dev->devfn = devfn;
	/* [한국어] 이 장치의 좌표를 기록. 이후 모든 config 접근이 이 값을 쓴다 */
	dev->vendor = l & 0xffff;
	/* [한국어] 하위 16비트가 Vendor ID(PCI-SIG 가 회사마다 부여한 번호).
	 * 예: 0x144d 삼성, 0x8086 인텔. */
	dev->device = (l >> 16) & 0xffff;
	/* [한국어] 상위 16비트가 Device ID(회사가 제품마다 정한 번호).
	 * 이 두 값이 드라이버 id_table 매칭의 기본 키다. */

	if (pci_setup_device(dev)) {
		/* [한국어] 헤더 타입을 해석해 BAR/IRQ/클래스를 채운다.
		 * 0 이 아니면 이 장치를 다룰 수 없다는 뜻이다. */
		pci_bus_put(dev->bus);
		/* [한국어] pci_alloc_dev() 에서 잡은 버스 참조를 되돌린다 */
		kfree(dev);
		/* [한국어] 아직 device_initialize 전이라 kfree 가 맞다.
		 * (device_add 이후였다면 put_device 를 써야 한다.) */
		return NULL;
	}

	return dev;
	/* [한국어] 완성된 pci_dev. 아직 버스 목록에도 sysfs 에도 없다 */
}

/*
 * [한국어]
 * pcie_report_downtraining - 링크가 최대치보다 낮게 협상되었으면 알린다
 *
 * @dev: 검사할 장치.
 * @return: 없음. 문제가 있으면 로그를 남긴다.
 *
 * downtraining 이란: PCIe 링크는 양끝이 협상해 속도(Gen)와 폭(x1/x2/x4/…)을
 * 정한다. 신호 품질 문제나 슬롯 배선 제약 때문에 양쪽이 지원하는 최대치보다
 * 낮게 정해지는 것을 downtraining 이라 한다. 하드웨어는 아무 오류도 보고하지
 * 않고 조용히 느리게 동작하므로, 커널이 알려 주지 않으면 알아채기 어렵다.
 *
 * NVMe 접점: Gen4 x4 로 붙어야 할 SSD 가 Gen3 x4 로 잡히면 대역폭이 절반이
 * 되고, x4 여야 할 것이 x1 이면 4분의 1 이 된다. "새 SSD 인데 왜 이렇게
 * 느리지" 하는 상황의 첫 번째 확인 지점이 이 로그다.
 *
 * 세 가지 필터의 이유:
 *  1) 엔드포인트/레거시 엔드포인트/스위치 상향 포트만 본다 — 아래 영어
 *     주석대로 "장치 쪽에서 위를 보는" 방향이라야, 아무것도 꽂히지 않은
 *     하향 포트에 대해 헛된 경고가 나오지 않는다.
 *  2) function 0 에서만 보고한다 — 멀티펑션 장치의 모든 function 은 같은
 *     물리 링크를 공유하므로 같은 경고가 여러 번 나올 이유가 없다.
 *  3) VF 는 제외한다 — VF 도 PF 와 같은 링크를 쓴다.
 *
 * 실행 컨텍스트: 프로세스 문맥, pci_init_capabilities() 안에서 호출.
 *
 * 호출 체인:
 *   pci_device_add() → pci_init_capabilities() → [pcie_report_downtraining]
 *     → __pcie_print_link_status()
 */
void pcie_report_downtraining(struct pci_dev *dev)
{
	if (!pci_is_pcie(dev))
		return;
	/* [한국어] 링크 협상은 PCIe 개념이다 */

	/* Look from the device up to avoid downstream ports with no devices */
	if ((pci_pcie_type(dev) != PCI_EXP_TYPE_ENDPOINT) &&
	    (pci_pcie_type(dev) != PCI_EXP_TYPE_LEG_END) &&
	    (pci_pcie_type(dev) != PCI_EXP_TYPE_UPSTREAM))
		return;
	/* [한국어] 링크의 "아래쪽 끝"에 해당하는 세 종류만 검사한다.
	 * 이렇게 해야 빈 하향 포트(아무것도 꽂히지 않아 링크가 안 올라온 곳)에
	 * 대해 의미 없는 경고가 쏟아지지 않는다. NVMe SSD 는 ENDPOINT 라 통과한다. */

	/* Multi-function PCIe devices share the same link/status */
	if (PCI_FUNC(dev->devfn) != 0 || dev->is_virtfn)
		return;
	/* [한국어] 같은 링크에 대한 중복 경고를 막는다. function 1~7 과 VF 는
	 * function 0 / PF 와 물리 링크를 공유하므로 대표 하나만 보고하면 된다. */

	/* Print link status only if the device is constrained by the fabric */
	__pcie_print_link_status(dev, false);
	/* [한국어] 두 번째 인자 false 는 "항상 찍지 말고, 경로가 장치의 능력을
	 * 제한하고 있을 때만 찍어라"는 뜻이다(true 면 무조건 출력).
	 * 즉 정상적으로 최대 속도로 붙었으면 아무 로그도 나오지 않는다. */
}

/*
 * [한국어]
 * pci_imm_ready_init - Immediate Readiness 지원 여부를 기록한다
 *
 * @dev: 대상 장치.
 * @return: 없음. 지원하면 dev->imm_ready 가 1 이 된다.
 *
 * Immediate Readiness 란: 보통 장치는 D3hot 에서 D0 으로 돌아온 뒤 config
 * 접근에 답할 수 있게 되기까지 규정된 대기 시간(10ms 등)이 필요하다.
 * 이 비트가 서 있는 장치는 그 대기 없이 즉시 응답할 수 있다고 선언하는 것이다.
 * Status 레지스터(offset 0x06)의 PCI_STATUS_IMM_READY 비트로 표시된다.
 *
 * 왜 중요한가: 커널이 불필요한 지연을 건너뛸 수 있어 절전 복귀가 빨라진다.
 * 자주 절전에 들었다 나오는 장치일수록 누적 효과가 크다.
 *
 * 실행 컨텍스트: 프로세스 문맥, pci_init_capabilities() 안에서 호출.
 *
 * 호출 체인:
 *   pci_device_add() → pci_init_capabilities() → [pci_imm_ready_init]
 */
static void pci_imm_ready_init(struct pci_dev *dev)
{
	u16 status;
	/* [한국어] Status 레지스터 값 */

	pci_read_config_word(dev, PCI_STATUS, &status);
	/* [한국어] offset 0x06 — 오류 비트와 capability 목록 유무 등이 함께 있는
	 * 레지스터. 그중 한 비트가 Immediate Readiness 다. */
	if (status & PCI_STATUS_IMM_READY)
		dev->imm_ready = 1;
	/* [한국어] 지원하면 기록. 전원 상태 전환 코드(drivers/pci/pci.c)가
	 * 이 값을 보고 대기를 생략한다. */
}

/*
 * [한국어]
 * pci_init_capabilities - 장치가 가진 각 capability 별 하위 시스템을 초기화한다
 *
 * @dev: 방금 발견되어 아직 등록 전인 장치.
 * @return: 없음.
 *
 * 왜 필요한가: capability 는 "이 장치가 추가로 무엇을 할 수 있는가"의 목록이고,
 * 각 항목마다 그것을 다루는 커널 하위 시스템이 따로 있다. 이 함수는 그
 * 초기화 훅들을 한 줄씩 부르는 목록이다. 각 훅은 자기 capability 가 없으면
 * 조용히 아무 일도 하지 않으므로, 모든 장치에 대해 전부 불러도 안전하다.
 *
 * capability 를 찾는 공통 방식: 각 훅은 pci_find_capability()(표준 목록) 또는
 * pci_find_ext_capability()(확장 목록)를 쓴다. 표준 목록은 Status 레지스터의
 * CAP_LIST 비트를 확인한 뒤 Capabilities Pointer(header type 0/1 은 0x34,
 * CardBus 는 0x14)가 가리키는 곳에서 시작해, 각 항목의 첫 2바이트
 * [Capability ID, Next Pointer] 를 읽으며 Next 를 따라간다. Next 가 0 이면 끝.
 * 무한 루프 방지는 drivers/pci/pci.h 의 PCI_FIND_NEXT_CAP 매크로에 있는
 * TTL 48회 제한이며(PCI_FIND_CAP_TTL), 그 밖에 오프셋이 0x40 미만이면 중단,
 * 4바이트 경계로 내림, ID 가 0xff 면(응답 없음) 중단하는 방어가 함께 있다.
 * 확장 목록은 0x100 에서 시작하고 항목 헤더가 4바이트다.
 *
 * 순서의 의미: MSI/MSI-X 를 먼저 끄는 것이 중요하다 — 펌웨어가 켜 둔 채로
 * 남겨 두면 드라이버가 준비되기 전에 인터럽트가 들어올 수 있다.
 * 저장 버퍼 할당이 pci_pm_init() 보다 앞서는 것도 마찬가지 이유다.
 *
 * NVMe 접점: NVMe 컨트롤러에서는 pci_msix_init(MSI-X), pci_pm_init(전원 관리),
 * pci_aer_init(오류 보고), pci_iov_init(SR-IOV), pci_dpc_init(오류 격리)가
 * 실제로 의미 있는 일을 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 드라이버 바인딩 전.
 *
 * 호출 체인:
 *   pci_device_add() → [pci_init_capabilities] → 각 하위 시스템 init 함수
 */
static void pci_init_capabilities(struct pci_dev *dev)
{
	pci_ea_init(dev);		/* Enhanced Allocation */
	/* [한국어] 하드웨어에 고정된 BAR/버스 번호를 알려 주는 capability 처리 */
	pci_msi_init(dev);		/* Disable MSI */
	/* [한국어] MSI capability 를 찾아 위치를 기록하고, 펌웨어가 켜 두었다면
	 * 끈다. 드라이버가 준비되기 전에 인터럽트가 들어오면 안 되기 때문이다. */
	pci_msix_init(dev);		/* Disable MSI-X */
	/* [한국어] MSI-X 도 마찬가지로 위치를 기록하고 비활성화한다.
	 * MSI-X 는 표준 capability(ID 0x11)라 256바이트 영역 안에 있다.
	 * NVMe 드라이버는 나중에 pci_alloc_irq_vectors_affinity() 로
	 * 큐 수만큼 벡터를 요청해 다시 켠다. */

	/* Buffers for saving PCIe and PCI-X capabilities */
	pci_allocate_cap_save_buffers(dev);
	/* [한국어] 절전 진입 시 capability 레지스터 내용을 담아 둘 버퍼를 미리
	 * 할당한다. 복귀 시 하드웨어가 초기화되어 있으므로 커널이 저장해 둔
	 * 값을 되써야 한다. 아래 pci_pm_init() 이 이 버퍼를 전제로 동작한다. */

	pci_imm_ready_init(dev);	/* Immediate Readiness */
	/* [한국어] 절전 복귀 대기를 생략할 수 있는 장치인지 기록 */
	pci_pm_init(dev);		/* Power Management */
	/* [한국어] PM capability 를 읽어 D0~D3 지원 범위와 현재 상태를 확정.
	 * dev->current_state 가 PCI_UNKNOWN 에서 실제 값으로 바뀐다. */
	pci_vpd_init(dev);		/* Vital Product Data */
	/* [한국어] 제조사가 심어 둔 일련번호/부품번호 등을 읽는 통로 준비 */
	pci_configure_ari(dev);		/* Alternative Routing-ID Forwarding */
	/* [한국어] ARI 는 한 장치가 8개 넘는 function 을 가질 수 있게 하는 확장.
	 * SR-IOV 로 VF 를 많이 만들 때 필요하다. */
	pci_iov_init(dev);		/* Single Root I/O Virtualization */
	/* [한국어] SR-IOV capability 를 읽어 VF 를 몇 개까지 만들 수 있는지 등을
	 * 파악한다. NVMe 도 SR-IOV 를 지원하는 제품이 있다. */
	pci_ats_init(dev);		/* Address Translation Services */
	/* [한국어] 장치가 IOMMU 의 주소 변환 결과를 자체 캐시(ATC)에 두는 기능.
	 * untrusted 장치에는 허용되지 않는다. */
	pci_pri_init(dev);		/* Page Request Interface */
	/* [한국어] 장치가 없는 페이지를 만났을 때 호스트에 페이지 폴트를 요청하는
	 * 기능. ATS 위에서 동작한다. */
	pci_pasid_init(dev);		/* Process Address Space ID */
	/* [한국어] 장치가 여러 프로세스의 주소 공간을 구분해 접근할 수 있게 하는
	 * 식별자 기능. SVM(Shared Virtual Memory)의 기반이다. */
	pci_acs_init(dev);		/* Access Control Services */
	/* [한국어] 장치 간 peer-to-peer 트래픽을 IOMMU 로 강제로 우회시키는 기능.
	 * 이것이 있어야 IOMMU 그룹을 잘게 나눌 수 있어 VFIO 통과가 쉬워진다. */
	pci_ptm_init(dev);		/* Precision Time Measurement */
	/* [한국어] 호스트와 장치의 시계를 정밀하게 맞추는 기능 */
	pci_aer_init(dev);		/* Advanced Error Reporting */
	/* [한국어] 확장 capability 인 AER 을 초기화한다. 이것이 성공해야
	 * NVMe 컨트롤러의 링크 오류가 커널에 보고되고 복구 흐름이 돈다.
	 * 호스트 브리지의 native_aer 가 0 이면 실질적으로 동작하지 않는다. */
	pci_dpc_init(dev);		/* Downstream Port Containment */
	/* [한국어] 오류가 난 링크를 즉시 차단해 오염이 퍼지지 않게 하는 기능 */
	pci_rcec_init(dev);		/* Root Complex Event Collector */
	/* [한국어] 루트 컴플렉스 내장 장치들의 오류를 모아 보고하는 장치와의 연결 */
	pci_doe_init(dev);		/* Data Object Exchange */
	/* [한국어] config space 를 통해 장치와 구조화된 데이터를 주고받는 통로.
	 * CMA/SPDM 같은 인증 프로토콜이 이 위에서 동작한다. */
	pci_tph_init(dev);		/* TLP Processing Hints */
	/* [한국어] 장치가 TLP 에 "이 데이터는 어느 캐시에 두면 좋다" 같은 힌트를
	 * 실어 보내 캐시 적중률을 높이는 기능 */
	pci_rebar_init(dev);		/* Resizable BAR */
	/* [한국어] BAR 크기를 소프트웨어가 바꿀 수 있게 하는 확장.
	 * 큰 BAR 를 요구하는 장치가 주소 공간이 모자랄 때 줄일 수 있다. */
	pci_dev3_init(dev);		/* Device 3 capabilities */
	/* [한국어] 위에서 본 Device 3 capability — flit 모드 감지 여부 기록 */
	pci_ide_init(dev);		/* Link Integrity and Data Encryption */
	/* [한국어] 링크 구간의 무결성 검증과 암호화 기능 초기화 */

	pcie_report_downtraining(dev);
	/* [한국어] 링크가 최대치보다 낮게 붙었으면 경고. 위 초기화들이 끝난
	 * 뒤라 링크 정보가 모두 갖춰져 있다. */
	pci_init_reset_methods(dev);
	/* [한국어] 이 장치를 리셋할 수 있는 방법들(FLR, PM 리셋, 버스 리셋 등)을
	 * 조사해 우선순위 목록으로 만든다. NVMe 컨트롤러가 응답하지 않을 때
	 * 커널이 시도할 복구 수단이 여기서 정해진다. */
}

/*
 * This is the equivalent of pci_host_bridge_msi_domain() that acts on
 * devices. Firmware interfaces that can select the MSI domain on a
 * per-device basis should be called from here.
 */
/*
 * [한국어]
 * pci_dev_msi_domain - 이 장치만의 MSI 도메인이 따로 있는지 찾는다
 *
 * @dev: 대상 장치.
 * @return: 장치 전용 도메인이 있으면 그것, 없으면 NULL(버스 것을 상속해야 함).
 *
 * 왜 장치별 도메인이 필요한가: 대부분의 장치는 버스의 도메인을 그대로 쓰지만,
 * 인터럽트 리매핑을 하는 시스템에서는 장치마다 다른 도메인이 필요할 수 있다.
 * 위 영어 주석대로, 장치 단위로 도메인을 고를 수 있는 펌웨어 인터페이스가
 * 있다면 여기서 물어봐야 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, pci_device_add() 안에서 호출.
 *
 * 호출 체인:
 *   pci_device_add() → pci_set_msi_domain() → [pci_dev_msi_domain]
 */
static struct irq_domain *pci_dev_msi_domain(struct pci_dev *dev)
{
	struct irq_domain *d;
	/* [한국어] 찾은 도메인 */

	/*
	 * If a domain has been set through the pcibios_device_add()
	 * callback, then this is the one (platform code knows best).
	 */
	d = dev_get_msi_domain(&dev->dev);
	/* [한국어] 1순위: 아키텍처 코드가 pcibios_device_add() 훅에서 이미
	 * 설정해 둔 도메인. 영어 주석대로 플랫폼 코드가 가장 잘 안다. */
	if (d)
		return d;

	/*
	 * Let's see if we have a firmware interface able to provide
	 * the domain.
	 */
	d = pci_msi_get_device_domain(dev);
	/* [한국어] 2순위: ACPI/DT 가 이 장치(BDF)에 대해 지정한 도메인 */
	if (d)
		return d;

	return NULL;
	/* [한국어] 장치 전용 도메인이 없다 — 호출자가 버스 것을 상속시킨다 */
}

/*
 * [한국어]
 * pci_set_msi_domain - 장치의 MSI 도메인을 확정한다
 *
 * @dev: 대상 장치.
 * @return: 없음. dev->dev 의 msi_domain 이 설정된다.
 *
 * 왜 필요한가: MSI/MSI-X 벡터를 요청하려면 그 벡터를 발급할 도메인이
 * 장치에 연결되어 있어야 한다. 장치 전용 도메인이 있으면 그것을, 없으면
 * 버스의 도메인을 물려받는다.
 *
 * NVMe 접점: NVMe 드라이버가 pci_alloc_irq_vectors_affinity() 로 큐마다
 * MSI-X 벡터를 요청하면(drivers/nvme/host/pci.c:2849), 그 벡터들은 여기서
 * 확정된 도메인에서 할당된다. 도메인이 NULL 이면 MSI-X 를 쓸 수 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥, device_add() 전.
 *
 * 호출 체인:
 *   pci_device_add() → [pci_set_msi_domain] → pci_dev_msi_domain()
 */
static void pci_set_msi_domain(struct pci_dev *dev)
{
	struct irq_domain *d;
	/* [한국어] 확정할 도메인 */

	/*
	 * If the platform or firmware interfaces cannot supply a
	 * device-specific MSI domain, then inherit the default domain
	 * from the host bridge itself.
	 */
	d = pci_dev_msi_domain(dev);
	/* [한국어] 장치 전용 도메인을 먼저 찾아본다 */
	if (!d)
		d = dev_get_msi_domain(&dev->bus->dev);
	/* [한국어] 없으면 이 장치가 붙은 버스의 도메인을 그대로 쓴다.
	 * 버스의 도메인은 pci_set_bus_msi_domain() 이 이미 정해 두었다. */

	dev_set_msi_domain(&dev->dev, d);
	/* [한국어] 장치에 기록. 이 값이 NULL 이면 이 장치는 MSI/MSI-X 를
	 * 할당받을 수 없고 레거시 INTx 로 떨어진다. */
}

/*
 * [한국어]
 * pci_device_add - 완성된 pci_dev 를 드라이버 모델에 올린다 (드라이버 매칭 개시 지점)
 *
 * @dev: pci_setup_device() 까지 마쳐 필드가 채워진 pci_dev.
 * @bus: 이 장치가 붙은 버스.
 * @return: 없음. 실패는 WARN_ON 으로 알릴 뿐 되돌리지 않는다.
 *
 * ★ 이 함수의 device_add() 호출이 "장치가 시스템의 일부가 되는" 순간이다 ★
 * device_add() 가 성공하면 드라이버 코어가 pci_bus_type 에 등록된 모든
 * 드라이버를 훑으며 pci_bus_match() 로 id_table 대조를 시작한다. 일치하는
 * 드라이버가 있으면 그 자리에서 probe 가 호출된다. NVMe SSD 라면
 * nvme_id_table 의 PCI_DEVICE_CLASS(PCI_CLASS_STORAGE_EXPRESS, 0xffffff)
 * (drivers/nvme/host/pci.c:4137)에 클래스 0x010802 가 걸려 nvme_probe() 가
 * 불리고, 그 안에서 pci_enable_device_mem() → pci_request_mem_regions() →
 * BAR0 ioremap → CAP 읽기 → Admin Queue 설정으로 이어진다.
 * 즉 /dev/nvme0 이 나타나는 사슬의 시작점이 이 한 줄이다.
 *
 * 왜 순서가 이런가:
 *  1) pci_configure_device()  — 드라이버가 붙기 전에 링크/전송 설정을 끝낸다.
 *  2) device_initialize()     — refcount 를 1 로 만든다. 이후 kfree 금지.
 *  3) DMA 파라미터 설정       — 드라이버가 DMA 를 시작하기 전에 준비.
 *  4) 헤더 quirk, capability 초기화 — 아래 영어 주석대로 알림(notifier)이
 *     capability 정보를 참조할 수 있으므로 device_add 보다 먼저 끝내야 한다.
 *  5) 버스 목록에 등록        — fixup 함수들이 목록을 훑을 수 있게.
 *  6) MSI 도메인 설정         — 드라이버가 벡터를 요청하기 전에.
 *  7) device_add()            — 드라이버 매칭 개시.
 *
 * 실행 컨텍스트: 프로세스 문맥. 버스 목록 갱신 구간에서만 pci_bus_sem 을
 * 쓰기 모드로 잡는다. device_add() 자체는 락 없이 호출되며, 그 안에서
 * 드라이버 probe 가 동기적으로 실행될 수 있다.
 *
 * 호출 체인:
 *   pci_scan_single_device() → [pci_device_add]
 *     → pci_configure_device(), pci_init_capabilities(), device_add()
 *     → (드라이버 매칭) → nvme_probe() 등
 */
void pci_device_add(struct pci_dev *dev, struct pci_bus *bus)
{
	int ret;
	/* [한국어] 하위 호출의 결과 코드. 실패해도 진행하되 WARN 으로 알린다 */

	pci_configure_device(dev);
	/* [한국어] MPS/Extended Tag/RO/LTR/ASPM L1SS/EETLP/SERR/RCB 설정.
	 * 드라이버가 장치를 쓰기 시작하기 전에 링크 설정을 끝내야 한다. */

	device_initialize(&dev->dev);
	/* [한국어] 드라이버 모델 객체 초기화. refcount 가 1 이 되며, 이 시점
	 * 이후로는 kfree 가 아니라 put_device() 로 해제해야 한다. */
	dev->dev.release = pci_release_dev;
	/* [한국어] refcount 0 시 부를 소멸자를 직접 지정한다.
	 * pci_dev_type 에는 release 가 없으므로 여기서 설정하는 것이 유일한
	 * 해제 경로다. */

	set_dev_node(&dev->dev, pcibus_to_node(bus));
	/* [한국어] 이 장치가 속한 NUMA 노드를 기록. 이후 커널이 DMA 버퍼를
	 * 그 노드의 메모리에서 할당하고 인터럽트도 그 노드의 CPU 에 배치해
	 * 원격 노드 접근 지연을 줄인다. NVMe 성능에 직접 영향을 준다. */
	dev->dev.dma_mask = &dev->dma_mask;
	/* [한국어] 일반 device 구조체가 pci_dev 안의 dma_mask 를 가리키게 한다.
	 * 드라이버가 dma_set_mask() 를 부르면 이 포인터를 통해 갱신된다. */
	dev->dev.dma_parms = &dev->dma_parms;
	/* [한국어] 최대 세그먼트 크기/경계 같은 DMA 제약을 담는 구조체 연결.
	 * 블록 계층이 이 값을 보고 요청을 쪼갠다. */
	dev->dev.coherent_dma_mask = 0xffffffffull;
	/* [한국어] 일관성(coherent) DMA 버퍼의 주소 상한 기본값 = 32비트.
	 * NVMe 의 Admin/IO 큐 메모리처럼 dma_alloc_coherent 로 잡는 버퍼에
	 * 적용된다. 드라이버가 64비트로 올릴 수 있다. */

	dma_set_max_seg_size(&dev->dev, 65536);
	/* [한국어] 한 DMA 세그먼트의 최대 크기 기본값 64KB. 보수적인 값이며
	 * 드라이버가 더 크게 올릴 수 있다. */
	dma_set_seg_boundary(&dev->dev, 0xffffffff);
	/* [한국어] DMA 세그먼트가 넘어서는 안 되는 주소 경계 = 4GB.
	 * 구식 하드웨어 중에 4GB 경계를 넘는 전송을 처리하지 못하는 것이
	 * 있어 기본값이 보수적이다. */

	pcie_failed_link_retrain(dev);
	/* [한국어] 링크가 제대로 학습되지 않은 경우 재학습을 시도하는 quirk
	 * 성격의 처리. 일부 하드웨어에서 첫 협상이 실패하면 장치가 아예
	 * 보이지 않거나 낮은 속도로 붙는다. */

	/* Fix up broken headers */
	pci_fixup_device(pci_fixup_header, dev);
	/* [한국어] 헤더 단계 quirk 적용. 벤더가 잘못 보고한 클래스 코드나
	 * BAR 정보를 이 단계에서 바로잡는다. capability 초기화 전에 해야
	 * 잘못된 정보로 하위 시스템이 초기화되는 것을 막는다. */

	pci_reassigndev_resource_alignment(dev);
	/* [한국어] pci=resource_alignment 커널 파라미터로 사용자가 특정 장치의
	 * BAR 정렬을 강제한 경우 그것을 반영한다. VFIO 로 장치를 게스트에
	 * 넘길 때 페이지 단위 정렬이 필요해 쓰이는 기능이다. */

	pci_init_capabilities(dev);
	/* [한국어] MSI/MSI-X 비활성화, PM/AER/SR-IOV/ATS 등 모든 capability
	 * 하위 시스템 초기화. device_add 전에 끝나야 한다(아래 주석 참조). */

	/*
	 * Add the device to our list of discovered devices
	 * and the bus list for fixup functions, etc.
	 */
	down_write(&pci_bus_sem);
	/* [한국어] 버스의 장치 목록을 보호하는 세마포어를 쓰기 모드로 획득.
	 * 다른 CPU 가 동시에 목록을 순회(sysfs, 전원 관리)하거나 수정
	 * (핫플러그 제거)하는 것을 막는다. */
	list_add_tail(&dev->bus_list, &bus->devices);
	/* [한국어] 이 버스의 장치 목록 끝에 매단다. devfn 오름차순으로 스캔하므로
	 * tail 에 붙이면 자연히 번호 순으로 정렬된다.
	 * 이제 pci_get_slot() 같은 조회 함수가 이 장치를 찾을 수 있다. */
	up_write(&pci_bus_sem);
	/* [한국어] 락 해제. 목록 조작만 보호하면 되므로 구간을 최소화했다 */

	ret = pcibios_device_add(dev);
	/* [한국어] 아키텍처별 장치 추가 훅. x86/ACPI 는 여기서 MSI 도메인이나
	 * DMA 설정을 손볼 수 있다(그래서 아래 pci_set_msi_domain 이 이것보다
	 * 뒤에 온다 — pci_dev_msi_domain() 이 그 결과를 1순위로 본다). */
	WARN_ON(ret < 0);
	/* [한국어] 실패는 정상 동작에서 일어나지 않으므로 스택 트레이스를 남긴다.
	 * 그래도 등록을 중단하지는 않는다 — 장치를 통째로 잃는 것보다 낫다. */

	/* Set up MSI IRQ domain */
	pci_set_msi_domain(dev);
	/* [한국어] 이 장치가 MSI/MSI-X 벡터를 받아 올 도메인을 확정한다.
	 * 드라이버 probe 에서 벡터를 요청하기 전에 반드시 끝나야 한다. */

	/* Notifier could use PCI capabilities */
	ret = device_add(&dev->dev);
	/* [한국어] ★드라이버 모델 등록★ sysfs 에 노드가 만들어지고, uevent 가
	 * 사용자 공간(udev)으로 나가며, pci_bus_type 의 드라이버들과 매칭이
	 * 시작된다. 일치하는 드라이버가 이미 등록되어 있으면 이 호출 안에서
	 * 동기적으로 probe 가 실행된다.
	 * 위 영어 주석은 이 시점에 불리는 알림(notifier) 수신자들이 capability
	 * 정보를 참조할 수 있으므로 pci_init_capabilities() 가 먼저여야 한다는
	 * 순서 제약을 설명한 것이다. */
	WARN_ON(ret < 0);
	/* [한국어] 등록 실패 시 경고. 이 경우 장치는 목록에는 있지만 sysfs 에는
	 * 없는 어중간한 상태가 된다. */

	/* Establish pdev->tsm for newly added (e.g. new SR-IOV VFs) */
	pci_tsm_init(dev);
	/* [한국어] TSM = TEE Security Manager. TDISP(TEE Device Interface
	 * Security Protocol)에서 장치 인증, 링크 암호화, 기밀 컴퓨팅 VM 에 대한
	 * function 할당을 관리하는 플랫폼 에이전트다(drivers/pci/Kconfig 의
	 * PCI_TSM 설명). 위 영어 주석대로 새로 추가된 장치(새 SR-IOV VF 포함)에
	 * 대해 pdev->tsm 연결을 세운다. CONFIG_PCI_TSM 이 꺼져 있으면 빈 함수다. */

	pci_npem_create(dev);
	/* [한국어] NPEM = Native PCIe Enclosure Management. 스토리지 인클로저의
	 * LED 표시(OK/Locate/Fail/Rebuild)를 PCIe 표준 방식으로 제어하는 기능이며
	 * LED 클래스 장치를 만든다(drivers/pci/Kconfig 의 PCI_NPEM).
	 * NVMe 백플레인에서 특정 베이의 위치 표시등을 켜는 것이 이 기능이다. */

	pci_doe_sysfs_init(dev);
	/* [한국어] DOE(Data Object Exchange) 관련 sysfs 항목 생성.
	 * device_add() 이후여야 sysfs 부모 노드가 존재한다. */
}

/*
 * [한국어]
 * pci_scan_single_device - 한 좌표의 장치를 스캔해 등록까지 마친다
 *
 * @bus:   대상 버스.
 * @devfn: 대상 (device, function) 좌표.
 * @return: 그 좌표의 pci_dev(새로 만든 것이든 이미 있던 것이든).
 *          장치가 없으면 NULL.
 *
 * 왜 필요한가: "발견 → 해석 → 등록"의 전 과정을 한 좌표에 대해 수행하는
 * 가장 바깥 단위다. 핫플러그 드라이버가 슬롯 하나를 다시 훑을 때도 이
 * 함수를 쓰므로 외부에 공개되어 있다.
 *
 * 재스캔 안전성: 이미 등록된 장치가 있으면 새로 만들지 않고 기존 것을
 * 돌려준다. rescan 시 같은 장치가 두 번 만들어지는 것을 막는다.
 *
 * 반환 참조에 대한 주의: pci_get_slot() 이 올린 참조를 곧바로
 * pci_dev_put() 으로 내려놓고 포인터만 돌려준다. 즉 호출자는 참조를
 * 소유하지 않는다. 호출자(pci_scan_slot 등)가 버스 스캔 문맥 안에서만
 * 이 포인터를 쓰고, 그동안 장치가 사라지지 않음이 보장되기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 내부에서 최대 60초까지 잠들 수 있다.
 *
 * 호출 체인:
 *   pci_scan_slot() → [pci_scan_single_device]
 *     → pci_scan_device() → pci_device_add()
 */
struct pci_dev *pci_scan_single_device(struct pci_bus *bus, int devfn)
{
	struct pci_dev *dev;
	/* [한국어] 찾았거나 새로 만든 장치 */

	dev = pci_get_slot(bus, devfn);
	/* [한국어] 이 좌표에 이미 등록된 장치가 있는지 버스 목록에서 찾는다.
	 * 찾으면 참조를 하나 올려서 돌려준다. */
	if (dev) {
		pci_dev_put(dev);
		/* [한국어] 방금 올린 참조를 즉시 내린다. 호출자에게 참조를
		 * 넘기지 않는다는 이 함수의 규약 때문이다. */
		return dev;
		/* [한국어] 이미 알고 있는 장치 — 재스캔 시의 정상 경로다 */
	}

	dev = pci_scan_device(bus, devfn);
	/* [한국어] ★새 장치 발견 시도★ Vendor ID 를 읽어 존재를 판정하고,
	 * 있으면 config 를 해석해 pci_dev 를 만든다. */
	if (!dev)
		return NULL;
	/* [한국어] 이 좌표는 비어 있다 */

	pci_device_add(dev, bus);
	/* [한국어] ★드라이버 모델 등록★ 이 호출 안의 device_add() 에서
	 * 드라이버 매칭이 시작된다. NVMe SSD 라면 여기서 nvme_probe() 가
	 * 실행되어 컨트롤러 초기화가 시작된다. */

	return dev;
	/* [한국어] 등록까지 마친 장치 */
}
EXPORT_SYMBOL(pci_scan_single_device);
/* [한국어] 핫플러그 드라이버 등이 모듈에서 쓰므로 공개 */

/*
 * [한국어]
 * next_ari_fn - ARI 링크를 따라 다음 function 번호를 얻는다
 *
 * @bus: 대상 버스(현재 구현에서는 참조하지 않지만 next_fn 과 시그니처를 맞춘다).
 * @dev: 직전에 스캔한 장치. 첫 호출에서는 function 0 의 장치다.
 * @fn:  직전 function 번호.
 * @return: 다음에 스캔할 function 번호. 더 없으면 -ENODEV.
 *
 * ARI 란: Alternative Routing-ID Interpretation. 원래 PCI 는 devfn 8비트를
 * "device 5비트 + function 3비트"로 나눠 한 장치당 function 을 8개까지만
 * 허용했다. ARI 는 그 8비트 전체를 function 번호로 재해석해 한 장치가
 * 최대 256개의 function 을 가질 수 있게 한다. SR-IOV 로 VF 를 많이 만들 때
 * 필수적인 확장이다.
 *
 * 어떻게 순회하나: ARI 는 8개를 순서대로 훑는 방식이 아니라, 각 function 의
 * ARI capability 안에 있는 "Next Function Number" 필드가 다음 function 을
 * 가리키는 연결 리스트 구조다. 구현되지 않은 번호를 건너뛸 수 있어 효율적이다.
 * 리스트의 끝은 Next Function Number 가 현재 번호 이하가 되는 것으로 판정한다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_scan_slot() → next_fn() → [next_ari_fn] → pci_find_ext_capability()
 */
static int next_ari_fn(struct pci_bus *bus, struct pci_dev *dev, int fn)
{
	int pos;
	/* [한국어] ARI 확장 capability 의 오프셋 */
	u16 cap = 0;
	/* [한국어] ARI Capability 레지스터 값. 읽기 실패 시에도 0 이 남도록 초기화 */
	unsigned int next_fn;
	/* [한국어] 추출한 다음 function 번호 */

	if (!dev)
		return -ENODEV;
	/* [한국어] 직전 function 이 존재하지 않으면 그 capability 를 읽을 수 없다.
	 * ARI 리스트가 끊긴 것이므로 순회를 끝낸다. */

	pos = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_ARI);
	/* [한국어] ARI 는 확장 capability(0x100 이상)다. 확장 config 접근이
	 * 막혀 있으면 여기서 0 이 나온다. */
	if (!pos)
		return -ENODEV;
	/* [한국어] ARI 미지원 — 이 장치에는 더 이상의 function 이 없다 */

	pci_read_config_word(dev, pos + PCI_ARI_CAP, &cap);
	/* [한국어] ARI Capability 레지스터 — Next Function Number 필드를 담는다 */
	next_fn = PCI_ARI_CAP_NFN(cap);
	/* [한국어] 그 필드를 뽑아내는 매크로. 0 이면 리스트의 끝을 뜻한다 */
	if (next_fn <= fn)
		return -ENODEV;	/* protect against malformed list */
	/* [한국어] ★무한 루프 방지★ 다음 번호는 반드시 현재보다 커야 한다.
	 * 같거나 작으면 리스트가 자기 자신이나 앞쪽을 가리키는 것이라
	 * 순환이 생긴다. 위 영어 주석의 "malformed list" 방어다.
	 * 번호가 단조 증가하고 상한이 255 이므로 순회는 반드시 끝난다. */

	return next_fn;
	/* [한국어] 다음에 스캔할 function 번호 */
}

/*
 * [한국어]
 * next_fn - 다음에 스캔할 function 번호를 정한다 (ARI / 전통 방식 분기)
 *
 * @bus: 대상 버스.
 * @dev: 직전에 스캔한 장치(없을 수도 있다).
 * @fn:  직전 function 번호.
 * @return: 다음 function 번호. 더 스캔할 것이 없으면 -ENODEV.
 *
 * 왜 필요한가: function 순회 방식이 두 가지다. ARI 가 켜진 버스에서는
 * capability 링크를 따라가고, 그렇지 않으면 0~7 을 차례로 훑는다.
 * 그 분기를 한곳에 모아 pci_scan_slot() 의 루프를 단순하게 유지한다.
 *
 * 전통 방식의 중요한 최적화: function 0 의 헤더 타입 최상위 비트
 * (PCI_HEADER_TYPE_MFD, 0x80)가 0 이면 그 장치는 단일 function 이므로
 * 1~7 을 볼 필요가 없다. 이 검사가 없으면 모든 슬롯에서 헛되이 7번의
 * config 읽기를 더 하게 되어 부팅이 느려진다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_scan_slot() → [next_fn] → next_ari_fn()
 */
static int next_fn(struct pci_bus *bus, struct pci_dev *dev, int fn)
{
	if (pci_ari_enabled(bus))
		return next_ari_fn(bus, dev, fn);
	/* [한국어] 이 버스에서 ARI 가 켜져 있으면(상위 브리지가 ARI 포워딩을
	 * 활성화했으면) capability 링크를 따라간다. 최대 256개까지 가능하다. */

	if (fn >= 7)
		return -ENODEV;
	/* [한국어] 전통 방식의 상한. devfn 의 function 필드가 3비트라 0~7 뿐이다 */
	/* only multifunction devices may have more functions */
	if (dev && !dev->multifunction)
		return -ENODEV;
	/* [한국어] function 0 의 헤더 타입에 멀티펑션 비트가 없으면 단일 function
	 * 장치다. 1~7 을 읽어 봐야 아무것도 없으므로 즉시 끝낸다.
	 * dev 가 NULL 인 경우(function 0 이 없는데 하이퍼바이저 통과 모드라
	 * 계속 훑는 상황)에는 이 검사를 건너뛰고 다음 번호로 넘어간다. */

	return fn + 1;
	/* [한국어] 다음 function 번호 */
}

/*
 * [한국어]
 * only_one_child - 이 버스에는 device 0 만 있을 수 있는가
 *
 * @bus: 대상 버스.
 * @return: 1 이면 device 0 만 스캔하면 된다. 0 이면 32개 device 를 모두 훑는다.
 *
 * 왜 필요한가: PCI 버스 하나에는 device 번호가 0~31 까지 있을 수 있어,
 * 모두 훑으려면 32번(멀티펑션까지 하면 그 이상)의 config 접근이 필요하다.
 * 그런데 PCIe 링크는 점대점(point-to-point) 연결이라 아래 영어 주석이
 * 인용하는 PCIe spec r3.1 sec 7.3.1 대로 device 0 하나만 존재한다.
 * 그래서 하향 포트 아래에서는 device 0 만 보고 끝낸다. 장치가 많은 서버에서
 * 부팅 시간에 눈에 띄는 차이를 만든다.
 *
 * PCI_SCAN_ALL_PCIE_DEVS 예외: 일부 특이한 토폴로지(가상화 환경, 비표준
 * 브리지)에서는 하향 포트 아래에 device 0 이 아닌 장치가 나타난다.
 * 그런 시스템은 이 플래그를 세워 최적화를 끈다.
 *
 * NVMe 접점: NVMe SSD 는 Root Port 나 스위치 하향 포트 아래에 device 0 으로
 * 붙으므로 이 최적화의 혜택을 받는다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_scan_slot() → [only_one_child]
 */
static int only_one_child(struct pci_bus *bus)
{
	struct pci_dev *bridge = bus->self;
	/* [한국어] 이 버스로 내려오는 브리지. 루트 버스면 NULL 이다 */

	/*
	 * Systems with unusual topologies set PCI_SCAN_ALL_PCIE_DEVS so
	 * we scan for all possible devices, not just Device 0.
	 */
	if (pci_has_flag(PCI_SCAN_ALL_PCIE_DEVS))
		return 0;
	/* [한국어] 전역 플래그로 최적화가 꺼져 있으면 전부 훑는다.
	 * 아키텍처 코드나 quirk 가 이 플래그를 세운다. */

	/*
	 * A PCIe Downstream Port normally leads to a Link with only Device
	 * 0 on it (PCIe spec r3.1, sec 7.3.1).  As an optimization, scan
	 * only for Device 0 in that situation.
	 */
	if (bridge && pci_is_pcie(bridge) && pcie_downstream_port(bridge))
		return 1;
	/* [한국어] 세 조건이 모두 맞아야 한다: 브리지가 존재하고, PCIe 이고,
	 * 하향 포트(Root Port 또는 스위치 하향 포트)여야 한다.
	 * 그 아래는 점대점 링크이므로 device 0 하나뿐이다. */

	return 0;
	/* [한국어] 구식 PCI 버스나 루트 버스 — 여러 device 가 공유할 수 있으므로
	 * 32개를 모두 훑어야 한다. */
}

/**
 * pci_scan_slot - Scan a PCI slot on a bus for devices
 * @bus: PCI bus to scan
 * @devfn: slot number to scan (must have zero function)
 *
 * Scan a PCI slot on the specified PCI bus for devices, adding
 * discovered devices to the @bus->devices list.  New devices
 * will not have is_added set.
 *
 * Returns the number of new devices found.
 */
/*
 * [한국어]
 * pci_scan_slot - 한 슬롯(device 번호 하나)의 모든 function 을 훑는다
 *
 * @bus:   대상 버스.
 * @devfn: 슬롯의 시작 좌표. function 부분이 0 이어야 한다(= device 번호 << 3).
 * @return: 이번에 새로 발견한 장치의 개수.
 *
 * ★ 장치 발견의 실질적 단위 ★
 * PCI 에서 "슬롯"은 하나의 device 번호를 뜻하고, 그 안에 최대 8개(ARI 면 256개)의
 * function 이 있을 수 있다. 이 함수는 function 0 부터 시작해 next_fn() 이
 * 정해 주는 순서대로 훑으며, 각 좌표에 대해 pci_scan_single_device() 를 부른다.
 *
 * function 0 의 특별한 지위: PCI 스펙상 function 0 이 없으면 그 슬롯에는
 * 아무 function 도 없는 것으로 본다. 그래서 function 0 이 비어 있으면 즉시
 * 중단해 나머지 7번의 헛된 config 읽기를 아낀다. 예외는 아래 영어 주석이
 * 설명하는 하이퍼바이저 통과 상황 — 게스트에 function 3 만 넘겨 주는 식으로
 * 개별 function 을 통과시키면 function 0 이 비어 있어도 뒤에 장치가 있다.
 *
 * NVMe 접점: NVMe SSD 는 대개 단일 function 이라 function 0 하나만 발견되고,
 * dev->multifunction 이 0 이므로 next_fn() 이 곧바로 -ENODEV 를 돌려 루프가 끝난다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 내부에서 RRS 대기로 잠들 수 있다.
 *
 * 호출 체인:
 *   pci_scan_child_bus_extend() → [pci_scan_slot]
 *     → pci_scan_single_device(), next_fn(), pcie_aspm_init_link_state()
 */
int pci_scan_slot(struct pci_bus *bus, int devfn)
{
	struct pci_dev *dev;
	/* [한국어] 방금 스캔한 function 의 장치(없으면 NULL) */
	int fn = 0, nr = 0;
	/* [한국어] fn = 현재 function 번호(0 부터 시작),
	 * nr = 이번 호출에서 새로 발견한 장치 수(반환값). */

	if (only_one_child(bus) && (devfn > 0))
		return 0; /* Already scanned the entire slot */
	/* [한국어] PCIe 하향 포트 아래에서는 device 0(devfn 0)만 존재한다.
	 * devfn 이 0 보다 크다는 것은 device 1 이상을 훑으려는 것이므로
	 * 헛수고다. 첫 호출(devfn == 0)에서 이미 그 슬롯 전체를 봤다. */

	do {
		dev = pci_scan_single_device(bus, devfn + fn);
		/* [한국어] devfn 은 슬롯의 시작 좌표(function 0)이고 fn 을 더하면
		 * 그 슬롯 안의 특정 function 좌표가 된다. 이 호출이 장치를
		 * 발견하고 등록까지 마친다. */
		if (dev) {
			/* [한국어] 이 function 에 장치가 있다 */
			if (!pci_dev_is_added(dev))
				nr++;
			/* [한국어] 재스캔에서 이미 등록되어 있던 장치는 세지 않는다.
			 * 반환값은 "이번에 새로 나타난 장치 수"라야 호출자가
			 * 핫플러그 결과를 판단할 수 있다. */
			if (fn > 0)
				dev->multifunction = 1;
			/* [한국어] function 0 이 아닌 자리에서 장치가 나왔다는 것은
			 * 이 슬롯이 실제로 멀티펑션이라는 증거다. function 0 의 헤더에
			 * 멀티펑션 비트를 세우지 않은 스펙 위반 장치가 있어, 발견된
			 * 사실로부터 역으로 표시해 준다. */
		} else if (fn == 0) {
			/*
			 * Function 0 is required unless we are running on
			 * a hypervisor that passes through individual PCI
			 * functions.
			 */
			if (!hypervisor_isolated_pci_functions())
				break;
			/* [한국어] function 0 이 비어 있다 = 보통은 이 슬롯 전체가
			 * 비어 있다는 뜻이므로 즉시 중단한다.
			 * 다만 하이퍼바이저가 function 단위로 장치를 통과시키는
			 * 환경(예: 게스트에 function 1 만 할당)에서는 function 0 이
			 * 없어도 뒤에 장치가 있을 수 있으므로 계속 훑는다. */
		}
		fn = next_fn(bus, dev, fn);
		/* [한국어] 다음 function 번호를 정한다. ARI 면 capability 링크를,
		 * 아니면 fn+1 을 돌려주며, 더 없으면 음수(-ENODEV)를 돌려준다. */
	} while (fn >= 0);
	/* [한국어] 음수가 나오면 순회 종료. do-while 이라 function 0 은
	 * 조건과 무관하게 반드시 한 번 검사된다. */

	/* Only one slot has PCIe device */
	if (bus->self && nr)
		pcie_aspm_init_link_state(bus->self);
	/* [한국어] 이 버스에 장치가 실제로 하나라도 새로 생겼을 때에만,
	 * 이 버스로 내려오는 링크의 ASPM 상태를 초기화한다. 링크 양끝의
	 * 능력을 모두 알아야 절전 정책을 정할 수 있는데, 아래쪽 장치가
	 * 이제 막 발견되었기 때문이다. 장치가 없는 빈 링크에는 할 일이 없다. */

	return nr;
	/* [한국어] 새로 발견한 장치 수. 호출자는 이 값을 직접 쓰지는 않지만,
	 * 핫플러그 경로에서는 "무엇이 나타났는가"의 판단 근거가 된다. */
}
EXPORT_SYMBOL(pci_scan_slot);
/* [한국어] 핫플러그 드라이버 등이 모듈에서 쓰므로 공개 */

/*
 * [한국어]
 * pcie_find_smpss - 계층 안에서 가장 작은 MPSS 를 찾는 pci_walk_bus 콜백
 *
 * @dev:  순회 중 만난 장치.
 * @data: u8 * — 지금까지 찾은 최솟값을 담고 있으며 여기서 갱신된다.
 * @return: 항상 0(0 이 아니면 pci_walk_bus 순회가 중단된다).
 *
 * 왜 필요한가: PCIE_BUS_SAFE 정책은 계층 안 모든 장치가 감당할 수 있는
 * 공통 MPS 를 쓴다. 그러려면 먼저 최솟값을 알아야 하고, 그것을 구하려고
 * 버스 전체를 한 번 훑는 것이 이 콜백이다.
 *
 * 핫플러그 브리지를 만나면 0(=128B)으로 못박는 이유: 아래 영어 주석이
 * 자세히 설명한다. 드라이버가 이미 붙은 장치의 MPS 는 바꿀 수 없는데,
 * 나중에 꽂힐 장치는 128B 만 지원할 수도 있다. 그러면 계층 전체를 그 장치에
 * 맞춰 낮춰야 하는데 그때는 이미 늦다. 그래서 핫플러그 가능성이 있으면
 * 미리 128B 로 낮춰 둔다.
 *
 * Root Port 는 예외인 이유: Root Port 아래 링크에는 장치가 하나뿐이므로
 * (점대점), 나중에 장치를 꽂으면 Root Port 와 그 장치 둘만 다시 설정하면 된다.
 * 다른 장치가 영향받지 않으므로 미리 낮출 필요가 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥, pci_walk_bus() 순회 안.
 *
 * 호출 체인:
 *   pcie_bus_configure_settings() → pci_walk_bus() → [pcie_find_smpss]
 */
static int pcie_find_smpss(struct pci_dev *dev, void *data)
{
	u8 *smpss = data;
	/* [한국어] 호출자가 넘긴 "지금까지의 최솟값" 저장소.
	 * void * 콜백 규약이라 타입을 되살려 쓴다. */

	if (!pci_is_pcie(dev))
		return 0;
	/* [한국어] 구식 PCI 장치는 MPS 개념이 없어 최솟값 계산에 영향을 주지 않는다 */

	/*
	 * We don't have a way to change MPS settings on devices that have
	 * drivers attached.  A hot-added device might support only the minimum
	 * MPS setting (MPS=128).  Therefore, if the fabric contains a bridge
	 * where devices may be hot-added, we limit the fabric MPS to 128 so
	 * hot-added devices will work correctly.
	 *
	 * However, if we hot-add a device to a slot directly below a Root
	 * Port, it's impossible for there to be other existing devices below
	 * the port.  We don't limit the MPS in this case because we can
	 * reconfigure MPS on both the Root Port and the hot-added device,
	 * and there are no other devices involved.
	 *
	 * Note that this PCIE_BUS_SAFE path assumes no peer-to-peer DMA.
	 */
	if (dev->is_hotplug_bridge &&
	    pci_pcie_type(dev) != PCI_EXP_TYPE_ROOT_PORT)
		*smpss = 0;
	/* [한국어] 핫플러그 가능 브리지인데 Root Port 는 아니다 =
	 * 그 아래에 이미 다른 장치가 있을 수 있는 스위치 하향 포트다.
	 * 나중에 128B 만 지원하는 장치가 꽂혀도 기존 장치들의 MPS 를 바꿀 수
	 * 없으므로, 미리 최소값(코드 0 = 128바이트)으로 못박는다. */

	if (*smpss > dev->pcie_mpss)
		*smpss = dev->pcie_mpss;
	/* [한국어] 이 장치가 더 작은 값만 지원하면 최솟값을 갱신한다.
	 * 순회가 끝나면 *smpss 는 계층 전체가 공통으로 감당 가능한 값이 된다. */

	return 0;
	/* [한국어] 0 을 돌려 순회를 계속한다 */
}

/*
 * [한국어]
 * pcie_write_mps - 정책에 맞는 MPS 값을 장치에 기록한다
 *
 * @dev: 대상 장치.
 * @mps: 기본으로 쓸 MPS(바이트). PERFORMANCE 모드에서는 이 값을 무시하고
 *       다시 계산한다.
 * @return: 없음. 실패는 오류 로그로만 알린다.
 *
 * 왜 정책에 따라 계산이 다른가:
 *  - SAFE / PEER2PEER : 호출자가 이미 계층 최솟값을 구해 넘겨 주므로 그대로 쓴다.
 *  - PERFORMANCE      : 각 장치가 낼 수 있는 최대치를 쓰되, 부모 브리지의
 *                       MPS 를 넘을 수는 없다.
 *
 * PERFORMANCE 모드의 논리: 아래 영어 주석이 설명한다. 하향(호스트 → 장치)
 * 트래픽은 MRRS 를 넘지 않으므로 걱정할 필요가 없고, 상향(장치 → 호스트)
 * 트래픽만 MPS 제약을 받는다. 그래서 트리를 위에서 아래로 훑으며 자식의
 * MPS 를 부모 값 이하로 맞추면 된다. pci_walk_bus 가 위에서 아래 순서를
 * 보장하므로 이 시점에 부모는 이미 설정이 끝나 있다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   pcie_bus_configure_set() → [pcie_write_mps] → pcie_set_mps()
 */
static void pcie_write_mps(struct pci_dev *dev, int mps)
{
	int rc;
	/* [한국어] pcie_set_mps() 결과 */

	if (pcie_bus_config == PCIE_BUS_PERFORMANCE) {
		/* [한국어] 성능 우선 정책 — 계층 최솟값이 아니라 각자의 최대치를 쓴다 */
		mps = 128 << dev->pcie_mpss;
		/* [한국어] 이 장치가 지원하는 최대 MPS(바이트). MPSS 는 지수 코드다 */

		if (pci_pcie_type(dev) != PCI_EXP_TYPE_ROOT_PORT &&
		    dev->bus->self)

			/*
			 * For "Performance", the assumption is made that
			 * downstream communication will never be larger than
			 * the MRRS.  So, the MPS only needs to be configured
			 * for the upstream communication.  This being the case,
			 * walk from the top down and set the MPS of the child
			 * to that of the parent bus.
			 *
			 * Configure the device MPS with the smaller of the
			 * device MPSS or the bridge MPS (which is assumed to be
			 * properly configured at this point to the largest
			 * allowable MPS based on its parent bus).
			 */
			mps = min(mps, pcie_get_mps(dev->bus->self));
		/* [한국어] Root Port 가 아니고 상위 브리지가 있는 장치는, 자기
		 * 최대치와 부모의 현재 MPS 중 작은 값을 쓴다. 부모보다 큰 MPS 로
		 * 보내면 부모가 Malformed TLP 로 판정하기 때문이다.
		 * Root Port 는 부모가 루트 컴플렉스라 이 제약이 없다. */
	}

	rc = pcie_set_mps(dev, mps);
	/* [한국어] Device Control 레지스터의 MPS 필드에 기록 */
	if (rc)
		pci_err(dev, "Failed attempting to set the MPS\n");
	/* [한국어] 실패해도 되돌리지 않고 오류만 남긴다. 이 경우 기존 값이
	 * 유지되므로 동작은 하되 성능이 기대와 다를 수 있다. */
}

/*
 * [한국어]
 * pcie_write_mrrs - Max Read Request Size 를 가능한 최대로 올린다
 *
 * @dev: 대상 장치.
 * @return: 없음.
 *
 * MRRS 란: 이 장치가 메모리 읽기 요청 하나로 몇 바이트까지 요구할 수
 * 있는지의 상한이다. MPS 가 "한 패킷의 데이터 크기"라면, MRRS 는
 * "한 요청으로 받아 올 총량"이다. MRRS 가 크면 같은 양의 데이터를 더
 * 적은 수의 요청으로 가져올 수 있어 오버헤드가 줄어든다.
 *
 * NVMe 접점: NVMe 는 PRP/SGL 로 큰 DMA 읽기를 하므로 MRRS 가 실효 처리량에
 * 직접 영향을 준다.
 *
 * SAFE 모드에서 건드리지 않는 이유: 아래 영어 주석대로 MRRS 를 0(=128B)으로
 * 설정했을 때 오작동하는 장치가 여럿 알려져 있다. 안전을 우선하는 정책에서는
 * 아예 손대지 않는 편이 낫다.
 *
 * 축소 재시도 루프: MRRS 레지스터는 읽기/쓰기가 가능하지만, 하드웨어가
 * 받아들이지 못하는 값을 쓰면 조용히 무시할 수 있다. 그래서 쓴 뒤 되읽어
 * 확인하고, 어긋나면 절반으로 줄여 다시 시도한다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   pcie_bus_configure_set() → [pcie_write_mrrs] → pcie_set_readrq()
 */
static void pcie_write_mrrs(struct pci_dev *dev)
{
	int rc, mrrs;
	/* [한국어] rc = 쓰기 결과, mrrs = 시도할 값(바이트) */

	/*
	 * In the "safe" case, do not configure the MRRS.  There appear to be
	 * issues with setting MRRS to 0 on a number of devices.
	 */
	if (pcie_bus_config != PCIE_BUS_PERFORMANCE)
		return;
	/* [한국어] 성능 정책일 때만 MRRS 를 조정한다 */

	/*
	 * For max performance, the MRRS must be set to the largest supported
	 * value.  However, it cannot be configured larger than the MPS the
	 * device or the bus can support.  This should already be properly
	 * configured by a prior call to pcie_write_mps().
	 */
	mrrs = pcie_get_mps(dev);
	/* [한국어] 시작값은 방금 설정된 MPS. 영어 주석대로 MRRS 가 MPS 를
	 * 넘어설 수 없으므로, 바로 앞에서 pcie_write_mps() 가 정해 놓은 값이
	 * 곧 시도할 수 있는 최대치다. */

	/*
	 * MRRS is a R/W register.  Invalid values can be written, but a
	 * subsequent read will verify if the value is acceptable or not.
	 * If the MRRS value provided is not acceptable (e.g., too large),
	 * shrink the value until it is acceptable to the HW.
	 */
	while (mrrs != pcie_get_readrq(dev) && mrrs >= 128) {
		/* [한국어] 되읽은 값이 원하는 값과 다르고, 아직 128B 이상 남아
		 * 있으면 계속 시도한다. mrrs 가 매번 절반이 되므로 루프는
		 * 반드시 끝난다(로그 스케일). */
		rc = pcie_set_readrq(dev, mrrs);
		/* [한국어] Device Control 의 MRRS 필드에 기록 시도 */
		if (!rc)
			break;
		/* [한국어] 쓰기 자체가 성공하면 종료. (루프 조건의 되읽기 비교는
		 * 하드웨어가 값을 조용히 거부하는 경우를 잡기 위한 것이다.) */

		pci_warn(dev, "Failed attempting to set the MRRS\n");
		/* [한국어] 이번 시도가 실패했음을 알린다 */
		mrrs /= 2;
		/* [한국어] 절반으로 줄여 다시 시도. 128 → 64 가 되면 루프 조건에
		 * 걸려 빠져나간다. */
	}

	if (mrrs < 128)
		pci_err(dev, "MRRS was unable to be configured with a safe value.  If problems are experienced, try running with pci=pcie_bus_safe\n");
	/* [한국어] 128B 아래까지 내려갔는데도 실패했다 = 어떤 값도 받아들여지지
	 * 않았다. 사용자에게 pci=pcie_bus_safe 로 이 조정을 아예 끄는 방법을 안내한다. */
}

/*
 * [한국어]
 * pcie_bus_configure_set - 장치 하나에 MPS/MRRS 를 적용하는 pci_walk_bus 콜백
 *
 * @dev:  순회 중 만난 장치.
 * @data: u8 * — 적용할 MPSS 코드(계층 최솟값 또는 0).
 * @return: 항상 0(순회 계속).
 *
 * 왜 필요한가: 계층 최솟값을 구한 뒤(pcie_find_smpss) 그것을 실제로 적용하는
 * 두 번째 순회의 본체다. 정책이 TUNE_OFF 나 DEFAULT 면 아무 일도 하지 않는데,
 * DEFAULT 정책에서는 이미 pci_configure_mps() 가 장치 발견 시점에 처리했기
 * 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥, pci_walk_bus() 순회 안.
 *
 * 호출 체인:
 *   pcie_bus_configure_settings() → pci_walk_bus() → [pcie_bus_configure_set]
 *     → pcie_write_mps(), pcie_write_mrrs()
 */
static int pcie_bus_configure_set(struct pci_dev *dev, void *data)
{
	int mps, orig_mps;
	/* [한국어] mps = 적용할 목표값(바이트), orig_mps = 바꾸기 전 값(로그용) */

	if (!pci_is_pcie(dev))
		return 0;
	/* [한국어] 구식 PCI 장치는 대상이 아니다 */

	if (pcie_bus_config == PCIE_BUS_TUNE_OFF ||
	    pcie_bus_config == PCIE_BUS_DEFAULT)
		return 0;
	/* [한국어] TUNE_OFF 는 사용자가 조정을 금지한 것이고,
	 * DEFAULT 는 장치 발견 시점의 pci_configure_mps() 가 이미 처리했다.
	 * 남는 것은 SAFE / PERFORMANCE / PEER2PEER 세 정책이다. */

	mps = 128 << *(u8 *)data;
	/* [한국어] 넘겨받은 MPSS 코드를 바이트 수로 바꾼다.
	 * PEER2PEER 나 핫플러그 제약이 걸렸으면 코드가 0 이라 128B 가 된다. */
	orig_mps = pcie_get_mps(dev);
	/* [한국어] 변경 전 값을 로그에 남기기 위해 저장 */

	pcie_write_mps(dev, mps);
	/* [한국어] MPS 적용(PERFORMANCE 모드면 내부에서 다시 계산한다) */
	pcie_write_mrrs(dev);
	/* [한국어] MRRS 적용. 반드시 MPS 설정 뒤여야 한다 — MRRS 의 시작값이
	 * 방금 설정된 MPS 이기 때문이다. */

	pci_info(dev, "Max Payload Size set to %4d/%4d (was %4d), Max Read Rq %4d\n",
		 pcie_get_mps(dev), 128 << dev->pcie_mpss,
		 orig_mps, pcie_get_readrq(dev));
	/* [한국어] "설정값/최대가능값 (이전값), 읽기요청크기" 형식으로 남긴다.
	 * NVMe 성능 진단 시 이 줄로 MPS 가 128 에 묶여 있는지 확인할 수 있다. */

	return 0;
	/* [한국어] 순회 계속 */
}

/*
 * pcie_bus_configure_settings() requires that pci_walk_bus work in a top-down,
 * parents then children fashion.  If this changes, then this code will not
 * work as designed.
 */
/*
 * [한국어]
 * pcie_bus_configure_settings - 한 버스 아래 전체의 MPS/MRRS 정책을 적용한다
 *
 * @bus: 설정을 적용할 버스.
 * @return: 없음.
 *
 * 왜 필요한가: MPS 는 계층 전체가 정합해야 하는 값이라, 장치를 하나씩
 * 발견할 때마다 정할 수 없는 정책이 있다(SAFE 는 최솟값을 알아야 하고,
 * PERFORMANCE 는 부모가 먼저 정해져야 한다). 그래서 열거가 끝난 뒤 이
 * 함수가 트리를 훑으며 일괄 적용한다.
 *
 * ★ 순서 의존성 ★ 위 영어 주석이 못박아 두었듯이, 이 코드는
 * pci_walk_bus() 가 "부모 먼저, 자식 나중" 순서로 순회한다는 것에 의존한다.
 * PERFORMANCE 모드에서 자식의 MPS 를 부모 값에 맞추므로, 부모가 먼저
 * 설정되어 있지 않으면 잘못된 값이 전파된다.
 *
 * 두 번 순회하는 이유: SAFE 모드에서는 (1) 최솟값을 구하는 순회와
 * (2) 그 값을 적용하는 순회가 분리되어야 한다. 한 번에 하면 아직 만나지
 * 않은 장치가 더 작은 값을 요구할 때 이미 설정한 장치를 되돌릴 수 없다.
 *
 * bus->self 를 따로 처리하는 이유: pci_walk_bus() 는 bus 아래의 장치들만
 * 훑고 그 버스로 내려오는 브리지 자신은 포함하지 않는다. 브리지도 링크의
 * 한쪽 끝이므로 함께 설정해야 하며, 위에서 아래 순서를 지키려면 반드시
 * 먼저 처리해야 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 열거 완료 후.
 *
 * 호출 체인:
 *   pci_host_probe()/pci_scan_root_bus() → [pcie_bus_configure_settings]
 *     → pci_walk_bus(pcie_find_smpss), pci_walk_bus(pcie_bus_configure_set)
 */
void pcie_bus_configure_settings(struct pci_bus *bus)
{
	u8 smpss = 0;
	/* [한국어] 적용할 MPSS 코드. 0 은 128바이트를 뜻하며, PEER2PEER 나
	 * 정책이 없을 때의 안전한 기본값이다. */

	if (!bus->self)
		return;
	/* [한국어] 루트 버스에는 이 버스로 내려오는 브리지가 없다.
	 * 기준으로 삼을 링크가 없으므로 할 일이 없다. */

	if (!pci_is_pcie(bus->self))
		return;
	/* [한국어] MPS/MRRS 는 PCIe 개념이다 */

	/*
	 * FIXME - Peer to peer DMA is possible, though the endpoint would need
	 * to be aware of the MPS of the destination.  To work around this,
	 * simply force the MPS of the entire system to the smallest possible.
	 */
	if (pcie_bus_config == PCIE_BUS_PEER2PEER)
		smpss = 0;
	/* [한국어] 장치끼리 직접 DMA 를 주고받으려면 서로의 MPS 를 알아야 하는데
	 * 그럴 방법이 없다. 위 영어 주석의 FIXME 대로, 모든 장치를 최소값
	 * 128B 로 통일해 어떤 조합이든 통신 가능하게 만드는 것이 현재의 해법이다. */

	if (pcie_bus_config == PCIE_BUS_SAFE) {
		/* [한국어] 계층 전체의 공통 최솟값을 구해 쓰는 정책 */
		smpss = bus->self->pcie_mpss;
		/* [한국어] 브리지 자신의 지원값에서 시작한다 */

		pcie_find_smpss(bus->self, &smpss);
		/* [한국어] 브리지 자신도 검사 대상에 넣는다. pci_walk_bus 가
		 * 브리지를 포함하지 않으므로 직접 불러 준다. 여기서 브리지가
		 * 핫플러그 가능하면 smpss 가 0 으로 못박힌다. */
		pci_walk_bus(bus, pcie_find_smpss, &smpss);
		/* [한국어] 이 버스 아래 모든 장치를 훑어 최솟값을 확정한다 */
	}

	pcie_bus_configure_set(bus->self, &smpss);
	/* [한국어] 확정된 값을 브리지에 먼저 적용한다. 위에서 아래 순서를
	 * 지켜야 PERFORMANCE 모드의 부모-자식 전파가 올바르게 동작한다. */
	pci_walk_bus(bus, pcie_bus_configure_set, &smpss);
	/* [한국어] 이어서 아래 모든 장치에 적용한다. NVMe SSD 의 최종 MPS/MRRS
	 * 가 이 순회에서 확정된다. */
}
EXPORT_SYMBOL_GPL(pcie_bus_configure_settings);
/* [한국어] 일부 호스트 컨트롤러 드라이버가 모듈에서 호출하므로 공개 */

/*
 * Called after each bus is probed, but before its children are examined.  This
 * is marked as __weak because multiple architectures define it.
 */
/*
 * [한국어]
 * pcibios_fixup_bus - 아키텍처별 버스 보정 훅(기본 구현은 비어 있음)
 *
 * @bus: 방금 스캔한 버스.
 * @return: 없음.
 *
 * 왜 필요한가: 예전에는 아키텍처마다 버스 스캔 직후 손봐야 할 것이 있어
 * 이 훅을 두었다. __weak 이므로 아키텍처가 같은 이름의 함수를 정의하면
 * 그쪽이 링크된다. 아래 영어 주석대로 상류에서는 이 훅을 없앨 계획이다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 버스 스캔 경로.
 *
 * 호출 체인:
 *   pci_scan_child_bus_extend() → [pcibios_fixup_bus]
 */
void __weak pcibios_fixup_bus(struct pci_bus *bus)
{
       /* nothing to do, expected to be removed in the future */
       /* [한국어] 기본 구현은 아무 일도 하지 않는다. 영어 주석이 밝히듯
	* 장래에 제거될 예정인 훅이다. */
}

/**
 * pci_scan_child_bus_extend() - Scan devices below a bus
 * @bus: Bus to scan for devices
 * @available_buses: Total number of buses available (%0 does not try to
 *		     extend beyond the minimal)
 *
 * Scans devices below @bus including subordinate buses. Returns new
 * subordinate number including all the found devices. Passing
 * @available_buses causes the remaining bus space to be distributed
 * equally between hotplug-capable bridges to allow future extension of the
 * hierarchy.
 */
/*
 * [한국어]
 * pci_scan_child_bus_extend - 한 버스의 모든 슬롯과 그 아래 브리지들을 훑는다
 *
 * @bus:             훑을 버스.
 * @available_buses: 이 버스 아래에 여유로 쓸 수 있는 버스 번호 개수.
 *                   0 이면 꼭 필요한 만큼만 쓴다.
 * @return: 이 버스와 그 아래 전체를 포함하는 최대 버스 번호(subordinate).
 *
 * ★ 재귀 하강의 다른 한쪽 ★
 * pci_scan_bridge_extend() 와 서로를 호출하며 PCI 트리 전체를 깊이 우선으로
 * 훑는다. 이 함수가 하는 일은 세 가지다:
 *   1) device 0~31 각각에 대해 pci_scan_slot() 을 불러 장치를 발견한다.
 *   2) 발견된 브리지들에 대해 2-pass 로 버스 번호를 배정하고 아래로 하강한다.
 *   3) 남은 버스 번호를 핫플러그 브리지들에게 나눠 준다.
 *
 * 왜 여유 버스를 나눠 주는가: 핫플러그 슬롯에 나중에 스위치가 달린 장치가
 * 꽂히면 버스 번호가 여러 개 필요하다. 그때 가서 번호를 만들려면 이미 배정된
 * 이웃 브리지들의 번호를 전부 옮겨야 하는데, 동작 중인 시스템에서는 사실상
 * 불가능하다. 그래서 지금 미리 빈 번호를 남겨 둔다.
 *
 * NVMe 접점: NVMe SSD 가 실제로 발견되는 지점이 이 함수의
 * pci_scan_slot() 루프다. 백플레인의 핫플러그 베이라면 여유 버스 분배가
 * 나중에 SSD 를 꽂았을 때 정상 인식되는지를 좌우한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 재귀 깊이는 PCI 트리 깊이(현실적으로 한 자릿수).
 *
 * 호출 체인:
 *   pci_scan_child_bus()/pci_scan_bridge_extend() → [pci_scan_child_bus_extend]
 *     → pci_scan_slot(), pci_scan_bridge_extend() → …(상호 재귀)
 */
static unsigned int pci_scan_child_bus_extend(struct pci_bus *bus,
					      unsigned int available_buses)
{
	unsigned int used_buses, normal_bridges = 0, hotplug_bridges = 0;
	/* [한국어] used_buses      = 지금까지 이 버스 아래에서 소비한 번호 개수,
	 * normal_bridges  = 이 버스의 일반 브리지 수,
	 * hotplug_bridges = 이 버스의 핫플러그 가능 브리지 수(여유분을 나눌 대상). */
	unsigned int start = bus->busn_res.start;
	/* [한국어] 이 버스 자신의 번호. 아래에서 "얼마나 늘었는가"를 재는 기준점 */
	unsigned int devnr, cmax, max = start;
	/* [한국어] devnr = 슬롯 순회 인덱스,
	 * cmax  = 한 브리지를 처리하기 직전의 max(증가분 계산용),
	 * max   = 지금까지 쓰인 최대 버스 번호(반환값이 된다). */
	struct pci_dev *dev;
	/* [한국어] 브리지 순회 커서 */

	dev_dbg(&bus->dev, "scanning bus\n");
	/* [한국어] 스캔 시작 디버그 로그 */

	/* Go find them, Rover! */
	for (devnr = 0; devnr < PCI_MAX_NR_DEVS; devnr++)
		pci_scan_slot(bus, PCI_DEVFN(devnr, 0));
	/* [한국어] ★장치 발견 루프★ device 번호 0~31(PCI_MAX_NR_DEVS)을 모두
	 * 훑는다. PCI_DEVFN(devnr, 0) 은 (devnr << 3) | 0 으로 각 슬롯의
	 * function 0 좌표를 만든다. pci_scan_slot() 이 그 안의 function 들을
	 * 처리한다. PCIe 하향 포트 아래라면 only_one_child() 덕분에 devnr 0 만
	 * 실제로 검사되고 나머지는 즉시 반환한다.
	 * NVMe SSD 는 이 루프에서 발견된다. */

	/* Reserve buses for SR-IOV capability */
	used_buses = pci_iov_bus_range(bus);
	/* [한국어] 이 버스의 장치들이 SR-IOV VF 를 만들 때 필요한 버스 번호
	 * 개수를 계산한다. VF 는 PF 와 다른 버스 번호에 나타날 수 있어
	 * 미리 자리를 비워 두어야 한다. */
	max += used_buses;
	/* [한국어] 그만큼 최대 번호를 밀어 둔다. 실제로 VF 를 만들 때
	 * 그 번호들이 쓰인다. */

	/*
	 * After performing arch-dependent fixup of the bus, look behind
	 * all PCI-to-PCI bridges on this bus.
	 */
	if (!bus->is_added) {
		/* [한국어] 이 버스에 대해 아직 fixup 을 하지 않았을 때만.
		 * 재스캔에서 중복 실행되는 것을 막는다. */
		dev_dbg(&bus->dev, "fixups for bus\n");
		pcibios_fixup_bus(bus);
		/* [한국어] 아키텍처별 보정 훅(기본은 빈 함수) */
		bus->is_added = 1;
		/* [한국어] 완료 표시 */
	}

	/*
	 * Calculate how many hotplug bridges and normal bridges there
	 * are on this bus. We will distribute the additional available
	 * buses between hotplug bridges.
	 */
	for_each_pci_bridge(dev, bus) {
		/* [한국어] 이 버스의 장치 중 브리지만 골라 순회하는 매크로 */
		if (dev->is_hotplug_bridge)
			hotplug_bridges++;
		/* [한국어] 나중에 여유 버스를 나눠 받을 대상 */
		else
			normal_bridges++;
		/* [한국어] 여유 버스가 필요 없는 고정 브리지 */
	}

	/*
	 * Scan bridges that are already configured. We don't touch them
	 * unless they are misconfigured (which will be done in the second
	 * scan below).
	 */
	for_each_pci_bridge(dev, bus) {
		/* [한국어] ★1차 순회(pass 0)★ 펌웨어가 이미 번호를 넣어 둔
		 * 브리지들을 먼저 처리해 그 번호들을 max 에 반영한다.
		 * 이렇게 해야 2차에서 새 번호를 매길 때 충돌하지 않는다. */
		cmax = max;
		/* [한국어] 이 브리지 처리 전의 값을 기억 */
		max = pci_scan_bridge_extend(bus, dev, max, 0, 0);
		/* [한국어] pass=0, available_buses=0 으로 호출.
		 * 이미 설정된 브리지만 실제로 처리되고, 설정이 필요한 브리지는
		 * 아무 일도 하지 않고 돌아온다(그쪽은 2차에서 처리). */

		/*
		 * Reserve one bus for each bridge now to avoid extending
		 * hotplug bridges too much during the second scan below.
		 */
		used_buses++;
		/* [한국어] 브리지 하나당 최소 버스 하나는 반드시 필요하다.
		 * 영어 주석대로 이것을 지금 예약해 두어야, 2차에서 핫플러그
		 * 브리지에게 여유분을 나눠 줄 때 아직 처리되지 않은 브리지들의
		 * 몫까지 가져가 버리는 일이 없다. */
		if (max - cmax > 1)
			used_buses += max - cmax - 1;
		/* [한국어] 이 브리지가 실제로 두 개 이상의 번호를 썼다면
		 * (아래에 또 브리지가 있어서) 그 추가분도 소비량에 반영한다.
		 * 하나는 위에서 이미 셌으므로 -1 한다. */
	}

	/* Scan bridges that need to be reconfigured */
	for_each_pci_bridge(dev, bus) {
		/* [한국어] ★2차 순회(pass 1)★ 이제 기존 번호를 모두 파악했으므로
		 * 새 번호를 안전하게 배정할 수 있다. */
		unsigned int buses = 0;
		/* [한국어] 이 브리지에게 넘겨 줄 여유 버스 개수 */

		if (!hotplug_bridges && normal_bridges == 1) {
			/*
			 * There is only one bridge on the bus (upstream
			 * port) so it gets all available buses which it
			 * can then distribute to the possible hotplug
			 * bridges below.
			 */
			buses = available_buses;
			/* [한국어] 이 버스에 브리지가 딱 하나뿐이라면(전형적으로
			 * 스위치의 상향 포트), 나눌 상대가 없으므로 여유분을 통째로
			 * 넘긴다. 그 브리지가 아래로 내려가 자기 하향 포트들에게
			 * 다시 나눠 준다. */
		} else if (dev->is_hotplug_bridge) {
			/*
			 * Distribute the extra buses between hotplug
			 * bridges if any.
			 */
			buses = available_buses / hotplug_bridges;
			/* [한국어] 여유분을 핫플러그 브리지 수로 나눠 균등 분배 */
			buses = min(buses, available_buses - used_buses + 1);
			/* [한국어] 다만 이미 소비한 만큼은 빼야 한다. 앞선 브리지들이
			 * 예상보다 많이 썼다면 뒤쪽 브리지의 몫이 줄어든다.
			 * +1 은 위 1차 순회에서 이 브리지 몫으로 미리 하나를
			 * used_buses 에 더해 두었기 때문에 그것을 되돌리는 보정이다. */
		}

		cmax = max;
		/* [한국어] 처리 전 값 기억 */
		max = pci_scan_bridge_extend(bus, dev, cmax, buses, 1);
		/* [한국어] pass=1 로 호출 — 여기서 실제로 번호가 배정되고
		 * 아래로 재귀 하강이 일어난다. 여유분 buses 도 함께 넘긴다. */
		/* One bus is already accounted so don't add it again */
		if (max - cmax > 1)
			used_buses += max - cmax - 1;
		/* [한국어] 1차와 같은 계산. 영어 주석대로 버스 하나는 이미
		 * 셌으므로 중복해서 더하지 않는다. */
	}

	/*
	 * Make sure a hotplug bridge has at least the minimum requested
	 * number of buses but allow it to grow up to the maximum available
	 * bus number if there is room.
	 */
	if (bus->self && bus->self->is_hotplug_bridge) {
		/* [한국어] 이 버스 자신이 핫플러그 브리지 아래에 있는 경우.
		 * 지금 아래가 비어 있어도 나중에 장치가 꽂힐 수 있으므로
		 * 번호 구간을 미리 넓혀 둔다. */
		used_buses = max(available_buses, pci_hotplug_bus_size - 1);
		/* [한국어] 호출자가 준 여유분과 커널 기본 예약치
		 * (pci_hotplug_bus_size, 커널 파라미터로 조정 가능) 중 큰 쪽을 쓴다.
		 * -1 은 자기 자신의 번호를 제외한 하위 구간 크기이기 때문이다. */
		if (max - start < used_buses) {
			/* [한국어] 실제로 쓴 범위가 예약하려는 크기보다 작으면 넓힌다 */
			max = start + used_buses;
			/* [한국어] 시작 번호에 예약 크기를 더해 새 상한을 만든다 */

			/* Do not allocate more buses than we have room left */
			if (max > bus->busn_res.end)
				max = bus->busn_res.end;
			/* [한국어] 부모가 허용한 구간을 넘어설 수는 없다.
			 * 넘으면 조상 브리지가 그 번호의 요청을 전달하지 않는다. */

			dev_dbg(&bus->dev, "%pR extended by %#02x\n",
				&bus->busn_res, max - start);
			/* [한국어] 얼마나 넓혔는지 디버그 로그로 남긴다 */
		}
	}

	/*
	 * We've scanned the bus and so we know all about what's on
	 * the other side of any bridges that may be on this bus plus
	 * any devices.
	 *
	 * Return how far we've got finding sub-buses.
	 */
	dev_dbg(&bus->dev, "bus scan returning with max=%02x\n", max);
	/* [한국어] 이 서브트리가 소비한 최대 번호를 로그로 남긴다 */
	return max;
	/* [한국어] 호출자(pci_scan_bridge_extend)가 이 값을 브리지의
	 * subordinate 로 프로그래밍하거나, 다음 브리지 처리에 이어 쓴다. */
}

/**
 * pci_scan_child_bus() - Scan devices below a bus
 * @bus: Bus to scan for devices
 *
 * Scans devices below @bus including subordinate buses. Returns new
 * subordinate number including all the found devices.
 */
/*
 * [한국어]
 * pci_scan_child_bus - 여유 버스 없이 버스 아래를 훑는 공개 API
 *
 * @bus: 훑을 버스.
 * @return: 이 버스 아래 전체를 포함하는 최대 버스 번호.
 *
 * 왜 필요한가: pci_scan_child_bus_extend() 의 available_buses 를 0 으로
 * 고정한 래퍼다. 핫플러그 여유분을 남기지 않는 가장 단순한 형태이며,
 * 부팅 시 루트 버스 스캔의 기본 경로다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_scan_root_bus_bridge()/pci_scan_bus() → [pci_scan_child_bus]
 *     → pci_scan_child_bus_extend()
 */
unsigned int pci_scan_child_bus(struct pci_bus *bus)
{
	return pci_scan_child_bus_extend(bus, 0);
	/* [한국어] 여유 버스 0 — 꼭 필요한 만큼만 배정한다 */
}
EXPORT_SYMBOL_GPL(pci_scan_child_bus);
/* [한국어] 호스트 컨트롤러/핫플러그 드라이버가 모듈에서 쓰므로 공개 */

/**
 * pcibios_root_bridge_prepare - Platform-specific host bridge setup
 * @bridge: Host bridge to set up
 *
 * Default empty implementation.  Replace with an architecture-specific setup
 * routine, if necessary.
 */
/*
 * [한국어]
 * pcibios_root_bridge_prepare - 루트 브리지 등록 직전의 아키텍처 훅
 *
 * @bridge: 준비할 호스트 브리지.
 * @return: 0 이면 계속 진행. 음수면 pci_register_host_bridge() 가 실패한다.
 *
 * 왜 필요한가: 루트 브리지를 드라이버 모델에 올리기 전에 아키텍처가 손봐야
 * 할 것(NUMA 노드, ACPI 정보 연결 등)이 있을 수 있다. __weak 이므로
 * 아키텍처가 같은 이름의 강한 심볼을 정의하면 그쪽이 링크된다.
 *
 * 실행 컨텍스트: 프로세스 문맥, device_add() 전.
 *
 * 호출 체인:
 *   pci_register_host_bridge() → [pcibios_root_bridge_prepare]
 */
int __weak pcibios_root_bridge_prepare(struct pci_host_bridge *bridge)
{
	return 0;
	/* [한국어] 기본 구현은 할 일이 없으므로 성공만 알린다 */
}

/*
 * [한국어]
 * pcibios_add_bus - 버스가 등록된 직후의 아키텍처 훅
 *
 * @bus: 방금 등록된 버스.
 * @return: 없음.
 *
 * 왜 필요한가: 아키텍처가 버스별 자료구조를 만들거나 펌웨어에 알려야 하는
 * 경우를 위한 자리다. 기본 구현은 비어 있다.
 *
 * 실행 컨텍스트: 프로세스 문맥, device_register() 직후.
 *
 * 호출 체인:
 *   pci_register_host_bridge()/pci_alloc_child_bus() → [pcibios_add_bus]
 */
void __weak pcibios_add_bus(struct pci_bus *bus)
{
	/* [한국어] 기본 구현 없음 */
}

/*
 * [한국어]
 * pcibios_remove_bus - 버스가 제거되기 직전의 아키텍처 훅
 *
 * @bus: 제거될 버스.
 * @return: 없음.
 *
 * 왜 필요한가: pcibios_add_bus() 에서 만든 것을 되돌리는 짝이다.
 * 기본 구현은 비어 있다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 버스 제거 경로(drivers/pci/remove.c).
 *
 * 호출 체인:
 *   pci_remove_bus() → [pcibios_remove_bus]
 */
void __weak pcibios_remove_bus(struct pci_bus *bus)
{
	/* [한국어] 기본 구현 없음 */
}

/*
 * [한국어]
 * pci_create_root_bus - 호스트 브리지를 만들어 루트 버스를 생성하는 구식 API
 *
 * @parent:    호스트 컨트롤러 device(없으면 NULL).
 * @bus:       루트 버스 번호.
 * @ops:       이 버스의 config 접근 함수 집합.
 * @sysdata:   컨트롤러 전용 데이터 포인터.
 * @resources: 이 브리지가 제공하는 주소 창 목록. 성공하면 이 목록은
 *             브리지로 옮겨져 비워진다.
 * @return: 생성된 루트 버스. 실패 시 NULL.
 *
 * 왜 있는가: pci_alloc_host_bridge() + 필드 설정 + pci_register_host_bridge()
 * 를 한 번에 하는 편의 함수다. 브리지 객체를 직접 다루지 않는 오래된
 * 컨트롤러 드라이버들이 쓴다. 새 드라이버는 devm_pci_alloc_host_bridge() 와
 * pci_host_probe() 조합을 쓰는 것이 권장된다.
 *
 * 실행 컨텍스트: 프로세스 문맥의 드라이버 probe.
 *
 * 호출 체인:
 *   호스트 컨트롤러 드라이버 → [pci_create_root_bus]
 *     → pci_alloc_host_bridge(), pci_register_host_bridge()
 */
struct pci_bus *pci_create_root_bus(struct device *parent, int bus,
		struct pci_ops *ops, void *sysdata, struct list_head *resources)
{
	int error;
	/* [한국어] 등록 결과 코드 */
	struct pci_host_bridge *bridge;
	/* [한국어] 만들 호스트 브리지 */

	bridge = pci_alloc_host_bridge(0);
	/* [한국어] 드라이버 전용 영역 없이(priv=0) 브리지만 할당 */
	if (!bridge)
		return NULL;
	/* [한국어] 메모리 부족 */

	bridge->dev.parent = parent;
	/* [한국어] sysfs 계층에서의 부모 */

	list_splice_init(resources, &bridge->windows);
	/* [한국어] 호출자가 준비한 창 목록을 브리지로 통째로 옮기고 원본은 비운다.
	 * 이제 그 리소스들의 수명은 브리지가 관리한다. */
	bridge->sysdata = sysdata;
	/* [한국어] config 접근 함수가 쓸 컨트롤러 전용 데이터 */
	bridge->busnr = bus;
	/* [한국어] 루트 버스 번호 */
	bridge->ops = ops;
	/* [한국어] config 읽기/쓰기 함수 집합 */

	error = pci_register_host_bridge(bridge);
	/* [한국어] 실제 등록. 여기서 pci_bus 가 만들어지고 창들이 버스 리소스로
	 * 옮겨지며 전역 루트 버스 목록에 들어간다. */
	if (error < 0)
		goto err_out;
	/* [한국어] 등록 실패 */

	return bridge->bus;
	/* [한국어] 만들어진 루트 버스를 돌려준다. 호출자는 이어서
	 * pci_scan_child_bus() 로 장치를 훑는다. */

err_out:
	put_device(&bridge->dev);
	/* [한국어] pci_alloc_host_bridge() 안의 device_initialize() 로 잡힌
	 * 참조를 놓는다. 마지막 참조라면 소멸자가 돌아 브리지가 해제된다.
	 * (list_splice_init 으로 옮긴 창 목록도 소멸자가 정리한다.) */
	return NULL;
}
EXPORT_SYMBOL_GPL(pci_create_root_bus);
/* [한국어] 구식 컨트롤러 드라이버들이 모듈에서 쓰므로 공개 */

/*
 * [한국어]
 * pci_host_probe - 호스트 브리지 하나에 대한 열거·리소스 할당·드라이버 바인딩 전 과정
 *
 * @bridge: 컨트롤러 드라이버가 준비한 호스트 브리지.
 * @return: 0 이면 성공, 음수면 스캔 실패 errno.
 *
 * ★ 컨트롤러 드라이버 입장에서의 "한 방" 진입점 ★
 * 이 함수 하나를 부르면 그 아래 PCIe 트리 전체가 발견되고, 주소가 배정되고,
 * 드라이버가 붙는다. 단계는 다음과 같다:
 *   1) pci_scan_root_bus_bridge()  — 루트 버스 생성 + 트리 전체 재귀 스캔.
 *      이 단계가 끝나면 모든 pci_dev 가 만들어져 있지만, BAR 주소는
 *      아직 확정되지 않았을 수 있다(IORESOURCE_UNSET).
 *   2) 리소스 확정 — 펌웨어 설정을 보존할지에 따라 claim 또는 재배정.
 *   3) pcie_bus_configure_settings() — MPS/MRRS 정책 적용.
 *   4) pci_bus_add_devices()        — 드라이버 바인딩.
 *   5) runtime PM 준비.
 *
 * NVMe 접점: NVMe SSD 의 pci_dev 는 1)에서 만들어지고, BAR0 의 물리 주소는
 * 2)에서 확정되며, nvme_probe() 는 4)에서 호출된다. nvme_probe 가
 * pci_resource_start(pdev, 0) 으로 읽는 값이 2)에서 정해진 주소다.
 *
 * 실행 컨텍스트: 프로세스 문맥의 드라이버 probe. 스캔과 장치 추가 구간에서
 * pci_lock_rescan_remove() 로 다른 rescan/remove 와 상호 배제한다.
 *
 * 호출 체인:
 *   호스트 컨트롤러 드라이버 probe → [pci_host_probe]
 *     → pci_scan_root_bus_bridge() → pci_scan_child_bus() → …
 *     → pci_bus_add_devices() → 드라이버 probe(nvme_probe 등)
 */
int pci_host_probe(struct pci_host_bridge *bridge)
{
	struct pci_bus *bus, *child;
	/* [한국어] bus = 만들어진 루트 버스, child = 자식 버스 순회 커서 */
	int ret;
	/* [한국어] 스캔 결과 */

	pci_lock_rescan_remove();
	/* [한국어] 전역 rescan/remove 뮤텍스 획득. 스캔 도중 다른 경로에서
	 * 장치를 제거하거나 재스캔하면 트리가 깨지므로 직렬화한다. */
	ret = pci_scan_root_bus_bridge(bridge);
	/* [한국어] ★열거 전 과정★ 루트 버스를 만들고 그 아래를 재귀 스캔한다.
	 * 이 안에서 NVMe SSD 를 포함한 모든 장치의 pci_dev 가 만들어진다. */
	pci_unlock_rescan_remove();
	/* [한국어] 락 해제 */
	if (ret < 0) {
		dev_err(bridge->dev.parent, "Scanning root bridge failed");
		/* [한국어] 스캔 실패 — 이 컨트롤러 아래는 쓸 수 없다 */
		return ret;
	}

	bus = bridge->bus;
	/* [한국어] pci_register_host_bridge() 가 채워 준 루트 버스 */

	/* If we must preserve the resource configuration, claim now */
	if (bridge->preserve_config)
		pci_bus_claim_resources(bus);
	/* [한국어] 펌웨어 설정을 보존해야 하는 경우, 지금 그 주소들을 리소스
	 * 트리에 "이미 사용 중"으로 등록해 둔다. 그러면 아래의 재배정이
	 * 그 영역을 건드리지 않는다. */

	/*
	 * Assign whatever was left unassigned. If we didn't claim above,
	 * this will reassign everything.
	 */
	pci_assign_unassigned_root_bus_resources(bus);
	/* [한국어] ★BAR 주소 확정★ IORESOURCE_UNSET 으로 남아 있던 BAR 와
	 * 브리지 창에 실제 CPU 주소를 배정한다(drivers/pci/setup-bus.c).
	 * 위에서 claim 하지 않았다면 영어 주석대로 전부 새로 배정한다.
	 * 이 단계가 끝나야 NVMe 의 BAR0 이 실제 물리 주소를 갖는다. */

	list_for_each_entry(child, &bus->children, node)
		pcie_bus_configure_settings(child);
	/* [한국어] 루트 버스의 각 자식 버스에 대해 MPS/MRRS 정책을 적용한다.
	 * 트리 전체가 만들어진 뒤여야 계층 최솟값을 구할 수 있다. */

	pci_lock_rescan_remove();
	/* [한국어] 장치 추가 구간도 직렬화한다 */
	pci_bus_add_devices(bus);
	/* [한국어] ★드라이버 바인딩★ 트리의 모든 장치에 대해 device_attach()
	 * 를 수행한다. 여기서 nvme 드라이버가 클래스 0x010802 장치에 매칭되어
	 * nvme_probe() 가 실행되고, BAR0 매핑 → CAP 읽기 → Admin Queue 설정 →
	 * 네임스페이스 스캔을 거쳐 /dev/nvme0n1 이 나타난다. */
	pci_unlock_rescan_remove();
	/* [한국어] 락 해제 */

	/*
	 * Ensure pm_runtime_enable() is called for the controller drivers
	 * before calling pci_host_probe(). The PM framework expects that
	 * if the parent device supports runtime PM, it will be enabled
	 * before child runtime PM is enabled.
	 */
	pm_runtime_set_active(&bridge->dev);
	/* [한국어] 호스트 브리지를 "지금 동작 중"으로 표시한다. 자식들이
	 * runtime PM 을 켤 때 부모가 활성 상태여야 하기 때문이다. */
	pm_runtime_no_callbacks(&bridge->dev);
	/* [한국어] 브리지 객체 자체에는 절전 콜백이 없다고 알린다. 그러면
	 * PM 코어가 이 객체를 그냥 통과시켜 부모-자식 관계만 유지한다. */
	devm_pm_runtime_enable(&bridge->dev);
	/* [한국어] runtime PM 활성화. devm_ 접두어라 컨트롤러 device 가
	 * 사라질 때 자동으로 비활성화된다.
	 * 위 영어 주석은 순서 제약을 설명한다 — PM 프레임워크는 부모가 먼저
	 * 활성화되어 있을 것을 기대하므로, 자식 장치들이 붙은 뒤인 여기서
	 * 브리지의 PM 을 켠다. NVMe 의 RTD3(유휴 시 D3 진입) 같은 자식 쪽
	 * 절전이 이 상위 구조 위에서 동작한다. */

	return 0;
	/* [한국어] 성공. 이 시점에 이 컨트롤러 아래의 모든 장치가 동작 중이다 */
}
EXPORT_SYMBOL_GPL(pci_host_probe);
/* [한국어] 모든 호스트 컨트롤러 드라이버가 모듈에서 쓰므로 공개 */

/*
 * [한국어]
 * pci_bus_insert_busn_res - 버스가 담당할 번호 구간을 리소스 트리에 등록한다
 *
 * @b:       대상 버스.
 * @bus:     구간의 시작 번호(= 이 버스 자신의 번호).
 * @bus_max: 구간의 끝 번호(= subordinate).
 * @return: 충돌 없이 등록되었으면 1(참), 충돌했으면 0(거짓).
 *          bool 이 아니라 int 로 "conflict == NULL" 을 그대로 돌려주는 형태다.
 *
 * 왜 필요한가: 버스 번호도 메모리/IO 처럼 리소스 트리로 관리한다. 그래야
 * 두 브리지가 같은 번호 구간을 주장하는 상황을 커널이 자동으로 잡아낼 수 있다.
 * 부모는 상위 버스의 번호 구간이고, 루트 버스라면 도메인 전체(0~0xff)다.
 *
 * 충돌 시 왜 실패로 끝내지 않는가: 로그만 남기고 진행한다. 버스 번호 구간이
 * 겹치면 그 아래 장치에 접근하지 못할 수는 있지만, 열거 전체를 중단하는 것보다
 * 나머지 장치라도 살리는 편이 낫다는 판단이다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 리소스 트리 조작은 커널 resource 락이 보호한다.
 *
 * 호출 체인:
 *   pci_scan_bridge_extend()/pci_register_host_bridge() → [pci_bus_insert_busn_res]
 *     → get_pci_domain_busn_res(), request_resource_conflict()
 */
int pci_bus_insert_busn_res(struct pci_bus *b, int bus, int bus_max)
{
	struct resource *res = &b->busn_res;
	/* [한국어] 이 버스에 내장된 "버스 번호 구간" 리소스 */
	struct resource *parent_res, *conflict;
	/* [한국어] parent_res = 이 구간이 들어갈 부모 리소스,
	 * conflict   = 충돌한 기존 리소스(없으면 NULL). */

	res->start = bus;
	/* [한국어] 구간 시작 = 이 버스의 번호 */
	res->end = bus_max;
	/* [한국어] 구간 끝 = 이 버스 아래를 모두 포함하는 최대 번호 */
	res->flags = IORESOURCE_BUS;
	/* [한국어] 메모리/IO 가 아니라 버스 번호 축의 리소스임을 표시 */

	if (!pci_is_root_bus(b))
		parent_res = &b->parent->busn_res;
	/* [한국어] 일반 버스는 상위 버스의 번호 구간 아래에 들어간다.
	 * 이 포함 관계가 곧 "조상 브리지가 이 번호를 전달할 수 있는가"를 뜻한다. */
	else {
		/* [한국어] 루트 버스는 위에 버스가 없다 */
		parent_res = get_pci_domain_busn_res(pci_domain_nr(b));
		/* [한국어] 대신 이 도메인의 0~0xff 전체 구간을 부모로 삼는다.
		 * 도메인이 여럿이면 각자 별도의 번호 공간을 갖는다. */
		res->flags |= IORESOURCE_PCI_FIXED;
		/* [한국어] 루트 버스의 번호는 펌웨어가 정한 고정값이므로
		 * 리소스 할당기가 옮기지 못하도록 표시한다. */
	}

	conflict = request_resource_conflict(parent_res, res);
	/* [한국어] 부모 아래에 이 구간을 등록 시도한다. 이미 겹치는 구간이
	 * 있으면 그 리소스 포인터를, 성공하면 NULL 을 돌려준다. */

	if (conflict)
		dev_info(&b->dev,
			   "busn_res: can not insert %pR under %s%pR (conflicts with %s %pR)\n",
			    res, pci_is_root_bus(b) ? "domain " : "",
			    parent_res, conflict->name, conflict);
	/* [한국어] 무엇이 무엇과 충돌했는지 구체적으로 남긴다. 루트 버스면
	 * 부모가 도메인 리소스임을 "domain " 접두어로 구분해 보여 준다.
	 * 이 로그가 나오면 그 구간의 장치에 접근하지 못할 수 있다. */

	return conflict == NULL;
	/* [한국어] 충돌이 없었으면 1(성공), 있었으면 0. 호출자 대부분은
	 * 이 값을 검사하지 않고 진행한다. */
}

/*
 * [한국어]
 * pci_bus_update_busn_res_end - 버스 번호 구간의 끝을 실제 값으로 좁힌다
 *
 * @b:       대상 버스.
 * @bus_max: 새로 확정된 끝 번호.
 * @return: 0 이면 성공. res->start 가 bus_max 보다 크면 -EINVAL,
 *          adjust_resource() 가 실패하면 그 오류.
 *
 * 왜 필요한가: 브리지 아래를 스캔하기 전에는 버스가 몇 개나 필요한지 모른다.
 * 그래서 pci_scan_bridge_extend() 는 일단 구간을 최대로 열어 두고 스캔한 뒤,
 * 실제로 쓴 만큼으로 좁힌다. 그래야 남은 번호를 다른 브리지가 쓸 수 있다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_scan_bridge_extend() → [pci_bus_update_busn_res_end]
 *     → adjust_resource(), pci_bus_insert_busn_res()
 */
int pci_bus_update_busn_res_end(struct pci_bus *b, int bus_max)
{
	struct resource *res = &b->busn_res;
	/* [한국어] 좁힐 대상 구간 */
	struct resource old_res = *res;
	/* [한국어] 로그에 "무엇이 어떻게 바뀌었는가"를 찍기 위한 사본.
	 * adjust_resource() 가 res 를 바꾸기 전에 떠 둔다. */
	resource_size_t size;
	/* [한국어] 새 구간의 크기(번호 개수) */
	int ret;
	/* [한국어] adjust_resource 결과 */

	if (res->start > bus_max)
		return -EINVAL;
	/* [한국어] 끝이 시작보다 앞이면 빈 구간이 되어 말이 되지 않는다 */

	size = bus_max - res->start + 1;
	/* [한국어] 포함(inclusive) 구간이므로 +1 — 예: 2~5 는 4개 */
	ret = adjust_resource(res, res->start, size);
	/* [한국어] 리소스 트리에 등록된 상태 그대로 크기만 줄인다.
	 * 자식 리소스가 새 범위를 벗어나면 실패한다. */
	dev_info(&b->dev, "busn_res: %pR end %s updated to %02x\n",
			&old_res, ret ? "can not be" : "is", bus_max);
	/* [한국어] 성공/실패를 문장 안에서 "is"/"can not be" 로 바꿔 찍는다.
	 * 원래 구간(old_res)을 함께 보여 주어 변화가 드러나게 한다. */

	if (!ret && !res->parent)
		pci_bus_insert_busn_res(b, res->start, res->end);
	/* [한국어] 조정에 성공했는데 이 리소스가 아직 트리에 붙어 있지 않다면
	 * (parent 가 NULL) 지금 등록한다. adjust_resource() 는 이미 등록된
	 * 리소스를 조정할 뿐 새로 넣지는 않기 때문이다. */

	return ret;
	/* [한국어] 호출자는 대부분 이 값을 검사하지 않는다 */
}

/*
 * [한국어]
 * pci_bus_release_busn_res - 버스 번호 구간을 리소스 트리에서 뗀다
 *
 * @b: 대상 버스.
 * @return: 없음.
 *
 * 왜 필요한가: 버스를 제거할 때 그 번호 구간을 반납해야 나중에 같은 번호를
 * 재사용할 수 있다. 반납하지 않으면 핫플러그를 반복할 때마다 번호가 고갈된다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 버스 제거 경로.
 *
 * 호출 체인:
 *   pci_remove_root_bus() 등 → [pci_bus_release_busn_res] → release_resource()
 */
void pci_bus_release_busn_res(struct pci_bus *b)
{
	struct resource *res = &b->busn_res;
	/* [한국어] 반납할 구간 */
	int ret;
	/* [한국어] release_resource 결과 */

	if (!res->flags || !res->parent)
		return;
	/* [한국어] 애초에 설정된 적이 없거나(flags 0) 트리에 등록되지 않았으면
	 * (parent NULL) 반납할 것이 없다. 두 조건을 모두 보는 것이 중요한데,
	 * 등록에 실패한 구간은 flags 는 있어도 parent 가 없기 때문이다. */

	ret = release_resource(res);
	/* [한국어] 부모 리소스에서 떼어 낸다. 이제 이 번호 구간은 비어 있다 */
	dev_info(&b->dev, "busn_res: %pR %s released\n",
			res, ret ? "can not be" : "is");
	/* [한국어] 반납 결과를 로그로 남긴다 */
}

/*
 * [한국어]
 * pci_scan_root_bus_bridge - 호스트 브리지를 등록하고 그 아래 트리를 전부 훑는다
 *
 * @bridge: 컨트롤러 드라이버가 준비한 호스트 브리지(windows 목록 포함).
 * @return: 0 이면 성공, 음수면 -EINVAL 또는 등록 실패 errno.
 *
 * 왜 필요한가: "루트 버스 생성 + 재귀 스캔"을 묶은 표준 진입점이다.
 * pci_host_probe() 가 이 함수를 부르고, 리소스 배정과 드라이버 바인딩은
 * 그 뒤에 이어진다.
 *
 * busn 리소스를 찾는 이유: 컨트롤러 드라이버가 창 목록에 IORESOURCE_BUS
 * 항목을 넣어 두었다면, 그것이 이 호스트 브리지가 쓸 수 있는 버스 번호
 * 범위를 뜻한다. 그 시작값이 곧 루트 버스 번호다. 없으면 0~255 전체를
 * 쓴다고 가정하고, 스캔이 끝난 뒤 실제 사용량으로 좁힌다.
 *
 * NVMe 접점: NVMe SSD 가 발견되는 재귀 스캔이 이 함수의
 * pci_scan_child_bus() 호출에서 시작된다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 호출자가 pci_lock_rescan_remove() 로
 * 직렬화한 상태여야 한다.
 *
 * 호출 체인:
 *   pci_host_probe() → [pci_scan_root_bus_bridge]
 *     → pci_register_host_bridge(), pci_scan_child_bus()
 */
int pci_scan_root_bus_bridge(struct pci_host_bridge *bridge)
{
	struct resource_entry *window;
	/* [한국어] 창 목록 순회 커서 */
	bool found = false;
	/* [한국어] 창 목록에 버스 번호 리소스가 있었는지 */
	struct pci_bus *b;
	/* [한국어] 생성된 루트 버스 */
	int max, bus, ret;
	/* [한국어] max = 스캔 결과 최대 버스 번호, bus = 루트 버스 번호,
	 * ret = 등록 결과. */

	if (!bridge)
		return -EINVAL;
	/* [한국어] 인자 검증 */

	resource_list_for_each_entry(window, &bridge->windows)
		if (window->res->flags & IORESOURCE_BUS) {
			/* [한국어] 창 목록에서 버스 번호 축의 리소스를 찾는다 */
			bridge->busnr = window->res->start;
			/* [한국어] 그 구간의 시작이 루트 버스 번호다 */
			found = true;
			break;
			/* [한국어] 버스 번호 리소스는 하나뿐이므로 찾는 즉시 종료 */
		}

	ret = pci_register_host_bridge(bridge);
	/* [한국어] 루트 버스 생성 + sysfs 등록 + 창들을 버스 리소스로 이관 */
	if (ret < 0)
		return ret;
	/* [한국어] 등록 실패(-ENOMEM, -EEXIST 등) */

	b = bridge->bus;
	/* [한국어] 방금 만들어진 루트 버스 */
	bus = bridge->busnr;
	/* [한국어] 루트 버스 번호 */

	if (!found) {
		/* [한국어] 컨트롤러가 버스 번호 범위를 알려 주지 않은 경우 */
		dev_info(&b->dev,
		 "No busn resource found for root bus, will use [bus %02x-ff]\n",
			bus);
		/* [한국어] 무엇을 가정하는지 명시적으로 알린다 */
		pci_bus_insert_busn_res(b, bus, 255);
		/* [한국어] 일단 끝까지(255) 열어 둔다. 스캔 도중 브리지에 번호를
		 * 배정하려면 이 구간이 넉넉해야 하기 때문이다. */
	}

	max = pci_scan_child_bus(b);
	/* [한국어] ★트리 전체 재귀 스캔★ 여기서 모든 장치가 발견되고
	 * 브리지에 버스 번호가 배정된다. 반환값은 실제로 쓰인 최대 번호다. */

	if (!found)
		pci_bus_update_busn_res_end(b, max);
	/* [한국어] 넉넉히 열어 두었던 구간을 실제 사용량으로 좁힌다.
	 * 컨트롤러가 범위를 지정해 준 경우에는 그 값을 존중해 건드리지 않는다. */

	return 0;
	/* [한국어] 성공. 호출자가 리소스 배정과 드라이버 바인딩을 이어 간다 */
}
EXPORT_SYMBOL(pci_scan_root_bus_bridge);
/* [한국어] 호스트 컨트롤러 드라이버가 모듈에서 쓰므로 공개 */

/*
 * [한국어]
 * pci_scan_root_bus - 루트 버스를 만들고 스캔하는 구식 진입점
 *
 * @parent:    호스트 컨트롤러 device(없으면 NULL).
 * @bus:       루트 버스 번호.
 * @ops:       config 접근 함수 집합.
 * @sysdata:   컨트롤러 전용 데이터.
 * @resources: 주소 창 목록(성공 시 브리지로 이관되어 비워진다).
 * @return: 생성된 루트 버스, 실패 시 NULL.
 *
 * 왜 있는가: pci_create_root_bus() + pci_scan_child_bus() 를 묶은 구식 API 다.
 * pci_host_bridge 객체를 직접 다루지 않는 오래된 아키텍처/드라이버 코드가 쓴다.
 * 동작은 pci_scan_root_bus_bridge() 와 사실상 같다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   구식 아키텍처/컨트롤러 코드 → [pci_scan_root_bus]
 *     → pci_create_root_bus(), pci_scan_child_bus()
 */
struct pci_bus *pci_scan_root_bus(struct device *parent, int bus,
		struct pci_ops *ops, void *sysdata, struct list_head *resources)
{
	struct resource_entry *window;
	/* [한국어] 창 목록 순회 커서 */
	bool found = false;
	/* [한국어] 버스 번호 리소스가 목록에 있었는지 */
	struct pci_bus *b;
	/* [한국어] 생성된 루트 버스 */
	int max;
	/* [한국어] 스캔 결과 최대 버스 번호 */

	resource_list_for_each_entry(window, resources)
		if (window->res->flags & IORESOURCE_BUS) {
			found = true;
			break;
		}
	/* [한국어] 위 함수와 달리 여기서는 busnr 을 뽑지 않는다. 루트 버스
	 * 번호는 인자 @bus 로 이미 받았기 때문이며, 존재 여부만 확인한다.
	 * 목록은 아래 pci_create_root_bus() 에서 브리지로 옮겨지므로,
	 * 반드시 옮기기 전에 검사해야 한다. */

	b = pci_create_root_bus(parent, bus, ops, sysdata, resources);
	/* [한국어] 브리지 할당 + 등록 + 루트 버스 생성을 한 번에 */
	if (!b)
		return NULL;
	/* [한국어] 생성 실패 — 호출자가 resources 를 직접 정리해야 한다 */

	if (!found) {
		/* [한국어] 버스 번호 범위를 알려 주지 않은 경우 */
		dev_info(&b->dev,
		 "No busn resource found for root bus, will use [bus %02x-ff]\n",
			bus);
		pci_bus_insert_busn_res(b, bus, 255);
		/* [한국어] 끝까지 열어 두고 스캔한다 */
	}

	max = pci_scan_child_bus(b);
	/* [한국어] 트리 전체 재귀 스캔 */

	if (!found)
		pci_bus_update_busn_res_end(b, max);
	/* [한국어] 실제 사용량으로 구간을 좁힌다 */

	return b;
	/* [한국어] 만들어진 루트 버스 */
}
EXPORT_SYMBOL(pci_scan_root_bus);
/* [한국어] 구식 코드가 모듈에서 쓰므로 공개 */

/*
 * [한국어]
 * pci_scan_bus - 주소 공간 전체를 창으로 삼아 루트 버스를 만드는 최소 API
 *
 * @bus:     루트 버스 번호.
 * @ops:     config 접근 함수 집합.
 * @sysdata: 컨트롤러 전용 데이터.
 * @return: 생성된 루트 버스, 실패 시 NULL.
 *
 * 왜 있는가: 주소 변환도 없고 창 제약도 없는 단순한 플랫폼을 위한 가장
 * 오래된 API 다. I/O 포트 공간 전체, 메모리 공간 전체, 버스 번호 0~255
 * 전체를 그대로 창으로 삼는다. 실제 하드웨어 제약이 있는 현대 시스템에는
 * 맞지 않으므로 새 코드는 pci_host_probe() 경로를 써야 한다.
 *
 * 전역 리소스를 쓰는 것에 유의: ioport_resource/iomem_resource 는 커널
 * 전역의 루트 리소스이고, busn_resource 는 이 파일 앞부분에서 정의한
 * 0~255 구간이다. 즉 "제한 없음"을 뜻한다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   구식 아키텍처 초기화 코드 → [pci_scan_bus]
 *     → pci_create_root_bus(), pci_scan_child_bus()
 */
struct pci_bus *pci_scan_bus(int bus, struct pci_ops *ops,
					void *sysdata)
{
	LIST_HEAD(resources);
	/* [한국어] 이 함수 안에서만 쓰는 지역 창 목록 */
	struct pci_bus *b;
	/* [한국어] 생성된 루트 버스 */

	pci_add_resource(&resources, &ioport_resource);
	/* [한국어] I/O 포트 공간 전체를 창으로 등록 */
	pci_add_resource(&resources, &iomem_resource);
	/* [한국어] 메모리 공간 전체를 창으로 등록. 주소 변환 오프셋이 없으므로
	 * 버스 주소와 CPU 주소가 같다고 가정하는 것이다. */
	pci_add_resource(&resources, &busn_resource);
	/* [한국어] 버스 번호 0~255 전체. 이 파일 앞부분의 정적 리소스다 */
	b = pci_create_root_bus(NULL, bus, ops, sysdata, &resources);
	/* [한국어] 부모 device 없이 루트 버스 생성. 성공하면 위 목록이
	 * 브리지로 옮겨져 지역 목록은 비워진다. */
	if (b) {
		pci_scan_child_bus(b);
		/* [한국어] 트리 재귀 스캔. 버스 번호 구간을 따로 좁히지 않는
		 * 것은 이 API 가 제약 없는 환경을 전제하기 때문이다. */
	} else {
		pci_free_resource_list(&resources);
		/* [한국어] 생성 실패 시에는 목록이 옮겨지지 않고 지역 변수에
		 * 남아 있으므로, 직접 해제해 누수를 막아야 한다. */
	}
	return b;
	/* [한국어] 만들어진 루트 버스 또는 NULL */
}
EXPORT_SYMBOL(pci_scan_bus);
/* [한국어] 구식 아키텍처 코드가 쓰므로 공개 */

/**
 * pci_rescan_bus_bridge_resize - Scan a PCI bus for devices
 * @bridge: PCI bridge for the bus to scan
 *
 * Scan a PCI bus and child buses for new devices, add them,
 * and enable them, resizing bridge mmio/io resource if necessary
 * and possible.  The caller must ensure the child devices are already
 * removed for resizing to occur.
 *
 * Returns the max number of subordinate bus discovered.
 */
/*
 * [한국어]
 * pci_rescan_bus_bridge_resize - 브리지 아래를 재스캔하고 창 크기까지 다시 잡는다
 *
 * @bridge: 재스캔할 브리지. bridge->subordinate 가 그 아래 버스다.
 * @return: 발견된 최대 subordinate 버스 번호.
 *
 * 왜 필요한가: 핫플러그로 새 장치가 꽂히면 기존 브리지 창으로는 그 장치의
 * BAR 를 담지 못할 수 있다. 이 함수는 재스캔한 뒤 브리지 창 자체를 다시
 * 계산해 필요하면 넓힌다.
 *
 * 중요한 전제: 위 영어 주석이 못박듯이, 창 크기를 바꾸려면 그 아래 자식
 * 장치들이 미리 제거되어 있어야 한다. 사용 중인 장치의 BAR 주소를 옮길 수는
 * 없기 때문이다. 그 보장은 호출자의 책임이다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 호출자가 pci_lock_rescan_remove() 로
 * 직렬화해야 한다.
 *
 * 호출 체인:
 *   핫플러그 드라이버 → [pci_rescan_bus_bridge_resize]
 *     → pci_scan_child_bus(), pci_assign_unassigned_bridge_resources(),
 *       pci_bus_add_devices()
 */
unsigned int pci_rescan_bus_bridge_resize(struct pci_dev *bridge)
{
	unsigned int max;
	/* [한국어] 재스캔 결과 최대 버스 번호 */
	struct pci_bus *bus = bridge->subordinate;
	/* [한국어] 이 브리지 아래 버스. 브리지가 아니면 NULL 이지만,
	 * 호출자가 브리지임을 보장한다. */

	max = pci_scan_child_bus(bus);
	/* [한국어] 그 아래를 다시 훑어 새 장치를 발견한다. 이미 등록된 장치는
	 * pci_scan_single_device() 가 기존 객체를 돌려주므로 중복되지 않는다. */

	pci_assign_unassigned_bridge_resources(bridge);
	/* [한국어] ★창 재계산★ 이 브리지의 I/O/MEM/prefetch 창을 새 장치들의
	 * 요구에 맞춰 다시 잡는다. 필요하면 창 자체를 넓힌다. */

	pci_bus_add_devices(bus);
	/* [한국어] 주소가 배정된 새 장치들에 드라이버를 붙인다.
	 * 새 NVMe SSD 라면 여기서 nvme_probe() 가 실행된다. */

	return max;
	/* [한국어] 발견된 최대 버스 번호 */
}

/**
 * pci_rescan_bus - Scan a PCI bus for devices
 * @bus: PCI bus to scan
 *
 * Scan a PCI bus and child buses for new devices, add them,
 * and enable them.
 *
 * Returns the max number of subordinate bus discovered.
 */
/*
 * [한국어]
 * pci_rescan_bus - 버스를 다시 훑어 새 장치를 찾고 붙인다
 *
 * @bus: 재스캔할 버스.
 * @return: 발견된 최대 subordinate 버스 번호.
 *
 * 왜 필요한가: 사용자가 sysfs 에 `echo 1 > /sys/bus/pci/rescan` 을 하거나
 * 핫플러그 이벤트가 발생했을 때 새 장치를 찾는 경로다. 위의
 * _bridge_resize 판과 달리 브리지 창 크기는 건드리지 않고, 이미 있는 창
 * 안에서 미배정 리소스만 배정한다. 따라서 자식 장치를 미리 제거할 필요가 없다.
 *
 * NVMe 접점: 동작 중인 시스템에 NVMe SSD 를 꽂았을 때 이 경로로 발견되어
 * nvme_probe() 까지 이어진다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 호출자가 pci_lock_rescan_remove() 로
 * 직렬화해야 한다(아래 주석의 규약).
 *
 * 호출 체인:
 *   sysfs rescan / 핫플러그 → [pci_rescan_bus]
 *     → pci_scan_child_bus(), pci_assign_unassigned_bus_resources(),
 *       pci_bus_add_devices()
 */
unsigned int pci_rescan_bus(struct pci_bus *bus)
{
	unsigned int max;
	/* [한국어] 재스캔 결과 최대 버스 번호 */

	max = pci_scan_child_bus(bus);
	/* [한국어] 트리를 다시 훑어 새 장치의 pci_dev 를 만든다 */
	pci_assign_unassigned_bus_resources(bus);
	/* [한국어] 새 장치의 BAR 에 실제 주소를 배정한다. 기존 브리지 창
	 * 안에서만 배정하므로 이미 동작 중인 장치는 영향받지 않는다. */
	pci_bus_add_devices(bus);
	/* [한국어] 드라이버 바인딩 — 새 NVMe SSD 라면 nvme_probe() 실행 */

	return max;
	/* [한국어] 발견된 최대 버스 번호 */
}
EXPORT_SYMBOL_GPL(pci_rescan_bus);
/* [한국어] 핫플러그/sysfs 코드가 모듈에서 쓰므로 공개 */

/*
 * pci_rescan_bus(), pci_rescan_bus_bridge_resize() and PCI device removal
 * routines should always be executed under this mutex.
 */
DEFINE_MUTEX(pci_rescan_remove_lock);
/*
 * [한국어] pci_rescan_remove_lock - 재스캔과 장치 제거를 서로 배제하는 전역 뮤텍스.
 * 위 영어 주석이 규약을 못박는다: 재스캔 계열 함수와 장치 제거 루틴은 항상
 * 이 뮤텍스 아래에서 실행되어야 한다.
 * 왜 필요한가: 한쪽이 트리에 장치를 추가하는 동안 다른 쪽이 같은 트리에서
 * 장치를 떼어 내면 목록이 깨지고, 이미 해제된 pci_dev 를 참조하게 된다.
 * 설정자/읽는 자: 아래 pci_lock_rescan_remove()/pci_unlock_rescan_remove().
 * 동기화: 뮤텍스이므로 잠들 수 있다 — 인터럽트 문맥에서 잡으면 안 된다.
 */

/*
 * [한국어]
 * pci_lock_rescan_remove - 재스캔/제거 상호 배제 락을 잡는다
 *
 * @return: 없음.
 *
 * 왜 함수로 감싸는가: 뮤텍스 자체를 외부에 노출하지 않고 접근을 두 함수로
 * 좁혀, 호출자가 잘못된 방식으로 다루는 것을 막는다. 핫플러그 드라이버 등
 * 모듈에서도 써야 하므로 EXPORT 되어 있다.
 *
 * 실행 컨텍스트: 프로세스 문맥 전용(뮤텍스는 잠들 수 있다).
 *
 * 호출 체인:
 *   pci_host_probe()/핫플러그 드라이버 → [pci_lock_rescan_remove]
 */
void pci_lock_rescan_remove(void)
{
	mutex_lock(&pci_rescan_remove_lock);
	/* [한국어] 이미 잠겨 있으면 잠들어 기다린다 */
}
EXPORT_SYMBOL_GPL(pci_lock_rescan_remove);
/* [한국어] 핫플러그 드라이버 등이 모듈에서 쓰므로 공개 */

/*
 * [한국어]
 * pci_unlock_rescan_remove - 재스캔/제거 상호 배제 락을 놓는다
 *
 * @return: 없음.
 *
 * 실행 컨텍스트: 프로세스 문맥. 반드시 같은 스레드가 잡은 락을 놓아야 한다.
 *
 * 호출 체인:
 *   pci_host_probe()/핫플러그 드라이버 → [pci_unlock_rescan_remove]
 */
void pci_unlock_rescan_remove(void)
{
	mutex_unlock(&pci_rescan_remove_lock);
	/* [한국어] 기다리던 스레드가 있으면 깨어난다 */
}
EXPORT_SYMBOL_GPL(pci_unlock_rescan_remove);
/* [한국어] 핫플러그 드라이버 등이 모듈에서 쓰므로 공개 */

/*
 * [한국어]
 * pci_sort_bf_cmp - 장치를 (도메인, 버스, devfn) 순으로 비교하는 정렬 함수
 *
 * @d_a: 비교할 첫 device(안에 pci_dev 가 들어 있다).
 * @d_b: 비교할 둘째 device.
 * @return: a 가 앞서면 -1, 뒤서면 1, 같으면 0.
 *
 * 왜 필요한가: 커널의 전역 장치 목록은 스캔 순서대로 쌓이는데, 그 순서가
 * 깊이 우선이라 사람이 보기에 뒤죽박죽이다. 이 비교 함수로 다시 정렬하면
 * lspci 처럼 BDF 오름차순이 된다. 아래 pci_sort_breadthfirst() 가 쓴다.
 *
 * 비교 순서의 의미: 도메인이 가장 바깥, 그다음 버스 번호, 마지막이 devfn 이다.
 * devfn 안에 device 번호와 function 번호가 함께 들어 있으므로 이 하나로
 * 두 단계가 동시에 정렬된다.
 *
 * 실행 컨텍스트: 부팅 시 __init 문맥.
 *
 * 호출 체인:
 *   pci_sort_breadthfirst() → bus_sort_breadthfirst() → [pci_sort_bf_cmp]
 */
static int __init pci_sort_bf_cmp(const struct device *d_a,
				  const struct device *d_b)
{
	const struct pci_dev *a = to_pci_dev(d_a);
	/* [한국어] 첫 device 에서 pci_dev 복원 */
	const struct pci_dev *b = to_pci_dev(d_b);
	/* [한국어] 둘째 device 에서 pci_dev 복원 */

	if      (pci_domain_nr(a->bus) < pci_domain_nr(b->bus)) return -1;
	else if (pci_domain_nr(a->bus) > pci_domain_nr(b->bus)) return  1;
	/* [한국어] 1차 키: PCI 도메인(세그먼트) 번호. 다른 도메인은 완전히
	 * 별개의 주소 공간이므로 가장 바깥 기준이다. */

	if      (a->bus->number < b->bus->number) return -1;
	else if (a->bus->number > b->bus->number) return  1;
	/* [한국어] 2차 키: 버스 번호. 같은 도메인 안에서의 순서 */

	if      (a->devfn < b->devfn) return -1;
	else if (a->devfn > b->devfn) return  1;
	/* [한국어] 3차 키: devfn. 상위 5비트가 device, 하위 3비트가 function
	 * 이므로 이 한 번의 비교로 두 단계가 동시에 정렬된다. */

	return 0;
	/* [한국어] 세 키가 모두 같다 = 같은 장치. 실제로는 일어나지 않는다 */
}

/*
 * [한국어]
 * pci_sort_breadthfirst - 전역 PCI 장치 목록을 BDF 오름차순으로 재정렬한다
 *
 * @return: 없음.
 *
 * 왜 필요한가: 스캔은 깊이 우선으로 진행되므로 장치가 발견된 순서가
 * 사람이 기대하는 순서와 다르다. 부팅 후 이 함수를 한 번 불러
 * (도메인, 버스, devfn) 오름차순으로 정렬해 두면, 이후 목록을 훑는 코드와
 * 사용자 공간 도구가 안정적이고 예측 가능한 순서를 보게 된다.
 *
 * 실행 컨텍스트: 부팅 시 __init 문맥, 열거 완료 후 한 번.
 *
 * 호출 체인:
 *   아키텍처 초기화 코드 → [pci_sort_breadthfirst] → bus_sort_breadthfirst()
 */
void __init pci_sort_breadthfirst(void)
{
	bus_sort_breadthfirst(&pci_bus_type, &pci_sort_bf_cmp);
	/* [한국어] 드라이버 코어가 pci_bus_type 의 장치 목록을 위 비교 함수로
	 * 정렬한다. "breadthfirst" 라는 이름은 결과가 너비 우선 순서처럼
	 * 버스 번호 순으로 보이기 때문이다. */
}

/*
 * [한국어]
 * pci_hp_add_bridge - 핫플러그로 나타난 브리지에 버스 번호를 배정하고 하강한다
 *
 * @dev: 핫플러그로 발견된 브리지 장치.
 * @return: 0 이면 성공. 쓸 버스 번호가 없거나 하위 버스 생성에 실패하면 -1.
 *
 * 왜 별도 함수인가: 부팅 시 스캔은 트리 전체를 한꺼번에 훑으며 번호를
 * 배정하지만, 핫플러그는 이미 번호가 대부분 배정된 상태에서 한 브리지만
 * 끼워 넣어야 한다. 그래서 (1) 비어 있는 번호를 직접 찾고,
 * (2) 남은 번호를 여유분으로 넘겨 그 아래에 또 핫플러그가 생길 여지를
 * 남기는 별도의 절차가 필요하다.
 *
 * NVMe 접점: 핫스왑 베이에 PCIe 스위치가 달린 캐리어나 NVMe 장치를 꽂았을
 * 때, 그 아래 SSD 를 발견하려면 먼저 이 함수가 브리지에 버스 번호를 주어야 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 호출자(pciehp 등)가
 * pci_lock_rescan_remove() 로 직렬화한 상태여야 한다.
 *
 * 호출 체인:
 *   pciehp 등 핫플러그 드라이버 → [pci_hp_add_bridge]
 *     → pci_scan_bridge(), pci_scan_bridge_extend()
 */
int pci_hp_add_bridge(struct pci_dev *dev)
{
	struct pci_bus *parent = dev->bus;
	/* [한국어] 이 브리지가 꽂힌 버스 */
	int busnr, start = parent->busn_res.start;
	/* [한국어] busnr = 찾을 빈 번호, start = 부모가 담당하는 구간의 시작 */
	unsigned int available_buses = 0;
	/* [한국어] 이 브리지 아래에 여유로 넘겨 줄 번호 개수 */
	int end = parent->busn_res.end;
	/* [한국어] 부모가 담당하는 구간의 끝. 이 범위를 넘는 번호는 조상
	 * 브리지가 전달하지 않으므로 쓸 수 없다. */

	for (busnr = start; busnr <= end; busnr++) {
		/* [한국어] 부모 구간을 처음부터 훑으며 아직 쓰이지 않은 번호를 찾는다 */
		if (!pci_find_bus(pci_domain_nr(parent), busnr))
			break;
		/* [한국어] 그 번호의 버스가 존재하지 않으면 비어 있는 것이다 */
	}
	if (busnr-- > end) {
		/* [한국어] 루프가 break 없이 끝났다면 busnr == end+1 이다.
		 * 즉 부모 구간이 전부 차 있다. 후위 감소로 busnr 을 다시
		 * 마지막 유효 번호로 되돌리면서(아래에서 쓰이지는 않는다)
		 * 비교는 감소 전 값으로 수행한다. */
		pci_err(dev, "No bus number available for hot-added bridge\n");
		/* [한국어] 번호 고갈 — 이 브리지 아래는 쓸 수 없다.
		 * 부팅 시 핫플러그 여유분을 충분히 남기지 못했을 때 생긴다. */
		return -1;
	}

	/* Scan bridges that are already configured */
	busnr = pci_scan_bridge(parent, dev, busnr, 0);
	/* [한국어] pass 0 — 이 브리지가 이미 펌웨어 설정을 갖고 있다면
	 * 그것을 존중해 처리한다. 반환값은 갱신된 최대 번호다. */

	/*
	 * Distribute the available bus numbers between hotplug-capable
	 * bridges to make extending the chain later possible.
	 */
	available_buses = end - busnr;
	/* [한국어] 부모 구간의 끝까지 남은 번호를 모두 여유분으로 삼는다.
	 * 영어 주석대로, 이 브리지 아래에 또 핫플러그 브리지가 있으면
	 * pci_scan_child_bus_extend() 가 그들에게 나눠 준다. 그래야 나중에
	 * 체인을 더 늘릴 수 있다. */

	/* Scan bridges that need to be reconfigured */
	pci_scan_bridge_extend(parent, dev, busnr, available_buses, 1);
	/* [한국어] pass 1 — 실제로 번호를 배정하고 아래로 재귀 하강한다.
	 * 여기서 브리지 아래의 NVMe SSD 등이 발견된다. */

	if (!dev->subordinate)
		return -1;
	/* [한국어] 하위 버스가 만들어지지 않았다 = 위 스캔이 실패했다.
	 * pci_scan_bridge_extend() 는 실패를 반환값으로 알리지 않으므로,
	 * subordinate 포인터의 유무로 판정한다. */

	return 0;
	/* [한국어] 성공. 호출자가 이어서 리소스 배정과 드라이버 바인딩을 한다 */
}
EXPORT_SYMBOL_GPL(pci_hp_add_bridge);
/* [한국어] pciehp 등 핫플러그 드라이버가 모듈에서 쓰므로 공개 */
