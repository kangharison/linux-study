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

#define dev_fmt(fmt) "pwrctrl: " fmt

#include <linux/device.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/of_platform.h>
#include <linux/pci.h>
#include <linux/pci-pwrctrl.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/slab.h>

#include "../pci.h"

static int pci_pwrctrl_notify(struct notifier_block *nb, unsigned long action,
			      void *data)
{
	struct pci_pwrctrl *pwrctrl = container_of(nb, struct pci_pwrctrl, nb);
	struct device *dev = data;

	if (dev_fwnode(dev) != dev_fwnode(pwrctrl->dev))
		return NOTIFY_DONE;

	switch (action) {
	case BUS_NOTIFY_ADD_DEVICE:
		/*
		 * We will have two struct device objects bound to two different
		 * drivers on different buses but consuming the same DT node. We
		 * must not bind the pins twice in this case but only once for
		 * the first device to be added.
		 *
		 * If we got here then the PCI device is the second after the
		 * power control platform device. Mark its OF node as reused.
		 */
		dev->of_node_reused = true;
		break;
	}

	return NOTIFY_DONE;
}

/**
 * pci_pwrctrl_init() - Initialize the PCI power control context struct
 *
 * @pwrctrl: PCI power control data
 * @dev: Parent device
 */
void pci_pwrctrl_init(struct pci_pwrctrl *pwrctrl, struct device *dev)
{
	pwrctrl->dev = dev;
	dev_set_drvdata(dev, pwrctrl);
}
EXPORT_SYMBOL_GPL(pci_pwrctrl_init);

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
int pci_pwrctrl_device_set_ready(struct pci_pwrctrl *pwrctrl)
{
	int ret;

	if (!pwrctrl->dev)
		return -ENODEV;

	pwrctrl->nb.notifier_call = pci_pwrctrl_notify;
	ret = bus_register_notifier(&pci_bus_type, &pwrctrl->nb);
	if (ret)
		return ret;

	return 0;
}
EXPORT_SYMBOL_GPL(pci_pwrctrl_device_set_ready);

/**
 * pci_pwrctrl_device_unset_ready() - Notify the pwrctrl subsystem that the PCI
 * device is about to be powered-down.
 *
 * @pwrctrl: PCI power control data.
 */
void pci_pwrctrl_device_unset_ready(struct pci_pwrctrl *pwrctrl)
{
	/*
	 * We don't have to delete the link here. Typically, this function
	 * is only called when the power control device is being detached. If
	 * it is being detached then the child PCI device must have already
	 * been unbound too or the device core wouldn't let us unbind.
	 */
	bus_unregister_notifier(&pci_bus_type, &pwrctrl->nb);
}
EXPORT_SYMBOL_GPL(pci_pwrctrl_device_unset_ready);

static void devm_pci_pwrctrl_device_unset_ready(void *data)
{
	struct pci_pwrctrl *pwrctrl = data;

	pci_pwrctrl_device_unset_ready(pwrctrl);
}

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
int devm_pci_pwrctrl_device_set_ready(struct device *dev,
				      struct pci_pwrctrl *pwrctrl)
{
	int ret;

	ret = pci_pwrctrl_device_set_ready(pwrctrl);
	if (ret)
		return ret;

	return devm_add_action_or_reset(dev,
					devm_pci_pwrctrl_device_unset_ready,
					pwrctrl);
}
EXPORT_SYMBOL_GPL(devm_pci_pwrctrl_device_set_ready);

static int __pci_pwrctrl_power_off_device(struct device *dev)
{
	struct pci_pwrctrl *pwrctrl = dev_get_drvdata(dev);

	if (!pwrctrl)
		return 0;

	return pwrctrl->power_off(pwrctrl);
}

static void pci_pwrctrl_power_off_device(struct device_node *np)
{
	struct platform_device *pdev;
	int ret;

	for_each_available_child_of_node_scoped(np, child)
		pci_pwrctrl_power_off_device(child);

	pdev = of_find_device_by_node(np);
	if (!pdev)
		return;

	if (device_is_bound(&pdev->dev)) {
		ret = __pci_pwrctrl_power_off_device(&pdev->dev);
		if (ret)
			dev_err(&pdev->dev, "Failed to power off device: %d", ret);
	}

	platform_device_put(pdev);
}

/**
 * pci_pwrctrl_power_off_devices - Power off pwrctrl devices
 *
 * @parent: PCI host controller device
 *
 * Recursively traverse all pwrctrl devices for the devicetree hierarchy
 * below the specified PCI host controller and power them off in a depth
 * first manner.
 */
void pci_pwrctrl_power_off_devices(struct device *parent)
{
	struct device_node *np = parent->of_node;

	for_each_available_child_of_node_scoped(np, child)
		pci_pwrctrl_power_off_device(child);
}
EXPORT_SYMBOL_GPL(pci_pwrctrl_power_off_devices);

static int __pci_pwrctrl_power_on_device(struct device *dev)
{
	struct pci_pwrctrl *pwrctrl = dev_get_drvdata(dev);

	if (!pwrctrl)
		return 0;

	return pwrctrl->power_on(pwrctrl);
}

/*
 * Power on the devices in a depth first manner. Before powering on the device,
 * make sure its driver is bound.
 */
static int pci_pwrctrl_power_on_device(struct device_node *np)
{
	struct platform_device *pdev;
	int ret;

	for_each_available_child_of_node_scoped(np, child) {
		ret = pci_pwrctrl_power_on_device(child);
		if (ret)
			return ret;
	}

	pdev = of_find_device_by_node(np);
	if (!pdev)
		return 0;

	if (device_is_bound(&pdev->dev)) {
		ret = __pci_pwrctrl_power_on_device(&pdev->dev);
	} else {
		/* FIXME: Use blocking wait instead of probe deferral */
		dev_dbg(&pdev->dev, "driver is not bound\n");
		ret = -EPROBE_DEFER;
	}

	platform_device_put(pdev);

	return ret;
}

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
int pci_pwrctrl_power_on_devices(struct device *parent)
{
	struct device_node *np = parent->of_node;
	struct device_node *child = NULL;
	int ret;

	for_each_available_child_of_node(np, child) {
		ret = pci_pwrctrl_power_on_device(child);
		if (ret)
			goto err_power_off;
	}

	return 0;

err_power_off:
	for_each_available_child_of_node_scoped(np, tmp) {
		if (tmp == child)
			break;
		pci_pwrctrl_power_off_device(tmp);
	}
	of_node_put(child);

	return ret;
}
EXPORT_SYMBOL_GPL(pci_pwrctrl_power_on_devices);

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
static bool pci_pwrctrl_is_required(struct device_node *np)
{
	struct device_node *endpoint;
	const char *compat;
	int ret;

	ret = of_property_read_string(np, "compatible", &compat);
	if (ret < 0)
		return false;

	if (!strstarts(compat, "pci"))
		return false;

	if (of_pci_supply_present(np))
		return true;

	if (of_graph_is_present(np)) {
		for_each_endpoint_of_node(np, endpoint) {
			struct device_node *remote __free(device_node) =
				of_graph_get_remote_port_parent(endpoint);
			if (remote) {
				if (of_pci_supply_present(remote)) {
					of_node_put(endpoint);
					return true;
				}
			}
		}
	}

	return false;
}

static int pci_pwrctrl_create_device(struct device_node *np,
				     struct device *parent)
{
	struct platform_device *pdev;
	int ret;

	for_each_available_child_of_node_scoped(np, child) {
		ret = pci_pwrctrl_create_device(child, parent);
		if (ret)
			return ret;
	}

	/* Bail out if the platform device is already available for the node */
	pdev = of_find_device_by_node(np);
	if (pdev) {
		platform_device_put(pdev);
		return 0;
	}

	if (!pci_pwrctrl_is_required(np)) {
		dev_dbg(parent, "Skipping OF node: %s\n", np->name);
		return 0;
	}

	/* Now create the pwrctrl device */
	pdev = of_platform_device_create(np, NULL, parent);
	if (!pdev) {
		dev_err(parent, "Failed to create pwrctrl device for node: %s\n", np->name);
		return -EINVAL;
	}

	return 0;
}

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
int pci_pwrctrl_create_devices(struct device *parent)
{
	int ret;

	for_each_available_child_of_node_scoped(parent->of_node, child) {
		ret = pci_pwrctrl_create_device(child, parent);
		if (ret) {
			pci_pwrctrl_destroy_devices(parent);
			return ret;
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(pci_pwrctrl_create_devices);

static void pci_pwrctrl_destroy_device(struct device_node *np)
{
	struct platform_device *pdev;

	for_each_available_child_of_node_scoped(np, child)
		pci_pwrctrl_destroy_device(child);

	pdev = of_find_device_by_node(np);
	if (!pdev)
		return;

	of_device_unregister(pdev);
	platform_device_put(pdev);

	of_node_clear_flag(np, OF_POPULATED);
}

/**
 * pci_pwrctrl_destroy_devices - Destroy pwrctrl devices
 *
 * @parent: PCI host controller device
 *
 * Recursively destroy pwrctrl devices for the devicetree hierarchy below
 * the specified PCI host controller in a depth first manner.
 */
void pci_pwrctrl_destroy_devices(struct device *parent)
{
	struct device_node *np = parent->of_node;

	for_each_available_child_of_node_scoped(np, child)
		pci_pwrctrl_destroy_device(child);
}
EXPORT_SYMBOL_GPL(pci_pwrctrl_destroy_devices);

MODULE_AUTHOR("Bartosz Golaszewski <bartosz.golaszewski@linaro.org>");
MODULE_DESCRIPTION("PCI Device Power Control core driver");
MODULE_LICENSE("GPL");
