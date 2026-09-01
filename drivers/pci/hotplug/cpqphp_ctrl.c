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
 * [한국어 설명] Compaq 핫플러그 컨트롤러의 이벤트 상태 기계와 자원 할당기 (cpqphp_ctrl.c)
 *
 * === 파일의 역할 ===
 * Compaq(현 HPE) 서버의 PCI 핫플러그 컨트롤러에서 "슬롯에 무슨 일이
 * 일어났을 때 무엇을 할 것인가" 를 담당한다. 크게 세 덩어리다.
 * 첫째, 인터럽트를 받아 어떤 사건인지 분류하고 이벤트 큐에 쌓는 부분
 * (handle_switch_change, handle_presence_change, handle_power_fault,
 * cpqhp_ctrl_intr). 둘째, 그 큐를 커널 스레드에서 꺼내 LED 를 켜고 슬롯에
 * 전원을 넣고 카드를 설정하는 상태 기계(interrupt_event_handler,
 * cpqhp_pushbutton_thread, board_added, board_replaced, remove_board).
 * 셋째, **이 드라이버가 직접 굴리는 PCI 자원 할당기** 다
 * (get_resource, get_max_resource, get_io_resource, sort_by_size,
 * cpqhp_resource_sort_and_combine, configure_new_function).
 *
 * 세 번째가 이 파일에서 가장 이질적인 대목이다. 이 드라이버는 PCI 코어의
 * 자원 할당기를 쓰지 않고, IO/메모리/prefetchable 메모리/버스 번호마다
 * 자기 자유 목록(free list)을 단일 연결 리스트로 들고 다니면서 직접
 * 쪼개고 합친다. 그 이유는 아래 부가 절에서 따로 짚는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 드라이버가 파일 넷으로 나뉘어 있고, 이 파일은 그 한가운데에 있다.
 *
 *   cpqphp_core.c  : PCI 드라이버 등록, 컨트롤러 탐지, 슬롯 sysfs 등록,
 *                    pci_hotplug 코어의 hotplug_slot_ops 콜백 구현.
 *                    **아직 주석되지 않았다.**
 *   [이 파일]      : 인터럽트 처리, 이벤트 상태 기계, 자원 할당,
 *                    카드 추가/제거/교체 절차.
 *   cpqphp_pci.c   : PCI 설정공간을 저장하고 되돌리는 일, ROM 안의
 *                    자원 표(HRT)를 읽어 자유 목록의 초기값을 만드는 일.
 *   cpqphp_nvram.c : NVRAM 에 상태를 남기는 일.
 *   cpqphp.h       : 자료구조 전부와, 레지스터를 두드리는 인라인 함수들
 *                    (LED 제어, 슬롯 전원, set_SOGO, wait_for_ctrl_irq).
 *                    **아직 주석되지 않았다.**
 *
 * 위로는 drivers/pci/hotplug/pci_hotplug_core.c 가 sysfs 를 통해 들어오고,
 * 아래로는 컨트롤러의 MMIO 레지스터(ctrl->hpc_reg)와 PCI 설정공간을 직접
 * 두드린다. PCI 코어에는 pci_bus_read_config_ 계열과
 * pci_scan_slot / pci_stop_and_remove_bus_device 로 닿는다.
 *
 * 부팅 시 흐름:
 *   cpqphp_core.c 의 probe
 *     → cpqhp_save_config          (cpqphp_pci.c: 기존 카드 설정공간 저장)
 *     → cpqhp_find_available_resources (cpqphp_pci.c: 자유 목록 초기화)
 *     → cpqhp_event_start_thread   (이 파일: 전역 커널 스레드 하나 기동)
 *     → request_irq(cpqhp_ctrl_intr)
 *
 * 사건이 일어났을 때 흐름:
 *   슬롯 하드웨어 변화
 *     → cpqhp_ctrl_intr            (하드 인터럽트 컨텍스트)
 *         → handle_switch_change / _presence_change / _power_fault
 *             → ctrl->event_queue[] 에 적재
 *         → wake_up_process(cpqhp_event_thread)
 *     → event_thread               (프로세스 컨텍스트)
 *         → interrupt_event_handler
 *             → LED 를 깜빡이고 5초 타이머를 건다
 *     → pushbutton_helper_thread   (타이머 컨텍스트)
 *         → 다시 event_thread 를 깨운다
 *     → cpqhp_pushbutton_thread
 *         → cpqhp_process_SI / cpqhp_process_SS
 *             → board_added / remove_board
 *
 * 실행 컨텍스트가 넷으로 갈린다 -- 하드 인터럽트(cpqhp_ctrl_intr), 타이머
 * (pushbutton_helper_thread), 커널 스레드(그 밖의 거의 전부), 그리고
 * sysfs 를 통해 들어오는 프로세스 컨텍스트(cpqhp_hardware_test 등)다.
 * 잠들 수 있는 코드가 대부분이라, 인터럽트 핸들러는 분류와 적재만 하고
 * 실제 일은 전부 스레드로 넘긴다.
 *
 * === 타 모듈과의 연결 ===
 * 이 파일이 **의존하는** 쪽:
 *   cpqphp.h 의 인라인 함수들 : set_SOGO(:470), wait_for_ctrl_irq(:704),
 *     amber_LED_on/off, green_LED_on/off/blink, slot_enable/disable,
 *     enable_slot_power/disable_slot_power, is_slot_enabled,
 *     get_adapter_speed, get_presence_status, return_resource(:461).
 *     이들이 MMIO 레지스터를 실제로 두드리므로, 이 파일은 레지스터
 *     오프셋을 거의 직접 쓰지 않고 그 함수들을 부른다.
 *   cpqphp_pci.c : cpqhp_save_slot_config, cpqhp_save_base_addr_length,
 *     cpqhp_save_used_resources, cpqhp_configure_board, cpqhp_valid_replace,
 *     cpqhp_return_board_resources, cpqhp_destroy_resource_list,
 *     cpqhp_destroy_board_resources, cpqhp_configure_device,
 *     cpqhp_unconfigure_device, cpqhp_set_irq.
 *   PCI 코어 : pci_bus_read_config_ 및 _write_config_ 계열.
 *   커널 : kthread, timer_list, msleep_interruptible, mutex.
 *
 * 이 파일이 **내보내는** 쪽(cpqphp_core.c 가 부른다):
 *   cpqhp_ctrl_intr, cpqhp_event_start_thread, cpqhp_event_stop_thread,
 *   cpqhp_process_SI, cpqhp_process_SS, cpqhp_slot_create, cpqhp_slot_find,
 *   cpqhp_resource_sort_and_combine, cpqhp_hardware_test,
 *   cpqhp_pushbutton_thread.
 *
 * 공유 상태 -- **전역이 많다는 것이 이 드라이버의 시대적 특징이다**:
 *   cpqhp_slot_list[256] : 버스 번호로 색인하는 struct pci_func 연결
 *     리스트의 배열. 시스템 전체에 하나뿐이며 cpqphp_core.c 가 정의한다.
 *     이 파일의 cpqhp_slot_create / slot_remove / cpqhp_slot_find 가
 *     그것을 직접 조작한다. 락이 없다.
 *   cpqhp_ctrl_list : 컨트롤러들의 연결 리스트. event_thread 가 순회한다.
 *   cpqhp_event_thread : **커널 스레드 하나를 모든 컨트롤러가 공유한다.**
 *   pushbutton_pending : 이 파일의 static 전역. 타이머와 스레드 사이의
 *     유일한 통로이며, 값이 있으면 스레드가 버튼 처리로 분기한다.
 *   ctrl->event_queue[10] : 컨트롤러마다 열 칸짜리 고정 링 버퍼.
 *   ctrl->crit_sect : 레지스터를 만지는 구간을 지키는 뮤텍스.
 *
 * === 주요 함수/구조체 요약 ===
 * 함수:
 *   cpqhp_ctrl_intr           : 하드 인터럽트 핸들러. MISC 레지스터로
 *     자기 인터럽트인지 가리고, 사건을 세 분류 함수에 넘긴 뒤
 *     스레드를 깨운다.
 *   handle_switch_change / handle_presence_change / handle_power_fault
 *     : 변화한 비트를 슬롯별로 훑어 이벤트 종류를 정하고 큐에 적재한다.
 *     **여기서는 하드웨어를 거의 건드리지 않는다** -- 인터럽트 컨텍스트라
 *     오래 걸리는 일을 할 수 없기 때문이다.
 *   event_thread              : 전역 커널 스레드의 본체. 깨어나면
 *     pushbutton_pending 이 있으면 버튼 처리로, 없으면 모든 컨트롤러의
 *     이벤트 큐 처리로 간다.
 *   interrupt_event_handler   : 이벤트 큐를 비우는 상태 기계. 버튼
 *     누름/뗌/취소를 BLINKINGON/BLINKINGOFF/STATIC 상태로 옮기고,
 *     뗌이면 5초 타이머를 걸어 취소할 시간을 준다.
 *   cpqhp_pushbutton_thread   : 5초가 지나 실제로 전원을 올리거나 내린다.
 *   board_added / board_replaced / remove_board : 카드 추가·교체·제거의
 *     실제 절차. 전원, LED, 속도 협상, 설정공간 처리를 순서대로 한다.
 *   set_controller_speed      : PCI/PCI-X 주파수와 모드를 카드에 맞춘다.
 *   configure_new_device / configure_new_function : 새 카드의 설정공간을
 *     채운다. 브리지를 만나면 **재귀** 로 그 아래까지 내려간다.
 *   get_resource / get_max_resource / get_io_resource : 자유 목록에서
 *     조건에 맞는 조각을 떼어 낸다. 필요하면 노드를 쪼갠다.
 *   cpqhp_resource_sort_and_combine : 자유 목록을 주소순으로 정렬하고
 *     인접한 조각을 합친다. 이 할당기의 쓰레기 수집에 해당한다.
 *
 * 구조체(정의는 cpqphp.h 에 있다):
 *   struct controller   : 컨트롤러 하나. MMIO 기준 주소, 자유 목록 네 개의
 *     머리, 이벤트 링 버퍼, 슬롯 리스트, 뮤텍스, 대기 큐를 담는다.
 *   struct slot         : 사용자에게 보이는 슬롯 하나. 상태(state)와
 *     5초 타이머(task_event)가 여기 있다.
 *   struct pci_func     : 버스/장치/함수 하나. 설정공간 사본
 *     (config_space[0x20])과 그 카드가 쓰는 자원 목록 네 개를 들고 있다.
 *   struct pci_resource : 자유 목록의 노드. base 와 length 와 next 뿐인
 *     단순한 단일 연결 리스트 원소다.
 *   struct resource_lists : 자유 목록 네 개와 IRQ 정보를 한 묶음으로
 *     넘기기 위한 꾸러미. 재귀 호출에서 값으로 복사해 쓴다.
 *
 * === 이벤트 경로 -- shpchp, pciehp 와의 3세대 대비 ===
 * 같은 디렉터리의 세 드라이버가 "인터럽트에서 받은 사건을 잠들 수 있는
 * 컨텍스트로 넘긴다" 는 같은 문제를 세 가지 방식으로 푼다. 시간순으로
 * 늘어놓으면 커널의 관용구가 어떻게 바뀌었는지가 그대로 보인다.
 *
 *   cpqphp (2001, 이 파일)
 *     적재: ctrl->event_queue[10] 고정 링 버퍼에 next_event 를 10으로
 *       나눈 나머지로 넣는다. **큐가 가득 차도 막지 않는다** -- 열한 번째
 *       사건은 처리되지 않은 첫 번째를 덮어쓴다.
 *     통지: wake_up_process 로 **전역 스레드 하나** 를 깨운다.
 *     소비: 그 스레드가 cpqhp_ctrl_list 를 순회하며 모든 컨트롤러의
 *       큐를 훑는다. 큐가 빌 때까지 열 칸을 반복해서 스캔한다.
 *     버튼: 별도의 전역 pushbutton_pending 과 타이머로 같은 스레드를
 *       재사용한다.
 *
 *   shpchp
 *     적재: 사건마다 struct event_info 를 새로 할당한다.
 *     통지·소비: queue_work 로 **슬롯마다 따로 있는 워크큐** 에 넣는다
 *       (shpchp_ctrl.c:153, :156). 한 슬롯의 느린 처리가 다른 슬롯을
 *       막지 않는 구조다.
 *
 *   pciehp
 *     적재: atomic_or 로 ctrl->pending_events 비트마스크에 쌓는다.
 *     통지·소비: request_threaded_irq 의 **IRQ 스레드** 가 atomic_xchg 로
 *       한꺼번에 가져간다(pciehp_hpc.c:50, :90, :91).
 *       할당도 큐도 없고, 같은 사건이 여러 번 와도 비트 하나로 합쳐진다.
 *
 * 정리하면 전역 스레드 + 고정 링 버퍼 → 슬롯별 워크큐 + 동적 할당 →
 * 스레드 IRQ + 원자적 비트마스크로 옮겨 왔다. 뒤로 갈수록 할당이 줄고,
 * 소유 관계가 좁아지고, 유실 가능성이 사라진다.
 *
 * === 인터럽트와 폴링, 그리고 wait_for_ctrl_irq 이야기 ===
 * 이 컨트롤러는 명령을 내린 뒤 완료를 기다리는 방법이 특이하다.
 * set_SOGO(cpqphp.h:470)로 "지금 설정한 값을 슬롯으로 밀어내라" 고
 * 지시한 뒤 wait_for_ctrl_irq(cpqphp.h:704)로 기다리는데, 그 함수는
 * ctrl->queue 대기 큐에 자기를 걸어 놓고 msleep_interruptible(1000)을
 * 부른 뒤 큐에서 빠지고, 시그널이 있었으면 -EINTR 을 돌려준다.
 * 한편 cpqhp_ctrl_intr 은 Serial Output 인터럽트를 받으면
 * wake_up_interruptible(&ctrl->queue)로 그 큐를 깨운다.
 *
 * 다만 **깨움이 실제로 그 1초를 줄여 주는지는 이 트리에서 확인할 수
 * 없다.** msleep_interruptible 의 구현이 이 스파스 체크아웃에 없기
 * 때문이다(kernel/ 이 없다). 코드에서 확실히 읽히는 것은 두 가지뿐이다 --
 * 대기 큐 등록과 깨움이 짝을 이루고 있다는 것, 그리고 그 함수가
 * wait_event 계열이 아니라 무조건 msleep 을 부르는 형태라는 것이다.
 * 이 파일의 여러 함수가 LED 하나 바꿀 때마다 이 대기를 하므로,
 * 카드 하나를 켜는 데 초 단위가 걸리는 이유가 여기 있다.
 *
 * === 자원 할당기를 직접 굴리는 이유 ===
 * 이 파일은 IO, 메모리, prefetchable 메모리, 버스 번호 네 종류마다
 * struct pci_resource 단일 연결 리스트를 자유 목록으로 들고 있다.
 * 그 초기값은 cpqphp_pci.c 의 cpqhp_find_available_resources 가 시스템
 * ROM 안의 Hot Plug Resource Table(HRT)에서 읽어 온다.
 *
 * 왜 PCI 코어의 할당기를 쓰지 않는가에 대해 코드는 이유를 적어 두지
 * 않았다. 다만 코드에서 읽히는 제약이 몇 가지 있다.
 *   BIOS 가 미리 정해 준 범위 안에서만 나눠 써야 한다 -- HRT 는 펌웨어가
 *     "이 슬롯에는 이만큼 써도 된다" 고 남긴 표이고, 이 드라이버는 그
 *     범위를 넘어서지 않는다.
 *   브리지 뒤로 재귀해 내려가며 창을 정렬해 잘라 줘야 한다
 *     (do_pre_bridge_resource_split, do_bridge_resource_split). 브리지의
 *     IO 창은 4KiB, 메모리 창은 1MiB 단위로 정렬되어야 하는데, 남는
 *     부분을 다시 부모 풀로 돌려주는 처리가 필요하다.
 *   실패하면 이미 나눠 준 것을 전부 회수해야 한다
 *     (cpqhp_return_board_resources, free_and_out 경로).
 * 2001년 당시 PCI 코어에 핫플러그용 재할당 경로가 지금과 같은 형태로
 * 있었는지는 이 트리만으로 판단할 수 없어, 그 부분은 단정하지 않는다.
 *
 * 할당기의 동작은 단순하다. 요청이 오면 목록을 크기순으로 정렬한 뒤
 * (sort_by_size 또는 sort_by_max_size) 조건에 맞는 첫 노드를 찾고,
 * 정렬이 안 맞거나 너무 크면 kmalloc 으로 노드를 새로 만들어 쪼갠다.
 * 반납할 때는 return_resource 로 머리에 도로 매달고,
 * cpqhp_resource_sort_and_combine 이 주소순으로 정렬하며 인접한 조각을
 * 합친다. 정렬이 전부 버블 정렬이라 O(n^2)이지만, 노드가 수십 개를
 * 넘지 않으므로 문제가 되지 않는다.
 *
 * === 2000년대 초 관용구에 대하여 ===
 * 지금 기준으로 낯선 것들이 있어 미리 적어 둔다. 아래 주석에서 그때마다
 * 다시 짚는다.
 *   자체 로그 매크로 : dbg/err/info/warn 이 cpqphp.h:25~28 에 printk 로
 *     정의되어 있다. dbg 는 전역 cpqhp_debug(cpqphp_core.c:36)가 참일
 *     때만 찍는다. dev_dbg 계열이나 pr_fmt 관용구가 자리 잡기 전이다.
 *   전역 상태 : cpqhp_slot_list[256] 배열과 전역 스레드 하나가 시스템
 *     전체를 대표한다. 컨트롤러가 여럿이어도 같은 배열과 같은 스레드를
 *     쓴다.
 *   바쁜 대기와 긴 지연 : mdelay(1100) 같은 밀리초 단위 바쁜 대기가
 *     set_controller_speed 에 있다. long_delay 는 그것을 jiffies 로
 *     받아 msleep_interruptible 로 넘기는 얇은 껍데기이며, 그 위의
 *     원문 주석이 "심심한 사람은 호출자를 전부 msleep_interruptible 로
 *     고쳐 달라" 고 적어 두었다.
 *   FIXME 와 죽은 코드 : handle_power_fault 안에 simulated_NMI 와
 *     panic 호출이 주석 처리된 채 남아 있고, is_bridge 위에는 "이대로는
 *     동작하지 않을 것 같다" 는 메모가 있다. 원문 그대로 보존한다.
 *   헝가리안 표기와 대문자 지역 변수 : taskInfo, DevError, FirstSupported,
 *     hold_IO_node 처럼 지금의 커널 스타일과 어긋나는 이름이 많다.
 *
 * === 값의 근거에 대하여 ===
 * 확인할 수 없는 것을 단정하지 않기 위해 아래 원칙으로 적었다.
 *   이 스파스 체크아웃에는 arch/ 와 include/asm 이 없다. 그래서
 *     pcibios_set_irq_routing, inb/outb, asm/pci_x86.h 에 기대는 코드는
 *     쓰임새로만 설명한다.
 *   kmalloc_obj 와 kzalloc_obj 매크로의 정의도 이 트리에 없다. 인자
 *     형태(*변수)로 보아 그 변수가 가리키는 타입 크기만큼 할당하는
 *     도우미이나, 값을 0 으로 미는지 여부는 이름으로만 구분해 적는다.
 *   컨트롤러 레지스터의 비트 의미는 cpqphp.h 의 enum ctrl_offsets 와
 *     인라인 함수들이 유일한 근거다. 공개 문서가 트리에 없으므로,
 *     각 비트가 어떻게 쓰이는지(어느 함수가 세우고 지우는지)로만 설명한다.
 *   0x0157 이나 0x07 같은 설정값은 위의 원문 주석이 어떤 플래그의
 *     합인지 적어 두었으므로 그 범위에서만 옮긴다.
 *
 * === NVMe 관점 ===
 * drivers/nvme 는 이 파일의 어떤 심볼도 부르지 않는다(이 트리에서 전수
 * 확인: 0건). 시대가 맞지 않는다 -- 이 드라이버는 2001년 PCI/PCI-X
 * 서버용이고, 최고 속도가 PCI-X 133MHz 다(set_controller_speed 참조).
 * NVMe SSD 가 쓰는 PCIe 는 이 하드웨어에 존재하지 않는다.
 *
 * 다만 핫플러그라는 문제 자체는 이어진다. "카드를 뽑았다 끼웠을 때 자원을
 * 어떻게 회수하고 다시 나눠 줄 것인가" 는 지금 NVMe 드라이브를 핫스왑할
 * 때도 같은 문제이며, 그 답이 이 파일의 자유 목록에서 PCI 코어의
 * 자원 재할당으로 옮겨 갔을 뿐이다. 이 파일을 읽는 값어치는 그 이전
 * 형태를 볼 수 있다는 데 있다.
 */

/* [한국어] MODULE_ 매크로 계열. 이 파일 자체는 모듈 메타데이터를 선언하지
 * 않지만(그것은 cpqphp_core.c 가 한다) 관련 타입을 위해 포함한다 */
#include <linux/module.h>
/* [한국어] printk, container_of 등 기본 매크로. cpqphp.h 의 dbg/err/info 매크로가
 * printk 로 정의되어 있어 이 헤더에 의존한다 */
#include <linux/kernel.h>
/* [한국어] u8, u16, u32 등 커널 정수 타입. 이 파일은 하드웨어 레지스터를 다루므로
 * 폭이 고정된 타입만 쓴다 */
#include <linux/types.h>
/* [한국어] kmalloc, kzalloc, kfree. **자유 목록의 노드를 쪼갤 때마다 새 노드를
 * 할당한다** -- get_resource 계열이 그 일을 한다 */
#include <linux/slab.h>
/* [한국어] struct work_struct. struct controller 에 int_task_event 필드가 있어
 * 타입이 필요하다. **다만 이 파일은 워크큐를 쓰지 않는다** --
 * 전역 kthread 를 쓴다. 뒷세대 shpchp 가 워크큐로 옮겨 간 것과 대비된다 */
#include <linux/workqueue.h>
/* [한국어] irqreturn_t 와 IRQ_HANDLED/IRQ_NONE. cpqhp_ctrl_intr 의 반환 타입이다 */
#include <linux/interrupt.h>
/* [한국어] msleep_interruptible 과 mdelay. long_delay 가 앞의 것을,
 * set_controller_speed 가 뒤의 것을 쓴다 -- **1100ms 바쁜 대기가
 * 이 파일에서 가장 눈에 띄는 옛 관용구다** */
#include <linux/delay.h>
/* [한국어] DECLARE_WAITQUEUE 와 wake_up_interruptible. 컨트롤러의 대기 큐
 * (ctrl->queue)로 명령 완료를 기다리는 데 쓴다.
 * 실제 대기는 cpqphp.h:704 의 wait_for_ctrl_irq 가 한다 */
#include <linux/wait.h>
/* [한국어] struct pci_bus, pci_bus_read_config_ 계열, PCI_DEVFN, PCI_HEADER_TYPE_
 * 계열 상수. 이 파일이 설정공간을 직접 두드리므로 필수다 */
#include <linux/pci.h>
/* [한국어] 핫플러그 코어의 타입들. struct hotplug_slot 이 struct slot 안에
 * 박혀 있어 필요하다 */
#include <linux/pci_hotplug.h>
/* [한국어] kthread_run, kthread_stop, kthread_should_stop.
 * **전역 이벤트 스레드 하나가 이 헤더에 의존한다** */
#include <linux/kthread.h>
/* [한국어] 이 드라이버의 공용 헤더. 자료구조 전부(struct controller, slot,
 * pci_func, pci_resource)와 레지스터를 두드리는 인라인 함수들
 * (set_SOGO:470, wait_for_ctrl_irq:704, LED 제어, 슬롯 전원),
 * 로그 매크로(:25~28), 오류 코드(:366~375)가 여기서 온다.
 * **아직 주석되지 않은 파일이다** */
#include "cpqphp.h"

/* [한국어] configure_new_device 의 전방 선언. **정의는 파일 끝에 있고 그 앞에
 * 블록 주석이 붙어 있다.** 여기 선언이 필요한 이유는
 * configure_new_function 과 **서로를 부르는 상호 재귀** 이기 때문이다 --
 * 둘 중 어느 쪽을 먼저 정의해도 다른 쪽을 미리 알려야 한다 */
static u32 configure_new_device(struct controller *ctrl, struct pci_func *func,
			u8 behind_bridge, struct resource_lists *resources);
/* [한국어] configure_new_function 의 전방 선언. 위와 같은 이유다 */
static int configure_new_function(struct controller *ctrl, struct pci_func *func,
			u8 behind_bridge, struct resource_lists *resources);
/* [한국어] interrupt_event_handler 의 전방 선언. event_thread 가 이것을 부르는데
 * 정의는 그보다 아래에 있어 미리 알려야 한다.
 * 상호 재귀는 아니고 단순한 순서 문제다 */
static void interrupt_event_handler(struct controller *ctrl);


/* [한국어] **전역 이벤트 스레드 하나.** 시스템에 컨트롤러가 여럿이어도 이
 * 스레드 하나가 전부를 담당한다.
 * 설정자: cpqhp_event_start_thread 가 kthread_run 으로 만든다.
 * 읽는 자: cpqhp_ctrl_intr 과 pushbutton_helper_thread 가
 *   wake_up_process 로 깨우고, cpqhp_event_stop_thread 가 멈춘다.
 * 값 범위: 유효한 포인터, 또는 kthread_run 이 실패했으면 오류 포인터.
 * **뒷세대와의 대비**: shpchp 는 슬롯마다 워크큐를, pciehp 는
 *   request_threaded_irq 의 IRQ 스레드를 쓴다. 전역 하나로 모든 것을
 *   담당하는 이 방식이 가장 오래된 형태다.
 * 동기화: 없다. 접근이 전부 wake_up_process 와 kthread_stop 이라
 *   커널이 내부적으로 처리한다 */
static struct task_struct *cpqhp_event_thread;
/* [한국어] **타이머와 이벤트 스레드 사이의 유일한 통로.**
 * 설정자: pushbutton_helper_thread 가 만료된 타이머 포인터를 넣는다.
 * 읽는 자: event_thread 가 NULL 이 아니면 버튼 처리로 분기하고,
 *   cpqhp_pushbutton_thread 가 맨 먼저 NULL 로 되돌린다.
 * 값 범위: NULL 이거나 유효한 타이머 포인터. 오른쪽 원문 주석이 초기값이 NULL 임을 밝힌다 -- static 이라 자동으로 0 이다.
 * **한계**: 전역이 하나뿐이라 슬롯 여러 개의 타이머가 거의 동시에
 *   만료되면 나중 것이 앞의 것을 덮어쓴다.
 * 동기화: **없다.** 타이머(소프트 인터럽트)와 스레드가 락 없이
 *   주고받는다. 뒷세대 pciehp 가 같은 자리에 원자적 비트마스크를 쓰는
 *   것과 대비된다 */
static struct timer_list *pushbutton_pending;	/* = NULL */

/* delay is in jiffies to wait for */
/* [한국어]
 * long_delay - jiffies 단위로 받은 시간만큼 잠든다
 *
 * @delay: 기다릴 시간. **jiffies 단위** 다.
 * @return: 없음.
 *
 * 위의 원문 주석이 이 함수의 처지를 그대로 밝힌다 -- "심심한 사람은
 * 호출자를 전부 msleep_interruptible 로 고쳐 달라, 그쪽이 자연스러운
 * 단위(밀리초)로 시간을 지정하려는 것을 굳이 jiffies 로 바꾸느라
 * 애를 쓰고 있다" 는 것이다.
 *
 * 즉 이 함수는 **단위 변환 껍데기** 다. 호출자들이 1*HZ, (2*HZ)/10 처럼
 * jiffies 로 시간을 쓰고 있어서, 그것을 다시 밀리초로 되돌려
 * msleep_interruptible 에 넘긴다. HZ 로 곱했다가 나누는 셈이라
 * 정밀도만 잃는다.
 *
 * **반환값을 버린다.** msleep_interruptible 은 시그널로 일찍 깨면 남은
 * 시간을 돌려주는데, 이 함수는 그것을 무시하므로 호출자는 실제로 잤는지
 * 중간에 깼는지 알 수 없다.
 *
 * 부르는 곳은 카드 전원을 넣은 뒤 1초를 기다리는 자리
 * (board_added, board_replaced)와 LED 시험 패턴 사이의 간격
 * (switch_leds, cpqhp_hardware_test)이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 잠들므로 인터럽트 컨텍스트에서
 * 부르면 안 된다.
 *
 * 호출 체인:
 *   board_added / board_replaced / switch_leds / cpqhp_hardware_test
 *     → [이 함수] → msleep_interruptible
 */
static void long_delay(int delay)
{
	/*
	 * XXX(hch): if someone is bored please convert all callers
	 * to call msleep_interruptible directly.  They really want
	 * to specify timeouts in natural units and spend a lot of
	 * effort converting them to jiffies..
	 */
	msleep_interruptible(jiffies_to_msecs(delay));
}


/* FIXME: The following line needs to be somewhere else... */
/* [한국어] **엉뚱한 자리에 있는 상수다.** 바로 위의 원문 주석이 그 사실을
 * 밝힌다 -- "이 줄은 다른 곳에 있어야 한다".
 * 다른 오류 코드들은 cpqphp.h:366~375 에 모여 있는데 이것만 여기 있다.
 * 읽는 자: board_added 와 board_replaced 가 set_controller_speed 가
 *   실패했을 때 이 값을 rc 에 넣는다.
 * 값 0x07 이 cpqphp.h 의 다른 코드들과 겹치지 않는지는 확인해 두는
 *   편이 좋은데, 그 파일의 0x03(ADD_NOT_SUPPORTED, REMOVE_NOT_SUPPORTED)
 *   이 이미 둘로 겹쳐 쓰이고 있어 이 드라이버의 코드 공간이 엄밀하지
 *   않음을 보여 준다 */
#define WRONG_BUS_FREQUENCY 0x07
/* [한국어]
 * handle_switch_change - 슬롯 레버(스위치) 변화를 이벤트 큐에 적재한다
 *
 * @change: 변화한 슬롯의 비트마스크. 비트 하나가 슬롯 하나다.
 * @ctrl:   해당 컨트롤러.
 * @return: 적재한 이벤트 개수. 호출자가 스레드를 깨울지 정하는 데 쓴다.
 *
 * 슬롯의 물리적 레버가 열리거나 닫혔을 때 불린다. **인터럽트 컨텍스트라
 * 분류와 적재만 하고 실제 일은 하지 않는다** -- LED 를 켜거나 전원을
 * 넣는 것은 나중에 커널 스레드가 한다.
 *
 * 슬롯 여섯 개를 훑는 이유: 이 컨트롤러의 최대 슬롯 수가 6 이다.
 * **상수 6 이 코드에 직접 박혀 있고** ctrl 에서 실제 슬롯 수를 읽어
 * 쓰지 않는다. 없는 슬롯의 비트는 어차피 서지 않으므로 결과는 같다.
 *
 * 이벤트 적재 방식이 이 드라이버의 특징이다.
 *   ctrl->event_queue[ctrl->next_event] 에 쓰고
 *   next_event 를 10 으로 나눈 나머지로 증가시킨다.
 * **가득 찼는지 확인하지 않는다.** 스레드가 미처 비우기 전에 열한 번째
 * 사건이 오면 처리되지 않은 첫 번째를 덮어쓴다. 뒷세대 shpchp 가 사건마다
 * 메모리를 새로 할당하고, pciehp 가 비트마스크로 합치는 것과 대비된다.
 *
 * 존재 여부(presence_save)를 두 비트로 저장하는 계산에 주의:
 * 상태 레지스터 상위 16비트에서 hp_slot 자리와 hp_slot+7 자리를 각각
 * 뽑아 0x01 과 0x02 자리에 놓는다. 이 컨트롤러가 카드의 존재를 핀 두 개로
 * 감지하기 때문으로 보이나, 그 배선의 근거 문서는 이 트리에 없다.
 *
 * **func 를 NULL 검사 없이 역참조한다.** cpqhp_slot_find 는 못 찾으면
 * NULL 을 돌려주는데 곧바로 func->presence_save 에 쓴다. 같은 파일의
 * interrupt_event_handler 는 같은 호출 뒤에 NULL 을 검사하므로,
 * 두 곳의 방어 수준이 다르다. 코드는 손대지 않고 사실만 적어 둔다.
 *
 * 실행 컨텍스트: 하드 인터럽트. 잠들 수 없다.
 *
 * 호출 체인:
 *   cpqhp_ctrl_intr → [이 함수] → cpqhp_slot_find
 */
static u8 handle_switch_change(u8 change, struct controller *ctrl)
{
	int hp_slot;
	/* [한국어] 적재한 이벤트 개수. 호출자가 스레드를 깨울지 정하는 데 쓴다 */
	u8 rc = 0;
	/* [한국어] 비트 계산에 쓸 임시 변수 */
	u16 temp_word;
	/* [한국어] 이 슬롯의 함수 노드 */
	struct pci_func *func;
	/* [한국어] 이벤트 큐의 빈 칸을 가리킬 포인터 */
	struct event_info *taskInfo;

	/* [한국어] 변화가 없으면 할 일이 없다 */
	if (!change)
		return 0;

	/* Switch Change */
	dbg("cpqsbd:  Switch interrupt received.\n");

	/* [한국어] **슬롯 여섯 개를 훑는다** -- 이 컨트롤러의 최대 슬롯 수다 */
	for (hp_slot = 0; hp_slot < 6; hp_slot++) {
		if (change & (0x1L << hp_slot)) {
			/*
			 * this one changed.
			 */
			func = cpqhp_slot_find(ctrl->bus,
				(hp_slot + ctrl->slot_device_offset), 0);

			/* this is the structure that tells the worker thread
			 * what to do
			 */
			taskInfo = &(ctrl->event_queue[ctrl->next_event]);
			/* [한국어] **링 버퍼의 다음 자리로 넘어간다.** 10 으로 나눈 나머지를 쓰므로
			 * 열 칸을 돌려 쓴다 */
			ctrl->next_event = (ctrl->next_event + 1) % 10;
			/* [한국어] 어느 슬롯의 사건인지 기록한다 */
			taskInfo->hp_slot = hp_slot;

			/* [한국어] 적재한 개수를 센다 */
			rc++;

			/* [한국어] **상태 레지스터의 상위 16비트에 존재 정보가 있다** */
			temp_word = ctrl->ctrl_int_comp >> 16;
			/* [한국어] 첫 번째 존재 감지 비트를 뽑는다 */
			func->presence_save = (temp_word >> hp_slot) & 0x01;
			/* [한국어] **두 번째 존재 감지 비트를 더한다.** hp_slot + 7 자리를 쓰는 것은
			 * 이 컨트롤러가 카드 존재를 핀 두 개로 감지하기 때문으로 보이나,
			 * 그 배선의 근거 문서는 이 트리에 없다 */
			func->presence_save |= (temp_word >> (hp_slot + 7)) & 0x02;

			if (ctrl->ctrl_int_comp & (0x1L << hp_slot)) {
				/*
				 * Switch opened
				 */

				/* [한국어] **레버가 열렸다** -- 0 을 저장해 둔다 */
				func->switch_save = 0;

				/* [한국어] 레버가 열렸음을 이벤트로 남긴다 */
				taskInfo->event_type = INT_SWITCH_OPEN;
			} else {
				/*
				 * Switch closed
				 */

				/* [한국어] **레버가 닫혔다** -- 0x10 을 저장해 둔다 */
				func->switch_save = 0x10;

				/* [한국어] 레버가 닫혔음을 이벤트로 남긴다 */
				taskInfo->event_type = INT_SWITCH_CLOSE;
			}
		}
	}

	/* [한국어] 적재한 개수를 돌려준다 */
	return rc;
}

/**
 * cpqhp_find_slot - find the struct slot of given device
 * @ctrl: scan lots of this controller
 * @device: the device id to find
 */
/* [한국어]
 * cpqhp_find_slot - 장치 번호로 struct slot 을 찾는다
 *
 * @ctrl:   훑을 컨트롤러.
 * @device: 찾을 장치 번호.
 * @return: 찾은 슬롯, 없으면 NULL.
 *
 * 컨트롤러의 슬롯 연결 리스트를 처음부터 훑는다. 세 줄짜리 선형 탐색이다.
 *
 * **struct slot 과 struct pci_func 을 구별해야 이 파일이 읽힌다.**
 *   struct slot     : 사용자에게 보이는 물리적 슬롯. LED 상태, 5초 타이머,
 *     상태 기계의 state 가 여기 있다. ctrl->slot 리스트에 달린다.
 *   struct pci_func : PCI 버스/장치/함수 하나. 설정공간 사본과 자원 목록이
 *     여기 있다. 전역 cpqhp_slot_list[bus] 리스트에 달린다.
 * 같은 슬롯을 두 관점으로 보는 것이며, 이 함수는 앞쪽을,
 * cpqhp_slot_find 는 뒤쪽을 찾는다. **이름이 비슷해 헷갈리기 쉽다.**
 *
 * 슬롯이 여섯 개를 넘지 않아 선형 탐색으로 충분하다.
 *
 * 실행 컨텍스트: 어디서나. 리스트를 읽기만 하고 락을 잡지 않는다 --
 * 슬롯 리스트는 probe 때 만들어진 뒤 바뀌지 않기 때문이다.
 *
 * 호출 체인:
 *   handle_presence_change / interrupt_event_handler /
 *   set_controller_speed / board_added / cpqhp_process_SI / _SS → [이 함수]
 */
static struct slot *cpqhp_find_slot(struct controller *ctrl, u8 device)
{
	struct slot *slot = ctrl->slot;

	/* [한국어] **장치 번호가 맞는 슬롯을 찾을 때까지 훑는다.** 슬롯이 여섯 개를
	 * 넘지 않아 선형 탐색으로 충분하다 */
	while (slot && (slot->device != device))
		/* [한국어] 다음 슬롯으로 넘어간다 */
		slot = slot->next;

	/* [한국어] 찾은 슬롯을 돌려준다. 못 찾았으면 NULL 이다 */
	return slot;
}


/* [한국어]
 * handle_presence_change - 카드 존재 변화 또는 버튼 누름을 이벤트 큐에 적재한다
 *
 * @change: 변화한 비트마스크. **하위와 상위 바이트를 함께 본다**
 *   (0x0101 << hp_slot) -- 존재 감지 핀이 둘이기 때문이다.
 * @ctrl:   해당 컨트롤러.
 * @return: 적재한 이벤트 개수.
 *
 * 이름은 "존재 변화" 지만 **실제로는 두 가지를 가른다.** 레버가 닫혀 있고
 * 컨트롤러가 버튼 모드이면 버튼 누름으로, 그렇지 않으면 카드가 꽂히거나
 * 빠진 것으로 해석한다. 하드웨어가 같은 신호선을 두 용도로 쓰기 때문이다.
 *
 * 버튼 경로 (func->switch_save 가 참이고 ctrl->push_button 이 1):
 *   지금 읽은 존재 상태를 저장해 둔 값과 견준다.
 *     다르면 → 버튼을 **누른** 것. 위의 원문 주석대로 아무 일도 하지 않는다.
 *     같으면 → 버튼을 **뗀** 것. 여기서 실제 동작이 결정된다.
 *   뗀 경우에도 슬롯의 현재 상태에 따라 세 갈래로 갈린다.
 *     깜빡이는 중(BLINKINGON/BLINKINGOFF)이면 → INT_BUTTON_CANCEL.
 *       5초 안에 버튼을 다시 누르면 취소된다는 뜻이며, 사용자가 마음을
 *       바꿀 시간을 주는 설계다.
 *     전원 조작 중(POWERON/POWEROFF)이면 → INT_BUTTON_IGNORE.
 *       이미 일이 진행 중이라 무시한다.
 *     그 밖(STATIC)이면 → INT_BUTTON_RELEASE. 실제로 켜거나 끈다.
 *
 * 존재 경로 (레버가 열려 있음):
 *   존재 상태를 저장하고, 두 감지 비트 중 하나라도 0 이면 카드가 있는
 *   것으로 본다(INT_PRESENCE_ON). 둘 다 1 이면 없는 것이다 --
 *   **0 이 "있음" 인 음논리** 다.
 *
 * 주의할 점 둘:
 *   p_slot 은 NULL 검사를 하는데 **func 는 하지 않는다.** 그리고 NULL 이면
 *     return 0 을 하므로, 그 시점까지 적재한 이벤트가 있어도 개수를
 *     0 으로 보고해 스레드가 깨어나지 않을 수 있다.
 *   슬롯 번호를 구할 때 SLOT_MASK 레지스터를 읽어 상위 니블을 쓴다.
 *     ctrl->slot_device_offset 을 쓰는 다른 곳들과 방식이 다른데,
 *     두 값이 같은 것을 가리키는지는 이 트리에서 확인할 수 없다.
 *
 * 실행 컨텍스트: 하드 인터럽트.
 *
 * 호출 체인:
 *   cpqhp_ctrl_intr → [이 함수] → cpqhp_slot_find, cpqhp_find_slot
 */
static u8 handle_presence_change(u16 change, struct controller *ctrl)
{
	int hp_slot;
	/* [한국어] 적재한 이벤트 개수 */
	u8 rc = 0;
	/* [한국어] 비트 계산에 쓸 임시 변수 */
	u8 temp_byte;
	/* [한국어] 존재 상태를 담을 변수 */
	u16 temp_word;
	/* [한국어] 이 슬롯의 함수 노드 */
	struct pci_func *func;
	/* [한국어] 이벤트 큐의 빈 칸을 가리킬 포인터 */
	struct event_info *taskInfo;
	/* [한국어] 사용자에게 보이는 슬롯 */
	struct slot *p_slot;

	/* [한국어] 변화가 없으면 할 일이 없다 */
	if (!change)
		return 0;

	/*
	 * Presence Change
	 */
	dbg("cpqsbd:  Presence/Notify input change.\n");
	/* [한국어] 어느 비트가 변했는지 로그로 남긴다 */
	dbg("         Changed bits are 0x%4.4x\n", change);

	/* [한국어] 슬롯 여섯 개를 훑는다 */
	for (hp_slot = 0; hp_slot < 6; hp_slot++) {
		if (change & (0x0101 << hp_slot)) {
			/*
			 * this one changed.
			 */
			func = cpqhp_slot_find(ctrl->bus,
				(hp_slot + ctrl->slot_device_offset), 0);

			/* [한국어] 이벤트 큐의 다음 빈 칸을 잡는다 */
			taskInfo = &(ctrl->event_queue[ctrl->next_event]);
			/* [한국어] 링 버퍼의 다음 자리로 넘어간다 */
			ctrl->next_event = (ctrl->next_event + 1) % 10;
			/* [한국어] 어느 슬롯의 사건인지 기록한다 */
			taskInfo->hp_slot = hp_slot;

			/* [한국어] 적재한 개수를 센다 */
			rc++;

			/* [한국어] **슬롯 번호를 SLOT_MASK 의 상위 니블로 구한다.**
			 * ctrl->slot_device_offset 을 쓰는 다른 곳들과 방식이 다른데,
			 * 두 값이 같은 것을 가리키는지는 이 트리에서 확인할 수 없다 */
			p_slot = cpqhp_find_slot(ctrl, hp_slot + (readb(ctrl->hpc_reg + SLOT_MASK) >> 4));
			/* [한국어] **슬롯을 못 찾으면 0 을 돌려준다.** 그때까지 적재한 이벤트가
			 * 있어도 개수를 0 으로 보고해 스레드가 깨어나지 않을 수 있다 */
			if (!p_slot)
				return 0;

			/* If the switch closed, must be a button
			 * If not in button mode, nevermind
			 */
			if (func->switch_save && (ctrl->push_button == 1)) {
				/* [한국어] 현재 존재 상태를 읽는다 */
				temp_word = ctrl->ctrl_int_comp >> 16;
				/* [한국어] 첫 번째 존재 감지 비트를 뽑는다 */
				temp_byte = (temp_word >> hp_slot) & 0x01;
				/* [한국어] 두 번째 존재 감지 비트를 더한다 */
				temp_byte |= (temp_word >> (hp_slot + 7)) & 0x02;

				if (temp_byte != func->presence_save) {
					/*
					 * button Pressed (doesn't do anything)
					 */
					dbg("hp_slot %d button pressed\n", hp_slot);
					/* [한국어] **버튼을 눌렀다.** 위의 원문 주석대로 아무 일도 하지 않는다 */
					taskInfo->event_type = INT_BUTTON_PRESS;
				} else {
					/*
					 * button Released - TAKE ACTION!!!!
					 */
					dbg("hp_slot %d button released\n", hp_slot);
					/* [한국어] **버튼을 뗐다 -- 여기서 실제 동작이 결정된다** */
					taskInfo->event_type = INT_BUTTON_RELEASE;

					/* Cancel if we are still blinking */
					if ((p_slot->state == BLINKINGON_STATE)
					    /* [한국어] 끄려던 중인지도 확인한다 */
					    || (p_slot->state == BLINKINGOFF_STATE)) {
						/* [한국어] **깜빡이는 중이었으면 취소로 바꾼다** -- 5초 안에 버튼을 다시
						 * 누른 경우다 */
						taskInfo->event_type = INT_BUTTON_CANCEL;
						/* [한국어] 취소를 로그로 남긴다 */
						dbg("hp_slot %d button cancel\n", hp_slot);
					/* [한국어] **전원 조작이 이미 진행 중이면 무시한다** */
					} else if ((p_slot->state == POWERON_STATE)
						   || (p_slot->state == POWEROFF_STATE)) {
						/* info(msg_button_ignore, p_slot->number); */
						taskInfo->event_type = INT_BUTTON_IGNORE;
						/* [한국어] 무시함을 로그로 남긴다 */
						dbg("hp_slot %d button ignore\n", hp_slot);
					}
				}
			} else {
				/* Switch is open, assume a presence change
				 * Save the presence state
				 */
				temp_word = ctrl->ctrl_int_comp >> 16;
				/* [한국어] **존재 상태를 저장해 둔다.** 다음 인터럽트가 이 값과 견줘
				 * 버튼인지 존재 변화인지 가린다 */
				func->presence_save = (temp_word >> hp_slot) & 0x01;
				/* [한국어] 두 번째 존재 감지 비트를 더한다 */
				func->presence_save |= (temp_word >> (hp_slot + 7)) & 0x02;

				/* [한국어] **두 감지 비트 중 하나라도 0 이면 카드가 있는 것으로 본다** --
				 * 0 이 "있음" 인 음논리다 */
				if ((!(ctrl->ctrl_int_comp & (0x010000 << hp_slot))) ||
				    (!(ctrl->ctrl_int_comp & (0x01000000 << hp_slot)))) {
					/* Present */
					taskInfo->event_type = INT_PRESENCE_ON;
				} else {
					/* Not Present */
					taskInfo->event_type = INT_PRESENCE_OFF;
				}
			}
		}
	}

	/* [한국어] 적재한 개수를 돌려준다 */
	return rc;
}


/* [한국어]
 * handle_power_fault - 전원 결함 발생·해제를 이벤트 큐에 적재한다
 *
 * @change: 변화한 슬롯의 비트마스크.
 * @ctrl:   해당 컨트롤러.
 * @return: 적재한 이벤트 개수.
 *
 * 슬롯의 전원 회로가 이상을 감지했을 때 불린다.
 *
 * **컨트롤러 리비전에 따라 대응이 갈리는 것이 이 함수의 요점이다.**
 *   rev < 4 : 위의 원문 주석이 밝히듯 **치명적 상황으로 보고 기계를
 *     멈추려 했다.** 데이터 손상을 막기 위해 simulated_NMI 를 걸고,
 *     그것이 돌아오면 panic 을 부르는 코드가 있었다. 지금은 둘 다
 *     FIXME 와 함께 주석 처리되어 있어, 실제로는 LED 만 바꾸고 지나간다.
 *   rev >= 4 : func->status 에 0xFF 를 표시하고 로그만 남긴다.
 *     board_added 와 board_replaced 가 카드를 켠 뒤 그 값을 확인해
 *     POWER_FAILURE 로 처리한다 -- **status 가 인터럽트와 스레드 사이의
 *     통로 노릇을 한다.**
 *
 * 즉 같은 사건을 옛 하드웨어는 "즉시 기계를 세울 일" 로, 새 하드웨어는
 * "그 슬롯만 포기할 일" 로 다룬다. 하드웨어가 개별 슬롯을 안전하게
 * 차단할 수 있게 되면서 대응 수위가 낮아진 셈이다.
 *
 * 결함이 해제된 경우(INT_POWER_FAULT_CLEAR)에는 status 를 0 으로 되돌린다.
 *
 * 여기서도 func 를 NULL 검사 없이 역참조한다.
 *
 * info() 로 로그를 남기는데, dbg 와 달리 cpqhp_debug 와 무관하게 항상
 * 찍힌다. 전원 결함은 사용자가 반드시 알아야 할 사건이기 때문이다.
 *
 * 실행 컨텍스트: 하드 인터럽트.
 *
 * 호출 체인:
 *   cpqhp_ctrl_intr → [이 함수] → cpqhp_slot_find, amber_LED_on,
 *     green_LED_off, set_SOGO
 */
static u8 handle_power_fault(u8 change, struct controller *ctrl)
{
	int hp_slot;
	/* [한국어] 적재한 이벤트 개수. 호출자가 스레드를 깨울지 정하는 데 쓴다 */
	u8 rc = 0;
	/* [한국어] 이 슬롯의 함수 노드 */
	struct pci_func *func;
	/* [한국어] 이벤트 큐의 빈 칸을 가리킬 포인터 */
	struct event_info *taskInfo;

	/* [한국어] 변화가 없으면 할 일이 없다 */
	if (!change)
		return 0;

	/*
	 * power fault
	 */

	/* [한국어] **전원 결함을 알린다.** dbg 가 아니라 info 라 항상 찍힌다 --
	 * 사용자가 반드시 알아야 할 사건이기 때문이다 */
	info("power fault interrupt\n");

	/* [한국어] **슬롯 여섯 개를 훑는다.** 상수 6 이 직접 박혀 있고 실제 슬롯 수를
	 * 읽어 쓰지 않는다 -- 없는 슬롯의 비트는 서지 않으므로 결과는 같다 */
	for (hp_slot = 0; hp_slot < 6; hp_slot++) {
		if (change & (0x01 << hp_slot)) {
			/*
			 * this one changed.
			 */
			func = cpqhp_slot_find(ctrl->bus,
				(hp_slot + ctrl->slot_device_offset), 0);

			/* [한국어] **이벤트 큐의 다음 빈 칸을 잡는다** */
			taskInfo = &(ctrl->event_queue[ctrl->next_event]);
			/* [한국어] **링 버퍼의 다음 자리로 넘어간다.** 가득 찼는지 확인하지 않으므로
			 * 열한 번째 사건은 처리되지 않은 첫 번째를 덮어쓴다 */
			ctrl->next_event = (ctrl->next_event + 1) % 10;
			/* [한국어] 어느 슬롯의 사건인지 기록한다 */
			taskInfo->hp_slot = hp_slot;

			/* [한국어] 적재한 개수를 센다 */
			rc++;

			if (ctrl->ctrl_int_comp & (0x00000100 << hp_slot)) {
				/*
				 * power fault Cleared
				 */
				func->status = 0x00;

				/* [한국어] **결함이 해제되었다** -- 아래에서 status 도 0 으로 되돌린다 */
				taskInfo->event_type = INT_POWER_FAULT_CLEAR;
			} else {
				/*
				 * power fault
				 */
				taskInfo->event_type = INT_POWER_FAULT;

				/* [한국어] **리비전 4 미만이면 치명적 상황으로 다뤘다.** 위의 원문 주석대로
				 * 원래는 NMI 를 걸고 panic 하는 코드가 있었으나 지금은 주석 처리되어
				 * 있어, 실제로는 LED 만 바꾸고 지나간다 */
				if (ctrl->rev < 4) {
					/* [한국어] 황색을 켠다 -- 오류 표시다 */
					amber_LED_on(ctrl, hp_slot);
					/* [한국어] 녹색을 끈다 */
					green_LED_off(ctrl, hp_slot);
					set_SOGO(ctrl);

					/* this is a fatal condition, we want
					 * to crash the machine to protect from
					 * data corruption. simulated_NMI
					 * shouldn't ever return */
					/* FIXME
					simulated_NMI(hp_slot, ctrl); */

					/* The following code causes a software
					 * crash just in case simulated_NMI did
					 * return */
					/*FIXME
					panic(msg_power_fault); */
				} else {
					/* set power fault status for this board */
					func->status = 0xFF;
					/* [한국어] **전원 결함 비트를 세웠음을 알린다.** info 라 항상 찍힌다 */
					info("power fault bit %x set\n", hp_slot);
				}
			}
		}
	}

	/* [한국어] 적재한 개수를 돌려준다 */
	return rc;
}


/**
 * sort_by_size - sort nodes on the list by their length, smallest first.
 * @head: list to sort
 */
/* [한국어]
 * sort_by_size - 자유 목록을 길이 오름차순으로 정렬한다 (작은 것 먼저)
 *
 * @head: 정렬할 목록의 머리를 가리키는 포인터의 포인터.
 * @return: 목록이 비었으면 1, 그 밖에는 0.
 *
 * **버블 정렬** 이다. 교환이 한 번도 없을 때까지 목록을 반복해서 훑는다.
 * 단일 연결 리스트라 노드를 교환하려면 앞 노드의 next 를 고쳐야 하고,
 * 머리를 교환할 때는 head 포인터 자체를 바꿔야 해서 그 경우만 따로
 * 처리한다 -- 그래서 같은 모양의 코드가 두 벌 있다.
 *
 * O(n^2)이지만 문제가 되지 않는다. 이 목록의 노드 수는 시스템의 자유
 * 자원 조각 수이고, 실제로는 몇 개에서 수십 개 수준이다.
 *
 * **작은 것을 앞에 두는 이유** 는 호출자를 보면 드러난다.
 * get_resource 와 get_io_resource 가 이 함수로 정렬한 뒤 앞에서부터
 * 훑으며 "요청 크기 이상인 첫 노드" 를 고른다. 작은 것부터 보므로
 * **요청에 가장 근접한 조각** 을 고르게 되고, 큰 조각을 불필요하게
 * 쪼개는 일이 줄어든다. 이른바 최적 적합(best fit)에 해당한다.
 *
 * 반대로 sort_by_max_size 는 큰 것을 앞에 두며, 브리지 창처럼 큰 덩어리가
 * 필요한 get_max_resource 가 그것을 쓴다.
 *
 * 빈 목록에 1 을 돌려주는 것에 주의 -- 호출자들이 그것을 오류로 보고
 * NULL 을 반환한다. 원소가 하나뿐이면 이미 정렬된 것이므로 0 이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(자원 할당 경로).
 *
 * 호출 체인:
 *   get_io_resource / get_resource → [이 함수]
 */
static int sort_by_size(struct pci_resource **head)
{
	struct pci_resource *current_res;
	/* [한국어] 교환할 때 쓸 임시 포인터 */
	struct pci_resource *next_res;
	/* [한국어] 교환이 있었는지 표시하는 플래그. 1 로 시작해 루프에 진입한다 */
	int out_of_order = 1;

	/* [한국어] 목록이 비었는지 확인한다 */
	if (!(*head))
		/* [한국어] **빈 목록에 1 을 돌려준다** -- 호출자가 그것을 실패로 본다 */
		return 1;

	/* [한국어] 원소가 하나뿐이면 이미 정렬된 것이다 */
	if (!((*head)->next))
		return 0;

	/* [한국어] **교환이 한 번도 없을 때까지 반복한다** -- 버블 정렬이라 O(n^2)
	 * 이지만 노드가 몇 개에서 수십 개 수준이라 문제가 되지 않는다 */
	while (out_of_order) {
		/* [한국어] 한 바퀴 시작 전에 내려 둔다 */
		out_of_order = 0;

		/* Special case for swapping list head */
		if (((*head)->next) &&
		    ((*head)->length > (*head)->next->length)) {
			/* [한국어] 교환이 있었음을 표시한다 */
			out_of_order++;
			current_res = *head;
			*head = (*head)->next;
			current_res->next = (*head)->next;
			/* [한국어] 교환을 마무리한다 */
			(*head)->next = current_res;
		}

		/* [한국어] 머리부터 훑기 시작한다 */
		current_res = *head;

		/* [한국어] **세 번째 노드까지 있어야 중간을 교환할 수 있다** */
		while (current_res->next && current_res->next->next) {
			/* [한국어] **뒤가 더 크면 순서가 어긋난 것이다** -- 작은 것을 앞으로 보낸다 */
			if (current_res->next->length > current_res->next->next->length) {
				/* [한국어] 교환이 있었음을 표시한다 */
				out_of_order++;
				/* [한국어] 뒤 노드를 기억해 둔다 */
				next_res = current_res->next;
				/* [한국어] 교환 중 */
				current_res->next = current_res->next->next;
				/* [한국어] 교환 중 */
				current_res = current_res->next;
				/* [한국어] 교환 중 */
				next_res->next = current_res->next;
				/* [한국어] 교환 중 */
				current_res->next = next_res;
			/* [한국어] **세 노드의 연결을 다시 잇는다.** 단일 연결 리스트의 교환은
			 * 이렇게 네 줄이 필요하다 */
			} else
				/* [한국어] 이미 순서가 맞으면 다음으로 넘어간다 */
				current_res = current_res->next;
		}
	/* [한국어] 교환이 없을 때까지 반복했다 */
	}  /* End of out_of_order loop */

	return 0;
}


/**
 * sort_by_max_size - sort nodes on the list by their length, largest first.
 * @head: list to sort
 */
/* [한국어]
 * sort_by_max_size - 자유 목록을 길이 내림차순으로 정렬한다 (큰 것 먼저)
 *
 * @head: 정렬할 목록의 머리를 가리키는 포인터의 포인터.
 * @return: 목록이 비었으면 1, 그 밖에는 0.
 *
 * sort_by_size 와 **비교 부등호만 다르고 나머지가 완전히 같다.**
 * `>` 가 `<` 로 바뀐 두 자리를 빼면 글자까지 동일하다. 2000년대 초
 * 코드에서 흔한 복사·붙여넣기이며, 지금이라면 비교 함수를 인자로 받거나
 * list_sort 를 썼을 자리다.
 *
 * 큰 것을 앞에 두는 이유는 유일한 호출자인 get_max_resource 가
 * "조건을 만족하는 **가장 큰** 조각" 을 원하기 때문이다. 정렬해 두면
 * 앞에서부터 훑다가 처음 만나는 것이 곧 답이 된다.
 *
 * 그것이 필요한 곳은 브리지 설정이다. configure_new_function 이
 * 브리지를 만나면 그 뒤에 매달릴 장치들이 나눠 쓸 창을 통째로 잡아야
 * 하므로, 잘게 쪼개진 조각이 아니라 가장 큰 덩어리를 원한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(자원 할당 경로).
 *
 * 호출 체인:
 *   get_max_resource → [이 함수]
 */
static int sort_by_max_size(struct pci_resource **head)
{
	struct pci_resource *current_res;
	/* [한국어] 목록을 훑을 포인터 */
	struct pci_resource *next_res;
	/* [한국어] 교환할 때 쓸 임시 포인터 */
	int out_of_order = 1;

	/* [한국어] 목록이 비었는지 확인한다 */
	if (!(*head))
		/* [한국어] 빈 목록에 1 을 돌려준다 -- 호출자가 그것을 실패로 본다 */
		return 1;

	/* [한국어] 원소가 하나뿐이면 이미 정렬된 것이다 */
	if (!((*head)->next))
		return 0;

	/* [한국어] **교환이 한 번도 없을 때까지 반복한다** -- 버블 정렬이다 */
	while (out_of_order) {
		/* [한국어] 한 바퀴 시작 전에 내려 둔다 */
		out_of_order = 0;

		/* Special case for swapping list head */
		if (((*head)->next) &&
		    ((*head)->length < (*head)->next->length)) {
			/* [한국어] 교환이 있었음을 표시한다 */
			out_of_order++;
			current_res = *head;
			*head = (*head)->next;
			current_res->next = (*head)->next;
			/* [한국어] 교환을 마무리한다 */
			(*head)->next = current_res;
		}

		/* [한국어] 머리부터 훑기 시작한다 */
		current_res = *head;

		/* [한국어] **세 번째 노드까지 있어야 교환할 수 있다** */
		while (current_res->next && current_res->next->next) {
			/* [한국어] **부등호가 sort_by_size 와 반대다** -- 큰 것을 앞으로 보낸다 */
			if (current_res->next->length < current_res->next->next->length) {
				/* [한국어] 교환이 있었음을 표시한다 */
				out_of_order++;
				/* [한국어] 뒤 노드를 기억해 둔다 */
				next_res = current_res->next;
				/* [한국어] 교환 중 */
				current_res->next = current_res->next->next;
				/* [한국어] 교환 중 */
				current_res = current_res->next;
				/* [한국어] 교환 중 */
				next_res->next = current_res->next;
				/* [한국어] 교환의 마지막 연결 */
				current_res->next = next_res;
			} else
				/* [한국어] 이미 순서가 맞으면 다음으로 넘어간다 */
				current_res = current_res->next;
		}
	/* [한국어] 교환이 없을 때까지 반복했다 */
	}  /* End of out_of_order loop */

	return 0;
}


/**
 * do_pre_bridge_resource_split - find node of resources that are unused
 * @head: new list head
 * @orig_head: original list head
 * @alignment: max node size (?)
 */
/* [한국어]
 * do_pre_bridge_resource_split - 브리지 창 **앞쪽** 의 남는 부분을 떼어 낸다
 *
 * @head:      브리지 뒤에서 실제로 쓰고 남은 자유 목록.
 * @orig_head: 브리지에게 원래 준 덩어리.
 * @alignment: 정렬 단위. IO 는 0x1000, 메모리는 0x100000 이 온다.
 * @return: 떼어 낸 조각, 떼어 낼 것이 없으면 NULL.
 *
 * 브리지에게 큰 창을 통째로 준 뒤, 그 뒤의 장치들이 실제로는 일부만
 * 썼을 때 남는 앞쪽을 회수한다. **뒤쪽을 회수하는 것은
 * do_bridge_resource_split 이 맡는다.**
 *
 * 동작을 순서대로 보면:
 *   1) 남은 목록을 정렬·병합한다.
 *   2) **시작 주소가 원래와 다르면 포기한다.** 앞쪽이 이미 쓰이고
 *      있다는 뜻이므로 떼어 낼 것이 없다.
 *   3) **길이가 원래와 같으면 포기한다.** 아무것도 안 쓴 것이므로
 *      이 함수가 다룰 경우가 아니다.
 *   4) 남은 첫 노드의 길이가 정렬 단위의 배수가 아니면, 배수가 되도록
 *      앞쪽을 잘라 새 노드로 만들어 목록 앞에 끼워 넣는다. 그 계산이
 *      `(length | (alignment-1)) + 1 - alignment` 인데,
 *      길이를 정렬 단위로 **내림** 한 값이다.
 *   5) 그렇게 정리한 노드가 정렬 단위보다 작으면 포기한다.
 *   6) 그 노드를 목록에서 떼어 내 돌려준다.
 *
 * 정렬을 맞춰야 하는 이유: PCI 브리지의 IO 창은 4KiB, 메모리 창은 1MiB
 * 경계에서만 시작하고 끝날 수 있다. 그래서 회수하는 조각도 그 경계에
 * 맞아야 브리지의 base/limit 레지스터로 표현할 수 있다.
 *
 * 호출자는 돌려받은 조각을 부모의 자유 목록으로 반납하고, 브리지의
 * base 레지스터를 그만큼 뒤로 밀어 준다.
 *
 * **4)에서 만든 split_node 는 목록에 남는다** -- 떼어 내는 것은 그
 * 뒤의 node 이고, split_node 는 브리지가 계속 쓸 부분이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(configure_new_function 안).
 *
 * 호출 체인:
 *   configure_new_function → [이 함수] → cpqhp_resource_sort_and_combine
 */
static struct pci_resource *do_pre_bridge_resource_split(struct pci_resource **head,
				struct pci_resource **orig_head, u32 alignment)
{
	struct pci_resource *prevnode = NULL;
	/* [한국어] 목록을 훑을 포인터 */
	struct pci_resource *node;
	/* [한국어] 쪼갤 때 만들 새 노드 */
	struct pci_resource *split_node;
	/* [한국어] 정렬·병합 결과 */
	u32 rc;
	/* [한국어] 정렬 계산에 쓸 임시 변수 */
	u32 temp_dword;
	/* [한국어] 함수 진입을 로그로 남긴다 */
	dbg("do_pre_bridge_resource_split\n");

	/* [한국어] 둘 중 하나라도 비었으면 비교할 수 없다 */
	if (!(*head) || !(*orig_head))
		return NULL;

	/* [한국어] 주소순으로 정렬하고 인접한 것을 합친다 */
	rc = cpqhp_resource_sort_and_combine(head);

	/* [한국어] 정렬·병합에 실패하면 포기한다 */
	if (rc)
		return NULL;

	/* [한국어] **시작 주소가 원래와 다르면 앞쪽이 이미 쓰이는 중** 이라
	 * 떼어 낼 것이 없다 */
	if ((*head)->base != (*orig_head)->base)
		return NULL;

	/* [한국어] **길이가 원래와 같으면 아무것도 안 쓴 것** 이라 이 함수가 다룰
	 * 경우가 아니다 */
	if ((*head)->length == (*orig_head)->length)
		return NULL;


	/* If we got here, there the bridge requires some of the resource, but
	 * we may be able to split some off of the front
	 */

	/* [한국어] 정렬·병합된 목록의 머리를 잡는다 */
	node = *head;

	if (node->length & (alignment - 1)) {
		/* this one isn't an aligned length, so we'll make a new entry
		 * and split it up.
		 */
		split_node = kmalloc_obj(*split_node);

		/* [한국어] 할당 실패면 포기한다 */
		if (!split_node)
			return NULL;

		/* [한국어] **길이를 정렬 단위로 내림한 값을 구한다.**
		 * (length | (alignment-1)) + 1 이 올림이고, 거기서 alignment 를 빼면
		 * 내림이 된다 */
		temp_dword = (node->length | (alignment-1)) + 1 - alignment;

		/* [한국어] 자투리는 원래 시작부터다 */
		split_node->base = node->base;
		/* [한국어] 자투리의 길이를 정한다 */
		split_node->length = temp_dword;

		/* [한국어] node 는 그만큼 짧아진다 */
		node->length -= temp_dword;
		/* [한국어] node 의 시작을 그만큼 뒤로 옮긴다 */
		node->base += split_node->length;

		/* Put it in the list */
		*head = split_node;
		split_node->next = node;
	}

	/* [한국어] 정렬 단위보다 작으면 쓸 수 없다 */
	if (node->length < alignment)
		return NULL;

	/* Now unlink it */
	if (*head == node) {
		*head = node->next;
	} else {
		/* [한국어] 머리부터 찾기 시작한다 */
		prevnode = *head;
		/* [한국어] **앞 노드를 찾는다** -- 단일 연결 리스트라 필요하다 */
		while (prevnode->next != node)
			/* [한국어] 다음 노드로 넘어간다 */
			prevnode = prevnode->next;

		/* [한국어] 목록에서 뺀다 */
		prevnode->next = node->next;
	}
	/* [한국어] 연결을 끊는다 */
	node->next = NULL;

	/* [한국어] **떼어 낸 조각을 돌려준다** */
	return node;
}


/**
 * do_bridge_resource_split - find one node of resources that aren't in use
 * @head: list head
 * @alignment: max node size (?)
 */
/* [한국어]
 * do_bridge_resource_split - 브리지 창 **뒤쪽** 의 남는 부분을 떼어 낸다
 *
 * @head:      브리지 뒤에서 쓰고 남은 자유 목록.
 * @alignment: 정렬 단위(0x1000 또는 0x100000).
 * @return: 떼어 낸 조각, 조건이 안 맞으면 NULL.
 *
 * do_pre_bridge_resource_split 의 짝이다. 그쪽이 앞쪽을 회수한다면
 * 이쪽은 마지막 조각을 회수한다.
 *
 * **목록의 마지막 노드만 남기고 나머지를 전부 해제하는 것** 이 이 함수의
 * 독특한 점이다. while 루프가 처음부터 끝까지 걸어가며 지나온 노드를
 * kfree 한다. 정렬·병합을 거친 뒤이므로 노드들은 주소순이고, 마지막
 * 노드가 곧 가장 뒤쪽의 빈 공간이다. 앞쪽 조각들은 이 함수가 다룰 대상이
 * 아니며 -- 그것은 앞쪽 함수가 이미 처리했거나 브리지가 쓰는 중이다 --
 * 그래서 버린다.
 *
 * **해제한다는 것이 곧 그 주소를 잃는다는 뜻이다.** 부모 목록으로
 * 돌려주지 않으므로 그 범위는 아무도 쓰지 못하게 된다. 브리지 창 안의
 * 쓰다 남은 틈이라 부모가 쓸 수도 없기 때문으로 보이나, 코드가 그
 * 판단을 적어 두지는 않았다.
 *
 * 남긴 노드에 대해 세 가지를 확인한다.
 *   정렬 단위보다 작으면 실패.
 *   시작 주소가 정렬되지 않았으면 앞을 잘라 맞추고, 그러고도 작으면 실패.
 *   길이가 정렬 단위의 배수가 아니면 실패 -- 위의 원문 주석대로
 *     "이 노드 뒤에 쓰이는 것이 있다" 는 뜻이라 통째로 회수할 수 없다.
 *
 * 실패하면 error 라벨에서 마지막 노드까지 해제한다. 즉 **실패 시 목록
 * 전체가 사라지고 NULL 이 반환된다.** 호출자는 NULL 을 "떼어 낼 것이
 * 없다" 로 해석해 브리지가 창 전부를 쓴 것으로 처리한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   configure_new_function → [이 함수] → cpqhp_resource_sort_and_combine
 */
static struct pci_resource *do_bridge_resource_split(struct pci_resource **head, u32 alignment)
{
	struct pci_resource *prevnode = NULL;
	/* [한국어] 목록을 훑을 포인터 */
	struct pci_resource *node;
	/* [한국어] 정렬·병합 결과 */
	u32 rc;
	/* [한국어] 정렬 계산에 쓸 임시 변수 */
	u32 temp_dword;

	/* [한국어] **주소순으로 정렬하고 인접한 것을 합친다.** 그래야 마지막 노드가
	 * 가장 뒤쪽의 빈 공간이 된다 */
	rc = cpqhp_resource_sort_and_combine(head);

	/* [한국어] 정렬·병합에 실패하면 포기한다 */
	if (rc)
		return NULL;

	/* [한국어] 정렬·병합된 목록의 머리를 잡는다 */
	node = *head;

	/* [한국어] **마지막 노드까지 걸어간다** */
	while (node->next) {
		/* [한국어] 현재 노드를 지울 대상으로 기억해 둔다 */
		prevnode = node;
		/* [한국어] **지나온 노드를 해제한다.** 마지막 노드만 남기고 앞쪽은 버리는데,
		 * 부모에게 돌려주지 않으므로 그 주소는 아무도 못 쓰게 된다.
		 * 브리지 창 안의 틈이라 부모도 쓸 수 없기 때문으로 보이나,
		 * 코드가 그 판단을 적어 두지는 않았다 */
		node = node->next;
		kfree(prevnode);
	}

	/* [한국어] 정렬 단위보다 작으면 쓸 수 없다 */
	if (node->length < alignment)
		goto error;

	if (node->base & (alignment - 1)) {
		/* Short circuit if adjusted size is too small */
		temp_dword = (node->base | (alignment-1)) + 1;
		/* [한국어] **잘라 내고 나면 너무 작아지는지 확인한다.** 위의 원문 주석이
		 * 그 뜻을 밝힌다 */
		if ((node->length - (temp_dword - node->base)) < alignment)
			goto error;

		/* [한국어] **앞을 잘라 낸 만큼 길이를 줄인다** */
		node->length -= (temp_dword - node->base);
		/* [한국어] 시작을 정렬된 자리로 옮긴다 */
		node->base = temp_dword;
	}

	if (node->length & (alignment - 1))
		/* There's stuff in use after this node */
		goto error;

	/* [한국어] **조건을 모두 통과한 마지막 노드를 돌려준다** */
	return node;
error:
	kfree(node);
	return NULL;
}


/**
 * get_io_resource - find first node of given size not in ISA aliasing window.
 * @head: list to search
 * @size: size of node to find, must be a power of two.
 *
 * Description: This function sorts the resource list by size and then
 * returns the first node of "size" length that is not in the ISA aliasing
 * window.  If it finds a node larger than "size" it will split it up.
 */
/* [한국어]
 * get_io_resource - ISA 별칭 구간을 피해 IO 공간을 떼어 낸다
 *
 * @head: 자유 목록의 머리를 가리키는 포인터의 포인터.
 * @size: 필요한 크기. **2의 거듭제곱이어야 한다**(위의 원문 주석).
 * @return: 떼어 낸 조각, 못 찾으면 NULL.
 *
 * get_resource 와 거의 같되 **한 가지 검사가 더 있다** -- 위의 원문
 * 주석이 말하는 ISA 별칭 창(ISA aliasing window) 회피다.
 *
 * 그 검사가 `if (node->base & 0x300L) continue;` 한 줄이다.
 * 비트 8과 9가 모두 0 인 주소만 받아들인다는 뜻이며, 결과적으로
 * 1KiB 블록마다 앞쪽 256바이트만 쓴다.
 *
 * 왜 그런가: 옛 ISA 장치는 IO 주소를 10비트만 디코딩했다. 그래서
 * 0x100 단위로 주소가 되풀이되어 보였고, PCI 장치가 그런 구간에 자리
 * 잡으면 ISA 장치와 충돌한다. 그 관례를 피하려고 각 1KiB의 첫 256바이트만
 * 쓰는 것이며, PCI 규격이 ISA 호환을 위해 정한 방식이다.
 * **다만 그 규격 문서는 이 트리에 없어 위 설명은 코드가 하는 일
 * (비트 8,9 를 거른다)에서 읽어 낸 것이다.**
 *
 * 나머지 절차는 get_resource 와 같다.
 *   1) 목록을 정렬·병합하고 크기 오름차순으로 정렬한다.
 *   2) 앞에서부터 훑으며 요청 크기 이상인 노드를 찾는다.
 *   3) 시작 주소가 정렬되지 않았으면 앞을 잘라 새 노드로 만들어
 *      목록에 끼운다.
 *   4) 남은 길이가 요청보다 크면 뒤를 잘라 새 노드로 만들어 끼운다.
 *   5) ISA 별칭 검사를 통과하면 목록에서 떼어 내 돌려준다.
 *
 * **3)과 4)에서 만든 조각은 목록에 남는다** -- 쓰지 않는 부분이므로
 * 자유 목록에 그대로 두는 것이 맞다.
 *
 * 5)에서 걸리면 continue 로 다음 노드를 본다. 이때 이미 쪼갠 조각들은
 * 목록에 남아 있으므로 낭비되지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   configure_new_function → [이 함수]
 *     → cpqhp_resource_sort_and_combine, sort_by_size
 */
static struct pci_resource *get_io_resource(struct pci_resource **head, u32 size)
{
	struct pci_resource *prevnode;
	/* [한국어] 목록을 훑을 포인터 */
	struct pci_resource *node;
	/* [한국어] 쪼갤 때 만들 새 노드 */
	struct pci_resource *split_node;
	/* [한국어] 정렬 계산에 쓸 임시 변수 */
	u32 temp_dword;

	/* [한국어] 목록이 비었으면 줄 것이 없다 */
	if (!(*head))
		return NULL;

	/* [한국어] 정렬·병합에 실패하면(목록이 비었으면) 포기한다 */
	if (cpqhp_resource_sort_and_combine(head))
		return NULL;

	/* [한국어] **크기 오름차순으로 정렬한다** -- 작은 것부터 보므로 요청에
	 * 가장 근접한 조각을 만나게 된다(최적 적합) */
	if (sort_by_size(head))
		return NULL;

	/* [한국어] 크기 오름차순으로 정렬된 목록을 앞에서부터 훑는다 */
	for (node = *head; node; node = node->next) {
		/* [한국어] **요청보다 작으면 다음 노드를 본다** */
		if (node->length < size)
			continue;

		if (node->base & (size - 1)) {
			/* this one isn't base aligned properly
			 * so we'll make a new entry and split it up
			 */
			temp_dword = (node->base | (size-1)) + 1;

			/* Short circuit if adjusted size is too small */
			if ((node->length - (temp_dword - node->base)) < size)
				continue;

			/* [한국어] **앞쪽 자투리를 담을 새 노드를 만든다.** 목록에 남겨 두면
			 * 다음 요청이 쓸 수 있다 */
			split_node = kmalloc_obj(*split_node);

			/* [한국어] 할당 실패면 포기한다 */
			if (!split_node)
				return NULL;

			/* [한국어] 자투리는 원래 시작부터다 */
			split_node->base = node->base;
			/* [한국어] **앞쪽 자투리의 길이** -- 원래 시작에서 정렬된 자리까지다 */
			split_node->length = temp_dword - node->base;
			/* [한국어] node 의 시작을 정렬된 자리로 옮긴다 */
			node->base = temp_dword;
			/* [한국어] node 는 그만큼 짧아진다 */
			node->length -= split_node->length;

			/* Put it in the list */
			split_node->next = node->next;
			/* [한국어] 쪼갠 조각을 목록에 끼운다 */
			node->next = split_node;
		} /* End of non-aligned base */

		/* Don't need to check if too small since we already did */
		if (node->length > size) {
			/* this one is longer than we need
			 * so we'll make a new entry and split it up
			 */
			split_node = kmalloc_obj(*split_node);

			/* [한국어] 할당 실패면 포기한다 */
			if (!split_node)
				return NULL;

			/* [한국어] 뒤쪽 자투리는 요청 크기 뒤부터다 */
			split_node->base = node->base + size;
			/* [한국어] 자투리의 길이를 정한다 */
			split_node->length = node->length - size;
			/* [한국어] node 를 요청 크기에 딱 맞춘다 */
			node->length = size;

			/* Put it in the list */
			split_node->next = node->next;
			/* [한국어] 쪼갠 조각을 목록에 끼운다 */
			node->next = split_node;
		/* [한국어] 뒤쪽 자투리 처리가 끝났다 */
		}  /* End of too big on top end */

		/* For IO make sure it's not in the ISA aliasing space */
		if (node->base & 0x300L)
			continue;

		/* If we got here, then it is the right size
		 * Now take it out of the list and break
		 */
		if (*head == node) {
			*head = node->next;
		} else {
			/* [한국어] 머리부터 찾기 시작한다 */
			prevnode = *head;
			/* [한국어] 앞 노드를 찾는다 */
			while (prevnode->next != node)
				/* [한국어] 다음 노드로 넘어간다 */
				prevnode = prevnode->next;

			/* [한국어] 목록에서 뺀다 */
			prevnode->next = node->next;
		}
		/* [한국어] 연결을 끊는다 */
		node->next = NULL;
		break;
	}

	/* [한국어] **떼어 낸 조각을 돌려준다** */
	return node;
}


/**
 * get_max_resource - get largest node which has at least the given size.
 * @head: the list to search the node in
 * @size: the minimum size of the node to find
 *
 * Description: Gets the largest node that is at least "size" big from the
 * list pointed to by head.  It aligns the node on top and bottom
 * to "size" alignment before returning it.
 */
/* [한국어]
 * get_max_resource - 요청 크기 이상인 **가장 큰** 조각을 위아래로 정렬해 떼어 낸다
 *
 * @head: 자유 목록의 머리를 가리키는 포인터의 포인터.
 * @size: 최소 크기이자 정렬 단위.
 * @return: 떼어 낸 조각, 못 찾으면 NULL.
 *
 * get_resource 가 "요청에 가장 가까운 것" 을 고른다면 이 함수는
 * **가장 큰 것** 을 고른다. sort_by_max_size 로 내림차순 정렬한 뒤
 * 앞에서부터 보므로 첫 적합이 곧 최대가 된다.
 *
 * 또 한 가지 다른 점은 **양쪽 끝을 모두 정렬한다** 는 것이다.
 *   아래쪽 : 시작 주소가 정렬 단위의 배수가 아니면 앞을 잘라 낸다.
 *   위쪽   : 끝 주소가 정렬 단위의 배수가 아니면 뒤를 잘라 낸다.
 * get_resource 는 위쪽을 "요청 크기에 맞춰" 자르지만, 이쪽은
 * "정렬 경계에 맞춰" 자른다 -- 크기를 줄이는 것이 목적이 아니라
 * 경계를 맞추는 것이 목적이기 때문이다.
 *
 * 그렇게 양끝을 다듬고 나면 크기가 줄 수 있어, 마지막에 다시 한 번
 * 요청 크기를 만족하는지 확인한다(위의 원문 주석이 그 사실을 밝힌다).
 * 못 미치면 continue 로 다음 노드를 본다.
 *
 * 이 함수가 필요한 곳은 브리지 설정이다. configure_new_function 이
 * 브리지를 만나면 그 뒤 장치들이 나눠 쓸 창을 통째로 잡아야 하는데,
 * 브리지의 base/limit 레지스터가 정렬된 경계만 표현할 수 있으므로
 * 양끝이 맞아떨어진 큰 덩어리가 필요하다.
 *
 * 목록에서 떼어 내는 부분이 get_resource 와 미묘하게 다르다 --
 * temp 를 걸어가며 찾되 **temp 가 NULL 이 되는 경우까지 검사한다.**
 * 정렬 과정에서 max 가 목록에 없을 수 있다고 본 방어로 보인다.
 *
 * 호출자가 버스 번호에도 이 함수를 쓴다(size 로 1 을 넘긴다).
 * 버스 번호는 정렬 개념이 없으므로 사실상 "가장 긴 연속 구간 찾기" 가 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   configure_new_function → [이 함수]
 *     → cpqhp_resource_sort_and_combine, sort_by_max_size
 */
static struct pci_resource *get_max_resource(struct pci_resource **head, u32 size)
{
	struct pci_resource *max;
	/* [한국어] 목록에서 노드를 뺄 때 쓸 임시 포인터 */
	struct pci_resource *temp;
	/* [한국어] 쪼갤 때 만들 새 노드 */
	struct pci_resource *split_node;
	/* [한국어] 정렬 계산에 쓸 임시 변수 */
	u32 temp_dword;

	/* [한국어] 정렬·병합에 실패하면 포기한다 */
	if (cpqhp_resource_sort_and_combine(head))
		return NULL;

	/* [한국어] **크기 내림차순으로 정렬한다** -- 큰 것부터 보므로 첫 적합이
	 * 곧 최대가 된다 */
	if (sort_by_max_size(head))
		return NULL;

	for (max = *head; max; max = max->next) {
		/* If not big enough we could probably just bail,
		 * instead we'll continue to the next.
		 */
		if (max->length < size)
			continue;

		if (max->base & (size - 1)) {
			/* this one isn't base aligned properly
			 * so we'll make a new entry and split it up
			 */
			temp_dword = (max->base | (size-1)) + 1;

			/* Short circuit if adjusted size is too small */
			if ((max->length - (temp_dword - max->base)) < size)
				continue;

			/* [한국어] **앞쪽 자투리를 담을 새 노드를 만든다** */
			split_node = kmalloc_obj(*split_node);

			/* [한국어] 할당 실패면 포기한다 */
			if (!split_node)
				return NULL;

			/* [한국어] 자투리는 원래 시작부터다 */
			split_node->base = max->base;
			/* [한국어] 앞쪽 자투리의 길이 */
			split_node->length = temp_dword - max->base;
			/* [한국어] max 의 시작을 정렬된 자리로 옮긴다 */
			max->base = temp_dword;
			/* [한국어] max 는 그만큼 짧아진다 */
			max->length -= split_node->length;

			/* [한국어] 목록에 연결한다 */
			split_node->next = max->next;
			/* [한국어] 쪼갠 조각을 목록에 끼운다 */
			max->next = split_node;
		}

		if ((max->base + max->length) & (size - 1)) {
			/* this one isn't end aligned properly at the top
			 * so we'll make a new entry and split it up
			 */
			split_node = kmalloc_obj(*split_node);

			/* [한국어] 할당 실패면 포기한다 */
			if (!split_node)
				return NULL;
			/* [한국어] **끝 주소를 정렬 단위로 내림한 자리를 구한다** */
			temp_dword = ((max->base + max->length) & ~(size - 1));
			/* [한국어] 자투리는 그 자리부터다 */
			split_node->base = temp_dword;
			/* [한국어] 자투리의 길이를 계산한다 */
			split_node->length = max->length + max->base
					     /* [한국어] 끝 주소에서 자투리 시작을 뺀 값이다 */
					     - split_node->base;
			/* [한국어] **max 는 그만큼 짧아진다** -- 이제 끝이 정렬 경계에 맞는다 */
			max->length -= split_node->length;

			/* [한국어] 목록에 연결한다 */
			split_node->next = max->next;
			/* [한국어] 쪼갠 조각을 목록에 끼운다 */
			max->next = split_node;
		}

		/* Make sure it didn't shrink too much when we aligned it */
		if (max->length < size)
			continue;

		/* Now take it out of the list */
		temp = *head;
		if (temp == max) {
			*head = max->next;
		} else {
			/* [한국어] **앞 노드를 찾되 NULL 검사를 함께 한다** */
			while (temp && temp->next != max)
				/* [한국어] 다음 노드로 넘어간다 */
				temp = temp->next;

			/* [한국어] 찾았는지 확인한다 */
			if (temp)
				/* [한국어] **temp 가 NULL 이 아닐 때만 잇는다** -- 정렬 과정에서 max 가
				 * 목록에 없을 수 있다고 본 방어다 */
				temp->next = max->next;
		}

		/* [한국어] 연결을 끊는다 */
		max->next = NULL;
		break;
	}

	/* [한국어] **찾은 노드를 돌려준다.** 못 찾았으면 NULL 이다 */
	return max;
}


/**
 * get_resource - find resource of given size and split up larger ones.
 * @head: the list to search for resources
 * @size: the size limit to use
 *
 * Description: This function sorts the resource list by size and then
 * returns the first node of "size" length.  If it finds a node
 * larger than "size" it will split it up.
 *
 * size must be a power of two.
 */
/* [한국어]
 * get_resource - 요청 크기에 가장 가까운 조각을 떼어 낸다
 *
 * @head: 자유 목록의 머리를 가리키는 포인터의 포인터.
 * @size: 필요한 크기. **2의 거듭제곱이어야 한다**(위의 원문 주석).
 * @return: 떼어 낸 조각, 못 찾으면 NULL.
 *
 * 이 드라이버 자원 할당의 기본 함수다. 메모리와 prefetchable 메모리를
 * 카드의 BAR 에 배정할 때 쓴다.
 *
 * get_io_resource 와 **ISA 별칭 검사 한 줄만 빼고 같다.** 그쪽은 IO
 * 전용이라 그 검사가 필요하고, 이쪽은 메모리라 필요 없다. 두 함수가
 * 거의 통째로 중복되어 있는 것 역시 그 시절 코드의 특징이다.
 *
 * 절차:
 *   1) 정렬·병합한 뒤 크기 **오름차순** 으로 정렬한다. 작은 것부터 보므로
 *      요청에 가장 근접한 조각을 만나게 된다 -- 최적 적합이다.
 *   2) 요청 크기 이상인 첫 노드를 찾는다.
 *   3) 시작 주소가 정렬되지 않았으면 앞부분을 새 노드로 떼어 목록에
 *      끼우고, node 의 시작을 정렬된 자리로 옮긴다. 그러고도 크기가
 *      모자라면 continue 로 다음 노드를 본다.
 *   4) 남은 길이가 요청보다 크면 뒤를 새 노드로 떼어 목록에 끼운다.
 *   5) 목록에서 node 를 빼내 돌려준다.
 *
 * **3)과 4)가 만든 조각은 자유 목록에 남는다.** 쓰지 않는 부분이므로
 * 다음 요청이 쓸 수 있다.
 *
 * dbg 로그가 유난히 촘촘한데(요청 크기, 정렬 여부, 크기 초과 여부,
 * 성공), 자원 할당이 어긋나면 원인을 찾기 어렵기 때문으로 보인다.
 * 그 로그는 전역 cpqhp_debug 가 참일 때만 나온다.
 *
 * **할당 실패 시 이미 쪼갠 조각을 되돌리지 않는다.** 3)에서 쪼갠 뒤
 * 크기가 모자라 continue 하면 그 조각은 목록에 남는데, 이는 손해가
 * 아니다 -- 정렬 경계에서 쪼개진 것뿐이고 cpqhp_resource_sort_and_combine
 * 이 나중에 인접한 것끼리 다시 합쳐 준다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   configure_new_function → [이 함수]
 *     → cpqhp_resource_sort_and_combine, sort_by_size
 */
static struct pci_resource *get_resource(struct pci_resource **head, u32 size)
{
	struct pci_resource *prevnode;
	/* [한국어] 목록을 훑을 포인터 */
	struct pci_resource *node;
	/* [한국어] 쪼갤 때 만들 새 노드 */
	struct pci_resource *split_node;
	/* [한국어] 정렬 계산에 쓸 임시 변수 */
	u32 temp_dword;

	/* [한국어] 정렬·병합에 실패하면 포기한다 */
	if (cpqhp_resource_sort_and_combine(head))
		return NULL;

	/* [한국어] 크기 오름차순으로 정렬한다 -- 최적 적합을 위해서다 */
	if (sort_by_size(head))
		return NULL;

	/* [한국어] 크기 오름차순으로 정렬된 목록을 앞에서부터 훑는다 */
	for (node = *head; node; node = node->next) {
		/* [한국어] **어느 노드를 보고 있는지 로그로 남긴다.** 자원 할당이 어긋나면
		 * 원인을 찾기 어려워 로그가 촘촘하다 */
		dbg("%s: req_size =%x node=%p, base=%x, length=%x\n",
		    __func__, size, node, node->base, node->length);
		/* [한국어] 요청보다 작으면 다음 노드를 본다 */
		if (node->length < size)
			continue;

		/* [한국어] **시작 주소가 정렬되지 않았는지 본다** */
		if (node->base & (size - 1)) {
			dbg("%s: not aligned\n", __func__);
			/* this one isn't base aligned properly
			 * so we'll make a new entry and split it up
			 */
			temp_dword = (node->base | (size-1)) + 1;

			/* Short circuit if adjusted size is too small */
			if ((node->length - (temp_dword - node->base)) < size)
				continue;

			/* [한국어] 앞쪽 자투리를 담을 새 노드를 만든다 */
			split_node = kmalloc_obj(*split_node);

			/* [한국어] 할당 실패면 포기한다 */
			if (!split_node)
				return NULL;

			/* [한국어] 자투리는 원래 시작부터다 */
			split_node->base = node->base;
			/* [한국어] 앞쪽 자투리의 길이 */
			split_node->length = temp_dword - node->base;
			/* [한국어] node 의 시작을 정렬된 자리로 옮긴다 */
			node->base = temp_dword;
			/* [한국어] node 는 그만큼 짧아진다 */
			node->length -= split_node->length;

			/* [한국어] 목록에 연결한다 */
			split_node->next = node->next;
			/* [한국어] 쪼갠 조각을 목록에 끼운다 */
			node->next = split_node;
		/* [한국어] 앞쪽 자투리 처리가 끝났다 */
		} /* End of non-aligned base */

		/* Don't need to check if too small since we already did */
		if (node->length > size) {
			dbg("%s: too big\n", __func__);
			/* this one is longer than we need
			 * so we'll make a new entry and split it up
			 */
			split_node = kmalloc_obj(*split_node);

			/* [한국어] 할당 실패면 포기한다 */
			if (!split_node)
				return NULL;

			/* [한국어] **뒤쪽 자투리는 요청 크기 뒤부터다** */
			split_node->base = node->base + size;
			/* [한국어] 자투리의 길이를 정한다 */
			split_node->length = node->length - size;
			/* [한국어] **node 를 요청 크기에 딱 맞춘다** */
			node->length = size;

			/* Put it in the list */
			split_node->next = node->next;
			/* [한국어] 쪼갠 조각을 목록에 끼운다 */
			node->next = split_node;
		/* [한국어] 뒤쪽 자투리 처리가 끝났다 */
		}  /* End of too big on top end */

		dbg("%s: got one!!!\n", __func__);
		/* If we got here, then it is the right size
		 * Now take it out of the list */
		if (*head == node) {
			*head = node->next;
		} else {
			/* [한국어] 머리부터 찾기 시작한다 */
			prevnode = *head;
			/* [한국어] 앞 노드를 찾는다 */
			while (prevnode->next != node)
				/* [한국어] 다음 노드로 넘어간다 */
				prevnode = prevnode->next;

			/* [한국어] 목록에서 뺀다 */
			prevnode->next = node->next;
		}
		/* [한국어] 연결을 끊는다 */
		node->next = NULL;
		break;
	}
	/* [한국어] **떼어 낸 조각을 돌려준다.** 못 찾았으면 NULL 이다 */
	return node;
}


/**
 * cpqhp_resource_sort_and_combine - sort nodes by base addresses and clean up
 * @head: the list to sort and clean up
 *
 * Description: Sorts all of the nodes in the list in ascending order by
 * their base addresses.  Also does garbage collection by
 * combining adjacent nodes.
 *
 * Returns %0 if success.
 */
/* [한국어]
 * cpqhp_resource_sort_and_combine - 자유 목록을 주소순으로 정렬하고 인접 조각을 합친다
 *
 * @head: 정리할 목록의 머리를 가리키는 포인터의 포인터.
 * @return: 목록이 비었으면 1, 그 밖에는 0.
 *
 * **이 할당기의 쓰레기 수집에 해당한다.** 위의 원문 주석이 그 사실을
 * 직접 밝힌다 -- 시작 주소 오름차순으로 정렬하고, 인접한 노드를 합쳐
 * 파편화를 되돌린다.
 *
 * 두 단계다.
 *   1) 버블 정렬로 base 오름차순 정렬. sort_by_size 와 같은 모양이며
 *      비교 대상만 length 에서 base 로 바뀌었다.
 *   2) 앞에서부터 걸어가며 `node1->base + node1->length == node1->next->base`
 *      이면 둘을 합치고 뒤 노드를 kfree 한다. 주소가 정확히 이어질 때만
 *      합치므로 틈이 있으면 그대로 둔다.
 *
 * 정렬이 먼저여야 하는 이유: 인접 여부는 주소가 이어지는지로 판단하는데,
 * 정렬되지 않은 목록에서는 이웃한 노드가 주소상 이웃이 아니다.
 *
 * **이 함수가 없으면 목록이 계속 잘게 쪼개진다.** get_resource 계열이
 * 할당할 때마다 노드를 쪼개고, return_resource 는 반납한 조각을 그냥
 * 머리에 매달 뿐이므로, 정리하지 않으면 같은 주소 범위가 여러 노드로
 * 흩어진 채 남는다. 그래서 board_added, remove_board,
 * cpqhp_return_board_resources, cpqhp_find_available_resources 가
 * 자원을 만진 직후마다 네 목록 모두에 이 함수를 부른다.
 *
 * 빈 목록에 1 을 돌려주는 규약이 호출자마다 다르게 쓰인다 --
 * do_pre_bridge_resource_split 은 그것을 실패로 보고 NULL 을 반환하지만,
 * cpqhp_find_available_resources 는 네 번의 결과를 & 로 묶어
 * **전부 비었을 때만** 실패로 본다.
 *
 * dbg 로그에 "8..
 * " 같은 의미를 알 수 없는 문자열이 있다. 개발 중
 * 남은 흔적으로 보이며 원문 그대로 둔다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   board_added / remove_board / get_resource 계열 /
 *   cpqhp_return_board_resources / cpqhp_find_available_resources → [이 함수]
 */
int cpqhp_resource_sort_and_combine(struct pci_resource **head)
{
	struct pci_resource *node1;
	/* [한국어] 교환할 때 쓸 임시 포인터 */
	struct pci_resource *node2;
	/* [한국어] 교환이 있었는지 표시하는 플래그 */
	int out_of_order = 1;

	/* [한국어] 목록의 머리를 로그로 남긴다 */
	dbg("%s: head = %p, *head = %p\n", __func__, head, *head);

	/* [한국어] 목록이 비었는지 확인한다 */
	if (!(*head))
		/* [한국어] **빈 목록에 1 을 돌려준다.** 호출자마다 이 값을 다르게 해석한다 --
		 * do_pre_bridge_resource_split 은 실패로 보고,
		 * cpqhp_find_available_resources 는 네 결과를 & 로 묶어 전부 비었을
		 * 때만 실패로 본다 */
		return 1;

	/* [한국어] 둘째 노드를 로그로 남긴다 */
	dbg("*head->next = %p\n", (*head)->next);

	/* [한국어] 원소가 하나뿐인지 확인한다 */
	if (!(*head)->next)
		/* [한국어] **원소가 하나뿐이면 이미 정렬된 것이다.** 오른쪽 원문 주석이
		 * 그 뜻을 밝힌다 */
		return 0;	/* only one item on the list, already sorted! */

	/* [한국어] 머리의 시작 주소를 로그로 남긴다 */
	dbg("*head->base = 0x%x\n", (*head)->base);
	/* [한국어] 둘째 노드의 시작 주소를 로그로 남긴다 */
	dbg("*head->next->base = 0x%x\n", (*head)->next->base);
	/* [한국어] **교환이 없을 때까지 반복한다** -- 주소순 버블 정렬이다 */
	while (out_of_order) {
		/* [한국어] 한 바퀴 시작 전에 내려 둔다 */
		out_of_order = 0;

		/* Special case for swapping list head */
		if (((*head)->next) &&
		    ((*head)->base > (*head)->next->base)) {
			/* [한국어] 원래 머리를 기억해 둔다 */
			node1 = *head;
			/* [한국어] 머리를 다음으로 옮긴다 */
			(*head) = (*head)->next;
			/* [한국어] 교환 중 */
			node1->next = (*head)->next;
			/* [한국어] 교환을 마무리한다 */
			(*head)->next = node1;
			/* [한국어] 교환이 있었음을 표시한다 */
			out_of_order++;
		}

		/* [한국어] 머리부터 훑기 시작한다 */
		node1 = (*head);

		/* [한국어] 세 번째 노드까지 있어야 중간을 교환할 수 있다 */
		while (node1->next && node1->next->next) {
			/* [한국어] **주소가 어긋났으면 교환한다** -- 오름차순으로 정렬한다 */
			if (node1->next->base > node1->next->next->base) {
				/* [한국어] 교환이 있었음을 표시한다 */
				out_of_order++;
				/* [한국어] 뒤 노드를 기억해 둔다 */
				node2 = node1->next;
				/* [한국어] 교환 중 */
				node1->next = node1->next->next;
				/* [한국어] 교환 중 */
				node1 = node1->next;
				/* [한국어] 교환 중 */
				node2->next = node1->next;
				/* [한국어] 교환의 마지막 연결 */
				node1->next = node2;
			} else
				/* [한국어] 이미 순서가 맞으면 다음으로 넘어간다 */
				node1 = node1->next;
		}
	/* [한국어] 교환이 없을 때까지 반복했다 */
	}  /* End of out_of_order loop */

	/* [한국어] 병합을 위해 머리부터 다시 시작한다 */
	node1 = *head;

	/* [한국어] **인접한 노드를 합치며 걸어간다.** 이것이 이 할당기의 쓰레기
	 * 수집이다 */
	while (node1 && node1->next) {
		if ((node1->base + node1->length) == node1->next->base) {
			/* Combine */
			dbg("8..\n");
			/* [한국어] **두 노드를 하나로 합친다** -- 뒤 노드의 길이를 앞에 더한다 */
			node1->length += node1->next->length;
			/* [한국어] 해제할 노드를 기억해 둔다 */
			node2 = node1->next;
			/* [한국어] 뒤 노드를 리스트에서 뺀다 */
			node1->next = node1->next->next;
			kfree(node2);
		/* [한국어] 인접하지 않은 경우로 넘어간다 */
		} else
			/* [한국어] 합칠 수 없으면 다음으로 넘어간다 */
			node1 = node1->next;
	}

	return 0;
}


/* [한국어]
 * cpqhp_ctrl_intr - 하드 인터럽트 핸들러. 사건을 분류해 큐에 넣고 스레드를 깨운다
 *
 * @IRQ:  인터럽트 번호. 쓰지 않는다.
 * @data: request_irq 에 넘긴 struct controller.
 * @return: 우리 인터럽트가 아니면 IRQ_NONE, 처리했으면 IRQ_HANDLED.
 *
 * **이 드라이버에서 하드 인터럽트 컨텍스트로 도는 유일한 함수다.**
 * 그래서 여기서는 레지스터를 읽고 분류하고 큐에 넣는 일만 하고,
 * 잠들 수 있는 일은 전부 스레드로 넘긴다.
 *
 *   1) MISC 레지스터의 비트 2,3 을 보고 **자기 인터럽트인지 가린다.**
 *      아니면 IRQ_NONE 을 돌려줘 공유 선의 다른 핸들러에게 넘긴다.
 *   2) 비트 2(Serial Output 완료)면 그 비트를 지우고
 *      **wake_up_interruptible(&ctrl->queue)** 로 대기 큐를 깨운다.
 *      set_SOGO 로 명령을 내린 뒤 wait_for_ctrl_irq 로 기다리는 쪽이
 *      그 큐에 걸려 있다. 다만 그 깨움이 실제로 대기를 줄이는지는
 *      이 트리에서 확인할 수 없다(파일 상단 헤더의 해당 절 참조).
 *   3) 비트 3(범용 입력 변화)이면 INT_INPUT_CLEAR 를 읽어
 *      **이전 값과 XOR 해 변화한 비트만 뽑는다.** 그 Diff 를 되써서
 *      인터럽트를 지우고, 세 분류 함수에 나눠 준다.
 *        하위 8비트    → 레버 변화
 *        상위 16비트   → 존재/버튼 변화
 *        비트 8~15     → 전원 결함
 *   4) RESET_FREQ_MODE 의 비트 6 이 서 있으면 버스 리셋이 끝난 것이므로
 *      지우고 대기 큐를 깨운다.
 *   5) 적재한 이벤트가 하나라도 있으면 전역 스레드를 깨운다.
 *
 * **"쓰기 뒤 되읽기" 관용구가 두 번 나온다.** 위의 원문 주석이 이유를
 * 밝히는데, 지연 쓰기(posted write)를 강제로 밀어내기 위해서다. PCI 쓰기는
 * 버퍼에 머물 수 있으므로, 같은 영역을 읽어야 실제로 하드웨어에 도달한
 * 것이 보장된다. 인터럽트를 지우는 쓰기가 늦으면 같은 인터럽트가 다시
 * 들어오므로 중요하다.
 *
 * Diff 가 0 인 경우(변화가 없는데 인터럽트가 온 경우) 0xFFFFFFFF 를 써서
 * 모든 상태를 지운다 -- 원인을 모를 때 전부 지워 폭주를 막는 방어다.
 *
 * **ctrl->ctrl_int_comp 가 인터럽트와 스레드가 공유하는 상태다.**
 * 분류 함수들이 그 값을 읽어 현재 상태를 판단하는데, 락 없이 갱신된다.
 *
 * 실행 컨텍스트: 하드 인터럽트. 잠들 수 없다.
 * 뒷세대 pciehp 가 request_threaded_irq 로 스레드 절반을 두는 것과 달리,
 * 이쪽은 전역 kthread 를 wake_up_process 로 깨우는 옛 방식이다.
 *
 * 호출 체인:
 *   슬롯 하드웨어 변화 → 커널 → [이 함수]
 *     → handle_switch_change / handle_presence_change / handle_power_fault,
 *       wake_up_process(cpqhp_event_thread)
 */
irqreturn_t cpqhp_ctrl_intr(int IRQ, void *data)
{
	struct controller *ctrl = data;
	/* [한국어] **적재한 이벤트 개수의 합.** 0 이 아니면 스레드를 깨운다 */
	u8 schedule_flag = 0;
	/* [한국어] 버스 리셋 상태를 담을 변수 */
	u8 reset;
	/* [한국어] MISC 레지스터 값 */
	u16 misc;
	/* [한국어] **변화한 비트만 담을 변수.** 이전 값과 XOR 해서 구한다 */
	u32 Diff;


	misc = readw(ctrl->hpc_reg + MISC);
	/*
	 * Check to see if it was our interrupt
	 */
	if (!(misc & 0x000C))
		return IRQ_NONE;

	if (misc & 0x0004) {
		/*
		 * Serial Output interrupt Pending
		 */

		/* Clear the interrupt */
		misc |= 0x0004;
		/* [한국어] 비트를 되써서 인터럽트를 지운다 */
		writew(misc, ctrl->hpc_reg + MISC);

		/* Read to clear posted writes */
		misc = readw(ctrl->hpc_reg + MISC);

		/* [한국어] 깨우기 직전을 로그로 남긴다 */
		dbg("%s - waking up\n", __func__);
		wake_up_interruptible(&ctrl->queue);
	}

	if (misc & 0x0008) {
		/* General-interrupt-input interrupt Pending */
		Diff = readl(ctrl->hpc_reg + INT_INPUT_CLEAR) ^ ctrl->ctrl_int_comp;

		/* [한국어] **현재 상태를 저장해 둔다.** 분류 함수들이 이 값을 읽어 지금
		 * 상태를 판단한다. 인터럽트와 스레드가 공유하는데 락이 없다 */
		ctrl->ctrl_int_comp = readl(ctrl->hpc_reg + INT_INPUT_CLEAR);

		/* Clear the interrupt */
		writel(Diff, ctrl->hpc_reg + INT_INPUT_CLEAR);

		/* Read it back to clear any posted writes */
		readl(ctrl->hpc_reg + INT_INPUT_CLEAR);

		if (!Diff)
			/* Clear all interrupts */
			writel(0xFFFFFFFF, ctrl->hpc_reg + INT_INPUT_CLEAR);

		/* [한국어] **하위 8비트는 레버 변화다.** 세 분류 함수가 각각 자기 몫의
		 * 비트만 받아 처리하고, 적재한 개수를 더해 돌려준다 */
		schedule_flag += handle_switch_change((u8)(Diff & 0xFFL), ctrl);
		/* [한국어] **상위 16비트는 존재/버튼 변화다** */
		schedule_flag += handle_presence_change((u16)((Diff & 0xFFFF0000L) >> 16), ctrl);
		/* [한국어] **비트 8~15 는 전원 결함이다** */
		schedule_flag += handle_power_fault((u8)((Diff & 0xFF00L) >> 8), ctrl);
	}

	/* [한국어] **버스 리셋이 끝났는지 확인한다** */
	reset = readb(ctrl->hpc_reg + RESET_FREQ_MODE);
	if (reset & 0x40) {
		/* Bus reset has completed */
		reset &= 0xCF;
		/* [한국어] 지운 값을 쓴다 */
		writeb(reset, ctrl->hpc_reg + RESET_FREQ_MODE);
		/* [한국어] 되읽어 지연 쓰기를 밀어낸다 */
		reset = readb(ctrl->hpc_reg + RESET_FREQ_MODE);
		wake_up_interruptible(&ctrl->queue);
	}

	/* [한국어] **적재한 이벤트가 하나라도 있으면 스레드를 깨운다** */
	if (schedule_flag) {
		wake_up_process(cpqhp_event_thread);
		/* [한국어] 스레드를 깨웠음을 로그로 남긴다 */
		dbg("Waking even thread");
	}
	return IRQ_HANDLED;
}


/**
 * cpqhp_slot_create - Creates a node and adds it to the proper bus.
 * @busnumber: bus where new node is to be located
 *
 * Returns pointer to the new node or %NULL if unsuccessful.
 */
/* [한국어]
 * cpqhp_slot_create - struct pci_func 를 만들어 전역 버스 목록에 매단다
 *
 * @busnumber: 이 노드를 매달 버스 번호.
 * @return: 새 노드, 할당에 실패하면 NULL.
 *
 * 전역 배열 cpqhp_slot_list[256] 에서 해당 버스의 연결 리스트를 찾아
 * **끝에** 새 노드를 붙인다. 배열이 256칸인 것은 PCI 버스 번호가
 * 8비트이기 때문이다.
 *
 * kzalloc 으로 0 을 채운 뒤 configured 만 1 로 둔다. 나머지 필드
 * (설정공간 사본, 자원 목록 넷)는 0/NULL 로 시작해 나중에 채워진다.
 *
 * **락이 없다.** 전역 리스트를 여러 컨트롤러가 공유하는데도 보호가 없다.
 * 실제로는 이 함수를 부르는 곳이 전부 전역 스레드 하나이거나 probe
 * 경로라 직렬화되지만, 코드가 그 사실에 기대고 있을 뿐 명시하지는 않는다.
 *
 * 끝에 붙이느라 매번 리스트를 끝까지 걸어간다. 노드 수가 적어 문제가
 * 되지 않는다.
 *
 * 이 함수가 만드는 것은 **빈 슬롯을 나타내는 자리표** 이기도 하다.
 * remove_board 와 cpqhp_process_SI 가 카드를 뺀 뒤 is_a_board 를 0 으로
 * 둔 노드를 새로 만들어 두는데, 그래야 다음에 카드가 꽂혔을 때
 * cpqhp_slot_find 가 그 슬롯을 찾을 수 있기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. kzalloc(GFP_KERNEL)이라 잠들 수 있다.
 *
 * 호출 체인:
 *   cpqhp_save_config / remove_board / cpqhp_process_SI /
 *   configure_new_device / configure_new_function → [이 함수]
 */
struct pci_func *cpqhp_slot_create(u8 busnumber)
{
	struct pci_func *new_slot;
	/* [한국어] 리스트를 훑을 포인터 */
	struct pci_func *next;

	/* [한국어] **0 으로 채워 할당한다.** 설정공간 사본과 자원 목록 넷이 전부
	 * 0/NULL 로 시작한다 */
	new_slot = kzalloc_obj(*new_slot);
	/* [한국어] 할당 실패를 확인한다 */
	if (new_slot == NULL)
		/* [한국어] **할당 실패면 NULL 을 그대로 돌려준다** */
		return new_slot;

	/* [한국어] 연결을 끊어 둔다. kzalloc 덕에 이미 NULL 이지만 명시한다 */
	new_slot->next = NULL;
	/* [한국어] **설정된 것으로 표시한다.** kzalloc 이 나머지를 0 으로 채웠으므로
	 * 이 필드만 따로 세운다 */
	new_slot->configured = 1;

	/* [한국어] **이 버스의 리스트가 비었으면 머리로 삼는다** */
	if (cpqhp_slot_list[busnumber] == NULL) {
		cpqhp_slot_list[busnumber] = new_slot;
	} else {
		/* [한국어] 리스트 머리를 잡는다 */
		next = cpqhp_slot_list[busnumber];
		/* [한국어] **리스트 끝까지 걸어간다.** 노드 수가 적어 문제가 되지 않는다 */
		while (next->next != NULL)
			/* [한국어] 다음 노드로 넘어간다 */
			next = next->next;
		/* [한국어] **끝에 매단다** */
		next->next = new_slot;
	}
	/* [한국어] 새 노드를 돌려준다 */
	return new_slot;
}


/**
 * slot_remove - Removes a node from the linked list of slots.
 * @old_slot: slot to remove
 *
 * Returns %0 if successful, !0 otherwise.
 */
/* [한국어]
 * slot_remove - struct pci_func 를 전역 목록에서 빼고 해제한다
 *
 * @old_slot: 없앨 노드.
 * @return: 0 성공, 1 이면 인자나 목록이 비었음, 2 면 목록에서 못 찾음.
 *
 * cpqhp_slot_create 의 짝이다. 해당 버스의 리스트를 걸어가며 노드를
 * 찾아 잇고, 그 노드가 들고 있던 자원 목록까지 해제한다.
 *
 * **cpqhp_destroy_board_resources 를 함께 부르는 것이 중요하다.**
 * struct pci_func 에는 io_head, mem_head, p_mem_head, bus_head 네 개의
 * 연결 리스트가 매달려 있어, 노드만 kfree 하면 그 리스트들이 통째로
 * 샌다. 다만 그 함수는 자원을 자유 목록으로 **돌려주지 않고 버린다** --
 * 자유 목록으로 되돌리는 것은 cpqhp_return_board_resources 의 일이고,
 * 호출자가 그것을 먼저 부른 뒤 이 함수를 부르는 순서다.
 *
 * 머리를 지우는 경우와 중간을 지우는 경우를 나눠 처리한다. 단일 연결
 * 리스트라 앞 노드를 알아야 잇을 수 있기 때문이다.
 *
 * 반환값 셋을 구별해 쓰는 곳은 remove_board 하나뿐인데,
 * 그것도 `while (!slot_remove(next))` 로 **0 인지 아닌지만** 본다.
 * 즉 1 과 2 의 구별은 실제로 쓰이지 않는다.
 *
 * bridge_slot_remove 가 이 함수를 반복 호출해 하위 버스의 노드를 전부
 * 지우는데, 매번 리스트 머리를 다시 읽는 방식이라 안전하다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   remove_board / cpqhp_process_SI / bridge_slot_remove → [이 함수]
 *     → cpqhp_destroy_board_resources
 */
static int slot_remove(struct pci_func *old_slot)
{
	struct pci_func *next;

	if (old_slot == NULL)
		return 1;

	/* [한국어] **전역 배열에서 이 버스의 리스트를 꺼낸다** */
	next = cpqhp_slot_list[old_slot->bus];
	/* [한국어] 리스트가 비었는지 확인한다 */
	if (next == NULL)
		/* [한국어] 리스트가 비었으면 1 을 돌려준다 */
		return 1;

	/* [한국어] 머리인지 확인한다 */
	if (next == old_slot) {
		/* [한국어] **머리가 지울 노드면 머리를 다음으로 옮긴다** */
		cpqhp_slot_list[old_slot->bus] = old_slot->next;
		cpqhp_destroy_board_resources(old_slot);
		kfree(old_slot);
		return 0;
	}

	/* [한국어] **지울 노드의 앞 노드를 찾는다.** 단일 연결 리스트라 앞 노드를
	 * 알아야 이을 수 있다 */
	while ((next->next != old_slot) && (next->next != NULL))
		/* [한국어] 다음 노드로 넘어간다 */
		next = next->next;

	/* [한국어] 찾았는지 확인한다 */
	if (next->next == old_slot) {
		/* [한국어] **리스트에서 노드를 뺀다** */
		next->next = old_slot->next;
		cpqhp_destroy_board_resources(old_slot);
		kfree(old_slot);
		return 0;
	/* [한국어] 찾지 못한 경우로 넘어간다 */
	} else
		/* [한국어] **리스트에 없으면 2 를 돌려준다.** 다만 호출자는 0 인지 아닌지만
		 * 보므로 1 과 2 의 구별은 실제로 쓰이지 않는다 */
		return 2;
}


/**
 * bridge_slot_remove - Removes a node from the linked list of slots.
 * @bridge: bridge to remove
 *
 * Returns %0 if successful, !0 otherwise.
 */
/* [한국어]
 * bridge_slot_remove - 브리지와 그 아래 모든 노드를 지운다
 *
 * @bridge: 없앨 브리지 노드.
 * @return: 0 성공, 1 이면 목록이 비었음, 2 면 목록에서 못 찾음.
 *
 * 브리지를 뽑으면 그 뒤에 매달려 있던 장치들도 함께 사라지므로,
 * 하위 버스 번호 범위의 노드를 전부 지운 뒤 브리지 자신을 지운다.
 *
 * 버스 범위는 **저장해 둔 설정공간 사본에서 읽는다** --
 * config_space[0x06] 이 오프셋 0x18 의 dword 이고, 그 안에서
 * 비트 15:8 이 보조 버스, 비트 23:16 이 종속 버스다. 실제 하드웨어를
 * 읽지 않는 이유는 이 시점에 카드가 이미 없을 수 있기 때문이다.
 *
 * 하위 노드를 지우는 루프가 특이하다.
 *   `while (!slot_remove(next)) next = cpqhp_slot_list[tempBus];`
 * 매번 리스트의 머리를 다시 읽어 그것을 지운다. 리스트가 빌 때까지
 * 머리만 반복해서 지우는 셈이라, 순회 중 노드가 사라져도 안전하다.
 *
 * **브리지 자신은 cpqhp_destroy_board_resources 를 부르지 않는다.**
 * slot_remove 가 하위 노드에 대해서는 그것을 부르는데, out 라벨에서는
 * kfree(bridge) 만 한다. 브리지가 들고 있던 자원 목록은 호출자
 * (remove_board)가 cpqhp_return_board_resources 로 이미 자유 목록에
 * 돌려준 뒤이기 때문으로 보이나, 코드가 그 사실을 적어 두지는 않았다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   remove_board / cpqhp_process_SI → [이 함수] → slot_remove
 */
static int bridge_slot_remove(struct pci_func *bridge)
{
	u8 subordinateBus, secondaryBus;
	/* [한국어] 버스 번호 순회 변수 */
	u8 tempBus;
	/* [한국어] 리스트를 훑을 포인터 */
	struct pci_func *next;

	/* [한국어] **보조 버스 번호는 비트 15:8 이다.** config_space[0x06] 이 설정공간
	 * 오프셋 0x18 의 dword 이고, 그 안에 기본·보조·종속 버스 번호가
	 * 차례로 들어 있다. **실제 하드웨어가 아니라 사본을 읽는 것** 은
	 * 이 시점에 카드가 이미 없을 수 있기 때문이다 */
	secondaryBus = (bridge->config_space[0x06] >> 8) & 0xFF;
	/* [한국어] **종속 버스 번호는 비트 23:16 이다** */
	subordinateBus = (bridge->config_space[0x06] >> 16) & 0xFF;

	/* [한국어] **보조 버스부터 종속 버스까지 모두 훑는다** -- 브리지 뒤에
	 * 매달린 모든 버스다 */
	for (tempBus = secondaryBus; tempBus <= subordinateBus; tempBus++) {
		/* [한국어] 그 버스의 리스트 머리를 꺼낸다 */
		next = cpqhp_slot_list[tempBus];

		/* [한국어] 지울 것이 없을 때까지 반복한다 */
		while (!slot_remove(next))
			/* [한국어] **매번 머리를 다시 읽는다.** 리스트가 빌 때까지 머리만 반복해서
			 * 지우는 방식이라, 순회 중 노드가 사라져도 안전하다 */
			next = cpqhp_slot_list[tempBus];
	}

	/* [한국어] **이제 브리지 자신을 지운다** -- 하위 노드는 위에서 다 지웠다 */
	next = cpqhp_slot_list[bridge->bus];

	/* [한국어] 리스트가 비었는지 확인한다 */
	if (next == NULL)
		/* [한국어] 리스트가 비었으면 1 을 돌려준다 */
		return 1;

	/* [한국어] 머리인지 확인한다 */
	if (next == bridge) {
		/* [한국어] **머리가 브리지면 머리를 다음으로 옮긴다** */
		cpqhp_slot_list[bridge->bus] = bridge->next;
		goto out;
	}

	/* [한국어] 브리지의 앞 노드를 찾는다 */
	while ((next->next != bridge) && (next->next != NULL))
		next = next->next;

	/* [한국어] 찾았는지 확인한다 */
	if (next->next != bridge)
		/* [한국어] 리스트에 없으면 2 를 돌려준다 */
		return 2;
	/* [한국어] **브리지를 리스트에서 뺀다** */
	next->next = bridge->next;
out:
	kfree(bridge);
	return 0;
}


/**
 * cpqhp_slot_find - Looks for a node by bus, and device, multiple functions accessed
 * @bus: bus to find
 * @device: device to find
 * @index: is %0 for first function found, %1 for the second...
 *
 * Returns pointer to the node if successful, %NULL otherwise.
 */
/* [한국어]
 * cpqhp_slot_find - 버스·장치 번호로 struct pci_func 를 찾는다. 다중 함수 지원
 *
 * @bus:    찾을 버스 번호.
 * @device: 찾을 장치 번호.
 * @index:  같은 장치의 몇 번째 함수인가. **0 이면 첫 번째, 1 이면 두 번째.**
 * @return: 찾은 노드, 없으면 NULL.
 *
 * 전역 cpqhp_slot_list[bus] 리스트를 훑는다. 다중 함수 카드는 함수마다
 * 노드가 하나씩 있으므로, 같은 device 를 가진 노드가 여럿일 수 있다.
 * index 로 그중 몇 번째를 원하는지 지정한다.
 *
 * **cpqhp_find_slot 과 이름이 비슷하지만 찾는 대상이 다르다.**
 * 그쪽은 struct slot(물리 슬롯), 이쪽은 struct pci_func(PCI 함수)다.
 *
 * 호출자들이 이 함수를 index 를 증가시키며 반복 호출해
 * **같은 장치의 모든 함수를 순회하는 관용구** 를 쓴다:
 *   `index = 0; func = cpqhp_slot_find(bus, dev, index++);
 *    while (func) { ...; func = cpqhp_slot_find(bus, dev, index++); }`
 * 매번 리스트를 처음부터 다시 훑으므로 O(n^2)이지만 노드가 적어
 * 문제가 되지 않는다.
 *
 * **found 를 -1 로 시작하는 것에 주의.** 첫 노드가 조건에 맞으면
 * 곧바로 돌려주는 특수 경로가 위에 있고, 그 뒤로는 일치할 때마다
 * found 를 증가시켜 index 와 비교한다. -1 에서 시작하므로 두 번째
 * 일치에서 found 가 0 이 되는데, 첫 번째 일치는 이미 위에서 처리했으므로
 * 셈이 맞는다. 읽기는 까다롭지만 동작은 맞다.
 *
 * **func 가 NULL 이 아닌지 확인하지 않고 func->next 를 보는 경로는
 * 없다** -- 맨 위에서 NULL 이면 곧바로 돌려주기 때문이다.
 *
 * 실행 컨텍스트: 어디서나. **인터럽트 컨텍스트에서도 불린다**
 * (handle_switch_change 등). 락을 잡지 않는다.
 *
 * 호출 체인:
 *   이 파일과 cpqphp_pci.c 의 여러 함수 → [이 함수]
 */
struct pci_func *cpqhp_slot_find(u8 bus, u8 device, u8 index)
{
	int found = -1;
	/* [한국어] 리스트를 훑을 포인터 */
	struct pci_func *func;

	/* [한국어] **전역 배열에서 이 버스의 리스트를 꺼낸다.** 락이 없다 */
	func = cpqhp_slot_list[bus];

	/* [한국어] **리스트가 비었거나 첫 노드가 곧 답인 경우를 먼저 처리한다** */
	if ((func == NULL) || ((func->device == device) && (index == 0)))
		/* [한국어] **첫 노드를 그대로 돌려준다.** 리스트가 비었으면 NULL 이 나간다 */
		return func;

	/* [한국어] 첫 노드의 장치 번호가 맞는지 본다 */
	if (func->device == device)
		/* [한국어] **첫 노드가 일치하면 found 를 -1 에서 0 으로 올린다.**
		 * 첫 일치는 위에서 이미 처리했으므로, 여기부터는 두 번째 일치가
		 * found == 0 이 되어 index 1 과 짝이 맞는다 */
		found++;

	/* [한국어] 리스트를 끝까지 훑는다 */
	while (func->next != NULL) {
		func = func->next;

		/* [한국어] 장치 번호가 맞는지 본다 */
		if (func->device == device)
			/* [한국어] 일치 횟수를 센다 */
			found++;

		/* [한국어] 원하는 순번인지 본다 */
		if (found == index)
			/* [한국어] **index 번째 일치를 찾았다** */
			return func;
	}

	return NULL;
}


/* DJZ: I don't think is_bridge will work as is.
 * FIXME */
/* [한국어]
 * is_bridge - 저장해 둔 설정공간 사본으로 브리지인지 판정한다
 *
 * @func: 검사할 노드.
 * @return: 브리지면 1, 아니면 0.
 *
 * **위의 원문 주석이 이 함수를 믿지 말라고 경고한다** --
 * "DJZ: 이대로는 동작하지 않을 것 같다. FIXME" 라고 적혀 있다.
 * 왜 그런지는 코드에 적혀 있지 않으므로 추측하지 않는다.
 *
 * 판정 방법: config_space[0x03] 은 설정공간 오프셋 0x0C 의 dword 이고,
 * 그 안에서 비트 23:16 이 헤더 타입 바이트다. 그것을 0x01 과 견줘
 * PCI-to-PCI 브리지인지 본다.
 *
 * **실제 하드웨어가 아니라 사본을 읽는다.** 그래서 사본이 채워지지 않은
 * 노드 -- 예컨대 remove_board 가 만들어 두는 빈 슬롯 자리표 -- 에서는
 * config_space 가 전부 0 이라 0x00 으로 읽혀 "브리지 아님" 이 된다.
 * 원문 주석이 우려하는 지점이 이것일 수 있으나 단정하지 않는다.
 *
 * 헤더 타입의 다중 함수 비트(0x80)를 지우지 않고 0x01 과 직접 비교하는
 * 것에도 주의 -- 다중 함수 브리지는 헤더 타입이 0x81 이라 이 검사를
 * 통과하지 못한다. 이 파일의 다른 곳들은 PCI_HEADER_TYPE_MASK 로
 * 마스크한 뒤 비교하므로 방식이 다르다.
 *
 * 호출자는 remove_board 와 cpqhp_process_SI 둘뿐이며, 결과에 따라
 * bridge_slot_remove 를 쓸지 slot_remove 를 쓸지 정한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   remove_board / cpqhp_process_SI → [이 함수]
 */
static int is_bridge(struct pci_func *func)
{
	/* Check the header type */
	if (((func->config_space[0x03] >> 16) & 0xFF) == 0x01)
		/* [한국어] **찾지 못했다.** 이 함수는 브리지 판정에 쓰이므로 0 은
		 * "브리지가 아님" 을 뜻한다 */
		return 1;
	else
		return 0;
}


/**
 * set_controller_speed - set the frequency and/or mode of a specific controller segment.
 * @ctrl: controller to change frequency/mode for.
 * @adapter_speed: the speed of the adapter we want to match.
 * @hp_slot: the slot number where the adapter is installed.
 *
 * Returns %0 if we successfully change frequency and/or mode to match the
 * adapter speed.
 */
/* [한국어]
 * set_controller_speed - 버스 주파수와 PCI/PCI-X 모드를 카드에 맞춘다
 *
 * @ctrl:          대상 컨트롤러.
 * @adapter_speed: 새로 꽂힌 카드가 낼 수 있는 속도.
 * @hp_slot:       그 카드가 꽂힌 슬롯 번호.
 * @return: 0 이면 맞췄거나 맞출 필요가 없음, 1 이면 맞출 수 없음.
 *
 * **PCI 버스는 세그먼트 전체가 한 속도로 돈다.** 그래서 새 카드 하나를
 * 위해 속도를 바꾸면 같은 세그먼트의 다른 카드가 영향을 받는다.
 * 이 함수의 절반이 "바꿔도 되는가" 를 따지는 데 쓰인다.
 *
 * 판정 순서:
 *   1) 이미 같은 속도면 할 일이 없다.
 *   2) **같은 컨트롤러에 다른 카드가 동작 중이면 원칙적으로 거절한다.**
 *      다만 현재 속도가 새 카드보다 느리면, 새 카드가 그 느린 속도로
 *      동작하면 되므로 0 을 돌려준다(위의 원문 주석 참조).
 *   3) 컨트롤러가 속도 변경을 지원하지 않으면(pcix_speed_capability 가 0),
 *      현재가 더 빠르면 거절하고 더 느리면 그대로 받아들인다.
 *   4) 컨트롤러의 최대 속도가 카드보다 낮으면 최대 속도로 낮춰 잡는다.
 *
 * 실제 변경 절차는 하드웨어 순서에 묶여 있다:
 *   LED 와 슬롯 활성화를 모두 끄고 SOGO 로 밀어낸다.
 *   설정공간 0x41 에 값을 쓴다(속도에 따라 0xF4 또는 0xF5).
 *   NEXT_CURR_FREQ 레지스터의 하위 4비트에 속도 코드를 넣고,
 *     비트 15:12 에 0xB 를 넣는다.
 *   5ms 기다린다.
 *   인터럽트를 다시 켠다.
 *   0x41 을 한 번 더 쓴다.
 *   0x43 으로 상태 기계를 재시작한다.
 *   모드가 바뀌는 경우(PCI 66MHz ↔ PCI-X 66MHz)에만 SOGO 를 한 번 더 친다.
 *   **1100ms 를 바쁘게 기다린다.**
 *   LED 와 슬롯 전원을 원래대로 되돌린다.
 *
 * **mdelay(1100)이 이 파일에서 가장 눈에 띄는 옛 관용구다.** 1.1초 동안
 * CPU 를 붙들고 도는데, 주파수 전환 뒤 버스가 안정되기를 기다리는
 * 것으로 보인다. msleep 이 아니라 mdelay 인 이유는 코드에 적혀 있지 않다.
 *
 * 0x41, 0x43, 0xF4, 0xF5, 0x71~0x75, 0xB 같은 값은 **이 컨트롤러 고유의
 * 설정이며 근거 문서가 이 트리에 없다.** 각 값이 어느 레지스터의 어느
 * 자리에 들어가는지까지만 적는다.
 *
 * 0x43 을 다루는 부분에 주의 -- `reg = ~0xF;` 로 초기화한 뒤 곧바로
 * pci_read_config_byte 로 덮어쓰므로 **그 초기화는 아무 효과가 없다.**
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. mdelay 로 CPU 를 오래 점유한다.
 * 호출자가 ctrl->crit_sect 를 쥔 채 부르므로 그 구간 전체가 직렬화된다.
 *
 * 호출 체인:
 *   board_added / board_replaced → [이 함수]
 *     → get_presence_status, set_SOGO, wait_for_ctrl_irq, cpqhp_find_slot
 */
static u8 set_controller_speed(struct controller *ctrl, u8 adapter_speed, u8 hp_slot)
{
	struct slot *slot;
	/* [한국어] 현재 버스 속도를 읽기 위해 pci_bus 가 필요하다 */
	struct pci_bus *bus = ctrl->pci_bus;
	/* [한국어] 설정공간에 쓸 값 */
	u8 reg;
	/* [한국어] **슬롯 전원 상태도 저장해 둔다** */
	u8 slot_power = readb(ctrl->hpc_reg + SLOT_POWER);
	/* [한국어] 주파수 레지스터 값 */
	u16 reg16;
	/* [한국어] **LED 상태를 저장해 둔다.** 주파수 변경 중에 껐다가 나중에
	 * 되돌리기 위해서다 */
	u32 leds = readl(ctrl->hpc_reg + LED_CONTROL);

	/* [한국어] **이미 같은 속도면 할 일이 없다** */
	if (bus->cur_bus_speed == adapter_speed)
		return 0;

	/* We don't allow freq/mode changes if we find another adapter running
	 * in another slot on this controller
	 */
	for (slot = ctrl->slot; slot; slot = slot->next) {
		/* [한국어] **자기 자신은 건너뛴다** -- 지금 켜려는 슬롯이다 */
		if (slot->device == (hp_slot + ctrl->slot_device_offset))
			continue;
		/* [한국어] 카드가 꽂혀 있지 않으면 건너뛴다 */
		if (get_presence_status(ctrl, slot) == 0)
			continue;
		/* If another adapter is running on the same segment but at a
		 * lower speed/mode, we allow the new adapter to function at
		 * this rate if supported
		 */
		if (bus->cur_bus_speed < adapter_speed)
			return 0;

		/* [한국어] **다른 카드가 더 빠르게 돌고 있으면 거절한다** -- 그 카드를
		 * 느리게 만들 수 없기 때문이다 */
		return 1;
	}

	/* If the controller doesn't support freq/mode changes and the
	 * controller is running at a higher mode, we bail
	 */
	if ((bus->cur_bus_speed > adapter_speed) && (!ctrl->pcix_speed_capability))
		/* [한국어] **컨트롤러가 속도 변경을 지원하지 않고 현재가 더 빠르면 거절한다.**
		 * 낮출 방법이 없기 때문이다 */
		return 1;

	/* But we allow the adapter to run at a lower rate if possible */
	if ((bus->cur_bus_speed < adapter_speed) && (!ctrl->pcix_speed_capability))
		return 0;

	/* We try to set the max speed supported by both the adapter and
	 * controller
	 */
	if (bus->max_bus_speed < adapter_speed) {
		/* [한국어] **이미 최고 속도로 돌고 있으면 바꿀 것이 없다** */
		if (bus->cur_bus_speed == bus->max_bus_speed)
			return 0;
		/* [한국어] 컨트롤러가 낼 수 있는 최고 속도로 낮춰 잡는다 */
		adapter_speed = bus->max_bus_speed;
	}

	/* [한국어] **LED 를 모두 끈다.** 주파수를 바꾸는 동안 슬롯을 조용하게
	 * 만들어 두는 것이다 */
	writel(0x0L, ctrl->hpc_reg + LED_CONTROL);
	/* [한국어] 슬롯 활성화도 모두 끈다 */
	writeb(0x00, ctrl->hpc_reg + SLOT_ENABLE);

	set_SOGO(ctrl);
	wait_for_ctrl_irq(ctrl);

	/* [한국어] **속도에 따라 0x41 에 쓸 값이 갈린다** */
	if (adapter_speed != PCI_SPEED_133MHz_PCIX)
		/* [한국어] 그 밖이면 0xF5 */
		reg = 0xF5;
	else
		/* [한국어] PCI-X 133MHz 면 0xF4 */
		reg = 0xF4;
	/* [한국어] **설정공간 0x41 에 값을 쓴다.** 이 컨트롤러 고유의 레지스터이며
	 * 그 의미의 근거 문서는 이 트리에 없다 */
	pci_write_config_byte(ctrl->pci_dev, 0x41, reg);

	/* [한국어] 현재 주파수 설정을 읽는다 */
	reg16 = readw(ctrl->hpc_reg + NEXT_CURR_FREQ);
	/* [한국어] 하위 4비트(속도 코드)를 지운다 */
	reg16 &= ~0x000F;
	/* [한국어] 속도에 따라 레지스터 값을 고른다 */
	switch (adapter_speed) {
		/* [한국어] **PCI-X 133MHz -- 이 하드웨어의 최고 속도다** */
		case(PCI_SPEED_133MHz_PCIX):
			reg = 0x75;
			/* [한국어] PCI-X 133MHz 의 코드 */
			reg16 |= 0xB;
			break;
		/* [한국어] PCI-X 100MHz */
		case(PCI_SPEED_100MHz_PCIX):
			reg = 0x74;
			/* [한국어] PCI-X 100MHz 의 코드 */
			reg16 |= 0xA;
			break;
		/* [한국어] PCI-X 66MHz */
		case(PCI_SPEED_66MHz_PCIX):
			reg = 0x73;
			/* [한국어] PCI-X 66MHz 의 코드 */
			reg16 |= 0x9;
			break;
		/* [한국어] PCI 66MHz -- **PCI-X 가 아닌 일반 PCI 다** */
		case(PCI_SPEED_66MHz):
			reg = 0x73;
			/* [한국어] PCI 66MHz 의 코드 */
			reg16 |= 0x1;
			break;
		/* [한국어] **그 밖은 33MHz PCI 2.2 다.** reg16 의 하위 4비트를 0 으로 둔다 --
		 * 위에서 이미 지웠으므로 아무것도 더하지 않는다 */
		default: /* 33MHz PCI 2.2 */
			reg = 0x71;
			break;

	}
	/* [한국어] **비트 15:12 에 0xB 를 넣는다.** 그 필드의 의미는 이 트리에서
	 * 확인할 수 없다 */
	reg16 |= 0xB << 12;
	/* [한국어] 주파수 설정을 쓴다 */
	writew(reg16, ctrl->hpc_reg + NEXT_CURR_FREQ);

	mdelay(5);

	/* Re-enable interrupts */
	writel(0, ctrl->hpc_reg + INT_MASK);

	/* [한국어] **0x41 을 한 번 더 쓴다.** 위에서 이미 같은 값을 썼는데 반복하는
	 * 이유는 코드에 적혀 있지 않다 */
	pci_write_config_byte(ctrl->pci_dev, 0x41, reg);

	/* Restart state machine */
	reg = ~0xF;
	/* [한국어] **상태 기계를 재시작한다.** 0x43 레지스터를 읽어 그대로 되쓰는데,
	 * 읽기 자체나 되쓰기가 재시작을 유발하는 것으로 보인다.
	 * 그 근거 문서는 이 트리에 없다 */
	pci_read_config_byte(ctrl->pci_dev, 0x43, &reg);
	/* [한국어] 읽은 값을 그대로 되쓴다 */
	pci_write_config_byte(ctrl->pci_dev, 0x43, reg);

	/* Only if mode change...*/
	if (((bus->cur_bus_speed == PCI_SPEED_66MHz) && (adapter_speed == PCI_SPEED_66MHz_PCIX)) ||
		((bus->cur_bus_speed == PCI_SPEED_66MHz_PCIX) && (adapter_speed == PCI_SPEED_66MHz)))
			set_SOGO(ctrl);

	wait_for_ctrl_irq(ctrl);
	mdelay(1100);

	/* Restore LED/Slot state */
	writel(leds, ctrl->hpc_reg + LED_CONTROL);
	/* [한국어] **슬롯 전원 상태도 되돌린다.** 위에서 저장해 둔 값이다 */
	writeb(slot_power, ctrl->hpc_reg + SLOT_ENABLE);

	set_SOGO(ctrl);
	wait_for_ctrl_irq(ctrl);

	/* [한국어] **소프트웨어가 기억하는 현재 속도를 갱신한다** */
	bus->cur_bus_speed = adapter_speed;
	/* [한국어] 로그에 찍을 슬롯 번호를 얻는다 */
	slot = cpqhp_find_slot(ctrl, hp_slot + ctrl->slot_device_offset);

	/* [한국어] **성공을 사용자에게 알린다.** dbg 가 아니라 info 라 항상 찍힌다 */
	info("Successfully changed frequency/mode for adapter in slot %d\n",
			slot->number);
	return 0;
}

/* the following routines constitute the bulk of the
 * hotplug controller logic
 */


/**
 * board_replaced - Called after a board has been replaced in the system.
 * @func: PCI device/function information
 * @ctrl: hotplug controller
 *
 * This is only used if we don't have resources for hot add.
 * Turns power on for the board.
 * Checks to see if board is the same.
 * If board is same, reconfigures it.
 * If board isn't same, turns it back off.
 */
/* [한국어]
 * board_replaced - 같은 카드로 교체되었을 때의 처리
 *
 * @func: 대상 PCI 함수 정보.
 * @ctrl: 해당 컨트롤러.
 * @return: 0 이면 성공, 그 밖에는 오류 코드 또는 1.
 *
 * 위의 원문 주석이 이 함수의 조건을 밝힌다 -- **핫애드용 자원이 없을 때만
 * 쓰인다.** 자원을 새로 나눠 줄 수 없으므로, 뽑기 전과 똑같은 카드가
 * 다시 꽂혔을 때만 저장해 둔 설정을 되쓰는 방식으로 대응한다.
 *
 * 먼저 두 가지를 거른다.
 *   레버가 열려 있으면 INTERLOCK_OPEN.
 *   이미 전원이 켜져 있으면 CARD_FUNCTIONING.
 *
 * 그다음 절차가 **두 단계로 나뉜 것** 이 특징이다.
 *   1단계 -- 버스에 붙이지 않고 전원만 넣어 속도를 확인한다.
 *     enable_slot_power 로 전원만 주고(slot_enable 이 아니다),
 *     속도를 읽어 set_controller_speed 로 맞춘 뒤,
 *     disable_slot_power 로 다시 끈다.
 *     **왜 이렇게 하는가**: 속도가 맞지 않는 카드를 버스에 붙이면
 *     같은 세그먼트의 다른 카드가 오동작할 수 있기 때문이다.
 *   2단계 -- 속도가 맞으면 slot_enable 로 실제로 버스에 붙이고
 *     녹색 LED 를 깜빡인다.
 *
 * 중간의 "슬롯 전원 레지스터 값을 0 으로 썼다가 되돌리는" 두 줄은
 * 위의 원문 주석대로 **타이머 버그 우회** 다. 값을 바꿔야 하드웨어가
 * 한 번 더 시프트아웃을 하기 때문이라고 적혀 있다.
 *
 * 전원을 넣고 **1초를 기다린다.** 원문 주석대로 핫플러그 규격이 정한
 * 시간이다. 그 뒤 func->status 가 0xFF 인지 보는데, 그것은
 * handle_power_fault 가 인터럽트 컨텍스트에서 세워 둔 값이다 --
 * **인터럽트와 이 스레드가 status 필드로 통신한다.**
 *
 * 전원 결함이 없었으면 cpqhp_valid_replace 로 같은 카드인지 검사하고,
 * 같으면 cpqhp_configure_board 로 설정공간 사본을 되쓴다.
 *
 * **반환값 처리에 특이한 점이 있다.** 설정에 성공(rc == 0)해도
 * LED 를 끄고 슬롯을 비활성화한 뒤 1 을 돌려준다. 위의 원문 주석은
 * "설정에 실패하면 끈다" 고 적혀 있는데 성공 경로도 같은 정리를 지나간다.
 * 코드는 손대지 않고 사실만 적어 둔다.
 *
 * crit_sect 뮤텍스를 네 번 잡았다 놓는다. 레지스터를 만지는 구간만
 * 짧게 감싸고, 잠드는 구간(long_delay)은 밖에 두기 위해서다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(전역 이벤트 스레드).
 *
 * 호출 체인:
 *   cpqhp_process_SI → [이 함수]
 *     → set_controller_speed, cpqhp_valid_replace, cpqhp_configure_board
 */
static u32 board_replaced(struct pci_func *func, struct controller *ctrl)
{
	struct pci_bus *bus = ctrl->pci_bus;
	/* [한국어] 컨트롤러 안의 슬롯 번호 */
	u8 hp_slot;
	/* [한국어] 레지스터 값 임시 변수 */
	u8 temp_byte;
	/* [한국어] 카드가 낼 수 있는 속도 */
	u8 adapter_speed;
	/* [한국어] 반환값. 0 으로 시작한다 */
	u32 rc = 0;

	/* [한국어] 컨트롤러 안의 슬롯 번호로 바꾼다 */
	hp_slot = func->device - ctrl->slot_device_offset;

	/*
	 * The switch is open.
	 */
	if (readl(ctrl->hpc_reg + INT_INPUT_CLEAR) & (0x01L << hp_slot))
		rc = INTERLOCK_OPEN;
	/*
	 * The board is already on
	 */
	else if (is_slot_enabled(ctrl, hp_slot))
		/* [한국어] **이미 켜져 있으면 아무것도 하지 않는다** */
		rc = CARD_FUNCTIONING;
	else {
		mutex_lock(&ctrl->crit_sect);

		/* turn on board without attaching to the bus */
		enable_slot_power(ctrl, hp_slot);

		set_SOGO(ctrl);

		/* Wait for SOBS to be unset */
		wait_for_ctrl_irq(ctrl);

		/* Change bits in slot power register to force another shift out
		 * NOTE: this is to work around the timer bug */
		temp_byte = readb(ctrl->hpc_reg + SLOT_POWER);
		/* [한국어] 일단 0 을 쓴다 */
		writeb(0x00, ctrl->hpc_reg + SLOT_POWER);
		/* [한국어] 원래 값을 도로 쓴다 -- 타이머 버그 우회다 */
		writeb(temp_byte, ctrl->hpc_reg + SLOT_POWER);

		set_SOGO(ctrl);

		/* Wait for SOBS to be unset */
		wait_for_ctrl_irq(ctrl);

		/* [한국어] 카드가 낼 수 있는 속도를 읽는다 */
		adapter_speed = get_adapter_speed(ctrl, hp_slot);
		/* [한국어] 현재 버스 속도와 다를 때만 바꾸려 시도한다 */
		if (bus->cur_bus_speed != adapter_speed)
			/* [한국어] 속도를 맞춰 본다 */
			if (set_controller_speed(ctrl, adapter_speed, hp_slot))
				/* [한국어] 속도를 못 맞추면 오류 코드를 남긴다 */
				rc = WRONG_BUS_FREQUENCY;

		/* turn off board without attaching to the bus */
		disable_slot_power(ctrl, hp_slot);

		set_SOGO(ctrl);

		/* Wait for SOBS to be unset */
		wait_for_ctrl_irq(ctrl);

		mutex_unlock(&ctrl->crit_sect);

		/* [한국어] 속도 설정 실패를 확인한다 */
		if (rc)
			/* [한국어] 속도를 못 맞췄으면 여기서 끝낸다 */
			return rc;

		mutex_lock(&ctrl->crit_sect);

		/* [한국어] **슬롯을 버스에 붙인다** -- 속도 확인이 끝났으므로 이제 안전하다 */
		slot_enable(ctrl, hp_slot);
		/* [한국어] 녹색을 깜빡여 진행 중임을 알린다 */
		green_LED_blink(ctrl, hp_slot);

		/* [한국어] 황색을 끈다 */
		amber_LED_off(ctrl, hp_slot);

		set_SOGO(ctrl);

		/* Wait for SOBS to be unset */
		wait_for_ctrl_irq(ctrl);

		mutex_unlock(&ctrl->crit_sect);

		/* Wait for ~1 second because of hot plug spec */
		long_delay(1*HZ);

		/* Check for a power fault */
		if (func->status == 0xFF) {
			/* power fault occurred, but it was benign */
			rc = POWER_FAILURE;
			/* [한국어] 전원 결함 표시를 지운다 */
			func->status = 0;
		/* [한국어] 전원 결함 표시를 지운다 */
		} else
			/* [한국어] **같은 카드인지 검사한다.** 0 이 아니면 다른 카드다 */
			rc = cpqhp_valid_replace(ctrl, func);

		if (!rc) {
			/* It must be the same board */

			/* [한국어] **저장해 둔 설정공간 사본을 되쓴다.** 같은 카드이므로 뽑기 전
			 * 설정을 그대로 되살린다 */
			rc = cpqhp_configure_board(ctrl, func);

			/* If configuration fails, turn it off
			 * Get slot won't work for devices behind
			 * bridges, but in this case it will always be
			 * called for the "base" bus/dev/func of an
			 * adapter.
			 */

			mutex_lock(&ctrl->crit_sect);

			/* [한국어] 황색을 켠다 */
			amber_LED_on(ctrl, hp_slot);
			/* [한국어] 녹색을 끈다 */
			green_LED_off(ctrl, hp_slot);
			/* [한국어] 슬롯을 버스에서 뗀다 */
			slot_disable(ctrl, hp_slot);

			set_SOGO(ctrl);

			/* Wait for SOBS to be unset */
			wait_for_ctrl_irq(ctrl);

			mutex_unlock(&ctrl->crit_sect);

			/* [한국어] 설정 결과를 확인한다 */
			if (rc)
				/* [한국어] 설정 실패를 그대로 올린다 */
				return rc;
			else
				/* [한국어] **설정에 성공해도 1 을 돌려준다.** 위의 원문 주석은 "설정에
				 * 실패하면 끈다" 고 적혀 있는데 성공 경로도 같은 정리를 지나간다.
				 * 코드는 손대지 않고 사실만 적어 둔다 */
				return 1;

		} else {
			/* Something is wrong

			 * Get slot won't work for devices behind bridges, but
			 * in this case it will always be called for the "base"
			 * bus/dev/func of an adapter.
			 */

			mutex_lock(&ctrl->crit_sect);

			/* [한국어] 황색을 켠다 */
			amber_LED_on(ctrl, hp_slot);
			/* [한국어] 녹색을 끈다 */
			green_LED_off(ctrl, hp_slot);
			/* [한국어] 슬롯을 버스에서 뗀다 */
			slot_disable(ctrl, hp_slot);

			set_SOGO(ctrl);

			/* Wait for SOBS to be unset */
			wait_for_ctrl_irq(ctrl);

			mutex_unlock(&ctrl->crit_sect);
		}

	}
	/* [한국어] 결과를 돌려준다 */
	return rc;

}


/**
 * board_added - Called after a board has been added to the system.
 * @func: PCI device/function info
 * @ctrl: hotplug controller
 *
 * Turns power on for the board.
 * Configures board.
 */
/* [한국어]
 * board_added - 새 카드가 꽂혔을 때 전원을 넣고 자원을 배정한다
 *
 * @func: 대상 PCI 함수 정보.
 * @ctrl: 해당 컨트롤러.
 * @return: 0 성공, 그 밖에는 오류 코드.
 *
 * **이 드라이버의 핫애드 경로 본체다.** board_replaced 가 "같은 카드를
 * 되돌리는" 길이라면 이쪽은 "새 카드에 자원을 나눠 주는" 길이다.
 *
 * 앞부분은 board_replaced 와 같다 -- 버스에 붙이지 않고 전원만 넣어
 * 속도를 맞추고, 타이머 버그를 우회하고, 다시 끈다. 그다음 실제로
 * slot_enable 하고 녹색 LED 를 깜빡이고 1초를 기다린다.
 *
 * **여기서부터 갈린다.**
 *   1) 전원 결함이 있었으면(status == 0xFF) POWER_FAILURE 로 끝낸다.
 *   2) 없으면 벤더/장치 ID 를 읽는다. 전부 F 면 카드가 없거나
 *      망가진 것이라 NO_ADAPTER_PRESENT 로 끝낸다.
 *   3) 유효한 카드면 **컨트롤러의 자유 목록 네 개를 res_lists 로 묶어**
 *      configure_new_device 에 넘긴다. 그 함수가 자원을 떼어 쓰고,
 *      돌아오면 줄어든 목록을 다시 컨트롤러로 옮겨 담는다.
 *      그 뒤 네 목록 모두 정렬·병합한다.
 *   4) 설정에 실패하면 LED 를 켜고 슬롯을 끄고 오류를 돌려준다.
 *      **이때 이미 떼어 쓴 자원은 configure_new_device 가 자기 안에서
 *      되돌린다.**
 *   5) 성공하면 cpqhp_save_slot_config 로 설정공간 사본을 갱신하고,
 *      is_a_board 를 1 로 표시한다.
 *   6) **리눅스 쪽 struct pci_dev 를 만든다.** index 를 늘려 가며
 *      같은 장치의 모든 함수를 순회하며 cpqhp_configure_device 를 부른다.
 *      이 시점에야 리눅스가 그 카드를 인식하고 드라이버가 붙는다.
 *   7) 녹색 LED 를 켠다.
 *
 * **res_lists 를 값으로 복사해 넘기고 돌아와 되받는 방식** 에 주의.
 * configure_new_device 가 목록의 머리를 바꿀 수 있으므로, 구조체를
 * 통째로 주고받아 갱신된 머리를 회수한다.
 *
 * 6)의 루프에 특이한 점이 있다 -- `while (new_slot)` 인데 루프 안에서
 * new_slot 을 다시 대입하므로, 마지막에 NULL 이 되어야 끝난다.
 * `!new_slot->pci_dev` 를 확인하는 것은 이미 pci_dev 가 있는 함수를
 * 중복 등록하지 않기 위해서다.
 *
 * dbg 로그가 유난히 촘촘하다(뮤텍스 전후, 각 LED 조작 전후). 이 경로가
 * 잠금과 하드웨어 대기가 뒤섞여 있어 어디서 멈췄는지 알아야 했기
 * 때문으로 보인다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(전역 이벤트 스레드). 여러 번 잠든다.
 *
 * 호출 체인:
 *   cpqhp_process_SI → [이 함수]
 *     → set_controller_speed, configure_new_device, cpqhp_save_slot_config,
 *       cpqhp_configure_device
 */
static u32 board_added(struct pci_func *func, struct controller *ctrl)
{
	u8 hp_slot;
	/* [한국어] 레지스터 값 임시 변수 */
	u8 temp_byte;
	/* [한국어] 카드가 낼 수 있는 속도 */
	u8 adapter_speed;
	/* [한국어] 같은 장치의 함수를 순회할 첨자 */
	int index;
	/* [한국어] **전부 1 로 초기화한다** -- 빈 슬롯을 뜻하는 값이라, 아래에서
	 * 읽기에 실패해도 안전한 기본값이 된다 */
	u32 temp_register = 0xFFFFFFFF;
	/* [한국어] 반환값. 0 으로 시작한다 */
	u32 rc = 0;
	/* [한국어] 리눅스 pci_dev 를 만들 때 순회할 노드 */
	struct pci_func *new_slot = NULL;
	/* [한국어] 현재 버스 속도를 읽기 위해 pci_bus 가 필요하다 */
	struct pci_bus *bus = ctrl->pci_bus;
	/* [한국어] **자유 목록 꾸러미.** 아래에서 컨트롤러의 목록 넷을 여기 담아
	 * 하위 함수에 넘긴다 */
	struct resource_lists res_lists;

	/* [한국어] 컨트롤러 안의 슬롯 번호로 바꾼다 */
	hp_slot = func->device - ctrl->slot_device_offset;
	/* [한국어] 슬롯 번호 계산 결과를 로그로 남긴다 */
	dbg("%s: func->device, slot_offset, hp_slot = %d, %d ,%d\n",
	    __func__, func->device, ctrl->slot_device_offset, hp_slot);

	mutex_lock(&ctrl->crit_sect);

	/* turn on board without attaching to the bus */
	enable_slot_power(ctrl, hp_slot);

	set_SOGO(ctrl);

	/* Wait for SOBS to be unset */
	wait_for_ctrl_irq(ctrl);

	/* Change bits in slot power register to force another shift out
	 * NOTE: this is to work around the timer bug
	 */
	temp_byte = readb(ctrl->hpc_reg + SLOT_POWER);
	/* [한국어] 일단 0 을 쓴다 */
	writeb(0x00, ctrl->hpc_reg + SLOT_POWER);
	/* [한국어] **원래 값을 도로 쓴다.** 위의 원문 주석대로 값을 바꿔야 하드웨어가
	 * 한 번 더 시프트아웃을 하는 타이머 버그 우회다 */
	writeb(temp_byte, ctrl->hpc_reg + SLOT_POWER);

	set_SOGO(ctrl);

	/* Wait for SOBS to be unset */
	wait_for_ctrl_irq(ctrl);

	adapter_speed = get_adapter_speed(ctrl, hp_slot);
	/* [한국어] 카드가 낼 수 있는 속도를 읽는다 */
	if (bus->cur_bus_speed != adapter_speed)
		/* [한국어] **현재 버스 속도와 다를 때만 바꾸려 시도한다** */
		if (set_controller_speed(ctrl, adapter_speed, hp_slot))
			/* [한국어] 속도를 못 맞추면 오류 코드를 남긴다 */
			rc = WRONG_BUS_FREQUENCY;

	/* turn off board without attaching to the bus */
	disable_slot_power(ctrl, hp_slot);

	set_SOGO(ctrl);

	/* Wait for SOBS to be unset */
	wait_for_ctrl_irq(ctrl);

	mutex_unlock(&ctrl->crit_sect);

	/* [한국어] 속도 설정 실패를 확인한다 */
	if (rc)
		/* [한국어] 속도를 못 맞췄으면 여기서 끝낸다 */
		return rc;

	/* [한국어] **반환값을 쓰지 않는다.** 슬롯을 찾기만 하고 버리는데,
	 * 부작용도 없는 함수라 이 호출은 아무 효과가 없다 */
	cpqhp_find_slot(ctrl, hp_slot + ctrl->slot_device_offset);

	/* turn on board and blink green LED */

	/* [한국어] 락 잡기 직전을 로그로 남긴다 */
	dbg("%s: before down\n", __func__);
	mutex_lock(&ctrl->crit_sect);
	/* [한국어] 락을 잡았음을 로그로 남긴다 */
	dbg("%s: after down\n", __func__);

	/* [한국어] 슬롯 활성화 직전을 로그로 남긴다 */
	dbg("%s: before slot_enable\n", __func__);
	/* [한국어] **슬롯을 버스에 붙인다.** 위의 enable_slot_power 가 전원만 준
	 * 것과 달리, 이제 카드가 버스에 응답하기 시작한다 */
	slot_enable(ctrl, hp_slot);

	/* [한국어] 녹색 LED 조작 직전을 로그로 남긴다 */
	dbg("%s: before green_LED_blink\n", __func__);
	/* [한국어] 녹색을 깜빡여 진행 중임을 알린다 */
	green_LED_blink(ctrl, hp_slot);

	/* [한국어] 황색 LED 조작 직전을 로그로 남긴다 */
	dbg("%s: before amber_LED_blink\n", __func__);
	/* [한국어] **황색을 끈다.** 로그 문구는 amber_LED_blink 라고 되어 있으나
	 * 실제 호출은 amber_LED_off 다. 원문 그대로 둔다 */
	amber_LED_off(ctrl, hp_slot);

	/* [한국어] SOGO 직전을 로그로 남긴다 */
	dbg("%s: before set_SOGO\n", __func__);
	set_SOGO(ctrl);

	/* Wait for SOBS to be unset */
	dbg("%s: before wait_for_ctrl_irq\n", __func__);
	wait_for_ctrl_irq(ctrl);
	/* [한국어] 대기가 끝났음을 로그로 남긴다 */
	dbg("%s: after wait_for_ctrl_irq\n", __func__);

	/* [한국어] 락을 놓기 직전을 로그로 남긴다 */
	dbg("%s: before up\n", __func__);
	mutex_unlock(&ctrl->crit_sect);
	/* [한국어] 락을 놓았음을 로그로 남긴다 */
	dbg("%s: after up\n", __func__);

	/* Wait for ~1 second because of hot plug spec */
	dbg("%s: before long_delay\n", __func__);
	long_delay(1*HZ);
	/* [한국어] 대기가 끝났음을 로그로 남긴다 */
	dbg("%s: after long_delay\n", __func__);

	dbg("%s: func status = %x\n", __func__, func->status);
	/* Check for a power fault */
	if (func->status == 0xFF) {
		/* power fault occurred, but it was benign */
		temp_register = 0xFFFFFFFF;
		/* [한국어] 전원 결함으로 값을 설정했음을 로그로 남긴다 */
		dbg("%s: temp register set to %x by power fault\n", __func__, temp_register);
		/* [한국어] **전원 결함이 있었다** -- 다만 위의 원문 주석대로 치명적이지는 않다 */
		rc = POWER_FAILURE;
		/* [한국어] 전원 결함 표시를 지운다 */
		func->status = 0;
	} else {
		/* Get vendor/device ID u32 */
		ctrl->pci_bus->number = func->bus;
		/* [한국어] **벤더/장치 ID 를 읽어 카드가 살아 있는지 본다** */
		rc = pci_bus_read_config_dword(ctrl->pci_bus, PCI_DEVFN(func->device, func->function), PCI_VENDOR_ID, &temp_register);
		/* [한국어] 읽기 결과를 로그로 남긴다 */
		dbg("%s: pci_read_config_dword returns %d\n", __func__, rc);
		/* [한국어] 읽은 값을 로그로 남긴다 */
		dbg("%s: temp_register is %x\n", __func__, temp_register);

		if (rc != 0) {
			/* Something's wrong here */
			temp_register = 0xFFFFFFFF;
			/* [한국어] 읽기 실패를 로그로 남긴다 */
			dbg("%s: temp register set to %x by error\n", __func__, temp_register);
		}
		/* Preset return code.  It will be changed later if things go okay. */
		rc = NO_ADAPTER_PRESENT;
	}

	/* All F's is an empty slot or an invalid board */
	if (temp_register != 0xFFFFFFFF) {
		/* [한국어] **컨트롤러의 자유 목록 넷을 꾸러미로 묶는다.** 이 꾸러미가
		 * 하위 함수들이 자원을 떼어 쓸 밑천이다 */
		res_lists.io_head = ctrl->io_head;
		/* [한국어] 메모리 자유 목록을 넘긴다 */
		res_lists.mem_head = ctrl->mem_head;
		/* [한국어] prefetchable 메모리 자유 목록을 넘긴다 */
		res_lists.p_mem_head = ctrl->p_mem_head;
		/* [한국어] 버스 번호 자유 목록을 넘긴다 */
		res_lists.bus_head = ctrl->bus_head;
		/* [한국어] **IRQ 정보는 넘기지 않는다** -- 최상위라 물려받을 것이 없다 */
		res_lists.irqs = NULL;

		/* [한국어] **자원을 배정하고 설정공간을 채운다.** behind_bridge 에 0 을 넘겨
		 * 최상위임을 알린다 */
		rc = configure_new_device(ctrl, func, 0, &res_lists);

		/* [한국어] 돌아왔음을 로그로 남긴다 */
		dbg("%s: back from configure_new_device\n", __func__);
		/* [한국어] **줄어든 목록을 컨트롤러로 되받는다.** 꾸러미를 값으로 넘겼으므로
		 * 갱신된 머리를 이렇게 회수해야 한다 */
		ctrl->io_head = res_lists.io_head;
		/* [한국어] 메모리 목록도 되받는다 */
		ctrl->mem_head = res_lists.mem_head;
		/* [한국어] prefetchable 메모리 목록도 되받는다 */
		ctrl->p_mem_head = res_lists.p_mem_head;
		/* [한국어] 버스 목록도 되받는다 */
		ctrl->bus_head = res_lists.bus_head;

		cpqhp_resource_sort_and_combine(&(ctrl->mem_head));
		cpqhp_resource_sort_and_combine(&(ctrl->p_mem_head));
		cpqhp_resource_sort_and_combine(&(ctrl->io_head));
		cpqhp_resource_sort_and_combine(&(ctrl->bus_head));

		/* [한국어] **설정에 실패했으면 카드를 끈다.** 이미 떼어 쓴 자원은
		 * configure_new_device 가 자기 안에서 회수했다 */
		if (rc) {
			mutex_lock(&ctrl->crit_sect);

			/* [한국어] 황색을 켠다 */
			amber_LED_on(ctrl, hp_slot);
			/* [한국어] 녹색을 끈다 */
			green_LED_off(ctrl, hp_slot);
			/* [한국어] 슬롯을 버스에서 뗀다 */
			slot_disable(ctrl, hp_slot);

			set_SOGO(ctrl);

			/* Wait for SOBS to be unset */
			wait_for_ctrl_irq(ctrl);

			mutex_unlock(&ctrl->crit_sect);
			/* [한국어] 설정 실패를 그대로 올린다 */
			return rc;
		} else {
			/* [한국어] **설정공간 사본을 갱신한다.** 나중에 같은 카드로 교체될 때
			 * 되쓸 밑천이 된다 */
			cpqhp_save_slot_config(ctrl, func);
		}


		/* [한국어] 전원 결함 표시를 지운다 */
		func->status = 0;
		/* [한국어] 레버가 닫혀 있음을 기록한다 */
		func->switch_save = 0x10;
		/* [한국어] **보드로 표시한다.** 이제 이 슬롯은 카드가 꽂힌 것으로 취급된다 */
		func->is_a_board = 0x01;

		/* next, we will instantiate the linux pci_dev structures (with
		 * appropriate driver notification, if already present) */
		dbg("%s: configure linux pci_dev structure\n", __func__);
		/* [한국어] 첫 번째 함수부터 시작한다 */
		index = 0;
		do {
			/* [한국어] 같은 장치의 다음 함수를 찾는다 */
			new_slot = cpqhp_slot_find(ctrl->bus, func->device, index++);
			/* [한국어] **아직 pci_dev 가 없는 함수만 처리한다** -- 중복 등록을 막는다 */
			if (new_slot && !new_slot->pci_dev)
				/* [한국어] **리눅스 pci_dev 를 만든다.** 이 시점에야 리눅스가 카드를 인식하고
				 * 드라이버가 붙는다 */
				cpqhp_configure_device(ctrl, new_slot);
		} while (new_slot);

		mutex_lock(&ctrl->crit_sect);

		/* [한국어] **녹색을 켠다.** 카드가 정상 동작 중이라는 최종 표시다 */
		green_LED_on(ctrl, hp_slot);

		set_SOGO(ctrl);

		/* Wait for SOBS to be unset */
		wait_for_ctrl_irq(ctrl);

		mutex_unlock(&ctrl->crit_sect);
	} else {
		mutex_lock(&ctrl->crit_sect);

		/* [한국어] 황색을 켠다 -- 오류 표시다 */
		amber_LED_on(ctrl, hp_slot);
		/* [한국어] 녹색을 끈다 */
		green_LED_off(ctrl, hp_slot);
		/* [한국어] 슬롯을 버스에서 뗀다 */
		slot_disable(ctrl, hp_slot);

		set_SOGO(ctrl);

		/* Wait for SOBS to be unset */
		wait_for_ctrl_irq(ctrl);

		mutex_unlock(&ctrl->crit_sect);

		/* [한국어] **카드가 없거나 망가졌다** -- NO_ADAPTER_PRESENT 를 돌려준다 */
		return rc;
	}
	return 0;
}


/**
 * remove_board - Turns off slot and LEDs
 * @func: PCI device/function info
 * @replace_flag: whether replacing or adding a new device
 * @ctrl: target controller
 */
/* [한국어]
 * remove_board - 카드를 끄고 자원을 회수한다
 *
 * @func:         대상 PCI 함수 정보.
 * @replace_flag: 1 이면 교체 대비, 0 이면 완전 제거.
 * @ctrl:         해당 컨트롤러.
 * @return: 0 성공, 1 실패.
 *
 * 카드를 뽑을 때 불린다. **자원을 어떻게 기록하고 회수하느냐가
 * replace_flag 와 ctrl->add_support 에 따라 갈린다.**
 *
 *   1) 리눅스 쪽 pci_dev 를 먼저 제거한다. 실패하면 여기서 끝낸다 --
 *      드라이버가 아직 쓰고 있는 카드의 자원을 건드리면 안 되기 때문이다.
 *   2) 자원 정보를 저장한다.
 *      교체 대비이거나 핫애드를 지원하지 않으면
 *        → cpqhp_save_base_addr_length (BAR 크기만 기록)
 *      핫애드를 지원하고 이 카드의 자원 목록이 아직 비어 있으면
 *        → cpqhp_save_used_resources (실제 쓰는 범위를 기록)
 *      **다만 같은 장치의 다른 함수가 이미 자원을 기록해 두었으면
 *      건너뛴다** -- skip 플래그가 그 검사다. 다중 함수 카드에서 한 번만
 *      기록하면 되기 때문이다.
 *   3) 상태를 종료로 바꾸고 LED 를 끄고 슬롯을 비활성화한다.
 *      **SERR 도 함께 끈다** -- 카드가 사라진 슬롯에서 오류 신호가
 *      올라오지 않게 하기 위해서다.
 *   4) 완전 제거이고 핫애드를 지원하면, 같은 장치의 모든 함수를 돌며
 *      cpqhp_return_board_resources 로 자원을 자유 목록에 반납하고
 *      노드를 지운다. 브리지면 bridge_slot_remove 로 하위까지 지운다.
 *      그 뒤 **빈 슬롯을 나타내는 자리표 노드를 새로 만들어 둔다** --
 *      그래야 다음에 카드가 꽂혔을 때 cpqhp_slot_find 가 찾을 수 있다.
 *
 * 4)의 while 루프가 `func = cpqhp_slot_find(ctrl->bus, device, 0)` 로
 * 매번 첫 번째를 다시 찾는 방식이라, 노드가 사라져도 안전하다.
 * device 를 미리 지역 변수에 복사해 둔 이유가 이것이다 -- func 가
 * 해제된 뒤에는 func->device 를 읽을 수 없기 때문이다.
 *
 * **교체 경로에서는 노드를 지우지 않는다.** 같은 카드가 다시 꽂힐 것을
 * 전제하므로 설정공간 사본과 노드를 그대로 두었다가 되쓴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   cpqhp_process_SS → [이 함수]
 *     → cpqhp_unconfigure_device, cpqhp_save_base_addr_length,
 *       cpqhp_save_used_resources, cpqhp_return_board_resources,
 *       slot_remove / bridge_slot_remove, cpqhp_slot_create
 */
static u32 remove_board(struct pci_func *func, u32 replace_flag, struct controller *ctrl)
{
	int index;
	u8 skip = 0;
	/* [한국어] **이미 자원을 기록한 함수가 있는지 표시하는 플래그** */
	u8 device;
	/* [한국어] 컨트롤러 안의 슬롯 번호 */
	u8 hp_slot;
	/* [한국어] 레지스터 값 임시 변수 */
	u8 temp_byte;
	/* [한국어] 자원을 반납할 때 쓸 꾸러미 */
	struct resource_lists res_lists;
	/* [한국어] 자원 기록 여부를 확인할 때 순회할 노드 */
	struct pci_func *temp_func;

	/* [한국어] **리눅스 pci_dev 를 먼저 제거한다.** 이것이 반드시 첫 단계다 */
	if (cpqhp_unconfigure_device(func))
		/* [한국어] **제거에 실패하면 여기서 끝낸다.** 드라이버가 아직 쓰고 있는
		 * 카드의 BAR 를 건드리면 안 되기 때문이다 */
		return 1;

	/* [한국어] **장치 번호를 미리 복사해 둔다** -- 아래에서 func 가 해제되기
	 * 때문이다 */
	device = func->device;

	/* [한국어] 컨트롤러 안의 슬롯 번호로 바꾼다 */
	hp_slot = func->device - ctrl->slot_device_offset;
	/* [한국어] 슬롯 번호를 로그로 남긴다 */
	dbg("In %s, hp_slot = %d\n", __func__, hp_slot);

	/* When we get here, it is safe to change base address registers.
	 * We will attempt to save the base address register lengths */
	if (replace_flag || !ctrl->add_support)
		/* [한국어] **교체 대비이거나 핫애드를 지원하지 않으면 BAR 크기만 기록한다.**
		 * 교체 검증에는 크기만 있으면 되기 때문이다 */
		cpqhp_save_base_addr_length(ctrl, func);
	/* [한국어] **핫애드를 지원하고 이 노드에 자원 기록이 아직 없으면** 실제로
	 * 쓰는 범위를 읽어 둔다 */
	else if (!func->bus_head && !func->mem_head &&
		 !func->p_mem_head && !func->io_head) {
		/* Here we check to see if we've saved any of the board's
		 * resources already.  If so, we'll skip the attempt to
		 * determine what's being used. */
		index = 0;
		/* [한국어] 첫 번째 함수부터 확인한다 */
		temp_func = cpqhp_slot_find(func->bus, func->device, index++);
		/* [한국어] 같은 장치의 모든 함수를 훑는다 */
		while (temp_func) {
			/* [한국어] 버스나 메모리 자원이 기록되어 있는지 본다 */
			if (temp_func->bus_head || temp_func->mem_head
			    /* [한국어] prefetchable 메모리나 IO 도 확인한다 */
			    || temp_func->p_mem_head || temp_func->io_head) {
				/* [한국어] **하나라도 기록해 두었으면 건너뛴다** -- 다중 함수 카드에서
				 * 한 번만 기록하면 되기 때문이다 */
				skip = 1;
				break;
			}
			/* [한국어] 같은 장치의 다음 함수를 본다 */
			temp_func = cpqhp_slot_find(temp_func->bus, temp_func->device, index++);
		}

		/* [한국어] 이미 기록해 둔 함수가 없을 때만 한다 */
		if (!skip)
			/* [한국어] **실제로 쓰는 주소 범위를 읽어 기록한다.** 나중에 자유 목록으로
			 * 돌려주려면 어디를 쓰는지 알아야 한다 */
			cpqhp_save_used_resources(ctrl, func);
	}
	/* Change status to shutdown */
	if (func->is_a_board)
		/* [한국어] **보드였으면 종료 상태로 표시한다** */
		func->status = 0x01;
	/* [한국어] **설정되지 않음으로 표시한다** */
	func->configured = 0;

	mutex_lock(&ctrl->crit_sect);

	/* [한국어] 녹색을 끈다 -- 이제 이 슬롯은 꺼진 것으로 보인다 */
	green_LED_off(ctrl, hp_slot);
	/* [한국어] 슬롯을 버스에서 뗀다 */
	slot_disable(ctrl, hp_slot);

	set_SOGO(ctrl);

	/* turn off SERR for slot */
	temp_byte = readb(ctrl->hpc_reg + SLOT_SERR);
	/* [한국어] **이 슬롯의 SERR 비트를 지운다.** 카드가 사라진 슬롯에서 오류
	 * 신호가 올라오지 않게 하기 위해서다 */
	temp_byte &= ~(0x01 << hp_slot);
	/* [한국어] 바꾼 값을 쓴다 */
	writeb(temp_byte, ctrl->hpc_reg + SLOT_SERR);

	/* Wait for SOBS to be unset */
	wait_for_ctrl_irq(ctrl);

	mutex_unlock(&ctrl->crit_sect);

	/* [한국어] **완전 제거이고 핫애드를 지원할 때만 자원을 회수한다.**
	 * 교체 대비이면 노드를 그대로 두었다가 되쓴다 */
	if (!replace_flag && ctrl->add_support) {
		/* [한국어] **같은 장치의 모든 함수를 지울 때까지 돈다** */
		while (func) {
			/* [한국어] 컨트롤러의 자유 목록을 꾸러미로 묶는다 */
			res_lists.io_head = ctrl->io_head;
			/* [한국어] 메모리 자유 목록을 넘긴다 */
			res_lists.mem_head = ctrl->mem_head;
			/* [한국어] prefetchable 메모리 자유 목록을 넘긴다 */
			res_lists.p_mem_head = ctrl->p_mem_head;
			/* [한국어] 버스 번호 자유 목록을 넘긴다 */
			res_lists.bus_head = ctrl->bus_head;

			/* [한국어] **이 노드가 쓰던 자원을 자유 목록으로 돌려준다.**
			 * 아래 slot_remove 가 노드를 해제하기 전에 반드시 해야 한다 */
			cpqhp_return_board_resources(func, &res_lists);

			/* [한국어] 갱신된 목록을 컨트롤러로 되받는다 */
			ctrl->io_head = res_lists.io_head;
			/* [한국어] 메모리 목록도 되받는다 */
			ctrl->mem_head = res_lists.mem_head;
			/* [한국어] prefetchable 메모리 목록도 되받는다 */
			ctrl->p_mem_head = res_lists.p_mem_head;
			/* [한국어] 버스 목록도 되받는다 */
			ctrl->bus_head = res_lists.bus_head;

			cpqhp_resource_sort_and_combine(&(ctrl->mem_head));
			cpqhp_resource_sort_and_combine(&(ctrl->p_mem_head));
			cpqhp_resource_sort_and_combine(&(ctrl->io_head));
			cpqhp_resource_sort_and_combine(&(ctrl->bus_head));

			/* [한국어] 브리지인지 확인한다 */
			if (is_bridge(func)) {
				bridge_slot_remove(func);
			/* [한국어] 브리지면 하위 버스의 노드까지 지운다 */
			} else
				slot_remove(func);

			/* [한국어] **같은 장치의 첫 노드를 다시 찾는다.** 매번 처음부터 찾으므로
			 * 노드가 사라져도 안전하다. NULL 이면 루프가 끝난다 */
			func = cpqhp_slot_find(ctrl->bus, device, 0);
		}

		/* Setup slot structure with entry for empty slot */
		func = cpqhp_slot_create(ctrl->bus);

		/* [한국어] 할당 실패를 확인한다 */
		if (func == NULL)
			/* [한국어] 노드를 못 만들면 포기한다 */
			return 1;

		/* [한국어] 컨트롤러의 버스에 둔다 */
		func->bus = ctrl->bus;
		/* [한국어] **미리 복사해 둔 장치 번호를 쓴다** -- 원래 func 는 이미 해제되었다 */
		func->device = device;
		/* [한국어] 함수 0 으로 둔다 */
		func->function = 0;
		/* [한국어] 아직 설정되지 않았다 */
		func->configured = 0;
		/* [한국어] 레버가 닫혀 있다고 기록해 둔다 */
		func->switch_save = 0x10;
		/* [한국어] 보드가 아님을 표시한다 */
		func->is_a_board = 0;
		/* [한국어] **타이머 포인터를 지운다** -- 빈 슬롯에는 예약된 작업이 없다 */
		func->p_task_event = NULL;
	}

	return 0;
}

/* [한국어]
 * pushbutton_helper_thread - 5초 타이머가 만료되면 이벤트 스레드를 깨운다
 *
 * @t: 만료된 타이머.
 * @return: 없음.
 *
 * **이름과 달리 스레드가 아니라 타이머 콜백이다.** 세 줄뿐이며,
 * 전역 pushbutton_pending 에 타이머 포인터를 넣고 전역 이벤트 스레드를
 * 깨운다.
 *
 * 왜 이런 구조인가: 타이머 콜백은 소프트 인터럽트 컨텍스트라 잠들 수
 * 없는데, 실제로 할 일(전원 넣기, 카드 설정)은 초 단위로 잠든다.
 * 그래서 여기서는 "할 일이 생겼다" 는 표시만 남기고 넘긴다.
 *
 * **전역 변수 하나가 타이머와 스레드 사이의 유일한 통로다.**
 * event_thread 가 pushbutton_pending 이 NULL 이 아니면 버튼 처리로
 * 분기하고, cpqhp_pushbutton_thread 가 맨 먼저 그것을 NULL 로 되돌린다.
 *
 * 이 방식의 한계가 분명하다 -- **슬롯 여러 개의 타이머가 거의 동시에
 * 만료되면 나중 것이 앞의 것을 덮어쓴다.** 전역 변수가 하나뿐이기
 * 때문이다. 뒷세대 shpchp 가 슬롯마다 워크큐를 두는 이유 중 하나가
 * 이런 문제로 보이나, 그 인과를 코드가 적어 두지는 않았다.
 *
 * 실행 컨텍스트: 타이머(소프트 인터럽트). 잠들 수 없다.
 *
 * 호출 체인:
 *   add_timer 로 건 5초 만료 → 커널 타이머 → [이 함수]
 *     → wake_up_process(cpqhp_event_thread)
 */
static void pushbutton_helper_thread(struct timer_list *t)
{
	pushbutton_pending = t;

	wake_up_process(cpqhp_event_thread);
}


/* this is the main worker thread */
/* [한국어]
 * event_thread - 전역 커널 스레드의 본체
 *
 * @data: kthread_run 에 넘긴 인자. 쓰지 않는다.
 * @return: 항상 0. 종료 요청을 받았을 때만 반환한다.
 *
 * **시스템 전체에 이 스레드 하나가 모든 컨트롤러를 담당한다.**
 * 잠들 수 있는 모든 핫플러그 작업이 여기서 돈다.
 *
 * 구조는 단순하다.
 *   1) TASK_INTERRUPTIBLE 로 상태를 바꾸고 schedule() 로 잠든다.
 *   2) 누가 wake_up_process 로 깨우면 이어서 실행된다.
 *   3) 종료 요청이면 루프를 빠져나간다.
 *   4) **pushbutton_pending 이 있으면 버튼 처리로, 없으면 모든 컨트롤러의
 *      이벤트 큐 처리로 분기한다.**
 *
 * 깨우는 쪽이 둘이다 -- cpqhp_ctrl_intr(하드 인터럽트)과
 * pushbutton_helper_thread(타이머). 두 경로가 같은 스레드를 공유하면서
 * 전역 변수 하나로 구별된다.
 *
 * **set_current_state 와 schedule 을 직접 쓰는 것이 옛 관용구다.**
 * 지금이라면 wait_event_interruptible 이나 워크큐를 쓸 자리다.
 * 이 방식은 깨움이 상태 변경보다 먼저 오면 놓칠 수 있는 구조인데,
 * 여기서는 깨우는 쪽이 wake_up_process 를 쓰므로 잠들기 직전에 깨워도
 * 상태가 TASK_RUNNING 으로 바뀌어 schedule 이 곧바로 돌아온다.
 *
 * 4)에서 두 경로가 배타적인 것에 주의 -- 버튼 처리를 하는 동안에는
 * 컨트롤러 큐를 보지 않는다. 큐에 쌓인 이벤트는 다음 깨움 때 처리된다.
 *
 * **뒷세대와의 대비**: shpchp 는 슬롯마다 워크큐를 두고, pciehp 는
 * request_threaded_irq 의 IRQ 스레드를 쓴다. 둘 다 소유 관계가 좁고
 * 전역 상태가 없다. 이 파일의 전역 스레드 하나는 그 이전 형태다.
 *
 * 실행 컨텍스트: 커널 스레드(프로세스 컨텍스트). 잠들 수 있다.
 *
 * 호출 체인:
 *   cpqhp_event_start_thread 가 kthread_run 으로 띄운다 → [이 함수]
 *     → cpqhp_pushbutton_thread 또는 interrupt_event_handler
 */
static int event_thread(void *data)
{
	struct controller *ctrl;

	/* [한국어] **무한 루프.** 종료 요청을 받았을 때만 break 로 빠져나간다 */
	while (1) {
		/* [한국어] 잠들기 직전을 로그로 남긴다 */
		dbg("!!!!event_thread sleeping\n");
		set_current_state(TASK_INTERRUPTIBLE);
		schedule();

		/* [한국어] **종료 요청을 확인한다.** schedule 바로 뒤에 있어야 잠에서 깬 직후
		 * 알아챌 수 있다 */
		if (kthread_should_stop())
			break;
		/* Do stuff here */
		if (pushbutton_pending)
			cpqhp_pushbutton_thread(pushbutton_pending);
		else
			/* [한국어] **모든 컨트롤러의 이벤트 큐를 훑는다.** 전역 리스트를 순회하는데,
			 * 스레드가 하나뿐이라 이렇게 해야 한다 */
			for (ctrl = cpqhp_ctrl_list; ctrl; ctrl = ctrl->next)
				interrupt_event_handler(ctrl);
	}
	/* [한국어] 종료를 로그로 남긴다 */
	dbg("event_thread signals exit\n");
	return 0;
}

/* [한국어]
 * cpqhp_event_start_thread - 전역 이벤트 스레드를 띄운다
 *
 * @return: 0 성공, 실패하면 PTR_ERR 로 변환한 음수.
 *
 * kthread_run 으로 event_thread 를 "phpd_event" 라는 이름으로 띄운다.
 * 그 이름은 ps 나 top 에서 보인다.
 *
 * **컨트롤러마다 부르는 것이 아니라 드라이버 전체에서 한 번만 부른다.**
 * cpqphp_core.c 의 모듈 초기화 경로에서 불린다.
 *
 * IS_ERR 로 실패를 확인하는 것에 주의 -- kthread_run 은 실패를 오류
 * 포인터로 돌려주므로 NULL 검사로는 잡을 수 없다.
 *
 * **실패해도 cpqhp_event_thread 에 오류 포인터가 남는다.** 그 값을
 * NULL 로 되돌리지 않으므로, 호출자가 오류를 무시하면
 * cpqhp_event_stop_thread 가 오류 포인터를 kthread_stop 에 넘기게 된다.
 * 호출자가 실제로 오류를 확인하는지는 cpqphp_core.c 를 봐야 하는데,
 * 그 파일은 아직 주석되지 않았으므로 여기서는 사실만 적어 둔다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(모듈 초기화).
 *
 * 호출 체인:
 *   cpqphp_core.c 의 초기화 → [이 함수] → kthread_run
 */
int cpqhp_event_start_thread(void)
{
	cpqhp_event_thread = kthread_run(event_thread, NULL, "phpd_event");
	/* [한국어] **kthread_run 은 실패를 오류 포인터로 돌려주므로** NULL 검사로는
	 * 잡을 수 없다 */
	if (IS_ERR(cpqhp_event_thread)) {
		/* [한국어] 스레드를 못 띄웠음을 알린다 */
		err("Can't start up our event thread\n");
		return PTR_ERR(cpqhp_event_thread);
	}

	return 0;
}


/* [한국어]
 * cpqhp_event_stop_thread - 전역 이벤트 스레드를 멈춘다
 *
 * @return: 없음.
 *
 * kthread_stop 한 줄이다. 그 함수는 스레드에 종료 요청을 걸고
 * 깨운 뒤 실제로 끝날 때까지 기다린다.
 *
 * event_thread 쪽에서는 kthread_should_stop 이 참이 되어 루프를
 * 빠져나간다. 다만 그 검사가 **schedule() 바로 뒤에 있어**,
 * 스레드가 잠들어 있어야 종료 요청을 알아챈다. kthread_stop 이
 * 깨워 주므로 성립한다.
 *
 * **반환값을 버린다.** kthread_stop 은 스레드의 반환값을 돌려주는데
 * 여기서는 무시한다. event_thread 가 항상 0 을 돌려주므로 실질적
 * 정보가 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(모듈 해제). 스레드가 끝날 때까지
 * 잠긴다.
 *
 * 호출 체인:
 *   cpqphp_core.c 의 해제 → [이 함수] → kthread_stop
 */
void cpqhp_event_stop_thread(void)
{
	kthread_stop(cpqhp_event_thread);
}


/* [한국어]
 * interrupt_event_handler - 이벤트 큐를 비우는 상태 기계
 *
 * @ctrl: 처리할 컨트롤러.
 * @return: 없음.
 *
 * **이 드라이버 상태 기계의 본체다.** 인터럽트가 큐에 쌓아 둔 사건을
 * 꺼내 슬롯의 state 를 옮기고 LED 를 바꾼다.
 *
 * 바깥 while 루프가 특이하다 -- 열 칸을 한 번 훑어 **하나라도 처리했으면
 * 처음부터 다시 훑는다.** 처리 도중 인터럽트가 새 사건을 넣을 수 있기
 * 때문이며, change 플래그가 그 판단이다. 큐가 완전히 빌 때까지 돈다.
 *
 * 처리하는 사건은 네 가지다.
 *   INT_BUTTON_PRESS  : **아무 일도 하지 않는다.** 로그만 찍는다.
 *     실제 동작은 버튼을 뗐을 때 결정되기 때문이다.
 *   INT_BUTTON_CANCEL : 5초 타이머를 지우고 LED 를 원래대로 되돌린다.
 *     깜빡이던 방향에 따라 녹색을 켜거나 끈다 --
 *       BLINKINGOFF(끄려던 중)였으면 → 켜진 상태로 되돌린다.
 *       BLINKINGON(켜려던 중)였으면 → 꺼진 상태로 되돌린다.
 *     그리고 state 를 STATIC 으로 되돌린다.
 *   INT_BUTTON_RELEASE: 실제 동작을 예약한다. 현재 슬롯이 켜져 있으면
 *     BLINKINGOFF, 꺼져 있으면 BLINKINGON 으로 state 를 옮기고,
 *     녹색 LED 를 깜빡이고 **5초 타이머를 건다.**
 *     그 5초가 사용자에게 주는 취소 기회다.
 *   INT_POWER_FAULT   : 로그만 찍는다. 실제 대응은
 *     handle_power_fault 가 인터럽트 때 이미 했다.
 *
 * 처리한 칸은 event_type 을 0 으로 지워 빈 칸임을 표시한다.
 *
 * **func 와 p_slot 을 NULL 검사한다** -- handle_switch_change 계열이
 * 같은 호출 뒤에 검사하지 않는 것과 대비된다. 다만 NULL 이면 return 으로
 * 함수 전체를 빠져나가므로, 큐에 남은 다른 사건도 처리되지 못한다.
 *
 * 타이머를 걸기 전에 p_slot->hp_slot 과 ctrl 을 채워 두는 것에 주의 --
 * cpqhp_pushbutton_thread 가 타이머에서 struct slot 을 되찾은 뒤
 * 그 필드로 컨트롤러까지 거슬러 올라가기 때문이다.
 *
 * 주석 처리된 `p_slot->physical_slot = physical_slot;` 한 줄이 남아 있다.
 * 원문 그대로 둔다.
 *
 * 실행 컨텍스트: 커널 스레드(프로세스 컨텍스트). crit_sect 를 잡았다
 * 놓기를 반복한다.
 *
 * 호출 체인:
 *   event_thread → [이 함수]
 *     → cpqhp_slot_find, cpqhp_find_slot, LED 인라인 함수들,
 *       set_SOGO, wait_for_ctrl_irq, add_timer
 */
static void interrupt_event_handler(struct controller *ctrl)
{
	int loop;
	/* [한국어] **한 바퀴에 하나라도 처리했는지 표시하는 플래그.**
	 * 1 로 시작해 루프에 진입한다 */
	int change = 1;
	/* [한국어] 이 슬롯의 함수 노드 */
	struct pci_func *func;
	/* [한국어] 컨트롤러 안의 슬롯 번호 */
	u8 hp_slot;
	/* [한국어] 사용자에게 보이는 슬롯 */
	struct slot *p_slot;

	/* [한국어] **큐가 완전히 빌 때까지 반복한다** */
	while (change) {
		/* [한국어] 한 바퀴 시작 전에 내려 둔다 */
		change = 0;

		for (loop = 0; loop < 10; loop++) {
			/* dbg("loop %d\n", loop); */
			if (ctrl->event_queue[loop].event_type != 0) {
				/* [한국어] 어느 슬롯의 사건인지 꺼낸다 */
				hp_slot = ctrl->event_queue[loop].hp_slot;

				/* [한국어] 이 슬롯의 첫 번째 함수 노드를 찾는다 */
				func = cpqhp_slot_find(ctrl->bus, (hp_slot + ctrl->slot_device_offset), 0);
				/* [한국어] **노드를 못 찾으면 함수 전체를 빠져나간다.**
				 * handle_switch_change 계열이 같은 호출 뒤에 검사하지 않는 것과 대비된다 */
				if (!func)
					return;

				/* [한국어] 사용자에게 보이는 슬롯을 찾는다 */
				p_slot = cpqhp_find_slot(ctrl, hp_slot + ctrl->slot_device_offset);
				/* [한국어] **슬롯을 못 찾으면 함수 전체를 빠져나간다.** 큐에 남은 다른
				 * 사건도 처리되지 못한다 */
				if (!p_slot)
					return;

				/* [한국어] 어느 슬롯의 사건인지 로그로 남긴다 */
				dbg("hp_slot %d, func %p, p_slot %p\n",
				    hp_slot, func, p_slot);

				/* [한국어] 버튼 누름인지 본다 */
				if (ctrl->event_queue[loop].event_type == INT_BUTTON_PRESS) {
					/* [한국어] **누름은 로그만 찍는다.** 실제 동작은 뗐을 때 결정된다 */
					dbg("button pressed\n");
				/* [한국어] **버튼 취소인지 본다** -- 5초 안에 버튼을 다시 누른 경우다 */
				} else if (ctrl->event_queue[loop].event_type ==
					   INT_BUTTON_CANCEL) {
					/* [한국어] 취소를 로그로 남긴다 */
					dbg("button cancel\n");
					timer_delete(&p_slot->task_event);

					mutex_lock(&ctrl->crit_sect);

					if (p_slot->state == BLINKINGOFF_STATE) {
						/* slot is on */
						dbg("turn on green LED\n");
						/* [한국어] **끄려던 중이었으면 켜진 상태로 되돌린다** */
						green_LED_on(ctrl, hp_slot);
					} else if (p_slot->state == BLINKINGON_STATE) {
						/* slot is off */
						dbg("turn off green LED\n");
						/* [한국어] **켜려던 중이었으면 꺼진 상태로 되돌린다** */
						green_LED_off(ctrl, hp_slot);
					}

					/* [한국어] 사용자에게 취소되었음을 알린다 */
					info(msg_button_cancel, p_slot->number);

					/* [한국어] **진행 중 표시를 푼다** -- 취소되었으므로 원래 상태로 돌아간다 */
					p_slot->state = STATIC_STATE;

					/* [한국어] 황색도 끈다 */
					amber_LED_off(ctrl, hp_slot);

					set_SOGO(ctrl);

					/* Wait for SOBS to be unset */
					wait_for_ctrl_irq(ctrl);

					mutex_unlock(&ctrl->crit_sect);
				}
				/*** button Released (No action on press...) */
				else if (ctrl->event_queue[loop].event_type == INT_BUTTON_RELEASE) {
					/* [한국어] 버튼을 뗐음을 로그로 남긴다 */
					dbg("button release\n");

					/* [한국어] **현재 켜져 있는지 보고 방향을 정한다** */
					if (is_slot_enabled(ctrl, hp_slot)) {
						/* [한국어] 켜져 있음을 로그로 남긴다 */
						dbg("slot is on\n");
						/* [한국어] **끄려는 중** 으로 표시한다. 5초 안에 버튼을 다시 누르면 취소된다 */
						p_slot->state = BLINKINGOFF_STATE;
						/* [한국어] 끄는 중임을 알린다 */
						info(msg_button_off, p_slot->number);
					} else {
						/* [한국어] 꺼져 있음을 로그로 남긴다 */
						dbg("slot is off\n");
						/* [한국어] **켜려는 중** 으로 표시한다 */
						p_slot->state = BLINKINGON_STATE;
						/* [한국어] 켜는 중임을 알린다 */
						info(msg_button_on, p_slot->number);
					}
					mutex_lock(&ctrl->crit_sect);

					/* [한국어] LED 조작을 로그로 남긴다 */
					dbg("blink green LED and turn off amber\n");

					/* [한국어] 황색을 끈다 */
					amber_LED_off(ctrl, hp_slot);
					/* [한국어] 녹색을 깜빡여 진행 중임을 알린다 */
					green_LED_blink(ctrl, hp_slot);

					set_SOGO(ctrl);

					/* Wait for SOBS to be unset */
					wait_for_ctrl_irq(ctrl);

					mutex_unlock(&ctrl->crit_sect);
					timer_setup(&p_slot->task_event,
						    pushbutton_helper_thread,
						    0);
					/* [한국어] **슬롯 번호를 타이머 콜백이 쓸 수 있게 저장해 둔다** */
					p_slot->hp_slot = hp_slot;
					p_slot->ctrl = ctrl;
/*					p_slot->physical_slot = physical_slot; */
					p_slot->task_event.expires = jiffies + 5 * HZ;   /* 5 second delay */

					/* [한국어] 타이머를 거는 것을 로그로 남긴다 */
					dbg("add_timer p_slot = %p\n", p_slot);
					add_timer(&p_slot->task_event);
				}
				/***********POWER FAULT */
				else if (ctrl->event_queue[loop].event_type == INT_POWER_FAULT) {
					/* [한국어] **전원 결함은 로그만 찍는다.** 실제 대응은 handle_power_fault 가
					 * 인터럽트 때 이미 했다 */
					dbg("power fault\n");
				}

				/* [한국어] **처리한 칸을 빈 칸으로 표시한다** */
				ctrl->event_queue[loop].event_type = 0;

				/* [한국어] **하나라도 처리했으면 바깥 루프를 한 번 더 돌린다.**
				 * 처리 도중 인터럽트가 새 사건을 넣을 수 있기 때문이다 */
				change = 1;
			}
		/* [한국어] 열 칸을 다 훑었다 */
		}		/* End of FOR loop */
	}
}


/**
 * cpqhp_pushbutton_thread - handle pushbutton events
 * @t: pointer to struct timer_list which holds all timer-related callbacks
 *
 * Scheduled procedure to handle blocking stuff for the pushbuttons.
 * Handles all pending events and exits.
 */
/* [한국어]
 * cpqhp_pushbutton_thread - 5초가 지난 뒤 실제로 전원을 올리거나 내린다
 *
 * @t: 만료된 타이머. 여기서 struct slot 을 되찾는다.
 * @return: 없음.
 *
 * **이름은 스레드지만 전역 이벤트 스레드가 부르는 일반 함수다.**
 * 타이머 콜백(pushbutton_helper_thread)이 스레드를 깨우고, 그 스레드가
 * 이 함수를 부른다.
 *
 * timer_container_of 로 타이머에서 struct slot 을 되찾고, 그 안의
 * ctrl 필드로 컨트롤러까지 올라간다. 그 두 필드는
 * interrupt_event_handler 가 타이머를 걸기 전에 채워 둔 것이다.
 *
 * **맨 먼저 pushbutton_pending 을 NULL 로 되돌린다.** 그러지 않으면
 * event_thread 가 다음 깨움 때 또 버튼 처리로 분기한다.
 *
 * 현재 슬롯 상태에 따라 두 갈래다.
 *   켜져 있으면 → POWEROFF_STATE 로 옮기고 cpqhp_process_SS 로 끈다.
 *   꺼져 있으면 → POWERON_STATE 로 옮기고 cpqhp_process_SI 로 켠다.
 *
 * **실패하면 LED 조합으로 알린다.**
 *   끄기 실패 → 황색과 녹색을 **둘 다 켠다.**
 *   켜기 실패 → 황색을 켜고 녹색을 끈다.
 * 끄기 실패에서 녹색을 켜는 것은 카드가 여전히 동작 중이라는 뜻으로
 * 읽힌다.
 *
 * 어느 쪽이든 마지막에 state 를 STATIC_STATE 로 되돌린다.
 * 성공했든 실패했든 "더 이상 진행 중이 아님" 을 표시하는 것이다.
 *
 * **func 가 NULL 이면 state 를 되돌리지 않고 return 한다.**
 * 그러면 슬롯이 POWEROFF_STATE 나 POWERON_STATE 에 갇혀,
 * handle_presence_change 가 이후의 버튼 누름을 INT_BUTTON_IGNORE 로
 * 처리하게 된다. 코드는 손대지 않고 사실만 적어 둔다.
 *
 * 켜는 쪽만 `if (ctrl != NULL)` 검사가 있고 끄는 쪽에는 없다 --
 * 두 경로의 방어 수준이 다르다.
 *
 * 실행 컨텍스트: 커널 스레드. 하위 함수들이 오래 잠든다.
 *
 * 호출 체인:
 *   event_thread → [이 함수] → cpqhp_process_SI / cpqhp_process_SS
 */
void cpqhp_pushbutton_thread(struct timer_list *t)
{
	u8 hp_slot;
	/* [한국어] 이 슬롯의 함수 노드 */
	struct pci_func *func;
	/* [한국어] **타이머에서 struct slot 을 되찾는다.** task_event 가 slot 안에
	 * 값으로 박혀 있어 container_of 로 바깥을 계산할 수 있다 */
	struct slot *p_slot = timer_container_of(p_slot, t, task_event);
	/* [한국어] **슬롯에서 컨트롤러까지 거슬러 올라간다.** 그 필드도
	 * interrupt_event_handler 가 채워 둔 것이다 */
	struct controller *ctrl = (struct controller *) p_slot->ctrl;

	/* [한국어] **전역 표시를 지운다.** 그러지 않으면 event_thread 가 다음 깨움
	 * 때 또 버튼 처리로 분기한다 */
	pushbutton_pending = NULL;
	/* [한국어] 슬롯 번호를 꺼낸다. interrupt_event_handler 가 타이머를 걸기 전에
	 * 채워 둔 값이다 */
	hp_slot = p_slot->hp_slot;

	/* [한국어] **현재 켜져 있는지에 따라 끄기와 켜기로 갈린다** */
	if (is_slot_enabled(ctrl, hp_slot)) {
		p_slot->state = POWEROFF_STATE;
		/* power Down board */
		func = cpqhp_slot_find(p_slot->bus, p_slot->device, 0);
		/* [한국어] 어느 노드를 다루는지 로그로 남긴다 */
		dbg("In power_down_board, func = %p, ctrl = %p\n", func, ctrl);
		/* [한국어] 노드를 못 찾으면 반환한다 */
		if (!func) {
			/* [한국어] 노드를 못 찾았음을 로그로 남긴다 */
			dbg("Error! func NULL in %s\n", __func__);
			return;
		}

		/* [한국어] **카드 끄기에 실패했는지 본다** */
		if (cpqhp_process_SS(ctrl, func) != 0) {
			/* [한국어] 황색을 켠다 */
			amber_LED_on(ctrl, hp_slot);
			/* [한국어] **녹색도 함께 켠다** -- 카드가 여전히 동작 중이라는 뜻으로 읽힌다.
			 * 켜기 실패에서 녹색을 끄는 것과 대비된다 */
			green_LED_on(ctrl, hp_slot);

			set_SOGO(ctrl);

			/* Wait for SOBS to be unset */
			wait_for_ctrl_irq(ctrl);
		}

		/* [한국어] 진행 중 표시를 푼다 */
		p_slot->state = STATIC_STATE;
	} else {
		p_slot->state = POWERON_STATE;
		/* slot is off */

		/* [한국어] 이 슬롯의 첫 번째 함수 노드를 찾는다 */
		func = cpqhp_slot_find(p_slot->bus, p_slot->device, 0);
		/* [한국어] 어느 노드를 다루는지 로그로 남긴다 */
		dbg("In add_board, func = %p, ctrl = %p\n", func, ctrl);
		/* [한국어] **노드를 못 찾으면 state 를 되돌리지 않고 반환한다.**
		 * 그러면 슬롯이 POWERON_STATE 에 갇혀, 이후 버튼 누름이
		 * INT_BUTTON_IGNORE 로 처리된다 */
		if (!func) {
			/* [한국어] 노드를 못 찾았음을 로그로 남긴다 */
			dbg("Error! func NULL in %s\n", __func__);
			return;
		}

		/* [한국어] **켜는 쪽만 NULL 검사가 있다.** 위의 끄는 경로에는 같은 검사가
		 * 없어 두 갈래의 방어 수준이 다르다 */
		if (ctrl != NULL) {
			/* [한국어] **카드 켜기에 실패했는지 본다** */
			if (cpqhp_process_SI(ctrl, func) != 0) {
				/* [한국어] 황색을 켠다 -- 오류 표시다 */
				amber_LED_on(ctrl, hp_slot);
				/* [한국어] 녹색을 끈다 -- 카드가 켜지지 않았다는 표시다 */
				green_LED_off(ctrl, hp_slot);

				set_SOGO(ctrl);

				/* Wait for SOBS to be unset */
				wait_for_ctrl_irq(ctrl);
			}
		}

		/* [한국어] **진행 중 표시를 푼다.** 성공했든 실패했든 STATIC 으로 되돌려야
		 * 다음 버튼 누름을 받을 수 있다 */
		p_slot->state = STATIC_STATE;
	}
}


/* [한국어]
 * cpqhp_process_SI - 슬롯을 켠다 (Slot Insert)
 *
 * @ctrl: 해당 컨트롤러.
 * @func: 대상 PCI 함수 정보.
 * @return: 0 성공, 그 밖에는 오류.
 *
 * 이름의 SI 는 슬롯 삽입(Slot Insertion)을 뜻하는 것으로 보인다.
 * 사용자가 버튼을 눌러 슬롯을 켜려 할 때, 또는 sysfs 로 활성화를
 * 요청했을 때 불린다.
 *
 * 먼저 **레버가 닫혀 있는지 확인한다.** INT_INPUT_CLEAR 의 해당 비트가
 * 서 있으면 열려 있다는 뜻이라 1 을 돌려주고 끝낸다. 레버가 열린 채로
 * 전원을 넣으면 위험하기 때문이다.
 *
 * 그다음 두 갈래로 갈린다.
 *   이미 보드로 인식된 노드면(func->is_a_board) → board_replaced.
 *     **핫애드용 자원이 없을 때의 경로다.**
 *   아니면 → 노드를 새로 만들고 board_added 를 부른다.
 *     빈 슬롯 자리표 노드를 slot_remove 로 지운 뒤
 *     cpqhp_slot_create 로 새로 만드는데, 이는 자리표에 남아 있던
 *     낡은 정보를 버리기 위한 것으로 보인다.
 *
 * 새 노드를 만들 때 **존재 상태와 레버 상태를 저장해 둔다.**
 * handle_presence_change 가 나중에 버튼인지 존재 변화인지 가릴 때
 * 그 값과 견주기 때문이다. 저장 계산은 handle_switch_change 와 같다.
 *
 * board_added 가 실패하면 **만든 노드를 지우고 빈 슬롯 자리표를 다시
 * 만든다.** 그 코드가 위와 거의 똑같이 반복되는데, 차이는
 * is_a_board 를 0 으로 두는 것뿐이다. 카드가 없는 슬롯으로 되돌리는
 * 것이며, 2000년대 초 코드답게 중복을 함수로 빼지 않았다.
 *
 * 실패 경로에서도 브리지면 bridge_slot_remove 를 쓴다 -- 브리지 뒤에
 * 이미 노드가 생겼을 수 있기 때문이다.
 *
 * **rc 가 초기화되지 않은 채 쓰일 수 있는 경로는 없다** --
 * if/else 두 갈래가 모두 rc 에 대입한다.
 *
 * 실행 컨텍스트: 커널 스레드 또는 sysfs 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   cpqhp_pushbutton_thread / cpqphp_core.c 의 sysfs 콜백 → [이 함수]
 *     → board_replaced 또는 board_added
 */
int cpqhp_process_SI(struct controller *ctrl, struct pci_func *func)
{
	u8 device, hp_slot;
	/* [한국어] 존재 상태를 담을 변수 */
	u16 temp_word;
	/* [한국어] 레지스터 값 임시 변수 */
	u32 tempdword;
	/* [한국어] 하위 함수의 반환값 */
	int rc;
	/* [한국어] 사용자에게 보이는 슬롯 */
	struct slot *p_slot;

	/* [한국어] **0 으로 초기화하지만 곧바로 덮어쓴다** -- 아래에서
	 * readl 결과를 대입하므로 이 초기화는 효과가 없다 */
	tempdword = 0;

	/* [한국어] 장치 번호를 미리 복사해 둔다 -- 아래에서 func 가 바뀌기 때문이다 */
	device = func->device;
	/* [한국어] 컨트롤러 안의 슬롯 번호로 바꾼다. slot_device_offset 이 첫 슬롯의
	 * 장치 번호이므로 그것을 빼면 0부터 세는 번호가 된다 */
	hp_slot = device - ctrl->slot_device_offset;
	/* [한국어] 사용자에게 보이는 슬롯도 찾아 둔다.
	 * **다만 이 함수에서 p_slot 을 쓰는 곳이 없다** */
	p_slot = cpqhp_find_slot(ctrl, device);

	/* Check to see if the interlock is closed */
	tempdword = readl(ctrl->hpc_reg + INT_INPUT_CLEAR);

	/* [한국어] 해당 슬롯의 비트가 서 있으면 레버가 열린 것이다 */
	if (tempdword & (0x01 << hp_slot))
		/* [한국어] **레버가 열려 있으면 켜지 않는다.** 열린 채로 전원을 넣으면
		 * 위험하기 때문이다 */
		return 1;

	/* [한국어] 보드인지 빈 슬롯 자리표인지 가른다 */
	if (func->is_a_board) {
		/* [한국어] **이미 보드로 인식된 노드면 교체 경로를 탄다** -- 저장해 둔
		 * 설정공간을 되쓰는 길이다 */
		rc = board_replaced(func, ctrl);
	} else {
		/* add board */
		slot_remove(func);

		/* [한국어] **노드를 새로 만든다.** 자리표에 남아 있던 낡은 정보를 버리기
		 * 위해서다 */
		func = cpqhp_slot_create(ctrl->bus);
		/* [한국어] 할당 실패를 확인한다 */
		if (func == NULL)
			/* [한국어] 노드를 못 만들면 포기한다 */
			return 1;

		/* [한국어] 컨트롤러의 버스에 있다 */
		func->bus = ctrl->bus;
		/* [한국어] 장치 번호를 기록한다 */
		func->device = device;
		/* [한국어] 함수 0 부터 시작한다 */
		func->function = 0;
		/* [한국어] 아직 설정되지 않았다 */
		func->configured = 0;
		/* [한국어] 보드로 표시한다 */
		func->is_a_board = 1;

		/* We have to save the presence info for these slots */
		temp_word = ctrl->ctrl_int_comp >> 16;
		/* [한국어] **존재 상태를 저장해 둔다.** handle_presence_change 가 나중에
		 * 버튼인지 존재 변화인지 가릴 때 이 값과 견준다 */
		func->presence_save = (temp_word >> hp_slot) & 0x01;
		/* [한국어] 두 번째 존재 감지 비트를 더한다.
		 * **hp_slot + 7 자리를 쓰는 것** 은 이 컨트롤러가 카드 존재를 핀
		 * 두 개로 감지하기 때문으로 보이나, 그 배선의 근거는 이 트리에 없다 */
		func->presence_save |= (temp_word >> (hp_slot + 7)) & 0x02;

		/* [한국어] 레버 상태를 확인한다 */
		if (ctrl->ctrl_int_comp & (0x1L << hp_slot)) {
			/* [한국어] 레버가 열려 있다 */
			func->switch_save = 0;
		} else {
			/* [한국어] 레버가 닫혀 있다 */
			func->switch_save = 0x10;
		}

		/* [한국어] **실제로 카드를 켜고 자원을 배정한다** */
		rc = board_added(func, ctrl);
		/* [한국어] **추가에 실패했으면 만든 노드를 지우고 빈 슬롯 자리표를 다시 만든다.**
		 * 아래 코드가 위와 거의 똑같이 반복되는데, is_a_board 만 0 이다 */
		if (rc) {
			/* [한국어] 브리지인지 확인한다 */
			if (is_bridge(func)) {
				bridge_slot_remove(func);
			/* [한국어] 브리지면 하위까지 지운다 */
			} else
				slot_remove(func);

			/* Setup slot structure with entry for empty slot */
			func = cpqhp_slot_create(ctrl->bus);

			/* [한국어] 할당 실패를 확인한다 */
			if (func == NULL)
				/* [한국어] 노드를 못 만들면 포기한다 */
				return 1;

			/* [한국어] 컨트롤러의 버스에 있다 */
			func->bus = ctrl->bus;
			/* [한국어] 장치 번호를 기록한다 */
			func->device = device;
			/* [한국어] 함수 0 으로 둔다 */
			func->function = 0;
			/* [한국어] 아직 설정되지 않았다 */
			func->configured = 0;
			/* [한국어] **보드가 아님을 표시한다** -- 이것이 위의 성공 경로와 다른 유일한 점이다 */
			func->is_a_board = 0;

			/* We have to save the presence info for these slots */
			temp_word = ctrl->ctrl_int_comp >> 16;
			/* [한국어] **빈 슬롯 자리표에도 존재 상태를 저장해 둔다** -- 다음 인터럽트가
			 * 이 값과 견주기 때문이다 */
			func->presence_save = (temp_word >> hp_slot) & 0x01;
			/* [한국어] 두 번째 존재 감지 비트를 더한다 */
			func->presence_save |=
			(temp_word >> (hp_slot + 7)) & 0x02;

			/* [한국어] 레버 상태를 확인한다 */
			if (ctrl->ctrl_int_comp & (0x1L << hp_slot)) {
				/* [한국어] 레버가 열려 있다 */
				func->switch_save = 0;
			} else {
				/* [한국어] 레버가 닫혀 있다 */
				func->switch_save = 0x10;
			}
		}
	}

	/* [한국어] 실패 여부를 확인한다 */
	if (rc)
		/* [한국어] 실패했으면 로그로 남긴다 */
		dbg("%s: rc = %d\n", __func__, rc);

	/* [한국어] 결과를 돌려준다 */
	return rc;
}


/* [한국어]
 * cpqhp_process_SS - 슬롯을 끈다 (Slot Standby)
 *
 * @ctrl: 해당 컨트롤러.
 * @func: 대상 PCI 함수 정보.
 * @return: 0 성공, 그 밖에는 오류.
 *
 * cpqhp_process_SI 의 짝이다. 카드를 끄기 전에 **끌 수 있는 카드인지
 * 먼저 검사한다.**
 *
 * 같은 장치의 모든 함수를 훑으며 두 가지를 본다.
 *   클래스 코드가 디스플레이면 → REMOVE_NOT_SUPPORTED.
 *     **비디오 카드는 뽑을 수 없다** -- 콘솔이 그 위에 있을 수 있고,
 *     끄면 화면이 사라져 복구할 수 없기 때문으로 보인다.
 *   브리지면 그 브리지 제어 레지스터의 VGA Enable 비트를 본다.
 *     서 있으면 그 뒤에 비디오 카드가 있다는 뜻이라 역시 거절한다.
 *
 * **VGA Enable 검사가 이 함수에서 가장 볼 만한 대목이다.** 브리지 자신은
 * 비디오가 아니지만 그 뒤로 VGA 트래픽을 흘려보내도록 설정되어 있으면,
 * 뽑았을 때 화면이 사라진다. 브리지 뒤를 직접 훑지 않고 그 비트 하나로
 * 판단하는 것이다.
 *
 * 검사를 통과하면 remove_board 를 부른다.
 * replace_flag 는 `!(ctrl->add_support)` 로 정한다 -- 핫애드를
 * 지원하지 않는 컨트롤러에서는 "뽑았다가 같은 카드를 다시 끼울 것" 으로
 * 전제한다는 뜻이다. 위의 원문 주석이 그 값을 인자로 받아야 한다고
 * FIXME 를 남겨 두었다.
 *
 * **while 루프의 조건이 `func && !rc` 인 것에 주의.** 오류가 나면
 * 곧바로 멈추므로 나머지 함수는 검사하지 않는다. 그리고 pci 설정 읽기가
 * 실패하면 그 rc 를 그대로 돌려주는데, 그 값은 PCIBIOS 오류 코드라
 * REMOVE_NOT_SUPPORTED 같은 이 드라이버의 코드와 값 공간이 섞인다.
 *
 * 검사 루프가 끝난 뒤 func 를 index 0 으로 다시 찾는 것에 주의 --
 * 루프가 func 를 NULL 로 만들어 놓았기 때문이다.
 *
 * 실행 컨텍스트: 커널 스레드 또는 sysfs 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   cpqhp_pushbutton_thread / cpqphp_core.c 의 sysfs 콜백 → [이 함수]
 *     → cpqhp_slot_find, cpqhp_find_slot, remove_board
 */
int cpqhp_process_SS(struct controller *ctrl, struct pci_func *func)
{
	u8 device, class_code, header_type, BCR;
	/* [한국어] 같은 장치의 함수를 순회할 첨자 */
	u8 index = 0;
	/* [한국어] 교체 대비인지 완전 제거인지 표시 */
	u8 replace_flag;
	/* [한국어] 반환값. 0 으로 시작해 검사에 걸리면 오류 코드가 들어간다 */
	u32 rc = 0;
	/* [한국어] 장치·함수를 합친 번호 */
	unsigned int devfn;
	/* [한국어] 사용자에게 보이는 슬롯 */
	struct slot *p_slot;
	/* [한국어] 설정공간 접근에 쓸 임시 버스 */
	struct pci_bus *pci_bus = ctrl->pci_bus;

	/* [한국어] **장치 번호를 미리 복사해 둔다** -- 아래에서 func 가 바뀌기 때문이다 */
	device = func->device;
	/* [한국어] **첫 번째 함수부터 검사를 시작한다** */
	func = cpqhp_slot_find(ctrl->bus, device, index++);
	/* [한국어] 사용자에게 보이는 슬롯도 찾아 둔다.
	 * **다만 이 함수에서 p_slot 을 쓰는 곳이 없다** -- 선언과 대입만 있다 */
	p_slot = cpqhp_find_slot(ctrl, device);

	/* Make sure there are no video controllers here */
	while (func && !rc) {
		/* [한국어] 임시 pci_bus 의 번호를 이 함수의 버스로 맞춘다 */
		pci_bus->number = func->bus;
		/* [한국어] 장치·함수 번호를 합친다 */
		devfn = PCI_DEVFN(func->device, func->function);

		/* Check the Class Code */
		rc = pci_bus_read_config_byte(pci_bus, devfn, 0x0B, &class_code);
		/* [한국어] 읽기 실패를 확인한다 */
		if (rc)
			/* [한국어] 읽기에 실패하면 그대로 올린다 */
			return rc;

		if (class_code == PCI_BASE_CLASS_DISPLAY) {
			/* Display/Video adapter (not supported) */
			rc = REMOVE_NOT_SUPPORTED;
		} else {
			/* See if it's a bridge */
			rc = pci_bus_read_config_byte(pci_bus, devfn, PCI_HEADER_TYPE, &header_type);
			/* [한국어] 읽기 실패를 확인한다 */
			if (rc)
				/* [한국어] 읽기에 실패하면 그대로 올린다 */
				return rc;

			/* If it's a bridge, check the VGA Enable bit */
			if ((header_type & PCI_HEADER_TYPE_MASK) == PCI_HEADER_TYPE_BRIDGE) {
				/* [한국어] **브리지 제어 레지스터의 VGA Enable 비트를 본다.** 브리지 뒤를
				 * 직접 훑지 않고 이 비트 하나로 판단한다 */
				rc = pci_bus_read_config_byte(pci_bus, devfn, PCI_BRIDGE_CONTROL, &BCR);
				/* [한국어] 읽기 실패를 확인한다 */
				if (rc)
					/* [한국어] 읽기에 실패하면 그대로 올린다 */
					return rc;

				/* If the VGA Enable bit is set, remove isn't
				 * supported */
				if (BCR & PCI_BRIDGE_CTL_VGA)
					/* [한국어] **VGA 가 켜져 있으면 제거를 거부한다.** 그 뒤에 비디오 카드가
					 * 있다는 뜻이라, 뽑으면 화면이 사라진다 */
					rc = REMOVE_NOT_SUPPORTED;
			}
		}

		/* [한국어] 같은 장치의 다음 함수를 본다 */
		func = cpqhp_slot_find(ctrl->bus, device, index++);
	}

	/* [한국어] **첫 번째 함수를 다시 찾는다.** 위의 검사 루프가 func 를 NULL 로
	 * 만들어 놓았기 때문이다 */
	func = cpqhp_slot_find(ctrl->bus, device, 0);
	if ((func != NULL) && !rc) {
		/* FIXME: Replace flag should be passed into process_SS */
		replace_flag = !(ctrl->add_support);
		/* [한국어] **실제로 카드를 끈다** */
		rc = remove_board(func, replace_flag, ctrl);
	/* [한국어] 검사는 통과했는데 노드가 없는 경우다 */
	} else if (!rc) {
		/* [한국어] **노드를 못 찾았으면 실패로 본다** */
		rc = 1;
	}

	/* [한국어] 결과를 돌려준다 */
	return rc;
}

/**
 * switch_leds - switch the leds, go from one site to the other.
 * @ctrl: controller to use
 * @num_of_slots: number of slots to use
 * @work_LED: LED control value
 * @direction: 1 to start from the left side, 0 to start right.
 */
/* [한국어]
 * switch_leds - LED 를 한 칸씩 밀며 시험 패턴을 만든다
 *
 * @ctrl:         대상 컨트롤러.
 * @num_of_slots: 슬롯 개수. 그만큼 반복한다.
 * @work_LED:     LED 제어 값. **입출력 겸용** 이라 포인터로 받는다.
 * @direction:    1 이면 오른쪽으로, 0 이면 왼쪽으로 민다.
 *
 * **진단용이다.** 실제 핫플러그 동작과는 무관하며, 사용자가 sysfs 로
 * 하드웨어 시험을 요청했을 때만 불린다.
 *
 * 한 칸 밀 때마다 LED 레지스터에 쓰고, SOGO 로 밀어내고,
 * wait_for_ctrl_irq 로 기다린 뒤 0.2초를 더 쉰다.
 * **슬롯 하나당 1초 넘게 걸린다** -- wait_for_ctrl_irq 가 최대 1초를
 * 기다리기 때문이다.
 *
 * work_LED 를 포인터로 받는 이유: 호출자가 이 함수를 연달아 부르며
 * LED 값을 이어서 밀어 나가기 때문이다. 값으로 받으면 매번 처음
 * 상태로 되돌아간다.
 *
 * 방향 인자의 이름과 실제 연산이 어긋나 보인다 -- direction 이 1 이면
 * 오른쪽 시프트(>>)인데, 위의 원문 주석은 "1 이면 왼쪽에서 시작" 이라고
 * 적는다. LED 비트 배치에서 어느 쪽이 물리적 왼쪽인지에 달린 문제이며,
 * 그 배치의 근거는 이 트리에 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 오래 잠든다.
 *
 * 호출 체인:
 *   cpqhp_hardware_test → [이 함수] → set_SOGO, wait_for_ctrl_irq, long_delay
 */
static void switch_leds(struct controller *ctrl, const int num_of_slots,
			u32 *work_LED, const int direction)
{
	int loop;

	/* [한국어] 슬롯 개수만큼 한 칸씩 민다 */
	for (loop = 0; loop < num_of_slots; loop++) {
		if (direction)
			*work_LED = *work_LED >> 1;
		else
			*work_LED = *work_LED << 1;
		writel(*work_LED, ctrl->hpc_reg + LED_CONTROL);

		set_SOGO(ctrl);

		/* Wait for SOGO interrupt */
		wait_for_ctrl_irq(ctrl);

		/* Get ready for next iteration */
		long_delay((2*HZ)/10);
	}
}

/**
 * cpqhp_hardware_test - runs hardware tests
 * @ctrl: target controller
 * @test_num: the number written to the "test" file in sysfs.
 *
 * For hot plug ctrl folks to play with.
 */
/* [한국어]
 * cpqhp_hardware_test - 하드웨어 시험 루틴
 *
 * @ctrl:     대상 컨트롤러.
 * @test_num: sysfs 의 "test" 파일에 쓴 숫자.
 * @return: 항상 0.
 *
 * 위의 원문 주석이 이 함수의 성격을 그대로 밝힌다 --
 * "핫플러그 컨트롤러를 만지는 사람들이 가지고 놀라고 둔 것" 이다.
 * 제품 기능이 아니라 개발·진단용이다.
 *
 * **test 1 만 구현되어 있다.** 2 와 3 은 빈 case 에 "여기에 다른 것",
 * "그리고 더" 라는 주석만 있다. 원문 그대로 둔다.
 *
 * test 1 이 하는 일은 LED 를 이런저런 패턴으로 깜빡이는 것이다.
 *   현재 LED 상태를 저장해 둔다.
 *   0x01010101 로 시작해 좌우로 네 번 훑는다.
 *   0x01010000 과 0x00000101 로 각각 좌우로 훑는다.
 *   슬롯마다 상하위 16비트를 뒤바꿔 가며 깜빡인다.
 *   **저장해 둔 상태로 되돌린다.**
 *
 * LED 값이 32비트인데 슬롯당 두 비트를 쓰는 것으로 보인다 --
 * 0x01010101 이 네 자리에 하나씩 켜진 모양이고, 상하위 16비트를
 * 뒤바꾸는 연산이 나오기 때문이다. 다만 어느 비트가 황색이고 어느
 * 비트가 녹색인지는 cpqphp.h 의 인라인 함수를 봐야 하며,
 * 그 배치의 근거 문서는 이 트리에 없다.
 *
 * **슬롯 수를 SLOT_MASK 레지스터의 하위 니블에서 읽는다.**
 * handle_presence_change 가 같은 레지스터의 **상위** 니블을 슬롯
 * 번호 기준으로 쓰는 것과 대비된다 -- 한 레지스터에 두 정보가 들어 있다.
 *
 * 전체가 끝나는 데 수십 초가 걸린다. 슬롯당 1.2초 남짓한 대기가
 * 수십 번 반복되기 때문이다.
 *
 * 실행 컨텍스트: sysfs 쓰기(프로세스 컨텍스트). 아주 오래 잠근다.
 *
 * 호출 체인:
 *   cpqphp_core.c 의 sysfs 콜백 → [이 함수] → switch_leds, long_delay
 */
int cpqhp_hardware_test(struct controller *ctrl, int test_num)
{
	u32 save_LED;
	/* [한국어] 밀어 가며 쓸 LED 값 */
	u32 work_LED;
	/* [한국어] 순회 첨자 */
	int loop;
	/* [한국어] 슬롯 개수 */
	int num_of_slots;

	/* [한국어] **슬롯 개수를 SLOT_MASK 레지스터의 하위 니블에서 읽는다.**
	 * handle_presence_change 가 같은 레지스터의 상위 니블을 쓰는 것과
	 * 대비된다 -- 한 레지스터에 두 정보가 들어 있다 */
	num_of_slots = readb(ctrl->hpc_reg + SLOT_MASK) & 0x0f;

	/* [한국어] 시험 번호로 갈린다. **1 만 구현되어 있고 2 와 3 은 빈 case 다** */
	switch (test_num) {
	case 1:
		/* Do stuff here! */

		/* Do that funky LED thing */
		/* so we can restore them later */
		save_LED = readl(ctrl->hpc_reg + LED_CONTROL);
		/* [한국어] 네 자리에 하나씩 켜진 패턴으로 시작한다 */
		work_LED = 0x01010101;
		/* [한국어] **좌우로 네 번 훑는다.** work_LED 를 포인터로 넘기므로 값이
		 * 이어져 흘러간다 */
		switch_leds(ctrl, num_of_slots, &work_LED, 0);
		/* [한국어] 두 번째 -- 오른쪽 */
		switch_leds(ctrl, num_of_slots, &work_LED, 1);
		/* [한국어] 세 번째 -- 왼쪽 */
		switch_leds(ctrl, num_of_slots, &work_LED, 0);
		/* [한국어] 네 번째 -- 오른쪽 */
		switch_leds(ctrl, num_of_slots, &work_LED, 1);

		/* [한국어] **상위 절반만 켜진 패턴** 으로 바꾼다 */
		work_LED = 0x01010000;
		/* [한국어] 바꾼 값을 쓴다 */
		writel(work_LED, ctrl->hpc_reg + LED_CONTROL);
		/* [한국어] 한쪽 방향으로 훑는다 */
		switch_leds(ctrl, num_of_slots, &work_LED, 0);
		/* [한국어] 반대 방향으로 훑는다 */
		switch_leds(ctrl, num_of_slots, &work_LED, 1);
		/* [한국어] **하위 절반만 켜진 패턴** 으로 바꾼다 */
		work_LED = 0x00000101;
		/* [한국어] 바꾼 값을 쓴다 */
		writel(work_LED, ctrl->hpc_reg + LED_CONTROL);
		/* [한국어] 한쪽 방향으로 훑는다 */
		switch_leds(ctrl, num_of_slots, &work_LED, 0);
		/* [한국어] 반대 방향으로 훑는다 */
		switch_leds(ctrl, num_of_slots, &work_LED, 1);

		/* [한국어] 상위 절반만 켜진 패턴으로 다시 시작한다 */
		work_LED = 0x01010000;
		/* [한국어] 바꾼 값을 쓴다 */
		writel(work_LED, ctrl->hpc_reg + LED_CONTROL);
		/* [한국어] 슬롯마다 한 바퀴 돈다 */
		for (loop = 0; loop < num_of_slots; loop++) {
			set_SOGO(ctrl);

			/* Wait for SOGO interrupt */
			wait_for_ctrl_irq(ctrl);

			/* Get ready for next iteration */
			long_delay((3*HZ)/10);
			/* [한국어] **상하위 16비트를 맞바꾼다.** LED 값 32비트가 슬롯당 두 비트를
			 * 쓰는 것으로 보이며, 이 연산이 황색과 녹색을 뒤바꾸는 효과를 내는
			 * 것으로 읽힌다. 다만 그 비트 배치의 근거 문서는 이 트리에 없다 */
			work_LED = work_LED >> 16;
			/* [한국어] 바꾼 값을 쓴다 */
			writel(work_LED, ctrl->hpc_reg + LED_CONTROL);

			set_SOGO(ctrl);

			/* Wait for SOGO interrupt */
			wait_for_ctrl_irq(ctrl);

			/* Get ready for next iteration */
			long_delay((3*HZ)/10);
			/* [한국어] **상하위 16비트를 도로 맞바꾼다** */
			work_LED = work_LED << 16;
			/* [한국어] 되돌린 값을 쓴다 */
			writel(work_LED, ctrl->hpc_reg + LED_CONTROL);
			/* [한국어] 한 칸 더 왼쪽으로 민다 */
			work_LED = work_LED << 1;
			/* [한국어] 민 값을 쓴다. 이 세 줄이 슬롯 하나를 지나며 LED 를 한 칸 옮긴다 */
			writel(work_LED, ctrl->hpc_reg + LED_CONTROL);
		}

		/* put it back the way it was */
		writel(save_LED, ctrl->hpc_reg + LED_CONTROL);

		set_SOGO(ctrl);

		/* Wait for SOBS to be unset */
		wait_for_ctrl_irq(ctrl);
		break;
	case 2:
		/* Do other stuff here! */
		break;
	case 3:
		/* and more... */
		break;
	}
	return 0;
}


/**
 * configure_new_device - Configures the PCI header information of one board.
 * @ctrl: pointer to controller structure
 * @func: pointer to function structure
 * @behind_bridge: 1 if this is a recursive call, 0 if not
 * @resources: pointer to set of resource lists
 *
 * Returns 0 if success.
 */
/* [한국어]
 * configure_new_device - 카드 하나의 모든 함수를 설정한다
 *
 * @ctrl:          대상 컨트롤러.
 * @func:          시작할 함수 노드.
 * @behind_bridge: 1 이면 브리지 뒤에서 재귀로 들어온 것.
 * @resources:     자유 목록 꾸러미.
 * @return: 0 성공, 그 밖에는 오류.
 *
 * 다중 함수 카드를 처리하는 바깥 껍데기다. 실제 설정은
 * configure_new_function 이 하고, 이 함수는 **함수를 하나씩 찾아 가며
 * 그것을 반복 호출한다.**
 *
 *   1) 헤더 타입 바이트(오프셋 0x0E)의 비트 7 을 보고 다중 함수인지
 *      판단해 최대 함수 수를 8 또는 1 로 잡는다.
 *   2) 현재 함수를 설정한다.
 *   3) **실패하면 이미 나눠 준 자원을 전부 회수한다** -- 같은 장치의
 *      모든 함수를 돌며 cpqhp_return_board_resources 를 부른다.
 *   4) 다음으로 존재하는 함수를 찾는다. 벤더 ID 를 읽어 오류 값이면
 *      건너뛰고, 유효하면 노드를 새로 만든다.
 *   5) 최대 함수 수에 이를 때까지 반복한다.
 *
 * **3)의 회수 루프에 주의.** `while (new_slot)` 안에서 new_slot 을
 * cpqhp_slot_find 로 다시 대입하는데, index 가 계속 증가하므로
 * 결국 NULL 이 되어 끝난다. 다만 그 루프는 **현재 실패한 함수의 자원까지
 * 포함해** 같은 장치의 모든 함수를 회수한다.
 *
 * 4)의 안쪽 while 루프가 특이하다 -- stop_it 플래그로 "함수를 하나
 * 찾았으면 멈춘다" 를 표현한다. break 를 쓰지 않고 플래그를 쓰는 것이
 * 그 시절 스타일이다. 그리고 **노드를 만들기만 하고 설정하지 않는다** --
 * 설정은 바깥 do-while 이 다음 바퀴에서 한다.
 *
 * `PCI_POSSIBLE_ERROR(ID)` 로 벤더 ID 가 유효한지 보는데, 이는 전부 1 인
 * 값(0xFFFFFFFF)을 걸러 내는 커널 매크로다. 없는 함수를 읽으면 버스가
 * 그 값을 돌려주기 때문이다.
 *
 * behind_bridge 를 그대로 아래로 넘긴다 -- 재귀 깊이를 세지 않고
 * "브리지 뒤인가 아닌가" 만 구별한다. IRQ 배선을 부모가 할지
 * 자식이 할지 정하는 데 쓰인다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(커널 스레드).
 * **configure_new_function 과 서로를 부르는 상호 재귀** 를 이룬다.
 *
 * 호출 체인:
 *   board_added / configure_new_function → [이 함수]
 *     → configure_new_function, cpqhp_slot_create,
 *       cpqhp_return_board_resources
 */
static u32 configure_new_device(struct controller  *ctrl, struct pci_func  *func,
				 u8 behind_bridge, struct resource_lists  *resources)
{
	u8 temp_byte, function, max_functions, stop_it;
	/* [한국어] 하위 함수의 반환값 */
	int rc;
	/* [한국어] 벤더/장치 ID 를 담을 변수 */
	u32 ID;
	/* [한국어] 이번에 다룰 함수 노드 */
	struct pci_func *new_slot;
	/* [한국어] 같은 장치의 함수를 순회할 첨자 */
	int index;

	/* [한국어] 첫 함수부터 시작한다 */
	new_slot = func;

	dbg("%s\n", __func__);
	/* Check for Multi-function device */
	ctrl->pci_bus->number = func->bus;
	/* [한국어] **헤더 타입 바이트(오프셋 0x0E)를 읽는다.** PCI_HEADER_TYPE 상수를
	 * 쓰지 않고 숫자를 직접 쓴 것이 그 시절 스타일이다 */
	rc = pci_bus_read_config_byte(ctrl->pci_bus, PCI_DEVFN(func->device, func->function), 0x0E, &temp_byte);
	/* [한국어] 읽기 실패를 확인한다 */
	if (rc) {
		/* [한국어] 실패를 로그로 남긴다 */
		dbg("%s: rc = %d\n", __func__, rc);
		/* [한국어] 읽기에 실패하면 그대로 올린다 */
		return rc;
	}

	/* [한국어] 헤더 타입의 비트 7 이 다중 함수 표시다.
	 * 오른쪽 원문 주석이 그 뜻을 밝힌다 */
	if (temp_byte & 0x80)	/* Multi-function device */
		/* [한국어] **다중 함수 카드는 함수가 최대 여덟 개다** */
		max_functions = 8;
	else
		/* [한국어] 단일 함수 카드다 */
		max_functions = 1;

	/* [한국어] 함수 0 부터 시작한다 */
	function = 0;

	do {
		/* [한국어] **이 함수 하나의 설정공간을 채운다.** 브리지면 그 안에서
		 * 다시 이 함수를 재귀 호출한다 */
		rc = configure_new_function(ctrl, new_slot, behind_bridge, resources);

		/* [한국어] 설정에 실패했는지 확인한다 */
		if (rc) {
			/* [한국어] 실패를 로그로 남긴다 */
			dbg("configure_new_function failed %d\n", rc);
			/* [한국어] 회수 순회를 처음부터 시작한다 */
			index = 0;

			/* [한국어] **실패했으므로 같은 장치의 모든 함수에서 자원을 회수한다.**
			 * 방금 실패한 함수의 것까지 포함한다 */
			while (new_slot) {
				/* [한국어] 같은 장치의 다음 함수를 찾는다 */
				new_slot = cpqhp_slot_find(new_slot->bus, new_slot->device, index++);

				/* [한국어] 더 찾았는지 확인한다 */
				if (new_slot)
					/* [한국어] **그 함수가 쓰던 자원을 자유 목록으로 돌려준다** */
					cpqhp_return_board_resources(new_slot, resources);
			}

			/* [한국어] 설정 실패를 그대로 올린다 */
			return rc;
		}

		/* [한국어] 다음 함수 번호로 넘어간다 */
		function++;

		/* [한국어] 찾았는지 표시할 플래그를 내린다 */
		stop_it = 0;

		/* The following loop skips to the next present function
		 * and creates a board structure */

		/* [한국어] **최대 함수 수에 이르거나 하나를 찾을 때까지 훑는다** */
		while ((function < max_functions) && (!stop_it)) {
			/* [한국어] 다음 함수의 벤더/장치 ID 를 읽어 존재하는지 본다 */
			pci_bus_read_config_dword(ctrl->pci_bus, PCI_DEVFN(func->device, function), 0x00, &ID);

			/* [한국어] 벤더 ID 가 전부 1 이면 그 함수는 없다 */
			if (PCI_POSSIBLE_ERROR(ID)) {
				/* [한국어] **없는 함수면 다음으로 넘어간다.** 다중 함수 카드라도 번호가
				 * 연속이라는 보장이 없다 */
				function++;
			} else {
				/* Setup slot structure. */
				new_slot = cpqhp_slot_create(func->bus);

				/* [한국어] 할당 실패를 확인한다 */
				if (new_slot == NULL)
					/* [한국어] 노드를 못 만들면 포기한다 */
					return 1;

				/* [한국어] 같은 버스에 있다 */
				new_slot->bus = func->bus;
				/* [한국어] 같은 장치 번호를 쓴다 */
				new_slot->device = func->device;
				/* [한국어] 이 함수의 번호를 기록한다 */
				new_slot->function = function;
				/* [한국어] 보드로 표시한다 */
				new_slot->is_a_board = 1;
				/* [한국어] 상태를 초기화한다 */
				new_slot->status = 0;

				/* [한국어] **함수를 찾았으므로 안쪽 루프를 멈춘다.** break 대신 플래그를 쓰는
				 * 것이 그 시절 스타일이다 */
				stop_it++;
			}
		}

	} while (function < max_functions);
	/* [한국어] 함수를 다 훑었음을 로그로 남긴다 */
	dbg("returning from configure_new_device\n");

	return 0;
}


/*
 * Configuration logic that involves the hotplug data structures and
 * their bookkeeping
 */


/**
 * configure_new_function - Configures the PCI header information of one device
 * @ctrl: pointer to controller structure
 * @func: pointer to function structure
 * @behind_bridge: 1 if this is a recursive call, 0 if not
 * @resources: pointer to set of resource lists
 *
 * Calls itself recursively for bridged devices.
 * Returns 0 if success.
 */
/* [한국어]
 * configure_new_function - PCI 함수 하나의 설정공간을 채운다. 브리지면 재귀한다
 *
 * @ctrl:          대상 컨트롤러.
 * @func:          설정할 함수 노드.
 * @behind_bridge: 1 이면 브리지 뒤에서 들어온 것.
 * @resources:     자유 목록 꾸러미. 여기서 자원을 떼어 쓴다.
 * @return: 0 성공, 그 밖에는 오류 코드.
 *
 * **이 파일에서 가장 긴 함수이자 자원 할당의 실제 소비처다.**
 * 헤더 타입에 따라 세 갈래로 갈린다.
 *
 * -- 브리지인 경우 --
 * 브리지는 자기가 쓸 자원이 아니라 **뒤에 매달릴 장치들이 나눠 쓸 창** 을
 * 받아야 한다. 그래서 절차가 길다.
 *
 *   1) 기본 버스 번호를 쓴다.
 *   2) get_max_resource 로 버스 번호 구간을 통째로 받아, 보조 버스와
 *      종속 버스를 설정한다.
 *   3) 지연 타이머(0x40)와 캐시 라인 크기(0x08)를 쓴다.
 *   4) IO 는 4KiB, 메모리와 prefetchable 메모리는 1MiB 정렬로
 *      get_max_resource 를 불러 창을 받는다.
 *   5) IRQ 정보를 준비한다. 부모에게서 물려받거나 새로 시작한다.
 *   6) **받은 노드를 hold_ 사본으로 복사해 둔다.** 실패했을 때 되돌릴
 *      밑천이며, 성공했을 때는 "이 카드가 쓰는 자원" 목록에 들어간다.
 *   7) 브리지의 base/limit 레지스터에 창의 시작과 끝을 쓴다.
 *      IO 는 8비트 오른쪽 시프트, 메모리는 16비트 시프트인데,
 *      PCI 규격이 그 레지스터에 상위 비트만 담기 때문이다.
 *   8) **버스 번호를 하나 늘려 자식에게 넘긴다** -- 브리지 자신이
 *      보조 버스 번호를 쓰므로 그다음부터가 자식 몫이다.
 *   9) 장치 0~0x1F 를 훑으며 존재하는 것마다 configure_new_device 를
 *      **재귀 호출** 한다. barber_pole 을 한 칸씩 돌려 IRQ 를 분산한다.
 *  10) 자식들이 쓰고 남은 창을 do_pre_bridge_resource_split 과
 *      do_bridge_resource_split 으로 잘라 부모에게 돌려주고,
 *      브리지의 limit 레지스터를 실제로 쓴 만큼으로 줄인다.
 *  11) 명령 레지스터(0x0157)와 브리지 제어(0x07)를 써서 카드를 켠다.
 *      그 두 값이 어떤 플래그의 합인지는 위의 원문 주석에 적혀 있다.
 *
 * **barber_pole 이 이 함수의 독특한 개념이다.** 이름 그대로 이발소
 * 간판처럼 값을 계속 돌리는 것으로, 장치마다 INTA~INTD 배정을 한 칸씩
 * 밀어 인터럽트가 한 선에 몰리지 않게 한다. PCI 규격의 인터럽트 스위즐링
 * 관례를 구현한 것으로 보이나, 그 규격 문서는 이 트리에 없다.
 *
 * -- 일반 장치인 경우 --
 *   1) 클래스 코드가 디스플레이면 거절한다 -- 비디오 카드는 지원하지 않는다.
 *   2) BAR 를 0x10 부터 0x24 까지 훑으며 **0xFFFFFFFF 를 써 보고 되읽어
 *      크기를 알아낸다.** 그 관용구는 cpqphp_pci.c 헤더에 자세히 적었다.
 *      비트 조합으로 종류를 가른다 --
 *        하위 2비트가 01 이면 IO                   → get_io_resource
 *        하위 4비트가 1000 이면 prefetchable 메모리 → get_resource
 *        하위 4비트가 0000 이면 일반 메모리         → get_resource
 *        그 밖이면 NOT_ENOUGH_RESOURCES 로 거절한다.
 *      받은 주소를 BAR 에 쓰고, 그 노드를 func 의 목록에 매단다.
 *   3) 64비트 BAR 이면 다음 칸에 0 을 쓰고 건너뛴다.
 *      **위의 원문 주석이 "요즘 시스템에서는 상위 32비트가 항상 0"
 *      이라면서도 알파와 ia64 에서는 아닐 것이라고 FIXME 를 남겼다.**
 *   4) 레거시 모드면 IRQ 를 배정한다. 부모가 이미 그 핀을 배정했으면
 *      같은 것을 공유하고, 아니면 카드 종류로 정한다 --
 *      **저장장치면 cpqhp_disk_irq, 아니면 cpqhp_nic_irq** 라는
 *      두 전역 중 하나다.
 *   5) 지연 타이머, 캐시 라인, ROM 주소 비활성화, 명령 레지스터를 쓴다.
 *
 * -- 그 밖 --
 *   카드버스 등 알 수 없는 헤더 타입은 DEVICE_TYPE_NOT_SUPPORTED 로 거절한다.
 *
 * **주의할 점 몇 가지.**
 *   rc 에 대입만 하고 검사하지 않는 pci_bus_write_config_ 호출이 많다.
 *     브리지 창을 쓰는 부분 대부분이 그렇다.
 *   4)에서 temp_byte 는 cpqhp_legacy_mode 가 거짓이면 초기화되지 않은
 *     채 아래 cpqhp_set_irq 나 resources->irqs 색인에 쓰인다.
 *     코드는 손대지 않고 사실만 적어 둔다.
 *   free_and_out 라벨은 브리지 경로에서만 도달한다. 임시 목록을 파괴하고
 *     hold_ 사본 넷을 부모 목록으로 돌려준다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 재귀 호출로 스택을 쓴다.
 *
 * 호출 체인:
 *   configure_new_device → [이 함수]
 *     → get_max_resource, get_io_resource, get_resource,
 *       자유 목록에서 떼어 내는 세 함수와, 남는 부분을 잘라 내는
 *       do_pre_bridge_resource_split 및 do_bridge_resource_split,
 *       configure_new_device(재귀), cpqhp_set_irq, cpqhp_slot_create
 */
static int configure_new_function(struct controller *ctrl, struct pci_func *func,
				   u8 behind_bridge,
				   struct resource_lists *resources)
{
	int cloop;
	/* [한국어] 배정할 IRQ 번호. **0 으로 초기화한다** -- 레거시 모드가 아니면
	 * 그대로 0 이 쓰인다 */
	u8 IRQ = 0;
	/* [한국어] 8비트 레지스터 값 임시 변수 */
	u8 temp_byte;
	/* [한국어] 브리지 뒤를 훑을 때의 장치 번호 */
	u8 device;
	/* [한국어] 클래스 코드를 담을 변수 */
	u8 class_code;
	/* [한국어] 명령 레지스터와 브리지 제어 레지스터에 쓸 값 */
	u16 command;
	/* [한국어] 16비트 레지스터 값 임시 변수 */
	u16 temp_word;
	/* [한국어] **선언만 되고 쓰이지 않는 변수다.** ROM 주소를 0 으로 쓰는 곳에서
	 * 쓰이는데, 그 호출이 write_config_word 라 u32 를 넘기는 셈이 된다 */
	u32 temp_dword;
	/* [한국어] 반환값. **대입만 하고 검사하지 않는 곳이 많다** */
	u32 rc;
	/* [한국어] BAR 크기 질의에 쓸 임시 변수 */
	u32 temp_register;
	/* [한국어] BAR 에 써 넣을 주소이자 요구 크기를 담는 겸용 변수 */
	u32 base;
	/* [한국어] 벤더/장치 ID 를 담을 변수 */
	u32 ID;
	/* [한국어] 장치·함수를 합친 번호 */
	unsigned int devfn;
	/* [한국어] 이 카드가 받을 메모리 조각 */
	struct pci_resource *mem_node;
	/* [한국어] 이 카드가 받을 prefetchable 메모리 조각 */
	struct pci_resource *p_mem_node;
	/* [한국어] 이 카드가 받을 IO 조각 */
	struct pci_resource *io_node;
	/* [한국어] 브리지가 받을 버스 번호 구간 */
	struct pci_resource *bus_node;
	/* [한국어] 메모리 노드의 사본 */
	struct pci_resource *hold_mem_node;
	/* [한국어] prefetchable 메모리 노드의 사본 */
	struct pci_resource *hold_p_mem_node;
	/* [한국어] IO 노드의 사본 */
	struct pci_resource *hold_IO_node;
	/* [한국어] 버스 노드의 사본. 실패 시 되돌릴 밑천이다 */
	struct pci_resource *hold_bus_node;
	/* [한국어] **IRQ 배정 상태.** barber_pole 로 핀을 돌려 가며 배정한다 */
	struct irq_mapping irqs;
	/* [한국어] 브리지 뒤에서 새로 만들 노드 */
	struct pci_func *new_slot;
	/* [한국어] 설정공간 접근에 쓸 임시 버스 */
	struct pci_bus *pci_bus;
	/* [한국어] **자식에게 넘길 임시 자원 꾸러미.** 브리지가 받은 창만 담아
	 * 자식들이 그 범위 밖으로 못 나가게 한다 */
	struct resource_lists temp_resources;

	/* [한국어] 컨트롤러가 들고 있는 임시 pci_bus 를 빌린다 */
	pci_bus = ctrl->pci_bus;
	/* [한국어] **임시 pci_bus 의 번호를 이 함수의 버스로 맞춘다.**
	 * 이 파일 전체가 pci_bus 하나를 돌려 쓰는 관용구를 따른다 */
	pci_bus->number = func->bus;
	/* [한국어] 장치·함수 번호를 devfn 하나로 합친다 */
	devfn = PCI_DEVFN(func->device, func->function);

	/* Check for Bridge */
	rc = pci_bus_read_config_byte(pci_bus, devfn, PCI_HEADER_TYPE, &temp_byte);
	/* [한국어] 읽기 실패를 확인한다 */
	if (rc)
		/* [한국어] 헤더 타입을 못 읽으면 포기한다 */
		return rc;

	if ((temp_byte & PCI_HEADER_TYPE_MASK) == PCI_HEADER_TYPE_BRIDGE) {
		/* set Primary bus */
		dbg("set Primary bus = %d\n", func->bus);
		/* [한국어] **기본 버스 번호를 쓴다** -- 이 브리지가 붙어 있는 위쪽 버스다 */
		rc = pci_bus_write_config_byte(pci_bus, devfn, PCI_PRIMARY_BUS, func->bus);
		if (rc)
			/* [한국어] 쓰기에 실패하면 포기한다 */
			return rc;

		/* find range of buses to use */
		dbg("find ranges of buses to use\n");
		/* [한국어] **버스 번호 구간을 통째로 받는다.** 크기 1 을 넘기지만
		 * get_max_resource 는 "그 이상인 가장 큰 것" 을 주므로, 실제로는
		 * 가장 긴 연속 버스 번호 구간을 받는다 */
		bus_node = get_max_resource(&(resources->bus_head), 1);

		/* If we don't have any buses to allocate, we can't continue */
		if (!bus_node)
			return -ENOMEM;

		/* set Secondary bus */
		temp_byte = bus_node->base;
		/* [한국어] 보조 버스 번호를 로그로 남긴다 */
		dbg("set Secondary bus = %d\n", bus_node->base);
		/* [한국어] **보조 버스 번호를 쓴다** -- 이 브리지 바로 아래 버스의 번호다 */
		rc = pci_bus_write_config_byte(pci_bus, devfn, PCI_SECONDARY_BUS, temp_byte);
		if (rc)
			/* [한국어] 쓰기에 실패하면 포기한다 */
			return rc;

		/* set subordinate bus */
		temp_byte = bus_node->base + bus_node->length - 1;
		/* [한국어] 종속 버스 번호를 로그로 남긴다 */
		dbg("set subordinate bus = %d\n", bus_node->base + bus_node->length - 1);
		/* [한국어] 종속 버스 번호를 쓴다 */
		rc = pci_bus_write_config_byte(pci_bus, devfn, PCI_SUBORDINATE_BUS, temp_byte);
		if (rc)
			/* [한국어] 쓰기에 실패하면 포기한다 */
			return rc;

		/* set subordinate Latency Timer and base Latency Timer */
		temp_byte = 0x40;
		/* [한국어] **보조 버스 쪽 지연 타이머를 0x40(64클록)으로 쓴다.**
		 * 버스를 얼마나 오래 점유할 수 있는지 정하는 값이다 */
		rc = pci_bus_write_config_byte(pci_bus, devfn, PCI_SEC_LATENCY_TIMER, temp_byte);
		if (rc)
			/* [한국어] 쓰기에 실패하면 포기한다 */
			return rc;
		/* [한국어] **기본 지연 타이머도 같은 값으로 쓴다.** 브리지는 두 버스에
		 * 걸쳐 있어 타이머가 둘이다 */
		rc = pci_bus_write_config_byte(pci_bus, devfn, PCI_LATENCY_TIMER, temp_byte);
		if (rc)
			/* [한국어] 쓰기에 실패하면 포기한다 */
			return rc;

		/* set Cache Line size */
		temp_byte = 0x08;
		/* [한국어] **캐시 라인 크기를 8 dword(32바이트)로 쓴다.** 그 시절 CPU 의
		 * 캐시 라인 크기에 맞춘 값이다 */
		rc = pci_bus_write_config_byte(pci_bus, devfn, PCI_CACHE_LINE_SIZE, temp_byte);
		if (rc)
			/* [한국어] 쓰기에 실패하면 포기한다 */
			return rc;

		/* Setup the IO, memory, and prefetchable windows */
		io_node = get_max_resource(&(resources->io_head), 0x1000);
		/* [한국어] IO 가 모자라면 포기한다 */
		if (!io_node)
			return -ENOMEM;
		/* [한국어] **메모리 창은 1MiB 정렬이다** -- PCI 브리지의 메모리 base/limit
		 * 레지스터가 상위 16비트만 담아 1MiB 경계에서만 시작할 수 있기 때문이다 */
		mem_node = get_max_resource(&(resources->mem_head), 0x100000);
		/* [한국어] 메모리가 모자라면 포기한다 */
		if (!mem_node)
			return -ENOMEM;
		/* [한국어] **prefetchable 메모리 창도 1MiB 정렬로 잡는다** */
		p_mem_node = get_max_resource(&(resources->p_mem_head), 0x100000);
		/* [한국어] prefetchable 메모리가 모자라면 포기한다 */
		if (!p_mem_node)
			return -ENOMEM;
		/* [한국어] 세 창을 다 잡았음을 로그로 남긴다 */
		dbg("Setup the IO, memory, and prefetchable windows\n");
		/* [한국어] IO 창을 로그로 남긴다 */
		dbg("io_node\n");
		/* [한국어] IO 창의 내용을 로그로 남긴다 */
		dbg("(base, len, next) (%x, %x, %p)\n", io_node->base,
					io_node->length, io_node->next);
		/* [한국어] 메모리 창을 로그로 남긴다 */
		dbg("mem_node\n");
		/* [한국어] 메모리 창의 내용을 로그로 남긴다 */
		dbg("(base, len, next) (%x, %x, %p)\n", mem_node->base,
					mem_node->length, mem_node->next);
		/* [한국어] prefetchable 창을 로그로 남긴다 */
		dbg("p_mem_node\n");
		/* [한국어] prefetchable 창의 내용을 로그로 남긴다 */
		dbg("(base, len, next) (%x, %x, %p)\n", p_mem_node->base,
					p_mem_node->length, p_mem_node->next);

		/* set up the IRQ info */
		if (!resources->irqs) {
			/* [한국어] **부모가 없으므로 처음부터 시작한다.** 이 브리지가 최상위라는 뜻이다 */
			irqs.barber_pole = 0;
			/* [한국어] INTA 를 비운다 */
			irqs.interrupt[0] = 0;
			/* [한국어] INTB 를 비운다 */
			irqs.interrupt[1] = 0;
			/* [한국어] INTC 를 비운다 */
			irqs.interrupt[2] = 0;
			/* [한국어] INTD 를 비운다 */
			irqs.interrupt[3] = 0;
			/* [한국어] 아직 배정된 핀이 없다 */
			irqs.valid_INT = 0;
		} else {
			/* [한국어] **부모의 barber_pole 위치를 이어받는다** -- 형제 장치들과 배정이
			 * 겹치지 않게 하려는 것이다 */
			irqs.barber_pole = resources->irqs->barber_pole;
			/* [한국어] INTA 배정을 물려받는다 */
			irqs.interrupt[0] = resources->irqs->interrupt[0];
			/* [한국어] INTB 배정을 물려받는다 */
			irqs.interrupt[1] = resources->irqs->interrupt[1];
			/* [한국어] INTC 배정을 물려받는다 */
			irqs.interrupt[2] = resources->irqs->interrupt[2];
			/* [한국어] INTD 배정을 물려받는다 */
			irqs.interrupt[3] = resources->irqs->interrupt[3];
			/* [한국어] 유효 핀 마스크도 물려받는다 */
			irqs.valid_INT = resources->irqs->valid_INT;
		}

		/* set up resource lists that are now aligned on top and bottom
		 * for anything behind the bridge. */
		temp_resources.bus_head = bus_node;
		/* [한국어] IO 창. **자식들은 이 꾸러미 안의 자원만 쓸 수 있다** --
		 * 부모의 자유 목록에는 손대지 못한다 */
		temp_resources.io_head = io_node;
		/* [한국어] 메모리 창 */
		temp_resources.mem_head = mem_node;
		/* [한국어] prefetchable 메모리 창 */
		temp_resources.p_mem_head = p_mem_node;
		/* [한국어] IRQ 정보를 꾸러미에 넣는다 */
		temp_resources.irqs = &irqs;

		/* Make copies of the nodes we are going to pass down so that
		 * if there is a problem,we can just use these to free resources
		 */
		hold_bus_node = kmalloc_obj(*hold_bus_node);
		/* [한국어] IO 노드의 사본을 담을 자리 */
		hold_IO_node = kmalloc_obj(*hold_IO_node);
		/* [한국어] 메모리 노드의 사본을 담을 자리 */
		hold_mem_node = kmalloc_obj(*hold_mem_node);
		/* [한국어] prefetchable 노드의 사본을 담을 자리 */
		hold_p_mem_node = kmalloc_obj(*hold_p_mem_node);

		/* [한국어] **넷 중 하나라도 실패하면 전부 해제한다.** kfree 는 NULL 을 무시하므로
		 * 성공한 것만 골라 낼 필요가 없다 */
		if (!hold_bus_node || !hold_IO_node || !hold_mem_node || !hold_p_mem_node) {
			kfree(hold_bus_node);
			kfree(hold_IO_node);
			kfree(hold_mem_node);
			kfree(hold_p_mem_node);

			/* [한국어] 넷 중 하나라도 실패했으면 포기한다 */
			return 1;
		}

		/* [한국어] **버스 노드의 사본을 뜬다.** 위의 원문 주석대로, 문제가 생겼을 때
		 * 이 사본으로 자원을 되돌리기 위해서다 */
		memcpy(hold_bus_node, bus_node, sizeof(struct pci_resource));

		/* [한국어] **버스 번호를 하나 늘려 자식에게 넘긴다.** 브리지 자신이 보조 버스
		 * 번호를 쓰므로 그다음부터가 자식 몫이다 */
		bus_node->base += 1;
		/* [한국어] 길이도 하나 줄인다 */
		bus_node->length -= 1;
		/* [한국어] **연결을 끊어 자식에게는 이 노드 하나만 보이게 한다** */
		bus_node->next = NULL;

		/* If we have IO resources copy them and fill in the bridge's
		 * IO range registers */
		memcpy(hold_IO_node, io_node, sizeof(struct pci_resource));
		/* [한국어] 원본의 연결을 끊는다 */
		io_node->next = NULL;

		/* set IO base and Limit registers */
		temp_byte = io_node->base >> 8;
		/* [한국어] IO 창의 시작을 쓴다 */
		rc = pci_bus_write_config_byte(pci_bus, devfn, PCI_IO_BASE, temp_byte);

		/* [한국어] 끝 주소를 계산해 8비트 오른쪽으로 민다 */
		temp_byte = (io_node->base + io_node->length - 1) >> 8;
		/* [한국어] IO 창의 끝을 쓴다 */
		rc = pci_bus_write_config_byte(pci_bus, devfn, PCI_IO_LIMIT, temp_byte);

		/* Copy the memory resources and fill in the bridge's memory
		 * range registers.
		 */
		memcpy(hold_mem_node, mem_node, sizeof(struct pci_resource));
		/* [한국어] 원본의 연결을 끊어 자식이 이 노드만 보게 한다 */
		mem_node->next = NULL;

		/* set Mem base and Limit registers */
		temp_word = mem_node->base >> 16;
		/* [한국어] 메모리 창의 시작을 쓴다. **16비트 오른쪽으로 미는 것은** 메모리
		 * base/limit 레지스터가 상위 16비트만 담기 때문이다 */
		rc = pci_bus_write_config_word(pci_bus, devfn, PCI_MEMORY_BASE, temp_word);

		/* [한국어] 끝 주소를 계산한다. 길이에서 1 을 빼는 것은 limit 이 마지막 주소를
		 * 담기 때문이다 */
		temp_word = (mem_node->base + mem_node->length - 1) >> 16;
		/* [한국어] 메모리 창의 끝을 쓴다 */
		rc = pci_bus_write_config_word(pci_bus, devfn, PCI_MEMORY_LIMIT, temp_word);

		/* [한국어] prefetchable 노드도 사본을 뜬다 */
		memcpy(hold_p_mem_node, p_mem_node, sizeof(struct pci_resource));
		/* [한국어] **사본을 뜬 뒤 원본의 연결을 끊는다.** 자식에게는 이 노드 하나만
		 * 보이게 해서 그 범위 밖으로 나가지 못하게 한다 */
		p_mem_node->next = NULL;

		/* set Pre Mem base and Limit registers */
		temp_word = p_mem_node->base >> 16;
		/* [한국어] prefetchable 창의 시작을 쓴다 */
		rc = pci_bus_write_config_word(pci_bus, devfn, PCI_PREF_MEMORY_BASE, temp_word);

		/* [한국어] 끝 주소를 계산해 16비트 오른쪽으로 민다 */
		temp_word = (p_mem_node->base + p_mem_node->length - 1) >> 16;
		/* [한국어] prefetchable 창의 끝을 쓴다 */
		rc = pci_bus_write_config_word(pci_bus, devfn, PCI_PREF_MEMORY_LIMIT, temp_word);

		/* Adjust this to compensate for extra adjustment in first loop
		 */
		irqs.barber_pole--;

		/* [한국어] 루프에 들어가기 전에 성공으로 초기화한다 */
		rc = 0;

		/* Here we actually find the devices and configure them */
		for (device = 0; (device <= 0x1F) && !rc; device++) {
			/* [한국어] **barber_pole 을 한 칸 돌린다.** 장치마다 INTA~INTD 배정을 밀어
			 * 인터럽트가 한 선에 몰리지 않게 하는 PCI 스위즐링 관례다.
			 * 3 으로 마스크해 0~3 을 순환한다 */
			irqs.barber_pole = (irqs.barber_pole + 1) & 0x03;

			/* [한국어] 없는 장치를 읽으면 전부 1 이 돌아오므로 그 값으로 초기화해 둔다 */
			ID = 0xFFFFFFFF;
			/* [한국어] **임시 pci_bus 의 번호를 하위 버스로 바꾼다.** 브리지 뒤를 들여다보려면
			 * 그 버스 번호로 설정 사이클을 내보내야 한다 */
			pci_bus->number = hold_bus_node->base;
			/* [한국어] 벤더/장치 ID 를 읽어 장치가 있는지 본다 */
			pci_bus_read_config_dword(pci_bus, PCI_DEVFN(device, 0), 0x00, &ID);
			/* [한국어] **버스 번호를 원래대로 되돌린다.** 임시 pci_bus 하나를 돌려 쓰므로
			 * 읽기가 끝나면 반드시 복원해야 한다 */
			pci_bus->number = func->bus;

			if (!PCI_POSSIBLE_ERROR(ID)) {	  /*  device present */
				/* Setup slot structure. */
				new_slot = cpqhp_slot_create(hold_bus_node->base);

				/* [한국어] 할당 실패를 확인한다 */
				if (new_slot == NULL) {
					/* [한국어] 노드를 못 만들었다 */
					rc = -ENOMEM;
					continue;
				}

				/* [한국어] **하위 버스 번호를 기록한다** -- 이 장치는 브리지 뒤에 있다 */
				new_slot->bus = hold_bus_node->base;
				/* [한국어] 장치 번호를 기록한다 */
				new_slot->device = device;
				/* [한국어] 함수 0 부터 시작한다. 나머지 함수는 configure_new_device 가 찾는다 */
				new_slot->function = 0;
				/* [한국어] 보드로 표시한다 */
				new_slot->is_a_board = 1;
				/* [한국어] 상태를 초기화한다 */
				new_slot->status = 0;

				/* [한국어] **재귀 호출.** behind_bridge 에 1 을 넘겨 "이 아래는 브리지 뒤"
				 * 임을 알리고, 임시 꾸러미를 넘겨 자식들이 그 안에서만 자원을 쓰게 한다 */
				rc = configure_new_device(ctrl, new_slot, 1, &temp_resources);
				/* [한국어] 재귀 결과를 로그로 남긴다 */
				dbg("configure_new_device rc=0x%x\n", rc);
			/* [한국어] 장치가 있는 경우의 처리가 끝났다 */
			}	/* End of IF (device in slot?) */
		/* [한국어] 장치 0x00~0x1F 를 모두 훑었다 */
		}		/* End of FOR loop */

		/* [한국어] 자식 설정에 실패했으면 정리 경로로 간다 */
		if (rc)
			goto free_and_out;
		/* save the interrupt routing information */
		if (resources->irqs) {
			/* [한국어] **IRQ 배선 정보를 부모에게 올려 준다.** 부모가 있으면 배선은 부모가
			 * 하고, 여기서는 알아낸 것을 전달만 한다 */
			resources->irqs->interrupt[0] = irqs.interrupt[0];
			/* [한국어] INTB 정보를 부모에게 넘긴다 */
			resources->irqs->interrupt[1] = irqs.interrupt[1];
			/* [한국어] INTC 정보를 부모에게 넘긴다 */
			resources->irqs->interrupt[2] = irqs.interrupt[2];
			/* [한국어] INTD 정보를 부모에게 넘긴다 */
			resources->irqs->interrupt[3] = irqs.interrupt[3];
			/* [한국어] 유효 핀 마스크도 넘긴다 */
			resources->irqs->valid_INT = irqs.valid_INT;
		} else if (!behind_bridge) {
			/* We need to hook up the interrupts here */
			for (cloop = 0; cloop < 4; cloop++) {
				/* [한국어] 이 핀이 실제로 쓰이는지 본다 */
				if (irqs.valid_INT & (0x01 << cloop)) {
					/* [한국어] **부모가 없으니 여기서 직접 배선한다.** cloop 에 1 을 더하는 것은
					 * 핀 번호가 1부터(INTA) 시작하기 때문이다 */
					rc = cpqhp_set_irq(func->bus, func->device,
							   cloop + 1, irqs.interrupt[cloop]);
					/* [한국어] 배선에 실패하면 정리 경로로 간다 */
					if (rc)
						goto free_and_out;
				}
			/* [한국어] 핀 넷을 모두 훑었다 */
			}	/* end of for loop */
		}
		/* Return unused bus resources
		 * First use the temporary node to store information for
		 * the board */
		if (bus_node && temp_resources.bus_head) {
			/* [한국어] **실제로 쓴 버스 개수를 계산한다.** 남은 목록의 시작에서 원래
			 * 시작을 빼면 자식들이 소비한 개수가 나온다 */
			hold_bus_node->length = bus_node->base - hold_bus_node->base;

			/* [한국어] 목록에 연결한다 */
			hold_bus_node->next = func->bus_head;
			/* [한국어] 이 카드의 버스 목록 머리에 매단다 */
			func->bus_head = hold_bus_node;

			/* [한국어] **종속 버스 번호를 실제로 쓴 만큼으로 줄인다.** 자식들이 쓴
			 * 마지막 버스 번호가 남은 첫 번호의 바로 앞이다 */
			temp_byte = temp_resources.bus_head->base - 1;

			/* set subordinate bus */
			rc = pci_bus_write_config_byte(pci_bus, devfn, PCI_SUBORDINATE_BUS, temp_byte);

			/* [한국어] **남은 버스 번호가 없으면 노드를 해제한다.** 길이 0 짜리 노드를
			 * 자유 목록에 남기면 나중에 할당기가 헛돌기 때문이다 */
			if (temp_resources.bus_head->length == 0) {
				kfree(temp_resources.bus_head);
				/* [한국어] 포인터를 지워 두 번 해제되지 않게 한다 */
				temp_resources.bus_head = NULL;
			} else {
				/* [한국어] 남은 버스 번호를 부모에게 돌려준다 */
				return_resource(&(resources->bus_head), temp_resources.bus_head);
			}
		}

		/* If we have IO space available and there is some left,
		 * return the unused portion */
		if (hold_IO_node && temp_resources.io_head) {
			/* [한국어] IO 창의 앞쪽 남는 부분을 잘라 낸다 */
			io_node = do_pre_bridge_resource_split(&(temp_resources.io_head),
							       &hold_IO_node, 0x1000);

			/* Check if we were able to split something off */
			if (io_node) {
				/* [한국어] 사본의 시작을 잘라 낸 만큼 뒤로 옮긴다 */
				hold_IO_node->base = io_node->base + io_node->length;

				/* [한국어] 8비트 오른쪽으로 밀어 레지스터 형식으로 만든다 */
				temp_byte = (hold_IO_node->base) >> 8;
				/* [한국어] **브리지의 IO base 를 뒤로 민다.** 다만 이 호출은 write_config_word
				 * 인데 temp_byte 는 u8 이다 -- 상위 바이트에 무엇이 들어갈지는
				 * 호출 시점의 값에 달린다. 아래 4304줄은 같은 자리에
				 * write_config_byte 를 쓴다. 코드는 손대지 않고 사실만 적어 둔다 */
				rc = pci_bus_write_config_word(pci_bus, devfn, PCI_IO_BASE, temp_byte);

				/* [한국어] 앞쪽 남은 부분을 부모에게 돌려준다 */
				return_resource(&(resources->io_head), io_node);
			}

			/* [한국어] IO 창의 뒤쪽 남는 부분을 잘라 낸다. 4KiB 정렬이다 */
			io_node = do_bridge_resource_split(&(temp_resources.io_head), 0x1000);

			/* Check if we were able to split something off */
			if (io_node) {
				/* First use the temporary node to store
				 * information for the board */
				hold_IO_node->length = io_node->base - hold_IO_node->base;

				/* If we used any, add it to the board's list */
				if (hold_IO_node->length) {
					/* [한국어] 목록에 연결한다 */
					hold_IO_node->next = func->io_head;
					/* [한국어] 이 카드의 IO 목록 머리에 매단다 */
					func->io_head = hold_IO_node;

					/* [한국어] 잘라 낸 조각의 바로 앞이 이 브리지의 마지막 IO 주소다 */
					temp_byte = (io_node->base - 1) >> 8;
					/* [한국어] limit 을 실제로 쓴 만큼으로 줄인다. 8비트 오른쪽으로 미는 것은
					 * IO base/limit 레지스터가 상위 8비트만 담기 때문이다 */
					rc = pci_bus_write_config_byte(pci_bus, devfn, PCI_IO_LIMIT, temp_byte);

					/* [한국어] 쓰고 남은 부분을 부모에게 돌려준다 */
					return_resource(&(resources->io_head), io_node);
				} else {
					/* it doesn't need any IO */
					temp_word = 0x0000;
					/* [한국어] **limit 을 0 으로 써서 IO 창을 닫는다.** base 가 limit 보다 커지므로
					 * 브리지가 IO 트랜잭션을 전달하지 않게 된다 */
					rc = pci_bus_write_config_word(pci_bus, devfn, PCI_IO_LIMIT, temp_word);

					/* [한국어] 쓰지 않은 창을 부모에게 돌려준다 */
					return_resource(&(resources->io_head), io_node);
					kfree(hold_IO_node);
				}
			} else {
				/* it used most of the range */
				hold_IO_node->next = func->io_head;
				/* [한국어] 대부분을 썼으므로 사본을 그대로 이 카드 몫으로 둔다 */
				func->io_head = hold_IO_node;
			}
		} else if (hold_IO_node) {
			/* it used the whole range */
			hold_IO_node->next = func->io_head;
			/* [한국어] IO 창을 통째로 다 썼다 */
			func->io_head = hold_IO_node;
		}
		/* If we have memory space available and there is some left,
		 * return the unused portion */
		if (hold_mem_node && temp_resources.mem_head) {
			/* [한국어] 메모리 창의 앞쪽 남는 부분을 잘라 낸다.
			 * **인자에 공백이 끼어 있다**(`temp_resources.  mem_head`) -- 오타이나
			 * C 문법상 유효해 동작에는 영향이 없다 */
			mem_node = do_pre_bridge_resource_split(&(temp_resources.  mem_head),
								&hold_mem_node, 0x100000);

			/* Check if we were able to split something off */
			if (mem_node) {
				/* [한국어] 사본의 시작을 잘라 낸 만큼 옮긴다 */
				hold_mem_node->base = mem_node->base + mem_node->length;

				/* [한국어] 16비트 오른쪽으로 민다 */
				temp_word = (hold_mem_node->base) >> 16;
				/* [한국어] 브리지의 메모리 base 를 뒤로 민다 */
				rc = pci_bus_write_config_word(pci_bus, devfn, PCI_MEMORY_BASE, temp_word);

				/* [한국어] 앞쪽 남은 부분을 부모에게 돌려준다 */
				return_resource(&(resources->mem_head), mem_node);
			}

			/* [한국어] 메모리 창의 뒤쪽 남는 부분을 잘라 낸다 */
			mem_node = do_bridge_resource_split(&(temp_resources.mem_head), 0x100000);

			/* Check if we were able to split something off */
			if (mem_node) {
				/* First use the temporary node to store
				 * information for the board */
				hold_mem_node->length = mem_node->base - hold_mem_node->base;

				/* [한국어] **실제로 쓴 길이가 있는지 본다.** 0 이면 이 브리지 뒤의 장치들이
				 * 메모리를 전혀 쓰지 않았다는 뜻이다 */
				if (hold_mem_node->length) {
					/* [한국어] 목록에 연결한다 */
					hold_mem_node->next = func->mem_head;
					/* [한국어] 이 카드의 메모리 목록 머리에 매단다 */
					func->mem_head = hold_mem_node;

					/* configure end address */
					temp_word = (mem_node->base - 1) >> 16;
					/* [한국어] limit 을 실제로 쓴 만큼으로 줄인다 */
					rc = pci_bus_write_config_word(pci_bus, devfn, PCI_MEMORY_LIMIT, temp_word);

					/* Return unused resources to the pool */
					return_resource(&(resources->mem_head), mem_node);
				} else {
					/* it doesn't need any Mem */
					temp_word = 0x0000;
					/* [한국어] limit 을 0 으로 써서 메모리 창을 닫는다 */
					rc = pci_bus_write_config_word(pci_bus, devfn, PCI_MEMORY_LIMIT, temp_word);

					/* [한국어] 쓰지 않은 창을 부모에게 돌려준다 */
					return_resource(&(resources->mem_head), mem_node);
					kfree(hold_mem_node);
				}
			} else {
				/* it used most of the range */
				hold_mem_node->next = func->mem_head;
				/* [한국어] 대부분을 썼으므로 사본을 그대로 이 카드 몫으로 둔다 */
				func->mem_head = hold_mem_node;
			}
		} else if (hold_mem_node) {
			/* it used the whole range */
			hold_mem_node->next = func->mem_head;
			/* [한국어] prefetchable 창을 통째로 다 썼다 */
			func->mem_head = hold_mem_node;
		}
		/* If we have prefetchable memory space available and there
		 * is some left at the end, return the unused portion */
		if (temp_resources.p_mem_head) {
			/* [한국어] **앞쪽에서 남는 부분을 잘라 낸다** */
			p_mem_node = do_pre_bridge_resource_split(&(temp_resources.p_mem_head),
								  &hold_p_mem_node, 0x100000);

			/* Check if we were able to split something off */
			if (p_mem_node) {
				/* [한국어] **사본의 시작을 잘라 낸 만큼 뒤로 옮긴다** -- 이제 이 카드가
				 * 실제로 쓰는 범위는 그 뒤부터다 */
				hold_p_mem_node->base = p_mem_node->base + p_mem_node->length;

				/* [한국어] 16비트 오른쪽으로 밀어 레지스터 형식으로 만든다 */
				temp_word = (hold_p_mem_node->base) >> 16;
				/* [한국어] 브리지의 base 를 그만큼 뒤로 민다 */
				rc = pci_bus_write_config_word(pci_bus, devfn, PCI_PREF_MEMORY_BASE, temp_word);

				/* [한국어] 앞쪽 남은 부분을 부모에게 돌려준다 */
				return_resource(&(resources->p_mem_head), p_mem_node);
			}

			/* [한국어] **뒤쪽에서 남는 부분을 잘라 낸다.** 위의 do_pre_ 가 앞쪽을,
			 * 이것이 뒤쪽을 맡는다 */
			p_mem_node = do_bridge_resource_split(&(temp_resources.p_mem_head), 0x100000);

			/* Check if we were able to split something off */
			if (p_mem_node) {
				/* First use the temporary node to store
				 * information for the board */
				hold_p_mem_node->length = p_mem_node->base - hold_p_mem_node->base;

				/* If we used any, add it to the board's list */
				if (hold_p_mem_node->length) {
					/* [한국어] 목록에 연결한다 */
					hold_p_mem_node->next = func->p_mem_head;
					/* [한국어] 이 카드의 목록 머리에 매단다 */
					func->p_mem_head = hold_p_mem_node;

					/* [한국어] 16비트 오른쪽으로 밀어 레지스터 형식으로 만든다 */
					temp_word = (p_mem_node->base - 1) >> 16;
					/* [한국어] **limit 을 실제로 쓴 만큼으로 줄인다.** 잘라 낸 조각의 시작 바로
					 * 앞이 이 브리지가 쓰는 마지막 주소다 */
					rc = pci_bus_write_config_word(pci_bus, devfn, PCI_PREF_MEMORY_LIMIT, temp_word);

					/* [한국어] 남은 부분을 부모 목록으로 돌려준다 */
					return_resource(&(resources->p_mem_head), p_mem_node);
				} else {
					/* it doesn't need any PMem */
					temp_word = 0x0000;
					/* [한국어] **limit 을 0 으로 써서 창을 닫는다.** base 가 limit 보다 크면
					 * 브리지가 그 종류의 트랜잭션을 전달하지 않는다는 PCI 규약을 이용한다 */
					rc = pci_bus_write_config_word(pci_bus, devfn, PCI_PREF_MEMORY_LIMIT, temp_word);

					/* [한국어] 쓰지 않은 창을 부모에게 돌려준다 */
					return_resource(&(resources->p_mem_head), p_mem_node);
					kfree(hold_p_mem_node);
				}
			} else {
				/* it used the most of the range */
				hold_p_mem_node->next = func->p_mem_head;
				/* [한국어] **대부분을 썼다** -- 뒤쪽을 잘라 낼 수 없었으므로 사본을 통째로
				 * 이 카드 몫으로 둔다 */
				func->p_mem_head = hold_p_mem_node;
			}
		} else if (hold_p_mem_node) {
			/* it used the whole range */
			hold_p_mem_node->next = func->p_mem_head;
			/* [한국어] **창을 통째로 다 썼다** -- 남는 부분이 없으므로 사본을 그대로
			 * 이 카드의 목록에 매단다 */
			func->p_mem_head = hold_p_mem_node;
		}
		/* We should be configuring an IRQ and the bridge's base address
		 * registers if it needs them.  Although we have never seen such
		 * a device */

		/* enable card */
		command = 0x0157;	/* = PCI_COMMAND_IO |
					 *   PCI_COMMAND_MEMORY |
					 *   PCI_COMMAND_MASTER |
					 *   PCI_COMMAND_INVALIDATE |
					 *   PCI_COMMAND_PARITY |
					 *   PCI_COMMAND_SERR */
		rc = pci_bus_write_config_word(pci_bus, devfn, PCI_COMMAND, command);

		/* set Bridge Control Register */
		command = 0x07;		/* = PCI_BRIDGE_CTL_PARITY |
					 *   PCI_BRIDGE_CTL_SERR |
					 *   PCI_BRIDGE_CTL_NO_ISA */
		rc = pci_bus_write_config_word(pci_bus, devfn, PCI_BRIDGE_CONTROL, command);
	} else if ((temp_byte & PCI_HEADER_TYPE_MASK) == PCI_HEADER_TYPE_NORMAL) {
		/* Standard device */
		rc = pci_bus_read_config_byte(pci_bus, devfn, 0x0B, &class_code);

		if (class_code == PCI_BASE_CLASS_DISPLAY) {
			/* Display (video) adapter (not supported) */
			return DEVICE_TYPE_NOT_SUPPORTED;
		}
		/* Figure out IO and memory needs */
		for (cloop = 0x10; cloop <= 0x24; cloop += 4) {
			/* [한국어] 질의에 쓸 값을 준비한다 */
			temp_register = 0xFFFFFFFF;

			/* [한국어] 어느 BAR 를 다루는지 로그로 남긴다 */
			dbg("CND: bus=%d, devfn=%d, offset=%d\n", pci_bus->number, devfn, cloop);
			/* [한국어] **BAR 에 전부 1 을 써 본다.** PCI 규격이 정한 크기 질의 방법이다 */
			rc = pci_bus_write_config_dword(pci_bus, devfn, cloop, temp_register);

			/* [한국어] **되읽어 크기를 알아낸다.** 카드는 자기가 쓰지 않는 하위 비트를
			 * 0 으로 고정하므로, 남은 1 비트가 정렬 요구를 나타낸다 */
			rc = pci_bus_read_config_dword(pci_bus, devfn, cloop, &temp_register);
			/* [한국어] 되읽은 값을 로그로 남긴다 */
			dbg("CND: base = 0x%x\n", temp_register);

			/* [한국어] **BAR 가 구현되어 있는지 본다.** 되읽은 값이 0 이면 그 BAR 는
			 * 존재하지 않는다. 오른쪽 원문 주석이 그 뜻을 밝힌다 */
			if (temp_register) {	  /* If this register is implemented */
				if ((temp_register & 0x03L) == 0x01) {
					/* Map IO */

					/* set base = amount of IO space */
					base = temp_register & 0xFFFFFFFC;
					/* [한국어] **2의 보수를 취해 요구 크기를 얻는다.** 이 두 줄이 BAR 크기 질의
					 * 관용구의 마지막 단계다 -- 비트를 뒤집고 1 을 더하면 그 BAR 가
					 * 요구하는 바이트 수가 나온다 */
					base = ~base + 1;

					/* [한국어] 요구 크기를 로그로 남긴다 */
					dbg("CND:      length = 0x%x\n", base);
					/* [한국어] **IO 는 전용 함수를 쓴다** -- ISA 별칭 구간을 피해야 하기 때문이다 */
					io_node = get_io_resource(&(resources->io_head), base);
					/* [한국어] IO 공간이 모자라면 -ENOMEM 을 돌려준다 */
					if (!io_node)
						return -ENOMEM;
					/* [한국어] 받은 노드를 로그로 남긴다 */
					dbg("Got io_node start = %8.8x, length = %8.8x next (%p)\n",
					    io_node->base, io_node->length, io_node->next);
					/* [한국어] 목록 상태를 로그로 남긴다 */
					dbg("func (%p) io_head (%p)\n", func, func->io_head);

					/* allocate the resource to the board */
					base = io_node->base;
					/* [한국어] 목록에 연결한다 */
					io_node->next = func->io_head;
					/* [한국어] 이 카드의 IO 목록 머리에 매단다 */
					func->io_head = io_node;
				} else if ((temp_register & 0x0BL) == 0x08) {
					/* Map prefetchable memory */
					base = temp_register & 0xFFFFFFF0;
					/* [한국어] 2의 보수를 취해 요구 크기를 얻는다 */
					base = ~base + 1;

					/* [한국어] 요구 크기를 로그로 남긴다 */
					dbg("CND:      length = 0x%x\n", base);
					/* [한국어] prefetchable 메모리를 자유 목록에서 떼어 낸다 */
					p_mem_node = get_resource(&(resources->p_mem_head), base);

					/* allocate the resource to the board */
					if (p_mem_node) {
						/* [한국어] 배정받은 주소를 꺼낸다 */
						base = p_mem_node->base;

						/* [한국어] 목록에 연결한다 */
						p_mem_node->next = func->p_mem_head;
						/* [한국어] 이 카드의 prefetchable 메모리 목록 머리에 매단다 */
						func->p_mem_head = p_mem_node;
					/* [한국어] 할당에 실패했다 */
					} else
						return -ENOMEM;
				} else if ((temp_register & 0x0BL) == 0x00) {
					/* Map memory */
					base = temp_register & 0xFFFFFFF0;
					/* [한국어] 2의 보수를 취해 요구 크기를 얻는다 */
					base = ~base + 1;

					/* [한국어] 요구 크기를 로그로 남긴다 */
					dbg("CND:      length = 0x%x\n", base);
					/* [한국어] 일반 메모리를 자유 목록에서 떼어 낸다 */
					mem_node = get_resource(&(resources->mem_head), base);

					/* allocate the resource to the board */
					if (mem_node) {
						/* [한국어] 배정받은 주소를 꺼낸다 */
						base = mem_node->base;

						/* [한국어] 목록에 연결한다 */
						mem_node->next = func->mem_head;
						/* [한국어] 이 카드의 메모리 목록 머리에 매단다 */
						func->mem_head = mem_node;
					/* [한국어] 할당에 실패했다 */
					} else
						return -ENOMEM;
				} else {
					/* Reserved bits or requesting space below 1M */
					return NOT_ENOUGH_RESOURCES;
				}

				/* [한국어] **받은 주소를 BAR 에 써 넣는다.** 세 갈래(IO/prefetchable/메모리)가
				 * 모두 base 변수에 주소를 담아 여기로 모인다 */
				rc = pci_bus_write_config_dword(pci_bus, devfn, cloop, base);

				/* Check for 64-bit base */
				if ((temp_register & 0x07L) == 0x04) {
					/* [한국어] **64비트 BAR 이므로 다음 칸을 건너뛴다.** 위의 원문 주석이
					 * "요즘 시스템에서는 상위 32비트가 항상 0" 이라면서도 알파와 ia64
					 * 에서는 아닐 것이라고 FIXME 를 남겼다 */
					cloop += 4;

					/* Upper 32 bits of address always zero
					 * on today's systems */
					/* FIXME this is probably not true on
					 * Alpha and ia64??? */
					base = 0;
					/* [한국어] 상위 32비트에 0 을 쓴다 */
					rc = pci_bus_write_config_dword(pci_bus, devfn, cloop, base);
				}
			}
		/* [한국어] BAR 여섯 개를 모두 훑었다 */
		}		/* End of base register loop */
		if (cpqhp_legacy_mode) {
			/* Figure out which interrupt pin this function uses */
			rc = pci_bus_read_config_byte(pci_bus, devfn,
				PCI_INTERRUPT_PIN, &temp_byte);

			/* If this function needs an interrupt and we are behind
			 * a bridge and the pin is tied to something that's
			 * already mapped, set this one the same */
			if (temp_byte && resources->irqs &&
			    (resources->irqs->valid_INT &
			     (0x01 << ((temp_byte + resources->irqs->barber_pole - 1) & 0x03)))) {
				/* We have to share with something already set up */
				IRQ = resources->irqs->interrupt[(temp_byte +
					resources->irqs->barber_pole - 1) & 0x03];
			} else {
				/* Program IRQ based on card type */
				rc = pci_bus_read_config_byte(pci_bus, devfn, 0x0B, &class_code);

				/* [한국어] **카드 종류로 IRQ 를 가른다.** 저장장치와 그 밖에 서로 다른 IRQ 를
				 * 주어 인터럽트 부하를 분산하려는 것으로 보인다.
				 * 두 값은 cpqphp_pci.c 가 HRT 의 미사용 IRQ 목록에서 골라 둔 전역이다 */
				if (class_code == PCI_BASE_CLASS_STORAGE)
					/* [한국어] 저장장치면 디스크용 IRQ 를 쓴다 */
					IRQ = cpqhp_disk_irq;
				else
					/* [한국어] 그 밖은 네트워크용 IRQ 를 쓴다 */
					IRQ = cpqhp_nic_irq;
			}

			/* IRQ Line */
			rc = pci_bus_write_config_byte(pci_bus, devfn, PCI_INTERRUPT_LINE, IRQ);
		}

		/* [한국어] 브리지 뒤인지에 따라 갈린다 -- 뒤라면 부모가 나중에 배선하므로
		 * 여기서는 기록만 한다 */
		if (!behind_bridge) {
			/* [한국어] **브리지 뒤가 아니면 지금 바로 IRQ 를 배선한다.**
			 * temp_byte 는 인터럽트 핀 번호인데, cpqhp_legacy_mode 가 거짓이면
			 * 그 값이 초기화되지 않은 채 쓰인다 */
			rc = cpqhp_set_irq(func->bus, func->device, temp_byte, IRQ);
			/* [한국어] 실패를 확인한다 */
			if (rc)
				/* [한국어] IRQ 배선에 실패하면 1 을 돌려준다 */
				return 1;
		} else {
			/* TBD - this code may also belong in the other clause
			 * of this If statement */
			resources->irqs->interrupt[(temp_byte + resources->irqs->barber_pole - 1) & 0x03] = IRQ;
			/* [한국어] **valid_INT 비트를 세워 이 핀이 배정되었음을 기록한다.**
			 * 연산자 우선순위에 주의 -- `|=` 의 오른쪽이 `(0x01 << ...) & 0x03` 으로
			 * 묶이므로, 시프트 결과를 3 으로 마스크한 값이 들어간다.
			 * 의도가 `0x01 << (... & 0x03)` 이었다면 결과가 달라진다.
			 * 코드는 손대지 않고 사실만 적어 둔다 */
			resources->irqs->valid_INT |= 0x01 << (temp_byte + resources->irqs->barber_pole - 1) & 0x03;
		}

		/* Latency Timer */
		temp_byte = 0x40;
		/* [한국어] 지연 타이머를 0x40(64클록)으로 쓴다. 브리지 경로와 같은 값이다 */
		rc = pci_bus_write_config_byte(pci_bus, devfn,
					PCI_LATENCY_TIMER, temp_byte);

		/* Cache Line size */
		temp_byte = 0x08;
		/* [한국어] 캐시 라인 크기를 8 로 쓴다. dword 단위이므로 32바이트다 */
		rc = pci_bus_write_config_byte(pci_bus, devfn,
					PCI_CACHE_LINE_SIZE, temp_byte);

		/* disable ROM base Address */
		temp_dword = 0x00L;
		/* [한국어] 명령 레지스터를 써서 카드를 켠다. 값 0x0157 이 어떤 플래그의 합인지는
		 * 위의 원문 주석에 적혀 있다 -- IO, 메모리, 버스 마스터, 무효화,
		 * 패리티, SERR 이다 */
		rc = pci_bus_write_config_word(pci_bus, devfn,
					PCI_ROM_ADDRESS, temp_dword);

		/* enable card */
		temp_word = 0x0157;	/* = PCI_COMMAND_IO |
					 *   PCI_COMMAND_MEMORY |
					 *   PCI_COMMAND_MASTER |
					 *   PCI_COMMAND_INVALIDATE |
					 *   PCI_COMMAND_PARITY |
					 *   PCI_COMMAND_SERR */
		rc = pci_bus_write_config_word(pci_bus, devfn,
					PCI_COMMAND, temp_word);
	} else {		/* End of Not-A-Bridge else */
		/* It's some strange type of PCI adapter (Cardbus?) */
		return DEVICE_TYPE_NOT_SUPPORTED;
	}

	/* [한국어] 설정을 마쳤음을 표시한다. 이 필드가 0 이면 아직 설정되지 않은
	 * 노드라는 뜻이다 */
	func->configured = 1;

	return 0;
free_and_out:
	cpqhp_destroy_resource_list(&temp_resources);

	/* [한국어] **hold_ 사본 넷을 부모 목록으로 돌려준다.** 그 사본들은 브리지에게
	 * 준 창의 원래 모습이라, 실패했으니 통째로 반납하는 것이 맞다 */
	return_resource(&(resources->bus_head), hold_bus_node);
	/* [한국어] IO 창도 돌려준다 */
	return_resource(&(resources->io_head), hold_IO_node);
	/* [한국어] 메모리 창도 돌려준다 */
	return_resource(&(resources->mem_head), hold_mem_node);
	/* [한국어] prefetchable 메모리 창도 돌려준다 */
	return_resource(&(resources->p_mem_head), hold_p_mem_node);
	/* [한국어] 브리지 경로에서 마지막으로 남은 rc 를 돌려준다 */
	return rc;
}
