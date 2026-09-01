// SPDX-License-Identifier: GPL-2.0+
/*
 * Compaq Hot Plug Controller Driver
 *
 * Copyright (C) 1995,2001 Compaq Computer Corporation
 * Copyright (C) 2001 Greg Kroah-Hartman <greg@kroah.com>
 * Copyright (C) 2001 IBM Corp.
 *
 * All rights reserved.
 *
 * Send feedback to <greg@kroah.com>
 *
 * Jan 12, 2003 -	Added 66/100/133MHz PCI-X support,
 *			Torben Mathiasen <torben.mathiasen@hp.com>
 */

/*
 * [한국어 설명] Compaq 핫플러그 컨트롤러 드라이버의 모듈 진입점과 슬롯 등록
 * (cpqphp_core.c)
 *
 * === 파일의 역할 ===
 * cpqphp 드라이버가 커널에 붙고 떨어지는 전 과정을 담당한다. 모듈이 올라오면
 * pci_register_driver 로 핫플러그 컨트롤러 클래스의 PCI 장치를 기다리고,
 * 장치를 만나면 cpqhpc_probe 가 그 컨트롤러의 능력을 알아내고 MMIO 를 매핑하고
 * 인터럽트를 걸고 슬롯을 하나씩 만들어 리눅스 핫플러그 코어에 등록한다.
 * 그 과정에서 시스템 ROM 을 뒤져 SMBIOS 표와 HRT(Hot Plug Resource Table)를
 * 찾아내고, BIOS 의 $PIR 인터럽트 라우팅 표를 받아 슬롯의 물리 번호를 알아낸다.
 * 또한 sysfs 로 들어오는 여덟 개의 슬롯 조작 콜백(전원 켜기·끄기, LED 켜기,
 * 상태 읽기)을 구현해 cpqphp_ctrl.c 의 실제 동작 함수로 이어 준다.
 * 마지막으로 모듈이 내려갈 때 자원과 목록을 전부 되돌려 준다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * cpqphp 드라이버는 네 파일로 나뉜다. 이 파일이 **입구이자 출구** 이고,
 * cpqphp_ctrl.c 가 사건 처리와 상태 기계, cpqphp_pci.c 가 설정공간 저장과
 * 자원 관리, cpqphp_nvram.c 가 Compaq 고유의 NVRAM 접근을 맡는다.
 * 공유 선언은 모두 cpqphp.h 에 있다.
 *
 * 위에서 아래로 본 호출 체인은 이렇다.
 *   module_init → cpqhpc_init → pci_register_driver
 *     → (PCI 코어가 장치를 찾으면) cpqhpc_probe
 *        → one_time_init: $PIR 표 받기, 사건 스레드 띄우기, ROM 매핑,
 *          SMBIOS 표 찾기 (모듈 전체에 한 번만)
 *        → get_slot_mapping: 첫 슬롯의 물리 번호 알아내기
 *        → cpqhp_save_config(cpqphp_pci.c): 이 버스의 설정공간 전부 저장
 *        → cpqhp_find_available_resources(cpqphp_pci.c): HRT 로 자유 목록 채우기
 *        → ctrl_slot_setup: 슬롯마다 struct slot 을 만들어 pci_hp_register
 *        → request_irq(cpqhp_ctrl_intr): 인터럽트 열기
 *   사용자가 sysfs 에 쓰면
 *     핫플러그 코어 → cpqphp_hotplug_slot_ops 의 여덟 콜백 중 하나
 *       → cpqhp_process_SI / cpqhp_process_SS / cpqhp_hardware_test
 *         (모두 cpqphp_ctrl.c)
 *   module_exit → cpqhpc_cleanup → unload_cpqphpd → ctrl_slot_cleanup
 *
 * 실행 컨텍스트는 세 갈래다. probe 와 exit 는 프로세스 컨텍스트,
 * sysfs 콜백도 프로세스 컨텍스트(사용자가 write 한 문맥), 그리고 이 파일이
 * 등록한 cpqhp_ctrl_intr 만 인터럽트 컨텍스트에서 돈다. 이 파일 안의 코드는
 * 인터럽트 컨텍스트에서 도는 것이 없다.
 *
 * === 타 모듈과의 연결 ===
 * **위쪽(이 파일을 부르는 쪽)**: 리눅스 PCI 코어가 probe 를 부르고,
 * 리눅스 PCI 핫플러그 코어(pci_hp_register 로 등록한 뒤)가 sysfs 콜백을 부른다.
 * 커널 인터럽트 코어는 여기서 request_irq 로 건 cpqhp_ctrl_intr 을 부른다.
 *
 * **아래쪽(이 파일이 부르는 쪽)**: cpqphp_ctrl.c 의 cpqhp_process_SI/SS 와
 * cpqhp_hardware_test, cpqphp_pci.c 의 cpqhp_save_config 와
 * cpqhp_find_available_resources 와 cpqhp_get_bus_dev 와 cpqhp_slot_find,
 * cpqphp_nvram.c 의 compaq_nvram_init/store, 그리고 cpqphp.h 의 인라인 함수들
 * (set_SOGO, amber_LED_on/off, green_LED_off, slot_disable, get_slot_enabled,
 * cpq_get_latch_status, get_presence_status, wait_for_ctrl_irq).
 *
 * **데이터 흐름**: 시스템 ROM(0xF0000~)에서 SMBIOS 표와 HRT 를 읽어 들이고,
 * BIOS 에서 $PIR 표를 받아 cpqhp_routing_table 전역에 둔다. 그 셋을 근거로
 * struct controller 하나와 struct slot 여럿을 만들어 채우고, 슬롯마다 SMBIOS
 * Type 9(System Slot) 항목을 찾아 slot->p_sm_slot 에 이어 둔다. 카드가 쓸 수
 * 있는 IO·메모리·버스 번호는 HRT 에서 뽑아 컨트롤러의 네 자유 목록이 된다.
 *
 * **공유하는 핵심 자료구조**: 이 파일이 정의하는 전역 다섯 개
 * (cpqhp_debug, cpqhp_legacy_mode, cpqhp_ctrl_list, cpqhp_slot_list[256],
 * cpqhp_routing_table)를 다른 세 파일이 extern 으로 쓴다. 특히
 * cpqhp_slot_list 는 버스 번호로 색인하는 전역 배열 하나로 온 시스템의
 * PCI 함수 정보를 들고 있으며, cpqphp_pci.c 가 채우고 이 파일과
 * cpqphp_ctrl.c 가 읽는다.
 *
 * === 주요 함수/구조체 요약 ===
 * - cpqhpc_probe: 이 파일에서 가장 긴 함수. 컨트롤러 하나를 처음부터 끝까지
 *   세운다. 벤더·서브시스템 ID 로 능력을 정하고, MMIO 를 잡고, 자원과 슬롯을
 *   만들고, 인터럽트를 열고, 빈 슬롯의 전원을 끈다. 실패하면 여덟 단계의
 *   goto 라벨을 거슬러 올라가며 정확히 되돌린다.
 * - one_time_init: 컨트롤러가 여럿이어도 한 번만 해야 하는 일(전역 표 받기,
 *   ROM 매핑, SMBIOS 찾기)을 initialized 표시로 묶어 둔다.
 * - ctrl_slot_setup: 컨트롤러의 슬롯 수만큼 struct slot 을 만들어 능력 비트를
 *   채우고 pci_hp_register 로 코어에 등록한다. 5초 타이머도 여기서 준비한다.
 * - get_slot_mapping: $PIR 표를 훑어 PCI 장치 번호에 대응하는 물리 슬롯 번호를
 *   찾는다. 찾지 못하면 브리지 뒤라 보고 브리지의 슬롯 번호를 쓴다.
 * - get_SMBIOS_entry / get_subsequent_smbios_entry: 가변 길이 SMBIOS 표를
 *   길이 필드와 이중 NULL 종결자로 훑는 한 쌍.
 * - cpqphp_hotplug_slot_ops: sysfs 로 드러나는 여덟 콜백의 표. 이 드라이버가
 *   바깥 세계에 보이는 면 전부다.
 * - unload_cpqphpd: 컨트롤러 목록과 cpqhp_slot_list 256칸을 전부 훑으며
 *   네 자원 목록을 하나씩 해제한다.
 *
 * === 이 파일을 읽을 때 알아 두면 좋은 것 ===
 * **왜 ROM 을 직접 뒤지는가**: 2001년 커널에는 SMBIOS 를 파싱해 주는 공통
 * 코드(dmi_scan)가 지금처럼 갖춰져 있지 않았고, HRT 는 Compaq 고유 표라
 * 어차피 이 드라이버가 직접 읽어야 했다. 그래서 물리 주소 0xF0000 을
 * ioremap 하고 16바이트씩 서명을 찾아 걷는 코드가 남아 있다.
 *
 * **왜 pci_bus 구조체를 복사하는가**: 설정공간을 읽을 때 버스 번호만 갈아
 * 끼우기 위해서다. 진짜 버스 구조체의 number 를 건드리면 커널 전체가
 * 깨지므로 kmemdup 으로 사본을 하나 만들어 그것만 고친다. 지금 기준으로는
 * 위험한 관용구이며, 요즘은 도메인을 인지하는 API 를 쓴다.
 *
 * **remove 콜백이 없다**: cpqphc_driver 의 .remove 가 주석 처리되어 있어
 * 컨트롤러를 뽑는 경로가 없다. 그래서 ctrl_slot_cleanup 에 닿는 길은
 * 모듈을 내리는 unload_cpqphpd 하나뿐이다.
 *
 * **세대 대비**: 같은 문제를 뒤 세대가 어떻게 다르게 푸는지가 이 디렉터리의
 * 볼거리다. 이 파일이 ROM 을 직접 뒤져 슬롯 정보를 얻는 자리를 acpiphp 는
 * ACPI 네임스페이스에서, pciehp 는 PCIe 슬롯 능력 레지스터에서 얻는다.
 */

/* [한국어] 모듈 뼈대(module_init, MODULE_LICENSE 등). 이 파일이 커널 모듈이므로 필수다 */
#include <linux/module.h>
/* [한국어] module_param 매크로. 아래의 power_mode 와 debug 두 인자를 위해 필요하다 */
#include <linux/moduleparam.h>
/* [한국어] printk 와 기본 매크로들 */
#include <linux/kernel.h>
/* [한국어] u8, u16, u32 같은 고정 폭 정수 타입.
 * 레지스터 폭이 곧 의미인 코드라 이 타입들이 곳곳에 나온다 */
#include <linux/types.h>
/* [한국어] proc 파일 시스템 인터페이스.
 * **이 파일에서 proc 관련 함수를 쓰는 곳을 찾을 수 없다** --
 * debugfs 로 옮겨 가기 전의 잔재로 보인다 */
#include <linux/proc_fs.h>
/* [한국어] kzalloc, kfree, kmemdup. 슬롯과 컨트롤러 구조체를 힙에 만든다 */
#include <linux/slab.h>
/* [한국어] struct work_struct 와 schedule_work.
 * controller 의 int_task_event 필드가 이 타입이며,
 * 인터럽트가 미룬 일을 스레드 문맥으로 넘기는 데 쓴다 */
#include <linux/workqueue.h>
/* [한국어] PCI 코어 API. pci_register_driver, 설정공간 접근, 자원 조회 전부 */
#include <linux/pci.h>
/* [한국어] **핫플러그 코어 API.** struct hotplug_slot, hotplug_slot_ops,
 * pci_hp_register 가 여기 있다. 이 드라이버가 코어에 붙는 접점이다.
 * **이 스파스 체크아웃에는 이 헤더가 없어** 안쪽 정의를 확인하지 못했다 */
#include <linux/pci_hotplug.h>
/* [한국어] __init 과 __exit 표시. 초기화 코드를 부팅 뒤 버릴 수 있게 한다 */
#include <linux/init.h>
/* [한국어] request_irq, free_irq, irqreturn_t, IRQF_SHARED.
 * 컨트롤러 인터럽트를 여는 데 필요하다 */
#include <linux/interrupt.h>

/* [한국어] 사용자 공간 메모리 접근 함수.
 * **이 파일에서 copy_to_user 계열을 쓰는 곳을 찾을 수 없다** --
 * sysfs 로 넘어가기 전 직접 ioctl 을 다루던 시절의 잔재로 보인다 */
#include <linux/uaccess.h>

/* [한국어] **이 드라이버의 공유 헤더.** 구조체, 레지스터 오프셋 enum,
 * 로그 매크로, 인라인 함수, 그리고 다른 세 파일의 함수 선언이 전부 여기 있다 */
#include "cpqphp.h"
/* [한국어] Compaq 고유 NVRAM 접근 함수 선언.
 * compaq_nvram_init 과 compaq_nvram_store 를 쓰기 위해 필요하다.
 * **이 드라이버가 x86 BIOS 의 int15 진입점을 직접 부르는 부분이다** */
#include "cpqphp_nvram.h"


/* Global variables */
/* [한국어] **디버그 출력 켜기 표시. cpqphp.h 의 dbg 매크로가 이 값을 본다.**
 * 설정자: cpqhpc_init 이 모듈 인자 debug 를 한 번 복사한다.
 * 읽는 자: 네 파일의 모든 dbg 호출, 그리고 one_time_init 이
 *   $PIR 표를 찍을지 정할 때.
 * **BSS 에 놓이므로 초기값은 0(꺼짐)이다** */
int cpqhp_debug;
/* [한국어] 레거시 모드 표시.
 * 설정자: cpqhpc_probe 가 IRQ 번호가 0x10 보다 작으면 1 로 세운다.
 * **읽어서 갈래를 나누는 코드를 이 트리에서 찾을 수 없다** --
 * 설정만 되고 쓰이지 않는 전역이다 */
int cpqhp_legacy_mode;
/* [한국어] **컨트롤러 전역 리스트의 머리.**
 * 설정자: cpqhpc_probe 가 컨트롤러마다 머리에 매단다.
 * 읽는 자: unload_cpqphpd 가 훑으며 전부 해제한다.
 * 원문 주석이 초기값이 NULL 임을 밝힌다 -- BSS 전역이라 자동으로 그렇다.
 * **락이 없다.** probe 와 모듈 내리기가 겹치지 않는다고 본 것이다 */
struct controller *cpqhp_ctrl_list;	/* = NULL */
/* [한국어] **버스 번호로 색인하는 pci_func 리스트 배열. 온 시스템 몫이다.**
 * 칸 하나가 그 버스에 있는 PCI 함수들의 연결 리스트 머리다.
 * 설정자: cpqhp_slot_create(cpqphp_pci.c)가 노드를 만들어 매단다.
 * 읽는 자: cpqhp_slot_find 가 버스·장치·함수 번호로 찾는다.
 * **PCI 도메인 개념이 없다** -- 도메인이 여럿이면 버스 번호가 겹칠 수
 *   있으나 이 배열은 그것을 구분하지 못한다. 2001년에는 문제가 되지
 *   않았지만 지금 기준으로는 결함이다.
 * **락이 없다** */
struct pci_func *cpqhp_slot_list[256];
/* [한국어] **BIOS 의 $PIR 인터럽트 라우팅 표 사본.**
 * 설정자: init_cpqhp_routing_table 이 pcibios_get_irq_routing_table 로 받는다.
 * 읽는 자: get_slot_mapping, pci_print_IRQ_route,
 *   cpqhp_routing_table_length(cpqphp.h), cpqhp_set_irq(cpqphp_pci.c).
 * **해제하는 곳이 init_cpqhp_routing_table 의 실패 경로뿐이다** --
 *   모듈을 내릴 때 이 표를 kfree 하는 코드를 찾을 수 없다 */
struct irq_routing_table *cpqhp_routing_table;

/* local variables */
/* [한국어] **SMBIOS 진입점 구조체를 가리키는 매핑 주소.**
 * 설정자: one_time_init 이 detect_SMBIOS_pointer 의 결과를 담는다.
 * 읽는 자: ctrl_slot_setup 이 슬롯의 Type 9 항목을 찾을 때 넘겨받는다.
 * **iounmap 되지 않는다** -- cpqhp_rom_start 안을 가리키는 주소이고
 *   그쪽이 해제될 때 함께 무효가 되므로 따로 놓을 것이 없다 */
static void __iomem *smbios_table;
/* [한국어] **SMBIOS 표 본문의 매핑 주소.**
 * 설정자: one_time_init 이 진입점의 ST_ADDRESS/ST_LENGTH 를 읽어
 *   따로 ioremap 한 결과.
 * 읽는 자: ctrl_slot_setup 과 get_SMBIOS_entry.
 * **표 본문은 진입점과 다른 곳에 있을 수 있어 따로 매핑한다** */
static void __iomem *smbios_start;
/* [한국어] **시스템 ROM 구간(0xF0000, 64KB)의 매핑 주소.**
 * 설정자: one_time_init 이 ioremap 한다.
 * 읽는 자: detect_SMBIOS_pointer 의 탐색 범위,
 *   cpqhp_find_available_resources 의 HRT 탐색 범위,
 *   compaq_nvram_init/store 의 int15 진입점 탐색 범위.
 * **이 드라이버가 펌웨어와 만나는 유일한 창구다** */
static void __iomem *cpqhp_rom_start;
/* [한국어] **빈 슬롯의 전원을 켠 채 둘지 정하는 모듈 인자.**
 * 0 이면 probe 가 카드 없는 슬롯의 전원을 끈다.
 * 설정자: module_param 으로 사용자가 준 값. **그런데 one_time_init 이
 *   맨 앞에서 0 으로 덮어쓴다** -- 사용자가 준 값이 무시되는 셈이며,
 *   그 이유가 코드에 적혀 있지 않다.
 * 읽는 자: cpqhpc_probe 의 마무리 부분 */
static bool power_mode;
/* [한국어] 디버그 출력 모듈 인자.
 * 설정자: module_param. 읽는 자: cpqhpc_init 이 cpqhp_debug 로 복사한다.
 * **sysfs 로 0644 로 노출되어 나중에 바꿀 수 있지만**, 복사본인
 * cpqhp_debug 는 따라 바뀌지 않으므로 실제 효과는 모듈을 올릴 때뿐이다 */
static bool debug;
/* [한국어] **one_time_init 이 이미 끝났는지 나타내는 표시.**
 * 설정자: one_time_init 이 성공하면 1 로 세운다.
 * 읽는 자: one_time_init 자신의 맨 앞 검사, 그리고 unload_cpqphpd 가
 *   사건 기구를 세울지 정할 때.
 * **컨트롤러가 여럿일 때 probe 가 여러 번 불리는 것에 대비한 장치다** */
static int initialized;

/* [한국어] 드라이버 판 번호 문자열. cpqhpc_init 이 시작 로그에 찍는다.
 * **0.9.8 에서 멈춰 있다** -- 기능이 굳은 지 오래된 드라이버다 */
#define DRIVER_VERSION	"0.9.8"
/* [한국어] MODULE_AUTHOR 에 넘길 저작자 문자열 */
#define DRIVER_AUTHOR	"Dan Zink <dan.zink@compaq.com>, Greg Kroah-Hartman <greg@kroah.com>"
/* [한국어] MODULE_DESCRIPTION 과 시작 로그에 함께 쓰는 설명 문자열 */
#define DRIVER_DESC	"Compaq Hot Plug PCI Controller Driver"

/* [한국어] modinfo 에 저작자를 새긴다 */
MODULE_AUTHOR(DRIVER_AUTHOR);
/* [한국어] modinfo 에 설명을 새긴다 */
MODULE_DESCRIPTION(DRIVER_DESC);
/* [한국어] **라이선스를 GPL 로 선언한다.** 이것이 없으면 커널이 모듈을
 * "오염됨(tainted)" 으로 표시하고 GPL 전용 심볼을 쓸 수 없다 */
MODULE_LICENSE("GPL");

/* [한국어] power_mode 를 모듈 인자로 노출한다.
 * 0644 는 sysfs 에서 소유자 쓰기·모두 읽기 권한이다 */
module_param(power_mode, bool, 0644);
/* [한국어] modinfo 에 보이는 power_mode 설명문 */
MODULE_PARM_DESC(power_mode, "Power mode enabled or not");

/* [한국어] debug 를 모듈 인자로 노출한다 */
module_param(debug, bool, 0644);
/* [한국어] modinfo 에 보이는 debug 설명문 */
MODULE_PARM_DESC(debug, "Debugging mode enabled or not");

/* [한국어] **옛 문자 장치의 마이너 번호.**
 * 이 드라이버가 /dev 노드로 사용자 공간과 통신하던 시절의 잔재이며,
 * **지금 이 파일 어디에서도 쓰이지 않는다.** sysfs 로 옮겨 간 뒤
 * 정의만 남았다 */
#define CPQHPC_MODULE_MINOR 208

/* [한국어]
 * is_slot64bit - 이 슬롯이 64비트 폭인지 SMBIOS 표에서 읽어 온다
 *
 * @slot: 능력을 알고 싶은 슬롯. p_sm_slot 이 채워져 있어야 한다.
 * @return: 64비트 슬롯이면 1, 아니면 0.
 *
 * **하드웨어 레지스터가 아니라 펌웨어가 적어 둔 표를 본다.** 슬롯의 물리적
 * 폭은 컨트롤러가 알려 주는 값이 아니라 메인보드 설계에 달린 값이므로,
 * BIOS 가 SMBIOS Type 9(System Slot) 항목에 적어 둔 것을 읽는 수밖에 없다.
 *
 * slot->p_sm_slot 은 ctrl_slot_setup 이 슬롯 번호가 맞는 Type 9 항목을 찾아
 * 이어 둔 __iomem 포인터다. 거기에 SMBIOS_SLOT_WIDTH 오프셋을 더해 readb 한다.
 * 값 0x06 이 64비트를 뜻하는 것은 SMBIOS 규격이 정한 열거값이며,
 * **그 규격 문서는 이 트리에 없으므로 코드가 쓰는 값 그대로 적는다.**
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe 경로). 잠들지 않는다.
 *
 * 호출 체인:
 *   ctrl_slot_setup → [이 함수] → readb
 */
static inline int is_slot64bit(struct slot *slot)
{
	/* [한국어] **SMBIOS Type 9 항목의 슬롯 폭 필드를 읽어 0x06 인지 본다.**
	 * slot->p_sm_slot 은 ctrl_slot_setup 이 찾아 이어 둔 __iomem 주소이고,
	 * SMBIOS_SLOT_WIDTH 는 cpqphp.h 의 offsetof 상수다.
	 * **0x06 이 64비트를 뜻하는 것은 SMBIOS 규격의 열거값이며 그 문서는
	 * 이 트리에 없다.** 코드가 쓰는 값 그대로 적는다.
	 * **p_sm_slot 이 NULL 이면 NULL 역참조가 된다** -- 호출자인
	 * ctrl_slot_setup 이 검사 없이 부르며, 표에서 항목을 못 찾으면 NULL 이
	 * 남는다. 코드는 손대지 않고 사실만 적는다 */
	return (readb(slot->p_sm_slot + SMBIOS_SLOT_WIDTH) == 0x06) ? 1 : 0;
}

/* [한국어]
 * is_slot66mhz - 이 슬롯이 66MHz 슬롯인지 SMBIOS 표에서 읽어 온다
 *
 * @slot: 능력을 알고 싶은 슬롯.
 * @return: 66MHz 슬롯이면 1, 아니면 0.
 *
 * is_slot64bit 과 짝을 이루는 함수이며 같은 표의 다른 필드를 본다.
 * **다만 보는 필드가 WIDTH 가 아니라 TYPE 이다** -- SMBIOS 는 슬롯 종류
 * 열거값 안에 속도까지 담아 두었고, 0x0E 가 66MHz PCI 를 가리킨다.
 * 그 열거값의 근거 문서도 이 트리에 없다.
 *
 * 여기서 얻은 두 값이 slot->capabilities 의 PCISLOT_64_BIT_SUPPORTED 와
 * PCISLOT_66_MHZ_SUPPORTED 비트가 된다. **다만 그 capabilities 필드를 읽는
 * 곳을 이 드라이버에서 찾을 수 없다** -- 원본 주석도 FIXME 로 쓰이지 않는
 * 값이라고 밝힌다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe 경로).
 *
 * 호출 체인:
 *   ctrl_slot_setup → [이 함수] → readb
 */
static inline int is_slot66mhz(struct slot *slot)
{
	/* [한국어] **같은 항목의 슬롯 종류 필드를 읽어 0x0E 인지 본다.**
	 * SMBIOS 는 종류 열거값 안에 속도까지 담아 두었고 0x0E 가 66MHz PCI 다.
	 * 앞 함수가 WIDTH 를, 이 함수가 TYPE 을 보는 차이가 있다 */
	return (readb(slot->p_sm_slot + SMBIOS_SLOT_TYPE) == 0x0E) ? 1 : 0;
}

/**
 * detect_SMBIOS_pointer - find the System Management BIOS Table in mem region.
 * @begin: begin pointer for region to be scanned.
 * @end: end pointer for region to be scanned.
 *
 * Returns pointer to the head of the SMBIOS tables (or %NULL).
 */
/* [한국어]
 * detect_SMBIOS_pointer - ROM 구간을 훑어 SMBIOS 진입점을 찾는다
 *
 * @begin: 훑기 시작할 주소(ioremap 된 ROM 의 처음).
 * @end: 훑기를 끝낼 주소.
 * @return: "_SM_" 서명을 찾은 자리, 못 찾으면 NULL.
 *
 * **펌웨어가 남긴 표를 찾는 가장 원시적인 방법이다** -- 물리 주소 0xF0000 부터
 * 64KB 를 16바이트 간격으로 걸으며 네 글자 서명을 찾는다.
 * 16바이트 간격인 것은 SMBIOS 규격이 진입점을 16바이트 경계에 두도록 정하기
 * 때문이며, 그 덕에 네 번에 한 번만 읽으면 되는 대신 경계를 벗어난 표는
 * 찾지 못한다.
 *
 * 한 자리에서 네 바이트를 따로 readb 하는 것은 __iomem 영역이라 문자열
 * 비교 함수를 쓸 수 없기 때문이다. 네 글자가 모두 맞으면 status 를 세우고
 * 빠져나오며, 끝까지 못 찾으면 fp 를 NULL 로 만들어 돌려준다.
 *
 * **찾은 것이 진짜 표인지 체크섬으로 확인하지 않는다.** 서명 네 글자만 믿는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(모듈 초기화). 잠들지 않는다.
 *
 * 호출 체인:
 *   one_time_init → [이 함수] → readb
 */
static void __iomem *detect_SMBIOS_pointer(void __iomem *begin, void __iomem *end)
{
	/* [한국어] 지금 살펴보는 자리를 가리키는 이동 포인터 */
	void __iomem *fp;
	/* [한국어] 살펴봐도 되는 마지막 자리. 아래에서 계산한다 */
	void __iomem *endp;
	/* [한국어] 서명 네 글자를 한 바이트씩 받아 둘 자리.
	 * **__iomem 영역이라 문자열 함수를 쓸 수 없어** 네 번 따로 읽는다 */
	u8 temp1, temp2, temp3, temp4;
	/* [한국어] 서명을 찾았는지 표시. 0 이면 못 찾은 것이다 */
	int status = 0;

	/* [한국어] **네 바이트를 읽어야 하므로 끝에서 세 바이트를 물러난 자리가 한계다.**
	 * 이 계산이 없으면 마지막 항목을 읽다가 매핑 밖으로 나간다 */
	endp = (end - sizeof(u32) + 1);

	/* [한국어] **16바이트씩 건너뛰며 훑는다.**
	 * SMBIOS 규격이 진입점을 16바이트 경계에 두도록 정하므로 그 사이는 볼
	 * 필요가 없다. 덕분에 탐색이 16배 빠르지만 경계를 벗어난 표는 놓친다 */
	for (fp = begin; fp <= endp; fp += 16) {
		/* [한국어] 서명 첫 글자 후보 */
		temp1 = readb(fp);
		/* [한국어] 둘째 글자 후보 */
		temp2 = readb(fp+1);
		/* [한국어] 셋째 글자 후보 */
		temp3 = readb(fp+2);
		/* [한국어] 넷째 글자 후보 */
		temp4 = readb(fp+3);
		/* [한국어] **"_SM_" 네 글자가 SMBIOS 진입점의 서명이다.**
		 * 네 조건을 모두 만족해야 한다.
		 * **체크섬은 확인하지 않는다** -- 서명만 보고 믿는다 */
		if (temp1 == '_' &&
		    temp2 == 'S' &&
		    temp3 == 'M' &&
		    temp4 == '_') {
			/* [한국어] 찾았음을 표시한다 */
			status = 1;
			/* [한국어] 찾았으니 더 훑을 필요가 없다 */
			break;
		}
	}

	/* [한국어] 끝까지 훑었는데 못 찾은 경우 */
	if (!status)
		/* [한국어] **못 찾았음을 NULL 로 알린다.**
		 * 이때 fp 는 endp 를 넘어선 값이므로 그대로 두면 잘못된 주소가 된다 */
		fp = NULL;

	/* [한국어] 찾은 자리를 찍는다. 못 찾았으면 (null) 로 나온다 */
	dbg("Discovered SMBIOS Entry point at %p\n", fp);

	/* [한국어] 호출자 one_time_init 이 NULL 이면 초기화를 접는다 */
	return fp;
}

/**
 * init_SERR - Initializes the per slot SERR generation.
 * @ctrl: controller to use
 *
 * For unexpected switch opens
 */
/* [한국어]
 * init_SERR - 슬롯마다 SERR 발생을 켠다
 *
 * @ctrl: 대상 컨트롤러.
 * @return: 성공 0, ctrl 이 NULL 이면 1.
 *
 * **SERR 은 PCI 버스의 시스템 오류 신호다.** 레버가 예고 없이 열리거나
 * 빈 슬롯이 버스를 흔들면 이 신호로 알려야 시스템이 조용히 망가지지 않는다.
 * 원문 주석이 밝히는 "For unexpected switch opens" 가 그 뜻이다.
 *
 * SLOT_MASK 레지스터의 하위 니블에서 슬롯 개수를 얻어 그 횟수만큼 돌며
 * SLOT_SERR 에 0 을 쓴다.
 *
 * **눈여겨볼 점이 둘 있다.** 첫째, 반복 안에서 tempdword 를 하나씩 늘리지만
 * 그 값을 쓰지 않는다 -- 슬롯 번호를 따라가려던 흔적으로 보이나 쓰이는 곳이
 * 없다. 둘째, 매번 같은 주소에 같은 값(0)을 쓴다 -- 슬롯마다 다른 자리를
 * 건드리는 것이 아니라 같은 레지스터에 여러 번 쓴다. 코드는 손대지 않고
 * 사실만 적는다.
 *
 * 호출자는 crit_sect 를 쥔 채 부른다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe 경로).
 *
 * 호출 체인:
 *   cpqhpc_probe → [이 함수] → readb, writeb
 */
static int init_SERR(struct controller *ctrl)
{
	/* [한국어] 슬롯 번호를 따라가려던 변수.
	 * **아래에서 늘어나기만 하고 읽히지 않는다** */
	u32 tempdword;
	/* [한국어] 남은 슬롯 수를 세는 반복 변수 */
	u32 number_of_slots;

	/* [한국어] 방어적 검사. **호출자 cpqhpc_probe 는 늘 유효한 포인터를 넘긴다** */
	if (!ctrl)
		/* [한국어] NULL 이면 실패를 알린다. 0 이 성공이므로 1 이 실패다 */
		return 1;

	/* [한국어] 첫 슬롯의 물리 번호에서 시작한다.
	 * **앞서 적었듯 이 값은 이후 쓰이지 않는다** */
	tempdword = ctrl->first_slot;

	/* [한국어] **SLOT_MASK 의 하위 니블이 슬롯 개수다.**
	 * 상위 니블은 첫 슬롯의 PCI 장치 번호이며 여기서는 쓰지 않는다.
	 * 0x0F 로 하위 네 비트만 남기므로 최대 15슬롯까지 셀 수 있다 */
	number_of_slots = readb(ctrl->hpc_reg + SLOT_MASK) & 0x0F;
	/* Loop through slots */
	/* [한국어] 슬롯 수만큼 반복한다. 원문 주석의 Loop through slots 다 */
	while (number_of_slots) {
		/* [한국어] **SLOT_SERR 에 0 을 써서 SERR 보고를 켠다.**
		 * 반복 안에 있으나 슬롯마다 다른 자리를 건드리는 것이 아니라
		 * **매번 같은 주소에 같은 값을 쓴다.** 코드는 손대지 않고 사실만 적는다 */
		writeb(0, ctrl->hpc_reg + SLOT_SERR);
		/* [한국어] 슬롯 번호를 하나 늘린다. 쓰이지 않는 값이다 */
		tempdword++;
		/* [한국어] 남은 슬롯 수를 줄인다. 이 줄이 반복을 끝낸다 */
		number_of_slots--;
	}

	/* [한국어] 성공. 호출자가 0 이 아니면 실패로 보고 되돌린다 */
	return 0;
}

/* [한국어]
 * init_cpqhp_routing_table - BIOS 의 $PIR 인터럽트 라우팅 표를 받아 둔다
 *
 * @return: 성공 0, 표를 못 받으면 -ENOMEM, 표가 비었으면 -1.
 *
 * **이 드라이버가 슬롯의 물리 번호를 알아내는 유일한 근거가 이 표다.**
 * $PIR 은 BIOS 가 메모리에 남기는 PCI IRQ Routing Table 로, 어느 버스·장치가
 * 어느 슬롯에 꽂혀 있고 그 인터럽트 핀이 어느 선에 이어지는지를 담는다.
 *
 * pcibios_get_irq_routing_table 로 사본을 받아 전역 cpqhp_routing_table 에
 * 둔다. **그 함수는 x86 고유이며 정의가 arch/x86 에 있어 이 스파스 체크아웃에
 * 없다** -- 이름과 쓰임으로만 설명한다. 이 드라이버가 x86 전용인 이유 가운데
 * 하나가 이 의존이다.
 *
 * 받은 표의 항목 수를 cpqhp_routing_table_length(cpqphp.h)로 세어 0 이면
 * 쓸모가 없으므로 해제하고 전역을 NULL 로 되돌린 뒤 실패를 알린다.
 * **전역을 NULL 로 되돌리는 것이 중요하다** -- 그러지 않으면 해제된 메모리를
 * 가리키는 포인터가 남는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(모듈 초기화). kmalloc 이 있어 잠들 수 있다.
 *
 * 호출 체인:
 *   cpqhpc_probe → one_time_init → [이 함수] → pcibios_get_irq_routing_table
 */
static int init_cpqhp_routing_table(void)
{
	/* [한국어] 표에 든 항목 수를 담을 자리 */
	int len;

	/* [한국어] **BIOS 의 $PIR 표 사본을 받아 전역에 둔다.**
	 * 이 함수는 x86 고유이며 정의가 arch/x86 에 있어 이 스파스 체크아웃에
	 * 없다. 안에서 kmalloc 으로 사본을 만들어 돌려주므로 **해제 책임이
	 * 호출자에게 있다** */
	cpqhp_routing_table = pcibios_get_irq_routing_table();
	/* [한국어] BIOS 가 표를 주지 않았거나 메모리가 모자란 경우 */
	if (cpqhp_routing_table == NULL)
		/* [한국어] 실패를 알린다. **원인이 둘인데 오류 코드는 하나다** */
		return -ENOMEM;

	/* [한국어] 항목 수를 센다. cpqphp.h 의 인라인 함수이며
	 * 표 전체 크기에서 헤더를 빼고 항목 크기로 나눈다 */
	len = cpqhp_routing_table_length();
	/* [한국어] **표는 있는데 항목이 하나도 없는 경우.**
	 * 슬롯 번호를 알아낼 수 없으므로 이 드라이버는 동작할 수 없다 */
	if (len == 0) {
		/* [한국어] 받은 사본을 놓는다. 해제 책임이 여기 있기 때문이다 */
		kfree(cpqhp_routing_table);
		/* [한국어] **전역을 반드시 NULL 로 되돌린다.**
		 * 그러지 않으면 해제된 메모리를 가리키는 포인터가 남아,
		 * cpqhp_routing_table_length 의 BUG_ON 도 통과해 버린다 */
		cpqhp_routing_table = NULL;
		/* [한국어] 실패를 알린다. **위와 달리 errno 가 아닌 -1 이다** --
		 * 호출자 one_time_init 은 0 이 아니면 실패로만 보므로 구별하지 않는다 */
		return -1;
	}

	/* [한국어] 성공 */
	return 0;
}

/* nice debugging output */
/* [한국어]
 * pci_print_IRQ_route - $PIR 표 전체를 디버그 로그로 찍는다
 *
 * @return: 없음.
 *
 * **디버깅 전용이며 cpqhp_debug 가 켜졌을 때만 불린다.**
 * one_time_init 이 `if (cpqhp_debug)` 로 감싸 부른다.
 * 원문 주석의 "nice debugging output" 이 그 성격을 말한다.
 *
 * 표의 항목 수만큼 돌며 버스 번호, 장치 번호, 함수 번호, 슬롯 번호를 찍는다.
 * devfn 한 바이트에서 `>> 3` 으로 장치 번호를, `& 0x7` 로 함수 번호를 꺼내는
 * 것은 PCI 규격이 정한 배치다(상위 5비트 장치, 하위 3비트 함수).
 *
 * **이 함수가 존재한다는 것 자체가 당시 사정을 말해 준다** -- BIOS 마다 표
 * 내용이 달라 슬롯 번호가 어긋나는 일이 잦았고, 그때 사람이 눈으로 확인할
 * 수단이 필요했다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(모듈 초기화).
 *
 * 호출 체인:
 *   one_time_init → [이 함수] → cpqhp_routing_table_length, dbg
 */
static void pci_print_IRQ_route(void)
{
	/* [한국어] 항목 수 */
	int len;
	/* [한국어] 반복 색인 */
	int loop;
	/* [한국어] 항목에서 꺼낸 버스·devfn·슬롯 번호를 담는 임시 변수 셋 */
	u8 tbus, tdevice, tslot;

	/* [한국어] 항목 수를 센다 */
	len = cpqhp_routing_table_length();

	/* [한국어] 표의 머리글을 찍는다. 아래 줄들과 열을 맞추기 위한 것이다 */
	dbg("bus dev func slot\n");
	/* [한국어] 항목을 하나씩 훑는다 */
	for (loop = 0; loop < len; ++loop) {
		/* [한국어] 이 항목의 버스 번호.
		 * **slots 는 구조체 끝에 붙은 가변 길이 배열이다** */
		tbus = cpqhp_routing_table->slots[loop].bus;
		/* [한국어] 장치·함수 번호가 합쳐진 한 바이트 */
		tdevice = cpqhp_routing_table->slots[loop].devfn;
		/* [한국어] **이 항목이 알려 주는 물리 슬롯 번호.**
		 * 0 이면 온보드 장치이고, 0 이 아니면 꽂을 수 있는 슬롯이다 */
		tslot = cpqhp_routing_table->slots[loop].slot;
		/* [한국어] **devfn 을 장치와 함수로 쪼개 찍는다.**
		 * 상위 5비트가 장치 번호, 하위 3비트가 함수 번호라는 PCI 규격의 배치를
		 * `>> 3` 과 `& 0x7` 로 푼다 */
		dbg("%d %d %d %d\n", tbus, tdevice >> 3, tdevice & 0x7, tslot);

	}
}


/**
 * get_subsequent_smbios_entry: get the next entry from bios table.
 * @smbios_start: where to start in the SMBIOS table
 * @smbios_table: location of the SMBIOS table
 * @curr: %NULL or pointer to previously returned structure
 *
 * Gets the first entry if previous == NULL;
 * otherwise, returns the next entry.
 * Uses global SMBIOS Table pointer.
 *
 * Returns a pointer to an SMBIOS structure or NULL if none found.
 */
/* [한국어]
 * get_subsequent_smbios_entry - SMBIOS 표에서 다음 항목의 시작을 찾는다
 *
 * @smbios_start: 표 본문의 시작(ioremap 된 주소).
 * @smbios_table: 진입점 구조체의 주소. 표 길이를 여기서 읽는다.
 * @curr: 지금 항목의 시작. NULL 이면 NULL 을 돌려준다.
 * @return: 다음 항목의 시작, 표 끝을 넘었으면 NULL.
 *
 * **SMBIOS 항목 하나는 고정 길이 부분과 그 뒤에 붙는 문자열 뭉치로 이루어진다.**
 * 고정 부분의 길이는 항목의 length 필드가 알려 주지만, 문자열 뭉치의 길이는
 * 알 수 없다. 그래서 이 함수는 두 단계로 걷는다.
 *
 * 먼저 curr 에 항목의 length 를 더해 문자열 뭉치의 시작으로 간다.
 * 그다음 바이트를 하나씩 읽으며 **NULL 두 개가 잇달아 나오는 자리** 를 찾는다.
 * 문자열 하나가 NULL 로 끝나고 뭉치 전체가 다시 NULL 로 끝나므로,
 * 연속된 두 NULL 이 곧 항목의 끝이다. previous_byte 를 1 로 시작하는 것은
 * 첫 바이트가 NULL 이어도 곧바로 종료로 오해하지 않게 하려는 것이다.
 *
 * p_max 를 넘어서면 NULL 을 돌려준다. 표 밖을 읽지 않기 위한 울타리다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe 경로).
 *
 * 호출 체인:
 *   get_SMBIOS_entry → [이 함수] → readb, readw
 */
static void __iomem *get_subsequent_smbios_entry(void __iomem *smbios_start,
						void __iomem *smbios_table,
						void __iomem *curr)
{
	/* [한국어] 이중 NULL 종결자를 만났음을 알리는 표시 */
	u8 bail = 0;
	/* [한국어] **바로 앞 바이트를 기억해 둔다.**
	 * 1 로 시작하는 것은 첫 바이트가 0 이어도 곧바로 종료로 오해하지 않게
	 * 하려는 것이다. 문자열 뭉치는 최소한 종결자 하나로 시작한다 */
	u8 previous_byte = 1;
	/* [한국어] 지금 살펴보는 자리 */
	void __iomem *p_temp;
	/* [한국어] 표의 끝. 여기를 넘으면 멈춘다 */
	void __iomem *p_max;

	/* [한국어] 표가 없거나 시작점이 없으면 나아갈 수 없다 */
	if (!smbios_table || !curr)
		/* [한국어] 호출자 get_SMBIOS_entry 가 NULL 을 반복 종료로 읽는다 */
		return NULL;

	/* set p_max to the end of the table */
	/* [한국어] **진입점의 표 길이 필드를 읽어 끝 주소를 구한다.**
	 * ST_LENGTH 는 cpqphp.h 의 offsetof 상수이며 u16 이라 readw 를 쓴다.
	 * 원문 주석이 set p_max to the end of the table 이라 밝힌다 */
	p_max = smbios_start + readw(smbios_table + ST_LENGTH);

	/* [한국어] 지금 항목의 시작에서 출발한다 */
	p_temp = curr;
	/* [한국어] **항목의 length 필드만큼 건너뛰어 문자열 뭉치의 시작으로 간다.**
	 * 고정 부분의 길이는 이 필드가 알려 주지만 문자열 뭉치의 길이는
	 * 알 수 없어서, 여기서부터는 한 바이트씩 걸어야 한다 */
	p_temp += readb(curr + SMBIOS_GENERIC_LENGTH);

	/* [한국어] 표 끝에 닿거나 이중 NULL 을 만날 때까지 한 바이트씩 나아간다 */
	while ((p_temp < p_max) && !bail) {
		/* Look for the double NULL terminator
		 * The first condition is the previous byte
		 * and the second is the curr
		 */
		/* [한국어] **앞 바이트도 0 이고 지금 바이트도 0 이면 항목의 끝이다.**
		 * 문자열 하나가 0 으로 끝나고 뭉치 전체가 다시 0 으로 끝나기 때문이다.
		 * 원문 주석 세 줄이 이 조건의 두 항을 그대로 설명한다 */
		if (!previous_byte && !(readb(p_temp)))
			/* [한국어] 종료 표시를 세운다. **break 를 쓰지 않고 표시로 반복을 끝낸다** --
			 * 그래서 아래 두 줄이 한 번 더 실행되고 p_temp 가 한 칸 더 나아간다.
			 * 그 결과 p_temp 는 두 번째 0 의 **다음** 자리, 곧 다음 항목의 시작을
			 * 가리키게 된다. 의도한 결과다 */
			bail = 1;

		/* [한국어] 지금 바이트를 다음 판을 위해 기억해 둔다 */
		previous_byte = readb(p_temp);
		/* [한국어] 한 바이트 나아간다 */
		p_temp++;
	}

	/* [한국어] 아직 표 안이면 다음 항목이 있다는 뜻이다 */
	if (p_temp < p_max)
		/* [한국어] 다음 항목의 시작을 돌려준다 */
		return p_temp;
	/* [한국어] 표 끝에 닿은 경우 */
	else
		/* [한국어] 더 볼 항목이 없음을 알린다 */
		return NULL;
}


/**
 * get_SMBIOS_entry - return the requested SMBIOS entry or %NULL
 * @smbios_start: where to start in the SMBIOS table
 * @smbios_table: location of the SMBIOS table
 * @type: SMBIOS structure type to be returned
 * @previous: %NULL or pointer to previously returned structure
 *
 * Gets the first entry of the specified type if previous == %NULL;
 * Otherwise, returns the next entry of the given type.
 * Uses global SMBIOS Table pointer.
 * Uses get_subsequent_smbios_entry.
 *
 * Returns a pointer to an SMBIOS structure or %NULL if none found.
 */
/* [한국어]
 * get_SMBIOS_entry - 원하는 종류의 SMBIOS 항목을 찾아 돌려준다
 *
 * @smbios_start: 표 본문의 시작.
 * @smbios_table: 진입점 구조체의 주소.
 * @type: 찾고 싶은 항목 종류. 이 드라이버는 9(System Slot)만 쓴다.
 * @previous: NULL 이면 처음부터, 아니면 그 항목의 다음부터 찾는다.
 * @return: 찾은 항목의 주소, 없으면 NULL.
 *
 * **반복자(iterator) 형태의 함수다.** 같은 종류의 항목이 여럿 있을 수 있으므로
 * (슬롯마다 하나씩), 호출자가 앞서 받은 것을 다시 넘겨 그다음을 받는다.
 * ctrl_slot_setup 이 그렇게 슬롯 번호가 맞는 항목을 만날 때까지 반복한다.
 *
 * previous 가 NULL 이면 표의 맨 앞을 시작점으로 삼고, 아니면
 * get_subsequent_smbios_entry 로 한 칸 나아간다. 그다음 종류가 맞을 때까지
 * 계속 나아간다. **종류를 읽을 때 SMBIOS_GENERIC_TYPE 오프셋을 쓴다** --
 * 항목의 실제 종류를 아직 모르는 채 머리 세 필드만 읽는 것이며,
 * 그 셋의 자리는 종류와 무관하게 같다는 규격의 약속에 기댄다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe 경로).
 *
 * 호출 체인:
 *   ctrl_slot_setup → [이 함수] → get_subsequent_smbios_entry, readb
 */
static void __iomem *get_SMBIOS_entry(void __iomem *smbios_start,
					void __iomem *smbios_table,
					u8 type,
					void __iomem *previous)
{
	/* [한국어] 표가 없으면 찾을 수 없다 */
	if (!smbios_table)
		/* [한국어] 호출자가 NULL 을 못 찾음으로 읽는다 */
		return NULL;

	/* [한국어] 처음 찾는 경우 */
	if (!previous)
		/* [한국어] **표의 맨 앞을 첫 후보로 삼는다.**
		 * 아래 반복이 곧바로 종류를 확인하므로 첫 항목도 걸러진다 */
		previous = smbios_start;
	/* [한국어] 이미 받아 간 항목이 있는 경우 */
	else
		/* [한국어] **한 칸 나아간다.** 같은 종류의 다음 항목을 찾으려면
		 * 지금 것을 지나쳐야 하기 때문이다 */
		previous = get_subsequent_smbios_entry(smbios_start,
					smbios_table, previous);

	/* [한국어] 후보가 있는 동안 반복한다. NULL 이 되면 표를 다 본 것이다 */
	while (previous)
		/* [한국어] **항목의 종류 필드를 읽어 찾는 종류와 견준다.**
		 * SMBIOS_GENERIC_TYPE 은 종류와 무관하게 같은 자리에 있는 공통 머리의
		 * 오프셋이라, 항목이 무엇인지 모르는 채로도 읽을 수 있다.
		 * 이 드라이버가 찾는 종류는 9(System Slot) 하나뿐이다 */
		if (readb(previous + SMBIOS_GENERIC_TYPE) != type)
			/* [한국어] 종류가 다르면 다음 항목으로 나아간다 */
			previous = get_subsequent_smbios_entry(smbios_start,
						smbios_table, previous);
		/* [한국어] 종류가 맞은 경우 */
		else
			/* [한국어] 찾았으니 반복을 끝낸다 */
			break;

	/* [한국어] 찾은 항목, 또는 표를 다 봤으면 NULL */
	return previous;
}

/* [한국어]
 * ctrl_slot_cleanup - 컨트롤러의 슬롯과 하드웨어 자원을 모두 되돌린다
 *
 * @ctrl: 정리할 컨트롤러.
 * @return: 늘 0. 호출자도 이 값을 보지 않는다.
 *
 * **cpqhpc_probe 가 세운 것을 역순으로 허문다.** 슬롯 리스트를 훑으며
 * 핫플러그 코어에서 등록을 해제하고 struct slot 을 해제한 뒤, debugfs 파일,
 * 인터럽트, MMIO 매핑, 예약해 둔 메모리 영역을 차례로 놓는다.
 *
 * **이 함수에 닿는 길은 하나뿐이다** -- cpqhpc_driver 의 .remove 항목이 통째로
 * 주석 처리되어 있어(cpqhpc_remove_one 을 가리키던 줄) 컨트롤러를 뽑는 경로가
 * 없다. 그래서 모듈을 내리는 unload_cpqphpd 만이 이 함수를 부른다.
 *
 * 리스트를 걷기 전에 ctrl->slot 을 NULL 로 먼저 밀어 두는 것이 눈에 띈다 --
 * 걷는 도중 다른 곳이 리스트를 보지 못하게 하려는 뜻으로 보이나, 락이 없어
 * 실제로 경쟁을 막지는 못한다. 모듈 내리기와 겹칠 일이 없다고 본 것이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(모듈 내리기). free_irq 는 잠들 수 있다.
 *
 * 호출 체인:
 *   cpqhpc_cleanup → unload_cpqphpd → [이 함수]
 *     → pci_hp_deregister, cpqhp_remove_debugfs_files, free_irq, iounmap,
 *       release_mem_region
 */
static int ctrl_slot_cleanup(struct controller *ctrl)
{
	/* [한국어] 리스트를 걸으며 지금 노드와 다음 노드를 잡아 둘 자리.
	 * **해제하기 전에 next 를 먼저 빼 두어야** 해제된 메모리를 읽지 않는다 */
	struct slot *old_slot, *next_slot;

	/* [한국어] 슬롯 리스트의 머리에서 시작한다 */
	old_slot = ctrl->slot;
	/* [한국어] **리스트를 컨트롤러에서 먼저 떼어 낸다.**
	 * 걷는 도중 다른 곳이 보지 못하게 하려는 뜻으로 보이나 락이 없어
	 * 실제로 경쟁을 막지는 못한다 */
	ctrl->slot = NULL;

	/* [한국어] 리스트 끝까지 걷는다 */
	while (old_slot) {
		/* [한국어] **해제하기 전에 다음 노드를 먼저 챙긴다** */
		next_slot = old_slot->next;
		/* [한국어] **핫플러그 코어에서 이 슬롯의 등록을 해제한다.**
		 * sysfs 디렉터리가 사라지고 여덟 콜백이 닫힌다.
		 * 안쪽 hotplug_slot 구조체의 주소를 넘기는 것은 등록할 때와 같은 방식이다 */
		pci_hp_deregister(&old_slot->hotplug_slot);
		/* [한국어] 슬롯 구조체를 해제한다.
		 * **p_sm_slot 은 ROM 매핑 안을 가리키는 주소라 따로 놓을 것이 없다** */
		kfree(old_slot);
		/* [한국어] 다음 노드로 넘어간다 */
		old_slot = next_slot;
	}

	/* [한국어] 이 컨트롤러의 debugfs 파일을 지운다(cpqphp_sysfs.c) */
	cpqhp_remove_debugfs_files(ctrl);

	/* Free IRQ associated with hot plug device */
	/* [한국어] **인터럽트 처리기를 뗀다.**
	 * IRQF_SHARED 로 걸었으므로 두 번째 인자(ctrl)가 어느 처리기인지
	 * 가려내는 열쇠가 된다. 원문 주석이 밝히는 대로다.
	 * **이 함수는 진행 중인 처리기가 끝날 때까지 기다리므로 잠들 수 있다** */
	free_irq(ctrl->interrupt, ctrl);
	/* Unmap the memory */
	/* [한국어] 컨트롤러 MMIO 매핑을 놓는다. 원문 주석: Unmap the memory */
	iounmap(ctrl->hpc_reg);
	/* Finally reclaim PCI mem */
	/* [한국어] **예약해 둔 MMIO 영역을 커널에 돌려준다.**
	 * probe 의 request_mem_region 과 짝이며, 시작 주소와 길이를 다시 계산해
	 * 넘긴다. 원문 주석: Finally reclaim PCI mem */
	release_mem_region(pci_resource_start(ctrl->pci_dev, 0),
			   pci_resource_len(ctrl->pci_dev, 0));

	/* [한국어] 늘 성공을 알린다. **호출자 unload_cpqphpd 는 이 값을 보지 않는다** */
	return 0;
}


/**
 * get_slot_mapping - determine logical slot mapping for PCI device
 *
 * Won't work for more than one PCI-PCI bridge in a slot.
 *
 * @bus: pointer to the PCI bus structure
 * @bus_num: bus number of PCI device
 * @dev_num: device number of PCI device
 * @slot: Pointer to u8 where slot number will	be returned
 *
 * Output:	SUCCESS or FAILURE
 */
/* [한국어]
 * get_slot_mapping - PCI 장치 번호에 대응하는 물리 슬롯 번호를 찾는다
 *
 * @bus: 설정공간 접근에 쓸 pci_bus. **번호를 갈아 끼우는 사본이어야 한다.**
 * @bus_num: 찾는 장치의 버스 번호.
 * @dev_num: 찾는 장치의 장치 번호.
 * @slot: 찾은 물리 슬롯 번호를 담아 돌려줄 자리.
 * @return: 찾았으면 0, 못 찾았으면 -1.
 *
 * **사람이 보는 슬롯 번호와 PCI 주소를 잇는 함수다.** 섀시에 인쇄된 번호는
 * 하드웨어 어디에도 적혀 있지 않고 BIOS 의 $PIR 표에만 있으므로, 그 표를
 * 처음부터 끝까지 훑어 버스·장치 번호가 맞는 항목을 찾는다.
 *
 * 찾지 못했을 때가 이 함수의 요점이다. 표를 훑는 동안 **PCI-to-PCI 브리지를
 * 만나면 그 브리지의 secondary 버스 번호가 찾는 버스와 같은지 확인해 두고**,
 * 같으면 브리지의 슬롯 번호를 bridgeSlot 에 기억한다. 끝까지 직접 항목을
 * 찾지 못하면 "찾는 장치는 그 브리지 뒤에 있다" 고 보고 브리지의 슬롯 번호를
 * 돌려준다. 브리지 카드에 다시 카드가 꽂힌 구조를 다루기 위한 대비다.
 * 원문 주석이 밝히듯 **슬롯 하나에 브리지가 둘 이상이면 동작하지 않는다.**
 *
 * 설정공간을 읽을 때 `bus->number = tbus` 로 사본의 버스 번호를 갈아 끼운다.
 * 진짜 버스 구조체였다면 커널 전체가 깨지는 관용구이며, 그래서 cpqhpc_probe 가
 * kmemdup 으로 사본을 만들어 둔다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe 경로).
 *
 * 호출 체인:
 *   cpqhpc_probe → [이 함수]
 *     → cpqhp_routing_table_length, pci_bus_read_config_dword
 */
static int
get_slot_mapping(struct pci_bus *bus, u8 bus_num, u8 dev_num, u8 *slot)
{
	/* [한국어] 설정공간에서 읽은 dword 를 받아 둘 자리.
	 * 클래스 코드와 버스 번호 레지스터를 번갈아 담는다 */
	u32 work;
	/* [한국어] $PIR 표의 항목 수 */
	long len;
	/* [한국어] 반복 색인 */
	long loop;

	/* [한국어] 항목에서 꺼낸 버스·장치·슬롯 번호와,
	 * **브리지를 만났을 때 기억해 둘 슬롯 번호** */
	u8 tbus, tdevice, tslot, bridgeSlot;

	/* [한국어] 들어온 인자를 그대로 찍는다.
	 * 슬롯 번호가 어긋나는 일이 잦았던 자리라 흔적이 남아 있다 */
	dbg("%s: %p, %d, %d, %p\n", __func__, bus, bus_num, dev_num, slot);

	/* [한국어] **아직 브리지를 못 찾았음을 뜻하는 표시값.**
	 * 슬롯 번호로 쓰이지 않는 값을 골라 "없음" 을 나타낸다 */
	bridgeSlot = 0xFF;

	/* [한국어] 표의 항목 수를 센다 */
	len = cpqhp_routing_table_length();
	/* [한국어] 항목을 하나씩 훑는다. **끝까지 훑는다** --
	 * 찾더라도 브리지 정보를 모으기 위해 계속 도는 것이 아니라,
	 * 찾으면 곧바로 돌려주므로 실제로는 끝까지 도는 경우가 못 찾는 경우다 */
	for (loop = 0; loop < len; ++loop) {
		/* [한국어] 이 항목의 버스 번호 */
		tbus = cpqhp_routing_table->slots[loop].bus;
		/* [한국어] **devfn 에서 장치 번호만 꺼낸다.**
		 * 함수 번호는 슬롯을 가리는 데 쓸모가 없다 -- 한 슬롯의 여러 함수는
		 * 같은 슬롯에 있기 때문이다 */
		tdevice = cpqhp_routing_table->slots[loop].devfn >> 3;
		/* [한국어] 이 항목이 알려 주는 물리 슬롯 번호 */
		tslot = cpqhp_routing_table->slots[loop].slot;

		/* [한국어] **찾는 장치와 정확히 맞는 항목을 만난 경우** */
		if ((tbus == bus_num) && (tdevice == dev_num)) {
			/* [한국어] 물리 슬롯 번호를 호출자에게 담아 준다 */
			*slot = tslot;
			/* [한국어] 성공. 브리지 검사를 더 할 필요가 없다 */
			return 0;
		/* [한국어] 맞지 않은 경우 -- 이 항목이 브리지인지 확인해 둔다.
		 * 원문 주석 여덟 줄이 그 이유를 자세히 밝힌다 */
		} else {
			/* Did not get a match on the target PCI device. Check
			 * if the current IRQ table entry is a PCI-to-PCI
			 * bridge device.  If so, and its secondary bus
			 * matches the bus number for the target device, I need
			 * to save the bridge's slot number.  If I can not find
			 * an entry for the target device, I will have to
			 * assume it's on the other side of the bridge, and
			 * assign it the bridge's slot.
			 */
			/* [한국어] **설정공간을 읽을 버스 번호를 사본에 갈아 끼운다.**
			 * 이것이 cpqhpc_probe 가 kmemdup 으로 pci_bus 사본을 만들어 둔 이유다.
			 * 진짜 버스 구조체였다면 커널 전체가 깨진다 */
			bus->number = tbus;
			/* [한국어] **클래스 코드와 리비전을 한 dword 로 읽는다.**
			 * PCI_CLASS_REVISION 은 설정공간 오프셋 0x08 이며,
			 * 하위 한 바이트가 리비전, 상위 세 바이트가 클래스 코드다.
			 * 함수 번호로 0 을 쓰는 것은 다기능 카드라도 브리지 여부는 함수 0 이
			 * 대표하기 때문이다 */
			pci_bus_read_config_dword(bus, PCI_DEVFN(tdevice, 0),
						PCI_CLASS_REVISION, &work);

			/* [한국어] **리비전 한 바이트를 밀어내면 클래스 코드만 남는다.**
			 * 0x060400 이면 PCI-to-PCI 브리지다 */
			if ((work >> 8) == PCI_TO_PCI_BRIDGE_CLASS) {
				/* [한국어] **브리지의 버스 번호 레지스터를 읽는다.**
				 * PCI_PRIMARY_BUS 는 오프셋 0x18 이며, 네 바이트가 차례로
				 * primary, secondary, subordinate, secondary latency timer 다 */
				pci_bus_read_config_dword(bus,
							PCI_DEVFN(tdevice, 0),
							PCI_PRIMARY_BUS, &work);
				// See if bridge's secondary bus matches target bus.
				/* [한국어] **둘째 바이트인 secondary 버스 번호를 꺼내 찾는 버스와 견준다.**
				 * 같으면 찾는 장치가 이 브리지 뒤에 있다는 뜻이다.
				 * 원문 주석이 See if bridge's secondary bus matches target bus 라 밝힌다 */
				if (((work >> 8) & 0x000000FF) == (long) bus_num)
					/* [한국어] **그 브리지의 슬롯 번호를 기억해 둔다.**
					 * 직접 항목을 못 찾았을 때 이 값을 답으로 쓴다 */
					bridgeSlot = tslot;
			}
		}

	}

	/* If we got here, we didn't find an entry in the IRQ mapping table for
	 * the target PCI device.  If we did determine that the target device
	 * is on the other side of a PCI-to-PCI bridge, return the slot number
	 * for the bridge.
	 */
	/* [한국어] **표에서 직접 못 찾았지만 브리지는 찾은 경우.**
	 * 원문 주석 네 줄이 이 판단을 설명한다 */
	if (bridgeSlot != 0xFF) {
		/* [한국어] 브리지가 꽂힌 슬롯 번호를 답으로 준다.
		 * 브리지 카드 위의 장치는 결국 그 슬롯에 있는 것이기 때문이다 */
		*slot = bridgeSlot;
		/* [한국어] 성공으로 알린다 */
		return 0;
	}
	/* Couldn't find an entry in the routing table for this PCI device */
	/* [한국어] **표에도 없고 브리지도 못 찾은 경우.**
	 * 호출자 cpqhpc_probe 는 이때 초기화를 접는다 -- 슬롯 번호를 모르면
	 * 슬롯을 등록할 수 없기 때문이다 */
	return -1;
}


/**
 * cpqhp_set_attention_status - Turns the Amber LED for a slot on or off
 * @ctrl: struct controller to use
 * @func: PCI device/function info
 * @status: LED control flag: 1 = LED on, 0 = LED off
 */
/* [한국어]
 * cpqhp_set_attention_status - 슬롯의 황색 LED 를 켜거나 끈다
 *
 * @ctrl: 대상 컨트롤러.
 * @func: 대상 슬롯의 함수 정보. NULL 이면 실패한다.
 * @status: 1 이면 켜기, 0 이면 끄기. 그 밖의 값이면 실패한다.
 * @return: 성공 0, 실패 1.
 *
 * **이 파일에서 컨트롤러 하드웨어를 직접 만지는 몇 안 되는 함수다.**
 * 황색 LED 는 "이 슬롯을 봐 달라" 는 표시이며, 관리자가 여러 슬롯 가운데
 * 어느 것을 뽑아야 하는지 알려 주는 데 쓴다.
 *
 * 동작은 이 드라이버의 전형적인 하드웨어 조작 절차를 그대로 따른다.
 * crit_sect 를 잡고, LED 비트를 고치고, set_SOGO 로 컨트롤러에 반영을 지시하고,
 * wait_for_ctrl_irq 로 완료를 기다린 뒤 락을 놓는다. **set_SOGO 와
 * wait_for_ctrl_irq 는 반드시 한 락 안에서 짝을 이뤄야 한다** -- 그 사이에
 * 다른 명령이 끼어들면 어느 명령의 완료를 기다리는지 알 수 없어진다.
 *
 * status 가 0 도 1 도 아닌 경우 **락을 놓고 나가는 것을 잊지 않았다.**
 * 초기 커널 코드에서 흔했던 락 누수를 피한 자리다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(sysfs 쓰기). 뮤텍스를 잡으므로 잠들 수 있다.
 *
 * 호출 체인:
 *   set_attention_status → [이 함수]
 *     → mutex_lock, amber_LED_on/off, set_SOGO, wait_for_ctrl_irq, mutex_unlock
 */
static int
cpqhp_set_attention_status(struct controller *ctrl, struct pci_func *func,
				u32 status)
{
	/* [한국어] 컨트롤러 안 슬롯 번호(0부터). 레지스터 비트 자리가 된다 */
	u8 hp_slot;

	/* [한국어] **호출자가 이미 NULL 을 걸렀지만 한 번 더 본다** */
	if (func == NULL)
		/* [한국어] 실패를 알린다 */
		return 1;

	/* [한국어] **PCI 장치 번호를 레지스터 비트 자리로 바꾼다.**
	 * slot_device_offset 은 첫 슬롯의 장치 번호이므로 빼면 0부터의 번호가 된다 */
	hp_slot = func->device - ctrl->slot_device_offset;

	/* Wait for exclusive access to hardware */
	/* [한국어] **컨트롤러 레지스터를 독점한다.**
	 * 아래의 LED 고치기와 set_SOGO 와 완료 대기가 나뉘어서는 안 되기 때문이다.
	 * 원문 주석: Wait for exclusive access to hardware */
	mutex_lock(&ctrl->crit_sect);

	/* [한국어] 켜기 요청 */
	if (status == 1)
		/* [한국어] 황색 LED 비트를 세운다(cpqphp.h 의 인라인 함수).
		 * **아직 하드웨어에 반영되지는 않는다** */
		amber_LED_on(ctrl, hp_slot);
	/* [한국어] 끄기 요청 */
	else if (status == 0)
		/* [한국어] 황색 LED 비트를 지운다 */
		amber_LED_off(ctrl, hp_slot);
	/* [한국어] **0 도 1 도 아닌 값이 온 경우** */
	else {
		/* Done with exclusive hardware access */
		/* [한국어] **락을 놓고 나간다.** 이 줄이 없으면 락이 새어 시스템이 멈춘다 */
		mutex_unlock(&ctrl->crit_sect);
		/* [한국어] 잘못된 인자임을 알린다 */
		return 1;
	}

	/* [한국어] **MISC 레지스터의 명령 개시 비트를 세워 하드웨어에 반영을 지시한다** */
	set_SOGO(ctrl);

	/* Wait for SOBS to be unset */
	/* [한국어] **반영이 끝나기를 기다린다.**
	 * 대기 큐에 올려 두고 최대 1초를 잔다. 원문 주석의 SOBS 는
	 * 컨트롤러의 완료 표시 비트를 가리키는 이름으로 보이나,
	 * **그 문서는 이 트리에 없다.**
	 * 반환값을 확인하지 않는다 -- 신호로 중단되어도 그냥 넘어간다 */
	wait_for_ctrl_irq(ctrl);

	/* Done with exclusive hardware access */
	/* [한국어] 락을 놓는다. 원문 주석: Done with exclusive hardware access */
	mutex_unlock(&ctrl->crit_sect);

	/* [한국어] 성공 */
	return 0;
}


/**
 * set_attention_status - Turns the Amber LED for a slot on or off
 * @hotplug_slot: slot to change LED on
 * @status: LED control flag
 */
/* [한국어]
 * set_attention_status - sysfs 의 attention 쓰기를 받아 LED 를 바꾼다
 *
 * @hotplug_slot: 핫플러그 코어가 넘겨 준 슬롯.
 * @status: 사용자가 쓴 값.
 * @return: 성공 0, 슬롯 정보를 찾지 못하면 -ENODEV.
 *
 * **cpqphp_hotplug_slot_ops 의 여덟 콜백 가운데 하나이며, 사용자 공간이 이
 * 드라이버에 닿는 입구다.** 이 파일의 콜백 여덟 개는 모두 같은 앞머리를 쓴다 --
 * to_slot 으로 이 드라이버의 슬롯을 되찾고, 거기서 컨트롤러를 꺼내고, 슬롯
 * 이름을 로그에 찍는다.
 *
 * 이어서 슬롯 번호로 버스·장치·함수 번호를 얻고(cpqhp_get_bus_dev),
 * 그 셋으로 pci_func 노드를 찾아(cpqhp_slot_find) 실제 동작 함수에 넘긴다.
 * **여기서는 cpqhp_slot_find 의 NULL 을 제대로 검사한다** -- 같은 호출을
 * 검사 없이 역참조하는 자리가 cpqphp_ctrl.c 에 있는 것과 대비된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(사용자가 sysfs 에 write 한 문맥).
 *
 * 호출 체인:
 *   핫플러그 코어(sysfs) → [이 함수]
 *     → to_slot, cpqhp_get_bus_dev, cpqhp_slot_find, cpqhp_set_attention_status
 */
static int set_attention_status(struct hotplug_slot *hotplug_slot, u8 status)
{
	/* [한국어] 이 슬롯의 함수 정보를 담을 자리.
	 * **아래 cpqhp_slot_find 로 찾아 채운 뒤 cpqhp_set_attention_status 에 넘긴다** */
	struct pci_func *slot_func;
	/* [한국어] **핫플러그 코어의 슬롯에서 이 드라이버의 슬롯을 되찾는다.**
	 * cpqphp.h 의 to_slot 이 container_of 로 계산한다.
	 * 여덟 콜백이 모두 이 줄로 시작한다 */
	struct slot *slot = to_slot(hotplug_slot);
	/* [한국어] 슬롯이 붙어 있는 컨트롤러. 실제 하드웨어 조작이 여기를 거친다 */
	struct controller *ctrl = slot->ctrl;
	/* [한국어] 이 슬롯의 버스 번호를 받을 자리 */
	u8 bus;
	/* [한국어] 장치·함수 번호가 합쳐진 한 바이트를 받을 자리 */
	u8 devfn;
	/* [한국어] devfn 에서 꺼낸 장치 번호 */
	u8 device;
	/* [한국어] devfn 에서 꺼낸 함수 번호 */
	u8 function;

	/* [한국어] 어느 슬롯에 대한 요청인지 찍는다.
	 * slot_name 은 핫플러그 코어가 들고 있는 이름 문자열을 꺼내 준다 */
	dbg("%s - physical_slot = %s\n", __func__, slot_name(slot));

	/* [한국어] **물리 슬롯 번호로 버스·devfn 을 얻는다**(cpqphp_pci.c).
	 * 출력이 둘이라 포인터로 받는다 */
	if (cpqhp_get_bus_dev(ctrl, &bus, &devfn, slot->number) == -1)
		/* [한국어] 슬롯 번호에 맞는 장치가 없으면 여기서 접는다 */
		return -ENODEV;

	/* [한국어] **상위 5비트가 장치 번호다**(PCI 규격의 devfn 배치) */
	device = devfn >> 3;
	/* [한국어] **하위 3비트가 함수 번호다** */
	function = devfn & 0x7;
	/* [한국어] 쪼갠 세 값을 찍는다 */
	dbg("bus, dev, fn = %d, %d, %d\n", bus, device, function);

	/* [한국어] **전역 cpqhp_slot_list 에서 이 함수의 노드를 찾는다.**
	 * 그 노드에 저장해 둔 설정공간과 자원 목록이 들어 있다 */
	slot_func = cpqhp_slot_find(bus, device, function);
	/* [한국어] **NULL 을 제대로 검사한다.**
	 * 같은 호출을 검사 없이 역참조하는 자리가 cpqphp_ctrl.c 에 있는 것과
	 * 대비되는 자리다 */
	if (!slot_func)
		/* [한국어] 저장된 정보가 없으면 다룰 수 없다 */
		return -ENODEV;

	/* [한국어] **실제 LED 조작 함수로 넘긴다.**
	 * 그 함수가 crit_sect 를 잡고 LED 비트를 고치고 set_SOGO 로 반영한다 */
	return cpqhp_set_attention_status(ctrl, slot_func, status);
}


/* [한국어]
 * process_SI - sysfs 의 enable_slot 요청을 받아 카드를 올린다
 *
 * @hotplug_slot: 핫플러그 코어가 넘겨 준 슬롯.
 * @return: 성공 0, 슬롯 정보를 찾지 못하면 -ENODEV, 그 밖에는 아래 함수의 값.
 *
 * **SI 는 Slot Insert 의 줄임이다.** 사용자가 sysfs 의 power 파일에 1 을 쓰면
 * 핫플러그 코어가 .enable_slot 콜백으로 이 함수를 부른다.
 *
 * 앞머리는 set_attention_status 와 같지만, 실제 동작에 넘기기 전에
 * **pci_func 노드의 네 필드를 다시 채운다** -- bus, device, function 을 방금
 * 얻은 값으로 갱신하고 configured 를 0 으로 되돌린다. 저장해 둔 정보가 낡았을
 * 수 있으므로 지금 값으로 맞추고 "아직 설정되지 않았다" 고 표시하는 것이다.
 *
 * 그다음 cpqhp_process_SI(cpqphp_ctrl.c)가 전원 넣기, 속도 협상, 설정공간
 * 복원, 리눅스 코어 등록까지 나머지 전부를 맡는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(sysfs write). 아래로 내려가면 잠들 수 있다.
 *
 * 호출 체인:
 *   핫플러그 코어(sysfs power=1) → [이 함수]
 *     → to_slot, cpqhp_get_bus_dev, cpqhp_slot_find, cpqhp_process_SI
 */
static int process_SI(struct hotplug_slot *hotplug_slot)
{
	/* [한국어] 이 슬롯의 함수 정보를 담을 자리.
	 * **아래 cpqhp_slot_find 로 찾아 채운 뒤 cpqhp_process_SI 에 넘긴다** */
	struct pci_func *slot_func;
	/* [한국어] **핫플러그 코어의 슬롯에서 이 드라이버의 슬롯을 되찾는다.**
	 * cpqphp.h 의 to_slot 이 container_of 로 계산한다.
	 * 여덟 콜백이 모두 이 줄로 시작한다 */
	struct slot *slot = to_slot(hotplug_slot);
	/* [한국어] 슬롯이 붙어 있는 컨트롤러. 실제 하드웨어 조작이 여기를 거친다 */
	struct controller *ctrl = slot->ctrl;
	/* [한국어] 이 슬롯의 버스 번호를 받을 자리 */
	u8 bus;
	/* [한국어] 장치·함수 번호가 합쳐진 한 바이트를 받을 자리 */
	u8 devfn;
	/* [한국어] devfn 에서 꺼낸 장치 번호 */
	u8 device;
	/* [한국어] devfn 에서 꺼낸 함수 번호 */
	u8 function;

	/* [한국어] 어느 슬롯에 대한 요청인지 찍는다.
	 * slot_name 은 핫플러그 코어가 들고 있는 이름 문자열을 꺼내 준다 */
	dbg("%s - physical_slot = %s\n", __func__, slot_name(slot));

	/* [한국어] **물리 슬롯 번호로 버스·devfn 을 얻는다**(cpqphp_pci.c).
	 * 출력이 둘이라 포인터로 받는다 */
	if (cpqhp_get_bus_dev(ctrl, &bus, &devfn, slot->number) == -1)
		/* [한국어] 슬롯 번호에 맞는 장치가 없으면 여기서 접는다 */
		return -ENODEV;

	/* [한국어] **상위 5비트가 장치 번호다**(PCI 규격의 devfn 배치) */
	device = devfn >> 3;
	/* [한국어] **하위 3비트가 함수 번호다** */
	function = devfn & 0x7;
	/* [한국어] 쪼갠 세 값을 찍는다 */
	dbg("bus, dev, fn = %d, %d, %d\n", bus, device, function);

	/* [한국어] **전역 cpqhp_slot_list 에서 이 함수의 노드를 찾는다.**
	 * 그 노드에 저장해 둔 설정공간과 자원 목록이 들어 있다 */
	slot_func = cpqhp_slot_find(bus, device, function);
	/* [한국어] **NULL 을 제대로 검사한다.**
	 * 같은 호출을 검사 없이 역참조하는 자리가 cpqphp_ctrl.c 에 있는 것과
	 * 대비되는 자리다 */
	if (!slot_func)
		/* [한국어] 저장된 정보가 없으면 다룰 수 없다 */
		return -ENODEV;

	/* [한국어] **넘기기 전에 노드의 위치 정보를 지금 값으로 맞춘다.**
	 * 저장된 값이 낡았을 수 있기 때문이다 */
	slot_func->bus = bus;
	/* [한국어] 장치 번호를 갱신한다 */
	slot_func->device = device;
	/* [한국어] 함수 번호를 갱신한다 */
	slot_func->function = function;
	/* [한국어] **아직 설정되지 않았다고 표시한다.**
	 * 이 표시가 있어야 아래 경로가 설정공간 복원과 코어 등록을 다시 한다 */
	slot_func->configured = 0;
	/* [한국어] **로그 문구가 실제 부를 함수 이름과 다르다** --
	 * board_added 는 cpqhp_process_SI 가 안에서 부르는 함수이며,
	 * 이 줄이 직접 부르는 것은 아니다 */
	dbg("board_added(%p, %p)\n", slot_func, ctrl);
	/* [한국어] **카드를 올리는 전체 절차를 넘긴다**(cpqphp_ctrl.c).
	 * 전원 넣기, 속도 협상, 설정공간 복원, 리눅스 코어 등록까지 전부 그쪽 몫이다 */
	return cpqhp_process_SI(ctrl, slot_func);
}


/* [한국어]
 * process_SS - sysfs 의 disable_slot 요청을 받아 카드를 내린다
 *
 * @hotplug_slot: 핫플러그 코어가 넘겨 준 슬롯.
 * @return: 성공 0, 슬롯 정보를 찾지 못하면 -ENODEV, 그 밖에는 아래 함수의 값.
 *
 * **SS 는 Slot Standby 의 줄임이며 process_SI 의 짝이다.** 사용자가 power 파일에
 * 0 을 쓰면 .disable_slot 콜백으로 불린다.
 *
 * process_SI 와 달리 **pci_func 의 필드를 고치지 않는다.** 내릴 때는 저장해 둔
 * 정보를 그대로 써야 하기 때문이다. 어떤 자원을 쥐고 있었는지가 그 노드에
 * 적혀 있고, 그것을 근거로 자원을 컨트롤러의 자유 목록으로 되돌린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(sysfs write).
 *
 * 호출 체인:
 *   핫플러그 코어(sysfs power=0) → [이 함수]
 *     → to_slot, cpqhp_get_bus_dev, cpqhp_slot_find, cpqhp_process_SS
 */
static int process_SS(struct hotplug_slot *hotplug_slot)
{
	/* [한국어] 이 슬롯의 함수 정보를 담을 자리.
	 * **아래 cpqhp_slot_find 로 찾아 채운 뒤 cpqhp_process_SS 에 넘긴다** */
	struct pci_func *slot_func;
	/* [한국어] **핫플러그 코어의 슬롯에서 이 드라이버의 슬롯을 되찾는다.**
	 * cpqphp.h 의 to_slot 이 container_of 로 계산한다.
	 * 여덟 콜백이 모두 이 줄로 시작한다 */
	struct slot *slot = to_slot(hotplug_slot);
	/* [한국어] 슬롯이 붙어 있는 컨트롤러. 실제 하드웨어 조작이 여기를 거친다 */
	struct controller *ctrl = slot->ctrl;
	/* [한국어] 이 슬롯의 버스 번호를 받을 자리 */
	u8 bus;
	/* [한국어] 장치·함수 번호가 합쳐진 한 바이트를 받을 자리 */
	u8 devfn;
	/* [한국어] devfn 에서 꺼낸 장치 번호 */
	u8 device;
	/* [한국어] devfn 에서 꺼낸 함수 번호 */
	u8 function;

	/* [한국어] 어느 슬롯에 대한 요청인지 찍는다.
	 * slot_name 은 핫플러그 코어가 들고 있는 이름 문자열을 꺼내 준다 */
	dbg("%s - physical_slot = %s\n", __func__, slot_name(slot));

	/* [한국어] **물리 슬롯 번호로 버스·devfn 을 얻는다**(cpqphp_pci.c).
	 * 출력이 둘이라 포인터로 받는다 */
	if (cpqhp_get_bus_dev(ctrl, &bus, &devfn, slot->number) == -1)
		/* [한국어] 슬롯 번호에 맞는 장치가 없으면 여기서 접는다 */
		return -ENODEV;

	/* [한국어] **상위 5비트가 장치 번호다**(PCI 규격의 devfn 배치) */
	device = devfn >> 3;
	/* [한국어] **하위 3비트가 함수 번호다** */
	function = devfn & 0x7;
	/* [한국어] 쪼갠 세 값을 찍는다 */
	dbg("bus, dev, fn = %d, %d, %d\n", bus, device, function);

	/* [한국어] **전역 cpqhp_slot_list 에서 이 함수의 노드를 찾는다.**
	 * 그 노드에 저장해 둔 설정공간과 자원 목록이 들어 있다 */
	slot_func = cpqhp_slot_find(bus, device, function);
	/* [한국어] **NULL 을 제대로 검사한다.**
	 * 같은 호출을 검사 없이 역참조하는 자리가 cpqphp_ctrl.c 에 있는 것과
	 * 대비되는 자리다 */
	if (!slot_func)
		/* [한국어] 저장된 정보가 없으면 다룰 수 없다 */
		return -ENODEV;

	/* [한국어] 넘기는 두 포인터를 찍는다 */
	dbg("In %s, slot_func = %p, ctrl = %p\n", __func__, slot_func, ctrl);
	/* [한국어] **카드를 내리는 전체 절차를 넘긴다**(cpqphp_ctrl.c).
	 * **process_SI 와 달리 노드의 필드를 고치지 않는다** --
	 * 내릴 때는 저장된 정보가 그대로 필요하기 때문이다 */
	return cpqhp_process_SS(ctrl, slot_func);
}


/* [한국어]
 * hardware_test - sysfs 로 들어온 하드웨어 시험 번호를 넘긴다
 *
 * @hotplug_slot: 핫플러그 코어가 넘겨 준 슬롯.
 * @value: 사용자가 쓴 시험 번호.
 * @return: cpqhp_hardware_test 의 반환값.
 *
 * **여덟 콜백 가운데 이 드라이버 고유의 것이다.** 다른 일곱은 어느 핫플러그
 * 드라이버에나 있지만, .hardware_test 는 컨트롤러의 LED 를 순서대로 켜 보는
 * 정도의 진단 기능을 위해 남아 있는 자리다.
 *
 * 슬롯 번호로 pci_func 을 찾는 앞머리가 없다 -- 시험 대상이 슬롯이 아니라
 * 컨트롤러 전체이기 때문이다. to_slot 으로 컨트롤러만 꺼내 그대로 넘긴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(sysfs write).
 *
 * 호출 체인:
 *   핫플러그 코어(sysfs test) → [이 함수] → to_slot, cpqhp_hardware_test
 */
static int hardware_test(struct hotplug_slot *hotplug_slot, u32 value)
{
	/* [한국어] 코어의 슬롯에서 이 드라이버의 슬롯을 되찾는다 */
	struct slot *slot = to_slot(hotplug_slot);
	/* [한국어] **시험 대상은 슬롯이 아니라 컨트롤러 전체다.**
	 * 그래서 아래 앞머리에 슬롯 조회가 없다 */
	struct controller *ctrl = slot->ctrl;

	/* [한국어] 어느 슬롯을 통해 들어온 요청인지 찍는다 */
	dbg("%s - physical_slot = %s\n", __func__, slot_name(slot));

	/* [한국어] 시험 번호를 그대로 넘긴다(cpqphp_ctrl.c).
	 * LED 를 순서대로 켜 보는 정도의 진단이다 */
	return cpqhp_hardware_test(ctrl, value);
}


/* [한국어]
 * get_power_status - 슬롯에 전원이 들어와 있는지 sysfs 에 알린다
 *
 * @hotplug_slot: 핫플러그 코어가 넘겨 준 슬롯.
 * @value: 결과를 담아 돌려줄 자리. 0 또는 1.
 * @return: 늘 0. 읽기는 실패하지 않는다.
 *
 * **아래 넷은 모두 상태를 읽어 돌려주는 콜백이며 구조가 같다** --
 * to_slot 으로 슬롯과 컨트롤러를 꺼내고, 로그를 한 줄 찍고, cpqphp.h 의 인라인
 * 함수 하나를 불러 그 결과를 value 에 담는다.
 *
 * 여기서는 get_slot_enabled 를 부른다. 그 함수는 SLOT_ENABLE 레지스터의 이
 * 슬롯 비트를 읽는다. **전원과 버스 연결이 별개 레지스터인데 전원 상태로
 * 버스 연결 비트를 보고한다** -- 두 가지가 늘 함께 켜지고 꺼지므로 실질적
 * 차이가 없다고 본 것으로 보이나, 코드가 그 이유를 적어 두지는 않았다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(사용자가 sysfs 를 read 한 문맥).
 *
 * 호출 체인:
 *   핫플러그 코어(sysfs power 읽기) → [이 함수] → to_slot, get_slot_enabled
 */
static int get_power_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	/* [한국어] 코어의 슬롯에서 이 드라이버의 슬롯을 되찾는다 */
	struct slot *slot = to_slot(hotplug_slot);
	/* [한국어] 레지스터를 읽을 컨트롤러를 꺼낸다 */
	struct controller *ctrl = slot->ctrl;

	/* [한국어] 어느 슬롯을 읽는지 찍는다 */
	dbg("%s - physical_slot = %s\n", __func__, slot_name(slot));

	/* [한국어] SLOT_ENABLE 레지스터의 이 슬롯 비트를 읽어 0 또는 1 을 담는다.
	 * **전원과 버스 연결이 별개 레지스터인데 활성 비트로 전원을 보고한다** --
	 * 둘이 늘 함께 켜지고 꺼지므로 실질적 차이가 없다고 본 것으로 보인다 */
	*value = get_slot_enabled(ctrl, slot);
	/* [한국어] **읽기는 실패하지 않으므로 늘 0 이다.**
	 * 결과는 반환값이 아니라 value 포인터로 나간다 */
	return 0;
}

/* [한국어]
 * get_attention_status - 황색 LED 가 켜져 있는지 sysfs 에 알린다
 *
 * @hotplug_slot: 핫플러그 코어가 넘겨 준 슬롯.
 * @value: 결과를 담아 돌려줄 자리. 0 또는 1.
 * @return: 늘 0.
 *
 * **LED 자체가 상태 저장소다.** 이 드라이버는 "주의 표시가 켜져 있는가" 를
 * 따로 변수로 두지 않고 하드웨어 레지스터를 그대로 읽어 답한다.
 * 그래서 set_attention_status 로 켠 값이 그대로 여기로 되돌아온다.
 *
 * cpq_get_attention_status(cpqphp.h)가 슬롯 번호를 레지스터 비트 자리로
 * 바꾼 뒤 read_amber_LED 를 부른다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(sysfs read).
 *
 * 호출 체인:
 *   핫플러그 코어(sysfs attention 읽기) → [이 함수]
 *     → to_slot, cpq_get_attention_status
 */
static int get_attention_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	/* [한국어] 코어의 슬롯에서 이 드라이버의 슬롯을 되찾는다 */
	struct slot *slot = to_slot(hotplug_slot);
	/* [한국어] 레지스터를 읽을 컨트롤러를 꺼낸다 */
	struct controller *ctrl = slot->ctrl;

	/* [한국어] 어느 슬롯을 읽는지 찍는다 */
	dbg("%s - physical_slot = %s\n", __func__, slot_name(slot));

	/* [한국어] **황색 LED 가 켜져 있는지를 그대로 답으로 쓴다.**
	 * 따로 상태 변수를 두지 않고 하드웨어를 상태 저장소로 삼는다 */
	*value = cpq_get_attention_status(ctrl, slot);
	/* [한국어] **읽기는 실패하지 않으므로 늘 0 이다.**
	 * 결과는 반환값이 아니라 value 포인터로 나간다 */
	return 0;
}

/* [한국어]
 * get_latch_status - 슬롯 레버가 닫혀 있는지 sysfs 에 알린다
 *
 * @hotplug_slot: 핫플러그 코어가 넘겨 준 슬롯.
 * @value: 결과를 담아 돌려줄 자리. 닫혀 있으면 1.
 * @return: 늘 0.
 *
 * 레버(latch)는 카드를 물리적으로 잠그는 장치다. 열려 있으면 카드를 뽑을 수
 * 있다는 뜻이므로, 동작 중인 슬롯의 레버가 열리면 위험 신호가 된다.
 *
 * cpq_get_latch_status(cpqphp.h)가 INT_INPUT_CLEAR 레지스터의 이 슬롯 비트를
 * 읽는데, **하드웨어가 열림을 1 로 알리므로 그 함수가 뜻을 뒤집어 돌려준다.**
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(sysfs read).
 *
 * 호출 체인:
 *   핫플러그 코어(sysfs latch 읽기) → [이 함수]
 *     → to_slot, cpq_get_latch_status
 */
static int get_latch_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	/* [한국어] 코어의 슬롯에서 이 드라이버의 슬롯을 되찾는다 */
	struct slot *slot = to_slot(hotplug_slot);
	/* [한국어] 레지스터를 읽을 컨트롤러를 꺼낸다 */
	struct controller *ctrl = slot->ctrl;

	/* [한국어] 어느 슬롯을 읽는지 찍는다 */
	dbg("%s - physical_slot = %s\n", __func__, slot_name(slot));

	/* [한국어] 레버가 닫혀 있으면 1 을 담는다.
	 * **하드웨어는 열림을 1 로 알리므로 그 함수가 뜻을 뒤집어 돌려준다** */
	*value = cpq_get_latch_status(ctrl, slot);

	/* [한국어] **읽기는 실패하지 않으므로 늘 0 이다.**
	 * 결과는 반환값이 아니라 value 포인터로 나간다 */
	return 0;
}

/* [한국어]
 * get_adapter_status - 슬롯에 카드가 꽂혀 있는지 sysfs 에 알린다
 *
 * @hotplug_slot: 핫플러그 코어가 넘겨 준 슬롯.
 * @value: 결과를 담아 돌려줄 자리. 꽂혀 있으면 0 이 아닌 값.
 * @return: 늘 0.
 *
 * **돌려주는 값이 0 또는 1 이 아니라 0 또는 2 다.** get_presence_status
 * (cpqphp.h)가 감지선 비트를 `& 0x02` 로 뽑아내기 때문이다. 사용자 공간은
 * 참/거짓으로만 읽으므로 문제가 되지 않는다.
 *
 * 감지선을 두 자리에서 읽어 OR 하는 것은 PCI 규격이 카드 폭을 알리려고
 * 감지선(PRSNT1, PRSNT2)을 두 개 두기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(sysfs read).
 *
 * 호출 체인:
 *   핫플러그 코어(sysfs adapter 읽기) → [이 함수]
 *     → to_slot, get_presence_status
 */
static int get_adapter_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	/* [한국어] 코어의 슬롯에서 이 드라이버의 슬롯을 되찾는다 */
	struct slot *slot = to_slot(hotplug_slot);
	/* [한국어] 레지스터를 읽을 컨트롤러를 꺼낸다 */
	struct controller *ctrl = slot->ctrl;

	/* [한국어] 어느 슬롯을 읽는지 찍는다 */
	dbg("%s - physical_slot = %s\n", __func__, slot_name(slot));

	/* [한국어] **카드가 꽂혀 있으면 0 이 아닌 값을 담는다 -- 1 이 아니라 2 다.**
	 * 감지선 비트를 `& 0x02` 로 뽑아내기 때문이며,
	 * 사용자 공간이 참/거짓으로만 읽으므로 문제가 되지 않는다 */
	*value = get_presence_status(ctrl, slot);

	/* [한국어] **읽기는 실패하지 않으므로 늘 0 이다.**
	 * 결과는 반환값이 아니라 value 포인터로 나간다 */
	return 0;
}

/* [한국어] **이 드라이버가 바깥 세계에 보이는 면 전부.**
 * 핫플러그 코어가 sysfs 파일을 만들고, 사용자가 그 파일을 읽거나 쓰면
 * 여기 적힌 함수를 부른다. ctrl_slot_setup 이 슬롯마다
 * `slot->hotplug_slot.ops = &cpqphp_hotplug_slot_ops` 로 이어 둔다.
 * **const 이며 모든 슬롯이 같은 표 하나를 공유한다** -- 슬롯별 상태는
 * hotplug_slot 구조체 쪽에 있고 콜백은 to_slot 으로 그것을 되찾는다.
 * **여덟 개 가운데 hardware_test 만 이 드라이버 고유이고** 나머지 일곱은
 * 어느 핫플러그 드라이버에나 있는 항목이다 */
static const struct hotplug_slot_ops cpqphp_hotplug_slot_ops = {
	/* [한국어] sysfs 의 attention 파일에 쓰면 불린다. 황색 LED 를 켜고 끈다 */
	.set_attention_status =	set_attention_status,
	/* [한국어] power 파일에 1 을 쓰면 불린다. **카드를 올린다** */
	.enable_slot =		process_SI,
	/* [한국어] power 파일에 0 을 쓰면 불린다. **카드를 내린다** */
	.disable_slot =		process_SS,
	/* [한국어] test 파일에 시험 번호를 쓰면 불린다.
	 * **이 드라이버 고유 항목이며 지금 세대 드라이버에는 없다** */
	.hardware_test =	hardware_test,
	/* [한국어] power 파일을 읽으면 불린다 */
	.get_power_status =	get_power_status,
	/* [한국어] attention 파일을 읽으면 불린다 */
	.get_attention_status =	get_attention_status,
	/* [한국어] latch 파일을 읽으면 불린다 */
	.get_latch_status =	get_latch_status,
	/* [한국어] adapter 파일을 읽으면 불린다.
	 * **여기까지 여덟 개가 이 드라이버의 사용자 인터페이스 전부다** */
	.get_adapter_status =	get_adapter_status,
};

/* [한국어] **sysfs 슬롯 이름 버퍼의 크기.**
 * 이름은 슬롯 번호를 그대로 찍은 문자열이라 열 바이트면 넉넉하다.
 * ctrl_slot_setup 의 지역 배열 name 의 크기가 된다 */
#define SLOT_NAME_SIZE 10

/* [한국어]
 * ctrl_slot_setup - 컨트롤러의 슬롯을 모두 만들어 핫플러그 코어에 등록한다
 *
 * @ctrl: 슬롯을 달아 줄 컨트롤러.
 * @smbios_start: SMBIOS 표 본문의 시작.
 * @smbios_table: SMBIOS 진입점 구조체의 주소.
 * @return: 성공 0, 메모리 부족이면 -ENOMEM, 등록 실패면 그 오류.
 *
 * **이 파일에서 struct slot 이 태어나는 유일한 자리다.** 컨트롤러의 SLOT_MASK
 * 레지스터가 알려 주는 슬롯 개수만큼 반복하며, 슬롯 하나마다 다음을 한다.
 *
 * 1. kzalloc 으로 struct slot 을 만들고 컨트롤러·버스·장치·번호를 채운다.
 *    장치 번호는 SLOT_MASK 의 상위 니블에서 시작해 하나씩 늘어나고,
 *    물리 번호는 ctrl->first_slot 에서 시작해 하나씩 늘어난다.
 *    **두 번호가 따로 세어지는 것이 이 드라이버의 두 번호 체계다.**
 * 2. SMBIOS Type 9 항목 가운데 슬롯 번호가 맞는 것을 찾아 p_sm_slot 에 잇는다.
 *    그래야 is_slot64bit / is_slot66mhz 로 물리적 능력을 읽을 수 있다.
 * 3. 5초 타이머를 준비한다. **타이머 콜백이 cpqhp_pushbutton_thread 이며**,
 *    버튼을 눌렀을 때 취소 창을 여는 장치다. expires 를 미리 채워 두지만
 *    여기서 타이머를 걸지는 않는다.
 * 4. capabilities 비트를 채운다. 앞의 둘은 무조건 켜고, 폭과 속도는 SMBIOS 에서,
 *    현재 카드 유무·레버 상태·전원 상태는 레지스터에서 읽는다. **원본 주석이
 *    FIXME 로 밝히듯 이 필드를 읽는 곳이 없다.**
 * 5. 슬롯 번호를 문자열로 만들어 pci_hp_register 로 코어에 등록한다.
 *    이 순간부터 sysfs 에 슬롯 디렉터리가 생기고 여덟 콜백이 열린다.
 * 6. 컨트롤러의 슬롯 리스트 **머리에** 매단다. 그래서 리스트는 번호 역순이다.
 *
 * 실패하면 error_slot 으로 가 방금 만든 슬롯만 해제한다. **앞서 등록에 성공한
 * 슬롯들은 그대로 남는다** -- 되돌리는 일은 ctrl_slot_cleanup 이 맡는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe 경로). kzalloc 이 있어 잠들 수 있다.
 *
 * 호출 체인:
 *   cpqhpc_probe → [이 함수]
 *     → get_SMBIOS_entry, timer_setup, is_slot64bit, is_slot66mhz,
 *       read_slot_enable, pci_hp_register
 */
static int ctrl_slot_setup(struct controller *ctrl,
			void __iomem *smbios_start,
			void __iomem *smbios_table)
{
	/* [한국어] 지금 만들고 있는 슬롯 */
	struct slot *slot;
	/* [한국어] **컨트롤러가 들고 있는 pci_bus 사본.**
	 * 아래에서 현재 버스 속도를 읽는 데만 쓴다 */
	struct pci_bus *bus = ctrl->pci_bus;
	/* [한국어] 남은 슬롯 수. 반복을 끝내는 값이다 */
	u8 number_of_slots;
	/* [한국어] 지금 슬롯의 PCI 장치 번호. 한 슬롯마다 하나씩 늘어난다 */
	u8 slot_device;
	/* [한국어] 지금 슬롯의 물리 번호. 이것도 하나씩 늘어난다 */
	u8 slot_number;
	/* [한국어] 컨트롤러 안 슬롯 번호(0부터). 레지스터 비트 자리로 쓴다 */
	u8 ctrl_slot;
	/* [한국어] INT_INPUT_CLEAR 를 한 번 읽어 둔 값.
	 * **반복 밖에서 한 번만 읽어 모든 슬롯이 같은 시점의 상태를 본다** */
	u32 tempdword;
	/* [한국어] sysfs 에 등록할 이름 문자열 버퍼 */
	char name[SLOT_NAME_SIZE];
	/* [한국어] **SMBIOS 표 탐색의 이어보기 위치.**
	 * NULL 로 시작해 반복마다 앞서 본 곳 다음부터 이어 찾는다.
	 * **반복 밖에 선언되어 슬롯이 바뀌어도 유지된다** -- 표 항목이 슬롯
	 * 순서대로 있다는 전제에서 앞으로만 나아가는 구조다 */
	void __iomem *slot_entry = NULL;
	/* [한국어] 오류 코드를 담아 실패 라벨로 들고 갈 자리 */
	int result;

	/* [한국어] 이 함수에 들어왔음을 찍는다 */
	dbg("%s\n", __func__);

	/* [한국어] **슬롯 상태(카드 유무, 레버)를 한 번에 읽어 둔다.**
	 * 읽기만 하므로 인터럽트 상태가 지워지지는 않는다 */
	tempdword = readl(ctrl->hpc_reg + INT_INPUT_CLEAR);

	/* [한국어] **하위 니블이 슬롯 개수다** */
	number_of_slots = readb(ctrl->hpc_reg + SLOT_MASK) & 0x0F;
	/* [한국어] **상위 니블이 첫 슬롯의 PCI 장치 번호다.**
	 * 같은 레지스터를 두 번 읽는데 값이 바뀔 일은 없다 */
	slot_device = readb(ctrl->hpc_reg + SLOT_MASK) >> 4;
	/* [한국어] **물리 번호의 시작값.**
	 * get_slot_mapping 이 $PIR 표에서 찾아 둔 값이다 */
	slot_number = ctrl->first_slot;

	/* [한국어] 슬롯 수만큼 반복하며 하나씩 만든다 */
	while (number_of_slots) {
		/* [한국어] **슬롯 구조체를 0 으로 채워 만든다.**
		 * 0 초기화 덕에 next, capabilities, p_sm_slot 이 자동으로 비워진다 */
		slot = kzalloc_obj(*slot);
		/* [한국어] 메모리 부족 */
		if (!slot) {
			/* [한국어] 오류 코드를 담는다 */
			result = -ENOMEM;
			/* [한국어] **error 라벨로 간다** -- slot 이 NULL 이므로 kfree 를 건너뛰어야 한다 */
			goto error;
		}

		/* [한국어] 어느 컨트롤러 소속인지 이어 둔다. 콜백이 이 고리로 컨트롤러를 찾는다 */
		slot->ctrl = ctrl;
		/* [한국어] 컨트롤러가 관장하는 버스 번호를 복사한다 */
		slot->bus = ctrl->bus;
		/* [한국어] 이 슬롯의 PCI 장치 번호 */
		slot->device = slot_device;
		/* [한국어] **사용자에게 보이는 물리 번호.** sysfs 이름이 된다 */
		slot->number = slot_number;
		/* [한국어] 매긴 번호를 찍는다 */
		dbg("slot->number = %u\n", slot->number);

		/* [한국어] **SMBIOS Type 9(System Slot) 항목을 하나 가져온다.**
		 * 앞서 본 곳 다음부터 이어 찾는다 */
		slot_entry = get_SMBIOS_entry(smbios_start, smbios_table, 9,
					slot_entry);

		/* [한국어] **슬롯 번호가 맞는 항목을 만날 때까지 계속 나아간다.**
		 * 표의 항목 순서와 슬롯 순서가 반드시 같지는 않기 때문이다 */
		while (slot_entry && (readw(slot_entry + SMBIOS_SLOT_NUMBER) !=
				slot->number)) {
			/* [한국어] 다음 Type 9 항목으로 나아간다 */
			slot_entry = get_SMBIOS_entry(smbios_start,
						smbios_table, 9, slot_entry);
		}

		/* [한국어] **찾은 항목을 슬롯에 이어 둔다.**
		 * **못 찾았으면 NULL 이 들어가는데 아래 is_slot64bit 이 검사 없이
		 * 역참조한다.** 코드는 손대지 않고 사실만 적는다 */
		slot->p_sm_slot = slot_entry;

		/* [한국어] **5초 취소 창을 여는 타이머를 준비한다.**
		 * 콜백이 cpqhp_pushbutton_thread(cpqphp_ctrl.c)이며,
		 * **이름은 스레드지만 실제로는 타이머 콜백이다.**
		 * 여기서는 준비만 하고 걸지는 않는다 -- 버튼을 눌렀을 때 건다 */
		timer_setup(&slot->task_event, cpqhp_pushbutton_thread, 0);
		/* [한국어] **만료 시각을 지금부터 5초 뒤로 미리 적어 둔다.**
		 * HZ 는 초당 타이머 틱 수이므로 `5 * HZ` 가 5초다.
		 * **타이머를 걸 때 다시 계산하지 않으면 이 값은 이미 지난 시각이 된다** --
		 * cpqphp_ctrl.c 의 버튼 경로가 걸기 직전에 다시 채운다 */
		slot->task_event.expires = jiffies + 5 * HZ;

		/*FIXME: these capabilities aren't used but if they are
		 *	 they need to be correctly implemented
		 */
		/* [한국어] **교체 지원을 무조건 켠다.** 원문 주석이 FIXME 로 밝히듯
		 * 이 capabilities 필드를 읽는 곳이 없어 값이 맞는지는 확인되지 않는다 */
		slot->capabilities |= PCISLOT_REPLACE_SUPPORTED;
		/* [한국어] **레버 지원도 무조건 켠다.**
		 * 컨트롤러의 slot_switch_type 을 보지 않으므로 레버가 없는 기종에서도 켜진다 */
		slot->capabilities |= PCISLOT_INTERLOCK_SUPPORTED;

		/* [한국어] SMBIOS 가 이 슬롯을 64비트로 적어 두었는지 본다 */
		if (is_slot64bit(slot))
			/* [한국어] 64비트 지원 비트를 세운다 */
			slot->capabilities |= PCISLOT_64_BIT_SUPPORTED;
		/* [한국어] SMBIOS 가 66MHz 슬롯으로 적어 두었는지 본다 */
		if (is_slot66mhz(slot))
			/* [한국어] 66MHz 지원 비트를 세운다 */
			slot->capabilities |= PCISLOT_66_MHZ_SUPPORTED;
		/* [한국어] **지금 버스가 실제로 66MHz 로 돌고 있는지 본다.**
		 * 앞의 SUPPORTED 비트와 달리 이쪽은 현재 동작 속도다 */
		if (bus->cur_bus_speed == PCI_SPEED_66MHz)
			/* [한국어] 66MHz 동작 비트를 세운다 */
			slot->capabilities |= PCISLOT_66_MHZ_OPERATION;

		/* [한국어] **컨트롤러 안 슬롯 번호를 구한다.**
		 * 지금 장치 번호에서 첫 슬롯의 장치 번호를 빼면 0부터의 자리가 된다.
		 * 아래 세 줄이 이 값으로 레지스터 비트를 뽑는다 */
		ctrl_slot =
			slot_device - (readb(ctrl->hpc_reg + SLOT_MASK) >> 4);

		/* Check presence */
		/* [한국어] **카드가 꽂혀 있는지를 비트로 담는다.**
		 * cpqphp.h 의 get_presence_status 와 똑같은 식이다 -- 뒤집고,
		 * 23비트와 15비트 시프트를 OR 하고, 슬롯 자리로 내린 뒤 0x02 만 남긴다.
		 * 감지선이 두 벌이라 두 자리를 합쳐야 한다.
		 * 원문 주석: Check presence */
		slot->capabilities |=
			((((~tempdword) >> 23) |
			 ((~tempdword) >> 15)) >> ctrl_slot) & 0x02;
		/* Check the switch state */
		/* [한국어] **레버 상태를 비트로 담는다.**
		 * 하위 한 바이트에 슬롯별 레버 비트가 있고, 뒤집어야 닫힘이 1 이 된다.
		 * 원문 주석: Check the switch state */
		slot->capabilities |=
			((~tempdword & 0xFF) >> ctrl_slot) & 0x01;
		/* Check the slot enable */
		/* [한국어] **슬롯 활성 상태를 비트로 담는다.**
		 * read_slot_enable 로 여덟 슬롯 비트를 통째로 읽어 2비트 왼쪽으로 민 뒤
		 * 슬롯 자리로 내려 0x04 만 남긴다. 미리 밀어 두는 것은
		 * PCISLOT_POWERED 가 비트 2 이기 때문이다.
		 * 원문 주석: Check the slot enable */
		slot->capabilities |=
			((read_slot_enable(ctrl) << 2) >> ctrl_slot) & 0x04;

		/* register this slot with the hotplug pci core */
		/* [한국어] **물리 번호를 그대로 sysfs 이름으로 삼는다.**
		 * snprintf 라 버퍼를 넘치지 않는다 */
		snprintf(name, SLOT_NAME_SIZE, "%u", slot->number);
		/* [한국어] **여덟 콜백 표를 이어 붙인다.**
		 * 이 줄이 있어야 코어가 sysfs 요청을 이 드라이버로 보낸다 */
		slot->hotplug_slot.ops = &cpqphp_hotplug_slot_ops;

		/* [한국어] 등록 직전의 모든 번호를 한꺼번에 찍는다.
		 * **두 번호 체계가 얽히는 자리라 흔적이 남아 있다** */
		dbg("registering bus %d, dev %d, number %d, ctrl->slot_device_offset %d, slot %d\n",
				slot->bus, slot->device,
				slot->number, ctrl->slot_device_offset,
				slot_number);
		/* [한국어] **핫플러그 코어에 이 슬롯을 등록한다.**
		 * 이 순간부터 sysfs 에 슬롯 디렉터리가 생기고 콜백이 열린다.
		 * 버스로 `ctrl->pci_dev->bus` 를 넘기는 것이 눈에 띈다 --
		 * 슬롯이 실제로 붙은 버스(slot->bus)가 아니라 **컨트롤러 자신이 있는
		 * 버스** 다. 컨트롤러가 브리지라 두 버스가 다를 수 있다 */
		result = pci_hp_register(&slot->hotplug_slot,
					 ctrl->pci_dev->bus,
					 slot->device,
					 name);
		/* [한국어] 등록 실패 */
		if (result) {
			/* [한국어] 실패를 로그에 남긴다 */
			err("pci_hp_register failed with error %d\n", result);
			/* [한국어] **방금 만든 슬롯만 해제한다** -- 앞서 등록에 성공한 슬롯은 그대로 남고,
			 * 되돌리는 일은 ctrl_slot_cleanup 이 맡는다 */
			goto error_slot;
		}

		/* [한국어] **리스트 머리에 매단다.** 그래서 리스트는 슬롯 번호 역순이 된다 */
		slot->next = ctrl->slot;
		/* [한국어] 컨트롤러의 머리를 새 슬롯으로 바꾼다 */
		ctrl->slot = slot;

		/* [한국어] 남은 슬롯 수를 줄인다 */
		number_of_slots--;
		/* [한국어] 다음 슬롯의 PCI 장치 번호. **연속이라고 전제한다** */
		slot_device++;
		/* [한국어] 다음 슬롯의 물리 번호. 이것도 연속이라고 전제한다 */
		slot_number++;
	}

	/* [한국어] 모든 슬롯을 만들고 등록했다 */
	return 0;
/* [한국어] 등록에 실패했을 때 오는 자리 */
error_slot:
	/* [한국어] 방금 만든 슬롯 구조체만 해제한다 */
	kfree(slot);
/* [한국어] 메모리 부족으로 왔을 때 오는 자리. 해제할 것이 없다 */
error:
	/* [한국어] 오류 코드를 그대로 올린다.
	 * 호출자 cpqhpc_probe 가 err_iounmap 으로 되돌린다 */
	return result;
}

/* [한국어]
 * one_time_init - 컨트롤러가 여럿이어도 한 번만 해야 하는 초기화
 *
 * @return: 성공 0, 실패면 음수 오류 코드.
 *
 * **컨트롤러마다 부르지만 실제 일은 첫 번째에만 한다.** 맨 앞의
 * `if (initialized) return 0;` 이 그 장치다. Compaq 서버는 PCI 버스마다
 * 핫플러그 컨트롤러를 따로 두는 구성이 있어 probe 가 여러 번 불릴 수 있는데,
 * $PIR 표를 받거나 ROM 을 매핑하는 일은 시스템에 하나뿐인 자원을 다루므로
 * 한 번이면 된다.
 *
 * 하는 일은 다섯이다.
 * 1. init_cpqhp_routing_table 로 BIOS 의 $PIR 표를 받는다.
 * 2. cpqhp_debug 가 켜져 있으면 그 표를 사람이 읽게 찍는다.
 * 3. cpqhp_event_start_thread 로 사건 처리 기구를 띄운다.
 * 4. cpqhp_slot_list 256칸을 NULL 로 민다. **전역 배열이라 이미 0 이지만
 *    명시적으로 지운다.**
 * 5. ROM 을 ioremap 하고 NVRAM 진입점을 잡고 SMBIOS 표를 찾아 매핑한다.
 *
 * **오류 처리에 눈에 띄는 점이 있다.** error_smbios_start 라벨이
 * `iounmap(smbios_start)` 를 부르는데, 그 라벨로 오는 경로는 smbios_start 의
 * ioremap 이 실패해 NULL 인 경우다. 그다음 error_rom_start 로 이어지지 않고
 * 곧바로 error 로 떨어지므로 cpqhp_rom_start 매핑이 남는다. 코드는 손대지
 * 않고 사실만 적는다.
 *
 * 또한 맨 앞에서 `power_mode = 0` 으로 모듈 인자를 덮어쓴다 -- 사용자가 준
 * 값을 무시하는 셈이며, 그 이유가 코드에 적혀 있지 않다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe 경로). 잠들 수 있다.
 *
 * 호출 체인:
 *   cpqhpc_probe → [이 함수]
 *     → init_cpqhp_routing_table, pci_print_IRQ_route, cpqhp_event_start_thread,
 *       ioremap, compaq_nvram_init, detect_SMBIOS_pointer
 */
static int one_time_init(void)
{
	/* [한국어] cpqhp_slot_list 256칸을 미는 반복 색인 */
	int loop;
	/* [한국어] 오류 코드. 0 으로 시작해 실패한 자리에서만 바뀐다 */
	int retval = 0;

	/* [한국어] **이미 한 번 했으면 아무것도 하지 않는다.**
	 * 컨트롤러가 여럿이면 probe 가 여러 번 불리기 때문이다 */
	if (initialized)
		/* [한국어] 두 번째 이후의 호출은 성공으로 곧바로 돌아간다 */
		return 0;

	/* [한국어] **모듈 인자 power_mode 를 0 으로 덮어쓴다.**
	 * 사용자가 준 값이 무시되는 셈이며 **그 이유가 코드에 적혀 있지 않다.**
	 * 그 결과 cpqhpc_probe 는 늘 빈 슬롯의 전원을 끄는 쪽으로 간다 */
	power_mode = 0;

	/* [한국어] **BIOS 의 $PIR 표를 받아 전역에 둔다.**
	 * 이 표가 없으면 슬롯의 물리 번호를 알 수 없어 드라이버가 성립하지 않는다 */
	retval = init_cpqhp_routing_table();
	/* [한국어] 표를 못 받은 경우 */
	if (retval)
		/* [한국어] **되돌릴 것이 없으므로 곧바로 반환한다** */
		goto error;

	/* [한국어] 디버그가 켜져 있을 때만 */
	if (cpqhp_debug)
		/* [한국어] 받은 표 전체를 사람이 읽을 수 있게 찍는다 */
		pci_print_IRQ_route();

	/* [한국어] 사건 처리 기구를 띄운다는 것을 알린다 */
	dbg("Initialize + Start the notification mechanism\n");

	/* [한국어] **사건 처리 기구를 시작한다**(cpqphp_ctrl.c).
	 * 이름은 스레드지만 지금은 워크큐 기반이며,
	 * 인터럽트가 event_queue 에 적은 사건을 스레드 문맥에서 처리한다 */
	retval = cpqhp_event_start_thread();
	/* [한국어] 기구를 못 띄운 경우 */
	if (retval)
		/* [한국어] **$PIR 표를 해제하지 않고 나간다** -- 되돌리는 코드가 없다.
		 * 코드는 손대지 않고 사실만 적는다 */
		goto error;

	/* [한국어] 전역 배열을 비운다는 것을 알린다 */
	dbg("Initialize slot lists\n");
	/* [한국어] 버스 번호 256칸을 모두 훑는다 */
	for (loop = 0; loop < 256; loop++)
		/* [한국어] **칸을 비운다.**
		 * 전역 배열이라 BSS 에서 이미 0 이지만 명시적으로 지운다.
		 * **모듈을 두 번 올리고 내려도 같은 메모리를 다시 쓰는 것은 아니므로
		 * 이 초기화가 꼭 필요하지는 않다** */
		cpqhp_slot_list[loop] = NULL;

	/* FIXME: We also need to hook the NMI handler eventually.
	 * this also needs to be worked with Christoph
	 * register_NMI_handler();
	 */
	/* Map rom address */
	/* [한국어] **시스템 ROM 구간(물리 0xF0000, 64KB)을 커널 주소 공간에 매핑한다.**
	 * PC 구조에서 BIOS 가 자리 잡는 전통적인 구간이며, 여기에 SMBIOS 표와
	 * HRT 와 int15 진입점이 모두 들어 있다.
	 * 원문 주석: Map rom address.
	 * 바로 위 FIXME 는 NMI 처리기를 걸어야 한다는 미완의 계획을 남긴 것이다 */
	cpqhp_rom_start = ioremap(ROM_PHY_ADDR, ROM_PHY_LEN);
	/* [한국어] 매핑 실패 */
	if (!cpqhp_rom_start) {
		/* [한국어] 실패를 로그에 남긴다 */
		err("Could not ioremap memory region for ROM\n");
		/* [한국어] 입출력 오류로 알린다 */
		retval = -EIO;
		/* [한국어] 되돌릴 매핑이 아직 없다 */
		goto error;
	}

	/* Now, map the int15 entry point if we are on compaq specific
	 * hardware
	 */
	/* [한국어] **Compaq 고유 NVRAM 접근 준비**(cpqphp_nvram.c).
	 * ROM 안에서 int15 진입점을 찾아 둔다. 원문 주석이 밝히듯 Compaq
	 * 하드웨어일 때만 의미가 있으며, **반환값을 확인하지 않는다** */
	compaq_nvram_init(cpqhp_rom_start);

	/* Map smbios table entry point structure */
	/* [한국어] **ROM 을 훑어 SMBIOS 진입점의 "_SM_" 서명을 찾는다.**
	 * 원문 주석: Map smbios table entry point structure */
	smbios_table = detect_SMBIOS_pointer(cpqhp_rom_start,
					cpqhp_rom_start + ROM_PHY_LEN);
	/* [한국어] 진입점을 못 찾은 경우 */
	if (!smbios_table) {
		/* [한국어] 실패를 로그에 남긴다 */
		err("Could not find the SMBIOS pointer in memory\n");
		/* [한국어] 입출력 오류로 알린다 */
		retval = -EIO;
		/* [한국어] **ROM 매핑을 놓아야 하므로 그 라벨로 간다** */
		goto error_rom_start;
	}

	/* [한국어] **진입점이 알려 주는 주소와 길이로 표 본문을 따로 매핑한다.**
	 * ST_ADDRESS 는 u32 라 readl, ST_LENGTH 는 u16 이라 readw 를 쓴다.
	 * **표 본문은 ROM 밖에 있을 수 있어 별도 매핑이 필요하다** */
	smbios_start = ioremap(readl(smbios_table + ST_ADDRESS),
					readw(smbios_table + ST_LENGTH));
	/* [한국어] 본문 매핑 실패 */
	if (!smbios_start) {
		/* [한국어] 실패를 로그에 남긴다 */
		err("Could not ioremap memory region taken from SMBIOS values\n");
		/* [한국어] 입출력 오류로 알린다 */
		retval = -EIO;
		/* [한국어] **이 라벨은 smbios_start 를 iounmap 하는데 그 값은 지금 NULL 이다** --
		 * iounmap(NULL) 은 아무 일도 하지 않으므로 해롭지는 않으나,
		 * 정작 놓아야 할 cpqhp_rom_start 는 놓지 않고 error 로 떨어진다.
		 * 코드는 손대지 않고 사실만 적는다 */
		goto error_smbios_start;
	}

	/* [한국어] **여기까지 왔으면 성공이다.** 다음 probe 는 맨 앞에서 곧바로 돌아간다 */
	initialized = 1;

	/* [한국어] 0 을 돌려준다 */
	return retval;

/* [한국어] 표 본문 매핑에 실패했을 때 오는 자리 */
error_smbios_start:
	/* [한국어] **NULL 을 놓는 셈이라 실질적으로 아무 일도 하지 않는다** */
	iounmap(smbios_start);
/* [한국어] SMBIOS 진입점을 못 찾았을 때 오는 자리 */
error_rom_start:
	/* [한국어] ROM 매핑을 놓는다 */
	iounmap(cpqhp_rom_start);
/* [한국어] 앞 단계에서 실패했을 때 오는 자리. 놓을 것이 없다 */
error:
	/* [한국어] 오류 코드를 호출자 cpqhpc_probe 에 올린다 */
	return retval;
}

/* [한국어]
 * cpqhpc_probe - 핫플러그 컨트롤러 하나를 처음부터 끝까지 세운다
 *
 * @pdev: PCI 코어가 찾아 준 컨트롤러 장치.
 * @ent: 어느 표 항목에 걸렸는지. **이 함수는 이 인자를 쓰지 않는다** --
 *   hpcd_pci_tbl 이 클래스 코드 하나로만 걸러 내므로 구별할 것이 없다.
 * @return: 성공 0, 실패면 음수 오류 코드.
 *
 * **이 드라이버에서 가장 긴 함수이며, 전체 구조가 여기 다 들어 있다.**
 * 크게 다섯 단계로 읽으면 된다.
 *
 * **1단계 -- 이 장치를 맡을 수 있는지 가린다.** 브리지인지, 벤더가 Compaq 이나
 * Intel 인지, 리비전이 지원 범위인지, 서브시스템 벤더가 맞는지 차례로 본다.
 * 하나라도 어긋나면 곧바로 물러난다.
 *
 * **2단계 -- 능력을 정한다.** 서브시스템 벤더에 따라 갈래가 갈린다. Compaq 은
 * 서브시스템 **장치 ID 하나가 기종 하나** 를 뜻해 switch 로 기종별 설정을
 * 통째로 적어 둔다. Intel 은 **서브시스템 장치 ID 의 비트 하나하나가 기능
 * 하나** 를 뜻해 비트마다 if 로 푼다. 같은 필드를 두 벤더가 전혀 다르게 쓰는
 * 것이며, 원문 주석도 그 차이를 밝힌다.
 *
 * **3단계 -- 하드웨어를 잡는다.** pci_bus 사본을 만들고, 뮤텍스와 대기 큐를
 * 초기화하고, one_time_init 으로 전역 자원을 준비하고, MMIO 영역을 예약해
 * ioremap 한다.
 *
 * **4단계 -- 정보를 모아 슬롯을 만든다.** get_slot_mapping 으로 첫 슬롯의 물리
 * 번호를 알아내고, cpqhp_save_config 로 이 버스의 설정공간을 통째로 저장하고,
 * cpqhp_find_available_resources 로 HRT 에서 자유 목록을 채우고,
 * ctrl_slot_setup 으로 슬롯을 만들어 등록한다.
 *
 * **5단계 -- 인터럽트를 열고 초기 상태를 맞춘다.** 먼저 모든 인터럽트를 막고
 * request_irq 로 처리기를 걸고, MISC 에 0x4006 을 세우고, 밀린 인터럽트를 모두
 * 지운 뒤 마스크를 풀어 준다. 마지막으로 카드가 꽂혀 있지 않은 슬롯의 전원을
 * 꺼서 불필요한 전력을 아낀다. 그 사이 crit_sect 를 쥔다.
 *
 * **실패 경로가 여덟 단계로 나뉜다** -- err_free_irq, err_iounmap,
 * err_free_mem_region, err_free_bus, err_free_ctrl, err_disable_device.
 * 잡은 순서의 정확히 역순으로 놓아 준다. 이 드라이버가 오래된 코드임에도
 * 정리 경로만큼은 잘 짜여 있는 자리다.
 *
 * **다만 성공한 뒤 이 컨트롤러를 뽑는 경로는 없다** -- .remove 가 주석 처리되어
 * 있어 모듈을 내릴 때 unload_cpqphpd 가 한꺼번에 정리하는 것이 전부다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(PCI 코어의 probe). 여러 곳에서 잠들 수 있다.
 *
 * 호출 체인:
 *   PCI 코어 → [이 함수]
 *     → one_time_init, get_slot_mapping, cpqhp_save_config,
 *       cpqhp_find_available_resources, ctrl_slot_setup, request_irq, init_SERR
 */
static int cpqhpc_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	/* [한국어] 마무리 단계에서 빈 슬롯의 전원을 끌 때 쓰는 반복 변수 */
	u8 num_of_slots = 0;
	/* [한국어] 컨트롤러 안 슬롯 번호(0부터). 레지스터 비트 자리로 쓴다 */
	u8 hp_slot = 0;
	/* [한국어] 마무리 단계에서 훑을 PCI 장치 번호 */
	u8 device;
	/* [한국어] Compaq CIOBX 기종에서 설정공간 0x41 에서 읽는 버스 능력 바이트 */
	u8 bus_cap;
	/* [한국어] MISC 레지스터 값과 감지선 상태를 번갈아 담는 임시 변수 */
	u16 temp_word;
	/* [한국어] **컨트롤러 칩의 벤더 ID. Compaq 인지 Intel 인지가 이 값으로 갈린다** */
	u16 vendor_id;
	/* [한국어] 서브시스템 벤더 ID. **능력 해석 방식을 정하는 갈래의 열쇠다** */
	u16 subsystem_vid;
	/* [한국어] 서브시스템 장치 ID.
	 * **Compaq 에서는 기종 하나를 뜻하고 Intel 에서는 비트마다 기능 하나를 뜻한다** */
	u16 subsystem_deviceid;
	/* [한국어] 오류 코드. **u32(부호 없음)인데 음수 오류 코드를 담는다** --
	 * 호출자에게 int 로 돌아가므로 결과는 맞지만 타입이 어긋난 자리다 */
	u32 rc;
	/* [한국어] 이 함수가 만들어 채울 컨트롤러 구조체 */
	struct controller *ctrl;
	/* [한국어] 마무리 단계에서 슬롯별 함수 노드를 잡을 자리 */
	struct pci_func *func;
	/* [한국어] **컨트롤러 뒤(subordinate)의 버스.**
	 * 슬롯들이 실제로 붙어 있는 버스이며, 속도 정보를 여기에 기록한다 */
	struct pci_bus *bus;
	/* [한국어] pci_enable_device 의 반환값만 담는다. rc 와 따로 두는 이유는
	 * **err 이라는 이름이 로그 매크로 err() 와 겹치기 때문으로 보이나**,
	 * 지역 변수가 매크로를 가리지는 않으므로 실제 충돌은 없다 */
	int err;

	/* [한국어] **컨트롤러 장치를 깨운다.** 전원을 넣고 IO/메모리 디코딩을 켠다 */
	err = pci_enable_device(pdev);
	/* [한국어] 깨우기 실패 */
	if (err) {
		/* [한국어] **이 파일에서 유일하게 printk 를 직접 쓰는 자리다.**
		 * 다른 곳은 모두 cpqphp.h 의 err/info/dbg 매크로를 쓴다 */
		printk(KERN_ERR MY_NAME ": cannot enable PCI device %s (%d)\n",
			pci_name(pdev), err);
		/* [한국어] **아직 아무것도 잡지 않았으므로 곧바로 나간다** --
		 * 되돌릴 것이 없어 goto 라벨을 쓰지 않는다 */
		return err;
	}

	/* [한국어] **컨트롤러 뒤의 버스를 꺼낸다.**
	 * 핫플러그 컨트롤러는 PCI 브리지라 자기 뒤에 버스를 거느린다 */
	bus = pdev->subordinate;
	/* [한국어] 뒤에 버스가 없으면 브리지가 아니다 */
	if (!bus) {
		/* [한국어] 이 장치는 맡지 않는다는 것을 알린다 */
		pci_notice(pdev, "the device is not a bridge, skipping\n");
		/* [한국어] 장치 없음으로 알린다 */
		rc = -ENODEV;
		/* [한국어] pci_enable_device 를 되돌려야 한다 */
		goto err_disable_device;
	}

	/* Need to read VID early b/c it's used to differentiate CPQ and INTC
	 * discovery
	 */
	/* [한국어] **벤더 ID 를 일찍 읽는다.**
	 * 원문 주석이 밝히듯 Compaq(CPQ)과 Intel(INTC)의 탐색 방식이 달라
	 * 이 값이 뒤의 모든 갈래를 가른다 */
	vendor_id = pdev->vendor;
	/* [한국어] **Compaq(0x0E11)도 Intel(0x8086)도 아니면 다룰 수 없다.**
	 * 표(hpcd_pci_tbl)가 클래스 코드만 보고 걸러 오므로 다른 벤더의
	 * 핫플러그 컨트롤러도 여기까지 온다 */
	if ((vendor_id != PCI_VENDOR_ID_COMPAQ) &&
	    (vendor_id != PCI_VENDOR_ID_INTEL)) {
		/* [한국어] 미리 정의해 둔 문자열로 거절 사유를 남긴다 */
		err(msg_HPC_non_compaq_or_intel);
		/* [한국어] 장치 없음으로 알린다 */
		rc = -ENODEV;
		/* [한국어] 되돌리고 나간다 */
		goto err_disable_device;
	}
	/* [한국어] 통과한 벤더 ID 를 찍는다 */
	dbg("Vendor ID: %x\n", vendor_id);

	/* [한국어] 칩 리비전을 찍는다. 아래 검사의 근거가 되는 값이다 */
	dbg("revision: %d\n", pdev->revision);
	/* [한국어] **Compaq 인데 리비전이 0 이면 거절한다.**
	 * 원문 주석이 밝히듯 Intel 컨트롤러는 리비전 0 일 수 있으므로
	 * 이 검사는 Compaq 에만 건다 */
	if ((vendor_id == PCI_VENDOR_ID_COMPAQ) && (!pdev->revision)) {
		/* [한국어] 지원하지 않는 리비전임을 남긴다 */
		err(msg_HPC_rev_error);
		/* [한국어] 장치 없음으로 알린다 */
		rc = -ENODEV;
		/* [한국어] 되돌리고 나간다 */
		goto err_disable_device;
	}

	/* Check for the proper subsystem IDs
	 * Intel uses a different SSID programming model than Compaq.
	 * For Intel, each SSID bit identifies a PHP capability.
	 * Also Intel HPCs may have RID=0.
	 */
	/* [한국어] **리비전 2 이하의 Compaq 컨트롤러는 이 드라이버가 다루지 않는다.**
	 * 초기 하드웨어라 레지스터 배치가 달랐던 것으로 보이나,
	 * 그 문서는 이 트리에 없다. 원문 주석 넷째 줄이 Intel 은 리비전 0 도
	 * 있을 수 있다고 밝히므로 Intel 을 예외로 둔다 */
	if ((pdev->revision <= 2) && (vendor_id != PCI_VENDOR_ID_INTEL)) {
		/* [한국어] 이 시스템을 지원하지 않는다는 문구를 남긴다 */
		err(msg_HPC_not_supported);
		/* [한국어] 장치 없음으로 알린다 */
		rc = -ENODEV;
		/* [한국어] 되돌리고 나간다 */
		goto err_disable_device;
	}

	/* TODO: This code can be made to support non-Compaq or Intel
	 * subsystem IDs
	 */
	/* [한국어] **서브시스템 벤더 ID 를 읽는다.**
	 * 아래 switch 의 갈래를 정하는 값이며, 원문 TODO 는 언젠가 다른 벤더도
	 * 지원할 수 있다고 적어 두었다 */
	subsystem_vid = pdev->subsystem_vendor;
	/* [한국어] 읽은 값을 찍는다 */
	dbg("Subsystem Vendor ID: %x\n", subsystem_vid);
	/* [한국어] **서브시스템 벤더도 두 벤더 중 하나여야 한다.**
	 * 칩은 Compaq 이 만들었어도 보드는 다른 회사일 수 있기 때문이다 */
	if ((subsystem_vid != PCI_VENDOR_ID_COMPAQ) && (subsystem_vid != PCI_VENDOR_ID_INTEL)) {
		/* [한국어] 거절 사유를 남긴다 */
		err(msg_HPC_non_compaq_or_intel);
		/* [한국어] 장치 없음으로 알린다 */
		rc = -ENODEV;
		/* [한국어] 되돌리고 나간다 */
		goto err_disable_device;
	}

	/* [한국어] **컨트롤러 구조체를 0 으로 채워 만든다.**
	 * 여기부터 실패하면 이 메모리도 되돌려야 하므로 라벨이 하나 늘어난다 */
	ctrl = kzalloc_obj(struct controller);
	/* [한국어] 메모리 부족 */
	if (!ctrl) {
		/* [한국어] 메모리 부족으로 알린다 */
		rc = -ENOMEM;
		/* [한국어] **ctrl 이 NULL 이므로 err_free_ctrl 이 아니라 이 라벨로 간다** */
		goto err_disable_device;
	}

	/* [한국어] **서브시스템 장치 ID 를 읽는다.**
	 * 두 벤더가 이 필드를 전혀 다르게 쓴다 -- 아래 switch 가 그 차이다 */
	subsystem_deviceid = pdev->subsystem_device;

	/* [한국어] **dbg 가 아니라 info 라 늘 찍힌다.**
	 * 기종을 가리는 열쇠라 언제나 보이게 한 것으로 보인다 */
	info("Hot Plug Subsystem Device ID: %x\n", subsystem_deviceid);

	/* Set Vendor ID, so it can be accessed later from other
	 * functions
	 */
	/* [한국어] **나중에 쓸 수 있도록 벤더 ID 를 구조체에 남긴다.**
	 * 원문 주석이 그 뜻을 밝히며, cpqhp_ctrl_intr 이 벤더에 따라
	 * 인터럽트 확인 방식을 달리할 때 이 값을 본다 */
	ctrl->vendor_id = vendor_id;

	/* [한국어] **여기서 두 벤더의 능력 해석이 갈린다.**
	 * Compaq 은 장치 ID 하나가 기종 하나, Intel 은 비트 하나가 기능 하나다 */
	switch (subsystem_vid) {
	/* [한국어] Compaq 보드 -- 기종별로 능력을 통째로 적어 둔 갈래 */
	case PCI_VENDOR_ID_COMPAQ:
		/* [한국어] **리비전 0x13 이상은 CIOBX 라는 별도 계열이라 장치 ID 를 보지 않는다.**
		 * 원문 주석의 CIOBX 는 Compaq 의 IO 브리지 칩 이름으로 보이나,
		 * 그 문서는 이 트리에 없다 */
		if (pdev->revision >= 0x13) { /* CIOBX */
			/* [한국어] 버튼 처리 표시를 켠다.
			 * **읽어서 갈래를 나누는 코드는 이 드라이버에 없다** */
			ctrl->push_flag = 1;
			/* [한국어] 슬롯에 레버가 있다 */
			ctrl->slot_switch_type = 1;
			/* [한국어] 누름 버튼이 있다. **이 값이 5초 취소 창을 쓸지 정한다** */
			ctrl->push_button = 1;
			/* [한국어] 설정공간 인덱스/데이터 창을 지원한다고 기록해 둔다 */
			ctrl->pci_config_space = 1;
			/* [한국어] 핫플러그가 켜져 있다. **0 이면 이 컨트롤러를 쓰지 않는다** */
			ctrl->defeature_PHP = 1;
			/* [한국어] PCI-X 버스다 */
			ctrl->pcix_support = 1;
			/* [한국어] PCI-X 속도를 바꿀 수 있다 */
			ctrl->pcix_speed_capability = 1;
			/* [한국어] **설정공간 오프셋 0x41 에서 버스 능력 바이트를 읽는다.**
			 * 표준 설정공간 머리(0x00~0x3F) 밖이라 **벤더 고유 레지스터이며,
			 * 그 문서는 이 트리에 없다.** 아래 네 비트의 뜻은 코드가 붙인
			 * 로그 문구로만 알 수 있다 */
			pci_read_config_byte(pdev, 0x41, &bus_cap);
			/* [한국어] 비트 7 -- 133MHz PCI-X 까지 낼 수 있다 */
			if (bus_cap & 0x80) {
				/* [한국어] 판별 결과를 찍는다 */
				dbg("bus max supports 133MHz PCI-X\n");
				/* [한국어] **버스의 최대 속도를 기록한다.**
				 * 핫플러그 코어가 sysfs 로 이 값을 보여 준다 */
				bus->max_bus_speed = PCI_SPEED_133MHz_PCIX;
				/* [한국어] **switch 를 빠져나간다** -- 안쪽 if 가 아니라 바깥 switch 다 */
				break;
			}
			/* [한국어] 비트 6 -- 100MHz PCI-X */
			if (bus_cap & 0x40) {
				/* [한국어] 판별 결과를 찍는다 */
				dbg("bus max supports 100MHz PCI-X\n");
				/* [한국어] 최대 속도를 기록한다 */
				bus->max_bus_speed = PCI_SPEED_100MHz_PCIX;
				/* [한국어] switch 를 빠져나간다 */
				break;
			}
			/* [한국어] 비트 5 -- 66MHz PCI-X */
			if (bus_cap & 0x20) {
				/* [한국어] 판별 결과를 찍는다 */
				dbg("bus max supports 66MHz PCI-X\n");
				/* [한국어] 최대 속도를 기록한다 */
				bus->max_bus_speed = PCI_SPEED_66MHz_PCIX;
				/* [한국어] switch 를 빠져나간다 */
				break;
			}
			/* [한국어] 비트 4 -- 보통 PCI 66MHz */
			if (bus_cap & 0x10) {
				/* [한국어] 판별 결과를 찍는다 */
				dbg("bus max supports 66MHz PCI\n");
				/* [한국어] 최대 속도를 기록한다 */
				bus->max_bus_speed = PCI_SPEED_66MHz;
				/* [한국어] switch 를 빠져나간다 */
				break;
			}

			/* [한국어] **네 비트가 모두 0 이면 max_bus_speed 를 채우지 않은 채 나간다.**
			 * kzalloc 으로 만든 구조체가 아니라 커널이 준 pci_bus 이므로,
			 * 이때 그 필드에 무엇이 들어 있는지는 이 파일에서 알 수 없다 */
			break;
		}

		/* [한국어] **Compaq 은 서브시스템 장치 ID 하나가 기종 하나를 뜻한다.**
		 * 그래서 기종마다 능력 일곱 개를 통째로 적어 둔다.
		 * 아래 Intel 갈래가 비트마다 if 를 쓰는 것과 정반대의 방식이다 */
		switch (subsystem_deviceid) {
		/* [한국어] **0xA2F7 -- 6500/7000 계열의 처음 구현.**
		 * 원문 주석: Original 6500/7000 implementation */
		case PCI_SUB_HPC_ID:
			/* Original 6500/7000 implementation */
			/* [한국어] 레버가 있다 */
			ctrl->slot_switch_type = 1;
			/* [한국어] 33MHz 버스다 */
			bus->max_bus_speed = PCI_SPEED_33MHz;
			/* [한국어] **버튼이 없다.** 이 기종은 sysfs 로만 조작한다 */
			ctrl->push_button = 0;
			/* [한국어] 설정공간 인덱스/데이터 창을 지원한다 */
			ctrl->pci_config_space = 1;
			/* [한국어] 핫플러그가 켜져 있다 */
			ctrl->defeature_PHP = 1;
			/* [한국어] PCI-X 가 아니다 */
			ctrl->pcix_support = 0;
			/* [한국어] 속도 조정도 없다 */
			ctrl->pcix_speed_capability = 0;
			/* [한국어] 이 기종의 설정 끝 */
			break;
		/* [한국어] **0xA2F8 -- 누름 버튼이 처음 들어간 기종.**
		 * 원문 주석: First Pushbutton implementation */
		case PCI_SUB_HPC_ID2:
			/* First Pushbutton implementation */
			/* [한국어] 버튼 처리 표시를 켠다 */
			ctrl->push_flag = 1;
			/* [한국어] 레버가 있다 */
			ctrl->slot_switch_type = 1;
			/* [한국어] 33MHz 버스다 */
			bus->max_bus_speed = PCI_SPEED_33MHz;
			/* [한국어] **버튼이 있다.** 여기서부터 5초 취소 창이 의미를 갖는다 */
			ctrl->push_button = 1;
			/* [한국어] 설정공간 창을 지원한다 */
			ctrl->pci_config_space = 1;
			/* [한국어] 핫플러그가 켜져 있다 */
			ctrl->defeature_PHP = 1;
			/* [한국어] PCI-X 가 아니다 */
			ctrl->pcix_support = 0;
			/* [한국어] 속도 조정이 없다 */
			ctrl->pcix_speed_capability = 0;
			/* [한국어] 이 기종의 설정 끝 */
			break;
		/* [한국어] **0xA2FA -- 제3자가 만든 6500/7000 계열.**
		 * 이름의 INTC 에도 불구하고 **Compaq 서브시스템 벤더 갈래 안에 있다.**
		 * 원문 주석: Third party (6500/7000) */
		case PCI_SUB_HPC_ID_INTC:
			/* Third party (6500/7000) */
			/* [한국어] 레버가 있다 */
			ctrl->slot_switch_type = 1;
			/* [한국어] 33MHz 버스다 */
			bus->max_bus_speed = PCI_SPEED_33MHz;
			/* [한국어] 버튼이 없다 */
			ctrl->push_button = 0;
			/* [한국어] 설정공간 창을 지원한다 */
			ctrl->pci_config_space = 1;
			/* [한국어] 핫플러그가 켜져 있다 */
			ctrl->defeature_PHP = 1;
			/* [한국어] PCI-X 가 아니다 */
			ctrl->pcix_support = 0;
			/* [한국어] 속도 조정이 없다 */
			ctrl->pcix_speed_capability = 0;
			/* [한국어] 이 기종의 설정 끝 */
			break;
		/* [한국어] **0xA2F9 -- 66MHz 가 처음 들어간 기종.**
		 * 원문 주석: First 66 Mhz implementation */
		case PCI_SUB_HPC_ID3:
			/* First 66 Mhz implementation */
			/* [한국어] 버튼 처리 표시를 켠다 */
			ctrl->push_flag = 1;
			/* [한국어] 레버가 있다 */
			ctrl->slot_switch_type = 1;
			/* [한국어] **66MHz 버스다.** 33MHz 에서 처음 올라간 세대다 */
			bus->max_bus_speed = PCI_SPEED_66MHz;
			/* [한국어] 버튼이 있다 */
			ctrl->push_button = 1;
			/* [한국어] 설정공간 창을 지원한다 */
			ctrl->pci_config_space = 1;
			/* [한국어] 핫플러그가 켜져 있다 */
			ctrl->defeature_PHP = 1;
			/* [한국어] 아직 PCI-X 는 아니다 */
			ctrl->pcix_support = 0;
			/* [한국어] 속도 조정이 없다 */
			ctrl->pcix_speed_capability = 0;
			/* [한국어] 이 기종의 설정 끝 */
			break;
		/* [한국어] **0xA2FD -- PCI-X 가 처음 들어간 기종, 100MHz.**
		 * 원문 주석: First PCI-X implementation, 100MHz */
		case PCI_SUB_HPC_ID4:
			/* First PCI-X implementation, 100MHz */
			/* [한국어] 버튼 처리 표시를 켠다 */
			ctrl->push_flag = 1;
			/* [한국어] 레버가 있다 */
			ctrl->slot_switch_type = 1;
			/* [한국어] **PCI-X 100MHz 버스다** */
			bus->max_bus_speed = PCI_SPEED_100MHz_PCIX;
			/* [한국어] 버튼이 있다 */
			ctrl->push_button = 1;
			/* [한국어] 설정공간 창을 지원한다 */
			ctrl->pci_config_space = 1;
			/* [한국어] 핫플러그가 켜져 있다 */
			ctrl->defeature_PHP = 1;
			/* [한국어] **PCI-X 버스다** */
			ctrl->pcix_support = 1;
			/* [한국어] **다만 속도를 바꿔 줄 수는 없다.**
			 * 두 값이 나뉜 이유가 이 기종에서 드러난다 -- PCI-X 이면서
			 * 속도 조정은 못 하는 조합이 실제로 있었다 */
			ctrl->pcix_speed_capability = 0;
			/* [한국어] 이 기종의 설정 끝 */
			break;
		/* [한국어] **아는 기종이 아니다** */
		default:
			/* [한국어] 지원하지 않는다는 문구를 남긴다 */
			err(msg_HPC_not_supported);
			/* [한국어] 장치 없음으로 알린다 */
			rc = -ENODEV;
			/* [한국어] **ctrl 을 이미 만들었으므로 그것도 해제하는 라벨로 간다** */
			goto err_free_ctrl;
		}
		/* [한국어] Compaq 갈래 전체의 끝 */
		break;

	/* [한국어] **Intel 보드 -- 서브시스템 장치 ID 의 비트 하나가 기능 하나다.**
	 * 같은 필드를 Compaq 은 기종 번호로, Intel 은 비트 모음으로 쓴다.
	 * 원문 주석이 위에서 그 차이를 밝힌다 */
	case PCI_VENDOR_ID_INTEL:
		/* Check for speed capability (0=33, 1=66) */
		/* [한국어] **비트 0 -- 버스 속도.** 원문 주석: 0=33, 1=66 */
		if (subsystem_deviceid & 0x0001)
			/* [한국어] 66MHz 버스로 기록한다 */
			bus->max_bus_speed = PCI_SPEED_66MHz;
		/* [한국어] 비트가 0 인 경우 */
		else
			/* [한국어] 33MHz 버스로 기록한다 */
			bus->max_bus_speed = PCI_SPEED_33MHz;

		/* Check for push button */
		/* [한국어] **비트 1 -- 버튼 유무. 뜻이 뒤집혀 있다** */
		if (subsystem_deviceid & 0x0002)
			/* [한국어] **비트가 서 있으면 버튼이 없다** */
			ctrl->push_button = 0;
		/* [한국어] 비트가 0 인 경우 */
		else
			/* [한국어] 버튼이 있다 */
			ctrl->push_button = 1;

		/* Check for slot switch type (0=mechanical, 1=not mechanical) */
		/* [한국어] **비트 2 -- 레버 종류.** 원문 주석: 0=mechanical, 1=not mechanical */
		if (subsystem_deviceid & 0x0004)
			/* [한국어] 기계식 레버가 아니라는 뜻이므로 0 으로 둔다 */
			ctrl->slot_switch_type = 0;
		/* [한국어] 비트가 0 인 경우 */
		else
			/* [한국어] 기계식 레버가 있다 */
			ctrl->slot_switch_type = 1;

		/* PHP Status (0=De-feature PHP, 1=Normal operation) */
		/* [한국어] **비트 3 -- 핫플러그 자체의 활성 여부** */
		if (subsystem_deviceid & 0x0008)
			/* [한국어] 핫플러그가 켜져 있다. 원문 주석: PHP supported */
			ctrl->defeature_PHP = 1;	/* PHP supported */
		/* [한국어] 비트가 0 인 경우 */
		else
			/* [한국어] 핫플러그가 꺼져 있다. 원문 주석: PHP not supported */
			ctrl->defeature_PHP = 0;	/* PHP not supported */

		/* Alternate Base Address Register Interface
		 * (0=not supported, 1=supported)
		 */
		/* [한국어] **비트 4 -- 대체 BAR 인터페이스 지원.**
		 * 원문 주석이 밝히듯 0 이면 미지원, 1 이면 지원이다 */
		if (subsystem_deviceid & 0x0010)
			/* [한국어] 지원한다고 기록한다.
			 * **읽어서 갈래를 나누는 코드는 이 드라이버에 없다** */
			ctrl->alternate_base_address = 1;
		/* [한국어] 비트가 0 인 경우 */
		else
			/* [한국어] 지원하지 않는다고 기록한다 */
			ctrl->alternate_base_address = 0;

		/* PCI Config Space Index (0=not supported, 1=supported) */
		/* [한국어] **비트 5 -- 설정공간 인덱스/데이터 창 지원** */
		if (subsystem_deviceid & 0x0020)
			/* [한국어] 지원한다고 기록한다.
			 * **이 값도 읽어서 갈래를 나누는 코드가 없다** */
			ctrl->pci_config_space = 1;
		/* [한국어] 비트가 0 인 경우 */
		else
			/* [한국어] 지원하지 않는다고 기록한다 */
			ctrl->pci_config_space = 0;

		/* PCI-X support */
		/* [한국어] **비트 7 -- PCI-X 지원 여부. 이 비트가 서야 비트 6 이 뜻을 갖는다** */
		if (subsystem_deviceid & 0x0080) {
			/* [한국어] PCI-X 버스로 기록한다 */
			ctrl->pcix_support = 1;
			/* [한국어] **비트 6 -- PCI-X 속도 등급.**
			 * 원문 주석 세 줄이 비트 7 과 비트 0 의 조합까지 설명한다 */
			if (subsystem_deviceid & 0x0040)
				/* 133MHz PCI-X if bit 7 is 1 */
				/* [한국어] 133MHz PCI-X 를 낼 수 있다 */
				ctrl->pcix_speed_capability = 1;
			/* [한국어] 비트 6 이 0 인 경우 */
			else
				/* 100MHz PCI-X if bit 7 is 1 and bit 0 is 0, */
				/* 66MHz PCI-X if bit 7 is 1 and bit 0 is 1 */
				/* [한국어] 133MHz 는 못 낸다.
				 * 원문 주석 두 줄이 비트 0 과 조합해 100MHz 인지 66MHz 인지를 밝힌다 */
				ctrl->pcix_speed_capability = 0;
		/* [한국어] **비트 7 이 0 -- 보통 PCI 다.** 원문 주석: Conventional PCI */
		} else {
			/* Conventional PCI */
			/* [한국어] PCI-X 가 아니라고 기록한다 */
			ctrl->pcix_support = 0;
			/* [한국어] 속도 조정도 없다 */
			ctrl->pcix_speed_capability = 0;
		}
		/* [한국어] Intel 갈래 전체의 끝 */
		break;

	/* [한국어] **Compaq 도 Intel 도 아닌 서브시스템 벤더.**
	 * 위에서 이미 걸렀으므로 실제로는 닿지 않는 자리다 */
	default:
		/* [한국어] 지원하지 않는다는 문구를 남긴다 */
		err(msg_HPC_not_supported);
		/* [한국어] 장치 없음으로 알린다 */
		rc = -ENODEV;
		/* [한국어] ctrl 을 해제하고 나간다 */
		goto err_free_ctrl;
	}

	/* Tell the user that we found one. */
	/* [한국어] **dbg 가 아니라 info 라 늘 찍힌다.**
	 * 원문 주석대로 사용자에게 컨트롤러를 찾았음을 알리는 줄이다 */
	info("Initializing the PCI hot plug controller residing on PCI bus %d\n",
					pdev->bus->number);

	/* [한국어] **아래 일곱 줄이 방금 정한 능력을 사람이 읽게 늘어놓는다.**
	 * 두 벤더의 갈래가 무엇을 채웠는지 한눈에 보게 하는 자리다 */
	dbg("Hotplug controller capabilities:\n");
	/* [한국어] 버스 최대 속도. **숫자 그대로 찍으므로 열거값을 알아야 읽을 수 있다** */
	dbg("    speed_capability       %d\n", bus->max_bus_speed);
	/* [한국어] 레버 유무를 글로 찍는다 */
	dbg("    slot_switch_type       %s\n", ctrl->slot_switch_type ?
					"switch present" : "no switch");
	/* [한국어] 핫플러그 활성 여부를 글로 찍는다 */
	dbg("    defeature_PHP          %s\n", ctrl->defeature_PHP ?
					"PHP supported" : "PHP not supported");
	/* [한국어] 대체 BAR 인터페이스 지원 여부 */
	dbg("    alternate_base_address %s\n", ctrl->alternate_base_address ?
					"supported" : "not supported");
	/* [한국어] 설정공간 창 지원 여부 */
	dbg("    pci_config_space       %s\n", ctrl->pci_config_space ?
					"supported" : "not supported");
	/* [한국어] PCI-X 속도 조정 가능 여부 */
	dbg("    pcix_speed_capability  %s\n", ctrl->pcix_speed_capability ?
					"supported" : "not supported");
	/* [한국어] PCI-X 버스 여부 */
	dbg("    pcix_support           %s\n", ctrl->pcix_support ?
					"supported" : "not supported");

	/* [한국어] **컨트롤러 자신의 pci_dev 를 기억해 둔다.**
	 * 슬롯에 꽂힌 카드가 아니라 컨트롤러 자체다 */
	ctrl->pci_dev = pdev;
	/* [한국어] **PCI 코어가 들고 있는 드라이버 데이터 자리에 ctrl 을 걸어 둔다.**
	 * .remove 콜백이 있다면 여기서 되찾았을 값이지만,
	 * **이 드라이버에는 .remove 가 없어 다시 꺼내는 곳이 없다** */
	pci_set_drvdata(pdev, ctrl);

	/* make our own copy of the pci bus structure,
	 * as we like tweaking it a lot */
	/* [한국어] **진짜 pci_bus 구조체를 통째로 복사한다.**
	 * 설정공간을 읽을 때 `bus->number` 를 갈아 끼워야 하는데 진짜를 건드리면
	 * 커널 전체가 깨지므로 사본을 만든다. 원문 주석이 as we like tweaking it
	 * a lot 이라고 그 뜻을 밝힌다.
	 * **지금 기준으로는 위험한 관용구이며**, 요즘은 도메인을 인지하는 API 를 쓴다 */
	ctrl->pci_bus = kmemdup(pdev->bus, sizeof(*ctrl->pci_bus), GFP_KERNEL);
	/* [한국어] 복사 실패 */
	if (!ctrl->pci_bus) {
		/* [한국어] 메모리 부족을 남긴다 */
		err("out of memory\n");
		/* [한국어] 메모리 부족으로 알린다 */
		rc = -ENOMEM;
		/* [한국어] ctrl 을 해제하고 나간다 */
		goto err_free_ctrl;
	}

	/* [한국어] **컨트롤러가 있는 버스 번호를 기록한다.**
	 * 슬롯들이 붙은 버스와 다를 수 있다 */
	ctrl->bus = pdev->bus->number;
	/* [한국어] 칩 리비전을 기록한다 */
	ctrl->rev = pdev->revision;
	/* [한국어] **컨트롤러 자신의 PCI 주소와 리비전을 찍는다.**
	 * PCI_SLOT 과 PCI_FUNC 이 devfn 을 장치·함수로 쪼개는 커널 매크로다 */
	dbg("bus device function rev: %d %d %d %d\n", ctrl->bus,
		PCI_SLOT(pdev->devfn), PCI_FUNC(pdev->devfn), ctrl->rev);

	/* [한국어] **레지스터 임계 구역 뮤텍스를 초기화한다.**
	 * 이 뒤로 set_SOGO 와 wait_for_ctrl_irq 의 짝이 이 락 안에서만 돈다 */
	mutex_init(&ctrl->crit_sect);
	/* [한국어] **wait_for_ctrl_irq 가 잠들 대기 큐를 초기화한다.**
	 * cpqhp_ctrl_intr 이 이 큐를 깨운다 */
	init_waitqueue_head(&ctrl->queue);

	/* initialize our threads if they haven't already been started up */
	/* [한국어] **전역 자원을 준비한다.** 두 번째 컨트롤러부터는 곧바로 돌아온다.
	 * 원문 주석의 threads 는 지금은 워크큐를 가리킨다 */
	rc = one_time_init();
	/* [한국어] 전역 초기화 실패 */
	if (rc)
		/* [한국어] pci_bus 사본부터 되돌린다 */
		goto err_free_bus;

	/* [한국어] 장치 포인터를 찍는다 */
	dbg("pdev = %p\n", pdev);
	/* [한국어] BAR0 의 시작 물리 주소를 찍는다.
	 * **64비트 주소일 수 있어 unsigned long long 으로 형변환한다** */
	dbg("pci resource start %llx\n", (unsigned long long)pci_resource_start(pdev, 0));
	/* [한국어] BAR0 의 길이를 찍는다 */
	dbg("pci resource len %llx\n", (unsigned long long)pci_resource_len(pdev, 0));

	/* [한국어] **BAR0 의 MMIO 영역을 커널에 예약한다.**
	 * 다른 드라이버가 같은 영역을 쓰지 못하게 막는 표시이며,
	 * MY_NAME 이 /proc/iomem 에 보이는 소유자 이름이 된다 */
	if (!request_mem_region(pci_resource_start(pdev, 0),
				pci_resource_len(pdev, 0), MY_NAME)) {
		/* [한국어] 예약 실패를 남긴다. 다른 드라이버가 이미 쥐고 있는 경우다 */
		err("cannot reserve MMIO region\n");
		/* [한국어] **자원 충돌인데 메모리 부족 코드를 쓴다** */
		rc = -ENOMEM;
		/* [한국어] 아직 매핑 전이므로 pci_bus 사본부터 되돌린다 */
		goto err_free_bus;
	}

	/* [한국어] **컨트롤러 레지스터를 커널 주소 공간에 매핑한다.**
	 * 이 뒤로 이 파일과 cpqphp_ctrl.c 의 모든 레지스터 접근이
	 * `ctrl->hpc_reg + <오프셋>` 형태로 이 주소를 쓴다 */
	ctrl->hpc_reg = ioremap(pci_resource_start(pdev, 0),
					pci_resource_len(pdev, 0));
	/* [한국어] 매핑 실패 */
	if (!ctrl->hpc_reg) {
		/* [한국어] 길이와 시작 주소를 함께 남긴다 */
		err("cannot remap MMIO region %llx @ %llx\n",
		    (unsigned long long)pci_resource_len(pdev, 0),
		    (unsigned long long)pci_resource_start(pdev, 0));
		/* [한국어] 장치 없음으로 알린다 */
		rc = -ENODEV;
		/* [한국어] **예약한 영역을 놓아야 하므로 그 라벨로 간다** */
		goto err_free_mem_region;
	}

	/* Check for 66Mhz operation */
	/* [한국어] **지금 버스가 실제로 어떤 속도로 돌고 있는지 읽어 기록한다.**
	 * 앞서 정한 max_bus_speed 는 낼 수 있는 최대이고 이쪽은 현재 값이다.
	 * 원문 주석: Check for 66Mhz operation.
	 * **여기서부터 아래 배너까지가 설정공간 저장 단계다** */
	bus->cur_bus_speed = get_controller_speed(ctrl);


	/********************************************************
	 *
	 *              Save configuration headers for this and
	 *              subordinate PCI buses
	 *
	 ********************************************************/

	/* find the physical slot number of the first hot plug slot */

	/* Get slot won't work for devices behind bridges, but
	 * in this case it will always be called for the "base"
	 * bus/dev/func of a slot.
	 * CS: this is leveraging the PCIIRQ routing code from the kernel
	 * (pci-pc.c: get_irq_routing_table) */
	/* [한국어] **첫 슬롯의 물리 번호를 $PIR 표에서 찾는다.**
	 * 둘째 인자로 SLOT_MASK 의 상위 니블, 곧 첫 슬롯의 PCI 장치 번호를 넘긴다.
	 * 결과는 ctrl->first_slot 에 담기며 ctrl_slot_setup 의 시작값이 된다.
	 * 원문 주석 네 줄이 브리지 뒤 장치에는 통하지 않는다는 것과,
	 * 커널의 $PIR 처리 코드를 빌려 쓴다는 것을 밝힌다 */
	rc = get_slot_mapping(ctrl->pci_bus, pdev->bus->number,
				(readb(ctrl->hpc_reg + SLOT_MASK) >> 4),
				&(ctrl->first_slot));
	/* [한국어] 찾은 번호와 결과를 함께 찍는다 */
	dbg("get_slot_mapping: first_slot = %d, returned = %d\n",
				ctrl->first_slot, rc);
	/* [한국어] **슬롯 번호를 못 찾은 경우** */
	if (rc) {
		/* [한국어] 초기화 실패 문구에 오류 값을 실어 남긴다 */
		err(msg_initialization_err, rc);
		/* [한국어] 매핑부터 되돌린다 */
		goto err_iounmap;
	}

	/* Store PCI Config Space for all devices on this bus */
	/* [한국어] **이 버스에 있는 모든 장치의 설정공간을 저장한다**(cpqphp_pci.c).
	 * 브리지를 만나면 그 뒤로 재귀한다. 셋째 인자로 SLOT_MASK 를 통째로
	 * 넘겨 어느 장치가 핫플러그 슬롯인지 가리게 한다.
	 * **여기서 만들어진 pci_func 노드들이 전역 cpqhp_slot_list 를 채운다.**
	 * 원문 주석: Store PCI Config Space for all devices on this bus */
	rc = cpqhp_save_config(ctrl, ctrl->bus, readb(ctrl->hpc_reg + SLOT_MASK));
	/* [한국어] 저장 실패 */
	if (rc) {
		/* [한국어] 실패 사유를 남긴다 */
		err("%s: unable to save PCI configuration data, error %d\n",
				__func__, rc);
		/* [한국어] 매핑부터 되돌린다 */
		goto err_iounmap;
	}

	/*
	 * Get IO, memory, and IRQ resources for new devices
	 */
	/* The next line is required for cpqhp_find_available_resources */
	/* [한국어] **커널이 배정한 IRQ 번호를 쓴다.**
	 * 원문 주석대로 아래 cpqhp_find_available_resources 가 이 값을 필요로 하므로
	 * 그보다 먼저 채워야 한다 */
	ctrl->interrupt = pdev->irq;
	/* [한국어] **IRQ 번호가 16보다 작으면 레거시 배치로 본다.**
	 * MPS(MultiProcessor Specification) 표를 전부 매핑하는 모드에서는
	 * IRQ 번호가 16 이상이 되기 때문이다 */
	if (ctrl->interrupt < 0x10) {
		/* [한국어] 전역에 레거시 표시를 남긴다.
		 * **읽어서 갈래를 나누는 코드를 이 트리에서 찾을 수 없다** */
		cpqhp_legacy_mode = 1;
		/* [한국어] **로그 문구와 조건이 어긋나 보인다** -- 조건은 IRQ 가 16 미만일 때인데
		 * 문구는 Full Table Mapped 모드라고 말한다. 코드는 손대지 않고 사실만 적는다 */
		dbg("System seems to be configured for Full Table Mapped MPS mode\n");
	}

	/* [한국어] 설정공간 IRQ 값을 담을 자리를 비운다 */
	ctrl->cfgspc_irq = 0;
	/* [한국어] **설정공간에 BIOS 가 적어 둔 IRQ 번호를 따로 읽는다.**
	 * 커널이 재배정하면 pdev->irq 와 달라질 수 있어 둘을 다 들고 있다.
	 * **다만 이 값을 읽어 갈래를 나누는 코드를 이 트리에서 찾을 수 없다** */
	pci_read_config_byte(pdev, PCI_INTERRUPT_LINE, &ctrl->cfgspc_irq);

	/* [한국어] **ROM 의 HRT 를 읽어 컨트롤러의 네 자유 목록을 채운다**(cpqphp_pci.c).
	 * 이 드라이버가 새 카드에 자원을 줄 수 있는 근거 전부가 여기서 나온다.
	 * 원문 주석: Get IO, memory, and IRQ resources for new devices */
	rc = cpqhp_find_available_resources(ctrl, cpqhp_rom_start);
	/* [한국어] **성공했으면 빈 슬롯에 새 카드 넣기를 지원한다.**
	 * 자원 목록이 없으면 새 카드에 줄 것이 없으므로 지원할 수 없다.
	 * cpqhp_process_SI 가 이 값이 0 이면 ADD_NOT_SUPPORTED 로 거절한다 */
	ctrl->add_support = !rc;
	/* [한국어] HRT 를 못 읽은 경우 */
	if (rc) {
		/* [한국어] 반환값을 찍는다 */
		dbg("cpqhp_find_available_resources = 0x%x\n", rc);
		/* [한국어] 실패 사유를 남긴다 */
		err("unable to locate PCI configuration resources for hot plug add.\n");
		/* [한국어] **바로 위에서 add_support 를 채웠지만 여기서 접는다** --
		 * 자원이 없으면 아예 이 컨트롤러를 등록하지 않는다 */
		goto err_iounmap;
	}

	/*
	 * Finish setting up the hot plug ctrl device
	 */
	/* [한국어] **첫 슬롯의 PCI 장치 번호를 기록한다.**
	 * 이 값이 컨트롤러 안 슬롯 번호와 PCI 장치 번호를 잇는 다리다 */
	ctrl->slot_device_offset = readb(ctrl->hpc_reg + SLOT_MASK) >> 4;
	/* [한국어] **로그 이름이 값과 어긋난다** -- 찍는 것은 슬롯 개수가 아니라
	 * 첫 슬롯의 장치 번호다. 코드는 손대지 않고 사실만 적는다 */
	dbg("NumSlots %d\n", ctrl->slot_device_offset);

	/* [한국어] **event_queue 링의 쓰기 위치를 0 으로 맞춘다.**
	 * 인터럽트를 열기 전에 반드시 해야 하는 초기화다 */
	ctrl->next_event = 0;

	/* Setup the slot information structures */
	/* [한국어] **슬롯을 만들어 핫플러그 코어에 등록한다.**
	 * 이 줄이 끝나면 sysfs 에 슬롯 디렉터리가 생긴다.
	 * 원문 주석: Setup the slot information structures */
	rc = ctrl_slot_setup(ctrl, smbios_start, smbios_table);
	/* [한국어] 슬롯 만들기 실패 */
	if (rc) {
		/* [한국어] **초기화 실패 문구에 상수 6 을 실어 남긴다** --
		 * 오류 코드가 아니라 단계 번호로 보이나 그 뜻이 코드에 적혀 있지 않다 */
		err(msg_initialization_err, 6);
		/* [한국어] **문구가 실제 실패와 어긋난다** -- 설정공간 저장이 아니라
		 * 슬롯 만들기가 실패한 자리다. 위 550줄 근처에서 복사해 온 흔적이다 */
		err("%s: unable to save PCI configuration data, error %d\n",
			__func__, rc);
		/* [한국어] 매핑부터 되돌린다 */
		goto err_iounmap;
	}

	/* Mask all general input interrupts */
	/* [한국어] **모든 인터럽트를 막는다.**
	 * 처리기를 걸기 전에 인터럽트가 들어오면 준비되지 않은 상태에서
	 * 처리하게 되므로 먼저 닫아 둔다. 원문 주석: Mask all general input interrupts */
	writel(0xFFFFFFFFL, ctrl->hpc_reg + INT_MASK);

	/* set up the interrupt */
	/* [한국어] 쓸 IRQ 번호를 찍는다 */
	dbg("HPC interrupt = %d\n", ctrl->interrupt);
	/* [한국어] **인터럽트 처리기를 건다.**
	 * IRQF_SHARED 는 이 IRQ 선을 다른 장치와 함께 쓴다는 뜻이며,
	 * 마지막 인자 ctrl 이 처리기에 넘어가 어느 컨트롤러인지 가려낸다.
	 * **이 순간부터 cpqhp_ctrl_intr 이 인터럽트 컨텍스트에서 돌 수 있다** */
	if (request_irq(ctrl->interrupt, cpqhp_ctrl_intr,
			IRQF_SHARED, MY_NAME, ctrl)) {
		/* [한국어] IRQ 를 못 얻었음을 남긴다 */
		err("Can't get irq %d for the hotplug pci controller\n",
			ctrl->interrupt);
		/* [한국어] 장치 없음으로 알린다 */
		rc = -ENODEV;
		/* [한국어] **슬롯은 이미 등록되었는데 되돌리지 않고 나간다** --
		 * ctrl_slot_setup 이 만든 슬롯을 해제하는 라벨이 없다.
		 * 코드는 손대지 않고 사실만 적는다 */
		goto err_iounmap;
	}

	/* Enable Shift Out interrupt and clear it, also enable SERR on power
	 * fault
	 */
	/* [한국어] MISC 레지스터를 읽어 고칠 준비를 한다 */
	temp_word = readw(ctrl->hpc_reg + MISC);
	/* [한국어] **세 비트를 세운다 -- 비트 14, 비트 2, 비트 1.**
	 * 원문 주석이 밝히는 대로 Shift Out 인터럽트를 켜고 지우며,
	 * 전원 이상 시 SERR 도 켜는 조합이다.
	 * **어느 비트가 어느 기능인지는 이 트리에서 확인할 수 없다** */
	temp_word |= 0x4006;
	/* [한국어] 고친 값을 되쓴다 */
	writew(temp_word, ctrl->hpc_reg + MISC);

	/* Changed 05/05/97 to clear all interrupts at start */
	/* [한국어] **밀려 있던 인터럽트 상태를 모두 지운다.**
	 * 이 레지스터는 쓰기로 지워지므로 전체 비트를 쓰면 전부 지워진다.
	 * 원문 주석의 날짜(05/05/97)가 이 코드의 나이를 말해 준다 */
	writel(0xFFFFFFFFL, ctrl->hpc_reg + INT_INPUT_CLEAR);

	/* [한국어] **지운 직후의 상태를 읽어 기준값으로 삼는다.**
	 * 아래 마무리 단계가 이 값에서 슬롯별 카드 유무와 레버 상태를 뽑아낸다.
	 * cpqhp_ctrl_intr 도 이 필드를 갱신하며 인터럽트와 공유한다 */
	ctrl->ctrl_int_comp = readl(ctrl->hpc_reg + INT_INPUT_CLEAR);

	/* [한국어] **마스크를 완전히 풀어 인터럽트를 연다.**
	 * 앞서 0xFFFFFFFF 로 막아 두었던 것을 여기서 되돌린다.
	 * 처리기가 걸리고 밀린 상태를 지운 뒤이므로 이제 받아도 안전하다 */
	writel(0x0L, ctrl->hpc_reg + INT_MASK);

	/* [한국어] 전역 컨트롤러 리스트가 비어 있는 경우 -- 첫 컨트롤러다 */
	if (!cpqhp_ctrl_list) {
		/* [한국어] 머리로 삼는다 */
		cpqhp_ctrl_list = ctrl;
		/* [한국어] 뒤가 없음을 표시한다 */
		ctrl->next = NULL;
	/* [한국어] 이미 컨트롤러가 있는 경우 */
	} else {
		/* [한국어] 기존 머리를 자기 뒤에 단다 */
		ctrl->next = cpqhp_ctrl_list;
		/* [한국어] **자기가 새 머리가 된다.**
		 * 두 갈래를 나눴지만 kzalloc 이 next 를 이미 0 으로 만들어 두므로
		 * 실제로는 else 쪽 두 줄만으로 충분한 자리다 */
		cpqhp_ctrl_list = ctrl;
	}

	/* turn off empty slots here unless command line option "ON" set
	 * Wait for exclusive access to hardware
	 */
	/* [한국어] **빈 슬롯의 전원을 끄는 동안 레지스터를 독점한다.**
	 * 원문 주석대로 명령줄로 ON 을 주지 않는 한 빈 슬롯은 꺼 둔다 */
	mutex_lock(&ctrl->crit_sect);

	/* [한국어] 하위 니블에서 슬롯 개수를 얻는다 */
	num_of_slots = readb(ctrl->hpc_reg + SLOT_MASK) & 0x0F;

	/* find first device number for the ctrl */
	/* [한국어] **상위 니블에서 첫 슬롯의 장치 번호를 얻는다.**
	 * 원문 주석: find first device number for the ctrl */
	device = readb(ctrl->hpc_reg + SLOT_MASK) >> 4;

	/* [한국어] 슬롯을 하나씩 훑는다 */
	while (num_of_slots) {
		/* [한국어] 남은 슬롯 수를 찍는다 */
		dbg("num_of_slots: %d\n", num_of_slots);
		/* [한국어] **이 장치 번호의 함수 0 노드를 찾는다.**
		 * cpqhp_save_config 가 앞서 만들어 둔 노드다 */
		func = cpqhp_slot_find(ctrl->bus, device, 0);
		/* [한국어] 노드가 없으면 이 자리는 핫플러그 슬롯이 아니다 */
		if (!func)
			/* [한국어] **반복 전체를 끝낸다** -- 남은 슬롯을 건너뛰지 않고 멈춘다.
			 * 장치 번호가 연속이라는 전제에서 나온 판단이다 */
			break;

		/* [한국어] 장치 번호를 컨트롤러 안 슬롯 번호로 바꾼다 */
		hp_slot = func->device - ctrl->slot_device_offset;
		/* [한국어] 바꾼 번호를 찍는다 */
		dbg("hp_slot: %d\n", hp_slot);

		/* We have to save the presence info for these slots */
		/* [한국어] **기준값의 상위 16비트에 감지선 정보가 있다.**
		 * 원문 주석: We have to save the presence info for these slots */
		temp_word = ctrl->ctrl_int_comp >> 16;
		/* [한국어] **첫 번째 감지선 비트를 저장한다.**
		 * 이 값이 있어야 나중에 인터럽트가 왔을 때 무엇이 달라졌는지 알 수 있다 */
		func->presence_save = (temp_word >> hp_slot) & 0x01;
		/* [한국어] **두 번째 감지선 비트를 같은 바이트의 다른 자리에 더한다.**
		 * `hp_slot + 7` 은 두 감지선 묶음 사이의 거리이며,
		 * cpqphp.h 의 get_presence_status 가 23 과 15 로 하는 일과 같은 계산이다 */
		func->presence_save |= (temp_word >> (hp_slot + 7)) & 0x02;

		/* [한국어] **하위 16비트에 레버 상태가 있다.**
		 * 비트가 서 있으면 레버가 열려 있다는 뜻이다 */
		if (ctrl->ctrl_int_comp & (0x1L << hp_slot))
			/* [한국어] 열림을 0 으로 저장한다 */
			func->switch_save = 0;
		/* [한국어] 비트가 0 인 경우 -- 레버가 닫혀 있다 */
		else
			/* [한국어] **닫힘을 0x10 으로 저장한다.**
			 * 0/1 이 아니라 0/0x10 인 것은 cpqphp_ctrl.c 가 이 값을
			 * 다른 비트들과 합쳐 다루기 때문으로 보이나, 그 배치가 코드에 적혀 있지 않다 */
			func->switch_save = 0x10;

		/* [한국어] **power_mode 가 꺼져 있으면 빈 슬롯의 전원을 끈다.**
		 * one_time_init 이 이 인자를 0 으로 덮어쓰므로 사실상 늘 참이다 */
		if (!power_mode)
			/* [한국어] **카드가 꽂혀 있지 않은 슬롯만 고른다.**
			 * is_a_board 는 cpqhp_save_config 가 설정공간을 읽어 카드가 있다고
			 * 판단했을 때 세워 둔 표시다 */
			if (!func->is_a_board) {
				/* [한국어] 녹색(전원) LED 를 끈다 */
				green_LED_off(ctrl, hp_slot);
				/* [한국어] **슬롯을 버스에서 떼어 낸다.**
				 * 빈 슬롯에 전원을 넣어 두면 전력만 쓰고 버스에 잡음을 실을 수 있다 */
				slot_disable(ctrl, hp_slot);
			}

		/* [한국어] 다음 장치 번호로 넘어간다 */
		device++;
		/* [한국어] 남은 슬롯 수를 줄인다 */
		num_of_slots--;
	}

	/* [한국어] 위에서 LED 와 활성 비트를 고쳤다면 */
	if (!power_mode) {
		/* [한국어] **모아 둔 변경을 하드웨어에 한 번에 반영한다.**
		 * 슬롯마다 반영하지 않고 반복이 끝난 뒤 한 번만 부르는 것이
		 * 이 컨트롤러의 명령 방식에 맞는다 */
		set_SOGO(ctrl);
		/* Wait for SOBS to be unset */
		/* [한국어] 반영이 끝나기를 기다린다. 원문 주석: Wait for SOBS to be unset */
		wait_for_ctrl_irq(ctrl);
	}

	/* [한국어] **슬롯별 SERR 보고를 켠다.**
	 * 예고 없이 레버가 열리는 것 같은 사고를 잡아내기 위함이다 */
	rc = init_SERR(ctrl);
	/* [한국어] SERR 설정 실패 */
	if (rc) {
		/* [한국어] 실패를 남긴다 */
		err("init_SERR failed\n");
		mutex_unlock(&ctrl->crit_sect);
		/* [한국어] **락을 놓은 뒤 라벨로 간다.**
		 * 바로 위 줄의 mutex_unlock 이 그 준비다 -- 락을 쥔 채 나가면
		 * 시스템이 멈춘다 */
		goto err_free_irq;
	}

	/* Done with exclusive hardware access */
	mutex_unlock(&ctrl->crit_sect);

	/* [한국어] **이 컨트롤러의 debugfs 파일을 만든다**(cpqphp_sysfs.c).
	 * sysfs 로는 드러내지 않는 레지스터 덤프를 볼 수 있게 한다 */
	cpqhp_create_debugfs_files(ctrl);

	return 0;

/* [한국어] **실패 라벨의 시작. 잡은 순서의 정확히 역순으로 놓는다.**
 * 인터럽트를 건 뒤에 실패했을 때 오는 자리다 */
err_free_irq:
	free_irq(ctrl->interrupt, ctrl);
/* [한국어] MMIO 매핑을 놓아야 할 때 오는 자리 */
err_iounmap:
	iounmap(ctrl->hpc_reg);
/* [한국어] 예약한 MMIO 영역을 돌려줘야 할 때 오는 자리 */
err_free_mem_region:
	release_mem_region(pci_resource_start(pdev, 0), pci_resource_len(pdev, 0));
/* [한국어] pci_bus 사본을 놓아야 할 때 오는 자리 */
err_free_bus:
	kfree(ctrl->pci_bus);
/* [한국어] 컨트롤러 구조체를 놓아야 할 때 오는 자리 */
err_free_ctrl:
	kfree(ctrl);
/* [한국어] **마지막 라벨.** pci_enable_device 를 되돌린다 */
err_disable_device:
	pci_disable_device(pdev);
	return rc;
}

/* [한국어]
 * unload_cpqphpd - 드라이버가 잡고 있던 모든 것을 되돌린다
 *
 * @return: 없음.
 *
 * **모듈을 내릴 때 단 한 번 불린다.** .remove 콜백이 없으므로 컨트롤러 정리와
 * 전역 목록 정리를 이 함수가 한꺼번에 맡는다.
 *
 * 세 부분으로 나뉜다.
 *
 * **첫째, 컨트롤러 목록을 훑는다.** 컨트롤러마다 하드웨어를 조용히 만들고
 * (SLOT_SERR 끄기, 살아 있는 슬롯만 빼고 인터럽트 막기, MISC 의 한 비트 끄기),
 * ctrl_slot_cleanup 으로 슬롯과 자원을 놓고, 네 자유 목록(io, mem, p_mem, bus)의
 * 노드를 하나씩 해제하고, pci_bus 사본과 컨트롤러 자신을 해제한다.
 *
 * **둘째, cpqhp_slot_list 256칸을 훑는다.** 칸마다 pci_func 연결 리스트를 걸으며
 * 역시 네 자원 목록을 해제하고 노드를 해제한다. **여기서 해제하는 것은 카드가
 * 쓰고 있던 자원 기록이며, 앞의 것은 아직 나눠 주지 않은 여유분이다** --
 * 같은 struct pci_resource 를 두 방향에서 쓰는 이 드라이버의 구조가 정리
 * 코드에서도 그대로 드러난다.
 *
 * **셋째, 사건 기구를 세우고 ROM 매핑을 놓는다.**
 *
 * **해제 관용구가 스물네 번 되풀이된다** -- `tres = res; res = res->next;
 * kfree(tres);` 세 줄이 목록마다 나온다. 지금이라면 함수 하나로 뺐을 자리다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(모듈 내리기).
 *
 * 호출 체인:
 *   module_exit → cpqhpc_cleanup → [이 함수]
 *     → compaq_nvram_store, ctrl_slot_cleanup, kfree, cpqhp_event_stop_thread,
 *       iounmap
 */
static void __exit unload_cpqphpd(void)
{
	/* [한국어] cpqhp_slot_list 를 걸을 때 지금 노드 */
	struct pci_func *next;
	/* [한국어] **해제 직전에 노드를 잡아 둘 자리.**
	 * 이름이 대문자로 시작하는 것이 이 파일에서 유일하다 -- 초기 코드의 흔적이다 */
	struct pci_func *TempSlot;
	/* [한국어] cpqhp_slot_list 256칸을 훑는 색인 */
	int loop;
	/* [한국어] read_slot_enable 의 결과를 담는다.
	 * **오류 코드가 아니라 슬롯 활성 비트 모음이다** */
	u32 rc;
	/* [한국어] 컨트롤러 리스트를 걸을 때 지금 노드 */
	struct controller *ctrl;
	/* [한국어] 해제 직전에 컨트롤러를 잡아 둘 자리 */
	struct controller *tctrl;
	/* [한국어] 자원 목록을 걸을 때 지금 노드 */
	struct pci_resource *res;
	/* [한국어] 해제 직전에 자원 노드를 잡아 둘 자리 */
	struct pci_resource *tres;

	/* [한국어] **Compaq NVRAM 에 현재 상태를 저장한다**(cpqphp_nvram.c).
	 * 다음 부팅 때 BIOS 가 이 정보를 읽어 슬롯 상태를 복원할 수 있게 한다.
	 * **ROM 을 놓기 전에 해야 하므로 맨 앞에 있다** */
	compaq_nvram_store(cpqhp_rom_start);

	/* [한국어] 컨트롤러 리스트의 머리에서 시작한다 */
	ctrl = cpqhp_ctrl_list;

	/* [한국어] 컨트롤러를 하나씩 정리한다 */
	while (ctrl) {
		/* [한국어] **MMIO 가 매핑되어 있을 때만 하드웨어를 만진다.**
		 * probe 중간에 실패한 컨트롤러는 리스트에 오르지 않으므로
		 * 실제로는 늘 참인 검사다 */
		if (ctrl->hpc_reg) {
			/* [한국어] MISC 레지스터 값을 담을 자리. **블록 안 선언이라 범위가 좁다** */
			u16 misc;
			/* [한국어] **여덟 슬롯의 활성 비트를 통째로 읽는다.**
			 * 아래 마스크 계산의 재료다 */
			rc = read_slot_enable(ctrl);

			/* [한국어] **SERR 보고를 끈다.**
			 * 드라이버가 사라진 뒤 오류 신호를 받아 줄 사람이 없기 때문이다 */
			writeb(0, ctrl->hpc_reg + SLOT_SERR);
			/* [한국어] **살아 있는 슬롯만 빼고 인터럽트를 막는다.**
			 * 0xFFFFFFC0 은 하위 6비트를 뺀 모든 비트를 세우는 마스크이고,
			 * `~rc` 는 꺼져 있는 슬롯의 비트를 세운다. 둘을 OR 하면
			 * **켜져 있는 슬롯의 하위 6비트만 열린 채로 남는다.**
			 * 왜 그 슬롯들만 남기는지는 코드가 밝히지 않으나,
			 * 드라이버 없이도 하드웨어가 최소한의 보호를 하도록 둔 것으로 보인다 */
			writel(0xFFFFFFC0L | ~rc, ctrl->hpc_reg + INT_MASK);

			/* [한국어] MISC 를 읽어 고칠 준비를 한다 */
			misc = readw(ctrl->hpc_reg + MISC);
			/* [한국어] **비트 1 을 지운다.**
			 * probe 에서 세운 0x4006 가운데 하나를 되돌리는 것이며,
			 * **세 비트 가운데 하나만 지우는 이유는 코드에 적혀 있지 않다** */
			misc &= 0xFFFD;
			/* [한국어] 고친 값을 되쓴다 */
			writew(misc, ctrl->hpc_reg + MISC);
		}

		/* [한국어] **슬롯 등록 해제, debugfs, 인터럽트, 매핑, 예약 영역을 한꺼번에 놓는다.**
		 * 반환값을 확인하지 않는다 -- 늘 0 이기 때문이다 */
		ctrl_slot_cleanup(ctrl);

		/* [한국어] **IO 자유 목록부터 해제한다.**
		 * 여기 남아 있는 것은 아직 아무 카드에도 주지 않은 여유분이다 */
		res = ctrl->io_head;
		/* [한국어] 목록 끝까지 걷는다 */
		while (res) {
			/* [한국어] **해제할 노드를 잡아 둔다** */
			tres = res;
			/* [한국어] **해제하기 전에 다음으로 먼저 옮긴다** */
			res = res->next;
			/* [한국어] 잡아 둔 노드를 해제한다.
			 * **이 세 줄 관용구가 이 함수에서 여덟 번 되풀이된다** --
			 * 지금이라면 함수 하나로 뺐을 자리다 */
			/* [한국어] 잡아 둔 IO 자원 노드를 해제한다. 위와 같은 세 줄 관용구다 */
			kfree(tres);
		}

		/* [한국어] 일반 메모리 자유 목록 */
		res = ctrl->mem_head;
		while (res) {
			tres = res;
			res = res->next;
			/* [한국어] 일반 메모리 자원 노드를 해제한다 */
			kfree(tres);
		}

		/* [한국어] prefetchable 메모리 자유 목록 */
		res = ctrl->p_mem_head;
		while (res) {
			tres = res;
			res = res->next;
			/* [한국어] prefetchable 메모리 자원 노드를 해제한다 */
			kfree(tres);
		}

		/* [한국어] 버스 번호 자유 목록 */
		res = ctrl->bus_head;
		while (res) {
			tres = res;
			res = res->next;
			/* [한국어] 버스 번호 자원 노드를 해제한다 */
			kfree(tres);
		}

		/* [한국어] **probe 가 kmemdup 으로 만든 pci_bus 사본을 놓는다.**
		 * 진짜 버스 구조체가 아니므로 마음대로 해제해도 된다 */
		kfree(ctrl->pci_bus);

		/* [한국어] 해제할 컨트롤러를 잡아 둔다 */
		tctrl = ctrl;
		/* [한국어] 다음 컨트롤러로 먼저 옮긴다 */
		ctrl = ctrl->next;
		/* [한국어] 컨트롤러 구조체를 해제한다 */
		kfree(tctrl);
	}

	/* [한국어] **여기부터는 전역 cpqhp_slot_list 를 훑는다.**
	 * 버스 번호 256칸을 모두 본다 */
	for (loop = 0; loop < 256; loop++) {
		/* [한국어] 이 버스의 함수 노드 리스트 머리 */
		next = cpqhp_slot_list[loop];
		/* [한국어] 리스트 끝까지 걷는다 */
		while (next != NULL) {
			/* [한국어] **이쪽 목록은 카드가 실제로 쓰고 있던 자원 기록이다.**
			 * 앞의 컨트롤러 목록이 여유분이었던 것과 방향이 반대다 */
			res = next->io_head;
			while (res) {
				tres = res;
				res = res->next;
				/* [한국어] 카드가 쓰던 IO 자원 기록을 해제한다 */
				kfree(tres);
			}

			/* [한국어] 카드가 쓰던 일반 메모리 기록 */
			res = next->mem_head;
			while (res) {
				tres = res;
				res = res->next;
				/* [한국어] 카드가 쓰던 일반 메모리 기록을 해제한다 */
				kfree(tres);
			}

			/* [한국어] 카드가 쓰던 prefetchable 메모리 기록 */
			res = next->p_mem_head;
			while (res) {
				tres = res;
				res = res->next;
				/* [한국어] 카드가 쓰던 prefetchable 메모리 기록을 해제한다 */
				kfree(tres);
			}

			/* [한국어] 카드 뒤에 붙었던 버스 번호 기록 */
			res = next->bus_head;
			while (res) {
				tres = res;
				res = res->next;
				/* [한국어] 카드 뒤에 붙었던 버스 번호 기록을 해제한다 */
				kfree(tres);
			}

			/* [한국어] 해제할 함수 노드를 잡아 둔다 */
			TempSlot = next;
			/* [한국어] 다음 노드로 먼저 옮긴다 */
			next = next->next;
			/* [한국어] 함수 노드를 해제한다.
			 * **칸의 머리 포인터는 NULL 로 되돌리지 않는다** -- 모듈이 사라지므로
			 * 남은 포인터를 볼 사람이 없다고 본 것이다 */
			kfree(TempSlot);
		}
	}

	/* Stop the notification mechanism */
	/* [한국어] **one_time_init 이 실제로 돌았을 때만 세운다.**
	 * 원문 주석: Stop the notification mechanism */
	if (initialized)
		/* [한국어] 사건 처리 기구를 세운다(cpqphp_ctrl.c) */
		cpqhp_event_stop_thread();

	/* unmap the rom address */
	/* [한국어] ROM 을 매핑했다면. 원문 주석: unmap the rom address */
	if (cpqhp_rom_start)
		/* [한국어] ROM 매핑을 놓는다 */
		iounmap(cpqhp_rom_start);
	/* [한국어] SMBIOS 표 본문을 매핑했다면 */
	if (smbios_start)
		/* [한국어] **표 본문 매핑을 놓는다.**
		 * **$PIR 표(cpqhp_routing_table)는 여기서 kfree 되지 않는다** --
		 * 모듈이 내려가도 그 메모리가 남는다. 코드는 손대지 않고 사실만 적는다 */
		iounmap(smbios_start);
}

/* [한국어] **이 드라이버가 맡을 장치를 알려 주는 표.**
 * PCI 코어가 이 표와 맞는 장치를 찾을 때마다 cpqhpc_probe 를 부른다.
 * **벤더나 장치 ID 가 아니라 클래스 코드 하나로만 고른다** --
 * 어떤 회사가 만들었든 "PCI 핫플러그 컨트롤러" 이면 일단 받아 보고,
 * 벤더 검사는 probe 안에서 한다. 원문 주석 두 줄이 그 뜻을 밝힌다 */
static const struct pci_device_id hpcd_pci_tbl[] = {
	{
	/* handle any PCI Hotplug controller */
	/* [한국어] **클래스 코드 0x0C0600 을 만든다.**
	 * PCI_CLASS_SYSTEM_PCI_HOTPLUG 가 상위 두 바이트(0x0C06)이고
	 * 하위 한 바이트 0x00 이 프로그래밍 인터페이스다.
	 * 8비트 왼쪽으로 미는 것은 설정공간의 클래스 코드가 24비트이기 때문이다 */
	.class =        ((PCI_CLASS_SYSTEM_PCI_HOTPLUG << 8) | 0x00),
	/* [한국어] **클래스 코드 전체를 정확히 비교한다.**
	 * 마스크의 모든 비트가 서 있으므로 프로그래밍 인터페이스까지 같아야 한다 */
	.class_mask =   ~0,

	/* no matter who makes it */
	/* [한국어] 어떤 벤더라도 좋다. 원문 주석: no matter who makes it */
	.vendor =       PCI_ANY_ID,
	/* [한국어] 어떤 장치 ID 라도 좋다 */
	.device =       PCI_ANY_ID,
	/* [한국어] 어떤 서브시스템 벤더라도 좋다 */
	.subvendor =    PCI_ANY_ID,
	/* [한국어] 어떤 서브시스템 장치 ID 라도 좋다.
	 * **넷을 모두 열어 두었으므로 실제 선별은 probe 가 한다** */
	.subdevice =    PCI_ANY_ID,

	/* [한국어] **표의 끝을 나타내는 빈 항목.**
	 * 모든 필드가 0 인 항목이 종결자 노릇을 한다 -- PCI 코어가
	 * 항목 수를 따로 받지 않고 이것을 만나면 멈춘다 */
	}, { /* end: all zeroes */ }
};

/* [한국어] **이 표를 모듈 바이너리에 새겨 넣는다.**
 * depmod 가 그것을 읽어 modules.alias 를 만들고,
 * udev 가 장치를 발견했을 때 이 모듈을 자동으로 올릴 수 있게 된다 */
MODULE_DEVICE_TABLE(pci, hpcd_pci_tbl);

/* [한국어] **PCI 코어에 등록할 드라이버 구조체.**
 * cpqhpc_init 이 pci_register_driver 로 이것을 넘긴다 */
static struct pci_driver cpqhpc_driver = {
	/* [한국어] /sys/bus/pci/drivers 아래에 보이는 이름 */
	.name =		"compaq_pci_hotplug",
	/* [한국어] 맡을 장치를 알려 주는 위의 표 */
	.id_table =	hpcd_pci_tbl,
	/* [한국어] **장치를 만났을 때 부를 함수.**
	 * 바로 아래 줄의 주석 처리된 remove 가 이 드라이버의 성격을 결정한다 --
	 * **컨트롤러를 뽑는 경로가 없어** 모듈을 내릴 때 한꺼번에 정리한다 */
	.probe =	cpqhpc_probe,
	/* remove:	cpqhpc_remove_one, */
};

/* [한국어]
 * cpqhpc_init - 모듈 진입점. PCI 드라이버를 등록한다
 *
 * @return: pci_register_driver 의 결과. 0 이면 성공.
 *
 * **모듈이 올라올 때 커널이 부르는 첫 함수다.** 하는 일은 셋뿐이다 --
 * 모듈 인자 debug 를 전역 cpqhp_debug 에 옮기고, debugfs 뿌리를 만들고,
 * PCI 드라이버를 등록한다.
 *
 * **실제 초기화는 여기서 하지 않는다.** pci_register_driver 가 이미 꽂혀 있는
 * 장치들을 훑어 cpqhpc_probe 를 부르고, 진짜 일은 거기서 일어난다.
 * 그래서 이 함수 자체는 짧다.
 *
 * debug 인자를 여기서 옮기는 것이 눈에 띈다 -- module_param 으로 sysfs 에
 * 0644 로 노출되어 있어 나중에 바꿀 수 있지만, cpqhp_debug 는 이 시점의 값을
 * 복사한 것이라 **나중에 debug 를 바꿔도 cpqhp_debug 는 따라 바뀌지 않는다.**
 *
 * **pci_register_driver 가 실패해도 debugfs 뿌리는 남는다** -- 되돌리는 코드가
 * 없다. 코드는 손대지 않고 사실만 적는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(insmod).
 *
 * 호출 체인:
 *   module_init → [이 함수] → cpqhp_initialize_debugfs, pci_register_driver
 */
static int __init cpqhpc_init(void)
{
	/* [한국어] pci_register_driver 의 결과를 담는다 */
	int result;

	/* [한국어] **모듈 인자를 전역으로 복사한다.**
	 * 이 뒤로 네 파일의 dbg 매크로가 이 전역을 본다.
	 * **복사본이라 나중에 sysfs 로 debug 를 바꿔도 따라 바뀌지 않는다** */
	cpqhp_debug = debug;

	/* [한국어] **드라이버 이름과 판 번호를 알린다.**
	 * 문자열 상수를 나란히 두면 컴파일러가 이어 붙이는 C 의 성질을 쓴다 */
	info(DRIVER_DESC " version: " DRIVER_VERSION "\n");
	/* [한국어] debugfs 뿌리 디렉터리를 만든다(cpqphp_sysfs.c) */
	cpqhp_initialize_debugfs();
	/* [한국어] **PCI 코어에 드라이버를 등록한다.**
	 * 이 호출 안에서 코어가 이미 꽂혀 있는 장치들을 훑어
	 * cpqhpc_probe 를 부르므로, **이 줄이 돌아올 때 이미 컨트롤러가
	 * 다 세워져 있다.**
	 * **실패해도 위에서 만든 debugfs 뿌리를 지우지 않는다** --
	 * 되돌리는 코드가 없다 */
	result = pci_register_driver(&cpqhpc_driver);
	/* [한국어] 등록 결과를 찍는다 */
	dbg("pci_register_driver = %d\n", result);
	/* [한국어] 0 이 아니면 insmod 가 실패한다 */
	return result;
}

/* [한국어]
 * cpqhpc_cleanup - 모듈 종료점. 자원을 놓고 드라이버를 뗀다
 *
 * @return: 없음.
 *
 * cpqhpc_init 의 짝이며 순서를 뒤집는다.
 *
 * **다만 순서에 눈여겨볼 점이 있다.** unload_cpqphpd 를 먼저 부르고 그다음
 * pci_unregister_driver 를 부른다. 즉 **드라이버를 코어에서 떼기 전에 이미
 * 컨트롤러 자료구조를 해제한다.** .remove 콜백이 없어 unregister 가 아무것도
 * 정리하지 않기 때문에 실제로는 문제가 되지 않지만, 보통의 드라이버라면
 * 반대 순서로 써야 하는 자리다.
 *
 * 마지막으로 debugfs 뿌리를 지운다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(rmmod).
 *
 * 호출 체인:
 *   module_exit → [이 함수]
 *     → unload_cpqphpd, pci_unregister_driver, cpqhp_shutdown_debugfs
 */
static void __exit cpqhpc_cleanup(void)
{
	/* [한국어] 정리를 시작한다는 것을 알린다 */
	dbg("unload_cpqphpd()\n");
	/* [한국어] **컨트롤러와 전역 목록을 모두 해제한다.**
	 * **드라이버를 코어에서 떼기 전에 부르는 것이 눈에 띈다** --
	 * 보통은 반대 순서지만 .remove 콜백이 없어 아래 unregister 가
	 * 아무것도 정리하지 않으므로 실제 문제는 없다 */
	unload_cpqphpd();

	/* [한국어] 드라이버를 뗀다는 것을 알린다 */
	dbg("pci_unregister_driver\n");
	/* [한국어] **PCI 코어에서 드라이버를 뗀다.**
	 * .remove 가 없으므로 코어가 부를 것이 없다 */
	pci_unregister_driver(&cpqhpc_driver);
	/* [한국어] debugfs 뿌리를 지운다 */
	cpqhp_shutdown_debugfs();
}

/* [한국어] **모듈이 올라올 때 cpqhpc_init 을 부르라고 커널에 알린다.**
 * 이 매크로가 특별한 섹션에 함수 포인터를 심어 두면
 * 모듈 로더가 그것을 찾아 부른다 */
module_init(cpqhpc_init);
/* [한국어] **모듈이 내려갈 때 cpqhpc_cleanup 을 부르라고 알린다.**
 * 두 함수에 붙은 __init 과 __exit 표시가 이 짝과 맞물려,
 * 쓰이지 않게 된 코드를 커널이 버릴 수 있게 한다 */
module_exit(cpqhpc_cleanup);
