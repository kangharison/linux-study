// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Linaro Ltd.
 */

/*
 * [한국어 설명] 전원 시퀀서에 전원 제어를 위임하는 pwrctrl 드라이버 (pci-pwrctrl-pwrseq.c)
 *
 * === 파일의 역할 ===
 * pwrctrl 프레임워크와 커널의 pwrseq(power sequencer) 서브시스템을 잇는
 * 얇은 다리다.
 *
 * 배경을 알아야 한다. 어떤 칩은 전원을 넣는 순서가 까다롭다 — 여러 레귤레이터를
 * 정해진 순서와 간격으로 켜고, 클럭을 넣고, 리셋을 특정 시점에 풀어야 한다.
 * 그리고 그 칩 하나가 여러 인터페이스로 노출되는 경우가 있다. 퀄컴의 WCN
 * 계열 콤보 칩이 그렇다 — 하나의 칩에 Wi-Fi(PCIe)와 Bluetooth(UART)가 함께
 * 들어 있고, 둘이 같은 전원 레일을 공유한다.
 *
 * 그러면 문제가 생긴다. PCIe 쪽 드라이버가 전원을 끄면 UART 쪽 Bluetooth 도
 * 죽는다. 그래서 전원 제어를 어느 한쪽 드라이버가 소유하면 안 되고,
 * 별도의 중재자가 참조 카운트로 관리해야 한다. 그것이 pwrseq 서브시스템이다.
 *
 * 이 파일이 하는 일은 그 위임뿐이다 — pwrctrl 의 power_on/power_off 콜백을
 * pwrseq_power_on/off 로 넘긴다. 실제 순서와 타이밍은 pwrseq 쪽 provider 가
 * 알고 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * DT 에 이 칩이 기술되어 있으면
 *   -> pwrctrl/core.c 가 platform device 를 만들고
 *      -> 드라이버 코어가 compatible("pci17cb,1101" 등)로 이 드라이버를 바인딩
 *         -> [이 파일] pwrseq_pwrctrl_probe()
 *            -> devm_pwrseq_get() 으로 시퀀서 핸들 획득
 *            -> pwrctrl 콜백을 자기 함수로 채우고
 *            -> devm_pci_pwrctrl_device_set_ready() 로 코어에 알린다
 *               -> 버스 재스캔 -> Wi-Fi 장치 발견 -> ath11k/ath12k 바인딩
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe). pwrseq 동작이 잠들 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: 플랫폼 드라이버 코어, pwrctrl/core.c.
 * 아래쪽: drivers/power/sequencing/ 의 pwrseq 서브시스템.
 * 옆쪽: 같은 칩의 Bluetooth 드라이버 — 같은 시퀀서를 공유하며,
 *   참조 카운트 덕분에 둘 중 하나가 살아 있는 동안 전원이 유지된다.
 *
 * === NVMe 관점 ===
 * NVMe 와는 관련이 없다. 이 드라이버가 담당하는 것은 퀄컴 WCN 계열
 * Wi-Fi 칩 세 종뿐이며(아래 of_match 표 참고), NVMe 컨트롤러는 없다.
 *
 * 학습 관점에서 볼 만한 것은 "여러 인터페이스가 하나의 전원을 공유할 때"
 * 의 해법이다. NVMe 에는 그런 구조가 없지만(하나의 컨트롤러가 PCIe 하나로만
 * 노출된다), 임베디드에서는 흔한 문제다.
 *
 * === 주요 함수/구조체 요약 ===
 * pwrseq_pwrctrl_probe()          : 시퀀서 핸들을 얻고 pwrctrl 에 등록한다.
 * pwrseq_pwrctrl_power_on/off()   : pwrctrl 콜백. pwrseq 로 그대로 위임한다.
 * pwrseq_pwrctrl_qcm_wcn_validate_device() : 옛 DT 를 걸러내는 검사.
 *                                   자세한 이유는 그 함수의 주석 참고.
 * struct pwrseq_pwrctrl           : pwrctrl 상태 + 시퀀서 핸들.
 * struct pwrseq_pwrctrl_pdata     : 칩 종류별 데이터(시퀀서 대상 이름과 검사 함수).
 * pwrseq_pwrctrl_of_match[]       : 담당할 DT compatible 목록.
 */

#include <linux/device.h>		/* [한국어] struct device, device_get_match_data */
#include <linux/mod_devicetable.h>	/* [한국어] struct of_device_id — DT 매칭 표 */
#include <linux/module.h>		/* [한국어] MODULE_* 매크로와 module_platform_driver */
#include <linux/pci-pwrctrl.h>		/* [한국어] struct pci_pwrctrl 과 그 등록 함수들 */
#include <linux/platform_device.h>	/* [한국어] struct platform_driver/platform_device */
#include <linux/property.h>		/* [한국어] device_property_present — DT 속성 존재 확인 */
#include <linux/pwrseq/consumer.h>	/* [한국어] pwrseq_get/power_on/power_off — 이 파일의 핵심 의존 */
#include <linux/slab.h>			/* [한국어] devm_kzalloc */
#include <linux/types.h>		/* [한국어] 기본 타입 */

/* [한국어] 이 드라이버의 인스턴스 하나가 들고 있는 상태.
 * pwrctrl 을 첫 필드로 두어 container_of 로 서로를 오갈 수 있게 한 것이
 * 커널의 관용적인 확장 방식이다 — pwrctrl 코어는 struct pci_pwrctrl 만
 * 알고, 콜백에서 그 포인터로 바깥 구조체를 되찾는다. */
struct pwrseq_pwrctrl {
	struct pci_pwrctrl pwrctrl;
	/* [한국어] 전원 시퀀서 핸들.
	 * 설정자: probe 의 devm_pwrseq_get().
	 * 읽는 자: power_on/power_off 콜백.
	 * 값 범위: 유효한 핸들. 획득 실패는 IS_ERR 로 판정하며 probe 에서 걸러진다.
	 * 동기화: pwrseq 서브시스템이 자체 참조 카운트와 락으로 보호한다.
	 *   그래서 이 드라이버가 별도 락을 잡지 않는다. */
	struct pwrseq_desc *pwrseq;
};

/* [한국어] 칩 종류별로 달라지는 것을 모아 둔 데이터. DT 매칭 표의 .data 로
 * 연결되어, probe 가 device_get_match_data() 로 꺼내 쓴다.
 * 이렇게 하면 칩이 늘어나도 probe 코드를 고칠 필요가 없다. */
struct pwrseq_pwrctrl_pdata {
	/* [한국어] pwrseq 서브시스템에 요청할 "대상" 이름.
	 * 하나의 시퀀서가 여러 소비자를 가질 수 있어(Wi-Fi 와 Bluetooth),
	 * 어느 쪽으로 요청하는지를 이 문자열로 구분한다.
	 * 설정자: 각 pdata 의 정적 초기화("wlan").
	 * 읽는 자: probe 의 devm_pwrseq_get(). */
	const char *target;
	/*
	 * Called before doing anything else to perform device-specific
	 * verification between requesting the power sequencing handle.
	 */
	/* [한국어] 시퀀서를 요청하기 전에 이 장치가 정말 대상인지 확인하는 콜백.
	 * NULL 이면 검사를 건너뛴다.
	 * 설정자: 각 pdata 의 정적 초기화.
	 * 읽는 자: probe. 0 이 아닌 값을 돌려주면 그대로 probe 실패다.
	 * 왜 필요한가: 아래 qcm_wcn 판의 주석 참고 — 옛 DT 를 걸러내야 한다. */
	int (*validate_device)(struct device *dev);
};

/*
 * [한국어]
 * pwrseq_pwrctrl_qcm_wcn_validate_device - 옛 DT 노드를 걸러낸다
 *
 * @dev:    검사할 장치
 * @return: 0 = 이 드라이버가 담당해도 되는 노드,
 *          -ENODEV = 담당하면 안 되는 옛 노드.
 *
 * 아래 원문 주석이 문제를 설명한다. 요약하면 — 전원 시퀀싱이 커널에
 * 들어오기 전부터 있던 DT 들이 이 칩의 Wi-Fi 노드를 이미 갖고 있는데,
 * 그 노드들은 PMU 의 레귤레이터 출력을 소비하지 않는다. 즉 전원 정보가
 * 없다.
 *
 * 그런 노드에 이 드라이버가 바인딩되면 devm_pwrseq_get() 이 영원히
 * -EPROBE_DEFER 를 돌려주고, 커널 로그에 "무한 probe 지연" 오류가 쌓인다.
 * 장치는 어차피 동작하지 않는데 로그만 더러워지는 것이다.
 *
 * 그래서 모든 WCN 모델이 갖는 vddaon-supply 속성이 있는지 먼저 본다.
 * 그 속성이 있으면 전원 정보를 갖춘 새 DT 이고, 없으면 옛 DT 다.
 * "존재 여부만" 확인하고 값은 보지 않는다는 점에 주의 — 판별이 목적이지
 * 실제 사용이 아니기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe 초반).
 * 호출자: pwrseq_pwrctrl_probe() 가 pdata->validate_device 를 통해 부른다.
 */
static int pwrseq_pwrctrl_qcm_wcn_validate_device(struct device *dev)
{
	/*
	 * Old device trees for some platforms already define wifi nodes for
	 * the WCN family of chips since before power sequencing was added
	 * upstream.
	 *
	 * These nodes don't consume the regulator outputs from the PMU, and
	 * if we allow this driver to bind to one of such "incomplete" nodes,
	 * we'll see a kernel log error about the indefinite probe deferral.
	 *
	 * Check the existence of the regulator supply that exists on all
	 * WCN models before moving forward.
	 */
	/* [한국어] 속성의 존재만 확인한다. 값을 읽지 않는 이유는 이것이
	 * "새 DT 인가" 를 가르는 표지일 뿐이고, 실제 전원 정보는 pwrseq
	 * provider 가 따로 갖고 있기 때문이다. */
	if (!device_property_present(dev, "vddaon-supply"))
		return -ENODEV;

	return 0;	/* [한국어] 담당해도 되는 노드다 */
}

/* [한국어] 퀄컴 WCN 계열 세 모델이 공유하는 데이터.
 * 시퀀서 대상은 "wlan"(같은 칩의 Bluetooth 는 다른 이름으로 요청한다),
 * 검사 함수는 위의 옛 DT 걸러내기다. */
static const struct pwrseq_pwrctrl_pdata pwrseq_pwrctrl_qcom_wcn_pdata = {
	.target = "wlan",
	.validate_device = pwrseq_pwrctrl_qcm_wcn_validate_device,
};

/*
 * [한국어]
 * pwrseq_pwrctrl_power_on - pwrctrl 의 전원 켜기 요청을 시퀀서로 넘긴다
 *
 * @pwrctrl: pwrctrl 코어가 넘겨준 포인터. 우리 구조체의 첫 필드다.
 * @return:  0 = 성공, 음수 = 시퀀서가 실패를 알림.
 *
 * 실제 순서와 타이밍은 pwrseq provider 가 알고 있으므로 이 함수는
 * 위임만 한다. 여러 소비자가 같은 시퀀서를 공유하면 pwrseq 가 참조
 * 카운트를 올려, 이미 켜져 있으면 아무것도 하지 않고 성공을 돌려준다.
 *
 * container_of 로 바깥 구조체를 되찾는 것이 이 함수의 유일한 기교다 —
 * pwrctrl 코어는 struct pci_pwrctrl 만 알고 우리 구조체를 모르기 때문이다.
 *
 * 실행 컨텍스트: pwrctrl 코어가 부르는 문맥. 프로세스 컨텍스트이며
 *   시퀀서 동작이 잠들 수 있다.
 * 호출자: pwrctrl 코어.
 */
static int pwrseq_pwrctrl_power_on(struct pci_pwrctrl *pwrctrl)
{
	/* [한국어] pwrctrl 이 우리 구조체의 첫 필드이므로 주소가 같지만,
	 * container_of 를 쓰는 것이 규약이다 — 필드 순서가 바뀌어도 안전하다. */
	struct pwrseq_pwrctrl *pwrseq = container_of(pwrctrl,
					   struct pwrseq_pwrctrl, pwrctrl);

	/* [한국어] 시퀀서에게 켜기를 요청한다. 이미 켜져 있으면(다른 소비자가
	 * 쓰고 있으면) 참조 카운트만 올라가고 0 이 돌아온다. */
	return pwrseq_power_on(pwrseq->pwrseq);
}

/*
 * [한국어]
 * pwrseq_pwrctrl_power_off - pwrctrl 의 전원 끄기 요청을 시퀀서로 넘긴다
 *
 * @pwrctrl: pwrctrl 코어가 넘겨준 포인터.
 * @return:  0 = 성공, 음수 = 실패.
 *
 * power_on 의 짝. 참조 카운트가 남아 있으면(같은 칩의 Bluetooth 가 아직
 * 쓰고 있으면) 시퀀서가 실제로 끄지 않고 카운트만 줄인다. 그 판단을
 * 이 드라이버가 하지 않아도 되는 것이 pwrseq 를 쓰는 이유다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: pwrctrl 코어.
 */
static int pwrseq_pwrctrl_power_off(struct pci_pwrctrl *pwrctrl)
{
	struct pwrseq_pwrctrl *pwrseq = container_of(pwrctrl,
					   struct pwrseq_pwrctrl, pwrctrl);

	/* [한국어] 끄기를 요청한다. 다른 소비자가 남아 있으면 실제로 끄지 않고
	 * 참조 카운트만 내린다. */
	return pwrseq_power_off(pwrseq->pwrseq);
}

/*
 * [한국어]
 * pwrseq_pwrctrl_probe - 시퀀서 핸들을 얻어 pwrctrl 에 등록한다
 *
 * @pdev:   pwrctrl 코어가 만든 platform device
 * @return: 0 = 성공, 음수 errno = 실패(그 경우 devres 가 자동 정리한다).
 *
 * 다섯 단계다.
 *   1) DT 매칭에서 칩별 데이터를 꺼낸다.
 *   2) 그 데이터의 검사 함수로 이 노드가 담당 대상인지 확인한다.
 *   3) 상태 구조체를 잡는다.
 *   4) 전원 시퀀서 핸들을 얻는다. 시퀀서 provider 가 아직 준비되지
 *      않았으면 -EPROBE_DEFER 가 돌아오고, 커널이 나중에 다시 부른다.
 *   5) 콜백을 채워 pwrctrl 코어에 등록한다. 등록 순간 코어가 버스
 *      재스캔을 예약해 장치가 열거된다.
 *
 * 에러 정리 코드가 없다는 점이 눈에 띈다. 모든 자원이 devm_ 로 잡혀 있어
 * 실패하면 커널이 역순으로 알아서 되돌린다(devres.c 의 헤더 참고).
 * 그래서 각 실패 지점에서 그냥 return 하면 된다.
 *
 * dev_err_probe() 를 쓰는 것도 관용구다. -EPROBE_DEFER 일 때는 로그를
 * 찍지 않고(정상적인 재시도이므로), 다른 오류일 때만 메시지를 남긴다.
 * 그러지 않으면 부팅 로그가 지연 메시지로 뒤덮인다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 여러 지점에서 잠들 수 있다.
 * 호출자: 플랫폼 드라이버 코어.
 */
static int pwrseq_pwrctrl_probe(struct platform_device *pdev)
{
	const struct pwrseq_pwrctrl_pdata *pdata;	/* [한국어] 칩별 데이터 */
	struct pwrseq_pwrctrl *pwrseq;			/* [한국어] 이 인스턴스의 상태 */
	struct device *dev = &pdev->dev;		/* [한국어] devm_ 계열이 요구하는 device */
	int ret;

	/* [한국어] 1단계 — DT 매칭 표의 .data 를 꺼낸다.
	 * 표에 없는 장치가 여기 올 수는 없지만, target 이 비어 있으면
	 * 아래 devm_pwrseq_get() 이 무엇을 요청할지 알 수 없으므로 함께 막는다. */
	pdata = device_get_match_data(dev);
	if (!pdata || !pdata->target)
		return -EINVAL;

	/* [한국어] 2단계 — 칩별 검사. 옛 DT 노드를 걸러낸다.
	 * 콜백이 없는 칩도 있을 수 있어 NULL 검사를 먼저 한다. */
	if (pdata->validate_device) {
		ret = pdata->validate_device(dev);
		if (ret)
			return ret;	/* [한국어] 검사가 준 errno 를 그대로 올린다 */
	}

	/* [한국어] 3단계 — 상태 구조체. devm_ 이라 드라이버가 떨어질 때 자동 해제된다. */
	pwrseq = devm_kzalloc(dev, sizeof(*pwrseq), GFP_KERNEL);
	if (!pwrseq)
		return -ENOMEM;

	/* [한국어] 4단계 — 시퀀서 핸들 획득. pdata->target("wlan")으로
	 * 어느 소비자로 요청하는지 알린다. */
	pwrseq->pwrseq = devm_pwrseq_get(dev, pdata->target);
	/* [한국어] 실패는 NULL 이 아니라 ERR_PTR 로 온다. IS_ERR 로 판정하고
	 * PTR_ERR 로 errno 를 꺼낸다.
	 * dev_err_probe 는 -EPROBE_DEFER 면 조용히 넘어가고 다른 오류만 찍는다 —
	 * 시퀀서 provider 가 아직 로드되지 않은 것은 흔한 정상 상황이다. */
	if (IS_ERR(pwrseq->pwrseq))
		return dev_err_probe(dev, PTR_ERR(pwrseq->pwrseq),
				     "Failed to get the power sequencer\n");

	/* [한국어] 5단계 — pwrctrl 코어가 부를 콜백을 채운다.
	 * 이 두 함수가 요청을 시퀀서로 넘긴다. */
	pwrseq->pwrctrl.power_on = pwrseq_pwrctrl_power_on;
	pwrseq->pwrctrl.power_off = pwrseq_pwrctrl_power_off;

	/* [한국어] pwrctrl 상태를 초기화한다(워크큐 준비 등). */
	pci_pwrctrl_init(&pwrseq->pwrctrl, dev);

	/* [한국어] 코어에 "준비됐다" 고 알린다. 이 호출이 버스 재스캔을 예약하고,
	 * 그 결과로 장치가 열거되어 진짜 드라이버(ath11k 등)가 붙는다.
	 * devm_ 판이라 드라이버가 떨어질 때 자동으로 해제된다. */
	ret = devm_pci_pwrctrl_device_set_ready(dev, &pwrseq->pwrctrl);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to register the pwrctrl wrapper\n");

	return 0;
}

/* [한국어] 이 드라이버가 담당할 DT compatible 목록.
 * "pci17cb,NNNN" 형식은 PCI 장치를 DT 로 기술할 때의 관례로,
 * 17cb 는 퀄컴의 PCI 벤더 ID 이고 뒤 네 자리가 device ID 다.
 * of_property.c 의 of_pci_prop_compatible() 이 이 형식을 만든다.
 *
 * 세 항목이 모두 같은 pdata 를 가리킨다 — 패키지는 달라도 전원 시퀀싱
 * 요구가 같기 때문이다. 마지막 { } 는 배열의 끝 표시다. */
static const struct of_device_id pwrseq_pwrctrl_of_match[] = {
	{
		/* ATH11K in QCA6390 package. */
		.compatible = "pci17cb,1101",
		.data = &pwrseq_pwrctrl_qcom_wcn_pdata,
	},
	{
		/* ATH11K in WCN6855 package. */
		.compatible = "pci17cb,1103",
		.data = &pwrseq_pwrctrl_qcom_wcn_pdata,
	},
	{
		/* ATH12K in WCN7850 package. */
		.compatible = "pci17cb,1107",
		.data = &pwrseq_pwrctrl_qcom_wcn_pdata,
	},
	{ }
};
/* [한국어] 이 표를 모듈 메타데이터로 내보낸다. udev/modprobe 가 그것을 읽어
 * 해당 장치가 나타났을 때 이 모듈을 자동으로 로드한다. */
MODULE_DEVICE_TABLE(of, pwrseq_pwrctrl_of_match);

/* [한국어] 플랫폼 드라이버 정의. remove 콜백이 없는 것이 눈에 띄는데,
 * 모든 자원을 devm_ 로 잡아 커널이 자동으로 정리하기 때문이다. */
static struct platform_driver pwrseq_pwrctrl_driver = {
	.driver = {
		.name = "pci-pwrctrl-pwrseq",
		.of_match_table = pwrseq_pwrctrl_of_match,
	},
	.probe = pwrseq_pwrctrl_probe,
};
/* [한국어] module_init/module_exit 과 등록/해제 코드를 한 줄로 만들어 주는
 * 매크로. 특별한 초기화가 없는 드라이버의 표준 관용구다. */
module_platform_driver(pwrseq_pwrctrl_driver);

MODULE_AUTHOR("Bartosz Golaszewski <bartosz.golaszewski@linaro.org>");
MODULE_DESCRIPTION("Generic PCI Power Control module for power sequenced devices");
MODULE_LICENSE("GPL");
