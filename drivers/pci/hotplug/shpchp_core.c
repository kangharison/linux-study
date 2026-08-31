// SPDX-License-Identifier: GPL-2.0+
/*
 * Standard Hot Plug Controller Driver
 *
 * Copyright (C) 1995,2001 Compaq Computer Corporation
 * Copyright (C) 2001 Greg Kroah-Hartman (greg@kroah.com)
 * Copyright (C) 2001 IBM Corp.
 * Copyright (C) 2003-2004 Intel Corporation
 *
 * All rights reserved.
 *
 * Send feedback to <greg@kroah.com>, <kristen.c.accardi@intel.com>
 *
 */

/*
 * [한국어 설명] SHPC(Standard Hot-Plug Controller) 핫플러그 드라이버의 등록/슬롯 노출 계층 (shpchp_core.c)
 *
 * === 파일의 역할 ===
 * SHPC 는 PCI SIG 의 "PCI Standard Hot-Plug Controller and Subsystem
 * Specification" 이 정의한, PCIe 이전 세대의 PCI/PCI-X 브리지에 얹히는 표준
 * 핫플러그 컨트롤러다. 이 파일은 그 컨트롤러를 다루는 shpchp 모듈의 "얼굴"에
 * 해당하며, 네 가지 일을 한다. (1) 모듈 초기화/해제와 모듈 파라미터 정의,
 * (2) PCI 코어에 pci_driver 로 자신을 등록해 PCI-to-PCI 브리지 클래스
 * (PCI_CLASS_BRIDGE_PCI_NORMAL = 0x060400) 장치를 잡는 일, (3) probe 시
 * struct controller 를 할당하고 슬롯 개수만큼 struct slot 을 만들어
 * pci_hp_register() 로 핫플러그 코어에 올리는 일, (4) 핫플러그 코어가 sysfs
 * 접근을 되돌려줄 때 쓰는 hotplug_slot_ops 콜백 7개를 구현하는 일이다.
 * 이 파일 자체는 SHPC 레지스터를 한 번도 직접 읽고 쓰지 않는다 — 하드웨어
 * 접근은 전부 shpchp_hpc.c 에, 슬롯 상태 머신은 shpchp_ctrl.c 에 위임한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 리눅스 PCI 핫플러그는 "공용 코어 + 규격별 드라이버" 2층 구조다. 공용 코어인
 * drivers/pci/hotplug/pci_hotplug_core.c 가 /sys/bus/pci/slots/ 아래 sysfs 를
 * 만들고, 규격별 드라이버(pciehp = 네이티브 PCIe, shpchp = 이 파일, acpiphp =
 * 펌웨어 ACPI 이벤트, cpqphp/ibmphp = 벤더 전용)가 그 아래에 붙는다. 이 파일은
 * pciehp_core.c 와 정확히 대응하는 위치 — 즉 SHPC 규격 쪽의 등록/노출 계층이다.
 * 모듈 적재 시 흐름은 shpcd_init() → pci_register_driver() → (PCI 코어가 클래스
 * 매칭) → shpc_probe() → shpc_capable() 로 SHPC capability 확인 →
 * acpi_get_hp_hw_control_from_firmware() 로 펌웨어와 소유권 협상 → shpc_init()
 * (shpchp_hpc.c) 로 MMIO 매핑과 인터럽트 설치 → init_slots() 로 슬롯 객체 생성
 * → pci_hp_register() 로 sysfs 노출 이다. 실행 컨텍스트는 전부 프로세스
 * 컨텍스트(모듈 init 또는 PCI 코어의 driver probe 경로)이며, 인터럽트 문맥에서
 * 실행되는 코드는 이 파일에 없다.
 *
 * === 타 모듈과의 연결 ===
 * 이 파일이 의존하는 쪽: shpchp_hpc.c(shpc_init/shpchp_release_ctlr 및 모든
 * shpchp_get_ 계열 상태 조회, shpchp_set_attention_status), shpchp_ctrl.c
 * (shpchp_sysfs_enable_slot/shpchp_sysfs_disable_slot 슬롯 상태 머신,
 * shpchp_queue_pushbutton_work 지연 작업 핸들러), shpchp_sysfs.c
 * (shpchp_create_ctrl_files/shpchp_remove_ctrl_files 로 컨트롤러 단위
 * 디버그 파일), pci_hotplug_core.c(pci_hp_register/pci_hp_deregister),
 * drivers/pci/hotplug/acpi_pcihp.c(acpi_get_hp_hw_control_from_firmware).
 * 이 파일에 의존하는 쪽: shpchp_hpc.c 의 shpchp_release_ctlr() 이 여기 정의된
 * cleanup_slots() 를 부르고, 나머지 shpchp 파일들은 shpchp.h 를 통해 여기서
 * 정의한 전역 shpchp_poll_mode/shpchp_poll_time 을 읽는다. 데이터 흐름은
 * "SHPC 하드웨어 → shpchp_hpc.c 인터럽트 → shpchp_ctrl.c 상태 머신 →
 * struct slot 갱신 → 이 파일의 get_ 계열 콜백 → pci_hotplug_core.c → sysfs"
 * 방향과, 그 반대인 "sysfs 쓰기 → 이 파일의 enable_slot/disable_slot →
 * shpchp_ctrl.c → shpchp_hpc.c → 하드웨어" 방향 두 가지다. 공유 자료구조는
 * shpchp.h 의 struct controller(컨트롤러 1개 = 브리지 1개)와 struct slot
 * (물리 슬롯 1개)이며, controller->slot_list 로 연결된다.
 * 하위 장치 드라이버와의 관계는 한 방향이다. 이 드라이버가 슬롯을 켜면
 * shpchp_ctrl.c → shpchp_pci.c 의 shpchp_configure_device() 가 pci_scan_slot()
 * 과 pci_bus_add_devices() 를 돌리고, 그 결과로 슬롯에 꽂힌 장치의 드라이버
 * probe 가 일어난다(슬롯에 NVMe SSD 가 있었다면 그때 nvme_probe() 가 불린다).
 * 반대 방향은 없다 — 이 트리의 drivers/nvme 는 shpchp_ 로 시작하는 심볼을 단
 * 하나도 호출하지 않는다.
 *
 * === 주요 함수/구조체 요약 ===
 * - shpcd_init()/shpcd_cleanup() : module_init/module_exit 진입점. pci_driver
 *   등록과 해제만 한다.
 * - shpc_probe() : PCI 코어가 브리지를 물려줄 때 호출. SHPC 능력 확인 → 펌웨어
 *   소유권 협상 → controller 할당 → shpc_init() → init_slots() 순서로 올리고,
 *   성공하면 pdev->shpc_managed 를 1로 세워 "이 브리지는 shpchp 가 관리한다"고
 *   커널 전체에 알린다(pci-acpi.c 의 shpchp_is_native() 가 이 비트를 읽는다).
 * - shpc_remove() : probe 의 역순 해제. shpc_managed 를 먼저 0으로 내린다.
 * - init_slots()/cleanup_slots() : struct slot 배열 생성/파괴와 핫플러그 코어
 *   등록/해제. cleanup_slots() 는 shpchp_hpc.c 의 shpchp_release_ctlr() 에서도
 *   불리므로 non-static 이다.
 * - shpchp_hotplug_slot_ops : 핫플러그 코어가 sysfs 접근을 되돌려주는 콜백 표.
 *   여기 등록된 7개 함수가 이 파일의 나머지 절반이다.
 * - struct controller(shpchp.h) : 브리지 1개 = SHPC 컨트롤러 1개. num_slots,
 *   slot_device_offset, first_slot, slot_num_inc, creg(MMIO) 등을 담는다.
 * - struct slot(shpchp.h) : 물리 슬롯 1개. hotplug_slot 임베드, 상태 캐시
 *   (attention_save/presence_save/latch_save/pwr_save), 워크큐와 지연 작업.
 */

#include <linux/module.h>	/* [한국어] 모듈 등록 매크로(MODULE_AUTHOR/MODULE_DEVICE_TABLE/module_init 등)와 THIS_MODULE 을 쓰기 위해 필요 */
#include <linux/moduleparam.h>	/* [한국어] module_param()/MODULE_PARM_DESC() 로 shpchp_poll_mode, shpchp_poll_time 을 sysfs 노출하기 위해 필요 */
#include <linux/kernel.h>	/* [한국어] printk 계열, 컨테이너 매크로 등 커널 공통 정의를 쓰기 위해 필요 */
#include <linux/types.h>	/* [한국어] u8/u32 같은 고정폭 정수 타입을 쓰기 위해 필요 */
#include <linux/slab.h>	/* [한국어] kzalloc_obj()/kfree() 로 controller 와 slot 을 힙에서 잡기 위해 필요 */
#include <linux/pci.h>	/* [한국어] struct pci_dev, pci_register_driver(), pci_find_capability() 등 PCI 코어 API 를 쓰기 위해 필요 */
#include "shpchp.h"	/* [한국어] struct controller/struct slot, ctrl_dbg 계열 로그 매크로, shpchp_ 계열 내부 함수 선언을 가져오기 위해 필요 */

/* Global variables */
bool shpchp_poll_mode;	/* [한국어] 폴링 모드 전역 플래그. 설정자: module_param 으로 부팅/모듈 인자 또는 sysfs 쓰기(0644). 읽는 자: shpchp_hpc.c 가 인터럽트 대신 poll_timer 로 슬롯을 훑을지 결정할 때. 값 범위: false(인터럽트 사용, 기본) / true(폴링). 동기화: 단순 bool 읽기라 별도 락 없음 */
int shpchp_poll_time;	/* [한국어] 폴링 주기(초) 전역 값. 설정자: module_param(0644). 읽는 자: shpchp_hpc.c 의 폴링 타이머 재무장 코드. 값 범위: 0 이면 드라이버가 기본값을 쓰고, 1~60 이 유효 범위로 취급된다. 동기화: 타이머 재무장 시점에만 읽으므로 락 없음 */

#define DRIVER_VERSION	"0.4"	/* [한국어] 모듈 버전 문자열. MODULE_ 매크로에 쓰이며 modinfo 로 노출된다 */
#define DRIVER_AUTHOR	"Dan Zink <dan.zink@compaq.com>, Greg Kroah-Hartman <greg@kroah.com>, Dely Sy <dely.l.sy@intel.com>"	/* [한국어] 모듈 작성자 문자열. MODULE_AUTHOR() 에 그대로 들어간다 */
#define DRIVER_DESC	"Standard Hot Plug PCI Controller Driver"	/* [한국어] 모듈 설명 문자열. MODULE_DESCRIPTION() 에 그대로 들어간다 */

MODULE_AUTHOR(DRIVER_AUTHOR);	/* [한국어] modinfo author 필드 등록 — 커널 모듈 메타데이터 */
MODULE_DESCRIPTION(DRIVER_DESC);	/* [한국어] modinfo description 필드 등록 — 커널 모듈 메타데이터 */

module_param(shpchp_poll_mode, bool, 0644);	/* [한국어] shpchp_poll_mode 를 bool 파라미터로 노출. 퍼미션 0644 라 /sys/module/shpchp/parameters/ 아래에서 런타임 변경 가능 */
module_param(shpchp_poll_time, int, 0644);	/* [한국어] shpchp_poll_time 을 int 파라미터로 노출. 마찬가지로 런타임 변경 가능 */
MODULE_PARM_DESC(shpchp_poll_mode, "Using polling mechanism for hot-plug events or not");	/* [한국어] shpchp_poll_mode 파라미터 설명문 등록 — modinfo 에서 보인다 */
MODULE_PARM_DESC(shpchp_poll_time, "Polling mechanism frequency, in seconds");	/* [한국어] shpchp_poll_time 파라미터 설명문 등록 — modinfo 에서 보인다 */

#define SHPC_MODULE_NAME "shpchp"	/* [한국어] pci_driver.name 에 들어갈 드라이버 이름. /sys/bus/pci/drivers/shpchp 경로가 이 이름으로 만들어진다 */

static int set_attention_status(struct hotplug_slot *slot, u8 value);	/* [한국어] hotplug_slot_ops 표를 정의부보다 먼저 채우기 위한 전방 선언 — attention LED 제어 콜백 */
static int enable_slot(struct hotplug_slot *slot);	/* [한국어] 전방 선언 — sysfs 로 슬롯을 켜라는 요청을 받는 콜백 */
static int disable_slot(struct hotplug_slot *slot);	/* [한국어] 전방 선언 — sysfs 로 슬롯을 끄라는 요청을 받는 콜백 */
static int get_power_status(struct hotplug_slot *slot, u8 *value);	/* [한국어] 전방 선언 — 슬롯 전원 상태 조회 콜백. init_slots() 에서도 직접 부른다 */
static int get_attention_status(struct hotplug_slot *slot, u8 *value);	/* [한국어] 전방 선언 — attention LED 상태 조회 콜백. init_slots() 에서도 직접 부른다 */
static int get_latch_status(struct hotplug_slot *slot, u8 *value);	/* [한국어] 전방 선언 — MRL(Manually-operated Retention Latch, 수동 걸쇠) 상태 조회 콜백 */
static int get_adapter_status(struct hotplug_slot *slot, u8 *value);	/* [한국어] 전방 선언 — 어댑터(카드) 물리적 존재 여부 조회 콜백 */

static const struct hotplug_slot_ops shpchp_hotplug_slot_ops = {	/* [한국어] 핫플러그 코어가 sysfs 접근을 이 드라이버로 되돌릴 때 쓰는 콜백 표. const 이며 모든 슬롯이 이 한 인스턴스를 공유한다 */
	.set_attention_status =	set_attention_status,	/* [한국어] echo 로 attention 파일에 값을 쓰면 호출. 값 0=꺼짐 1=켜짐 2=깜빡임(SHPC 규격의 Attention Indicator 상태) */
	.enable_slot =		enable_slot,	/* [한국어] power 파일에 1 을 쓰면 호출 — 슬롯 전원 인가 + 장치 열거 요청 */
	.disable_slot =		disable_slot,	/* [한국어] power 파일에 0 을 쓰면 호출 — 장치 제거 + 슬롯 전원 차단 요청 */
	.get_power_status =	get_power_status,	/* [한국어] power 파일을 읽으면 호출 */
	.get_attention_status =	get_attention_status,	/* [한국어] attention 파일을 읽으면 호출 */
	.get_latch_status =	get_latch_status,	/* [한국어] latch 파일을 읽으면 호출 */
	.get_adapter_status =	get_adapter_status,	/* [한국어] adapter 파일을 읽으면 호출. hardware_test 와 reset_slot 은 SHPC 가 지원하지 않아 비워 둔다 */
};

/*
 * [한국어]
 * init_slots - 컨트롤러가 가진 물리 슬롯 개수만큼 slot 객체를 만들어 핫플러그 코어에 등록한다
 *
 * @ctrl: shpc_init() 이 이미 채워 둔 컨트롤러 서술자. num_slots, first_slot,
 *        slot_device_offset, slot_num_inc, pci_dev 가 유효해야 한다.
 * @return: 0 이면 모든 슬롯 등록 성공. 음수 errno(-ENOMEM 또는 pci_hp_register
 *          가 돌려준 값)이면 실패이며, 호출자 shpc_probe() 는 이를 받아
 *          err_out_release_ctlr 경로로 간다.
 *
 * SHPC 컨트롤러는 브리지 하나에 여러 물리 슬롯을 매단다. 커널이 사용자에게
 * 슬롯을 보여 주려면 슬롯마다 struct slot 을 만들고 그 안의 hotplug_slot 을
 * 핫플러그 코어에 등록해야 하는데, 그 일을 여기서 한다. 이 함수가 없으면
 * /sys/bus/pci/slots/ 아래에 아무것도 생기지 않아 사용자가 슬롯을 켜고 끌 방법이
 * 사라진다.
 *
 * 동작 과정은 슬롯마다 다음 순서다. (1) slot 객체 할당, (2) 논리 슬롯 번호
 * (hp_slot), 버스/장치 번호, 사람이 보는 물리 슬롯 번호 계산, (3) 푸시버튼 지연
 * 처리용 전용 워크큐 생성과 뮤텍스/지연작업 초기화, (4) 물리 슬롯 번호를 이름으로
 * pci_hp_register() 호출, (5) 하드웨어에서 전원/attention/걸쇠/카드존재 네 가지
 * 상태를 한 번 읽어 캐시에 저장, (6) 컨트롤러의 슬롯 리스트에 연결.
 *
 * 실행 컨텍스트는 프로세스 컨텍스트(PCI 코어의 probe 경로)이고, 이 시점에는 아직
 * 슬롯이 외부에 노출되지 않았거나 방금 노출된 상태라 동시 진입은 없다. 다만
 * pci_hp_register() 가 돌아온 직후부터는 sysfs 로 콜백이 들어올 수 있으므로,
 * 등록 이후에 만지는 필드는 상태 캐시(pwr_save 등)뿐이다.
 *
 * 에러 경로는 3단 라벨로 나뉜다. error_slotwq 는 워크큐까지 만든 뒤 실패한 경우,
 * error_slot 은 slot 만 할당한 경우, error 는 아무것도 못 잡은 경우다. 주의할
 * 점은 이 함수가 "이미 성공적으로 등록해 둔 이전 슬롯들"은 정리하지 않는다는
 * 것이다 — 그 정리는 호출자가 shpchp_release_ctlr() 안의 cleanup_slots() 로
 * 처리한다.
 *
 * 호출 체인:
 *   shpc_probe() → [init_slots] → pci_hp_register()(pci_hotplug_core.c),
 *                                  alloc_workqueue(), get_power_status() 외 3개
 */
static int init_slots(struct controller *ctrl)
{
	struct slot *slot;	/* [한국어] this 루프에서 새로 만드는 슬롯 객체 포인터 */
	struct hotplug_slot *hotplug_slot;	/* [한국어] slot 안에 임베드된 hotplug_slot 을 가리키는 축약 포인터. 핫플러그 코어에 넘기는 것은 이 주소다 */
	char name[SLOT_NAME_SIZE];	/* [한국어] 슬롯 이름 버퍼. SLOT_NAME_SIZE(=10) 바이트로, "%d" 형태의 물리 슬롯 번호가 들어간다 */
	int retval;	/* [한국어] 실패 시 호출자에게 돌려줄 음수 errno 를 담는 변수 */
	int i;	/* [한국어] 0..num_slots-1 을 도는 논리 슬롯 인덱스(hp_slot) */

	for (i = 0; i < ctrl->num_slots; i++) {	/* [한국어] shpc_init() 이 SHPC Slot Count 필드에서 읽어 채워 둔 슬롯 개수만큼 반복 */
		slot = kzalloc_obj(*slot);	/* [한국어] slot 객체를 0 으로 채워 할당. 실패 시 아래에서 -ENOMEM 으로 빠진다 */
		if (!slot) {	/* [한국어] 할당 실패 진입 — 이미 만든 슬롯들은 error 라벨을 지나 호출자(shpc_probe)가 cleanup 하지 않고 그대로 두므로, 실패한 슬롯 자신만 정리한다 */
			retval = -ENOMEM;	/* [한국어] 메모리 부족을 호출자에게 알릴 반환값 설정 */
			goto error;	/* [한국어] 이번 회차에는 아직 wq 도 slot 도 없으므로 아무것도 해제하지 않는 error 라벨로 점프 */
		}

		hotplug_slot = &slot->hotplug_slot;	/* [한국어] slot 안에 통째로 박혀 있는 hotplug_slot 의 주소를 잡아 둔다 — 별도 할당이 아니라 임베드 구조체다 */

		slot->hp_slot = i;	/* [한국어] 논리 슬롯 번호(0부터). SHPC 레지스터의 Slot N 을 지목할 때 쓰인다 */
		slot->ctrl = ctrl;	/* [한국어] 이 슬롯이 속한 컨트롤러 역참조 포인터 — 콜백에서 slot->ctrl 로 되짚어 온다 */
		slot->bus = ctrl->pci_dev->subordinate->number;	/* [한국어] 슬롯이 놓인 버스 번호. 컨트롤러 브리지의 세컨더리(subordinate) 버스 번호를 쓴다 */
		slot->device = ctrl->slot_device_offset + i;	/* [한국어] PCI 장치 번호 = 컨트롤러의 slot_device_offset 에 논리 인덱스를 더한 값. SHPC 규격상 슬롯들은 연속된 장치 번호를 차지한다 */
		slot->number = ctrl->first_slot + (ctrl->slot_num_inc * i);	/* [한국어] 사람이 보는 물리 슬롯 번호. first_slot 에서 시작해 slot_num_inc(+1 또는 -1) 만큼 증가/감소한다 — 섀시에 따라 번호가 거꾸로 매겨지기 때문 */

		slot->wq = alloc_workqueue("shpchp-%d", WQ_PERCPU, 0,	/* [한국어] 슬롯마다 전용 워크큐 생성. 푸시버튼 눌림 후 5초 지연 처리를 다른 슬롯과 독립적으로 돌리기 위해서다. WQ_PERCPU 는 특정 CPU 에 묶인 큐를 뜻한다 */
					   slot->number);	/* [한국어] alloc_workqueue 이름 포맷 "shpchp-%d" 에 채워 넣을 물리 슬롯 번호 인자 */
		if (!slot->wq) {	/* [한국어] 워크큐 생성 실패 진입 — 이미 할당한 slot 을 되돌려야 한다 */
			retval = -ENOMEM;	/* [한국어] 메모리 부족 반환값 설정 */
			goto error_slot;	/* [한국어] slot 만 kfree 하는 라벨로 점프(워크큐는 만들어지지 않았다) */
		}

		mutex_init(&slot->lock);	/* [한국어] 슬롯 상태 머신(shpchp_ctrl.c)이 slot->state 를 바꿀 때 쓰는 뮤텍스 초기화 */
		INIT_DELAYED_WORK(&slot->work, shpchp_queue_pushbutton_work);	/* [한국어] 푸시버튼 지연 작업 초기화. 핸들러는 shpchp_ctrl.c 의 shpchp_queue_pushbutton_work */

		/* register this slot with the hotplug pci core */
		snprintf(name, SLOT_NAME_SIZE, "%d", slot->number);	/* [한국어] 슬롯 이름을 물리 슬롯 번호 10진수 문자열로 만든다. 이 이름이 /sys/bus/pci/slots/ 아래 디렉터리 이름이 된다 */
		hotplug_slot->ops = &shpchp_hotplug_slot_ops;	/* [한국어] 콜백 표 연결 — 이 줄이 있어야 핫플러그 코어가 sysfs 접근을 이 파일로 되돌린다 */

		ctrl_dbg(ctrl, "Registering domain:bus:dev=%04x:%02x:%02x hp_slot=%x sun=%x slot_device_offset=%x\n",	/* [한국어] 등록 직전 상태를 디버그 로그로 남긴다(도메인:버스:장치, 논리 슬롯, _SUN 대응 물리 번호, 장치 번호 오프셋) */
			 pci_domain_nr(ctrl->pci_dev->subordinate),	/* [한국어] PCI 도메인 번호(세그먼트) 출력 인자 */
			 slot->bus, slot->device, slot->hp_slot, slot->number,	/* [한국어] 버스/장치/논리 슬롯/물리 슬롯 번호 출력 인자 */
			 ctrl->slot_device_offset);	/* [한국어] 컨트롤러의 슬롯 장치 번호 시작점 출력 인자 */
		retval = pci_hp_register(hotplug_slot,	/* [한국어] 핫플러그 코어에 슬롯 등록 — pci_hp_register 는 매크로로 THIS_MODULE 과 KBUILD_MODNAME 을 덧붙여 __pci_hp_register() 를 부른다 */
				ctrl->pci_dev->subordinate, slot->device, name);	/* [한국어] 슬롯이 붙을 버스(브리지의 세컨더리 버스), 장치 번호, sysfs 디렉터리 이름 */
		if (retval) {	/* [한국어] 등록 실패 진입 — 이름 충돌(-EBUSY)이나 메모리 부족 등 */
			ctrl_err(ctrl, "pci_hp_register failed with error %d\n",	/* [한국어] 실패 사유를 errno 와 함께 남긴다 */
				 retval);	/* [한국어] ctrl_err 의 두 번째 인자(반환된 음수 errno) */
			goto error_slotwq;	/* [한국어] 워크큐까지 만들어진 상태이므로 워크큐부터 되돌리는 라벨로 점프 */
		}

		get_power_status(hotplug_slot, &slot->pwr_save);	/* [한국어] 전원 상태를 하드웨어에서 한 번 읽어 캐시에 저장 — 나중에 하드웨어 읽기가 실패하면 이 값을 대신 돌려준다 */
		get_attention_status(hotplug_slot, &slot->attention_save);	/* [한국어] attention LED 상태를 캐시에 저장(같은 이유) */
		get_latch_status(hotplug_slot, &slot->latch_save);	/* [한국어] MRL 걸쇠 상태를 캐시에 저장(같은 이유) */
		get_adapter_status(hotplug_slot, &slot->presence_save);	/* [한국어] 어댑터 존재 여부를 캐시에 저장(같은 이유) */

		list_add(&slot->slot_list, &ctrl->slot_list);	/* [한국어] 완성된 슬롯을 컨트롤러의 슬롯 리스트에 연결. cleanup_slots() 가 이 리스트를 훑는다 */
	}

	return 0;	/* [한국어] 모든 슬롯이 성공적으로 올라갔음을 shpc_probe() 에 알린다 */
error_slotwq:	/* [한국어] 워크큐 생성 이후 실패한 경로가 들어오는 라벨 */
	destroy_workqueue(slot->wq);	/* [한국어] 이번 회차의 워크큐 파괴 — 대기 중 작업을 모두 흘려보낸 뒤 해제한다 */
error_slot:	/* [한국어] 슬롯 할당은 됐지만 워크큐 이전에 실패한 경로가 들어오는 라벨 */
	kfree(slot);	/* [한국어] 이번 회차의 슬롯 객체 해제 */
error:	/* [한국어] 슬롯 할당조차 실패한 경로가 들어오는 라벨 */
	return retval;	/* [한국어] 음수 errno 를 shpc_probe() 로 반환 — 호출자는 이를 받아 err_out_release_ctlr 로 간다 */
}

/*
 * [한국어]
 * cleanup_slots - 컨트롤러에 매달린 모든 슬롯을 핫플러그 코어에서 떼어 내고 해제한다
 *
 * @ctrl: 슬롯 리스트를 가진 컨트롤러 서술자.
 * @return: 없음. 실패할 수 있는 단계가 없다.
 *
 * init_slots() 의 정확한 역연산이다. 슬롯을 그냥 kfree 하면 sysfs 디렉터리가
 * 남아 해제된 메모리를 가리키게 되고, 워크큐에 남은 푸시버튼 작업이 나중에 깨어나
 * 사라진 슬롯을 건드린다. 그래서 "리스트에서 제거 → 지연작업 취소 → 워크큐 파괴 →
 * sysfs 등록 해제 → 메모리 해제" 순서를 지킨다.
 *
 * 실행 컨텍스트는 프로세스 컨텍스트다. destroy_workqueue() 가 잠들 수 있으므로
 * 아토믹 문맥에서는 부를 수 없다. static 이 아닌 이유는 shpchp_hpc.c 의
 * shpchp_release_ctlr() 도 이 함수를 부르기 때문이며, 선언은 shpchp.h 에 있다.
 *
 * 에러 경로는 없다. 리스트가 비어 있으면 아무 일도 하지 않고 돌아온다.
 *
 * 호출 체인:
 *   shpc_probe()(실패 경로) 또는 shpchp_release_ctlr()(shpchp_hpc.c)
 *     → [cleanup_slots] → cancel_delayed_work() / destroy_workqueue()
 *                          / pci_hp_deregister()(pci_hotplug_core.c) / kfree()
 */
void cleanup_slots(struct controller *ctrl)
{
	struct slot *slot, *next;	/* [한국어] 리스트를 훑으며 원소를 지우기 때문에 다음 원소를 미리 담아 둘 next 가 필요하다 */

	list_for_each_entry_safe(slot, next, &ctrl->slot_list, slot_list) {	/* [한국어] list_for_each_entry_safe 는 순회 중 현재 원소를 제거해도 안전하다 — 아래에서 실제로 kfree 하기 때문에 반드시 safe 버전이어야 한다 */
		list_del(&slot->slot_list);	/* [한국어] 컨트롤러 슬롯 리스트에서 먼저 떼어 낸다 */
		cancel_delayed_work(&slot->work);	/* [한국어] 아직 실행되지 않은 푸시버튼 지연 작업을 취소. 이미 실행 중인 작업은 기다리지 않는다 */
		destroy_workqueue(slot->wq);	/* [한국어] 슬롯 전용 워크큐 파괴 — destroy_workqueue 는 큐에 남은 작업을 모두 마친 뒤 돌아오므로, 위의 cancel 과 합쳐 작업이 사라진 슬롯을 참조하는 것을 막는다 */
		pci_hp_deregister(&slot->hotplug_slot);	/* [한국어] 핫플러그 코어에서 슬롯 등록 해제 — /sys/bus/pci/slots/ 아래 디렉터리가 사라진다 */
		kfree(slot);	/* [한국어] 슬롯 객체 해제 */
	}
}

/*
 * [한국어]
 * set_attention_status - hotplug_slot_ops.set_attention_status 구현. 슬롯의 황색 LED 를 켜거나 끄거나 깜빡이게 한다
 *
 * @hotplug_slot: 핫플러그 코어가 sysfs 쓰기를 받아 넘겨준 슬롯 핸들. 여기서
 *                container_of 로 바깥 struct slot 을 복원한다.
 * @status: 사용자가 attention 파일에 쓴 값. 0=꺼짐, 1=켜짐, 2=깜빡임 (SHPC
 *          규격의 Attention Indicator 상태값).
 * @return: 항상 0. SHPC 의 attention 설정에는 실패를 알릴 경로가 없다.
 *
 * 데이터센터에서 "어느 슬롯의 디스크를 뽑아야 하는가"를 눈으로 지목하기 위한
 * 기능이다. 값을 먼저 slot->attention_save 에 캐시한 뒤 하드웨어에 내려보내는데,
 * 이는 나중에 하드웨어 읽기가 실패하더라도 get_attention_status() 가 방금 쓴 값을
 * 되돌려줄 수 있게 하기 위해서다.
 *
 * 실행 컨텍스트는 sysfs write(2) 를 처리하는 프로세스 컨텍스트다. 서로 다른 슬롯의
 * 콜백은 동시에 들어올 수 있으나 각자 다른 struct slot 을 만지므로 충돌하지
 * 않는다. 같은 슬롯에 대한 동시 쓰기의 직렬화는 하지 않으며, 마지막 쓰기가 이긴다.
 *
 * 에러 경로: 없다. shpchp_set_attention_status() 내부에서 컨트롤러 명령이
 * 실패하면 그쪽에서 로그만 남긴다.
 *
 * 호출 체인:
 *   sysfs attention 쓰기 → pci_hotplug_core.c 의 store 핸들러
 *     → hotplug_slot_ops.set_attention_status → [set_attention_status]
 *     → shpchp_set_attention_status()(shpchp_hpc.c)
 */
/*
 * set_attention_status - Turns the Amber LED for a slot on, off or blink
 */
static int set_attention_status(struct hotplug_slot *hotplug_slot, u8 status)
{
	struct slot *slot = get_slot(hotplug_slot);	/* [한국어] sysfs 가 넘겨준 hotplug_slot 주소로부터 container_of 로 바깥 struct slot 을 복원한다 */

	ctrl_dbg(slot->ctrl, "%s: physical_slot = %s\n",	/* [한국어] 어느 물리 슬롯에 대한 요청인지 디버그 로그 */
		 __func__, slot_name(slot));	/* [한국어] 함수 이름과 슬롯 이름 출력 인자 */

	slot->attention_save = status;	/* [한국어] 하드웨어 쓰기가 실패하더라도 sysfs 읽기가 방금 쓴 값을 돌려줄 수 있도록 먼저 캐시에 저장 */
	shpchp_set_attention_status(slot, status);	/* [한국어] shpchp_hpc.c 로 내려가 SHPC Slot Register 의 Attention Indicator 필드를 갱신한다 */

	return 0;	/* [한국어] SHPC attention 설정은 실패를 보고하지 않는 설계라 항상 0(성공)을 돌려준다 */
}

/*
 * [한국어]
 * enable_slot - hotplug_slot_ops.enable_slot 구현. 슬롯에 전원을 넣고 꽂힌 카드를 열거한다
 *
 * @hotplug_slot: 핫플러그 코어가 넘겨준 슬롯 핸들.
 * @return: shpchp_sysfs_enable_slot() 의 반환값을 그대로 전달한다. 0 이면 성공,
 *          음수 errno 면 실패(슬롯이 이미 켜져 있음, 카드 없음, 전원 결함 등)이며
 *          sysfs write(2) 의 반환값으로 사용자에게 그대로 전달된다.
 *
 * 사용자가 /sys/bus/pci/slots/N/power 에 1 을 쓰면 결국 여기로 온다. 이 함수 자체는
 * 얇은 껍데기이며, 실제 일은 shpchp_ctrl.c 의 상태 머신이 한다 — 슬롯 상태 검사,
 * MRL 걸쇠와 카드 존재 확인, 전원 인가, 버스 속도 협상, 그리고 shpchp_pci.c 의
 * shpchp_configure_device() 를 통한 pci_scan_slot() 과 pci_bus_add_devices() 다.
 * 껍데기를 따로 두는 이유는 hotplug_slot_ops 의 시그니처(hotplug_slot 인자)와
 * 내부 상태 머신의 시그니처(struct slot 인자)가 다르기 때문이다.
 *
 * 실행 컨텍스트는 sysfs write(2) 프로세스 컨텍스트다. 아래 단계인
 * shpchp_sysfs_enable_slot() 이 슬롯 뮤텍스로 재진입을 막는다.
 *
 * 이 경로가 성공하면 pci_bus_add_devices() 가 새로 발견된 장치의 드라이버 probe 를
 * 부른다. 슬롯에 NVMe SSD 가 꽂혀 있었다면 그 결과로 nvme_probe() 가 실행되지만,
 * 그것은 PCI 코어를 거친 간접 효과이고 이 파일이 NVMe 를 직접 부르는 것은 아니다.
 *
 * 에러 경로: 하위 함수가 돌려준 음수 errno 를 그대로 위로 올린다.
 *
 * 호출 체인:
 *   sysfs power 쓰기(1) → pci_hotplug_core.c 의 store 핸들러
 *     → hotplug_slot_ops.enable_slot → [enable_slot]
 *     → shpchp_sysfs_enable_slot()(shpchp_ctrl.c)
 */
static int enable_slot(struct hotplug_slot *hotplug_slot)
{
	struct slot *slot = get_slot(hotplug_slot);	/* [한국어] container_of 로 hotplug_slot 에서 struct slot 복원 */

	ctrl_dbg(slot->ctrl, "%s: physical_slot = %s\n",	/* [한국어] 어느 슬롯을 켜라는 요청인지 디버그 로그 */
		 __func__, slot_name(slot));	/* [한국어] 함수 이름과 슬롯 이름 출력 인자 */

	return shpchp_sysfs_enable_slot(slot);	/* [한국어] shpchp_ctrl.c 의 상태 머신으로 위임 — 슬롯 상태 검사, 전원 인가, shpchp_configure_device() 를 통한 버스 재스캔까지 거기서 처리한다 */
}

/*
 * [한국어]
 * disable_slot - hotplug_slot_ops.disable_slot 구현. 슬롯의 장치를 제거하고 전원을 내린다
 *
 * @hotplug_slot: 핫플러그 코어가 넘겨준 슬롯 핸들.
 * @return: shpchp_sysfs_disable_slot() 의 반환값. 0 이면 성공, 음수 errno 면
 *          실패(이미 꺼져 있음, 상태 머신이 다른 작업 중 등)이고 sysfs write(2) 의
 *          반환값이 된다.
 *
 * enable_slot() 의 짝이다. /sys/bus/pci/slots/N/power 에 0 을 쓰면 여기로 온다.
 * 실제 일은 shpchp_ctrl.c 가 하며, 순서는 "장치 드라이버 제거 → 버스에서 장치
 * 제거(shpchp_unconfigure_device) → 슬롯 전원 차단" 이다. 순서가 중요한 이유는
 * 드라이버가 아직 붙어 있는 장치의 전원을 끊으면 드라이버가 사라진 하드웨어에
 * MMIO 접근을 시도해 머신 체크가 나기 때문이다.
 *
 * 실행 컨텍스트는 sysfs write(2) 프로세스 컨텍스트이고, 재진입 방지는 하위
 * 상태 머신의 슬롯 뮤텍스가 담당한다.
 *
 * 에러 경로: 하위 함수의 음수 errno 를 그대로 전달한다.
 *
 * 호출 체인:
 *   sysfs power 쓰기(0) → pci_hotplug_core.c 의 store 핸들러
 *     → hotplug_slot_ops.disable_slot → [disable_slot]
 *     → shpchp_sysfs_disable_slot()(shpchp_ctrl.c)
 */
static int disable_slot(struct hotplug_slot *hotplug_slot)
{
	struct slot *slot = get_slot(hotplug_slot);	/* [한국어] container_of 로 hotplug_slot 에서 struct slot 복원 */

	ctrl_dbg(slot->ctrl, "%s: physical_slot = %s\n",	/* [한국어] 어느 슬롯을 끄라는 요청인지 디버그 로그 */
		 __func__, slot_name(slot));	/* [한국어] 함수 이름과 슬롯 이름 출력 인자 */

	return shpchp_sysfs_disable_slot(slot);	/* [한국어] shpchp_ctrl.c 의 상태 머신으로 위임 — 장치 제거(shpchp_unconfigure_device)와 전원 차단이 거기서 일어난다 */
}

/*
 * [한국어]
 * get_power_status - hotplug_slot_ops.get_power_status 구현. 슬롯 전원이 켜져 있는지 보고한다
 *
 * @hotplug_slot: 핫플러그 코어가 넘겨준 슬롯 핸들.
 * @value: 결과를 채울 곳. 1 이면 전원 인가 상태, 0 이면 차단 상태.
 * @return: 항상 0. 하드웨어 읽기가 실패해도 캐시값으로 대체하고 성공으로 보고한다.
 *
 * /sys/bus/pci/slots/N/power 를 읽으면 여기로 온다. 하드웨어를 먼저 물어보고,
 * 실패하면 마지막으로 알던 값(slot->pwr_save)을 돌려준다. 이렇게 하는 이유는
 * SHPC 컨트롤러 명령이 타임아웃될 수 있고, 그럴 때 사용자에게 read(2) 에러를
 * 던지는 것보다 마지막으로 관측된 값을 보여 주는 편이 낫기 때문이다.
 * 이 함수는 init_slots() 에서도 직접 불려 최초 캐시값을 채우는 데 쓰인다.
 *
 * 실행 컨텍스트는 sysfs read(2) 프로세스 컨텍스트, 그리고 probe 경로다. 읽기
 * 전용 조회라 재진입해도 안전하다.
 *
 * 에러 경로: shpchp_get_power_status() 가 음수를 돌려주면 캐시로 대체할 뿐,
 * 위로 올리는 에러는 없다.
 *
 * 호출 체인:
 *   sysfs power 읽기 → pci_hotplug_core.c 의 show 핸들러
 *     → hotplug_slot_ops.get_power_status → [get_power_status]
 *     → shpchp_get_power_status()(shpchp_hpc.c)
 *   init_slots() → [get_power_status]
 */
static int get_power_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	struct slot *slot = get_slot(hotplug_slot);	/* [한국어] container_of 로 hotplug_slot 에서 struct slot 복원 */
	int retval;	/* [한국어] 하드웨어 조회 결과(음수면 실패)를 받을 변수 */

	ctrl_dbg(slot->ctrl, "%s: physical_slot = %s\n",	/* [한국어] 어느 슬롯의 전원을 읽는지 디버그 로그 */
		 __func__, slot_name(slot));	/* [한국어] 함수 이름과 슬롯 이름 출력 인자 */

	retval = shpchp_get_power_status(slot, value);	/* [한국어] SHPC Slot Register 의 Slot State 필드를 읽어 *value 에 채운다 */
	if (retval < 0)	/* [한국어] 하드웨어 읽기 실패(예: 컨트롤러 명령 타임아웃) 진입 */
		*value = slot->pwr_save;	/* [한국어] 마지막으로 알던 값으로 대체 — sysfs 읽기가 에러 대신 캐시값을 보게 한다 */

	return 0;	/* [한국어] 캐시 대체까지 마쳤으므로 sysfs 쪽에는 항상 성공으로 보고한다 */
}

/*
 * [한국어]
 * get_attention_status - hotplug_slot_ops.get_attention_status 구현. 황색 LED 상태를 보고한다
 *
 * @hotplug_slot: 핫플러그 코어가 넘겨준 슬롯 핸들.
 * @value: 결과를 채울 곳. 0=꺼짐, 1=켜짐, 2=깜빡임.
 * @return: 항상 0. 하드웨어 읽기 실패 시 캐시값으로 대체한다.
 *
 * /sys/bus/pci/slots/N/attention 을 읽으면 여기로 온다. set_attention_status()
 * 가 써 둔 slot->attention_save 가 폴백이므로, 하드웨어가 대답하지 않아도 사용자가
 * 방금 설정한 값이 되돌아온다. init_slots() 가 최초 캐시를 채울 때도 이 함수를
 * 직접 부른다.
 *
 * 실행 컨텍스트는 sysfs read(2) 프로세스 컨텍스트와 probe 경로다. 읽기 전용이라
 * 재진입 안전하다.
 *
 * 에러 경로: 하드웨어 조회 실패는 캐시 대체로 흡수하고 성공을 보고한다.
 *
 * 호출 체인:
 *   sysfs attention 읽기 → pci_hotplug_core.c 의 show 핸들러
 *     → hotplug_slot_ops.get_attention_status → [get_attention_status]
 *     → shpchp_get_attention_status()(shpchp_hpc.c)
 *   init_slots() → [get_attention_status]
 */
static int get_attention_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	struct slot *slot = get_slot(hotplug_slot);	/* [한국어] container_of 로 hotplug_slot 에서 struct slot 복원 */
	int retval;	/* [한국어] 하드웨어 조회 결과를 받을 변수 */

	ctrl_dbg(slot->ctrl, "%s: physical_slot = %s\n",	/* [한국어] 어느 슬롯의 attention 을 읽는지 디버그 로그 */
		 __func__, slot_name(slot));	/* [한국어] 함수 이름과 슬롯 이름 출력 인자 */

	retval = shpchp_get_attention_status(slot, value);	/* [한국어] SHPC Slot Register 의 Attention Indicator 필드를 읽어 *value 에 채운다 */
	if (retval < 0)	/* [한국어] 하드웨어 읽기 실패 진입 */
		*value = slot->attention_save;	/* [한국어] set_attention_status() 가 저장해 둔 캐시값으로 대체 */

	return 0;	/* [한국어] 캐시 대체까지 마쳤으므로 항상 성공 보고 */
}

/*
 * [한국어]
 * get_latch_status - hotplug_slot_ops.get_latch_status 구현. MRL 걸쇠가 열렸는지 보고한다
 *
 * @hotplug_slot: 핫플러그 코어가 넘겨준 슬롯 핸들.
 * @value: 결과를 채울 곳. SHPC MRL(Manually-operated Retention Latch, 사람이
 *         손으로 여닫는 카드 고정 걸쇠) 센서 상태다.
 * @return: 항상 0. 하드웨어 읽기 실패 시 캐시값으로 대체한다.
 *
 * /sys/bus/pci/slots/N/latch 를 읽으면 여기로 온다. 걸쇠가 열린 것을 사람이 카드를
 * 뽑으려 한다는 신호로 쓰기 때문에, 상태 머신뿐 아니라 사용자에게도 노출한다.
 * init_slots() 가 최초 캐시를 채울 때도 직접 부른다.
 *
 * 실행 컨텍스트는 sysfs read(2) 프로세스 컨텍스트와 probe 경로이며 읽기 전용이다.
 *
 * 에러 경로: 하드웨어 조회 실패는 slot->latch_save 로 대체하고 성공을 보고한다.
 *
 * 호출 체인:
 *   sysfs latch 읽기 → pci_hotplug_core.c 의 show 핸들러
 *     → hotplug_slot_ops.get_latch_status → [get_latch_status]
 *     → shpchp_get_latch_status()(shpchp_hpc.c)
 *   init_slots() → [get_latch_status]
 */
static int get_latch_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	struct slot *slot = get_slot(hotplug_slot);	/* [한국어] container_of 로 hotplug_slot 에서 struct slot 복원 */
	int retval;	/* [한국어] 하드웨어 조회 결과를 받을 변수 */

	ctrl_dbg(slot->ctrl, "%s: physical_slot = %s\n",	/* [한국어] 어느 슬롯의 걸쇠를 읽는지 디버그 로그 */
		 __func__, slot_name(slot));	/* [한국어] 함수 이름과 슬롯 이름 출력 인자 */

	retval = shpchp_get_latch_status(slot, value);	/* [한국어] SHPC Slot Register 의 MRL Sensor 상태를 읽어 *value 에 채운다 */
	if (retval < 0)	/* [한국어] 하드웨어 읽기 실패 진입 */
		*value = slot->latch_save;	/* [한국어] 마지막으로 알던 걸쇠 상태로 대체 */

	return 0;	/* [한국어] 캐시 대체까지 마쳤으므로 항상 성공 보고 */
}

/*
 * [한국어]
 * get_adapter_status - hotplug_slot_ops.get_adapter_status 구현. 슬롯에 카드가 꽂혀 있는지 보고한다
 *
 * @hotplug_slot: 핫플러그 코어가 넘겨준 슬롯 핸들.
 * @value: 결과를 채울 곳. 1 이면 카드 존재, 0 이면 빈 슬롯.
 * @return: 항상 0. 하드웨어 읽기 실패 시 캐시값으로 대체한다.
 *
 * /sys/bus/pci/slots/N/adapter 를 읽으면 여기로 온다. "전원이 켜져 있는가"
 * (get_power_status)와 "물리적으로 꽂혀 있는가"(이 함수)는 다른 질문이며, 카드가
 * 꽂혀 있어도 전원이 꺼져 있을 수 있다. init_slots() 가 최초 캐시를 채울 때도
 * 직접 부른다.
 *
 * 실행 컨텍스트는 sysfs read(2) 프로세스 컨텍스트와 probe 경로이며 읽기 전용이다.
 *
 * 에러 경로: 하드웨어 조회 실패는 slot->presence_save 로 대체하고 성공을 보고한다.
 *
 * 호출 체인:
 *   sysfs adapter 읽기 → pci_hotplug_core.c 의 show 핸들러
 *     → hotplug_slot_ops.get_adapter_status → [get_adapter_status]
 *     → shpchp_get_adapter_status()(shpchp_hpc.c)
 *   init_slots() → [get_adapter_status]
 */
static int get_adapter_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	struct slot *slot = get_slot(hotplug_slot);	/* [한국어] container_of 로 hotplug_slot 에서 struct slot 복원 */
	int retval;	/* [한국어] 하드웨어 조회 결과를 받을 변수 */

	ctrl_dbg(slot->ctrl, "%s: physical_slot = %s\n",	/* [한국어] 어느 슬롯의 카드 존재를 읽는지 디버그 로그 */
		 __func__, slot_name(slot));	/* [한국어] 함수 이름과 슬롯 이름 출력 인자 */

	retval = shpchp_get_adapter_status(slot, value);	/* [한국어] SHPC Slot Register 의 Card Present 상태를 읽어 *value 에 채운다 */
	if (retval < 0)	/* [한국어] 하드웨어 읽기 실패 진입 */
		*value = slot->presence_save;	/* [한국어] 마지막으로 알던 존재 여부로 대체 */

	return 0;	/* [한국어] 캐시 대체까지 마쳤으므로 항상 성공 보고 */
}

/*
 * [한국어]
 * shpc_capable - 이 브리지가 SHPC 컨트롤러를 가지고 있는지 판정한다
 *
 * @bridge: 방금 probe 로 들어온 PCI-to-PCI 브리지 장치.
 * @return: true 면 shpchp 가 다룰 수 있는 브리지, false 면 대상 아님.
 *
 * 이 드라이버의 pci_device_id 표는 벤더/디바이스가 아니라 "PCI-to-PCI 브리지
 * 클래스" 로 매칭하기 때문에, 시스템의 거의 모든 브리지가 shpc_probe() 로 들어온다.
 * 그 중 진짜 SHPC 를 가진 것만 골라내는 것이 이 함수의 존재 이유다.
 *
 * 판정은 두 단계다. 먼저 AMD GOLAM 7450 을 하드코딩 예외로 통과시킨다 — 이 칩은
 * SHPC 를 실제로 지원하지만 PCI capability 리스트에는 SHPC capability 를 광고하지
 * 않는다는 것이 상류 주석에 기록되어 있다. 그 다음 일반 경로로
 * pci_find_capability(PCI_CAP_ID_SHPC = 0x0C) 를 돌려 capability 존재를 확인한다.
 *
 * 실행 컨텍스트는 PCI 코어의 probe 경로(프로세스 컨텍스트)다. 상태를 바꾸지 않는
 * 순수 조회이므로 재진입해도 안전하다.
 *
 * 에러 경로: 없다. 판정 결과만 bool 로 돌려주고, false 일 때 호출자가 -ENODEV 로
 * 물러난다.
 *
 * 호출 체인:
 *   shpc_probe() → [shpc_capable] → pci_find_capability()(drivers/pci/pci.c)
 */
static bool shpc_capable(struct pci_dev *bridge)
{
	/*
	 * It is assumed that AMD GOLAM chips support SHPC but they do not
	 * have SHPC capability.
	 */
	if (bridge->vendor == PCI_VENDOR_ID_AMD &&	/* [한국어] AMD GOLAM 7450 브리지는 SHPC capability 를 광고하지 않지만 실제로는 SHPC 를 지원한다는 것이 알려져 있어, 벤더/디바이스 ID 로 예외 처리한다 */
	    bridge->device == PCI_DEVICE_ID_AMD_GOLAM_7450)	/* [한국어] 위 조건의 디바이스 ID 부분 — include/linux/pci_ids.h 의 PCI_DEVICE_ID_AMD_GOLAM_7450(0x7450) */
		return true;	/* [한국어] 예외 목록에 걸렸으므로 capability 검사 없이 지원으로 간주 */

	if (pci_find_capability(bridge, PCI_CAP_ID_SHPC))	/* [한국어] PCI capability 리스트를 훑어 ID 0x0C(PCI_CAP_ID_SHPC, PCI Standard Hot-Plug Controller)를 찾는다. 있으면 그 capability 오프셋(0 아님)이 돌아온다 */
		return true;	/* [한국어] SHPC capability 가 실제로 있으므로 지원 */

	return false;	/* [한국어] 예외 목록에도 없고 capability 도 없다 — 이 브리지는 shpchp 대상이 아니다 */
}

/*
 * [한국어]
 * shpc_probe - PCI 코어가 브리지를 물려줄 때 SHPC 컨트롤러를 통째로 올린다
 *
 * @pdev: 클래스 매칭으로 걸린 PCI-to-PCI 브리지.
 * @ent: 매칭에 쓰인 pci_device_id 항목. 이 드라이버는 클래스 하나로만 매칭하므로
 *       내용을 보지 않는다.
 * @return: 0 이면 바인딩 성공. 실패는 사유와 무관하게 항상 -ENODEV 로 보고하며,
 *          PCI 코어는 이를 "이 드라이버는 이 장치를 맡지 않는다"로 처리해 다른
 *          드라이버가 시도할 수 있게 둔다.
 *
 * 이 함수가 shpchp 모듈의 실질적 시작점이다. 순서와 그 이유는 다음과 같다.
 * (1) shpc_capable() — 클래스 매칭으로 들어온 수많은 브리지 중 SHPC 를 가진 것만
 *     남긴다.
 * (2) acpi_get_hp_hw_control_from_firmware() — ACPI _OSC 로 펌웨어와 핫플러그
 *     하드웨어 소유권을 협상한다. 펌웨어가 계속 쥐겠다고 하면 OS 드라이버가 같은
 *     레지스터를 만질 수 없으므로 여기서 물러나야 한다.
 * (3) controller 할당과 슬롯 리스트 초기화.
 * (4) shpc_init()(shpchp_hpc.c) — MMIO 매핑, 능력 레지스터 파싱(슬롯 개수,
 *     첫 슬롯 번호, 장치 번호 오프셋, 번호 증가 방향), 인터럽트 또는 폴링 설치.
 * (5) init_slots() — 슬롯 객체 생성과 sysfs 노출.
 * (6) shpchp_create_ctrl_files() — 컨트롤러 단위 디버그 파일.
 * (7) pdev->shpc_managed = 1 — 커널 전체에 "이 브리지는 shpchp 소유" 를 알린다.
 *     이 비트를 pci-acpi.c 의 shpchp_is_native() 가 읽고, 그 결과가
 *     hotplug_is_native() 를 통해 acpiphp 로 전달되어 두 드라이버가 같은 슬롯을
 *     동시에 관리하는 사태를 막는다.
 *
 * 실행 컨텍스트는 프로세스 컨텍스트다. 모듈 적재 시에는 shpcd_init() 안에서
 * 동기적으로, 이후에는 PCI 장치가 새로 나타날 때 PCI 코어의 버스 락 아래에서
 * 불린다. 장치마다 한 번씩만 불리므로 재진입 문제는 없다.
 *
 * 에러 경로는 4단 라벨 되감기다. err_cleanup_slots → err_out_release_ctlr →
 * err_out_free_ctrl → err_out_none 순으로 잡은 것만 정확히 되돌린다. 어느 경로든
 * 최종 반환은 -ENODEV 이며, 구체적 사유는 ctrl_dbg/ctrl_err 로그로 남긴다.
 *
 * 호출 체인:
 *   shpcd_init() → pci_register_driver() → (PCI 코어) → [shpc_probe]
 *     → shpc_capable() / acpi_get_hp_hw_control_from_firmware()(acpi_pcihp.c)
 *       / shpc_init()(shpchp_hpc.c) / init_slots()
 *       / shpchp_create_ctrl_files()(shpchp_sysfs.c)
 */
static int shpc_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	int rc;	/* [한국어] 중간 단계 반환값(음수 errno)을 받는 변수. 다만 실패 시 최종 반환값은 항상 -ENODEV 로 뭉뚱그린다 */
	struct controller *ctrl;	/* [한국어] 이 브리지를 대표할 컨트롤러 객체 포인터 */

	if (!shpc_capable(pdev))	/* [한국어] SHPC 하드웨어가 없는 브리지는 즉시 거른다 — pci_driver 가 클래스로 매칭하므로 모든 PCI-to-PCI 브리지가 여기까지 들어온다 */
		return -ENODEV;	/* [한국어] 드라이버 미대응을 뜻하는 -ENODEV 반환. PCI 코어는 이 값을 실패가 아닌 "안 맞음"으로 취급한다 */

	if (acpi_get_hp_hw_control_from_firmware(pdev))	/* [한국어] ACPI _OSC 를 통해 펌웨어에게서 핫플러그 하드웨어 제어권을 넘겨받는다(drivers/pci/hotplug/acpi_pcihp.c). 0 이 아니면 펌웨어가 계속 제어하겠다는 뜻이라 OS 드라이버는 물러나야 한다 */
		return -ENODEV;	/* [한국어] 펌웨어가 제어권을 안 줬으므로 -ENODEV 로 물러난다 */

	ctrl = kzalloc_obj(*ctrl);	/* [한국어] 컨트롤러 객체를 0 으로 채워 할당 */
	if (!ctrl)	/* [한국어] 할당 실패 진입 — 아직 아무것도 잡지 않았으므로 해제할 것이 없다 */
		goto err_out_none;	/* [한국어] 해제 없이 -ENODEV 로 나가는 라벨로 점프(rc 값은 쓰이지 않는다) */

	INIT_LIST_HEAD(&ctrl->slot_list);	/* [한국어] 슬롯 리스트 헤드 초기화 — init_slots() 가 여기에 슬롯을 매단다 */

	rc = shpc_init(ctrl, pdev);	/* [한국어] shpchp_hpc.c 로 내려가 SHPC MMIO(또는 config 공간 창)를 매핑하고, 컨트롤러 능력 레지스터에서 num_slots/first_slot/slot_device_offset/slot_num_inc 를 읽고, 인터럽트(또는 폴링 타이머)를 설치한다 */
	if (rc) {	/* [한국어] 컨트롤러 초기화 실패 진입 */
		ctrl_dbg(ctrl, "Controller initialization failed\n");	/* [한국어] 실패 사실만 디버그 로그로 남긴다(구체적 사유는 shpc_init 안에서 이미 로그됨) */
		goto err_out_free_ctrl;	/* [한국어] ctrl 만 해제하는 라벨로 점프 */
	}

	pci_set_drvdata(pdev, ctrl);	/* [한국어] pci_dev 의 드라이버 전용 포인터에 컨트롤러를 매달아 둔다 — shpc_remove() 가 이걸로 되찾는다 */

	/* Setup the slot information structures */
	rc = init_slots(ctrl);	/* [한국어] 슬롯 객체를 만들고 핫플러그 코어에 등록해 sysfs 를 노출한다 */
	if (rc) {	/* [한국어] 슬롯 초기화 실패 진입 */
		ctrl_err(ctrl, "Slot initialization failed\n");	/* [한국어] 실패 로그 */
		goto err_out_release_ctlr;	/* [한국어] 컨트롤러 자원까지 되돌리는 라벨로 점프 */
	}

	rc = shpchp_create_ctrl_files(ctrl);	/* [한국어] 컨트롤러 단위 디버그용 sysfs 파일 생성(shpchp_sysfs.c). __must_check 함수라 반환값을 반드시 본다 */
	if (rc)	/* [한국어] sysfs 파일 생성 실패 진입 */
		goto err_cleanup_slots;	/* [한국어] 이미 등록한 슬롯들을 되돌리는 라벨로 점프 */

	pdev->shpc_managed = 1;	/* [한국어] 이 브리지의 SHPC 는 shpchp 가 관리한다고 표시. drivers/pci/pci-acpi.c 의 shpchp_is_native() 가 이 비트를 읽고, 그 값이 pci_hotplug.h 의 hotplug_is_native() 를 통해 acpiphp 가 이 슬롯에서 손을 떼게 만든다 */
	return 0;	/* [한국어] probe 성공 — PCI 코어가 이 드라이버를 이 장치에 바인딩한다 */

err_cleanup_slots:	/* [한국어] sysfs 파일 생성 실패가 들어오는 라벨 */
	cleanup_slots(ctrl);	/* [한국어] 등록된 슬롯 전부를 해제(이 파일 위쪽 정의) */
err_out_release_ctlr:	/* [한국어] 슬롯 초기화 실패가 들어오는 라벨 */
	shpchp_release_ctlr(ctrl);	/* [한국어] MMIO 해제, 인터럽트 해제 등 shpc_init() 이 잡은 것을 되돌린다(shpchp_hpc.c) */
err_out_free_ctrl:	/* [한국어] 컨트롤러 초기화 실패가 들어오는 라벨 */
	kfree(ctrl);	/* [한국어] 컨트롤러 객체 해제 */
err_out_none:	/* [한국어] 아무것도 잡지 못한 실패가 들어오는 라벨 */
	return -ENODEV;	/* [한국어] 어떤 실패든 PCI 코어에는 -ENODEV 하나로 보고한다 — 구체적 사유는 이미 로그로 남겼다 */
}

/*
 * [한국어]
 * shpc_remove - 브리지에서 드라이버를 뗄 때 shpc_probe() 가 잡은 것을 모두 되돌린다
 *
 * @dev: 바인딩을 해제할 브리지 장치.
 * @return: 없음. PCI 코어의 remove 콜백은 실패를 보고할 수 없다.
 *
 * 모듈 해제나 브리지 자체의 논리적 제거 시 호출된다. 순서가 중요하다.
 * 먼저 shpc_managed 를 0 으로 내려 "이제 shpchp 소유가 아님" 을 알린다 — 이렇게
 * 해야 해제 도중 hotplug_is_native() 를 묻는 다른 코드가 사라지는 중인 컨트롤러를
 * 여전히 유효하다고 착각하지 않는다. 그 다음 sysfs 디버그 파일을 없애고,
 * shpchp_release_ctlr() 로 인터럽트/폴링 타이머와 MMIO 를 반납한다(그 안에서
 * cleanup_slots() 가 불려 슬롯과 워크큐도 함께 정리된다). 마지막으로 컨트롤러
 * 객체를 해제한다.
 *
 * 실행 컨텍스트는 프로세스 컨텍스트다. 하위 단계에 잠들 수 있는 호출
 * (destroy_workqueue, free_irq)이 있으므로 아토믹 문맥에서는 불릴 수 없다.
 *
 * 에러 경로: 없다. 모든 하위 해제 함수는 실패하지 않는다.
 *
 * 호출 체인:
 *   pci_unregister_driver() 또는 PCI 장치 제거 → (PCI 코어) → [shpc_remove]
 *     → shpchp_remove_ctrl_files()(shpchp_sysfs.c)
 *       / shpchp_release_ctlr()(shpchp_hpc.c, 내부에서 cleanup_slots() 호출)
 */
static void shpc_remove(struct pci_dev *dev)
{
	struct controller *ctrl = pci_get_drvdata(dev);	/* [한국어] probe 에서 pci_set_drvdata() 로 매달아 둔 컨트롤러를 되찾는다 */

	dev->shpc_managed = 0;	/* [한국어] 해제를 시작하기 전에 먼저 "더 이상 shpchp 소유가 아님"을 표시 — 이 순간부터 hotplug_is_native() 가 false 를 돌려준다 */
	shpchp_remove_ctrl_files(ctrl);	/* [한국어] 컨트롤러 단위 sysfs 디버그 파일 제거 */
	shpchp_release_ctlr(ctrl);	/* [한국어] 인터럽트/폴링 타이머 해제와 MMIO 해제. 이 함수 안에서 cleanup_slots() 가 불려 슬롯들도 함께 정리된다 */
	kfree(ctrl);	/* [한국어] 컨트롤러 객체 해제 — 이 시점 이후 pci_get_drvdata() 로 얻은 포인터는 무효다 */
}

static const struct pci_device_id shpcd_pci_tbl[] = {	/* [한국어] PCI 코어가 이 드라이버를 어떤 장치에 물릴지 판단하는 매칭 표 */
	{PCI_DEVICE_CLASS(PCI_CLASS_BRIDGE_PCI_NORMAL, ~0)},	/* [한국어] 벤더/디바이스가 아니라 클래스로 매칭한다. PCI_CLASS_BRIDGE_PCI_NORMAL(0x060400)은 "PCI-to-PCI 브리지, 일반 디코드" 이고 마스크 ~0 은 클래스 24비트 전부 일치를 요구한다. 즉 모든 PCI-to-PCI 브리지가 일단 probe 로 들어오고, 실제 걸러내기는 shpc_capable() 이 한다 */
	{ /* end: all zeroes */ }	/* [한국어] 표의 끝을 알리는 0 항목 — PCI 코어는 이 널 항목을 만나면 순회를 멈춘다 */
};
MODULE_DEVICE_TABLE(pci, shpcd_pci_tbl);	/* [한국어] 이 매칭 표를 모듈 메타데이터로 노출해 udev/모듈 자동 적재가 동작하게 한다 */

static struct pci_driver shpc_driver = {	/* [한국어] PCI 버스에 등록할 드라이버 서술자 */
	.name =		SHPC_MODULE_NAME,	/* [한국어] /sys/bus/pci/drivers/shpchp 디렉터리 이름이 되는 드라이버 이름 */
	.id_table =	shpcd_pci_tbl,	/* [한국어] 위에서 정의한 클래스 매칭 표 연결 */
	.probe =	shpc_probe,	/* [한국어] 매칭된 장치마다 호출될 probe 콜백 */
	.remove =	shpc_remove,	/* [한국어] 바인딩 해제 시 호출될 remove 콜백. suspend/resume 콜백은 두지 않아 SHPC 는 PM 이벤트에 관여하지 않는다 */
};

/*
 * [한국어]
 * shpcd_init - 모듈 적재 진입점. PCI 버스에 shpchp 드라이버를 등록한다
 *
 * @return: pci_register_driver() 의 반환값. 0 이면 성공, 음수 errno 면 모듈 적재
 *          자체가 실패한다.
 *
 * 모듈이 하는 일은 등록 한 번뿐이다. 등록하는 순간 PCI 코어가 이미 열거해 둔 모든
 * PCI-to-PCI 브리지에 대해 shpc_probe() 를 동기적으로 호출하므로, 이 함수가
 * 돌아왔을 때는 SHPC 를 가진 브리지들의 슬롯이 이미 sysfs 에 올라와 있다.
 * 별도의 전역 초기화가 없는 이유는 이 드라이버의 상태가 전부 컨트롤러/슬롯 객체
 * 안에 들어 있고, 전역이라고는 폴링 관련 모듈 파라미터 두 개뿐이기 때문이다.
 *
 * 실행 컨텍스트는 insmod/modprobe 또는 부팅 시 initcall 의 프로세스 컨텍스트이며,
 * 모듈당 정확히 한 번만 실행된다.
 *
 * 에러 경로: pci_register_driver() 가 실패하면 그 값을 그대로 올려 모듈 적재를
 * 중단시킨다. 이 함수가 따로 되돌릴 것은 없다.
 *
 * 호출 체인:
 *   module_init → [shpcd_init] → pci_register_driver()(drivers/pci/pci-driver.c)
 *     → shpc_probe()
 */
static int __init shpcd_init(void)
{
	return pci_register_driver(&shpc_driver);	/* [한국어] PCI 코어에 드라이버 등록. 등록 즉시 이미 열거된 브리지들에 대해 shpc_probe() 가 동기적으로 불린다. 반환값(음수 errno)은 그대로 모듈 적재 실패로 이어진다 */
}

/*
 * [한국어]
 * shpcd_cleanup - 모듈 해제 진입점. PCI 버스에서 shpchp 드라이버를 등록 해제한다
 *
 * @return: 없음.
 *
 * shpcd_init() 의 정확한 역연산이다. pci_unregister_driver() 는 이 드라이버에
 * 바인딩된 모든 장치에 대해 shpc_remove() 를 부른 뒤에야 돌아오므로, 이 함수가
 * 끝나면 shpchp 가 만든 슬롯과 워크큐, MMIO 매핑이 하나도 남아 있지 않다.
 * 그래서 여기서 따로 정리할 것이 없다.
 *
 * 실행 컨텍스트는 rmmod 의 프로세스 컨텍스트이며 모듈당 한 번만 실행된다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   module_exit → [shpcd_cleanup] → pci_unregister_driver() → shpc_remove()
 */
static void __exit shpcd_cleanup(void)
{
	pci_unregister_driver(&shpc_driver);	/* [한국어] 드라이버 등록 해제. 안에서 바인딩된 장치마다 shpc_remove() 가 호출된 뒤에 돌아온다 */
}

module_init(shpcd_init);	/* [한국어] shpcd_init() 을 모듈 적재 진입점으로 지정 */
module_exit(shpcd_cleanup);	/* [한국어] shpcd_cleanup() 을 모듈 해제 진입점으로 지정 */
