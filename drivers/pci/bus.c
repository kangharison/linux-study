// SPDX-License-Identifier: GPL-2.0
/*
 * From setup-res.c, by:
 *	Dave Rusling (david.rusling@reo.mts.dec.com)
 *	David Mosberger (davidm@cs.arizona.edu)
 *	David Miller (davem@redhat.com)
 *	Ivan Kokshaysky (ink@jurassic.park.msu.ru)
 */

/*
 * [한국어 설명] 버스 객체의 자원 목록과 장치 등록·순회 (bus.c)
 *
 * === 파일의 역할 ===
 * struct pci_bus 하나가 "어떤 주소 범위를 자기 것으로 갖는가" 를 관리하고,
 * 그 범위 안에서 하위 장치에게 자리를 떼어 주며, 버스 트리를 훑는 도구를
 * 제공한다. 세 덩어리로 나뉜다.
 *
 *   1) 자원 목록 관리 - pci_add_resource(), pci_bus_add_resource(),
 *      pci_bus_remove_resources(). 호스트 브리지가 "이 도메인은 메모리
 *      0x80000000~0xBFFFFFFF 와 I/O 0x0~0xFFFF 를 쓴다" 고 알려 준 것을
 *      루트 버스의 자원 목록에 등록한다. 하위 브리지는 자기 윈도우 레지스터
 *      값이 자원이 된다.
 *   2) 자원 할당 - pci_bus_alloc_resource(). 장치의 BAR 하나가 요구하는
 *      크기와 정렬을 만족하는 빈 구간을 이 버스(또는 조상 버스)의 자원에서
 *      찾아 준다. setup-bus.c 가 BAR 배치를 정할 때 실제로 자리를 얻는 곳이다.
 *   3) 장치 등록과 순회 - pci_bus_add_device()/pci_bus_add_devices() 는
 *      새로 발견한 장치를 sysfs 에 올리고 드라이버 바인딩을 시작시킨다.
 *      pci_walk_bus() 계열은 버스 트리 전체에 콜백을 적용한다(에러 복구나
 *      전원 상태 변경처럼 "이 아래 전부" 를 대상으로 하는 동작에 쓴다).
 *
 * === 전체 아키텍처에서의 위치 ===
 * 열거:  probe.c 의 pci_scan_child_bus 가 장치를 찾아 struct pci_dev 를 만든다
 *   -> setup-bus.c 가 BAR 크기를 모아 배치를 계산
 *      -> [이 파일] pci_bus_alloc_resource() 로 실제 주소 구간을 얻는다
 *   -> [이 파일] pci_bus_add_devices()
 *      -> pci_bus_add_device() -> device_attach()
 *         -> pci-driver.c 의 pci_device_probe() -> nvme_probe()
 *
 * 순회: AER 복구, D3 전환, 리셋 등
 *   -> [이 파일] pci_walk_bus(bus, cb, userdata) -> 서브트리의 모든 장치에 cb
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. pci_walk_bus() 는 pci_bus_sem 을 읽기
 * 잠금으로 잡고 돌므로 콜백 안에서 장치를 제거하면 안 된다. 잠금 없이 도는
 * __pci_walk_bus() 는 호출자가 이미 잠금을 쥔 경우에만 쓴다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: probe.c(열거 후 등록), setup-bus.c(자원 배치), hotplug 드라이버.
 * 아래쪽: 커널 공통 자원 관리(kernel/resource.c 의 allocate_resource),
 *   드라이버 모델(device_attach), pci.c 의 전원 관리 함수.
 * 공유 상태: struct pci_bus 의 resources 목록(struct pci_bus_resource 로
 *   감싼 struct resource 포인터들), pci_bus_sem(버스 트리 전역 rw 세마포어),
 *   그리고 struct pci_dev 의 match_driver / is_added 플래그.
 * 데이터 흐름: 펌웨어/브리지 윈도우 -> 버스 자원 목록 -> BAR 별 구간 할당
 *   -> struct resource 로 pci_dev->resource[] 에 기록 -> 드라이버가
 *   pci_resource_start() 로 읽는다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 하나도 직접 부르지 않는다(전수 확인).
 * 그럼에도 NVMe 가 동작하기 위한 두 가지 전제를 이 파일이 만든다.
 *
 *   1) BAR0 의 주소. NVMe 컨트롤러 레지스터(CAP, VS, CC, CSTS, 그리고
 *      도어벨 배열)는 전부 BAR0 가 가리키는 메모리 창 안에 있다. 그 창이
 *      물리 주소 공간의 어디에 놓일지를 정하는 마지막 단계가
 *      pci_bus_alloc_resource() 다. 결과는 pdev->resource[0] 에 담기고,
 *      NVMe 드라이버가 pci_resource_start(pdev, 0) 로 읽어
 *      ioremap() 하는 값이 바로 이것이다.
 *
 *   2) nvme_probe() 가 불리는 계기. pci_bus_add_device() 가 장치를 드라이버
 *      모델에 올리면서 device_attach() 를 부르고, 그 끝에 pci_device_probe()
 *      를 거쳐 nvme_probe() 가 실행된다. NVMe SSD 를 핫플러그로 꽂았을 때도
 *      pciehp 가 재스캔한 뒤 이 경로를 탄다.
 *
 * (기존 주석은 NVMe 경로로 "pci_enable_device -> pci_request_regions ->
 *  pci_iomap" 을 적어 두었으나 drivers/nvme/ 에 pci_request_regions 와
 *  pci_iomap 호출은 0건이다. 실제로는 pci_enable_device_mem() 뒤에
 *  ioremap(pci_resource_start(pdev, 0), size) 로 직접 매핑한다.)
 *
 * === 주요 함수/구조체 요약 ===
 * pci_add_resource() / pci_add_resource_offset()
 *                            : 호스트 브리지 등록용 자원 목록에 항목을 추가.
 *                              offset 판은 CPU 주소와 PCI 버스 주소가 다른
 *                              플랫폼에서 그 차이를 함께 기록한다.
 * pci_bus_add_resource()     : 이미 만들어진 버스에 자원을 붙인다.
 * pci_bus_for_each_resource(): 이 버스와 조상 버스의 자원을 순회하는 매크로.
 * pci_bus_alloc_resource()   : 크기/정렬/타입 조건에 맞는 빈 구간을 찾아 예약.
 *                              실패하면 상위 버스로 올라가며 다시 시도한다.
 * pci_bus_clip_resource()    : 브리지 윈도우 밖으로 삐져나온 자원을 잘라낸다.
 *                              펌웨어가 잘못 설정해 둔 경우를 수습하는 용도.
 * pci_bus_add_device()       : 장치 하나를 sysfs 에 올리고 드라이버 바인딩 시작.
 * pci_bus_add_devices()      : 버스 아래 모든 장치에 대해 위를 재귀 수행.
 * pci_walk_bus()             : 서브트리의 모든 장치에 콜백 적용(잠금 포함).
 * pci_walk_bus_reverse()     : 역순 순회. 제거 경로에서 잎부터 처리해야 할 때.
 * pci_bus_get() / pci_bus_put() : struct pci_bus 참조 카운트.
 */
/* [한국어] EXPORT_SYMBOL 계열 매크로. 이 파일의 함수 대부분이 모듈에서 쓰이므로 필요하다. */

/* [한국어] min()/max() 와 기본 커널 관용구. */
#include <linux/module.h>
/* [한국어] __free() 정리 속성. 이 파일에서는 순회 매크로의 _scoped 판이 그것에 기댄다. */
#include <linux/kernel.h>
/* [한국어] struct pci_bus, struct pci_dev, pci_bus_for_each_resource() 등 이 파일이 다루는 타입 전부. */
#include <linux/cleanup.h>
/* [한국어] -ENOMEM 같은 오류 코드. */
#include <linux/pci.h>
/* [한국어] struct resource 와 allocate_resource(), IORESOURCE_ 계열 플래그. 이 파일의 주제다. */
#include <linux/errno.h>
/* [한국어] of_device_is_available() — 디바이스 트리 노드가 사용 가능으로 표시됐는지 확인한다. */
#include <linux/ioport.h>
/* [한국어] of_pci_make_dev_node() 를 위한 헤더. */
#include <linux/of.h>
/* [한국어] 플랫폼 장치 타입. 디바이스 트리 노드에서 만들어진 장치를 다루는 데 필요하다. */
#include <linux/of_platform.h>
/* [한국어] pm_runtime_enable(). pci_bus_add_device() 가 등록 마지막에 런타임 절전을 켠다. */
#include <linux/platform_device.h>
/* [한국어] pci_proc_attach_device() 를 위한 procfs 선언. */
#include <linux/pm_runtime.h>
/* [한국어] kzalloc_obj()/kfree(). pci_bus_resource 항목을 할당한다. */
#include <linux/proc_fs.h>
#include <linux/slab.h>
/* [한국어] 이 디렉터리 안에서만 쓰는 선언들 — pci_fixup_device(), pci_dev_is_added(),
 * pci_bridge_d3_update() 등이 여기 있다. */

#include "pci.h"

/*
 * The first PCI_BRIDGE_RESOURCE_NUM PCI bus resources (those that correspond
 * to P2P or CardBus bridge windows) go in a table.  Additional ones (for
 * buses below host bridges or subtractive decode bridges) go in the list.
 * Use pci_bus_for_each_resource() to iterate through all the resources.
 */

/* [한국어] 버스의 '추가' 자원 하나를 목록에 매달기 위한 포장 구조체.
 * 위 상류 주석이 밝히듯 처음 PCI_BRIDGE_RESOURCE_NUM 개는 배열에 들어가고,
 * 그것을 넘는 자원 — 호스트 브리지 아래나 subtractive decode 브리지의 창 —
 * 만 이 구조체에 싸여 연결 목록으로 간다.
 * 자원 구조체 자체를 목록에 넣지 못하는 이유는 그것이 다른 곳(브리지 장치의
 * 자원 배열, 호스트 브리지 드라이버의 정적 배열)에 이미 소속돼 있어
 *  list_head 를 빌려 쓸 수 없기 때문이다. */
struct pci_bus_resource {
	/* [한국어] 연결 목록의 고리.
	 * 설정자: pci_bus_add_resource() 가 list_add_tail() 로 버스의 목록 끝에 건다.
	 * 읽는 자: pci_bus_resource_n() 이 번호로 따라가고, pci_bus_remove_resource()
	 * 와 pci_bus_remove_resources() 가 list_del() 로 뺀다.
	 * 값 범위: bus->resources 를 머리로 하는 원형 목록의 한 마디.
	 * 동기화: 별도 락이 없다. 이 목록을 고치는 것은 버스 설정과 제거 시점뿐이고,
	 * 그때는 pci_bus_sem 을 쥔 상위 경로 안이라는 전제다. */
	struct list_head	list;
	/* [한국어] 이 항목이 가리키는 실제 자원.
	 * 설정자: pci_bus_add_resource() 가 호출자에게 받은 포인터를 그대로 담는다.
	 * 읽는 자: pci_bus_resource_n() 이 돌려주고, 배정·조사 경로가 그것을 쓴다.
	 * 값 범위: 유효한 struct resource 포인터. 소유권은 넘어오지 않으므로
	 * 이 항목이 해제될 때 자원 자체는 해제하지 않는다 — 그것을 소유한 쪽
	 * (브리지 장치나 호스트 브리지 드라이버)이 따로 관리한다.
	 * 동기화: list 필드와 같다. */
	struct resource		*res;
};

/* [한국어]
 * pci_add_resource_offset - 호스트 브리지 창을 오프셋과 함께 목록에 넣는다
 *
 * @resources: 창들을 모아 둘 목록.
 * @res: 추가할 자원. CPU 주소로 적혀 있다.
 * @offset: CPU 주소에서 버스 주소를 얻기 위해 뺄 값.
 *
 * 호스트 브리지 드라이버가 probe 에서 "이 도메인은 어떤 주소 범위를 쓰는가"
 * 를 알릴 때 쓰는 함수다.
 *
 * 오프셋이 이 함수의 존재 이유다. CPU 가 보는 주소와 PCI 버스가 보는 주소가
 * 같지 않은 시스템이 많다 — CPU 의 0x80000000 이 버스의 0x0 인 식이다.
 * 그 차이를 여기서 함께 기록해 두어야, 나중에 BAR 에 쓸 값을 계산할 때
 * pcibios_resource_to_bus() 가 변환할 수 있다.
 *
 * 할당이 실패하면 기록만 남기고 조용히 돌아간다. 반환값이 없어 알릴 방법이
 * 없는데, 결과적으로 그 창이 없는 것처럼 동작하게 된다.
 *
 * 실행 컨텍스트: 호스트 브리지 드라이버 probe. 할당이 있어 프로세스
 * 컨텍스트여야 한다.
 *
 * 에러 경로: 할당 실패는 pr_err 로만 남는다.
 *
 * 호출 체인:
 *   호스트 브리지 드라이버 probe → [이 함수]
 *     → resource_list_create_entry() → resource_list_add_tail()
 */
void pci_add_resource_offset(struct list_head *resources, struct resource *res,
			     resource_size_t offset)
{
	struct resource_entry *entry;

	entry = resource_list_create_entry(res, 0);
	/* [한국어] 항목 할당이 실패하면, */
	if (!entry) {
		/* [한국어] 어느 창을 못 넣었는지 남기고 — %pR 은 자원의 시작·끝을 사람이 읽을 형식으로 찍는 커널 확장 지정자다. */
		pr_err("PCI: can't add host bridge window %pR\n", res);
		/* [한국어] 반환값이 없어 알릴 방법이 없으므로 조용히 물러난다. 결과적으로 그 창이 없는 것처럼 동작한다. */
		return;
	}

	entry->offset = offset;
	/* [한국어] 목록의 **끝** 에 붙인다. 창의 등록 순서가 곧 배정 시 탐색 순서가 되므로,
	 * 드라이버가 알린 순서를 그대로 지킨다. */
	resource_list_add_tail(entry, resources);
}
EXPORT_SYMBOL(pci_add_resource_offset);

/* [한국어]
 * pci_add_resource - 오프셋 없는 창을 목록에 넣는다
 *
 * @resources: 창들을 모아 둘 목록.
 * @res: 추가할 자원.
 *
 * pci_add_resource_offset() 에 오프셋 0 을 넘기는 한 줄 껍데기다.
 *
 * CPU 주소와 버스 주소가 같은 시스템 — x86 이 대표적이다 — 에서는 오프셋이
 * 언제나 0 이라, 그쪽 드라이버들이 이 짧은 이름을 쓴다.
 *
 * 실행 컨텍스트: 호스트 브리지 드라이버 probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 아래 함수와 같다.
 *
 * 호출 체인:
 *   호스트 브리지 드라이버 probe → [이 함수]
 *     → pci_add_resource_offset(offset=0)
 */
void pci_add_resource(struct list_head *resources, struct resource *res)
{
	pci_add_resource_offset(resources, res, 0);
}
EXPORT_SYMBOL(pci_add_resource);

/* [한국어]
 * pci_free_resource_list - 창 목록을 통째로 해제한다
 *
 * @resources: 해제할 목록.
 *
 * pci_add_resource() 계열이 만든 항목들을 모두 놓는다.
 *
 * resource_list_free() 로 넘기는 한 줄이며, 자원 자체가 아니라 목록 항목만
 * 해제한다는 점이 중요하다. 자원 구조체는 대개 드라이버가 소유한다.
 *
 * 실행 컨텍스트: 호스트 브리지 드라이버의 오류 경로와 remove.
 * 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   호스트 브리지 드라이버 → [이 함수] → resource_list_free()
 */
void pci_free_resource_list(struct list_head *resources)
{
	resource_list_free(resources);
}
/* [한국어] 주의: 이 심볼은 EXPORT_SYMBOL(GPL 아님)이다. 같은 파일의
 * pci_bus_resource_n 등이 EXPORT_SYMBOL_GPL 인 것과 대비되며, 비-GPL 모듈도
 * 이 함수를 쓸 수 있다는 뜻이다. 내보내기 종류는 코드이므로 바꾸면 안 된다. */
EXPORT_SYMBOL(pci_free_resource_list);

/* [한국어]
 * pci_bus_add_resource - 버스의 추가 자원 목록에 자원을 붙인다
 *
 * @bus: 대상 버스.
 * @res: 붙일 자원.
 *
 * 파일 앞머리의 상류 주석이 이 함수가 왜 필요한지를 밝힌다 — 버스의 처음
 * PCI_BRIDGE_RESOURCE_NUM 개 자원은 배열에 들어가고, 그것을 넘는 것은
 * 연결 목록으로 간다.
 *
 * 배열과 목록으로 나뉜 이유는 대부분의 버스가 창을 서너 개만 갖기 때문이다.
 * 브리지 창의 개수가 하드웨어로 정해져 있어 그만큼은 배열이 빠르고, 호스트
 * 브리지나 subtractive decode 브리지처럼 더 많은 창을 갖는 예외만 목록으로
 * 넘긴다.
 *
 * 두 저장소를 함께 훑는 것은 pci_bus_for_each_resource() 매크로가 맡아,
 * 호출자는 그 나뉨을 신경 쓰지 않는다.
 *
 * 할당이 실패하면 기록만 남긴다. 반환값이 없어 알릴 방법이 없다.
 *
 * 실행 컨텍스트: 버스 설정. 할당이 있어 프로세스 컨텍스트여야 한다.
 *
 * 에러 경로: 할당 실패는 dev_err 로만 남는다.
 *
 * 호출 체인:
 *   pci_register_host_bridge() 등 → [이 함수] → kzalloc_obj()
 */
void pci_bus_add_resource(struct pci_bus *bus, struct resource *res)
{
	struct pci_bus_resource *bus_res;

	bus_res = kzalloc_obj(struct pci_bus_resource);
	/* [한국어] 할당이 실패하면, */
	if (!bus_res) {
		/* [한국어] 어느 자원을 못 붙였는지 남기고, */
		dev_err(&bus->dev, "can't add %pR resource\n", res);
		/* [한국어] 역시 조용히 물러난다. */
		return;
	}

	bus_res->res = res;
	/* [한국어] 버스의 추가 자원 목록 끝에 붙인다. 배열이 아니라 목록으로 가는 것은
	 * 번호가 PCI_BRIDGE_RESOURCE_NUM 이상인 자리이기 때문이다. */
	list_add_tail(&bus_res->list, &bus->resources);
}

/* [한국어]
 * pci_bus_resource_n - 버스의 n 번째 자원을 얻는다
 *
 * @bus: 대상 버스.
 * @n: 자원 번호.
 * @return: 그 자원, 없으면 NULL.
 *
 * 배열과 목록으로 나뉜 저장소를 하나의 번호 공간처럼 보이게 하는 함수다.
 * pci_bus_for_each_resource() 매크로가 이것을 반복해 부른다.
 *
 * 번호가 배열 범위 안이면 배열에서 꺼내고, 넘으면 그만큼 빼서 목록을
 * 그 횟수만큼 따라간다. 즉 번호가 배열 다음으로 목록을 이어 세는 셈이다.
 *
 * 목록 쪽이 O(n) 이라 순회 전체가 O(n²) 이 된다. 창이 서너 개뿐이라
 * 실제로는 문제가 되지 않는다.
 *
 * 실행 컨텍스트: 어디서든. 잠들지 않는다.
 *
 * 에러 경로: 범위를 넘으면 NULL 이며, 순회 매크로가 그것을 끝 표시로 쓴다.
 *
 * 호출 체인:
 *   pci_bus_for_each_resource() → [이 함수]
 */
struct resource *pci_bus_resource_n(const struct pci_bus *bus, int n)
{
	struct pci_bus_resource *bus_res;

	if (n < PCI_BRIDGE_RESOURCE_NUM)
		/* [한국어] 배열 범위 안이면 그대로 꺼낸다. 브리지 창은 개수가 하드웨어로 정해져 있어
		 * 배열이 맞다. */
		return bus->resource[n];

	n -= PCI_BRIDGE_RESOURCE_NUM;
	/* [한국어] 그 뒤 번호는 목록에서 센다. */
	list_for_each_entry(bus_res, &bus->resources, list) {
		/* [한국어] n 을 하나씩 깎아 0 이 되는 항목이 찾는 것이다. 후위 감소라 비교는 깎기 전 값으로 한다. */
		if (n-- == 0)
			/* [한국어] 찾은 자원을 돌려준다. */
			return bus_res->res;
	/* [한국어] 목록을 다 돌았는데 못 찾으면 아래에서 NULL 이 나가고, 순회 매크로가 그것을 끝으로 읽는다. */
	}
	return NULL;
}
EXPORT_SYMBOL_GPL(pci_bus_resource_n);

/* [한국어]
 * pci_bus_remove_resource - 버스에서 자원 하나를 뗀다
 *
 * @bus: 대상 버스.
 * @res: 뗄 자원.
 *
 * pci_bus_add_resource() 의 짝이지만, 배열에 있는 것도 뗄 수 있어야 하므로
 * 두 저장소를 모두 뒤진다.
 *
 * 두 경우의 처리가 다르다. 배열이면 자리를 NULL 로 비우기만 하고, 목록이면
 * 항목을 빼고 해제한다. 배열 자리는 버스 구조체의 일부라 해제할 것이 없기
 * 때문이다.
 *
 * 찾으면 즉시 돌아간다. 같은 자원이 두 번 등록되는 일은 없다고 보는 것이다.
 *
 * 실행 컨텍스트: 버스 정리. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 못 찾아도 조용히 돌아간다.
 *
 * 호출 체인:
 *   버스 자원 정리 → [이 함수] → list_del() → kfree()
 */
void pci_bus_remove_resource(struct pci_bus *bus, struct resource *res)
{
	struct pci_bus_resource *bus_res, *tmp;
	int i;

	for (i = 0; i < PCI_BRIDGE_RESOURCE_NUM; i++) {
		/* [한국어] 배열 자리가 이 자원을 가리키고 있으면, */
		if (bus->resource[i] == res) {
			/* [한국어] 자리를 비운다. 배열 칸은 버스 구조체의 일부라 해제할 것이 없다. */
			bus->resource[i] = NULL;
			/* [한국어] 같은 자원이 두 번 등록되지는 않으므로 곧바로 돌아간다. */
			return;
		}
	}

	list_for_each_entry_safe(bus_res, tmp, &bus->resources, list) {
		/* [한국어] 목록 항목이 이 자원을 가리키고 있으면, */
		if (bus_res->res == res) {
			/* [한국어] 목록에서 뺀다. */
			list_del(&bus_res->list);
			kfree(bus_res);
			return;
		}
	}
}

/* [한국어]
 * pci_bus_remove_resources - 버스의 자원을 전부 뗀다
 *
 * @bus: 대상 버스.
 *
 * pci_bus_remove_resource() 를 하나씩 부르는 대신 두 저장소를 각각 비운다.
 * 배열은 통째로 NULL 로 채우고, 목록은 항목을 모두 빼며 해제한다.
 *
 * _safe 순회를 쓰는 것이 필수다. 순회 중에 항목을 해제하므로, 다음 포인터를
 * 미리 잡아 두지 않으면 해제된 메모리에서 그것을 읽게 된다.
 *
 * 버스가 사라질 때 불린다.
 *
 * 실행 컨텍스트: 버스 제거. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_remove_bus() / 호스트 브리지 정리 → [이 함수]
 *     → list_del() → kfree()
 */
void pci_bus_remove_resources(struct pci_bus *bus)
{
	int i;
	struct pci_bus_resource *bus_res, *tmp;

	for (i = 0; i < PCI_BRIDGE_RESOURCE_NUM; i++)
		/* [한국어] 배열은 자리마다 NULL 로 비운다. */
		bus->resource[i] = NULL;

	list_for_each_entry_safe(bus_res, tmp, &bus->resources, list) {
		/* [한국어] 목록 항목은 빼고 해제한다. _safe 순회라 해제 전에 다음 포인터를 잡아 둔 상태다. */
		list_del(&bus_res->list);
		kfree(bus_res);
	}
}

/* [한국어]
 * devm_request_pci_bus_resources - 창들을 시스템 자원 트리에 예약한다
 *
 * @dev: 수명을 맡길 장치.
 * @resources: 예약할 창 목록.
 * @return: 0 = 성공, 또는 음수 오류.
 *
 * 창 목록에 적어 두는 것만으로는 그 주소 범위가 우리 것이 되지 않는다.
 * 시스템 전체의 자원 트리(/proc/iomem, /proc/ioports 에 보이는 것)에
 * 등록해야 다른 드라이버가 같은 범위를 가져가지 못한다.
 *
 * 부모를 종류에 따라 고르는 것이 이 함수의 뼈대다 — I/O 는 ioport_resource,
 * 메모리는 iomem_resource 아래로 들어간다. 그 둘이 아닌 종류(버스 번호 등)는
 * 건너뛰는데, 시스템 자원 트리에 자리가 없기 때문이다.
 *
 * devm 판이라 드라이버가 떨어질 때 자동으로 풀린다. 예약을 남긴 채 드라이버가
 * 사라지면 그 주소 범위를 영영 아무도 못 쓴다.
 *
 * 실행 컨텍스트: 호스트 브리지 드라이버 probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 하나라도 실패하면 즉시 그 오류로 중단한다. 앞서 성공한 예약은
 * devres 가 나중에 자동으로 푼다.
 *
 * 호출 체인:
 *   호스트 브리지 드라이버 probe → [이 함수] → devm_request_resource()
 */
int devm_request_pci_bus_resources(struct device *dev,
				   struct list_head *resources)
{
	struct resource_entry *win;
	struct resource *parent, *res;
	/* [한국어] 예약 결과. */
	int err;

	resource_list_for_each_entry(win, resources) {
		/* [한국어] 이 항목이 가리키는 자원. */
		res = win->res;
		/* [한국어] 자원의 종류에 따라 등록할 부모가 갈린다. */
		switch (resource_type(res)) {
		/* [한국어] I/O 포트 공간이면, */
		case IORESOURCE_IO:
			/* [한국어] 시스템의 I/O 포트 자원 트리(/proc/ioports)가 부모다. */
			parent = &ioport_resource;
			break;
		case IORESOURCE_MEM:
			/* [한국어] 메모리 공간이면 iomem 트리(/proc/iomem)가 부모다. */
			parent = &iomem_resource;
			break;
		default:
			continue;
		}

		err = devm_request_resource(dev, parent, res);
		/* [한국어] 예약이 실패하면 — 다른 드라이버가 겹치는 범위를 이미 가져갔다는 뜻이다. */
		if (err)
			/* [한국어] 즉시 그 오류로 중단한다. 앞서 성공한 예약은 devres 가 나중에 자동으로 푼다. */
			return err;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(devm_request_pci_bus_resources);

static struct pci_bus_region pci_32_bit = {0, 0xffffffffULL};
/* [한국어] 아래 두 구간은 4GB 위 주소를 다루므로, 그것을 표현할 수 있는 아키텍처에서만 정의한다. */
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
static struct pci_bus_region pci_64_bit = {0,
				/* [한국어] 64비트 전체 구간. 아래쪽 4GB 도 포함하므로 pci_high 가 실패했을 때의 대안이 된다. */
				(pci_bus_addr_t) 0xffffffffffffffffULL};
static struct pci_bus_region pci_high = {(pci_bus_addr_t) 0x100000000ULL,
				/* [한국어] 4GB 위쪽만. 64비트 BAR 을 여기로 몰아 아래쪽 4GB 를 32비트 전용 장치에 남긴다. */
				(pci_bus_addr_t) 0xffffffffffffffffULL};
#endif

/*
 * @res contains CPU addresses.  Clip it so the corresponding bus addresses
 * on @bus are entirely within @region.  This is used to control the bus
 * addresses of resources we allocate, e.g., we may need a resource that
 * can be mapped by a 32-bit BAR.
 */

/* [한국어]
 * pci_clip_resource_to_region - 자원의 버스 주소가 지정 구간 안에 들도록 자른다
 *
 * @bus: 기준 버스. 주소 변환에 쓴다.
 * @res: 자를 자원. CPU 주소로 적혀 있다.
 * @region: 버스 주소 기준의 허용 구간.
 *
 * 위 영어 주석이 목적을 밝힌다 — 32비트 BAR 로 매핑할 수 있는 자원을 얻는
 * 것처럼, 배정 결과의 **버스 주소** 에 제약을 걸어야 할 때가 있다.
 *
 * 세 번의 주소 변환이 이 함수의 구조다. 자원은 CPU 주소로 적혀 있고 제약은
 * 버스 주소로 주어지므로, 버스 주소로 바꿔 자른 뒤 다시 CPU 주소로 되돌린다.
 *
 * 교집합이 비면 시작보다 끝을 하나 작게 만든다. 그것이 커널의 "빈 자원"
 * 표현이라, 호출자가 따로 검사하지 않아도 그 창을 건너뛰게 된다.
 *
 * 실행 컨텍스트: 자원 배정. 잠들지 않는다.
 *
 * 에러 경로: 없다. 교집합 없음이 빈 자원으로 표현된다.
 *
 * 호출 체인:
 *   pci_bus_alloc_from_region() → [이 함수]
 *     → pcibios_resource_to_bus() → pcibios_bus_to_resource()
 */
static void pci_clip_resource_to_region(struct pci_bus *bus,
					struct resource *res,
					struct pci_bus_region *region)
{
	struct pci_bus_region r;

	pcibios_resource_to_bus(bus, &r, res);
	/* [한국어] 버스 주소로 바꾼 값이 허용 구간보다 앞서면, */
	if (r.start < region->start)
		/* [한국어] 시작을 구간 시작으로 끌어올린다. */
		r.start = region->start;
	/* [한국어] 끝이 허용 구간을 넘으면, */
	if (r.end > region->end)
		/* [한국어] 끝을 구간 끝으로 끌어내린다. */
		r.end = region->end;

	if (r.end < r.start)
		/* [한국어] 교집합이 비었다. 시작보다 끝을 하나 작게 만드는 것이 커널의 '빈 자원' 표현이며,
		 * 호출자가 따로 검사하지 않아도 그 창을 건너뛰게 된다. */
		res->end = res->start - 1;
	else
		pcibios_bus_to_resource(bus, res, &r);
}

/* [한국어]
 * pci_bus_alloc_from_region - 지정한 버스 주소 구간 안에서 빈 자리를 찾아 배정한다
 *
 * @bus: 자리를 찾을 버스.
 * @res: 배정 결과를 담을 자원. 종류 플래그가 미리 채워져 있어야 한다.
 * @size: 필요한 크기.
 * @align: 필요한 정렬.
 * @min: 최소 주소.
 * @type_mask: 창과 맞춰 볼 종류 비트.
 * @alignf: 아키텍처별 정렬 보정 콜백.
 * @alignf_data: 그 콜백에 넘길 자료.
 * @region: 버스 주소 기준의 허용 구간.
 * @return: 0 = 성공, -ENOMEM = 맞는 자리가 없음.
 *
 * 실제 배정이 일어나는 곳이다. 버스의 창을 하나씩 보며 조건에 맞는 첫 창에서
 * 자리를 떼어 낸다.
 *
 * 거르는 조건이 넷이다.
 * 1. 쓸 수 없는 창(UNSET / DISABLED)은 건너뛴다.
 * 2. 종류가 다르면 건너뛴다. 메모리 자원을 I/O 창에서 뗄 수는 없다.
 * 3. prefetchable 창에서 non-prefetchable 자원을 떼지 않는다(옆의 상류 주석).
 *    그 방향의 배정은 안전하지 않은데, prefetchable 구간은 브리지가 미리 읽고
 *    합칠 수 있어 부작용이 있는 레지스터를 그 안에 두면 안 되기 때문이다.
 *    반대 방향은 허용된다.
 * 4. 남은 공간이 필요한 크기보다 작으면 건너뛴다.
 *
 * min 을 다루는 방식이 미묘하다. 보통은 인자로 받은 하한(PCIBIOS_MIN_IO 등)을
 * 쓰는데, 옆의 상류 주석대로 그것은 잘못 서술된 마더보드 자원을 피하려는
 * 보수적인 값이다. 그런데 창의 시작이 이미 정해져 있다면 그 값이 하한을
 * 대신한다 — 이미 설정된 브리지 창이라면 그 안이 안전하다고 보는 것이다.
 *
 * 실행 컨텍스트: 자원 배정. 프로세스 컨텍스트.
 *
 * 에러 경로: 모든 창을 봐도 자리가 없으면 -ENOMEM. 호출자가 다른 구간으로
 * 다시 시도할 수 있다.
 *
 * 호출 체인:
 *   pci_bus_alloc_resource() → [이 함수]
 *     → pci_clip_resource_to_region() → allocate_resource()
 */
static int pci_bus_alloc_from_region(struct pci_bus *bus, struct resource *res,
		resource_size_t size, resource_size_t align,
		resource_size_t min, unsigned long type_mask,
		resource_alignf alignf,
		void *alignf_data,
		struct pci_bus_region *region)
{
	struct resource *r, avail;
	resource_size_t max;
	/* [한국어] allocate_resource() 의 결과. */
	int ret;

	type_mask |= IORESOURCE_TYPE_BITS;

	pci_bus_for_each_resource(bus, r) {
		/* [한국어] 이 창에 쓸 하한. 인자로 받은 값에서 출발하되 아래에서 창 시작으로 덮일 수 있어
		 * 루프 안에서 매번 새로 잡는다. */
		resource_size_t min_used = min;

		if (!r)
			/* [한국어] 빈 배열 칸이면 건너뛴다. */
			continue;

		if (r->flags & (IORESOURCE_UNSET|IORESOURCE_DISABLED))
			/* [한국어] 아직 배정되지 않았거나(UNSET) 꺼져 있는(DISABLED) 창은 쓸 수 없다. */
			continue;

		/* type_mask must match */
		if ((res->flags ^ r->flags) & type_mask)
			continue;

		/* We cannot allocate a non-prefetching resource
		   from a pre-fetching area */
		if ((r->flags & IORESOURCE_PREFETCH) &&
		    !(res->flags & IORESOURCE_PREFETCH))
			continue;

		avail = *r;
		/* [한국어] 이 창을 허용 구간 안으로 자른다. 원본이 아니라 사본(avail)을 자르는 것이 중요한데,
		 * 버스의 실제 자원을 좁히면 다음 배정이 그만큼 손해를 본다. */
		pci_clip_resource_to_region(bus, &avail, region);

		/*
		 * "min" is typically PCIBIOS_MIN_IO or PCIBIOS_MIN_MEM to
		 * protect badly documented motherboard resources, but if
		 * this is an already-configured bridge window, its start
		 * overrides "min".
		 */
		if (avail.start)
			/* [한국어] 이미 설정된 창이라면 그 시작이 하한을 대신한다(위 상류 주석). 인자로 받은 하한은
			 * 잘못 서술된 마더보드 자원을 피하려는 보수적인 값일 뿐이라, 창 안이 안전하다고
			 * 알고 있는 경우에는 그것에 얽매일 이유가 없다. */
			min_used = avail.start;

		/* [한국어] 상한은 잘라 낸 창의 끝이다. 이 두 값이 아래 allocate_resource() 의 탐색 범위가 된다. */
		max = avail.end;

		/* Don't bother if available space isn't large enough */
		if (size > max - min_used + 1)
			continue;

		/* Ok, try it out.. */
		ret = allocate_resource(r, res, size, min_used, max,
					align, alignf, alignf_data);
		if (ret == 0)
			/* [한국어] 배정에 성공했으면 곧바로 돌아간다. 첫 번째로 맞는 창을 쓰는 first-fit 이다. */
			return 0;
	}
	return -ENOMEM;
}

/**
 * pci_bus_alloc_resource - allocate a resource from a parent bus
 * @bus: PCI bus
 * @res: resource to allocate
 * @size: size of resource to allocate
 * @align: alignment of resource to allocate
 * @min: minimum /proc/iomem address to allocate
 * @type_mask: IORESOURCE_* type flags
 * @alignf: resource alignment function
 * @alignf_data: data argument for resource alignment function
 *
 * Given the PCI bus a device resides on, the size, minimum address,
 * alignment and type, try to find an acceptable resource allocation
 * for a specific device resource.
 */

/* [한국어]
 * pci_bus_alloc_resource - 이 버스에서 장치 자원 하나의 자리를 얻는다
 *
 * @bus: 자리를 찾을 버스.
 * @res: 배정 결과를 담을 자원.
 * @size: 필요한 크기.
 * @align: 필요한 정렬.
 * @min: 최소 주소.
 * @type_mask: 창과 맞춰 볼 종류 비트.
 * @alignf: 아키텍처별 정렬 보정 콜백.
 * @alignf_data: 그 콜백에 넘길 자료.
 * @return: 0 = 성공, -ENOMEM.
 *
 * setup-bus.c 가 BAR 배치를 정할 때 실제로 자리를 얻는 진입점이다.
 *
 * 이 함수가 하는 일은 **어느 주소 구간에서 찾을지** 를 정하는 것뿐이고,
 * 찾기 자체는 pci_bus_alloc_from_region() 이 한다.
 *
 * 64비트 BAR 의 처리가 이 함수의 요점이다. 4GB 위쪽(pci_high)을 먼저 시도하고,
 * 실패하면 64비트 전체(pci_64_bit)로 넓힌다. 위쪽을 먼저 보는 이유는 아래쪽의
 * 4GB 가 귀하기 때문이다 — 32비트 BAR 만 쓸 수 있는 장치가 그 공간을 필요로
 * 하므로, 64비트를 쓸 수 있는 장치는 위로 비켜 주는 것이 좋다.
 *
 * 32비트 BAR 은 선택의 여지가 없어 pci_32_bit 하나로 간다.
 *
 * 64비트 경로 전체가 CONFIG_ARCH_DMA_ADDR_T_64BIT 로 감싸여 있다. 그 설정이
 * 꺼진 아키텍처에서는 4GB 위 주소를 아예 표현할 수 없다.
 *
 * 실행 컨텍스트: 자원 배정. 프로세스 컨텍스트.
 *
 * 에러 경로: 어느 구간에서도 못 찾으면 -ENOMEM 이 올라간다.
 *
 * 호출 체인:
 *   setup-res.c 의 _pci_assign_resource() → [이 함수]
 *     → pci_bus_alloc_from_region()
 */
int pci_bus_alloc_resource(struct pci_bus *bus, struct resource *res,
		resource_size_t size, resource_size_t align,
		resource_size_t min, unsigned long type_mask,
		resource_alignf alignf,
		void *alignf_data)
{
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
	int rc;

	if (res->flags & IORESOURCE_MEM_64) {
		/* [한국어] 먼저 4GB 위쪽에서 찾는다. */
		rc = pci_bus_alloc_from_region(bus, res, size, align, min,
				       /* [한국어] 인자를 그대로 넘기고 구간만 pci_high 로 지정한다. */
				       type_mask, alignf, alignf_data,
				       &pci_high);
		if (rc == 0)
			/* [한국어] 위쪽에서 자리를 얻었으면 끝이다. */
			return 0;

		return pci_bus_alloc_from_region(bus, res, size, align, min,
					 /* [한국어] 위쪽이 안 되면 64비트 전체로 넓혀 다시 시도한다 — 결과적으로 아래쪽 4GB 도 후보가 된다. */
					 type_mask, alignf, alignf_data,
					 &pci_64_bit);
	}
#endif

	return pci_bus_alloc_from_region(bus, res, size, align, min,
				 /* [한국어] 32비트 BAR 은 4GB 아래만 쓸 수 있어 선택의 여지가 없다. */
				 type_mask, alignf, alignf_data,
				 &pci_32_bit);
}
EXPORT_SYMBOL(pci_bus_alloc_resource);

/*
 * The @idx resource of @dev should be a PCI-PCI bridge window.  If this
 * resource fits inside a window of an upstream bridge, do nothing.  If it
 * overlaps an upstream window but extends outside it, clip the resource so
 * it fits completely inside.
 */

/* [한국어]
 * pci_bus_clip_resource - 브리지 창이 상위 창을 벗어나면 안쪽으로 잘라 넣는다
 *
 * @dev: 브리지 장치.
 * @idx: 그 창의 자원 번호.
 * @return: true = 잘랐다, false = 자를 필요가 없거나 겹치는 창이 없다.
 *
 * 위 영어 주석이 세 경우를 나눈다 — 상위 창 안에 완전히 들어가면 그대로 두고,
 * 겹치되 삐져나오면 안쪽으로 자르고, 아예 겹치지 않으면 손대지 않는다.
 *
 * 펌웨어가 설정해 둔 브리지 창이 상위 창을 벗어나 있는 일이 실제로 있어,
 * 그대로 두면 그 범위로 향한 트랜잭션이 상위 브리지에서 막힌다. 커널이
 * 그것을 발견하고 쓸 수 있는 부분만 남기는 것이다.
 *
 * 교집합을 계산해 그것으로 대체하는 것이 자르기의 전부다.
 *
 * UNSET 플래그를 지우는 것이 눈에 띈다. 자른 결과가 유효한 범위이므로 더는
 * "배정되지 않음" 이 아니게 된다. 원본 사본에서도 같은 비트를 지우는데,
 * 로그에 찍을 때 두 값이 같은 형식으로 보이게 하려는 것이다.
 *
 * 첫 번째로 겹치는 창을 찾으면 거기서 결론을 내고 돌아간다.
 *
 * 실행 컨텍스트: 자원 조사. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 겹치는 창이 없으면 false 로 답하고, 호출자가 그때
 * 자원을 다시 배정한다.
 *
 * 호출 체인:
 *   setup-bus.c 의 브리지 창 조사 → [이 함수] → pci_info()
 */
bool pci_bus_clip_resource(struct pci_dev *dev, int idx)
{
	struct pci_bus *bus = dev->bus;
	struct resource *res = &dev->resource[idx];
	/* [한국어] 자르기 전 값의 사본. 아래 로그에서 '무엇이 무엇으로 바뀌었는지' 를 찍기 위해 남긴다. */
	struct resource orig_res = *res;
	/* [한국어] 상위 버스의 창을 훑을 포인터. */
	struct resource *r;

	pci_bus_for_each_resource(bus, r) {
		/* [한국어] 이 창과의 교집합 시작·끝. */
		resource_size_t start, end;

		if (!r)
			/* [한국어] 빈 배열 칸이면 건너뛴다. */
			continue;

		if (resource_type(res) != resource_type(r))
			/* [한국어] 종류가 다른 창은 비교 대상이 아니다. */
			continue;

		start = max(r->start, res->start);
		/* [한국어] 교집합의 끝은 두 끝 중 작은 쪽이다. 시작은 위에서 큰 쪽으로 잡았다. */
		end = min(r->end, res->end);

		if (start > end)
			/* [한국어] 시작이 끝보다 크면 겹치지 않는다는 뜻이라, 다음 창을 본다. */
			continue;	/* no overlap */

		if (res->start == start && res->end == end)
			/* [한국어] 이미 상위 창 안에 완전히 들어 있으면 바꿀 것이 없다. */
			return false;	/* no change */

		res->start = start;
		/* [한국어] 교집합의 끝으로 줄인다. 시작은 위 줄에서 이미 옮겼다. */
		res->end = end;
		/* [한국어] 자른 결과가 유효한 범위이므로 '배정되지 않음' 표시를 지운다. */
		res->flags &= ~IORESOURCE_UNSET;
		/* [한국어] 로그에 찍을 사본에서도 같은 비트를 지운다. 두 값이 같은 형식으로 보이게 하려는 것이다. */
		orig_res.flags &= ~IORESOURCE_UNSET;
		/* [한국어] 무엇이 어떻게 잘렸는지 남긴다. 펌웨어 설정이 잘못됐다는 신호라 기록할 가치가 있다. */
		pci_info(dev, "%pR clipped to %pR\n", &orig_res, res);

		return true;
	}

	return false;
}

/* [한국어]
 * pcibios_resource_survey_bus - 아키텍처가 버스 자원을 조사할 자리(기본은 빈 함수)
 *
 * @bus: 대상 버스.
 *
 * __weak 로 정의된 훅이다. 아키텍처가 같은 이름의 함수를 정의하면 링커가
 * 그쪽을 쓰고, 정의하지 않으면 이 빈 함수가 남는다.
 *
 * 이런 훅이 필요한 이유는 펌웨어가 자원을 어떻게 남겨 두는지가 아키텍처마다
 * 다르기 때문이다. 어떤 아키텍처는 열거 직후에 자기 방식으로 자원을 살펴
 * 보정해야 한다.
 *
 * 이 트리에 arch/ 가 없어 어느 아키텍처가 이것을 재정의하는지는 확인 못 함.
 *
 * 실행 컨텍스트: 버스 열거 직후. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   probe.c 의 열거 마무리 → [이 함수](아키텍처가 재정의했다면 그쪽)
 */
void __weak pcibios_resource_survey_bus(struct pci_bus *bus) { }

/* [한국어]
 * pcibios_bus_add_device - 아키텍처가 장치 등록에 끼어들 자리(기본은 빈 함수)
 *
 * @pdev: 등록 중인 장치.
 *
 * pcibios_resource_survey_bus() 와 같은 __weak 훅이며, 장치 하나가 sysfs 에
 * 올라가기 직전에 불린다.
 *
 * 아키텍처별 초기화 — 예를 들어 IOMMU 그룹 배정이나 펌웨어 정보 연결 —
 * 를 이 자리에서 한다.
 *
 * pci_bus_add_device() 가 이것을 가장 먼저 부르는 것이 중요하다. 그 뒤의
 * 단계들(sysfs 생성, 드라이버 바인딩)이 여기서 세운 것에 기댈 수 있다.
 *
 * 이 트리에 arch/ 가 없어 어느 아키텍처가 이것을 재정의하는지는 확인 못 함.
 *
 * 실행 컨텍스트: 장치 등록. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 반환값이 없어 실패를 알릴 수 없다.
 *
 * 호출 체인:
 *   pci_bus_add_device() → [이 함수](아키텍처가 재정의했다면 그쪽)
 */
void __weak pcibios_bus_add_device(struct pci_dev *pdev) { }

/**
 * pci_bus_add_device - start driver for a single device
 * @dev: device to add
 *
 * This adds add sysfs entries and start device drivers
 */

/* [한국어]
 * pci_bus_add_device - 장치를 sysfs 에 올리고 드라이버를 붙인다
 *
 * @dev: 등록할 장치.
 *
 * 열거로 만들어진 pci_dev 가 실제로 **살아나는** 지점이다. 이 함수가 끝나면
 * 드라이버가 붙어 장치가 동작한다.
 *
 * 순서가 이 함수의 전부이며, 각 단계가 앞 단계에 기댄다.
 * 1. 아키텍처 훅과 final 쿼크. 쿼크가 여기 있는 이유는 위 상류 주석이
 *    밝힌다 — pci_device_add() 시점에는 자원이 아직 배정되지 않은 장치가
 *    있어 그때 부를 수 없었다.
 * 2. 브리지면 디바이스 트리 노드를 만든다.
 * 3. sysfs 와 procfs 항목 생성.
 * 4. 브리지의 D3 가능 여부 갱신.
 * 5. config 공간 저장. 옆의 상류 주석대로 오류 복구에 쓰기 위해서다 —
 *    리셋 뒤에 되돌릴 원본이 있어야 한다.
 * 6. 런타임 PM 활성화. 위 상류 주석이 왜 여기여야 하는지를 밝힌다.
 *    활성화하는 순간 장치가 곧바로 절전에 들어갈 수 있으므로, PCI 상태가
 *    완전히 갖춰진 뒤여야 한다.
 * 7. 디바이스 트리가 이 노드를 사용 가능으로 표시했을 때만 바인딩을 허용한다.
 *    트리에 status = "disabled" 로 적힌 장치는 드라이버를 붙이지 않는다.
 * 8. 드라이버 바인딩 시작. 여기서 드라이버의 probe 가 불린다.
 * 9. 등록 완료 표시. pci_bus_add_devices() 가 이 표시로 중복 등록과
 *    실패한 장치를 가려낸다.
 *
 * 실행 컨텍스트: 열거 또는 핫플러그. 프로세스 컨텍스트이며 드라이버 probe 가
 * 연쇄로 불려 오래 걸린다.
 *
 * 에러 경로: 반환값이 없다. 바인딩 실패는 오류가 아니며(맞는 드라이버가
 * 없을 뿐이다) 장치는 그대로 남는다.
 *
 * 호출 체인:
 *   pci_bus_add_devices() / 핫플러그 → [이 함수]
 *     → pcibios_bus_add_device() → pci_create_sysfs_dev_files()
 *     → pci_save_state() → pm_runtime_enable() → device_initial_probe()
 */
void pci_bus_add_device(struct pci_dev *dev)
{
	struct device_node *dn = dev->dev.of_node;

	/*
	 * Can not put in pci_device_add yet because resources
	 * are not assigned yet for some devices.
	 */
	pcibios_bus_add_device(dev);
	pci_fixup_device(pci_fixup_final, dev);
	/* [한국어] 브리지라면, */
	if (pci_is_bridge(dev))
		/* [한국어] 디바이스 트리에 이 브리지 노드를 만든다. 그 아래 장치를 트리로 서술하려면
		 * 부모 노드가 먼저 있어야 한다. */
		of_pci_make_dev_node(dev);
	pci_create_sysfs_dev_files(dev);
	pci_proc_attach_device(dev);
	pci_bridge_d3_update(dev);

	/* Save config space for error recoverability */
	pci_save_state(dev);

	/*
	 * Enable runtime PM, which potentially allows the device to
	 * suspend immediately, only after the PCI state has been
	 * configured completely.
	 */
	pm_runtime_enable(&dev->dev);

	if (!dn || of_device_is_available(dn))
		/* [한국어] 디바이스 트리 노드가 없거나(트리를 쓰지 않는 시스템) 사용 가능으로 표시돼 있을 때만
		 * 드라이버 바인딩을 허용한다. status = "disabled" 로 적힌 장치는 여기서 걸러진다. */
		pci_dev_allow_binding(dev);

	device_initial_probe(&dev->dev);

	pci_dev_assign_added(dev);
}
EXPORT_SYMBOL_GPL(pci_bus_add_device);

/**
 * pci_bus_add_devices - start driver for PCI devices
 * @bus: bus to check for new devices
 *
 * Start driver for PCI devices and add some sysfs entries.
 */

/* [한국어]
 * pci_bus_add_devices - 이 버스와 그 아래 전부의 장치를 등록한다
 *
 * @bus: 시작 버스.
 *
 * pci_bus_add_device() 를 트리 전체에 적용한다.
 *
 * 루프가 **둘로 나뉜 것** 이 이 함수의 핵심이다. 먼저 이 버스의 장치를 모두
 * 등록하고, 그 다음에야 하위 버스로 내려간다.
 *
 * 그 순서여야 하는 이유는 브리지 때문이다. 브리지의 드라이버가 붙어야
 * 그 아래 버스가 제대로 동작하는데, 한 루프에서 등록과 재귀를 함께 하면
 * 브리지가 준비되기 전에 그 아래로 내려가는 경우가 생긴다.
 *
 * 두 루프 모두 등록 표시를 본다. 앞의 루프는 이미 등록된 것을 건너뛰어
 * 핫플러그로 부분적으로 채워진 버스를 안전하게 다루고, 뒤의 루프는 등록에
 * 실패한 장치의 하위로는 내려가지 않는다.
 *
 * 실행 컨텍스트: 열거 마무리. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 개별 실패는 등록 표시가 없는 것으로 나타나고, 그
 * 하위 트리는 건너뛴다.
 *
 * 호출 체인:
 *   호스트 브리지 드라이버 / 핫플러그 → [이 함수]
 *     → pci_bus_add_device() → [이 함수](하위 버스마다 재귀)
 */
void pci_bus_add_devices(const struct pci_bus *bus)
{
	struct pci_dev *dev;
	struct pci_bus *child;

	/* [한국어] 먼저 이 버스의 장치를 모두 등록한다. 하위로 내려가는 것은 그 다음 루프이며,
	 * 브리지 드라이버가 붙은 뒤에야 그 아래 버스를 다뤄야 하기 때문이다. */
	list_for_each_entry(dev, &bus->devices, bus_list) {
		/* Skip already-added devices */
		if (pci_dev_is_added(dev))
			continue;
		pci_bus_add_device(dev);
	}

	list_for_each_entry(dev, &bus->devices, bus_list) {
		/* Skip if device attach failed */
		if (!pci_dev_is_added(dev))
			continue;
		child = dev->subordinate;
		/* [한국어] 브리지라서 하위 버스를 갖고 있으면, */
		if (child)
			/* [한국어] 그 아래로 재귀한다. 위 루프가 이 버스의 장치를 모두 등록한 뒤이므로,
			 * 브리지 드라이버가 이미 붙어 있다. */
			pci_bus_add_devices(child);
	}
}
/* [한국어] 주의: 원본은 EXPORT_SYMBOL(GPL 아님)이다. 내보내기 종류는 코드이므로
 * 주석을 달면서 바꾸면 안 된다 — 비-GPL 모듈이 이 심볼을 못 쓰게 된다. */
EXPORT_SYMBOL(pci_bus_add_devices);

/* [한국어]
 * __pci_walk_bus - 버스 트리를 앞에서부터 훑으며 콜백을 부른다
 *
 * @top: 시작 버스.
 * @cb: 장치마다 부를 콜백.
 * @userdata: 콜백에 함께 넘길 포인터.
 * @return: 콜백이 0 이 아닌 값을 돌려주면 그 값, 아니면 0.
 *
 * 재귀로 트리 전체를 훑는다. 장치를 먼저 처리하고 그 아래로 내려가므로,
 * 부모가 자식보다 먼저 불린다.
 *
 * 이 순서가 맞는 동작이 있다 — 예를 들어 전원을 켜는 일은 브리지가 먼저
 * 켜져야 그 아래가 살아난다.
 *
 * 콜백이 0 이 아닌 값을 돌려주면 즉시 멈추고 그 값을 올려보낸다. 찾기를
 * 겸하는 쓰임(원하는 장치를 만나면 멈춤)과 오류 전파를 함께 지원하는 규약이다.
 *
 * 잠금은 잡지 않는다. 호출자인 pci_walk_bus() 나 pci_walk_bus_locked() 가
 * pci_bus_sem 을 책임진다 — 재귀 함수가 직접 잡으면 재진입 시 교착한다.
 *
 * 실행 컨텍스트: pci_bus_sem 을 읽기로 쥔 상태. 프로세스 컨텍스트.
 *
 * 에러 경로: 콜백의 반환값을 그대로 올려보낸다.
 *
 * 호출 체인:
 *   pci_walk_bus() / pci_walk_bus_locked() → [이 함수](재귀) → cb()
 */
static int __pci_walk_bus(struct pci_bus *top, int (*cb)(struct pci_dev *, void *),
			  void *userdata)
{
	struct pci_dev *dev;
	int ret = 0;

	list_for_each_entry(dev, &top->devices, bus_list) {
		/* [한국어] 이 장치에 콜백을 적용한다. 자신을 먼저 처리하는 것이 이 방향의 정의다. */
		ret = cb(dev, userdata);
		/* [한국어] 콜백이 0 이 아닌 값을 돌려주면, */
		if (ret)
			/* [한국어] 즉시 멈춘다. 찾기를 겸하는 쓰임과 오류 전파를 함께 지원하는 규약이다. */
			break;
		if (dev->subordinate) {
			/* [한국어] 하위 버스로 내려간다. */
			ret = __pci_walk_bus(dev->subordinate, cb, userdata);
			/* [한국어] 아래에서 멈춤 신호가 올라왔으면, */
			if (ret)
				/* [한국어] 여기서도 멈춘다. 이렇게 재귀 전체가 한 번에 풀린다. */
				break;
		}
	}
	return ret;
}

/* [한국어]
 * __pci_walk_bus_reverse - 버스 트리를 뒤에서부터 훑으며 콜백을 부른다
 *
 * @top: 시작 버스.
 * @cb: 장치마다 부를 콜백.
 * @userdata: 콜백에 함께 넘길 포인터.
 * @return: 콜백이 0 이 아닌 값을 돌려주면 그 값, 아니면 0.
 *
 * __pci_walk_bus() 와 **두 가지** 가 뒤집혀 있다. 목록을 거꾸로 돌고,
 * 자식을 먼저 처리한 뒤 자신을 처리한다.
 *
 * 그 둘이 함께 뒤집혀야 진짜 역순이 된다. 자식 먼저가 필요한 동작이 있는데,
 * 전원을 끄거나 장치를 제거할 때가 그렇다 — 브리지를 먼저 끄면 그 아래
 * 장치에 접근할 수 없게 되어 정리를 마치지 못한다.
 *
 * 이 파일의 remove 경로와 절전 경로가 그래서 이 방향을 쓴다.
 *
 * 여기서도 잠금은 호출자의 몫이다.
 *
 * 실행 컨텍스트: pci_bus_sem 을 읽기로 쥔 상태. 프로세스 컨텍스트.
 *
 * 에러 경로: 콜백의 반환값을 그대로 올려보낸다.
 *
 * 호출 체인:
 *   pci_walk_bus_reverse() → [이 함수](재귀) → cb()
 */
static int __pci_walk_bus_reverse(struct pci_bus *top,
				  int (*cb)(struct pci_dev *, void *),
				  void *userdata)
{
	struct pci_dev *dev;
	int ret = 0;

	list_for_each_entry_reverse(dev, &top->devices, bus_list) {
		/* [한국어] 하위 버스가 있으면, */
		if (dev->subordinate) {
			/* [한국어] **먼저** 그 아래를 다 처리한다. 자신을 나중에 처리하는 것이 역순의 정의이며,
			 * 전원 끄기나 제거처럼 자식이 먼저여야 하는 동작을 위한 것이다. */
			ret = __pci_walk_bus_reverse(dev->subordinate, cb,
					     /* [한국어] 콜백과 사용자 자료를 그대로 넘긴다. */
					     userdata);
			if (ret)
				/* [한국어] 아래에서 멈췄으면 여기서도 멈춘다. */
				break;
		}
		ret = cb(dev, userdata);
		/* [한국어] 콜백이 멈춤을 알렸으면, */
		if (ret)
			/* [한국어] 루프를 빠져나간다. */
			break;
	}
	return ret;
}

/**
 *  pci_walk_bus - walk devices on/under bus, calling callback.
 *  @top: bus whose devices should be walked
 *  @cb: callback to be called for each device found
 *  @userdata: arbitrary pointer to be passed to callback
 *
 *  Walk the given bus, including any bridged devices
 *  on buses under this bus.  Call the provided callback
 *  on each device found.
 *
 *  We check the return of @cb each time. If it returns anything
 *  other than 0, we break out.
 */

/* [한국어]
 * pci_walk_bus - 잠금을 잡고 버스 트리를 훑는다
 *
 * @top: 시작 버스.
 * @cb: 장치마다 부를 콜백.
 * @userdata: 콜백에 함께 넘길 포인터.
 *
 * 공개 진입점이다. pci_bus_sem 을 읽기로 잡고 __pci_walk_bus() 를 부른다.
 *
 * 잠금이 필요한 이유는 순회 중에 장치가 추가되거나 제거되면 목록이 끊어지기
 * 때문이다. 읽기 잠금이면 충분한데, 여러 순회가 동시에 일어나도 서로
 * 방해하지 않고 목록을 바꾸는 쪽만 막으면 되기 때문이다.
 *
 * 그래서 콜백은 장치를 추가하거나 제거해서는 안 된다 — 그 안에서 쓰기
 * 잠금을 잡으려 하면 교착한다. 이미 잠금을 쥔 호출자를 위해서는
 * pci_walk_bus_locked() 가 따로 있다.
 *
 * AER 복구, D3 전환, 리셋처럼 "이 아래 전부" 를 대상으로 하는 동작이
 * 이것을 쓴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. rwsem 을 잡으므로 잠들 수 있다.
 *
 * 에러 경로: 반환값이 없어 콜백의 오류가 버려진다. 오류를 받아야 하면
 * __pci_walk_bus() 를 직접 쓰는 경로를 택해야 한다.
 *
 * 호출 체인:
 *   err.c / pci.c 등 → [이 함수]
 *     → down_read(pci_bus_sem) → __pci_walk_bus() → up_read()
 */
void pci_walk_bus(struct pci_bus *top, int (*cb)(struct pci_dev *, void *), void *userdata)
{
	down_read(&pci_bus_sem);
	__pci_walk_bus(top, cb, userdata);
	/* [한국어] 순회가 끝났으니 읽기 잠금을 놓는다. */
	up_read(&pci_bus_sem);
}
EXPORT_SYMBOL_GPL(pci_walk_bus);

/**
 * pci_walk_bus_reverse - walk devices on/under bus, calling callback.
 * @top: bus whose devices should be walked
 * @cb: callback to be called for each device found
 * @userdata: arbitrary pointer to be passed to callback
 *
 * Same semantics as pci_walk_bus(), but walks the bus in reverse order.
 */

/* [한국어]
 * pci_walk_bus_reverse - 잠금을 잡고 버스 트리를 역순으로 훑는다
 *
 * @top: 시작 버스.
 * @cb: 장치마다 부를 콜백.
 * @userdata: 콜백에 함께 넘길 포인터.
 *
 * pci_walk_bus() 와 같은 구조이며 __pci_walk_bus_reverse() 를 부른다는 것만
 * 다르다. 위 영어 주석도 그렇게 말한다.
 *
 * 자식을 먼저 다뤄야 하는 동작 — 전원 끄기, 제거 준비 — 이 이것을 쓴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. rwsem 을 잡으므로 잠들 수 있다.
 *
 * 에러 경로: 반환값이 없어 콜백의 오류가 버려진다.
 *
 * 호출 체인:
 *   절전·제거 경로 → [이 함수]
 *     → down_read(pci_bus_sem) → __pci_walk_bus_reverse() → up_read()
 */
void pci_walk_bus_reverse(struct pci_bus *top,
			  int (*cb)(struct pci_dev *, void *), void *userdata)
{
	down_read(&pci_bus_sem);
	__pci_walk_bus_reverse(top, cb, userdata);
	/* [한국어] 역순 순회가 끝났으니 읽기 잠금을 놓는다. */
	up_read(&pci_bus_sem);
}
EXPORT_SYMBOL_GPL(pci_walk_bus_reverse);

/* [한국어]
 * pci_walk_bus_locked - 이미 잠금을 쥔 호출자를 위한 순회
 *
 * @top: 시작 버스.
 * @cb: 장치마다 부를 콜백.
 * @userdata: 콜백에 함께 넘길 포인터.
 *
 * pci_walk_bus() 에서 잠금만 뺀 판이다. 이미 pci_bus_sem 을 쥐고 있는
 * 경로에서 부르며, 그때 pci_walk_bus() 를 부르면 rwsem 을 두 번 잡아
 * 교착하거나 lockdep 경고를 낸다.
 *
 * lockdep_assert_held() 가 그 규약을 강제한다. 디버그 빌드에서 잠금 없이
 * 부르면 경고가 뜨므로, 규약을 어긴 호출을 개발 중에 잡아낸다.
 *
 * 실행 컨텍스트: pci_bus_sem 을 이미 쥔 상태. 프로세스 컨텍스트.
 *
 * 에러 경로: 반환값이 없어 콜백의 오류가 버려진다.
 *
 * 호출 체인:
 *   pci_bus_sem 을 이미 쥔 경로 → [이 함수] → __pci_walk_bus()
 */
void pci_walk_bus_locked(struct pci_bus *top, int (*cb)(struct pci_dev *, void *), void *userdata)
{
	lockdep_assert_held(&pci_bus_sem);

	__pci_walk_bus(top, cb, userdata);
}

/* [한국어]
 * pci_bus_get - 버스의 참조 수를 올린다
 *
 * @bus: 대상 버스. NULL 이어도 된다.
 * @return: 넘겨받은 그 버스.
 *
 * 버스도 장치 모델의 객체라 참조로 수명이 관리된다. 이 함수를 부른 쪽이
 * 놓기 전까지 그 버스는 해제되지 않는다.
 *
 * NULL 을 허용하고 그대로 돌려주는 것이 호출부를 짧게 만든다.
 * `bus = pci_bus_get(maybe_null)` 을 조건 없이 쓸 수 있다.
 *
 * pci_bus_put() 과 반드시 짝을 이뤄야 한다.
 *
 * 실행 컨텍스트: 어디서든. get_device() 는 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   버스를 붙잡아 두려는 코드 → [이 함수] → get_device()
 */
struct pci_bus *pci_bus_get(struct pci_bus *bus)
{
	if (bus)
		get_device(&bus->dev);
	return bus;
}

/* [한국어]
 * pci_bus_put - 버스의 참조 수를 내린다
 *
 * @bus: 대상 버스. NULL 이어도 된다.
 *
 * pci_bus_get() 의 짝이다. 마지막 참조가 내려가면 장치 모델이 버스를
 * 해제한다.
 *
 * 여기서도 NULL 을 허용해 정리 경로를 짧게 만든다.
 *
 * 실행 컨텍스트: 어디서든. 다만 마지막 참조라면 해제 콜백이 이어져
 * 잠들 수 있는 문맥이 안전하다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   버스 사용을 마친 코드 → [이 함수] → put_device()
 */
void pci_bus_put(struct pci_bus *bus)
{
	if (bus)
		put_device(&bus->dev);
}
