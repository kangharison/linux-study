// SPDX-License-Identifier: GPL-2.0+
/*
 * Compaq Hot Plug Controller Driver
 *
 * Copyright (C) 1995,2001 Compaq Computer Corporation
 * Copyright (C) 2001 Greg Kroah-Hartman (greg@kroah.com)
 * Copyright (C) 2001 IBM Corp.
 *
 * All rights reserved.
 *
 * Send feedback to <greg@kroah.com>
 *
 */

/*
 * [한국어 설명] Compaq 핫플러그의 PCI 설정공간 저장·복원과 자원 목록 만들기 (cpqphp_pci.c)
 *
 * === 파일의 역할 ===
 * cpqphp_ctrl.c 가 "언제 무엇을 할지" 를 정한다면, 이 파일은 그 결정을
 * 실제 PCI 설정공간에 옮기고 되돌리는 일을 맡는다. 네 갈래다.
 *
 * 첫째, **설정공간 사본 뜨기와 되돌리기.** 드라이버가 올라올 때
 * cpqhp_save_config 가 버스를 훑어 모든 카드의 설정공간 128바이트를
 * struct pci_func 안에 통째로 복사해 둔다. 카드를 뽑았다 같은 카드를
 * 다시 끼우면 cpqhp_configure_board 가 그 사본을 그대로 되쓴다.
 * cpqhp_valid_replace 는 되쓰기 전에 "정말 같은 카드인가" 를 검사한다.
 *
 * 둘째, **BAR 길이 재기.** cpqhp_save_base_addr_length 가 각 BAR 에
 * 0xFFFFFFFF 를 써 보고 되읽어 카드가 요구하는 크기를 알아낸다.
 * 이것이 PCI 규격이 정한 크기 질의 방법이며, 교체된 카드가 같은 크기를
 * 요구하는지 확인하는 근거가 된다.
 *
 * 셋째, **자유 목록의 초기값 만들기.** cpqhp_find_available_resources 가
 * 시스템 ROM 에서 Hot Plug Resource Table(HRT)을 찾아, 펌웨어가 남긴
 * "이 슬롯에는 이 범위를 써도 된다" 는 정보를 struct pci_resource
 * 연결 리스트로 바꾼다. **cpqphp_ctrl.c 의 할당기가 나눠 줄 밑천이 전부
 * 여기서 나온다.**
 *
 * 넷째, **자원 회수와 파괴.** cpqhp_return_board_resources 는 카드가
 * 쓰던 자원을 자유 목록으로 돌려주고, cpqhp_destroy_resource_list 와
 * cpqhp_destroy_board_resources 는 목록을 통째로 해제한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 드라이버 파일 넷 중 PCI 설정공간과 맞닿은 층이다.
 *
 *   cpqphp_core.c  : 드라이버 등록과 슬롯 sysfs. **주석 없음.**
 *   cpqphp_ctrl.c  : 이벤트 상태 기계와 자원 할당기. 이 파일의 함수를
 *                    부르는 쪽이다.
 *   [이 파일]      : 설정공간 저장·복원, BAR 길이 재기, HRT 읽기,
 *                    자원 회수.
 *   cpqphp_nvram.c : compaq_nvram_load 로 NVRAM 상태를 읽는다.
 *                    이 파일이 cpqhp_find_available_resources 에서 부른다.
 *   cpqphp.h       : 자료구조와 인라인 함수. **주석 없음.**
 *
 * 호출 관계가 한 방향이다 -- cpqphp_ctrl.c 가 이 파일을 부르고,
 * 이 파일은 cpqphp_ctrl.c 의 것 중 cpqhp_slot_create, cpqhp_slot_find,
 * cpqhp_resource_sort_and_combine 셋만 되부른다. 그 셋은 자료구조를
 * 다루는 도우미라 계층을 거스르지 않는다.
 *
 * 부팅 시:
 *   cpqphp_core.c 의 probe
 *     → cpqhp_save_config              (이 파일: 기존 카드 전부 사본 뜨기)
 *     → cpqhp_find_available_resources (이 파일: HRT 로 자유 목록 만들기)
 *         → compaq_nvram_load          (cpqphp_nvram.c)
 *
 * 카드를 뽑을 때:
 *   cpqphp_ctrl.c 의 remove_board
 *     → cpqhp_unconfigure_device       (이 파일: 리눅스 pci_dev 제거)
 *     → cpqhp_save_base_addr_length    (교체 대비 BAR 크기 기록)
 *       또는 cpqhp_save_used_resources (핫애드 대비 사용 중 자원 기록)
 *     → cpqhp_return_board_resources   (자유 목록으로 반납)
 *
 * 카드를 끼울 때:
 *   같은 카드면 → cpqhp_valid_replace → cpqhp_configure_board (사본 되쓰기)
 *   새 카드면   → cpqphp_ctrl.c 의 configure_new_function 이 자유 목록에서
 *                 자원을 떼어 직접 설정 → cpqhp_save_slot_config 로 사본 갱신
 *                 → cpqhp_configure_device 로 리눅스 pci_dev 생성
 *
 * **이 두 갈래가 이 드라이버의 핵심 분기다.** "같은 카드 교체" 는 사본을
 * 되쓰면 그만이지만, "새 카드 추가" 는 자원을 새로 나눠 줘야 한다.
 * 그래서 컨트롤러가 add_support 를 지원하는지에 따라 경로가 달라진다.
 *
 * 실행 컨텍스트: 전부 프로세스 컨텍스트다. cpqphp_ctrl.c 의 커널 스레드나
 * probe 경로에서만 불리며, 인터럽트 컨텍스트에서 불리는 함수는 없다.
 * 그래서 kmalloc(GFP_KERNEL)과 pci_lock_rescan_remove 를 자유롭게 쓴다.
 *
 * === 타 모듈과의 연결 ===
 * 이 파일이 **의존하는** 쪽:
 *   PCI 코어 : pci_bus_read_config_ 및 _write_config_ 계열이 거의 모든
 *     함수의 본체다. 그 밖에 pci_scan_slot, pci_bus_add_devices,
 *     장치를 없애고 찾는 pci_stop_and_remove_bus_device 와
 *     pci_get_domain_bus_and_slot,
 *     pci_hp_add_bridge, pci_lock_rescan_remove 를 쓴다.
 *   ../pci.h : PCI 서브시스템 내부 헤더. pci_bus_read_dev_vendor_id 가
 *     여기서 온다.
 *   cpqphp.h : struct pci_func, struct pci_resource, struct controller,
 *     return_resource(:461), 오류 코드 상수들(:366~375),
 *     PCI_TO_PCI_BRIDGE_CLASS(:364), dbg/err/info 매크로(:25~28).
 *   cpqphp_nvram.h : compaq_nvram_load 와 HRT 오프셋 상수들
 *     (SIG0~SIG3, UNUSED_IRQ, PCIIRQ, NUMBER_OF_ENTRIES, DEV_FUNC,
 *     PRIMARY_BUS, SECONDARY_BUS, MAX_BUS, IO_BASE, IO_LENGTH,
 *     MEM_BASE, MEM_LENGTH, PRE_MEM_BASE, PRE_MEM_LENGTH).
 *   아키텍처 : pcibios_set_irq_routing 과 inb/outb. **arch/ 와
 *     include/asm 이 이 스파스 체크아웃에 없어 확인하지 못했다.**
 *
 * 이 파일이 **내보내는** 전역:
 *   cpqhp_nic_irq, cpqhp_disk_irq : HRT 의 미사용 IRQ 목록에서 골라
 *     둔 두 IRQ 번호. cpqphp_ctrl.c 의 configure_new_function 이 카드
 *     종류(저장장치인가 아닌가)에 따라 둘 중 하나를 배정한다.
 *     **시스템 전체에 하나뿐인 전역이다.**
 *
 * 데이터 흐름 -- 설정공간 사본이 중심이다:
 *   실제 하드웨어 설정공간
 *     ↕ pci_bus_read/write_config_dword
 *   func->config_space[0x20]   (u32 서른두 칸 = 128바이트)
 *     ↕
 *   func->base_length[6], func->base_type[6]  (BAR 크기와 종류)
 *     ↕
 *   func->io_head / mem_head / p_mem_head / bus_head  (이 카드가 쓰는 자원)
 *     ↕ return_resource / cpqhp_return_board_resources
 *   ctrl->io_head / mem_head / p_mem_head / bus_head  (컨트롤러의 자유 목록)
 *
 * === 주요 함수/구조체 요약 ===
 * 함수:
 *   cpqhp_save_config            : 버스를 훑어 모든 카드의 설정공간을
 *     사본으로 뜬다. 브리지를 만나면 **재귀** 로 하위 버스까지 내려간다.
 *     빈 슬롯에도 struct pci_func 를 만들어 자리를 잡아 둔다.
 *   cpqhp_save_slot_config       : 슬롯 하나에 대해 같은 일을 한다.
 *     카드를 새로 설정한 뒤 사본을 갱신하는 용도다.
 *   cpqhp_save_base_addr_length  : BAR 에 0xFFFFFFFF 를 써 보고 되읽어
 *     카드가 요구하는 크기를 알아낸다. 교체 검증의 근거가 된다.
 *   cpqhp_save_used_resources    : 이미 꽂혀 있던 카드가 **실제로 쓰고
 *     있는** 주소 범위를 읽어 목록으로 만든다. 위 함수가 "얼마나 필요한가"
 *     를 재는 것과 달리 이쪽은 "지금 어디를 쓰는가" 를 읽는다.
 *   cpqhp_configure_board        : 사본을 설정공간에 되쓴다.
 *     **높은 오프셋부터 거꾸로 쓴다** -- 제어 레지스터를 마지막에 쓰기
 *     위해서이며, 그 이유가 원문 주석에 적혀 있다.
 *   cpqhp_valid_replace          : 벤더/장치 ID, 리비전, 클래스 코드,
 *     서브시스템 ID, BAR 크기를 사본과 견줘 같은 카드인지 판정한다.
 *   cpqhp_find_available_resources : ROM 에서 HRT 를 찾아 자유 목록을
 *     만든다. **이 드라이버 자원 할당의 출발점이다.**
 *   cpqhp_return_board_resources : 카드의 자원 목록 넷을 자유 목록으로
 *     옮기고 정렬·병합한다.
 *   cpqhp_configure_device / cpqhp_unconfigure_device : 리눅스 쪽
 *     struct pci_dev 를 만들고 없앤다. 하드웨어가 아니라 커널 자료구조를
 *     다루는 유일한 두 함수다.
 *   cpqhp_set_irq                : 레거시 모드에서 IRQ 라우팅을 프로그래밍한다.
 *   detect_HRT_floating_pointer  : ROM 을 16바이트 간격으로 훑어
 *     "$HRT" 서명을 찾는다.
 *
 * === HRT -- 펌웨어가 남긴 자원 표 ===
 * 이 드라이버가 자원을 어디서 얻는지가 이 파일에서 가장 볼 만한 대목이다.
 *
 * 시스템 ROM 어딘가에 Compaq 이 정한 Hot Plug Resource Table 이 놓여
 * 있고, 그 시작에 "$HRT" 네 글자가 있다. detect_HRT_floating_pointer 가
 * ROM 영역을 16바이트씩 건너뛰며 그 서명을 찾는다. 16바이트 간격인 것은
 * 그 시절 펌웨어 표들이 문단 경계(paragraph, 16바이트)에 정렬되는
 * 관례를 따랐기 때문으로 보이나, 그 근거 문서는 이 트리에 없다.
 *
 * 표를 찾으면 struct hrt 헤더 뒤로 struct slot_rt 항목이 이어진다.
 * 항목마다 장치/함수 번호, 기본·보조·최대 버스 번호, 그리고 IO·메모리·
 * prefetchable 메모리의 시작과 길이가 들어 있다.
 * cpqhp_find_available_resources 가 그것을 하나씩 읽어,
 *   빈 슬롯이면 → 컨트롤러의 자유 목록(ctrl->io_head 등)에 넣고,
 *   카드가 꽂힌 슬롯이면 → 그 카드의 목록(func->io_head 등)에 넣는다.
 * 즉 **같은 표가 "나눠 줄 수 있는 것" 과 "이미 쓰이는 것" 을 함께
 * 알려 준다.** 빈 슬롯 몫만 할당기의 밑천이 되고, 꽂힌 슬롯 몫은 그
 * 카드를 뽑을 때 비로소 자유 목록으로 돌아온다.
 *
 * 이 방식의 성격은 분명하다 -- 이 드라이버는 자원을 **발견하지 않고
 * 통보받는다.** 펌웨어가 정해 준 범위 밖으로는 나가지 않으며,
 * HRT 가 없으면 cpqhp_find_available_resources 가 -ENODEV 를 돌려주고
 * 핫애드 기능 자체가 꺼진다.
 *
 * === BAR 크기 질의라는 관용구 ===
 * 이 파일에 세 번 되풀이되는 패턴이 있다
 * (cpqhp_save_base_addr_length, cpqhp_save_used_resources,
 * cpqhp_valid_replace, 그리고 cpqphp_ctrl.c 의 configure_new_function).
 *
 *   BAR 에 0xFFFFFFFF 를 쓴다
 *   BAR 를 되읽는다
 *   하위 종류 비트를 지운다 (IO 면 0x1, 메모리면 0xF)
 *   비트를 뒤집고 1 을 더한다 -- (~base) + 1
 *
 * 마지막 줄이 요구 크기가 된다. PCI 장치는 BAR 의 하위 비트를 자기가
 * 쓰지 않는 만큼 0 으로 고정해 두므로, 전부 1 을 써 넣고 되읽으면
 * "내가 정렬을 요구하는 자리" 만 1 로 남는다. 그 2의 보수가 곧 크기다.
 * 예컨대 4KiB 를 요구하는 IO BAR 는 0xFFFFF001 로 읽히고,
 * 0xFFFFF000 을 뒤집어 1 을 더하면 0x1000 이 된다.
 *
 * **이 관용구는 카드를 잠깐 못 쓰게 만든다.** BAR 에 쓰레기 값이 들어간
 * 상태가 되므로, cpqhp_save_used_resources 는 시작할 때 명령 레지스터를
 * 0 으로 만들어 카드를 꺼 두고 작업한다. 다만 그 함수는 저장해 둔
 * save_command 를 다시 써 주지 않는다 -- 어차피 카드를 뽑으려는
 * 참이라 되살릴 이유가 없기 때문으로 보이나, 코드가 그 판단을 적어
 * 두지는 않았다.
 *
 * === 2000년대 초 관용구에 대하여 ===
 *   자체 로그 매크로 : dbg/err/info 가 cpqphp.h:25~28 에 정의되어 있다.
 *     이 파일만 pr_fmt 를 정의해 두었는데(맨 위), 정작 pr_ 계열을 쓰는
 *     곳은 PCI_ScanBusForNonBridge 의 pr_warn 하나뿐이다.
 *     나머지는 전부 옛 매크로를 쓴다 -- 두 세대의 관용구가 한 파일에
 *     섞여 있는 셈이다.
 *   가짜 구조체 만들기 : cpqhp_set_irq 가 pcibios_set_irq_routing 에
 *     넘기려고 struct pci_dev 와 struct pci_bus 를 kmalloc 으로 만들어
 *     devfn 과 bus number 만 채운 뒤 곧바로 버린다. 그 API 가 pci_dev
 *     포인터를 요구하는데 여기에는 진짜 pci_dev 가 없기 때문이다.
 *   버스 번호를 바꿔 가며 재사용하기 : ctrl->pci_bus->number 에 원하는
 *     버스 번호를 대입한 뒤 pci_bus_read_config_ 를 부르는 패턴이
 *     파일 전체에 흩어져 있다. 임시 pci_bus 하나를 돌려 쓰는 것이라,
 *     재귀 호출에서 돌아온 뒤 번호를 되돌려 놓는 줄이 짝을 이룬다.
 *   반환문 괄호 : return(0), return(rc) 처럼 함수 호출처럼 쓴 곳이 많다.
 *   빈 else 절 : cpqhp_save_base_addr_length 끝에 주석만 있는 빈
 *     else 블록이 있다.
 *
 * === 값의 근거에 대하여 ===
 *   HRT 오프셋 상수(SIG0, DEV_FUNC, IO_BASE 등)의 값은 cpqphp_nvram.h 가
 *     유일한 근거다. 표의 형식을 정한 공개 문서가 트리에 없으므로,
 *     각 항목이 어떻게 쓰이는지로만 설명한다.
 *   0x0B 같은 설정공간 오프셋을 상수 이름 없이 직접 쓴 곳이 여럿 있다.
 *     PCI 규격상 0x0B 는 클래스 코드의 최상위 바이트인데, 코드가 그
 *     사실을 적어 두지 않았으므로 쓰임새(class_code 변수에 담는다)로
 *     설명한다.
 *   pcibios_set_irq_routing, inb/outb, 0x4d0/0x4d1 포트(ELCR)는 x86
 *     고유이며 **arch/ 가 이 트리에 없어 확인하지 못했다.** 위의 원문
 *     주석이 "x86 전용" 이라고 밝힌 범위에서만 옮긴다.
 *   kmalloc_obj 와 kzalloc_obj 매크로의 정의도 이 트리에 없다.
 *
 * === NVMe 관점 ===
 * drivers/nvme 는 이 파일의 어떤 심볼도 부르지 않는다(이 트리에서 전수
 * 확인: 0건). PCI/PCI-X 시절 하드웨어라 NVMe 와 시대가 겹치지 않는다.
 *
 * 다만 이 파일이 푸는 문제 -- "카드가 요구하는 BAR 크기를 재고, 정해진
 * 범위 안에서 겹치지 않게 주소를 나눠 주고, 뽑으면 회수한다" -- 는
 * 지금 NVMe 드라이브를 핫스왑할 때 PCI 코어가 하는 일과 같다.
 * 다른 것은 그 일을 드라이버가 직접 하느냐(이 파일) 코어에 맡기느냐일
 * 뿐이다. BAR 크기를 0xFFFFFFFF 로 질의하는 관용구는 그때나 지금이나
 * 똑같이 쓰인다.
 */

/* [한국어] pr_ 계열 로그의 접두사를 정한다.
 * **정작 이 파일에서 pr_ 계열을 쓰는 곳은 PCI_ScanBusForNonBridge 의
 * pr_warn 하나뿐이다.** 나머지는 전부 cpqphp.h:25~28 의 옛 dbg/err/info
 * 매크로를 쓴다 -- 두 세대의 로그 관용구가 한 파일에 섞여 있고,
 * 이 줄과 그 pr_warn 이 나중에 추가되었음을 보여 준다 */
#define pr_fmt(fmt) "cpqphp: " fmt

/* [한국어] MODULE_ 매크로 계열의 타입 */
#include <linux/module.h>
/* [한국어] 기본 매크로. cpqphp.h 의 로그 매크로가 printk 에 의존한다 */
#include <linux/kernel.h>
/* [한국어] pr_warn 과 pr_fmt. **위의 pr_fmt 정의와 짝을 이룬다** --
 * cpqphp_ctrl.c 에는 이 include 가 없는데, 그쪽은 pr_ 계열을 쓰지 않기
 * 때문이다 */
#include <linux/printk.h>
/* [한국어] u8, u16, u32 등 폭이 고정된 정수 타입 */
#include <linux/types.h>
/* [한국어] kmalloc 과 kfree. **자원 노드를 만들 때마다 할당한다** --
 * cpqhp_save_used_resources 와 cpqhp_find_available_resources 가
 * struct pci_resource 를 하나씩 잡는다 */
#include <linux/slab.h>
/* [한국어] struct work_struct. struct controller 의 필드 타입을 위해 필요하며,
 * **이 파일은 워크큐를 쓰지 않는다** */
#include <linux/workqueue.h>
/* [한국어] procfs 관련. **이 파일이 쓰는 심볼이 보이지 않는다** --
 * 과거에 쓰였다가 남은 것으로 보이나 근거는 확인하지 못했다 */
#include <linux/proc_fs.h>
/* [한국어] struct pci_dev, struct pci_bus, pci_bus_read_config_ 계열,
 * pci_scan_slot, pci_stop_and_remove_bus_device, PCI_ 상수 전부.
 * **이 파일의 거의 모든 줄이 이 헤더에 의존한다** */
#include <linux/pci.h>
/* [한국어] pci_hp_add_bridge 등 핫플러그 코어의 함수 */
#include <linux/pci_hotplug.h>
/* [한국어] PCI 서브시스템 **내부** 헤더. 여기서 쓰는 것은
 * pci_bus_read_dev_vendor_id 로 보이며, 그것은 외부에 공개되지 않는
 * 함수라 이 경로로만 닿을 수 있다 */
#include "../pci.h"
/* [한국어] 이 드라이버의 공용 헤더. struct pci_func, struct pci_resource,
 * struct controller, return_resource(:461), 오류 코드(:366~375),
 * PCI_TO_PCI_BRIDGE_CLASS(:364), 로그 매크로(:25~28)가 여기서 온다.
 * **아직 주석되지 않은 파일이다** */
#include "cpqphp.h"
/* [한국어] compaq_nvram_load 와 **HRT 표의 오프셋 상수 전부**
 * (SIG0~SIG3, UNUSED_IRQ, PCIIRQ, NUMBER_OF_ENTRIES, DEV_FUNC,
 * PRIMARY_BUS, SECONDARY_BUS, MAX_BUS, IO_BASE, IO_LENGTH, MEM_BASE,
 * MEM_LENGTH, PRE_MEM_BASE, PRE_MEM_LENGTH).
 * cpqhp_find_available_resources 가 그 상수들로 ROM 의 표를 해석한다 */
#include "cpqphp_nvram.h"


/* [한국어] 저장장치가 아닌 카드에 배정할 IRQ 번호.
 * 설정자: cpqhp_find_available_resources 가 HRT 의 미사용 IRQ 비트맵에서
 *   **두 번째로 찾은** 1 비트의 위치를 넣는다. 못 찾으면 컨트롤러
 *   자신의 IRQ(ctrl->cfgspc_irq)를 쓴다.
 * 읽는 자: cpqphp_ctrl.c 의 configure_new_function 이 레거시 모드에서
 *   카드 클래스가 저장장치가 아닐 때 이 값을 배정한다.
 * 값 범위: IRQ 번호. 0 이면 아직 정해지지 않은 것이며, 그 경우를
 *   위의 설정자가 걸러 낸다.
 * **시스템 전체에 하나뿐인 전역이다** -- 컨트롤러가 여럿이어도 나중에
 *   탐지된 것이 앞의 값을 덮어쓴다.
 * 동기화: 없다. probe 경로에서만 쓰인다 */
u8 cpqhp_nic_irq;
/* [한국어] 저장장치 카드에 배정할 IRQ 번호.
 * 설정자: cpqhp_find_available_resources 가 미사용 IRQ 비트맵에서
 *   **첫 번째로 찾은** 1 비트의 위치를 넣는다.
 * 읽는 자: configure_new_function 이 클래스 코드가
 *   PCI_BASE_CLASS_STORAGE 일 때 이 값을 배정한다.
 * **둘로 나눈 이유**: 저장장치와 네트워크 카드가 서로 다른 IRQ 를 쓰게
 *   해서 인터럽트 처리 부하를 분산하려는 것으로 보인다.
 *   코드가 그 의도를 적어 두지는 않았다.
 * 동기화: 없다 */
u8 cpqhp_disk_irq;

/* [한국어] HRT 에서 읽은 미사용 IRQ 비트맵을 임시로 담는 변수.
 * 설정자·읽는 자: cpqhp_find_available_resources 하나뿐이다.
 * **전역일 이유가 없어 보인다** -- 그 함수 안의 지역 변수로 충분하며,
 *   실제로 함수 밖에서 읽는 코드가 없다. 그 시절 코드에서 흔한 관성이다.
 * 값 범위: 비트마스크. 오른쪽으로 밀어 가며 1 비트를 찾는 데 쓰이므로
 *   함수가 끝날 때는 대개 0 이나 남은 상위 비트가 된다.
 * 타입이 u16 인데 readl 로 32비트를 읽어 넣는 것에 주의 --
 *   상위 16비트가 잘린다. IRQ 번호가 16 미만이라 실질적 문제는 없다.
 * 동기화: 없다. probe 경로에서만 쓰인다 */
static u16 unused_IRQ;

/*
 * detect_HRT_floating_pointer
 *
 * find the Hot Plug Resource Table in the specified region of memory.
 *
 */
/* [한국어]
 * detect_HRT_floating_pointer - ROM 에서 "$HRT" 서명을 찾는다
 *
 * @begin: 훑기 시작할 주소.
 * @end:   훑기를 마칠 주소.
 * @return: 표를 찾은 주소, 못 찾으면 NULL.
 *
 * 시스템 ROM 어딘가에 놓인 Hot Plug Resource Table 의 시작을 찾는다.
 * **이 드라이버가 자원 정보를 얻는 출발점이다** -- 이 함수가 NULL 을
 * 돌려주면 핫애드 기능 자체가 꺼진다.
 *
 * 찾는 방법은 단순한 서명 탐색이다. 네 바이트가 차례로 '$', 'H', 'R', 'T'
 * 인 자리를 찾는다. SIG0~SIG3 은 cpqphp_nvram.h 가 정의한 오프셋이며,
 * 그 값은 이 트리에서 확인할 수 있으나 표 형식을 정한 문서는 없다.
 *
 * **16바이트씩 건너뛰는 것** 이 이 함수의 특징이다. 한 바이트씩 훑으면
 * 16배 오래 걸리는데, 그럴 필요가 없다고 본 것이다. 그 시절 펌웨어의
 * 표들이 문단 경계(paragraph, 16바이트)에 정렬되는 관례를 따랐기
 * 때문으로 보이나, **그 관례의 근거 문서는 이 트리에 없다.**
 *
 * 끝 주소를 struct hrt 크기만큼 앞당겨 잡는 것에 주의 --
 * 표 헤더가 통째로 들어갈 자리가 있어야 하므로, 마지막 후보는
 * end - sizeof(struct hrt) + 1 이다.
 *
 * readb 로 읽는다 -- ROM 이 __iomem 영역으로 매핑되어 있어 직접
 * 역참조할 수 없기 때문이다.
 *
 * status 플래그를 쓰는 대신 break 뒤에 fp 를 검사해도 되지만,
 * 루프가 끝까지 돌았을 때 fp 가 endp 를 넘어선 값이 되므로
 * 플래그가 필요하다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe 경로).
 *
 * 호출 체인:
 *   cpqhp_find_available_resources → [이 함수] → readb
 */
static void __iomem *detect_HRT_floating_pointer(void __iomem *begin, void __iomem *end)
{
	void __iomem *fp;
	/* [한국어] 훑기를 마칠 주소 */
	void __iomem *endp;
	/* [한국어] 서명 네 바이트를 담을 변수 */
	u8 temp1, temp2, temp3, temp4;
	/* [한국어] 찾았는지 표시하는 플래그 */
	int status = 0;

	/* [한국어] **표 헤더가 통째로 들어갈 자리가 있어야 하므로** 끝을 그만큼
	 * 앞당겨 잡는다 */
	endp = (end - sizeof(struct hrt) + 1);

	/* [한국어] **16바이트씩 건너뛰며 훑는다.** 그 시절 펌웨어 표들이 문단 경계
	 * (16바이트)에 정렬되는 관례를 따랐기 때문으로 보이나,
	 * 그 관례의 근거 문서는 이 트리에 없다 */
	for (fp = begin; fp <= endp; fp += 16) {
		/* [한국어] **첫 서명 바이트를 읽는다.** ROM 이 __iomem 영역이라 직접
		 * 역참조할 수 없어 readb 를 쓴다 */
		temp1 = readb(fp + SIG0);
		/* [한국어] 두 번째 서명 바이트 */
		temp2 = readb(fp + SIG1);
		/* [한국어] 세 번째 서명 바이트 */
		temp3 = readb(fp + SIG2);
		/* [한국어] 네 번째 서명 바이트 */
		temp4 = readb(fp + SIG3);
		/* [한국어] **네 바이트가 '$HRT' 인지 본다** */
		if (temp1 == '$' &&
		    temp2 == 'H' &&
		    temp3 == 'R' &&
		    temp4 == 'T') {
			/* [한국어] 찾았음을 표시한다 */
			status = 1;
			break;
		}
	}

	/* [한국어] 찾았는지 확인한다 */
	if (!status)
		/* [한국어] **못 찾았으면 NULL 로 만든다.** 루프가 끝까지 돌면 fp 가 endp 를
		 * 넘어선 값이라, 그것을 그대로 돌려주면 안 되기 때문이다 */
		fp = NULL;

	/* [한국어] 결과를 로그로 남긴다 */
	dbg("Discovered Hotplug Resource Table at %p\n", fp);
	/* [한국어] 찾은 주소를 돌려준다. 못 찾았으면 NULL 이다 */
	return fp;
}


/* [한국어]
 * cpqhp_configure_device - 리눅스 쪽 struct pci_dev 를 만들어 드라이버를 붙인다
 *
 * @ctrl: 대상 컨트롤러.
 * @func: 대상 PCI 함수 정보.
 * @return: 항상 0. **실패해도 0 을 돌려준다.**
 *
 * **이 파일에서 하드웨어가 아니라 커널 자료구조를 다루는 두 함수 중
 * 하나다.** 설정공간을 다 채운 뒤, 리눅스가 그 카드를 인식하고
 * 드라이버를 붙이도록 만드는 마지막 단계다.
 *
 *   1) pci_lock_rescan_remove 로 전역 재훑기 락을 잡는다. 버스 스캔과
 *      장치 제거를 직렬화하는 커널 공통 락이다.
 *   2) 이미 pci_dev 가 있으면 그것을 쓰고, 없으면 찾아본다.
 *   3) 그래도 없으면 **pci_scan_slot 으로 새로 훑는다.** 찾은 것이 있으면
 *      pci_bus_add_devices 로 등록해 드라이버가 붙게 한다.
 *   4) 브리지면 pci_hp_add_bridge 로 버스 번호를 배정하고,
 *      그 아래 버스의 장치들도 등록한다.
 *   5) pci_dev_put 으로 참조를 놓는다.
 *
 * **참조 계수 처리가 미묘하다.** pci_get_domain_bus_and_slot 은 참조를
 * 올려 주고, 마지막의 pci_dev_put 이 그것을 내린다. 그런데
 * func->pci_dev 에는 포인터가 그대로 남으므로, 참조 없이 포인터만
 * 보관하는 셈이 된다. 그 포인터를 나중에 쓰는 곳이
 * cpqphp_ctrl.c 의 board_added(`!new_slot->pci_dev` 검사)인데,
 * 값이 NULL 인지만 보므로 실질적 문제는 없다.
 *
 * **out 라벨이 pci_dev 가 여전히 NULL 인 경우로만 간다.** 그 경우
 * pci_dev_put 을 건너뛰는데, 애초에 참조를 얻지 못했으므로 맞다.
 *
 * 실패해도 0 을 돌려주는 것에 주의 -- 호출자인 board_added 가
 * 반환값을 보지 않으므로 실질적 차이는 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(커널 스레드). 락을 잡고 잠들 수 있다.
 *
 * 호출 체인:
 *   cpqphp_ctrl.c 의 board_added → [이 함수]
 *     → pci_scan_slot, pci_bus_add_devices, pci_hp_add_bridge
 */
int cpqhp_configure_device(struct controller *ctrl, struct pci_func *func)
{
	struct pci_bus *child;
	/* [한국어] 스캔에서 찾은 장치 수 */
	int num;

	pci_lock_rescan_remove();

	/* [한국어] **이미 있으면 그것을 쓰고, 없으면 찾아본다** */
	if (func->pci_dev == NULL)
		func->pci_dev = pci_get_domain_bus_and_slot(0, func->bus,
							PCI_DEVFN(func->device,
							func->function));

	/* No pci device, we need to create it then */
	if (func->pci_dev == NULL) {
		/* [한국어] 아직 없음을 로그로 남긴다 */
		dbg("INFO: pci_dev still null\n");

		/* [한국어] **슬롯을 새로 훑는다.** 리눅스가 아직 모르는 장치이므로 스캔이 필요하다 */
		num = pci_scan_slot(ctrl->pci_dev->bus, PCI_DEVFN(func->device, func->function));
		/* [한국어] 찾은 것이 있으면 등록한다 */
		if (num)
			pci_bus_add_devices(ctrl->pci_dev->bus);

		/* [한국어] 스캔한 뒤 다시 찾아본다 */
		func->pci_dev = pci_get_domain_bus_and_slot(0, func->bus,
							PCI_DEVFN(func->device,
							func->function));
		/* [한국어] 찾았는지 확인한다 */
		if (func->pci_dev == NULL) {
			/* [한국어] 그래도 못 찾았음을 로그로 남긴다 */
			dbg("ERROR: pci_dev still null\n");
			goto out;
		}
	}

	/* [한국어] **브리지면 버스 번호를 배정한다** */
	if (func->pci_dev->hdr_type == PCI_HEADER_TYPE_BRIDGE) {
		pci_hp_add_bridge(func->pci_dev);
		/* [한국어] 하위 버스를 꺼낸다 */
		child = func->pci_dev->subordinate;
		/* [한국어] 하위 버스가 있으면 그 장치들도 등록한다 */
		if (child)
			pci_bus_add_devices(child);
	}

	pci_dev_put(func->pci_dev);

 out:
	pci_unlock_rescan_remove();
	return 0;
}


/* [한국어]
 * cpqhp_unconfigure_device - 리눅스 쪽 struct pci_dev 를 제거한다
 *
 * @func: 대상 PCI 함수 정보.
 * @return: 항상 0.
 *
 * cpqhp_configure_device 의 짝이다. 카드를 뽑기 전에 리눅스가 그 카드를
 * 잊게 만든다.
 *
 * **함수 여덟 개를 모두 훑는다.** 다중 함수 카드일 수 있으므로
 * 0 부터 7 까지 돌며 존재하는 것마다 제거한다. func->function 하나만
 * 보지 않는 이유가 그것이다.
 *
 * pci_get_domain_bus_and_slot 으로 참조를 얻은 뒤
 * **pci_dev_put 을 먼저 부르고 pci_stop_and_remove_bus_device 를
 * 나중에 부른다.** 순서가 거꾸로 보이지만, 그 함수가 자기 안에서
 * 필요한 참조를 다시 잡기 때문에 성립하는 관용구다.
 *
 * **이 함수가 반드시 먼저 불려야 한다.** cpqphp_ctrl.c 의 remove_board 가
 * 맨 처음 이것을 부르는데, 드라이버가 아직 쓰고 있는 카드의 BAR 를
 * 건드리면 안 되기 때문이다. 실패하면 remove_board 가 곧바로 1 을
 * 돌려주고 제거를 포기한다.
 *
 * pci_lock_rescan_remove 로 감싸는 이유는 configure 쪽과 같다 --
 * 버스 재훑기와 장치 제거가 겹치면 안 되기 때문이다.
 *
 * **항상 0 을 돌려준다.** remove_board 는 그 값을 오류로 검사하지만
 * 실제로는 실패를 알릴 수 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(커널 스레드).
 *
 * 호출 체인:
 *   cpqphp_ctrl.c 의 remove_board → [이 함수]
 *     → pci_get_domain_bus_and_slot, pci_stop_and_remove_bus_device
 */
int cpqhp_unconfigure_device(struct pci_func *func)
{
	int j;

	/* [한국어] 어느 장치를 제거하는지 로그로 남긴다 */
	dbg("%s: bus/dev/func = %x/%x/%x\n", __func__, func->bus, func->device, func->function);

	pci_lock_rescan_remove();
	/* [한국어] **함수 여덟 개를 모두 훑는다** -- 다중 함수 카드일 수 있기 때문이다 */
	for (j = 0; j < 8 ; j++) {
		/* [한국어] 그 함수의 pci_dev 를 찾는다 */
		struct pci_dev *temp = pci_get_domain_bus_and_slot(0,
							func->bus,
							PCI_DEVFN(func->device,
							j));
		/* [한국어] **존재하면 참조를 놓고 제거한다.** pci_dev_put 을 먼저 부르는 것이
		 * 거꾸로 보이지만, 그다음 함수가 자기 안에서 필요한 참조를 다시 잡는
		 * 관용구다 */
		if (temp) {
			pci_dev_put(temp);
			pci_stop_and_remove_bus_device(temp);
		}
	}
	pci_unlock_rescan_remove();
	return 0;
}

/*
 * cpqhp_set_irq
 *
 * @bus_num: bus number of PCI device
 * @dev_num: device number of PCI device
 * @slot: pointer to u8 where slot number will be returned
 */
/* [한국어]
 * cpqhp_set_irq - 레거시 모드에서 IRQ 라우팅을 프로그래밍한다
 *
 * @bus_num: PCI 버스 번호.
 * @dev_num: 장치 번호.
 * @int_pin: 인터럽트 핀(1~4 가 INTA~INTD).
 * @irq_num: 배정할 IRQ 번호.
 * @return: 0 성공, 그 밖에는 오류.
 *
 * **cpqhp_legacy_mode 가 참일 때만 무언가 한다.** 아니면 곧바로 0 을
 * 돌려준다. 그 전역은 cpqphp_core.c 가 정한다.
 *
 * 두 가지를 한다.
 *
 *   1) **가짜 struct pci_dev 와 struct pci_bus 를 만든다.**
 *      pcibios_set_irq_routing 이 pci_dev 포인터를 요구하는데 여기에는
 *      진짜 pci_dev 가 없기 때문이다. devfn 과 버스 번호만 채우고
 *      나머지는 쓰레기 값인 채로 넘긴 뒤 곧바로 해제한다.
 *      지금 기준으로는 위험한 관용구이지만, 그 API 가 그 두 필드만
 *      본다는 전제에 기댄 것이다.
 *
 *   2) **ELCR(Edge/Level Control Register)을 직접 두드린다.**
 *      0x4d0 과 0x4d1 포트를 읽어 해당 IRQ 비트를 세우고 되쓴다.
 *      위의 원문 주석이 "이것은 x86 전용" 이라고 밝힌다. 그 두 포트는
 *      PC 호환기의 인터럽트 컨트롤러가 각 IRQ 를 에지로 볼지 레벨로
 *      볼지 정하는 자리이며, PCI 인터럽트는 레벨이라 비트를 세운다.
 *
 * **pcibios_set_irq_routing 이 성공하면 곧바로 반환한다** --
 * `if (!rc) return !rc;` 라는 특이한 표현인데, rc 가 0 이면 0 을
 * 돌려준다는 뜻이다. 즉 성공하면 ELCR 을 건드리지 않고,
 * 실패했을 때만 아래로 내려가 ELCR 을 직접 손본다.
 *
 * pcibios_set_irq_routing, inb, outb 는 **arch/ 와 include/asm 이 이
 * 스파스 체크아웃에 없어 확인하지 못했다.** 이름과 위의 원문 주석이
 * 알려 주는 범위에서만 설명한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. kmalloc(GFP_KERNEL)을 쓴다.
 *
 * 호출 체인:
 *   cpqphp_ctrl.c 의 configure_new_function → [이 함수]
 */
int cpqhp_set_irq(u8 bus_num, u8 dev_num, u8 int_pin, u8 irq_num)
{
	int rc = 0;

	/* [한국어] **레거시 모드일 때만 무언가 한다** */
	if (cpqhp_legacy_mode) {
		/* [한국어] 가짜 장치 */
		struct pci_dev *fakedev;
		/* [한국어] 가짜 버스 */
		struct pci_bus *fakebus;
		/* [한국어] ELCR 값을 담을 변수 */
		u16 temp_word;

		/* [한국어] **가짜 struct pci_dev 를 만든다.** pcibios_set_irq_routing 이
		 * pci_dev 포인터를 요구하는데 여기에는 진짜가 없기 때문이다.
		 * 두 필드만 채우고 나머지는 쓰레기 값인 채로 넘긴다 */
		fakedev = kmalloc_obj(*fakedev);
		/* [한국어] 가짜 버스도 만든다 */
		fakebus = kmalloc_obj(*fakebus);
		/* [한국어] 둘 중 하나라도 실패하면 전부 해제한다 */
		if (!fakedev || !fakebus) {
			kfree(fakedev);
			kfree(fakebus);
			return -ENOMEM;
		}

		/* [한국어] **devfn 을 만든다** -- 장치 번호를 3비트 왼쪽으로 밀면 함수 0 이다 */
		fakedev->devfn = dev_num << 3;
		/* [한국어] 가짜 장치를 가짜 버스에 연결한다 */
		fakedev->bus = fakebus;
		/* [한국어] 버스 번호를 채운다 */
		fakebus->number = bus_num;
		/* [한국어] 인자를 로그로 남긴다 */
		dbg("%s: dev %d, bus %d, pin %d, num %d\n",
		    __func__, dev_num, bus_num, int_pin, irq_num);
		/* [한국어] **IRQ 라우팅을 설정한다.** 핀 번호에서 1 을 빼는 것은 그 API 가
		 * 0 기반을 쓰기 때문으로 보인다.
		 * **arch/ 가 이 트리에 없어 그 함수의 정의를 확인하지 못했다** */
		rc = pcibios_set_irq_routing(fakedev, int_pin - 1, irq_num);
		kfree(fakedev);
		kfree(fakebus);
		/* [한국어] 결과를 로그로 남긴다 */
		dbg("%s: rc %d\n", __func__, rc);
		/* [한국어] 라우팅 설정이 성공했는지 본다 */
		if (!rc)
			/* [한국어] **성공하면 곧바로 돌려준다.** rc 가 0 이면 !rc 도 0 이라 결과가
			 * 같은데, 표현이 에둘러 있다 */
			return !rc;

		/* set the Edge Level Control Register (ELCR) */
		temp_word = inb(0x4d0);
		/* [한국어] 상위 바이트를 읽어 합친다 */
		temp_word |= inb(0x4d1) << 8;

		/* [한국어] **해당 IRQ 의 비트를 세운다** -- PCI 인터럽트는 레벨이라
		 * 에지가 아닌 레벨로 표시한다 */
		temp_word |= 0x01 << irq_num;

		/* This should only be for x86 as it sets the Edge Level
		 * Control Register
		 */
		outb((u8)(temp_word & 0xFF), 0x4d0);
		/* [한국어] 상위 바이트를 쓴다 */
		outb((u8)((temp_word & 0xFF00) >> 8), 0x4d1);
		/* [한국어] 성공으로 표시한다 */
		rc = 0;
	}

	/* [한국어] **레거시 모드가 아니면 0 을 그대로 돌려준다** */
	return rc;
}


/* [한국어]
 * PCI_ScanBusForNonBridge - 버스에서 브리지가 아닌 첫 장치를 찾는다
 *
 * @ctrl:     대상 컨트롤러. pci_bus 를 빌려 쓴다.
 * @bus_num:  훑을 버스 번호.
 * @dev_num:  찾은 장치 번호를 담을 곳(출력).
 * @return: 0 이면 찾음, -1 이면 못 찾음, 그 밖에는 설정 읽기 오류.
 *
 * 브리지 뒤에 실제 장치가 있는지 확인하는 데 쓴다. 슬롯 번호로 장치를
 * 찾다가 브리지를 만났을 때, 그 뒤로 한 단계 더 내려가기 위한 함수다.
 *
 * **위의 원문 주석이 중요한 사실을 밝힌다** -- 원래는 브리지 아래로
 * 재귀해 내려가는 코드가 있었는데, 디버그 출력만 그렇게 말했을 뿐
 * **실제로는 한 번도 재귀하지 않았기에 제거되었다.** 그래서 지금은
 * 브리지를 만나면 경고만 찍고 넘어간다. pr_warn 의 문구가
 * "missing feature: bridge scan recursion not implemented" 인 것이 그것이다.
 *
 * **이 파일에서 pr_ 계열을 쓰는 유일한 곳이다.** 파일 맨 위에 pr_fmt 를
 * 정의해 두었는데 정작 쓰는 곳이 여기뿐이고, 나머지는 전부 옛
 * dbg/err/info 매크로를 쓴다. 두 세대의 로그 관용구가 한 파일에 섞여
 * 있는 셈이며, 이 줄이 나중에 추가되었음을 보여 준다.
 *
 * 루프 조건이 `tdevice < 0xFF` 인 것에 주의 -- tdevice 를 devfn 으로
 * 쓰므로 실제로는 장치 0~31 의 함수 0~7 을 훑는 셈이다.
 * pci_bus_read_dev_vendor_id 로 먼저 접근 가능한지 보고, 안 되면
 * 건너뛴다.
 *
 * 클래스 코드를 8비트 오른쪽으로 밀어 PCI_TO_PCI_BRIDGE_CLASS
 * (cpqphp.h:364, 0x00060400)와 견준다. PCI_CLASS_REVISION 레지스터의
 * 상위 24비트가 클래스 코드이기 때문이다.
 *
 * **ret 초기값이 -1 이고, 브리지를 만나면 0 으로 바꾼다.** 그래서
 * 브리지만 있고 일반 장치가 없는 버스에서는 0 을 돌려주는데,
 * 그때 *dev_num 은 채워지지 않은 채다. 호출자가 그 값을 쓰므로
 * 주의가 필요하나, 코드는 손대지 않고 사실만 적어 둔다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe 경로).
 *
 * 호출 체인:
 *   PCI_GetBusDevHelper → [이 함수]
 */
static int PCI_ScanBusForNonBridge(struct controller *ctrl, u8 bus_num, u8 *dev_num)
{
	u16 tdevice;
	/* [한국어] 설정공간에서 읽은 값을 담을 변수 */
	u32 work;
	/* [한국어] **못 찾음을 뜻하는 -1 로 시작한다.** 다만 브리지를 만나면 0 으로
	 * 바뀌므로, 브리지만 있고 일반 장치가 없으면 0 을 돌려주면서
	 * *dev_num 은 채워지지 않은 채가 된다 */
	int ret = -1;

	/* [한국어] 임시 pci_bus 의 번호를 맞춘다 */
	ctrl->pci_bus->number = bus_num;

	for (tdevice = 0; tdevice < 0xFF; tdevice++) {
		/* Scan for access first */
		if (!pci_bus_read_dev_vendor_id(ctrl->pci_bus, tdevice, &work, 0))
			continue;
		/* [한국어] 클래스 코드를 읽는다 */
		ret = pci_bus_read_config_dword(ctrl->pci_bus, tdevice, PCI_CLASS_REVISION, &work);
		/* [한국어] 읽기에 실패하면 다음 장치를 본다 */
		if (ret)
			continue;
		dbg("Looking for nonbridge bus_num %d dev_num %d\n", bus_num, tdevice);
		/* Yep we got one. Not a bridge ? */
		if ((work >> 8) != PCI_TO_PCI_BRIDGE_CLASS) {
			*dev_num = tdevice;
			dbg("found it !\n");
			return 0;
		} else {
			/*
			 * XXX: Code whose debug printout indicated
			 * recursion to buses underneath bridges might be
			 * necessary was removed because it never did
			 * any recursion.
			 */
			ret = 0;
			/* [한국어] **재귀가 구현되지 않았음을 경고한다.** 위의 원문 주석대로 원래는
			 * 브리지 아래로 내려가는 코드가 있었으나, 디버그 출력만 그렇게 말했을
			 * 뿐 실제로는 한 번도 재귀하지 않아 제거되었다.
			 * **이 파일에서 pr_ 계열을 쓰는 유일한 곳이다** */
			pr_warn("missing feature: bridge scan recursion not implemented\n");
		}
	}


	return ret;
}


/* [한국어]
 * PCI_GetBusDevHelper - 슬롯 번호로 버스·장치 번호를 찾는다
 *
 * @ctrl:     대상 컨트롤러.
 * @bus_num:  찾은 버스 번호를 담을 곳(출력).
 * @dev_num:  찾은 장치 번호를 담을 곳(출력).
 * @slot:     찾을 물리 슬롯 번호.
 * @nobridge: 1 이면 브리지를 건너뛰고 그 뒤의 실제 장치를 찾는다.
 * @return: 0 이면 찾음, -1 이면 못 찾음.
 *
 * **BIOS 의 IRQ 라우팅 표를 슬롯 번호로 검색한다.** 그 표
 * (cpqhp_routing_table)는 펌웨어가 남긴 것으로, 슬롯 번호와
 * 버스·장치 번호의 대응을 담고 있다. cpqhp_routing_table_length 가
 * 표의 항목 수를 계산해 준다.
 *
 * nobridge 가 0 이면 단순하다 -- 슬롯 번호가 맞는 항목을 찾아
 * 버스·장치 번호를 돌려주고 끝낸다.
 *
 * nobridge 가 1 이면 한 단계 더 간다.
 *   벤더 ID 를 읽어 장치가 없으면 그대로 돌려준다.
 *   클래스 코드가 브리지면 보조 버스 번호를 읽어
 *     PCI_ScanBusForNonBridge 로 그 뒤의 실제 장치를 찾는다.
 *   브리지가 아니면 그대로 돌려준다.
 *
 * **다만 이 파일에서 nobridge 를 1 로 넘기는 호출자가 없다.**
 * 유일한 호출자인 cpqhp_get_bus_dev 가 0 을 넘기며, 그 주석도
 * "평범한 경우(브리지 허용)" 라고 적는다. 즉 브리지 처리 경로는
 * 현재 죽은 코드다. PCI_ScanBusForNonBridge 의 재귀가 제거된 것과
 * 같은 맥락으로 보이나, 코드가 그 인과를 적어 두지는 않았다.
 *
 * `if (!nobridge || PCI_POSSIBLE_ERROR(work))` 조건에 주의 --
 * nobridge 가 0 이면 곧바로 성공을 돌려주므로, 그 아래 브리지 처리는
 * nobridge 가 1 이고 장치가 존재할 때만 실행된다.
 *
 * ctrl->pci_bus->number 를 바꿔 가며 설정공간을 읽는 관용구가 쓰인다 --
 * 임시 pci_bus 하나를 돌려 쓰는 이 드라이버의 방식이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   cpqhp_get_bus_dev → [이 함수] → PCI_ScanBusForNonBridge
 */
static int PCI_GetBusDevHelper(struct controller *ctrl, u8 *bus_num, u8 *dev_num, u8 slot, u8 nobridge)
{
	int loop, len;
	/* [한국어] 설정공간 읽기 결과를 담을 변수 */
	u32 work;
	/* [한국어] 표에서 읽은 값을 담을 변수들 */
	u8 tbus, tdevice, tslot;

	/* [한국어] **BIOS 의 IRQ 라우팅 표의 항목 수를 구한다.**
	 * cpqhp_routing_table_length 가 cpqphp.h 에 있다 */
	len = cpqhp_routing_table_length();
	/* [한국어] **표의 모든 항목을 훑는다** */
	for (loop = 0; loop < len; ++loop) {
		/* [한국어] 이 항목의 버스 번호 */
		tbus = cpqhp_routing_table->slots[loop].bus;
		/* [한국어] 이 항목의 장치·함수 번호 */
		tdevice = cpqhp_routing_table->slots[loop].devfn;
		/* [한국어] 이 항목의 슬롯 번호 */
		tslot = cpqhp_routing_table->slots[loop].slot;

		if (tslot == slot) {
			*bus_num = tbus;
			*dev_num = tdevice;
			ctrl->pci_bus->number = tbus;
			/* [한국어] 그 자리에 장치가 있는지 벤더 ID 로 확인한다 */
			pci_bus_read_config_dword(ctrl->pci_bus, *dev_num, PCI_VENDOR_ID, &work);
			/* [한국어] **nobridge 가 0 이면 곧바로 성공을 돌려준다.** 그래서 아래 브리지
			 * 처리는 nobridge 가 1 이고 장치가 존재할 때만 실행되는데,
			 * **이 파일에서 nobridge 를 1 로 넘기는 호출자가 없다** */
			if (!nobridge || PCI_POSSIBLE_ERROR(work))
				return 0;

			/* [한국어] 찾은 버스·장치 번호를 로그로 남긴다 */
			dbg("bus_num %d devfn %d\n", *bus_num, *dev_num);
			/* [한국어] 클래스 코드를 읽는다 */
			pci_bus_read_config_dword(ctrl->pci_bus, *dev_num, PCI_CLASS_REVISION, &work);
			/* [한국어] 클래스 코드 비교를 로그로 남긴다 */
			dbg("work >> 8 (%x) = BRIDGE (%x)\n", work >> 8, PCI_TO_PCI_BRIDGE_CLASS);

			/* [한국어] **브리지인지 본다** */
			if ((work >> 8) == PCI_TO_PCI_BRIDGE_CLASS) {
				/* [한국어] 보조 버스 번호를 읽는다 */
				pci_bus_read_config_byte(ctrl->pci_bus, *dev_num, PCI_SECONDARY_BUS, &tbus);
				/* [한국어] 어느 버스를 훑는지 로그로 남긴다 */
				dbg("Scan bus for Non Bridge: bus %d\n", tbus);
				if (PCI_ScanBusForNonBridge(ctrl, tbus, dev_num) == 0) {
					*bus_num = tbus;
					return 0;
				}
			/* [한국어] 브리지가 아니면 그대로 돌려준다 */
			} else
				return 0;
		}
	}
	/* [한국어] **표를 다 훑었는데 못 찾았다** */
	return -1;
}


/* [한국어]
 * cpqhp_get_bus_dev - 슬롯 번호로 버스·장치 번호를 찾는다 (브리지 허용)
 *
 * @ctrl:    대상 컨트롤러.
 * @bus_num: 찾은 버스 번호를 담을 곳(출력).
 * @dev_num: 찾은 장치 번호를 담을 곳(출력).
 * @slot:    찾을 물리 슬롯 번호.
 * @return: 0 이면 찾음, -1 이면 못 찾음.
 *
 * PCI_GetBusDevHelper 에 nobridge = 0 을 넘기는 **한 줄짜리 껍데기** 다.
 * 위의 원문 주석이 "평범한 경우(브리지 허용)" 라고 그 뜻을 적었다.
 *
 * 껍데기를 따로 두는 이유는 nobridge = 1 인 짝을 함께 두려던 흔적으로
 * 보이나, 그런 함수는 이 파일에 없다. 결과적으로 도우미 함수의
 * 브리지 처리 경로는 아무도 쓰지 않는다.
 *
 * cpqphp_core.c 가 슬롯을 등록할 때 이 함수로 각 슬롯의 버스·장치
 * 번호를 알아낸다. **그 파일은 아직 주석되지 않았으므로** 호출 맥락은
 * 이름과 인자에서 읽어 낸 것이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe 경로).
 *
 * 호출 체인:
 *   cpqphp_core.c → [이 함수] → PCI_GetBusDevHelper
 */
int cpqhp_get_bus_dev(struct controller *ctrl, u8 *bus_num, u8 *dev_num, u8 slot)
{
	/* plain (bridges allowed) */
	return PCI_GetBusDevHelper(ctrl, bus_num, dev_num, slot, 0);
}


/* More PCI configuration routines; this time centered around hotplug
 * controller
 */


/*
 * cpqhp_save_config
 *
 * Reads configuration for all slots in a PCI bus and saves info.
 *
 * Note:  For non-hot plug buses, the slot # saved is the device #
 *
 * returns 0 if success
 */
/* [한국어]
 * cpqhp_save_config - 버스의 모든 카드 설정공간을 사본으로 뜬다
 *
 * @ctrl:       대상 컨트롤러.
 * @busnumber:  훑을 버스 번호.
 * @is_hot_plug: 0 이 아니면 **슬롯 마스크** 로 해석한다. 0 이면 일반 버스.
 * @return: 0 성공, 그 밖에는 오류.
 *
 * 드라이버가 올라올 때 시스템에 이미 꽂혀 있던 카드를 전부 기록한다.
 * **나중에 같은 카드로 교체되었을 때 되쓸 밑천이 여기서 만들어진다.**
 *
 * is_hot_plug 인자가 특이하다 -- 불리언이 아니라 **슬롯 마스크** 다.
 * 위의 원문 주석이 그 사실을 밝히며, 상위 니블이 첫 슬롯 번호,
 * 하위 니블이 슬롯 개수다. 그것으로 훑을 장치 번호 범위를 정한다.
 * 0 이면 핫플러그 버스가 아니므로 0~0x1F 전체를 훑는다.
 *
 * 장치마다:
 *   1) 벤더 ID 를 읽는다. 전부 F 면 빈 슬롯이다.
 *      **핫플러그 버스라면 빈 슬롯에도 자리표 노드를 만든다** --
 *      그래야 나중에 카드가 꽂혔을 때 cpqhp_slot_find 가 찾는다.
 *      일반 버스면 그냥 건너뛴다.
 *   2) 헤더 타입의 다중 함수 비트를 보고 최대 함수 수를 8 또는 1 로 잡는다.
 *   3) 함수마다:
 *      브리지면 **보조 버스 번호를 읽어 자기 자신을 재귀 호출한다.**
 *        그때 is_hot_plug 를 0 으로 넘긴다 -- 하위 버스는 핫플러그
 *        대상이 아니기 때문이다. 재귀에서 돌아온 뒤
 *        **ctrl->pci_bus->number 를 원래대로 되돌린다.**
 *      기존 노드를 찾거나 새로 만든다.
 *      **설정공간 128바이트(dword 서른두 개)를 통째로 복사한다.**
 *      pci_dev 포인터도 찾아 둔다(곧 pci_dev_put 으로 참조를 놓는다).
 *   4) 다음으로 존재하는 함수를 찾는다.
 *
 * **DevError 변수가 0 으로만 쓰인다.** 매 바퀴 0 으로 초기화한 뒤
 * new_slot->status 에 대입하는데, 그 사이에 값을 바꾸는 코드가 없다.
 * 이름으로 보아 원래는 오류를 담으려던 것으로 보이나 지금은 0 을
 * 넣는 것과 같다.
 *
 * 기존 노드를 찾는 while 루프에 주의 -- cpqhp_slot_find 는 index 로
 * "같은 device 의 몇 번째" 를 찾으므로, function 이 맞는 것을 찾을
 * 때까지 index 를 늘려 가며 반복한다.
 *
 * **임시 pci_bus 를 돌려 쓰는 관용구** 가 여기서도 나온다.
 * ctrl->pci_bus->number 에 대입한 뒤 읽고, 재귀에서 돌아오면 되돌린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe 경로). 재귀로 스택을 쓴다.
 *
 * 호출 체인:
 *   cpqphp_core.c / cpqhp_save_slot_config → [이 함수]
 *     → 자기 자신(재귀), cpqhp_slot_create, cpqhp_slot_find
 */
int cpqhp_save_config(struct controller *ctrl, int busnumber, int is_hot_plug)
{
	long rc;
	/* [한국어] 클래스 코드를 담을 변수 */
	u8 class_code;
	/* [한국어] 헤더 타입을 담을 변수 */
	u8 header_type;
	/* [한국어] 벤더/장치 ID 를 담을 변수 */
	u32 ID;
	/* [한국어] 보조 버스 번호를 담을 변수 */
	u8 secondary_bus;
	/* [한국어] 이번에 만들거나 찾을 노드 */
	struct pci_func *new_slot;
	/* [한국어] 정수로 바꾼 보조 버스 번호 */
	int sub_bus;
	/* [한국어] 훑을 첫 장치 번호 */
	int FirstSupported;
	/* [한국어] 훑을 마지막 장치 번호 */
	int LastSupported;
	/* [한국어] 이 카드의 최대 함수 수 */
	int max_functions;
	/* [한국어] 현재 훑고 있는 함수 번호 */
	int function;
	/* [한국어] **0 으로만 쓰이는 변수.** 위의 주석 참조 */
	u8 DevError;
	/* [한국어] 현재 훑고 있는 장치 번호 */
	int device = 0;
	/* [한국어] 설정공간을 훑을 첨자 */
	int cloop = 0;
	/* [한국어] 함수를 찾았는지 표시할 플래그 */
	int stop_it;
	/* [한국어] 같은 장치의 함수를 순회할 첨자 */
	int index;
	/* [한국어] 장치·함수를 합친 번호 */
	u16 devfn;

	/* Decide which slots are supported */

	if (is_hot_plug) {
		/*
		 * is_hot_plug is the slot mask
		 */
		FirstSupported = is_hot_plug >> 4;
		/* [한국어] **하위 니블이 슬롯 개수다.** 그것을 더하고 1 을 빼 마지막 장치
		 * 번호를 구한다 */
		LastSupported = FirstSupported + (is_hot_plug & 0x0F) - 1;
	} else {
		/* [한국어] 장치 0 부터 시작한다 */
		FirstSupported = 0;
		/* [한국어] **일반 버스면 장치 0~0x1F 전체를 훑는다** */
		LastSupported = 0x1F;
	}

	/* Save PCI configuration space for all devices in supported slots */
	ctrl->pci_bus->number = busnumber;
	/* [한국어] **정한 범위의 장치를 모두 훑는다** */
	for (device = FirstSupported; device <= LastSupported; device++) {
		/* [한국어] 없는 장치를 읽으면 전부 1 이 돌아오므로 그 값으로 초기화해 둔다 */
		ID = 0xFFFFFFFF;
		/* [한국어] 벤더/장치 ID 를 읽는다 */
		rc = pci_bus_read_config_dword(ctrl->pci_bus, PCI_DEVFN(device, 0), PCI_VENDOR_ID, &ID);

		/* [한국어] **빈 슬롯인지 본다.** 핫플러그 버스라면 자리표 노드를 만들고,
		 * 일반 버스면 그냥 건너뛴다 */
		if (ID == 0xFFFFFFFF) {
			if (is_hot_plug) {
				/* Setup slot structure with entry for empty
				 * slot
				 */
				new_slot = cpqhp_slot_create(busnumber);
				/* [한국어] 할당 실패를 확인한다 */
				if (new_slot == NULL)
					/* [한국어] 노드를 못 만들면 포기한다 */
					return 1;

				/* [한국어] 버스 번호를 기록한다 */
				new_slot->bus = (u8) busnumber;
				/* [한국어] 장치 번호를 기록한다 */
				new_slot->device = (u8) device;
				/* [한국어] 함수 0 으로 둔다 */
				new_slot->function = 0;
				/* [한국어] **보드가 아님을 표시한다** -- 빈 슬롯 자리표다 */
				new_slot->is_a_board = 0;
				/* [한국어] 존재 상태를 0 으로 둔다 */
				new_slot->presence_save = 0;
				/* [한국어] 레버 상태도 0 으로 둔다 */
				new_slot->switch_save = 0;
			}
			continue;
		}

		/* [한국어] **클래스 코드를 읽는다.** 오프셋 0x0B 를 상수 이름 없이 직접 쓴다 */
		rc = pci_bus_read_config_byte(ctrl->pci_bus, PCI_DEVFN(device, 0), 0x0B, &class_code);
		/* [한국어] 읽기 실패를 확인한다 */
		if (rc)
			/* [한국어] 읽기에 실패하면 그대로 올린다 */
			return rc;

		/* [한국어] 헤더 타입을 읽는다 */
		rc = pci_bus_read_config_byte(ctrl->pci_bus, PCI_DEVFN(device, 0), PCI_HEADER_TYPE, &header_type);
		/* [한국어] 읽기 실패를 확인한다 */
		if (rc)
			/* [한국어] 읽기에 실패하면 그대로 올린다 */
			return rc;

		/* If multi-function device, set max_functions to 8 */
		if (header_type & PCI_HEADER_TYPE_MFD)
			/* [한국어] 다중 함수 카드는 함수가 최대 여덟 개다 */
			max_functions = 8;
		else
			/* [한국어] 단일 함수 카드다 */
			max_functions = 1;

		/* [한국어] 함수 0 부터 시작한다 */
		function = 0;

		do {
			/* [한국어] **0 으로 초기화하지만 값을 바꾸는 코드가 없다.** 아래에서
			 * new_slot->status 에 대입하므로 결국 0 을 넣는 것과 같다.
			 * 이름으로 보아 원래는 오류를 담으려던 것으로 보인다 */
			DevError = 0;
			if ((header_type & PCI_HEADER_TYPE_MASK) == PCI_HEADER_TYPE_BRIDGE) {
				/* Recurse the subordinate bus
				 * get the subordinate bus number
				 */
				rc = pci_bus_read_config_byte(ctrl->pci_bus, PCI_DEVFN(device, function), PCI_SECONDARY_BUS, &secondary_bus);
				/* [한국어] 읽기 실패를 확인한다 */
				if (rc) {
					/* [한국어] 읽기에 실패하면 그대로 올린다 */
					return rc;
				} else {
					/* [한국어] 정수로 바꾼다 */
					sub_bus = (int) secondary_bus;

					/* Save secondary bus cfg spc
					 * with this recursive call.
					 */
					rc = cpqhp_save_config(ctrl, sub_bus, 0);
					/* [한국어] 재귀 실패를 확인한다 */
					if (rc)
						/* [한국어] 실패하면 그대로 올린다 */
						return rc;
					/* [한국어] **재귀에서 돌아왔으므로 버스 번호를 되돌린다** */
					ctrl->pci_bus->number = busnumber;
				}
			}

			/* [한국어] 순회를 처음부터 시작한다 */
			index = 0;
			/* [한국어] 첫 번째 함수부터 찾는다 */
			new_slot = cpqhp_slot_find(busnumber, device, index++);
			/* [한국어] **함수 번호가 맞는 노드를 찾을 때까지 훑는다.** cpqhp_slot_find 는
			 * "같은 device 의 몇 번째" 를 찾으므로 함수 번호로 다시 걸러야 한다 */
			while (new_slot &&
			       (new_slot->function != (u8) function))
				/* [한국어] 같은 장치의 다음 함수를 본다 */
				new_slot = cpqhp_slot_find(busnumber, device, index++);

			if (!new_slot) {
				/* Setup slot structure. */
				new_slot = cpqhp_slot_create(busnumber);
				/* [한국어] 할당 실패를 확인한다 */
				if (new_slot == NULL)
					/* [한국어] 노드를 못 만들면 포기한다 */
					return 1;
			}

			/* [한국어] 버스 번호를 기록한다 */
			new_slot->bus = (u8) busnumber;
			/* [한국어] 장치 번호를 기록한다 */
			new_slot->device = (u8) device;
			/* [한국어] 이 함수의 번호를 기록한다 */
			new_slot->function = (u8) function;
			/* [한국어] 보드로 표시한다 */
			new_slot->is_a_board = 1;
			new_slot->switch_save = 0x10;
			/* In case of unsupported board */
			new_slot->status = DevError;
			/* [한국어] **장치·함수를 devfn 으로 합친다** -- PCI_DEVFN 매크로를 쓰지 않고
			 * 직접 시프트한 것이 그 시절 스타일이다 */
			devfn = (new_slot->device << 3) | new_slot->function;
			/* [한국어] 리눅스 pci_dev 도 찾아 둔다 */
			new_slot->pci_dev = pci_get_domain_bus_and_slot(0,
							new_slot->bus, devfn);

			/* [한국어] **dword 서른두 개 = 128바이트를 통째로 복사한다** */
			for (cloop = 0; cloop < 0x20; cloop++) {
				/* [한국어] **설정공간 dword 하나를 사본에 복사한다.** cloop << 2 가 바이트
				 * 오프셋이다 */
				rc = pci_bus_read_config_dword(ctrl->pci_bus, PCI_DEVFN(device, function), cloop << 2, (u32 *) &(new_slot->config_space[cloop]));
				/* [한국어] 읽기 실패를 확인한다 */
				if (rc)
					/* [한국어] 읽기에 실패하면 그대로 올린다 */
					return rc;
			}

			pci_dev_put(new_slot->pci_dev);

			/* [한국어] 다음 함수 번호로 넘어간다 */
			function++;

			/* [한국어] 찾았는지 표시할 플래그를 내린다 */
			stop_it = 0;

			/* this loop skips to the next present function
			 * reading in Class Code and Header type.
			 */
			while ((function < max_functions) && (!stop_it)) {
				/* [한국어] 다음 함수가 존재하는지 본다 */
				rc = pci_bus_read_config_dword(ctrl->pci_bus, PCI_DEVFN(device, function), PCI_VENDOR_ID, &ID);
				/* [한국어] 벤더 ID 가 전부 1 이면 그 함수는 없다 */
				if (ID == 0xFFFFFFFF) {
					/* [한국어] 없는 함수면 다음으로 넘어간다 */
					function++;
					continue;
				}
				/* [한국어] **클래스 코드를 읽는다.** 오프셋 0x0B 는 PCI 규격상 클래스 코드의
				 * 최상위 바이트인데, 코드가 상수 이름을 쓰지 않고 숫자를 직접 썼다 */
				rc = pci_bus_read_config_byte(ctrl->pci_bus, PCI_DEVFN(device, function), 0x0B, &class_code);
				/* [한국어] 읽기 실패를 확인한다 */
				if (rc)
					/* [한국어] 읽기에 실패하면 그대로 올린다 */
					return rc;

				/* [한국어] 헤더 타입도 읽는다 */
				rc = pci_bus_read_config_byte(ctrl->pci_bus, PCI_DEVFN(device, function), PCI_HEADER_TYPE, &header_type);
				/* [한국어] 읽기 실패를 확인한다 */
				if (rc)
					/* [한국어] 읽기에 실패하면 그대로 올린다 */
					return rc;

				/* [한국어] 함수를 찾았으므로 안쪽 루프를 멈춘다 */
				stop_it++;
			}

		} while (function < max_functions);
	/* [한국어] 장치 번호 범위를 모두 훑었다 */
	}			/* End of FOR loop */

	return 0;
}


/*
 * cpqhp_save_slot_config
 *
 * Saves configuration info for all PCI devices in a given slot
 * including subordinate buses.
 *
 * returns 0 if success
 */
/* [한국어]
 * cpqhp_save_slot_config - 슬롯 하나의 설정공간을 사본으로 뜬다
 *
 * @ctrl:     대상 컨트롤러.
 * @new_slot: 대상 노드.
 * @return: 0 성공, 2 면 카드가 없음.
 *
 * cpqhp_save_config 가 버스 전체를 훑는다면 이쪽은 슬롯 하나만 한다.
 * **카드를 새로 설정한 직후에 불려 사본을 갱신한다** --
 * cpqphp_ctrl.c 의 board_added 가 configure_new_device 로 자원을 배정한
 * 뒤 이것을 부른다.
 *
 * 절차는 cpqhp_save_config 의 안쪽 루프와 거의 같다.
 *   벤더 ID 를 읽어 없으면 2 를 돌려준다.
 *   다중 함수 여부로 최대 함수 수를 정한다.
 *   함수마다 브리지면 하위 버스를 cpqhp_save_config 로 재귀 저장하고,
 *   설정공간 128바이트를 복사한다.
 *   다음으로 존재하는 함수를 찾는다.
 *
 * **cpqhp_save_config 와 달리 노드를 만들지 않는다.** new_slot 하나에
 * 모든 함수의 설정공간을 덮어쓰는 구조라, 다중 함수 카드에서는
 * **마지막 함수의 설정공간만 남는다.** 같은 노드의 config_space 배열에
 * 반복해서 쓰기 때문이다. 코드가 그 판단을 적어 두지는 않았으므로
 * 사실만 기록한다.
 *
 * status 를 0 으로 미는 줄이 루프 안에 있는데, 매 함수마다 같은 필드를
 * 0 으로 되돌리는 셈이다.
 *
 * **rc 를 선언했으나 브리지 경로에서만 쓴다.** 그 밖의
 * pci_bus_read_config_ 호출은 반환값을 받지 않는다 --
 * cpqhp_save_config 가 모든 호출의 반환값을 검사하는 것과 대비되는
 * 느슨함이다.
 *
 * `return(rc)` 처럼 괄호를 쓴 반환문이 있다. 그 시절 스타일이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(커널 스레드).
 *
 * 호출 체인:
 *   cpqphp_ctrl.c 의 board_added → [이 함수] → cpqhp_save_config
 */
int cpqhp_save_slot_config(struct controller *ctrl, struct pci_func *new_slot)
{
	long rc;
	/* [한국어] 클래스 코드를 담을 변수 */
	u8 class_code;
	/* [한국어] 헤더 타입을 담을 변수 */
	u8 header_type;
	/* [한국어] 벤더/장치 ID 를 담을 변수 */
	u32 ID;
	/* [한국어] 보조 버스 번호를 담을 변수 */
	u8 secondary_bus;
	/* [한국어] 정수로 바꾼 보조 버스 번호 */
	int sub_bus;
	/* [한국어] 이 카드의 최대 함수 수 */
	int max_functions;
	/* [한국어] 함수 0 부터 시작한다 */
	int function = 0;
	/* [한국어] 설정공간을 훑을 첨자 */
	int cloop;
	/* [한국어] 함수를 찾았는지 표시할 플래그 */
	int stop_it;

	/* [한국어] 없는 장치를 읽으면 전부 1 이 돌아오므로 그 값으로 초기화해 둔다 */
	ID = 0xFFFFFFFF;

	/* [한국어] 임시 pci_bus 의 번호를 맞춘다 */
	ctrl->pci_bus->number = new_slot->bus;
	/* [한국어] 벤더/장치 ID 를 읽는다 */
	pci_bus_read_config_dword(ctrl->pci_bus, PCI_DEVFN(new_slot->device, 0), PCI_VENDOR_ID, &ID);

	/* [한국어] 카드가 있는지 확인한다 */
	if (ID == 0xFFFFFFFF)
		/* [한국어] **카드가 없으면 2 를 돌려준다** */
		return 2;

	/* [한국어] 클래스 코드를 읽는다. **반환값을 받지 않는다** --
	 * cpqhp_save_config 가 모든 호출을 검사하는 것과 대비되는 느슨함이다 */
	pci_bus_read_config_byte(ctrl->pci_bus, PCI_DEVFN(new_slot->device, 0), 0x0B, &class_code);
	/* [한국어] 헤더 타입을 읽는다 */
	pci_bus_read_config_byte(ctrl->pci_bus, PCI_DEVFN(new_slot->device, 0), PCI_HEADER_TYPE, &header_type);

	/* [한국어] 다중 함수 비트를 본다 */
	if (header_type & PCI_HEADER_TYPE_MFD)
		/* [한국어] 다중 함수 카드는 함수가 최대 여덟 개다 */
		max_functions = 8;
	else
		/* [한국어] 단일 함수 카드다 */
		max_functions = 1;

	/* [한국어] **함수를 하나씩 훑는다.** 노드는 하나뿐이고 그 안의 사본만 바뀐다 */
	while (function < max_functions) {
		if ((header_type & PCI_HEADER_TYPE_MASK) == PCI_HEADER_TYPE_BRIDGE) {
			/*  Recurse the subordinate bus */
			pci_bus_read_config_byte(ctrl->pci_bus, PCI_DEVFN(new_slot->device, function), PCI_SECONDARY_BUS, &secondary_bus);

			/* [한국어] 정수로 바꾼다 */
			sub_bus = (int) secondary_bus;

			/* Save the config headers for the secondary
			 * bus.
			 */
			rc = cpqhp_save_config(ctrl, sub_bus, 0);
			/* [한국어] 실패하면 그대로 올린다 */
			if (rc)
				return(rc);
			/* [한국어] 재귀에서 돌아왔으므로 버스 번호를 되돌린다 */
			ctrl->pci_bus->number = new_slot->bus;

		}

		/* [한국어] 상태를 초기화한다. **루프 안에 있어 매 함수마다 되돌린다** */
		new_slot->status = 0;

		/* [한국어] dword 서른두 개를 훑는다 */
		for (cloop = 0; cloop < 0x20; cloop++)
			/* [한국어] **설정공간 128바이트를 통째로 복사한다.** cloop << 2 가 바이트
			 * 오프셋이고 cloop 가 배열 색인이다.
			 * **같은 노드에 반복해서 쓰므로 다중 함수 카드에서는 마지막 함수의
			 * 설정공간만 남는다** -- cpqhp_save_config 가 함수마다 노드를 따로
			 * 만드는 것과 대비된다 */
			pci_bus_read_config_dword(ctrl->pci_bus, PCI_DEVFN(new_slot->device, function), cloop << 2, (u32 *) &(new_slot->config_space[cloop]));

		/* [한국어] 다음 함수 번호로 넘어간다 */
		function++;

		/* [한국어] 찾았는지 표시할 플래그를 내린다 */
		stop_it = 0;

		/* this loop skips to the next present function
		 * reading in the Class Code and the Header type.
		 */
		while ((function < max_functions) && (!stop_it)) {
			/* [한국어] 다음 함수가 존재하는지 본다 */
			pci_bus_read_config_dword(ctrl->pci_bus, PCI_DEVFN(new_slot->device, function), PCI_VENDOR_ID, &ID);

			/* [한국어] 벤더 ID 가 전부 1 이면 그 함수는 없다 */
			if (ID == 0xFFFFFFFF)
				/* [한국어] 없는 함수면 다음으로 넘어간다 */
				function++;
			else {
				/* [한국어] 클래스 코드를 읽는다 */
				pci_bus_read_config_byte(ctrl->pci_bus, PCI_DEVFN(new_slot->device, function), 0x0B, &class_code);
				/* [한국어] 헤더 타입도 읽는다 */
				pci_bus_read_config_byte(ctrl->pci_bus, PCI_DEVFN(new_slot->device, function), PCI_HEADER_TYPE, &header_type);
				/* [한국어] 함수를 찾았으므로 안쪽 루프를 멈춘다 */
				stop_it++;
			}
		}

	}

	return 0;
}


/*
 * cpqhp_save_base_addr_length
 *
 * Saves the length of all base address registers for the
 * specified slot.  this is for hot plug REPLACE
 *
 * returns 0 if success
 */
/* [한국어]
 * cpqhp_save_base_addr_length - 각 BAR 가 요구하는 크기를 재어 기록한다
 *
 * @ctrl: 대상 컨트롤러.
 * @func: 대상 노드. 이 노드의 장치에 속한 모든 함수를 훑는다.
 * @return: 0 성공, 그 밖에는 오류.
 *
 * **교체 검증의 밑천을 만든다.** 카드를 뽑기 전에 각 BAR 가 얼마를
 * 요구하는지 재어 두었다가, 나중에 cpqhp_valid_replace 가 새로 꽂힌
 * 카드의 BAR 크기와 견줘 같은 카드인지 판정한다.
 *
 * 크기를 재는 방법이 파일 헤더에 적은 그 관용구다 --
 * BAR 에 0xFFFFFFFF 를 쓰고 되읽어, 종류 비트를 지우고 뒤집어 1 을
 * 더한다. 그 값이 카드가 요구하는 크기다.
 *
 *   IO 이면(비트 0 이 1)      → 0xFFFFFFFE 로 마스크, type = 1
 *   메모리이면(비트 0 이 0)   → 0xFFFFFFF0 으로 마스크, type = 0
 *   구현되지 않은 BAR 이면    → 크기 0, type 0
 *
 * **결과를 되돌려 놓지 않는다.** BAR 에 쓰레기 값이 남은 채로 끝나는데,
 * 이 함수는 카드를 뽑기 직전에만 불리므로 문제가 되지 않는다.
 * cpqphp_ctrl.c 의 remove_board 가 pci_dev 를 먼저 제거한 뒤에 부르는
 * 순서가 그것을 보장한다.
 *
 * **브리지와 일반 장치의 BAR 개수가 다르다.**
 *   브리지     : 0x10 부터 0x14 까지 (BAR 두 개)
 *   일반 장치  : 0x10 부터 0x24 까지 (BAR 여섯 개)
 * PCI 규격이 브리지 헤더에는 BAR 를 둘만 두기 때문이다.
 * **위의 원문 주석이 그 두 루프가 중복이라며 합칠 수 있다고 적어 두었다.**
 *
 * 브리지면 하위 버스의 모든 노드에 대해 **재귀 호출** 한다.
 * 전역 cpqhp_slot_list[sub_bus] 를 걸어가며 각각을 처리하고,
 * 돌아온 뒤 pci_bus->number 를 되돌린다.
 *
 * 같은 장치의 모든 함수를 index 를 늘려 가며 순회하는 관용구를 쓴다.
 *
 * **끝에 빈 else 블록이 있다.** 브리지도 일반 장치도 아닌 헤더 타입에
 * 대해 주석만 있고 본문이 없다. 원문 그대로 둔다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   cpqphp_ctrl.c 의 remove_board → [이 함수] → 자기 자신(재귀)
 */
int cpqhp_save_base_addr_length(struct controller *ctrl, struct pci_func *func)
{
	u8 cloop;
	/* [한국어] 헤더 타입을 담을 변수 */
	u8 header_type;
	/* [한국어] 보조 버스 번호를 담을 변수 */
	u8 secondary_bus;
	/* [한국어] BAR 종류(IO 인가 메모리인가) */
	u8 type;
	/* [한국어] 정수로 바꾼 보조 버스 번호 */
	int sub_bus;
	/* [한국어] 질의에 쓸 임시 변수 */
	u32 temp_register;
	/* [한국어] 되읽은 BAR 값 */
	u32 base;
	/* [한국어] 재귀 호출의 반환값 */
	u32 rc;
	/* [한국어] 하위 버스의 노드를 훑을 포인터 */
	struct pci_func *next;
	/* [한국어] 같은 장치의 함수를 순회할 첨자 */
	int index = 0;
	/* [한국어] 설정공간 접근에 쓸 임시 버스 */
	struct pci_bus *pci_bus = ctrl->pci_bus;
	/* [한국어] 장치·함수를 합친 번호 */
	unsigned int devfn;

	/* [한국어] 첫 번째 함수부터 시작한다 */
	func = cpqhp_slot_find(func->bus, func->device, index++);

	/* [한국어] 같은 장치의 모든 함수를 훑는다 */
	while (func != NULL) {
		/* [한국어] 임시 pci_bus 의 번호를 맞춘다 */
		pci_bus->number = func->bus;
		/* [한국어] 장치·함수 번호를 합친다 */
		devfn = PCI_DEVFN(func->device, func->function);

		/* Check for Bridge */
		pci_bus_read_config_byte(pci_bus, devfn, PCI_HEADER_TYPE, &header_type);

		/* [한국어] **브리지인지 본다.** 브리지는 BAR 가 둘, 일반 장치는 여섯이다 */
		if ((header_type & PCI_HEADER_TYPE_MASK) == PCI_HEADER_TYPE_BRIDGE) {
			/* [한국어] 보조 버스 번호를 읽는다 */
			pci_bus_read_config_byte(pci_bus, devfn, PCI_SECONDARY_BUS, &secondary_bus);

			/* [한국어] 정수로 바꿔 배열 색인에 쓴다 */
			sub_bus = (int) secondary_bus;

			/* [한국어] 그 버스의 노드 리스트를 꺼낸다 */
			next = cpqhp_slot_list[sub_bus];

			/* [한국어] 하위 버스의 모든 노드를 훑는다 */
			while (next != NULL) {
				/* [한국어] **하위 버스의 노드마다 재귀 호출한다** */
				rc = cpqhp_save_base_addr_length(ctrl, next);
				/* [한국어] 실패를 확인한다 */
				if (rc)
					/* [한국어] 하나라도 실패하면 그대로 올린다 */
					return rc;

				/* [한국어] 다음 노드로 넘어간다 */
				next = next->next;
			}
			/* [한국어] **재귀에서 돌아왔으므로 버스 번호를 되돌린다** */
			pci_bus->number = func->bus;

			/* FIXME: this loop is duplicated in the non-bridge
			 * case.  The two could be rolled together Figure out
			 * IO and memory base lengths
			 */
			for (cloop = 0x10; cloop <= 0x14; cloop += 4) {
				/* [한국어] 질의에 쓸 값을 준비한다 */
				temp_register = 0xFFFFFFFF;
				/* [한국어] BAR 에 전부 1 을 써 본다 */
				pci_bus_write_config_dword(pci_bus, devfn, cloop, temp_register);
				pci_bus_read_config_dword(pci_bus, devfn, cloop, &base);
				/* If this register is implemented */
				if (base) {
					if (base & 0x01L) {
						/* IO base
						 * set base = amount of IO space
						 * requested
						 */
						base = base & 0xFFFFFFFE;
						/* [한국어] 2의 보수를 취해 요구 크기를 얻는다 */
						base = (~base) + 1;

						/* [한국어] IO 로 표시한다 */
						type = 1;
					} else {
						/* memory base */
						base = base & 0xFFFFFFF0;
						/* [한국어] 2의 보수를 취해 요구 크기를 얻는다 */
						base = (~base) + 1;

						/* [한국어] 메모리로 표시한다 */
						type = 0;
					}
				} else {
					/* [한국어] 구현되지 않은 BAR 는 크기 0 으로 본다 */
					base = 0x0L;
					/* [한국어] 종류도 0 으로 둔다 */
					type = 0;
				}

				/* Save information in slot structure */
				func->base_length[(cloop - 0x10) >> 2] =
				base;
				/* [한국어] 종류도 기록한다 */
				func->base_type[(cloop - 0x10) >> 2] = type;

			/* [한국어] BAR 두 개를 모두 훑었다 */
			}	/* End of base register loop */

		} else if ((header_type & PCI_HEADER_TYPE_MASK) == PCI_HEADER_TYPE_NORMAL) {
			/* Figure out IO and memory base lengths */
			for (cloop = 0x10; cloop <= 0x24; cloop += 4) {
				/* [한국어] 질의에 쓸 값을 준비한다 */
				temp_register = 0xFFFFFFFF;
				/* [한국어] BAR 에 전부 1 을 써 본다 */
				pci_bus_write_config_dword(pci_bus, devfn, cloop, temp_register);
				/* [한국어] 되읽어 크기를 알아낸다 */
				pci_bus_read_config_dword(pci_bus, devfn, cloop, &base);

				/* If this register is implemented */
				if (base) {
					if (base & 0x01L) {
						/* IO base
						 * base = amount of IO space
						 * requested
						 */
						base = base & 0xFFFFFFFE;
						/* [한국어] 2의 보수를 취해 요구 크기를 얻는다 */
						base = (~base) + 1;

						/* [한국어] IO 로 표시한다 */
						type = 1;
					} else {
						/* memory base
						 * base = amount of memory
						 * space requested
						 */
						base = base & 0xFFFFFFF0;
						/* [한국어] 2의 보수를 취해 요구 크기를 얻는다 */
						base = (~base) + 1;

						/* [한국어] 메모리로 표시한다 */
						type = 0;
					}
				} else {
					/* [한국어] 구현되지 않은 BAR 는 크기 0 으로 본다 */
					base = 0x0L;
					/* [한국어] 종류도 0 으로 둔다 */
					type = 0;
				}

				/* Save information in slot structure */
				func->base_length[(cloop - 0x10) >> 2] = base;
				/* [한국어] 종류도 기록한다 */
				func->base_type[(cloop - 0x10) >> 2] = type;

			/* [한국어] BAR 여섯 개를 모두 훑었다 */
			}	/* End of base register loop */

		/* [한국어] **브리지도 일반 장치도 아니면 아무것도 하지 않는다.**
		 * 본문이 비어 있고 주석만 있다. 원문 그대로 둔다 */
		} else {	  /* Some other unknown header type */
		}

		/* find the next device in this slot */
		func = cpqhp_slot_find(func->bus, func->device, index++);
	}

	return(0);
}


/*
 * cpqhp_save_used_resources
 *
 * Stores used resource information for existing boards.  this is
 * for boards that were in the system when this driver was loaded.
 * this function is for hot plug ADD
 *
 * returns 0 if success
 */
/* [한국어]
 * cpqhp_save_used_resources - 카드가 실제로 쓰고 있는 주소 범위를 기록한다
 *
 * @ctrl: 대상 컨트롤러.
 * @func: 대상 노드.
 * @return: 0 성공, 1 이면 알 수 없는 BAR 종류, -ENOMEM 이면 할당 실패.
 *
 * cpqhp_save_base_addr_length 가 "얼마나 필요한가" 를 잰다면
 * **이 함수는 "지금 어디를 쓰고 있는가" 를 읽는다.** 드라이버가 올라오기
 * 전부터 꽂혀 있던 카드의 자원을 회수하려면 그 주소를 알아야 한다.
 *
 * 위의 원문 주석이 그 목적을 밝힌다 -- 핫애드를 위한 것이며,
 * 드라이버가 적재될 때 이미 시스템에 있던 보드를 위한 함수다.
 *
 * **맨 먼저 명령 레지스터를 0 으로 만들어 카드를 끈다.** BAR 에
 * 0xFFFFFFFF 를 써 보는 과정에서 카드가 엉뚱한 주소에 반응하면
 * 안 되기 때문이다. 원래 값은 save_command 에 저장해 두는데,
 * **되돌려 주지는 않는다** -- 어차피 뽑을 카드이기 때문으로 보이나
 * 코드가 그 판단을 적어 두지는 않았다.
 *
 * save_command 는 다른 용도로 쓰인다. **각 BAR 를 기록할지 말지
 * 판단하는 데 쓴다** --
 *   비트 0(IO 활성화)이 꺼져 있으면 IO BAR 를 기록하지 않는다.
 *   비트 1(메모리 활성화)이 꺼져 있으면 메모리 BAR 를 기록하지 않는다.
 * 꺼져 있던 자원은 실제로 쓰이지 않았다는 뜻이므로 회수할 것이 없다.
 *
 * 브리지면 창 레지스터도 함께 읽는다.
 *   버스 번호     : 보조~종속 버스 범위를 bus_head 에 넣는다.
 *   IO 창         : base/limit 을 8비트 왼쪽으로 밀어 실제 주소로 만든다.
 *                   길이 계산이 `(limit - base + 0x10) << 8` 인데,
 *                   브리지 IO 창의 단위가 4KiB 라 그 보정이 들어간다.
 *   메모리 창     : base/limit 을 16비트 왼쪽으로 민다.
 *   prefetchable  : 같은 방식.
 * 그 뒤 브리지의 BAR 두 개도 읽는다.
 *
 * 일반 장치면 BAR 여섯 개를 읽는다. 각 BAR 에 대해
 * **저장해 둔 원래 값(save_base)에서 주소를, 질의 결과에서 크기를**
 * 얻어 노드를 만든다. 두 값을 모두 써야 하므로 질의 전에 원래 값을
 * 먼저 읽어 두는 순서가 중요하다.
 *
 * **세 번째 분기의 주석이 잘못되어 있다.** `(base & 0x0BL) == 0x00`
 * 경우에 "prefetchable memory base" 라고 적혀 있는데, 실제로는
 * 일반 메모리이고 코드도 mem_node 를 만든다. 원문 그대로 두고
 * 사실만 적어 둔다. 같은 오기가 브리지 경로와 일반 경로 양쪽에 있다.
 *
 * **할당 실패 시 이미 만든 노드를 정리하지 않는다.** 곧바로 -ENOMEM 을
 * 돌려주므로, 그때까지 func 의 목록에 매단 노드들은 남는다. 다만
 * 그것들은 나중에 cpqhp_destroy_board_resources 가 해제하므로 새지는
 * 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   cpqphp_ctrl.c 의 remove_board → [이 함수]
 */
int cpqhp_save_used_resources(struct controller *ctrl, struct pci_func *func)
{
	u8 cloop;
	/* [한국어] 헤더 타입을 담을 변수 */
	u8 header_type;
	/* [한국어] 보조 버스 번호를 담을 변수 */
	u8 secondary_bus;
	/* [한국어] 종속 버스 번호를 담을 변수 */
	u8 temp_byte;
	/* [한국어] 8비트 창의 시작 */
	u8 b_base;
	/* [한국어] 8비트 창의 끝 */
	u8 b_length;
	/* [한국어] 레지스터에 쓸 값 */
	u16 command;
	/* [한국어] **원래 명령 레지스터 값.** 카드를 끄기 전에 저장해 두고,
	 * 아래에서 각 BAR 를 기록할지 판단하는 데 쓴다.
	 * **되돌려 주지는 않는다** -- 어차피 뽑을 카드이기 때문으로 보이나
	 * 코드가 그 판단을 적어 두지는 않았다 */
	u16 save_command;
	/* [한국어] 16비트 창의 시작 */
	u16 w_base;
	/* [한국어] 16비트 창의 끝 */
	u16 w_length;
	/* [한국어] 질의에 쓸 임시 변수 */
	u32 temp_register;
	/* [한국어] **BAR 의 원래 값** -- 주소를 얻는 데 쓴다 */
	u32 save_base;
	/* [한국어] 질의로 알아낸 BAR 크기 */
	u32 base;
	/* [한국어] 같은 장치의 함수를 순회할 첨자 */
	int index = 0;
	/* [한국어] 메모리 노드 */
	struct pci_resource *mem_node;
	/* [한국어] prefetchable 메모리 노드 */
	struct pci_resource *p_mem_node;
	/* [한국어] IO 노드 */
	struct pci_resource *io_node;
	/* [한국어] 버스 구간 노드 */
	struct pci_resource *bus_node;
	/* [한국어] 설정공간 접근에 쓸 임시 버스 */
	struct pci_bus *pci_bus = ctrl->pci_bus;
	/* [한국어] 장치·함수를 합친 번호 */
	unsigned int devfn;

	/* [한국어] 첫 번째 함수부터 시작한다 */
	func = cpqhp_slot_find(func->bus, func->device, index++);

	/* [한국어] **보드인 함수만 훑는다** -- 빈 슬롯 자리표는 건너뛴다 */
	while ((func != NULL) && func->is_a_board) {
		/* [한국어] 임시 pci_bus 의 번호를 맞춘다 */
		pci_bus->number = func->bus;
		/* [한국어] 장치·함수 번호를 합친다 */
		devfn = PCI_DEVFN(func->device, func->function);

		/* Save the command register */
		pci_bus_read_config_word(pci_bus, devfn, PCI_COMMAND, &save_command);

		/* disable card */
		command = 0x00;
		/* [한국어] **카드를 끈다.** BAR 에 전부 1 을 써 보는 동안 카드가 엉뚱한
		 * 주소에 반응하면 안 되기 때문이다 */
		pci_bus_write_config_word(pci_bus, devfn, PCI_COMMAND, command);

		/* Check for Bridge */
		pci_bus_read_config_byte(pci_bus, devfn, PCI_HEADER_TYPE, &header_type);

		if ((header_type & PCI_HEADER_TYPE_MASK) == PCI_HEADER_TYPE_BRIDGE) {
			/* Clear Bridge Control Register */
			command = 0x00;
			/* [한국어] **브리지 제어 레지스터도 0 으로 만든다** -- 브리지가 트랜잭션을
			 * 전달하지 않게 한다 */
			pci_bus_write_config_word(pci_bus, devfn, PCI_BRIDGE_CONTROL, command);
			/* [한국어] 보조 버스 번호를 읽는다 */
			pci_bus_read_config_byte(pci_bus, devfn, PCI_SECONDARY_BUS, &secondary_bus);
			/* [한국어] 종속 버스 번호를 읽는다 */
			pci_bus_read_config_byte(pci_bus, devfn, PCI_SUBORDINATE_BUS, &temp_byte);

			/* [한국어] 버스 구간 노드를 만든다 */
			bus_node = kmalloc_obj(*bus_node);
			/* [한국어] 할당 실패면 포기한다 */
			if (!bus_node)
				return -ENOMEM;

			/* [한국어] 보조 버스 번호가 시작이다 */
			bus_node->base = secondary_bus;
			/* [한국어] **버스 개수를 계산한다** -- 종속에서 보조를 빼고 1 을 더한다 */
			bus_node->length = temp_byte - secondary_bus + 1;

			/* [한국어] 목록에 연결한다 */
			bus_node->next = func->bus_head;
			/* [한국어] 이 카드의 버스 목록에 매단다 */
			func->bus_head = bus_node;

			/* Save IO base and Limit registers */
			pci_bus_read_config_byte(pci_bus, devfn, PCI_IO_BASE, &b_base);
			/* [한국어] IO 창의 끝을 읽는다 */
			pci_bus_read_config_byte(pci_bus, devfn, PCI_IO_LIMIT, &b_length);

			/* [한국어] **창이 유효하고 IO 가 활성화되어 있어야 기록한다** */
			if ((b_base <= b_length) && (save_command & 0x01)) {
				/* [한국어] IO 창 노드를 만든다 */
				io_node = kmalloc_obj(*io_node);
				/* [한국어] 할당 실패면 포기한다 */
				if (!io_node)
					return -ENOMEM;

				/* [한국어] **하위 니블을 지우고 8비트 왼쪽으로 민다** -- IO base 레지스터는
				 * 상위 니블만 주소이고 하위 니블은 창 종류 표시다 */
				io_node->base = (b_base & 0xF0) << 8;
				/* [한국어] **0x10 보정을 더하고 8비트 왼쪽으로 민다** -- 브리지 IO 창이
				 * 4KiB 단위이기 때문이다 */
				io_node->length = (b_length - b_base + 0x10) << 8;

				/* [한국어] 목록에 연결한다 */
				io_node->next = func->io_head;
				/* [한국어] 이 카드의 IO 목록에 매단다 */
				func->io_head = io_node;
			}

			/* Save memory base and Limit registers */
			pci_bus_read_config_word(pci_bus, devfn, PCI_MEMORY_BASE, &w_base);
			/* [한국어] 메모리 창의 끝을 읽는다 */
			pci_bus_read_config_word(pci_bus, devfn, PCI_MEMORY_LIMIT, &w_length);

			/* [한국어] **창이 유효하고 메모리가 활성화되어 있어야 기록한다** */
			if ((w_base <= w_length) && (save_command & 0x02)) {
				/* [한국어] 메모리 창 노드를 만든다 */
				mem_node = kmalloc_obj(*mem_node);
				/* [한국어] 할당 실패면 포기한다 */
				if (!mem_node)
					return -ENOMEM;

				/* [한국어] 16비트 왼쪽으로 밀어 실제 주소로 만든다 */
				mem_node->base = w_base << 16;
				/* [한국어] **0x10 보정을 더한다** -- 브리지 메모리 창이 1MiB 단위라
				 * limit 에서 base 를 뺀 값에 한 단위를 더해야 실제 길이가 된다 */
				mem_node->length = (w_length - w_base + 0x10) << 16;

				/* [한국어] 목록에 연결한다 */
				mem_node->next = func->mem_head;
				/* [한국어] 이 카드의 메모리 목록에 매단다 */
				func->mem_head = mem_node;
			}

			/* Save prefetchable memory base and Limit registers */
			pci_bus_read_config_word(pci_bus, devfn, PCI_PREF_MEMORY_BASE, &w_base);
			/* [한국어] prefetchable 창의 끝을 읽는다 */
			pci_bus_read_config_word(pci_bus, devfn, PCI_PREF_MEMORY_LIMIT, &w_length);

			/* [한국어] **창이 유효하고 메모리가 활성화되어 있어야 기록한다** */
			if ((w_base <= w_length) && (save_command & 0x02)) {
				/* [한국어] prefetchable 메모리 창 노드를 만든다 */
				p_mem_node = kmalloc_obj(*p_mem_node);
				/* [한국어] 할당 실패면 포기한다 */
				if (!p_mem_node)
					return -ENOMEM;

				/* [한국어] 16비트 왼쪽으로 밀어 실제 주소로 만든다 */
				p_mem_node->base = w_base << 16;
				/* [한국어] **길이 계산에 0x10 보정이 들어간다** -- 브리지의 prefetchable 창이
				 * 1MiB 단위라 limit 에서 base 를 뺀 값에 한 단위를 더해야 한다 */
				p_mem_node->length = (w_length - w_base + 0x10) << 16;

				/* [한국어] 목록에 연결한다 */
				p_mem_node->next = func->p_mem_head;
				/* [한국어] 이 카드의 prefetchable 메모리 목록에 매단다 */
				func->p_mem_head = p_mem_node;
			}
			/* Figure out IO and memory base lengths */
			for (cloop = 0x10; cloop <= 0x14; cloop += 4) {
				/* [한국어] **원래 값을 먼저 읽어 둔다** */
				pci_bus_read_config_dword(pci_bus, devfn, cloop, &save_base);

				/* [한국어] 질의에 쓸 값을 준비한다 */
				temp_register = 0xFFFFFFFF;
				/* [한국어] BAR 에 전부 1 을 써 본다 */
				pci_bus_write_config_dword(pci_bus, devfn, cloop, temp_register);
				/* [한국어] 되읽어 크기를 알아낸다 */
				pci_bus_read_config_dword(pci_bus, devfn, cloop, &base);

				/* [한국어] 되읽은 값을 기억해 둔다 */
				temp_register = base;

				/* If this register is implemented */
				if (base) {
					/* [한국어] IO 이고 IO 가 활성화되어 있는지 본다 */
					if (((base & 0x03L) == 0x01)
					    && (save_command & 0x01)) {
						/* IO base
						 * set temp_register = amount
						 * of IO space requested
						 */
						temp_register = base & 0xFFFFFFFE;
						/* [한국어] 2의 보수를 취해 요구 크기를 얻는다 */
						temp_register = (~temp_register) + 1;

						/* [한국어] IO 노드를 만든다 */
						io_node = kmalloc_obj(*io_node);
						/* [한국어] 할당 실패면 포기한다 */
						if (!io_node)
							return -ENOMEM;

						/* [한국어] **하위 2비트를 지운다** -- 아래 일반 장치 경로가 ~0x01 을 쓰는 것과
						 * 마스크가 다르다 */
						io_node->base =
						save_base & (~0x03L);
						/* [한국어] 요구 크기를 넣는다 */
						io_node->length = temp_register;

						/* [한국어] 목록에 연결한다 */
						io_node->next = func->io_head;
						/* [한국어] 이 카드의 IO 목록에 매단다 */
						func->io_head = io_node;
					/* [한국어] 다음 종류를 본다 */
					} else
						/* [한국어] prefetchable 메모리인지 본다 */
						if (((base & 0x0BL) == 0x08)
						    && (save_command & 0x02)) {
						/* prefetchable memory base */
						temp_register = base & 0xFFFFFFF0;
						/* [한국어] 2의 보수를 취해 요구 크기를 얻는다 */
						temp_register = (~temp_register) + 1;

						/* [한국어] prefetchable 메모리 노드를 만든다 */
						p_mem_node = kmalloc_obj(*p_mem_node);
						/* [한국어] 할당 실패면 포기한다 */
						if (!p_mem_node)
							return -ENOMEM;

						/* [한국어] 저장해 둔 원래 주소에서 하위 4비트를 지운다 */
						p_mem_node->base = save_base & (~0x0FL);
						/* [한국어] 요구 크기를 넣는다 */
						p_mem_node->length = temp_register;

						/* [한국어] 목록에 연결한다 */
						p_mem_node->next = func->p_mem_head;
						/* [한국어] 이 카드의 prefetchable 메모리 목록에 매단다 */
						func->p_mem_head = p_mem_node;
					/* [한국어] 다음 종류를 본다 */
					} else
						/* [한국어] **일반 메모리인지 본다.** 여기도 원문 주석이 prefetchable 이라
						 * 적혀 있으나 실제로는 일반 메모리다 */
						if (((base & 0x0BL) == 0x00)
						    && (save_command & 0x02)) {
						/* prefetchable memory base */
						temp_register = base & 0xFFFFFFF0;
						/* [한국어] 2의 보수를 취해 요구 크기를 얻는다 */
						temp_register = (~temp_register) + 1;

						/* [한국어] 메모리 노드를 만든다 */
						mem_node = kmalloc_obj(*mem_node);
						/* [한국어] 할당 실패면 포기한다 */
						if (!mem_node)
							return -ENOMEM;

						/* [한국어] 저장해 둔 원래 주소에서 하위 4비트를 지운다 */
						mem_node->base = save_base & (~0x0FL);
						/* [한국어] 요구 크기를 넣는다 */
						mem_node->length = temp_register;

						/* [한국어] 목록에 연결한다 */
						mem_node->next = func->mem_head;
						/* [한국어] 이 카드의 메모리 목록에 매단다 */
						func->mem_head = mem_node;
					/* [한국어] 세 종류 어디에도 맞지 않으면 1 을 돌려준다 */
					} else
						return(1);
				}
			}	/* End of base register loop */
		/* Standard header */
		} else if ((header_type & PCI_HEADER_TYPE_MASK) == PCI_HEADER_TYPE_NORMAL) {
			/* Figure out IO and memory base lengths */
			for (cloop = 0x10; cloop <= 0x24; cloop += 4) {
				/* [한국어] **원래 값을 먼저 읽어 둔다.** 주소는 여기서, 크기는 아래 질의에서
				 * 얻으므로 이 순서가 중요하다 */
				pci_bus_read_config_dword(pci_bus, devfn, cloop, &save_base);

				/* [한국어] 질의에 쓸 값을 준비한다 */
				temp_register = 0xFFFFFFFF;
				/* [한국어] BAR 에 전부 1 을 써 본다 */
				pci_bus_write_config_dword(pci_bus, devfn, cloop, temp_register);
				/* [한국어] 되읽어 크기를 알아낸다 */
				pci_bus_read_config_dword(pci_bus, devfn, cloop, &base);

				/* [한국어] 되읽은 값을 기억해 둔다 */
				temp_register = base;

				/* If this register is implemented */
				if (base) {
					/* [한국어] **IO 이고 IO 가 활성화되어 있는지 본다.** save_command 의 비트 0 이
					 * 꺼져 있으면 그 자원은 실제로 쓰이지 않았다는 뜻이라 기록하지 않는다 */
					if (((base & 0x03L) == 0x01)
					    && (save_command & 0x01)) {
						/* IO base
						 * set temp_register = amount
						 * of IO space requested
						 */
						temp_register = base & 0xFFFFFFFE;
						/* [한국어] 2의 보수를 취해 요구 크기를 얻는다 */
						temp_register = (~temp_register) + 1;

						/* [한국어] IO 노드를 만든다 */
						io_node = kmalloc_obj(*io_node);
						/* [한국어] 할당 실패면 포기한다 */
						if (!io_node)
							return -ENOMEM;

						/* [한국어] **저장해 둔 원래 주소에서 최하위 비트만 지운다** --
						 * IO BAR 는 비트 0 만 종류 표시라, 브리지 경로가 ~0x03 을 쓰는 것과
						 * 마스크가 다르다 */
						io_node->base = save_base & (~0x01L);
						/* [한국어] 요구 크기를 넣는다 */
						io_node->length = temp_register;

						/* [한국어] 목록에 연결한다 */
						io_node->next = func->io_head;
						/* [한국어] 이 카드의 IO 목록에 매단다 */
						func->io_head = io_node;
					/* [한국어] 다음 종류를 본다 */
					} else
						/* [한국어] prefetchable 메모리인지 본다 */
						if (((base & 0x0BL) == 0x08)
						    && (save_command & 0x02)) {
						/* prefetchable memory base */
						temp_register = base & 0xFFFFFFF0;
						/* [한국어] 2의 보수를 취해 요구 크기를 얻는다 */
						temp_register = (~temp_register) + 1;

						/* [한국어] prefetchable 메모리 노드를 만든다 */
						p_mem_node = kmalloc_obj(*p_mem_node);
						/* [한국어] 할당 실패면 포기한다 */
						if (!p_mem_node)
							return -ENOMEM;

						/* [한국어] 저장해 둔 원래 주소에서 하위 4비트를 지운다 */
						p_mem_node->base = save_base & (~0x0FL);
						/* [한국어] 요구 크기를 넣는다 */
						p_mem_node->length = temp_register;

						/* [한국어] 목록에 연결한다 */
						p_mem_node->next = func->p_mem_head;
						/* [한국어] 이 카드의 prefetchable 메모리 목록에 매단다 */
						func->p_mem_head = p_mem_node;
					/* [한국어] 다음 종류를 본다 */
					} else
						/* [한국어] **일반 메모리인지 본다.** 위의 원문 주석이 여기도 prefetchable 이라
						 * 적혀 있으나 실제로는 일반 메모리이고 코드도 mem_node 를 만든다.
						 * 원문 그대로 두고 사실만 적어 둔다 */
						if (((base & 0x0BL) == 0x00)
						    && (save_command & 0x02)) {
						/* prefetchable memory base */
						temp_register = base & 0xFFFFFFF0;
						/* [한국어] 2의 보수를 취해 요구 크기를 얻는다 */
						temp_register = (~temp_register) + 1;

						/* [한국어] 메모리 노드를 만든다 */
						mem_node = kmalloc_obj(*mem_node);
						/* [한국어] 할당 실패면 포기한다 */
						if (!mem_node)
							return -ENOMEM;

						/* [한국어] **저장해 둔 원래 주소에서 하위 4비트를 지운다** */
						mem_node->base = save_base & (~0x0FL);
						/* [한국어] 요구 크기를 넣는다 */
						mem_node->length = temp_register;

						/* [한국어] 목록에 연결한다 */
						mem_node->next = func->mem_head;
						/* [한국어] 이 카드의 메모리 목록에 매단다 */
						func->mem_head = mem_node;
					/* [한국어] **세 종류 어디에도 맞지 않으면 1 을 돌려준다** */
					} else
						return(1);
				}
			/* [한국어] BAR 여섯 개를 모두 훑었다 */
			}	/* End of base register loop */
		}

		/* find the next device in this slot */
		func = cpqhp_slot_find(func->bus, func->device, index++);
	}

	return 0;
}


/*
 * cpqhp_configure_board
 *
 * Copies saved configuration information to one slot.
 * this is called recursively for bridge devices.
 * this is for hot plug REPLACE!
 *
 * returns 0 if success
 */
/* [한국어]
 * cpqhp_configure_board - 저장해 둔 설정공간 사본을 되쓴다
 *
 * @ctrl: 대상 컨트롤러.
 * @func: 대상 노드.
 * @return: 0 성공, 1 이면 BAR 가 사본과 달라 실패.
 *
 * **교체 경로의 핵심이다.** 같은 카드가 다시 꽂혔다고 판정되면
 * (cpqhp_valid_replace 가 0 을 돌려주면) 이 함수가 뽑기 전의 설정을
 * 그대로 되살린다. 새로 자원을 나눠 줄 필요가 없다.
 *
 * **되쓰는 순서가 거꾸로다** -- 오프셋 0x3C 에서 0x04 까지 4씩 줄여
 * 가며 쓴다. 위의 원문 주석이 이유를 밝힌다: 제어 레지스터를 마지막에
 * 쓰기 위해서다. 명령 레지스터(0x04)에 IO/메모리 활성화 비트를 켜는
 * 순간 카드가 버스에 응답하기 시작하므로, BAR 가 먼저 제자리에
 * 있어야 한다.
 *
 * **오프셋 0x00 은 쓰지 않는다** -- 루프 조건이 `cloop > 0` 이라
 * 0x04 에서 멈춘다. 0x00 은 벤더/장치 ID 로 읽기 전용이므로 맞다.
 *
 * 브리지면 하위 버스의 모든 노드에 대해 **재귀 호출** 한다.
 *
 * 브리지가 아니면 **되쓴 결과를 검증한다.** 오프셋 16 부터 40 까지
 * (BAR 여섯 개와 그 뒤)를 되읽어 사본과 견주고, 하나라도 다르면
 * 1 을 돌려준다. 위의 원문 주석이 그 뜻을 밝힌다 -- 다르면 다른
 * 보드라는 것이다.
 *
 * **검증 범위가 BAR 를 넘어선다.** 16~39 는 BAR 여섯 개(0x10~0x24)에
 * 더해 카드버스 CIS 포인터(0x28)와 서브시스템 ID(0x2C)까지 포함한다.
 * 그 둘은 대개 읽기 전용이라 되쓴 값이 반영되지 않을 수 있는데,
 * 사본도 원래 하드웨어에서 읽은 값이므로 일치한다.
 *
 * 검증 실패 시 dbg 로 어느 오프셋이 어떻게 다른지 세 줄로 찍는다.
 * 그 로그는 전역 cpqhp_debug 가 참일 때만 나온다.
 *
 * 성공하면 func->configured 를 1 로 표시한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(커널 스레드).
 *
 * 호출 체인:
 *   cpqphp_ctrl.c 의 board_replaced → [이 함수] → 자기 자신(재귀)
 */
int cpqhp_configure_board(struct controller *ctrl, struct pci_func *func)
{
	int cloop;
	/* [한국어] 헤더 타입을 담을 변수 */
	u8 header_type;
	/* [한국어] 보조 버스 번호를 담을 변수 */
	u8 secondary_bus;
	/* [한국어] 정수로 바꾼 보조 버스 번호 */
	int sub_bus;
	/* [한국어] 하위 버스의 노드를 훑을 포인터 */
	struct pci_func *next;
	/* [한국어] 되읽은 값을 담을 변수 */
	u32 temp;
	/* [한국어] 재귀 호출의 반환값 */
	u32 rc;
	/* [한국어] 같은 장치의 함수를 순회할 첨자 */
	int index = 0;
	/* [한국어] 설정공간 접근에 쓸 임시 버스 */
	struct pci_bus *pci_bus = ctrl->pci_bus;
	/* [한국어] 장치·함수를 합친 번호 */
	unsigned int devfn;

	/* [한국어] 첫 번째 함수부터 시작한다 */
	func = cpqhp_slot_find(func->bus, func->device, index++);

	/* [한국어] 같은 장치의 모든 함수를 훑는다 */
	while (func != NULL) {
		/* [한국어] 임시 pci_bus 의 번호를 맞춘다 */
		pci_bus->number = func->bus;
		/* [한국어] 장치·함수 번호를 합친다 */
		devfn = PCI_DEVFN(func->device, func->function);

		/* Start at the top of config space so that the control
		 * registers are programmed last
		 */
		for (cloop = 0x3C; cloop > 0; cloop -= 4)
			/* [한국어] **사본의 dword 를 그대로 되쓴다.** cloop >> 2 가 배열 색인이다 */
			pci_bus_write_config_dword(pci_bus, devfn, cloop, func->config_space[cloop >> 2]);

		/* [한국어] 헤더 타입을 읽어 브리지인지 본다 */
		pci_bus_read_config_byte(pci_bus, devfn, PCI_HEADER_TYPE, &header_type);

		/* If this is a bridge device, restore subordinate devices */
		if ((header_type & PCI_HEADER_TYPE_MASK) == PCI_HEADER_TYPE_BRIDGE) {
			/* [한국어] 보조 버스 번호를 읽는다 */
			pci_bus_read_config_byte(pci_bus, devfn, PCI_SECONDARY_BUS, &secondary_bus);

			/* [한국어] 정수로 바꿔 배열 색인에 쓴다 */
			sub_bus = (int) secondary_bus;

			/* [한국어] 그 버스의 노드 리스트를 꺼낸다 */
			next = cpqhp_slot_list[sub_bus];

			/* [한국어] 하위 버스의 모든 노드를 훑는다 */
			while (next != NULL) {
				/* [한국어] **하위 버스의 노드마다 재귀 호출한다** */
				rc = cpqhp_configure_board(ctrl, next);
				/* [한국어] 실패를 확인한다 */
				if (rc)
					/* [한국어] 하나라도 실패하면 그대로 올린다 */
					return rc;

				/* [한국어] 다음 노드로 넘어간다 */
				next = next->next;
			}
		} else {

			/* Check all the base Address Registers to make sure
			 * they are the same.  If not, the board is different.
			 */

			/* [한국어] **오프셋 16~39 를 검증한다.** BAR 여섯 개(0x10~0x24)에 더해
			 * 카드버스 CIS 포인터(0x28)와 서브시스템 ID(0x2C)까지 포함한다.
			 * 그 둘은 대개 읽기 전용이라 되쓴 값이 반영되지 않지만, 사본도
			 * 원래 하드웨어에서 읽은 값이므로 일치한다 */
			for (cloop = 16; cloop < 40; cloop += 4) {
				/* [한국어] 되쓴 값을 되읽는다 */
				pci_bus_read_config_dword(pci_bus, devfn, cloop, &temp);

				/* [한국어] 사본과 견준다 */
				if (temp != func->config_space[cloop >> 2]) {
					/* [한국어] 어느 오프셋이 다른지 찍는다 */
					dbg("Config space compare failure!!! offset = %x\n", cloop);
					/* [한국어] 어느 함수인지 찍는다 */
					dbg("bus = %x, device = %x, function = %x\n", func->bus, func->device, func->function);
					/* [한국어] 읽은 값과 사본을 함께 찍는다 */
					dbg("temp = %x, config space = %x\n\n", temp, func->config_space[cloop >> 2]);
					/* [한국어] **하나라도 다르면 다른 카드다** -- 위의 원문 주석이 그 뜻을 밝힌다 */
					return 1;
				}
			}
		}

		/* [한국어] 설정을 마쳤음을 표시한다 */
		func->configured = 1;

		/* [한국어] 같은 장치의 다음 함수로 넘어간다 */
		func = cpqhp_slot_find(func->bus, func->device, index++);
	}

	return 0;
}


/*
 * cpqhp_valid_replace
 *
 * this function checks to see if a board is the same as the
 * one it is replacing.  this check will detect if the device's
 * vendor or device id's are the same
 *
 * returns 0 if the board is the same nonzero otherwise
 */
/* [한국어]
 * cpqhp_valid_replace - 새로 꽂힌 카드가 뽑기 전과 같은 카드인지 검사한다
 *
 * @ctrl: 대상 컨트롤러.
 * @func: 대상 노드.
 * @return: 0 이면 같은 카드, 그 밖에는 다른 이유의 오류 코드.
 *
 * **교체 경로의 문지기다.** 이 함수가 0 을 돌려줘야
 * cpqhp_configure_board 가 사본을 되쓴다. 다른 카드에 남의 설정을
 * 씌우면 안 되기 때문이다.
 *
 * 같은 장치의 모든 함수를 훑으며 차례로 검사한다.
 *   1) 노드가 보드가 아니면 ADD_NOT_SUPPORTED -- 애초에 교체가 아니다.
 *   2) 벤더/장치 ID 가 전부 F 면 NO_ADAPTER_PRESENT.
 *   3) 벤더/장치 ID 가 사본과 다르면 ADAPTER_NOT_SAME.
 *   4) 클래스 코드와 리비전이 다르면 ADAPTER_NOT_SAME.
 *   5) 브리지면 **버스 번호 레지스터를 먼저 되쓴 뒤** 하위 버스의
 *      노드들에 재귀한다. 위의 원문 주석이 그 이유를 밝힌다 --
 *      하위 버스에 접근하려면 브리지가 그 번호에 응답하도록
 *      먼저 프로그래밍해야 한다.
 *   6) 일반 장치면 서브시스템 벤더/장치 ID 를 견준다.
 *      **여기에 예외가 하나 있다** -- 위의 원문 주석대로,
 *      SMART-2 컨트롤러(벤더/장치 ID 가 0xAE100E11)이면서 그 값이
 *      0 이면 무시한다. 펌웨어가 오래된 것뿐이라는 것이다.
 *   7) BAR 여섯 개의 크기와 종류를 견준다. 하나라도 다르면
 *      ADAPTER_NOT_SAME.
 *   8) 브리지도 일반 장치도 아니면 DEVICE_TYPE_NOT_SUPPORTED.
 *
 * **7)이 BAR 를 파괴한다.** 크기를 재려고 0xFFFFFFFF 를 써 넣고
 * 되돌려 놓지 않는다. 그래서 이 함수가 성공하면 곧바로
 * cpqhp_configure_board 가 사본을 되써서 BAR 를 복구해야 하며,
 * 실제로 cpqphp_ctrl.c 의 board_replaced 가 그 순서로 부른다.
 * **둘의 순서가 뒤바뀌면 카드가 망가진 상태로 남는다.**
 *
 * 5)에서 `pci_bus_write_config_dword(pci_bus, devfn, PCI_PRIMARY_BUS, ...)`
 * 로 dword 를 통째로 쓰는 것에 주의 -- 기본/보조/종속 버스 번호와
 * 보조 지연 타이머가 한 dword 에 들어 있어 네 값을 한 번에 되쓴다.
 *
 * 반환값이 오류 코드 상수(cpqphp.h:366~375)이며 0 이 아닌 값은 전부
 * "교체 불가" 를 뜻한다. 호출자는 그것을 board_replaced 의 rc 로
 * 그대로 올린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(커널 스레드).
 *
 * 호출 체인:
 *   cpqphp_ctrl.c 의 board_replaced → [이 함수] → 자기 자신(재귀)
 */
int cpqhp_valid_replace(struct controller *ctrl, struct pci_func *func)
{
	u8 cloop;
	/* [한국어] 헤더 타입을 담을 변수 */
	u8 header_type;
	/* [한국어] 보조 버스 번호를 담을 변수 */
	u8 secondary_bus;
	/* [한국어] BAR 종류(IO 인가 메모리인가) */
	u8 type;
	/* [한국어] 설정공간 값을 담을 변수 */
	u32 temp_register = 0;
	/* [한국어] BAR 크기를 담을 변수 */
	u32 base;
	/* [한국어] 재귀 호출의 반환값 */
	u32 rc;
	/* [한국어] 하위 버스의 노드를 훑을 포인터 */
	struct pci_func *next;
	/* [한국어] 같은 장치의 함수를 순회할 첨자 */
	int index = 0;
	/* [한국어] 설정공간 접근에 쓸 임시 버스 */
	struct pci_bus *pci_bus = ctrl->pci_bus;
	/* [한국어] 장치·함수를 합친 번호 */
	unsigned int devfn;

	/* [한국어] **보드가 아니면 애초에 교체가 아니다** */
	if (!func->is_a_board)
		return(ADD_NOT_SUPPORTED);

	/* [한국어] 첫 번째 함수부터 검사한다 */
	func = cpqhp_slot_find(func->bus, func->device, index++);

	/* [한국어] 같은 장치의 모든 함수를 훑는다 */
	while (func != NULL) {
		/* [한국어] 임시 pci_bus 의 번호를 맞춘다 */
		pci_bus->number = func->bus;
		/* [한국어] 장치·함수 번호를 합친다 */
		devfn = PCI_DEVFN(func->device, func->function);

		/* [한국어] 벤더/장치 ID 를 읽는다 */
		pci_bus_read_config_dword(pci_bus, devfn, PCI_VENDOR_ID, &temp_register);

		/* No adapter present */
		if (temp_register == 0xFFFFFFFF)
			return(NO_ADAPTER_PRESENT);

		/* [한국어] **벤더/장치 ID 가 사본과 다르면 다른 카드다** */
		if (temp_register != func->config_space[0])
			return(ADAPTER_NOT_SAME);

		/* Check for same revision number and class code */
		pci_bus_read_config_dword(pci_bus, devfn, PCI_CLASS_REVISION, &temp_register);

		/* Adapter not the same */
		if (temp_register != func->config_space[0x08 >> 2])
			return(ADAPTER_NOT_SAME);

		/* Check for Bridge */
		pci_bus_read_config_byte(pci_bus, devfn, PCI_HEADER_TYPE, &header_type);

		if ((header_type & PCI_HEADER_TYPE_MASK) == PCI_HEADER_TYPE_BRIDGE) {
			/* In order to continue checking, we must program the
			 * bus registers in the bridge to respond to accesses
			 * for its subordinate bus(es)
			 */

			/* [한국어] **사본에서 버스 번호 dword 를 꺼낸다** -- 기본·보조·종속 버스 번호와
			 * 보조 지연 타이머가 한 dword 에 들어 있어 통째로 되쓴다 */
			temp_register = func->config_space[0x18 >> 2];
			/* [한국어] **버스 번호를 되쓴다.** 위의 원문 주석대로, 하위 버스에 접근하려면
			 * 브리지가 그 번호에 응답하도록 먼저 프로그래밍해야 한다 */
			pci_bus_write_config_dword(pci_bus, devfn, PCI_PRIMARY_BUS, temp_register);

			/* [한국어] **보조 버스 번호를 뽑는다** -- 사본의 비트 15:8 이다 */
			secondary_bus = (temp_register >> 8) & 0xFF;

			/* [한국어] 그 버스의 노드 리스트를 꺼낸다 */
			next = cpqhp_slot_list[secondary_bus];

			/* [한국어] 하위 버스의 모든 노드를 훑는다 */
			while (next != NULL) {
				/* [한국어] **하위 버스의 노드마다 재귀 검사한다** */
				rc = cpqhp_valid_replace(ctrl, next);
				/* [한국어] 검사 실패를 확인한다 */
				if (rc)
					/* [한국어] 하나라도 다르면 그대로 올린다 */
					return rc;

				/* [한국어] 다음 노드로 넘어간다 */
				next = next->next;
			}

		}
		/* Check to see if it is a standard config header */
		else if ((header_type & PCI_HEADER_TYPE_MASK) == PCI_HEADER_TYPE_NORMAL) {
			/* Check subsystem vendor and ID */
			pci_bus_read_config_dword(pci_bus, devfn, PCI_SUBSYSTEM_VENDOR_ID, &temp_register);

			if (temp_register != func->config_space[0x2C >> 2]) {
				/* If it's a SMART-2 and the register isn't
				 * filled in, ignore the difference because
				 * they just have an old rev of the firmware
				 */
				if (!((func->config_space[0] == 0xAE100E11)
				      /* [한국어] **서브시스템 ID 가 0 이기도 하면 무시한다.** 위의 원문 주석대로
				       * SMART-2 의 오래된 펌웨어가 그 자리를 채우지 않기 때문이다 */
				      && (temp_register == 0x00L)))
					return(ADAPTER_NOT_SAME);
			}
			/* Figure out IO and memory base lengths */
			for (cloop = 0x10; cloop <= 0x24; cloop += 4) {
				/* [한국어] 질의에 쓸 값을 준비한다 */
				temp_register = 0xFFFFFFFF;
				/* [한국어] **BAR 에 전부 1 을 써 본다.** 이 함수가 성공하면 곧바로
				 * cpqhp_configure_board 가 사본을 되써서 BAR 를 복구해야 한다 --
				 * **둘의 순서가 뒤바뀌면 카드가 망가진 상태로 남는다** */
				pci_bus_write_config_dword(pci_bus, devfn, cloop, temp_register);
				/* [한국어] 되읽어 크기를 알아낸다 */
				pci_bus_read_config_dword(pci_bus, devfn, cloop, &base);

				/* If this register is implemented */
				if (base) {
					if (base & 0x01L) {
						/* IO base
						 * set base = amount of IO
						 * space requested
						 */
						base = base & 0xFFFFFFFE;
						/* [한국어] 2의 보수를 취해 요구 크기를 얻는다 */
						base = (~base) + 1;

						/* [한국어] IO 로 표시한다 */
						type = 1;
					} else {
						/* memory base */
						base = base & 0xFFFFFFF0;
						/* [한국어] 2의 보수를 취해 요구 크기를 얻는다 */
						base = (~base) + 1;

						/* [한국어] 메모리로 표시한다 */
						type = 0;
					}
				} else {
					/* [한국어] 구현되지 않은 BAR 는 크기 0 으로 본다 */
					base = 0x0L;
					type = 0;
				}

				/* Check information in slot structure */
				if (func->base_length[(cloop - 0x10) >> 2] != base)
					return(ADAPTER_NOT_SAME);

				/* [한국어] **BAR 종류도 사본과 견준다** -- 크기가 같아도 IO 와 메모리가
				 * 바뀌었으면 다른 카드다 */
				if (func->base_type[(cloop - 0x10) >> 2] != type)
					return(ADAPTER_NOT_SAME);

			/* [한국어] BAR 여섯 개를 모두 훑었다 */
			}	/* End of base register loop */

		/* [한국어] 일반 장치 처리가 끝났다 */
		}		/* End of (type 0 config space) else */
		else {
			/* this is not a type 0 or 1 config space header so
			 * we don't know how to do it
			 */
			return(DEVICE_TYPE_NOT_SUPPORTED);
		}

		/* Get the next function */
		func = cpqhp_slot_find(func->bus, func->device, index++);
	}


	return 0;
}


/*
 * cpqhp_find_available_resources
 *
 * Finds available memory, IO, and IRQ resources for programming
 * devices which may be added to the system
 * this function is for hot plug ADD!
 *
 * returns 0 if success
 */
/* [한국어]
 * cpqhp_find_available_resources - ROM 의 HRT 를 읽어 자유 목록을 만든다
 *
 * @ctrl:      대상 컨트롤러.
 * @rom_start: 시스템 ROM 의 시작 주소.
 * @return: 0 성공, -ENODEV 면 표가 없음, 1 이면 쓸 자원이 없음.
 *
 * **이 드라이버 자원 할당의 출발점이다.** 여기서 만든 자유 목록을
 * cpqphp_ctrl.c 의 할당기가 나눠 쓴다. 이 함수가 실패하면
 * 핫애드 기능 자체가 꺼진다.
 *
 *   1) ROM 에서 "$HRT" 표를 찾는다. 없으면 -ENODEV.
 *   2) **미사용 IRQ 비트맵에서 IRQ 둘을 고른다.** 최하위부터 1 인
 *      비트를 찾아 첫 번째를 cpqhp_disk_irq, 그다음을 cpqhp_nic_irq 로
 *      삼는다. 두 전역은 configure_new_function 이 카드 종류에 따라
 *      배정할 때 쓴다 -- **저장장치와 그 밖(주로 네트워크)에 서로 다른
 *      IRQ 를 주어 인터럽트를 분산하려는 것이다.**
 *      못 찾으면 컨트롤러 자신의 IRQ(ctrl->cfgspc_irq)를 쓴다.
 *   3) NVRAM 상태를 읽는다(cpqphp_nvram.c).
 *   4) **표의 항목을 하나씩 훑는다.** 항목마다 장치/함수 번호,
 *      버스 번호 셋, IO·메모리·prefetchable 메모리의 시작과 길이가 있다.
 *      우리 컨트롤러의 버스가 아니면 건너뛴다.
 *      그 슬롯에 카드가 꽂혀 있는지 벤더 ID 로 확인한다.
 *        꽂혀 있으면 → 그 카드의 노드를 찾아 **func 의 목록** 에 넣는다.
 *        비어 있으면 → **컨트롤러의 자유 목록** 에 넣는다.
 *      **같은 표가 "나눠 줄 것" 과 "이미 쓰이는 것" 을 함께 알려 주는
 *      셈이다.** 꽂힌 슬롯 몫은 그 카드를 뽑을 때 비로소 자유 목록으로
 *      돌아온다.
 *   5) 네 목록을 정렬·병합한다.
 *
 * **주소 단위 변환에 주의.**
 *   IO 는 표의 값을 그대로 쓴다(이미 바이트 단위).
 *   메모리와 prefetchable 은 16비트 왼쪽으로 민다 -- 표가 64KiB 단위로
 *     적기 때문이다.
 *   버스 번호는 `max_bus - secondary_bus + 1` 로 개수를 구한다.
 *
 * **범위 검사가 `temp_dword < 0x10000` 이다.** 시작 + 길이가 64K 를
 * 넘으면 그 항목을 무시한다. 표의 필드가 16비트라 그 이상을 표현할 수
 * 없기 때문으로 보인다.
 *
 * 반환값 계산이 특이하다 -- `rc = 1` 로 시작해 네 정렬 결과를 **& 로
 * 묶는다.** cpqhp_resource_sort_and_combine 은 빈 목록에 1 을 돌려주므로,
 * **네 목록이 전부 비었을 때만 1(실패)이 된다.** 하나라도 자원이 있으면
 * 0 이다. 위의 원문 주석이 그 뜻을 밝힌다.
 *
 * **bridged_slot 변수는 설정만 되고 쓰이지 않는다.** 위의 원문 주석이
 * "이것은 동작하지 않을 수 있고 쓰면 안 된다" 고 적어 두었다.
 *
 * i 를 10 으로 초기화했다가 곧바로 표의 항목 수로 덮어쓴다 --
 * 그 초기값은 의미가 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe 경로).
 *
 * 호출 체인:
 *   cpqphp_core.c → [이 함수]
 *     → detect_HRT_floating_pointer, compaq_nvram_load,
 *       cpqhp_slot_find, cpqhp_resource_sort_and_combine
 */
int cpqhp_find_available_resources(struct controller *ctrl, void __iomem *rom_start)
{
	u8 temp;
	/* [한국어] 카드가 꽂혀 있는지 표시하는 변수 */
	u8 populated_slot;
	/* [한국어] **브리지 뒤인지 표시하는 변수. 설정만 되고 읽히지 않는다** --
	 * 위의 원문 주석이 "동작하지 않을 수 있고 쓰면 안 된다" 고 적었다 */
	u8 bridged_slot;
	/* [한국어] 표의 현재 항목을 가리키는 포인터 */
	void __iomem *one_slot;
	/* [한국어] 찾은 표의 주소 */
	void __iomem *rom_resource_table;
	/* [한국어] 현재 항목에 해당하는 노드 */
	struct pci_func *func = NULL;
	/* [한국어] **10 으로 초기화하지만 아래에서 표의 항목 수로 덮어쓴다** --
	 * 그 초기값은 의미가 없다 */
	int i = 10, index;
	u32 temp_dword, rc;
	struct pci_resource *mem_node;
	struct pci_resource *p_mem_node;
	/* [한국어] IO 노드 */
	struct pci_resource *io_node;
	/* [한국어] 버스 구간 노드 */
	struct pci_resource *bus_node;

	/* [한국어] **ROM 에서 표를 찾는다.** 64KiB 범위를 훑는다 */
	rom_resource_table = detect_HRT_floating_pointer(rom_start, rom_start+0xffff);
	/* [한국어] 찾은 주소를 로그로 남긴다 */
	dbg("rom_resource_table = %p\n", rom_resource_table);

	/* [한국어] **표가 없으면 핫애드 기능 자체가 꺼진다** */
	if (rom_resource_table == NULL)
		return -ENODEV;

	/* Sum all resources and setup resource maps */
	unused_IRQ = readl(rom_resource_table + UNUSED_IRQ);
	/* [한국어] 읽은 비트맵을 로그로 남긴다 */
	dbg("unused_IRQ = %x\n", unused_IRQ);

	/* [한국어] 비트 위치를 셀 변수를 초기화한다 */
	temp = 0;
	/* [한국어] **비트맵을 오른쪽으로 밀어 가며 1 인 자리를 찾는다** */
	while (unused_IRQ) {
		/* [한국어] 가장 낮은 1 비트를 찾는다 */
		if (unused_IRQ & 1) {
			/* [한국어] **첫 번째로 찾은 IRQ 를 디스크용으로 삼는다** */
			cpqhp_disk_irq = temp;
			break;
		}
		/* [한국어] 다음 비트를 본다 */
		unused_IRQ = unused_IRQ >> 1;
		/* [한국어] 비트 위치를 센다 */
		temp++;
	}

	/* [한국어] 찾은 IRQ 를 로그로 남긴다 */
	dbg("cpqhp_disk_irq= %d\n", cpqhp_disk_irq);
	/* [한국어] **첫 번째로 찾은 비트를 건너뛴다** -- 이미 디스크용으로 썼다 */
	unused_IRQ = unused_IRQ >> 1;
	/* [한국어] 비트 위치도 하나 올린다 */
	temp++;

	/* [한국어] **두 번째 IRQ 를 찾는다** */
	while (unused_IRQ) {
		/* [한국어] 가장 낮은 1 비트를 찾는다 */
		if (unused_IRQ & 1) {
			/* [한국어] **두 번째로 찾은 IRQ 를 네트워크용으로 삼는다** */
			cpqhp_nic_irq = temp;
			break;
		}
		/* [한국어] 다음 비트를 본다 */
		unused_IRQ = unused_IRQ >> 1;
		/* [한국어] 비트 위치를 센다 */
		temp++;
	}

	/* [한국어] 찾은 IRQ 를 로그로 남긴다 */
	dbg("cpqhp_nic_irq= %d\n", cpqhp_nic_irq);
	/* [한국어] **PCIIRQ 항목을 읽어 덮어쓰지만 그 값을 쓰는 코드가 없다.**
	 * 과거에 쓰였다가 남은 것으로 보인다 */
	unused_IRQ = readl(rom_resource_table + PCIIRQ);

	/* [한국어] **0 으로 되돌리지만 아래에서 쓰이지 않는다** -- 위의 두 while 루프가
	 * 이미 끝났고 그 아래는 temp 를 읽지 않는다 */
	temp = 0;

	/* [한국어] 네트워크용 IRQ 가 정해졌는지 본다 */
	if (!cpqhp_nic_irq)
		/* [한국어] 못 찾았으면 컨트롤러 자신의 IRQ 를 쓴다 */
		cpqhp_nic_irq = ctrl->cfgspc_irq;

	/* [한국어] 디스크용 IRQ 가 정해졌는지 본다 */
	if (!cpqhp_disk_irq)
		/* [한국어] **못 찾았으면 컨트롤러 자신의 IRQ 를 쓴다** */
		cpqhp_disk_irq = ctrl->cfgspc_irq;

	/* [한국어] 두 IRQ 를 로그로 남긴다 */
	dbg("cpqhp_disk_irq, cpqhp_nic_irq= %d, %d\n", cpqhp_disk_irq, cpqhp_nic_irq);

	/* [한국어] NVRAM 에 저장된 상태를 읽는다 */
	rc = compaq_nvram_load(rom_start, ctrl);
	/* [한국어] 읽기 실패를 확인한다 */
	if (rc)
		/* [한국어] NVRAM 을 못 읽으면 포기한다 */
		return rc;

	/* [한국어] **헤더 뒤부터 항목이 시작된다** */
	one_slot = rom_resource_table + sizeof(struct hrt);

	/* [한국어] **표의 항목 수를 읽는다.** 위에서 i 를 10 으로 초기화했으나
	 * 여기서 덮어쓰므로 그 초기값은 의미가 없다 */
	i = readb(rom_resource_table + NUMBER_OF_ENTRIES);
	/* [한국어] 항목 수를 로그로 남긴다 */
	dbg("number_of_entries = %d\n", i);

	/* [한국어] 첫 항목이 유효한지 본다 */
	if (!readb(one_slot + SECONDARY_BUS))
		/* [한국어] **첫 항목이 비어 있으면 쓸 자원이 없다** */
		return 1;

	/* [한국어] 표 형식의 헤더를 로그로 남긴다 */
	dbg("dev|IO base|length|Mem base|length|Pre base|length|PB SB MB\n");

	/* [한국어] **항목이 남아 있고 보조 버스가 유효한 동안 반복한다** */
	while (i && readb(one_slot + SECONDARY_BUS)) {
		/* [한국어] **표 한 항목에서 장치·함수 번호를 읽는다.** 아래 열 줄이
		 * struct slot_rt 의 필드를 하나씩 꺼낸다 */
		u8 dev_func = readb(one_slot + DEV_FUNC);
		/* [한국어] 이 슬롯이 붙어 있는 버스 번호 */
		u8 primary_bus = readb(one_slot + PRIMARY_BUS);
		/* [한국어] 이 슬롯의 보조 버스 번호 */
		u8 secondary_bus = readb(one_slot + SECONDARY_BUS);
		/* [한국어] 이 슬롯 아래의 최대 버스 번호 */
		u8 max_bus = readb(one_slot + MAX_BUS);
		/* [한국어] IO 시작 */
		u16 io_base = readw(one_slot + IO_BASE);
		/* [한국어] IO 길이 */
		u16 io_length = readw(one_slot + IO_LENGTH);
		/* [한국어] 메모리 시작 */
		u16 mem_base = readw(one_slot + MEM_BASE);
		/* [한국어] 메모리 길이 */
		u16 mem_length = readw(one_slot + MEM_LENGTH);
		/* [한국어] prefetchable 메모리 시작 */
		u16 pre_mem_base = readw(one_slot + PRE_MEM_BASE);
		/* [한국어] prefetchable 메모리 길이 */
		u16 pre_mem_length = readw(one_slot + PRE_MEM_LENGTH);

		/* [한국어] 표의 한 줄을 로그로 남긴다. 위에 헤더 줄이 있어 표처럼 읽힌다 */
		dbg("%2.2x | %4.4x  | %4.4x | %4.4x   | %4.4x | %4.4x   | %4.4x |%2.2x %2.2x %2.2x\n",
		    dev_func, io_base, io_length, mem_base, mem_length, pre_mem_base, pre_mem_length,
		    primary_bus, secondary_bus, max_bus);

		/* If this entry isn't for our controller's bus, ignore it */
		if (primary_bus != ctrl->bus) {
			/* [한국어] 남은 항목 수를 줄인다 */
			i--;
			/* [한국어] 다음 항목으로 넘어간다 */
			one_slot += sizeof(struct slot_rt);
			continue;
		}
		/* find out if this entry is for an occupied slot */
		ctrl->pci_bus->number = primary_bus;
		/* [한국어] 이 슬롯에 카드가 있는지 벤더 ID 로 확인한다 */
		pci_bus_read_config_dword(ctrl->pci_bus, dev_func, PCI_VENDOR_ID, &temp_dword);
		/* [한국어] 읽은 값을 로그로 남긴다 */
		dbg("temp_D_word = %x\n", temp_dword);

		/* [한국어] **카드가 꽂혀 있는지 본다.** 전부 1 이면 빈 슬롯이다 */
		if (temp_dword != 0xFFFFFFFF) {
			/* [한국어] 첫 번째 함수부터 찾는다 */
			index = 0;
			/* [한국어] **dev_func 를 3비트 오른쪽으로 밀어 장치 번호를 얻는다** */
			func = cpqhp_slot_find(primary_bus, dev_func >> 3, 0);

			/* [한국어] **함수 번호가 맞는 노드를 찾을 때까지 훑는다.** dev_func 의
			 * 하위 3비트가 함수 번호다 */
			while (func && (func->function != (dev_func & 0x07))) {
				/* [한국어] 어느 함수를 보고 있는지 로그로 남긴다 */
				dbg("func = %p (bus, dev, fun) = (%d, %d, %d)\n", func, primary_bus, dev_func >> 3, index);
				/* [한국어] 같은 장치의 다음 함수를 찾는다 */
				func = cpqhp_slot_find(primary_bus, dev_func >> 3, index++);
			}

			/* If we can't find a match, skip this table entry */
			if (!func) {
				/* [한국어] 남은 항목 수를 줄인다 */
				i--;
				/* [한국어] 다음 항목으로 넘어간다 */
				one_slot += sizeof(struct slot_rt);
				continue;
			}
			/* this may not work and shouldn't be used */
			if (secondary_bus != primary_bus)
				/* [한국어] **보조 버스가 다르면 브리지 뒤로 본다.** 다만 위의 원문 주석이
				 * "이것은 동작하지 않을 수 있고 쓰면 안 된다" 고 적어 두었고,
				 * 실제로 bridged_slot 을 읽는 코드가 없다 */
				bridged_slot = 1;
			else
				/* [한국어] 같은 버스면 브리지가 아니다 */
				bridged_slot = 0;

			/* [한국어] **카드가 꽂혀 있다** -- 이 항목의 자원은 그 카드 몫이다 */
			populated_slot = 1;
		} else {
			/* [한국어] **빈 슬롯이다** -- 이 항목의 자원은 자유 목록으로 간다 */
			populated_slot = 0;
			/* [한국어] 브리지 뒤도 아니다 */
			bridged_slot = 0;
		}


		/* If we've got a valid IO base, use it */

		/* [한국어] 끝 주소를 계산해 범위를 확인할 준비를 한다 */
		temp_dword = io_base + io_length;

		/* [한국어] 시작이 유효하고 끝이 64K 를 넘지 않아야 한다 */
		if ((io_base) && (temp_dword < 0x10000)) {
			/* [한국어] IO 노드를 만든다 */
			io_node = kmalloc_obj(*io_node);
			/* [한국어] 할당 실패면 포기한다 */
			if (!io_node)
				return -ENOMEM;

			/* [한국어] 시작 주소를 그대로 넣는다 */
			io_node->base = io_base;
			/* [한국어] **IO 는 표의 값을 그대로 쓴다** -- 메모리와 달리 시프트하지 않는데,
			 * 이미 바이트 단위이기 때문이다 */
			io_node->length = io_length;

			/* [한국어] 찾은 구간을 로그로 남긴다 */
			dbg("found io_node(base, length) = %x, %x\n",
					io_node->base, io_node->length);
			/* [한국어] 빈 슬롯인지 로그로 남긴다 */
			dbg("populated slot = %d\n", populated_slot);
			/* [한국어] 빈 슬롯인지에 따라 갈린다 */
			if (!populated_slot) {
				/* [한국어] 목록에 연결한다 */
				io_node->next = ctrl->io_head;
				/* [한국어] 컨트롤러의 자유 목록에 넣는다 */
				ctrl->io_head = io_node;
			} else {
				/* [한국어] 목록에 연결한다 */
				io_node->next = func->io_head;
				/* [한국어] 이 카드가 쓰는 IO 목록에 넣는다 */
				func->io_head = io_node;
			}
		}

		/* If we've got a valid memory base, use it */
		temp_dword = mem_base + mem_length;
		/* [한국어] 시작이 유효하고 끝이 64K 를 넘지 않아야 한다 */
		if ((mem_base) && (temp_dword < 0x10000)) {
			/* [한국어] 메모리 노드를 만든다 */
			mem_node = kmalloc_obj(*mem_node);
			/* [한국어] 할당 실패면 포기한다 */
			if (!mem_node)
				return -ENOMEM;

			/* [한국어] 표의 값을 16비트 왼쪽으로 민다 */
			mem_node->base = mem_base << 16;

			/* [한국어] 길이도 16비트 왼쪽으로 민다 */
			mem_node->length = mem_length << 16;

			/* [한국어] 찾은 구간을 로그로 남긴다 */
			dbg("found mem_node(base, length) = %x, %x\n",
					mem_node->base, mem_node->length);
			/* [한국어] 빈 슬롯인지 로그로 남긴다 */
			dbg("populated slot = %d\n", populated_slot);
			/* [한국어] 빈 슬롯인지에 따라 갈린다 */
			if (!populated_slot) {
				/* [한국어] 목록에 연결한다 */
				mem_node->next = ctrl->mem_head;
				/* [한국어] 컨트롤러의 자유 목록에 넣는다 */
				ctrl->mem_head = mem_node;
			} else {
				/* [한국어] 목록에 연결한다 */
				mem_node->next = func->mem_head;
				/* [한국어] 이 카드가 쓰는 메모리 목록에 넣는다 */
				func->mem_head = mem_node;
			}
		}

		/* If we've got a valid prefetchable memory base, and
		 * the base + length isn't greater than 0xFFFF
		 */
		temp_dword = pre_mem_base + pre_mem_length;
		/* [한국어] **시작이 유효하고 끝이 64K 를 넘지 않아야 한다.** 표의 필드가
		 * 16비트라 그 이상을 표현할 수 없기 때문으로 보인다 */
		if ((pre_mem_base) && (temp_dword < 0x10000)) {
			/* [한국어] prefetchable 메모리 노드를 만든다 */
			p_mem_node = kmalloc_obj(*p_mem_node);
			/* [한국어] 할당 실패면 포기한다 */
			if (!p_mem_node)
				return -ENOMEM;

			/* [한국어] **표의 값을 16비트 왼쪽으로 민다** -- 표가 64KiB 단위로 적기 때문이다 */
			p_mem_node->base = pre_mem_base << 16;

			/* [한국어] **길이도 16비트 왼쪽으로 민다** */
			p_mem_node->length = pre_mem_length << 16;
			/* [한국어] 찾은 구간을 로그로 남긴다 */
			dbg("found p_mem_node(base, length) = %x, %x\n",
					p_mem_node->base, p_mem_node->length);
			/* [한국어] 빈 슬롯인지 로그로 남긴다 */
			dbg("populated slot = %d\n", populated_slot);

			/* [한국어] 빈 슬롯인지에 따라 갈린다 */
			if (!populated_slot) {
				/* [한국어] 목록에 연결한다 */
				p_mem_node->next = ctrl->p_mem_head;
				/* [한국어] 컨트롤러의 자유 목록에 넣는다 */
				ctrl->p_mem_head = p_mem_node;
			} else {
				/* [한국어] 목록에 연결한다 */
				p_mem_node->next = func->p_mem_head;
				/* [한국어] 이 카드가 쓰는 prefetchable 메모리 목록에 넣는다 */
				func->p_mem_head = p_mem_node;
			}
		}

		/* If we've got a valid bus number, use it
		 * The second condition is to ignore bus numbers on
		 * populated slots that don't have PCI-PCI bridges
		 */
		if (secondary_bus && (secondary_bus != primary_bus)) {
			/* [한국어] 버스 구간 노드를 만든다 */
			bus_node = kmalloc_obj(*bus_node);
			/* [한국어] 할당 실패면 포기한다 */
			if (!bus_node)
				return -ENOMEM;

			/* [한국어] 보조 버스 번호가 시작이다 */
			bus_node->base = secondary_bus;
			/* [한국어] **버스 개수를 계산한다** -- 최대 버스에서 보조 버스를 빼고 1 을 더한다 */
			bus_node->length = max_bus - secondary_bus + 1;
			/* [한국어] 찾은 버스 구간을 로그로 남긴다 */
			dbg("found bus_node(base, length) = %x, %x\n",
					bus_node->base, bus_node->length);
			/* [한국어] 빈 슬롯인지 로그로 남긴다 */
			dbg("populated slot = %d\n", populated_slot);
			/* [한국어] **빈 슬롯이면 자유 목록으로, 카드가 있으면 그 카드 몫으로 간다.**
			 * 같은 판단이 네 자원 종류에 모두 반복된다 */
			if (!populated_slot) {
				/* [한국어] 목록에 연결한다 */
				bus_node->next = ctrl->bus_head;
				/* [한국어] 컨트롤러의 자유 목록에 넣는다 */
				ctrl->bus_head = bus_node;
			} else {
				/* [한국어] 목록에 연결한다 */
				bus_node->next = func->bus_head;
				/* [한국어] 이 카드가 쓰는 버스 목록에 넣는다 */
				func->bus_head = bus_node;
			}
		}

		/* [한국어] 남은 항목 수를 줄인다 */
		i--;
		/* [한국어] 다음 항목으로 넘어간다 */
		one_slot += sizeof(struct slot_rt);
	}

	/* If all of the following fail, we don't have any resources for
	 * hot plug add
	 */
	rc = 1;
	/* [한국어] **메모리 목록을 정렬·병합한다.** 연산자가 `&=` 인 것에 주의 --
	 * 위의 원문 주석대로 "아래가 전부 실패하면 핫애드용 자원이 없다"
	 * 는 뜻이라, 네 목록이 모두 비었을 때만 1 이 된다 */
	rc &= cpqhp_resource_sort_and_combine(&(ctrl->mem_head));
	/* [한국어] prefetchable 메모리 목록도 정리한다 */
	rc &= cpqhp_resource_sort_and_combine(&(ctrl->p_mem_head));
	/* [한국어] IO 목록도 정리한다 */
	rc &= cpqhp_resource_sort_and_combine(&(ctrl->io_head));
	/* [한국어] 버스 목록도 정리한다 */
	rc &= cpqhp_resource_sort_and_combine(&(ctrl->bus_head));

	/* [한국어] **정렬 결과를 돌려준다.** 네 목록이 전부 비었을 때만 1(실패)이다 */
	return rc;
}


/*
 * cpqhp_return_board_resources
 *
 * this routine returns all resources allocated to a board to
 * the available pool.
 *
 * returns 0 if success
 */
/* [한국어]
 * cpqhp_return_board_resources - 카드가 쓰던 자원을 자유 목록으로 돌려준다
 *
 * @func:      자원을 반납할 노드.
 * @resources: 받을 자유 목록 꾸러미.
 * @return: 0 성공, 1 이면 func 가 NULL.
 *
 * 카드를 뽑을 때 그 카드가 쓰던 IO·메모리·prefetchable 메모리·버스 번호를
 * 컨트롤러의 자유 목록으로 옮긴다.
 *
 * 네 목록에 대해 같은 일을 반복한다.
 *   머리를 지역 변수에 잡고 func 쪽 머리를 NULL 로 만든다.
 *   노드를 하나씩 걸어가며 return_resource(cpqphp.h:461)로 목적지
 *     목록의 **머리에** 매단다.
 *   **다음 포인터를 미리 잡아 두는 것이 중요하다** -- return_resource 가
 *     node->next 를 덮어쓰기 때문이다.
 *
 * 옮긴 뒤 네 목록을 모두 정렬·병합한다. return_resource 는 그냥 머리에
 * 붙일 뿐이라 순서가 뒤죽박죽이 되고, 방금 반납한 조각이 기존 조각과
 * 인접해도 합쳐지지 않기 때문이다. **그 병합이 있어야 다음 카드가
 * 큰 덩어리를 요청할 수 있다.**
 *
 * 반환값 계산이 `rc |=` 인 것에 주의 -- 네 정렬 중 **하나라도 빈 목록이면
 * 1** 이 된다. cpqhp_find_available_resources 가 `&=` 로 "전부 비어야
 * 실패" 를 표현한 것과 정반대다. 두 함수가 같은 반환 규약을 다르게
 * 해석하는 셈인데, 호출자인 remove_board 와 configure_new_device 가
 * 이 반환값을 검사하지 않으므로 실질적 영향은 없다.
 *
 * **func 의 목록을 비우기만 하고 노드를 해제하지 않는다** --
 * 노드 자체는 목적지 목록으로 옮겨 갔으므로 맞다.
 * 해제하는 것은 cpqhp_destroy_board_resources 의 일이며, 그쪽은
 * 반납하지 않고 버린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   cpqphp_ctrl.c 의 remove_board / configure_new_device → [이 함수]
 *     → return_resource, cpqhp_resource_sort_and_combine
 */
int cpqhp_return_board_resources(struct pci_func *func, struct resource_lists *resources)
{
	int rc = 0;
	/* [한국어] 목록을 훑을 포인터 */
	struct pci_resource *node;
	/* [한국어] 다음 포인터를 미리 담아 둘 변수 */
	struct pci_resource *t_node;
	/* [한국어] 함수 진입을 로그로 남긴다 */
	dbg("%s\n", __func__);

	/* [한국어] 인자를 확인한다 */
	if (!func)
		/* [한국어] 노드가 없으면 반납할 것도 없다 */
		return 1;

	/* [한국어] IO 목록의 머리를 잡는다 */
	node = func->io_head;
	/* [한국어] **func 쪽 머리를 먼저 지운다** -- 옮기는 도중 이 목록을 다시
	 * 읽는 코드가 없도록 하는 것이다 */
	func->io_head = NULL;
	/* [한국어] IO 목록을 끝까지 옮긴다 */
	while (node) {
		/* [한국어] 다음 포인터를 미리 잡아 둔다 */
		t_node = node->next;
		/* [한국어] **자유 목록의 머리에 매단다.** 노드를 해제하지 않고 옮기는 것이
		 * cpqhp_destroy_board_resources 와 다른 점이다 */
		return_resource(&(resources->io_head), node);
		/* [한국어] 다음 노드로 넘어간다 */
		node = t_node;
	}

	/* [한국어] 메모리 목록의 머리를 잡는다 */
	node = func->mem_head;
	/* [한국어] func 쪽 머리를 지운다 */
	func->mem_head = NULL;
	/* [한국어] 메모리 목록을 옮긴다 */
	while (node) {
		/* [한국어] 다음 포인터를 미리 잡아 둔다 */
		t_node = node->next;
		/* [한국어] 자유 목록의 머리에 매단다 */
		return_resource(&(resources->mem_head), node);
		/* [한국어] 다음 노드로 넘어간다 */
		node = t_node;
	}

	/* [한국어] prefetchable 메모리 목록의 머리를 잡는다 */
	node = func->p_mem_head;
	/* [한국어] func 쪽 머리를 지운다 */
	func->p_mem_head = NULL;
	/* [한국어] prefetchable 메모리 목록을 옮긴다 */
	while (node) {
		/* [한국어] 다음 포인터를 미리 잡아 둔다 */
		t_node = node->next;
		/* [한국어] 자유 목록의 머리에 매단다 */
		return_resource(&(resources->p_mem_head), node);
		/* [한국어] 다음 노드로 넘어간다 */
		node = t_node;
	}

	/* [한국어] 버스 목록의 머리를 잡는다 */
	node = func->bus_head;
	/* [한국어] func 쪽 머리를 지운다 */
	func->bus_head = NULL;
	/* [한국어] 버스 목록을 끝까지 옮긴다 */
	while (node) {
		/* [한국어] **다음 포인터를 미리 잡아 둔다** -- return_resource 가 node->next 를
		 * 덮어쓰기 때문이다 */
		t_node = node->next;
		/* [한국어] 자유 목록의 머리에 매단다 */
		return_resource(&(resources->bus_head), node);
		/* [한국어] 다음 노드로 넘어간다 */
		node = t_node;
	}

	/* [한국어] **옮긴 뒤 네 목록을 모두 정렬·병합한다.** return_resource 가 머리에
	 * 붙이기만 하므로 순서가 뒤죽박죽이고, 방금 반납한 조각이 기존 것과
	 * 인접해도 합쳐지지 않기 때문이다.
	 * **연산자가 `|=` 다** -- 하나라도 빈 목록이면 1 이 된다.
	 * cpqhp_find_available_resources 가 `&=` 를 쓰는 것과 정반대인데,
	 * 호출자들이 이 반환값을 검사하지 않아 실질적 영향은 없다 */
	rc |= cpqhp_resource_sort_and_combine(&(resources->mem_head));
	/* [한국어] prefetchable 메모리 목록도 정리한다 */
	rc |= cpqhp_resource_sort_and_combine(&(resources->p_mem_head));
	/* [한국어] IO 목록도 정리한다 */
	rc |= cpqhp_resource_sort_and_combine(&(resources->io_head));
	/* [한국어] 버스 목록도 정리한다 */
	rc |= cpqhp_resource_sort_and_combine(&(resources->bus_head));

	/* [한국어] 정렬 결과를 돌려준다 */
	return rc;
}


/*
 * cpqhp_destroy_resource_list
 *
 * Puts node back in the resource list pointed to by head
 */
/* [한국어]
 * cpqhp_destroy_resource_list - 자유 목록 꾸러미를 통째로 해제한다
 *
 * @resources: 해제할 꾸러미.
 * @return: 없음.
 *
 * 네 목록의 모든 노드를 kfree 한다. **자유 목록으로 돌려주지 않고
 * 버린다** -- cpqhp_return_board_resources 와 대비되는 지점이다.
 *
 * 각 목록에 대해 같은 세 줄이 반복된다 -- 머리를 잡고, 머리를 NULL 로
 * 만들고, 걸어가며 해제한다. **해제 전에 next 를 미리 잡아 두는** 관용구를
 * 쓴다.
 *
 * 머리를 NULL 로 먼저 만드는 이유: 중간에 실패할 여지가 없으므로
 * 엄밀히는 필요 없지만, 해제한 포인터가 구조체에 남지 않게 하는
 * 방어다.
 *
 * 부르는 곳이 하나뿐이다 -- cpqphp_ctrl.c 의 configure_new_function 이
 * 브리지 설정에 실패했을 때, 그 브리지에게 준 임시 꾸러미
 * (temp_resources)를 정리하는 free_and_out 경로다. 그 꾸러미는 브리지
 * 뒤에서만 쓰이던 것이라 부모에게 돌려줄 것이 아니고, 부모 몫은
 * hold_ 사본으로 따로 보관되어 있다.
 *
 * 위의 원문 주석이 "노드를 head 가 가리키는 목록에 도로 넣는다" 고
 * 적혀 있는데, **실제 동작은 해제다.** return_resource 의 주석을
 * 복사해 온 것으로 보인다. 원문 그대로 두고 사실만 적어 둔다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   cpqphp_ctrl.c 의 configure_new_function(오류 경로) → [이 함수]
 */
void cpqhp_destroy_resource_list(struct resource_lists *resources)
{
	struct pci_resource *res, *tres;

	/* [한국어] IO 목록의 머리를 잡는다 */
	res = resources->io_head;
	/* [한국어] 머리를 먼저 지운다 */
	resources->io_head = NULL;

	/* [한국어] IO 목록을 끝까지 걸어가며 해제한다 */
	while (res) {
		/* [한국어] **해제 전에 다음 포인터를 미리 잡아 둔다** */
		tres = res;
		/* [한국어] 다음 노드로 넘어간다 */
		res = res->next;
		kfree(tres);
	}

	/* [한국어] 메모리 목록의 머리를 잡는다 */
	res = resources->mem_head;
	/* [한국어] 포인터를 지운다 */
	resources->mem_head = NULL;

	/* [한국어] 메모리 목록을 해제한다 */
	while (res) {
		/* [한국어] 해제할 노드를 기억해 둔다 */
		tres = res;
		/* [한국어] 다음 노드로 넘어간다 */
		res = res->next;
		kfree(tres);
	}

	/* [한국어] prefetchable 메모리 목록의 머리를 잡는다 */
	res = resources->p_mem_head;
	/* [한국어] 포인터를 지운다 */
	resources->p_mem_head = NULL;

	/* [한국어] prefetchable 메모리 목록을 해제한다 */
	while (res) {
		/* [한국어] 해제할 노드를 기억해 둔다 */
		tres = res;
		/* [한국어] 다음 노드로 넘어간다 */
		res = res->next;
		kfree(tres);
	}

	/* [한국어] 버스 목록의 머리를 잡는다 */
	res = resources->bus_head;
	/* [한국어] 포인터를 지운다 */
	resources->bus_head = NULL;

	/* [한국어] 버스 목록을 해제한다 */
	while (res) {
		/* [한국어] 해제할 노드를 기억해 둔다 */
		tres = res;
		/* [한국어] 다음 노드로 넘어간다 */
		res = res->next;
		kfree(tres);
	}
}


/*
 * cpqhp_destroy_board_resources
 *
 * Puts node back in the resource list pointed to by head
 */
/* [한국어]
 * cpqhp_destroy_board_resources - 노드가 들고 있던 자원 목록을 해제한다
 *
 * @func: 대상 노드.
 * @return: 없음.
 *
 * struct pci_func 에 매달린 네 목록을 전부 kfree 한다.
 * cpqhp_destroy_resource_list 와 코드가 거의 같고, 대상이
 * struct resource_lists 냐 struct pci_func 냐만 다르다.
 *
 * **slot_remove 가 노드를 해제하기 직전에 이것을 부른다.** 그러지
 * 않으면 노드만 사라지고 그 아래 매달린 리스트 넷이 통째로 샌다.
 *
 * 여기서도 **자유 목록으로 돌려주지 않고 버린다.** 돌려주는 것은
 * cpqhp_return_board_resources 의 일이며, cpqphp_ctrl.c 의
 * remove_board 가 그것을 먼저 부른 뒤 slot_remove 를 부르는 순서다.
 * 그래서 이 함수가 실제로 해제할 노드는 대개 없다 -- 이미 옮겨진 뒤라
 * 목록이 비어 있기 때문이다. **순서가 어긋난 경로에서만 실제로
 * 무언가를 해제하는 안전망 노릇을 한다.**
 *
 * 위의 원문 주석이 cpqhp_destroy_resource_list 와 똑같이
 * "노드를 목록에 도로 넣는다" 로 되어 있다. 실제 동작은 해제다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   cpqphp_ctrl.c 의 slot_remove → [이 함수] → kfree
 */
void cpqhp_destroy_board_resources(struct pci_func *func)
{
	struct pci_resource *res, *tres;

	/* [한국어] IO 목록의 머리를 잡는다 */
	res = func->io_head;
	/* [한국어] **머리를 먼저 지운다** -- 해제한 포인터가 구조체에 남지 않게 하는
	 * 방어다 */
	func->io_head = NULL;

	/* [한국어] IO 목록을 끝까지 걸어가며 해제한다 */
	while (res) {
		/* [한국어] **해제 전에 다음 포인터를 미리 잡아 둔다** */
		tres = res;
		/* [한국어] 다음 노드로 넘어간다 */
		res = res->next;
		kfree(tres);
	}

	/* [한국어] 메모리 목록의 머리를 잡는다 */
	res = func->mem_head;
	/* [한국어] 포인터를 지운다 */
	func->mem_head = NULL;

	/* [한국어] 메모리 목록을 해제한다 */
	while (res) {
		/* [한국어] 해제할 노드를 기억해 둔다 */
		tres = res;
		/* [한국어] 다음 노드로 넘어간다 */
		res = res->next;
		kfree(tres);
	}

	/* [한국어] prefetchable 메모리 목록의 머리를 잡는다 */
	res = func->p_mem_head;
	/* [한국어] 포인터를 지운다 */
	func->p_mem_head = NULL;

	/* [한국어] prefetchable 메모리 목록을 해제한다 */
	while (res) {
		/* [한국어] 해제할 노드를 기억해 둔다 */
		tres = res;
		/* [한국어] 다음 노드로 넘어간다 */
		res = res->next;
		kfree(tres);
	}

	/* [한국어] 버스 목록의 머리를 잡는다 */
	res = func->bus_head;
	/* [한국어] 포인터를 지운다 */
	func->bus_head = NULL;

	/* [한국어] 버스 목록을 끝까지 걸어가며 해제한다 */
	while (res) {
		/* [한국어] 해제할 노드를 기억해 둔다 */
		tres = res;
		/* [한국어] 다음 노드로 넘어간다 */
		res = res->next;
		kfree(tres);
	}
}
