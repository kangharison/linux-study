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

/* PCI/NVMe: 표준 BAR 6개 일괄 읽기. NVMe PCIe 엔드포인트 리소스 획득 */
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
 * agp_speed() - AGP 상태 비트를 enum pci_bus_speed로 변환
 *
 * NVMe 연결: NVMe 장치는 AGP를 사용하지 않으므로 직접 관련 없음.
 */
/* PCI/NVMe: agp_speed 함수 정의 */
static enum pci_bus_speed agp_speed(int agp3, int agpstat)
{
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	int index = 0;

	/* PCI/NVMe: 조걸 분기, 플래그/비트 마스크 검사 */
	if (agpstat & 4)
		index = 3; /* PCI/NVMe: AGP 4x 또는 AGP3 8x */
	/* PCI/NVMe: 추가 조걸 분기 */
	else if (agpstat & 2)
		index = 2; /* PCI/NVMe: AGP 2x 또는 AGP3 4x */
	/* PCI/NVMe: 추가 조걸 분기 */
	else if (agpstat & 1)
		index = 1; /* PCI/NVMe: AGP 1x 또는 AGP3 1x */
	/* PCI/NVMe: 조걸 분기의 else 경로 */
	else
		goto out;

	/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
	if (agp3) {
		index += 2; /* PCI/NVMe: AGP3 모드 시 속도 등급 보정 */
		/* PCI/NVMe: 조걸 분기, 값 일치 여부 검사 */
		if (index == 5)
			index = 0; /* PCI/NVMe: 유효하지 않은 조합을 UNKNOWN으로 */
	}

 out:
	return agp_speeds[index]; /* PCI/NVMe: AGP 속도 enum 반환 */
}

/*
 * pci_set_bus_speed() - 버스의 최대/현재 속도를 bridge capability에서 읽음
 *
 * NVMe 연결: NVMe SSD가 연결된 PCIe 링크의 속도는 DMA throughput과
 * 직결된다. 이 함수는 AGP/PCI-X/PCIe capability를 순회하며 버스 속도를
 * 결정. PCIe의 경우 LNKCAP/LNKSTA를 통해 link speed를 읽는다.
 */
/* PCI/NVMe: pci_set_bus_speed 함수 정의 */
static void pci_set_bus_speed(struct pci_bus *bus)
{
	struct pci_dev *bridge = bus->self; /* PCI/NVMe: 이 버스를 만든 bridge 장치 */
	/* PCI/NVMe: pos 변수 선언/초기화: config space 오프셋. NVMe capability/BAR 위치 */
	int pos;

	pos = pci_find_capability(bridge, PCI_CAP_ID_AGP); /* PCI/NVMe: AGP capability 위치 탐색 */
	/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
	if (!pos)
		pos = pci_find_capability(bridge, PCI_CAP_ID_AGP3); /* PCI/NVMe: AGP3 capability 탐색 */
	/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
	if (pos) {
		/* PCI/NVMe: 지역 변수 선언 및 초기화 */
		u32 agpstat, agpcmd;

		pci_read_config_dword(bridge, pos + PCI_AGP_STATUS, &agpstat); /* PCI/NVMe: AGP 상태 레지스터 읽기 */
		bus->max_bus_speed = agp_speed(agpstat & 8, agpstat & 7); /* PCI/NVMe: AGP 최대 속도 산출 */

		pci_read_config_dword(bridge, pos + PCI_AGP_COMMAND, &agpcmd); /* PCI/NVMe: AGP command 레지스터 읽기 */
		bus->cur_bus_speed = agp_speed(agpstat & 8, agpcmd & 7); /* PCI/NVMe: AGP 현재 속도 산출 */
	}

	pos = pci_find_capability(bridge, PCI_CAP_ID_PCIX); /* PCI/NVMe: PCI-X capability 위치 탐색 */
	/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
	if (pos) {
		/* PCI/NVMe: 지역 변수 선언 및 초기화 */
		u16 status;
		/* PCI/NVMe: max 변수 선언/초기화: 최대 bus 번호/값. NVMe 하위 버스 범위 */
		enum pci_bus_speed max;

		/* PCI/NVMe: PCI config space 16-bit 읽기. COMMAND/STATUS 등 워드 단위 접근 */
		pci_read_config_word(bridge, pos + PCI_X_BRIDGE_SSTATUS, /* PCI/NVMe: PCI config space 읽기. NVMe capability/CSR 탐색 */
				     &status); /* PCI/NVMe: PCI-X bridge secondary status 읽기 */

		if (status & PCI_X_SSTATUS_533MHZ) { /* PCI/NVMe: 533MHz PCI-X */
			/* PCI/NVMe: 변수에 값 할당: max */
			max = PCI_SPEED_133MHz_PCIX_533;
		} else if (status & PCI_X_SSTATUS_266MHZ) { /* PCI/NVMe: 266MHz PCI-X */
			/* PCI/NVMe: 변수에 값 할당: max */
			max = PCI_SPEED_133MHz_PCIX_266;
		} else if (status & PCI_X_SSTATUS_133MHZ) { /* PCI/NVMe: 133MHz PCI-X */
			/* PCI/NVMe: 조걸 분기, 값 일치 여부 검사 */
			if ((status & PCI_X_SSTATUS_VERS) == PCI_X_SSTATUS_V2)
				/* PCI/NVMe: 변수에 값 할당: max */
				max = PCI_SPEED_133MHz_PCIX_ECC;
			/* PCI/NVMe: 조걸 분기의 else 경로 */
			else
				/* PCI/NVMe: 변수에 값 할당: max */
				max = PCI_SPEED_133MHz_PCIX;
		/* PCI/NVMe: 후속 코드 동작 수행 */
		} else {
			max = PCI_SPEED_66MHz_PCIX; /* PCI/NVMe: 기본 66MHz PCI-X */
		}

		bus->max_bus_speed = max; /* PCI/NVMe: PCI-X 최대 속도 기록 */
		/* PCI/NVMe: 구조체 필드에 값 저장: bus->cur_bus_speed */
		bus->cur_bus_speed =
			pcix_bus_speed[FIELD_GET(PCI_X_SSTATUS_FREQ, status)]; /* PCI/NVMe: PCI-X 현재 속도 기록 */

		return;
	}

	if (pci_is_pcie(bridge)) { /* PCI/NVMe: PCIe bridge이면 LNKCAP/LNKSTA 기반 속도 설정; NVMe 링크 속도 결정 */
		/* PCI/NVMe: 지역 변수 선언 및 초기화 */
		u32 linkcap;

		/* PCI/NVMe: PCIe capability 4바이트 읽기 */
		pcie_capability_read_dword(bridge, PCI_EXP_LNKCAP, &linkcap);
		/* PCI/NVMe: 구조체 필드에 비트 마스크 적용: bus->max_bus_speed */
		bus->max_bus_speed = pcie_link_speed[linkcap & PCI_EXP_LNKCAP_SLS];

		/* PCI/NVMe: 현재 링크 속도 갱신 */
		pcie_update_link_speed(bus, PCIE_ADD_BUS);
	}
}

/*
 * pci_host_bridge_msi_domain() - host bridge에 연결된 MSI/MSI-X IRQ domain 탐색
 *
 * NVMe 연결: NVMe 컨트롤러의 per-queue MSI-X 인터럽트는 이 IRQ domain을
 * 통해 Linux irq 번호로 매핑. Interrupt remapping, vCPU affinity, IRQ
 * delivery 모드가 domain에 의해 결정되므로 NVMe I/O completion 지연과
 * 관련.
 */
/* PCI/NVMe: 호스트 브리지 MSI 도메인 획득 */
static struct irq_domain *pci_host_bridge_msi_domain(struct pci_bus *bus)
{
	/* PCI/NVMe: 후속 코드 동작 수행 */
	struct irq_domain *d;

	/* If the host bridge driver sets a MSI domain of the bridge, use it */
	d = dev_get_msi_domain(bus->bridge); /* PCI/NVMe: host bridge device에 직접 등록된 MSI domain 우선 사용 */

	/*
	 * Any firmware interface that can resolve the msi_domain
	 * should be called from here.
	 */
	/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
	if (!d)
		d = pci_host_bridge_of_msi_domain(bus); /* PCI/NVMe: device tree에서 MSI domain 탐색 */
	/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
	if (!d)
		d = pci_host_bridge_acpi_msi_domain(bus); /* PCI/NVMe: ACPI MADT/MSI mapping에서 domain 탐색 */

	/*
	 * If no IRQ domain was found via the OF tree, try looking it up
	 * directly through the fwnode_handle.
	 */
	/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
	if (!d) {
		struct fwnode_handle *fwnode = pci_root_bus_fwnode(bus); /* PCI/NVMe: root bus의 firmware node 획득 */

		/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
		if (fwnode)
			/* PCI/NVMe: fwnode로 IRQ domain 검색 */
			d = irq_find_matching_fwnode(fwnode,
						     DOMAIN_BUS_PCI_MSI); /* PCI/NVMe: firmware node로부터 PCI MSI domain 검색 */
	}

	return d; /* PCI/NVMe: 찾은 MSI IRQ domain 반환; 없으면 NULL */
}

/*
 * pci_set_bus_msi_domain() - 버스 device에 MSI domain 연결
 *
 * NVMe 연결: NVMe 컨트롤러가 연결된 버스의 MSI domain을 결정. SR-IOV
 * virtual bus를 포함해 상위 bridge의 MSI domain을 따라 올라가고, 없으면
 * host bridge의 domain을 상속.
 */
/* PCI/NVMe: pci_set_bus_msi_domain 함수 정의 */
static void pci_set_bus_msi_domain(struct pci_bus *bus)
{
	/* PCI/NVMe: 후속 코드 동작 수행 */
	struct irq_domain *d;
	/* PCI/NVMe: 후속 코드 동작 수행 */
	struct pci_bus *b;

	/*
	 * The bus can be a root bus, a subordinate bus, or a virtual bus
	 * created by an SR-IOV device.  Walk up to the first bridge device
	 * found or derive the domain from the host bridge.
	 */
	for (b = bus, d = NULL; !d && !pci_is_root_bus(b); b = b->parent) { /* PCI/NVMe: root bus에 도달하거나 MSI domain을 찾을 때까지 상위로 이동 */
		/* PCI/NVMe: 조걸 분기, 값 크기 비교 */
		if (b->self)
			d = dev_get_msi_domain(&b->self->dev); /* PCI/NVMe: bridge device에 설정된 MSI domain 확인; NVMe VF의 경우 PF 경로 */
	}

	/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
	if (!d)
		d = pci_host_bridge_msi_domain(b); /* PCI/NVMe: 상위에서 domain을 찾지 못하면 host bridge의 domain 사용 */

	dev_set_msi_domain(&bus->dev, d); /* PCI/NVMe: 버스 device의 MSI domain 설정; 이 버스의 NVMe 장치가 상속 */
}

/*
 * pci_preserve_config() - firmware가 설정한 PCI 리소스를 그대로 유지할지 결정
 *
 * NVMe 연결: true이면 kernel이 BAR/버스 번호를 재할당하지 않고 firmware
 * 설정을 존중. NVMe BAR의 물리 주소가 부팅 후 변경되지 않으므로
 * nvme_probe에서 보는 pci_resource_start() 값이 firmware가 배치한 값과
 * 동일.
 */
/* PCI/NVMe: pci_preserve_config 함수 정의 */
static bool pci_preserve_config(struct pci_host_bridge *host_bridge)
{
	if (pci_acpi_preserve_config(host_bridge)) /* PCI/NVMe: ACPI _OSC 등에서 config preserve 요청 시 true */
		/* PCI/NVMe: 참 반환 */
		return true;

	/* PCI/NVMe: 조걸 분기, 값 크기 비교 */
	if (host_bridge->dev.parent && host_bridge->dev.parent->of_node)
		return of_pci_preserve_config(host_bridge->dev.parent->of_node); /* PCI/NVMe: device tree에서 preserve 속성 확인 */

	return false; /* PCI/NVMe: 기본적으로 firmware 설정을 유지하지 않고 kernel이 재할당 가능 */
}

/*
 * pci_register_host_bridge() - host bridge를 PCI 코어에 등록하고 root bus 생성
 *
 * NVMe 연결: NVMe 컨트롤러가 연결될 root bus가 여기서 만들어지고,
 * bridge의 windows(MEM/IO/BUS 리소스)가 bus resources로 추가된다.
 * MSI domain, NUMA node, preserve_config 등 NVMe 동작에 영향을 주는
 * 설정이 이루어진다.
 */
/* PCI/NVMe: pci_register_host_bridge 함수 정의 */
static int pci_register_host_bridge(struct pci_host_bridge *bridge)
{
	/* PCI/NVMe: 변수에 값 할당: struct device *parent */
	struct device *parent = bridge->dev.parent;
	/* PCI/NVMe: 후속 코드 동작 수행 */
	struct resource_entry *window, *next, *n;
	/* PCI/NVMe: 후속 코드 동작 수행 */
	struct pci_bus *bus, *b;
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	resource_size_t offset, next_offset;
	/* PCI/NVMe: LIST_HEAD 함수 호출 */
	LIST_HEAD(resources);
	/* PCI/NVMe: 후속 코드 동작 수행 */
	struct resource *res, *next_res;
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	bool bus_registered = false;
	/* PCI/NVMe: 후속 코드 동작 수행 */
	char addr[64], *fmt;
	/* PCI/NVMe: 후속 코드 동작 수행 */
	const char *name;
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	int err;

	bus = pci_alloc_bus(NULL); /* PCI/NVMe: root bus용 pci_bus 할당 */
	if (!bus) /* PCI/NVMe: 메모리 부족 시 -ENOMEM */
		/* PCI/NVMe: 오류 코드 반환: -ENOMEM */
		return -ENOMEM;

	bridge->bus = bus; /* PCI/NVMe: host bridge가 생성한 root bus 연결 */

	bus->sysdata = bridge->sysdata; /* PCI/NVMe: platform-specific sysdata 연결 */
	bus->ops = bridge->ops; /* PCI/NVMe: PCI config space access ops 연결 */
	bus->number = bus->busn_res.start = bridge->busnr; /* PCI/NVMe: root bus 번호 설정 */
/* PCI/NVMe: 컴파일 조건: CONFIG_PCI_DOMAINS_GENERIC 정의 시 포함 */
#ifdef CONFIG_PCI_DOMAINS_GENERIC
	/* PCI/NVMe: 조걸 분기, 값 일치 여부 검사 */
	if (bridge->domain_nr == PCI_DOMAIN_NR_NOT_SET)
		bus->domain_nr = pci_bus_find_domain_nr(bus, parent); /* PCI/NVMe: domain 번호 동적 할당; NUMA/RC별 NVMe 구분 */
	/* PCI/NVMe: 조걸 분기의 else 경로 */
	else
		bus->domain_nr = bridge->domain_nr; /* PCI/NVMe: 미리 지정된 domain 번호 사용 */
	/* PCI/NVMe: 조걸 분기, 값 크기 비교 */
	if (bus->domain_nr < 0) {
		/* PCI/NVMe: 변수에 값 할당: err */
		err = bus->domain_nr;
		/* PCI/NVMe: 오류 처리/종료 지점으로 이동: free */
		goto free;
	}
/* PCI/NVMe: 컴파일 조건 종료 */
#endif

	/* PCI/NVMe: 버스 검색 */
	b = pci_find_bus(pci_domain_nr(bus), bridge->busnr);
	/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
	if (b) {
		/* Ignore it if we already got here via a different bridge */
		dev_dbg(&b->dev, "bus already known\n"); /* PCI/NVMe: 동일 domain/bus의 중복 bridge이면 무시 */
		/* PCI/NVMe: 변수에 값 할당: err */
		err = -EEXIST;
		/* PCI/NVMe: 오류 처리/종료 지점으로 이동: free */
		goto free;
	}

	/* PCI/NVMe: 장치 이름 설정 */
	dev_set_name(&bridge->dev, "pci%04x:%02x", pci_domain_nr(bus),
		     bridge->busnr); /* PCI/NVMe: host bridge device 이름 설정 */

	err = pcibios_root_bridge_prepare(bridge); /* PCI/NVMe: 아키텍처별 root bridge 준비 */
	/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
	if (err)
		/* PCI/NVMe: 오류 처리/종료 지점으로 이동: free */
		goto free;

	/* Temporarily move resources off the list */
	list_splice_init(&bridge->windows, &resources); /* PCI/NVMe: bridge window를 임시 리스트로 이동 */
	err = device_add(&bridge->dev); /* PCI/NVMe: host bridge device를 device model에 추가 */
	/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
	if (err)
		/* PCI/NVMe: 오류 처리/종료 지점으로 이동: free */
		goto free;

	bus->bridge = get_device(&bridge->dev); /* PCI/NVMe: bus가 host bridge device를 참조 */
	device_enable_async_suspend(bus->bridge); /* PCI/NVMe: 비동기 suspend 활성화 */
	pci_set_bus_of_node(bus); /* PCI/NVMe: device tree node 연결 */
	pci_set_bus_msi_domain(bus); /* PCI/NVMe: root bus의 MSI domain 설정; 하위 NVMe 장치 상속 */
	/* PCI/NVMe: 조걸 분기, 값 크기 비교 */
	if (bridge->msi_domain && !dev_get_msi_domain(&bus->dev) &&
	    /* PCI/NVMe: OF MSI map 존재 여부 확인 */
	    !pci_host_of_has_msi_map(parent))
		bus->bus_flags |= PCI_BUS_FLAGS_NO_MSI; /* PCI/NVMe: MSI domain이 없으면 bus에 MSI 불가 표시; NVMe MSI-X 사용 불가 */

	/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
	if (!parent)
		set_dev_node(bus->bridge, pcibus_to_node(bus)); /* PCI/NVMe: NUMA node 설정; NVMe locality에 영향 */

	bus->dev.class = &pcibus_class; /* PCI/NVMe: bus device class 설정 */
	bus->dev.parent = bus->bridge; /* PCI/NVMe: bus device의 부모를 host bridge로 */

	dev_set_name(&bus->dev, "%04x:%02x", pci_domain_nr(bus), bus->number); /* PCI/NVMe: bus device 이름 설정; 예: 0000:00 */
	name = dev_name(&bus->dev); /* PCI/NVMe: 설정된 이름 포인터 획득 */

	err = device_register(&bus->dev); /* PCI/NVMe: bus device 등록 */
	bus_registered = true; /* PCI/NVMe: bus 등록 완료 플래그 설정 */
	/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
	if (err)
		/* PCI/NVMe: 오류 처리/종료 지점으로 이동: unregister */
		goto unregister;

	pcibios_add_bus(bus); /* PCI/NVMe: 아키텍처별 bus 추가 처리 */

	/* PCI/NVMe: 조걸 분기, 값 크기 비교 */
	if (bus->ops->add_bus) {
		err = bus->ops->add_bus(bus); /* PCI/NVMe: platform-specific bus 추가 콜백 */
		/* PCI/NVMe: 조걸 분기, 값 크기 비교 */
		if (WARN_ON(err < 0))
			/* PCI/NVMe: 장치 오류 메시지 출력 */
			dev_err(&bus->dev, "failed to add bus: %d\n", err);
	}

	/* Create legacy_io and legacy_mem files for this bus */
	pci_create_legacy_files(bus); /* PCI/NVMe: 레거시 I/O/MEM sysfs 파일 생성 */

	/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
	if (parent)
		dev_info(parent, "PCI host bridge to bus %s\n", name); /* PCI/NVMe: parent device에 host bridge 정보 출력 */
	/* PCI/NVMe: 조걸 분기의 else 경로 */
	else
		pr_info("PCI host bridge to bus %s\n", name); /* PCI/NVMe: parent 없이 host bridge 정보 출력 */

	/* PCI/NVMe: 조걸 분기, 값 일치 여부 검사 */
	if (nr_node_ids > 1 && pcibus_to_node(bus) == NUMA_NO_NODE)
		dev_warn(&bus->dev, "Unknown NUMA node; performance will be reduced\n"); /* PCI/NVMe: NUMA node 미지정 시 성능 경고; NVMe latency에 영향 */

	/* Check if the boot configuration by FW needs to be preserved */
	bridge->preserve_config = pci_preserve_config(bridge); /* PCI/NVMe: firmware config 유지 여부 결정; NVMe BAR 재할당 여부 */

	/* Coalesce contiguous windows */
	resource_list_for_each_entry_safe(window, n, &resources) { /* PCI/NVMe: 인접한 address window를 병합 */
		if (list_is_last(&window->node, &resources)) /* PCI/NVMe: 마지막 entry이면 더 병합할 것이 없음 */
			break;

		next = list_next_entry(window, node); /* PCI/NVMe: 다음 window entry 획득 */
		offset = window->offset; /* PCI/NVMe: 현재 window의 CPU↔bus 오프셋 */
		res = window->res; /* PCI/NVMe: 현재 window의 resource */
		next_offset = next->offset; /* PCI/NVMe: 다음 window의 오프셋 */
		next_res = next->res; /* PCI/NVMe: 다음 window의 resource */

		if (res->flags != next_res->flags || offset != next_offset) /* PCI/NVMe: type이나 오프셋이 다륾면 병합 불가 */
			continue;

		if (res->end + 1 == next_res->start) { /* PCI/NVMe: 인접하면 두 window를 하나로 병합 */
			/* PCI/NVMe: 구조체 필드에 값 저장: next_res->start */
			next_res->start = res->start;
			/* PCI/NVMe: 구조체 필드에 값 저장: res->flags */
			res->flags = res->start = res->end = 0;
		}
	}

	/* Add initial resources to the bus */
	resource_list_for_each_entry_safe(window, n, &resources) { /* PCI/NVMe: 임시 리스트의 window를 bus resources로 이동 */
		/* PCI/NVMe: 변수에 값 할당: offset */
		offset = window->offset;
		/* PCI/NVMe: 변수에 값 할당: res */
		res = window->res;
		if (!res->flags && !res->start && !res->end) { /* PCI/NVMe: 병합으로 제거된 빈 entry는 해제 */
			/* PCI/NVMe: 리소스 해제 */
			release_resource(res);
			/* PCI/NVMe: resource_list_destroy_entry 함수 호출 */
			resource_list_destroy_entry(window);
			continue;
		}

		list_move_tail(&window->node, &bridge->windows); /* PCI/NVMe: 유효한 window를 bridge->windows로 복원 */

		/* PCI/NVMe: 조걸 분기, 플래그/비트 마스크 검사 */
		if (res->flags & IORESOURCE_BUS)
			pci_bus_insert_busn_res(bus, bus->number, res->end); /* PCI/NVMe: BUS 리소스이면 버스 번호 범위 등록 */
		/* PCI/NVMe: 조걸 분기의 else 경로 */
		else
			pci_bus_add_resource(bus, res); /* PCI/NVMe: MEM/IO 리소스를 bus resources에 추가; NVMe BAR 할당 기준 */

		/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
		if (offset) {
			/* PCI/NVMe: 조걸 분기, 값 일치 여부 검사 */
			if (resource_type(res) == IORESOURCE_IO)
				/* PCI/NVMe: 변수에 값 할당: fmt */
				fmt = " (bus address [%#06llx-%#06llx])";
			/* PCI/NVMe: 조걸 분기의 else 경로 */
			else
				/* PCI/NVMe: 변수에 값 할당: fmt */
				fmt = " (bus address [%#010llx-%#010llx])";

			/* PCI/NVMe: 문자열 포맷 출력 */
			snprintf(addr, sizeof(addr), fmt,
				 /* PCI/NVMe: 후속 코드 동작 수행 */
				 (unsigned long long)(res->start - offset),
				 (unsigned long long)(res->end - offset)); /* PCI/NVMe: bus 주소 범위를 문자열로 변환 */
		/* PCI/NVMe: 후속 코드 동작 수행 */
		} else
			addr[0] = '\0'; /* PCI/NVMe: 오프셋이 0이면 bus 주주 문자열 생략 */

		dev_info(&bus->dev, "root bus resource %pR%s\n", res, addr); /* PCI/NVMe: root bus 리소스 정보 로깅 */
	}

	of_pci_make_host_bridge_node(bridge); /* PCI/NVMe: device tree에서 host bridge node 생성/연결 */

	down_write(&pci_bus_sem); /* PCI/NVMe: root bus 리스트 쓰기 잠금 */
	list_add_tail(&bus->node, &pci_root_buses); /* PCI/NVMe: 전역 root bus 리스트에 추가 */
	up_write(&pci_bus_sem); /* PCI/NVMe: 잠금 해제 */

	/* PCI/NVMe: 정상 종료 및 반환 */
	return 0;

unregister:
	/* PCI/NVMe: bridge device reference count 감소, 0이면 메모리 해제 */
	put_device(&bridge->dev);
	/* PCI/NVMe: 장치 제거 */
	device_del(&bridge->dev);
free:
/* PCI/NVMe: 컴파일 조건: CONFIG_PCI_DOMAINS_GENERIC 정의 시 포함 */
#ifdef CONFIG_PCI_DOMAINS_GENERIC
	/* PCI/NVMe: 조걸 분기, 값 일치 여부 검사 */
	if (bridge->domain_nr == PCI_DOMAIN_NR_NOT_SET)
		pci_bus_release_domain_nr(parent, bus->domain_nr); /* PCI/NVMe: 동적 할당 domain 번호 반납 */
/* PCI/NVMe: 컴파일 조건 종료 */
#endif
	/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
	if (bus_registered)
		put_device(&bus->dev); /* PCI/NVMe: 등록된 bus device 참조 감소 */
	/* PCI/NVMe: 조걸 분기의 else 경로 */
	else
		kfree(bus); /* PCI/NVMe: 등록되지 않았으면 메모리 직접 해제 */

	/* PCI/NVMe: 결과 반환: err */
	return err;
}

/*
 * pci_bridge_child_ext_cfg_accessible() - 브리지 하위 버스에서 4KB extended
 *                                          config space 접근 가능 여부 판단
 *
 * NVMe 연결: NVMe PCIe 컨트롤러는 4KB extended config space에 MSI-X,
 * AER, LTR 등의 capability를 가진다. 이 함수가 false를 반환하면 하위
 * 버스의 장치들은 256B standard config space만 접근 가능하며, NVMe
 * MSI-X capability(offset 0x100 이상)를 읽을 수 없게 된다.
 */
/* PCI/NVMe: pci_bridge_child_ext_cfg_accessible 함수 정의 */
static bool pci_bridge_child_ext_cfg_accessible(struct pci_dev *bridge)
{
	/* PCI/NVMe: pos 변수 선언/초기화: config space 오프셋. NVMe capability/BAR 위치 */
	int pos;
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	u32 status;

	/*
	 * If extended config space isn't accessible on a bridge's primary
	 * bus, we certainly can't access it on the secondary bus.
	 */
	if (bridge->bus->bus_flags & PCI_BUS_FLAGS_NO_EXTCFG) /* PCI/NVMe: 상위 버스에서 extended config가 불가능하면 하위도 불가 */
		/* PCI/NVMe: 거짓 반환 */
		return false;

	/*
	 * PCIe Root Ports and switch ports are PCIe on both sides, so if
	 * extended config space is accessible on the primary, it's also
	 * accessible on the secondary.
	 */
	if (pci_is_pcie(bridge) && /* PCI/NVMe: PCIe bridge이고 */
	    (pci_pcie_type(bridge) == PCI_EXP_TYPE_ROOT_PORT || /* PCI/NVMe: Root Port이거나 */
	     pci_pcie_type(bridge) == PCI_EXP_TYPE_UPSTREAM || /* PCI/NVMe: Switch Upstream Port이거나 */
	     pci_pcie_type(bridge) == PCI_EXP_TYPE_DOWNSTREAM)) /* PCI/NVMe: Switch Downstream Port이면 */
		return true; /* PCI/NVMe: extended config space 접근 가능; NVMe MSI-X/AER/LTR 접근 가능 */

	/*
	 * For the other bridge types:
	 *   - PCI-to-PCI bridges
	 *   - PCIe-to-PCI/PCI-X forward bridges
	 *   - PCI/PCI-X-to-PCIe reverse bridges
	 * extended config space on the secondary side is only accessible
	 * if the bridge supports PCI-X Mode 2.
	 */
	pos = pci_find_capability(bridge, PCI_CAP_ID_PCIX); /* PCI/NVMe: PCI-X capability 탐색 */
	/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
	if (!pos)
		/* PCI/NVMe: 거짓 반환 */
		return false;

	pci_read_config_dword(bridge, pos + PCI_X_STATUS, &status); /* PCI/NVMe: PCI-X status 레지스터 읽기 */
	return status & (PCI_X_STATUS_266MHZ | PCI_X_STATUS_533MHZ); /* PCI/NVMe: PCI-X 266/533MHz 지원 시 extended config 접근 가능 */
}

/*
 * pci_alloc_child_bus() - parent bus 아래 새로운 하위 PCI 버스 할당
 *
 * NVMe 연결: NVMe SSD가 연결될 수 있는 하위 PCIe 버스(예: Root Port
 * 아래의 bus)가 여기서 생성. ops, bus_flags, MSI domain, NUMA node,
 * extended config 접근성 등을 부모로부터 상속받는다.
 */
/* PCI/NVMe: 자식 버스 할당 */
static struct pci_bus *pci_alloc_child_bus(struct pci_bus *parent,
					   /* PCI/NVMe: 후속 코드 동작 수행 */
					   struct pci_dev *bridge, int busnr)
{
	/* PCI/NVMe: 후속 코드 동작 수행 */
	struct pci_bus *child;
	/* PCI/NVMe: 후속 코드 동작 수행 */
	struct pci_host_bridge *host;
	/* PCI/NVMe: i 변수 선언/초기화: 반복 인덱스. NVMe BAR/버스/슬롯 순회 */
	int i;
	/* PCI/NVMe: ret 변수 선언/초기화: 반환값 변수. NVMe 초기화 성공/실패 상태 */
	int ret;

	/* Allocate a new bus and inherit stuff from the parent */
	child = pci_alloc_bus(parent); /* PCI/NVMe: 부모 bus로부터 상속하여 자식 bus 할당 */
	if (!child) /* PCI/NVMe: 할당 실패 시 NULL 반환 */
		/* PCI/NVMe: NULL 반환(메모리/리소스 부족 또는 초기화 실패) */
		return NULL;

	child->parent = parent; /* PCI/NVMe: 부모 bus 포인터 설정 */
	child->sysdata = parent->sysdata; /* PCI/NVMe: platform sysdata 상속 */
	child->bus_flags = parent->bus_flags; /* PCI/NVMe: bus flags 상속; NO_MSI/NO_EXTCFG 등 NVMe 기능 제한 전파 */

	host = pci_find_host_bridge(parent); /* PCI/NVMe: 부모 버스의 host bridge 획득 */
	/* PCI/NVMe: 조걸 분기, 값 크기 비교 */
	if (host->child_ops)
		child->ops = host->child_ops; /* PCI/NVMe: host bridge가 지정한 child_ops 사용 */
	/* PCI/NVMe: 조걸 분기의 else 경로 */
	else
		child->ops = parent->ops; /* PCI/NVMe: 부모 bus의 config access ops 상속 */

	/*
	 * Initialize some portions of the bus device, but don't register
	 * it now as the parent is not properly set up yet.
	 */
	child->dev.class = &pcibus_class; /* PCI/NVMe: bus device class 설정 */
	dev_set_name(&child->dev, "%04x:%02x", pci_domain_nr(child), busnr); /* PCI/NVMe: 자식 bus device 이름 설정; 예: 0000:01 */

	/* Set up the primary, secondary and subordinate bus numbers */
	child->number = child->busn_res.start = busnr; /* PCI/NVMe: secondary bus 번호 설정 */
	child->primary = parent->busn_res.start; /* PCI/NVMe: primary bus 번호 설정 */
	child->busn_res.end = 0xff; /* PCI/NVMe: subordinate bus 번호를 최대 0xff로 초기화 */

	if (!bridge) { /* PCI/NVMe: bridge가 없으면 root bus의 가상 자식으로 취급 */
		/* PCI/NVMe: child->dev.parent 설정: 부모 device(NVMe PCIe 계층) */
		child->dev.parent = parent->bridge;
		/* PCI/NVMe: 오류 처리/종료 지점으로 이동: add_dev */
		goto add_dev;
	}

	child->self = bridge; /* PCI/NVMe: 이 버스를 만든 bridge pci_dev 설정 */
	child->bridge = get_device(&bridge->dev); /* PCI/NVMe: bridge device 참조 증가 */
	child->dev.parent = child->bridge; /* PCI/NVMe: bus device의 부모를 bridge로 설정 */
	pci_set_bus_of_node(child); /* PCI/NVMe: device tree node 연결 */
	pci_set_bus_speed(child); /* PCI/NVMe: 버스 속도 설정; NVMe 링크 속도 반영 */

	/*
	 * Check whether extended config space is accessible on the child
	 * bus.  Note that we currently assume it is always accessible on
	 * the root bus.
	 */
	if (!pci_bridge_child_ext_cfg_accessible(bridge)) { /* PCI/NVMe: extended config 접근 불가 시 */
		child->bus_flags |= PCI_BUS_FLAGS_NO_EXTCFG; /* PCI/NVMe: bus flag에 extended config 불가 표시; NVMe MSI-X capability 접근 제한 */
		/* PCI/NVMe: pci_info() 진단 메시지 출력. NVMe 초기화/리소스 상태 기록 */
		pci_info(child, "extended config space not accessible\n"); /* PCI/NVMe: 진단/오류/경고 메시지 출력 */
	}

	/* Set up default resource pointers and names */
	for (i = 0; i < PCI_BRIDGE_RESOURCE_NUM; i++) { /* PCI/NVMe: bridge의 I/O/MEM/PREF MEM resource 포인터 연결 */
		/* PCI/NVMe: 구조체 필드에 비트 마스크 적용: child->resource[i] */
		child->resource[i] = &bridge->resource[PCI_BRIDGE_RESOURCES+i];
		/* PCI/NVMe: 구조체 필드에 값 저장: child->resource[i]->name */
		child->resource[i]->name = child->name;
	}
	bridge->subordinate = child; /* PCI/NVMe: bridge가 이 child bus를 하위로 가리킴 */

add_dev:
	pci_set_bus_msi_domain(child); /* PCI/NVMe: 자식 bus의 MSI domain 설정; NVMe MSI-X 상속 */
	ret = device_register(&child->dev); /* PCI/NVMe: 자식 bus device 등록 */
	/* PCI/NVMe: 조걸 분기, 값 크기 비교 */
	if (WARN_ON(ret < 0)) {
		put_device(&child->dev); /* PCI/NVMe: 등록 실패 시 device 참조 감소 */
		/* PCI/NVMe: NULL 반환(메모리/리소스 부족 또는 초기화 실패) */
		return NULL;
	}

	pcibios_add_bus(child); /* PCI/NVMe: 아키텍처별 자식 bus 추가 처리 */

	/* PCI/NVMe: 조걸 분기, 값 크기 비교 */
	if (child->ops->add_bus) {
		ret = child->ops->add_bus(child); /* PCI/NVMe: platform-specific bus 추가 콜백 */
		/* PCI/NVMe: 조걸 분기, 값 크기 비교 */
		if (WARN_ON(ret < 0))
			/* PCI/NVMe: 장치 오류 메시지 출력 */
			dev_err(&child->dev, "failed to add bus: %d\n", ret);
	}

	/* Create legacy_io and legacy_mem files for this bus */
	pci_create_legacy_files(child); /* PCI/NVMe: 레거시 I/O/MEM sysfs 파일 생성 */

	return child; /* PCI/NVMe: 초기화된 자식 bus 반환 */
}

/*
 * pci_add_new_bus() - parent bus 아래 새로운 하위 버스를 할당하고 연결
 *
 * NVMe 연결: bridge 뒤의 NVMe SSD가 위치할 하위 버스를 PCI 트리에
 * 추가. pci_bus_sem으로 트리 동시 수정을 보호.
 */
/* PCI/NVMe: 새 버스 추가 */
struct pci_bus *pci_add_new_bus(struct pci_bus *parent, struct pci_dev *dev,
				/* PCI/NVMe: 후속 코드 동작 수행 */
				int busnr)
{
	/* PCI/NVMe: 후속 코드 동작 수행 */
	struct pci_bus *child;

	child = pci_alloc_child_bus(parent, dev, busnr); /* PCI/NVMe: 지정 bus 번호로 자식 bus 할당 */
	if (child) { /* PCI/NVMe: 할당 성공 시 parent의 children 리스트에 추가 */
		down_write(&pci_bus_sem); /* PCI/NVMe: bus tree 쓰기 잠금 */
		list_add_tail(&child->node, &parent->children); /* PCI/NVMe: 부모의 하위 버스 리스트에 연결 */
		up_write(&pci_bus_sem); /* PCI/NVMe: 잠금 해제 */
	}
	return child; /* PCI/NVMe: 생성된 자식 bus 또는 NULL 반환 */
}
/* PCI/NVMe: EXPORT_SYMBOL 함수 호출 */
EXPORT_SYMBOL(pci_add_new_bus); /* PCI/NVMe: 심볼 외부 낸출. nvme-pci 등 모듈에서 참조 가능 */

/*
 * pci_enable_rrs_sv() - Root Port의 Configuration RRS Software Visibility 활성화
 *
 * NVMe 연결: RRS(Retry Request Status)는 장치가 준비되지 않았을 때
 * config read가 retry됨을 알리는 메커니즘. Software visibility를
 * 활성화하면 OS가 retry 상태를 직접 관찰할 수 있어, NVMe 컨트롤러의
 * config space 접근 대기 시간을 더 정확히 제어할 수 있다.
 */
/* PCI/NVMe: pci_enable_rrs_sv 함수 정의 */
static void pci_enable_rrs_sv(struct pci_dev *pdev)
{
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	u16 root_cap = 0;

	/* Enable Configuration RRS Software Visibility if supported */
	pcie_capability_read_word(pdev, PCI_EXP_RTCAP, &root_cap); /* PCI/NVMe: Root Capabilities 레지스터 읽기 */
	if (root_cap & PCI_EXP_RTCAP_RRS_SV) { /* PCI/NVMe: RRS Software Visibility 지원 여부 확인 */
		/* PCI/NVMe: PCIe capability 비트 설정 */
		pcie_capability_set_word(pdev, PCI_EXP_RTCTL,
					 PCI_EXP_RTCTL_RRS_SVE); /* PCI/NVMe: RRS Software Visibility Enable bit 설정 */
		pdev->config_rrs_sv = 1; /* PCI/NVMe: pci_dev에 RRS SV 활성화 표시 */
	}
}

/* PCI/NVMe: 하위 버스 재귀 스캔(확장 버스 분배) */
static unsigned int pci_scan_child_bus_extend(struct pci_bus *bus,
					      /* PCI/NVMe: 후속 코드 동작 수행 */
					      unsigned int available_buses);

/*
 * pbus_validate_busn() - 하위 버스 번호 범위가 상위 버스 범위 내에 있는지 검증
 *
 * NVMe 연결: NVMe SSD가 연결된 하위 버스의 busn_res가 상위 bridge의
 * secondary/subordinate 범위를 벗어나면 config cycle이 도달하지 못해
 * NVMe 장치에 접근할 수 없게 된다.
 */
/* PCI/NVMe: bus 번호 리소스 유효성 검사. 핫플러그/새 NVMe 장치용 번호 여유 확보 */
/* PCI/NVMe: pbus_validate_busn 함수 정의 */
void pbus_validate_busn(struct pci_bus *bus)
{
	struct pci_bus *upstream = bus->parent; /* PCI/NVMe: 직계 상위 버스 */
	struct pci_dev *bridge = bus->self; /* PCI/NVMe: 이 버스를 만든 bridge 장치 */

	/* Check that all devices are accessible */
	while (upstream->parent) { /* PCI/NVMe: root bus에 도달할 때까지 상위 버스를 따라 검증 */
		if ((bus->busn_res.end > upstream->busn_res.end) || /* PCI/NVMe: 하위 subordinate가 상위 subordinate보다 크면 오류 */
		    (bus->number > upstream->busn_res.end) || /* PCI/NVMe: 하위 secondary가 상위 subordinate보다 크면 오류 */
		    (bus->number < upstream->number) || /* PCI/NVMe: 하위 secondary가 상위 secondary보다 작으면 오류 */
		    (bus->busn_res.end < upstream->number)) { /* PCI/NVMe: 하위 subordinate가 상위 secondary보다 작으면 오류 */
			/* PCI/NVMe: pci_info() 진단 메시지 출력. NVMe 초기화/리소스 상태 기록 */
			pci_info(bridge, "devices behind bridge are unusable because %pR cannot be assigned for them\n",
				 &bus->busn_res); /* PCI/NVMe: 범위가 잘못되면 bridge 뒤 장치(NVMe 포함) 사용 불가 경고 */
			break;
		}
		upstream = upstream->parent; /* PCI/NVMe: 한 단계 더 상위 버스로 이동 */
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
 * pci_ea_fixed_busnrs() - Enhanced Allocation capability에서 고정된 secondary/
 *                         subordinate bus 번호 읽기
 *
 * NVMe 연결: EA capability는 firmware가 미리 할당한 bus 번호를
 * 알려준다. NVMe SSD가 연결될 하위 버스 번호가 EA에 의해 고정되어
 * 있으면 kernel은 이를 존중하여 할당해야 한다.
 */
/* PCI/NVMe: Enhanced Allocation 고정 bus 번호 파싱. NVMe SR-IOV VF bus 배치에 영향 */
/* PCI/NVMe: pci_ea_fixed_busnrs 함수 정의 */
bool pci_ea_fixed_busnrs(struct pci_dev *dev, u8 *sec, u8 *sub)
{
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	int ea, offset;
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	u32 dw;
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	u8 ea_sec, ea_sub;

	if (dev->hdr_type != PCI_HEADER_TYPE_BRIDGE) /* PCI/NVMe: bridge가 아니면 bus 번호가 없음 */
		/* PCI/NVMe: 거짓 반환 */
		return false;

	/* find PCI EA capability in list */
	ea = pci_find_capability(dev, PCI_CAP_ID_EA); /* PCI/NVMe: EA capability offset 탐색 */
	if (!ea) /* PCI/NVMe: EA capability가 없으면 고정 bus 번호 없음 */
		/* PCI/NVMe: 거짓 반환 */
		return false;

	offset = ea + PCI_EA_FIRST_ENT; /* PCI/NVMe: EA의 첫 번째 entry offset 계산 */
	pci_read_config_dword(dev, offset, &dw); /* PCI/NVMe: EA entry에서 bus 번호 필드가 포함된 dword 읽기 */
	ea_sec = FIELD_GET(PCI_EA_SEC_BUS_MASK, dw); /* PCI/NVMe: secondary bus 번호 추출 */
	ea_sub = FIELD_GET(PCI_EA_SUB_BUS_MASK, dw); /* PCI/NVMe: subordinate bus 번호 추출 */
	if (ea_sec  == 0 || ea_sub < ea_sec) /* PCI/NVMe: secondary가 0이거나 subordinate이 더 작으면 무효 */
		/* PCI/NVMe: 거짓 반환 */
		return false;

	*sec = ea_sec; /* PCI/NVMe: 호출자에게 secondary bus 번호 반환 */
	*sub = ea_sub; /* PCI/NVMe: 호출자에게 subordinate bus 번호 반환 */
	return true; /* PCI/NVMe: 고정 bus 번호 존재 */
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
/* PCI/NVMe: 브리지 뒤편 버스 확장 스캔 */
static int pci_scan_bridge_extend(struct pci_bus *bus, struct pci_dev *dev,
				  /* PCI/NVMe: max 변수 선언/초기화: 최대 bus 번호/값. NVMe 하위 버스 범위 */
				  int max, unsigned int available_buses,
				  /* PCI/NVMe: 후속 코드 동작 수행 */
				  int pass)
{
	/* PCI/NVMe: 후속 코드 동작 수행 */
	struct pci_bus *child;
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	u32 buses;
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	u16 bctl;
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	u8 primary, secondary, subordinate;
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	int broken = 0;
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	bool fixed_buses;
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	u8 fixed_sec, fixed_sub;
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	int next_busnr;

	/*
	 * Make sure the bridge is powered on to be able to access config
	 * space of devices below it.
	 */
	/* PCI/NVMe: 런타임 PM 레퍼런스 획득 */
	pm_runtime_get_sync(&dev->dev);

	/* PCI/NVMe: PCI config space 32-bit 읽기. NVMe capability/CSR 접근 기본 단위 */
	pci_read_config_dword(dev, PCI_PRIMARY_BUS, &buses); /* PCI/NVMe: PCI config space 읽기. NVMe capability/CSR 탐색 */
	/* PCI/NVMe: FIELD_GET 함수 호출 */
	primary = FIELD_GET(PCI_PRIMARY_BUS_MASK, buses);
	/* PCI/NVMe: FIELD_GET 함수 호출 */
	secondary = FIELD_GET(PCI_SECONDARY_BUS_MASK, buses);
	/* PCI/NVMe: FIELD_GET 함수 호출 */
	subordinate = FIELD_GET(PCI_SUBORDINATE_BUS_MASK, buses);

	/* PCI/NVMe: 디버그 메시지 출력 */
	pci_dbg(dev, "scanning [bus %02x-%02x] behind bridge, pass %d\n", /* PCI/NVMe: 진단/오류/경고 메시지 출력 */
		/* PCI/NVMe: 후속 코드 동작 수행 */
		secondary, subordinate, pass);

	/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
	if (!primary && (primary != bus->number) && secondary && subordinate) {
		/* PCI/NVMe: pci_warn() 경고 메시지 출력. NVMe 성능/호환성 문제 알림 */
		pci_warn(dev, "Primary bus is hard wired to 0\n"); /* PCI/NVMe: 진단/오류/경고 메시지 출력 */
		/* PCI/NVMe: 변수에 값 할당: primary */
		primary = bus->number;
	}

	/* Check if setup is sensible at all */
	/* PCI/NVMe: 조걸 분기, NVMe 장치 상태/플래그에 따른 경로 선택 */
	if (!pass &&
	    /* PCI/NVMe: 비트 연산으로 값 설정/마스크: (primary ! */
	    (primary != bus->number || secondary <= bus->number ||
	     /* PCI/NVMe: 후속 코드 동작 수행 */
	     secondary > subordinate)) {
		/* PCI/NVMe: pci_info() 진단 메시지 출력. NVMe 초기화/리소스 상태 기록 */
		pci_info(dev, "bridge configuration invalid ([bus %02x-%02x]), reconfiguring\n", /* PCI/NVMe: 진단/오류/경고 메시지 출력 */
			 /* PCI/NVMe: 후속 코드 동작 수행 */
			 secondary, subordinate);
		/* PCI/NVMe: 변수에 값 할당: broken */
		broken = 1;
	}

	/*
	 * Disable Master-Abort Mode during probing to avoid reporting of
	 * bus errors in some architectures.
	 */
	/* PCI/NVMe: PCI config space 16-bit 읽기. COMMAND/STATUS 등 워드 단위 접근 */
	pci_read_config_word(dev, PCI_BRIDGE_CONTROL, &bctl); /* PCI/NVMe: PCI config space 읽기. NVMe capability/CSR 탐색 */
	/* PCI/NVMe: PCI config space 16-bit 쓰기. COMMAND decode/MSE/IOSE 비트 제어 */
	pci_write_config_word(dev, PCI_BRIDGE_CONTROL, /* PCI/NVMe: PCI config space 쓰기. NVMe 레지스터/비트 제어 */
			      /* PCI/NVMe: 후속 코드 동작 수행 */
			      bctl & ~PCI_BRIDGE_CTL_MASTER_ABORT);

	/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
	if (pci_is_cardbus_bridge(dev)) {
		/* PCI/NVMe: CardBus 브리지 확장 스캔 */
		max = pci_cardbus_scan_bridge_extend(bus, dev, buses, max,
						     /* PCI/NVMe: 후속 코드 동작 수행 */
						     available_buses,
						     /* PCI/NVMe: 후속 코드 동작 수행 */
						     pass);
		goto out;
	}

	/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
	if ((secondary || subordinate) &&
	    /* PCI/NVMe: 모든 버스 수동 할당 여부 */
	    !pcibios_assign_all_busses() && !broken) {
		/* PCI/NVMe: 지역 변수 선언 및 초기화 */
		unsigned int cmax, buses;

		/*
		 * Bus already configured by firmware, process it in the
		 * first pass and just note the configuration.
		 */
		/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
		if (pass)
			goto out;

		/*
		 * The bus might already exist for two reasons: Either we
		 * are rescanning the bus or the bus is reachable through
		 * more than one bridge. The second case can happen with
		 * the i450NX chipset.
		 */
		/* PCI/NVMe: 버스 검색 */
		child = pci_find_bus(pci_domain_nr(bus), secondary);
		/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
		if (!child) {
			/* PCI/NVMe: 새 버스 추가 */
			child = pci_add_new_bus(bus, dev, secondary);
			/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
			if (!child)
				goto out;
			/* PCI/NVMe: 구조체 필드에 값 저장: child->primary */
			child->primary = primary;
			/* PCI/NVMe: bus 번호 리소스 삽입 */
			pci_bus_insert_busn_res(child, secondary, subordinate);
			/* PCI/NVMe: 구조체 필드에 값 저장: child->bridge_ctl */
			child->bridge_ctl = bctl;
		}

		/* PCI/NVMe: 변수에 값 할당: buses */
		buses = subordinate - secondary;
		/* PCI/NVMe: 하위 버스 재귀 스캔(확장 버스 분배) */
		cmax = pci_scan_child_bus_extend(child, buses);
		/* PCI/NVMe: 조걸 분기, 값 크기 비교 */
		if (cmax > subordinate)
			/* PCI/NVMe: pci_warn() 경고 메시지 출력. NVMe 성능/호환성 문제 알림 */
			pci_warn(dev, "bridge has subordinate %02x but max busn %02x\n", /* PCI/NVMe: 진단/오류/경고 메시지 출력 */
				 /* PCI/NVMe: 후속 코드 동작 수행 */
				 subordinate, cmax);

		/* Subordinate should equal child->busn_res.end */
		/* PCI/NVMe: 조걸 분기, 값 크기 비교 */
		if (subordinate > max)
			/* PCI/NVMe: 변수에 값 할당: max */
			max = subordinate;
	/* PCI/NVMe: 후속 코드 동작 수행 */
	} else {

		/*
		 * We need to assign a number to this bus which we always
		 * do in the second pass.
		 */
		/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
		if (!pass) {
			/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
			if (pcibios_assign_all_busses() || broken)

				/*
				 * Temporarily disable forwarding of the
				 * configuration cycles on all bridges in
				 * this bus segment to avoid possible
				 * conflicts in the second pass between two
				 * bridges programmed with overlapping bus
				 * ranges.
				 */
				/* PCI/NVMe: PCI config space 32-bit 쓰기. BAR/capability 레지스터 제어 */
				pci_write_config_dword(dev, PCI_PRIMARY_BUS, /* PCI/NVMe: PCI config space 쓰기. NVMe 레지스터/비트 제어 */
						       /* PCI/NVMe: 후속 코드 동작 수행 */
						       buses & PCI_SEC_LATENCY_TIMER_MASK);
			goto out;
		}

		/* Clear errors */
		/* PCI/NVMe: PCI config space 16-bit 쓰기. COMMAND decode/MSE/IOSE 비트 제어 */
		pci_write_config_word(dev, PCI_STATUS, 0xffff); /* PCI/NVMe: PCI config space 쓰기. NVMe 레지스터/비트 제어 */

		/* Read bus numbers from EA Capability (if present) */
		/* PCI/NVMe: pci_ea_fixed_busnrs 함수 호출 */
		fixed_buses = pci_ea_fixed_busnrs(dev, &fixed_sec, &fixed_sub);
		/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
		if (fixed_buses)
			/* PCI/NVMe: 변수에 값 할당: next_busnr */
			next_busnr = fixed_sec;
		/* PCI/NVMe: 조걸 분기의 else 경로 */
		else
			/* PCI/NVMe: 변수에 값 할당: next_busnr */
			next_busnr = max + 1;

		/*
		 * Prevent assigning a bus number that already exists.
		 * This can happen when a bridge is hot-plugged, so in this
		 * case we only re-scan this bus.
		 */
		/* PCI/NVMe: 버스 검색 */
		child = pci_find_bus(pci_domain_nr(bus), next_busnr);
		/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
		if (!child) {
			/* PCI/NVMe: 새 버스 추가 */
			child = pci_add_new_bus(bus, dev, next_busnr);
			/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
			if (!child)
				goto out;
			/* PCI/NVMe: bus 번호 리소스 삽입 */
			pci_bus_insert_busn_res(child, next_busnr,
						/* PCI/NVMe: 후속 코드 동작 수행 */
						bus->busn_res.end);
		}
		/* PCI/NVMe: 카운터 증감 */
		max++;
		/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
		if (available_buses)
			/* PCI/NVMe: 카운터 증감 */
			available_buses--;

		/* PCI/NVMe: 비트 연산으로 값 설정/마스크: buses */
		buses = (buses & PCI_SEC_LATENCY_TIMER_MASK) |
			/* PCI/NVMe: FIELD_PREP 함수 호출 */
			FIELD_PREP(PCI_PRIMARY_BUS_MASK, child->primary) |
			/* PCI/NVMe: FIELD_PREP 함수 호출 */
			FIELD_PREP(PCI_SECONDARY_BUS_MASK, child->busn_res.start) |
			/* PCI/NVMe: FIELD_PREP 함수 호출 */
			FIELD_PREP(PCI_SUBORDINATE_BUS_MASK, child->busn_res.end);

		/* We need to blast all three values with a single write */
		/* PCI/NVMe: PCI config space 32-bit 쓰기. BAR/capability 레지스터 제어 */
		pci_write_config_dword(dev, PCI_PRIMARY_BUS, buses); /* PCI/NVMe: PCI config space 쓰기. NVMe 레지스터/비트 제어 */

		/* PCI/NVMe: 구조체 필드에 값 저장: child->bridge_ctl */
		child->bridge_ctl = bctl;
		/* PCI/NVMe: 하위 버스 재귀 스캔(확장 버스 분배) */
		max = pci_scan_child_bus_extend(child, available_buses);

		/*
		 * Set subordinate bus number to its real value.
		 * If fixed subordinate bus number exists from EA
		 * capability then use it.
		 */
		/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
		if (fixed_buses)
			/* PCI/NVMe: 변수에 값 할당: max */
			max = fixed_sub;
		/* PCI/NVMe: bus 번호 리소스 끝 갱신 */
		pci_bus_update_busn_res_end(child, max);
		/* PCI/NVMe: PCI config space 8-bit 쓰기. latency timer, cache line 등 설정 */
		pci_write_config_byte(dev, PCI_SUBORDINATE_BUS, max); /* PCI/NVMe: PCI config space 쓰기. NVMe 레지스터/비트 제어 */
	}
	/* PCI/NVMe: 문자열 포맷 출력(길이 제한) */
	scnprintf(child->name, sizeof(child->name), "PCI Bus %04x:%02x",
		  /* PCI/NVMe: PCI 도메인 번호 획득 */
		  pci_domain_nr(bus), child->number);

	/* PCI/NVMe: pbus_validate_busn 함수 호출 */
	pbus_validate_busn(child);

out:
	/* Clear errors in the Secondary Status Register */
	/* PCI/NVMe: PCI config space 16-bit 쓰기. COMMAND decode/MSE/IOSE 비트 제어 */
	pci_write_config_word(dev, PCI_SEC_STATUS, 0xffff); /* PCI/NVMe: PCI config space 쓰기. NVMe 레지스터/비트 제어 */

	/* PCI/NVMe: PCI config space 16-bit 쓰기. COMMAND decode/MSE/IOSE 비트 제어 */
	pci_write_config_word(dev, PCI_BRIDGE_CONTROL, bctl); /* PCI/NVMe: PCI config space 쓰기. NVMe 레지스터/비트 제어 */

	/* PCI/NVMe: 런타임 PM 레퍼런스 반납 */
	pm_runtime_put(&dev->dev);

	/* PCI/NVMe: 결과 반환: max */
	return max;
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
 * pci_scan_bridge() - bridge 뒤의 버스를 스캔(확장 옵션 없음)
 *
 * NVMe 연결: available_buses=0으로 bridge 뒤를 스캔. NVMe SSD가
 * 연결된 downstream port를 탐색할 때 사용.
 */
/* PCI/NVMe: PCI 브리지 뒤쪽 버스/장치 탐색. NVMe SSD가 Switch/Root Port 뒤에 있을 때 호출 */
/* PCI/NVMe: pci_scan_bridge 함수 정의 */
int pci_scan_bridge(struct pci_bus *bus, struct pci_dev *dev, int max, int pass)
{
	return pci_scan_bridge_extend(bus, dev, max, 0, pass); /* PCI/NVMe: 추가 available_buses 없이 bridge 스캔 */
}
/* PCI/NVMe: EXPORT_SYMBOL 함수 호출 */
EXPORT_SYMBOL(pci_scan_bridge); /* PCI/NVMe: 심볼 외부 낸출. nvme-pci 등 모듈에서 참조 가능 */

/*
 * Read interrupt line and base address registers.
 * The architecture-dependent code can tweak these, of course.
 */
/* PCI/NVMe: pci_read_irq 함수 정의 */
static void pci_read_irq(struct pci_dev *dev)
{
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	unsigned char irq;

	/* VFs are not allowed to use INTx, so skip the config reads */
	/* PCI/NVMe: 조걸 분기, 값 크기 비교 */
	if (dev->is_virtfn) {
		/* PCI/NVMe: 구조체 필드에 값 저장: dev->pin */
		dev->pin = 0;
		/* PCI/NVMe: 구조체 필드에 값 저장: dev->irq */
		dev->irq = 0;
		return;
	}

	/* PCI/NVMe: PCI config space 8-bit 읽기. secondary/subordinate bus 등 바이트 접근 */
	pci_read_config_byte(dev, PCI_INTERRUPT_PIN, &irq); /* PCI/NVMe: PCI config space 읽기. NVMe capability/CSR 탐색 */
	/* PCI/NVMe: 구조체 필드에 값 저장: dev->pin */
	dev->pin = irq;
	/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
	if (irq)
		/* PCI/NVMe: PCI config space 8-bit 읽기. secondary/subordinate bus 등 바이트 접근 */
		pci_read_config_byte(dev, PCI_INTERRUPT_LINE, &irq); /* PCI/NVMe: PCI config space 읽기. NVMe capability/CSR 탐색 */
	/* PCI/NVMe: 구조체 필드에 값 저장: dev->irq */
	dev->irq = irq;
}

/* PCI/NVMe: PCIe port type(RP/EP/UP/DP) 식별. NVMe SSD의 upstream/downstream 관계 파악 */
/* PCI/NVMe: set_pcie_port_type 함수 정의 */
void set_pcie_port_type(struct pci_dev *pdev)
{
	/* PCI/NVMe: pos 변수 선언/초기화: config space 오프셋. NVMe capability/BAR 위치 */
	int pos;
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	u16 reg16;
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	u32 reg32;
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	int type;
	/* PCI/NVMe: 후속 코드 동작 수행 */
	struct pci_dev *parent;

	/* PCI/NVMe: PCI capability 위치 탐색 */
	pos = pci_find_capability(pdev, PCI_CAP_ID_EXP); /* PCI/NVMe: NVMe MSI-X/PCIe/AER capability 탐색 */
	/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
	if (!pos)
		return;

	/* PCI/NVMe: 구조체 필드에 값 저장: pdev->pcie_cap */
	pdev->pcie_cap = pos;
	/* PCI/NVMe: PCI config space 16-bit 읽기. COMMAND/STATUS 등 워드 단위 접근 */
	pci_read_config_word(pdev, pos + PCI_EXP_FLAGS, &reg16); /* PCI/NVMe: PCI config space 읽기. NVMe capability/CSR 탐색 */
	/* PCI/NVMe: 구조체 필드에 값 저장: pdev->pcie_flags_reg */
	pdev->pcie_flags_reg = reg16;

	/* PCI/NVMe: PCIe 포트 타입 획득 */
	type = pci_pcie_type(pdev);
	/* PCI/NVMe: 조걸 분기, 값 일치 여부 검사 */
	if (type == PCI_EXP_TYPE_ROOT_PORT)
		/* PCI/NVMe: Configuration RRS Software Visibility 활성화 */
		pci_enable_rrs_sv(pdev);

	/* PCI/NVMe: PCI config space 32-bit 읽기. NVMe capability/CSR 접근 기본 단위 */
	pci_read_config_dword(pdev, pos + PCI_EXP_DEVCAP, &pdev->devcap); /* PCI/NVMe: PCI config space 읽기. NVMe capability/CSR 탐색 */
	/* PCI/NVMe: FIELD_GET 함수 호출 */
	pdev->pcie_mpss = FIELD_GET(PCI_EXP_DEVCAP_PAYLOAD, pdev->devcap);

	/* PCI/NVMe: PCIe capability 4바이트 읽기 */
	pcie_capability_read_dword(pdev, PCI_EXP_LNKCAP, &reg32);
	/* PCI/NVMe: 조걸 분기, 플래그/비트 마스크 검사 */
	if (reg32 & PCI_EXP_LNKCAP_DLLLARC)
		/* PCI/NVMe: 구조체 필드에 값 저장: pdev->link_active_reporting */
		pdev->link_active_reporting = 1;

/* PCI/NVMe: 컴파일 조건: CONFIG_PCIEASPM 정의 시 포함 */
#ifdef CONFIG_PCIEASPM
	/* PCI/NVMe: 조걸 분기, 플래그/비트 마스크 검사 */
	if (reg32 & PCI_EXP_LNKCAP_ASPM_L0S)
		/* PCI/NVMe: 구조체 필드에 값 저장: pdev->aspm_l0s_support */
		pdev->aspm_l0s_support = 1;
	/* PCI/NVMe: 조걸 분기, 플래그/비트 마스크 검사 */
	if (reg32 & PCI_EXP_LNKCAP_ASPM_L1)
		/* PCI/NVMe: 구조체 필드에 값 저장: pdev->aspm_l1_support */
		pdev->aspm_l1_support = 1;
/* PCI/NVMe: 컴파일 조건 종료 */
#endif

	/* PCI/NVMe: 상위 브리지 획득 */
	parent = pci_upstream_bridge(pdev);
	/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
	if (!parent)
		return;

	/*
	 * Some systems do not identify their upstream/downstream ports
	 * correctly so detect impossible configurations here and correct
	 * the port type accordingly.
	 */
	/* PCI/NVMe: 조걸 분기, 값 일치 여부 검사 */
	if (type == PCI_EXP_TYPE_DOWNSTREAM) {
		/*
		 * If pdev claims to be downstream port but the parent
		 * device is also downstream port assume pdev is actually
		 * upstream port.
		 */
		/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
		if (pcie_downstream_port(parent)) {
			/* PCI/NVMe: pci_info() 진단 메시지 출력. NVMe 초기화/리소스 상태 기록 */
			pci_info(pdev, "claims to be downstream port but is acting as upstream port, correcting type\n");
			/* PCI/NVMe: 구조체 필드에 비트 마스크 적용: pdev->pcie_flags_reg & */
			pdev->pcie_flags_reg &= ~PCI_EXP_FLAGS_TYPE;
			/* PCI/NVMe: 구조체 필드에 값 저장: pdev->pcie_flags_reg | */
			pdev->pcie_flags_reg |= PCI_EXP_TYPE_UPSTREAM;
		}
	/* PCI/NVMe: if 함수 호출 */
	} else if (type == PCI_EXP_TYPE_UPSTREAM) {
		/*
		 * If pdev claims to be upstream port but the parent
		 * device is also upstream port assume pdev is actually
		 * downstream port.
		 */
		/* PCI/NVMe: 조걸 분기, 값 일치 여부 검사 */
		if (pci_pcie_type(parent) == PCI_EXP_TYPE_UPSTREAM) {
			/* PCI/NVMe: pci_info() 진단 메시지 출력. NVMe 초기화/리소스 상태 기록 */
			pci_info(pdev, "claims to be upstream port but is acting as downstream port, correcting type\n");
			/* PCI/NVMe: 구조체 필드에 비트 마스크 적용: pdev->pcie_flags_reg & */
			pdev->pcie_flags_reg &= ~PCI_EXP_FLAGS_TYPE;
			/* PCI/NVMe: 구조체 필드에 값 저장: pdev->pcie_flags_reg | */
			pdev->pcie_flags_reg |= PCI_EXP_TYPE_DOWNSTREAM;
		}
	}
}

/*
 * set_pcie_hotplug_bridge() - PCIe hotplug capability 지원 여부를 표시
 *
 * NVMe 연결: NVMe SSD가 hotplug slot에 연결된 경우(예: U.2/U.3
 * 백플레인), 이 함수가 bridge에 hotplug 플래그를 설정. 이후 PCI
 * hotplug 이벤트가 발생하면 nvme_remove_work 등을 통해 NVMe 드라이버가
 * 정리된다.
 */
/* PCI/NVMe: 핫플러그 브리지 플래그 설정. NVMe 핫플러그 이벤트 처리 준비 */
/* PCI/NVMe: set_pcie_hotplug_bridge 함수 정의 */
void set_pcie_hotplug_bridge(struct pci_dev *pdev)
{
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	u32 reg32;

	pcie_capability_read_dword(pdev, PCI_EXP_SLTCAP, &reg32); /* PCI/NVMe: Slot Capabilities 레지스터 읽기 */
	if (reg32 & PCI_EXP_SLTCAP_HPC) /* PCI/NVMe: Hot-Plug Controller가 slot에 내장되어 있으면 */
		pdev->is_hotplug_bridge = pdev->is_pciehp = 1; /* PCI/NVMe: bridge에 hotplug 지원 표시 */
}

/*
 * set_pcie_thunderbolt() - Thunderbolt 컨트롤러 하위 장치 여부 표시
 *
 * NVMe 연결: Thunderbolt 도킹이나 외장 NVMe 케이스가 연결된 경우,
 * 해당 PCIe 장치들은 is_thunderbolt=1로 표시되어 보안/전원 정책이
 * 다르게 적용될 수 있다.
 */
/* PCI/NVMe: set_pcie_thunderbolt 함수 정의 */
static void set_pcie_thunderbolt(struct pci_dev *dev)
{
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	u16 vsec;

	/* Is the device part of a Thunderbolt controller? */
	vsec = pci_find_vsec_capability(dev, PCI_VENDOR_ID_INTEL, PCI_VSEC_ID_INTEL_TBT); /* PCI/NVMe: Intel Thunderbolt vendor-specific capability 탐색 */
	/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
	if (vsec)
		dev->is_thunderbolt = 1; /* PCI/NVMe: Thunderbolt 계열 장치로 표시 */
}

/*
 * set_pcie_cxl() - CXL(Compute Express Link) 장치 여부 표시
 *
 * NVMe 연결: CXL 메모리 확장 장치나 CXL 2.0/3.0 기반 NVMe가 연결된
 * 경우 is_cxl 플래그가 설정. 이 플래그는 DMA coherence, 리소스 할당,
 * 전원 관리 정책에 영향을 줄 수 있다.
 */
/* PCI/NVMe: set_pcie_cxl 함수 정의 */
static void set_pcie_cxl(struct pci_dev *dev)
{
	/* PCI/NVMe: 후속 코드 동작 수행 */
	struct pci_dev *bridge;
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	u16 dvsec, cap;

	if (!pci_is_pcie(dev)) /* PCI/NVMe: PCIe 장치가 아니면 CXL 불가 */
		return;

	/*
	 * Update parent's CXL state because alternate protocol training
	 * may have changed
	 */
	bridge = pci_upstream_bridge(dev); /* PCI/NVMe: 상위 bridge 획득 */
	/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
	if (bridge)
		set_pcie_cxl(bridge); /* PCI/NVMe: 상위 bridge의 CXL 상태 재귀 갱신 */

	/* PCI/NVMe: Designated vendor-specific extended capability 탐색 */
	dvsec = pci_find_dvsec_capability(dev, PCI_VENDOR_ID_CXL,
					  PCI_DVSEC_CXL_FLEXBUS_PORT); /* PCI/NVMe: CXL Designated Vendor-Specific Extended Capability 탐색 */
	if (!dvsec) /* PCI/NVMe: CXL DVSEC가 없으면 CXL 장치 아님 */
		return;

	/* PCI/NVMe: PCI config space 16-bit 읽기. COMMAND/STATUS 등 워드 단위 접근 */
	pci_read_config_word(dev, dvsec + PCI_DVSEC_CXL_FLEXBUS_PORT_STATUS, /* PCI/NVMe: PCI config space 읽기. NVMe capability/CSR 탐색 */
			     &cap); /* PCI/NVMe: CXL Flexbus Port Status 읽기 */

	dev->is_cxl = FIELD_GET(PCI_DVSEC_CXL_FLEXBUS_PORT_STATUS_CACHE, cap) || /* PCI/NVMe: CXL cache 기능 지원 시 */
		FIELD_GET(PCI_DVSEC_CXL_FLEXBUS_PORT_STATUS_MEM, cap); /* PCI/NVMe: CXL memory 기능 지원 시 */

}

/*
 * set_pcie_untrusted() - 외부/제거 가능한 PCIe 장치를 untrusted로 표시
 *
 * NVMe 연결: 외장 NVMe enclosure나 Thunderbolt NVMe 같은 removable
 * 장치는 untrusted로 표시되어 DMA attack 방지를 위한 IOMMU/ATS 정책이
 * 적용될 수 있다.
 */
/* PCI/NVMe: set_pcie_untrusted 함수 정의 */
static void set_pcie_untrusted(struct pci_dev *dev)
{
	struct pci_dev *parent = pci_upstream_bridge(dev); /* PCI/NVMe: 상위 bridge 장치 획득 */

	if (!parent) /* PCI/NVMe: root bus 직접 연결 장치이면 신뢰 여부를 여기서 결정하지 않음 */
		return;
	/*
	 * If the upstream bridge is untrusted we treat this device as
	 * untrusted as well.
	 */
	if (parent->untrusted) { /* PCI/NVMe: 상위 bridge가 untrusted이면 상속 */
		/* PCI/NVMe: 구조체 필드에 값 저장: dev->untrusted */
		dev->untrusted = true;
		return;
	}

	if (arch_pci_dev_is_removable(dev)) { /* PCI/NVMe: 아키텍처에서 제거 가능으로 판단하면 */
		/* PCI/NVMe: 디버그 메시지 출력 */
		pci_dbg(dev, "marking as untrusted\n"); /* PCI/NVMe: 진단/오류/경고 메시지 출력 */
		dev->untrusted = true; /* PCI/NVMe: untrusted 표시; IOMMU/ATS 정책에 영향 */
	}
}

/*
 * pci_set_removable() - 사용자가 제거할 수 있는 PCIe 장치로 표시
 *
 * NVMe 연결: 외장 NVMe 케이스, Thunderbolt NVMe 등은 removable로
 * 표시되어 userspace(udev 등)가 이를 인식할 수 있다. 이는 eject
 * 처리와 보안 정책에 사용.
 */
/* PCI/NVMe: pci_set_removable 함수 정의 */
static void pci_set_removable(struct pci_dev *dev)
{
	struct pci_dev *parent = pci_upstream_bridge(dev); /* PCI/NVMe: 상위 bridge 장치 획득 */

	if (!parent) /* PCI/NVMe: root 직접 연결 장치는 여기서 처리하지 않음 */
		return;
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
	if (dev_is_removable(&parent->dev)) { /* PCI/NVMe: 상위 bridge가 external_facing/removable이면 */
		dev_set_removable(&dev->dev, DEVICE_REMOVABLE); /* PCI/NVMe: 이 장치도 removable로 표시 */
		return;
	}

	if (arch_pci_dev_is_removable(dev)) { /* PCI/NVMe: 아키텍처에서 removable로 판단하면 */
		/* PCI/NVMe: 디버그 메시지 출력 */
		pci_dbg(dev, "marking as removable\n"); /* PCI/NVMe: 진단/오류/경고 메시지 출력 */
		dev_set_removable(&dev->dev, DEVICE_REMOVABLE); /* PCI/NVMe: removable 속성 설정 */
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
 * pci_ext_cfg_is_aliased() - extended config space가 standard space의
 *                            alias인지 검출
 *
 * NVMe 연결: 일부 broken bridge는 extended config access(0x100~)를
 * 256B standard config로 잘못 forwarding. 이 경우 NVMe 컨트롤러의
 * MSI-X capability(0x100 이상)가 올바르게 읽히지 않아 MSI-X 인터럽트
 * 초기화가 실패할 수 있다.
 */
/* PCI/NVMe: pci_ext_cfg_is_aliased 함수 정의 */
static bool pci_ext_cfg_is_aliased(struct pci_dev *dev)
{
/* PCI/NVMe: 컴파일 조건: CONFIG_PCI_QUIRKS 정의 시 포함 */
#ifdef CONFIG_PCI_QUIRKS
	/* PCI/NVMe: pos 변수 선언/초기화: config space 오프셋. NVMe capability/BAR 위치 */
	int pos, ret;
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	u32 header, tmp;

	pci_read_config_dword(dev, PCI_VENDOR_ID, &header); /* PCI/NVMe: standard config의 vendor/device ID 읽기 */

	/* PCI/NVMe: 반복문, NVMe 리소스/장치 일괄 처리 */
	for (pos = PCI_CFG_SPACE_SIZE;
	     pos < PCI_CFG_SPACE_EXP_SIZE; pos += PCI_CFG_SPACE_SIZE) { /* PCI/NVMe: 0x100, 0x200, 0x300 등 extended offset 순회 */
		ret = pci_read_config_dword(dev, pos, &tmp); /* PCI/NVMe: extended offset에서 dword 읽기 */
		if ((ret != PCIBIOS_SUCCESSFUL) || (header != tmp)) /* PCI/NVMe: 읽기 실패거나 vendor ID와 다륾면 alias 아님 */
			/* PCI/NVMe: 거짓 반환 */
			return false;
	}

	return true; /* PCI/NVMe: 모든 extended offset이 vendor ID와 동일하면 alias로 판단; extended config 사용 불가 */
/* PCI/NVMe: 컴파일 조건: 이전 조건의 반대 경로 */
#else
	return false; /* PCI/NVMe: PCI_QUIRKS가 꺼져 있으면 alias 검출 안 함 */
/* PCI/NVMe: 컴파일 조건 종료 */
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
 * pci_cfg_space_size_ext() - extended config space 접근 가능성 테스트
 *
 * NVMe 연결: NVMe PCIe 컨트롤러는 4KB config space가 필요(MSI-X, AER,
 * LTR 등). 0x100에서 유효한 extended capability header를 읽을 수 있어야
 * 4KB로 인식. alias나 error response면 256B로 제한되어 NVMe 고급
 * capability 접근이 불가능해진다.
 */
/* PCI/NVMe: pci_cfg_space_size_ext 함수 정의 */
static int pci_cfg_space_size_ext(struct pci_dev *dev)
{
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	u32 status;
	int pos = PCI_CFG_SPACE_SIZE; /* PCI/NVMe: extended config space 시작 offset 0x100 */

	if (pci_read_config_dword(dev, pos, &status) != PCIBIOS_SUCCESSFUL) /* PCI/NVMe: 0x100 dword 읽기 시도 */
		return PCI_CFG_SPACE_SIZE; /* PCI/NVMe: 접근 실패 시 256B로 제한; NVMe MSI-X 접근 불가 */
	if (PCI_POSSIBLE_ERROR(status) || pci_ext_cfg_is_aliased(dev)) /* PCI/NVMe: error response이거나 alias이면 */
		return PCI_CFG_SPACE_SIZE; /* PCI/NVMe: 256B로 제한 */

	return PCI_CFG_SPACE_EXP_SIZE; /* PCI/NVMe: 4KB extended config space 사용 가능; NVMe MSI-X/AER/LTR 접근 가능 */
}

/*
 * pci_cfg_space_size() - 장치의 config space 크기(256B 또는 4KB) 결정
 *
 * NVMe 연결: NVMe PCIe 컨트롤러는 PCIe capability를 가지므로
 * pci_cfg_space_size_ext()를 통해 4KB로 인식. 4KB가 확볼되어야
 * pci_init_capabilities()에서 MSI-X, AER, LTR 등의 extended capability를
 * 파싱할 수 있다. SR-IOV VF는 spec상 4KB를 사용.
 */
/* PCI/NVMe: PCI config space 크기(256/4096) 결정. NVMe Extended Tags/AER 접근 범위 결정 */
/* PCI/NVMe: pci_cfg_space_size 함수 정의 */
int pci_cfg_space_size(struct pci_dev *dev)
{
	/* PCI/NVMe: pos 변수 선언/초기화: config space 오프셋. NVMe capability/BAR 위치 */
	int pos;
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	u32 status;
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	u16 class;

/* PCI/NVMe: 컴파일 조건: CONFIG_PCI_IOV 정의 시 포함 */
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
	if (dev->is_virtfn) /* PCI/NVMe: SR-IOV VF는 항상 4KB config space 사용; NVMe VF의 MSI-X 접근 보장 */
		/* PCI/NVMe: 결과 반환: PCI_CFG_SPACE_EXP_SIZE */
		return PCI_CFG_SPACE_EXP_SIZE;
/* PCI/NVMe: 컴파일 조건 종료 */
#endif

	if (dev->bus->bus_flags & PCI_BUS_FLAGS_NO_EXTCFG) /* PCI/NVMe: 상위 버스에서 extended config가 불가능하면 */
		return PCI_CFG_SPACE_SIZE; /* PCI/NVMe: 256B로 제한; NVMe MSI-X 초기화 실패 가능 */

	class = dev->class >> 8; /* PCI/NVMe: class code 상위 3바이트 추출 */
	/* PCI/NVMe: 조걸 분기, 값 일치 여부 검사 */
	if (class == PCI_CLASS_BRIDGE_HOST)
		return pci_cfg_space_size_ext(dev); /* PCI/NVMe: host bridge도 4KB 가능성 테스트 */

	if (pci_is_pcie(dev)) /* PCI/NVMe: PCIe 장치(NVMe 포함)이면 */
		return pci_cfg_space_size_ext(dev); /* PCI/NVMe: 4KB 접근 가능성 테스트 */

	pos = pci_find_capability(dev, PCI_CAP_ID_PCIX); /* PCI/NVMe: PCI-X capability 탐색 */
	/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
	if (!pos)
		return PCI_CFG_SPACE_SIZE; /* PCI/NVMe: PCI-X capability 없으면 256B */

	pci_read_config_dword(dev, pos + PCI_X_STATUS, &status); /* PCI/NVMe: PCI-X status 읽기 */
	if (status & (PCI_X_STATUS_266MHZ | PCI_X_STATUS_533MHZ)) /* PCI/NVMe: PCI-X 266/533MHz이면 extended config 가능 */
		return pci_cfg_space_size_ext(dev); /* PCI/NVMe: 4KB 테스트 수행 */

	return PCI_CFG_SPACE_SIZE; /* PCI/NVMe: 기본 256B config space */
}

/*
 * pci_class() - 장치의 class/revision 코드 읽기
 *
 * NVMe 연결: NVMe 컨트롤러는 class 0x010802(Non-Volatile Memory
 * Controller)를 가진다. 이 값이 pci_setup_device()에서 dev->class에
 * 저장되어 드라이버 매칭(nvme_pci_driver)의 기준이 된다.
 */
/* PCI/NVMe: pci_class 함수 정의 */
static u32 pci_class(struct pci_dev *dev)
{
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	u32 class;

/* PCI/NVMe: 컴파일 조건: CONFIG_PCI_IOV 정의 시 포함 */
#ifdef CONFIG_PCI_IOV
	if (dev->is_virtfn) /* PCI/NVMe: VF는 PF의 SR-IOV 구조체에 저장된 class 사용 */
		/* PCI/NVMe: 결과 반환: dev->physfn->sriov->class */
		return dev->physfn->sriov->class;
/* PCI/NVMe: 컴파일 조건 종료 */
#endif
	pci_read_config_dword(dev, PCI_CLASS_REVISION, &class); /* PCI/NVMe: 설정 공간의 class/revision dword 읽기 */
	return class; /* PCI/NVMe: class(상위 3바이트)와 revision(하위 1바이트) 반환 */
}

/*
 * pci_subsystem_ids() - subsystem vendor/device ID 읽기
 *
 * NVMe 연결: 동일 NVMe controller chip이라도 subsystem vendor/device가
 * 다륾면 firmware, thermal, form factor 등이 달라질 수 있다. quirk나
 * 드라이버 매칭에 사용.
 */
/* PCI/NVMe: pci_subsystem_ids 함수 정의 */
static void pci_subsystem_ids(struct pci_dev *dev, u16 *vendor, u16 *device)
{
/* PCI/NVMe: 컴파일 조건: CONFIG_PCI_IOV 정의 시 포함 */
#ifdef CONFIG_PCI_IOV
	if (dev->is_virtfn) { /* PCI/NVMe: VF는 PF의 SR-IOV 구조체에서 subsystem ID 상속 */
		*vendor = dev->physfn->sriov->subsystem_vendor; /* PCI/NVMe: subsystem vendor ID */
		*device = dev->physfn->sriov->subsystem_device; /* PCI/NVMe: subsystem device ID */
		return;
	}
/* PCI/NVMe: 컴파일 조건 종료 */
#endif
	pci_read_config_word(dev, PCI_SUBSYSTEM_VENDOR_ID, vendor); /* PCI/NVMe: 설정 공간에서 subsystem vendor ID 읽기 */
	pci_read_config_word(dev, PCI_SUBSYSTEM_ID, device); /* PCI/NVMe: 설정 공간에서 subsystem device ID 읽기 */
}

/*
 * pci_hdr_type() - PCI header type 읽기
 *
 * NVMe 연결: NVMe 컨트롤러는 PCI_HEADER_TYPE_NORMAL(0x00) 헤더를
 * 사용하며, 이로 인해 pci_setup_device()에서 standard BAR(6개)와
 * INTx/MSI-X를 파싱한다.
 */
/* PCI/NVMe: pci_hdr_type 함수 정의 */
static u8 pci_hdr_type(struct pci_dev *dev)
{
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	u8 hdr_type;

/* PCI/NVMe: 컴파일 조건: CONFIG_PCI_IOV 정의 시 포함 */
#ifdef CONFIG_PCI_IOV
	if (dev->is_virtfn) /* PCI/NVMe: VF는 PF의 SR-IOV 구조체에서 header type 상속 */
		/* PCI/NVMe: 결과 반환: dev->physfn->sriov->hdr_type */
		return dev->physfn->sriov->hdr_type;
/* PCI/NVMe: 컴파일 조건 종료 */
#endif
	pci_read_config_byte(dev, PCI_HEADER_TYPE, &hdr_type); /* PCI/NVMe: 설정 공간의 header type 바이트 읽기 */
	return hdr_type; /* PCI/NVMe: header type(멀티펑션 비트 포함) 반환 */
}

/* PCI/NVMe: 매크로 정의: 레거시 IO 리소스 플래그 */
#define LEGACY_IO_RESOURCE	(IORESOURCE_IO | IORESOURCE_PCI_FIXED)

/**
 * pci_intx_mask_broken - Test PCI_COMMAND_INTX_DISABLE writability
 * @dev: PCI device
 *
 * Test whether PCI_COMMAND_INTX_DISABLE is writable for @dev.  Check this
 * at enumeration-time to avoid modifying PCI_COMMAND at run-time.
 */
/*
 * pci_intx_mask_broken() - PCI_COMMAND_INTX_DISABLE 비트의 쓰기 가능성 테스트
 *
 * NVMe 연결: NVMe는 주로 MSI-X를 사용하지만 INTx disable이 제대로
 * 작동하지 않으면 런타임에 pci_intx() 호출 시 의도치 않은 INTx가 남을
 * 수 있다. 이 함수는 probe 시점에 미리 감지하여 dev->broken_intx_masking
 * 플래그를 설정.
 */
/* PCI/NVMe: pci_intx_mask_broken 함수 정의 */
static int pci_intx_mask_broken(struct pci_dev *dev)
{
	/* PCI/NVMe: orig 변수 선언/초기화: 원래 레지스터 값. NVMe BAR/COMMAND 백업 */
	u16 orig, toggle, new;

	pci_read_config_word(dev, PCI_COMMAND, &orig); /* PCI/NVMe: 원래 COMMAND 레지스터 값 읽기 */
	toggle = orig ^ PCI_COMMAND_INTX_DISABLE; /* PCI/NVMe: INTx disable bit를 toggle한 값 */
	pci_write_config_word(dev, PCI_COMMAND, toggle); /* PCI/NVMe: toggle 값 쓰기 */
	pci_read_config_word(dev, PCI_COMMAND, &new); /* PCI/NVMe: 실제 쓰인 값 읽기 */

	pci_write_config_word(dev, PCI_COMMAND, orig); /* PCI/NVMe: 원래 값 복원 */

	/*
	 * PCI_COMMAND_INTX_DISABLE was reserved and read-only prior to PCI
	 * r2.3, so strictly speaking, a device is not *broken* if it's not
	 * writable.  But we'll live with the misnomer for now.
	 */
	if (new != toggle) /* PCI/NVMe: 쓰기가 반영되지 않으면 INTx masking이 broken */
		return 1; /* PCI/NVMe: broken_intx_masking = true */
	return 0; /* PCI/NVMe: 정상 동작 */
}

/*
 * early_dump_pci_device() - probe 시점에 장치의 standard config space를 덤프
 *
 * NVMe 연결: pci_early_dump 커널 파라미터가 켜져 있으면 NVMe 컨트롤러의
 * 설정 공간을 부팅 초기에 16진수로 출력. 디버깅/ Bring-up 시 유용.
 */
/* PCI/NVMe: early_dump_pci_device 함수 정의 */
static void early_dump_pci_device(struct pci_dev *pdev)
{
	/* PCI/NVMe: sizeof 함수 호출 */
	u32 value[PCI_CFG_SPACE_SIZE / sizeof(u32)];
	/* PCI/NVMe: i 변수 선언/초기화: 반복 인덱스. NVMe BAR/버스/슬롯 순회 */
	int i;

	pci_info(pdev, "config space:\n"); /* PCI/NVMe: config space 덤프 시작 로그 */

	for (i = 0; i < ARRAY_SIZE(value); i++) /* PCI/NVMe: 256B를 4바이트 단위로 읽기 */
		pci_read_config_dword(pdev, i * sizeof(u32), &value[i]); /* PCI/NVMe: offset i*4에서 dword 읽기 */

	/* PCI/NVMe: 16진수 덤프 출력 */
	print_hex_dump(KERN_INFO, "", DUMP_PREFIX_OFFSET, 16, 1,
		       value, ARRAY_SIZE(value) * sizeof(u32), false); /* PCI/NVMe: 읽은 256B를 16진수로 출력 */
}

/*
 * pci_type_str() - PCIe/PCI 장치 유형을 사람이 읽을 수 있는 문자열로 변환
 *
 * NVMe 연결: NVMe 컨트롤러는 일반적으로 "PCIe Endpoint"로 인쇄. 로그를
 * 통해 NVMe 장치가 정상적으로 PCIe Endpoint로 인식되었는지 확인 가능.
 */
/* PCI/NVMe: 장치 타입 문자열 반환 */
static const char *pci_type_str(struct pci_dev *dev)
{
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	static const char * const str[] = {
		/* PCI/NVMe: 후속 코드 동작 수행 */
		"PCIe Endpoint",
		/* PCI/NVMe: 후속 코드 동작 수행 */
		"PCIe Legacy Endpoint",
		/* PCI/NVMe: 후속 코드 동작 수행 */
		"PCIe unknown",
		/* PCI/NVMe: 후속 코드 동작 수행 */
		"PCIe unknown",
		/* PCI/NVMe: 후속 코드 동작 수행 */
		"PCIe Root Port",
		/* PCI/NVMe: 후속 코드 동작 수행 */
		"PCIe Switch Upstream Port",
		/* PCI/NVMe: 후속 코드 동작 수행 */
		"PCIe Switch Downstream Port",
		/* PCI/NVMe: 후속 코드 동작 수행 */
		"PCIe to PCI/PCI-X bridge",
		/* PCI/NVMe: 후속 코드 동작 수행 */
		"PCI/PCI-X to PCIe bridge",
		/* PCI/NVMe: 후속 코드 동작 수행 */
		"PCIe Root Complex Integrated Endpoint",
		/* PCI/NVMe: 후속 코드 동작 수행 */
		"PCIe Root Complex Event Collector",
	};
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	int type;

	if (pci_is_pcie(dev)) { /* PCI/NVMe: PCIe 장치이면 */
		type = pci_pcie_type(dev); /* PCI/NVMe: PCIe capability의 device/port type 필드 읽기 */
		/* PCI/NVMe: 조걸 분기, 값 크기 비교 */
		if (type < ARRAY_SIZE(str))
			return str[type]; /* PCI/NVMe: NVMe 컨트롤러는 보통 여기서 "PCIe Endpoint" 반환 */

		/* PCI/NVMe: 결과 반환: "PCIe unknown" */
		return "PCIe unknown";
	}

	switch (dev->hdr_type) { /* PCI/NVMe: PCIe가 아닌 legacy PCI 장치 유형 */
	/* PCI/NVMe: case 분기: PCI_HEADER_TYPE_NORMAL */
	case PCI_HEADER_TYPE_NORMAL:
		/* PCI/NVMe: 결과 반환: "conventional PCI endpoint" */
		return "conventional PCI endpoint";
	/* PCI/NVMe: case 분기: PCI_HEADER_TYPE_BRIDGE */
	case PCI_HEADER_TYPE_BRIDGE:
		/* PCI/NVMe: 결과 반환: "conventional PCI bridge" */
		return "conventional PCI bridge";
	/* PCI/NVMe: case 분기: PCI_HEADER_TYPE_CARDBUS */
	case PCI_HEADER_TYPE_CARDBUS:
		/* PCI/NVMe: 결과 반환: "CardBus bridge" */
		return "CardBus bridge";
	default:
		/* PCI/NVMe: 결과 반환: "conventional PCI" */
		return "conventional PCI";
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
/* PCI/NVMe: PCI 장치 공통 초기화. NVMe 장치의 class, BAR, MSI-X capability 등 설정 */
/* PCI/NVMe: pci_setup_device 함수 정의 */
int pci_setup_device(struct pci_dev *dev)
{
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	u32 class;
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	u16 cmd;
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	u8 hdr_type;
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	int err, pos = 0;
	/* PCI/NVMe: region 변수 선언/초기화: pci_bus_region. bus 주소 ↔ CPU physical address 변환용 */
	struct pci_bus_region region;
	/* PCI/NVMe: 후속 코드 동작 수행 */
	struct resource *res;

	/* PCI/NVMe: 헤더 타입 읽기 */
	hdr_type = pci_hdr_type(dev);

	/* PCI/NVMe: 구조체 필드에 값 저장: dev->sysdata */
	dev->sysdata = dev->bus->sysdata;
	/* PCI/NVMe: dev->dev.parent 설정: 부모 device(NVMe PCIe 계층) */
	dev->dev.parent = dev->bus->bridge;
	/* PCI/NVMe: dev->dev.bus 플래그: 소속 PCI 버스(NVMe BDF 경로) */
	dev->dev.bus = &pci_bus_type;
	/* PCI/NVMe: FIELD_GET 함수 호출 */
	dev->hdr_type = FIELD_GET(PCI_HEADER_TYPE_MASK, hdr_type);
	/* PCI/NVMe: FIELD_GET 함수 호출 */
	dev->multifunction = FIELD_GET(PCI_HEADER_TYPE_MFD, hdr_type);
	/* PCI/NVMe: 구조체 필드에 값 저장: dev->error_state */
	dev->error_state = pci_channel_io_normal;
	/* PCI/NVMe: PCIe 포트 타입 설정 */
	set_pcie_port_type(dev);

	/* PCI/NVMe: OF node 연결 */
	err = pci_set_of_node(dev);
	/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
	if (err)
		/* PCI/NVMe: 결과 반환: err */
		return err;
	/* PCI/NVMe: ACPI fwnode 연결 */
	pci_set_acpi_fwnode(dev);

	/* PCI/NVMe: PCI 슬롯 할당 */
	pci_dev_assign_slot(dev);

	/*
	 * Assume 32-bit PCI; let 64-bit PCI cards (which are far rarer)
	 * set this higher, assuming the system even supports it.
	 */
	/* PCI/NVMe: 구조체 필드에 값 저장: dev->dma_mask */
	dev->dma_mask = 0xffffffff;

	/*
	 * Assume 64-bit addresses for MSI initially. Will be changed to 32-bit
	 * if MSI (rather than MSI-X) capability does not have
	 * PCI_MSI_FLAGS_64BIT. Can also be overridden by driver.
	 */
	/* PCI/NVMe: DMA_BIT_MASK 함수 호출 */
	dev->msi_addr_mask = DMA_BIT_MASK(64);

	/* PCI/NVMe: 장치 이름 설정 */
	dev_set_name(&dev->dev, "%04x:%02x:%02x.%d", pci_domain_nr(dev->bus),
		     /* PCI/NVMe: PCI_SLOT 함수 호출 */
		     dev->bus->number, PCI_SLOT(dev->devfn),
		     /* PCI/NVMe: PCI_FUNC 함수 호출 */
		     PCI_FUNC(dev->devfn));

	/* PCI/NVMe: Class Code/Revision 읽기 */
	class = pci_class(dev);

	/* PCI/NVMe: 구조체 필드에 비트 마스크 적용: dev->revision */
	dev->revision = class & 0xff;
	dev->class = class >> 8;		    /* upper 3 bytes */

	/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
	if (pci_early_dump)
		/* PCI/NVMe: 초기 config space 덤프 */
		early_dump_pci_device(dev);

	/* Need to have dev->class ready */
	/* PCI/NVMe: config space 크기 결정 */
	dev->cfg_size = pci_cfg_space_size(dev);

	/* Need to have dev->cfg_size ready */
	/* PCI/NVMe: Thunderbolt 장치 표시 */
	set_pcie_thunderbolt(dev);

	/* PCI/NVMe: CXL 장치 표시 */
	set_pcie_cxl(dev);

	/* PCI/NVMe: 신뢰할 수 없는 장치 표시 */
	set_pcie_untrusted(dev);

	/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
	if (pci_is_pcie(dev))
		/* PCI/NVMe: 지원 링크 속도 획득 */
		dev->supported_speeds = pcie_get_supported_speeds(dev);

	/* "Unknown power state" */
	/* PCI/NVMe: 구조체 필드에 값 저장: dev->current_state */
	dev->current_state = PCI_UNKNOWN;

	/* Early fixups, before probing the BARs */
	/* PCI/NVMe: 장치 quirk/workaround 적용 */
	pci_fixup_device(pci_fixup_early, dev);

	/* PCI/NVMe: 제거 가능 장치 표시 */
	pci_set_removable(dev);

	/* PCI/NVMe: pci_info() 진단 메시지 출력. NVMe 초기화/리소스 상태 기록 */
	pci_info(dev, "[%04x:%04x] type %02x class %#08x %s\n", /* PCI/NVMe: 진단/오류/경고 메시지 출력 */
		 /* PCI/NVMe: 후속 코드 동작 수행 */
		 dev->vendor, dev->device, dev->hdr_type, dev->class,
		 /* PCI/NVMe: 장치 타입 문자열 반환 */
		 pci_type_str(dev));

	/* Device class may be changed after fixup */
	/* PCI/NVMe: 비트 연산으로 값 설정/마스크: class */
	class = dev->class >> 8;

	/* PCI/NVMe: 조걸 분기, 값 크기 비교 */
	if (dev->non_compliant_bars && !dev->mmio_always_on) {
		/* PCI/NVMe: PCI config space 16-bit 읽기. COMMAND/STATUS 등 워드 단위 접근 */
		pci_read_config_word(dev, PCI_COMMAND, &cmd); /* PCI/NVMe: PCI config space 읽기. NVMe capability/CSR 탐색 */
		/* PCI/NVMe: 조걸 분기, 플래그/비트 마스크 검사 */
		if (cmd & (PCI_COMMAND_IO | PCI_COMMAND_MEMORY)) {
			/* PCI/NVMe: pci_info() 진단 메시지 출력. NVMe 초기화/리소스 상태 기록 */
			pci_info(dev, "device has non-compliant BARs; disabling IO/MEM decoding\n"); /* PCI/NVMe: 진단/오류/경고 메시지 출력 */
			/* PCI/NVMe: 비트 연산으로 값 설정/마스크: cmd & */
			cmd &= ~PCI_COMMAND_IO;
			/* PCI/NVMe: 비트 연산으로 값 설정/마스크: cmd & */
			cmd &= ~PCI_COMMAND_MEMORY;
			/* PCI/NVMe: PCI config space 16-bit 쓰기. COMMAND decode/MSE/IOSE 비트 제어 */
			pci_write_config_word(dev, PCI_COMMAND, cmd); /* PCI/NVMe: PCI config space 쓰기. NVMe 레지스터/비트 제어 */
		}
	}

	/* PCI/NVMe: INTx 마스크 쓰기 가능 여부 검사 */
	dev->broken_intx_masking = pci_intx_mask_broken(dev);

	switch (dev->hdr_type) {		    /* header type */
	case PCI_HEADER_TYPE_NORMAL:		    /* standard header */
		/* PCI/NVMe: 조걸 분기, 값 일치 여부 검사 */
		if (class == PCI_CLASS_BRIDGE_PCI)
			/* PCI/NVMe: 오류 처리/종료 지점으로 이동: bad */
			goto bad;
		/* PCI/NVMe: INTx 인터럽트 라인 읽기 */
		pci_read_irq(dev);
		/* PCI/NVMe: 표준 BAR 6개 일괄 읽기. NVMe PCIe 엔드포인트 리소스 획득 */
		pci_read_bases(dev, PCI_STD_NUM_BARS, PCI_ROM_ADDRESS);

		/* PCI/NVMe: 서브시스템 ID 읽기 */
		pci_subsystem_ids(dev, &dev->subsystem_vendor, &dev->subsystem_device);

		/*
		 * Do the ugly legacy mode stuff here rather than broken chip
		 * quirk code. Legacy mode ATA controllers have fixed
		 * addresses. These are not always echoed in BAR0-3, and
		 * BAR0-3 in a few cases contain junk!
		 */
		/* PCI/NVMe: 조걸 분기, 값 일치 여부 검사 */
		if (class == PCI_CLASS_STORAGE_IDE) {
			/* PCI/NVMe: 지역 변수 선언 및 초기화 */
			u8 progif;
			/* PCI/NVMe: PCI config space 8-bit 읽기. secondary/subordinate bus 등 바이트 접근 */
			pci_read_config_byte(dev, PCI_CLASS_PROG, &progif); /* PCI/NVMe: PCI config space 읽기. NVMe capability/CSR 탐색 */
			/* PCI/NVMe: 조걸 분기, 값 일치 여부 검사 */
			if ((progif & 1) == 0) {
				/* PCI/NVMe: region.start 설정: 리소스 시작 주소(NVMe BAR 매핑 범위) */
				region.start = 0x1F0;
				/* PCI/NVMe: region.end 설정: 리소스 끝 주소(NVMe BAR 매핑 범위) */
				region.end = 0x1F7;
				/* PCI/NVMe: 비트 연산으로 값 설정/마스크: res */
				res = &dev->resource[0];
				/* PCI/NVMe: 구조체 필드에 값 저장: res->flags */
				res->flags = LEGACY_IO_RESOURCE;
				/* PCI/NVMe: bus 주소를 CPU physical address로 변환. NVMe BAR 매핑 시 사용 */
				pcibios_bus_to_resource(dev->bus, res, &region);
				/* PCI/NVMe: pci_info() 진단 메시지 출력. NVMe 초기화/리소스 상태 기록 */
				pci_info(dev, "BAR 0 %pR: legacy IDE quirk\n", /* PCI/NVMe: 진단/오류/경고 메시지 출력 */
					 /* PCI/NVMe: 후속 코드 동작 수행 */
					 res);
				/* PCI/NVMe: region.start 설정: 리소스 시작 주소(NVMe BAR 매핑 범위) */
				region.start = 0x3F6;
				/* PCI/NVMe: region.end 설정: 리소스 끝 주소(NVMe BAR 매핑 범위) */
				region.end = 0x3F6;
				/* PCI/NVMe: 비트 연산으로 값 설정/마스크: res */
				res = &dev->resource[1];
				/* PCI/NVMe: 구조체 필드에 값 저장: res->flags */
				res->flags = LEGACY_IO_RESOURCE;
				/* PCI/NVMe: bus 주소를 CPU physical address로 변환. NVMe BAR 매핑 시 사용 */
				pcibios_bus_to_resource(dev->bus, res, &region);
				/* PCI/NVMe: pci_info() 진단 메시지 출력. NVMe 초기화/리소스 상태 기록 */
				pci_info(dev, "BAR 1 %pR: legacy IDE quirk\n", /* PCI/NVMe: 진단/오류/경고 메시지 출력 */
					 /* PCI/NVMe: 후속 코드 동작 수행 */
					 res);
			}
			/* PCI/NVMe: 조걸 분기, 값 일치 여부 검사 */
			if ((progif & 4) == 0) {
				/* PCI/NVMe: region.start 설정: 리소스 시작 주소(NVMe BAR 매핑 범위) */
				region.start = 0x170;
				/* PCI/NVMe: region.end 설정: 리소스 끝 주소(NVMe BAR 매핑 범위) */
				region.end = 0x177;
				/* PCI/NVMe: 비트 연산으로 값 설정/마스크: res */
				res = &dev->resource[2];
				/* PCI/NVMe: 구조체 필드에 값 저장: res->flags */
				res->flags = LEGACY_IO_RESOURCE;
				/* PCI/NVMe: bus 주소를 CPU physical address로 변환. NVMe BAR 매핑 시 사용 */
				pcibios_bus_to_resource(dev->bus, res, &region);
				/* PCI/NVMe: pci_info() 진단 메시지 출력. NVMe 초기화/리소스 상태 기록 */
				pci_info(dev, "BAR 2 %pR: legacy IDE quirk\n", /* PCI/NVMe: 진단/오류/경고 메시지 출력 */
					 /* PCI/NVMe: 후속 코드 동작 수행 */
					 res);
				/* PCI/NVMe: region.start 설정: 리소스 시작 주소(NVMe BAR 매핑 범위) */
				region.start = 0x376;
				/* PCI/NVMe: region.end 설정: 리소스 끝 주소(NVMe BAR 매핑 범위) */
				region.end = 0x376;
				/* PCI/NVMe: 비트 연산으로 값 설정/마스크: res */
				res = &dev->resource[3];
				/* PCI/NVMe: 구조체 필드에 값 저장: res->flags */
				res->flags = LEGACY_IO_RESOURCE;
				/* PCI/NVMe: bus 주소를 CPU physical address로 변환. NVMe BAR 매핑 시 사용 */
				pcibios_bus_to_resource(dev->bus, res, &region);
				/* PCI/NVMe: pci_info() 진단 메시지 출력. NVMe 초기화/리소스 상태 기록 */
				pci_info(dev, "BAR 3 %pR: legacy IDE quirk\n", /* PCI/NVMe: 진단/오류/경고 메시지 출력 */
					 /* PCI/NVMe: 후속 코드 동작 수행 */
					 res);
			}
		}
		break;

	case PCI_HEADER_TYPE_BRIDGE:		    /* bridge header */
		/*
		 * The PCI-to-PCI bridge spec requires that subtractive
		 * decoding (i.e. transparent) bridge must have programming
		 * interface code of 0x01.
		 */
		/* PCI/NVMe: INTx 인터럽트 라인 읽기 */
		pci_read_irq(dev);
		/* PCI/NVMe: 구조체 필드에 비트 마스크 적용: dev->transparent */
		dev->transparent = ((dev->class & 0xff) == 1);
		/* PCI/NVMe: 표준 BAR 6개 일괄 읽기. NVMe PCIe 엔드포인트 리소스 획득 */
		pci_read_bases(dev, 2, PCI_ROM_ADDRESS1);
		/* PCI/NVMe: 브리지 윈도우 일괄 읽기 */
		pci_read_bridge_windows(dev);
		/* PCI/NVMe: 핫플러그 브리지 플래그 설정 */
		set_pcie_hotplug_bridge(dev);
		/* PCI/NVMe: PCI capability 위치 탐색 */
		pos = pci_find_capability(dev, PCI_CAP_ID_SSVID); /* PCI/NVMe: NVMe MSI-X/PCIe/AER capability 탐색 */
		/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
		if (pos) {
			/* PCI/NVMe: PCI config space 16-bit 읽기. COMMAND/STATUS 등 워드 단위 접근 */
			pci_read_config_word(dev, pos + PCI_SSVID_VENDOR_ID, &dev->subsystem_vendor); /* PCI/NVMe: PCI config space 읽기. NVMe capability/CSR 탐색 */
			/* PCI/NVMe: PCI config space 16-bit 읽기. COMMAND/STATUS 등 워드 단위 접근 */
			pci_read_config_word(dev, pos + PCI_SSVID_DEVICE_ID, &dev->subsystem_device); /* PCI/NVMe: PCI config space 읽기. NVMe capability/CSR 탐색 */
		}
		break;

	case PCI_HEADER_TYPE_CARDBUS:		    /* CardBus bridge header */
		/* PCI/NVMe: 조걸 분기, 값 불일치 여부 검사 */
		if (class != PCI_CLASS_BRIDGE_CARDBUS)
			/* PCI/NVMe: 오류 처리/종료 지점으로 이동: bad */
			goto bad;
		/* PCI/NVMe: INTx 인터럽트 라인 읽기 */
		pci_read_irq(dev);
		/* PCI/NVMe: 표준 BAR 6개 일괄 읽기. NVMe PCIe 엔드포인트 리소스 획득 */
		pci_read_bases(dev, 1, 0);
		/* PCI/NVMe: PCI config space 16-bit 읽기. COMMAND/STATUS 등 워드 단위 접근 */
		pci_read_config_word(dev, PCI_CB_SUBSYSTEM_VENDOR_ID, &dev->subsystem_vendor); /* PCI/NVMe: PCI config space 읽기. NVMe capability/CSR 탐색 */
		/* PCI/NVMe: PCI config space 16-bit 읽기. COMMAND/STATUS 등 워드 단위 접근 */
		pci_read_config_word(dev, PCI_CB_SUBSYSTEM_ID, &dev->subsystem_device); /* PCI/NVMe: PCI config space 읽기. NVMe capability/CSR 탐색 */
		break;

	default:				    /* unknown header */
		/* PCI/NVMe: pci_err() 오류 메시지 출력. NVMe 장치 초기화 실패 기록 */
		pci_err(dev, "unknown header type %02x, ignoring device\n", /* PCI/NVMe: 진단/오류/경고 메시지 출력 */
			/* PCI/NVMe: 후속 코드 동작 수행 */
			dev->hdr_type);
		/* PCI/NVMe: OF node 레퍼런스 해제 */
		pci_release_of_node(dev);
		/* PCI/NVMe: 오류 코드 반환: -EIO */
		return -EIO;

	bad:
		/* PCI/NVMe: pci_err() 오류 메시지 출력. NVMe 장치 초기화 실패 기록 */
		pci_err(dev, "ignoring class %#08x (doesn't match header type %02x)\n", /* PCI/NVMe: 진단/오류/경고 메시지 출력 */
			/* PCI/NVMe: 후속 코드 동작 수행 */
			dev->class, dev->hdr_type);
		/* PCI/NVMe: 구조체 필드에 비트 마스크 적용: dev->class */
		dev->class = PCI_CLASS_NOT_DEFINED << 8;
	}

	/* We found a fine healthy device, go go go... */
	/* PCI/NVMe: 정상 종료 및 반환 */
	return 0;
}

/* PCI/NVMe: pci_configure_mps 함수 정의 */
static void pci_configure_mps(struct pci_dev *dev)
{
	/* PCI/NVMe: 상위 브리지 획득 */
	struct pci_dev *bridge = pci_upstream_bridge(dev);
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	int mps, mpss, p_mps, rc;

	/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
	if (!pci_is_pcie(dev))
		return;

	/* MPS and MRRS fields are of type 'RsvdP' for VFs, short-circuit out */
	/* PCI/NVMe: 조걸 분기, 값 크기 비교 */
	if (dev->is_virtfn)
		return;

	/*
	 * For Root Complex Integrated Endpoints, program the maximum
	 * supported value unless limited by the PCIE_BUS_PEER2PEER case.
	 */
	/* PCI/NVMe: 조걸 분기, 값 일치 여부 검사 */
	if (pci_pcie_type(dev) == PCI_EXP_TYPE_RC_END) {
		/* PCI/NVMe: 조걸 분기, 값 일치 여부 검사 */
		if (pcie_bus_config == PCIE_BUS_PEER2PEER)
			/* PCI/NVMe: 변수에 값 할당: mps */
			mps = 128;
		/* PCI/NVMe: 조걸 분기의 else 경로 */
		else
			/* PCI/NVMe: 비트 연산으로 값 설정/마스크: mps */
			mps = 128 << dev->pcie_mpss;
		/* PCI/NVMe: MPS 레지스터 쓰기 */
		rc = pcie_set_mps(dev, mps);
		/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
		if (rc) {
			/* PCI/NVMe: pci_warn() 경고 메시지 출력. NVMe 성능/호환성 문제 알림 */
			pci_warn(dev, "can't set Max Payload Size to %d; if necessary, use \"pci=pcie_bus_safe\" and report a bug\n",
				 /* PCI/NVMe: 후속 코드 동작 수행 */
				 mps);
		}
		return;
	}

	/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
	if (!bridge || !pci_is_pcie(bridge))
		return;

	/* PCI/NVMe: MPS 레지스터 읽기 */
	mps = pcie_get_mps(dev);
	/* PCI/NVMe: MPS 레지스터 읽기 */
	p_mps = pcie_get_mps(bridge);

	/* PCI/NVMe: 조걸 분기, 값 일치 여부 검사 */
	if (mps == p_mps)
		return;

	/* PCI/NVMe: 조걸 분기, 값 일치 여부 검사 */
	if (pcie_bus_config == PCIE_BUS_TUNE_OFF) {
		/* PCI/NVMe: pci_warn() 경고 메시지 출력. NVMe 성능/호환성 문제 알림 */
		pci_warn(dev, "Max Payload Size %d, but upstream %s set to %d; if necessary, use \"pci=pcie_bus_safe\" and report a bug\n",
			 /* PCI/NVMe: pci_name()으로 BDF 문자열 획득, 리소스 이름으로 사용 */
			 mps, pci_name(bridge), p_mps);
		return;
	}

	/*
	 * Fancier MPS configuration is done later by
	 * pcie_bus_configure_settings()
	 */
	/* PCI/NVMe: 조걸 분기, 값 불일치 여부 검사 */
	if (pcie_bus_config != PCIE_BUS_DEFAULT)
		return;

	/* PCI/NVMe: 비트 연산으로 값 설정/마스크: mpss */
	mpss = 128 << dev->pcie_mpss;
	/* PCI/NVMe: 조걸 분기, 값 일치 여부 검사 */
	if (mpss < p_mps && pci_pcie_type(bridge) == PCI_EXP_TYPE_ROOT_PORT) {
		/* PCI/NVMe: MPS 레지스터 쓰기 */
		pcie_set_mps(bridge, mpss);
		/* PCI/NVMe: pci_info() 진단 메시지 출력. NVMe 초기화/리소스 상태 기록 */
		pci_info(dev, "Upstream bridge's Max Payload Size set to %d (was %d, max %d)\n", /* PCI/NVMe: 진단/오류/경고 메시지 출력 */
			 /* PCI/NVMe: 후속 코드 동작 수행 */
			 mpss, p_mps, 128 << bridge->pcie_mpss);
		/* PCI/NVMe: MPS 레지스터 읽기 */
		p_mps = pcie_get_mps(bridge);
	}

	/* PCI/NVMe: MPS 레지스터 쓰기 */
	rc = pcie_set_mps(dev, p_mps);
	/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
	if (rc) {
		/* PCI/NVMe: pci_warn() 경고 메시지 출력. NVMe 성능/호환성 문제 알림 */
		pci_warn(dev, "can't set Max Payload Size to %d; if necessary, use \"pci=pcie_bus_safe\" and report a bug\n",
			 /* PCI/NVMe: 후속 코드 동작 수행 */
			 p_mps);
		return;
	}

	/* PCI/NVMe: pci_info() 진단 메시지 출력. NVMe 초기화/리소스 상태 기록 */
	pci_info(dev, "Max Payload Size set to %d (was %d, max %d)\n", /* PCI/NVMe: 진단/오류/경고 메시지 출력 */
		 /* PCI/NVMe: 후속 코드 동작 수행 */
		 p_mps, mps, mpss);
}

/* PCI/NVMe: PCIe Extended Tag Field 활성화. NVMe 큐 깊이/동시 outstanding IO 향상 */
/* PCI/NVMe: pci_configure_extended_tags 함수 정의 */
int pci_configure_extended_tags(struct pci_dev *dev, void *ign)
{
	/* PCI/NVMe: 후속 코드 동작 수행 */
	struct pci_host_bridge *host;
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	u32 cap;
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	u16 ctl;
	/* PCI/NVMe: ret 변수 선언/초기화: 반환값 변수. NVMe 초기화 성공/실패 상태 */
	int ret;

	/* PCI_EXP_DEVCTL_EXT_TAG is RsvdP in VFs */
	/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
	if (!pci_is_pcie(dev) || dev->is_virtfn)
		/* PCI/NVMe: 정상 종료 및 반환 */
		return 0;

	/* PCI/NVMe: PCIe capability 4바이트 읽기 */
	ret = pcie_capability_read_dword(dev, PCI_EXP_DEVCAP, &cap);
	/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
	if (ret)
		/* PCI/NVMe: 정상 종료 및 반환 */
		return 0;

	/* PCI/NVMe: 조걸 분기, 플래그/비트 마스크 검사 */
	if (!(cap & PCI_EXP_DEVCAP_EXT_TAG))
		/* PCI/NVMe: 정상 종료 및 반환 */
		return 0;

	/* PCI/NVMe: PCIe capability 2바이트 읽기 */
	ret = pcie_capability_read_word(dev, PCI_EXP_DEVCTL, &ctl);
	/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
	if (ret)
		/* PCI/NVMe: 정상 종료 및 반환 */
		return 0;

	/* PCI/NVMe: 호스트 브리지 검색 */
	host = pci_find_host_bridge(dev->bus);
	/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
	if (!host)
		/* PCI/NVMe: 정상 종료 및 반환 */
		return 0;

	/*
	 * If some device in the hierarchy doesn't handle Extended Tags
	 * correctly, make sure they're disabled.
	 */
	/* PCI/NVMe: 조걸 분기, 값 크기 비교 */
	if (host->no_ext_tags) {
		/* PCI/NVMe: 조걸 분기, 플래그/비트 마스크 검사 */
		if (ctl & PCI_EXP_DEVCTL_EXT_TAG) {
			/* PCI/NVMe: pci_info() 진단 메시지 출력. NVMe 초기화/리소스 상태 기록 */
			pci_info(dev, "disabling Extended Tags\n"); /* PCI/NVMe: 진단/오류/경고 메시지 출력 */
			/* PCI/NVMe: PCIe capability 비트 클리어 */
			pcie_capability_clear_word(dev, PCI_EXP_DEVCTL,
						   /* PCI/NVMe: 후속 코드 동작 수행 */
						   PCI_EXP_DEVCTL_EXT_TAG);
		}
		/* PCI/NVMe: 정상 종료 및 반환 */
		return 0;
	}

	/* PCI/NVMe: 조걸 분기, 플래그/비트 마스크 검사 */
	if (!(ctl & PCI_EXP_DEVCTL_EXT_TAG)) {
		/* PCI/NVMe: pci_info() 진단 메시지 출력. NVMe 초기화/리소스 상태 기록 */
		pci_info(dev, "enabling Extended Tags\n"); /* PCI/NVMe: 진단/오류/경고 메시지 출력 */
		/* PCI/NVMe: PCIe capability 비트 설정 */
		pcie_capability_set_word(dev, PCI_EXP_DEVCTL,
					 /* PCI/NVMe: 후속 코드 동작 수행 */
					 PCI_EXP_DEVCTL_EXT_TAG);
	}
	/* PCI/NVMe: 정상 종료 및 반환 */
	return 0;
}

/*
 * pci_dev3_init() - PCI Express Device 3(PCIe 6.0+) capability 초기화
 *
 * NVMe 연결: Device 3 capability는 FLIT 모드, Retry buffer 등 PCIe 6.0
 * 이상의 기능을 노출. FLIT 모드는 NVMe PCIe 6.0 SSD의 데이터 링크
 * 효율에 영향을 줄 수 있다(추정).
 */
/* PCI/NVMe: pci_dev3_init 함수 정의 */
static void pci_dev3_init(struct pci_dev *pdev)
{
	u16 cap = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_DEV3); /* PCI/NVMe: Device 3 extended capability 탐색 */
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	u32 val = 0;

	if (!cap) /* PCI/NVMe: capability가 없으면 초기화 불필요 */
		return;
	pci_read_config_dword(pdev, cap + PCI_DEV3_STA, &val); /* PCI/NVMe: Device 3 Status 레지스터 읽기 */
	pdev->fm_enabled = !!(val & PCI_DEV3_STA_SEGMENT); /* PCI/NVMe: FLIT 모드 segment 지원 여부를 pci_dev에 기록 */
}

/**
 * pcie_relaxed_ordering_enabled - Probe for PCIe relaxed ordering enable
 * @dev: PCI device to query
 *
 * Returns true if the device has enabled relaxed ordering attribute.
 */
/*
 * pcie_relaxed_ordering_enabled() - 장치의 Relaxed Ordering 활성화 여부 반환
 *
 * NVMe 연결: NVMe queue 간 순서 독립성이 높아 RO(Relaxed Ordering)를
 * 활성화하면 링크 사용률을 높일 수 있다. 이 함수는 다른 드라이버나
 * quirk에서 RO 상태를 확인할 때 사용.
 */
/* PCI/NVMe: Relaxed Ordering 활성화 여부 확인. NVMe 성능/일관성 튜닝 참고 */
/* PCI/NVMe: pcie_relaxed_ordering_enabled 함수 정의 */
bool pcie_relaxed_ordering_enabled(struct pci_dev *dev)
{
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	u16 v;

	pcie_capability_read_word(dev, PCI_EXP_DEVCTL, &v); /* PCI/NVMe: Device Control 레지스터 읽기 */

	return !!(v & PCI_EXP_DEVCTL_RELAX_EN); /* PCI/NVMe: Relaxed Ordering Enable bit 반환 */
}
/* PCI/NVMe: EXPORT_SYMBOL 함수 호출 */
EXPORT_SYMBOL(pcie_relaxed_ordering_enabled); /* PCI/NVMe: 심볼 외부 낸출. nvme-pci 등 모듈에서 참조 가능 */

/*
 * pci_configure_relaxed_ordering() - Root Port가 RO를 지원하지 않으면
 *                                     endpoint의 RO를 비활성화
 *
 * NVMe 연결: 일부 Root Port는 Relaxed Ordering을 제대로 지원하지 않아
 * 데이터 무결성 문제를 일으킬 수 있다. NVMe endpoint의 RO를 비활성화하여
 * 안정성을 확보.
 */
/* PCI/NVMe: pci_configure_relaxed_ordering 함수 정의 */
static void pci_configure_relaxed_ordering(struct pci_dev *dev)
{
	/* PCI/NVMe: 후속 코드 동작 수행 */
	struct pci_dev *root;

	/* PCI_EXP_DEVCTL_RELAX_EN is RsvdP in VFs */
	if (dev->is_virtfn) /* PCI/NVMe: VF는 RO bit가 예약되어 있으므로 건드리지 않음 */
		return;

	if (!pcie_relaxed_ordering_enabled(dev)) /* PCI/NVMe: 이미 RO가 꺼져 있으면 할 것 없음 */
		return;

	/*
	 * For now, we only deal with Relaxed Ordering issues with Root
	 * Ports. Peer-to-Peer DMA is another can of worms.
	 */
	root = pcie_find_root_port(dev); /* PCI/NVMe: NVMe 장치의 Root Port 탐색 */
	if (!root) /* PCI/NVMe: Root Port를 찾을 수 없으면(가상화 등) 처리 불가 */
		return;

	if (root->dev_flags & PCI_DEV_FLAGS_NO_RELAXED_ORDERING) { /* PCI/NVMe: Root Port가 RO 미지원으로 표시되어 있으면 */
		/* PCI/NVMe: PCIe capability 비트 클리어 */
		pcie_capability_clear_word(dev, PCI_EXP_DEVCTL,
					   PCI_EXP_DEVCTL_RELAX_EN); /* PCI/NVMe: NVMe endpoint의 RO 비활성화 */
		/* PCI/NVMe: pci_info() 진단 메시지 출력. NVMe 초기화/리소스 상태 기록 */
		pci_info(dev, "Relaxed Ordering disabled because the Root Port didn't support it\n"); /* PCI/NVMe: 진단/오류/경고 메시지 출력 */
	}
}

/*
 * pci_configure_eetlp_prefix() - End-to-End TLP Prefix 지원 크기 설정
 *
 * NVMe 연결: EETLP Prefix는 PCIe 3.1 이상의 고급 기능으로, TLP에
 * 추가 메타데이터를 담을 수 있다. NVMe와 직접 관련은 적으나, prefix
 * 지원 여부는 향상된 에러 보고나 보안 확장에 사용될 수 있다.
 */
/* PCI/NVMe: pci_configure_eetlp_prefix 함수 정의 */
static void pci_configure_eetlp_prefix(struct pci_dev *dev)
{
	/* PCI/NVMe: 후속 코드 동작 수행 */
	struct pci_dev *bridge;
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	unsigned int eetlp_max;
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	int pcie_type;
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	u32 cap;

	if (!pci_is_pcie(dev)) /* PCI/NVMe: PCIe 장치가 아니면 EETLP prefix 불가 */
		return;

	pcie_capability_read_dword(dev, PCI_EXP_DEVCAP2, &cap); /* PCI/NVMe: Device Capabilities 2 레지스터 읽기 */
	if (!(cap & PCI_EXP_DEVCAP2_EE_PREFIX)) /* PCI/NVMe: EETLP Prefix 지원 bit 확인 */
		return;

	pcie_type = pci_pcie_type(dev); /* PCI/NVMe: 장치의 PCIe type 확인 */

	eetlp_max = FIELD_GET(PCI_EXP_DEVCAP2_EE_PREFIX_MAX, cap); /* PCI/NVMe: 지원하는 prefix 최대 개수 필드 추출 */
	/* 00b means 4 */
	eetlp_max = eetlp_max ?: 4; /* PCI/NVMe: 0이면 최대 4개로 해석 */

	/* PCI/NVMe: 조걸 분기, NVMe 장치 상태/플래그에 따른 경로 선택 */
	if (pcie_type == PCI_EXP_TYPE_ROOT_PORT ||
	    pcie_type == PCI_EXP_TYPE_RC_END) /* PCI/NVMe: Root Port나 RCiE이면 자신의 prefix 크기 설정 */
		/* PCI/NVMe: 구조체 필드에 값 저장: dev->eetlp_prefix_max */
		dev->eetlp_prefix_max = eetlp_max;
	/* PCI/NVMe: 조걸 분기의 else 경로 */
	else {
		bridge = pci_upstream_bridge(dev); /* PCI/NVMe: 상위 bridge 획득 */
		if (bridge && bridge->eetlp_prefix_max) /* PCI/NVMe: 상위 bridge가 prefix를 지원하면 endpoint도 설정 */
			/* PCI/NVMe: 구조체 필드에 값 저장: dev->eetlp_prefix_max */
			dev->eetlp_prefix_max = eetlp_max;
	}
}

/*
 * pci_configure_serr() - bridge의 SERR# forwarding 활성화
 *
 * NVMe 연결: bridge가 SERR# forwarding을 하지 않으면 NVMe endpoint에서
 * 발생한 PCIe fatal/non-fatal error(ERR_COR/ERR_NONFATAL/ERR_FATAL)가
 * Root Complex에 도달하지 못해 AER 인터럽트가 발생하지 않는다.
 */
/* PCI/NVMe: pci_configure_serr 함수 정의 */
static void pci_configure_serr(struct pci_dev *dev)
{
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	u16 control;

	if (dev->hdr_type == PCI_HEADER_TYPE_BRIDGE) { /* PCI/NVMe: bridge 장치에만 적용 */

		/*
		 * A bridge will not forward ERR_ messages coming from an
		 * endpoint unless SERR# forwarding is enabled.
		 */
		pci_read_config_word(dev, PCI_BRIDGE_CONTROL, &control); /* PCI/NVMe: Bridge Control 레지스터 읽기 */
		if (!(control & PCI_BRIDGE_CTL_SERR)) { /* PCI/NVMe: SERR forwarding이 꺼져 있으면 */
			control |= PCI_BRIDGE_CTL_SERR; /* PCI/NVMe: SERR forwarding enable bit 설정 */
			pci_write_config_word(dev, PCI_BRIDGE_CONTROL, control); /* PCI/NVMe: Bridge Control에 기록 */
		}
	}
}

/*
 * pci_configure_rcb() - Read Completion Boundary를 Root Port와 일치시킴
 *
 * NVMe 연결: RCB는 Memory Read completion을 정렬하는 단위(64B 또는
 * 128B). NVMe DMA read 완료 데이터의 정렬 방식이 RCB에 따라 달라질 수
 * 있어, endpoint의 RCB를 Root Port와 맞추는 것이 바람직하다.
 */
/* PCI/NVMe: pci_configure_rcb 함수 정의 */
static void pci_configure_rcb(struct pci_dev *dev)
{
	/* PCI/NVMe: 후속 코드 동작 수행 */
	struct pci_dev *rp;
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	u16 rp_lnkctl;

	/*
	 * Per PCIe r7.0, sec 7.5.3.7, RCB is only meaningful in Root Ports
	 * (where it is read-only), Endpoints, and Bridges.  It may only be
	 * set for Endpoints and Bridges if it is set in the Root Port. For
	 * Endpoints, it is 'RsvdP' for Virtual Functions.
	 */
	if (!pci_is_pcie(dev) || /* PCI/NVMe: PCIe 장치가 아니면 RCB 의미 없음 */
	    pci_pcie_type(dev) == PCI_EXP_TYPE_ROOT_PORT || /* PCI/NVMe: Root Port의 RCB는 read-only */
	    pci_pcie_type(dev) == PCI_EXP_TYPE_UPSTREAM || /* PCI/NVMe: Switch upstream은 RCB 설정 대상 아님 */
	    pci_pcie_type(dev) == PCI_EXP_TYPE_DOWNSTREAM || /* PCI/NVMe: Switch downstream은 RCB 설정 대상 아님 */
	    pci_pcie_type(dev) == PCI_EXP_TYPE_RC_EC || /* PCI/NVMe: RC Event Collector 제외 */
	    dev->is_virtfn) /* PCI/NVMe: VF는 RCB bit가 RsvdP */
		return;

	/* Root Port often not visible to virtualized guests */
	rp = pcie_find_root_port(dev); /* PCI/NVMe: NVMe 장치의 Root Port 탐색 */
	if (!rp) /* PCI/NVMe: 가상화 등에서 Root Port를 찾지 못하면 처리 불가 */
		return;

	pcie_capability_read_word(rp, PCI_EXP_LNKCTL, &rp_lnkctl); /* PCI/NVMe: Root Port의 Link Control 읽기 */
	/* PCI/NVMe: PCIe capability 비트 클리어 후 설정 */
	pcie_capability_clear_and_set_word(dev, PCI_EXP_LNKCTL,
					   /* PCI/NVMe: 후속 코드 동작 수행 */
					   PCI_EXP_LNKCTL_RCB,
					   /* PCI/NVMe: 후속 코드 동작 수행 */
					   (rp_lnkctl & PCI_EXP_LNKCTL_RCB) ?
					   PCI_EXP_LNKCTL_RCB : 0); /* PCI/NVMe: Root Port의 RCB 값을 NVMe endpoint에 동기화 */
}

/* PCI/NVMe: pci_configure_device 함수 정의 */
static void pci_configure_device(struct pci_dev *dev)
{
	/* PCI/NVMe: Max Payload Size 설정 */
	pci_configure_mps(dev);
	/* PCI/NVMe: Extended Tag 활성화 */
	pci_configure_extended_tags(dev, NULL);
	/* PCI/NVMe: Relaxed Ordering 설정 */
	pci_configure_relaxed_ordering(dev);
	/* PCI/NVMe: Latency Tolerance Reporting 설정 */
	pci_configure_ltr(dev);
	/* PCI/NVMe: ASPM L1 Substates 설정 */
	pci_configure_aspm_l1ss(dev);
	/* PCI/NVMe: End-to-End TLP Prefix 설정 */
	pci_configure_eetlp_prefix(dev);
	/* PCI/NVMe: SERR 포워딩 설정 */
	pci_configure_serr(dev);
	/* PCI/NVMe: Read Completion Boundary 설정 */
	pci_configure_rcb(dev);

	/* PCI/NVMe: ACPI 핫플러그 파라미터 적용 */
	pci_acpi_program_hp_params(dev);
}

/*
 * pci_release_capabilities() - pci_dev의 PCI/PCIe 고급 capability 정리
 *
 * NVMe 연결: NVMe 컨트롤러가 제거되거나 해제될 때 AER, SR-IOV, capability
 * save buffer 등을 정리. MSI-X는 pci_disable_msix() 등에서 별도 처리.
 */
/* PCI/NVMe: pci_release_capabilities 함수 정의 */
static void pci_release_capabilities(struct pci_dev *dev)
{
	pci_aer_exit(dev); /* PCI/NVMe: AER 콜백 및 자원 정리; NVMe 오류 보고 중단 */
	pci_rcec_exit(dev); /* PCI/NVMe: Root Complex Event Collector 관련 정리 */
	pci_iov_release(dev); /* PCI/NVMe: SR-IOV 자원 반납; NVMe VF 정리 */
	pci_free_cap_save_buffers(dev); /* PCI/NVMe: suspend/resume용 capability 저장 버퍼 해제 */
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
 * pci_release_dev() - pci_dev의 최종 해제
 *
 * NVMe 연결: NVMe 컨트롤러의 struct device 참조 카운트가 0이 되면
 * 호출. capability, OF node, DMA alias bitmap, bus 참조 등을 정리하고
 * pci_dev 메모리를 해제.
 */
/* PCI/NVMe: pci_release_dev 함수 정의 */
static void pci_release_dev(struct device *dev)
{
	/* PCI/NVMe: 후속 코드 동작 수행 */
	struct pci_dev *pci_dev;

	pci_dev = to_pci_dev(dev); /* PCI/NVMe: device에서 pci_dev 구조체 획득 */
	pci_release_capabilities(pci_dev); /* PCI/NVMe: AER/SR-IOV/cap save buffer 정리 */
	pci_release_of_node(pci_dev); /* PCI/NVMe: device tree node 참조 해제 */
	pcibios_release_device(pci_dev); /* PCI/NVMe: 아키텍처별 device 해제 처리 */
	pci_bus_put(pci_dev->bus); /* PCI/NVMe: bus 참조 카운트 감소 */
	bitmap_free(pci_dev->dma_alias_mask); /* PCI/NVMe: DMA alias bitmap 해제; NVMe DMA alias 정리 */
	dev_dbg(dev, "device released\n"); /* PCI/NVMe: device 해제 로그 */
	kfree(pci_dev); /* PCI/NVMe: pci_dev 메모리 해제 */
}

/* PCI/NVMe: 지역 변수 선언 및 초기화 */
static const struct device_type pci_dev_type = {
	/* PCI/NVMe: .groups 설정: sysfs attribute group */
	.groups = pci_dev_attr_groups,
};

/*
 * pci_alloc_dev() - 새 PCI 디바이스를 표현할 struct pci_dev 할당
 *
 * NVMe 연결: nvme_pci_driver가 바인딩될 대상 pci_dev가 여기서 생성된다.
 * dev->resource[]는 아직 비어 있고, 뒤의 pci_setup_device()에서
 * BAR/IRQ/클록 등을 채운다. msi_lock은 MSI-X 벡터 할당 시 사용.
 */
/* PCI/NVMe: PCI 장치 구조체 할당 */
struct pci_dev *pci_alloc_dev(struct pci_bus *bus)
{
	/* PCI/NVMe: 후속 코드 동작 수행 */
	struct pci_dev *dev;

	dev = kzalloc_obj(struct pci_dev); /* PCI/NVMe: pci_dev 구조체를 0으로 초기화하며 할당 */
	if (!dev) /* PCI/NVMe: 메모리 부족 시 NULL 반환 */
		/* PCI/NVMe: NULL 반환(메모리/리소스 부족 또는 초기화 실패) */
		return NULL;

	INIT_LIST_HEAD(&dev->bus_list); /* PCI/NVMe: bus devices 리스트 연결용 node 초기화 */
	dev->dev.type = &pci_dev_type; /* PCI/NVMe: PCI device type 설정 */
	dev->bus = pci_bus_get(bus); /* PCI/NVMe: 소속 bus 참조 카운트 증가 */
	/* PCI/NVMe: 구조체 필드에 값 저장: dev->driver_exclusive_resource */
	dev->driver_exclusive_resource = (struct resource) {
		/* PCI/NVMe: .name 설정: 리소스/장치 식별자 */
		.name = "PCI Exclusive",
		/* PCI/NVMe: .start 설정: 리소스 시작 주소(NVMe BAR 매핑 범위) */
		.start = 0,
		/* PCI/NVMe: .end 설정: 리소스 끝 주소(NVMe BAR 매핑 범위) */
		.end = -1,
	}; /* PCI/NVMe: 드라이버 전용 resource 범위 초기화 */

	spin_lock_init(&dev->pcie_cap_lock); /* PCI/NVMe: PCIe capability 접근 보호용 spinlock 초기화 */
/* PCI/NVMe: 컴파일 조건: CONFIG_PCI_MSI 정의 시 포함 */
#ifdef CONFIG_PCI_MSI
	raw_spin_lock_init(&dev->msi_lock); /* PCI/NVMe: MSI/MSI-X 할당 보호용 raw spinlock 초기화; NVMe per-queue MSI-X에 사용 */
/* PCI/NVMe: 컴파일 조건 종료 */
#endif
	return dev; /* PCI/NVMe: 초기화된 pci_dev 반환 */
}
/* PCI/NVMe: EXPORT_SYMBOL 함수 호출 */
EXPORT_SYMBOL(pci_alloc_dev); /* PCI/NVMe: 심볼 외부 낸출. nvme-pci 등 모듈에서 참조 가능 */

/*
 * pci_bus_wait_rrs() - Configuration Request Retry Status 대기
 *
 * NVMe 연결: NVMe 컨트롤러가 부팅 직후 아직 준비되지 않았을 때,
 * config read는 RRS(예약된 vendor ID)로 완료될 수 있다. 이 함수는
 * 지수 백오프로 재시도하여 컨트롤러가 준비될 때까지 기다린다.
 */
/* PCI/NVMe: Configuration RRS 대기 */
static bool pci_bus_wait_rrs(struct pci_bus *bus, int devfn, u32 *l,
			     /* PCI/NVMe: 후속 코드 동작 수행 */
			     int timeout)
{
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	int delay = 1;

	/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
	if (!pci_bus_rrs_vendor_id(*l))
		return true;	/* not a Configuration RRS completion */ /* PCI/NVMe: RRS가 아니면 즉시 true */

	/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
	if (!timeout)
		return false;	/* RRS, but caller doesn't want to wait */ /* PCI/NVMe: 대기를 원하지 않으면 false */

	/*
	 * We got the reserved Vendor ID that indicates a completion with
	 * Configuration Request Retry Status (RRS).  Retry until we get a
	 * valid Vendor ID or we time out.
	 */
	while (pci_bus_rrs_vendor_id(*l)) { /* PCI/NVMe: RRS가 해제될 때까지 반복 */
		if (delay > timeout) { /* PCI/NVMe: timeout 초과 시 */
			/* PCI/NVMe: 커널 경고 메시지 출력 */
			pr_warn("pci %04x:%02x:%02x.%d: not ready after %dms; giving up\n",
				/* PCI/NVMe: PCI 도메인 번호 획득 */
				pci_domain_nr(bus), bus->number,
				PCI_SLOT(devfn), PCI_FUNC(devfn), delay - 1); /* PCI/NVMe: NVMe 장치 준비 실패 경고 */

			/* PCI/NVMe: 거짓 반환 */
			return false;
		}
		/* PCI/NVMe: 조걸 분기, 값 크기 비교 */
		if (delay >= 1000)
			/* PCI/NVMe: 커널 정보 메시지 출력 */
			pr_info("pci %04x:%02x:%02x.%d: not ready after %dms; waiting\n",
				/* PCI/NVMe: PCI 도메인 번호 획득 */
				pci_domain_nr(bus), bus->number,
				PCI_SLOT(devfn), PCI_FUNC(devfn), delay - 1); /* PCI/NVMe: 1초 이상 대기 시 정보 로그 */

		msleep(delay); /* PCI/NVMe: 지연 시간만큼 대기 */
		delay *= 2; /* PCI/NVMe: 지수 백오프로 대기 시간 증가 */

		/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
		if (pci_bus_read_config_dword(bus, devfn, PCI_VENDOR_ID, l))
			return false; /* PCI/NVMe: config read 자체가 실패하면 false */
	}

	/* PCI/NVMe: 조걸 분기, 값 크기 비교 */
	if (delay >= 1000)
		/* PCI/NVMe: 커널 정보 메시지 출력 */
		pr_info("pci %04x:%02x:%02x.%d: ready after %dms\n",
			/* PCI/NVMe: PCI 도메인 번호 획득 */
			pci_domain_nr(bus), bus->number,
			PCI_SLOT(devfn), PCI_FUNC(devfn), delay - 1); /* PCI/NVMe: 준비 완료 로그 */

	return true; /* PCI/NVMe: 유효한 vendor ID 획득 */
}

/*
 * pci_bus_generic_read_dev_vendor_id() - slot에서 vendor/device ID 읽기
 *
 * NVMe 연결: bus/dev/function 위치에 실제로 장치(예: NVMe 컨트롤러)가
 * 있는지 확인하는 첫 단계. 유효한 vendor ID가 읽히면 해당 위치에
 * pci_dev를 할당하고 계속 초기화.
 */
/* PCI/NVMe: Vendor/Device ID 일반 읽기 */
bool pci_bus_generic_read_dev_vendor_id(struct pci_bus *bus, int devfn, u32 *l,
					/* PCI/NVMe: 후속 코드 동작 수행 */
					int timeout)
{
	if (pci_bus_read_config_dword(bus, devfn, PCI_VENDOR_ID, l)) /* PCI/NVMe: 설정 공간 0x00에서 vendor/device ID 읽기 시도 */
		return false; /* PCI/NVMe: 읽기 실패 시 slot이 비어있거나 접근 불가 */

	/* Some broken boards return 0 or ~0 (PCI_ERROR_RESPONSE) if a slot is empty: */
	/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
	if (PCI_POSSIBLE_ERROR(*l) || *l == 0x00000000 ||
	    *l == 0x0000ffff || *l == 0xffff0000) /* PCI/NVMe: empty slot이나 error response이면 장치 없음 */
		/* PCI/NVMe: 거짓 반환 */
		return false;

	if (pci_bus_rrs_vendor_id(*l)) /* PCI/NVMe: RRS(예약 vendor ID)이면 */
		return pci_bus_wait_rrs(bus, devfn, l, timeout); /* PCI/NVMe: 장치 준비될 때까지 대기 */

	return true; /* PCI/NVMe: 유효한 vendor/device ID 획득; NVMe 컨트롤러 탐색 성공 */
}

/*
 * pci_bus_read_dev_vendor_id() - vendor/device ID 읽기의 아키텍처 기본 래퍼
 *
 * NVMe 연결: 대부분의 플랫폼에서 pci_bus_generic_read_dev_vendor_id()를
 * 직접 호출. NVMe 컨트롤러 탐색의 출발점.
 */
/* PCI/NVMe: pci_bus_read_dev_vendor_id 함수 호출 */
bool pci_bus_read_dev_vendor_id(struct pci_bus *bus, int devfn, u32 *l,
				/* PCI/NVMe: 후속 코드 동작 수행 */
				int timeout)
{
	return pci_bus_generic_read_dev_vendor_id(bus, devfn, l, timeout); /* PCI/NVMe: 일반 vendor ID 읽기 경로 */
}
/* PCI/NVMe: EXPORT_SYMBOL 함수 호출 */
EXPORT_SYMBOL(pci_bus_read_dev_vendor_id); /* PCI/NVMe: 심볼 외부 낸출. nvme-pci 등 모듈에서 참조 가능 */

/*
 * Read the config data for a PCI device, sanity-check it,
 * and fill in the dev structure.
 */
/* PCI/NVMe: PCI config space 읽어 장치 구조체 채우기 */
static struct pci_dev *pci_scan_device(struct pci_bus *bus, int devfn)
{
	/* PCI/NVMe: 후속 코드 동작 수행 */
	struct pci_dev *dev;
	/* PCI/NVMe: l 변수 선언/초기화: config space 읽은 32-bit 값. NVMe capability/CSR 원시 데이터 */
	u32 l;

	/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
	if (!pci_bus_read_dev_vendor_id(bus, devfn, &l, 60*1000))
		/* PCI/NVMe: NULL 반환(메모리/리소스 부족 또는 초기화 실패) */
		return NULL;

	/* PCI/NVMe: PCI 장치 구조체 할당 */
	dev = pci_alloc_dev(bus);
	/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
	if (!dev)
		/* PCI/NVMe: NULL 반환(메모리/리소스 부족 또는 초기화 실패) */
		return NULL;

	/* PCI/NVMe: 구조체 필드에 값 저장: dev->devfn */
	dev->devfn = devfn;
	/* PCI/NVMe: 구조체 필드에 비트 마스크 적용: dev->vendor */
	dev->vendor = l & 0xffff;
	/* PCI/NVMe: 구조체 필드에 비트 마스크 적용: dev->device */
	dev->device = (l >> 16) & 0xffff;

	/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
	if (pci_setup_device(dev)) {
		/* PCI/NVMe: 버스 레퍼런스 해제 */
		pci_bus_put(dev->bus);
		/* PCI/NVMe: 동적 메모리 해제 */
		kfree(dev);
		/* PCI/NVMe: NULL 반환(메모리/리소스 부족 또는 초기화 실패) */
		return NULL;
	}

	/* PCI/NVMe: 결과 반환: dev */
	return dev;
}

/*
 * pcie_report_downtraining() - 링크가 최대 속도/폭보다 낮게 협상되면 경고
 *
 * NVMe 연결: NVMe SSD가 x4 링크에 연결되어야 하는데 x1로 협상되거나,
 * Gen4로 연결되어야 하는데 Gen3로 낮아지면 성능이 크게 저하. 이 함수는
 * pci_init_capabilities()에서 호출되어 문제를 로그에 남긴다.
 */
/* PCI/NVMe: 링크 다운트레이닝 경고. NVMe SSD가 최대 대역폭으로 협상되지 않은 경우 */
/* PCI/NVMe: pcie_report_downtraining 함수 정의 */
void pcie_report_downtraining(struct pci_dev *dev)
{
	if (!pci_is_pcie(dev)) /* PCI/NVMe: PCIe 장치가 아니면 무관 */
		return;

	/* Look from the device up to avoid downstream ports with no devices */
	if ((pci_pcie_type(dev) != PCI_EXP_TYPE_ENDPOINT) && /* PCI/NVMe: Endpoint가 아니고 */
	    (pci_pcie_type(dev) != PCI_EXP_TYPE_LEG_END) && /* PCI/NVMe: Legacy Endpoint가 아니고 */
	    (pci_pcie_type(dev) != PCI_EXP_TYPE_UPSTREAM)) /* PCI/NVMe: Switch Upstream Port도 아니면 skip */
		return;

	/* Multi-function PCIe devices share the same link/status */
	if (PCI_FUNC(dev->devfn) != 0 || dev->is_virtfn) /* PCI/NVMe: 멀티펑션 장치는 function 0에서만 보고; VF는 skip */
		return;

	/* Print link status only if the device is constrained by the fabric */
	__pcie_print_link_status(dev, false); /* PCI/NVMe: 링크가 최대 값보다 낮으면 경고 출력; NVMe 성능 저하 원인 분석 */
}

/*
 * pci_imm_ready_init() - Immediate Readiness Status bit 확인
 *
 * NVMe 연결: Immediate Readiness를 지원하는 장치는 전원 상태 전환 후
 * 추가 지연 없이 config access에 응답. NVMe RTD3 복귀 지연에 영향.
 */
/* PCI/NVMe: pci_imm_ready_init 함수 정의 */
static void pci_imm_ready_init(struct pci_dev *dev)
{
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	u16 status;

	pci_read_config_word(dev, PCI_STATUS, &status); /* PCI/NVMe: PCI Status 레지스터 읽기 */
	if (status & PCI_STATUS_IMM_READY) /* PCI/NVMe: Immediate Readiness bit 확인 */
		dev->imm_ready = 1; /* PCI/NVMe: pci_dev에 즉시 준비 가능 표시 */
}

/* PCI/NVMe: pci_init_capabilities 함수 정의 */
static void pci_init_capabilities(struct pci_dev *dev)
{
	pci_ea_init(dev);		/* Enhanced Allocation */
	pci_msi_init(dev);		/* Disable MSI */
	pci_msix_init(dev);		/* Disable MSI-X */

	/* Buffers for saving PCIe and PCI-X capabilities */
	/* PCI/NVMe: capability 저장 버퍼 할당 */
	pci_allocate_cap_save_buffers(dev);

	pci_imm_ready_init(dev);	/* Immediate Readiness */
	pci_pm_init(dev);		/* Power Management */
	pci_vpd_init(dev);		/* Vital Product Data */
	pci_configure_ari(dev);		/* Alternative Routing-ID Forwarding */
	pci_iov_init(dev);		/* Single Root I/O Virtualization */
	pci_ats_init(dev);		/* Address Translation Services */
	pci_pri_init(dev);		/* Page Request Interface */
	pci_pasid_init(dev);		/* Process Address Space ID */
	pci_acs_init(dev);		/* Access Control Services */
	pci_ptm_init(dev);		/* Precision Time Measurement */
	pci_aer_init(dev);		/* Advanced Error Reporting */
	pci_dpc_init(dev);		/* Downstream Port Containment */
	pci_rcec_init(dev);		/* Root Complex Event Collector */
	pci_doe_init(dev);		/* Data Object Exchange */
	pci_tph_init(dev);		/* TLP Processing Hints */
	pci_rebar_init(dev);		/* Resizable BAR */
	pci_dev3_init(dev);		/* Device 3 capabilities */
	pci_ide_init(dev);		/* Link Integrity and Data Encryption */

	/* PCI/NVMe: 링크 다운트레이닝 경고 */
	pcie_report_downtraining(dev);
	/* PCI/NVMe: 리셋 방법 초기화 */
	pci_init_reset_methods(dev);
}

/*
 * This is the equivalent of pci_host_bridge_msi_domain() that acts on
 * devices. Firmware interfaces that can select the MSI domain on a
 * per-device basis should be called from here.
 */
/*
 * pci_dev_msi_domain() - 개별 PCI 장치에 대한 MSI domain 탐색
 *
 * NVMe 연결: NVMe 컨트롤러의 MSI-X 벡터가 매핑될 IRQ domain을 찾는다.
 * interrupt remapping이 활성화된 시스템에서는 MSI message가 이 domain에
 * 의해 물리 인터럽트로 변환.
 */
/* PCI/NVMe: 장치별 MSI 도메인 획득 */
static struct irq_domain *pci_dev_msi_domain(struct pci_dev *dev)
{
	/* PCI/NVMe: 후속 코드 동작 수행 */
	struct irq_domain *d;

	/*
	 * If a domain has been set through the pcibios_device_add()
	 * callback, then this is the one (platform code knows best).
	 */
	d = dev_get_msi_domain(&dev->dev); /* PCI/NVMe: platform이 미리 설정한 MSI domain 확인 */
	/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
	if (d)
		return d; /* PCI/NVMe: platform-specific domain 우선 사용 */

	/*
	 * Let's see if we have a firmware interface able to provide
	 * the domain.
	 */
	d = pci_msi_get_device_domain(dev); /* PCI/NVMe: firmware(ACPI/DT)에서 장치별 MSI domain 검색 */
	/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
	if (d)
		return d; /* PCI/NVMe: firmware가 제공한 domain 사용 */

	return NULL; /* PCI/NVMe: 개별 domain 없음; bus/host bridge domain 상속 필요 */
}

/*
 * pci_set_msi_domain() - pci_dev에 최종 MSI domain 설정
 *
 * NVMe 연결: pci_device_add()에서 호출되며, NVMe 컨트롤러의 MSI-X
 * 인터럽트가 속할 IRQ domain을 확정. 개별 domain이 없으면 버스의
 * domain을 상속.
 */
/* PCI/NVMe: pci_set_msi_domain 함수 정의 */
static void pci_set_msi_domain(struct pci_dev *dev)
{
	/* PCI/NVMe: 후속 코드 동작 수행 */
	struct irq_domain *d;

	/*
	 * If the platform or firmware interfaces cannot supply a
	 * device-specific MSI domain, then inherit the default domain
	 * from the host bridge itself.
	 */
	d = pci_dev_msi_domain(dev); /* PCI/NVMe: 장치별 MSI domain 탐색 */
	/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
	if (!d)
		d = dev_get_msi_domain(&dev->bus->dev); /* PCI/NVMe: 없으면 bus의 MSI domain 상속; NVMe MSI-X affinity의 기반 */

	dev_set_msi_domain(&dev->dev, d); /* PCI/NVMe: pci_dev의 MSI domain 확정; nvme_probe에서 pci_enable_msix_range() 사용 */
}

/* PCI/NVMe: pci_dev를 글로벌 트리에 추가. NVMe SSD가 sysfs와 드라이버 모델에 노출됨 */
/* PCI/NVMe: pci_device_add 함수 정의 */
void pci_device_add(struct pci_dev *dev, struct pci_bus *bus)
{
	/* PCI/NVMe: ret 변수 선언/초기화: 반환값 변수. NVMe 초기화 성공/실패 상태 */
	int ret;

	/* PCI/NVMe: 장치 PCIe 설정 일괄 구성 */
	pci_configure_device(dev);

	/* PCI/NVMe: 커널 장치 초기화 */
	device_initialize(&dev->dev);
	/* PCI/NVMe: dev->dev.release 설정: device release 콜백 */
	dev->dev.release = pci_release_dev;

	/* PCI/NVMe: NUMA 노드 설정 */
	set_dev_node(&dev->dev, pcibus_to_node(bus));
	/* PCI/NVMe: dev->dev.dma_mask 플래그: DMA 주소 마스크(NVMe PRP/SGL 범위) */
	dev->dev.dma_mask = &dev->dma_mask;
	/* PCI/NVMe: dev->dev.dma_parms 플래그: DMA 매개변수(NVMe segment/boundary) */
	dev->dev.dma_parms = &dev->dma_parms;
	/* PCI/NVMe: dev->dev.coherent_dma_mask 설정: 일관성 DMA 주소 마스크 */
	dev->dev.coherent_dma_mask = 0xffffffffull;

	/* PCI/NVMe: dma_set_max_seg_size 함수 호출 */
	dma_set_max_seg_size(&dev->dev, 65536);
	/* PCI/NVMe: dma_set_seg_boundary 함수 호출 */
	dma_set_seg_boundary(&dev->dev, 0xffffffff);

	/* PCI/NVMe: 링크 재학습 실패 기록 */
	pcie_failed_link_retrain(dev);

	/* Fix up broken headers */
	/* PCI/NVMe: 장치 quirk/workaround 적용 */
	pci_fixup_device(pci_fixup_header, dev);

	/* PCI/NVMe: 장치 리소스 정렬 재할당 */
	pci_reassigndev_resource_alignment(dev);

	/* PCI/NVMe: PCIe 확장 기능 초기화 */
	pci_init_capabilities(dev);

	/*
	 * Add the device to our list of discovered devices
	 * and the bus list for fixup functions, etc.
	 */
	/* PCI/NVMe: do-while 반복문 시작 */
	down_write(&pci_bus_sem); /* PCI/NVMe: 잠금 획득. NVMe rescan/remove/hotplug 동시 접근 보호 */
	/* PCI/NVMe: 새 도메인 bus 번호 리소스를 전역 리스트에 추가 */
	list_add_tail(&dev->bus_list, &bus->devices);
	/* PCI/NVMe: 쓰기 세마포어 해제 */
	up_write(&pci_bus_sem); /* PCI/NVMe: 잠금 해제. NVMe rescan/remove/hotplug 동시 접근 보호 */

	/* PCI/NVMe: 아키텍처별 장치 추가 */
	ret = pcibios_device_add(dev);
	/* PCI/NVMe: 조건 경고 */
	WARN_ON(ret < 0);

	/* Set up MSI IRQ domain */
	/* PCI/NVMe: MSI 인터럽트 도메인 설정 */
	pci_set_msi_domain(dev);

	/* Notifier could use PCI capabilities */
	/* PCI/NVMe: 커널에 장치 추가 */
	ret = device_add(&dev->dev);
	/* PCI/NVMe: 조건 경고 */
	WARN_ON(ret < 0);

	/* Establish pdev->tsm for newly added (e.g. new SR-IOV VFs) */
	/* PCI/NVMe: Thermal Status Management 초기화 */
	pci_tsm_init(dev);

	/* PCI/NVMe: NPEM 객체 생성 */
	pci_npem_create(dev);

	/* PCI/NVMe: DOE sysfs 초기화 */
	pci_doe_sysfs_init(dev);
}

/*
 * pci_scan_single_device() - 특정 bus/dev/function에서 장치를 스캔하거나
 *                            이미 등록된 장치 반환
 *
 * NVMe 연결: NVMe 컨트롤러가 있는 정확한 bus/dev/function 위치를
 * 처리. 이미 스캔된 장치면 참조를 반환하고, 새 장치면 pci_device_add()를
 * 통해 nvme_pci_driver가 probe할 수 있게 등록.
 */
/* PCI/NVMe: 단일 PCI function 스캔 */
struct pci_dev *pci_scan_single_device(struct pci_bus *bus, int devfn)
{
	/* PCI/NVMe: 후속 코드 동작 수행 */
	struct pci_dev *dev;

	dev = pci_get_slot(bus, devfn); /* PCI/NVMe: 해당 slot에 이미 등록된 pci_dev가 있는지 확인 */
	/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
	if (dev) {
		pci_dev_put(dev); /* PCI/NVMe: 참조 카운트 감소 */
		return dev; /* PCI/NVMe: 이미 스캔된 장치 반환 */
	}

	dev = pci_scan_device(bus, devfn); /* PCI/NVMe: 설정 공간을 읽어 새 pci_dev 생성; NVMe 컨트롤러 발견 */
	/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
	if (!dev)
		return NULL; /* PCI/NVMe: 해당 slot에 장치 없음 */

	pci_device_add(dev, bus); /* PCI/NVMe: pci_dev를 PCI 코어에 등록; nvme_pci_driver.probe -> nvme_probe */

	return dev; /* PCI/NVMe: 새로 등록된 pci_dev 반환 */
}
/* PCI/NVMe: EXPORT_SYMBOL 함수 호출 */
EXPORT_SYMBOL(pci_scan_single_device); /* PCI/NVMe: 심볼 외부 낸출. nvme-pci 등 모듈에서 참조 가능 */

/*
 * next_ari_fn() - ARI(Alternative Routing-ID)를 사용하는 장치의 다음
 *                 function 번호 반환
 *
 * NVMe 연결: ARI를 지원하는 NVMe 컨트롤러는 function 번호를 0~255까지
 * 사용할 수 있어, multifunction/SR-IOV 형태로 더 많은 function을 노출.
 */
/* PCI/NVMe: next_ari_fn 함수 정의 */
static int next_ari_fn(struct pci_bus *bus, struct pci_dev *dev, int fn)
{
	/* PCI/NVMe: pos 변수 선언/초기화: config space 오프셋. NVMe capability/BAR 위치 */
	int pos;
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	u16 cap = 0;
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	unsigned int next_fn;

	if (!dev) /* PCI/NVMe: function 0이 없으면 ARI function도 없음 */
		/* PCI/NVMe: 오류 코드 반환: -ENODEV */
		return -ENODEV;

	pos = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_ARI); /* PCI/NVMe: ARI extended capability 탐색 */
	/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
	if (!pos)
		return -ENODEV; /* PCI/NVMe: ARI 미지원 */

	pci_read_config_word(dev, pos + PCI_ARI_CAP, &cap); /* PCI/NVMe: ARI Capability 레지스터 읽기 */
	next_fn = PCI_ARI_CAP_NFN(cap); /* PCI/NVMe: Next Function Number 필드 추출 */
	/* PCI/NVMe: 조걸 분기, 값 크기 비교 */
	if (next_fn <= fn)
		return -ENODEV;	/* protect against malformed list */ /* PCI/NVMe: 잘못된 ARI capability이면 중단 */

	return next_fn; /* PCI/NVMe: 다음 스캔할 ARI function 번호 반환 */
}

/*
 * next_fn() - 다음에 스캔할 PCI function 번호 반환
 *
 * NVMe 연결: ARI가 활성화된 버스에서는 ARI function 번호를 따르고,
 * 그렇지 않으면 전통적인 8개 function을 multifunction 장치에 대해서만
 * 스캔. 멀티펑션 NVMe 컨트롤러를 모두 발견하는 데 사용.
 */
/* PCI/NVMe: next_fn 함수 정의 */
static int next_fn(struct pci_bus *bus, struct pci_dev *dev, int fn)
{
	if (pci_ari_enabled(bus)) /* PCI/NVMe: 버스에서 ARI가 활성화되어 있으면 */
		return next_ari_fn(bus, dev, fn); /* PCI/NVMe: ARI 기반 다음 function 번호 반환 */

	if (fn >= 7) /* PCI/NVMe: 전통적인 function 번호가 7에 도달하면 종료 */
		/* PCI/NVMe: 오류 코드 반환: -ENODEV */
		return -ENODEV;
	/* only multifunction devices may have more functions */
	if (dev && !dev->multifunction) /* PCI/NVMe: 단일 function 장치이면 더 이상 스캔하지 않음 */
		/* PCI/NVMe: 오류 코드 반환: -ENODEV */
		return -ENODEV;

	return fn + 1; /* PCI/NVMe: 다음 function 번호 반환 */
}

/*
 * only_one_child() - PCIe Downstream Port 아래에서는 Device 0만 스캔할지 결정
 *
 * NVMe 연결: PCIe link에는 보통 Device 0 하나만 연결되므로 function 1~7
 * 스캔을 생략하여 부팅 시간을 단축. 다만 ARI를 사용하는 NVMe
 * 컨트롤러는 function 0이 여러 function을 대표할 수 있다.
 */
/* PCI/NVMe: only_one_child 함수 정의 */
static int only_one_child(struct pci_bus *bus)
{
	struct pci_dev *bridge = bus->self; /* PCI/NVMe: 이 버스의 upstream bridge */

	/*
	 * Systems with unusual topologies set PCI_SCAN_ALL_PCIE_DEVS so
	 * we scan for all possible devices, not just Device 0.
	 */
	if (pci_has_flag(PCI_SCAN_ALL_PCIE_DEVS)) /* PCI/NVMe: 강제 전체 스캔 플래그가 켜져 있으면 */
		return 0; /* PCI/NVMe: Device 0만 스캔하지 않고 모든 device/function 스캔 */

	/*
	 * A PCIe Downstream Port normally leads to a Link with only Device
	 * 0 on it (PCIe spec r3.1, sec 7.3.1).  As an optimization, scan
	 * only for Device 0 in that situation.
	 */
	if (bridge && pci_is_pcie(bridge) && pcie_downstream_port(bridge)) /* PCI/NVMe: PCIe Downstream Port 아래면 */
		return 1; /* PCI/NVMe: Device 0만 스캔; NVMe는 보통 function 0에 위치 */

	return 0; /* PCI/NVMe: 일반 PCI 버스이면 모든 device/function 스캔 */
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
/* PCI/NVMe: 한 슬롯(8개 function) 탐색. NVMe AIC/EDSFF 슬롯의 다중 function 검사 */
/* PCI/NVMe: pci_scan_slot 함수 정의 */
int pci_scan_slot(struct pci_bus *bus, int devfn)
{
	/* PCI/NVMe: 후속 코드 동작 수행 */
	struct pci_dev *dev;
	/* PCI/NVMe: 변수에 값 할당: int fn */
	int fn = 0, nr = 0;

	/* PCI/NVMe: 조걸 분기, 값 크기 비교 */
	if (only_one_child(bus) && (devfn > 0))
		return 0; /* Already scanned the entire slot */

	/* PCI/NVMe: do-while 반복문 시작 */
	do {
		/* PCI/NVMe: 단일 PCI function 스캔 */
		dev = pci_scan_single_device(bus, devfn + fn);
		/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
		if (dev) {
			/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
			if (!pci_dev_is_added(dev))
				/* PCI/NVMe: 카운터 증감 */
				nr++;
			/* PCI/NVMe: 조걸 분기, 값 크기 비교 */
			if (fn > 0)
				/* PCI/NVMe: 구조체 필드에 값 저장: dev->multifunction */
				dev->multifunction = 1;
		/* PCI/NVMe: if 함수 호출 */
		} else if (fn == 0) {
			/*
			 * Function 0 is required unless we are running on
			 * a hypervisor that passes through individual PCI
			 * functions.
			 */
			/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
			if (!hypervisor_isolated_pci_functions())
				break;
		}
		/* PCI/NVMe: next_fn 함수 호출 */
		fn = next_fn(bus, dev, fn);
	/* PCI/NVMe: while 함수 호출 */
	} while (fn >= 0);

	/* Only one slot has PCIe device */
	/* PCI/NVMe: 조걸 분기, 값 크기 비교 */
	if (bus->self && nr)
		/* PCI/NVMe: ASPM 링크 상태 초기화 */
		pcie_aspm_init_link_state(bus->self);

	/* PCI/NVMe: 결과 반환: nr */
	return nr;
}
/* PCI/NVMe: EXPORT_SYMBOL 함수 호출 */
EXPORT_SYMBOL(pci_scan_slot); /* PCI/NVMe: 심볼 외부 낸출. nvme-pci 등 모듈에서 참조 가능 */

/*
 * pcie_find_smpss() - 버스 계층에서 가장 작은 MPSS(MPS Supported Size) 찾기
 *
 * NVMe 연결: MPS는 PCIe TLP payload 상한을 결정. SAFE 모드에서는 계층
 * 내 모든 장치의 MPSS 중 가장 작은 값을 선택해 호환성을 보장. NVMe
 * 성능보다 안정성을 우선.
 */
/* PCI/NVMe: pcie_find_smpss 함수 정의 */
static int pcie_find_smpss(struct pci_dev *dev, void *data)
{
	u8 *smpss = data; /* PCI/NVMe: 현재까지 찾은 최소 MPSS 포인터 */

	if (!pci_is_pcie(dev)) /* PCI/NVMe: PCIe 장치가 아니면 MPS 개념 없음 */
		/* PCI/NVMe: 정상 종료 및 반환 */
		return 0;

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
	if (dev->is_hotplug_bridge && /* PCI/NVMe: hotplug bridge이고 */
	    pci_pcie_type(dev) != PCI_EXP_TYPE_ROOT_PORT) /* PCI/NVMe: Root Port 직접 아래가 아니면 */
		*smpss = 0; /* PCI/NVMe: MPS를 최소 128B로 제한; 향후 hot-add 장치 호환성 */

	if (*smpss > dev->pcie_mpss) /* PCI/NVMe: 현재 최소값보다 장치의 MPSS가 더 작으면 */
		*smpss = dev->pcie_mpss; /* PCI/NVMe: 최소 MPSS 갱신 */

	return 0; /* PCI/NVMe: pci_walk_bus 콜백 계속 진행 */
}

/*
 * pcie_write_mps() - 장치의 Max Payload Size 레지스터에 기록
 *
 * NVMe 연결: PERFORMANCE 모드에서는 장치가 지원하는 최대 MPS를 사용하여
 * NVMe Read/Write TLP 효율을 극대화. SAFE 모드에서는 이미 계산된
 * smpss 값을 사용.
 */
/* PCI/NVMe: pcie_write_mps 함수 정의 */
static void pcie_write_mps(struct pci_dev *dev, int mps)
{
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	int rc;

	if (pcie_bus_config == PCIE_BUS_PERFORMANCE) { /* PCI/NVMe: 성능 우선 모드이면 */
		mps = 128 << dev->pcie_mpss; /* PCI/NVMe: 장치가 지원하는 최대 MPS로 설정 */

		if (pci_pcie_type(dev) != PCI_EXP_TYPE_ROOT_PORT && /* PCI/NVMe: Root Port가 아니고 */
		    dev->bus->self) /* PCI/NVMe: 상위 bridge가 있으면 */

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
			mps = min(mps, pcie_get_mps(dev->bus->self)); /* PCI/NVMe: 장치 최대 MPS와 상위 bridge MPS 중 작은 값 선택 */
	}

	rc = pcie_set_mps(dev, mps); /* PCI/NVMe: PCI_EXP_DEVCTL의 MPS 필드 기록 */
	/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
	if (rc)
		pci_err(dev, "Failed attempting to set the MPS\n"); /* PCI/NVMe: MPS 설정 실패 로그; NVMe TLP 크기 제한 가능 */
}

/*
 * pcie_write_mrrs() - Max Read Request Size 설정
 *
 * NVMe 연결: MRRS는 한 번의 Memory Read 요청이 요청할 수 있는 최대
 * 바이트 수. PERFORMANCE 모드에서 큰 값으로 설정하면 NVMe의 대용량
 * DMA Read(PRP/SGL) 효율이 향상. MPS보다 클 수 없다.
 */
/* PCI/NVMe: pcie_write_mrrs 함수 정의 */
static void pcie_write_mrrs(struct pci_dev *dev)
{
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	int rc, mrrs;

	/*
	 * In the "safe" case, do not configure the MRRS.  There appear to be
	 * issues with setting MRRS to 0 on a number of devices.
	 */
	if (pcie_bus_config != PCIE_BUS_PERFORMANCE) /* PCI/NVMe: PERFORMANCE 모드가 아니면 MRRS 변경 안 함 */
		return;

	/*
	 * For max performance, the MRRS must be set to the largest supported
	 * value.  However, it cannot be configured larger than the MPS the
	 * device or the bus can support.  This should already be properly
	 * configured by a prior call to pcie_write_mps().
	 */
	mrrs = pcie_get_mps(dev); /* PCI/NVMe: MPS를 초과할 수 없으므로 MPS 값을 MRRS 초기값으로 사용 */

	/*
	 * MRRS is a R/W register.  Invalid values can be written, but a
	 * subsequent read will verify if the value is acceptable or not.
	 * If the MRRS value provided is not acceptable (e.g., too large),
	 * shrink the value until it is acceptable to the HW.
	 */
	while (mrrs != pcie_get_readrq(dev) && mrrs >= 128) { /* PCI/NVMe: 설정된 MRRS가 실제 값과 다륾고 128B 이상이면 재시도 */
		rc = pcie_set_readrq(dev, mrrs); /* PCI/NVMe: MRRS 레지스터 기록 */
		/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
		if (!rc)
			break; /* PCI/NVMe: 성공하면 루프 종료 */

		/* PCI/NVMe: pci_warn() 경고 메시지 출력. NVMe 성능/호환성 문제 알림 */
		pci_warn(dev, "Failed attempting to set the MRRS\n"); /* PCI/NVMe: 진단/오류/경고 메시지 출력 */
		mrrs /= 2; /* PCI/NVMe: 실패하면 MRRS를 절반으로 줄여 재시도 */
	}

	/* PCI/NVMe: 조걸 분기, 값 크기 비교 */
	if (mrrs < 128)
		pci_err(dev, "MRRS was unable to be configured with a safe value.  If problems are experienced, try running with pci=pcie_bus_safe\n"); /* PCI/NVMe: MRRS 설정 최종 실패; NVMe Read 효율 저하 */
}

/*
 * pcie_bus_configure_set() - 개별 장치에 MPS/MRRS 적용
 *
 * NVMe 연결: pcie_bus_configure_settings()가 버스를 순회하면서 각
 * 장치(NVMe 포함)의 MPS/MRRS를 설정. TUNE_OFF나 DEFAULT 모드에서는
 * 아무 것도 하지 않는다.
 */
/* PCI/NVMe: pcie_bus_configure_set 함수 정의 */
static int pcie_bus_configure_set(struct pci_dev *dev, void *data)
{
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	int mps, orig_mps;

	if (!pci_is_pcie(dev)) /* PCI/NVMe: PCIe 장치가 아니면 MPS/MRRS 무관 */
		/* PCI/NVMe: 정상 종료 및 반환 */
		return 0;

	/* PCI/NVMe: 조걸 분기, NVMe 장치 상태/플래그에 따른 경로 선택 */
	if (pcie_bus_config == PCIE_BUS_TUNE_OFF ||
	    pcie_bus_config == PCIE_BUS_DEFAULT) /* PCI/NVMe: tuning하지 않는 모드이면 skip */
		/* PCI/NVMe: 정상 종료 및 반환 */
		return 0;

	mps = 128 << *(u8 *)data; /* PCI/NVMe: smpss에 기반한 목표 MPS 산출; 128*2^smpss */
	orig_mps = pcie_get_mps(dev); /* PCI/NVMe: 설정 전 원래 MPS 저장 */

	pcie_write_mps(dev, mps); /* PCI/NVMe: 목표 MPS로 설정; NVMe TLP payload 크기 결정 */
	pcie_write_mrrs(dev); /* PCI/NVMe: MRRS 설정; NVMe Read request 크기 결정 */

	/* PCI/NVMe: pci_info() 진단 메시지 출력. NVMe 초기화/리소스 상태 기록 */
	pci_info(dev, "Max Payload Size set to %4d/%4d (was %4d), Max Read Rq %4d\n", /* PCI/NVMe: 진단/오류/경고 메시지 출력 */
		 /* PCI/NVMe: MPS 레지스터 읽기 */
		 pcie_get_mps(dev), 128 << dev->pcie_mpss,
		 orig_mps, pcie_get_readrq(dev)); /* PCI/NVMe: 설정 결과 로깅 */

	return 0; /* PCI/NVMe: pci_walk_bus 콜백 계속 진행 */
}

/*
 * pcie_bus_configure_settings() requires that pci_walk_bus work in a top-down,
 * parents then children fashion.  If this changes, then this code will not
 * work as designed.
 */
/* PCI/NVMe: PCIe 링크 설정 재구성. NVMe SSD MPS, RCB, ASPM 등 최적화 */
/* PCI/NVMe: pcie_bus_configure_settings 함수 정의 */
void pcie_bus_configure_settings(struct pci_bus *bus)
{
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	u8 smpss = 0;

	/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
	if (!bus->self)
		return;

	/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
	if (!pci_is_pcie(bus->self))
		return;

	/*
	 * FIXME - Peer to peer DMA is possible, though the endpoint would need
	 * to be aware of the MPS of the destination.  To work around this,
	 * simply force the MPS of the entire system to the smallest possible.
	 */
	/* PCI/NVMe: 조걸 분기, 값 일치 여부 검사 */
	if (pcie_bus_config == PCIE_BUS_PEER2PEER)
		/* PCI/NVMe: 변수에 값 할당: smpss */
		smpss = 0;

	/* PCI/NVMe: 조걸 분기, 값 일치 여부 검사 */
	if (pcie_bus_config == PCIE_BUS_SAFE) {
		/* PCI/NVMe: 변수에 값 할당: smpss */
		smpss = bus->self->pcie_mpss;

		/* PCI/NVMe: 최소 MPS 인덱스 탐색 */
		pcie_find_smpss(bus->self, &smpss);
		/* PCI/NVMe: 버스 전체 순회 */
		pci_walk_bus(bus, pcie_find_smpss, &smpss);
	}

	/* PCI/NVMe: 버스 설정 적용 */
	pcie_bus_configure_set(bus->self, &smpss);
	/* PCI/NVMe: 버스 전체 순회 */
	pci_walk_bus(bus, pcie_bus_configure_set, &smpss);
}
/* PCI/NVMe: EXPORT_SYMBOL_GPL 함수 호출 */
EXPORT_SYMBOL_GPL(pcie_bus_configure_settings); /* PCI/NVMe: 심볼 외부 낸출. nvme-pci 등 모듈에서 참조 가능 */

/*
 * Called after each bus is probed, but before its children are examined.  This
 * is marked as __weak because multiple architectures define it.
 */
/*
 * pcibios_fixup_bus() - 아키텍처별 bus fixup(기본 빈 구현)
 *
 * NVMe 연결: 특정 아키텍처에서 필요한 bus 리소스나 속성을 조정.
 * 대부분의 플랫폼에서는 아무 것도 하지 않는다.
 */
/* PCI/NVMe: 아키텍처별 버스 fixup */
void __weak pcibios_fixup_bus(struct pci_bus *bus)
{
       /* nothing to do, expected to be removed in the future */ /* PCI/NVMe: 기본적으로 수행할 작업 없음 */
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
 * pci_scan_child_bus_extend() - 버스 아래의 모든 장치와 하위 버스를 재귀 스캔
 *
 * NVMe 연결: 이 함수는 NVMe SSD가 발견되는 핵심 루프. 모든 slot을
 * 스캔하고, SR-IOV용 bus 번호를 예약하며, bridge 뒤의 하위 버스를
 * 재귀적으로 탐색. available_buses는 hotplug bridge를 위한 추가 bus
 * 번호를 분배하는 데 사용.
 */
/* PCI/NVMe: 하위 버스 재귀 스캔(확장 버스 분배) */
static unsigned int pci_scan_child_bus_extend(struct pci_bus *bus,
					      /* PCI/NVMe: 후속 코드 동작 수행 */
					      unsigned int available_buses)
{
	/* PCI/NVMe: 변수에 값 할당: unsigned int used_buses, normal_bridges */
	unsigned int used_buses, normal_bridges = 0, hotplug_bridges = 0;
	/* PCI/NVMe: 변수에 값 할당: unsigned int start */
	unsigned int start = bus->busn_res.start;
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	unsigned int devnr, cmax, max = start;
	/* PCI/NVMe: 후속 코드 동작 수행 */
	struct pci_dev *dev;

	dev_dbg(&bus->dev, "scanning bus\n"); /* PCI/NVMe: 버스 스캔 시작 디버그 로그 */

	/* Go find them, Rover! */
	for (devnr = 0; devnr < PCI_MAX_NR_DEVS; devnr++) /* PCI/NVMe: 0~31 slot 순회 */
		pci_scan_slot(bus, PCI_DEVFN(devnr, 0)); /* PCI/NVMe: 각 slot의 function 0부터 스캔; NVMe 컨트롤러 탐색 */

	/* Reserve buses for SR-IOV capability */
	used_buses = pci_iov_bus_range(bus); /* PCI/NVMe: 이 버스의 SR-IOV VF들을 위해 필요한 bus 번호 수 계산 */
	max += used_buses; /* PCI/NVMe: 사용된 bus 번호만큼 max 증가 */

	/*
	 * After performing arch-dependent fixup of the bus, look behind
	 * all PCI-to-PCI bridges on this bus.
	 */
	if (!bus->is_added) { /* PCI/NVMe: 처음 추가된 버스이면 */
		/* PCI/NVMe: 장치 디버그 메시지 출력 */
		dev_dbg(&bus->dev, "fixups for bus\n");
		pcibios_fixup_bus(bus); /* PCI/NVMe: 아키텍처별 bus fixup 수행 */
		bus->is_added = 1; /* PCI/NVMe: fixup 완료 표시 */
	}

	/*
	 * Calculate how many hotplug bridges and normal bridges there
	 * are on this bus. We will distribute the additional available
	 * buses between hotplug bridges.
	 */
	for_each_pci_bridge(dev, bus) { /* PCI/NVMe: 이 버스에 있는 모든 bridge 순회 */
		/* PCI/NVMe: 조걸 분기, 값 크기 비교 */
		if (dev->is_hotplug_bridge)
			hotplug_bridges++; /* PCI/NVMe: hotplug bridge 카운트 */
		/* PCI/NVMe: 조걸 분기의 else 경로 */
		else
			normal_bridges++; /* PCI/NVMe: 일반 bridge 카운트 */
	}

	/*
	 * Scan bridges that are already configured. We don't touch them
	 * unless they are misconfigured (which will be done in the second
	 * scan below).
	 */
	for_each_pci_bridge(dev, bus) { /* PCI/NVMe: 이미 firmware가 설정한 bridge들을 첫 pass로 스캔 */
		/* PCI/NVMe: 변수에 값 할당: cmax */
		cmax = max;
		max = pci_scan_bridge_extend(bus, dev, max, 0, 0); /* PCI/NVMe: pass 0로 bridge 뒤 스캔; NVMe가 연결될 수 있음 */

		/*
		 * Reserve one bus for each bridge now to avoid extending
		 * hotplug bridges too much during the second scan below.
		 */
		used_buses++; /* PCI/NVMe: 각 bridge에 최소 1개 bus 예약 */
		/* PCI/NVMe: 조걸 분기, 값 크기 비교 */
		if (max - cmax > 1)
			used_buses += max - cmax - 1; /* PCI/NVMe: 추가로 사용된 bus 수만큼 증가 */
	}

	/* Scan bridges that need to be reconfigured */
	for_each_pci_bridge(dev, bus) { /* PCI/NVMe: 재설정이 필요한 bridge들을 두 번째 pass로 스캔 */
		/* PCI/NVMe: 지역 변수 선언 및 초기화 */
		unsigned int buses = 0;

		if (!hotplug_bridges && normal_bridges == 1) { /* PCI/NVMe: hotplug bridge가 없고 일반 bridge가 1개뿐이면 */
			/*
			 * There is only one bridge on the bus (upstream
			 * port) so it gets all available buses which it
			 * can then distribute to the possible hotplug
			 * bridges below.
			 */
			buses = available_buses; /* PCI/NVMe: 모든 추가 available bus를 이 bridge에 할당 */
		} else if (dev->is_hotplug_bridge) { /* PCI/NVMe: hotplug bridge이면 */
			/*
			 * Distribute the extra buses between hotplug
			 * bridges if any.
			 */
			buses = available_buses / hotplug_bridges; /* PCI/NVMe: hotplug bridge들 간 추가 bus 분배 */
			buses = min(buses, available_buses - used_buses + 1); /* PCI/NVMe: 남은 bus를 초과하지 않도록 제한 */
		}

		/* PCI/NVMe: 변수에 값 할당: cmax */
		cmax = max;
		max = pci_scan_bridge_extend(bus, dev, cmax, buses, 1); /* PCI/NVMe: pass 1로 재설정 및 하위 스캔 */
		/* One bus is already accounted so don't add it again */
		/* PCI/NVMe: 조걸 분기, 값 크기 비교 */
		if (max - cmax > 1)
			used_buses += max - cmax - 1; /* PCI/NVMe: 추가 사용 bus 수 갱신 */
	}

	/*
	 * Make sure a hotplug bridge has at least the minimum requested
	 * number of buses but allow it to grow up to the maximum available
	 * bus number if there is room.
	 */
	if (bus->self && bus->self->is_hotplug_bridge) { /* PCI/NVMe: 현재 버스가 hotplug bridge 아래이면 */
		used_buses = max(available_buses, pci_hotplug_bus_size - 1); /* PCI/NVMe: 최소 요구 bus 수 확보 */
		if (max - start < used_buses) { /* PCI/NVMe: 현재 max가 최소 요구보다 작으면 확장 */
			/* PCI/NVMe: 변수에 값 할당: max */
			max = start + used_buses;

			/* Do not allocate more buses than we have room left */
			/* PCI/NVMe: 조걸 분기, 값 크기 비교 */
			if (max > bus->busn_res.end)
				max = bus->busn_res.end; /* PCI/NVMe: 상위가 허용한 bus 번호를 초과하지 않도록 제한 */

			/* PCI/NVMe: 장치 디버그 메시지 출력 */
			dev_dbg(&bus->dev, "%pR extended by %#02x\n",
				&bus->busn_res, max - start); /* PCI/NVMe: bus 번호 범위 확장 디버그 로그 */
		}
	}

	/*
	 * We've scanned the bus and so we know all about what's on
	 * the other side of any bridges that may be on this bus plus
	 * any devices.
	 *
	 * Return how far we've got finding sub-buses.
	 */
	dev_dbg(&bus->dev, "bus scan returning with max=%02x\n", max); /* PCI/NVMe: 스캔 완료 및 최대 subordinate bus 출력 */
	return max; /* PCI/NVMe: 현재 버스 계층에서 사용한 최대 bus 번호 반환 */
}

/**
 * pci_scan_child_bus() - Scan devices below a bus
 * @bus: Bus to scan for devices
 *
 * Scans devices below @bus including subordinate buses. Returns new
 * subordinate number including all the found devices.
 */
/*
 * pci_scan_child_bus() - 추가 available bus 없이 하위 버스 스캔
 *
 * NVMe 연결: 일반적인 PCI 스캔 경로. NVMe SSD가 발견될 때까지
 * pci_scan_child_bus_extend()를 호출.
 */
/* PCI/NVMe: 하위 버스 재귀 스캔 */
unsigned int pci_scan_child_bus(struct pci_bus *bus)
{
	return pci_scan_child_bus_extend(bus, 0); /* PCI/NVMe: 추가 bus 예약 없이 하위 스캔 */
}
/* PCI/NVMe: EXPORT_SYMBOL_GPL 함수 호출 */
EXPORT_SYMBOL_GPL(pci_scan_child_bus); /* PCI/NVMe: 심볼 외부 낸출. nvme-pci 등 모듈에서 참조 가능 */

/**
 * pcibios_root_bridge_prepare - Platform-specific host bridge setup
 * @bridge: Host bridge to set up
 *
 * Default empty implementation.  Replace with an architecture-specific setup
 * routine, if necessary.
 */
/* PCI/NVMe: 아키텍처별 호스트 브리지 준비 */
int __weak pcibios_root_bridge_prepare(struct pci_host_bridge *bridge)
{
	/* PCI/NVMe: 정상 종료 및 반환 */
	return 0;
}

/* PCI/NVMe: 아키텍처별 버스 추가 */
void __weak pcibios_add_bus(struct pci_bus *bus)
{
}

/* PCI/NVMe: 아키텍처별 버스 제거 */
void __weak pcibios_remove_bus(struct pci_bus *bus)
{
}

/*
 * pci_create_root_bus() - host bridge를 할당하고 root bus 등록
 *
 * NVMe 연결: PCI host controller driver가 Root Complex 아래의 root bus를
 * 생성할 때 사용. resources에는 MEM/IO/BUS window가 포함되며, 이는
 * NVMe BAR 할당의 전체 공간이 된다.
 */
/* PCI/NVMe: 루트 PCI 버스 생성 */
struct pci_bus *pci_create_root_bus(struct device *parent, int bus,
		/* PCI/NVMe: 후속 코드 동작 수행 */
		struct pci_ops *ops, void *sysdata, struct list_head *resources)
{
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	int error;
	/* PCI/NVMe: 후속 코드 동작 수행 */
	struct pci_host_bridge *bridge;

	bridge = pci_alloc_host_bridge(0); /* PCI/NVMe: host bridge 할당 */
	if (!bridge) /* PCI/NVMe: 할당 실패 시 NULL 반환 */
		/* PCI/NVMe: NULL 반환(메모리/리소스 부족 또는 초기화 실패) */
		return NULL;

	bridge->dev.parent = parent; /* PCI/NVMe: host bridge의 부모 device 설정 */

	list_splice_init(resources, &bridge->windows); /* PCI/NVMe: caller가 제공한 resource window를 bridge로 이동 */
	bridge->sysdata = sysdata; /* PCI/NVMe: platform-specific data 연결 */
	bridge->busnr = bus; /* PCI/NVMe: root bus 번호 설정 */
	bridge->ops = ops; /* PCI/NVMe: PCI config space ops 연결 */

	error = pci_register_host_bridge(bridge); /* PCI/NVMe: host bridge 등록 및 root bus 생성; NVMe 트리의 시작점 */
	/* PCI/NVMe: 조걸 분기, 값 크기 비교 */
	if (error < 0)
		/* PCI/NVMe: 오류 처리/종료 지점으로 이동: err_out */
		goto err_out;

	return bridge->bus; /* PCI/NVMe: 생성된 root bus 반환 */

err_out:
	put_device(&bridge->dev); /* PCI/NVMe: 등록 실패 시 host bridge 참조 감소 */
	/* PCI/NVMe: NULL 반환(메모리/리소스 부족 또는 초기화 실패) */
	return NULL;
}
/* PCI/NVMe: EXPORT_SYMBOL_GPL 함수 호출 */
EXPORT_SYMBOL_GPL(pci_create_root_bus); /* PCI/NVMe: 심볼 외부 낸출. nvme-pci 등 모듈에서 참조 가능 */

/*
 * pci_host_probe() - PCI host bridge 아래 전체 PCIe 계층 탐색 및 초기화
 *
 * NVMe 연결: host controller driver가 이 함수를 호출하여 Root Complex
 * 아래의 모든 PCIe 장치를 스캔하고, 리소스를 할당하며, 매칭되는
 * 드라이버를 probe. NVMe SSD는 pci_bus_add_devices()에서
 * nvme_pci_driver의 nvme_probe()가 호출되어 초기화.
 */
/* PCI/NVMe: host bridge 기반 루트 버스 탐색. NVMe PCIe 컨트롤러 열수 시작 */
/* PCI/NVMe: pci_host_probe 함수 정의 */
int pci_host_probe(struct pci_host_bridge *bridge)
{
	/* PCI/NVMe: 후속 코드 동작 수행 */
	struct pci_bus *bus, *child;
	/* PCI/NVMe: ret 변수 선언/초기화: 반환값 변수. NVMe 초기화 성공/실패 상태 */
	int ret;

	pci_lock_rescan_remove(); /* PCI/NVMe: rescan/remove 상호 배제 잠금 */
	ret = pci_scan_root_bus_bridge(bridge); /* PCI/NVMe: root bus 및 전체 계층 스캔; NVMe 컨트롤러 pci_dev 생성 */
	pci_unlock_rescan_remove(); /* PCI/NVMe: 잠금 해제 */
	/* PCI/NVMe: 조걸 분기, 값 크기 비교 */
	if (ret < 0) {
		dev_err(bridge->dev.parent, "Scanning root bridge failed"); /* PCI/NVMe: root bridge 스캔 실패 로그 */
		/* PCI/NVMe: 결과 반환: ret */
		return ret;
	}

	bus = bridge->bus; /* PCI/NVMe: 스캔이 완료된 root bus 획득 */

	/* If we must preserve the resource configuration, claim now */
	/* PCI/NVMe: 조걸 분기, 값 크기 비교 */
	if (bridge->preserve_config)
		pci_bus_claim_resources(bus); /* PCI/NVMe: firmware 설정을 유지하며 리소스 claim */

	/*
	 * Assign whatever was left unassigned. If we didn't claim above,
	 * this will reassign everything.
	 */
	pci_assign_unassigned_root_bus_resources(bus); /* PCI/NVMe: 할당되지 않은 BAR/버스 번호 할당; NVMe BAR 물리 주소 확정 */

	/* PCI/NVMe: 도메인별 bus 번호 리스트 순회. 다중 루트 컴플렉스/VMD 그룹핑 참고 */
	list_for_each_entry(child, &bus->children, node)
		pcie_bus_configure_settings(child); /* PCI/NVMe: 각 서브 버스의 MPS/MRRS 설정; NVMe 링크 효율 결정 */

	pci_lock_rescan_remove(); /* PCI/NVMe: device 추가 동안 잠금 */
	pci_bus_add_devices(bus); /* PCI/NVMe: 매칭되는 PCI 드라이버 probe 호출; nvme_probe 실행 */
	pci_unlock_rescan_remove(); /* PCI/NVMe: 잠금 해제 */

	/*
	 * Ensure pm_runtime_enable() is called for the controller drivers
	 * before calling pci_host_probe(). The PM framework expects that
	 * if the parent device supports runtime PM, it will be enabled
	 * before child runtime PM is enabled.
	 */
	pm_runtime_set_active(&bridge->dev); /* PCI/NVMe: host bridge runtime PM 활성 상태 설정 */
	pm_runtime_no_callbacks(&bridge->dev); /* PCI/NVMe: bridge 자체에는 runtime PM 콜백 없음 */
	devm_pm_runtime_enable(&bridge->dev); /* PCI/NVMe: device-managed runtime PM 활성화; NVMe RTD3의 상위 전원 관리 */

	/* PCI/NVMe: 정상 종료 및 반환 */
	return 0;
}
/* PCI/NVMe: EXPORT_SYMBOL_GPL 함수 호출 */
EXPORT_SYMBOL_GPL(pci_host_probe); /* PCI/NVMe: 심볼 외부 낸출. nvme-pci 등 모듈에서 참조 가능 */

/* PCI/NVMe: 버스 번호 리소스 삽입. NVMe 장치 추가를 위한 bus 번호 범위 예약 */
/* PCI/NVMe: pci_bus_insert_busn_res 함수 정의 */
int pci_bus_insert_busn_res(struct pci_bus *b, int bus, int bus_max)
{
	/* PCI/NVMe: 비트 연산으로 값 설정/마스크: struct resource *res */
	struct resource *res = &b->busn_res;
	/* PCI/NVMe: 후속 코드 동작 수행 */
	struct resource *parent_res, *conflict;

	/* PCI/NVMe: 구조체 필드에 값 저장: res->start */
	res->start = bus;
	/* PCI/NVMe: 구조체 필드에 값 저장: res->end */
	res->end = bus_max;
	/* PCI/NVMe: 구조체 필드에 값 저장: res->flags */
	res->flags = IORESOURCE_BUS;

	/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
	if (!pci_is_root_bus(b))
		/* PCI/NVMe: 비트 연산으로 값 설정/마스크: parent_res */
		parent_res = &b->parent->busn_res;
	/* PCI/NVMe: 조걸 분기의 else 경로 */
	else {
		/* PCI/NVMe: get_pci_domain_busn_res 함수 호출 */
		parent_res = get_pci_domain_busn_res(pci_domain_nr(b));
		/* PCI/NVMe: 구조체 필드에 값 저장: res->flags | */
		res->flags |= IORESOURCE_PCI_FIXED;
	}

	/* PCI/NVMe: 리소스 충돌 검사 */
	conflict = request_resource_conflict(parent_res, res);

	/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
	if (conflict)
		/* PCI/NVMe: 장치 정보 메시지 출력 */
		dev_info(&b->dev,
			   /* PCI/NVMe: pR 함수 호출 */
			   "busn_res: can not insert %pR under %s%pR (conflicts with %s %pR)\n",
			    /* PCI/NVMe: 루트 버스 여부 확인 */
			    res, pci_is_root_bus(b) ? "domain " : "",
			    /* PCI/NVMe: 후속 코드 동작 수행 */
			    parent_res, conflict->name, conflict);

	/* PCI/NVMe: 결과 반환: conflict == NULL */
	return conflict == NULL;
}

/* PCI/NVMe: bus 번호 리소스 끝 갱신. NVMe 핫플러그/리스캔 시 범위 조정 */
/* PCI/NVMe: pci_bus_update_busn_res_end 함수 정의 */
int pci_bus_update_busn_res_end(struct pci_bus *b, int bus_max)
{
	/* PCI/NVMe: 비트 연산으로 값 설정/마스크: struct resource *res */
	struct resource *res = &b->busn_res;
	/* PCI/NVMe: 변수에 값 할당: struct resource old_res */
	struct resource old_res = *res;
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	resource_size_t size;
	/* PCI/NVMe: ret 변수 선언/초기화: 반환값 변수. NVMe 초기화 성공/실패 상태 */
	int ret;

	/* PCI/NVMe: 조걸 분기, 값 크기 비교 */
	if (res->start > bus_max)
		/* PCI/NVMe: 오류 코드 반환: -EINVAL */
		return -EINVAL;

	/* PCI/NVMe: 변수에 값 할당: size */
	size = bus_max - res->start + 1;
	/* PCI/NVMe: 리소스 범위 조정 */
	ret = adjust_resource(res, res->start, size);
	/* PCI/NVMe: 장치 정보 메시지 출력 */
	dev_info(&b->dev, "busn_res: %pR end %s updated to %02x\n",
			/* PCI/NVMe: 후속 코드 동작 수행 */
			&old_res, ret ? "can not be" : "is", bus_max);

	/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
	if (!ret && !res->parent)
		/* PCI/NVMe: bus 번호 리소스 삽입 */
		pci_bus_insert_busn_res(b, res->start, res->end);

	/* PCI/NVMe: 결과 반환: ret */
	return ret;
}

/* PCI/NVMe: bus 번호 리소스 해제. NVMe 장치 제거 후 번호 반환 */
/* PCI/NVMe: pci_bus_release_busn_res 함수 정의 */
void pci_bus_release_busn_res(struct pci_bus *b)
{
	/* PCI/NVMe: 비트 연산으로 값 설정/마스크: struct resource *res */
	struct resource *res = &b->busn_res;
	/* PCI/NVMe: ret 변수 선언/초기화: 반환값 변수. NVMe 초기화 성공/실패 상태 */
	int ret;

	/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
	if (!res->flags || !res->parent)
		return;

	/* PCI/NVMe: 리소스 해제 */
	ret = release_resource(res);
	/* PCI/NVMe: 장치 정보 메시지 출력 */
	dev_info(&b->dev, "busn_res: %pR %s released\n",
			/* PCI/NVMe: 후속 코드 동작 수행 */
			res, ret ? "can not be" : "is");
}

/* PCI/NVMe: host bridge에서 루트 버스 스캔. NVMe PCIe 엔드포인트 발견 루트 진입점 */
/* PCI/NVMe: pci_scan_root_bus_bridge 함수 정의 */
int pci_scan_root_bus_bridge(struct pci_host_bridge *bridge)
{
	/* PCI/NVMe: 후속 코드 동작 수행 */
	struct resource_entry *window;
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	bool found = false;
	/* PCI/NVMe: 후속 코드 동작 수행 */
	struct pci_bus *b;
	/* PCI/NVMe: max 변수 선언/초기화: 최대 bus 번호/값. NVMe 하위 버스 범위 */
	int max, bus, ret;

	/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
	if (!bridge)
		/* PCI/NVMe: 오류 코드 반환: -EINVAL */
		return -EINVAL;

	/* PCI/NVMe: resource_list_for_each_entry 함수 호출 */
	resource_list_for_each_entry(window, &bridge->windows)
		/* PCI/NVMe: 조걸 분기, 플래그/비트 마스크 검사 */
		if (window->res->flags & IORESOURCE_BUS) {
			/* PCI/NVMe: 구조체 필드에 값 저장: bridge->busnr */
			bridge->busnr = window->res->start;
			/* PCI/NVMe: 변수에 값 할당: found */
			found = true;
			break;
		}

	/* PCI/NVMe: 호스트 브리지 등록 */
	ret = pci_register_host_bridge(bridge);
	/* PCI/NVMe: 조걸 분기, 값 크기 비교 */
	if (ret < 0)
		/* PCI/NVMe: 결과 반환: ret */
		return ret;

	/* PCI/NVMe: 변수에 값 할당: b */
	b = bridge->bus;
	/* PCI/NVMe: 변수에 값 할당: bus */
	bus = bridge->busnr;

	/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
	if (!found) {
		/* PCI/NVMe: 장치 정보 메시지 출력 */
		dev_info(&b->dev,
		 /* PCI/NVMe: 후속 코드 동작 수행 */
		 "No busn resource found for root bus, will use [bus %02x-ff]\n",
			/* PCI/NVMe: 후속 코드 동작 수행 */
			bus);
		/* PCI/NVMe: bus 번호 리소스 삽입 */
		pci_bus_insert_busn_res(b, bus, 255);
	}

	/* PCI/NVMe: 하위 버스 재귀 스캔 */
	max = pci_scan_child_bus(b);

	/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
	if (!found)
		/* PCI/NVMe: bus 번호 리소스 끝 갱신 */
		pci_bus_update_busn_res_end(b, max);

	/* PCI/NVMe: 정상 종료 및 반환 */
	return 0;
}
/* PCI/NVMe: EXPORT_SYMBOL 함수 호출 */
EXPORT_SYMBOL(pci_scan_root_bus_bridge); /* PCI/NVMe: 심볼 외부 낸출. nvme-pci 등 모듈에서 참조 가능 */

/* PCI/NVMe: 루트 버스 스캔 */
struct pci_bus *pci_scan_root_bus(struct device *parent, int bus,
		/* PCI/NVMe: 후속 코드 동작 수행 */
		struct pci_ops *ops, void *sysdata, struct list_head *resources)
{
	/* PCI/NVMe: 후속 코드 동작 수행 */
	struct resource_entry *window;
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	bool found = false;
	/* PCI/NVMe: 후속 코드 동작 수행 */
	struct pci_bus *b;
	/* PCI/NVMe: max 변수 선언/초기화: 최대 bus 번호/값. NVMe 하위 버스 범위 */
	int max;

	/* PCI/NVMe: resource_list_for_each_entry 함수 호출 */
	resource_list_for_each_entry(window, resources)
		/* PCI/NVMe: 조걸 분기, 플래그/비트 마스크 검사 */
		if (window->res->flags & IORESOURCE_BUS) {
			/* PCI/NVMe: 변수에 값 할당: found */
			found = true;
			break;
		}

	/* PCI/NVMe: 루트 PCI 버스 생성 */
	b = pci_create_root_bus(parent, bus, ops, sysdata, resources);
	/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
	if (!b)
		/* PCI/NVMe: NULL 반환(메모리/리소스 부족 또는 초기화 실패) */
		return NULL;

	/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
	if (!found) {
		/* PCI/NVMe: 장치 정보 메시지 출력 */
		dev_info(&b->dev,
		 /* PCI/NVMe: 후속 코드 동작 수행 */
		 "No busn resource found for root bus, will use [bus %02x-ff]\n",
			/* PCI/NVMe: 후속 코드 동작 수행 */
			bus);
		/* PCI/NVMe: bus 번호 리소스 삽입 */
		pci_bus_insert_busn_res(b, bus, 255);
	}

	/* PCI/NVMe: 하위 버스 재귀 스캔 */
	max = pci_scan_child_bus(b);

	/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
	if (!found)
		/* PCI/NVMe: bus 번호 리소스 끝 갱신 */
		pci_bus_update_busn_res_end(b, max);

	/* PCI/NVMe: 결과 반환: b */
	return b;
}
/* PCI/NVMe: EXPORT_SYMBOL 함수 호출 */
EXPORT_SYMBOL(pci_scan_root_bus); /* PCI/NVMe: 심볼 외부 낸출. nvme-pci 등 모듈에서 참조 가능 */

/*
 * pci_scan_bus() - 단순 root bus 스캔(레거시 API)
 *
 * NVMe 연결: MEM/IO/BUS 전체 리소스를 사용하여 root bus를 만들고
 * 스캔. 일부 아키텍처나 초기화 코드에서 여전히 사용.
 */
/* PCI/NVMe: pci_scan_bus 함수 호출 */
struct pci_bus *pci_scan_bus(int bus, struct pci_ops *ops,
					/* PCI/NVMe: 후속 코드 동작 수행 */
					void *sysdata)
{
	/* PCI/NVMe: LIST_HEAD 함수 호출 */
	LIST_HEAD(resources);
	/* PCI/NVMe: 후속 코드 동작 수행 */
	struct pci_bus *b;

	pci_add_resource(&resources, &ioport_resource); /* PCI/NVMe: 전체 I/O port 리소스 추가 */
	pci_add_resource(&resources, &iomem_resource); /* PCI/NVMe: 전체 memory 리소스 추가 */
	pci_add_resource(&resources, &busn_resource); /* PCI/NVMe: bus 번호 리소스 추가 */
	b = pci_create_root_bus(NULL, bus, ops, sysdata, &resources); /* PCI/NVMe: root bus 생성; NVMe BAR 할당 공간 설정 */
	/* PCI/NVMe: 조걸 분기, 조건에 따른 실행 경로 선택 */
	if (b) {
		pci_scan_child_bus(b); /* PCI/NVMe: 하위 계층 스캔; NVMe SSD 탐색 */
	/* PCI/NVMe: 후속 코드 동작 수행 */
	} else {
		pci_free_resource_list(&resources); /* PCI/NVMe: root bus 생성 실패 시 리소스 해제 */
	}
	return b; /* PCI/NVMe: 생성된 root bus 또는 NULL 반환 */
}
/* PCI/NVMe: EXPORT_SYMBOL 함수 호출 */
EXPORT_SYMBOL(pci_scan_bus); /* PCI/NVMe: 심볼 외부 낸출. nvme-pci 등 모듈에서 참조 가능 */

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
 * pci_rescan_bus_bridge_resize() - bridge 아래 버스를 재스캔하고 리소스 재할당
 *
 * NVMe 연결: hotplug나 SR-IOV VF 추가 등으로 bridge 아래에 새 NVMe
 * 장치가 추가되었을 때 호출. 필요하면 bridge window 크기를 조정.
 */
/* PCI/NVMe: pci_rescan_bus_bridge_resize 함수 호출 */
unsigned int pci_rescan_bus_bridge_resize(struct pci_dev *bridge)
{
	/* PCI/NVMe: max 변수 선언/초기화: 최대 bus 번호/값. NVMe 하위 버스 범위 */
	unsigned int max;
	struct pci_bus *bus = bridge->subordinate; /* PCI/NVMe: 재스캔할 하위 버스 */

	max = pci_scan_child_bus(bus); /* PCI/NVMe: 하위 버스 재스캔; 새 NVMe 장치 탐색 */

	pci_assign_unassigned_bridge_resources(bridge); /* PCI/NVMe: 새 장치의 BAR를 위해 bridge 리소스 재할당/확장 */

	pci_bus_add_devices(bus); /* PCI/NVMe: 새로 발견된 장치에 드라이버 probe; 새 NVMe 컨트롤러 초기화 */

	return max; /* PCI/NVMe: 새로 발견된 최대 subordinate bus 번호 반환 */
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
 * pci_rescan_bus() - bus를 재스캔하고 리소스 할당 및 드라이버 바인딩
 *
 * NVMe 연결: runtime rescan 요청(예: sysfs echo 1 > rescan) 시 호출.
 * 새로 연결된 NVMe SSD를 발견하고 초기화.
 */
/* PCI/NVMe: pci_rescan_bus 함수 호출 */
unsigned int pci_rescan_bus(struct pci_bus *bus)
{
	/* PCI/NVMe: max 변수 선언/초기화: 최대 bus 번호/값. NVMe 하위 버스 범위 */
	unsigned int max;

	max = pci_scan_child_bus(bus); /* PCI/NVMe: 하위 계층 재스캔; NVMe SSD 탐색 */
	pci_assign_unassigned_bus_resources(bus); /* PCI/NVMe: 할당되지 않은 리소스(BAR) 할당 */
	pci_bus_add_devices(bus); /* PCI/NVMe: 새 장치에 드라이버 probe; nvme_probe 호출 */

	return max; /* PCI/NVMe: 새로 발견된 최대 subordinate bus 번호 반환 */
}
/* PCI/NVMe: EXPORT_SYMBOL_GPL 함수 호출 */
EXPORT_SYMBOL_GPL(pci_rescan_bus); /* PCI/NVMe: 심볼 외부 낸출. nvme-pci 등 모듈에서 참조 가능 */

/*
 * pci_rescan_bus(), pci_rescan_bus_bridge_resize() and PCI device removal
 * routines should always be executed under this mutex.
 */
/* PCI/NVMe: 뮤텍스 정의 */
DEFINE_MUTEX(pci_rescan_remove_lock);

/* PCI/NVMe: rescan/remove 상호배제 잠금. NVMe 핫플러그와 리스캔 동시 실행 방지 */
/* PCI/NVMe: pci_lock_rescan_remove 함수 정의 */
void pci_lock_rescan_remove(void)
{
	/* PCI/NVMe: 뮤텍스 획득 */
	mutex_lock(&pci_rescan_remove_lock); /* PCI/NVMe: 잠금 획득. NVMe rescan/remove/hotplug 동시 접근 보호 */
}
/* PCI/NVMe: EXPORT_SYMBOL_GPL 함수 호출 */
EXPORT_SYMBOL_GPL(pci_lock_rescan_remove); /* PCI/NVMe: 심볼 외부 낸출. nvme-pci 등 모듈에서 참조 가능 */

/* PCI/NVMe: rescan/remove 잠금 해제. NVMe 핫플러그 처리 재개 */
/* PCI/NVMe: pci_unlock_rescan_remove 함수 정의 */
void pci_unlock_rescan_remove(void)
{
	/* PCI/NVMe: 뮤텍스 해제 */
	mutex_unlock(&pci_rescan_remove_lock); /* PCI/NVMe: 잠금 해제. NVMe rescan/remove/hotplug 동시 접근 보호 */
}
/* PCI/NVMe: EXPORT_SYMBOL_GPL 함수 호출 */
EXPORT_SYMBOL_GPL(pci_unlock_rescan_remove); /* PCI/NVMe: 심볼 외부 낸출. nvme-pci 등 모듈에서 참조 가능 */

/* PCI/NVMe: pci_sort_bf_cmp 함수 호출 */
static int __init pci_sort_bf_cmp(const struct device *d_a,
				  /* PCI/NVMe: 후속 코드 동작 수행 */
				  const struct device *d_b)
{
	/* PCI/NVMe: device를 pci_dev로 변환 */
	const struct pci_dev *a = to_pci_dev(d_a);
	/* PCI/NVMe: device를 pci_dev로 변환 */
	const struct pci_dev *b = to_pci_dev(d_b);

	/* PCI/NVMe: 조걸 분기, 값 크기 비교 */
	if      (pci_domain_nr(a->bus) < pci_domain_nr(b->bus)) return -1;
	/* PCI/NVMe: 추가 조걸 분기 */
	else if (pci_domain_nr(a->bus) > pci_domain_nr(b->bus)) return  1;

	/* PCI/NVMe: 조걸 분기, 값 크기 비교 */
	if      (a->bus->number < b->bus->number) return -1;
	/* PCI/NVMe: 추가 조걸 분기 */
	else if (a->bus->number > b->bus->number) return  1;

	/* PCI/NVMe: 조걸 분기, 값 크기 비교 */
	if      (a->devfn < b->devfn) return -1;
	/* PCI/NVMe: 추가 조걸 분기 */
	else if (a->devfn > b->devfn) return  1;

	/* PCI/NVMe: 정상 종료 및 반환 */
	return 0;
}

/* PCI/NVMe: pci_sort_breadthfirst 함수 호출 */
void __init pci_sort_breadthfirst(void)
{
	/* PCI/NVMe: bus_sort_breadthfirst 함수 호출 */
	bus_sort_breadthfirst(&pci_bus_type, &pci_sort_bf_cmp);
}

/*
 * pci_hp_add_bridge() - hotplug로 추가된 bridge를 PCI 트리에 통합
 *
 * NVMe 연결: hotplug slot에 새 Root Port/Switch가 추가되었을 때 호출.
 * 이 bridge 아래에 NVMe SSD가 연결될 수 있으므로 bus 번호를 할당하고
 * 하위를 스캔.
 */
/* PCI/NVMe: 핫플러그로 추가된 브리지 처리. NVMe 핫스왑 SSD 연결 시 버스 확장 */
/* PCI/NVMe: pci_hp_add_bridge 함수 정의 */
int pci_hp_add_bridge(struct pci_dev *dev)
{
	struct pci_bus *parent = dev->bus; /* PCI/NVMe: hotplug bridge의 부모 버스 */
	int busnr, start = parent->busn_res.start; /* PCI/NVMe: 부모 버스 번호 범위 시작 */
	/* PCI/NVMe: 지역 변수 선언 및 초기화 */
	unsigned int available_buses = 0;
	int end = parent->busn_res.end; /* PCI/NVMe: 부모 버스 번호 범위 끝 */

	for (busnr = start; busnr <= end; busnr++) { /* PCI/NVMe: 부모 범위 내에서 사용 가능한 bus 번호 탐색 */
		/* PCI/NVMe: 조걸 분기, NULL/0/미설정 여부 검사 */
		if (!pci_find_bus(pci_domain_nr(parent), busnr))
			break; /* PCI/NVMe: 비어있는 bus 번호 찾기 */
	}
	if (busnr-- > end) { /* PCI/NVMe: 사용 가능한 bus 번호가 없으면 */
		pci_err(dev, "No bus number available for hot-added bridge\n"); /* PCI/NVMe: bus 번호 부족 오류 */
		/* PCI/NVMe: 결과 반환: -1 */
		return -1;
	}

	/* Scan bridges that are already configured */
	busnr = pci_scan_bridge(parent, dev, busnr, 0); /* PCI/NVMe: pass 0로 이미 설정된 bridge 스캔 */

	/*
	 * Distribute the available bus numbers between hotplug-capable
	 * bridges to make extending the chain later possible.
	 */
	available_buses = end - busnr; /* PCI/NVMe: 남은 추가 bus 번호 계산 */

	/* Scan bridges that need to be reconfigured */
	pci_scan_bridge_extend(parent, dev, busnr, available_buses, 1); /* PCI/NVMe: pass 1로 재설정 및 하위 스캔; NVMe 장치 탐색 */

	if (!dev->subordinate) /* PCI/NVMe: subordinate bus가 생성되지 않았으면 실패 */
		/* PCI/NVMe: 결과 반환: -1 */
		return -1;

	return 0; /* PCI/NVMe: hotplug bridge 통합 성공 */
}
/* PCI/NVMe: EXPORT_SYMBOL_GPL 함수 호출 */
EXPORT_SYMBOL_GPL(pci_hp_add_bridge); /* PCI/NVMe: 심볼 외부 낸출. nvme-pci 등 모듈에서 참조 가능 */
