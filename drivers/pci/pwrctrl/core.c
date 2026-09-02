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
 *                 -> bus_register_notifier() 로 PCI 버스 알림을 구독한다.
 *                    이 트리의 이 파일에는 재스캔 워크도 pci_rescan_bus()
 *                    호출도 없다(전수 grep 확인). 알림을 받으면
 *                    pci_pwrctrl_notify() 가 같은 DT 노드를 쓰는 PCI 장치에
 *                    of_node_reused 를 표시할 뿐이다
 *
 * 제거:     [이 파일] pci_pwrctrl_device_unset_ready() / _cleanup()
 *
 * 실행 컨텍스트: 전부 프로세스 컨텍스트다. 알림 콜백도 버스 알림 사슬
 * 안에서 불리므로 마찬가지다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: probe.c 와 of.c 의 열거 경로, 그리고 각 pwrctrl 드라이버.
 * 아래쪽: 플랫폼 장치 인프라와 버스 알림(bus_register_notifier).
 * 공유 상태: struct pci_pwrctrl — notifier 블록과 대상 장치를 담는다.
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
 * pci_pwrctrl_device_set_ready()  : PCI 버스 알림을 구독한다(:151).
 * pci_pwrctrl_device_unset_ready(): 구독을 해제한다. 함수 안의 영어 주석대로
 *                                   링크를 따로 지울 필요는 없다.
 * devm_pci_pwrctrl_device_set_ready() : devres 판. 드라이버가 떨어질 때
 *                                   자동으로 unset 된다.
 * pci_pwrctrl_notify()            : 알림 콜백. 같은 DT 노드를 platform device 와
 *                                   PCI device 가 함께 쓰는 상황에서, 나중에
 *                                   온 PCI 쪽에 of_node_reused 를 표시해
 *                                   핀을 두 번 잡지 않게 한다.
 * pci_pwrctrl_power_on_devices() / _power_off_devices() : 부모 아래 DT 노드를
 *                                   훑어 pwrctrl 장치들을 켜고 끈다.
 * pci_pwrctrl_create_devices() / _destroy_devices() : 그 platform device 들을
 *                                   만들고 없앤다. pci_pwrctrl_is_required() 가
 *                                   대상 노드인지 판정한다.
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

/* [한국어]
 * pci_pwrctrl_notify - 같은 DT 노드를 쓰는 PCI 장치에 of_node_reused 를 표시한다
 *
 * @nb: 등록해 둔 notifier_block. container_of 로 pwrctrl 을 되찾는다.
 * @action: 알림 종류. BUS_NOTIFY_ADD_DEVICE 만 처리한다.
 * @data: 알림 대상 device.
 * @return: 언제나 NOTIFY_DONE — 다른 구독자를 막지 않는다.
 *
 * pci_pwrctrl_device_set_ready() 가 PCI 버스 알림에 등록해 둔 콜백이다.
 *
 * 해결하는 문제는 함수 안의 영어 주석에 있다. 하나의 DT 노드를 두 device 가
 * 함께 쓰는 상황이 생긴다 — 먼저 pwrctrl 의 platform device 가 그 노드로
 * 만들어지고, 전원이 들어온 뒤 같은 노드를 쓰는 PCI device 가 나타난다.
 * 서로 다른 버스에 있는 두 device 가 같은 노드를 참조하는 것이라, 핀 설정을
 * 두 번 적용하면 안 된다.
 *
 * 그래서 나중에 온 PCI 쪽에 of_node_reused 를 세워 "이 노드는 이미 누가
 * 쓰고 있다" 고 알린다. 판정은 이름이 아니라 dev_fwnode() 비교로 한다.
 *
 * [상류 코드 관찰] 이 트리의 이 파일에는 버스 재스캔을 예약하는 코드가 없다.
 * set_ready() 가 하는 일은 알림 구독뿐이고, 이 콜백은 위 표시만 남긴다.
 *
 * 실행 컨텍스트: PCI 버스 알림 사슬. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 남의 장치면 그대로 NOTIFY_DONE 이다.
 *
 * 호출 체인:
 *   device_add() → PCI 버스 알림 사슬 → [이 함수]
 */
static int pci_pwrctrl_notify(struct notifier_block *nb, unsigned long action,
			      void *data)
{
	struct pci_pwrctrl *pwrctrl = container_of(nb, struct pci_pwrctrl, nb);
	/* [한국어] 알림과 함께 온 device. 이 알림이 우리와 상관있는지 아래에서 판정한다. */
	struct device *dev = data;

	/* [한국어] 펌웨어 노드가 다르면 남의 장치다. **노드로** 비교하는 것이 핵심인데,
	 * pwrctrl 의 platform device 와 그 뒤에 나타나는 PCI device 가 서로 다른
	 * 버스에 있으면서 같은 DT 노드를 쓰기 때문이다. */
	if (dev_fwnode(dev) != dev_fwnode(pwrctrl->dev))
		return NOTIFY_DONE;

	/* [한국어] 알림 종류에 따라 갈린다. 지금은 한 가지만 처리한다. */
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
/* [한국어]
 * pci_pwrctrl_init - 전원 제어 문맥을 초기화한다
 *
 * @pwrctrl: 초기화할 문맥.
 * @dev: 이 문맥을 소유하는 플랫폼 장치.
 *
 * pwrctrl 드라이버가 probe 에서 가장 먼저 부르는 함수다.
 *
 * 두 방향의 연결을 세우는 것이 하는 일의 전부다. 문맥이 자기 장치를 가리키고,
 * 장치의 drvdata 가 그 문맥을 가리킨다. 뒤의 연결이 있어야 이 파일의
 * __pci_pwrctrl_power_on/off_device() 가 device 포인터만 들고도 문맥을 되찾을
 * 수 있다 — 그 함수들은 디바이스 트리를 훑다가 만난 장치를 다루므로 문맥을
 * 직접 받을 방법이 없다.
 *
 * 알림 구독은 여기서 하지 않는다. pci_pwrctrl_device_set_ready() 가 따로
 * 하는데, 전원이 실제로 들어온 뒤에 구독해야 하기 때문이다.
 *
 * 실행 컨텍스트: pwrctrl 드라이버 probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pwrctrl 드라이버 probe → [이 함수]
 */
void pci_pwrctrl_init(struct pci_pwrctrl *pwrctrl, struct device *dev)
{
	/* [한국어] 어느 장치의 pwrctrl 인지 기록한다. */
	pwrctrl->dev = dev;
	/* [한국어] 반대 방향도 이어 둔다. 이렇게 해 두면 아래 __pci_pwrctrl_power_on/off_device()
	 * 가 device 만으로 pwrctrl 을 되찾을 수 있다. */
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
/* [한국어]
 * pci_pwrctrl_device_set_ready - 전원 준비가 끝났음을 알리고 PCI 버스 알림을 구독한다
 *
 * @pwrctrl: 준비된 문맥.
 * @return: 0 = 성공, -ENODEV = 초기화되지 않음, 또는 구독 오류.
 *
 * 이 파일의 설계가 이 함수에 모인다. pwrctrl 장치와 그것이 전원을 대는 PCI
 * 장치는 **같은 디바이스 트리 노드** 를 쓰는데, 커널의 장치 모델은 한 노드에
 * 장치가 하나인 것을 전제한다. 그 충돌을 알림으로 푼다.
 *
 * 버스 알림을 구독하면, 나중에 그 노드로 PCI 장치가 추가될 때 이 파일의
 * pci_pwrctrl_notify() 가 불려 "이 노드는 이미 쓰이고 있다" 고 표시할 수
 * 있다. 그 표시가 없으면 장치 모델이 노드 중복을 문제 삼는다.
 *
 * dev 가 없으면 -ENODEV 로 물러난다. pci_pwrctrl_init() 을 부르지 않았다는
 * 뜻이며, 그 상태로 구독하면 알림이 와도 어느 장치인지 알 수 없다.
 *
 * pci_pwrctrl_device_unset_ready() 와 짝을 이루며, 드라이버가 떨어질 때 반드시
 * 구독을 풀어야 한다.
 *
 * 실행 컨텍스트: 전원을 넣은 뒤의 pwrctrl 드라이버 probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 초기화 누락은 -ENODEV, 구독 실패는 그 오류를 그대로 올려보낸다.
 *
 * 호출 체인:
 *   pwrctrl 드라이버 probe → [이 함수] → bus_register_notifier()
 */
int pci_pwrctrl_device_set_ready(struct pci_pwrctrl *pwrctrl)
{
	/* [한국어] 알림 등록 결과. */
	int ret;

	/* [한국어] pci_pwrctrl_init() 을 거치지 않았으면 대상 장치를 모르므로, */
	if (!pwrctrl->dev)
		return -ENODEV;

	/* [한국어] 알림 콜백을 연결한다. */
	pwrctrl->nb.notifier_call = pci_pwrctrl_notify;
	/* [한국어] PCI 버스의 알림 사슬에 등록한다. 이 시점부터 PCI 장치가 추가될 때마다
	 * 위 콜백이 불린다. */
	ret = bus_register_notifier(&pci_bus_type, &pwrctrl->nb);
	/* [한국어] 실패하면, */
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
/* [한국어]
 * pci_pwrctrl_device_unset_ready - PCI 버스 알림 구독을 푼다
 *
 * @pwrctrl: 대상 문맥.
 *
 * pci_pwrctrl_device_set_ready() 의 짝이며 구독 해제가 전부다.
 *
 * 반드시 풀어야 하는 이유는 알림 블록이 문맥 안에 들어 있기 때문이다.
 * 문맥이 해제된 뒤에도 구독이 남아 있으면 다음 알림이 사라진 메모리를
 * 건드린다.
 *
 * pci_pwrctrl_init() 이 세운 dev 와 drvdata 는 되돌리지 않는다 — 옆의 상류
 * 주석이 그 이유를 밝히며, 그 정리는 장치 모델이 알아서 한다.
 *
 * 실행 컨텍스트: pwrctrl 드라이버 remove. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pwrctrl 드라이버 remove / devm 정리 → [이 함수]
 *     → bus_unregister_notifier()
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

/* [한국어] devres 가 넘겨 준 데이터가 곧 pwrctrl 이다. */
/* [한국어]
 * devm_pci_pwrctrl_device_unset_ready - devres 가 부르는 해제 어댑터
 *
 * @data: devm_add_action_or_reset() 에 맡긴 pwrctrl 포인터.
 *
 * devres 액션은 void * 하나만 받는 규약이라, 타입을 되돌려 실제 해제 함수로
 * 넘기는 한 줄짜리 어댑터가 필요하다.
 *
 * 이 함수가 있는 덕분에 드라이버는 해제를 잊을 수 없다. 드라이버가 떨어질
 * 때 devres 가 알아서 부른다.
 *
 * 실행 컨텍스트: 드라이버 언바인드 시 devres 해제. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   드라이버 언바인드 → devres → [이 함수] → pci_pwrctrl_device_unset_ready()
 */
static void devm_pci_pwrctrl_device_unset_ready(void *data)
{
	/* [한국어] 타입을 되돌린다. */
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
/* [한국어]
 * devm_pci_pwrctrl_device_set_ready - 구독을 devres 에 맡겨 자동으로 풀리게 한다
 *
 * @dev: 수명을 맡길 장치.
 * @pwrctrl: 준비된 문맥.
 * @return: 0 = 성공, 또는 음수 오류.
 *
 * pci_pwrctrl_device_set_ready() 의 devres 판이다. 드라이버가 떨어질 때
 * 자동으로 구독이 풀려, remove 에서 짝을 맞출 필요가 없다.
 *
 * devm_add_action_or_reset() 이 그 자동화의 핵심이다. 이름의 or_reset 이
 * 중요한데, 등록 자체가 실패하면 방금 넘긴 정리 동작을 **그 자리에서** 한 번
 * 실행하고 오류를 돌려준다. 그 덕분에 호출자는 "성공이면 아무것도, 실패면
 * 아무것도" 라는 단순한 규약만 지키면 되고, 구독이 남은 채로 실패하는 상태가
 * 생기지 않는다.
 *
 * 실행 컨텍스트: pwrctrl 드라이버 probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 구독 실패는 그대로 올려보낸다. devres 등록 실패는 구독을
 * 되돌린 뒤 오류를 돌려준다.
 *
 * 호출 체인:
 *   pwrctrl 드라이버 probe → [이 함수]
 *     → pci_pwrctrl_device_set_ready() → devm_add_action_or_reset()
 */
int devm_pci_pwrctrl_device_set_ready(struct device *dev,
				      struct pci_pwrctrl *pwrctrl)
{
	/* [한국어] 등록 결과. */
	int ret;

	/* [한국어] 먼저 알림을 등록하고, */
	ret = pci_pwrctrl_device_set_ready(pwrctrl);
	/* [한국어] 실패하면 devres 액션을 걸지 않고 그대로 반환한다. 등록되지 않은 것을
	 * 해제하려 들면 안 되기 때문이다. */
	if (ret)
		return ret;

	/* [한국어] 성공했으면 해제 액션을 devres 에 건다. _or_reset 판이라 액션 등록 자체가
	 * 실패하면 그 자리에서 액션을 한 번 실행해 준다 — 그래야 방금 등록한 알림이
	 * 누수되지 않는다. */
	return devm_add_action_or_reset(dev,
					devm_pci_pwrctrl_device_unset_ready,
					pwrctrl);
}
EXPORT_SYMBOL_GPL(devm_pci_pwrctrl_device_set_ready);

/* [한국어]
 * __pci_pwrctrl_power_off_device - device 하나의 전원 차단 콜백을 부른다
 *
 * @dev: 전원을 끌 platform device.
 * @return: 0 = 성공(또는 대상이 아님), 그 밖에 드라이버 콜백의 오류.
 *
 * drvdata 에 걸어 둔 pwrctrl 을 되찾아 그 power_off 콜백을 부른다.
 * 실제 레귤레이터·클럭 조작은 generic.c 같은 개별 드라이버가 한다.
 *
 * pwrctrl 이 없으면 0 을 반환하는 것이 중요하다. 이 함수는 DT 트리를 훑다
 * 만난 모든 노드에 대해 불릴 수 있고, 그중 상당수는 pwrctrl 대상이 아니다.
 * "대상이 아님" 을 오류로 다루면 순회 전체가 실패한다.
 *
 * 이름 앞의 밑줄 둘은 "잠금이나 검사 없이 곧장 부른다" 는 관례 표시로,
 * 아래 pci_pwrctrl_power_off_device() 가 노드 순회와 참조 관리를 맡는다.
 *
 * 실행 컨텍스트: 전원 차단 경로. 프로세스 컨텍스트이며 드라이버 콜백이
 * 잠들 수 있다.
 *
 * 에러 경로: 드라이버 콜백의 오류를 그대로 올려보낸다.
 *
 * 호출 체인:
 *   pci_pwrctrl_power_off_device() → [이 함수] → pwrctrl->power_off()
 */
static int __pci_pwrctrl_power_off_device(struct device *dev)
{
	/* [한국어] 이 장치에 매달린 pwrctrl 을 되찾는다. pci_pwrctrl_init() 이 걸어 둔 것이다. */
	struct pci_pwrctrl *pwrctrl = dev_get_drvdata(dev);

	/* [한국어] pwrctrl 이 없으면 이 장치는 전원 제어 대상이 아니므로, */
	if (!pwrctrl)
		return 0;

	/* [한국어] 드라이버가 제공한 전원 차단 콜백을 부른다. 실제 레귤레이터·클럭 조작은
	 * generic.c 같은 개별 드라이버가 한다. */
	return pwrctrl->power_off(pwrctrl);
}

/* [한국어]
 * pci_pwrctrl_power_off_device - DT 서브트리를 훑으며 아래에서 위로 전원을 끈다
 *
 * @np: 이 서브트리의 뿌리 노드.
 *
 * 재귀로 자식을 먼저 끄고 자기를 끈다. 전원은 위에서 아래로 공급되므로,
 * 끌 때는 아래에서 위로 올라가야 아직 전원이 필요한 자식이 먼저 끊기지 않는다.
 *
 * _scoped 판 순회를 쓰는 덕분에 노드 참조가 자동으로 반환된다. 반면
 * of_find_device_by_node() 가 올린 platform device 참조는 직접 놓아야 하며,
 * 장치가 없어 조기 반환하는 경로에서는 애초에 참조를 잡지 않았으므로 문제가 없다.
 *
 * device_is_bound() 검사가 필요한 이유는 platform device 가 만들어졌더라도
 * 드라이버가 아직 붙지 않았을 수 있기 때문이다. 그때는 부를 콜백이 없다.
 *
 * 실패해도 계속 진행한다. 반환값이 없어 알릴 방법이 없고, 해제 경로에서
 * 중단하면 나머지가 켜진 채 남기 때문이다.
 *
 * 실행 컨텍스트: 전원 차단 경로. 프로세스 컨텍스트이며 잠들 수 있다.
 *
 * 에러 경로: 로그만 남긴다.
 *
 * 호출 체인:
 *   pci_pwrctrl_power_off_devices() / pci_pwrctrl_power_on_devices() 의 되감기
 *     → [이 함수](재귀) → of_find_device_by_node()
 *     → __pci_pwrctrl_power_off_device() → platform_device_put()
 */
static void pci_pwrctrl_power_off_device(struct device_node *np)
{
	/* [한국어] 이 노드에 대응하는 platform device. */
	struct platform_device *pdev;
	/* [한국어] 차단 결과. */
	int ret;

	/* [한국어] **자식을 먼저** 끈다. 전원은 위에서 아래로 공급되므로, 끌 때는 아래에서
	 * 위로 올라가야 아직 전원이 필요한 자식이 먼저 끊기지 않는다. */
	for_each_available_child_of_node_scoped(np, child)
		pci_pwrctrl_power_off_device(child);

	/* [한국어] 이 노드의 platform device 를 찾는다. 참조를 올려 반환하므로 아래에서
	 * 반드시 놓아야 한다. */
	pdev = of_find_device_by_node(np);
	/* [한국어] 장치가 없으면(pwrctrl 대상이 아닌 노드면), */
	if (!pdev)
		return;

	/* [한국어] 드라이버가 붙어 있을 때만 콜백을 부를 수 있다. */
	if (device_is_bound(&pdev->dev)) {
		/* [한국어] 실제 차단. */
		ret = __pci_pwrctrl_power_off_device(&pdev->dev);
		/* [한국어] 실패하면, */
		if (ret)
			/* [한국어] 기록만 남긴다. 반환값이 없어 알릴 방법이 없고, 해제 경로라 중단할 수도 없다. */
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
/* [한국어]
 * pci_pwrctrl_power_off_devices - 이 노드 아래 pwrctrl 장치들의 전원을 끊는다
 *
 * @parent: 부모 장치. 그 디바이스 트리 노드의 자식들이 대상이다.
 *
 * 컨트롤러 드라이버가 절전에 들어가거나 내려갈 때 부른다.
 *
 * 바로 아래 자식만 훑는다. 켜는 쪽(pci_pwrctrl_power_on_device)이 재귀로
 * 손자까지 내려가는 것과 다르며, 상류 코드가 그렇게 되어 있다.
 *
 * _scoped 순회 매크로를 쓰는 것이 눈에 띈다. 순회 중 얻은 노드 참조를
 * 매 반복 끝에서 자동으로 놓아 주므로, 중간에 빠져나가도 참조가 새지 않는다.
 *
 * 반환값이 없다. 전원을 끄는 것은 실패해도 되돌릴 수 없는 정리 동작이라,
 * 호출자가 할 수 있는 일이 없기 때문이다.
 *
 * 실행 컨텍스트: 컨트롤러 드라이버의 절전·remove. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 개별 실패는 아래에서 로그로만 남는다.
 *
 * 호출 체인:
 *   컨트롤러 드라이버 → [이 함수] → pci_pwrctrl_power_off_device()
 */
void pci_pwrctrl_power_off_devices(struct device *parent)
{
	/* [한국어] 부모의 DT 노드. */
	struct device_node *np = parent->of_node;

	/* [한국어] 직속 자식마다 재귀 차단을 시작한다. _scoped 판이라 순회가 끝나면
	 * 노드 참조가 자동으로 반환된다. */
	for_each_available_child_of_node_scoped(np, child)
		pci_pwrctrl_power_off_device(child);
}
EXPORT_SYMBOL_GPL(pci_pwrctrl_power_off_devices);

/* [한국어]
 * __pci_pwrctrl_power_on_device - device 하나의 전원 공급 콜백을 부른다
 *
 * @dev: 전원을 넣을 platform device.
 * @return: 0 = 성공(또는 대상이 아님), 그 밖에 드라이버 콜백의 오류.
 *
 * __pci_pwrctrl_power_off_device() 와 완전히 대칭이며, 부르는 콜백만 다르다.
 * pwrctrl 이 없을 때 0 을 반환하는 이유도 같다 — DT 트리에는 pwrctrl 대상이
 * 아닌 노드가 훨씬 많다.
 *
 * 차단 쪽과 달리 이 반환값은 실제로 쓰인다. 공급이 실패하면 위에서
 * 되감기를 해야 하기 때문이다.
 *
 * 실행 컨텍스트: 전원 공급 경로. 프로세스 컨텍스트이며 드라이버 콜백이
 * 잠들 수 있다.
 *
 * 에러 경로: 드라이버 콜백의 오류를 그대로 올려보낸다.
 *
 * 호출 체인:
 *   pci_pwrctrl_power_on_device() → [이 함수] → pwrctrl->power_on()
 */
static int __pci_pwrctrl_power_on_device(struct device *dev)
{
	/* [한국어] 이 장치의 pwrctrl. */
	struct pci_pwrctrl *pwrctrl = dev_get_drvdata(dev);

	/* [한국어] 없으면 대상이 아니므로, */
	if (!pwrctrl)
		return 0;

	/* [한국어] 드라이버의 전원 공급 콜백을 부른다. 차단 쪽과 완전히 대칭이다. */
	return pwrctrl->power_on(pwrctrl);
}

/*
 * Power on the devices in a depth first manner. Before powering on the device,
 * make sure its driver is bound.
 */
/* [한국어]
 * pci_pwrctrl_power_on_device - 한 노드와 그 아래 전부의 전원을 켠다
 *
 * @np: 대상 디바이스 트리 노드.
 * @return: 0 = 성공, -EPROBE_DEFER, 또는 아래의 오류.
 *
 * 자기 자신을 부르는 재귀 함수다. 자식을 먼저 다 켜고 자신을 켠다.
 *
 * 그 순서가 이 함수의 요점이다. 전원 공급이 사슬을 이룰 수 있어 — 어떤
 * 장치의 전원이 다른 장치를 거쳐 오는 경우 — 안쪽부터 켜야 바깥이 켜질 때
 * 이미 준비가 되어 있다.
 *
 * 노드에 대응하는 플랫폼 장치가 없으면 성공으로 답한다. 전원 제어가 필요
 * 없는 노드라는 뜻이므로 할 일이 없는 것이다.
 *
 * 장치는 있는데 드라이버가 아직 붙지 않았으면 -EPROBE_DEFER 를 돌려준다.
 * 호출자인 컨트롤러의 probe 를 미뤘다가 다시 시도하게 하는 것이다. 옆의
 * FIXME 가 이것을 임시 방편으로 표시하고 있는데, 기다림으로 바꾸는 것이
 * 더 나은 해법이라는 상류 개발자의 메모다.
 *
 * 찾은 장치의 참조를 반드시 놓는다. of_find_device_by_node() 가 참조를
 * 올려 주기 때문이다.
 *
 * 실행 컨텍스트: 컨트롤러 드라이버 probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 자식 하나라도 실패하면 즉시 그 오류로 중단한다. 되감기는
 * 상위의 pci_pwrctrl_power_on_devices() 가 한다.
 *
 * 호출 체인:
 *   pci_pwrctrl_power_on_devices() → [이 함수](재귀)
 *     → of_find_device_by_node() → __pci_pwrctrl_power_on_device()
 */
static int pci_pwrctrl_power_on_device(struct device_node *np)
{
	/* [한국어] platform device. */
	struct platform_device *pdev;
	/* [한국어] 결과. */
	int ret;

	/* [한국어] **자식을 먼저** 켠다. 차단이 아래에서 위로였던 것과 반대로, 공급은
	 * 위에서 아래로 가야 할 것 같지만 여기서는 자식이 먼저다.
	 * DT 트리에서 자식 노드가 전원 공급 장치를 나타내는 구조이기 때문으로,
	 * 차단 쪽과 순서가 같아 두 함수가 대칭을 이룬다. */
	for_each_available_child_of_node_scoped(np, child) {
		/* [한국어] 재귀. */
		ret = pci_pwrctrl_power_on_device(child);
		/* [한국어] 하나라도 실패하면, */
		if (ret)
			return ret;
	}

	/* [한국어] 이 노드의 platform device 를 찾는다. */
	pdev = of_find_device_by_node(np);
	/* [한국어] 없으면 켤 것이 없으므로 성공으로 친다. */
	if (!pdev)
		return 0;

	/* [한국어] 드라이버가 붙어 있을 때만. */
	if (device_is_bound(&pdev->dev)) {
		/* [한국어] 실제 공급. */
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
/* [한국어]
 * pci_pwrctrl_power_on_devices - 이 노드 아래 pwrctrl 장치들의 전원을 켠다
 *
 * @parent: 부모 장치.
 * @return: 0 = 성공, 또는 음수 오류.
 *
 * pci_pwrctrl_power_off_devices() 의 짝이며, 컨트롤러가 버스를 스캔하기
 * **전에** 불려야 한다. 전원이 없는 장치는 config 접근에 응답하지 않아
 * 스캔에서 보이지 않는다.
 *
 * 되감기가 있는 것이 끄는 쪽과 다르다. 중간에 실패하면 지금까지 켠 것들을
 * 다시 끄고 오류를 돌려주는데, 절반만 켜진 상태로 두면 다음 시도에서
 * 어디까지 켜졌는지 알 수 없기 때문이다.
 *
 * 그래서 순회 매크로도 _scoped 판이 아니다. 되감기 경로에서 순회를 이어
 * 가야 해 노드 포인터를 함수 범위에서 직접 들고 있어야 한다.
 *
 * 실행 컨텍스트: 컨트롤러 드라이버 probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 실패하면 goto 로 되감기 경로에 들어가 지금까지 켠 것을 끈다.
 * -EPROBE_DEFER 가 올라오면 컨트롤러의 probe 가 미뤄진다.
 *
 * 호출 체인:
 *   컨트롤러 드라이버 probe → [이 함수]
 *     → pci_pwrctrl_power_on_device() → (실패 시) pci_pwrctrl_power_off_device()
 */
int pci_pwrctrl_power_on_devices(struct device *parent)
{
	/* [한국어] 부모의 DT 노드. */
	struct device_node *np = parent->of_node;
	/* [한국어] 순회 커서. **_scoped 판이 아니라** 일반 순회를 쓰는데, 실패 시 되감기에서
	 * "어디까지 켰는가" 를 알아야 하므로 루프 밖에서도 이 값이 살아 있어야
	 * 하기 때문이다. */
	struct device_node *child = NULL;
	/* [한국어] 결과. */
	int ret;

	/* [한국어] 자식마다 켠다. */
	for_each_available_child_of_node(np, child) {
		/* [한국어] 재귀 공급. */
		ret = pci_pwrctrl_power_on_device(child);
		/* [한국어] 실패하면, */
		if (ret)
			goto err_power_off;
	}

	return 0;

err_power_off:
	for_each_available_child_of_node_scoped(np, tmp) {
		/* [한국어] 되감기 루프. 방금 실패한 노드에 닿으면 멈춘다 — 그 노드는 켜지지 않았으므로
		 * 끌 것이 없다. 그 앞까지 켜 둔 것만 차례로 끈다. */
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
/* [한국어]
 * pci_pwrctrl_is_required - 이 노드에 전원 제어 장치를 만들어야 하는지 판단한다
 *
 * @np: 검사할 디바이스 트리 노드.
 * @return: true = 필요, false = 불필요.
 *
 * 디바이스 트리의 모든 노드에 pwrctrl 장치를 만들 수는 없다. 대부분은
 * 전원 제어가 필요 없고, 만들면 쓸데없는 장치가 늘어난다. 그 선별이
 * 이 함수의 일이다.
 *
 * 세 단계로 좁힌다.
 * 1. compatible 문자열이 "pci" 로 시작하는가. PCI 장치를 서술하는 노드만
 *    대상이다.
 * 2. 전원 공급이 명시되어 있는가. 그렇다면 그 전원을 켜 줄 주체가 필요하다.
 * 3. 그렇지 않더라도, graph 로 연결된 상대가 전원 공급을 갖고 있는가.
 *    전원이 물리적으로 다른 노드에 적혀 있는 배치가 있어 그쪽까지 본다.
 *
 * 3번이 이 함수에서 가장 미묘한 부분이다. of_graph 는 노드끼리의 연결을
 * 서술하는 별도 표현이며, 이 판단이 그것을 따라간다.
 *
 * __free(device_node) 로 얻은 참조가 범위를 벗어날 때 자동으로 놓인다.
 * 검사 중간에 true 로 빠져나가는 경로가 여럿이라 수동 해제로는 놓치기 쉽다.
 *
 * 실행 컨텍스트: 컨트롤러 드라이버 probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 판단이 서지 않는 모든 경우가 false 로 합쳐진다.
 *
 * 호출 체인:
 *   pci_pwrctrl_create_device() → [이 함수]
 *     → of_property_read_string() → of_pci_supply_present()
 *     → of_graph_get_remote_port_parent()
 */
static bool pci_pwrctrl_is_required(struct device_node *np)
{
	/* [한국어] of_graph 연결의 상대 노드를 찾을 때 쓸 임시 변수. */
	struct device_node *endpoint;
	/* [한국어] compatible 문자열. */
	const char *compat;
	/* [한국어] 조회 결과. */
	int ret;

	/* [한국어] 이 노드의 compatible 을 읽는다. */
	ret = of_property_read_string(np, "compatible", &compat);
	/* [한국어] 없으면 판단할 근거가 없으므로, */
	if (ret < 0)
		return false;

	/* [한국어] "pci" 로 시작하지 않으면 PCI 장치를 나타내는 노드가 아니다. 문자열
	 * 접두사로 거르는 것이라 느슨하지만, 아래 두 검사가 실제 판정을 한다. */
	if (!strstarts(compat, "pci"))
		return false;

	/* [한국어] 노드 자체에 전원 공급 속성이 있으면 pwrctrl 이 필요하다. */
	if (of_pci_supply_present(np))
		return true;

	/* [한국어] of_graph 연결이 있으면 한 단계 더 본다. */
	if (of_graph_is_present(np)) {
		/* [한국어] 연결점마다, */
		for_each_endpoint_of_node(np, endpoint) {
			/* [한국어] 상대편 노드를 얻는다. __free(device_node) 로 선언해 스코프를 벗어날 때
			 * 참조가 자동 반환되므로, 아래 조기 반환 경로에서도 누수가 없다. */
			struct device_node *remote __free(device_node) =
				of_graph_get_remote_port_parent(endpoint);
			/* [한국어] 상대가 있고, */
			if (remote) {
				/* [한국어] 그쪽에 전원 공급 속성이 있으면 이 노드도 pwrctrl 대상이다.
				 * 전원 시퀀서를 별도 노드로 두는 구성을 잡아내기 위한 검사다. */
				if (of_pci_supply_present(remote)) {
					of_node_put(endpoint);
					return true;
				}
			}
		}
	}

	return false;
}

/* [한국어]
 * pci_pwrctrl_create_device - DT 서브트리를 훑으며 필요한 pwrctrl platform device 를 만든다
 *
 * @np: 이 서브트리의 뿌리 노드.
 * @parent: 만들어질 platform device 의 부모.
 * @return: 0 = 성공(또는 만들 필요 없음), -EINVAL = 생성 실패.
 *
 * 재귀로 자식을 먼저 만들고 자기를 만든다. 전원 차단·공급과 같은 아래에서
 * 위로의 순서다.
 *
 * 세 가지를 차례로 확인한다. 이미 platform device 가 있으면 만들 필요가 없고
 * (그 경우 조회로 올라간 참조를 바로 놓는다), pci_pwrctrl_is_required() 가
 * 아니라고 하면 건너뛰며, 둘 다 아니면 만든다.
 *
 * 건너뛰는 경우가 오류가 아니라는 점이 중요하다. PCI 노드 아래에는 pwrctrl 과
 * 무관한 노드가 많고, 그 전부에 대해 device 를 만들 수는 없다.
 *
 * 실행 컨텍스트: 호스트 브리지 probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 생성 실패만 -EINVAL 이며, 호출자가 그것을 받아 지금까지 만든
 * 것을 전부 없앤다.
 *
 * 호출 체인:
 *   pci_pwrctrl_create_devices() → [이 함수](재귀)
 *     → of_find_device_by_node() → pci_pwrctrl_is_required()
 *     → of_platform_device_create()
 */
static int pci_pwrctrl_create_device(struct device_node *np,
				     struct device *parent)
{
	/* [한국어] 이 노드에 대응하는 platform device. */
	struct platform_device *pdev;
	/* [한국어] 결과. */
	int ret;

	/* [한국어] **자식을 먼저** 만든다. 아래에서 위로 만들어야 부모가 생길 때 자식이
	 * 이미 있는 구조가 된다. */
	for_each_available_child_of_node_scoped(np, child) {
		/* [한국어] 재귀 생성. */
		ret = pci_pwrctrl_create_device(child, parent);
		/* [한국어] 실패하면, */
		if (ret)
			return ret;
	}

	/* Bail out if the platform device is already available for the node */
	pdev = of_find_device_by_node(np);
	/* [한국어] 이미 platform device 가 있으면 만들 필요가 없다. */
	if (pdev) {
		platform_device_put(pdev);
		return 0;
	}

	/* [한국어] pwrctrl 이 필요한 노드가 아니면, */
	if (!pci_pwrctrl_is_required(np)) {
		/* [한국어] 건너뛴 사실만 디버그 로그에 남긴다. */
		dev_dbg(parent, "Skipping OF node: %s\n", np->name);
		return 0;
	}

	/* Now create the pwrctrl device */
	pdev = of_platform_device_create(np, NULL, parent);
	/* [한국어] 생성에 실패하면, */
	if (!pdev) {
		/* [한국어] 어느 노드였는지 남기고, */
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
/* [한국어]
 * pci_pwrctrl_create_devices - 이 노드 아래에 필요한 pwrctrl 장치들을 만든다
 *
 * @parent: 부모 장치.
 * @return: 0 = 성공, 또는 음수 오류.
 *
 * 컨트롤러 드라이버 probe 의 이른 단계에서 불린다. 전원을 켜려면 켤 주체가
 * 먼저 있어야 하기 때문이다.
 *
 * 노드마다 만들지 말지는 pci_pwrctrl_is_required() 가 아래에서 판단하므로,
 * 이 함수는 자식을 훑으며 넘기기만 한다.
 *
 * 실패하면 지금까지 만든 것을 모두 없앤다. 절반만 만들어진 상태로 두면
 * 컨트롤러 probe 가 실패해 되감을 때 무엇이 만들어졌는지 알 수 없다.
 * 되감기가 pci_pwrctrl_destroy_devices() 하나로 끝나는 것은 그쪽이 노드
 * 전체를 훑으며 있는 것만 없애기 때문이다.
 *
 * 실행 컨텍스트: 컨트롤러 드라이버 probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 실패 시 pci_pwrctrl_destroy_devices() 로 전부 되감고 오류를
 * 돌려준다.
 *
 * 호출 체인:
 *   컨트롤러 드라이버 probe → [이 함수]
 *     → pci_pwrctrl_create_device() → (실패 시) pci_pwrctrl_destroy_devices()
 */
int pci_pwrctrl_create_devices(struct device *parent)
{
	/* [한국어] 결과. */
	int ret;

	/* [한국어] 부모의 직속 자식마다, */
	for_each_available_child_of_node_scoped(parent->of_node, child) {
		/* [한국어] 재귀 생성을 시작한다. */
		ret = pci_pwrctrl_create_device(child, parent);
		/* [한국어] 하나라도 실패하면 지금까지 만든 것을 전부 없앤다. 개별 되감기 대신
		 * 전체 해제를 부르는 것은 destroy 쪽이 없는 노드를 만나도 조용히 넘어가도록
		 * 만들어져 있기 때문이다. */
		if (ret) {
			pci_pwrctrl_destroy_devices(parent);
			return ret;
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(pci_pwrctrl_create_devices);

/* [한국어]
 * pci_pwrctrl_destroy_device - DT 서브트리의 pwrctrl platform device 들을 없앤다
 *
 * @np: 이 서브트리의 뿌리 노드.
 *
 * pci_pwrctrl_create_device() 의 짝이며 순서도 같다 — 자식을 먼저 없앤다.
 *
 * 없는 장치를 만나면 조용히 넘어가는 관대함이 설계의 일부다. 그 덕분에
 * pci_pwrctrl_create_devices() 가 도중에 실패했을 때 개별 되감기를 만들지 않고
 * 전체 해제를 그대로 부를 수 있다.
 *
 * 마지막의 OF_POPULATED 플래그 지우기가 중요하다. DT 코어가 이 플래그로
 * "이 노드로는 이미 device 를 만들었다" 를 기억하므로, 지워 두지 않으면
 * 나중에 같은 노드로 다시 만들 수 없다. 호스트 브리지를 뺐다 꽂는 경우가
 * 그 시나리오다.
 *
 * of_device_unregister() 와 platform_device_put() 이 나란히 오는 것은
 * 전자가 등록을 해제하고 후자가 조회로 올린 참조를 놓기 때문이다.
 *
 * 실행 컨텍스트: 호스트 브리지 remove 또는 create 실패 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_pwrctrl_destroy_devices() → [이 함수](재귀)
 *     → of_find_device_by_node() → of_device_unregister()
 *     → platform_device_put() → of_node_clear_flag(OF_POPULATED)
 */
static void pci_pwrctrl_destroy_device(struct device_node *np)
{
	/* [한국어] platform device. */
	struct platform_device *pdev;

	/* [한국어] **자식을 먼저** 없앤다. 생성과 같은 순서다. */
	for_each_available_child_of_node_scoped(np, child)
		pci_pwrctrl_destroy_device(child);

	/* [한국어] 이 노드의 platform device 를 찾는다. */
	pdev = of_find_device_by_node(np);
	/* [한국어] 없으면 없앨 것도 없다. 이 관대함 덕분에 위 create 실패 경로가
	 * 전체 해제를 그대로 부를 수 있다. */
	if (!pdev)
		return;

	of_device_unregister(pdev);
	platform_device_put(pdev);

	/* [한국어] OF_POPULATED 표시를 지운다. 이렇게 해야 나중에 같은 노드로 다시
	 * platform device 를 만들 수 있다 — DT 코어가 이 플래그로 중복 생성을
	 * 막기 때문이다. */
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
/* [한국어]
 * pci_pwrctrl_destroy_devices - 이 노드 아래 pwrctrl 장치들을 없앤다
 *
 * @parent: 부모 장치.
 *
 * pci_pwrctrl_create_devices() 의 짝이며, 그쪽의 되감기 경로에서도 쓰인다.
 *
 * 한 함수가 정상 정리와 되감기를 겸할 수 있는 이유는 아래 함수가 없는 것을
 * 건너뛰기 때문이다. 절반만 만들어진 상태에서 불려도 만들어진 것만 없앤다.
 *
 * 바로 아래 자식만 훑는 것이 pci_pwrctrl_power_off_devices() 와 같다.
 *
 * 반환값이 없다. 정리 동작이라 실패해도 호출자가 할 수 있는 일이 없다.
 *
 * 실행 컨텍스트: 컨트롤러 드라이버 remove, 또는 create 의 되감기.
 * 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   컨트롤러 드라이버 remove / pci_pwrctrl_create_devices() 의 되감기
 *     → [이 함수] → pci_pwrctrl_destroy_device()
 */
void pci_pwrctrl_destroy_devices(struct device *parent)
{
	/* [한국어] 부모의 DT 노드. */
	struct device_node *np = parent->of_node;

	/* [한국어] 자식마다 재귀 해제를 시작한다. */
	for_each_available_child_of_node_scoped(np, child)
		pci_pwrctrl_destroy_device(child);
}
EXPORT_SYMBOL_GPL(pci_pwrctrl_destroy_devices);

MODULE_AUTHOR("Bartosz Golaszewski <bartosz.golaszewski@linaro.org>");
MODULE_DESCRIPTION("PCI Device Power Control core driver");
MODULE_LICENSE("GPL");
