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
 * [한국어 설명] 링크 속도 변화를 감지하고 속도를 제어하는 서비스 (bwctrl.c)
 *
 * === 파일의 역할 ===
 * PCIe 링크는 동작 중에 속도와 폭이 바뀔 수 있다. 신호 품질이 나빠지면
 * 하드웨어가 스스로 속도를 낮추고, 전력 관리 정책이 폭을 줄이기도 한다.
 * 이 파일은 그 변화를 알림(Link Bandwidth Notification)으로 받아 처리한다.
 *
 * 두 가지 알림 비트가 있다.
 *   LBMS (Link Bandwidth Management Status) - 소프트웨어가 요청한 속도
 *         변경이 끝났을 때 선다.
 *   LABS (Link Autonomous Bandwidth Status) - 하드웨어가 스스로 속도를
 *         바꿨을 때 선다. 이쪽이 문제 신호다 — 링크가 불안정하다는 뜻이다.
 *
 * 하는 일은 둘이다.
 *   1) 감지 - 알림 인터럽트를 받아 현재 링크 속도를 다시 읽고,
 *      pci_dev 의 캐시(sysfs 의 current_link_speed)를 갱신한다.
 *      그러지 않으면 sysfs 값이 실제와 어긋난 채로 남는다.
 *   2) 제어 - 속도 상한을 지정할 수 있게 한다(pcie_set_target_speed).
 *      LNKCTL2 의 Target Link Speed 를 바꾸고 링크를 재훈련한다.
 *      thermal cooling device 로도 노출되어, 발열이 심하면 속도를 낮춰
 *      전력을 줄이는 데 쓸 수 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 등록: portdrv 가 BWCTRL 서비스를 가진 포트에 바인딩
 *         -> pcie_bwnotif_probe() -> IRQ 등록, LNKCTL 의 알림 활성화
 *
 * 발생: 링크 속도/폭 변화
 *         -> [이 파일] pcie_bwnotif_irq()(하드 IRQ)
 *            -> pcie_bwnotif_irq_thread()(스레드)
 *               -> pcie_update_link_speed() 로 캐시 갱신
 *               -> LBMS/LABS 상태 비트를 지운다(RW1C)
 *
 * 제어: thermal 코어 또는 커널 내부
 *         -> [이 파일] pcie_set_target_speed()
 *            -> LNKCTL2 설정 -> 링크 재훈련 -> 완료 대기
 *
 * 실행 컨텍스트: IRQ 핸들러는 하드 IRQ, 실제 처리는 스레드. 속도 제어는
 * 프로세스 컨텍스트(재훈련 대기가 있다).
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pcie/portdrv.c(서비스 등록), thermal 서브시스템(cooling device).
 * 아래쪽: pci.c 의 pcie_update_link_speed(), access.c 의 capability 접근.
 * 공유 상태: struct pci_dev 의 current_link_speed / current_link_width
 *   (sysfs 에 노출되는 캐시), 그리고 이 파일이 관리하는 속도 상한 목록.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 직접 부르지 않는다(전수 확인).
 *
 * NVMe 학습에서 이 파일이 의미 있는 지점은 진단이다. NVMe SSD 의 성능이
 * 갑자기 떨어졌을 때, 링크가 Gen4 에서 Gen1 으로 내려앉은 것이 원인일 수
 * 있다. 하드웨어가 신호 품질 문제로 스스로 낮춘 경우이며, LABS 알림이
 * 그것을 잡아낸다.
 *
 * 이 서비스가 없으면 sysfs 의 current_link_speed 가 옛 값 그대로 남아,
 * 실제로는 느려졌는데도 정상으로 보인다. 그래서 이 파일의 진짜 가치는
 * "속도를 바꾸는 것" 보다 "바뀐 것을 정확히 알려 주는 것" 에 있다.
 *
 * === 주요 함수/구조체 요약 ===
 * pcie_bwnotif_probe()        : 포트에 이 서비스를 붙인다. IRQ 를 등록하고
 *                               LNKCTL 의 LBMIE/LABIE 알림 비트를 켠다.
 * pcie_bwnotif_irq()          : 하드 IRQ. 자기 인터럽트인지 확인하고
 *                               스레드를 깨운다.
 * pcie_bwnotif_irq_thread()   : 링크 속도를 다시 읽어 캐시를 갱신하고
 *                               상태 비트를 지운다.
 * pcie_update_link_speed()    : 하위 버스의 속도 캐시를 갱신한다(pci.c 정의).
 * pcie_set_target_speed()     : 링크 속도 상한을 지정하고 재훈련한다.
 * pcie_bwctrl_select_speed()  : 여러 제약(thermal, 사용자 지정) 중
 *                               가장 낮은 값을 고른다.
 * pcie_bwnotif_enable() / _disable() : 알림을 켜고 끈다.
 */

#define dev_fmt(fmt) "bwctrl: " fmt

#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/pci-bwctrl.h>
#include <linux/rwsem.h>
#include <linux/slab.h>
#include <linux/types.h>

#include "../pci.h"
#include "portdrv.h"

/**
 * struct pcie_bwctrl_data - PCIe bandwidth controller
 * @set_speed_mutex:	Serializes link speed changes
 * @cdev:		Thermal cooling device associated with the port
 */
struct pcie_bwctrl_data {
	struct mutex set_speed_mutex;
	struct thermal_cooling_device *cdev;
};

/* Prevent port removal during Link Speed changes. */
static DECLARE_RWSEM(pcie_bwctrl_setspeed_rwsem);

/*
 * pcie_valid_speed:
 *   주어진 PCIe bus speed가 유효한 범위 내에 있는지 검사한다.
 *   NVMe 장치가 지원하는 링크 속도가 올바른 값인지 확인할 때 사용된다.
 */
static bool pcie_valid_speed(enum pci_bus_speed speed)
{
	return (speed >= PCIE_SPEED_2_5GT) && (speed <= PCIE_SPEED_64_0GT);
}

/*
 * pci_bus_speed2lnkctl2:
 *   enum pci_bus_speed 값을 PCI Express Link Control 2 레지스터의
 *   Target Link Speed(TLS) 필드에 쓸 값으로 변환한다.
 *   NVMe 장치의 링크 속도 변경 시 Root Port 레지스터에 기록할 값을 만든다.
 */
static u16 pci_bus_speed2lnkctl2(enum pci_bus_speed speed)
{
	static const u8 speed_conv[] = {
		[PCIE_SPEED_2_5GT] = PCI_EXP_LNKCTL2_TLS_2_5GT,
		[PCIE_SPEED_5_0GT] = PCI_EXP_LNKCTL2_TLS_5_0GT,
		[PCIE_SPEED_8_0GT] = PCI_EXP_LNKCTL2_TLS_8_0GT,
		[PCIE_SPEED_16_0GT] = PCI_EXP_LNKCTL2_TLS_16_0GT,
		[PCIE_SPEED_32_0GT] = PCI_EXP_LNKCTL2_TLS_32_0GT,
		[PCIE_SPEED_64_0GT] = PCI_EXP_LNKCTL2_TLS_64_0GT,
	};

	if (WARN_ON_ONCE(!pcie_valid_speed(speed)))
		return 0;

	return speed_conv[speed];
}

/*
 * pcie_supported_speeds2target_speed:
 *   supported_speeds 비트맵에서 가장 높은 속도 비트를 추출해 target speed로 변환.
 *   NVMe 장치와 Root Port가 공통으로 지원하는 최고 속도를 선택할 때 사용.
 */
static inline u16 pcie_supported_speeds2target_speed(u8 supported_speeds)
{
	return __fls(supported_speeds);
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
	struct pci_bus *bus = port->subordinate;
	u8 desired_speeds, supported_speeds;
	struct pci_dev *dev;

	desired_speeds = GENMASK(pci_bus_speed2lnkctl2(speed_req),
				 __fls(PCI_EXP_LNKCAP2_SLS_2_5GB));

	supported_speeds = port->supported_speeds;
	if (bus) {
		down_read(&pci_bus_sem);
		dev = list_first_entry_or_null(&bus->devices, struct pci_dev, bus_list);
		if (dev)
			supported_speeds &= dev->supported_speeds;
		up_read(&pci_bus_sem);
	}
	if (!supported_speeds)
		supported_speeds = PCI_EXP_LNKCAP2_SLS_2_5GB;

	return pcie_supported_speeds2target_speed(supported_speeds & desired_speeds);
}

/*
 * pcie_bwctrl_change_speed:
 *   Root Port의 Link Control 2 레지스터 TLS 필드를 target_speed로 설정하고
 *   link retrain을 수행한다.
 *   NVMe 장치 아래 링크의 실제 속도를 변경하는 핵심 동작.
 */
static int pcie_bwctrl_change_speed(struct pci_dev *port, u16 target_speed, bool use_lt)
{
	int ret;

	ret = pcie_capability_clear_and_set_word(port, PCI_EXP_LNKCTL2,
						 PCI_EXP_LNKCTL2_TLS, target_speed);
	if (ret != PCIBIOS_SUCCESSFUL)
		return pcibios_err_to_errno(ret);

	return pcie_retrain_link(port, use_lt);
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
	struct pci_bus *bus = port->subordinate;
	u16 target_speed;
	int ret;

	if (WARN_ON_ONCE(!pcie_valid_speed(speed_req)))
		return -EINVAL;

	if (bus && bus->cur_bus_speed == speed_req)
		return 0;

	target_speed = pcie_bwctrl_select_speed(port, speed_req);

	scoped_guard(rwsem_read, &pcie_bwctrl_setspeed_rwsem) {
		struct pcie_bwctrl_data *data = port->link_bwctrl;

		/*
		 * port->link_bwctrl is NULL during initial scan when called
		 * e.g. from the Target Speed quirk.
		 */
		if (data)
			mutex_lock(&data->set_speed_mutex);

		ret = pcie_bwctrl_change_speed(port, target_speed, use_lt);

		if (data)
			mutex_unlock(&data->set_speed_mutex);
	}

	/*
	 * Despite setting higher speed into the Target Link Speed, empty
	 * bus won't train to 5GT+ speeds.
	 */
	if (!ret && bus && bus->cur_bus_speed != speed_req &&
	    !list_empty(&bus->devices))
		ret = -EAGAIN;

	return ret;
}

/*
 * pcie_bwnotif_enable:
 *   Bandwidth Notification 인터럽트를 활성화하고 현재 LBMS 상태를 기록한다.
 *   NVMe 장치의 링크 속도/폭 변화를 OS에 알릴 수 있도록 준비.
 */
static void pcie_bwnotif_enable(struct pcie_device *srv)
{
	struct pci_dev *port = srv->port;
	u16 link_status;
	int ret;

	/* Note if LBMS has been seen so far */
	ret = pcie_capability_read_word(port, PCI_EXP_LNKSTA, &link_status);
	if (ret == PCIBIOS_SUCCESSFUL && link_status & PCI_EXP_LNKSTA_LBMS)
		set_bit(PCI_LINK_LBMS_SEEN, &port->priv_flags);

	pcie_capability_set_word(port, PCI_EXP_LNKCTL,
				 PCI_EXP_LNKCTL_LBMIE | PCI_EXP_LNKCTL_LABIE);
	pcie_capability_write_word(port, PCI_EXP_LNKSTA,
				   PCI_EXP_LNKSTA_LBMS | PCI_EXP_LNKSTA_LABS);

	/*
	 * Update after enabling notifications & clearing status bits ensures
	 * link speed is up to date.
	 */
	pcie_update_link_speed(port->subordinate, PCIE_BWCTRL_ENABLE);
}

/*
 * pcie_bwnotif_disable:
 *   Bandwidth Notification 인터럽트를 비활성화한다.
 *   NVMe 장치 제거/suspend 등에서 링크 변화 알림을 멈출 때 사용.
 */
static void pcie_bwnotif_disable(struct pci_dev *port)
{
	pcie_capability_clear_word(port, PCI_EXP_LNKCTL,
				   PCI_EXP_LNKCTL_LBMIE | PCI_EXP_LNKCTL_LABIE);
}

/*
 * pcie_bwnotif_irq:
 *   Bandwidth Notification 인터럽트 핸들러.
 *   NVMe 장치의 PCIe 링크 속도가 하드웨어적으로 변경되었을 때 호출되어
 *   sysfs에 노출된 bus->cur_bus_speed를 갱신한다.
 */
static irqreturn_t pcie_bwnotif_irq(int irq, void *context)
{
	struct pcie_device *srv = context;
	struct pci_dev *port = srv->port;
	u16 link_status, events;
	int ret;

	ret = pcie_capability_read_word(port, PCI_EXP_LNKSTA, &link_status);
	if (ret != PCIBIOS_SUCCESSFUL)
		return IRQ_NONE;

	events = link_status & (PCI_EXP_LNKSTA_LBMS | PCI_EXP_LNKSTA_LABS);
	if (!events)
		return IRQ_NONE;

	if (events & PCI_EXP_LNKSTA_LBMS)
		set_bit(PCI_LINK_LBMS_SEEN, &port->priv_flags);

	pcie_capability_write_word(port, PCI_EXP_LNKSTA, events);

	/*
	 * Interrupts will not be triggered from any further Link Speed
	 * change until LBMS is cleared by the write. Therefore, re-read the
	 * speed (inside pcie_update_link_speed()) after LBMS has been
	 * cleared to avoid missing link speed changes.
	 */
	pcie_update_link_speed(port->subordinate, PCIE_BWCTRL_IRQ);

	return IRQ_HANDLED;
}

/*
 * pcie_reset_lbms:
 *   LBMS(Link Bandwidth Management Status) 플래그와 상태 비트를 리셋.
 *   NVMe 링크 속도 재협상 전후에 상태를 초기화할 때 사용.
 */
void pcie_reset_lbms(struct pci_dev *port)
{
	clear_bit(PCI_LINK_LBMS_SEEN, &port->priv_flags);
	pcie_capability_write_word(port, PCI_EXP_LNKSTA, PCI_EXP_LNKSTA_LBMS);
}

/*
 * pcie_bwnotif_probe:
 *   PCIe bandwidth notification 서비스 드라이버의 probe 함수.
 *   NVMe 장치가 연결된 포트에서 bwctrl 서비스를 등록하고 인터럽트를
 *   활성화하며, thermal cooling device를 등록한다.
 */
static int pcie_bwnotif_probe(struct pcie_device *srv)
{
	struct pci_dev *port = srv->port;
	int ret;

	if (port->no_bw_notif)
		return -ENODEV;

	/* Can happen if we run out of bus numbers during enumeration. */
	if (!port->subordinate)
		return -ENODEV;

	struct pcie_bwctrl_data *data = devm_kzalloc(&srv->device,
						     sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	ret = devm_mutex_init(&srv->device, &data->set_speed_mutex);
	if (ret)
		return ret;

	scoped_guard(rwsem_write, &pcie_bwctrl_setspeed_rwsem) {
		port->link_bwctrl = data;

		ret = request_irq(srv->irq, pcie_bwnotif_irq,
				  IRQF_SHARED, "PCIe bwctrl", srv);
		if (ret) {
			port->link_bwctrl = NULL;
			return ret;
		}

		pcie_bwnotif_enable(srv);
	}

	pci_dbg(port, "enabled with IRQ %d\n", srv->irq);

	/* Don't fail on errors. Don't leave IS_ERR() "pointer" into ->cdev */
	port->link_bwctrl->cdev = pcie_cooling_device_register(port);
	if (IS_ERR(port->link_bwctrl->cdev))
		port->link_bwctrl->cdev = NULL;

	return 0;
}

/*
 * pcie_bwnotif_remove:
 *   PCIe bandwidth notification 서비스 드라이버의 remove 함수.
 *   NVMe 장치가 제거되거나 서비스가 해제될 때 인터럽트와 cooling device를
 *   정리한다.
 */
static void pcie_bwnotif_remove(struct pcie_device *srv)
{
	struct pcie_bwctrl_data *data = srv->port->link_bwctrl;

	pcie_cooling_device_unregister(data->cdev);

	scoped_guard(rwsem_write, &pcie_bwctrl_setspeed_rwsem) {
		pcie_bwnotif_disable(srv->port);

		free_irq(srv->irq, srv);

		srv->port->link_bwctrl = NULL;
	}
}

/*
 * pcie_bwnotif_suspend:
 *   시스템 suspend 시 bandwidth notification 인터럽트를 비활성화.
 *   NVMe 장치가 있는 시스템의 절전 상태 진입 시 호출.
 */
static int pcie_bwnotif_suspend(struct pcie_device *srv)
{
	pcie_bwnotif_disable(srv->port);
	return 0;
}

/*
 * pcie_bwnotif_resume:
 *   시스템 resume 시 bandwidth notification 인터럽트를 다시 활성화.
 *   NVMe 장치가 있는 시스템이 절전에서 복귀할 때 호출.
 */
static int pcie_bwnotif_resume(struct pcie_device *srv)
{
	pcie_bwnotif_enable(srv);
	return 0;
}

/*
 * pcie_bwctrl_driver:
 *   PCIe port service driver 등록 구조체.
 *   NVMe 장치가 연결된 모든 PCIe 포트(PCIE_ANY_PORT)에서 bandwidth
 *   controller 서비스를 제공한다.
 */
static struct pcie_port_service_driver pcie_bwctrl_driver = {
	.name		= "pcie_bwctrl",
	.port_type	= PCIE_ANY_PORT,
	.service	= PCIE_PORT_SERVICE_BWCTRL,
	.probe		= pcie_bwnotif_probe,
	.suspend	= pcie_bwnotif_suspend,
	.resume		= pcie_bwnotif_resume,
	.remove		= pcie_bwnotif_remove,
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
	return pcie_port_service_register(&pcie_bwctrl_driver);
}
