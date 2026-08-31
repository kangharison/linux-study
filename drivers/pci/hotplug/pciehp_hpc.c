// SPDX-License-Identifier: GPL-2.0+
/*
 * PCI Express PCI Hot Plug Driver
 *
 * Copyright (C) 1995,2001 Compaq Computer Corporation
 * Copyright (C) 2001 Greg Kroah-Hartman (greg@kroah.com)
 * Copyright (C) 2001 IBM Corp.
 * Copyright (C) 2003-2004 Intel Corporation
 *
 * All rights reserved.
 *
 * Send feedback to <greg@kroah.com>,<kristen.c.accardi@intel.com>
 */

/*
 * [한국어 설명] PCIe 네이티브 핫플러그의 하드웨어 접근 계층 (pciehp_hpc.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 PCIe Downstream Port(루트 포트 또는 스위치 다운스트림 포트)가 가진
 * 슬롯 관련 레지스터를 직접 읽고 쓰는 유일한 계층이다. 다루는 레지스터는 모두
 * PCI Express Capability 구조 안에 있으며 Slot Capabilities(PCI_EXP_SLTCAP),
 * Slot Capabilities 2(PCI_EXP_SLTCAP2), Slot Control(PCI_EXP_SLTCTL),
 * Slot Status(PCI_EXP_SLTSTA), Link Control(PCI_EXP_LNKCTL),
 * Link Status(PCI_EXP_LNKSTA / PCI_EXP_LNKSTA2) 여섯 가지다. 즉 이 파일이 하는
 * 일은 "슬롯 전원을 켜라", "표시등을 깜빡여라", "링크를 죽여라" 같은 상위 요청을
 * 실제 config space 접근으로 번역하고, 반대로 하드웨어가 올린 이벤트 비트를 읽어
 * 상위 상태 기계에 전달하는 것이다.
 * 또한 인터럽트 경로 전체를 이 파일이 소유한다. 하드 IRQ 핸들러 pciehp_isr() 이
 * Slot Status 를 읽어 이벤트 비트를 긁어내고 write-1-to-clear 로 지운 뒤,
 * 스레드 IRQ 핸들러 pciehp_ist() 가 잠들 수 있는 문맥에서 실제 처리를 한다.
 * 모듈 파라미터 pciehp_poll_mode 가 켜져 있으면 IRQ 대신 pciehp_poll() 커널
 * 스레드가 같은 두 함수를 주기적으로 직접 호출한다.
 * 마지막으로 Slot Control 에 명령을 쓴 뒤에는 하드웨어가 Command Completed(CC)
 * 를 올릴 때까지 기다려야 다음 명령을 쓸 수 있다(PCIe r4.0 sec 6.7.3.2). CC 를
 * 지원하지 않는 하드웨어(Slot Capabilities 의 NCCS 비트)와 CC 를 지원한다고
 * 거짓 보고하는 브리지(broken_cmd_compl)를 위한 우회 처리도 전부 여기에 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * pciehp 드라이버는 다섯 조각으로 나뉜다.
 *   pciehp.h      — struct controller 정의, 슬롯 상태 상수, 파일 간 프로토타입
 *   pciehp_core.c — PCIe 포트 서비스 드라이버 등록, sysfs hotplug_slot_ops 연결,
 *                   서스펜드/리줌 콜백
 *   pciehp_ctrl.c — 슬롯 상태 기계. 상태는 여섯 개다:
 *                   OFF_STATE / BLINKINGON_STATE / BLINKINGOFF_STATE /
 *                   POWERON_STATE / POWEROFF_STATE / ON_STATE
 *   pciehp_pci.c  — 실제 PCI 열거와 제거 (pci_scan_slot, pci_bus_add_devices,
 *                   pci_stop_and_remove_bus_device)
 *   pciehp_hpc.c  — [이 파일] 위 세 파일이 요구하는 모든 레지스터 접근과 인터럽트
 * 아래에서 위로 올라가는 방향이 이벤트 경로다.
 *   하드웨어 슬롯 이벤트 → pciehp_isr() (하드 IRQ) → ctrl->pending_events
 *     → pciehp_ist() (IRQ 스레드) → pciehp_ctrl.c 의 핸들러 세 개
 * 위에서 아래로 내려가는 방향이 명령 경로다.
 *   sysfs 쓰기 또는 상태 기계 → pciehp_power_on_slot() / pciehp_set_indicators()
 *     → pcie_write_cmd() → pcie_do_write_cmd() → pcie_capability_write_word()
 * 실행 컨텍스트는 셋이 섞인다. pciehp_isr() 은 하드 IRQ 문맥이라 절대 잠들 수
 * 없고, pciehp_ist() 와 pciehp_poll() 은 프로세스 문맥이라 msleep 과 뮤텍스가
 * 허용되며, 나머지 대부분은 pciehp_ctrl.c 의 워크큐 또는 sysfs write 를 수행한
 * 유저 프로세스 문맥에서 불린다.
 *
 * === 타 모듈과의 연결 ===
 * 이 파일이 의존하는 쪽:
 *   drivers/pci/pci.c  — pcie_wait_for_link(), pci_bridge_secondary_bus_reset(),
 *                        pci_get_dsn(), pci_bus_read_dev_vendor_id(),
 *                        pci_config_pm_runtime_get 와 put
 *   drivers/pci/pci.h  — __pcie_update_link_speed(), pci_dpc_recovered(),
 *                        pci_hp_spurious_link_change()
 *   drivers/pci/hotplug/pci_hotplug_core.c — pci_hp_ignore_link_change() 와
 *                        pci_hp_unignore_link_change()
 *   drivers/pci/pcie/portdrv.h — struct pcie_device, get_service_data()
 * 이 파일에 의존하는 쪽: pciehp_core.c 와 pciehp_ctrl.c (프로토타입은 pciehp.h).
 * 데이터 흐름은 struct controller 하나로 모인다. slot_cap 과 slot_ctrl 은
 * 하드웨어 레지스터의 소프트웨어 사본이고, pending_events 는 하드 IRQ 와 IRQ
 * 스레드가 주고받는 유일한 통로(atomic_t)이며, ctrl_lock 은 Slot Control 의
 * 읽고-고쳐-쓰기를 직렬화하고, reset_lock 은 열거 도중의 링크 변화 처리를 막는다.
 * NVMe 와의 관계는 한 방향뿐이다. drivers/nvme 아래에서 pciehp 를 호출하는 코드는
 * 이 트리에 없다(drivers/nvme 전체를 주석 제거한 토큰열로 훑어 pciehp/shpc 로
 * 시작하는 식별자가 하나도 없음을 확인했다). 반대로, 이 파일이 올린 삽입 이벤트가
 * pciehp_ctrl.c 를 거쳐 pciehp_pci.c 의 pci_bus_add_devices() 에 도달하면 드라이버
 * 코어가 장치를 바인드하고, 그 장치가 NVMe SSD 라면 그 결과로
 * drivers/nvme/host/pci.c 의 nvme_probe() 가 불린다. 제거 방향에서는
 * pciehp_pci.c 의 pci_stop_and_remove_bus_device() 가 nvme_remove() 를 유발한다.
 * 이 파일이 nvme 심볼을 직접 부르는 곳은 한 군데도 없다.
 *
 * === 주요 함수/구조체 요약 ===
 * pcie_do_write_cmd() — Slot Control 읽고-고쳐-쓰기의 단일 진입점. ctrl_lock 을
 *   잡고 이전 명령의 Command Completed 를 기다린 뒤 새 값을 쓴다.
 * pcie_wait_cmd() 와 pcie_poll_cmd() — CC 를 기다리는 두 방식. 전자는 인터럽트가
 *   깨워 주기를 기다리고, 후자는 10ms 간격으로 Slot Status 를 직접 읽는다.
 * pciehp_isr() — 하드 IRQ. Slot Status 의 이벤트 비트를 읽어 클리어하고
 *   atomic_or 로 ctrl->pending_events 에 적재한 뒤 IRQ 스레드를 깨운다.
 * pciehp_ist() — IRQ 스레드. atomic_xchg 로 이벤트를 통째로 가져와
 *   pciehp_handle_button_press(), pciehp_handle_disable_request(),
 *   pciehp_handle_presence_or_link_change() 로 분배한다.
 * pcie_init() — struct controller 할당과 초기화. Slot Capabilities 를 읽어 능력
 *   플래그를 확정하고, 빈 슬롯에 전원이 켜져 있으면 끈 뒤 컨트롤러를 돌려준다.
 * struct controller (pciehp.h 에 정의) — 이 파일이 채우는 주요 필드는
 *   slot_cap(능력 사본), slot_ctrl(Slot Control 사본), cmd_busy 와
 *   cmd_started(CC 대기 상태), pending_events(하드 IRQ 와 IRQ 스레드의 통로),
 *   dsn(장치 교체 감지용 Device Serial Number), depth(중첩 핫플러그 포트 깊이,
 *   reset_lock 의 lockdep 서브클래스로 쓰인다), state(슬롯 상태 기계의 현재 상태).
 */

#define dev_fmt(fmt) "pciehp: " fmt
/* [한국어] 이 파일 안의 모든 pci_dbg/pci_info/pci_err 출력 앞에 "pciehp: " 를 붙인다.
 * dev_printk 계열 매크로가 포맷 문자열을 dev_fmt() 로 감싸므로, 이 한 줄만으로 dmesg 에서
 * 핫플러그 로그를 다른 PCI 로그와 구분할 수 있다. <linux/pci.h> 보다 먼저 정의해야
 * 그 헤더가 끌어오는 dev_printk 정의에 반영된다. */

#include <linux/bitfield.h>	/* [한국어] FIELD_GET 과 FIELD_PREP. Slot Capabilities 의 Physical Slot Number 나 Slot Control 의 AIC/PIC 처럼 연속된 비트 필드를 시프트 상수 없이 뽑고 넣기 위해 필요하다. */
#include <linux/dmi.h>		/* [한국어] dmi_first_match(). 아래 inband_presence_disabled_dmi_table 로 Dell 시스템을 식별해 in-band presence 를 강제로 끄기 위해 필요하다. */
#include <linux/kernel.h>	/* [한국어] msleep(), container_of, min/max 등 커널 전역 기본 매크로. */
#include <linux/types.h>	/* [한국어] u8/u16/u32 고정폭 타입. 레지스터 폭이 스펙으로 못 박혀 있으므로 반드시 고정폭 타입을 쓴다. */
#include <linux/jiffies.h>	/* [한국어] jiffies, msecs_to_jiffies(), time_before_eq(). Command Completed 대기 시간 계산에 쓴다. */
#include <linux/kthread.h>	/* [한국어] kthread_run() 과 kthread_stop(). pciehp_poll_mode 에서 폴링 스레드를 띄우고 내리는 데 필요하다. */
#include <linux/pci.h>	/* [한국어] struct pci_dev, pcie_capability_read_word 계열, PCI_EXP_SLT* 레지스터/비트 정의. 이 파일의 뼈대다. */
#include <linux/pm_runtime.h>	/* [한국어] pm_runtime_get_noresume(), pm_runtime_active(), pm_runtime_put(). 포트가 런타임 서스펜드된 동안 config space 를 읽지 않도록 막는 데 쓴다. */
#include <linux/interrupt.h>	/* [한국어] irqreturn_t, request_threaded_irq(), synchronize_hardirq(), disable_irq_nosync(). 하드 IRQ 와 IRQ 스레드 두 단계 구조에 필요하다. */
#include <linux/slab.h>	/* [한국어] kfree(). kzalloc_obj() 로 할당한 struct controller 를 반납할 때 쓴다. */

#include "../pci.h"		/* [한국어] drivers/pci 내부 전용 헤더. pci_bridge_secondary_bus_reset(), __pcie_update_link_speed(), pci_dpc_recovered(), pci_hp_spurious_link_change(), pci_bus_sem 이 여기 선언돼 있다. */
#include "pciehp.h"		/* [한국어] pciehp 전용 헤더. struct controller, 슬롯 상태 상수(OFF_STATE 등), ATTN_BUTTN/POWER_CTRL 같은 능력 질의 매크로, 파일 간 프로토타입이 들어 있다. */

/*
 * [한국어] inband_presence_disabled_dmi_table - in-band presence 를 강제로 끌 시스템 목록
 *
 * PCIe 슬롯의 카드 존재 여부는 두 경로로 알 수 있다. 하나는 전용 핀(out-of-band)
 * 이고 다른 하나는 링크 신호로부터 유추하는 in-band presence 다. 후자는 링크가
 * 죽는 순간 카드가 빠진 것처럼 보이게 만들어 오탐을 낳는다. Slot Capabilities 2
 * 의 IBPD(In-Band Presence Disable) 비트가 있으면 하드웨어가 이를 끄도록 요청할 수
 * 있지만, 일부 Dell 시스템은 이미 in-band presence 가 꺼져 있으면서도 그 사실을
 * 보고할 비트를 지원하지 않는다. 그래서 DMI OEM 문자열로 기종을 식별해 소프트웨어
 * 쪽에서 ctrl->inband_presence_disabled 를 세워 준다.
 * 읽는 자: pcie_init() 이 dmi_first_match() 로 한 번만 조회한다.
 * 값 범위: 마지막 원소가 빈 항목 {} 인 널 종단 배열이어야 dmi_first_match 가 멈춘다.
 * 동기화: const 정적 데이터라 잠금이 필요 없다.
 */
static const struct dmi_system_id inband_presence_disabled_dmi_table[] = {
	/*
	 * Match all Dell systems, as some Dell systems have inband
	 * presence disabled on NVMe slots (but don't support the bit to
	 * report it). Setting inband presence disabled should have no
	 * negative effect, except on broken hotplug slots that never
	 * assert presence detect--and those will still work, they will
	 * just have a bit of extra delay before being probed.
	 */
	{
		.ident = "Dell System",		/* [한국어] dmi_first_match 가 매칭 시 로그에 남기는 사람이 읽을 이름. */
		.matches = {
			DMI_MATCH(DMI_OEM_STRING, "Dell System"),			/* [한국어] DMI 테이블의 OEM Strings(type 11) 에 "Dell System" 이 들어 있는지 본다. 특정 모델이 아니라 Dell 전 기종을 잡는 것이 의도다. */
		},
	},
	{}	/* [한국어] 널 종단 표식. 이 빈 항목이 없으면 dmi_first_match 가 배열 밖을 읽는다. */
};

/*
 * [한국어]
 * ctrl_dev - controller 에서 그 핫플러그 슬롯을 소유한 다운스트림 포트를 꺼낸다
 *
 * @ctrl: 이 슬롯을 관리하는 pciehp 컨트롤러
 * @return: 슬롯 레지스터를 담고 있는 브리지(루트 포트 또는 스위치 다운스트림
 *          포트)의 struct pci_dev. NULL 이 될 수 없다 — pcie_init() 이
 *          ctrl->pcie 를 채운 뒤에만 이 함수가 불리기 때문이다.
 *
 * pciehp 에서 "슬롯"은 별도의 하드웨어가 아니라 브리지의 PCI Express Capability
 * 안에 있는 레지스터 묶음이다. 따라서 Slot Control/Status 에 접근하려면 매번
 * 그 브리지의 pci_dev 가 필요하고, ctrl->pcie->port 를 반복해 쓰는 대신 이 한 줄
 * 래퍼로 의도를 드러낸다. 슬롯에 꽂힌 장치(NVMe SSD 등)의 pci_dev 가 아니라
 * 슬롯을 제공하는 상위 브리지의 pci_dev 라는 점이 중요하다.
 * 실행 컨텍스트: 순수 포인터 역참조뿐이라 하드 IRQ 문맥에서도 안전하고 재진입 가능하다.
 * 에러 경로: 없다. 실패할 수 있는 연산이 없다.
 *
 * 호출 체인:
 *   이 파일의 거의 모든 레지스터 접근 함수 → [ctrl_dev] → (반환값을)
 *   pcie_capability_read_word 계열에 전달
 */
static inline struct pci_dev *ctrl_dev(struct controller *ctrl)
{
	return ctrl->pcie->port;	/* [한국어] pcie_device(포트 서비스 장치)가 가리키는 브리지 pci_dev 를 그대로 돌려준다. */
}

/*
 * [한국어] 전방 선언 세 개.
 * pciehp_request_irq() 가 세 함수를 모두 참조하지만 정의는 파일 뒤쪽에 있으므로
 * 여기서 미리 선언한다. 또 pciehp_poll() 이 pciehp_isr() 과 pciehp_ist() 를
 * 직접 부르고, pciehp_ist() 는 다시 pciehp_isr() 을 부르는 상호 참조 구조라
 * 선언 없이는 순서를 잡을 수 없다.
 */
static irqreturn_t pciehp_isr(int irq, void *dev_id);	/* [한국어] 하드 IRQ(top-half). Slot Status 를 읽어 이벤트 비트만 걷어 낸다. 잠들 수 없다. */
static irqreturn_t pciehp_ist(int irq, void *dev_id);	/* [한국어] 스레드 IRQ(bottom-half). 실제 핫플러그 처리를 프로세스 문맥에서 수행한다. */
static int pciehp_poll(void *data);		/* [한국어] pciehp_poll_mode 전용 커널 스레드. 인터럽트 없이 위 두 함수를 주기적으로 부른다. */

/*
 * [한국어]
 * pciehp_request_irq - 핫플러그 이벤트를 받을 통로(인터럽트 또는 폴링 스레드)를 연다
 *
 * @ctrl: 초기화가 끝난 pciehp 컨트롤러. ctrl->pcie->irq 에 포트 서비스가 배정한
 *        IRQ 번호가, ctrl->hotplug_slot 에 슬롯 이름이 이미 들어 있어야 한다.
 * @return: 0 이면 성공. 음수면 실패이며 호출자 pcie_init_notification() 이 이를
 *          -1 로 뭉뚱그려 pciehp_core.c 의 pciehp_probe() 에 알린다.
 *
 * 이벤트를 받는 방법은 두 가지다. 기본은 인터럽트이고, 인터럽트가 망가진
 * 하드웨어를 위해 모듈 파라미터 pciehp_poll_mode 로 폴링을 고를 수 있다. 두 방식이
 * 같은 핸들러 쌍을 쓰기 때문에 진입점만 여기서 갈라 놓는다.
 * 동작 단계: (1) 폴 모드면 kthread_run() 으로 pciehp_poll 스레드를 띄우고 끝낸다.
 * (2) 아니면 request_threaded_irq() 로 하드 IRQ 핸들러와 스레드 핸들러를 함께
 * 등록한다. 두 단계로 나누는 이유는 실제 처리에 뮤텍스와 msleep 이 필요해
 * 하드 IRQ 문맥에서는 불가능하기 때문이다.
 * IRQF_SHARED 를 주는 이유: PCIe 포트 서비스(AER, DPC, PME, hotplug)가 같은
 * 인터럽트 선을 공유할 수 있어서, 자기 이벤트가 아니면 IRQ_NONE 을 돌려주는
 * 규약을 지켜야 한다. 그 판정을 pciehp_isr() 이 한다.
 * 실행 컨텍스트: pciehp_probe() 의 프로세스 문맥. 컨트롤러당 한 번만 불린다.
 * 에러 경로: kthread_run 실패는 PTR_ERR_OR_ZERO 로 음수화되고,
 * request_threaded_irq 실패는 ctrl_err 로 기록한 뒤 그대로 반환된다. 어느 쪽이든
 * 호출자가 이어서 pciehp_release_ctrl() 로 되돌린다.
 *
 * 호출 체인:
 *   pciehp_probe() [core.c] → pcie_init_notification() → [pciehp_request_irq]
 *     → kthread_run(pciehp_poll) 또는 request_threaded_irq(pciehp_isr, pciehp_ist)
 */
static inline int pciehp_request_irq(struct controller *ctrl)
{
	int retval, irq = ctrl->pcie->irq;	/* [한국어] 포트 서비스 드라이버가 이 hotplug 서비스에 배정한 IRQ 번호. MSI/MSI-X 가 가능하면 그 벡터, 아니면 공유 INTx 선이다. */

	if (pciehp_poll_mode) {	/* [한국어] 인터럽트를 신뢰할 수 없는 하드웨어를 위한 우회 경로. 부팅 파라미터 pciehp.pciehp_poll_mode=1 로 켠다. */
		ctrl->poll_thread = kthread_run(&pciehp_poll, ctrl,		/* [한국어] 폴링 스레드를 만들어 바로 실행시킨다. 스레드 함수는 pciehp_poll, 인자는 ctrl 자신이다. */
						"pciehp_poll-%s",						/* [한국어] 스레드 이름 형식. ps 에서 어느 슬롯의 폴러인지 구분하려고 슬롯 이름을 붙인다. */
						slot_name(ctrl));						/* [한국어] hotplug_slot_name() 이 돌려주는 슬롯 이름(대개 Physical Slot Number 문자열). */
		return PTR_ERR_OR_ZERO(ctrl->poll_thread);		/* [한국어] kthread_run 은 실패 시 ERR_PTR 를 돌려주므로 포인터를 그대로 저장하고 오류만 정수로 바꿔 반환한다. */
	}

	/* Installs the interrupt handler */
	retval = request_threaded_irq(irq, pciehp_isr, pciehp_ist,	/* [한국어] 하드 IRQ 핸들러와 스레드 핸들러를 한 번에 등록한다. 하드 IRQ 가 IRQ_WAKE_THREAD 를 반환하면 커널이 스레드 핸들러를 깨운다. */
				      IRQF_SHARED, "pciehp", ctrl);				      /* [한국어] IRQF_SHARED: 같은 선을 AER/DPC/PME 등 다른 포트 서비스와 공유할 수 있다. "pciehp" 는 /proc/interrupts 표시 이름, ctrl 은 두 핸들러에 넘어갈 dev_id 다. */
	if (retval)	/* [한국어] 0 이 아니면 등록 실패. 이미 점유된 IRQ 이거나 메모리 부족이다. */
		ctrl_err(ctrl, "Cannot get irq %d for the hotplug controller\n",		/* [한국어] 어느 IRQ 에서 실패했는지 남긴다. 이 로그가 보이면 이 슬롯은 핫플러그 이벤트를 영영 못 받는다. */
			 irq);
	return retval;	/* [한국어] 성공 0 또는 request_threaded_irq 의 음수 오류를 그대로 위로 전달. */
}

/*
 * [한국어]
 * pciehp_free_irq - pciehp_request_irq() 가 연 통로를 닫는다
 *
 * @ctrl: 정리 대상 컨트롤러. ctrl->poll_thread 또는 등록된 IRQ 중 하나가 살아 있다.
 * @return: 없음.
 *
 * 왜 필요한가: 드라이버가 내려가는 동안 인터럽트가 들어오면 이미 해제된
 * struct controller 를 건드리게 된다. 그래서 알림을 끈 직후 반드시 이 함수로
 * 핸들러를 떼어 낸다. 여는 쪽과 정확히 대칭이어야 하므로 pciehp_poll_mode 분기를
 * 그대로 되풀이한다.
 * 동작 단계: 폴 모드면 kthread_stop() 이 스레드에 정지를 요청하고 실제로 빠져나올
 * 때까지 기다린다. 인터럽트 모드면 free_irq() 가 진행 중인 핸들러 실행이 끝날
 * 때까지 기다린 뒤 등록을 해제한다. 두 함수 모두 동기적으로 완료를 보장하므로,
 * 반환 이후에는 핸들러가 다시 불릴 일이 없다.
 * 실행 컨텍스트: 프로세스 문맥. 두 호출 모두 잠들 수 있으므로 IRQ 문맥에서
 * 부르면 안 된다.
 * 에러 경로: 없다. 두 호출 모두 실패를 보고하지 않는다.
 *
 * 호출 체인:
 *   pciehp_remove()/pciehp_probe 실패 경로 [core.c] → pcie_shutdown_notification()
 *     → [pciehp_free_irq] → kthread_stop() 또는 free_irq()
 */
static inline void pciehp_free_irq(struct controller *ctrl)
{
	if (pciehp_poll_mode)	/* [한국어] 열 때와 같은 조건으로 갈라야 짝이 맞는다. 이 값은 부팅 후 바뀌지 않는다. */
		kthread_stop(ctrl->poll_thread);		/* [한국어] pciehp_poll 이 kthread_should_stop() 을 보고 루프를 빠져나올 때까지 블록한다. */
	else
		free_irq(ctrl->pcie->irq, ctrl);		/* [한국어] 핸들러 등록을 해제한다. dev_id 로 ctrl 을 넘겨야 공유 IRQ 중 정확히 우리 핸들러만 떼어 낸다. */
}

/*
 * [한국어]
 * pcie_poll_cmd - Slot Status 를 직접 읽어 Command Completed 를 기다린다
 *
 * @ctrl: 명령을 발행한 컨트롤러.
 * @timeout: 남은 대기 시간(밀리초). 호출자가 jiffies 차이를 ms 로 바꿔 넘긴다.
 * @return: 1 이면 CC 비트를 보고 클리어까지 마쳤다. 0 이면 타임아웃이거나 포트가
 *          응답하지 않아 포기했다. 호출자 pcie_wait_cmd() 는 0 일 때 경고를 찍고
 *          그대로 진행한다 — 명령 자체는 이미 하드웨어에 쓰였기 때문이다.
 *
 * 왜 필요한가: Command Completed 인터럽트가 꺼져 있거나(폴 모드, CCIE 미설정)
 * 하드웨어가 인터럽트를 못 올리는 상황에서도 Slot Control 의 다음 쓰기 전에는
 * 이전 명령의 완료를 확인해야 한다(PCIe r4.0 sec 6.7.3.2). 그 마지막 수단이다.
 * 동작 단계: 10ms 씩 자면서 Slot Status 를 읽고, CC 비트가 서면 그 비트에 1 을
 * 써서 지우고(RW1C) cmd_busy 를 내린 뒤 성공을 반환한다.
 * 실행 컨텍스트: msleep() 을 쓰므로 반드시 프로세스 문맥이다. 호출자가
 * ctrl->ctrl_lock 을 잡은 상태로 부르므로 동시에 두 번 실행될 수 없다.
 * 에러 경로: config 읽기가 0xffff 계열로 돌아오면(PCI_POSSIBLE_ERROR) 포트 자체가
 * 사라진 것이므로 즉시 0 을 돌려 무한 대기를 피한다.
 *
 * 호출 체인:
 *   pcie_do_write_cmd() → pcie_wait_cmd() → [pcie_poll_cmd]
 *     → pcie_capability_read_word() / pcie_capability_write_word()
 */
static int pcie_poll_cmd(struct controller *ctrl, int timeout)
{
	struct pci_dev *pdev = ctrl_dev(ctrl);	/* [한국어] 슬롯 레지스터를 담고 있는 브리지. 여기 config space 를 두드린다. */
	u16 slot_status;	/* [한국어] 읽어 온 Slot Status 원본값. 이벤트 비트와 상태 비트가 섞여 있다. */

	do {	/* [한국어] timeout 이 음수가 될 때까지 반복. 최초 1회는 무조건 읽어 본다. */
		pcie_capability_read_word(pdev, PCI_EXP_SLTSTA, &slot_status);		/* [한국어] PCI Express Capability 기준 오프셋 PCI_EXP_SLTSTA 에서 Slot Status 16비트를 읽는다. */
		if (PCI_POSSIBLE_ERROR(slot_status)) {		/* [한국어] 반환값이 전부 1 이면 config 읽기가 실패한 것이다. 카드가 아니라 포트 자체가 사라진 경우다. */
			ctrl_info(ctrl, "%s: no response from device\n",			/* [한국어] 더 기다려도 소용없음을 알린다. 슬롯이 아니라 브리지가 응답하지 않는 상황이다. */
				  __func__);
			return 0;			/* [한국어] 실패로 반환. 호출자는 타임아웃과 동일하게 취급한다. */
		}

		if (slot_status & PCI_EXP_SLTSTA_CC) {		/* [한국어] Command Completed. 직전에 쓴 Slot Control 명령을 하드웨어가 받아들였다는 뜻이다. */
			pcie_capability_write_word(pdev, PCI_EXP_SLTSTA,			/* [한국어] Slot Status 의 이벤트 비트는 RW1C 다. 1 을 써야 지워지고, 지워야 다음 CC 를 받을 수 있다. */
						   PCI_EXP_SLTSTA_CC);						   /* [한국어] CC 비트만 쓴다. 다른 비트에 0 을 쓰는 것은 무해하며 그 이벤트를 건드리지 않는다. */
			ctrl->cmd_busy = 0;			/* [한국어] 다음 명령을 써도 된다는 표시. pcie_wait_cmd 의 대기 조건이기도 하다. */
			smp_mb();			/* [한국어] 전체 메모리 배리어. cmd_busy=0 이 CC 클리어보다 먼저 보이는 재배치를 막아, 다른 CPU 의 pcie_wait_cmd 가 낡은 상태를 보고 잘못 판단하지 않게 한다. */
			return 1;			/* [한국어] 성공. 하드웨어가 새 명령을 받을 준비가 되었다. */
		}
		msleep(10);		/* [한국어] 10ms 양보. 밀리초 단위 폴링이라 busy-wait 보다 스케줄러에 넘기는 편이 훨씬 낫다. */
		timeout -= 10;		/* [한국어] 잔여 시간 차감. 실제 잔 시간이 조금 길 수 있어 대기가 약간 짧아질 수 있지만 상한만 지키면 되므로 무해하다. */
	} while (timeout >= 0);	/* [한국어] timeout 이 0 이 되는 순간까지는 한 번 더 확인한다. */
	return 0;	/* timeout */		/* [한국어] 시간 초과. CC 를 못 봤지만 명령 자체는 이미 하드웨어에 전달된 상태다. */
}

/*
 * [한국어]
 * pcie_wait_cmd - 직전에 발행한 Slot Control 명령이 끝날 때까지 기다린다
 *
 * @ctrl: 명령을 발행한 컨트롤러. cmd_started 에 발행 시각(jiffies)이,
 *        cmd_busy 에 진행 중 여부가 들어 있다.
 * @return: 없음. 타임아웃해도 호출자를 막지 않고 로그만 남긴다.
 *
 * 왜 필요한가: PCIe 규격은 Slot Control 에 명령을 쓴 뒤 Command Completed 가
 * 올라오기 전에 또 쓰면 안 된다고 규정한다(PCIe r4.0 sec 6.7.3.2). 컨트롤러가
 * CC 를 지원하지 않으면 대신 1초(폴 모드는 2.5초)를 그냥 기다려야 한다. 이
 * 함수가 그 규약을 한 곳에 모은다.
 * 동작 단계: (1) NCCS 가 서 있으면 하드웨어가 CC 를 아예 안 올리므로 대기가 무의미
 * 해 즉시 반환한다. (2) cmd_busy 가 0 이면 이미 끝났으므로 반환한다. (3) 남은
 * 시간을 계산한다. 이미 지났더라도 1 jiffy 는 남겨 두는데, 이는 pcie_poll_cmd 가
 * 밀린 CC 비트를 반드시 한 번은 지우게 하기 위해서다. (4) HPIE 와 CCIE 가 모두
 * 켜져 있으면 인터럽트가 깨워 줄 것이므로 wait_event_timeout 으로 잠들고, 아니면
 * pcie_poll_cmd 로 직접 읽는다.
 * 실행 컨텍스트: 프로세스 문맥. 호출자 pcie_do_write_cmd() 가 ctrl->ctrl_lock 을
 * 잡은 채 부르므로 이 대기 중에는 다른 Slot Control 쓰기가 끼어들 수 없다.
 * 깨우는 쪽은 pciehp_isr() 이 CC 이벤트를 보고 호출하는 wake_up(&ctrl->queue) 다.
 * 에러 경로: 타임아웃이면 ctrl_info 로 어떤 명령이 몇 ms 전에 나갔는지 남긴다.
 * 명령을 되돌리지는 않는다 — 되돌릴 방법이 없기 때문이다.
 *
 * 호출 체인:
 *   pcie_do_write_cmd() → [pcie_wait_cmd]
 *     → wait_event_timeout(ctrl->queue) 또는 pcie_poll_cmd()
 *   (깨우는 쪽) pciehp_isr() → wake_up(&ctrl->queue)
 */
static void pcie_wait_cmd(struct controller *ctrl)
{
	unsigned int msecs = pciehp_poll_mode ? 2500 : 1000;	/* [한국어] 폴 모드는 최대 2.5초, 인터럽트 모드는 1초. 폴 모드는 확인 간격이 성기므로 여유를 더 준다. */
	unsigned long duration = msecs_to_jiffies(msecs);	/* [한국어] 밀리초를 jiffies 로 변환. 커널 시간 비교는 jiffies 단위로 한다. */
	unsigned long cmd_timeout = ctrl->cmd_started + duration;	/* [한국어] 절대 마감 시각 = 명령 발행 시각 + 허용 시간. 대기 시작 시각이 아니라 발행 시각 기준이라는 점이 중요하다. */
	unsigned long now, timeout;	/* [한국어] 현재 시각과 실제로 기다릴 jiffies. */
	int rc;	/* [한국어] 대기 함수의 반환값. 0 이면 타임아웃이다. */

	/*
	 * If the controller does not generate notifications for command
	 * completions, we never need to wait between writes.
	 */
	if (NO_CMD_CMPL(ctrl))	/* [한국어] Slot Capabilities 의 NCCS(No Command Completed Support). 이 비트가 서면 하드웨어는 CC 를 절대 올리지 않으므로 연속 쓰기가 허용된다. */
		return;		/* [한국어] 기다릴 것이 없으니 즉시 복귀. */

	if (!ctrl->cmd_busy)	/* [한국어] 진행 중인 명령이 없다. 첫 명령이거나 이미 CC 를 받아 처리했다. */
		return;		/* [한국어] 대기 불필요. */

	/*
	 * Even if the command has already timed out, we want to call
	 * pcie_poll_cmd() so it can clear PCI_EXP_SLTSTA_CC.
	 */
	now = jiffies;	/* [한국어] 시각 스냅숏. 아래 두 비교에서 같은 값을 써야 일관된다. */
	if (time_before_eq(cmd_timeout, now))	/* [한국어] jiffies 랩어라운드에 안전한 비교. 마감이 이미 지났는가. */
		timeout = 1;		/* [한국어] 0 이 아니라 1 을 주는 이유는 pcie_poll_cmd 를 최소 한 번 돌려 밀린 CC 비트를 지우게 하기 위해서다. */
	else
		timeout = cmd_timeout - now;		/* [한국어] 아직 여유가 있으면 남은 만큼만 기다린다. */

	if (ctrl->slot_ctrl & PCI_EXP_SLTCTL_HPIE &&	/* [한국어] HPIE(Hot-Plug Interrupt Enable): 슬롯 이벤트로 인터럽트를 낼 수 있는가. */
	    ctrl->slot_ctrl & PCI_EXP_SLTCTL_CCIE)	    /* [한국어] CCIE(Command Completed Interrupt Enable): CC 도 인터럽트를 내는가. 둘 다 켜져 있어야 인터럽트가 깨워 준다. */
		rc = wait_event_timeout(ctrl->queue, !ctrl->cmd_busy, timeout);		/* [한국어] cmd_busy 가 0 이 되기를 기다리며 잠든다. pciehp_isr 이 CC 를 보고 wake_up 해 준다. 0 을 반환하면 타임아웃이다. */
	else
		rc = pcie_poll_cmd(ctrl, jiffies_to_msecs(timeout));		/* [한국어] 인터럽트를 못 믿는 경우. 직접 Slot Status 를 폴링한다. jiffies 를 다시 ms 로 되돌려 넘긴다. */

	if (!rc)	/* [한국어] 두 경로 모두 0 이 곧 타임아웃이다. */
		ctrl_info(ctrl, "Timeout on hotplug command %#06x (issued %u msec ago)\n",		/* [한국어] 어떤 Slot Control 값이 몇 ms 전에 나갔는지 남긴다. 펌웨어/하드웨어 문제 진단의 실마리가 된다. */
			  ctrl->slot_ctrl,
			  jiffies_to_msecs(jiffies - ctrl->cmd_started));
}

/*
 * [한국어] CC_ERRATUM_MASK - Command Completed erratum 판정을 위한 Slot Control 마스크
 *
 * Intel CF118 계열 브리지는 Command Completed 를 지원한다고 광고하면서도 실제로는
 * 아래 네 "Control" 필드가 바뀔 때만 CC 를 올린다. Enable 계열 비트만 건드린
 * 쓰기에는 CC 가 영영 오지 않아 pcie_wait_cmd 가 매번 1초를 낭비한다. 그래서
 * pcie_do_write_cmd 는 쓰기 전후로 이 마스크 부분이 그대로면 CC 를 포기하고
 * cmd_busy 를 즉시 내린다.
 * 구성 비트: PCC(Power Controller Control, 슬롯 전원), PIC(Power Indicator
 * Control, 전원 표시등), AIC(Attention Indicator Control, 주의 표시등),
 * EIC(Electromechanical Interlock Control, 전자식 잠금장치).
 * 읽는 자: pcie_do_write_cmd() 한 곳뿐이다.
 */
#define CC_ERRATUM_MASK		(PCI_EXP_SLTCTL_PCC |	\
				 PCI_EXP_SLTCTL_PIC |	\
				 PCI_EXP_SLTCTL_AIC |	\
				 PCI_EXP_SLTCTL_EIC)				 /* [한국어] 네 Control 필드의 합. 이 중 하나라도 값이 달라져야 문제 있는 하드웨어가 CC 를 올린다. */

/*
 * [한국어]
 * pcie_do_write_cmd - Slot Control 레지스터 읽고-고쳐-쓰기의 유일한 진입점
 *
 * @ctrl: 대상 컨트롤러.
 * @cmd: 새로 넣을 비트 값. mask 로 걸러진 부분만 반영된다.
 * @mask: 이번에 건드릴 비트들. mask 밖 비트는 하드웨어의 현재 값이 보존된다.
 * @wait: true 면 쓰기 후 Command Completed 까지 기다린다. 전원 제어처럼 결과가
 *        확정돼야 다음 단계로 갈 수 있는 명령이 true 를 쓴다.
 * @return: 없음. 실패해도 호출자에게 알리지 않고 로그만 남긴다.
 *
 * 왜 필요한가: Slot Control 은 전원, 표시등, 인터럽트 enable 이 한 레지스터에
 * 섞여 있어 반드시 읽고-고쳐-쓰기를 해야 한다. 이 과정이 여러 스레드에서 겹치면
 * 서로의 변경을 덮어쓰므로 ctrl_lock 으로 직렬화한다. 또 규격상 이전 명령이 끝나기
 * 전에 다시 쓸 수 없어 대기 로직도 여기에 함께 둔다.
 * 동작 단계: (1) ctrl_lock 획득 (2) 이전 명령 완료 대기 (3) 현재 값 읽기
 * (4) mask 부분만 교체 (5) cmd_busy 를 세우고 배리어 (6) 소프트웨어 사본
 * ctrl->slot_ctrl 갱신 후 실제 쓰기 (7) 발행 시각 기록 (8) CC erratum 보정
 * (9) 요청 시 완료 대기 (10) 락 해제.
 * 실행 컨텍스트: mutex 와 msleep 을 쓰므로 프로세스 문맥 전용이다. 상태 기계
 * (워크큐), sysfs write(유저 프로세스), pcie_init()(probe) 에서 불린다. 하드 IRQ
 * 에서는 절대 부르면 안 된다.
 * 동시성: ctrl_lock 이 이 함수 전체를 감싸므로 읽기와 쓰기 사이에 다른 쓰기가
 * 끼어들 수 없다. pciehp_isr() 은 이 락을 잡지 않지만 Slot Control 을 쓰지 않고
 * cmd_busy 만 내리므로 충돌하지 않는다. 그 한 지점을 smp_mb 로 보호한다.
 * 에러 경로: 읽기가 모두 1 로 돌아오면 포트가 사라진 것이므로 아무것도 쓰지 않고
 * out 라벨로 빠져 락만 푼다. 쓰기 실패는 config 쓰기 특성상 감지할 수 없다.
 *
 * 호출 체인:
 *   pcie_write_cmd() / pcie_write_cmd_nowait() → [pcie_do_write_cmd]
 *     → pcie_wait_cmd() → pcie_capability_write_word()
 */
static void pcie_do_write_cmd(struct controller *ctrl, u16 cmd,
			      u16 mask, bool wait)
{
	struct pci_dev *pdev = ctrl_dev(ctrl);	/* [한국어] Slot Control 을 담고 있는 브리지. */
	u16 slot_ctrl_orig, slot_ctrl;	/* [한국어] 변경 전 원본과 변경 후 값. 둘을 비교해야 CC erratum 조건을 판정할 수 있다. */

	mutex_lock(&ctrl->ctrl_lock);	/* [한국어] 읽고-고쳐-쓰기 전체를 직렬화한다. 이 락이 없으면 표시등 변경이 전원 명령을 덮어쓸 수 있다. */

	/*
	 * Always wait for any previous command that might still be in progress
	 */
	pcie_wait_cmd(ctrl);	/* [한국어] 이전 명령이 아직 진행 중일 수 있으므로 무조건 먼저 기다린다. 규격이 요구하는 순서다. */

	pcie_capability_read_word(pdev, PCI_EXP_SLTCTL, &slot_ctrl);	/* [한국어] 하드웨어의 현재 Slot Control 을 읽는다. 캐시된 ctrl->slot_ctrl 대신 실제 값을 쓰는 이유는 펌웨어나 다른 주체가 바꿨을 수 있기 때문이다. */
	if (PCI_POSSIBLE_ERROR(slot_ctrl)) {	/* [한국어] 전부 1 이면 포트가 응답하지 않는다. 서프라이즈 제거나 D3cold 진입 중일 수 있다. */
		ctrl_info(ctrl, "%s: no response from device\n", __func__);		/* [한국어] 무응답 기록. 쓰기를 시도하지 않고 빠져나간다. */
		goto out;		/* [한국어] 락 해제만 하고 종료. 아무 부작용도 남기지 않는다. */
	}

	slot_ctrl_orig = slot_ctrl;	/* [한국어] erratum 비교용 원본 보존. */
	slot_ctrl &= ~mask;	/* [한국어] 이번에 건드릴 비트를 먼저 0 으로 비운다. */
	slot_ctrl |= (cmd & mask);	/* [한국어] 비운 자리에 요청 값을 넣는다. cmd 의 mask 밖 비트는 무시되므로 호출자가 실수해도 안전하다. */
	ctrl->cmd_busy = 1;	/* [한국어] 명령 진행 중 표시. 쓰기보다 먼저 세워야 CC 인터럽트가 쓰기 직후 도착해도 놓치지 않는다. */
	smp_mb();	/* [한국어] 전체 메모리 배리어. cmd_busy=1 이 아래 config 쓰기보다 먼저 다른 CPU 에 보이도록 강제한다. 이것이 없으면 CC 를 처리하는 pciehp_isr 이 cmd_busy=0 인 상태를 보고 아무도 깨우지 않아 pcie_wait_cmd 가 헛되이 1초를 기다린다. */
	ctrl->slot_ctrl = slot_ctrl;	/* [한국어] 소프트웨어 사본 갱신. pciehp_isr 과 pcie_wait_cmd 가 HPIE/CCIE 여부를 판단할 때 이 값을 읽는다. */
	pcie_capability_write_word(pdev, PCI_EXP_SLTCTL, slot_ctrl);	/* [한국어] 실제 config space 쓰기. 이 순간 하드웨어가 전원/표시등/인터럽트 설정을 바꾼다. */
	ctrl->cmd_started = jiffies;	/* [한국어] 발행 시각 기록. pcie_wait_cmd 가 이 값을 기준으로 마감 시각을 계산한다. */

	/*
	 * Controllers with the Intel CF118 and similar errata advertise
	 * Command Completed support, but they only set Command Completed
	 * if we change the "Control" bits for power, power indicator,
	 * attention indicator, or interlock.  If we only change the
	 * "Enable" bits, they never set the Command Completed bit.
	 */
	if (pdev->broken_cmd_compl &&	/* [한국어] 이 브리지가 CC erratum 을 가졌는가. quirk_cmd_compl() 이 부팅 초기에 세워 둔 플래그다. */
	    (slot_ctrl_orig & CC_ERRATUM_MASK) == (slot_ctrl & CC_ERRATUM_MASK))	    /* [한국어] 네 Control 필드가 하나도 바뀌지 않았다면 이 하드웨어는 CC 를 올리지 않는다. */
		ctrl->cmd_busy = 0;		/* [한국어] 오지 않을 CC 를 기다리지 않도록 즉시 완료 처리. 이 보정이 없으면 명령마다 1초씩 낭비된다. */

	/*
	 * Optionally wait for the hardware to be ready for a new command,
	 * indicating completion of the above issued command.
	 */
	if (wait)	/* [한국어] 호출자가 결과 확정을 요구했는가. pcie_write_cmd 는 true, pcie_write_cmd_nowait 는 false 다. */
		pcie_wait_cmd(ctrl);		/* [한국어] 하드웨어가 다음 명령을 받을 준비가 될 때까지 블록한다. */

out:
	mutex_unlock(&ctrl->ctrl_lock);	/* [한국어] 성공 경로와 무응답 경로가 공유하는 유일한 해제 지점. */
}

/**
 * pcie_write_cmd - Issue controller command
 * @ctrl: controller to which the command is issued
 * @cmd:  command value written to slot control register
 * @mask: bitmask of slot control register to be modified
 */
/*
 * [한국어]
 * pcie_write_cmd - Slot Control 명령을 발행하고 완료까지 기다린다
 *
 * @ctrl: 대상 컨트롤러.
 * @cmd: 새 비트 값.
 * @mask: 건드릴 비트 범위.
 * @return: 없음.
 *
 * 왜 필요한가: 슬롯 전원 On/Off 나 인터럽트 마스킹처럼 "정말로 적용되었는지"가
 * 다음 단계의 전제인 명령은 Command Completed 를 확인해야 한다. 그 의도를 이름에
 * 드러내려고 wait=true 로 고정한 얇은 래퍼를 둔다.
 * 실행 컨텍스트: 잠들 수 있으므로 프로세스 문맥 전용. 호출자는 상태 기계 또는
 * 초기화 경로다.
 * 에러 경로: 자체 에러 처리는 없고 전부 pcie_do_write_cmd 가 흡수한다.
 *
 * 호출 체인:
 *   pciehp_power_on_slot() / pciehp_power_off_slot() / pcie_disable_notification()
 *   / pcie_enable_interrupt() / pcie_disable_interrupt() → [pcie_write_cmd]
 *     → pcie_do_write_cmd(wait=true)
 */
static void pcie_write_cmd(struct controller *ctrl, u16 cmd, u16 mask)
{
	pcie_do_write_cmd(ctrl, cmd, mask, true);	/* [한국어] wait=true. 하드웨어가 명령을 받아들일 때까지 블록한다. */
}

/* Same as above without waiting for the hardware to latch */
/*
 * [한국어]
 * pcie_write_cmd_nowait - Slot Control 명령을 발행하고 완료를 기다리지 않는다
 *
 * @ctrl: 대상 컨트롤러.
 * @cmd: 새 비트 값.
 * @mask: 건드릴 비트 범위.
 * @return: 없음.
 *
 * 왜 필요한가: 표시등 상태나 이벤트 enable 비트처럼 결과를 즉시 확인할 필요가
 * 없는 명령까지 매번 최대 1초를 기다리면 삽입/제거 처리가 눈에 띄게 느려진다.
 * 다음 명령이 들어오면 어차피 pcie_do_write_cmd 첫머리에서 이전 명령을 기다리므로
 * 규격의 순서 제약은 그대로 지켜진다.
 * 실행 컨텍스트: 여전히 ctrl_lock 을 잡고 이전 명령을 기다릴 수 있으므로 프로세스
 * 문맥 전용이다. "nowait" 는 발행 후 대기를 생략한다는 뜻이지 잠들지 않는다는
 * 뜻이 아니다.
 * 에러 경로: 없음. 전부 pcie_do_write_cmd 가 처리한다.
 *
 * 호출 체인:
 *   pciehp_set_indicators() / pciehp_set_raw_indicator_status() /
 *   pcie_enable_notification() / pcie_init() → [pcie_write_cmd_nowait]
 *     → pcie_do_write_cmd(wait=false)
 */
static void pcie_write_cmd_nowait(struct controller *ctrl, u16 cmd, u16 mask)
{
	pcie_do_write_cmd(ctrl, cmd, mask, false);	/* [한국어] wait=false. 쓰기만 하고 바로 락을 풀어 호출자를 돌려보낸다. */
}

/**
 * pciehp_check_link_active() - Is the link active
 * @ctrl: PCIe hotplug controller
 *
 * Check whether the downstream link is currently active. Note it is
 * possible that the card is removed immediately after this so the
 * caller may need to take it into account.
 *
 * If the hotplug controller itself is not available anymore returns
 * %-ENODEV.
 */
/*
 * [한국어]
 * pciehp_check_link_active - 다운스트림 링크가 현재 살아 있는지 본다
 *
 * @ctrl: 대상 컨트롤러.
 * @return: 1 이면 Data Link Layer 가 활성, 0 이면 비활성, -ENODEV 면 포트 자체가
 *          사라졌다. 호출자는 -ENODEV 를 "슬롯 판정 불가"로 다뤄야 한다(0 이
 *          아니므로 참으로 취급되는 자리에서는 주의가 필요하다).
 *
 * 왜 필요한가: Presence Detect 핀이 없거나 0 으로 고정된 하드웨어에서는 링크가
 * 살아 있다는 사실만이 카드가 꽂혀 있다는 유일한 증거다. 또 링크 다운 이벤트를
 * 받았을 때 그것이 진짜인지 확인하는 데도 쓰인다.
 * 동작 단계: Link Status 를 한 번 읽고 DLLLA(Data Link Layer Link Active) 비트만
 * 본다. 이 비트는 링크 트레이닝이 끝나 DLLP 를 주고받을 수 있는 상태를 뜻한다.
 * 주의: 읽은 직후 카드가 빠질 수 있으므로 반환값은 스냅숏일 뿐이다. 상위 코드가
 * 이를 감안해 재확인한다.
 * 실행 컨텍스트: config 읽기 한 번뿐이라 IRQ 스레드와 프로세스 문맥 양쪽에서
 * 불린다. 잠금은 필요 없고 재진입해도 안전하다.
 * 에러 경로: PCIBIOS_DEVICE_NOT_FOUND 이거나 값이 전부 1 이면 -ENODEV.
 *
 * 호출 체인:
 *   pciehp_card_present_or_link_active() / pciehp_ignore_link_change() /
 *   pciehp_slot_reset() / pciehp_ctrl.c 의 상태 기계 → [pciehp_check_link_active]
 *     → pcie_capability_read_word(PCI_EXP_LNKSTA)
 */
int pciehp_check_link_active(struct controller *ctrl)
{
	struct pci_dev *pdev = ctrl_dev(ctrl);	/* [한국어] Link Status 를 가진 브리지. */
	u16 lnk_status;	/* [한국어] 읽어 온 Link Status 값. */
	int ret;	/* [한국어] 읽기 결과이자 최종 반환값으로 재사용된다. */

	ret = pcie_capability_read_word(pdev, PCI_EXP_LNKSTA, &lnk_status);	/* [한국어] PCI Express Capability 의 Link Status 레지스터를 읽는다. */
	if (ret == PCIBIOS_DEVICE_NOT_FOUND || PCI_POSSIBLE_ERROR(lnk_status))	/* [한국어] 브리지가 응답하지 않거나 값이 전부 1 이면 포트가 사라진 것이다. */
		return -ENODEV;		/* [한국어] 링크 상태를 판정할 근거가 없음을 명확히 알린다. */

	ret = !!(lnk_status & PCI_EXP_LNKSTA_DLLLA);	/* [한국어] DLLLA(Data Link Layer Link Active). 링크 트레이닝이 끝나 DLLP 교환이 가능한 상태를 뜻한다. 논리값 0/1 로 정규화한다. */
	ctrl_dbg(ctrl, "%s: lnk_status = %x\n", __func__, lnk_status);	/* [한국어] 원본 레지스터 값을 통째로 남겨 사후 분석에 쓰게 한다. */

	return ret;	/* [한국어] 1(활성) 또는 0(비활성). */
}

/*
 * [한국어]
 * pci_bus_check_dev - 지정한 버스/devfn 에 장치가 실제로 응답할 때까지 최대 1초 기다린다
 *
 * @bus: 검사할 PCI 버스. 핫플러그 포트의 subordinate 버스가 들어온다.
 * @devfn: 검사할 장치/함수 번호. 호출자는 PCI_DEVFN(0, 0) 만 넘긴다 — pciehp 는
 *         포트 하나에 슬롯 하나(1:1) 구조라 슬롯의 첫 장치가 항상 dev 0 func 0 다.
 * @return: true 면 Vendor ID 를 읽는 데 성공했다. false 면 1초 동안 응답이 없었다.
 *
 * 왜 필요한가: 링크가 올라와도 장치의 config space 가 즉시 응답하지는 않는다.
 * 규격은 링크 업 이후에도 준비 시간을 허용하고, 실제 하드웨어는 그보다 더 늦기도
 * 한다. 여기서 기다려 주지 않으면 곧이어 실행될 pci_scan_slot() 이 빈 슬롯으로
 * 오판해 장치가 열거되지 않는다.
 * 동작 단계: pci_bus_read_dev_vendor_id() 를 성공할 때까지 20ms 간격으로 최대
 * 50회 재시도한다. 두 번 이상 걸렸으면 몇 번 만에 무엇을 읽었는지 pr_debug 로
 * 남긴다. 이 로그는 느린 장치를 식별하는 데 쓴다.
 * 실행 컨텍스트: msleep 을 쓰므로 프로세스 문맥 전용. 실제로는 IRQ 스레드
 * (pciehp_ist) 아래에서 pciehp_check_link_status 를 통해 불린다.
 * 에러 경로: 실패는 false 반환뿐이며, 호출자가 "No device found" 로 보고한다.
 *
 * 호출 체인:
 *   pciehp_ist() → pciehp_handle_presence_or_link_change() [ctrl.c]
 *     → pciehp_check_link_status() → [pci_bus_check_dev]
 *       → pci_bus_read_dev_vendor_id()
 */
static bool pci_bus_check_dev(struct pci_bus *bus, int devfn)
{
	u32 l;	/* [한국어] 읽어 온 32비트. 하위 16비트가 Vendor ID, 상위 16비트가 Device ID 다(config 오프셋 0x00). */
	int count = 0;	/* [한국어] 시도 횟수. 로그 조건 판단에만 쓴다. */
	int delay = 1000, step = 20;	/* [한국어] 총 허용 시간 1000ms 를 20ms 간격으로 나눠 최대 50회 재시도한다. */
	bool found = false;	/* [한국어] 장치 응답 성공 여부. */

	do {	/* [한국어] 최소 한 번은 즉시 읽어 본다. 대부분의 장치는 첫 시도에 응답한다. */
		found = pci_bus_read_dev_vendor_id(bus, devfn, &l, 0);		/* [한국어] Vendor ID 를 읽어 0xffff(장치 없음)나 CRS(0x0001) 가 아닌지 판정한다. 마지막 인자 0 은 CRS 재시도를 이 함수에 맡기지 않겠다는 뜻이다. */
		count++;		/* [한국어] 시도 횟수 누적. */

		if (found)		/* [한국어] 장치가 응답했다. */
			break;			/* [한국어] 더 기다릴 필요 없이 빠져나간다. */

		msleep(step);		/* [한국어] 20ms 양보. 장치의 config space 초기화 시간을 벌어 준다. */
		delay -= step;		/* [한국어] 잔여 허용 시간 차감. */
	} while (delay > 0);	/* [한국어] 1초를 다 쓸 때까지 반복. */

	if (count > 1)	/* [한국어] 한 번에 성공하지 못한 경우에만 기록한다. 정상 장치의 로그를 더럽히지 않기 위해서다. */
		pr_debug("pci %04x:%02x:%02x.%d id reading try %d times with interval %d ms to get %08x\n",		/* [한국어] 도메인:버스:슬롯.함수 와 재시도 횟수, 간격, 최종 읽은 값을 남긴다. 느린 장치를 특정하는 데 쓴다. */
			pci_domain_nr(bus), bus->number, PCI_SLOT(devfn),
			PCI_FUNC(devfn), count, step, l);

	return found;	/* [한국어] 응답 성공 여부를 그대로 반환. */
}

/*
 * [한국어]
 * pcie_wait_for_presence - Presence Detect State 비트가 설 때까지 최대 1.25초 기다린다
 *
 * @pdev: 슬롯 레지스터를 가진 다운스트림 포트.
 * @return: 없음. 타임아웃해도 알리지 않고 그냥 복귀한다.
 *
 * 왜 필요한가: in-band presence 를 끈 슬롯에서는 PDS 가 순수하게 물리 핀
 * (out-of-band) 신호로만 결정된다. 이 신호는 링크 업보다 늦게 안정될 수 있어,
 * 링크만 보고 진행하면 이후 PDS 를 읽는 코드가 "카드 없음"으로 오판한다. 그래서
 * in-band presence 가 꺼진 경우에만 여기서 추가로 기다린다.
 * 동작 단계: 10ms 간격으로 Slot Status 를 읽어 PDS 가 서면 즉시 복귀한다.
 * 1250ms 는 상위 코드가 쓰는 여러 타임아웃과 겹치지 않도록 고른 상한이다.
 * 실행 컨텍스트: msleep 을 쓰므로 프로세스 문맥 전용. pciehp_check_link_status
 * 안에서만 불리므로 IRQ 스레드 아래다.
 * 에러 경로: 타임아웃해도 조용히 반환한다. 뒤이은 pci_bus_check_dev 가 장치
 * 유무를 다시 판정하므로 여기서 실패를 보고할 필요가 없다.
 *
 * 호출 체인:
 *   pciehp_check_link_status() → [pcie_wait_for_presence]
 *     → pcie_capability_read_word(PCI_EXP_SLTSTA)
 */
static void pcie_wait_for_presence(struct pci_dev *pdev)
{
	int timeout = 1250;	/* [한국어] 최대 1250ms. 10ms 간격이므로 최대 125회 확인한다. */
	u16 slot_status;	/* [한국어] 읽어 온 Slot Status. */

	do {	/* [한국어] 타임아웃 전까지 PDS 비트를 반복 확인한다. */
		pcie_capability_read_word(pdev, PCI_EXP_SLTSTA, &slot_status);		/* [한국어] Slot Status 읽기. 여기서는 실패 검사를 하지 않는데, 실패하면 값이 전부 1 이라 PDS 가 선 것처럼 보여 어차피 즉시 빠져나오기 때문이다. */
		if (slot_status & PCI_EXP_SLTSTA_PDS)		/* [한국어] PDS(Presence Detect State). 슬롯에 카드가 물리적으로 꽂혀 있음을 뜻한다. */
			return;			/* [한국어] 확인됐으니 더 기다리지 않는다. */
		msleep(10);		/* [한국어] 10ms 양보. */
		timeout -= 10;		/* [한국어] 잔여 시간 차감. */
	} while (timeout > 0);	/* [한국어] 시간을 다 쓰면 조용히 복귀한다. */
}

/*
 * [한국어]
 * pciehp_check_link_status - 카드 삽입 후 링크와 장치가 쓸 만한 상태인지 종합 판정한다
 *
 * @ctrl: 대상 컨트롤러.
 * @return: 0 이면 열거를 진행해도 된다. -1 이면 링크가 없거나 트레이닝에 실패했
 *          거나 장치가 응답하지 않는다. 호출자 board_added() [ctrl.c] 는 -1 이면
 *          슬롯 전원을 다시 끄고 상태를 OFF 로 되돌린다.
 *
 * 왜 필요한가: 전원을 넣었다고 곧바로 config 접근이 가능한 것은 아니다. 링크
 * 트레이닝 완료, presence 확정, 장치 응답이라는 세 단계를 모두 통과해야 안전하게
 * pci_scan_slot() 을 부를 수 있다. 이 함수가 그 관문이다.
 * 동작 단계: (1) pcie_wait_for_link() 로 링크 업 대기 (2) in-band presence 가
 * 꺼져 있으면 PDS 추가 대기 (3) dev 0 func 0 의 Vendor ID 응답 대기 (4) 장치를
 * 찾았다면 그 사이 쌓인 링크/presence 변화 이벤트를 폐기 (5) Link Status 를 다시
 * 읽어 Link Training 이 끝났고 Negotiated Link Width 가 0 이 아닌지 확인
 * (6) 협상된 속도와 폭을 버스 구조체에 반영 (7) 장치를 못 찾았으면 실패.
 * 실행 컨텍스트: IRQ 스레드(pciehp_ist) 아래의 상태 기계에서 불린다. 잠들 수 있다.
 * 동시성: (4) 단계의 atomic_and 는 하드 IRQ 가 같은 시각에 atomic_or 로 이벤트를
 * 넣을 수 있기 때문에 원자적 연산이어야 한다.
 * 에러 경로: 세 실패 지점 모두 ctrl_info 로 슬롯 이름과 함께 이유를 남기고 -1 을
 * 반환한다. 자원을 잡지 않으므로 되돌릴 것이 없다.
 *
 * 호출 체인:
 *   pciehp_ist() → pciehp_handle_presence_or_link_change() [ctrl.c]
 *     → board_added() [ctrl.c] → [pciehp_check_link_status]
 *       → pcie_wait_for_link() / pcie_wait_for_presence() / pci_bus_check_dev()
 *         / __pcie_update_link_speed()
 */
int pciehp_check_link_status(struct controller *ctrl)
{
	struct pci_dev *pdev = ctrl_dev(ctrl);	/* [한국어] 슬롯 레지스터를 가진 브리지. */
	bool found;	/* [한국어] dev 0 func 0 가 config 응답을 했는지. */
	u16 lnk_status, linksta2;	/* [한국어] Link Status 와 Link Status 2. 후자에는 협상된 링크 속도 관련 필드가 들어 있다. */

	if (!pcie_wait_for_link(pdev, true)) {	/* [한국어] 링크가 활성화될 때까지 기다린다. 두 번째 인자 true 는 "활성 상태가 되기를" 기다린다는 뜻이다. */
		ctrl_info(ctrl, "Slot(%s): No link\n", slot_name(ctrl));		/* [한국어] 링크가 끝내 올라오지 않았다. 카드가 없거나 불량이다. */
		return -1;		/* [한국어] 실패. 상위에서 슬롯을 다시 끈다. */
	}

	if (ctrl->inband_presence_disabled)	/* [한국어] in-band presence 를 끈 슬롯은 PDS 가 늦게 안정되므로 별도로 기다린다. */
		pcie_wait_for_presence(pdev);		/* [한국어] 물리 핀 기반 presence 확정 대기. */

	found = pci_bus_check_dev(ctrl->pcie->port->subordinate,	/* [한국어] 포트 아래 버스의 dev 0 func 0 가 실제로 응답하는지 최대 1초 확인한다. pciehp 는 포트당 슬롯 하나이므로 이 한 지점만 보면 된다. */
					PCI_DEVFN(0, 0));

	/* ignore link or presence changes up to this point */
	if (found)	/* [한국어] 장치가 확인된 경우에만 폐기한다. 못 찾았다면 그 이벤트들이 여전히 유효한 정보다. */
		atomic_and(~(PCI_EXP_SLTSTA_DLLSC | PCI_EXP_SLTSTA_PDC),		/* [한국어] 여기까지 오는 동안 링크가 몇 번 튀며 쌓인 DLLSC/PDC 이벤트를 지운다. 그대로 두면 방금 성공한 삽입을 다시 처리하려 든다. 하드 IRQ 가 동시에 atomic_or 하므로 원자적 AND 여야 한다. */
			   &ctrl->pending_events);

	pcie_capability_read_word(pdev, PCI_EXP_LNKSTA, &lnk_status);	/* [한국어] 트레이닝 결과를 확인하려고 Link Status 를 다시 읽는다. */
	ctrl_dbg(ctrl, "%s: lnk_status = %x\n", __func__, lnk_status);	/* [한국어] 원본 값을 남겨 사후 분석에 쓰게 한다. */
	if ((lnk_status & PCI_EXP_LNKSTA_LT) ||	/* [한국어] LT(Link Training) 가 아직 서 있으면 트레이닝이 끝나지 않았다. */
	    !(lnk_status & PCI_EXP_LNKSTA_NLW)) {	    /* [한국어] NLW(Negotiated Link Width) 가 0 이면 폭 협상 자체가 실패한 것이다. */
		ctrl_info(ctrl, "Slot(%s): Cannot train link: status %#06x\n",		/* [한국어] 어떤 Link Status 값에서 실패했는지 남긴다. 신호 품질 문제 진단의 출발점이다. */
			  slot_name(ctrl), lnk_status);
		return -1;		/* [한국어] 링크가 쓸 수 없는 상태이므로 열거하지 않는다. */
	}

	pcie_capability_read_word(pdev, PCI_EXP_LNKSTA2, &linksta2);	/* [한국어] Link Status 2 를 읽는다. 협상된 속도를 정확히 계산하려면 이 값이 함께 필요하다. */
	__pcie_update_link_speed(ctrl->pcie->port->subordinate, PCIE_HOTPLUG,	/* [한국어] 포트 아래 버스의 cur_bus_speed 등을 갱신한다. PCIE_HOTPLUG 는 갱신 요청의 출처를 표시해 다른 갱신 경로와 구분하게 한다. */
				 lnk_status, linksta2);

	if (!found) {	/* [한국어] 링크는 정상인데 config 응답이 없었던 경우. 카드가 트레이닝만 하고 죽었거나 매우 느린 장치다. */
		ctrl_info(ctrl, "Slot(%s): No device found\n",		/* [한국어] 슬롯 이름과 함께 장치 없음을 기록한다. */
			  slot_name(ctrl));
		return -1;		/* [한국어] 열거를 진행하지 않는다. */
	}

	return 0;	/* [한국어] 링크와 장치 모두 정상. 이제 pci_scan_slot 을 불러도 안전하다. */
}

/*
 * [한국어]
 * __pciehp_link_set - Link Control 의 Link Disable 비트를 세우거나 지운다
 *
 * @ctrl: 대상 컨트롤러.
 * @enable: true 면 링크를 살리고(LD=0), false 면 강제로 죽인다(LD=1).
 * @return: 항상 0. 반환형이 int 인 것은 호출자 pciehp_power_on_slot() 이 실패를
 *          보고할 여지를 남겨 두기 위한 형태일 뿐, 현재 구현은 실패하지 않는다.
 *
 * 왜 필요한가: 슬롯 전원을 넣기만 해서는 링크가 자동으로 다시 트레이닝되지 않을
 * 수 있다. 그래서 전원을 켠 뒤 Link Disable 을 명시적으로 내려 트레이닝을
 * 시작시킨다. Link Disable 은 Slot Control 이 아니라 Link Control 레지스터에 있어
 * 이 파일에서 Slot Control 경로(pcie_write_cmd)를 타지 않는 유일한 쓰기다.
 * 그래서 Command Completed 대기도 필요 없다.
 * 동작 단계: pcie_capability_clear_and_set_word 로 LD 비트만 원자적으로 바꾼다.
 * 이 헬퍼는 내부에서 락을 잡고 읽고-고쳐-쓰기를 하므로 다른 Link Control 사용자
 * (ASPM 등)와 경쟁하지 않는다.
 * 실행 컨텍스트: 프로세스 문맥. 전원 인가 직후에만 불린다.
 * 에러 경로: 없다. config 쓰기 실패는 감지되지 않는다.
 *
 * 호출 체인:
 *   pciehp_power_on_slot() → pciehp_link_enable() → [__pciehp_link_set]
 *     → pcie_capability_clear_and_set_word(PCI_EXP_LNKCTL)
 */
static int __pciehp_link_set(struct controller *ctrl, bool enable)
{
	struct pci_dev *pdev = ctrl_dev(ctrl);	/* [한국어] Link Control 을 가진 브리지. */

	pcie_capability_clear_and_set_word(pdev, PCI_EXP_LNKCTL,	/* [한국어] 읽고-고쳐-쓰기를 커널 헬퍼에 맡긴다. 내부에서 잠금을 잡아 다른 Link Control 변경(ASPM 등)과의 경쟁을 막는다. */
					   PCI_EXP_LNKCTL_LD,					   /* [한국어] 지울 비트: LD(Link Disable). 먼저 지우고 아래 값으로 다시 세운다. */
					   enable ? 0 : PCI_EXP_LNKCTL_LD);					   /* [한국어] 세울 비트. enable 이면 0(=LD 를 지운 채로 두어 링크 트레이닝 허용), 아니면 LD 를 세워 링크를 강제 다운시킨다. */

	return 0;	/* [한국어] 항상 성공. 호출자 인터페이스를 위해 int 를 돌려준다. */
}

/*
 * [한국어]
 * pciehp_link_enable - 다운스트림 링크를 다시 살린다
 *
 * @ctrl: 대상 컨트롤러.
 * @return: __pciehp_link_set() 의 반환값(현재 항상 0).
 *
 * 왜 필요한가: 호출 지점에서 "링크를 켠다"는 의도를 그대로 읽히게 하려는 얇은
 * 래퍼다. 반대 방향(링크 끄기)에 해당하는 래퍼는 현재 이 파일에 없고,
 * __pciehp_link_set(ctrl, false) 를 부르는 코드도 없다. 즉 enable=false 경로는
 * 지금은 쓰이지 않는다.
 * 실행 컨텍스트: pciehp_power_on_slot() 안, 프로세스 문맥.
 * 에러 경로: 반환값을 그대로 전달하고, 0 이 아니면 호출자가 ctrl_err 를 남긴다.
 *
 * 호출 체인:
 *   board_added() [ctrl.c] → pciehp_power_on_slot() → [pciehp_link_enable]
 *     → __pciehp_link_set(ctrl, true)
 */
static int pciehp_link_enable(struct controller *ctrl)
{
	return __pciehp_link_set(ctrl, true);	/* [한국어] enable=true. Link Disable 비트를 내려 트레이닝을 허용한다. */
}

/*
 * [한국어]
 * pciehp_get_raw_indicator_status - 두 표시등 필드를 가공 없이 4비트 값으로 돌려준다
 *
 * @hotplug_slot: PCI 핫플러그 코어가 넘겨주는 슬롯. to_ctrl() 로 controller 를 얻는다.
 * @status: 결과를 담을 곳. AIC 와 PIC 를 합친 4비트가 들어간다.
 * @return: 항상 0(성공).
 *
 * 왜 필요한가: 일부 플랫폼은 표준 Attention/Power 표시등 의미 대신 자체 규약으로
 * 이 두 필드를 쓴다(pci_dev 의 hotplug_user_indicators). 그런 슬롯에서는 커널이
 * 값을 해석하지 않고 유저 공간에 원본을 그대로 노출해야 한다. 그래서 이 함수는
 * ON/BLINK/OFF 로 번역하지 않는다.
 * 동작 단계: 런타임 PM 참조를 잡아 포트가 서스펜드되지 않게 한 뒤 Slot Control 을
 * 읽고, AIC(비트 7:6)와 PIC(비트 9:8)를 한 덩어리로 뽑아 6비트 오른쪽으로 민다.
 * 결과의 하위 2비트가 AIC, 그 위 2비트가 PIC 다.
 * 실행 컨텍스트: sysfs 읽기, 즉 유저 프로세스 문맥. 잠들 수 있다.
 * 에러 경로: 읽기 실패를 검사하지 않는다. 포트가 사라졌다면 0xf 가 보인다.
 *
 * 호출 체인:
 *   sysfs 의 attention 파일 읽기 → hotplug_slot_ops->get_attention_status
 *     (init_slot() [core.c] 이 hotplug_user_indicators 일 때 이 함수를 등록)
 *     → [pciehp_get_raw_indicator_status] → pcie_capability_read_word()
 */
int pciehp_get_raw_indicator_status(struct hotplug_slot *hotplug_slot,
				    u8 *status)
{
	struct controller *ctrl = to_ctrl(hotplug_slot);	/* [한국어] container_of 로 hotplug_slot 을 감싸고 있는 struct controller 를 복원한다. */
	struct pci_dev *pdev = ctrl_dev(ctrl);	/* [한국어] Slot Control 을 가진 브리지. */
	u16 slot_ctrl;	/* [한국어] 읽어 온 Slot Control 값. */

	pci_config_pm_runtime_get(pdev);	/* [한국어] 런타임 PM 참조 획득. 이것이 없으면 포트가 D3 로 내려가 config 읽기가 전부 1 로 돌아올 수 있다. */
	pcie_capability_read_word(pdev, PCI_EXP_SLTCTL, &slot_ctrl);	/* [한국어] Slot Control 전체를 읽는다. */
	pci_config_pm_runtime_put(pdev);	/* [한국어] 참조 반납. 읽기가 끝났으니 포트는 다시 서스펜드돼도 된다. */
	*status = (slot_ctrl & (PCI_EXP_SLTCTL_AIC | PCI_EXP_SLTCTL_PIC)) >> 6;	/* [한국어] AIC(비트 7:6)와 PIC(비트 9:8)를 함께 뽑아 6비트 내린다. 결과는 하위 2비트=AIC, 상위 2비트=PIC 인 4비트 값이며, 해석은 유저 공간에 맡긴다. */
	return 0;	/* [한국어] 이 경로는 실패하지 않는다. */
}

/*
 * [한국어]
 * pciehp_get_attention_status - 주의 표시등 상태를 켜짐/깜빡임/꺼짐으로 번역해 돌려준다
 *
 * @hotplug_slot: PCI 핫플러그 코어가 넘겨주는 슬롯.
 * @status: 결과. 1=On, 2=Blink, 0=Off, 0xFF=규격상 예약값(해석 불가).
 * @return: 항상 0(성공).
 *
 * 왜 필요한가: sysfs 의 attention 파일은 0/1/2 라는 표준화된 값을 노출하기로
 * 약속돼 있다. 하드웨어의 2비트 AIC 인코딩(01=On, 10=Blink, 11=Off)은 그 순서와
 * 다르므로 여기서 반드시 번역해야 한다. 값 00 은 규격상 예약이라 0xFF 로 보고한다.
 * 동작 단계: 런타임 PM 참조를 잡고 Slot Control 을 읽어 AIC 필드만 switch 로
 * 매핑한다. 디버그 로그에는 이 레지스터의 절대 config 오프셋도 함께 남기는데,
 * lspci 출력과 대조하기 위해서다.
 * 실행 컨텍스트: sysfs 읽기, 유저 프로세스 문맥.
 * 에러 경로: 읽기 실패를 따로 검사하지 않는다. 실패하면 AIC 가 11 로 보여 Off 로
 * 보고된다.
 *
 * 호출 체인:
 *   sysfs attention 읽기 → hotplug_slot_ops->get_attention_status
 *     (init_slot() [core.c] 이 ATTN_LED 인 슬롯에 이 함수를 등록)
 *     → [pciehp_get_attention_status] → pcie_capability_read_word()
 */
int pciehp_get_attention_status(struct hotplug_slot *hotplug_slot, u8 *status)
{
	struct controller *ctrl = to_ctrl(hotplug_slot);	/* [한국어] hotplug_slot 에서 controller 복원. */
	struct pci_dev *pdev = ctrl_dev(ctrl);	/* [한국어] Slot Control 을 가진 브리지. */
	u16 slot_ctrl;	/* [한국어] 읽어 온 Slot Control. */

	pci_config_pm_runtime_get(pdev);	/* [한국어] 런타임 PM 참조 획득. config 접근 동안 포트를 D0 에 묶어 둔다. */
	pcie_capability_read_word(pdev, PCI_EXP_SLTCTL, &slot_ctrl);	/* [한국어] Slot Control 읽기. */
	pci_config_pm_runtime_put(pdev);	/* [한국어] 참조 반납. */
	ctrl_dbg(ctrl, "%s: SLOTCTRL %x, value read %x\n", __func__,	/* [한국어] 디버그 로그. pci_pcie_cap() 은 PCI Express Capability 의 시작 오프셋이라, 여기에 PCI_EXP_SLTCTL 을 더하면 lspci 로 볼 수 있는 절대 config 오프셋이 된다. */
		 pci_pcie_cap(ctrl->pcie->port) + PCI_EXP_SLTCTL, slot_ctrl);

	switch (slot_ctrl & PCI_EXP_SLTCTL_AIC) {	/* [한국어] AIC(Attention Indicator Control) 2비트 필드만 남긴다. */
	case PCI_EXP_SLTCTL_ATTN_IND_ON:	/* [한국어] 인코딩 01: 표시등 켜짐. */
		*status = 1;	/* On */		/* [한국어] sysfs 규약의 1(On) 로 번역. */
		break;
	case PCI_EXP_SLTCTL_ATTN_IND_BLINK:	/* [한국어] 인코딩 10: 깜빡임. 사용자가 슬롯을 찾도록 유도하는 상태다. */
		*status = 2;	/* Blink */		/* [한국어] sysfs 규약의 2(Blink). */
		break;
	case PCI_EXP_SLTCTL_ATTN_IND_OFF:	/* [한국어] 인코딩 11: 꺼짐. */
		*status = 0;	/* Off */		/* [한국어] sysfs 규약의 0(Off). 하드웨어 인코딩과 순서가 다르다는 점에 주의. */
		break;
	default:	/* [한국어] 남은 인코딩 00 은 PCIe 규격에서 예약값이다. */
		*status = 0xFF;		/* [한국어] 해석 불가를 뜻하는 0xFF 로 보고한다. */
		break;
	}

	return 0;	/* [한국어] 이 경로는 실패하지 않는다. */
}

/*
 * [한국어]
 * pciehp_get_power_status - 슬롯 전원 컨트롤러가 켜져 있는지 읽는다
 *
 * @ctrl: 대상 컨트롤러.
 * @status: 결과. 1=On, 0=Off, 0xFF=해석 불가.
 * @return: 없음.
 *
 * 왜 필요한가: 상태 기계는 자신이 마지막으로 내린 명령이 아니라 하드웨어의 실제
 * 전원 상태를 알아야 할 때가 있다. 특히 부팅 직후 pcie_init() 은 빈 슬롯에 펌웨어가
 * 켜 둔 전원을 발견해 꺼야 하고, 슬롯을 켜라는 요청이 왔을 때 이미 켜져 있으면
 * 중복 동작을 피해야 한다.
 * 동작 단계: Slot Control 을 읽어 PCC(Power Controller Control) 비트만 본다.
 * 규격 인코딩은 0=Power On, 1=Power Off 로 직관과 반대이므로 반드시 번역한다.
 * 실행 컨텍스트: 상태 기계(워크큐/IRQ 스레드), sysfs 읽기, pcie_init 초기화 등
 * 여러 프로세스 문맥에서 불린다. 잠금은 잡지 않는다 — 단일 읽기라 원자적이다.
 * 에러 경로: 읽기 실패는 검사하지 않는다. 실패 시 PCC 가 1 로 보여 Off 로 보고된다.
 *
 * 호출 체인:
 *   pcie_init() / get_power_status() [core.c] / pciehp_ctrl.c 의 상태 점검
 *     → [pciehp_get_power_status] → pcie_capability_read_word(PCI_EXP_SLTCTL)
 */
void pciehp_get_power_status(struct controller *ctrl, u8 *status)
{
	struct pci_dev *pdev = ctrl_dev(ctrl);	/* [한국어] Slot Control 을 가진 브리지. */
	u16 slot_ctrl;	/* [한국어] 읽어 온 Slot Control. */

	pcie_capability_read_word(pdev, PCI_EXP_SLTCTL, &slot_ctrl);	/* [한국어] Slot Control 읽기. 전원 상태는 Status 가 아니라 Control 레지스터에 있다. */
	ctrl_dbg(ctrl, "%s: SLOTCTRL %x value read %x\n", __func__,	/* [한국어] 디버그 로그. 절대 config 오프셋과 읽은 값을 함께 남긴다. */
		 pci_pcie_cap(ctrl->pcie->port) + PCI_EXP_SLTCTL, slot_ctrl);

	switch (slot_ctrl & PCI_EXP_SLTCTL_PCC) {	/* [한국어] PCC(Power Controller Control) 비트만 남긴다. */
	case PCI_EXP_SLTCTL_PWR_ON:	/* [한국어] 인코딩 0: 전원 인가 상태. */
		*status = 1;	/* On */		/* [한국어] 켜짐을 1 로 보고. */
		break;
	case PCI_EXP_SLTCTL_PWR_OFF:	/* [한국어] 인코딩 1: 전원 차단 상태. 값과 의미가 반대라는 점이 함정이다. */
		*status = 0;	/* Off */		/* [한국어] 꺼짐을 0 으로 보고. */
		break;
	default:	/* [한국어] 단일 비트라 여기 올 수 없지만 방어적으로 둔다. */
		*status = 0xFF;		/* [한국어] 해석 불가. */
		break;
	}
}

/*
 * [한국어]
 * pciehp_get_latch_status - MRL(Manual Retention Latch) 센서 상태를 읽는다
 *
 * @ctrl: 대상 컨트롤러.
 * @status: 결과. 1 이면 래치가 열려 있고 0 이면 닫혀 있다.
 * @return: 없음.
 *
 * 왜 필요한가: MRL 은 카드를 물리적으로 고정하는 걸쇠다. 걸쇠가 열려 있는 동안은
 * 사용자가 카드를 뽑는 중일 수 있으므로 슬롯을 켜서는 안 된다. 상태 기계가 슬롯을
 * 켜기 전에 이 값을 확인하고, sysfs 의 latch 파일도 이 값을 노출한다.
 * 동작 단계: Slot Status 를 읽어 MRLSS(MRL Sensor State) 비트를 0/1 로 정규화한다.
 * 이 함수는 MRL 센서가 없는 슬롯에서도 불릴 수 있지만, 그런 슬롯에서는 상위 코드가
 * MRL_SENS() 로 걸러 애초에 부르지 않는다.
 * 실행 컨텍스트: 프로세스 문맥. 단일 읽기라 잠금이 없다.
 * 에러 경로: 읽기 실패를 검사하지 않는다. 실패 시 전부 1 이라 "열림"으로 보인다.
 *
 * 호출 체인:
 *   get_latch_status() [core.c] / pciehp_ctrl.c 의 슬롯 켜기 전 점검
 *     → [pciehp_get_latch_status] → pcie_capability_read_word(PCI_EXP_SLTSTA)
 */
void pciehp_get_latch_status(struct controller *ctrl, u8 *status)
{
	struct pci_dev *pdev = ctrl_dev(ctrl);	/* [한국어] Slot Status 를 가진 브리지. */
	u16 slot_status;	/* [한국어] 읽어 온 Slot Status. */

	pcie_capability_read_word(pdev, PCI_EXP_SLTSTA, &slot_status);	/* [한국어] Slot Status 읽기. 래치 상태는 이벤트가 아니라 상태 비트다. */
	*status = !!(slot_status & PCI_EXP_SLTSTA_MRLSS);	/* [한국어] MRLSS(MRL Sensor State). 1 이면 걸쇠가 열려 있다. 논리값으로 정규화한다. */
}

/**
 * pciehp_card_present() - Is the card present
 * @ctrl: PCIe hotplug controller
 *
 * Function checks whether the card is currently present in the slot and
 * in that case returns true. Note it is possible that the card is
 * removed immediately after the check so the caller may need to take
 * this into account.
 *
 * If the hotplug controller itself is not available anymore returns
 * %-ENODEV.
 */
/*
 * [한국어]
 * pciehp_card_present - Presence Detect State 하나만으로 카드 유무를 판정한다
 *
 * @ctrl: 대상 컨트롤러.
 * @return: 1=카드 있음, 0=없음, -ENODEV=포트 자체가 사라져 판정 불가.
 *
 * 왜 필요한가: 링크 상태와 무관하게 "슬롯에 물체가 꽂혀 있는가"만 알아야 하는
 * 지점이 있다. 예를 들어 링크가 죽은 뒤에도 카드가 남아 있다면 그것은 카드 제거가
 * 아니라 링크 장애다. 그 구분에 이 함수가 쓰인다.
 * 동작 단계: Slot Status 를 한 번 읽고 PDS 비트만 본다.
 * 주의: 읽은 직후 카드가 빠질 수 있으므로 스냅숏일 뿐이다. -ENODEV 는 음수라
 * 조건식에서 참으로 평가되므로 호출자가 명시적으로 구분해야 한다 —
 * pciehp_card_present_or_link_active() 는 실제로 이 값을 그대로 되돌려 준다.
 * 실행 컨텍스트: IRQ 스레드와 프로세스 문맥 양쪽. 잠금 없음, 재진입 안전.
 * 에러 경로: 브리지가 응답하지 않으면 -ENODEV.
 *
 * 호출 체인:
 *   pciehp_card_present_or_link_active() / pciehp_ctrl.c 의 상태 판정
 *     → [pciehp_card_present] → pcie_capability_read_word(PCI_EXP_SLTSTA)
 */
int pciehp_card_present(struct controller *ctrl)
{
	struct pci_dev *pdev = ctrl_dev(ctrl);	/* [한국어] Slot Status 를 가진 브리지. */
	u16 slot_status;	/* [한국어] 읽어 온 Slot Status. */
	int ret;	/* [한국어] config 읽기 결과 코드. */

	ret = pcie_capability_read_word(pdev, PCI_EXP_SLTSTA, &slot_status);	/* [한국어] Slot Status 읽기. */
	if (ret == PCIBIOS_DEVICE_NOT_FOUND || PCI_POSSIBLE_ERROR(slot_status))	/* [한국어] 브리지가 응답하지 않거나 값이 전부 1 이면 판정할 근거가 없다. */
		return -ENODEV;		/* [한국어] 0/1 과 명확히 구분되는 오류값을 돌려준다. */

	return !!(slot_status & PCI_EXP_SLTSTA_PDS);	/* [한국어] PDS(Presence Detect State) 를 0/1 로 정규화해 반환. */
}

/**
 * pciehp_card_present_or_link_active() - whether given slot is occupied
 * @ctrl: PCIe hotplug controller
 *
 * Unlike pciehp_card_present(), which determines presence solely from the
 * Presence Detect State bit, this helper also returns true if the Link Active
 * bit is set.  This is a concession to broken hotplug ports which hardwire
 * Presence Detect State to zero, such as Wilocity's [1ae9:0200].
 *
 * Returns: %1 if the slot is occupied and %0 if it is not. If the hotplug
 *	    port is not present anymore returns %-ENODEV.
 */
/*
 * [한국어]
 * pciehp_card_present_or_link_active - presence 또는 링크 중 하나라도 살아 있으면 점유로 본다
 *
 * @ctrl: 대상 컨트롤러.
 * @return: 1=슬롯 점유, 0=비어 있음, -ENODEV=포트가 사라짐.
 *
 * 왜 필요한가: Presence Detect State 를 0 으로 하드와이어해 버린 망가진 포트가
 * 실제로 존재한다(주석의 Wilocity 1ae9:0200). 그런 하드웨어에서 PDS 만 믿으면
 * 멀쩡히 동작 중인 카드를 없다고 판정해 전원을 끊는다. 그래서 PDS 가 0 일 때
 * Link Active 를 두 번째 증거로 쓴다.
 * 동작 단계: 먼저 pciehp_card_present() 를 부르고, 0 이 아니면(1 또는 -ENODEV)
 * 그대로 반환한다. 0 일 때만 pciehp_check_link_active() 로 다시 물어본다.
 * 실행 컨텍스트: IRQ 스레드와 프로세스 문맥 양쪽. 두 번의 config 읽기 사이에
 * 상태가 바뀔 수 있으므로 결과는 여전히 스냅숏이다.
 * 에러 경로: 두 하위 함수 중 어느 쪽이 -ENODEV 를 돌려도 그대로 전달된다.
 *
 * 호출 체인:
 *   pcie_init() / pciehp_probe()/pciehp_resume 계열 [core.c] / pciehp_ctrl.c
 *     → [pciehp_card_present_or_link_active]
 *       → pciehp_card_present() → pciehp_check_link_active()
 */
int pciehp_card_present_or_link_active(struct controller *ctrl)
{
	int ret;	/* [한국어] 첫 판정 결과를 담을 변수. */

	ret = pciehp_card_present(ctrl);	/* [한국어] 먼저 물리 presence 를 본다. */
	if (ret)	/* [한국어] 1(있음)이든 -ENODEV(판정 불가)든 0 이 아니면 더 볼 필요가 없다. */
		return ret;		/* [한국어] 그대로 전달. 음수 오류도 호출자에게 그대로 보인다. */

	return pciehp_check_link_active(ctrl);	/* [한국어] PDS 가 0 인 경우에만 링크 활성 여부를 두 번째 근거로 확인한다. */
}

/*
 * [한국어]
 * pciehp_query_power_fault - 전원 결함(Power Fault)이 감지된 적 있는지 읽는다
 *
 * @ctrl: 대상 컨트롤러.
 * @return: 1 이면 PFD 비트가 서 있다(전원 결함 발생). 0 이면 정상.
 *
 * 왜 필요한가: 슬롯 전원을 켠 뒤 카드가 과전류를 끌면 전원 컨트롤러가 스스로
 * 차단하고 Slot Status 의 PFD 를 세운다. 상태 기계는 카드를 붙이기 전에 이 값을
 * 확인해, 결함이 있으면 열거를 포기하고 슬롯을 다시 끈다.
 * 동작 단계: Slot Status 를 읽어 PFD(Power Fault Detected) 비트를 0/1 로 정규화한다.
 * 이 함수는 비트를 지우지 않는다 — 클리어는 pciehp_power_on_slot() 이 전원을
 * 다시 넣을 때 수행한다.
 * 실행 컨텍스트: IRQ 스레드 아래의 상태 기계. 잠금 없음.
 * 에러 경로: 읽기 실패를 검사하지 않아 포트가 사라지면 결함으로 보고된다. 어차피
 * 그 상황에서도 열거를 포기하는 것이 옳으므로 결과적으로 안전한 방향이다.
 *
 * 호출 체인:
 *   pciehp_ist() → pciehp_handle_presence_or_link_change() → board_added() [ctrl.c]
 *     → [pciehp_query_power_fault] → pcie_capability_read_word(PCI_EXP_SLTSTA)
 */
int pciehp_query_power_fault(struct controller *ctrl)
{
	struct pci_dev *pdev = ctrl_dev(ctrl);	/* [한국어] Slot Status 를 가진 브리지. */
	u16 slot_status;	/* [한국어] 읽어 온 Slot Status. */

	pcie_capability_read_word(pdev, PCI_EXP_SLTSTA, &slot_status);	/* [한국어] Slot Status 읽기. */
	return !!(slot_status & PCI_EXP_SLTSTA_PFD);	/* [한국어] PFD(Power Fault Detected) 를 0/1 로 정규화. 여기서 지우지는 않는다. */
}

/*
 * [한국어]
 * pciehp_set_raw_indicator_status - 유저 공간이 준 4비트를 두 표시등 필드에 그대로 쓴다
 *
 * @hotplug_slot: PCI 핫플러그 코어가 넘겨주는 슬롯.
 * @status: 유저 공간이 sysfs 로 쓴 값. 하위 2비트가 AIC, 그 위 2비트가 PIC 에
 *          해당하도록 FIELD_PREP 가 배치한다.
 * @return: 항상 0(성공).
 *
 * 왜 필요한가: hotplug_user_indicators 플랫폼에서는 표시등의 의미를 커널이 알지
 * 못하므로 유저 공간이 정한 비트 조합을 손대지 않고 전달해야 한다. 표준 의미로
 * 번역하는 pciehp_set_indicators() 와 짝을 이루는 반대편 인터페이스다.
 * 동작 단계: 런타임 PM 참조를 잡고, FIELD_PREP 로 status 를 AIC 와 PIC 를 합친
 * 마스크 위치에 배치한 뒤 Slot Control 에 쓴다. 완료를 기다리지 않는
 * pcie_write_cmd_nowait 를 쓰는 이유는 표시등은 결과 확인이 필요 없기 때문이다.
 * 실행 컨텍스트: sysfs 쓰기, 유저 프로세스 문맥. pcie_do_write_cmd 안에서 뮤텍스를
 * 잡고 잠들 수 있다.
 * 에러 경로: 없다. 잘못된 값은 마스크 밖으로 잘려 무시된다.
 *
 * 호출 체인:
 *   sysfs attention 쓰기 → hotplug_slot_ops->set_attention_status
 *     (init_slot() [core.c] 이 hotplug_user_indicators 일 때 등록)
 *     → [pciehp_set_raw_indicator_status] → pcie_write_cmd_nowait()
 */
int pciehp_set_raw_indicator_status(struct hotplug_slot *hotplug_slot,
				    u8 status)
{
	struct controller *ctrl = to_ctrl(hotplug_slot);	/* [한국어] hotplug_slot 에서 controller 복원. */
	struct pci_dev *pdev = ctrl_dev(ctrl);	/* [한국어] Slot Control 을 가진 브리지. */

	pci_config_pm_runtime_get(pdev);	/* [한국어] 런타임 PM 참조 획득. 쓰기가 끝날 때까지 포트를 D0 에 묶어 둔다. */

	/* Attention and Power Indicator Control bits are supported */
	pcie_write_cmd_nowait(ctrl, FIELD_PREP(PCI_EXP_SLTCTL_AIC | PCI_EXP_SLTCTL_PIC, status),	/* [한국어] FIELD_PREP 가 status 를 AIC|PIC 마스크 위치로 옮긴다. 즉 status 의 비트 0-1 이 AIC(비트 7:6), 비트 2-3 이 PIC(비트 9:8)로 간다. */
			      PCI_EXP_SLTCTL_AIC | PCI_EXP_SLTCTL_PIC);			      /* [한국어] 건드릴 범위도 같은 두 필드로 한정한다. 나머지 Slot Control 비트는 하드웨어 값이 보존된다. */
	pci_config_pm_runtime_put(pdev);	/* [한국어] 런타임 PM 참조 반납. */
	return 0;	/* [한국어] 이 경로는 실패하지 않는다. */
}

/**
 * pciehp_set_indicators() - set attention indicator, power indicator, or both
 * @ctrl: PCIe hotplug controller
 * @pwr: one of:
 *	PCI_EXP_SLTCTL_PWR_IND_ON
 *	PCI_EXP_SLTCTL_PWR_IND_BLINK
 *	PCI_EXP_SLTCTL_PWR_IND_OFF
 * @attn: one of:
 *	PCI_EXP_SLTCTL_ATTN_IND_ON
 *	PCI_EXP_SLTCTL_ATTN_IND_BLINK
 *	PCI_EXP_SLTCTL_ATTN_IND_OFF
 *
 * Either @pwr or @attn can also be INDICATOR_NOOP to leave that indicator
 * unchanged.
 */
/*
 * [한국어]
 * pciehp_set_indicators - 전원 표시등과 주의 표시등을 한 번의 명령으로 설정한다
 *
 * @ctrl: 대상 컨트롤러.
 * @pwr: PCI_EXP_SLTCTL_PWR_IND_ON / _BLINK / _OFF 중 하나, 또는 INDICATOR_NOOP
 *       (-1)이면 전원 표시등을 건드리지 않는다.
 * @attn: PCI_EXP_SLTCTL_ATTN_IND_ON / _BLINK / _OFF 중 하나, 또는 INDICATOR_NOOP.
 * @return: 없음.
 *
 * 왜 필요한가: 두 표시등은 같은 Slot Control 레지스터에 있으므로 따로 쓰면 명령이
 * 두 번 나가고 그때마다 Command Completed 를 기다려야 한다. 상태 전이마다 두 등을
 * 함께 바꾸는 경우가 많아 한 번에 처리하는 것이 훨씬 싸다. 또 하드웨어에 없는
 * 표시등에 쓰는 것을 막아야 하므로 능력 검사도 여기서 한다.
 * 동작 단계: PWR_LED()/ATTN_LED() 로 해당 표시등이 존재하는지 확인하고, 존재하고
 * NOOP 가 아닌 것만 cmd 와 mask 에 누적한다. 최종적으로 바꿀 것이 하나라도 있으면
 * 완료를 기다리지 않는 쓰기를 한 번 수행한다.
 * 실행 컨텍스트: 상태 기계(IRQ 스레드/워크큐)와 sysfs 양쪽. 프로세스 문맥이다.
 * 에러 경로: 없다. 바꿀 것이 없으면 아무 명령도 내보내지 않는다.
 *
 * 호출 체인:
 *   pciehp_ist() 의 Power Fault 처리 / pciehp_ctrl.c 의 모든 상태 전이 /
 *   set_attention_status() [core.c] → [pciehp_set_indicators]
 *     → pcie_write_cmd_nowait() → pcie_do_write_cmd()
 */
void pciehp_set_indicators(struct controller *ctrl, int pwr, int attn)
{
	u16 cmd = 0, mask = 0;	/* [한국어] 누적할 값과 건드릴 범위. 둘 다 0 으로 시작해 조건에 맞는 것만 더한다. */

	if (PWR_LED(ctrl) && pwr != INDICATOR_NOOP) {	/* [한국어] Slot Capabilities 의 PIP(Power Indicator Present) 가 있고, 호출자가 변경을 요청했는가. */
		cmd |= (pwr & PCI_EXP_SLTCTL_PIC);		/* [한국어] 전달받은 값에서 PIC 필드만 취한다. 호출자가 잘못된 비트를 섞어도 걸러진다. */
		mask |= PCI_EXP_SLTCTL_PIC;		/* [한국어] 이번 쓰기가 PIC 필드를 건드린다고 표시. */
	}

	if (ATTN_LED(ctrl) && attn != INDICATOR_NOOP) {	/* [한국어] Slot Capabilities 의 AIP(Attention Indicator Present) 가 있고, 호출자가 변경을 요청했는가. */
		cmd |= (attn & PCI_EXP_SLTCTL_AIC);		/* [한국어] AIC 필드만 취해 누적. */
		mask |= PCI_EXP_SLTCTL_AIC;		/* [한국어] AIC 필드를 건드린다고 표시. */
	}

	if (cmd) {	/* [한국어] 실제로 세울 비트가 하나도 없으면 명령을 낭비하지 않는다. 다만 값이 전부 0 인 조합(둘 다 인코딩 00)은 여기서 걸러져 쓰이지 않는데, 00 은 규격상 예약값이라 실제 호출자가 쓰지 않는다. */
		pcie_write_cmd_nowait(ctrl, cmd, mask);		/* [한국어] 표시등은 결과 확인이 필요 없으므로 완료를 기다리지 않는다. */
		ctrl_dbg(ctrl, "%s: SLOTCTRL %x write cmd %x\n", __func__,		/* [한국어] 어떤 값을 어느 절대 오프셋에 썼는지 남긴다. */
			 pci_pcie_cap(ctrl->pcie->port) + PCI_EXP_SLTCTL, cmd);
	}
}

/*
 * [한국어]
 * pciehp_power_on_slot - 슬롯에 전원을 넣고 링크를 살린다
 *
 * @ctrl: 대상 컨트롤러.
 * @return: 0 이면 성공. 음수면 링크 활성화에 실패했다(현재 구현상 실제로는
 *          발생하지 않는다). 호출자 board_added() [ctrl.c] 는 0 이 아니면
 *          슬롯을 다시 끄고 상태를 되돌린다.
 *
 * 왜 필요한가: 카드가 꽂혔다고 판단되면 실제로 전원을 인가해야 링크 트레이닝이
 * 시작되고 config space 가 살아난다. 그 전에 이전 전원 결함의 잔재를 지워야
 * 새로 발생하는 결함을 구분할 수 있다.
 * 동작 단계: (1) Slot Status 의 PFD 가 남아 있으면 1 을 써서 지운다 (2) 소프트웨어
 * 쪽 power_fault_detected 도 내린다 (3) PCC 를 Power On 으로 바꾸고 Command
 * Completed 까지 기다린다 (4) Link Disable 을 내려 링크 트레이닝을 허용한다.
 * 실행 컨텍스트: IRQ 스레드 아래의 상태 기계. pcie_write_cmd 안에서 잠들 수 있다.
 * 에러 경로: 링크 활성화 실패는 ctrl_err 로 남기고 그대로 반환한다. 전원 인가
 * 자체의 실패는 감지할 수 없고, 대신 이어지는 pciehp_check_link_status() 가
 * 걸러낸다.
 *
 * 호출 체인:
 *   pciehp_ist() → pciehp_handle_presence_or_link_change() → board_added() [ctrl.c]
 *     → [pciehp_power_on_slot] → pcie_write_cmd() / pciehp_link_enable()
 */
int pciehp_power_on_slot(struct controller *ctrl)
{
	struct pci_dev *pdev = ctrl_dev(ctrl);	/* [한국어] Slot Status 와 Slot Control 을 가진 브리지. */
	u16 slot_status;	/* [한국어] 읽어 온 Slot Status. */
	int retval;	/* [한국어] 링크 활성화 결과이자 최종 반환값. */

	/* Clear power-fault bit from previous power failures */
	pcie_capability_read_word(pdev, PCI_EXP_SLTSTA, &slot_status);	/* [한국어] 이전 전원 결함의 흔적이 남아 있는지 먼저 확인한다. */
	if (slot_status & PCI_EXP_SLTSTA_PFD)	/* [한국어] PFD 가 서 있으면 지워야 한다. 지우지 않으면 이번 전원 인가 후 새로 생긴 결함인지 구분할 수 없다. */
		pcie_capability_write_word(pdev, PCI_EXP_SLTSTA,		/* [한국어] RW1C 이므로 그 비트에 1 을 써서 지운다. */
					   PCI_EXP_SLTSTA_PFD);					   /* [한국어] PFD 비트만 지정. 다른 이벤트 비트는 건드리지 않는다. */
	ctrl->power_fault_detected = 0;	/* [한국어] 소프트웨어 쪽 중복 보고 억제 플래그도 함께 초기화한다. 이것이 pciehp_isr 이 다음 결함을 보고할 수 있게 하는 조건이다. */

	pcie_write_cmd(ctrl, PCI_EXP_SLTCTL_PWR_ON, PCI_EXP_SLTCTL_PCC);	/* [한국어] PCC 를 Power On(인코딩 0) 으로. 완료를 기다려야 전원이 실제로 인가된 뒤 링크를 건드릴 수 있다. */
	ctrl_dbg(ctrl, "%s: SLOTCTRL %x write cmd %x\n", __func__,	/* [한국어] 어느 절대 오프셋에 무슨 값을 썼는지 남긴다. */
		 pci_pcie_cap(ctrl->pcie->port) + PCI_EXP_SLTCTL,
		 PCI_EXP_SLTCTL_PWR_ON);

	retval = pciehp_link_enable(ctrl);	/* [한국어] Link Disable 을 내려 링크 트레이닝을 허용한다. 전원만으로는 링크가 다시 올라오지 않을 수 있다. */
	if (retval)	/* [한국어] 0 이 아니면 실패. 현재 __pciehp_link_set 은 항상 0 이라 실질적으로 진입하지 않는 방어 코드다. */
		ctrl_err(ctrl, "%s: Can not enable the link!\n", __func__);		/* [한국어] 실패를 기록한다. */

	return retval;	/* [한국어] 결과를 그대로 상위 상태 기계에 전달. */
}

/*
 * [한국어]
 * pciehp_power_off_slot - 슬롯 전원을 차단한다
 *
 * @ctrl: 대상 컨트롤러.
 * @return: 없음.
 *
 * 왜 필요한가: 카드를 제거했거나 열거에 실패했거나 사용자가 슬롯을 끄라고 했을 때
 * 실제로 전력을 끊어야 한다. 전원이 끊기면 그 슬롯 뒤의 장치는 config space,
 * BAR, 인터럽트 어느 것으로도 접근할 수 없게 되므로, 상위 코드는 반드시 장치를
 * 먼저 커널에서 제거한 뒤에 이 함수를 불러야 한다.
 * 동작 단계: PCC 를 Power Off(인코딩 1) 로 바꾸고 Command Completed 를 기다린다.
 * 기다리는 이유는 전원이 실제로 내려간 뒤에야 상태를 OFF 로 확정할 수 있기
 * 때문이다.
 * 실행 컨텍스트: 상태 기계(IRQ 스레드/워크큐)와 pcie_init() 초기화 경로.
 * 프로세스 문맥이며 pcie_write_cmd 안에서 잠들 수 있다.
 * 에러 경로: 없다. 실패해도 알 방법이 없고, 타임아웃이면 pcie_wait_cmd 가 로그를
 * 남긴다.
 *
 * 호출 체인:
 *   pciehp_ctrl.c 의 remove_board()/전이 실패 경로, pcie_init() 의 빈 슬롯 정리
 *     → [pciehp_power_off_slot] → pcie_write_cmd() → pcie_do_write_cmd()
 */
void pciehp_power_off_slot(struct controller *ctrl)
{
	pcie_write_cmd(ctrl, PCI_EXP_SLTCTL_PWR_OFF, PCI_EXP_SLTCTL_PCC);	/* [한국어] PCC 를 Power Off(인코딩 1) 로. 완료를 기다려 실제 차단을 확인한다. */
	ctrl_dbg(ctrl, "%s: SLOTCTRL %x write cmd %x\n", __func__,	/* [한국어] 어느 절대 오프셋에 무슨 값을 썼는지 남긴다. */
		 pci_pcie_cap(ctrl->pcie->port) + PCI_EXP_SLTCTL,
		 PCI_EXP_SLTCTL_PWR_OFF);
}

/*
 * [한국어]
 * pciehp_device_replaced - 슬롯의 장치가 그 사이 다른 장치로 바뀌었는지 판정한다
 *
 * @ctrl: 대상 컨트롤러. ctrl->dsn 에 pcie_init() 시점의 Device Serial Number 가
 *        저장돼 있다.
 * @return: true 면 다른 장치이거나 장치가 사라졌다. false 면 같은 장치 그대로다.
 *
 * 왜 필요한가: 시스템이 잠든 사이, 또는 링크가 잠깐 끊긴 사이에 사용자가 카드를
 * 다른 것으로 바꿔 꽂을 수 있다. 커널이 이를 눈치채지 못하면 이전 장치의 드라이버가
 * 새 하드웨어를 조작하게 되어 위험하다. 그래서 신원 정보를 여러 겹으로 대조한다.
 * 동작 단계: (1) 포트가 이미 disconnected 면 판정 자체가 무의미하므로 false
 * (2) dev 0 func 0 의 pci_dev 가 없으면 장치가 사라진 것이므로 true (3) Vendor/
 * Device ID(오프셋 0x00) 와 Class/Revision(오프셋 0x08) 을 커널이 기억하는 값과
 * 대조 (4) 일반 헤더 타입이면 Subsystem ID(오프셋 0x2c) 도 대조 (5) 마지막으로
 * Device Serial Number(확장 capability) 를 비교한다. DSN 은 같은 모델 사이도
 * 구분하는 유일한 값이라 가장 강한 증거다.
 * 실행 컨텍스트: 리줌 콜백(프로세스 문맥)과 pciehp_ignore_link_change()(IRQ 스레드)
 * 에서 불린다. pci_get_slot 이 잠금을 잡으므로 하드 IRQ 에서는 부를 수 없다.
 * 자원 관리: pci_get_slot 이 올린 참조는 __free(pci_dev_put) 정리 속성이 함수를
 * 벗어날 때 자동으로 내려 준다. 그래서 각 return 마다 pci_dev_put 을 부르지 않는다.
 * 에러 경로: config 읽기가 실패하면(0 이 아닌 반환) 곧바로 "교체됨"으로 본다.
 * 확인할 수 없으면 안전한 쪽(장치를 다시 열거하는 쪽)으로 기울이는 설계다.
 *
 * 호출 체인:
 *   pciehp_resume 계열 [core.c] / pciehp_ignore_link_change()
 *     → [pciehp_device_replaced] → pci_get_slot() / pci_read_config_dword()
 *       / pci_get_dsn()
 */
bool pciehp_device_replaced(struct controller *ctrl)
{
	struct pci_dev *pdev __free(pci_dev_put) = NULL;	/* [한국어] __free(pci_dev_put) 정리 속성. 이 변수가 스코프를 벗어나는 모든 경로에서 참조가 자동 반납된다. NULL 초기화가 필수인데, 값을 넣기 전에 return 해도 정리 함수가 불리기 때문이다. */
	u32 reg;	/* [한국어] config 에서 읽어 온 32비트 값을 담는 임시 변수. 세 번 재사용된다. */

	if (pci_dev_is_disconnected(ctrl->pcie->port))	/* [한국어] 포트가 이미 사라진 상태면 아무 config 도 읽을 수 없다. */
		return false;		/* [한국어] 교체가 아니라 판정 불가이므로 false 를 돌려 상위가 다른 경로로 처리하게 둔다. */

	pdev = pci_get_slot(ctrl->pcie->port->subordinate, PCI_DEVFN(0, 0));	/* [한국어] 포트 아래 버스에서 dev 0 func 0 의 pci_dev 를 찾아 참조를 올린다. pciehp 는 포트당 슬롯 하나라 이 한 지점만 본다. */
	if (!pdev)	/* [한국어] 커널이 알고 있던 장치가 이미 제거된 상태다. */
		return true;		/* [한국어] 교체된 것으로 간주해 상위가 재열거하도록 한다. */

	if (pci_read_config_dword(pdev, PCI_VENDOR_ID, &reg) ||	/* [한국어] config 오프셋 0x00: 하위 16비트 Vendor ID, 상위 16비트 Device ID. 읽기 실패면 즉시 교체로 본다. */
	    reg != (pdev->vendor | (pdev->device << 16)) ||	    /* [한국어] 커널이 기억하는 vendor 와 device 를 같은 32비트 배치로 합쳐 비교한다. */
	    pci_read_config_dword(pdev, PCI_CLASS_REVISION, &reg) ||	    /* [한국어] config 오프셋 0x08: 하위 8비트 Revision ID, 상위 24비트 Class Code. */
	    reg != (pdev->revision | (pdev->class << 8)))	    /* [한국어] revision 과 class 를 같은 배치로 합쳐 비교한다. class 는 24비트라 8비트 왼쪽으로 민다. */
		return true;		/* [한국어] 하나라도 다르면 다른 장치다. */

	if (pdev->hdr_type == PCI_HEADER_TYPE_NORMAL &&	/* [한국어] Subsystem ID 는 일반(type 0) 헤더에만 있다. 브리지(type 1) 에는 같은 오프셋에 다른 필드가 있어 읽으면 오판한다. */
	    (pci_read_config_dword(pdev, PCI_SUBSYSTEM_VENDOR_ID, &reg) ||	    /* [한국어] config 오프셋 0x2c: 하위 16비트 Subsystem Vendor ID, 상위 16비트 Subsystem Device ID. */
	     reg != (pdev->subsystem_vendor | (pdev->subsystem_device << 16))))	     /* [한국어] 커널이 기억하는 서브시스템 식별자와 비교. 같은 칩을 쓰는 다른 보드를 구분한다. */
		return true;		/* [한국어] 서브시스템이 다르면 다른 보드다. */

	if (pci_get_dsn(pdev) != ctrl->dsn)	/* [한국어] Device Serial Number 확장 capability 를 읽어 저장해 둔 값과 비교한다. 같은 모델의 개체까지 구분하는 가장 강한 증거다. DSN 이 없는 장치는 0 이 나오므로, 원래도 0 이었다면 이 검사는 아무것도 걸러내지 못한다. */
		return true;		/* [한국어] 시리얼이 다르면 확실히 다른 개체다. */

	return false;	/* [한국어] 모든 검사를 통과했다. 같은 장치가 그대로 있다. */
}

/*
 * [한국어]
 * pciehp_ignore_link_change - 이미 원인을 아는 링크 변화 이벤트를 폐기하고, 필요하면 되살린다
 *
 * @ctrl: 대상 컨트롤러.
 * @pdev: 슬롯 레지스터를 가진 브리지.
 * @irq: 이 컨트롤러의 IRQ 번호. 하드 IRQ 와의 동기화에 쓴다.
 * @ignored_events: 폐기할 이벤트 비트(항상 DLLSC, in-band presence 가 켜져 있으면
 *                  PDC 도 포함).
 * @return: 없음.
 *
 * 왜 필요한가: DPC 복구, Secondary Bus Reset, D3cold 진입, 펌웨어 업데이트 같은
 * 정상 동작도 링크를 잠깐 죽였다 살린다. 이를 그대로 처리하면 커널이 멀쩡한 장치를
 * 제거했다가 다시 열거해 버린다. 그래서 원인을 아는 링크 변화는 버린다.
 * 그런데 무조건 버리면 진짜로 링크가 죽은 경우까지 놓치므로, 버린 직후 실제 상태를
 * 확인해 문제가 있으면 이벤트를 인위적으로 다시 만들어 넣는다.
 * 동작 단계: (1) synchronize_hardirq 로 진행 중인 하드 IRQ 가 끝나기를 기다린다.
 * 이 대기가 없으면 지운 직후 같은 이벤트가 다시 적재될 수 있다. (2) atomic_and 로
 * pending_events 에서 해당 비트를 지운다. (3) 폴 모드에서는 하드 IRQ 가 비트를
 * 지워 주지 않으므로 Slot Status 에도 직접 1 을 써서 지운다. (4) 링크가 죽어 있거나
 * 장치가 바뀌었으면 pciehp_request 로 같은 이벤트를 다시 넣어 정상 경로로 처리한다.
 * 실행 컨텍스트: pciehp_ist(), 즉 IRQ 스레드. synchronize_hardirq 와
 * down_read_nested 가 잠들 수 있으므로 하드 IRQ 에서는 부를 수 없다.
 * 동시성: reset_lock 을 읽기 모드로 잡는 이유는 이 판정과 재요청이 진행되는 동안
 * pciehp_reset_slot() 의 쓰기 잠금과 겹치지 않게 하기 위해서다. ctrl->depth 를
 * 서브클래스로 넘기는 것은 중첩된 핫플러그 포트에서 같은 종류의 락을 여러 겹
 * 잡을 때 lockdep 이 거짓 경보를 내지 않게 하기 위함이다.
 * 에러 경로: 없다. 판정이 애매하면 이벤트를 되살리는 쪽으로 기운다.
 *
 * 호출 체인:
 *   pciehp_ist() → [pciehp_ignore_link_change]
 *     → synchronize_hardirq() / pciehp_check_link_active()
 *       / pciehp_device_replaced() / pciehp_request() [ctrl.c]
 */
static void pciehp_ignore_link_change(struct controller *ctrl,
				      struct pci_dev *pdev, int irq,
				      u16 ignored_events)
{
	/*
	 * Ignore link changes which occurred while waiting for DPC recovery.
	 * Could be several if DPC triggered multiple times consecutively.
	 * Also ignore link changes caused by Secondary Bus Reset, etc.
	 */
	synchronize_hardirq(irq);	/* [한국어] 이 IRQ 의 하드 핸들러가 다른 CPU 에서 실행 중이면 끝날 때까지 기다린다. 그래야 아래에서 지운 비트가 곧바로 다시 채워지는 경쟁을 막을 수 있다. */
	atomic_and(~ignored_events, &ctrl->pending_events);	/* [한국어] 폐기 대상 비트만 원자적으로 지운다. 하드 IRQ 가 동시에 atomic_or 로 다른 비트를 넣을 수 있으므로 읽고-쓰기가 아니라 원자적 AND 여야 한다. */
	if (pciehp_poll_mode)	/* [한국어] 폴 모드에서는 하드 IRQ 가 돌지 않아 Slot Status 의 이벤트 비트가 그대로 남아 있다. */
		pcie_capability_write_word(pdev, PCI_EXP_SLTSTA,		/* [한국어] 그래서 하드웨어 레지스터에도 직접 1 을 써서 지운다(RW1C). 인터럽트 모드에서는 pciehp_isr 이 이미 지웠으므로 불필요하다. */
					   ignored_events);
	ctrl_info(ctrl, "Slot(%s): Link Down/Up ignored\n", slot_name(ctrl));	/* [한국어] 무엇을 무시했는지 사용자에게 알린다. 이 로그가 보이는데도 장치가 사라졌다면 아래 재요청 경로를 의심해야 한다. */

	/*
	 * If the link is unexpectedly down after successful recovery,
	 * the corresponding link change may have been ignored above.
	 * Synthesize it to ensure that it is acted on.
	 */
	down_read_nested(&ctrl->reset_lock, ctrl->depth);	/* [한국어] reset_lock 을 읽기 모드로 획득. 리셋 중에는 이 판정을 하지 않도록 막는다. depth 는 중첩 포트에서 lockdep 서브클래스로 쓰인다. */
	if (!pciehp_check_link_active(ctrl) || pciehp_device_replaced(ctrl))	/* [한국어] 링크가 실제로 죽어 있거나 장치가 바뀌었으면, 방금 버린 이벤트가 사실은 진짜였다는 뜻이다. */
		pciehp_request(ctrl, ignored_events);		/* [한국어] 같은 이벤트를 인위적으로 다시 큐에 넣어 정상 처리 경로를 태운다. pciehp_request() [ctrl.c] 는 pending_events 에 atomic_or 로 넣고, 폴 모드가 아니면 irq_wake_thread 로 IRQ 스레드를 깨운다. */
	up_read(&ctrl->reset_lock);	/* [한국어] reset_lock 해제. */
}

/*
 * [한국어]
 * pciehp_isr - 핫플러그 하드 인터럽트 핸들러(top-half)
 *
 * @irq: 발생한 IRQ 번호. 폴 모드에서는 IRQ_NOTCONNECTED 가 들어온다.
 * @dev_id: request_threaded_irq 에 넘긴 struct controller 포인터.
 * @return: IRQ_NONE 이면 내 이벤트가 아니다(공유 IRQ 규약상 중요하다).
 *          IRQ_HANDLED 면 여기서 다 처리했다. IRQ_WAKE_THREAD 면 pciehp_ist 가
 *          이어서 처리해야 한다.
 *
 * 왜 필요한가: 실제 핫플러그 처리는 뮤텍스와 msleep, 심지어 PCI 열거까지 필요해
 * 하드 IRQ 문맥에서 할 수 없다. 그래서 이 단계에서는 하드웨어 레지스터를 읽어
 * 이벤트 비트를 걷어내 즉시 클리어하는 일만 하고, 나머지는 스레드로 넘긴다.
 * 레지스터를 빨리 클리어해야 하는 이유는 MSI 모드에서 이벤트 비트가 남아 있으면
 * 다음 인터럽트가 아예 발행되지 않기 때문이다(PCIe r5.0 sec 6.7.3.4).
 * 동작 단계: (1) 포트가 D3cold 이거나 HPIE 가 꺼져 있으면 내 인터럽트가 아니다
 * (2) 부모의 런타임 PM 참조를 잡아 config 접근 가능 상태를 보장한다. 부모가 잠들어
 * 있으면 여기서 깨울 수 없으므로 IRQ 를 마스크하고 RERUN_ISR 플래그와 함께 스레드로
 * 넘긴다 (3) Slot Status 를 읽어 이벤트 비트만 남긴다 (4) 이미 보고한 전원 결함은
 * 중복 보고하지 않는다 (5) 이벤트 비트를 써서 클리어하고, MSI 모드면 다시 읽어
 * 그 사이 새로 선 비트를 확인한다 (6) Command Completed 는 대기자가 있을 수 있어
 * 스레드로 미루지 않고 즉시 처리한다 (7) 나머지를 pending_events 에 원자적으로
 * 적재하고 스레드를 깨운다.
 * 실행 컨텍스트: 하드 IRQ. 절대 잠들 수 없고, 스핀락도 IRQ 안전한 것만 쓸 수 있다.
 * 폴 모드에서는 예외적으로 pciehp_poll() 이 프로세스 문맥에서 직접 호출한다.
 * 동시성: pending_events 를 atomic_or 로만 건드린다. 소비자인 pciehp_ist 는
 * atomic_xchg 로 통째로 가져가므로 둘 사이에 락이 필요 없다. cmd_busy 를 내릴 때는
 * smp_mb 로 순서를 보장한 뒤 wake_up 한다.
 * 에러 경로: config 읽기가 전부 1 이면 참조만 반납하고 IRQ_NONE 을 돌려준다.
 * 공유 IRQ 이므로 함부로 IRQ_HANDLED 를 반환하면 다른 서비스의 인터럽트가 묻힌다.
 *
 * 호출 체인:
 *   하드웨어 인터럽트 → [pciehp_isr] → (IRQ_WAKE_THREAD) → pciehp_ist()
 *   폴 모드: pciehp_poll() → [pciehp_isr]
 *   재실행: pciehp_ist() → [pciehp_isr]
 */
static irqreturn_t pciehp_isr(int irq, void *dev_id)
{
	struct controller *ctrl = (struct controller *)dev_id;	/* [한국어] 등록 시 넘긴 dev_id 를 컨트롤러로 되돌린다. 공유 IRQ 에서 어느 인스턴스인지 구분하는 유일한 근거다. */
	struct pci_dev *pdev = ctrl_dev(ctrl);	/* [한국어] 슬롯 레지스터를 가진 브리지. */
	struct device *parent = pdev->dev.parent;	/* [한국어] 브리지의 부모 장치(대개 상위 브리지 또는 호스트 브리지). 런타임 PM 참조를 여기에 건다. */
	u16 status, events = 0;	/* [한국어] 읽은 Slot Status 와, 최종적으로 스레드에 넘길 이벤트 누적값. */

	/*
	 * Interrupts only occur in D3hot or shallower and only if enabled
	 * in the Slot Control register (PCIe r4.0, sec 6.7.3.4).
	 */
	if (pdev->current_state == PCI_D3cold ||	/* [한국어] D3cold 에서는 포트가 config 에 응답하지 않으므로 이 인터럽트는 우리 것일 수 없다(PCIe r4.0 sec 6.7.3.4). */
	    (!(ctrl->slot_ctrl & PCI_EXP_SLTCTL_HPIE) && !pciehp_poll_mode))	    /* [한국어] HPIE 가 꺼져 있는데 폴 모드도 아니면 하드웨어가 우리에게 인터럽트를 낼 이유가 없다. 공유 IRQ 이므로 남의 것이라고 판단한다. */
		return IRQ_NONE;		/* [한국어] 내 인터럽트가 아님을 알린다. 이 반환이 정확해야 공유 IRQ 의 다른 서비스가 정상 동작한다. */

	/*
	 * Keep the port accessible by holding a runtime PM ref on its parent.
	 * Defer resume of the parent to the IRQ thread if it's suspended.
	 * Mask the interrupt until then.
	 */
	if (parent) {	/* [한국어] 최상위 브리지에는 부모가 없을 수 있다. */
		pm_runtime_get_noresume(parent);		/* [한국어] 참조만 올리고 resume 은 하지 않는다. 하드 IRQ 문맥이라 잠들 수 있는 resume 을 부를 수 없기 때문이다. */
		if (!pm_runtime_active(parent)) {		/* [한국어] 부모가 이미 서스펜드돼 있으면 config 접근이 위험하다. */
			pm_runtime_put(parent);			/* [한국어] 방금 올린 참조를 되돌린다. */
			disable_irq_nosync(irq);			/* [한국어] IRQ 를 마스크한다. 마스크하지 않으면 원인을 지우지 못한 채 인터럽트 폭풍이 난다. nosync 인 이유는 하드 IRQ 문맥에서 자기 자신의 완료를 기다릴 수 없기 때문이다. */
			atomic_or(RERUN_ISR, &ctrl->pending_events);			/* [한국어] "스레드에서 ISR 을 다시 돌려라"는 내부 플래그. 이벤트 비트와 겹치지 않도록 비트 17 을 쓴다(pciehp.h 의 RERUN_ISR). */
			return IRQ_WAKE_THREAD;			/* [한국어] 스레드에서 부모를 깨운 뒤 재시도하도록 넘긴다. */
		}
	}

read_status: /* [한국어] MSI 모드에서 Slot Status 를 다시 읽기 위한 재진입 지점. */
	pcie_capability_read_word(pdev, PCI_EXP_SLTSTA, &status);	/* [한국어] Slot Status 읽기. 이벤트 비트와 상태 비트가 함께 들어 있다. */
	if (PCI_POSSIBLE_ERROR(status)) {	/* [한국어] 전부 1 이면 포트가 사라졌다. */
		ctrl_info(ctrl, "%s: no response from device\n", __func__);		/* [한국어] 무응답 기록. */
		if (parent)		/* [한국어] 부모 참조를 잡았다면 반드시 반납해야 한다. */
			pm_runtime_put(parent);
		return IRQ_NONE;		/* [한국어] 읽을 수 없으니 내 이벤트라고 주장하지 않는다. */
	}

	/*
	 * Slot Status contains plain status bits as well as event
	 * notification bits; right now we only want the event bits.
	 */
	status &= PCI_EXP_SLTSTA_ABP | PCI_EXP_SLTSTA_PFD |	/* [한국어] ABP(Attention Button Pressed): 사용자가 슬롯 버튼을 눌렀다. PFD(Power Fault Detected): 전원 결함. */
		  PCI_EXP_SLTSTA_PDC | PCI_EXP_SLTSTA_CC |		  /* [한국어] PDC(Presence Detect Changed): 카드 삽입/제거. CC(Command Completed): 직전 Slot Control 명령 완료. */
		  PCI_EXP_SLTSTA_DLLSC;		  /* [한국어] DLLSC(Data Link Layer State Changed): 링크가 올라오거나 내려갔다. MRLSC 는 여기 없는데, pcie_enable_notification 이 MRL 변화 알림을 켜지 않기 때문이다. */

	/*
	 * If we've already reported a power fault, don't report it again
	 * until we've done something to handle it.
	 */
	if (ctrl->power_fault_detected)	/* [한국어] 이미 한 번 보고한 전원 결함인가. 이 플래그는 pciehp_power_on_slot 에서만 내려간다. */
		status &= ~PCI_EXP_SLTSTA_PFD;		/* [한국어] 중복 보고를 막는다. 그대로 두면 결함이 지속되는 동안 인터럽트 폭풍이 난다(주석의 TBD 항목과 같은 이유다). */
	else if (status & PCI_EXP_SLTSTA_PFD)	/* [한국어] 처음 보는 전원 결함이면 */
		ctrl->power_fault_detected = true;		/* [한국어] 보고했다고 표시해 다음부터 억제한다. */

	events |= status;	/* [한국어] 이번 라운드에서 걷은 이벤트를 누적한다. MSI 재읽기 루프를 돌면 여러 번 OR 된다. */
	if (!events) {	/* [한국어] 걷은 것이 하나도 없으면 우리 인터럽트가 아니다. */
		if (parent)		/* [한국어] 부모 참조 반납. */
			pm_runtime_put(parent);
		return IRQ_NONE;		/* [한국어] 공유 IRQ 규약대로 남에게 넘긴다. */
	}

	if (status) {	/* [한국어] 이번에 읽은 값에 이벤트 비트가 있으면 반드시 지워야 한다. */
		pcie_capability_write_word(pdev, PCI_EXP_SLTSTA, status);		/* [한국어] RW1C. 방금 읽은 비트들에 1 을 써서 지운다. 읽은 값 그대로 쓰므로 그 사이 새로 선 비트는 건드리지 않는다. */

		/*
		 * In MSI mode, all event bits must be zero before the port
		 * will send a new interrupt (PCIe Base Spec r5.0 sec 6.7.3.4).
		 * So re-read the Slot Status register in case a bit was set
		 * between read and write.
		 */
		if (pci_dev_msi_enabled(pdev) && !pciehp_poll_mode)		/* [한국어] MSI/MSI-X 모드인가. 폴 모드에서는 재읽기가 무의미하다. */
			goto read_status;			/* [한국어] 규격상 모든 이벤트 비트가 0 이어야 다음 MSI 가 발행된다. 읽기와 쓰기 사이에 새 비트가 섰다면 지금 지우지 않으면 영영 인터럽트가 오지 않으므로 다시 읽는다. */
	}

	ctrl_dbg(ctrl, "pending interrupts %#06x from Slot Status\n", events);	/* [한국어] 최종적으로 어떤 이벤트를 걷었는지 남긴다. */
	if (parent)	/* [한국어] 여기부터는 config 접근을 하지 않으므로 */
		pm_runtime_put(parent);		/* [한국어] 부모의 런타임 PM 참조를 반납해도 안전하다. */

	/*
	 * Command Completed notifications are not deferred to the
	 * IRQ thread because it may be waiting for their arrival.
	 */
	if (events & PCI_EXP_SLTSTA_CC) {	/* [한국어] Command Completed 는 pcie_wait_cmd 가 잠들어 기다리고 있을 수 있다. */
		ctrl->cmd_busy = 0;		/* [한국어] 명령이 끝났음을 표시. */
		smp_mb();		/* [한국어] 전체 메모리 배리어. cmd_busy=0 이 아래 wake_up 보다 먼저 보여야, 깨어난 쪽이 조건을 다시 검사했을 때 참이 된다. 순서가 뒤바뀌면 깨웠는데도 조건이 거짓이라 다시 잠드는 잃어버린 깨우기가 생긴다. */
		wake_up(&ctrl->queue);		/* [한국어] ctrl->queue 에서 기다리는 pcie_wait_cmd 를 깨운다. */

		if (events == PCI_EXP_SLTSTA_CC)		/* [한국어] 걷은 이벤트가 CC 하나뿐이면 */
			return IRQ_HANDLED;			/* [한국어] 스레드를 깨울 필요가 없다. 여기서 완결된다. */

		events &= ~PCI_EXP_SLTSTA_CC;		/* [한국어] CC 는 처리했으니 스레드에 넘길 목록에서 뺀다. */
	}

	if (pdev->ignore_hotplug) {	/* [한국어] 상위 코드가 이 포트의 핫플러그를 무시하라고 표시한 경우(예: D3cold 진입 절차, 드라이버가 자체적으로 링크를 내리는 경우). */
		ctrl_dbg(ctrl, "ignoring hotplug event %#06x\n", events);		/* [한국어] 무엇을 버렸는지 남긴다. */
		return IRQ_HANDLED;		/* [한국어] 인터럽트는 우리 것이 맞으므로 처리했다고 보고하되, 아무 동작도 하지 않는다. */
	}

	/* Save pending events for consumption by IRQ thread. */
	atomic_or(events, &ctrl->pending_events);	/* [한국어] 스레드가 소비할 이벤트를 원자적으로 누적한다. 소비자는 atomic_xchg 로 통째로 가져가므로 이 OR 만으로 락 없는 인계가 성립한다. */
	return IRQ_WAKE_THREAD;	/* [한국어] IRQ 스레드를 깨운다. 커널이 pciehp_ist 를 프로세스 문맥에서 실행해 준다. */
}

/*
 * [한국어]
 * pciehp_ist - 핫플러그 스레드 인터럽트 핸들러(bottom-half)
 *
 * @irq: IRQ 번호. 폴 모드에서는 IRQ_NOTCONNECTED.
 * @dev_id: struct controller 포인터.
 * @return: IRQ_HANDLED 이면 이벤트를 처리했다. IRQ_NONE 이면 처리할 것이 없었다.
 *
 * 왜 필요한가: 실제 핫플러그 처리는 뮤텍스를 잡고, 최대 1초씩 기다리고, PCI 열거와
 * 드라이버 바인딩까지 유발한다. 이 모두는 프로세스 문맥이 필요하다. 그래서 하드
 * IRQ 가 걷어 놓은 이벤트를 여기서 소비한다.
 * 동작 단계: (1) ist_running 을 세워 다른 코드가 이 핸들러의 실행 여부를 알 수
 * 있게 한다 (2) 포트의 런타임 PM 참조를 잡는다. 여기서는 프로세스 문맥이라 실제로
 * resume 까지 할 수 있다 (3) RERUN_ISR 이 서 있으면 하드 IRQ 가 포트에 접근하지
 * 못했던 경우이므로 pciehp_isr 을 직접 다시 돌리고 마스크했던 IRQ 를 푼다
 * (4) 하드 IRQ 와 동기화한 뒤 pending_events 를 통째로 가져온다 (5) Attention
 * Button, Power Fault 를 처리한다 (6) DPC 복구나 리셋이 원인인 링크 변화는 버린다
 * (7) reset_lock 을 읽기 모드로 잡고 슬롯 비활성 요청 또는 삽입/제거를 처리한다
 * (8) 정리하고 대기자를 깨운다.
 * 우선순위 규칙: DISABLE_SLOT 요청이 PDC/DLLSC 보다 우선한다. 사용자가 명시적으로
 * 끄라고 한 것이 하드웨어가 관측한 변화보다 강한 의사 표시이기 때문이다.
 * 실행 컨텍스트: IRQ 스레드(프로세스 문맥). 잠들 수 있다. 폴 모드에서는
 * pciehp_poll() 이 자기 스레드에서 직접 호출한다. 컨트롤러당 한 번에 하나만
 * 실행된다 — 커널의 스레드 IRQ 구조가 그것을 보장한다.
 * 동시성: pending_events 를 atomic_xchg 로 비우므로 하드 IRQ 가 동시에 새 이벤트를
 * 넣어도 잃어버리지 않는다. reset_lock 은 pciehp_reset_slot 의 쓰기 잠금과 배타적
 * 이어서, 리셋 도중에는 삽입/제거 처리가 진입하지 못한다.
 * 에러 경로: 이벤트가 없으면 IRQ_NONE. 개별 핸들러의 실패는 pciehp_ctrl.c 안에서
 * 처리되고 여기로 올라오지 않는다.
 *
 * 호출 체인:
 *   pciehp_isr() → (IRQ_WAKE_THREAD) → [pciehp_ist]
 *     → pciehp_handle_button_press() / pciehp_handle_disable_request()
 *       / pciehp_handle_presence_or_link_change() [모두 ctrl.c]
 *   폴 모드: pciehp_poll() → [pciehp_ist]
 */
static irqreturn_t pciehp_ist(int irq, void *dev_id)
{
	struct controller *ctrl = (struct controller *)dev_id;	/* [한국어] dev_id 를 컨트롤러로 복원. */
	struct pci_dev *pdev = ctrl_dev(ctrl);	/* [한국어] 슬롯 레지스터를 가진 브리지. */
	irqreturn_t ret;	/* [한국어] 최종 반환값. out 라벨을 거치는 경로가 여럿이라 변수로 모은다. */
	u32 events;	/* [한국어] 이번에 소비할 이벤트 비트 묶음. 내부 플래그(DISABLE_SLOT, RERUN_ISR)까지 담기므로 u16 이 아니라 u32 다. */

	ctrl->ist_running = true;	/* [한국어] 이 핸들러가 실행 중임을 표시. pciehp_ctrl.c 가 요청을 넣고 완료를 기다릴 때 이 값을 본다. */
	pci_config_pm_runtime_get(pdev);	/* [한국어] 포트를 D0 로 올리고 참조를 잡는다. 프로세스 문맥이라 실제 resume 이 가능하다. */

	/* rerun pciehp_isr() if the port was inaccessible on interrupt */
	if (atomic_fetch_and(~RERUN_ISR, &ctrl->pending_events) & RERUN_ISR) {	/* [한국어] RERUN_ISR 비트를 원자적으로 읽고 지운다. 하드 IRQ 가 부모의 서스펜드 때문에 포기했던 경우에만 서 있다. */
		ret = pciehp_isr(irq, dev_id);		/* [한국어] 이제 포트가 깨어 있으므로 하드 IRQ 로직을 프로세스 문맥에서 그대로 재실행한다. */
		enable_irq(irq);		/* [한국어] disable_irq_nosync 로 마스크했던 IRQ 를 되살린다. 이 짝이 맞지 않으면 이 슬롯은 인터럽트를 영영 못 받는다. */
		if (ret != IRQ_WAKE_THREAD)		/* [한국어] 재실행 결과가 스레드 처리를 요구하지 않으면 */
			goto out;			/* [한국어] 정리만 하고 끝낸다. */
	}

	synchronize_hardirq(irq);	/* [한국어] 다른 CPU 에서 하드 IRQ 가 실행 중이면 끝날 때까지 기다린다. 그래야 바로 아래 xchg 가 그 이벤트까지 포함해 가져간다. */
	events = atomic_xchg(&ctrl->pending_events, 0);	/* [한국어] 보류 이벤트를 통째로 가져오고 0 으로 비운다. 읽기와 비우기가 한 번에 일어나므로 하드 IRQ 와 경쟁해도 이벤트를 잃지 않는다. */
	if (!events) {	/* [한국어] 가져온 것이 없으면 헛깨움이다. */
		ret = IRQ_NONE;		/* [한국어] 처리한 것이 없다고 보고. */
		goto out;		/* [한국어] 공통 정리 경로로. */
	}

	/* Check Attention Button Pressed */
	if (events & PCI_EXP_SLTSTA_ABP)	/* [한국어] ABP: 사용자가 슬롯의 주의 버튼을 눌렀다. */
		pciehp_handle_button_press(ctrl);		/* [한국어] 상태 기계가 5초 유예 후 슬롯을 켜거나 끄도록 예약한다(누름 취소 기능 때문에 지연 워크를 쓴다). */

	/* Check Power Fault Detected */
	if (events & PCI_EXP_SLTSTA_PFD) {	/* [한국어] PFD: 전원 컨트롤러가 과전류 등을 감지해 전원을 차단했다. */
		ctrl_err(ctrl, "Slot(%s): Power fault\n", slot_name(ctrl));		/* [한국어] 사용자에게 확실히 보이도록 에러 등급으로 남긴다. */
		pciehp_set_indicators(ctrl, PCI_EXP_SLTCTL_PWR_IND_OFF,		/* [한국어] 전원 표시등은 끄고 */
				      PCI_EXP_SLTCTL_ATTN_IND_ON);				      /* [한국어] 주의 표시등은 켜서 어느 슬롯이 문제인지 물리적으로 표시한다. */
	}

	/*
	 * Ignore Link Down/Up events caused by Downstream Port Containment
	 * if recovery succeeded, or caused by Secondary Bus Reset,
	 * suspend to D3cold, firmware update, FPGA reconfiguration, etc.
	 */
	if ((events & (PCI_EXP_SLTSTA_PDC | PCI_EXP_SLTSTA_DLLSC)) &&	/* [한국어] 링크나 presence 변화가 있고 */
	    (pci_dpc_recovered(pdev) || pci_hp_spurious_link_change(pdev)) &&	    /* [한국어] 그 원인이 DPC 복구 성공이거나(pci_dpc_recovered) 리셋/서스펜드 같은 우리가 아는 동작이면(pci_hp_spurious_link_change) */
	    ctrl->state == ON_STATE) {	    /* [한국어] 그리고 슬롯이 원래 켜져 있던 상태라면. 이 조건이 없으면 진짜 삽입까지 버리게 된다. */
		u16 ignored_events = PCI_EXP_SLTSTA_DLLSC;		/* [한국어] 링크 상태 변화는 언제나 버릴 후보다. */

		if (!ctrl->inband_presence_disabled)		/* [한국어] in-band presence 가 살아 있는 슬롯은 링크가 끊기면 presence 도 함께 흔들린다. */
			ignored_events |= PCI_EXP_SLTSTA_PDC;			/* [한국어] 그래서 PDC 도 함께 버린다. in-band presence 를 껐다면 PDC 는 물리 핀 기반이라 신뢰할 수 있으므로 버리지 않는다. */

		events &= ~ignored_events;		/* [한국어] 이번 처리 목록에서 제외. */
		pciehp_ignore_link_change(ctrl, pdev, irq, ignored_events);		/* [한국어] 하드웨어와 pending_events 에 남은 흔적도 지우고, 실제로 링크가 죽었으면 이벤트를 되살린다. */
	}

	/*
	 * Disable requests have higher priority than Presence Detect Changed
	 * or Data Link Layer State Changed events.
	 */
	down_read_nested(&ctrl->reset_lock, ctrl->depth);	/* [한국어] reset_lock 읽기 획득. pciehp_reset_slot 이 쓰기 모드로 잡고 있으면 여기서 대기해, 리셋 중 열거/제거가 끼어들지 않는다. depth 는 중첩 포트용 lockdep 서브클래스다. */
	if (events & DISABLE_SLOT)	/* [한국어] 사용자가 sysfs 로 슬롯을 끄라고 한 요청. 비트 16 을 쓰므로 하드웨어 이벤트 비트와 겹치지 않는다. */
		pciehp_handle_disable_request(ctrl);		/* [한국어] 장치를 제거하고 슬롯을 끄는 경로. 하드웨어 관측보다 우선한다. */
	else if (events & (PCI_EXP_SLTSTA_PDC | PCI_EXP_SLTSTA_DLLSC))	/* [한국어] 카드 삽입/제거 또는 링크 상태 변화. */
		pciehp_handle_presence_or_link_change(ctrl, events);		/* [한국어] 어느 쪽인지 판정해 열거 또는 제거를 수행한다. events 를 그대로 넘겨 PDC 와 DLLSC 를 구분하게 한다. */
	up_read(&ctrl->reset_lock);	/* [한국어] reset_lock 해제. */

	ret = IRQ_HANDLED;	/* [한국어] 여기까지 왔으면 하나 이상 처리했다. */
out: /* [한국어] 성공/실패 공통 정리 지점. */
	pci_config_pm_runtime_put(pdev);	/* [한국어] 포트의 런타임 PM 참조 반납. 이제 다시 서스펜드돼도 된다. */
	ctrl->ist_running = false;	/* [한국어] 실행 종료 표시. 이 순서(먼저 플래그를 내리고 그다음 깨우기)여야 대기자가 깨어나 조건을 참으로 본다. */
	wake_up(&ctrl->requester);	/* [한국어] ctrl->requester 에서 이 핸들러의 종료를 기다리는 쪽(sysfs enable/disable 경로)을 깨운다. */
	return ret;	/* [한국어] 인터럽트 처리 결과를 커널에 보고. */
}

/*
 * [한국어]
 * pciehp_poll - 인터럽트 없이 핫플러그 이벤트를 감시하는 커널 스레드
 *
 * @data: kthread_run 에 넘긴 struct controller 포인터.
 * @return: 0. kthread_stop 이 호출돼 루프를 빠져나올 때만 반환한다.
 *
 * 왜 필요한가: 일부 하드웨어와 가상화 환경은 핫플러그 인터럽트를 신뢰할 수 없다.
 * 모듈 파라미터 pciehp_poll_mode 로 이 스레드를 켜면 인터럽트 대신 주기적으로
 * 레지스터를 읽어 같은 처리를 한다. 코드 중복을 피하려고 별도 로직을 만들지 않고
 * pciehp_isr 과 pciehp_ist 를 그대로 호출한다.
 * 동작 단계: 부팅 직후 열거와 겹치지 않도록 10초 쉰 뒤, 정지 요청이 올 때까지
 * 반복한다. 한 주기 안에서는 이벤트가 더 없을 때까지 isr 과 ist 를 번갈아 돌리고,
 * 그다음 pciehp_poll_time 초만큼 쉰다.
 * 실행 컨텍스트: 전용 커널 스레드. 프로세스 문맥이므로 두 핸들러가 안에서 잠들어도
 * 문제없다. IRQ 번호로 IRQ_NOTCONNECTED 를 넘겨 두 핸들러가 실제 IRQ 조작
 * (synchronize_hardirq, enable_irq)을 무해하게 넘기도록 한다.
 * 에러 경로: 없다. kthread_should_stop 이 참이 되면 정상 종료한다.
 *
 * 호출 체인:
 *   pciehp_request_irq() → kthread_run() → [pciehp_poll]
 *     → pciehp_isr() → pciehp_ist()
 *   (종료) pciehp_free_irq() → kthread_stop()
 */
static int pciehp_poll(void *data)
{
	struct controller *ctrl = data;	/* [한국어] kthread_run 이 넘겨준 컨트롤러. */

	schedule_timeout_idle(10 * HZ); /* start with 10 sec delay */	/* [한국어] 부팅 직후 10초 유예. 초기 PCI 열거가 끝나기 전에 폴링이 끼어들어 중복 처리하는 것을 피한다. idle 변형이라 이 대기가 시스템의 유휴 판정을 방해하지 않는다. */

	while (!kthread_should_stop()) {	/* [한국어] kthread_stop 이 정지를 요청할 때까지 계속 돈다. */
		/* poll for interrupt events or user requests */
		while (pciehp_isr(IRQ_NOTCONNECTED, ctrl) == IRQ_WAKE_THREAD ||		/* [한국어] 하드 IRQ 로직을 직접 호출한다. 이벤트를 걷어 pending_events 에 넣고 WAKE_THREAD 를 돌려주면 처리할 것이 있다는 뜻이다. */
		       atomic_read(&ctrl->pending_events))		       /* [한국어] 또는 다른 경로(sysfs 요청 등)가 넣어 둔 이벤트가 남아 있으면 계속 돈다. */
			pciehp_ist(IRQ_NOTCONNECTED, ctrl);			/* [한국어] 스레드 핸들러를 같은 문맥에서 직접 실행한다. IRQ_NOTCONNECTED 라 안쪽의 enable_irq/synchronize_hardirq 가 실제 하드웨어를 건드리지 않는다. */

		if (pciehp_poll_time <= 0 || pciehp_poll_time > 60)		/* [한국어] 사용자가 준 폴 주기가 비정상이면 */
			pciehp_poll_time = 2; /* clamp to sane value */			/* [한국어] 2초로 강제한다. 0 이면 CPU 를 태우고, 너무 크면 핫플러그가 사실상 동작하지 않는다. */

		schedule_timeout_idle(pciehp_poll_time * HZ);		/* [한국어] 다음 주기까지 잠든다. */
	}

	return 0;	/* [한국어] kthread_stop 대기자에게 정상 종료를 알린다. */
}

/*
 * [한국어]
 * pcie_enable_notification - 어떤 슬롯 이벤트로 알림을 받을지 Slot Control 에 설정한다
 *
 * @ctrl: 대상 컨트롤러.
 * @return: 없음.
 *
 * 왜 필요한가: 하드웨어는 Slot Control 의 enable 비트가 켜진 이벤트에 대해서만
 * 인터럽트(또는 PME)를 발생시킨다. IRQ 핸들러를 등록해 놓고 이 비트를 켜지 않으면
 * 아무 일도 일어나지 않는다. 그래서 IRQ 등록 직후 반드시 이 함수를 부른다.
 * 정책 결정: (1) 링크 이벤트(DLLSCE)는 항상 켠다. 링크 업을 삽입으로, 링크 다운을
 * 제거로 다루겠다는 뜻이다. (2) Attention Button 이 있는 슬롯은 버튼 이벤트(ABPE)를
 * 켜고, 없는 슬롯만 presence 변화(PDCE)를 켠다. 버튼이 있으면 사용자의 의사 표시가
 * 더 신뢰할 만하기 때문이다. (3) 인터럽트 모드에서만 HPIE 를 켠다. (4) 하드웨어가
 * Command Completed 를 지원할 때만 CCIE 를 켠다. (5) PFDE(전원 결함 알림)는
 * mask 에는 들어 있지만 cmd 에는 없어 결과적으로 꺼진다. 위쪽 영어 주석대로 일부
 * 기종에서 인터럽트 폭풍을 일으켰기 때문이다.
 * 실행 컨텍스트: pcie_init_notification() 안, probe 의 프로세스 문맥.
 * 완료를 기다리지 않는 쓰기를 쓰는 이유는 이 시점에 다음 명령이 곧바로 이어지지
 * 않기 때문이다.
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pciehp_probe() [core.c] → pcie_init_notification() → [pcie_enable_notification]
 *     → pcie_write_cmd_nowait() → pcie_do_write_cmd()
 */
static void pcie_enable_notification(struct controller *ctrl)
{
	u16 cmd, mask;	/* [한국어] 설정할 값과 건드릴 범위. */

	/*
	 * TBD: Power fault detected software notification support.
	 *
	 * Power fault detected software notification is not enabled
	 * now, because it caused power fault detected interrupt storm
	 * on some machines. On those machines, power fault detected
	 * bit in the slot status register was set again immediately
	 * when it is cleared in the interrupt service routine, and
	 * next power fault detected interrupt was notified again.
	 */

	/*
	 * Always enable link events: thus link-up and link-down shall
	 * always be treated as hotplug and unplug respectively. Enable
	 * presence detect only if Attention Button is not present.
	 */
	cmd = PCI_EXP_SLTCTL_DLLSCE;	/* [한국어] DLLSCE(Data Link Layer State Changed Enable). 링크 업/다운을 삽입/제거로 취급하기 위해 항상 켠다. */
	if (ATTN_BUTTN(ctrl))	/* [한국어] Slot Capabilities 의 ABP(Attention Button Present). */
		cmd |= PCI_EXP_SLTCTL_ABPE;		/* [한국어] ABPE(Attention Button Pressed Enable). 버튼이 있으면 사용자의 명시적 의사를 우선한다. */
	else
		cmd |= PCI_EXP_SLTCTL_PDCE;		/* [한국어] PDCE(Presence Detect Changed Enable). 버튼이 없는 슬롯에서만 켠다. 둘 다 켜면 같은 삽입을 두 번 처리하게 된다. */
	if (!pciehp_poll_mode)	/* [한국어] 폴 모드에서는 하드웨어 인터럽트가 필요 없다. */
		cmd |= PCI_EXP_SLTCTL_HPIE;		/* [한국어] HPIE(Hot-Plug Interrupt Enable). 위에서 켠 이벤트들이 실제 인터럽트를 내도록 하는 최상위 스위치다. */
	if (!pciehp_poll_mode && !NO_CMD_CMPL(ctrl))	/* [한국어] 인터럽트 모드이고 하드웨어가 Command Completed 를 지원할 때만. */
		cmd |= PCI_EXP_SLTCTL_CCIE;		/* [한국어] CCIE(Command Completed Interrupt Enable). 이 비트가 켜져야 pcie_wait_cmd 가 폴링 대신 잠들어 기다릴 수 있다. */

	mask = (PCI_EXP_SLTCTL_PDCE | PCI_EXP_SLTCTL_ABPE |	/* [한국어] 건드릴 범위: presence 변화와 주의 버튼 */
		PCI_EXP_SLTCTL_PFDE |		/* [한국어] 전원 결함 알림. mask 에만 있고 cmd 에는 없으므로 결과적으로 꺼진다 — 위 영어 주석이 설명하는 인터럽트 폭풍 때문이다. */
		PCI_EXP_SLTCTL_HPIE | PCI_EXP_SLTCTL_CCIE |		/* [한국어] 핫플러그 인터럽트 전체 스위치와 Command Completed 인터럽트. */
		PCI_EXP_SLTCTL_DLLSCE);		/* [한국어] 링크 상태 변화. MRLSCE 는 범위에 없어 현재 값이 보존된다. */

	pcie_write_cmd_nowait(ctrl, cmd, mask);	/* [한국어] 한 번의 쓰기로 모두 반영한다. 완료를 기다릴 필요가 없다. */
	ctrl_dbg(ctrl, "%s: SLOTCTRL %x write cmd %x\n", __func__,	/* [한국어] 어느 절대 오프셋에 무슨 값을 썼는지 남긴다. */
		 pci_pcie_cap(ctrl->pcie->port) + PCI_EXP_SLTCTL, cmd);
}

/*
 * [한국어]
 * pcie_disable_notification - 모든 슬롯 이벤트 알림을 끈다
 *
 * @ctrl: 대상 컨트롤러.
 * @return: 없음.
 *
 * 왜 필요한가: IRQ 핸들러를 떼어 내기 전에 하드웨어가 인터럽트를 내지 못하게 먼저
 * 막아야 한다. 순서가 반대면 핸들러가 없는 상태로 인터럽트가 들어와 처리되지 않은
 * 인터럽트가 쌓이거나, 이미 해제된 컨트롤러를 참조하게 된다. pcie_init() 도 IRQ
 * 등록 전에 빈 슬롯 전원을 끌 때 이 함수로 알림을 잠시 꺼 둔다.
 * 동작 단계: mask 에 열거한 모든 enable 비트를 0 으로 쓰고, 이번에는 Command
 * Completed 까지 기다린다. 기다리는 이유는 반환 직후 IRQ 를 해제해도 안전하다는
 * 것을 보장하기 위해서다.
 * 실행 컨텍스트: 프로세스 문맥(remove, probe 실패 경로, pcie_init).
 * 에러 경로: 없다. 타임아웃이면 pcie_wait_cmd 가 로그를 남긴다.
 *
 * 호출 체인:
 *   pcie_shutdown_notification() / pcie_init() → [pcie_disable_notification]
 *     → pcie_write_cmd() → pcie_do_write_cmd(wait=true)
 */
static void pcie_disable_notification(struct controller *ctrl)
{
	u16 mask;	/* [한국어] 0 으로 지울 비트들의 범위. */

	mask = (PCI_EXP_SLTCTL_PDCE | PCI_EXP_SLTCTL_ABPE |	/* [한국어] presence 변화와 주의 버튼 알림. */
		PCI_EXP_SLTCTL_MRLSCE | PCI_EXP_SLTCTL_PFDE |		/* [한국어] MRL 센서 변화와 전원 결함 알림. 켠 적이 없더라도 확실히 끄기 위해 범위에 넣는다. */
		PCI_EXP_SLTCTL_HPIE | PCI_EXP_SLTCTL_CCIE |		/* [한국어] 핫플러그 인터럽트 전체 스위치와 Command Completed 인터럽트. */
		PCI_EXP_SLTCTL_DLLSCE);		/* [한국어] 링크 상태 변화 알림. */
	pcie_write_cmd(ctrl, 0, mask);	/* [한국어] 값 0 을 mask 범위에 쓴다 = 전부 끈다. 완료를 기다려야 이후 IRQ 해제가 안전하다. */
	ctrl_dbg(ctrl, "%s: SLOTCTRL %x write cmd %x\n", __func__,	/* [한국어] 무엇을 껐는지 남긴다. */
		 pci_pcie_cap(ctrl->pcie->port) + PCI_EXP_SLTCTL, 0);
}

/*
 * [한국어]
 * pcie_clear_hotplug_events - 밀려 있는 presence/링크 변화 이벤트 비트를 지운다
 *
 * @ctrl: 대상 컨트롤러.
 * @return: 없음.
 *
 * 왜 필요한가: 시스템이 절전 상태에서 깨어나는 동안 링크가 반드시 한 번 끊겼다
 * 붙는다. 그 흔적이 Slot Status 에 남아 있으면 알림을 다시 켜는 순간 가짜 제거
 * 이벤트가 발생한다. 그래서 인터럽트를 되살리기 직전에 이 비트들을 지운다.
 * 동작 단계: Slot Status 의 PDC 와 DLLSC 에 1 을 써서 지운다(RW1C). Slot Control
 * 경로가 아니라 Status 직접 쓰기이므로 Command Completed 와 무관하고 ctrl_lock 도
 * 필요 없다 — 이벤트 비트는 각각 독립적으로 지워지기 때문이다.
 * 실행 컨텍스트: 리줌 콜백(프로세스 문맥). 이 시점에는 알림이 꺼져 있어 하드
 * IRQ 와 경쟁하지 않는다.
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pciehp_resume / pciehp_runtime_resume 계열 [core.c]
 *     → [pcie_clear_hotplug_events] → pcie_capability_write_word(PCI_EXP_SLTSTA)
 */
void pcie_clear_hotplug_events(struct controller *ctrl)
{
	pcie_capability_write_word(ctrl_dev(ctrl), PCI_EXP_SLTSTA,	/* [한국어] Slot Status 에 직접 쓴다. ctrl_dev() 로 브리지를 얻는다. */
				   PCI_EXP_SLTSTA_PDC | PCI_EXP_SLTSTA_DLLSC);				   /* [한국어] PDC 와 DLLSC 에 1 을 써서 지운다(RW1C). 절전 복귀 중 발생한 가짜 삽입/제거를 없앤다. */
}

/*
 * [한국어]
 * pcie_enable_interrupt - 절전에서 돌아올 때 핫플러그 인터럽트를 다시 켠다
 *
 * @ctrl: 대상 컨트롤러.
 * @return: 없음.
 *
 * 왜 필요한가: pcie_disable_interrupt() 가 D3cold 진입 전에 마스크한 두 비트를
 * 되살린다. 전체 알림 설정을 다시 계산하는 pcie_enable_notification() 과 달리
 * 여기서는 마스크했던 것만 정확히 되돌려, 서스펜드 전에 정해 둔 나머지 정책
 * (ABPE 또는 PDCE 선택 등)을 건드리지 않는다.
 * 동작 단계: DLLSCE 를 켜고, 인터럽트 모드면 HPIE 도 켠다. 값과 마스크에 같은
 * 변수를 넘기므로 "이 비트들을 1 로 만들라"는 뜻이 된다. 완료를 기다려 인터럽트가
 * 실제로 살아난 뒤 반환한다.
 * 실행 컨텍스트: 리줌 콜백, 프로세스 문맥.
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pciehp_resume 계열 [core.c] → [pcie_enable_interrupt] → pcie_write_cmd()
 */
void pcie_enable_interrupt(struct controller *ctrl)
{
	u16 mask;	/* [한국어] 켤 비트들. 값과 마스크로 동시에 쓰인다. */

	mask = PCI_EXP_SLTCTL_DLLSCE;	/* [한국어] 링크 상태 변화 알림은 항상 되살린다. */
	if (!pciehp_poll_mode)	/* [한국어] 폴 모드에서는 하드웨어 인터럽트가 필요 없다. */
		mask |= PCI_EXP_SLTCTL_HPIE;		/* [한국어] 핫플러그 인터럽트 전체 스위치도 되살린다. */
	pcie_write_cmd(ctrl, mask, mask);	/* [한국어] cmd 와 mask 를 같은 값으로 주면 그 비트들만 1 로 세운다는 뜻이다. 완료를 기다려 실제 반영을 확인한다. */
}

/*
 * [한국어]
 * pcie_disable_interrupt - D3cold 진입 전에 핫플러그 인터럽트를 마스크한다
 *
 * @ctrl: 대상 컨트롤러.
 * @return: 없음.
 *
 * 왜 필요한가: 포트를 절전 상태로 내리면 링크가 죽고, 그 순간 링크 다운 이벤트가
 * 발생한다. 이를 그대로 두면 커널이 절전 진입을 카드 제거로 오해한다. 그래서 두
 * 비트를 미리 끈다. 이벤트 감지 자체는 계속되므로 절전 중 실제로 카드가 바뀌면
 * PME 로 깨어날 수 있고, 깨어난 뒤 pciehp_device_replaced() 가 확인한다.
 * 동작 단계: HPIE 와 DLLSCE 를 0 으로 쓰고 완료를 기다린다. DLLSCE 까지 끄는
 * 이유는 위 영어 주석대로 이 이벤트가 링크가 죽는 즉시 PME 를 만들어 내기
 * 때문이다.
 * 실행 컨텍스트: 서스펜드 콜백, 프로세스 문맥.
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pciehp_suspend 계열 [core.c] → [pcie_disable_interrupt] → pcie_write_cmd()
 */
void pcie_disable_interrupt(struct controller *ctrl)
{
	u16 mask;	/* [한국어] 끌 비트들의 범위. */

	/*
	 * Mask hot-plug interrupt to prevent it triggering immediately
	 * when the link goes inactive (we still get PME when any of the
	 * enabled events is detected). Same goes with Link Layer State
	 * changed event which generates PME immediately when the link goes
	 * inactive so mask it as well.
	 */
	mask = PCI_EXP_SLTCTL_HPIE | PCI_EXP_SLTCTL_DLLSCE;	/* [한국어] HPIE(인터럽트 전체 스위치)와 DLLSCE(링크 상태 변화). 둘 다 꺼야 절전 진입이 삽입/제거로 오인되지 않는다. */
	pcie_write_cmd(ctrl, 0, mask);	/* [한국어] 값 0 을 그 범위에 써서 끈다. 완료를 기다려 절전 진입 전에 확실히 반영시킨다. */
}

/**
 * pciehp_slot_reset() - ignore link event caused by error-induced hot reset
 * @dev: PCI Express port service device
 *
 * Called from pcie_portdrv_slot_reset() after AER or DPC initiated a reset
 * further up in the hierarchy to recover from an error.  The reset was
 * propagated down to this hotplug port.  Ignore the resulting link flap.
 * If the link failed to retrain successfully, synthesize the ignored event.
 * Surprise removal during reset is detected through Presence Detect Changed.
 */
/*
 * [한국어]
 * pciehp_slot_reset - 상위에서 시작된 오류 복구 리셋이 만든 링크 요동을 무시한다
 *
 * @dev: PCIe 포트 서비스 장치. get_service_data() 로 우리 controller 를 꺼낸다.
 * @return: 항상 0. 포트 드라이버의 slot_reset 콜백 규약상 정수를 돌려준다.
 *
 * 왜 필요한가: AER 이나 DPC 가 계층 상위에서 리셋을 걸면 그 아래 모든 링크가 한 번
 * 끊겼다 붙는다. 이 포트의 슬롯 입장에서는 카드가 빠졌다 꽂힌 것처럼 보이지만
 * 실제로는 같은 카드가 그대로 있다. 그래서 그 흔적을 지운다. 다만 리셋 후 링크가
 * 정말로 살아나지 못했다면 그 사실은 처리해야 하므로 이벤트를 다시 만들어 넣는다.
 * 리셋 도중 카드가 진짜로 빠진 경우는 Presence Detect Changed 로 잡히며, 이 함수는
 * DLLSC 만 건드리므로 그 경로를 막지 않는다.
 * 동작 단계: (1) 슬롯이 ON 상태가 아니면 지울 것도 되살릴 것도 없으므로 즉시 반환
 * (2) Slot Status 의 DLLSC 에 1 을 써서 지운다 (3) 링크가 죽어 있으면
 * pciehp_request 로 DLLSC 이벤트를 인위적으로 넣어 정상 제거 절차를 태운다.
 * 실행 컨텍스트: 오류 복구 경로(pcie_portdrv_slot_reset)에서, 프로세스 문맥.
 * 에러 경로: 없다. 판정이 애매하면 이벤트를 되살리는 쪽으로 기운다.
 *
 * 호출 체인:
 *   AER/DPC 복구 → pcie_portdrv_slot_reset() [pcie/portdrv_core 경로]
 *     → [pciehp_slot_reset] → pcie_capability_write_word()
 *       / pciehp_check_link_active() / pciehp_request() [ctrl.c]
 */
int pciehp_slot_reset(struct pcie_device *dev)
{
	struct controller *ctrl = get_service_data(dev);	/* [한국어] 포트 서비스 장치에 매달아 둔 우리 컨트롤러를 꺼낸다. pciehp_probe 가 set_service_data 로 심어 둔 값이다. */

	if (ctrl->state != ON_STATE)	/* [한국어] 슬롯이 켜져 있고 장치가 붙어 있는 상태에서만 의미가 있다. */
		return 0;		/* [한국어] 그 외 상태에서는 무시할 이벤트도, 되살릴 이벤트도 없다. */

	pcie_capability_write_word(dev->port, PCI_EXP_SLTSTA,	/* [한국어] 리셋이 만든 링크 요동의 흔적을 Slot Status 에서 지운다(RW1C). */
				   PCI_EXP_SLTSTA_DLLSC);				   /* [한국어] DLLSC 만 지운다. PDC 는 남겨 두어야 리셋 중 실제로 카드가 빠진 경우를 놓치지 않는다. */

	if (!pciehp_check_link_active(ctrl))	/* [한국어] 리셋 후 링크가 되살아나지 못했다면 방금 지운 이벤트가 사실은 진짜였다. */
		pciehp_request(ctrl, PCI_EXP_SLTSTA_DLLSC);		/* [한국어] DLLSC 이벤트를 인위적으로 넣어 정상 제거 절차를 밟게 한다. */

	return 0;	/* [한국어] 포트 드라이버 규약상 항상 성공을 보고한다. */
}

/*
 * pciehp has a 1:1 bus:slot relationship so we ultimately want a secondary
 * bus reset of the bridge, but at the same time we want to ensure that it is
 * not seen as a hot-unplug, followed by the hot-plug of the device. Thus,
 * disable link state notification and presence detection change notification
 * momentarily, if we see that they could interfere. Also, clear any spurious
 * events after.
 */
/*
 * [한국어]
 * pciehp_reset_slot - sysfs 의 reset_slot 요청을 Secondary Bus Reset 으로 수행한다
 *
 * @hotplug_slot: PCI 핫플러그 코어가 넘겨주는 슬롯.
 * @probe: true 면 "이 슬롯이 리셋을 지원하는가"만 묻는 것이므로 실제로 리셋하지
 *         않고 0(지원함)을 돌려준다. false 면 실제로 리셋한다.
 * @return: 0 이면 성공, 음수면 pci_bridge_secondary_bus_reset() 의 오류.
 *
 * 왜 필요한가: pciehp 는 포트 하나에 슬롯 하나(1:1) 구조이므로 슬롯 리셋은 곧
 * 브리지의 Secondary Bus Reset 이다. 그런데 이 리셋은 링크를 반드시 끊었다 붙이며,
 * 그대로 두면 커널이 제거 후 재삽입으로 오해해 장치를 뽑아 버린다. 그래서 리셋
 * 전후로 링크 변화를 무시하도록 표시해 둔다.
 * 동작 단계: (1) probe 면 즉시 0 (2) reset_lock 을 쓰기 모드로 잡아 IRQ 스레드의
 * 삽입/제거 처리를 배제한다 (3) pci_hp_ignore_link_change() 로 이후 발생할 링크
 * 변화를 가짜로 표시한다 — 이 표시를 나중에 pciehp_ist 가
 * pci_hp_spurious_link_change() 로 확인한다 (4) 실제 Secondary Bus Reset 수행
 * (5) 표시 해제 (6) 락 해제.
 * 실행 컨텍스트: sysfs 쓰기 또는 커널 내부 리셋 요청, 프로세스 문맥. 리셋과 대기
 * 때문에 수백 ms 이상 걸릴 수 있다.
 * 동시성: reset_lock 을 쓰기 모드로 잡으므로, 읽기 모드로 잡는 pciehp_ist() 와
 * pciehp_ignore_link_change() 는 이 구간 동안 진입하지 못한다. ctrl->depth 를
 * 서브클래스로 넘겨 중첩 포트에서 lockdep 거짓 경보를 피한다.
 * 에러 경로: 리셋 함수의 반환값을 그대로 올려 보낸다. 실패해도 표시 해제와 락
 * 해제는 반드시 수행된다.
 *
 * 호출 체인:
 *   sysfs reset 또는 pci_reset_bus 계열 → hotplug_slot_ops->reset_slot
 *     (init_slot() [core.c] 이 등록) → [pciehp_reset_slot]
 *       → pci_hp_ignore_link_change() / pci_bridge_secondary_bus_reset()
 */
int pciehp_reset_slot(struct hotplug_slot *hotplug_slot, bool probe)
{
	struct controller *ctrl = to_ctrl(hotplug_slot);	/* [한국어] hotplug_slot 에서 controller 복원. */
	struct pci_dev *pdev = ctrl_dev(ctrl);	/* [한국어] 리셋 대상 브리지이자 슬롯 레지스터의 소유자. */
	int rc;	/* [한국어] Secondary Bus Reset 의 결과. */

	if (probe)	/* [한국어] 능력 질의 단계인가. PCI 코어는 실제 리셋 전에 지원 여부를 먼저 묻는다. */
		return 0;		/* [한국어] 0 은 "이 슬롯은 리셋을 지원한다"는 뜻이다. 실제 동작은 하지 않는다. */

	down_write_nested(&ctrl->reset_lock, ctrl->depth);	/* [한국어] reset_lock 쓰기 획득. 리셋 구간 전체에서 IRQ 스레드의 열거/제거를 배제한다. depth 는 중첩 포트용 lockdep 서브클래스다. */

	pci_hp_ignore_link_change(pdev);	/* [한국어] 이 순간 이후의 링크 변화를 "우리가 일으킨 가짜"로 표시한다. pciehp_ist 가 pci_hp_spurious_link_change 로 이 표시를 확인해 이벤트를 버린다. */

	rc = pci_bridge_secondary_bus_reset(ctrl->pcie->port);	/* [한국어] 브리지의 Secondary Bus Reset 을 수행한다. 이 한 줄이 슬롯 뒤 장치를 하드웨어적으로 초기화한다. */

	pci_hp_unignore_link_change(pdev);	/* [한국어] 표시 해제. 이후의 링크 변화는 다시 진짜로 취급된다. */

	up_write(&ctrl->reset_lock);	/* [한국어] reset_lock 해제. 대기 중이던 IRQ 스레드가 진행할 수 있다. */
	return rc;	/* [한국어] 리셋 결과를 그대로 전달. */
}

/*
 * [한국어]
 * pcie_init_notification - IRQ 를 등록하고 슬롯 이벤트 알림을 켠다
 *
 * @ctrl: pcie_init() 이 만들어 둔 컨트롤러.
 * @return: 0 이면 성공, -1 이면 IRQ 등록 실패.
 *
 * 왜 필요한가: 이벤트를 받으려면 두 가지가 모두 필요하다 — 소프트웨어 쪽 핸들러
 * 등록과 하드웨어 쪽 enable 비트. 순서도 중요해서, 핸들러를 먼저 등록한 뒤에
 * 알림을 켜야 첫 인터럽트를 놓치지 않는다. 그 순서를 이 함수가 고정한다.
 * 동작 단계: (1) pciehp_request_irq (2) pcie_enable_notification
 * (3) notification_enabled 를 세워 나중에 pcie_shutdown_notification 이 정확히
 * 한 번만 되돌리게 한다.
 * 실행 컨텍스트: pciehp_probe(), 프로세스 문맥. 컨트롤러당 한 번.
 * 에러 경로: IRQ 등록에 실패하면 알림을 켜지 않고 -1 을 반환한다. 이때
 * notification_enabled 는 0 이므로 이후 shutdown 이 불려도 아무것도 되돌리지 않는다.
 *
 * 호출 체인:
 *   pciehp_probe() [core.c] → [pcie_init_notification]
 *     → pciehp_request_irq() → pcie_enable_notification()
 */
int pcie_init_notification(struct controller *ctrl)
{
	if (pciehp_request_irq(ctrl))	/* [한국어] 먼저 핸들러(또는 폴 스레드)를 준비한다. 알림을 먼저 켜면 핸들러 없는 인터럽트가 들어올 수 있다. */
		return -1;		/* [한국어] 구체적 오류 코드를 버리고 -1 로 통일한다. 호출자는 성공/실패만 구분한다. */
	pcie_enable_notification(ctrl);	/* [한국어] 이제 하드웨어가 실제로 이벤트를 올리도록 enable 비트를 켠다. */
	ctrl->notification_enabled = 1;	/* [한국어] 되돌릴 대상이 있음을 표시. shutdown 이 이 값을 보고 한 번만 정리한다. */
	return 0;	/* [한국어] 성공. */
}

/*
 * [한국어]
 * pcie_shutdown_notification - 알림을 끄고 IRQ 를 해제한다
 *
 * @ctrl: 정리 대상 컨트롤러.
 * @return: 없음.
 *
 * 왜 필요한가: pcie_init_notification() 의 정확한 역순이다. 순서가 중요해서,
 * 하드웨어 알림을 먼저 끈 뒤에 핸들러를 떼어 내야 핸들러 없는 인터럽트가
 * 발생하지 않는다. notification_enabled 로 보호하는 이유는 probe 실패 경로와
 * remove 경로 양쪽에서 불릴 수 있어 두 번 정리하는 것을 막아야 하기 때문이다.
 * 실행 컨텍스트: pciehp_remove() 또는 probe 실패 경로, 프로세스 문맥.
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pciehp_remove() / pciehp_probe() 실패 경로 [core.c]
 *     → [pcie_shutdown_notification] → pcie_disable_notification()
 *       → pciehp_free_irq()
 */
void pcie_shutdown_notification(struct controller *ctrl)
{
	if (ctrl->notification_enabled) {	/* [한국어] 초기화에 성공한 경우에만 정리한다. 이중 해제 방지 장치다. */
		pcie_disable_notification(ctrl);		/* [한국어] 먼저 하드웨어가 인터럽트를 못 내게 막는다(완료 대기 포함). */
		pciehp_free_irq(ctrl);		/* [한국어] 그다음 핸들러 또는 폴 스레드를 떼어 낸다. 이 순서가 뒤바뀌면 처리되지 않은 인터럽트가 발생한다. */
		ctrl->notification_enabled = 0;		/* [한국어] 정리 완료 표시. 다시 불려도 아무 일도 하지 않는다. */
	}
}

/*
 * [한국어]
 * dbg_ctrl - 초기화 시점의 슬롯 레지스터 세 개를 디버그 로그로 남긴다
 *
 * @ctrl: 초기화 중인 컨트롤러. slot_cap 은 이미 채워져 있어야 한다.
 * @return: 없음.
 *
 * 왜 필요한가: 핫플러그 문제는 대부분 하드웨어가 광고한 능력과 실제 동작이 다른
 * 데서 생긴다. 부팅 직후의 Slot Capabilities/Status/Control 원본값이 남아 있으면
 * lspci 없이 dmesg 만으로 원인을 좁힐 수 있다. dynamic debug 로 꺼져 있으면
 * 비용이 거의 없다.
 * 동작 단계: 캐시된 slot_cap 을 찍고, Slot Status 와 Slot Control 은 그 자리에서
 * 읽어 찍는다. 세 값을 같은 형식으로 나란히 남긴다.
 * 실행 컨텍스트: pcie_init() 안, probe 의 프로세스 문맥.
 * 에러 경로: 없다. 읽기 실패는 검사하지 않으며 0xffff 가 그대로 로그에 남는데,
 * 그 자체가 진단에 유용한 정보다.
 *
 * 호출 체인:
 *   pciehp_probe() [core.c] → pcie_init() → [dbg_ctrl]
 *     → pcie_capability_read_word()
 */
static inline void dbg_ctrl(struct controller *ctrl)
{
	struct pci_dev *pdev = ctrl->pcie->port;	/* [한국어] 슬롯 레지스터를 가진 브리지. 여기서는 ctrl_dev() 대신 직접 꺼낸다. */
	u16 reg16;	/* [한국어] 두 번 재사용하는 16비트 임시 변수. */

	ctrl_dbg(ctrl, "Slot Capabilities      : 0x%08x\n", ctrl->slot_cap);	/* [한국어] pcie_init 이 방금 읽어 보정까지 끝낸 Slot Capabilities 사본. 32비트다. */
	pcie_capability_read_word(pdev, PCI_EXP_SLTSTA, &reg16);	/* [한국어] Slot Status 를 읽는다. 부팅 시점에 어떤 이벤트 비트가 서 있었는지 보여 준다. */
	ctrl_dbg(ctrl, "Slot Status            : 0x%04x\n", reg16);	/* [한국어] 읽은 Slot Status 출력. */
	pcie_capability_read_word(pdev, PCI_EXP_SLTCTL, &reg16);	/* [한국어] Slot Control 을 읽는다. 펌웨어가 남겨 둔 전원/표시등/알림 설정이 보인다. */
	ctrl_dbg(ctrl, "Slot Control           : 0x%04x\n", reg16);	/* [한국어] 읽은 Slot Control 출력. */
}

/*
 * [한국어] FLAG - 능력 비트를 사람이 읽기 좋은 부호 한 글자로 바꾸는 매크로
 *
 * @x: 검사할 레지스터 값. @y: 검사할 비트 마스크.
 * 결과: 비트가 서 있으면 '+', 아니면 '-'.
 * 아래 pcie_init() 의 요약 로그 한 줄에서만 쓰인다. "AttnBtn+ PwrCtrl- MRL-"
 * 같은 출력이 나오도록 해, 슬롯의 능력을 한눈에 보게 만든다. 인자를 괄호로
 * 감싸 우선순위 문제를 막는다.
 */
#define FLAG(x, y)	(((x) & (y)) ? '+' : '-')

/*
 * [한국어]
 * pcie_hotplug_depth - 이 장치 위로 몇 개의 pciehp 포트가 겹쳐 있는지 센다
 *
 * @dev: 이 핫플러그 서비스가 붙은 다운스트림 포트.
 * @return: 조상 중 pciehp 가 관리하는 브리지의 개수(0 이상).
 *
 * 왜 필요한가: 썬더볼트처럼 핫플러그 포트가 여러 겹 중첩된 토폴로지에서는
 * 바깥 포트의 reset_lock 을 잡은 채 안쪽 포트의 reset_lock 을 잡는 상황이 정상적으로
 * 발생한다. lockdep 은 같은 클래스의 락을 두 번 잡는 것을 데드락 후보로 보고
 * 경고하므로, 깊이를 서브클래스로 넘겨 서로 다른 락으로 인식시킨다. 그래서 이
 * 값은 down_read_nested()/down_write_nested() 의 인자로만 쓰인다.
 * 동작 단계: 부모 버스를 따라 루트까지 올라가며, 각 버스의 상위 브리지가
 * is_pciehp 로 표시돼 있으면 센다. is_pciehp 는 pciehp 가 그 브리지를 관리한다는
 * 표시다.
 * 실행 컨텍스트: pcie_init() 안, probe 의 프로세스 문맥. 이때 토폴로지는 안정적이라
 * 잠금 없이 순회해도 된다.
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pciehp_probe() [core.c] → pcie_init() → [pcie_hotplug_depth]
 */
static inline int pcie_hotplug_depth(struct pci_dev *dev)
{
	struct pci_bus *bus = dev->bus;	/* [한국어] 이 포트 자신이 속한 버스에서 출발한다. */
	int depth = 0;	/* [한국어] 지금까지 센 중첩 핫플러그 포트 개수. */

	while (bus->parent) {	/* [한국어] 루트 버스에 도달할 때까지 위로 올라간다. */
		bus = bus->parent;		/* [한국어] 한 단계 위 버스로 이동. */
		if (bus->self && bus->self->is_pciehp)		/* [한국어] bus->self 는 그 버스를 만든 상위 브리지다. is_pciehp 면 pciehp 가 관리하는 핫플러그 포트다. */
			depth++;			/* [한국어] 중첩 한 겹 추가. */
	}

	return depth;	/* [한국어] reset_lock 의 lockdep 서브클래스로 쓰인다. */
}

/*
 * [한국어]
 * pcie_init - struct controller 를 만들고 슬롯 하드웨어를 알려진 상태로 맞춘다
 *
 * @dev: PCIe 포트 서비스가 넘겨준 hotplug 서비스 장치. dev->port 가 실제 브리지다.
 * @return: 초기화된 controller 포인터. 메모리 할당 실패 시 NULL 이며 호출자
 *          pciehp_probe() 는 -ENOMEM 으로 바꿔 실패시킨다.
 *
 * 왜 필요한가: 이후 모든 동작의 전제가 되는 두 가지를 여기서 확정한다. 첫째는
 * 이 슬롯이 무엇을 할 수 있는가(Slot Capabilities)이고, 둘째는 지금 어떤 상태인가
 * (슬롯이 비었는지, 전원이 켜져 있는지)이다. 능력은 quirk 로 보정되고, 상태는
 * 필요하면 강제로 정리된다.
 * 동작 단계: (1) controller 할당 (2) 중첩 깊이 계산 (3) Slot Capabilities 읽기
 * (4) 플랫폼이 표시등을 직접 쓰면 표시등 능력을 지운다 (5) 썬더볼트면 Command
 * Completed 미지원으로 강제한다 (6) 동기화 객체와 워크 초기화 (7) 아래 버스에
 * 장치가 있는지로 초기 상태를 ON/OFF 로 정한다 (8) 하드웨어가 지원하면 in-band
 * presence 를 끈다 (9) DMI 매칭이면 소프트웨어로도 끈 것으로 표시한다 (10) 남아
 * 있는 모든 이벤트 비트를 지운다 (11) 능력 요약을 한 줄로 남긴다 (12) 빈 슬롯에
 * 전원이 켜져 있으면 끈다 (13) 현재 장치의 DSN 을 기억한다.
 * 실행 컨텍스트: pciehp_probe(), 프로세스 문맥. 컨트롤러당 한 번.
 * 동시성: 이 시점에는 아직 IRQ 를 등록하지 않았으므로 하드 IRQ 와 경쟁하지 않는다.
 * (12) 단계에서 알림을 잠시 끄는 것도 그 때문에 안전하다 — 다만 pcie_disable_notification
 * 은 명령을 실제로 보내므로, 이 시점에 굳이 끄는 이유는 전원 차단 명령이 스스로
 * 이벤트를 만들어 나중에 가짜 인터럽트로 남는 것을 피하기 위해서다.
 * 에러 경로: 할당 실패만 NULL 로 보고한다. 이후 단계의 레지스터 접근 실패는
 * 검사하지 않는데, 실패했다면 곧이어 pcie_init_notification 이나 첫 이벤트 처리에서
 * 드러나기 때문이다.
 *
 * 호출 체인:
 *   pciehp_probe() [core.c] → [pcie_init]
 *     → pcie_hotplug_depth() / dbg_ctrl() / pcie_write_cmd_nowait()
 *       / pciehp_get_power_status() / pciehp_power_off_slot() / pci_get_dsn()
 */
struct controller *pcie_init(struct pcie_device *dev)
{
	struct controller *ctrl;	/* [한국어] 이 함수가 만들어 돌려줄 컨트롤러. */
	u32 slot_cap, slot_cap2;	/* [한국어] Slot Capabilities 와 Slot Capabilities 2 의 원본값. 둘 다 32비트다. */
	u8 poweron;	/* [한국어] 빈 슬롯 정리 단계에서 읽을 현재 전원 상태. */
	struct pci_dev *pdev = dev->port;	/* [한국어] 슬롯 레지스터를 가진 브리지. 함수 뒷부분에서 다른 pci_dev 로 재사용되므로 주의해야 한다. */
	struct pci_bus *subordinate = pdev->subordinate;	/* [한국어] 이 포트 아래의 버스. 슬롯에 꽂힌 장치가 여기에 열거된다. */

	ctrl = kzalloc_obj(*ctrl);	/* [한국어] struct controller 를 0 으로 채워 할당한다. 0 초기화 덕분에 모든 플래그가 꺼진 상태에서 출발한다. */
	if (!ctrl)	/* [한국어] 메모리 부족. */
		return NULL;		/* [한국어] 호출자가 -ENOMEM 으로 바꿔 probe 를 실패시킨다. */

	ctrl->pcie = dev;	/* [한국어] 역참조 경로 확보. 이후 ctrl_dev() 와 ctrl_dbg() 가 모두 이 포인터를 탄다. */
	ctrl->depth = pcie_hotplug_depth(dev->port);	/* [한국어] 중첩 핫플러그 깊이. reset_lock 의 lockdep 서브클래스로만 쓰인다. */
	pcie_capability_read_dword(pdev, PCI_EXP_SLTCAP, &slot_cap);	/* [한국어] Slot Capabilities 를 읽는다. 여기에 이 슬롯이 가진 버튼/전원 컨트롤러/표시등/MRL 유무와 Physical Slot Number 가 들어 있다. */

	if (pdev->hotplug_user_indicators)	/* [한국어] 플랫폼이 표시등을 자체 규약으로 쓰는 경우. */
		slot_cap &= ~(PCI_EXP_SLTCAP_AIP | PCI_EXP_SLTCAP_PIP);		/* [한국어] AIP/PIP(Attention/Power Indicator Present) 를 지워 표준 표시등 제어 경로가 동작하지 않게 한다. 대신 raw indicator 인터페이스가 등록된다. */

	/*
	 * We assume no Thunderbolt controllers support Command Complete events,
	 * but some controllers falsely claim they do.
	 */
	if (pdev->is_thunderbolt)	/* [한국어] 썬더볼트 컨트롤러인가. */
		slot_cap |= PCI_EXP_SLTCAP_NCCS;		/* [한국어] NCCS(No Command Completed Support) 를 강제로 세운다. 지원한다고 광고해도 실제로는 CC 를 올리지 않으므로, 매 명령마다 1초를 낭비하지 않도록 아예 기다리지 않게 만든다. */

	ctrl->slot_cap = slot_cap;	/* [한국어] 보정이 끝난 능력값을 컨트롤러에 저장한다. 이후 ATTN_BUTTN/POWER_CTRL/NO_CMD_CMPL 매크로가 이 값을 본다. */
	mutex_init(&ctrl->ctrl_lock);	/* [한국어] Slot Control 읽고-고쳐-쓰기 직렬화용 뮤텍스. */
	mutex_init(&ctrl->state_lock);	/* [한국어] 슬롯 상태 기계 전이를 보호하는 뮤텍스. 이 파일은 쓰지 않고 pciehp_ctrl.c 가 쓴다. */
	init_rwsem(&ctrl->reset_lock);	/* [한국어] 리셋과 열거/제거를 배타적으로 만드는 읽기-쓰기 세마포어. */
	init_waitqueue_head(&ctrl->requester);	/* [한국어] IRQ 스레드의 종료를 기다리는 쪽이 잠들 대기 큐. */
	init_waitqueue_head(&ctrl->queue);	/* [한국어] Command Completed 를 기다리는 쪽이 잠들 대기 큐. pciehp_isr 이 여기를 깨운다. */
	INIT_DELAYED_WORK(&ctrl->button_work, pciehp_queue_pushbutton_work);	/* [한국어] 주의 버튼 누름 후 5초 유예를 구현하는 지연 워크. 실행 함수는 pciehp_ctrl.c 의 pciehp_queue_pushbutton_work 다. */
	dbg_ctrl(ctrl);	/* [한국어] 부팅 시점 레지스터 상태를 디버그 로그로 남긴다. */

	down_read(&pci_bus_sem);	/* [한국어] 버스의 장치 목록을 읽는 동안 열거가 목록을 바꾸지 못하게 막는다. */
	ctrl->state = list_empty(&subordinate->devices) ? OFF_STATE : ON_STATE;	/* [한국어] 아래 버스에 장치가 하나도 없으면 슬롯이 비어 있다고 보고 OFF 로, 있으면 이미 켜져 있다고 보고 ON 으로 출발한다. 부팅 때 이미 꽂혀 있던 카드를 다시 열거하지 않기 위한 판정이다. */
	up_read(&pci_bus_sem);	/* [한국어] 버스 세마포어 해제. */

	pcie_capability_read_dword(pdev, PCI_EXP_SLTCAP2, &slot_cap2);	/* [한국어] Slot Capabilities 2 를 읽는다. 현재 이 파일이 쓰는 필드는 IBPD 하나뿐이다. */
	if (slot_cap2 & PCI_EXP_SLTCAP2_IBPD) {	/* [한국어] IBPD(In-Band Presence Disable Supported). 하드웨어가 in-band presence 끄기를 지원하는가. */
		pcie_write_cmd_nowait(ctrl, PCI_EXP_SLTCTL_IBPD_DISABLE,		/* [한국어] 지원하면 실제로 꺼서 링크 신호가 presence 판정을 오염시키지 않게 한다. */
				      PCI_EXP_SLTCTL_IBPD_DISABLE);				      /* [한국어] 같은 비트를 값과 마스크로 주어 그 비트만 1 로 세운다. */
		ctrl->inband_presence_disabled = 1;		/* [한국어] 소프트웨어에도 기록해 pciehp_check_link_status 와 pciehp_ist 의 판정에 쓰이게 한다. */
	}

	if (dmi_first_match(inband_presence_disabled_dmi_table))	/* [한국어] Dell 처럼 이미 in-band presence 가 꺼져 있으면서 그 사실을 보고할 비트가 없는 기종인가. */
		ctrl->inband_presence_disabled = 1;		/* [한국어] 하드웨어를 건드리지 않고 소프트웨어 판정만 맞춘다. */

	/* Clear all remaining event bits in Slot Status register. */
	pcie_capability_write_word(pdev, PCI_EXP_SLTSTA,	/* [한국어] Slot Status 에 남아 있는 모든 이벤트 비트를 한 번에 지운다(RW1C). 펌웨어가 남긴 흔적을 삽입/제거로 오인하지 않기 위해서다. */
		PCI_EXP_SLTSTA_ABP | PCI_EXP_SLTSTA_PFD |		/* [한국어] ABP(주의 버튼 눌림)와 PFD(전원 결함). */
		PCI_EXP_SLTSTA_MRLSC | PCI_EXP_SLTSTA_CC |		/* [한국어] MRLSC(래치 변화)와 CC(명령 완료). */
		PCI_EXP_SLTSTA_DLLSC | PCI_EXP_SLTSTA_PDC);		/* [한국어] DLLSC(링크 상태 변화)와 PDC(presence 변화). */

	ctrl_info(ctrl, "Slot #%d AttnBtn%c PwrCtrl%c MRL%c AttnInd%c PwrInd%c HotPlug%c Surprise%c Interlock%c NoCompl%c IbPresDis%c LLActRep%c%s\n",	/* [한국어] 슬롯 능력을 한 줄로 요약해 남긴다. 핫플러그 문제 보고에서 가장 먼저 확인하는 로그다. */
		FIELD_GET(PCI_EXP_SLTCAP_PSN, slot_cap),		/* [한국어] PSN(Physical Slot Number). 섀시에 인쇄된 슬롯 번호이며 sysfs 슬롯 이름으로도 쓰인다. */
		FLAG(slot_cap, PCI_EXP_SLTCAP_ABP),		/* [한국어] ABP: 주의 버튼이 달려 있는가. */
		FLAG(slot_cap, PCI_EXP_SLTCAP_PCP),		/* [한국어] PCP(Power Controller Present): 소프트웨어로 전원을 끊을 수 있는가. */
		FLAG(slot_cap, PCI_EXP_SLTCAP_MRLSP),		/* [한국어] MRLSP(MRL Sensor Present): 걸쇠 센서가 있는가. */
		FLAG(slot_cap, PCI_EXP_SLTCAP_AIP),		/* [한국어] AIP: 주의 표시등이 있는가. */
		FLAG(slot_cap, PCI_EXP_SLTCAP_PIP),		/* [한국어] PIP: 전원 표시등이 있는가. */
		FLAG(slot_cap, PCI_EXP_SLTCAP_HPC),		/* [한국어] HPC(Hot-Plug Capable): 이 슬롯이 애초에 핫플러그를 지원하는가. */
		FLAG(slot_cap, PCI_EXP_SLTCAP_HPS),		/* [한국어] HPS(Hot-Plug Surprise): 예고 없는 제거가 허용되는가. */
		FLAG(slot_cap, PCI_EXP_SLTCAP_EIP),		/* [한국어] EIP(Electromechanical Interlock Present): 전자식 잠금장치가 있는가. */
		FLAG(slot_cap, PCI_EXP_SLTCAP_NCCS),		/* [한국어] NCCS: Command Completed 를 지원하지 않는가(위에서 썬더볼트 보정이 반영된 값). */
		FLAG(slot_cap2, PCI_EXP_SLTCAP2_IBPD),		/* [한국어] Slot Capabilities 2 의 IBPD. in-band presence 를 끌 수 있는가. */
		FLAG(pdev->link_active_reporting, true),		/* [한국어] Link Active Reporting 지원 여부. 이 값이 '-' 면 DLLLA 를 믿을 수 없어 presence 판정이 불안정해진다. */
		pdev->broken_cmd_compl ? " (with Cmd Compl erratum)" : "");		/* [한국어] Command Completed erratum 이 적용된 브리지인지 문자열로 덧붙인다. */

	/*
	 * If empty slot's power status is on, turn power off.  The IRQ isn't
	 * requested yet, so avoid triggering a notification with this command.
	 */
	if (POWER_CTRL(ctrl)) {	/* [한국어] 전원 컨트롤러가 있는 슬롯에서만 전원을 만질 수 있다. */
		pciehp_get_power_status(ctrl, &poweron);		/* [한국어] 하드웨어의 현재 전원 상태를 읽는다. */
		if (!pciehp_card_present_or_link_active(ctrl) && poweron) {		/* [한국어] 카드가 없는데 전원만 켜져 있는 상태. 펌웨어가 켜 두고 넘긴 경우다. */
			pcie_disable_notification(ctrl);			/* [한국어] 아직 IRQ 를 등록하기 전이지만, 아래 전원 차단 명령이 이벤트 비트를 남기지 않도록 알림을 먼저 끈다. */
			pciehp_power_off_slot(ctrl);			/* [한국어] 빈 슬롯의 전력 낭비를 없앤다. */
		}
	}

	pdev = pci_get_slot(subordinate, PCI_DEVFN(0, 0));	/* [한국어] 아래 버스의 dev 0 func 0 를 찾는다. pdev 변수를 브리지에서 슬롯 장치로 재사용한다는 점에 주의. */
	if (pdev)	/* [한국어] 장치가 있으면 */
		ctrl->dsn = pci_get_dsn(pdev);		/* [한국어] Device Serial Number 를 기억해 둔다. 나중에 pciehp_device_replaced 가 이 값과 비교해 카드 교체를 잡아낸다. */
	pci_dev_put(pdev);	/* [한국어] pci_get_slot 이 올린 참조 반납. pdev 가 NULL 이어도 안전하다. */

	return ctrl;	/* [한국어] 초기화가 끝난 컨트롤러를 돌려준다. */
}

/*
 * [한국어]
 * pciehp_release_ctrl - controller 와 그에 매달린 지연 워크를 해제한다
 *
 * @ctrl: pcie_init() 이 만든 컨트롤러.
 * @return: 없음.
 *
 * 왜 필요한가: 주의 버튼 처리는 5초 유예를 두는 지연 워크로 구현돼 있어, 컨트롤러
 * 메모리를 그냥 반납하면 나중에 실행된 워크가 해제된 메모리를 만진다. 그래서 반드시
 * 워크가 실행 중이 아님을 보장한 뒤 해제해야 한다.
 * 동작 단계: cancel_delayed_work_sync() 가 예약을 취소하고, 이미 실행 중이면
 * 끝날 때까지 기다린다. 그다음에야 kfree 한다.
 * 실행 컨텍스트: pciehp_remove() 또는 probe 실패 경로, 프로세스 문맥.
 * 동기 취소가 잠들 수 있으므로 IRQ 문맥에서는 부를 수 없다.
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pciehp_remove() / pciehp_probe() 실패 경로 [core.c] → [pciehp_release_ctrl]
 *     → cancel_delayed_work_sync() → kfree()
 */
void pciehp_release_ctrl(struct controller *ctrl)
{
	cancel_delayed_work_sync(&ctrl->button_work);	/* [한국어] 주의 버튼 지연 워크를 취소하고, 실행 중이면 끝날 때까지 기다린다. 이 동기 대기가 없으면 아래 kfree 이후에 워크가 깨어나 해제된 메모리를 참조한다. */
	kfree(ctrl);	/* [한국어] 컨트롤러 메모리 반납. hotplug_slot 은 이 구조체 안에 임베드돼 있어 따로 해제하지 않는다. */
}

/*
 * [한국어]
 * quirk_cmd_compl - Command Completed 를 지원한다고 거짓 광고하는 브리지에 표시를 남긴다
 *
 * @pdev: 부팅 초기 fixup 대상이 된 PCI 장치.
 * @return: 없음.
 *
 * 왜 필요한가: Intel CF118 계열과 몇몇 Qualcomm/HXT 브리지는 Slot Capabilities 에서
 * NCCS 를 0 으로(=Command Completed 지원) 보고하면서도, 실제로는 전원/표시등/잠금
 * 장치의 Control 비트가 바뀔 때만 CC 를 올린다. Enable 비트만 바꾼 명령에는 CC 가
 * 오지 않아 매번 1초씩 낭비된다. 그래서 해당 장치에 broken_cmd_compl 표시를 남기고,
 * pcie_do_write_cmd() 가 이를 보고 우회한다.
 * 동작 단계: PCIe 장치인지 확인하고, Slot Capabilities 를 읽어 핫플러그 가능
 * 슬롯이면서 CC 를 지원한다고 주장하면 플래그를 세운다.
 * 실행 컨텍스트: DECLARE_PCI_FIXUP_CLASS_EARLY 로 등록되어 PCI 열거 아주 이른
 * 시점에 실행된다. 이 시점에는 드라이버가 아직 붙지 않았고 pciehp 도 초기화되기
 * 전이라, 컨트롤러 구조체가 아니라 pci_dev 에 표시를 남기는 방식을 쓴다.
 * 에러 경로: 없다. 조건에 맞지 않으면 조용히 아무것도 하지 않는다.
 *
 * 호출 체인:
 *   PCI 열거 → pci_fixup_device(pci_fixup_early) → [quirk_cmd_compl]
 *   (나중에) pcie_do_write_cmd() 가 pdev->broken_cmd_compl 을 읽어 CC 대기를 건너뛴다
 */
static void quirk_cmd_compl(struct pci_dev *pdev)
{
	u32 slot_cap;	/* [한국어] 읽어 온 Slot Capabilities. */

	if (pci_is_pcie(pdev)) {	/* [한국어] PCI Express Capability 가 없는 구형 PCI 장치에는 Slot Capabilities 자체가 없다. */
		pcie_capability_read_dword(pdev, PCI_EXP_SLTCAP, &slot_cap);		/* [한국어] Slot Capabilities 읽기. */
		if (slot_cap & PCI_EXP_SLTCAP_HPC &&		/* [한국어] HPC(Hot-Plug Capable): 핫플러그 슬롯을 제공하는 브리지인가. */
		    !(slot_cap & PCI_EXP_SLTCAP_NCCS))		    /* [한국어] NCCS 가 0 이면 "Command Completed 를 지원한다"고 주장하는 것이다. 이 조합이 곧 erratum 대상이다. */
			pdev->broken_cmd_compl = 1;			/* [한국어] pci_dev 에 표시를 남긴다. pcie_do_write_cmd 가 이 값을 보고 오지 않을 CC 를 기다리지 않는다. */
	}
}
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_INTEL, PCI_ANY_ID, /* [한국어] Intel 의 모든 PCI-to-PCI 브리지(class 0x0604, 상위 8비트 비교)에 적용. CF118 erratum 이 광범위해 벤더 전체를 잡는다. */
			      PCI_CLASS_BRIDGE_PCI, 8, quirk_cmd_compl);	/* [한국어] 인자 8 은 class 비교 시 무시할 하위 비트 수다. 즉 프로그래밍 인터페이스 바이트를 뺀 상위 24비트만 비교한다. */
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_QCOM, 0x010e, /* [한국어] Qualcomm 브리지 0x010e. */
			      PCI_CLASS_BRIDGE_PCI, 8, quirk_cmd_compl);	/* [한국어] 같은 class 조건으로 quirk 적용. */
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_QCOM, 0x0110, /* [한국어] Qualcomm 브리지 0x0110. */
			      PCI_CLASS_BRIDGE_PCI, 8, quirk_cmd_compl);	/* [한국어] 같은 class 조건으로 quirk 적용. */
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_QCOM, 0x0400, /* [한국어] Qualcomm 브리지 0x0400. */
			      PCI_CLASS_BRIDGE_PCI, 8, quirk_cmd_compl);	/* [한국어] 같은 class 조건으로 quirk 적용. */
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_QCOM, 0x0401, /* [한국어] Qualcomm 브리지 0x0401. */
			      PCI_CLASS_BRIDGE_PCI, 8, quirk_cmd_compl);	/* [한국어] 같은 class 조건으로 quirk 적용. */
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_HXT, 0x0401, /* [한국어] HXT 브리지 0x0401. HXT 는 Qualcomm 서버 칩 설계를 이어받은 벤더라 같은 erratum 을 갖는다. */
			      PCI_CLASS_BRIDGE_PCI, 8, quirk_cmd_compl);	/* [한국어] 같은 class 조건으로 quirk 적용. 이 파일의 마지막 항목이다. */
