// SPDX-License-Identifier: GPL-2.0+
/*
 * IBM Hot Plug Controller Driver
 *
 * Written By: Irene Zubarev, IBM Corporation
 *
 * Copyright (C) 2001 Greg Kroah-Hartman (greg@kroah.com)
 * Copyright (C) 2001,2002 IBM Corp.
 *
 * All rights reserved.
 *
 * Send feedback to <gregkh@us.ibm.com>
 *
 */

/*
 * [한국어 설명] IBM 핫플러그 컨트롤러의 PCI 설정기 (ibmphp_pci.c)
 *
 * === 파일의 역할 ===
 * 카드가 슬롯에 꽂혔을 때 그 카드의 **PCI config 공간을 직접 채우는** 파일이다.
 * BAR 하나하나에 주소를 정해 써 넣고, 브리지면 그 뒤 버스까지 재귀로 훑어
 * 구성한다. 뽑을 때는 그 반대로 되돌린다.
 *
 * 짝이 되는 ibmphp_res.c 가 **장부**라면 이 파일은 **집행자**다. 자리를
 * 고르는 일은 ibmphp_check_resource() 에 맡기고, 여기서는 그 결과를
 * pci_bus_write_config_ 계열로 하드웨어에 적은 뒤 ibmphp_add_resource() 로
 * 장부에 올린다.
 *
 * 이 파일을 낯설게 만드는 것은 **PCI 코어의 열거기를 쓰지 않는다** 는 점이다.
 * 커널의 일반 경로는 drivers/pci/probe.c 의 pci_scan_child_bus() 계열이
 * 버스를 훑어 struct pci_dev 를 만들고, drivers/pci/setup-bus.c 가 BAR 에
 * 주소를 배정한다. 이 드라이버는 그 사슬을 타지 않고 config 공간을 직접
 * 읽고 쓰며 자기 struct pci_func 트리를 만든다. 아래 "왜 코어를 다시
 * 만들었는가" 절이 그 배경을 다룬다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 드라이버는 파일 넷이 한 벌로 움직인다(ibmphp_res.c 와 이 파일 외에는
 * 아직 한국어 주석이 없다).
 *   ibmphp_ebda.c — BIOS 의 EBDA 표를 읽어 슬롯·버스·자원 목록을 만든다.
 *   ibmphp_hpc.c  — 핫플러그 컨트롤러 하드웨어와 대화한다(전원·LED·상태).
 *   ibmphp_core.c — 슬롯 등록과 enable/disable 흐름. 카드가 꽂히면 이 파일의
 *                   ibmphp_configure_card() 를, 뽑히면
 *                   ibmphp_unconfigure_card() 를 부른다.
 *   ibmphp_res.c  — 주소 자원 장부. 이 파일이 그것을 읽고 쓴다.
 *
 * 삽입 흐름은 이렇다.
 *   ibmphp_core.c
 *     -> ibmphp_configure_card(func, slotno)
 *          [함수 0~7 을 훑으며 헤더 타입으로 갈린다]
 *          일반 장치  -> configure_device()
 *                          BAR 마다: 길이 탐침 -> ibmphp_check_resource()
 *                          -> ibmphp_add_resource() -> BAR 에 주소 쓰기
 *          브리지     -> configure_bridge()
 *                          -> scan_behind_bridge()  그 뒤 장치들이 필요한
 *                             자원의 총량을 먼저 재고
 *                          -> find_sec_number()     2차 버스 번호를 고르고
 *                          -> ibmphp_check_resource(..., 1) 로 창을 잡은 뒤
 *                          -> add_new_bus()         장부에 2차 버스를 만들고
 *                          -> 브리지 레지스터에 창을 써 넣는다
 *                        그다음 **자기 자신을 재귀 호출**해 그 뒤 장치를 구성한다
 *
 * 제거 흐름은 대칭이다.
 *   ibmphp_core.c -> ibmphp_unconfigure_card()
 *     -> (부팅 때부터 있던 카드이면) unconfigure_boot_card()
 *          -> unconfigure_boot_device() / unconfigure_boot_bridge()
 *     -> (이 드라이버가 꽂아 넣은 카드이면) func 목록을 따라
 *        ibmphp_remove_resource() 로 장부에서 지운다
 *
 * 실행 컨텍스트: 전부 프로세스 컨텍스트다. 슬롯 enable/disable 경로에서만
 * 불린다. 이 파일에도 락이 없다 — 직렬화는 상위(ibmphp_core.c)가 맡는
 * 것으로 보이나, 그 보장의 근거는 이 트리에서 확인 못 함.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: ibmphp_core.c 가 ibmphp_configure_card() 와
 *   ibmphp_unconfigure_card() 를 부른다. 선언은 ibmphp.h 에 있다.
 * 옆쪽: **ibmphp_res.c 의 장부 API 를 거의 전부 쓴다** —
 *   ibmphp_check_resource(자리 고르기), ibmphp_add_resource(등록),
 *   ibmphp_remove_resource(해제), ibmphp_find_resource(되찾기),
 *   ibmphp_find_res_bus(버스 조회), ibmphp_add_pfmem_from_mem(MEM 에서 떼어
 *   쓴 PFMEM 등록), ibmphp_remove_bus(브리지 뒤 버스 통째 제거).
 *   그 둘이 이 드라이버의 자원 관리 전부다.
 * 아래쪽: PCI config 접근(pci_bus_read_config_ 계열 / pci_bus_write_config_
 *   계열)만 쓴다. 그때 쓰는 버스가 전역 ibmphp_pci_bus 인데, 실제로 열거된
 *   struct pci_bus 가 아니라 **번호만 갈아 끼우며 재사용하는 껍데기**다
 *   (ibmphp_ebda.c 가 만든다). 이 파일의 거의 모든 함수가 config 접근 직전에
 *   ibmphp_pci_bus->number 를 바꾸는 것이 그 때문이다.
 * 공유 상태: 이 파일에는 전역이 없다. struct pci_func 는 ibmphp.h 에
 *   정의되어 ibmphp_core.c 및 ibmphp_ebda.c 와 공유한다.
 *
 * === 주요 함수/구조체 요약 ===
 * ibmphp_configure_card()   : 삽입 진입점. 함수 0~7 을 훑고 헤더 타입으로 갈린다.
 * configure_device()        : 일반 장치의 BAR 여섯을 채운다.
 * configure_bridge()        : 브리지의 창 셋을 잡고 2차 버스를 만든다.
 * scan_behind_bridge()      : 브리지 뒤 장치들이 필요한 자원 총량을 미리 잰다.
 * find_sec_number()         : 쓰이지 않는 2차 버스 번호를 고른다.
 * add_new_bus()             : 장부(ibmphp_res.c)에 2차 버스와 창을 만든다.
 * ibmphp_unconfigure_card() : 제거 진입점.
 * unconfigure_boot_device() : 부팅 때부터 있던 장치의 자원을 장부에서 지운다.
 * unconfigure_boot_bridge() : 같은 일을 브리지에 대해 하고 버스도 지운다.
 * assign_alt_irq()          : BIOS 가 라우팅을 안 주면 종류별 기본 IRQ 를 넣는다.
 * struct pci_func           : 이 드라이버가 만드는 장치 하나의 표현(ibmphp.h).
 * struct res_needed         : 브리지 뒤 자원 총량을 담는 임시 집계 구조체.
 *
 * struct res_needed 는 ibmphp.h:359 에 정의되어 있어 그 파일에는 손대지
 * 않고 여기에 필드별로 적는다. scan_behind_bridge() 가 kzalloc 으로 만들어
 * 채우고, configure_bridge() 가 읽은 뒤 kfree 로 버린다 — 즉 한 번의
 * 카드 삽입 처리 안에서만 살아 있는 값이며 다른 스레드가 보지 않으므로
 * 별도 동기화가 없다.
 *   u32 mem;
 *     브리지 뒤 장치들의 일반 메모리 BAR 길이를 단순히 더한 값.
 *     설정자: scan_behind_bridge() 가 BAR 를 탐침해 누적한다.
 *     읽는 자: configure_bridge() 가 브리지 메모리 창의 크기로 쓴다.
 *     값 범위: 0(뒤쪽이 메모리를 안 씀) 또는 MEMBRIDGE(1MB) 이상.
 *     0 이 아니면서 MEMBRIDGE 보다 작으면 scan_behind_bridge() 마지막에
 *     MEMBRIDGE 로 올린다. 정렬로 생기는 틈은 더하지 않는다.
 *   u32 pfmem;
 *     같은 방식으로 더한 프리페치 메모리 총량.
 *     설정자/읽는 자와 하한 규칙은 mem 과 같다.
 *     읽는 자 쪽에서 프리페치 창을 못 잡으면 MEM 창에서 떼어 쓰는
 *     경로(ibmphp_add_pfmem_from_mem())로 넘어간다.
 *   u32 io;
 *     같은 방식으로 더한 I/O 총량.
 *     하한은 IOBRIDGE(4KB)다 — 브리지 I/O 창의 최소 단위이기 때문이다.
 *   u8 not_correct;
 *     이 구성을 그대로 쓸 수 없다는 표시. 상류 주석은 "needed for return"
 *     이라고만 적혀 있다.
 *     설정자: scan_behind_bridge() 가 세 경우에 1 로 세운다 — 뒤에 또
 *     브리지가 있을 때, VGA(호환 또는 디스플레이) 장치가 있을 때,
 *     그리고 장치를 하나도 못 찾았을 때.
 *     읽는 자: configure_bridge() 가 1 이면 이미 잡아 둔 브리지 BAR 자원을
 *     모두 되돌리고 -ENODEV 로 나간다.
 *     값 범위: 0(정상) 또는 1(부적합).
 *   int devices[32];
 *     2차 버스의 장치 번호 32개 중 어느 자리에 함수가 실재하는지를 담는
 *     비트맵 역할의 배열(값은 0 또는 1).
 *     설정자: scan_behind_bridge() 가 벤더 ID 응답이 있는 자리마다 1 로
 *     둔다. 배열 자체는 kzalloc 으로 0 이지만 루프 앞에서 한 번 더 0 을
 *     넣는다.
 *     읽는 자: configure_bridge() 가 func->devices[] 로 베끼고, 그것을
 *     ibmphp_configure_card() 가 보고 뒤쪽 장치를 재귀로 설정한다.
 *     값 범위: 인덱스 0~31 = PCI 장치 번호. 함수 번호는 담지 않으므로
 *     뒤쪽 다중 함수 장치의 함수별 구분은 이 배열로 할 수 없다.
 *
 * === 왜 PCI 코어의 열거기를 다시 만들었는가 ===
 * 코드에서 근거를 읽을 수 있는 것만 적는다.
 *   - **struct pci_dev 를 만들지 않는다.** 이 파일이 다루는 단위는
 *     struct pci_func 이며, 그것은 EBDA/IRQ 라우팅 표에서 온 정보와
 *     이 드라이버가 배정한 자원 포인터(io[6]/mem[6]/pfmem[6])를 담는
 *     자체 구조체다. 코어의 열거기를 쓰면 struct pci_dev 가 만들어지는데,
 *     이 드라이버의 자원 장부는 그것과 연결되어 있지 않다.
 *   - **버스 번호를 스스로 고른다.** find_sec_number() 가 EBDA 장부를 보고
 *     빈 번호를 고르는데, 코어의 열거는 그 장부를 모른다.
 *   - **자원을 ibmphp_res.c 의 장부에서 받는다.** 코어의 배정기를 쓰면
 *     그 장부와 어긋난다.
 *   - 그 결과로 config 공간 접근이 전부 pci_bus_read_config_ 계열의
 *     저수준 호출이 되고, 껍데기 버스의 번호를 매번 갈아 끼우는 방식이
 *     생겼다.
 * 다만 "당시 코어가 핫플러그 열거를 제공하지 않아 이렇게 했다" 는 시기적
 * 판단의 1차 근거(설계 문서나 커밋 기록)는 이 트리에서 확인 못 함.
 *
 * === 이 파일에서 눈에 띄는 제약들 ===
 * 상류 주석과 코드가 스스로 밝히는 한계다.
 *   - VGA 호환 장치는 핫플러그를 지원하지 않는다고 거절한다.
 *   - 브리지 뒤에 또 브리지가 있는 구성(중첩 브리지)을 지원하지 않는다.
 *   - 64비트 BAR 의 상위 워드에는 0 을 쓴다. 상류 주석대로 다룰 수 없기
 *     때문이다.
 *   - configure_bridge() 의 상류 주석은 브리지 뒤 자원이 부족할 때
 *     "다른 창을 다시 잡아 본다" 는 재시도를 TO DO 로 남겨 두었다.
 *
 * === NVMe 관점 ===
 * drivers/nvme/ 에서 이 파일의 심볼을 쓰는 곳은 없다(전수 grep 확인).
 * 시기적으로도 접점이 없다 — 이 코드는 2001~2002년 IBM 서버용이고 NVMe
 * 규격은 2011년에 나왔다. 다만 이 파일이 하는 일은 NVMe SSD 를 핫플러그로
 * 꽂았을 때 오늘날 PCI 코어가 하는 일과 정확히 같다 — BAR 0 에 컨트롤러
 * 레지스터 창을 잡아 주고, 명령 레지스터를 켜고, 인터럽트 선을 적는 것이다.
 * 이 파일의 configure_device() 마지막 네 줄(캐시 라인, 레이턴시 타이머,
 * ROM 주소, 명령 레지스터)이 그 마무리에 해당한다.
 */

#include <linux/module.h> /* [한국어] MODULE_ 계열 정의. 이 파일은 직접 쓰지 않지만 드라이버 한 벌이 모듈로 빌드되기 위해 필요하다 */
#include <linux/slab.h> /* [한국어] kzalloc_obj/kfree — pci_func 와 자원·창 노드를 잡고 놓는다 */
#include <linux/pci.h> /* [한국어] PCI 코어 API 와 PCI_BASE_ADDRESS_ / PCI_HEADER_TYPE_ 계열 스펙 상수, 그리고 config 접근 함수들 */
#include <linux/list.h> /* [한국어] list_add — add_new_bus() 가 버스를 장부 목록에 매달 때 쓴다 */
#include "ibmphp.h" /* [한국어] 이 드라이버 한 벌이 공유하는 헤더. struct pci_func / bus_node / resource_node / res_needed 와 ibmphp_res.c 의 API 선언이 여기 있다 */


static int configure_device(struct pci_func *); /* [한국어] 아래 ibmphp_configure_card() 가 먼저 부르므로 전방 선언이 필요하다 */
static int configure_bridge(struct pci_func **, u8); /* [한국어] 같은 이유 */
static struct res_needed *scan_behind_bridge(struct pci_func *, u8); /* [한국어] configure_bridge() 가 부른다 */
static int add_new_bus(struct bus_node *, struct resource_node *, struct resource_node *, struct resource_node *, u8); /* [한국어] configure_bridge() 가 부른다 */
static u8 find_sec_number(u8 primary_busno, u8 slotno); /* [한국어] configure_bridge() 가 부른다 */

/*
 * NOTE..... If BIOS doesn't provide default routing, we assign:
 * 9 for SCSI, 10 for LAN adapters, and 11 for everything else.
 * If adapter is bridged, then we assign 11 to it and devices behind it.
 * We also assign the same irq numbers for multi function devices.
 * These are PIC mode, so shouldn't matter n.e.ways (hopefully)
 */
/* [한국어]
 * assign_alt_irq - BIOS 가 IRQ 라우팅을 주지 않았을 때 종류별 기본값을 넣는다
 *
 * @cur_func:   대상 함수(장치).
 * @class_code: PCI 클래스 코드의 최상위 바이트(대분류).
 *
 * 바로 위 상류 주석이 규칙을 그대로 밝힌다 — SCSI 는 9, LAN 은 10, 그 밖은
 * 11 을 준다. 브리지면 11 을 주고 그 뒤 장치들에도 같은 값을 물려주며,
 * 다기능 장치의 여러 함수에도 같은 번호를 쓴다. PIC 모드라 어차피 큰 문제가
 * 없을 것이라는 단서까지 달려 있다.
 *
 * 0xff 인 자리만 채운다는 점이 요점이다. 그 값이 "BIOS 가 라우팅을 알려
 * 주지 않았다" 는 표시이고, 이미 값이 있으면 건드리지 않는다.
 *
 * 네 자리를 도는 것은 INTA~INTD 네 핀에 각각 IRQ 가 대응하기 때문이다.
 * 호출자가 나중에 config 공간의 PCI_INTERRUPT_PIN 을 읽어 그중 하나를 골라
 * PCI_INTERRUPT_LINE 에 적는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(카드 삽입).
 *
 * 호출 체인:  ibmphp_configure_card() → [이 함수]
 */
static void assign_alt_irq(struct pci_func *cur_func, u8 class_code)
{
	int j; /* [한국어] 네 인터럽트 핀(INTA~INTD)을 도는 인덱스 */
	for (j = 0; j < 4; j++) { /* [한국어] 핀 넷을 모두 본다 */
		if (cur_func->irq[j] == 0xff) { /* [한국어] 0xff 는 BIOS 가 라우팅을 알려 주지 않았다는 표시다. 값이 있으면 건드리지 않는다 */
			switch (class_code) { /* [한국어] 상류 주석의 규칙대로 클래스 대분류로 갈린다 */
				case PCI_BASE_CLASS_STORAGE: /* [한국어] 저장 장치(SCSI 등)이면 */
					cur_func->irq[j] = SCSI_IRQ; /* [한국어] 상류 주석대로 9번 */
					break;
				case PCI_BASE_CLASS_NETWORK: /* [한국어] 네트워크 어댑터이면 */
					cur_func->irq[j] = LAN_IRQ; /* [한국어] 상류 주석대로 10번 */
					break;
				default: /* [한국어] 그 밖은 */
					cur_func->irq[j] = OTHER_IRQ; /* [한국어] 상류 주석대로 11번. 브리지도 여기로 온다 */
					break;
			}
		}
	}
}

/*
 * Configures the device to be added (will allocate needed resources if it
 * can), the device can be a bridge or a regular pci device, can also be
 * multi-functional
 *
 * Input: function to be added
 *
 * TO DO:  The error case with Multifunction device or multi function bridge,
 * if there is an error, will need to go through all previous functions and
 * unconfigure....or can add some code into unconfigure_card....
 */
/* [한국어]
 * ibmphp_configure_card - 꽂힌 카드의 모든 함수를 훑어 config 공간을 채운다
 *
 * @func:   설정할 함수(장치). busno 와 device 는 IRQ 라우팅 표에서 왔다.
 * @slotno: 이 카드가 꽂힌 슬롯 번호. 브리지의 2차 버스 번호를 고를 때 쓴다.
 * @return: 0 성공. VGA·중첩 브리지 등 지원하지 않는 구성이면 -ENODEV,
 *          알 수 없는 헤더 타입이면 -ENXIO, 그 밖에는 하위 함수의 오류.
 *
 * **삽입 경로의 진입점**이며, 이 파일에서 PCI 코어의 열거기가 하는 일을
 * 대신하는 함수다.
 *
 * 바로 위 상류 주석대로 브리지든 일반 장치든 다기능이든 모두 다루며,
 * 다기능 장치·브리지에서 오류가 났을 때 앞선 함수들까지 되돌리는 처리는
 * TO DO 로 남아 있다.
 *
 * 함수 0~7 을 훑으며 벤더 ID 로 존재를 확인하고, 헤더 타입으로 갈린다.
 *   NORMAL      — 단일 함수 장치. configure_device() 로 BAR 를 채운 뒤
 *                 function 을 8 로 만들어 루프를 끝낸다.
 *   MULTIDEVICE — 다기능 일반 장치. 같은 처리를 하고, **다음 함수를 담을
 *                 pci_func 를 미리 만들어** 목록에 잇는다.
 *   MULTIBRIDGE — 다기능 브리지. configure_bridge() 로 창을 잡은 뒤,
 *                 2차 버스 번호를 읽어 그 뒤 장치마다 pci_func 를 만들고
 *                 **자기 자신을 재귀 호출**한다. 그다음 이 브리지의 다음
 *                 함수를 담을 노드를 하나 더 만든다.
 *   BRIDGE      — 단일 함수 브리지. 위와 같되 마지막에 function 을 8 로
 *                 만들어 루프를 끝낸다.
 *
 * VGA 장치는 두 클래스 모두 거절한다 — 상류 메시지대로 핫플러그를 지원하지
 * 않는다.
 *
 * [관찰] MULTIDEVICE 갈래에서 newfunc 의 IRQ 를 복사하는 루프가
 * `newfunc->irq[j] = cur_func->irq[j]` 인데, 그 직전에 cur_func 가 이미
 * newfunc 로 바뀌어 있다. 즉 자기 자신을 복사한다(kzalloc 이라 0). 다른
 * 갈래들은 cur_func 를 바꾸기 **전에** 복사해 값이 제대로 넘어간다.
 * 상류 코드 그대로이며 여기서는 고치지 않는다.
 *
 * 되돌리기가 error 라벨 하나뿐이며, cleanup_count 로 몇 개의 BAR 를 되돌릴지
 * 정한다 — 일반 장치는 6, 브리지는 2 다. func->bus 를 1 로 세워 두는 것은
 * 나중에 ibmphp_unconfigure_card() 가 "이것은 브리지" 임을 알아보게 하기
 * 위해서다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 브리지에서는 자기 자신을 재귀 호출한다.
 *
 * 호출 체인:  ibmphp_core.c → [이 함수]
 *               → configure_device() / configure_bridge() → (재귀) [이 함수]
 */
int ibmphp_configure_card(struct pci_func *func, u8 slotno)
{
	u16 vendor_id; /* [한국어] 벤더 ID. 그 자리에 장치가 있는지 판정한다 */
	u32 class; /* [한국어] 클래스 코드 레지스터 전체(클래스·서브클래스·프로그래밍 인터페이스·리비전) */
	u8 class_code; /* [한국어] 그중 최상위 바이트(대분류). assign_alt_irq() 에 넘긴다 */
	u8 hdr_type, device, sec_number; /* [한국어] 헤더 타입, 장치 번호, 브리지의 2차 버스 번호 */
	u8 function; /* [한국어] 지금 보고 있는 함수 번호 */
	struct pci_func *newfunc;	/* for multi devices */ /* [한국어] 다기능 장치·브리지 뒤 장치를 담을 새 노드 */
	struct pci_func *cur_func, *prev_func; /* [한국어] 지금 다루는 노드와 목록 끝을 찾을 때 쓰는 노드 */
	int rc, i, j; /* [한국어] 하위 함수 결과와 두 루프 인덱스 */
	int cleanup_count; /* [한국어] 오류 시 되돌릴 BAR 개수. 일반 장치는 6, 브리지는 2 다 */
	u8 flag; /* [한국어] 브리지 뒤 장치를 목록에 처음 잇는지 표시한다 */
	u8 valid_device = 0x00;	/* to see if we are able to read from card any device info at all */ /* [한국어] 카드에서 장치 정보를 하나라도 읽었는지 센다 */

	debug("inside configure_card, func->busno = %x\n", func->busno); /* [한국어] 어느 버스의 카드를 다루는지 남긴다 */

	device = func->device; /* [한국어] 상류 주석대로 IRQ 라우팅 표에서 온 장치 번호 */
	cur_func = func; /* [한국어] 첫 함수는 인자로 받은 노드가 담당한다 */

	/* We only get bus and device from IRQ routing table.  So at this point,
	 * func->busno is correct, and func->device contains only device (at the 5
	 * highest bits)
	 */

	/* For every function on the card */
	for (function = 0x00; function < 0x08; function++) { /* [한국어] 상류 주석대로 카드의 함수 0~7 을 모두 훑는다 */
		unsigned int devfn = PCI_DEVFN(device, function); /* [한국어] 장치·함수를 하나의 devfn 으로 합친다 */
		ibmphp_pci_bus->number = cur_func->busno; /* [한국어] **전역 껍데기 버스의 번호를 갈아 끼운다** — 실제 열거된 struct pci_bus 가 아니라 config 접근용 틀이다 */

		cur_func->function = function; /* [한국어] 이 노드가 담당할 함수 번호를 적어 둔다 */

		debug("inside the loop, cur_func->busno = %x, cur_func->device = %x, cur_func->function = %x\n",
			cur_func->busno, cur_func->device, cur_func->function); /* [한국어] 어느 자리를 보는지 남긴다 */

		pci_bus_read_config_word(ibmphp_pci_bus, devfn, PCI_VENDOR_ID, &vendor_id); /* [한국어] 벤더 ID 를 읽어 그 자리에 장치가 있는지 본다 */

		debug("vendor_id is %x\n", vendor_id); /* [한국어] 읽은 값을 남긴다 */
		if (vendor_id != PCI_VENDOR_ID_NOTVALID) { /* [한국어] 유효한 벤더 ID 이면 장치가 있다 */
			/* found correct device!!! */
			debug("found valid device, vendor_id = %x\n", vendor_id); /* [한국어] 찾았음을 남긴다 */

			++valid_device; /* [한국어] 하나라도 읽었다고 센다 — 마지막에 이 값이 0 이면 카드를 못 읽은 것이다 */

			/* header: x x x x x x x x
			 *         | |___________|=> 1=PPB bridge, 0=normal device, 2=CardBus Bridge
			 *         |_=> 0 = single function device, 1 = multi-function device
			 */

			pci_bus_read_config_byte(ibmphp_pci_bus, devfn, PCI_HEADER_TYPE, &hdr_type); /* [한국어] 헤더 타입을 읽는다. 위 상류 주석의 비트 그림이 그 의미를 설명한다 */
			pci_bus_read_config_dword(ibmphp_pci_bus, devfn, PCI_CLASS_REVISION, &class); /* [한국어] 클래스 코드 레지스터를 읽는다 */

			class_code = class >> 24; /* [한국어] 최상위 바이트가 대분류다 */
			debug("hrd_type = %x, class = %x, class_code %x\n", hdr_type, class, class_code); /* [한국어] 읽은 값들을 남긴다 */
			class >>= 8;	/* to take revision out, class = class.subclass.prog i/f */ /* [한국어] 리비전 바이트를 떼어 낸다 */
			if (class == PCI_CLASS_NOT_DEFINED_VGA) { /* [한국어] 클래스 코드가 정의되기 전의 VGA 이면 */
				err("The device %x is VGA compatible and as is not supported for hot plugging. "
				     "Please choose another device.\n", cur_func->device); /* [한국어] 상류 메시지대로 핫플러그를 지원하지 않는다 */
				return -ENODEV; /* [한국어] 거절한다 */
			} else if (class == PCI_CLASS_DISPLAY_VGA) { /* [한국어] 표준 VGA 디스플레이 컨트롤러이면 */
				err("The device %x is not supported for hot plugging. Please choose another device.\n",
				     cur_func->device); /* [한국어] 역시 지원하지 않는다 */
				return -ENODEV; /* [한국어] 거절한다 */
			}
			switch (hdr_type) { /* [한국어] 헤더 타입으로 갈린다 */
				case PCI_HEADER_TYPE_NORMAL: /* [한국어] 단일 함수 일반 장치이면 */
					debug("single device case.... vendor id = %x, hdr_type = %x, class = %x\n", vendor_id, hdr_type, class); /* [한국어] 어느 장치인지 남긴다 */
					assign_alt_irq(cur_func, class_code); /* [한국어] BIOS 가 IRQ 라우팅을 안 주었으면 기본값을 채운다 */
					rc = configure_device(cur_func); /* [한국어] BAR 여섯을 채운다 */
					if (rc < 0) { /* [한국어] 실패했으면 */
						/* We need to do this in case some other BARs were properly inserted */
						err("was not able to configure devfunc %x on bus %x.\n",
						     cur_func->device, cur_func->busno); /* [한국어] 상류 주석대로 이미 제대로 넣은 BAR 가 있을 수 있어 되돌려야 한다 */
						cleanup_count = 6; /* [한국어] 일반 장치라 BAR 여섯을 되돌린다 */
						goto error; /* [한국어] 공통 정리 경로로 간다 */
					}
					cur_func->next = NULL; /* [한국어] 단일 함수라 뒤에 이을 노드가 없다 */
					function = 0x8; /* [한국어] 함수 루프를 끝내 나머지 7개를 건너뛴다 */
					break;
				case PCI_HEADER_TYPE_MULTIDEVICE:
					assign_alt_irq(cur_func, class_code); /* [한국어] IRQ 기본값을 채운다 */
					rc = configure_device(cur_func); /* [한국어] BAR 여섯을 채운다 */
					if (rc < 0) { /* [한국어] 실패했으면 */
						/* We need to do this in case some other BARs were properly inserted */
						err("was not able to configure devfunc %x on bus %x...bailing out\n",
						     cur_func->device, cur_func->busno); /* [한국어] 상류 주석대로 이미 넣은 BAR 를 되돌려야 한다 */
						cleanup_count = 6; /* [한국어] 일반 장치라 6 개 */
						goto error; /* [한국어] 정리 경로로 */
					}
					newfunc = kzalloc_obj(*newfunc); /* [한국어] **다음 함수를 담을 노드를 미리 만든다** — 다기능 장치라 함수가 더 있을 수 있다 */
					if (!newfunc) /* [한국어] 메모리가 없으면 */
						return -ENOMEM; /* [한국어] 그대로 돌아간다 */

					newfunc->busno = cur_func->busno; /* [한국어] 같은 버스 */
					newfunc->device = device; /* [한국어] 같은 장치 번호. 함수 번호는 다음 반복이 채운다 */
					cur_func->next = newfunc; /* [한국어] 목록에 잇고 */
					cur_func = newfunc; /* [한국어] 다음 반복이 이 노드를 다루게 한다 */
					for (j = 0; j < 4; j++) /* [한국어] IRQ 넷을 물려준다 */
						newfunc->irq[j] = cur_func->irq[j]; /* [한국어] [관찰] 이 시점에는 cur_func 가 이미 newfunc 라 자기 자신을 복사한다(kzalloc 이라 0). 다른 갈래들은 cur_func 를 바꾸기 전에 복사한다. 상류 코드 그대로다 */
					break;
				case PCI_HEADER_TYPE_MULTIBRIDGE:
					class >>= 8; /* [한국어] 서브클래스까지 보려고 한 번 더 민다 */
					if (class != PCI_CLASS_BRIDGE_PCI) { /* [한국어] PCI-to-PCI 브리지가 아니면 */
						err("This %x is not PCI-to-PCI bridge, and as is not supported for hot-plugging.  Please insert another card.\n",
						     cur_func->device); /* [한국어] CardBus 등은 지원하지 않는다 */
						return -ENODEV; /* [한국어] 거절한다 */
					}
					assign_alt_irq(cur_func, class_code); /* [한국어] IRQ 기본값을 채운다 */
					rc = configure_bridge(&cur_func, slotno); /* [한국어] 창 셋을 잡고 2차 버스를 만든다 */
					if (rc == -ENODEV) { /* [한국어] 중첩 브리지이거나 뒤에 장치가 없으면 */
						err("You chose to insert Single Bridge, or nested bridges, this is not supported...\n");
						err("Bus %x, devfunc %x\n", cur_func->busno, cur_func->device); /* [한국어] 상류 메시지대로 지원하지 않는 구성이다 */
						return rc; /* [한국어] 되돌릴 것 없이 그대로 올린다 */
					}
					if (rc) { /* [한국어] 그 밖의 실패면 */
						/* We need to do this in case some other BARs were properly inserted */
						err("was not able to hot-add PPB properly.\n"); /* [한국어] 무엇이 실패했는지 알린다 */
						func->bus = 1;	/* To indicate to the unconfigure function that this is a PPB */ /* [한국어] 이 카드가 브리지임을 표시한다. 나중에 ibmphp_unconfigure_card() 가 BAR 개수를 2 로 세는 근거다 */
						cleanup_count = 2; /* [한국어] 브리지라 BAR 2 개만 되돌린다 */
						goto error; /* [한국어] 정리 경로로 */
					}

					pci_bus_read_config_byte(ibmphp_pci_bus, devfn, PCI_SECONDARY_BUS, &sec_number); /* [한국어] configure_bridge() 가 정한 2차 버스 번호를 되읽는다 */
					flag = 0; /* [한국어] 아직 뒤 장치를 하나도 잇지 않았다 */
					for (i = 0; i < 32; i++) { /* [한국어] 브리지 뒤 장치 번호 32개를 훑는다 */
						if (func->devices[i]) { /* [한국어] scan_behind_bridge() 가 살아 있다고 표시한 자리이면 */
							newfunc = kzalloc_obj(*newfunc); /* [한국어] 그 장치를 담을 노드를 만든다 */
							if (!newfunc) /* [한국어] 메모리가 없으면 */
								return -ENOMEM; /* [한국어] 그대로 돌아간다 */

							newfunc->busno = sec_number; /* [한국어] **2차 버스**에 속한다 */
							newfunc->device = (u8) i; /* [한국어] 그 장치 번호 */
							for (j = 0; j < 4; j++)
								newfunc->irq[j] = cur_func->irq[j]; /* [한국어] 브리지의 IRQ 를 물려준다 — 상류 주석대로 브리지 뒤 장치도 같은 번호를 쓴다 */

							if (flag) { /* [한국어] 이미 하나 이상 이었으면 */
								for (prev_func = cur_func; prev_func->next; prev_func = prev_func->next) ; /* [한국어] 목록 끝을 찾아 */
								prev_func->next = newfunc; /* [한국어] 거기에 잇는다 */
							} else /* [한국어] 첫 번째이면 */
								cur_func->next = newfunc; /* [한국어] 브리지 노드 바로 뒤에 잇는다 */

							rc = ibmphp_configure_card(newfunc, slotno); /* [한국어] **자기 자신을 재귀 호출**해 그 장치를 구성한다 */
	/* This could only happen if kmalloc failed */ /* [한국어] 상류 주석대로 kmalloc 실패에서만 일어날 수 있다 */
							if (rc) {
	/* We need to do this in case bridge itself got configured properly, but devices behind it failed */ /* [한국어] 상류 주석대로 브리지 자체는 제대로 설정되었지만 뒤 장치가 실패한 경우다 */
								func->bus = 1;	/* To indicate to the unconfigure function that this is a PPB */ /* [한국어] 이 카드가 브리지임을 표시한다. 나중에 ibmphp_unconfigure_card() 가 BAR 개수를 2 로 세는 근거다 */
								cleanup_count = 2; /* [한국어] 브리지라 2 개 */
								goto error; /* [한국어] 정리 경로로 */
							}
							flag = 1; /* [한국어] 다음부터는 목록 끝에 이어야 한다 */
						}
					}

					newfunc = kzalloc_obj(*newfunc); /* [한국어] **이 브리지의 다음 함수를 담을 노드**를 만든다 — 다기능 브리지이기 때문이다 */
					if (!newfunc) /* [한국어] 메모리가 없으면 */
						return -ENOMEM; /* [한국어] 그대로 돌아간다 */

					newfunc->busno = cur_func->busno; /* [한국어] 브리지와 같은 버스 */
					newfunc->device = device; /* [한국어] 같은 장치 번호 */
					for (j = 0; j < 4; j++)
						newfunc->irq[j] = cur_func->irq[j]; /* [한국어] IRQ 를 물려준다 — 여기서는 cur_func 를 바꾸기 전이라 값이 제대로 넘어간다 */
					for (prev_func = cur_func; prev_func->next; prev_func = prev_func->next); /* [한국어] 목록 끝을 찾아 */
					prev_func->next = newfunc; /* [한국어] 거기에 잇는다 */
					cur_func = newfunc; /* [한국어] 다음 반복이 이 노드를 다루게 한다 */
					break; /* [한국어] 이 헤더 타입 처리를 끝낸다 */
				case PCI_HEADER_TYPE_BRIDGE: /* [한국어] 단일 함수 브리지이면. 아래가 다기능 브리지와 거의 같다 */
					class >>= 8; /* [한국어] 서브클래스까지 보려고 한 번 더 민다 */
					debug("class now is %x\n", class); /* [한국어] 무엇으로 판정했는지 남긴다 */
					if (class != PCI_CLASS_BRIDGE_PCI) { /* [한국어] PCI-to-PCI 브리지가 아니면 */
						err("This %x is not PCI-to-PCI bridge, and as is not supported for hot-plugging.  Please insert another card.\n",
						     cur_func->device); /* [한국어] CardBus 등은 지원하지 않는다 */
						return -ENODEV; /* [한국어] 거절한다 */
					}

					assign_alt_irq(cur_func, class_code); /* [한국어] IRQ 기본값을 채운다 */

					debug("cur_func->busno b4 configure_bridge is %x\n", cur_func->busno); /* [한국어] 설정 전 버스 번호를 남긴다 */
					rc = configure_bridge(&cur_func, slotno); /* [한국어] 창 셋을 잡고 2차 버스를 만든다 */
					if (rc == -ENODEV) { /* [한국어] 중첩 브리지이거나 뒤에 장치가 없으면 */
						err("You chose to insert Single Bridge, or nested bridges, this is not supported...\n");
						err("Bus %x, devfunc %x\n", cur_func->busno, cur_func->device); /* [한국어] 어느 자리인지 함께 남긴다 */
						return rc; /* [한국어] 되돌릴 것 없이 그대로 올린다 */
					}
					if (rc) { /* [한국어] 그 밖의 실패면 */
						/* We need to do this in case some other BARs were properly inserted */
						func->bus = 1;	/* To indicate to the unconfigure function that this is a PPB */ /* [한국어] 이 카드가 브리지임을 표시한다 */
						err("was not able to hot-add PPB properly.\n"); /* [한국어] 무엇이 실패했는지 알린다 */
						cleanup_count = 2; /* [한국어] 브리지라 BAR 2 개만 되돌린다 */
						goto error; /* [한국어] 정리 경로로 */
					}
					debug("cur_func->busno = %x, device = %x, function = %x\n",
						cur_func->busno, device, function); /* [한국어] 설정 뒤 상태를 남긴다 */
					pci_bus_read_config_byte(ibmphp_pci_bus, devfn, PCI_SECONDARY_BUS, &sec_number); /* [한국어] configure_bridge() 가 정한 2차 버스 번호를 되읽는다 */
					debug("after configuring bridge..., sec_number = %x\n", sec_number); /* [한국어] 고른 번호를 남긴다 */
					flag = 0; /* [한국어] 아직 뒤 장치를 하나도 잇지 않았다 */
					for (i = 0; i < 32; i++) { /* [한국어] 브리지 뒤 장치 번호 32개를 훑는다 */
						if (func->devices[i]) { /* [한국어] 살아 있다고 표시된 자리이면 */
							debug("inside for loop, device is %x\n", i); /* [한국어] 어느 장치인지 남긴다 */
							newfunc = kzalloc_obj(*newfunc); /* [한국어] 그 장치를 담을 노드를 만든다 */
							if (!newfunc) /* [한국어] 메모리가 없으면 */
								return -ENOMEM; /* [한국어] 그대로 돌아간다 */

							newfunc->busno = sec_number; /* [한국어] 2차 버스에 속한다 */
							newfunc->device = (u8) i; /* [한국어] 그 장치 번호 */
							for (j = 0; j < 4; j++)
								newfunc->irq[j] = cur_func->irq[j]; /* [한국어] 브리지의 IRQ 를 물려준다 */

							if (flag) { /* [한국어] 이미 하나 이상 이었으면 */
								for (prev_func = cur_func; prev_func->next; prev_func = prev_func->next); /* [한국어] 목록 끝을 찾아 */
								prev_func->next = newfunc; /* [한국어] 거기에 잇는다 */
							} else /* [한국어] 첫 번째이면 */
								cur_func->next = newfunc; /* [한국어] 브리지 노드 바로 뒤에 잇는다 */

							rc = ibmphp_configure_card(newfunc, slotno); /* [한국어] **자기 자신을 재귀 호출**해 그 장치를 구성한다 */

	/* Again, this case should not happen... For complete paranoia, will need to call remove_bus */ /* [한국어] 상류 주석대로 일어나서는 안 되는 경우이며, 철저히 하려면 remove_bus 를 불러야 한다고 적혀 있다 */
							if (rc) { /* [한국어] 그래도 실패하면 */
								/* We need to do this in case some other BARs were properly inserted */
								func->bus = 1;	/* To indicate to the unconfigure function that this is a PPB */ /* [한국어] 이 카드가 브리지임을 표시한다 */
								cleanup_count = 2; /* [한국어] 브리지라 2 개 */
								goto error; /* [한국어] 정리 경로로 */
							}
							flag = 1; /* [한국어] 다음부터는 목록 끝에 이어야 한다 */
						}
					}

					function = 0x8; /* [한국어] 단일 함수 브리지라 함수 루프를 끝낸다 */
					break; /* [한국어] 이 헤더 타입 처리를 끝낸다 */
				default: /* [한국어] 셋 중 어느 것도 아니면 */
					err("MAJOR PROBLEM!!!!, header type not supported? %x\n", hdr_type); /* [한국어] 읽을 수 없는 헤더 타입이다 */
					return -ENXIO; /* [한국어] 거절한다 */
			}	/* end of switch */
		}	/* end of valid device */
	}	/* end of for */

	if (!valid_device) { /* [한국어] 카드에서 장치를 하나도 못 읽었으면 */
		err("Cannot find any valid devices on the card.  Or unable to read from card.\n");
		return -ENODEV; /* [한국어] 이미 뽑혔거나 전원이 꺼진 상황이다 */
	}

	return 0; /* [한국어] 카드 전체 설정 완료 */

error: /* [한국어] 위 세 갈래의 실패가 모두 여기로 온다 */
	for (i = 0; i < cleanup_count; i++) { /* [한국어] 되돌릴 BAR 개수만큼 — 일반 장치는 6, 브리지는 2 다 */
		if (cur_func->io[i]) { /* [한국어] I/O 자원이 잡혀 있으면 */
			ibmphp_remove_resource(cur_func->io[i]); /* [한국어] 장부에서 지우고 */
			cur_func->io[i] = NULL; /* [한국어] 포인터를 끊는다 */
		} else if (cur_func->pfmem[i]) { /* [한국어] 프리페치 메모리이면 */
			ibmphp_remove_resource(cur_func->pfmem[i]); /* [한국어] 장부에서 지우고 */
			cur_func->pfmem[i] = NULL; /* [한국어] 포인터를 끊는다 */
		} else if (cur_func->mem[i]) { /* [한국어] 메모리이면 */
			ibmphp_remove_resource(cur_func->mem[i]); /* [한국어] 장부에서 지우고 */
			cur_func->mem[i] = NULL; /* [한국어] 포인터를 끊는다. [관찰] else-if 사슬이라 같은 인덱스에 두 종류가 잡혀 있으면 하나만 지워진다 */
		}
	}
	return rc; /* [한국어] 원래 실패 원인을 올린다 */
}

/*
 * This function configures the pci BARs of a single device.
 * Input: pointer to the pci_func
 * Output: configured PCI, 0, or error
 */
/* [한국어]
 * configure_device - 일반 장치의 BAR 여섯에 주소를 정해 써 넣는다
 *
 * @func: 설정할 함수(장치).
 * @return: 0 성공, -ENOMEM 은 할당 실패, -EIO 는 자리를 못 찾은 경우.
 *
 * 바로 위 상류 주석대로 장치 하나의 PCI BAR 를 설정한다. **PCI 코어의
 * pci_assign_resource() 가 하는 일을 이 드라이버가 직접 하는 자리**다.
 *
 * BAR 마다 같은 절차를 밟는다.
 *   1) **길이 탐침** — BAR 에 0xFFFFFFFF 를 쓰고 되읽는다. 하드웨어가 자기가
 *      디코딩하지 않는 하위 비트를 0 으로 돌려주므로, 그 값을 뒤집고 1 을
 *      더하면 요구 크기가 나온다(`~len + 1`). 되읽은 값이 0 이면 그 BAR 는
 *      구현되지 않은 것이다.
 *   2) 종류를 가른다 — 최하위 비트가 I/O, 아니면 메모리이고 그중 프리페치
 *      비트가 있으면 PFMEM 이다. 마스크가 갈리는데(I/O 는 0xFFFFFFFC,
 *      메모리는 0xFFFFFFF0) BAR 의 플래그 비트 수가 다르기 때문이다.
 *   3) 자원 노드를 만들어 **ibmphp_check_resource() 로 자리를 얻고**
 *      ibmphp_add_resource() 로 장부에 올린다.
 *   4) 고른 시작 주소를 BAR 에 써 넣는다.
 *
 * PFMEM 이 특별하다. 프리페치 창에서 자리를 못 찾으면 **일반 MEM 창에서
 * 떼어 쓴다** — 임시 MEM 노드로 자리를 얻은 뒤 그 결과를 PFMEM 노드에
 * 베끼고 fromMem 을 1 로 세워 ibmphp_add_pfmem_from_mem() 으로 곁가지
 * 목록에 매단다. ibmphp_res.c 의 once_over() 가 부팅 때 하는 일과 같은
 * 처리다.
 *
 * 64비트 BAR 는 다음 워드까지 차지하므로 count 를 하나 더 밀고, 상류 주석대로
 * 그 상위 워드에는 0 을 쓴다 — 이 드라이버가 4GB 위를 다루지 못하기 때문이다.
 *
 * 마지막 다섯 줄이 마무리다. func->bus 를 0 으로 두어 브리지가 아님을
 * 표시하고, 인터럽트 핀을 읽어 대응하는 IRQ 를 라인 레지스터에 적고,
 * 캐시 라인 크기와 레이턴시 타이머를 정하고, 확장 ROM 을 끄고, 마지막에
 * 명령 레지스터를 켜 장치를 활성화한다.
 *
 * [관찰] 오류 경로에서 앞서 등록한 BAR 들을 되돌리지 않는다. 그 정리는
 * 호출자(ibmphp_configure_card)의 error 라벨이 cleanup_count 만큼 맡는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(카드 삽입).
 *
 * 호출 체인:  ibmphp_configure_card() → [이 함수]
 *               → ibmphp_check_resource() → ibmphp_add_resource()
 *               → ibmphp_add_pfmem_from_mem()
 */
static int configure_device(struct pci_func *func)
{
	u32 bar[6]; /* [한국어] BAR 여섯의 탐침 결과 */
	static const u32 address[] = { /* [한국어] BAR 여섯의 config 공간 오프셋. 마지막 0 이 루프의 끝 표시다 */
		PCI_BASE_ADDRESS_0, /* [한국어] BAR 0 */
		PCI_BASE_ADDRESS_1, /* [한국어] BAR 1 */
		PCI_BASE_ADDRESS_2, /* [한국어] BAR 2 */
		PCI_BASE_ADDRESS_3, /* [한국어] BAR 3 */
		PCI_BASE_ADDRESS_4, /* [한국어] BAR 4 */
		PCI_BASE_ADDRESS_5, /* [한국어] BAR 5 */
		0 /* [한국어] 끝 표시 */
	};
	u8 irq; /* [한국어] 인터럽트 핀 번호를 읽어 담을 자리 */
	int count; /* [한국어] BAR 루프 인덱스 */
	int len[6]; /* [한국어] BAR 마다의 요구 길이 */
	struct resource_node *io[6]; /* [한국어] I/O 자원 노드 배열 */
	struct resource_node *mem[6]; /* [한국어] 메모리 자원 노드 배열 */
	struct resource_node *mem_tmp; /* [한국어] PFMEM 을 MEM 창에서 떼어 쓸 때 만드는 임시 노드 */
	struct resource_node *pfmem[6]; /* [한국어] 프리페치 메모리 자원 노드 배열 */
	unsigned int devfn; /* [한국어] config 접근에 쓸 장치·함수 번호 */

	debug("%s - inside\n", __func__);

	devfn = PCI_DEVFN(func->device, func->function); /* [한국어] 장치·함수를 devfn 으로 합친다 */
	ibmphp_pci_bus->number = func->busno; /* [한국어] 전역 껍데기 버스의 번호를 이 장치의 버스로 갈아 끼운다 */

	for (count = 0; address[count]; count++) {	/* for 6 BARs */ /* [한국어] BAR 여섯을 차례로 본다 */

		/* not sure if i need this.  per scott, said maybe need * something like this
		   if devices don't adhere 100% to the spec, so don't want to write
		   to the reserved bits

		pcibios_read_config_byte(cur_func->busno, cur_func->device,
		PCI_BASE_ADDRESS_0 + 4 * count, &tmp);
		if (tmp & 0x01) // IO
			pcibios_write_config_dword(cur_func->busno, cur_func->device,
			PCI_BASE_ADDRESS_0 + 4 * count, 0xFFFFFFFD);
		else  // Memory
			pcibios_write_config_dword(cur_func->busno, cur_func->device,
			PCI_BASE_ADDRESS_0 + 4 * count, 0xFFFFFFFF);
		 */
		pci_bus_write_config_dword(ibmphp_pci_bus, devfn, address[count], 0xFFFFFFFF); /* [한국어] 길이 탐침 — 모든 비트를 1 로 써 본다 */
		pci_bus_read_config_dword(ibmphp_pci_bus, devfn, address[count], &bar[count]); /* [한국어] 되읽으면 하드웨어가 디코딩하지 않는 하위 비트가 0 으로 남아, 요구 크기를 알 수 있다 */

		if (!bar[count])	/* This BAR is not implemented */ /* [한국어] 되읽은 값이 0 이면 그 BAR 는 구현되지 않았다 */
			continue; /* [한국어] 다음 BAR 로 */

		debug("Device %x BAR %d wants %x\n", func->device, count, bar[count]); /* [한국어] 무엇을 요구하는지 남긴다 */

		if (bar[count] & PCI_BASE_ADDRESS_SPACE_IO) { /* [한국어] 최하위 비트가 서 있으면 I/O 공간이다 */
			/* This is IO */
			debug("inside IO SPACE\n"); /* [한국어] I/O 갈래로 들어왔음을 남긴다 */

			len[count] = bar[count] & 0xFFFFFFFC; /* [한국어] I/O BAR 는 하위 2비트가 플래그라 그것을 지운다 */
			len[count] = ~len[count] + 1; /* [한국어] 뒤집고 1 을 더하면 요구 크기가 된다 */

			debug("len[count] in IO %x, count %d\n", len[count], count); /* [한국어] 구한 길이를 남긴다 */

			io[count] = kzalloc_obj(struct resource_node); /* [한국어] 자원 노드를 잡는다 */

			if (!io[count]) /* [한국어] 메모리가 없으면 */
				return -ENOMEM; /* [한국어] 그대로 돌아간다 */

			io[count]->type = IO; /* [한국어] 종류는 I/O */
			io[count]->busno = func->busno; /* [한국어] 이 장치의 버스 */
			io[count]->devfunc = PCI_DEVFN(func->device, func->function); /* [한국어] 이 장치·함수 */
			io[count]->len = len[count]; /* [한국어] 요구 길이 */
			if (ibmphp_check_resource(io[count], 0) == 0) { /* [한국어] **장부에서 자리를 고른다**. 두 번째 인자 0 은 일반 장치라는 뜻이다 */
				ibmphp_add_resource(io[count]); /* [한국어] 고른 자리를 장부에 등록하고 */
				func->io[count] = io[count]; /* [한국어] 함수 구조체에도 매달아 둔다 — 나중에 제거할 때 이 포인터로 지운다 */
			} else { /* [한국어] 자리를 못 찾으면 */
				err("cannot allocate requested io for bus %x device %x function %x len %x\n",
				     func->busno, func->device, func->function, len[count]); /* [한국어] 무엇이 안 되었는지 알리고 */
				kfree(io[count]); /* [한국어] 만든 노드를 버린 뒤 */
				return -EIO; /* [한국어] 그대로 돌아간다 */
			}
			pci_bus_write_config_dword(ibmphp_pci_bus, devfn, address[count], func->io[count]->start); /* [한국어] **고른 시작 주소를 BAR 에 써 넣는다** — 이것이 이 함수의 핵심 동작이다 */

			/* _______________This is for debugging purposes only_____________________ */
			debug("b4 writing, the IO address is %x\n", func->io[count]->start); /* [한국어] 쓰기 전 값을 남긴다 */
			pci_bus_read_config_dword(ibmphp_pci_bus, devfn, address[count], &bar[count]); /* [한국어] 되읽어 */
			debug("after writing.... the start address is %x\n", bar[count]); /* [한국어] 실제로 반영되었는지 확인한다 */
			/* _________________________________________________________________________*/

		} else { /* [한국어] 최하위 비트가 0 이면 메모리 공간이다 */
			/* This is Memory */
			if (bar[count] & PCI_BASE_ADDRESS_MEM_PREFETCH) { /* [한국어] 프리페치 비트가 서 있으면 PFMEM 이다 */
				/* pfmem */
				debug("PFMEM SPACE\n"); /* [한국어] PFMEM 갈래임을 남긴다 */

				len[count] = bar[count] & 0xFFFFFFF0; /* [한국어] 메모리 BAR 는 하위 4비트가 플래그라 그것을 지운다 */
				len[count] = ~len[count] + 1; /* [한국어] 뒤집고 1 을 더해 요구 크기를 구한다 */

				debug("len[count] in PFMEM %x, count %d\n", len[count], count); /* [한국어] 구한 길이를 남긴다 */

				pfmem[count] = kzalloc_obj(struct resource_node); /* [한국어] 자원 노드를 잡는다 */
				if (!pfmem[count]) /* [한국어] 메모리가 없으면 */
					return -ENOMEM; /* [한국어] 그대로 돌아간다 */

				pfmem[count]->type = PFMEM; /* [한국어] 종류는 프리페치 메모리 */
				pfmem[count]->busno = func->busno; /* [한국어] 이 장치의 버스 */
				pfmem[count]->devfunc = PCI_DEVFN(func->device,
							func->function); /* [한국어] 이 장치·함수 */
				pfmem[count]->len = len[count]; /* [한국어] 요구 길이 */
				pfmem[count]->fromMem = 0; /* [한국어] 아직 MEM 에서 떼어 쓴 것이 아니다 */
				if (ibmphp_check_resource(pfmem[count], 0) == 0) { /* [한국어] 프리페치 창에서 자리를 고른다 */
					ibmphp_add_resource(pfmem[count]); /* [한국어] 찾았으면 장부에 등록하고 */
					func->pfmem[count] = pfmem[count]; /* [한국어] 함수 구조체에 매단다 */
				} else { /* [한국어] 못 찾으면 **일반 MEM 창에서 떼어 쓴다** */
					mem_tmp = kzalloc_obj(*mem_tmp); /* [한국어] 임시 MEM 노드를 만든다 */
					if (!mem_tmp) { /* [한국어] 메모리가 없으면 */
						kfree(pfmem[count]); /* [한국어] PFMEM 노드도 버리고 */
						return -ENOMEM; /* [한국어] 그대로 돌아간다 */
					}
					mem_tmp->type = MEM; /* [한국어] 종류만 메모리로 바꾸고 */
					mem_tmp->busno = pfmem[count]->busno; /* [한국어] 같은 버스 */
					mem_tmp->devfunc = pfmem[count]->devfunc; /* [한국어] 같은 장치·함수 */
					mem_tmp->len = pfmem[count]->len; /* [한국어] 같은 길이로 자리를 찾는다 */
					debug("there's no pfmem... going into mem.\n"); /* [한국어] 무엇을 하는지 남긴다 */
					if (ibmphp_check_resource(mem_tmp, 0) == 0) { /* [한국어] 메모리 창에서 자리를 고른다 */
						ibmphp_add_resource(mem_tmp); /* [한국어] 찾았으면 **MEM 으로** 장부에 등록한다 — 그래야 기존 코드가 이 구간을 쓰이는 것으로 본다 */
						pfmem[count]->fromMem = 1; /* [한국어] 출처가 MEM 임을 표시하고 */
						pfmem[count]->rangeno = mem_tmp->rangeno; /* [한국어] 창 번호와 */
						pfmem[count]->start = mem_tmp->start; /* [한국어] 시작과 */
						pfmem[count]->end = mem_tmp->end; /* [한국어] 끝을 그 MEM 노드에서 베낀다 */
						ibmphp_add_pfmem_from_mem(pfmem[count]); /* [한국어] 곁가지 목록(firstPFMemFromMem)에 매단다 */
						func->pfmem[count] = pfmem[count]; /* [한국어] 함수 구조체에는 PFMEM 노드를 매단다 */
					} else { /* [한국어] 거기서도 못 찾으면 */
						err("cannot allocate requested pfmem for bus %x, device %x, len %x\n",
						     func->busno, func->device, len[count]); /* [한국어] 알리고 */
						kfree(mem_tmp); /* [한국어] 임시 노드와 */
						kfree(pfmem[count]); /* [한국어] PFMEM 노드를 모두 버린 뒤 */
						return -EIO; /* [한국어] 그대로 돌아간다 */
					}
				}

				pci_bus_write_config_dword(ibmphp_pci_bus, devfn, address[count], func->pfmem[count]->start); /* [한국어] 고른 시작 주소를 BAR 에 써 넣는다. 위 두 갈래 어느 쪽이든 func->pfmem[count] 가 채워져 있다 */

				/*_______________This is for debugging purposes only______________________________*/
				debug("b4 writing, start address is %x\n", func->pfmem[count]->start); /* [한국어] 쓰기 전 값을 남긴다 */
				pci_bus_read_config_dword(ibmphp_pci_bus, devfn, address[count], &bar[count]); /* [한국어] 되읽어 */
				debug("after writing, start address is %x\n", bar[count]); /* [한국어] 반영되었는지 확인한다 */
				/*_________________________________________________________________________________*/

				if (bar[count] & PCI_BASE_ADDRESS_MEM_TYPE_64) {	/* takes up another dword */ /* [한국어] 64비트 BAR 이면 다음 워드까지 차지한다 */
					debug("inside the mem 64 case, count %d\n", count); /* [한국어] 어느 자리인지 남긴다 */
					count += 1; /* [한국어] 그 워드를 건너뛴다 */
					/* on the 2nd dword, write all 0s, since we can't handle them n.e.ways */
					pci_bus_write_config_dword(ibmphp_pci_bus, devfn, address[count], 0x00000000); /* [한국어] 상류 주석대로 상위 워드에는 0 을 쓴다 — 이 드라이버가 4GB 위를 다루지 못하기 때문이다 */
				}
			} else { /* [한국어] 프리페치가 아니면 일반 메모리다 */
				/* regular memory */
				debug("REGULAR MEM SPACE\n"); /* [한국어] 일반 메모리 갈래임을 남긴다 */

				len[count] = bar[count] & 0xFFFFFFF0; /* [한국어] 하위 4비트 플래그를 지운다 */
				len[count] = ~len[count] + 1; /* [한국어] 뒤집고 1 을 더해 요구 크기를 구한다 */

				debug("len[count] in Mem %x, count %d\n", len[count], count); /* [한국어] 구한 길이를 남긴다 */

				mem[count] = kzalloc_obj(struct resource_node); /* [한국어] 자원 노드를 잡는다 */
				if (!mem[count]) /* [한국어] 메모리가 없으면 */
					return -ENOMEM; /* [한국어] 그대로 돌아간다 */

				mem[count]->type = MEM; /* [한국어] 종류는 메모리 */
				mem[count]->busno = func->busno; /* [한국어] 이 장치의 버스 */
				mem[count]->devfunc = PCI_DEVFN(func->device,
							func->function); /* [한국어] 이 장치·함수 */
				mem[count]->len = len[count]; /* [한국어] 요구 길이 */
				if (ibmphp_check_resource(mem[count], 0) == 0) { /* [한국어] 메모리 창에서 자리를 고른다 */
					ibmphp_add_resource(mem[count]); /* [한국어] 찾았으면 장부에 등록하고 */
					func->mem[count] = mem[count]; /* [한국어] 함수 구조체에 매단다 */
				} else { /* [한국어] 못 찾으면 */
					err("cannot allocate requested mem for bus %x, device %x, len %x\n",
					     func->busno, func->device, len[count]); /* [한국어] 알리고 */
					kfree(mem[count]); /* [한국어] 만든 노드를 버린 뒤 */
					return -EIO; /* [한국어] 그대로 돌아간다 */
				}
				pci_bus_write_config_dword(ibmphp_pci_bus, devfn, address[count], func->mem[count]->start); /* [한국어] 고른 시작 주소를 BAR 에 써 넣는다 */
				/* _______________________This is for debugging purposes only _______________________*/
				debug("b4 writing, start address is %x\n", func->mem[count]->start); /* [한국어] 쓰기 전 값을 남긴다 */
				pci_bus_read_config_dword(ibmphp_pci_bus, devfn, address[count], &bar[count]); /* [한국어] 되읽어 */
				debug("after writing, the address is %x\n", bar[count]); /* [한국어] 반영되었는지 확인한다 */
				/* __________________________________________________________________________________*/

				if (bar[count] & PCI_BASE_ADDRESS_MEM_TYPE_64) { /* [한국어] 64비트 BAR 이면 */
					/* takes up another dword */
					debug("inside mem 64 case, reg. mem, count %d\n", count); /* [한국어] 어느 자리인지 남긴다 */
					count += 1; /* [한국어] 다음 워드를 건너뛰고 */
					/* on the 2nd dword, write all 0s, since we can't handle them n.e.ways */
					pci_bus_write_config_dword(ibmphp_pci_bus, devfn, address[count], 0x00000000); /* [한국어] 상류 주석대로 상위 워드에는 0 을 쓴다 */
				}
			}
		}		/* end of mem */
	}			/* end of for */

	func->bus = 0;	/* To indicate that this is not a PPB */ /* [한국어] 브리지가 아님을 표시한다. ibmphp_unconfigure_card() 가 BAR 개수를 6 으로 세는 근거다 */
	pci_bus_read_config_byte(ibmphp_pci_bus, devfn, PCI_INTERRUPT_PIN, &irq); /* [한국어] 이 장치가 어느 인터럽트 핀을 쓰는지 읽는다 */
	if ((irq > 0x00) && (irq < 0x05)) /* [한국어] 유효한 핀 번호(1~4)이면 */
		pci_bus_write_config_byte(ibmphp_pci_bus, devfn, PCI_INTERRUPT_LINE, func->irq[irq - 1]); /* [한국어] assign_alt_irq() 가 채워 둔 IRQ 중 그 핀의 것을 라인 레지스터에 적는다 */

	pci_bus_write_config_byte(ibmphp_pci_bus, devfn, PCI_CACHE_LINE_SIZE, CACHE); /* [한국어] 캐시 라인 크기를 정한다 */
	pci_bus_write_config_byte(ibmphp_pci_bus, devfn, PCI_LATENCY_TIMER, LATENCY); /* [한국어] 레이턴시 타이머를 정한다 */

	pci_bus_write_config_dword(ibmphp_pci_bus, devfn, PCI_ROM_ADDRESS, 0x00L); /* [한국어] 확장 ROM 을 끈다 — 이 드라이버는 옵션 ROM 을 다루지 않는다 */
	pci_bus_write_config_word(ibmphp_pci_bus, devfn, PCI_COMMAND, DEVICEENABLE); /* [한국어] 마지막으로 명령 레지스터를 켜 장치를 활성화한다. 이 한 줄로 장치가 버스에 응답하기 시작한다 */

	return 0; /* [한국어] 장치 설정 완료 */
}

/******************************************************************************
 * This routine configures a PCI-2-PCI bridge and the functions behind it
 * Parameters: pci_func
 * Returns:
 ******************************************************************************/
/* [한국어]
 * configure_bridge - 브리지의 창 셋을 잡고 그 뒤 2차 버스를 만든다
 *
 * @func_passed: 설정할 브리지 함수. 이중 포인터로 받는다.
 * @slotno:      이 카드가 꽂힌 슬롯 번호. 2차 버스 번호를 고를 때 쓴다.
 * @return: 0 성공, -EINVAL 은 버스 번호를 못 얻은 경우, -ENODEV 는 그 뒤
 *          구성이 지원되지 않는 경우, -ENOMEM / -EIO 는 각각 할당·배정 실패.
 *
 * **이 파일에서 가장 긴 함수**이며, PCI 코어의 setup-bus.c 가 하는 일
 * (브리지 창 크기 산정과 배정)을 이 드라이버가 직접 하는 자리다.
 *
 * 절차가 다섯 토막이다.
 *
 *   [1] 버스 번호 심기
 *       1차 버스 번호를 적고, find_sec_number() 로 2차 버스 번호를 고른 뒤
 *       2차·subordinate 에 같은 값을 적는다. 상류 주석대로 EBDA 가 슬롯마다
 *       버스 번호를 하나씩만 주기 때문에 둘이 같다 — 곧 브리지 뒤에 버스가
 *       하나뿐이라는 뜻이고, 중첩 브리지를 지원하지 않는 이유이기도 하다.
 *       캐시 라인·레이턴시도 함께 적는다.
 *
 *   [2] 브리지 자신의 BAR 둘
 *       브리지도 BAR 를 가질 수 있어 configure_device() 와 같은 방식으로
 *       길이를 탐침해 자리를 잡는다. 다만 BAR 가 여섯이 아니라 둘이다.
 *
 *   [3] 뒤에 필요한 양 재기
 *       scan_behind_bridge() 가 그 뒤 장치들의 BAR 를 모두 훑어 종류별 총합을
 *       돌려준다. not_correct 가 서 있으면(중첩 브리지·VGA·장치 없음) 앞서
 *       잡은 BAR 자원을 되돌리고 -ENODEV 로 끝낸다.
 *
 *   [4] 창 잡기
 *       종류마다 자원 노드를 만들어 **ibmphp_check_resource(..., 1)** 로 자리를
 *       얻는다. 두 번째 인자 1 이 "브리지용" 이라, 그 함수가 IO 4KB·(PF)MEM
 *       1MB 정렬 규칙을 적용한다. 필요 없으면(총합이 0이면) flag 만 세운다.
 *       PFMEM 은 configure_device() 와 마찬가지로 못 잡으면 MEM 창에서 떼어 쓴다.
 *
 *   [5] 버스 등록과 창 써 넣기
 *       셋 다 성공했을 때만 진행한다. 상류 주석대로 이전에 ibmphp 가 로드
 *       되었을 때 만들어 둔 버스 구조가 남아 있을 수 있어, 먼저
 *       ibmphp_find_res_bus() 로 찾아보고 없을 때만 새로 만든다.
 *       그다음 add_new_bus() 로 장부에 창을 등록하고, 브리지의 base/limit
 *       레지스터에 실제 값을 써 넣는다. 창이 없는 종류는 base 를 limit 보다
 *       크게(0xffff / 0x0000) 만들어 **닫힌 창**으로 표시한다.
 *       64비트 확장 레지스터에는 0 을 쓴다 — 상류 주석대로 다룰 수 없기 때문이다.
 *       마지막에 명령 레지스터와 브리지 제어를 켜고, 뒤 장치 목록을 func 에
 *       베끼고, func->bus 를 1 로 세워 브리지임을 표시한다.
 *
 * [관찰] 첫 줄의 devfn 계산이 `PCI_DEVFN(func->function, func->device)` 로
 * **인자 순서가 뒤바뀌어 있다.** 이 파일의 다른 모든 곳은
 * `PCI_DEVFN(func->device, func->function)` 이다. 상류 코드 그대로이며
 * 여기서는 고치지 않는다.
 *
 * [관찰] `func_passed = &func;` 은 지역 포인터에만 대입하는 것이라 호출자에게
 * 전달되지 않는다. 바로 아래 디버그 출력에만 쓰인다. 상류 코드 그대로다.
 *
 * 되돌리기가 error 라벨 하나이며, 잡은 창 셋과 브리지 자신의 BAR 둘을 모두
 * 장부에서 지운다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(카드 삽입).
 *
 * 호출 체인:  ibmphp_configure_card() → [이 함수]
 *               → find_sec_number() → scan_behind_bridge()
 *               → ibmphp_check_resource() → add_new_bus()
 */
static int configure_bridge(struct pci_func **func_passed, u8 slotno)
{
	int count; /* [한국어] BAR 루프 인덱스 */
	int i; /* [한국어] IRQ 핀 루프 인덱스 */
	int rc; /* [한국어] 하위 함수 반환값 임시 보관 */
	u8 sec_number; /* [한국어] 이 브리지 뒤에 붙일 2차 버스 번호 */
	u8 io_base; /* [한국어] 브리지 I/O 창 베이스 레지스터에 넣을 값 */
	u16 pfmem_base; /* [한국어] 브리지 프리페치 창 베이스 레지스터에 넣을 값 */
	u32 bar[2]; /* [한국어] 브리지 자신의 BAR 두 개 탐침 결과. 브리지는 헤더 타입 1 이라 BAR 가 둘뿐이다 */
	u32 len[2]; /* [한국어] BAR 두 개의 요구 길이 */
	u8 flag_io = 0; /* [한국어] 브리지 뒤에 I/O 를 쓰는 장치가 있는지 표시 */
	u8 flag_mem = 0; /* [한국어] 브리지 뒤에 일반 메모리를 쓰는 장치가 있는지 표시 */
	u8 flag_pfmem = 0; /* [한국어] 브리지 뒤에 프리페치 메모리를 쓰는 장치가 있는지 표시 */
	u8 need_io_upper = 0; /* [한국어] I/O 창이 64KB 를 넘어 상위 16비트 레지스터까지 써야 하는지 표시 */
	u8 need_pfmem_upper = 0; /* [한국어] 프리페치 창이 4GB 를 넘어 상위 32비트 레지스터까지 써야 하는지 표시 */
	struct res_needed *amount_needed = NULL; /* [한국어] scan_behind_bridge() 가 채워 줄, 뒤쪽 장치들의 요구량 합계 */
	struct resource_node *io = NULL; /* [한국어] 브리지 뒤 전체를 덮을 I/O 창 자원 노드 */
	struct resource_node *bus_io[2] = {NULL, NULL}; /* [한국어] 브리지 자신의 BAR 용 I/O 자원 노드 배열 */
	struct resource_node *mem = NULL; /* [한국어] 브리지 뒤 전체를 덮을 메모리 창 자원 노드 */
	struct resource_node *bus_mem[2] = {NULL, NULL}; /* [한국어] 브리지 자신의 BAR 용 메모리 자원 노드 배열 */
	struct resource_node *mem_tmp = NULL; /* [한국어] 프리페치를 MEM 창에서 떼어 쓸 때 만드는 임시 노드 */
	struct resource_node *pfmem = NULL; /* [한국어] 브리지 뒤 전체를 덮을 프리페치 창 자원 노드 */
	struct resource_node *bus_pfmem[2] = {NULL, NULL}; /* [한국어] 브리지 자신의 BAR 용 프리페치 자원 노드 배열 */
	struct bus_node *bus; /* [한국어] 새로 만들 2차 버스의 장부 노드 */
	static const u32 address[] = { /* [한국어] 브리지의 BAR 오프셋 표. 헤더 타입 1 은 BAR 가 둘이라 그 둘과 끝 표시 0 만 있다 */
		PCI_BASE_ADDRESS_0, /* [한국어] BAR 0 */
		PCI_BASE_ADDRESS_1, /* [한국어] BAR 1 */
		0 /* [한국어] 끝 표시 */
	};
	struct pci_func *func = *func_passed; /* [한국어] 호출자가 넘긴 포인터의 포인터에서 실제 함수 구조체를 꺼낸다 */
	unsigned int devfn; /* [한국어] config 접근에 쓸 장치·함수 번호 */
	u8 irq; /* [한국어] 인터럽트 핀별로 배정된 IRQ 를 담을 자리 */
	int retval; /* [한국어] 에러 경로에서 error 라벨로 넘길 반환값 */

	debug("%s - enter\n", __func__); /* [한국어] 진입 기록 */

	devfn = PCI_DEVFN(func->function, func->device); /* [한국어] 상류 코드가 device 와 function 을 반대 순서로 넘긴다 — 아래 자원 노드 설정부의 PCI_DEVFN(func->device, func->function) 과 인자 순서가 다르다 */
	ibmphp_pci_bus->number = func->busno; /* [한국어] 전역 껍데기 버스의 번호를 이 브리지가 붙어 있는 버스로 맞춘다 */

	/* Configuring necessary info for the bridge so that we could see the devices
	 * behind it
	 */

	pci_bus_write_config_byte(ibmphp_pci_bus, devfn, PCI_PRIMARY_BUS, func->busno); /* [한국어] **1차 버스 번호**를 브리지에 적는다 — 브리지 위쪽이 어느 버스인지 알린다 */

	/* _____________________For debugging purposes only __________________________
	pci_bus_config_byte(ibmphp_pci_bus, devfn, PCI_PRIMARY_BUS, &pri_number);
	debug("primary # written into the bridge is %x\n", pri_number);
	 ___________________________________________________________________________*/

	/* in EBDA, only get allocated 1 additional bus # per slot */
	sec_number = find_sec_number(func->busno, slotno); /* [한국어] EBDA 표에서 이 슬롯 몫으로 예약된 버스 번호를 하나 꺼낸다. 슬롯당 하나뿐이라 브리지가 이중으로 겹치면 실패한다 */
	if (sec_number == 0xff) { /* [한국어] 0xff 는 남은 번호가 없다는 뜻이다 */
		err("cannot allocate secondary bus number for the bridged device\n"); /* [한국어] 무엇이 없는지 알리고 */
		return -EINVAL; /* [한국어] 설정을 포기한다 */
	}

	debug("after find_sec_number, the number we got is %x\n", sec_number); /* [한국어] 받은 번호를 남긴다 */
	debug("AFTER FIND_SEC_NUMBER, func->busno IS %x\n", func->busno); /* [한국어] 1차 버스가 그대로인지도 남긴다 */

	pci_bus_write_config_byte(ibmphp_pci_bus, devfn, PCI_SECONDARY_BUS, sec_number); /* [한국어] **2차 버스 번호**를 적는다 — 이 브리지 바로 뒤의 버스 번호다 */

	/* __________________For debugging purposes only __________________________________
	pci_bus_read_config_byte(ibmphp_pci_bus, devfn, PCI_SECONDARY_BUS, &sec_number);
	debug("sec_number after write/read is %x\n", sec_number);
	 ________________________________________________________________________________*/

	pci_bus_write_config_byte(ibmphp_pci_bus, devfn, PCI_SUBORDINATE_BUS, sec_number); /* [한국어] **하위 버스 번호**도 같은 값으로 적는다. 뒤에 또 브리지가 없다고 보는 것이다 */

	/* __________________For debugging purposes only ____________________________________
	pci_bus_read_config_byte(ibmphp_pci_bus, devfn, PCI_SUBORDINATE_BUS, &sec_number);
	debug("subordinate number after write/read is %x\n", sec_number);
	 __________________________________________________________________________________*/

	pci_bus_write_config_byte(ibmphp_pci_bus, devfn, PCI_CACHE_LINE_SIZE, CACHE); /* [한국어] 캐시 라인 크기를 정한다 */
	pci_bus_write_config_byte(ibmphp_pci_bus, devfn, PCI_LATENCY_TIMER, LATENCY); /* [한국어] 1차 쪽 레이턴시 타이머를 정한다 */
	pci_bus_write_config_byte(ibmphp_pci_bus, devfn, PCI_SEC_LATENCY_TIMER, LATENCY); /* [한국어] 2차 쪽 레이턴시 타이머를 정한다 */

	debug("func->busno is %x\n", func->busno); /* [한국어] 1차 버스 번호를 남긴다 */
	debug("sec_number after writing is %x\n", sec_number); /* [한국어] 2차 버스 번호를 남긴다 */


	/* !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	   !!!!!!!!!!!!!!!NEED TO ADD!!!  FAST BACK-TO-BACK ENABLE!!!!!!!!!!!!!!!!!!!!
	   !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!*/


	/* First we need to allocate mem/io for the bridge itself in case it needs it */
	for (count = 0; address[count]; count++) {	/* for 2 BARs */ /* [한국어] 브리지 자신의 BAR 두 개를 먼저 본다. 뒤쪽 장치들보다 브리지 자신의 자원을 먼저 잡는다 */
		pci_bus_write_config_dword(ibmphp_pci_bus, devfn, address[count], 0xFFFFFFFF); /* [한국어] 길이 탐침 — 모든 비트를 1 로 써 본다 */
		pci_bus_read_config_dword(ibmphp_pci_bus, devfn, address[count], &bar[count]); /* [한국어] 되읽어 요구 크기를 알아낸다 */

		if (!bar[count]) { /* [한국어] 되읽은 값이 0 이면 */
			/* This BAR is not implemented */
			debug("so we come here then, eh?, count = %d\n", count); /* [한국어] 구현되지 않은 BAR 임을 남기고 */
			continue; /* [한국어] 다음 BAR 로 */
		}
		//  tmp_bar = bar[count];

		debug("Bar %d wants %x\n", count, bar[count]); /* [한국어] 무엇을 요구하는지 남긴다 */

		if (bar[count] & PCI_BASE_ADDRESS_SPACE_IO) { /* [한국어] 최하위 비트가 서 있으면 I/O 공간이다 */
			/* This is IO */
			len[count] = bar[count] & 0xFFFFFFFC; /* [한국어] I/O BAR 는 하위 2비트가 플래그라 그것을 지운다 */
			len[count] = ~len[count] + 1; /* [한국어] 뒤집고 1 을 더하면 요구 크기가 된다 */

			debug("len[count] in IO = %x\n", len[count]); /* [한국어] 구한 길이를 남긴다 */

			bus_io[count] = kzalloc_obj(struct resource_node); /* [한국어] 자원 노드를 잡는다 */

			if (!bus_io[count]) { /* [한국어] 메모리가 없으면 */
				retval = -ENOMEM; /* [한국어] ENOMEM 을 담아 */
				goto error; /* [한국어] 공통 정리 경로로 간다 */
			}
			bus_io[count]->type = IO; /* [한국어] 종류는 I/O */
			bus_io[count]->busno = func->busno; /* [한국어] 이 브리지가 붙어 있는 버스 */
			bus_io[count]->devfunc = PCI_DEVFN(func->device, /* [한국어] 이 브리지의 장치·함수 */
							func->function);
			bus_io[count]->len = len[count]; /* [한국어] 요구 길이 */
			if (ibmphp_check_resource(bus_io[count], 0) == 0) { /* [한국어] 장부에서 자리를 고른다. 두 번째 인자 0 은 브리지 창이 아니라 일반 BAR 라는 뜻이다 */
				ibmphp_add_resource(bus_io[count]); /* [한국어] 고른 자리를 장부에 등록하고 */
				func->io[count] = bus_io[count]; /* [한국어] 함수 구조체에 매단다 */
			} else { /* [한국어] 자리를 못 찾으면 */
				err("cannot allocate requested io for bus %x, device %x, len %x\n", /* [한국어] 무엇이 안 되었는지 알리고 */
				     func->busno, func->device, len[count]);
				kfree(bus_io[count]); /* [한국어] 만든 노드를 버린 뒤 */
				return -EIO; /* [한국어] 설정을 포기한다 */
			}

			pci_bus_write_config_dword(ibmphp_pci_bus, devfn, address[count], func->io[count]->start); /* [한국어] 고른 시작 주소를 BAR 에 써 넣는다 */

		} else { /* [한국어] 최하위 비트가 0 이면 메모리 공간이다 */
			/* This is Memory */
			if (bar[count] & PCI_BASE_ADDRESS_MEM_PREFETCH) { /* [한국어] 프리페치 비트가 서 있으면 PFMEM 이다 */
				/* pfmem */
				len[count] = bar[count] & 0xFFFFFFF0; /* [한국어] 메모리 BAR 는 하위 4비트가 플래그라 그것을 지운다 */
				len[count] = ~len[count] + 1; /* [한국어] 뒤집고 1 을 더해 요구 크기를 구한다 */

				debug("len[count] in PFMEM = %x\n", len[count]); /* [한국어] 구한 길이를 남긴다 */

				bus_pfmem[count] = kzalloc_obj(struct resource_node); /* [한국어] 자원 노드를 잡는다 */
				if (!bus_pfmem[count]) { /* [한국어] 메모리가 없으면 */
					retval = -ENOMEM; /* [한국어] ENOMEM 을 담아 */
					goto error; /* [한국어] 공통 정리 경로로 간다 */
				}
				bus_pfmem[count]->type = PFMEM; /* [한국어] 종류는 프리페치 메모리 */
				bus_pfmem[count]->busno = func->busno; /* [한국어] 이 브리지가 붙어 있는 버스 */
				bus_pfmem[count]->devfunc = PCI_DEVFN(func->device, /* [한국어] 이 브리지의 장치·함수 */
							func->function);
				bus_pfmem[count]->len = len[count]; /* [한국어] 요구 길이 */
				bus_pfmem[count]->fromMem = 0; /* [한국어] 아직 MEM 에서 떼어 쓴 것이 아니다 */
				if (ibmphp_check_resource(bus_pfmem[count], 0) == 0) { /* [한국어] 프리페치 창에서 자리를 고른다 */
					ibmphp_add_resource(bus_pfmem[count]); /* [한국어] 찾았으면 장부에 등록하고 */
					func->pfmem[count] = bus_pfmem[count]; /* [한국어] 함수 구조체에 매단다 */
				} else { /* [한국어] 못 찾으면 일반 MEM 창에서 떼어 쓴다 */
					mem_tmp = kzalloc_obj(*mem_tmp); /* [한국어] 임시 MEM 노드를 만든다 */
					if (!mem_tmp) { /* [한국어] 메모리가 없으면 */
						retval = -ENOMEM; /* [한국어] ENOMEM 을 담아 */
						goto error; /* [한국어] 공통 정리 경로로 간다 */
					}
					mem_tmp->type = MEM; /* [한국어] 종류만 메모리로 바꾸고 */
					mem_tmp->busno = bus_pfmem[count]->busno; /* [한국어] 같은 버스 */
					mem_tmp->devfunc = bus_pfmem[count]->devfunc; /* [한국어] 같은 장치·함수 */
					mem_tmp->len = bus_pfmem[count]->len; /* [한국어] 같은 길이로 자리를 찾는다 */
					if (ibmphp_check_resource(mem_tmp, 0) == 0) { /* [한국어] 메모리 창에서 자리를 고른다 */
						ibmphp_add_resource(mem_tmp); /* [한국어] 찾았으면 MEM 으로 장부에 등록한다 */
						bus_pfmem[count]->fromMem = 1; /* [한국어] 출처가 MEM 임을 표시하고 */
						bus_pfmem[count]->rangeno = mem_tmp->rangeno; /* [한국어] 어느 창에서 떼어 왔는지 창 번호를 베낀다 */
						ibmphp_add_pfmem_from_mem(bus_pfmem[count]); /* [한국어] 곁가지 목록에 매단다 */
						func->pfmem[count] = bus_pfmem[count]; /* [한국어] 함수 구조체에는 PFMEM 노드를 매단다 */
					} else {
						err("cannot allocate requested pfmem for bus %x, device %x, len %x\n", /* [한국어] 무엇이 안 되었는지 알리고 */
						     func->busno, func->device, len[count]);
						kfree(mem_tmp); /* [한국어] 임시 노드와 */
						kfree(bus_pfmem[count]); /* [한국어] PFMEM 노드를 모두 버린 뒤 */
						return -EIO; /* [한국어] 설정을 포기한다 */
					}
				}

				pci_bus_write_config_dword(ibmphp_pci_bus, devfn, address[count], func->pfmem[count]->start); /* [한국어] 고른 시작 주소를 BAR 에 써 넣는다 */

				if (bar[count] & PCI_BASE_ADDRESS_MEM_TYPE_64) { /* [한국어] 64비트 BAR 이면 다음 워드까지 차지한다 */
					/* takes up another dword */
					count += 1; /* [한국어] 그 워드를 건너뛰고 */
					/* on the 2nd dword, write all 0s, since we can't handle them n.e.ways */
					pci_bus_write_config_dword(ibmphp_pci_bus, devfn, address[count], 0x00000000); /* [한국어] 상류 주석대로 상위 워드에는 0 을 쓴다 — 이 드라이버가 4GB 위를 다루지 못하기 때문이다 */

				}
			} else {
				/* regular memory */
				len[count] = bar[count] & 0xFFFFFFF0; /* [한국어] 프리페치가 아닌 일반 메모리. 하위 4비트 플래그를 지운다 */
				len[count] = ~len[count] + 1; /* [한국어] 뒤집고 1 을 더해 요구 크기를 구한다 */

				debug("len[count] in Memory is %x\n", len[count]); /* [한국어] 구한 길이를 남긴다 */

				bus_mem[count] = kzalloc_obj(struct resource_node); /* [한국어] 자원 노드를 잡는다 */
				if (!bus_mem[count]) { /* [한국어] 메모리가 없으면 */
					retval = -ENOMEM; /* [한국어] ENOMEM 을 담아 */
					goto error; /* [한국어] 공통 정리 경로로 간다 */
				}
				bus_mem[count]->type = MEM; /* [한국어] 종류는 메모리 */
				bus_mem[count]->busno = func->busno; /* [한국어] 이 브리지가 붙어 있는 버스 */
				bus_mem[count]->devfunc = PCI_DEVFN(func->device, /* [한국어] 이 브리지의 장치·함수 */
							func->function);
				bus_mem[count]->len = len[count]; /* [한국어] 요구 길이 */
				if (ibmphp_check_resource(bus_mem[count], 0) == 0) { /* [한국어] 메모리 창에서 자리를 고른다 */
					ibmphp_add_resource(bus_mem[count]); /* [한국어] 찾았으면 장부에 등록하고 */
					func->mem[count] = bus_mem[count]; /* [한국어] 함수 구조체에 매단다 */
				} else { /* [한국어] 못 찾으면 */
					err("cannot allocate requested mem for bus %x, device %x, len %x\n", /* [한국어] 무엇이 안 되었는지 알리고 */
					     func->busno, func->device, len[count]);
					kfree(bus_mem[count]); /* [한국어] 만든 노드를 버린 뒤 */
					return -EIO; /* [한국어] 설정을 포기한다 */
				}

				pci_bus_write_config_dword(ibmphp_pci_bus, devfn, address[count], func->mem[count]->start); /* [한국어] 고른 시작 주소를 BAR 에 써 넣는다 */

				if (bar[count] & PCI_BASE_ADDRESS_MEM_TYPE_64) { /* [한국어] 64비트 BAR 이면 */
					/* takes up another dword */
					count += 1; /* [한국어] 다음 워드를 건너뛰고 */
					/* on the 2nd dword, write all 0s, since we can't handle them n.e.ways */
					pci_bus_write_config_dword(ibmphp_pci_bus, devfn, address[count], 0x00000000); /* [한국어] 상위 워드에는 0 을 쓴다 */

				}
			}
		}	/* end of mem */ /* [한국어] 메모리 갈래 끝 */
	}			/* end of for  */

	/* Now need to see how much space the devices behind the bridge needed */
	amount_needed = scan_behind_bridge(func, sec_number); /* [한국어] **여기부터가 브리지 특유의 일이다** — 뒤쪽 버스를 훑어 필요한 총량을 구한다 */
	if (amount_needed == NULL) /* [한국어] 구조체 할당조차 못 했으면 */
		return -ENOMEM; /* [한국어] 그대로 돌아간다 */

	ibmphp_pci_bus->number = func->busno; /* [한국어] scan_behind_bridge() 가 껍데기 버스 번호를 2차 버스로 바꿔 놓았으므로 되돌린다 */
	debug("after coming back from scan_behind_bridge\n"); /* [한국어] 돌아왔음을 남긴다 */
	debug("amount_needed->not_correct = %x\n", amount_needed->not_correct); /* [한국어] 뒤쪽 구성을 신뢰할 수 없는지 남긴다 */
	debug("amount_needed->io = %x\n", amount_needed->io); /* [한국어] 필요한 I/O 총량 */
	debug("amount_needed->mem = %x\n", amount_needed->mem); /* [한국어] 필요한 메모리 총량 */
	debug("amount_needed->pfmem =  %x\n", amount_needed->pfmem); /* [한국어] 필요한 프리페치 메모리 총량 */

	if (amount_needed->not_correct) { /* [한국어] 뒤쪽에 브리지가 또 있거나 다중 함수라 이 드라이버가 감당 못 하는 구성이면 */
		debug("amount_needed is not correct\n"); /* [한국어] 포기함을 남기고 */
		for (count = 0; address[count]; count++) { /* [한국어] 브리지 자신에게 이미 잡아 둔 BAR 자원을 되돌린다 */
			/* for 2 BARs */
			if (bus_io[count]) { /* [한국어] I/O 를 잡아 두었으면 */
				ibmphp_remove_resource(bus_io[count]); /* [한국어] 장부에서 지우고 */
				func->io[count] = NULL; /* [한국어] 함수 구조체의 포인터도 끊는다 */
			} else if (bus_pfmem[count]) { /* [한국어] 프리페치를 잡아 두었으면 */
				ibmphp_remove_resource(bus_pfmem[count]); /* [한국어] 장부에서 지우고 */
				func->pfmem[count] = NULL; /* [한국어] 포인터를 끊는다 */
			} else if (bus_mem[count]) { /* [한국어] 일반 메모리를 잡아 두었으면 */
				ibmphp_remove_resource(bus_mem[count]); /* [한국어] 장부에서 지우고 */
				func->mem[count] = NULL; /* [한국어] 포인터를 끊는다 */
			}
		}
		kfree(amount_needed); /* [한국어] 요구량 구조체를 버리고 */
		return -ENODEV; /* [한국어] 장치 없음으로 돌아간다 */
	}

	if (!amount_needed->io) { /* [한국어] 뒤쪽이 I/O 를 전혀 안 쓰면 */
		debug("it doesn't want IO?\n"); /* [한국어] 그 사실을 남기고 */
		flag_io = 1; /* [한국어] I/O 는 준비된 것으로 친다 */
	} else {
		debug("it wants %x IO behind the bridge\n", amount_needed->io); /* [한국어] 얼마가 필요한지 남긴다 */
		io = kzalloc_obj(*io); /* [한국어] 브리지 I/O 창 노드를 잡는다 */

		if (!io) { /* [한국어] 메모리가 없으면 */
			retval = -ENOMEM; /* [한국어] ENOMEM 을 담아 */
			goto error; /* [한국어] 공통 정리 경로로 간다 */
		}
		io->type = IO; /* [한국어] 종류는 I/O */
		io->busno = func->busno; /* [한국어] 창을 여는 브리지가 붙어 있는 버스 */
		io->devfunc = PCI_DEVFN(func->device, func->function); /* [한국어] 브리지 자신의 장치·함수 */
		io->len = amount_needed->io; /* [한국어] 뒤쪽 총량만큼 */
		if (ibmphp_check_resource(io, 1) == 0) { /* [한국어] **두 번째 인자가 1** — 브리지 창이므로 4KB 경계로 정렬해 자리를 고른다 */
			debug("were we able to add io\n"); /* [한국어] 성공했음을 남기고 */
			ibmphp_add_resource(io); /* [한국어] 장부에 등록한 뒤 */
			flag_io = 1; /* [한국어] I/O 준비 완료로 표시한다 */
		}
	}

	if (!amount_needed->mem) { /* [한국어] 뒤쪽이 일반 메모리를 전혀 안 쓰면 */
		debug("it doesn't want n.e.memory?\n"); /* [한국어] 그 사실을 남기고 */
		flag_mem = 1; /* [한국어] 메모리는 준비된 것으로 친다 */
	} else {
		debug("it wants %x memory behind the bridge\n", amount_needed->mem); /* [한국어] 얼마가 필요한지 남긴다 */
		mem = kzalloc_obj(*mem); /* [한국어] 브리지 메모리 창 노드를 잡는다 */
		if (!mem) { /* [한국어] 메모리가 없으면 */
			retval = -ENOMEM; /* [한국어] ENOMEM 을 담아 */
			goto error; /* [한국어] 공통 정리 경로로 간다 */
		}
		mem->type = MEM; /* [한국어] 종류는 메모리 */
		mem->busno = func->busno; /* [한국어] 브리지가 붙어 있는 버스 */
		mem->devfunc = PCI_DEVFN(func->device, func->function); /* [한국어] 브리지 자신의 장치·함수 */
		mem->len = amount_needed->mem; /* [한국어] 뒤쪽 총량만큼 */
		if (ibmphp_check_resource(mem, 1) == 0) { /* [한국어] 브리지 창이므로 1MB 경계로 정렬해 자리를 고른다 */
			ibmphp_add_resource(mem); /* [한국어] 장부에 등록하고 */
			flag_mem = 1; /* [한국어] 메모리 준비 완료로 표시한 뒤 */
			debug("were we able to add mem\n"); /* [한국어] 성공했음을 남긴다 */
		}
	}

	if (!amount_needed->pfmem) { /* [한국어] 뒤쪽이 프리페치 메모리를 전혀 안 쓰면 */
		debug("it doesn't want n.e.pfmem mem?\n"); /* [한국어] 그 사실을 남기고 */
		flag_pfmem = 1; /* [한국어] 프리페치는 준비된 것으로 친다 */
	} else {
		debug("it wants %x pfmemory behind the bridge\n", amount_needed->pfmem); /* [한국어] 얼마가 필요한지 남긴다 */
		pfmem = kzalloc_obj(*pfmem); /* [한국어] 브리지 프리페치 창 노드를 잡는다 */
		if (!pfmem) { /* [한국어] 메모리가 없으면 */
			retval = -ENOMEM; /* [한국어] ENOMEM 을 담아 */
			goto error; /* [한국어] 공통 정리 경로로 간다 */
		}
		pfmem->type = PFMEM; /* [한국어] 종류는 프리페치 메모리 */
		pfmem->busno = func->busno; /* [한국어] 브리지가 붙어 있는 버스 */
		pfmem->devfunc = PCI_DEVFN(func->device, func->function); /* [한국어] 브리지 자신의 장치·함수 */
		pfmem->len = amount_needed->pfmem; /* [한국어] 뒤쪽 총량만큼 */
		pfmem->fromMem = 0; /* [한국어] 아직 MEM 에서 떼어 쓴 것이 아니다 */
		if (ibmphp_check_resource(pfmem, 1) == 0) { /* [한국어] 브리지 창이므로 1MB 경계로 정렬해 프리페치 창에서 자리를 고른다 */
			ibmphp_add_resource(pfmem); /* [한국어] 장부에 등록하고 */
			flag_pfmem = 1; /* [한국어] 프리페치 준비 완료로 표시한다 */
		} else { /* [한국어] 프리페치 창에서 못 찾으면 일반 MEM 창에서 떼어 쓴다 */
			mem_tmp = kzalloc_obj(*mem_tmp); /* [한국어] 임시 MEM 노드를 만든다 */
			if (!mem_tmp) { /* [한국어] 메모리가 없으면 */
				retval = -ENOMEM; /* [한국어] ENOMEM 을 담아 */
				goto error; /* [한국어] 공통 정리 경로로 간다 */
			}
			mem_tmp->type = MEM; /* [한국어] 종류만 메모리로 바꾸고 */
			mem_tmp->busno = pfmem->busno; /* [한국어] 같은 버스 */
			mem_tmp->devfunc = pfmem->devfunc; /* [한국어] 같은 장치·함수 */
			mem_tmp->len = pfmem->len; /* [한국어] 같은 길이로 */
			if (ibmphp_check_resource(mem_tmp, 1) == 0) { /* [한국어] 메모리 창에서 1MB 정렬 자리를 고른다 */
				ibmphp_add_resource(mem_tmp); /* [한국어] 찾았으면 MEM 으로 장부에 등록한다 */
				pfmem->fromMem = 1; /* [한국어] 출처가 MEM 임을 표시하고 */
				pfmem->rangeno = mem_tmp->rangeno; /* [한국어] 어느 창에서 떼어 왔는지 창 번호를 베낀 뒤 */
				ibmphp_add_pfmem_from_mem(pfmem); /* [한국어] 곁가지 목록에 매단다 */
				flag_pfmem = 1; /* [한국어] 프리페치 준비 완료로 표시한다 */
			}
		}
	}

	debug("b4 if (flag_io && flag_mem && flag_pfmem)\n"); /* [한국어] 세 플래그를 확인하기 직전임을 남긴다 */
	debug("flag_io = %x, flag_mem = %x, flag_pfmem = %x\n", flag_io, flag_mem, flag_pfmem); /* [한국어] 세 플래그 값을 남긴다 */

	if (flag_io && flag_mem && flag_pfmem) { /* [한국어] **셋 다 준비되었을 때만** 2차 버스를 실제로 만든다. 하나라도 없으면 아래 error 경로로 간다 */
		/* If on bootup, there was a bridged card in this slot,
		 * then card was removed and ibmphp got unloaded and loaded
		 * back again, there's no way for us to remove the bus
		 * struct, so no need to kmalloc, can use existing node
		 */
		bus = ibmphp_find_res_bus(sec_number); /* [한국어] 부팅 때 이미 이 슬롯에 브리지 카드가 있었다면 장부에 2차 버스 노드가 남아 있다 */
		if (!bus) { /* [한국어] 없으면 새로 만든다 */
			bus = kzalloc_obj(*bus); /* [한국어] 버스 노드를 잡는다 */
			if (!bus) { /* [한국어] 메모리가 없으면 */
				retval = -ENOMEM; /* [한국어] ENOMEM 을 담아 */
				goto error; /* [한국어] 공통 정리 경로로 간다 */
			}
			bus->busno = sec_number; /* [한국어] 방금 배정받은 2차 버스 번호를 붙이고 */
			debug("b4 adding new bus\n"); /* [한국어] 추가 직전임을 남긴다 */
			rc = add_new_bus(bus, io, mem, pfmem, func->busno); /* [한국어] 세 창을 이 버스의 범위로 등록한다. 마지막 인자는 부모 버스 번호다 */
		} else if (!(bus->rangeIO) && !(bus->rangeMem) && !(bus->rangePFMem)) /* [한국어] 노드는 있는데 범위가 셋 다 비어 있으면 껍데기만 남은 것이다 */
			rc = add_new_bus(bus, io, mem, pfmem, 0xFF); /* [한국어] 부모 번호 자리에 0xFF 를 넘겨 부모 범위를 다시 쪼개지 않도록 한다 */
		else { /* [한국어] 범위가 이미 차 있으면 예상 밖 상태다 */
			err("expected bus structure not empty?\n"); /* [한국어] 무엇이 이상한지 알리고 */
			retval = -EIO; /* [한국어] 입출력 오류로 */
			goto error; /* [한국어] 공통 정리 경로로 간다 */
		}
		if (rc) { /* [한국어] 버스 등록이 실패했으면 */
			if (rc == -ENOMEM) { /* [한국어] 메모리 부족이면 */
				ibmphp_remove_bus(bus, func->busno); /* [한국어] 만들던 버스를 통째로 걷어내고 */
				kfree(amount_needed); /* [한국어] 요구량 구조체를 버린 뒤 */
				return rc; /* [한국어] 그 오류를 그대로 돌려준다 */
			}
			retval = rc; /* [한국어] 그 밖의 오류는 */
			goto error; /* [한국어] 공통 정리 경로로 넘긴다 */
		}
		pci_bus_read_config_byte(ibmphp_pci_bus, devfn, PCI_IO_BASE, &io_base); /* [한국어] 브리지가 32비트 I/O 창을 지원하는지 확인하려고 베이스 레지스터를 읽는다 */
		pci_bus_read_config_word(ibmphp_pci_bus, devfn, PCI_PREF_MEMORY_BASE, &pfmem_base); /* [한국어] 프리페치 창이 64비트인지 확인하려고 베이스 레지스터를 읽는다 */

		if ((io_base & PCI_IO_RANGE_TYPE_MASK) == PCI_IO_RANGE_TYPE_32) { /* [한국어] 하위 4비트가 0x1 이면 32비트 I/O 디코딩이다 */
			debug("io 32\n"); /* [한국어] 그 사실을 남기고 */
			need_io_upper = 1; /* [한국어] 상위 16비트 레지스터도 써야 한다고 표시한다 */
		}
		if ((pfmem_base & PCI_PREF_RANGE_TYPE_MASK) == PCI_PREF_RANGE_TYPE_64) { /* [한국어] 하위 4비트가 0x1 이면 64비트 프리페치 디코딩이다 */
			debug("pfmem 64\n"); /* [한국어] 그 사실을 남기고 */
			need_pfmem_upper = 1; /* [한국어] 상위 32비트 레지스터도 써야 한다고 표시한다 */
		}

		if (bus->noIORanges) { /* [한국어] 이 버스에 I/O 범위가 잡혔으면 */
			pci_bus_write_config_byte(ibmphp_pci_bus, devfn, PCI_IO_BASE, 0x00 | bus->rangeIO->start >> 8); /* [한국어] **I/O 베이스**를 적는다. I/O 창 레지스터는 상위 8비트만 담으므로 8비트 오른쪽으로 민다 */
			pci_bus_write_config_byte(ibmphp_pci_bus, devfn, PCI_IO_LIMIT, 0x00 | bus->rangeIO->end >> 8); /* [한국어] **I/O 리밋**도 같은 방식으로 적는다 */

			/* _______________This is for debugging purposes only ____________________
			pci_bus_read_config_byte(ibmphp_pci_bus, devfn, PCI_IO_BASE, &temp);
			debug("io_base = %x\n", (temp & PCI_IO_RANGE_TYPE_MASK) << 8);
			pci_bus_read_config_byte(ibmphp_pci_bus, devfn, PCI_IO_LIMIT, &temp);
			debug("io_limit = %x\n", (temp & PCI_IO_RANGE_TYPE_MASK) << 8);
			 ________________________________________________________________________*/

			if (need_io_upper) {	/* since can't support n.e.ways */ /* [한국어] 32비트 I/O 디코딩이면 상위 레지스터도 채워야 한다 */
				pci_bus_write_config_word(ibmphp_pci_bus, devfn, PCI_IO_BASE_UPPER16, 0x0000); /* [한국어] 상류 주석대로 상위 베이스에 0 을 쓴다 — 이 드라이버는 64KB 위를 쓰지 않는다 */
				pci_bus_write_config_word(ibmphp_pci_bus, devfn, PCI_IO_LIMIT_UPPER16, 0x0000); /* [한국어] 상위 리밋에도 0 을 쓴다 */
			}
		} else {
			pci_bus_write_config_byte(ibmphp_pci_bus, devfn, PCI_IO_BASE, 0x00); /* [한국어] 범위가 없으면 베이스를 0 으로 */
			pci_bus_write_config_byte(ibmphp_pci_bus, devfn, PCI_IO_LIMIT, 0x00); /* [한국어] 리밋도 0 으로 두어 창을 사실상 닫는다 */
		}

		if (bus->noMemRanges) { /* [한국어] 이 버스에 메모리 범위가 잡혔으면 */
			pci_bus_write_config_word(ibmphp_pci_bus, devfn, PCI_MEMORY_BASE, 0x0000 | bus->rangeMem->start >> 16); /* [한국어] **메모리 베이스**를 적는다. 메모리 창 레지스터는 상위 16비트만 담으므로 16비트 오른쪽으로 민다 */
			pci_bus_write_config_word(ibmphp_pci_bus, devfn, PCI_MEMORY_LIMIT, 0x0000 | bus->rangeMem->end >> 16); /* [한국어] **메모리 리밋**도 같은 방식으로 적는다 */

			/* ____________________This is for debugging purposes only ________________________
			pci_bus_read_config_word(ibmphp_pci_bus, devfn, PCI_MEMORY_BASE, &temp);
			debug("mem_base = %x\n", (temp & PCI_MEMORY_RANGE_TYPE_MASK) << 16);
			pci_bus_read_config_word(ibmphp_pci_bus, devfn, PCI_MEMORY_LIMIT, &temp);
			debug("mem_limit = %x\n", (temp & PCI_MEMORY_RANGE_TYPE_MASK) << 16);
			 __________________________________________________________________________________*/

		} else {
			pci_bus_write_config_word(ibmphp_pci_bus, devfn, PCI_MEMORY_BASE, 0xffff); /* [한국어] 범위가 없으면 베이스를 0xffff 로 */
			pci_bus_write_config_word(ibmphp_pci_bus, devfn, PCI_MEMORY_LIMIT, 0x0000); /* [한국어] 리밋을 0 으로 두어 베이스 > 리밋 이 되게 한다 — 이것이 창을 닫는 규격상 방법이다 */
		}
		if (bus->noPFMemRanges) { /* [한국어] 이 버스에 프리페치 범위가 잡혔으면 */
			pci_bus_write_config_word(ibmphp_pci_bus, devfn, PCI_PREF_MEMORY_BASE, 0x0000 | bus->rangePFMem->start >> 16); /* [한국어] **프리페치 베이스**를 상위 16비트만 적는다 */
			pci_bus_write_config_word(ibmphp_pci_bus, devfn, PCI_PREF_MEMORY_LIMIT, 0x0000 | bus->rangePFMem->end >> 16); /* [한국어] **프리페치 리밋**도 같은 방식으로 적는다 */

			/* __________________________This is for debugging purposes only _______________________
			pci_bus_read_config_word(ibmphp_pci_bus, devfn, PCI_PREF_MEMORY_BASE, &temp);
			debug("pfmem_base = %x", (temp & PCI_MEMORY_RANGE_TYPE_MASK) << 16);
			pci_bus_read_config_word(ibmphp_pci_bus, devfn, PCI_PREF_MEMORY_LIMIT, &temp);
			debug("pfmem_limit = %x\n", (temp & PCI_MEMORY_RANGE_TYPE_MASK) << 16);
			 ______________________________________________________________________________________*/

			if (need_pfmem_upper) {	/* since can't support n.e.ways */ /* [한국어] 64비트 프리페치 디코딩이면 상위 레지스터도 채워야 한다 */
				pci_bus_write_config_dword(ibmphp_pci_bus, devfn, PCI_PREF_BASE_UPPER32, 0x00000000); /* [한국어] 상류 주석대로 상위 베이스에 0 을 쓴다 — 4GB 위를 쓰지 않기 때문이다 */
				pci_bus_write_config_dword(ibmphp_pci_bus, devfn, PCI_PREF_LIMIT_UPPER32, 0x00000000); /* [한국어] 상위 리밋에도 0 을 쓴다 */
			}
		} else {
			pci_bus_write_config_word(ibmphp_pci_bus, devfn, PCI_PREF_MEMORY_BASE, 0xffff); /* [한국어] 범위가 없으면 베이스를 0xffff 로 */
			pci_bus_write_config_word(ibmphp_pci_bus, devfn, PCI_PREF_MEMORY_LIMIT, 0x0000); /* [한국어] 리밋을 0 으로 두어 창을 닫는다 */
		}

		debug("b4 writing control information\n"); /* [한국어] 제어 정보를 쓰기 직전임을 남긴다 */

		pci_bus_read_config_byte(ibmphp_pci_bus, devfn, PCI_INTERRUPT_PIN, &irq); /* [한국어] 브리지 자신이 어느 인터럽트 핀을 쓰는지 읽는다 */
		if ((irq > 0x00) && (irq < 0x05)) /* [한국어] 유효한 핀 번호(1~4)이면 */
			pci_bus_write_config_byte(ibmphp_pci_bus, devfn, PCI_INTERRUPT_LINE, func->irq[irq - 1]); /* [한국어] 배정된 IRQ 를 라인 레지스터에 적는다 */
		/*
		pci_bus_write_config_byte(ibmphp_pci_bus, devfn, PCI_BRIDGE_CONTROL, ctrl);
		pci_bus_write_config_byte(ibmphp_pci_bus, devfn, PCI_BRIDGE_CONTROL, PCI_BRIDGE_CTL_PARITY);
		pci_bus_write_config_byte(ibmphp_pci_bus, devfn, PCI_BRIDGE_CONTROL, PCI_BRIDGE_CTL_SERR);
		 */

		pci_bus_write_config_word(ibmphp_pci_bus, devfn, PCI_COMMAND, DEVICEENABLE); /* [한국어] 명령 레지스터를 켜 브리지를 활성화한다 */
		pci_bus_write_config_word(ibmphp_pci_bus, devfn, PCI_BRIDGE_CONTROL, 0x07); /* [한국어] 브리지 제어에 0x07 을 쓴다 — 패리티 오류 응답, SERR 켜기, ISA 인식 세 비트다 */
		for (i = 0; i < 32; i++) { /* [한국어] 2차 버스의 장치 슬롯 32개를 */
			if (amount_needed->devices[i]) { /* [한국어] 뒤쪽 훑기에서 장치가 발견된 자리마다 */
				debug("device where devices[i] is 1 = %x\n", i); /* [한국어] 어느 자리인지 남기고 */
				func->devices[i] = 1; /* [한국어] 함수 구조체의 장치 비트맵에 표시한다 — 나중에 제거할 때 이 비트맵으로 뒤쪽 장치를 되짚는다 */
			}
		}
		func->bus = 1;	/* For unconfiguring, to indicate it's PPB */ /* [한국어] 브리지(PPB)임을 표시한다. ibmphp_unconfigure_card() 가 BAR 개수를 2 로 세는 근거다 */
		func_passed = &func; /* [한국어] 상류 코드가 지역 변수 func_passed 에 다시 대입한다 — 호출자에게는 보이지 않으므로 실제 효과가 없다 */
		debug("func->busno b4 returning is %x\n", func->busno); /* [한국어] 돌려주기 직전 버스 번호를 남긴다 */
		debug("func->busno b4 returning in the other structure is %x\n", (*func_passed)->busno); /* [한국어] 같은 값을 포인터 경유로도 남긴다 */
		kfree(amount_needed); /* [한국어] 요구량 구조체를 버리고 */
		return 0; /* [한국어] 성공으로 돌아간다 */
	} else { /* [한국어] 셋 중 하나라도 준비되지 않았으면 */
		err("Configuring bridge was unsuccessful...\n"); /* [한국어] 실패를 알리고 */
		mem_tmp = NULL; /* [한국어] 임시 노드는 이미 장부에 들어갔으므로 아래에서 두 번 지우지 않도록 끊는다 */
		retval = -EIO; /* [한국어] 입출력 오류로 */
		goto error; /* [한국어] 공통 정리 경로로 간다 */
	}

error: /* [한국어] **공통 정리 경로** — 여기까지 잡아 둔 자원을 모두 되돌린다 */
	kfree(amount_needed); /* [한국어] 요구량 구조체를 버린다 */
	if (pfmem) /* [한국어] 브리지 프리페치 창을 잡아 두었으면 */
		ibmphp_remove_resource(pfmem); /* [한국어] 장부에서 지운다 */
	if (io) /* [한국어] 브리지 I/O 창을 잡아 두었으면 */
		ibmphp_remove_resource(io); /* [한국어] 장부에서 지운다 */
	if (mem) /* [한국어] 브리지 메모리 창을 잡아 두었으면 */
		ibmphp_remove_resource(mem); /* [한국어] 장부에서 지운다 */
	for (i = 0; i < 2; i++) {	/* for 2 BARs */ /* [한국어] 브리지 자신의 BAR 두 개도 되돌린다 */
		if (bus_io[i]) { /* [한국어] I/O 를 잡아 두었으면 */
			ibmphp_remove_resource(bus_io[i]); /* [한국어] 장부에서 지우고 */
			func->io[i] = NULL; /* [한국어] 함수 구조체의 포인터를 끊는다 */
		} else if (bus_pfmem[i]) { /* [한국어] 프리페치를 잡아 두었으면 */
			ibmphp_remove_resource(bus_pfmem[i]); /* [한국어] 장부에서 지우고 */
			func->pfmem[i] = NULL; /* [한국어] 포인터를 끊는다 */
		} else if (bus_mem[i]) { /* [한국어] 일반 메모리를 잡아 두었으면 */
			ibmphp_remove_resource(bus_mem[i]); /* [한국어] 장부에서 지우고 */
			func->mem[i] = NULL; /* [한국어] 포인터를 끊는다 */
		}
	}
	return retval; /* [한국어] 담아 둔 오류 코드를 돌려준다 */
}

/*****************************************************************************
 * This function adds up the amount of resources needed behind the PPB bridge
 * and passes it to the configure_bridge function
 * Input: bridge function
 * Output: amount of resources needed
 *****************************************************************************/
/* [한국어]
 * scan_behind_bridge - 브리지 뒤 장치들이 필요한 자원 총량을 미리 잰다
 *
 * @func:  브리지 함수.
 * @busno: 그 브리지의 2차 버스 번호.
 * @return: 종류별 총량이 담긴 struct res_needed. 할당 실패면 NULL.
 *
 * 바로 위 상류 주석대로 브리지 뒤에 필요한 자원의 양을 더해 configure_bridge()
 * 에 넘긴다.
 *
 * **BAR 에 값을 쓰지 않고 크기만 잰다**는 점이 configure_device() 와 다르다.
 * 길이 탐침(0xFFFFFFFF 쓰고 되읽기)은 하지만 결과 주소를 써 넣지 않고
 * 합계에만 더한다. 브리지 창을 먼저 잡아야 그 안에서 개별 BAR 를 배정할 수
 * 있기 때문이다 — 즉 2단계 배정의 1단계다.
 *
 * 장치 32 x 함수 8 을 모두 훑는 완전 탐색이다. 훑다가 셋 중 하나를 만나면
 * not_correct 를 세우고 **그 자리에서 돌려준다**.
 *   - 브리지가 또 있으면 — 상류 메시지대로 중첩 브리지는 지원하지 않는다.
 *   - VGA 호환 장치이면 — 핫플러그를 지원하지 않는다.
 *   - 장치를 하나도 못 찾았으면(howmany 가 0) — 마지막에 세운다.
 *
 * devices[] 배열에 어느 장치 번호가 살아 있는지 표시해 두는데, 그것을
 * configure_bridge() 가 func->devices[] 로 베끼고 ibmphp_configure_card() 가
 * 그 배열을 보고 재귀 호출 대상을 정한다.
 *
 * 마지막 세 줄이 하한을 건다. 총합이 0 이 아니면서 브리지 최소 단위보다
 * 작으면 그 단위로 올린다 — 브리지 창은 IO 4KB, (PF)MEM 1MB 아래로 잘게
 * 나눌 수 없기 때문이다.
 *
 * [관찰] 합계가 단순 덧셈이라 각 BAR 의 정렬 요구로 생기는 틈을 고려하지
 * 않는다. 그래서 실제로는 이 총량으로 잡은 창 안에 개별 BAR 가 다 들어가지
 * 못할 수 있는데, configure_bridge() 의 상류 주석이 그 재시도를 TO DO 로
 * 남겨 두었다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(카드 삽입).
 *
 * 호출 체인:  configure_bridge() → [이 함수]
 */
static struct res_needed *scan_behind_bridge(struct pci_func *func, u8 busno)
{
	int count, len[6]; /* [한국어] BAR 루프 인덱스와 BAR 여섯의 요구 길이 */
	u16 vendor_id; /* [한국어] 벤더 ID — 장치가 있는지 판별하는 값 */
	u8 hdr_type; /* [한국어] 헤더 타입 — 브리지인지 판별하는 값 */
	u8 device, function; /* [한국어] 훑는 중인 장치·함수 번호 */
	unsigned int devfn; /* [한국어] config 접근에 쓸 장치·함수 번호 */
	int howmany = 0;	/*this is to see if there are any devices behind the bridge */ /* [한국어] 브리지 뒤에서 장치를 하나라도 찾았는지 세는 값 */

	u32 bar[6], class; /* [한국어] BAR 여섯의 탐침 결과와 클래스 코드 */
	static const u32 address[] = { /* [한국어] BAR 여섯의 config 공간 오프셋 */
		PCI_BASE_ADDRESS_0, /* [한국어] BAR 0 */
		PCI_BASE_ADDRESS_1, /* [한국어] BAR 1 */
		PCI_BASE_ADDRESS_2, /* [한국어] BAR 2 */
		PCI_BASE_ADDRESS_3, /* [한국어] BAR 3 */
		PCI_BASE_ADDRESS_4, /* [한국어] BAR 4 */
		PCI_BASE_ADDRESS_5, /* [한국어] BAR 5 */
		0 /* [한국어] 끝 표시 */
	};
	struct res_needed *amount; /* [한국어] 호출자에게 돌려줄 요구량 구조체 */

	amount = kzalloc_obj(*amount); /* [한국어] 요구량 구조체를 잡는다. kzalloc 이라 세 총량과 비트맵이 모두 0 으로 시작한다 */
	if (amount == NULL) /* [한국어] 메모리가 없으면 */
		return NULL; /* [한국어] NULL 로 알린다 — 호출자가 -ENOMEM 으로 바꾼다 */

	ibmphp_pci_bus->number = busno; /* [한국어] **껍데기 버스의 번호를 2차 버스로 바꾼다**. 이 한 줄로 이후 config 접근이 브리지 뒤로 향한다 */

	debug("the bus_no behind the bridge is %x\n", busno); /* [한국어] 어느 버스를 훑는지 남긴다 */
	debug("scanning devices behind the bridge...\n"); /* [한국어] 훑기 시작을 남긴다 */
	for (device = 0; device < 32; device++) { /* [한국어] 2차 버스의 장치 자리 32개를 모두 본다 */
		amount->devices[device] = 0; /* [한국어] 해당 자리를 아직 비어 있는 것으로 둔다 */
		for (function = 0; function < 8; function++) { /* [한국어] 각 장치의 함수 8개를 모두 본다 */
			devfn = PCI_DEVFN(device, function); /* [한국어] config 접근용 번호를 만든다 */

			pci_bus_read_config_word(ibmphp_pci_bus, devfn, PCI_VENDOR_ID, &vendor_id); /* [한국어] 벤더 ID 를 읽는다 — 응답이 없으면 0xffff 가 읽힌다 */

			if (vendor_id != PCI_VENDOR_ID_NOTVALID) { /* [한국어] 유효한 벤더 ID 이면 장치가 실재한다 */
				/* found correct device!!! */
				howmany++; /* [한국어] 찾은 개수를 늘린다 */

				pci_bus_read_config_byte(ibmphp_pci_bus, devfn, PCI_HEADER_TYPE, &hdr_type); /* [한국어] 헤더 타입을 읽는다 */
				pci_bus_read_config_dword(ibmphp_pci_bus, devfn, PCI_CLASS_REVISION, &class); /* [한국어] 클래스 코드와 리비전을 한 워드로 읽는다 */

				debug("hdr_type behind the bridge is %x\n", hdr_type); /* [한국어] 어떤 헤더인지 남긴다 */
				if ((hdr_type & PCI_HEADER_TYPE_MASK) == PCI_HEADER_TYPE_BRIDGE) { /* [한국어] 뒤에 또 브리지가 있으면 */
					err("embedded bridges not supported for hot-plugging.\n"); /* [한국어] 지원하지 않는다고 알리고 */
					amount->not_correct = 1; /* [한국어] 구성이 부적합하다고 표시한 뒤 */
					return amount; /* [한국어] 그대로 돌려준다 — 호출자가 이 표시를 보고 전체를 되돌린다 */
				}

				class >>= 8;	/* to take revision out, class = class.subclass.prog i/f */ /* [한국어] 하위 8비트가 리비전이므로 밀어내면 클래스·서브클래스·프로그래밍 인터페이스만 남는다 */
				if (class == PCI_CLASS_NOT_DEFINED_VGA) { /* [한국어] VGA 호환 장치이면 */
					err("The device %x is VGA compatible and as is not supported for hot plugging.  Please choose another device.\n", device); /* [한국어] 다른 장치를 고르라고 알리고 */
					amount->not_correct = 1; /* [한국어] 구성이 부적합하다고 표시한 뒤 */
					return amount; /* [한국어] 그대로 돌려준다 */
				} else if (class == PCI_CLASS_DISPLAY_VGA) { /* [한국어] 디스플레이 VGA 장치도 마찬가지로 */
					err("The device %x is not supported for hot plugging.  Please choose another device.\n", device); /* [한국어] 지원하지 않는다고 알리고 */
					amount->not_correct = 1; /* [한국어] 구성이 부적합하다고 표시한 뒤 */
					return amount; /* [한국어] 그대로 돌려준다 */
				}

				amount->devices[device] = 1; /* [한국어] 이 자리에 장치가 있다고 비트맵에 표시한다 */

				for (count = 0; address[count]; count++) { /* [한국어] BAR 여섯을 차례로 본다 */
					/* for 6 BARs */
					/*
					pci_bus_read_config_byte(ibmphp_pci_bus, devfn, address[count], &tmp);
					if (tmp & 0x01) // IO
						pci_bus_write_config_dword(ibmphp_pci_bus, devfn, address[count], 0xFFFFFFFD);
					else // MEMORY
						pci_bus_write_config_dword(ibmphp_pci_bus, devfn, address[count], 0xFFFFFFFF);
					*/
					pci_bus_write_config_dword(ibmphp_pci_bus, devfn, address[count], 0xFFFFFFFF); /* [한국어] 길이 탐침 — 모든 비트를 1 로 써 본다 */
					pci_bus_read_config_dword(ibmphp_pci_bus, devfn, address[count], &bar[count]); /* [한국어] 되읽어 요구 크기를 알아낸다 */

					debug("what is bar[count]? %x, count = %d\n", bar[count], count); /* [한국어] 무엇이 읽혔는지 남긴다 */

					if (!bar[count])	/* This BAR is not implemented */ /* [한국어] 되읽은 값이 0 이면 구현되지 않은 BAR 다 */
						continue; /* [한국어] 다음 BAR 로 */

					//tmp_bar = bar[count];

					debug("count %d device %x function %x wants %x resources\n", count, device, function, bar[count]); /* [한국어] 어느 장치가 무엇을 요구하는지 남긴다 */

					if (bar[count] & PCI_BASE_ADDRESS_SPACE_IO) { /* [한국어] 최하위 비트가 서 있으면 I/O 공간이다 */
						/* This is IO */
						len[count] = bar[count] & 0xFFFFFFFC; /* [한국어] 하위 2비트 플래그를 지운다 */
						len[count] = ~len[count] + 1; /* [한국어] 뒤집고 1 을 더해 요구 크기를 구한다 */
						amount->io += len[count]; /* [한국어] **총량에 그냥 더한다** — 정렬이나 겹침을 따지지 않는 단순 합이다 */
					} else {
						/* This is Memory */
						if (bar[count] & PCI_BASE_ADDRESS_MEM_PREFETCH) { /* [한국어] 프리페치 비트가 서 있으면 PFMEM 이다 */
							/* pfmem */
							len[count] = bar[count] & 0xFFFFFFF0; /* [한국어] 하위 4비트 플래그를 지운다 */
							len[count] = ~len[count] + 1; /* [한국어] 뒤집고 1 을 더해 요구 크기를 구한다 */
							amount->pfmem += len[count]; /* [한국어] 프리페치 총량에 더한다 */
							if (bar[count] & PCI_BASE_ADDRESS_MEM_TYPE_64) /* [한국어] 64비트 BAR 이면 */
								/* takes up another dword */
								count += 1; /* [한국어] 다음 워드를 건너뛴다 */

						} else {
							/* regular memory */
							len[count] = bar[count] & 0xFFFFFFF0; /* [한국어] 프리페치가 아닌 일반 메모리. 하위 4비트 플래그를 지운다 */
							len[count] = ~len[count] + 1; /* [한국어] 뒤집고 1 을 더해 요구 크기를 구한다 */
							amount->mem += len[count]; /* [한국어] 메모리 총량에 더한다 */
							if (bar[count] & PCI_BASE_ADDRESS_MEM_TYPE_64) { /* [한국어] 64비트 BAR 이면 */
								/* takes up another dword */
								count += 1; /* [한국어] 다음 워드를 건너뛴다 */
							}
						}
					}
				}	/* end for */ /* [한국어] BAR 여섯 훑기 끝 */
			}	/* end if (valid) */ /* [한국어] 유효한 함수 처리 끝 */
		}	/* end for */
	}	/* end for */

	if (!howmany) /* [한국어] 브리지 뒤에서 장치를 하나도 못 찾았으면 */
		amount->not_correct = 1; /* [한국어] 구성이 부적합하다고 표시한다 — 빈 브리지에 창을 열 이유가 없다 */
	else /* [한국어] 하나라도 찾았으면 */
		amount->not_correct = 0; /* [한국어] 구성이 적합하다고 표시한다 */
	if ((amount->io) && (amount->io < IOBRIDGE)) /* [한국어] I/O 를 쓰는데 그 합이 브리지 최소 단위보다 작으면 */
		amount->io = IOBRIDGE; /* [한국어] 브리지 I/O 창의 최소 크기(4KB)로 올린다 — 규격상 그보다 잘게 열 수 없다 */
	if ((amount->mem) && (amount->mem < MEMBRIDGE)) /* [한국어] 메모리를 쓰는데 그 합이 최소 단위보다 작으면 */
		amount->mem = MEMBRIDGE; /* [한국어] 브리지 메모리 창의 최소 크기(1MB)로 올린다 */
	if ((amount->pfmem) && (amount->pfmem < MEMBRIDGE)) /* [한국어] 프리페치를 쓰는데 그 합이 최소 단위보다 작으면 */
		amount->pfmem = MEMBRIDGE; /* [한국어] 같은 최소 크기로 올린다 */
	return amount; /* [한국어] 채운 요구량을 돌려준다 */
}

/* The following 3 unconfigure_boot_ routines deal with the case when we had the card
 * upon bootup in the system, since we don't allocate func to such case, we need to read
 * the start addresses from pci config space and then find the corresponding entries in
 * our resource lists.  The functions return either 0, -ENODEV, or -1 (general failure)
 * Change: we also call these functions even if we configured the card ourselves (i.e., not
 * the bootup case), since it should work same way
 */
/* [한국어]
 * unconfigure_boot_device - 장치가 쓰던 자원을 config 공간에서 읽어 장부에서 지운다
 *
 * @busno:    버스 번호.
 * @device:   장치 번호.
 * @function: 함수 번호.
 * @return: 0 성공, -EINVAL 은 버스를 못 찾은 경우, -EIO 는 대응 자원을 못
 *          찾은 경우.
 *
 * 바로 위 상류 주석이 이 셋(boot_device / boot_bridge / boot_card)의 배경을
 * 밝힌다 — 부팅 때부터 꽂혀 있던 카드는 이 드라이버가 pci_func 를 만들어 두지
 * 않았으므로, **config 공간에서 시작 주소를 직접 읽어** 장부에서 대응 항목을
 * 찾아야 한다. 뒤에 덧붙은 문장대로 이 드라이버가 직접 꽂은 카드에도 같은
 * 방식을 쓴다.
 *
 * BAR 마다 절차가 이렇다.
 *   1) 현재 값을 읽어 두고, 길이 탐침을 한 뒤 **원래 값을 되돌려 쓴다.**
 *      상류 주석대로 이 시점에는 카드의 장치 드라이버가 이미 멈춰 있어
 *      안전하다.
 *   2) 크기가 0 이면 구현되지 않은 BAR 라 건너뛴다.
 *   3) 시작 주소로 장부에서 자원을 찾아 지운다.
 *
 * I/O 에만 while 루프가 하나 더 있다. 상류 주석대로 **옛 BIOS 의 I/O 제약**
 * 때문에 하나의 BAR 가 장부에서는 여러 조각으로 나뉘어 있을 수 있어, 끝
 * 주소에 닿을 때까지 이어지는 조각을 계속 찾아 지운다.
 *
 * 메모리 쪽은 조각 처리가 없고, 64비트 BAR 이면 다음 워드를 건너뛴다.
 *
 * 상류가 물음표로 남겨 둔 자리가 둘 있다 — 지운 뒤 config 공간에 무엇인가를
 * 되써야 하는지 확신하지 못한다는 뜻이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(카드 제거).
 *
 * 호출 체인:  unconfigure_boot_card() → [이 함수]
 *               → ibmphp_find_resource() → ibmphp_remove_resource()
 */
static int unconfigure_boot_device(u8 busno, u8 device, u8 function)
{
	u32 start_address; /* [한국어] BAR 에 들어 있던 시작 주소 */
	static const u32 address[] = { /* [한국어] BAR 여섯의 config 공간 오프셋 */
		PCI_BASE_ADDRESS_0, /* [한국어] BAR 0 */
		PCI_BASE_ADDRESS_1, /* [한국어] BAR 1 */
		PCI_BASE_ADDRESS_2, /* [한국어] BAR 2 */
		PCI_BASE_ADDRESS_3, /* [한국어] BAR 3 */
		PCI_BASE_ADDRESS_4, /* [한국어] BAR 4 */
		PCI_BASE_ADDRESS_5, /* [한국어] BAR 5 */
		0 /* [한국어] 끝 표시 */
	};
	int count; /* [한국어] BAR 루프 인덱스 */
	struct resource_node *io; /* [한국어] 장부에서 찾아낸 I/O 자원 노드 */
	struct resource_node *mem; /* [한국어] 장부에서 찾아낸 메모리 자원 노드 */
	struct resource_node *pfmem; /* [한국어] 장부에서 찾아낸 프리페치 자원 노드 */
	struct bus_node *bus; /* [한국어] 이 장치가 붙어 있는 버스의 장부 노드 */
	u32 end_address; /* [한국어] 시작 + 크기 - 1 로 구한 끝 주소 */
	u32 temp_end; /* [한국어] 장부에 실제로 등록된 끝 주소 */
	u32 size; /* [한국어] 길이 탐침 결과 */
	u32 tmp_address; /* [한국어] 플래그 비트를 지운 시작 주소 */
	unsigned int devfn; /* [한국어] config 접근에 쓸 장치·함수 번호 */

	debug("%s - enter\n", __func__); /* [한국어] 진입 기록 */

	bus = ibmphp_find_res_bus(busno); /* [한국어] 이 버스의 장부 노드를 찾는다 */
	if (!bus) { /* [한국어] 없으면 이 드라이버가 모르는 버스다 */
		debug("cannot find corresponding bus.\n"); /* [한국어] 그 사실을 남기고 */
		return -EINVAL; /* [한국어] 인자 오류로 돌아간다 */
	}

	devfn = PCI_DEVFN(device, function); /* [한국어] config 접근용 번호를 만든다 */
	ibmphp_pci_bus->number = busno; /* [한국어] 껍데기 버스의 번호를 이 장치의 버스로 맞춘다 */
	for (count = 0; address[count]; count++) {	/* for 6 BARs */ /* [한국어] BAR 여섯을 차례로 본다 */
		pci_bus_read_config_dword(ibmphp_pci_bus, devfn, address[count], &start_address); /* [한국어] **부팅 때 BIOS 가 넣어 둔 시작 주소를 읽는다** — 이 값이 장부에서 지울 자리를 가리킨다 */

		/* We can do this here, b/c by that time the device driver of the card has been stopped */

		pci_bus_write_config_dword(ibmphp_pci_bus, devfn, address[count], 0xFFFFFFFF); /* [한국어] 길이 탐침 — 모든 비트를 1 로 써 본다 */
		pci_bus_read_config_dword(ibmphp_pci_bus, devfn, address[count], &size); /* [한국어] 되읽어 크기를 알아낸다 */
		pci_bus_write_config_dword(ibmphp_pci_bus, devfn, address[count], start_address); /* [한국어] **읽어 둔 원래 주소를 곧바로 되돌려 쓴다** — 탐침이 BAR 를 망가뜨린 채로 두지 않는다 */

		debug("start_address is %x\n", start_address); /* [한국어] 읽은 시작 주소를 남긴다 */
		debug("busno, device, function %x %x %x\n", busno, device, function); /* [한국어] 어느 장치인지 남긴다 */
		if (!size) { /* [한국어] 되읽은 값이 0 이면 */
			/* This BAR is not implemented */
			debug("is this bar no implemented?, count = %d\n", count); /* [한국어] 구현되지 않은 BAR 임을 남기고 */
			continue; /* [한국어] 다음 BAR 로 */
		}
		tmp_address = start_address; /* [한국어] 플래그 비트가 살아 있는 원본을 따로 둔다 — 아래에서 64비트 여부를 판별할 때 쓴다 */
		if (start_address & PCI_BASE_ADDRESS_SPACE_IO) { /* [한국어] 최하위 비트가 서 있으면 I/O 공간이다 */
			/* This is IO */
			start_address &= PCI_BASE_ADDRESS_IO_MASK; /* [한국어] 하위 2비트 플래그를 지워 실제 주소만 남긴다 */
			size = size & 0xFFFFFFFC; /* [한국어] 탐침 값에서도 플래그를 지운다 */
			size = ~size + 1; /* [한국어] 뒤집고 1 을 더해 크기를 구한다 */
			end_address = start_address + size - 1; /* [한국어] 시작 + 크기 - 1 이 이 BAR 가 차지한 끝 주소다 */
			if (ibmphp_find_resource(bus, start_address, &io, IO)) /* [한국어] 그 시작 주소로 장부에서 I/O 노드를 찾는다 */
				goto report_search_failure; /* [한국어] 못 찾으면 공통 실패 경로로 간다 */

			debug("io->start = %x\n", io->start); /* [한국어] 찾은 노드의 시작을 남긴다 */
			temp_end = io->end; /* [한국어] 장부에 등록된 끝을 기억해 둔다 */
			start_address = io->end + 1; /* [한국어] 다음 조각을 찾을 시작점을 그 다음 바이트로 옮긴다 */
			ibmphp_remove_resource(io); /* [한국어] 찾은 조각을 장부에서 지운다 */
			/* This is needed b/c of the old I/O restrictions in the BIOS */
			while (temp_end < end_address) { /* [한국어] **장부의 조각이 BAR 하나보다 잘게 쪼개져 있을 수 있다** — 상류 주석대로 옛 BIOS 의 I/O 제약 때문이다. 끝까지 덮을 때까지 이어서 지운다 */
				if (ibmphp_find_resource(bus, start_address, /* [한국어] 다음 조각을 찾는다 */
							 &io, IO))
					goto report_search_failure; /* [한국어] 못 찾으면 공통 실패 경로로 간다 */

				debug("io->start = %x\n", io->start); /* [한국어] 찾은 조각의 시작을 남긴다 */
				temp_end = io->end; /* [한국어] 그 조각의 끝을 기억하고 */
				start_address = io->end + 1; /* [한국어] 다음 시작점을 옮긴 뒤 */
				ibmphp_remove_resource(io); /* [한국어] 그 조각도 지운다 */
			}

			/* ????????? DO WE NEED TO WRITE ANYTHING INTO THE PCI CONFIG SPACE BACK ?????????? */
		} else { /* [한국어] 최하위 비트가 0 이면 메모리 공간이다 */
			/* This is Memory */
			if (start_address & PCI_BASE_ADDRESS_MEM_PREFETCH) { /* [한국어] 프리페치 비트가 서 있으면 PFMEM 이다 */
				/* pfmem */
				debug("start address of pfmem is %x\n", start_address); /* [한국어] 어느 주소인지 남기고 */
				start_address &= PCI_BASE_ADDRESS_MEM_MASK; /* [한국어] 하위 4비트 플래그를 지워 실제 주소만 남긴다 */

				if (ibmphp_find_resource(bus, start_address, &pfmem, PFMEM) < 0) { /* [한국어] 그 시작 주소로 장부에서 프리페치 노드를 찾는다 */
					err("cannot find corresponding PFMEM resource to remove\n"); /* [한국어] 못 찾으면 알리고 */
					return -EIO; /* [한국어] 입출력 오류로 돌아간다 */
				}
				if (pfmem) { /* [한국어] 찾았으면 */
					debug("pfmem->start = %x\n", pfmem->start); /* [한국어] 그 시작을 남기고 */

					ibmphp_remove_resource(pfmem); /* [한국어] 장부에서 지운다 */
				}
			} else {
				/* regular memory */
				debug("start address of mem is %x\n", start_address); /* [한국어] 프리페치가 아닌 일반 메모리. 어느 주소인지 남기고 */
				start_address &= PCI_BASE_ADDRESS_MEM_MASK; /* [한국어] 하위 4비트 플래그를 지운다 */

				if (ibmphp_find_resource(bus, start_address, &mem, MEM) < 0) { /* [한국어] 그 시작 주소로 장부에서 메모리 노드를 찾는다 */
					err("cannot find corresponding MEM resource to remove\n"); /* [한국어] 못 찾으면 알리고 */
					return -EIO; /* [한국어] 입출력 오류로 돌아간다 */
				}
				if (mem) { /* [한국어] 찾았으면 */
					debug("mem->start = %x\n", mem->start); /* [한국어] 그 시작을 남기고 */

					ibmphp_remove_resource(mem); /* [한국어] 장부에서 지운다 */
				}
			}
			if (tmp_address & PCI_BASE_ADDRESS_MEM_TYPE_64) { /* [한국어] 원본 BAR 값이 64비트였으면 */
				/* takes up another dword */
				count += 1; /* [한국어] 다음 워드를 건너뛴다 */
			}
		}	/* end of mem */
	}	/* end of for */

	return 0; /* [한국어] 여섯 BAR 를 모두 정리했으면 성공 */

report_search_failure: /* [한국어] **공통 실패 경로** — I/O 조각을 끝까지 찾지 못한 경우 */
	err("cannot find corresponding IO resource to remove\n"); /* [한국어] 무엇을 못 찾았는지 알리고 */
	return -EIO; /* [한국어] 입출력 오류로 돌아간다 */
}

/* [한국어]
 * unconfigure_boot_bridge - 브리지가 쓰던 자원과 그 뒤 버스를 통째로 지운다
 *
 * @busno:    버스 번호.
 * @device:   장치 번호.
 * @function: 함수 번호.
 * @return: 0 성공, -EINVAL 은 번호가 어긋나거나 자원을 못 찾은 경우,
 *          -ENODEV 는 뒤에 버스가 여럿인 경우, -EIO 는 I/O 자원을 못 찾은 경우.
 *
 * unconfigure_boot_device() 의 브리지 판이며, 앞에 검증이 셋 붙는다.
 *
 *   1) config 공간의 1차 버스 번호가 인자와 같은지 본다. 다르면 장부와
 *      하드웨어가 어긋난 것이다.
 *   2) 2차와 subordinate 가 같은지 본다. 다르면 상류 메시지대로 **뒤에 버스가
 *      여럿**이라는 뜻이고, 이 드라이버는 그런 구성의 제거를 지원하지 않는다.
 *   3) 2차 버스가 장부에 있는지 본다.
 *
 * 그다음 ibmphp_remove_bus() 로 **그 버스와 창, 그리고 부모 쪽 대응 자원까지
 * 한꺼번에** 지운다. 이것이 이 함수의 핵심이며, 장치 판에는 없는 단계다.
 *
 * 마지막으로 브리지 자신의 BAR 둘을 장치 판과 같은 방식으로 지운다.
 *
 * [관찰] BAR 를 지울 때 넘기는 버스가 `bus`(2차 버스)인데, 그 버스는 바로 위
 * ibmphp_remove_bus() 에서 이미 해제되었다. 게다가 브리지 자신의 BAR 는
 * 2차가 아니라 1차 버스의 장부에 올라가 있다(configure_bridge() 가
 * `bus_io[count]->busno = func->busno` 로 등록한다). 상류 코드 그대로이며
 * 여기서는 고치지 않는다.
 *
 * [관찰] 장치 판과 달리 길이 탐침을 하지 않는다. 시작 주소가 0 인지만 보고
 * 구현 여부를 판단한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(카드 제거).
 *
 * 호출 체인:  unconfigure_boot_card() → [이 함수]
 *               → ibmphp_remove_bus() → ibmphp_find_resource()
 *               → ibmphp_remove_resource()
 */
static int unconfigure_boot_bridge(u8 busno, u8 device, u8 function)
{
	int count; /* [한국어] BAR 루프 인덱스 */
	int bus_no, pri_no, sub_no, sec_no = 0; /* [한국어] 버스 번호들을 int 로 비교하기 위한 사본 */
	u32 start_address, tmp_address; /* [한국어] BAR 에 들어 있던 시작 주소와 플래그가 살아 있는 사본 */
	u8 sec_number, sub_number, pri_number; /* [한국어] 브리지에서 읽어 낸 2차·하위·1차 버스 번호 */
	struct resource_node *io = NULL; /* [한국어] 장부에서 찾아낸 I/O 자원 노드 */
	struct resource_node *mem = NULL; /* [한국어] 장부에서 찾아낸 메모리 자원 노드 */
	struct resource_node *pfmem = NULL; /* [한국어] 장부에서 찾아낸 프리페치 자원 노드 */
	struct bus_node *bus; /* [한국어] 브리지 뒤 2차 버스의 장부 노드 */
	static const u32 address[] = { /* [한국어] 브리지의 BAR 오프셋 표. 헤더 타입 1 은 BAR 가 둘뿐이다 */
		PCI_BASE_ADDRESS_0, /* [한국어] BAR 0 */
		PCI_BASE_ADDRESS_1, /* [한국어] BAR 1 */
		0 /* [한국어] 끝 표시 */
	};
	unsigned int devfn; /* [한국어] config 접근에 쓸 장치·함수 번호 */

	devfn = PCI_DEVFN(device, function); /* [한국어] config 접근용 번호를 만든다 */
	ibmphp_pci_bus->number = busno; /* [한국어] 껍데기 버스의 번호를 이 브리지가 붙어 있는 버스로 맞춘다 */
	bus_no = (int) busno; /* [한국어] 인자로 받은 버스 번호를 int 로 둔다 */
	debug("busno is %x\n", busno); /* [한국어] 어느 버스인지 남긴다 */
	pci_bus_read_config_byte(ibmphp_pci_bus, devfn, PCI_PRIMARY_BUS, &pri_number); /* [한국어] 브리지에 적혀 있는 1차 버스 번호를 읽는다 */
	debug("%s - busno = %x, primary_number = %x\n", __func__, busno, pri_number); /* [한국어] 읽은 값을 남긴다 */

	pci_bus_read_config_byte(ibmphp_pci_bus, devfn, PCI_SECONDARY_BUS, &sec_number); /* [한국어] 2차 버스 번호를 읽는다 */
	debug("sec_number is %x\n", sec_number); /* [한국어] 읽은 값을 남긴다 */
	sec_no = (int) sec_number; /* [한국어] int 로 바꿔 두고 */
	pri_no = (int) pri_number; /* [한국어] 1차 번호도 int 로 바꾼다 */
	if (pri_no != bus_no) { /* [한국어] **드라이버가 아는 버스 번호와 하드웨어에 적힌 1차 번호가 다르면** 상태가 어긋난 것이다 */
		err("primary numbers in our structures and pci config space don't match.\n"); /* [한국어] 그 사실을 알리고 */
		return -EINVAL; /* [한국어] 인자 오류로 돌아간다 */
	}

	pci_bus_read_config_byte(ibmphp_pci_bus, devfn, PCI_SUBORDINATE_BUS, &sub_number); /* [한국어] 하위 버스 번호를 읽는다 */
	sub_no = (int) sub_number; /* [한국어] int 로 바꾼다 */
	debug("sub_no is %d, sec_no is %d\n", sub_no, sec_no); /* [한국어] 두 값을 남긴다 */
	if (sec_no != sub_number) { /* [한국어] **2차와 하위가 다르면 브리지 뒤에 버스가 더 있다는 뜻이다** */
		err("there're more buses behind this bridge.  Hot removal is not supported.  Please choose another card\n"); /* [한국어] 중첩 브리지는 뽑기를 지원하지 않는다고 알리고 */
		return -ENODEV; /* [한국어] 장치 없음으로 돌아간다 */
	}

	bus = ibmphp_find_res_bus(sec_number); /* [한국어] 2차 버스의 장부 노드를 찾는다 */
	if (!bus) { /* [한국어] 없으면 이 드라이버가 모르는 버스다 */
		err("cannot find Bus structure for the bridged device\n"); /* [한국어] 그 사실을 알리고 */
		return -EINVAL; /* [한국어] 인자 오류로 돌아간다 */
	}
	debug("bus->busno is %x\n", bus->busno); /* [한국어] 찾은 버스 번호를 남긴다 */
	debug("sec_number is %x\n", sec_number); /* [한국어] 하드웨어에서 읽은 번호도 남긴다 */

	ibmphp_remove_bus(bus, busno); /* [한국어] **2차 버스를 통째로 걷어낸다** — 그 버스의 범위를 부모 버스의 자유 목록으로 되돌린다 */

	for (count = 0; address[count]; count++) { /* [한국어] 이제 브리지 자신의 BAR 두 개를 정리한다 */
		/* for 2 BARs */
		pci_bus_read_config_dword(ibmphp_pci_bus, devfn, address[count], &start_address); /* [한국어] 부팅 때 BIOS 가 넣어 둔 시작 주소를 읽는다 */

		if (!start_address) { /* [한국어] 0 이면 */
			/* This BAR is not implemented */
			continue; /* [한국어] 구현되지 않은 BAR 이므로 넘어간다 */
		}

		tmp_address = start_address; /* [한국어] 플래그가 살아 있는 원본을 따로 둔다 */

		if (start_address & PCI_BASE_ADDRESS_SPACE_IO) { /* [한국어] 최하위 비트가 서 있으면 I/O 공간이다 */
			/* This is IO */
			start_address &= PCI_BASE_ADDRESS_IO_MASK; /* [한국어] 하위 2비트 플래그를 지운다 */
			if (ibmphp_find_resource(bus, start_address, &io, IO) < 0) { /* [한국어] 그 시작 주소로 장부에서 I/O 노드를 찾는다 */
				err("cannot find corresponding IO resource to remove\n"); /* [한국어] 못 찾으면 알리고 */
				return -EIO; /* [한국어] 입출력 오류로 돌아간다 */
			}
			if (io) /* [한국어] 찾았으면 */
				debug("io->start = %x\n", io->start); /* [한국어] 그 시작을 남긴다 */

			ibmphp_remove_resource(io); /* [한국어] 장부에서 지운다. 여기서는 unconfigure_boot_device() 와 달리 조각 이어 지우기를 하지 않는다 */

			/* ????????? DO WE NEED TO WRITE ANYTHING INTO THE PCI CONFIG SPACE BACK ?????????? */
		} else { /* [한국어] 최하위 비트가 0 이면 메모리 공간이다 */
			/* This is Memory */
			if (start_address & PCI_BASE_ADDRESS_MEM_PREFETCH) { /* [한국어] 프리페치 비트가 서 있으면 PFMEM 이다 */
				/* pfmem */
				start_address &= PCI_BASE_ADDRESS_MEM_MASK; /* [한국어] 하위 4비트 플래그를 지운다 */
				if (ibmphp_find_resource(bus, start_address, &pfmem, PFMEM) < 0) { /* [한국어] 그 시작 주소로 장부에서 프리페치 노드를 찾는다 */
					err("cannot find corresponding PFMEM resource to remove\n"); /* [한국어] 못 찾으면 알리고 */
					return -EINVAL; /* [한국어] 인자 오류로 돌아간다 */
				}
				if (pfmem) { /* [한국어] 찾았으면 */
					debug("pfmem->start = %x\n", pfmem->start); /* [한국어] 그 시작을 남기고 */

					ibmphp_remove_resource(pfmem); /* [한국어] 장부에서 지운다 */
				}
			} else {
	/* regular memory */ /* [한국어] 프리페치가 아닌 일반 메모리. 하위 4비트 플래그를 지운다 */
				start_address &= PCI_BASE_ADDRESS_MEM_MASK;
				if (ibmphp_find_resource(bus, start_address, &mem, MEM) < 0) { /* [한국어] 그 시작 주소로 장부에서 메모리 노드를 찾는다 */
					err("cannot find corresponding MEM resource to remove\n"); /* [한국어] 못 찾으면 알리고 */
					return -EINVAL; /* [한국어] 인자 오류로 돌아간다 */
				}
				if (mem) { /* [한국어] 찾았으면 */
					debug("mem->start = %x\n", mem->start); /* [한국어] 그 시작을 남기고 */

					ibmphp_remove_resource(mem); /* [한국어] 장부에서 지운다 */
				}
			}
			if (tmp_address & PCI_BASE_ADDRESS_MEM_TYPE_64) { /* [한국어] 원본 BAR 값이 64비트였으면 */
				/* takes up another dword */
				count += 1; /* [한국어] 다음 워드를 건너뛴다 */
			}
		}	/* end of mem */
	}	/* end of for */
	debug("%s - exiting, returning success\n", __func__); /* [한국어] 정리를 마쳤음을 남기고 */
	return 0; /* [한국어] 성공으로 돌아간다 */
}

/* [한국어]
 * unconfigure_boot_card - 카드의 모든 함수를 훑어 자원을 되돌린다
 *
 * @slot_cur: 대상 슬롯. device 와 bus 를 여기서 꺼낸다.
 * @return: 0 성공, -ENODEV 는 지원하지 않는 구성, -1 은 알 수 없는 헤더 타입
 *          이거나 장치를 하나도 못 찾은 경우, 그 밖에는 하위 함수의 오류.
 *
 * ibmphp_configure_card() 의 거울상이다. 함수 0~7 을 훑고 헤더 타입으로
 * 갈려 unconfigure_boot_device() 또는 unconfigure_boot_bridge() 를 부른다.
 *
 * 삽입 쪽과 구조가 같지만 **재귀가 없다**는 점이 다르다. 브리지 뒤 장치는
 * 따로 훑지 않는데, unconfigure_boot_bridge() 가 ibmphp_remove_bus() 로
 * 그 버스의 자원을 통째로 지우기 때문이다.
 *
 * VGA 거절과 "PCI-to-PCI 브리지가 아니면 거절" 도 삽입 쪽과 같다.
 *
 * valid_device 로 카드에서 아무것도 못 읽은 경우를 잡는다 — 이미 뽑혔거나
 * 전원이 꺼진 상황이다.
 *
 * [관찰] BRIDGE 와 MULTIBRIDGE 갈래에서 `class >>= 8` 을 한 번 더 하는데,
 * 위에서 이미 리비전을 떼어 냈으므로 이 시점의 class 는 class.subclass 다.
 * 삽입 쪽(ibmphp_configure_card)도 같은 모양이라 두 파일이 일관되어 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(카드 제거).
 *
 * 호출 체인:  ibmphp_unconfigure_card() → [이 함수]
 *               → unconfigure_boot_device() / unconfigure_boot_bridge()
 */
static int unconfigure_boot_card(struct slot *slot_cur)
{
	u16 vendor_id; /* [한국어] 벤더 ID — 함수가 실재하는지 판별하는 값 */
	u32 class; /* [한국어] 클래스 코드 — VGA 인지, PCI-PCI 브리지인지 판별하는 값 */
	u8 hdr_type; /* [한국어] 헤더 타입 — 아래 switch 의 갈래를 정하는 값 */
	u8 device; /* [한국어] 슬롯이 꽂힌 장치 번호 */
	u8 busno; /* [한국어] 슬롯이 꽂힌 버스 번호 */
	u8 function; /* [한국어] 훑는 중인 함수 번호 */
	int rc; /* [한국어] 하위 함수 반환값 임시 보관 */
	unsigned int devfn; /* [한국어] config 접근에 쓸 장치·함수 번호 */
	u8 valid_device = 0x00;	/* To see if we are ever able to find valid device and read it */ /* [한국어] 유효한 함수를 하나라도 찾았는지 세는 값 */

	debug("%s - enter\n", __func__); /* [한국어] 진입 기록 */

	device = slot_cur->device; /* [한국어] 슬롯 구조체에서 장치 번호를 꺼낸다 */
	busno = slot_cur->bus; /* [한국어] 버스 번호도 꺼낸다 */

	debug("b4 for loop, device is %x\n", device); /* [한국어] 루프 직전 장치 번호를 남긴다 */
	/* For every function on the card */
	for (function = 0x0; function < 0x08; function++) { /* [한국어] 카드의 함수 8개를 모두 본다 */
		devfn = PCI_DEVFN(device, function); /* [한국어] config 접근용 번호를 만든다 */
		ibmphp_pci_bus->number = busno; /* [한국어] 껍데기 버스의 번호를 이 카드의 버스로 맞춘다 */

		pci_bus_read_config_word(ibmphp_pci_bus, devfn, PCI_VENDOR_ID, &vendor_id); /* [한국어] 벤더 ID 를 읽는다 — 응답이 없으면 0xffff 가 읽힌다 */

		if (vendor_id != PCI_VENDOR_ID_NOTVALID) { /* [한국어] 유효한 벤더 ID 이면 함수가 실재한다 */
			/* found correct device!!! */
			++valid_device; /* [한국어] 찾은 개수를 늘린다 */

			debug("%s - found correct device\n", __func__); /* [한국어] 찾았음을 남긴다 */

			/* header: x x x x x x x x
			 *         | |___________|=> 1=PPB bridge, 0=normal device, 2=CardBus Bridge
			 *         |_=> 0 = single function device, 1 = multi-function device
			 */

			pci_bus_read_config_byte(ibmphp_pci_bus, devfn, PCI_HEADER_TYPE, &hdr_type); /* [한국어] 헤더 타입을 읽는다 */
			pci_bus_read_config_dword(ibmphp_pci_bus, devfn, PCI_CLASS_REVISION, &class); /* [한국어] 클래스 코드와 리비전을 한 워드로 읽는다 */

			debug("hdr_type %x, class %x\n", hdr_type, class); /* [한국어] 읽은 두 값을 남긴다 */
			class >>= 8;	/* to take revision out, class = class.subclass.prog i/f */ /* [한국어] 하위 8비트가 리비전이므로 밀어낸다 */
			if (class == PCI_CLASS_NOT_DEFINED_VGA) { /* [한국어] VGA 호환 장치이면 */
				err("The device %x function %x is VGA compatible and is not supported for hot removing.  Please choose another device.\n", device, function); /* [한국어] 다른 장치를 고르라고 알리고 */
				return -ENODEV; /* [한국어] 장치 없음으로 돌아간다 */
			} else if (class == PCI_CLASS_DISPLAY_VGA) { /* [한국어] 디스플레이 VGA 장치도 마찬가지로 */
				err("The device %x function %x is not supported for hot removing.  Please choose another device.\n", device, function); /* [한국어] 지원하지 않는다고 알리고 */
				return -ENODEV; /* [한국어] 장치 없음으로 돌아간다 */
			}

			switch (hdr_type) { /* [한국어] 헤더 타입에 따라 갈래를 나눈다 */
				case PCI_HEADER_TYPE_NORMAL: /* [한국어] 단일 함수 일반 장치 */
					rc = unconfigure_boot_device(busno, device, function); /* [한국어] 그 장치의 자원을 장부에서 걷어낸다 */
					if (rc) { /* [한국어] 실패하면 */
						err("was not able to unconfigure device %x func %x on bus %x. bailing out...\n", /* [한국어] 무엇이 안 되었는지 알리고 */
						     device, function, busno);
						return rc; /* [한국어] 그 오류를 그대로 돌려준다 */
					}
					function = 0x8; /* [한국어] 단일 함수이므로 나머지 함수는 볼 필요가 없다 — 루프 변수를 끝값으로 밀어 종료시킨다 */
					break; /* [한국어] 갈래를 벗어난다 */
				case PCI_HEADER_TYPE_MULTIDEVICE: /* [한국어] 다중 함수 장치 */
					rc = unconfigure_boot_device(busno, device, function); /* [한국어] 그 함수의 자원을 걷어낸다 */
					if (rc) { /* [한국어] 실패하면 */
						err("was not able to unconfigure device %x func %x on bus %x. bailing out...\n", /* [한국어] 무엇이 안 되었는지 알리고 */
						     device, function, busno);
						return rc; /* [한국어] 그 오류를 그대로 돌려준다 */
					}
					break; /* [한국어] 다중 함수이므로 다음 함수도 계속 본다 */
				case PCI_HEADER_TYPE_BRIDGE: /* [한국어] 단일 함수 브리지 */
					class >>= 8; /* [한국어] 클래스를 8비트 더 밀어 상위 클래스·서브클래스만 남긴다 */
					if (class != PCI_CLASS_BRIDGE_PCI) { /* [한국어] PCI-PCI 브리지가 아니면 */
						err("This device %x function %x is not PCI-to-PCI bridge, and is not supported for hot-removing.  Please try another card.\n", device, function); /* [한국어] 지원하지 않는다고 알리고 */
						return -ENODEV; /* [한국어] 장치 없음으로 돌아간다 */
					}
					rc = unconfigure_boot_bridge(busno, device, function); /* [한국어] 2차 버스와 브리지 자신의 자원을 걷어낸다 */
					if (rc != 0) { /* [한국어] 실패하면 */
						err("was not able to hot-remove PPB properly.\n"); /* [한국어] 그 사실을 알리고 */
						return rc; /* [한국어] 오류를 돌려준다 */
					}

					function = 0x8; /* [한국어] 단일 함수이므로 루프를 끝낸다 */
					break; /* [한국어] 갈래를 벗어난다 */
				case PCI_HEADER_TYPE_MULTIBRIDGE: /* [한국어] 다중 함수 브리지 */
					class >>= 8; /* [한국어] 클래스를 8비트 더 민다 */
					if (class != PCI_CLASS_BRIDGE_PCI) { /* [한국어] PCI-PCI 브리지가 아니면 */
						err("This device %x function %x is not PCI-to-PCI bridge,  and is not supported for hot-removing.  Please try another card.\n", device, function); /* [한국어] 지원하지 않는다고 알리고 */
						return -ENODEV; /* [한국어] 장치 없음으로 돌아간다 */
					}
					rc = unconfigure_boot_bridge(busno, device, function); /* [한국어] 2차 버스와 브리지 자신의 자원을 걷어낸다 */
					if (rc != 0) { /* [한국어] 실패하면 */
						err("was not able to hot-remove PPB properly.\n"); /* [한국어] 그 사실을 알리고 */
						return rc; /* [한국어] 오류를 돌려준다 */
					}
					break; /* [한국어] 다중 함수이므로 다음 함수도 계속 본다 */
				default: /* [한국어] 알 수 없는 헤더 타입 */
					err("MAJOR PROBLEM!!!! Cannot read device's header\n"); /* [한국어] 읽을 수 없다고 알리고 */
					return -1; /* [한국어] 일반 오류로 돌아간다 */
			}	/* end of switch */
		}	/* end of valid device */
	}	/* end of for */

	if (!valid_device) { /* [한국어] 유효한 함수를 하나도 못 찾았으면 카드를 읽지 못한 것이다 */
		err("Could not find device to unconfigure.  Or could not read the card.\n"); /* [한국어] 그 사실을 알리고 */
		return -1; /* [한국어] 일반 오류로 돌아간다 */
	}
	return 0; /* [한국어] 모든 함수를 정리했으면 성공 */
}

/*
 * free the resources of the card (multi, single, or bridged)
 * Parameters: slot, flag to say if this is for removing entire module or just
 * unconfiguring the device
 * TO DO:  will probably need to add some code in case there was some resource,
 * to remove it... this is from when we have errors in the configure_card...
 *			!!!!!!!!!!!!!!!!!!!!!!!!!FOR BUSES!!!!!!!!!!!!
 * Returns: 0, -1, -ENODEV
 */
/* [한국어]
 * ibmphp_unconfigure_card - 카드의 자원을 모두 놓고 pci_func 목록을 해제한다
 *
 * @slot_cur: 대상 슬롯(이중 포인터).
 * @the_end:  모듈 전체를 내리는 중인지를 나타내는 플래그.
 * @return: 0 성공, 그 밖에는 unconfigure_boot_card() 의 오류.
 *
 * **제거 경로의 진입점**이다. 바로 위 상류 주석대로 단일·다기능·브리지 카드를
 * 모두 다루며, 삽입 중 오류가 났을 때 남은 자원을 치우는 처리는 TO DO 로
 * 남아 있다.
 *
 * 두 단계다.
 *
 *   1) the_end 가 0 이면 unconfigure_boot_card() 로 **config 공간을 읽어**
 *      장부에서 자원을 지운다. 이 경로가 실패해도 아래 2)는 계속 진행한다 —
 *      상류 주석대로 어느 경우든 pci_func 구조체는 정리해야 하기 때문이다.
 *      다만 -ENODEV / -EIO / -EINVAL 이면 그 자리에서 돌아간다.
 *
 *   2) sl->func 목록을 따라가며 각 함수가 들고 있는 자원 포인터를 지운다.
 *      BAR 개수가 갈리는데, func->bus 가 1 이면 브리지라 2 개, 아니면 6 개다.
 *      그 플래그는 configure_device() 와 configure_bridge() 가 세워 둔 것이다.
 *
 * `the_end > 0` 일 때만 ibmphp_remove_resource() 를 부르고, 아닐 때는
 * 포인터만 NULL 로 만든다. 1) 에서 이미 장부에서 지웠기 때문에 두 번 지우지
 * 않으려는 것으로 보이나, 그 대응 관계의 명시적 근거는 이 트리에서 확인 못 함.
 *
 * 상류 주석이 느낌표로 강조해 둔 대로 브리지의 버스 구조를 장부에서 지우는
 * 처리도 TO DO 로 남아 있다 — 실제로는 unconfigure_boot_bridge() 가 그것을
 * 하므로 the_end 경로에서만 빠진다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(카드 제거 또는 모듈 언로드).
 *
 * 호출 체인:  ibmphp_core.c → [이 함수] → unconfigure_boot_card()
 *               → ibmphp_remove_resource()
 */
int ibmphp_unconfigure_card(struct slot **slot_cur, int the_end)
{
	int i; /* [한국어] BAR 루프 인덱스 */
	int count; /* [한국어] 이 함수에서 볼 BAR 개수 — 브리지면 2, 일반 장치면 6 */
	int rc; /* [한국어] 하위 함수 반환값 임시 보관 */
	struct slot *sl = *slot_cur; /* [한국어] 호출자가 넘긴 포인터의 포인터에서 실제 슬롯 구조체를 꺼낸다 */
	struct pci_func *cur_func = NULL; /* [한국어] 훑는 중인 함수 구조체 */
	struct pci_func *temp_func; /* [한국어] 다음 함수를 미리 붙잡아 둘 자리 — 해제 뒤에는 cur_func 를 못 읽기 때문이다 */

	debug("%s - enter\n", __func__); /* [한국어] 진입 기록 */

	if (!the_end) { /* [한국어] the_end 가 0 이면 아직 드라이버가 살아 있는 정상 뽑기다 */
		/* Need to unconfigure the card */
		rc = unconfigure_boot_card(sl); /* [한국어] 하드웨어를 직접 훑어 부팅 때부터 있던 자원을 걷어낸다 */
		if ((rc == -ENODEV) || (rc == -EIO) || (rc == -EINVAL)) { /* [한국어] 카드를 읽을 수 없거나 지원하지 않는 구성이면 */
			/* In all other cases, will still need to get rid of func structure if it exists */
			return rc; /* [한국어] 그 오류를 그대로 돌려준다. 상류 주석대로 그 밖의 오류에서는 아래로 내려가 함수 구조체를 마저 정리한다 */
		}
	}

	if (sl->func) { /* [한국어] 이 슬롯에 함수 구조체 목록이 매달려 있으면 */
		cur_func = sl->func; /* [한국어] 첫 함수부터 시작해 */
		while (cur_func) { /* [한국어] 목록 끝까지 훑는다 */
			/* TO DO: WILL MOST LIKELY NEED TO GET RID OF THE BUS STRUCTURE FROM RESOURCES AS WELL */
			if (cur_func->bus) { /* [한국어] 브리지(PPB)로 표시된 함수면 */
				/* in other words, it's a PPB */
				count = 2; /* [한국어] BAR 가 둘뿐이다 */
			} else { /* [한국어] 일반 장치면 */
				count = 6; /* [한국어] BAR 가 여섯이다 */
			}

			for (i = 0; i < count; i++) { /* [한국어] 그 개수만큼 BAR 자리를 본다 */
				if (cur_func->io[i]) { /* [한국어] I/O 자원이 매달려 있으면 */
					debug("io[%d] exists\n", i); /* [한국어] 어느 자리인지 남기고 */
					if (the_end > 0) /* [한국어] the_end 가 양수일 때만 — 즉 드라이버 언로드가 아닐 때만 */
						ibmphp_remove_resource(cur_func->io[i]); /* [한국어] 장부에서 지운다. 언로드 경로에서는 장부 자체를 통째로 버리므로 여기서 건드리지 않는다 */
					cur_func->io[i] = NULL; /* [한국어] 포인터를 끊는다 */
				}
				if (cur_func->mem[i]) { /* [한국어] 메모리 자원이 매달려 있으면 */
					debug("mem[%d] exists\n", i); /* [한국어] 어느 자리인지 남기고 */
					if (the_end > 0) /* [한국어] the_end 가 양수일 때만 */
						ibmphp_remove_resource(cur_func->mem[i]); /* [한국어] 장부에서 지운다 */
					cur_func->mem[i] = NULL; /* [한국어] 포인터를 끊는다 */
				}
				if (cur_func->pfmem[i]) { /* [한국어] 프리페치 자원이 매달려 있으면 */
					debug("pfmem[%d] exists\n", i); /* [한국어] 어느 자리인지 남기고 */
					if (the_end > 0) /* [한국어] the_end 가 양수일 때만 */
						ibmphp_remove_resource(cur_func->pfmem[i]); /* [한국어] 장부에서 지운다 */
					cur_func->pfmem[i] = NULL; /* [한국어] 포인터를 끊는다 */
				}
			}

			temp_func = cur_func->next; /* [한국어] 해제 전에 다음 함수를 붙잡아 두고 */
			kfree(cur_func); /* [한국어] 현재 함수 구조체를 버린 뒤 */
			cur_func = temp_func; /* [한국어] 붙잡아 둔 다음 함수로 넘어간다 */
		}
	}

	sl->func = NULL; /* [한국어] 목록이 사라졌으므로 슬롯의 머리 포인터를 끊는다 */
	*slot_cur = sl; /* [한국어] 상류 코드가 호출자 포인터에 슬롯을 다시 담는다 — 값이 같아 실제 효과는 없다 */
	debug("%s - exit\n", __func__); /* [한국어] 끝났음을 남기고 */
	return 0; /* [한국어] 성공으로 돌아간다 */
}

/*
 * add a new bus resulting from hot-plugging a PPB bridge with devices
 *
 * Input: bus and the amount of resources needed (we know we can assign those,
 *        since they've been checked already
 * Output: bus added to the correct spot
 *         0, -1, error
 */
/* [한국어]
 * add_new_bus - 장부(ibmphp_res.c)에 2차 버스와 그 창 셋을 등록한다
 *
 * @bus:          등록할 버스 노드. 호출자가 만들어 busno 를 채워 둔다.
 * @io:           그 버스가 쓸 I/O 창의 근거가 될 자원(없으면 NULL).
 * @mem:          같은 메모리 창.
 * @pfmem:        같은 프리페치 메모리 창.
 * @parent_busno: 부모 버스 번호. 0xFF 면 목록에 매달지 않는다.
 * @return: 0 성공, -ENODEV 는 부모 버스를 못 찾은 경우, -ENOMEM 은 할당 실패.
 *
 * 바로 위 상류 주석대로 브리지를 꽂아 새 버스가 생겼을 때 그것을 장부에
 * 넣는다. 자원이 이미 확보되었음이 보장된 상태에서 불린다.
 *
 * **ibmphp_res.c 의 자료구조를 이 파일이 직접 만드는 유일한 자리**다. 다른
 * 곳은 모두 그쪽 API 를 통하는데, 여기서는 struct range_node 를 직접
 * kzalloc 해 bus->rangeIO 등에 꽂는다. 창이 하나뿐이라 rangeno 를 1 로
 * 못박고 개수도 1 로 둔다.
 *
 * parent_busno 가 0xFF 인 경우가 특이하다. configure_bridge() 가 **이미
 * 장부에 있는 버스 껍데기**를 재사용할 때 그 값을 넘긴다 — 그 버스는 이미
 * gbuses 에 매달려 있으므로 다시 매달면 안 되기 때문이다.
 *
 * [관찰] list_add(&bus->bus_list, &cur_bus->bus_list) 는 부모 버스 **뒤에**
 * 끼우는 것이지 부모의 자식으로 넣는 것이 아니다. gbuses 는 평평한 목록이라
 * 계층 관계를 담지 않으며, 부모-자식 관계는 필요할 때마다 인자로 전달된다
 * (ibmphp_remove_bus 의 parent_busno 처럼).
 *
 * [관찰] 중간에서 할당이 실패하면 앞서 만든 range 를 되돌리지 않는다.
 * 호출자(configure_bridge)가 -ENOMEM 을 받으면 ibmphp_remove_bus() 를
 * 부르므로 그때 함께 정리된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(카드 삽입).
 *
 * 호출 체인:  configure_bridge() → [이 함수] → ibmphp_find_res_bus()
 */
static int add_new_bus(struct bus_node *bus, struct resource_node *io, struct resource_node *mem, struct resource_node *pfmem, u8 parent_busno)
{
	struct range_node *io_range = NULL; /* [한국어] 새 버스가 가질 I/O 범위 노드 */
	struct range_node *mem_range = NULL; /* [한국어] 새 버스가 가질 메모리 범위 노드 */
	struct range_node *pfmem_range = NULL; /* [한국어] 새 버스가 가질 프리페치 범위 노드 */
	struct bus_node *cur_bus = NULL; /* [한국어] 부모 버스의 장부 노드 — 새 버스를 이 노드 뒤에 끼워 넣는다 */

	/* Trying to find the parent bus number */
	if (parent_busno != 0xFF) { /* [한국어] 0xFF 는 부모에 끼워 넣지 말라는 표시다. 이미 목록에 있는 버스를 다시 끼우지 않으려는 것이다 */
		cur_bus	= ibmphp_find_res_bus(parent_busno); /* [한국어] 부모 버스의 장부 노드를 찾는다 */
		if (!cur_bus) { /* [한국어] 없으면 있을 수 없는 상태다 */
			err("strange, cannot find bus which is supposed to be at the system... something is terribly wrong...\n"); /* [한국어] 그 사실을 알리고 */
			return -ENODEV; /* [한국어] 장치 없음으로 돌아간다 */
		}

		list_add(&bus->bus_list, &cur_bus->bus_list); /* [한국어] **전역 버스 목록 gbuses 에 새 버스를 부모 바로 뒤로 끼워 넣는다**. 순서를 이렇게 두어야 부모를 찾은 뒤 자식을 만나게 된다 */
	}
	if (io) { /* [한국어] I/O 창을 잡아 왔으면 */
		io_range = kzalloc_obj(*io_range); /* [한국어] 범위 노드를 만든다 */
		if (!io_range) /* [한국어] 메모리가 없으면 */
			return -ENOMEM; /* [한국어] ENOMEM 으로 돌아간다. 앞서 끼워 넣은 버스는 호출자가 ibmphp_remove_bus() 로 걷어낸다 */

		io_range->start = io->start; /* [한국어] 창의 시작을 그대로 범위의 시작으로 삼고 */
		io_range->end = io->end; /* [한국어] 창의 끝을 범위의 끝으로 삼는다 */
		io_range->rangeno = 1; /* [한국어] 새 버스의 첫 범위이므로 번호는 1 */
		bus->noIORanges = 1; /* [한국어] 이 버스의 I/O 범위 개수는 하나뿐이다 */
		bus->rangeIO = io_range; /* [한국어] 버스 노드에 범위를 매단다 */
	}
	if (mem) { /* [한국어] 메모리 창을 잡아 왔으면 */
		mem_range = kzalloc_obj(*mem_range); /* [한국어] 범위 노드를 만든다 */
		if (!mem_range) /* [한국어] 메모리가 없으면 */
			return -ENOMEM; /* [한국어] ENOMEM 으로 돌아간다 */

		mem_range->start = mem->start; /* [한국어] 창의 시작을 범위의 시작으로, */
		mem_range->end = mem->end; /* [한국어] 창의 끝을 범위의 끝으로 삼는다 */
		mem_range->rangeno = 1; /* [한국어] 첫 범위이므로 번호는 1 */
		bus->noMemRanges = 1; /* [한국어] 메모리 범위 개수는 하나뿐이다 */
		bus->rangeMem = mem_range; /* [한국어] 버스 노드에 범위를 매단다 */
	}
	if (pfmem) { /* [한국어] 프리페치 창을 잡아 왔으면 */
		pfmem_range = kzalloc_obj(*pfmem_range); /* [한국어] 범위 노드를 만든다 */
		if (!pfmem_range) /* [한국어] 메모리가 없으면 */
			return -ENOMEM; /* [한국어] ENOMEM 으로 돌아간다 */

		pfmem_range->start = pfmem->start; /* [한국어] 창의 시작을 범위의 시작으로, */
		pfmem_range->end = pfmem->end; /* [한국어] 창의 끝을 범위의 끝으로 삼는다 */
		pfmem_range->rangeno = 1; /* [한국어] 첫 범위이므로 번호는 1 */
		bus->noPFMemRanges = 1; /* [한국어] 프리페치 범위 개수는 하나뿐이다 */
		bus->rangePFMem = pfmem_range; /* [한국어] 버스 노드에 범위를 매단다 */
	}
	return 0; /* [한국어] 세 범위를 다 붙였으면 성공 */
}

/*
 * find the 1st available bus number for PPB to set as its secondary bus
 * Parameters: bus_number of the primary bus
 * Returns: bus_number of the secondary bus or 0xff in case of failure
 */
/* [한국어]
 * find_sec_number - 브리지에 줄 2차 버스 번호를 고른다
 *
 * @primary_busno: 그 브리지가 붙어 있는 버스 번호.
 * @slotno:        브리지 카드가 꽂힌 슬롯 번호.
 * @return: 쓸 수 있는 버스 번호. 못 고르면 0xff.
 *
 * 바로 위 상류 주석대로 첫 번째로 쓸 수 있는 버스 번호를 찾아 준다.
 *
 * **번호를 탐색하지 않고 계산한다**는 점이 요점이다. EBDA 가 슬롯마다 버스
 * 번호를 하나씩만 배정하므로, 슬롯 번호에서 그 버스의 최소 슬롯 번호를 뺀
 * 값을 1차 버스 번호에 더하면 그 슬롯 몫의 번호가 나온다. 즉 슬롯과 버스
 * 번호가 1:1 로 고정되어 있다.
 *
 * 그다음 그 번호가 실제로 비어 있는지 장부에서 확인한다. 상류 주석대로
 * **버스 구조가 있어도 창이 하나도 없으면 비어 있는 것으로 본다** — 이전
 * 로드에서 브리지 카드를 뽑았을 때 껍데기만 남는 경우가 있기 때문이다.
 * configure_bridge() 가 그 껍데기를 재사용하는 것과 짝을 이룬다.
 *
 * bus_info 는 ibmphp_ebda.c 가 EBDA 에서 읽어 둔 슬롯 범위 정보이며,
 * ibmphp_find_same_bus_num() 으로 얻는다.
 *
 * 이 계산 방식 때문에 브리지 뒤에 버스가 하나뿐이라는 제약이 생긴다 —
 * 슬롯 몫이 번호 하나이므로 그 뒤에 또 브리지를 둘 수 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(카드 삽입).
 *
 * 호출 체인:  configure_bridge() → [이 함수]
 *               → ibmphp_find_same_bus_num() / ibmphp_find_res_bus()
 */
static u8 find_sec_number(u8 primary_busno, u8 slotno)
{
	int min, max; /* [한국어] 이 버스가 담당하는 슬롯 번호의 최소·최대 */
	u8 busno; /* [한국어] 계산해 낸 2차 버스 번호 */
	struct bus_info *bus; /* [한국어] EBDA 에서 온 버스 정보 — 슬롯 번호 범위가 여기 들어 있다 */
	struct bus_node *bus_cur; /* [한국어] 같은 번호의 버스가 이미 장부에 있는지 확인할 노드 */

	bus = ibmphp_find_same_bus_num(primary_busno); /* [한국어] EBDA 표에서 이 1차 버스의 정보를 찾는다 */
	if (!bus) { /* [한국어] 없으면 BIOS 가 알려 준 범위를 알 수 없다 */
		err("cannot get slot range of the bus from the BIOS\n"); /* [한국어] 그 사실을 알리고 */
		return 0xff; /* [한국어] 실패 표시 0xff 를 돌려준다 */
	}
	max = bus->slot_max; /* [한국어] 이 버스의 마지막 슬롯 번호 */
	min = bus->slot_min; /* [한국어] 이 버스의 첫 슬롯 번호 */
	if ((slotno > max) || (slotno < min)) { /* [한국어] 슬롯 번호가 그 범위 밖이면 짝이 맞지 않는 것이다 */
		err("got the wrong range\n"); /* [한국어] 그 사실을 알리고 */
		return 0xff; /* [한국어] 실패 표시 0xff 를 돌려준다 */
	}
	busno = (u8) (slotno - (u8) min); /* [한국어] **슬롯의 순번(0 부터)** 을 구한다 */
	busno += primary_busno + 0x01; /* [한국어] 1차 버스 번호 바로 다음부터 그 순번만큼 떨어진 번호를 쓴다 — 슬롯 하나에 버스 번호 하나가 이렇게 고정 배정된다 */
	bus_cur = ibmphp_find_res_bus(busno); /* [한국어] 그 번호가 이미 장부에 있는지 본다 */
	/* either there is no such bus number, or there are no ranges, which
	 * can only happen if we removed the bridged device in previous load
	 * of the driver, and now only have the skeleton bus struct
	 */
	if ((!bus_cur) || (!(bus_cur->rangeIO) && !(bus_cur->rangeMem) && !(bus_cur->rangePFMem))) /* [한국어] 노드가 없거나, 있어도 범위가 셋 다 비어 있으면 아직 아무도 쓰지 않는 번호다 */
		return busno; /* [한국어] 그 번호를 돌려준다 */
	return 0xff; /* [한국어] 이미 쓰이고 있으면 실패 표시 0xff 를 돌려준다 */
}
