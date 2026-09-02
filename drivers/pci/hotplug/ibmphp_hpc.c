// SPDX-License-Identifier: GPL-2.0+
/*
 * IBM Hot Plug Controller Driver
 *
 * Written By: Jyoti Shah, IBM Corporation
 *
 * Copyright (C) 2001-2003 IBM Corp.
 *
 * All rights reserved.
 *
 * Send feedback to <gregkh@us.ibm.com>
 *                  <jshah@us.ibm.com>
 *
 */

/*
 * [한국어 설명] IBM 핫플러그 컨트롤러와 실제로 대화하는 계층 (ibmphp_hpc.c)
 *
 * === 파일의 역할 ===
 * ibmphp 드라이버에서 **하드웨어에 손이 닿는 유일한 파일이다.**
 * 위층(ibmphp_core.c)이 "이 슬롯을 켜라" 고 하면, 이 파일이 그것을
 * 컨트롤러가 알아듣는 명령 바이트로 바꿔 실제 버스로 내보낸다.
 *
 * 하는 일은 크게 셋이다.
 *
 *   1. **네 가지 전송 방식을 하나의 얼굴로 감싼다.** IBM 서버는 세대에
 *      따라 핫플러그 컨트롤러가 붙은 자리가 다르다 -- ISA IO 포트,
 *      PCI 설정공간, 그리고 Winnipeg 칩셋의 I2C 두 변종이다.
 *      ctrl_read()/ctrl_write() 가 controller 의 ctlr_type 을 보고
 *      알맞은 아래층 함수로 갈라 준다. 그래서 위층은 어느 방식인지
 *      알 필요가 없다.
 *
 *   2. **명령을 직렬화한다.** 컨트롤러는 한 번에 명령 하나만 처리하므로,
 *      sem_hpcaccess 뮤텍스로 접근을 하나씩 줄 세우고
 *      hpc_wait_ctlr_notworking() 으로 이전 명령이 끝나기를 기다린 뒤에야
 *      다음 명령을 낸다.
 *
 *   3. **폴링 스레드를 돌린다.** 이 컨트롤러에는 인터럽트가 없다.
 *      poll_hpc() 커널 스레드가 2초마다 깨어나 래치 레지스터와 슬롯
 *      상태를 읽고, 지난번과 달라진 것이 있으면 그것을 사건으로 만들어
 *      위층에 알린다. **이 파일이 이 드라이버의 심장인 이유가 여기 있다.**
 *
 * === 전체 아키텍처에서의 위치 ===
 * ibmphp 는 다섯 파일로 나뉜다. ibmphp_ebda.c 가 BIOS 의 EBDA 를 읽어
 * 컨트롤러와 슬롯 목록을 만들고, ibmphp_res.c 가 자원을 관리하며,
 * ibmphp_pci.c 가 PCI 설정을 맡고, ibmphp_core.c 가 핫플러그 코어와
 * 이어지는 진입점이다. **이 파일은 그 가장 아래에 있다.**
 *
 * 명령이 내려가는 길:
 *   사용자가 sysfs 에 씀
 *     → ibmphp_core.c 의 enable_slot / disable_slot
 *        → ibmphp_lock_operations()          [이 파일 -- 자료구조 잠금]
 *        → ibmphp_hpc_writeslot(pslot, cmd)  [이 파일]
 *           → hpc_writecmdtoindex()          [명령을 인덱스로]
 *           → get_hpc_access()               [컨트롤러 독점]
 *           → hpc_wait_ctlr_notworking()     [이전 명령 완료 대기]
 *           → ctrl_write()                   [ctlr_type 별 갈래]
 *              → isa_ctrl_write / pci_ctrl_write / i2c_ctrl_write
 *           → 명령 완료 대기 → free_hpc_access()
 *
 * 상태가 올라오는 길:
 *   poll_hpc() 커널 스레드가 2초마다
 *     → ibmphp_hpc_readslot(pslot, READ_SLOTLATCHLOWREG 또는 READ_ALLSTAT)
 *     → 지난번 값과 견주어 달라졌으면
 *        → process_changeinlatch() / process_changeinstatus()
 *           → ibmphp_do_disable_slot() 또는 ibmphp_update_slot_info()
 *             (둘 다 ibmphp_core.c)
 *
 * 실행 컨텍스트: **인터럽트 문맥이 없다.** 폴링 스레드와 sysfs 경로가
 * 모두 프로세스 문맥이며, 두 뮤텍스와 msleep 으로 서로 양보한다.
 *
 * === 타 모듈과의 연결 ===
 * **위쪽**: ibmphp_core.c 가 이 파일의 여섯 공개 함수를 부른다 --
 * ibmphp_hpc_readslot, ibmphp_hpc_writeslot, ibmphp_lock_operations,
 * ibmphp_unlock_operations, ibmphp_hpc_start_poll_thread,
 * ibmphp_hpc_stop_poll_thread. 거꾸로 이 파일은 위층의
 * ibmphp_do_disable_slot 과 ibmphp_update_slot_info 를 부른다 --
 * **폴링이 발견한 변화를 처리하는 것은 위층의 몫이기 때문이다.**
 *
 * **옆쪽**: ibmphp_ebda.c 의 ibmphp_get_bus_index,
 * ibmphp_get_total_controllers, ibmphp_get_slot_from_physical_num 과
 * 전역 목록 ibmphp_slot_head. 컨트롤러와 슬롯의 실체는 그 파일이 만든다.
 *
 * **아래쪽**: inb/outb(ISA), pci_read/write_config_byte(PCI),
 * readl/writel(I2C MMIO). 이 파일이 커널 밖 하드웨어와 만나는 지점이다.
 *
 * **공유 상태**: struct controller 의 status 와 ctlr_type,
 * struct slot 의 status / ext_status / busstatus.
 * 폴링 스레드가 쓰고 위층이 읽으며, operations_mutex 가 그 사이를 지킨다.
 *
 * === 주요 함수/구조체 요약 ===
 * - ibmphp_hpc_readslot() : 컨트롤러에서 상태를 읽는다. 읽을 대상마다
 *   갈래가 아홉이며, READ_ALLSTAT 만 slot 구조체를 갱신한다.
 * - ibmphp_hpc_writeslot() : 컨트롤러에 명령을 낸다. 낸 뒤 완료까지
 *   기다리는 것이 이 함수의 절반이다.
 * - ctrl_read() / ctrl_write() : ctlr_type 으로 네 전송 방식을 가르는
 *   분배기. 이 파일의 추상화가 성립하는 자리다.
 * - i2c_ctrl_read() / i2c_ctrl_write() : Winnipeg I2C 여섯 단계 절차.
 *   레지스터에 값을 쓰고, 시작 비트를 세우고, 그것이 내려가기를 기다리고,
 *   상태 레지스터를 확인한 뒤 데이터를 읽는다.
 * - hpc_writecmdtoindex() / hpc_readcmdtoindex() : 명령을 컨트롤러 안의
 *   인덱스로 옮긴다. 컨트롤러가 하나의 주소 공간에 컨트롤러 자신, 슬롯,
 *   확장 슬롯, 버스를 나란히 두기 때문에 필요하다.
 * - poll_hpc() : 폴링 커널 스레드. 세 상태를 오가며 래치를 다섯 번 보고
 *   슬롯을 한 번 본다.
 * - process_changeinstatus() / process_changeinlatch() : 달라진 비트를
 *   해석해 슬롯을 내리거나 정보를 갱신한다.
 * - hpc_wait_ctlr_notworking() : 컨트롤러가 놀고 있을 때까지 기다린다.
 *   읽기와 쓰기가 모두 이것으로 시작한다.
 * - get_hpc_access() / free_hpc_access() : 컨트롤러 독점을 위한 뮤텍스 짝.
 * - ibmphp_lock_operations() / ibmphp_unlock_operations() : 자료구조
 *   보호를 위한 다른 뮤텍스 짝. 폴링 스레드도 같은 것을 쓴다.
 *
 * === 이 파일을 읽을 때 알아 두면 좋은 것 ===
 * **뮤텍스가 둘이고 지키는 것이 다르다.**
 *   sem_hpcaccess    -- 컨트롤러 하드웨어 접근. read/writeslot 이 잡는다.
 *   operations_mutex -- 슬롯·컨트롤러 자료구조. 폴링 스레드와 위층이 잡는다.
 * 폴링 스레드는 잠들기 전에 operations_mutex 를 반드시 놓는다 --
 * 원문 주석의 "don't sleep with a lock on the hardware" 가 그것이다.
 *
 * **ctlr_type 의 값이 이 파일 곳곳에서 갈래를 가른다.**
 *   0 -- ISA IO 포트
 *   1 -- PCI 설정공간
 *   2 -- Winnipeg I2C, 주소 지정 방식
 *   4 -- Winnipeg I2C, 직접 방식(확장 상자)
 * ibmphp_ebda.c 가 EBDA 를 읽어 이 값을 정한다.
 *
 * **주석 형식이 이 디렉터리의 다른 파일과 다르다.** 함수마다
 * Name/Action/Return 을 적은 상자 주석이 붙어 있고 `//` 줄 주석을 쓴다.
 * 2001년 IBM 내부 코딩 관행의 흔적이며, 상류 그대로 보존한다.
 *
 * **Winnipeg 은 IBM 서버 칩셋의 이름이다.** 이 파일에서 WPG 로 줄여 쓰며,
 * 그 레지스터 배치를 담은 문서는 이 트리에 없다. 오프셋과 마스크의
 * 뜻은 상류 주석이 적어 둔 것과 코드가 쓰는 방식으로만 설명한다.
 */

/* [한국어] 대기 큐 관련 선언. **이 파일이 직접 쓰는 곳은 없으나**
 * completion 이 그 위에 얹혀 있어 함께 들어온 것으로 보인다 */
#include <linux/wait.h>
/* [한국어] 시간 관련 선언. **이 파일이 직접 쓰는 곳을 찾을 수 없다** */
#include <linux/time.h>
/* [한국어] DECLARE_COMPLETION 과 complete/wait_for_completion.
 * **폴링 스레드가 정말로 끝났는지 확인하는 데 쓴다** */
#include <linux/completion.h>
/* [한국어] msleep. **이 파일에서 가장 많이 쓰이는 함수 중 하나다** --
 * 컨트롤러 완료를 기다리는 모든 루프가 이것으로 양보한다 */
#include <linux/delay.h>
/* [한국어] 모듈 뼈대 매크로. **이 파일에는 module_init 이 없고**
 * ibmphp_core.c 가 진입점을 갖는다 */
#include <linux/module.h>
/* [한국어] pci_read_config_byte / pci_write_config_byte.
 * ctlr_type 1(PCI 설정공간) 갈래가 이것을 쓴다 */
#include <linux/pci.h>
/* [한국어] __init 과 __exit 표시. 폴링 스레드 시작·정지 함수에 붙는다 */
#include <linux/init.h>
/* [한국어] DEFINE_MUTEX 와 mutex_lock/unlock.
 * **이 파일의 두 뮤텍스가 여기서 온다** */
#include <linux/mutex.h>
/* [한국어] 스케줄러 관련 선언. **이 파일이 직접 쓰는 곳을 찾을 수 없으나**
 * kthread 쪽이 필요로 하는 것으로 보인다 */
#include <linux/sched.h>
/* [한국어] kthread_run, kthread_stop, kthread_should_stop.
 * **폴링 스레드의 수명을 다루는 세 함수가 모두 여기 있다** */
#include <linux/kthread.h>
/* [한국어] **이 드라이버의 공유 헤더.** struct controller, struct slot,
 * HPC_ 계열 명령 상수, READ_ 계열 읽기 명령, CTLR_WORKING 같은 매크로,
 * 그리고 다른 파일의 함수 선언이 전부 여기 있다 */
#include "ibmphp.h"

/* [한국어] **debug_polling() 매크로를 열고 닫는 스위치.**
 * 설정자: ibmphp_lock_operations() 가 1 로, ibmphp_unlock_operations() 가
 *   0 으로 되돌린다.
 * 읽는 자: 바로 아래 debug_polling 매크로.
 * 값 범위: 0 이면 저수준 로그를 막고, 1 이면 찍는다.
 * **왜 필요한가**: 폴링 스레드가 2초마다 같은 경로를 도므로 그 로그를
 *   늘 켜 두면 dmesg 가 채워진다. 그래서 **위층이 실제로 슬롯을 조작하는
 *   동안에만** 켠다.
 * 동기화: 없음. operations_mutex 를 쥔 쪽이 바꾸므로 사실상 그것이 지킨다 */
static int to_debug = 0;
/* [한국어] **to_debug 가 켜져 있을 때만 debug() 로 넘기는 매크로.**
 * `do { } while (0)` 으로 감싸는 것은 if 문 뒤에 세미콜론 없이 써도
 * 문법이 깨지지 않게 하는 커널 관용이다.
 * `arg...` 는 가변 인자를 그대로 넘기는 GNU C 확장이다 */
#define debug_polling(fmt, arg...)	do { if (to_debug) debug(fmt, arg); } while (0)

//----------------------------------------------------------------------------
// timeout values
//----------------------------------------------------------------------------
/* [한국어] **명령 완료를 기다릴 최대 시간(초).**
 * I2C 루프에서는 10밀리초씩 60번, 명령 완료 루프에서는 1초씩 60번이라
 * 같은 상수가 두 자리에서 다른 뜻으로 쓰인다.
 * 원문 주석: give HPC 60 sec to finish cmd */
#define CMD_COMPLETE_TOUT_SEC	60	// give HPC 60 sec to finish cmd
/* [한국어] 컨트롤러가 일을 놓기를 기다릴 최대 시간(초).
 * hpc_wait_ctlr_notworking() 의 첫 인자로 늘 이 값이 들어간다 */
#define HPC_CTLR_WORKING_TOUT	60	// give HPC 60 sec to finish cmd
/* [한국어] 컨트롤러 독점을 얻기까지의 제한 시간.
 * **이 파일에서 이 상수를 쓰는 곳을 찾을 수 없다** --
 * get_hpc_access() 가 시간 제한 없는 mutex_lock 을 쓰기 때문이다 */
#define HPC_GETACCESS_TIMEOUT	60	// seconds
/* [한국어] **폴링 주기(초).** poll_hpc 가 이 값에 1000 을 곱해 msleep 한다 */
#define POLL_INTERVAL_SEC	2	// poll HPC every 2 seconds
/* [한국어] **래치를 몇 번 본 뒤 슬롯 전체를 볼지.**
 * 래치 읽기는 컨트롤러당 한 번이라 싸고 슬롯 훑기는 비싸므로,
 * 다섯 번에 한 번만 비싼 쪽을 한다 */
#define POLL_LATCH_CNT		5	// poll latch 5 times, then poll slots

//----------------------------------------------------------------------------
// Winnipeg Architected Register Offsets
//----------------------------------------------------------------------------
/* [한국어] **Winnipeg 레지스터 오프셋. 여기부터 다섯 개다.**
 * I2C 메시지 버퍼(하위). 보낼 명령과 받은 데이터가 오가는 자리다 */
#define WPG_I2CMBUFL_OFFSET	0x08	// I2C Message Buffer Low
/* [한국어] I2C 마스터 동작 설정 레지스터.
 * **무엇을 어떻게 읽고 쓸지를 여기 적어 넣는다** */
#define WPG_I2CMOSUP_OFFSET	0x10	// I2C Master Operation Setup Reg
/* [한국어] I2C 마스터 제어 레지스터. 시작 비트가 여기 있다 */
#define WPG_I2CMCNTL_OFFSET	0x20	// I2C Master Control Register
/* [한국어] I2C 파라미터 레지스터.
 * **이 파일에서 쓰는 곳을 찾을 수 없다** */
#define WPG_I2CPARM_OFFSET	0x40	// I2C Parameter Register
/* [한국어] I2C 상태 레지스터. 오류 비트를 여기서 확인한다 */
#define WPG_I2CSTAT_OFFSET	0x70	// I2C Status Register

//----------------------------------------------------------------------------
// Winnipeg Store Type commands (Add this commands to the register offset)
//----------------------------------------------------------------------------
/* [한국어] **오프셋에 더해 쓰면 대입이 아니라 AND 가 되는 창.**
 * **이 파일에서 쓰는 곳을 찾을 수 없다** */
#define WPG_I2C_AND		0x1000	// I2C AND operation
/* [한국어] **오프셋에 더해 쓰면 대입이 아니라 OR 가 되는 창.**
 * i2c_ctrl_read/write 의 3단계가 시작 비트를 세울 때 쓴다 --
 * 읽어 오지 않고도 한 비트만 세울 수 있게 해 준다.
 * 원문 주석이 그 방식을 밝힌다 */
#define WPG_I2C_OR		0x2000	// I2C OR operation

//----------------------------------------------------------------------------
// Command set for I2C Master Operation Setup Register
//----------------------------------------------------------------------------
/* [한국어] **동작 설정 레지스터에 넣을 명령 조합. 여기부터 넷이다.**
 * 읽기 + 바이트 길이 + I2C 주소(시프트됨) + 인덱스.
 * ctlr_type 2 가 쓴다 */
#define WPG_READATADDR_MASK	0x00010000	// read,bytes,I2C shifted,index
/* [한국어] 쓰기 + 바이트 길이 + I2C 주소 + 인덱스. ctlr_type 2 가 쓴다 */
#define WPG_WRITEATADDR_MASK	0x40010000	// write,bytes,I2C shifted,index
/* [한국어] **주소 없이 인덱스만으로 읽는다.** ctlr_type 4(확장 상자)가 쓴다 */
#define WPG_READDIRECT_MASK	0x10010000
/* [한국어] 주소 없이 인덱스만으로 쓴다. ctlr_type 4 가 쓴다.
 * **네 값의 각 비트가 무슨 뜻인지는 Winnipeg 문서에 있고
 * 이 트리에서 확인할 수 없다** -- 상류 주석이 적어 둔 것으로만 설명한다 */
#define WPG_WRITEDIRECT_MASK	0x60010000


//----------------------------------------------------------------------------
// bit masks for I2C Master Control Register
//----------------------------------------------------------------------------
/* [한국어] **마스터 제어 레지스터의 시작 비트.**
 * 이것을 세우면 동작이 시작되고, 하드웨어가 마치면 스스로 내린다.
 * i2c_ctrl_read/write 의 4단계가 그것이 내려가기를 기다린다 */
#define WPG_I2CMCNTL_STARTOP_MASK	0x00000002	// Start the Operation

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
/* [한국어] **ioremap 할 창의 크기.**
 * 가장 높은 오프셋인 상태 레지스터(0x70)보다 훨씬 크며,
 * 그 여유가 무엇을 위한 것인지는 코드에 적혀 있지 않다 */
#define WPG_I2C_IOREMAP_SIZE	0x2044	// size of linear address interval

//----------------------------------------------------------------------------
// command index
//----------------------------------------------------------------------------
/* [한국어] **컨트롤러 안 인덱스 공간의 배치. 여기부터 넷이다.**
 * 슬롯이 시작하는 자리.
 * **이 상수를 쓰는 곳을 이 파일에서 찾을 수 없다** --
 * 슬롯 인덱스는 변환 없이 그대로 쓰기 때문이다 */
#define WPG_1ST_SLOT_INDEX	0x01	// index - 1st slot for ctlr
/* [한국어] **컨트롤러 자신의 인덱스.**
 * hpc_wait_ctlr_notworking() 이 상태를 읽을 때, 그리고 두 변환 함수가
 * 컨트롤러 전체 명령의 인덱스로 쓴다 */
#define WPG_CTLR_INDEX		0x0F	// index - ctlr
/* [한국어] **확장 슬롯 영역의 시작.**
 * READ_EXTSLOTSTATUS 와 READ_ALLSTAT 의 두 번째 읽기가 여기에 더한다 */
#define WPG_1ST_EXTSLOT_INDEX	0x10	// index - 1st ext slot for ctlr
/* [한국어] **버스 영역의 시작.**
 * 버스 명령이 여기에 더하고 1 을 뺀다 -- 버스 번호가 1 부터 세어지기 때문이다 */
#define WPG_1ST_BUS_INDEX	0x1F	// index - 1st bus for ctlr

//----------------------------------------------------------------------------
// macro utilities
//----------------------------------------------------------------------------
// if bits 20,22,25,26,27,29,30 are OFF return 1
/* [한국어] **I2C 상태 레지스터의 오류 비트 일곱 개를 한 번에 확인한다.**
 * 0x00000A76 이 비트 20, 22, 25, 26, 27, 29, 30 을 세운 마스크이며,
 * 원문 주석이 밝히듯 **그 비트가 모두 꺼져 있으면 1 을 돌려준다.**
 * 삼항 연산이 뒤집혀 있어 읽기 어려운데, 마스크에 걸리면 0(오류),
 * 걸리지 않으면 1(정상)이다.
 * **각 비트가 무슨 오류인지는 Winnipeg 문서에 있고 이 트리에 없다** */
#define HPC_I2CSTATUS_CHECK(s)	((u8)((s & 0x00000A76) ? 0 : 1))

//----------------------------------------------------------------------------
// global variables
//----------------------------------------------------------------------------
/* [한국어] **컨트롤러 하드웨어 접근을 직렬화하는 뮤텍스.**
 * 잡는 자: get_hpc_access(), 곧 ibmphp_hpc_readslot 과 _writeslot.
 * 놓는 자: free_hpc_access(). **ibmphp_hpc_stop_poll_thread() 도
 *   이것을 부르는데 그 자리에서는 잡은 적이 없다.**
 * **왜 필요한가**: 컨트롤러가 한 번에 명령 하나만 처리하므로,
 *   두 경로가 동시에 명령을 내면 서로의 완료를 기다리며 상태가 엉킨다.
 * 원문 주석: lock access to HPC */
static DEFINE_MUTEX(sem_hpcaccess);	// lock access to HPC
/* [한국어] **슬롯·컨트롤러 자료구조를 지키는 다른 뮤텍스.**
 * 잡는 자: ibmphp_lock_operations()(위층), 그리고 poll_hpc() 가 직접.
 * **폴링 스레드가 lock_operations 를 쓰지 않고 직접 잡는 이유** 는
 *   그 함수가 to_debug 도 함께 켜기 때문이다 -- 폴링 중에 저수준 로그가
 *   쏟아지는 것을 막는다.
 * **sem_hpcaccess 와 지키는 것이 다르다** -- 이쪽은 자료구조,
 *   저쪽은 하드웨어다.
 * 원문 주석: lock all operations and access to data structures */
static DEFINE_MUTEX(operations_mutex);	// lock all operations and
					// access to data structures
/* [한국어] **폴링 스레드가 정말로 빠져나갔는지 확인하는 completion.**
 * 세우는 자: poll_hpc() 가 루프를 나가며 complete() 를 부른다.
 * 기다리는 자: ibmphp_hpc_stop_poll_thread().
 * **kthread_stop() 이 이미 스레드 종료를 기다리므로 두 겹의 확인이 된다.**
 * 원문 주석: make sure polling thread goes away */
static DECLARE_COMPLETION(exit_complete); // make sure polling thread goes away
/* [한국어] **폴링 커널 스레드의 task 포인터.**
 * 설정자: ibmphp_hpc_start_poll_thread() 가 kthread_run 의 결과를 담는다.
 * 읽는 자: ibmphp_hpc_stop_poll_thread() 가 kthread_stop 에 넘긴다.
 * 값 범위: 유효한 포인터 또는 IS_ERR 로 확인해야 하는 오류 포인터.
 * 동기화: 없음. 시작과 정지가 겹치지 않는다 */
static struct task_struct *ibmphp_poll_thread;
//----------------------------------------------------------------------------
// local function prototypes
//----------------------------------------------------------------------------
/* [한국어] **지역 함수 원형 목록. 여기부터 열 개다.**
 * 아래 정의보다 먼저 쓰이는 함수들을 미리 알린다.
 * I2C 읽기 -- ctrl_read() 가 이것을 부른다 */
static u8 i2c_ctrl_read(struct controller *, void __iomem *, u8);
/* [한국어] I2C 쓰기. ctrl_write() 가 부른다 */
static u8 i2c_ctrl_write(struct controller *, void __iomem *, u8, u8);
/* [한국어] 쓰기 명령을 인덱스로 옮긴다 */
static u8 hpc_writecmdtoindex(u8, u8);
/* [한국어] 읽기 명령을 인덱스로 옮긴다 */
static u8 hpc_readcmdtoindex(u8, u8);
/* [한국어] 컨트롤러 독점을 얻는다 */
static void get_hpc_access(void);
/* [한국어] **여기서는 static 으로 적혀 있으나 정의에는 static 이 없다.**
 * ibmphp_hpc_stop_poll_thread() 가 부르므로 실제로는 파일 밖에서도
 * 보이는 이름이며, 두 선언이 어긋나 있다.
 * 코드는 손대지 않고 사실만 적는다 */
static void free_hpc_access(void);
/* [한국어] 폴링 스레드 본체 */
static int poll_hpc(void *data);
/* [한국어] 슬롯 상태 변화를 해석한다 */
static int process_changeinstatus(struct slot *, struct slot *);
/* [한국어] 래치 레지스터 변화를 슬롯별로 나눈다 */
static int process_changeinlatch(u8, u8, struct controller *);
/* [한국어] 컨트롤러가 일을 놓기를 기다린다.
 * **읽기와 쓰기가 모두 이것으로 시작한다** */
static int hpc_wait_ctlr_notworking(int, struct controller *, void __iomem *, u8 *);
//----------------------------------------------------------------------------


/*----------------------------------------------------------------------
* Name:    i2c_ctrl_read
*
* Action:  read from HPC over I2C
*
*---------------------------------------------------------------------*/
/* [한국어]
 * i2c_ctrl_read - Winnipeg I2C 를 거쳐 컨트롤러에서 한 바이트를 읽는다
 *
 * @ctlr_ptr: 대상 컨트롤러. ctlr_type 과 I2C 주소를 여기서 얻는다.
 * @WPGBbar: ioremap 해 둔 Winnipeg 레지스터 창의 시작.
 * @index: 읽을 대상의 컨트롤러 안 인덱스.
 * @return: 읽은 바이트, 실패하면 HPC_ERROR.
 *
 * **여섯 단계로 이루어진 I2C 트랜잭션이며, 상류 주석이 그 번호를 붙여 두었다.**
 * 1. **동작 설정 레지스터에 무엇을 어떻게 읽을지 적는다.**
 *    ctlr_type 2 는 I2C 주소를 실어 보내는 방식(READATADDR),
 *    ctlr_type 4 는 주소 없이 인덱스만 쓰는 방식(READDIRECT)이다.
 *    그 밖의 종류는 이 함수가 다루지 않는다.
 * 2. 메시지 버퍼를 0 으로 지운다.
 * 3. **마스터 제어 레지스터의 시작 비트를 세워 동작을 개시한다.**
 *    오프셋에 WPG_I2C_OR 를 더하는 것이 요점이다 -- 그 창에 쓰면
 *    대입이 아니라 OR 가 되므로 다른 비트를 읽어 올 필요가 없다.
 * 4. **시작 비트가 스스로 내려가기를 기다린다.** 10밀리초씩 최대
 *    CMD_COMPLETE_TOUT_SEC 번이다.
 * 5. **I2C 상태 레지스터가 정상이 되기를 기다린다.**
 *    HPC_I2CSTATUS_CHECK 가 오류 비트 일곱 개를 한꺼번에 본다.
 * 6. 메시지 버퍼에서 결과를 읽어 하위 한 바이트를 돌려준다.
 *
 * **swab32 가 곳곳에 나오는 이유**: 이 레지스터들이 LOHI 형식이라 CPU 가
 * 쓰는 HILO 와 바이트 순서가 반대다. 그래서 쓰기 전과 읽은 뒤에 뒤집는다.
 * 상류 주석의 "swap data before writing" 이 그것이다.
 *
 * **I2C 주소를 오른쪽으로 한 비트 미는 것**은 I2C 의 7비트 주소가
 * 버스에서는 왼쪽으로 한 칸 밀린 채 실리기 때문이다. 여기서는 그것을
 * 되돌려 순수한 주소를 얻은 뒤 다시 8비트 왼쪽으로 밀어 필드에 넣는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. msleep 으로 잠들며, 최악의 경우
 * 두 대기 루프에서 각각 0.6초쯤 머문다.
 *
 * 호출 체인:
 *   ctrl_read → [이 함수] → readl(), writel(), msleep()
 */
static u8 i2c_ctrl_read(struct controller *ctlr_ptr, void __iomem *WPGBbar, u8 index)
{
	/* [한국어] 메시지 버퍼에서 읽어 낸 결과 바이트 */
	u8 status;
	/* [한국어] 두 대기 루프의 남은 횟수. 0 이 되면 시간 초과다 */
	int i;
	/* [한국어] 지금 접근할 레지스터의 절대 주소. 창 시작에 오프셋을 더해 만든다 */
	void __iomem *wpg_addr;	// base addr + offset
	/* [한국어] **레지스터에 오가는 LOHI 형식 값.**
	 * CPU 가 쓰는 HILO 와 바이트 순서가 반대라 swab32 로 뒤집어 다룬다 */
	unsigned long wpg_data;	// data to/from WPG LOHI format
	/* [한국어] I2C 주소나 인덱스를 자리에 맞게 미는 임시 변수 */
	unsigned long ultemp;
	/* [한국어] **CPU 쪽 HILO 형식 값.** 사람이 읽는 쪽은 늘 이 변수다 */
	unsigned long data;	// actual data HILO format

	/* [한국어] **to_debug 가 켜져 있을 때만 찍는다.**
	 * 폴링이 2초마다 이 경로를 도므로 평소에는 막아 둔다 */
	debug_polling("%s - Entry WPGBbar[%p] index[%x]\n", __func__, WPGBbar, index);

	//--------------------------------------------------------------------
	// READ - step 1
	// read at address, byte length, I2C address (shifted), index
	// or read direct, byte length, index
	/* [한국어] **1단계 -- 동작 설정. ctlr_type 이 두 방식을 가른다.**
	 * 0x02 는 I2C 주소를 실어 보내는 방식이다 */
	if (ctlr_ptr->ctlr_type == 0x02) {
		/* [한국어] 읽기 + 바이트 길이 + 주소 지정 조합을 바탕값으로 삼는다 */
		data = WPG_READATADDR_MASK;
		// fill in I2C address
		/* [한국어] **union u 의 wpeg_ctlr 갈래에서 I2C 주소를 꺼낸다.**
		 * ctlr_type 이 2 나 4 일 때만 그 갈래가 유효하다 */
		ultemp = (unsigned long)ctlr_ptr->u.wpeg_ctlr.i2c_addr;
		/* [한국어] **한 비트 오른쪽으로 민다.**
		 * I2C 의 7비트 주소가 버스에서는 왼쪽으로 한 칸 밀린 채 실리므로,
		 * 그것을 되돌려 순수한 주소를 얻는다 */
		ultemp = ultemp >> 1;
		/* [한국어] **8비트 왼쪽으로 밀어 주소 필드 자리에 넣는다** */
		data |= (ultemp << 8);

		// fill in index
		/* [한국어] **인덱스는 맨 아래 바이트에 그대로 넣는다.**
		 * 주소 지정 방식에서는 시프트가 없다 */
		data |= (unsigned long)index;
	/* [한국어] 0x04 는 확장 상자용이며 **주소 없이 인덱스만 보낸다** */
	} else if (ctlr_ptr->ctlr_type == 0x04) {
		/* [한국어] 직접 읽기 조합을 바탕값으로 삼는다 */
		data = WPG_READDIRECT_MASK;

		// fill in index
		/* [한국어] 인덱스를 임시 변수에 옮긴다 */
		ultemp = (unsigned long)index;
		/* [한국어] **8비트 왼쪽으로 민다.**
		 * 주소 지정 방식과 인덱스가 들어가는 자리가 다르다 */
		ultemp = ultemp << 8;
		/* [한국어] 밀어 둔 인덱스를 넣는다 */
		data |= ultemp;
	/* [한국어] **0x02 도 0x04 도 아닌 종류가 이 함수로 왔다.**
	 * 호출자 ctrl_read() 가 그 둘만 이리로 보내므로 실제로는 닿지 않는다 */
	} else {
		/* [한국어] 지원하지 않는 종류임을 남긴다 */
		err("this controller type is not supported\n");
		/* [한국어] 오류를 알린다 */
		return HPC_ERROR;
	}

	/* [한국어] **쓰기 전에 바이트 순서를 뒤집는다.**
	 * 원문 주석이 그 뜻을 밝힌다 */
	wpg_data = swab32(data);	// swap data before writing
	/* [한국어] 동작 설정 레지스터의 주소를 만든다 */
	wpg_addr = WPGBbar + WPG_I2CMOSUP_OFFSET;
	/* [한국어] **무엇을 어떻게 읽을지를 컨트롤러에 알린다** */
	writel(wpg_data, wpg_addr);

	//--------------------------------------------------------------------
	// READ - step 2 : clear the message buffer
	/* [한국어] **2단계 -- 메시지 버퍼를 0 으로 지운다.**
	 * 읽기이므로 보낼 값이 없고, 결과가 여기 담겨 돌아온다 */
	data = 0x00000000;
	/* [한국어] 0 이라 뒤집어도 같지만 형식을 맞춘다 */
	wpg_data = swab32(data);
	/* [한국어] 메시지 버퍼의 주소를 만든다 */
	wpg_addr = WPGBbar + WPG_I2CMBUFL_OFFSET;
	/* [한국어] 버퍼를 비운다 */
	writel(wpg_data, wpg_addr);

	//--------------------------------------------------------------------
	// READ - step 3 : issue start operation, I2C master control bit 30:ON
	//                 2020 : [20] OR operation at [20] offset 0x20
	/* [한국어] **3단계 -- 시작 비트만 담은 값을 만든다** */
	data = WPG_I2CMCNTL_STARTOP_MASK;
	/* [한국어] 바이트 순서를 뒤집는다 */
	wpg_data = swab32(data);
	/* [한국어] **오프셋에 OR 창을 더한다.**
	 * 그 창에 쓰면 대입이 아니라 OR 가 되므로,
	 * 다른 비트를 읽어 오지 않고도 시작 비트 하나만 세울 수 있다.
	 * 원문 주석이 그 방식을 밝힌다 */
	wpg_addr = WPGBbar + WPG_I2CMCNTL_OFFSET + WPG_I2C_OR;
	/* [한국어] **동작이 시작된다** */
	writel(wpg_data, wpg_addr);

	//--------------------------------------------------------------------
	// READ - step 4 : wait until start operation bit clears
	/* [한국어] **4단계 -- 남은 횟수를 채운다.**
	 * 10밀리초씩 세므로 실제로는 0.6초쯤이다 -- 상수 이름의 SEC 와 어긋난다 */
	i = CMD_COMPLETE_TOUT_SEC;
	/* [한국어] 시작 비트가 내려갈 때까지 돈다 */
	while (i) {
		/* [한국어] **먼저 자고 그다음 읽는다.** 하드웨어가 시작할 틈을 준다 */
		msleep(10);
		/* [한국어] **여기서는 OR 창을 더하지 않는다.** 읽기이기 때문이다 */
		wpg_addr = WPGBbar + WPG_I2CMCNTL_OFFSET;
		/* [한국어] 마스터 제어 레지스터를 읽는다 */
		wpg_data = readl(wpg_addr);
		/* [한국어] 바이트 순서를 되돌린다 */
		data = swab32(wpg_data);
		/* [한국어] **시작 비트가 내려갔는가.**
		 * 하드웨어가 동작을 마치면 스스로 내린다 */
		if (!(data & WPG_I2CMCNTL_STARTOP_MASK))
			/* [한국어] 끝났으므로 빠져나간다 */
			break;
		/* [한국어] 남은 횟수를 줄인다 */
		i--;
	}
	/* [한국어] **끝까지 내려가지 않았다.**
	 * break 로 나왔다면 i 는 0 이 아니다 */
	if (i == 0) {
		/* [한국어] 시간 초과를 남긴다. **debug_polling 이 아니라 debug 라 늘 찍힌다** */
		debug("%s - Error : WPG timeout\n", __func__);
		/* [한국어] **읽기는 여기서 곧바로 물러난다** -- 쓰기 쪽이 계속 진행하는 것과 다르다 */
		return HPC_ERROR;
	}
	//--------------------------------------------------------------------
	// READ - step 5 : read I2C status register
	/* [한국어] **5단계 -- 상태 확인용 남은 횟수를 다시 채운다** */
	i = CMD_COMPLETE_TOUT_SEC;
	/* [한국어] I2C 상태가 정상이 될 때까지 돈다 */
	while (i) {
		/* [한국어] 10밀리초 양보한다 */
		msleep(10);
		/* [한국어] I2C 상태 레지스터의 주소를 만든다 */
		wpg_addr = WPGBbar + WPG_I2CSTAT_OFFSET;
		/* [한국어] 상태를 읽는다 */
		wpg_data = readl(wpg_addr);
		/* [한국어] 바이트 순서를 되돌린다 */
		data = swab32(wpg_data);
		/* [한국어] **오류 비트 일곱 개가 모두 꺼져 있는가.**
		 * 그 매크로가 1 을 주면 정상이다 */
		if (HPC_I2CSTATUS_CHECK(data))
			/* [한국어] 정상이므로 빠져나간다 */
			break;
		/* [한국어] 남은 횟수를 줄인다 */
		i--;
	}
	/* [한국어] 끝까지 정상이 되지 않았다 */
	if (i == 0) {
		/* [한국어] **함수 이름 대신 "ctrl_read" 라 적혀 있다.**
		 * 윗단계 메시지가 __func__ 를 쓰는 것과 어긋나며, 상류 그대로다 */
		debug("ctrl_read - Exit Error:I2C timeout\n");
		/* [한국어] 오류를 알린다 */
		return HPC_ERROR;
	}

	//--------------------------------------------------------------------
	// READ - step 6 : get DATA
	/* [한국어] **6단계 -- 결과가 담긴 메시지 버퍼의 주소를 만든다** */
	wpg_addr = WPGBbar + WPG_I2CMBUFL_OFFSET;
	/* [한국어] 버퍼를 읽는다 */
	wpg_data = readl(wpg_addr);
	/* [한국어] 바이트 순서를 되돌린다 */
	data = swab32(wpg_data);

	/* [한국어] **하위 한 바이트만 결과다.** 나머지는 버린다 */
	status = (u8) data;

	/* [한국어] 읽어 낸 값을 남긴다. 진입 로그와 짝을 이룬다 */
	debug_polling("%s - Exit index[%x] status[%x]\n", __func__, index, status);

	/* [한국어] 읽은 바이트를 돌려준다 */
	return (status);
}

/*----------------------------------------------------------------------
* Name:    i2c_ctrl_write
*
* Action:  write to HPC over I2C
*
* Return   0 or error codes
*---------------------------------------------------------------------*/
/* [한국어]
 * i2c_ctrl_write - Winnipeg I2C 를 거쳐 컨트롤러에 한 바이트를 쓴다
 *
 * @ctlr_ptr: 대상 컨트롤러.
 * @WPGBbar: ioremap 해 둔 Winnipeg 레지스터 창의 시작.
 * @index: 쓸 대상의 컨트롤러 안 인덱스.
 * @cmd: 써 넣을 명령 바이트.
 * @return: 성공 0, 실패하면 HPC_ERROR.
 *
 * **i2c_ctrl_read() 와 거울처럼 닮았고 다른 곳이 둘뿐이다.**
 * 동작 설정에 READ 대신 WRITE 마스크를 쓰고, 2단계에서 메시지 버퍼를
 * 0 으로 지우는 대신 **보낼 명령 바이트를 넣는다.**
 *
 * **단계 이름은 그대로 "clear the message buffer" 로 남아 있는데 실제로는
 * 값을 채운다** -- 읽기 쪽에서 복사해 온 주석으로 보인다.
 * 코드는 손대지 않고 사실만 적는다.
 *
 * **오류 처리 방식이 읽기 쪽과 다르다.** 읽기는 시간 초과 때 곧바로
 * HPC_ERROR 를 돌려주지만, 쓰기는 rc 에 담아 두고 **다음 단계를 계속
 * 진행한다.** 그래서 4단계에서 시간이 초과되어도 5단계의 상태 확인이
 * 실행된다. 그 선택의 이유가 코드에 적혀 있지는 않다.
 *
 * **5단계 실패 메시지가 "ctrl_read" 로 시작한다** -- 읽기 함수에서
 * 복사해 온 문구다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. msleep 으로 잠든다.
 *
 * 호출 체인:
 *   ctrl_write → [이 함수] → readl(), writel(), msleep()
 */
static u8 i2c_ctrl_write(struct controller *ctlr_ptr, void __iomem *WPGBbar, u8 index, u8 cmd)
{
	/* [한국어] **결과를 담아 두는 변수.**
	 * 읽기 쪽이 곧바로 return 하는 것과 달리 이쪽은 여기 담고 계속 진행한다 */
	u8 rc;
	/* [한국어] 지금 접근할 레지스터의 절대 주소 */
	void __iomem *wpg_addr;	// base addr + offset
	/* [한국어] 레지스터에 오가는 LOHI 형식 값 */
	unsigned long wpg_data;	// data to/from WPG LOHI format
	/* [한국어] 주소나 인덱스를 미는 임시 변수 */
	unsigned long ultemp;
	/* [한국어] CPU 쪽 HILO 형식 값 */
	unsigned long data;	// actual data HILO format
	/* [한국어] 대기 루프의 남은 횟수. **읽기 쪽과 선언 순서가 다르다** */
	int i;

	/* [한국어] 진입을 남긴다. **읽기와 달리 보낼 명령까지 찍는다** */
	debug_polling("%s - Entry WPGBbar[%p] index[%x] cmd[%x]\n", __func__, WPGBbar, index, cmd);

	/* [한국어] 성공을 기본값으로 둔다 */
	rc = 0;
	//--------------------------------------------------------------------
	// WRITE - step 1
	// write at address, byte length, I2C address (shifted), index
	// or write direct, byte length, index
	/* [한국어] **바탕값을 비운다.**
	 * 아래 두 갈래가 곧바로 덮어쓰므로 실질적 효과는 없다 */
	data = 0x00000000;

	/* [한국어] **1단계 -- 주소 지정 방식** */
	if (ctlr_ptr->ctlr_type == 0x02) {
		/* [한국어] 쓰기 + 주소 지정 조합 */
		data = WPG_WRITEATADDR_MASK;
		// fill in I2C address
		/* [한국어] I2C 주소를 꺼낸다 */
		ultemp = (unsigned long)ctlr_ptr->u.wpeg_ctlr.i2c_addr;
		/* [한국어] 버스에서 밀려 실리는 한 비트를 되돌린다 */
		ultemp = ultemp >> 1;
		/* [한국어] 주소 필드 자리에 넣는다 */
		data |= (ultemp << 8);

		// fill in index
		/* [한국어] 인덱스를 맨 아래 바이트에 넣는다 */
		data |= (unsigned long)index;
	/* [한국어] **직접 방식 -- 확장 상자용** */
	} else if (ctlr_ptr->ctlr_type == 0x04) {
		/* [한국어] 쓰기 + 직접 조합 */
		data = WPG_WRITEDIRECT_MASK;

		// fill in index
		/* [한국어] 인덱스를 임시 변수에 옮긴다 */
		ultemp = (unsigned long)index;
		/* [한국어] 직접 방식의 인덱스 자리로 민다 */
		ultemp = ultemp << 8;
		/* [한국어] 넣는다 */
		data |= ultemp;
	/* [한국어] 지원하지 않는 종류. 호출자가 걸러 오므로 닿지 않는다 */
	} else {
		/* [한국어] 오류를 남긴다 */
		err("this controller type is not supported\n");
		/* [한국어] 물러난다 */
		return HPC_ERROR;
	}

	/* [한국어] 쓰기 전에 바이트 순서를 뒤집는다 */
	wpg_data = swab32(data);	// swap data before writing
	/* [한국어] 동작 설정 레지스터의 주소 */
	wpg_addr = WPGBbar + WPG_I2CMOSUP_OFFSET;
	/* [한국어] 무엇을 어떻게 쓸지 알린다 */
	writel(wpg_data, wpg_addr);

	//--------------------------------------------------------------------
	// WRITE - step 2 : clear the message buffer
	/* [한국어] **2단계 -- 보낼 명령을 메시지 버퍼 값으로 만든다.**
	 * **단계 이름은 "clear the message buffer" 인데 실제로는 값을 채운다** --
	 * 읽기 쪽에서 복사해 온 주석이다.
	 * `0x00000000 |` 는 값에 아무 영향이 없다 */
	data = 0x00000000 | (unsigned long)cmd;
	/* [한국어] 바이트 순서를 뒤집는다 */
	wpg_data = swab32(data);
	/* [한국어] 메시지 버퍼의 주소 */
	wpg_addr = WPGBbar + WPG_I2CMBUFL_OFFSET;
	/* [한국어] **보낼 명령을 버퍼에 넣는다** */
	writel(wpg_data, wpg_addr);

	//--------------------------------------------------------------------
	// WRITE - step 3 : issue start operation,I2C master control bit 30:ON
	//                 2020 : [20] OR operation at [20] offset 0x20
	/* [한국어] **3단계 -- 시작 비트만 담은 값** */
	data = WPG_I2CMCNTL_STARTOP_MASK;
	/* [한국어] 바이트 순서를 뒤집는다 */
	wpg_data = swab32(data);
	/* [한국어] OR 창을 더해 다른 비트를 건드리지 않는다 */
	wpg_addr = WPGBbar + WPG_I2CMCNTL_OFFSET + WPG_I2C_OR;
	/* [한국어] **동작이 시작된다** */
	writel(wpg_data, wpg_addr);

	//--------------------------------------------------------------------
	// WRITE - step 4 : wait until start operation bit clears
	/* [한국어] **4단계 -- 남은 횟수를 채운다** */
	i = CMD_COMPLETE_TOUT_SEC;
	/* [한국어] 시작 비트가 내려갈 때까지 돈다 */
	while (i) {
		/* [한국어] 10밀리초 양보한다 */
		msleep(10);
		/* [한국어] 읽기이므로 OR 창을 더하지 않는다 */
		wpg_addr = WPGBbar + WPG_I2CMCNTL_OFFSET;
		/* [한국어] 마스터 제어 레지스터를 읽는다 */
		wpg_data = readl(wpg_addr);
		/* [한국어] 바이트 순서를 되돌린다 */
		data = swab32(wpg_data);
		/* [한국어] 시작 비트가 내려갔는가 */
		if (!(data & WPG_I2CMCNTL_STARTOP_MASK))
			/* [한국어] 끝났다 */
			break;
		/* [한국어] 남은 횟수를 줄인다 */
		i--;
	}
	/* [한국어] 시간이 초과되었다 */
	if (i == 0) {
		/* [한국어] 시간 초과를 남긴다 */
		debug("%s - Exit Error:WPG timeout\n", __func__);
		/* [한국어] **읽기와 달리 곧바로 물러나지 않고 결과만 담는다.**
		 * 그래서 아래 5단계가 그대로 실행된다. 그 선택의 이유가 코드에 없다 */
		rc = HPC_ERROR;
	}

	//--------------------------------------------------------------------
	// WRITE - step 5 : read I2C status register
	/* [한국어] **5단계 -- 상태 확인용 남은 횟수를 다시 채운다** */
	i = CMD_COMPLETE_TOUT_SEC;
	/* [한국어] I2C 상태가 정상이 될 때까지 돈다 */
	while (i) {
		/* [한국어] 10밀리초 양보한다 */
		msleep(10);
		/* [한국어] I2C 상태 레지스터의 주소 */
		wpg_addr = WPGBbar + WPG_I2CSTAT_OFFSET;
		/* [한국어] 상태를 읽는다 */
		wpg_data = readl(wpg_addr);
		/* [한국어] 바이트 순서를 되돌린다 */
		data = swab32(wpg_data);
		/* [한국어] 오류 비트가 모두 꺼져 있는가 */
		if (HPC_I2CSTATUS_CHECK(data))
			/* [한국어] 정상이므로 빠져나간다 */
			break;
		/* [한국어] 남은 횟수를 줄인다 */
		i--;
	}
	/* [한국어] 시간이 초과되었다 */
	if (i == 0) {
		/* [한국어] **쓰기 함수인데 메시지가 "ctrl_read" 로 시작한다** --
		 * 읽기 함수에서 복사해 온 문구이며 상류 그대로다 */
		debug("ctrl_read - Error : I2C timeout\n");
		/* [한국어] 결과에 오류를 담는다 */
		rc = HPC_ERROR;
	}

	/* [한국어] 결과를 남긴다. **다른 로그와 달리 함수 이름 뒤에 하이픈이 없다** */
	debug_polling("%s Exit rc[%x]\n", __func__, rc);
	/* [한국어] **성공이든 실패든 여기 한 자리에서 돌아간다** */
	return (rc);
}

//------------------------------------------------------------
//  Read from ISA type HPC
//------------------------------------------------------------
/* [한국어]
 * isa_ctrl_read - ISA IO 포트로 컨트롤러에서 한 바이트를 읽는다
 *
 * @ctlr_ptr: 대상 컨트롤러. IO 시작 주소를 여기서 얻는다.
 * @offset: 시작 주소에서의 오프셋.
 * @return: 읽은 바이트.
 *
 * **네 전송 방식 중 가장 단순하다.** 컨트롤러가 ISA IO 공간에 직접
 * 붙어 있으므로 inb 한 번이면 끝난다 -- I2C 처럼 여섯 단계를 밟거나
 * 완료를 기다릴 필요가 없다.
 *
 * **io_start 는 union u 의 isa_ctlr 갈래에 있다.** ctlr_type 이 0 일 때만
 * 그 갈래가 유효하며, 그것을 확인하는 일은 호출자 ctrl_read() 가 한다.
 *
 * **오류를 알릴 방법이 없다.** 포트가 없거나 응답하지 않으면 0xFF 가
 * 읽히는데, 그것과 진짜 0xFF 를 구분하지 못한다.
 *
 * **inb 는 x86 계열의 IO 포트 명령이다.** 그래서 이 갈래는 그 아키텍처
 * 에서만 뜻이 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 잠들지 않는다.
 *
 * 호출 체인:
 *   ctrl_read → [이 함수] → inb()
 */
static u8 isa_ctrl_read(struct controller *ctlr_ptr, u8 offset)
{
	/* [한국어] 컨트롤러가 붙어 있는 IO 포트의 시작 주소 */
	u16 start_address;
	/* [한국어] 읽은 바이트 */
	u8 data;

	/* [한국어] **union u 의 isa_ctlr 갈래에서 IO 시작 주소를 꺼낸다.**
	 * ctlr_type 이 0 일 때만 그 갈래가 유효하며,
	 * 확인은 호출자 ctrl_read() 가 이미 했다 */
	start_address = ctlr_ptr->u.isa_ctlr.io_start;
	/* [한국어] **IO 포트에서 한 바이트를 읽는다.**
	 * I2C 처럼 여섯 단계를 밟거나 완료를 기다릴 필요가 없다 --
	 * 포트 읽기는 그 자리에서 끝난다.
	 * **inb 는 x86 계열의 명령이라 이 갈래는 그 아키텍처에서만 뜻이 있다.**
	 * **응답이 없으면 0xFF 가 읽히는데 진짜 0xFF 와 구별되지 않는다** */
	data = inb(start_address + offset);
	/* [한국어] 읽은 바이트를 그대로 돌려준다 */
	return data;
}

//--------------------------------------------------------------
// Write to ISA type HPC
//--------------------------------------------------------------
/* [한국어]
 * isa_ctrl_write - ISA IO 포트로 컨트롤러에 한 바이트를 쓴다
 *
 * @ctlr_ptr: 대상 컨트롤러.
 * @offset: 시작 주소에서의 오프셋.
 * @data: 쓸 바이트.
 * @return: 없음.
 *
 * **isa_ctrl_read() 의 짝이며 outb 한 번이 전부다.**
 *
 * **반환형이 void 인 것이 다른 write 갈래와 다르다.** pci_ctrl_write 와
 * i2c_ctrl_write 는 u8 을 돌려주는데 이쪽만 없다. 그래서 호출자
 * ctrl_write() 는 ISA 갈래에서 rc 를 갱신하지 않고 초기값 0 을 그대로
 * 돌려준다 -- 곧 **ISA 쓰기는 늘 성공으로 보고된다.**
 * 코드는 손대지 않고 사실만 적는다.
 *
 * **port_address 를 따로 계산해 두는 것이 읽기 쪽과 다르다.** 읽기는
 * inb 인자 안에서 바로 더하는데 이쪽은 변수를 하나 더 둔다.
 * 동작은 같다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 잠들지 않는다.
 *
 * 호출 체인:
 *   ctrl_write → [이 함수] → outb()
 */
static void isa_ctrl_write(struct controller *ctlr_ptr, u8 offset, u8 data)
{
	/* [한국어] IO 포트의 시작 주소 */
	u16 start_address;
	/* [한국어] **실제로 쓸 포트 주소.**
	 * 읽기 쪽이 inb 인자 안에서 바로 더하는 것과 달리 변수를 하나 더 둔다 */
	u16 port_address;

	/* [한국어] IO 시작 주소를 꺼낸다 */
	start_address = ctlr_ptr->u.isa_ctlr.io_start;
	/* [한국어] 오프셋을 더해 목적지 주소를 만든다 */
	port_address = start_address + (u16) offset;
	/* [한국어] **IO 포트에 한 바이트를 쓴다.**
	 * **성공 여부를 알 방법이 없고, 그래서 이 함수는 void 다** --
	 * 다른 두 쓰기 갈래가 u8 을 돌려주는 것과 다르며,
	 * 그 결과 ctrl_write() 는 ISA 쓰기를 늘 성공으로 보고한다 */
	outb(data, port_address);
}

/* [한국어]
 * pci_ctrl_read - PCI 설정공간으로 컨트롤러에서 한 바이트를 읽는다
 *
 * @ctrl: 대상 컨트롤러.
 * @offset: HPC_PCI_OFFSET 에서의 오프셋.
 * @return: 읽은 바이트. 장치가 없으면 0.
 *
 * **컨트롤러가 PCI 장치로 붙어 있는 세대를 위한 갈래다.**
 * ctrl_dev 가 그 PCI 장치이며, ibmphp_ebda.c 가 EBDA 를 읽고 찾아 둔 것이다.
 *
 * **HPC_PCI_OFFSET 을 더하는 것이 요점이다.** 컨트롤러 레지스터가
 * 설정공간의 표준 헤더 뒤 벤더 고유 영역에 있기 때문이며,
 * 그 상수는 ibmphp.h 에 있다.
 *
 * **ctrl_dev 가 NULL 이면 0 을 돌려준다.** data 를 0x00 으로 초기화해 둔
 * 덕이며, **오류와 진짜 0 을 구분하지 못한다.**
 *
 * **debug 로 진입을 남긴다.** 이 파일의 다른 저수준 함수들이
 * debug_polling 을 쓰는 것과 달리 여기는 debug 라, 폴링 중에도 늘 찍힌다.
 * 코드는 손대지 않고 사실만 적는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   ctrl_read → [이 함수] → pci_read_config_byte()
 */
static u8 pci_ctrl_read(struct controller *ctrl, u8 offset)
{
	/* [한국어] **0 으로 초기화해 두는 것이 실패 시의 기본값을 겸한다.**
	 * ctrl_dev 가 없으면 이 값이 그대로 나가며, 진짜 0 과 구별되지 않는다 */
	u8 data = 0x00;
	/* [한국어] **debug_polling 이 아니라 debug 라 폴링 중에도 늘 찍힌다.**
	 * 이 파일의 다른 저수준 함수들과 다른 자리다.
	 * 코드는 손대지 않고 사실만 적는다 */
	debug("inside pci_ctrl_read\n");
	/* [한국어] **컨트롤러가 PCI 장치로 붙어 있는가.**
	 * ibmphp_ebda.c 가 EBDA 를 읽고 그 장치를 찾아 둔다 */
	if (ctrl->ctrl_dev)
		/* [한국어] **설정공간의 벤더 고유 영역에서 읽는다.**
		 * HPC_PCI_OFFSET 을 더하는 것은 컨트롤러 레지스터가 표준 헤더 뒤에
		 * 있기 때문이며, 그 상수는 ibmphp.h 에 있다.
		 * **읽기 실패를 확인하지 않는다** */
		pci_read_config_byte(ctrl->ctrl_dev, HPC_PCI_OFFSET + offset, &data);
	/* [한국어] 읽은 바이트, 또는 장치가 없으면 0 */
	return data;
}

/* [한국어]
 * pci_ctrl_write - PCI 설정공간으로 컨트롤러에 한 바이트를 쓴다
 *
 * @ctrl: 대상 컨트롤러.
 * @offset: HPC_PCI_OFFSET 에서의 오프셋.
 * @data: 쓸 바이트.
 * @return: 성공 0, 장치가 없으면 -ENODEV 를 u8 로 자른 값.
 *
 * **pci_ctrl_read() 의 짝이다.**
 *
 * **반환값에 눈에 띄는 점이 있다.** `u8 rc = -ENODEV;` 로 시작하는데
 * -ENODEV 는 -19 이고 u8 은 부호 없는 8비트라, 실제로 담기는 값은 237 이다.
 * 그것이 그대로 ctrl_write() 를 거쳐 위층까지 올라간다.
 * 호출자 ibmphp_hpc_writeslot() 은 ctrl_write() 의 반환값을 아예 받지
 * 않으므로 실질적 영향은 없으나, 오류 코드가 뜻대로 전달되지 않는 자리다.
 * 코드는 손대지 않고 사실만 적는다.
 *
 * **pci_write_config_byte 의 결과는 확인하지 않는다.** 쓰기가 실패해도
 * rc 는 0 이 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   ctrl_write → [이 함수] → pci_write_config_byte()
 */
static u8 pci_ctrl_write(struct controller *ctrl, u8 offset, u8 data)
{
	/* [한국어] **부호 없는 8비트에 음수를 넣는다.**
	 * -ENODEV 는 -19 이고 u8 로 잘리면 237 이 된다.
	 * 그 값이 ctrl_write() 를 거쳐 그대로 올라가나,
	 * 호출자 ibmphp_hpc_writeslot() 이 반환값을 받지 않으므로
	 * 실질적 영향은 없다. 코드는 손대지 않고 사실만 적는다 */
	u8 rc = -ENODEV;
	/* [한국어] 읽기 쪽과 마찬가지로 debug 라 늘 찍힌다 */
	debug("inside pci_ctrl_write\n");
	/* [한국어] PCI 장치가 있는가 */
	if (ctrl->ctrl_dev) {
		/* [한국어] **설정공간의 벤더 고유 영역에 쓴다.**
		 * **쓰기 결과를 확인하지 않는다** -- 실패해도 아래에서 0(성공)이 된다 */
		pci_write_config_byte(ctrl->ctrl_dev, HPC_PCI_OFFSET + offset, data);
		/* [한국어] 성공으로 바꾼다 */
		rc = 0;
	}
	/* [한국어] 성공 0, 장치가 없으면 잘린 -ENODEV */
	return rc;
}

/* [한국어]
 * ctrl_read - 컨트롤러 종류에 맞는 읽기 함수로 갈라 준다
 *
 * @ctlr: 대상 컨트롤러.
 * @base: I2C 갈래에서 쓸 ioremap 된 창. 다른 갈래에서는 쓰이지 않는다.
 * @offset: 읽을 대상의 인덱스 또는 오프셋.
 * @return: 읽은 바이트, 아는 종류가 아니면 -ENODEV 를 u8 로 자른 값.
 *
 * **이 파일의 추상화가 성립하는 자리다.** 위층은 컨트롤러가 ISA 에 있는지
 * PCI 에 있는지 I2C 뒤에 있는지 알 필요가 없고, 이 한 함수가 ctlr_type 을
 * 보고 갈라 준다.
 *
 * **네 갈래의 뜻이 이렇다.**
 * - 0 : ISA IO 포트. inb 한 번.
 * - 1 : PCI 설정공간. 벤더 영역을 읽는다.
 * - 2, 4 : Winnipeg I2C. 같은 함수를 쓰며 그 안에서 다시 갈린다 --
 *   2 는 I2C 주소를 실어 보내고 4 는 인덱스만 보낸다.
 *
 * **default 갈래가 u8 반환형에 -ENODEV 를 넣는다.** -19 가 u8 로 잘려
 * 237 이 되며, 그것이 정상적으로 읽힌 0xED 와 구별되지 않는다.
 * 다만 ctlr_type 은 ibmphp_ebda.c 가 EBDA 에서 읽어 정하므로
 * 실제로 이 갈래에 닿는 경우는 없는 것으로 보인다.
 *
 * **base 인자가 I2C 갈래에서만 쓰인다.** 다른 갈래에서는 NULL 이어도
 * 문제가 없으며, 실제로 ibmphp_hpc_readslot() 이 그렇게 넘긴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   ibmphp_hpc_readslot / hpc_wait_ctlr_notworking
 *     → [이 함수] → isa_ctrl_read / pci_ctrl_read / i2c_ctrl_read
 */
static u8 ctrl_read(struct controller *ctlr, void __iomem *base, u8 offset)
{
	/* [한국어] 아래층이 돌려준 값을 담는다 */
	u8 rc;
	/* [한국어] **이 파일의 추상화가 성립하는 자리.**
	 * ctlr_type 하나로 네 전송 방식을 가른다 */
	switch (ctlr->ctlr_type) {
	/* [한국어] ISA IO 포트 */
	case 0:
		/* [한국어] inb 한 번으로 끝난다 */
		rc = isa_ctrl_read(ctlr, offset);
		/* [한국어] 갈래를 마친다 */
		break;
	/* [한국어] PCI 설정공간 */
	case 1:
		/* [한국어] 벤더 고유 영역을 읽는다 */
		rc = pci_ctrl_read(ctlr, offset);
		/* [한국어] 갈래를 마친다 */
		break;
	/* [한국어] **Winnipeg I2C -- 주소 지정 방식** */
	case 2:
	/* [한국어] **Winnipeg I2C -- 직접 방식(확장 상자).**
	 * 두 종류가 같은 함수로 가고 그 안에서 다시 갈린다 */
	case 4:
		/* [한국어] **여기서만 base 인자가 쓰인다.**
		 * 다른 갈래에서는 NULL 이어도 문제가 없다 */
		rc = i2c_ctrl_read(ctlr, base, offset);
		/* [한국어] 갈래를 마친다 */
		break;
	/* [한국어] **아는 종류가 아니다.**
	 * ctlr_type 은 ibmphp_ebda.c 가 EBDA 에서 읽어 정하므로
	 * 실제로 이 갈래에 닿는 경우는 없는 것으로 보인다 */
	default:
		/* [한국어] **u8 반환형에 -19 를 넣어 237 이 된다.**
		 * 정상적으로 읽힌 0xED 와 구별되지 않는다.
		 * 코드는 손대지 않고 사실만 적는다 */
		return -ENODEV;
	}
	/* [한국어] 아래층이 돌려준 값을 그대로 올린다 */
	return rc;
}

/* [한국어]
 * ctrl_write - 컨트롤러 종류에 맞는 쓰기 함수로 갈라 준다
 *
 * @ctlr: 대상 컨트롤러.
 * @base: I2C 갈래에서 쓸 ioremap 된 창.
 * @offset: 쓸 대상의 인덱스 또는 오프셋.
 * @data: 쓸 바이트.
 * @return: 성공 0, 아는 종류가 아니면 -ENODEV 를 u8 로 자른 값.
 *
 * **ctrl_read() 의 짝이며 갈래가 같다.**
 *
 * **ISA 갈래만 반환값을 받지 않는다.** isa_ctrl_write 가 void 라
 * rc 가 초기값 0 그대로 남는다 -- 곧 **ISA 쓰기는 늘 성공으로 보고된다.**
 *
 * **유일한 호출자가 반환값을 쓰지 않는다.** ibmphp_hpc_writeslot() 이
 * `ctrl_write(...)` 를 값으로 받지 않고 그냥 부른다.
 * 그래서 아래층이 돌려준 오류가 위로 전달되지 않으며, 대신 그 함수는
 * 명령을 낸 뒤 컨트롤러 상태를 다시 읽어 완료를 확인한다.
 * 코드는 손대지 않고 사실만 적는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   ibmphp_hpc_writeslot
 *     → [이 함수] → isa_ctrl_write / pci_ctrl_write / i2c_ctrl_write
 */
static u8 ctrl_write(struct controller *ctlr, void __iomem *base, u8 offset, u8 data)
{
	/* [한국어] **성공을 기본값으로 둔다.**
	 * ISA 갈래가 반환값을 주지 않으므로 이 값이 그대로 나간다 */
	u8 rc = 0;
	/* [한국어] 읽기 쪽과 같은 네 갈래 */
	switch (ctlr->ctlr_type) {
	/* [한국어] ISA IO 포트 */
	case 0:
		/* [한국어] **반환값을 받지 않는다** -- 그 함수가 void 라
		 * ISA 쓰기는 늘 성공으로 보고된다 */
		isa_ctrl_write(ctlr, offset, data);
		/* [한국어] 갈래를 마친다 */
		break;
	/* [한국어] PCI 설정공간 */
	case 1:
		/* [한국어] 장치가 없으면 잘린 -ENODEV 가 담긴다 */
		rc = pci_ctrl_write(ctlr, offset, data);
		/* [한국어] 갈래를 마친다 */
		break;
	/* [한국어] Winnipeg I2C -- 주소 지정 방식 */
	case 2:
	/* [한국어] Winnipeg I2C -- 직접 방식 */
	case 4:
		/* [한국어] 여섯 단계 트랜잭션을 수행한다 */
		rc = i2c_ctrl_write(ctlr, base, offset, data);
		/* [한국어] 갈래를 마친다 */
		break;
	/* [한국어] 아는 종류가 아니다 */
	default:
		/* [한국어] u8 로 잘려 237 이 된다 */
		return -ENODEV;
	}
	/* [한국어] **유일한 호출자가 이 값을 받지 않는다** --
	 * ibmphp_hpc_writeslot() 은 대신 컨트롤러 상태를 다시 읽어 완료를 확인한다 */
	return rc;
}
/*----------------------------------------------------------------------
* Name:    hpc_writecmdtoindex()
*
* Action:  convert a write command to proper index within a controller
*
* Return   index, HPC_ERROR
*---------------------------------------------------------------------*/
/* [한국어]
 * hpc_writecmdtoindex - 쓰기 명령을 컨트롤러 안의 인덱스로 옮긴다
 *
 * @cmd: 낼 명령.
 * @index: 슬롯이나 버스의 상대 번호.
 * @return: 컨트롤러 안 인덱스, 모르는 명령이면 HPC_ERROR.
 *
 * **컨트롤러가 하나의 인덱스 공간에 여러 대상을 나란히 두기 때문에
 * 필요한 변환이다.** 그 배치가 상수 이름에 드러난다 --
 * WPG_1ST_SLOT_INDEX(0x01)부터 슬롯, WPG_CTLR_INDEX(0x0F)가 컨트롤러
 * 자신, WPG_1ST_EXTSLOT_INDEX(0x10)부터 확장 슬롯,
 * WPG_1ST_BUS_INDEX(0x1F)부터 버스다.
 *
 * **명령을 세 무리로 나눈다.**
 * 1. **컨트롤러 전체에 대한 명령** -- 인터럽트 켜기·끄기·지우기, 리셋,
 *    IRQ 스티어링, 모든 슬롯 켜기·끄기. 대상이 컨트롤러 자신이므로
 *    인덱스는 0x0F 로 고정된다.
 * 2. **슬롯 하나에 대한 명령** -- 켜기·끄기, 주의 표시등, 깜빡임.
 *    슬롯 번호를 그대로 인덱스로 쓴다.
 * 3. **버스에 대한 명령** -- 33/66MHz 통상 모드와 66/100/133MHz PCI-X 모드.
 *    버스 인덱스에 WPG_1ST_BUS_INDEX 를 더하고 1 을 뺀다 --
 *    버스 번호가 1 부터 세어지기 때문이다.
 *
 * **각 case 옆의 주석이 세 정보를 담는다** -- 명령 코드, 완료 확인이
 * 필요한지(Y/N), 그리고 유효한 인덱스 범위다. 그 표기법은 상류 주석의
 * 관행이며 이 파일 안에 설명이 없다.
 *
 * 실행 컨텍스트: 어디서든 부를 수 있다. 순수 계산이다.
 *
 * 호출 체인:
 *   ibmphp_hpc_writeslot → [이 함수]
 */
static u8 hpc_writecmdtoindex(u8 cmd, u8 index)
{
	/* [한국어] 옮겨 낸 인덱스 */
	u8 rc;

	/* [한국어] **명령을 세 무리로 나눈다** -- 컨트롤러 전체, 슬롯 하나, 버스 */
	switch (cmd) {
	/* [한국어] **여기부터 일곱은 컨트롤러 전체에 대한 명령이다.**
	 * 각 case 옆 주석의 표기는 명령 코드, 완료 확인 필요 여부(Y/N),
	 * 유효한 인덱스 범위를 뜻한다 */
	case HPC_CTLR_ENABLEIRQ:	// 0x00.N.15
	case HPC_CTLR_CLEARIRQ:	// 0x06.N.15
	case HPC_CTLR_RESET:	// 0x07.N.15
	case HPC_CTLR_IRQSTEER:	// 0x08.N.15
	case HPC_CTLR_DISABLEIRQ:	// 0x01.N.15
	case HPC_ALLSLOT_ON:	// 0x11.N.15
	case HPC_ALLSLOT_OFF:	// 0x12.N.15
		/* [한국어] **대상이 컨트롤러 자신이므로 WPG_CTLR_INDEX 로 고정된다** */
		rc = 0x0F;
		/* [한국어] 갈래를 마친다 */
		break;

	/* [한국어] **여기부터 다섯은 슬롯 하나에 대한 명령이다** --
	 * 끄기, 켜기, 주의 표시등 끄기·켜기, 깜빡임 */
	case HPC_SLOT_OFF:	// 0x02.Y.0-14
	case HPC_SLOT_ON:	// 0x03.Y.0-14
	case HPC_SLOT_ATTNOFF:	// 0x04.N.0-14
	case HPC_SLOT_ATTNON:	// 0x05.N.0-14
	case HPC_SLOT_BLINKLED:	// 0x13.N.0-14
		/* [한국어] **슬롯 번호를 그대로 인덱스로 쓴다.**
		 * 슬롯이 인덱스 공간의 맨 앞(0x01부터)에 있기 때문이다 */
		rc = index;
		/* [한국어] 갈래를 마친다 */
		break;

	/* [한국어] **여기부터 다섯은 버스 속도 모드 명령이다** --
	 * 33/66MHz 통상 모드와 66/100/133MHz PCI-X 모드 */
	case HPC_BUS_33CONVMODE:
	case HPC_BUS_66CONVMODE:
	case HPC_BUS_66PCIXMODE:
	case HPC_BUS_100PCIXMODE:
	case HPC_BUS_133PCIXMODE:
		/* [한국어] **버스 영역의 시작(0x1F)에 더하고 1 을 뺀다.**
		 * 버스 번호가 1 부터 세어지므로 0 기준으로 맞추는 것이다 */
		rc = index + WPG_1ST_BUS_INDEX - 1;
		/* [한국어] 갈래를 마친다 */
		break;

	/* [한국어] 아는 명령이 아니다 */
	default:
		/* [한국어] **오류 메시지를 남긴다.**
		 * 읽기 쪽 변환 함수가 조용히 물러나는 것과 다르다 */
		err("hpc_writecmdtoindex - Error invalid cmd[%x]\n", cmd);
		/* [한국어] 호출자가 이 값을 보고 -EINVAL 로 바꾼다 */
		rc = HPC_ERROR;
	}

	/* [한국어] 컨트롤러 안 인덱스를 돌려준다 */
	return rc;
}

/*----------------------------------------------------------------------
* Name:    hpc_readcmdtoindex()
*
* Action:  convert a read command to proper index within a controller
*
* Return   index, HPC_ERROR
*---------------------------------------------------------------------*/
/* [한국어]
 * hpc_readcmdtoindex - 읽기 명령을 컨트롤러 안의 인덱스로 옮긴다
 *
 * @cmd: 낼 읽기 명령.
 * @index: 슬롯이나 버스의 상대 번호.
 * @return: 컨트롤러 안 인덱스, 모르는 명령이면 HPC_ERROR.
 *
 * **hpc_writecmdtoindex() 의 읽기 쪽 짝이며 같은 인덱스 공간을 쓴다.**
 *
 * 여덟 갈래가 이렇다.
 * - READ_CTLRSTATUS -- 컨트롤러 자신이므로 0x0F.
 * - READ_SLOTSTATUS, READ_ALLSTAT -- 슬롯 번호를 그대로 쓴다.
 * - READ_EXTSLOTSTATUS -- 확장 슬롯 영역이므로 0x10 을 더한다.
 * - READ_BUSSTATUS -- 버스 영역이므로 0x1F 를 더하고 1 을 뺀다.
 * - **READ_SLOTLATCHLOWREG(0x28), READ_REVLEVEL(0x25),
 *   READ_HPCOPTIONS(0x27) -- 인덱스가 아니라 고정 주소다.**
 *   슬롯이나 버스가 아니라 컨트롤러의 특정 레지스터를 가리키므로
 *   index 인자를 아예 쓰지 않는다.
 *
 * **READ_SLOTLATCHLOWREG 가 이 파일에서 가장 자주 쓰인다** --
 * 폴링 스레드가 2초마다 이것으로 래치 상태를 확인한다.
 *
 * **write 쪽과 달리 default 가 오류 메시지를 남기지 않는다.**
 * HPC_ERROR 만 돌려주고 조용히 물러난다.
 *
 * 실행 컨텍스트: 어디서든 부를 수 있다. 순수 계산이다.
 *
 * 호출 체인:
 *   ibmphp_hpc_readslot → [이 함수]
 */
static u8 hpc_readcmdtoindex(u8 cmd, u8 index)
{
	/* [한국어] 옮겨 낸 인덱스 */
	u8 rc;

	/* [한국어] 읽기 명령 여덟 갈래 */
	switch (cmd) {
	/* [한국어] 컨트롤러 자신의 상태 */
	case READ_CTLRSTATUS:
		/* [한국어] 컨트롤러 인덱스로 고정 */
		rc = 0x0F;
		/* [한국어] 갈래를 마친다 */
		break;
	/* [한국어] 슬롯 상태 */
	case READ_SLOTSTATUS:
	/* [한국어] **슬롯 상태와 확장 상태를 함께 읽는 명령.**
	 * 인덱스 계산은 슬롯 상태와 같고, 확장 쪽은 호출자가 따로 더한다 */
	case READ_ALLSTAT:
		/* [한국어] 슬롯 번호를 그대로 쓴다 */
		rc = index;
		/* [한국어] 갈래를 마친다 */
		break;
	/* [한국어] 확장 슬롯 상태 */
	case READ_EXTSLOTSTATUS:
		/* [한국어] **확장 슬롯 영역의 시작(0x10)에 더한다.**
		 * 버스 쪽과 달리 1 을 빼지 않는다 -- 슬롯 번호가 0 부터이기 때문이다 */
		rc = index + WPG_1ST_EXTSLOT_INDEX;
		/* [한국어] 갈래를 마친다 */
		break;
	/* [한국어] 버스 상태 */
	case READ_BUSSTATUS:
		/* [한국어] 버스 영역의 시작에 더하고 1 을 뺀다. 쓰기 쪽과 같은 계산이다 */
		rc = index + WPG_1ST_BUS_INDEX - 1;
		/* [한국어] 갈래를 마친다 */
		break;
	/* [한국어] **래치 레지스터. 이 파일에서 가장 자주 읽히는 대상이다** --
	 * 폴링이 2초마다 이것을 본다 */
	case READ_SLOTLATCHLOWREG:
		/* [한국어] **인덱스가 아니라 고정 주소다.** index 인자를 쓰지 않는다 */
		rc = 0x28;
		/* [한국어] 갈래를 마친다 */
		break;
	/* [한국어] 컨트롤러 리비전 */
	case READ_REVLEVEL:
		/* [한국어] 고정 주소 */
		rc = 0x25;
		/* [한국어] 갈래를 마친다 */
		break;
	/* [한국어] 컨트롤러가 지원하는 기능 목록 */
	case READ_HPCOPTIONS:
		/* [한국어] 고정 주소 */
		rc = 0x27;
		/* [한국어] 갈래를 마친다 */
		break;
	/* [한국어] 아는 명령이 아니다 */
	default:
		/* [한국어] **쓰기 쪽과 달리 오류 메시지를 남기지 않는다** */
		rc = HPC_ERROR;
	}
	/* [한국어] 컨트롤러 안 인덱스 또는 고정 주소 */
	return rc;
}

/*----------------------------------------------------------------------
* Name:    HPCreadslot()
*
* Action:  issue a READ command to HPC
*
* Input:   pslot   - cannot be NULL for READ_ALLSTAT
*          pstatus - can be NULL for READ_ALLSTAT
*
* Return   0 or error codes
*---------------------------------------------------------------------*/
/* [한국어]
 * ibmphp_hpc_readslot - 컨트롤러에서 상태를 읽는다
 *
 * @pslot: 대상 슬롯. NULL 이면 안 된다.
 * @cmd: 무엇을 읽을지. READ_ 계열 아홉 가지.
 * @pstatus: 결과를 담아 돌려줄 자리. 일부 명령에서는 NULL 이어도 된다.
 * @return: 성공 0, 실패면 음수.
 *
 * **이 파일의 두 공개 진입점 중 하나이며, 읽기 쪽 전부를 맡는다.**
 * 위층(ibmphp_core.c)과 폴링 스레드가 모두 이것을 부른다.
 *
 * 절차가 다섯이다.
 * 1. **인자를 검증한다.** pstatus 가 NULL 이어도 되는 것은
 *    READ_ALLSTAT 과 READ_BUSSTATUS 뿐이다 -- 그 둘은 결과를 slot
 *    구조체에 직접 넣기 때문이다.
 * 2. **인덱스를 정한다.** 버스 상태면 ibmphp_get_bus_index() 로 버스
 *    번호를 얻고, 아니면 슬롯의 ctlr_index 를 쓴다. 그것을
 *    hpc_readcmdtoindex() 로 컨트롤러 안 주소로 옮긴다.
 * 3. **컨트롤러를 독점하고 I2C 창을 매핑한다.** ctlr_type 2 나 4 일 때만
 *    ioremap 이 필요하다 -- 나머지는 IO 포트나 설정공간을 쓴다.
 * 4. **컨트롤러가 놀고 있을 때까지 기다린 뒤 실제로 읽는다.**
 *    명령마다 결과를 어디에 담을지가 다르다.
 * 5. **매핑을 풀고 독점을 놓는다.**
 *
 * **READ_ALLSTAT 만 slot 구조체를 갱신한다.** 나머지는 주석이 밝히듯
 * "DO NOT update the slot structure" 이며 pstatus 로만 결과를 낸다.
 * 그 차이가 폴링 스레드의 동작을 좌우한다 -- 폴링은 READ_ALLSTAT 으로
 * 구조체를 갱신한 뒤 갱신 전 사본과 견주어 변화를 찾는다.
 *
 * **READ_ALLSTAT 은 두 번 읽는다.** 슬롯 상태와 확장 슬롯 상태를
 * 따로 읽어야 하고, 그 사이에 다시 완료를 기다린다 --
 * 컨트롤러가 한 번에 명령 하나만 처리하기 때문이다.
 *
 * **READ_ALLSLOT 갈래는 원문 주석이 "Not used" 라 밝힌다.**
 * 모든 슬롯을 훑는 코드가 남아 있으나 이 명령을 내는 곳이 없다.
 * **그 안에서 pslot 인자를 반복 변수로 덮어쓰는 것도 눈에 띈다** --
 * 호출자가 넘긴 슬롯을 잃게 되나, 쓰이지 않는 경로라 문제가 되지 않는다.
 *
 * **wpg_bbar 를 NULL 로 초기화해 두는 것이 요점이다.** I2C 가 아닌
 * 컨트롤러에서는 매핑하지 않고 그대로 아래층에 넘기는데, 그 갈래에서는
 * 쓰이지 않으므로 안전하다.
 *
 * **ioremap 의 실패를 확인하지 않는다.** NULL 이 돌아오면 I2C 갈래의
 * readl 이 그 주소를 쓰게 된다. 코드는 손대지 않고 사실만 적는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 뮤텍스를 잡고 msleep 으로 잠든다.
 *
 * 호출 체인:
 *   ibmphp_core.c 의 상태 조회 / poll_hpc / process_changeinstatus
 *     → [이 함수] → get_hpc_access(), ioremap(),
 *       hpc_wait_ctlr_notworking(), ctrl_read(), iounmap(), free_hpc_access()
 */
int ibmphp_hpc_readslot(struct slot *pslot, u8 cmd, u8 *pstatus)
{
	/* [한국어] **I2C 창의 매핑 주소. NULL 로 시작하는 것이 중요하다** --
	 * I2C 가 아닌 컨트롤러에서는 매핑하지 않고 그대로 아래층에 넘기는데,
	 * 그 갈래에서는 쓰이지 않으므로 안전하다 */
	void __iomem *wpg_bbar = NULL;
	/* [한국어] 슬롯이 붙어 있는 컨트롤러 */
	struct controller *ctlr_ptr;
	/* [한국어] index 는 옮겨 낸 컨트롤러 안 주소,
	 * status 는 hpc_wait_ctlr_notworking 이 채워 주는 컨트롤러 상태 */
	u8 index, status;
	/* [한국어] 각 단계의 결과이자 반환값 */
	int rc = 0;
	/* [한국어] **버스 상태를 읽을 때만 쓴다. int 인 것이 요점이다** --
	 * ibmphp_get_bus_index 가 음수로 실패를 알리므로 u8 로 받으면 안 된다 */
	int busindex;

	/* [한국어] 진입을 남긴다. **폴링이 자주 부르므로 debug_polling 을 쓴다** */
	debug_polling("%s - Entry pslot[%p] cmd[%x] pstatus[%p]\n", __func__, pslot, cmd, pstatus);

	/* [한국어] **인자 검증. pstatus 가 NULL 이어도 되는 명령이 둘 있다** --
	 * READ_ALLSTAT 과 READ_BUSSTATUS 는 결과를 slot 구조체에 직접 넣기 때문이다 */
	if ((pslot == NULL)
	    || ((pstatus == NULL) && (cmd != READ_ALLSTAT) && (cmd != READ_BUSSTATUS))) {
		/* [한국어] 잘못된 인자 */
		rc = -EINVAL;
		/* [한국어] 오류를 남긴다 */
		err("%s - Error invalid pointer, rc[%d]\n", __func__, rc);
		/* [한국어] **아직 아무것도 잡지 않았으므로 곧바로 나간다** */
		return rc;
	}

	/* [한국어] 버스 상태를 읽는 경우 */
	if (cmd == READ_BUSSTATUS) {
		/* [한국어] **슬롯의 버스 번호를 컨트롤러 안 버스 인덱스로 옮긴다**(ibmphp_ebda.c) */
		busindex = ibmphp_get_bus_index(pslot->bus);
		/* [한국어] 그 버스를 이 컨트롤러가 모른다 */
		if (busindex < 0) {
			/* [한국어] 잘못된 버스 */
			rc = -EINVAL;
			/* [한국어] 오류를 남긴다 */
			err("%s - Exit Error:invalid bus, rc[%d]\n", __func__, rc);
			/* [한국어] 물러난다 */
			return rc;
		/* [한국어] 버스를 찾았다 */
		} else
			/* [한국어] 음수가 아님을 확인한 뒤 u8 로 좁힌다 */
			index = (u8) busindex;
	/* [한국어] 버스 상태가 아닌 모든 명령 */
	} else
		/* [한국어] **슬롯의 컨트롤러 안 번호를 쓴다.**
		 * ibmphp_ebda.c 가 EBDA 를 읽어 넣어 둔 값이다 */
		index = pslot->ctlr_index;

	/* [한국어] **명령에 맞는 인덱스 공간의 주소로 옮긴다.**
	 * 같은 변수에 덮어쓰므로 이 뒤의 index 는 이미 변환된 값이다 */
	index = hpc_readcmdtoindex(cmd, index);

	/* [한국어] 아는 명령이 아니었다 */
	if (index == HPC_ERROR) {
		/* [한국어] 잘못된 명령 */
		rc = -EINVAL;
		/* [한국어] 오류를 남긴다 */
		err("%s - Exit Error:invalid index, rc[%d]\n", __func__, rc);
		/* [한국어] 물러난다 */
		return rc;
	}

	/* [한국어] 슬롯이 가리키는 컨트롤러를 꺼낸다 */
	ctlr_ptr = pslot->ctrl;

	/* [한국어] **컨트롤러를 독점한다.**
	 * 여기서부터 free_hpc_access() 까지가 임계 구역이며,
	 * 중간에 return 하는 경로가 없다 */
	get_hpc_access();

	//--------------------------------------------------------------------
	// map physical address to logical address
	//--------------------------------------------------------------------
	/* [한국어] **I2C 컨트롤러일 때만 창을 매핑한다.**
	 * ISA 와 PCI 는 IO 포트와 설정공간을 쓰므로 매핑이 필요 없다 */
	if ((ctlr_ptr->ctlr_type == 2) || (ctlr_ptr->ctlr_type == 4))
		/* [한국어] **물리 주소를 커널 주소로 매핑한다.**
		 * **실패(NULL)를 확인하지 않는다** -- 그 경우 아래층 readl 이 NULL 을 쓴다.
		 * 코드는 손대지 않고 사실만 적는다.
		 * **읽을 때마다 매핑하고 끝나면 푼다** -- 오래 들고 있지 않는 방식이다 */
		wpg_bbar = ioremap(ctlr_ptr->u.wpeg_ctlr.wpegbbar, WPG_I2C_IOREMAP_SIZE);

	//--------------------------------------------------------------------
	// check controller status before reading
	//--------------------------------------------------------------------
	/* [한국어] **컨트롤러가 놀고 있을 때까지 기다린다.**
	 * 이전 명령이 진행 중이면 지금 읽어도 값이 뜻이 없다.
	 * status 에 컨트롤러 상태가 담겨 돌아온다 */
	rc = hpc_wait_ctlr_notworking(HPC_CTLR_WORKING_TOUT, ctlr_ptr, wpg_bbar, &status);
	/* [한국어] 기다리기에 성공한 경우에만 실제로 읽는다 */
	if (!rc) {
		/* [한국어] **읽을 대상마다 결과를 어디에 담을지가 다르다** */
		switch (cmd) {
		/* [한국어] **슬롯 상태와 확장 상태를 함께 읽는 명령.**
		 * **이 갈래만 slot 구조체를 갱신한다** -- 폴링의 변화 탐지가 여기 기댄다 */
		case READ_ALLSTAT:
			// update the slot structure
			/* [한국어] **방금 기다리며 읽어 둔 컨트롤러 상태를 함께 저장한다** */
			pslot->ctrl->status = status;
			/* [한국어] 슬롯 상태 바이트를 읽어 구조체에 넣는다 */
			pslot->status = ctrl_read(ctlr_ptr, wpg_bbar, index);
			/* [한국어] **다시 기다린다.** 컨트롤러가 한 번에 명령 하나만 처리하므로
			 * 두 번째 읽기 전에 또 놀기를 기다려야 한다 */
			rc = hpc_wait_ctlr_notworking(HPC_CTLR_WORKING_TOUT, ctlr_ptr, wpg_bbar,
						       &status);
			/* [한국어] 기다리기에 성공했는가 */
			if (!rc)
				/* [한국어] **확장 슬롯 영역에서 두 번째 바이트를 읽는다.**
				 * 인덱스에 0x10 을 더해 확장 영역으로 옮긴다 */
				pslot->ext_status = ctrl_read(ctlr_ptr, wpg_bbar, index + WPG_1ST_EXTSLOT_INDEX);

			/* [한국어] 갈래를 마친다 */
			break;

		/* [한국어] 슬롯 상태만 읽는다 */
		case READ_SLOTSTATUS:
			// DO NOT update the slot structure
			/* [한국어] **구조체를 건드리지 않고 pstatus 로만 낸다.**
			 * 원문 주석이 그것을 못박는다 */
			*pstatus = ctrl_read(ctlr_ptr, wpg_bbar, index);
			/* [한국어] 갈래를 마친다 */
			break;

		/* [한국어] 확장 슬롯 상태만 읽는다 */
		case READ_EXTSLOTSTATUS:
			// DO NOT update the slot structure
			/* [한국어] 인덱스는 이미 변환 함수가 확장 영역으로 옮겨 두었다 */
			*pstatus = ctrl_read(ctlr_ptr, wpg_bbar, index);
			/* [한국어] 갈래를 마친다 */
			break;

		/* [한국어] 컨트롤러 상태 */
		case READ_CTLRSTATUS:
			// DO NOT update the slot structure
			/* [한국어] **여기서는 ctrl_read 를 부르지 않는다** --
			 * 위의 hpc_wait_ctlr_notworking 이 이미 컨트롤러 상태를 읽어 두었기 때문이다 */
			*pstatus = status;
			/* [한국어] 갈래를 마친다 */
			break;

		/* [한국어] 버스 상태 */
		case READ_BUSSTATUS:
			/* [한국어] **pstatus 가 아니라 구조체에 넣는다.**
			 * 그래서 이 명령은 pstatus 가 NULL 이어도 된다 */
			pslot->busstatus = ctrl_read(ctlr_ptr, wpg_bbar, index);
			/* [한국어] 갈래를 마친다 */
			break;
		/* [한국어] 컨트롤러 리비전 */
		case READ_REVLEVEL:
			/* [한국어] 고정 주소 0x25 에서 읽는다 */
			*pstatus = ctrl_read(ctlr_ptr, wpg_bbar, index);
			/* [한국어] 갈래를 마친다 */
			break;
		/* [한국어] 컨트롤러가 지원하는 기능 목록 */
		case READ_HPCOPTIONS:
			/* [한국어] 고정 주소 0x27 에서 읽는다 */
			*pstatus = ctrl_read(ctlr_ptr, wpg_bbar, index);
			/* [한국어] 갈래를 마친다 */
			break;
		/* [한국어] **래치 레지스터. 폴링이 2초마다 부르는 갈래다** */
		case READ_SLOTLATCHLOWREG:
			// DO NOT update the slot structure
			/* [한국어] 고정 주소 0x28 에서 한 바이트를 읽는다 --
			 * 그 안에 여러 슬롯의 래치 상태가 비트로 들어 있다 */
			*pstatus = ctrl_read(ctlr_ptr, wpg_bbar, index);
			/* [한국어] 갈래를 마친다 */
			break;

			// Not used
		/* [한국어] **원문 주석이 "Not used" 라 밝히는 갈래.**
		 * 이 명령을 내는 곳을 이 트리에서 찾을 수 없다 */
		case READ_ALLSLOT:
			/* [한국어] **전역 슬롯 목록을 모두 훑는다.**
			 * **pslot 인자를 반복 변수로 덮어쓰는 것이 눈에 띈다** --
			 * 호출자가 넘긴 슬롯을 잃게 되나, 쓰이지 않는 경로라 문제가 되지 않는다 */
			list_for_each_entry(pslot, &ibmphp_slot_head,
					    ibm_slot_list) {
				/* [한국어] 슬롯마다 인덱스를 다시 잡는다 */
				index = pslot->ctlr_index;
				/* [한국어] 슬롯마다 컨트롤러가 놀기를 기다린다 */
				rc = hpc_wait_ctlr_notworking(HPC_CTLR_WORKING_TOUT, ctlr_ptr,
								wpg_bbar, &status);
				/* [한국어] 기다리기에 성공했는가 */
				if (!rc) {
					/* [한국어] 슬롯 상태를 읽는다 */
					pslot->status = ctrl_read(ctlr_ptr, wpg_bbar, index);
					/* [한국어] 확장 상태를 읽기 전에 또 기다린다 */
					rc = hpc_wait_ctlr_notworking(HPC_CTLR_WORKING_TOUT,
									ctlr_ptr, wpg_bbar, &status);
					/* [한국어] 기다리기에 성공했는가 */
					if (!rc)
						/* [한국어] 확장 슬롯 상태를 읽어 넣는다 */
						pslot->ext_status =
						    ctrl_read(ctlr_ptr, wpg_bbar,
								index + WPG_1ST_EXTSLOT_INDEX);
				/* [한국어] 기다리기에 실패했다 */
				} else {
					/* [한국어] **메시지가 ctrl_read 실패라 하는데 실제로 실패한 것은
					 * hpc_wait_ctlr_notworking 이다.** 코드는 손대지 않고 사실만 적는다 */
					err("%s - Error ctrl_read failed\n", __func__);
					/* [한국어] 오류로 바꾼다 */
					rc = -EINVAL;
					/* [한국어] **안쪽 반복문을 빠져나간다.** 남은 슬롯은 읽지 않는다 */
					break;
				}
			}
			/* [한국어] 바깥 switch 의 갈래를 마친다 */
			break;
		/* [한국어] 아는 명령이 아니다 */
		default:
			/* [한국어] **변환 함수가 이미 걸렀으므로 실제로는 닿기 어렵다** */
			rc = -EINVAL;
			/* [한국어] 갈래를 마친다 */
			break;
		}
	}
	//--------------------------------------------------------------------
	// cleanup
	//--------------------------------------------------------------------

	// remove physical to logical address mapping
	/* [한국어] **매핑했던 경우에만 푼다.** 매핑 조건과 같은 식이다 */
	if ((ctlr_ptr->ctlr_type == 2) || (ctlr_ptr->ctlr_type == 4))
		/* [한국어] 창 매핑을 놓는다 */
		iounmap(wpg_bbar);

	/* [한국어] **컨트롤러 독점을 놓는다.**
	 * **이 함수의 모든 성공 경로가 여기를 지난다** -- 임계 구역 안에서
	 * 중간에 return 하는 자리가 없기 때문이다 */
	free_hpc_access();

	/* [한국어] 결과를 남긴다. 진입 로그와 짝을 이룬다 */
	debug_polling("%s - Exit rc[%d]\n", __func__, rc);
	/* [한국어] 성공 0, 실패면 음수 */
	return rc;
}

/*----------------------------------------------------------------------
* Name:    ibmphp_hpc_writeslot()
*
* Action: issue a WRITE command to HPC
*---------------------------------------------------------------------*/
/* [한국어]
 * ibmphp_hpc_writeslot - 컨트롤러에 명령을 내고 완료를 기다린다
 *
 * @pslot: 대상 슬롯. NULL 이면 안 된다.
 * @cmd: 낼 명령. HPC_ 계열.
 * @return: 성공 0, 실패면 음수.
 *
 * **이 파일의 두 공개 진입점 중 다른 하나이며, 쓰기 쪽 전부를 맡는다.**
 * 사용자가 sysfs 로 슬롯을 켜고 끄는 일이 결국 여기로 온다.
 *
 * 앞머리는 ibmphp_hpc_readslot() 과 같은 모양이다 -- 인자 검증,
 * 인덱스 변환, 컨트롤러 독점, I2C 창 매핑, 완료 대기.
 * **버스 속도 명령 다섯 가지만 버스 인덱스를 쓰고 나머지는 슬롯 인덱스를 쓴다.**
 *
 * **읽기와 결정적으로 다른 것은 명령을 낸 뒤의 절반이다.**
 * ctrl_write() 로 명령을 낸 다음, 컨트롤러가 그 명령을 마쳤는지
 * 반복해서 확인한다. 그 확인이 두 겹이다.
 * 1. hpc_wait_ctlr_notworking() -- 컨트롤러가 일을 놓았는가.
 * 2. **NEEDTOCHECK_CMDSTATUS(cmd) 가 참이면** CTLR_FINISHED(status) 까지
 *    본다. 명령에 따라 "일을 놓았다" 만으로는 부족하고 완료 표시를
 *    따로 확인해야 하기 때문이다 -- hpc_writecmdtoindex() 의 case 주석에
 *    붙은 Y/N 이 그 구분이다.
 *
 * **1초씩 최대 CMD_COMPLETE_TOUT_SEC 번 기다린다.** 시간이 초과되면
 * -EFAULT 를 담고 루프를 끝낸다.
 *
 * **ctrl_write() 의 반환값을 받지 않는다.** 아래층이 돌려준 오류가
 * 위로 전달되지 않으며, 대신 위의 완료 확인이 그 역할을 대신한다.
 * 코드는 손대지 않고 사실만 적는다.
 *
 * **타임아웃 검사가 감소보다 먼저 온다.** `if (timeout < 1)` 을 보고
 * 아니면 `timeout--` 하므로, 초기값 60 이면 실제로 61번 잔 뒤 포기한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 최악의 경우 1분 넘게 잠들 수 있다.
 *
 * 호출 체인:
 *   ibmphp_core.c 의 power_on / power_off / 표시등 조작
 *     → [이 함수] → get_hpc_access(), ioremap(),
 *       hpc_wait_ctlr_notworking(), ctrl_write(), iounmap(), free_hpc_access()
 */
int ibmphp_hpc_writeslot(struct slot *pslot, u8 cmd)
{
	/* [한국어] I2C 창의 매핑 주소. 읽기 쪽과 같은 이유로 NULL 로 시작한다 */
	void __iomem *wpg_bbar = NULL;
	/* [한국어] 슬롯이 붙어 있는 컨트롤러 */
	struct controller *ctlr_ptr;
	/* [한국어] index 는 변환한 컨트롤러 안 주소, status 는 컨트롤러 상태 */
	u8 index, status;
	/* [한국어] 버스 속도 명령에서만 쓴다. 음수 실패를 받아야 하므로 int 다 */
	int busindex;
	/* [한국어] **명령 완료 확인 루프를 끝낼 표시.**
	 * break 대신 표시를 쓰는 것이 이 파일의 관용이다 */
	u8 done;
	/* [한국어] 각 단계의 결과이자 반환값 */
	int rc = 0;
	/* [한국어] 완료를 기다릴 남은 초 수 */
	int timeout;

	/* [한국어] 진입을 남긴다 */
	debug_polling("%s - Entry pslot[%p] cmd[%x]\n", __func__, pslot, cmd);
	/* [한국어] **인자 검증. 읽기 쪽보다 단순하다** -- 결과를 담을 자리가 없기 때문이다 */
	if (pslot == NULL) {
		/* [한국어] 잘못된 인자 */
		rc = -EINVAL;
		/* [한국어] 오류를 남긴다 */
		err("%s - Error Exit rc[%d]\n", __func__, rc);
		/* [한국어] 곧바로 물러난다 */
		return rc;
	}

	/* [한국어] **버스 속도 명령 다섯 가지만 버스 인덱스를 쓴다.**
	 * 나머지는 모두 슬롯 인덱스를 쓴다 */
	if ((cmd == HPC_BUS_33CONVMODE) || (cmd == HPC_BUS_66CONVMODE) ||
		(cmd == HPC_BUS_66PCIXMODE) || (cmd == HPC_BUS_100PCIXMODE) ||
		(cmd == HPC_BUS_133PCIXMODE)) {
		/* [한국어] 버스 번호를 컨트롤러 안 인덱스로 옮긴다 */
		busindex = ibmphp_get_bus_index(pslot->bus);
		/* [한국어] 그 버스를 모른다 */
		if (busindex < 0) {
			/* [한국어] 잘못된 버스 */
			rc = -EINVAL;
			/* [한국어] 오류를 남긴다 */
			err("%s - Exit Error:invalid bus, rc[%d]\n", __func__, rc);
			/* [한국어] 물러난다 */
			return rc;
		/* [한국어] 버스를 찾았다 */
		} else
			/* [한국어] u8 로 좁힌다 */
			index = (u8) busindex;
	/* [한국어] 버스 명령이 아닌 경우 */
	} else
		/* [한국어] 슬롯의 컨트롤러 안 번호를 쓴다 */
		index = pslot->ctlr_index;

	/* [한국어] **명령에 맞는 인덱스로 옮긴다.**
	 * 컨트롤러 전체 명령이면 0x0F 로 고정되고, 슬롯이면 그대로,
	 * 버스면 버스 영역으로 옮겨진다 */
	index = hpc_writecmdtoindex(cmd, index);

	/* [한국어] 아는 명령이 아니었다 */
	if (index == HPC_ERROR) {
		/* [한국어] 잘못된 명령 */
		rc = -EINVAL;
		/* [한국어] 오류를 남긴다 */
		err("%s - Error Exit rc[%d]\n", __func__, rc);
		/* [한국어] 물러난다 */
		return rc;
	}

	/* [한국어] 컨트롤러를 꺼낸다 */
	ctlr_ptr = pslot->ctrl;

	/* [한국어] **컨트롤러를 독점한다.** 여기서부터 임계 구역이다 */
	get_hpc_access();

	//--------------------------------------------------------------------
	// map physical address to logical address
	//--------------------------------------------------------------------
	/* [한국어] I2C 컨트롤러일 때만 창을 매핑한다 */
	if ((ctlr_ptr->ctlr_type == 2) || (ctlr_ptr->ctlr_type == 4)) {
		/* [한국어] 물리 주소를 커널 주소로 매핑한다. **실패를 확인하지 않는다** */
		wpg_bbar = ioremap(ctlr_ptr->u.wpeg_ctlr.wpegbbar, WPG_I2C_IOREMAP_SIZE);

		/* [한국어] **매핑 결과를 자세히 남긴다.**
		 * 읽기 쪽에는 없는 로그이며, debug 라 늘 찍힌다.
		 * 물리 주소와 커널 주소를 함께 찍으므로 매핑이 맞는지 확인할 수 있다 */
		debug("%s - ctlr id[%x] physical[%lx] logical[%lx] i2c[%x]\n", __func__,
		ctlr_ptr->ctlr_id, (ulong) (ctlr_ptr->u.wpeg_ctlr.wpegbbar), (ulong) wpg_bbar,
		ctlr_ptr->u.wpeg_ctlr.i2c_addr);
	}
	//--------------------------------------------------------------------
	// check controller status before writing
	//--------------------------------------------------------------------
	/* [한국어] **명령을 내기 전에 컨트롤러가 놀고 있는지 확인한다.**
	 * 이전 명령이 진행 중이면 새 명령을 낼 수 없다 */
	rc = hpc_wait_ctlr_notworking(HPC_CTLR_WORKING_TOUT, ctlr_ptr, wpg_bbar, &status);
	/* [한국어] 기다리기에 성공한 경우에만 명령을 낸다 */
	if (!rc) {

		/* [한국어] **명령을 낸다. 이 한 줄이 이 함수의 목적 전부다.**
		 * **반환값을 받지 않는다** -- 아래층의 오류가 위로 전달되지 않으며,
		 * 대신 이어지는 완료 확인이 그 역할을 대신한다 */
		ctrl_write(ctlr_ptr, wpg_bbar, index, cmd);

		//--------------------------------------------------------------------
		// check controller is still not working on the command
		//--------------------------------------------------------------------
		/* [한국어] **완료를 기다릴 남은 초 수를 채운다** */
		timeout = CMD_COMPLETE_TOUT_SEC;
		/* [한국어] 아직 끝나지 않았다 */
		done = 0;
		/* [한국어] **명령이 끝날 때까지 확인을 되풀이한다** */
		while (!done) {
			/* [한국어] 컨트롤러가 일을 놓았는지 본다. status 에 상태가 담긴다 */
			rc = hpc_wait_ctlr_notworking(HPC_CTLR_WORKING_TOUT, ctlr_ptr, wpg_bbar,
							&status);
			/* [한국어] 상태를 읽어 냈는가 */
			if (!rc) {
				/* [한국어] **명령에 따라 확인이 한 겹 더 필요하다.**
				 * hpc_writecmdtoindex() 의 case 주석에 붙은 Y/N 이 그 구분이며,
				 * 일을 놓았다는 것만으로는 부족한 명령이 있다 */
				if (NEEDTOCHECK_CMDSTATUS(cmd)) {
					/* [한국어] **완료 표시까지 확인한다** */
					if (CTLR_FINISHED(status) == HPC_CTLR_FINISHED_YES)
						/* [한국어] 끝났다 */
						done = 1;
				/* [한국어] 완료 표시를 볼 필요가 없는 명령 */
				} else
					/* [한국어] 일을 놓았으면 끝난 것으로 본다 */
					done = 1;
			}
			/* [한국어] 아직 끝나지 않았다 */
			if (!done) {
				/* [한국어] **1초 양보한다.** I2C 루프의 10밀리초보다 훨씬 길다 --
				 * 명령 자체가 전원 인가처럼 오래 걸리는 일이기 때문이다 */
				msleep(1000);
				/* [한국어] **감소보다 검사가 먼저 온다.**
				 * 초기값 60 이면 실제로 61번 잔 뒤 포기한다 */
				if (timeout < 1) {
					/* [한국어] 더 기다리지 않는다 */
					done = 1;
					/* [한국어] 시간 초과를 남긴다 */
					err("%s - Error command complete timeout\n", __func__);
					/* [한국어] **읽기 쪽이 -EINVAL 을 쓰는 것과 달리 -EFAULT 다** */
					rc = -EFAULT;
				/* [한국어] 아직 시간이 남았다 */
				} else
					/* [한국어] 남은 초 수를 줄인다 */
					timeout--;
			}
		}
		/* [한국어] **마지막으로 읽은 컨트롤러 상태를 저장한다.**
		 * 시간이 초과되어 나온 경우에도 저장된다 */
		ctlr_ptr->status = status;
	}
	// cleanup

	// remove physical to logical address mapping
	/* [한국어] 매핑했던 경우에만 푼다 */
	if ((ctlr_ptr->ctlr_type == 2) || (ctlr_ptr->ctlr_type == 4))
		/* [한국어] 창 매핑을 놓는다 */
		iounmap(wpg_bbar);
	/* [한국어] **컨트롤러 독점을 놓는다.**
	 * 읽기 쪽과 마찬가지로 임계 구역 안에서 중간에 return 하는 자리가 없다 */
	free_hpc_access();

	/* [한국어] 결과를 남긴다 */
	debug_polling("%s - Exit rc[%d]\n", __func__, rc);
	/* [한국어] 성공 0, 실패면 음수 */
	return rc;
}

/*----------------------------------------------------------------------
* Name:    get_hpc_access()
*
* Action: make sure only one process can access HPC at one time
*---------------------------------------------------------------------*/
/* [한국어]
 * get_hpc_access - 컨트롤러 하드웨어를 독점한다
 *
 * @return: 없음.
 *
 * **뮤텍스 하나를 잡는 한 줄짜리 함수다.** 상류 주석이 그 뜻을 밝힌다 --
 * 한 번에 한 프로세스만 컨트롤러에 접근하게 한다.
 *
 * **왜 필요한가**: 이 컨트롤러는 한 번에 명령 하나만 처리한다.
 * 두 경로가 동시에 명령을 내면 서로의 완료를 기다리게 되어 상태가 엉킨다.
 *
 * **operations_mutex 와 지키는 것이 다르다.**
 *   sem_hpcaccess    -- 컨트롤러 하드웨어 접근(이 뮤텍스).
 *   operations_mutex -- 슬롯·컨트롤러 자료구조.
 * 읽기와 쓰기 두 진입점이 이것을 잡고, 폴링 스레드와 위층이 저것을 잡는다.
 *
 * **함수로 감싸 두는 이유**: 뮤텍스가 static 이라 이 파일 밖에서 잡을 수
 * 없는데, free_hpc_access() 는 ibmphp_hpc_stop_poll_thread() 에서도
 * 불려야 하기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 잠들 수 있다.
 *
 * 호출 체인:
 *   ibmphp_hpc_readslot / ibmphp_hpc_writeslot → [이 함수] → mutex_lock()
 */
static void get_hpc_access(void)
{
	/* [한국어] **컨트롤러 하드웨어 접근을 독점한다.**
	 * 이미 다른 경로가 쥐고 있으면 여기서 잠든다.
	 * **시간 제한이 없다** -- HPC_GETACCESS_TIMEOUT 상수가 정의되어 있으나
	 * 쓰이지 않는다 */
	mutex_lock(&sem_hpcaccess);
}

/*----------------------------------------------------------------------
* Name:    free_hpc_access()
*---------------------------------------------------------------------*/
/* [한국어]
 * free_hpc_access - 컨트롤러 하드웨어 독점을 놓는다
 *
 * @return: 없음.
 *
 * **get_hpc_access() 의 짝이다.**
 *
 * **static 이 아닌 것이 짝과 다르다.** 이 파일 안의 두 진입점 말고도
 * ibmphp_hpc_stop_poll_thread() 가 이것을 부르기 때문이다.
 * **다만 그 자리에서는 앞서 get_hpc_access() 를 부른 적이 없다** --
 * 곧 잡지 않은 뮤텍스를 푸는 셈이다. 그 함수의 주석에 자세히 적는다.
 *
 * **ibmphp.h 에 선언이 있는지는 이 파일에서 알 수 없다.**
 * 파일 앞머리의 지역 함수 원형 목록에 static 으로 적혀 있는데
 * 정의는 static 없이 되어 있어, 두 선언이 어긋난다.
 * 코드는 손대지 않고 사실만 적는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   ibmphp_hpc_readslot / ibmphp_hpc_writeslot / ibmphp_hpc_stop_poll_thread
 *     → [이 함수] → mutex_unlock()
 */
void free_hpc_access(void)
{
	/* [한국어] **독점을 놓는다.**
	 * **ibmphp_hpc_stop_poll_thread() 도 이것을 부르는데
	 * 그 자리에서는 앞서 잡은 적이 없다** -- 그 함수의 주석에 자세히 적는다 */
	mutex_unlock(&sem_hpcaccess);
}

/*----------------------------------------------------------------------
* Name:    ibmphp_lock_operations()
*
* Action: make sure only one process can change the data structure
*---------------------------------------------------------------------*/
/* [한국어]
 * ibmphp_lock_operations - 슬롯·컨트롤러 자료구조를 잠근다
 *
 * @return: 없음.
 *
 * **폴링 스레드와 위층이 같은 자료구조를 만지므로 필요한 뮤텍스다.**
 * 위층이 슬롯을 켜는 동안 폴링이 그 슬롯의 상태를 읽어 "변했다" 고
 * 판단하면 엉뚱한 처리가 일어난다.
 *
 * **to_debug 를 1 로 세우는 것이 이 함수의 숨은 절반이다.**
 * 그 전역이 debug_polling() 매크로를 열고 닫는 스위치다 --
 * 폴링 중에는 같은 메시지가 2초마다 쏟아지므로 평소에는 막아 두고,
 * **위층이 실제로 조작하는 동안에만 저수준 로그를 켠다.**
 * ibmphp_unlock_operations() 가 다시 0 으로 되돌린다.
 *
 * **따라서 폴링 스레드가 부르는 mutex_lock 은 이 함수가 아니다** --
 * poll_hpc() 는 operations_mutex 를 직접 잡는다. 그래야 폴링 중에
 * 저수준 로그가 켜지지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 잠들 수 있다.
 *
 * 호출 체인:
 *   ibmphp_core.c 의 슬롯 조작 경로 / ibmphp_hpc_stop_poll_thread
 *     → [이 함수] → mutex_lock()
 */
void ibmphp_lock_operations(void)
{
	/* [한국어] **슬롯·컨트롤러 자료구조를 잠근다.**
	 * 폴링 스레드와 겹치지 않게 한다 */
	mutex_lock(&operations_mutex);
	/* [한국어] **저수준 로그를 켠다.**
	 * 위층이 실제로 조작하는 동안에만 debug_polling 이 열리므로,
	 * 폴링이 도는 평소에는 dmesg 가 조용하다.
	 * **폴링 스레드가 이 함수를 쓰지 않고 뮤텍스를 직접 잡는 이유가 여기 있다** */
	to_debug = 1;
}

/*----------------------------------------------------------------------
* Name:    ibmphp_unlock_operations()
*---------------------------------------------------------------------*/
/* [한국어]
 * ibmphp_unlock_operations - 자료구조 잠금을 놓고 저수준 로그를 다시 막는다
 *
 * @return: 없음.
 *
 * **ibmphp_lock_operations() 의 짝이며 두 가지를 되돌린다** --
 * 뮤텍스를 풀고 to_debug 를 0 으로 내린다.
 *
 * **푸는 순서가 눈에 띈다.** 뮤텍스를 먼저 풀고 그다음 to_debug 를
 * 내리므로, 그 사이에 폴링 스레드가 들어오면 debug_polling 이 아직
 * 켜진 채로 돈다. 짧은 창이지만 로그가 섞일 수 있다.
 * 코드는 손대지 않고 사실만 적는다.
 *
 * **진입과 퇴장을 모두 debug 로 남긴다.** lock 쪽에는 없는 것이며,
 * 푸는 쪽이 더 자세히 기록된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   ibmphp_core.c 의 슬롯 조작 경로 / ibmphp_hpc_stop_poll_thread
 *     → [이 함수] → mutex_unlock()
 */
void ibmphp_unlock_operations(void)
{
	/* [한국어] 진입을 남긴다. **lock 쪽에는 없는 로그다** */
	debug("%s - Entry\n", __func__);
	/* [한국어] 자료구조 잠금을 놓는다 */
	mutex_unlock(&operations_mutex);
	/* [한국어] **저수준 로그를 다시 막는다.**
	 * **뮤텍스를 먼저 풀었으므로 그사이 들어온 폴링이 로그를 찍을 수 있다** --
	 * 짧은 창이지만 순서가 뒤바뀐 자리다. 코드는 손대지 않고 사실만 적는다 */
	to_debug = 0;
	/* [한국어] 퇴장을 남긴다 */
	debug("%s - Exit\n", __func__);
}

/*----------------------------------------------------------------------
* Name:    poll_hpc()
*---------------------------------------------------------------------*/
/* [한국어] **poll_hpc 의 상태 기계가 가질 세 값. 여기부터다.**
 * 0 -- 래치 레지스터만 읽는 싼 단계. 컨트롤러당 읽기 한 번이다 */
#define POLL_LATCH_REGISTER	0
/* [한국어] 1 -- 슬롯을 모두 훑는 비싼 단계. 슬롯마다 두 번 읽고 그때마다 기다린다 */
#define POLL_SLOTS		1
/* [한국어] 2 -- 2초 자고 다음 상태를 정하는 단계.
 * **락을 놓고 자는 것이 이 단계의 핵심이다** */
#define POLL_SLEEP		2
/* [한국어]
 * poll_hpc - 인터럽트 없는 컨트롤러를 2초마다 들여다보는 커널 스레드
 *
 * @data: 쓰이지 않는다. kthread_run 이 요구하는 시그니처를 맞춘 자리다.
 * @return: 늘 0.
 *
 * **이 드라이버의 심장이다.** IBM 핫플러그 컨트롤러에는 인터럽트가 없어,
 * 카드가 꽂히거나 빠진 것을 알아채려면 주기적으로 물어보는 수밖에 없다.
 *
 * **세 상태를 오가는 작은 상태 기계다.**
 * - POLL_LATCH_REGISTER -- 컨트롤러마다 래치 레지스터 한 바이트를 읽어
 *   지난번과 견준다. 달라졌으면 process_changeinlatch() 로 넘긴다.
 *   **싸다** -- 컨트롤러당 읽기 한 번이다.
 * - POLL_SLOTS -- 슬롯을 모두 훑어 전체 상태를 읽는다.
 *   **비싸다** -- 슬롯마다 두 번씩 읽고 그때마다 완료를 기다린다.
 * - POLL_SLEEP -- 2초 잔 뒤 다음 상태를 정한다.
 *
 * **래치를 다섯 번 볼 때마다 슬롯을 한 번 본다.** POLL_LATCH_CNT 가 그
 * 비율이며, 잦은 사건(레버 여닫기)은 자주 보고 드문 사건(전원 이상,
 * 표시등 변화)은 가끔 본다는 뜻이다.
 *
 * **잠들기 전에 반드시 락을 놓는다.** 원문 주석의
 * "don't sleep with a lock on the hardware" 가 그것이며,
 * 2초 동안 락을 쥐고 있으면 위층의 sysfs 조작이 그만큼 멈춘다.
 *
 * **operations_mutex 를 직접 잡는다** -- ibmphp_lock_operations() 를
 * 쓰지 않는다. 그 함수는 to_debug 도 함께 켜는데, 폴링 중에 그것이
 * 켜지면 로그가 2초마다 쏟아지기 때문이다.
 *
 * **POLL_SLOTS 가 구조체 사본을 떠 두는 것이 변화 탐지의 방식이다.**
 * memcpy 로 옛 상태를 복사한 뒤 READ_ALLSTAT 으로 갱신하고,
 * 둘을 견주어 달라진 것을 찾는다.
 *
 * **rc 에 담긴 읽기 결과를 확인하지 않는다.** 세 자리에서
 * `rc = ibmphp_hpc_readslot(...)` 로 받지만 그 값을 보는 코드가 없다 --
 * 읽기가 실패해도 옛 값과 새 값을 그대로 견주게 된다.
 * 코드는 손대지 않고 사실만 적는다.
 *
 * **out_sleep 라벨이 루프 안에 있다.** POLL_SLEEP 에서 종료 요청을
 * 확인했을 때 그리로 뛰는데, 그 시점에는 이미 락을 놓은 상태라
 * 루프 바닥의 mutex_unlock 을 건너뛰는 것이 옳다.
 *
 * **루프를 나가며 complete() 로 종료를 알린다.**
 * ibmphp_hpc_stop_poll_thread() 가 그것을 기다린다.
 *
 * 실행 컨텍스트: 커널 스레드(프로세스 컨텍스트). 대부분의 시간을 자며 보낸다.
 *
 * 호출 체인:
 *   kthread_run → [이 함수]
 *     → ibmphp_hpc_readslot(), process_changeinlatch(),
 *       process_changeinstatus(), complete()
 */
static int poll_hpc(void *data)
{
	/* [한국어] **갱신 전 슬롯 상태를 떠 둘 사본. 스택에 통째로 둔다.**
	 * 이것과 갱신된 원본을 견주어 무엇이 달라졌는지 찾는다 */
	struct slot myslot;
	/* [한국어] 목록을 걷는 반복 변수 */
	struct slot *pslot = NULL;
	/* [한국어] **읽기 결과를 받는 변수. 그러나 이 함수에서 그 값을 읽는 곳이 없다** --
	 * 세 자리에서 대입만 하고 확인하지 않는다.
	 * 코드는 손대지 않고 사실만 적는다 */
	int rc;
	/* [한국어] **상태 기계의 현재 상태. 래치 읽기부터 시작한다** */
	int poll_state = POLL_LATCH_REGISTER;
	/* [한국어] 지난번 래치 레지스터 값 */
	u8 oldlatchlow = 0x00;
	/* [한국어] 방금 읽은 래치 레지스터 값. **둘을 견주어 변화를 찾는다** */
	u8 curlatchlow = 0x00;
	/* [한국어] **래치를 몇 번 봤는지 센다.** POLL_LATCH_CNT 에 이르면 슬롯을 훑는다 */
	int poll_count = 0;
	/* [한국어] **컨트롤러를 몇 개 봤는지 센다.**
	 * 슬롯 목록을 걷지만 래치는 컨트롤러당 하나이므로 중복을 피해야 한다 */
	u8 ctrl_count = 0x00;

	/* [한국어] 스레드 시작을 남긴다 */
	debug("%s - Entry\n", __func__);

	/* [한국어] **종료 요청이 올 때까지 돈다.**
	 * kthread_stop() 이 그 조건을 참으로 만든다 */
	while (!kthread_should_stop()) {
		/* try to get the lock to do some kind of hardware access */
		/* [한국어] **자료구조를 잠근다.**
		 * **ibmphp_lock_operations() 를 쓰지 않는 것이 요점이다** --
		 * 그 함수는 to_debug 도 켜는데, 폴링 중에 저수준 로그가 쏟아지기 때문이다.
		 * 원문 주석: try to get the lock to do some kind of hardware access */
		mutex_lock(&operations_mutex);

		/* [한국어] 세 상태 중 하나를 수행한다 */
		switch (poll_state) {
		/* [한국어] **싼 단계 -- 컨트롤러당 래치 레지스터 한 바이트만 읽는다** */
		case POLL_LATCH_REGISTER:
			/* [한국어] **지난번 값을 옛 값으로 옮긴다.** 이제 curlatchlow 를 새로 채운다 */
			oldlatchlow = curlatchlow;
			/* [한국어] 컨트롤러 세기를 처음부터 시작한다 */
			ctrl_count = 0x00;
			/* [한국어] **전역 슬롯 목록을 걷는다.**
			 * 래치는 컨트롤러 단위인데 목록은 슬롯 단위라 걸러 내야 한다 */
			list_for_each_entry(pslot, &ibmphp_slot_head,
					    ibm_slot_list) {
				/* [한국어] **컨트롤러를 다 봤으면 그만둔다.**
				 * 남은 슬롯은 이미 본 컨트롤러의 것이다 */
				if (ctrl_count >= ibmphp_get_total_controllers())
					/* [한국어] 목록 걷기를 끝낸다 */
					break;
				/* [한국어] **컨트롤러마다 한 번만 읽기 위한 걸러내기.**
				 * 상대 ID 가 지금 세는 번호와 맞는 슬롯에서만 읽는다 --
				 * 곧 컨트롤러당 첫 슬롯 하나만 대표로 쓴다 */
				if (pslot->ctrl->ctlr_relative_id == ctrl_count) {
					/* [한국어] 다음 컨트롤러를 세러 간다 */
					ctrl_count++;
					/* [한국어] **이 컨트롤러가 래치 레지스터를 지원하는가.**
					 * 그 매크로는 컨트롤러의 options 비트를 본다(ibmphp.h) */
					if (READ_SLOT_LATCH(pslot->ctrl)) {
						/* [한국어] **래치 레지스터 한 바이트를 읽는다.**
						 * 그 안에 이 컨트롤러가 담당하는 여러 슬롯의 래치 상태가 비트로 들어 있다.
						 * **rc 를 받지만 확인하지 않는다** -- 읽기가 실패해도 아래에서
						 * curlatchlow 를 옛 값과 견주게 된다 */
						rc = ibmphp_hpc_readslot(pslot,
									  READ_SLOTLATCHLOWREG,
									  &curlatchlow);
						/* [한국어] **지난번과 달라졌는가.** 이 한 줄이 폴링 변화 탐지의 전부다 */
						if (oldlatchlow != curlatchlow)
							/* [한국어] **달라진 비트를 슬롯별 처리로 넘긴다** */
							process_changeinlatch(oldlatchlow,
									       curlatchlow,
									       pslot->ctrl);
					}
				}
			}
			/* [한국어] 래치를 본 횟수를 늘린다 */
			++poll_count;
			/* [한국어] **다음은 무조건 잠자기다.** 두 작업 상태가 모두 여기로 간다 */
			poll_state = POLL_SLEEP;
			/* [한국어] 상태 갈래를 마친다 */
			break;
		/* [한국어] **비싼 단계 -- 슬롯을 모두 훑는다** */
		case POLL_SLOTS:
			/* [한국어] **슬롯 하나하나를 본다.** 컨트롤러 걸러내기가 없다 */
			list_for_each_entry(pslot, &ibmphp_slot_head,
					    ibm_slot_list) {
				// make a copy of the old status
				/* [한국어] **갱신 전 상태를 통째로 복사해 둔다.**
				 * struct slot 전체를 뜨지만 실제로 견주는 것은 status 와 ext_status 둘뿐이다 */
				memcpy((void *) &myslot, (void *) pslot,
					sizeof(struct slot));
				/* [한국어] **READ_ALLSTAT 만 slot 구조체를 갱신한다.**
				 * 그래서 이 호출 뒤 pslot 은 새 값, myslot 은 옛 값이 된다.
				 * pstatus 를 NULL 로 넘길 수 있는 것도 그 때문이다.
				 * **rc 를 확인하지 않는다** */
				rc = ibmphp_hpc_readslot(pslot, READ_ALLSTAT, NULL);
				/* [한국어] **두 상태 바이트 중 하나라도 달라졌는가** */
				if ((myslot.status != pslot->status)
				    || (myslot.ext_status != pslot->ext_status))
					/* [한국어] **달라진 비트를 해석해 처리한다.**
					 * 새 것을 앞에, 옛 것을 뒤에 넘긴다 */
					process_changeinstatus(pslot, &myslot);
			}
			/* [한국어] **슬롯을 다 본 뒤 래치도 한 번 읽어 둔다.**
			 * 그래야 다음 POLL_LATCH_REGISTER 가 견줄 기준값이 최신이 된다 */
			ctrl_count = 0x00;
			/* [한국어] 컨트롤러 단위로 걸러 내며 다시 걷는다 */
			list_for_each_entry(pslot, &ibmphp_slot_head,
					    ibm_slot_list) {
				/* [한국어] 컨트롤러를 다 봤는가 */
				if (ctrl_count >= ibmphp_get_total_controllers())
					/* [한국어] 목록 걷기를 끝낸다 */
					break;
				/* [한국어] 컨트롤러당 한 번만 */
				if (pslot->ctrl->ctlr_relative_id == ctrl_count) {
					/* [한국어] 다음 컨트롤러로 */
					ctrl_count++;
					/* [한국어] 래치를 지원하는가 */
					if (READ_SLOT_LATCH(pslot->ctrl))
						/* [한국어] **래치 값만 갱신하고 견주지는 않는다.**
						 * 방금 슬롯 전체를 처리했으므로 여기서 다시 볼 필요가 없고,
						 * 다음 래치 단계를 위한 기준값만 새로 잡는다 */
						rc = ibmphp_hpc_readslot(pslot,
									  READ_SLOTLATCHLOWREG,
									  &curlatchlow);
				}
			}
			/* [한국어] 횟수를 늘린다 */
			++poll_count;
			/* [한국어] 다음은 잠자기 */
			poll_state = POLL_SLEEP;
			/* [한국어] 상태 갈래를 마친다 */
			break;
		/* [한국어] **잠자기 단계 -- 다음에 무엇을 할지도 여기서 정한다** */
		case POLL_SLEEP:
			/* don't sleep with a lock on the hardware */
			/* [한국어] **자기 전에 반드시 락을 놓는다.**
			 * 원문 주석의 "don't sleep with a lock on the hardware" 가 그것이며,
			 * 2초 동안 쥐고 있으면 위층의 sysfs 조작이 그만큼 멈춘다 */
			mutex_unlock(&operations_mutex);
			/* [한국어] **2초 잔다.** 이 드라이버가 사건을 알아채는 최소 지연이기도 하다 */
			msleep(POLL_INTERVAL_SEC * 1000);

			/* [한국어] **자는 동안 종료 요청이 왔는가** */
			if (kthread_should_stop())
				/* [한국어] **락을 이미 놓았으므로 루프 바닥의 unlock 을 건너뛰어야 한다.**
				 * 그래서 라벨이 그 아래에 있다 */
				goto out_sleep;

			/* [한국어] 다시 잠근다. 아래에서 상태 변수를 만지기 때문이다 */
			mutex_lock(&operations_mutex);

			/* [한국어] **래치를 다섯 번 봤는가** */
			if (poll_count >= POLL_LATCH_CNT) {
				/* [한국어] 세기를 처음부터 */
				poll_count = 0;
				/* [한국어] **비싼 단계로 간다.** 다섯 번에 한 번이다 */
				poll_state = POLL_SLOTS;
			/* [한국어] 아직 다섯 번이 안 되었다 */
			} else
				/* [한국어] 싼 단계를 되풀이한다 */
				poll_state = POLL_LATCH_REGISTER;
			/* [한국어] 상태 갈래를 마친다 */
			break;
		}
		/* give up the hardware semaphore */
		/* [한국어] **작업 단계를 마친 뒤 락을 놓는다.**
		 * POLL_SLEEP 갈래에서 goto 로 온 경우는 여기를 지나지 않는다.
		 * 원문 주석: give up the hardware semaphore */
		mutex_unlock(&operations_mutex);
		/* sleep for a short time just for good measure */
/* [한국어] **종료 요청으로 잠자기 단계를 빠져나올 때 오는 자리.**
 * 락이 이미 풀린 상태다 */
out_sleep:
		/* [한국어] **짧게 한 번 더 잔다.**
		 * 원문 주석이 "just for good measure" 라 밝히며,
		 * 다른 문맥이 락을 잡을 틈을 주려는 것으로 보인다 */
		msleep(100);
	}
	/* [한국어] **루프를 나왔음을 알린다.**
	 * ibmphp_hpc_stop_poll_thread() 가 이것을 기다린다 */
	complete(&exit_complete);
	/* [한국어] 스레드 종료를 남긴다 */
	debug("%s - Exit\n", __func__);
	/* [한국어] **늘 0 이다.** kthread 의 반환값은 kthread_stop() 이 받는다 */
	return 0;
}


/*----------------------------------------------------------------------
* Name:    process_changeinstatus
*
* Action:  compare old and new slot status, process the change in status
*
* Input:   pointer to slot struct, old slot struct
*
* Return   0 or error codes
* Value:
*
* Side
* Effects: None.
*
* Notes:
*---------------------------------------------------------------------*/
/* [한국어]
 * process_changeinstatus - 슬롯 상태의 변화를 해석해 처리한다
 *
 * @pslot: 방금 읽어 갱신된 슬롯.
 * @poldslot: 갱신 전에 떠 둔 사본.
 * @return: 성공 0, 슬롯 내리기가 실패하면 그 오류.
 *
 * **폴링이 찾아낸 차이를 실제 동작으로 옮기는 함수다.**
 * 상태 바이트의 비트를 하나씩 견주며 두 표시를 세운다 --
 * update(정보만 갱신하면 됨)와 disable(슬롯을 내려야 함).
 *
 * **비트마다 다루는 방식이 다르다.**
 * - 비트 0 전원, 비트 2 주의 표시등, 비트 3·4 존재 감지 --
 *   달라졌으면 update 만 세운다.
 * - **비트 1 연결과 비트 6 버스 속도는 아예 보지 않는다.**
 *   원문 주석이 "ignore" 라 밝힌다.
 * - **비트 5 전원 양호(PWRGD)** -- 꺼짐에서 켜짐은 무시하고,
 *   **켜짐에서 꺼짐일 때만 disable 을 세운다.** 그것도 이전에 연결되어
 *   있었고 카드가 있었을 때만이다. 전원이 스스로 꺼진 것은
 *   고장이나 과전류를 뜻하므로 슬롯을 내려야 한다.
 * - **비트 7 래치** -- 이 함수에서 가장 복잡한 갈래다.
 *   **열림에서 닫힘** 이면 원문 주석이 밝히듯 래치를 닫은 직후 전원이
 *   켜졌다 꺼지는 일이 있어, **1초 기다렸다가 다시 읽어** 전원이
 *   그대로인지 확인한다. 아니면 pslot 의 전원 비트를 손으로 지운다.
 *   **닫힘에서 열림** 이면 동작 중인 카드의 레버가 열린 것이므로
 *   disable 을 세운다.
 * - 확장 상태의 비트 3 -- 주의 표시등 깜빡임.
 *
 * **주석과 코드가 어긋나는 자리가 있다.** 마지막 검사의 주석은
 * "bit 4 - HPC_SLOT_BLINK_ATTN" 인데 마스크는 0x08 이라 비트 3 이다.
 * 원본에서 확인했으며 코드는 손대지 않고 사실만 적는다.
 *
 * **래치 갈래 안의 `update = 1` 이 중복이다.** 바깥에서 이미 세운 뒤라
 * 같은 값을 다시 넣는다.
 *
 * **disable 이면 flag 를 0 으로 내리고 슬롯을 내린다.**
 * update 나 disable 중 하나라도 섰으면 sysfs 쪽 정보를 갱신한다 --
 * 곧 **내린 슬롯도 정보 갱신을 받는다.**
 *
 * 실행 컨텍스트: 폴링 스레드(프로세스 컨텍스트).
 * **operations_mutex 를 쥔 채로 불린다** -- 호출자 poll_hpc 가 잡는다.
 *
 * 호출 체인:
 *   poll_hpc / process_changeinlatch → [이 함수]
 *     → ibmphp_hpc_readslot(), ibmphp_do_disable_slot(),
 *       ibmphp_update_slot_info()
 */
static int process_changeinstatus(struct slot *pslot, struct slot *poldslot)
{
	/* [한국어] 래치가 닫힌 뒤 전원을 다시 확인할 때만 쓴다 */
	u8 status;
	/* [한국어] 슬롯 내리기의 결과이자 반환값 */
	int rc = 0;
	/* [한국어] **슬롯을 내려야 하는가.** 전원이 스스로 꺼졌거나 레버가 열렸을 때 선다 */
	u8 disable = 0;
	/* [한국어] **sysfs 쪽 정보만 갱신하면 되는가** */
	u8 update = 0;

	/* [한국어] **__func__ 대신 함수 이름을 손으로 적었다.**
	 * 이 파일의 다른 로그와 어긋나며 상류 그대로다 */
	debug("process_changeinstatus - Entry pslot[%p], poldslot[%p]\n", pslot, poldslot);

	// bit 0 - HPC_SLOT_POWER
	/* [한국어] **비트 0 -- 전원 상태(HPC_SLOT_POWER).**
	 * 켜졌는지 꺼졌는지가 달라졌으면 알리기만 하면 된다 */
	if ((pslot->status & 0x01) != (poldslot->status & 0x01))
		/* [한국어] 정보 갱신 표시를 세운다 */
		update = 1;

	// bit 1 - HPC_SLOT_CONNECT
	// ignore

	// bit 2 - HPC_SLOT_ATTN
	/* [한국어] **비트 2 -- 주의 표시등(HPC_SLOT_ATTN).**
	 * 비트 1(연결)은 원문 주석대로 무시한다 */
	if ((pslot->status & 0x04) != (poldslot->status & 0x04))
		/* [한국어] 정보 갱신 표시를 세운다 */
		update = 1;

	// bit 3 - HPC_SLOT_PRSNT2
	// bit 4 - HPC_SLOT_PRSNT1
	/* [한국어] **비트 3·4 -- 카드 존재 감지 두 선(PRSNT2, PRSNT1).**
	 * PCI 는 카드 폭을 알리려고 감지선을 두 개 두므로 둘 다 본다 */
	if (((pslot->status & 0x08) != (poldslot->status & 0x08))
		|| ((pslot->status & 0x10) != (poldslot->status & 0x10)))
		/* [한국어] 정보 갱신 표시를 세운다 */
		update = 1;

	// bit 5 - HPC_SLOT_PWRGD
	/* [한국어] **비트 5 -- 전원 양호(PWRGD). 이 함수에서 처음으로 disable 이 걸리는 자리다** */
	if ((pslot->status & 0x20) != (poldslot->status & 0x20))
		// OFF -> ON: ignore, ON -> OFF: disable slot
		/* [한국어] **켜짐에서 꺼짐일 때만, 그것도 이전에 연결되어 있고 카드가 있었을 때만.**
		 * 원문 주석이 그 방향을 밝힌다 -- 꺼짐에서 켜짐은 정상적인 전원 인가라
		 * 무시하고, 반대는 고장이나 과전류를 뜻한다 */
		if ((poldslot->status & 0x20) && (SLOT_CONNECT(poldslot->status) == HPC_SLOT_CONNECTED) && (SLOT_PRESENT(poldslot->status)))
			/* [한국어] **슬롯을 내려야 한다** */
			disable = 1;

	// bit 6 - HPC_SLOT_BUS_SPEED
	// ignore

	// bit 7 - HPC_SLOT_LATCH
	/* [한국어] **비트 7 -- 레버(래치). 이 함수에서 가장 복잡한 갈래다.**
	 * 비트 6(버스 속도)은 원문 주석대로 무시한다 */
	if ((pslot->status & 0x80) != (poldslot->status & 0x80)) {
		/* [한국어] 어느 방향이든 정보는 갱신한다 */
		update = 1;
		// OPEN -> CLOSE
		/* [한국어] **열림에서 닫힘 -- 카드를 넣고 레버를 잠갔다** */
		if (pslot->status & 0x80) {
			/* [한국어] 전원이 양호한가 */
			if (SLOT_PWRGD(pslot->status)) {
				// power goes on and off after closing latch
				// check again to make sure power is still ON
				/* [한국어] **1초 기다린다.** 원문 주석이 사정을 밝힌다 --
				 * 레버를 닫은 직후 전원이 켜졌다 꺼지는 일이 있어,
				 * 그 흔들림이 가라앉기를 기다린 뒤 다시 확인해야 한다 */
				msleep(1000);
				/* [한국어] **구조체를 갱신하지 않는 명령으로 다시 읽는다.**
				 * 지금 판단 중인 pslot->status 를 덮어쓰면 안 되기 때문이다 */
				rc = ibmphp_hpc_readslot(pslot, READ_SLOTSTATUS, &status);
				/* [한국어] 1초 뒤에도 전원이 양호한가 */
				if (SLOT_PWRGD(status))
					/* [한국어] **바깥에서 이미 세운 값이라 중복이다.**
					 * 코드는 손대지 않고 사실만 적는다 */
					update = 1;
				/* [한국어] 전원이 꺼져 있다 */
				else	// overwrite power in pslot to OFF
					/* [한국어] **구조체의 전원 비트를 손으로 지운다.**
					 * 읽어 온 값이 아니라 판단 결과를 반영하는 것이며,
					 * 원문 주석의 "overwrite power in pslot to OFF" 가 그 뜻이다 */
					pslot->status &= ~HPC_SLOT_POWER;
			}
		}
		// CLOSE -> OPEN
		/* [한국어] **닫힘에서 열림 -- 동작 중인 카드의 레버가 열렸다.**
		 * 이전에 전원이 양호했고 연결되어 있었고 카드가 있었을 때만이다 */
		else if ((SLOT_PWRGD(poldslot->status) == HPC_SLOT_PWRGD_GOOD)
			&& (SLOT_CONNECT(poldslot->status) == HPC_SLOT_CONNECTED) && (SLOT_PRESENT(poldslot->status))) {
			/* [한국어] **슬롯을 내려야 한다.** 레버가 열렸다는 것은 곧 뽑겠다는 뜻이다 */
			disable = 1;
		}
		// else - ignore
	}
	// bit 4 - HPC_SLOT_BLINK_ATTN
	/* [한국어] **확장 상태의 비트 3 -- 주의 표시등 깜빡임.**
	 * **바로 위 주석은 "bit 4" 라 하는데 마스크는 0x08 이라 비트 3 이다.**
	 * 원본에서 확인했으며 코드는 손대지 않고 사실만 적는다 */
	if ((pslot->ext_status & 0x08) != (poldslot->ext_status & 0x08))
		/* [한국어] 정보 갱신 표시를 세운다 */
		update = 1;

	/* [한국어] 슬롯을 내려야 하는가 */
	if (disable) {
		/* [한국어] 내린다는 것을 남긴다 */
		debug("process_changeinstatus - disable slot\n");
		/* [한국어] **슬롯 표시를 내린다.** 그 필드의 뜻은 ibmphp_core.c 가 정하며,
		 * 사용자가 요청한 조작인지 하드웨어가 강제한 것인지를 가리는 데 쓴다 */
		pslot->flag = 0;
		/* [한국어] **위층에 슬롯 내리기를 맡긴다.**
		 * 장치 제거와 자원 반납은 이 파일의 일이 아니다 */
		rc = ibmphp_do_disable_slot(pslot);
	}

	/* [한국어] **둘 중 하나라도 섰으면 sysfs 정보를 갱신한다.**
	 * 곧 내린 슬롯도 갱신을 받는다 */
	if (update || disable)
		/* [한국어] 핫플러그 코어 쪽 정보를 새로 쓴다(ibmphp_core.c) */
		ibmphp_update_slot_info(pslot);

	/* [한국어] **두 표시를 함께 찍어 어떤 판단이 내려졌는지 남긴다** */
	debug("%s - Exit rc[%d] disable[%x] update[%x]\n", __func__, rc, disable, update);

	/* [한국어] 슬롯 내리기의 결과. 내리지 않았으면 0 */
	return rc;
}

/*----------------------------------------------------------------------
* Name:    process_changeinlatch
*
* Action:  compare old and new latch reg status, process the change
*
* Input:   old and current latch register status
*
* Return   0 or error codes
* Value:
*---------------------------------------------------------------------*/
/* [한국어]
 * process_changeinlatch - 래치 레지스터의 변화를 슬롯별 처리로 나눈다
 *
 * @old: 지난번 래치 레지스터 값.
 * @new: 방금 읽은 값.
 * @ctrl: 이 레지스터를 가진 컨트롤러.
 * @return: 성공 0, 슬롯을 못 찾으면 -EINVAL.
 *
 * **한 바이트 안에 여러 슬롯의 래치 상태가 들어 있어 필요한 함수다.**
 * 비트 하나가 슬롯 하나이며, 달라진 비트마다 그 슬롯의 전체 상태를
 * 다시 읽어 process_changeinstatus() 로 넘긴다.
 *
 * **컨트롤러가 담당하는 슬롯 범위만 훑는다.** starting_slot_num 부터
 * ending_slot_num 까지이며, 그 값은 ibmphp_ebda.c 가 EBDA 에서 읽어
 * 넣어 둔 것이다.
 *
 * **비트 자리가 곧 슬롯의 물리 번호다.** `mask = 0x01 << i` 로 만들어
 * 견주고, 달라졌으면 ibmphp_get_slot_from_physical_num(i) 로 슬롯을 찾는다.
 * 원문 주석이 밝히듯 **비트 0 은 예약이고 실제로는 비트 1~6 이 여섯 슬롯
 * 을 나타내는데**, 반복은 컨트롤러가 알려 준 범위를 그대로 쓴다.
 *
 * **슬롯을 찾으면 사본을 뜨고 전체 상태를 다시 읽는다.**
 * 래치만 바뀐 것이 아닐 수 있으므로 READ_ALLSTAT 으로 모두 갱신한 뒤
 * process_changeinstatus() 가 무엇이 달라졌는지 판단한다.
 *
 * **읽기 결과를 확인하지 않는다.** `rc = ibmphp_hpc_readslot(...)` 로
 * 받지만 곧바로 process_changeinstatus() 를 부르므로,
 * 읽기가 실패해도 옛 값과 견주게 된다. 코드는 손대지 않고 사실만 적는다.
 *
 * **rc 가 마지막 슬롯의 결과만 남긴다.** 여러 슬롯이 바뀌었을 때
 * 앞쪽의 오류는 덮어써진다.
 *
 * 실행 컨텍스트: 폴링 스레드(프로세스 컨텍스트). operations_mutex 를 쥔 채다.
 *
 * 호출 체인:
 *   poll_hpc → [이 함수]
 *     → ibmphp_get_slot_from_physical_num(), ibmphp_hpc_readslot(),
 *       process_changeinstatus()
 */
static int process_changeinlatch(u8 old, u8 new, struct controller *ctrl)
{
	/* [한국어] myslot 은 갱신 전 사본, pslot 은 찾아낸 슬롯 */
	struct slot myslot, *pslot;
	/* [한국어] 슬롯 번호이자 비트 자리 */
	u8 i;
	/* [한국어] 그 비트만 남기는 마스크 */
	u8 mask;
	/* [한국어] 결과. **여러 슬롯이 바뀌면 마지막 것만 남는다** */
	int rc = 0;

	/* [한국어] 옛 값과 새 값을 함께 남긴다 */
	debug("%s - Entry old[%x], new[%x]\n", __func__, old, new);
	// bit 0 reserved, 0 is LSB, check bit 1-6 for 6 slots

	/* [한국어] **이 컨트롤러가 담당하는 슬롯 범위만 훑는다.**
	 * 그 범위는 ibmphp_ebda.c 가 EBDA 에서 읽어 넣어 둔 값이다.
	 * 원문 주석이 밝히듯 비트 0 은 예약이고 실제로는 비트 1~6 이 여섯 슬롯이다 */
	for (i = ctrl->starting_slot_num; i <= ctrl->ending_slot_num; i++) {
		/* [한국어] **비트 자리가 곧 슬롯의 물리 번호다** */
		mask = 0x01 << i;
		/* [한국어] 이 슬롯의 래치가 달라졌는가 */
		if ((mask & old) != (mask & new)) {
			/* [한국어] 물리 번호로 슬롯을 찾는다(ibmphp_ebda.c) */
			pslot = ibmphp_get_slot_from_physical_num(i);
			/* [한국어] 찾았는가 */
			if (pslot) {
				/* [한국어] **갱신 전 상태를 통째로 복사해 둔다.**
				 * 래치만 바뀐 것이 아닐 수 있으므로 전체를 견주어야 한다 */
				memcpy((void *) &myslot, (void *) pslot, sizeof(struct slot));
				/* [한국어] **전체 상태를 다시 읽어 구조체를 갱신한다.**
				 * **rc 를 확인하지 않는다** -- 읽기가 실패해도 아래에서 견주게 된다 */
				rc = ibmphp_hpc_readslot(pslot, READ_ALLSTAT, NULL);
				/* [한국어] 어느 슬롯을 처리하는지 남긴다 */
				debug("%s - call process_changeinstatus for slot[%d]\n", __func__, i);
				/* [한국어] **무엇이 달라졌는지 판단하고 처리하는 일을 넘긴다** */
				process_changeinstatus(pslot, &myslot);
			/* [한국어] 슬롯을 찾지 못했다 */
			} else {
				/* [한국어] 오류로 표시한다 */
				rc = -EINVAL;
				/* [한국어] **래치 비트는 있는데 대응하는 슬롯이 없다는 뜻이다** --
				 * EBDA 가 알려 준 범위와 실제 슬롯 목록이 어긋난 경우다 */
				err("%s - Error bad pointer for slot[%d]\n", __func__, i);
			}
		}
	}
	/* [한국어] 결과를 남긴다 */
	debug("%s - Exit rc[%d]\n", __func__, rc);
	/* [한국어] **호출자 poll_hpc 가 이 값을 확인하지 않는다** */
	return rc;
}

/*----------------------------------------------------------------------
* Name:    ibmphp_hpc_start_poll_thread
*
* Action:  start polling thread
*---------------------------------------------------------------------*/
/* [한국어]
 * ibmphp_hpc_start_poll_thread - 폴링 커널 스레드를 띄운다
 *
 * @return: 성공 0, 실패면 음수.
 *
 * **인터럽트가 없는 컨트롤러를 감시할 유일한 수단을 시작한다.**
 * 이 스레드가 없으면 카드를 꽂거나 빼도 아무도 알아채지 못한다.
 *
 * **kthread_run 이 만들기와 깨우기를 함께 한다.** 이름을 "hpc_poll" 로
 * 주므로 ps 에서 그 이름으로 보인다.
 *
 * **IS_ERR 로 확인하고 PTR_ERR 로 오류를 꺼낸다.** kthread_run 이
 * 실패를 포인터에 실어 돌려주는 커널 관용이며, NULL 검사로는 잡히지 않는다.
 *
 * **__init 표시가 붙어 있다.** 부팅·모듈 로드 때 한 번만 불리므로
 * 그 뒤 코드를 버릴 수 있게 한다.
 *
 * **ibmphp_poll_thread 전역에 담아 둔다.**
 * ibmphp_hpc_stop_poll_thread() 가 그것으로 스레드를 세운다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(모듈 초기화).
 *
 * 호출 체인:
 *   ibmphp_core.c 의 초기화 경로 → [이 함수] → kthread_run()
 */
int __init ibmphp_hpc_start_poll_thread(void)
{
	/* [한국어] 진입을 남긴다 */
	debug("%s - Entry\n", __func__);

	/* [한국어] **폴링 스레드를 만들고 곧바로 깨운다.**
	 * kthread_create 와 wake_up_process 를 합친 것이며,
	 * 이름 "hpc_poll" 이 ps 에 그대로 보인다.
	 * 둘째 인자 NULL 이 poll_hpc 의 data 인자가 되는데 쓰이지 않는다 */
	ibmphp_poll_thread = kthread_run(poll_hpc, NULL, "hpc_poll");
	/* [한국어] **실패를 포인터에 실어 돌려주므로 NULL 검사로는 잡히지 않는다.**
	 * IS_ERR 이 그 커널 관용을 확인한다 */
	if (IS_ERR(ibmphp_poll_thread)) {
		/* [한국어] 스레드를 띄우지 못했음을 남긴다 */
		err("%s - Error, thread not started\n", __func__);
		/* [한국어] **포인터에 실려 온 오류 코드를 꺼내 올린다** */
		return PTR_ERR(ibmphp_poll_thread);
	}
	/* [한국어] 성공. **이 뒤로 폴링이 2초마다 돈다** */
	return 0;
}

/*----------------------------------------------------------------------
* Name:    ibmphp_hpc_stop_poll_thread
*
* Action:  stop polling thread and cleanup
*---------------------------------------------------------------------*/
/* [한국어]
 * ibmphp_hpc_stop_poll_thread - 폴링 스레드를 세우고 뒷정리한다
 *
 * @return: 없음.
 *
 * **ibmphp_hpc_start_poll_thread() 의 짝이며, 순서에 눈여겨볼 점이 여럿이다.**
 *
 * 1. **kthread_stop() 이 스레드가 돌아올 때까지 기다린다.**
 *    그 함수가 kthread_should_stop() 을 참으로 만들고, poll_hpc 의
 *    루프가 그것을 보고 빠져나온다.
 * 2. **ibmphp_lock_operations() 로 자료구조를 잠근다.**
 *    폴링이 이미 끝났으므로 경쟁할 상대가 없으나, 뒤이은 정리를
 *    위층과 겹치지 않게 하려는 것으로 보인다.
 * 3. **wait_for_completion(&exit_complete) 로 종료 알림을 기다린다.**
 *    poll_hpc 가 루프를 나가며 complete() 를 부른다.
 *    **kthread_stop() 이 이미 스레드 종료를 기다린 뒤이므로 이 대기는
 *    곧바로 돌아온다** -- 두 겹의 확인인 셈이다.
 * 4. **free_hpc_access() 를 부른다.**
 *    **이 함수는 앞서 get_hpc_access() 를 부른 적이 없다.**
 *    곧 잡지 않은 sem_hpcaccess 뮤텍스를 푸는 셈이다.
 *    원본에서 확인했으며 코드는 손대지 않고 사실만 적는다.
 *    폴링 스레드가 그 뮤텍스를 쥔 채 죽었을 경우를 대비한 것으로
 *    보이나, poll_hpc 는 sem_hpcaccess 를 직접 잡지 않고
 *    ibmphp_hpc_readslot() 안에서 잡았다 놓으므로 그 상황은 생기지 않는다.
 * 5. ibmphp_unlock_operations() 로 자료구조 잠금을 놓는다.
 *
 * **단계마다 debug 로그가 앞뒤로 붙어 있다.** 종료 순서가 얽히기 쉬운
 * 자리라 어디서 멈췄는지 알 수 있게 한 것으로 보인다.
 *
 * **__exit 표시가 붙어 있다.** 모듈을 내릴 때만 불린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(모듈 언로드). 잠들 수 있다.
 *
 * 호출 체인:
 *   ibmphp_core.c 의 종료 경로 → [이 함수]
 *     → kthread_stop(), ibmphp_lock_operations(), wait_for_completion(),
 *       free_hpc_access(), ibmphp_unlock_operations()
 */
void __exit ibmphp_hpc_stop_poll_thread(void)
{
	/* [한국어] 진입을 남긴다 */
	debug("%s - Entry\n", __func__);

	/* [한국어] **종료를 요청하고 스레드가 돌아올 때까지 기다린다.**
	 * poll_hpc 의 kthread_should_stop() 이 참이 되어 루프를 빠진다 */
	kthread_stop(ibmphp_poll_thread);
	/* [한국어] **단계마다 앞뒤로 로그를 남긴다.**
	 * 종료 순서가 얽히기 쉬운 자리라 어디서 멈췄는지 알 수 있게 한 것으로 보인다 */
	debug("before locking operations\n");
	/* [한국어] **자료구조를 잠근다.**
	 * 폴링이 이미 끝났으므로 경쟁할 상대가 없으나,
	 * 뒤이은 정리를 위층과 겹치지 않게 하려는 것으로 보인다 */
	ibmphp_lock_operations();
	/* [한국어] 잠갔음을 남긴다 */
	debug("after locking operations\n");

	// wait for poll thread to exit
	/* [한국어] 기다리기 전을 남긴다 */
	debug("before exit_complete down\n");
	/* [한국어] **폴링 스레드가 complete() 를 부를 때까지 기다린다.**
	 * **kthread_stop() 이 이미 스레드 종료를 기다렸으므로 곧바로 돌아온다** --
	 * 두 겹의 확인인 셈이다 */
	wait_for_completion(&exit_complete);
	/* [한국어] 기다리기가 끝났음을 남긴다 */
	debug("after exit_completion down\n");

	// cleanup
	/* [한국어] 정리 전을 남긴다 */
	debug("before free_hpc_access\n");
	/* [한국어] **sem_hpcaccess 를 푼다. 그런데 이 함수는 그것을 잡은 적이 없다.**
	 * 원본에서 확인했으며 코드는 손대지 않고 사실만 적는다.
	 * 폴링 스레드가 그 뮤텍스를 쥔 채 죽은 경우를 대비한 것으로 보이나,
	 * poll_hpc 는 그것을 직접 잡지 않고 ibmphp_hpc_readslot() 안에서
	 * 잡았다 놓으므로 그런 상황은 생기지 않는다 */
	free_hpc_access();
	/* [한국어] 정리했음을 남긴다 */
	debug("after free_hpc_access\n");
	/* [한국어] 자료구조 잠금을 놓고 저수준 로그를 다시 막는다 */
	ibmphp_unlock_operations();
	/* [한국어] 풀었음을 남긴다 */
	debug("after unlock operations\n");

	/* [한국어] 종료를 남긴다 */
	debug("%s - Exit\n", __func__);
}

/*----------------------------------------------------------------------
* Name:    hpc_wait_ctlr_notworking
*
* Action:  wait until the controller is in a not working state
*
* Return   0, HPC_ERROR
* Value:
*---------------------------------------------------------------------*/
/* [한국어]
 * hpc_wait_ctlr_notworking - 컨트롤러가 일을 놓을 때까지 기다린다
 *
 * @timeout: 최대 몇 초를 기다릴지.
 * @ctlr_ptr: 대상 컨트롤러.
 * @wpg_bbar: I2C 갈래에서 쓸 매핑된 창.
 * @pstatus: 마지막으로 읽은 상태 바이트를 담아 돌려줄 자리.
 * @return: 성공 0, 실패나 시간 초과면 HPC_ERROR.
 *
 * **이 컨트롤러가 한 번에 명령 하나만 처리하기 때문에 필요한 함수다.**
 * 읽기와 쓰기 두 진입점이 모두 이것으로 시작하며, 쓰기는 명령을 낸 뒤에도
 * 이것으로 완료를 확인한다.
 *
 * **컨트롤러 자신의 인덱스(WPG_CTLR_INDEX, 0x0F)를 읽는다.**
 * 슬롯이 아니라 컨트롤러의 상태 바이트이며, 그 안의 한 비트가
 * "지금 일하는 중인가" 를 나타낸다. CTLR_WORKING 매크로가 그것을 꺼낸다.
 *
 * **두 가지 끝내는 조건이 있다.**
 * 1. 읽은 값이 HPC_ERROR 면 아래층이 실패한 것이다.
 * 2. CTLR_WORKING 이 HPC_CTLR_WORKING_NO 면 놀고 있는 것이다.
 *
 * **둘 다 아니면 1초 자고 다시 읽는다.** 타임아웃 검사가 감소보다
 * 앞에 오므로, timeout 이 60 이면 실제로 61번 잔 뒤 포기한다.
 *
 * **pstatus 를 늘 채운다.** 성공하든 실패하든 마지막으로 읽은 값이
 * 들어가므로, 호출자가 그것을 그대로 상태로 쓸 수 있다 --
 * ibmphp_hpc_readslot() 의 READ_CTLRSTATUS 갈래가 그렇게 한다.
 *
 * **HPC_ERROR 로 끝날 때도 done 을 세우고 루프를 빠진다.**
 * 그 뒤의 `if (!done)` 이 거짓이 되므로 잠들지 않고 곧바로 돌아간다.
 *
 * **오류 메시지가 "HPCreadslot" 으로 시작한다** -- 쓰기 경로에서
 * 불릴 때도 그 문구가 찍히며, 상류 코드가 그대로 두었다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. msleep 으로 잠든다.
 *
 * 호출 체인:
 *   ibmphp_hpc_readslot / ibmphp_hpc_writeslot
 *     → [이 함수] → ctrl_read(), msleep()
 */
static int hpc_wait_ctlr_notworking(int timeout, struct controller *ctlr_ptr, void __iomem *wpg_bbar,
				    u8 *pstatus)
{
	/* [한국어] 성공을 기본값으로 둔다 */
	int rc = 0;
	/* [한국어] **루프를 끝낼 표시.** break 대신 표시를 쓰는 것이 이 파일의 관용이다 */
	u8 done = 0;

	/* [한국어] **__func__ 대신 함수 이름을 손으로 적었다.** 상류 그대로다 */
	debug_polling("hpc_wait_ctlr_notworking - Entry timeout[%d]\n", timeout);

	/* [한국어] 컨트롤러가 놀거나 오류가 날 때까지 돈다 */
	while (!done) {
		/* [한국어] **컨트롤러 자신의 상태 바이트를 읽는다.**
		 * 슬롯이 아니라 컨트롤러이므로 인덱스가 0x0F 로 고정된다.
		 * **pstatus 를 늘 채우므로 호출자가 그것을 그대로 상태로 쓸 수 있다** --
		 * ibmphp_hpc_readslot() 의 READ_CTLRSTATUS 갈래가 그렇게 한다 */
		*pstatus = ctrl_read(ctlr_ptr, wpg_bbar, WPG_CTLR_INDEX);
		/* [한국어] **아래층 읽기가 실패했다** */
		if (*pstatus == HPC_ERROR) {
			/* [한국어] 오류를 담는다 */
			rc = HPC_ERROR;
			/* [한국어] **표시를 세워 아래 잠자기를 건너뛴다** */
			done = 1;
		}
		/* [한국어] **컨트롤러가 일을 놓았는가.**
		 * 상태 바이트의 한 비트를 그 매크로가 꺼낸다(ibmphp.h) */
		if (CTLR_WORKING(*pstatus) == HPC_CTLR_WORKING_NO)
			/* [한국어] 기다리기가 끝났다 */
			done = 1;
		/* [한국어] 아직 일하는 중이다 */
		if (!done) {
			/* [한국어] **1초 양보한다.**
			 * I2C 루프의 10밀리초와 달리 여기는 초 단위다 --
			 * 컨트롤러 명령 자체가 오래 걸리기 때문이다 */
			msleep(1000);
			/* [한국어] **감소보다 검사가 먼저 온다.**
			 * 초기값 60 이면 실제로 61번 잔 뒤 포기한다 */
			if (timeout < 1) {
				/* [한국어] 더 기다리지 않는다 */
				done = 1;
				/* [한국어] **쓰기 경로에서 불릴 때도 "HPCreadslot" 이 찍힌다.**
				 * 상류 그대로이며 코드는 손대지 않는다 */
				err("HPCreadslot - Error ctlr timeout\n");
				/* [한국어] 시간 초과를 오류로 알린다 */
				rc = HPC_ERROR;
			/* [한국어] 아직 시간이 남았다 */
			} else
				/* [한국어] 남은 초 수를 줄인다 */
				timeout--;
		}
	}
	/* [한국어] 결과와 마지막으로 읽은 상태를 함께 남긴다 */
	debug_polling("hpc_wait_ctlr_notworking - Exit rc[%x] status[%x]\n", rc, *pstatus);
	/* [한국어] 성공 0, 실패나 시간 초과면 HPC_ERROR */
	return rc;
}
