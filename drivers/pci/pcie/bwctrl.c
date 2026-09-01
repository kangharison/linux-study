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
 *         -> [이 파일] pcie_bwnotif_irq() — 스레드로 넘기지 않고 이 핸들러
 *            안에서 전부 처리한다(request_irq 를 IRQF_SHARED 로만 걸며,
 *            :344 에 스레드 함수 인자가 없다)
 *               -> LBMS/LABS 상태 비트를 먼저 지우고(RW1C)
 *               -> 그 뒤에 pcie_update_link_speed() 로 캐시 갱신.
 *                  순서가 반대면 지우는 사이에 일어난 변화를 놓친다
 *                  (함수 안의 영어 주석이 그 이유를 밝힌다)
 *
 * 제어: thermal 코어 또는 커널 내부
 *         -> [이 파일] pcie_set_target_speed()
 *            -> LNKCTL2 설정 -> 링크 재훈련 -> 완료 대기
 *
 * 실행 컨텍스트: 알림 처리는 전부 하드 IRQ 안이다 — config 접근과 캐시
 * 갱신만 하므로 잠들지 않는다. 속도 제어는 프로세스 컨텍스트(재훈련 대기가
 * 있다).
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
 * pcie_bwnotif_irq()          : 공유 하드 IRQ 핸들러. LNKSTA 를 읽어 자기
 *                               인터럽트인지 확인하고(아니면 IRQ_NONE),
 *                               LBMS 를 보았으면 그 사실을 priv_flags 에
 *                               기록하고, 상태 비트를 지운 뒤 속도 캐시를
 *                               갱신한다. 스레드 절반은 없다.
 * pcie_reset_lbms()           : 기록해 둔 LBMS 표시와 상태 비트를 함께 지운다.
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
	/* [한국어] 이 포트를 thermal 서브시스템에 냉각 장치로 등록한 결과.
	 * 설정자: probe 의 pcie_cooling_device_register(). 실패하면 NULL 로 둔다.
	 * 읽는 자: remove 의 등록 해제.
	 * 값 범위: 유효한 포인터 또는 NULL. 냉각 장치 등록 실패가 서비스 전체를
	 *   실패시키지는 않는다 — 대역폭 알림은 그와 무관하게 동작한다.
	 * 동기화: probe/remove 에서만 다루므로 별도 보호가 없다. */
	struct thermal_cooling_device *cdev;
};

/* Prevent port removal during Link Speed changes. */
static DECLARE_RWSEM(pcie_bwctrl_setspeed_rwsem);

/* [한국어] 인자가 유효한 PCIe 속도 열거값인지 확인한다. */
/* [한국어]
 * pcie_valid_speed - 유효한 PCIe 속도 열거값인지 확인한다
 *
 * @speed: 검사할 속도.
 * @return: true = 유효, false = 아님.
 *
 * pci_bus_speed 열거형에는 PCIe 가 아닌 값(PCI, PCI-X 계열)도 들어 있고
 * 알 수 없음을 뜻하는 값도 있다. 이 함수는 PCIe 범위만 걸러 낸다.
 *
 * 열거값이 연속으로 배치되어 있어 범위 비교 하나로 충분하다.
 *
 * pci_bus_speed2lnkctl2() 가 배열 인덱스로 쓰기 전에 이 검사를 통과시키므로,
 * 배열 범위 밖 접근을 막는 안전장치이기도 하다.
 *
 * 실행 컨텍스트: 어디서든. 산술뿐이라 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_bus_speed2lnkctl2() / pcie_set_target_speed() → [이 함수]
 */
static bool pcie_valid_speed(enum pci_bus_speed speed)
{
	/* [한국어] 열거값이 연속이라 범위 비교만으로 충분하다. */
	return (speed >= PCIE_SPEED_2_5GT) && (speed <= PCIE_SPEED_64_0GT);
}

/* [한국어] 버스 속도 열거값을 LNKCTL2 의 Target Link Speed 인코딩으로 바꾼다. */
/* [한국어]
 * pci_bus_speed2lnkctl2 - 버스 속도 열거값을 LNKCTL2 인코딩으로 바꾼다
 *
 * @speed: 커널 내부의 속도 열거값.
 * @return: LNKCTL2 의 Target Link Speed 필드에 넣을 값.
 *
 * 커널이 쓰는 열거값과 하드웨어 레지스터의 인코딩이 서로 다르므로 변환이
 * 필요하다.
 *
 * 지정 초기화 배열을 쓰는 것이 요령이다. 인덱스가 곧 열거값이라 대응이
 * 눈에 보이고, 열거형에 값이 추가되어도 빠진 인덱스는 0 으로 남아 조용히
 * 잘못된 값을 주지 않는다 — 그런 경우는 WARN_ON_ONCE 가 먼저 잡는다.
 *
 * 실행 컨텍스트: 속도 설정 경로. 잠들지 않는다.
 *
 * 에러 경로: 유효하지 않은 속도는 경고와 함께 0 을 반환한다.
 *
 * 호출 체인:
 *   pcie_bwctrl_select_speed() → [이 함수] → pcie_valid_speed()
 */
static u16 pci_bus_speed2lnkctl2(enum pci_bus_speed speed)
{
	/* [한국어] 지정 초기화 배열이라 인덱스가 곧 열거값이다. 빠진 인덱스는 0 이 되며,
	 * 위 pcie_valid_speed() 검사가 그런 값이 들어오지 않게 막는다. */
	static const u8 speed_conv[] = {
		/* [한국어] 2.5GT/s. */
		[PCIE_SPEED_2_5GT] = PCI_EXP_LNKCTL2_TLS_2_5GT,
		[PCIE_SPEED_5_0GT] = PCI_EXP_LNKCTL2_TLS_5_0GT,
		[PCIE_SPEED_8_0GT] = PCI_EXP_LNKCTL2_TLS_8_0GT,
		[PCIE_SPEED_16_0GT] = PCI_EXP_LNKCTL2_TLS_16_0GT,
		[PCIE_SPEED_32_0GT] = PCI_EXP_LNKCTL2_TLS_32_0GT,
		[PCIE_SPEED_64_0GT] = PCI_EXP_LNKCTL2_TLS_64_0GT,
	};

	/* [한국어] 유효하지 않은 속도가 들어오면 배열 범위를 벗어난다. */
	if (WARN_ON_ONCE(!pcie_valid_speed(speed)))
		return 0;

	/* [한국어] 변환된 값. */
	return speed_conv[speed];
}

/* [한국어] 지원 속도 비트맵에서 가장 높은 속도를 고른다. */
/* [한국어]
 * pcie_supported_speeds2target_speed - 지원 속도 비트맵에서 가장 높은 속도를 고른다
 *
 * @supported_speeds: 지원 속도 비트맵.
 * @return: 그중 가장 높은 속도의 LNKCTL2 인코딩.
 *
 * 한 줄짜리 함수지만 전제가 중요하다. 비트맵의 비트 순서가 속도 순서와
 * 같도록 정의되어 있어, 가장 높은 세워진 비트(__fls)가 곧 가장 빠른 속도이고
 * 그 비트 위치가 그대로 LNKCTL2 인코딩이 된다.
 *
 * 그 두 표현이 우연히 맞는 것이 아니라 규격이 그렇게 정한 것이며,
 * 그 덕분에 이 변환이 시프트 하나로 끝난다.
 *
 * 실행 컨텍스트: 속도 선택 경로. 잠들지 않는다.
 *
 * 에러 경로: 없다. 인자가 0 이면 __fls 의 결과가 정의되지 않으므로,
 * 호출자가 그런 값을 넘기지 않도록 보장해야 한다 —
 * pcie_bwctrl_select_speed() 가 그 앞에서 빈 비트맵을 최저 속도로 대체한다.
 *
 * 호출 체인:
 *   pcie_bwctrl_select_speed() → [이 함수] → __fls()
 */
static inline u16 pcie_supported_speeds2target_speed(u8 supported_speeds)
{
	/* [한국어] __fls 는 가장 높은 세워진 비트의 위치를 준다. 비트맵의 비트 순서가
	 * 속도 순서와 같도록 정의되어 있어, 가장 높은 비트가 곧 가장 빠른 속도다. */
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
/* [한국어]
 * pcie_bwctrl_select_speed - 양쪽이 지원하는 범위 안에서 실제 목표 속도를 고른다
 *
 * @port: 대상 포트.
 * @speed_req: 요청받은 속도.
 * @return: LNKCTL2 에 넣을 목표 속도 인코딩.
 *
 * 요청 속도를 그대로 쓰지 않는 이유가 이 함수의 존재 이유다. 링크는 양쪽이
 * 합의해야 성립하므로, 한쪽만 지원하는 속도를 목표로 걸면 협상이 실패한다.
 *
 * 세 단계로 좁힌다. 먼저 요청 속도 **이하** 의 모든 속도를 담은 마스크를
 * 만들고(정확히 그 속도가 아니라 "그 이하 중 가장 높은 것" 을 고르려는 것이다),
 * 포트와 링크 상대의 supported_speeds 를 AND 로 교집합을 내고, 그 교집합과
 * 요청 범위의 교집합에서 가장 높은 비트를 고른다.
 *
 * 링크 상대를 하위 버스의 **첫 장치** 로 삼는 것이 성립하는 이유는 PCIe 링크가
 * 점대점이기 때문이다. 하위 버스에 장치가 하나뿐인 것이 정상이다.
 *
 * 교집합이 비면 최저 속도로 물러난다. 하드웨어가 supported_speeds 를 보고하지
 * 않는 경우가 그에 해당하며, 안전한 쪽을 택하는 것이다.
 *
 * 실행 컨텍스트: 속도 설정 경로. pci_bus_sem 을 읽기로 잡는 구간이 있어
 * 프로세스 컨텍스트여야 한다.
 *
 * 에러 경로: 없다. 언제나 유효한 인코딩을 돌려준다.
 *
 * 호출 체인:
 *   pcie_set_target_speed() → [이 함수]
 *     → pci_bus_speed2lnkctl2() → pcie_supported_speeds2target_speed()
 */
static u16 pcie_bwctrl_select_speed(struct pci_dev *port, enum pci_bus_speed speed_req)
{
	/* [한국어] 이 포트 아래의 버스. 없을 수도 있다. */
	struct pci_bus *bus = port->subordinate;
	/* [한국어] 원하는 속도 범위와 양쪽이 지원하는 속도. */
	u8 desired_speeds, supported_speeds;
	/* [한국어] 하위 장치. */
	struct pci_dev *dev;

	/* [한국어] 요청 속도 이하의 모든 속도를 담은 마스크를 만든다. 정확히 그 속도가
	 * 아니라 "그 이하 중 가장 높은 것" 을 고르려는 것이므로 범위가 필요하다. */
	desired_speeds = GENMASK(pci_bus_speed2lnkctl2(speed_req),
				 __fls(PCI_EXP_LNKCAP2_SLS_2_5GB));

	/* [한국어] 포트가 지원하는 속도에서 시작한다. */
	supported_speeds = port->supported_speeds;
	/* [한국어] 하위 버스가 있으면, */
	if (bus) {
		down_read(&pci_bus_sem);
		/* [한국어] 그 버스의 첫 장치를 가져온다. 링크는 점대점이라 하위 버스에 장치가
		 * 하나뿐인 것이 정상이며, 첫 장치가 곧 링크 상대다. */
		dev = list_first_entry_or_null(&bus->devices, struct pci_dev, bus_list);
		/* [한국어] 장치가 있으면, */
		if (dev)
			/* [한국어] 양쪽이 모두 지원하는 속도만 남긴다. 한쪽만 지원하는 속도로 협상할 수
			 * 없기 때문이다. */
			supported_speeds &= dev->supported_speeds;
		up_read(&pci_bus_sem);
	}
	/* [한국어] 교집합이 비었으면(하드웨어가 supported_speeds 를 보고하지 않는 경우), */
	if (!supported_speeds)
		/* [한국어] 최저 속도만 지원한다고 가정한다. 안전한 쪽으로 물러나는 것이다. */
		supported_speeds = PCI_EXP_LNKCAP2_SLS_2_5GB;

	/* [한국어] 지원 교집합과 원하는 범위의 교집합에서 가장 높은 속도를 고른다. */
	return pcie_supported_speeds2target_speed(supported_speeds & desired_speeds);
}

/* [한국어]
 * pcie_bwctrl_change_speed - 목표 속도를 쓰고 링크를 재훈련한다
 *
 * @port: 대상 포트.
 * @target_speed: LNKCTL2 인코딩의 목표 속도.
 * @use_lt: 재훈련 완료를 Link Training 비트로 기다릴지.
 * @return: 0 = 성공, 그 밖에 config 접근이나 재훈련의 오류.
 *
 * 두 단계뿐이다. LNKCTL2 의 Target Link Speed 필드를 갈아 끼우고, 재훈련한다.
 *
 * clear_and_set 판을 쓰는 이유는 그 레지스터에 이퀄라이제이션 관련 필드가
 * 함께 있어 보존해야 하기 때문이다.
 *
 * 목표 속도를 쓰는 것만으로는 아무 일도 일어나지 않는다. 재훈련이 실제로
 * 링크를 다시 협상시키며, 그때 하드웨어가 이 목표를 상한으로 삼는다.
 *
 * 실행 컨텍스트: 속도 설정 경로. 재훈련 대기가 있어 잠들 수 있다.
 *
 * 에러 경로: config 접근 실패는 pcibios_err_to_errno() 로 변환해 올려보내고,
 * 재훈련 실패는 그 함수의 결과를 그대로 반환한다.
 *
 * 호출 체인:
 *   pcie_set_target_speed() → [이 함수]
 *     → pcie_capability_clear_and_set_word() → pcie_retrain_link()
 */
static int pcie_bwctrl_change_speed(struct pci_dev *port, u16 target_speed, bool use_lt)
{
	/* [한국어] 결과. */
	int ret;

	/* [한국어] LNKCTL2 의 Target Link Speed 필드만 새 값으로 갈아 끼운다. 그 레지스터의
	 * 다른 필드(이퀄라이제이션 관련)를 보존해야 하므로 clear_and_set 판을 쓴다. */
	ret = pcie_capability_clear_and_set_word(port, PCI_EXP_LNKCTL2,
						 PCI_EXP_LNKCTL2_TLS, target_speed);
	/* [한국어] config 접근이 실패하면, */
	if (ret != PCIBIOS_SUCCESSFUL)
		/* [한국어] PCIBIOS 오류를 errno 로 바꿔 올려보낸다. */
		return pcibios_err_to_errno(ret);

	/* [한국어] 목표 속도를 정했으니 실제로 재훈련시킨다. 두 단계를 나눈 것이 이 함수의
	 * 전부다 — 목표를 쓰고, 재훈련한다. */
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
/* [한국어]
 * pcie_set_target_speed - 링크 속도 상한을 정하고 재훈련시킨다
 *
 * @port: 대상 포트.
 * @speed_req: 원하는 속도.
 * @use_lt: 재훈련 완료를 Link Training 비트로 기다릴지.
 * @return: 0 = 성공, -EINVAL, -EAGAIN, 또는 config/재훈련 오류.
 *
 * thermal 냉각 장치와 커널 내부가 링크 속도를 낮추거나 되돌릴 때 부르는
 * 공개 진입점이다.
 *
 * 이미 그 속도면 곧바로 성공을 답한다. 재훈련이 링크를 잠시 끊으므로
 * 불필요하게 하지 않는다.
 *
 * 잠금이 두 겹인 것이 이 함수의 요점이다. 바깥의 rwsem 읽기 잠금은
 * port->link_bwctrl 포인터를 읽는 동안 probe/remove 가 그것을 갈아 끼우지
 * 못하게 막고, 안쪽의 뮤텍스는 여러 주체가 동시에 속도를 바꾸지 못하게 막는다.
 * 서비스가 붙지 않은 포트에서는 그 포인터가 NULL 이라 뮤텍스를 건너뛰는데,
 * 대역폭 알림 없이도 속도 설정 자체는 가능하기 때문이다.
 *
 * 마지막의 -EAGAIN 이 세밀한 처리다. 재훈련이 성공했는데도 협상된 속도가
 * 요청과 다르고 하위 버스에 장치가 있다면, 링크가 이유를 알 수 없이 다른
 * 속도로 합의한 것이므로 호출자에게 다시 시도할 여지를 준다. 장치가 없는
 * 빈 포트에서는 속도가 맞지 않는 것이 정상이라 그 경우를 제외한다.
 *
 * 실행 컨텍스트: thermal 콜백과 커널 내부. 재훈련 대기가 있어 잠들 수 있다.
 *
 * 에러 경로: 유효하지 않은 속도는 -EINVAL, 협상 결과 불일치는 -EAGAIN,
 * 그 밖은 아래 호출의 오류를 그대로 올려보낸다.
 *
 * 호출 체인:
 *   thermal 냉각 장치 / 커널 내부 → [이 함수]
 *     → pcie_bwctrl_select_speed() → pcie_bwctrl_change_speed()
 *     → pcie_retrain_link()
 */
int pcie_set_target_speed(struct pci_dev *port, enum pci_bus_speed speed_req,
			  bool use_lt)
{
	/* [한국어] 이 포트 아래의 버스. */
	struct pci_bus *bus = port->subordinate;
	/* [한국어] 실제로 설정할 속도. */
	u16 target_speed;
	/* [한국어] 결과. */
	int ret;

	/* [한국어] 유효하지 않은 속도면, */
	if (WARN_ON_ONCE(!pcie_valid_speed(speed_req)))
		return -EINVAL;

	/* [한국어] 이미 그 속도면 할 일이 없다. 재훈련은 링크를 잠시 끊으므로 불필요하게
	 * 하지 않는다. */
	if (bus && bus->cur_bus_speed == speed_req)
		return 0;

	/* [한국어] 양쪽이 지원하는 범위 안에서 실제 목표를 고른다. 요청 속도를 그대로
	 * 쓰지 않는 이유가 여기 있다. */
	target_speed = pcie_bwctrl_select_speed(port, speed_req);

	/* [한국어] 읽기 잠금을 잡는다. probe/remove 가 쓰기 잠금으로 link_bwctrl 을
	 * 갈아 끼우므로, 그 사이에 이 포인터를 읽으면 안 된다. */
	scoped_guard(rwsem_read, &pcie_bwctrl_setspeed_rwsem) {
		/* [한국어] 이 포트에 서비스가 붙어 있으면 그 상태를 얻는다. NULL 일 수 있는데,
		 * 대역폭 알림 서비스 없이도 속도 설정 자체는 가능하기 때문이다. */
		struct pcie_bwctrl_data *data = port->link_bwctrl;

		/*
		 * port->link_bwctrl is NULL during initial scan when called
		 * e.g. from the Target Speed quirk.
		 */
		if (data)
			mutex_lock(&data->set_speed_mutex);

		/* [한국어] 실제 설정. 뮤텍스 안에서 하는 이유는 여러 주체(thermal, 사용자)가
		 * 동시에 속도를 바꾸려 할 수 있기 때문이다. */
		ret = pcie_bwctrl_change_speed(port, target_speed, use_lt);

		/* [한국어] 서비스가 있었으면, */
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

/* [한국어] 이 서비스가 붙은 포트. */
/* [한국어]
 * pcie_bwnotif_enable - 대역폭 변화 알림을 켠다
 *
 * @srv: 이 서비스의 pcie_device.
 *
 * 세 단계의 순서가 이 함수의 전부다.
 *
 * 먼저 LBMS 비트가 이미 서 있는지 읽어 두고, 서 있으면 그 사실을
 * priv_flags 에 기록한다. 알림을 켜기 전에 일어난 대역폭 변화를 놓치지
 * 않으려는 것이다.
 * 그 다음 LNKCTL 의 두 허용 비트(LBMIE, LABIE)를 세워 인터럽트를 연다.
 * 마지막으로 상태 비트를 지운다.
 *
 * 지우기를 마지막에 두는 것이 아니라 **확인을 먼저** 두는 것이 요점이다.
 * 순서를 바꾸면 켜기 직전의 변화가 흔적 없이 사라진다.
 *
 * resume 경로에서도 그대로 쓰인다 — 절전 중에 링크가 바뀌었을 수 있으므로
 * 같은 확인이 필요하다.
 *
 * 실행 컨텍스트: probe 와 resume 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 상태 읽기가 실패하면 LBMS 확인만 건너뛰고 알림은 켠다.
 *
 * 호출 체인:
 *   pcie_bwnotif_probe() / pcie_bwnotif_resume() → [이 함수]
 *     → pcie_capability_read_word() → set_bit() → pcie_capability_set_word()
 */
static void pcie_bwnotif_enable(struct pcie_device *srv)
{
	struct pci_dev *port = srv->port;
	/* [한국어] 링크 상태. */
	u16 link_status;
	/* [한국어] config 접근 결과. */
	int ret;

	/* Note if LBMS has been seen so far */
	ret = pcie_capability_read_word(port, PCI_EXP_LNKSTA, &link_status);
	/* [한국어] 읽기에 성공했고 LBMS 비트가 이미 서 있으면, */
	if (ret == PCIBIOS_SUCCESSFUL && link_status & PCI_EXP_LNKSTA_LBMS)
		/* [한국어] 그 사실을 기록해 둔다. 알림을 켜기 전에 일어난 대역폭 변화를 놓치지
		 * 않으려는 것이다. */
		set_bit(PCI_LINK_LBMS_SEEN, &port->priv_flags);

	/* [한국어] LNKCTL 의 두 알림 허용 비트를 세운다. 이 시점부터 인터럽트가 들어온다. */
	pcie_capability_set_word(port, PCI_EXP_LNKCTL,
				 PCI_EXP_LNKCTL_LBMIE | PCI_EXP_LNKCTL_LABIE);
	/* [한국어] 상태 비트를 지운다. 켜는 순서가 중요한데, 지우기 **전에** 위에서
	 * LBMS 를 확인해 두었으므로 정보를 잃지 않는다. */
	pcie_capability_write_word(port, PCI_EXP_LNKSTA,
				   PCI_EXP_LNKSTA_LBMS | PCI_EXP_LNKSTA_LABS);

	/*
	 * Update after enabling notifications & clearing status bits ensures
	 * link speed is up to date.
	 */
	pcie_update_link_speed(port->subordinate, PCIE_BWCTRL_ENABLE);
}

/* [한국어] 두 알림 허용 비트를 지운다. 상태 비트는 건드리지 않는데, 다시 켤 때
 * enable 쪽이 확인하고 지우기 때문이다. */
/* [한국어]
 * pcie_bwnotif_disable - 대역폭 변화 알림을 끈다
 *
 * @port: 대상 포트.
 *
 * LNKCTL 의 두 허용 비트를 지우는 것이 전부다.
 *
 * 상태 비트를 건드리지 않는 점이 pcie_bwnotif_enable() 과 비대칭이다.
 * 다시 켤 때 enable 쪽이 그것을 확인하고 지우므로, 여기서 지우면 그 사이의
 * 정보를 잃는다. 절전 중에 링크 속도가 바뀐 경우가 그 시나리오다.
 *
 * 실행 컨텍스트: remove 와 suspend 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pcie_bwnotif_remove() / pcie_bwnotif_suspend() → [이 함수]
 *     → pcie_capability_clear_word()
 */
static void pcie_bwnotif_disable(struct pci_dev *port)
{
	pcie_capability_clear_word(port, PCI_EXP_LNKCTL,
				   PCI_EXP_LNKCTL_LBMIE | PCI_EXP_LNKCTL_LABIE);
}

/* [한국어]
 * pcie_bwnotif_irq - 대역폭 변화 인터럽트를 처리한다 (스레드 없음)
 *
 * @irq: 인터럽트 번호. 쓰지 않는다.
 * @context: 등록 시 넘겨 둔 pcie_device.
 * @return: IRQ_HANDLED 또는 IRQ_NONE.
 *
 * 공유 인터럽트 핸들러이며 스레드 절반이 없다 — request_irq 에 스레드 함수를
 * 주지 않으므로, 이 함수가 하드 IRQ 문맥에서 전부 처리한다. 하는 일이
 * config 접근 몇 번과 캐시 갱신뿐이라 그래도 된다.
 *
 * 우리 인터럽트인지 판정하는 것이 먼저다. LNKSTA 를 읽어 LBMS 나 LABS 가
 * 서 있지 않으면 IRQ_NONE 을 돌려주어야 커널이 다른 핸들러를 시도한다.
 *
 * 순서가 이 함수의 핵심이다. 상태 비트를 **먼저** 지우고 그 다음에 속도를
 * 다시 읽는다. 함수 안의 영어 주석이 이유를 밝힌다 — LBMS 를 지우기 전에는
 * 추가 속도 변화가 새 인터럽트를 만들지 않으므로, 지운 뒤에 읽어야 그 사이의
 * 변화를 놓치지 않는다.
 *
 * LBMS 를 보았다는 사실은 priv_flags 에 따로 기록한다. 링크 훈련이 실제로
 * 일어났다는 증거로 다른 코드가 참고한다.
 *
 * 실행 컨텍스트: 하드 IRQ. 잠들 수 없다.
 *
 * 에러 경로: config 읽기 실패는 IRQ_NONE 으로 답한다 — 장치가 사라졌을 수
 * 있으므로 우리 인터럽트라고 주장하지 않는다.
 *
 * 호출 체인:
 *   하드웨어 인터럽트 → [이 함수]
 *     → pcie_capability_read_word() → set_bit() → pcie_capability_write_word()
 *     → pcie_update_link_speed()
 */
static irqreturn_t pcie_bwnotif_irq(int irq, void *context)
{
	/* [한국어] 등록 시 넘겨 둔 서비스 객체. */
	struct pcie_device *srv = context;
	/* [한국어] 이 서비스가 붙은 포트. */
	struct pci_dev *port = srv->port;
	/* [한국어] 링크 상태와 그중 우리가 볼 이벤트. */
	u16 link_status, events;
	/* [한국어] config 접근 결과. */
	int ret;

	/* [한국어] 링크 상태를 읽는다. */
	ret = pcie_capability_read_word(port, PCI_EXP_LNKSTA, &link_status);
	/* [한국어] 읽기가 실패하면 장치가 사라졌을 수 있으므로, */
	if (ret != PCIBIOS_SUCCESSFUL)
		return IRQ_NONE;

	/* [한국어] 대역폭 관리(LBMS)와 자동 대역폭(LABS) 두 비트만 본다. */
	events = link_status & (PCI_EXP_LNKSTA_LBMS | PCI_EXP_LNKSTA_LABS);
	/* [한국어] 둘 다 서 있지 않으면 우리 인터럽트가 아니다. 공유 인터럽트이므로
	 * IRQ_NONE 을 돌려주어야 커널이 다른 핸들러를 시도한다. */
	if (!events)
		return IRQ_NONE;

	/* [한국어] LBMS 를 보았으면, */
	if (events & PCI_EXP_LNKSTA_LBMS)
		/* [한국어] 그 사실을 기록해 둔다. 링크 훈련이 실제로 일어났다는 증거로,
		 * 다른 코드가 이 플래그를 본다. */
		set_bit(PCI_LINK_LBMS_SEEN, &port->priv_flags);

	/* [한국어] 상태 비트를 지운다. 함수 안의 영어 주석대로 이것을 **먼저** 해야 한다 —
	 * 지우기 전에는 속도 변화가 있어도 새 인터럽트가 오지 않으므로, 지운 뒤에
	 * 속도를 다시 읽어야 그 사이의 변화를 놓치지 않는다. */
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

/* [한국어]
 * pcie_reset_lbms - 기록해 둔 LBMS 표시와 하드웨어 상태 비트를 함께 지운다
 *
 * @port: 대상 포트.
 *
 * LBMS(Link Bandwidth Management Status)는 링크가 다시 훈련되었음을 뜻한다.
 * 그 사실을 하드웨어 비트와 소프트웨어 플래그 두 곳에 기록해 두므로,
 * 지울 때도 두 곳을 함께 지워야 어긋나지 않는다.
 *
 * 바깥으로 열린 함수라, 링크 훈련을 유발한 쪽(예: 속도 변경이나 리셋)이
 * 그 흔적을 정리할 때 부른다.
 *
 * 실행 컨텍스트: 호출자에 따라 다르다. config 쓰기와 비트 조작뿐이라
 * 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   링크 훈련을 유발한 코드 → [이 함수]
 *     → clear_bit() → pcie_capability_write_word()
 */
void pcie_reset_lbms(struct pci_dev *port)
{
	/* [한국어] 기록해 둔 LBMS 표시를 지우고, */
	clear_bit(PCI_LINK_LBMS_SEEN, &port->priv_flags);
	/* [한국어] 하드웨어 상태 비트도 지운다. 두 곳을 함께 지워야 소프트웨어와 하드웨어가
	 * 어긋나지 않는다. */
	pcie_capability_write_word(port, PCI_EXP_LNKSTA, PCI_EXP_LNKSTA_LBMS);
}

/* [한국어]
 * pcie_bwnotif_probe - 포트에 대역폭 알림 서비스를 붙인다
 *
 * @srv: 이 서비스의 pcie_device.
 * @return: 0 = 성공, -ENODEV, -ENOMEM, 또는 request_irq() 의 오류.
 *
 * 순서와 잠금이 이 함수의 요점이다.
 *
 * 먼저 no_bw_notif 쿼크를 확인한다. 알림이 신뢰할 수 없는 하드웨어가 있어,
 * 그런 포트에서는 아예 붙지 않는다.
 *
 * 상태를 할당하고 뮤텍스를 초기화한 뒤, 쓰기 잠금 아래에서 세 가지를 한다 —
 * port->link_bwctrl 에 상태를 매달고, 인터럽트를 걸고, 알림을 켠다.
 * 그 대입이 곧 "서비스가 붙었다" 는 표시이고, pcie_set_target_speed() 가
 * 읽기 잠금 아래에서 그것을 읽으므로 두 잠금이 짝을 이룬다.
 * IRQ 등록이 실패하면 방금 매단 포인터를 도로 지워 절반만 붙은 상태를
 * 남기지 않는다.
 *
 * 냉각 장치 등록은 잠금 밖에서, 그리고 **실패해도 오류로 반환하지 않고**
 * NULL 로 둔다. 부가 기능이라 그것 없이도 대역폭 알림은 동작하기 때문이다.
 *
 * 실행 컨텍스트: 포트 서비스 probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 쿼크는 -ENODEV, 할당 실패는 -ENOMEM, IRQ 등록 실패는 그 오류.
 * devm 할당이라 되돌릴 것은 포인터 대입뿐이다.
 *
 * 호출 체인:
 *   포트 서비스 코어 → [이 함수]
 *     → devm_kzalloc() → devm_mutex_init() → request_irq()
 *     → pcie_bwnotif_enable() → pcie_cooling_device_register()
 */
static int pcie_bwnotif_probe(struct pcie_device *srv)
{
	/* [한국어] 이 서비스가 붙을 포트. */
	struct pci_dev *port = srv->port;
	/* [한국어] 결과. */
	int ret;

	/* [한국어] 이 포트에서 대역폭 알림을 쓰지 않기로 표시되어 있으면(쿼크 등), */
	if (port->no_bw_notif)
		return -ENODEV;

	/* Can happen if we run out of bus numbers during enumeration. */
	if (!port->subordinate)
		return -ENODEV;

	/* [한국어] 서비스 상태를 devm 으로 할당한다. */
	struct pcie_bwctrl_data *data = devm_kzalloc(&srv->device,
						     sizeof(*data), GFP_KERNEL);
	/* [한국어] 실패하면, */
	if (!data)
		return -ENOMEM;

	/* [한국어] 속도 설정 뮤텍스를 초기화한다. devm 판이라 해제가 자동이다. */
	ret = devm_mutex_init(&srv->device, &data->set_speed_mutex);
	/* [한국어] 실패하면, */
	if (ret)
		return ret;

	/* [한국어] 쓰기 잠금을 잡는다. 아래에서 link_bwctrl 을 채우는 동안
	 * pcie_set_target_speed() 가 그것을 읽으면 안 되기 때문이다. */
	scoped_guard(rwsem_write, &pcie_bwctrl_setspeed_rwsem) {
		/* [한국어] 포트에 서비스 상태를 매단다. 이 대입이 곧 "서비스가 붙었다" 는 표시다. */
		port->link_bwctrl = data;

		/* [한국어] 인터럽트를 건다. */
		ret = request_irq(srv->irq, pcie_bwnotif_irq,
				  IRQF_SHARED, "PCIe bwctrl", srv);
		/* [한국어] 실패하면, */
		if (ret) {
			/* [한국어] 방금 매단 포인터를 도로 지운다. 절반만 붙은 상태를 남기지 않기 위해서다. */
			port->link_bwctrl = NULL;
			return ret;
		}

		pcie_bwnotif_enable(srv);
	}

	/* [한국어] 어느 IRQ 로 알림을 받는지 남긴다. */
	pci_dbg(port, "enabled with IRQ %d\n", srv->irq);

	/* Don't fail on errors. Don't leave IS_ERR() "pointer" into ->cdev */
	port->link_bwctrl->cdev = pcie_cooling_device_register(port);
	/* [한국어] 냉각 장치 등록이 실패하면, */
	if (IS_ERR(port->link_bwctrl->cdev))
		/* [한국어] NULL 로 둔다. 오류로 반환하지 않는 것이 중요한데, 냉각 장치는 부가
		 * 기능이고 대역폭 알림 자체는 그것 없이도 동작하기 때문이다. */
		port->link_bwctrl->cdev = NULL;

	return 0;
}

/* [한국어]
 * pcie_bwnotif_remove - 대역폭 알림 서비스를 뗀다
 *
 * @srv: 이 서비스의 pcie_device.
 *
 * probe 의 역순이되 순서에 이유가 있다.
 *
 * 냉각 장치를 잠금 밖에서 먼저 해제한다. thermal 서브시스템이 그 과정에서
 * 속도 설정을 시도할 수 있는데, 잠금 안에서 하면 교착하기 때문으로 보인다.
 *
 * 그 다음 쓰기 잠금 아래에서 알림을 끄고, 인터럽트를 떼고, 마지막으로
 * 포인터를 지운다. free_irq() 가 진행 중인 핸들러가 끝날 때까지 기다려
 * 주므로, 그 **뒤에** 포인터를 지우면 핸들러가 NULL 을 보는 일이 없다.
 *
 * 실행 컨텍스트: 포트 서비스 remove. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   포트 서비스 코어 → [이 함수]
 *     → pcie_cooling_device_unregister() → pcie_bwnotif_disable()
 *     → free_irq()
 */
static void pcie_bwnotif_remove(struct pcie_device *srv)
{
	/* [한국어] 이 서비스의 상태. */
	struct pcie_bwctrl_data *data = srv->port->link_bwctrl;

	pcie_cooling_device_unregister(data->cdev);

	/* [한국어] 쓰기 잠금을 잡는다. 아래 세 단계가 하나의 원자적 해제여야 한다. */
	scoped_guard(rwsem_write, &pcie_bwctrl_setspeed_rwsem) {
		pcie_bwnotif_disable(srv->port);

		/* [한국어] 인터럽트를 뗀다. 진행 중인 핸들러가 끝날 때까지 기다려 준다. */
		free_irq(srv->irq, srv);

		/* [한국어] 마지막으로 포인터를 지운다. 인터럽트를 뗀 **뒤에** 지우는 순서라,
		 * 핸들러가 도는 동안 NULL 을 보는 일이 없다. */
		srv->port->link_bwctrl = NULL;
	}
}

/* [한국어]
 * pcie_bwnotif_suspend - 절전 진입 시 알림을 끈다
 *
 * @srv: 이 서비스의 pcie_device.
 * @return: 언제나 0.
 *
 * 알림만 끄고 인터럽트는 그대로 둔다. 절전 중에는 어차피 인터럽트가 오지
 * 않으며, 복귀 시 다시 걸 필요가 없어 간단해진다.
 *
 * 상태 비트를 지우지 않는 것이 pcie_bwnotif_disable() 의 설계와 맞물린다 —
 * 절전 중에 링크가 바뀌었다면 그 흔적이 남아, 복귀 시 resume 이 그것을
 * 알아챈다.
 *
 * 실행 컨텍스트: 절전 진입. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   포트 서비스 코어의 suspend → [이 함수] → pcie_bwnotif_disable()
 */
static int pcie_bwnotif_suspend(struct pcie_device *srv)
{
	pcie_bwnotif_disable(srv->port);
	return 0;
}

/* [한국어]
 * pcie_bwnotif_resume - 절전 복귀 시 알림을 다시 켠다
 *
 * @srv: 이 서비스의 pcie_device.
 * @return: 언제나 0.
 *
 * pcie_bwnotif_suspend() 의 짝이며, probe 가 쓰는 것과 같은 enable 함수를
 * 그대로 쓴다.
 *
 * 그 함수가 알림을 켜기 전에 LBMS 를 먼저 확인하는 덕분에, 절전 중에 링크
 * 속도가 바뀌었더라도 그 사실이 기록된다. suspend 가 상태 비트를 지우지
 * 않은 것이 여기서 값을 한다.
 *
 * 실행 컨텍스트: 절전 복귀. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   포트 서비스 코어의 resume → [이 함수] → pcie_bwnotif_enable()
 */
static int pcie_bwnotif_resume(struct pcie_device *srv)
{
	pcie_bwnotif_enable(srv);
	return 0;
}

static struct pcie_port_service_driver pcie_bwctrl_driver = {
	/* [한국어] sysfs 와 로그에 보일 서비스 이름. */
	.name		= "pcie_bwctrl",
	.port_type	= PCIE_ANY_PORT,
	.service	= PCIE_PORT_SERVICE_BWCTRL,
	.probe		= pcie_bwnotif_probe,
	.suspend	= pcie_bwnotif_suspend,
	.resume		= pcie_bwnotif_resume,
	.remove		= pcie_bwnotif_remove,
};

/* [한국어]
 * pcie_bwctrl_init - 대역폭 제어 서비스를 포트 서비스로 등록한다
 *
 * @return: pcie_port_service_register() 의 결과. 0 = 성공.
 *
 * 이 파일의 유일한 초기화 진입점이다. 등록이 끝나면 대역폭 알림을 지원하는
 * 포트마다 pcie_bwnotif_probe() 가 불린다.
 *
 * __init 라 부팅 후 해제된다. AER 이나 PME 와 달리 CONFIG_ 조건으로 감싸여
 * 있지 않아 언제나 빌드에 들어간다 — portdrv.h 의 다른 네 서비스가
 * #ifdef/#else 스텁 쌍을 갖는 것과 대비된다.
 *
 * 실행 컨텍스트: PCIe 포트 드라이버 초기화. 프로세스 컨텍스트.
 *
 * 에러 경로: 등록 실패를 그대로 올려보낸다.
 *
 * 호출 체인:
 *   pcie_portdrv_init() → [이 함수] → pcie_port_service_register()
 */
int __init pcie_bwctrl_init(void)
{
	/* [한국어] 포트 서비스로 등록한다. 이 호출이 끝나면 대역폭 알림을 지원하는
	 * 포트마다 probe 가 불린다. */
	return pcie_port_service_register(&pcie_bwctrl_driver);
}
