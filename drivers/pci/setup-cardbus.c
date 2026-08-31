// SPDX-License-Identifier: GPL-2.0
/*
 * Cardbus bridge setup routines.
 */

/*
 * [한국어 설명] CardBus 브리지의 자원 예약과 설정 (setup-cardbus.c)
 *
 * === 파일의 역할 ===
 * CardBus 는 노트북의 PC Card 슬롯을 32비트 PCI 로 확장한 규격이다.
 * 2000년대 초의 기술이고 ExpressCard 를 거쳐 지금은 Thunderbolt 로
 * 대체됐다. 이 파일은 그 브리지를 설정하는 코드를 담고 있으며,
 * 사실상 레거시다.
 *
 * 일반 PCI-to-PCI 브리지와 다른 점이 이 파일의 존재 이유다.
 *
 *   1) 자원을 미리 예약해야 한다. PC Card 는 언제든 꽂힐 수 있는데,
 *      꽂힌 뒤에 자원을 배정하려면 이미 다른 장치가 주소 공간을 다
 *      차지한 뒤일 수 있다. 그래서 슬롯이 비어 있어도 미리 일정량을
 *      잡아 둔다(기본 I/O 256바이트, 메모리 64MB).
 *
 *   2) 윈도우가 네 개다. 일반 브리지는 I/O 하나와 메모리 하나(+프리페치
 *      하나)를 갖지만, CardBus 브리지는 I/O 두 개와 메모리 두 개를 갖는다.
 *      PC Card 하나가 여러 자원 영역을 요구할 수 있어서다.
 *      그래서 PCI_CB_BRIDGE_IO_0/1_WINDOW, MEM_0/1_WINDOW 네 개를 다룬다.
 *
 *   3) 버스 번호도 미리 잡아 둔다(CARDBUS_RESERVE_BUSNR = 3). 꽂힌 카드가
 *      자기 아래 또 브리지를 가질 수 있기 때문이다.
 *
 * 예약량은 부팅 인자로 조정할 수 있다 — "pci=cbiosize=nn,cbmemsize=nnM".
 * 예약이 지나치면 다른 장치가 쓸 주소 공간이 줄고, 모자라면 카드를
 * 꽂았을 때 동작하지 않는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 열거: probe.c 가 CardBus 브리지를 발견
 *         -> [이 파일] pci_cardbus_scan_bridge_extend() 로 버스 번호 배정
 *
 * 크기 계산: setup-bus.c 의 자원 배치
 *         -> [이 파일] pci_bus_size_cardbus_bridge() 로 예약량을 자원에 반영
 *
 * 기록: setup-bus.c 가 주소를 정한 뒤
 *         -> [이 파일] pci_setup_cardbus_bridge() 로 네 윈도우를 config 에 쓴다
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 열거와 자원 배치 경로다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: probe.c(열거), setup-bus.c(자원 배치), pci.c(부팅 인자 파싱).
 * 아래쪽: access.c 의 config 접근, host-bridge.c 의 주소 변환.
 * 옆쪽: drivers/pcmcia/ 의 yenta_socket 등 실제 CardBus 컨트롤러 드라이버.
 * 공유 상태: pci_cardbus_io_size / pci_cardbus_mem_size 두 전역 변수.
 *
 * === NVMe 관점 ===
 * NVMe 와는 아무 관련이 없다. CardBus 는 NVMe 가 나오기 훨씬 전의
 * 기술이고, 두 규격이 같은 시스템에 공존하는 경우도 사실상 없다.
 *
 * 학습 관점에서 하나 볼 만한 것은 "핫플러그를 위한 자원 예약" 이라는
 * 발상이다. 같은 문제를 현대 PCIe 핫플러그도 갖고 있고, 그 해법이
 * pci.c 의 pci_hotplug_io_size / pci_hotplug_mmio_size 부팅 인자다.
 * 30년 가까이 같은 문제를 같은 방식으로 풀고 있는 셈이다.
 *
 * === 주요 함수/구조체 요약 ===
 * pci_cardbus_resource_alignment() : 자원 종류에 따른 정렬 요구를 돌려준다.
 *                                    예약 크기가 곧 정렬 단위다.
 * pci_bus_size_cardbus_bridge()    : 네 윈도우에 예약량을 설정한다.
 *                                    realloc_head 가 있으면 "선택적 추가
 *                                    크기" 로 등록해, 자리가 있으면 더 받고
 *                                    없으면 최소만 받게 한다.
 * pci_setup_cardbus_bridge()       : 정해진 주소를 네 윈도우 레지스터에 쓴다.
 * pci_setup_cardbus()              : "pci=cbiosize=/cbmemsize=" 인자를 파싱한다.
 *                                    pci.c 의 pci_setup() 이 부른다.
 * pci_cardbus_scan_bridge_extend() : 버스 번호를 배정하고 하위를 열거한다.
 *                                    일반 브리지의 pci_scan_bridge_extend()
 *                                    와 짝을 이루는 CardBus 판이다.
 */

#include <linux/bitfield.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/pci.h>
#include <linux/sizes.h>
#include <linux/sprintf.h>
#include <linux/types.h>

#include "pci.h"

/* [한국어] 세컨더리 버스의 지연 타이머에 넣을 값. 브리지가 버스를
 * 점유할 수 있는 최대 클럭 수이며, 이 값(176)은 오랜 경험으로 정착한
 * 절충치다. 너무 크면 다른 장치가 굶고, 너무 작으면 전송이 잘게 끊긴다. */
#define CARDBUS_LATENCY_TIMER		176	/* secondary latency timer */
/* [한국어] CardBus 브리지마다 미리 잡아 둘 버스 번호 개수.
 * 꽂힌 카드가 자기 아래 또 브리지를 가질 수 있어 여유를 둔다. */
#define CARDBUS_RESERVE_BUSNR		3

/* [한국어] 슬롯이 비어 있어도 미리 예약할 I/O 공간 크기(256바이트).
 * I/O 주소 공간은 x86 에서 64KB 뿐이라 인색하게 잡는다. */
#define DEFAULT_CARDBUS_IO_SIZE		SZ_256
/* [한국어] 예약할 메모리 공간 크기(64MB). 메모리는 넉넉하므로 크게 잡는다. */
#define DEFAULT_CARDBUS_MEM_SIZE	SZ_64M
/* pci=cbmemsize=nnM,cbiosize=nn can override this */
/* [한국어] 실제로 쓰이는 예약 크기. 위 기본값으로 시작하고,
 * 부팅 인자가 있으면 pci_setup_cardbus() 가 덮어쓴다.
 * 설정자: pci_setup_cardbus() (부팅 중 1회).
 * 읽는 자: pci_cardbus_resource_alignment(), pci_bus_size_cardbus_bridge().
 * 동기화: 부팅 중 한 번 정해지고 이후 읽기만 하므로 보호가 없다. */
static unsigned long pci_cardbus_io_size = DEFAULT_CARDBUS_IO_SIZE;
static unsigned long pci_cardbus_mem_size = DEFAULT_CARDBUS_MEM_SIZE;

/*
 * [한국어]
 * pci_cardbus_resource_alignment - CardBus 자원의 정렬 요구를 돌려준다
 *
 * @res:    대상 자원
 * @return: 바이트 단위 정렬. 해당 없으면 0.
 *
 * CardBus 윈도우는 예약 크기 단위로 정렬되어야 한다. 예약 크기가 곧
 * 정렬 단위인 셈이라, 이 함수는 자원 종류만 보고 그 크기를 돌려준다.
 *
 * setup-bus.c 의 자원 배치가 각 자원의 정렬을 물을 때 이 값을 쓴다.
 * CardBus 가 아닌 자원에는 0 을 돌려주어 "특별한 요구 없음" 을 알린다.
 *
 * 실행 컨텍스트: 제약 없음. 플래그 검사와 전역 읽기뿐이다.
 * 호출자: setup-bus.c 의 정렬 계산.
 */
unsigned long pci_cardbus_resource_alignment(struct resource *res)
{
	/* [한국어] I/O 윈도우 — I/O 예약 크기 단위로 정렬. */
	if (res->flags & IORESOURCE_IO)
		return pci_cardbus_io_size;
	/* [한국어] 메모리 윈도우 — 메모리 예약 크기 단위로 정렬. */
	if (res->flags & IORESOURCE_MEM)
		return pci_cardbus_mem_size;
	/* [한국어] 둘 다 아니면 CardBus 윈도우가 아니다. 0 = 요구 없음. */
	return 0;
}

/*
 * [한국어]
 * pci_bus_size_cardbus_bridge - 네 윈도우에 예약 크기를 설정한다
 *
 * @bus:          CardBus 브리지가 만든 버스
 * @realloc_head: 선택적 추가 크기를 등록할 목록. NULL 이면 등록하지 않는다.
 * @return:       항상 0.
 *
 * 슬롯이 비어 있어도 자원을 미리 잡아 두는 것이 이 함수의 일이다.
 * 카드가 나중에 꽂혔을 때 쓸 공간이 없으면 곤란하기 때문이다.
 *
 * realloc_head 가 있으면 "두 단계 예약" 을 한다.
 *   - 자원 자체의 크기는 0 으로 줄인다(end -= size).
 *   - 그 크기를 "있으면 좋은 추가분" 으로 목록에 등록한다.
 * 그러면 setup-bus.c 가 먼저 필수 자원을 다 배치한 뒤, 남는 공간이
 * 있을 때만 이 추가분을 준다. 주소 공간이 빠듯한 시스템에서 CardBus
 * 예약 때문에 실제 장치가 자리를 잃는 것을 막는 장치다.
 *
 * 네 윈도우를 순서대로 처리하며, 이미 주소가 배정된 것은 건너뛴다
 * (goto handle_b_res_N 사슬이 그 건너뛰기다).
 *
 * 프리페치 메모리 처리가 특이하다. MEM0 이 프리페치를 지원하면 두
 * 영역으로 나눠 잡고(프리페치용 + 일반용), 지원하지 않으면 MEM1 하나에
 * 두 배 크기를 잡는다. b_res_3_size 가 그 분기를 담는 변수다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. config 접근이 있다.
 * 호출자: setup-bus.c 의 크기 계산 패스.
 */
int pci_bus_size_cardbus_bridge(struct pci_bus *bus,
				struct list_head *realloc_head)
{
	struct pci_dev *bridge = bus->self;	/* [한국어] 이 버스를 만든 CardBus 브리지 */
	struct resource *b_res;			/* [한국어] 지금 다루는 윈도우 */
	/* [한국어] MEM1 에 잡을 크기. 기본은 두 배(프리페치를 못 쓰는 경우)이고,
	 * MEM0 이 프리페치를 지원하면 아래에서 절반으로 줄인다. */
	resource_size_t b_res_3_size = pci_cardbus_mem_size * 2;
	u16 ctrl;	/* [한국어] CardBus Bridge Control 레지스터 값 */

	/* [한국어] 첫 번째 I/O 윈도우. 이미 주소가 배정돼 있으면 건드리지 않는다 —
	 * 펌웨어가 설정해 둔 것을 존중한다. */
	b_res = &bridge->resource[PCI_CB_BRIDGE_IO_0_WINDOW];
	if (resource_assigned(b_res))
		goto handle_b_res_1;
	/*
	 * Reserve some resources for CardBus.  We reserve a fixed amount
	 * of bus space for CardBus bridges.
	 */
	/* [한국어] start 자리에 정렬값을, size 자리에 크기를 넣는다.
	 * 아래 STARTALIGN 플래그와 짝을 이뤄 "이 크기만큼, 이 경계에 맞춰" 를
	 * 표현한다(setup-res.c 의 pci_request_resource_alignment 주석 참고). */
	resource_set_range(b_res, pci_cardbus_io_size, pci_cardbus_io_size);
	b_res->flags |= IORESOURCE_IO | IORESOURCE_STARTALIGN;
	if (realloc_head) {
		/* [한국어] 두 단계 예약. 자원 크기를 0 으로 줄이고 그만큼을
		 * "추가로 있으면 좋은 것" 으로 등록한다. 필수 자원이 아니게 되어
		 * 주소 공간이 빠듯할 때 양보한다. */
		b_res->end -= pci_cardbus_io_size;
		pci_dev_res_add_to_list(realloc_head, bridge, b_res,
					pci_cardbus_io_size,
					pci_cardbus_io_size);
	}

handle_b_res_1:
	/* [한국어] 두 번째 I/O 윈도우. 위와 완전히 같은 처리다.
	 * CardBus 브리지가 I/O 윈도우를 둘 갖는 것은 카드 하나가 서로 떨어진
	 * 두 I/O 영역을 요구할 수 있기 때문이다. */
	b_res = &bridge->resource[PCI_CB_BRIDGE_IO_1_WINDOW];
	if (resource_assigned(b_res))
		goto handle_b_res_2;
	/* [한국어] 첫 번째 윈도우와 같은 예약. 정렬과 크기 모두 I/O 예약 크기다. */
	resource_set_range(b_res, pci_cardbus_io_size, pci_cardbus_io_size);
	b_res->flags |= IORESOURCE_IO | IORESOURCE_STARTALIGN;
	if (realloc_head) {
		/* [한국어] 첫 번째 윈도우와 같은 두 단계 예약. */
		b_res->end -= pci_cardbus_io_size;
		/* [한국어] 크기와 정렬을 같은 값으로 등록한다. */
		pci_dev_res_add_to_list(realloc_head, bridge, b_res,
					pci_cardbus_io_size,
					pci_cardbus_io_size);
	}

handle_b_res_2:
	/* MEM1 must not be pref MMIO */
	/* [한국어] MEM1 은 반드시 비프리페치여야 한다. 프리페치 비트가 서 있으면
	 * 내리고, 되읽어 실제로 내려갔는지 확인한다. 되읽기가 필요한 이유는
	 * 이 비트가 하드와이어된 브리지가 있기 때문이다 — 쓰기가 무시될 수 있다. */
	pci_read_config_word(bridge, PCI_CB_BRIDGE_CONTROL, &ctrl);
	if (ctrl & PCI_CB_BRIDGE_CTL_PREFETCH_MEM1) {
		ctrl &= ~PCI_CB_BRIDGE_CTL_PREFETCH_MEM1;
		pci_write_config_word(bridge, PCI_CB_BRIDGE_CONTROL, ctrl);
		/* [한국어] 되읽어 ctrl 을 실제 하드웨어 상태로 갱신한다.
		 * 쓰기가 무시됐을 수 있으므로 아래 판정은 이 갱신된 값으로 해야 한다. */
		pci_read_config_word(bridge, PCI_CB_BRIDGE_CONTROL, &ctrl);
	}

	/* Check whether prefetchable memory is supported by this bridge. */
	/* [한국어] MEM0 의 프리페치 지원 여부를 알아내는 방법이 영리하다 —
	 * 그냥 물어볼 수단이 없으므로, 비트를 세워 보고 되읽어 남아 있는지
	 * 확인한다. 지원하지 않는 브리지에서는 쓰기가 무시되어 0 으로 남는다.
	 * 그 결과가 아래 윈도우 배치 방식을 가른다. */
	pci_read_config_word(bridge, PCI_CB_BRIDGE_CONTROL, &ctrl);
	if (!(ctrl & PCI_CB_BRIDGE_CTL_PREFETCH_MEM0)) {
		ctrl |= PCI_CB_BRIDGE_CTL_PREFETCH_MEM0;
		pci_write_config_word(bridge, PCI_CB_BRIDGE_CONTROL, ctrl);
		/* [한국어] 이 되읽기가 곧 지원 여부 판정이다. 비트가 남아 있으면
		 * 프리페치를 지원하고, 0 으로 돌아왔으면 지원하지 않는다. */
		pci_read_config_word(bridge, PCI_CB_BRIDGE_CONTROL, &ctrl);
	}

	/* [한국어] 첫 번째 메모리 윈도우. */
	b_res = &bridge->resource[PCI_CB_BRIDGE_MEM_0_WINDOW];
	if (resource_assigned(b_res))
		goto handle_b_res_3;
	/*
	 * If we have prefetchable memory support, allocate two regions.
	 * Otherwise, allocate one region of twice the size.
	 */
	/* [한국어] 위 영어 주석대로, 프리페치를 지원하면 MEM0 을 프리페치용으로
	 * 잡고 MEM1 을 일반용으로 잡아 각각 기본 크기씩 준다. 지원하지 않으면
	 * MEM0 을 건너뛰고 MEM1 하나에 두 배를 몰아 준다 — 총량은 같다. */
	if (ctrl & PCI_CB_BRIDGE_CTL_PREFETCH_MEM0) {
		resource_set_range(b_res, pci_cardbus_mem_size,
				   pci_cardbus_mem_size);
		/* [한국어] PREFETCH 플래그를 붙여야 setup-bus.c 가 프리페치
		 * 윈도우에 배치한다. 프리페치 영역은 상위 브리지가 미리 읽어
		 * 둘 수 있어 성능이 좋지만, 부작용 있는 레지스터를 두면 안 된다. */
		b_res->flags |= IORESOURCE_MEM | IORESOURCE_PREFETCH |
				    IORESOURCE_STARTALIGN;
		if (realloc_head) {
			/* [한국어] I/O 윈도우와 같은 두 단계 예약. 크기를 0 으로
			 * 줄이고 그만큼을 선택적 추가분으로 등록한다. */
			b_res->end -= pci_cardbus_mem_size;
			pci_dev_res_add_to_list(realloc_head, bridge, b_res,
						pci_cardbus_mem_size,
						pci_cardbus_mem_size);
		}

		/* Reduce that to half */
		/* [한국어] MEM0 에 절반을 줬으므로 MEM1 은 나머지 절반만 필요하다. */
		b_res_3_size = pci_cardbus_mem_size;
	}

handle_b_res_3:
	/* [한국어] 두 번째 메모리 윈도우. 항상 비프리페치다(위에서 그 비트를
	 * 내렸다). 크기는 위 분기 결과에 따라 기본 크기이거나 그 두 배다. */
	b_res = &bridge->resource[PCI_CB_BRIDGE_MEM_1_WINDOW];
	if (resource_assigned(b_res))
		goto handle_done;
	/* [한국어] 정렬은 기본 크기 단위, 크기는 b_res_3_size.
	 * 두 값이 다를 수 있어 인자가 따로인 것에 주의. */
	resource_set_range(b_res, pci_cardbus_mem_size, b_res_3_size);
	b_res->flags |= IORESOURCE_MEM | IORESOURCE_STARTALIGN;
	if (realloc_head) {
		/* [한국어] 여기서는 크기(b_res_3_size)와 정렬(pci_cardbus_mem_size)이
		 * 다를 수 있어 두 인자가 서로 다른 값이다. 위 세 곳은 둘이 같았다. */
		b_res->end -= b_res_3_size;
		pci_dev_res_add_to_list(realloc_head, bridge, b_res,
					b_res_3_size, pci_cardbus_mem_size);
	}

handle_done:
	/* [한국어] 이 함수는 실패하지 않는다. 자원 구조체만 채웠을 뿐이고,
	 * 실제 배치가 가능한지는 setup-bus.c 가 나중에 판단한다. */
	return 0;
}

/*
 * [한국어]
 * pci_setup_cardbus_bridge - 정해진 주소를 네 윈도우 레지스터에 써 넣는다
 *
 * @bus:    CardBus 브리지가 만든 버스
 * @return: 없음.
 *
 * setup-bus.c 가 주소를 확정한 뒤, 그 값을 실제 하드웨어에 기록한다.
 * 일반 브리지의 pci_setup_bridge() 에 대응하는 CardBus 판이다.
 *
 * 네 윈도우를 같은 방식으로 처리한다 — 자원이 배정됐고 종류가 맞으면
 * base 와 limit 레지스터에 쓰고, 아니면 건너뛴다. 배정되지 않은 윈도우를
 * 그냥 두면 base > limit 인 초기 상태로 남아 자연히 비활성이 된다.
 *
 * 일반 브리지와 다른 점: base 와 limit 이 각각 별도의 32비트 레지스터다.
 * 일반 브리지는 하나의 dword 에 base 와 limit 을 16비트씩 나눠 담지만,
 * CardBus 는 32비트 주소를 그대로 다루므로 레지스터가 따로다.
 *
 * pcibios_resource_to_bus() 로 CPU 주소를 PCI 버스 주소로 바꾸는 것을
 * 잊으면 안 된다. 레지스터에는 버스 주소가 들어가야 하기 때문이다
 * (host-bridge.c 의 주소 변환 주석 참고).
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. config 쓰기가 있다.
 * 호출자: setup-bus.c 의 배치 완료 경로.
 */
void pci_setup_cardbus_bridge(struct pci_bus *bus)
{
	struct pci_dev *bridge = bus->self;	/* [한국어] 이 버스를 만든 CardBus 브리지 */
	struct resource *res;			/* [한국어] 지금 다루는 윈도우 */
	/* [한국어] CPU 주소를 PCI 버스 주소로 변환한 결과를 담을 곳.
	 * 두 주소가 같은 플랫폼이 많지만 다를 수 있어 반드시 변환한다. */
	struct pci_bus_region region;

	/* [한국어] 어느 버스 번호 범위를 담당하는지 로그에 남긴다.
	 * %pR 은 struct resource 를 "[bus 03-05]" 형태로 찍는 커널 확장 형식이다. */
	pci_info(bridge, "CardBus bridge to %pR\n",
		 &bus->busn_res);

	/* [한국어] 첫 번째 I/O 윈도우. */
	res = bus->resource[0];
	pcibios_resource_to_bus(bridge->bus, &region, res);
	/* [한국어] 주소가 배정됐고 실제로 I/O 자원인지 둘 다 확인한다.
	 * 배정되지 않았으면 쓸 값이 없고, 종류가 다르면 엉뚱한 레지스터에
	 * 쓰게 된다. */
	if (resource_assigned(res) && res->flags & IORESOURCE_IO) {
		/*
		 * The IO resource is allocated a range twice as large as it
		 * would normally need.  This allows us to set both IO regs.
		 */
		pci_info(bridge, "  bridge window %pR\n", res);
		/* [한국어] base 와 limit 이 각각 별도의 32비트 레지스터다.
		 * 일반 PCI-to-PCI 브리지가 하나의 dword 에 둘을 나눠 담는 것과
		 * 다른 점이다. */
		pci_write_config_dword(bridge, PCI_CB_IO_BASE_0,
					region.start);
		pci_write_config_dword(bridge, PCI_CB_IO_LIMIT_0,
					region.end);
	}

	/* [한국어] 두 번째 I/O 윈도우. 위와 같은 처리를 레지스터만 바꿔 반복한다. */
	res = bus->resource[1];
	pcibios_resource_to_bus(bridge->bus, &region, res);
	if (resource_assigned(res) && res->flags & IORESOURCE_IO) {
		/* [한국어] 어느 범위를 열었는지 로그에 남긴다. 자원 배치가
		 * 의도대로 됐는지 확인하는 근거가 된다. */
		pci_info(bridge, "  bridge window %pR\n", res);
		pci_write_config_dword(bridge, PCI_CB_IO_BASE_1,
					region.start);
		/* [한국어] limit 은 마지막 유효 주소다(끝 다음이 아니라).
		 * 그래서 struct resource 의 end 를 그대로 쓴다. */
		pci_write_config_dword(bridge, PCI_CB_IO_LIMIT_1,
					region.end);
	}

	/* [한국어] 첫 번째 메모리 윈도우. 프리페치용일 수 있다
	 * (pci_bus_size_cardbus_bridge 의 분기 참고). */
	res = bus->resource[2];
	pcibios_resource_to_bus(bridge->bus, &region, res);
	if (resource_assigned(res) && res->flags & IORESOURCE_MEM) {
		pci_info(bridge, "  bridge window %pR\n", res);
		/* [한국어] 메모리 윈도우 0. 프리페치용일 수 있지만 레지스터는 같다 —
		 * 프리페치 여부는 Bridge Control 의 비트로 따로 정한다. */
		pci_write_config_dword(bridge, PCI_CB_MEMORY_BASE_0,
					region.start);
		pci_write_config_dword(bridge, PCI_CB_MEMORY_LIMIT_0,
					region.end);
	}

	/* [한국어] 두 번째 메모리 윈도우. 항상 비프리페치다. */
	res = bus->resource[3];
	pcibios_resource_to_bus(bridge->bus, &region, res);
	if (resource_assigned(res) && res->flags & IORESOURCE_MEM) {
		pci_info(bridge, "  bridge window %pR\n", res);
		/* [한국어] 메모리 윈도우 1. 항상 비프리페치다. */
		pci_write_config_dword(bridge, PCI_CB_MEMORY_BASE_1,
					region.start);
		pci_write_config_dword(bridge, PCI_CB_MEMORY_LIMIT_1,
					region.end);
	}
}
EXPORT_SYMBOL(pci_setup_cardbus_bridge);

/*
 * [한국어]
 * pci_setup_cardbus - "pci=cbiosize=/cbmemsize=" 부팅 인자를 해석한다
 *
 * @str:    "pci=" 뒤의 한 항목
 * @return: 0 = 내가 처리한 옵션, -ENOENT = 내 것이 아님(다른 처리기에게 넘긴다).
 *
 * 반환값 규약이 특이하다. pci.c 의 pci_setup() 은 이 함수가 0 을 돌려주면
 * "처리됐다" 고 보고 나머지 비교를 건너뛴다. 그래서 모르는 옵션에는
 * 반드시 -ENOENT 를 돌려줘야 다른 옵션들이 정상 처리된다.
 *
 * memparse() 는 "1M", "256K", "0x100" 같은 표기를 해석하고, 두 번째 인자로
 * 파싱이 끝난 위치를 돌려준다. 여기서는 str 에 다시 담지만 그 값을 쓰지는
 * 않는다 — 한 항목이 하나의 값만 갖기 때문이다.
 *
 * 실행 컨텍스트: 부팅 초기(early_param). 메모리 할당기가 아직 없을 수 있다.
 * 호출자: pci.c 의 pci_setup().
 */
int pci_setup_cardbus(char *str)
{
	/* [한국어] I/O 예약 크기. 문자열 길이 9 는 "cbiosize=" 의 길이다. */
	if (!strncmp(str, "cbiosize=", 9)) {
		pci_cardbus_io_size = memparse(str + 9, &str);
		return 0;
	/* [한국어] 메모리 예약 크기. "cbmemsize=" 는 10자다. */
	} else if (!strncmp(str, "cbmemsize=", 10)) {
		pci_cardbus_mem_size = memparse(str + 10, &str);
		return 0;
	}

	/* [한국어] 내 옵션이 아니다. 호출자가 다음 비교를 계속하게 한다. */
	return -ENOENT;
}

/*
 * [한국어]
 * pci_cardbus_scan_bridge_extend - CardBus 브리지에 버스 번호를 배정한다
 *
 * @bus:             이 브리지가 붙어 있는 부모 버스
 * @dev:             CardBus 브리지 장치
 * @buses:           현재 PCI_PRIMARY_BUS dword 값(primary/secondary/subordinate/
 *                   latency timer 네 바이트가 한 dword 에 들어 있다)
 * @max:             지금까지 배정된 가장 큰 버스 번호
 * @available_buses: 이 브리지 아래에 쓸 수 있는 버스 번호 개수
 * @pass:            0 = 첫 번째 패스, 1 = 두 번째 패스
 * @return:          갱신된 max.
 *
 * 일반 브리지의 pci_scan_bridge_extend() 에 대응하는 CardBus 판이다.
 * 두 패스로 나뉘는 구조가 핵심이다.
 *
 *   1차 패스 — 아무 번호도 배정하지 않고, 대신 이 브리지의 config 전달을
 *     잠시 꺼 둔다. 위 원문 주석이 이유를 밝힌다: 2차 패스에서 두 브리지에
 *     겹치는 버스 범위를 써 넣는 순간이 있을 수 있는데, 그때 전달이 켜져
 *     있으면 같은 버스 번호에 두 브리지가 응답해 충돌한다.
 *   2차 패스 — 실제로 번호를 배정하고 하위를 열거한다.
 *
 * CardBus 고유의 처리가 두 가지다.
 *   - 버스 번호를 여유 있게 잡는다(CARDBUS_RESERVE_BUSNR = 3). 꽂힌 카드가
 *     자기 아래 또 브리지를 가질 수 있기 때문이다. 다만 그 여유가 다른
 *     브리지의 번호를 침범하면 절반으로 줄인다.
 *   - 세컨더리 지연 타이머를 176 으로 강제한다. 원문 주석이 밝히듯
 *     yenta.c(실제 CardBus 컨트롤러 드라이버)의 동작을 그대로 따른 것이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 열거 경로다.
 * 호출자: probe.c 의 pci_scan_bridge_extend() 가 CardBus 브리지를 만나면 위임한다.
 */
int pci_cardbus_scan_bridge_extend(struct pci_bus *bus, struct pci_dev *dev,
				   u32 buses, int max,
				   unsigned int available_buses, int pass)
{
	struct pci_bus *child;		/* [한국어] 이 브리지가 만들 하위 버스 */
	bool fixed_buses;		/* [한국어] EA capability 가 번호를 못박아 두었는가 */
	u8 fixed_sec, fixed_sub;	/* [한국어] 그 경우의 secondary/subordinate 번호 */
	int next_busnr;			/* [한국어] 하위 버스에 줄 번호 */
	/* [한국어] i = 여유로 잡을 버스 개수를 세는 루프 변수,
	 * j = "다른 브리지의 번호를 침범했다" 는 표시. 0 으로 초기화해야
	 * 침범이 없을 때 아래 if (j) 가 거짓이 된다. */
	u32 i, j = 0;

	/*
	 * We need to assign a number to this bus which we always do in the
	 * second pass.
	 */
	if (!pass) {
		/*
		 * Temporarily disable forwarding of the configuration
		 * cycles on all bridges in this bus segment to avoid
		 * possible conflicts in the second pass between two bridges
		 * programmed with overlapping bus ranges.
		 */
		/* [한국어] 지연 타이머만 남기고 세 버스 번호를 전부 0 으로 만든다.
		 * secondary=0 이면 이 브리지는 어떤 config 요청도 아래로 보내지 않는다.
		 * 2차 패스에서 번호를 다시 채울 때까지의 안전장치다. */
		pci_write_config_dword(dev, PCI_PRIMARY_BUS,
				       buses & PCI_SEC_LATENCY_TIMER_MASK);
		return max;	/* [한국어] 1차 패스는 번호를 소비하지 않는다 */
	}

	/* Clear errors */
	/* [한국어] Status 레지스터의 오류 비트를 지운다. RW1C 라 1 을 쓰면 지워지고,
	 * 0xffff 는 "모든 비트에 1" 이므로 전부 지운다는 뜻이다.
	 * 열거 과정에서 없는 장치를 읽어 생긴 오류 기록을 치우는 것이다. */
	pci_write_config_word(dev, PCI_STATUS, 0xffff);

	/* Read bus numbers from EA Capability (if present) */
	/* [한국어] EA(Enhanced Allocation) capability 가 있으면 버스 번호가
	 * 하드웨어에 못박혀 있어 커널이 고를 수 없다. 그 값을 읽어 온다. */
	fixed_buses = pci_ea_fixed_busnrs(dev, &fixed_sec, &fixed_sub);
	if (fixed_buses)
		next_busnr = fixed_sec;		/* [한국어] 하드웨어가 정한 번호 */
	else
		next_busnr = max + 1;		/* [한국어] 다음 빈 번호 */

	/*
	 * Prevent assigning a bus number that already exists. This can
	 * happen when a bridge is hot-plugged, so in this case we only
	 * re-scan this bus.
	 */
	/* [한국어] 그 번호의 버스가 이미 있는지 본다. 위 영어 주석이 설명하듯,
	 * 브리지를 핫플러그로 꽂았다 뺐다 하면 같은 번호가 재사용될 수 있다.
	 * 이미 있으면 새로 만들지 않고 재스캔만 한다. */
	child = pci_find_bus(pci_domain_nr(bus), next_busnr);
	if (!child) {
		child = pci_add_new_bus(bus, dev, next_busnr);
		if (!child)
			return max;	/* [한국어] 버스 객체를 못 만들면 포기. max 는 그대로 */
		/* [한국어] 일단 부모 버스의 끝까지를 이 버스의 범위로 잡아 둔다.
		 * 아래에서 실제 subordinate 를 정한 뒤 좁힌다. */
		pci_bus_insert_busn_res(child, next_busnr, bus->busn_res.end);
	}
	max++;	/* [한국어] 번호 하나를 소비했다 */
	if (available_buses)
		available_buses--;	/* [한국어] 남은 여유도 하나 줄인다 */

	/* [한국어] PCI_PRIMARY_BUS dword 를 새로 조립한다. 네 바이트가 각각
	 * primary / secondary / subordinate / 지연 타이머다.
	 * 지연 타이머는 기존 값을 유지하고(첫 항), 나머지 셋을 새로 채운다.
	 * FIELD_PREP 은 값을 그 필드의 비트 자리로 옮기는 매크로다. */
	buses = (buses & PCI_SEC_LATENCY_TIMER_MASK) |
		FIELD_PREP(PCI_PRIMARY_BUS_MASK, child->primary) |
		FIELD_PREP(PCI_SECONDARY_BUS_MASK, child->busn_res.start) |
		FIELD_PREP(PCI_SUBORDINATE_BUS_MASK, child->busn_res.end);

	/*
	 * yenta.c forces a secondary latency timer of 176.
	 * Copy that behaviour here.
	 */
	/* [한국어] 방금 유지한 지연 타이머를 다시 지우고 176 으로 덮어쓴다.
	 * 위에서 유지했다가 여기서 덮는 것이 어색해 보이지만, 두 동작이
	 * 서로 다른 관심사라 분리해 둔 것이다 — 앞은 "다른 필드를 건드리지
	 * 않는다", 여기는 "yenta 와 같은 값을 강제한다". */
	buses &= ~PCI_SEC_LATENCY_TIMER_MASK;
	buses |= FIELD_PREP(PCI_SEC_LATENCY_TIMER_MASK, CARDBUS_LATENCY_TIMER);

	/* We need to blast all three values with a single write */
	/* [한국어] 세 버스 번호를 한 번의 dword 쓰기로 동시에 바꾼다.
	 * 바이트 단위로 나눠 쓰면 중간 상태(예: secondary 는 새 값인데
	 * subordinate 는 옛 값)가 생겨, 그 순간 들어온 config 요청이
	 * 엉뚱하게 라우팅된다. */
	pci_write_config_dword(dev, PCI_PRIMARY_BUS, buses);

	/*
	 * For CardBus bridges, we leave 4 bus numbers as cards with a
	 * PCI-to-PCI bridge can be inserted later.
	 */
	/* [한국어] 여유 번호를 몇 개까지 잡을 수 있는지 세어 본다.
	 * 최대 CARDBUS_RESERVE_BUSNR(3) 개이며, 두 가지 조건에서 멈춘다. */
	for (i = 0; i < CARDBUS_RESERVE_BUSNR; i++) {
		struct pci_bus *parent = bus;	/* [한국어] 조상 버스를 훑을 커서 */

		/* [한국어] 조건 1 — 그 번호를 이미 다른 버스가 쓰고 있다.
		 * 더 잡을 수 없으므로 여기까지. */
		if (pci_find_bus(pci_domain_nr(bus), max + i + 1))
			break;

		/* [한국어] 조건 2 — 조상 브리지 중 하나의 subordinate 범위가
		 * 우리가 잡으려는 구간과 겹치는가. 겹치면 그 조상의 범위를
		 * 침범하는 것이라 허용할 수 없다.
		 * pcibios_assign_all_busses() 가 참이면 커널이 모든 번호를 새로
		 * 배정하므로 기존 범위를 신경 쓸 필요가 없어 이 검사를 건너뛴다. */
		while (parent->parent) {
			if (!pcibios_assign_all_busses() &&
			    (parent->busn_res.end > max) &&
			    (parent->busn_res.end <= max + i)) {
				j = 1;	/* [한국어] 침범 발견 */
			}
			parent = parent->parent;	/* [한국어] 한 단계 위로 */
		}
		if (j) {
			/*
			 * Often, there are two CardBus bridges -- try to
			 * leave one valid bus number for each one.
			 */
			/* [한국어] 침범이 있으면 욕심을 반으로 줄인다. 위 영어 주석이
			 * 이유를 밝힌다 — 노트북에 CardBus 슬롯이 둘인 경우가 많고,
			 * 한쪽이 다 가져가면 다른 쪽이 굶는다. 절반씩 나눠 갖게 한다. */
			i /= 2;
			break;
		}
	}
	max += i;	/* [한국어] 확보한 여유만큼 max 를 밀어 둔다 */

	/*
	 * Set subordinate bus number to its real value. If fixed
	 * subordinate bus number exists from EA capability then use it.
	 */
	/* [한국어] EA 로 못박힌 번호가 있으면 우리가 계산한 것을 버리고
	 * 그 값을 쓴다. 하드웨어가 정한 것이라 선택의 여지가 없다. */
	if (fixed_buses)
		max = fixed_sub;
	/* [한국어] 아까 부모 끝까지로 넓게 잡아 두었던 이 버스의 범위를
	 * 실제 값으로 좁힌다. */
	pci_bus_update_busn_res_end(child, max);
	/* [한국어] 하드웨어의 subordinate 레지스터도 같은 값으로 갱신한다.
	 * 여기는 한 바이트만 바뀌므로 dword 통짜 쓰기가 필요 없다 —
	 * 위의 세 값 동시 갱신과 달리 중간 상태가 생기지 않기 때문이다. */
	pci_write_config_byte(dev, PCI_SUBORDINATE_BUS, max);

	/* [한국어] 버스 이름을 만든다. lspci 나 sysfs 에 보이는 문자열이다.
	 * scnprintf 는 잘린 경우에도 실제로 쓴 길이를 돌려주어 버퍼 넘침이 없다. */
	scnprintf(child->name, sizeof(child->name), "PCI CardBus %04x:%02x",
		  pci_domain_nr(bus), child->number);

	/* [한국어] 배정한 범위가 부모 범위 안에 들어가는지 등을 검사하고,
	 * 어긋나면 경고를 남긴다. 잘못된 범위는 나중에 라우팅 오류로 나타나므로
	 * 여기서 잡아 두는 편이 낫다. */
	pbus_validate_busn(child);

	return max;	/* [한국어] 갱신된 최대 버스 번호를 호출자에게 돌려준다 */
}
