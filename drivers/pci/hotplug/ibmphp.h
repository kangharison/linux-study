/* SPDX-License-Identifier: GPL-2.0+ */
/* [한국어] **중복 포함 방지 가드.** 다섯 파일이 모두 이 헤더를 넣으므로,
 * 한 번역 단위에 두 번 들어와도 정의가 겹치지 않게 한다 */
#ifndef __IBMPHP_H
/* [한국어] 가드 매크로를 세운다. 파일 끝의 #endif 와 짝이다 */
#define __IBMPHP_H

/*
 * IBM Hot Plug Controller Driver
 *
 * Written By: Jyoti Shah, Tong Yu, Irene Zubarev, IBM Corporation
 *
 * Copyright (C) 2001 Greg Kroah-Hartman (greg@kroah.com)
 * Copyright (C) 2001-2003 IBM Corp.
 *
 * All rights reserved.
 *
 * Send feedback to <gregkh@us.ibm.com>
 *
 */

/*
 * [한국어 설명] ibmphp 드라이버 다섯 파일이 공유하는 유일한 헤더 (ibmphp.h)
 *
 * === 파일의 역할 ===
 * IBM 핫플러그 드라이버의 자료구조, 상수, 매크로가 모두 이 한 파일에 있다.
 * 다른 다섯 파일(ibmphp_core.c, _ebda.c, _res.c, _pci.c, _hpc.c)은 각자
 * 맡은 일이 다르지만 다루는 것은 여기 정의된 같은 구조체들이다.
 *
 * 담고 있는 것을 넷으로 나눌 수 있다.
 *
 *   1. **EBDA 표의 모양.** BIOS 가 EBDA(Extended BIOS Data Area)에 남긴
 *      바이트 배열을 그대로 옮긴 구조체들이다 -- rio_table_hdr,
 *      ebda_hpc_list, ebda_hpc_slot, ebda_hpc_bus, ebda_rsrc_list,
 *      ebda_pci_rsrc. **이 구조체들은 펌웨어가 정한 배치를 따르므로
 *      필드 순서를 바꿀 수 없다.** ibmphp_ebda.c 만이 이것들을 읽는다.
 *
 *   2. **드라이버가 만들어 쓰는 자료구조.** struct slot(슬롯 하나),
 *      struct controller(컨트롤러 하나), struct bus_node 와
 *      struct resource_node(자원 관리), struct pci_func(PCI 함수 하나).
 *      이쪽은 펌웨어와 무관하게 이 드라이버가 정한 모양이다.
 *
 *   3. **하드웨어 프로토콜 상수.** HPC_ 로 시작하는 명령 코드와 상태 비트,
 *      READ_ 로 시작하는 읽기 명령. ibmphp_hpc.c 가 컨트롤러와 주고받는
 *      바이트의 뜻이 전부 여기 정의되어 있다.
 *
 *   4. **상태 비트를 뜻으로 바꾸는 매크로.** SLOT_POWER(s),
 *      CTLR_WORKING(c) 같은 것들이며, 상태 바이트 한 비트를 읽어
 *      이름 있는 값으로 돌려준다. **여러 매크로가 뜻을 뒤집는다** --
 *      SLOT_CONNECT 와 SLOT_LATCH 가 그렇고, 원문 주석이 그 사정을 밝힌다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 헤더는 다섯 파일의 한가운데 있으며, 각 파일이 서로를 부를 때 쓰는
 * 함수 원형도 모두 여기 모여 있다.
 *
 *   ibmphp_ebda.c  -- BIOS 의 EBDA 를 읽어 controller 와 slot 을 만든다.
 *                     이 헤더의 ebda_ 계열 구조체를 유일하게 쓰는 곳이다.
 *   ibmphp_res.c   -- bus_node 와 resource_node 로 IO/메모리/PFMem 를
 *                     관리한다. PCI 코어의 할당기를 쓰지 않고 직접 굴린다.
 *   ibmphp_pci.c   -- 꽂힌 카드의 설정공간을 읽고 자원을 배정한다.
 *                     pci_func 과 res_needed 를 쓴다.
 *   ibmphp_hpc.c   -- 컨트롤러와 실제로 대화한다. HPC_ 와 READ_ 상수,
 *                     그리고 상태 해석 매크로가 이 파일을 위한 것이다.
 *   ibmphp_core.c  -- 핫플러그 코어와 이어지는 진입점. slot 의 상태
 *                     기계를 돌리며 위의 넷을 모두 부른다.
 *
 * 데이터가 흐르는 순서로 보면 이렇다.
 *   BIOS EBDA
 *     -> ibmphp_ebda.c 가 ebda_hpc_list / _slot / _bus / _rsrc 를 읽어
 *        -> struct controller 와 struct slot 을 만들어 전역 목록에 매단다
 *        -> struct ebda_pci_rsrc 목록을 만든다
 *           -> ibmphp_res.c 가 그것으로 bus_node 와 range_node 를 세운다
 *              -> 카드가 꽂히면 ibmphp_pci.c 가 resource_node 를 떼어 쓴다
 *
 * 실행 컨텍스트: 이 헤더 자체는 코드가 아니다. 유일한 실행 코드는 파일
 * 끝의 to_slot() 인라인 함수이며, 어디서든 부를 수 있는 포인터 계산이다.
 *
 * === 타 모듈과의 연결 ===
 * **위쪽**: linux/pci_hotplug.h 의 struct hotplug_slot 이 struct slot 안에
 * 값으로 박혀 있고, to_slot() 이 그 사이를 잇는다. linux/pci_regs.h 는
 * PCI_HEADER_TYPE_ 계열 상수를 위해 들어온다.
 * **이 스파스 체크아웃에는 두 헤더가 모두 없어** 안쪽 정의는 확인할 수 없다.
 *
 * **아래쪽**: 없다. 이 헤더는 다른 드라이버 헤더를 부르지 않는다.
 *
 * **공유 상태**: extern 으로 내보내는 것이 넷이다 --
 * ibmphp_debug(로그 스위치), ibmphp_ebda_pci_rsrc_head 와
 * ibmphp_slot_head(전역 목록 둘), ibmphp_pci_bus(설정공간 접근용 버스),
 * 그리고 ibmphp_hotplug_slot_ops(핫플러그 코어에 넘길 콜백 표).
 *
 * === 주요 함수/구조체 요약 ===
 * - struct slot : 슬롯 하나의 전부. 물리 번호, 능력, 상태 세 바이트,
 *   그리고 소속 컨트롤러와 버스로 가는 고리를 갖는다. 이 드라이버에서
 *   가장 자주 오가는 구조체다.
 * - struct controller : 컨트롤러 하나. ctlr_type 이 네 전송 방식을 가르고,
 *   union u 가 그에 맞는 접근 정보를 담는다.
 * - struct bus_node / struct range_node / struct resource_node :
 *   자원 관리 세 짝. 버스마다 IO/Mem/PFMem 범위를 두고, 그 안에서
 *   자원을 떼어 준다.
 * - struct pci_func : PCI 함수 하나가 쥔 자원과 인터럽트.
 * - struct ebda_* : BIOS 표의 배치를 그대로 옮긴 것들.
 * - SLOT_ 계열 매크로 : 상태 바이트를 뜻으로 바꾼다.
 * - CTLR_ 계열 매크로 : 컨트롤러 상태 바이트를 뜻으로 바꾼다.
 * - NEEDTOCHECK_CMDSTATUS(c) : 명령을 낸 뒤 완료 표시까지 확인해야
 *   하는지 가린다. ibmphp_hpc_writeslot() 의 대기 루프가 이것을 본다.
 * - to_slot() : 핫플러그 코어의 슬롯에서 이 드라이버의 슬롯을 되찾는다.
 *
 * === 이 파일을 읽을 때 알아 두면 좋은 것 ===
 * **주석 형식이 세 가지가 섞여 있다.** 별표로 둘러싼 상자 주석,
 * 하이픈으로 그은 줄, 그리고 `//` 줄 주석이다. 2001년 IBM 내부 코딩
 * 관행의 흔적이며 상류 그대로 보존한다.
 *
 * **주석 처리되어 남아 있는 필드가 둘 있다** -- scal_detail 의
 * scal_detail_list 와 ebda_hpc_list 의 같은 이름 필드다. 목록으로
 * 관리하려다 그만둔 흔적으로 보이나, 그 사정이 코드에 적혀 있지는 않다.
 *
 * **이름에 오타가 하나 있다** -- HPC_CTLR_RESULE2 는 RESULT2 여야
 * 자연스럽다. 다만 그 상수를 쓰는 곳이 없어 드러나지 않는다.
 *
 * **상태 해석 매크로의 여러 개가 뜻을 뒤집는다.** SLOT_CONNECT 는 비트가
 * 서 있으면 "끊김", SLOT_LATCH 는 비트가 서 있으면 "닫힘" 을 돌려준다.
 * 후자에는 원문 주석이 PCI 규격과 반대임을 밝혀 두었다.
 */

/* [한국어] **핫플러그 코어 API.** struct hotplug_slot 과 hotplug_slot_ops 가 여기 있고,
 * struct slot 안에 그 구조체가 값으로 박힌다.
 * **이 스파스 체크아웃에는 없어** 안쪽 배치는 확인할 수 없다 */
#include <linux/pci_hotplug.h>
/* [한국어] PCI 규격 상수. 아래 PCI_HEADER_TYPE_MFD 계열을 조합하는 데 쓴다.
 * **이 트리에 없어** 각 상수의 실제 값은 확인할 수 없다 */
#include <linux/pci_regs.h>

/* [한국어] **로그 스위치. ibmphp_core.c 가 모듈 인자로 받아 정의한다.**
 * 읽는 자: 바로 아래 debug 와 debug_pci 매크로.
 * 값 범위: 0 이면 조용, 1 이면 debug 까지, 그 밖의 참값이면 debug_pci 만.
 * 동기화: 없음. 모듈을 올릴 때 정해지고 바뀌지 않는다 */
extern int ibmphp_debug;

/* [한국어] **커널에 붙박이로 들어갔는가.** 그렇다면 THIS_MODULE 이 없으므로
 * 이름을 문자열로 박아 둔다 */
#if !defined(MODULE)
	/* [한국어] 붙박이일 때의 로그 접두어. **끝의 d 는 데몬(daemon)의 흔적으로 보이나
	 * 그 사정이 코드에 적혀 있지는 않다** */
	#define MY_NAME "ibmphpd"
/* [한국어] 모듈로 빌드된 경우 */
#else
	/* [한국어] **모듈 이름을 실행 중에 읽어 쓴다.**
	 * 모듈 파일명을 바꿔도 로그가 따라가게 하려는 것이다 */
	#define MY_NAME THIS_MODULE->name
/* [한국어] MODULE 갈래의 끝 */
#endif
/* [한국어] **ibmphp_debug 가 정확히 1 일 때만 찍는 디버그 로그.**
 * `do { } while (0)` 으로 감싸는 것은 if 뒤에 세미콜론 없이 써도
 * 문법이 깨지지 않게 하는 커널 관용이다.
 * `arg...` 는 가변 인자를 그대로 넘기는 GNU C 확장이다 */
#define debug(fmt, arg...) do { if (ibmphp_debug == 1) printk(KERN_DEBUG "%s: " fmt, MY_NAME, ## arg); } while (0)
/* [한국어] **ibmphp_debug 가 0 이 아니면 찍는다.**
 * debug 보다 조건이 느슨해 더 자주 나온다 --
 * **두 단계로 나눈 것은 PCI 설정 쪽 로그가 특히 많기 때문으로 보인다** */
#define debug_pci(fmt, arg...) do { if (ibmphp_debug) printk(KERN_DEBUG "%s: " fmt, MY_NAME, ## arg); } while (0)
/* [한국어] 오류 로그. 조건 없이 늘 찍는다 */
#define err(format, arg...) printk(KERN_ERR "%s: " format, MY_NAME, ## arg)
/* [한국어] 정보 로그. 조건 없이 늘 찍는다 */
#define info(format, arg...) printk(KERN_INFO "%s: " format, MY_NAME, ## arg)
/* [한국어] 경고 로그. 조건 없이 늘 찍는다 */
#define warn(format, arg...) printk(KERN_WARNING "%s: " format, MY_NAME, ## arg)


/* EBDA stuff */

/***********************************************************
* SLOT CAPABILITY                                          *
***********************************************************/

/* [한국어] **슬롯 능력 바이트(ebda_hpc_slot 의 slot_cap)의 비트.**
 * 133MHz PCI-X 까지 낼 수 있는 슬롯.
 * 읽는 자: ibmphp_ebda.c 가 슬롯을 만들며 supported_speed 를 정할 때 */
#define EBDA_SLOT_133_MAX		0x20
/* [한국어] 100MHz PCI-X 까지 낼 수 있는 슬롯 */
#define EBDA_SLOT_100_MAX		0x10
/* [한국어] 66MHz 까지 낼 수 있는 슬롯 */
#define EBDA_SLOT_66_MAX		0x02
/* [한국어] **PCI-X 모드를 지원하는 슬롯.**
 * 속도 비트와 별개이며, supported_bus_mode 를 정하는 데 쓴다 */
#define EBDA_SLOT_PCIX_CAP		0x08


/************************************************************
*  RESOURCE TYPE                                             *
************************************************************/

/* [한국어] **EBDA 자원 항목의 종류 필드를 뽑는 마스크(하위 2비트).**
 * 읽는 자: ibmphp_ebda.c 의 ebda_rsrc_rsrc().
 * 헤더 아래쪽의 RESTYPE 과 값이 같으나 쓰이는 자리가 다르다 */
#define EBDA_RSRC_TYPE_MASK		0x03
/* [한국어] 0 -- IO 공간 자원 */
#define EBDA_IO_RSRC_TYPE		0x00
/* [한국어] 1 -- 일반 메모리 자원 */
#define EBDA_MEM_RSRC_TYPE		0x01
/* [한국어] 3 -- prefetchable 메모리 자원.
 * **2 를 건너뛰고 3 인 것이 눈에 띄는데**, 2 는 바로 아래 예약 종류다 */
#define EBDA_PFM_RSRC_TYPE		0x03
/* [한국어] 2 -- 예약된 자원. 이 드라이버가 나눠 주지 않는다 */
#define EBDA_RES_RSRC_TYPE		0x02


/*************************************************************
*  IO RESTRICTION TYPE                                       *
*************************************************************/

/* [한국어] **IO 제한 필드를 뽑는 마스크(비트 2~3).**
 * 파일 아래쪽 주석이 밝히듯 **이 제한은 주 버스에서만 뜻이 있다** */
#define EBDA_IO_RESTRI_MASK		0x0c
/* [한국어] 제한 없음 */
#define EBDA_NO_RESTRI			0x00
/* [한국어] VGA 주소를 피하라 */
#define EBDA_AVO_VGA_ADDR		0x04
/* [한국어] VGA 주소와 그 별칭까지 피하라.
 * **ISA 시절 별칭 때문에 같은 장치가 여러 주소에 보이던 사정에서 온다** */
#define EBDA_AVO_VGA_ADDR_AND_ALIA	0x08
/* [한국어] ISA 주소를 피하라 */
#define EBDA_AVO_ISA_ADDR		0x0c


/**************************************************************
*  DEVICE TYPE DEF                                            *
**************************************************************/

/* [한국어] **PCI 장치인지 아닌지를 뽑는 마스크(비트 4)** */
#define EBDA_DEV_TYPE_MASK		0x10
/* [한국어] PCI 장치다 */
#define EBDA_PCI_DEV			0x10
/* [한국어] PCI 장치가 아니다. **이 드라이버는 다루지 않는다** */
#define EBDA_NON_PCI_DEV		0x00


/***************************************************************
*  PRIMARY DEF DEFINITION                                      *
***************************************************************/

/* [한국어] **이 항목이 주 버스 정보인지 뽑는 마스크(비트 5)** */
#define EBDA_PRI_DEF_MASK		0x20
/* [한국어] 주 PCI 버스 정보다. **버스가 쓸 수 있는 주소 범위를 알려 준다** */
#define EBDA_PRI_PCI_BUS_INFO		0x20
/* [한국어] 보통 장치의 자원 정보다. **이미 쓰이고 있는 자원을 알려 준다** */
#define EBDA_NORM_DEV_RSRC_INFO		0x00


//--------------------------------------------------------------
// RIO TABLE DATA STRUCTURE
//--------------------------------------------------------------

/* [한국어] **RIO(Remote I/O) 표의 머리. BIOS 가 EBDA 에 남긴 배치를 그대로 옮긴 것이다.**
 * RIO 는 여러 섀시를 케이블로 이어 확장하는 IBM 의 구성이며,
 * 이 표가 그 위상을 알려 준다.
 * 읽는 자: ibmphp_ebda.c 의 ebda_rio_table() 하나뿐이다.
 * **필드 순서를 바꿀 수 없다** -- 펌웨어가 정한 바이트 배치이기 때문이다 */
struct rio_table_hdr {
	/* [한국어] 표의 판 번호.
	 * 설정자: 시스템 펌웨어가 EBDA 에 써 둔다.
	 * 읽는 자: ibmphp_ebda.c 가 읽어 두지만 갈래를 나누지는 않는다.
	 * 동기화: 부팅 뒤 불변 */
	u8 ver_num;
	/* [한국어] **뒤따르는 scal_detail 항목의 개수.**
	 * 읽는 자: ibmphp_ebda.c 가 그 횟수만큼 반복해 확장성 정보를 읽는다.
	 * 동기화: 불변 */
	u8 scal_count;
	/* [한국어] **뒤따르는 rio_detail 항목의 개수.**
	 * 읽는 자: ibmphp_ebda.c 가 그 횟수만큼 반복해 RIO 장치 정보를 읽는다.
	 * 동기화: 불변 */
	u8 riodev_count;
	/* [한국어] **두 항목 배열이 시작하는 자리까지의 오프셋.**
	 * 읽는 자: ibmphp_ebda.c 가 표 시작 주소에 더해 첫 항목을 찾는다.
	 * 동기화: 불변 */
	u16 offset;
};

//-------------------------------------------------------------
// SCALABILITY DETAIL
//-------------------------------------------------------------

/* [한국어] **확장성(scalability) 상세 -- 노드 하나가 어느 포트로 무엇과 이어져 있는지.**
 * 여러 섀시를 잇는 구성에서 그 연결 관계를 담는다.
 * 읽는 자: ibmphp_ebda.c 의 ebda_rio_table().
 * **이 드라이버가 실제로 쓰는 필드는 거의 없다** -- 읽어서 디버그로
 * 찍는 것이 대부분이다 */
struct scal_detail {
	/* [한국어] 이 노드의 번호.
	 * 설정자: 펌웨어. 읽는 자: ibmphp_ebda.c 의 디버그 출력.
	 * 동기화: 불변 */
	u8 node_id;
	/* [한국어] **Configuration Base Address Register.**
	 * 이 노드의 설정 공간이 시작하는 주소로 보이나,
	 * **이 드라이버가 그 값을 쓰는 곳을 찾을 수 없다.**
	 * 동기화: 불변 */
	u32 cbar;
	/* [한국어] 0번 포트가 이어진 상대 노드 번호.
	 * 읽는 자: ibmphp_ebda.c 의 디버그 출력.
	 * 동기화: 불변 */
	u8 port0_node_connect;
	/* [한국어] 0번 포트가 이어진 상대의 포트 번호.
	 * **노드와 포트를 짝으로 두어야 연결이 결정된다** */
	u8 port0_port_connect;
	/* [한국어] 1번 포트가 이어진 상대 노드 번호 */
	u8 port1_node_connect;
	/* [한국어] 1번 포트가 이어진 상대의 포트 번호 */
	u8 port1_port_connect;
	/* [한국어] 2번 포트가 이어진 상대 노드 번호.
	 * **포트가 셋인 것이 scal_detail 과 rio_detail 의 차이다** --
	 * 그쪽은 둘뿐이다 */
	u8 port2_node_connect;
	/* [한국어] 2번 포트가 이어진 상대의 포트 번호 */
	u8 port2_port_connect;
	/* [한국어] 이 노드가 속한 섀시 번호.
	 * **여러 섀시 구성에서 슬롯 번호가 겹치지 않게 하는 열쇠다.**
	 * 동기화: 불변 */
	u8 chassis_num;
//	struct list_head scal_detail_list;
};

//--------------------------------------------------------------
// RIO DETAIL
//--------------------------------------------------------------

/* [한국어] **RIO 장치 상세 -- 확장 상자 하나의 정보.**
 * 읽는 자: ibmphp_ebda.c 가 읽어 목록에 매달고,
 * 그 목록으로 슬롯의 실제 물리 번호를 계산한다.
 * **scal_detail 과 달리 이쪽은 list_head 를 갖고 실제로 목록이 된다** */
struct rio_detail {
	/* [한국어] 이 RIO 장치의 노드 번호.
	 * 읽는 자: ibmphp_ebda.c 가 확장 상자를 짝지을 때.
	 * 동기화: 불변 */
	u8 rio_node_id;
	/* [한국어] **Bus Base Address Register.**
	 * **이 드라이버가 값을 쓰는 곳을 찾을 수 없다.** 동기화: 불변 */
	u32 bbar;
	/* [한국어] **장치 종류.** ibmphp_ebda.c 가 이 값으로 확장 상자인지 아닌지를
	 * 가려 opt_rio 나 opt_rio_lo 목록에 나눠 담는다.
	 * 동기화: 불변 */
	u8 rio_type;
	/* [한국어] 이 장치를 소유한 노드의 번호.
	 * 읽는 자: ibmphp_ebda.c 가 어느 섀시에 속하는지 정할 때.
	 * 동기화: 불변 */
	u8 owner_id;
	/* [한국어] 0번 포트가 이어진 상대 노드 번호 */
	u8 port0_node_connect;
	/* [한국어] 0번 포트가 이어진 상대의 포트 번호 */
	u8 port0_port_connect;
	/* [한국어] 1번 포트가 이어진 상대 노드 번호 */
	u8 port1_node_connect;
	/* [한국어] 1번 포트가 이어진 상대의 포트 번호.
	 * **scal_detail 과 달리 포트가 둘뿐이다** */
	u8 port1_port_connect;
	/* [한국어] **이 확장 상자의 첫 슬롯 번호.**
	 * 읽는 자: ibmphp_ebda.c 가 슬롯의 real_physical_slot_num 을 계산할 때
	 *   기준으로 삼는다.
	 * **여러 상자를 이어 붙였을 때 번호가 겹치지 않게 하는 값이다.**
	 * 동기화: 불변 */
	u8 first_slot_num;
	/* [한국어] 이 장치의 상태.
	 * **이 드라이버가 읽어 갈래를 나누는 곳을 찾을 수 없다.** 동기화: 불변 */
	u8 status;
	/* [한국어] **확장 상자 안에서의 순번으로 보인다.**
	 * 읽는 자: ibmphp_ebda.c 가 디버그로 찍는다.
	 * **그 이름의 뜻과 정확한 쓰임은 EBDA 규격 문서에 있고 이 트리에 없다.**
	 * 동기화: 불변 */
	u8 wpindex;
	/* [한국어] 이 장치가 속한 섀시 번호.
	 * 읽는 자: ibmphp_ebda.c 가 슬롯 번호를 계산할 때.
	 * 동기화: 불변 */
	u8 chassis_num;
	/* [한국어] **전역 rio_vg_head 목록에 매달릴 고리.**
	 * 설정자: ibmphp_ebda.c 가 항목을 읽어 만들며 매단다.
	 * 읽는 자: 같은 파일이 슬롯 번호를 계산하며 훑는다.
	 * 동기화: 없음. 부팅 때 만들어진 뒤 바뀌지 않는다 */
	struct list_head rio_detail_list;
};

/* [한국어] **같은 종류·섀시의 RIO 장치를 묶어 두는 항목.**
 * 설정자·읽는 자: ibmphp_ebda.c 뿐이다.
 * **왜 필요한가**: 확장 상자가 여럿이면 슬롯 번호를 이어 붙여야 하는데,
 *   그러려면 같은 무리의 장치를 먼저 모아 세어야 한다 */
struct opt_rio {
	/* [한국어] 이 무리의 장치 종류. rio_detail 의 같은 이름 필드에서 온다.
	 * 동기화: 불변 */
	u8 rio_type;
	/* [한국어] 이 무리가 속한 섀시 번호.
	 * **종류와 섀시가 같으면 한 무리다** -- 이 둘이 무리의 열쇠다.
	 * 동기화: 불변 */
	u8 chassis_num;
	/* [한국어] 이 무리의 첫 슬롯 번호.
	 * 동기화: 불변 */
	u8 first_slot_num;
	/* [한국어] **무리 안에서의 중간 번호.**
	 * 설정자: ibmphp_ebda.c 가 같은 종류의 장치를 세며 채운다.
	 * **정확한 뜻은 EBDA 규격 문서에 있고 이 트리에서 확인할 수 없다** --
	 *   코드는 슬롯 번호를 계산할 때 이 값을 더한다.
	 * 동기화: 불변 */
	u8 middle_num;
	/* [한국어] opt_vg_head 목록에 매달릴 고리.
	 * 설정자·읽는 자: ibmphp_ebda.c. 동기화: 없음 */
	struct list_head opt_rio_list;
};

/* [한국어] **opt_rio 와 거의 같으나 필드가 하나 더 있다.**
 * **두 구조체를 나눈 이유는 코드에 적혀 있지 않으나**, ibmphp_ebda.c 가
 * 서로 다른 rio_type 무리를 두 목록으로 나눠 관리하는 데 쓴다 */
struct opt_rio_lo {
	/* [한국어] 이 무리의 장치 종류. 동기화: 불변 */
	u8 rio_type;
	/* [한국어] 이 무리가 속한 섀시 번호. 동기화: 불변 */
	u8 chassis_num;
	/* [한국어] 이 무리의 첫 슬롯 번호. 동기화: 불변 */
	u8 first_slot_num;
	/* [한국어] 무리 안에서의 중간 번호. opt_rio 의 같은 필드와 같은 방식이다 */
	u8 middle_num;
	/* [한국어] **이 무리에 묶인 장치의 개수.**
	 * 설정자: ibmphp_ebda.c 가 같은 무리를 세며 늘린다.
	 * **opt_rio 에는 없는 필드이며, 그것이 두 구조체를 나눈 이유로 보인다.**
	 * 동기화: 불변 */
	u8 pack_count;
	/* [한국어] opt_lo_head 목록에 매달릴 고리. 동기화: 없음 */
	struct list_head opt_rio_lo_list;
};

/****************************************************************
*  HPC DESCRIPTOR NODE                                          *
****************************************************************/

/* [한국어] **EBDA 의 핫플러그 컨트롤러 목록 머리.**
 * 읽는 자: ibmphp_ebda.c 의 ebda_rsrc_controller() 가 이것을 읽어
 *   컨트롤러가 몇 개인지, 그 정보가 어디에 있는지 알아낸다.
 * **BIOS 가 정한 바이트 배치를 그대로 옮긴 것이다** */
struct ebda_hpc_list {
	/* [한국어] 표의 형식 번호.
	 * 설정자: 펌웨어. 읽는 자: ibmphp_ebda.c 가 읽어 두지만 갈래를 나누지는 않는다.
	 * 동기화: 불변 */
	u8 format;
	/* [한국어] **이 시스템의 핫플러그 컨트롤러 개수.**
	 * 읽는 자: ibmphp_ebda.c 가 그 횟수만큼 반복해 컨트롤러를 만들고,
	 *   ibmphp_get_total_controllers() 가 그 값을 돌려준다.
	 *   ibmphp_hpc.c 의 폴링 스레드가 컨트롤러를 다 봤는지 판단할 때 쓴다.
	 * 동기화: 불변 */
	u16 num_ctlrs;
	/* [한국어] **컨트롤러 정보 배열이 있는 물리 주소.**
	 * **short 인 것이 눈에 띈다** -- 부호 있는 16비트라 0x8000 이상이면
	 *   음수가 되는데, 코드는 그것을 EBDA 시작 주소에 더해 쓴다.
	 *   코드는 손대지 않고 사실만 적는다.
	 * 동기화: 불변 */
	short phys_addr;
//      struct list_head ebda_hpc_list;
};
/*****************************************************************
*   IN HPC DATA STRUCTURE, THE ASSOCIATED SLOT AND BUS           *
*   STRUCTURE                                                    *
*****************************************************************/

/* [한국어] **컨트롤러가 담당하는 슬롯 하나의 EBDA 정보.**
 * 읽는 자: ibmphp_ebda.c 가 컨트롤러마다 slot_count 만큼 읽어
 *   controller->slots 배열에 담고, 그것으로 struct slot 을 만든다 */
struct ebda_hpc_slot {
	/* [한국어] **이 슬롯의 물리 번호.**
	 * 읽는 자: ibmphp_ebda.c 가 struct slot 의 number 로 옮기고,
	 *   RIO 구성이면 real_physical_slot_num 을 따로 계산한다.
	 * 동기화: 불변 */
	u8 slot_num;
	/* [한국어] 이 슬롯이 붙어 있는 버스 번호.
	 * 읽는 자: ibmphp_ebda.c 가 struct slot 의 bus 로 옮긴다.
	 * **u32 이지만 담기는 값은 버스 번호라 8비트로 충분하다.**
	 * 동기화: 불변 */
	u32 slot_bus_num;
	/* [한국어] **이 슬롯의 컨트롤러 안 인덱스.**
	 * 읽는 자: ibmphp_ebda.c 가 struct slot 의 ctlr_index 로 옮기고,
	 *   ibmphp_hpc.c 가 명령을 낼 때 그 값을 인덱스 변환 함수에 넘긴다.
	 * **물리 번호와 다르다** -- 이쪽은 컨트롤러가 아는 번호다.
	 * 동기화: 불변 */
	u8 ctl_index;
	/* [한국어] **이 슬롯의 능력 비트 모음.**
	 * 값 범위: EBDA_SLOT_133_MAX / _100_MAX / _66_MAX / _PCIX_CAP 조합.
	 * 읽는 자: ibmphp_ebda.c 가 struct slot 의 supported_speed 와
	 *   supported_bus_mode 를 정할 때.
	 * 동기화: 불변 */
	u8 slot_cap;
};

/* [한국어] **컨트롤러가 담당하는 버스 하나의 EBDA 정보.**
 * 읽는 자: ibmphp_ebda.c 가 컨트롤러마다 bus_count 만큼 읽어
 *   controller->buses 배열에 담고, struct bus_info 를 만든다.
 * **속도별 슬롯 수를 담는 것이 이 구조체의 요점이다** --
 *   PCI-X 는 버스에 꽂힌 카드 수와 속도가 얽혀 있어 그 계산에 필요하다 */
struct ebda_hpc_bus {
	/* [한국어] 이 버스의 번호.
	 * 읽는 자: ibmphp_ebda.c 가 struct bus_info 의 busno 로 옮긴다.
	 * 동기화: 불변 */
	u32 bus_num;
	/* [한국어] **33MHz 통상 모드로 돌 때 쓸 수 있는 슬롯 수.**
	 * 읽는 자: ibmphp_ebda.c 가 bus_info 로 옮기고,
	 *   ibmphp_core.c 가 카드를 올릴 때 버스 속도를 정하며 본다.
	 * 동기화: 불변 */
	u8 slots_at_33_conv;
	/* [한국어] 66MHz 통상 모드로 돌 때 쓸 수 있는 슬롯 수 */
	u8 slots_at_66_conv;
	/* [한국어] 66MHz PCI-X 모드로 돌 때 쓸 수 있는 슬롯 수 */
	u8 slots_at_66_pcix;
	/* [한국어] 100MHz PCI-X 모드로 돌 때 쓸 수 있는 슬롯 수 */
	u8 slots_at_100_pcix;
	/* [한국어] **133MHz PCI-X 모드로 돌 때 쓸 수 있는 슬롯 수.**
	 * 다섯 값이 함께 있는 이유가 여기서 드러난다 -- 빠를수록 꽂을 수 있는
	 * 카드가 적어지므로, 속도를 정하려면 다섯을 모두 알아야 한다 */
	u8 slots_at_133_pcix;
};


/********************************************************************
*   THREE TYPE OF HOT PLUG CONTROLLER                                *
********************************************************************/

/* [한국어] **ctlr_type 0 -- ISA IO 포트에 붙은 컨트롤러의 접근 정보.**
 * controller 의 union u 세 갈래 중 하나이며,
 * ctlr_type 이 0 일 때만 유효하다.
 * 읽는 자: ibmphp_hpc.c 의 isa_ctrl_read / isa_ctrl_write */
struct isa_ctlr_access {
	/* [한국어] **IO 포트 영역의 시작 주소.**
	 * 설정자: ibmphp_ebda.c 가 EBDA 에서 읽어 넣는다.
	 * 읽는 자: ibmphp_hpc.c 가 여기에 오프셋을 더해 inb/outb 한다.
	 * 동기화: 불변 */
	u16 io_start;
	/* [한국어] IO 포트 영역의 끝 주소.
	 * **ibmphp_hpc.c 가 이 값을 쓰는 곳을 찾을 수 없다** --
	 *   범위 검사를 하지 않고 오프셋을 그대로 더한다.
	 * 동기화: 불변 */
	u16 io_end;
};

/* [한국어] **ctlr_type 1 -- PCI 장치로 붙은 컨트롤러의 접근 정보.**
 * **다만 ibmphp_hpc.c 는 이 갈래를 쓰지 않고**
 * controller 의 ctrl_dev(struct pci_dev 포인터)로 설정공간에 접근한다.
 * 그 포인터는 ibmphp_ebda.c 가 아래 두 필드로 장치를 찾아 넣어 둔 것이다 */
struct pci_ctlr_access {
	/* [한국어] 컨트롤러가 있는 PCI 버스 번호.
	 * 설정자: ibmphp_ebda.c 가 EBDA 에서 읽는다.
	 * 읽는 자: 같은 파일이 pci_get_domain_bus_and_slot 으로 장치를 찾을 때.
	 * 동기화: 불변 */
	u8 bus;
	/* [한국어] **장치·함수 번호가 합쳐진 한 바이트.**
	 * 상위 5비트가 장치, 하위 3비트가 함수라는 PCI 규격의 배치다.
	 * 읽는 자: ibmphp_ebda.c 가 장치를 찾을 때.
	 * 동기화: 불변 */
	u8 dev_fun;
};

/* [한국어] **ctlr_type 2 와 4 -- Winnipeg I2C 뒤에 있는 컨트롤러의 접근 정보.**
 * 읽는 자: ibmphp_hpc.c 가 창을 ioremap 하고 I2C 트랜잭션을 만들 때.
 * **두 종류가 같은 갈래를 쓰고 i2c_addr 의 쓰임만 다르다** --
 * type 2 는 주소를 실어 보내고 type 4 는 인덱스만 보낸다 */
struct wpeg_i2c_ctlr_access {
	/* [한국어] **Winnipeg 레지스터 창의 물리 주소.**
	 * 설정자: ibmphp_ebda.c 가 EBDA 에서 읽는다.
	 * 읽는 자: ibmphp_hpc.c 의 readslot/writeslot 이 읽기·쓰기마다
	 *   ioremap 했다가 곧 iounmap 한다 -- 오래 들고 있지 않는 방식이다.
	 * 동기화: 불변 */
	ulong wpegbbar;
	/* [한국어] **I2C 버스에서의 컨트롤러 주소.**
	 * 읽는 자: ibmphp_hpc.c 의 i2c_ctrl_read/write 가 ctlr_type 2 일 때만 쓴다.
	 *   그때 오른쪽으로 한 비트 밀어 순수한 7비트 주소를 얻는다 --
	 *   I2C 규격이 주소를 왼쪽으로 한 칸 밀어 싣기 때문이다.
	 * 동기화: 불변 */
	u8 i2c_addr;
};

/* [한국어] **핫플러그 컨트롤러의 PCI 장치 ID.**
 * 읽는 자: ibmphp_ebda.c 가 ctlr_type 1 인 컨트롤러를 찾을 때 */
#define HPC_DEVICE_ID		0x0246
/* [한국어] 그 컨트롤러의 서브시스템 ID. 같은 자리에서 함께 확인한다 */
#define HPC_SUBSYSTEM_ID	0x0247
/* [한국어] **설정공간에서 컨트롤러 레지스터가 시작하는 오프셋.**
 * 표준 헤더(0x00~0x3F) 바로 뒤의 벤더 고유 영역이다.
 * 읽는 자: ibmphp_hpc.c 의 pci_ctrl_read / pci_ctrl_write 가
 *   이 값에 오프셋을 더해 접근한다 */
#define HPC_PCI_OFFSET		0x40
/*************************************************************************
*   RSTC DESCRIPTOR NODE                                                 *
*************************************************************************/

/* [한국어] **EBDA 의 자원 목록 머리.**
 * 읽는 자: ibmphp_ebda.c 의 ebda_rsrc_rsrc().
 * **ebda_hpc_list 와 달리 next 포인터가 있어 여러 블록이 이어질 수 있다** --
 * 원문 주석이 아래쪽에서 밝히듯 주 버스마다 자원 블록이 여럿일 수 있기 때문이다 */
struct ebda_rsrc_list {
	/* [한국어] 표의 형식 번호. 동기화: 불변 */
	u8 format;
	/* [한국어] **이 블록에 든 자원 항목의 개수.**
	 * 읽는 자: ibmphp_ebda.c 가 그 횟수만큼 ebda_pci_rsrc 를 만든다.
	 * 동기화: 불변 */
	u16 num_entries;
	/* [한국어] 자원 항목 배열이 있는 물리 주소.
	 * **ebda_hpc_list 의 같은 뜻 필드가 short 인 것과 달리 u16 이다.**
	 * 동기화: 불변 */
	u16 phys_addr;
	/* [한국어] **다음 자원 블록.** NULL 이면 마지막이다.
	 * 읽는 자: ibmphp_ebda.c 가 이 고리를 따라 모든 블록을 훑는다.
	 * 동기화: 불변 */
	struct ebda_rsrc_list *next;
};


/***************************************************************************
*   PCI RSRC NODE                                                          *
***************************************************************************/

/* [한국어] **EBDA 가 알려 주는 자원 항목 하나.**
 * **이 드라이버 자원 관리의 출발점이다** -- 여기 담긴 정보로
 * ibmphp_res.c 가 bus_node 와 range_node 를 세운다.
 * 설정자: ibmphp_ebda.c 가 EBDA 를 읽어 만들고 전역 목록에 매단다.
 * 읽는 자: ibmphp_res.c 의 ibmphp_rsrc_init() */
struct ebda_pci_rsrc {
	/* [한국어] **자원의 종류와 성질을 담은 바이트.**
	 * 아래쪽 원문 주석이 비트 배치를 그림으로 밝힌다 --
	 *   비트 0~1 종류(IO/Mem/PFMem), 비트 2~3 IO 제한,
	 *   비트 4 PCI 장치 여부, 비트 5 주 버스 정보 여부.
	 * 읽는 자: ibmphp_res.c 가 EBDA_RSRC_TYPE_MASK 계열로 갈래를 나눌 때.
	 * 동기화: 불변 */
	u8 rsrc_type;
	/* [한국어] 이 자원이 속한 버스 번호.
	 * 읽는 자: ibmphp_res.c 가 bus_node 를 찾거나 만들 때.
	 * 동기화: 불변 */
	u8 bus_num;
	/* [한국어] 이 자원을 쓰는 장치·함수 번호.
	 * **주 버스 정보 항목에서는 뜻이 없다** -- 그때는 장치가 아니라
	 *   버스 전체의 범위를 나타내기 때문이다.
	 * 동기화: 불변 */
	u8 dev_fun;
	/* [한국어] 자원 구간의 시작 주소(또는 IO 포트 번호).
	 * 읽는 자: ibmphp_res.c 가 range_node 나 resource_node 를 만들 때.
	 * 동기화: 불변 */
	u32 start_addr;
	/* [한국어] **자원 구간의 끝 주소. 마지막 바이트를 가리키며 그 다음이 아니다.**
	 * ibmphp_res.c 가 길이를 구할 때 `end - start + 1` 로 계산한다.
	 * 동기화: 불변 */
	u32 end_addr;
	/* [한국어] **NVRAM 저장용 표시.**
	 * 원문 주석이 for NVRAM 이라 밝힌다.
	 * 설정자: ibmphp_ebda.c 가 구조체를 만들며 0 으로 둔다.
	 * **세우거나 읽어 갈래를 나누는 코드를 이 트리에서 찾을 수 없다** --
	 *   NVRAM 에 상태를 되쓰는 기능이 미완으로 남은 흔적으로 보인다.
	 * 동기화: 없음 */
	u8 marked;	/* for NVRAM */
	/* [한국어] **전역 ibmphp_ebda_pci_rsrc_head 목록에 매달릴 고리.**
	 * 설정자: ibmphp_ebda.c. 읽는 자: ibmphp_res.c 의 초기화.
	 * 동기화: 없음. 부팅 때 만들어진 뒤 바뀌지 않는다 */
	struct list_head ebda_pci_rsrc_list;
};


/***********************************************************
* BUS_INFO DATE STRUCTURE                                  *
***********************************************************/

/* [한국어] **버스 하나의 속도·모드 정보와 슬롯 범위.**
 * 설정자: ibmphp_ebda.c 가 ebda_hpc_bus 를 읽어 만들고 목록에 매단다.
 * 읽는 자: ibmphp_core.c 가 카드를 올릴 때 버스 속도를 정하며,
 *   ibmphp_ebda.c 의 ibmphp_find_same_bus_num() 이 번호로 찾는다.
 * **struct bus_node 와 이름이 비슷하나 전혀 다르다** --
 *   이쪽은 속도·모드, 그쪽은 주소 범위와 자원이다 */
struct bus_info {
	/* [한국어] 이 버스의 가장 작은 슬롯 번호.
	 * 설정자: ibmphp_ebda.c 가 슬롯을 훑으며 갱신한다.
	 * 동기화: 부팅 뒤 불변 */
	u8 slot_min;
	/* [한국어] 이 버스의 가장 큰 슬롯 번호.
	 * **min 과 max 로 이 버스에 어느 슬롯이 속하는지 가린다** */
	u8 slot_max;
	/* [한국어] 이 버스에 붙은 슬롯 개수.
	 * 읽는 자: ibmphp_core.c 가 버스 속도를 정할 때 꽂힌 카드 수와 견준다 */
	u8 slot_count;
	/* [한국어] **이 버스의 번호. 목록에서 찾는 열쇠다.**
	 * 읽는 자: ibmphp_find_same_bus_num(), ibmphp_get_bus_index().
	 * 동기화: 불변 */
	u8 busno;
	/* [한국어] 이 버스를 담당하는 컨트롤러의 ID.
	 * 설정자: ibmphp_ebda.c. 동기화: 불변 */
	u8 controller_id;
	/* [한국어] **지금 이 버스가 실제로 돌고 있는 속도.**
	 * 설정자: ibmphp_core.c 가 카드를 올리며 갱신하고,
	 *   ibmphp_ebda.c 가 처음에 채운다.
	 * 값 범위: BUS_SPEED_33 / _66 / _100 / _133 등.
	 * 동기화: operations_mutex 를 쥔 쪽이 바꾼다 */
	u8 current_speed;
	/* [한국어] **지금 이 버스가 PCI-X 모드인가 통상 PCI 모드인가.**
	 * 값 범위: BUS_MODE_PCI(0) 또는 BUS_MODE_PCIX(1).
	 * 설정자·읽는 자: current_speed 와 같다.
	 * 동기화: operations_mutex */
	u8 current_bus_mode;
	/* [한국어] **컨트롤러 안에서 이 버스가 몇 번째인가.**
	 * 읽는 자: ibmphp_get_bus_index() 가 이 값을 돌려주고,
	 *   ibmphp_hpc.c 가 버스 명령의 인덱스로 쓴다.
	 * 동기화: 불변 */
	u8 index;
	/* [한국어] 33MHz 통상 모드로 돌 때 쓸 수 있는 슬롯 수.
	 * ebda_hpc_bus 의 같은 이름 필드에서 복사해 온다 */
	u8 slots_at_33_conv;
	/* [한국어] 66MHz 통상 모드로 돌 때 쓸 수 있는 슬롯 수 */
	u8 slots_at_66_conv;
	/* [한국어] 66MHz PCI-X 모드로 돌 때 쓸 수 있는 슬롯 수 */
	u8 slots_at_66_pcix;
	/* [한국어] 100MHz PCI-X 모드로 돌 때 쓸 수 있는 슬롯 수 */
	u8 slots_at_100_pcix;
	/* [한국어] **133MHz PCI-X 모드로 돌 때 쓸 수 있는 슬롯 수.**
	 * 다섯 값을 함께 보아야 지금 꽂힌 카드 수로 낼 수 있는 최고 속도가 정해진다 */
	u8 slots_at_133_pcix;
	/* [한국어] 전역 bus_info_head 목록에 매달릴 고리.
	 * 설정자: ibmphp_ebda.c. 동기화: 없음 */
	struct list_head bus_info_list;
};


/***********************************************************
* GLOBAL VARIABLES                                         *
***********************************************************/
/* [한국어] **EBDA 가 알려 준 자원 항목의 전역 목록.**
 * 정의: ibmphp_ebda.c.
 * 설정자: 같은 파일이 EBDA 를 읽으며 채운다.
 * 읽는 자: ibmphp_res.c 의 ibmphp_rsrc_init() 이 이것으로 자원 관리를 세운다.
 * 동기화: 없음. 부팅 때 만들어진 뒤 바뀌지 않는다 */
extern struct list_head ibmphp_ebda_pci_rsrc_head;
/* [한국어] **모든 슬롯의 전역 목록.**
 * 정의: ibmphp_ebda.c.
 * 설정자: 같은 파일이 슬롯을 만들며 매단다.
 * 읽는 자: ibmphp_hpc.c 의 폴링 스레드가 2초마다 훑고,
 *   ibmphp_core.c 가 슬롯을 찾을 때 쓴다.
 * 동기화: operations_mutex */
extern struct list_head ibmphp_slot_head;
/***********************************************************
* FUNCTION PROTOTYPES                                      *
***********************************************************/

/* [한국어] **아래는 ibmphp_ebda.c 가 제공하는 함수들이다.**
 * 컨트롤러 목록과 그에 딸린 슬롯·버스 배열을 해제한다 */
void ibmphp_free_ebda_hpc_queue(void);
/* [한국어] **EBDA 를 통째로 읽어 컨트롤러와 슬롯을 만든다.**
 * 이 드라이버 초기화의 첫 단계이며 ibmphp_core.c 가 부른다 */
int ibmphp_access_ebda(void);
/* [한국어] 물리 슬롯 번호로 슬롯을 찾는다.
 * 읽는 자: ibmphp_hpc.c 의 process_changeinlatch() 가
 *   래치 비트 자리를 슬롯으로 바꿀 때 */
struct slot *ibmphp_get_slot_from_physical_num(u8);
/* [한국어] bus_info 목록을 해제한다 */
void ibmphp_free_bus_info_queue(void);
/* [한국어] ebda_pci_rsrc 목록을 해제한다 */
void ibmphp_free_ebda_pci_rsrc_queue(void);
/* [한국어] 버스 번호로 bus_info 를 찾는다.
 * 읽는 자: ibmphp_core.c 가 버스 속도를 정할 때 */
struct bus_info *ibmphp_find_same_bus_num(u32);
/* [한국어] **버스 번호를 컨트롤러 안 버스 인덱스로 옮긴다.**
 * 읽는 자: ibmphp_hpc.c 의 readslot/writeslot 이 버스 명령을 낼 때.
 * **음수로 실패를 알리므로 반환형이 int 다** */
int ibmphp_get_bus_index(u8);
/* [한국어] 컨트롤러 개수를 돌려준다.
 * 읽는 자: ibmphp_hpc.c 의 폴링 스레드가 컨트롤러를 다 봤는지 판단할 때 */
u16 ibmphp_get_total_controllers(void);
/* [한국어] 이 드라이버를 PCI 드라이버로 등록한다.
 * **ctlr_type 1 인 컨트롤러를 찾기 위한 것이다** */
int ibmphp_register_pci(void);

/* passed parameters */
/* [한국어] **자원 종류를 나타내는 세 값. 여기부터다.**
 * 0 -- 일반 메모리.
 * 읽는 자: ibmphp_res.c 와 _pci.c 가 resource_node 의 type 필드로 쓴다.
 * 원문 주석: passed parameters */
#define MEM		0
/* [한국어] 1 -- IO 공간 */
#define IO		1
/* [한국어] **2 -- prefetchable 메모리.**
 * **EBDA 쪽 종류 값(EBDA_PFM_RSRC_TYPE 은 3)과 다르다** --
 * 이쪽은 드라이버 내부 표현이고 저쪽은 펌웨어 표현이다 */
#define PFMEM		2

/* bit masks */
/* [한국어] **EBDA 자원 종류 필드를 뽑는 마스크(하위 2비트).**
 * 위쪽의 EBDA_RSRC_TYPE_MASK 와 값이 같으나 쓰이는 파일이 다르다 --
 * 이쪽은 ibmphp_res.c 와 _ebda.c 가 쓴다.
 * 원문 주석: bit masks */
#define RESTYPE		0x03
/* [한국어] **IO 종류를 나타내는 값(0).**
 * 원문 주석이 밝히듯 **보수를 취해야 쓸 수 있다** --
 * 0 과 AND 하면 늘 0 이라 그대로는 검사가 되지 않기 때문이다 */
#define IOMASK		0x00	/* will need to take its complement */
/* [한국어] 일반 메모리 종류를 나타내는 값(1) */
#define MMASK		0x01
/* [한국어] prefetchable 메모리 종류를 나타내는 값(3) */
#define PFMASK		0x03
/* [한국어] **PCI 장치인지 뽑는 마스크(비트 4).**
 * 원문 주석이 밝히듯 이 드라이버가 다루는 것은 늘 PCI 장치여야 한다 */
#define PCIDEVMASK	0x10	/* we should always have PCI devices */
/* [한국어] **주 버스 정보인지 뽑는 마스크(비트 5).**
 * 서 있으면 장치의 자원이 아니라 버스 전체의 범위를 뜻한다 */
#define PRIMARYBUSMASK	0x20

/* pci specific defines */
/* [한국어] **빈 슬롯을 읽었을 때 나오는 벤더 ID.**
 * 응답이 없으면 버스가 모두 1 을 돌려주므로 0xFFFF 가 된다.
 * 읽는 자: ibmphp_pci.c 가 카드가 있는지 가릴 때 */
#define PCI_VENDOR_ID_NOTVALID		0xFFFF
/* [한국어] **다기능 일반 장치의 헤더 타입.**
 * MFD(Multi-Function Device) 비트와 NORMAL 을 OR 한 값이며,
 * ibmphp_pci.c 가 헤더 타입을 견줄 때 쓴다.
 * **두 상수의 실제 값은 pci_regs.h 에 있고 이 트리에 없다** */
#define PCI_HEADER_TYPE_MULTIDEVICE	(PCI_HEADER_TYPE_MFD|PCI_HEADER_TYPE_NORMAL)
/* [한국어] 다기능 브리지의 헤더 타입. 위와 같은 방식으로 만든다 */
#define PCI_HEADER_TYPE_MULTIBRIDGE	(PCI_HEADER_TYPE_MFD|PCI_HEADER_TYPE_BRIDGE)

/* [한국어] **설정공간의 Latency Timer 에 써 넣을 값(0x64 = 100).**
 * 장치가 버스를 한 번에 붙잡고 있을 수 있는 클록 수를 정한다.
 * 읽는 자: ibmphp_pci.c 가 카드를 설정할 때 */
#define LATENCY		0x64
/* [한국어] **Cache Line Size 에 써 넣을 값(64).**
 * **LATENCY 는 16진수로, 이것은 10진수로 적혀 있다** -- 상류 그대로다 */
#define CACHE		64
/* [한국어] **Command 레지스터에 써 넣을 값.**
 * IO/메모리 디코딩, 버스 마스터 등을 한꺼번에 켠다.
 * 원문 주석이 밝히듯 **Compaq 드라이버는 0x0157 을 쓴다** --
 * 이 드라이버가 비트 하나를 더 켠다는 뜻이며, 어느 비트인지는
 * **pci_regs.h 가 이 트리에 없어 확인할 수 없다** */
#define DEVICEENABLE	0x015F		/* CPQ has 0x0157 */

/* [한국어] **브리지 뒤에 떼어 줄 IO 공간의 최소 단위(4KB).**
 * 원문 주석의 4k 가 그것이며, PCI 브리지의 IO 창이 4KB 단위이기 때문이다.
 * 읽는 자: ibmphp_pci.c 가 브리지를 설정할 때 */
#define IOBRIDGE	0x1000		/* 4k */
/* [한국어] **브리지 뒤에 떼어 줄 메모리 공간의 최소 단위(1MB).**
 * PCI 브리지의 메모리 창이 1MB 단위이기 때문이다 */
#define MEMBRIDGE	0x100000	/* 1M */

/* irqs */
/* [한국어] **장치 종류별로 미리 정해 둔 IRQ 번호. 여기부터 셋이다.**
 * SCSI 컨트롤러용.
 * 읽는 자: ibmphp_pci.c 가 카드의 클래스 코드를 보고 IRQ 를 정할 때.
 * **요즘 커널은 이런 고정 배정을 하지 않는다** -- ACPI 나 MP 표가
 *   라우팅을 알려 주기 전 시대의 방식이다 */
#define SCSI_IRQ	0x09
/* [한국어] 네트워크 카드용 */
#define LAN_IRQ		0x0A
/* [한국어] 그 밖의 장치용 */
#define OTHER_IRQ	0x0B

/* Data Structures */

/* type is of the form x x xx xx
 *                     | |  |  |_ 00 - I/O, 01 - Memory, 11 - PFMemory
 *                     | |  - 00 - No Restrictions, 01 - Avoid VGA, 10 - Avoid
 *                     | |    VGA and their aliases, 11 - Avoid ISA
 *                     | - 1 - PCI device, 0 - non pci device
 *                     - 1 - Primary PCI Bus Information (0 if Normal device)
 * the IO restrictions [2:3] are only for primary buses
 */


/* we need this struct because there could be several resource blocks
 * allocated per primary bus in the EBDA
 */
/* [한국어] **버스 하나가 쓸 수 있는 주소 범위 한 조각.**
 * 설정자: ibmphp_res.c 가 EBDA 의 주 버스 정보 항목으로 만든다.
 * 읽는 자: 같은 파일이 자원을 떼어 줄 때 어느 범위 안인지 가린다.
 * **한 버스에 범위가 여럿일 수 있어 목록이 된다** --
 *   바로 위 원문 주석이 그 사정을 밝힌다 */
struct range_node {
	/* [한국어] **이 범위의 번호. 1부터 센다.**
	 * 설정자: ibmphp_res.c 가 범위를 만들며 매긴다.
	 * 읽는 자: resource_node 의 같은 이름 필드가 이 번호를 가리켜,
	 *   그 자원이 어느 범위에 속하는지 알려 준다.
	 * 동기화: 없음 */
	int rangeno;
	/* [한국어] 범위의 시작 주소 */
	u32 start;
	/* [한국어] **범위의 끝 주소. 마지막 바이트를 가리킨다.**
	 * ibmphp_res.c 가 `end - start + 1` 로 길이를 구한다 */
	u32 end;
	/* [한국어] **같은 버스의 다음 범위.** 주소 오름차순으로 이어진다.
	 * 설정자·읽는 자: ibmphp_res.c. 동기화: 없음 */
	struct range_node *next;
};

/* [한국어] **버스 하나의 자원 관리 상태. 이 드라이버 자원 관리의 중심이다.**
 * **PCI 코어의 할당기를 쓰지 않고 직접 굴리는 이유**: 2001년 커널에는
 *   동작 중인 시스템에서 자원을 되찾아 다시 나눠 줄 수단이 없었다.
 *   그래서 EBDA 가 알려 준 범위를 여기 담아 스스로 관리한다.
 * 설정자: ibmphp_res.c 의 ibmphp_rsrc_init() 이 만들고,
 *   자원을 넣고 뺄 때마다 갱신한다.
 * 읽는 자: 같은 파일과 ibmphp_pci.c.
 * **struct bus_info 와 이름이 비슷하나 전혀 다르다** --
 *   그쪽은 속도·모드, 이쪽은 주소 범위와 자원 목록이다 */
struct bus_node {
	/* [한국어] **이 버스의 번호. 찾는 열쇠다.**
	 * 읽는 자: ibmphp_find_res_bus() 가 이 값으로 찾는다.
	 * 동기화: 없음. operations_mutex 를 쥔 쪽이 다룬다 */
	u8 busno;
	/* [한국어] **이 버스의 IO 범위 개수.**
	 * 설정자: ibmphp_res.c 가 범위를 추가할 때마다 늘린다.
	 * 읽는 자: 같은 파일과 ibmphp_pci.c 가 범위가 있는지 확인할 때.
	 * **0 이면 이 버스에 IO 를 줄 수 없다.**
	 * 동기화: 없음 */
	int noIORanges;
	/* [한국어] IO 범위 목록의 머리. 주소 오름차순이다.
	 * 설정자·읽는 자: ibmphp_res.c. 동기화: 없음 */
	struct range_node *rangeIO;
	/* [한국어] 이 버스의 일반 메모리 범위 개수 */
	int noMemRanges;
	/* [한국어] 일반 메모리 범위 목록의 머리 */
	struct range_node *rangeMem;
	/* [한국어] 이 버스의 prefetchable 메모리 범위 개수 */
	int noPFMemRanges;
	/* [한국어] prefetchable 메모리 범위 목록의 머리.
	 * **세 종류를 따로 두는 이유**: PCI 브리지의 창 레지스터가 셋으로
	 *   나뉘어 있어 할당도 따로 해야 하기 때문이다 */
	struct range_node *rangePFMem;
	/* [한국어] **이 버스의 IO 범위를 다시 계산해야 하는가.**
	 * 설정자·읽는 자: ibmphp_res.c 뿐이다 --
	 *   브리지 뒤에 새 버스가 생기면 그 범위를 부모에서 떼어 와야 하는데,
	 *   그 갱신이 필요함을 표시한다.
	 * 동기화: 없음 */
	int needIOUpdate;
	/* [한국어] 일반 메모리 범위를 다시 계산해야 하는가 */
	int needMemUpdate;
	/* [한국어] prefetchable 메모리 범위를 다시 계산해야 하는가 */
	int needPFMemUpdate;
	/* [한국어] **이 버스에서 이미 쓰이고 있는 IO 자원 목록의 머리.**
	 * 원문 주석: first IO resource on the Bus.
	 * 설정자: ibmphp_add_resource() 가 주소 오름차순으로 끼워 넣는다.
	 * 읽는 자: ibmphp_check_resource() 가 빈자리를 찾을 때 이 목록의
	 *   틈을 본다 -- **쓰이는 것을 적어 두고 그 사이를 내주는 방식이다.**
	 * 동기화: 없음 */
	struct resource_node *firstIO;	/* first IO resource on the Bus */
	/* [한국어] 쓰이고 있는 일반 메모리 자원 목록의 머리 */
	struct resource_node *firstMem;	/* first memory resource on the Bus */
	/* [한국어] 쓰이고 있는 prefetchable 메모리 자원 목록의 머리 */
	struct resource_node *firstPFMem;	/* first prefetchable memory resource on the Bus */
	/* [한국어] **prefetchable 이 모자라 일반 메모리에서 떼어 온 자원의 목록.**
	 * 원문 주석이 그 사정을 밝힌다.
	 * 설정자: ibmphp_add_pfmem_from_mem() 이 매단다.
	 * 읽는 자: ibmphp_res.c 와 ibmphp_pci.c 가 해제할 때 어느 목록에서
	 *   뺄지 가리며, resource_node 의 fromMem 필드가 그 짝이다.
	 * **목록을 따로 두는 이유**: 돌려줄 때 일반 메모리 쪽으로 되돌려야
	 *   하는데, 섞여 있으면 구분할 수 없기 때문이다.
	 * 동기화: 없음 */
	struct resource_node *firstPFMemFromMem;	/* when run out of pfmem available, taking from Mem */
	/* [한국어] 전역 gbuses 목록에 매달릴 고리.
	 * 설정자·읽는 자: ibmphp_res.c. 동기화: 없음 */
	struct list_head bus_list;
};

/* [한국어] **실제로 쓰이고 있는 자원 한 덩어리.**
 * 설정자: ibmphp_pci.c 가 카드의 BAR 를 읽어 만들고,
 *   ibmphp_res.c 가 버스 목록에 끼워 넣는다.
 * 읽는 자: 자원을 찾고 빼는 ibmphp_res.c 의 모든 함수.
 * **이 드라이버는 빈자리 목록이 아니라 쓰이는 자리 목록을 유지한다** --
 *   범위 안에서 이 목록의 틈이 곧 빈자리다 */
struct resource_node {
	/* [한국어] **이 자원이 속한 range_node 의 번호.**
	 * 설정자: ibmphp_res.c 가 자원을 넣으며 어느 범위에 드는지 찾아 채운다.
	 * 읽는 자: 같은 파일과 ibmphp_pci.c.
	 * **-1 이면 어느 범위에도 속하지 않는다** -- 그런 자원은 이 드라이버가
	 *   다시 배정하지 못한다.
	 * 동기화: 없음 */
	int rangeno;
	/* [한국어] 이 자원이 속한 버스 번호. 동기화: 없음 */
	u8 busno;
	/* [한국어] **이 자원을 쓰는 장치·함수 번호가 합쳐진 한 바이트.**
	 * 읽는 자: ibmphp_res.c 가 카드를 뺄 때 그 장치의 자원만 골라 뺀다.
	 * 동기화: 없음 */
	u8 devfunc;
	/* [한국어] 자원 구간의 시작 주소 */
	u32 start;
	/* [한국어] **자원 구간의 끝 주소. 마지막 바이트를 가리킨다** */
	u32 end;
	/* [한국어] **구간의 길이.** start 와 end 로도 구할 수 있으나 따로 들고 있다 --
	 *   ibmphp_check_resource() 가 크기를 견줄 때 자주 쓰기 때문이다.
	 * 동기화: 없음 */
	u32 len;
	/* [한국어] **자원의 종류.**
	 * 값 범위: MEM(0), IO(1), PFMEM(2).
	 * 읽는 자: ibmphp_res.c 가 어느 목록에 넣을지 가릴 때.
	 * 원문 주석: MEM, IO, PFMEM.
	 * 동기화: 없음 */
	int type;		/* MEM, IO, PFMEM */
	/* [한국어] **이 prefetchable 자원이 실은 일반 메모리에서 떼어 온 것인가.**
	 * 원문 주석이 그 뜻을 밝힌다.
	 * 설정자: ibmphp_add_pfmem_from_mem() 이 세운다.
	 * 읽는 자: 해제 경로가 이 값을 보고 일반 메모리 쪽으로 되돌린다.
	 * **bus_node 의 firstPFMemFromMem 목록과 짝을 이루는 표시다.**
	 * 동기화: 없음 */
	u8 fromMem;		/* this is to indicate that the range is from
				 * the Memory bucket rather than from PFMem */
	/* [한국어] **같은 범위 안의 다음 자원.** 주소 오름차순이다.
	 * 설정자·읽는 자: ibmphp_res.c. 동기화: 없음 */
	struct resource_node *next;
	/* [한국어] **다음 범위의 첫 자원.**
	 * 원문 주석이 밝히듯 한 버스에 범위가 여럿일 수 있어,
	 *   next 는 범위 안을, 이 필드는 범위 사이를 잇는다.
	 * **두 겹 목록이라 훑는 코드가 이중 루프가 된다.**
	 * 동기화: 없음 */
	struct resource_node *nextRange;	/* for the other mem range on bus */
};

/* [한국어] **브리지 하나가 뒤쪽 전체를 위해 얼마나 필요한지 세어 둔 것.**
 * 설정자: ibmphp_pci.c 가 브리지 뒤를 훑으며 채운다.
 * 읽는 자: 같은 파일이 브리지의 창 크기를 정할 때.
 * **브리지는 자기 뒤의 모든 장치를 한 창으로 덮어야 하므로**
 *   먼저 합계를 세고 그다음 한 번에 떼어 온다 */
struct res_needed {
	/* [한국어] 뒤쪽 전체가 필요로 하는 일반 메모리 크기의 합 */
	u32 mem;
	/* [한국어] 뒤쪽 전체가 필요로 하는 prefetchable 메모리 크기의 합 */
	u32 pfmem;
	/* [한국어] 뒤쪽 전체가 필요로 하는 IO 크기의 합 */
	u32 io;
	/* [한국어] **세는 도중 다룰 수 없는 것을 만났는가.**
	 * 원문 주석: needed for return.
	 * 설정자·읽는 자: ibmphp_pci.c 뿐이다 -- 이 값이 서면 그 브리지 뒤를
	 *   설정하지 않고 물러난다.
	 * **반환값 대신 구조체 필드로 실패를 알리는 형태다.**
	 * 동기화: 없음 */
	u8 not_correct;		/* needed for return */
	/* [한국어] **브리지 뒤에 있는 장치 번호를 표시한 배열.**
	 * 원문 주석: for device numbers behind this bridge.
	 * 설정자: ibmphp_pci.c 가 장치를 발견할 때마다 그 번호 칸을 1 로 만든다.
	 * **PCI 버스 하나에 장치가 최대 32개라 크기가 32다.**
	 * 읽는 자: 같은 파일이 브리지를 설정하고 해제할 때.
	 * 동기화: 없음 */
	int devices[32];	/* for device numbers behind this bridge */
};

/* functions */

/* [한국어] **아래는 ibmphp_res.c 가 제공하는 자원 관리 함수들이다.**
 * EBDA 자원 목록으로 bus_node 와 range_node 를 세운다. 초기화의 둘째 단계다 */
int ibmphp_rsrc_init(void);
/* [한국어] **쓰이는 자원을 버스 목록에 주소 오름차순으로 끼워 넣는다.**
 * 어느 범위에 드는지 찾아 rangeno 도 채운다 */
int ibmphp_add_resource(struct resource_node *);
/* [한국어] **쓰이던 자원을 목록에서 빼고 해제한다.**
 * fromMem 이 서 있으면 일반 메모리 쪽으로 되돌린다 */
int ibmphp_remove_resource(struct resource_node *);
/* [한국어] **시작 주소로 이미 등록된 자원을 찾는다.**
 * 넷째 인자가 종류(MEM/IO/PFMEM)이며,
 * 셋째 인자로 찾은 것을 돌려주므로 이중 포인터다 */
int ibmphp_find_resource(struct bus_node *, u32, struct resource_node **, int);
/* [한국어] **요청한 크기가 들어갈 빈자리를 찾아 자원의 start 와 end 를 채운다.**
 * 범위 안에서 쓰이는 자원 목록의 틈을 훑는다.
 * 둘째 인자는 브리지용인지 여부이며, 그때는 정렬 요구가 달라진다 */
int ibmphp_check_resource(struct resource_node *, u8);
/* [한국어] 브리지 뒤의 버스가 사라질 때 그 bus_node 를 없앤다 */
int ibmphp_remove_bus(struct bus_node *, u8);
/* [한국어] 모든 버스와 자원을 해제한다. 모듈을 내릴 때 부른다 */
void ibmphp_free_resources(void);
/* [한국어] **prefetchable 이 모자랄 때 일반 메모리에서 떼어 온다.**
 * 그 자원에 fromMem 을 세우고 firstPFMemFromMem 목록에 매단다 */
int ibmphp_add_pfmem_from_mem(struct resource_node *);
/* [한국어] 버스 번호로 bus_node 를 찾는다. 자원 관리의 거의 모든 함수가 이것으로 시작한다 */
struct bus_node *ibmphp_find_res_bus(u8);
/* [한국어] 자원 상태를 전부 찍는 디버깅 함수.
 * 원문 주석: for debugging purposes */
void ibmphp_print_test(void);	/* for debugging purposes */

/* [한국어] **아래는 ibmphp_hpc.c 가 제공하는 컨트롤러 접근 함수들이다.**
 * 컨트롤러에서 상태를 읽는다. 둘째 인자가 READ_ 계열 명령이다 */
int ibmphp_hpc_readslot(struct slot *, u8, u8 *);
/* [한국어] 컨트롤러에 명령을 내고 완료까지 기다린다. 둘째 인자가 HPC_ 계열 명령이다 */
int ibmphp_hpc_writeslot(struct slot *, u8);
/* [한국어] **슬롯·컨트롤러 자료구조를 잠그고 저수준 로그를 켠다.**
 * 로그를 함께 켜는 것이 이 함수의 숨은 절반이며,
 * 그래서 폴링 스레드는 이것을 쓰지 않고 뮤텍스를 직접 잡는다 */
void ibmphp_lock_operations(void);
/* [한국어] 그 잠금을 놓고 저수준 로그를 다시 막는다 */
void ibmphp_unlock_operations(void);
/* [한국어] **폴링 커널 스레드를 띄운다.**
 * 이 컨트롤러에는 인터럽트가 없어 2초마다 상태를 물어보는 수밖에 없다 */
int ibmphp_hpc_start_poll_thread(void);
/* [한국어] 그 스레드를 세우고 뒷정리한다 */
void ibmphp_hpc_stop_poll_thread(void);

//----------------------------------------------------------------------------


//----------------------------------------------------------------------------
// HPC return codes
//----------------------------------------------------------------------------
/* [한국어] **컨트롤러 접근이 실패했음을 나타내는 값(0xFF).**
 * 읽는 자: ibmphp_hpc.c 의 거의 모든 저수준 함수.
 * **진짜 0xFF 와 구별되지 않는다** -- 응답이 없는 버스도 0xFF 를 돌려준다.
 * 원문 주석: HPC return codes */
#define HPC_ERROR			0xFF

//-----------------------------------------------------------------------------
// BUS INFO
//-----------------------------------------------------------------------------
/* [한국어] **버스 상태 바이트에서 속도 필드를 뽑는 마스크(비트 4~5).**
 * 읽는 자: CURRENT_BUS_SPEED 매크로 */
#define BUS_SPEED			0x30
/* [한국어] **버스 상태 바이트에서 모드 비트를 뽑는 마스크(비트 6).**
 * 읽는 자: CURRENT_BUS_MODE 매크로 */
#define BUS_MODE			0x40
/* [한국어] PCI-X 모드를 뜻하는 값(1) */
#define BUS_MODE_PCIX			0x01
/* [한국어] 통상 PCI 모드를 뜻하는 값(0) */
#define BUS_MODE_PCI			0x00
/* [한국어] **속도 필드의 상위 비트.** CURRENT_BUS_SPEED 가 이것과 BUS_SPEED_1 둘을
 * 조합해 네 속도를 가려낸다 */
#define BUS_SPEED_2			0x20
/* [한국어] 속도 필드의 하위 비트 */
#define BUS_SPEED_1			0x10
/* [한국어] **속도를 나타내는 값. 여기부터 여섯이다.**
 * 0 -- 33MHz.
 * **위의 BUS_SPEED_1/_2 와 이름이 비슷하나 뜻이 다르다** --
 *   저쪽은 상태 바이트의 비트 자리, 이쪽은 해석된 결과값이다 */
#define BUS_SPEED_33			0x00
/* [한국어] 1 -- 66MHz */
#define BUS_SPEED_66			0x01
/* [한국어] 2 -- 100MHz */
#define BUS_SPEED_100			0x02
/* [한국어] 3 -- 133MHz */
#define BUS_SPEED_133			0x03
/* [한국어] 4 -- 66MHz PCI-X.
 * **CURRENT_BUS_SPEED 매크로는 이 값을 돌려주지 않는다** --
 *   그 매크로는 0~3 만 내며, 이 값은 다른 경로가 쓴다 */
#define BUS_SPEED_66PCIX		0x04
/* [한국어] 5 -- 66MHz 인데 모드를 알 수 없음 */
#define BUS_SPEED_66UNKNOWN		0x05
/* [한국어] **컨트롤러 옵션 비트 -- 버스 상태를 읽을 수 있는가.**
 * 읽는 자: READ_BUS_STATUS 매크로 */
#define BUS_STATUS_AVAILABLE		0x01
/* [한국어] **컨트롤러 옵션 비트 -- 버스 속도를 바꿀 수 있는가.**
 * 읽는 자: SET_BUS_STATUS 매크로 */
#define BUS_CONTROL_AVAILABLE		0x02
/* [한국어] **컨트롤러 옵션 비트 -- 래치 레지스터를 지원하는가.**
 * 읽는 자: READ_SLOT_LATCH 매크로.
 * **ibmphp_hpc.c 의 폴링 스레드가 이 값을 보고 래치를 읽을지 정한다** */
#define SLOT_LATCH_REGS_SUPPORTED	0x10

/* [한국어] **컨트롤러 리비전 바이트에서 상위 니블을 뽑는 마스크.**
 * 읽는 자: READ_BUS_MODE 매크로가 0x20 이상인지 견준다 --
 *   곧 리비전 2 이상이어야 버스 모드를 읽을 수 있다는 뜻이다 */
#define PRGM_MODEL_REV_LEVEL		0xF0
/* [한국어] **카드가 없음을 나타내는 값(9).**
 * **이 값을 쓰는 곳을 이 트리에서 찾을 수 없다** */
#define MAX_ADAPTER_NONE		0x09

//----------------------------------------------------------------------------
// HPC 'write' operations/commands
//----------------------------------------------------------------------------
//	Command			Code	State	Write to reg
//					Machine	at index
//-------------------------	----	-------	------------
/* [한국어] **컨트롤러에 낼 쓰기 명령. 여기부터 열여덟이다.**
 * 각 줄 오른쪽 주석의 세 칸은 명령 코드, 상태 기계에 영향을 주는지(Y/N),
 * 그리고 써 넣을 인덱스 범위를 뜻한다.
 * 인터럽트 켜기 -- 인덱스 15 는 컨트롤러 자신이다 */
#define HPC_CTLR_ENABLEIRQ	0x00	// N	15
/* [한국어] 인터럽트 끄기 */
#define HPC_CTLR_DISABLEIRQ	0x01	// N	15
/* [한국어] **슬롯 전원 끄기. 상태 기계에 영향을 주므로 완료 확인이 필요하다** */
#define HPC_SLOT_OFF		0x02	// Y	0-14
/* [한국어] 슬롯 전원 켜기. 이것도 완료 확인이 필요하다 */
#define HPC_SLOT_ON		0x03	// Y	0-14
/* [한국어] 주의 표시등 끄기 */
#define HPC_SLOT_ATTNOFF	0x04	// N	0-14
/* [한국어] 주의 표시등 켜기 */
#define HPC_SLOT_ATTNON		0x05	// N	0-14
/* [한국어] 인터럽트 상태 지우기 */
#define HPC_CTLR_CLEARIRQ	0x06	// N	15
/* [한국어] **컨트롤러 리셋. 완료 확인이 필요하다** */
#define HPC_CTLR_RESET		0x07	// Y	15
/* [한국어] 인터럽트 라우팅 설정 */
#define HPC_CTLR_IRQSTEER	0x08	// N	15
/* [한국어] **버스를 33MHz 통상 모드로 바꾼다.**
 * 인덱스 31~34 는 버스 영역이며, 완료 확인이 필요하다 */
#define HPC_BUS_33CONVMODE	0x09	// Y	31-34
/* [한국어] 버스를 66MHz 통상 모드로 바꾼다 */
#define HPC_BUS_66CONVMODE	0x0A	// Y	31-34
/* [한국어] 버스를 66MHz PCI-X 모드로 바꾼다 */
#define HPC_BUS_66PCIXMODE	0x0B	// Y	31-34
/* [한국어] 버스를 100MHz PCI-X 모드로 바꾼다 */
#define HPC_BUS_100PCIXMODE	0x0C	// Y	31-34
/* [한국어] 버스를 133MHz PCI-X 모드로 바꾼다 */
#define HPC_BUS_133PCIXMODE	0x0D	// Y	31-34
/* [한국어] **모든 슬롯 전원 끄기.** 완료 확인이 필요하다 */
#define HPC_ALLSLOT_OFF		0x11	// Y	15
/* [한국어] 모든 슬롯 전원 켜기. 완료 확인이 필요하다 */
#define HPC_ALLSLOT_ON		0x12	// Y	15
/* [한국어] 주의 표시등 깜빡이기.
 * **코드 0x13 으로 뛰는 것이 눈에 띈다** -- 0x0E~0x10 이 비어 있다 */
#define HPC_SLOT_BLINKLED	0x13	// N	0-14

//----------------------------------------------------------------------------
// read commands
//----------------------------------------------------------------------------
/* [한국어] **컨트롤러에서 읽어 올 대상. 여기부터 아홉이다.**
 * 슬롯 상태 한 바이트 */
#define READ_SLOTSTATUS		0x01
/* [한국어] 확장 슬롯 상태 한 바이트 */
#define READ_EXTSLOTSTATUS	0x02
/* [한국어] **버스 상태.** 결과가 slot 의 busstatus 에 담긴다 */
#define READ_BUSSTATUS		0x03
/* [한국어] 컨트롤러 상태 */
#define READ_CTLRSTATUS		0x04
/* [한국어] **슬롯 상태와 확장 상태를 함께 읽어 slot 구조체를 갱신한다.**
 * **아홉 중 유일하게 구조체를 갱신하는 명령이며**,
 * 폴링의 변화 탐지가 여기 기댄다 */
#define READ_ALLSTAT		0x05
/* [한국어] 모든 슬롯을 훑어 갱신한다.
 * **ibmphp_hpc.c 의 주석이 Not used 라 밝히며, 이 명령을 내는 곳이 없다** */
#define READ_ALLSLOT		0x06
/* [한국어] **래치 레지스터.** 폴링이 2초마다 읽는 대상이며,
 * 한 바이트에 여러 슬롯의 래치 상태가 비트로 들어 있다 */
#define READ_SLOTLATCHLOWREG	0x07
/* [한국어] 컨트롤러 리비전 */
#define READ_REVLEVEL		0x08
/* [한국어] **컨트롤러가 지원하는 기능 목록.**
 * BUS_STATUS_AVAILABLE 등의 비트가 여기서 온다 */
#define READ_HPCOPTIONS		0x09
//----------------------------------------------------------------------------
// slot status
//----------------------------------------------------------------------------
/* [한국어] **슬롯 상태 바이트의 비트 여덟 개. 여기부터다.**
 * 비트 0 -- 전원이 들어와 있는가.
 * 읽는 자: SLOT_POWER 매크로와 ibmphp_hpc.c 의 process_changeinstatus */
#define HPC_SLOT_POWER		0x01
/* [한국어] 비트 1 -- 버스에 연결되어 있는가.
 * **process_changeinstatus 는 이 비트를 무시한다** */
#define HPC_SLOT_CONNECT	0x02
/* [한국어] 비트 2 -- 주의 표시등이 켜져 있는가 */
#define HPC_SLOT_ATTN		0x04
/* [한국어] **비트 3 -- 두 번째 존재 감지선.**
 * PCI 는 카드 폭을 알리려고 감지선을 두 개 두며,
 * SLOT_PRESENT 매크로가 둘을 조합해 네 상태를 가려낸다 */
#define HPC_SLOT_PRSNT2		0x08
/* [한국어] 비트 4 -- 첫 번째 존재 감지선 */
#define HPC_SLOT_PRSNT1		0x10
/* [한국어] **비트 5 -- 전원이 정상인가.**
 * 이 비트가 켜짐에서 꺼짐으로 바뀌면 슬롯을 내려야 한다 */
#define HPC_SLOT_PWRGD		0x20
/* [한국어] 비트 6 -- 버스 속도가 어긋났는가.
 * **process_changeinstatus 는 이 비트를 무시한다** */
#define HPC_SLOT_BUS_SPEED	0x40
/* [한국어] **비트 7 -- 레버 상태.**
 * 원문 주석이 아래에서 밝히듯 PCI 규격과 뜻이 반대다 */
#define HPC_SLOT_LATCH		0x80

//----------------------------------------------------------------------------
// HPC_SLOT_POWER status return codes
//----------------------------------------------------------------------------
/* [한국어] SLOT_POWER 매크로가 돌려주는 값 -- 전원 꺼짐 */
#define HPC_SLOT_POWER_OFF	0x00
/* [한국어] SLOT_POWER 매크로가 돌려주는 값 -- 전원 켜짐 */
#define HPC_SLOT_POWER_ON	0x01

//----------------------------------------------------------------------------
// HPC_SLOT_CONNECT status return codes
//----------------------------------------------------------------------------
/* [한국어] **SLOT_CONNECT 매크로가 돌려주는 값 -- 연결됨(0).**
 * **비트가 꺼져 있을 때 이 값이 나온다** -- 매크로가 뜻을 뒤집는다 */
#define HPC_SLOT_CONNECTED	0x00
/* [한국어] SLOT_CONNECT 매크로가 돌려주는 값 -- 끊김(1).
 * **비트가 서 있을 때 이 값이 나온다** */
#define HPC_SLOT_DISCONNECTED	0x01

//----------------------------------------------------------------------------
// HPC_SLOT_ATTN status return codes
//----------------------------------------------------------------------------
/* [한국어] SLOT_ATTN 매크로가 돌려주는 값 -- 표시등 꺼짐 */
#define HPC_SLOT_ATTN_OFF	0x00
/* [한국어] SLOT_ATTN 매크로가 돌려주는 값 -- 표시등 켜짐 */
#define HPC_SLOT_ATTN_ON	0x01
/* [한국어] **SLOT_ATTN 매크로가 돌려주는 값 -- 깜빡임.**
 * 그 매크로는 확장 상태의 BLINK_ATTN 비트를 먼저 보므로,
 * 깜빡임이 켜짐보다 우선한다 */
#define HPC_SLOT_ATTN_BLINK	0x02

//----------------------------------------------------------------------------
// HPC_SLOT_PRSNT status return codes
//----------------------------------------------------------------------------
/* [한국어] **SLOT_PRESENT 매크로가 돌려주는 값 -- 빈 슬롯.**
 * 두 감지선이 모두 서 있을 때다 */
#define HPC_SLOT_EMPTY		0x00
/* [한국어] 카드가 있고 폭이 7 인 경우.
 * **7/15/25 라는 숫자의 뜻은 EBDA 규격 문서에 있고 이 트리에 없다** */
#define HPC_SLOT_PRSNT_7	0x01
/* [한국어] 카드가 있고 폭이 15 인 경우 */
#define HPC_SLOT_PRSNT_15	0x02
/* [한국어] 카드가 있고 폭이 25 인 경우 */
#define HPC_SLOT_PRSNT_25	0x03

//----------------------------------------------------------------------------
// HPC_SLOT_PWRGD status return codes
//----------------------------------------------------------------------------
/* [한국어] SLOT_PWRGD 매크로가 돌려주는 값 -- 전원 이상 없음(비트가 꺼짐) */
#define HPC_SLOT_PWRGD_FAULT_NONE	0x00
/* [한국어] SLOT_PWRGD 매크로가 돌려주는 값 -- 전원 정상(비트가 섬) */
#define HPC_SLOT_PWRGD_GOOD		0x01

//----------------------------------------------------------------------------
// HPC_SLOT_BUS_SPEED status return codes
//----------------------------------------------------------------------------
/* [한국어] SLOT_BUS_SPEED 매크로가 돌려주는 값 -- 속도가 맞음 */
#define HPC_SLOT_BUS_SPEED_OK	0x00
/* [한국어] SLOT_BUS_SPEED 매크로가 돌려주는 값 -- 속도가 어긋남 */
#define HPC_SLOT_BUS_SPEED_MISM	0x01

//----------------------------------------------------------------------------
// HPC_SLOT_LATCH status return codes
//----------------------------------------------------------------------------
/* [한국어] **SLOT_LATCH 매크로가 돌려주는 값 -- 레버 열림(1).**
 * 원문 주석이 밝히듯 **PCI 규격에서는 비트가 꺼진 것이 열림** 이라,
 * 이 드라이버의 표현이 규격과 반대다 */
#define HPC_SLOT_LATCH_OPEN	0x01	// NOTE : in PCI spec bit off = open
/* [한국어] **SLOT_LATCH 매크로가 돌려주는 값 -- 레버 닫힘(0).**
 * 원문 주석: PCI 규격에서는 비트가 켜진 것이 닫힘 */
#define HPC_SLOT_LATCH_CLOSED	0x00	// NOTE : in PCI spec bit on  = closed


//----------------------------------------------------------------------------
// extended slot status
//----------------------------------------------------------------------------
/* [한국어] **확장 슬롯 상태 바이트의 비트 여덟 개. 여기부터다.**
 * 비트 0 -- 이 슬롯이 PCI-X 인가 */
#define HPC_SLOT_PCIX		0x01
/* [한국어] 비트 1 -- 속도 필드의 하위 비트 */
#define HPC_SLOT_SPEED1		0x02
/* [한국어] **비트 2 -- 속도 필드의 상위 비트.**
 * SLOT_SPEED 매크로가 둘을 조합해 33/66/133 을 가려낸다 */
#define HPC_SLOT_SPEED2		0x04
/* [한국어] **비트 3 -- 주의 표시등이 깜빡이는가.**
 * **ibmphp_hpc.c 의 process_changeinstatus 가 이 비트를 검사하면서
 * 주석에는 bit 4 라 적어 두었다** -- 마스크 0x08 은 비트 3 이다 */
#define HPC_SLOT_BLINK_ATTN	0x08
/* [한국어] 비트 4 -- 예약. 쓰이지 않는다 */
#define HPC_SLOT_RSRVD1		0x10
/* [한국어] 비트 5 -- 예약. 쓰이지 않는다 */
#define HPC_SLOT_RSRVD2		0x20
/* [한국어] 비트 6 -- 버스 모드가 어긋났는가 */
#define HPC_SLOT_BUS_MODE	0x40
/* [한국어] 비트 7 -- 예약. 쓰이지 않는다 */
#define HPC_SLOT_RSRVD3		0x80

//----------------------------------------------------------------------------
// HPC_XSLOT_PCIX_CAP status return codes
//----------------------------------------------------------------------------
/* [한국어] SLOT_PCIX 매크로가 돌려주는 값 -- PCI-X 가 아님 */
#define HPC_SLOT_PCIX_NO	0x00
/* [한국어] SLOT_PCIX 매크로가 돌려주는 값 -- PCI-X 임 */
#define HPC_SLOT_PCIX_YES	0x01

//----------------------------------------------------------------------------
// HPC_XSLOT_SPEED status return codes
//----------------------------------------------------------------------------
/* [한국어] SLOT_SPEED 매크로가 돌려주는 값 -- 33MHz */
#define HPC_SLOT_SPEED_33	0x00
/* [한국어] SLOT_SPEED 매크로가 돌려주는 값 -- 66MHz */
#define HPC_SLOT_SPEED_66	0x01
/* [한국어] **SLOT_SPEED 매크로가 돌려주는 값 -- 133MHz.**
 * **100MHz 를 나타내는 값이 없다** -- 두 비트로 세 상태만 가려낸다 */
#define HPC_SLOT_SPEED_133	0x02

//----------------------------------------------------------------------------
// HPC_XSLOT_ATTN_BLINK status return codes
//----------------------------------------------------------------------------
/* [한국어] 깜빡임 꺼짐. **이 값을 쓰는 매크로를 이 헤더에서 찾을 수 없다** */
#define HPC_SLOT_ATTN_BLINK_OFF	0x00
/* [한국어] 깜빡임 켜짐. 마찬가지로 쓰는 곳이 없다 */
#define HPC_SLOT_ATTN_BLINK_ON	0x01

//----------------------------------------------------------------------------
// HPC_XSLOT_BUS_MODE status return codes
//----------------------------------------------------------------------------
/* [한국어] SLOT_BUS_MODE 매크로가 돌려주는 값 -- 모드가 맞음 */
#define HPC_SLOT_BUS_MODE_OK	0x00
/* [한국어] SLOT_BUS_MODE 매크로가 돌려주는 값 -- 모드가 어긋남 */
#define HPC_SLOT_BUS_MODE_MISM	0x01

//----------------------------------------------------------------------------
// Controller status
//----------------------------------------------------------------------------
/* [한국어] **컨트롤러 상태 바이트의 비트 여덟 개. 여기부터다.**
 * 비트 0 -- 지금 명령을 처리하는 중인가.
 * **hpc_wait_ctlr_notworking() 이 이 비트가 내려가기를 기다린다** */
#define HPC_CTLR_WORKING	0x01
/* [한국어] **비트 1 -- 명령을 마쳤는가.**
 * NEEDTOCHECK_CMDSTATUS 가 참인 명령에서 이것까지 확인한다 */
#define HPC_CTLR_FINISHED	0x02
/* [한국어] 비트 2 -- 결과 코드의 하위 비트 */
#define HPC_CTLR_RESULT0	0x04
/* [한국어] **비트 3 -- 결과 코드의 상위 비트.**
 * CTLR_RESULT 매크로가 둘을 조합해 네 결과를 가려낸다 */
#define HPC_CTLR_RESULT1	0x08
/* [한국어] **비트 4 -- 이름에 오타가 있다.** RESULT2 여야 자연스러우나
 * 상류 그대로이며, **이 상수를 쓰는 곳이 없어 드러나지 않는다** */
#define HPC_CTLR_RESULE2	0x10
/* [한국어] 비트 5 -- 결과 관련 비트. **쓰는 곳이 없다** */
#define HPC_CTLR_RESULT3	0x20
/* [한국어] 비트 6 -- 인터럽트 라우팅 상태. **쓰는 곳이 없다** */
#define HPC_CTLR_IRQ_ROUTG	0x40
/* [한국어] 비트 7 -- 인터럽트 대기 중. **쓰는 곳이 없다** */
#define HPC_CTLR_IRQ_PENDG	0x80

//----------------------------------------------------------------------------
// HPC_CTLR_WORKING status return codes
//----------------------------------------------------------------------------
/* [한국어] CTLR_WORKING 매크로가 돌려주는 값 -- 놀고 있음 */
#define HPC_CTLR_WORKING_NO	0x00
/* [한국어] CTLR_WORKING 매크로가 돌려주는 값 -- 일하는 중 */
#define HPC_CTLR_WORKING_YES	0x01

//----------------------------------------------------------------------------
// HPC_CTLR_FINISHED status return codes
//----------------------------------------------------------------------------
/* [한국어] CTLR_FINISHED 매크로가 돌려주는 값 -- 아직 안 끝남 */
#define HPC_CTLR_FINISHED_NO	0x00
/* [한국어] CTLR_FINISHED 매크로가 돌려주는 값 -- 끝남 */
#define HPC_CTLR_FINISHED_YES	0x01

//----------------------------------------------------------------------------
// HPC_CTLR_RESULT status return codes
//----------------------------------------------------------------------------
/* [한국어] CTLR_RESULT 매크로가 돌려주는 값 -- 성공 */
#define HPC_CTLR_RESULT_SUCCESS	0x00
/* [한국어] CTLR_RESULT 매크로가 돌려주는 값 -- 실패 */
#define HPC_CTLR_RESULT_FAILED	0x01
/* [한국어] CTLR_RESULT 매크로가 돌려주는 값 -- 예약 */
#define HPC_CTLR_RESULT_RSVD	0x02
/* [한국어] **CTLR_RESULT 매크로가 돌려주는 값 -- 응답 없음.**
 * **네 결과값을 쓰는 곳을 이 트리에서 찾을 수 없다** --
 * CTLR_RESULT 매크로 자체가 불리지 않는다 */
#define HPC_CTLR_RESULT_NORESP	0x03


//----------------------------------------------------------------------------
// macro for slot info
//----------------------------------------------------------------------------
/* [한국어] **상태 바이트 한 비트를 이름 있는 값으로 바꾸는 매크로. 여기부터다.**
 * 전원 비트를 읽어 ON/OFF 를 돌려준다.
 * **뜻이 뒤집히지 않는 드문 매크로다** -- 비트가 서면 ON 이다 */
#define SLOT_POWER(s)	((u8) ((s & HPC_SLOT_POWER) \
	? HPC_SLOT_POWER_ON : HPC_SLOT_POWER_OFF))

/* [한국어] **연결 비트를 읽는데 뜻이 뒤집혀 있다.**
 * 비트가 서 있으면 DISCONNECTED, 꺼져 있으면 CONNECTED 다.
 * 읽는 자: ibmphp_hpc.c 의 process_changeinstatus 가
 *   슬롯을 내릴지 판단할 때 */
#define SLOT_CONNECT(s)	((u8) ((s & HPC_SLOT_CONNECT) \
	? HPC_SLOT_DISCONNECTED : HPC_SLOT_CONNECTED))

/* [한국어] **두 바이트를 함께 보는 유일한 매크로다.**
 * 확장 상태의 깜빡임 비트를 먼저 보고, 서 있으면 BLINK 를 돌려준다.
 * 그렇지 않을 때만 일반 상태의 주의 비트를 본다 --
 * **곧 깜빡임이 켜짐보다 우선한다** */
#define SLOT_ATTN(s, es)	((u8) ((es & HPC_SLOT_BLINK_ATTN) \
	? HPC_SLOT_ATTN_BLINK \
	: ((s & HPC_SLOT_ATTN) ? HPC_SLOT_ATTN_ON : HPC_SLOT_ATTN_OFF)))

/* [한국어] **두 감지선을 조합해 네 상태를 가려낸다.**
 * PRSNT1 이 서 있으면 PRSNT2 에 따라 EMPTY 또는 PRSNT_15,
 * 꺼져 있으면 PRSNT_25 또는 PRSNT_7 이다.
 * **둘 다 서 있을 때가 빈 슬롯이다** -- 카드가 없으면 두 선이 모두
 *   풀업으로 올라가기 때문이다 */
#define SLOT_PRESENT(s)	((u8) ((s & HPC_SLOT_PRSNT1) \
	? ((s & HPC_SLOT_PRSNT2) ? HPC_SLOT_EMPTY : HPC_SLOT_PRSNT_15) \
	: ((s & HPC_SLOT_PRSNT2) ? HPC_SLOT_PRSNT_25 : HPC_SLOT_PRSNT_7)))

/* [한국어] 전원 양호 비트를 읽어 GOOD 또는 FAULT_NONE 을 돌려준다.
 * 읽는 자: process_changeinstatus 가 전원 이상을 판단할 때 */
#define SLOT_PWRGD(s)	((u8) ((s & HPC_SLOT_PWRGD) \
	? HPC_SLOT_PWRGD_GOOD : HPC_SLOT_PWRGD_FAULT_NONE))

/* [한국어] **버스 속도 어긋남 비트를 읽는데 뜻이 뒤집혀 있다.**
 * 비트가 서 있으면 MISM(어긋남)이다 */
#define SLOT_BUS_SPEED(s)	((u8) ((s & HPC_SLOT_BUS_SPEED) \
	? HPC_SLOT_BUS_SPEED_MISM : HPC_SLOT_BUS_SPEED_OK))

/* [한국어] **레버 비트를 읽는데 뜻이 뒤집혀 있다.**
 * 비트가 서 있으면 CLOSED 를 돌려준다 -- PCI 규격과 반대이며,
 * 상수 정의 옆의 원문 주석이 그 사정을 밝힌다 */
#define SLOT_LATCH(s)	((u8) ((s & HPC_SLOT_LATCH) \
	? HPC_SLOT_LATCH_CLOSED : HPC_SLOT_LATCH_OPEN))

/* [한국어] **확장 상태의 PCI-X 비트를 읽는다.**
 * 여기부터 셋은 인자가 확장 상태 바이트(es)다 */
#define SLOT_PCIX(es)	((u8) ((es & HPC_SLOT_PCIX) \
	? HPC_SLOT_PCIX_YES : HPC_SLOT_PCIX_NO))

/* [한국어] **속도 비트 둘을 조합한다.**
 * SPEED2 가 서 있으면 SPEED1 에 따라 133 또는 66,
 * 꺼져 있으면 33 이다.
 * **100MHz 를 나타내는 갈래가 없다** -- 두 비트로 세 값만 낸다 */
#define SLOT_SPEED(es)	((u8) ((es & HPC_SLOT_SPEED2) \
	? ((es & HPC_SLOT_SPEED1) ? HPC_SLOT_SPEED_133   \
				: HPC_SLOT_SPEED_66)   \
	: HPC_SLOT_SPEED_33))

/* [한국어] **버스 모드 어긋남 비트를 읽는데 뜻이 뒤집혀 있다.**
 * 비트가 서 있으면 MISM 이다 */
#define SLOT_BUS_MODE(es)	((u8) ((es & HPC_SLOT_BUS_MODE) \
	? HPC_SLOT_BUS_MODE_MISM : HPC_SLOT_BUS_MODE_OK))

//--------------------------------------------------------------------------
// macro for bus info
//---------------------------------------------------------------------------
/* [한국어] **버스 상태 바이트의 속도 비트 둘을 조합한다.**
 * BUS_SPEED_2 가 서 있으면 BUS_SPEED_1 에 따라 133 또는 100,
 * 꺼져 있으면 66 또는 33 이다.
 * 읽는 자: ibmphp_core.c 가 bus_info 의 current_speed 를 채울 때.
 * **괄호 배치가 눈에 띈다** -- `((u8) (s & BUS_SPEED_2))` 가 아니라
 *   `((u8) (s & BUS_SPEED_2) ? ... )` 라, 형변환이 삼항 연산의
 *   조건에만 걸린다. 결과는 같다 */
#define CURRENT_BUS_SPEED(s)	((u8) (s & BUS_SPEED_2) \
	? ((s & BUS_SPEED_1) ? BUS_SPEED_133 : BUS_SPEED_100) \
	: ((s & BUS_SPEED_1) ? BUS_SPEED_66 : BUS_SPEED_33))

/* [한국어] 버스 모드 비트를 읽어 PCIX 또는 PCI 를 돌려준다.
 * 읽는 자: ibmphp_core.c 가 current_bus_mode 를 채울 때 */
#define CURRENT_BUS_MODE(s)	((u8) (s & BUS_MODE) ? BUS_MODE_PCIX : BUS_MODE_PCI)

/* [한국어] **이 컨트롤러가 버스 상태를 읽을 수 있는가.**
 * 인자가 상태 바이트가 아니라 **controller 포인터** 이며,
 * 그 options 필드를 본다 -- 위쪽 SLOT_ 계열과 다른 점이다 */
#define READ_BUS_STATUS(s)	((u8) (s->options & BUS_STATUS_AVAILABLE))

/* [한국어] **이 컨트롤러가 버스 모드를 읽을 수 있는가.**
 * 리비전의 상위 니블이 0x20 이상인지 본다 --
 * 곧 리비전 2 이상이어야 한다는 뜻이다 */
#define READ_BUS_MODE(s)	((s->revision & PRGM_MODEL_REV_LEVEL) >= 0x20)

/* [한국어] **이 컨트롤러가 버스 속도를 바꿀 수 있는가.**
 * **이름이 SET 으로 시작하지만 값을 바꾸지 않고 능력을 물어본다** */
#define SET_BUS_STATUS(s)	((u8) (s->options & BUS_CONTROL_AVAILABLE))

/* [한국어] **이 컨트롤러가 래치 레지스터를 지원하는가.**
 * 읽는 자: ibmphp_hpc.c 의 폴링 스레드가 래치를 읽을지 정할 때.
 * **이 매크로가 거짓이면 그 컨트롤러의 슬롯은 레버 변화를 감지하지 못한다** */
#define READ_SLOT_LATCH(s)	((u8) (s->options & SLOT_LATCH_REGS_SUPPORTED))

//----------------------------------------------------------------------------
// macro for controller info
//----------------------------------------------------------------------------
/* [한국어] **컨트롤러 상태 바이트를 읽는 매크로. 여기부터 셋이다.**
 * 지금 명령을 처리하는 중인지 돌려준다.
 * 읽는 자: ibmphp_hpc.c 의 hpc_wait_ctlr_notworking() --
 *   **이 파일에서 가장 자주 불리는 매크로다** */
#define CTLR_WORKING(c) ((u8) ((c & HPC_CTLR_WORKING) \
	? HPC_CTLR_WORKING_YES : HPC_CTLR_WORKING_NO))
/* [한국어] 명령을 마쳤는지 돌려준다.
 * 읽는 자: ibmphp_hpc_writeslot() 이 완료 확인이 필요한 명령에서 본다 */
#define CTLR_FINISHED(c) ((u8) ((c & HPC_CTLR_FINISHED) \
	? HPC_CTLR_FINISHED_YES : HPC_CTLR_FINISHED_NO))
/* [한국어] **결과 비트 둘을 조합해 네 값을 가려낸다.**
 * RESULT1 이 서 있으면 RESULT0 에 따라 NORESP 또는 RSVD,
 * 꺼져 있으면 FAILED 또는 SUCCESS 다.
 * **이 매크로를 부르는 곳을 이 트리에서 찾을 수 없다** --
 *   컨트롤러가 결과를 알려 주지만 드라이버가 보지 않는다 */
#define CTLR_RESULT(c) ((u8) ((c & HPC_CTLR_RESULT1)  \
	? ((c & HPC_CTLR_RESULT0) ? HPC_CTLR_RESULT_NORESP \
				: HPC_CTLR_RESULT_RSVD)  \
	: ((c & HPC_CTLR_RESULT0) ? HPC_CTLR_RESULT_FAILED \
				: HPC_CTLR_RESULT_SUCCESS)))

// command that affect the state machine of HPC
/* [한국어] **명령을 낸 뒤 완료 표시까지 확인해야 하는가.**
 * 열 가지 명령을 나열하며, 원문 주석이 밝히듯
 * **컨트롤러의 상태 기계에 영향을 주는 명령들이다.**
 * 읽는 자: ibmphp_hpc_writeslot() 의 완료 대기 루프.
 * **그 명령들은 위쪽 HPC_ 정의에서 오른쪽 주석에 Y 로 표시되어 있다** --
 *   두 곳이 같은 목록을 나타내므로 한쪽을 고치면 다른 쪽도 고쳐야 한다.
 * 백슬래시로 줄을 잇는 것은 매크로가 여러 줄에 걸치기 때문이다 */
#define NEEDTOCHECK_CMDSTATUS(c) ((c == HPC_SLOT_OFF)        || \
				  (c == HPC_SLOT_ON)         || \
				  (c == HPC_CTLR_RESET)      || \
				  (c == HPC_BUS_33CONVMODE)  || \
				  (c == HPC_BUS_66CONVMODE)  || \
				  (c == HPC_BUS_66PCIXMODE)  || \
				  (c == HPC_BUS_100PCIXMODE) || \
				  (c == HPC_BUS_133PCIXMODE) || \
				  (c == HPC_ALLSLOT_OFF)     || \
				  (c == HPC_ALLSLOT_ON))


/* Core part of the driver */

/* [한국어] **핫플러그 조작의 방향을 나타내는 두 값.**
 * 1 -- 슬롯을 켠다.
 * 읽는 자: ibmphp_core.c 가 enable/disable 경로를 한 함수로 합칠 때.
 * 원문 주석: Core part of the driver */
#define ENABLE		1
/* [한국어] 0 -- 슬롯을 끈다 */
#define DISABLE		0

/* [한국어] **카드 정보 필드를 뽑는 마스크(하위 3비트).**
 * 읽는 자: ibmphp_core.c 가 확장 상태에서 속도 정보를 꺼낼 때 */
#define CARD_INFO	0x07
/* [한국어] 카드가 133MHz PCI-X 임을 나타내는 값 */
#define PCIX133		0x07
/* [한국어] 카드가 66MHz PCI-X 임을 나타내는 값 */
#define PCIX66		0x05
/* [한국어] 카드가 66MHz 통상 PCI 임을 나타내는 값.
 * **33MHz 를 나타내는 값이 없다** -- 그 밖의 값이 모두 33MHz 로 다뤄진다 */
#define PCI66		0x04

/* [한국어] **설정공간 접근에 쓰는 pci_bus.**
 * 정의: ibmphp_core.c.
 * 설정자: ibmphp_ebda.c 가 하나 만들어 채운다.
 * 읽는 자: ibmphp_pci.c 가 `ibmphp_pci_bus->number = busno` 로 번호만
 *   갈아 끼운 뒤 pci_bus_read_config_ 계열을 부른다.
 * **cpqphp 가 pci_bus 사본을 만들어 쓰는 것과 같은 관용이며**,
 *   지금 기준으로는 위험한 방식이다.
 * 동기화: 없음 */
extern struct pci_bus *ibmphp_pci_bus;

/* Variables */

/* [한국어] **PCI 함수 하나가 쥔 자원과 설정.**
 * 설정자: ibmphp_pci.c 가 카드를 설정하며 만들고 채운다.
 * 읽는 자: 같은 파일이 카드를 뗄 때 자원을 되돌리며 훑는다.
 * **slot 하나에 여러 pci_func 이 매달릴 수 있다** -- 다기능 카드나
 *   브리지 뒤의 장치들이 next 로 이어진다 */
struct pci_func {
	/* [한국어] **리눅스 쪽 struct pci_dev.**
	 * 원문 주석: from the OS.
	 * 설정자: ibmphp_pci.c 가 pci_get_domain_bus_and_slot 등으로 찾아 넣는다.
	 * 읽는 자: 같은 파일이 장치를 떼거나 다시 훑을 때.
	 * 동기화: 없음 */
	struct pci_dev *dev;	/* from the OS */
	/* [한국어] 이 함수가 있는 버스 번호. 동기화: 없음 */
	u8 busno;
	/* [한국어] 이 함수의 PCI 장치 번호 */
	u8 device;
	/* [한국어] **이 함수의 함수 번호(0~7).**
	 * 다기능 카드면 같은 device 에 function 만 다른 pci_func 이 여럿 생긴다 */
	u8 function;
	/* [한국어] **이 함수가 쥔 IO 자원. BAR 여섯 개에 대응한다.**
	 * 설정자: ibmphp_pci.c 가 BAR 를 읽어 크기를 알아낸 뒤 자원을 배정하며 채운다.
	 * 읽는 자: 같은 파일이 카드를 뗄 때 되돌리며 훑는다.
	 * 값 범위: 쓰지 않는 BAR 자리는 NULL.
	 * 동기화: 없음 */
	struct resource_node *io[6];
	/* [한국어] 이 함수가 쥔 일반 메모리 자원. BAR 여섯 개에 대응한다 */
	struct resource_node *mem[6];
	/* [한국어] **이 함수가 쥔 prefetchable 메모리 자원.**
	 * 세 배열을 따로 두는 것은 BAR 하나가 셋 중 한 종류이기 때문이며,
	 * 같은 색인에 둘이 동시에 차는 일은 없다 */
	struct resource_node *pfmem[6];
	/* [한국어] **같은 슬롯의 다음 함수.**
	 * 설정자: ibmphp_pci.c 가 다기능 카드나 브리지 뒤를 훑으며 잇는다.
	 * 읽는 자: ibmphp_core.c 와 _pci.c 가 슬롯의 모든 함수를 훑을 때.
	 * 동기화: 없음 */
	struct pci_func *next;
	/* [한국어] **이 함수가 브리지일 때 그 뒤에 있는 장치 번호 표시.**
	 * 원문 주석: for bridge config.
	 * 설정자·읽는 자: ibmphp_pci.c.
	 * **res_needed 의 같은 이름 필드와 쓰임이 같다.**
	 * 동기화: 없음 */
	int devices[32];	/* for bridge config */
	/* [한국어] **INTA~INTD 각각에 배정된 IRQ 번호.**
	 * 원문 주석: for interrupt config.
	 * 설정자: ibmphp_pci.c 가 카드의 인터럽트 핀을 보고 채운다.
	 * 읽는 자: 같은 파일이 설정공간의 Interrupt Line 에 써 넣을 때.
	 * 동기화: 없음 */
	u8 irq[4];		/* for interrupt config */
	/* [한국어] **이 함수가 PCI-to-PCI 브리지인지 나타내는 표시.**
	 * 원문 주석: flag for unconfiguring, to say if PPB.
	 * **이름이 bus 인데 버스 번호가 아니라 참/거짓이다** -- busno 와
	 *   헷갈리기 쉬운 자리다.
	 * 설정자·읽는 자: ibmphp_pci.c 의 해제 경로.
	 * 동기화: 없음 */
	u8 bus;			/* flag for unconfiguring, to say if PPB */
};

/* [한국어] **슬롯 하나의 전부. 이 드라이버에서 가장 자주 오가는 구조체다.**
 * 설정자: ibmphp_ebda.c 가 EBDA 를 읽어 만들고 전역 목록에 매단다.
 * 읽는 자: 다섯 파일 모두.
 * **핫플러그 코어의 struct hotplug_slot 이 값으로 박혀 있고**,
 *   파일 끝의 to_slot() 이 그 사이를 잇는다.
 * **상태 세 바이트(status, ext_status, busstatus)가 폴링의 대상이며**,
 *   ibmphp_hpc.c 가 그것을 갱신하고 변화를 찾는다 */
struct slot {
	/* [한국어] 이 슬롯이 붙어 있는 버스 번호.
	 * 설정자: ibmphp_ebda.c 가 ebda_hpc_slot 의 slot_bus_num 에서 옮긴다.
	 * 읽는 자: ibmphp_hpc.c 가 버스 명령의 인덱스를 구할 때.
	 * 동기화: 부팅 뒤 불변 */
	u8 bus;
	/* [한국어] 이 슬롯의 PCI 장치 번호.
	 * 설정자: ibmphp_core.c 의 ibmphp_init_devno() 가 BIOS IRQ 라우팅 표를
	 *   뒤져 채운다.
	 * 읽는 자: ibmphp_pci.c 가 설정공간에 접근할 때.
	 * 동기화: 불변 */
	u8 device;
	/* [한국어] **컨트롤러가 아는 슬롯 번호.**
	 * 설정자: ibmphp_ebda.c 가 ebda_hpc_slot 의 slot_num 에서 옮긴다.
	 * 읽는 자: ibmphp_core.c 가 sysfs 이름을 만들 때.
	 * **real_physical_slot_num 과 다를 수 있다** -- RIO 구성에서 갈린다.
	 * 동기화: 불변 */
	u8 number;
	/* [한국어] **사용자가 섀시에서 보는 진짜 슬롯 번호.**
	 * 설정자: ibmphp_ebda.c 가 RIO 표를 보고 계산한다 --
	 *   확장 상자를 이어 붙이면 번호가 겹치므로 다시 매겨야 하기 때문이다.
	 * 읽는 자: ibmphp_core.c 가 sysfs 이름과 로그에 쓴다.
	 * 동기화: 불변 */
	u8 real_physical_slot_num;
	/* [한국어] **이 슬롯의 능력 비트 모음.**
	 * 설정자: ibmphp_core.c 가 슬롯을 등록하며 채운다.
	 * 읽는 자: 핫플러그 코어 쪽 조회 경로.
	 * 동기화: 불변 */
	u32 capabilities;
	/* [한국어] **이 슬롯이 낼 수 있는 최고 속도.**
	 * 설정자: ibmphp_ebda.c 가 ebda_hpc_slot 의 slot_cap 을 해석해 채운다.
	 * 읽는 자: ibmphp_core.c 가 카드를 올리며 버스 속도를 정할 때.
	 * 동기화: 불변 */
	u8 supported_speed;
	/* [한국어] **이 슬롯이 PCI-X 를 지원하는가.**
	 * 설정자: ibmphp_ebda.c 가 slot_cap 의 PCIX 비트로 정한다.
	 * 읽는 자: ibmphp_core.c.
	 * 동기화: 불변 */
	u8 supported_bus_mode;
	/* [한국어] **슬롯 내리기와 폴링 사이를 잇는 표시.**
	 * 원문 주석: this is for disable slot and polling.
	 * 설정자: ibmphp_core.c 가 사용자 요청으로 내릴 때 세우고,
	 *   ibmphp_hpc.c 의 process_changeinstatus 가 하드웨어가 강제한
	 *   내리기에서는 0 으로 내린다.
	 * 읽는 자: ibmphp_core.c 의 내리기 경로가 두 경우를 가릴 때.
	 * 동기화: operations_mutex */
	u8 flag;		/* this is for disable slot and polling */
	/* [한국어] **컨트롤러 안에서 이 슬롯의 인덱스.**
	 * 설정자: ibmphp_ebda.c 가 ebda_hpc_slot 의 ctl_index 에서 옮긴다.
	 * 읽는 자: ibmphp_hpc.c 의 readslot/writeslot 이 명령 인덱스를 만들 때.
	 * **number 나 real_physical_slot_num 과 다르다** -- 이쪽은 하드웨어가
	 *   아는 번호다.
	 * 동기화: 불변 */
	u8 ctlr_index;
	/* [한국어] **핫플러그 코어 쪽 구조체가 값으로 박혀 있다.**
	 * 설정자: ibmphp_core.c 가 ops 를 이어 붙이고 pci_hp_register 로 등록한다.
	 * 읽는 자: 핫플러그 코어. 콜백은 to_slot() 으로 바깥 slot 을 되찾는다.
	 * **포인터가 아니라 값이라 두 구조체의 수명이 같다.**
	 * **정의는 linux/pci_hotplug.h 에 있고 이 트리에 없다** */
	struct hotplug_slot hotplug_slot;
	/* [한국어] **이 슬롯을 담당하는 컨트롤러.**
	 * 설정자: ibmphp_ebda.c 가 슬롯을 만들며 잇는다.
	 * 읽는 자: ibmphp_hpc.c 의 모든 저수준 함수가 이 고리로 컨트롤러에 닿는다.
	 * 동기화: 불변 */
	struct controller *ctrl;
	/* [한국어] **이 슬롯에 꽂힌 카드의 함수 목록 머리.**
	 * 설정자: ibmphp_pci.c 가 카드를 설정하며 만들고,
	 *   뗄 때 해제하고 NULL 로 되돌린다.
	 * 읽는 자: ibmphp_core.c 와 _pci.c.
	 * 값 범위: 빈 슬롯이면 NULL.
	 * 동기화: operations_mutex */
	struct pci_func *func;
	/* [한국어] **이 슬롯의 INTA~INTD 에 배정된 IRQ 번호.**
	 * 설정자: ibmphp_core.c 의 ibmphp_init_devno() 가 BIOS 라우팅 표에서 읽는다.
	 * 읽는 자: ibmphp_pci.c 가 카드의 Interrupt Line 을 채울 때.
	 * 동기화: 불변 */
	u8 irq[4];
	/* [한국어] **이 슬롯이 32비트인가 64비트인가.**
	 * 원문 주석: 0 = 32, 1 = 64.
	 * 설정자·읽는 자: ibmphp_core.c 뿐이다.
	 * 동기화: 없음 */
	int bit_mode;		/* 0 = 32, 1 = 64 */
	/* [한국어] **이 슬롯이 붙어 있는 버스의 속도·모드 정보.**
	 * 설정자: ibmphp_ebda.c 가 버스 번호로 찾아 잇는다.
	 * 읽는 자: ibmphp_core.c 가 카드를 올리며 버스 속도를 정할 때 --
	 *   같은 버스의 다른 슬롯 상태까지 함께 봐야 하기 때문이다.
	 * 동기화: 불변 */
	struct bus_info *bus_on;
	/* [한국어] **전역 ibmphp_slot_head 목록에 매달릴 고리.**
	 * 설정자: ibmphp_ebda.c.
	 * 읽는 자: ibmphp_hpc.c 의 폴링 스레드가 2초마다 훑는다.
	 * 동기화: operations_mutex */
	struct list_head ibm_slot_list;
	/* [한국어] **슬롯 상태 바이트. 폴링이 갱신하는 세 값 중 하나다.**
	 * 설정자: ibmphp_hpc.c 의 ibmphp_hpc_readslot(READ_ALLSTAT) 만이 갱신한다.
	 * 읽는 자: 같은 파일의 process_changeinstatus 가 옛 사본과 견주고,
	 *   SLOT_ 계열 매크로가 이 값에서 비트를 꺼낸다.
	 * 값 범위: HPC_SLOT_ 계열 여덟 비트의 조합.
	 * 동기화: operations_mutex */
	u8 status;
	/* [한국어] **확장 슬롯 상태 바이트.**
	 * 설정자·읽는 자: status 와 같다.
	 * **status 와 나뉜 이유**: 컨트롤러가 두 인덱스에서 따로 읽어 주므로
	 *   READ_ALLSTAT 이 두 번 읽어 각각에 담는다.
	 * 동기화: operations_mutex */
	u8 ext_status;
	/* [한국어] **버스 상태 바이트.**
	 * 설정자: ibmphp_hpc_readslot(READ_BUSSTATUS).
	 * 읽는 자: ibmphp_core.c 가 CURRENT_BUS_SPEED 와 CURRENT_BUS_MODE 로
	 *   지금 버스가 어떤 속도인지 알아낼 때.
	 * **폴링이 갱신하지 않는다** -- 앞의 둘과 달리 필요할 때만 읽는다.
	 * 동기화: operations_mutex */
	u8 busstatus;
};

/* [한국어] **컨트롤러 하나의 전부.**
 * 설정자: ibmphp_ebda.c 가 EBDA 를 읽어 만들고 목록에 매단다.
 * 읽는 자: ibmphp_hpc.c 의 모든 함수.
 * **ctlr_type 이 네 전송 방식을 가르고 union u 가 그에 맞는 접근 정보를
 *   담는 것이 이 구조체의 핵심 설계다** */
struct controller {
	/* [한국어] **이 컨트롤러가 담당하는 슬롯들의 EBDA 정보 배열.**
	 * 설정자: ibmphp_ebda.c 가 slot_count 만큼 잡아 채운다.
	 * 읽는 자: 같은 파일이 struct slot 을 만들 때.
	 * 동기화: 불변 */
	struct ebda_hpc_slot *slots;
	/* [한국어] 이 컨트롤러가 담당하는 버스들의 EBDA 정보 배열.
	 * 설정자: ibmphp_ebda.c 가 bus_count 만큼 잡아 채운다.
	 * 동기화: 불변 */
	struct ebda_hpc_bus *buses;
	/* [한국어] **컨트롤러가 PCI 장치로 붙어 있을 때 그 장치.**
	 * 원문 주석: in case where controller is PCI.
	 * 설정자: ibmphp_ebda.c 가 pci_ctlr_access 의 bus/dev_fun 으로 찾아 넣는다.
	 * 읽는 자: ibmphp_hpc.c 의 pci_ctrl_read / pci_ctrl_write.
	 * 값 범위: ctlr_type 이 1 이 아니면 NULL.
	 * 동기화: 불변 */
	struct pci_dev *ctrl_dev; /* in case where controller is PCI */
	/* [한국어] **이 컨트롤러가 담당하는 첫 슬롯 번호.**
	 * 원문 주석: starting and ending slot #'s this ctrl controls.
	 * 읽는 자: ibmphp_hpc.c 의 process_changeinlatch() 가 훑을 범위를 정할 때.
	 * 동기화: 불변 */
	u8 starting_slot_num;	/* starting and ending slot #'s this ctrl controls*/
	/* [한국어] 이 컨트롤러가 담당하는 마지막 슬롯 번호.
	 * **래치 레지스터의 비트 자리가 이 범위 안에 든다** */
	u8 ending_slot_num;
	/* [한국어] **컨트롤러의 리비전.**
	 * 읽는 자: READ_BUS_MODE 매크로가 상위 니블을 보고
	 *   버스 모드를 읽을 수 있는 세대인지 가린다.
	 * 동기화: 불변 */
	u8 revision;
	/* [한국어] **이 컨트롤러가 지원하는 기능 비트 모음.**
	 * 원문 주석: which options HPC supports.
	 * 설정자: ibmphp_ebda.c 가 READ_HPCOPTIONS 로 읽어 채운다.
	 * 읽는 자: READ_BUS_STATUS, SET_BUS_STATUS, READ_SLOT_LATCH 세 매크로.
	 * **그중 READ_SLOT_LATCH 가 폴링의 동작을 좌우한다.**
	 * 동기화: 불변 */
	u8 options;		/* which options HPC supports */
	/* [한국어] **마지막으로 읽은 컨트롤러 상태 바이트.**
	 * 설정자: ibmphp_hpc.c 의 readslot(READ_ALLSTAT)과 writeslot 이 갱신한다.
	 * 읽는 자: CTLR_ 계열 매크로.
	 * 동기화: operations_mutex 와 sem_hpcaccess */
	u8 status;
	/* [한국어] 이 컨트롤러의 ID.
	 * 설정자: ibmphp_ebda.c. 읽는 자: 같은 파일의 디버그 출력.
	 * 동기화: 불변 */
	u8 ctlr_id;
	/* [한국어] 이 컨트롤러가 담당하는 슬롯 개수. slots 배열의 크기다 */
	u8 slot_count;
	/* [한국어] 이 컨트롤러가 담당하는 버스 개수. buses 배열의 크기다 */
	u8 bus_count;
	/* [한국어] **컨트롤러들 사이에서의 순번(0부터).**
	 * 설정자: ibmphp_ebda.c 가 컨트롤러를 만들며 매긴다.
	 * 읽는 자: ibmphp_hpc.c 의 폴링 스레드가 슬롯 목록을 걸으며
	 *   **컨트롤러당 한 번만 래치를 읽으려고 이 값으로 걸러낸다.**
	 * 동기화: 불변 */
	u8 ctlr_relative_id;
	/* [한국어] **이 컨트롤러의 IRQ 번호.**
	 * 설정자: ibmphp_ebda.c 가 EBDA 에서 읽는다.
	 * **이 드라이버는 인터럽트를 쓰지 않고 폴링하므로 request_irq 를
	 *   부르는 곳이 없다** -- 값만 기록해 둔다.
	 * 동기화: 불변 */
	u32 irq;
	/* [한국어] **네 전송 방식 중 이 컨트롤러에 맞는 접근 정보 하나.**
	 * ctlr_type 이 어느 갈래가 유효한지 정한다 --
	 *   0 이면 isa_ctlr, 1 이면 pci_ctlr, 2 나 4 면 wpeg_ctlr.
	 * **잘못된 갈래를 읽으면 엉뚱한 값이 나오므로 ctlr_type 확인이 필수다** */
	union {
		struct isa_ctlr_access isa_ctlr;
		struct pci_ctlr_access pci_ctlr;
		struct wpeg_i2c_ctlr_access wpeg_ctlr;
	} u;
	/* [한국어] **union u 의 어느 갈래가 유효한지, 그리고 어떤 방식으로 접근할지 정한다.**
	 * 값 범위: 0(ISA IO 포트), 1(PCI 설정공간),
	 *   2(Winnipeg I2C 주소 지정), 4(Winnipeg I2C 직접, 확장 상자).
	 * 설정자: ibmphp_ebda.c 가 EBDA 에서 읽는다.
	 * 읽는 자: ibmphp_hpc.c 의 ctrl_read/ctrl_write 가 이 값으로 갈래를 나누고,
	 *   ioremap 이 필요한지도 이것으로 정한다.
	 * **이 파일에서 가장 자주 검사되는 필드다.**
	 * 동기화: 불변 */
	u8 ctlr_type;
	/* [한국어] 전역 ebda_hpc_head 목록에 매달릴 고리.
	 * 설정자·읽는 자: ibmphp_ebda.c. 동기화: 없음 */
	struct list_head ebda_hpc_list;
};

/* Functions */

/* [한국어] **아래는 ibmphp_core.c 가 제공하는 함수들이다.**
 * BIOS IRQ 라우팅 표를 뒤져 슬롯의 device 번호와 irq 배열을 채운다.
 * 원문 주석이 밝히듯 **EBDA 쪽에서 부르므로 static 일 수 없다.**
 * **이중 포인터인 것이 눈에 띄는데**, 슬롯 포인터를 바꾸지는 않는다 */
int ibmphp_init_devno(struct slot **);	/* This function is called from EBDA, so we need it not be static */
/* [한국어] **슬롯을 내린다.**
 * 읽는 자: ibmphp_hpc.c 의 process_changeinstatus 가
 *   전원 이상이나 레버 열림을 감지했을 때 부른다 --
 *   **폴링이 위층으로 올라가는 두 경로 중 하나다** */
int ibmphp_do_disable_slot(struct slot *slot_cur);
/* [한국어] **핫플러그 코어 쪽 슬롯 정보를 새로 쓴다.**
 * 원문 주석이 밝히듯 HPC 쪽에서 부르므로 static 일 수 없다.
 * **폴링이 위층으로 올라가는 다른 한 경로다** */
int ibmphp_update_slot_info(struct slot *);	/* This function is called from HPC, so we need it to not be static */
/* [한국어] **꽂힌 카드의 설정공간을 읽고 자원을 배정한다**(ibmphp_pci.c).
 * 둘째 인자는 슬롯 번호다 */
int ibmphp_configure_card(struct pci_func *, u8);
/* [한국어] **카드가 쥔 자원을 되돌리고 설정을 지운다**(ibmphp_pci.c).
 * 둘째 인자가 어느 단계까지 되돌릴지 정한다 */
int ibmphp_unconfigure_card(struct slot **, int);
/* [한국어] **핫플러그 코어에 넘길 콜백 표.**
 * 정의: ibmphp_core.c.
 * 읽는 자: 같은 파일이 슬롯을 등록하며 `slot->hotplug_slot.ops` 에 잇는다.
 * **const 이며 모든 슬롯이 같은 표 하나를 공유한다.**
 * 동기화: 불변 */
extern const struct hotplug_slot_ops ibmphp_hotplug_slot_ops;

/* [한국어] **이 헤더의 유일한 실행 코드다.**
 * 핫플러그 코어가 struct hotplug_slot 포인터만 알고 있으므로,
 * container_of 로 바깥 struct slot 을 계산해 꺼내야 이 드라이버의 상태에 닿는다.
 * **ibmphp_core.c 의 sysfs 콜백들이 모두 첫 줄에서 이것을 부른다.**
 * 같은 관용을 cpqphp 와 pnv_php 가 쓴다 --
 * "남이 아는 부분에서 내 전체로 나오는" 변환이다.
 * 
 * 실행 컨텍스트: 어디서든 부를 수 있다. 순수 포인터 계산이다.
 * 
 * 호출 체인:
 *   ibmphp_core.c 의 핫플러그 콜백들 → [이 함수] → container_of */
static inline struct slot *to_slot(struct hotplug_slot *hotplug_slot)
{
	/* [한국어] **hotplug_slot 멤버의 오프셋을 빼서 바깥 구조체의 시작을 구한다.**
	 * 그 오프셋은 컴파일 시점에 계산되므로 실행 비용이 없다 */
	return container_of(hotplug_slot, struct slot, hotplug_slot);
}

/* [한국어] **파일 맨 앞의 중복 포함 가드와 짝을 이루는 끝.**
 * 옆의 주석이 어느 #ifdef 의 끝인지 밝히는 관행이다 */
#endif				//__IBMPHP_H

