// SPDX-License-Identifier: GPL-2.0+
/*
 * PCIe bandwidth controller
 *
 * Author: Alexandru Gagniuc <mr.nuke.me@gmail.com>
 *
 * Copyright (C) 2019 Dell Inc
 * Copyright (C) 2023-2024 Intel Corporation
 *
 * The PCIe bandwidth controller provides a way to alter PCIe Link Speeds
 * and notify the operating system when the Link Width or Speed changes. The
 * notification capability is required for all Root Ports and Downstream
 * Ports supporting Link Width wider than x1 and/or multiple Link Speeds.
 *
 * This service port driver hooks into the Bandwidth Notification interrupt
 * watching for changes or links becoming degraded in operation. It updates
 * the cached Current Link Speed that is exposed to user space through sysfs.
 */

/*
 * ===================================================================
 * NVMe PCIe 호스트 드라이버 관점 파일 요약
 * -------------------------------------------------------------------
 * 본 파일(drivers/pci/pcie/bwctrl.c)은 PCIe 링크 대역폭(Link Speed/Width)
 * 변화를 감지하고, 운영체제가 원하는 target link speed를 설정하는 PCIe
 * bandwidth controller 서비스 드라이버이다.
 *
 * NVMe SSD 입장에서 본 파일은 다음과 밀접하게 연관된다.
 *   - NVMe 장치가 연결된 Root Port/Downstream Port의 현재 링크 속도 캐싱
 *     및 sysfs 노출
 *   - 링크 속도/폭 변경 시 Bandwidth Change Notification(BCN) 인터럽트 처리
 *   - NVMe 성능에 직접 영향을 주는 link speed downgrade/upgrade 감지
 *   - thermal cooling device 등록을 통한 NVMe 링크 속도 기반 쓰로틀링
 *
 * 일반적인 NVMe 드라이버와의 연관 경로:
 *   NVMe SSD -> Root Port -> pcie_bwctrl_driver (PCIE_PORT_SERVICE_BWCTRL)
 *   - pcie_bwnotif_irq(): 링크 상태 변화 시 호출, NVMe 장치의 PCIe 링크
 *     속도 변화를 OS에 알림
 *   - pcie_set_target_speed(): NVMe 장치 아래 링크 속도를 요청 속도로
 *     변경(예: cooling policy 또는 사용자 sysfs 요청)
 *   - pcie_update_link_speed(): 변경된 링크 속도를 bus->cur_bus_speed에
 *     반영, NVMe 드라이버에서 pci_get_max_link_speed() 등으로 확인 가능
 *
 * 본 파일은 PCIe native hotplug, PTM(Precision Time Measurement), DOE,
 * ROM 등과 직접 연관되지는 않지만, NVMe SSD의 PCIe link 품질/속도를
 * 모니터링하고 제어하는 핵심 서비스 드라이버이다.
 * ===================================================================
 */

#define dev_fmt(fmt) "bwctrl: " fmt /* NVMe: dmesg 등에서 메시지 접두사로 "bwctrl:" 사용. */

#include <linux/atomic.h>   /* NVMe: 원자적 연산을 위한 헤더. */
#include <linux/bitops.h>   /* NVMe: 비트 연산(비트 설정/해제) 헤더. */
#include <linux/bits.h>     /* NVMe: GENMASK 등 비트 유틸리티 헤더. */
#include <linux/cleanup.h>  /* NVMe: scoped_guard 자동 정리 헤더. */
#include <linux/errno.h>    /* NVMe: 에러 코드 정의 헤더. */
#include <linux/interrupt.h>/* NVMe: IRQ 처리(request_irq, free_irq 등) 헤더. */
#include <linux/mutex.h>    /* NVMe: 링크 속도 변경 직렬화를 위한 mutex 헤더. */
#include <linux/pci.h>      /* NVMe: PCIe 핵심 구조체 및 함수 헤더. */
#include <linux/pci-bwctrl.h>/* NVMe: PCIe bandwidth controller 외부 인터페이스 헤더. */
#include <linux/rwsem.h>    /* NVMe: 읽기/쓰기 세마포어 헤더. */
#include <linux/slab.h>     /* NVMe: 메모리 할당(kzalloc 등) 헤더. */
#include <linux/types.h>    /* NVMe: 기본 타입 정의 헤더. */

#include "../pci.h"         /* NVMe: PCI 서브시스템 내부 함수/매크로 헤더. */
#include "portdrv.h"        /* NVMe: PCIe port service driver 헤더. */

/**
 * struct pcie_bwctrl_data - PCIe bandwidth controller
 * @set_speed_mutex:	Serializes link speed changes
 * @cdev:		Thermal cooling device associated with the port
 */
/* NVMe: PCIe bandwidth controller의 NVMe 링크 속도 제어를 위한 per-port 데이터 구조체. */
struct pcie_bwctrl_data {
	struct mutex set_speed_mutex;              /* NVMe: 동일 포트에서 링크 속도 변경을 직렬화. */
	struct thermal_cooling_device *cdev;       /* NVMe: 해당 포트의 thermal cooling device 포인터. */
};

/* Prevent port removal during Link Speed changes. */
/* NVMe: 링크 속도 변경 중 포트가 제거되는 것을 방지하는 전역 읽기/쓰기 세마포어. */
static DECLARE_RWSEM(pcie_bwctrl_setspeed_rwsem);

/*
 * pcie_valid_speed:
 *   주어진 PCIe bus speed가 유효한 범위 내에 있는지 검사한다.
 *   NVMe 장치가 지원하는 링크 속도가 올바른 값인지 확인할 때 사용된다.
 */
static bool pcie_valid_speed(enum pci_bus_speed speed)
{
	return (speed >= PCIE_SPEED_2_5GT) && (speed <= PCIE_SPEED_64_0GT); /* NVMe: 2.5GT/s ~ 64GT/s 범위면 true. */
}

/*
 * pci_bus_speed2lnkctl2:
 *   enum pci_bus_speed 값을 PCI Express Link Control 2 레지스터의
 *   Target Link Speed(TLS) 필드에 쓸 값으로 변환한다.
 *   NVMe 장치의 링크 속도 변경 시 Root Port 레지스터에 기록할 값을 만든다.
 */
static u16 pci_bus_speed2lnkctl2(enum pci_bus_speed speed)
{
	static const u8 speed_conv[] = {           /* NVMe: bus speed -> Link Control 2 TLS 값 매핑 테이블. */
		[PCIE_SPEED_2_5GT] = PCI_EXP_LNKCTL2_TLS_2_5GT,   /* NVMe: 2.5GT/s -> TLS 1. */
		[PCIE_SPEED_5_0GT] = PCI_EXP_LNKCTL2_TLS_5_0GT,   /* NVMe: 5GT/s -> TLS 2. */
		[PCIE_SPEED_8_0GT] = PCI_EXP_LNKCTL2_TLS_8_0GT,   /* NVMe: 8GT/s -> TLS 3. */
		[PCIE_SPEED_16_0GT] = PCI_EXP_LNKCTL2_TLS_16_0GT, /* NVMe: 16GT/s -> TLS 4. */
		[PCIE_SPEED_32_0GT] = PCI_EXP_LNKCTL2_TLS_32_0GT, /* NVMe: 32GT/s -> TLS 5. */
		[PCIE_SPEED_64_0GT] = PCI_EXP_LNKCTL2_TLS_64_0GT, /* NVMe: 64GT/s -> TLS 6. */
	};

	if (WARN_ON_ONCE(!pcie_valid_speed(speed))) /* NVMe: speed가 유효 범위가 아니면 경고 출력. */
		return 0;                              /* NVMe: 잘못된 speed면 0 반환. */

	return speed_conv[speed];                  /* NVMe: TLS 값 반환. */
}

/*
 * pcie_supported_speeds2target_speed:
 *   supported_speeds 비트맵에서 가장 높은 속도 비트를 추출해 target speed로 변환.
 *   NVMe 장치와 Root Port가 공통으로 지원하는 최고 속도를 선택할 때 사용.
 */
static inline u16 pcie_supported_speeds2target_speed(u8 supported_speeds)
{
	return __fls(supported_speeds);            /* NVMe: 비트맵에서 최상위 비트 인덱스를 target speed로 반환. */
}

/**
 * pcie_bwctrl_select_speed - Select Target Link Speed
 * @port:	PCIe Port
 * @speed_req:	Requested PCIe Link Speed
 *
 * Select Target Link Speed by take into account Supported Link Speeds of
 * both the Root Port and the Endpoint.
 *
 * Return: Target Link Speed (1=2.5GT/s, 2=5GT/s, 3=8GT/s, etc.)
 */
/*
 * NVMe: NVMe endpoint와 Root Port/Downstream Port가 모두 지원하는 속도 중
 * 요청된 speed_req에 가장 가까운(하지만 초과하지 않는) target speed를 선택.
 * NVMe SSD가 Gen4로 협상되도록 요청했으나 Root Port가 Gen3까지만 지원하면
 * Gen3로 낮춰 설정하는 등의 상황을 처리.
 */
static u16 pcie_bwctrl_select_speed(struct pci_dev *port, enum pci_bus_speed speed_req)
{
	struct pci_bus *bus = port->subordinate;   /* NVMe: 이 포트 아래의 하위 bus(연결된 NVMe 포함). */
	u8 desired_speeds, supported_speeds;       /* NVMe: 희망 속도 마스크와 공통 지원 속도 마스크. */
	struct pci_dev *dev;                       /* NVMe: 하위 bus의 첫 번째 PCIe 장치(예: NVMe). */

	desired_speeds = GENMASK(pci_bus_speed2lnkctl2(speed_req), /* NVMe: speed_req 이하의 모든 속도 비트를 1로 설정. */
				 __fls(PCI_EXP_LNKCAP2_SLS_2_5GB));    /* NVMe: 최저 속도(2.5GB) 비트 위치. */

	supported_speeds = port->supported_speeds; /* NVMe: 우선 Root Port/포트 자체가 지원하는 속도로 초기화. */
	if (bus) {                                 /* NVMe: 하위 bus가 존재하면(NVMe 장치가 연결되어 있으면). */
		down_read(&pci_bus_sem);               /* NVMe: pci_bus_sem을 읽기 모드로 획득(장치 리스트 보호). */
		dev = list_first_entry_or_null(&bus->devices, struct pci_dev, bus_list); /* NVMe: 하위 bus의 첫 장치(예: NVMe) 획득. */
		if (dev)                               /* NVMe: NVMe endpoint가 존재하면. */
			supported_speeds &= dev->supported_speeds; /* NVMe: 포트와 NVMe가 공통 지원하는 속도만 남김. */
		up_read(&pci_bus_sem);                 /* NVMe: pci_bus_sem 읽기 잠금 해제. */
	}
	if (!supported_speeds)                     /* NVMe: 공통 지원 속도가 없으면(예: 버스가 비어 있거나). */
		supported_speeds = PCI_EXP_LNKCAP2_SLS_2_5GB; /* NVMe: 안전하게 2.5GT/s로 폴백. */

	return pcie_supported_speeds2target_speed(supported_speeds & desired_speeds); /* NVMe: 희망 범위 내 공통 최고 속도 반환. */
}

/*
 * pcie_bwctrl_change_speed:
 *   Root Port의 Link Control 2 레지스터 TLS 필드를 target_speed로 설정하고
 *   link retrain을 수행한다.
 *   NVMe 장치 아래 링크의 실제 속도를 변경하는 핵심 동작.
 */
static int pcie_bwctrl_change_speed(struct pci_dev *port, u16 target_speed, bool use_lt)
{
	int ret;                                   /* NVMe: 함수 반환값 변수. */

	ret = pcie_capability_clear_and_set_word(port, PCI_EXP_LNKCTL2, /* NVMe: Link Control 2 레지스터의 TLS 필드 갱신. */
						 PCI_EXP_LNKCTL2_TLS, target_speed); /* NVMe: 기존 TLS 비트를 클리어하고 target_speed 설정. */
	if (ret != PCIBIOS_SUCCESSFUL)             /* NVMe: 레지스터 쓰기가 성공하지 않으면. */
		return pcibios_err_to_errno(ret);      /* NVMe: PCIBIOS 에러를 errno로 변환해 반환. */

	return pcie_retrain_link(port, use_lt);    /* NVMe: link retrain 실행, use_lt로 종료 조건 선택. */
}

/**
 * pcie_set_target_speed - Set downstream Link Speed for PCIe Port
 * @port:	PCIe Port
 * @speed_req:	Requested PCIe Link Speed
 * @use_lt:	Wait for the LT or DLLLA bit to detect the end of link training
 *
 * Attempt to set PCIe Port Link Speed to @speed_req. @speed_req may be
 * adjusted downwards to the best speed supported by both the Port and PCIe
 * Device underneath it.
 *
 * Return:
 * * 0		- on success
 * * -EINVAL	- @speed_req is not a PCIe Link Speed
 * * -ENODEV	- @port is not controllable
 * * -ETIMEDOUT	- changing Link Speed took too long
 * * -EAGAIN	- Link Speed was changed but @speed_req was not achieved
 */
/*
 * NVMe: NVMe 장치가 연결된 downstream 포트의 링크 속도를 speed_req로 설정.
 * sysfs(thermal cooling 등)나 power management에서 호출될 수 있으며,
 * 설정 후 bus->cur_bus_speed가 갱신되어 NVMe 드라이버가 최대 링크 속도를
 * 확인할 수 있다.
 */
int pcie_set_target_speed(struct pci_dev *port, enum pci_bus_speed speed_req,
			  bool use_lt)
{
	struct pci_bus *bus = port->subordinate;   /* NVMe: NVMe 장치가 연결된 하위 bus. */
	u16 target_speed;                          /* NVMe: 실제로 레지스터에 쓸 target link speed. */
	int ret;                                   /* NVMe: 반환값 변수. */

	if (WARN_ON_ONCE(!pcie_valid_speed(speed_req))) /* NVMe: 요청 속도가 유효하지 않으면 경고. */
		return -EINVAL;                        /* NVMe: 잘못된 인자 에러 반환. */

	if (bus && bus->cur_bus_speed == speed_req) /* NVMe: 이미 원하는 속도로 설정되어 있으면. */
		return 0;                              /* NVMe: 아무 것도 하지 않고 성공 반환. */

	target_speed = pcie_bwctrl_select_speed(port, speed_req); /* NVMe: 포트와 NVMe가 공통 지원하는 target speed 산출. */

	scoped_guard(rwsem_read, &pcie_bwctrl_setspeed_rwsem) { /* NVMe: setspeed_rwsem을 읽기 잠금(포트 제거 방지). */
		struct pcie_bwctrl_data *data = port->link_bwctrl; /* NVMe: 포트의 bandwidth controller 데이터 획득. */

		/*
		 * port->link_bwctrl is NULL during initial scan when called
		 * e.g. from the Target Speed quirk.
		 */
		if (data)                              /* NVMe: 초기 scan 외에는 bwctrl 데이터가 존재. */
			mutex_lock(&data->set_speed_mutex); /* NVMe: 동일 포트에서 속도 변경 직렬화. */

		ret = pcie_bwctrl_change_speed(port, target_speed, use_lt); /* NVMe: 실제 링크 속도 변경 시도. */

		if (data)                              /* NVMe: bwctrl 데이터가 존재하면. */
			mutex_unlock(&data->set_speed_mutex); /* NVMe: 속도 변경 mutex 해제. */
	}

	/*
	 * Despite setting higher speed into the Target Link Speed, empty
	 * bus won't train to 5GT+ speeds.
	 */
	if (!ret && bus && bus->cur_bus_speed != speed_req && /* NVMe: 성공했으나 요청 속도와 현재 속도가 다르고. */
	    !list_empty(&bus->devices))              /* NVMe: 버스에 NVMe 장치가 실제로 연결된 경우. */
		ret = -EAGAIN;                         /* NVMe: 요청 속도 달성 실패, 재시도 필요 알림. */

	return ret;                                /* NVMe: 성공/에러 코드 반환. */
}

/*
 * pcie_bwnotif_enable:
 *   Bandwidth Notification 인터럽트를 활성화하고 현재 LBMS 상태를 기록한다.
 *   NVMe 장치의 링크 속도/폭 변화를 OS에 알릴 수 있도록 준비.
 */
static void pcie_bwnotif_enable(struct pcie_device *srv)
{
	struct pci_dev *port = srv->port;          /* NVMe: 서비스가 동작하는 PCIe 포트(Root/Downstream). */
	u16 link_status;                           /* NVMe: Link Status 레지스터 값을 읽을 변수. */
	int ret;                                   /* NVMe: 반환값 변수. */

	/* Note if LBMS has been seen so far */
	ret = pcie_capability_read_word(port, PCI_EXP_LNKSTA, &link_status); /* NVMe: Link Status 레지스터 읽기. */
	if (ret == PCIBIOS_SUCCESSFUL && link_status & PCI_EXP_LNKSTA_LBMS) /* NVMe: 읽기 성공하고 LBMS 비트가 설정되어 있으면. */
		set_bit(PCI_LINK_LBMS_SEEN, &port->priv_flags); /* NVMe: LBMS가 이미 관찰되었음을 priv_flags에 기록. */

	pcie_capability_set_word(port, PCI_EXP_LNKCTL, /* NVMe: Link Control 레지스터 설정. */
				 PCI_EXP_LNKCTL_LBMIE | PCI_EXP_LNKCTL_LABIE); /* NVMe: Link Bandwidth Management Interrupt Enable + Link Autonomous Bandwidth Interrupt Enable. */
	pcie_capability_write_word(port, PCI_EXP_LNKSTA, /* NVMe: Link Status 레지스터에 쓰기. */
				   PCI_EXP_LNKSTA_LBMS | PCI_EXP_LNKSTA_LABS); /* NVMe: LBMS/LABS 상태 비트 클리어(W1C). */

	/*
	 * Update after enabling notifications & clearing status bits ensures
	 * link speed is up to date.
	 */
	pcie_update_link_speed(port->subordinate, PCIE_BWCTRL_ENABLE); /* NVMe: 인터럽트 활성화 후 현재 링크 속도를 캐시에 갱신. */
}

/*
 * pcie_bwnotif_disable:
 *   Bandwidth Notification 인터럽트를 비활성화한다.
 *   NVMe 장치 제거/suspend 등에서 링크 변화 알림을 멈출 때 사용.
 */
static void pcie_bwnotif_disable(struct pci_dev *port)
{
	pcie_capability_clear_word(port, PCI_EXP_LNKCTL, /* NVMe: Link Control 레지스터에서 특정 비트 클리어. */
				   PCI_EXP_LNKCTL_LBMIE | PCI_EXP_LNKCTL_LABIE); /* NVMe: LBMIE와 LABIE 비트 클리어로 인터럽트 비활성화. */
}

/*
 * pcie_bwnotif_irq:
 *   Bandwidth Notification 인터럽트 핸들러.
 *   NVMe 장치의 PCIe 링크 속도가 하드웨어적으로 변경되었을 때 호출되어
 *   sysfs에 노출된 bus->cur_bus_speed를 갱신한다.
 */
static irqreturn_t pcie_bwnotif_irq(int irq, void *context)
{
	struct pcie_device *srv = context;         /* NVMe: 인터럽트 컨텍스트에서 pcie_device 포인터 복원. */
	struct pci_dev *port = srv->port;          /* NVMe: 서비스가 동작하는 PCIe 포트. */
	u16 link_status, events;                   /* NVMe: Link Status 값과 발생한 이벤트 마스크. */
	int ret;                                   /* NVMe: 반환값 변수. */

	ret = pcie_capability_read_word(port, PCI_EXP_LNKSTA, &link_status); /* NVMe: Link Status 레지스터 읽기. */
	if (ret != PCIBIOS_SUCCESSFUL)             /* NVMe: 레지스터 읽기 실패하면. */
		return IRQ_NONE;                       /* NVMe: 이 IRQ가 아님을 알림. */

	events = link_status & (PCI_EXP_LNKSTA_LBMS | PCI_EXP_LNKSTA_LABS); /* NVMe: LBMS/LABS 이벤트 비트만 추출. */
	if (!events)                               /* NVMe: bandwidth 관련 이벤트가 없으면. */
		return IRQ_NONE;                       /* NVMe: 이 IRQ가 아님을 알림. */

	if (events & PCI_EXP_LNKSTA_LBMS)          /* NVMe: Link Bandwidth Management Status가 설정되었으면. */
		set_bit(PCI_LINK_LBMS_SEEN, &port->priv_flags); /* NVMe: LBMS 관찰 플래그 설정. */

	pcie_capability_write_word(port, PCI_EXP_LNKSTA, events); /* NVMe: LBMS/LABS 상태 비트를 W1C로 클리어. */

	/*
	 * Interrupts will not be triggered from any further Link Speed
	 * change until LBMS is cleared by the write. Therefore, re-read the
	 * speed (inside pcie_update_link_speed()) after LBMS has been
	 * cleared to avoid missing link speed changes.
	 */
	pcie_update_link_speed(port->subordinate, PCIE_BWCTRL_IRQ); /* NVMe: 클리어 후 현재 링크 속도를 다시 읽어 캐시 갱신. */

	return IRQ_HANDLED;                        /* NVMe: IRQ 처리 완료. */
}

/*
 * pcie_reset_lbms:
 *   LBMS(Link Bandwidth Management Status) 플래그와 상태 비트를 리셋.
 *   NVMe 링크 속도 재협상 전후에 상태를 초기화할 때 사용.
 */
void pcie_reset_lbms(struct pci_dev *port)
{
	clear_bit(PCI_LINK_LBMS_SEEN, &port->priv_flags); /* NVMe: LBMS 관찰 플래그 클리어. */
	pcie_capability_write_word(port, PCI_EXP_LNKSTA, PCI_EXP_LNKSTA_LBMS); /* NVMe: LBMS 상태 비트를 W1C로 클리어. */
}

/*
 * pcie_bwnotif_probe:
 *   PCIe bandwidth notification 서비스 드라이버의 probe 함수.
 *   NVMe 장치가 연결된 포트에서 bwctrl 서비스를 등록하고 인터럽트를
 *   활성화하며, thermal cooling device를 등록한다.
 */
static int pcie_bwnotif_probe(struct pcie_device *srv)
{
	struct pci_dev *port = srv->port;          /* NVMe: 서비스가 동작하는 PCIe 포트. */
	int ret;                                   /* NVMe: 반환값 변수. */

	if (port->no_bw_notif)                     /* NVMe: 해당 포트에서 bandwidth notification을 지원하지 않으면. */
		return -ENODEV;                        /* NVMe: probe 실패. */

	/* Can happen if we run out of bus numbers during enumeration. */
	if (!port->subordinate)                    /* NVMe: 초기화 중 bus 번호가 부족해 하위 bus가 없으면. */
		return -ENODEV;                        /* NVMe: probe 실패. */

	struct pcie_bwctrl_data *data = devm_kzalloc(&srv->device, /* NVMe: 서비스 device에 매핑될 bwctrl 데이터 메모리 할당. */
						     sizeof(*data), GFP_KERNEL); /* NVMe: 커널 메모리에서 구조체 크기만큼 할당. */
	if (!data)                                 /* NVMe: 메모리 할당 실패 시. */
		return -ENOMEM;                        /* NVMe: 메모리 부족 에러 반환. */

	ret = devm_mutex_init(&srv->device, &data->set_speed_mutex); /* NVMe: 메모리 해제 시 자동 정리되는 mutex 초기화. */
	if (ret)                                   /* NVMe: mutex 초기화 실패하면. */
		return ret;                            /* NVMe: 에러 반환. */

	scoped_guard(rwsem_write, &pcie_bwctrl_setspeed_rwsem) { /* NVMe: 포트 제거 방지를 위해 쓰기 잠금 획득. */
		port->link_bwctrl = data;              /* NVMe: 포트 구조체에 bwctrl 데이터 연결. */

		ret = request_irq(srv->irq, pcie_bwnotif_irq, /* NVMe: bandwidth notification 인터럽트 핸들러 등록. */
				  IRQF_SHARED, "PCIe bwctrl", srv); /* NVMe: 공유 IRQ로 등록, 서비스 객체를 dev_id로 전달. */
		if (ret) {                             /* NVMe: IRQ 등록 실패 시. */
			port->link_bwctrl = NULL;          /* NVMe: 포트에서 bwctrl 데이터 연결 해제. */
			return ret;                        /* NVMe: 에러 반환. */
		}

		pcie_bwnotif_enable(srv);              /* NVMe: 인터럽트 활성화 및 초기 링크 속도 갱신. */
	}

	pci_dbg(port, "enabled with IRQ %d\n", srv->irq); /* NVMe: bandwidth controller 활성화 로그 출력. */

	/* Don't fail on errors. Don't leave IS_ERR() "pointer" into ->cdev */
	port->link_bwctrl->cdev = pcie_cooling_device_register(port); /* NVMe: thermal cooling device 등록(링크 속도 기반 쓰로틀링). */
	if (IS_ERR(port->link_bwctrl->cdev))       /* NVMe: 등록 결과가 에러 포인터이면. */
		port->link_bwctrl->cdev = NULL;        /* NVMe: NULL로 설정해 dangling 포인터 방지. */

	return 0;                                  /* NVMe: probe 성공. */
}

/*
 * pcie_bwnotif_remove:
 *   PCIe bandwidth notification 서비스 드라이버의 remove 함수.
 *   NVMe 장치가 제거되거나 서비스가 해제될 때 인터럽트와 cooling device를
 *   정리한다.
 */
static void pcie_bwnotif_remove(struct pcie_device *srv)
{
	struct pcie_bwctrl_data *data = srv->port->link_bwctrl; /* NVMe: 포트의 bwctrl 데이터 획득. */

	pcie_cooling_device_unregister(data->cdev); /* NVMe: thermal cooling device 해제. */

	scoped_guard(rwsem_write, &pcie_bwctrl_setspeed_rwsem) { /* NVMe: 포트 제거 방지 쓰기 잠금. */
		pcie_bwnotif_disable(srv->port);       /* NVMe: bandwidth notification 인터럽트 비활성화. */

		free_irq(srv->irq, srv);               /* NVMe: 등록했던 IRQ 해제. */

		srv->port->link_bwctrl = NULL;         /* NVMe: 포트에서 bwctrl 데이터 연결 해제. */
	}
}

/*
 * pcie_bwnotif_suspend:
 *   시스템 suspend 시 bandwidth notification 인터럽트를 비활성화.
 *   NVMe 장치가 있는 시스템의 절전 상태 진입 시 호출.
 */
static int pcie_bwnotif_suspend(struct pcie_device *srv)
{
	pcie_bwnotif_disable(srv->port);           /* NVMe: 인터럽트 비활성화. */
	return 0;                                  /* NVMe: suspend 성공. */
}

/*
 * pcie_bwnotif_resume:
 *   시스템 resume 시 bandwidth notification 인터럽트를 다시 활성화.
 *   NVMe 장치가 있는 시스템이 절전에서 복귀할 때 호출.
 */
static int pcie_bwnotif_resume(struct pcie_device *srv)
{
	pcie_bwnotif_enable(srv);                  /* NVMe: 인터럽트 재활성화 및 링크 속도 갱신. */
	return 0;                                  /* NVMe: resume 성공. */
}

/*
 * pcie_bwctrl_driver:
 *   PCIe port service driver 등록 구조체.
 *   NVMe 장치가 연결된 모든 PCIe 포트(PCIE_ANY_PORT)에서 bandwidth
 *   controller 서비스를 제공한다.
 */
static struct pcie_port_service_driver pcie_bwctrl_driver = {
	.name		= "pcie_bwctrl",           /* NVMe: 서비스 드라이버 이름. */
	.port_type	= PCIE_ANY_PORT,           /* NVMe: 모든 PCIe 포트 타입에서 동작. */
	.service	= PCIE_PORT_SERVICE_BWCTRL, /* NVMe: bandwidth controller 서비스 ID. */
	.probe		= pcie_bwnotif_probe,      /* NVMe: probe 함수 등록. */
	.suspend	= pcie_bwnotif_suspend,    /* NVMe: suspend 콜백 등록. */
	.resume		= pcie_bwnotif_resume,     /* NVMe: resume 콜백 등록. */
	.remove		= pcie_bwnotif_remove,     /* NVMe: remove 콜백 등록. */
};

/*
 * pcie_bwctrl_init:
 *   PCIe bandwidth controller 서비스 드라이버를 PCIe port service
 *   레지스트리에 등록.
 *   NVMe PCIe 호스트 드라이버가 로드되기 전에 PCI 서브시스템에서 먼저
 *   등록되어 NVMe 장치의 링크 대역폭 변화를 감시할 수 있게 한다.
 */
int __init pcie_bwctrl_init(void)
{
	return pcie_port_service_register(&pcie_bwctrl_driver); /* NVMe: bwctrl 서비스 드라이버 등록. */
}
