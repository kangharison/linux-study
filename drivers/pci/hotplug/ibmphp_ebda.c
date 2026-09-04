// SPDX-License-Identifier: GPL-2.0+
/*
 * IBM Hot Plug Controller Driver
 *
 * Written By: Tong Yu, IBM Corporation
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
 * [한국어 설명] EBDA(Extended BIOS Data Area) 표 파서 (ibmphp_ebda.c)
 *
 * === 파일의 역할 ===
 * IBM 서버의 BIOS(POST)가 물리 메모리 저편에 남겨 둔 EBDA 표를 읽어, 이
 * 드라이버가 쓰는 커널 쪽 자료구조 전부를 세우는 파일이다. 바로 아래 상류
 * 주석이 밝히듯 POST 는 핫플러그 컨트롤러의 구성과 이미 배정해 둔 자원
 * (MEM/PFMEM/IO)을 EBDA 안의 데이터 블록으로 기술해 두는데, 이 파일이 그
 * 블록들을 물리 주소에서 ioremap 으로 매핑해 바이트 단위로 뜯어 읽는다.
 * 결과물은 세 갈래다 — 컨트롤러 목록(ebda_hpc_head), 슬롯 목록
 * (ibmphp_slot_head), 그리고 POST 가 이미 배정한 자원 목록
 * (ibmphp_ebda_pci_rsrc_head)이다. 세 번째가 ibmphp_res.c 의 자유 목록
 * 장부가 세워지는 원재료다. 즉 이 파일은 하드웨어를 탐색하지 않고
 * **BIOS 가 적어 둔 표를 그대로 믿고 옮겨 적는다**.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 드라이버 초기화의 가장 첫 단계다. ibmphp_core.c 의 ibmphp_init() 이
 * ibmphp_access_ebda() 를 부르는 것이 이 드라이버가 하는 첫 실질적인 일이고,
 * 그것이 성공해야 그 다음의 ibmphp_rsrc_init()(ibmphp_res.c)과
 * ibmphp_register_pci() 가 의미를 갖는다.
 *
 * 초기화 순서(ibmphp_core.c:1209 부근):
 *   ibmphp_init()
 *     → ibmphp_access_ebda()      [이 파일] EBDA 를 훑어 세 목록을 세운다
 *         → ebda_rio_table()      RIO 표(섀시/확장 상자) 파싱
 *         → ebda_rsrc_controller() 컨트롤러·슬롯 파싱 + 핫플러그 코어 등록
 *         → ebda_rsrc_rsrc()       POST 가 배정한 자원 목록 파싱
 *     → ibmphp_rsrc_init()        [ibmphp_res.c] 위 자원 목록을 장부로 변환
 *     → ibmphp_register_pci()     [이 파일] PCI 형 컨트롤러가 있으면 드라이버 등록
 *
 * 실행 컨텍스트: 모듈 초기화(프로세스 컨텍스트). 이 파일의 파싱 함수 대부분이
 * __init 로 표시되어 있어 초기화가 끝나면 코드가 버려진다. 다만 목록을
 * 되짚어 주는 조회 함수들(ibmphp_find_same_bus_num, ibmphp_get_bus_index,
 * ibmphp_get_slot_from_physical_num, ibmphp_get_total_controllers)과 해제
 * 함수들은 __init 가 아니라 드라이버가 살아 있는 내내 쓰인다.
 *
 * === 타 모듈과의 연결 ===
 * 이 파일이 세우고 다른 파일이 읽는 것:
 *   - ibmphp_ebda_pci_rsrc_head — POST 가 이미 배정한 자원의 목록.
 *     ibmphp_res.c:491 의 ibmphp_rsrc_init() 이 이 목록을 통째로 훑어
 *     자기 자유 목록 장부(gbuses / bus_node / resource_node)를 세운다.
 *     세우고 나면 ibmphp_core.c 가 ibmphp_free_ebda_pci_rsrc_queue() 로 버린다.
 *   - ibmphp_slot_head — 슬롯 목록. ibmphp_core.c(:99, :407, :543, :569)와
 *     ibmphp_hpc.c(:597, :806, :827, :838)가 훑는다.
 *   - bus_info_head — 버스별 슬롯 번호 범위. 파일 밖에서는 직접 못 보고
 *     ibmphp_find_same_bus_num() / ibmphp_get_bus_index() 로만 접근한다.
 *     전자는 ibmphp_pci.c:2246 의 find_sec_number() 가 2차 버스 번호를
 *     고를 때, 후자는 ibmphp_hpc.c:521, :661 이 컨트롤러 레지스터를
 *     짚을 때 쓴다.
 *   - ebda_hpc_head — 컨트롤러 목록. 이 파일 안에서만 쓰이고,
 *     ibmphp_get_total_controllers() 로 개수만 ibmphp_hpc.c:808, :840 에
 *     알려 준다.
 * 이 파일이 부르는 것:
 *   - ibmphp_hpc.c 의 ibmphp_hpc_readslot() — 슬롯을 등록하기 전에 실제
 *     상태를 한 번 읽어 채운다(fillslotinfo()).
 *   - ibmphp_core.c 의 ibmphp_init_devno() — 슬롯의 PCI 장치 번호를 IRQ
 *     라우팅 표에서 찾아 채운다.
 *   - 핫플러그 코어의 pci_hp_register() — 슬롯을 sysfs 에 노출한다.
 * 데이터 흐름은 한 방향이다: 물리 메모리의 EBDA → readb/readw/readl →
 * 이 파일의 목록 → (ibmphp_res.c 의 장부 / ibmphp_core.c 의 슬롯 조작).
 *
 * === 주요 함수/구조체 요약 ===
 * ibmphp_access_ebda()      : 진입점. EBDA 를 찾아 매핑하고 블록을 훑는다.
 * ebda_rio_table()          : RIO 표에서 섀시/확장 상자 정보를 뽑는다.
 * ebda_rsrc_controller()    : 컨트롤러·슬롯·버스를 뽑고 슬롯을 코어에 등록한다.
 * ebda_rsrc_rsrc()          : POST 가 배정한 자원 항목을 목록으로 만든다.
 * create_file_name()        : 슬롯의 sysfs 이름("chassis1slot2" 꼴)을 짓는다.
 * ibmphp_find_same_bus_num(): 버스 번호로 bus_info 를 찾는다(파일 밖에서 씀).
 * ibmphp_register_pci()     : PCI 형 컨트롤러가 있으면 pci_driver 를 등록한다.
 * struct ebda_hpc_list      : 컨트롤러 기술자 묶음의 머리(개수와 시작 오프셋).
 * struct ebda_rsrc_list     : 자원 항목 묶음의 머리(개수와 시작 오프셋).
 * struct rio_detail         : RIO 표의 한 항목(섀시 또는 확장 상자 하나).
 * struct bus_info           : 버스 하나의 슬롯 번호 범위와 속도 능력.
 *
 * 이 파일이 다루는 구조체는 모두 ibmphp.h 에 정의되어 있어 그 파일에는
 * 손대지 않고, 파일 경계를 넘어 쓰이는 두 개만 여기에 필드별로 적는다.
 *
 * struct ebda_pci_rsrc (ibmphp.h:223) — POST 가 이미 배정해 둔 자원 하나.
 * ebda_rsrc_rsrc() 가 만들어 ibmphp_ebda_pci_rsrc_head 에 매달고,
 * ibmphp_res.c:491 의 ibmphp_rsrc_init() 이 읽어 장부로 옮긴 뒤
 * ibmphp_free_ebda_pci_rsrc_queue() 가 버린다. 초기화 흐름 하나에서만
 * 다뤄지므로 동기화가 없다.
 *   u8 rsrc_type;
 *     EBDA 항목의 종류 바이트를 **하위 비트를 지우지 않고 통째로** 담은 값.
 *     설정자: ebda_rsrc_rsrc() 가 readb 로 읽어 그대로 넣는다.
 *     읽는 자: ibmphp_rsrc_init() 이 마스크를 씌워 세 가지를 꺼낸다 —
 *     하위 2비트(RESTYPE)로 IO/MEM/PFMEM 을, PCIDEVMASK(0x10)로 PCI
 *     장치인지를, PRIMARYBUSMASK(0x20)로 버스의 범위 선언인지를 가른다.
 *     값 범위: 하위 2비트가 0x00 IO, 0x01 MEM, 0x03 PFMEM, 0x02 는 예약.
 *   u8 bus_num;
 *     이 자원이 배정된 PCI 버스 번호.
 *     설정자: ebda_rsrc_rsrc() 가 항목의 두 번째 바이트에서 읽는다.
 *     읽는 자: ibmphp_rsrc_init() 이 어느 bus_node 에 넣을지 정할 때.
 *   u8 dev_fun;
 *     그 버스에서의 장치·함수 번호(devfn).
 *     설정자/읽는 자는 bus_num 과 같다. 버스 범위 선언 항목에서는
 *     장치를 가리키지 않으므로 의미가 없다.
 *   u32 start_addr;
 *     배정된 구간의 시작 주소.
 *     설정자: I/O 항목이면 readw 로 16비트를, MEM/PFMEM 항목이면 readl 로
 *     32비트를 읽는다. 필드는 둘 다 u32 다.
 *     읽는 자: ibmphp_rsrc_init() 이 range 또는 resource_node 의 start 로.
 *   u32 end_addr;
 *     배정된 구간의 끝 주소(마지막 바이트). 길이가 아니라 끝 주소다.
 *     설정자/읽는 자는 start_addr 과 같다.
 *   u8 marked;
 *     상류 주석이 "for NVRAM" 이라고만 적어 둔 필드. 이 파일은 쓰지 않고
 *     kzalloc 이 남긴 0 그대로다. 무엇에 쓰였는지는 확인 못 함.
 *   struct list_head ebda_pci_rsrc_list;
 *     ibmphp_ebda_pci_rsrc_head 목록의 연결 고리.
 *     설정자: ebda_rsrc_rsrc() 의 list_add. 목록 앞쪽에 붙이므로 표에 적힌
 *     순서와 목록 순서가 뒤집힌다.
 *
 * struct bus_info (ibmphp.h:238) — 버스 하나의 슬롯 범위와 속도 능력.
 * ebda_rsrc_controller() 가 만들어 bus_info_head 에 매달고, 밖에서는
 * ibmphp_find_same_bus_num()/ibmphp_get_bus_index() 로만 본다. 초기화
 * 이후로는 current_speed/current_bus_mode 를 빼면 바뀌지 않는다.
 *   u8 slot_min, slot_max;
 *     이 버스에 달린 슬롯 번호의 최소와 최대.
 *     설정자: ebda_rsrc_controller() 가 슬롯을 하나씩 읽으며 min/max 로
 *     넓힌다. 슬롯이 하나뿐이면 두 값이 같다(상류 주석이 밝히는 사정).
 *     읽는 자: ibmphp_pci.c 의 find_sec_number() — 슬롯 번호에서 min 을
 *     빼 순번을 구하고, 범위를 벗어나면 브리지 설정을 포기한다.
 *   u8 slot_count;
 *     이 버스에 달린 슬롯 개수. 설정자는 위와 같고, 읽는 곳은 디버그
 *     출력(print_bus_info)뿐이다.
 *   u8 busno;
 *     버스 번호 자신. 두 조회 함수가 이 필드로 항목을 찾는다.
 *   u8 controller_id;
 *     이 버스를 담당하는 컨트롤러의 id.
 *   u8 current_speed, current_bus_mode;
 *     지금 이 버스가 실제로 도는 속도와 모드.
 *     설정자: 여기서는 "모름" 을 뜻하는 0xff 로만 둔다. 실제 값은
 *     ibmphp_hpc.c 가 컨트롤러에게 물어 채운다.
 *     따라서 이 두 필드만 초기화 이후에도 바뀐다.
 *   u8 index;
 *     **컨트롤러 안에서의 상대 버스 번호.** 컨트롤러마다 1 부터 매긴다.
 *     설정자: ebda_rsrc_controller() 의 bus_index++.
 *     읽는 자: ibmphp_get_bus_index() 를 거쳐 ibmphp_hpc.c:521, :661 —
 *     버스 레지스터의 자리를 정하는 데 쓴다. 시스템 전체의 버스 번호로는
 *     레지스터를 짚을 수 없어 이 값이 따로 필요하다.
 *   u8 slots_at_33_conv, slots_at_66_conv,
 *      slots_at_66_pcix, slots_at_100_pcix, slots_at_133_pcix;
 *     각 속도에서 쓸 수 있는 슬롯 수.
 *     설정자: ebda_rsrc_controller() 의 버스 루프가 EBDA 의 버스당
 *     8바이트 구역에서 앞 5바이트를 읽어 베낀다.
 *     읽는 자: 이 드라이버 안에서는 디버그 출력 외에 없다.
 *   struct list_head bus_info_list;
 *     bus_info_head 목록의 연결 고리. list_add_tail 로 붙이므로 표에
 *     적힌 순서가 목록에 그대로 남는다.
 *
 * === EBDA 를 찾아가는 길 ===
 * 코드가 실제로 읽는 순서만 적는다. 표 형식의 출처 문서는 이 트리에 없어
 * 확인 못 함 — 아래는 전부 코드에서 직접 읽은 것이다.
 *   1. 물리 주소 0x40E(= (0x40 << 4) + 0x0e)에서 워드 하나를 읽는다.
 *      이것이 EBDA 의 실모드 세그먼트 값이다.
 *   2. 그 세그먼트를 16배 한 물리 주소(ebda_seg << 4)의 첫 바이트가
 *      EBDA 의 크기이며 단위는 KiB 다.
 *   3. 그 크기만큼 통째로 ioremap 해 두고, 오프셋 0x180 부터 블록을 훑는다.
 *      0x180 이라는 시작 오프셋의 근거는 코드에 없다 — 확인 못 함.
 *
 * === 블록의 바이트 배치 ===
 * 블록 하나의 머리는 워드 둘이다.
 *   +0  u16 다음 블록의 오프셋 (0 이면 마지막)
 *   +2  u16 블록 id
 * 이 파일이 보는 id 는 둘뿐이고 나머지는 건너뛴다.
 *   0x4853 — 핫스왑 블록. 상류 주석이 "hot swap block" 이라 부른다.
 *   0x4752 — RIO 블록. 상류 주석이 "rio block" 이라 부른다.
 *
 * 핫스왑 블록(0x4853)의 몸통:
 *   +4  u8  format — 4 가 아니면 지원하지 않는 판으로 보고 -ENODEV.
 *   +5      여기가 base. 두 개의 하위 블록이 이 base 를 기준으로 놓인다.
 *   base+0  u16 re    — RE 하위 블록까지의 상대 오프셋
 *   base+2  u16 rc_id — 0x5243 이어야 한다. 상류 주석이 "rc sub blk
 *                       signature" 라 적었고 바이트 값이 'R'(0x52),
 *                       'C'(0x43) 와 같다.
 *   base+4  u8  num_ctlrs — 컨트롤러 개수
 *   base+5  여기부터가 컨트롤러 기술자 배열(RSRC_CONTROLLER).
 *           이 오프셋이 hpc_list_ptr->phys_addr 로 기억된다.
 *   base+re+0 u16 다음 하위 블록 (상류가 FIXME 로 "rc is never used" 라 적음)
 *   base+re+2 u16 re_id — 0x5245 이어야 한다. 상류 주석이 "signature of re",
 *                       바이트 값이 'R'(0x52), 'E'(0x45) 와 같다.
 *   base+re+4 u16 num_entries — 자원 항목 개수
 *   base+re+6 여기부터가 자원 항목 배열(RSRC_ENTRIES).
 *             이 오프셋이 rsrc_list_ptr->phys_addr 로 기억된다.
 *
 * RIO 블록(0x4752)의 몸통:
 *   +4  u8 ver_num     — 3 일 때만 ebda_rio_table() 이 돈다
 *   +5  u8 scal_count  — 확장성(scalability) 항목 개수
 *   +6  u8 riodev_count— RIO 장치 항목 개수
 *   +7  여기부터가 표의 몸통. 확장성 항목은 12바이트씩이라 그만큼 건너뛰고
 *       RIO 장치 항목만 읽는다. 항목 하나는 15바이트다 —
 *       +0 rio_node_id(u8), +1 bbar(u32), +5 rio_type(u8), +6 owner_id(u8),
 *       +7~+10 포트 연결 정보 4바이트, +11 first_slot_num(u8), +12 status(u8),
 *       +13 wpindex(u8), +14 chassis_num(u8).
 *       rio_type 이 4 나 5 면 섀시, 6 이나 7 이면 확장 상자로 나누고
 *       그 밖은 버린다. 4/5/6/7 이라는 값의 의미는 코드에 근거가 없다 —
 *       확인 못 함.
 *
 * 컨트롤러 기술자 하나의 배치(ebda_rsrc_controller() 가 읽는 순서):
 *   +0  u8 ctlr_id
 *   +1  u8 slot_num — 이 컨트롤러가 담당하는 슬롯 개수
 *   +2  슬롯 배열. **필드별로 나뉜 네 개의 평행 배열**이라, 슬롯 i 의
 *       네 값은 서로 slot_num 바이트씩 떨어져 있다 —
 *       [slot_num 바이트: 슬롯 번호][slot_num 바이트: 슬롯의 버스 번호]
 *       [slot_num 바이트: 컨트롤러 안의 인덱스][slot_num 바이트: 슬롯 능력]
 *   그 다음 u8 bus_num — 이 컨트롤러가 담당하는 버스 개수
 *   그 다음 버스 배열 — [bus_num 바이트: 버스 번호][버스당 8바이트]
 *       버스당 8바이트 중 앞 5바이트만 쓴다(33/66 conv, 66/100/133 PCI-X 에서
 *       쓸 수 있는 슬롯 수). 나머지 3바이트는 이 코드가 읽지 않는다.
 *   그 다음 u8 ctlr_type — 컨트롤러가 어디에 붙어 있는지
 *       0 = ISA(io_start u16, io_end u16, irq u8 — 5바이트)
 *       1 = PCI(bus u8, dev_fun u8, irq u8 — 3바이트)
 *       2 또는 4 = i2c(wpegbbar u32, i2c_addr u8, irq u8 — 6바이트)
 *       그 밖의 값이면 -ENODEV. 2 와 4 를 나누는 기준은 코드에 없다 —
 *       다만 create_file_name() 이 type 4 를 확장 상자(rxe)로 다룬다.
 *
 * 자원 항목 하나의 배치(ebda_rsrc_rsrc() 가 읽는 순서):
 *   +0  u8 type — 하위 2비트(EBDA_RSRC_TYPE_MASK)가 종류다.
 *       0x00 IO, 0x01 MEM, 0x03 PFMEM, 0x02 는 예약(EBDA_RES_RSRC_TYPE).
 *   IO 항목은 6바이트가 이어진다 — bus(u8), dev_fun(u8), start(u16), end(u16).
 *   MEM/PFMEM 항목은 10바이트 — bus(u8), dev_fun(u8), start(u32), end(u32).
 *   즉 IO 는 16비트 주소, 메모리는 32비트 주소로 적혀 있다.
 *
 * === 여기서 만든 것이 ibmphp_res.c 로 어떻게 흘러가는가 ===
 * ebda_rsrc_rsrc() 가 만드는 struct ebda_pci_rsrc 하나가 "POST 가 이 장치의
 * 이 BAR 에 이만큼을 이미 줬다" 는 기록이다. ibmphp_res.c:491 의
 * ibmphp_rsrc_init() 이 그 목록을 훑으면서, type 의 상위 비트로 그것이
 * 버스의 범위 선언인지(PRIMARYBUSMASK) 개별 장치의 배정인지(PCIDEVMASK)를
 * 갈라, 전자는 bus_node 의 range 로 후자는 resource_node 로 만든다. 그래서
 * ibmphp_res.c 의 자유 목록은 하드웨어를 훑어 만든 것이 아니라 **이 표를
 * 그대로 옮겨 적은 것**이며, 그것이 그 파일이 PCI 코어의 자원 할당기를
 * 쓰지 않는 이유와 직접 이어진다 — 코어의 자원 트리는 이 표를 모른다.
 *
 * === 이 파일에서 눈에 띄는 것들 ===
 * 코드에서 읽히는 것만 적는다.
 *   - **io_mem 이 파일 전역 하나뿐이다.** ibmphp_access_ebda() 가 EBDA
 *     전체를 매핑해 두고, ebda_rio_table()/ebda_rsrc_controller()/
 *     ebda_rsrc_rsrc() 가 그 포인터를 그대로 쓴다. 그래서 이 셋은
 *     ibmphp_access_ebda() 밖에서 부르면 안 되고, 실제로 그 함수만 부른다.
 *   - ebda_rsrc_rsrc() 는 실패할 때 io_mem 을 iounmap 하고 돌아가는데,
 *     호출자인 ibmphp_access_ebda() 도 out 라벨에서 다시 iounmap 한다.
 *   - ebda_rsrc_rsrc() 의 걷기는 항목 종류가 IO/MEM/PFMEM 일 때만 addr 를
 *     그 항목 길이만큼 밀어 준다. 예약 종류(0x02)를 만나면 addr 가 1 만
 *     밀린 채 다음 항목을 읽게 된다.
 *   - free_ebda_hpc() 는 controller 를 통째로 버리는데, ISA 형 컨트롤러가
 *     ebda_rsrc_controller() 안에서 잡은 request_region 은
 *     ibmphp_free_ebda_hpc_queue() 쪽에서만 풀어 준다.
 *   - create_file_name() 이 돌려주는 것은 static 배열의 주소다. 호출자가
 *     곧바로 snprintf 로 베끼기 때문에 지금 구조에서는 문제가 되지 않지만,
 *     재진입은 불가능하다.
 *
 * === NVMe 관점 ===
 * drivers/nvme/ 에서 이 파일의 심볼을 쓰는 곳은 없다(전수 grep 확인).
 * 시기적으로도 접점이 없다 — 이 코드는 2001~2003년 IBM 서버용이고 NVMe
 * 규격은 2011년에 나왔다. 다만 "펌웨어가 남긴 표를 읽어 그것을 근거로
 * 자원과 장치를 세운다" 는 구조 자체는 오늘날 ACPI 를 읽어 PCI 호스트
 * 브리지의 자원 창을 세우는 경로와 같은 모양이다. NVMe SSD 를 핫플러그로
 * 꽂을 때 그 SSD 의 BAR 가 어느 창 안에서 배정되는지를 정하는 것이 결국
 * 이런 펌웨어 표라는 점에서, 이 파일은 그 계보의 이른 예에 해당한다.
 */

#include <linux/module.h> /* [한국어] MODULE_DEVICE_TABLE 매크로를 쓰기 위해 필요하다 — 이 파일의 id_table 이 그것으로 노출된다 */
#include <linux/errno.h> /* [한국어] -ENODEV/-ENOMEM/-EINVAL 같은 오류 코드 정의 */
#include <linux/mm.h> /* [한국어] 메모리 관리 일반 헤더 */
#include <linux/slab.h> /* [한국어] kzalloc_obj/kzalloc_objs/kfree — 이 파일이 만드는 모든 목록 항목의 할당에 쓴다 */
#include <linux/pci.h> /* [한국어] struct pci_dev, pci_register_driver, pci_find_bus, PCI_VENDOR_ID_IBM 등 PCI 코어 API */
#include <linux/list.h> /* [한국어] list_head 와 list_add/list_for_each_entry — 이 파일이 세우는 목록 여덟 개가 모두 이 자료구조다 */
#include <linux/init.h> /* [한국어] __init 표시. 이 파일의 파싱 함수 대부분이 초기화 뒤 버려진다 */
#include "ibmphp.h" /* [한국어] 이 드라이버 한 벌이 공유하는 헤더. struct controller / slot / bus_info / ebda_* 정의와 EBDA_ 비트 마스크가 여기 있다 */

/*
 * POST builds data blocks(in this data block definition, a char-1
 * byte, short(or word)-2 byte, long(dword)-4 byte) in the Extended
 * BIOS Data Area which describe the configuration of the hot-plug
 * controllers and resources used by the PCI Hot-Plug devices.
 *
 * This file walks EBDA, maps data block from physical addr,
 * reconstruct linked lists about all system resource(MEM, PFM, IO)
 * already assigned by POST, as well as linked lists about hot plug
 * controllers (ctlr#, slot#, bus&slot features...)
 */

/* Global lists */
LIST_HEAD(ibmphp_ebda_pci_rsrc_head); /* [한국어] **POST 가 이미 배정해 둔 자원 항목의 목록 머리.** ebda_rsrc_rsrc() 가 채우고 ibmphp_res.c:491 의 ibmphp_rsrc_init() 이 읽어 자유 목록 장부를 세운다. 전역이라 ibmphp.h:259 에 extern 으로 선언되어 있다 */
LIST_HEAD(ibmphp_slot_head); /* [한국어] **슬롯 목록의 머리.** ebda_rsrc_controller() 가 채우고 ibmphp_core.c 와 ibmphp_hpc.c 가 훑는다. 역시 ibmphp.h:260 에 extern 이다 */

/* Local variables */
static struct ebda_hpc_list *hpc_list_ptr; /* [한국어] 핫스왑 블록의 RC 하위 블록에서 읽은 값(판, 컨트롤러 개수, 기술자 배열 오프셋)을 담는다. 해제하지 않고 드라이버가 살아 있는 내내 둔다 — ibmphp_get_total_controllers() 가 읽기 때문이다 */
static struct ebda_rsrc_list *rsrc_list_ptr; /* [한국어] RE 하위 블록에서 읽은 값(판, 항목 개수, 항목 배열 오프셋)을 담는다. ebda_rsrc_rsrc() 가 표를 다 읽은 뒤 버리고 NULL 로 둔다 */
static struct rio_table_hdr *rio_table_ptr = NULL; /* [한국어] RIO 블록의 머리(판 번호, 확장성 항목 수, RIO 항목 수, 표 오프셋). 명시적으로 NULL 로 초기화한 이유는 RIO 블록이 없는 시스템에서 create_file_name() 이 이 포인터의 유무로 갈라지기 때문이다 */
static LIST_HEAD(ebda_hpc_head); /* [한국어] 컨트롤러 목록의 머리. 파일 안에서만 쓰고, 개수만 ibmphp_get_total_controllers() 로 밖에 알린다 */
static LIST_HEAD(bus_info_head); /* [한국어] 버스별 슬롯 번호 범위 목록의 머리. 밖에서는 ibmphp_find_same_bus_num()/ibmphp_get_bus_index() 로만 본다 */
static LIST_HEAD(rio_vg_head); /* [한국어] RIO 표에서 뽑은 **섀시** 항목 목록의 머리(rio_type 4 또는 5) */
static LIST_HEAD(rio_lo_head); /* [한국어] RIO 표에서 뽑은 **확장 상자** 항목 목록의 머리(rio_type 6 또는 7) */
static LIST_HEAD(opt_vg_head); /* [한국어] 섀시를 섀시 번호별로 뭉친 목록의 머리. combine_wpg_for_chassis() 가 만든다 */
static LIST_HEAD(opt_lo_head); /* [한국어] 확장 상자를 섀시 번호별로 뭉친 목록의 머리. combine_wpg_for_expansion() 이 만든다 */
static void __iomem *io_mem; /* [한국어] **ioremap 으로 매핑해 둔 EBDA 전체의 커널 가상 주소.** 이 파일의 모든 readb/readw/readl 이 이 포인터를 기준으로 오프셋을 더해 읽는다. ibmphp_access_ebda() 가 잡고 풀며, 그 사이에만 유효하다 */

/* Local functions */
static int ebda_rsrc_controller(void); /* [한국어] ibmphp_access_ebda() 가 아래쪽 정의보다 먼저 부르므로 미리 선언한다 */
static int ebda_rsrc_rsrc(void); /* [한국어] 같은 이유로 미리 선언한다 */
static int ebda_rio_table(void); /* [한국어] 같은 이유로 미리 선언한다 */

/* [한국어]
 * alloc_ebda_hpc_list - 컨트롤러 기술자 묶음의 머리 구조체를 하나 잡는다
 *
 * @return: 0 으로 채워진 struct ebda_hpc_list. 메모리가 없으면 NULL.
 *
 * EBDA 의 핫스왑 블록에서 읽어 낼 세 값(format, num_ctlrs, 기술자 배열의
 * 오프셋)을 담아 둘 자리를 미리 만든다. 값 자체는 호출자가 채운다.
 *
 * __init 이므로 초기화가 끝나면 이 코드는 버려진다. 잡은 메모리는 파일
 * 전역 hpc_list_ptr 이 들고 있으며, ebda_rsrc_rsrc() 가 쌍인
 * rsrc_list_ptr 을 버리는 것과 달리 이쪽은 해제하는 곳이 없다 —
 * ibmphp_get_total_controllers() 가 드라이버가 살아 있는 내내 이 구조체를
 * 읽기 때문이다.
 *
 * 실행 컨텍스트: 모듈 초기화(프로세스 컨텍스트), 단일 스레드.
 *
 * 호출 체인:  ibmphp_access_ebda() -> [이 함수] -> kzalloc_obj()
 */
static struct ebda_hpc_list * __init alloc_ebda_hpc_list(void)
{
	return kzalloc_obj(struct ebda_hpc_list); /* [한국어] 0 으로 채워진 머리 구조체 하나를 잡아 그대로 돌려준다. 값은 호출자가 채운다 */
}

/* [한국어]
 * alloc_ebda_hpc - 컨트롤러 하나와 그에 딸린 슬롯·버스 배열을 한꺼번에 잡는다
 *
 * @slot_count: 이 컨트롤러가 담당하는 슬롯 개수. EBDA 기술자에서 읽은 값.
 * @bus_count:  이 컨트롤러가 담당하는 버스 개수. 같은 곳에서 읽은 값.
 * @return: 세 덩이가 모두 잡힌 struct controller. 하나라도 실패하면
 *          앞서 잡은 것을 되돌리고 NULL.
 *
 * 컨트롤러 구조체는 슬롯 배열과 버스 배열을 포인터로 들고 있어 세 번의
 * 할당이 필요하다. 그래서 실패 지점마다 되돌릴 곳이 다르고, 라벨 세 개를
 * 쓰는 계단식 정리(goto error_slots -> error_contr -> error)가 된다.
 * 개수를 인자로 받는 이유는 EBDA 표가 컨트롤러마다 다른 개수를 적어 두기
 * 때문이다.
 *
 * kzalloc 계열이라 잡자마자 0 으로 채워진다. 뒤에서 ctlr_type 을 읽기
 * 전까지 union u 가 쓰레기 값을 갖지 않는다는 뜻이다.
 *
 * 실행 컨텍스트: 모듈 초기화(프로세스 컨텍스트), 단일 스레드.
 *
 * 호출 체인:  ebda_rsrc_controller() -> [이 함수] -> kzalloc_obj/kzalloc_objs()
 */
static struct controller *alloc_ebda_hpc(u32 slot_count, u32 bus_count)
{
	struct controller *controller; /* [한국어] 만들 컨트롤러 구조체 */
	struct ebda_hpc_slot *slots; /* [한국어] 그에 딸릴 슬롯 배열 */
	struct ebda_hpc_bus *buses; /* [한국어] 그에 딸릴 버스 배열 */

	controller = kzalloc_obj(struct controller); /* [한국어] 컨트롤러 구조체를 먼저 잡는다. kzalloc 이라 union u 를 포함해 전부 0 으로 시작한다 */
	if (!controller) /* [한국어] 못 잡으면 */
		goto error; /* [한국어] 되돌릴 것이 없으므로 맨 끝 라벨로 간다 */

	slots = kzalloc_objs(struct ebda_hpc_slot, slot_count); /* [한국어] EBDA 가 알려 준 개수만큼 슬롯 배열을 잡는다 */
	if (!slots) /* [한국어] 못 잡으면 */
		goto error_contr; /* [한국어] 컨트롤러만 되돌리는 라벨로 간다 */
	controller->slots = slots; /* [한국어] 잡은 배열을 컨트롤러에 매단다 */

	buses = kzalloc_objs(struct ebda_hpc_bus, bus_count); /* [한국어] 같은 방식으로 버스 배열을 잡는다 */
	if (!buses) /* [한국어] 못 잡으면 */
		goto error_slots; /* [한국어] 슬롯 배열까지 되돌리는 라벨로 간다 */
	controller->buses = buses; /* [한국어] 잡은 배열을 컨트롤러에 매단다 */

	return controller; /* [한국어] 셋을 다 잡았으면 컨트롤러를 돌려준다 */
error_slots: /* [한국어] 버스 배열 할당 실패 — 슬롯 배열부터 되돌린다 */
	kfree(controller->slots); /* [한국어] 슬롯 배열을 버리고 아래로 이어진다 */
error_contr: /* [한국어] 슬롯 배열 할당 실패 — 컨트롤러만 되돌리면 된다 */
	kfree(controller); /* [한국어] 컨트롤러를 버리고 아래로 이어진다 */
error: /* [한국어] 컨트롤러 할당 실패 — 되돌릴 것이 없다 */
	return NULL; /* [한국어] 호출자에게 실패를 NULL 로 알린다 */
}

/* [한국어]
 * free_ebda_hpc - alloc_ebda_hpc() 가 잡은 세 덩이를 모두 되돌린다
 *
 * @controller: 버릴 컨트롤러. NULL 을 넘기면 안 된다(검사하지 않는다).
 *
 * alloc_ebda_hpc() 의 짝이다. 슬롯 배열과 버스 배열을 먼저 버리고 컨트롤러
 * 자신을 버린다. 순서를 뒤집으면 이미 해제된 구조체에서 포인터를 읽게 된다.
 *
 * [관찰] ISA 형 컨트롤러(ctlr_type 0)가 ebda_rsrc_controller() 에서 잡은
 * request_region 은 여기서 풀지 않는다. 그 해제는
 * ibmphp_free_ebda_hpc_queue() 안에서 이 함수를 부르기 직전에 따로 한다.
 * 따라서 ebda_rsrc_controller() 의 실패 경로가 이 함수를 부를 때는 그
 * 영역이 남는다.
 *
 * 실행 컨텍스트: 모듈 초기화 실패 경로와 모듈 해제, 둘 다 프로세스 컨텍스트.
 *
 * 호출 체인:  ebda_rsrc_controller() 실패 경로 / ibmphp_free_ebda_hpc_queue()
 *               -> [이 함수] -> kfree()
 */
static void free_ebda_hpc(struct controller *controller)
{
	kfree(controller->slots); /* [한국어] 슬롯 배열을 버린다 */
	kfree(controller->buses); /* [한국어] 버스 배열을 버린다 */
	kfree(controller); /* [한국어] 컨트롤러 자신을 마지막에 버린다 — 순서를 뒤집으면 이미 해제된 구조체에서 포인터를 읽게 된다 */
}

/* [한국어]
 * alloc_ebda_rsrc_list - 자원 항목 묶음의 머리 구조체를 하나 잡는다
 *
 * @return: 0 으로 채워진 struct ebda_rsrc_list. 메모리가 없으면 NULL.
 *
 * EBDA 의 RE 하위 블록에서 읽어 낼 세 값(format, num_entries, 항목 배열의
 * 오프셋)을 담아 둘 자리다. alloc_ebda_hpc_list() 와 짝을 이루는 함수이며,
 * 이쪽 구조체는 ebda_rsrc_rsrc() 가 표를 다 읽은 뒤 kfree 로 버린다 —
 * 자원 항목이 이미 별도 목록으로 옮겨져 머리가 더 이상 필요 없기 때문이다.
 *
 * 실행 컨텍스트: 모듈 초기화(프로세스 컨텍스트), 단일 스레드.
 *
 * 호출 체인:  ibmphp_access_ebda() -> [이 함수] -> kzalloc_obj()
 */
static struct ebda_rsrc_list * __init alloc_ebda_rsrc_list(void)
{
	return kzalloc_obj(struct ebda_rsrc_list); /* [한국어] 0 으로 채워진 머리 구조체 하나를 잡아 그대로 돌려준다 */
}

/* [한국어]
 * alloc_ebda_pci_rsrc - 자원 항목 하나를 담을 구조체를 잡는다
 *
 * @return: 0 으로 채워진 struct ebda_pci_rsrc. 메모리가 없으면 NULL.
 *
 * "POST 가 이 버스의 이 장치에 이 구간을 이미 줬다" 는 기록 하나에 해당한다.
 * 여기서 만든 것들이 ibmphp_ebda_pci_rsrc_head 목록에 매달리고, 그 목록이
 * ibmphp_res.c 의 ibmphp_rsrc_init() 이 자유 목록 장부를 세우는 유일한
 * 원재료가 된다.
 *
 * 다른 alloc_ 계열과 달리 __init 가 아닌데, 정작 부르는 곳은 __init 인
 * ebda_rsrc_rsrc() 한 곳뿐이다(전수 grep 확인).
 *
 * 실행 컨텍스트: 모듈 초기화(프로세스 컨텍스트), 단일 스레드.
 *
 * 호출 체인:  ebda_rsrc_rsrc() -> [이 함수] -> kzalloc_obj()
 */
static struct ebda_pci_rsrc *alloc_ebda_pci_rsrc(void)
{
	return kzalloc_obj(struct ebda_pci_rsrc); /* [한국어] 자원 항목 하나를 담을 구조체를 0 으로 채워 잡는다 */
}

/* [한국어]
 * print_bus_info - bus_info 목록 전체를 디버그 로그로 찍는다
 *
 * 버스마다 슬롯 번호의 최소·최대·개수와 속도 능력 다섯 가지를 남긴다.
 * debug() 는 ibmphp.h 에서 정의되며 모듈 파라미터로 켜지 않으면 아무것도
 * 찍지 않으므로, 평소에는 목록을 훑기만 하는 빈 루프가 된다.
 *
 * ebda_rsrc_controller() 가 컨트롤러 하나를 다 읽을 때마다 부르기 때문에
 * 컨트롤러가 여럿이면 같은 목록이 여러 번 찍힌다.
 *
 * 실행 컨텍스트: 모듈 초기화(프로세스 컨텍스트). 목록을 읽기만 하므로 락이
 * 없지만, 초기화 중에는 이 목록을 고치는 다른 실행 흐름이 없다.
 *
 * 호출 체인:  ebda_rsrc_controller() -> [이 함수] -> debug()
 */
static void __init print_bus_info(void)
{
	struct bus_info *ptr; /* [한국어] 목록을 훑을 반복 포인터 */

	list_for_each_entry(ptr, &bus_info_head, bus_info_list) { /* [한국어] 버스별 정보를 하나씩 본다 */
		debug("%s - slot_min = %x\n", __func__, ptr->slot_min); /* [한국어] 이 버스의 가장 작은 슬롯 번호 */
		debug("%s - slot_max = %x\n", __func__, ptr->slot_max); /* [한국어] 이 버스의 가장 큰 슬롯 번호 */
		debug("%s - slot_count = %x\n", __func__, ptr->slot_count); /* [한국어] 이 버스에 달린 슬롯 개수 */
		debug("%s - bus# = %x\n", __func__, ptr->busno); /* [한국어] 버스 번호 자신 */
		debug("%s - current_speed = %x\n", __func__, ptr->current_speed); /* [한국어] 현재 동작 속도 — ebda_rsrc_controller() 가 0xff(모름)로 두고, ibmphp_hpc.c 가 실제 값을 채운다 */
		debug("%s - controller_id = %x\n", __func__, ptr->controller_id); /* [한국어] 이 버스를 담당하는 컨트롤러의 id */

		debug("%s - slots_at_33_conv = %x\n", __func__, ptr->slots_at_33_conv); /* [한국어] 33MHz 통상(conventional) PCI 로 쓸 수 있는 슬롯 수 */
		debug("%s - slots_at_66_conv = %x\n", __func__, ptr->slots_at_66_conv); /* [한국어] 66MHz 통상 PCI 로 쓸 수 있는 슬롯 수 */
		debug("%s - slots_at_66_pcix = %x\n", __func__, ptr->slots_at_66_pcix); /* [한국어] 66MHz PCI-X 로 쓸 수 있는 슬롯 수 */
		debug("%s - slots_at_100_pcix = %x\n", __func__, ptr->slots_at_100_pcix); /* [한국어] 100MHz PCI-X 로 쓸 수 있는 슬롯 수 */
		debug("%s - slots_at_133_pcix = %x\n", __func__, ptr->slots_at_133_pcix); /* [한국어] 133MHz PCI-X 로 쓸 수 있는 슬롯 수 */

	}
}

/* [한국어]
 * print_lo_info - 확장 상자(expansion box) RIO 항목 목록을 찍는다
 *
 * rio_lo_head 에 매달린 rio_detail 들을 순회하며 노드 id, 종류, 소유자 id,
 * 첫 슬롯 번호, wpindex, 섀시 번호를 남긴다. rio_type 이 6 이나 7 인
 * 항목들이 여기 모여 있다.
 *
 * 같은 구조의 print_vg_info() 와 달리 __init 가 붙어 있지 않다. 부르는 곳은
 * __init 인 ebda_rio_table() 한 곳뿐이다(전수 grep 확인).
 *
 * 실행 컨텍스트: 모듈 초기화(프로세스 컨텍스트).
 *
 * 호출 체인:  ebda_rio_table() -> [이 함수] -> debug()
 */
static void print_lo_info(void)
{
	struct rio_detail *ptr; /* [한국어] 목록을 훑을 반복 포인터 */
	debug("print_lo_info ----\n"); /* [한국어] 어느 목록을 찍는지 표시한다. 아래 print_vg_info() 와 달리 __func__ 대신 이름을 직접 적었다 */
	list_for_each_entry(ptr, &rio_lo_head, rio_detail_list) { /* [한국어] 확장 상자 항목을 하나씩 본다 */
		debug("%s - rio_node_id = %x\n", __func__, ptr->rio_node_id); /* [한국어] 이 RIO 노드의 id */
		debug("%s - rio_type = %x\n", __func__, ptr->rio_type); /* [한국어] 항목 종류 — 여기 있는 것은 6 이나 7 이다 */
		debug("%s - owner_id = %x\n", __func__, ptr->owner_id); /* [한국어] 이 항목을 소유한 노드의 id */
		debug("%s - first_slot_num = %x\n", __func__, ptr->first_slot_num); /* [한국어] 이 상자의 첫 슬롯 번호 — 슬롯 이름을 지을 때 기준이 되는 값이다 */
		debug("%s - wpindex = %x\n", __func__, ptr->wpindex); /* [한국어] wpindex. 이 드라이버 안에서 읽는 곳은 이 디버그 출력뿐이다 */
		debug("%s - chassis_num = %x\n", __func__, ptr->chassis_num); /* [한국어] 섀시 번호 — combine_wpg_for_expansion() 이 이 값을 열쇠로 항목을 뭉친다 */

	}
}

/* [한국어]
 * print_vg_info - 섀시(chassis) RIO 항목 목록을 찍는다
 *
 * rio_vg_head 에 매달린 rio_detail 들을 순회한다. rio_type 이 4 나 5 인
 * 항목들이 여기 모여 있다. 찍는 필드는 print_lo_info() 와 같다.
 *
 * "vg" 라는 이름의 뜻은 코드에 근거가 없다 - 확인 못 함. 다만 이 목록이
 * 섀시용이라는 것은 ebda_rio_table() 의 상류 주석("create linked list of
 * chassis")에서 확인된다.
 *
 * 실행 컨텍스트: 모듈 초기화(프로세스 컨텍스트).
 *
 * 호출 체인:  ebda_rio_table() -> [이 함수] -> debug()
 */
static void print_vg_info(void)
{
	struct rio_detail *ptr; /* [한국어] 목록을 훑을 반복 포인터 */
	debug("%s ---\n", __func__); /* [한국어] 어느 목록을 찍는지 표시한다 */
	list_for_each_entry(ptr, &rio_vg_head, rio_detail_list) { /* [한국어] 섀시 항목을 하나씩 본다 */
		debug("%s - rio_node_id = %x\n", __func__, ptr->rio_node_id); /* [한국어] 이 RIO 노드의 id */
		debug("%s - rio_type = %x\n", __func__, ptr->rio_type); /* [한국어] 항목 종류 — 여기 있는 것은 4 나 5 다 */
		debug("%s - owner_id = %x\n", __func__, ptr->owner_id); /* [한국어] 이 항목을 소유한 노드의 id */
		debug("%s - first_slot_num = %x\n", __func__, ptr->first_slot_num); /* [한국어] 이 섀시의 첫 슬롯 번호 */
		debug("%s - wpindex = %x\n", __func__, ptr->wpindex); /* [한국어] wpindex */
		debug("%s - chassis_num = %x\n", __func__, ptr->chassis_num); /* [한국어] 섀시 번호 — combine_wpg_for_chassis() 가 이 값을 열쇠로 항목을 뭉친다 */

	}
}

/* [한국어]
 * print_ebda_pci_rsrc - POST 가 배정해 둔 자원 목록 전체를 찍는다
 *
 * ibmphp_ebda_pci_rsrc_head 를 순회하며 항목마다 종류, 버스 번호, 장치·함수,
 * 시작·끝 주소를 한 줄로 남긴다. 이 목록이 곧 ibmphp_res.c 가 장부를 세울 때
 * 읽을 원재료이므로, 장부가 이상할 때 가장 먼저 보게 되는 로그다.
 *
 * ebda_rsrc_rsrc() 가 표를 다 읽은 직후 한 번만 부른다.
 *
 * 실행 컨텍스트: 모듈 초기화(프로세스 컨텍스트).
 *
 * 호출 체인:  ebda_rsrc_rsrc() -> [이 함수] -> debug()
 */
static void __init print_ebda_pci_rsrc(void)
{
	struct ebda_pci_rsrc *ptr; /* [한국어] 목록을 훑을 반복 포인터 */

	list_for_each_entry(ptr, &ibmphp_ebda_pci_rsrc_head, ebda_pci_rsrc_list) { /* [한국어] POST 가 배정해 둔 자원 항목을 하나씩 본다 */
		debug("%s - rsrc type: %x bus#: %x dev_func: %x start addr: %x end addr: %x\n", /* [한국어] 항목 하나를 한 줄로 남긴다. 이 목록이 ibmphp_res.c 가 장부를 세울 원재료이므로 장부가 이상할 때 가장 먼저 보게 되는 로그다 */
			__func__, ptr->rsrc_type, ptr->bus_num, ptr->dev_fun, ptr->start_addr, ptr->end_addr); /* [한국어] 종류 바이트, 버스 번호, 장치·함수, 시작 주소, 끝 주소를 차례로 넘긴다 */
	}
}

/* [한국어]
 * print_ibm_slot - 등록된 슬롯들의 번호만 찍는다
 *
 * ibmphp_slot_head 를 순회하며 slot->number 하나씩만 남긴다. 슬롯 구조체의
 * 나머지 필드는 print_ebda_hpc() 가 컨트롤러 쪽에서 찍으므로 여기서는
 * 중복을 피한 것으로 보인다.
 *
 * ebda_rsrc_controller() 가 모든 컨트롤러를 처리하고 슬롯을 코어에 등록한
 * 뒤 마지막에 한 번 부른다.
 *
 * 실행 컨텍스트: 모듈 초기화(프로세스 컨텍스트).
 *
 * 호출 체인:  ebda_rsrc_controller() -> [이 함수] -> debug()
 */
static void __init print_ibm_slot(void)
{
	struct slot *ptr; /* [한국어] 목록을 훑을 반복 포인터 */

	list_for_each_entry(ptr, &ibmphp_slot_head, ibm_slot_list) { /* [한국어] 등록된 슬롯을 하나씩 본다 */
		debug("%s - slot_number: %x\n", __func__, ptr->number); /* [한국어] 물리 슬롯 번호만 남긴다 — 나머지 필드는 print_ebda_hpc() 가 컨트롤러 쪽에서 찍는다 */
	}
}

/* [한국어]
 * print_opt_vg - 섀시 단위로 합쳐 놓은 opt_rio 목록을 찍는다
 *
 * combine_wpg_for_chassis() 가 rio_vg_head 를 섀시 번호별로 뭉쳐 만든
 * opt_vg_head 를 순회한다. 항목마다 종류, 섀시 번호, 그리고 그 섀시가
 * 차지하는 슬롯 번호 구간의 양 끝(first_slot_num, middle_num)을 남긴다.
 *
 * 짝인 확장 상자 쪽(opt_lo_head)에는 대응하는 출력 함수가 없다.
 *
 * 실행 컨텍스트: 모듈 초기화(프로세스 컨텍스트).
 *
 * 호출 체인:  combine_wpg_for_chassis() -> [이 함수] -> debug()
 */
static void __init print_opt_vg(void)
{
	struct opt_rio *ptr; /* [한국어] 목록을 훑을 반복 포인터 */
	debug("%s ---\n", __func__); /* [한국어] 어느 목록을 찍는지 표시한다 */
	list_for_each_entry(ptr, &opt_vg_head, opt_rio_list) { /* [한국어] 섀시 번호별로 뭉쳐 놓은 항목을 하나씩 본다 */
		debug("%s - rio_type %x\n", __func__, ptr->rio_type); /* [한국어] 항목 종류(4 또는 5) */
		debug("%s - chassis_num: %x\n", __func__, ptr->chassis_num); /* [한국어] 섀시 번호 — 이 목록의 열쇠다 */
		debug("%s - first_slot_num: %x\n", __func__, ptr->first_slot_num); /* [한국어] 이 섀시가 차지하는 슬롯 번호 구간의 **아래쪽 끝** */
		debug("%s - middle_num: %x\n", __func__, ptr->middle_num); /* [한국어] 이름과 달리 가운데 값이 아니라 구간의 **위쪽 끝**이다. combine_wpg_for_chassis() 가 max 로 올려 둔 값이다 */
	}
}

/* [한국어]
 * print_ebda_hpc - 컨트롤러 목록 전체를 종류별로 나눠 찍는다
 *
 * ebda_hpc_head 를 순회하며 컨트롤러 하나마다 담당 슬롯 전부(물리 슬롯 번호,
 * 그 슬롯의 버스 번호, 컨트롤러 안 인덱스, 슬롯 능력)와 담당 버스 번호
 * 전부를 남긴 뒤, ctlr_type 에 따라 서로 다른 접근 정보를 찍는다 —
 * 1 이면 PCI(버스·devfn), 0 이면 ISA(I/O 포트 구간), 2 나 4 면 i2c(wpegbbar,
 * i2c 주소)다. union u 를 종류에 맞게 읽어야 하므로 switch 가 필요하다.
 *
 * EBDA 파싱 결과 전체를 한눈에 보는 함수라, 표를 잘못 읽었을 때 어디서
 * 어긋났는지 확인하는 자리가 된다.
 *
 * 실행 컨텍스트: 모듈 초기화(프로세스 컨텍스트).
 *
 * 호출 체인:  ebda_rsrc_controller() -> [이 함수] -> debug()
 */
static void __init print_ebda_hpc(void)
{
	struct controller *hpc_ptr; /* [한국어] 컨트롤러 목록을 훑을 반복 포인터 */
	u16 index; /* [한국어] 슬롯·버스 배열을 훑을 인덱스 */

	list_for_each_entry(hpc_ptr, &ebda_hpc_head, ebda_hpc_list) { /* [한국어] 컨트롤러를 하나씩 본다 */
		for (index = 0; index < hpc_ptr->slot_count; index++) { /* [한국어] 이 컨트롤러가 담당하는 슬롯 전부를 본다 */
			debug("%s - physical slot#: %x\n", __func__, hpc_ptr->slots[index].slot_num); /* [한국어] 물리 슬롯 번호 */
			debug("%s - pci bus# of the slot: %x\n", __func__, hpc_ptr->slots[index].slot_bus_num); /* [한국어] 그 슬롯이 붙어 있는 PCI 버스 번호 */
			debug("%s - index into ctlr addr: %x\n", __func__, hpc_ptr->slots[index].ctl_index); /* [한국어] 컨트롤러 레지스터를 짚을 때 쓰는 슬롯 인덱스 */
			debug("%s - cap of the slot: %x\n", __func__, hpc_ptr->slots[index].slot_cap); /* [한국어] 슬롯 능력 비트 — EBDA_SLOT_133_MAX 계열로 풀린다 */
		}

		for (index = 0; index < hpc_ptr->bus_count; index++) /* [한국어] 이 컨트롤러가 담당하는 버스 전부를 본다 */
			debug("%s - bus# of each bus controlled by this ctlr: %x\n", __func__, hpc_ptr->buses[index].bus_num); /* [한국어] 버스 번호만 남긴다 */

		debug("%s - type of hpc: %x\n", __func__, hpc_ptr->ctlr_type); /* [한국어] 컨트롤러가 어디에 붙어 있는지 */
		switch (hpc_ptr->ctlr_type) { /* [한국어] 종류에 따라 union u 의 어느 갈래를 읽을지가 달라진다 */
		case 1: /* [한국어] PCI 형 컨트롤러 */
			debug("%s - bus: %x\n", __func__, hpc_ptr->u.pci_ctlr.bus); /* [한국어] 컨트롤러가 붙어 있는 PCI 버스 번호 */
			debug("%s - dev_fun: %x\n", __func__, hpc_ptr->u.pci_ctlr.dev_fun); /* [한국어] 컨트롤러의 장치·함수 번호 */
			debug("%s - irq: %x\n", __func__, hpc_ptr->irq); /* [한국어] 컨트롤러가 쓰는 IRQ */
			break; /* [한국어] 갈래를 벗어난다 */

		case 0: /* [한국어] ISA 형 컨트롤러 */
			debug("%s - io_start: %x\n", __func__, hpc_ptr->u.isa_ctlr.io_start); /* [한국어] 컨트롤러가 차지하는 I/O 포트 구간의 시작 */
			debug("%s - io_end: %x\n", __func__, hpc_ptr->u.isa_ctlr.io_end); /* [한국어] 그 구간의 끝 */
			debug("%s - irq: %x\n", __func__, hpc_ptr->irq); /* [한국어] 컨트롤러가 쓰는 IRQ */
			break; /* [한국어] 갈래를 벗어난다 */

		case 2: /* [한국어] i2c 형 컨트롤러 — 2 와 4 를 같이 다룬다 */
		case 4: /* [한국어] 4 는 create_file_name() 이 확장 상자로 보는 종류다 */
			debug("%s - wpegbbar: %lx\n", __func__, hpc_ptr->u.wpeg_ctlr.wpegbbar); /* [한국어] i2c 접근의 기준 주소 */
			debug("%s - i2c_addr: %x\n", __func__, hpc_ptr->u.wpeg_ctlr.i2c_addr); /* [한국어] 컨트롤러의 i2c 주소 */
			debug("%s - irq: %x\n", __func__, hpc_ptr->irq); /* [한국어] 컨트롤러가 쓰는 IRQ */
			break; /* [한국어] 갈래를 벗어난다 */
		}
	}
}

/* [한국어]
 * ibmphp_access_ebda - EBDA 를 찾아 매핑하고 그 안의 블록들을 훑는다
 *
 * @return: 0 이면 성공. 필요한 블록을 못 찾았거나 판이 맞지 않으면 -ENODEV,
 *          매핑이나 할당에 실패하면 -ENOMEM. 호출자인 ibmphp_init() 은
 *          0 이 아니면 그 자리에서 초기화를 접는다.
 *
 * 이 드라이버가 하는 첫 실질적인 일이며, 이 파일 전체의 진입점이다.
 * 하드웨어를 탐색하지 않고 BIOS(POST)가 남긴 표를 읽는 것이 전부다.
 *
 * 동작은 세 단계다.
 *   1. EBDA 를 찾는다. 물리 주소 0x40E 에서 워드를 읽어 실모드 세그먼트를
 *      얻고, 그것을 16배 한 곳의 첫 바이트에서 크기를 KiB 단위로 읽은 뒤,
 *      그 크기만큼 통째로 ioremap 한다. 세 번의 ioremap 중 앞의 둘은 값을
 *      읽자마자 곧바로 iounmap 하고, 마지막 매핑만 파일 전역 io_mem 에
 *      남겨 이 파일의 다른 파싱 함수들이 쓰게 한다.
 *   2. 오프셋 0x180 부터 블록 사슬을 걷는다. 블록 머리는 (다음 블록
 *      오프셋, 블록 id) 두 워드이고, 다음 오프셋이 0 이면 끝이다. 관심
 *      있는 id 는 핫스왑(0x4853)과 RIO(0x4752) 둘뿐이고 나머지는 건너뛴다.
 *      걷기 전에 매핑 범위를 넘지 않는지 WARN 으로 확인한다.
 *   3. 두 블록을 읽어 hpc_list_ptr / rsrc_list_ptr / rio_table_ptr 을
 *      채운 다음, 실제 파싱 함수 셋을 순서대로 부른다.
 *
 * 셋의 순서에는 의존 관계가 있다. ebda_rio_table() 이 먼저여야
 * ebda_rsrc_controller() 안의 create_file_name() 이 섀시·확장 상자 목록을
 * 볼 수 있고, ebda_rsrc_controller() 가 먼저여야 ebda_rsrc_rsrc() 가
 * 만드는 자원 목록이 이미 등록된 슬롯들과 짝이 맞는다.
 *
 * 실행 컨텍스트: 모듈 초기화(프로세스 컨텍스트), 단일 스레드. __init 이다.
 *
 * 에러 경로: error_nodev 라벨은 -ENODEV 를 담고 out 으로 떨어지며, out 은
 * io_mem 을 반드시 iounmap 한다. 즉 어느 경로로 나가든 마지막 매핑은
 * 풀린다. 다만 ebda_rsrc_rsrc() 는 자기 실패 경로에서 io_mem 을 한 번 더
 * iounmap 한다.
 *
 * 호출 체인:
 *   ibmphp_init() -> [이 함수] -> ioremap()/readw()/readb()
 *                             -> ebda_rio_table()
 *                             -> ebda_rsrc_controller()
 *                             -> ebda_rsrc_rsrc()
 */
int __init ibmphp_access_ebda(void)
{
	u8 format, num_ctlrs, rio_complete, hs_complete, ebda_sz; /* [한국어] format 은 핫스왑 블록의 판 번호(4 만 지원), num_ctlrs 는 컨트롤러 개수, rio_complete/hs_complete 는 두 블록을 각각 찾았는지 표시, ebda_sz 는 EBDA 크기(KiB) */
	u16 ebda_seg, num_entries, next_offset, offset, blk_id, sub_addr, re, rc_id, re_id, base; /* [한국어] ebda_seg 는 EBDA 의 실모드 세그먼트, num_entries 는 자원 항목 개수, next_offset/offset 은 블록 걷기 위치, blk_id 는 블록 종류, sub_addr 는 하위 블록 걷기 위치, re 는 RE 하위 블록까지의 상대 오프셋, rc_id/re_id 는 하위 블록 서명, base 는 하위 블록들의 기준점 */
	int rc = 0; /* [한국어] 호출자에게 돌려줄 값. 성공이면 0 으로 남는다 */


	rio_complete = 0; /* [한국어] RIO 블록을 아직 못 찾았다 */
	hs_complete = 0; /* [한국어] 핫스왑 블록도 아직 못 찾았다 */

	io_mem = ioremap((0x40 << 4) + 0x0e, 2); /* [한국어] **물리 주소 0x40E 에 워드 하나를 매핑한다.** 0x40 은 BIOS 데이터 영역의 세그먼트이고 그 안 오프셋 0x0e 에 EBDA 세그먼트가 적혀 있다. (0x40 << 4) 가 세그먼트를 물리 주소로 바꾸는 계산이다 */
	if (!io_mem) /* [한국어] 매핑에 실패하면 */
		return -ENOMEM; /* [한국어] 메모리 부족으로 돌아간다 */
	ebda_seg = readw(io_mem); /* [한국어] EBDA 의 실모드 세그먼트 값을 읽는다 */
	iounmap(io_mem); /* [한국어] 값 하나만 필요했으므로 곧바로 매핑을 푼다 */
	debug("returned ebda segment: %x\n", ebda_seg); /* [한국어] 읽은 세그먼트를 남긴다 */

	io_mem = ioremap(ebda_seg<<4, 1); /* [한국어] 세그먼트를 16배 하면 물리 주소가 된다. 그 첫 바이트만 매핑한다 */
	if (!io_mem) /* [한국어] 매핑에 실패하면 */
		return -ENOMEM; /* [한국어] 메모리 부족으로 돌아간다 */
	ebda_sz = readb(io_mem); /* [한국어] **EBDA 의 크기를 KiB 단위로 읽는다** — 첫 바이트가 크기를 담고 있다 */
	iounmap(io_mem); /* [한국어] 값 하나만 필요했으므로 곧바로 매핑을 푼다 */
	debug("ebda size: %d(KiB)\n", ebda_sz); /* [한국어] 읽은 크기를 남긴다 */
	if (ebda_sz == 0) /* [한국어] 크기가 0 이면 EBDA 가 없는 것이다 */
		return -ENOMEM; /* [한국어] 메모리 부족으로 돌아간다 */

	io_mem = ioremap(ebda_seg<<4, (ebda_sz * 1024)); /* [한국어] **이번에는 EBDA 전체를 매핑한다.** 이 매핑이 파일 전역 io_mem 에 남아 아래의 파싱 함수 셋이 그대로 쓴다 */
	if (!io_mem) /* [한국어] 매핑에 실패하면 */
		return -ENOMEM; /* [한국어] 메모리 부족으로 돌아간다 */
	next_offset = 0x180; /* [한국어] 블록 걷기의 시작 오프셋. 0x180 이라는 값의 근거는 코드에 없다 — 확인 못 함 */

	for (;;) { /* [한국어] 블록 사슬을 끝까지 걷는다 */
		offset = next_offset; /* [한국어] 앞 회차가 알려 준 다음 블록 위치로 옮긴다 */

		/* Make sure what we read is still in the mapped section */
		if (WARN(offset > (ebda_sz * 1024 - 4), /* [한국어] **읽을 자리가 매핑 범위 안인지 먼저 확인한다.** 표가 망가져 있으면 오프셋이 EBDA 밖을 가리킬 수 있고, 그러면 매핑되지 않은 주소를 읽게 된다. 워드 두 개(4바이트)를 읽을 것이므로 끝에서 4 를 뺀다 */
			 "ibmphp_ebda: next read is beyond ebda_sz\n")) /* [한국어] 경고 문구 */
			break; /* [한국어] 범위를 벗어나면 걷기를 멈춘다 */

		next_offset = readw(io_mem + offset);	/* offset of next blk */ /* [한국어] 블록 머리의 첫 워드 — 다음 블록의 오프셋 */

		offset += 2; /* [한국어] 읽은 만큼 위치를 민다 */
		if (next_offset == 0)	/* 0 indicate it's last blk */ /* [한국어] 다음 오프셋이 0 이면 여기가 마지막 블록이다 */
			break; /* [한국어] 걷기를 멈춘다 */
		blk_id = readw(io_mem + offset);	/* this blk id */ /* [한국어] 블록 머리의 둘째 워드 — 블록 종류 */

		offset += 2; /* [한국어] 읽은 만큼 위치를 민다 */
		/* check if it is hot swap block or rio block */
		if (blk_id != 0x4853 && blk_id != 0x4752) /* [한국어] 핫스왑도 RIO 도 아니면 이 파일이 볼 것이 없다 */
			continue; /* [한국어] 다음 블록으로 넘어간다 */
		/* found hs table */
		if (blk_id == 0x4853) { /* [한국어] **핫스왑 블록** — 컨트롤러와 자원 항목이 여기 들어 있다 */
			debug("now enter hot swap block---\n"); /* [한국어] 어느 블록에 들어왔는지 남긴다 */
			debug("hot blk id: %x\n", blk_id); /* [한국어] 블록 id 도 남긴다 */
			format = readb(io_mem + offset); /* [한국어] 몸통의 첫 바이트가 판 번호다 */

			offset += 1; /* [한국어] 읽은 만큼 위치를 민다 */
			if (format != 4) /* [한국어] **판이 4 가 아니면 이 코드가 아는 배치가 아니다** */
				goto error_nodev; /* [한국어] 장치 없음으로 나가는 라벨로 간다 */
			debug("hot blk format: %x\n", format); /* [한국어] 판 번호를 남긴다 */
			/* hot swap sub blk */
			base = offset; /* [한국어] 여기가 하위 블록들의 기준점이다. 아래의 상대 오프셋 re 가 이 값을 기준으로 더해진다 */

			sub_addr = base; /* [한국어] 첫 하위 블록(RC)은 기준점 바로 그 자리에 있다 */
			re = readw(io_mem + sub_addr);	/* next sub blk */ /* [한국어] RC 하위 블록의 첫 워드 — RE 하위 블록까지의 상대 오프셋 */

			sub_addr += 2; /* [한국어] 읽은 만큼 위치를 민다 */
			rc_id = readw(io_mem + sub_addr);	/* sub blk id */ /* [한국어] RC 하위 블록의 서명 */

			sub_addr += 2; /* [한국어] 읽은 만큼 위치를 민다 */
			if (rc_id != 0x5243) /* [한국어] **0x5243 이어야 한다.** 바이트 값이 R(0x52), C(0x43) 와 같고 상류 주석도 "rc sub blk signature" 라 적었다 */
				goto error_nodev; /* [한국어] 다르면 장치 없음으로 나간다 */
			/* rc sub blk signature  */
			num_ctlrs = readb(io_mem + sub_addr); /* [한국어] 컨트롤러 개수 */

			sub_addr += 1; /* [한국어] 읽은 만큼 위치를 민다 — 이제 sub_addr 가 컨트롤러 기술자 배열의 시작을 가리킨다 */
			hpc_list_ptr = alloc_ebda_hpc_list(); /* [한국어] 그 세 값을 담아 둘 머리 구조체를 잡는다 */
			if (!hpc_list_ptr) { /* [한국어] 못 잡으면 */
				rc = -ENOMEM; /* [한국어] 메모리 부족을 담아 */
				goto out; /* [한국어] 매핑을 푸는 공통 출구로 간다 */
			}
			hpc_list_ptr->format = format; /* [한국어] 판 번호를 담는다 */
			hpc_list_ptr->num_ctlrs = num_ctlrs; /* [한국어] 컨트롤러 개수를 담는다 */
			hpc_list_ptr->phys_addr = sub_addr;	/*  offset of RSRC_CONTROLLER blk */ /* [한국어] 기술자 배열의 오프셋을 담는다. 나중에 ebda_rsrc_controller() 가 이 값에서 걷기를 시작한다 */
			debug("info about hpc descriptor---\n"); /* [한국어] 읽은 내용을 남긴다 */
			debug("hot blk format: %x\n", format); /* [한국어] 판 번호 */
			debug("num of controller: %x\n", num_ctlrs); /* [한국어] 컨트롤러 개수 */
			debug("offset of hpc data structure entries: %x\n ", sub_addr); /* [한국어] 기술자 배열의 오프셋 */

			sub_addr = base + re;	/* re sub blk */ /* [한국어] **RE 하위 블록으로 건너뛴다.** 기준점에 상대 오프셋을 더한 자리다 */
			/* FIXME: rc is never used/checked */
			rc = readw(io_mem + sub_addr);	/* next sub blk */ /* [한국어] RE 하위 블록의 첫 워드. 상류가 바로 위에 FIXME 로 "rc is never used" 라 적어 둔 대입이다 — 여기 담긴 값은 아래에서 덮어써진다 */

			sub_addr += 2; /* [한국어] 읽은 만큼 위치를 민다 */
			re_id = readw(io_mem + sub_addr);	/* sub blk id */ /* [한국어] RE 하위 블록의 서명 */

			sub_addr += 2; /* [한국어] 읽은 만큼 위치를 민다 */
			if (re_id != 0x5245) /* [한국어] **0x5245 이어야 한다.** 바이트 값이 R(0x52), E(0x45) 와 같고 상류 주석도 "signature of re" 라 적었다 */
				goto error_nodev; /* [한국어] 다르면 장치 없음으로 나간다 */

			/* signature of re */
			num_entries = readw(io_mem + sub_addr); /* [한국어] 자원 항목 개수 */

			sub_addr += 2;	/* offset of RSRC_ENTRIES blk */ /* [한국어] 읽은 만큼 위치를 민다 — 이제 sub_addr 가 자원 항목 배열의 시작을 가리킨다 */
			rsrc_list_ptr = alloc_ebda_rsrc_list(); /* [한국어] 그 세 값을 담아 둘 머리 구조체를 잡는다 */
			if (!rsrc_list_ptr) { /* [한국어] 못 잡으면 */
				rc = -ENOMEM; /* [한국어] 메모리 부족을 담아 */
				goto out; /* [한국어] 매핑을 푸는 공통 출구로 간다 */
			}
			rsrc_list_ptr->format = format; /* [한국어] 판 번호를 담는다 */
			rsrc_list_ptr->num_entries = num_entries; /* [한국어] 자원 항목 개수를 담는다 */
			rsrc_list_ptr->phys_addr = sub_addr; /* [한국어] 항목 배열의 오프셋을 담는다. 나중에 ebda_rsrc_rsrc() 가 이 값에서 걷기를 시작한다 */

			debug("info about rsrc descriptor---\n"); /* [한국어] 읽은 내용을 남긴다 */
			debug("format: %x\n", format); /* [한국어] 판 번호 */
			debug("num of rsrc: %x\n", num_entries); /* [한국어] 자원 항목 개수 */
			debug("offset of rsrc data structure entries: %x\n ", sub_addr); /* [한국어] 항목 배열의 오프셋 */

			hs_complete = 1; /* [한국어] 핫스왑 블록을 다 읽었다고 표시한다 */
		} else {
		/* found rio table, blk_id == 0x4752 */
			debug("now enter io table ---\n"); /* [한국어] 어느 블록에 들어왔는지 남긴다 */
			debug("rio blk id: %x\n", blk_id); /* [한국어] 블록 id 도 남긴다 */

			rio_table_ptr = kzalloc_obj(struct rio_table_hdr); /* [한국어] RIO 표의 머리를 담을 구조체를 잡는다. 다른 두 머리와 달리 전용 alloc 함수 없이 직접 잡는다 */
			if (!rio_table_ptr) { /* [한국어] 못 잡으면 */
				rc = -ENOMEM; /* [한국어] 메모리 부족을 담아 */
				goto out; /* [한국어] 매핑을 푸는 공통 출구로 간다 */
			}
			rio_table_ptr->ver_num = readb(io_mem + offset); /* [한국어] 몸통의 첫 바이트가 판 번호. 3 일 때만 ebda_rio_table() 이 돈다 */
			rio_table_ptr->scal_count = readb(io_mem + offset + 1); /* [한국어] 둘째 바이트가 확장성 항목 개수 — 항목당 12바이트라 나중에 그만큼 건너뛴다 */
			rio_table_ptr->riodev_count = readb(io_mem + offset + 2); /* [한국어] 셋째 바이트가 RIO 장치 항목 개수 */
			rio_table_ptr->offset = offset + 3 ; /* [한국어] 넷째 바이트부터가 표의 몸통이다 */

			debug("info about rio table hdr ---\n"); /* [한국어] 읽은 내용을 남긴다 */
			debug("ver_num: %x\nscal_count: %x\nriodev_count: %x\noffset of rio table: %x\n ", /* [한국어] 판 번호, 확장성 항목 수, RIO 항목 수, 표 오프셋을 한 번에 남긴다 */
				rio_table_ptr->ver_num, rio_table_ptr->scal_count,
				rio_table_ptr->riodev_count, rio_table_ptr->offset);

			rio_complete = 1; /* [한국어] RIO 블록을 다 읽었다고 표시한다 */
		}
	}

	if (!hs_complete && !rio_complete) /* [한국어] **두 블록을 하나도 못 찾았으면** 이 드라이버가 다룰 시스템이 아니다 */
		goto error_nodev; /* [한국어] 장치 없음으로 나간다 */

	if (rio_table_ptr) { /* [한국어] RIO 표를 읽어 두었으면 */
		if (rio_complete && rio_table_ptr->ver_num == 3) { /* [한국어] 그리고 그 판이 3 이면 — 이 코드가 아는 유일한 판이다 */
			rc = ebda_rio_table(); /* [한국어] 섀시·확장 상자 항목을 뽑는다 */
			if (rc) /* [한국어] 실패하면 */
				goto out; /* [한국어] 매핑을 푸는 공통 출구로 간다 */
		}
	}
	rc = ebda_rsrc_controller(); /* [한국어] **컨트롤러·슬롯·버스를 뽑고 슬롯을 핫플러그 코어에 등록한다.** RIO 표를 먼저 읽어야 하는 이유는 이 안의 create_file_name() 이 섀시 목록을 보기 때문이다 */
	if (rc) /* [한국어] 실패하면 */
		goto out; /* [한국어] 매핑을 푸는 공통 출구로 간다 */

	rc = ebda_rsrc_rsrc(); /* [한국어] 마지막으로 POST 가 배정해 둔 자원 항목을 목록으로 만든다 */
	goto out; /* [한국어] 그 결과를 그대로 들고 공통 출구로 간다 */
error_nodev: /* [한국어] **장치 없음 경로** — 표가 없거나 판이 맞지 않을 때 여기로 온다 */
	rc = -ENODEV; /* [한국어] 오류 코드를 담고 아래로 이어진다 */
out: /* [한국어] **공통 출구** — 어느 경로로 나가든 매핑을 반드시 푼다 */
	iounmap(io_mem); /* [한국어] EBDA 전체 매핑을 푼다 */
	return rc; /* [한국어] 담아 둔 결과를 돌려준다 */
}

/*
 * map info of scalability details and rio details from physical address
 */
/* [한국어]
 * ebda_rio_table - RIO 표에서 섀시와 확장 상자 항목을 뽑아 두 목록으로 나눈다
 *
 * @return: 항상 0, 항목 하나를 할당하지 못하면 -ENOMEM.
 *
 * 바로 위 상류 주석대로 확장성(scalability) 정보와 RIO 정보를 물리 주소에서
 * 읽어 온다. 다만 코드는 확장성 항목을 읽지 않고 건너뛴다 — 항목 하나가
 * 12바이트이므로 scal_count 만큼 곱한 값을 오프셋에 더해 넘어간다. 상류
 * 주석("we do concern about rio details")이 그 의도를 밝히고 있다.
 *
 * RIO 항목 하나는 15바이트 고정이며, 필드마다 오프셋을 직접 더해 읽는다.
 * 읽고 나서 rio_type 값으로 셋 중 하나로 간다 —
 *   4 또는 5 -> 섀시 목록(rio_vg_head)
 *   6 또는 7 -> 확장 상자 목록(rio_lo_head)
 *   그 밖   -> 관심 대상이 아니므로 그 자리에서 버린다.
 * 4/5/6/7 이라는 값이 각각 무엇을 가리키는지는 코드에 근거가 없다 -
 * 확인 못 함. 섀시와 확장 상자로 갈린다는 것만 상류 주석에서 확인된다.
 *
 * 여기서 만든 두 목록을 나중에 combine_wpg_for_chassis() 와
 * combine_wpg_for_expansion() 이 섀시 번호별로 뭉쳐, 슬롯 이름을 지을 때
 * 쓰는 opt_vg_head / opt_lo_head 를 만든다.
 *
 * 실행 컨텍스트: 모듈 초기화(프로세스 컨텍스트), 단일 스레드. __init 이다.
 * 파일 전역 io_mem 과 rio_table_ptr 을 그대로 읽으므로 ibmphp_access_ebda()
 * 밖에서 부르면 안 된다.
 *
 * 에러 경로: 중간에서 -ENOMEM 으로 나가면 그때까지 두 목록에 매단 항목은
 * 그대로 남는다. 호출자가 그 상태로 out 라벨을 거쳐 초기화를 접는다.
 *
 * 호출 체인:
 *   ibmphp_access_ebda() -> [이 함수] -> readb()/readl()/list_add()
 *                                     -> print_lo_info()/print_vg_info()
 */
static int __init ebda_rio_table(void)
{
	u16 offset; /* [한국어] 표 안을 걷는 오프셋 */
	u8 i; /* [한국어] RIO 항목 루프 인덱스 */
	struct rio_detail *rio_detail_ptr; /* [한국어] 항목 하나를 담을 구조체 */

	offset = rio_table_ptr->offset; /* [한국어] ibmphp_access_ebda() 가 기억해 둔 표 몸통의 시작 오프셋 */
	offset += 12 * rio_table_ptr->scal_count; /* [한국어] **확장성 항목을 통째로 건너뛴다.** 항목 하나가 12바이트이므로 개수만큼 곱해 더한다. 이 코드는 확장성 정보를 쓰지 않는다 */

	// we do concern about rio details
	for (i = 0; i < rio_table_ptr->riodev_count; i++) { /* [한국어] RIO 장치 항목을 개수만큼 읽는다 */
		rio_detail_ptr = kzalloc_obj(struct rio_detail); /* [한국어] 항목 하나를 담을 구조체를 잡는다 */
		if (!rio_detail_ptr) /* [한국어] 못 잡으면 */
			return -ENOMEM; /* [한국어] 메모리 부족으로 돌아간다. 앞서 두 목록에 매단 항목은 그대로 남고 호출자가 초기화를 접는다 */
		rio_detail_ptr->rio_node_id = readb(io_mem + offset); /* [한국어] +0 RIO 노드 id */
		rio_detail_ptr->bbar = readl(io_mem + offset + 1); /* [한국어] +1 bbar — 4바이트라 readl 로 읽는다. 이 값을 읽는 곳은 이 대입뿐이다(전수 grep 확인) */
		rio_detail_ptr->rio_type = readb(io_mem + offset + 5); /* [한국어] +5 항목 종류. 아래에서 이 값으로 목록을 가른다 */
		rio_detail_ptr->owner_id = readb(io_mem + offset + 6); /* [한국어] +6 이 항목을 소유한 노드의 id */
		rio_detail_ptr->port0_node_connect = readb(io_mem + offset + 7); /* [한국어] +7 포트 0 이 연결된 노드 */
		rio_detail_ptr->port0_port_connect = readb(io_mem + offset + 8); /* [한국어] +8 포트 0 이 연결된 포트 */
		rio_detail_ptr->port1_node_connect = readb(io_mem + offset + 9); /* [한국어] +9 포트 1 이 연결된 노드 */
		rio_detail_ptr->port1_port_connect = readb(io_mem + offset + 10); /* [한국어] +10 포트 1 이 연결된 포트 */
		rio_detail_ptr->first_slot_num = readb(io_mem + offset + 11); /* [한국어] +11 **이 섀시/상자의 첫 슬롯 번호** — 슬롯 이름을 지을 때 기준이 되는 값이다 */
		rio_detail_ptr->status = readb(io_mem + offset + 12); /* [한국어] +12 상태 */
		rio_detail_ptr->wpindex = readb(io_mem + offset + 13); /* [한국어] +13 wpindex */
		rio_detail_ptr->chassis_num = readb(io_mem + offset + 14); /* [한국어] +14 섀시 번호 — combine_wpg_for_* 가 이 값을 열쇠로 항목을 뭉친다 */
//		debug("rio_node_id: %x\nbbar: %x\nrio_type: %x\nowner_id: %x\nport0_node: %x\nport0_port: %x\nport1_node: %x\nport1_port: %x\nfirst_slot_num: %x\nstatus: %x\n", rio_detail_ptr->rio_node_id, rio_detail_ptr->bbar, rio_detail_ptr->rio_type, rio_detail_ptr->owner_id, rio_detail_ptr->port0_node_connect, rio_detail_ptr->port0_port_connect, rio_detail_ptr->port1_node_connect, rio_detail_ptr->port1_port_connect, rio_detail_ptr->first_slot_num, rio_detail_ptr->status);
		//create linked list of chassis
		if (rio_detail_ptr->rio_type == 4 || rio_detail_ptr->rio_type == 5) /* [한국어] 종류가 4 나 5 면 섀시다. 두 값이 각각 무엇인지는 코드에 근거가 없다 — 확인 못 함 */
			list_add(&rio_detail_ptr->rio_detail_list, &rio_vg_head); /* [한국어] 섀시 목록에 매단다 */
		//create linked list of expansion box
		else if (rio_detail_ptr->rio_type == 6 || rio_detail_ptr->rio_type == 7) /* [한국어] 종류가 6 이나 7 이면 확장 상자다 */
			list_add(&rio_detail_ptr->rio_detail_list, &rio_lo_head); /* [한국어] 확장 상자 목록에 매단다 */
		else
			// not in my concern
			kfree(rio_detail_ptr); /* [한국어] 상류 주석대로 관심 대상이 아니므로 그 자리에서 버린다 */
		offset += 15; /* [한국어] **항목 하나가 15바이트 고정**이므로 그만큼 밀어 다음 항목으로 간다 */
	}
	print_lo_info(); /* [한국어] 읽은 확장 상자 목록을 남긴다 */
	print_vg_info(); /* [한국어] 읽은 섀시 목록을 남긴다 */
	return 0; /* [한국어] 항목을 다 읽었으면 성공 */
}

/*
 * reorganizing linked list of chassis
 */
/* [한국어]
 * search_opt_vg - 섀시 번호로 이미 만들어 둔 opt_rio 항목을 찾는다
 *
 * @chassis_num: 찾을 섀시 번호. RIO 항목의 chassis_num 필드에서 온 값.
 * @return: 같은 섀시 번호를 가진 opt_rio. 없으면 NULL.
 *
 * combine_wpg_for_chassis() 가 RIO 항목을 하나씩 훑으면서 "이 섀시는 이미
 * 봤는가" 를 묻는 데만 쓴다. 없으면 새로 만들고, 있으면 슬롯 번호 구간만
 * 넓히는 식이다.
 *
 * 목록이 짧다는 전제의 선형 탐색이다. 섀시 개수만큼만 도는데, 그 개수는
 * RIO 표에 적힌 섀시 수를 넘지 않는다.
 *
 * 실행 컨텍스트: 모듈 초기화(프로세스 컨텍스트).
 *
 * 호출 체인:  combine_wpg_for_chassis() -> [이 함수] -> list_for_each_entry()
 */
static struct opt_rio *search_opt_vg(u8 chassis_num)
{
	struct opt_rio *ptr; /* [한국어] 목록을 훑을 반복 포인터 */
	list_for_each_entry(ptr, &opt_vg_head, opt_rio_list) { /* [한국어] 뭉쳐 둔 섀시 항목을 하나씩 본다 */
		if (ptr->chassis_num == chassis_num) /* [한국어] 섀시 번호가 같으면 이미 만들어 둔 항목이다 */
			return ptr; /* [한국어] 그것을 돌려준다 */
	}
	return NULL; /* [한국어] 끝까지 못 찾았으면 처음 보는 섀시다 */
}

/* [한국어]
 * combine_wpg_for_chassis - 섀시 RIO 항목들을 섀시 번호별로 하나씩 뭉친다
 *
 * @return: 0. 항목을 할당하지 못하면 -ENOMEM.
 *
 * 바로 위 상류 주석대로 섀시 연결 목록을 재구성한다. rio_vg_head 에는 섀시
 * 하나에 대해 항목이 여럿 있을 수 있는데(RIO 노드마다 하나씩), 슬롯 이름을
 * 지을 때 필요한 것은 "이 섀시가 몇 번 슬롯부터 몇 번 슬롯까지인가" 하나뿐이다.
 * 그래서 섀시 번호를 열쇠로 opt_rio 하나로 접는다.
 *
 * 처음 보는 섀시면 새 opt_rio 를 만들고 first_slot_num 과 middle_num 을 둘 다
 * 그 항목의 first_slot_num 으로 둔다. 이미 있으면 first_slot_num 은 min 으로
 * 낮추고 middle_num 은 max 로 올린다 — 즉 두 필드가 그 섀시가 차지하는 슬롯
 * 번호 구간의 양 끝이 된다. 이름과 달리 middle_num 은 가운데 값이 아니다.
 *
 * [관찰] ebda_rsrc_controller() 가 컨트롤러마다 이 함수를 다시 부른다.
 * opt_vg_head 는 그 사이 비워지지 않으므로 두 번째 호출부터는 이미 만들어진
 * 항목을 다시 찾아 구간만 넓히게 되며, 새로 만드는 일은 일어나지 않는다.
 *
 * 실행 컨텍스트: 모듈 초기화(프로세스 컨텍스트). __init 이다.
 *
 * 호출 체인:
 *   ebda_rsrc_controller() -> [이 함수] -> search_opt_vg() -> print_opt_vg()
 */
static int __init combine_wpg_for_chassis(void)
{
	struct opt_rio *opt_rio_ptr = NULL; /* [한국어] 찾았거나 새로 만든 뭉친 항목 */
	struct rio_detail *rio_detail_ptr = NULL; /* [한국어] RIO 목록을 훑을 반복 포인터 */

	list_for_each_entry(rio_detail_ptr, &rio_vg_head, rio_detail_list) { /* [한국어] 섀시 RIO 항목을 하나씩 본다 */
		opt_rio_ptr = search_opt_vg(rio_detail_ptr->chassis_num); /* [한국어] 이 섀시를 이미 본 적이 있는지 묻는다 */
		if (!opt_rio_ptr) { /* [한국어] 처음 보는 섀시면 */
			opt_rio_ptr = kzalloc_obj(struct opt_rio); /* [한국어] 뭉친 항목을 새로 만든다 */
			if (!opt_rio_ptr) /* [한국어] 못 잡으면 */
				return -ENOMEM; /* [한국어] 메모리 부족으로 돌아간다 */
			opt_rio_ptr->rio_type = rio_detail_ptr->rio_type; /* [한국어] 종류를 베낀다 */
			opt_rio_ptr->chassis_num = rio_detail_ptr->chassis_num; /* [한국어] 섀시 번호를 베낀다 — 이것이 목록의 열쇠다 */
			opt_rio_ptr->first_slot_num = rio_detail_ptr->first_slot_num; /* [한국어] 구간의 아래쪽 끝을 이 항목의 첫 슬롯 번호로 둔다 */
			opt_rio_ptr->middle_num = rio_detail_ptr->first_slot_num; /* [한국어] 구간의 위쪽 끝도 같은 값으로 시작한다 */
			list_add(&opt_rio_ptr->opt_rio_list, &opt_vg_head); /* [한국어] 뭉친 목록에 매단다 */
		} else {
			opt_rio_ptr->first_slot_num = min(opt_rio_ptr->first_slot_num, rio_detail_ptr->first_slot_num); /* [한국어] 아래쪽 끝은 더 작은 쪽으로 낮춘다 */
			opt_rio_ptr->middle_num = max(opt_rio_ptr->middle_num, rio_detail_ptr->first_slot_num); /* [한국어] 위쪽 끝은 더 큰 쪽으로 올린다 — 이름과 달리 middle_num 은 가운데 값이 아니다 */
		}
	}
	print_opt_vg(); /* [한국어] 뭉친 결과를 남긴다 */
	return 0; /* [한국어] 성공으로 돌아간다 */
}

/*
 * reorganizing linked list of expansion box
 */
/* [한국어]
 * search_opt_lo - 섀시 번호로 이미 만들어 둔 opt_rio_lo 항목을 찾는다
 *
 * @chassis_num: 찾을 섀시 번호. RIO 항목의 chassis_num 필드에서 온 값.
 * @return: 같은 섀시 번호를 가진 opt_rio_lo. 없으면 NULL.
 *
 * search_opt_vg() 와 같은 일을 확장 상자 쪽 목록(opt_lo_head)에 대해 한다.
 * 두 목록이 자료형과 list_head 필드 이름만 다를 뿐 구조가 같아 함수가
 * 둘로 갈라져 있다.
 *
 * 실행 컨텍스트: 모듈 초기화(프로세스 컨텍스트).
 *
 * 호출 체인:  combine_wpg_for_expansion() -> [이 함수] -> list_for_each_entry()
 */
static struct opt_rio_lo *search_opt_lo(u8 chassis_num)
{
	struct opt_rio_lo *ptr; /* [한국어] 목록을 훑을 반복 포인터 */
	list_for_each_entry(ptr, &opt_lo_head, opt_rio_lo_list) { /* [한국어] 뭉쳐 둔 확장 상자 항목을 하나씩 본다 */
		if (ptr->chassis_num == chassis_num) /* [한국어] 섀시 번호가 같으면 이미 만들어 둔 항목이다 */
			return ptr; /* [한국어] 그것을 돌려준다 */
	}
	return NULL; /* [한국어] 끝까지 못 찾았으면 처음 보는 상자다 */
}

/* [한국어]
 * combine_wpg_for_expansion - 확장 상자 RIO 항목들을 섀시 번호별로 뭉친다
 *
 * @return: 0. 항목을 할당하지 못하면 -ENOMEM.
 *
 * 바로 위 상류 주석대로 확장 상자 연결 목록을 재구성한다.
 * combine_wpg_for_chassis() 와 같은 접기를 rio_lo_head 에 대해 하며,
 * opt_rio_lo 에만 있는 pack_count 를 하나 더 채운다 — 처음 보는 섀시면 1,
 * 같은 섀시를 다시 만나면 2 로 둔다. 2 를 넘겨 세지 않으므로 개수라기보다
 * "항목이 하나였나 둘 이상이었나" 를 나타내는 표시에 가깝다. 다만 이 파일과
 * 드라이버 안에서 pack_count 를 읽는 곳은 없다(전수 grep 확인).
 *
 * 짝인 combine_wpg_for_chassis() 와 달리 __init 가 붙어 있지 않고, 끝에서
 * 목록을 찍는 출력 함수도 부르지 않는다.
 *
 * 실행 컨텍스트: 모듈 초기화(프로세스 컨텍스트).
 *
 * 호출 체인:  ebda_rsrc_controller() -> [이 함수] -> search_opt_lo()
 */
static int combine_wpg_for_expansion(void)
{
	struct opt_rio_lo *opt_rio_lo_ptr = NULL; /* [한국어] 찾았거나 새로 만든 뭉친 항목 */
	struct rio_detail *rio_detail_ptr = NULL; /* [한국어] RIO 목록을 훑을 반복 포인터 */

	list_for_each_entry(rio_detail_ptr, &rio_lo_head, rio_detail_list) { /* [한국어] 확장 상자 RIO 항목을 하나씩 본다 */
		opt_rio_lo_ptr = search_opt_lo(rio_detail_ptr->chassis_num); /* [한국어] 이 섀시를 이미 본 적이 있는지 묻는다 */
		if (!opt_rio_lo_ptr) { /* [한국어] 처음 보는 섀시면 */
			opt_rio_lo_ptr = kzalloc_obj(struct opt_rio_lo); /* [한국어] 뭉친 항목을 새로 만든다 */
			if (!opt_rio_lo_ptr) /* [한국어] 못 잡으면 */
				return -ENOMEM; /* [한국어] 메모리 부족으로 돌아간다 */
			opt_rio_lo_ptr->rio_type = rio_detail_ptr->rio_type; /* [한국어] 종류를 베낀다 */
			opt_rio_lo_ptr->chassis_num = rio_detail_ptr->chassis_num; /* [한국어] 섀시 번호를 베낀다 — 이것이 목록의 열쇠다 */
			opt_rio_lo_ptr->first_slot_num = rio_detail_ptr->first_slot_num; /* [한국어] 구간의 아래쪽 끝을 이 항목의 첫 슬롯 번호로 둔다 */
			opt_rio_lo_ptr->middle_num = rio_detail_ptr->first_slot_num; /* [한국어] 구간의 위쪽 끝도 같은 값으로 시작한다 */
			opt_rio_lo_ptr->pack_count = 1; /* [한국어] 섀시 쪽 opt_rio 에는 없는 필드. 항목이 하나뿐이라는 표시로 1 을 둔다 */

			list_add(&opt_rio_lo_ptr->opt_rio_lo_list, &opt_lo_head); /* [한국어] 뭉친 목록에 매단다 */
		} else {
			opt_rio_lo_ptr->first_slot_num = min(opt_rio_lo_ptr->first_slot_num, rio_detail_ptr->first_slot_num); /* [한국어] 아래쪽 끝은 더 작은 쪽으로 낮춘다 */
			opt_rio_lo_ptr->middle_num = max(opt_rio_lo_ptr->middle_num, rio_detail_ptr->first_slot_num); /* [한국어] 위쪽 끝은 더 큰 쪽으로 올린다 */
			opt_rio_lo_ptr->pack_count = 2; /* [한국어] 항목이 둘 이상이라는 표시로 2 를 둔다. 2 를 넘겨 세지 않으며, 이 값을 읽는 곳은 드라이버 안에 없다(전수 grep 확인) */
		}
	}
	return 0; /* [한국어] 짝인 combine_wpg_for_chassis() 와 달리 목록을 찍지 않고 곧바로 돌아간다 */
}


/* Since we don't know the max slot number per each chassis, hence go
 * through the list of all chassis to find out the range
 * Arguments: slot_num, 1st slot number of the chassis we think we are on,
 * var (0 = chassis, 1 = expansion box)
 */
/* [한국어]
 * first_slot_num - 이 슬롯 번호가 주어진 섀시/상자보다 뒤쪽 것인지 가른다
 *
 * @slot_num:   판별할 물리 슬롯 번호.
 * @first_slot: "우리가 지금 여기라고 보는" 섀시 또는 상자의 첫 슬롯 번호.
 * @var:        어느 목록을 볼지 - 0 이면 섀시(opt_vg_head), 1 이면 확장
 *              상자(opt_lo_head). 바로 위 상류 주석이 그렇게 밝힌다.
 * @return: 0 이면 first_slot 이 가리키는 쪽이 맞다. -ENODEV 면 slot_num 이
 *          사실 더 뒤쪽 섀시/상자에 속한다는 뜻이다.
 *
 * 상류 주석이 밝히듯 섀시마다 슬롯이 몇 개인지 알 수 없기 때문에 필요한
 * 함수다. 각 섀시는 첫 슬롯 번호만 알려져 있고 끝 번호는 없다. 그래서
 * "내가 후보로 잡은 섀시보다 더 뒤에서 시작하면서, 그 시작점이 이미
 * slot_num 이하인 섀시" 가 목록에 하나라도 있으면 후보가 틀렸다고 본다 —
 * 그런 섀시가 있다는 것은 slot_num 이 후보 섀시의 범위를 넘어 그 다음
 * 섀시로 들어갔다는 뜻이기 때문이다.
 *
 * 반환값이 오류 코드처럼 생겼지만 실제로는 참/거짓으로 쓰인다.
 * find_rxe_num()/find_chassis_num() 이 !first_slot_num(...) 꼴로 부른다.
 *
 * 실행 컨텍스트: 모듈 초기화(프로세스 컨텍스트).
 *
 * 호출 체인:  find_rxe_num() / find_chassis_num() -> [이 함수]
 */
static int first_slot_num(u8 slot_num, u8 first_slot, u8 var)
{
	struct opt_rio *opt_vg_ptr = NULL; /* [한국어] 섀시 목록을 훑을 반복 포인터 */
	struct opt_rio_lo *opt_lo_ptr = NULL; /* [한국어] 확장 상자 목록을 훑을 반복 포인터 */
	int rc = 0; /* [한국어] 판정 결과. 0 이면 후보가 맞다는 뜻이다 */

	if (!var) { /* [한국어] var 가 0 이면 섀시 목록을 본다 */
		list_for_each_entry(opt_vg_ptr, &opt_vg_head, opt_rio_list) { /* [한국어] 뭉쳐 둔 섀시를 하나씩 본다 */
			if ((first_slot < opt_vg_ptr->first_slot_num) && (slot_num >= opt_vg_ptr->first_slot_num)) { /* [한국어] **후보보다 뒤에서 시작하면서 그 시작점이 이미 slot_num 이하인 섀시**가 있으면, slot_num 은 후보의 범위를 넘어 그 섀시로 들어간 것이다. 섀시의 끝 번호가 표에 없어 이렇게 간접적으로 판정한다 */
				rc = -ENODEV; /* [한국어] 후보가 틀렸다고 표시하고 */
				break; /* [한국어] 더 볼 것 없이 멈춘다 */
			}
		}
	} else {
		list_for_each_entry(opt_lo_ptr, &opt_lo_head, opt_rio_lo_list) { /* [한국어] 뭉쳐 둔 확장 상자를 하나씩 본다 */
			if ((first_slot < opt_lo_ptr->first_slot_num) && (slot_num >= opt_lo_ptr->first_slot_num)) { /* [한국어] 섀시 쪽과 똑같은 판정을 상자 목록에 대해 한다 */
				rc = -ENODEV; /* [한국어] 후보가 틀렸다고 표시하고 */
				break; /* [한국어] 더 볼 것 없이 멈춘다 */
			}
		}
	}
	return rc; /* [한국어] 0 이면 후보가 맞다. 호출자가 !first_slot_num(...) 꼴로 참/거짓처럼 쓴다 */
}

/* [한국어]
 * find_rxe_num - 이 슬롯 번호가 속한 확장 상자(RXE)를 찾는다
 *
 * @slot_num: 물리 슬롯 번호.
 * @return: 그 슬롯을 담고 있는 opt_rio_lo. 못 찾으면 NULL.
 *
 * 확장 상자 목록을 훑으며 두 조건을 함께 본다 - 슬롯 번호가 그 상자의 첫
 * 슬롯 번호 이상일 것, 그리고 first_slot_num() 이 "더 뒤쪽 상자가 있다" 고
 * 하지 않을 것. 두 조건이 함께여야 하는 이유는 상자의 끝 슬롯 번호가
 * 표에 없기 때문이다 - 시작점만으로는 구간을 정할 수 없다.
 *
 * RXE 는 상류 코드가 create_file_name() 에서 슬롯 이름의 접두사로 쓰는
 * 말이다("rxe"). 그 약자가 무엇을 줄인 것인지는 코드에 근거가 없다 -
 * 확인 못 함.
 *
 * 실행 컨텍스트: 모듈 초기화(프로세스 컨텍스트).
 *
 * 호출 체인:  create_file_name() -> [이 함수] -> first_slot_num()
 */
static struct opt_rio_lo *find_rxe_num(u8 slot_num)
{
	struct opt_rio_lo *opt_lo_ptr; /* [한국어] 목록을 훑을 반복 포인터 */

	list_for_each_entry(opt_lo_ptr, &opt_lo_head, opt_rio_lo_list) { /* [한국어] 뭉쳐 둔 확장 상자를 하나씩 본다 */
		//check to see if this slot_num belongs to expansion box
		if ((slot_num >= opt_lo_ptr->first_slot_num) && (!first_slot_num(slot_num, opt_lo_ptr->first_slot_num, 1))) /* [한국어] 슬롯 번호가 이 상자의 첫 슬롯 이상이고, 더 뒤쪽 상자가 없어야 한다. 두 조건이 함께여야 하는 이유는 상자의 끝 슬롯 번호가 표에 없기 때문이다 */
			return opt_lo_ptr; /* [한국어] 조건이 맞으면 이 상자를 돌려준다 */
	}
	return NULL; /* [한국어] 끝까지 못 찾았으면 이 슬롯은 어느 확장 상자에도 속하지 않는다 */
}

/* [한국어]
 * find_chassis_num - 이 슬롯 번호가 속한 섀시를 찾는다
 *
 * @slot_num: 물리 슬롯 번호.
 * @return: 그 슬롯을 담고 있는 opt_rio. 못 찾으면 NULL.
 *
 * find_rxe_num() 과 같은 판별을 섀시 목록(opt_vg_head)에 대해 한다.
 * first_slot_num() 에 넘기는 마지막 인자만 0 으로 달라진다.
 *
 * create_file_name() 은 이 함수와 find_rxe_num() 을 둘 다 부른 뒤, 둘 다
 * 찾았으면 슬롯 번호에 더 가까운 쪽(첫 슬롯 번호와의 차이가 작은 쪽)을
 * 고른다 - 섀시 안에 확장 상자가 들어 있는 구성을 그렇게 가른다.
 *
 * 실행 컨텍스트: 모듈 초기화(프로세스 컨텍스트).
 *
 * 호출 체인:  create_file_name() -> [이 함수] -> first_slot_num()
 */
static struct opt_rio *find_chassis_num(u8 slot_num)
{
	struct opt_rio *opt_vg_ptr; /* [한국어] 목록을 훑을 반복 포인터 */

	list_for_each_entry(opt_vg_ptr, &opt_vg_head, opt_rio_list) { /* [한국어] 뭉쳐 둔 섀시를 하나씩 본다 */
		//check to see if this slot_num belongs to chassis
		if ((slot_num >= opt_vg_ptr->first_slot_num) && (!first_slot_num(slot_num, opt_vg_ptr->first_slot_num, 0))) /* [한국어] find_rxe_num() 과 같은 판정. 마지막 인자만 0 으로 달라 섀시 목록을 보게 한다 */
			return opt_vg_ptr; /* [한국어] 조건이 맞으면 이 섀시를 돌려준다 */
	}
	return NULL; /* [한국어] 끝까지 못 찾았으면 이 슬롯은 어느 섀시에도 속하지 않는다 */
}

/* This routine will find out how many slots are in the chassis, so that
 * the slot numbers for rxe100 would start from 1, and not from 7, or 6 etc
 */
/* [한국어]
 * calculate_first_slot - RIO 표가 없을 때 확장 상자의 첫 슬롯 번호를 어림한다
 *
 * @slot_num: 이름을 지으려는 슬롯의 물리 번호.
 * @return: 그 슬롯이 속한 상자의 첫 슬롯 번호로 볼 값(최소 1).
 *
 * 바로 위 상류 주석이 목적을 밝힌다 - rxe100 의 슬롯 번호가 7 이나 6 이
 * 아니라 1 부터 시작하게 만들려는 것이다. 물리 슬롯 번호는 시스템 전체에서
 * 통짜로 매겨지므로, 상자 안에서의 번호를 얻으려면 그 앞까지의 슬롯 수를
 * 빼야 한다.
 *
 * 이미 등록된 슬롯들을 훑으며 "확장 상자형이 아니고(ctlr_type != 4),
 * 담당 슬롯 번호의 끝이 지금까지 본 것보다 크며, 그 끝이 아직 slot_num
 * 보다 앞인" 컨트롤러의 마지막 슬롯 번호를 찾는다. 그 다음 번호가 상자의
 * 첫 슬롯이 된다.
 *
 * 이 어림은 RIO 표에서 섀시도 상자도 못 찾았을 때만 쓰인다 -
 * create_file_name() 이 flag 가 서지 않은 경우에만 부른다.
 *
 * 실행 컨텍스트: 모듈 초기화(프로세스 컨텍스트). ibmphp_slot_head 를 읽는데,
 * 그 목록은 같은 초기화 흐름의 ebda_rsrc_controller() 가 채우는 중이다.
 *
 * 호출 체인:  create_file_name() -> [이 함수]
 */
static u8 calculate_first_slot(u8 slot_num)
{
	u8 first_slot = 1; /* [한국어] 못 찾았을 때의 기본값. 아래에서 1 을 더하므로 최종 결과는 최소 2 가 된다 */
	struct slot *slot_cur; /* [한국어] 슬롯 목록을 훑을 반복 포인터 */

	list_for_each_entry(slot_cur, &ibmphp_slot_head, ibm_slot_list) { /* [한국어] 이미 등록된 슬롯을 하나씩 본다 */
		if (slot_cur->ctrl) { /* [한국어] 컨트롤러가 이어져 있는 슬롯만 본다 */
			if ((slot_cur->ctrl->ctlr_type != 4) && (slot_cur->ctrl->ending_slot_num > first_slot) && (slot_num > slot_cur->ctrl->ending_slot_num)) /* [한국어] 확장 상자형(type 4)이 아니면서, 담당 슬롯의 끝 번호가 지금까지 본 것보다 크고, 그 끝이 아직 찾는 슬롯보다 앞인 컨트롤러를 고른다 */
				first_slot = slot_cur->ctrl->ending_slot_num; /* [한국어] 그 컨트롤러의 마지막 슬롯 번호를 기억한다 */
		}
	}
	return first_slot + 1; /* [한국어] 그 다음 번호가 상자의 첫 슬롯이 된다 */

}

#define SLOT_NAME_SIZE 30 /* [한국어] 슬롯 이름 문자열의 최대 길이. "chassis"(7) + 번호 + "slot"(4) + 번호를 담기에 넉넉한 값이다 */

/* [한국어]
 * create_file_name - 슬롯의 sysfs 이름을 짓는다("chassis1slot2" 또는 "rxe2slot1")
 *
 * @slot_cur: 이름을 지으려는 슬롯. NULL 이면 오류를 남기고 NULL 을 돌려준다.
 * @return: 지어진 이름이 담긴 문자열. RIO 표가 3판인데 섀시도 상자도
 *          못 찾았으면 NULL.
 *
 * 사용자에게 보이는 이름을 만드는 곳이다. 물리 슬롯 번호는 시스템 전체에서
 * 통짜로 매겨지므로 그대로 쓰면 "3번 상자의 1번 슬롯" 같은 물리적 위치가
 * 드러나지 않는다. 그래서 (섀시인가 상자인가, 몇 번째 상자인가, 그 안에서
 * 몇 번째 슬롯인가) 셋을 구해 조합한다.
 *
 * 셋을 정하는 순서:
 *   1. RIO 표가 있고 그 판이 3 이면 find_chassis_num() 과 find_rxe_num() 을
 *      둘 다 부른다.
 *   2. 둘 다 찾았으면 첫 슬롯 번호가 슬롯에 더 가까운 쪽을 고른다 -
 *      섀시 안에 상자가 들어 있는 구성에서 안쪽(상자)을 고르게 된다.
 *   3. 하나만 찾았으면 그쪽을 쓴다.
 *   4. 둘 다 못 찾았는데 RIO 표는 3판이면, 표가 옳다는 전제 아래 이름을
 *      지을 수 없다고 보고 NULL 을 돌려준다.
 *   5. 그 밖(RIO 표가 없거나 판이 3 이 아님)이면 컨트롤러 종류로 가른다 -
 *      type 4 면 상자로 보고 calculate_first_slot() 으로 시작 번호를
 *      어림하고, 아니면 섀시로 보고 1 번부터 센다.
 * 마지막에 (슬롯 번호 - 첫 슬롯 번호 + 1)로 상자 안에서의 순번을 만든다.
 *
 * [관찰] 돌려주는 것은 파일 정적 배열 str 의 주소다. 호출자인
 * ebda_rsrc_controller() 가 곧바로 snprintf 로 베끼므로 지금 구조에서는
 * 문제가 없지만 재진입은 불가능하다. 또 4번 갈래에서 NULL 을 돌려주면
 * 호출자가 그것을 그대로 snprintf 의 %s 에 넘긴다.
 *
 * 실행 컨텍스트: 모듈 초기화(프로세스 컨텍스트), 단일 스레드.
 *
 * 호출 체인:
 *   ebda_rsrc_controller() -> [이 함수] -> find_chassis_num()/find_rxe_num()
 *                                       -> calculate_first_slot()
 */
static char *create_file_name(struct slot *slot_cur)
{
	struct opt_rio *opt_vg_ptr = NULL; /* [한국어] find_chassis_num() 이 찾아 줄 섀시 */
	struct opt_rio_lo *opt_lo_ptr = NULL; /* [한국어] find_rxe_num() 이 찾아 줄 확장 상자 */
	static char str[SLOT_NAME_SIZE]; /* [한국어] **파일 정적 배열**이라 이 함수는 재진입할 수 없다. 호출자가 곧바로 snprintf 로 베끼기 때문에 지금 구조에서는 드러나지 않는다 */
	int which = 0;	/* rxe = 1, chassis = 0 */ /* [한국어] 섀시인지 상자인지 — 아래 sprintf 에서 접두사를 고른다 */
	u8 number = 1;	/* either chassis or rxe # */ /* [한국어] 몇 번째 섀시 또는 상자인지 */
	u8 first_slot = 1; /* [한국어] 그 안에서 슬롯 번호가 몇 번부터 시작하는지 */
	u8 slot_num; /* [한국어] 이름을 지으려는 슬롯의 물리 번호 */
	u8 flag = 0; /* [한국어] 섀시나 상자를 찾았는지 표시. 못 찾았을 때만 아래의 어림 경로로 간다 */

	if (!slot_cur) { /* [한국어] 슬롯을 안 넘겼으면 */
		err("Structure passed is empty\n"); /* [한국어] 그 사실을 알리고 */
		return NULL; /* [한국어] 이름을 지을 수 없다고 알린다 */
	}

	slot_num = slot_cur->number; /* [한국어] 물리 슬롯 번호를 꺼낸다 */

	memset(str, 0, sizeof(str)); /* [한국어] 앞선 호출이 남긴 문자열이 섞이지 않도록 정적 배열을 비운다 */

	if (rio_table_ptr) { /* [한국어] RIO 표를 읽어 두었고 */
		if (rio_table_ptr->ver_num == 3) { /* [한국어] 그 판이 3 이면 — 이 코드가 아는 유일한 판이다 */
			opt_vg_ptr = find_chassis_num(slot_num); /* [한국어] 이 슬롯이 속한 섀시를 찾는다 */
			opt_lo_ptr = find_rxe_num(slot_num); /* [한국어] 이 슬롯이 속한 확장 상자도 찾는다 */
		}
	}
	if (opt_vg_ptr) { /* [한국어] 섀시를 찾았으면 */
		if (opt_lo_ptr) { /* [한국어] 상자도 함께 찾았으면 둘 중 하나를 골라야 한다 */
			if ((slot_num - opt_vg_ptr->first_slot_num) > (slot_num - opt_lo_ptr->first_slot_num)) { /* [한국어] **첫 슬롯 번호가 슬롯에 더 가까운 쪽을 고른다.** 섀시 안에 상자가 들어 있는 구성에서 안쪽(상자)이 뽑히게 된다 */
				number = opt_lo_ptr->chassis_num; /* [한국어] 상자 번호를 쓰고 */
				first_slot = opt_lo_ptr->first_slot_num; /* [한국어] 상자의 첫 슬롯을 기준으로 삼고 */
				which = 1;	/* it is RXE */ /* [한국어] 상자로 표시한다 */
			} else {
				first_slot = opt_vg_ptr->first_slot_num; /* [한국어] 그렇지 않으면 섀시의 첫 슬롯을 기준으로 삼고 */
				number = opt_vg_ptr->chassis_num; /* [한국어] 섀시 번호를 쓰고 */
				which = 0; /* [한국어] 섀시로 표시한다 */
			}
		} else {
			first_slot = opt_vg_ptr->first_slot_num; /* [한국어] 섀시의 첫 슬롯을 기준으로 삼고 */
			number = opt_vg_ptr->chassis_num; /* [한국어] 섀시 번호를 쓰고 */
			which = 0; /* [한국어] 섀시로 표시한다 */
		}
		++flag; /* [한국어] 찾았다고 표시한다 — 아래 어림 경로로 가지 않게 한다 */
	} else if (opt_lo_ptr) { /* [한국어] 섀시는 못 찾고 상자만 찾았으면 */
		number = opt_lo_ptr->chassis_num; /* [한국어] 상자 번호를 쓰고 */
		first_slot = opt_lo_ptr->first_slot_num; /* [한국어] 상자의 첫 슬롯을 기준으로 삼고 */
		which = 1; /* [한국어] 상자로 표시한 뒤 */
		++flag; /* [한국어] 찾았다고 표시한다 */
	} else if (rio_table_ptr) { /* [한국어] 둘 다 못 찾았는데 RIO 표는 있으면 */
		if (rio_table_ptr->ver_num == 3) { /* [한국어] 그 판이 3 이면 표가 옳다는 전제이므로 */
			/* if both NULL and we DO have correct RIO table in BIOS */
			return NULL; /* [한국어] 상류 주석대로 이름을 지을 수 없다고 보고 NULL 을 돌려준다 */
		}
	}
	if (!flag) { /* [한국어] 섀시도 상자도 못 찾았으면(또는 RIO 표가 없거나 판이 3 이 아니면) 어림한다 */
		if (slot_cur->ctrl->ctlr_type == 4) { /* [한국어] 컨트롤러가 확장 상자형이면 */
			first_slot = calculate_first_slot(slot_num); /* [한국어] 앞선 컨트롤러들의 슬롯 수로 시작 번호를 어림하고 */
			which = 1; /* [한국어] 상자로 표시한다 */
		} else {
			which = 0; /* [한국어] 섀시로 보고 1 번부터 센다 */
		}
	}

	sprintf(str, "%s%dslot%d", /* [한국어] "chassis1slot2" 또는 "rxe2slot1" 꼴로 짓는다 */
		which == 0 ? "chassis" : "rxe", /* [한국어] 접두사를 고른다 */
		number, slot_num - first_slot + 1); /* [한국어] 섀시/상자 번호와, 그 안에서의 슬롯 순번(1 부터)을 넣는다 */
	return str; /* [한국어] 정적 배열의 주소를 돌려준다 */
}

/* [한국어]
 * fillslotinfo - 슬롯을 등록하기 전에 하드웨어에서 현재 상태를 읽어 채운다
 *
 * @hotplug_slot: 핫플러그 코어 쪽 슬롯 구조체. 여기서는 to_slot() 으로
 *                감싸고 있는 struct slot 을 되찾는 데만 쓴다.
 * @return: ibmphp_hpc_readslot() 의 반환값을 그대로 돌려준다. 0 이면 성공.
 *
 * EBDA 표는 슬롯이 어디에 몇 개 있는지만 알려 줄 뿐, 지금 카드가 꽂혀
 * 있는지·전원이 들어와 있는지 같은 현재 상태는 알려 주지 않는다. 그것은
 * 핫플러그 컨트롤러에게 물어야 하므로, 슬롯을 코어에 등록하기 직전에 한 번
 * 읽어 둔다.
 *
 * READ_ALLSTAT 은 슬롯의 상태 전부를 한 번에 읽으라는 명령이다. 세 번째
 * 인자가 NULL 인 것은 읽은 값을 슬롯 구조체 자신에 채우라는 뜻이다.
 *
 * 실행 컨텍스트: 모듈 초기화(프로세스 컨텍스트). 하위의
 * ibmphp_hpc_readslot() 은 컨트롤러와 실제로 주고받으므로 시간이 걸린다.
 *
 * 호출 체인:
 *   ebda_rsrc_controller() -> [이 함수] -> ibmphp_hpc_readslot() [ibmphp_hpc.c]
 */
static int fillslotinfo(struct hotplug_slot *hotplug_slot)
{
	struct slot *slot; /* [한국어] 감싸고 있는 이 드라이버의 슬롯 구조체 */
	int rc = 0; /* [한국어] 하위 호출의 결과를 담을 자리 */

	slot = to_slot(hotplug_slot); /* [한국어] 핫플러그 코어 쪽 구조체에서 이 드라이버의 슬롯 구조체를 되찾는다. ibmphp.h:743 의 container_of 래퍼다 */
	rc = ibmphp_hpc_readslot(slot, READ_ALLSTAT, NULL); /* [한국어] **컨트롤러에게 이 슬롯의 현재 상태를 묻는다.** READ_ALLSTAT 은 상태 전부를 한 번에 읽으라는 명령이고, 세 번째 인자 NULL 은 읽은 값을 슬롯 구조체 자신에 채우라는 뜻이다 */
	return rc; /* [한국어] 결과를 그대로 돌려준다 */
}

static struct pci_driver ibmphp_driver; /* [한국어] 아래 정의를 ibmphp_free_ebda_hpc_queue() 가 먼저 쓰므로 미리 선언한다 */

/*
 * map info (ctlr-id, slot count, slot#.. bus count, bus#, ctlr type...) of
 * each hpc from physical address to a list of hot plug controllers based on
 * hpc descriptors.
 */
/* [한국어]
 * ebda_rsrc_controller - 컨트롤러·슬롯·버스 기술자를 읽고 슬롯을 코어에 등록한다
 *
 * @return: 0 이면 성공. 메모리 부족이면 -ENOMEM, 표가 이상하거나 I/O 영역을
 *          못 잡으면 -ENODEV, 하위 호출이 실패하면 그 코드를 그대로 돌려준다.
 *
 * 바로 위 상류 주석대로 각 핫플러그 컨트롤러의 정보(컨트롤러 id, 슬롯 수,
 * 슬롯 번호들, 버스 수, 버스 번호들, 컨트롤러 종류)를 물리 주소에서 읽어
 * 컨트롤러 목록으로 만든다. 이 파일에서 가장 큰 함수이며 하는 일이 넷이다.
 *
 *   1. **컨트롤러 기술자 걷기.** hpc_list_ptr->phys_addr 에서 시작해
 *      컨트롤러 하나씩 읽는다. 슬롯 정보가 **필드별 평행 배열**로 놓여
 *      있어(슬롯 번호 배열 전체, 그 다음 버스 번호 배열 전체, ...) 슬롯 i 의
 *      네 값이 slot_num 바이트씩 떨어져 있다. 그래서 addr_slot 에
 *      slot_num 의 배수를 더해 읽는다.
 *   2. **bus_info 목록 만들기.** 슬롯을 읽으면서 그 슬롯이 붙은 버스가
 *      이미 목록에 있는지 보고, 없으면 만들고 있으면 슬롯 번호 구간을
 *      넓힌다. 이 목록이 나중에 ibmphp_pci.c 의 find_sec_number() 가
 *      2차 버스 번호를 고를 때 근거가 된다.
 *   3. **컨트롤러 종류별 접근 정보 읽기.** ctlr_type 에 따라 union u 의
 *      어느 갈래를 채울지와 addr 를 몇 바이트 밀지가 달라진다. ISA 형은
 *      여기서 request_region 까지 잡는다.
 *   4. **슬롯 등록.** 컨트롤러가 담당하는 슬롯마다 struct slot 을 만들어
 *      능력 비트를 속도·모드 값으로 풀고, bus_info 와 컨트롤러를 이어
 *      매단 뒤, 현재 상태를 읽고(fillslotinfo) 장치 번호를 채운다
 *      (ibmphp_init_devno). 마지막으로 ibmphp_slot_head 에 매단다.
 *      모든 컨트롤러를 처리한 뒤 따로 한 번 더 돌면서 이름을 지어
 *      pci_hp_register() 로 코어에 등록한다.
 *
 * 등록을 두 번의 루프로 나눈 이유는 이름 짓기가 슬롯 목록 전체를 봐야 하기
 * 때문이다 - calculate_first_slot() 이 ibmphp_slot_head 를 훑는다.
 *
 * 실행 컨텍스트: 모듈 초기화(프로세스 컨텍스트), 단일 스레드. __init 이다.
 * 파일 전역 io_mem 과 hpc_list_ptr 을 그대로 읽으므로 ibmphp_access_ebda()
 * 밖에서 부르면 안 된다.
 *
 * 에러 경로: 라벨이 둘이다. error 는 방금 만든 tmp_slot 을 버린 뒤
 * error_no_slot 으로 떨어지고, error_no_slot 은 지금 만들던 컨트롤러를
 * free_ebda_hpc() 로 버린다. 앞서 처리를 마친 컨트롤러들과 이미
 * ibmphp_slot_head 에 매단 슬롯들은 여기서 되돌리지 않는다 - 그 정리는
 * 호출자를 거쳐 ibmphp_core.c 의 해제 경로가 맡는다.
 *
 * 호출 체인:
 *   ibmphp_access_ebda() -> [이 함수] -> alloc_ebda_hpc()
 *                                     -> ibmphp_find_same_bus_num()
 *                                     -> combine_wpg_for_chassis()/_expansion()
 *                                     -> fillslotinfo() -> ibmphp_hpc_readslot()
 *                                     -> ibmphp_init_devno() [ibmphp_core.c]
 *                                     -> create_file_name() -> pci_hp_register()
 */
static int __init ebda_rsrc_controller(void)
{
	u16 addr, addr_slot, addr_bus; /* [한국어] addr 는 컨트롤러 기술자를 걷는 위치, addr_slot 은 슬롯 배열의 시작, addr_bus 는 버스 배열의 시작 */
	u8 ctlr_id, temp, bus_index; /* [한국어] ctlr_id 는 컨트롤러 id, temp 는 컨트롤러 종류를 잠시 담는 자리, bus_index 는 컨트롤러 안에서 버스에 매기는 상대 번호 */
	u16 ctlr, slot, bus; /* [한국어] 컨트롤러·슬롯·버스 루프 인덱스 */
	u16 slot_num, bus_num, index; /* [한국어] slot_num 은 이 컨트롤러의 슬롯 개수, bus_num 은 버스 개수, index 는 슬롯 등록 루프의 인덱스 */
	struct controller *hpc_ptr; /* [한국어] 만들고 있는 컨트롤러 */
	struct ebda_hpc_bus *bus_ptr; /* [한국어] 슬롯 배열을 채울 때 쓰는 걷기 포인터 */
	struct ebda_hpc_slot *slot_ptr; /* [한국어] 버스 배열을 채울 때 쓰는 걷기 포인터 */
	struct bus_info *bus_info_ptr1, *bus_info_ptr2; /* [한국어] bus_info_ptr1 은 새로 만든 것, bus_info_ptr2 는 목록에서 찾은 것 */
	int rc; /* [한국어] 하위 호출 반환값 임시 보관 */
	struct slot *tmp_slot; /* [한국어] 만들고 있는 슬롯 */
	char name[SLOT_NAME_SIZE]; /* [한국어] 슬롯의 sysfs 이름을 담을 버퍼 */

	addr = hpc_list_ptr->phys_addr; /* [한국어] ibmphp_access_ebda() 가 기억해 둔 컨트롤러 기술자 배열의 시작에서 출발한다 */
	for (ctlr = 0; ctlr < hpc_list_ptr->num_ctlrs; ctlr++) { /* [한국어] 표에 적힌 개수만큼 컨트롤러를 읽는다 */
		bus_index = 1; /* [한국어] 컨트롤러가 바뀔 때마다 버스 상대 번호를 1 부터 다시 매긴다 */
		ctlr_id = readb(io_mem + addr); /* [한국어] +0 컨트롤러 id */
		addr += 1; /* [한국어] 읽은 만큼 민다 */
		slot_num = readb(io_mem + addr); /* [한국어] +1 이 컨트롤러가 담당하는 슬롯 개수 */

		addr += 1; /* [한국어] 읽은 만큼 민다 */
		addr_slot = addr;	/* offset of slot structure */ /* [한국어] 여기부터가 슬롯 배열이다 */
		addr += (slot_num * 4); /* [한국어] **슬롯 정보가 필드별 평행 배열 넷으로 놓여 있으므로** 슬롯 개수의 4배만큼을 통째로 건너뛴다 */

		bus_num = readb(io_mem + addr); /* [한국어] 그 다음 바이트가 이 컨트롤러가 담당하는 버스 개수 */

		addr += 1; /* [한국어] 읽은 만큼 민다 */
		addr_bus = addr;	/* offset of bus */ /* [한국어] 여기부터가 버스 배열이다 */
		addr += (bus_num * 9);	/* offset of ctlr_type */ /* [한국어] 버스 배열은 버스당 9바이트를 차지한다 — 버스 번호 1바이트가 앞쪽에 모여 있고 속도 능력 8바이트가 뒤쪽에 모여 있다 */
		temp = readb(io_mem + addr); /* [한국어] 그 다음 바이트가 컨트롤러 종류다. 아래에서 hpc_ptr 에 옮겨 담는다 */

		addr += 1; /* [한국어] 읽은 만큼 민다 — 이제 addr 가 종류별 접근 정보를 가리킨다 */
		/* init hpc structure */
		hpc_ptr = alloc_ebda_hpc(slot_num, bus_num); /* [한국어] 컨트롤러와 그에 딸린 슬롯·버스 배열을 한꺼번에 잡는다 */
		if (!hpc_ptr) { /* [한국어] 못 잡으면 */
			return -ENOMEM; /* [한국어] 메모리 부족으로 돌아간다. 앞서 만든 컨트롤러들은 여기서 되돌리지 않는다 */
		}
		hpc_ptr->ctlr_id = ctlr_id; /* [한국어] 표에서 읽은 컨트롤러 id */
		hpc_ptr->ctlr_relative_id = ctlr; /* [한국어] 몇 번째 컨트롤러인지 — 루프 인덱스를 그대로 쓴다 */
		hpc_ptr->slot_count = slot_num; /* [한국어] 담당 슬롯 개수 */
		hpc_ptr->bus_count = bus_num; /* [한국어] 담당 버스 개수 */
		debug("now enter ctlr data structure ---\n"); /* [한국어] 읽은 내용을 남긴다 */
		debug("ctlr id: %x\n", ctlr_id); /* [한국어] 컨트롤러 id */
		debug("ctlr_relative_id: %x\n", hpc_ptr->ctlr_relative_id); /* [한국어] 상대 번호 */
		debug("count of slots controlled by this ctlr: %x\n", slot_num); /* [한국어] 슬롯 개수 */
		debug("count of buses controlled by this ctlr: %x\n", bus_num); /* [한국어] 버스 개수 */

		/* init slot structure, fetch slot, bus, cap... */
		slot_ptr = hpc_ptr->slots; /* [한국어] 슬롯 배열의 첫 칸부터 채운다 */
		for (slot = 0; slot < slot_num; slot++) { /* [한국어] 슬롯 개수만큼 반복한다 */
			slot_ptr->slot_num = readb(io_mem + addr_slot); /* [한국어] **필드별 평행 배열**의 첫 배열 — 슬롯 번호 */
			slot_ptr->slot_bus_num = readb(io_mem + addr_slot + slot_num); /* [한국어] 둘째 배열 — 그 슬롯이 붙어 있는 PCI 버스 번호. 한 배열이 slot_num 바이트이므로 그만큼 떨어져 있다 */
			slot_ptr->ctl_index = readb(io_mem + addr_slot + 2*slot_num); /* [한국어] 셋째 배열 — 컨트롤러 레지스터를 짚을 때 쓰는 인덱스 */
			slot_ptr->slot_cap = readb(io_mem + addr_slot + 3*slot_num); /* [한국어] 넷째 배열 — 슬롯 능력 비트 */

			// create bus_info lined list --- if only one slot per bus: slot_min = slot_max

			bus_info_ptr2 = ibmphp_find_same_bus_num(slot_ptr->slot_bus_num); /* [한국어] 이 슬롯이 붙은 버스가 이미 목록에 있는지 본다 */
			if (!bus_info_ptr2) { /* [한국어] 없으면 처음 보는 버스다 */
				bus_info_ptr1 = kzalloc_obj(struct bus_info); /* [한국어] 버스 정보를 새로 만든다 */
				if (!bus_info_ptr1) { /* [한국어] 못 잡으면 */
					rc = -ENOMEM; /* [한국어] 메모리 부족을 담아 */
					goto error_no_slot; /* [한국어] 컨트롤러를 되돌리는 라벨로 간다 */
				}
				bus_info_ptr1->slot_min = slot_ptr->slot_num; /* [한국어] 슬롯이 하나뿐이므로 최소와 */
				bus_info_ptr1->slot_max = slot_ptr->slot_num; /* [한국어] 최대가 같은 값으로 시작한다. 바로 위 상류 주석이 그 사정을 밝힌다 */
				bus_info_ptr1->slot_count += 1; /* [한국어] 슬롯 개수를 하나 센다 */
				bus_info_ptr1->busno = slot_ptr->slot_bus_num; /* [한국어] 버스 번호를 담는다 */
				bus_info_ptr1->index = bus_index++; /* [한국어] **컨트롤러 안에서의 상대 번호**를 매기고 다음 값으로 올린다. ibmphp_get_bus_index() 가 이 값을 돌려주고, ibmphp_hpc.c 가 그것으로 버스 레지스터를 짚는다 */
				bus_info_ptr1->current_speed = 0xff; /* [한국어] 현재 속도는 아직 모른다는 뜻으로 0xff 를 둔다. 실제 값은 ibmphp_hpc.c 가 채운다 */
				bus_info_ptr1->current_bus_mode = 0xff; /* [한국어] 현재 버스 모드도 마찬가지로 모름으로 둔다 */

				bus_info_ptr1->controller_id = hpc_ptr->ctlr_id; /* [한국어] 이 버스를 담당하는 컨트롤러의 id 를 남긴다 */

				list_add_tail(&bus_info_ptr1->bus_info_list, &bus_info_head); /* [한국어] **목록 끝에 매단다.** list_add 가 아니라 list_add_tail 이라 표에 적힌 순서가 목록에 그대로 남는다 */

			} else {
				bus_info_ptr2->slot_min = min(bus_info_ptr2->slot_min, slot_ptr->slot_num); /* [한국어] 가장 작은 슬롯 번호를 낮추고 */
				bus_info_ptr2->slot_max = max(bus_info_ptr2->slot_max, slot_ptr->slot_num); /* [한국어] 가장 큰 슬롯 번호를 올린다. 이 두 값이 ibmphp_pci.c 의 find_sec_number() 가 2차 버스 번호를 고르는 근거가 된다 */
				bus_info_ptr2->slot_count += 1; /* [한국어] 슬롯 개수를 하나 더 센다 */

			}

			// end of creating the bus_info linked list

			slot_ptr++; /* [한국어] 다음 슬롯 칸으로 */
			addr_slot += 1; /* [한국어] 평행 배열 안에서 한 칸 옆으로 — 네 필드가 모두 이 하나의 이동으로 함께 밀린다 */
		}

		/* init bus structure */
		bus_ptr = hpc_ptr->buses; /* [한국어] 버스 배열의 첫 칸부터 채운다 */
		for (bus = 0; bus < bus_num; bus++) { /* [한국어] 버스 개수만큼 반복한다 */
			bus_ptr->bus_num = readb(io_mem + addr_bus + bus); /* [한국어] 앞쪽에 모여 있는 버스 번호 배열에서 읽는다 */
			bus_ptr->slots_at_33_conv = readb(io_mem + addr_bus + bus_num + 8 * bus); /* [한국어] 뒤쪽 속도 능력 구역의 시작은 버스 번호 배열 다음이므로 bus_num 을 더하고, 버스당 8바이트이므로 8을 곱한다. +0 은 33MHz 통상 PCI 슬롯 수 */
			bus_ptr->slots_at_66_conv = readb(io_mem + addr_bus + bus_num + 8 * bus + 1); /* [한국어] +1 은 66MHz 통상 PCI 슬롯 수 */

			bus_ptr->slots_at_66_pcix = readb(io_mem + addr_bus + bus_num + 8 * bus + 2); /* [한국어] +2 는 66MHz PCI-X 슬롯 수 */

			bus_ptr->slots_at_100_pcix = readb(io_mem + addr_bus + bus_num + 8 * bus + 3); /* [한국어] +3 은 100MHz PCI-X 슬롯 수 */

			bus_ptr->slots_at_133_pcix = readb(io_mem + addr_bus + bus_num + 8 * bus + 4); /* [한국어] +4 는 133MHz PCI-X 슬롯 수. 버스당 8바이트 중 나머지 3바이트는 이 코드가 읽지 않는다 */

			bus_info_ptr2 = ibmphp_find_same_bus_num(bus_ptr->bus_num); /* [한국어] 같은 버스의 bus_info 를 찾는다. 앞의 슬롯 루프가 이미 만들어 두었을 것이다 */
			if (bus_info_ptr2) { /* [한국어] 찾았으면 속도 능력을 그쪽에도 베낀다 */
				bus_info_ptr2->slots_at_33_conv = bus_ptr->slots_at_33_conv; /* [한국어] 33MHz 통상 */
				bus_info_ptr2->slots_at_66_conv = bus_ptr->slots_at_66_conv; /* [한국어] 66MHz 통상 */
				bus_info_ptr2->slots_at_66_pcix = bus_ptr->slots_at_66_pcix; /* [한국어] 66MHz PCI-X */
				bus_info_ptr2->slots_at_100_pcix = bus_ptr->slots_at_100_pcix; /* [한국어] 100MHz PCI-X */
				bus_info_ptr2->slots_at_133_pcix = bus_ptr->slots_at_133_pcix; /* [한국어] 133MHz PCI-X */
			}
			bus_ptr++; /* [한국어] 다음 버스 칸으로 */
		}

		hpc_ptr->ctlr_type = temp; /* [한국어] 앞에서 읽어 둔 종류를 컨트롤러에 담는다 */

		switch (hpc_ptr->ctlr_type) { /* [한국어] **종류에 따라 union u 의 어느 갈래를 채울지와 addr 를 몇 바이트 밀지가 달라진다** */
			case 1: /* [한국어] PCI 형 컨트롤러 — 3바이트 */
				hpc_ptr->u.pci_ctlr.bus = readb(io_mem + addr); /* [한국어] 컨트롤러가 붙어 있는 PCI 버스 번호 */
				hpc_ptr->u.pci_ctlr.dev_fun = readb(io_mem + addr + 1); /* [한국어] 컨트롤러의 장치·함수 번호. ibmphp_probe() 가 이 둘로 struct pci_dev 를 맞춰 붙인다 */
				hpc_ptr->irq = readb(io_mem + addr + 2); /* [한국어] 컨트롤러가 쓰는 IRQ */
				addr += 3; /* [한국어] 3바이트를 읽었으므로 그만큼 민다 */
				debug("ctrl bus = %x, ctlr devfun = %x, irq = %x\n", /* [한국어] 읽은 내용을 남긴다 */
					hpc_ptr->u.pci_ctlr.bus, /* [한국어] 버스 번호 */
					hpc_ptr->u.pci_ctlr.dev_fun, hpc_ptr->irq); /* [한국어] 장치·함수와 IRQ */
				break; /* [한국어] 갈래를 벗어난다 */

			case 0: /* [한국어] ISA 형 컨트롤러 — 5바이트 */
				hpc_ptr->u.isa_ctlr.io_start = readw(io_mem + addr); /* [한국어] 차지하는 I/O 포트 구간의 시작(2바이트) */
				hpc_ptr->u.isa_ctlr.io_end = readw(io_mem + addr + 2); /* [한국어] 그 구간의 끝(2바이트) */
				if (!request_region(hpc_ptr->u.isa_ctlr.io_start, /* [한국어] **그 I/O 구간을 커널에 예약한다.** 다른 드라이버가 같은 포트를 쓰지 못하게 막는 것으로, 세 종류 중 이쪽만 필요한 절차다 */
						     (hpc_ptr->u.isa_ctlr.io_end - hpc_ptr->u.isa_ctlr.io_start + 1), /* [한국어] 구간 길이는 끝에서 시작을 빼고 1 을 더한 값이다 */
						     "ibmphp")) { /* [한국어] 예약자 이름 */
					rc = -ENODEV; /* [한국어] 이미 다른 쪽이 쓰고 있으면 장치 없음을 담아 */
					goto error_no_slot; /* [한국어] 컨트롤러를 되돌리는 라벨로 간다 */
				}
				hpc_ptr->irq = readb(io_mem + addr + 4); /* [한국어] 컨트롤러가 쓰는 IRQ */
				addr += 5; /* [한국어] 5바이트를 읽었으므로 그만큼 민다 */
				break; /* [한국어] 갈래를 벗어난다 */

			case 2: /* [한국어] i2c 형 컨트롤러 — 6바이트 */
			case 4: /* [한국어] 종류 4 도 같은 배치다. create_file_name() 은 4 만 확장 상자로 다룬다 */
				hpc_ptr->u.wpeg_ctlr.wpegbbar = readl(io_mem + addr); /* [한국어] i2c 접근의 기준 주소(4바이트) */
				hpc_ptr->u.wpeg_ctlr.i2c_addr = readb(io_mem + addr + 4); /* [한국어] 컨트롤러의 i2c 주소 */
				hpc_ptr->irq = readb(io_mem + addr + 5); /* [한국어] 컨트롤러가 쓰는 IRQ */
				addr += 6; /* [한국어] 6바이트를 읽었으므로 그만큼 민다 */
				break; /* [한국어] 갈래를 벗어난다 */
			default: /* [한국어] 아는 종류가 아니면 */
				rc = -ENODEV; /* [한국어] 장치 없음을 담아 */
				goto error_no_slot; /* [한국어] 컨트롤러를 되돌리는 라벨로 간다 */
		}

		//reorganize chassis' linked list
		combine_wpg_for_chassis(); /* [한국어] 섀시 목록을 섀시 번호별로 뭉친다. 컨트롤러마다 다시 부르지만 목록이 비워지지 않으므로 두 번째부터는 구간만 넓어진다 */
		combine_wpg_for_expansion(); /* [한국어] 확장 상자 목록도 같은 방식으로 뭉친다 */
		hpc_ptr->revision = 0xff; /* [한국어] 개정 번호는 아직 모른다는 뜻으로 0xff 를 둔다. 실제 값은 ibmphp_hpc.c 가 컨트롤러에게 물어 채운다 */
		hpc_ptr->options = 0xff; /* [한국어] 지원 옵션도 마찬가지로 모름으로 둔다 */
		hpc_ptr->starting_slot_num = hpc_ptr->slots[0].slot_num; /* [한국어] 담당 슬롯 번호 구간의 시작 — 슬롯 배열의 첫 칸이다 */
		hpc_ptr->ending_slot_num = hpc_ptr->slots[slot_num-1].slot_num; /* [한국어] 담당 슬롯 번호 구간의 끝 — 마지막 칸이다. calculate_first_slot() 이 이 값을 읽는다 */

		// register slots with hpc core as well as create linked list of ibm slot
		for (index = 0; index < hpc_ptr->slot_count; index++) { /* [한국어] 담당 슬롯마다 struct slot 을 만들어 등록한다 */
			tmp_slot = kzalloc_obj(*tmp_slot); /* [한국어] 슬롯 구조체를 잡는다 */
			if (!tmp_slot) { /* [한국어] 못 잡으면 */
				rc = -ENOMEM; /* [한국어] 메모리 부족을 담아 */
				goto error_no_slot; /* [한국어] 컨트롤러를 되돌리는 라벨로 간다 */
			}

			tmp_slot->flag = 1; /* [한국어] ibmphp.h:697 이 "disable slot 과 폴링용" 이라고 적어 둔 표시. 1 로 시작한다 */

			tmp_slot->capabilities = hpc_ptr->slots[index].slot_cap; /* [한국어] 표에서 읽은 슬롯 능력 비트를 그대로 담아 둔다 */
			if ((hpc_ptr->slots[index].slot_cap & EBDA_SLOT_133_MAX) == EBDA_SLOT_133_MAX) /* [한국어] **133MHz 비트(0x20)가 서 있으면** */
				tmp_slot->supported_speed =  3; /* [한국어] 지원 속도 등급 3 */
			else if ((hpc_ptr->slots[index].slot_cap & EBDA_SLOT_100_MAX) == EBDA_SLOT_100_MAX) /* [한국어] 아니고 100MHz 비트(0x10)가 서 있으면 */
				tmp_slot->supported_speed =  2; /* [한국어] 지원 속도 등급 2 */
			else if ((hpc_ptr->slots[index].slot_cap & EBDA_SLOT_66_MAX) == EBDA_SLOT_66_MAX) /* [한국어] 아니고 66MHz 비트(0x02)가 서 있으면 */
				tmp_slot->supported_speed =  1; /* [한국어] 지원 속도 등급 1. 셋 다 아니면 kzalloc 이 남긴 0(33MHz)이 그대로 남는다 */

			if ((hpc_ptr->slots[index].slot_cap & EBDA_SLOT_PCIX_CAP) == EBDA_SLOT_PCIX_CAP) /* [한국어] PCI-X 능력 비트(0x08)가 서 있으면 */
				tmp_slot->supported_bus_mode = 1; /* [한국어] PCI-X 모드를 지원한다고 표시하고 */
			else
				tmp_slot->supported_bus_mode = 0; /* [한국어] 통상 PCI 만 지원한다고 표시한다 */


			tmp_slot->bus = hpc_ptr->slots[index].slot_bus_num; /* [한국어] 이 슬롯이 붙어 있는 PCI 버스 번호 */

			bus_info_ptr1 = ibmphp_find_same_bus_num(hpc_ptr->slots[index].slot_bus_num); /* [한국어] 그 버스의 bus_info 를 찾는다. 앞의 슬롯 루프가 이미 만들어 두었어야 한다 */
			if (!bus_info_ptr1) { /* [한국어] 없으면 표가 어긋난 것이다 */
				rc = -ENODEV; /* [한국어] 장치 없음을 담아 */
				goto error; /* [한국어] 만들던 슬롯부터 되돌리는 라벨로 간다 */
			}
			tmp_slot->bus_on = bus_info_ptr1; /* [한국어] 찾은 버스 정보를 슬롯에 이어 준다 */
			bus_info_ptr1 = NULL; /* [한국어] 다음 회차에서 잘못 재사용하지 않도록 끊어 둔다 */
			tmp_slot->ctrl = hpc_ptr; /* [한국어] 이 슬롯을 담당하는 컨트롤러를 이어 준다 */

			tmp_slot->ctlr_index = hpc_ptr->slots[index].ctl_index; /* [한국어] 컨트롤러 레지스터를 짚을 때 쓰는 인덱스 */
			tmp_slot->number = hpc_ptr->slots[index].slot_num; /* [한국어] 물리 슬롯 번호. ibmphp_get_slot_from_physical_num() 이 이 값으로 슬롯을 되찾는다 */

			rc = fillslotinfo(&tmp_slot->hotplug_slot); /* [한국어] **컨트롤러에게 이 슬롯의 현재 상태를 물어 채운다.** EBDA 표는 슬롯이 어디에 있는지만 알려 줄 뿐 지금 카드가 꽂혀 있는지는 알려 주지 않는다 */
			if (rc) /* [한국어] 실패하면 */
				goto error; /* [한국어] 만들던 슬롯부터 되돌리는 라벨로 간다 */

			rc = ibmphp_init_devno(&tmp_slot); /* [한국어] **슬롯의 PCI 장치 번호를 IRQ 라우팅 표에서 찾아 채운다.** ibmphp_core.c 에 있는 함수이며, 이중 포인터로 받는 것은 그쪽 구현 사정이다 */
			if (rc) /* [한국어] 실패하면 */
				goto error; /* [한국어] 만들던 슬롯부터 되돌리는 라벨로 간다 */
			tmp_slot->hotplug_slot.ops = &ibmphp_hotplug_slot_ops; /* [한국어] 핫플러그 코어가 부를 콜백 묶음을 이어 준다. ibmphp_core.c 가 정의한다 */

			// end of registering ibm slot with hotplug core

			list_add(&tmp_slot->ibm_slot_list, &ibmphp_slot_head); /* [한국어] 슬롯 목록에 매단다. 아직 코어에는 등록하지 않는다 — 이름을 지으려면 목록 전체가 서 있어야 하기 때문이다 */
		}

		print_bus_info(); /* [한국어] 이 컨트롤러까지 반영된 버스 정보를 남긴다 */
		list_add(&hpc_ptr->ebda_hpc_list, &ebda_hpc_head); /* [한국어] 컨트롤러를 목록에 매단다 */

	}			/* each hpc  */

	list_for_each_entry(tmp_slot, &ibmphp_slot_head, ibm_slot_list) { /* [한국어] **모든 컨트롤러를 처리한 뒤 다시 한 번 슬롯 전체를 돈다.** 이름 짓기가 슬롯 목록 전체를 봐야 하므로 등록을 두 단계로 나눈 것이다 */
		snprintf(name, SLOT_NAME_SIZE, "%s", create_file_name(tmp_slot)); /* [한국어] "chassis1slot2" 꼴 이름을 지어 버퍼에 담는다. create_file_name() 이 NULL 을 돌려줄 수 있는데 그대로 %s 에 넘어간다 */
		pci_hp_register(&tmp_slot->hotplug_slot, /* [한국어] **핫플러그 코어에 슬롯을 등록한다.** 이 호출이 끝나야 sysfs 에 슬롯이 보인다 */
			pci_find_bus(0, tmp_slot->bus), tmp_slot->device, name); /* [한국어] 0번 도메인에서 이 슬롯의 버스를 찾아 넘기고, 장치 번호와 지은 이름을 함께 넘긴다 */
	}

	print_ebda_hpc(); /* [한국어] 컨트롤러 목록 전체를 남긴다 */
	print_ibm_slot(); /* [한국어] 슬롯 목록도 남긴다 */
	return 0; /* [한국어] 모든 컨트롤러를 처리했으면 성공 */

error: /* [한국어] **슬롯 만들기 실패 경로** — 방금 잡은 슬롯을 먼저 버린다 */
	kfree(tmp_slot); /* [한국어] 아직 목록에 매달지 않은 슬롯이므로 그냥 버리면 된다 */
error_no_slot: /* [한국어] **컨트롤러 되돌리기 경로** — 위에서 떨어져 오거나 곧바로 뛰어온다 */
	free_ebda_hpc(hpc_ptr); /* [한국어] 만들던 컨트롤러와 그 슬롯·버스 배열을 버린다. 앞서 처리를 마친 컨트롤러들과 이미 목록에 매단 슬롯들은 여기서 되돌리지 않는다 */
	return rc; /* [한국어] 담아 둔 오류 코드를 돌려준다 */
}

/*
 * map info (bus, devfun, start addr, end addr..) of i/o, memory,
 * pfm from the physical addr to a list of resource.
 */
/* [한국어]
 * ebda_rsrc_rsrc - POST 가 이미 배정해 둔 자원 항목들을 목록으로 만든다
 *
 * @return: 0 이면 성공. 항목을 할당하지 못하면 -ENOMEM.
 *
 * 바로 위 상류 주석대로 I/O·메모리·프리페치 메모리 자원의 (버스, devfun,
 * 시작 주소, 끝 주소)를 물리 주소에서 읽어 자원 목록으로 만든다. 이 함수의
 * 결과물인 ibmphp_ebda_pci_rsrc_head 가 ibmphp_res.c 의 ibmphp_rsrc_init()
 * 이 자유 목록 장부를 세우는 유일한 원재료다. 즉 이 드라이버가 "지금 무엇이
 * 어디에 배정되어 있는가" 를 아는 근거가 전부 여기서 나온다.
 *
 * 항목 하나는 종류 바이트로 시작하고, 그 하위 2비트가 종류를 가른다.
 * I/O 항목은 주소가 16비트라 6바이트, 메모리·프리페치는 32비트라 10바이트가
 * 이어진다. 종류 바이트 전체를 rsrc_type 에 그대로 담아 두는데, 상위 비트에
 * "PCI 장치인가"(EBDA_PCI_DEV) 와 "버스의 범위 선언인가"(EBDA_PRI_PCI_BUS_INFO)
 * 같은 정보가 더 들어 있고 그것을 ibmphp_res.c 가 읽는다.
 *
 * [관찰] 걷기의 addr 는 I/O·메모리·프리페치 갈래 안에서만 밀린다. 예약
 * 종류(EBDA_RES_RSRC_TYPE, 0x02)를 만나면 종류 바이트 1 만 밀린 채 다음
 * 항목을 읽게 된다. 그런 항목이 실제 표에 나타나는지는 이 트리에서
 * 확인 못 함.
 *
 * [관찰] 실패 경로가 io_mem 을 iounmap 하고 돌아가는데, 호출자인
 * ibmphp_access_ebda() 도 out 라벨에서 다시 iounmap 한다.
 *
 * 다 읽고 나면 머리 구조체(rsrc_list_ptr)를 버리고 NULL 로 둔다. 항목들이
 * 이미 목록으로 옮겨졌기 때문이다.
 *
 * 실행 컨텍스트: 모듈 초기화(프로세스 컨텍스트), 단일 스레드. __init 이다.
 *
 * 호출 체인:
 *   ibmphp_access_ebda() -> [이 함수] -> alloc_ebda_pci_rsrc()
 *                                     -> readb()/readw()/readl()
 *                                     -> print_ebda_pci_rsrc()
 */
static int __init ebda_rsrc_rsrc(void)
{
	u16 addr; /* [한국어] 항목 배열을 걷는 위치 */
	short rsrc; /* [한국어] 항목 루프 인덱스 */
	u8 type, rsrc_type; /* [한국어] type 은 종류 바이트 전체, rsrc_type 은 그 하위 2비트만 남긴 값 */
	struct ebda_pci_rsrc *rsrc_ptr; /* [한국어] 항목 하나를 담을 구조체 */

	addr = rsrc_list_ptr->phys_addr; /* [한국어] ibmphp_access_ebda() 가 기억해 둔 항목 배열의 시작에서 출발한다 */
	debug("now entering rsrc land\n"); /* [한국어] 걷기 시작을 남긴다 */
	debug("offset of rsrc: %x\n", rsrc_list_ptr->phys_addr); /* [한국어] 시작 오프셋도 남긴다 */

	for (rsrc = 0; rsrc < rsrc_list_ptr->num_entries; rsrc++) { /* [한국어] 표에 적힌 개수만큼 항목을 읽는다 */
		type = readb(io_mem + addr); /* [한국어] 항목의 첫 바이트가 종류다 */

		addr += 1; /* [한국어] 읽은 만큼 민다 */
		rsrc_type = type & EBDA_RSRC_TYPE_MASK; /* [한국어] **하위 2비트만 남겨 종류를 가른다.** 상위 비트에는 PCI 장치인지(EBDA_PCI_DEV 0x10), 버스의 범위 선언인지(EBDA_PRI_PCI_BUS_INFO 0x20) 같은 정보가 더 들어 있고 그것은 ibmphp_res.c 가 읽는다 */

		if (rsrc_type == EBDA_IO_RSRC_TYPE) { /* [한국어] 종류가 0x00 이면 I/O 자원이다 */
			rsrc_ptr = alloc_ebda_pci_rsrc(); /* [한국어] 항목 구조체를 잡는다 */
			if (!rsrc_ptr) { /* [한국어] 못 잡으면 */
				iounmap(io_mem); /* [한국어] 매핑을 풀고 */
				return -ENOMEM; /* [한국어] 메모리 부족으로 돌아간다. 호출자도 out 라벨에서 다시 iounmap 한다 */
			}
			rsrc_ptr->rsrc_type = type; /* [한국어] **하위 비트를 지우지 않고 종류 바이트를 통째로 담는다** — 상위 비트가 ibmphp_res.c 에 필요하기 때문이다 */

			rsrc_ptr->bus_num = readb(io_mem + addr); /* [한국어] +0 이 자원이 배정된 버스 번호 */
			rsrc_ptr->dev_fun = readb(io_mem + addr + 1); /* [한국어] +1 그 버스에서의 장치·함수 번호 */
			rsrc_ptr->start_addr = readw(io_mem + addr + 2); /* [한국어] +2 시작 주소. **I/O 는 주소가 16비트**라 readw 로 읽는다 */
			rsrc_ptr->end_addr = readw(io_mem + addr + 4); /* [한국어] +4 끝 주소 */
			addr += 6; /* [한국어] I/O 항목은 종류 바이트 뒤로 6바이트이므로 그만큼 민다 */

			debug("rsrc from io type ----\n"); /* [한국어] 어느 종류를 읽었는지 남긴다 */
			debug("rsrc type: %x bus#: %x dev_func: %x start addr: %x end addr: %x\n", /* [한국어] 항목 내용을 남긴다 */
				rsrc_ptr->rsrc_type, rsrc_ptr->bus_num, rsrc_ptr->dev_fun, rsrc_ptr->start_addr, rsrc_ptr->end_addr); /* [한국어] 종류, 버스, 장치·함수, 시작·끝 주소 */

			list_add(&rsrc_ptr->ebda_pci_rsrc_list, &ibmphp_ebda_pci_rsrc_head); /* [한국어] **자원 목록에 매단다.** 이 목록이 ibmphp_res.c 의 ibmphp_rsrc_init() 이 자유 목록 장부를 세울 원재료다 */
		}

		if (rsrc_type == EBDA_MEM_RSRC_TYPE || rsrc_type == EBDA_PFM_RSRC_TYPE) { /* [한국어] 종류가 0x01(메모리)이거나 0x03(프리페치 메모리)이면 */
			rsrc_ptr = alloc_ebda_pci_rsrc(); /* [한국어] 항목 구조체를 잡는다 */
			if (!rsrc_ptr) { /* [한국어] 못 잡으면 */
				iounmap(io_mem); /* [한국어] 매핑을 풀고 */
				return -ENOMEM; /* [한국어] 메모리 부족으로 돌아간다 */
			}
			rsrc_ptr->rsrc_type = type; /* [한국어] 종류 바이트를 통째로 담는다 */

			rsrc_ptr->bus_num = readb(io_mem + addr); /* [한국어] +0 이 자원이 배정된 버스 번호 */
			rsrc_ptr->dev_fun = readb(io_mem + addr + 1); /* [한국어] +1 그 버스에서의 장치·함수 번호 */
			rsrc_ptr->start_addr = readl(io_mem + addr + 2); /* [한국어] +2 시작 주소. **메모리는 주소가 32비트**라 readl 로 읽는다 */
			rsrc_ptr->end_addr = readl(io_mem + addr + 6); /* [한국어] +6 끝 주소 */
			addr += 10; /* [한국어] 메모리 항목은 종류 바이트 뒤로 10바이트이므로 그만큼 민다 */

			debug("rsrc from mem or pfm ---\n"); /* [한국어] 어느 종류를 읽었는지 남긴다 */
			debug("rsrc type: %x bus#: %x dev_func: %x start addr: %x end addr: %x\n", /* [한국어] 항목 내용을 남긴다 */
				rsrc_ptr->rsrc_type, rsrc_ptr->bus_num, rsrc_ptr->dev_fun, rsrc_ptr->start_addr, rsrc_ptr->end_addr); /* [한국어] 종류, 버스, 장치·함수, 시작·끝 주소 */

			list_add(&rsrc_ptr->ebda_pci_rsrc_list, &ibmphp_ebda_pci_rsrc_head); /* [한국어] 같은 자원 목록에 매단다 */
		}
	}
	kfree(rsrc_list_ptr); /* [한국어] 항목이 모두 목록으로 옮겨졌으므로 머리 구조체는 더 필요 없다 */
	rsrc_list_ptr = NULL; /* [한국어] 매달린 포인터를 끊는다 */
	print_ebda_pci_rsrc(); /* [한국어] 만든 목록 전체를 남긴다 */
	return 0; /* [한국어] 표를 다 읽었으면 성공 */
}

/* [한국어]
 * ibmphp_get_total_controllers - EBDA 표에 적혀 있던 컨트롤러 개수를 알려 준다
 *
 * @return: 핫스왑 블록의 RC 하위 블록에서 읽은 num_ctlrs 값.
 *
 * ibmphp_hpc.c 의 폴링 스레드(:808, :840)가 "컨트롤러를 다 돌았는가" 를
 * 판단하는 데 쓴다. 그 파일은 컨트롤러 목록 자체를 볼 수 없으므로 개수만
 * 이 함수로 받아 간다.
 *
 * hpc_list_ptr 은 초기화 때 한 번 만들어지고 해제되지 않으므로 드라이버가
 * 살아 있는 내내 유효하다. 다만 ibmphp_access_ebda() 가 핫스왑 블록을 찾지
 * 못한 채(hs_complete 가 0) RIO 블록만으로 성공한 경우에는 hpc_list_ptr 이
 * NULL 인 채로 남는다 - 그 상태에서 이 함수를 부르면 NULL 을 따라간다.
 * 그런 구성이 실제로 있는지는 확인 못 함.
 *
 * 실행 컨텍스트: ibmphp_hpc.c 의 폴링 커널 스레드. 읽기만 하므로 락이 없다.
 *
 * 호출 체인:  ibmphp_hpc.c 의 폴링 루프 -> [이 함수]
 */
u16 ibmphp_get_total_controllers(void)
{
	return hpc_list_ptr->num_ctlrs; /* [한국어] ibmphp_access_ebda() 가 RC 하위 블록에서 읽어 담아 둔 개수를 그대로 돌려준다 */
}

/* [한국어]
 * ibmphp_get_slot_from_physical_num - 물리 슬롯 번호로 슬롯 구조체를 찾는다
 *
 * @physical_num: 찾을 물리 슬롯 번호(slot->number).
 * @return: 그 번호를 가진 struct slot. 없으면 NULL.
 *
 * 사용자나 컨트롤러 쪽에서 오는 정보는 물리 슬롯 번호이므로, 그것을
 * 드라이버의 슬롯 구조체로 되돌리는 통로가 필요하다.
 *
 * 읽는 곳은 셋이다 - ibmphp_core.c:718 과 :843(슬롯을 켜고 끌 때 짝이 되는
 * 슬롯을 찾는 자리), 그리고 ibmphp_hpc.c:996(컨트롤러가 알린 상태 변화를
 * 어느 슬롯의 것인지 되짚는 자리).
 *
 * 목록이 짧다는 전제의 선형 탐색이며 락이 없다. ibmphp_slot_head 는 초기화
 * 때 다 채워진 뒤로는 모듈이 내려갈 때까지 바뀌지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(sysfs 조작)와 폴링 커널 스레드 양쪽.
 *
 * 호출 체인:  ibmphp_core.c / ibmphp_hpc.c -> [이 함수]
 */
struct slot *ibmphp_get_slot_from_physical_num(u8 physical_num)
{
	struct slot *slot; /* [한국어] 목록을 훑을 반복 포인터 */

	list_for_each_entry(slot, &ibmphp_slot_head, ibm_slot_list) { /* [한국어] 등록된 슬롯을 하나씩 본다 */
		if (slot->number == physical_num) /* [한국어] 물리 슬롯 번호가 같으면 */
			return slot; /* [한국어] 그 슬롯을 돌려준다 */
	}
	return NULL; /* [한국어] 끝까지 못 찾았으면 그런 슬롯이 없다 */
}

/* To find:
 *	- the smallest slot number
 *	- the largest slot number
 *	- the total number of the slots based on each bus
 *	  (if only one slot per bus slot_min = slot_max )
 */
/* [한국어]
 * ibmphp_find_same_bus_num - 버스 번호로 그 버스의 bus_info 를 찾는다
 *
 * @num: 찾을 PCI 버스 번호.
 * @return: 그 버스의 struct bus_info. 없으면 NULL.
 *
 * 바로 위 상류 주석이 bus_info 가 무엇을 담는지 밝힌다 - 그 버스의 가장
 * 작은 슬롯 번호, 가장 큰 슬롯 번호, 슬롯 개수(버스에 슬롯이 하나뿐이면
 * min 과 max 가 같다).
 *
 * 이 파일 밖에서 쓰는 곳이 두 군데다.
 *   - ibmphp_pci.c:2246 의 find_sec_number() - 브리지 뒤에 붙일 2차 버스
 *     번호를 고를 때 "이 버스가 담당하는 슬롯 번호 범위" 를 여기서 얻는다.
 *     슬롯의 순번을 구해 1차 버스 번호에 더하는 방식이라, 이 함수가 NULL 을
 *     돌려주면 브리지 카드를 설정할 수 없다.
 *   - ibmphp_core.c:624 - 이 버스 번호가 시스템에 이미 있는지 판별한다.
 * 파일 안에서는 ebda_rsrc_controller() 가 목록을 만들면서 중복을 거를 때
 * 쓴다.
 *
 * 목록이 짧다는 전제의 선형 탐색이며 락이 없다. bus_info_head 는 초기화
 * 때만 바뀐다.
 *
 * 실행 컨텍스트: 모듈 초기화와 프로세스 컨텍스트(카드 삽입) 양쪽.
 *
 * 호출 체인:  ebda_rsrc_controller() / ibmphp_pci.c / ibmphp_core.c -> [이 함수]
 */
struct bus_info *ibmphp_find_same_bus_num(u32 num)
{
	struct bus_info *ptr; /* [한국어] 목록을 훑을 반복 포인터 */

	list_for_each_entry(ptr, &bus_info_head, bus_info_list) { /* [한국어] 버스 정보를 하나씩 본다 */
		if (ptr->busno == num) /* [한국어] 버스 번호가 같으면 */
			 return ptr; /* [한국어] 그 버스 정보를 돌려준다 */
	}
	return NULL; /* [한국어] 끝까지 못 찾았으면 이 드라이버가 모르는 버스다 */
}

/*  Finding relative bus number, in order to map corresponding
 *  bus register
 */
/* [한국어]
 * ibmphp_get_bus_index - 버스 번호를 컨트롤러 안의 상대 인덱스로 바꾼다
 *
 * @num: PCI 버스 번호.
 * @return: 그 버스의 index 필드. 못 찾으면 -ENODEV.
 *
 * 바로 위 상류 주석대로 대응하는 버스 레지스터를 짚기 위한 상대 번호를
 * 구한다. 핫플러그 컨트롤러의 레지스터는 시스템 전체의 PCI 버스 번호가
 * 아니라 그 컨트롤러가 몇 번째로 담당하는 버스인지로 주소가 매겨지기
 * 때문이다. index 는 ebda_rsrc_controller() 가 bus_info 를 만들면서
 * 컨트롤러마다 1 부터 매긴 값이다.
 *
 * 읽는 곳은 ibmphp_hpc.c:521 과 :661 - 슬롯의 버스 속도를 읽고 쓸 때
 * 어느 레지스터를 볼지 정하는 자리다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트와 폴링 커널 스레드 양쪽.
 *
 * 호출 체인:  ibmphp_hpc.c -> [이 함수]
 */
int ibmphp_get_bus_index(u8 num)
{
	struct bus_info *ptr; /* [한국어] 목록을 훑을 반복 포인터 */

	list_for_each_entry(ptr, &bus_info_head, bus_info_list) { /* [한국어] 버스 정보를 하나씩 본다 */
		if (ptr->busno == num) /* [한국어] 버스 번호가 같으면 */
			return ptr->index; /* [한국어] **컨트롤러 안에서의 상대 번호**를 돌려준다. 시스템 전체의 버스 번호가 아니라 그 컨트롤러가 몇 번째로 담당하는 버스인지다 */
	}
	return -ENODEV; /* [한국어] 못 찾으면 장치 없음. 앞의 함수와 달리 오류 코드를 쓴다 — 반환형이 포인터가 아니라 int 이기 때문이다 */
}

/* [한국어]
 * ibmphp_free_bus_info_queue - bus_info 목록의 모든 항목을 버린다
 *
 * 모듈이 내려갈 때 ibmphp_core.c:1171 이 부르는 세 해제 함수 중 첫째다.
 * ebda_rsrc_controller() 가 슬롯을 읽으면서 만든 버스별 정보를 되돌린다.
 *
 * list_for_each_entry_safe() 를 쓰는 이유는 순회 중에 항목을 해제하기
 * 때문이다 - 다음 포인터를 미리 붙잡아 두지 않으면 이미 버린 메모리에서
 * 다음 항목을 읽게 된다.
 *
 * [관찰] 항목을 kfree 하지만 list_del 로 목록에서 빼지는 않는다. 머리인
 * bus_info_head 는 파일 정적이고 모듈이 내려가는 중이라 다시 훑히지
 * 않으므로 지금 구조에서는 드러나지 않는다. 뒤의 두 해제 함수도 같다.
 *
 * 실행 컨텍스트: 모듈 해제(프로세스 컨텍스트), 단일 스레드.
 *
 * 호출 체인:  ibmphp_exit() [ibmphp_core.c] -> [이 함수] -> kfree()
 */
void ibmphp_free_bus_info_queue(void)
{
	struct bus_info *bus_info, *next; /* [한국어] bus_info 는 지금 버릴 항목, next 는 미리 붙잡아 둘 다음 항목 */

	list_for_each_entry_safe(bus_info, next, &bus_info_head, /* [한국어] **해제하면서 순회하므로 safe 판을 쓴다.** 다음 포인터를 미리 붙잡아 두지 않으면 이미 버린 메모리에서 다음 항목을 읽게 된다 */
				 bus_info_list) { /* [한국어] 목록 연결 필드 이름 */
		kfree (bus_info); /* [한국어] 항목을 버린다. list_del 로 빼지는 않는데, 모듈이 내려가는 중이라 이 목록을 다시 훑는 곳이 없다 */
	}
}

/* [한국어]
 * ibmphp_free_ebda_hpc_queue - 컨트롤러 목록을 버리고 종류별 뒷정리를 한다
 *
 * 단순한 해제가 아니라 컨트롤러 종류에 따라 두 가지를 더 한다.
 *   - ISA 형(ctlr_type 0)이면 ebda_rsrc_controller() 가 잡아 둔 I/O 포트
 *     영역을 release_region 으로 돌려준다. request_region 과 짝을 맞추는
 *     유일한 자리다.
 *   - PCI 형(ctlr_type 1)이 하나라도 있으면 pci_unregister_driver() 로
 *     ibmphp_driver 를 뺀다. pci_flag 로 한 번만 하도록 막는데, 등록도
 *     ibmphp_register_pci() 에서 한 번만 했기 때문이다 - PCI 형 컨트롤러가
 *     여럿이어도 pci_driver 는 하나다.
 * 그 뒤 free_ebda_hpc() 로 컨트롤러와 그 슬롯·버스 배열을 버린다.
 *
 * 실행 컨텍스트: 모듈 해제(프로세스 컨텍스트), 단일 스레드.
 *
 * 호출 체인:
 *   ibmphp_exit() [ibmphp_core.c] -> [이 함수] -> release_region()
 *                                              -> pci_unregister_driver()
 *                                              -> free_ebda_hpc()
 */
void ibmphp_free_ebda_hpc_queue(void)
{
	struct controller *controller = NULL, *next; /* [한국어] controller 는 지금 버릴 항목, next 는 미리 붙잡아 둘 다음 항목 */
	int pci_flag = 0; /* [한국어] pci_driver 를 한 번만 빼도록 막는 표시 */

	list_for_each_entry_safe(controller, next, &ebda_hpc_head, /* [한국어] 해제하면서 순회하므로 safe 판을 쓴다 */
				 ebda_hpc_list) { /* [한국어] 목록 연결 필드 이름 */
		if (controller->ctlr_type == 0) /* [한국어] ISA 형 컨트롤러면 */
			release_region(controller->u.isa_ctlr.io_start, (controller->u.isa_ctlr.io_end - controller->u.isa_ctlr.io_start + 1)); /* [한국어] **ebda_rsrc_controller() 가 잡은 I/O 포트 영역을 돌려준다.** request_region 과 짝을 맞추는 유일한 자리다. 길이는 끝에서 시작을 빼고 1 을 더한 값이다 */
		else if ((controller->ctlr_type == 1) && (!pci_flag)) { /* [한국어] PCI 형 컨트롤러이면서 아직 빼지 않았으면 */
			++pci_flag; /* [한국어] 뺐다고 표시하고 */
			pci_unregister_driver(&ibmphp_driver); /* [한국어] **pci_driver 를 뺀다.** 컨트롤러가 여럿이어도 등록은 한 번뿐이었으므로 해제도 한 번이다 */
		}
		free_ebda_hpc(controller); /* [한국어] 컨트롤러와 그 슬롯·버스 배열을 버린다 */
	}
}

/* [한국어]
 * ibmphp_free_ebda_pci_rsrc_queue - POST 자원 항목 목록을 버린다
 *
 * ebda_rsrc_rsrc() 가 만든 ibmphp_ebda_pci_rsrc_head 를 비운다. 이 목록은
 * ibmphp_res.c 의 ibmphp_rsrc_init() 이 자기 장부를 세우면서 이미 다 읽은
 * 뒤이므로, 모듈이 내려갈 때 버려도 잃는 정보가 없다.
 *
 * [관찰] 루프 안에서 kfree 뒤에 resource = NULL 을 넣는데, resource 는
 * list_for_each_entry_safe 가 다음 회차에 다시 채우는 반복 변수라 이 대입은
 * 바깥으로 전해지지 않는다.
 *
 * 실행 컨텍스트: 모듈 해제(프로세스 컨텍스트), 단일 스레드.
 *
 * 호출 체인:  ibmphp_exit() [ibmphp_core.c] -> [이 함수] -> kfree()
 */
void ibmphp_free_ebda_pci_rsrc_queue(void)
{
	struct ebda_pci_rsrc *resource, *next; /* [한국어] resource 는 지금 버릴 항목, next 는 미리 붙잡아 둘 다음 항목 */

	list_for_each_entry_safe(resource, next, &ibmphp_ebda_pci_rsrc_head, /* [한국어] 해제하면서 순회하므로 safe 판을 쓴다 */
				 ebda_pci_rsrc_list) { /* [한국어] 목록 연결 필드 이름 */
		kfree (resource); /* [한국어] 항목을 버린다 */
		resource = NULL; /* [한국어] 반복 변수에 대입하는 것이라 다음 회차에서 덮어써진다 — 바깥으로 전해지지 않는다 */
	}
}

static const struct pci_device_id id_table[] = { /* [한국어] 커널이 이 드라이버에 맞는 PCI 장치를 찾을 때 쓰는 표. PCI 형 컨트롤러 하나만 노린다 */
	{
		.vendor		= PCI_VENDOR_ID_IBM, /* [한국어] IBM 벤더 id */
		.device		= HPC_DEVICE_ID, /* [한국어] 핫플러그 컨트롤러의 장치 id. ibmphp.h 에 정의되어 있다 */
		.subvendor	= PCI_VENDOR_ID_IBM, /* [한국어] 서브시스템도 IBM 이어야 한다 */
		.subdevice	= HPC_SUBSYSTEM_ID, /* [한국어] 서브시스템 장치 id */
		.class		= ((PCI_CLASS_SYSTEM_PCI_HOTPLUG << 8) | 0x00), /* [한국어] **클래스 코드로 한 번 더 좁힌다.** PCI_CLASS_SYSTEM_PCI_HOTPLUG 는 상위 16비트 값이라 8비트 왼쪽으로 밀고, 프로그래밍 인터페이스 자리에는 0 을 둔다 */
	}, {} /* [한국어] 표의 끝을 알리는 빈 항목 */
};

MODULE_DEVICE_TABLE(pci, id_table); /* [한국어] 모듈 자동 적재를 위해 이 표를 모듈 정보에 심는다 */

static int ibmphp_probe(struct pci_dev *, const struct pci_device_id *); /* [한국어] 아래 초기화식에서 쓰므로 미리 선언한다 */
static struct pci_driver ibmphp_driver = { /* [한국어] PCI 형 컨트롤러를 잡을 드라이버 구조체 */
	.name		= "ibmphp", /* [한국어] 드라이버 이름 */
	.id_table	= id_table, /* [한국어] 위에서 만든 장치 표 */
	.probe		= ibmphp_probe, /* [한국어] 맞는 장치를 찾았을 때 커널이 부를 함수 */
};

/* [한국어]
 * ibmphp_register_pci - PCI 형 컨트롤러가 있을 때만 pci_driver 를 등록한다
 *
 * @return: pci_register_driver() 의 반환값. PCI 형 컨트롤러가 하나도 없으면
 *          등록하지 않고 0 을 돌려준다.
 *
 * 이 드라이버가 관리하는 핫플러그 컨트롤러는 ISA·PCI·i2c 세 갈래인데, PCI
 * 버스에 붙은 것만 커널의 PCI 드라이버 모형으로 잡을 수 있다. 그래서
 * 컨트롤러 목록을 훑어 종류 1 이 하나라도 있을 때만 등록하고, 찾자마자
 * break 로 빠져나온다 - 컨트롤러가 여럿이어도 pci_driver 는 하나면 된다.
 *
 * 등록하면 커널이 id_table 과 맞는 장치를 찾아 ibmphp_probe() 를 부르고,
 * 거기서 struct pci_dev 를 컨트롤러 구조체에 이어 준다.
 *
 * 실행 컨텍스트: 모듈 초기화(프로세스 컨텍스트). ibmphp_access_ebda() 와
 * ibmphp_rsrc_init() 이 끝난 뒤에 불린다 - 컨트롤러 목록이 이미 서 있어야
 * 하기 때문이다.
 *
 * 호출 체인:  ibmphp_init() [ibmphp_core.c] -> [이 함수] -> pci_register_driver()
 */
int ibmphp_register_pci(void)
{
	struct controller *ctrl; /* [한국어] 컨트롤러 목록을 훑을 반복 포인터 */
	int rc = 0; /* [한국어] 호출자에게 돌려줄 값. PCI 형이 하나도 없으면 0 그대로다 */

	list_for_each_entry(ctrl, &ebda_hpc_head, ebda_hpc_list) { /* [한국어] 컨트롤러를 하나씩 본다 */
		if (ctrl->ctlr_type == 1) { /* [한국어] PCI 형을 찾으면 */
			rc = pci_register_driver(&ibmphp_driver); /* [한국어] **pci_driver 를 등록한다.** 이 호출로 커널이 id_table 과 맞는 장치를 찾아 ibmphp_probe() 를 부른다 */
			break; /* [한국어] 컨트롤러가 여럿이어도 드라이버는 하나면 되므로 곧바로 멈춘다 */
		}
	}
	return rc; /* [한국어] 등록 결과(또는 PCI 형이 없었으면 0)를 돌려준다 */
}
/* [한국어]
 * ibmphp_probe - 커널이 찾아 준 PCI 장치를 컨트롤러 구조체에 이어 준다
 *
 * @dev: 커널이 id_table 과 맞다고 판단한 PCI 장치.
 * @ids: 어느 항목과 맞았는지. 이 함수는 쓰지 않는다.
 * @return: 짝이 되는 컨트롤러를 찾아 이었으면 0, 못 찾으면 -ENODEV.
 *
 * 보통의 PCI 드라이버와 달리 여기서 장치를 초기화하지 않는다. EBDA 표가
 * 이미 "PCI 형 컨트롤러는 몇 번 버스의 어느 devfn 에 있다" 고 알려 주었기
 * 때문에, 이 함수가 하는 일은 그 표의 항목과 커널이 열거해 준 struct pci_dev
 * 를 맞춰 붙이는 것뿐이다.
 *
 * 맞추는 기준은 devfn 과 버스 번호가 둘 다 같은 것이다. id_table 은
 * IBM 벤더 id 와 핫플러그 컨트롤러 클래스로 후보를 좁힐 뿐이라, 같은
 * 컨트롤러가 여럿일 때 어느 것인지는 이 비교로 가른다.
 *
 * 찾은 pci_dev 는 ctrl->ctrl_dev 에 담기며, 컨트롤러와 실제로 주고받는
 * ibmphp_hpc.c 가 그것을 통해 config 공간에 접근한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. pci_register_driver() 안에서 곧바로
 * 불릴 수도 있고, 나중에 장치가 나타날 때 불릴 수도 있다.
 *
 * 호출 체인:  PCI 코어 -> [이 함수] -> list_for_each_entry()
 */
static int ibmphp_probe(struct pci_dev *dev, const struct pci_device_id *ids)
{
	struct controller *ctrl; /* [한국어] 컨트롤러 목록을 훑을 반복 포인터 */

	debug("inside ibmphp_probe\n"); /* [한국어] 들어왔음을 남긴다 */

	list_for_each_entry(ctrl, &ebda_hpc_head, ebda_hpc_list) { /* [한국어] 컨트롤러를 하나씩 본다 */
		if (ctrl->ctlr_type == 1) { /* [한국어] PCI 형만 후보다 */
			if ((dev->devfn == ctrl->u.pci_ctlr.dev_fun) && (dev->bus->number == ctrl->u.pci_ctlr.bus)) { /* [한국어] **장치·함수 번호와 버스 번호가 둘 다 같아야 이 컨트롤러다.** id_table 은 벤더와 클래스로 후보를 좁힐 뿐이라, 같은 컨트롤러가 여럿일 때 어느 것인지는 이 비교로 가른다 */
				ctrl->ctrl_dev = dev; /* [한국어] 찾은 pci_dev 를 컨트롤러에 이어 준다. ibmphp_hpc.c 가 이것으로 config 공간에 접근한다 */
				debug("found device!!!\n"); /* [한국어] 찾았음을 남긴다 */
				debug("dev->device = %x, dev->subsystem_device = %x\n", dev->device, dev->subsystem_device); /* [한국어] 어떤 장치였는지 남긴다 */
				return 0; /* [한국어] 성공으로 돌아간다 */
			}
		}
	}
	return -ENODEV; /* [한국어] 표에 없는 장치면 이 드라이버가 다룰 것이 아니다 */
}
