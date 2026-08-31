// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Linaro Ltd.
 */

/*
 * [한국어 설명] 슬롯에 전원을 넣고 장치가 나타나기를 기다리는 인프라 (pwrctrl/core.c)
 *
 * === 파일의 역할 ===
 * 임베디드 보드에서는 PCIe 슬롯의 전원과 클럭이 자동으로 들어오지 않는다.
 * 전원 레귤레이터를 켜고, 클럭을 공급하고, 리셋 핀을 풀어 주는 순서를
 * 소프트웨어가 밟아야 한다. 그 순서를 담당하는 것이 pwrctrl 드라이버들이고,
 * 이 파일은 그들이 공유하는 인프라를 제공한다.
 *
 * 문제의 구조가 흥미롭다. PCI 열거는 "장치가 이미 거기 있다" 를 전제로
 * 하는데, 여기서는 전원을 넣기 전까지 장치가 존재하지 않는다. 그래서
 * 순서가 뒤집힌다 —
 *   1) DeviceTree 에 "이 슬롯에는 전원 제어가 필요하다" 고 적혀 있으면
 *      PCI 코어가 그 자리에 platform device 를 하나 만든다.
 *   2) 그 platform device 에 pwrctrl 드라이버가 바인딩되어 전원을 넣는다.
 *   3) 준비가 끝나면 pci_pwrctrl_device_set_ready() 로 알린다.
 *   4) 이 파일이 버스 재스캔을 걸어 그제서야 장치가 열거된다.
 *
 * 재스캔을 워크큐로 미루는 것도 이유가 있다. probe 문맥에서 곧바로
 * 재스캔하면 그 안에서 또 probe 가 불려 재귀가 되고, 드라이버 코어의
 * 잠금과 얽혀 교착할 수 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 열거 준비: of.c / probe.c 가 DT 를 보고
 *              -> [이 파일] pci_pwrctrl_create_device()
 *                 -> 그 자리에 platform device 생성
 *
 * 전원 인가: pwrctrl 드라이버(generic.c 등)가 그 device 에 바인딩
 *              -> 레귤레이터/클럭/리셋 제어
 *              -> [이 파일] pci_pwrctrl_device_set_ready()
 *                 -> 워크큐에 재스캔을 예약
 *                    -> pci_rescan_bus() -> 장치 발견 -> 드라이버 probe
 *
 * 제거:     [이 파일] pci_pwrctrl_device_unset_ready() / _cleanup()
 *
 * 실행 컨텍스트: 등록/해제는 프로세스 컨텍스트. 재스캔은 워크큐 스레드.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: probe.c 와 of.c 의 열거 경로, 그리고 각 pwrctrl 드라이버.
 * 아래쪽: 플랫폼 장치 인프라, 그리고 pci_rescan_bus().
 * 공유 상태: struct pci_pwrctrl — 재스캔 워크, notifier 블록,
 *   그리고 대상 장치를 담는다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 쓰지 않는다(전수 확인).
 *
 * 하지만 임베디드 보드에 NVMe 를 붙이는 경우 이 파일이 먼저 동작해야
 * NVMe 가 보인다. 전원과 클럭이 들어오고 리셋이 풀린 뒤에야 링크 훈련이
 * 시작되고, 그다음에 열거가 되어 nvme_probe() 가 불린다.
 *
 * 반대로 전원을 끄면 링크가 끊기고 장치가 사라져 nvme_remove() 로 간다.
 * 그 경로는 remove.c 의 일반적인 제거와 같다.
 *
 * (기존 주석은 이 파일이 "NVMe endpoint 가 탑재된 보드/슬롯의 전원 레일을
 *  켜고 끈다" 고 적었으나, 실제 전원 조작은 개별 pwrctrl 드라이버가 하고
 *  이 파일은 그들이 쓰는 인프라와 재스캔 트리거만 제공한다.)
 *
 * === 주요 함수/구조체 요약 ===
 * pci_pwrctrl_init()              : struct pci_pwrctrl 을 초기화한다.
 * pci_pwrctrl_device_set_ready()  : "전원이 들어왔다" 를 알리고 재스캔을 예약한다.
 * pci_pwrctrl_device_unset_ready(): 그 예약을 취소하고 정리한다.
 * devm_pci_pwrctrl_device_set_ready() : devres 판. 드라이버가 떨어질 때
 *                                   자동으로 unset 된다.
 * pci_pwrctrl_rescan()            : 워크큐가 실행하는 재스캔 본체.
 * pci_pwrctrl_notify()            : 장치 등록/해제 알림을 받아 처리한다.
 * struct pci_pwrctrl              : 이 인프라의 상태 묶음.
 */

#define dev_fmt(fmt) "pwrctrl: " fmt	/* NVMe: pwrctrl 로그 메시지 접두사 매크로 정의 */

#include <linux/device.h>     /* NVMe: device/driver core (struct device, drvdata 등) */
#include <linux/export.h>     /* NVMe: 심볼 난출용 EXPORT_SYMBOL_GPL */
#include <linux/kernel.h>     /* NVMe: 커널 기본 타입/매크로 */
#include <linux/of.h>         /* NVMe: Device Tree 노드 조작 (OF node, power supply) */
#include <linux/of_graph.h>   /* NVMe: DT graph/endpoint 순회 (remote port parent) */
#include <linux/of_platform.h>/* NVMe: of_platform_device_create()로 pwrctrl pdev 생성 */
#include <linux/pci.h>        /* NVMe: PCI bus 타입, pci_bus_type 등 */
#include <linux/pci-pwrctrl.h>/* NVMe: struct pci_pwrctrl, pwrctrl API 선언 */
#include <linux/platform_device.h> /* NVMe: platform_device 조작 */
#include <linux/property.h>   /* NVMe: device property 접근 */
#include <linux/slab.h>       /* NVMe: 메모리 할당 관련 */

#include "../pci.h"           /* NVMe: PCI 서브시스템 내部 헤더 */

/*
 * pci_pwrctrl_notify:
 *   PCI bus에 새 device(예: NVMe SSD의 pci_dev)가 추가될 때 호출되는
 *   notifier callback이다. pwrctrl platform device와 동일한 DT node를
 *   사용하는 PCI device가 나중에 추가되면, of_node_reused 플래그를 설정해
 *   pinctrl/clock 등이 두 번 bind되지 않도록 한다.
 */
static int pci_pwrctrl_notify(struct notifier_block *nb, unsigned long action,	/* NVMe: PCI bus notifier callback 정의: NVMe pci_dev 추가 시 중복 pinctrl 방지 */
			      void *data)	/* NVMe: notifier callback 매개변수 (추가/삭제된 PCI device) */
{	/* NVMe: pci_pwrctrl_notify() 함수 본문 시작: NVMe pci_dev 추가 시 중복 pinctrl 방지 처리 */
	struct pci_pwrctrl *pwrctrl = container_of(nb, struct pci_pwrctrl, nb); /* NVMe: notifier_block에서 pwrctrl 구조체 역참조 */
	struct device *dev = data;						/* NVMe: bus notify로 전달된 추가/삭제 대상 device (NVMe pci_dev 가능) */

	if (dev_fwnode(dev) != dev_fwnode(pwrctrl->dev))			/* NVMe: pwrctrl device와 같은 firmware node를 쓰는 device인지 확인 */
		return NOTIFY_DONE;						/* NVMe: 동일 node가 아니면 처리할 필요 없음 */

	switch (action) {							/* NVMe: bus notifier action 분기 */
	case BUS_NOTIFY_ADD_DEVICE:						/* NVMe: PCI device가 bus에 추가되는 시점 */
		/*
		 * We will have two struct device objects bound to two different
		 * drivers on different buses but consuming the same DT node. We
		 * must not bind the pins twice in this case but only once for
		 * the first device to be added.
		 *
		 * If we got here then the PCI device is the second after the
		 * power control platform device. Mark its OF node as reused.
		 */
		dev->of_node_reused = true;					/* NVMe: NVMe pci_dev 등의 OF node를 재사용으로 표시 -> pinctrl 중복 bind 방지 */
		break;								/* NVMe: case 종료 */
	}	/* NVMe: switch(action) 블록 종료 */

	return NOTIFY_DONE;							/* NVMe: notifier 처리 완료 반환 */
}	/* NVMe: pci_pwrctrl_notify() 함수 종료 */

/**
 * pci_pwrctrl_init() - Initialize the PCI power control context struct
 *
 * @pwrctrl: PCI power control data
 * @dev: Parent device
 */
/*
 * pci_pwrctrl_init:
 *   pwrctrl provider(platform driver)가 자신의 struct pci_pwrctrl 객체를
 *   초기화할 때 호출한다. parent device(예: host controller 아래 전원 제어
 *   장치)의 driver data로 pwrctrl을 등록해 NVMe 장치 탐색 전 전원 제어
 *   컨텍스트를 준비한다.
 */
void pci_pwrctrl_init(struct pci_pwrctrl *pwrctrl, struct device *dev)	/* NVMe: pwrctrl context 초기화 함수: parent device에 pwrctrl 저장 */
{	/* NVMe: pci_pwrctrl_init() 함수 본문 시작: pwrctrl context 초기화 */
	pwrctrl->dev = dev;							/* NVMe: pwrctrl 구조체가 관리하는 device 기록 */
	dev_set_drvdata(dev, pwrctrl);						/* NVMe: parent device의 driver data에 pwrctrl 저장 -> 후속 power_on/off 시 꺼냄 */
}	/* NVMe: pci_pwrctrl_init() 함수 종료 */
EXPORT_SYMBOL_GPL(pci_pwrctrl_init);	/* NVMe: pci_pwrctrl_init 심볼 외부 노출 */

/**
 * pci_pwrctrl_device_set_ready() - Notify the pwrctrl subsystem that the PCI
 * device is powered-up and ready to be detected.
 *
 * @pwrctrl: PCI power control data.
 *
 * Returns:
 * 0 on success, negative error number on error.
 *
 * Note:
 * This function returning 0 doesn't mean the device was detected. It means,
 * that the bus rescan was successfully started. The device will get bound to
 * its PCI driver asynchronously.
 */
/*
 * pci_pwrctrl_device_set_ready:
 *   전원이 켜진 PCI device(예: NVMe SSD)가 PCI bus에서 detect될 수 있도록
 *   bus notifier를 등록한다. 이후 bus rescan이 비동기적으로 이뤄지고,
 *   NVMe 장치가 발견되면 drivers/nvme/host/pci.c의 nvme_probe()가 호출되어
 *   BAR, MSI-X, queue, doorbell 등을 초기화한다.
 */
int pci_pwrctrl_device_set_ready(struct pci_pwrctrl *pwrctrl)	/* NVMe: NVMe 탐색 전 PCI bus notifier 등록 함수 */
{	/* NVMe: pci_pwrctrl_device_set_ready() 함수 본문 시작: NVMe 탐색을 위한 bus notifier 등록 */
	int ret;								/* NVMe: bus_register_notifier 반환값 저장 */

	if (!pwrctrl->dev)							/* NVMe: pwrctrl이 연결된 parent device가 없으면 */
		return -ENODEV;							/* NVMe: 장치 없음 오류 반환 -> NVMe 탐색 불가 */

	pwrctrl->nb.notifier_call = pci_pwrctrl_notify;				/* NVMe: bus notifier callback 설정 (NVMe pci_dev 추가 감지) */
	ret = bus_register_notifier(&pci_bus_type, &pwrctrl->nb);		/* NVMe: PCI bus에 notifier 등록 -> NVMe 등 PCI device 추가 이벤트 수신 */
	if (ret)								/* NVMe: notifier 등록 실패 시 */
		return ret;							/* NVMe: 오류 코드 반환, NVMe 탐색 시작 전 실패 */

	return 0;								/* NVMe: 성공 -> 이후 bus rescan으로 NVMe detect 가능 */
}	/* NVMe: pci_pwrctrl_device_set_ready() 함수 종료 */
EXPORT_SYMBOL_GPL(pci_pwrctrl_device_set_ready);	/* NVMe: pci_pwrctrl_device_set_ready 심볼 외부 노출 */

/**
 * pci_pwrctrl_device_unset_ready() - Notify the pwrctrl subsystem that the PCI
 * device is about to be powered-down.
 *
 * @pwrctrl: PCI power control data.
 */
/*
 * pci_pwrctrl_device_unset_ready:
 *   NVMe 장치 등 PCI device가 power-down되기 직전에 notifier를 해제한다.
 *   일반적으로 pwrctrl platform driver가 detach될 때 호출되며, 이 시점에서는
 *   이미 NVMe pci_dev가 unbound된 상태여야 한다.
 */
void pci_pwrctrl_device_unset_ready(struct pci_pwrctrl *pwrctrl)	/* NVMe: NVMe power-down 직전 PCI bus notifier 해제 함수 */
{	/* NVMe: pci_pwrctrl_device_unset_ready() 함수 본문 시작: NVMe power-down 직전 notifier 해제 */
	/*
	 * We don't have to delete the link here. Typically, this function
	 * is only called when the power control device is being detached. If
	 * it is being detached then the child PCI device must have already
	 * been unbound too or the device core wouldn't let us unbind.
	 */
	bus_unregister_notifier(&pci_bus_type, &pwrctrl->nb);			/* NVMe: PCI bus notifier 해제 -> NVMe device 추가 이벤트 더 이상 처리 안 함 */
}	/* NVMe: pci_pwrctrl_device_unset_ready() 함수 종료 */
EXPORT_SYMBOL_GPL(pci_pwrctrl_device_unset_ready);	/* NVMe: pci_pwrctrl_device_unset_ready 심볼 외부 노출 */

/*
 * devm_pci_pwrctrl_device_unset_ready:
 *   devm_add_action_or_reset()에 의해 등록된 release action callback이다.
 *   관리 device가 release될 때 자동으로 PCI bus notifier를 해제한다.
 */
static void devm_pci_pwrctrl_device_unset_ready(void *data)	/* NVMe: devm release action: pwrctrl notifier 자동 해제 */
{	/* NVMe: devm_pci_pwrctrl_device_unset_ready() release action 본문 시작 */
	struct pci_pwrctrl *pwrctrl = data;					/* NVMe: release action에 저장된 pwrctrl 객체 */

	pci_pwrctrl_device_unset_ready(pwrctrl);				/* NVMe: 관리 객체 해제 시 notifier 자동 제거 */
}	/* NVMe: devm_pci_pwrctrl_device_unset_ready() release action 종료 */

/**
 * devm_pci_pwrctrl_device_set_ready - Managed variant of
 * pci_pwrctrl_device_set_ready().
 *
 * @dev: Device managing this pwrctrl provider.
 * @pwrctrl: PCI power control data.
 *
 * Returns:
 * 0 on success, negative error number on error.
 */
/*
 * devm_pci_pwrctrl_device_set_ready:
 *   pci_pwrctrl_device_set_ready()의 managed 버전으로, 관리 device가
 *   release되면 자동으로 notifier를 해제한다. NVMe 장치가 hot-unplug되거나
 *   driver가 unload될 때 누수 없이 정리되도록 한다.
 */
int devm_pci_pwrctrl_device_set_ready(struct device *dev,	/* NVMe: managed notifier 등록 함수: NVMe hot-unplug 시 자동 정리 */
				      struct pci_pwrctrl *pwrctrl)	/* NVMe: 관리 대상 pwrctrl 객체 */
{	/* NVMe: devm_pci_pwrctrl_device_set_ready() 함수 본문 시작: managed notifier 등록 */
	int ret;								/* NVMe: 등록 결과 저장 */

	ret = pci_pwrctrl_device_set_ready(pwrctrl);				/* NVMe: 일반 버전으로 bus notifier 등록 (NVMe 탐색 준비) */
	if (ret)								/* NVMe: 등록 실패 시 */
		return ret;							/* NVMe: 즉시 오류 반환 */

	return devm_add_action_or_reset(dev,					/* NVMe: 관리 device에 release action 등록 */
					devm_pci_pwrctrl_device_unset_ready,	/* NVMe: release action callback: pwrctrl notifier 자동 해제 */
					pwrctrl);					/* NVMe: release 시 pwrctrl notifier 자동 해제 */
}	/* NVMe: devm_pci_pwrctrl_device_set_ready() 함수 종료 */
EXPORT_SYMBOL_GPL(devm_pci_pwrctrl_device_set_ready);	/* NVMe: devm_pci_pwrctrl_device_set_ready 심볼 외부 노출 */

/*
 * __pci_pwrctrl_power_off_device:
 *   이미 찾은 platform device의 driver data에서 pwrctrl 객체를 꺼내
 *   power_off callback을 호출한다. NVMe slot의 전원 레일을 끄는 핵심 동작이다.
 */
static int __pci_pwrctrl_power_off_device(struct device *dev)	/* NVMe: platform device에서 pwrctrl 꺼내 전원 끄는 헬퍼 */
{	/* NVMe: __pci_pwrctrl_power_off_device() 함수 본문 시작: pdev에서 pwrctl 꺼내 전원 off */
	struct pci_pwrctrl *pwrctrl = dev_get_drvdata(dev);			/* NVMe: platform device에서 pwrctrl context 획득 */

	if (!pwrctrl)								/* NVMe: driver data가 없으면 이미 off이거나 초기화 안 됨 */
		return 0;							/* NVMe: 정상 종료로 처리 */

	return pwrctrl->power_off(pwrctrl);					/* NVMe: pwrctrl 드라이버의 power_off() 호출 -> NVMe 장치 전원 차단 */
}	/* NVMe: __pci_pwrctrl_power_off_device() 함수 종료 */

/*
 * pci_pwrctrl_power_off_device:
 *   재귀적으로 DT 하위 노드를 따라 낮은 depth부터 power_off를 수행한다.
 *   leaf node에 해당하는 platform device가 bound되어 있으면 전원을 끈다.
 *   NVMe SSD가 탑재된 slot의 전원이 역순으로 차단되는 것을 보장한다.
 */
static void pci_pwrctrl_power_off_device(struct device_node *np)	/* NVMe: DT 노드별 pwrctrl device 전원 off (깊이 우선) */
{	/* NVMe: pci_pwrctrl_power_off_device() 함수 본문 시작: DT 노드별 깊이 우선 전원 off */
	struct platform_device *pdev;						/* NVMe: OF node에 해당하는 platform device */
	int ret;								/* NVMe: power_off 반환값 저장 */

	for_each_available_child_of_node_scoped(np, child)	/* NVMe: 자식 노드부터 재귀적으로 전원 off 순회 */
		pci_pwrctrl_power_off_device(child);				/* NVMe: 자식 노드부터 깊이 우선으로 전원 끄기 -> NVMe 장치보다 상위 regulator부터 순차 off */

	pdev = of_find_device_by_node(np);					/* NVMe: 현재 OF node에 등록된 platform device 검색 */
	if (!pdev)								/* NVMe: 해당 node에 pdev가 없으면 */
		return;								/* NVMe: 아무 것도 하지 않고 리턴 */

	if (device_is_bound(&pdev->dev)) {					/* NVMe: pwrctrl driver가 bind된 상태인지 확인 */
		ret = __pci_pwrctrl_power_off_device(&pdev->dev);		/* NVMe: bind된 경우 실제 power_off 수행 (NVMe slot off) */
		if (ret)							/* NVMe: power_off 실패 시 */
			dev_err(&pdev->dev, "Failed to power off device: %d", ret); /* NVMe: dmesg에 NVMe 관련 전원 off 실패 기록 */
	}	/* NVMe: if (device_is_bound) 블록 종료 */

	platform_device_put(pdev);						/* NVMe: of_find_device_by_node()로 얻은 참조 카운트 감소 */
}	/* NVMe: pci_pwrctrl_power_off_device() 함수 종료 */

/**
 * pci_pwrctrl_power_off_devices - Power off pwrctrl devices
 *
 * @parent: PCI host controller device
 *
 * Recursively traverse all pwrctrl devices for the devicetree hierarchy
 * below the specified PCI host controller and power them off in a depth
 * first manner.
 */
/*
 * pci_pwrctrl_power_off_devices:
 *   PCI host controller(예: NVMe Root Complex) 아래의 모든 pwrctrl device를
 *   깊이 우선으로 끈다. NVMe endpoint가 연결된 slot의 안전한 전원 차단에
 *   사용된다.
 */
void pci_pwrctrl_power_off_devices(struct device *parent)	/* NVMe: host controller 아래 모든 NVMe slot 전원 off */
{	/* NVMe: pci_pwrctrl_power_off_devices() 함수 본문 시작: host controller 아래 모든 NVMe slot off */
	struct device_node *np = parent->of_node;				/* NVMe: host controller의 OF node 획득 -> NVMe 트리 루트 */

	for_each_available_child_of_node_scoped(np, child)	/* NVMe: host controller 직계 자식 pwrctrl device 순회 */
		pci_pwrctrl_power_off_device(child);				/* NVMe: host controller 바로 아래 자식 pwrctrl device부터 전원 off */
}	/* NVMe: pci_pwrctrl_power_off_devices() 함수 종료 */
EXPORT_SYMBOL_GPL(pci_pwrctrl_power_off_devices);	/* NVMe: pci_pwrctrl_power_off_devices 심볼 외부 노출 */

/*
 * __pci_pwrctrl_power_on_device:
 *   platform device의 driver data에서 pwrctrl 객체를 꺼내 power_on callback을
 *   호출한다. NVMe endpoint가 detect되기 전 필요한 전원 레일을 켜는 동작이다.
 */
static int __pci_pwrctrl_power_on_device(struct device *dev)	/* NVMe: platform device에서 pwrctrl 꺼내 전원 켜는 헬퍼 */
{	/* NVMe: __pci_pwrctrl_power_on_device() 함수 본문 시작: pdev에서 pwrctrl 꺼내 전원 on */
	struct pci_pwrctrl *pwrctrl = dev_get_drvdata(dev);			/* NVMe: pdev에서 pwrctrl context 획득 */

	if (!pwrctrl)								/* NVMe: pwrctrl context가 없으면 */
		return 0;							/* NVMe: 이미 준비된 것으로 보고 정상 반환 */

	return pwrctrl->power_on(pwrctrl);					/* NVMe: pwrctrl 드라이버의 power_on() 호출 -> NVMe 장치 전원 공급 */
}	/* NVMe: __pci_pwrctrl_power_on_device() 함수 종료 */

/*
 * Power on the devices in a depth first manner. Before powering on the device,
 * make sure its driver is bound.
 */
/*
 * pci_pwrctrl_power_on_device:
 *   DT 하위 노드를 재귀적으로 따라가며 깊이 우선으로 power_on을 시도한다.
 *   platform device가 pwrctrl driver에 bind되어 있어야 실제로 전원을 켠다.
 *   driver가 아직 bind되지 않았으면 -EPROBE_DEFER를 반환해 NVMe probe를
 *   뒤로 미룬다.
 */
static int pci_pwrctrl_power_on_device(struct device_node *np)	/* NVMe: DT 노드별 pwrctrl device 전원 on (깊이 우선) */
{	/* NVMe: pci_pwrctrl_power_on_device() 함수 본문 시작: DT 노드별 깊이 우선 전원 on */
	struct platform_device *pdev;						/* NVMe: 현재 OF node의 platform device */
	int ret;								/* NVMe: 각 단계 결과 저장 */

	for_each_available_child_of_node_scoped(np, child) {			/* NVMe: 자식 노드부터 전원을 켜기 위해 순회 */
		ret = pci_pwrctrl_power_on_device(child);			/* NVMe: 자식 pwrctrl device 전원 켜기 재귀 호출 */
		if (ret)							/* NVMe: 자식에서 실패하면 */
			return ret;						/* NVMe: 상위로 오류 전파 -> NVMe 탐색 중단 */
	}	/* NVMe: 자식 노드 순회 for 루프 종료 */

	pdev = of_find_device_by_node(np);					/* NVMe: 현재 node에 등록된 platform device 검색 */
	if (!pdev)								/* NVMe: pdev가 없으면 */
		return 0;							/* NVMe: 전원 제어할 장치가 없는 것으로 정상 처리 */

	if (device_is_bound(&pdev->dev)) {					/* NVMe: pwrctrl driver가 platform device에 bind됐는지 확인 */
		ret = __pci_pwrctrl_power_on_device(&pdev->dev);		/* NVMe: bind된 경우 NVMe slot 전원 켜기 */
	} else {	/* NVMe: pwrctrl driver가 아직 bind되지 않은 경우: probe defer 처리 */
		/* FIXME: Use blocking wait instead of probe deferral */
		dev_dbg(&pdev->dev, "driver is not bound\n");			/* NVMe: driver 미bind 상태 디버그 출력 */
		ret = -EPROBE_DEFER;						/* NVMe: probe defer -> NVMe 초기화를 나중에 재시도 */
	}	/* NVMe: if-else (device_is_bound) 블록 종료 */

	platform_device_put(pdev);						/* NVMe: pdev 참조 카운트 감소 */

	return ret;								/* NVMe: 성공/EPROBE_DEFER/오류 반환 */
}	/* NVMe: pci_pwrctrl_power_on_device() 함수 종료 */

/**
 * pci_pwrctrl_power_on_devices - Power on pwrctrl devices
 *
 * @parent: PCI host controller device
 *
 * Recursively traverse all pwrctrl devices for the devicetree hierarchy
 * below the specified PCI host controller and power them on in a depth
 * first manner. On error, all powered on devices will be powered off.
 *
 * Return: 0 on success, -EPROBE_DEFER if any pwrctrl driver is not bound, an
 * appropriate error code otherwise.
 */
/*
 * pci_pwrctrl_power_on_devices:
 *   PCI host controller 아래 모든 pwrctrl device를 깊이 우선으로 켠다.
 *   중간에 실패하면 그전에 켠 device를 모두 끄므로 NVMe 장치의 부분 전원
 *   공급 상태로 남는 것을 방지한다.
 */
int pci_pwrctrl_power_on_devices(struct device *parent)	/* NVMe: host controller 아래 모든 NVMe slot 전원 on */
{	/* NVMe: pci_pwrctrl_power_on_devices() 함수 본문 시작: host controller 아래 모든 NVMe slot on */
	struct device_node *np = parent->of_node;				/* NVMe: host controller의 OF node */
	struct device_node *child = NULL;					/* NVMe: 순회 중인 자식 노드 포인터 */
	int ret;								/* NVMe: power_on_device 결과 저장 */

	for_each_available_child_of_node(np, child) {				/* NVMe: host controller 아래 사용 가능한 자식 노드 순회 */
		ret = pci_pwrctrl_power_on_device(child);			/* NVMe: 각 자식 pwrctrl device 전원 켜기 */
		if (ret)							/* NVMe: 하나라도 실패하면 */
			goto err_power_off;					/* NVMe: 이미 켠 device 정리로 이동 */
	}	/* NVMe: 성공 시 자식 노드 순회 for 루프 종료 */

	return 0;								/* NVMe: 모든 pwrctrl device 전원 켜기 성공 -> NVMe 탐색 진행 가능 */

err_power_off:	/* NVMe: 전원 on 실패 시 이미 켠 device를 끄는 레이블 */
	for_each_available_child_of_node_scoped(np, tmp) {			/* NVMe: 실패 지점까지 켰던 device를 다시 순회 */
		if (tmp == child)						/* NVMe: 실패한 노드에 도달하면 */
			break;							/* NVMe: 그 이후는 아직 켜지 않았으므로 중단 */
		pci_pwrctrl_power_off_device(tmp);				/* NVMe: 켰던 pwrctrl device를 다시 끔 -> NVMe slot 완전 off */
	}	/* NVMe: 실패 시 정리용 for 루프 종료 */
	of_node_put(child);							/* NVMe: for_each_... 반복자가 참조한 child node의 카운트 감소 */

	return ret;								/* NVMe: 오류 코드 반환 -> NVMe probe 지연/실패 */
}	/* NVMe: pci_pwrctrl_power_on_devices() 함수 종료 */
EXPORT_SYMBOL_GPL(pci_pwrctrl_power_on_devices);	/* NVMe: pci_pwrctrl_power_on_devices 심볼 외부 노출 */

/*
 * Check whether the pwrctrl device really needs to be created or not. The
 * pwrctrl device will only be created if the node satisfies below requirements:
 *
 * 1. Presence of compatible property with "pci" prefix to match against the
 *    pwrctrl driver (AND)
 * 2. At least one of the power supplies defined in the devicetree node of the
 *    device (OR) in the remote endpoint parent node to indicate pwrctrl
 *    requirement.
 */
/*
 * pci_pwrctrl_is_required:
 *   주어진 OF node가 NVMe/PCI endpoint를 위한 pwrctrl device를 필요로 하는지
 *   판단한다. compatible에 "pci" prefix가 있고, supply가 정의된 경우에만
 *   true를 반환한다.
 */
static bool pci_pwrctrl_is_required(struct device_node *np)	/* NVMe: NVMe/PCI endpoint용 pwrctrl device 필요 여부 판단 */
{	/* NVMe: pci_pwrctrl_is_required() 함수 본문 시작: NVMe/PCI endpoint용 pwrctrl 필요 여부 판단 */
	struct device_node *endpoint;						/* NVMe: DT graph endpoint 순회용 */
	const char *compat;							/* NVMe: compatible 문자열 */
	int ret;								/* NVMe: of_property_read_string 반환값 */

	ret = of_property_read_string(np, "compatible", &compat);		/* NVMe: node의 compatible property 읽기 */
	if (ret < 0)								/* NVMe: compatible이 없으면 */
		return false;							/* NVMe: pwrctrl device 불필요 */

	if (!strstarts(compat, "pci"))						/* NVMe: compatible이 "pci"로 시작하지 않으면 */
		return false;							/* NVMe: PCI/NVMe 관련 node가 아니므로 건 넘김 */

	if (of_pci_supply_present(np))						/* NVMe: 현재 node에 PCI power supply가 정의됐는지 확인 */
		return true;							/* NVMe: supply가 있으면 pwrctrl device 필요 */

	if (of_graph_is_present(np)) {						/* NVMe: DT graph(endpoint)가 있으면 remote 쪽도 검사 */
		for_each_endpoint_of_node(np, endpoint) {			/* NVMe: node의 모든 endpoint 순회 */
			struct device_node *remote __free(device_node) =	/* NVMe: endpoint 연결 remote node (자동 해제) */
				of_graph_get_remote_port_parent(endpoint);	/* NVMe: endpoint가 연결된 remote port parent 획득 (예: NVMe 컨트롤러 측 node) */
			if (remote) {						/* NVMe: remote node가 존재하면 */
				if (of_pci_supply_present(remote)) {		/* NVMe: remote node에 PCI power supply가 있는지 확인 */
					of_node_put(endpoint);			/* NVMe: endpoint 참조 카운트 감소 */
					return true;				/* NVMe: remote 쪽에 supply가 있으면 pwrctrl 필요 */
				}	/* NVMe: if (of_pci_supply_present(remote)) 블록 종료 */
			}	/* NVMe: if (remote) 블록 종료 */
		}	/* NVMe: for_each_endpoint_of_node 루프 종료 */
	}	/* NVMe: if (of_graph_is_present) 블록 종료 */

	return false;								/* NVMe: 어디에도 supply가 없으면 pwrctrl device 생성 안 함 */
}	/* NVMe: pci_pwrctrl_is_required() 함수 종료 */

/*
 * pci_pwrctrl_create_device:
 *   DT node를 재귀적으로 순회하며 pwrctrl platform device를 생성한다.
 *   이미 pdev가 있거나 pci_pwrctrl_is_required()가 false면 생성을 건 넘긴다.
 *   NVMe SSD가 연결될 slot의 전원 제어 장치를 준비하는 단계이다.
 */
static int pci_pwrctrl_create_device(struct device_node *np,	/* NVMe: DT 노드에 pwrctrl platform device 생성 (재귀) */
				     struct device *parent)	/* NVMe: platform device의 parent device (host controller) */
{	/* NVMe: pci_pwrctrl_create_device() 함수 본문 시작: DT 노드에 pwrctrl pdev 생성 */
	struct platform_device *pdev;						/* NVMe: 생성 대상/기존 platform device */
	int ret;								/* NVMe: 재귀 호출 결과 저장 */

	for_each_available_child_of_node_scoped(np, child) {			/* NVMe: 자식 노드부터 pwrctrl device 생성을 위해 순회 */
		ret = pci_pwrctrl_create_device(child, parent);			/* NVMe: 자식 node에서 pwrctrl pdev 생성 재귀 호출 */
		if (ret)							/* NVMe: 자식 생성 실패 시 */
			return ret;						/* NVMe: 상위로 오류 전파 -> NVMe 트리 준비 중단 */
	}	/* NVMe: 자식 노드 순회 for 루프 종료 */

	/* Bail out if the platform device is already available for the node */
	pdev = of_find_device_by_node(np);					/* NVMe: node에 이미 platform device가 있는지 확인 */
	if (pdev) {								/* NVMe: 이미 pdev가 존재하면 */
		platform_device_put(pdev);					/* NVMe: 참조 카운트 감소 */
		return 0;							/* NVMe: 중복 생성 방지 */
	}	/* NVMe: if (pdev) 블록 종료 (중복 생성 방지) */

	if (!pci_pwrctrl_is_required(np)) {					/* NVMe: 이 node가 pwrctrl을 필요로 하는지 재확인 */
		dev_dbg(parent, "Skipping OF node: %s\n", np->name);		/* NVMe: 필요 없는 node는 디버그 로그로 남기고 건 넘김 */
		return 0;							/* NVMe: pwrctrl device 생성 생략 */
	}	/* NVMe: if (!pci_pwrctrl_is_required) 블록 종료 */

	/* Now create the pwrctrl device */
	pdev = of_platform_device_create(np, NULL, parent);			/* NVMe: OF node에 platform device 생성 -> NVMe slot 전원 제어 드라이버 바인딩 대상 마련 */
	if (!pdev) {								/* NVMe: platform device 생성 실패 시 */
		dev_err(parent, "Failed to create pwrctrl device for node: %s\n", np->name); /* NVMe: NVMe 관련 전원 제어 장치 생성 실패 기록 */
		return -EINVAL;							/* NVMe: 잘못된 인자/생성 실패 오류 반환 */
	}	/* NVMe: if (!pdev) 블록 종료 */

	return 0;								/* NVMe: pwrctrl device 생성 성공 */
}	/* NVMe: pci_pwrctrl_create_device() 함수 종료 */

/**
 * pci_pwrctrl_create_devices - Create pwrctrl devices
 *
 * @parent: PCI host controller device
 *
 * Recursively create pwrctrl devices for the devicetree hierarchy below
 * the specified PCI host controller in a depth first manner. On error, all
 * created devices will be destroyed.
 *
 * Return: 0 on success, negative error number on error.
 */
/*
 * pci_pwrctrl_create_devices:
 *   PCI host controller 아래의 DT hierarchy에서 pwrctrl platform device를
 *   깊이 우선으로 생성한다. 생성 중 오류가 발생하면 이미 만든 device를
 *   모두 제거한다. NVMe SSD가 detect되기 전 전원 제어 장치를 먼저 세팅한다.
 */
int pci_pwrctrl_create_devices(struct device *parent)	/* NVMe: host controller 아래 모든 pwrctrl device 생성 */
{	/* NVMe: pci_pwrctrl_create_devices() 함수 본문 시작: host controller 아래 pwrctrl pdev 생성 */
	int ret;								/* NVMe: create_device 결과 저장 */

	for_each_available_child_of_node_scoped(parent->of_node, child) {	/* NVMe: host controller 아래 사용 가능한 모든 자식 node 순회 */
		ret = pci_pwrctrl_create_device(child, parent);			/* NVMe: 각 node에 대해 pwrctrl platform device 생성 */
		if (ret) {							/* NVMe: 생성 실패 시 */
			pci_pwrctrl_destroy_devices(parent);			/* NVMe: 이미 생성된 pwrctrl device를 모두 제거 -> NVMe 트리 정리 */
			return ret;						/* NVMe: 오류 반환 */
		}	/* NVMe: if (ret) 블록 종료 (생성 실패 시 정리) */
	}	/* NVMe: for_each_available_child_of_node_scoped 루프 종료 */

	return 0;								/* NVMe: 모든 pwrctrl device 생성 완료 -> 이후 power_on 및 NVMe 탐색 */
}	/* NVMe: pci_pwrctrl_create_devices() 함수 종료 */
EXPORT_SYMBOL_GPL(pci_pwrctrl_create_devices);	/* NVMe: pci_pwrctrl_create_devices 심볼 외부 노출 */

/*
 * pci_pwrctrl_destroy_device:
 *   재귀적으로 pwrctrl platform device를 OF node에서 제거한다.
 *   of_node_clear_flag()로 OF_POPULATED 플래그를 지워 추후 다시 create할 수
 *   있게 한다. NVMe 장치 제거 시 전원 제어 장치를 정리한다.
 */
static void pci_pwrctrl_destroy_device(struct device_node *np)	/* NVMe: DT 노드에서 pwrctrl platform device 제거 (재귀) */
{	/* NVMe: pci_pwrctrl_destroy_device() 함수 본문 시작: DT 노드에서 pwrctrl pdev 제거 */
	struct platform_device *pdev;						/* NVMe: 제거 대상 platform device */

	for_each_available_child_of_node_scoped(np, child)	/* NVMe: 자식 노드부터 pwrctrl device 제거 순회 */
		pci_pwrctrl_destroy_device(child);				/* NVMe: 자식 node부터 pwrctrl device 제거 (깊이 우선) */

	pdev = of_find_device_by_node(np);					/* NVMe: 현재 node에 등록된 platform device 검색 */
	if (!pdev)								/* NVMe: pdev가 없으면 */
		return;								/* NVMe: 제거할 것이 없음 */

	of_device_unregister(pdev);						/* NVMe: platform device를 OF tree에서 등록 해제 */
	platform_device_put(pdev);						/* NVMe: pdev 참조 카운트 감소 */

	of_node_clear_flag(np, OF_POPULATED);					/* NVMe: OF_POPULATED 플래그 클리어 -> 추후 NVMe 트리 재초기화 가능 */
}	/* NVMe: pci_pwrctrl_destroy_device() 함수 종료 */

/**
 * pci_pwrctrl_destroy_devices - Destroy pwrctrl devices
 *
 * @parent: PCI host controller device
 *
 * Recursively destroy pwrctrl devices for the devicetree hierarchy below
 * the specified PCI host controller in a depth first manner.
 */
/*
 * pci_pwrctrl_destroy_devices:
 *   PCI host controller 아래의 모든 pwrctrl device를 깊이 우선으로 제거한다.
 *   NVMe 장치가 제거된 후 호출되어 전원 제어 리소스를 깨끗이 정리한다.
 */
void pci_pwrctrl_destroy_devices(struct device *parent)	/* NVMe: host controller 아래 모든 pwrctrl device 제거 */
{	/* NVMe: pci_pwrctrl_destroy_devices() 함수 본문 시작: host controller 아래 pwrctrl pdev 제거 */
	struct device_node *np = parent->of_node;				/* NVMe: host controller의 OF node */

	for_each_available_child_of_node_scoped(np, child)	/* NVMe: host controller 직계 자식 pwrctrl device 제거 순회 */
		pci_pwrctrl_destroy_device(child);				/* NVMe: 자식 pwrctrl device부터 제거 -> NVMe slot 전원 제어 정리 */
}	/* NVMe: pci_pwrctrl_destroy_devices() 함수 종료 */
EXPORT_SYMBOL_GPL(pci_pwrctrl_destroy_devices);	/* NVMe: pci_pwrctrl_destroy_devices 심볼 외부 노출 */

MODULE_AUTHOR("Bartosz Golaszewski <bartosz.golaszewski@linaro.org>");	/* NVMe: 모듈 저자 정보 */
MODULE_DESCRIPTION("PCI Device Power Control core driver");	/* NVMe: 모듈 설명: PCI 전원 제어 코어 */
MODULE_LICENSE("GPL");	/* NVMe: 모듈 라이선스 (GPL) */
