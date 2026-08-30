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
 * NVMe 관점 요약:
 *  이 파일은 PCIe Native Hot Plug 컨트롤러의 하드웨어 제어 레이어(HPC, Hot Plug Controller)를 구현한다.
 *  NVMe SSD가 연결된 PCIe 슬롯의 전원, 링크 상태, 인터럽트, 핫플러그 이벤트를 다루며,
 *  drivers/nvme/host/pci.c 의 nvme_probe() -> pci_enable_device() -> pcim_enable_device()
 *  경로에서 장치가 탐색되기 전, 해당 NVMe 장치가 속한 다운스트림 포트(PCIe hotplug port)의
 *  상태를 제어하는 핵심 코드이다.
 *  주요 호출 경로:
 *   - PCIe 포트 드라이버가 pciehp_pci_slot_create() 를 통해 이 컨트롤러를 초기화:
 *     pcie_init() -> pcie_init_notification() -> pciehp_request_irq() -> request_threaded_irq()
 *   - 핫플러그 이벤트 발생 시:
 *     pciehp_isr() (hardirq) -> pciehp_ist() (threaded IRQ) ->
 *     pciehp_handle_presence_or_link_change() / pciehp_handle_disable_request()
 *   - NVMe 장치 탐색/제거 시:
 *     pciehp_check_link_status() -> pci_bus_check_dev() -> NVMe 장치의 CONFIG 공간 탐색
 *  NVMe와 직접 연관된 부분:
 *   - MSI/MSI-X: pciehp_isr() 에서 pci_dev_msi_enabled() 를 확인하고 MSI 모드에서는
 *     Slot Status 를 재읽어 모든 이벤트 비트를 클리어해야 다음 MSI가 발행된다.
 *   - IRQ domain / IRQ sharing: request_threaded_irq() with IRQF_SHARED 로 등록되며,
 *     NVMe의 MSI-X vector 들과 같은 PCI bus 상의 인터럽트 자원과 공존할 수 있다.
 *   - Power control: pciehp_power_on_slot() / pciehp_power_off_slot() 으로 NVMe SSD 슬롯의
 *     전원을 켜고 끄며, 전원이 꺼지면 NVMe BAR/MSI/CONFIG 공간 모두 접근 불가해진다.
 *   - ECAM / CONFIG 공간: pcie_capability_read_word/write_word() 는 PCIe capability에
 *     MMIO/ECAM 기반으로 접근하며, 이 포트 뒤의 NVMe 장치가 보이는지 여부를 판단한다.
 *   - Link training: pciehp_check_link_status() 에서 PCI_EXP_LNKSTA_DLLLA / NLW / LT 를
 *     확인하여 NVMe 장치와의 PCIe 링크가 정상인지 검사한다.
 */

#define dev_fmt(fmt) "pciehp: " fmt /* NVMe: printk prefix, dmesg에서 pciehp 관련 로그 식별용 */

#include <linux/bitfield.h>    /* NVMe: FIELD_GET/PREP 비트 조작 매크로 */
#include <linux/dmi.h>         /* NVMe: DMI 테이블 매칭, Dell 시스템에서 inband presence 비활성화 판단 */
#include <linux/kernel.h>      /* NVMe: 커널 기본 헤더 */
#include <linux/types.h>       /* NVMe: u16/u32 등 고정폭 타입 */
#include <linux/jiffies.h>     /* NVMe: 시간 기반 타임아웃 계산 (cmd_started 등) */
#include <linux/kthread.h>     /* NVMe: 폴 모드 커널 스레드 생성 */
#include <linux/pci.h>         /* NVMe: PCI/PCIe 핵심 구조체와 레지스터 정의 */
#include <linux/pm_runtime.h>  /* NVMe: NVMe 슬롯/부모 bridge의 런타임 전원 관리 */
#include <linux/interrupt.h>   /* NVMe: IRQ 핸들러, request_threaded_irq(), MSI 관련 플래그 */
#include <linux/slab.h>        /* NVMe: kzalloc_obj() 메모리 할당 */

#include "../pci.h"            /* NVMe: 남부 PCI 코어 내부 헤더, pci_bridge_secondary_bus_reset 등 */
#include "pciehp.h"            /* NVMe: pciehp 전용 구조체/매크로, controller, hotplug_slot 등 */

/* NVMe: DMI 테이블: Dell 시스템에서 NVMe 슬롯의 inband presence 감지가 비활성화된 경우 강제 적용 */
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
		.ident = "Dell System", /* NVMe: Dell 시스템 식별자 */
		.matches = {
			DMI_MATCH(DMI_OEM_STRING, "Dell System"), /* NVMe: DMI OEM 문자열 매칭 */
		},
	},
	{} /* NVMe: 테이블 종료 */
};

/* NVMe: controller 구조체에서 해당 PCIe 포트의 pci_dev (다운스트림 포트) 반환 */
static inline struct pci_dev *ctrl_dev(struct controller *ctrl)
{
	return ctrl->pcie->port; /* NVMe: 이 포트 뒤에 NVMe 장치가 연결됨 */
}

/* NVMe: 전방 선언: PCIe 핫플러그 하드웨어 인터럽트 핸들러 (top-half) */
static irqreturn_t pciehp_isr(int irq, void *dev_id);
/* NVMe: 전방 선언: PCIe 핫플러그 스레드 인터럽트 핸들러 (bottom-half) */
static irqreturn_t pciehp_ist(int irq, void *dev_id);
/* NVMe: 전방 선언: 폴 모드에서 주기적으로 이벤트를 검사하는 커널 스레드 */
static int pciehp_poll(void *data);

/* NVMe: PCIe 핫플러그 컨트롤러용 IRQ(또는 폴 스레드)를 요청/시작 */
static inline int pciehp_request_irq(struct controller *ctrl)
{
	int retval, irq = ctrl->pcie->irq; /* NVMe: MSI/MSI-X 또는 INTx로 할당된 hotplug IRQ 번호 */

	if (pciehp_poll_mode) { /* NVMe: 커널 파라미터 pciehp_poll_mode=1 이면 인터럽트 대신 폴링 */
		ctrl->poll_thread = kthread_run(&pciehp_poll, ctrl, /* NVMe: 폴 스레드 생성 */
						"pciehp_poll-%s", /* NVMe: 스레드 이름 형식 */
						slot_name(ctrl)); /* NVMe: 슬롯 이름 포함 */
		return PTR_ERR_OR_ZERO(ctrl->poll_thread); /* NVMe: 스레드 생성 실패 시 음수 반환 */
	}

	/* Installs the interrupt handler */
	retval = request_threaded_irq(irq, pciehp_isr, pciehp_ist, /* NVMe: top-half pciehp_isr, bottom-half pciehp_ist 등록 */
				      IRQF_SHARED, "pciehp", ctrl); /* NVMe: IRQ 공유 허용, NVMe MSI-X vector들과 같은 domain에 있을 수 있음 */
	if (retval) /* NVMe: IRQ 등록 실패 시 */
		ctrl_err(ctrl, "Cannot get irq %d for the hotplug controller\n", /* NVMe: dmesg에 실패 기록 */
			 irq);
	return retval; /* NVMe: 성공 0, 실패 음수 */
}

/* NVMe: PCIe 핫플러그 컨트롤러의 IRQ 해제 또는 폴 스레드 중지 */
static inline void pciehp_free_irq(struct controller *ctrl)
{
	if (pciehp_poll_mode) /* NVMe: 폴 모드면 */
		kthread_stop(ctrl->poll_thread); /* NVMe: pciehp_poll 스레드 종료 대기 */
	else
		free_irq(ctrl->pcie->irq, ctrl); /* NVMe: 등록된 인터럽트 해제, MSI/MSI-X 또는 INTx */
}

/* NVMe: Command Completed 비트가 하드웨어에 의해 세트될 때까지 폴링 */
static int pcie_poll_cmd(struct controller *ctrl, int timeout)
{
	struct pci_dev *pdev = ctrl_dev(ctrl); /* NVMe: 다운스트림 포트 pci_dev */
	u16 slot_status; /* NVMe: Slot Status 레지스터 값 저장 */

	do { /* NVMe: 타임아웃까지 반복 */
		pcie_capability_read_word(pdev, PCI_EXP_SLTSTA, &slot_status); /* NVMe: ECAM/CONFIG 공간에서 Slot Status 읽기 */
		if (PCI_POSSIBLE_ERROR(slot_status)) { /* NVMe: 읽기 에러(FF/0 등)면 장치 없음 */
			ctrl_info(ctrl, "%s: no response from device\n", /* NVMe: 장치 무응답 로그 */
				  __func__);
			return 0; /* NVMe: 실패, 명령 완료 못 기다림 */
		}

		if (slot_status & PCI_EXP_SLTSTA_CC) { /* NVMe: Command Completed 비트가 1이면 */
			pcie_capability_write_word(pdev, PCI_EXP_SLTSTA, /* NVMe: CC 비트 클리어 */
						   PCI_EXP_SLTSTA_CC);
			ctrl->cmd_busy = 0; /* NVMe: 새 명령 허용 */
			smp_mb(); /* NVMe: 메모리 배리어, cmd_busy 업데이트를 다른 CPU에 즉시 보임 */
			return 1; /* NVMe: 명령 완료 확인 */
		}
		msleep(10); /* NVMe: 10ms 대기 */
		timeout -= 10; /* NVMe: 남은 타임아웃 감소 */
	} while (timeout >= 0); /* NVMe: 타임아웃 초과까지 루프 */
	return 0;	/* timeout */ /* NVMe: 타임아웃 발생 */
}

/* NVMe: 이전 슬롯 제어 명령이 완료될 때까지 대기 (인터럽트 또는 폴링) */
static void pcie_wait_cmd(struct controller *ctrl)
{
	unsigned int msecs = pciehp_poll_mode ? 2500 : 1000; /* NVMe: 폴 모드면 2.5s, IRQ 모드면 1s */
	unsigned long duration = msecs_to_jiffies(msecs); /* NVMe: jiffies 단위로 변환 */
	unsigned long cmd_timeout = ctrl->cmd_started + duration; /* NVMe: 명령 시작 시점 + 최대 대기 시간 */
	unsigned long now, timeout; /* NVMe: 현재 시간과 남은 시간 */
	int rc; /* NVMe: wait_event_timeout 결과 */

	/*
	 * If the controller does not generate notifications for command
	 * completions, we never need to wait between writes.
	 */
	if (NO_CMD_CMPL(ctrl)) /* NVMe: No Command Completed Support (NCCS) 비트가 설정되면 대기 안 함 */
		return;

	if (!ctrl->cmd_busy) /* NVMe: 이미 완료됐거나 명령이 없으면 즉시 리턴 */
		return;

	/*
	 * Even if the command has already timed out, we want to call
	 * pcie_poll_cmd() so it can clear PCI_EXP_SLTSTA_CC.
	 */
	now = jiffies; /* NVMe: 현재 jiffies */
	if (time_before_eq(cmd_timeout, now)) /* NVMe: 이미 타임아웃 지남 */
		timeout = 1; /* NVMe: 최소 1 jiffy만 폴링으로 클리어 시도 */
	else
		timeout = cmd_timeout - now; /* NVMe: 남은 jiffies */

	if (ctrl->slot_ctrl & PCI_EXP_SLTCTL_HPIE && /* NVMe: Hot-Plug Interrupt Enable */
	    ctrl->slot_ctrl & PCI_EXP_SLTCTL_CCIE) /* NVMe: Command Completed Interrupt Enable */
		rc = wait_event_timeout(ctrl->queue, !ctrl->cmd_busy, timeout); /* NVMe: 인터럽트 기반 대기, 커널 스케줄러에게 양보 */
	else
		rc = pcie_poll_cmd(ctrl, jiffies_to_msecs(timeout)); /* NVMe: 폴링 방식으로 CC 기다림 */

	if (!rc) /* NVMe: rc==0 이면 타임아웃 */
		ctrl_info(ctrl, "Timeout on hotplug command %#06x (issued %u msec ago)\n", /* NVMe: dmesg에 타임아웃 기록 */
			  ctrl->slot_ctrl,
			  jiffies_to_msecs(jiffies - ctrl->cmd_started));
}

/* NVMe: Command Completed erratum용 마스크: power, power indicator, attention indicator, interlock */
#define CC_ERRATUM_MASK		(PCI_EXP_SLTCTL_PCC |	\
				 PCI_EXP_SLTCTL_PIC |	\
				 PCI_EXP_SLTCTL_AIC |	\
				 PCI_EXP_SLTCTL_EIC)

/* NVMe: PCIe Slot Control 레지스터에 명령을 쓰고, 필요하면 완료 대기 */
static void pcie_do_write_cmd(struct controller *ctrl, u16 cmd,
			      u16 mask, bool wait)
{
	struct pci_dev *pdev = ctrl_dev(ctrl); /* NVMe: 다운스트림 포트 */
	u16 slot_ctrl_orig, slot_ctrl; /* NVMe: 원본/변경 Slot Control 값 */

	mutex_lock(&ctrl->ctrl_lock); /* NVMe: Slot Control 접근 상호배제 */

	/*
	 * Always wait for any previous command that might still be in progress
	 */
	pcie_wait_cmd(ctrl); /* NVMe: 이전 명령 완료까지 대기, NVMe 장치 리셋/전원 제어 직전 핵심 */

	pcie_capability_read_word(pdev, PCI_EXP_SLTCTL, &slot_ctrl); /* NVMe: ECAM 기반 Slot Control 읽기 */
	if (PCI_POSSIBLE_ERROR(slot_ctrl)) { /* NVMe: CONFIG 읽기 실패, 장치 분리 가능성 */
		ctrl_info(ctrl, "%s: no response from device\n", __func__); /* NVMe: 무응답 로그 */
		goto out; /* NVMe: 락 해제 후 리턴 */
	}

	slot_ctrl_orig = slot_ctrl; /* NVMe: 원본 보존, erratum 비교용 */
	slot_ctrl &= ~mask; /* NVMe: 변경할 비트들을 0으로 클리어 */
	slot_ctrl |= (cmd & mask); /* NVMe: 원하는 비트들을 설정 */
	ctrl->cmd_busy = 1; /* NVMe: 명령 진행 중 표시 */
	smp_mb(); /* NVMe: cmd_busy 설정을 ISR이 즉시 볼 수 있도록 배리어 */
	ctrl->slot_ctrl = slot_ctrl; /* NVMe: 소프트웨어에 캐시된 Slot Control 값 갱신 */
	pcie_capability_write_word(pdev, PCI_EXP_SLTCTL, slot_ctrl); /* NVMe: ECAM에 실제 쓰기 -> NVMe 슬롯 하드웨어 동작 */
	ctrl->cmd_started = jiffies; /* NVMe: 완료 대기 시작 시점 기록 */

	/*
	 * Controllers with the Intel CF118 and similar errata advertise
	 * Command Completed support, but they only set Command Completed
	 * if we change the "Control" bits for power, power indicator,
	 * attention indicator, or interlock.  If we only change the
	 * "Enable" bits, they never set the Command Completed bit.
	 */
	if (pdev->broken_cmd_compl && /* NVMe: Command Completed erratum이 있는 Intel/QCOM bridge */
	    (slot_ctrl_orig & CC_ERRATUM_MASK) == (slot_ctrl & CC_ERRATUM_MASK)) /* NVMe: Control 비트가 변하지 않았으면 */
		ctrl->cmd_busy = 0; /* NVMe: CC가 안 오므로 바로 완료로 간주 */

	/*
	 * Optionally wait for the hardware to be ready for a new command,
	 * indicating completion of the above issued command.
	 */
	if (wait) /* NVMe: 호출자가 대기를 원하면 */
		pcie_wait_cmd(ctrl); /* NVMe: 하드웨어가 준비될 때까지 블록 */

out:
	mutex_unlock(&ctrl->ctrl_lock); /* NVMe: Slot Control 락 해제 */
}

/**
 * pcie_write_cmd - Issue controller command
 * @ctrl: controller to which the command is issued
 * @cmd:  command value written to slot control register
 * @mask: bitmask of slot control register to be modified
 */
/* NVMe: Slot Control 명령을 발행하고 완료까지 대기 (예: NVMe 슬롯 전원 켜기) */
static void pcie_write_cmd(struct controller *ctrl, u16 cmd, u16 mask)
{
	pcie_do_write_cmd(ctrl, cmd, mask, true); /* NVMe: wait=true */
}

/* Same as above without waiting for the hardware to latch */
/* NVMe: Slot Control 명령 발행 후 완료 대기 없음 (인터럽트/이벤트 비트 설정 등) */
static void pcie_write_cmd_nowait(struct controller *ctrl, u16 cmd, u16 mask)
{
	pcie_do_write_cmd(ctrl, cmd, mask, false); /* NVMe: wait=false */
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
/* NVMe: 다운스트림 링크가 활성화되어 있는지 확인 (NVMe 장치와의 PCIe 링크 상태) */
int pciehp_check_link_active(struct controller *ctrl)
{
	struct pci_dev *pdev = ctrl_dev(ctrl); /* NVMe: 다운스트림 포트 */
	u16 lnk_status; /* NVMe: Link Status 레지스터 값 */
	int ret; /* NVMe: 읽기 결과 */

	ret = pcie_capability_read_word(pdev, PCI_EXP_LNKSTA, &lnk_status); /* NVMe: PCIe Link Status 읽기 (NVMe 링크 품질 확인) */
	if (ret == PCIBIOS_DEVICE_NOT_FOUND || PCI_POSSIBLE_ERROR(lnk_status)) /* NVMe: 장치 없음/CONFIG 에러 */
		return -ENODEV; /* NVMe: 포트 자체가 사라짐 */

	ret = !!(lnk_status & PCI_EXP_LNKSTA_DLLLA); /* NVMe: Data Link Layer Link Active 비트 검사 */
	ctrl_dbg(ctrl, "%s: lnk_status = %x\n", __func__, lnk_status); /* NVMe: 디버그용 링크 상태 출력 */

	return ret; /* NVMe: 1=active, 0=inactive */
}

/* NVMe: 주어진 PCI bus+devfn에 실제로 장치(CONFIG vendor/device ID)가 읽히는지 확인 */
static bool pci_bus_check_dev(struct pci_bus *bus, int devfn)
{
	u32 l; /* NVMe: 읽은 vendor/device ID 저장 */
	int count = 0; /* NVMe: 재시도 횟수 */
	int delay = 1000, step = 20; /* NVMe: 총 1000ms, 20ms 간격으로 재시도 */
	bool found = false; /* NVMe: 장치 발견 여부 */

	do { /* NVMe: 장치가 응답할 때까지 폴링 */
		found = pci_bus_read_dev_vendor_id(bus, devfn, &l, 0); /* NVMe: ECAM에서 vendor/device ID 읽기 (NVMe SSD면 0x010802 등 class) */
		count++; /* NVMe: 시도 횟수 증가 */

		if (found) /* NVMe: ID 읽기 성공 */
			break; /* NVMe: 루프 탈출 */

		msleep(step); /* NVMe: 20ms 대기, 링크 트레이닝 시간 확보 */
		delay -= step; /* NVMe: 남은 대기 시간 감소 */
	} while (delay > 0); /* NVMe: 1초 소진 전까지 */

	if (count > 1) /* NVMe: 1회 초과 재시도 시 */
		pr_debug("pci %04x:%02x:%02x.%d id reading try %d times with interval %d ms to get %08x\n", /* NVMe: 재시도 로그 */
			pci_domain_nr(bus), bus->number, PCI_SLOT(devfn),
			PCI_FUNC(devfn), count, step, l);

	return found; /* NVMe: true면 NVMe 장치 발견, false면 없음 */
}

/* NVMe: Presence Detect State 비트가 1로 세트될 때까지 대기 */
static void pcie_wait_for_presence(struct pci_dev *pdev)
{
	int timeout = 1250; /* NVMe: 최대 1250ms */
	u16 slot_status; /* NVMe: Slot Status 레지스터 */

	do { /* NVMe: 타임아웃까지 PDS 비트 폴링 */
		pcie_capability_read_word(pdev, PCI_EXP_SLTSTA, &slot_status); /* NVMe: Slot Status 읽기 */
		if (slot_status & PCI_EXP_SLTSTA_PDS) /* NVMe: Presence Detect State: 카드가 슬롯에 있음 */
			return; /* NVMe: 카드 감지 완료 */
		msleep(10); /* NVMe: 10ms 대기 */
		timeout -= 10; /* NVMe: 남은 시간 감소 */
	} while (timeout > 0); /* NVMe: 1.25초 대기 */
}

/* NVMe: NVMe 장치가 탐색되기 전 PCIe 링크 상태를 종합 검사 */
int pciehp_check_link_status(struct controller *ctrl)
{
	struct pci_dev *pdev = ctrl_dev(ctrl); /* NVMe: 다운스트림 포트 */
	bool found; /* NVMe: 장치 발견 여부 */
	u16 lnk_status, linksta2; /* NVMe: Link Status / Link Status 2 */

	if (!pcie_wait_for_link(pdev, true)) { /* NVMe: 링크 up 대기, NVMe와의 연결 확보 시도 */
		ctrl_info(ctrl, "Slot(%s): No link\n", slot_name(ctrl)); /* NVMe: 링크 없음 */
		return -1; /* NVMe: 실패 */
	}

	if (ctrl->inband_presence_disabled) /* NVMe: inband presence 비활성화된 경우 별도 PDS 대기 */
		pcie_wait_for_presence(pdev);

	found = pci_bus_check_dev(ctrl->pcie->port->subordinate, /* NVMe: 다운스트림 버스의 dev 0 func 0 탐색, NVMe SSD는 보통 여기 */
					PCI_DEVFN(0, 0));

	/* ignore link or presence changes up to this point */
	if (found) /* NVMe: 장치를 찾았으면 */
		atomic_and(~(PCI_EXP_SLTSTA_DLLSC | PCI_EXP_SLTSTA_PDC), /* NVMe: 이전 링크/프리즌스 변화 이벤트 무시 */
			   &ctrl->pending_events);

	pcie_capability_read_word(pdev, PCI_EXP_LNKSTA, &lnk_status); /* NVMe: 링크 상태 재확인 */
	ctrl_dbg(ctrl, "%s: lnk_status = %x\n", __func__, lnk_status); /* NVMe: 디버그 출력 */
	if ((lnk_status & PCI_EXP_LNKSTA_LT) || /* NVMe: Link Training 진행 중 */
	    !(lnk_status & PCI_EXP_LNKSTA_NLW)) { /* NVMe: Negotiated Link Width가 0, 링크 미성립 */
		ctrl_info(ctrl, "Slot(%s): Cannot train link: status %#06x\n", /* NVMe: 트레이닝 실패 */
			  slot_name(ctrl), lnk_status);
		return -1; /* NVMe: NVMe 장치 사용 불가 */
	}

	pcie_capability_read_word(pdev, PCI_EXP_LNKSTA2, &linksta2); /* NVMe: Link Status 2 읽기 */
	__pcie_update_link_speed(ctrl->pcie->port->subordinate, PCIE_HOTPLUG, /* NVMe: 버스의 링크 속도/폭 업데이트 */
				 lnk_status, linksta2);

	if (!found) { /* NVMe: 링크는 up이지만 CONFIG 읽기 실패 */
		ctrl_info(ctrl, "Slot(%s): No device found\n", /* NVMe: 장치 없음 */
			  slot_name(ctrl));
		return -1; /* NVMe: 실패 */
	}

	return 0; /* NVMe: NVMe 장치 탐색 가능 */
}

/* NVMe: 다운스트림 PCIe 링크를 활성화/비활성화 (Link Disable 비트 제어) */
static int __pciehp_link_set(struct controller *ctrl, bool enable)
{
	struct pci_dev *pdev = ctrl_dev(ctrl); /* NVMe: 다운스트림 포트 */

	pcie_capability_clear_and_set_word(pdev, PCI_EXP_LNKCTL, /* NVMe: Link Control 레지스터 atomic clear/set */
					   PCI_EXP_LNKCTL_LD, /* NVMe: Link Disable 비트 */
					   enable ? 0 : PCI_EXP_LNKCTL_LD); /* NVMe: enable=true면 LD 클리어(링크 활성화), false면 LD 설정(링크 비활성화) */

	return 0; /* NVMe: 항상 성공 */
}

/* NVMe: 다운스트림 링크 활성화: NVMe 장치를 다시 볼 수 있도록 함 */
static int pciehp_link_enable(struct controller *ctrl)
{
	return __pciehp_link_set(ctrl, true); /* NVMe: Link Disable=0 */
}

/* NVMe: 핫플러그 슬롯의 attention/power indicator raw 상태를 읽음 */
int pciehp_get_raw_indicator_status(struct hotplug_slot *hotplug_slot,
				    u8 *status)
{
	struct controller *ctrl = to_ctrl(hotplug_slot); /* NVMe: hotplug_slot에서 controller 추출 */
	struct pci_dev *pdev = ctrl_dev(ctrl); /* NVMe: 다운스트림 포트 */
	u16 slot_ctrl; /* NVMe: Slot Control 값 */

	pci_config_pm_runtime_get(pdev); /* NVMe: NVMe 슬롯/포트 런타임 PM 활성화 보장, CONFIG 접근 가능 */
	pcie_capability_read_word(pdev, PCI_EXP_SLTCTL, &slot_ctrl); /* NVMe: Slot Control 읽기 */
	pci_config_pm_runtime_put(pdev); /* NVMe: 런타임 PM ref 해제 */
	*status = (slot_ctrl & (PCI_EXP_SLTCTL_AIC | PCI_EXP_SLTCTL_PIC)) >> 6; /* NVMe: Attention/Power Indicator Control 추출 */
	return 0; /* NVMe: 성공 */
}

/* NVMe: Attention indicator 상태 반환 */
int pciehp_get_attention_status(struct hotplug_slot *hotplug_slot, u8 *status)
{
	struct controller *ctrl = to_ctrl(hotplug_slot); /* NVMe: controller 추출 */
	struct pci_dev *pdev = ctrl_dev(ctrl); /* NVMe: 다운스트림 포트 */
	u16 slot_ctrl; /* NVMe: Slot Control 값 */

	pci_config_pm_runtime_get(pdev); /* NVMe: 런타임 PM ref 획득 */
	pcie_capability_read_word(pdev, PCI_EXP_SLTCTL, &slot_ctrl); /* NVMe: Slot Control 읽기 */
	pci_config_pm_runtime_put(pdev); /* NVMe: 런타임 PM ref 해제 */
	ctrl_dbg(ctrl, "%s: SLOTCTRL %x, value read %x\n", __func__, /* NVMe: 디버그 */
		 pci_pcie_cap(ctrl->pcie->port) + PCI_EXP_SLTCTL, slot_ctrl);

	switch (slot_ctrl & PCI_EXP_SLTCTL_AIC) { /* NVMe: Attention Indicator Control 필드 */
	case PCI_EXP_SLTCTL_ATTN_IND_ON:
		*status = 1;	/* On */ /* NVMe: 주의 표시등 켜짐 */
		break;
	case PCI_EXP_SLTCTL_ATTN_IND_BLINK:
		*status = 2;	/* Blink */ /* NVMe: 깜빡임 */
		break;
	case PCI_EXP_SLTCTL_ATTN_IND_OFF:
		*status = 0;	/* Off */ /* NVMe: 꺼짐 */
		break;
	default:
		*status = 0xFF; /* NVMe: 알 수 없음 */
		break;
	}

	return 0; /* NVMe: 성공 */
}

/* NVMe: 슬롯의 전원 상태(Power Control)를 읽음 */
void pciehp_get_power_status(struct controller *ctrl, u8 *status)
{
	struct pci_dev *pdev = ctrl_dev(ctrl); /* NVMe: 다운스트림 포트 */
	u16 slot_ctrl; /* NVMe: Slot Control 값 */

	pcie_capability_read_word(pdev, PCI_EXP_SLTCTL, &slot_ctrl); /* NVMe: Slot Control 읽기 */
	ctrl_dbg(ctrl, "%s: SLOTCTRL %x value read %x\n", __func__, /* NVMe: 디버그 */
		 pci_pcie_cap(ctrl->pcie->port) + PCI_EXP_SLTCTL, slot_ctrl);

	switch (slot_ctrl & PCI_EXP_SLTCTL_PCC) { /* NVMe: Power Control 필드 */
	case PCI_EXP_SLTCTL_PWR_ON:
		*status = 1;	/* On */ /* NVMe: NVMe 슬롯 전원 켜짐 */
		break;
	case PCI_EXP_SLTCTL_PWR_OFF:
		*status = 0;	/* Off */ /* NVMe: NVMe 슬롯 전원 꺼짐 */
		break;
	default:
		*status = 0xFF; /* NVMe: 알 수 없음 */
		break;
	}
}

/* NVMe: 슬롯의 MRL(Manual Retention Latch) 상태 읽기 */
void pciehp_get_latch_status(struct controller *ctrl, u8 *status)
{
	struct pci_dev *pdev = ctrl_dev(ctrl); /* NVMe: 다운스트림 포트 */
	u16 slot_status; /* NVMe: Slot Status 값 */

	pcie_capability_read_word(pdev, PCI_EXP_SLTSTA, &slot_status); /* NVMe: Slot Status 읽기 */
	*status = !!(slot_status & PCI_EXP_SLTSTA_MRLSS); /* NVMe: MRL Sensor State 비트 */
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
/* NVMe: NVMe SSD가 슬롯에 물리적으로 있는지 Presence Detect State로 확인 */
int pciehp_card_present(struct controller *ctrl)
{
	struct pci_dev *pdev = ctrl_dev(ctrl); /* NVMe: 다운스트림 포트 */
	u16 slot_status; /* NVMe: Slot Status 값 */
	int ret; /* NVMe: 읽기 결과 */

	ret = pcie_capability_read_word(pdev, PCI_EXP_SLTSTA, &slot_status); /* NVMe: Slot Status 읽기 */
	if (ret == PCIBIOS_DEVICE_NOT_FOUND || PCI_POSSIBLE_ERROR(slot_status)) /* NVMe: 포트 접근 불가 */
		return -ENODEV; /* NVMe: 장치 없음 */

	return !!(slot_status & PCI_EXP_SLTSTA_PDS); /* NVMe: PDS 비트가 1이면 카드 존재 */
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
/* NVMe: PDS 또는 DLLA 둘 중 하나라도 활성이면 슬롯에 NVMe 장치가 있다고 판단 */
int pciehp_card_present_or_link_active(struct controller *ctrl)
{
	int ret; /* NVMe: 중간 결과 */

	ret = pciehp_card_present(ctrl); /* NVMe: PDS 확인 */
	if (ret) /* NVMe: PDS가 1이면 */
		return ret; /* NVMe: 장치 존재 */

	return pciehp_check_link_active(ctrl); /* NVMe: PDS가 0이면 링크 active로 대체 판단 */
}

/* NVMe: 전원 결함(power fault) 이벤트 상태 읽기 */
int pciehp_query_power_fault(struct controller *ctrl)
{
	struct pci_dev *pdev = ctrl_dev(ctrl); /* NVMe: 다운스트림 포트 */
	u16 slot_status; /* NVMe: Slot Status 값 */

	pcie_capability_read_word(pdev, PCI_EXP_SLTSTA, &slot_status); /* NVMe: Slot Status 읽기 */
	return !!(slot_status & PCI_EXP_SLTSTA_PFD); /* NVMe: Power Fault Detected 비트 */
}

/* NVMe: Attention/Power indicator raw 상태 설정 */
int pciehp_set_raw_indicator_status(struct hotplug_slot *hotplug_slot,
				    u8 status)
{
	struct controller *ctrl = to_ctrl(hotplug_slot); /* NVMe: controller 추출 */
	struct pci_dev *pdev = ctrl_dev(ctrl); /* NVMe: 다운스트림 포트 */

	pci_config_pm_runtime_get(pdev); /* NVMe: 런타임 PM ref 획득 */

	/* Attention and Power Indicator Control bits are supported */
	pcie_write_cmd_nowait(ctrl, FIELD_PREP(PCI_EXP_SLTCTL_AIC | PCI_EXP_SLTCTL_PIC, status), /* NVMe: AIC/PIC 필드에 status 인코딩 */
			      PCI_EXP_SLTCTL_AIC | PCI_EXP_SLTCTL_PIC); /* NVMe: AIC/PIC 마스크 */
	pci_config_pm_runtime_put(pdev); /* NVMe: 런타임 PM ref 해제 */
	return 0; /* NVMe: 성공 */
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
/* NVMe: NVMe 슬롯의 전원/주의 표시등 상태를 설정 (사용자 알림용) */
void pciehp_set_indicators(struct controller *ctrl, int pwr, int attn)
{
	u16 cmd = 0, mask = 0; /* NVMe: Slot Control에 쓸 값과 마스크 */

	if (PWR_LED(ctrl) && pwr != INDICATOR_NOOP) { /* NVMe: Power LED 지원하고 변경 요청이면 */
		cmd |= (pwr & PCI_EXP_SLTCTL_PIC); /* NVMe: PIC 필드 설정 */
		mask |= PCI_EXP_SLTCTL_PIC; /* NVMe: PIC 마스크 */
	}

	if (ATTN_LED(ctrl) && attn != INDICATOR_NOOP) { /* NVMe: Attention LED 지원하고 변경 요청이면 */
		cmd |= (attn & PCI_EXP_SLTCTL_AIC); /* NVMe: AIC 필드 설정 */
		mask |= PCI_EXP_SLTCTL_AIC; /* NVMe: AIC 마스크 */
	}

	if (cmd) { /* NVMe: 변경할 비트가 있으면 */
		pcie_write_cmd_nowait(ctrl, cmd, mask); /* NVMe: Slot Control에 쓰기 */
		ctrl_dbg(ctrl, "%s: SLOTCTRL %x write cmd %x\n", __func__, /* NVMe: 디버그 */
			 pci_pcie_cap(ctrl->pcie->port) + PCI_EXP_SLTCTL, cmd);
	}
}

/* NVMe: NVMe SSD가 탑재된 슬롯에 전원을 공급하고 링크를 활성화 */
int pciehp_power_on_slot(struct controller *ctrl)
{
	struct pci_dev *pdev = ctrl_dev(ctrl); /* NVMe: 다운스트림 포트 */
	u16 slot_status; /* NVMe: Slot Status 값 */
	int retval; /* NVMe: 결과 */

	/* Clear power-fault bit from previous power failures */
	pcie_capability_read_word(pdev, PCI_EXP_SLTSTA, &slot_status); /* NVMe: Slot Status 읽기 */
	if (slot_status & PCI_EXP_SLTSTA_PFD) /* NVMe: 이전 전원 결함 비트가 남아있으면 */
		pcie_capability_write_word(pdev, PCI_EXP_SLTSTA, /* NVMe: PFD 클리어 */
					   PCI_EXP_SLTSTA_PFD);
	ctrl->power_fault_detected = 0; /* NVMe: 전원 결함 상태 초기화 */

	pcie_write_cmd(ctrl, PCI_EXP_SLTCTL_PWR_ON, PCI_EXP_SLTCTL_PCC); /* NVMe: Slot Control에서 Power On 명령 */
	ctrl_dbg(ctrl, "%s: SLOTCTRL %x write cmd %x\n", __func__, /* NVMe: 디버그 */
		 pci_pcie_cap(ctrl->pcie->port) + PCI_EXP_SLTCTL,
		 PCI_EXP_SLTCTL_PWR_ON);

	retval = pciehp_link_enable(ctrl); /* NVMe: PCIe 링크 활성화 (NVMe SSD와 통신 준비) */
	if (retval) /* NVMe: 링크 활성화 실패 시 */
		ctrl_err(ctrl, "%s: Can not enable the link!\n", __func__); /* NVMe: 에러 로그 */

	return retval; /* NVMe: 성공 0, 실패 음수 */
}

/* NVMe: NVMe SSD 슬롯의 전원을 끔 (BAR/MSI/CONFIG 접근 불가) */
void pciehp_power_off_slot(struct controller *ctrl)
{
	pcie_write_cmd(ctrl, PCI_EXP_SLTCTL_PWR_OFF, PCI_EXP_SLTCTL_PCC); /* NVMe: Power Off 명령 */
	ctrl_dbg(ctrl, "%s: SLOTCTRL %x write cmd %x\n", __func__, /* NVMe: 디버그 */
		 pci_pcie_cap(ctrl->pcie->port) + PCI_EXP_SLTCTL,
		 PCI_EXP_SLTCTL_PWR_OFF);
}

/* NVMe: 슬롯의 NVMe 장치가 교첸되었는지(같은 위치에 다른 장치) 확인 */
bool pciehp_device_replaced(struct controller *ctrl)
{
	struct pci_dev *pdev __free(pci_dev_put) = NULL; /* NVMe: 자동 pci_dev_put으로 ref 관리 */
	u32 reg; /* NVMe: CONFIG 공간에서 읽은 값 */

	if (pci_dev_is_disconnected(ctrl->pcie->port)) /* NVMe: 포트 자체가 disconnected면 */
		return false; /* NVMe: 교체로 보지 않음 */

	pdev = pci_get_slot(ctrl->pcie->port->subordinate, PCI_DEVFN(0, 0)); /* NVMe: 다운스트림 버스의 장치 얻기 (보통 NVMe) */
	if (!pdev) /* NVMe: 장치가 없으면 */
		return true; /* NVMe: 제거된 것으로 간주, 교첸된 셈 */

	if (pci_read_config_dword(pdev, PCI_VENDOR_ID, &reg) || /* NVMe: vendor/device ID 읽기 */
	    reg != (pdev->vendor | (pdev->device << 16)) || /* NVMe: 저장된 값과 다륾 (서프라이즈 리묶 등) */
	    pci_read_config_dword(pdev, PCI_CLASS_REVISION, &reg) || /* NVMe: class/revision 읽기 */
	    reg != (pdev->revision | (pdev->class << 8))) /* NVMe: class/revision 변경 확인 */
		return true; /* NVMe: 장치가 교첸됨 */

	if (pdev->hdr_type == PCI_HEADER_TYPE_NORMAL && /* NVMe: 일반 PCIe 장치면 */
	    (pci_read_config_dword(pdev, PCI_SUBSYSTEM_VENDOR_ID, &reg) || /* NVMe: subsystem ID 읽기 */
	     reg != (pdev->subsystem_vendor | (pdev->subsystem_device << 16)))) /* NVMe: 서브시스템 변경 확인 */
		return true; /* NVMe: 장치 교체 */

	if (pci_get_dsn(pdev) != ctrl->dsn) /* NVMe: Device Serial Number가 달라지면 */
		return true; /* NVMe: 분명히 다른 NVMe 장치 */

	return false; /* NVMe: 동일 장치, 교체 아님 */
}

/* NVMe: DPC/Secondary Bus Reset 등으로 인한 링크 변화를 의도적으로 무시 */
static void pciehp_ignore_link_change(struct controller *ctrl,
				      struct pci_dev *pdev, int irq,
				      u16 ignored_events)
{
	/*
	 * Ignore link changes which occurred while waiting for DPC recovery.
	 * Could be several if DPC triggered multiple times consecutively.
	 * Also ignore link changes caused by Secondary Bus Reset, etc.
	 */
	synchronize_hardirq(irq); /* NVMe: top-half 완료까지 동기화, 진행 중인 MSI 처리 방지 */
	atomic_and(~ignored_events, &ctrl->pending_events); /* NVMe: 무시할 이벤트 비트 클리어 */
	if (pciehp_poll_mode) /* NVMe: 폴 모드면 하드웨어 레지스터도 직접 클리어 */
		pcie_capability_write_word(pdev, PCI_EXP_SLTSTA, /* NVMe: Slot Status에 ignored_events 쓰기로 클리어 */
					   ignored_events);
	ctrl_info(ctrl, "Slot(%s): Link Down/Up ignored\n", slot_name(ctrl)); /* NVMe: 사용자/개발자용 정보 */

	/*
	 * If the link is unexpectedly down after successful recovery,
	 * the corresponding link change may have been ignored above.
	 * Synthesize it to ensure that it is acted on.
	 */
	down_read_nested(&ctrl->reset_lock, ctrl->depth); /* NVMe: reset lock read 획득 (재귀 허용) */
	if (!pciehp_check_link_active(ctrl) || pciehp_device_replaced(ctrl)) /* NVMe: 링크가 inactive거나 장치 교체 시 */
		pciehp_request(ctrl, ignored_events); /* NVMe: 다시 핫플러그 처리 요청 */
	up_read(&ctrl->reset_lock); /* NVMe: reset lock 해제 */
}

/* NVMe: PCIe 핫플러그 인터럽트 top-half (hardirq): Slot Status 읽고 이벤트 수집 */
static irqreturn_t pciehp_isr(int irq, void *dev_id)
{
	struct controller *ctrl = (struct controller *)dev_id; /* NVMe: IRQ 등록 시 전달된 controller */
	struct pci_dev *pdev = ctrl_dev(ctrl); /* NVMe: 다운스트림 포트 */
	struct device *parent = pdev->dev.parent; /* NVMe: 부모 bridge/device, 런타임 PM 제어용 */
	u16 status, events = 0; /* NVMe: 읽은 Slot Status, 수집된 이벤트 */

	/*
	 * Interrupts only occur in D3hot or shallower and only if enabled
	 * in the Slot Control register (PCIe r4.0, sec 6.7.3.4).
	 */
	if (pdev->current_state == PCI_D3cold || /* NVMe: 포트가 D3cold면 인터럽트 없음 */
	    (!(ctrl->slot_ctrl & PCI_EXP_SLTCTL_HPIE) && !pciehp_poll_mode)) /* NVMe: HPIE 꺼져 있고 폴 모드 아니면 */
		return IRQ_NONE; /* NVMe: 이 IRQ는 pciehp 이벤트 아님 */

	/*
	 * Keep the port accessible by holding a runtime PM ref on its parent.
	 * Defer resume of the parent to the IRQ thread if it's suspended.
	 * Mask the interrupt until then.
	 */
	if (parent) { /* NVMe: 부모가 있으면 */
		pm_runtime_get_noresume(parent); /* NVMe: 부모 런타임 PM 활성화 ref 증가, CONFIG 접근 가능 유지 */
		if (!pm_runtime_active(parent)) { /* NVMe: 부모가 여전히 suspended면 */
			pm_runtime_put(parent); /* NVMe: 앞서 증가한 ref 반납 */
			disable_irq_nosync(irq); /* NVMe: IRQ 마스크, threaded IRQ에서 rerun 처리 */
			atomic_or(RERUN_ISR, &ctrl->pending_events); /* NVMe: ISR 재실행 플래그 설정 */
			return IRQ_WAKE_THREAD; /* NVMe: bottom-half 깨워서 resume 후 재처리 */
		}
	}

read_status:
	pcie_capability_read_word(pdev, PCI_EXP_SLTSTA, &status); /* NVMe: Slot Status 읽기 (ECAM) */
	if (PCI_POSSIBLE_ERROR(status)) { /* NVMe: CONFIG 에러, 장치 제거됨 */
		ctrl_info(ctrl, "%s: no response from device\n", __func__); /* NVMe: 무응답 로그 */
		if (parent)
			pm_runtime_put(parent); /* NVMe: ref 반납 */
		return IRQ_NONE; /* NVMe: 이벤트 아님 */
	}

	/*
	 * Slot Status contains plain status bits as well as event
	 * notification bits; right now we only want the event bits.
	 */
	status &= PCI_EXP_SLTSTA_ABP | PCI_EXP_SLTSTA_PFD | /* NVMe: Attention Button Pressed */
		  PCI_EXP_SLTSTA_PDC | PCI_EXP_SLTSTA_CC | /* NVMe: Presence Detect Changed, Command Completed */
		  PCI_EXP_SLTSTA_DLLSC; /* NVMe: Data Link Layer State Changed */

	/*
	 * If we've already reported a power fault, don't report it again
	 * until we've done something to handle it.
	 */
	if (ctrl->power_fault_detected) /* NVMe: 이미 전원 결함 보고됨 */
		status &= ~PCI_EXP_SLTSTA_PFD; /* NVMe: PFD 중복 보고 방지 */
	else if (status & PCI_EXP_SLTSTA_PFD) /* NVMe: 새 전원 결함 감지 */
		ctrl->power_fault_detected = true; /* NVMe: 플래그 설정 */

	events |= status; /* NVMe: 이벤트 누적 */
	if (!events) { /* NVMe: 처리할 이벤트 없음 */
		if (parent)
			pm_runtime_put(parent); /* NVMe: ref 반납 */
		return IRQ_NONE; /* NVMe: spurious interrupt */
	}

	if (status) { /* NVMe: 이벤트 비트가 있으면 */
		pcie_capability_write_word(pdev, PCI_EXP_SLTSTA, status); /* NVMe: 이벤트 비트 클리어 */

		/*
		 * In MSI mode, all event bits must be zero before the port
		 * will send a new interrupt (PCIe Base Spec r5.0 sec 6.7.3.4).
		 * So re-read the Slot Status register in case a bit was set
		 * between read and write.
		 */
		if (pci_dev_msi_enabled(pdev) && !pciehp_poll_mode) /* NVMe: MSI/MSI-X 사용 중이면 */
			goto read_status; /* NVMe: 클리어 후 다시 읽어 놓친 이벤트 확인, NVMe MSI-X와 동일 포트 공유 시 중요 */
	}

	ctrl_dbg(ctrl, "pending interrupts %#06x from Slot Status\n", events); /* NVMe: 디버그 */
	if (parent)
		pm_runtime_put(parent); /* NVMe: ref 반납 */

	/*
	 * Command Completed notifications are not deferred to the
	 * IRQ thread because it may be waiting for their arrival.
	 */
	if (events & PCI_EXP_SLTSTA_CC) { /* NVMe: Command Completed 이벤트면 */
		ctrl->cmd_busy = 0; /* NVMe: 명령 완료, 다음 명령 가능 */
		smp_mb(); /* NVMe: 메모리 배리어 */
		wake_up(&ctrl->queue); /* NVMe: pcie_wait_cmd() 에서 대기 중인 쓰레드 깨움 */

		if (events == PCI_EXP_SLTSTA_CC) /* NVMe: CC만 있으면 */
			return IRQ_HANDLED; /* NVMe: bottom-half 필요 없음 */

		events &= ~PCI_EXP_SLTSTA_CC; /* NVMe: CC는 이미 처리, 나머지만 bottom-half로 */
	}

	if (pdev->ignore_hotplug) { /* NVMe: 핫플러그 무시 플래그 (예: D3cold 진입 중) */
		ctrl_dbg(ctrl, "ignoring hotplug event %#06x\n", events); /* NVMe: 디버그 */
		return IRQ_HANDLED; /* NVMe: 이벤트 폐기 */
	}

	/* Save pending events for consumption by IRQ thread. */
	atomic_or(events, &ctrl->pending_events); /* NVMe: bottom-half에서 처리할 이벤트 저장 */
	return IRQ_WAKE_THREAD; /* NVMe: pciehp_ist 깨움 */
}

/* NVMe: PCIe 핫플러그 인터럽트 bottom-half (threaded IRQ): 실제 핫플러그 처리 */
static irqreturn_t pciehp_ist(int irq, void *dev_id)
{
	struct controller *ctrl = (struct controller *)dev_id; /* NVMe: controller */
	struct pci_dev *pdev = ctrl_dev(ctrl); /* NVMe: 다운스트림 포트 */
	irqreturn_t ret; /* NVMe: 반환값 */
	u32 events; /* NVMe: 처리할 이벤트 */

	ctrl->ist_running = true; /* NVMe: bottom-half 실행 중 플래그 */
	pci_config_pm_runtime_get(pdev); /* NVMe: CONFIG 접근 위해 런타임 PM ref */

	/* rerun pciehp_isr() if the port was inaccessible on interrupt */
	if (atomic_fetch_and(~RERUN_ISR, &ctrl->pending_events) & RERUN_ISR) { /* NVMe: ISR 재실행 요청이 있으면 */
		ret = pciehp_isr(irq, dev_id); /* NVMe: top-half 다시 실행 */
		enable_irq(irq); /* NVMe: 마스크했던 IRQ 다시 활성화 */
		if (ret != IRQ_WAKE_THREAD) /* NVMe: 더 이상 bottom-half 필요 없으면 */
			goto out; /* NVMe: 종료 */
	}

	synchronize_hardirq(irq); /* NVMe: top-half 완료 보장 */
	events = atomic_xchg(&ctrl->pending_events, 0); /* NVMe: 보류 이벤트 원자적으로 가져오고 0으로 클리어 */
	if (!events) { /* NVMe: 처리할 이벤트 없으면 */
		ret = IRQ_NONE; /* NVMe: spurious wake */
		goto out; /* NVMe: 종료 */
	}

	/* Check Attention Button Pressed */
	if (events & PCI_EXP_SLTSTA_ABP) /* NVMe: Attention Button 눌림 */
		pciehp_handle_button_press(ctrl); /* NVMe: 버튼 눌림 처리 (슬롯 활성/비활성 토글) */

	/* Check Power Fault Detected */
	if (events & PCI_EXP_SLTSTA_PFD) { /* NVMe: 전원 결함 감지 */
		ctrl_err(ctrl, "Slot(%s): Power fault\n", slot_name(ctrl)); /* NVMe: 에러 로그 */
		pciehp_set_indicators(ctrl, PCI_EXP_SLTCTL_PWR_IND_OFF, /* NVMe: 전원 표시등 끔 */
				      PCI_EXP_SLTCTL_ATTN_IND_ON); /* NVMe: 주의 표시등 켬 */
	}

	/*
	 * Ignore Link Down/Up events caused by Downstream Port Containment
	 * if recovery succeeded, or caused by Secondary Bus Reset,
	 * suspend to D3cold, firmware update, FPGA reconfiguration, etc.
	 */
	if ((events & (PCI_EXP_SLTSTA_PDC | PCI_EXP_SLTSTA_DLLSC)) && /* NVMe: Presence/Link 변화 이벤트가 있고 */
	    (pci_dpc_recovered(pdev) || pci_hp_spurious_link_change(pdev)) && /* NVMe: DPC 복구 성공 또는 허위 링크 변화 */
	    ctrl->state == ON_STATE) { /* NVMe: 현재 슬롯이 ON 상태면 */
		u16 ignored_events = PCI_EXP_SLTSTA_DLLSC; /* NVMe: 일단 DLLSC 무시 */

		if (!ctrl->inband_presence_disabled) /* NVMe: inband PDC 사용 중이면 */
			ignored_events |= PCI_EXP_SLTSTA_PDC; /* NVMe: PDC도 무시 */

		events &= ~ignored_events; /* NVMe: 무시할 이벤트 제외 */
		pciehp_ignore_link_change(ctrl, pdev, irq, ignored_events); /* NVMe: 링크 변화 무시 또는 재합성 */
	}

	/*
	 * Disable requests have higher priority than Presence Detect Changed
	 * or Data Link Layer State Changed events.
	 */
	down_read_nested(&ctrl->reset_lock, ctrl->depth); /* NVMe: reset lock read 획득 */
	if (events & DISABLE_SLOT) /* NVMe: 사용자/시스템이 슬롯 비활성 요청 */
		pciehp_handle_disable_request(ctrl); /* NVMe: NVMe 장치 제거 및 슬롯 off 처리 */
	else if (events & (PCI_EXP_SLTSTA_PDC | PCI_EXP_SLTSTA_DLLSC)) /* NVMe: 카드 삽입/제거 또는 링크 변화 */
		pciehp_handle_presence_or_link_change(ctrl, events); /* NVMe: NVMe 장치 탐색 또는 제거 처리 */
	up_read(&ctrl->reset_lock); /* NVMe: reset lock 해제 */

	ret = IRQ_HANDLED; /* NVMe: 이벤트 처리 완료 */
out:
	pci_config_pm_runtime_put(pdev); /* NVMe: 런타임 PM ref 반납 */
	ctrl->ist_running = false; /* NVMe: bottom-half 종료 플래그 */
	wake_up(&ctrl->requester); /* NVMe: pciehp_ist 종료를 기다리는 쓰레드 깨움 */
	return ret; /* NVMe: IRQ 처리 결과 */
}

/* NVMe: 폴 모드 커널 스레드: 인터럽트 없이 주기적으로 Slot Status 확인 */
static int pciehp_poll(void *data)
{
	struct controller *ctrl = data; /* NVMe: controller 인자 */

	schedule_timeout_idle(10 * HZ); /* start with 10 sec delay */ /* NVMe: 시작 시 10초 딜레이 */

	while (!kthread_should_stop()) { /* NVMe: 중지 요청까지 루프 */
		/* poll for interrupt events or user requests */
		while (pciehp_isr(IRQ_NOTCONNECTED, ctrl) == IRQ_WAKE_THREAD || /* NVMe: 폴 시점의 top-half 호출, 이벤트 있으면 WAKE_THREAD */
		       atomic_read(&ctrl->pending_events)) /* NVMe: 보류 이벤트가 있으면 */
			pciehp_ist(IRQ_NOTCONNECTED, ctrl); /* NVMe: bottom-half 직접 실행 */

		if (pciehp_poll_time <= 0 || pciehp_poll_time > 60) /* NVMe: 폴 주기 검증 */
			pciehp_poll_time = 2; /* clamp to sane value */ /* NVMe: 2초로 클램프 */

		schedule_timeout_idle(pciehp_poll_time * HZ); /* NVMe: 다음 폴까지 대기 */
	}

	return 0; /* NVMe: 스레드 종료 */
}

/* NVMe: PCIe 핫플러그 이벤트 알림을 Slot Control에 활성화 */
static void pcie_enable_notification(struct controller *ctrl)
{
	u16 cmd, mask; /* NVMe: 설정할 비트와 마스크 */

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
	cmd = PCI_EXP_SLTCTL_DLLSCE; /* NVMe: Data Link Layer State Changed Enable: 링크 up/down 이벤트 활성화 */
	if (ATTN_BUTTN(ctrl)) /* NVMe: Attention Button 지원하면 */
		cmd |= PCI_EXP_SLTCTL_ABPE; /* NVMe: Attention Button Pressed Enable */
	else
		cmd |= PCI_EXP_SLTCTL_PDCE; /* NVMe: Presence Detect Changed Enable */
	if (!pciehp_poll_mode) /* NVMe: 폴 모드가 아니면 */
		cmd |= PCI_EXP_SLTCTL_HPIE; /* NVMe: Hot-Plug Interrupt Enable: MSI/MSI-X/INTx 인터럽트 발생 허용 */
	if (!pciehp_poll_mode && !NO_CMD_CMPL(ctrl)) /* NVMe: IRQ 모드이고 Command Completed 지원하면 */
		cmd |= PCI_EXP_SLTCTL_CCIE; /* NVMe: Command Completed Interrupt Enable */

	mask = (PCI_EXP_SLTCTL_PDCE | PCI_EXP_SLTCTL_ABPE | /* NVMe: Presence/Attention Button */
		PCI_EXP_SLTCTL_PFDE | /* NVMe: Power Fault Detected Enable (코멘트로 비활성화됨) */
		PCI_EXP_SLTCTL_HPIE | PCI_EXP_SLTCTL_CCIE | /* NVMe: Hotplug/CC 인터럽트 */
		PCI_EXP_SLTCTL_DLLSCE); /* NVMe: Link state changed */

	pcie_write_cmd_nowait(ctrl, cmd, mask); /* NVMe: Slot Control에 이벤트 enable 비트 쓰기 */
	ctrl_dbg(ctrl, "%s: SLOTCTRL %x write cmd %x\n", __func__, /* NVMe: 디버그 */
		 pci_pcie_cap(ctrl->pcie->port) + PCI_EXP_SLTCTL, cmd);
}

/* NVMe: PCIe 핫플러그 이벤트 알림 비활성화 */
static void pcie_disable_notification(struct controller *ctrl)
{
	u16 mask; /* NVMe: 클리어할 비트 마스크 */

	mask = (PCI_EXP_SLTCTL_PDCE | PCI_EXP_SLTCTL_ABPE | /* NVMe: Presence/Attention Button */
		PCI_EXP_SLTCTL_MRLSCE | PCI_EXP_SLTCTL_PFDE | /* NVMe: MRL/Power Fault */
		PCI_EXP_SLTCTL_HPIE | PCI_EXP_SLTCTL_CCIE | /* NVMe: Hotplug/CC 인터럽트 */
		PCI_EXP_SLTCTL_DLLSCE); /* NVMe: Link state changed */
	pcie_write_cmd(ctrl, 0, mask); /* NVMe: mask에 해당하는 비트들을 0으로 클리어, 완료 대기 */
	ctrl_dbg(ctrl, "%s: SLOTCTRL %x write cmd %x\n", __func__, /* NVMe: 디버그 */
		 pci_pcie_cap(ctrl->pcie->port) + PCI_EXP_SLTCTL, 0);
}

/* NVMe: 보류 중인 Presence Detect Changed / Link State Changed 이벤트를 하드웨어에서 클리어 */
void pcie_clear_hotplug_events(struct controller *ctrl)
{
	pcie_capability_write_word(ctrl_dev(ctrl), PCI_EXP_SLTSTA, /* NVMe: Slot Status 레지스터 */
				   PCI_EXP_SLTSTA_PDC | PCI_EXP_SLTSTA_DLLSC); /* NVMe: PDC와 DLLSC 클리어 */
}

/* NVMe: 핫플러그 인터럽트 활성화 (D0 복귀 등) */
void pcie_enable_interrupt(struct controller *ctrl)
{
	u16 mask; /* NVMe: enable 할 비트 */

	mask = PCI_EXP_SLTCTL_DLLSCE; /* NVMe: 링크 상태 변화 이벤트 */
	if (!pciehp_poll_mode) /* NVMe: 폴 모드 아니면 */
		mask |= PCI_EXP_SLTCTL_HPIE; /* NVMe: 핫플러그 인터럽트 enable */
	pcie_write_cmd(ctrl, mask, mask); /* NVMe: 해당 비트만 설정, 완료 대기 */
}

/* NVMe: 핫플러그 인터럽트 비활성화 (D3cold 진입 등) */
void pcie_disable_interrupt(struct controller *ctrl)
{
	u16 mask; /* NVMe: disable 할 비트 */

	/*
	 * Mask hot-plug interrupt to prevent it triggering immediately
	 * when the link goes inactive (we still get PME when any of the
	 * enabled events is detected). Same goes with Link Layer State
	 * changed event which generates PME immediately when the link goes
	 * inactive so mask it as well.
	 */
	mask = PCI_EXP_SLTCTL_HPIE | PCI_EXP_SLTCTL_DLLSCE; /* NVMe: Hotplug IRQ와 Link state changed event 모두 마스크 */
	pcie_write_cmd(ctrl, 0, mask); /* NVMe: 해당 비트 클리어, 완료 대기 */
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
/* NVMe: AER/DPC 등 상위에서 유발된 reset으로 인한 링크 변화를 무시/재합성 */
int pciehp_slot_reset(struct pcie_device *dev)
{
	struct controller *ctrl = get_service_data(dev); /* NVMe: pcie_device에서 controller 얻기 */

	if (ctrl->state != ON_STATE) /* NVMe: 슬롯이 ON 상태가 아니면 처리 안 함 */
		return 0; /* NVMe: 성공 (할 것 없음) */

	pcie_capability_write_word(dev->port, PCI_EXP_SLTSTA, /* NVMe: Slot Status 레지스터 */
				   PCI_EXP_SLTSTA_DLLSC); /* NVMe: Link State Changed 클리어 */

	if (!pciehp_check_link_active(ctrl)) /* NVMe: 링크가 inactive면 */
		pciehp_request(ctrl, PCI_EXP_SLTSTA_DLLSC); /* NVMe: 링크 변화 이벤트를 다시 요청해 처리 유도 */

	return 0; /* NVMe: 성공 */
}

/*
 * pciehp has a 1:1 bus:slot relationship so we ultimately want a secondary
 * bus reset of the bridge, but at the same time we want to ensure that it is
 * not seen as a hot-unplug, followed by the hot-plug of the device. Thus,
 * disable link state notification and presence detection change notification
 * momentarily, if we see that they could interfere. Also, clear any spurious
 * events after.
 */
/* NVMe: NVMe 슬롯에 Secondary Bus Reset 수행 (NVMe controller reset 등) */
int pciehp_reset_slot(struct hotplug_slot *hotplug_slot, bool probe)
{
	struct controller *ctrl = to_ctrl(hotplug_slot); /* NVMe: controller 추출 */
	struct pci_dev *pdev = ctrl_dev(ctrl); /* NVMe: 다운스트림 포트 */
	int rc; /* NVMe: reset 결과 */

	if (probe) /* NVMe: probe 단계(사실상 능력 확인)면 */
		return 0; /* NVMe: 실제 리셋 안 함 */

	down_write_nested(&ctrl->reset_lock, ctrl->depth); /* NVMe: reset lock write 획득 */

	pci_hp_ignore_link_change(pdev); /* NVMe: 리셋으로 인한 링크 변화를 핫플러그로 인식하지 않도록 설정 */

	rc = pci_bridge_secondary_bus_reset(ctrl->pcie->port); /* NVMe: 다운스트림 bridge의 secondary bus reset 수행, NVMe SSD 리셋 */

	pci_hp_unignore_link_change(pdev); /* NVMe: 링크 변화 무시 해제 */

	up_write(&ctrl->reset_lock); /* NVMe: reset lock 해제 */
	return rc; /* NVMe: reset 결과 반환 */
}

/* NVMe: PCIe 핫플러그 컨트롤러의 IRQ 등록과 이벤트 알림 활성화 초기화 */
int pcie_init_notification(struct controller *ctrl)
{
	if (pciehp_request_irq(ctrl)) /* NVMe: MSI/MSI-X/INTx 또는 폴 스레드 시작 */
		return -1; /* NVMe: IRQ 등록 실패 */
	pcie_enable_notification(ctrl); /* NVMe: Slot Control에서 핫플러그 이벤트 enable */
	ctrl->notification_enabled = 1; /* NVMe: 알림 활성화 플래그 설정 */
	return 0; /* NVMe: 성공 */
}

/* NVMe: PCIe 핫플러그 컨트롤러의 알림 비활성화와 IRQ 해제 */
void pcie_shutdown_notification(struct controller *ctrl)
{
	if (ctrl->notification_enabled) { /* NVMe: 활성화된 경우에만 */
		pcie_disable_notification(ctrl); /* NVMe: Slot Control 이벤트 disable */
		pciehp_free_irq(ctrl); /* NVMe: IRQ 해제 또는 폴 스레드 중지 */
		ctrl->notification_enabled = 0; /* NVMe: 플래그 클리어 */
	}
}

/* NVMe: 디버그용으로 Slot Capabilities/Status/Control 값 출력 */
static inline void dbg_ctrl(struct controller *ctrl)
{
	struct pci_dev *pdev = ctrl->pcie->port; /* NVMe: 다운스트림 포트 */
	u16 reg16; /* NVMe: 16비트 레지스터 값 */

	ctrl_dbg(ctrl, "Slot Capabilities      : 0x%08x\n", ctrl->slot_cap); /* NVMe: Slot Capabilities */
	pcie_capability_read_word(pdev, PCI_EXP_SLTSTA, &reg16); /* NVMe: Slot Status 읽기 */
	ctrl_dbg(ctrl, "Slot Status            : 0x%04x\n", reg16); /* NVMe: Slot Status */
	pcie_capability_read_word(pdev, PCI_EXP_SLTCTL, &reg16); /* NVMe: Slot Control 읽기 */
	ctrl_dbg(ctrl, "Slot Control           : 0x%04x\n", reg16); /* NVMe: Slot Control */
}

/* NVMe: 디버그 출력용 플래그 매크로, 비트가 설정되면 '+', 아니면 '-' */
#define FLAG(x, y)	(((x) & (y)) ? '+' : '-')

/* NVMe: 현재 PCIe 장치부터 루트까지 거쳐올 중첩된 pciehp 다운스트림 포트 개수 계산 */
static inline int pcie_hotplug_depth(struct pci_dev *dev)
{
	struct pci_bus *bus = dev->bus; /* NVMe: 현재 장치의 버스 */
	int depth = 0; /* NVMe: 중첩 깊이 */

	while (bus->parent) { /* NVMe: 루트 버스까지 올라감 */
		bus = bus->parent; /* NVMe: 부모 버스로 이동 */
		if (bus->self && bus->self->is_pciehp) /* NVMe: 부모 bridge가 pciehp 포트면 */
			depth++; /* NVMe: 깊이 증가 */
	}

	return depth; /* NVMe: reset lock 중첩 깊이로 사용 */
}

/* NVMe: PCIe 핫플러그 컨트롤러를 초기화하고 상태를 설정 */
struct controller *pcie_init(struct pcie_device *dev)
{
	struct controller *ctrl; /* NVMe: 새 controller 구조체 포인터 */
	u32 slot_cap, slot_cap2; /* NVMe: Slot Capabilities / Slot Capabilities 2 */
	u8 poweron; /* NVMe: 전원 상태 저장 */
	struct pci_dev *pdev = dev->port; /* NVMe: 다운스트림 포트 */
	struct pci_bus *subordinate = pdev->subordinate; /* NVMe: 다운스트림 버스, NVMe 장치가 여기 연결 */

	ctrl = kzalloc_obj(*ctrl); /* NVMe: controller 구조체 메모리 할당 */
	if (!ctrl) /* NVMe: 메모리 부족 */
		return NULL; /* NVMe: 초기화 실패 */

	ctrl->pcie = dev; /* NVMe: 역참조용 pcie_device 저장 */
	ctrl->depth = pcie_hotplug_depth(dev->port); /* NVMe: reset lock 중첩 깊이 계산 */
	pcie_capability_read_dword(pdev, PCI_EXP_SLTCAP, &slot_cap); /* NVMe: Slot Capabilities 읽기 (NVMe 슬롯 능력) */

	if (pdev->hotplug_user_indicators) /* NVMe: 사용자 공간에서 indicator를 제어하면 */
		slot_cap &= ~(PCI_EXP_SLTCAP_AIP | PCI_EXP_SLTCAP_PIP); /* NVMe: Attention/Power Indicator Present 클리어 */

	/*
	 * We assume no Thunderbolt controllers support Command Complete events,
	 * but some controllers falsely claim they do.
	 */
	if (pdev->is_thunderbolt) /* NVMe: Thunderbolt 컨트롤러면 */
		slot_cap |= PCI_EXP_SLTCAP_NCCS; /* NVMe: No Command Completed Support 강제 설정 */

	ctrl->slot_cap = slot_cap; /* NVMe: controller에 slot capabilities 저장 */
	mutex_init(&ctrl->ctrl_lock); /* NVMe: Slot Control 명령용 뮤텍스 초기화 */
	mutex_init(&ctrl->state_lock); /* NVMe: 상태 전이용 뮤텍스 초기화 */
	init_rwsem(&ctrl->reset_lock); /* NVMe: reset용 rwsem 초기화 */
	init_waitqueue_head(&ctrl->requester); /* NVMe: 요청자 대기 큐 초기화 */
	init_waitqueue_head(&ctrl->queue); /* NVMe: Command Completed 대기 큐 초기화 */
	INIT_DELAYED_WORK(&ctrl->button_work, pciehp_queue_pushbutton_work); /* NVMe: Attention Button 눌림 지연 작업 초기화 */
	dbg_ctrl(ctrl); /* NVMe: 초기 레지스터 값 출력 */

	down_read(&pci_bus_sem); /* NVMe: 버스 장치 목록 읽기 락 */
	ctrl->state = list_empty(&subordinate->devices) ? OFF_STATE : ON_STATE; /* NVMe: 다운스트림에 NVMe 장치가 있으면 ON, 없으면 OFF */
	up_read(&pci_bus_sem); /* NVMe: 버스 락 해제 */

	pcie_capability_read_dword(pdev, PCI_EXP_SLTCAP2, &slot_cap2); /* NVMe: Slot Capabilities 2 읽기 */
	if (slot_cap2 & PCI_EXP_SLTCAP2_IBPD) { /* NVMe: In-Band Presence Disable Supported */
		pcie_write_cmd_nowait(ctrl, PCI_EXP_SLTCTL_IBPD_DISABLE, /* NVMe: inband presence 비활성화 */
				      PCI_EXP_SLTCTL_IBPD_DISABLE); /* NVMe: IBPD 마스크 */
		ctrl->inband_presence_disabled = 1; /* NVMe: 플래그 설정 */
	}

	if (dmi_first_match(inband_presence_disabled_dmi_table)) /* NVMe: DMI 테이블에 매칭되면 (Dell 등) */
		ctrl->inband_presence_disabled = 1; /* NVMe: inband presence 강제 비활성화 */

	/* Clear all remaining event bits in Slot Status register. */
	pcie_capability_write_word(pdev, PCI_EXP_SLTSTA, /* NVMe: Slot Status 레지스터 */
		PCI_EXP_SLTSTA_ABP | PCI_EXP_SLTSTA_PFD | /* NVMe: Attention Button/Power Fault */
		PCI_EXP_SLTSTA_MRLSC | PCI_EXP_SLTSTA_CC | /* NVMe: MRL Sensor/Command Completed */
		PCI_EXP_SLTSTA_DLLSC | PCI_EXP_SLTSTA_PDC); /* NVMe: Link/Presence Changed */

	ctrl_info(ctrl, "Slot #%d AttnBtn%c PwrCtrl%c MRL%c AttnInd%c PwrInd%c HotPlug%c Surprise%c Interlock%c NoCompl%c IbPresDis%c LLActRep%c%s\n", /* NVMe: 슬롯 능력 요약 출력 */
		FIELD_GET(PCI_EXP_SLTCAP_PSN, slot_cap), /* NVMe: Physical Slot Number */
		FLAG(slot_cap, PCI_EXP_SLTCAP_ABP), /* NVMe: Attention Button Present */
		FLAG(slot_cap, PCI_EXP_SLTCAP_PCP), /* NVMe: Power Controller Present */
		FLAG(slot_cap, PCI_EXP_SLTCAP_MRLSP), /* NVMe: MRL Sensor Present */
		FLAG(slot_cap, PCI_EXP_SLTCAP_AIP), /* NVMe: Attention Indicator Present */
		FLAG(slot_cap, PCI_EXP_SLTCAP_PIP), /* NVMe: Power Indicator Present */
		FLAG(slot_cap, PCI_EXP_SLTCAP_HPC), /* NVMe: Hot-Plug Capable */
		FLAG(slot_cap, PCI_EXP_SLTCAP_HPS), /* NVMe: Hot-Plug Surprise */
		FLAG(slot_cap, PCI_EXP_SLTCAP_EIP), /* NVMe: Electromechanical Interlock Present */
		FLAG(slot_cap, PCI_EXP_SLTCAP_NCCS), /* NVMe: No Command Completed Support */
		FLAG(slot_cap2, PCI_EXP_SLTCAP2_IBPD), /* NVMe: In-Band Presence Disable */
		FLAG(pdev->link_active_reporting, true), /* NVMe: Link Active Reporting Capable */
		pdev->broken_cmd_compl ? " (with Cmd Compl erratum)" : ""); /* NVMe: Command Completed erratum 여부 */

	/*
	 * If empty slot's power status is on, turn power off.  The IRQ isn't
	 * requested yet, so avoid triggering a notification with this command.
	 */
	if (POWER_CTRL(ctrl)) { /* NVMe: Power Controller 지원하면 */
		pciehp_get_power_status(ctrl, &poweron); /* NVMe: 현재 전원 상태 읽기 */
		if (!pciehp_card_present_or_link_active(ctrl) && poweron) { /* NVMe: 장치 없는데 전원 켜져 있으면 */
			pcie_disable_notification(ctrl); /* NVMe: 알림 끔 (IRQ 아직 등록 안 됨) */
			pciehp_power_off_slot(ctrl); /* NVMe: 전원 off */
		}
	}

	pdev = pci_get_slot(subordinate, PCI_DEVFN(0, 0)); /* NVMe: 다운스트림 버스의 첫 장치 얻기 (NVMe SSD) */
	if (pdev) /* NVMe: 장치가 있으면 */
		ctrl->dsn = pci_get_dsn(pdev); /* NVMe: Device Serial Number 저장, 장치 교체 감지용 */
	pci_dev_put(pdev); /* NVMe: pci_get_slot으로 증가한 ref 반납 */

	return ctrl; /* NVMe: 초기화된 controller 반환 */
}

/* NVMe: controller 해제 (지연 작업 취소 및 메모리 반납) */
void pciehp_release_ctrl(struct controller *ctrl)
{
	cancel_delayed_work_sync(&ctrl->button_work); /* NVMe: Attention Button 지연 작업 취소 및 완료 대기 */
	kfree(ctrl); /* NVMe: controller 메모리 반납 */
}

/* NVMe: Intel/QCOM bridge의 Command Completed erratum을 조기에 표시하는 quirk */
static void quirk_cmd_compl(struct pci_dev *pdev)
{
	u32 slot_cap; /* NVMe: Slot Capabilities 값 */

	if (pci_is_pcie(pdev)) { /* NVMe: PCIe 장치인 경우에만 */
		pcie_capability_read_dword(pdev, PCI_EXP_SLTCAP, &slot_cap); /* NVMe: Slot Capabilities 읽기 */
		if (slot_cap & PCI_EXP_SLTCAP_HPC && /* NVMe: Hot-Plug Capable이고 */
		    !(slot_cap & PCI_EXP_SLTCAP_NCCS)) /* NVMe: Command Completed Supported라고 주장하면 */
			pdev->broken_cmd_compl = 1; /* NVMe: broken_cmd_compl 플래그 설정 */
	}
}
/* NVMe: Intel PCI bridge 클래스(PCI_CLASS_BRIDGE_PCI)에서 broken cmd compl erratum 적용 */
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_INTEL, PCI_ANY_ID,
			      PCI_CLASS_BRIDGE_PCI, 8, quirk_cmd_compl);
/* NVMe: Qualcomm 0x010e bridge에서 broken cmd compl erratum 적용 */
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_QCOM, 0x010e,
			      PCI_CLASS_BRIDGE_PCI, 8, quirk_cmd_compl);
/* NVMe: Qualcomm 0x0110 bridge에서 broken cmd compl erratum 적용 */
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_QCOM, 0x0110,
			      PCI_CLASS_BRIDGE_PCI, 8, quirk_cmd_compl);
/* NVMe: Qualcomm 0x0400 bridge에서 broken cmd compl erratum 적용 */
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_QCOM, 0x0400,
			      PCI_CLASS_BRIDGE_PCI, 8, quirk_cmd_compl);
/* NVMe: Qualcomm 0x0401 bridge에서 broken cmd compl erratum 적용 */
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_QCOM, 0x0401,
			      PCI_CLASS_BRIDGE_PCI, 8, quirk_cmd_compl);
/* NVMe: HXT 0x0401 bridge에서 broken cmd compl erratum 적용 */
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_HXT, 0x0401,
			      PCI_CLASS_BRIDGE_PCI, 8, quirk_cmd_compl);
