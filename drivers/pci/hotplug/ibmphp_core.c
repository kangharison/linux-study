// SPDX-License-Identifier: GPL-2.0+
/*
 * IBM Hot Plug Controller Driver
 *
 * Written By: Chuck Cole, Jyoti Shah, Tong Yu, Irene Zubarev, IBM Corporation
 *
 * Copyright (C) 2001,2003 Greg Kroah-Hartman (greg@kroah.com)
 * Copyright (C) 2001-2003 IBM Corp.
 *
 * All rights reserved.
 *
 * Send feedback to <gregkh@us.ibm.com>
 *
 */

/*
 * [한국어 설명] 모듈 진입점과 핫플러그 코어 콜백, 슬롯 상태 기계 (ibmphp_core.c)
 *
 * === 파일의 역할 ===
 * 이 드라이버의 바깥 경계를 이루는 파일이다. 하는 일이 셋이다. 첫째, 모듈
 * 초기화와 해제(ibmphp_init/ibmphp_exit)를 맡아 나머지 네 파일을 정해진
 * 순서로 세우고 허문다. 둘째, 리눅스 핫플러그 코어가 부를 콜백 묶음
 * (ibmphp_hotplug_slot_ops)을 구현해 sysfs 를 통한 사용자 조작을 받는다.
 * 셋째, 카드를 꽂고 뽑는 실제 순서 — 검사, 버스 속도 맞추기, 전원 켜기,
 * 자원 설정, 커널 열거 — 를 정해진 단계로 밟는 상태 기계를 담는다.
 * 나머지 파일들은 각자 한 가지 일만 한다(EBDA 파싱, 자원 장부, 자원 설정,
 * 컨트롤러 통신). 그 넷을 실제 동작 순서로 엮는 것이 이 파일이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 드라이버 다섯 파일 중 유일하게 커널 쪽과 양방향으로 맞닿아 있다.
 * 위로는 핫플러그 코어와 PCI 코어를, 아래로는 나머지 네 파일을 부른다.
 *
 * 초기화 순서(ibmphp_init):
 *   ibmphp_init()
 *     -> ibmphp_pci_bus 를 만들고 0번 버스에서 통째로 베낀다
 *     -> ibmphp_access_ebda()          [ibmphp_ebda.c] EBDA 표를 읽는다
 *     -> ibmphp_rsrc_init()            [ibmphp_res.c]  자원 장부를 세운다
 *     -> get_max_slots()               슬롯 번호의 상한을 구한다
 *     -> ibmphp_register_pci()         [ibmphp_ebda.c] PCI 형 컨트롤러 등록
 *     -> init_ops()                    슬롯 상태를 읽고 빈 슬롯 전원을 끈다
 *     -> ibmphp_hpc_start_poll_thread() [ibmphp_hpc.c] 폴링 스레드를 띄운다
 *
 * 카드 삽입 경로(사용자가 sysfs 에 쓰거나 폴링 스레드가 감지):
 *   enable_slot()
 *     -> validate()          슬롯 번호와 현재 상태가 조작 가능한지
 *     -> set_bus()           버스가 비어 있으면 속도·모드를 카드에 맞춘다
 *     -> check_limitations() 같은 버스의 카드 수가 전기적 한계 안인지
 *     -> power_on()          컨트롤러에 전원 명령을 보내고 3초 기다린다
 *     -> ibmphp_configure_card()  [ibmphp_pci.c] BAR 와 브리지 창을 채운다
 *     -> ibm_configure_device()   PCI 코어에게 이제 열거하라고 알린다
 *
 * 카드 제거 경로:
 *   ibmphp_disable_slot() -> ibmphp_do_disable_slot()
 *     -> validate()
 *     -> ibm_unconfigure_device()   PCI 코어에서 struct pci_dev 를 걷어낸다
 *     -> ibmphp_unconfigure_card()  [ibmphp_pci.c] 장부에서 자원을 지운다
 *     -> ibmphp_hpc_writeslot(HPC_SLOT_OFF)  전원을 끈다
 *
 * 실행 컨텍스트: 대부분 프로세스 컨텍스트다. 콜백들은 sysfs 쓰기에서
 * 불리고, ibmphp_do_disable_slot() 은 ibmphp_hpc.c 의 폴링 커널 스레드에서도
 * 불린다. 초기화·해제 함수는 module_init/module_exit 경로다.
 *
 * === 타 모듈과의 연결 ===
 * 이 파일이 부르는 것:
 *   - ibmphp_ebda.c — ibmphp_access_ebda(), ibmphp_register_pci(),
 *     ibmphp_get_slot_from_physical_num(), ibmphp_find_same_bus_num(),
 *     그리고 해제 함수 셋. 슬롯 목록 ibmphp_slot_head 도 직접 훑는다.
 *   - ibmphp_res.c — ibmphp_rsrc_init(), ibmphp_free_resources(),
 *     ibmphp_print_test().
 *   - ibmphp_pci.c — ibmphp_configure_card(), ibmphp_unconfigure_card().
 *   - ibmphp_hpc.c — ibmphp_hpc_readslot()/writeslot() 으로 컨트롤러와
 *     주고받고, ibmphp_lock_operations()/unlock 으로 직렬화하며,
 *     폴링 스레드를 띄우고 멈춘다.
 *   - 커널 — 핫플러그 코어(pci_hp_del/destroy), PCI 코어(pci_scan_slot,
 *     pci_hp_add_bridge, pci_stop_and_remove_bus_device 등), 그리고
 *     BIOS IRQ 라우팅 표(pcibios_get_irq_routing_table).
 * 이 파일이 내놓는 것:
 *   - ibmphp_pci_bus — **이 드라이버 전체가 쓰는 껍데기 버스**. 여기서
 *     만들어지고, ibmphp_pci.c 가 config 접근 때마다 ->number 를 갈아
 *     끼우며 쓴다.
 *   - ibmphp_debug — debug() 매크로가 보는 전역 스위치.
 *   - ibmphp_hotplug_slot_ops — ibmphp_ebda.c 가 슬롯을 등록할 때 매단다.
 *   - ibmphp_init_devno() — ibmphp_ebda.c 가 슬롯마다 부른다.
 *   - ibmphp_update_slot_info(), ibmphp_do_disable_slot() — ibmphp_hpc.c 의
 *     폴링 스레드가 상태 변화를 감지했을 때 부른다.
 *
 * === 주요 함수/구조체 요약 ===
 * ibmphp_init()             : 모듈 진입점. 다섯 단계를 순서대로 세운다.
 * ibmphp_unload()           : 세운 것을 역순으로 허문다.
 * enable_slot()             : 카드 삽입의 전 과정을 밟는 상태 기계.
 * ibmphp_do_disable_slot()  : 카드 제거의 전 과정.
 * validate()                : 슬롯 번호와 현재 상태로 조작 가능 여부를 가른다.
 * set_bus()                 : 버스가 비었을 때 속도·모드를 카드에 맞춘다.
 * check_limitations()       : 같은 버스의 카드 수가 전기적 한계 안인지 본다.
 * ibmphp_init_devno()       : BIOS IRQ 라우팅 표에서 슬롯의 장치 번호와 IRQ.
 * ibm_configure_device()    : PCI 코어에게 열거를 맡긴다.
 * ibm_unconfigure_device()  : PCI 코어에서 struct pci_dev 를 걷어낸다.
 * bus_structure_fixup()     : 커널이 모르는 버스를 억지로 만들어 넣는다.
 * ibmphp_hotplug_slot_ops   : 핫플러그 코어가 부를 콜백 여덟 개.
 *
 * 이 파일이 다루는 구조체는 모두 ibmphp.h 에 정의되어 있어 그 파일에는
 * 손대지 않고, 이 파일이 가장 많이 만지는 struct slot 만 여기에 필드별로
 * 적는다. 슬롯은 ibmphp_ebda.c 의 ebda_rsrc_controller() 가 만들어
 * ibmphp_slot_head 에 매달고, 이 파일의 free_slots() 가 버린다. 살아 있는
 * 동안 프로세스 컨텍스트(sysfs)와 ibmphp_hpc.c 의 폴링 커널 스레드가
 * 함께 보므로, 이 구조체를 읽거나 고치는 구간은 모두
 * ibmphp_lock_operations() 의 뮤텍스로 직렬화된다.
 *
 * struct slot (ibmphp.h:689)
 *   u8 bus;
 *     이 슬롯이 붙어 있는 PCI 버스 번호.
 *     설정자: ebda_rsrc_controller() 가 EBDA 표에서 읽어 넣는다.
 *     읽는 자: ibmphp_init_devno() 가 IRQ 표를 맞출 때, enable_slot() 이
 *     pci_func 를 채울 때, pci_hp_register() 가 버스를 찾을 때.
 *   u8 device;
 *     그 버스에서의 PCI 장치 번호.
 *     설정자: **ibmphp_init_devno() 가 BIOS IRQ 라우팅 표에서 구한다.**
 *     EBDA 표에는 없는 값이라 여기서만 채워진다.
 *     읽는 자: enable_slot()/ibmphp_do_disable_slot() 이 pci_func 를
 *     채울 때, ibm_unconfigure_device() 가 devfn 을 만들 때.
 *   u8 number;
 *     물리 슬롯 번호. 시스템 전체에서 통짜로 매겨진 값이다.
 *     설정자: ebda_rsrc_controller().
 *     읽는 자: validate() 가 max_slots 와 비교할 때,
 *     ibmphp_get_slot_from_physical_num() 이 슬롯을 되찾을 때,
 *     create_file_name() 이 sysfs 이름을 지을 때.
 *   u8 real_physical_slot_num;
 *     이 파일과 드라이버 안에서 읽거나 쓰는 곳이 없다(전수 grep 확인).
 *   u32 capabilities;
 *     EBDA 표의 슬롯 능력 비트를 그대로 담은 값.
 *     설정자: ebda_rsrc_controller(). 읽는 곳은 없다.
 *   u8 supported_speed;
 *     이 슬롯이 낼 수 있는 최대 속도 등급(0=33, 1=66, 2=100, 3=133).
 *     설정자: ebda_rsrc_controller() 가 능력 비트를 풀어 넣는다.
 *     읽는 자: get_max_bus_speed() 와 set_bus() — 후자가 카드의 요구와
 *     이 값 중 작은 쪽을 골라 버스 속도를 정한다.
 *   u8 supported_bus_mode;
 *     PCI-X 를 지원하면 1, 통상 PCI 만이면 0.
 *     설정자/읽는 자는 supported_speed 와 같다.
 *   u8 flag;
 *     상류 주석이 "disable slot 과 폴링용" 이라 적어 둔 표시.
 *     설정자: ebda_rsrc_controller() 가 1 로 시작하고,
 *     ibmphp_do_disable_slot() 이 진입할 때마다 1 로 세운다.
 *     읽는 자: ibmphp_do_disable_slot() 이 진입 시점 값으로 갈린다 —
 *     1 이면 정상 제거 요청, 0 이면 걸쇠가 갑자기 열렸거나 전원 결함이
 *     난 비정상 상황이라 자원 정리 없이 커널 목록에서만 뺀다.
 *     0 을 넣는 곳은 ibmphp_hpc.c 의 폴링 루프다.
 *   u8 ctlr_index;
 *     컨트롤러 레지스터를 짚을 때 쓰는 슬롯 인덱스.
 *     설정자: ebda_rsrc_controller(). 읽는 자: ibmphp_hpc.c.
 *   struct hotplug_slot hotplug_slot;
 *     핫플러그 코어 쪽 구조체를 **값으로** 품고 있다. 그래서 코어가
 *     콜백에 넘겨 준 포인터에서 to_slot()(container_of 래퍼)으로 이
 *     구조체를 되찾을 수 있다.
 *     설정자: ebda_rsrc_controller() 가 ops 를 매달고 pci_hp_register()
 *     를 부른다. free_slots() 가 pci_hp_del/destroy 로 허문다.
 *   struct controller *ctrl;
 *     이 슬롯을 담당하는 핫플러그 컨트롤러.
 *     설정자: ebda_rsrc_controller(). free_slots() 가 NULL 로 끊는다.
 *     읽는 자: 컨트롤러와 주고받는 모든 자리 — power_on/off() 가
 *     CTLR_RESULT 를 볼 때, get_cur_bus_info() 가 지원 여부를 볼 때,
 *     calculate_first_slot() 이 ctlr_type 을 볼 때.
 *   struct pci_func *func;
 *     이 슬롯에 꽂힌 카드의 함수 목록 머리.
 *     설정자: enable_slot() 이 만들고, ibmphp_pci.c 가 다중 함수와
 *     브리지 뒤 장치를 이어 붙인다. ibmphp_unconfigure_card() 가 목록을
 *     버리고 나면 이 파일이 NULL 로 끊는다.
 *     읽는 자: ibm_slot_find() 가 목록을 훑을 때.
 *     값 범위: 카드가 없거나 아직 설정 전이면 NULL.
 *   u8 irq[4];
 *     인터럽트 핀 INTA~INTD 에 배정된 IRQ 번호.
 *     설정자: **ibmphp_init_devno() 가 IO_APIC_get_PCI_irq_vector() 로
 *     구한다.** 읽는 자: enable_slot() 이 pci_func 로 베끼고, 그것을
 *     ibmphp_pci.c 가 PCI_INTERRUPT_LINE 에 적는다.
 *   int bit_mode;
 *     0 이면 32비트, 1 이면 64비트라고 ibmphp.h 의 상류 주석이 적어
 *     두었다. 이 드라이버 안에서 읽거나 쓰는 곳은 없다(전수 grep 확인).
 *   struct bus_info *bus_on;
 *     이 슬롯이 붙은 버스의 정보(ibmphp_ebda.c 가 만든 것).
 *     설정자: ebda_rsrc_controller(). free_slots() 가 NULL 로 끊는다.
 *     읽는 자: get_cur_bus_info() 가 현재 속도를 여기 담고,
 *     is_bus_empty()/check_limitations() 가 슬롯 번호 구간과 한계값을
 *     여기서 꺼내며, ibmphp_update_slot_info() 가 현재 속도를 읽는다.
 *   struct list_head ibm_slot_list;
 *     ibmphp_slot_head 목록의 연결 고리.
 *   u8 status;
 *     컨트롤러가 준 슬롯 상태 바이트. 전원·존재·걸쇠·속도 불일치 비트가
 *     들어 있다.
 *     설정자: ibmphp_hpc_readslot(..., NULL) 계열 — 이 파일에서는
 *     slot_update() 가 부른다. 조회 콜백들은 사본에 담아 이 필드를
 *     건드리지 않는다.
 *     읽는 자: SLOT_POWER/PWRGD/PRESENT/LATCH/CONNECT/BUS_SPEED 매크로를
 *     거쳐 validate(), init_ops(), enable_slot(), is_bus_empty(),
 *     check_limitations() 가 본다.
 *   u8 ext_status;
 *     확장 상태 바이트. 카드의 요구 속도, PCI-X 여부, 모드 불일치,
 *     LED 깜빡임 여부가 들어 있다.
 *     설정자/읽는 자는 status 와 같으며, SLOT_SPEED/PCIX/BUS_MODE 와
 *     SLOT_ATTN 의 둘째 인자, print_card_capability() 가 쓴다.
 *   u8 busstatus;
 *     컨트롤러가 준 버스 상태 바이트.
 *     설정자: get_cur_bus_info() 가 READ_BUSSTATUS 로 읽어 담는다.
 *     읽는 자: 같은 함수가 CURRENT_BUS_SPEED/CURRENT_BUS_MODE 매크로로
 *     풀어 bus_on 에 옮긴다.
 *
 * === 상태 비트를 읽는 방식 ===
 * 슬롯의 현재 상태는 컨트롤러가 주는 두 바이트(status, ext_status)에
 * 담겨 있고, ibmphp.h 의 SLOT_* 매크로가 그 비트를 뜻으로 바꾼다.
 * 이 파일이 쓰는 것만 적는다.
 *   SLOT_PRESENT(status)   카드가 꽂혀 있는가. HPC_SLOT_EMPTY 면 빈 슬롯.
 *                          두 비트(PRSNT1/PRSNT2) 조합이라 값이 넷이다.
 *   SLOT_PWRGD(status)     전원이 정상으로 들어와 있는가.
 *   SLOT_POWER(status)     전원 스위치가 켜져 있는가. PWRGD 와 다르다 —
 *                          POWER 는 켜라고 했는가, PWRGD 는 실제로
 *                          들어왔는가다. 둘이 어긋나면 전원 결함이다.
 *   SLOT_LATCH(status)     걸쇠가 닫혀 있는가. 상류 주석이 밝히듯 PCI 규격과
 *                          비트의 의미가 반대다.
 *   SLOT_CONNECT(status)   버스에 연결되어 있는가.
 *   SLOT_BUS_SPEED(status) 버스 속도가 맞지 않는가(불일치면 1).
 *   SLOT_SPEED(ext)        카드가 요구하는 속도(33/66/133).
 *   SLOT_PCIX(ext)         카드가 PCI-X 인가.
 *   SLOT_BUS_MODE(ext)     버스 모드가 맞지 않는가.
 *   SLOT_ATTN(status,ext)  주의 LED 가 꺼짐/켜짐/깜빡임 중 어느 상태인가.
 *   CTLR_RESULT(ctrl->status) 방금 보낸 명령이 성공했는가.
 *
 * === 왜 삽입이 이렇게 여러 단계인가 ===
 * 코드에서 근거를 읽을 수 있는 것만 적는다.
 *   - **버스 속도를 먼저 정해야 한다.** PCI 버스는 한 버스에 하나의 속도만
 *     있을 수 있으므로, 이미 다른 카드가 도는 버스에서는 속도를 바꿀 수
 *     없다. set_bus() 가 is_bus_empty() 로 버스가 빈 것을 확인했을 때만
 *     속도를 건드리는 이유다.
 *   - **전기적 한계가 있다.** check_limitations() 의 상류 주석이 예를
 *     든다 — 같은 버스에 133MHz 카드가 둘 이상이거나 66MHz PCI 카드가
 *     셋 이상이면 안 된다. 그 한계값은 EBDA 표의 slots_at_*_* 필드에서
 *     온다. 즉 BIOS 가 알려 준 값이다.
 *   - **전원을 켠 뒤 3초를 기다린다.** power_on() 의 상류 주석이 그 대상을
 *     밝힌다 — ServeRAID 카드와 일부 66MHz PCI 카드다.
 *   - **자원 설정과 커널 열거가 나뉘어 있다.** ibmphp_configure_card() 가
 *     BAR 를 다 채운 뒤에야 ibm_configure_device() 가 PCI 코어에게
 *     열거를 맡긴다. 순서를 뒤집으면 코어가 자원 없는 장치를 보게 된다.
 *
 * === 이 파일에서 눈에 띄는 것들 ===
 * 코드에서 읽히는 것만 적는다.
 *   - **ibmphp_pci_bus 는 0번 버스를 통째로 베낀 껍데기다.** 진짜 버스가
 *     아니라 pci_bus_read_config_* 계열을 부르기 위한 매개체이며,
 *     ibmphp_pci.c 가 접근 때마다 ->number 를 갈아 끼운다.
 *   - bus_structure_fixup() 은 상류 주석이 스스로 "커널 버그를 고치려는
 *     것" 이라 밝히는 함수다. struct pci_bus 와 struct pci_dev 를 스택이
 *     아닌 힙에 잡아 임시로 꾸며 놓고 config 를 읽어 본 뒤 버린다.
 *   - irqs[16] 은 초기화에서 0 으로 채우기만 하고 읽는 곳이 없다
 *     (전수 grep 확인). 상류 주석이 용도를 적어 두었으나 그 용도로
 *     쓰이는 코드는 남아 있지 않다.
 *   - get_attention_status() 등 조회 콜백 넷은 슬롯 구조체를 지역 변수로
 *     통째로 memcpy 한 뒤 그 사본에 읽어 담는다. 진짜 슬롯 구조체를
 *     건드리지 않으려는 것으로 보이나, 정작 읽기 자체는 원본 포인터를
 *     넘겨 수행한다.
 *   - enable_slot() 의 error_cont 라벨은 error_nopower 에서 떨어져 오거나
 *     error_power 에서 뛰어 오는 두 경로로만 닿는다.
 *
 * === NVMe 관점 ===
 * drivers/nvme/ 에서 이 파일의 심볼을 쓰는 곳은 없다(전수 grep 확인).
 * 시기적으로도 접점이 없다 — 이 코드는 2001~2003년 IBM 서버용이고 NVMe
 * 규격은 2011년에 나왔다. 다만 오늘날 NVMe SSD 를 핫플러그로 꽂았을 때
 * pciehp 가 밟는 순서가 이 파일의 enable_slot() 과 같은 모양이다 —
 * 슬롯 상태 확인, 전원 인가, 링크가 올라오기를 기다림, 자원 배정,
 * pci_bus_add_devices() 로 드라이버 결합. 이 파일이 msleep(3000) 으로
 * 기다리는 자리를 오늘날에는 링크 훈련 완료 비트로 확인한다는 것이
 * 가장 큰 차이다.
 */

#include <linux/init.h> /* [한국어] __init/__exit 표시. 초기화·해제 함수를 별도 구역에 두어 뒤에 버릴 수 있게 한다 */
#include <linux/module.h> /* [한국어] module_init/module_exit/module_param/MODULE_LICENSE 등 모듈 뼈대 */
#include <linux/slab.h> /* [한국어] kmalloc_obj/kzalloc_obj/kfree — 껍데기 버스와 pci_func, 슬롯 구조체 할당에 쓴다 */
#include <linux/pci.h> /* [한국어] struct pci_bus/pci_dev, pci_find_bus, pci_scan_slot, pci_hp_add_bridge 등 PCI 코어 API */
#include <linux/interrupt.h> /* [한국어] 인터럽트 관련 정의. 이 파일이 직접 쓰는 심볼은 없다 */
#include <linux/delay.h> /* [한국어] msleep — power_on() 의 3초 대기와 set_bus() 의 1초 대기에 쓴다 */
#include <linux/wait.h> /* [한국어] 대기 큐 정의. 이 파일이 직접 쓰는 심볼은 없다 */
#include "../pci.h" /* [한국어] **drivers/pci 안쪽 전용 헤더.** pci_hp_add_bridge() 처럼 코어 밖에 공개되지 않은 함수를 쓰기 위해 필요하다 */
#include <asm/pci_x86.h>	/* for struct irq_routing_table */ /* [한국어] struct irq_routing_table 정의. ibmphp_init_devno() 가 BIOS 의 IRQ 라우팅 표를 읽는 데 필요하며 x86 전용이다 */
#include <asm/io_apic.h> /* [한국어] IO_APIC_get_PCI_irq_vector() — 인터럽트 핀을 실제 IRQ 번호로 바꾼다. 이 역시 x86 전용이며 이 트리에 arch/ 가 없어 구현은 확인 못 함 */
#include "ibmphp.h" /* [한국어] 이 드라이버 한 벌이 공유하는 헤더. struct slot/controller/bus_info 와 SLOT_* 상태 매크로, HPC_* 명령 코드가 여기 있다 */

#define attn_on(sl)  ibmphp_hpc_writeslot(sl, HPC_SLOT_ATTNON) /* [한국어] 주의 LED 켜기. ibmphp_hpc.c 에 명령을 보내는 것을 이름으로 감싼 것이다 */
#define attn_off(sl) ibmphp_hpc_writeslot(sl, HPC_SLOT_ATTNOFF) /* [한국어] 주의 LED 끄기 */
#define attn_LED_blink(sl) ibmphp_hpc_writeslot(sl, HPC_SLOT_BLINKLED) /* [한국어] 주의 LED 깜빡이기 — 작업 중임을 알린다 */
#define get_ctrl_revision(sl, rev) ibmphp_hpc_readslot(sl, READ_REVLEVEL, rev) /* [한국어] 컨트롤러의 개정 번호를 읽는다. init_ops() 가 0xFF(모름)일 때만 부른다 */
#define get_hpc_options(sl, opt) ibmphp_hpc_readslot(sl, READ_HPCOPTIONS, opt) /* [한국어] 컨트롤러가 지원하는 옵션을 읽는다. 역시 0xFF 일 때만 부른다 */

#define DRIVER_VERSION	"0.6" /* [한국어] 드라이버 판 번호. 초기화 때 한 줄 찍는 데만 쓴다 */
#define DRIVER_DESC	"IBM Hot Plug PCI Controller Driver" /* [한국어] 드라이버 설명. MODULE_DESCRIPTION 과 초기화 로그 양쪽에 쓴다 */

int ibmphp_debug; /* [한국어] **debug() 매크로가 보는 전역 스위치.** ibmphp.h:22 에 extern 으로 선언되어 다섯 파일이 모두 이 변수를 본다 */

static bool debug; /* [한국어] 모듈 파라미터로 받은 값. 초기화에서 위 전역에 옮겨 담는다 */
module_param(debug, bool, S_IRUGO | S_IWUSR); /* [한국어] sysfs 로 읽고 쓸 수 있게 노출한다 */
MODULE_PARM_DESC(debug, "Debugging mode enabled or not"); /* [한국어] 파라미터 설명 */
MODULE_LICENSE("GPL"); /* [한국어] 라이선스 표시. 이것이 없으면 커널이 오염 표시를 남긴다 */
MODULE_DESCRIPTION(DRIVER_DESC); /* [한국어] 모듈 설명 */

struct pci_bus *ibmphp_pci_bus; /* [한국어] **이 드라이버 전체가 쓰는 껍데기 버스.** ibmphp_init() 이 0번 버스를 통째로 베껴 만들고, ibmphp_pci.c 가 config 접근 때마다 ->number 를 갈아 끼운다. ibmphp.h 에 extern 으로 선언되어 있다 */
static int max_slots; /* [한국어] 슬롯 번호의 상한. get_max_slots() 가 채우고 validate() 가 읽는다 */

/* [한국어] PIC 모드에서 쓰고 있는 IRQ 를 기록해 두려던 배열. 바로 옆 상류 주석이
 * 용도를 적어 두었으나(MPS 표가 빈 슬롯의 기본 정보를 주지 않는 경우 대비),
 * 이 배열을 읽는 코드는 이 트리에 남아 있지 않다 — ibmphp_init() 이 0 으로
 * 채우는 것이 전부다(전수 grep 확인). */
static int irqs[16];    /* PIC mode IRQs we're using so far (in case MPS
			 * tables don't provide default info for empty slots */

static int init_flag; /* [한국어] 초기화가 진행 중임을 나타내는 표시. 서 있는 동안 slot_update() 가 버스 정보를 읽지 않는다 */

/* [한국어]
 * get_cur_bus_info - 컨트롤러에게 버스의 현재 속도와 모드를 물어 채운다
 *
 * @sl: 슬롯의 포인터의 포인터. 함수 끝에서 같은 값을 다시 담아 준다.
 * @return: 0 이면 성공. 컨트롤러 읽기가 실패하면 그 코드를 그대로 돌려준다.
 *
 * EBDA 표는 버스가 어떤 속도를 낼 수 있는지(능력)만 알려 줄 뿐, 지금 실제로
 * 몇 MHz 로 돌고 있는지는 알려 주지 않는다. 그것은 컨트롤러에게 물어야
 * 하므로 이 함수가 필요하다. 채우는 곳은 slot->bus_on 이 가리키는
 * struct bus_info 이며, 그것은 ibmphp_ebda.c 가 만든 목록의 항목이다.
 *
 * READ_BUS_STATUS(ctrl) 로 이 컨트롤러가 버스 상태 읽기를 지원하는지 먼저
 * 본다. 지원하지 않으면 rc 가 1 인 채로 남아 곧바로 1 을 돌려준다 —
 * 호출자 쪽에서는 실패로 다뤄진다. 지원하면 컨트롤러에서 busstatus 를
 * 읽고, CURRENT_BUS_SPEED/CURRENT_BUS_MODE 매크로로 그 비트를 풀어 담는다.
 * 모드 읽기를 지원하지 않는 컨트롤러에서는 모드를 0xFF(모름)로 둔다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 호출자가 이미
 * ibmphp_lock_operations() 로 직렬화해 둔 구간에서 불린다.
 *
 * 호출 체인:
 *   slot_update() / init_ops() / check_limitations() / enable_slot()
 *     -> [이 함수] -> ibmphp_hpc_readslot() [ibmphp_hpc.c]
 */
static inline int get_cur_bus_info(struct slot **sl)
{
	int rc = 1; /* [한국어] 1 로 시작한다 — 아래 if 가 거짓이면 이 값이 그대로 반환되어 호출자가 실패로 본다 */
	struct slot *slot_cur = *sl; /* [한국어] 포인터의 포인터에서 슬롯을 꺼낸다 */

	debug("options = %x\n", slot_cur->ctrl->options); /* [한국어] 컨트롤러가 지원하는 옵션을 남긴다 */
	debug("revision = %x\n", slot_cur->ctrl->revision); /* [한국어] 컨트롤러의 개정 번호를 남긴다 */

	if (READ_BUS_STATUS(slot_cur->ctrl)) /* [한국어] **이 컨트롤러가 버스 상태 읽기를 지원하는지** 먼저 본다. 옵션 비트로 판별한다 */
		rc = ibmphp_hpc_readslot(slot_cur, READ_BUSSTATUS, NULL); /* [한국어] 지원하면 컨트롤러에서 busstatus 를 읽는다. 세 번째 인자 NULL 은 슬롯 구조체 자신에 담으라는 뜻이다 */

	if (rc) /* [한국어] 지원하지 않거나 읽기가 실패했으면 */
		return rc; /* [한국어] 그 값을 그대로 돌려준다 */

	slot_cur->bus_on->current_speed = CURRENT_BUS_SPEED(slot_cur->busstatus); /* [한국어] busstatus 의 속도 비트를 풀어 버스 정보에 담는다. 이 값이 check_limitations() 와 ibmphp_update_slot_info() 의 근거가 된다 */
	if (READ_BUS_MODE(slot_cur->ctrl)) /* [한국어] 모드 읽기까지 지원하는 컨트롤러이면 */
		slot_cur->bus_on->current_bus_mode = /* [한국어] busstatus 의 모드 비트도 풀어 담고 */
				CURRENT_BUS_MODE(slot_cur->busstatus);
	else
		slot_cur->bus_on->current_bus_mode = 0xFF; /* [한국어] 모름을 뜻하는 0xFF 로 둔다. ibmphp_update_slot_info() 가 이 값을 만나면 PCI_SPEED_UNKNOWN 으로 접는다 */

	debug("busstatus = %x, bus_speed = %x, bus_mode = %x\n", /* [한국어] 읽은 세 값을 남긴다 */
			slot_cur->busstatus, /* [한국어] 원본 상태 바이트 */
			slot_cur->bus_on->current_speed, /* [한국어] 풀어낸 속도 */
			slot_cur->bus_on->current_bus_mode); /* [한국어] 풀어낸 모드 */

	*sl = slot_cur; /* [한국어] 상류 코드가 호출자 포인터에 슬롯을 다시 담는다 — 값이 같아 실제 효과는 없다 */
	return 0; /* [한국어] 여기까지 왔으면 성공 */
}

/* [한국어]
 * slot_update - 슬롯의 상태 전부를 컨트롤러에서 다시 읽는다
 *
 * @sl: 갱신할 슬롯의 포인터의 포인터.
 * @return: 0 이면 성공. 하위 호출이 실패하면 그 코드.
 *
 * 이 파일의 거의 모든 단계가 "지금 슬롯이 어떤 상태인가" 를 근거로
 * 갈리므로, 조작 전후로 상태를 다시 읽는 자리가 반복해서 필요하다.
 * 그 두 줄(상태 읽기 + 버스 정보 읽기)을 묶은 것이 이 함수다.
 *
 * READ_ALLSTAT 은 status 와 ext_status 를 한 번에 읽으라는 명령이다.
 * 그 다음 init_flag 가 서 있지 않을 때만 버스 정보까지 읽는다 —
 * 초기화 중에는 컨트롤러가 아직 버스 상태를 줄 준비가 되지 않았다고 보는
 * 것으로 보이며, init_ops() 가 끝나면서 그 표시를 내린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트와 폴링 커널 스레드 양쪽. 어느 쪽이든
 * 호출자가 ibmphp_lock_operations() 안에서 부른다.
 *
 * 호출 체인:
 *   validate() / init_ops() / enable_slot() / ibmphp_do_disable_slot() /
 *   is_bus_empty() / set_bus() -> [이 함수] -> ibmphp_hpc_readslot()
 *                                           -> get_cur_bus_info()
 */
static inline int slot_update(struct slot **sl)
{
	int rc; /* [한국어] 하위 호출의 결과를 담을 자리 */
	rc = ibmphp_hpc_readslot(*sl, READ_ALLSTAT, NULL); /* [한국어] **슬롯의 상태 두 바이트를 한 번에 읽는다.** 세 번째 인자 NULL 은 슬롯 구조체 자신의 status/ext_status 에 담으라는 뜻이다 */
	if (rc) /* [한국어] 읽기가 실패하면 */
		return rc; /* [한국어] 그 코드를 그대로 돌려준다 */
	if (!init_flag) /* [한국어] 초기화가 끝난 뒤에만 — 초기화 중에는 컨트롤러가 버스 상태를 줄 준비가 안 되어 있다고 보는 것이다 */
		rc = get_cur_bus_info(sl); /* [한국어] 버스의 현재 속도와 모드까지 함께 읽는다 */
	return rc; /* [한국어] 두 읽기의 결과를 돌려준다 */
}

/* [한국어]
 * get_max_slots - 등록된 슬롯 중 가장 큰 물리 슬롯 번호를 구한다
 *
 * @return: 가장 큰 슬롯 번호. 슬롯이 하나도 없으면 0.
 *
 * validate() 가 슬롯 번호의 상한을 검사하는 데 쓸 값을 미리 구해 둔다.
 * 바로 안쪽 상류 주석이 왜 개수를 세지 않고 최댓값을 찾는지 밝힌다 —
 * 핫플러그 슬롯의 번호가 늘 1 부터 시작하지 않고 4 부터 시작하기도 하기
 * 때문이다. 그래서 개수와 최댓값이 다를 수 있다.
 *
 * 결과는 파일 전역 max_slots 에 담긴다.
 *
 * 실행 컨텍스트: 모듈 초기화(프로세스 컨텍스트). __init 이며
 * ibmphp_access_ebda() 가 슬롯 목록을 다 채운 뒤에 불린다.
 *
 * 호출 체인:  ibmphp_init() -> [이 함수] -> list_for_each_entry()
 */
static int __init get_max_slots(void)
{
	struct slot *slot_cur; /* [한국어] 슬롯 목록을 훑을 반복 포인터 */
	u8 slot_count = 0; /* [한국어] 지금까지 본 가장 큰 슬롯 번호 */

	list_for_each_entry(slot_cur, &ibmphp_slot_head, ibm_slot_list) { /* [한국어] 등록된 슬롯을 하나씩 본다 */
		/* sometimes the hot-pluggable slots start with 4 (not always from 1) */
		slot_count = max(slot_count, slot_cur->number); /* [한국어] 개수를 세지 않고 **최댓값**을 찾는다. 바로 위 상류 주석이 그 이유를 밝힌다 — 슬롯 번호가 늘 1 부터 시작하지는 않기 때문이다 */
	}
	return slot_count; /* [한국어] 찾은 최댓값을 돌려준다. max_slots 에 담겨 validate() 의 상한이 된다 */
}

/* This routine will put the correct slot->device information per slot.  It's
 * called from initialization of the slot structures. It will also assign
 * interrupt numbers per each slot.
 * Parameters: struct slot
 * Returns 0 or errors
 */
/* [한국어]
 * ibmphp_init_devno - BIOS IRQ 라우팅 표에서 슬롯의 장치 번호와 IRQ 를 찾는다
 *
 * @cur_slot: 채울 슬롯의 포인터의 포인터. 상류 주석이 밝히듯 슬롯 구조체를
 *            초기화하는 쪽(ibmphp_ebda.c)에서 부르므로 static 이 아니다.
 * @return: 짝이 되는 항목을 찾아 채웠으면 0. 표가 없으면 -ENOMEM,
 *          표는 있는데 이 슬롯의 항목이 없으면 -1.
 *
 * 바로 위 상류 주석대로 슬롯마다 올바른 slot->device 를 넣고 인터럽트
 * 번호도 배정한다. EBDA 표는 슬롯이 몇 번 버스에 있는지는 알려 주지만
 * 그 버스에서 몇 번 장치인지는 알려 주지 않는다. 그것은 BIOS 의 IRQ 라우팅
 * 표에 있으므로 여기서 따로 가져온다.
 *
 * pcibios_get_irq_routing_table() 이 주는 표는 (버스, 슬롯 번호, devfn,
 * 핀별 IRQ 비트맵) 항목의 배열이다. 항목 개수는 표의 전체 크기에서 머리
 * 크기를 빼고 항목 하나의 크기로 나눠 구한다. 버스와 슬롯 번호가 모두
 * 맞는 항목을 찾으면 devfn 의 상위 5비트(PCI_SLOT)를 장치 번호로 쓰고,
 * 인터럽트 핀 네 개마다 IO_APIC_get_PCI_irq_vector() 로 실제 IRQ 번호를
 * 구해 slot->irq[] 에 담는다. 그 배열을 나중에 ibmphp_pci.c 의
 * configure_device()/configure_bridge() 가 PCI_INTERRUPT_LINE 에 적는다.
 *
 * 표는 이 함수가 부를 때마다 새로 받아 오고 나가기 전에 반드시 kfree
 * 한다 — 세 출구 모두에 kfree 가 있다.
 *
 * [관찰] pcibios_get_irq_routing_table() 과 IO_APIC_get_PCI_irq_vector()
 * 는 x86 전용이라 이 파일은 asm/pci_x86.h 와 asm/io_apic.h 를 직접
 * 포함한다. 이 트리에는 arch/ 가 없어 두 함수의 구현은 확인 못 함.
 *
 * 실행 컨텍스트: 모듈 초기화(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   ebda_rsrc_controller() [ibmphp_ebda.c] -> [이 함수]
 *     -> pcibios_get_irq_routing_table() -> IO_APIC_get_PCI_irq_vector()
 */
int ibmphp_init_devno(struct slot **cur_slot)
{
	struct irq_routing_table *rtable; /* [한국어] BIOS 가 준 IRQ 라우팅 표 */
	int len; /* [한국어] 표에 든 항목 개수 */
	int loop; /* [한국어] 표를 훑을 인덱스 */
	int i; /* [한국어] 인터럽트 핀 루프 인덱스 */

	rtable = pcibios_get_irq_routing_table(); /* [한국어] **BIOS 에서 IRQ 라우팅 표를 받아 온다.** 이 호출이 표를 새로 만들어 주므로 나가기 전에 반드시 kfree 해야 한다 */
	if (!rtable) { /* [한국어] 표가 없으면 */
		err("no BIOS routing table...\n"); /* [한국어] 그 사실을 알리고 */
		return -ENOMEM; /* [한국어] 메모리 부족으로 돌아간다 */
	}

	len = (rtable->size - sizeof(struct irq_routing_table)) / /* [한국어] **항목 개수를 크기에서 역산한다.** 표 전체 크기에서 머리 크기를 빼면 항목 배열의 바이트 수가 되고, 그것을 항목 하나의 크기로 나눈다 */
			sizeof(struct irq_info); /* [한국어] 항목 하나의 크기 */

	if (!len) { /* [한국어] 항목이 하나도 없으면 */
		kfree(rtable); /* [한국어] 받은 표를 버리고 */
		return -1; /* [한국어] 실패로 돌아간다 */
	}
	for (loop = 0; loop < len; loop++) { /* [한국어] 표의 항목을 하나씩 본다 */
		if ((*cur_slot)->number == rtable->slots[loop].slot && /* [한국어] 슬롯 번호가 같고 */
		    (*cur_slot)->bus == rtable->slots[loop].bus) { /* [한국어] 버스 번호도 같으면 이 슬롯의 항목이다 */
			(*cur_slot)->device = PCI_SLOT(rtable->slots[loop].devfn); /* [한국어] **devfn 의 상위 5비트가 장치 번호**다. EBDA 표가 알려 주지 않는 값이라 여기서 얻는다 */
			for (i = 0; i < 4; i++) /* [한국어] 인터럽트 핀 네 개(INTA~INTD)마다 */
				(*cur_slot)->irq[i] = IO_APIC_get_PCI_irq_vector((int) (*cur_slot)->bus, /* [한국어] 실제 IRQ 번호를 구해 담는다. 이 배열을 나중에 ibmphp_pci.c 가 PCI_INTERRUPT_LINE 에 적는다 */
						(int) (*cur_slot)->device, i); /* [한국어] 핀 번호를 0~3 으로 넘긴다 */

			debug("(*cur_slot)->irq[0] = %x\n", /* [한국어] INTA 에 배정된 IRQ 를 남긴다 */
					(*cur_slot)->irq[0]);
			debug("(*cur_slot)->irq[1] = %x\n", /* [한국어] INTB */
					(*cur_slot)->irq[1]);
			debug("(*cur_slot)->irq[2] = %x\n", /* [한국어] INTC */
					(*cur_slot)->irq[2]);
			debug("(*cur_slot)->irq[3] = %x\n", /* [한국어] INTD */
					(*cur_slot)->irq[3]);

			debug("rtable->exclusive_irqs = %x\n", /* [한국어] 표 전체에서 배타적으로 쓰이는 IRQ 목록 */
					rtable->exclusive_irqs);
			debug("rtable->slots[loop].irq[0].bitmap = %x\n", /* [한국어] INTA 가 쓸 수 있는 IRQ 후보 비트맵 */
					rtable->slots[loop].irq[0].bitmap);
			debug("rtable->slots[loop].irq[1].bitmap = %x\n", /* [한국어] INTB 의 후보 비트맵 */
					rtable->slots[loop].irq[1].bitmap);
			debug("rtable->slots[loop].irq[2].bitmap = %x\n", /* [한국어] INTC 의 후보 비트맵 */
					rtable->slots[loop].irq[2].bitmap);
			debug("rtable->slots[loop].irq[3].bitmap = %x\n", /* [한국어] INTD 의 후보 비트맵 */
					rtable->slots[loop].irq[3].bitmap);

			debug("rtable->slots[loop].irq[0].link = %x\n", /* [한국어] INTA 가 연결된 인터럽트 링크 */
					rtable->slots[loop].irq[0].link);
			debug("rtable->slots[loop].irq[1].link = %x\n", /* [한국어] INTB 의 링크 */
					rtable->slots[loop].irq[1].link);
			debug("rtable->slots[loop].irq[2].link = %x\n", /* [한국어] INTC 의 링크 */
					rtable->slots[loop].irq[2].link);
			debug("rtable->slots[loop].irq[3].link = %x\n", /* [한국어] INTD 의 링크 */
					rtable->slots[loop].irq[3].link);
			debug("end of init_devno\n"); /* [한국어] 끝났음을 남긴다 */
			kfree(rtable); /* [한국어] 받은 표를 버리고 */
			return 0; /* [한국어] 성공으로 돌아간다 */
		}
	}

	kfree(rtable); /* [한국어] 맞는 항목을 못 찾았어도 표는 반드시 버린다 */
	return -1; /* [한국어] 이 슬롯의 항목이 표에 없다는 뜻이다 */
}

/* [한국어]
 * power_on - 슬롯에 전원을 넣고 카드가 준비될 때까지 기다린다
 *
 * @slot_cur: 전원을 넣을 슬롯.
 * @return: 0 이면 성공. 명령 전달이 실패하면 그 코드, 컨트롤러가 명령을
 *          성공으로 끝내지 못했으면 -EIO.
 *
 * 확인이 두 겹이다. ibmphp_hpc_writeslot() 의 반환값은 "명령을 컨트롤러에
 * 전달했는가" 이고, CTLR_RESULT(ctrl->status) 는 "컨트롤러가 그 명령을
 * 성공으로 마쳤는가" 다. 둘은 다른 것이라 따로 본다.
 *
 * 마지막의 3초 대기는 상류 주석이 대상을 밝힌다 — ServeRAID 카드와 일부
 * 66MHz PCI 카드다. 전원이 들어온 뒤 카드가 config 공간에 응답할 준비가
 * 될 때까지 걸리는 시간을 고정값으로 기다리는 것이며, 오늘날의 pciehp 가
 * 링크 훈련 완료 비트를 확인하는 자리에 해당한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. msleep 으로 잠들므로 인터럽트나
 * 스핀락 안에서 부를 수 없다. 호출자가 ibmphp_lock_operations() 안에서
 * 부르므로 그 3초 동안 다른 슬롯 조작이 모두 막힌다.
 *
 * 호출 체인:  enable_slot() -> [이 함수] -> ibmphp_hpc_writeslot()
 */
static inline int power_on(struct slot *slot_cur)
{
	u8 cmd = HPC_SLOT_ON; /* [한국어] 전원 켜기 명령 코드 */
	int retval; /* [한국어] 명령 전달 결과 */

	retval = ibmphp_hpc_writeslot(slot_cur, cmd); /* [한국어] **컨트롤러에 전원 켜기 명령을 보낸다** */
	if (retval) { /* [한국어] 명령 전달 자체가 실패했으면 */
		err("power on failed\n"); /* [한국어] 그 사실을 알리고 */
		return retval; /* [한국어] 그 코드를 돌려준다 */
	}
	if (CTLR_RESULT(slot_cur->ctrl->status)) { /* [한국어] **명령은 전달되었지만 컨트롤러가 성공으로 마치지 못했는지** 따로 본다. 두 검사는 서로 다른 것이다 */
		err("command not completed successfully in power_on\n"); /* [한국어] 그 사실을 알리고 */
		return -EIO; /* [한국어] 입출력 오류로 돌아간다 */
	}
	msleep(3000);	/* For ServeRAID cards, and some 66 PCI */ /* [한국어] 상류 주석이 대상을 밝히는 고정 대기 — 전원이 들어온 뒤 카드가 config 에 응답할 준비가 될 때까지 기다린다. 오늘날 pciehp 가 링크 훈련 완료 비트를 확인하는 자리에 해당한다 */
	return 0; /* [한국어] 전원이 정상으로 들어왔으면 성공 */
}

/* [한국어]
 * power_off - 슬롯의 전원을 끈다
 *
 * @slot_cur: 전원을 끌 슬롯.
 * @return: 0 이면 성공. 명령 전달이 실패하면 그 코드, 컨트롤러가 명령을
 *          성공으로 끝내지 못했으면 -EIO.
 *
 * power_on() 과 대칭이지만 두 가지가 다르다. 대기가 없고 — 전원을 끄는
 * 데는 카드가 준비될 시간이 필요 없다 — 두 번째 검사에서 곧바로 돌아가지
 * 않고 retval 에 담아 마지막에 함께 돌려준다.
 *
 * 부르는 곳이 둘이다. init_ops() 가 부팅 때 BIOS 가 켜 둔 빈 슬롯을 끌
 * 때, 그리고 enable_slot() 의 error_power 경로가 켜 놓은 전원을 되돌릴 때.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 호출자가 직렬화해 둔 구간이다.
 *
 * 호출 체인:  init_ops() / enable_slot() 실패 경로 -> [이 함수]
 *               -> ibmphp_hpc_writeslot()
 */
static inline int power_off(struct slot *slot_cur)
{
	u8 cmd = HPC_SLOT_OFF; /* [한국어] 전원 끄기 명령 코드 */
	int retval; /* [한국어] 명령 전달 결과 */

	retval = ibmphp_hpc_writeslot(slot_cur, cmd); /* [한국어] 컨트롤러에 전원 끄기 명령을 보낸다 */
	if (retval) { /* [한국어] 명령 전달 자체가 실패했으면 */
		err("power off failed\n"); /* [한국어] 그 사실을 알리고 */
		return retval; /* [한국어] 그 코드를 돌려준다 */
	}
	if (CTLR_RESULT(slot_cur->ctrl->status)) { /* [한국어] 컨트롤러가 명령을 성공으로 마치지 못했으면 */
		err("command not completed successfully in power_off\n"); /* [한국어] 그 사실을 알리고 */
		retval = -EIO; /* [한국어] 곧바로 돌아가지 않고 담아만 둔다 — power_on() 과 다른 점이다 */
	}
	return retval; /* [한국어] 담아 둔 값을 돌려준다 */
}

/* [한국어]
 * set_attention_status - 주의 LED 를 끄거나 켜거나 깜빡이게 한다
 *
 * @hotplug_slot: 핫플러그 코어 쪽 슬롯 구조체.
 * @value: HPC_SLOT_ATTN_OFF / _ON / _BLINK 중 하나.
 * @return: 0 이면 성공. 셋 중 어느 것도 아니면 -ENODEV, 슬롯이 NULL 이어도
 *          -ENODEV.
 *
 * 핫플러그 코어가 sysfs 의 attention 파일에 쓰기가 있을 때 부르는 콜백이다.
 * 사용자가 어느 슬롯인지 눈으로 찾을 수 있게 하는 것이 목적이다.
 *
 * 코어가 준 값과 컨트롤러 명령 코드가 다른 체계라 switch 로 옮겨 담는다.
 * 값이 셋 중 어느 것도 아니면 명령을 보내지 않고 -ENODEV 로 끝낸다.
 *
 * **ibmphp_lock_operations() 로 감싸는 이유**는 이 드라이버가 컨트롤러와
 * 한 번에 하나씩만 주고받을 수 있기 때문이다. 그 잠금은 ibmphp_hpc.c 의
 * 뮤텍스이며 폴링 스레드와도 공유한다. 그래서 이 함수가 도는 동안
 * 폴링 스레드는 컨트롤러를 건드리지 못한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(sysfs 쓰기).
 *
 * 호출 체인:
 *   핫플러그 코어 -> [이 함수] -> ibmphp_lock_operations()
 *                              -> ibmphp_hpc_writeslot()
 */
static int set_attention_status(struct hotplug_slot *hotplug_slot, u8 value)
{
	int rc = 0; /* [한국어] 호출자에게 돌려줄 값. 성공이면 0 그대로다 */
	struct slot *pslot; /* [한국어] 이 드라이버의 슬롯 구조체 */
	u8 cmd = 0x00;	/* avoid compiler warning */ /* [한국어] 컨트롤러에 보낼 명령 코드 */

	debug("set_attention_status - Entry hotplug_slot[%lx] value[%x]\n", /* [한국어] 어느 슬롯에 무엇을 요청했는지 남긴다 */
			(ulong) hotplug_slot, value); /* [한국어] 슬롯 포인터와 요청 값 */
	ibmphp_lock_operations(); /* [한국어] **컨트롤러와 한 번에 하나씩만 주고받을 수 있으므로 직렬화한다.** 이 잠금은 ibmphp_hpc.c 의 뮤텍스이며 폴링 스레드와도 공유한다 */


	if (hotplug_slot) { /* [한국어] 슬롯이 넘어왔으면 */
		switch (value) { /* [한국어] 코어가 준 값을 컨트롤러 명령 코드로 옮긴다 — 두 체계가 다르기 때문이다 */
		case HPC_SLOT_ATTN_OFF: /* [한국어] LED 끄기 요청이면 */
			cmd = HPC_SLOT_ATTNOFF; /* [한국어] 끄기 명령 */
			break; /* [한국어] 갈래를 벗어난다 */
		case HPC_SLOT_ATTN_ON: /* [한국어] LED 켜기 요청이면 */
			cmd = HPC_SLOT_ATTNON; /* [한국어] 켜기 명령 */
			break; /* [한국어] 갈래를 벗어난다 */
		case HPC_SLOT_ATTN_BLINK: /* [한국어] 깜빡임 요청이면 */
			cmd = HPC_SLOT_BLINKLED; /* [한국어] 깜빡임 명령 */
			break; /* [한국어] 갈래를 벗어난다 */
		default: /* [한국어] 셋 중 어느 것도 아니면 */
			rc = -ENODEV; /* [한국어] 장치 없음을 담고 */
			err("set_attention_status - Error : invalid input [%x]\n", /* [한국어] 무엇이 잘못되었는지 알린다 */
					value); /* [한국어] 받은 값 */
			break; /* [한국어] 갈래를 벗어난다 */
		}
		if (rc == 0) { /* [한국어] 명령 코드가 정해졌을 때만 */
			pslot = to_slot(hotplug_slot); /* [한국어] 핫플러그 코어 쪽 구조체에서 이 드라이버의 슬롯을 되찾는다 */
			rc = ibmphp_hpc_writeslot(pslot, cmd); /* [한국어] **컨트롤러에 명령을 보낸다** */
		}
	} else
		rc = -ENODEV; /* [한국어] 장치 없음으로 끝낸다 */

	ibmphp_unlock_operations(); /* [한국어] 잠금을 푼다. 성공·실패 어느 쪽이든 반드시 지나는 자리다 */

	debug("set_attention_status - Exit rc[%d]\n", rc); /* [한국어] 결과를 남긴다 */
	return rc; /* [한국어] 그 결과를 돌려준다 */
}

/* [한국어]
 * get_attention_status - 주의 LED 의 현재 상태를 읽어 준다
 *
 * @hotplug_slot: 핫플러그 코어 쪽 슬롯 구조체.
 * @value: 읽은 값을 담을 자리. 0=꺼짐, 1=켜짐, 2=깜빡임.
 * @return: 0 이면 성공. 슬롯이 NULL 이면 -ENODEV, 읽기가 실패하면 그 코드.
 *
 * sysfs 의 attention 파일을 읽을 때 불리는 콜백이다. LED 상태는 두
 * 레지스터에 나뉘어 있어 status 와 ext_status 를 모두 읽은 뒤
 * SLOT_ATTN(status, ext_status) 로 합쳐야 한다 — 깜빡임 여부가
 * ext_status 에, 켜짐 여부가 status 에 있기 때문이다.
 *
 * [관찰] 슬롯 구조체를 지역 변수 myslot 에 통째로 memcpy 한 뒤 그 사본의
 * 필드에 읽어 담는다. 진짜 슬롯 구조체를 건드리지 않으려는 것으로 보이나,
 * 읽기 자체는 원본 포인터(pslot)를 넘겨 수행한다.
 *
 * [관찰] 슬롯이 NULL 이거나 읽기가 실패하면 *value 에 아무것도 담지 않는데,
 * 마지막 debug() 는 그 값을 읽는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(sysfs 읽기).
 *
 * 호출 체인:
 *   핫플러그 코어 -> [이 함수] -> ibmphp_hpc_readslot() x2
 */
static int get_attention_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	int rc = -ENODEV; /* [한국어] **실패를 기본값으로 둔다.** 슬롯이 없거나 읽기가 안 되면 이 값이 그대로 나간다 */
	struct slot *pslot; /* [한국어] 이 드라이버의 슬롯 구조체 */
	struct slot myslot; /* [한국어] 슬롯 구조체의 사본 — 읽은 값을 여기에 담는다 */

	debug("get_attention_status - Entry hotplug_slot[%lx] pvalue[%lx]\n", /* [한국어] 어느 슬롯을 조회하는지 남긴다 */
					(ulong) hotplug_slot, (ulong) value); /* [한국어] 슬롯 포인터와 결과를 받을 자리 */

	ibmphp_lock_operations(); /* [한국어] 컨트롤러 접근을 직렬화한다 */
	if (hotplug_slot) { /* [한국어] 슬롯이 넘어왔으면 */
		pslot = to_slot(hotplug_slot); /* [한국어] 핫플러그 코어 쪽 구조체에서 이 드라이버의 슬롯을 되찾는다 */
		memcpy(&myslot, pslot, sizeof(struct slot)); /* [한국어] 슬롯 구조체를 통째로 베낀다. 진짜 구조체를 건드리지 않으려는 것으로 보이나, 읽기 자체는 아래에서 원본 포인터를 넘겨 수행한다 */
		rc = ibmphp_hpc_readslot(pslot, READ_SLOTSTATUS, /* [한국어] **status 레지스터를 읽어 사본에 담는다** */
					 &myslot.status); /* [한국어] 담을 자리를 명시적으로 넘긴다 — slot_update() 처럼 NULL 을 넘기면 원본에 담긴다 */
		if (!rc) /* [한국어] 첫 읽기가 성공했으면 */
			rc = ibmphp_hpc_readslot(pslot, READ_EXTSLOTSTATUS, /* [한국어] **ext_status 도 읽는다.** LED 의 깜빡임 여부가 그쪽에 있어 두 레지스터가 모두 필요하다 */
						 &myslot.ext_status); /* [한국어] 역시 사본에 담는다 */
		if (!rc) /* [한국어] 둘 다 성공했으면 */
			*value = SLOT_ATTN(myslot.status, myslot.ext_status); /* [한국어] 두 바이트를 합쳐 꺼짐/켜짐/깜빡임 셋 중 하나로 접는다 */
	}

	ibmphp_unlock_operations(); /* [한국어] 잠금을 푼다 */
	debug("get_attention_status - Exit rc[%d] value[%x]\n", rc, *value); /* [한국어] 결과를 남긴다. 실패했을 때도 *value 를 읽는다 */
	return rc; /* [한국어] 그 결과를 돌려준다 */
}

/* [한국어]
 * get_latch_status - 슬롯 걸쇠가 열려 있는지 닫혀 있는지 읽어 준다
 *
 * @hotplug_slot: 핫플러그 코어 쪽 슬롯 구조체.
 * @value: 읽은 값을 담을 자리.
 * @return: 0 이면 성공. 슬롯이 NULL 이면 -ENODEV, 읽기가 실패하면 그 코드.
 *
 * sysfs 의 latch 파일을 읽을 때 불리는 콜백이다. 걸쇠가 열려 있으면 카드를
 * 뽑거나 꽂는 중이라는 뜻이므로, 이 드라이버는 걸쇠가 닫혀 있을 때만
 * 전원 조작을 허용한다(validate() 가 그 검사를 한다).
 *
 * ibmphp.h 의 SLOT_LATCH 정의에 상류 주석이 붙어 있다 — PCI 규격에서는
 * 비트가 꺼져 있으면 열림인데 이 컨트롤러는 반대라는 것이다. 그래서
 * 비트를 그대로 쓰지 않고 매크로를 거친다.
 *
 * 앞의 get_attention_status() 와 달리 레지스터 하나만 읽으면 된다.
 * 사본을 만드는 방식은 같다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(sysfs 읽기).
 *
 * 호출 체인:  핫플러그 코어 -> [이 함수] -> ibmphp_hpc_readslot()
 */
static int get_latch_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	int rc = -ENODEV; /* [한국어] 실패를 기본값으로 둔다 */
	struct slot *pslot; /* [한국어] 이 드라이버의 슬롯 구조체 */
	struct slot myslot; /* [한국어] 슬롯 구조체의 사본 */

	debug("get_latch_status - Entry hotplug_slot[%lx] pvalue[%lx]\n", /* [한국어] 어느 슬롯을 조회하는지 남긴다 */
					(ulong) hotplug_slot, (ulong) value); /* [한국어] 슬롯 포인터와 결과를 받을 자리 */
	ibmphp_lock_operations(); /* [한국어] 컨트롤러 접근을 직렬화한다 */
	if (hotplug_slot) { /* [한국어] 슬롯이 넘어왔으면 */
		pslot = to_slot(hotplug_slot); /* [한국어] 이 드라이버의 슬롯을 되찾는다 */
		memcpy(&myslot, pslot, sizeof(struct slot)); /* [한국어] 슬롯 구조체를 통째로 베낀다 */
		rc = ibmphp_hpc_readslot(pslot, READ_SLOTSTATUS, /* [한국어] status 레지스터만 읽으면 된다 — 걸쇠 상태는 그 한 바이트에 있다 */
					 &myslot.status); /* [한국어] 사본에 담는다 */
		if (!rc) /* [한국어] 읽기가 성공했으면 */
			*value = SLOT_LATCH(myslot.status); /* [한국어] 걸쇠 비트를 뜻으로 바꾼다. ibmphp.h 의 정의에 붙은 상류 주석대로 PCI 규격과 비트의 의미가 반대라 매크로를 거친다 */
	}

	ibmphp_unlock_operations(); /* [한국어] 잠금을 푼다 */
	debug("get_latch_status - Exit rc[%d] rc[%x] value[%x]\n", /* [한국어] 결과를 남긴다 */
			rc, rc, *value); /* [한국어] 같은 값을 십진과 십육진으로 함께 찍는다 */
	return rc; /* [한국어] 그 결과를 돌려준다 */
}


/* [한국어]
 * get_power_status - 슬롯에 전원이 정상으로 들어와 있는지 읽어 준다
 *
 * @hotplug_slot: 핫플러그 코어 쪽 슬롯 구조체.
 * @value: 읽은 값을 담을 자리.
 * @return: 0 이면 성공. 슬롯이 NULL 이면 -ENODEV, 읽기가 실패하면 그 코드.
 *
 * sysfs 의 power 파일을 읽을 때 불리는 콜백이다. **SLOT_POWER 가 아니라
 * SLOT_PWRGD 를 쓴다** — 전자는 "전원을 켜라고 했는가", 후자는 "실제로
 * 정상 전압이 들어왔는가" 다. 사용자에게 알려야 하는 것은 후자이며,
 * 둘이 어긋나면 전원 결함이라는 판단을 enable_slot() 이 따로 한다.
 *
 * 구조는 get_latch_status() 와 같다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(sysfs 읽기).
 *
 * 호출 체인:  핫플러그 코어 -> [이 함수] -> ibmphp_hpc_readslot()
 */
static int get_power_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	int rc = -ENODEV; /* [한국어] 실패를 기본값으로 둔다 */
	struct slot *pslot; /* [한국어] 이 드라이버의 슬롯 구조체 */
	struct slot myslot; /* [한국어] 슬롯 구조체의 사본 */

	debug("get_power_status - Entry hotplug_slot[%lx] pvalue[%lx]\n", /* [한국어] 어느 슬롯을 조회하는지 남긴다 */
					(ulong) hotplug_slot, (ulong) value); /* [한국어] 슬롯 포인터와 결과를 받을 자리 */
	ibmphp_lock_operations(); /* [한국어] 컨트롤러 접근을 직렬화한다 */
	if (hotplug_slot) { /* [한국어] 슬롯이 넘어왔으면 */
		pslot = to_slot(hotplug_slot); /* [한국어] 이 드라이버의 슬롯을 되찾는다 */
		memcpy(&myslot, pslot, sizeof(struct slot)); /* [한국어] 슬롯 구조체를 통째로 베낀다 */
		rc = ibmphp_hpc_readslot(pslot, READ_SLOTSTATUS, /* [한국어] status 레지스터를 읽는다 */
					 &myslot.status); /* [한국어] 사본에 담는다 */
		if (!rc) /* [한국어] 읽기가 성공했으면 */
			*value = SLOT_PWRGD(myslot.status); /* [한국어] **SLOT_POWER 가 아니라 SLOT_PWRGD 를 쓴다** — 전자는 켜라고 했는가, 후자는 실제로 정상 전압이 들어왔는가다. 사용자에게 알려야 하는 것은 후자다 */
	}

	ibmphp_unlock_operations(); /* [한국어] 잠금을 푼다 */
	debug("get_power_status - Exit rc[%d] rc[%x] value[%x]\n", /* [한국어] 결과를 남긴다 */
			rc, rc, *value); /* [한국어] 같은 값을 십진과 십육진으로 함께 찍는다 */
	return rc; /* [한국어] 그 결과를 돌려준다 */
}

/* [한국어]
 * get_adapter_present - 슬롯에 카드가 꽂혀 있는지 읽어 준다
 *
 * @hotplug_slot: 핫플러그 코어 쪽 슬롯 구조체.
 * @value: 1 이면 카드가 있고 0 이면 빈 슬롯이다.
 * @return: 0 이면 성공. 슬롯이 NULL 이면 -ENODEV, 읽기가 실패하면 그 코드.
 *
 * sysfs 의 adapter 파일을 읽을 때 불리는 콜백이다.
 *
 * SLOT_PRESENT 는 참/거짓이 아니라 네 가지 값을 돌려준다 — 두 개의 존재
 * 감지 핀(PRSNT1/PRSNT2) 조합으로 빈 슬롯인지, 그리고 카드가 몇 와트급인지
 * (7.5W / 15W / 25W)까지 구분하기 때문이다. 이 콜백이 답해야 하는 것은
 * 있고 없고이므로 HPC_SLOT_EMPTY 인지만 보고 0 또는 1 로 접는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(sysfs 읽기).
 *
 * 호출 체인:  핫플러그 코어 -> [이 함수] -> ibmphp_hpc_readslot()
 */
static int get_adapter_present(struct hotplug_slot *hotplug_slot, u8 *value)
{
	int rc = -ENODEV; /* [한국어] 실패를 기본값으로 둔다 */
	struct slot *pslot; /* [한국어] 이 드라이버의 슬롯 구조체 */
	u8 present; /* [한국어] 네 가지 값 중 하나를 담을 자리 */
	struct slot myslot; /* [한국어] 슬롯 구조체의 사본 */

	debug("get_adapter_status - Entry hotplug_slot[%lx] pvalue[%lx]\n", /* [한국어] 어느 슬롯을 조회하는지 남긴다 */
					(ulong) hotplug_slot, (ulong) value); /* [한국어] 슬롯 포인터와 결과를 받을 자리 */
	ibmphp_lock_operations(); /* [한국어] 컨트롤러 접근을 직렬화한다 */
	if (hotplug_slot) { /* [한국어] 슬롯이 넘어왔으면 */
		pslot = to_slot(hotplug_slot); /* [한국어] 이 드라이버의 슬롯을 되찾는다 */
		memcpy(&myslot, pslot, sizeof(struct slot)); /* [한국어] 슬롯 구조체를 통째로 베낀다 */
		rc = ibmphp_hpc_readslot(pslot, READ_SLOTSTATUS, /* [한국어] status 레지스터를 읽는다 */
					 &myslot.status); /* [한국어] 사본에 담는다 */
		if (!rc) { /* [한국어] 읽기가 성공했으면 */
			present = SLOT_PRESENT(myslot.status); /* [한국어] **참/거짓이 아니라 네 가지 값이 나온다** — 두 개의 존재 감지 핀 조합으로 빈 슬롯인지, 그리고 카드가 7.5W/15W/25W 중 어느 급인지까지 구분하기 때문이다 */
			if (present == HPC_SLOT_EMPTY) /* [한국어] 빈 슬롯이면 */
				*value = 0; /* [한국어] 없다고 답하고 */
			else
				*value = 1; /* [한국어] 있다고 답한다 — 와트 구분은 이 콜백이 답할 것이 아니다 */
		}
	}

	ibmphp_unlock_operations(); /* [한국어] 잠금을 푼다 */
	debug("get_adapter_present - Exit rc[%d] value[%x]\n", rc, *value); /* [한국어] 결과를 남긴다 */
	return rc; /* [한국어] 그 결과를 돌려준다 */
}

/* [한국어]
 * get_max_bus_speed - 슬롯이 낼 수 있는 최대 버스 속도를 코어 쪽 값으로 옮긴다
 *
 * @slot: 대상 슬롯.
 * @return: 0 이면 성공. 아는 속도가 아니면 -ENODEV.
 *
 * 이 드라이버의 속도 표현(BUS_SPEED_33/66/100/133 과 별도의 모드 플래그)과
 * PCI 코어의 표현(enum pci_bus_speed 하나에 PCI-X 까지 녹아 있음)이 달라
 * 옮겨 담는 함수가 필요하다.
 *
 * 옮기는 규칙은 "PCI-X 이면 한 칸 올린다" 이다. 33MHz 는 PCI-X 가 없으므로
 * 그대로 두고, 66MHz 는 모드가 PCI-X 일 때만 올리며, 100/133MHz 는 PCI-X
 * 에만 있는 속도이므로 조건 없이 올린다. 그렇게 만든 값을 코어의
 * bus->max_bus_speed 에 담는다.
 *
 * default 갈래의 상류 주석이 앞날을 적어 두었다 — 곧 256, 512 도 생길
 * 것이므로 이 코드를 바꿔야 한다는 것이다.
 *
 * [관찰] 잠금 안에서 하는 일은 슬롯 구조체의 두 필드를 지역 변수로
 * 읽는 것뿐이고, 실제 변환과 코어 구조체 쓰기는 잠금 밖에서 한다.
 *
 * 실행 컨텍스트: 모듈 초기화(프로세스 컨텍스트). init_ops() 안에서만
 * 불린다(전수 grep 확인).
 *
 * 호출 체인:  init_ops() -> [이 함수] -> ibmphp_lock_operations()
 */
static int get_max_bus_speed(struct slot *slot)
{
	int rc = 0; /* [한국어] 호출자에게 돌려줄 값 */
	u8 mode = 0; /* [한국어] 슬롯이 PCI-X 를 지원하는지 */
	enum pci_bus_speed speed; /* [한국어] **PCI 코어 쪽 속도 표현.** 이 드라이버의 표현과 체계가 달라 옮겨 담아야 한다 */
	struct pci_bus *bus = slot->hotplug_slot.pci_slot->bus; /* [한국어] 값을 담을 코어 쪽 버스 구조체 */

	debug("%s - Entry slot[%p]\n", __func__, slot); /* [한국어] 어느 슬롯인지 남긴다 */

	ibmphp_lock_operations(); /* [한국어] 잠금을 잡는다. 여기서 하는 일은 아래 두 줄의 읽기뿐이고, 변환과 코어 구조체 쓰기는 잠금 밖이다 */
	mode = slot->supported_bus_mode; /* [한국어] 슬롯이 지원하는 버스 모드 */
	speed = slot->supported_speed; /* [한국어] 슬롯이 지원하는 최대 속도 */
	ibmphp_unlock_operations(); /* [한국어] 잠금을 푼다 */

	switch (speed) { /* [한국어] 속도별로 코어 쪽 값을 정한다. 규칙은 "PCI-X 이면 한 칸 올린다" 이다 */
	case BUS_SPEED_33: /* [한국어] 33MHz 는 PCI-X 가 없으므로 */
		break; /* [한국어] 그대로 둔다 */
	case BUS_SPEED_66: /* [한국어] 66MHz 는 통상 PCI 와 PCI-X 가 모두 있으므로 */
		if (mode == BUS_MODE_PCIX) /* [한국어] 모드가 PCI-X 일 때만 */
			speed += 0x01; /* [한국어] 한 칸 올린다 */
		break; /* [한국어] 갈래를 벗어난다 */
	case BUS_SPEED_100: /* [한국어] 100MHz 와 */
	case BUS_SPEED_133: /* [한국어] 133MHz 는 PCI-X 에만 있는 속도이므로 */
		speed += 0x01; /* [한국어] 조건 없이 한 칸 올린다 */
		break; /* [한국어] 갈래를 벗어난다 */
	default: /* [한국어] 아는 속도가 아니면 */
		/* Note (will need to change): there would be soon 256, 512 also */
		rc = -ENODEV; /* [한국어] 장치 없음으로 표시한다. 바로 위 상류 주석이 앞날을 적어 두었다 — 곧 256, 512 도 생길 것이므로 이 코드를 바꿔야 한다는 내용이다 */
	}

	if (!rc) /* [한국어] 변환에 성공했을 때만 */
		bus->max_bus_speed = speed; /* [한국어] 코어 쪽 버스 구조체에 담는다. sysfs 의 max_bus_speed 로 보이는 값이다 */

	debug("%s - Exit rc[%d] speed[%x]\n", __func__, rc, speed); /* [한국어] 결과를 남긴다 */
	return rc; /* [한국어] 그 결과를 돌려준다 */
}

/****************************************************************************
 * This routine will initialize the ops data structure used in the validate
 * function. It will also power off empty slots that are powered on since BIOS
 * leaves those on, albeit disconnected
 ****************************************************************************/
/* [한국어]
 * init_ops - 슬롯마다 컨트롤러 정보를 채우고 빈 슬롯의 전원을 끈다
 *
 * @return: 0 이면 성공. 컨트롤러 조회가 실패하면 -1, 하위 호출이 실패하면
 *          그 코드.
 *
 * 바로 위 상류 주석이 두 가지 목적을 밝힌다. 하나는 validate() 가 쓸
 * 데이터를 채우는 것이고, 다른 하나는 **BIOS 가 켜 둔 채로 둔 빈 슬롯의
 * 전원을 끄는 것**이다. 상류 주석이 그 사정을 적어 두었다 — BIOS 는 카드가
 * 없는 슬롯도 전원을 켠 채 두는데, 연결은 되어 있지 않은 상태다.
 *
 * 슬롯마다 순서대로 넷을 한다.
 *   1. 컨트롤러의 개정 번호가 아직 0xFF(모름)면 컨트롤러에게 묻는다.
 *   2. 버스의 현재 속도가 아직 0xFF 면 get_cur_bus_info() 로 읽는다.
 *   3. 최대 속도를 코어 쪽 값으로 옮긴다.
 *   4. 컨트롤러가 지원하는 옵션이 아직 0xFF 면 묻는다.
 * 0xFF 는 ibmphp_ebda.c 가 "아직 모름" 으로 넣어 둔 값이라, 이 검사가
 * 컨트롤러 하나당 한 번만 묻게 만든다 — 같은 컨트롤러의 슬롯이 여럿이어도
 * 두 번째부터는 이미 채워져 있다.
 *
 * 그 뒤 상태를 다시 읽어, **전원은 정상인데 카드가 없고 걸쇠도 열려 있으면**
 * 전원을 끈다. 셋이 모두 맞아야 하는 이유는 카드가 꽂혀 있거나 걸쇠가
 * 닫혀 있으면 사용 중일 수 있기 때문이다.
 *
 * 마지막에 init_flag 를 0 으로 내린다. 그 뒤로는 slot_update() 가 버스
 * 정보까지 함께 읽게 된다.
 *
 * 실행 컨텍스트: 모듈 초기화(프로세스 컨텍스트). __init 이다.
 * [관찰] 이 함수는 잠금을 잡지 않는다. 폴링 스레드는 아직 뜨기 전이라
 * 경쟁이 없다.
 *
 * 호출 체인:
 *   ibmphp_init() -> [이 함수] -> get_ctrl_revision()/get_hpc_options()
 *                              -> get_cur_bus_info() -> get_max_bus_speed()
 *                              -> slot_update() -> power_off()
 */
static int __init init_ops(void)
{
	struct slot *slot_cur; /* [한국어] 슬롯 목록을 훑을 반복 포인터 */
	int retval; /* [한국어] slot_update() 의 결과 */
	int rc; /* [한국어] power_off() 의 결과 */

	list_for_each_entry(slot_cur, &ibmphp_slot_head, ibm_slot_list) { /* [한국어] 등록된 슬롯을 하나씩 본다 */
		debug("BEFORE GETTING SLOT STATUS, slot # %x\n", /* [한국어] 어느 슬롯을 보는지 남긴다 */
							slot_cur->number); /* [한국어] 슬롯 번호 */
		if (slot_cur->ctrl->revision == 0xFF) /* [한국어] **컨트롤러의 개정 번호가 아직 모름이면** — ibmphp_ebda.c 가 0xFF 로 넣어 두었다 */
			if (get_ctrl_revision(slot_cur, /* [한국어] 컨트롤러에게 물어 채운다. 같은 컨트롤러의 슬롯이 여럿이어도 이 검사 덕에 한 번만 묻는다 */
						&slot_cur->ctrl->revision)) /* [한국어] 담을 자리 */
				return -1; /* [한국어] 못 물어보면 초기화를 접는다 */

		if (slot_cur->bus_on->current_speed == 0xFF) /* [한국어] 버스의 현재 속도가 아직 모름이면 */
			if (get_cur_bus_info(&slot_cur)) /* [한국어] 컨트롤러에게 물어 채운다. 못 물어보면 초기화를 접는다 */
				return -1;
		get_max_bus_speed(slot_cur); /* [한국어] 슬롯이 낼 수 있는 최대 속도를 코어 쪽 값으로 옮긴다. 반환값을 보지 않는다 */

		if (slot_cur->ctrl->options == 0xFF) /* [한국어] 컨트롤러가 지원하는 옵션이 아직 모름이면 */
			if (get_hpc_options(slot_cur, &slot_cur->ctrl->options)) /* [한국어] 컨트롤러에게 물어 채운다 */
				return -1; /* [한국어] 못 물어보면 초기화를 접는다 */

		retval = slot_update(&slot_cur); /* [한국어] 슬롯 상태를 새로 읽는다 */
		if (retval) /* [한국어] 실패하면 */
			return retval; /* [한국어] 그 코드로 초기화를 접는다 */

		debug("status = %x\n", slot_cur->status); /* [한국어] 읽은 status 를 남긴다 */
		debug("ext_status = %x\n", slot_cur->ext_status); /* [한국어] ext_status 도 남긴다 */
		debug("SLOT_POWER = %x\n", SLOT_POWER(slot_cur->status)); /* [한국어] 전원 스위치가 켜져 있는지 */
		debug("SLOT_PRESENT = %x\n", SLOT_PRESENT(slot_cur->status)); /* [한국어] 카드가 꽂혀 있는지 */
		debug("SLOT_LATCH = %x\n", SLOT_LATCH(slot_cur->status)); /* [한국어] 걸쇠가 닫혀 있는지 */

		if ((SLOT_PWRGD(slot_cur->status)) && /* [한국어] **전원은 정상으로 들어와 있는데** */
		    !(SLOT_PRESENT(slot_cur->status)) && /* [한국어] 카드는 없고 */
		    !(SLOT_LATCH(slot_cur->status))) { /* [한국어] 걸쇠도 열려 있으면 — 셋이 모두 맞아야 정말로 쓰이지 않는 슬롯이다 */
			debug("BEFORE POWER OFF COMMAND\n"); /* [한국어] 끄기 직전임을 남기고 */
				rc = power_off(slot_cur); /* [한국어] 전원을 끈다. 바로 위 상류 주석대로 BIOS 가 빈 슬롯도 켠 채 두기 때문이다 */
				if (rc) /* [한국어] 끄기가 실패하면 */
					return rc; /* [한국어] 그 코드로 초기화를 접는다 */

	/*		retval = slot_update(&slot_cur);
	 *		if (retval)
	 *			return retval;
	 *		ibmphp_update_slot_info(slot_cur);
	 */
		}
	}
	init_flag = 0; /* [한국어] **초기화가 끝났다고 표시한다.** 이 뒤로는 slot_update() 가 버스 정보까지 함께 읽는다 */
	return 0; /* [한국어] 모든 슬롯을 살폈으면 성공 */
}

/* This operation will check whether the slot is within the bounds and
 * the operation is valid to perform on that slot
 * Parameters: slot, operation
 * Returns: 0 or error codes
 */
/* [한국어]
 * validate - 슬롯 번호와 현재 상태로 그 조작이 가능한지 가른다
 *
 * @slot_cur: 검사할 슬롯.
 * @opn: 하려는 조작 - ENABLE(켜기) 또는 DISABLE(끄기).
 * @return: 0 이면 해도 된다. 슬롯이 NULL 이면 -ENODEV, 번호가 범위를
 *          벗어나면 -EBADSLT, 상태가 맞지 않으면 -EINVAL.
 *
 * 바로 위 상류 주석대로 슬롯이 범위 안인지와 그 조작이 유효한지를 함께
 * 본다. 삽입과 제거 양쪽의 첫 관문이다.
 *
 * 번호 검사는 get_max_slots() 가 구해 둔 max_slots 를 상한으로 쓴다.
 * 그 다음 slot_update() 로 상태를 새로 읽어 조작별로 조건을 본다.
 *   ENABLE  — 전원이 아직 안 들어와 있고, 카드가 꽂혀 있고, 걸쇠가 닫혀
 *             있어야 한다. 셋이 "지금 켤 수 있는 상태" 를 뜻한다.
 *   DISABLE — 전원이 들어와 있고, 카드가 꽂혀 있고, 걸쇠가 닫혀 있어야
 *             한다. ENABLE 과 첫 조건만 반대다.
 * 어느 갈래도 조건을 만족하지 못하면 아래로 떨어져 -EINVAL 로 끝난다.
 * default 갈래 역시 마찬가지라, 아는 조작이 아니면 거절된다.
 *
 * **걸쇠가 닫혀 있어야 한다는 조건이 양쪽에 공통**인 이유는, 걸쇠가 열려
 * 있다는 것은 사용자가 지금 카드를 손대고 있다는 뜻이기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트와 폴링 커널 스레드 양쪽. 호출자가
 * 잠금 안에서 부른다.
 *
 * 호출 체인:
 *   enable_slot() / ibmphp_do_disable_slot() -> [이 함수] -> slot_update()
 */
static int validate(struct slot *slot_cur, int opn)
{
	int number; /* [한국어] 슬롯 번호를 int 로 비교하기 위한 사본 */
	int retval; /* [한국어] slot_update() 의 결과 */

	if (!slot_cur) /* [한국어] 슬롯이 없으면 */
		return -ENODEV; /* [한국어] 장치 없음으로 거절한다 */
	number = slot_cur->number; /* [한국어] 물리 슬롯 번호를 꺼낸다 */
	if ((number > max_slots) || (number < 0)) /* [한국어] **get_max_slots() 가 구해 둔 상한을 넘거나 음수이면** 있을 수 없는 번호다 */
		return -EBADSLT; /* [한국어] 잘못된 슬롯으로 거절한다 */
	debug("slot_number in validate is %d\n", slot_cur->number); /* [한국어] 검사 중인 슬롯 번호를 남긴다 */

	retval = slot_update(&slot_cur); /* [한국어] 상태를 새로 읽는다 — 판단의 근거가 최신이어야 한다 */
	if (retval) /* [한국어] 읽기가 실패하면 */
		return retval; /* [한국어] 그 코드로 거절한다 */

	switch (opn) { /* [한국어] 하려는 조작에 따라 조건이 다르다 */
		case ENABLE: /* [한국어] **켜기 요청** */
			if (!(SLOT_PWRGD(slot_cur->status)) && /* [한국어] 전원이 아직 안 들어와 있고 */
			     (SLOT_PRESENT(slot_cur->status)) && /* [한국어] 카드는 꽂혀 있고 */
			     !(SLOT_LATCH(slot_cur->status))) /* [한국어] 걸쇠는 닫혀 있어야 한다 — 걸쇠가 열려 있다는 것은 사용자가 지금 손대고 있다는 뜻이다 */
				return 0; /* [한국어] 셋이 다 맞으면 허용한다 */
			break; /* [한국어] 하나라도 아니면 아래로 떨어진다 */
		case DISABLE: /* [한국어] **끄기 요청** */
			if ((SLOT_PWRGD(slot_cur->status)) && /* [한국어] 전원이 들어와 있고 — 켜기와 여기만 반대다 */
			    (SLOT_PRESENT(slot_cur->status)) && /* [한국어] 카드는 꽂혀 있고 */
			    !(SLOT_LATCH(slot_cur->status))) /* [한국어] 걸쇠는 닫혀 있어야 한다 */
				return 0; /* [한국어] 셋이 다 맞으면 허용한다 */
			break; /* [한국어] 하나라도 아니면 아래로 떨어진다 */
		default: /* [한국어] 아는 조작이 아니면 */
			break; /* [한국어] 그대로 아래로 떨어진다 */
	}
	err("validate failed....\n"); /* [한국어] 허용되지 않는 조작임을 알리고 */
	return -EINVAL; /* [한국어] 인자 오류로 거절한다 */
}

/****************************************************************************
 * This routine is for updating the data structures in the hotplug core
 * Parameters: struct slot
 * Returns: 0 or error
 ****************************************************************************/
/* [한국어]
 * ibmphp_update_slot_info - 버스의 현재 속도를 핫플러그 코어 쪽에 반영한다
 *
 * @slot_cur: 대상 슬롯.
 * @return: 항상 0.
 *
 * 바로 위 상류 주석대로 핫플러그 코어의 자료구조를 갱신한다. 상류 주석이
 * "so we need it to not be static" 이라 밝히듯 ibmphp_hpc.c 의 폴링
 * 스레드도 부르기 때문에 static 이 아니다.
 *
 * get_max_bus_speed() 와 같은 변환을 하지만 대상이 다르다 — 저쪽은
 * "낼 수 있는 최대 속도"(max_bus_speed)를 슬롯의 능력에서 옮기고,
 * 이쪽은 "지금 도는 속도"(cur_bus_speed)를 bus_on 의 현재 값에서 옮긴다.
 *
 * 변환 규칙도 한 가지 다르다. 66MHz 에서 모드가 PCI-X 도 PCI 도 아니면
 * PCI_SPEED_UNKNOWN 으로 둔다 — get_cur_bus_info() 가 모드를 못 읽었을 때
 * 0xFF 를 넣어 두기 때문이다. 66MHz 갈래의 빈 else if 는 모드가 통상
 * PCI 일 때 값을 그대로 두라는 뜻을 명시적으로 적은 것이다.
 *
 * 끝의 상류 주석은 bus_names 갱신이 아직 할 일로 남아 있음을 적어 두었다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트와 폴링 커널 스레드 양쪽.
 *
 * 호출 체인:
 *   enable_slot() / ibmphp_do_disable_slot() / ibmphp_hpc.c 의 폴링 루프
 *     -> [이 함수]
 */
int ibmphp_update_slot_info(struct slot *slot_cur)
{
	struct pci_bus *bus = slot_cur->hotplug_slot.pci_slot->bus; /* [한국어] 값을 담을 코어 쪽 버스 구조체 */
	u8 bus_speed; /* [한국어] 버스가 지금 도는 속도 */
	u8 mode; /* [한국어] 버스가 지금 도는 모드 */

	bus_speed = slot_cur->bus_on->current_speed; /* [한국어] **"지금 도는 속도"** 를 가져온다. get_max_bus_speed() 가 옮기는 "낼 수 있는 최대 속도" 와는 다른 값이다 */
	mode = slot_cur->bus_on->current_bus_mode; /* [한국어] 모드도 함께 가져온다 */

	switch (bus_speed) { /* [한국어] 속도별로 코어 쪽 값을 정한다 */
		case BUS_SPEED_33: /* [한국어] 33MHz 는 PCI-X 가 없으므로 */
			break; /* [한국어] 그대로 둔다 */
		case BUS_SPEED_66: /* [한국어] 66MHz 는 통상 PCI 와 PCI-X 가 모두 있으므로 모드를 봐야 한다 */
			if (mode == BUS_MODE_PCIX) /* [한국어] PCI-X 이면 */
				bus_speed += 0x01; /* [한국어] 한 칸 올린다 */
			else if (mode == BUS_MODE_PCI) /* [한국어] 통상 PCI 이면 */
				; /* [한국어] 값을 그대로 둔다는 뜻을 빈 문장으로 명시했다 */
			else
				bus_speed = PCI_SPEED_UNKNOWN; /* [한국어] 모른다고 표시한다 */
			break; /* [한국어] 갈래를 벗어난다 */
		case BUS_SPEED_100: /* [한국어] 100MHz 와 */
		case BUS_SPEED_133: /* [한국어] 133MHz 는 PCI-X 에만 있는 속도이므로 */
			bus_speed += 0x01; /* [한국어] 조건 없이 한 칸 올린다 */
			break; /* [한국어] 갈래를 벗어난다 */
		default: /* [한국어] 아는 속도가 아니면 */
			bus_speed = PCI_SPEED_UNKNOWN; /* [한국어] 모른다고 표시한다. get_max_bus_speed() 가 -ENODEV 로 거절하는 것과 달리 이쪽은 값만 바꾸고 계속한다 */
	}

	bus->cur_bus_speed = bus_speed; /* [한국어] 코어 쪽 버스 구조체에 담는다. sysfs 의 cur_bus_speed 로 보이는 값이다 */
	// To do: bus_names

	return 0; /* [한국어] 실패할 일이 없으므로 늘 성공이다 */
}


/******************************************************************************
 * This function will return the pci_func, given bus and devfunc, or NULL.  It
 * is called from visit routines
 ******************************************************************************/

/* [한국어]
 * ibm_slot_find - 버스·장치·함수 번호로 pci_func 를 찾는다
 *
 * @busno:    찾을 버스 번호.
 * @device:   찾을 장치 번호.
 * @function: 찾을 함수 번호.
 * @return: 맞는 struct pci_func. 없으면 NULL.
 *
 * 바로 위 상류 주석대로 버스와 devfunc 가 주어졌을 때 pci_func 를
 * 돌려준다. 이중 루프인 이유는 pci_func 가 슬롯마다 하나가 아니라
 * 연결 목록이기 때문이다 — 다중 함수 카드나 브리지 뒤 장치까지
 * ibmphp_pci.c 가 목록으로 이어 붙여 놓는다.
 *
 * enable_slot() 이 카드를 다 설정한 뒤 그 카드의 함수를 0 부터 훑으며
 * 이 함수를 부르고, 아직 struct pci_dev 가 없는 함수만 골라 커널 열거를
 * 맡긴다. 그래서 이 함수의 반환값이 NULL 이 되는 시점이 그 루프의
 * 종료 조건이 된다.
 *
 * [관찰] 상류 주석은 "visit routines" 에서 불린다고 적었으나, 이 트리에서
 * 부르는 곳은 enable_slot() 한 곳뿐이다(전수 grep 확인).
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 잠금 안에서 불린다.
 *
 * 호출 체인:  enable_slot() -> [이 함수] -> list_for_each_entry()
 */
static struct pci_func *ibm_slot_find(u8 busno, u8 device, u8 function)
{
	struct pci_func *func_cur; /* [한국어] 함수 목록을 훑을 반복 포인터 */
	struct slot *slot_cur; /* [한국어] 슬롯 목록을 훑을 반복 포인터 */
	list_for_each_entry(slot_cur, &ibmphp_slot_head, ibm_slot_list) { /* [한국어] 등록된 슬롯을 하나씩 본다 */
		if (slot_cur->func) { /* [한국어] 그 슬롯에 함수 목록이 매달려 있으면 */
			func_cur = slot_cur->func; /* [한국어] 첫 함수부터 */
			while (func_cur) { /* [한국어] 목록 끝까지 훑는다. **이중 루프인 이유**는 다중 함수 카드와 브리지 뒤 장치까지 ibmphp_pci.c 가 하나의 목록으로 이어 붙이기 때문이다 */
				if ((func_cur->busno == busno) && /* [한국어] 버스 번호가 같고 */
						(func_cur->device == device) && /* [한국어] 장치 번호가 같고 */
						(func_cur->function == function)) /* [한국어] 함수 번호도 같으면 */
					return func_cur; /* [한국어] 그 함수를 돌려준다 */
				func_cur = func_cur->next; /* [한국어] 다음 함수로 */
			}
		}
	}
	return NULL; /* [한국어] 끝까지 못 찾았으면 그런 함수가 없다. enable_slot() 의 do-while 은 이 NULL 을 종료 조건으로 쓴다 */
}

/*************************************************************
 * This routine frees up memory used by struct slot, including
 * the pointers to pci_func, bus, hotplug_slot, controller,
 * and deregistering from the hotplug core
 *************************************************************/
/* [한국어]
 * free_slots - 모든 슬롯을 코어에서 빼고 슬롯 구조체를 버린다
 *
 * 바로 위 상류 주석대로 struct slot 이 쓰던 메모리를 되돌리고, 그것이
 * 가리키던 pci_func / bus / hotplug_slot / controller 포인터를 정리하며,
 * 핫플러그 코어에서 등록을 해제한다.
 *
 * 순서에 뜻이 있다.
 *   1. pci_hp_del() 로 코어에서 먼저 뺀다 — 그래야 이후 사용자가 sysfs 로
 *      이 슬롯을 조작할 수 없다.
 *   2. ctrl 과 bus_on 포인터를 끊는다. 두 구조체 자체는 ibmphp_ebda.c 의
 *      해제 함수들이 따로 버리므로 여기서는 참조만 끊는다.
 *   3. ibmphp_unconfigure_card(&slot_cur, -1) 을 부른다. **마지막 인자가
 *      -1 인 것이 핵심**으로, 바로 위 상류 주석이 그 이유를 밝힌다 —
 *      자원을 실제로 걷어내지는 말라는 뜻이다. ibmphp_free_resources() 가
 *      장부를 통째로 버릴 것이므로 여기서 하나씩 지우면 두 번 일하게 된다.
 *   4. pci_hp_destroy() 로 코어 쪽 구조체를 허물고 슬롯을 버린다.
 *
 * list_for_each_entry_safe 를 쓰는 이유는 순회 중에 항목을 해제하기
 * 때문이다.
 *
 * 실행 컨텍스트: 모듈 해제와 초기화 실패 경로, 둘 다 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   ibmphp_unload() -> [이 함수] -> pci_hp_del() -> ibmphp_unconfigure_card()
 *                                 -> pci_hp_destroy() -> kfree()
 */
static void free_slots(void)
{
	struct slot *slot_cur, *next; /* [한국어] slot_cur 는 지금 버릴 슬롯, next 는 미리 붙잡아 둘 다음 슬롯 */

	debug("%s -- enter\n", __func__); /* [한국어] 들어왔음을 남긴다 */

	list_for_each_entry_safe(slot_cur, next, &ibmphp_slot_head, /* [한국어] **해제하면서 순회하므로 safe 판을 쓴다** */
				 ibm_slot_list) { /* [한국어] 목록 연결 필드 이름 */
		pci_hp_del(&slot_cur->hotplug_slot); /* [한국어] **먼저 핫플러그 코어에서 뺀다** — 그래야 이후 사용자가 sysfs 로 이 슬롯을 조작할 수 없다 */
		slot_cur->ctrl = NULL; /* [한국어] 컨트롤러 참조만 끊는다. 컨트롤러 구조체 자체는 ibmphp_free_ebda_hpc_queue() 가 버린다 */
		slot_cur->bus_on = NULL; /* [한국어] 버스 정보 참조도 끊는다. 그 구조체는 ibmphp_free_bus_info_queue() 가 버린다 */

		/*
		 * We don't want to actually remove the resources,
		 * since ibmphp_free_resources() will do just that.
		 */
		ibmphp_unconfigure_card(&slot_cur, -1); /* [한국어] **마지막 인자 -1 이 핵심이다.** 바로 위 상류 주석대로 자원을 실제로 걷어내지 말라는 뜻으로, ibmphp_free_resources() 가 장부를 통째로 버릴 것이기 때문이다 */

		pci_hp_destroy(&slot_cur->hotplug_slot); /* [한국어] 코어 쪽 구조체를 허문다 */
		kfree(slot_cur); /* [한국어] 슬롯 구조체를 버린다 */
	}
	debug("%s -- exit\n", __func__); /* [한국어] 끝났음을 남긴다 */
}

/* [한국어]
 * ibm_unconfigure_device - PCI 코어에서 이 슬롯의 장치들을 걷어낸다
 *
 * @func: 걷어낼 카드의 함수 구조체. busno 와 device 만 쓴다.
 *
 * 카드를 뽑기 전에 커널 쪽 표현을 먼저 없애는 자리다. 이 함수가 끝나야
 * 그 장치의 드라이버가 언바인드되고 struct pci_dev 가 사라진다.
 *
 * **함수 번호 8개를 모두 훑는다.** func 하나가 아니라 (device << 3) | j
 * 로 devfn 을 만들어 0~7 을 다 도는 이유는, 다중 함수 카드가 꽂혀 있으면
 * 함수 여럿이 같은 물리 슬롯에 있기 때문이다. 슬롯을 뽑으면 그 함수가
 * 전부 사라지므로 하나씩 짚어 없앤다.
 *
 * pci_get_domain_bus_and_slot() 은 찾은 장치의 참조 수를 올려 주므로
 * pci_stop_and_remove_bus_device() 뒤에 pci_dev_put() 으로 되돌린다.
 * 마지막의 pci_dev_put(func->dev) 는 ibm_configure_device() 가 잡아 둔
 * 참조를 푸는 것이다.
 *
 * pci_lock_rescan_remove() 로 감싸는 이유는 PCI 코어의 장치 추가·제거가
 * 그 잠금으로 직렬화되기 때문이다. 이 드라이버 자신의 잠금
 * (ibmphp_lock_operations)과는 다른 것이며, 둘 다 잡힌 상태로 돈다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 드라이버 언바인드가 일어나므로
 * 시간이 걸릴 수 있다.
 *
 * 호출 체인:
 *   ibmphp_do_disable_slot() -> [이 함수]
 *     -> pci_lock_rescan_remove() -> pci_get_domain_bus_and_slot()
 *     -> pci_stop_and_remove_bus_device()
 */
static void ibm_unconfigure_device(struct pci_func *func)
{
	struct pci_dev *temp; /* [한국어] 찾아낸 struct pci_dev 를 잠시 담을 자리 */
	u8 j; /* [한국어] 함수 번호 루프 인덱스 */

	debug("inside %s\n", __func__); /* [한국어] 들어왔음을 남긴다 */
	debug("func->device = %x, func->function = %x\n", /* [한국어] 어느 장치인지 남긴다 */
					func->device, func->function); /* [한국어] 장치와 함수 번호 */
	debug("func->device << 3 | 0x0  = %x\n", func->device << 3 | 0x0); /* [한국어] devfn 으로 만든 값도 남긴다 — 장치 번호를 3비트 왼쪽으로 밀면 함수 0 의 devfn 이 된다 */

	pci_lock_rescan_remove(); /* [한국어] **PCI 코어의 장치 추가·제거와 직렬화한다.** 이 드라이버 자신의 잠금과는 다른 것이며, 둘 다 잡힌 상태로 돈다 */

	for (j = 0; j < 0x08; j++) { /* [한국어] **함수 번호 8개를 모두 훑는다.** 다중 함수 카드가 꽂혀 있으면 함수 여럿이 같은 물리 슬롯에 있고, 슬롯을 뽑으면 그 함수가 전부 사라지기 때문이다 */
		temp = pci_get_domain_bus_and_slot(0, func->busno, /* [한국어] 0번 도메인에서 그 버스·devfn 의 장치를 찾는다 */
						   (func->device << 3) | j); /* [한국어] 장치 번호를 3비트 밀고 함수 번호를 더해 devfn 을 만든다 */
		if (temp) { /* [한국어] 찾았으면 */
			pci_stop_and_remove_bus_device(temp); /* [한국어] **드라이버를 떼어내고 struct pci_dev 를 코어에서 없앤다.** 이것이 끝나야 자원을 지워도 안전하다 */
			pci_dev_put(temp); /* [한국어] 찾을 때 올라간 참조 수를 되돌린다 */
		}
	}

	pci_dev_put(func->dev); /* [한국어] ibm_configure_device() 가 잡아 둔 참조를 푼다 */

	pci_unlock_rescan_remove(); /* [한국어] 코어 쪽 잠금을 푼다 */
}

/*
 * The following function is to fix kernel bug regarding
 * getting bus entries, here we manually add those primary
 * bus entries to kernel bus structure whenever apply
 */
/* [한국어]
 * bus_structure_fixup - 커널이 모르는 버스를 억지로 열거하게 만든다
 *
 * @busno: 문제의 버스 번호.
 * @return: 0 이면 이 함수가 실제로 버스를 훑었다. 1 이면 할 일이 없었거나
 *          (이미 커널이 아는 버스이거나 이 드라이버가 모르는 버스)
 *          메모리를 잡지 못했다.
 *
 * 바로 위 상류 주석이 스스로 밝힌다 — 커널의 버스 항목 처리에 있는 버그를
 * 고치려는 것이며, 필요할 때 1차 버스 항목을 손으로 커널 쪽 버스 구조에
 * 넣어 준다. 그 버그가 무엇이었는지는 이 트리에서 확인 못 함.
 *
 * 먼저 두 가지를 거른다. 커널이 이미 아는 버스이면(pci_find_bus 가 찾으면)
 * 할 일이 없고, 이 드라이버가 모르는 버스이면(EBDA 표에 없으면) 건드릴
 * 근거가 없다. 둘 중 하나라도 걸리면 1 을 돌려준다.
 *
 * 그 다음이 특이한 부분이다. **struct pci_bus 와 struct pci_dev 를 힙에
 * 잡아 최소한만 꾸민다** — 버스에는 번호와 config 접근 함수만, 장치에는
 * 그 버스를 가리키는 포인터만 넣는다. 그리고 devfn 을 8씩 올리며(즉 각
 * 장치의 0번 함수만) 벤더 ID 를 읽어 본다. 응답이 하나라도 있으면 그
 * 버스에 무언가 있다는 뜻이므로 pci_scan_bus() 로 진짜 열거를 시킨다.
 * 찾자마자 멈추고, 꾸며 둔 두 구조체는 반드시 버린다.
 *
 * 즉 이 두 구조체는 진짜 버스·장치가 아니라 pci_read_config_word() 를
 * 부르기 위한 매개체다. ibmphp_pci.c 의 ibmphp_pci_bus 와 같은 방식이며,
 * 차이는 그쪽이 전역 하나를 계속 쓰는 반면 이쪽은 그때그때 만들어
 * 버린다는 점이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 호출자가 pci_lock_rescan_remove() 를
 * 이미 잡은 상태다.
 *
 * 호출 체인:
 *   ibm_configure_device() -> [이 함수] -> pci_find_bus()
 *     -> ibmphp_find_same_bus_num() -> pci_read_config_word()
 *     -> pci_scan_bus() -> pci_bus_add_devices()
 */
static u8 bus_structure_fixup(u8 busno)
{
	struct pci_bus *bus, *b; /* [한국어] bus 는 꾸며 놓을 임시 버스, b 는 pci_scan_bus() 가 돌려줄 진짜 버스 */
	struct pci_dev *dev; /* [한국어] 꾸며 놓을 임시 장치 */
	u16 l; /* [한국어] 읽어 본 벤더 ID */

	if (pci_find_bus(0, busno) || !(ibmphp_find_same_bus_num(busno))) /* [한국어] **커널이 이미 아는 버스이거나, 이 드라이버가 모르는 버스이면** 손댈 이유가 없다 */
		return 1; /* [한국어] 할 일이 없다고 알린다 */

	bus = kmalloc_obj(*bus); /* [한국어] 임시 버스 구조체를 잡는다. kzalloc 이 아니라 kmalloc 이라 내용은 쓰레기 값이다 — 아래에서 쓸 두 필드만 채운다 */
	if (!bus) /* [한국어] 못 잡으면 */
		return 1; /* [한국어] 할 일을 못 했다고 알린다 */

	dev = kmalloc_obj(*dev); /* [한국어] 임시 장치 구조체를 잡는다 */
	if (!dev) { /* [한국어] 못 잡으면 */
		kfree(bus); /* [한국어] 앞서 잡은 버스를 버리고 */
		return 1; /* [한국어] 할 일을 못 했다고 알린다 */
	}

	bus->number = busno; /* [한국어] 버스 번호를 넣는다 */
	bus->ops = ibmphp_pci_bus->ops; /* [한국어] **config 접근 함수를 껍데기 버스에서 빌려 온다.** 이 두 필드만 있으면 pci_read_config_word() 가 돈다 */
	dev->bus = bus; /* [한국어] 장치가 그 버스에 붙어 있는 것처럼 꾸민다 */
	for (dev->devfn = 0; dev->devfn < 256; dev->devfn += 8) { /* [한국어] **devfn 을 8씩 올린다** — 각 장치의 0번 함수만 본다는 뜻이다. 장치가 하나라도 있으면 그 함수는 반드시 응답한다 */
		if (!pci_read_config_word(dev, PCI_VENDOR_ID, &l) && /* [한국어] 벤더 ID 를 읽어 보고 */
					(l != 0x0000) && (l != 0xffff)) { /* [한국어] 응답이 있으면(0x0000 도 0xffff 도 아니면) 이 버스에 무언가 있다는 뜻이다 */
			debug("%s - Inside bus_structure_fixup()\n", /* [한국어] 찾았음을 남긴다 */
							__func__);
			b = pci_scan_bus(busno, ibmphp_pci_bus->ops, NULL); /* [한국어] **진짜 열거를 시킨다.** 여기서 커널이 이 버스를 알게 된다 */
			if (!b) /* [한국어] 열거에 실패했으면 */
				continue; /* [한국어] 다음 장치 자리를 계속 본다 */

			pci_bus_add_devices(b); /* [한국어] 열거된 장치들에 드라이버를 붙인다 */
			break; /* [한국어] 하나 찾았으면 더 볼 필요가 없다 */
		}
	}

	kfree(dev); /* [한국어] 꾸며 둔 장치를 버린다 — 진짜 장치가 아니라 config 접근용 매개체였다 */
	kfree(bus); /* [한국어] 꾸며 둔 버스도 버린다 */

	return 0; /* [한국어] 실제로 버스를 훑었음을 알린다. 호출자는 이 0 을 보고 flag 를 세워 같은 버스를 두 번 훑지 않는다 */
}

/* [한국어]
 * ibm_configure_device - PCI 코어에게 이 장치를 열거하고 드라이버를 붙이라고 알린다
 *
 * @func: 설정이 끝난 카드의 함수 구조체.
 * @return: 항상 0. 실패해도 0 을 돌려준다.
 *
 * ibmphp_pci.c 가 BAR 와 브리지 창을 다 채운 뒤, 그 장치를 커널이 알게
 * 만드는 자리다. 이 함수가 끝나야 struct pci_dev 가 생기고 드라이버가
 * 붙는다. 순서를 뒤집으면 코어가 자원 없는 장치를 보게 된다.
 *
 * 하는 일이 셋이다.
 *   1. bus_structure_fixup() 으로 버스가 커널에 있는지 확인한다. 그 함수가
 *      실제로 열거를 했으면(0 을 돌려주면) flag 를 세운다. 지역 변수
 *      선언의 상류 주석이 그 이유를 밝힌다 — 브리지 장치에서 같은 버스를
 *      두 번 훑지 않으려는 것이다.
 *   2. 그 장치의 struct pci_dev 를 찾는다. 없으면 pci_scan_slot() 으로
 *      그 슬롯만 훑고 pci_bus_add_devices() 로 드라이버를 붙인 뒤 다시
 *      찾는다. 그래도 없으면 오류를 남기고 나간다.
 *   3. 찾은 장치가 브리지이고 1번에서 이미 훑지 않았으면
 *      pci_hp_add_bridge() 로 브리지 뒤 버스 번호를 코어가 정하게 하고,
 *      그 뒤 버스의 장치들에 드라이버를 붙인다.
 *
 * **여기서 브리지 뒤를 코어에게 맡기는 것이 ibmphp_pci.c 와 대비된다.**
 * 그쪽은 자원 배정을 위해 브리지 뒤를 직접 훑었지만, 커널 쪽 표현을
 * 만드는 일은 코어에게 넘긴다.
 *
 * [관찰] 세 실패 경로가 모두 out 라벨로 가고 거기서 0 을 돌려준다.
 * 호출자인 enable_slot() 은 이 함수의 반환값을 보지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. pci_lock_rescan_remove() 로 감싸
 * PCI 코어의 장치 추가와 직렬화한다.
 *
 * 호출 체인:
 *   enable_slot() -> [이 함수] -> bus_structure_fixup() -> pci_scan_slot()
 *     -> pci_bus_add_devices() -> pci_hp_add_bridge()
 */
static int ibm_configure_device(struct pci_func *func)
{
	struct pci_bus *child; /* [한국어] 브리지 뒤 버스를 담을 자리 */
	int num; /* [한국어] pci_scan_slot() 이 찾은 함수 개수 */
	/* [한국어] 버스를 이미 훑었는지 표시하는 값. 옆의 상류 주석이 이유를 밝힌다 —
	 * 같은 버스를 두 번 훑지 않으려는 것이며 특히 브리지 장치에서 문제가 된다.
	 * bus_structure_fixup() 이 실제로 열거를 했으면(0 을 돌려주면) 1 로 선다. */
	int flag = 0;	/* this is to make sure we don't double scan the bus,
					for bridged devices primarily */

	pci_lock_rescan_remove(); /* [한국어] **PCI 코어의 장치 추가·제거와 직렬화한다** */

	if (!(bus_structure_fixup(func->busno))) /* [한국어] 버스가 커널에 있는지 확인하고, 이 호출이 실제로 열거를 했으면 */
		flag = 1; /* [한국어] 표시해 둔다 — 아래에서 같은 버스를 두 번 훑지 않기 위해서다 */
	if (func->dev == NULL) /* [한국어] 아직 struct pci_dev 를 못 찾았으면 */
		func->dev = pci_get_domain_bus_and_slot(0, func->busno, /* [한국어] 0번 도메인에서 그 버스·devfn 의 장치를 찾는다 */
				PCI_DEVFN(func->device, func->function)); /* [한국어] 장치·함수 번호로 devfn 을 만든다 */

	if (func->dev == NULL) { /* [한국어] 그래도 못 찾았으면 아직 커널이 이 장치를 모르는 것이다 */
		struct pci_bus *bus = pci_find_bus(0, func->busno); /* [한국어] 그 버스를 찾는다 */
		if (!bus) /* [한국어] 버스조차 없으면 */
			goto out; /* [한국어] 잠금을 푸는 공통 출구로 간다 */

		num = pci_scan_slot(bus, /* [한국어] **그 슬롯 하나만 훑는다.** 버스 전체가 아니라 이 devfn 만 보는 것이라, 다른 슬롯의 카드를 건드리지 않는다 */
				PCI_DEVFN(func->device, func->function)); /* [한국어] 장치·함수 번호로 devfn 을 만든다 */
		if (num) /* [한국어] 함수를 하나라도 찾았으면 */
			pci_bus_add_devices(bus); /* [한국어] 찾은 장치들에 드라이버를 붙인다 */

		func->dev = pci_get_domain_bus_and_slot(0, func->busno, /* [한국어] 이제 struct pci_dev 가 생겼을 테니 다시 찾는다 */
				PCI_DEVFN(func->device, func->function)); /* [한국어] 같은 devfn 으로 */
		if (func->dev == NULL) { /* [한국어] 그래도 없으면 무언가 잘못된 것이다 */
			err("ERROR... : pci_dev still NULL\n"); /* [한국어] 그 사실을 알리고 */
			goto out; /* [한국어] 잠금을 푸는 공통 출구로 간다 */
		}
	}
	if (!(flag) && (func->dev->hdr_type == PCI_HEADER_TYPE_BRIDGE)) { /* [한국어] **앞에서 버스를 통째로 훑지 않았고, 이 장치가 브리지이면** */
		pci_hp_add_bridge(func->dev); /* [한국어] **브리지 뒤 버스 번호를 코어가 정하게 한다.** ibmphp_pci.c 가 자원 배정을 위해 브리지 뒤를 직접 훑은 것과 달리, 커널 쪽 표현을 만드는 일은 코어에게 넘긴다 */
		child = func->dev->subordinate; /* [한국어] 코어가 만든 2차 버스를 꺼내 */
		if (child) /* [한국어] 있으면 */
			pci_bus_add_devices(child); /* [한국어] 그 버스의 장치들에도 드라이버를 붙인다 */
	}

 out: /* [한국어] **공통 출구** — 세 실패 경로가 모두 여기로 온다 */
	pci_unlock_rescan_remove(); /* [한국어] 코어 쪽 잠금을 푼다 */
	return 0; /* [한국어] **실패했어도 0 을 돌려준다.** 호출자인 enable_slot() 은 이 반환값을 보지 않는다 */
}

/*******************************************************
 * Returns whether the bus is empty or not
 *******************************************************/
/* [한국어]
 * is_bus_empty - 이 슬롯이 붙은 버스에 다른 카드가 하나도 없는지 본다
 *
 * @slot_cur: 기준이 되는 슬롯. 자기 자신은 세지 않는다.
 * @return: 1 이면 버스가 비었다. 0 이면 다른 카드가 있거나 확인할 수 없다.
 *
 * 바로 위 상류 주석대로 버스가 비었는지를 돌려준다. set_bus() 가
 * 버스 속도를 바꿔도 되는지 판단하는 유일한 근거다 — PCI 버스는 한 버스에
 * 하나의 속도만 있을 수 있으므로, 다른 카드가 돌고 있으면 속도를 바꿀 수
 * 없다.
 *
 * bus_on 이 알려 주는 슬롯 번호 구간(slot_min ~ slot_max)을 전부 훑되
 * 자기 자신은 건너뛴다. 각 슬롯에 대해 상태를 새로 읽고, **카드가 꽂혀
 * 있으면서 전원도 들어와 있으면** 비어 있지 않다고 본다. 둘 다여야 하는
 * 이유는 카드가 꽂혀만 있고 꺼져 있으면 버스를 쓰지 않기 때문이다.
 *
 * 슬롯을 못 찾거나 상태를 못 읽어도 0(비어 있지 않음)을 돌려준다 —
 * 확인할 수 없으면 안전한 쪽으로 판단하는 것이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 호출자가 잠금 안에서 부른다.
 *
 * 호출 체인:
 *   set_bus() -> [이 함수] -> ibmphp_get_slot_from_physical_num()
 *                          -> slot_update()
 */
static int is_bus_empty(struct slot *slot_cur)
{
	int rc; /* [한국어] slot_update() 의 결과 */
	struct slot *tmp_slot; /* [한국어] 훑는 중인 다른 슬롯 */
	u8 i = slot_cur->bus_on->slot_min; /* [한국어] 이 버스가 담당하는 슬롯 번호 구간의 시작 */

	while (i <= slot_cur->bus_on->slot_max) { /* [한국어] 구간의 끝까지 훑는다 */
		if (i == slot_cur->number) { /* [한국어] 자기 자신이면 */
			i++; /* [한국어] 다음 번호로 넘어가고 */
			continue; /* [한국어] 세지 않는다 */
		}
		tmp_slot = ibmphp_get_slot_from_physical_num(i); /* [한국어] 그 번호의 슬롯을 찾는다 */
		if (!tmp_slot) /* [한국어] 없으면 확인할 수 없다는 뜻이므로 */
			return 0; /* [한국어] **안전한 쪽으로 판단해** 비어 있지 않다고 답한다 */
		rc = slot_update(&tmp_slot); /* [한국어] 그 슬롯의 상태를 새로 읽는다 */
		if (rc) /* [한국어] 읽기가 실패해도 */
			return 0; /* [한국어] 같은 이유로 비어 있지 않다고 답한다 */
		if (SLOT_PRESENT(tmp_slot->status) && /* [한국어] **카드가 꽂혀 있으면서** */
					SLOT_PWRGD(tmp_slot->status)) /* [한국어] 전원도 들어와 있으면 그 슬롯이 버스를 쓰고 있는 것이다. 둘 다여야 하는 이유는 꽂혀만 있고 꺼져 있으면 버스를 쓰지 않기 때문이다 */
			return 0; /* [한국어] 비어 있지 않다고 답한다 */
		i++; /* [한국어] 다음 슬롯 번호로 */
	}
	return 1; /* [한국어] 구간을 다 훑도록 아무도 없었으면 버스가 비었다 */
}

/***********************************************************
 * If the HPC permits and the bus currently empty, tries to set the
 * bus speed and mode at the maximum card and bus capability
 * Parameters: slot
 * Returns: bus is set (0) or error code
 ***********************************************************/
/* [한국어]
 * set_bus - 버스가 비어 있으면 속도와 모드를 카드에 맞춰 다시 정한다
 *
 * @slot_cur: 카드를 꽂으려는 슬롯.
 * @return: 0 이면 성공(속도를 바꿨거나, 바꿀 필요·여지가 없었거나).
 *          카드나 버스의 속도가 아는 값이 아니면 -ENODEV, 명령 전달이
 *          실패하면 그 코드, 컨트롤러가 명령을 못 마쳤으면 -EIO.
 *
 * 바로 위 상류 주석대로 컨트롤러가 허용하고 버스가 비어 있을 때만, 카드와
 * 버스가 함께 낼 수 있는 최대 속도·모드로 맞춘다. 두 조건이 함께여야
 * 하는 이유는 앞의 is_bus_empty() 설명과 같다.
 *
 * 정하는 방식은 **카드가 요구하는 속도와 슬롯이 지원하는 속도의 작은 쪽**을
 * 고르는 것이다. 바깥 switch 가 카드 쪽(ext_status 의 SLOT_SPEED),
 * 안쪽 갈래가 슬롯 쪽(supported_speed)을 본다.
 *   카드가 33MHz  — 통상 33 으로 고정.
 *   카드가 66MHz  — 카드가 PCI-X 인지 먼저 보고, 슬롯도 PCI-X 를 지원하면
 *                   66 PCI-X, 모드 불일치가 없으면 통상 66, 그 밖은 33.
 *                   중간 갈래의 상류 주석이 그 판단을 밝힌다.
 *   카드가 133MHz — 슬롯이 지원하는 만큼만 올린다(33/66/100/133).
 * 133MHz 갈래에는 상류 주석이 밝히는 우회가 하나 있다 — CIOBX 칩의 버그
 * 때문에, 그 칩이 있으면 100 PCI-X 를 먼저 한 번 보내고 나서 133 을 보낸다.
 * 그 칩은 함수 안의 정적 배열 ciobx 로 판별한다.
 *
 * 끝의 1초 대기에도 상류 주석이 붙어 있다 — x440 기종의 펌웨어가 고쳐지면
 * 필요 없어질 것이라는 내용이다. **이 대기는 버스가 비어 있지 않아 위
 * 블록을 통째로 건너뛴 경우에도 실행된다.**
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. msleep 으로 잠들며, 호출자가 잡아 둔
 * 잠금 안이라 그 1초 동안 다른 슬롯 조작이 모두 막힌다.
 *
 * 호출 체인:
 *   enable_slot() -> [이 함수] -> is_bus_empty() -> slot_update()
 *                               -> pci_dev_present() -> ibmphp_hpc_writeslot()
 */
static int set_bus(struct slot *slot_cur)
{
	int rc; /* [한국어] slot_update() 의 결과 */
	u8 speed; /* [한국어] 카드가 요구하는 속도 */
	u8 cmd = 0x0; /* [한국어] 컨트롤러에 보낼 버스 설정 명령 */
	int retval; /* [한국어] 명령 전달 결과 */
	static const struct pci_device_id ciobx[] = { /* [한국어] **CIOBX 칩을 판별하기 위한 장치 표.** 아래 133MHz 갈래에서만 쓴다 */
		{ PCI_DEVICE(PCI_VENDOR_ID_SERVERWORKS, 0x0101) }, /* [한국어] ServerWorks 벤더의 장치 0x0101 */
		{ }, /* [한국어] 표의 끝을 알리는 빈 항목 */
	};

	debug("%s - entry slot # %d\n", __func__, slot_cur->number); /* [한국어] 어느 슬롯인지 남긴다 */
	if (SET_BUS_STATUS(slot_cur->ctrl) && is_bus_empty(slot_cur)) { /* [한국어] **컨트롤러가 버스 설정을 지원하고, 그 버스가 비어 있을 때만** 속도를 건드린다. PCI 버스는 한 버스에 하나의 속도만 있을 수 있어 다른 카드가 돌면 바꿀 수 없다 */
		rc = slot_update(&slot_cur); /* [한국어] 상태를 새로 읽는다 — 카드의 속도 요구를 읽어야 하기 때문이다 */
		if (rc) /* [한국어] 실패하면 */
			return rc; /* [한국어] 그 코드를 돌려준다 */
		speed = SLOT_SPEED(slot_cur->ext_status); /* [한국어] **ext_status 에서 카드가 요구하는 속도를 꺼낸다**(33/66/133) */
		debug("ext_status = %x, speed = %x\n", slot_cur->ext_status, speed); /* [한국어] 읽은 값을 남긴다 */
		switch (speed) { /* [한국어] 카드 쪽 요구에 따라 갈린다. 안쪽에서 다시 슬롯 쪽 능력을 보아 작은 쪽을 고른다 */
		case HPC_SLOT_SPEED_33: /* [한국어] 카드가 33MHz 면 */
			cmd = HPC_BUS_33CONVMODE; /* [한국어] 통상 33 으로 고정한다 — 33MHz 에는 PCI-X 가 없다 */
			break; /* [한국어] 갈래를 벗어난다 */
		case HPC_SLOT_SPEED_66: /* [한국어] 카드가 66MHz 면 */
			if (SLOT_PCIX(slot_cur->ext_status)) { /* [한국어] **카드가 PCI-X 인지 먼저 본다** */
				if ((slot_cur->supported_speed >= BUS_SPEED_66) && /* [한국어] 슬롯도 66 이상을 지원하고 */
						(slot_cur->supported_bus_mode == BUS_MODE_PCIX)) /* [한국어] 슬롯의 모드도 PCI-X 이면 */
					cmd = HPC_BUS_66PCIXMODE; /* [한국어] 66MHz PCI-X 로 간다 */
				else if (!SLOT_BUS_MODE(slot_cur->ext_status)) /* [한국어] 그렇지 않은데 모드 불일치가 없으면 — 바로 아래 상류 주석이 그 판단을 밝힌다 */
					/* if max slot/bus capability is 66 pci
					and there's no bus mode mismatch, then
					the adapter supports 66 pci */
					cmd = HPC_BUS_66CONVMODE; /* [한국어] 통상 66 으로 간다 */
				else
					cmd = HPC_BUS_33CONVMODE; /* [한국어] 통상 33 으로 낮춘다 */
			} else {
				if (slot_cur->supported_speed >= BUS_SPEED_66) /* [한국어] 슬롯이 66 이상을 지원하면 */
					cmd = HPC_BUS_66CONVMODE; /* [한국어] 통상 66, */
				else
					cmd = HPC_BUS_33CONVMODE; /* [한국어] 통상 33 으로 간다 */
			}
			break; /* [한국어] 갈래를 벗어난다 */
		case HPC_SLOT_SPEED_133: /* [한국어] 카드가 133MHz 면 슬롯이 지원하는 만큼만 올린다 */
			switch (slot_cur->supported_speed) { /* [한국어] 슬롯 쪽 능력에 따라 갈린다 */
			case BUS_SPEED_33: /* [한국어] 슬롯이 33 까지면 */
				cmd = HPC_BUS_33CONVMODE; /* [한국어] 통상 33 */
				break; /* [한국어] 갈래를 벗어난다 */
			case BUS_SPEED_66: /* [한국어] 슬롯이 66 까지면 */
				if (slot_cur->supported_bus_mode == BUS_MODE_PCIX) /* [한국어] 모드가 PCI-X 인지 보아 */
					cmd = HPC_BUS_66PCIXMODE; /* [한국어] 66 PCI-X 또는 */
				else
					cmd = HPC_BUS_66CONVMODE; /* [한국어] 통상 66 으로 간다 */
				break; /* [한국어] 갈래를 벗어난다 */
			case BUS_SPEED_100: /* [한국어] 슬롯이 100 까지면 */
				cmd = HPC_BUS_100PCIXMODE; /* [한국어] 100 PCI-X — 이 속도는 PCI-X 에만 있다 */
				break; /* [한국어] 갈래를 벗어난다 */
			case BUS_SPEED_133: /* [한국어] 슬롯도 133 이면 */
				/* This is to take care of the bug in CIOBX chip */
				if (pci_dev_present(ciobx)) /* [한국어] **바로 위 상류 주석이 밝히는 CIOBX 칩 우회** — 그 칩이 있으면 */
					ibmphp_hpc_writeslot(slot_cur, /* [한국어] 100 PCI-X 를 한 번 먼저 보내고 */
							HPC_BUS_100PCIXMODE); /* [한국어] 그 다음에 아래 133 을 보낸다 */
				cmd = HPC_BUS_133PCIXMODE; /* [한국어] 133 PCI-X 로 간다 */
				break; /* [한국어] 갈래를 벗어난다 */
			default: /* [한국어] 슬롯의 지원 속도가 아는 값이 아니면 */
				err("Wrong bus speed\n"); /* [한국어] 그 사실을 알리고 */
				return -ENODEV; /* [한국어] 장치 없음으로 돌아간다 */
			}
			break; /* [한국어] 갈래를 벗어난다 */
		default: /* [한국어] 카드의 요구 속도가 아는 값이 아니면 */
			err("wrong slot speed\n"); /* [한국어] 그 사실을 알리고 */
			return -ENODEV; /* [한국어] 장치 없음으로 돌아간다 */
		}
		debug("setting bus speed for slot %d, cmd %x\n", /* [한국어] 무엇을 보낼지 남긴다 */
						slot_cur->number, cmd); /* [한국어] 슬롯 번호와 명령 코드 */
		retval = ibmphp_hpc_writeslot(slot_cur, cmd); /* [한국어] **컨트롤러에 버스 속도·모드 설정 명령을 보낸다** */
		if (retval) { /* [한국어] 명령 전달 자체가 실패했으면 */
			err("setting bus speed failed\n"); /* [한국어] 그 사실을 알리고 */
			return retval; /* [한국어] 그 코드를 돌려준다 */
		}
		if (CTLR_RESULT(slot_cur->ctrl->status)) { /* [한국어] 컨트롤러가 명령을 성공으로 마치지 못했으면 */
			err("command not completed successfully in set_bus\n"); /* [한국어] 그 사실을 알리고 */
			return -EIO; /* [한국어] 입출력 오류로 돌아간다 */
		}
	}
	/* This is for x440, once Brandon fixes the firmware,
	will not need this delay */
	msleep(1000); /* [한국어] 바로 위 상류 주석이 대상을 밝히는 고정 대기 — x440 기종의 펌웨어가 고쳐지면 필요 없어질 것이라는 내용이다. **버스가 비어 있지 않아 위 블록을 통째로 건너뛴 경우에도 실행된다** */
	debug("%s -Exit\n", __func__); /* [한국어] 나가기 직전임을 남긴다 */
	return 0; /* [한국어] 여기까지 왔으면 성공 */
}

/* This routine checks the bus limitations that the slot is on from the BIOS.
 * This is used in deciding whether or not to power up the slot.
 * (electrical/spec limitations. For example, >1 133 MHz or >2 66 PCI cards on
 * same bus)
 * Parameters: slot
 * Returns: 0 = no limitations, -EINVAL = exceeded limitations on the bus
 */
/* [한국어]
 * check_limitations - 같은 버스의 카드 수가 전기적 한계 안인지 본다
 *
 * @slot_cur: 카드를 꽂으려는 슬롯.
 * @return: 0 이면 한계 안이다. 넘으면 -EINVAL, 슬롯을 못 찾으면 -ENODEV.
 *
 * 바로 위 상류 주석이 목적과 예를 밝힌다 — BIOS 가 알려 준 버스의 제약을
 * 보고 전원을 켜도 되는지 정하며, 같은 버스에 133MHz 카드가 둘 이상이거나
 * 66MHz PCI 카드가 셋 이상이면 안 된다는 식의 전기적·규격적 한계다.
 *
 * 먼저 같은 버스의 슬롯을 모두 훑어 **이미 쓰이고 있는 카드 수를 센다**.
 * 세는 조건은 "전원이 정상으로 들어와 있으면서 버스에 연결되어 있는" 것
 * 이다 — SLOT_CONNECT 매크로가 연결됨을 0 으로 돌려주므로 부정 연산이
 * 붙어 있다.
 *
 * 그 다음 지금 버스가 도는 속도에 맞는 한계값을 bus_on 에서 꺼낸다.
 * 그 값들(slots_at_33_conv 등)은 ibmphp_ebda.c 가 EBDA 표에서 읽어 채운
 * 것이므로, **한계의 근거는 결국 BIOS 가 적어 둔 값**이다.
 *
 * 마지막으로 지금 카드까지 더한 수가 한계를 넘는지 본다.
 *
 * [관찰] 세는 루프는 슬롯의 status 를 새로 읽지 않고 이미 담겨 있는 값을
 * 쓴다. 앞선 단계의 slot_update() 가 남긴 값이다.
 * [관찰] switch 에 default 가 없어, 속도가 아는 값이 아니면 limitation 이
 * 0 인 채로 남아 반드시 -EINVAL 이 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 호출자가 잠금 안에서 부른다.
 *
 * 호출 체인:
 *   enable_slot() -> [이 함수] -> ibmphp_get_slot_from_physical_num()
 *                               -> get_cur_bus_info()
 */
static int check_limitations(struct slot *slot_cur)
{
	u8 i; /* [한국어] 슬롯 번호 루프 인덱스 */
	struct slot *tmp_slot; /* [한국어] 훑는 중인 다른 슬롯 */
	u8 count = 0; /* [한국어] **이미 쓰이고 있는 카드 수** */
	u8 limitation = 0; /* [한국어] 이 속도에서 허용되는 카드 수. BIOS 가 EBDA 표에 적어 둔 값이다 */

	for (i = slot_cur->bus_on->slot_min; i <= slot_cur->bus_on->slot_max; i++) { /* [한국어] 이 버스가 담당하는 슬롯 번호 구간을 전부 훑는다. is_bus_empty() 와 달리 자기 자신도 센다 */
		tmp_slot = ibmphp_get_slot_from_physical_num(i); /* [한국어] 그 번호의 슬롯을 찾는다 */
		if (!tmp_slot) /* [한국어] 없으면 표가 어긋난 것이므로 */
			return -ENODEV; /* [한국어] 장치 없음으로 돌아간다 */
		if ((SLOT_PWRGD(tmp_slot->status)) && /* [한국어] **전원이 정상으로 들어와 있고** */
					!(SLOT_CONNECT(tmp_slot->status))) /* [한국어] 버스에 연결되어 있으면 — SLOT_CONNECT 가 연결됨을 0 으로 돌려주므로 부정 연산이 붙는다 */
			count++; /* [한국어] 쓰이고 있는 것으로 센다 */
	}
	get_cur_bus_info(&slot_cur); /* [한국어] **버스가 지금 도는 속도를 새로 읽는다.** 한계값이 속도별로 다르기 때문이다. 반환값은 보지 않는다 */
	switch (slot_cur->bus_on->current_speed) { /* [한국어] 그 속도에 맞는 한계값을 꺼낸다 */
	case BUS_SPEED_33: /* [한국어] 33MHz 로 돌면 */
		limitation = slot_cur->bus_on->slots_at_33_conv; /* [한국어] 33MHz 통상 PCI 슬롯 수를 한계로 쓴다 */
		break; /* [한국어] 갈래를 벗어난다 */
	case BUS_SPEED_66: /* [한국어] 66MHz 로 돌면 모드를 더 봐야 한다 */
		if (slot_cur->bus_on->current_bus_mode == BUS_MODE_PCIX) /* [한국어] PCI-X 모드이면 */
			limitation = slot_cur->bus_on->slots_at_66_pcix; /* [한국어] 66MHz PCI-X 슬롯 수를, */
		else
			limitation = slot_cur->bus_on->slots_at_66_conv; /* [한국어] 66MHz 통상 슬롯 수를 한계로 쓴다 */
		break; /* [한국어] 갈래를 벗어난다 */
	case BUS_SPEED_100: /* [한국어] 100MHz 로 돌면 */
		limitation = slot_cur->bus_on->slots_at_100_pcix; /* [한국어] 100MHz PCI-X 슬롯 수를 한계로 쓴다 */
		break; /* [한국어] 갈래를 벗어난다 */
	case BUS_SPEED_133: /* [한국어] 133MHz 로 돌면 */
		limitation = slot_cur->bus_on->slots_at_133_pcix; /* [한국어] 133MHz PCI-X 슬롯 수를 한계로 쓴다 */
		break; /* [한국어] 갈래를 벗어난다. **default 가 없어** 아는 속도가 아니면 한계가 0 인 채로 남아 반드시 거절된다 */
	}

	if ((count + 1) > limitation) /* [한국어] **지금 꽂으려는 카드까지 더한 수가 한계를 넘으면** */
		return -EINVAL; /* [한국어] 인자 오류로 거절한다. 바로 위 상류 주석이 예를 든다 — 같은 버스에 133MHz 카드 둘 이상, 66MHz PCI 카드 셋 이상이 안 되는 식이다 */
	return 0; /* [한국어] 한계 안이면 허용한다 */
}

/* [한국어]
 * print_card_capability - 카드가 어떤 속도까지 낼 수 있는지 사용자에게 알린다
 *
 * @slot_cur: 대상 슬롯. ext_status 만 쓴다.
 *
 * 속도나 모드가 맞지 않아 전원 켜기가 실패했을 때, 사용자가 무엇이
 * 어긋났는지 알 수 있도록 카드 쪽 능력을 찍는다. debug() 가 아니라
 * info() 라 디버그 스위치와 무관하게 늘 보인다.
 *
 * ext_status 의 CARD_INFO 자리 비트를 마스크로 떼어 네 가지로 가른다 —
 * 133MHz PCI-X, 66MHz PCI-X, 66MHz PCI, 그리고 나머지는 33MHz PCI 다.
 * if/else 사슬이라 마지막이 기본값 역할을 한다.
 *
 * 부르는 곳은 enable_slot() 의 두 실패 갈래다 — 속도 불일치와 모드
 * 불일치. 어느 쪽이든 사용자가 볼 것은 같은 정보다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  enable_slot() 실패 경로 -> [이 함수] -> info()
 */
static inline void print_card_capability(struct slot *slot_cur)
{
	info("capability of the card is "); /* [한국어] debug() 가 아니라 info() 라 디버그 스위치와 무관하게 늘 보인다 */
	if ((slot_cur->ext_status & CARD_INFO) == PCIX133) /* [한국어] ext_status 의 카드 정보 자리를 떼어 */
		info("   133 MHz PCI-X\n"); /* [한국어] 133MHz PCI-X 카드임을 알린다 */
	else if ((slot_cur->ext_status & CARD_INFO) == PCIX66) /* [한국어] 그 다음 값이면 */
		info("    66 MHz PCI-X\n"); /* [한국어] 66MHz PCI-X 카드 */
	else if ((slot_cur->ext_status & CARD_INFO) == PCI66) /* [한국어] 그 다음 값이면 */
		info("    66 MHz PCI\n"); /* [한국어] 66MHz 통상 PCI 카드 */
	else
		info("    33 MHz PCI\n"); /* [한국어] 33MHz 통상 PCI 카드로 본다 — if/else 사슬이라 마지막이 기본값 역할을 한다 */

}

/* This routine will power on the slot, configure the device(s) and find the
 * drivers for them.
 * Parameters: hotplug_slot
 * Returns: 0 or failure codes
 */
/* [한국어]
 * enable_slot - 슬롯에 전원을 넣고 카드를 설정해 드라이버까지 붙인다
 *
 * @hs: 핫플러그 코어 쪽 슬롯 구조체.
 * @return: 0 이면 성공. 단계마다 다른 오류 코드를 돌려준다.
 *
 * 바로 위 상류 주석대로 슬롯 전원을 켜고 장치들을 설정하고 드라이버를
 * 찾아 준다. 이 드라이버에서 가장 긴 함수이며, 카드 삽입의 전 과정을
 * 순서대로 밟는 상태 기계다.
 *
 * 단계는 다음과 같고, 각 단계가 실패하면 되돌려야 할 것이 달라 라벨이
 * 셋으로 나뉜다.
 *   1. validate(ENABLE)   — 지금 켜도 되는 상태인가.
 *   2. attn_LED_blink()   — 작업 중임을 LED 로 알린다.
 *   3. set_bus()          — 버스가 비었으면 속도·모드를 카드에 맞춘다.
 *   4. check_limitations()— 같은 버스의 카드 수가 한계 안인가.
 *   5. power_on()         — 전원을 넣고 3초 기다린다.
 *   6. 상태 재확인        — 전원 결함·속도 불일치·모드 불일치를 각각 가른다.
 *   7. pci_func 를 만들고 슬롯의 버스·장치·IRQ 를 베낀다.
 *   8. ibmphp_configure_card()  [ibmphp_pci.c] BAR 와 브리지 창을 채운다.
 *   9. 함수 0 부터 훑으며 ibm_configure_device() 로 커널 열거를 맡긴다.
 *  10. attn_off() 로 LED 를 끄고 상태를 코어에 반영한다.
 *
 * **5단계 전후로 실패 처리가 달라진다.** 전원을 넣기 전이면 LED 만
 * 되돌리면 되고(error_nopower), 넣은 뒤면 전원까지 꺼야 한다(error_power).
 * error_cont 는 두 경로가 만나는 자리로, 상태를 다시 읽어 코어에 알리는
 * 공통 마무리다. 성공 경로와 실패 경로가 모두 exit 에서 잠금을 푼다.
 *
 * 6단계의 갈래 나누기가 이 함수에서 사용자에게 가장 쓸모 있는 부분이다.
 * 전원 스위치는 켜졌는데 정상 전압이 안 들어왔으면 전원 결함,
 * SLOT_BUS_SPEED 가 서 있으면 속도 불일치, ext_status 의 SLOT_BUS_MODE 가
 * 서 있으면 모드 불일치로 나눠 알린다.
 *
 * 9단계의 do-while 은 ibm_slot_find() 가 NULL 을 돌려줄 때까지 함수 번호를
 * 올리며 돈다. 다중 함수 카드와 브리지 뒤 장치가 모두 하나의 pci_func
 * 목록에 이어져 있기 때문이다. 이미 struct pci_dev 가 있는 함수는
 * 건너뛴다.
 *
 * 8단계가 실패하면 ibmphp_unconfigure_card(&slot_cur, 1) 을 부르는데,
 * 그 자리의 상류 주석이 마지막 인자 1 의 뜻을 밝힌다 — 자원을 실제로
 * 해제할 필요는 없고 참조만 끊으면 된다는 것이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(sysfs 쓰기 또는 폴링 스레드가 감지한
 * 삽입). 함수 전체가 ibmphp_lock_operations() 로 감싸여 있어, 그 안의
 * msleep(3000)+msleep(1000) 동안 다른 슬롯 조작이 모두 막힌다.
 *
 * 호출 체인:
 *   핫플러그 코어 -> [이 함수] -> validate() -> set_bus()
 *     -> check_limitations() -> power_on() -> ibmphp_configure_card()
 *     -> ibm_slot_find() -> ibm_configure_device()
 *     -> ibmphp_update_slot_info()
 */
static int enable_slot(struct hotplug_slot *hs)
{
	int rc, i, rcpr; /* [한국어] rc 는 호출자에게 돌려줄 값, i 는 IRQ 배열 루프 인덱스, rcpr 은 실패 경로에서 되돌리기의 결과 */
	struct slot *slot_cur; /* [한국어] 조작할 슬롯 */
	u8 function; /* [한국어] 커널 열거를 맡길 때 훑는 함수 번호 */
	struct pci_func *tmp_func; /* [한국어] ibm_slot_find() 가 찾아 준 함수 */

	ibmphp_lock_operations(); /* [한국어] **함수 전체를 잠금으로 감싼다.** 안에 3초와 1초의 대기가 있어 그동안 다른 슬롯 조작이 모두 막힌다 */

	debug("ENABLING SLOT........\n"); /* [한국어] 시작을 남긴다 */
	slot_cur = to_slot(hs); /* [한국어] 핫플러그 코어 쪽 구조체에서 이 드라이버의 슬롯을 되찾는다 */

	rc = validate(slot_cur, ENABLE); /* [한국어] **1단계** — 지금 켜도 되는 상태인지 본다 */
	if (rc) { /* [한국어] 아니면 */
		err("validate function failed\n"); /* [한국어] 그 사실을 알리고 */
		goto error_nopower; /* [한국어] 전원을 넣기 전이므로 LED 만 되돌리는 경로로 간다 */
	}

	attn_LED_blink(slot_cur); /* [한국어] **2단계** — 작업 중임을 LED 깜빡임으로 알린다 */

	rc = set_bus(slot_cur); /* [한국어] **3단계** — 버스가 비었으면 속도·모드를 카드에 맞춘다 */
	if (rc) { /* [한국어] 실패하면 */
		err("was not able to set the bus\n"); /* [한국어] 그 사실을 알리고 */
		goto error_nopower; /* [한국어] 전원을 넣기 전이므로 LED 만 되돌리는 경로로 간다 */
	}

	/*-----------------debugging------------------------------*/
	get_cur_bus_info(&slot_cur); /* [한국어] 설정 뒤 실제 속도를 확인한다. 반환값은 보지 않는다 */
	debug("the current bus speed right after set_bus = %x\n", /* [한국어] 설정 직후의 버스 속도를 남긴다 */
					slot_cur->bus_on->current_speed); /* [한국어] 읽은 속도 */
	/*----------------------------------------------------------*/

	rc = check_limitations(slot_cur); /* [한국어] **4단계** — 같은 버스의 카드 수가 전기적 한계 안인지 본다 */
	if (rc) { /* [한국어] 넘으면 */
		err("Adding this card exceeds the limitations of this bus.\n"); /* [한국어] 한계를 넘었음을 알리고 */
		err("(i.e., >1 133MHz cards running on same bus, or >2 66 PCI cards running on same bus.\n"); /* [한국어] 어떤 한계인지 예를 들어 알리고 */
		err("Try hot-adding into another bus\n"); /* [한국어] 다른 버스에 꽂아 보라고 알린다 */
		rc = -EINVAL; /* [한국어] 인자 오류로 바꿔 담고 */
		goto error_nopower; /* [한국어] 전원을 넣기 전이므로 LED 만 되돌리는 경로로 간다 */
	}

	rc = power_on(slot_cur); /* [한국어] **5단계** — 전원을 넣고 3초 기다린다. 이 줄을 지나면 실패 처리가 달라진다 */

	if (rc) { /* [한국어] 전원 넣기가 실패했으면 */
		err("something wrong when powering up... please see below for details\n"); /* [한국어] 아래에 자세한 이유가 나온다고 알린다 */
		/* need to turn off before on, otherwise, blinking overwrites */
		attn_off(slot_cur); /* [한국어] 바로 위 상류 주석대로 먼저 끈다 — 깜빡임 상태에서 바로 켜면 덮어써지지 않기 때문이다 */
		attn_on(slot_cur); /* [한국어] 그 다음 켠다 */
		if (slot_update(&slot_cur)) { /* [한국어] 상태조차 못 읽으면 */
			attn_off(slot_cur); /* [한국어] 같은 방식으로 LED 를 되돌리고 */
			attn_on(slot_cur);
			rc = -ENODEV; /* [한국어] 장치 없음으로 담아 */
			goto exit; /* [한국어] 잠금을 푸는 공통 출구로 간다 */
		}
		/* Check to see the error of why it failed */
		if ((SLOT_POWER(slot_cur->status)) && /* [한국어] **전원 스위치는 켜졌는데** */
					!(SLOT_PWRGD(slot_cur->status))) /* [한국어] 정상 전압이 안 들어왔으면 */
			err("power fault occurred trying to power up\n"); /* [한국어] 전원 결함이라고 알린다 */
		else if (SLOT_BUS_SPEED(slot_cur->status)) { /* [한국어] 그렇지 않고 속도 불일치 비트가 서 있으면 */
			err("bus speed mismatch occurred.  please check current bus speed and card capability\n"); /* [한국어] 그 사실을 알리고 */
			print_card_capability(slot_cur); /* [한국어] 카드가 낼 수 있는 속도를 함께 알려 준다 */
		} else if (SLOT_BUS_MODE(slot_cur->ext_status)) { /* [한국어] 그렇지 않고 모드 불일치 비트가 서 있으면 */
			err("bus mode mismatch occurred.  please check current bus mode and card capability\n"); /* [한국어] 그 사실을 알리고 */
			print_card_capability(slot_cur); /* [한국어] 카드의 능력을 함께 알려 준다 */
		}
		ibmphp_update_slot_info(slot_cur); /* [한국어] 바뀐 상태를 코어에 반영한다 */
		goto exit; /* [한국어] 잠금을 푸는 공통 출구로 간다. **전원은 이미 꺼진 상태이므로** error_power 로 가지 않는다 */
	}
	debug("after power_on\n"); /* [한국어] 전원이 들어왔음을 남긴다 */
	/*-----------------------debugging---------------------------*/
	get_cur_bus_info(&slot_cur); /* [한국어] 전원을 넣은 뒤 실제 속도를 다시 확인한다 */
	debug("the current bus speed right after power_on = %x\n", /* [한국어] 그 값을 남긴다 */
					slot_cur->bus_on->current_speed); /* [한국어] 읽은 속도 */
	/*----------------------------------------------------------*/

	rc = slot_update(&slot_cur); /* [한국어] **6단계** — 상태를 새로 읽어 무엇이 어긋났는지 가른다 */
	if (rc) /* [한국어] 읽기가 실패하면 */
		goto error_power; /* [한국어] 전원을 넣은 뒤이므로 전원까지 되돌리는 경로로 간다 */

	rc = -EINVAL; /* [한국어] **아래 세 검사의 기본 반환값을 미리 담아 둔다** — 어느 것에 걸리든 -EINVAL 이 된다 */
	if (SLOT_POWER(slot_cur->status) && !(SLOT_PWRGD(slot_cur->status))) { /* [한국어] 전원 스위치는 켜졌는데 정상 전압이 안 들어왔으면 */
		err("power fault occurred trying to power up...\n"); /* [한국어] 전원 결함이라고 알리고 */
		goto error_power; /* [한국어] 전원까지 되돌리는 경로로 간다 */
	}
	if (SLOT_POWER(slot_cur->status) && (SLOT_BUS_SPEED(slot_cur->status))) { /* [한국어] 전원은 켜졌는데 속도 불일치 비트가 서 있으면 */
		err("bus speed mismatch occurred.  please check current bus speed and card capability\n"); /* [한국어] 그 사실을 알리고 */
		print_card_capability(slot_cur); /* [한국어] 카드의 능력을 알려 준 뒤 */
		goto error_power; /* [한국어] 전원까지 되돌리는 경로로 간다 */
	}
	/* Don't think this case will happen after above checks...
	 * but just in case, for paranoia sake */
	if (!(SLOT_POWER(slot_cur->status))) { /* [한국어] 전원 스위치조차 안 켜졌으면 — 바로 위 상류 주석이 위의 검사들을 지나면 일어나지 않을 것이라 적으면서도 만일을 위해 둔 것이라 밝힌다 */
		err("power on failed...\n"); /* [한국어] 전원 켜기가 실패했다고 알리고 */
		goto error_power; /* [한국어] 전원까지 되돌리는 경로로 간다 */
	}

	slot_cur->func = kzalloc_obj(struct pci_func); /* [한국어] **7단계** — 이 카드의 함수 구조체를 만든다. ibmphp_pci.c 가 이것을 받아 BAR 를 채운다 */
	if (!slot_cur->func) { /* [한국어] 못 잡으면 */
		/* do update_slot_info here? */
		rc = -ENOMEM; /* [한국어] 메모리 부족을 담아 */
		goto error_power; /* [한국어] 전원까지 되돌리는 경로로 간다 */
	}
	slot_cur->func->busno = slot_cur->bus; /* [한국어] 슬롯의 버스 번호를 베낀다 */
	slot_cur->func->device = slot_cur->device; /* [한국어] 슬롯의 장치 번호를 베낀다. ibmphp_init_devno() 가 BIOS 표에서 구해 둔 값이다 */
	for (i = 0; i < 4; i++) /* [한국어] 인터럽트 핀 네 개마다 */
		slot_cur->func->irq[i] = slot_cur->irq[i]; /* [한국어] 배정된 IRQ 를 베낀다. ibmphp_pci.c 가 PCI_INTERRUPT_LINE 에 적을 값이다 */

	debug("b4 configure_card, slot_cur->bus = %x, slot_cur->device = %x\n", /* [한국어] 설정 직전 값을 남긴다 */
					slot_cur->bus, slot_cur->device); /* [한국어] 버스와 장치 번호 */

	if (ibmphp_configure_card(slot_cur->func, slot_cur->number)) { /* [한국어] **8단계** — BAR 와 브리지 창을 실제로 채운다. 이 파일 밖의 ibmphp_pci.c 가 맡는 일이다 */
		err("configure_card was unsuccessful...\n"); /* [한국어] 실패하면 그 사실을 알리고 */
		/* true because don't need to actually deallocate resources,
		 * just remove references */
		ibmphp_unconfigure_card(&slot_cur, 1); /* [한국어] 바로 위 상류 주석대로 **마지막 인자 1** 은 자원을 실제로 해제할 필요는 없고 참조만 끊으면 된다는 뜻이다 */
		debug("after unconfigure_card\n"); /* [한국어] 되돌렸음을 남긴다 */
		slot_cur->func = NULL; /* [한국어] 함수 목록의 머리를 끊는다 */
		rc = -ENOMEM; /* [한국어] 메모리 부족으로 담아 */
		goto error_power; /* [한국어] 전원까지 되돌리는 경로로 간다 */
	}

	function = 0x00; /* [한국어] **9단계** — 함수 0 부터 훑는다 */
	do { /* [한국어] ibm_slot_find() 가 NULL 을 돌려줄 때까지 돈다 */
		tmp_func = ibm_slot_find(slot_cur->bus, slot_cur->func->device, /* [한국어] 그 버스·장치의 다음 함수를 찾는다 */
							function++); /* [한국어] 찾은 뒤 함수 번호를 올린다 */
		if (tmp_func && !(tmp_func->dev)) /* [한국어] 찾았고 아직 struct pci_dev 가 없으면 */
			ibm_configure_device(tmp_func); /* [한국어] **PCI 코어에게 열거와 드라이버 결합을 맡긴다** */
	} while (tmp_func); /* [한국어] 못 찾을 때까지 반복한다. 다중 함수 카드와 브리지 뒤 장치가 모두 하나의 함수 목록에 이어져 있기 때문이다 */

	attn_off(slot_cur); /* [한국어] **10단계** — 작업이 끝났으므로 LED 를 끈다 */
	if (slot_update(&slot_cur)) { /* [한국어] 상태를 새로 읽는다. 실패하면 */
		rc = -EFAULT; /* [한국어] 오류를 담아 */
		goto exit; /* [한국어] 잠금을 푸는 공통 출구로 간다 */
	}
	ibmphp_print_test(); /* [한국어] 자원 장부 전체를 디버그 로그로 남긴다 */
	rc = ibmphp_update_slot_info(slot_cur); /* [한국어] 바뀐 상태를 코어에 반영한다 */
exit: /* [한국어] **공통 출구** — 성공 경로와 모든 실패 경로가 여기서 만난다 */
	ibmphp_unlock_operations(); /* [한국어] 잠금을 푼다. 이 줄을 반드시 지나야 다른 조작이 가능해진다 */
	return rc; /* [한국어] 담아 둔 결과를 돌려준다 */

error_nopower: /* [한국어] **전원을 넣기 전 실패 경로** — LED 만 되돌리면 된다 */
	attn_off(slot_cur);	/* need to turn off if was blinking b4 */ /* [한국어] 상류 주석대로 먼저 끈다 — 깜빡임 상태에서 바로 켜면 덮어써지지 않기 때문이다 */
	attn_on(slot_cur); /* [한국어] 그 다음 켜서 오류 상태임을 알린다 */
error_cont: /* [한국어] **두 실패 경로가 만나는 자리** — 상태를 다시 읽어 코어에 알리는 공통 마무리다 */
	rcpr = slot_update(&slot_cur); /* [한국어] 상태를 새로 읽는다 */
	if (rcpr) { /* [한국어] 그것마저 실패하면 */
		rc = rcpr; /* [한국어] 그 코드로 바꿔 담고 */
		goto exit; /* [한국어] 공통 출구로 간다 */
	}
	ibmphp_update_slot_info(slot_cur); /* [한국어] 바뀐 상태를 코어에 반영한다 */
	goto exit; /* [한국어] 공통 출구로 간다 */

error_power: /* [한국어] **전원을 넣은 뒤 실패 경로** — LED 와 전원을 모두 되돌려야 한다 */
	attn_off(slot_cur);	/* need to turn off if was blinking b4 */ /* [한국어] 상류 주석대로 먼저 끈다 */
	attn_on(slot_cur); /* [한국어] 그 다음 켜서 오류 상태임을 알린다 */
	rcpr = power_off(slot_cur); /* [한국어] **넣었던 전원을 끈다.** 이것이 error_nopower 와의 유일한 차이다 */
	if (rcpr) { /* [한국어] 끄기마저 실패하면 */
		rc = rcpr; /* [한국어] 그 코드로 바꿔 담고 */
		goto exit; /* [한국어] 공통 출구로 간다 */
	}
	goto error_cont; /* [한국어] 상태를 다시 읽어 알리는 공통 마무리로 이어진다 */
}

/**************************************************************
* HOT REMOVING ADAPTER CARD                                   *
* INPUT: POINTER TO THE HOTPLUG SLOT STRUCTURE                *
* OUTPUT: SUCCESS 0 ; FAILURE: UNCONFIGURE , VALIDATE         *
*		DISABLE POWER ,                               *
**************************************************************/
/* [한국어]
 * ibmphp_disable_slot - 핫플러그 코어의 제거 콜백. 잠금만 잡고 넘긴다
 *
 * @hotplug_slot: 핫플러그 코어 쪽 슬롯 구조체.
 * @return: ibmphp_do_disable_slot() 의 반환값 그대로.
 *
 * 실제 일은 전부 ibmphp_do_disable_slot() 이 한다. 이 얇은 껍데기가 따로
 * 있는 이유는 **잠금을 잡는 쪽과 잡지 않는 쪽이 둘 다 필요하기 때문**이다 —
 * ibmphp_hpc.c 의 폴링 스레드는 이미 잠금을 잡은 상태에서 제거를 하므로
 * 안쪽 함수를 직접 부른다. 여기서 다시 잡으면 자기 자신과 교착한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(sysfs 쓰기).
 *
 * 호출 체인:
 *   핫플러그 코어 -> [이 함수] -> ibmphp_lock_operations()
 *                              -> ibmphp_do_disable_slot()
 */
static int ibmphp_disable_slot(struct hotplug_slot *hotplug_slot)
{
	struct slot *slot = to_slot(hotplug_slot); /* [한국어] 핫플러그 코어 쪽 구조체에서 이 드라이버의 슬롯을 되찾는다 */
	int rc; /* [한국어] 하위 호출의 결과 */

	ibmphp_lock_operations(); /* [한국어] **여기서만 잠금을 잡는다.** 폴링 스레드는 이미 잡은 상태에서 아래 함수를 직접 부르므로, 그쪽 경로가 여기를 지나면 자기 자신과 교착한다 */
	rc = ibmphp_do_disable_slot(slot); /* [한국어] 실제 제거는 전부 이 함수가 한다 */
	ibmphp_unlock_operations(); /* [한국어] 잠금을 푼다 */
	return rc; /* [한국어] 결과를 그대로 돌려준다 */
}

/* [한국어]
 * ibmphp_do_disable_slot - 카드를 커널과 장부에서 걷어내고 전원을 끈다
 *
 * @slot_cur: 끌 슬롯.
 * @return: 0 이면 성공. 슬롯이나 컨트롤러가 없으면 -ENODEV, 메모리가
 *          없으면 -ENOMEM, 그 밖은 하위 호출의 코드.
 *
 * 카드 제거의 전 과정이다. 잠금을 잡지 않으므로 호출자가 이미 잡고 있어야
 * 한다 — ibmphp_disable_slot() 은 잡고 부르고, ibmphp_hpc.c 의 폴링
 * 스레드도 잡은 상태에서 부른다.
 *
 * **flag 가 이 함수의 갈림길**이다. 진입할 때 slot->flag 를 지역 변수로
 * 빼내고 슬롯 쪽은 1 로 세운다. flag 가 1 이었으면 정상적인 제거 요청이고,
 * 0 이었으면 아래 상류 주석이 밝히는 비정상 상황이다 — 카드가 돌고 있는데
 * 걸쇠가 갑자기 열렸거나 전원 결함이 난 경우다. 그때는 카드에 전원이 없어
 * 어떤 자원을 쓰고 있었는지 읽을 수조차 없으므로, 커널 목록에서 빼는 것만
 * 하고 자원 정리 없이 나간다.
 *
 * 정상 경로의 순서:
 *   1. validate(DISABLE)        — 지금 꺼도 되는 상태인가.
 *   2. attn_LED_blink()         — 작업 중임을 알린다.
 *   3. func 가 없으면 만든다    — 부팅 때부터 있던 카드는 이 드라이버가
 *      설정한 적이 없어 pci_func 가 없다. 그 경우에도 버스·장치 번호는
 *      알아야 하므로 최소한만 채워 만든다.
 *   4. ibm_unconfigure_device() — 커널에서 struct pci_dev 를 걷어낸다.
 *   5. ibmphp_unconfigure_card(&slot_cur, 0) — 장부에서 자원을 지운다.
 *      마지막 인자 0 은 "드라이버는 살아 있으니 장부에서 실제로 지우라" 는
 *      뜻이다(free_slots() 는 -1, enable_slot() 실패 경로는 1 을 쓴다).
 *   6. HPC_SLOT_OFF             — 전원을 끈다.
 *   7. attn_off() 와 상태 반영.
 *
 * **4번이 5번보다 먼저**인 이유는, 자원을 지우기 전에 그 자원을 쓰던
 * 드라이버를 먼저 떼어내야 하기 때문이다.
 *
 * 에러 경로는 LED 를 되돌리고 상태를 다시 읽어 코어에 알린다. 다만
 * flag 가 0 이었으면(비정상 상황) 코어에 알리지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트와 폴링 커널 스레드 양쪽.
 *
 * 호출 체인:
 *   ibmphp_disable_slot() / ibmphp_hpc.c 의 폴링 루프 -> [이 함수]
 *     -> validate() -> ibm_unconfigure_device() -> ibmphp_unconfigure_card()
 *     -> ibmphp_hpc_writeslot() -> ibmphp_update_slot_info()
 */
int ibmphp_do_disable_slot(struct slot *slot_cur)
{
	int rc; /* [한국어] 하위 호출 결과 임시 보관 */
	u8 flag; /* [한국어] **이 함수의 갈림길.** 진입 시점의 slot->flag 값을 빼내 둔다 */

	debug("DISABLING SLOT...\n"); /* [한국어] 시작을 남긴다 */

	if ((slot_cur == NULL) || (slot_cur->ctrl == NULL)) /* [한국어] 슬롯이 없거나 담당 컨트롤러가 없으면 */
		return -ENODEV; /* [한국어] 장치 없음으로 거절한다 */

	flag = slot_cur->flag; /* [한국어] **진입 시점의 값을 빼내고** */
	slot_cur->flag = 1; /* [한국어] 슬롯 쪽은 1 로 세운다. 다음 번 제거 요청은 정상 경로로 들어오게 된다 */

	if (flag == 1) { /* [한국어] **1 이었으면 정상적인 제거 요청**이다 */
		rc = validate(slot_cur, DISABLE); /* [한국어] 지금 꺼도 되는 상태인지 본다. 바로 옆 상류 주석대로 이미 꺼져 있는지와 슬롯 번호가 맞는지를 확인한다 */
			/* checking if powered off already & valid slot # */
		if (rc) /* [한국어] 아니면 */
			goto error; /* [한국어] LED 를 되돌리는 경로로 간다 */
	}
	attn_LED_blink(slot_cur); /* [한국어] 작업 중임을 LED 깜빡임으로 알린다 */

	if (slot_cur->func == NULL) { /* [한국어] **함수 구조체가 없으면** — 부팅 때부터 있던 카드는 이 드라이버가 설정한 적이 없어 목록이 없다 */
		/* We need this for functions that were there on bootup */
		slot_cur->func = kzalloc_obj(struct pci_func); /* [한국어] 바로 위 상류 주석대로 그런 경우를 위해 최소한만 만든다 */
		if (!slot_cur->func) { /* [한국어] 못 잡으면 */
			rc = -ENOMEM; /* [한국어] 메모리 부족을 담아 */
			goto error; /* [한국어] LED 를 되돌리는 경로로 간다 */
		}
		slot_cur->func->busno = slot_cur->bus; /* [한국어] 버스 번호만 채우고 */
		slot_cur->func->device = slot_cur->device; /* [한국어] 장치 번호도 채운다. 아래 ibm_unconfigure_device() 가 이 둘만 쓴다 */
	}

	ibm_unconfigure_device(slot_cur->func); /* [한국어] **커널에서 struct pci_dev 를 걷어낸다.** 자원을 지우기 전에 그 자원을 쓰던 드라이버를 먼저 떼어내야 한다 */

	/*
	 * If we got here from latch suddenly opening on operating card or
	 * a power fault, there's no power to the card, so cannot
	 * read from it to determine what resources it occupied.  This operation
	 * is forbidden anyhow.  The best we can do is remove it from kernel
	 * lists at least */

	if (!flag) { /* [한국어] **flag 가 0 이었으면 비정상 상황이다** — 바로 위 상류 주석이 밝히듯 걸쇠가 갑자기 열렸거나 전원 결함이 난 경우로, 카드에 전원이 없어 어떤 자원을 쓰고 있었는지 읽을 수조차 없다 */
		attn_off(slot_cur); /* [한국어] LED 만 끄고 */
		return 0; /* [한국어] 커널 목록에서 뺀 것으로 만족하고 나간다 */
	}

	rc = ibmphp_unconfigure_card(&slot_cur, 0); /* [한국어] **장부에서 자원을 지운다.** 마지막 인자 0 은 드라이버가 살아 있으니 실제로 지우라는 뜻이다(free_slots() 는 -1, enable_slot() 실패 경로는 1 을 쓴다) */
	slot_cur->func = NULL; /* [한국어] 함수 목록의 머리를 끊는다. 목록 자체는 위 호출이 이미 버렸다 */
	debug("in disable_slot. after unconfigure_card\n"); /* [한국어] 되돌렸음을 남긴다 */
	if (rc) { /* [한국어] 실패했으면 */
		err("could not unconfigure card.\n"); /* [한국어] 그 사실을 알리고 */
		goto error; /* [한국어] LED 를 되돌리는 경로로 간다 */
	}

	rc = ibmphp_hpc_writeslot(slot_cur, HPC_SLOT_OFF); /* [한국어] **전원을 끈다.** 여기까지 와야 카드를 실제로 뽑을 수 있다 */
	if (rc) /* [한국어] 실패하면 */
		goto error; /* [한국어] LED 를 되돌리는 경로로 간다 */

	attn_off(slot_cur); /* [한국어] 작업이 끝났으므로 LED 를 끈다 */
	rc = slot_update(&slot_cur); /* [한국어] 상태를 새로 읽는다 */
	if (rc) /* [한국어] 실패하면 */
		goto exit; /* [한국어] 공통 출구로 간다 */

	rc = ibmphp_update_slot_info(slot_cur); /* [한국어] 바뀐 상태를 코어에 반영한다 */
	ibmphp_print_test(); /* [한국어] 자원 장부 전체를 디버그 로그로 남긴다 */
exit: /* [한국어] **공통 출구** */
	return rc; /* [한국어] 담아 둔 결과를 돌려준다 */

error: /* [한국어] **실패 경로** — LED 를 되돌리고 상태를 다시 알린다 */
	/*  Need to turn off if was blinking b4 */
	attn_off(slot_cur); /* [한국어] 상류 주석대로 먼저 끈다 — 깜빡임 상태에서 바로 켜면 덮어써지지 않기 때문이다 */
	attn_on(slot_cur); /* [한국어] 그 다음 켜서 오류 상태임을 알린다 */
	if (slot_update(&slot_cur)) { /* [한국어] 상태를 새로 읽는다. 그것마저 실패하면 */
		rc = -EFAULT; /* [한국어] 오류로 바꿔 담고 */
		goto exit; /* [한국어] 공통 출구로 간다 */
	}
	if (flag) /* [한국어] **정상 요청이었을 때만** 코어에 알린다 — 비정상 상황에서는 상태 자체를 믿을 수 없기 때문이다 */
		ibmphp_update_slot_info(slot_cur); /* [한국어] 바뀐 상태를 코어에 반영한다 */
	goto exit; /* [한국어] 공통 출구로 간다 */
}

const struct hotplug_slot_ops ibmphp_hotplug_slot_ops = { /* [한국어] **핫플러그 코어가 부를 콜백 묶음.** ibmphp_ebda.c 가 슬롯을 등록할 때 이 구조체를 매단다 */
	.set_attention_status =		set_attention_status, /* [한국어] 주의 LED 조작 */
	.enable_slot =			enable_slot, /* [한국어] sysfs 로 슬롯을 켤 때 */
	.disable_slot =			ibmphp_disable_slot, /* [한국어] sysfs 로 슬롯을 끌 때. 이쪽만 잠금을 잡는 껍데기를 거친다 */
	.hardware_test =		NULL, /* [한국어] 하드웨어 시험은 지원하지 않는다 */
	.get_power_status =		get_power_status, /* [한국어] 전원 상태 조회 */
	.get_attention_status =		get_attention_status, /* [한국어] 주의 LED 상태 조회 */
	.get_latch_status =		get_latch_status, /* [한국어] 걸쇠 상태 조회 */
	.get_adapter_status =		get_adapter_present, /* [한국어] 카드 유무 조회 */
};

/* [한국어]
 * ibmphp_unload - 초기화가 세운 것을 역순으로 모두 허문다
 *
 * 초기화 실패 경로와 모듈 해제 양쪽에서 쓰는 공통 정리 함수다. 어느 단계
 * 에서 실패했든 여기로 오므로, 아직 세워지지 않은 것을 허물어도 문제가
 * 없도록 각 해제 함수가 빈 목록을 견딘다.
 *
 * 순서에 뜻이 있다.
 *   1. free_slots()                   슬롯을 코어에서 빼고 버린다. 자원은
 *                                     건드리지 않는다(-1 을 넘긴다).
 *   2. ibmphp_free_resources()        [ibmphp_res.c] 자원 장부를 통째로.
 *   3. ibmphp_free_bus_info_queue()   [ibmphp_ebda.c] 버스 정보 목록.
 *   4. ibmphp_free_ebda_hpc_queue()   [ibmphp_ebda.c] 컨트롤러 목록.
 *                                     ISA I/O 영역 반납과 pci_driver 해제도
 *                                     여기서 함께 일어난다.
 *   5. ibmphp_free_ebda_pci_rsrc_queue() [ibmphp_ebda.c] POST 자원 목록.
 *   6. ibmphp_pci_bus                 껍데기 버스를 버린다.
 * 1 이 2 보다 먼저인 이유는 슬롯이 자원을 가리키고 있기 때문이고,
 * 6 이 마지막인 이유는 앞 단계들이 config 접근에 그 버스를 쓸 수 있기
 * 때문이다.
 *
 * 실행 컨텍스트: 모듈 초기화 실패 경로와 모듈 해제, 둘 다 프로세스
 * 컨텍스트. 해제 경로에서는 폴링 스레드가 이미 멈춘 뒤다.
 *
 * 호출 체인:
 *   ibmphp_init() 실패 경로 / ibmphp_exit() -> [이 함수] -> 각 파일의 해제 함수
 */
static void ibmphp_unload(void)
{
	free_slots(); /* [한국어] **1. 슬롯을 코어에서 빼고 버린다.** 자원은 건드리지 않는다 — 아래에서 통째로 버리기 때문이다 */
	debug("after slots\n"); /* [한국어] 끝났음을 남긴다 */
	ibmphp_free_resources(); /* [한국어] **2. 자원 장부를 통째로 버린다** */
	debug("after resources\n"); /* [한국어] 끝났음을 남긴다 */
	ibmphp_free_bus_info_queue(); /* [한국어] **3. 버스 정보 목록을 버린다** */
	debug("after bus info\n"); /* [한국어] 끝났음을 남긴다 */
	ibmphp_free_ebda_hpc_queue(); /* [한국어] **4. 컨트롤러 목록을 버린다.** ISA I/O 영역 반납과 pci_driver 해제도 여기서 함께 일어난다 */
	debug("after ebda hpc\n"); /* [한국어] 끝났음을 남긴다 */
	ibmphp_free_ebda_pci_rsrc_queue(); /* [한국어] **5. POST 자원 목록을 버린다** */
	debug("after ebda pci rsrc\n"); /* [한국어] 끝났음을 남긴다 */
	kfree(ibmphp_pci_bus); /* [한국어] **6. 껍데기 버스를 마지막에 버린다** — 앞 단계들이 config 접근에 이 버스를 쓸 수 있기 때문이다 */
}

/* [한국어]
 * ibmphp_init - 모듈 진입점. 다섯 단계를 순서대로 세운다
 *
 * @return: 0 이면 성공. 어느 단계든 실패하면 그때까지 세운 것을 허물고
 *          그 오류 코드를 돌려준다.
 *
 * 이 드라이버 전체의 시작점이다. 하는 일이 준비 둘과 본 단계 다섯이다.
 *
 * 준비:
 *   - init_flag 를 세운다. 이 표시가 서 있는 동안 slot_update() 는 버스
 *     정보를 읽지 않는다. init_ops() 가 끝나면서 내린다.
 *   - **ibmphp_pci_bus 를 만들어 0번 도메인 0번 버스를 통째로 베낀다.**
 *     진짜 버스가 아니라 pci_bus_read_config_* 계열을 부르기 위한
 *     매개체이며, ibmphp_pci.c 가 접근 때마다 ->number 를 갈아 끼운다.
 *     0번 버스를 못 찾으면 여기서 접는다 — config 접근 함수를 얻을 곳이
 *     없기 때문이다.
 *
 * 본 단계:
 *   1. ibmphp_access_ebda()  [ibmphp_ebda.c] EBDA 표를 읽어 컨트롤러·슬롯·
 *      자원 목록 셋을 세운다. 이것이 실패하면 나머지가 모두 의미가 없다.
 *   2. ibmphp_rsrc_init()    [ibmphp_res.c] 위 자원 목록을 자유 목록 장부로
 *      바꾼다.
 *   3. get_max_slots()       슬롯 번호의 상한을 구해 둔다.
 *   4. ibmphp_register_pci() [ibmphp_ebda.c] PCI 형 컨트롤러가 있으면
 *      pci_driver 를 등록한다.
 *   5. init_ops()            슬롯마다 컨트롤러 정보를 채우고 BIOS 가 켜 둔
 *      빈 슬롯의 전원을 끈다.
 *   6. ibmphp_hpc_start_poll_thread() [ibmphp_hpc.c] 상태 변화를 감시하는
 *      커널 스레드를 띄운다. **가장 마지막인 이유**는 그 스레드가 도는
 *      순간부터 위의 모든 자료구조가 다른 실행 흐름에서 읽히기 때문이다.
 *
 * irqs[] 를 0 으로 채우는 루프가 있으나 그 배열을 읽는 곳은 없다
 * (전수 grep 확인).
 *
 * 실행 컨텍스트: 모듈 적재(프로세스 컨텍스트), 단일 스레드. __init 이다.
 *
 * 에러 경로: 모든 실패가 error 라벨로 모여 ibmphp_unload() 를 부른 뒤
 * exit 로 떨어진다. ibmphp_pci_bus 할당 실패만 예외로, 아직 허물 것이
 * 없으므로 곧바로 exit 로 간다.
 *
 * 호출 체인:
 *   module_init -> [이 함수] -> pci_find_bus() -> ibmphp_access_ebda()
 *     -> ibmphp_rsrc_init() -> get_max_slots() -> ibmphp_register_pci()
 *     -> init_ops() -> ibmphp_hpc_start_poll_thread()
 */
static int __init ibmphp_init(void)
{
	struct pci_bus *bus; /* [한국어] 0번 버스를 찾아 담을 자리 */
	int i = 0; /* [한국어] irqs[] 초기화 루프 인덱스 */
	int rc = 0; /* [한국어] 호출자에게 돌려줄 값 */

	init_flag = 1; /* [한국어] **초기화가 진행 중임을 표시한다.** 이 동안 slot_update() 는 버스 정보를 읽지 않는다. init_ops() 가 끝나면서 내린다 */

	info(DRIVER_DESC " version: " DRIVER_VERSION "\n"); /* [한국어] 드라이버 이름과 판 번호를 한 줄 남긴다 */

	ibmphp_pci_bus = kmalloc_obj(*ibmphp_pci_bus); /* [한국어] **껍데기 버스를 잡는다** */
	if (!ibmphp_pci_bus) { /* [한국어] 못 잡으면 */
		rc = -ENOMEM; /* [한국어] 메모리 부족을 담고 */
		goto exit; /* [한국어] 아직 허물 것이 없으므로 곧바로 나간다 */
	}

	bus = pci_find_bus(0, 0); /* [한국어] **0번 도메인 0번 버스를 찾는다** */
	if (!bus) { /* [한국어] 못 찾으면 config 접근 함수를 얻을 곳이 없다 */
		err("Can't find the root pci bus, can not continue\n"); /* [한국어] 그 사실을 알리고 */
		rc = -ENODEV; /* [한국어] 장치 없음을 담아 */
		goto error; /* [한국어] 허무는 경로로 간다 */
	}
	memcpy(ibmphp_pci_bus, bus, sizeof(*ibmphp_pci_bus)); /* [한국어] **0번 버스를 통째로 베낀다.** 진짜 버스가 아니라 pci_bus_read_config_* 계열을 부르기 위한 매개체이며, ibmphp_pci.c 가 접근 때마다 ->number 를 갈아 끼운다 */

	ibmphp_debug = debug; /* [한국어] 모듈 파라미터를 debug() 매크로가 보는 전역에 옮겨 담는다 */

	for (i = 0; i < 16; i++) /* [한국어] IRQ 기록 배열을 비운다 */
		irqs[i] = 0; /* [한국어] 0 으로 채운다. 이 배열을 읽는 코드는 남아 있지 않다 */

	rc = ibmphp_access_ebda(); /* [한국어] **1단계** — EBDA 표를 읽어 컨트롤러·슬롯·자원 목록 셋을 세운다 */
	if (rc) /* [한국어] 실패하면 */
		goto error; /* [한국어] 허무는 경로로 간다 */
	debug("after ibmphp_access_ebda()\n"); /* [한국어] 끝났음을 남긴다 */

	rc = ibmphp_rsrc_init(); /* [한국어] **2단계** — 위 자원 목록을 자유 목록 장부로 바꾼다 */
	if (rc) /* [한국어] 실패하면 */
		goto error; /* [한국어] 허무는 경로로 간다 */
	debug("AFTER Resource & EBDA INITIALIZATIONS\n"); /* [한국어] 두 초기화가 끝났음을 남긴다 */

	max_slots = get_max_slots(); /* [한국어] **3단계** — 슬롯 번호의 상한을 구해 둔다. validate() 가 이 값을 쓴다 */

	rc = ibmphp_register_pci(); /* [한국어] **4단계** — PCI 형 컨트롤러가 있으면 pci_driver 를 등록한다 */
	if (rc) /* [한국어] 실패하면 */
		goto error; /* [한국어] 허무는 경로로 간다 */

	if (init_ops()) { /* [한국어] **5단계** — 슬롯마다 컨트롤러 정보를 채우고 BIOS 가 켜 둔 빈 슬롯의 전원을 끈다 */
		rc = -ENODEV; /* [한국어] 실패하면 장치 없음을 담고 */
		goto error; /* [한국어] 허무는 경로로 간다 */
	}

	ibmphp_print_test(); /* [한국어] 세운 자원 장부 전체를 디버그 로그로 남긴다 */
	rc = ibmphp_hpc_start_poll_thread(); /* [한국어] **6단계** — 상태 변화를 감시하는 커널 스레드를 띄운다. **가장 마지막인 이유**는 그 스레드가 도는 순간부터 위의 모든 자료구조가 다른 실행 흐름에서 읽히기 때문이다 */
	if (rc) /* [한국어] 실패하면 */
		goto error; /* [한국어] 허무는 경로로 간다 */

exit: /* [한국어] **공통 출구** */
	return rc; /* [한국어] 담아 둔 결과를 돌려준다 */

error: /* [한국어] **허무는 경로** — 어느 단계에서 실패했든 여기로 온다 */
	ibmphp_unload(); /* [한국어] 초기화가 세운 것을 역순으로 허문다. 아직 세워지지 않은 것을 허물어도 문제가 없도록 각 해제 함수가 빈 목록을 견딘다 */
	goto exit; /* [한국어] 공통 출구로 간다 */
}

/* [한국어]
 * ibmphp_exit - 모듈 해제. 폴링 스레드를 먼저 멈추고 나머지를 허문다
 *
 * **순서가 중요하다.** 폴링 스레드를 먼저 멈춰야 하는 이유는, 그 스레드가
 * 슬롯 목록과 컨트롤러 목록을 계속 훑으면서 상태 변화가 있으면
 * ibmphp_do_disable_slot() 까지 부르기 때문이다. 허무는 도중에 그 스레드가
 * 살아 있으면 이미 버린 구조체를 읽게 된다.
 *
 * ibmphp_hpc_stop_poll_thread() 는 스레드에게 멈추라고 알리고 실제로
 * 끝날 때까지 기다린다(ibmphp_hpc.c 의 구현). 그 뒤에야
 * ibmphp_unload() 가 초기화가 세운 것을 역순으로 허문다.
 *
 * 실행 컨텍스트: 모듈 해제(프로세스 컨텍스트). __exit 이다.
 *
 * 호출 체인:
 *   module_exit -> [이 함수] -> ibmphp_hpc_stop_poll_thread()
 *                             -> ibmphp_unload()
 */
static void __exit ibmphp_exit(void)
{
	ibmphp_hpc_stop_poll_thread(); /* [한국어] **먼저 폴링 스레드를 멈추고 끝날 때까지 기다린다.** 그 스레드가 슬롯·컨트롤러 목록을 계속 훑으므로, 살아 있는 채로 허물면 이미 버린 구조체를 읽게 된다 */
	debug("after polling\n"); /* [한국어] 멈췄음을 남긴다 */
	ibmphp_unload(); /* [한국어] 그 다음에야 초기화가 세운 것을 역순으로 허문다 */
	debug("done\n"); /* [한국어] 끝났음을 남긴다 */
}

module_init(ibmphp_init);
module_exit(ibmphp_exit);
