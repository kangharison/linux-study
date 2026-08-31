// SPDX-License-Identifier: GPL-2.0
/*
 * Support routines for initializing a PCI subsystem
 *
 * Extruded from code written by
 *      Dave Rusling (david.rusling@reo.mts.dec.com)
 *      David Mosberger (davidm@cs.arizona.edu)
 *	David Miller (davem@redhat.com)
 *
 * Nov 2000, Ivan Kokshaysky <ink@jurassic.park.msu.ru>
 *	     PCI-PCI bridges cleanup, sorted resource allocation.
 * Feb 2002, Ivan Kokshaysky <ink@jurassic.park.msu.ru>
 *	     Converted to allocation in 3 passes, which gives
 *	     tighter packing. Prefetchable range support.
 */


/*
 * [한국어 설명] PCI 버스 자원(BAR/브리지 윈도우) 크기 산정과 주소 배치 (setup-bus.c)
 *
 * === 파일의 역할 ===
 * PCI 장치가 요구하는 메모리/IO 공간(BAR)과, 그 장치들을 아래에 거느린
 * PCI-to-PCI 브리지가 열어 주어야 하는 "윈도우(window)"의 크기와 실제 주소를
 * 결정하는 파일이다. 열거(probe.c)가 "이 장치는 이만큼의 공간을 원한다"를
 * 알아내는 단계라면, 이 파일은 "그 요구들을 CPU 물리 주소 공간의 어디에
 * 어떻게 겹치지 않게 놓을 것인가"를 푸는 단계다. 실제로 한 개의 자원에
 * 주소를 잡아 주는 원자적 동작(pci_assign_resource 등)은 setup-res.c 가 맡고,
 * 이 파일은 그 위에서 "무엇을 어떤 순서로 얼마만큼" 잡을지를 결정하는
 * 정책/알고리즘 계층이다.
 * 파일 맨 위 영어 주석이 밝히듯 이 코드는 2002년에 Ivan Kokshaysky 가
 * "3 passes" 방식으로 개편해 더 조밀한 패킹(tighter packing)을 얻었고,
 * prefetchable 범위 지원이 그때 함께 들어왔다.
 *
 * --- 왜 여러 패스로 나누는가 (크기 결정 <-> 주소 결정의 분리) ---
 * 브리지 윈도우는 그 아래 모든 장치의 BAR 를 한 덩어리로 감싸야 한다.
 * 그런데 브리지 윈도우의 크기는 "아래에 뭐가 얼마나 있는지"를 다 세어 봐야
 * 알 수 있고, 반대로 아래 장치의 BAR 주소는 "위 브리지 윈도우가 어디에
 * 놓였는지"를 알아야 정해진다. 두 결정은 서로 반대 방향의 의존을 갖는다.
 * 그래서 한 번에 풀 수 없고 방향을 나눈다:
 *   (1) 크기 모으기 pass — __pci_bus_size_bridges() 가 리프에서 루트 쪽으로
 *       (깊이 우선, bottom-up) 올라오며 각 브리지 창에 필요한 크기와 정렬을
 *       계산한다. 이때는 아직 주소를 정하지 않고 "크기"만 resource 에 적는다.
 *   (2) 여유 분배 pass — pci_root_bus_distribute_available_resources() 와
 *       pci_bus_distribute_available_resources() 가 위에서 아래로 내려가며
 *       남는 공간을 핫플러그 브리지들에 미리 나눠 준다(나중에 장치를 꽂을
 *       자리를 지금 확보해 두는 것).
 *   (3) 주소 배치 pass — __pci_bus_assign_resources() 가 루트에서 리프 쪽으로
 *       (top-down) 내려가며 실제 주소를 잡고, 브리지에는 base/limit 레지스터를
 *       기록한다(pci_setup_bridge).
 * 크기 단계와 배치 단계를 붙여 놓으면, 먼저 배치한 자원이 아직 크기를 모르는
 * 형제 자원의 자리를 잘라먹어 나중 것이 못 들어가는 일이 생긴다. 분리해 두면
 * "전체 요구량을 다 안 상태에서" 배치를 시작할 수 있어 훨씬 조밀해진다.
 *
 * --- 왜 정렬이 큰 것부터 배치하는가 ---
 * PCI BAR 는 자기 크기만큼 자연 정렬(naturally aligned)되어야 한다. 즉 1MB
 * BAR 는 1MB 경계에만 놓을 수 있다. 작은 것부터 놓으면, 남은 공간이 총량으로는
 * 충분해도 "큰 정렬 경계에 걸치는 연속 빈칸"이 남지 않아 큰 BAR 가 들어갈 곳이
 * 사라진다. 예를 들어 4MB 창에 1MB 짜리를 앞에 놓으면 남은 3MB 안에는 4MB 정렬
 * 경계가 없다. 반대로 큰 것부터 놓으면 큰 정렬 경계에 딱 맞게 채워지고, 그
 * 뒤에 남는 자투리는 작은(정렬 요구도 작은) 자원이 그대로 채울 수 있다.
 * 그래서 pdev_sort_resources() 는 정렬 내림차순으로 리스트를 만들고,
 * __assign_resources_sorted() 는 그 순서를 그대로 따라 배치한다. add_size
 * 때문에 정렬이 커지면 리스트 안에서 순서를 다시 옮기는 코드까지 들어 있다.
 *
 * --- 실패하면 어떻게 물러나는가 (fail_head 와 재시도 루프) ---
 * 배치는 실패할 수 있다(상위 창이 좁거나, 펌웨어가 잡아 둔 고정 자원과 겹치거나).
 * assign_requested_resources_sorted() 는 실패한 자원을 @fail_head 리스트에
 * 모아 둔다. 호출자(pci_assign_unassigned_root_bus_resources 등)는 fail_head 가
 * 비어 있지 않으면 pci_prepare_next_assign_round() 로
 *   - 실패한 자원이 속한 브리지 창을 놓아 주고(pci_bus_release_bridge_resources)
 *   - 저장해 둔 원래 크기/플래그를 pci_dev_res_restore() 로 되돌린 뒤
 *   - 다시 크기 산정부터 반복한다.
 * 1차 시도는 브리지 자원을 건드리지 않고, 2차부터 리프 브리지 창을 놓고,
 * 3차부터는 서브트리 전체(whole_subtree)를 놓는 식으로 점점 과감해진다.
 * 반복 횟수 상한은 버스 트리 깊이(pci_bus_get_depth)+1 이다.
 * __assign_resources_sorted() 안에도 더 작은 되돌리기가 하나 더 있다:
 * "요구 크기 + 선택적 크기"로 먼저 전부 시도해 보고, 필수 자원이 하나라도
 * 실패하면 save_head 에 저장해 둔 원래 값으로 되돌린 뒤 "필수만" 다시 배치하고,
 * 남는 공간이 있으면 그때 선택적 크기를 reassign_resources_sorted() 로 덧붙인다.
 *
 * --- add_size / min_align 의 의미 (선택적 추가 크기) ---
 * struct pci_dev_resource 의 add_size 와 min_align 은 "이만큼 더 있으면 좋지만
 * 없어도 동작에는 지장이 없다"는 선택적(optional) 요구를 표현한다.
 *   - add_size: 필수 크기 위에 더 얹고 싶은 바이트 수. 대표적 출처는 핫플러그
 *     브리지에 미리 잡아 두는 예비 공간(pci_hotplug_io_size 계열)과,
 *     SR-IOV VF BAR 나 비활성 ROM 처럼 없어도 되는 자원의 크기다.
 *   - min_align: 그 add_size 를 반영했을 때 창이 지켜야 할 최소 정렬.
 *     크기를 키우면 정렬 요구도 같이 커지므로 별도로 들고 다닌다.
 * 이 두 값을 담은 항목은 realloc_head(= add_list) 리스트에 실린다.
 * 배치가 빡빡하면 이 목록의 요구는 통째로 포기되고, 여유가 있으면
 * reassign_resources_sorted() 가 뒤늦게 창을 넓혀 준다.
 * pci_resource_is_optional() 이 "이 자원은 실패해도 치명적이지 않다"의 판정자다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * PCI 초기화 흐름에서 이 파일은 열거 직후, 드라이버 probe 직전에 놓인다:
 *   pci_scan_root_bus / pci_scan_child_bus (probe.c)
 *     -> 각 장치의 BAR 크기 읽기(__pci_read_base) — "얼마나 필요한가"
 *     -> [이 파일] 크기 산정 -> 여유 분배 -> 주소 배치 -> 브리지 창 기록
 *     -> pci_bus_add_devices() -> 드라이버 probe() -> 드라이버가 BAR 를 ioremap
 * 실행 컨텍스트는 커널 프로세스 문맥이다. 부팅 시 PCI 초기화 경로와,
 * 핫플러그 슬롯 이벤트를 처리하는 워크큐 문맥 양쪽에서 불린다. 인터럽트
 * 문맥에서는 불리지 않는다(config 공간 접근과 kzalloc, 세마포어를 쓴다).
 * 동기화는 호출자가 잡는다: pci_do_resource_release_and_resize() 와
 * pci_assign_unassigned_bus_resources() 는 down_read(&pci_bus_sem) 으로 버스
 * 트리 순회를 보호한다. 이 파일 안에서 자체적으로 잡는 락은 없다.
 * 여기서 결정된 값은 최종적으로 브리지의 Type 1 config 헤더(base/limit
 * 레지스터)와 각 장치의 BAR 에 기록되어, 이후 모든 MMIO 접근의 주소가 된다.
 *
 * === 타 모듈과의 연결 ===
 * 이 파일이 의존하는 쪽(callee):
 *   - drivers/pci/setup-res.c — pci_assign_resource(), pci_reassign_resource(),
 *     pci_release_resource(), pci_claim_resource(), pci_update_resource().
 *     자원 하나에 실제 주소를 잡고 BAR 에 써 넣는 실무를 담당한다.
 *   - drivers/pci/bus.c — pci_bus_clip_resource(), pci_bus_resource_n(),
 *     pci_walk_bus(). 버스의 자원 목록을 다룬다.
 *   - drivers/pci/setup-cardbus.c — pci_bus_size_cardbus_bridge(),
 *     pci_setup_cardbus_bridge(). CardBus 브리지는 규칙이 달라 분리돼 있다.
 *   - drivers/pci/rebar.c — pci_rebar_get_current_size(), pci_rebar_set_size(),
 *     pci_resize_resource_set_size(). Resizable BAR 크기 변경.
 *   - drivers/pci/probe.c — pci_read_bridge_bases(). 펌웨어가 이미 프로그램해
 *     둔 브리지 창 값을 읽어 온다(claim 경로).
 *   - drivers/pci/host-bridge.c — pcibios_resource_to_bus(). CPU 물리 주소를
 *     버스 주소로 바꾼다(주소 변환이 있는 플랫폼 대응).
 *   - kernel/resource.c 계열 — request_resource(), release_child_resources().
 * 이 파일에 의존하는 쪽(caller) — 이 트리에서 확인한 실제 호출부:
 *   - drivers/pci/probe.c:7936 pci_bus_claim_resources()
 *   - drivers/pci/probe.c:7945, drivers/pci/pci-acpi.c:2041
 *     pci_assign_unassigned_root_bus_resources()
 *   - drivers/pci/probe.c:8427, drivers/pci/hotplug/pciehp_pci.c:80,
 *     drivers/pci/hotplug/shpchp_pci.c:55,
 *     drivers/pci/hotplug/cpci_hotplug_pci.c:276
 *     pci_assign_unassigned_bridge_resources()
 *   - drivers/pci/probe.c:8478, drivers/pci/pci-sysfs.c:1605,
 *     drivers/pci/controller/vmd.c:949
 *     pci_assign_unassigned_bus_resources()
 *   - drivers/pci/rebar.c:410 pci_do_resource_release_and_resize()
 *   - drivers/pci/setup-cardbus.c pci_dev_res_add_to_list()
 *   - drivers/pci/setup-res.c pci_min_window_alignment(),
 *     pci_resource_is_optional()
 *   - drivers/pci/pci.c pci_realloc_get_opt() (부팅 파라미터 pci=realloc 파싱)
 * 공유 상태: struct pci_dev 의 resource[] 배열(BAR 와 브리지 창이 한 배열에
 * 구획을 나눠 들어 있다), struct pci_bus 의 자원 목록, 전역 pci_flags,
 * 전역 pci_hotplug_io_size 계열, 그리고 kernel/resource.c 의 자원 트리
 * (iomem_resource / ioport_resource 를 뿌리로 하는 부모-자식 트리).
 *
 * --- NVMe 학습자를 위한 연결 (근거와 함께) ---
 * 확인 결과: drivers/nvme 아래 어떤 파일도 이 파일의 함수를 직접 부르지 않는다
 * (주석을 제거한 코드 전체에서 토큰 검색 -> 직접 호출 0건).
 * 그래도 이 파일은 NVMe 동작의 전제 조건을 만든다. NVMe 드라이버는
 * drivers/nvme/host/pci.c:3001 에서
 *     dev->bar = ioremap(pci_resource_start(pdev, 0), size);
 * 로 BAR0 를 매핑한다. 여기서 pci_resource_start(pdev, 0) 이 돌려주는 값,
 * 즉 NVMe 컨트롤러 레지스터와 도어벨(doorbell) 창이 놓인 물리 주소가 바로
 * 이 파일이 결정한 값이다. 부팅 펌웨어가 이미 주소를 잡아 둔 경우에는
 * pci_bus_claim_resources() 경로로 그 값을 그대로 승인하고, 잡혀 있지 않으면
 * pci_assign_unassigned_* 경로가 여기 알고리즘으로 새로 정한다.
 * NVMe SSD 가 핫플러그 슬롯이나 Thunderbolt 뒤에 꽂히는 경우에는
 * drivers/pci/hotplug/pciehp_pci.c:80 이 부르는
 * pci_assign_unassigned_bridge_resources() 가 이 파일의 진입점이 된다.
 * 참고: NVMe 는 pci_iomap() / pci_request_regions() / pci_enable_msix_range() 를
 * 쓰지 않는다(같은 방식으로 검색해 0건 확인). pci_enable_device_mem() 으로
 * 메모리 디코딩을 켠 뒤 직접 ioremap 한다 — 그 디코딩이 의미를 가지려면
 * BAR 에 유효한 주소가 이미 들어 있어야 하고, 그 일을 이 파일이 한다.
 *
 * === 주요 함수/구조체 요약 ===
 *  - struct pci_dev_resource: 이 파일의 핵심 자료구조. 배치 작업 중인 자원
 *    하나를 리스트 원소로 감싼 것. res/dev 로 대상을 가리키고,
 *    start/end/flags 에 "건드리기 전의 원래 값"을 저장해 실패 시 되돌리며,
 *    add_size/min_align 으로 선택적 추가 요구를 실어 나른다.
 *  - __pci_bus_size_bridges(): 크기 산정 pass 의 재귀 본체. 아래에서 위로.
 *  - pbus_size_io() / pbus_size_mem(): 한 버스의 IO 창 / MEM 창 크기와
 *    정렬을 계산한다. pbus_size_mem() 안의 aligns[] 히스토그램이 정렬별
 *    합계를 모아 최소 창 정렬을 구하는 핵심 계산이다.
 *  - __assign_resources_sorted(): 배치 pass 의 심장. 정렬 내림차순 리스트를
 *    받아 "필수+선택 전부 시도 -> 실패하면 되돌리고 필수만 -> 여유가 있으면
 *    선택분 덧붙이기"의 3단 전략을 수행한다.
 *  - pbus_select_window() / pbus_select_window_for_type(): 어떤 자원을 어느
 *    브리지 창(IO / non-prefetchable MEM / prefetchable MEM)에 넣을지 고른다.
 *  - pci_setup_bridge_io() / _mmio() / _mmio_pref(): 확정된 창 주소를 브리지의
 *    Type 1 config 헤더 base/limit 레지스터에 실제로 기록한다.
 *  - pci_assign_unassigned_root_bus_resources(): 루트 버스 전체를 대상으로
 *    "크기 -> 분배 -> 배치"를 실패 시 재시도까지 포함해 돌리는 최상위 진입점.
 *  - pci_bus_distribute_available_resources(): 남는 공간을 핫플러그 브리지에
 *    미리 나눠 주어, 나중에 장치를 꽂아도 재배치가 필요 없게 만든다.
 */

/* [한국어] ALIGN()/ALIGN_DOWN()/IS_ALIGNED() 매크로. 브리지 창과 BAR 는 반드시
 * 2의 거듭제곱 경계에 맞춰야 하므로 이 파일 전체가 이 매크로에 의존한다. */
#include <linux/align.h>
/* [한국어] __ffs() 등 비트 연산. 정렬 값(2의 거듭제곱)을 지수로 바꿔
 * aligns[] 히스토그램의 첨자를 만들 때 쓴다 (pbus_size_mem 참조). */
#include <linux/bitops.h>
/* [한국어] WARN_ON_ONCE() 제공. 배치 로직의 내부 불변식이 깨졌을 때(예: 이미
 * 배정된 자원을 되돌리려 할 때, add_list 가 비워지지 않고 남았을 때) 한 번만
 * 경고를 찍고 계속 진행하기 위해 쓴다 — 부팅을 멈추기에는 과한 상황들이다. */
#include <linux/bug.h>
/* [한국어] __init 섹션 지정자. pci_realloc_get_opt() 은 부팅 파라미터 파싱용이라
 * 초기화가 끝나면 메모리를 회수해도 되므로 __init 로 표시된다. */
#include <linux/init.h>
/* [한국어] 커널 공용 매크로 모음. 이 파일에서는 ARRAY_SIZE(aligns) 와
 * panic(), pr_info() 계열의 기본 선언을 여기서 끌어온다. */
#include <linux/kernel.h>
/* [한국어] min()/max()/max_t() 매크로. 크기와 정렬을 합칠 때 "둘 중 큰 쪽"을
 * 고르는 계산이 이 파일 곳곳에 있다(정렬은 항상 더 엄격한 쪽을 따라야 한다). */
#include <linux/minmax.h>
/* [한국어] EXPORT_SYMBOL()/EXPORT_SYMBOL_GPL() 매크로. 이 파일은
 * pci_bus_size_bridges, pci_bus_assign_resources, pci_bus_claim_resources 를
 * 모듈에 공개하고, 핫플러그 드라이버용 심볼 2개를 GPL 전용으로 공개한다. */
#include <linux/module.h>
/* [한국어] struct pci_dev / struct pci_bus / resource[] 구획 상수
 * (PCI_BRIDGE_RESOURCES, PCI_BRIDGE_IO_WINDOW 등)와 IORESOURCE_* 플래그가
 * 여기서 온다. 이 파일의 거의 모든 타입이 이 헤더 소속이다. */
#include <linux/pci.h>
/* [한국어] -ENOMEM(kzalloc 실패), -EINVAL(창 선택 실패), -ENOSPC(재배치 후에도
 * 자리가 없음), -ENOENT(루트까지 올라갔는데 브리지가 없음) 등 반환 코드. */
#include <linux/errno.h>
/* [한국어] struct resource 와 resource_size()/resource_contains()/
 * request_resource()/release_child_resources() 등 자원 트리 API. PCI 자원은
 * 결국 kernel/resource.c 의 iomem_resource / ioport_resource 트리에 매달린다. */
#include <linux/ioport.h>
/* [한국어] 캐시라인 관련 정의 헤더. 다만 이 파일의 코드에서 __read_mostly 나
 * L1_CACHE_BYTES 같은 심볼을 쓰는 곳은 찾지 못했다(주석 제거 후 토큰 검색 0건).
 * 다른 헤더가 간접적으로 필요로 해서 남아 있는 것인지는 이 트리만으로는
 * 확인할 수 없다. */
#include <linux/cache.h>
/* [한국어] U32_MAX / SIZE_MAX 같은 타입 한계 상수 헤더. 이 파일 코드에서 직접
 * 쓰는 심볼은 찾지 못했다(토큰 검색 0건). 남아 있는 이유는 이 트리만으로는
 * 확인할 수 없다. */
#include <linux/limits.h>
/* [한국어] SZ_1K / SZ_4K / SZ_1M 크기 상수. 브리지 창의 최소 단위가 그대로
 * 여기서 온다 — I/O 창은 4KB(확장 시 1KB), 메모리 창은 1MB 경계다. */
#include <linux/sizes.h>
/* [한국어] kzalloc_obj()/kfree(). struct pci_dev_resource 를 리스트 원소로
 * 힙에 잡았다가 배치가 끝나면 해제한다 — 그래서 이 파일은 인터럽트 문맥에서
 * 불릴 수 없다. */
#include <linux/slab.h>
/* [한국어] ACPI_HANDLE() 과 acpi_ioapic_add(). 배치가 끝난 뒤
 * pci_assign_unassigned_resources() 가 루트 브리지에 짝이 되는 ACPI 장치가
 * 있으면 IOAPIC 등록을 이어서 수행한다. */
#include <linux/acpi.h>
/* [한국어] drivers/pci 내부 전용 헤더. pci_resource_num(),
 * pci_resource_is_bridge_win(), pci_resource_is_optional() 선언,
 * pci_hotplug_io_size 계열 전역, pci_assign_resource() 등 setup-res.c 의
 * 내부 API, pci_bus_sem 이 모두 여기 있다. 꺾쇠가 아니라 따옴표인 이유는
 * include/linux/pci.h 와 이름이 같은 서브시스템 사설 헤더이기 때문이다. */
#include "pci.h"

/* [한국어] 자원의 "종류"를 판별할 때 비교에 쓰는 플래그 마스크.
 * resource->flags 에는 정렬 요구(IORESOURCE_STARTALIGN 등)나 상태 비트
 * (IORESOURCE_UNSET, IORESOURCE_DISABLED)처럼 종류와 무관한 비트가 섞여 있어서,
 * 두 자원이 "같은 종류인가"를 볼 때는 반드시 이 마스크로 걸러야 한다.
 * 포함되는 4비트의 의미:
 *   IORESOURCE_IO       — I/O 포트 공간 (MEM 과 별개의 주소 공간)
 *   IORESOURCE_MEM      — 메모리 맵 공간
 *   IORESOURCE_PREFETCH — prefetchable. 읽어도 부작용이 없어 브리지가 미리
 *                         읽어 와도 되는 영역. 브리지는 이 영역을 위해
 *                         별도의 prefetchable 창을 가질 수 있다.
 *   IORESOURCE_MEM_64   — 64비트 주소 지정 가능. prefetchable 창이 64비트면
 *                         4GB 위쪽에 놓을 수 있어 32비트 공간을 아낀다.
 * 백슬래시 줄 잇기 중간에는 주석을 넣을 수 없어(백슬래시가 줄의 마지막 문자여야
 * 한다) 설명을 매크로 위쪽에 모아 둔다. */
#define PCI_RES_TYPE_MASK \
	(IORESOURCE_IO | IORESOURCE_MEM | IORESOURCE_PREFETCH |\
	 IORESOURCE_MEM_64)

/* [한국어] PCI 서브시스템 전역 동작 플래그 비트필드.
 * 역할: 아키텍처/플랫폼 코드가 "펌웨어 설정을 그대로 쓸지, 커널이 다시
 *   배치할지" 같은 전역 정책을 켜 두는 자리다.
 * 설정자: 아키텍처 초기화 코드와 PCI 호스트 컨트롤러 드라이버.
 * 읽는 자: drivers/pci 와 arch/ 의 여러 곳. 다만 이 파일 안에서 pci_flags 를
 *   실제로 읽는 코드는 없다 — 이 파일은 정의와 export 만 제공한다(정의를 둘
 *   자리가 필요해 여기 놓인 것으로 보이며, 그 이상의 이유는 이 트리만으로
 *   확인할 수 없다). 개별 비트 이름은 include/linux/pci.h 에 있고 그 헤더는
 *   이 트리에 없어 여기서 열거할 수 없다.
 * 값 범위: 비트마스크. 0 이면 아무 특수 정책도 켜지지 않은 기본 상태.
 * 동기화: 부팅 초기에 한 번 정해지고 이후에는 읽기만 하는 값이라 락이 없다. */
unsigned int pci_flags;
/* [한국어] 모듈(예: 호스트 컨트롤러 드라이버)에서도 이 전역을 볼 수 있도록
 * 공개한다. GPL 전용 심볼이다. */
EXPORT_SYMBOL_GPL(pci_flags);

/* [한국어]
 * struct pci_dev_resource - 배치 작업 중인 자원 하나를 감싸는 리스트 원소
 *
 * 이 파일의 핵심 자료구조다. 자원 배치 알고리즘은 struct resource 자체를
 * 직접 리스트에 꿰지 못한다. 이유가 둘 있다:
 *  (1) struct resource 는 이미 kernel/resource.c 의 부모-자식 트리에 매달려
 *      있어서, 그 연결 필드를 배치 알고리즘이 마음대로 쓸 수 없다.
 *  (2) 배치는 실패할 수 있고, 실패하면 건드리기 전 상태로 되돌려야 한다.
 *      그래서 "원래 값"을 어딘가 따로 적어 둘 그릇이 필요하다.
 * 그래서 자원마다 이 래퍼를 kzalloc 으로 하나씩 잡아 리스트에 넣는다.
 * 이 구조체가 실리는 리스트는 용도에 따라 넷으로 나뉜다:
 *   - head        : 이번에 배치할 자원들. 정렬 내림차순으로 정렬돼 있다.
 *   - realloc_head(= add_list) : 선택적 추가 크기를 요구하는 자원들.
 *   - fail_head   : 배치에 실패한 자원들. 재시도 전략의 입력이 된다.
 *   - save_head   : head 의 원래 값 사본. 되돌리기 전용.
 * 같은 struct resource 를 가리키는 래퍼가 head 와 realloc_head 에 각각 하나씩
 * 따로 존재하는 것이 정상이다(리스트마다 별도로 할당한다). res_to_dev_res() 가
 * "이 자원의 래퍼가 저 리스트에도 있는가"를 찾는 데 쓰인다.
 */
struct pci_dev_resource {
	struct list_head list;
	/* [한국어] 위에서 말한 head / realloc_head / fail_head / save_head 중
	 * 한 리스트에 이 원소를 꿰기 위한 연결 고리.
	 * 설정자: pci_dev_res_add_to_list() 의 list_add(), pdev_sort_resources()
	 *   의 list_add_tail(), 그리고 __assign_resources_sorted() 가 정렬 순서를
	 *   고칠 때 쓰는 list_move_tail().
	 * 읽는 자: list_for_each_entry() 계열 순회 전부.
	 * 값 범위: 항상 어떤 리스트에 연결된 상태이거나, list_del() 직후
	 *   곧바로 kfree() 되는 상태 둘 중 하나다.
	 * 동기화: 이 리스트들은 모두 호출자의 스택에 있는 LIST_HEAD 로,
	 *   한 배치 작업(한 스레드) 안에서만 다뤄지므로 락이 없다. 버스 트리
	 *   자체의 보호는 상위에서 down_read(&pci_bus_sem) 으로 이뤄진다. */

	struct resource *res;
	/* [한국어] 이 원소가 대표하는 실제 자원. struct pci_dev 의 resource[]
	 * 배열 원소를 가리키는 포인터다(BAR, ROM, VF BAR, 브리지 창 모두 이
	 * 한 배열에 구획을 나눠 들어 있다).
	 * 설정자: pci_dev_res_add_to_list(), pdev_sort_resources().
	 * 읽는 자: 거의 모든 함수. res_to_dev_res() 는 이 포인터 값을 키로
	 *   리스트를 검색하고, pci_resource_num(dev, res) 로 배열 인덱스를
	 *   역산해 "몇 번 BAR 인가"를 알아낸다.
	 * 값 범위: NULL 이 아닌 유효 포인터. 반드시 아래 dev 의 resource[] 안의
	 *   원소여야 한다 — pci_resource_num() 이 포인터 뺄셈으로 인덱스를
	 *   구하기 때문에 다른 곳을 가리키면 인덱스가 틀린다.
	 * 동기화: 가리키는 대상(struct resource)은 pci_dev 수명 동안 유지되고,
	 *   배치 작업은 직렬화돼 있어 별도 락이 없다. */

	struct pci_dev *dev;
	/* [한국어] res 를 소유한 PCI 장치. 브리지 창의 경우 그 창을 가진 브리지
	 * 장치 자신(= bus->self)이 들어간다.
	 * 설정자: pci_dev_res_add_to_list(), pdev_sort_resources().
	 * 읽는 자: pci_resource_num()/pci_resource_name() 호출, pci_dbg()/pci_info()
	 *   로그 출력, pci_assign_resource()/pci_release_resource() 호출.
	 * 값 범위: NULL 이 아닌 유효 포인터.
	 * 동기화: 참조 카운트를 따로 올리지 않는다. 배치 작업이 끝나기 전에
	 *   장치가 사라지지 않도록 보장하는 책임은 상위 호출자(핫플러그 코드,
	 *   pci_bus_sem)에 있다. */

	resource_size_t start;
	/* [한국어] 자원을 건드리기 전의 원래 시작 주소 사본(되돌리기용).
	 * 설정자: pci_dev_res_add_to_list() 와 pdev_sort_resources() 가
	 *   등록 시점의 res->start 를 그대로 복사해 둔다.
	 * 읽는 자: pci_dev_res_restore() 가 res->start 에 되써서 원상 복구한다.
	 * 값 범위: 아직 배정되지 않은 자원이면 크기 계산용 임시 값일 수 있고,
	 *   배정된 자원이면 실제 주소다.
	 * 동기화: 순수한 사본이라 경쟁 없음. */

	resource_size_t end;
	/* [한국어] 원래 끝 주소 사본. start 와 짝을 이뤄 "원래 크기"를 복원한다.
	 * 설정자: 위 start 와 동일한 지점.
	 * 읽는 자: pci_dev_res_restore().
	 * 값 범위: 크기는 resource_size() 로 얻는다(대략 end - start + 1).
	 * 동기화: 순수한 사본이라 경쟁 없음. */

	resource_size_t add_size;
	/* [한국어] "있으면 좋지만 없어도 되는" 추가 크기(바이트). 이 파일의
	 * 선택적(optional) 요구를 표현하는 핵심 필드다.
	 * 설정자: pci_dev_res_add_to_list() 의 인자로 들어온다. 실제 출처는
	 *   (a) 핫플러그 브리지에 미리 잡아 두는 예비 공간 — pbus_size_io()/
	 *       pbus_size_mem() 이 size1 - size0 을 넘긴다,
	 *   (b) SR-IOV VF BAR 나 비활성 ROM 처럼 없어도 되는 자원의 크기,
	 *   (c) adjust_bridge_window() 가 창을 줄일 때 여기서 깎아 낸다.
	 * 읽는 자: get_res_add_size(), reassign_resources_sorted(),
	 *   __assign_resources_sorted() 가 res->end 에 더해 "크게 시도"할 때.
	 * 값 범위: 0 이면 추가 요구 없음. 이 필드는 realloc_head 리스트의
	 *   원소에서만 의미가 있고, head/fail_head 원소에는 0 이 들어간다
	 *   (assign_requested_resources_sorted 가 넘기는 "don't care" 0).
	 * 동기화: 배치 작업 스레드 전용. 락 없음. */

	resource_size_t min_align;
	/* [한국어] add_size 를 반영했을 때 이 자원(주로 브리지 창)이 지켜야 하는
	 * 최소 정렬. 크기를 키우면 정렬 요구도 함께 커지므로 add_size 와 짝으로
	 * 들고 다닌다.
	 * 설정자: pci_dev_res_add_to_list() 의 인자. pbus_size_io() 는
	 *   min_align 을, pbus_size_mem() 은 add_align 을 넘긴다.
	 * 읽는 자: reassign_resources_sorted() 가 pci_reassign_resource() 에
	 *   넘기는 정렬 값으로 쓰고, __assign_resources_sorted() 는 이 값이
	 *   현재 정렬보다 크면 리스트 안 순서까지 다시 잡는다(정렬 내림차순
	 *   불변식을 지키기 위해).
	 * 값 범위: 2의 거듭제곱, 또는 0(정렬 요구 없음).
	 * 동기화: 배치 작업 스레드 전용. 락 없음. */

	unsigned long flags;
	/* [한국어] 자원을 건드리기 전의 원래 res->flags 사본(되돌리기용).
	 * 설정자: pci_dev_res_add_to_list(), pdev_sort_resources().
	 * 읽는 자: (a) pci_dev_res_restore() 가 res->flags 를 되돌릴 때,
	 *   (b) pci_fail_res_type_mask() 가 실패한 자원들의 종류를 OR 로 모을 때,
	 *   (c) pci_required_resource_failed() 와 pci_prepare_next_assign_round()
	 *       가 PCI_RES_TYPE_MASK 로 걸러 종류를 비교할 때,
	 *   (d) reassign_resources_sorted() 가 IORESOURCE_STARTALIGN/SIZEALIGN
	 *       두 비트만 골라 현재 res->flags 에 되살릴 때.
	 * 값 범위: IORESOURCE_* 비트 조합. 종류 비트(IO/MEM/PREFETCH/MEM_64)와
	 *   정렬 방식 비트(STARTALIGN = 시작 주소를 정렬, SIZEALIGN = 크기 자체가
	 *   정렬 단위), 상태 비트(UNSET/DISABLED)가 섞여 있다.
	 * 동기화: 순수한 사본이라 경쟁 없음. */
};

/*
 * [한국어]
 * pci_dev_res_free_list - 리스트에 달린 pci_dev_resource 래퍼를 전부 해제한다
 *
 * @head: 비울 리스트의 머리. head/realloc_head/fail_head/save_head 중 무엇이든
 *        올 수 있다. 리스트 자체는 호출자의 스택에 있는 LIST_HEAD 라 여기서
 *        해제하지 않고, 그 위에 매달린 원소들만 해제한다.
 * @return: 없음. 실패할 수 없는 연산이다.
 *
 * 왜 필요한가: 이 파일의 리스트 원소는 전부 kzalloc 으로 잡은 힙 객체다.
 * 배치가 끝났거나, 중간에 포기했거나, 저장본이 더 이상 필요 없어졌을 때
 * 누수 없이 한 번에 정리하는 통로가 필요하다.
 * 동작: 원소를 하나씩 리스트에서 떼어 내고(list_del) 곧바로 kfree 한다.
 *   함수가 끝나면 head 는 빈 리스트가 되므로, 같은 head 를 재사용해도 된다.
 * 실행 컨텍스트: 프로세스 문맥. kfree 를 부르므로 인터럽트 문맥 불가.
 *   락은 잡지 않는다 — 이 리스트들은 한 배치 작업 안에서만 쓰인다.
 * 주의: 원소가 가리키는 struct resource 자체는 pci_dev 소유라 건드리지
 *   않는다. 여기서 해제되는 것은 래퍼뿐이다.
 *
 * 호출 체인:
 *   __assign_resources_sorted() / pci_prepare_next_assign_round() /
 *   pci_assign_unassigned_root_bus_resources() /
 *   pci_assign_unassigned_bridge_resources() / pbus_reassign_bridge_resources() /
 *   pci_do_resource_release_and_resize() / pci_assign_unassigned_bus_resources()
 *     -> [이 함수] -> list_del(), kfree()
 */
static void pci_dev_res_free_list(struct list_head *head)
{
	/* [한국어] dev_res 는 현재 원소, tmp 는 다음 원소를 미리 잡아 두는 자리.
	 * 순회 도중 현재 원소를 kfree 하기 때문에 "다음"을 먼저 확보해야 한다. */
	struct pci_dev_resource *dev_res, *tmp;

	/* [한국어] _safe 판을 쓰는 이유가 바로 위 tmp 다. 일반
	 * list_for_each_entry() 로는 kfree 된 메모리에서 next 를 읽게 된다. */
	list_for_each_entry_safe(dev_res, tmp, head, list) {
		/* [한국어] 먼저 리스트에서 떼어 낸다. 떼어 내지 않고 해제하면
		 * 이웃 원소의 포인터가 해제된 메모리를 가리키게 된다. */
		list_del(&dev_res->list);
		/* [한국어] 래퍼 자체를 해제. res 가 가리키는 struct resource 는
		 * pci_dev 소유이므로 절대 해제하지 않는다. */
		kfree(dev_res);
	}
}

/*
 * [한국어]
 * pci_dev_res_add_to_list - 자원 하나를 추적 리스트에 등록한다
 *
 * @head: 등록 대상 리스트. 어떤 리스트냐에 따라 의미가 완전히 달라진다 —
 *        realloc_head 면 "이 자원에 add_size 만큼 더 주고 싶다"는 소원 목록,
 *        fail_head 면 "이 자원 배치에 실패했다"는 실패 보고서,
 *        save_head 면 "건드리기 전 값은 이랬다"는 되돌리기용 스냅숏,
 *        pci_do_resource_release_and_resize() 의 saved 면 리사이즈 롤백용이다.
 * @dev: 자원의 소유 장치. 브리지 창이면 브리지 자신(bus->self).
 * @res: 추적할 자원. dev->resource[] 안의 원소여야 한다.
 * @add_size: 선택적으로 더 주고 싶은 크기. 관심 없는 리스트(fail/save)에는
 *        0 을 넘긴다(호출부에 "don't care" 주석이 달려 있다).
 * @min_align: add_size 를 반영했을 때 지켜야 할 최소 정렬. 위와 같이
 *        관심 없으면 0.
 * @return: 0 성공, -ENOMEM 이면 래퍼 할당 실패. 호출자 처리는 제각각이다 —
 *        __assign_resources_sorted() 는 save_head 를 통째로 버리고 저장 없이
 *        진행하고(되돌리기를 포기하는 것), pbus_reassign_bridge_resources()
 *        와 pci_do_resource_release_and_resize() 는 오류를 그대로 위로 올린다.
 *        pbus_size_io()/pbus_size_mem()/assign_requested_resources_sorted() 는
 *        반환값을 아예 보지 않는데, 실패하면 "선택적 요구를 못 실었다"거나
 *        "실패 보고를 못 했다" 정도의 손해라 진행을 막지 않기 때문이다.
 *
 * 왜 필요한가: 배치 알고리즘의 모든 리스트가 이 한 함수를 통해 만들어진다.
 * 여기서 res->start/end/flags 를 사본으로 떠 두기 때문에, 나중에 배치가
 * 실패해도 pci_dev_res_restore() 로 원래 상태를 되살릴 수 있다.
 * 동작: 래퍼를 0 초기화 할당 -> 대상과 원래 값 복사 -> 리스트 머리에 삽입.
 * 실행 컨텍스트: 프로세스 문맥(kzalloc 사용). 락 없음.
 * 삽입 위치가 tail 이 아니라 head 인 점에 유의: 이 함수가 만드는 리스트들은
 * 순서가 의미를 갖지 않는다. 순서가 중요한 head 리스트는 이 함수가 아니라
 * pdev_sort_resources() 가 정렬 위치를 찾아 list_add_tail() 로 직접 넣는다.
 *
 * 호출 체인:
 *   pbus_size_io() / pbus_size_mem() / pbus_size_mem_optional() /
 *   assign_requested_resources_sorted() / __assign_resources_sorted() /
 *   pbus_reassign_bridge_resources() / pci_do_resource_release_and_resize() /
 *   drivers/pci/setup-cardbus.c
 *     -> [이 함수] -> kzalloc_obj(), list_add()
 */
/**
 * pci_dev_res_add_to_list() - Add a new resource tracker to the list
 * @head:	Head of the list
 * @dev:	Device to which the resource belongs
 * @res:	Resource to be tracked
 * @add_size:	Additional size to be optionally added to the resource
 * @min_align:	Minimum memory window alignment
 */
int pci_dev_res_add_to_list(struct list_head *head, struct pci_dev *dev,
			    struct resource *res, resource_size_t add_size,
			    resource_size_t min_align)
{
	/* [한국어] 새로 만들 래퍼. 아래 kzalloc_obj(*tmp) 가 이 변수의 타입에서
	 * 크기를 뽑아내므로 선언과 할당이 짝을 이룬다. */
	struct pci_dev_resource *tmp;

	/* [한국어] sizeof(struct pci_dev_resource) 만큼 0 으로 채워 할당한다.
	 * 0 초기화라서 아래에서 대입하지 않는 필드(list)는 안전한 초기 상태가
	 * 된다. GFP 플래그는 매크로 안에 감춰져 있다. */
	tmp = kzalloc_obj(*tmp);
	if (!tmp)
		/* [한국어] 메모리 부족. 위 @return 설명대로 호출자마다 대응이
		 * 다르며, 상당수는 이 실패를 치명적으로 보지 않는다. */
		return -ENOMEM;

	/* [한국어] 추적 대상 자원 포인터. 이후 res_to_dev_res() 검색의 키가 된다. */
	tmp->res = res;
	/* [한국어] 소유 장치. 로그 출력과 pci_resource_num() 인덱스 역산에 쓰인다. */
	tmp->dev = dev;
	/* [한국어] 여기부터 세 줄이 "되돌리기용 스냅숏"이다. 지금 시점의 값을
	 * 떠 두어야 배치 실패 후 pci_dev_res_restore() 가 복구할 수 있다. */
	tmp->start = res->start;
	/* [한국어] 끝 주소 사본 — start 와 함께 원래 크기를 복원한다. */
	tmp->end = res->end;
	/* [한국어] 플래그 사본 — 종류 비트와 정렬 방식 비트가 함께 보존된다.
	 * pci_fail_res_type_mask() 는 이 사본을 읽어 실패 종류를 집계한다. */
	tmp->flags = res->flags;
	/* [한국어] 선택적 추가 크기. 관심 없는 리스트에는 0 이 들어온다. */
	tmp->add_size = add_size;
	/* [한국어] 그 추가 크기를 반영했을 때의 최소 정렬 요구. */
	tmp->min_align = min_align;

	/* [한국어] 리스트 머리에 삽입. 이 함수가 만드는 리스트들은 순서가
	 * 의미를 갖지 않으므로 가장 싼 삽입 위치를 쓴다. */
	list_add(&tmp->list, head);

	/* [한국어] 성공. */
	return 0;
}

/*
 * [한국어]
 * pci_dev_res_remove_from_list - 특정 자원을 추적하는 래퍼 하나를 리스트에서 뺀다
 *
 * @head: 검색 대상 리스트(주로 realloc_head 또는 save_head).
 * @res: 찾을 자원. 포인터 값이 같은 원소를 찾는다(내용 비교가 아니다).
 * @return: 없음. 못 찾아도 조용히 아무 일도 하지 않는다 — 호출부들이
 *          "있으면 빼라"는 의미로 부르기 때문에 없는 것이 오류가 아니다.
 *
 * 왜 필요한가: 선택적 요구가 이미 충족되었거나 더 이상 유효하지 않게 되면
 * realloc_head 에서 그 항목을 치워야 한다. 예를 들어
 *  - __assign_resources_sorted() 가 첫 시도에서 전부 성공하면 head 에 있던
 *    자원들의 add_size 요구는 이미 반영된 것이므로 realloc_head 에서 뺀다.
 *  - adjust_bridge_window() 가 창을 확정했으면 그 창의 추가 요구를 없앤다.
 * 동작: 리스트를 앞에서부터 훑어 res 포인터가 일치하는 첫 원소를 떼고
 *   해제한 뒤 break 한다. 같은 자원의 래퍼가 한 리스트에 둘 이상 들어가는
 *   경우는 이 파일의 사용 방식에서 발생하지 않는다.
 * 실행 컨텍스트: 프로세스 문맥(kfree). 락 없음.
 *
 * 호출 체인:
 *   __assign_resources_sorted() / adjust_bridge_window()
 *     -> [이 함수] -> list_del(), kfree()
 */
static void pci_dev_res_remove_from_list(struct list_head *head,
					 struct resource *res)
{
	/* [한국어] 순회 커서와, kfree 후에도 다음으로 넘어갈 수 있게 하는 예비 포인터. */
	struct pci_dev_resource *dev_res, *tmp;

	/* [한국어] 원소를 해제하며 순회하므로 _safe 판이 필요하다. */
	list_for_each_entry_safe(dev_res, tmp, head, list) {
		/* [한국어] 자원 "포인터"가 같은지 비교한다. 같은 struct resource
		 * 를 가리키는 래퍼가 이 리스트에 있다는 뜻이다. */
		if (dev_res->res == res) {
			/* [한국어] 리스트에서 분리 — 이웃의 포인터를 먼저 정리한다. */
			list_del(&dev_res->list);
			/* [한국어] 래퍼 해제. 대상 자원 자체는 그대로 둔다. */
			kfree(dev_res);
			/* [한국어] 첫 일치만 제거하고 종료. 더 훑을 이유가 없다. */
			break;
		}
	}
}

/*
 * [한국어]
 * res_to_dev_res - 자원 포인터로 그 자원의 래퍼를 리스트에서 찾는다
 *
 * @head: 검색할 리스트.
 * @res: 찾을 자원 포인터.
 * @return: 일치하는 struct pci_dev_resource 포인터, 없으면 NULL.
 *          NULL 은 오류가 아니라 "이 리스트에는 이 자원이 없다"는 정보이며,
 *          호출부들은 그 사실 자체를 판단에 쓴다. 예를 들어
 *          reassign_resources_sorted() 는 head 에 없는 자원을 건너뛰고,
 *          pbus_size_mem_optional() 은 NULL 이면 새로 등록해야 한다고 본다.
 *
 * 왜 필요한가: 같은 struct resource 를 가리키는 래퍼가 리스트마다 따로
 * 존재하므로, "이 자원이 저 리스트에도 올라와 있는가"를 묻는 조회가 자주
 * 필요하다. 자원 쪽에서 래퍼로 가는 역포인터를 두지 않았기 때문에 선형
 * 검색이 유일한 방법이다.
 * 성능 주의: O(n) 선형 검색이다. pbus_size_mem_optional() 에는 이 비용 때문에
 * "불필요할 때는 아예 부르지 말라"는 취지의 영어 주석이 붙어 있다.
 * 실행 컨텍스트: 순수 조회. 메모리 할당도 락도 없다.
 *
 * 호출 체인:
 *   get_res_add_size() / reassign_resources_sorted() /
 *   __assign_resources_sorted() / pbus_size_mem_optional() /
 *   adjust_bridge_window()
 *     -> [이 함수] -> list_for_each_entry()
 */
static struct pci_dev_resource *res_to_dev_res(struct list_head *head,
					       struct resource *res)
{
	/* [한국어] 순회 커서. 여기서는 원소를 지우지 않으므로 _safe 가 필요 없다. */
	struct pci_dev_resource *dev_res;

	/* [한국어] 리스트를 앞에서부터 선형 검색. */
	list_for_each_entry(dev_res, head, list) {
		/* [한국어] 포인터 동일성 비교 — 값이 같은 다른 자원이 아니라
		 * 정확히 같은 struct resource 를 찾는다. */
		if (dev_res->res == res)
			/* [한국어] 찾았으면 래퍼를 그대로 돌려준다. 호출자는
			 * 여기서 add_size / min_align 을 읽거나 고친다. */
			return dev_res;
	}

	/* [한국어] 끝까지 못 찾음 = 이 리스트에 이 자원은 등록돼 있지 않다. */
	return NULL;
}

/*
 * [한국어]
 * get_res_add_size - 어떤 자원에 걸려 있는 선택적 추가 크기를 조회한다
 *
 * @head: realloc_head(= 선택적 요구 목록).
 * @res: 조회할 자원.
 * @return: 등록돼 있으면 그 add_size, 등록돼 있지 않으면 0.
 *          0 은 "추가로 원하는 크기가 없다"와 같은 뜻이므로, 없음과 0 을
 *          구분할 필요가 없어 이렇게 뭉뚱그릴 수 있다.
 *
 * 왜 필요한가: 상위 브리지 창의 크기를 계산할 때는 자식들이 "필수로 요구한
 * 크기"뿐 아니라 "추가로 요구한 크기"까지 합산해야, 나중에 그 요구를
 * 들어줄 여지가 남는다. pbus_size_io() 가 children_add_size 를 모을 때
 * 자식 자원마다 이 함수를 부른다.
 * 실행 컨텍스트: 순수 조회. 내부적으로 O(n) 검색이 일어난다.
 *
 * 호출 체인:
 *   pbus_size_io() -> [이 함수] -> res_to_dev_res()
 */
static resource_size_t get_res_add_size(struct list_head *head,
					struct resource *res)
{
	/* [한국어] 조회 결과를 받을 자리. */
	struct pci_dev_resource *dev_res;

	/* [한국어] realloc_head 에서 이 자원의 래퍼를 찾는다. */
	dev_res = res_to_dev_res(head, res);
	/* [한국어] 찾았으면 add_size, 없으면 0. 삼항 연산자로 "없음 = 0" 을
	 * 자연스럽게 흡수한다. */
	return dev_res ? dev_res->add_size : 0;
}

/*
 * [한국어]
 * pci_dev_res_restore - 저장해 둔 원래 start/end/flags 로 자원을 되돌린다
 *
 * @dev_res: 되돌릴 자원의 래퍼. 등록 시점에 떠 둔 사본(start/end/flags)을
 *           품고 있다.
 * @return: 없음. 이미 배정된 자원이면 경고만 찍고 아무것도 하지 않는다.
 *
 * 왜 필요한가: 이 파일의 배치 전략은 "일단 크게 잡아 보고, 안 되면 원래대로
 * 돌려놓고 다시"이다. 그 "원래대로"를 실행하는 함수다. 되돌리지 않으면
 * 실패한 시도가 남긴 부풀려진 크기(res->end += add_size)나 바뀐 정렬 플래그가
 * 다음 시도의 계산을 오염시킨다.
 * 동작 단계:
 *   1) 래퍼에서 대상 자원과 장치를 꺼내고, 로그용 이름을 얻는다.
 *   2) 이미 부모 자원 트리에 편입된(= 배정 완료된) 자원이면 되돌리면 안 된다.
 *      배정된 자원의 start/end 를 몰래 바꾸면 자원 트리의 부모-자식 관계가
 *      실제 주소와 어긋나 버린다. 그래서 WARN_ON_ONCE 로 한 번 경고하고 만다.
 *   3) 그렇지 않으면 세 필드를 사본에서 되쓴다.
 * 실행 컨텍스트: 프로세스 문맥. 락 없음. 여기서 kfree 는 하지 않는다 —
 *   래퍼의 수명은 호출자가 관리한다.
 *
 * 호출 체인:
 *   __assign_resources_sorted() / pci_prepare_next_assign_round() /
 *   pci_do_resource_release_and_resize()
 *     -> [이 함수] -> pci_resource_num(), pci_resource_name(), pci_dbg()
 */
static void pci_dev_res_restore(struct pci_dev_resource *dev_res)
{
	/* [한국어] 되돌릴 대상 자원. */
	struct resource *res = dev_res->res;
	/* [한국어] 그 자원의 소유 장치 — 로그 출력과 인덱스 역산에 쓴다. */
	struct pci_dev *dev = dev_res->dev;
	/* [한국어] resource[] 배열에서 몇 번째인지를 포인터 뺄셈으로 역산한다.
	 * 이 값이 있어야 아래에서 사람이 읽을 이름("BAR 0" 등)을 얻을 수 있다. */
	int idx = pci_resource_num(dev, res);
	/* [한국어] 로그 메시지에 쓸 자원 이름 문자열. */
	const char *res_name = pci_resource_name(dev, idx);

	/* [한국어] 이미 배정이 끝난(부모 자원에 편입된) 자원을 되돌리려는 것은
	 * 호출부의 논리 오류다. 실제로 주소가 하드웨어에 반영된 뒤라 몰래
	 * 바꾸면 자원 트리와 하드웨어가 어긋난다. 부팅을 멈출 만한 일은 아니라
	 * WARN_ON_ONCE 로 한 번만 알리고 그냥 돌아간다. */
	if (WARN_ON_ONCE(resource_assigned(res)))
		return;

	/* [한국어] 여기부터 세 줄이 실제 복구다. 등록 시점의 사본을 되쓴다. */
	res->start = dev_res->start;
	/* [한국어] 끝 주소 복구 — start 와 함께 원래 크기가 되살아난다. */
	res->end = dev_res->end;
	/* [한국어] 플래그 복구 — 시도 중에 붙였던 IORESOURCE_STARTALIGN 이나
	 * 지웠던 IORESOURCE_DISABLED 같은 비트도 원상태로 돌아간다. */
	res->flags = dev_res->flags;

	/* [한국어] 디버그 로그. %pR 은 struct resource 를 "[mem 0x...-0x...]"
	 * 형태로 찍어 주는 커널 전용 포맷 지정자다. */
	pci_dbg(dev, "%s %pR: resource restored\n", res_name, res);
}

/*
 * Helper function for sizing routines.  Assigned resources have non-NULL
 * parent resource.
 *
 * Return first unassigned resource of the correct type.  If there is none,
 * return first assigned resource of the correct type.  If none of the
 * above, return NULL.
 *
 * Returning an assigned resource of the correct type allows the caller to
 * distinguish between already assigned and no resource of the correct type.
 */
/*
 * [한국어]
 * find_bus_resource_of_type - 버스의 자원 목록에서 원하는 종류의 창을 찾는다
 *
 * @bus: 검색할 버스. 이 함수는 주로 루트 버스에 대해 불린다(루트 버스에는
 *       브리지 창 대신 호스트 브리지가 신고한 여러 개의 aperture 가 달려 있다).
 * @type_mask: 비교할 때 볼 플래그 비트들. 호출부는 항상 type 과 같은 값을
 *       넘겨서 "이 비트들이 정확히 이 조합이어야 한다"는 뜻으로 쓴다.
 * @type: 원하는 플래그 조합.
 * @return: 조건에 맞는 자원. 우선순위는 (1) 아직 배정되지 않은 것, (2) 없으면
 *       이미 배정된 것, (3) 그것도 없으면 NULL. 이 우선순위의 의미는 바로
 *       아래 영어 주석이 설명한다 — 배정된 것이라도 돌려주어야 호출자가
 *       "이미 배정됨"과 "그런 종류가 아예 없음"을 구분할 수 있다.
 *
 * 왜 필요한가: 루트 버스의 자원 목록은 개수도 순서도 플랫폼마다 다르다.
 * "32비트 non-prefetchable 메모리 창"처럼 종류로 지목해 찾아야 한다.
 * 실행 컨텍스트: 순수 조회. 락 없음. 버스 자원 목록의 안정성은 상위에서
 *   보장한다.
 *
 * 호출 체인:
 *   pbus_select_window_for_type() -> [이 함수] -> pci_bus_for_each_resource()
 */
static struct resource *find_bus_resource_of_type(struct pci_bus *bus,
						  unsigned long type_mask,
						  unsigned long type)
{
	/* [한국어] r 은 순회 커서, r_assigned 는 "종류는 맞지만 이미 배정된"
	 * 첫 후보를 기억해 두는 자리(차선책). */
	struct resource *r, *r_assigned = NULL;

	/* [한국어] 이 버스가 가진 자원 목록을 처음부터 훑는다. */
	pci_bus_for_each_resource(bus, r) {
		/* [한국어] 세 가지를 걸러 낸다.
		 * - NULL: 자원 슬롯이 비어 있는 경우.
		 * - ioport_resource / iomem_resource: 커널 전역 자원 트리의
		 *   뿌리 자체다. 루트 버스가 "주소 공간 전체"를 자기 창으로
		 *   들고 있는 경우가 있는데, 그것을 브리지 창처럼 취급해
		 *   여기에 자식을 배치하면 안 된다. */
		if (!r || r == &ioport_resource || r == &iomem_resource)
			continue;

		/* [한국어] 종류 비교. type_mask 로 관심 있는 비트만 남긴 뒤
		 * type 과 "정확히" 일치해야 한다. 부분 일치가 아니라 완전
		 * 일치인 이유는, 예컨대 prefetchable 창을 찾을 때
		 * non-prefetchable 창이 걸리면 안 되기 때문이다. */
		if ((r->flags & type_mask) != type)
			continue;

		/* [한국어] 아직 배정되지 않은(부모 자원에 편입되지 않은) 창이
		 * 최우선이다 — 이 창은 지금 우리가 크기와 주소를 정할 수 있다. */
		if (!resource_assigned(r))
			return r;
		/* [한국어] 이미 배정된 창이면 곧바로 반환하지 않고 첫 개만
		 * 기억해 둔다. 더 뒤에 미배정 창이 있을 수 있으므로 순회는
		 * 계속한다. 두 번째 이후 배정된 창은 무시한다(첫 개면 충분). */
		if (!r_assigned)
			r_assigned = r;
	}
	/* [한국어] 미배정 창을 못 찾았을 때의 반환. 기억해 둔 배정된 창이 있으면
	 * 그것을, 아무것도 없었으면 NULL 을 돌려준다. */
	return r_assigned;
}

/**
 * pbus_select_window_for_type - Select bridge window for a resource type
 * @bus: PCI bus
 * @type: Resource type (resource flags can be passed as is)
 *
 * Select the bridge window based on a resource @type.
 *
 * For memory resources, the selection is done as follows:
 *
 * Any non-prefetchable resource is put into the non-prefetchable window.
 *
 * If there is no prefetchable MMIO window, put all memory resources into the
 * non-prefetchable window.
 *
 * If there's a 64-bit prefetchable MMIO window, put all 64-bit prefetchable
 * resources into it and place 32-bit prefetchable memory into the
 * non-prefetchable window.
 *
 * Otherwise, put all prefetchable resources into the prefetchable window.
 *
 * Return: the bridge window resource or NULL if no bridge window is found.
 */
/* [한국어] 아래 함수의 한국어 해설 (원문 kernel-doc 은 그 아래 그대로 둔다):
 * pbus_select_window_for_type - 자원 "종류"만 보고 담길 브리지 창을 고른다
 *
 * @bus: 자원이 놓일 버스. 이 버스의 위쪽 브리지가 가진 창 중에서 고른다.
 *       루트 버스면 브리지가 없으므로 호스트 브리지의 aperture 중에서 고른다.
 * @type: 자원 플래그. res->flags 를 그대로 넘겨도 되도록, 함수 안에서
 *        필요한 비트만 걸러 쓴다.
 * @return: 고른 창 자원, 없으면 NULL. NULL 이면 이 버스에는 그 종류의 자원을
 *        담을 곳이 없다는 뜻이라, 호출자(pbus_size_io/pbus_size_mem 등)는
 *        크기 계산 자체를 건너뛴다.
 *
 * 왜 필요한가: PCI-to-PCI 브리지는 창을 최대 세 개만 갖는다 — I/O 창,
 * non-prefetchable 메모리 창, prefetchable 메모리 창. 그런데 자원의 종류는
 * 그보다 세분화돼 있다(32/64비트 x prefetchable 여부). 그 사상(mapping)
 * 규칙을 한곳에 모은 것이 이 함수이며, 규칙 자체는 바로 아래 원문 kernel-doc
 * 이 조목조목 밝히고 있다. 요지는 "64비트 prefetchable 창이 있으면 64비트
 * prefetchable 자원만 거기 넣고, 32비트 prefetchable 은 non-prefetchable
 * 창으로 보낸다"이다 — 32비트 자원을 4GB 위쪽 창에 넣으면 주소를 표현할 수
 * 없기 때문이다.
 * 실행 컨텍스트: 순수 조회. 락 없음. 부작용 없음.
 *
 * 호출 체인:
 *   pbus_select_window() / pbus_size_io() / __pci_bus_size_bridges() /
 *   pci_prepare_next_assign_round()
 *     -> [이 함수] -> find_bus_resource_of_type(), pci_bus_resource_n()
 */
static struct resource *pbus_select_window_for_type(struct pci_bus *bus,
						    unsigned long type)
{
	/* [한국어] IORESOURCE_TYPE_BITS 는 "주소 공간 종류"만 뽑는 마스크다.
	 * 즉 IO 냐 MEM 이냐만 남기고 prefetch/64bit 같은 속성 비트는 뗀다
	 * (오른쪽 원문 주석 "w/o 64bit & pref" 가 그 뜻이다). */
	int iores_type = type & IORESOURCE_TYPE_BITS;	/* w/o 64bit & pref */
	/* [한국어] mmio = non-prefetchable 메모리 창, mmio_pref = prefetchable
	 * 메모리 창, win = 최종 선택 결과를 담을 임시 변수. */
	struct resource *mmio, *mmio_pref, *win;

	/* [한국어] 여기서는 반대로 속성 비트까지 포함해 남긴다. 아래 루트 버스
	 * 경로가 "완전히 일치하는 종류"부터 찾아 내려가야 하기 때문이다.
	 * res->flags 에 섞여 있던 정렬/상태 비트는 이 순간 전부 떨어져 나간다. */
	type &= PCI_RES_TYPE_MASK;			/* with 64bit & pref */

	/* [한국어] I/O 도 MEM 도 아닌 것(예: 버스 번호 자원, IRQ, DMA)은 브리지
	 * 창에 담기는 대상이 아니다. 조기에 NULL 로 돌려보낸다. */
	if ((iores_type != IORESOURCE_IO) && (iores_type != IORESOURCE_MEM))
		return NULL;

	/* [한국어] 루트 버스에는 위쪽 브리지가 없다. 대신 호스트 브리지가 신고한
	 * aperture 목록이 버스 자원으로 달려 있고, 개수도 종류도 플랫폼마다
	 * 다르다. 그래서 "정확히 맞는 것 -> 조금 느슨한 것" 순으로 세 번 찾는다. */
	if (pci_is_root_bus(bus)) {
		/* [한국어] 1순위: 요청한 종류와 완전히 같은 창(예: 64비트
		 * prefetchable 메모리 aperture). */
		win = find_bus_resource_of_type(bus, type, type);
		if (win)
			/* [한국어] 딱 맞는 것을 찾았으면 끝. */
			return win;

		/* [한국어] 2순위: 64비트 요구를 포기한다. 32비트 창이라도 주소가
		 * 4GB 아래면 64비트 자원을 담을 수 있으므로 안전한 완화다. */
		type &= ~IORESOURCE_MEM_64;
		win = find_bus_resource_of_type(bus, type, type);
		if (win)
			/* [한국어] 32비트 판으로 찾았으면 그것을 쓴다. */
			return win;

		/* [한국어] 3순위: prefetchable 요구까지 포기한다. prefetchable
		 * 자원을 non-prefetchable 창에 넣는 것은 성능상 손해일 뿐
		 * 동작에는 문제가 없다(반대 방향은 안 된다 — 부작용 있는
		 * 레지스터를 브리지가 미리 읽어 버리면 위험하다). */
		type &= ~IORESOURCE_PREFETCH;
		/* [한국어] 마지막 시도 결과를 그대로 반환. 여기서도 못 찾으면 NULL. */
		return find_bus_resource_of_type(bus, type, type);
	}

	/* [한국어] 여기부터는 일반 버스 — 위쪽에 PCI-to-PCI 브리지가 있고,
	 * 창은 정확히 세 개(IO / MEM / PREF MEM)뿐이라 인덱스로 바로 집는다. */
	switch (iores_type) {
	case IORESOURCE_IO:
		/* [한국어] 브리지의 I/O 창 슬롯을 꺼낸다. PCI_BUS_BRIDGE_IO_WINDOW
		 * 는 버스 자원 배열에서 I/O 창이 놓인 고정 인덱스다. */
		win = pci_bus_resource_n(bus, PCI_BUS_BRIDGE_IO_WINDOW);
		/* [한국어] 슬롯이 있고 실제로 I/O 창으로 유효한지 확인한다.
		 * 브리지가 I/O 창을 지원하지 않으면 플래그가 비어 있다
		 * (pci_bridge_check_ranges() 가 지원 여부에 따라 플래그를 켠다). */
		if (win && (win->flags & IORESOURCE_IO))
			return win;
		/* [한국어] I/O 창이 없는 브리지 아래에는 I/O 자원을 놓을 수 없다. */
		return NULL;

	case IORESOURCE_MEM:
		/* [한국어] non-prefetchable 메모리 창 슬롯. */
		mmio = pci_bus_resource_n(bus, PCI_BUS_BRIDGE_MEM_WINDOW);
		/* [한국어] prefetchable 메모리 창 슬롯(있을 수도, 없을 수도 있다). */
		mmio_pref = pci_bus_resource_n(bus, PCI_BUS_BRIDGE_PREF_MEM_WINDOW);

		/* [한국어] 슬롯은 있어도 메모리 창으로 유효하지 않으면(플래그에
		 * IORESOURCE_MEM 이 없으면) 없는 것으로 취급한다. */
		if (mmio && !(mmio->flags & IORESOURCE_MEM))
			mmio = NULL;
		/* [한국어] prefetchable 쪽도 같은 유효성 검사. 브리지가
		 * prefetchable 창을 지원하지 않으면 여기서 걸러진다. */
		if (mmio_pref && !(mmio_pref->flags & IORESOURCE_MEM))
			mmio_pref = NULL;

		/* [한국어] 규칙 1과 2: 자원이 prefetchable 이 아니거나, 브리지에
		 * prefetchable 창이 아예 없으면 무조건 non-prefetchable 창으로
		 * 보낸다. mmio 가 NULL 이면 그대로 NULL 이 반환된다. */
		if (!(type & IORESOURCE_PREFETCH) || !mmio_pref)
			return mmio;

		/* [한국어] 여기까지 왔다는 것은 "자원은 prefetchable 이고 브리지에
		 * prefetchable 창도 있다"는 뜻이다. 남은 판단은 주소 폭 궁합이다.
		 * - 자원이 64비트면 창이 32비트든 64비트든 담을 수 있다.
		 * - 창이 32비트면 32비트 자원도 당연히 담을 수 있다.
		 * 두 경우 모두 prefetchable 창으로 보낸다. */
		if ((type & IORESOURCE_MEM_64) ||
		    !(mmio_pref->flags & IORESOURCE_MEM_64))
			return mmio_pref;

		/* [한국어] 남은 경우는 "자원은 32비트인데 prefetchable 창은
		 * 64비트"뿐이다. 64비트 창은 4GB 위쪽에 놓일 수 있고, 그러면
		 * 32비트 BAR 로는 그 주소를 표현할 수 없다. 그래서 이 자원은
		 * non-prefetchable 창으로 보낸다(원문 kernel-doc 의 세 번째 규칙). */
		return mmio;
	default:
		/* [한국어] 위 조기 반환 덕에 도달할 수 없는 분기지만, switch 를
		 * 빠짐없이 닫아 두어 컴파일러 경고를 막는다. */
		return NULL;
	}
}

/**
 * pbus_select_window - Select bridge window for a resource
 * @bus: PCI bus
 * @res: Resource
 *
 * Select the bridge window for @res. If the resource is already assigned,
 * return the current bridge window.
 *
 * For memory resources, the selection is done as follows:
 *
 * Any non-prefetchable resource is put into the non-prefetchable window.
 *
 * If there is no prefetchable MMIO window, put all memory resources into the
 * non-prefetchable window.
 *
 * If there's a 64-bit prefetchable MMIO window, put all 64-bit prefetchable
 * resources into it and place 32-bit prefetchable memory into the
 * non-prefetchable window.
 *
 * Otherwise, put all prefetchable resources into the prefetchable window.
 *
 * Return: the bridge window resource or NULL if no bridge window is found.
 */
/* [한국어] 아래 함수의 한국어 해설 (원문 kernel-doc 은 그 아래 그대로 둔다):
 * pbus_select_window - 자원 하나가 담길(또는 이미 담긴) 브리지 창을 돌려준다
 *
 * @bus: 자원이 놓인 버스.
 * @res: 대상 자원. const 인 이유는 이 함수가 자원을 읽기만 하기 때문이다.
 * @return: 담길 창 자원. NULL 이면 담을 창이 없다.
 *
 * 왜 필요한가: 위의 pbus_select_window_for_type() 은 "종류만 보고" 창을
 * 고르므로, 이미 배정이 끝난 자원에까지 그 규칙을 적용하면 실제로 들어가
 * 있는 창과 다른 답이 나올 수 있다(예: 규칙이 바뀌었거나, 펌웨어가 규칙과
 * 다르게 배치해 둔 경우). 그래서 이 함수는 먼저 "이미 배정됐는가"를 보고,
 * 배정됐으면 자원 트리의 실제 부모를 그대로 돌려준다. 즉 추측이 아니라
 * 사실을 답하는 것이다.
 * 배정된 자원의 부모가 곧 그 자원을 감싸는 창이라는 점이 요점이다 —
 * kernel/resource.c 의 자원 트리에서 자식은 부모 범위 안에 완전히 들어간다.
 * 실행 컨텍스트: 순수 조회. 락 없음.
 *
 * 호출 체인:
 *   pbus_size_mem() / remove_dev_resources() /
 *   pbus_reassign_bridge_resources() / pci_do_resource_release_and_resize()
 *     -> [이 함수] -> pbus_select_window_for_type()
 */
struct resource *pbus_select_window(struct pci_bus *bus,
				    const struct resource *res)
{
	/* [한국어] 이미 배정된 자원이면 규칙을 다시 적용하지 않는다. */
	if (resource_assigned(res))
		/* [한국어] 자원 트리에서의 부모가 곧 이 자원을 감싸고 있는
		 * 브리지 창이다. 추측 대신 실제 배치를 답한다. */
		return res->parent;

	/* [한국어] 아직 배정 전이면 종류 기반 규칙으로 후보 창을 고른다.
	 * res->flags 를 손대지 않고 그대로 넘겨도 되도록 피호출 함수가
	 * 내부에서 필요한 비트만 걸러 쓴다. */
	return pbus_select_window_for_type(bus, res->flags);
}

/*
 * [한국어]
 * pdev_resources_assignable - 이 장치의 자원을 커널이 재배치해도 되는가
 *
 * @dev: 판정 대상 장치.
 * @return: true 면 이 장치의 BAR 들을 커널이 마음대로 옮겨도 된다.
 *          false 면 통째로 건드리지 말아야 한다.
 *
 * 왜 필요한가: 어떤 장치는 이미 다른 주체가 주소를 정해 놓고 그 주소에
 * 의존해 동작 중이라, 커널이 옮기면 그 주체가 깨진다. 이 함수는 그런
 * 예외 장치를 걸러 내는 문지기다. 세 부류를 막는다:
 *   1) class 코드가 정의되지 않은 장치 — 무엇인지 모르니 손대지 않는다.
 *   2) 호스트 브리지 — 자기가 주소 공간을 제공하는 쪽이라 재배치 대상이
 *      아니다. 호스트 브리지의 BAR 를 옮기면 그 아래 전부가 무너진다.
 *   3) 펌웨어가 이미 켜 둔 IOAPIC — 인터럽트 컨트롤러는 부팅 초기부터
 *      고정 주소로 접근되고 있어, 주소를 옮기면 인터럽트가 유실된다.
 *      "이미 켜졌는가"는 config 공간의 Command 레지스터에서 I/O 또는
 *      메모리 디코딩 비트가 서 있는지로 판정한다.
 * 실행 컨텍스트: 프로세스 문맥. config 공간을 읽으므로(느린 접근) 인터럽트
 *   문맥 불가. 락은 잡지 않는다.
 *
 * 호출 체인:
 *   pdev_sort_resources() / pbus_size_mem() -> [이 함수] -> pci_read_config_word()
 */
static bool pdev_resources_assignable(struct pci_dev *dev)
{
	/* [한국어] dev->class 는 24비트 class code(base class, sub class,
	 * programming interface)를 담고 있다. 8비트 오른쪽으로 밀면 하위
	 * programming interface 바이트가 떨어져 나가고 "base class + sub class"
	 * 16비트가 남는다 — PCI_CLASS_* 상수가 바로 그 16비트 값이다.
	 * command 는 아래에서 Command 레지스터를 읽어 담을 자리. */
	u16 class = dev->class >> 8, command;

	/* Don't touch classless devices or host bridges or IOAPICs */
	/* [한국어] 위 1)과 2) 부류를 한 번에 거른다. 정체를 모르는 장치와
	 * 주소 공간의 공급자인 호스트 브리지는 재배치 대상이 아니다. */
	if (class == PCI_CLASS_NOT_DEFINED || class == PCI_CLASS_BRIDGE_HOST)
		return false;

	/* Don't touch IOAPIC devices already enabled by firmware */
	/* [한국어] 3) 부류. 인터럽트 컨트롤러 계열(SYSTEM_PIC)만 추가 검사한다. */
	if (class == PCI_CLASS_SYSTEM_PIC) {
		/* [한국어] config 공간의 Command 레지스터를 읽는다. 이 레지스터는
		 * 표준 헤더의 고정 위치에 있으며, 장치가 I/O 공간과 메모리
		 * 공간의 요청에 응답할지를 켜고 끄는 비트를 담고 있다. */
		pci_read_config_word(dev, PCI_COMMAND, &command);
		/* [한국어] 두 디코딩 비트 중 하나라도 서 있으면 펌웨어가 이미
		 * 이 장치를 활성화해 쓰고 있다는 뜻이다. 지금 주소를 옮기면
		 * 사용 중인 접근이 엉뚱한 곳으로 간다. */
		if (command & (PCI_COMMAND_IO | PCI_COMMAND_MEMORY))
			return false;
	}

	/* [한국어] 위 어느 예외에도 걸리지 않았으므로 재배치해도 된다. */
	return true;
}

/*
 * [한국어]
 * pdev_resource_assignable - 이 자원 "하나"가 배치 대상이 될 수 있는가
 *
 * @dev: 자원의 소유 장치.
 * @res: 판정할 자원(dev->resource[] 의 원소).
 * @return: true 면 배치 후보, false 면 제외.
 *
 * 왜 필요한가: 앞의 pdev_resources_assignable() 이 "장치 단위" 문지기라면
 * 이 함수는 "자원 단위" 문지기다. 같은 장치 안에서도 BAR 마다 사정이 다르다.
 * 두 가지를 거른다:
 *   1) flags 가 0 인 자원 — 그 BAR 는 하드웨어가 구현하지 않았거나
 *      (BAR 크기 탐색 결과가 0), 앞선 검사에서 무효화된 슬롯이다.
 *      resource[] 배열은 고정 크기라 안 쓰는 칸이 늘 존재한다.
 *   2) IORESOURCE_DISABLED 가 켜진 브리지 창 — pbus_size_io()/pbus_size_mem()
 *      이 "이 창 아래에는 담을 것이 하나도 없다"고 판단해 꺼 둔 창이다.
 *      크기 0 짜리 창에 주소를 잡아 줄 이유가 없다. 이 검사를 브리지 창에만
 *      한정하는 이유는, DISABLED 비트를 이 파일이 브리지 창에만 붙이기
 *      때문이다(reset_resource() 와 pbus_size_* 참조).
 * 실행 컨텍스트: 순수 판정. 락 없음.
 *
 * 호출 체인:
 *   pdev_resource_should_fit() / pbus_size_io() -> [이 함수]
 *     -> pci_resource_num(), pci_resource_is_bridge_win()
 */
static bool pdev_resource_assignable(struct pci_dev *dev, struct resource *res)
{
	/* [한국어] resource[] 안에서의 인덱스를 역산한다. 아래에서 "이 칸이
	 * 브리지 창 구획인가"를 판정하려면 번호가 있어야 한다. */
	int idx = pci_resource_num(dev, res);

	/* [한국어] 플래그가 하나도 없으면 쓰이지 않는 빈 슬롯이다. 종류조차
	 * 없으니 어느 창에 넣을지도 정할 수 없다. */
	if (!res->flags)
		return false;

	/* [한국어] 브리지 창이면서 DISABLED 표시가 있는 경우만 제외한다.
	 * 연산자 우선순위상 & 가 && 보다 먼저 계산되므로 괄호 없이도
	 * (res->flags & IORESOURCE_DISABLED) 로 묶인다. */
	if (pci_resource_is_bridge_win(idx) && res->flags & IORESOURCE_DISABLED)
		return false;

	/* [한국어] 두 예외에 걸리지 않았으니 배치 후보로 인정한다. */
	return true;
}

/*
 * [한국어]
 * pdev_resource_should_fit - 이 자원을 이번 배치 계산에 넣어야 하는가
 *
 * @dev: 자원의 소유 장치.
 * @res: 판정할 자원.
 * @return: true 면 크기 합산과 주소 배치의 대상, false 면 제외.
 *
 * 왜 필요한가: pdev_resource_assignable() 이 "원리적으로 배치 가능한가"라면
 * 이 함수는 "지금 이번 라운드에서 자리를 잡아 줘야 하는가"를 묻는다.
 * 앞의 판정에 두 조건을 더 얹는다:
 *   1) 이미 배정된 자원 — 주소가 정해져 부모 창에 편입돼 있다. 다시 자리를
 *      찾아 줄 필요가 없다(그리고 옮기면 안 된다).
 *   2) IORESOURCE_PCI_FIXED — 주소를 옮길 수 없는 자원이다. 하드웨어나
 *      펌웨어 사정으로 특정 주소에 고정된 것이라 배치 알고리즘의 대상이
 *      아니다. 대신 pdev_assign_fixed_resources() 가 "그 고정 주소를 품는
 *      부모 창을 찾아 등록만" 해 주는 별도 경로를 탄다.
 * 실행 컨텍스트: 순수 판정. 락 없음.
 *
 * 호출 체인:
 *   pdev_sort_resources() / pbus_size_mem() -> [이 함수]
 *     -> pdev_resource_assignable()
 */
static bool pdev_resource_should_fit(struct pci_dev *dev, struct resource *res)
{
	/* [한국어] 이미 부모 창에 편입된 자원은 이번 배치의 관심 밖이다.
	 * (펌웨어가 잡아 둔 것을 claim 한 경우가 대표적이다.) */
	if (resource_assigned(res))
		return false;

	/* [한국어] 옮길 수 없게 못 박힌 자원. 크기 합산에는 넣지 않는다 —
	 * 창 크기를 키워 봐야 이 자원이 그 안으로 들어오지는 않기 때문이다. */
	if (res->flags & IORESOURCE_PCI_FIXED)
		return false;

	/* [한국어] 남은 판단은 앞 함수에 위임한다(빈 슬롯 / 꺼진 브리지 창 제외). */
	return pdev_resource_assignable(dev, res);
}

/*
 * [한국어]
 * pdev_sort_resources - 한 장치의 배치 대상 자원을 "정렬 내림차순"으로 리스트에 꿴다
 *
 * @dev: 자원을 훑을 장치.
 * @head: 결과를 쌓을 리스트. 여러 장치를 연달아 넣을 수 있고
 *        (pbus_assign_resources_sorted 가 버스 위 모든 장치를 한 리스트에
 *        모은다), 그때도 전체가 하나의 정렬 내림차순 순서를 유지한다.
 * @return: 없음.
 *
 * 왜 정렬 내림차순인가 — 이 파일 전체 알고리즘의 핵심 전제다.
 * PCI BAR 는 자기 크기만큼 자연 정렬되어야 한다(1MB BAR 는 1MB 경계에만).
 * 작은 것부터 놓으면 남은 공간이 총량으로는 충분해도 큰 정렬 경계가 안 남아
 * 큰 BAR 를 못 넣는다. 예: 4MB 창의 앞에 1MB 를 놓으면 남은 3MB 안에는
 * 4MB 경계가 없다. 반대로 큰 것부터 놓으면 자투리는 정렬 요구가 작은 자원이
 * 그대로 채울 수 있다. 그래서 정렬이 큰 것을 앞에 둔다.
 *
 * 동작 단계:
 *   1) 장치 단위 문지기를 먼저 통과시킨다(호스트 브리지, IOAPIC 등 제외).
 *   2) resource[] 를 훑으며 배치 대상만 고른다.
 *   3) 각 자원의 정렬 요구를 구하고, 0 이면 계산이 성립하지 않으므로 경고 후
 *      건너뛴다.
 *   4) 래퍼를 할당하고 원래 값을 사본으로 떠 둔다.
 *   5) 리스트를 앞에서부터 훑어 "나보다 정렬이 작은 첫 원소" 앞에 끼워 넣는다
 *      = 삽입 정렬. 자원 개수가 장치당 최대 십수 개라 O(n^2) 이 문제되지 않는다.
 * 실행 컨텍스트: 프로세스 문맥(kzalloc). 락 없음.
 * 에러 경로: 할당 실패 시 panic 한다. 자원 배치는 부팅 초기의 필수 단계라
 *   여기서 조용히 실패하면 시스템이 알 수 없는 상태로 진행되기 때문이다.
 *
 * 호출 체인:
 *   pdev_assign_resources_sorted() / pbus_assign_resources_sorted()
 *     -> [이 함수] -> pci_resource_alignment(), kzalloc_obj(), list_add_tail()
 */
/* Sort resources by alignment */
static void pdev_sort_resources(struct pci_dev *dev, struct list_head *head)
{
	/* [한국어] r 은 순회 중인 자원, i 는 그 자원의 resource[] 인덱스.
	 * 아래 pci_dev_for_each_resource() 매크로가 둘을 함께 갱신한다. */
	struct resource *r;
	int i;

	/* [한국어] 장치 단위 문지기. 여기서 걸리면 이 장치의 자원은 하나도
	 * 리스트에 넣지 않는다(호스트 브리지, 정체불명 장치, 활성 IOAPIC). */
	if (!pdev_resources_assignable(dev))
		return;

	/* [한국어] 장치의 resource[] 배열 전체를 순회한다. BAR 0~5, ROM,
	 * VF BAR, 브리지 창이 모두 이 한 배열에 구획을 나눠 들어 있다. */
	pci_dev_for_each_resource(dev, r, i) {
		/* [한국어] 경고 메시지에 쓸 사람이 읽는 이름("BAR 0" 등). */
		const char *r_name = pci_resource_name(dev, i);
		/* [한국어] dev_res 는 삽입 위치를 찾는 순회 커서, tmp 는 새로
		 * 만들 래퍼. tmp 는 위 함수의 지역 변수와 이름만 같을 뿐 별개다. */
		struct pci_dev_resource *dev_res, *tmp;
		/* [한국어] 이 자원이 요구하는 정렬(= 삽입 정렬의 키). */
		resource_size_t r_align;
		/* [한국어] 새 원소를 이 노드 "앞"에 끼워 넣겠다는 삽입 지점. */
		struct list_head *n;

		/* [한국어] 자원 단위 문지기 — 빈 슬롯, 이미 배정된 것,
		 * 고정 주소 자원, 꺼진 브리지 창을 걸러 낸다. */
		if (!pdev_resource_should_fit(dev, r))
			continue;

		/* [한국어] 정렬 요구를 얻는다. 일반 BAR 는 자기 크기와 같고,
		 * 브리지 창은 창 종류에 따른 최소 단위(1MB / 4KB 등)를 따른다. */
		r_align = pci_resource_alignment(dev, r);
		if (!r_align) {
			/* [한국어] 정렬 0 은 있을 수 없는 값이다(0 으로 정렬한다는
			 * 뜻이 없고, ALIGN 계산이 0 나눗셈처럼 무너진다). 이런
			 * 자원은 배치에서 제외하고 경고만 남긴다 — 하드웨어나
			 * quirk 의 이상 신호이므로 조용히 넘기지 않는다. */
			pci_warn(dev, "%s %pR: alignment must not be zero\n",
				 r_name, r);
			continue;
		}

		/* [한국어] 래퍼를 0 초기화로 할당. 이 파일은 여기서만 kzalloc 을
		 * 직접 부르고(다른 곳은 pci_dev_res_add_to_list 경유), 그 이유는
		 * 삽입 위치를 스스로 골라 list_add_tail 해야 하기 때문이다. */
		tmp = kzalloc_obj(*tmp);
		if (!tmp)
			/* [한국어] 부팅 초기 필수 경로라 실패를 감출 수 없다.
			 * 자원 배치가 어긋난 채 진행하면 장치 접근이 엉뚱한
			 * 주소로 가므로, 여기서 멈추는 편이 안전하다.
			 * __func__ 는 컴파일러가 넣어 주는 현재 함수 이름이다. */
			panic("%s: kzalloc() failed!\n", __func__);
		/* [한국어] 추적 대상 자원. */
		tmp->res = r;
		/* [한국어] 소유 장치 — 정렬 재계산과 로그에 필요하다. */
		tmp->dev = dev;
		/* [한국어] 아래 세 줄은 되돌리기용 스냅숏이다. 배치 중에
		 * res->start/end/flags 가 바뀌므로 지금 값을 떠 둔다. */
		tmp->start = r->start;
		/* [한국어] 끝 주소 사본. */
		tmp->end = r->end;
		/* [한국어] 플래그 사본. 여기서 add_size/min_align 은 kzalloc 의
		 * 0 초기화 덕에 0 으로 남는다 — 이 리스트(head)는 선택적 요구를
		 * 싣지 않기 때문이다. */
		tmp->flags = r->flags;

		/* Fallback is smallest one or list is empty */
		/* [한국어] 기본 삽입 지점은 head 자신이다. list_add_tail(x, head)
		 * 는 리스트의 맨 끝에 붙이는 것과 같으므로, "나보다 정렬이 작은
		 * 원소를 못 찾았다 = 내가 제일 작다"일 때 맨 뒤로 간다. */
		n = head;
		/* [한국어] 앞에서부터 훑으며 삽입 위치를 찾는다. 리스트는 이미
		 * 정렬 내림차순이므로 첫 번째로 "나보다 작은" 원소를 만나면
		 * 그 앞이 내 자리다. */
		list_for_each_entry(dev_res, head, list) {
			/* [한국어] 비교 대상의 정렬 요구. */
			resource_size_t align;

			/* [한국어] 정렬은 저장해 두지 않고 매번 다시 계산한다.
			 * 배치 도중 크기가 바뀌면 정렬도 바뀔 수 있어서, 캐시된
			 * 값을 믿는 대신 현재 상태에서 다시 묻는 것이다. */
			align = pci_resource_alignment(dev_res->dev,
							 dev_res->res);

			/* [한국어] 내 정렬이 더 크면 이 원소보다 앞에 와야 한다. */
			if (r_align > align) {
				/* [한국어] 삽입 지점을 이 원소로 잡고 탐색 종료. */
				n = &dev_res->list;
				break;
			}
		}
		/* Insert it just before n */
		/* [한국어] list_add_tail(new, n) 은 n 의 "직전"에 넣는 연산이다
		 * (n 을 리스트 머리로 볼 때의 꼬리 = n 바로 앞). 그래서 위에서
		 * 찾은 "나보다 작은 첫 원소" 앞에 정확히 들어간다. */
		list_add_tail(&tmp->list, n);
	}
}

/*
 * [한국어]
 * pci_resource_is_optional - 이 자원은 배치에 실패해도 괜찮은가
 *
 * @dev: 소유 장치.
 * @resno: resource[] 인덱스.
 * @return: true 면 "없어도 장치가 동작한다"(선택적), false 면 필수.
 *
 * 왜 필요한가: 이 파일의 재시도 전략은 "필수 자원이 실패했는가"로 갈린다.
 * 필수가 실패하면 되돌리고 다시 시도해야 하지만, 선택적 자원의 실패는
 * 그냥 포기하면 되는 정상적인 결과다. 그 경계를 정의하는 것이 이 함수다.
 * 선택적으로 보는 세 가지:
 *   1) SR-IOV VF BAR — VF 를 활성화하지 않으면 쓰이지 않는다. 물리 기능(PF)
 *      자체는 VF BAR 없이도 정상 동작한다.
 *   2) 활성화되지 않은 확장 ROM — ROM BAR 의 enable 비트가 꺼져 있으면
 *      옵션 ROM 을 실행하지 않겠다는 뜻이라 주소가 필요 없다. (부팅 후에는
 *      드라이버가 있으므로 옵션 ROM 이 없어도 무방하다.)
 *   3) 크기가 0 인 브리지 창 — 그 창 아래에 담을 자원이 하나도 없다는 뜻이다.
 *      창을 못 열어도 잃는 것이 없다.
 * 실행 컨텍스트: 순수 판정. 락 없음.
 *
 * 호출 체인:
 *   reassign_resources_sorted() / assign_requested_resources_sorted() /
 *   pci_required_resource_failed() / pbus_size_mem_optional() /
 *   drivers/pci/setup-res.c:591
 *     -> [이 함수] -> pci_resource_n(), pci_resource_is_iov(),
 *                     pci_resource_is_bridge_win()
 */
bool pci_resource_is_optional(const struct pci_dev *dev, int resno)
{
	/* [한국어] 인덱스로 resource[] 원소를 집는다. 아래에서 ROM enable
	 * 비트와 창 크기를 읽어야 하므로 포인터가 필요하다. */
	const struct resource *res = pci_resource_n(dev, resno);

	/* [한국어] 1) VF BAR 구획인가. VF 를 켜지 않으면 쓰이지 않으므로
	 * 주소를 못 받아도 PF 동작에는 지장이 없다. */
	if (pci_resource_is_iov(resno))
		return true;
	/* [한국어] 2) 확장 ROM 슬롯이면서 enable 비트가 꺼져 있는가.
	 * IORESOURCE_ROM_ENABLE 은 "이 ROM 을 실제로 노출할 것"이라는 표시라,
	 * 꺼져 있으면 주소를 잡아 줄 이유가 없다. */
	if (resno == PCI_ROM_RESOURCE && !(res->flags & IORESOURCE_ROM_ENABLE))
		return true;
	/* [한국어] 3) 브리지 창인데 크기가 0 인가. 담을 것이 없는 창이므로
	 * 열지 못해도 손해가 없다. resource_size() 가 0 이라는 것은 아직 크기
	 * 산정이 안 됐거나, 산정 결과 아래에 아무것도 없었다는 뜻이다. */
	if (pci_resource_is_bridge_win(resno) && !resource_size(res))
		return true;

	/* [한국어] 나머지는 전부 필수 — 실패하면 재시도 전략을 발동해야 한다. */
	return false;
}

/*
 * [한국어]
 * reset_resource - 배치에 최종 실패한 자원을 "없는 것"으로 정리한다
 *
 * @dev: 소유 장치.
 * @res: 정리할 자원.
 * @return: 없음.
 *
 * 왜 필요한가: 배치를 끝까지 못 한 자원을 크기만 남긴 채 두면, 이후 코드가
 * "주소는 0 인데 크기는 있는" 모순된 자원을 유효하다고 착각한다. 특히
 * 드라이버가 pci_resource_start() 로 0 을 읽어 그 주소에 접근하면 위험하다.
 * 그래서 흔적을 지워 "이 BAR 는 쓸 수 없다"는 상태로 만든다.
 *
 * 두 갈래로 나뉘는 이유:
 *   - 브리지 창은 flags 를 0 으로 밀면 안 된다. 창의 종류(IO/MEM/PREFETCH)와
 *     64비트 여부는 브리지 하드웨어의 성질이라 다음 재시도 라운드에서도
 *     그대로 필요하다. 그래서 IORESOURCE_UNSET 만 켜서 "지금은 주소가
 *     정해지지 않았다"고 표시하고 나머지는 보존한다.
 *   - 일반 BAR 는 start/end/flags 를 전부 0 으로 민다. 종류 정보는 필요하면
 *     BAR 를 다시 읽어 복원할 수 있다.
 * 실행 컨텍스트: 프로세스 문맥. 락 없음.
 *
 * 호출 체인:
 *   __assign_resources_sorted() 의 out: 정리 구간 -> [이 함수]
 *     -> pci_resource_num(), pci_resource_is_bridge_win(), pci_dbg()
 */
static void reset_resource(struct pci_dev *dev, struct resource *res)
{
	/* [한국어] resource[] 인덱스 역산 — 브리지 창인지 판정하는 데 쓴다. */
	int idx = pci_resource_num(dev, res);
	/* [한국어] 디버그 로그용 이름. */
	const char *res_name = pci_resource_name(dev, idx);

	/* [한국어] 브리지 창은 종류 플래그를 보존해야 한다(위 설명 참조). */
	if (pci_resource_is_bridge_win(idx)) {
		/* [한국어] "아직 주소가 정해지지 않음" 표시만 켠다.
		 * 다음 라운드에서 크기 산정이 다시 이 창을 채울 수 있다. */
		res->flags |= IORESOURCE_UNSET;
		return;
	}

	/* [한국어] 일반 BAR 를 지우기 전에 흔적을 남긴다. 자원 배치 실패는
	 * 나중에 "왜 이 장치가 안 뜨지"를 추적할 때 결정적인 단서가 된다. */
	pci_dbg(dev, "%s %pR: resetting resource\n", res_name, res);

	/* [한국어] 아래 세 줄로 자원을 완전히 무효화한다. 시작 주소 0. */
	res->start = 0;
	/* [한국어] 끝 주소 0 — start 와 함께 크기 0 을 뜻한다. */
	res->end = 0;
	/* [한국어] 플래그 0 — 이후 pdev_resource_assignable() 등이 이 칸을
	 * "쓰이지 않는 빈 슬롯"으로 보고 건너뛰게 된다. */
	res->flags = 0;
}

/**
 * reassign_resources_sorted() - Satisfy any additional resource requests
 *
 * @realloc_head:	Head of the list tracking requests requiring
 *			additional resources
 * @head:		Head of the list tracking requests with allocated
 *			resources
 *
 * Walk through each element of the realloc_head and try to procure additional
 * resources for the element, provided the element is in the head list.
 */
/* [한국어] 아래 함수의 한국어 해설 (원문 kernel-doc 은 그 아래 그대로 둔다):
 * reassign_resources_sorted - 여유가 남았을 때 선택적 추가 크기를 뒤늦게 반영한다
 *
 * @realloc_head: 선택적 요구 목록. 이 함수가 순회하며 통째로 비운다
 *        (성공하든 실패하든 각 원소를 리스트에서 떼고 kfree 한다).
 * @head: 이번 라운드에 실제로 배치를 시도한 자원 목록. realloc_head 의
 *        항목이라도 head 에 없으면 이 버스/장치와 무관한 요구이므로 건너뛴다.
 * @return: 없음. 실패는 로그로만 남는다 — 선택적 요구이므로 실패가 정상적인
 *        결과 중 하나다.
 *
 * 왜 필요한가: __assign_resources_sorted() 는 "필수 크기 + 선택 크기"로 먼저
 * 시도하고, 실패하면 되돌린 뒤 "필수만" 다시 배치한다. 그 뒤에도 공간이
 * 남아 있을 수 있으므로, 이 함수가 마지막으로 선택적 요구를 하나씩 다시
 * 시도한다. 이것이 "3 passes" 중 세 번째 단계에 해당하는 마무리 작업이다.
 *
 * 각 항목은 상태에 따라 세 갈래로 처리된다:
 *   (a) 아직 배정 안 됨 + 크기가 있음 + 필수 -> 앞선 시도에서 실패한 필수
 *       자원이다. 다시 해 봐야 또 실패하므로 손대지 않고 정리만 한다.
 *   (b) 아직 배정 안 됨 (그 외) -> "필수 크기 + add_size" 로 크기를 다시
 *       잡고 처음부터 배치를 시도한다.
 *   (c) 이미 배정됨 -> pci_reassign_resource() 로 자리를 유지한 채 확장하거나,
 *       정렬이 어긋나 있으면 정렬을 맞춰 다시 잡는다.
 * 실행 컨텍스트: 프로세스 문맥. 락 없음.
 *
 * 호출 체인:
 *   __assign_resources_sorted() -> [이 함수]
 *     -> pci_assign_resource() / pci_reassign_resource() (setup-res.c)
 */
static void reassign_resources_sorted(struct list_head *realloc_head,
				      struct list_head *head)
{
	/* [한국어] add_res 는 현재 처리 중인 선택적 요구 항목, tmp 는 그것을
	 * kfree 한 뒤에도 순회를 이어 가기 위한 예비 포인터. */
	struct pci_dev_resource *add_res, *tmp;
	/* [한국어] 아래에서 반복해 꺼내 쓸 소유 장치. */
	struct pci_dev *dev;
	/* [한국어] 대상 자원. */
	struct resource *res;
	/* [한국어] 로그용 자원 이름. */
	const char *res_name;
	/* [한국어] add_size 는 더 얹을 크기, align 은 그때 지켜야 할 정렬. */
	resource_size_t add_size, align;
	/* [한국어] resource[] 인덱스 — setup-res.c 의 API 들이 포인터가 아니라
	 * 번호를 받기 때문에 필요하다. */
	int idx;

	/* [한국어] 이 루프는 realloc_head 를 통째로 비운다. 아래 out: 라벨에서
	 * 매 반복마다 원소를 떼고 해제하므로 _safe 판이 필수다. */
	list_for_each_entry_safe(add_res, tmp, realloc_head, list) {
		/* [한국어] 요구 항목이 가리키는 실제 자원. */
		res = add_res->res;
		/* [한국어] 그 자원의 소유 장치. */
		dev = add_res->dev;
		/* [한국어] 배열 인덱스 역산 — 아래 API 호출과 optional 판정에 쓴다. */
		idx = pci_resource_num(dev, res);

		/* Skip this resource if not found in head list */
		/* [한국어] head 는 "이번 라운드에 배치를 시도한 자원들"이다.
		 * realloc_head 는 더 넓은 범위(다른 버스의 요구까지)를 담을 수
		 * 있으므로, 이번 대상이 아니면 건드리지 않고 리스트에 남긴다.
		 * continue 는 out: 을 건너뛰므로 이 항목은 해제되지 않고 살아남아
		 * 상위 호출자의 다음 처리에 넘어간다. */
		if (!res_to_dev_res(head, res))
			continue;

		/*
		 * Skip resource that failed the earlier assignment and is
		 * not optional as it would just fail again.
		 */
		/* [한국어] 갈래 (a). 세 조건이 모두 참이면 "앞선 시도에서 실패한
		 * 필수 자원"이다:
		 *   - 아직 배정되지 않았다(주소가 없다),
		 *   - 그런데 요구 크기는 0 이 아니다(정말로 자리가 필요했다),
		 *   - 그리고 선택적 자원이 아니다(없으면 안 된다).
		 * 필수 크기로도 실패한 자원에 add_size 를 더 얹어 다시 해 봐야
		 * 결과는 같다. 그래서 시도조차 하지 않고 out: 으로 뛰어 요구
		 * 항목만 정리한다 — 자원 자체는 미배정 상태로 남는다. */
		if (!resource_assigned(res) && resource_size(res) &&
		    !pci_resource_is_optional(dev, idx))
			goto out;

		/* [한국어] 로그용 이름 확보(여기까지 왔으면 실제로 처리할 항목이다). */
		res_name = pci_resource_name(dev, idx);
		/* [한국어] 이 자원에 더 얹고 싶은 크기. */
		add_size = add_res->add_size;
		/* [한국어] 그 크기를 반영했을 때의 최소 정렬 요구. */
		align = add_res->min_align;
		/* [한국어] 갈래 (b): 아직 주소가 없는 자원. 위 (a) 검사를 통과했으니
		 * "선택적이거나 크기가 0" 인 자원이다 — 처음부터 잡아 볼 수 있다. */
		if (!resource_assigned(res)) {
			/* [한국어] 정렬을 align 으로, 크기를 (기존 크기 + add_size)
			 * 로 다시 설정한다. resource_set_range() 의 첫 인자는
			 * "정렬(= 시작 주소가 아직 미정임을 나타내는 값)" 이고
			 * 두 번째가 크기다. 아직 주소가 없으므로 크기와 정렬만
			 * 적어 두면 setup-res.c 가 자리를 찾아 준다. */
			resource_set_range(res, align,
					   resource_size(res) + add_size);
			/* [한국어] 실제 배치를 시도한다. 성공하면 BAR 에 주소가
			 * 기록되고 자원 트리에 편입된다. */
			if (pci_assign_resource(dev, idx)) {
				/* [한국어] 실패해도 그냥 넘어간다 — 선택적
				 * 자원이라 없어도 되기 때문이다. 원문 메시지가
				 * "ignoring failure in optional allocation" 으로
				 * 그 의도를 분명히 밝힌다. 되돌리기도 하지
				 * 않는데, 아래 out: 에서 요구 항목만 정리하면
				 * 이 자원은 미배정 상태로 남아 최종적으로
				 * reset_resource() 대상이 되기 때문이다. */
				pci_dbg(dev,
					"%s %pR: ignoring failure in optional allocation\n",
					res_name, res);
			}
		/* [한국어] 갈래 (c): 이미 주소가 잡힌 자원. 두 경우에만 손댄다 —
		 * 더 얹을 크기가 있거나(add_size > 0), 현재 시작 주소가 요구
		 * 정렬에 맞지 않는 경우다. 후자는 나중에 창을 넓힐 때 문제가
		 * 되므로 지금 바로잡아 둔다. */
		} else if (add_size > 0 || !IS_ALIGNED(res->start, align)) {
			/* [한국어] 저장해 둔 원래 플래그에서 정렬 방식 비트 둘만
			 * 골라 현재 플래그에 되살린다.
			 *   IORESOURCE_STARTALIGN — 시작 주소를 정렬 값에 맞춘다
			 *     (브리지 창이 이 방식이다).
			 *   IORESOURCE_SIZEALIGN  — 크기 자체가 정렬 단위다
			 *     (일반 BAR 가 이 방식이다).
			 * 배치 과정에서 이 비트들이 지워졌을 수 있어, 재배치
			 * 루틴이 올바른 규칙을 쓰도록 복원하는 것이다. */
			res->flags |= add_res->flags &
				 (IORESOURCE_STARTALIGN|IORESOURCE_SIZEALIGN);
			/* [한국어] 이미 잡힌 자원을 add_size 만큼 키우고 align 에
			 * 맞춰 재배치한다. setup-res.c 가 필요하면 주소를 옮긴다. */
			if (pci_reassign_resource(dev, idx, add_size, align))
				/* [한국어] 실패하면 기존 배치는 그대로 유지된
				 * 채 확장만 무산된다. 선택적 요구라 진행을
				 * 막지 않고 정보성 로그만 남긴다. add_size 는
				 * resource_size_t 라 폭이 아키텍처마다 달라
				 * %llx 로 찍기 위해 unsigned long long 으로
				 * 명시적 캐스팅한다. */
				pci_info(dev, "%s %pR: failed to add optional %llx\n",
					 res_name, res,
					 (unsigned long long) add_size);
		}
/* [한국어] out: 은 위 (a) 갈래에서 goto 로 뛰어드는 지점이자, 정상 처리가
 * 끝난 뒤에도 그대로 흘러 들어오는 공통 정리 구간이다. 어느 경우든 이
 * 선택적 요구 항목의 수명은 여기서 끝난다. */
out:
		/* [한국어] 요구 목록에서 떼어 낸다. 이 루프가 realloc_head 를
		 * 비우는 주체이며, 호출자는 루프가 끝나면 리스트가 비었다고
		 * 가정한다(WARN_ON_ONCE 로 확인하는 호출부도 있다). */
		list_del(&add_res->list);
		/* [한국어] 래퍼 해제. 대상 자원 자체는 그대로 둔다. */
		kfree(add_res);
	}
}

/**
 * assign_requested_resources_sorted() - Satisfy resource requests
 *
 * @head:	Head of the list tracking requests for resources
 * @fail_head:	Head of the list tracking requests that could not be
 *		allocated
 * @optional:	Assign also optional resources
 *
 * Satisfy resource requests of each element in the list.  Add requests that
 * could not be satisfied to the failed_list.
 */
/* [한국어] 아래 함수의 한국어 해설 (원문 kernel-doc 은 그 아래 그대로 둔다):
 * assign_requested_resources_sorted - 정렬된 목록을 순서대로 실제 배치한다
 *
 * @head: 배치할 자원 목록. pdev_sort_resources() 가 정렬 내림차순으로
 *        만들어 둔 리스트여야 한다 — 이 함수는 순서를 그대로 따르며,
 *        그 순서가 곧 "큰 정렬부터 배치" 전략의 실행이다.
 * @fail_head: 실패한 자원을 모아 둘 목록. NULL 을 넘기면 실패를 기록하지
 *        않고 조용히 넘어간다(재시도할 계획이 없을 때 그렇게 쓴다).
 * @optional: true 면 선택적 자원까지 전부 시도, false 면 필수만 시도.
 *        __assign_resources_sorted() 가 1차 시도에는 true,
 *        되돌린 뒤 2차 시도에는 false 로 부른다.
 * @return: 없음. 결과는 fail_head 를 통해 전달된다.
 *
 * 왜 필요한가: 이 함수가 "배치 pass" 의 실행부다. 리스트 순서를 신뢰하고
 * 앞에서부터 pci_assign_resource() 를 부르기만 한다. 어떤 순서로 부르느냐가
 * 패킹 품질을 결정하고, 그 순서를 만드는 책임은 pdev_sort_resources() 에 있다.
 * 실행 컨텍스트: 프로세스 문맥. 락 없음. 여기서 config 공간 쓰기가 실제로
 *   일어난다(pci_assign_resource -> pci_update_resource -> BAR 기록).
 * 에러 경로: 개별 실패는 치명적이지 않다. fail_head 에 쌓아 두면 상위
 *   재시도 루프가 브리지 창을 놓고 다시 시도한다.
 *
 * 호출 체인:
 *   __assign_resources_sorted() -> [이 함수]
 *     -> pci_assign_resource() (setup-res.c) / pci_dev_res_add_to_list()
 */
static void assign_requested_resources_sorted(struct list_head *head,
					      struct list_head *fail_head,
					      bool optional)
{
	/* [한국어] 순회 커서. 여기서는 원소를 지우지 않으므로 _safe 가 아니다. */
	struct pci_dev_resource *dev_res;
	/* [한국어] 현재 항목의 대상 자원. */
	struct resource *res;
	/* [한국어] 그 자원의 소유 장치. */
	struct pci_dev *dev;
	/* [한국어] 이 자원이 선택적인지 여부(아래 필터에 쓴다). */
	bool optional_res;
	/* [한국어] resource[] 인덱스 — setup-res.c API 가 번호를 받는다. */
	int idx;

	/* [한국어] 정렬 내림차순 목록을 앞에서부터 그대로 훑는다. 이 순서가
	 * "정렬이 큰 것부터 배치"라는 핵심 전략의 실체다. */
	list_for_each_entry(dev_res, head, list) {
		/* [한국어] 대상 자원 꺼내기. */
		res = dev_res->res;
		/* [한국어] 소유 장치 꺼내기. */
		dev = dev_res->dev;
		/* [한국어] 인덱스 역산. */
		idx = pci_resource_num(dev, res);
		/* [한국어] 선택적 자원인지 미리 판정해 둔다(아래에서 쓴다). */
		optional_res = pci_resource_is_optional(dev, idx);

		/* [한국어] 크기가 0 인 자원은 잡아 줄 것이 없다. 크기 산정
		 * 단계에서 "아래에 아무것도 없다"고 결론난 브리지 창이 대표적. */
		if (!resource_size(res))
			continue;

		/* [한국어] "필수만" 모드인데 이 자원이 선택적이면 건너뛴다.
		 * 1차 시도가 실패해 되돌린 뒤의 2차 시도가 이 모드로 돌아간다 —
		 * 욕심을 버리고 꼭 필요한 것만 먼저 확보하는 단계다. */
		if (!optional && optional_res)
			continue;

		/* [한국어] 실제 배치. setup-res.c 가 부모 창 안에서 정렬을 지키는
		 * 빈 구간을 찾아 자원 트리에 편입하고 BAR 에 주소를 기록한다.
		 * 0 이 아닌 반환은 실패다. */
		if (pci_assign_resource(dev, idx)) {
			/* [한국어] 실패 보고를 원하는 호출자에게만 기록한다. */
			if (fail_head) {
				/* [한국어] 실패 목록에는 add_size / min_align 이
				 * 의미가 없어 0 을 넘긴다. 원문의
				 * "don't care" 주석이 그 뜻이다. 여기 저장되는
				 * start/end/flags 사본이 나중에
				 * pci_prepare_next_assign_round() 의 되돌리기
				 * 재료가 된다. 이 호출 자체가 -ENOMEM 으로
				 * 실패할 수도 있지만 반환값을 보지 않는데,
				 * 실패 보고 한 건을 못 남기는 것이 배치를
				 * 중단할 이유는 아니기 때문이다. */
				pci_dev_res_add_to_list(fail_head, dev, res,
							0 /* don't care */,
							0 /* don't care */);
			}
		}
	}
}

/*
 * [한국어]
 * pci_fail_res_type_mask - 실패한 자원들의 "종류"를 하나의 비트마스크로 집계한다
 *
 * @fail_head: 배치에 실패한 자원 목록.
 * @return: IORESOURCE_IO / IORESOURCE_MEM / IORESOURCE_PREFETCH 세 비트만
 *          남긴 OR 집계값. "어떤 종류의 공간이 부족했는가"를 뜻한다.
 *
 * 왜 필요한가: 실패했을 때 이미 배정에 성공한 자원까지 놓아 주고 다시
 * 시도해야 하는데, 전부 놓을 필요는 없다. I/O 공간이 부족해 실패했다면
 * 메모리 자원까지 놓는 것은 낭비다. 그래서 "실패한 종류"만 골라 그 종류의
 * 형제 자원만 놓는다. 이 함수가 그 판단의 입력을 만든다.
 *
 * prefetchable 의 비대칭성에 주의: prefetchable 자원은 prefetchable 창이
 * 없으면 non-prefetchable 창에 담길 수 있다(pbus_select_window_for_type 의
 * 규칙). 그래서 prefetchable 자원의 flags 에는 IORESOURCE_MEM 도 함께 서
 * 있고, 이 함수의 OR 집계는 자연히 MEM 비트도 켠다. 바로 아래 원문 주석이
 * 그 점을 설명하며, 결과적으로 non-prefetchable 형제까지 놓게 된다.
 * 실행 컨텍스트: 순수 집계. 락 없음.
 *
 * 호출 체인:
 *   __assign_resources_sorted() -> [이 함수] -> list_for_each_entry()
 */
static unsigned long pci_fail_res_type_mask(struct list_head *fail_head)
{
	/* [한국어] 순회 커서. */
	struct pci_dev_resource *fail_res;
	/* [한국어] 누적 비트마스크. 0 에서 시작해 OR 로 쌓는다. */
	unsigned long mask = 0;

	/* Check failed type */
	/* [한국어] 실패 목록 전체를 훑으며 저장해 둔 flags 사본을 OR 로 모은다.
	 * 사본을 쓰는 이유는 실패 이후 res->flags 가 이미 손상됐을 수 있기
	 * 때문이다(reset_resource 가 0 으로 밀기도 한다). */
	list_for_each_entry(fail_res, fail_head, list)
		mask |= fail_res->flags;

	/*
	 * One pref failed resource will set IORESOURCE_MEM, as we can
	 * allocate pref in non-pref range.  Will release all assigned
	 * non-pref sibling resources according to that bit.
	 */
	/* [한국어] OR 로 모으는 과정에서 정렬 비트나 상태 비트까지 섞여 들어
	 * 왔으므로, "공간 종류"를 나타내는 세 비트만 남기고 나머지를 버린다.
	 * 여기서 IORESOURCE_MEM_64 를 제외하는 것이 의도적이다 — 64비트 여부는
	 * "어떤 공간이 부족했는가"와 무관하고, 이 마스크를 소비하는
	 * pci_need_to_release() 도 그 비트를 보지 않는다. */
	return mask & (IORESOURCE_IO | IORESOURCE_MEM | IORESOURCE_PREFETCH);
}

/*
 * [한국어]
 * pci_need_to_release - 이미 배정된 이 자원을 놓아 주어야 하는가
 *
 * @mask: pci_fail_res_type_mask() 가 만든 "실패한 종류" 비트마스크.
 * @res: 이미 배정에 성공한 자원. 놓을지 말지를 판정한다.
 * @return: true 면 놓고 다시 배치, false 면 그대로 둔다.
 *
 * 왜 필요한가: 재시도 때 공간을 확보하려면 성공한 자원도 놓아야 하지만,
 * 실패한 종류와 무관한 자원까지 놓으면 헛수고에 더해 이미 잘 놓인 배치를
 * 망가뜨린다. 이 함수가 "관련 있는 것만" 골라 낸다.
 *
 * 종류별 판정 규칙:
 *   - I/O 자원: I/O 가 실패했을 때만 놓는다. I/O 는 완전히 별개의 주소
 *     공간이라 메모리 실패와 서로 영향을 주지 않는다.
 *   - prefetchable 자원: 두 경우에 놓는다.
 *       (1) prefetchable 공간 자체가 실패했을 때,
 *       (2) 메모리 공간이 실패했는데 이 자원이 실제로는 non-prefetchable
 *           창 안에 들어가 있을 때. 이 자원이 non-prefetchable 창의 자리를
 *           차지하고 있으니, 놓아 주면 실패한 메모리 자원이 그 자리를 쓸 수
 *           있다. "실제로 어디에 들어가 있는가"는 res->parent 로 확인한다 —
 *           배정된 자원의 부모가 곧 그것을 담고 있는 창이다.
 *   - 일반 메모리 자원: 메모리가 실패했을 때만 놓는다.
 * 순서가 중요하다: prefetchable 자원의 flags 에는 IORESOURCE_MEM 도 함께
 * 서 있으므로, prefetchable 검사를 먼저 하지 않으면 아래 MEM 분기가
 * 가로채 버려 (2) 의 세밀한 판단이 사라진다. 원문의 "Check pref at first"
 * 주석이 그 이유다.
 * 실행 컨텍스트: 순수 판정. res->parent 를 역참조하므로 호출자는 배정된
 *   자원에 대해서만 이 함수를 불러야 한다(그렇지 않으면 parent 가 NULL 이다).
 *   실제로 __assign_resources_sorted() 는 resource_assigned(res) 를 먼저
 *   확인한 뒤에만 부른다.
 *
 * 호출 체인:
 *   __assign_resources_sorted() -> [이 함수]
 */
static bool pci_need_to_release(unsigned long mask, struct resource *res)
{
	/* [한국어] I/O 자원 분기. I/O 는 메모리와 완전히 분리된 주소 공간이라
	 * 판정이 단순하다. !! 는 비트 검사 결과(0 또는 해당 비트값)를
	 * 0/1 의 bool 로 좁히는 관용 표현이다. */
	if (res->flags & IORESOURCE_IO)
		return !!(mask & IORESOURCE_IO);

	/* Check pref at first */
	/* [한국어] prefetchable 을 MEM 보다 먼저 본다. prefetchable 자원에는
	 * MEM 비트도 함께 서 있어서 순서를 바꾸면 아래 MEM 분기가 삼켜 버린다. */
	if (res->flags & IORESOURCE_PREFETCH) {
		/* [한국어] prefetchable 공간에서 실패가 났다면, prefetchable
		 * 자원은 종류가 같으므로 놓아 준다. */
		if (mask & IORESOURCE_PREFETCH)
			return true;
		/* Count pref if its parent is non-pref */
		/* [한국어] 메모리 공간이 실패했고, 이 prefetchable 자원이
		 * 실제로는 non-prefetchable 창 안에 들어가 있는 경우다. 자리를
		 * 비워 주면 실패한 메모리 자원이 그 공간을 쓸 수 있다.
		 * res->parent 는 이 자원을 담고 있는 실제 창이다(배정된 자원의
		 * 부모). 규칙이 아니라 사실을 보고 판단하는 지점이다. */
		else if ((mask & IORESOURCE_MEM) &&
			 !(res->parent->flags & IORESOURCE_PREFETCH))
			return true;
		else
			/* [한국어] prefetchable 창 안에 잘 들어가 있고
			 * prefetchable 공간은 실패하지 않았다 — 건드릴 이유가 없다. */
			return false;
	}

	/* [한국어] 남은 것은 non-prefetchable 메모리 자원이다. 메모리 공간이
	 * 실패했을 때만 놓는다. */
	if (res->flags & IORESOURCE_MEM)
		return !!(mask & IORESOURCE_MEM);

	/* [한국어] IO 도 MEM 도 아닌 자원은 애초에 이 배치 경로에 들어올 수
	 * 없다(pbus_select_window_for_type 이 걸러 낸다). 원문 주석
	 * "Should not get here" 가 그 뜻이며, 방어적으로 false 를 돌려
	 * 아무것도 놓지 않게 한다. */
	return false;	/* Should not get here */
}

/*
 * [한국어]
 * pci_required_resource_failed - 실패 목록에 "필수" 자원이 섞여 있는가
 *
 * @fail_head: 실패한 자원 목록.
 * @type: 관심 있는 자원 종류. 0 을 넘기면 종류를 가리지 않고 전부 본다.
 *        __assign_resources_sorted() 는 0 을, pbus_reassign_bridge_resources()
 *        는 리사이즈 대상 자원의 flags 를 넘겨 "그 종류만" 확인한다.
 * @return: true 면 되돌리고 재시도해야 한다. false 면 실패한 것이 전부
 *        선택적 자원이라, 그 실패를 그대로 받아들이고 진행해도 된다.
 *
 * 왜 필요한가: 실패했다고 해서 항상 재시도할 이유가 되는 것은 아니다.
 * SR-IOV VF BAR 나 비활성 ROM 처럼 없어도 되는 것들만 실패했다면, 비싼
 * 재시도(브리지 창을 놓고 전부 다시 계산)를 할 값어치가 없다. 이 함수가
 * 그 분기점을 만든다.
 * 실행 컨텍스트: 순수 판정. 락 없음.
 *
 * 호출 체인:
 *   __assign_resources_sorted() / pbus_reassign_bridge_resources()
 *     -> [이 함수] -> pci_resource_num(), pci_resource_is_optional()
 */
/* Return: @true if assignment of a required resource failed. */
static bool pci_required_resource_failed(struct list_head *fail_head,
					 unsigned long type)
{
	/* [한국어] 순회 커서. */
	struct pci_dev_resource *fail_res;

	/* [한국어] 넘어온 flags 에서 종류 비트만 남긴다. 호출자가 res->flags 를
	 * 그대로 넘겨도 되도록 여기서 정리하는 것이며, 아래 비교가 정렬/상태
	 * 비트 차이 때문에 어긋나지 않게 한다. type 이 0 이면 0 그대로 남아
	 * "종류를 가리지 않음"이 된다. */
	type &= PCI_RES_TYPE_MASK;

	/* [한국어] 실패 목록 전체를 훑는다. */
	list_for_each_entry(fail_res, fail_head, list) {
		/* [한국어] 이 실패 항목의 resource[] 인덱스. optional 판정에 필요. */
		int idx = pci_resource_num(fail_res->dev, fail_res->res);

		/* [한국어] 종류 필터. type 이 0 이 아니면(= 특정 종류만 관심)
		 * 종류가 다른 실패는 건너뛴다. 저장해 둔 flags 사본을 쓰는
		 * 이유는 실패 후 res->flags 가 이미 정리됐을 수 있어서다. */
		if (type && (fail_res->flags & PCI_RES_TYPE_MASK) != type)
			continue;

		/* [한국어] 필수 자원이 하나라도 실패했으면 즉시 참을 돌려준다.
		 * 더 볼 필요가 없다 — 재시도 여부는 이미 결정됐다. */
		if (!pci_resource_is_optional(fail_res->dev, idx))
			return true;
	}
	/* [한국어] 끝까지 훑었는데 전부 선택적이었다 = 재시도할 이유가 없다. */
	return false;
}

/*
 * [한국어]
 * __assign_resources_sorted - 배치 pass 의 심장. 3단 전략으로 자원을 배치한다
 *
 * @head: 정렬 내림차순으로 준비된 배치 대상 목록. 이 함수가 끝나면 통째로
 *        비워지고 해제된다(마지막 pci_dev_res_free_list(head)).
 * @realloc_head: 선택적 추가 크기 요구 목록(= add_list). NULL 이면 내부에서
 *        빈 더미 리스트로 대체해 "선택적 요구가 하나도 없는 경우"와 같게
 *        다룬다 — NULL 검사를 아래 곳곳에 흩뿌리지 않기 위한 기법이다.
 * @fail_head: 최종적으로 배치하지 못한 자원을 보고할 목록. NULL 이면 보고
 *        없이 조용히 진행한다.
 * @return: 없음.
 *
 * 왜 필요한가 — 3단 전략의 이유:
 * 자원을 "요구한 만큼만" 딱 맞게 잡으면 나중에 늘릴 수 없고, "넉넉하게"
 * 잡으면 공간이 모자라 실패할 수 있다. 그래서 낙관적으로 시작해 실패하면
 * 단계적으로 욕심을 줄인다:
 *   1단계 — head 의 각 자원에 realloc_head 의 add_size 를 미리 더해 놓고
 *          (필수 + 선택 전부) 한 번에 배치를 시도한다. 여기서 성공하면
 *          가장 좋은 결과다: 여유 공간까지 확보한 채로 끝난다.
 *   2단계 — 필수 자원이 하나라도 실패했으면, 성공한 것 중 "실패한 종류와
 *          관련 있는" 것들을 도로 놓고, 저장해 둔 원래 크기로 전부 되돌린
 *          뒤 "필수만" 다시 배치한다.
 *   3단계 — 그러고도 공간이 남으면 reassign_resources_sorted() 로 선택적
 *          요구를 하나씩 다시 얹어 본다.
 * 왜 요구분을 먼저 잡아 두면 안 되는가는 아래 원문 주석이 설명한다:
 * 먼저 잡아 둔 자원들이 서로 인접해 버리면, 나중에 하나를 키우려 해도
 * 옆이 막혀 부모 창 안에서 개별 재배치가 불가능해진다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 락 없음(상위에서 pci_bus_sem 을 잡는 경로가
 *   있다). 안에서 config 공간 쓰기와 kzalloc/kfree 가 일어난다.
 * 에러 경로: 자체적으로 재시도하고, 그래도 실패한 것은 fail_head 로 올려
 *   상위 루프(pci_assign_unassigned_root_bus_resources 등)의 더 큰 재시도에
 *   맡긴다. 최종 실패 자원은 reset_resource() 로 무효화된다.
 *
 * 호출 체인:
 *   pdev_assign_resources_sorted() / pbus_assign_resources_sorted()
 *     -> [이 함수] -> assign_requested_resources_sorted(),
 *                     reassign_resources_sorted(), pci_release_resource(),
 *                     pci_dev_res_restore(), reset_resource()
 */
static void __assign_resources_sorted(struct list_head *head,
				      struct list_head *realloc_head,
				      struct list_head *fail_head)
{
	/*
	 * Should not assign requested resources at first.  They could be
	 * adjacent, so later reassign can not reallocate them one by one in
	 * parent resource window.
	 *
	 * Try to assign required and any optional resources at beginning
	 * (add_size included). If all required resources were successfully
	 * assigned, get out early. If could not do that, we still try to
	 * assign required at first, then try to reassign some optional
	 * resources.
	 *
	 * Separate three resource type checking if we need to release
	 * assigned resource after requested + add_size try.
	 *
	 *	1. If IO port assignment fails, will release assigned IO
	 *	   port.
	 *	2. If pref MMIO assignment fails, release assigned pref
	 *	   MMIO.  If assigned pref MMIO's parent is non-pref MMIO
	 *	   and non-pref MMIO assignment fails, will release that
	 *	   assigned pref MMIO.
	 *	3. If non-pref MMIO assignment fails or pref MMIO
	 *	   assignment fails, will release assigned non-pref MMIO.
	 */
	/* [한국어] 1단계 시도 전의 원래 start/end/flags 를 담아 둘 스냅숏 목록.
	 * 되돌리기의 재료다. C99 이후 선언은 문장 뒤에도 올 수 있어, 위 긴
	 * 설명 주석 다음에 선언이 이어져도 문제없다. */
	LIST_HEAD(save_head);
	/* [한국어] 이 함수 안에서만 쓰는 실패 목록. 인자로 받은 fail_head 와
	 * 구분되는 이유는, 1단계의 실패는 아직 "최종 실패"가 아니라 2단계로
	 * 넘어갈지 판단하는 중간 정보이기 때문이다. */
	LIST_HEAD(local_fail_head);
	/* [한국어] realloc_head 가 NULL 일 때 대신 쓸 빈 리스트. 항상 비어
	 * 있으므로 "선택적 요구 없음"과 동일하게 동작하고, 덕분에 아래 코드가
	 * realloc_head 의 NULL 여부를 신경 쓰지 않아도 된다. */
	LIST_HEAD(dummy_head);
	/* [한국어] save_head 순회용 커서. */
	struct pci_dev_resource *save_res;
	/* [한국어] dev_res 는 주 순회 커서, tmp_res 는 삭제하며 순회할 때의
	 * 예비 포인터, dev_res2 는 정렬 위치를 다시 찾을 때의 내부 커서,
	 * addsize_res 는 realloc_head 에서 찾아낸 추가 요구 항목이다. */
	struct pci_dev_resource *dev_res, *tmp_res, *dev_res2, *addsize_res;
	/* [한국어] 반복해 꺼내 쓸 현재 자원. */
	struct resource *res;
	/* [한국어] 반복해 꺼내 쓸 현재 장치. */
	struct pci_dev *dev;
	/* [한국어] 실패한 자원들의 종류 집계 비트마스크(2단계 판단에 쓴다). */
	unsigned long fail_type;
	/* [한국어] 정렬 값 비교용 임시 변수. */
	resource_size_t align;

	/* [한국어] 선택적 요구 목록이 없다면 빈 더미로 바꿔 둔다. 이후 코드는
	 * realloc_head 가 항상 유효한 포인터라고 가정해도 된다. */
	if (!realloc_head)
		realloc_head = &dummy_head;

	/* Check if optional add_size is there */
	/* [한국어] 선택적 요구가 하나도 없으면 1단계와 2단계를 구분할 이유가
	 * 없다. 스냅숏을 뜰 필요도, 크기를 부풀릴 필요도 없으므로 곧바로
	 * assign: 으로 뛰어 한 번만 배치한다. */
	if (list_empty(realloc_head))
		goto assign;

	/* Save original start, end, flags etc at first */
	/* [한국어] 1단계에서 크기를 부풀리기 전에 원래 값을 통째로 떠 둔다.
	 * 이 스냅숏이 없으면 실패 후 "필수만" 상태로 돌아갈 수 없다. */
	list_for_each_entry(dev_res, head, list) {
		/* [한국어] head 의 각 항목에 대응하는 사본을 save_head 에 만든다.
		 * add_size / min_align 은 스냅숏에 의미가 없어 0 을 넘긴다. */
		if (pci_dev_res_add_to_list(&save_head, dev_res->dev,
					    dev_res->res, 0, 0)) {
			/* [한국어] 메모리 부족으로 스냅숏을 다 뜨지 못했다.
			 * 반쪽짜리 스냅숏으로 되돌리면 일부만 복구돼 상태가
			 * 더 나빠지므로, 지금까지 만든 것을 전부 버린다. */
			pci_dev_res_free_list(&save_head);
			/* [한국어] 되돌리기를 포기했으니 부풀리기(1단계)도 하지
			 * 않는다. 곧바로 "있는 그대로 한 번만" 배치한다. */
			goto assign;
		}
	}

	/* Update res in head list with add_size in realloc_head list */
	/* [한국어] 1단계 준비: head 의 각 자원 크기를 "필수 + 선택"으로 부풀린다.
	 * _safe 판을 쓰는 이유는 아래에서 list_move_tail() 로 현재 원소를
	 * 다른 위치로 옮기기 때문이다 — 옮기고 나면 원소의 next 가 달라져
	 * 일반 순회로는 리스트를 다시 돌거나 건너뛰게 된다. */
	list_for_each_entry_safe(dev_res, tmp_res, head, list) {
		/* [한국어] 현재 항목의 대상 자원. */
		res = dev_res->res;

		/* [한국어] 이 자원에 걸린 선택적 요구가 있는지 realloc_head 에서
		 * 찾는다. head 와 realloc_head 는 서로 다른 리스트라 같은 자원의
		 * 래퍼가 각각 하나씩 따로 존재한다. */
		addsize_res = res_to_dev_res(realloc_head, res);
		if (!addsize_res)
			/* [한국어] 추가 요구가 없는 자원은 부풀릴 것이 없다. */
			continue;

		/* [한국어] 끝 주소를 add_size 만큼 밀어 크기를 키운다. start 는
		 * 그대로 두므로 (end - start + 1) 이 늘어난 크기가 된다.
		 * 이 부풀림이 실패하면 위 save_head 스냅숏으로 되돌린다. */
		res->end += addsize_res->add_size;
		/*
		 * There are two kinds of additional resources in the list:
		 * 1. bridge resource  -- IORESOURCE_STARTALIGN
		 * 2. SR-IOV resource  -- IORESOURCE_SIZEALIGN
		 * Here just fix the additional alignment for bridge
		 */
		/* [한국어] 정렬 보정이 필요한 것은 STARTALIGN 방식(= 시작 주소를
		 * 정렬 값에 맞추는 방식)뿐이다. 위 원문 주석이 밝히듯 그것이
		 * 브리지 창이고, SIZEALIGN 방식인 SR-IOV 자원은 크기 자체가
		 * 정렬 단위라 여기서 손댈 것이 없다. */
		if (!(res->flags & IORESOURCE_STARTALIGN))
			continue;

		/* [한국어] STARTALIGN 자원에서 res->start 필드는 실제 주소가
		 * 아니라 "요구 정렬"을 담는 자리로 쓰인다(아직 주소가 정해지지
		 * 않았기 때문이다. resource_set_range() 의 첫 인자가 그 값이다).
		 * 이미 요구 정렬이 min_align 이상이면 더 키울 이유가 없다. */
		if (addsize_res->min_align <= res->start)
			continue;
		/*
		 * The "head" list is sorted by alignment so resources with
		 * bigger alignment will be assigned first.  After we
		 * change the alignment of a dev_res in "head" list, we
		 * need to reorder the list by alignment to make it
		 * consistent.
		 */
		/* [한국어] 요구 정렬을 min_align 으로 올린다. 크기는 방금 부풀린
		 * 값을 그대로 유지한다(resource_size(res) 를 다시 넣는 이유). */
		resource_set_range(res, addsize_res->min_align,
				   resource_size(res));

		/* [한국어] 정렬이 커졌으니 이 원소가 리스트에서 더 앞으로 가야
		 * 한다. 바로 위 원문 주석이 그 이유를 설명한다 — head 는 정렬
		 * 내림차순이라는 불변식을 지켜야 하고, 그 순서가 곧 배치 순서다.
		 * 앞에서부터 훑어 "새 정렬보다 작은 첫 원소"를 찾는다. */
		list_for_each_entry(dev_res2, head, list) {
			/* [한국어] 비교 대상의 현재 정렬 요구. */
			align = pci_resource_alignment(dev_res2->dev,
						       dev_res2->res);
			/* [한국어] 내 새 정렬이 더 크면 그 앞으로 가야 한다. */
			if (addsize_res->min_align > align) {
				/* [한국어] list_move_tail(x, y) 는 x 를 원래
				 * 자리에서 떼어 y 의 직전으로 옮긴다. 바깥
				 * 루프가 _safe 판인 덕에 이 이동이 순회를
				 * 망가뜨리지 않는다. */
				list_move_tail(&dev_res->list, &dev_res2->list);
				/* [한국어] 자리를 찾았으니 내부 탐색 종료. */
				break;
			}
		}

	}

/* [한국어] assign: 은 (a) 선택적 요구가 없어서, 또는 (b) 스냅숏을 못 떠서
 * 부풀리기를 건너뛴 경우의 진입점이자, 위 부풀리기가 끝난 뒤 그대로 흘러
 * 들어오는 지점이다. 여기서부터가 실제 배치 시도다. */
assign:
	/* [한국어] 1단계 시도: 필수와 선택을 가리지 않고(optional=true) 전부
	 * 배치해 본다. 실패한 것은 local_fail_head 에 쌓인다. head 의 순서가
	 * 정렬 내림차순이므로 큰 정렬부터 자리를 잡는다. */
	assign_requested_resources_sorted(head, &local_fail_head, true);

	/* All non-optional resources assigned? */
	/* [한국어] 1단계가 완전히 성공한 경우 — 실패 목록이 비어 있다.
	 * 부풀린 크기까지 전부 자리를 잡았으니 더 할 일이 없다. */
	if (list_empty(&local_fail_head)) {
		/* Remove head list from realloc_head list */
		/* [한국어] 이번에 배치한 자원들의 선택적 요구는 이미 반영됐다.
		 * realloc_head 에 남겨 두면 상위 호출자가 "아직 못 들어준 요구"로
		 * 오해하므로(그리고 WARN_ON_ONCE 로 잡아낸다) 여기서 지운다. */
		list_for_each_entry(dev_res, head, list)
			pci_dev_res_remove_from_list(realloc_head,
						     dev_res->res);
		/* [한국어] 되돌릴 일이 없으므로 스냅숏을 버린다. */
		pci_dev_res_free_list(&save_head);
		/* [한국어] 공통 마무리 구간으로. head 정리와 실패 보고가 거기 있다. */
		goto out;
	}

	/* Without realloc_head and only optional fails, nothing more to do. */
	/* [한국어] 두 조건이 함께 참인 경우 — 실패한 것이 전부 선택적 자원이고,
	 * 게다가 realloc_head 도 비어 있다(= 나중에 다시 얹어 볼 요구도 없다).
	 * 이때는 2단계 재시도가 아무것도 개선하지 못하므로 여기서 접는다.
	 * realloc_head 가 비었다는 조건이 필요한 이유: 남은 요구가 있다면
	 * 3단계(reassign_resources_sorted)에서 처리할 여지가 있어 아래로
	 * 내려가야 한다. */
	if (!pci_required_resource_failed(&local_fail_head, 0) &&
	    list_empty(realloc_head)) {
		/* [한국어] 배치되지 못한 자원만 원래 크기로 되돌린다. */
		list_for_each_entry(save_res, &save_head, list) {
			/* [한국어] 스냅숏이 가리키는 실제 자원. 바깥의 res 와
			 * 이름이 같지만 이 블록 안에서만 유효한 별개 변수다. */
			struct resource *res = save_res->res;

			/* [한국어] 이미 배정에 성공한 자원은 건드리면 안 된다.
			 * 자원 트리에 편입된 것의 start/end 를 몰래 바꾸면
			 * 실제 배치와 어긋난다(pci_dev_res_restore 도 이 경우
			 * WARN 을 찍는다). */
			if (resource_assigned(res))
				continue;

			/* [한국어] 부풀렸던 크기를 원래대로 되돌린다. 이렇게
			 * 해야 이후 코드가 "이 자원은 원래 이만큼을 원했다"는
			 * 정확한 값을 보게 된다. */
			pci_dev_res_restore(save_res);
		}
		/* [한국어] 중간 실패 목록 해제 — 이 실패는 fail_head 로 올리지
		 * 않는다. 아래 out: 구간이 미배정 자원을 다시 훑어 보고하므로
		 * 여기서 중복해 올릴 이유가 없다. */
		pci_dev_res_free_list(&local_fail_head);
		/* [한국어] 스냅숏도 다 썼으니 해제. */
		pci_dev_res_free_list(&save_head);
		/* [한국어] 공통 마무리로. */
		goto out;
	}

	/* Check failed type */
	/* [한국어] 2단계 시작. 먼저 "어떤 종류의 공간이 부족했는가"를 집계한다.
	 * 이 마스크가 아래에서 "무엇을 놓을 것인가"의 기준이 된다. */
	fail_type = pci_fail_res_type_mask(&local_fail_head);
	/* Remove not need to be released assigned res from head list etc */
	/* [한국어] 실패 종류와 무관하게 잘 배치된 자원은 그대로 두는 편이 낫다.
	 * 그런 자원을 head 에서 아예 빼 버리면 아래의 "전부 놓기" 루프가
	 * 건드리지 않는다. _safe 판인 이유는 여기서 원소를 지우기 때문이다. */
	list_for_each_entry_safe(dev_res, tmp_res, head, list) {
		/* [한국어] 현재 항목의 자원. */
		res = dev_res->res;

		/* [한국어] 배치에 성공했고, 실패한 종류와도 무관하다면 유지 대상. */
		if (resource_assigned(res) &&
		    !pci_need_to_release(fail_type, res)) {
			/* Remove it from realloc_head list */
			/* [한국어] 이 자원은 이번 재시도의 대상이 아니므로
			 * 선택적 요구 목록에서도 뺀다 — 이미 자리를 잡았고
			 * 건드리지 않을 것이기 때문이다. */
			pci_dev_res_remove_from_list(realloc_head, res);
			/* [한국어] 되돌릴 일이 없으니 스냅숏에서도 뺀다.
			 * 남겨 두면 아래 일괄 복구 루프가 배치된 자원을
			 * 되돌리려 해 WARN 이 뜬다. */
			pci_dev_res_remove_from_list(&save_head, res);
			/* [한국어] head 에서도 떼어 내고 */
			list_del(&dev_res->list);
			/* [한국어] 래퍼를 해제한다. 이후 head 에는 "놓았다가
			 * 다시 배치할 자원"만 남는다. */
			kfree(dev_res);
		}
	}

	/* [한국어] 1단계의 실패 목록은 역할을 다했다(종류 집계에 썼다). 해제. */
	pci_dev_res_free_list(&local_fail_head);
	/* Release assigned resource */
	/* [한국어] head 에 남은 자원 중 배치에 성공했던 것들을 실제로 놓아 준다.
	 * 이것이 2단계의 핵심 — 공간을 비워야 실패한 필수 자원이 들어갈 수 있다. */
	list_for_each_entry(dev_res, head, list) {
		/* [한국어] 현재 자원. */
		res = dev_res->res;
		/* [한국어] 소유 장치. */
		dev = dev_res->dev;

		/* [한국어] 자원 트리에서 떼어 내고 BAR 도 무효화한다. 미배정
		 * 자원에 대해 불러도 안전하도록 setup-res.c 쪽이 처리한다. */
		pci_release_resource(dev, pci_resource_num(dev, res));
		/* [한국어] 놓은 직후에 원래 크기로 되돌린다. head 원소가 들고
		 * 있는 사본은 pdev_sort_resources() 시점의 값이다. */
		pci_dev_res_restore(dev_res);
	}
	/* Restore start/end/flags from saved list */
	/* [한국어] 그리고 save_head 스냅숏으로 한 번 더 되돌린다. 이 스냅숏은
	 * 부풀리기 직전의 값이라, 부풀린 크기와 올려 둔 정렬이 여기서 완전히
	 * 원상 복구된다. head 쪽 복구와 겹치는 것처럼 보이지만, head 에서
	 * 위 루프가 빼 버린 항목들이 있어 두 목록의 내용이 같지 않다. */
	list_for_each_entry(save_res, &save_head, list)
		pci_dev_res_restore(save_res);
	/* [한국어] 스냅숏은 역할을 다했다. 해제. */
	pci_dev_res_free_list(&save_head);

	/* Satisfy the must-have resource requests */
	/* [한국어] 2단계 재배치: optional=false 로 "필수만" 잡는다. 욕심을 버린
	 * 최소 요구이므로 성공 확률이 가장 높다. fail_head 로 NULL 을 넘겨
	 * 여기서는 실패를 기록하지 않는데, 아래 out: 구간이 미배정 자원을 다시
	 * 훑어 인자로 받은 fail_head 에 보고하기 때문이다(중복 방지). */
	assign_requested_resources_sorted(head, NULL, false);

	/* Try to satisfy any additional optional resource requests */
	/* [한국어] 3단계: 필수를 다 잡고도 공간이 남았다면 선택적 요구를 하나씩
	 * 다시 얹어 본다. 이 호출이 realloc_head 를 비운다. */
	if (!list_empty(realloc_head))
		reassign_resources_sorted(realloc_head, head);

/* [한국어] out: 은 모든 경로가 모이는 공통 마무리 구간이다. 여기서 두 가지를
 * 한다 — (1) 끝내 배치하지 못한 자원을 호출자에게 보고하고, (2) 그 자원의
 * 흔적을 지운다. 그리고 head 를 통째로 해제한다. */
out:
	/* Reset any failed resource, cannot use fail_head as it can be NULL. */
	/* [한국어] 원문 주석이 밝히듯, 실패 목록을 훑는 대신 head 를 훑는다.
	 * fail_head 는 NULL 일 수 있고, 위 여러 경로에서 local_fail_head 를
	 * 이미 해제했기 때문에 "지금 시점에 배정되지 않은 자원"을 직접 다시
	 * 찾는 것이 유일하게 정확한 방법이다. */
	list_for_each_entry(dev_res, head, list) {
		/* [한국어] 현재 자원. */
		res = dev_res->res;
		/* [한국어] 소유 장치. */
		dev = dev_res->dev;

		/* [한국어] 배정에 성공한 자원은 그대로 둔다 — 정상 결과다. */
		if (resource_assigned(res))
			continue;

		/* [한국어] 실패 보고를 원하는 호출자에게만 기록한다. 이 목록이
		 * 상위 재시도 루프(pci_prepare_next_assign_round)의 입력이 된다. */
		if (fail_head) {
			/* [한국어] 실패 목록에는 add_size / min_align 이
			 * 의미 없어 0("don't care")을 넘긴다. 여기 저장되는
			 * flags 사본이 상위 루프에서 "어느 브리지 창을 놓을지"
			 * 고르는 근거가 된다. */
			pci_dev_res_add_to_list(fail_head, dev, res,
						0 /* don't care */,
						0 /* don't care */);
		}

		/* [한국어] 보고와 별개로 자원 자체를 무효화한다. 주소가 없는데
		 * 크기만 남아 있으면 이후 코드가 유효하다고 착각한다. */
		reset_resource(dev, res);
	}

	/* [한국어] 배치 목록의 수명은 여기서 끝난다. 호출자는 이 함수가
	 * head 를 비워 준다고 가정하고 스택의 LIST_HEAD 를 그냥 버린다. */
	pci_dev_res_free_list(head);
}

/*
 * [한국어]
 * pdev_assign_resources_sorted - 장치 "하나"의 자원을 정렬해 배치한다
 *
 * @dev: 대상 장치. 브리지 자신의 창을 배치할 때도 이 함수를 쓴다
 *       (__pci_bridge_assign_resources 가 브리지를 넘긴다).
 * @add_head: 선택적 요구 목록(= add_list). NULL 가능.
 * @fail_head: 실패 보고 목록. NULL 가능.
 * @return: 없음.
 *
 * 왜 필요한가: 배치의 두 단위 중 "장치 단위" 진입점이다. 정렬 리스트를
 * 만들고 배치 엔진에 넘기는 두 줄짜리 조립 함수지만, 리스트의 수명(스택의
 * LIST_HEAD 로 잡았다가 __assign_resources_sorted 가 비워 준다)을 여기서
 * 닫아 주는 역할을 한다.
 * 실행 컨텍스트: 프로세스 문맥. 락 없음.
 *
 * 호출 체인:
 *   __pci_bridge_assign_resources() -> [이 함수]
 *     -> pdev_sort_resources() -> __assign_resources_sorted()
 */
static void pdev_assign_resources_sorted(struct pci_dev *dev,
					 struct list_head *add_head,
					 struct list_head *fail_head)
{
	/* [한국어] 이 호출에서만 쓰는 배치 목록. 스택에 잡고,
	 * __assign_resources_sorted() 가 끝내면서 원소를 전부 해제해 준다. */
	LIST_HEAD(head);

	/* [한국어] 이 장치의 배치 대상 자원을 정렬 내림차순으로 리스트에 꿴다. */
	pdev_sort_resources(dev, &head);
	/* [한국어] 배치 엔진에 넘긴다. 이 호출이 끝나면 head 는 비어 있다. */
	__assign_resources_sorted(&head, add_head, fail_head);

}

/*
 * [한국어]
 * pbus_assign_resources_sorted - 버스 위 "모든 장치"의 자원을 한 리스트로 모아 배치한다
 *
 * @bus: 대상 버스. const 인 이유는 버스 구조체 자체를 고치지 않기 때문이다
 *       (자원은 장치 쪽에 있다).
 * @realloc_head: 선택적 요구 목록. NULL 가능.
 * @fail_head: 실패 보고 목록. NULL 가능.
 * @return: 없음.
 *
 * 왜 장치별로 따로 하지 않고 한 리스트에 모으는가 — 이 점이 중요하다.
 * 같은 버스의 장치들은 같은 브리지 창을 나눠 쓴다. 장치별로 따로 배치하면
 * A 장치의 작은 BAR 가 먼저 자리를 잡아 B 장치의 큰 BAR 가 들어갈 정렬된
 * 빈칸을 없앨 수 있다. 버스 전체를 한 리스트로 모아 정렬 내림차순으로
 * 배치해야 "큰 정렬 먼저" 원칙이 버스 전체에 적용된다.
 * 실행 컨텍스트: 프로세스 문맥. 락 없음. bus->devices 순회의 안전성은
 *   상위(pci_bus_sem 등)가 보장한다.
 *
 * 호출 체인:
 *   __pci_bus_assign_resources() -> [이 함수]
 *     -> pdev_sort_resources() (장치마다) -> __assign_resources_sorted()
 */
static void pbus_assign_resources_sorted(const struct pci_bus *bus,
					 struct list_head *realloc_head,
					 struct list_head *fail_head)
{
	/* [한국어] 버스에 달린 장치를 훑을 커서. */
	struct pci_dev *dev;
	/* [한국어] 이 버스 전체의 배치 목록. 아래 루프가 여러 장치의 자원을
	 * 여기 한곳에 모으고, 그 과정에서 정렬 내림차순이 유지된다. */
	LIST_HEAD(head);

	/* [한국어] 이 버스에 직접 달린 장치를 하나씩 훑는다(하위 버스는 이
	 * 함수의 관심 밖이며, 호출자가 재귀로 처리한다). */
	list_for_each_entry(dev, &bus->devices, bus_list)
		/* [한국어] 같은 head 에 계속 삽입한다. pdev_sort_resources() 가
		 * 삽입 정렬이므로 여러 장치를 넣어도 전체 순서가 유지된다. */
		pdev_sort_resources(dev, &head);

	/* [한국어] 버스 전체 목록을 한 번에 배치 엔진에 넘긴다. */
	__assign_resources_sorted(&head, realloc_head, fail_head);
}

/*
 * Initialize bridges with base/limit values we have collected.  PCI-to-PCI
 * Bridge Architecture Specification rev. 1.1 (1998) requires that if there
 * are no I/O ports or memory behind the bridge, the corresponding range
 * must be turned off by writing base value greater than limit to the
 * bridge's base/limit registers.
 *
 * Note: care must be taken when updating I/O base/limit registers of
 * bridges which support 32-bit I/O.  This update requires two config space
 * writes, so it's quite possible that an I/O window of the bridge will
 * have some undesirable address (e.g. 0) after the first write.  Ditto
 * 64-bit prefetchable MMIO.
 */
/*
 * [한국어]
 * pci_setup_bridge_io - 확정된 I/O 창 주소를 브리지 config 레지스터에 기록한다
 *
 * @bridge: 창을 프로그램할 PCI-to-PCI 브리지 장치.
 * @return: 없음.
 *
 * 왜 필요한가: 여기까지의 모든 계산은 커널 메모리 안의 struct resource 를
 * 고쳐 온 것뿐이다. 실제로 브리지가 그 주소 범위의 트랜잭션을 아래로
 * 통과시키게 하려면 Type 1 config 헤더의 base/limit 레지스터에 값을 써야
 * 한다. 이 함수가 그 마지막 한 걸음이다.
 *
 * 브리지 I/O 창의 인코딩 (아래 비트 연산의 근거):
 * I/O base/limit 레지스터는 주소의 상위 비트만 담는다. 하위 비트는 창의
 * 세분성(granularity)에 해당해 하드웨어가 암묵적으로 채운다. 코드가
 * 주소를 8비트 오른쪽으로 밀어 넣는 것이 그 표현이다 — 즉 창의 시작과 끝은
 * 256바이트 단위로만 표현되고, 나아가 io_mask 로 하위 니블을 지워
 * 4KB(또는 확장 시 1KB) 단위로 맞춘다.
 * 32비트 I/O 주소를 지원하는 브리지는 상위 16비트를 별도의
 * PCI_IO_BASE_UPPER16 레지스터에 담는다. 그래서 하나의 창을 프로그램하는 데
 * config 쓰기가 두 번 필요하고, 그 사이의 과도 상태를 어떻게 다룰지가
 * 아래 원문 주석의 경고 내용이다.
 *
 * 창을 끄는 방법: PCI-to-PCI 브리지 스펙은 "base > limit 로 쓰면 그 범위는
 * 꺼진 것"으로 규정한다. 그래서 담을 것이 없을 때 l = 0x00f0 을 쓴다 —
 * 하위 바이트(base)가 0xf0, 상위 바이트(limit)가 0x00 이므로
 * base(0xf000...) > limit(0x0fff) 이 되어 창이 닫힌다.
 *
 * 실행 컨텍스트: 프로세스 문맥. config 공간 쓰기가 일어난다. 락은 잡지
 *   않으며, 호출자가 이 브리지에 대한 배치를 직렬화한다고 가정한다.
 *
 * 호출 체인:
 *   __pci_setup_bridge() / pci_setup_one_bridge_window() -> [이 함수]
 *     -> pcibios_resource_to_bus(), pci_read_config_word(),
 *        pci_write_config_word(), pci_write_config_dword()
 */
static void pci_setup_bridge_io(struct pci_dev *bridge)
{
	/* [한국어] 프로그램할 I/O 창 자원(브리지의 resource[] 안에 있다). */
	struct resource *res;
	/* [한국어] 로그용 이름. */
	const char *res_name;
	/* [한국어] CPU 물리 주소가 아니라 "버스 주소"를 담을 자리. 두 주소가
	 * 다른 플랫폼이 있어 반드시 변환을 거쳐야 한다. */
	struct pci_bus_region region;
	/* [한국어] base/limit 레지스터에 남길 상위 비트를 고르는 마스크. */
	unsigned long io_mask;
	/* [한국어] 레지스터에 들어갈 base 와 limit 의 하위 8비트 필드. */
	u8 io_base_lo, io_limit_lo;
	/* [한국어] 두 8비트 필드를 합친 16비트 레지스터 값. */
	u16 l;
	/* [한국어] 32비트 I/O 주소의 상위 16비트 두 개를 합친 값
	 * (상위 절반이 limit, 하위 절반이 base). */
	u32 io_upper16;

	/* [한국어] 기본 세분성 마스크. 표준 브리지의 I/O 창은 4KB 단위다. */
	io_mask = PCI_IO_RANGE_MASK;
	/* [한국어] 일부 브리지는 1KB 단위 확장을 지원한다(probe 단계에서
	 * io_window_1k 로 표시해 둔다). 더 촘촘한 창을 쓸 수 있으면
	 * 마스크를 바꿔 하위 비트를 더 살린다. */
	if (bridge->io_window_1k)
		io_mask = PCI_IO_1K_RANGE_MASK;

	/* Set up the top and bottom of the PCI I/O segment for this bus */
	/* [한국어] 브리지 resource[] 에서 I/O 창 칸을 집는다. */
	res = &bridge->resource[PCI_BRIDGE_IO_WINDOW];
	/* [한국어] 로그용 이름 확보. */
	res_name = pci_resource_name(bridge, PCI_BRIDGE_IO_WINDOW);
	/* [한국어] CPU 물리 주소 -> 버스 주소 변환. 호스트 브리지가 주소를
	 * 옮겨 놓는 플랫폼(예: 오프셋이 있는 임베디드 시스템)에서 이 변환을
	 * 빠뜨리면 브리지에 엉뚱한 값을 쓰게 된다. */
	pcibios_resource_to_bus(bridge->bus, &region, res);
	/* [한국어] 창이 실제로 배정됐고 종류도 I/O 가 맞을 때만 값을 만든다.
	 * 둘 중 하나라도 아니면 아래 else 로 가서 창을 닫는다. */
	if (resource_assigned(res) && res->flags & IORESOURCE_IO) {
		/* [한국어] 현재 레지스터 값을 먼저 읽는다. 하위 4비트에는
		 * 주소가 아니라 "이 브리지가 16비트 I/O 인가 32비트 I/O 인가"를
		 * 나타내는 읽기 전용 표시 비트가 들어 있어서, 그 자리를
		 * 보존한 채 주소 비트만 갈아 끼우기 위해서다. 아래에서 l 은
		 * 통째로 덮어써지지만, io_mask 가 하위 니블을 0 으로 만들어
		 * 그 자리에 0 이 들어가고 읽기 전용 비트는 쓰기가 무시된다. */
		pci_read_config_word(bridge, PCI_IO_BASE, &l);
		/* [한국어] 창 시작 주소를 8비트 밀어 상위 바이트만 남기고,
		 * io_mask 로 세분성 아래 비트를 잘라 낸다. 밀어 낸 하위 8비트는
		 * 하드웨어가 0 으로 간주한다(= 창은 256바이트 경계에서 시작). */
		io_base_lo = (region.start >> 8) & io_mask;
		/* [한국어] 끝 주소도 같은 방식. limit 쪽의 잘려 나간 하위
		 * 비트는 하드웨어가 1 로 간주하므로(창의 마지막 바이트),
		 * 창이 의도한 끝까지 정확히 덮인다. */
		io_limit_lo = (region.end >> 8) & io_mask;
		/* [한국어] 한 16비트 레지스터에 두 필드를 합친다 —
		 * 상위 바이트가 limit, 하위 바이트가 base 다. */
		l = ((u16) io_limit_lo << 8) | io_base_lo;
		/* Set up upper 16 bits of I/O base/limit */
		/* [한국어] 32비트 I/O 주소의 상위 16비트를 담는 별도 레지스터
		 * 값을 만든다. 여기도 상위 절반이 limit, 하위 절반이 base 다:
		 * (end & 0xffff0000) 이 limit 의 상위 16비트를 제자리에 두고,
		 * (start >> 16) 이 base 의 상위 16비트를 하위 절반으로 내린다. */
		io_upper16 = (region.end & 0xffff0000) | (region.start >> 16);
		/* [한국어] 확정된 창을 부팅 로그에 남긴다. 앞의 공백 두 칸은
		 * 상위 "PCI bridge to ..." 줄 아래 들여쓰기로 보이게 하는 것. */
		pci_info(bridge, "  %s %pR\n", res_name, res);
	} else {
		/* Clear upper 16 bits of I/O base/limit */
		/* [한국어] 창을 끈다. 상위 16비트는 0 으로. */
		io_upper16 = 0;
		/* [한국어] base(하위 바이트) = 0xf0, limit(상위 바이트) = 0x00.
		 * base > limit 이므로 스펙에 따라 이 범위는 비활성이 된다.
		 * 위 함수 설명의 "창을 끄는 방법" 참조. */
		l = 0x00f0;
	}
	/* Temporarily disable the I/O range before updating PCI_IO_BASE */
	/* [한국어] 갱신에 config 쓰기가 두 번 필요하다는 데서 오는 위험을
	 * 없앤다. 상위 레지스터를 base=0xffff, limit=0x0000 으로 만들어
	 * (0x0000ffff 의 하위 절반이 base) base > limit 상태로 창을 먼저 닫는다.
	 * 이렇게 하지 않으면 첫 쓰기와 두 번째 쓰기 사이에 창이 엉뚱한 주소
	 * 범위를 가리키는 순간이 생긴다 — 바로 위 함수의 원문 주석이 경고하는
	 * 상황이다. */
	pci_write_config_dword(bridge, PCI_IO_BASE_UPPER16, 0x0000ffff);
	/* Update lower 16 bits of I/O base/limit */
	/* [한국어] 창이 닫힌 상태에서 하위 16비트를 안전하게 갱신한다. */
	pci_write_config_word(bridge, PCI_IO_BASE, l);
	/* Update upper 16 bits of I/O base/limit */
	/* [한국어] 마지막으로 상위 16비트를 쓴다. 이 쓰기가 완료되는 순간
	 * 비로소 창이 의도한 범위로 열린다(또는 io_upper16 이 0 이고 l 이
	 * 0x00f0 이면 닫힌 채로 남는다). */
	pci_write_config_dword(bridge, PCI_IO_BASE_UPPER16, io_upper16);
}

/*
 * [한국어]
 * pci_setup_bridge_mmio - 확정된 non-prefetchable 메모리 창을 브리지에 기록한다
 *
 * @bridge: 대상 브리지.
 * @return: 없음.
 *
 * 왜 필요한가: I/O 창과 같은 이유다. 계산 결과를 하드웨어에 반영해야
 * 브리지가 그 주소 범위의 메모리 트랜잭션을 아래로 통과시킨다.
 * NVMe 학습 관점: NVMe SSD 가 브리지 아래에 꽂혀 있다면, 그 컨트롤러
 * 레지스터 BAR(드라이버가 BAR0 으로 매핑하는 것 — drivers/nvme/host/pci.c:3001)
 * 를 감싸는 창을 여는 것이 이 함수이거나 아래의 prefetchable 판이다.
 * 둘 중 어느 쪽인지는 그 BAR 의 prefetchable 비트에 달려 있는데, 그 값은
 * 하드웨어가 BAR 에 표시하는 것이고 drivers/nvme 코드는 그것을 검사하지
 * 않으므로 이 트리의 정보만으로는 확정할 수 없다. 다만 드라이버가
 * ioremap()(쓰기 결합 없는 일반 매핑)으로 매핑한다는 사실은 확인된다 —
 * 도어벨처럼 접근에 부작용이 있는 레지스터를 담는 창이라는 뜻이다.
 * 어느 창이든, 그 창이 닫혀 있으면 드라이버가 ioremap 에 성공해도 실제
 * 읽기는 브리지에서 막힌다.
 *
 * 메모리 창의 인코딩 (아래 비트 연산의 근거):
 * 메모리 base/limit 는 하나의 32비트 레지스터에 함께 들어간다 —
 * 상위 16비트가 limit, 하위 16비트가 base 다. 각 16비트 필드는 주소의
 * 상위 16비트만 담고 하위 4비트는 읽기 전용이라, 실질적으로 주소의
 * 비트 31:20 만 프로그램된다. 그래서 메모리 창의 세분성이 1MB(2^20)이고,
 * 이 파일 곳곳에서 브리지 메모리 창 정렬을 SZ_1M 으로 잡는 이유가 여기 있다.
 * 코드가 (start >> 16) & 0xfff0 을 쓰는 것이 정확히 그 표현이다:
 * 16비트 내리고 하위 4비트를 지우면 비트 31:20 만 남는다.
 * limit 쪽은 (end & 0xfff00000) 으로, 이미 상위 절반 자리에 있는 비트
 * 31:20 을 그대로 쓴다(옮길 필요가 없다).
 *
 * 실행 컨텍스트: 프로세스 문맥, config 공간 쓰기. I/O 창과 달리 쓰기가
 *   한 번뿐이라 과도 상태 문제가 없다.
 *
 * 호출 체인:
 *   __pci_setup_bridge() / pci_setup_one_bridge_window() -> [이 함수]
 *     -> pcibios_resource_to_bus(), pci_write_config_dword()
 */
static void pci_setup_bridge_mmio(struct pci_dev *bridge)
{
	/* [한국어] 프로그램할 메모리 창 자원. */
	struct resource *res;
	/* [한국어] 로그용 이름. */
	const char *res_name;
	/* [한국어] 버스 주소로 변환한 범위를 담을 자리. */
	struct pci_bus_region region;
	/* [한국어] base 와 limit 를 함께 담을 32비트 레지스터 값. */
	u32 l;

	/* Set up the top and bottom of the PCI Memory segment for this bus */
	/* [한국어] 브리지 resource[] 에서 non-prefetchable 메모리 창 칸을 집는다. */
	res = &bridge->resource[PCI_BRIDGE_MEM_WINDOW];
	/* [한국어] 로그용 이름 확보. */
	res_name = pci_resource_name(bridge, PCI_BRIDGE_MEM_WINDOW);
	/* [한국어] CPU 물리 주소를 버스 주소로 변환. */
	pcibios_resource_to_bus(bridge->bus, &region, res);
	/* [한국어] 배정됐고 종류도 메모리가 맞을 때만 값을 만든다. */
	if (resource_assigned(res) && res->flags & IORESOURCE_MEM) {
		/* [한국어] base 필드(하위 16비트). 시작 주소를 16비트 내리고
		 * 하위 4비트를 지워 비트 31:20 만 남긴다 = 1MB 세분성. */
		l = (region.start >> 16) & 0xfff0;
		/* [한국어] limit 필드(상위 16비트). 끝 주소의 비트 31:20 은
		 * 이미 상위 절반 자리에 있으므로 마스킹만 하면 된다.
		 * 잘려 나간 하위 20비트는 하드웨어가 1 로 간주해 창의 끝이
		 * 1MB 경계 직전까지 정확히 덮인다. */
		l |= region.end & 0xfff00000;
		/* [한국어] 확정된 창을 부팅 로그에 남긴다. */
		pci_info(bridge, "  %s %pR\n", res_name, res);
	} else {
		/* [한국어] 창을 끈다. base(하위 16비트) = 0xfff0,
		 * limit(상위 16비트) = 0x0000 이라 base > limit 이 되어
		 * 스펙상 이 범위는 비활성이 된다. */
		l = 0x0000fff0;
	}
	/* [한국어] 한 번의 dword 쓰기로 base 와 limit 를 동시에 갱신한다.
	 * I/O 창과 달리 원자적이라 과도 상태가 생기지 않는다. */
	pci_write_config_dword(bridge, PCI_MEMORY_BASE, l);
}

/*
 * [한국어]
 * pci_setup_bridge_mmio_pref - 확정된 prefetchable 메모리 창을 브리지에 기록한다
 *
 * @bridge: 대상 브리지.
 * @return: 없음.
 *
 * 왜 별도 함수인가: prefetchable 창은 64비트 주소를 지원할 수 있다.
 * 그러면 base 와 limit 의 상위 32비트를 담는 레지스터가 각각 하나씩 더
 * 필요해, 창 하나를 프로그램하는 데 config 쓰기가 최대 네 번 든다.
 * 그 과도 상태를 다루는 순서가 이 함수의 핵심이며, 첫 번째 쓰기로 창을
 * 먼저 닫아 두는 것이 그 대책이다(아래 원문 주석 참조).
 *
 * 인코딩은 non-prefetchable 창과 같다 — 하위 32비트 레지스터에 base(하위
 * 16비트)와 limit(상위 16비트)가 들어가고 각각 주소의 비트 31:20 만 담는다.
 * 다만 이 레지스터의 하위 4비트는 읽기 전용으로 "이 창이 64비트를
 * 지원하는가"를 알려 주며, 그 값이 PCI_PREF_RANGE_TYPE_64 다
 * (pci_bridge_check_ranges() 가 그 상수를 플래그에 섞어 둔다).
 * 64비트일 때만 upper32 레지스터 두 개가 의미를 갖는다.
 *
 * 실행 컨텍스트: 프로세스 문맥, config 공간 쓰기 최대 4회.
 *
 * 호출 체인:
 *   __pci_setup_bridge() / pci_setup_one_bridge_window() -> [이 함수]
 *     -> pcibios_resource_to_bus(), pci_write_config_dword()
 */
static void pci_setup_bridge_mmio_pref(struct pci_dev *bridge)
{
	/* [한국어] 프로그램할 prefetchable 메모리 창 자원. */
	struct resource *res;
	/* [한국어] 로그용 이름. */
	const char *res_name;
	/* [한국어] 버스 주소로 변환한 범위. */
	struct pci_bus_region region;
	/* [한국어] l 은 하위 32비트 레지스터 값(base + limit),
	 * bu 는 base 의 상위 32비트, lu 는 limit 의 상위 32비트. */
	u32 l, bu, lu;

	/*
	 * Clear out the upper 32 bits of PREF limit.  If
	 * PCI_PREF_BASE_UPPER32 was non-zero, this temporarily disables
	 * PREF range, which is ok.
	 */
	/* [한국어] limit 의 상위 32비트를 0 으로 밀어 창을 먼저 닫는다.
	 * 이렇게 하면 (base 상위가 0 이 아닌 한) base > limit 이 되어 갱신
	 * 도중의 과도 상태가 안전해진다. 바로 위 원문 주석이 "그래도 괜찮다"고
	 * 밝히는 대목이다. */
	pci_write_config_dword(bridge, PCI_PREF_LIMIT_UPPER32, 0);

	/* Set up PREF base/limit */
	/* [한국어] 상위 32비트 기본값 0. 창을 끄거나 32비트 창일 때 그대로
	 * 0 이 쓰여, 주소가 4GB 아래에 있음을 뜻하게 된다. */
	bu = lu = 0;
	/* [한국어] 브리지 resource[] 에서 prefetchable 메모리 창 칸을 집는다. */
	res = &bridge->resource[PCI_BRIDGE_PREF_MEM_WINDOW];
	/* [한국어] 로그용 이름 확보. */
	res_name = pci_resource_name(bridge, PCI_BRIDGE_PREF_MEM_WINDOW);
	/* [한국어] CPU 물리 주소를 버스 주소로 변환. */
	pcibios_resource_to_bus(bridge->bus, &region, res);
	/* [한국어] 배정됐고 실제로 prefetchable 인 경우에만 값을 만든다.
	 * 여기서 IORESOURCE_MEM 이 아니라 IORESOURCE_PREFETCH 를 보는 것이
	 * non-prefetchable 판과 다른 점이다. */
	if (resource_assigned(res) && res->flags & IORESOURCE_PREFETCH) {
		/* [한국어] base 필드 — 주소를 16비트 내리고 하위 4비트를 지워
		 * 비트 31:20 만 남긴다(1MB 세분성). 지워지는 하위 4비트는
		 * 하드웨어의 읽기 전용 64비트 지원 표시 자리라, 여기에 0 을
		 * 써도 그 값이 바뀌지 않는다. */
		l = (region.start >> 16) & 0xfff0;
		/* [한국어] limit 필드 — 끝 주소의 비트 31:20 을 상위 절반에
		 * 그대로 둔다. */
		l |= region.end & 0xfff00000;
		/* [한국어] 창이 64비트를 지원할 때만 상위 32비트가 의미를 갖는다.
		 * 32비트 창에 상위 비트를 쓰면 무시되거나 잘못 해석될 수 있다. */
		if (res->flags & IORESOURCE_MEM_64) {
			/* [한국어] 시작 주소의 상위 32비트를 추출한다.
			 * upper_32_bits() 는 32비트 아키텍처에서 시프트 폭
			 * 경고가 나지 않도록 만든 커널 매크로다. */
			bu = upper_32_bits(region.start);
			/* [한국어] 끝 주소의 상위 32비트. */
			lu = upper_32_bits(region.end);
		}
		/* [한국어] 확정된 창을 부팅 로그에 남긴다. */
		pci_info(bridge, "  %s %pR\n", res_name, res);
	} else {
		/* [한국어] 창을 끈다 — base(0xfff0) > limit(0x0000). 이때
		 * bu 와 lu 는 위에서 0 으로 초기화된 값 그대로 쓰인다. */
		l = 0x0000fff0;
	}
	/* [한국어] 하위 32비트(base + limit) 갱신. 이 시점에도 limit 상위가
	 * 아직 0 이라 창은 여전히 닫혀 있거나 좁은 상태다. */
	pci_write_config_dword(bridge, PCI_PREF_MEMORY_BASE, l);

	/* Set the upper 32 bits of PREF base & limit */
	/* [한국어] base 의 상위 32비트를 먼저 쓴다. 이 순서가 중요하다 —
	 * limit 상위를 먼저 쓰면 base 상위가 아직 옛 값인 상태로 창이
	 * 열려 버려 엉뚱한 범위를 가리키는 순간이 생긴다. */
	pci_write_config_dword(bridge, PCI_PREF_BASE_UPPER32, bu);
	/* [한국어] 마지막으로 limit 의 상위 32비트를 쓴다. 이 쓰기가
	 * 끝나는 순간 창이 의도한 범위로 열린다. */
	pci_write_config_dword(bridge, PCI_PREF_LIMIT_UPPER32, lu);
}

/*
 * [한국어]
 * __pci_setup_bridge - 요청한 종류의 브리지 창을 모두 프로그램한다
 *
 * @bus: 브리지 "아래"의 버스. 브리지 장치 자체는 bus->self 로 얻는다.
 *       (창은 브리지의 것이지만, 창이 덮는 대상은 이 버스이므로 인자를
 *        버스로 받는 편이 호출부에서 자연스럽다.)
 * @type: 프로그램할 창의 종류 비트 조합. IORESOURCE_IO / IORESOURCE_MEM /
 *        IORESOURCE_PREFETCH 를 각각 켜면 해당 창을 갱신한다.
 * @return: 없음.
 *
 * 왜 필요한가: 세 창을 개별로 쓰는 함수들 위에, "이 브리지의 창들을
 * 한꺼번에 반영하라"는 상위 동작을 얹은 것이다. 마지막에 Bridge Control
 * 레지스터까지 다시 써서 브리지 동작 설정(리셋, VGA 포워딩, 오류 응답 등을
 * 담는 레지스터)을 커널이 들고 있는 값으로 맞춘다.
 * 실행 컨텍스트: 프로세스 문맥, config 공간 쓰기 다수.
 *
 * 호출 체인:
 *   pci_setup_bridge() -> [이 함수]
 *     -> pci_setup_bridge_io() / _mmio() / _mmio_pref(),
 *        pci_write_config_word()
 */
static void __pci_setup_bridge(struct pci_bus *bus, unsigned long type)
{
	/* [한국어] 이 버스의 상위 브리지 장치. bus->self 가 곧 "이 버스를
	 * 만들어 낸 브리지"다. 루트 버스에서는 NULL 이지만, 이 함수는 루트
	 * 버스에 대해 불리지 않는다(호출부가 브리지가 있는 경우에만 부른다). */
	struct pci_dev *bridge = bus->self;

	/* [한국어] 부팅 로그의 머리글. %pR 로 이 브리지가 담당하는 버스 번호
	 * 범위(secondary~subordinate)를 찍는다. 뒤이어 각 창 함수가 들여쓴
	 * 줄로 창 범위를 덧붙여 한 덩어리로 읽히게 만든다. */
	pci_info(bridge, "PCI bridge to %pR\n", &bus->busn_res);

	/* [한국어] I/O 창을 요청했으면 갱신. */
	if (type & IORESOURCE_IO)
		pci_setup_bridge_io(bridge);

	/* [한국어] non-prefetchable 메모리 창을 요청했으면 갱신. */
	if (type & IORESOURCE_MEM)
		pci_setup_bridge_mmio(bridge);

	/* [한국어] prefetchable 메모리 창을 요청했으면 갱신. */
	if (type & IORESOURCE_PREFETCH)
		pci_setup_bridge_mmio_pref(bridge);

	/* [한국어] Bridge Control 레지스터를 커널이 들고 있는 값으로 되쓴다.
	 * 이 레지스터는 창 주소와 별개로 브리지의 동작(예: secondary 버스
	 * 리셋, VGA 사이클 포워딩, 오류 보고)을 제어한다. 창을 갱신하는
	 * 김에 함께 맞춰 두어, 펌웨어가 남긴 설정과 커널의 인식이 어긋나지
	 * 않게 한다. */
	pci_write_config_word(bridge, PCI_BRIDGE_CONTROL, bus->bridge_ctl);
}

/*
 * [한국어]
 * pci_setup_one_bridge_window - resource[] 인덱스로 창 하나만 골라 프로그램한다
 *
 * @bridge: 대상 브리지 장치.
 * @resno: resource[] 인덱스. 브리지 창 세 칸 중 하나여야 의미가 있다.
 * @return: 없음. 브리지 창이 아닌 인덱스면 아무 일도 하지 않는다.
 *
 * 왜 필요한가: __pci_setup_bridge() 는 종류 비트마스크로 여러 창을 한꺼번에
 * 다루지만, 호출부에 따라 "방금 놓아 준 그 창 하나만" 하드웨어에 반영하고
 * 싶을 때가 있다. pci_bridge_release_resources() 는 창을 놓은 직후 그
 * 사실을 하드웨어에 알려야 하고(닫힌 창을 기록해야 브리지가 더 이상
 * 그 범위를 통과시키지 않는다), pci_claim_bridge_resource() 는 창을
 * 좁힌 뒤 그 결과만 반영하면 된다. 인덱스 -> 함수 사상만 하는 얇은 계층이다.
 * 실행 컨텍스트: 프로세스 문맥, config 공간 쓰기.
 *
 * 호출 체인:
 *   pci_claim_bridge_resource() / pci_bridge_release_resources()
 *     -> [이 함수] -> pci_setup_bridge_io() / _mmio() / _mmio_pref()
 */
static void pci_setup_one_bridge_window(struct pci_dev *bridge, int resno)
{
	/* [한국어] 브리지 창 세 칸을 인덱스로 구분한다. */
	switch (resno) {
	/* [한국어] I/O 창 칸. */
	case PCI_BRIDGE_IO_WINDOW:
		pci_setup_bridge_io(bridge);
		break;
	/* [한국어] non-prefetchable 메모리 창 칸. */
	case PCI_BRIDGE_MEM_WINDOW:
		pci_setup_bridge_mmio(bridge);
		break;
	/* [한국어] prefetchable 메모리 창 칸. */
	case PCI_BRIDGE_PREF_MEM_WINDOW:
		pci_setup_bridge_mmio_pref(bridge);
		break;
	default:
		/* [한국어] 일반 BAR 나 ROM 등 브리지 창이 아닌 인덱스가 들어온
		 * 경우다. 브리지 창 레지스터를 건드릴 이유가 없으므로 조용히
		 * 돌아간다 — 호출부가 "브리지 창일 수도 아닐 수도 있는"
		 * 인덱스를 그냥 넘길 수 있게 해 주는 방어적 처리다. */
		return;
	}
}

/*
 * [한국어]
 * pcibios_setup_bridge - 아키텍처별 브리지 설정 훅 (기본 구현은 빈 함수)
 *
 * @bus: 설정할 브리지 아래의 버스.
 * @type: 프로그램할 창 종류 비트 조합.
 * @return: 없음.
 *
 * 왜 필요한가: 일부 아키텍처나 플랫폼은 표준 config 공간 쓰기만으로는
 * 브리지 창이 완성되지 않는다(호스트 컨트롤러 쪽에 별도 주소 변환 테이블을
 * 같이 프로그램해야 하는 경우 등). __weak 로 선언해 두면, 그런 플랫폼이
 * 같은 이름의 강한 심볼을 정의해 이 기본 구현을 덮어쓸 수 있다. 링커가
 * 강한 정의를 우선하므로 #ifdef 로 코드를 어지럽히지 않아도 된다.
 * 대부분의 플랫폼에는 할 일이 없어 본문이 비어 있다.
 * 참고: 이 트리에서 이 함수를 재정의하는 아키텍처 코드가 있는지는
 *   arch/ 디렉터리가 포함돼 있지 않아 확인할 수 없다.
 * 실행 컨텍스트: 프로세스 문맥. 기본 구현은 아무것도 하지 않는다.
 *
 * 호출 체인:
 *   pci_setup_bridge() -> [이 함수] (기본 구현은 즉시 반환)
 */
void __weak pcibios_setup_bridge(struct pci_bus *bus, unsigned long type)
{
	/* [한국어] 기본 구현은 의도적으로 비어 있다. 필요한 플랫폼만
	 * 강한 심볼로 덮어쓴다. */
}

/*
 * [한국어]
 * pci_setup_bridge - 이 버스의 상위 브리지 창 셋을 모두 하드웨어에 반영한다
 *
 * @bus: 브리지 아래의 버스.
 * @return: 없음.
 *
 * 왜 필요한가: 배치가 끝난 뒤 "이 브리지의 모든 창을 현재 계산 결과대로
 * 맞춰라"는 가장 흔한 요청을 한 줄로 만든 것이다. 아키텍처 훅을 먼저
 * 부르고 표준 경로를 뒤에 부른다 — 플랫폼이 사전 준비를 할 기회를
 * 표준 레지스터 쓰기보다 앞에 두는 순서다.
 * 실행 컨텍스트: 프로세스 문맥, config 공간 쓰기 다수.
 *
 * 호출 체인:
 *   __pci_bus_assign_resources() / __pci_bridge_assign_resources() /
 *   pbus_reassign_bridge_resources() / pci_do_resource_release_and_resize()
 *     -> [이 함수] -> pcibios_setup_bridge(), __pci_setup_bridge()
 */
static void pci_setup_bridge(struct pci_bus *bus)
{
	/* [한국어] 세 창을 전부 대상으로 삼는다. 해당 창이 없거나 배정되지
	 * 않았으면 개별 함수가 알아서 "닫힘" 값을 쓰므로, 여기서 미리
	 * 걸러 낼 필요가 없다. */
	unsigned long type = IORESOURCE_IO | IORESOURCE_MEM |
				  IORESOURCE_PREFETCH;

	/* [한국어] 아키텍처별 사전 처리 훅(대부분 빈 함수). */
	pcibios_setup_bridge(bus, type);
	/* [한국어] 표준 경로 — 실제 config 레지스터 쓰기가 여기서 일어난다. */
	__pci_setup_bridge(bus, type);
}


/*
 * [한국어]
 * pci_claim_bridge_resource - 펌웨어가 프로그램해 둔 브리지 창을 그대로 승인한다
 *
 * @bridge: 대상 브리지 장치.
 * @i: resource[] 인덱스.
 * @return: 0 이면 "문제 없음"(승인 성공이거나, 애초에 이 함수가 다룰 대상이
 *          아니어서 신경 쓸 것이 없는 경우). 0 이 아니면 이 창을 그대로는
 *          쓸 수 없다는 뜻이다. 호출자(pci_claim_bridge_resources)는 반환값을
 *          보지 않고 넘어가는데, 실패한 창은 이후 재배치 경로가 다시 다루기
 *          때문이다.
 *
 * 왜 필요한가: 커널이 자원을 새로 배치하는 경로와 별개로, 펌웨어(BIOS/UEFI)가
 * 이미 잡아 둔 배치를 그대로 물려받는 경로가 있다(pci_bus_claim_resources).
 * "claim" 은 하드웨어의 현재 값을 커널 자원 트리에 등록해 소유권을 주장하는
 * 것이다. 이때 펌웨어가 정한 창이 상위 창 밖으로 삐져나와 있으면 그대로는
 * 등록할 수 없다. 그런 경우 창을 상위 범위 안으로 잘라(clip) 다시 시도한다.
 *
 * 왜 잘라도 되는가: 브리지 창을 좁히면 그 창 아래 장치 중 잘려 나간 범위에
 * 있던 것은 접근할 수 없게 되지만, 애초에 상위 창 밖의 주소는 실제로는
 * 도달할 수 없는 주소다. 좁히는 편이 정직하다.
 * 왜 PCI-to-PCI 브리지만 자르는가: CardBus 등 다른 브리지는 창 레지스터
 * 구조가 달라 이 파일의 자르기 로직이 맞지 않는다. 그래서 class 를 확인해
 * 표준 PCI 브리지가 아니면 손대지 않고 0 을 돌려준다.
 * 실행 컨텍스트: 프로세스 문맥. config 공간 접근 발생.
 *
 * 호출 체인:
 *   pci_claim_bridge_resources() -> [이 함수]
 *     -> pci_claim_resource() (setup-res.c),
 *        pci_bus_clip_resource() (bus.c), pci_setup_one_bridge_window()
 */
int pci_claim_bridge_resource(struct pci_dev *bridge, int i)
{
	/* [한국어] 기본 반환값은 실패다. 아래에서 승인에 성공해야만 0 이 된다.
	 * 자르기 시도조차 못 한 경우(clip 이 아무것도 못 바꾼 경우) 이 값이
	 * 그대로 나간다. */
	int ret = -EINVAL;

	/* [한국어] 브리지 창 구획이 아니면 이 함수가 다룰 대상이 아니다.
	 * 오류가 아니므로 0 을 돌려준다. */
	if (!pci_resource_is_bridge_win(i))
		return 0;

	/* [한국어] 먼저 있는 그대로 승인해 본다. 성공하면 펌웨어의 배치가
	 * 상위 창 안에 잘 들어맞았다는 뜻이라 더 할 일이 없다. */
	if (pci_claim_resource(bridge, i) == 0)
		return 0;	/* Claimed the window */

	/* [한국어] 여기부터는 승인 실패 후의 구제 경로다. 표준 PCI-to-PCI
	 * 브리지가 아니면 창 자르기 로직을 적용할 수 없다. class 를 8비트
	 * 내려 base+sub class 16비트를 얻어 비교한다. 0 을 돌려주는 이유는
	 * "우리가 다룰 종류가 아니다"이지 "오류"가 아니기 때문이다. */
	if ((bridge->class >> 8) != PCI_CLASS_BRIDGE_PCI)
		return 0;

	/* [한국어] 브리지 창 구획 안에서도 세 표준 창(IO/MEM/PREF MEM)만
	 * 자르기를 지원한다. 그 뒤 인덱스는 이 로직의 대상이 아니다. */
	if (i > PCI_BRIDGE_PREF_MEM_WINDOW)
		return -EINVAL;

	/* Try to clip the resource and claim the smaller window */
	/* [한국어] 상위 창 범위 안으로 잘라 낸다. 실제로 잘라 낼 것이 있어
	 * 창이 바뀐 경우에만 참을 돌려주며, 그때만 다시 승인을 시도한다. */
	if (pci_bus_clip_resource(bridge, i))
		ret = pci_claim_resource(bridge, i);

	/* [한국어] 자르기 성공 여부와 무관하게 현재 창 값을 하드웨어에 반영한다.
	 * 커널 자원 트리와 브리지 레지스터가 어긋난 채로 남으면, 이후의 모든
	 * 판단이 실제와 다른 전제 위에서 이뤄지기 때문이다. */
	pci_setup_one_bridge_window(bridge, i);

	/* [한국어] 승인 결과를 그대로 올린다(0 이면 좁힌 창으로 승인 성공). */
	return ret;
}

/*
 * Check whether the bridge supports optional I/O and prefetchable memory
 * ranges.  If not, the respective base/limit registers must be read-only
 * and read as 0.
 */
/*
 * [한국어]
 * pci_bridge_check_ranges - 브리지가 지원하는 창 종류를 자원 플래그에 반영한다
 *
 * @bus: 브리지 아래의 버스. 브리지 장치는 bus->self.
 * @return: 없음.
 *
 * 왜 필요한가: 브리지마다 가진 창이 다르다. non-prefetchable 메모리 창은
 * 필수지만, I/O 창과 prefetchable 메모리 창은 선택 사항이고, prefetchable
 * 창이 있어도 64비트 지원 여부가 갈린다. 크기 산정 코드
 * (pbus_select_window_for_type 등)는 "이 창이 유효한가"를 resource[] 의
 * flags 로만 판단하므로, 산정을 시작하기 전에 하드웨어의 실제 능력을
 * 그 플래그에 새겨 넣어야 한다. 그 일을 하는 함수다.
 *
 * 능력 판정 자체는 probe 단계에서 이미 끝나 struct pci_dev 의 io_window /
 * pref_window / pref_64_window 비트필드에 기록돼 있다. 그 판정 근거는 바로
 * 위 원문 주석이 밝히듯 "지원하지 않으면 해당 base/limit 레지스터가 읽기
 * 전용이고 0 으로 읽힌다"는 스펙 규정이다.
 * 실행 컨텍스트: 프로세스 문맥. config 공간 접근 없음(이미 읽어 둔 값을 쓴다).
 *
 * 호출 체인:
 *   __pci_bus_size_bridges() -> [이 함수]
 */
static void pci_bridge_check_ranges(struct pci_bus *bus)
{
	/* [한국어] 창을 소유한 브리지 장치. */
	struct pci_dev *bridge = bus->self;
	/* [한국어] 지금 손보는 창 자원을 가리킬 임시 포인터. */
	struct resource *b_res;

	/* [한국어] non-prefetchable 메모리 창은 모든 PCI-to-PCI 브리지가
	 * 반드시 갖는다. 조건 없이 유효 표시를 켠다. */
	b_res = &bridge->resource[PCI_BRIDGE_MEM_WINDOW];
	/* [한국어] 이 비트가 켜져야 pbus_select_window_for_type() 이 이 창을
	 * "쓸 수 있는 메모리 창"으로 인정한다. */
	b_res->flags |= IORESOURCE_MEM;

	/* [한국어] I/O 창은 선택 사항이다. probe 단계가 지원 여부를 확인해
	 * io_window 비트에 남겨 두었다. */
	if (bridge->io_window) {
		/* [한국어] I/O 창 칸을 집어 */
		b_res = &bridge->resource[PCI_BRIDGE_IO_WINDOW];
		/* [한국어] I/O 공간 종류 표시를 켠다. */
		b_res->flags |= IORESOURCE_IO;
	}

	/* [한국어] prefetchable 메모리 창도 선택 사항이다. */
	if (bridge->pref_window) {
		/* [한국어] prefetchable 창 칸을 집어 */
		b_res = &bridge->resource[PCI_BRIDGE_PREF_MEM_WINDOW];
		/* [한국어] "메모리이면서 prefetchable" 두 비트를 함께 켠다.
		 * MEM 도 켜는 이유: 이 창은 여전히 메모리 공간이며,
		 * pbus_select_window_for_type() 의 유효성 검사가
		 * IORESOURCE_MEM 을 본다. */
		b_res->flags |= IORESOURCE_MEM | IORESOURCE_PREFETCH;
		/* [한국어] 그 prefetchable 창이 64비트 주소까지 지원하는가. */
		if (bridge->pref_64_window) {
			/* [한국어] IORESOURCE_MEM_64 는 커널 쪽 표시로,
			 * "이 창은 4GB 위쪽에 놓을 수 있다"는 뜻이다.
			 * PCI_PREF_RANGE_TYPE_64 는 하드웨어 레지스터의
			 * 하위 4비트에 실제로 들어 있는 인코딩 값으로,
			 * pci_setup_bridge_mmio_pref() 가 base 필드를 만들 때
			 * 그 하위 니블 자리에 맞물린다. 둘을 함께 켜 두어
			 * 커널 표현과 하드웨어 표현이 한 필드 안에서
			 * 일관되게 유지된다. */
			b_res->flags |= IORESOURCE_MEM_64 |
					PCI_PREF_RANGE_TYPE_64;
		}
	}
}

/*
 * [한국어]
 * calculate_iosize - 브리지 I/O 창의 최종 크기를 산출한다
 *
 * @size: 256바이트 미만(ISA 별칭 문제에 걸릴 수 있는) 자식 I/O 자원들의
 *        크기 합. 아래 ISA 보정의 대상이 되는 몫이다.
 * @min_size: 최소로 보장할 크기. realloc_head 를 쓰지 않는 호출에서는
 *        핫플러그 예비 크기가 여기로 들어온다.
 * @size1: 1KB 이상인 자식 I/O 자원들의 크기 합. ISA 보정 대상이 아니다.
 * @add_size: 선택적으로 더 얹고 싶은 크기.
 * @children_add_size: 자식들이 각자 요구한 선택적 추가 크기의 합.
 * @old_size: 이 창의 기존 크기. 이보다 작게 줄이지 않기 위한 하한이다.
 * @align: 최종 결과를 맞출 정렬(창의 세분성).
 * @return: 정렬까지 마친 최종 창 크기.
 *
 * 왜 필요한가: 크기 산정 pass 의 마지막 계산식을 한곳에 모은 함수다.
 * pbus_size_io() 가 자식들을 훑어 모은 여러 몫을 넘기면, 여기서 우선순위와
 * ISA 보정을 적용해 하나의 수로 만든다.
 * 특히 ISA 별칭(aliasing) 보정이 이 함수만의 특수 사정이다 — 옛 ISA 버스는
 * I/O 주소의 상위 비트를 디코딩하지 않아, 1KB 블록마다 하위 256바이트가
 * 서로 별칭이 된다. 그래서 그 구간을 쓰려면 실제 필요량의 4배를 확보해
 * 충돌을 피해야 한다.
 * 실행 컨텍스트: 순수 계산. 부작용 없음.
 *
 * 호출 체인:
 *   pbus_size_io() -> [이 함수]
 */
static resource_size_t calculate_iosize(resource_size_t size,
					resource_size_t min_size,
					resource_size_t size1,
					resource_size_t add_size,
					resource_size_t children_add_size,
					resource_size_t old_size,
					resource_size_t align)
{
	/* [한국어] 최소 보장 크기보다 작으면 끌어올린다. */
	if (size < min_size)
		size = min_size;
	/* [한국어] 기존 크기가 1 이면 0 으로 본다. 크기 1 은 실제 자원이
	 * 아니라 "start == end" 인 퇴화 상태(사실상 빈 창)를 뜻하므로,
	 * 아래 max(size, old_size) 에서 하한 노릇을 하면 안 된다. */
	if (old_size == 1)
		old_size = 0;
	/*
	 * To be fixed in 2.5: we should have sort of HAVE_ISA flag in the
	 * struct pci_bus.
	 */
/* [한국어] ISA/EISA 버스가 설정에 포함된 커널에서만 별칭 보정을 한다.
 * 그런 옛 버스가 없는 시스템에서는 4배로 부풀릴 이유가 전혀 없으므로
 * 조건부 컴파일로 코드 자체를 없앤다. */
#if defined(CONFIG_ISA) || defined(CONFIG_EISA)
	/* [한국어] 별칭 보정 계산. 하위 256바이트 몫(size & 0xff)은 그대로 두고,
	 * 256바이트 경계 위의 몫((size & ~0xffUL))만 4배로 부풀린다
	 * (왼쪽 2비트 시프트 = x4). ISA 는 1KB 블록 안에서 상위 비트를
	 * 디코딩하지 않아 256바이트마다 같은 주소가 네 번 반복되는데,
	 * 그 반복분까지 확보해야 다른 장치와 겹치지 않기 때문이다. */
	size = (size & 0xff) + ((size & ~0xffUL) << 2);
#endif
	/* [한국어] 보정이 필요 없던 큰 자원들(1KB 이상)의 몫을 더한다.
	 * 이것들은 ISA 별칭 구간 밖에 놓이므로 4배 보정을 하지 않았다. */
	size = size + size1;

	/* [한국어] 선택적 추가 크기 반영. 더하는 것이 아니라 max 를 취하는
	 * 점이 중요하다 — add_size 는 "이만큼은 되게 해 달라"는 목표 크기이지
	 * "필수분에 더 얹을 양"이 아니다. 반면 children_add_size 는 자식들이
	 * 각자 따로 요구한 몫이라 실제로 더해 주어야 한다. */
	size = max(size, add_size) + children_add_size;
	/* [한국어] 마지막으로 기존 크기보다 줄지 않게 하한을 걸고, 창의
	 * 세분성(align)에 맞춰 위로 올림한다. 정렬되지 않은 크기는 브리지
	 * base/limit 레지스터로 표현할 수 없다. */
	return ALIGN(max(size, old_size), align);
}

/*
 * [한국어]
 * calculate_memsize - 브리지 메모리 창의 최종 크기를 산출한다
 *
 * @size: 자식 메모리 자원들의 크기 합(정렬 낭비까지 포함해 누적한 값).
 * @min_size: 최소로 보장할 크기(핫플러그 예비 등).
 * @children_add_size: 자식들이 요구한 선택적 추가 크기의 합.
 * @align: 창의 세분성(메모리 창은 최소 1MB).
 * @return: 정렬까지 마친 최종 창 크기.
 *
 * 왜 I/O 판보다 단순한가: 메모리 공간에는 ISA 별칭 문제가 없고, 기존 크기를
 * 하한으로 삼는 처리(old_size)도 호출부인 pbus_size_mem() 이 하지 않는다.
 * 그래서 "최소 보장 -> 자식 추가분 더하기 -> 정렬"의 세 단계로 끝난다.
 * 실행 컨텍스트: 순수 계산. 부작용 없음.
 *
 * 호출 체인:
 *   pbus_size_mem() -> [이 함수]
 */
static resource_size_t calculate_memsize(resource_size_t size,
					 resource_size_t min_size,
					 resource_size_t children_add_size,
					 resource_size_t align)
{
	/* [한국어] 최소 보장 크기를 하한으로 걸고(max), 자식들이 따로 요구한
	 * 선택적 추가분은 실제로 더한다(+). I/O 판과 같은 이유로 min_size 는
	 * 목표치이고 children_add_size 는 누적분이다. */
	size = max(size, min_size) + children_add_size;
	/* [한국어] 창 세분성에 맞춰 위로 올림. 메모리 창은 1MB 단위로만
	 * 표현되므로(base/limit 가 주소 비트 31:20 만 담는다) 반드시 필요하다. */
	return ALIGN(size, align);
}

/*
 * [한국어]
 * pcibios_window_alignment - 아키텍처별 추가 창 정렬 요구 훅 (기본값 1)
 *
 * @bus: 대상 버스.
 * @type: 창 종류 플래그.
 * @return: 이 플랫폼이 요구하는 최소 창 정렬. 기본 구현은 1 을 돌려주어
 *          "추가 요구 없음"을 뜻한다(1 로 정렬한다 = 아무 제약 없음).
 *
 * 왜 필요한가: PCI 스펙이 정한 창 세분성(메모리 1MB, I/O 4KB)보다 더 엄격한
 * 정렬을 요구하는 플랫폼이 있다. 예를 들어 IOMMU 나 호스트 브리지의 주소
 * 변환 창이 더 큰 단위로만 설정되는 경우다. __weak 로 두어 그런 플랫폼만
 * 강한 심볼로 덮어쓰게 한다. 반환값은 pci_min_window_alignment() 에서
 * 스펙 기본값과 max 로 합쳐지므로, 플랫폼이 더 느슨한 값을 돌려줘도
 * 스펙 요구가 무너지지 않는다.
 * 참고: 이 트리에서 이 함수를 재정의하는 아키텍처 코드가 있는지는 arch/
 *   디렉터리가 포함돼 있지 않아 확인할 수 없다.
 * 실행 컨텍스트: 순수 계산(기본 구현 기준).
 *
 * 호출 체인:
 *   pci_min_window_alignment() -> [이 함수]
 */
resource_size_t __weak pcibios_window_alignment(struct pci_bus *bus,
						unsigned long type)
{
	/* [한국어] 1 은 "1바이트 경계에 맞춘다" = 사실상 제약 없음을 뜻한다.
	 * 0 을 돌려주면 ALIGN 계산이 무너지므로 반드시 1 이어야 한다. */
	return 1;
}

/* [한국어] PCI-to-PCI(= P2P) 브리지 메모리 창의 기본 정렬: 1MB.
 * 근거는 하드웨어 인코딩이다 — 메모리 base/limit 레지스터는 주소의
 * 비트 31:20 만 담으므로 1MB 아래 자리는 표현할 방법이 없다
 * (pci_setup_bridge_mmio() 의 비트 연산 참조). */
#define PCI_P2P_DEFAULT_MEM_ALIGN	SZ_1M
/* [한국어] P2P 브리지 I/O 창의 기본 정렬: 4KB. I/O base/limit 레지스터가
 * 주소의 상위 니블만 담아 4KB 단위로만 창 경계를 표현할 수 있다. */
#define PCI_P2P_DEFAULT_IO_ALIGN	SZ_4K
/* [한국어] 1KB 세분성 확장을 지원하는 브리지용 I/O 창 정렬: 1KB.
 * pci_setup_bridge_io() 가 io_window_1k 일 때 마스크를 바꿔 하위 비트를
 * 더 살리는 것과 짝을 이룬다. 더 촘촘한 창을 쓸 수 있으면 좁은 I/O 공간을
 * 아낄 수 있다. */
#define PCI_P2P_DEFAULT_IO_ALIGN_1K	SZ_1K

/*
 * [한국어]
 * pci_min_window_alignment - 이 버스의 창이 지켜야 할 최소 정렬을 구한다
 *
 * @bus: 대상 버스. I/O 창의 1KB 확장 지원 여부를 bus->self 에서 확인한다.
 * @type: 창 종류 플래그(IORESOURCE_MEM / IORESOURCE_IO 를 본다).
 * @return: 스펙 기본값과 플랫폼 요구 중 더 엄격한(큰) 쪽.
 *
 * 왜 필요한가: 창 크기를 계산할 때마다 "이 종류의 창은 몇 바이트 경계여야
 * 하는가"를 물어야 한다. 그 답은 두 출처에서 오고(하드웨어 인코딩이 정한
 * 스펙 기본값 + 플랫폼이 추가로 요구하는 값) 둘 중 더 엄격한 쪽을 따라야
 * 한다. 이 함수가 그 합성을 담당한다.
 * 실행 컨텍스트: 순수 계산. 락 없음.
 *
 * 호출 체인:
 *   pbus_size_io() / pbus_size_mem() / drivers/pci/setup-res.c:333
 *     -> [이 함수] -> pcibios_window_alignment()
 */
resource_size_t pci_min_window_alignment(struct pci_bus *bus, unsigned long type)
{
	/* [한국어] align 은 스펙 기본값(1 = 제약 없음에서 시작),
	 * arch_align 은 플랫폼이 요구하는 값을 받을 자리. */
	resource_size_t align = 1, arch_align;

	/* [한국어] 메모리 창이면 1MB. prefetchable 이든 아니든 같은 세분성이다
	 * (두 창 모두 같은 레지스터 인코딩을 쓴다). */
	if (type & IORESOURCE_MEM)
		align = PCI_P2P_DEFAULT_MEM_ALIGN;
	/* [한국어] I/O 창이면 브리지의 확장 지원 여부에 따라 4KB 또는 1KB.
	 * MEM 을 먼저 검사하는 이유: 두 비트가 동시에 설 일은 없지만, 순서를
	 * 고정해 두면 판단이 명확하다. */
	else if (type & IORESOURCE_IO) {
		/*
		 * Per spec, I/O windows are 4K-aligned, but some bridges have
		 * an extension to support 1K alignment.
		 */
		/* [한국어] bus->self 를 먼저 확인하는 이유: 루트 버스에는 상위
		 * 브리지가 없어 NULL 이다. NULL 역참조를 피하면서, 루트 버스는
		 * 자연히 보수적인 4KB 쪽으로 떨어진다. */
		if (bus->self && bus->self->io_window_1k)
			align = PCI_P2P_DEFAULT_IO_ALIGN_1K;
		else
			/* [한국어] 확장을 지원하지 않으면 스펙 기본값 4KB. */
			align = PCI_P2P_DEFAULT_IO_ALIGN;
	}

	/* [한국어] 플랫폼이 추가로 요구하는 정렬을 묻는다. 기본 구현은 1 이라
	 * 아래 max 에서 아무 영향을 주지 않는다. */
	arch_align = pcibios_window_alignment(bus, type);
	/* [한국어] 둘 중 더 엄격한(큰) 쪽을 택한다. 정렬은 항상 더 큰 쪽을
	 * 따라야 두 제약을 모두 만족한다 — 큰 경계에 맞은 주소는 그것을
	 * 나누어떨어지게 하는 작은 경계에도 자동으로 맞기 때문이다
	 * (두 값이 모두 2의 거듭제곱이므로 성립한다). */
	return max(align, arch_align);
}

/**
 * pbus_size_io() - Size the I/O window of a given bus
 *
 * @bus:		The bus
 * @add_size:		Additional I/O window
 * @realloc_head:	Track the additional I/O window on this list
 *
 * Sizing the I/O windows of the PCI-PCI bridge is trivial, since these
 * windows have 1K or 4K granularity and the I/O ranges of non-bridge PCI
 * devices are limited to 256 bytes.  We must be careful with the ISA
 * aliasing though.
 */
/* [한국어] 아래 함수의 한국어 해설 (원문 kernel-doc 은 그 아래 그대로 둔다):
 * pbus_size_io - 한 버스의 I/O 창 크기와 정렬을 계산한다
 *
 * @bus: 크기를 정할 버스.
 * @add_size: 이 창에 얹고 싶은 선택적 예비 크기(핫플러그 브리지면
 *        pci_hotplug_io_size 가 들어온다).
 * @realloc_head: 선택적 요구를 실을 목록. NULL 이면 add_size 를 "필수"처럼
 *        취급해 한 번에 반영한다(뒤로 미룰 곳이 없기 때문이다).
 * @return: 없음. 결과는 b_res 의 크기/정렬/플래그에 기록되고, 선택적 몫은
 *        realloc_head 에 등록된다.
 *
 * 왜 필요한가: 크기 산정 pass 에서 "이 버스의 I/O 창이 얼마나 커야 하는가"를
 * 답하는 함수다. 아래 원문 kernel-doc 이 밝히듯 I/O 는 계산이 비교적 단순한데,
 * 창 세분성이 1KB/4KB 로 크고 일반 장치의 I/O BAR 는 256바이트를 넘지 않기
 * 때문이다. 다만 ISA 별칭 문제만은 조심해야 한다.
 *
 * 두 개의 크기를 계산하는 이유(size0 과 size1):
 *   size0 = 선택적 요구를 뺀 "필수" 크기. 실제로 창에 먼저 적용된다.
 *   size1 = 선택적 요구까지 포함한 "이상적" 크기.
 * 둘의 차이(size1 - size0)가 realloc_head 에 add_size 로 등록되어,
 * 나중에 공간이 남으면 그만큼 창을 넓히는 시도의 근거가 된다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 락 없음. bus->devices 순회의 안전성은
 *   상위가 보장한다.
 *
 * 호출 체인:
 *   __pci_bus_size_bridges() -> [이 함수]
 *     -> pbus_select_window_for_type(), pci_min_window_alignment(),
 *        get_res_add_size(), calculate_iosize(), pci_dev_res_add_to_list()
 */
static void pbus_size_io(struct pci_bus *bus, resource_size_t add_size,
			 struct list_head *realloc_head)
{
	/* [한국어] 버스 위 장치를 훑을 커서. */
	struct pci_dev *dev;
	/* [한국어] 크기를 정할 대상 창. 종류(IORESOURCE_IO)만 주고 고르게 한다. */
	struct resource *b_res = pbus_select_window_for_type(bus, IORESOURCE_IO);
	/* [한국어] size 는 1KB 미만 자원들의 합(ISA 보정 대상),
	 * size0 은 필수 크기 결과, size1 은 선택분까지 포함한 결과.
	 * size1 은 루프 안에서 "1KB 이상 자원들의 합"으로도 재사용된다 —
	 * calculate_iosize() 에 넘긴 뒤 결과로 덮어쓰는 방식이다. */
	resource_size_t size = 0, size0 = 0, size1 = 0;
	/* [한국어] 자식들이 각자 요구한 선택적 추가 크기의 합. */
	resource_size_t children_add_size = 0;
	/* [한국어] min_align 은 이 창이 지켜야 할 최종 정렬,
	 * align 은 자식 자원 하나의 정렬을 담는 임시 변수. */
	resource_size_t min_align, align;

	/* [한국어] 이 버스에 I/O 창 자체가 없으면(브리지가 I/O 를 지원하지
	 * 않거나 루트 버스에 I/O aperture 가 없으면) 계산할 것이 없다. */
	if (!b_res)
		return;

	/* If resource is already assigned, nothing more to do */
	/* [한국어] 이미 주소가 확정된 창은 크기를 다시 정할 수 없다. 펌웨어의
	 * 배치를 claim 한 경우가 대표적이다. */
	if (resource_assigned(b_res))
		return;

	/* [한국어] 창 세분성에서 출발한다(4KB 또는 1KB). 아래 루프에서
	 * 자식 자원의 정렬이 더 크면 이 값이 올라간다. */
	min_align = pci_min_window_alignment(bus, IORESOURCE_IO);
	/* [한국어] 이 버스에 직접 달린 장치를 모두 훑는다. 하위 버스는 이미
	 * 재귀로 처리되어 그 결과가 브리지 창 자원에 반영돼 있으므로,
	 * 여기서는 그 브리지의 창도 "이 버스의 자원 하나"로 함께 세어진다. */
	list_for_each_entry(dev, &bus->devices, bus_list) {
		/* [한국어] 장치의 자원을 훑을 커서. */
		struct resource *r;

		/* [한국어] resource[] 전체 순회(BAR, ROM, VF BAR, 브리지 창). */
		pci_dev_for_each_resource(dev, r) {
			/* [한국어] 이 자원의 크기를 담을 자리. */
			unsigned long r_size;

			/* [한국어] 이미 주소가 잡힌 자원은 새 창 크기 계산에
			 * 포함하지 않고, I/O 가 아닌 자원도 이 창과 무관하다. */
			if (resource_assigned(r) || !(r->flags & IORESOURCE_IO))
				continue;

			/* [한국어] 빈 슬롯이나 꺼진 브리지 창은 제외한다.
			 * 여기서 pdev_resource_should_fit() 대신 더 느슨한
			 * pdev_resource_assignable() 을 쓰는 점에 유의 —
			 * 고정 주소(IORESOURCE_PCI_FIXED) 자원도 이 창 안에
			 * 들어와야 하므로 크기에는 세어 주어야 한다. */
			if (!pdev_resource_assignable(dev, r))
				continue;

			/* [한국어] 이 자원이 요구하는 바이트 수. */
			r_size = resource_size(r);
			/* [한국어] 1KB 미만이면 ISA 별칭 구간에 걸릴 수 있어
			 * 별도로 모은다. calculate_iosize() 가 이 몫만 4배로
			 * 부풀린다(CONFIG_ISA/EISA 커널에서). */
			if (r_size < SZ_1K)
				/* Might be re-aligned for ISA */
				size += r_size;
			else
				/* [한국어] 1KB 이상은 별칭 구간 밖이라 보정
				 * 없이 그대로 더한다. */
				size1 += r_size;

			/* [한국어] 이 자식 자원의 정렬 요구. */
			align = pci_resource_alignment(dev, r);
			/* [한국어] 창은 가장 엄격한 자식의 정렬을 만족해야 한다.
			 * 그래야 창 시작점부터 그 자식을 놓을 수 있다. */
			if (align > min_align)
				min_align = align;

			/* [한국어] 선택적 요구 목록이 있으면, 이 자식이 요구한
			 * 추가 크기도 모아 둔다. 자식이 브리지라면 그 아래
			 * 계층이 원하는 예비 공간이 여기로 올라온다. */
			if (realloc_head)
				children_add_size += get_res_add_size(realloc_head, r);
		}
	}

	/* [한국어] 필수 크기 size0 계산. min_size 자리에 삼항 연산자가 있는
	 * 것이 요점이다 — realloc_head 가 있으면 add_size 를 여기 넣지 않고
	 * 0 을 넣어 "필수는 자식들이 실제로 요구한 만큼만"으로 잡는다.
	 * add_size 는 아래 size1 계산에서 선택분으로 따로 반영된다.
	 * realloc_head 가 없으면 선택분을 미룰 곳이 없으므로 지금 필수에
	 * 포함시킨다. add_size 와 children_add_size 자리에 0 을 넘기는 것도
	 * 같은 이유다(이 계산은 "필수만"을 구한다). */
	size0 = calculate_iosize(size, realloc_head ? 0 : add_size, size1, 0, 0,
			resource_size(b_res), min_align);

	/* [한국어] 필수 크기가 0 이 아니면 이 창은 실제로 쓰인다. 이전 라운드에
	 * 켜 두었을지 모를 "꺼짐" 표시를 지운다. */
	if (size0)
		b_res->flags &= ~IORESOURCE_DISABLED;

	/* [한국어] 기본값은 "선택분이 없다" = size1 == size0. 아래 조건이
	 * 맞을 때만 더 큰 값으로 다시 계산한다. */
	size1 = size0;
	/* [한국어] 선택분을 실을 목록이 있고, 실을 것도 있을 때만 이상적 크기를
	 * 따로 구한다. 둘 중 하나라도 없으면 계산 자체가 낭비다. */
	if (realloc_head && (add_size > 0 || children_add_size > 0)) {
		/* [한국어] 이번에는 add_size 와 children_add_size 를 제자리에
		 * 넣어 "필수 + 선택" 크기를 구한다. min_size 자리에는 0 을
		 * 넣는데, 최소 보장은 이미 size0 계산에서 다뤘기 때문이다.
		 * 세 번째 인자 size1 은 아직 "1KB 이상 자원들의 합"을 담고
		 * 있으며, 이 호출 결과로 덮어써진다. */
		size1 = calculate_iosize(size, 0, size1, add_size,
					 children_add_size, resource_size(b_res),
					 min_align);
	}

	/* [한국어] 필수도 0 이고 이상적 크기도 0 이면, 이 창 아래에 I/O 를
	 * 쓰는 장치가 하나도 없다는 뜻이다. */
	if (!size0 && !size1) {
		/* [한국어] 예전에 무언가 값이 들어 있던 창을 지금 끄는 경우에만
		 * 로그를 남긴다. 처음부터 비어 있던 창까지 알리면 부팅 로그가
		 * 의미 없이 길어진다. bus->self 검사는 루트 버스(브리지 없음)에서
		 * NULL 역참조를 막는다. */
		if (bus->self && (b_res->start || b_res->end))
			pci_info(bus->self, "disabling bridge window %pR to %pR (unused)\n",
				 b_res, &bus->busn_res);
		/* [한국어] 창을 꺼진 것으로 표시한다. 이후 배치 코드
		 * (pdev_resource_assignable)가 이 창을 건너뛰고,
		 * pci_setup_bridge_io() 가 base > limit 를 써서 실제로 닫는다. */
		b_res->flags |= IORESOURCE_DISABLED;
		return;
	}

	/* [한국어] 창에 "필수" 크기와 요구 정렬을 기록한다. 아직 주소는 없으므로
	 * 첫 인자(정렬)가 start 필드에 들어가고 두 번째가 크기가 된다.
	 * 여기서 size1 이 아니라 size0 을 쓰는 것이 중요하다 — 일단 최소한으로
	 * 잡아 두어야 배치 성공 가능성이 높고, 나머지는 아래에서 선택분으로
	 * 등록해 나중에 시도한다. */
	resource_set_range(b_res, min_align, size0);
	/* [한국어] 이 창은 "시작 주소를 정렬 값에 맞추는" 방식임을 표시한다.
	 * 일반 BAR 의 SIZEALIGN(크기 자체가 정렬 단위)과 구분되며,
	 * __assign_resources_sorted() 가 이 비트를 보고 정렬 보정 여부를
	 * 판단한다. */
	b_res->flags |= IORESOURCE_STARTALIGN;
	/* [한국어] 선택분을 등록할 조건: 브리지가 실제로 있고(루트 버스가
	 * 아니고), 이상적 크기가 필수보다 크며, 실을 목록이 있을 때. */
	if (bus->self && size1 > size0 && realloc_head) {
		/* [한국어] 선택분이 있다는 것은 이 창을 쓸 계획이 있다는 뜻이니
		 * 꺼짐 표시를 지운다(size0 이 0 이어서 위에서 못 지웠을 수 있다). */
		b_res->flags &= ~IORESOURCE_DISABLED;
		/* [한국어] 차액(size1 - size0)을 add_size 로, 계산된 정렬을
		 * min_align 으로 실어 선택적 요구 목록에 등록한다. 소유자로
		 * bus->self 를 넘기는 이유는 이 창이 상위 브리지의 자원이기
		 * 때문이다. */
		pci_dev_res_add_to_list(realloc_head, bus->self, b_res,
					size1 - size0, min_align);
		/* [한국어] 얼마를 예비로 요청했는지 로그에 남긴다. 핫플러그로
		 * 장치를 꽂았을 때 왜 공간이 있었는지(또는 없었는지)를 나중에
		 * 추적하는 단서가 된다. resource_size_t 폭이 아키텍처마다 달라
		 * %llx 로 찍기 위해 unsigned long long 으로 캐스팅한다. */
		pci_info(bus->self, "bridge window %pR to %pR add_size %llx\n",
			 b_res, &bus->busn_res,
			 (unsigned long long) size1 - size0);
	}
}

/*
 * [한국어]
 * calculate_mem_align - aligns[] 히스토그램에서 창의 최소 정렬을 유도한다
 *
 * @aligns: 정렬 등급별 크기 합계 배열. aligns[k] 에는 "정렬이 2^k MB 인
 *          자원들의 크기 합"이 들어 있다(pbus_size_mem 이 채운다).
 * @max_order: 실제로 값이 들어 있는 가장 높은 등급.
 * @return: 이 창이 가져야 할 최소 정렬.
 *
 * 왜 필요한가: 창의 정렬은 그 안에 담길 자원 중 가장 엄격한 것을 만족해야
 * 하지만, 무작정 가장 큰 정렬을 쓰면 창이 필요 이상으로 큰 경계로 밀려
 * 주소 공간이 낭비된다. 작은 자원들이 앞쪽 자투리를 채워 줄 수 있다면 더
 * 작은 정렬로도 큰 자원을 올바른 경계에 놓을 수 있다. 이 함수는 등급을
 * 낮은 쪽부터 훑으며 "지금까지 쌓인 작은 자원들의 총량으로 다음 등급의
 * 경계까지 밀어 올릴 수 있는가"를 따져 그 판단을 한다.
 *
 * 주의 — 이 함수는 이 파일 안에서 호출되는 곳이 없다. 같은 aligns[]
 * 히스토그램을 쓰는 pbus_size_mem() 은 대신 calculate_head_align() 을
 * 부른다. 왜 남아 있는지(이후 커밋에서 지워질 잔재인지, 다른 목적이
 * 있는지)는 이 트리의 정보만으로는 확인할 수 없다. static inline 이라
 * 호출부가 없으면 코드가 생성되지 않아 빌드 경고도 나지 않는다.
 * 실행 컨텍스트: 순수 계산.
 *
 * 호출 체인:
 *   (이 트리 안에 호출부 없음) -> [이 함수]
 */
static inline resource_size_t calculate_mem_align(resource_size_t *aligns,
						  int max_order)
{
	/* [한국어] 지금까지 훑은 등급들의 크기 누적합. */
	resource_size_t align = 0;
	/* [한국어] 현재까지 결정된 최소 정렬(반환값이 된다). */
	resource_size_t min_align = 0;
	/* [한국어] 등급 순회 인덱스. */
	int order;

	/* [한국어] 낮은 등급(작은 정렬)부터 차례로 올라간다. */
	for (order = 0; order <= max_order; order++) {
		/* [한국어] 이 등급에 해당하는 실제 정렬 값을 만들 자리. */
		resource_size_t align1 = 1;

		/* [한국어] __ffs(SZ_1M) 은 1MB 의 최하위 세워진 비트 위치,
		 * 즉 20 이다. 거기에 order 를 더해 왼쪽 시프트하면
		 * align1 = 2^order MB 가 된다. aligns[0] 이 1MB 등급인 이유가
		 * 여기 있다 — 브리지 메모리 창의 세분성이 1MB 이므로 그보다
		 * 작은 정렬은 구분할 의미가 없다. */
		align1 <<= order + __ffs(SZ_1M);

		/* [한국어] 아직 아무것도 쌓이지 않았다면(누적합 0), 이 등급의
		 * 정렬을 그대로 최소 정렬로 삼는다. */
		if (!align)
			min_align = align1;
		/* [한국어] 쌓인 자원들을 현재 최소 정렬로 올림해도 이 등급의
		 * 경계에 못 미친다면, 그 사이를 메울 자원이 부족하다는 뜻이다.
		 * 그때는 이 등급 정렬의 절반을 최소 정렬로 삼아 자투리가
		 * 남지 않게 한다. */
		else if (ALIGN(align + min_align, min_align) < align1)
			min_align = align1 >> 1;
		/* [한국어] 이 등급의 크기 합을 누적한다. 다음 등급 판단의
		 * "메울 수 있는 양"이 된다. */
		align += aligns[order];
	}

	/* [한국어] 모든 등급을 훑은 뒤의 최소 정렬. */
	return min_align;
}

/*
 * Calculate bridge window head alignment that leaves no gaps in between
 * resources.
 */
/*
 * [한국어]
 * calculate_head_align - 창 앞머리에 빈틈이 남지 않는 최소 정렬을 구한다
 *
 * @aligns: 정렬 등급별 크기 합계 히스토그램. aligns[k] = 정렬이 2^k MB 인
 *          자원들의 크기 합.
 * @max_order: 값이 들어 있는 가장 높은 등급 = 가장 엄격한 자원의 정렬 등급.
 * @return: 창이 시작해야 할 정렬. 항상 2의 거듭제곱이고 1MB 이상이다.
 *
 * 왜 필요한가 — 이 파일에서 가장 미묘한 계산이다.
 * 창을 가장 큰 자원의 정렬(2^max_order MB)에 맞춰 시작하면 그 자원은 창
 * 첫머리에 딱 들어간다. 하지만 그렇게 하면 창 자체가 큰 경계로 밀려
 * 상위 창 안에서 자리를 찾기 어려워진다.
 * 반대로 창 정렬을 낮추면 창은 놓기 쉬워지지만, 큰 자원은 여전히 자기
 * 정렬을 지켜야 하므로 창 시작점과 그 자원 사이에 빈틈(head room)이 생긴다.
 * 그 빈틈을 작은 자원들로 채울 수 있다면 낭비가 아니다.
 * 이 함수는 "가장 큰 정렬에서 출발해, 작은 자원들의 총량이 그 빈틈을 메울
 * 만큼 충분하면 정렬을 절반씩 낮춘다"를 반복해 그 균형점을 찾는다.
 * 바로 아래 원문 주석("leaves no gaps in between resources")이 목표를
 * 요약하고 있다.
 *
 * 왜 remainder >= head_align / 2 인가: 정렬을 절반으로 낮추면 큰 자원 앞에
 * 최대 (새 정렬)만큼의 빈틈이 생길 수 있다. 그 빈틈을 메우려면 작은
 * 자원들의 총량이 그만큼 있어야 한다. head_align / 2 가 곧 낮춘 뒤의
 * 정렬이므로 그 값과 비교하는 것이다.
 * 실행 컨텍스트: 순수 계산. 부작용 없음.
 *
 * 호출 체인:
 *   pbus_size_mem() -> [이 함수]
 */
static resource_size_t calculate_head_align(resource_size_t *aligns,
					    int max_order)
{
	/* [한국어] 결과가 될 창 시작 정렬. 아래에서 가장 큰 정렬로 초기화된다. */
	resource_size_t head_align = 1;
	/* [한국어] "빈틈을 메우는 데 쓸 수 있는" 작은 자원들의 누적 크기. */
	resource_size_t remainder = 0;
	/* [한국어] 등급 순회 인덱스. */
	int order;

	/* Take the largest alignment as the starting point. */
	/* [한국어] 가장 엄격한 자원의 정렬에서 출발한다. __ffs(SZ_1M) = 20
	 * 이므로 head_align = 2^max_order MB 가 된다. 여기서 시작해 아래에서
	 * 조건이 허락하는 만큼 낮춰 간다. */
	head_align <<= max_order + __ffs(SZ_1M);

	/* [한국어] 최상위 바로 아래 등급부터 낮은 쪽으로 훑는다. 최상위
	 * 자신은 "메우는 쪽"이 아니라 "메워져야 할 대상"이라 제외한다. */
	for (order = max_order - 1; order >= 0; order--) {
		/* [한국어] 이 등급의 실제 정렬 값을 만들 자리. */
		resource_size_t align1 = 1;

		/* [한국어] align1 = 2^order MB. 위와 같은 방식이다. */
		align1 <<= order + __ffs(SZ_1M);

		/*
		 * Account smaller resources with alignment < max_order that
		 * could be used to fill head room if alignment less than
		 * max_order is used.
		 */
		/* [한국어] 이 등급의 크기 합을 "메울 수 있는 양"에 더한다.
		 * 낮은 등급으로 내려갈수록 재료가 쌓인다. */
		remainder += aligns[order];

		/*
		 * Test if head fill is enough to satisfy the alignment of
		 * the larger resources after reducing the alignment.
		 */
		/* [한국어] 두 조건이 함께 참인 동안 정렬을 절반씩 낮춘다.
		 *  - head_align > align1: 이 등급의 정렬까지만 낮출 수 있다.
		 *    그 아래로 내려가는 것은 이 등급 자원들이 허락하지 않는다.
		 *  - remainder >= head_align / 2: 절반으로 낮췄을 때 생길
		 *    빈틈(= 새 정렬 크기)을 메울 재료가 충분하다.
		 * while 인 이유는 재료가 넉넉하면 한 번에 여러 단계를 낮출 수
		 * 있기 때문이다. */
		while ((head_align > align1) && (remainder >= head_align / 2)) {
			/* [한국어] 한 단계 낮춘다(2의 거듭제곱이므로 나누기 2). */
			head_align /= 2;
			/* [한국어] 낮추면서 생긴 빈틈만큼 재료를 소모한다.
			 * 이 시점의 head_align 이 이미 낮춘 값이므로, 소모량이
			 * 정확히 새로 생긴 빈틈 크기와 같다. */
			remainder -= head_align;
		}
	}

	/* [한국어] 더 낮출 수 없는 지점의 정렬이 답이다. 최소 1MB 는
	 * 보장된다(max_order >= 0 이면 초기값이 이미 1MB 이상이고,
	 * align1 도 1MB 이상이라 그 아래로 내려가지 않는다). */
	return head_align;
}

/*
 * pbus_size_mem_optional - Account optional resources in bridge window
 *
 * Account an optional resource or the optional part of the resource in bridge
 * window size.
 *
 * Return: %true if the resource is entirely optional.
 */
/* [한국어] 아래 함수의 한국어 해설 (원문 주석은 그 아래 그대로 둔다):
 * pbus_size_mem_optional - 자원의 선택적 몫을 창 크기 계산에서 분리해 낸다
 *
 * @dev: 자원의 소유 장치.
 * @resno: resource[] 인덱스.
 * @align: 이 자원의 정렬 요구(호출자가 이미 계산해 넘긴다).
 * @realloc_head: 선택적 요구를 실을 목록. NULL 이면 선택/필수를 구분할 수
 *        없으므로 곧바로 false 를 돌려 "전부 필수"로 다루게 한다.
 * @add_align: [출력] 선택적 몫까지 반영했을 때 창이 지켜야 할 정렬.
 *        호출자의 누적 변수를 가리키며, 이 함수가 max 로 갱신한다.
 * @children_add_size: [출력] 선택적 몫의 누적 크기. 마찬가지로 누적한다.
 * @return: true 면 "이 자원은 통째로 선택적이니 필수 크기 계산에서 빼라",
 *        false 면 "필수 크기에 포함시켜라". 호출자 pbus_size_mem() 은
 *        true 면 continue 로 그 자원을 건너뛴다.
 *
 * 왜 필요한가: 창 크기 계산은 "필수만"과 "필수+선택" 두 값을 동시에
 * 만들어야 한다. 자원마다 그 둘 중 어디에 얼마씩 들어가는지를 판정하는
 * 부분을 떼어 낸 것이 이 함수다. 두 종류의 선택적 몫을 다룬다:
 *   (1) 브리지 창의 "부분적" 선택 몫 — 창 자체는 필수지만 그 안에 핫플러그
 *       예비 공간이 얹혀 있다. 그 얹힌 몫만 선택으로 뽑아내고 창 본체는
 *       필수 계산에 남긴다(false 반환).
 *   (2) 통째로 선택적인 자원 — SR-IOV VF BAR, 비활성 ROM. 크기 전부가
 *       선택 몫이므로 필수 계산에서 빼고(true 반환) 목록에 등록한다.
 * 실행 컨텍스트: 프로세스 문맥(등록 시 kzalloc). 락 없음.
 *
 * 호출 체인:
 *   pbus_size_mem() -> [이 함수]
 *     -> pci_resource_is_optional(), res_to_dev_res(), pci_dev_res_add_to_list()
 */
static bool pbus_size_mem_optional(struct pci_dev *dev, int resno,
				   resource_size_t align,
				   struct list_head *realloc_head,
				   resource_size_t *add_align,
				   resource_size_t *children_add_size)
{
	/* [한국어] 인덱스로 대상 자원을 집는다. */
	struct resource *res = pci_resource_n(dev, resno);
	/* [한국어] 이 자원이 통째로 선택적인지 미리 판정해 둔다. */
	bool optional = pci_resource_is_optional(dev, resno);
	/* [한국어] 이 자원이 요구하는 크기. 통째로 선택적일 때 그대로
	 * children_add_size 에 더해진다. */
	resource_size_t r_size = resource_size(res);
	/* [한국어] 이 자원이 이미 realloc_head 에 등록돼 있으면 그 래퍼.
	 * NULL 초기화가 중요하다 — 아래에서 브리지 창이 아닌 경우 검색을
	 * 건너뛰므로, 그때도 "등록돼 있지 않다"는 뜻으로 남아야 한다. */
	struct pci_dev_resource *dev_res = NULL;

	/* [한국어] 선택분을 실을 곳이 없으면 구분 자체가 무의미하다.
	 * false 를 돌려 모든 자원을 필수로 세게 한다. */
	if (!realloc_head)
		return false;

	/*
	 * Only bridges have optional sizes in realloc_head at this
	 * point. As res_to_dev_res() walks the entire realloc_head
	 * list, skip calling it when known unnecessary.
	 */
	/* [한국어] 위 원문 주석이 밝히듯, 이 시점에 realloc_head 에 올라와
	 * 있을 수 있는 것은 브리지 창뿐이다(하위 버스를 재귀로 먼저 처리하며
	 * pbus_size_io/pbus_size_mem 이 등록해 둔 것들). 브리지 창이 아니면
	 * O(n) 검색을 아예 하지 않는 것이 이 조건의 목적이다. */
	if (pci_resource_is_bridge_win(resno)) {
		/* [한국어] 이 창에 걸린 선택적 요구가 있는지 찾는다. */
		dev_res = res_to_dev_res(realloc_head, res);
		if (dev_res) {
			/* [한국어] 하위 계층이 요구한 예비 크기를 이 창의
			 * 선택 몫 누적에 더한다. 이렇게 아래에서 위로 예비
			 * 공간 요구가 전파된다. */
			*children_add_size += dev_res->add_size;
			/* [한국어] 그 요구에 딸린 정렬도 반영한다. 정렬은
			 * 항상 더 엄격한 쪽을 따라야 하므로 max 다. */
			*add_align = max(*add_align, dev_res->min_align);
		}
	}

	/* [한국어] 여기까지 왔는데 통째로 선택적인 자원이 아니라면, 위에서
	 * 뽑아낸 "부분적 선택 몫"만 반영하고 자원 본체는 필수 계산에 남긴다. */
	if (!optional)
		return false;

	/*
	 * Put requested res to the optional list if not there yet (SR-IOV,
	 * disabled ROM). Bridge windows with an optional part are already
	 * on the list.
	 */
	/* [한국어] 아직 목록에 없으면 새로 등록한다. add_size 로 0 을 넘기는
	 * 것이 요점이다 — 이 자원은 "크기를 더 얹어 달라"가 아니라 "존재
	 * 자체가 선택적"이라, 크기는 아래 children_add_size 누적으로 표현되고
	 * 목록에는 정렬 요구만 실어 두면 되기 때문이다. 위 원문 주석이
	 * 밝히듯 SR-IOV 와 비활성 ROM 이 여기로 오고, 부분적 선택 몫을 가진
	 * 브리지 창은 이미 등록돼 있어 dev_res 가 NULL 이 아니다. */
	if (!dev_res)
		pci_dev_res_add_to_list(realloc_head, dev, res, 0, align);
	/* [한국어] 이 자원의 크기 전체를 선택 몫으로 누적한다. 호출자는
	 * true 를 받고 이 자원을 필수 계산에서 건너뛰므로 중복 계산이 없다. */
	*children_add_size += r_size;
	/* [한국어] 선택 몫까지 반영했을 때의 정렬 요구도 갱신한다. */
	*add_align = max(align, *add_align);

	/* [한국어] "통째로 선택적" — 호출자는 이 자원을 필수 크기 계산에서
	 * 제외해야 한다. */
	return true;
}

/**
 * pbus_size_mem() - Size the memory window of a given bus
 *
 * @bus:		The bus
 * @b_res:		The bridge window resource
 * @add_size:		Additional memory window
 * @realloc_head:	Track the additional memory window on this list
 *
 * Calculate the size of the bridge window @b_res and minimal alignment
 * which guarantees that all child resources fit in this size.
 *
 * Set the bus resource start/end to indicate the required size if there an
 * available unassigned bus resource of the desired @type.
 *
 * Add optional resource requests to the @realloc_head list if it is
 * supplied.
 */
/* [한국어] 아래 함수의 한국어 해설 (원문 kernel-doc 은 그 아래 그대로 둔다):
 * pbus_size_mem - 한 버스의 메모리 창 크기와 정렬을 계산한다
 *
 * @bus: 크기를 정할 버스.
 * @b_res: 대상 창 자원. 호출자가 미리 골라 넘긴다 — 같은 버스에 대해
 *        prefetchable 창과 non-prefetchable 창을 각각 한 번씩 부르기 때문에
 *        pbus_size_io() 와 달리 창을 인자로 받는다.
 * @add_size: 이 창에 얹고 싶은 선택적 예비 크기(핫플러그면
 *        pci_hotplug_mmio_size 또는 pci_hotplug_mmio_pref_size).
 * @realloc_head: 선택적 요구를 실을 목록. NULL 이면 add_size 를 필수로 본다.
 * @return: 없음. 결과는 b_res 와 realloc_head 에 기록된다.
 *
 * 왜 I/O 판보다 복잡한가 — aligns[] 히스토그램이 핵심이다.
 * 메모리 자원은 크기가 제각각이고 정렬 요구도 그만큼 다양하다. 창 정렬을
 * 어떻게 잡느냐에 따라 낭비가 크게 달라지므로, "정렬 등급별로 얼마나 있는가"
 * 를 히스토그램으로 모아 calculate_head_align() 에 넘긴다. aligns[k] 에는
 * 정렬이 2^k MB 인 자원들의 크기 합이 들어간다.
 *
 * 크기 누적에서 max(r_size, align) 을 쓰는 이유: 자원이 자기 정렬보다 작으면
 * (예: 정렬 1MB 인데 크기 64KB) 실제로는 정렬 단위만큼의 공간을 차지하는
 * 셈이다. 그 낭비까지 창 크기에 반영해야 실제로 다 들어간다.
 * 반대로 크기가 정렬보다 큰 자원은 히스토그램에 넣지 않는데(아래 조건
 * r_size <= align), 그런 자원은 앞머리 빈틈을 메우는 재료가 될 수 없기
 * 때문이다 — 자기 정렬 경계에서 시작해 그 너머로 뻗어 나간다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 락 없음.
 *
 * 호출 체인:
 *   __pci_bus_size_bridges() -> [이 함수]
 *     -> pbus_select_window(), pbus_size_mem_optional(),
 *        calculate_head_align(), calculate_memsize(), pci_dev_res_add_to_list()
 */
static void pbus_size_mem(struct pci_bus *bus, struct resource *b_res,
			  resource_size_t add_size,
			  struct list_head *realloc_head)
{
	/* [한국어] 버스 위 장치를 훑을 커서. */
	struct pci_dev *dev;
	/* [한국어] min_align 은 히스토그램에서 유도한 창 정렬,
	 * win_align 은 스펙/플랫폼이 요구하는 창 세분성,
	 * align 은 자식 자원 하나의 정렬을 담는 임시 변수,
	 * size 는 필수 크기 누적합, size0/size1 은 각각 필수/이상적 최종 크기. */
	resource_size_t min_align, win_align, align, size, size0, size1 = 0;
	/* [한국어] 정렬 등급별 크기 합계 히스토그램. 인덱스 k 는 정렬 2^k MB
	 * 를 뜻하므로 28칸이면 1MB(2^0 MB)부터 128TB(2^27 MB)까지 덮는다 —
	 * 오른쪽 원문 주석이 그 범위를 밝히고 있다. {} 로 전부 0 초기화한다. */
	resource_size_t aligns[28] = {}; /* Alignments from 1MB to 128TB */
	/* [한국어] order 는 현재 자원의 정렬 등급, max_order 는 등장한 가장
	 * 높은 등급(= 가장 엄격한 자원). */
	int order, max_order;
	/* [한국어] 선택적 몫의 누적 크기(pbus_size_mem_optional 이 채운다). */
	resource_size_t children_add_size = 0;
	/* [한국어] 선택적 몫까지 반영했을 때의 정렬 요구(같은 함수가 채운다). */
	resource_size_t add_align = 0;

	/* [한국어] 호출자가 넘긴 창이 없으면(그 종류의 창이 이 버스에 없으면)
	 * 계산할 것이 없다. */
	if (!b_res)
		return;

	/* If resource is already assigned, nothing more to do */
	/* [한국어] 이미 주소가 확정된 창은 크기를 다시 정할 수 없다. */
	if (resource_assigned(b_res))
		return;

	/* [한국어] 히스토그램의 최고 등급 초기화. 자원이 하나도 없으면
	 * 0 인 채로 calculate_head_align() 에 넘어가 1MB 를 돌려받는다. */
	max_order = 0;
	/* [한국어] 필수 크기 누적합 초기화. */
	size = 0;

	/* [한국어] 이 버스에 직접 달린 장치를 모두 훑는다. 하위 브리지의 창도
	 * "이 버스에 있는 자원 하나"로 여기서 함께 세어진다 — 하위 버스는
	 * 이미 재귀로 크기가 정해져 그 브리지 창 자원에 반영돼 있다. */
	list_for_each_entry(dev, &bus->devices, bus_list) {
		/* [한국어] 자원 순회 커서. */
		struct resource *r;
		/* [한국어] 그 자원의 resource[] 인덱스. */
		int i;

		/* [한국어] 장치의 resource[] 전체 순회. */
		pci_dev_for_each_resource(dev, r, i) {
			/* [한국어] 경고 로그용 이름. */
			const char *r_name = pci_resource_name(dev, i);
			/* [한국어] 이 자원이 요구하는 크기. */
			resource_size_t r_size;

			/* [한국어] 장치 단위 문지기와 자원 단위 문지기를 함께
			 * 적용한다. 장치 문지기를 루프 밖으로 빼지 않은 것은
			 * 이 파일의 선택이며, 결과는 같다. */
			if (!pdev_resources_assignable(dev) ||
			    !pdev_resource_should_fit(dev, r))
				continue;
			/* [한국어] 이 자원이 지금 계산 중인 창에 담길 것이
			 * 맞는지 확인한다. 같은 버스라도 자원마다 갈 창이
			 * 다르므로(prefetchable 규칙), 다른 창에 갈 자원을
			 * 여기서 세면 창이 부풀려진다. */
			if (b_res != pbus_select_window(bus, r))
				continue;

			/* [한국어] 이 자원의 정렬 요구. 일반 BAR 는 자기
			 * 크기와 같고, 하위 브리지 창이면 그 창의 요구 정렬이다. */
			align = pci_resource_alignment(dev, r);
			/*
			 * aligns[0] is for 1MB (since bridge memory
			 * windows are always at least 1MB aligned), so
			 * keep "order" from being negative for smaller
			 * resources.
			 */
			/* [한국어] 정렬 값을 히스토그램 등급으로 바꾼다.
			 * __ffs(align) 은 2의 거듭제곱 align 의 지수이고,
			 * __ffs(SZ_1M) 은 20 이다. 그 차이가 "1MB 의 몇 제곱배인가"
			 * = 등급이 된다. max_t(int, ..., 0) 으로 하한을 0 에
			 * 거는 이유는 위 원문 주석이 밝히듯 1MB 보다 작은 자원의
			 * 등급이 음수가 되어 배열 밖을 짚는 것을 막기 위해서다
			 * (브리지 메모리 창은 어차피 1MB 단위라 그 아래는
			 * 구분할 의미가 없다). */
			order = max_t(int, __ffs(align) - __ffs(SZ_1M), 0);
			/* [한국어] 128TB 를 넘는 정렬은 히스토그램에 담을 칸이
			 * 없다. 실제 하드웨어에서는 나올 수 없는 값이라 손상된
			 * BAR 나 잘못된 quirk 를 의심해야 한다. */
			if (order >= ARRAY_SIZE(aligns)) {
				/* [한국어] 이유를 남긴다. %#llx 의 # 는
				 * 0x 접두사를 붙이라는 지정이다. */
				pci_warn(dev, "%s %pR: disabling; bad alignment %#llx\n",
					 r_name, r, (unsigned long long) align);
				/* [한국어] 이 자원을 무효화한다. 이후 모든
				 * 문지기(pdev_resource_assignable 등)가
				 * flags == 0 을 보고 건너뛴다. 배열 밖을
				 * 짚느니 이 자원을 포기하는 편이 안전하다. */
				r->flags = 0;
				continue;
			}

			/* [한국어] 선택적 몫을 분리한다. 이 호출이
			 * add_align 과 children_add_size 를 갱신하고,
			 * 자원이 통째로 선택적이면 true 를 돌려준다. */
			if (pbus_size_mem_optional(dev, i, align,
						   realloc_head, &add_align,
						   &children_add_size))
				/* [한국어] 통째로 선택적인 자원은 필수 크기와
				 * 히스토그램에서 모두 제외한다. 필수 창 안에
				 * 자리를 마련해 줄 이유가 없기 때문이다. */
				continue;

			/* [한국어] 이 자원이 요구하는 크기. */
			r_size = resource_size(r);
			/* [한국어] 필수 크기에 누적한다. max(r_size, align) 인
			 * 이유: 크기가 정렬보다 작으면 실제로는 정렬 단위만큼의
			 * 공간을 차지하게 된다(다음 자원이 그 정렬 경계 뒤에서만
			 * 시작할 수 있으므로). 그 낭비까지 세어야 창이 실제로
			 * 모두를 담을 수 있다. */
			size += max(r_size, align);

			/*
			 * If resource's size is larger than its alignment,
			 * some configurations result in an unwanted gap in
			 * the head space that the larger resource cannot
			 * fill.
			 */
			/* [한국어] 히스토그램에는 "크기가 정렬 이하인" 자원만
			 * 넣는다. 위 원문 주석이 그 이유를 설명한다 — 크기가
			 * 정렬보다 큰 자원은 자기 정렬 경계에서 시작해 그
			 * 너머로 뻗으므로, 앞머리 빈틈을 메우는 재료가 될 수
			 * 없다. 더하는 값이 r_size 가 아니라 align 인 것도
			 * 같은 맥락으로, 그 자원이 실제로 점유하는 공간이
			 * 정렬 단위이기 때문이다. */
			if (r_size <= align)
				aligns[order] += align;
			/* [한국어] 가장 엄격한 등급을 기억해 둔다.
			 * calculate_head_align() 의 출발점이 된다. */
			if (order > max_order)
				max_order = order;
		}
	}

	/* [한국어] 이 창 종류가 지켜야 할 스펙/플랫폼 세분성(메모리는 1MB 이상).
	 * b_res->flags 를 그대로 넘겨 IO/MEM 판정을 맡긴다. */
	win_align = pci_min_window_alignment(bus, b_res->flags);
	/* [한국어] 히스토그램에서 "빈틈이 남지 않는" 창 정렬을 유도한다. */
	min_align = calculate_head_align(aligns, max_order);
	/* [한국어] 유도한 값이 세분성보다 느슨할 수 있으므로(자원이 하나도
	 * 없거나 전부 작은 경우) 더 엄격한 쪽을 택한다. 창은 어떤 경우에도
	 * 하드웨어 세분성 아래로 내려갈 수 없다. */
	min_align = max(min_align, win_align);
	/* [한국어] 필수 크기 계산. I/O 판과 같은 이유로, realloc_head 가
	 * 있으면 add_size 를 여기 넣지 않고 0 을 넣는다(선택분은 아래에서
	 * 따로 반영). children_add_size 자리에도 0 을 넣어 "필수만"을 구한다.
	 * 정렬은 min_align 이 아니라 win_align 을 쓰는 점에 유의 — 크기는
	 * 하드웨어 세분성 단위로만 표현되면 충분하고, 더 큰 min_align 으로
	 * 크기까지 올림하면 불필요하게 부풀기 때문이다. */
	size0 = calculate_memsize(size, realloc_head ? 0 : add_size,
				  0, win_align);

	/* [한국어] 필수 크기가 0 이 아니면 창을 실제로 쓴다. */
	if (size0) {
		/* [한국어] 요구 정렬과 필수 크기를 창에 기록한다. 아직 주소가
		 * 없으므로 첫 인자(min_align)가 start 자리에 들어간다.
		 * 아래에서 한 번 더 같은 호출이 나오는데, 여기서 먼저 쓰는
		 * 이유는 이 블록이 DISABLED 해제와 짝을 이루기 때문이다. */
		resource_set_range(b_res, min_align, size0);
		/* [한국어] 이전 라운드의 "꺼짐" 표시를 지운다. */
		b_res->flags &= ~IORESOURCE_DISABLED;
	}

	/* [한국어] 선택분을 실을 곳이 있고 실을 것도 있을 때만 이상적 크기를
	 * 따로 구한다. */
	if (realloc_head && (add_size > 0 || children_add_size > 0)) {
		/* [한국어] 선택분 반영 시의 정렬은 필수 정렬보다 느슨해질 수
		 * 없다. 더 엄격한 쪽으로 맞춘다. */
		add_align = max(min_align, add_align);
		/* [한국어] add_size 와 children_add_size 를 제자리에 넣어
		 * "필수 + 선택" 크기를 구한다. min_size 자리에 add_size 가
		 * 들어가는 것에 유의 — calculate_memsize() 는 min_size 를
		 * max 로, children_add_size 를 덧셈으로 다룬다. */
		size1 = calculate_memsize(size, add_size, children_add_size,
					  win_align);
	}

	/* [한국어] 필수도 0 이고 이상적 크기도 0 이면 이 창 아래에 담을 것이
	 * 하나도 없다. */
	if (!size0 && !size1) {
		/* [한국어] 값이 들어 있던 창을 지금 끄는 경우에만 로그를 남긴다.
		 * bus->self 검사는 루트 버스에서의 NULL 역참조 방지. */
		if (bus->self && (b_res->start || b_res->end))
			pci_info(bus->self, "disabling bridge window %pR to %pR (unused)\n",
				 b_res, &bus->busn_res);
		/* [한국어] 창을 꺼진 것으로 표시한다. 배치 코드가 건너뛰고,
		 * pci_setup_bridge_mmio() 계열이 base > limit 로 실제로 닫는다. */
		b_res->flags |= IORESOURCE_DISABLED;
		return;
	}

	/* [한국어] 창에 최종 요구 정렬과 "필수" 크기를 기록한다. 위 if 블록
	 * 안에서 이미 한 번 했더라도 여기서 다시 하는 이유는, size0 == 0 이고
	 * size1 만 0 이 아닌 경로(= 필수는 없고 선택분만 있는 창)로도 여기에
	 * 도달하기 때문이다. 그 경우 크기 0 으로 설정되고, 아래에서 선택분이
	 * 등록되어 나중에 창이 열릴 여지가 남는다. */
	resource_set_range(b_res, min_align, size0);
	/* [한국어] 브리지 창은 "시작 주소를 정렬에 맞추는" 방식임을 표시한다. */
	b_res->flags |= IORESOURCE_STARTALIGN;
	/* [한국어] 선택분 등록 조건. I/O 판과 달리 "size1 > size0" 외에
	 * "add_align > min_align" 도 조건에 들어간다 — 크기는 그대로여도
	 * 선택분이 더 엄격한 정렬을 요구할 수 있고, 그 요구도 나중에
	 * 반영하려면 목록에 실려 있어야 하기 때문이다. */
	if (bus->self && realloc_head && (size1 > size0 || add_align > min_align)) {
		/* [한국어] 선택분이 있으니 이 창을 쓸 계획이 있다. 꺼짐 표시 해제. */
		b_res->flags &= ~IORESOURCE_DISABLED;
		/* [한국어] 차액을 add_size 로 삼는다. 삼항 연산자가 필요한 이유:
		 * 위 조건이 "add_align > min_align" 만으로도 참이 될 수 있어
		 * size1 이 size0 보다 작거나 같을 수 있고, 그때 빼면 언더플로가
		 * 난다(resource_size_t 는 부호 없는 정수다). */
		add_size = size1 > size0 ? size1 - size0 : 0;
		/* [한국어] 선택적 요구로 등록한다. 소유자는 창을 가진 상위
		 * 브리지(bus->self)다. */
		pci_dev_res_add_to_list(realloc_head, bus->self, b_res,
					add_size, add_align);
		/* [한국어] 요청한 예비 크기와 정렬을 로그로 남긴다. 핫플러그
		 * 여유 공간이 왜 그만큼인지 추적하는 단서가 된다. */
		pci_info(bus->self, "bridge window %pR to %pR add_size %llx add_align %llx\n",
			   b_res, &bus->busn_res,
			   (unsigned long long) add_size,
			   (unsigned long long) add_align);
	}
}

/*
 * [한국어]
 * __pci_bus_size_bridges - 크기 산정 pass 의 재귀 본체 (아래에서 위로)
 *
 * @bus: 산정을 시작할 버스. 이 버스와 그 아래 전체를 훑는다.
 * @realloc_head: 선택적 요구를 실을 목록(= add_list). NULL 가능.
 * @return: 없음. 결과는 각 브리지의 창 자원 크기/정렬에 기록된다.
 *
 * 왜 재귀이고, 왜 아래에서 위로인가 — 이 파일 알고리즘의 첫 번째 pass 다.
 * 브리지 창의 크기는 그 아래 모든 것을 합친 값이라, 자식이 먼저 정해져야
 * 부모를 정할 수 있다. 그래서 함수 첫머리에서 하위 버스를 재귀로 먼저
 * 처리하고(깊이 우선), 돌아온 뒤에 자기 버스의 창 크기를 계산한다.
 * 하위 브리지의 창은 그 시점에 이미 크기가 정해져 있으므로, 상위 계산에서
 * 그냥 "자원 하나"로 세어진다. 이렇게 리프에서 루트까지 크기가 쌓여 올라온다.
 *
 * 동작 단계:
 *   1) 이 버스의 각 장치 중 하위 버스를 가진 것(= 브리지)을 재귀 처리한다.
 *      CardBus 브리지는 규칙이 달라 setup-cardbus.c 로 보낸다.
 *   2) 이 버스 자신의 창 크기를 정한다. 루트 버스는 특별 취급 —
 *      호스트 브리지가 "창 크기를 정해 달라"고 명시한 경우에만 진행한다.
 *   3) 핫플러그 브리지면 예비 공간(pci_hotplug_* 전역)을 선택 몫으로 얹는다.
 *   4) I/O 창 -> prefetchable 메모리 창 -> non-prefetchable 메모리 창 순으로
 *      크기를 계산한다.
 * 실행 컨텍스트: 프로세스 문맥. 재귀 깊이는 PCI 트리 깊이로 제한된다
 *   (실제 하드웨어에서 수 단계 수준이라 스택 위험은 낮다). 락 없음.
 *
 * 호출 체인:
 *   pci_bus_size_bridges() / pci_assign_unassigned_root_bus_resources() /
 *   pci_assign_unassigned_bridge_resources() / pbus_reassign_bridge_resources() /
 *   pci_assign_unassigned_bus_resources()
 *     -> [이 함수] -> (재귀) -> pci_bridge_check_ranges(), pbus_size_io(),
 *                     pbus_size_mem(), pci_bus_size_cardbus_bridge()
 */
void __pci_bus_size_bridges(struct pci_bus *bus, struct list_head *realloc_head)
{
	/* [한국어] 버스 위 장치를 훑을 커서. */
	struct pci_dev *dev;
	/* [한국어] 이 버스의 창에 얹을 핫플러그 예비 크기 세 개. 핫플러그
	 * 브리지가 아니면 0 으로 남아 아무 영향이 없다. */
	resource_size_t additional_io_size = 0, additional_mmio_size = 0,
			additional_mmio_pref_size = 0;
	/* [한국어] 크기를 정할 창 자원을 가리킬 임시 포인터. */
	struct resource *b_res;
	/* [한국어] 루트 버스일 때 호스트 브리지 구조체를 담을 자리. */
	struct pci_host_bridge *host;
	/* [한국어] 아래 switch 의 분기 키. 브리지 종류(또는 루트 버스임)를
	 * 나타낸다. */
	int hdr_type;

	/* [한국어] 1단계: 하위 버스를 먼저 처리한다(깊이 우선). */
	list_for_each_entry(dev, &bus->devices, bus_list) {
		/* [한국어] 이 장치 아래에 버스가 있는가 = 브리지인가. */
		struct pci_bus *b = dev->subordinate;
		if (!b)
			/* [한국어] 일반 장치(엔드포인트)는 아래에 버스가 없다.
			 * 그 BAR 들은 이 버스의 창 계산에서 세어진다. */
			continue;

		/* [한국어] 브리지 종류에 따라 처리 경로가 갈린다.
		 * hdr_type 은 config 헤더의 Header Type 필드로, 표준 장치인지
		 * PCI-to-PCI 브리지인지 CardBus 브리지인지를 알려 준다. */
		switch (dev->hdr_type) {
		case PCI_HEADER_TYPE_CARDBUS:
			/* [한국어] CardBus 브리지는 창 구조가 달라 전용 코드가
			 * 처리한다. 0 이 아닌 값을 돌려주면 그쪽에서 처리를
			 * 마쳤다는 뜻이라 아래 표준 재귀를 건너뛴다. */
			if (pci_bus_size_cardbus_bridge(b, realloc_head))
				continue;
			break;

		case PCI_HEADER_TYPE_BRIDGE:
		default:
			/* [한국어] 표준 PCI-to-PCI 브리지(그리고 정체가
			 * 모호한 경우)는 이 함수를 재귀 호출해 아래를 먼저
			 * 완성시킨다. 돌아오면 b 의 창 크기가 정해져 있고,
			 * 그 창이 아래 pbus_size_* 계산에서 "이 버스의
			 * 자원 하나"로 세어진다. */
			__pci_bus_size_bridges(b, realloc_head);
			break;
		}
	}

	/* The root bus? */
	/* [한국어] 2단계: 이제 이 버스 자신의 창 크기를 정한다. 먼저 루트
	 * 버스인지 확인한다 — 루트 버스에는 상위 브리지가 없어 처리가 다르다. */
	if (pci_is_root_bus(bus)) {
		/* [한국어] 이 버스를 제공하는 호스트 브리지 구조체를 얻는다. */
		host = to_pci_host_bridge(bus->bridge);
		/* [한국어] size_windows 는 "커널이 이 호스트 브리지의 창 크기를
		 * 정해도 되는가"를 나타낸다. 대부분의 플랫폼은 호스트 브리지
		 * aperture 를 펌웨어나 device tree 가 고정해 두므로 이 값이
		 * 꺼져 있고, 그때는 크기를 건드리지 않고 돌아간다. */
		if (!host->size_windows)
			return;
		/* [한국어] 루트 버스에는 브리지 장치가 없어 hdr_type 을 읽을
		 * 곳이 없다. 오른쪽 원문 주석대로 일부러 유효하지 않은 -1 을
		 * 넣어 아래 switch 의 default 로 떨어지게 한다 — 즉
		 * 핫플러그 예비 처리는 건너뛰고 창 크기 계산만 수행한다. */
		hdr_type = -1;	/* Intentionally invalid - not a PCI device. */
	} else {
		/* [한국어] 일반 버스는 상위 브리지의 헤더 타입을 그대로 쓴다. */
		hdr_type = bus->self->hdr_type;
	}

	/* [한국어] 이 버스의 창 크기를 정하는 본체. 위에서 정한 hdr_type 이
	 * 분기 키다. */
	switch (hdr_type) {
	case PCI_HEADER_TYPE_CARDBUS:
		/* Don't size CardBuses yet */
		/* [한국어] CardBus 브리지 "아래"의 버스는 여기서 크기를 정하지
		 * 않는다. 위 재귀 루프에서 pci_bus_size_cardbus_bridge() 가
		 * 이미 처리했거나, 그쪽이 처리하도록 남겨 둔다. */
		break;

	case PCI_HEADER_TYPE_BRIDGE:
		/* [한국어] 크기 계산에 앞서, 이 브리지가 실제로 어떤 창을
		 * 갖는지를 자원 플래그에 새겨 넣는다. 이것을 하지 않으면
		 * pbus_select_window_for_type() 이 창을 못 찾는다. */
		pci_bridge_check_ranges(bus);
		/* [한국어] 핫플러그 슬롯을 만드는 브리지인가. 그렇다면 지금은
		 * 비어 있어도 나중에 장치가 꽂힐 수 있으니 예비 공간을 잡아
		 * 둔다. 이것이 없으면 핫플러그 시점에 상위 창까지 전부 다시
		 * 배치해야 하고, 그 사이 다른 장치가 잠시 접근 불가가 된다. */
		if (bus->self->is_hotplug_bridge) {
			/* [한국어] 세 전역은 부팅 파라미터
			 * pci=hpiosize=/hpmemsize=/hpmmiosize= 등으로 조정된다
			 * (파싱은 drivers/pci/pci.c 에 있다). 여기서 선택 몫으로
			 * 넘겨지므로, 공간이 모자라면 포기되고 남으면 반영된다. */
			additional_io_size  = pci_hotplug_io_size;
			/* [한국어] non-prefetchable 메모리 예비 크기. */
			additional_mmio_size = pci_hotplug_mmio_size;
			/* [한국어] prefetchable 메모리 예비 크기. */
			additional_mmio_pref_size = pci_hotplug_mmio_pref_size;
		}
		/* [한국어] 의도적으로 다음 case 로 흘려보낸다. 브리지든
		 * 루트 버스든 창 크기를 계산하는 절차는 같기 때문이다.
		 * fallthrough 는 컴파일러의 "빠뜨린 break" 경고를 막는
		 * 명시적 표시다(빠뜨린 것이 아니라 의도한 것이라는 선언). */
		fallthrough;
	default:
		/* [한국어] I/O 창 크기를 먼저 정한다. 계산이 가장 단순하고
		 * 다른 창과 독립적이다. */
		pbus_size_io(bus, additional_io_size, realloc_head);

		/* [한국어] prefetchable 메모리 창을 찾는다. 세 비트를 모두 켜서
		 * 넘기는 이유는 pbus_select_window_for_type() 의 규칙 때문이다 —
		 * 64비트 prefetchable 요구로 물으면 64비트 prefetchable 창이
		 * 있을 때 그것을 돌려주고, 없으면 (루트 버스 경로에서) 조건을
		 * 완화하며 내려간다. 일반 버스에서는 브리지의 prefetchable
		 * 창을 그대로 돌려주거나 NULL 이다. */
		b_res = pbus_select_window_for_type(bus, IORESOURCE_MEM |
							 IORESOURCE_PREFETCH |
							 IORESOURCE_MEM_64);
		/* [한국어] 돌아온 창이 실제로 prefetchable 인지 다시 확인한다.
		 * 위 선택 함수는 prefetchable 창이 없으면 non-prefetchable
		 * 창을 대신 돌려줄 수 있는데, 그것을 여기서 prefetchable 창인
		 * 줄 알고 크기를 정하면 아래 두 번째 호출과 중복 계산된다. */
		if (b_res && (b_res->flags & IORESOURCE_PREFETCH)) {
			pbus_size_mem(bus, b_res, additional_mmio_pref_size,
				      realloc_head);
		}

		/* [한국어] 이제 non-prefetchable 메모리 창을 찾는다.
		 * prefetchable 비트를 빼고 물으면 그 창이 선택된다. */
		b_res = pbus_select_window_for_type(bus, IORESOURCE_MEM);
		/* [한국어] 창이 있으면 크기를 정한다. 여기서는 종류를 다시
		 * 확인하지 않는데, IORESOURCE_MEM 만으로 물었으므로 돌아온
		 * 것이 곧 non-prefetchable 창이기 때문이다.
		 * NVMe 관점: 엔드포인트의 BAR 가 prefetchable 이 아니면
		 * pbus_select_window_for_type() 의 첫 번째 규칙에 따라 이 창으로
		 * 오므로, 이 호출이 그 BAR 를 담을 창의 크기를 정한다.
		 * NVMe 컨트롤러 레지스터 BAR 가 실제로 어느 쪽인지는 위
		 * pci_setup_bridge_mmio() 주석에 적은 이유로 이 트리에서는
		 * 확정할 수 없다. */
		if (b_res) {
			pbus_size_mem(bus, b_res, additional_mmio_size,
				      realloc_head);
		}
		break;
	}
}

/*
 * [한국어]
 * pci_bus_size_bridges - 선택적 요구 없이 크기 산정만 수행하는 공개 진입점
 *
 * @bus: 산정할 버스.
 * @return: 없음.
 *
 * 왜 필요한가: 호스트 컨트롤러 드라이버처럼 "일단 크기만 정해 달라"는
 * 호출자를 위한 얇은 래퍼다. realloc_head 로 NULL 을 넘기므로 선택적
 * 요구(핫플러그 예비 등)는 목록에 실리지 않고, 대신 필수 크기에 곧바로
 * 반영된다(pbus_size_io/pbus_size_mem 의 "realloc_head ? 0 : add_size" 참조).
 * 즉 한 번에 최종 크기를 확정하는 단순한 모드다.
 * 실행 컨텍스트: 프로세스 문맥. 락 없음 — 호출자가 필요하면 잡는다.
 *
 * 호출 체인:
 *   (모듈/컨트롤러 드라이버) -> [이 함수] -> __pci_bus_size_bridges()
 */
void pci_bus_size_bridges(struct pci_bus *bus)
{
	/* [한국어] NULL 을 넘겨 "선택적 요구를 미루지 않는" 모드로 부른다. */
	__pci_bus_size_bridges(bus, NULL);
}
/* [한국어] 모듈에도 공개한다. GPL 전용이 아닌 일반 EXPORT_SYMBOL 인데,
 * 오래전부터 공개돼 온 인터페이스라 그렇다. */
EXPORT_SYMBOL(pci_bus_size_bridges);

/*
 * [한국어]
 * assign_fixed_resource_on_bus - 고정 주소 자원을 담을 부모 창을 이 버스에서 찾아 등록한다
 *
 * @b: 후보 부모 창을 찾을 버스.
 * @r: 등록할 고정 주소 자원(IORESOURCE_PCI_FIXED).
 * @return: 없음. 성공/실패는 r->parent 가 채워졌는지로 호출자가 확인한다.
 *
 * 왜 필요한가: 고정 주소 자원은 배치 알고리즘의 대상이 아니다
 * (pdev_resource_should_fit 이 제외한다). 하지만 커널 자원 트리에는
 * 등록해 두어야, 다른 자원을 배치할 때 그 영역을 침범하지 않는다.
 * 이 함수는 "그 고정 주소를 완전히 품는 창"을 찾아 자식으로 붙인다.
 * 동작: 버스의 자원 목록을 훑으며 (1) 공간 종류가 같고 (2) 범위가 완전히
 * 포함되는 창을 찾으면 request_resource() 로 자식 등록을 시도한다.
 * 이미 다른 자원이 그 영역을 차지하고 있으면 실패하지만, 반환값을 보지
 * 않고 계속 훑는다 — 다른 창에서 성공할 수도 있기 때문이다.
 * 실행 컨텍스트: 프로세스 문맥. request_resource() 내부에서 자원 트리
 *   전역 락을 잡는다.
 *
 * 호출 체인:
 *   pdev_assign_fixed_resources() -> [이 함수]
 *     -> pci_bus_for_each_resource(), resource_contains(), request_resource()
 */
static void assign_fixed_resource_on_bus(struct pci_bus *b, struct resource *r)
{
	/* [한국어] 후보 부모 창을 훑을 커서. */
	struct resource *parent_r;
	/* [한국어] 공간 종류 비교용 마스크. 여기에는 IORESOURCE_MEM_64 를
	 * 넣지 않는데, 64비트 여부는 "이 창에 담길 수 있는가"와 무관하고
	 * 실제 포함 관계는 아래 resource_contains() 가 주소로 확인하기
	 * 때문이다. */
	unsigned long mask = IORESOURCE_IO | IORESOURCE_MEM |
			     IORESOURCE_PREFETCH;

	/* [한국어] 이 버스가 가진 창(또는 aperture)들을 훑는다. */
	pci_bus_for_each_resource(b, parent_r) {
		/* [한국어] 비어 있는 자원 슬롯은 건너뛴다. */
		if (!parent_r)
			continue;

		/* [한국어] 두 조건을 모두 만족해야 부모가 될 수 있다.
		 * (1) 공간 종류가 정확히 같다 — I/O 자원을 메모리 창에
		 *     넣거나 prefetchable 여부가 다르면 안 된다.
		 * (2) 창이 이 자원의 주소 범위를 완전히 품는다. */
		if ((r->flags & mask) == (parent_r->flags & mask) &&
		    resource_contains(parent_r, r))
			/* [한국어] 자원 트리에 자식으로 등록한다. 이미 그
			 * 영역을 다른 자원이 쓰고 있으면 실패하지만, 반환값을
			 * 보지 않고 다음 창으로 넘어간다 — 겹치는 창이 여럿
			 * 있을 수 있어 다른 곳에서 성공할 여지가 남는다.
			 * 성공하면 r->parent 가 채워지고, 호출자의
			 * resource_assigned(r) 검사가 참이 되어 루프가 끝난다. */
			request_resource(parent_r, r);
	}
}

/*
 * Try to assign any resources marked as IORESOURCE_PCI_FIXED, as they are
 * skipped by pbus_assign_resources_sorted().
 */
/*
 * [한국어]
 * pdev_assign_fixed_resources - 한 장치의 고정 주소 자원들을 자원 트리에 등록한다
 *
 * @dev: 대상 장치.
 * @return: 없음.
 *
 * 왜 필요한가: 바로 위 원문 주석이 밝히듯, 고정 주소 자원은
 * pbus_assign_resources_sorted() 가 건너뛴다(옮길 수 없으니 배치할 것이
 * 없다). 그렇다고 방치하면 자원 트리에 그 영역이 비어 있는 것으로 보여
 * 다른 자원이 그 위에 배치될 수 있다. 그래서 배치 pass 뒤에 이 함수가
 * "이미 정해진 주소 그대로" 트리에 등록해 영역을 예약한다.
 *
 * 왜 상위로 거슬러 올라가는가: 고정 주소가 반드시 바로 위 브리지 창 안에
 * 있으리라는 보장이 없다. 그 창 밖이라면 더 위의 창(또는 루트의 aperture)이
 * 품고 있을 수 있으므로, 등록에 성공할 때까지 부모 버스를 따라 올라간다.
 * 실행 컨텍스트: 프로세스 문맥. 버스 트리를 위로 거슬러 오르므로 상위
 *   호출자가 트리 안정성을 보장해야 한다.
 *
 * 호출 체인:
 *   __pci_bus_assign_resources() -> [이 함수] -> assign_fixed_resource_on_bus()
 */
static void pdev_assign_fixed_resources(struct pci_dev *dev)
{
	/* [한국어] 장치의 자원을 훑을 커서. */
	struct resource *r;

	/* [한국어] resource[] 전체 순회. */
	pci_dev_for_each_resource(dev, r) {
		/* [한국어] 위로 거슬러 올라갈 버스 커서. */
		struct pci_bus *b;

		/* [한국어] 세 조건 중 하나라도 걸리면 이 함수의 대상이 아니다.
		 * (1) 이미 트리에 등록됨 — 할 일이 없다.
		 * (2) 고정 주소 표시가 없음 — 일반 배치 경로가 처리한다.
		 * (3) I/O 도 메모리도 아님 — 자원 트리에 넣을 대상이 아니다
		 *     (빈 슬롯이거나 다른 종류). */
		if (resource_assigned(r) ||
		    !(r->flags & IORESOURCE_PCI_FIXED) ||
		    !(r->flags & (IORESOURCE_IO | IORESOURCE_MEM)))
			continue;

		/* [한국어] 장치가 달린 버스에서 시작한다. */
		b = dev->bus;
		/* [한국어] 등록에 성공하거나(resource_assigned 가 참이 되거나)
		 * 루트를 지나쳐 b 가 NULL 이 될 때까지 위로 올라간다. */
		while (b && !resource_assigned(r)) {
			/* [한국어] 이 버스의 창들 중에 품어 줄 것이 있는지
			 * 시도한다. 성공하면 r->parent 가 채워진다. */
			assign_fixed_resource_on_bus(b, r);
			/* [한국어] 실패했으면 한 단계 위 버스로. 루트 버스의
			 * parent 는 NULL 이라 루프가 자연히 끝난다. */
			b = b->parent;
		}
	}
}

/*
 * [한국어]
 * __pci_bus_assign_resources - 배치 pass 의 재귀 본체 (위에서 아래로)
 *
 * @bus: 배치를 시작할 버스.
 * @realloc_head: 선택적 요구 목록(= add_list). NULL 가능.
 * @fail_head: 실패 보고 목록. NULL 가능.
 * @return: 없음.
 *
 * 왜 위에서 아래인가 — 크기 산정과 정반대 방향이다.
 * 자식 BAR 의 주소는 부모 창 안에서 정해져야 하므로, 부모 창의 주소가
 * 먼저 확정돼야 한다. 그래서 이 함수는 자기 버스의 자원을 먼저 배치하고
 * (pbus_assign_resources_sorted), 그다음 각 브리지 아래로 재귀해 내려간다.
 * 재귀에서 돌아온 뒤에는 그 브리지의 창을 하드웨어에 기록한다 — 아래
 * 배치가 끝나야 창의 최종 값이 확정되기 때문이다.
 *
 * 이 순서를 크기 산정 pass 와 나란히 보면 알고리즘 전체가 보인다:
 *   __pci_bus_size_bridges  : 리프 -> 루트 (크기를 위로 모음)
 *   [이 함수]               : 루트 -> 리프 (주소를 아래로 내림)
 *
 * 실행 컨텍스트: 프로세스 문맥. config 공간 쓰기 다수. 락 없음.
 *
 * 호출 체인:
 *   pci_bus_assign_resources() / pci_assign_unassigned_root_bus_resources() /
 *   __pci_bridge_assign_resources() / pci_assign_unassigned_bus_resources()
 *     -> [이 함수] -> (재귀) -> pbus_assign_resources_sorted(),
 *                     pdev_assign_fixed_resources(), pci_setup_bridge()
 */
void __pci_bus_assign_resources(const struct pci_bus *bus,
				struct list_head *realloc_head,
				struct list_head *fail_head)
{
	/* [한국어] 하위 버스를 가리킬 커서. */
	struct pci_bus *b;
	/* [한국어] 버스 위 장치를 훑을 커서. */
	struct pci_dev *dev;

	/* [한국어] 먼저 이 버스의 모든 장치 자원을 한 리스트로 모아 배치한다.
	 * 아래로 내려가기 전에 이 층을 끝내는 것이 top-down 의 핵심이다. */
	pbus_assign_resources_sorted(bus, realloc_head, fail_head);

	/* [한국어] 이제 각 장치를 다시 훑으며 고정 자원 등록과 하위 재귀를 한다. */
	list_for_each_entry(dev, &bus->devices, bus_list) {
		/* [한국어] 배치 대상이 아니었던 고정 주소 자원을 자원 트리에
		 * 등록해 영역을 예약한다. 위 배치가 끝난 뒤에 하는 이유는,
		 * 이 시점에야 부모 창의 주소가 확정돼 포함 관계를 판정할 수
		 * 있기 때문이다. */
		pdev_assign_fixed_resources(dev);

		/* [한국어] 이 장치 아래에 버스가 있는가(= 브리지인가). */
		b = dev->subordinate;
		if (!b)
			/* [한국어] 엔드포인트는 더 내려갈 곳이 없다. */
			continue;

		/* [한국어] 하위 버스로 재귀. 이 브리지의 창은 위 배치에서
		 * 이미 주소가 정해졌으므로, 그 안에서 자식들의 자리를 잡는다. */
		__pci_bus_assign_resources(b, realloc_head, fail_head);

		/* [한국어] 재귀에서 돌아왔다. 이제 이 브리지의 창을 하드웨어에
		 * 기록할 차례다. 브리지 종류에 따라 방법이 갈린다. */
		switch (dev->hdr_type) {
		case PCI_HEADER_TYPE_BRIDGE:
			/* [한국어] 이미 활성화된(드라이버가 붙어 동작 중인)
			 * 브리지의 창은 건드리지 않는다. 동작 중인 브리지의
			 * base/limit 를 바꾸면 그 아래로 오가던 트랜잭션이
			 * 순간적으로 끊긴다. 아직 활성화 전이라면 안전하게
			 * 새 값을 기록할 수 있다. */
			if (!pci_is_enabled(dev))
				pci_setup_bridge(b);
			break;

		case PCI_HEADER_TYPE_CARDBUS:
			/* [한국어] CardBus 브리지는 창 레지스터 구조가 달라
			 * 전용 함수가 기록한다. */
			pci_setup_cardbus_bridge(b);
			break;

		default:
			/* [한국어] 아래에 버스를 가졌는데 헤더 타입이 브리지가
			 * 아닌 이상한 경우다. 창 레지스터의 위치를 알 수 없어
			 * 프로그램할 수 없으므로 사실만 로그로 남긴다.
			 * %04x:%02x 는 "도메인:버스번호" 형식이다. */
			pci_info(dev, "not setting up bridge for bus %04x:%02x\n",
				 pci_domain_nr(b), b->number);
			break;
		}
	}
}

/*
 * [한국어]
 * pci_bus_assign_resources - 선택적 요구도 실패 보고도 없이 배치만 하는 공개 진입점
 *
 * @bus: 배치할 버스.
 * @return: 없음.
 *
 * 왜 필요한가: pci_bus_size_bridges() 와 짝을 이루는 단순 모드의 배치
 * 진입점이다. 두 목록 모두 NULL 이므로 선택적 요구를 미루지도 않고
 * 실패를 보고하지도 않는다 — "한 번에 되는 만큼만 하고 끝낸다".
 * 재시도 루프가 필요 없는 호출자(일부 호스트 컨트롤러 드라이버, 예를 들어
 * 이 트리의 drivers/pci/controller/pci-ftpci100.c 와 pci-hyperv.c,
 * drivers/pci/hotplug/octep_hp.c)가 쓴다.
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   (호스트 컨트롤러 / 핫플러그 드라이버) -> [이 함수]
 *     -> __pci_bus_assign_resources()
 */
void pci_bus_assign_resources(const struct pci_bus *bus)
{
	/* [한국어] 두 목록 모두 NULL — 선택분 미루기도, 실패 보고도 없다. */
	__pci_bus_assign_resources(bus, NULL, NULL);
}
/* [한국어] 모듈에 공개. 오래된 인터페이스라 GPL 전용이 아니다. */
EXPORT_SYMBOL(pci_bus_assign_resources);

/*
 * [한국어]
 * pci_claim_device_resources - 장치의 일반 BAR 들을 현재 값 그대로 승인한다
 *
 * @dev: 대상 장치.
 * @return: 없음. 개별 승인 실패는 무시한다(반환값을 보지 않는다).
 *
 * 왜 필요한가: 커널이 자원을 새로 배치하지 않고 펌웨어(BIOS/UEFI)가 이미
 * 잡아 둔 배치를 그대로 물려받는 경로의 일부다. BAR 에 이미 들어 있는
 * 주소를 커널 자원 트리에 등록해 "이 영역은 이 장치가 쓴다"고 표시한다.
 * 등록해 두지 않으면 나중에 다른 자원이 그 위에 배치될 수 있다.
 * 순회 범위가 0 부터 PCI_BRIDGE_RESOURCES 미만인 것이 요점 — 브리지 창은
 * 규칙이 달라(창 자르기 등) 별도 함수가 처리한다.
 * 실패를 무시하는 이유: 펌웨어의 배치가 서로 겹치거나 상위 창 밖인 경우가
 * 실제로 있다. 그런 BAR 는 등록되지 않은 채 남고, 필요하면 이후의
 * 재배치 경로가 다시 다룬다.
 * 실행 컨텍스트: 프로세스 문맥. 자원 트리 락은 pci_claim_resource() 내부.
 *
 * 호출 체인:
 *   pci_bus_allocate_dev_resources() -> [이 함수] -> pci_claim_resource()
 */
static void pci_claim_device_resources(struct pci_dev *dev)
{
	/* [한국어] resource[] 인덱스. */
	int i;

	/* [한국어] 0 부터 브리지 창 구획 직전까지 = 일반 BAR, ROM, VF BAR 구간.
	 * 브리지 창은 여기서 다루지 않는다. */
	for (i = 0; i < PCI_BRIDGE_RESOURCES; i++) {
		/* [한국어] 이번 인덱스의 자원. */
		struct resource *r = &dev->resource[i];

		/* [한국어] 빈 슬롯(flags 0)이거나 이미 트리에 등록된 자원은
		 * 승인할 것이 없다. */
		if (!r->flags || resource_assigned(r))
			continue;

		/* [한국어] 현재 주소 그대로 자원 트리에 등록을 시도한다.
		 * 반환값을 보지 않는 이유는 위 함수 설명 참조. */
		pci_claim_resource(dev, i);
	}
}

/*
 * [한국어]
 * pci_claim_bridge_resources - 브리지의 창들을 현재 값 그대로 승인한다
 *
 * @dev: 대상 브리지 장치.
 * @return: 없음. 개별 실패는 무시한다.
 *
 * 왜 별도 함수인가: 브리지 창은 일반 BAR 와 달리 "상위 창 밖으로 삐져나와
 * 있으면 잘라서라도 승인한다"는 구제 로직이 있다. 그 로직은
 * pci_claim_bridge_resource() 안에 있고, 이 함수는 브리지 창 구획만
 * 골라 그 함수를 부른다.
 * 실행 컨텍스트: 프로세스 문맥. config 공간 쓰기 발생 가능
 *   (창을 잘랐으면 그 값을 하드웨어에 반영한다).
 *
 * 호출 체인:
 *   pci_bus_allocate_resources() -> [이 함수] -> pci_claim_bridge_resource()
 */
static void pci_claim_bridge_resources(struct pci_dev *dev)
{
	/* [한국어] resource[] 인덱스. */
	int i;

	/* [한국어] 브리지 창 구획부터 배열 끝까지 순회한다. 위 함수와
	 * 정확히 상보적인 범위다. */
	for (i = PCI_BRIDGE_RESOURCES; i < PCI_NUM_RESOURCES; i++) {
		/* [한국어] 이번 인덱스의 자원. */
		struct resource *r = &dev->resource[i];

		/* [한국어] 빈 슬롯이거나 이미 등록된 창은 건너뛴다. */
		if (!r->flags || resource_assigned(r))
			continue;
		/* [한국어] 꺼진 창도 건너뛴다. 담을 것이 없어 닫아 둔 창을
		 * 자원 트리에 등록하면 쓰지도 않는 영역을 예약하는 셈이다.
		 * 이 검사가 위 일반 BAR 판에는 없는데, DISABLED 비트를 이
		 * 파일이 브리지 창에만 붙이기 때문이다. */
		if (r->flags & IORESOURCE_DISABLED)
			continue;

		/* [한국어] 창 자르기 구제까지 포함한 승인을 시도한다. */
		pci_claim_bridge_resource(dev, i);
	}
}

/*
 * [한국어]
 * pci_bus_allocate_dev_resources - 버스 트리 전체의 장치 BAR 를 재귀로 승인한다
 *
 * @b: 시작 버스.
 * @return: 없음.
 *
 * 왜 필요한가: claim 경로의 두 번째 단계다. 첫 단계
 * (pci_bus_allocate_resources)가 브리지 창을 먼저 승인해 부모를 만들어
 * 두었으므로, 이제 그 창 안의 장치 BAR 들을 자식으로 등록할 수 있다.
 * 순서가 뒤바뀌면 부모가 없어 등록이 전부 실패한다.
 * 순회 방식: 장치를 훑다가 브리지를 만나면 그 아래로 재귀한다.
 * 실행 컨텍스트: 프로세스 문맥. 재귀 깊이는 PCI 트리 깊이.
 *
 * 호출 체인:
 *   pci_bus_claim_resources() -> [이 함수] -> (재귀), pci_claim_device_resources()
 */
static void pci_bus_allocate_dev_resources(struct pci_bus *b)
{
	/* [한국어] 장치 순회 커서. */
	struct pci_dev *dev;
	/* [한국어] 하위 버스를 가리킬 커서. */
	struct pci_bus *child;

	/* [한국어] 이 버스에 달린 장치를 모두 훑는다. */
	list_for_each_entry(dev, &b->devices, bus_list) {
		/* [한국어] 이 장치의 일반 BAR 들을 승인한다. */
		pci_claim_device_resources(dev);

		/* [한국어] 브리지라면 아래에 버스가 있다. */
		child = dev->subordinate;
		if (child)
			/* [한국어] 하위 버스로 재귀. 트리 전체를 덮는다. */
			pci_bus_allocate_dev_resources(child);
	}
}

/*
 * [한국어]
 * pci_bus_allocate_resources - 버스 트리 전체의 브리지 창을 재귀로 승인한다
 *
 * @b: 시작 버스.
 * @return: 없음.
 *
 * 왜 필요한가: claim 경로의 첫 번째 단계다. 장치 BAR 를 자원 트리에 넣으려면
 * 그것을 품을 부모(브리지 창)가 먼저 트리에 있어야 한다. 그래서 창을
 * 먼저, 장치를 나중에 승인한다.
 * 창 값의 출처: 브리지의 config 레지스터에 펌웨어가 이미 써 둔 값이다.
 * pci_read_bridge_bases() 가 그 값을 읽어 struct resource 로 옮겨 준다.
 * 실행 컨텍스트: 프로세스 문맥. config 공간 읽기와(그리고 창을 자른 경우)
 *   쓰기가 발생한다.
 *
 * 호출 체인:
 *   pci_bus_claim_resources() -> [이 함수]
 *     -> pci_read_bridge_bases() (probe.c), pci_claim_bridge_resources(), (재귀)
 */
static void pci_bus_allocate_resources(struct pci_bus *b)
{
	/* [한국어] 하위 버스 순회 커서. */
	struct pci_bus *child;

	/*
	 * Carry out a depth-first search on the PCI bus tree to allocate
	 * bridge apertures.  Read the programmed bridge bases and
	 * recursively claim the respective bridge resources.
	 */
	/* [한국어] 루트 버스에는 상위 브리지가 없어 b->self 가 NULL 이다.
	 * 브리지가 있는 버스만 창을 읽고 승인한다. */
	if (b->self) {
		/* [한국어] 브리지의 base/limit 레지스터를 실제로 읽어
		 * b->self->resource[] 의 창 자원에 채워 넣는다. 즉 하드웨어의
		 * 현재 상태를 커널 표현으로 가져오는 단계다. */
		pci_read_bridge_bases(b);
		/* [한국어] 그렇게 읽어 온 창들을 자원 트리에 등록한다.
		 * 상위 창 밖으로 삐져나온 창은 잘려서 등록된다. */
		pci_claim_bridge_resources(b->self);
	}

	/* [한국어] 하위 버스들로 재귀한다. 여기서는 장치 목록(devices)이
	 * 아니라 자식 버스 목록(children)을 훑는 점에 유의 — 브리지 창만
	 * 관심사이므로 버스 단위로 내려가는 편이 직접적이다. */
	list_for_each_entry(child, &b->children, node)
		pci_bus_allocate_resources(child);
}

/*
 * [한국어]
 * pci_bus_claim_resources - 펌웨어가 잡아 둔 배치를 그대로 물려받는 공개 진입점
 *
 * @b: 대상 버스(보통 루트 버스).
 * @return: 없음.
 *
 * 왜 필요한가: 이 파일에는 두 개의 큰 경로가 있다.
 *   (a) 재배치 경로 — pci_assign_unassigned_* 계열. 커널이 크기를 다시
 *       계산하고 주소를 새로 정한다.
 *   (b) 승인(claim) 경로 — 이 함수. 펌웨어가 정해 둔 주소를 그대로
 *       인정하고 커널 자원 트리에만 등록한다.
 * (b)를 쓰는 이유: 펌웨어가 이미 동작하도록 설정해 둔 장치(부팅 디스크,
 * 콘솔 등)의 주소를 부팅 중에 옮기면 그 사이 접근이 깨진다. 또 ACPI 등이
 * 특정 주소를 전제로 테이블을 만들어 두었을 수도 있다.
 * 두 단계의 순서가 중요하다 — 창(부모)을 먼저, 장치 BAR(자식)를 나중에.
 *
 * NVMe 관점: 부팅 펌웨어가 NVMe SSD 의 BAR0 를 이미 프로그램해 둔 흔한
 * 경우, NVMe 드라이버가 pci_resource_start(pdev, 0) 으로 읽는 값은 이
 * 경로가 승인한 "펌웨어가 정한 주소"다. 재배치 경로를 탄 경우에만 이
 * 파일의 배치 알고리즘이 그 값을 새로 정한다.
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   drivers/pci/probe.c:7936 -> [이 함수]
 *     -> pci_bus_allocate_resources(), pci_bus_allocate_dev_resources()
 */
void pci_bus_claim_resources(struct pci_bus *b)
{
	/* [한국어] 1단계: 브리지 창부터. 자식이 매달릴 부모를 먼저 만든다. */
	pci_bus_allocate_resources(b);
	/* [한국어] 2단계: 그 창들 안에 장치 BAR 를 자식으로 등록한다. */
	pci_bus_allocate_dev_resources(b);
}
/* [한국어] 모듈에 공개. */
EXPORT_SYMBOL(pci_bus_claim_resources);

/*
 * [한국어]
 * __pci_bridge_assign_resources - 브리지 하나와 그 아래 서브트리를 배치한다
 *
 * @bridge: 시작점이 될 브리지 장치. const 로 받지만 아래에서 캐스팅으로
 *          벗겨 낸다(설명은 해당 줄 참조).
 * @add_head: 선택적 요구 목록.
 * @fail_head: 실패 보고 목록.
 * @return: 없음.
 *
 * 왜 필요한가: 핫플러그로 브리지 아래에 장치가 새로 나타났을 때, 루트부터
 * 전부 다시 배치할 수는 없다(이미 동작 중인 다른 장치가 깨진다).
 * 그래서 "이 브리지부터 아래만" 배치하는 진입점이 필요하다.
 * __pci_bus_assign_resources() 와의 차이는 시작점이다 — 그쪽은 버스에서
 * 시작해 그 위 브리지의 창은 이미 정해져 있다고 보지만, 이쪽은 브리지
 * 자신의 창부터 다시 잡는다(pdev_assign_resources_sorted 로).
 * 실행 컨텍스트: 프로세스 문맥. config 공간 쓰기 다수.
 *
 * 호출 체인:
 *   pci_assign_unassigned_bridge_resources() /
 *   pbus_reassign_bridge_resources()
 *     -> [이 함수] -> pdev_assign_resources_sorted(),
 *                     __pci_bus_assign_resources(), pci_setup_bridge()
 */
static void __pci_bridge_assign_resources(const struct pci_dev *bridge,
					  struct list_head *add_head,
					  struct list_head *fail_head)
{
	/* [한국어] 이 브리지 아래의 버스를 가리킬 커서. */
	struct pci_bus *b;

	/* [한국어] 먼저 브리지 자신의 자원(= 세 창)을 배치한다. 아래로
	 * 내려가기 전에 부모 창의 주소가 정해져야 하기 때문이다.
	 * const 를 캐스팅으로 벗기는 이유: 이 함수는 인터페이스상 브리지를
	 * 읽기 전용으로 선언했지만, 실제로는 그 창 자원을 수정한다. 아래
	 * 피호출 함수가 비-const 포인터를 요구해 캐스팅이 필요하다. */
	pdev_assign_resources_sorted((struct pci_dev *)bridge,
					 add_head, fail_head);

	/* [한국어] 브리지 아래의 버스. */
	b = bridge->subordinate;
	if (!b)
		/* [한국어] 아래에 버스가 없으면 더 내려갈 곳이 없다. 브리지
		 * 자신의 창은 위에서 이미 배치했으므로 여기서 끝낸다. */
		return;

	/* [한국어] 서브트리 전체를 위에서 아래로 배치한다. */
	__pci_bus_assign_resources(b, add_head, fail_head);

	/* [한국어] 재귀가 끝났으니 이 브리지의 창을 하드웨어에 기록한다.
	 * 여기서는 hdr_type 이 아니라 class 로 분기하는 점이 위
	 * __pci_bus_assign_resources() 와 다르다. 두 필드 모두 브리지
	 * 종류를 알려 주며, 이 파일에서는 둘 다 쓰인다. */
	switch (bridge->class >> 8) {
	case PCI_CLASS_BRIDGE_PCI:
		/* [한국어] 표준 PCI-to-PCI 브리지. 세 창을 모두 기록한다.
		 * 여기서는 __pci_bus_assign_resources() 와 달리
		 * pci_is_enabled() 검사를 하지 않는데, 이 경로는 애초에
		 * "이 브리지 아래를 다시 배치하라"는 명시적 요청이라
		 * 창을 갱신하는 것이 목적이기 때문이다. */
		pci_setup_bridge(b);
		break;

	case PCI_CLASS_BRIDGE_CARDBUS:
		/* [한국어] CardBus 브리지는 전용 함수로. */
		pci_setup_cardbus_bridge(b);
		break;

	default:
		/* [한국어] 아래에 버스가 있는데 브리지 class 가 아닌 경우.
		 * 창 레지스터 위치를 몰라 프로그램할 수 없으므로 로그만 남긴다. */
		pci_info(bridge, "not setting up bridge for bus %04x:%02x\n",
			 pci_domain_nr(b), b->number);
		break;
	}
}

/*
 * [한국어]
 * pci_bridge_release_resources - 브리지 창 하나와 그 안의 모든 자식을 놓는다
 *
 * @bus: 창을 소유한 브리지 아래의 버스(브리지는 bus->self).
 * @b_win: 놓을 창 자원.
 * @return: 없음.
 *
 * 왜 필요한가: 재시도 전략의 실행부다. 창이 너무 작아 자식이 다 못
 * 들어가는 경우, 창을 통째로 놓고 크기를 다시 계산해야 한다. 창을 놓으려면
 * 먼저 그 안에 매달린 자식 자원을 전부 떼어 내야 한다 — 자원 트리는 부모를
 * 제거하기 전에 자식이 없어야 하기 때문이다.
 *
 * 왜 마지막에 창을 하드웨어에 기록하는가: 커널 자원 트리에서 창을 뗐다면
 * 하드웨어에서도 그 창을 닫아야 한다. 그러지 않으면 브리지는 여전히 그
 * 범위를 아래로 통과시키는데 커널은 그 영역이 비었다고 여겨, 다른 장치가
 * 그 주소를 받았을 때 두 장치가 같은 주소에 응답하는 상황이 된다.
 * pci_setup_one_bridge_window() 는 미배정 창에 대해 base > limit 를 써서
 * 실제로 닫아 준다.
 * 실행 컨텍스트: 프로세스 문맥. config 공간 쓰기.
 *
 * 호출 체인:
 *   pci_bus_release_bridge_resources() -> [이 함수]
 *     -> release_child_resources(), pci_release_resource(),
 *        pci_setup_one_bridge_window()
 */
static void pci_bridge_release_resources(struct pci_bus *bus,
					 struct resource *b_win)
{
	/* [한국어] 창을 소유한 브리지 장치. */
	struct pci_dev *dev = bus->self;
	/* [한국어] idx 는 창의 resource[] 인덱스, ret 는 놓기 결과. */
	int idx, ret;

	/* [한국어] 배정되지 않은 창은 놓을 것이 없다. 자원 트리에 들어 있지
	 * 않으므로 자식도 있을 수 없다. */
	if (!resource_assigned(b_win))
		return;

	/* [한국어] 인덱스 역산 — 아래 API 들이 번호를 받는다. */
	idx = pci_resource_num(dev, b_win);

	/* If there are children, release them all */
	/* [한국어] 창 안에 매달린 자식 자원을 모두 떼어 낸다. 자원 트리는
	 * 자식이 있는 노드를 제거할 수 없으므로 반드시 먼저 해야 한다.
	 * 이 호출로 그 아래 장치들의 BAR 가 미배정 상태로 돌아간다. */
	release_child_resources(b_win);

	/* [한국어] 이제 창 자체를 자원 트리에서 뗀다. */
	ret = pci_release_resource(dev, idx);
	if (ret)
		/* [한국어] 놓기에 실패했으면 창은 여전히 트리에 있으므로
		 * 하드웨어를 건드리면 안 된다. 커널 표현과 하드웨어가
		 * 어긋나는 것이 더 나쁘다. */
		return;

	/* [한국어] 놓기에 성공했으니 하드웨어에서도 창을 닫는다. 이제 창이
	 * 미배정 상태라 이 함수가 base > limit 값을 써서 실제로 닫아 준다. */
	pci_setup_one_bridge_window(dev, idx);
}

/* [한국어]
 * enum release_type - 재시도 때 브리지 창을 얼마나 과감하게 놓을지의 강도
 *
 * pci_assign_unassigned_root_bus_resources() 의 재시도 루프가 이 값을
 * 단계적으로 올린다. 1~2차 시도는 leaf_only, 3차부터 whole_subtree 다.
 * 처음부터 과감하게 놓지 않는 이유는, 많이 놓을수록 다시 배치해야 할
 * 자원이 늘어 전체가 실패할 위험도 함께 커지기 때문이다.
 */
enum release_type {
	leaf_only,
	/* [한국어] 리프 브리지의 창만 놓는다. 아래에 다른 브리지를 거느리지
	 * 않은 브리지가 리프다.
	 * 설정자: pci_assign_unassigned_root_bus_resources() 가 초기값으로 준다.
	 * 읽는 자: pci_bus_release_bridge_resources() 의 두 조건 판단.
	 * 의미: 가장 보수적인 강도. 서브트리 구조는 그대로 두고 말단만
	 *   손봐 자리를 만들어 본다.
	 * 동기화: 지역 변수로만 쓰이는 값이라 동기화 대상이 아니다. */

	whole_subtree,
	/* [한국어] 해당 브리지 아래 서브트리의 창을 전부 놓는다.
	 * 설정자: pci_assign_unassigned_root_bus_resources() 가 3차 시도부터
	 *   올려 잡고, pci_assign_unassigned_bridge_resources() 는 처음부터
	 *   이 값을 쓴다(핫플러그 경로는 그 브리지 아래를 어차피 다시
	 *   구성하는 중이라 보수적일 이유가 없다).
	 * 읽는 자: pci_bus_release_bridge_resources() 가 이 값일 때만 하위
	 *   버스로 재귀하고, 리프가 아닌 브리지의 창도 놓는다.
	 * 의미: 가장 과감한 강도. 서브트리를 통째로 비우고 처음부터
	 *   다시 배치한다.
	 * 동기화: 위와 같다. */
};

/*
 * Try to release PCI bridge resources from leaf bridge, so we can allocate
 * a larger window later.
 */
/*
 * [한국어]
 * pci_bus_release_bridge_resources - 재시도를 위해 브리지 창을 재귀적으로 놓는다
 *
 * @bus: 창을 놓을 대상 버스(그 위 브리지의 창을 놓는다).
 * @b_win: 놓을 창 자원.
 * @rel_type: 강도. leaf_only 면 리프 브리지만, whole_subtree 면 서브트리 전체.
 * @return: 없음.
 *
 * 왜 필요한가: 배치에 실패했을 때 공간을 만드는 방법은 "이미 잡혀 있는
 * 브리지 창을 놓고 크기를 다시 계산하는 것"이다. 어디까지 놓을지가
 * 재시도 전략의 강도이며, 이 함수가 그 강도를 실행한다.
 *
 * 왜 아래부터 놓는가(재귀가 먼저): 창을 놓으려면 그 안의 자식이 먼저
 * 정리돼야 한다. 하위 브리지의 창은 이 창의 자식이므로, 아래를 먼저
 * 재귀 처리하고 돌아와서 자기 창을 놓는다. 순서를 뒤집으면
 * release_child_resources() 가 하위 창을 강제로 떼어 내며 하드웨어
 * 레지스터는 그대로 남아 커널과 어긋난다.
 *
 * is_leaf_bridge 의 의미: 순회 중 하위 버스를 가진 장치를 하나라도 만나면
 * 이 버스는 리프가 아니다. leaf_only 강도에서는 리프가 아닌 버스의 창을
 * 놓지 않는다 — 그 아래에 다른 브리지가 매달려 있어 파장이 크기 때문이다.
 * 실행 컨텍스트: 프로세스 문맥. 재귀. config 공간 쓰기.
 *
 * 호출 체인:
 *   pci_prepare_next_assign_round() -> [이 함수]
 *     -> (재귀) -> pci_bridge_release_resources()
 */
static void pci_bus_release_bridge_resources(struct pci_bus *bus,
					     struct resource *b_win,
					     enum release_type rel_type)
{
	/* [한국어] 장치 순회 커서. */
	struct pci_dev *dev;
	/* [한국어] "이 버스 아래에 다른 브리지가 없다"고 낙관적으로 시작해,
	 * 하나라도 발견하면 거짓으로 내린다. */
	bool is_leaf_bridge = true;

	/* [한국어] 이 버스의 장치들을 훑으며 하위 브리지를 찾는다. */
	list_for_each_entry(dev, &bus->devices, bus_list) {
		/* [한국어] 이 장치 아래의 버스(브리지가 아니면 NULL). */
		struct pci_bus *b = dev->subordinate;
		/* [한국어] 하위 버스의 창을 훑을 커서. */
		struct resource *res;

		if (!b)
			/* [한국어] 엔드포인트는 창을 갖지 않는다. */
			continue;

		/* [한국어] 하위 버스를 가진 장치를 만났으므로 이 버스는
		 * 리프가 아니다. 이 대입은 아래 세 continue 보다 먼저
		 * 일어나야 한다 — 어떤 종류의 브리지든, 강도가 무엇이든
		 * "아래에 무언가 있다"는 사실은 변하지 않기 때문이다. */
		is_leaf_bridge = false;

		/* [한국어] 표준 PCI-to-PCI 브리지가 아니면 이 함수의 창
		 * 정리 규칙을 적용하지 않는다(CardBus 등). */
		if ((dev->class >> 8) != PCI_CLASS_BRIDGE_PCI)
			continue;

		/* [한국어] 강도가 leaf_only 면 아래로 내려가지 않는다.
		 * 이 조건이 곧 "리프만 놓는다"를 재귀 차원에서 구현한다. */
		if (rel_type != whole_subtree)
			continue;

		/* [한국어] 하위 버스의 창들을 훑는다. */
		pci_bus_for_each_resource(b, res) {
			/* [한국어] 지금 놓으려는 창(b_win)의 자식인 창만
			 * 대상이다. 자원 트리의 부모 포인터로 확인한다 —
			 * 하위 브리지가 여러 창을 가져도 그중 이 창 안에
			 * 들어 있는 것만 정리하면 된다. 다른 종류의 창
			 * (예: 메모리 창을 놓는 중에 만난 I/O 창)은
			 * 부모가 달라 자연히 걸러진다. */
			if (res->parent != b_win)
				continue;

			/* [한국어] 그 하위 창을 재귀로 먼저 정리한다.
			 * 돌아오면 이 창의 자식이 하나 줄어 있다. */
			pci_bus_release_bridge_resources(b, res, rel_type);
		}
	}

	/* [한국어] 루트 버스에는 놓을 상위 브리지 창이 없다. 재귀의 종점. */
	if (pci_is_root_bus(bus))
		return;

	/* [한국어] 이 버스의 상위 브리지가 표준 PCI-to-PCI 브리지가 아니면
	 * 창 놓기 규칙을 적용하지 않는다. */
	if ((bus->self->class >> 8) != PCI_CLASS_BRIDGE_PCI)
		return;

	/* [한국어] 실제로 창을 놓을 조건. 강도가 whole_subtree 면 무조건 놓고,
	 * leaf_only 면 이 버스가 리프일 때만 놓는다. 위 재귀 덕분에 이
	 * 시점에는 이 창의 자식 창들이 이미 정리돼 있다. */
	if ((rel_type == whole_subtree) || is_leaf_bridge)
		pci_bridge_release_resources(bus, b_win);
}

/*
 * [한국어]
 * pci_bus_dump_res - 한 버스의 유효한 자원을 부팅 로그에 찍는다
 *
 * @bus: 대상 버스.
 * @return: 없음.
 *
 * 왜 필요한가: 자원 배치는 실패해도 조용히 넘어가는 부분이 많아
 * (선택적 자원 포기, 창 축소 등), 최종 결과를 눈으로 확인할 수단이
 * 필요하다. 배치가 전부 끝난 뒤 이 함수가 각 버스의 최종 창 배치를
 * 로그로 남긴다. 장치가 안 뜰 때 dmesg 에서 가장 먼저 보게 되는 정보다.
 * 실행 컨텍스트: 프로세스 문맥. 부작용은 로그 출력뿐.
 *
 * 호출 체인:
 *   pci_bus_dump_resources() -> [이 함수] -> dev_info()
 */
static void pci_bus_dump_res(struct pci_bus *bus)
{
	/* [한국어] 자원 순회 커서. */
	struct resource *res;
	/* [한국어] 자원 슬롯 번호(로그에 함께 찍는다). */
	int i;

	/* [한국어] 버스의 자원 목록을 인덱스와 함께 훑는다. */
	pci_bus_for_each_resource(bus, res, i) {
		/* [한국어] 세 가지를 걸러 낸다: 빈 슬롯, 끝 주소가 0 인 것
		 * (= 실질적으로 크기가 없는 것), 종류 플래그가 없는 것.
		 * 이런 항목까지 찍으면 부팅 로그가 의미 없이 길어진다. */
		if (!res || !res->end || !res->flags)
			continue;

		/* [한국어] "resource N [mem 0x...-0x...]" 형태로 출력한다.
		 * pci_info() 가 아니라 dev_info() 를 쓰는 이유는 대상이
		 * pci_dev 가 아니라 pci_bus 이기 때문이다. */
		dev_info(&bus->dev, "resource %d %pR\n", i, res);
	}
}

/*
 * [한국어]
 * pci_bus_dump_resources - 버스 트리 전체의 자원 배치를 재귀로 찍는다
 *
 * @bus: 시작 버스(보통 루트 버스).
 * @return: 없음.
 *
 * 왜 필요한가: 배치 결과를 트리 순서대로 보여 주어, 상위 창과 그 안의
 * 하위 창이 어떻게 포개져 있는지를 로그에서 바로 읽을 수 있게 한다.
 * 위에서 아래로(자기 먼저, 자식 나중) 찍으므로 로그의 순서가 곧
 * 포함 관계의 순서가 된다.
 * 실행 컨텍스트: 프로세스 문맥. 재귀.
 *
 * 호출 체인:
 *   pci_assign_unassigned_root_bus_resources() -> [이 함수]
 *     -> pci_bus_dump_res(), (재귀)
 */
static void pci_bus_dump_resources(struct pci_bus *bus)
{
	/* [한국어] 하위 버스 커서. */
	struct pci_bus *b;
	/* [한국어] 장치 순회 커서. */
	struct pci_dev *dev;


	/* [한국어] 자기 버스의 자원을 먼저 찍는다(상위가 위에 오도록). */
	pci_bus_dump_res(bus);

	/* [한국어] 장치를 훑으며 브리지를 찾아 아래로 내려간다. */
	list_for_each_entry(dev, &bus->devices, bus_list) {
		/* [한국어] 이 장치 아래의 버스. */
		b = dev->subordinate;
		if (!b)
			/* [한국어] 엔드포인트에는 더 내려갈 버스가 없다. */
			continue;

		/* [한국어] 하위 버스로 재귀. */
		pci_bus_dump_resources(b);
	}
}

/*
 * [한국어]
 * pci_bus_get_depth - 버스 트리의 최대 깊이를 재귀로 구한다
 *
 * @bus: 시작 버스.
 * @return: 이 버스 아래의 최대 깊이. 자식 버스가 하나도 없으면 0.
 *
 * 왜 필요한가: 재시도 횟수의 상한을 정하는 데 쓴다.
 * pci_assign_unassigned_root_bus_resources() 는 시도할 때마다 브리지 창을
 * 한 층씩 더 깊이 놓아 가며 공간을 만드는데, 트리 깊이보다 더 시도하는
 * 것은 의미가 없다(더 놓을 층이 없다). 그래서 pci_try_num = 깊이 + 1 로
 * 잡는다. 무한 재시도를 막는 자연스러운 상한이기도 하다.
 * 실행 컨텍스트: 순수 계산. 재귀. 락 없음.
 *
 * 호출 체인:
 *   pci_assign_unassigned_root_bus_resources() -> [이 함수] -> (재귀)
 */
static int pci_bus_get_depth(struct pci_bus *bus)
{
	/* [한국어] 지금까지 발견한 최대 깊이. 자식이 없으면 0 으로 끝난다. */
	int depth = 0;
	/* [한국어] 자식 버스 순회 커서. */
	struct pci_bus *child_bus;

	/* [한국어] 자식 버스 목록을 훑는다. 장치 목록이 아니라 버스 목록을
	 * 쓰는 이유는 깊이의 단위가 버스이기 때문이다. */
	list_for_each_entry(child_bus, &bus->children, node) {
		/* [한국어] 이 자식 아래의 깊이를 받을 자리. */
		int ret;

		/* [한국어] 자식의 깊이를 재귀로 구한다. */
		ret = pci_bus_get_depth(child_bus);
		/* [한국어] 자식 깊이 + 1(자식으로 내려가는 한 층)이 지금까지의
		 * 최대보다 크면 갱신한다. 여러 자식 중 가장 깊은 쪽을 택하는
		 * 것이므로 max 와 같은 계산이다. */
		if (ret + 1 > depth)
			depth = ret + 1;
	}

	/* [한국어] 이 버스를 뿌리로 한 서브트리의 최대 깊이. */
	return depth;
}

/*
 * -1: undefined, will auto detect later
 *  0: disabled by user
 *  1: disabled by auto detect
 *  2: enabled by user
 *  3: enabled by auto detect
 */
/* [한국어]
 * enum enable_type - 자원 재할당(realloc) 정책의 상태
 *
 * "재할당"이란 펌웨어가 잡아 둔 배치를 무시하고 커널이 전부 다시
 * 계산하는 동작이다. 켜면 배치가 더 조밀해지고 핫플러그 여유도 잡히지만,
 * 펌웨어에 의존하던 장치가 깨질 위험이 있어 기본으로 켜지 않는다.
 * 값이 넷이 아니라 "사용자가 정했는가 / 커널이 스스로 판단했는가"까지
 * 구분해 다섯인 이유는, 배치에 실패했을 때 사용자에게 어떤 안내를
 * 띄울지가 달라지기 때문이다(pci_assign_unassigned_root_bus_resources 의
 * 마지막 메시지 두 개 참조).
 * 값의 배치가 정교하다: user_enabled 이상이 "켜짐"이라
 * pci_realloc_enabled() 가 단순 비교 하나로 판정한다.
 * 바로 위 원문 주석이 각 값의 의미를 한 줄씩 밝히고 있다.
 */
enum enable_type {
	undefined = -1,
	/* [한국어] 아직 아무것도 정해지지 않은 초기 상태(-1).
	 * 설정자: 전역 pci_realloc_enable 의 초기값.
	 * 읽는 자: pci_realloc_detect() 가 이 값일 때만 자동 판정을 하고,
	 *   pci_assign_unassigned_root_bus_resources() 는 실패 시 이 값이면
	 *   "pci=realloc 으로 부팅해 보라"는 안내를 띄운다.
	 * 값 범위: -1 로 명시 지정. 나머지 값들이 0 부터 이어지도록 하는
	 *   출발점이며, 음수라서 pci_realloc_enabled() 의 비교에서 자연히
	 *   "꺼짐" 쪽에 떨어진다.
	 * 동기화: 부팅 초기에 한 번 정해지고 이후 읽기만 한다. */

	user_disabled,
	/* [한국어] 사용자가 pci=realloc=off 로 명시적으로 끈 상태(0).
	 * 설정자: pci_realloc_get_opt() 가 "off" 문자열을 만났을 때.
	 * 읽는 자: pci_realloc_detect() 가 undefined 가 아니면 그대로
	 *   돌려주므로, 이 값이면 자동 판정을 건너뛴다 — 사용자의 명시적
	 *   결정을 커널이 뒤집지 않는다는 원칙이다.
	 * 값 범위: 0 (enum 의 기본 증가).
	 * 동기화: 위와 같다. */

	auto_disabled,
	/* [한국어] 커널이 스스로 판단해 끈 상태(1).
	 * 설정자: pci_realloc_detect() 가 호스트 브리지의 preserve_config
	 *   플래그를 보고 결정한다("펌웨어 설정을 보존하라"는 뜻).
	 * 읽는 자: pci_realloc_enabled() 의 비교에서 "꺼짐"으로 판정된다.
	 * 값 범위: 1.
	 * 동기화: 위와 같다. */

	user_enabled,
	/* [한국어] 사용자가 pci=realloc 또는 pci=realloc=on 으로 켠 상태(2).
	 * 설정자: pci_realloc_get_opt() 가 "on" 문자열을 만났을 때.
	 * 읽는 자: pci_realloc_enabled() 의 경계값이다 — 이 값 이상이면
	 *   "켜짐"이다.
	 * 값 범위: 2. 이 값이 경계가 되도록 enum 순서를 짠 것이 요점이다.
	 * 동기화: 위와 같다. */

	auto_enabled,
	/* [한국어] 커널이 스스로 판단해 켠 상태(3).
	 * 설정자: pci_realloc_detect() 가 SR-IOV VF BAR 중 배정되지 않은
	 *   것을 발견했을 때(CONFIG_PCI_IOV 와 CONFIG_PCI_REALLOC_ENABLE_AUTO
	 *   가 모두 켜진 커널에서만).
	 * 읽는 자: pci_realloc_enabled() 에서 "켜짐"으로 판정되고,
	 *   실패 시에는 "문제가 있으면 pci=realloc=off 로 꺼 보라"는
	 *   반대 방향의 안내가 나간다.
	 * 값 범위: 3.
	 * 동기화: 위와 같다. */
};

/* [한국어] 재할당 정책 전역 상태.
 * 설정자: pci_realloc_get_opt() (부팅 파라미터 파싱 시점).
 * 읽는 자: pci_assign_unassigned_root_bus_resources() 가
 *   pci_realloc_detect() 에 넘겨 최종 판정을 받는다.
 * 값 범위: 위 enum 의 다섯 값. 초기값 undefined.
 * 동기화: 부팅 초기에 한 번 쓰이고 이후 읽기만 하므로 락이 없다. */
static enum enable_type pci_realloc_enable = undefined;
/*
 * [한국어]
 * pci_realloc_get_opt - 부팅 파라미터 pci=realloc=... 을 해석한다
 *
 * @str: "pci=realloc" 다음에 오는 문자열. 호출부
 *       (drivers/pci/pci.c:14025)는 "=" 뒤 부분을 잘라 넘기고,
 *       값 없이 "pci=realloc" 만 쓴 경우에는 "on" 을 대신 넘긴다
 *       (같은 파일 14028행).
 * @return: 없음. 결과는 전역 pci_realloc_enable 에 남는다.
 *
 * 왜 필요한가: 자원 재할당은 위험을 동반하므로 기본으로 켜지 않고
 * 사용자가 결정할 수 있게 열어 둔다. 이 함수가 그 스위치의 파서다.
 * 인식하지 못하는 문자열은 조용히 무시해 전역이 undefined 로 남고,
 * 그러면 커널의 자동 판정이 나중에 결정한다.
 * 실행 컨텍스트: 부팅 초기 파라미터 파싱 단계. __init 라 초기화가
 *   끝나면 이 코드의 메모리는 회수된다. 단일 스레드이므로 락이 없다.
 *
 * 호출 체인:
 *   drivers/pci/pci.c 의 pci= 파라미터 파서 -> [이 함수]
 */
void __init pci_realloc_get_opt(char *str)
{
	/* [한국어] "off" 세 글자를 비교한다. strncmp 는 같으면 0 이므로
	 * ! 를 붙여 "같다"를 참으로 만든다. 길이를 3 으로 제한해
	 * "off,something" 처럼 뒤에 다른 것이 붙어도 인식한다. */
	if (!strncmp(str, "off", 3))
		pci_realloc_enable = user_disabled;
	/* [한국어] "on" 두 글자. 사용자가 명시적으로 켠 것이므로
	 * auto_enabled 가 아니라 user_enabled 로 기록한다 — 나중에
	 * 실패 안내 문구가 달라진다. */
	else if (!strncmp(str, "on", 2))
		pci_realloc_enable = user_enabled;
}
/*
 * [한국어]
 * pci_realloc_enabled - 주어진 정책 상태가 "켜짐"인지 판정한다
 *
 * @enable: 판정할 상태값(보통 pci_realloc_detect() 의 반환값).
 * @return: true 면 재할당을 수행한다.
 *
 * 왜 필요한가: 다섯 개의 상태값을 "켜짐/꺼짐" 둘로 접는 곳을 한 군데로
 * 모아, enum 순서에 대한 의존을 이 함수 안에만 가둔다.
 * 실행 컨텍스트: 순수 판정.
 *
 * 호출 체인:
 *   pci_assign_unassigned_root_bus_resources() -> [이 함수]
 */
static bool pci_realloc_enabled(enum enable_type enable)
{
	/* [한국어] enum 값이 user_enabled(2) 이상이면 켜짐이다.
	 * undefined(-1), user_disabled(0), auto_disabled(1) 은 모두 그보다
	 * 작아 자연히 꺼짐으로 떨어진다. enum 의 값 배치가 이 한 줄
	 * 비교를 위해 설계되었다. */
	return enable >= user_enabled;
}

/* [한국어] 이 블록은 두 설정이 모두 켜졌을 때만 컴파일된다.
 *   CONFIG_PCI_IOV — SR-IOV 지원. 없으면 VF BAR 자체가 존재하지 않아
 *     아래 판정이 의미가 없다.
 *   CONFIG_PCI_REALLOC_ENABLE_AUTO — 커널이 스스로 재할당을 켜도 되는가.
 *     꺼져 있으면 자동 판정을 하지 않겠다는 뜻이므로 판정 코드가 불필요하다.
 * 아래 #else 에 같은 이름의 빈 스텁이 있어, 호출부는 설정과 무관하게
 * 같은 코드를 쓴다. */
#if defined(CONFIG_PCI_IOV) && defined(CONFIG_PCI_REALLOC_ENABLE_AUTO)
/*
 * [한국어]
 * iov_resources_unassigned - 배정되지 않은 SR-IOV VF BAR 를 찾는 순회 콜백
 *
 * @dev: pci_walk_bus() 가 넘겨 주는 현재 장치.
 * @data: 호출자가 넘긴 bool 포인터. 발견하면 true 로 채운다(출력 인자).
 * @return: 0 이면 순회 계속, 0 이 아니면 pci_walk_bus() 가 즉시 중단한다.
 *
 * 왜 필요한가: SR-IOV VF BAR 는 선택적 자원이라 첫 배치에서 자리를 못 잡는
 * 일이 흔하다. 그런데 VF 를 쓰려는 시스템이라면 그 자원이 실제로 필요하다.
 * 그래서 "배정되지 않은 VF BAR 가 하나라도 있으면 재할당을 켜서 다시
 * 해 보자"는 자동 판정의 근거를 이 콜백이 수집한다.
 *
 * 왜 region.start 로 판정하는가: 여기서는 resource_assigned() 대신
 * 버스 주소로 변환한 시작 주소가 0 인지를 본다. VF BAR 는 물리 기능(PF)이
 * 대표로 들고 있는 자원이라 자원 트리 편입 여부만으로는 판단이 어렵고,
 * "버스 주소가 0" 이 곧 "주소를 못 받았다"의 실질적 표시이기 때문이다.
 * 실행 컨텍스트: 프로세스 문맥. pci_walk_bus() 가 버스 트리를 훑으며
 *   장치마다 이 콜백을 부른다(내부에서 버스 목록 보호를 담당한다).
 *
 * 호출 체인:
 *   pci_realloc_detect() -> pci_walk_bus() -> [이 콜백]
 */
static int iov_resources_unassigned(struct pci_dev *dev, void *data)
{
	/* [한국어] VF BAR 번호(0~5) 순회 인덱스. */
	int i;
	/* [한국어] 호출자의 bool 변수를 가리킨다. void * 로 받은 것을
	 * 실제 타입으로 되돌리는 것 — pci_walk_bus() 콜백 규약이 그렇다. */
	bool *unassigned = data;

	/* [한국어] VF 도 표준 장치처럼 BAR 를 여섯 칸 갖는다. */
	for (i = 0; i < PCI_SRIOV_NUM_BARS; i++) {
		/* [한국어] VF BAR 번호를 resource[] 배열의 실제 인덱스로
		 * 바꾼다. VF BAR 는 일반 BAR 와 다른 구획에 따로 놓인다. */
		int idx = pci_resource_num_from_vf_bar(i);
		/* [한국어] 그 인덱스의 자원. */
		struct resource *r = &dev->resource[idx];
		/* [한국어] 버스 주소로 변환한 결과를 담을 자리. */
		struct pci_bus_region region;

		/* Not assigned or rejected by kernel? */
		/* [한국어] 플래그가 비어 있으면 이 VF BAR 는 하드웨어가
		 * 구현하지 않았거나 커널이 무효화한 것이다. 판정 대상이 아니다. */
		if (!r->flags)
			continue;

		/* [한국어] CPU 물리 주소를 버스 주소로 변환한다. 주소 변환이
		 * 있는 플랫폼에서는 물리 주소가 0 이 아니어도 버스 주소가
		 * 0 일 수 있고, 그 반대도 가능하다. 장치가 실제로 보는 값은
		 * 버스 주소이므로 그쪽으로 판정한다. */
		pcibios_resource_to_bus(dev->bus, &region, r);
		/* [한국어] 버스 주소 0 = 주소를 받지 못했다. */
		if (!region.start) {
			/* [한국어] 호출자에게 발견 사실을 전달한다. */
			*unassigned = true;
			/* [한국어] 하나만 찾으면 결론이 나므로 순회를 즉시
			 * 중단한다. 0 이 아닌 반환이 pci_walk_bus() 에게
			 * "그만 훑으라"는 신호다(오른쪽 원문 주석 참조). */
			return 1; /* Return early from pci_walk_bus() */
		}
	}

	/* [한국어] 이 장치에서는 못 찾았다. 다음 장치로 순회를 계속한다. */
	return 0;
}

/*
 * [한국어]
 * pci_realloc_detect - 재할당을 켤지 자동으로 판정한다 (기능 활성 판)
 *
 * @bus: 판정 대상 루트 버스.
 * @enable_local: 현재 정책 상태(보통 전역 pci_realloc_enable 의 값).
 * @return: 최종 정책 상태. 사용자가 이미 정했으면 그대로,
 *          아니면 자동 판정 결과(auto_disabled / auto_enabled) 또는
 *          판정 불가 시 받은 값 그대로.
 *
 * 왜 필요한가: 사용자가 명시하지 않았을 때 커널이 스스로 "재할당이
 * 필요해 보이는가"를 판단한다. 판단 근거는 두 가지다.
 *   (1) 호스트 브리지가 preserve_config 를 요구하면 -> 끈다.
 *       펌웨어 설정을 보존하라는 명시적 지시이므로 재배치하면 안 된다.
 *   (2) 배정되지 않은 SR-IOV VF BAR 가 있으면 -> 켠다.
 *       공간이 모자란 상태라는 신호이므로 다시 계산할 값어치가 있다.
 * 사용자의 명시적 결정을 절대 뒤집지 않는 것이 첫 번째 검사의 목적이다.
 * 실행 컨텍스트: 프로세스 문맥. pci_walk_bus() 로 트리 전체를 훑는다.
 *
 * 호출 체인:
 *   pci_assign_unassigned_root_bus_resources() -> [이 함수]
 *     -> pci_find_host_bridge(), pci_walk_bus() -> iov_resources_unassigned()
 */
static enum enable_type pci_realloc_detect(struct pci_bus *bus,
					   enum enable_type enable_local)
{
	/* [한국어] 콜백이 채워 줄 발견 여부. 거짓에서 시작한다. */
	bool unassigned = false;
	/* [한국어] 이 버스를 제공하는 호스트 브리지. */
	struct pci_host_bridge *host;

	/* [한국어] 사용자가 이미 결정했거나(user_*) 앞선 판정이 있었으면
	 * 자동 판정을 하지 않고 그대로 존중한다. */
	if (enable_local != undefined)
		return enable_local;

	/* [한국어] 이 버스의 호스트 브리지를 찾는다. */
	host = pci_find_host_bridge(bus);
	/* [한국어] "펌웨어가 설정한 자원 배치를 보존하라"는 요구가 있으면
	 * 재할당은 그 요구와 정면으로 충돌한다. 자동으로 끈다. */
	if (host->preserve_config)
		return auto_disabled;

	/* [한국어] 버스 트리 전체를 훑으며 배정되지 않은 VF BAR 를 찾는다.
	 * 세 번째 인자가 콜백에 그대로 전달되는 사용자 데이터다. */
	pci_walk_bus(bus, iov_resources_unassigned, &unassigned);
	/* [한국어] 하나라도 찾았으면 공간이 부족한 상태다. 재할당을 켜서
	 * 다시 계산할 값어치가 있다고 판단한다. */
	if (unassigned)
		return auto_enabled;

	/* [한국어] 켤 근거를 못 찾았다. 받은 값(undefined)을 그대로 돌려준다.
	 * auto_disabled 로 바꾸지 않는 이유: 그렇게 하면
	 * pci_assign_unassigned_root_bus_resources() 가 실패했을 때
	 * "pci=realloc 으로 부팅해 보라"는 안내를 띄우지 못한다. 그 안내는
	 * undefined 일 때만 나간다. */
	return enable_local;
}
/* [한국어] 아래는 SR-IOV 나 자동 재할당이 꺼진 커널을 위한 스텁이다.
 * 판정할 근거 자체가 없으므로 아무 판단도 하지 않는다. */
#else
/*
 * [한국어]
 * pci_realloc_detect - 자동 판정 스텁 (CONFIG_PCI_IOV 또는
 *                      CONFIG_PCI_REALLOC_ENABLE_AUTO 가 꺼진 경우)
 *
 * @bus: 쓰이지 않는다.
 * @enable_local: 현재 정책 상태.
 * @return: 받은 값을 그대로 돌려준다 = 아무 판단도 하지 않는다.
 *
 * 왜 필요한가: 호출부가 #ifdef 로 갈라지지 않게 하려는 것이다. 위 활성
 * 판과 시그니처가 같아, pci_assign_unassigned_root_bus_resources() 는
 * 커널 설정과 무관하게 똑같이 이 함수를 부르면 된다.
 * 이 경우 정책은 오직 사용자의 부팅 파라미터로만 정해진다.
 * 실행 컨텍스트: 순수 통과. 부작용 없음.
 *
 * 호출 체인:
 *   pci_assign_unassigned_root_bus_resources() -> [이 함수]
 */
static enum enable_type pci_realloc_detect(struct pci_bus *bus,
					   enum enable_type enable_local)
{
	/* [한국어] 판정 근거가 없으므로 받은 값을 그대로 통과시킨다. */
	return enable_local;
}
/* [한국어] CONFIG_PCI_IOV && CONFIG_PCI_REALLOC_ENABLE_AUTO 분기 끝. */
#endif

/*
 * [한국어]
 * adjust_bridge_window - 분배 결과에 맞춰 브리지 창의 목표 크기를 조정한다
 *
 * @bridge: 창을 소유한 브리지.
 * @res: 조정할 창 자원.
 * @add_list: 선택적 요구 목록. 창을 줄일 때 그 목록의 add_size 를 깎는 데
 *        쓴다. NULL 이면 줄이기를 포기한다(깎을 곳이 없으므로).
 * @new_size: 여유 분배 계산이 이 창에 배정한 크기.
 * @return: 없음.
 *
 * 왜 필요한가: 여유 분배 pass(pci_bus_distribute_available_resources)가
 * "이 창에는 이만큼 줄 수 있다"를 계산하면, 그 결과를 창의 목표 크기에
 * 반영해야 한다. 늘리는 경우와 줄이는 경우의 처리가 크게 다르다.
 *
 * 늘리는 경우: 그냥 크기를 키우면 된다(아래 resource_set_size).
 * 줄이는 경우: 조심스럽다. 필수 크기까지 깎아 버리면 장치가 못 들어간다.
 *   그래서 "핫플러그 예비로 얹어 둔 몫" 범위 안에서만 줄인다. 그 판단이
 *   아래 switch 문의 크기 비교다 — 현재 크기가 핫플러그 예비 크기보다
 *   크다면 필수 몫이 섞여 있다는 뜻이라 손대지 않는다.
 *
 * 마지막에 add_list 에서 항목을 제거하는 이유: 창 크기가 이 함수로
 * 확정됐으니, 나중에 또 늘려 달라는 요구가 남아 있으면 안 된다.
 * 실행 컨텍스트: 프로세스 문맥. 락 없음.
 *
 * 호출 체인:
 *   pci_bus_distribute_available_resources() -> [이 함수]
 *     -> res_to_dev_res(), pci_dev_res_remove_from_list(), resource_set_size()
 */
static void adjust_bridge_window(struct pci_dev *bridge, struct resource *res,
				 struct list_head *add_list,
				 resource_size_t new_size)
{
	/* [한국어] add_size 는 늘거나 준 차액, size 는 현재 크기. */
	resource_size_t add_size, size = resource_size(res);
	/* [한국어] add_list 에서 찾아낸 이 창의 요구 항목. */
	struct pci_dev_resource *dev_res;

	/* [한국어] 이미 주소가 확정된 창은 크기를 바꿀 수 없다. */
	if (resource_assigned(res))
		return;

	/* [한국어] 배정할 크기가 0 이면(그 종류의 공간이 남지 않았으면)
	 * 창을 0 으로 만들지 말고 현재 상태를 유지한다. 크기를 0 으로
	 * 밀면 이미 계산해 둔 필수 크기를 잃는다. */
	if (!new_size)
		return;

	/* [한국어] 늘리는 경우. */
	if (new_size > size) {
		/* [한국어] 늘어난 양을 기록해 로그에 남긴다. */
		add_size = new_size - size;
		/* [한국어] %pa 는 물리 주소/크기 타입을 폭에 맞게 찍는 커널
		 * 포맷 지정자다. 값이 아니라 주소를 넘겨야 해서 & 를 붙인다. */
		pci_dbg(bridge, "bridge window %pR extended by %pa\n", res,
			&add_size);
	/* [한국어] 줄이는 경우. 아래에서 "줄여도 되는가"를 까다롭게 따진다. */
	} else if (new_size < size) {
		/* [한국어] 창의 종류를 알아야 어떤 핫플러그 예비 크기와
		 * 비교할지 정할 수 있다. */
		int idx = pci_resource_num(bridge, res);

		/*
		 * hpio/mmio/mmioprefsize hasn't been included at all? See the
		 * add_size param at the callsites of calculate_memsize().
		 */
		/* [한국어] 선택적 요구 목록이 없으면, 위 원문 주석이 말하듯
		 * 핫플러그 예비 몫이 애초에 창 크기에 반영되지 않았다는 뜻이다
		 * (pbus_size_* 는 add_list 가 있을 때만 예비를 선택 몫으로
		 * 분리한다). 그렇다면 지금 크기는 전부 필수분이라 줄이면 안 된다. */
		if (!add_list)
			return;

		/* Only shrink if the hotplug extra relates to window size. */
		/* [한국어] 창 종류별로 대응하는 핫플러그 예비 크기와 비교한다.
		 * 현재 크기가 예비 크기보다 크면 그 안에 필수분이 섞여 있다는
		 * 뜻이므로 줄이기를 포기하고 돌아간다. 반대로 예비 크기 이하면
		 * 이 창은 사실상 예비 목적으로만 잡혀 있어 줄여도 잃을 것이 없다. */
		switch (idx) {
			case PCI_BRIDGE_IO_WINDOW:
				/* [한국어] I/O 창 — pci_hotplug_io_size 와 비교. */
				if (size > pci_hotplug_io_size)
					return;
				break;
			case PCI_BRIDGE_MEM_WINDOW:
				/* [한국어] non-prefetchable 메모리 창. */
				if (size > pci_hotplug_mmio_size)
					return;
				break;
			case PCI_BRIDGE_PREF_MEM_WINDOW:
				/* [한국어] prefetchable 메모리 창. */
				if (size > pci_hotplug_mmio_pref_size)
					return;
				break;
			default:
				/* [한국어] 브리지 창 세 종류 밖의 인덱스.
				 * 호출부가 항상 세 창 중 하나를 넘기므로
				 * 도달하지 않지만, switch 를 닫아 둔다. */
				break;
		}

		/* [한국어] 이 창의 선택적 요구 항목을 찾는다. 위 검사들을
		 * 통과했다는 것은 이 창이 핫플러그 예비 몫으로 잡혀 있다는
		 * 뜻이라, 그 몫이 add_list 에 실려 있으리라 전제한다. */
		dev_res = res_to_dev_res(add_list, res);
		/* [한국어] 줄여야 할 양. */
		add_size = size - new_size;
		/* [한국어] 줄일 양이 등록된 선택 몫보다 작으면, 선택 몫에서
		 * 그만큼만 깎아 내면 된다. 창 자체의 크기(res)는 건드리지
		 * 않는다 — 필수 크기는 그대로 두고 "더 달라"는 요구만
		 * 줄이는 것이다. */
		if (add_size < dev_res->add_size) {
			dev_res->add_size -= add_size;
			pci_dbg(bridge, "bridge window %pR optional size shrunken by %pa\n",
				res, &add_size);
		} else {
			/* [한국어] 줄일 양이 선택 몫 전체 이상이면 요구를
			 * 통째로 없앤다. 그 이상은 필수분이라 더 깎을 수 없다. */
			pci_dbg(bridge, "bridge window %pR optional size removed\n",
				res);
			pci_dev_res_remove_from_list(add_list, res);
		}
		/* [한국어] 줄이기 경로는 여기서 끝난다. 아래
		 * resource_set_size() 는 늘리는 경우만을 위한 것이다. */
		return;

	} else {
		/* [한국어] new_size == size. 바꿀 것이 없으므로 그대로 둔다.
		 * add_list 에서 요구를 지우지도 않는데, 아직 이 창의 크기가
		 * 이 함수로 확정된 것이 아니기 때문이다. */
		return;
	}

	/* [한국어] 여기까지 오는 것은 "늘리는 경우"뿐이다. 창의 크기를
	 * 새 값으로 설정한다. resource_set_size() 는 start 를 유지한 채
	 * end 를 옮겨 크기만 바꾼다. */
	resource_set_size(res, new_size);

	/* If the resource is part of the add_list, remove it now */
	/* [한국어] 크기가 확정됐으니 "더 달라"는 요구는 소멸한다. 남겨 두면
	 * 나중에 reassign_resources_sorted() 가 또 늘리려 하고, 상위 호출부의
	 * WARN_ON_ONCE(add_list 가 비었는지 확인) 에도 걸린다. */
	if (add_list)
		pci_dev_res_remove_from_list(add_list, res);
}

/*
 * [한국어]
 * remove_dev_resource - 남은 여유 공간에서 자원 하나가 차지할 몫을 뺀다
 *
 * @avail: [입출력] 남은 여유 공간을 나타내는 임시 resource. start 를 앞으로
 *         밀어 "쓴 만큼 줄이는" 방식으로 표현한다.
 * @dev: 자원의 소유 장치.
 * @res: 공간을 차지할 자원.
 * @return: 없음. 결과는 avail->start 갱신으로 나타난다.
 *
 * 왜 필요한가: 여유 분배 pass 는 "이 버스에서 아래로 넘겨줄 수 있는 공간이
 * 얼마인가"를 알아야 한다. 그 답은 "전체 창 - 이 버스의 장치들이 쓸 몫"이다.
 * 이 함수가 그 뺄셈을 자원 하나 단위로 수행한다.
 *
 * 왜 정렬 낭비까지 빼는가: 자원은 자기 정렬 경계에서만 시작할 수 있다.
 * 현재 avail->start 가 그 경계가 아니면, 경계까지 밀리는 만큼의 공간이
 * 쓸모없이 버려진다. 그 낭비를 계산에 넣지 않으면 실제보다 여유가 많다고
 * 착각해 아래에 과하게 배분하게 된다.
 * 계산: align 을 "정렬까지 밀리는 바이트 수"로 다시 쓰고(ALIGN 결과에서
 * 현재 위치를 뺀 값), 거기에 자원 크기를 더한 만큼 start 를 전진시킨다.
 *
 * 왜 min 으로 상한을 거는가: 여유보다 더 큰 자원을 만나면 start 가 end 를
 * 넘어가 버린다. resource_size() 는 부호 없는 뺄셈이라 그 상태에서
 * 거대한 값이 나올 수 있다. end + 1 로 잘라 두면 크기가 정확히 0 이 되어
 * "여유 없음"을 올바르게 나타낸다.
 * 실행 컨텍스트: 순수 계산. 락 없음.
 *
 * 호출 체인:
 *   remove_dev_resources() -> [이 함수] -> pci_resource_alignment()
 */
static void remove_dev_resource(struct resource *avail, struct pci_dev *dev,
				struct resource *res)
{
	/* [한국어] size 는 자원 크기, align 은 정렬(뒤에 "밀리는 양"으로
	 * 재사용된다), tmp 는 최종적으로 소모되는 총 바이트 수. */
	resource_size_t size, align, tmp;

	/* [한국어] 이 자원이 요구하는 크기. */
	size = resource_size(res);
	if (!size)
		/* [한국어] 크기 0 인 자원은 공간을 차지하지 않는다. */
		return;

	/* [한국어] 이 자원의 정렬 요구를 얻는다. */
	align = pci_resource_alignment(dev, res);
	/* [한국어] align 변수의 의미를 여기서 바꾼다 — "정렬 값"에서
	 * "정렬 경계까지 밀리는 바이트 수"로. ALIGN(x, a) 는 x 를 a 의
	 * 배수로 올림하므로, 거기서 x 를 빼면 낭비되는 앞부분 크기가 된다.
	 * 정렬 요구가 0 인 자원(있어서는 안 되지만 방어적으로)은 낭비 0. */
	align = align ? ALIGN(avail->start, align) - avail->start : 0;
	/* [한국어] 실제로 소모되는 총량 = 정렬 낭비 + 자원 크기. */
	tmp = align + size;
	/* [한국어] 여유의 시작점을 그만큼 전진시킨다. end + 1 을 상한으로
	 * 두어 시작점이 끝을 넘어가지 않게 한다 — 넘어가면 부호 없는
	 * 뺄셈에서 크기가 거대한 값으로 계산되기 때문이다. 정확히
	 * end + 1 이 되면 크기가 0 = 여유 없음이 된다. */
	avail->start = min(avail->start + tmp, avail->end + 1);
}

/*
 * [한국어]
 * remove_dev_resources - 한 장치가 쓸 몫을 세 종류의 여유에서 각각 뺀다
 *
 * @dev: 대상 장치.
 * @available: [입출력] 창 종류별 남은 여유 배열. 인덱스는 브리지 창 번호에서
 *        PCI_BRIDGE_RESOURCES 를 뺀 값(즉 0=IO, 1=MEM, 2=PREF MEM 에 대응).
 *        배열 크기 PCI_P2P_BRIDGE_RESOURCE_NUM 은 브리지 창의 개수이며,
 *        그 매크로의 정의는 이 트리에 없는 include/linux/pci.h 에 있어
 *        값 자체는 여기서 확인할 수 없다. 다만 이 파일의 사용 방식
 *        (PCI_BRIDGE_RESOURCES + i 로 세 창을 훑는다)으로 보아 세 창에
 *        대응하는 개수다.
 * @return: 없음.
 *
 * 왜 필요한가: 장치의 자원마다 갈 창이 다르므로, 여유도 창별로 따로
 * 관리해야 한다. 이 함수는 자원 하나하나에 대해 "어느 창으로 가는가"를
 * 물어 그 창의 여유에서 몫을 뺀다.
 * 실행 컨텍스트: 프로세스 문맥. 락 없음.
 *
 * 호출 체인:
 *   pci_bus_distribute_available_resources() -> [이 함수]
 *     -> pbus_select_window(), remove_dev_resource()
 */
static void remove_dev_resources(struct pci_dev *dev,
				 struct resource available[PCI_P2P_BRIDGE_RESOURCE_NUM])
{
	/* [한국어] res 는 자원 순회 커서, b_win 은 그 자원이 갈 창. */
	struct resource *res, *b_win;
	/* [한국어] available[] 의 첨자. */
	int idx;

	/* [한국어] 장치의 resource[] 전체를 훑는다. */
	pci_dev_for_each_resource(dev, res) {
		/* [한국어] 이 자원이 담길(또는 이미 담긴) 상위 브리지 창을
		 * 고른다. 이미 배정된 자원이면 실제 부모를 돌려주므로
		 * 사실에 근거한 답이 된다. */
		b_win = pbus_select_window(dev->bus, res);
		if (!b_win)
			/* [한국어] 담을 창이 없는 자원(종류가 IO/MEM 이 아니거나
			 * 빈 슬롯)은 여유를 소모하지 않는다. */
			continue;

		/* [한국어] 그 창이 상위 브리지의 resource[] 에서 몇 번인지
		 * 역산한다. dev->bus->self 가 창의 소유자인 브리지다. */
		idx = pci_resource_num(dev->bus->self, b_win);
		/* [한국어] 브리지 창 구획의 시작 번호를 빼서 0 기반 첨자로
		 * 만든다. 이렇게 해야 available[] 배열(크기가 창 개수)의
		 * 첨자로 쓸 수 있다. */
		idx -= PCI_BRIDGE_RESOURCES;

		/* [한국어] 해당 종류의 여유에서 이 자원의 몫(정렬 낭비 포함)을
		 * 뺀다. */
		remove_dev_resource(&available[idx], dev, res);
	}
}

/* [한국어] 정렬 값이 0 이 아닐 때만 내림 정렬하고, 0 이면 원래 값을
 * 그대로 두는 매크로.
 * 왜 필요한가: ALIGN_DOWN(x, 0) 은 0 으로 나누는 것과 같은 계산이라
 * 정의되지 않은 동작이 된다. 브리지 창의 정렬이 0 으로 나오는 경우
 * (창이 없거나 아직 설정되지 않은 경우)에도 안전하게 통과시키기 위해
 * 삼항 연산자로 감쌌다.
 * 왜 내림(DOWN)인가: 여유 공간을 나눠 줄 때 올림을 하면 실제로 가진
 * 것보다 많이 배분하게 된다. 아래 호출부의 주석도 "going above what is
 * available" 을 피하려는 것이라고 밝힌다.
 * 인자를 각각 괄호로 감싼 것은 매크로 전개 시 연산자 우선순위 사고를
 * 막는 표준 관용이다. 백슬래시 줄 잇기 중간에는 주석을 넣을 수 없어
 * 설명을 위쪽에 모아 둔다. */
#define ALIGN_DOWN_IF_NONZERO(addr, align) \
			((align) ? ALIGN_DOWN((addr), (align)) : (addr))

/*
 * io, mmio and mmio_pref contain the total amount of bridge window space
 * available. This includes the minimal space needed to cover all the
 * existing devices on the bus and the possible extra space that can be
 * shared with the bridges.
 */
/*
 * [한국어]
 * pci_bus_distribute_available_resources - 남는 공간을 아래 브리지들에 미리 나눠 준다
 *
 * @bus: 분배를 시작할 버스.
 * @add_list: 선택적 요구 목록. 창 크기를 조정할 때 함께 갱신된다.
 * @available_in: 이 버스가 위에서 물려받은, 창 종류별 사용 가능 공간.
 *        호출자의 배열을 그대로 참조하지 않고 아래에서 지역 배열로 복사한다.
 * @return: 없음. 결과는 각 브리지 창의 크기와 add_list 에 반영된다.
 *
 * 왜 필요한가 — "3 passes" 중 두 번째 pass 다.
 * 크기 산정이 끝나면 필수 크기는 정해지지만, 창에 남는 자리가 있을 수 있다.
 * 그 자리를 지금 핫플러그 브리지에 미리 나눠 주면, 나중에 장치를 꽂았을 때
 * 상위 창을 다시 배치하지 않고도 새 BAR 를 넣을 수 있다. 재배치는 그
 * 사이 동작 중인 다른 장치의 접근을 끊으므로 피하는 편이 훨씬 낫다.
 *
 * 분배 규칙(아래 원문 주석이 밝히는 대로):
 *   - 핫플러그 브리지가 하나라도 있으면 남는 공간을 그 브리지들끼리만
 *     균등하게 나눈다(일반 브리지는 나중에 장치가 늘지 않는다).
 *   - 핫플러그 브리지가 하나도 없으면 일반 브리지들끼리 나눈다. 그 아래
 *     더 깊은 곳에 핫플러그 브리지가 있을 수 있기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 재귀. 락 없음. 지역 배열 두 개를 스택에
 *   잡으므로 재귀 깊이만큼 스택을 쓴다(PCI 트리 깊이 수준이라 문제없다).
 *
 * 호출 체인:
 *   pci_root_bus_distribute_available_resources() /
 *   pci_bridge_distribute_available_resources()
 *     -> [이 함수] -> (재귀) -> adjust_bridge_window(), remove_dev_resources()
 */
static void pci_bus_distribute_available_resources(struct pci_bus *bus,
		    struct list_head *add_list,
		    struct resource available_in[PCI_P2P_BRIDGE_RESOURCE_NUM])
{
	/* [한국어] 호출자의 값을 복사해 이 함수 안에서만 깎아 쓸 작업본.
	 * 원본을 직접 고치지 않는 이유는, 아래에서 자식마다 값을 다르게
	 * 조정해 재귀로 넘겨야 하기 때문이다. */
	struct resource available[PCI_P2P_BRIDGE_RESOURCE_NUM];
	/* [한국어] 이 버스에 달린 브리지 개수를 종류별로 센다. 분배 대상을
	 * 고르고 몫을 나눌 분모가 된다. */
	unsigned int normal_bridges = 0, hotplug_bridges = 0;
	/* [한국어] dev 는 순회 커서, bridge 는 이 버스의 상위 브리지
	 * (= 지금 조정하려는 창들의 소유자). */
	struct pci_dev *dev, *bridge = bus->self;
	/* [한국어] 창 종류별로 브리지 하나에 돌아갈 몫. */
	resource_size_t per_bridge[PCI_P2P_BRIDGE_RESOURCE_NUM];
	/* [한국어] 정렬 계산용 임시 변수. */
	resource_size_t align;
	/* [한국어] 창 종류 순회 인덱스(0=IO, 1=MEM, 2=PREF MEM 에 대응). */
	int i;

	/* [한국어] 1단계: 이 버스의 상위 브리지 창들을 물려받은 공간에 맞춰
	 * 조정한다. 세 창을 차례로 처리한다. */
	for (i = 0; i < PCI_P2P_BRIDGE_RESOURCE_NUM; i++) {
		/* [한국어] 브리지 창 구획의 i 번째 창. PCI_BRIDGE_RESOURCES 가
		 * 그 구획의 시작 인덱스다. */
		struct resource *res =
			pci_resource_n(bridge, PCI_BRIDGE_RESOURCES + i);

		/* [한국어] 물려받은 값을 작업본으로 복사한다. 구조체 대입이라
		 * start/end/flags 가 모두 함께 복사된다. */
		available[i] = available_in[i];

		/*
		 * The alignment of this bridge is yet to be considered,
		 * hence it must be done now before extending its bridge
		 * window.
		 */
		/* [한국어] 이 창이 요구하는 정렬. */
		align = pci_resource_alignment(bridge, res);
		/* [한국어] 아직 주소가 정해지지 않은 창만 조정한다(배정된 창은
		 * 이미 정렬이 지켜진 자리에 있다). 물려받은 공간의 시작점을
		 * 이 창의 정렬 경계로 올림하면, 그만큼 앞부분이 쓸 수 없는
		 * 공간이 되어 실제 여유가 줄어든다. 위 원문 주석이 말하는
		 * "이 브리지의 정렬을 이제 반영해야 한다"가 이 계산이다.
		 * min 으로 end + 1 을 상한에 두어, 정렬 올림이 남은 공간을
		 * 넘어가면 여유를 0 으로 만든다. */
		if (!resource_assigned(res) && align)
			available[i].start = min(ALIGN(available[i].start, align),
						 available[i].end + 1);

		/*
		 * Now that we have adjusted for alignment, update the
		 * bridge window resources to fill as much remaining
		 * resource space as possible.
		 */
		/* [한국어] 정렬 보정이 끝난 여유 크기를 이 창의 목표 크기로
		 * 삼아 창을 조정한다. 늘리는 경우와 줄이는 경우의 처리가
		 * adjust_bridge_window() 안에서 갈린다. */
		adjust_bridge_window(bridge, res, add_list,
				     resource_size(&available[i]));
	}

	/*
	 * Calculate how many hotplug bridges and normal bridges there
	 * are on this bus.  We will distribute the additional available
	 * resources between hotplug bridges.
	 */
	/* [한국어] 2단계: 이 버스에 달린 브리지를 종류별로 센다.
	 * for_each_pci_bridge() 는 버스의 장치 중 브리지만 골라 순회하는
	 * 매크로다. */
	for_each_pci_bridge(dev, bus) {
		/* [한국어] 핫플러그 슬롯을 만드는 브리지 — 여유 공간의 주된
		 * 수혜자다. */
		if (dev->is_hotplug_bridge)
			hotplug_bridges++;
		else
			/* [한국어] 일반 브리지. 핫플러그 브리지가 하나도 없을
			 * 때만 분배 대상이 된다. */
			normal_bridges++;
	}

	/* [한국어] 브리지가 하나도 없으면 아래로 나눠 줄 곳이 없다.
	 * 이 버스의 창 조정(1단계)만 하고 끝낸다. */
	if (!(hotplug_bridges + normal_bridges))
		return;

	/*
	 * Calculate the amount of space we can forward from "bus" to any
	 * downstream buses, i.e., the space left over after assigning the
	 * BARs and windows on "bus".
	 */
	/* [한국어] 3단계: 이 버스의 장치들이 실제로 쓸 몫을 여유에서 뺀다.
	 * 남는 것이 아래로 넘겨줄 수 있는 공간이다. */
	list_for_each_entry(dev, &bus->devices, bus_list) {
		/* [한국어] 가상 함수(VF)는 제외한다. VF 의 자원은 물리
		 * 기능(PF)의 VF BAR 구획에 이미 포함돼 있어, 여기서 또 빼면
		 * 같은 공간을 두 번 세는 셈이 된다. */
		if (!dev->is_virtfn)
			remove_dev_resources(dev, available);
	}

	/*
	 * If there is at least one hotplug bridge on this bus it gets all
	 * the extra resource space that was left after the reductions
	 * above.
	 *
	 * If there are no hotplug bridges the extra resource space is
	 * split between non-hotplug bridges. This is to allow possible
	 * hotplug bridges below them to get the extra space as well.
	 */
	/* [한국어] 4단계: 브리지 하나당 돌아갈 몫을 창 종류별로 계산한다. */
	for (i = 0; i < PCI_P2P_BRIDGE_RESOURCE_NUM; i++) {
		/* [한국어] 남은 여유를 브리지 개수로 나눈다.
		 * div64_ul 을 쓰는 이유: resource_size_t 는 32비트
		 * 아키텍처에서도 64비트일 수 있는데, 그런 곳에서 64비트를
		 * 그냥 / 로 나누면 링크 오류가 난다(컴파일러가 라이브러리
		 * 함수를 부르려 하기 때문). 커널은 명시적 헬퍼를 쓴다.
		 * "hotplug_bridges ?: normal_bridges" 는 GNU 확장으로,
		 * 앞이 0 이 아니면 앞 값을, 0 이면 뒤 값을 쓴다 — 위 원문
		 * 주석의 분배 규칙을 그대로 옮긴 것이다. 위 검사 덕에
		 * 둘 다 0 인 경우는 없으므로 0 나눗셈이 발생하지 않는다. */
		per_bridge[i] = div64_ul(resource_size(&available[i]),
					 hotplug_bridges ?: normal_bridges);
	}

	/* [한국어] 5단계: 각 대상 브리지에 몫을 실어 재귀로 내려간다. */
	for_each_pci_bridge(dev, bus) {
		/* [한국어] 브리지의 창 자원을 가리킬 커서. */
		struct resource *res;
		/* [한국어] 브리지 아래의 버스. */
		struct pci_bus *b;

		/* [한국어] 아래에 버스가 없으면 내려갈 곳이 없다. */
		b = dev->subordinate;
		if (!b)
			continue;
		/* [한국어] 핫플러그 브리지가 하나라도 있으면 그것들만 대상이다.
		 * 일반 브리지는 건너뛴다 — 위 규칙의 실행부다. */
		if (hotplug_bridges && !dev->is_hotplug_bridge)
			continue;

		/* [한국어] 이 브리지에 넘길 여유를 창 종류별로 만든다. */
		for (i = 0; i < PCI_P2P_BRIDGE_RESOURCE_NUM; i++) {
			/* [한국어] 이 브리지의 i 번째 창. */
			res = pci_resource_n(dev, PCI_BRIDGE_RESOURCES + i);

			/*
			 * Make sure the split resource space is properly
			 * aligned for bridge windows (align it down to
			 * avoid going above what is available).
			 */
			/* [한국어] 이 브리지 창의 정렬 요구. */
			align = pci_resource_alignment(dev, res);
			/* [한국어] 몫을 그 정렬로 내림해 여유 크기를 정한다.
			 * 올림이 아니라 내림인 이유는 위 원문 주석대로
			 * "가진 것보다 많이 주지 않기" 위해서다. 정렬이 0 이면
			 * 매크로가 몫을 그대로 통과시킨다. */
			resource_set_size(&available[i],
					  ALIGN_DOWN_IF_NONZERO(per_bridge[i],
								align));

			/*
			 * The per_bridge holds the extra resource space
			 * that can be added for each bridge but there is
			 * the minimal already reserved as well so adjust
			 * x.start down accordingly to cover the whole
			 * space.
			 */
			/* [한국어] 시작점을 이 창이 이미 확보한 크기만큼
			 * 뒤로 당긴다. 위 원문 주석이 설명하듯 per_bridge 는
			 * "추가로 줄 수 있는 몫"이고, 창에는 이미 필수 크기가
			 * 잡혀 있다. 재귀로 내려가는 쪽은 "필수 + 추가"를
			 * 합친 전체 공간을 봐야 하므로, start 를 앞으로
			 * 물려 두 몫을 하나의 범위로 만든다. */
			available[i].start -= resource_size(res);
		}

		/* [한국어] 이 브리지 아래로 재귀. 방금 만든 available 을
		 * 그 서브트리의 물려받은 공간으로 넘긴다. */
		pci_bus_distribute_available_resources(b, add_list, available);

		/* [한국어] 다음 형제 브리지를 위해 available 의 시작점을
		 * 이번 몫의 끝 다음으로 옮긴다. 이렇게 하면 형제들이 서로
		 * 겹치지 않는 구간을 배정받는다. end 는 그대로 두는데,
		 * 다음 반복의 resource_set_size() 가 어차피 다시 정하기
		 * 때문이다 — 여기서는 start 의 전진만이 의미를 갖는다. */
		for (i = 0; i < PCI_P2P_BRIDGE_RESOURCE_NUM; i++)
			available[i].start += available[i].end + 1;
	}
}

/*
 * [한국어]
 * pci_bridge_distribute_available_resources - 핫플러그 브리지 하나를 기점으로 여유를 분배한다
 *
 * @bridge: 기점이 될 브리지. 핫플러그 브리지가 아니면 아무것도 하지 않는다.
 * @add_list: 선택적 요구 목록.
 * @return: 없음.
 *
 * 왜 필요한가: 여유 분배의 시작점을 만드는 함수다. 재귀 본체
 * (pci_bus_distribute_available_resources)는 "물려받은 공간"을 인자로
 * 받는데, 최초의 그 공간을 어디서 가져올지 정해야 한다. 여기서는
 * 핫플러그 브리지 자신의 창 전체를 초기 공간으로 삼는다 — 그 창은
 * 크기 산정 pass 에서 예비 공간까지 포함해 이미 잡혀 있으므로,
 * 그 안을 아래로 어떻게 나눌지가 남은 문제다.
 *
 * 왜 핫플러그 브리지만인가: 분배의 목적이 "나중에 장치를 꽂을 자리를
 * 미리 확보하는 것"이라, 장치가 늘어날 일이 없는 일반 브리지에서
 * 시작할 이유가 없다. 일반 브리지는 상위의
 * pci_root_bus_distribute_available_resources() 가 더 아래로 내려가며
 * 핫플러그 브리지를 찾는다.
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_root_bus_distribute_available_resources() /
 *   pci_assign_unassigned_bridge_resources()
 *     -> [이 함수] -> pci_bus_distribute_available_resources()
 */
static void pci_bridge_distribute_available_resources(struct pci_dev *bridge,
						      struct list_head *add_list)
{
	/* [한국어] res 는 창을 가리킬 커서, available 은 초기 공간을 담을
	 * 지역 배열(재귀 본체에 넘긴다). */
	struct resource *res, available[PCI_P2P_BRIDGE_RESOURCE_NUM];
	/* [한국어] 창 종류 순회 인덱스. */
	unsigned int i;

	/* [한국어] 핫플러그 브리지가 아니면 여기서 분배를 시작할 이유가 없다. */
	if (!bridge->is_hotplug_bridge)
		return;

	/* [한국어] 분배 시작을 로그로 남긴다. 나중에 "왜 이 브리지 아래에
	 * 이만큼 여유가 잡혔나"를 추적하는 표식이 된다. */
	pci_dbg(bridge, "distributing available resources\n");

	/* Take the initial extra resources from the hotplug port */
	/* [한국어] 이 브리지의 세 창을 그대로 초기 공간으로 삼는다. */
	for (i = 0; i < PCI_P2P_BRIDGE_RESOURCE_NUM; i++) {
		/* [한국어] 브리지 창 구획의 i 번째 창. */
		res = pci_resource_n(bridge, PCI_BRIDGE_RESOURCES + i);
		/* [한국어] 구조체 통째 복사. 포인터를 넘기지 않고 값을
		 * 복사하는 이유는, 재귀 본체가 이 값을 깎아 가며 쓰기 때문에
		 * 원본 창 자원이 훼손되면 안 되기 때문이다. */
		available[i] = *res;
	}

	/* [한국어] 브리지 아래의 버스부터 분배를 시작한다. */
	pci_bus_distribute_available_resources(bridge->subordinate,
					       add_list, available);
}

/*
 * [한국어]
 * pci_bridge_resources_not_assigned - 이 브리지의 창을 커널이 아직 정하는 중인가
 *
 * @dev: 판정할 브리지 장치.
 * @return: true 면 "세 창 모두 커널이 배치 중이거나 비어 있다".
 *          false 면 창 중 하나 이상이 이미 확정된 주소를 갖고 있다
 *          (보통 펌웨어가 정해 둔 값).
 *
 * 왜 필요한가: 여유 분배는 창 크기를 바꾸는 작업이다. 펌웨어가 정해 둔
 * 창을 함부로 바꾸면 그 아래에서 이미 동작하는 장치가 깨진다. 그래서
 * "커널이 지금 이 창들을 정하는 중인가"를 먼저 확인해야 하고,
 * 그럴 때만 분배를 시작한다.
 *
 * 판정 방법이 요점이다 — IORESOURCE_STARTALIGN 비트를 본다.
 * 이 비트는 pbus_size_io()/pbus_size_mem() 이 크기를 계산하며 붙이는
 * 표시로, "시작 주소가 아직 미정이고 정렬 값만 들어 있다"를 뜻한다.
 * 따라서 flags 가 있는데(창이 존재하는데) 이 비트가 없다면, 그 창은
 * 커널의 크기 산정을 거치지 않은 = 이미 주소가 박힌 창이다.
 * 위 원문 주석이 그 논리를 밝히고 있다.
 * 실행 컨텍스트: 순수 판정. 락 없음.
 *
 * 호출 체인:
 *   pci_root_bus_distribute_available_resources() -> [이 함수]
 */
static bool pci_bridge_resources_not_assigned(struct pci_dev *dev)
{
	/* [한국어] 창을 가리킬 커서. 읽기만 하므로 const 다. */
	const struct resource *r;

	/*
	 * If the child device's resources are not yet assigned it means we
	 * are configuring them (not the boot firmware), so we should be
	 * able to extend the upstream bridge resources in the same way we
	 * do with the normal hotplug case.
	 */
	/* [한국어] I/O 창 검사. flags 가 0 이면 창이 없는 것이라 통과시킨다
	 * (없는 창은 분배를 막을 이유가 없다). 창이 있는데 STARTALIGN 이
	 * 없으면 이미 주소가 확정된 창이므로 즉시 거짓을 돌려준다. */
	r = &dev->resource[PCI_BRIDGE_IO_WINDOW];
	if (r->flags && !(r->flags & IORESOURCE_STARTALIGN))
		return false;
	/* [한국어] non-prefetchable 메모리 창에 같은 검사. */
	r = &dev->resource[PCI_BRIDGE_MEM_WINDOW];
	if (r->flags && !(r->flags & IORESOURCE_STARTALIGN))
		return false;
	/* [한국어] prefetchable 메모리 창에 같은 검사. */
	r = &dev->resource[PCI_BRIDGE_PREF_MEM_WINDOW];
	if (r->flags && !(r->flags & IORESOURCE_STARTALIGN))
		return false;

	/* [한국어] 세 창 모두 통과 — 커널이 배치 중이므로 창 크기를
	 * 조정해도 안전하다. */
	return true;
}

/*
 * [한국어]
 * pci_root_bus_distribute_available_resources - 트리를 훑으며 분배 시작점을 찾는다
 *
 * @bus: 탐색을 시작할 버스(최초 호출은 루트 버스).
 * @add_list: 선택적 요구 목록.
 * @return: 없음.
 *
 * 왜 필요한가: 여유 분배는 아무 데서나 시작할 수 없다. "커널이 창을
 * 정하는 중인 브리지"에서 시작해야 안전하다. 이 함수가 트리를 내려가며
 * 그런 지점을 찾아 pci_bridge_distribute_available_resources() 를 부른다.
 * 조건에 맞지 않으면 더 깊이 내려가 다시 찾는다 — 상위가 고정돼 있어도
 * 하위 어딘가는 커널이 정하는 중일 수 있기 때문이다.
 *
 * 이 함수가 재귀 자체를 멈추는 지점: 분배를 시작한 가지는 더 내려가지
 * 않는다(pci_bridge_distribute_available_resources 가 그 아래를 통째로
 * 처리하므로 중복이 된다).
 * 실행 컨텍스트: 프로세스 문맥. 재귀.
 *
 * 호출 체인:
 *   pci_assign_unassigned_root_bus_resources() -> [이 함수]
 *     -> pci_bridge_resources_not_assigned(),
 *        pci_bridge_distribute_available_resources(), (재귀)
 */
static void
pci_root_bus_distribute_available_resources(struct pci_bus *bus,
					    struct list_head *add_list)
{
	/* [한국어] dev 는 브리지 순회 커서, bridge 는 이 버스의 상위 브리지.
	 * 루트 버스에서는 bridge 가 NULL 이며, 아래 조건이 그것을 이용한다. */
	struct pci_dev *dev, *bridge = bus->self;

	/* [한국어] 이 버스에 달린 브리지들을 훑는다. */
	for_each_pci_bridge(dev, bus) {
		/* [한국어] 브리지 아래의 버스. */
		struct pci_bus *b;

		/* [한국어] 아래에 버스가 없는 브리지는 내려갈 곳도, 분배할
		 * 곳도 없다. */
		b = dev->subordinate;
		if (!b)
			continue;

		/*
		 * Need to check "bridge" here too because it is NULL
		 * in case of root bus.
		 */
		/* [한국어] 두 조건이 함께 참일 때만 여기서 분배를 시작한다.
		 * (1) bridge != NULL — 위 원문 주석이 밝히듯 루트 버스에서는
		 *     bridge 가 NULL 이다. 루트 버스 바로 아래의 브리지에서는
		 *     분배를 시작하지 않고 한 단계 더 내려간다.
		 * (2) 이 브리지의 창들을 커널이 정하는 중이다 — 그래야 창
		 *     크기를 바꿔도 안전하다.
		 * 조건이 맞으면 이 가지는 통째로 그쪽에 맡기고 더 내려가지
		 * 않는다(else 가 아니라 if 쪽이라 재귀가 없다). */
		if (bridge && pci_bridge_resources_not_assigned(dev))
			pci_bridge_distribute_available_resources(dev, add_list);
		else
			/* [한국어] 여기서 시작할 수 없으면 더 깊이 내려가
			 * 다시 찾는다. 하위 어딘가에 조건을 만족하는
			 * 핫플러그 브리지가 있을 수 있다. */
			pci_root_bus_distribute_available_resources(b, add_list);
	}
}

/*
 * [한국어]
 * pci_prepare_next_assign_round - 실패 후 다음 재시도를 위해 상태를 되돌린다
 *
 * @fail_head: 실패한 자원 목록. 이 함수가 통째로 비우고 해제한다.
 * @tried_times: 지금까지 시도한 횟수(로그 출력용).
 * @rel_type: 창을 놓는 강도(leaf_only / whole_subtree).
 * @return: 없음.
 *
 * 왜 필요한가 — 재시도 전략의 준비 단계다. 세 가지를 한다:
 *   1) 지금이 몇 번째 시도인지 로그에 남긴다. 부팅 로그에서 재시도가
 *      몇 번 일어났는지가 자원 부족을 진단하는 첫 단서다.
 *   2) 실패한 자원이 속한 브리지 창을 놓는다. 창을 비워야 다음 라운드에서
 *      더 큰 창을 잡을 수 있다.
 *   3) 실패한 자원의 크기와 플래그를 원래대로 되돌린다. 실패 시도가
 *      남긴 부풀린 값이 다음 계산을 오염시키지 않게 한다.
 *
 * 순서가 중요하다: 창을 먼저 놓고 그다음에 되돌린다. 창을 놓는 과정에서
 * release_child_resources() 가 자식들을 미배정 상태로 만들기 때문에,
 * 그 뒤에야 pci_dev_res_restore() 가 WARN 없이 동작한다
 * (restore 는 이미 배정된 자원에 대해 경고를 찍고 아무것도 하지 않는다).
 * 실행 컨텍스트: 프로세스 문맥. config 공간 쓰기 발생.
 *
 * 호출 체인:
 *   pci_assign_unassigned_root_bus_resources() /
 *   pci_assign_unassigned_bridge_resources()
 *     -> [이 함수] -> pbus_select_window_for_type(),
 *                     pci_bus_release_bridge_resources(),
 *                     pci_dev_res_restore(), pci_dev_res_free_list()
 */
static void pci_prepare_next_assign_round(struct list_head *fail_head,
					  int tried_times,
					  enum release_type rel_type)
{
	/* [한국어] 실패 목록 순회 커서. */
	struct pci_dev_resource *fail_res;

	/* [한국어] 다음 시도 번호를 알린다(tried_times 는 이미 끝난 횟수라
	 * +1 이 다음 차례다). pci_info 가 아니라 pr_info 인 이유는 이
	 * 메시지가 특정 장치가 아니라 시스템 전체의 상태이기 때문이다. */
	pr_info("PCI: No. %d try to assign unassigned res\n", tried_times + 1);

	/*
	 * Try to release leaf bridge's resources that aren't big
	 * enough to contain child device resources.
	 */
	/* [한국어] 실패한 자원마다, 그것이 들어갔어야 할 브리지 창을 놓는다. */
	list_for_each_entry(fail_res, fail_head, list) {
		/* [한국어] 실패한 자원이 달린 버스. 그 위 브리지의 창이
		 * 대상이 된다. */
		struct pci_bus *bus = fail_res->dev->bus;
		/* [한국어] 놓을 창. */
		struct resource *b_win;

		/* [한국어] 저장해 둔 flags 사본으로 창 종류를 판정한다.
		 * 현재 res->flags 를 쓰지 않는 이유는, 실패 처리 과정에서
		 * reset_resource() 가 이미 0 으로 밀었을 수 있기 때문이다. */
		b_win = pbus_select_window_for_type(bus, fail_res->flags);
		if (!b_win)
			/* [한국어] 담을 창이 없는 자원이면 놓을 것도 없다.
			 * 창 자체가 없는 것이 실패 원인일 수 있다. */
			continue;
		/* [한국어] 강도에 따라 이 창(과 필요하면 아래 서브트리의
		 * 창들)을 놓는다. 여러 실패 자원이 같은 창을 가리켜 이
		 * 함수가 같은 창에 대해 여러 번 불릴 수 있는데,
		 * pci_bridge_release_resources() 가 미배정 창을 조기 반환으로
		 * 걸러 내므로 두 번째부터는 아무 일도 하지 않는다. */
		pci_bus_release_bridge_resources(bus, b_win, rel_type);
	}

	/* Restore size and flags */
	/* [한국어] 이제 창이 놓여 자식들이 미배정 상태가 되었으므로,
	 * 실패한 자원의 원래 크기와 플래그를 되돌릴 수 있다. 이것을 창
	 * 놓기보다 먼저 하면 아직 배정된 상태라 restore 가 WARN 만 찍고
	 * 아무것도 하지 않는다. */
	list_for_each_entry(fail_res, fail_head, list)
		pci_dev_res_restore(fail_res);

	/* [한국어] 실패 목록의 수명은 여기서 끝난다. 호출자는 이 함수 뒤에
	 * fail_head 가 비어 있다고 가정하고 다음 라운드에 재사용한다. */
	pci_dev_res_free_list(fail_head);
}

/*
 * First try will not touch PCI bridge res.
 * Second and later try will clear small leaf bridge res.
 * Will stop till to the max depth if can not find good one.
 */
/*
 * [한국어]
 * pci_assign_unassigned_root_bus_resources - 루트 버스 전체를 배치하는 최상위 진입점
 *
 * @bus: 대상 루트 버스.
 * @return: 없음. 결과는 로그와 각 자원의 상태로 남는다.
 *
 * 왜 필요한가: 이 파일이 제공하는 가장 완전한 진입점이다. 세 pass 를
 * 순서대로 돌리고, 실패하면 강도를 올려 가며 재시도한다:
 *   __pci_bus_size_bridges          (크기 산정, 아래에서 위로)
 *   pci_root_bus_distribute_...     (여유 분배, 위에서 아래로)
 *   __pci_bus_assign_resources      (주소 배치, 위에서 아래로)
 *   실패하면 pci_prepare_next_assign_round 로 되돌리고 처음부터 반복
 *
 * 재시도 전략의 두 축(바로 아래 원문 주석이 요약한다):
 *   (1) add_list 를 언제 쓰는가 — 마지막 시도에서만 쓴다. 앞선 시도들은
 *       add_list 를 NULL 로 넘겨 선택적 요구를 필수처럼 취급하고, 그래서
 *       실패하면 상위 브리지 창까지 놓고 다시 잡을 여지가 생긴다.
 *       마지막 시도에서야 선택/필수를 나눠 "되는 만큼만" 확보한다.
 *   (2) rel_type 을 언제 올리는가 — 1~2차는 leaf_only(리프 브리지 창만
 *       놓는다), 3차부터 whole_subtree(서브트리 전체를 놓는다).
 *
 * 시도 횟수 상한: 재할당이 켜져 있으면 버스 트리 깊이 + 1, 꺼져 있으면 1.
 * 깊이만큼 시도하는 이유는 한 층씩 더 깊이 놓아 가며 공간을 만들기 때문이다.
 *
 * NVMe 관점: 부팅 시 NVMe SSD 의 BAR0 주소가 여기서 정해질 수 있다
 * (펌웨어 값을 claim 하지 않고 재배치 경로를 탄 경우). 실제 호출부는
 * drivers/pci/probe.c:7945 와 drivers/pci/pci-acpi.c:2041 이다.
 * 실행 컨텍스트: 프로세스 문맥. 부팅 초기 또는 호스트 브리지 추가 시점.
 *   락은 잡지 않는다.
 *
 * 호출 체인:
 *   drivers/pci/probe.c:7945 / drivers/pci/pci-acpi.c:2041 -> [이 함수]
 *     -> __pci_bus_size_bridges(), pci_root_bus_distribute_available_resources(),
 *        __pci_bus_assign_resources(), pci_prepare_next_assign_round(),
 *        pci_bus_dump_resources()
 */
void pci_assign_unassigned_root_bus_resources(struct pci_bus *bus)
{
	/* [한국어] 선택적 요구를 담을 실제 리스트. 스택에 잡고, 마지막
	 * 시도에서만 add_list 가 이것을 가리키게 된다. */
	LIST_HEAD(realloc_head);
	/* List of resources that want additional resources */
	/* [한국어] 아래 pass 들에 넘길 포인터. NULL 인 동안은 선택적 요구를
	 * 미루지 않고 필수로 취급한다. */
	struct list_head *add_list = NULL;
	/* [한국어] 지금까지 끝낸 시도 횟수. */
	int tried_times = 0;
	/* [한국어] 창 놓기 강도. 가장 보수적인 값에서 시작한다. */
	enum release_type rel_type = leaf_only;
	/* [한국어] 실패 보고를 받을 목록. 매 라운드에서 채워지고 비워진다. */
	LIST_HEAD(fail_head);
	/* [한국어] 시도 횟수 상한. 기본은 1 = 재시도 없음. */
	int pci_try_num = 1;
	/* [한국어] 이 버스에 적용할 최종 재할당 정책. */
	enum enable_type enable_local;

	/* Don't realloc if asked to do so */
	/* [한국어] 부팅 파라미터와 자동 판정을 합쳐 정책을 확정한다. */
	enable_local = pci_realloc_detect(bus, pci_realloc_enable);
	/* [한국어] 재할당이 켜진 경우에만 여러 번 시도한다. 꺼져 있으면
	 * 한 번 해 보고 실패해도 그대로 둔다 — 펌웨어 배치를 흔들지
	 * 않겠다는 뜻이기 때문이다. */
	if (pci_realloc_enabled(enable_local)) {
		/* [한국어] 버스 트리의 최대 깊이. */
		int max_depth = pci_bus_get_depth(bus);

		/* [한국어] 깊이 + 1 이 상한이다. 시도마다 한 층씩 더 깊이
		 * 창을 놓아 가므로, 깊이를 넘겨 시도하는 것은 의미가 없다. */
		pci_try_num = max_depth + 1;
		dev_info(&bus->dev, "max bus depth: %d pci_try_num: %d\n",
			 max_depth, pci_try_num);
	}

	/* [한국어] 재시도 루프. 성공하거나 상한에 도달하면 break 로 나간다. */
	while (1) {
		/*
		 * Last try will use add_list, otherwise will try good to
		 * have as must have, so can realloc parent bridge resource
		 */
		/* [한국어] 마지막 시도에 들어설 때만 add_list 를 켠다.
		 * 위 원문 주석이 설명하듯, 앞선 시도들은 선택적 요구를
		 * "필수처럼" 취급해 크게 잡아 보고 실패하면 상위 창까지
		 * 놓고 다시 잡는다. 그런 여지가 없는 마지막 시도에서야
		 * 선택/필수를 나눠 되는 만큼만 확보한다. */
		if (tried_times + 1 == pci_try_num)
			add_list = &realloc_head;
		/*
		 * Depth first, calculate sizes and alignments of all
		 * subordinate buses.
		 */
		/* [한국어] pass 1 — 크기 산정. 리프에서 루트로 올라오며 각
		 * 브리지 창에 필요한 크기와 정렬을 계산한다. */
		__pci_bus_size_bridges(bus, add_list);

		/* [한국어] pass 2 — 여유 분배. 남는 공간을 핫플러그 브리지에
		 * 미리 나눠 준다. add_list 가 NULL 인 라운드에서는 창을
		 * 줄이는 쪽 처리가 억제된다(adjust_bridge_window 참조). */
		pci_root_bus_distribute_available_resources(bus, add_list);

		/* Depth last, allocate resources and update the hardware. */
		/* [한국어] pass 3 — 주소 배치. 루트에서 리프로 내려가며 실제
		 * 주소를 잡고 브리지 창을 하드웨어에 기록한다. 실패한 자원은
		 * fail_head 에 쌓인다. */
		__pci_bus_assign_resources(bus, add_list, &fail_head);
		/* [한국어] 배치가 끝났으면 선택적 요구는 모두 소비돼
		 * add_list 가 비어 있어야 한다. 남아 있다면 어딘가에서
		 * 요구를 처리하지 않은 버그다. 부팅을 멈출 일은 아니라
		 * 한 번 경고하고, 누수를 막기 위해 직접 해제한다. */
		if (WARN_ON_ONCE(add_list && !list_empty(add_list)))
			pci_dev_res_free_list(add_list);
		/* [한국어] 이번 시도를 마쳤다. */
		tried_times++;

		/* Any device complain? */
		/* [한국어] 실패 목록이 비었으면 모두 성공이다. 루프 종료. */
		if (list_empty(&fail_head))
			break;

		/* [한국어] 상한에 도달했으면 더 시도하지 않고 포기한다. */
		if (tried_times >= pci_try_num) {
			/* [한국어] 정책 상태에 따라 사용자에게 다른 안내를
			 * 준다. 이것이 enum enable_type 이 "누가 결정했는가"
			 * 까지 구분하는 실질적 이유다. */
			if (enable_local == undefined) {
				/* [한국어] 재할당을 아무도 켜지 않은 상태 —
				 * 켜 보면 해결될 수 있으니 그 방법을 알린다. */
				dev_info(&bus->dev,
					 "Some PCI device resources are unassigned, try booting with pci=realloc\n");
			} else if (enable_local == auto_enabled) {
				/* [한국어] 커널이 스스로 켰는데도 실패한 상태 —
				 * 오히려 그 자동 판단이 문제일 수 있으니
				 * 끄는 방법을 알린다. 사용자가 직접 켠
				 * (user_enabled) 경우에는 아무 안내도 하지
				 * 않는데, 이미 알고 켠 것이기 때문이다. */
				dev_info(&bus->dev,
					 "Automatically enabled pci realloc, if you have problem, try booting with pci=realloc=off\n");
			}
			/* [한국어] 더 쓸 일이 없는 실패 목록을 해제한다.
			 * 실패한 자원들은 이미 reset_resource() 로 무효화돼
			 * 있어, 그 장치의 드라이버는 나중에 자원이 없다는
			 * 것을 발견하고 probe 를 포기하게 된다. */
			pci_dev_res_free_list(&fail_head);
			break;
		}

		/* Third times and later will not check if it is leaf */
		/* [한국어] 3차 시도부터 강도를 올린다. tried_times 는 이번
		 * 라운드가 끝나 이미 증가한 값이므로, tried_times + 1 이
		 * 다음 시도 번호다. 그것이 2 를 넘으면(= 3차 이상이면)
		 * 서브트리 전체를 놓는 과감한 모드로 바꾼다. */
		if (tried_times + 1 > 2)
			rel_type = whole_subtree;

		/* [한국어] 창을 놓고 상태를 되돌려 다음 라운드를 준비한다.
		 * 이 호출이 fail_head 를 비우므로 다음 라운드에서 재사용된다. */
		pci_prepare_next_assign_round(&fail_head, tried_times, rel_type);
	}

	/* [한국어] 최종 배치 결과를 부팅 로그에 남긴다. 성공했든 포기했든
	 * 지금 상태가 그대로 찍히므로, 장치가 안 뜰 때 가장 먼저 볼 정보다. */
	pci_bus_dump_resources(bus);
}

/*
 * [한국어]
 * pci_assign_unassigned_resources - 시스템의 모든 루트 버스를 순회하며 배치한다
 *
 * @return: 없음. 인자도 없다 — 전역 루트 버스 목록을 대상으로 삼는다.
 *
 * 왜 필요한가: 시스템에 PCI 도메인(호스트 브리지)이 여럿일 수 있다.
 * 각각이 독립된 주소 공간과 자원 트리를 가지므로 루트 버스마다 배치를
 * 따로 돌려야 한다. 이 함수가 그 순회를 담당한다.
 *
 * ACPI IOAPIC 등록을 여기서 하는 이유: IOAPIC 은 PCI 장치로 나타나는
 * 인터럽트 컨트롤러이고, 그것을 커널에 등록하려면 먼저 주소가 확정돼
 * 있어야 한다. 배치 직후가 그 시점이다.
 * 참고: 이 트리에서 이 함수를 부르는 곳은 찾지 못했다(주석을 제거한
 *   코드 전체를 검색해도 정의 한 곳뿐이다). arch/ 디렉터리가 포함돼
 *   있지 않아, 아키텍처 초기화 코드가 부르는지는 여기서 확인할 수 없다.
 * 실행 컨텍스트: 프로세스 문맥. 부팅 초기.
 *
 * 호출 체인:
 *   (이 트리에서는 호출부를 확인할 수 없음) -> [이 함수]
 *     -> pci_assign_unassigned_root_bus_resources(), acpi_ioapic_add()
 */
void pci_assign_unassigned_resources(void)
{
	/* [한국어] 루트 버스 순회 커서. */
	struct pci_bus *root_bus;

	/* [한국어] pci_root_buses 는 시스템의 모든 루트 버스를 잇는 전역
	 * 리스트다(정의는 이 파일 밖에 있다). 도메인마다 하나씩 있다. */
	list_for_each_entry(root_bus, &pci_root_buses, node) {
		/* [한국어] 이 루트 버스 아래 전체를 배치한다(재시도 포함). */
		pci_assign_unassigned_root_bus_resources(root_bus);

		/* Make sure the root bridge has a companion ACPI device */
		/* [한국어] 이 루트 브리지에 대응하는 ACPI 장치 핸들이 있는지
		 * 확인한다. ACPI 가 없는 플랫폼(device tree 기반 등)에서는
		 * NULL 이라 아래를 건너뛴다. */
		if (ACPI_HANDLE(root_bus->bridge))
			/* [한국어] 그 아래에 IOAPIC 이 있으면 커널에 등록한다.
			 * 배치가 끝나 주소가 확정된 지금이 등록할 수 있는
			 * 시점이다. */
			acpi_ioapic_add(ACPI_HANDLE(root_bus->bridge));
	}
}

/*
 * [한국어]
 * pci_assign_unassigned_bridge_resources - 브리지 아래 서브트리만 다시 배치한다
 *
 * @bridge: 기점이 될 브리지. 이 브리지 아래 전체가 대상이다.
 * @return: 없음.
 *
 * 왜 필요한가 — 핫플러그의 주 경로다. 슬롯에 장치를 꽂으면 그 아래에
 * 새 BAR 가 생기므로 자원을 배치해야 하는데, 시스템 전체를 다시 배치할
 * 수는 없다(동작 중인 다른 장치가 깨진다). 그래서 이 브리지 아래만
 * 국소적으로 처리한다.
 *
 * 루트 버스 판과의 차이점 셋:
 *   (1) add_list 를 처음부터 항상 쓴다. 이 경로는 핫플러그 여유 공간을
 *       잡는 것이 목적이라 선택적 요구를 살려 둘 필요가 있다.
 *   (2) 여유 분배를 pci_root_bus_distribute_available_resources() 가 아니라
 *       pci_bridge_distribute_available_resources() 로 직접 시작한다 —
 *       기점이 이미 정해져 있으므로 찾을 필요가 없다.
 *   (3) 재시도는 최대 2회로 고정이고, 창 놓기 강도는 처음부터
 *       whole_subtree 다. 이 서브트리는 어차피 다시 구성하는 중이라
 *       보수적일 이유가 없다.
 *
 * 마지막에 브리지를 다시 활성화하는 이유: 창을 놓고 다시 잡는 과정에서
 * 브리지의 자원 상태가 바뀌었으므로, 커널의 활성화 카운트와 하드웨어의
 * Command 레지스터를 다시 맞춰야 한다.
 *
 * NVMe 관점: NVMe SSD 를 핫플러그 슬롯(또는 Thunderbolt 뒤)에 꽂으면
 * drivers/pci/hotplug/pciehp_pci.c:80 이 이 함수를 부르고, 그 결과로
 * 새 SSD 의 BAR0 주소가 정해진다. 그 값이 곧 NVMe 드라이버가
 * pci_resource_start(pdev, 0) 으로 읽어 ioremap 하는 주소다.
 * 실행 컨텍스트: 프로세스 문맥(핫플러그 워크큐). config 공간 접근 다수.
 *
 * 호출 체인:
 *   drivers/pci/probe.c:8427, drivers/pci/hotplug/pciehp_pci.c:80,
 *   drivers/pci/hotplug/shpchp_pci.c:55,
 *   drivers/pci/hotplug/cpci_hotplug_pci.c:276
 *     -> [이 함수] -> __pci_bus_size_bridges(),
 *        pci_bridge_distribute_available_resources(),
 *        __pci_bridge_assign_resources(), pci_reenable_device(), pci_set_master()
 */
void pci_assign_unassigned_bridge_resources(struct pci_dev *bridge)
{
	/* [한국어] 이 브리지 아래의 버스. 크기 산정의 시작점이 된다.
	 * 이름이 parent 인 것은 "그 아래 장치들의 부모 버스"라는 뜻이다. */
	struct pci_bus *parent = bridge->subordinate;
	/* List of resources that want additional resources */
	/* [한국어] 선택적 요구 목록. 루트 버스 판과 달리 처음부터 쓴다. */
	LIST_HEAD(add_list);
	/* [한국어] 시도 횟수. */
	int tried_times = 0;
	/* [한국어] 실패 보고 목록. */
	LIST_HEAD(fail_head);
	/* [한국어] 마지막 재활성화 결과를 받을 자리. */
	int ret;

	/* [한국어] 재시도 루프(최대 2회). */
	while (1) {
		/* [한국어] pass 1 — 이 서브트리의 크기를 산정한다. */
		__pci_bus_size_bridges(parent, &add_list);

		/*
		 * Distribute remaining resources (if any) equally between
		 * hotplug bridges below. This makes it possible to extend
		 * the hierarchy later without running out of resources.
		 */
		/* [한국어] pass 2 — 여유 분배. 위 원문 주석이 밝히듯, 남는
		 * 공간을 아래 핫플러그 브리지들에 균등하게 나눠 두어야
		 * 나중에 계층을 더 확장해도 자원이 모자라지 않는다. */
		pci_bridge_distribute_available_resources(bridge, &add_list);

		/* [한국어] pass 3 — 브리지 자신의 창부터 서브트리 전체까지
		 * 주소를 배치하고 하드웨어에 기록한다. */
		__pci_bridge_assign_resources(bridge, &add_list, &fail_head);
		/* [한국어] 선택적 요구가 모두 소비됐는지 확인한다. 남아 있으면
		 * 처리되지 않은 요구가 있다는 뜻이라 경고하고 직접 해제한다. */
		if (WARN_ON_ONCE(!list_empty(&add_list)))
			pci_dev_res_free_list(&add_list);
		/* [한국어] 이번 시도를 마쳤다. */
		tried_times++;

		/* [한국어] 실패가 없으면 성공. 루프 종료. */
		if (list_empty(&fail_head))
			break;

		/* [한국어] 2회를 다 썼으면 포기한다. 루트 버스 판처럼 깊이에
		 * 비례해 늘리지 않는 이유는, 이 경로가 핫플러그 이벤트
		 * 처리 중이라 시간을 오래 끌 수 없기 때문이다. */
		if (tried_times >= 2) {
			/* Still fail, don't need to try more */
			/* [한국어] 실패 목록을 해제하고 끝낸다. 자리를 못 잡은
			 * 장치는 드라이버가 붙지 못한다. */
			pci_dev_res_free_list(&fail_head);
			break;
		}

		/* [한국어] 되돌리고 다음 라운드를 준비한다. 강도는 처음부터
		 * whole_subtree — 이 서브트리는 어차피 재구성 중이다. */
		pci_prepare_next_assign_round(&fail_head, tried_times,
					      whole_subtree);
	}

	/* [한국어] 창을 놓고 다시 잡는 과정에서 브리지의 자원 상태가
	 * 바뀌었으므로, 커널의 활성화 상태와 하드웨어 Command 레지스터를
	 * 다시 맞춘다. 이것을 빠뜨리면 브리지가 메모리/IO 디코딩이 꺼진
	 * 채 남아 아래 장치가 전혀 보이지 않는다. */
	ret = pci_reenable_device(bridge);
	if (ret)
		/* [한국어] 실패해도 되돌릴 방법이 없다. 오류를 기록하고
		 * 아래 bus master 설정은 그대로 진행한다. */
		pci_err(bridge, "Error reenabling bridge (%d)\n", ret);
	/* [한국어] Bus Master 비트를 켠다. 브리지가 DMA 트랜잭션을 위로
	 * 전달하려면 이 비트가 필요하다 — 아래에 꽂힌 장치(예: NVMe SSD)가
	 * 호스트 메모리로 DMA 를 하려면 경로상의 브리지들이 모두 켜져 있어야
	 * 한다. */
	pci_set_master(bridge);
}
/* [한국어] 핫플러그 드라이버(pciehp, shpchp, cpci)가 모듈로 빌드될 수
 * 있어 공개한다. GPL 전용이다. */
EXPORT_SYMBOL_GPL(pci_assign_unassigned_bridge_resources);

/*
 * Walk to the root bus, find the bridge window relevant for @res and
 * release it when possible. If the bridge window contains assigned
 * resources, it cannot be released.
 */
/*
 * [한국어]
 * pbus_reassign_bridge_resources - 루트까지 창을 놓고 서브트리를 다시 배치한다
 *
 * @bus: 시작 버스(크기를 바꾸려는 장치가 달린 버스).
 * @res: 기준이 되는 자원. 이것을 담을 창을 따라 위로 올라간다.
 * @saved: [입출력] 놓은 창들의 원래 값을 쌓아 둘 목록. 호출자가
 *         롤백에 쓰므로 이 함수는 채우기만 하고 해제하지 않는다.
 * @return: 0 성공. -ENOENT 는 루트 버스에서 불려 놓을 브리지가 없는 경우,
 *          -ENOSPC 는 재배치 후에도 관련 종류의 필수 자원이 실패한 경우,
 *          그 밖에 -ENOMEM 등 하위 오류를 그대로 올린다. 호출자
 *          pci_do_resource_release_and_resize() 는 0 이 아니면 롤백 경로로 간다.
 *
 * 왜 필요한가: Resizable BAR 로 장치의 BAR 크기를 키우면, 그 BAR 가
 * 현재 창 안에 더는 들어가지 않을 수 있다. 그러면 상위 창을 넓혀야
 * 하는데, 창을 넓히려면 먼저 놓아야 한다. 그것도 한 층이 아니라
 * 루트까지 이어진 모든 창을 — 상위 창이 좁으면 하위 창도 넓힐 수 없기
 * 때문이다. 이 함수가 그 "루트까지 거슬러 올라가며 놓기"를 수행하고,
 * 그다음 가장 위의 브리지부터 다시 배치한다.
 *
 * 왜 res->child 를 보는가: 창 안에 아직 배정된 자식 자원이 있으면
 * 그 창은 놓을 수 없다(그 자원을 쓰는 장치가 동작 중일 수 있다).
 * 그런 창은 건너뛰고 경고만 남긴다 — 그래도 위로 계속 올라가는데,
 * 더 위쪽 창은 비어 있을 수 있기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 호출자가 down_read(&pci_bus_sem) 으로
 *   버스 트리 순회를 보호한 상태에서 불린다. config 공간 쓰기 다수.
 *
 * 호출 체인:
 *   pci_do_resource_release_and_resize() -> [이 함수]
 *     -> pbus_select_window(), pci_release_resource(),
 *        __pci_bus_size_bridges(), __pci_bridge_assign_resources(),
 *        pci_required_resource_failed(), pci_setup_bridge()
 */
static int pbus_reassign_bridge_resources(struct pci_bus *bus, struct resource *res,
					  struct list_head *saved)
{
	/* [한국어] 기준 자원의 종류를 기억해 둔다. 아래 루프에서 res 가
	 * 계속 바뀌므로(창을 따라 위로 올라간다) 원래 종류를 미리 저장한다.
	 * 마지막에 "관련 종류의 실패만 골라 보기" 위해 쓰인다. */
	unsigned long type = res->flags;
	/* [한국어] saved 목록 순회 커서(마지막 정리 단계에서 쓴다). */
	struct pci_dev_resource *dev_res;
	/* [한국어] 마지막으로 만난 브리지 = 가장 위쪽 브리지. NULL 초기화가
	 * 중요하다 — 루프가 한 번도 돌지 않으면(루트 버스에서 불리면)
	 * NULL 로 남아 아래에서 -ENOENT 를 돌려주는 근거가 된다. */
	struct pci_dev *bridge = NULL;
	/* [한국어] 재배치 시 선택적 요구를 담을 임시 목록. */
	LIST_HEAD(added);
	/* [한국어] 재배치 시 실패를 받을 임시 목록. */
	LIST_HEAD(failed);
	/* [한국어] resource[] 인덱스. */
	unsigned int i;
	/* [한국어] 반환값 누적. */
	int ret = 0;

	/* [한국어] 루트 버스에 닿을 때까지 위로 거슬러 올라가며 창을 놓는다. */
	while (!pci_is_root_bus(bus)) {
		/* [한국어] 이 버스의 상위 브리지. 루프가 끝나면 이 변수에는
		 * 루트 바로 아래의 브리지가 남는다. */
		bridge = bus->self;
		/* [한국어] 현재 자원을 담고 있는(또는 담을) 창을 구해 res 를
		 * 그 창으로 바꾼다. 다음 반복에서는 이 창이 "자원"이 되어
		 * 더 위의 창을 찾는 기준이 된다 — 이렇게 창의 사슬을
		 * 따라 올라간다. */
		res = pbus_select_window(bus, res);
		if (!res)
			/* [한국어] 담을 창이 없으면 더 올라갈 사슬이 끊긴
			 * 것이다. 지금까지 놓은 것으로 진행한다. */
			break;

		/* [한국어] 그 창의 인덱스. */
		i = pci_resource_num(bridge, res);

		/* Ignore BARs which are still in use */
		/* [한국어] res->child 가 NULL 이면 이 창 안에 배정된 자식이
		 * 없다 = 놓아도 안전하다. */
		if (!res->child) {
			/* [한국어] 놓기 전에 원래 값을 saved 에 저장한다.
			 * 재배치가 실패하면 호출자가 이것으로 되돌린다. */
			ret = pci_dev_res_add_to_list(saved, bridge, res, 0, 0);
			if (ret)
				/* [한국어] 저장에 실패하면 되돌릴 수 없으므로
				 * 창을 놓지 않고 즉시 중단한다. 이미 놓은
				 * 창들은 saved 에 들어 있어 호출자가 복구한다. */
				return ret;

			/* [한국어] 창을 자원 트리에서 뗀다. 여기서는 하드웨어
			 * 레지스터를 바로 갱신하지 않는데, 아래에서 다시
			 * 배치한 뒤 한꺼번에 기록할 것이기 때문이다. */
			pci_release_resource(bridge, i);
		} else {
			/* [한국어] 자식이 남아 있어 놓을 수 없는 창이다. */
			const char *res_name = pci_resource_name(bridge, i);

			/* [한국어] 사실을 경고로 남긴다. 이 창이 안 놓이면
			 * 아래 재배치가 좁은 공간에서 이뤄져 실패할 수
			 * 있으므로, 실패 원인 추적에 중요한 단서다. */
			pci_warn(bridge,
				 "%s %pR: was not released (still contains assigned resources)\n",
				 res_name, res);
		}

		/* [한국어] 한 층 위로. 루트 버스의 parent 는 NULL 이 아니라,
		 * pci_is_root_bus() 조건이 루프를 끝낸다. */
		bus = bus->parent;
	}

	/* [한국어] 브리지를 한 번도 만나지 못했다 = 처음부터 루트 버스였다.
	 * 놓을 창도 다시 배치할 대상도 없으므로 오류로 돌려준다. */
	if (!bridge)
		return -ENOENT;

	/* [한국어] 가장 위쪽 브리지 아래 전체의 크기를 다시 산정한다.
	 * 창들이 놓여 있으므로 이제 새 BAR 크기를 반영한 값이 나온다. */
	__pci_bus_size_bridges(bridge->subordinate, &added);
	/* [한국어] 그 브리지의 창부터 서브트리 전체를 다시 배치한다.
	 * 여기서 브리지 자신의 창은 하드웨어에 기록되지만, 그 아래
	 * 중간 브리지들의 창은 아직 기록되지 않았을 수 있다. */
	__pci_bridge_assign_resources(bridge, &added, &failed);
	/* [한국어] 선택적 요구가 모두 소비됐는지 확인하고, 남았으면
	 * 경고 후 해제해 누수를 막는다. */
	if (WARN_ON_ONCE(!list_empty(&added)))
		pci_dev_res_free_list(&added);

	/* [한국어] 재배치에서 실패한 자원이 있는가. */
	if (!list_empty(&failed)) {
		/* [한국어] 그중 "우리가 관심 있는 종류"의 필수 자원이
		 * 실패했는지만 본다. type 은 함수 첫머리에 저장해 둔 기준
		 * 자원의 종류다. 예컨대 메모리 BAR 를 키우는 중이었다면
		 * I/O 자원의 실패는 이 작업의 성패와 무관하다. */
		if (pci_required_resource_failed(&failed, type))
			ret = -ENOSPC;
		/* [한국어] 실패 목록은 여기서 소비하고 해제한다. */
		pci_dev_res_free_list(&failed);
		if (ret)
			/* [한국어] 관련 종류의 필수 자원이 실패했다 =
			 * 크기 변경이 불가능하다. 호출자가 롤백한다. */
			return ret;

		/* Only resources with unrelated types failed (again) */
		/* [한국어] 무관한 종류만 실패했으므로 이 작업은 성공으로
		 * 본다. 원문 주석의 "(again)" 은 그 실패가 이번에 새로
		 * 생긴 것이 아니라 원래부터 있던 상태라는 뜻이다. */
	}

	/* [한국어] 마무리: 놓았던 창들 중 아직 하드웨어에 기록되지 않은
	 * 것들을 기록한다. 위 __pci_bridge_assign_resources() 는 기점
	 * 브리지와 그 아래만 다루므로, 중간 계층의 창은 여기서 챙긴다. */
	list_for_each_entry(dev_res, saved, list) {
		/* [한국어] 이 저장 항목이 속한 장치(= 창을 가진 브리지). */
		struct pci_dev *dev = dev_res->dev;

		/* Skip the bridge we just assigned resources for */
		/* [한국어] 기점 브리지는 위에서 이미 창이 기록됐다. 다시
		 * 쓰면 불필요한 config 접근이 생긴다. */
		if (bridge == dev)
			continue;

		/* [한국어] 아래에 버스가 없으면 브리지 창을 프로그램할
		 * 대상이 아니다(방어적 검사). */
		if (!dev->subordinate)
			continue;

		/* [한국어] 이 브리지의 세 창을 현재 값대로 하드웨어에
		 * 기록한다. 놓았다가 다시 잡힌 창의 새 주소가 여기서
		 * 반영된다. */
		pci_setup_bridge(dev->subordinate);
	}

	/* [한국어] 성공. saved 목록은 호출자가 해제한다(롤백 재료로
	 * 계속 필요할 수 있다). */
	return 0;
}

/*
 * [한국어]
 * pci_do_resource_release_and_resize - Resizable BAR 크기 변경의 전체 절차
 *
 * @pdev: 대상 장치.
 * @resno: 크기를 바꿀 BAR 의 resource[] 인덱스.
 * @size: 새 크기(Resizable BAR 스펙의 크기 인코딩 값).
 * @exclude_bars: 이 과정에서 놓지 말아야 할 BAR 들의 비트마스크
 *        (BIT(i) 형태). 호출자가 "이 BAR 는 지금 쓰고 있으니 건드리지
 *        말라"고 지정하는 통로다.
 * @return: 0 성공, 음수 오류. 실패 시에는 이 함수 안에서 원래 상태로
 *        되돌린 뒤 오류를 돌려주므로, 호출자는 추가 복구를 하지 않아도 된다.
 *
 * 왜 필요한가: Resizable BAR 는 장치가 지원하는 여러 크기 중 하나를
 * 골라 쓸 수 있게 한 기능이다(예: GPU 가 VRAM 전체를 BAR 로 노출).
 * 그런데 크기를 바꾸면 그 BAR 는 물론이고 같은 창을 쓰는 형제 BAR 들의
 * 배치까지 전부 다시 계산해야 한다. 이 함수가 그 전체 절차를 묶는다:
 *   1) 현재 크기를 기억해 둔다(롤백용).
 *   2) 하드웨어의 BAR 크기를 새 값으로 바꾼다.
 *   3) 같은 창을 쓰는 형제 BAR 들을 저장하고 놓는다.
 *   4) 상위 창들을 루트까지 놓고 서브트리를 다시 배치한다.
 *   5) 실패하면 restore: 라벨에서 전부 되돌린다.
 *
 * 왜 BAR 크기를 먼저 복원해야 하는가(restore 구간의 원문 주석):
 * BAR 의 하위 비트는 크기에 따라 읽기 전용이 된다. 크기가 큰 상태에서는
 * 하위 주소 비트를 쓸 수 없으므로, 크기를 원래대로 되돌리기 전에는
 * 원래 주소를 다시 쓸 수 없을 수 있다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 안에서 down_read(&pci_bus_sem) 을 잡아
 *   버스 트리 순회를 보호한다(이 파일에서 유일하게 락을 직접 잡는 곳이다).
 *   config 공간 쓰기 다수.
 *
 * 호출 체인:
 *   drivers/pci/rebar.c:410 -> [이 함수]
 *     -> pci_rebar_get_current_size(), pci_rebar_set_size(),
 *        pci_release_resource(), pbus_reassign_bridge_resources(),
 *        pci_claim_resource(), pci_update_resource()
 */
int pci_do_resource_release_and_resize(struct pci_dev *pdev, int resno, int size,
				       int exclude_bars)
{
	/* [한국어] 크기를 바꿀 대상 BAR. */
	struct resource *res = pci_resource_n(pdev, resno);
	/* [한국어] 롤백 순회 커서. */
	struct pci_dev_resource *dev_res;
	/* [한국어] 이 장치가 달린 버스. */
	struct pci_bus *bus = pdev->bus;
	/* [한국어] b_win 은 대상 BAR 가 담긴 창, r 은 형제 BAR 순회 커서. */
	struct resource *b_win, *r;
	/* [한국어] 놓은 자원들의 원래 값을 쌓아 둘 목록(롤백 재료). */
	LIST_HEAD(saved);
	/* [한국어] resource[] 인덱스. */
	unsigned int i;
	/* [한국어] old 는 원래 BAR 크기(롤백용), ret 는 반환값. */
	int old, ret;

	/* [한국어] 대상 BAR 가 담긴 창을 확인한다. 아래에서 "같은 창을 쓰는
	 * 형제"를 고를 기준이 된다. */
	b_win = pbus_select_window(bus, res);
	if (!b_win)
		/* [한국어] 담을 창이 없다면 이 BAR 의 배치를 논할 수 없다. */
		return -EINVAL;

	/* [한국어] 현재 BAR 크기를 하드웨어에서 읽어 둔다. 실패 시 이 값으로
	 * 되돌린다. 음수는 오류(장치가 Resizable BAR 를 지원하지 않는 등). */
	old = pci_rebar_get_current_size(pdev, resno);
	if (old < 0)
		return old;

	/* [한국어] 하드웨어의 BAR 크기를 새 값으로 바꾼다. 이 시점부터
	 * 실패하면 반드시 크기를 되돌려야 하지만, 아직 자원은 하나도
	 * 놓지 않았으므로 여기서 실패하면 그냥 돌아가도 문제가 없다
	 * (pci_rebar_set_size 가 실패했다면 크기가 바뀌지 않았다). */
	ret = pci_rebar_set_size(pdev, resno, size);
	if (ret)
		return ret;

	/* [한국어] 이 장치의 BAR 들을 훑으며 "같은 창을 쓰는 형제"를 놓는다.
	 * 크기가 바뀐 BAR 하나 때문에 창 안의 배치가 전부 달라지므로,
	 * 같은 창의 BAR 들을 함께 놓고 다시 잡아야 한다. */
	pci_dev_for_each_resource(pdev, r, i) {
		/* [한국어] 브리지 창 구획에 닿으면 멈춘다. 이 장치가 브리지인
		 * 경우 그 창은 상위 로직(pbus_reassign_bridge_resources)이
		 * 다루므로 여기서 놓으면 안 된다. */
		if (i >= PCI_BRIDGE_RESOURCES)
			break;

		/* [한국어] 호출자가 "건드리지 말라"고 지정한 BAR 는 넘어간다.
		 * BIT(i) 로 i 번째 비트를 만들어 마스크와 AND 한다. */
		if (exclude_bars & BIT(i))
			continue;

		/* [한국어] 다른 창에 담기는 BAR 는 이번 변경의 영향을 받지
		 * 않는다. 예컨대 prefetchable 창의 BAR 는 non-prefetchable
		 * 창의 재배치와 무관하다. */
		if (b_win != pbus_select_window(bus, r))
			continue;

		/* [한국어] 놓기 전에 원래 값을 저장한다. 롤백의 재료다. */
		ret = pci_dev_res_add_to_list(&saved, pdev, r, 0, 0);
		if (ret)
			/* [한국어] 저장 실패 = 되돌릴 수 없게 된다.
			 * 지금까지 놓은 것들을 복구하러 간다. */
			goto restore;
		/* [한국어] 자원 트리에서 뗀다. 이렇게 비워야 새 크기의 BAR 가
		 * 들어갈 자리를 찾을 수 있다. */
		pci_release_resource(pdev, i);
	}

	/* [한국어] 커널 쪽 struct resource 의 크기를 새 값에 맞춘다.
	 * 위 pci_rebar_set_size() 가 하드웨어를, 이 호출이 커널 표현을
	 * 바꾼다 — 둘이 짝을 이뤄야 이후 배치 계산이 올바른 크기를 본다. */
	pci_resize_resource_set_size(pdev, resno, size);

	/* [한국어] 루트 버스에 직접 달린 장치라면 위로 놓을 브리지 창이
	 * 없다. 창 재배치를 건너뛰고 정리로 간다. 이때 ret 는 0 이라
	 * 성공으로 반환된다. 아래 out: 이 up_read 를 부르는데, 이 경로는
	 * down_read 를 하지 않았다는 점에 유의(그럼에도 이렇게 짜여 있다). */
	if (!bus->self)
		goto out;

	/* [한국어] 버스 트리를 위로 거슬러 오르며 순회할 것이므로, 그 사이
	 * 트리가 바뀌지 않도록 읽기 잠금을 잡는다. 읽기 잠금인 이유는
	 * 트리 "구조"를 바꾸는 것이 아니라 훑기만 하기 때문이다
	 * (자원 값 변경은 별개의 문제다). */
	down_read(&pci_bus_sem);
	/* [한국어] 루트까지 창을 놓고 서브트리를 다시 배치한다.
	 * saved 에 놓은 창들의 원래 값이 추가로 쌓인다. */
	ret = pbus_reassign_bridge_resources(bus, res, &saved);
	if (ret)
		/* [한국어] 재배치 실패. 락을 잡은 채로 restore 로 가는데,
		 * restore 구간이 끝나면 out: 으로 흘러 거기서 푼다. */
		goto restore;

/* [한국어] out: 은 성공 경로와 롤백 경로가 함께 모이는 마무리 구간이다. */
out:
	/* [한국어] 읽기 잠금 해제. */
	up_read(&pci_bus_sem);
	/* [한국어] 저장 목록을 해제한다. 성공했으면 되돌릴 일이 없고,
	 * 롤백 경로에서 왔으면 이미 다 썼다. */
	pci_dev_res_free_list(&saved);
	/* [한국어] 성공이면 0, 실패면 그 오류 코드. 롤백을 마쳤더라도
	 * 실패는 실패이므로 오류를 그대로 올린다. */
	return ret;

restore:
	/*
	 * Revert to the old configuration.
	 *
	 * BAR Size must be restored first because it affects the read-only
	 * bits in BAR (the old address might not be restorable otherwise
	 * due to low address bits).
	 */
	/* [한국어] 하드웨어의 BAR 크기를 먼저 원래 값으로 되돌린다.
	 * 순서가 중요한 이유는 바로 위 원문 주석이 설명한다 — 크기가
	 * BAR 의 어느 하위 비트가 읽기 전용인지를 결정하므로, 크기를
	 * 되돌리기 전에는 원래 주소를 다시 쓰지 못할 수 있다. */
	pci_rebar_set_size(pdev, resno, old);

	/* [한국어] 저장해 둔 모든 자원(형제 BAR 와 상위 창들)을 하나씩
	 * 원래 상태로 되돌린다. */
	list_for_each_entry(dev_res, &saved, list) {
		/* [한국어] 이 항목이 가리키는 자원. 바깥의 res 와 이름이
		 * 같지만 이 블록 안의 별개 변수다. */
		struct resource *res = dev_res->res;
		/* [한국어] 그 자원의 소유 장치. */
		struct pci_dev *dev = dev_res->dev;

		/* [한국어] 인덱스 역산. */
		i = pci_resource_num(dev, res);

		/* [한국어] 실패한 재배치가 이 자원에 새 주소를 잡아 놓았을
		 * 수 있다. 그렇다면 그것을 먼저 걷어 내야 원래 주소를
		 * 다시 잡을 수 있다. */
		if (resource_assigned(res)) {
			/* [한국어] 창이라면 그 안의 자식부터 떼어 낸다.
			 * 자식이 남아 있으면 창 자체를 놓을 수 없다. */
			release_child_resources(res);
			/* [한국어] 그리고 이 자원 자신을 자원 트리에서 뗀다. */
			pci_release_resource(dev, i);
		}

		/* [한국어] 저장해 둔 원래 start/end/flags 를 되쓴다. */
		pci_dev_res_restore(dev_res);

		/* [한국어] 원래 주소 그대로 자원 트리에 다시 등록한다.
		 * 실패하면 이 자원은 복구하지 못한 채 남는다 — 그래도
		 * 나머지 자원의 복구는 계속해야 하므로 continue 다. */
		if (pci_claim_resource(dev, i))
			continue;

		/* [한국어] 일반 BAR 구획이면 하드웨어 BAR 에도 원래 주소를
		 * 되쓴다. 브리지 창은 이 방식이 아니라 아래
		 * pci_setup_bridge() 로 기록한다. */
		if (i < PCI_BRIDGE_RESOURCES) {
			/* [한국어] 로그용 이름. */
			const char *res_name = pci_resource_name(dev, i);

			/* [한국어] struct resource 의 값을 실제 BAR 레지스터에
			 * 기록한다. 여기서 비로소 하드웨어가 원래 주소로
			 * 돌아온다. */
			pci_update_resource(dev, i);
			/* [한국어] 복구 사실을 로그로 남긴다. 크기 변경이
			 * 실패해 되돌렸다는 것을 사용자가 알 수 있게 한다. */
			pci_info(dev, "%s %pR: old value restored\n",
				 res_name, res);
		}
		/* [한국어] 이 자원의 소유자가 브리지라면 그 창들을 하드웨어에
		 * 다시 기록한다. 위 pci_update_resource() 는 일반 BAR 용이라
		 * 브리지 창에는 쓰이지 않는다. */
		if (dev->subordinate)
			pci_setup_bridge(dev->subordinate);
	}
	/* [한국어] 복구를 마쳤으니 공통 마무리로 간다 — 거기서 락을 풀고
	 * saved 목록을 해제한 뒤, 0 이 아닌 ret 를 실패로 돌려준다. */
	goto out;
}

/*
 * [한국어]
 * pci_assign_unassigned_bus_resources - 한 버스와 그 아래를 재시도 없이 배치한다
 *
 * @bus: 대상 버스.
 * @return: 없음.
 *
 * 왜 필요한가: 세 진입점 중 가장 단순한 것이다. 재시도 루프가 없고
 * 실패 목록도 받지 않아, "한 번 해 보고 되는 만큼만" 배치한다.
 * 이미 시스템이 동작 중인 상태에서(핫플러그, VMD 도메인 초기화,
 * sysfs 를 통한 재스캔) 국소적으로 자원을 붙일 때 쓴다. 그런 상황에서는
 * 실패했다고 브리지 창을 놓고 다시 하는 것이 오히려 위험하다 — 동작 중인
 * 다른 장치의 접근이 끊긴다.
 *
 * 락 범위에 주목: 크기 산정만 pci_bus_sem 안에서 하고, 배치는 락 밖에서
 * 한다. 크기 산정은 버스 트리를 재귀로 훑으므로 트리가 바뀌면 안 되지만,
 * 배치는 이미 수집한 정보로 진행하기 때문으로 보인다. 다만 그 설계 의도가
 * 명시된 곳은 이 트리에서 찾지 못했다.
 *
 * NVMe 관점: drivers/pci/controller/vmd.c:949 가 이 함수를 부른다.
 * VMD(Volume Management Device)는 인텔 플랫폼에서 NVMe SSD 들을 별도의
 * PCI 도메인으로 묶는 장치로, 그 도메인 안의 SSD BAR 주소가 여기서 정해진다.
 * 실행 컨텍스트: 프로세스 문맥. 안에서 pci_bus_sem 읽기 잠금을 잡았다 푼다.
 *
 * 호출 체인:
 *   drivers/pci/probe.c:8478, drivers/pci/pci-sysfs.c:1605,
 *   drivers/pci/controller/vmd.c:949
 *     -> [이 함수] -> __pci_bus_size_bridges(), __pci_bus_assign_resources()
 */
void pci_assign_unassigned_bus_resources(struct pci_bus *bus)
{
	/* [한국어] 브리지 순회 커서. */
	struct pci_dev *dev;
	/* List of resources that want additional resources */
	/* [한국어] 선택적 요구 목록. 이 경로는 처음부터 이것을 쓴다. */
	LIST_HEAD(add_list);

	/* [한국어] 크기 산정은 버스 트리를 재귀로 훑으므로 그동안 트리가
	 * 바뀌지 않도록 읽기 잠금을 잡는다. */
	down_read(&pci_bus_sem);
	/* [한국어] 이 버스에 달린 브리지들을 훑는다. */
	for_each_pci_bridge(dev, bus)
		/* [한국어] 아래에 버스를 실제로 거느린 브리지만 대상이다.
		 * pci_has_subordinate() 는 dev->subordinate 가 있는지 묻는
		 * 헬퍼로, 아래 역참조를 안전하게 만든다. */
		if (pci_has_subordinate(dev))
			/* [한국어] 그 서브트리의 크기를 산정한다. 이 버스
			 * 자신의 상위 창은 건드리지 않는 점에 유의 — 이미
			 * 동작 중인 상위 배치를 흔들지 않겠다는 뜻이다. */
			__pci_bus_size_bridges(dev->subordinate, &add_list);
	/* [한국어] 크기 산정이 끝났으니 잠금을 푼다. */
	up_read(&pci_bus_sem);
	/* [한국어] 이 버스부터 아래로 주소를 배치한다. fail_head 로 NULL 을
	 * 넘겨 실패를 보고받지 않는다 — 재시도할 계획이 없기 때문이다. */
	__pci_bus_assign_resources(bus, &add_list, NULL);
	/* [한국어] 선택적 요구가 모두 소비됐는지 확인한다. 남았으면 경고
	 * 후 해제해 누수를 막는다. */
	if (WARN_ON_ONCE(!list_empty(&add_list)))
		pci_dev_res_free_list(&add_list);
}
/* [한국어] VMD 와 핫플러그 드라이버가 모듈일 수 있어 공개한다. GPL 전용. */
EXPORT_SYMBOL_GPL(pci_assign_unassigned_bus_resources);
