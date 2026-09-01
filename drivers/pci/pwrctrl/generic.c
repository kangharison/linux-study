// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Linaro Ltd.
 * Author: Manivannan Sadhasivam <manivannan.sadhasivam@linaro.org>
 */

/*
 * [한국어 설명] DeviceTree 서술만으로 동작하는 범용 전원 제어 드라이버 (pwrctrl/generic.c)
 *
 * === 파일의 역할 ===
 * PCIe 슬롯에 전원을 넣는 가장 단순한 경우를 처리한다. DT 에 레귤레이터
 * 목록이 적혀 있으면 그것을 순서대로 켜고, pwrctrl 코어에 "준비됐다" 고
 * 알리는 것이 전부다.
 *
 * 특별한 순서 제약이나 클럭 조작이 필요한 보드는 자기 전용 드라이버를
 * 쓰지만(예: 같은 디렉터리의 tc9563), 단순히 전원만 넣으면 되는 보드는
 * 이 드라이버 하나로 충분하다. DT 의 compatible 문자열로 매칭된다.
 *
 * devm_ 계열(devres)을 적극적으로 쓴다는 점이 눈에 띈다. 레귤레이터 획득,
 * 활성화, pwrctrl 등록이 모두 devres 로 관리되어, probe 가 실패하거나
 * 드라이버가 떨어질 때 커널이 역순으로 알아서 되돌린다. 그래서 이 파일에는
 * 명시적인 에러 정리 코드가 거의 없다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * pwrctrl/core.c 가 DT 를 보고 만든 platform device
 *   -> 드라이버 코어가 compatible 로 이 드라이버를 바인딩
 *      -> [이 파일] pci_pwrctrl_generic_probe()
 *         -> devm_regulator_bulk_get_enable() 로 레귤레이터를 켜고
 *         -> devm_pci_pwrctrl_device_set_ready() 로 코어에 알린다
 *            -> 코어가 버스 재스캔을 예약 -> 장치 발견
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: 플랫폼 드라이버 코어.
 * 아래쪽: pwrctrl/core.c 의 인프라, regulator 서브시스템.
 * 공유 상태: struct pci_pwrctrl 하나.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버와 직접 관련이 없다(전수 확인).
 *
 * 임베디드 보드에 NVMe 를 붙였고 그 슬롯의 전원이 DT 에 단순 레귤레이터로
 * 기술돼 있다면, 이 드라이버가 전원을 넣은 뒤에야 NVMe 가 열거된다.
 * 자세한 흐름은 pwrctrl/core.c 의 헤더 참고.
 *
 * (기존 주석은 이 드라이버가 "전원/클록/리셋 시퀀스" 를 제어한다고 적었으나,
 *  이 파일이 실제로 다루는 것은 레귤레이터뿐이다. 클럭과 리셋을 다루는
 *  것은 같은 디렉터리의 보드 전용 드라이버들이다.)
 *
 * === 주요 함수/구조체 요약 ===
 * pci_pwrctrl_generic_probe()  : DT 의 레귤레이터 목록을 켜고 코어에 알린다.
 *                                devres 덕분에 정리 코드가 필요 없다.
 * pci_pwrctrl_generic_dt_ids[] : 이 드라이버가 담당할 DT compatible 목록.
 * pci_pwrctrl_generic_driver   : 플랫폼 드라이버 구조체.
 */

/* [한국어] clk_prepare_enable()/clk_disable_unprepare() 와 devm_clk_get_optional(). */
#include <linux/clk.h>
/* [한국어] struct device 와 dev_err/dev_of_node. */
#include <linux/device.h>
/* [한국어] struct of_device_id 정의. */
#include <linux/mod_devicetable.h>
/* [한국어] MODULE_* 매크로와 module_platform_driver. */
#include <linux/module.h>
/* [한국어] of_graph_is_present() — DT 노드에 그래프(remote-endpoint) 서술이 있는지 본다.
 * 이 드라이버는 그 유무로 전원 시퀀서 방식과 레귤레이터 방식을 가른다. */
#include <linux/of_graph.h>
/* [한국어] struct pci_pwrctrl 과 pci_pwrctrl_init(), devm_pci_pwrctrl_device_set_ready() —
 * PCI 전원 제어 프레임워크와의 접점이다. */
#include <linux/pci-pwrctrl.h>
/* [한국어] platform_driver 정의. */
#include <linux/platform_device.h>
/* [한국어] pwrseq_power_on()/off() 와 devm_pwrseq_get() — 전원 시퀀서 프레임워크.
 * 여러 전원을 정해진 순서와 지연으로 켜야 하는 장치를 위한 추상화다. */
#include <linux/pwrseq/consumer.h>
/* [한국어] regulator_bulk_enable()/disable() 과 of_regulator_bulk_get_all(). */
#include <linux/regulator/consumer.h>
/* [한국어] kzalloc 계열. */
#include <linux/slab.h>

struct slot_pwrctrl {
	/* [한국어] PCI 전원 제어 프레임워크가 요구하는 객체. 포인터가 아니라 값으로 내장되어
	 * 있어 두 콜백이 container_of 로 이 구조체를 되찾을 수 있다.
	 * 설정자: probe 가 power_on/power_off 콜백을 걸고 pci_pwrctrl_init() 이
	 *   나머지를 채운다.
	 * 읽는 자: PCI 코어가 슬롯 전원을 켜고 끌 때.
	 * 값 범위: 구조체 내장.
	 * 동기화: 프레임워크가 관리한다. */
	struct pci_pwrctrl pwrctrl;
	/* [한국어] 이 슬롯에 전원을 공급하는 레귤레이터들의 bulk 배열.
	 * 설정자: probe 가 of_regulator_bulk_get_all() 로 DT 에 적힌 것을 통째로 가져온다.
	 * 읽는 자: power_on/off 가 regulator_bulk_enable/disable 에 그대로 넘긴다.
	 * 값 범위: 유효 포인터. 전원 시퀀서 방식일 때는 채우지 않아 NULL 로 남는다.
	 * 동기화: 레귤레이터 프레임워크가 처리한다. */
	struct regulator_bulk_data *supplies;
	/* [한국어] 위 배열의 원소 수.
	 * 설정자: of_regulator_bulk_get_all() 의 반환값.
	 * 읽는 자: bulk enable/disable/free 의 개수 인자.
	 * 값 범위: 0 이상. 시퀀서 방식이면 0 으로 남는다.
	 * 동기화: 설정 후 읽기 전용. */
	int num_supplies;
	/* [한국어] 슬롯에 공급할 레퍼런스 클럭. optional 이라 없을 수 있다.
	 * 설정자: probe 의 devm_clk_get_optional().
	 * 읽는 자: power_on 이 켜고 power_off 가 끈다.
	 * 값 범위: 유효 포인터 또는 NULL(클럭이 없는 슬롯).
	 * 동기화: 클럭 프레임워크가 처리한다. */
	struct clk *clk;
	/* [한국어] 전원 시퀀서 서술자. 이것이 NULL 이 아니면 레귤레이터·클럭 대신
	 * 시퀀서가 전원을 관리한다.
	 * 설정자: probe 가 DT 에 그래프 서술이 있을 때만 devm_pwrseq_get() 으로 얻는다.
	 * 읽는 자: power_on/off 가 맨 먼저 이 값을 확인해 경로를 가른다.
	 * 값 범위: 유효 포인터 또는 NULL.
	 * 동기화: 시퀀서 프레임워크가 처리한다. */
	struct pwrseq_desc *pwrseq;
};

/* [한국어]
 * slot_pwrctrl_power_on - 슬롯에 전원과 클럭을 공급한다
 *
 * @pwrctrl: 프레임워크가 주는 pwrctrl 객체. container_of 로 이 드라이버의
 *       struct slot_pwrctrl 을 되찾는다.
 * @return: 0 = 성공, 음수 = 레귤레이터 또는 클럭 활성화 실패.
 *
 * 두 가지 전원 관리 방식을 하나의 콜백으로 다룬다.
 *   - 전원 시퀀서 방식: pwrseq 가 NULL 이 아니면 시퀀서에 전부 맡긴다. 여러
 *     전원의 순서와 지연이 시퀀서 서술 안에 이미 들어 있어 이 드라이버가
 *     관여할 것이 없다.
 *   - 레귤레이터 방식: DT 에 적힌 전원을 bulk 로 한 번에 켠 뒤 클럭을 켠다.
 *     순서가 중요하다 — 전원이 없는 상태에서 클럭을 넣으면 장치가 비정상
 *     동작할 수 있다.
 *
 * [상류 코드 관찰, 수정하지 않음] 시퀀서 갈래가 pwrseq_power_on() 의 반환값을
 * 버리고 무조건 0 을 돌려준다. 시퀀서가 전원을 못 켜도 PCI 코어는 성공으로 알게 된다.
 *
 * 실행 컨텍스트: PCI 코어의 슬롯 전원 제어 경로, 프로세스 컨텍스트.
 * 레귤레이터와 클럭 호출이 잠들 수 있다.
 *
 * 에러 경로: 레귤레이터 실패 시 클럭을 켜지 않고 곧장 반환한다. 클럭 실패는
 * 그 반환값이 그대로 나가며, 이미 켠 레귤레이터를 되돌리지 않는다 —
 * 호출자가 실패 시 power_off 를 부른다는 전제다.
 *
 * 호출 체인:
 *   PCI 코어의 슬롯 전원 요청 → pci_pwrctrl.power_on == [이 함수]
 *     → pwrseq_power_on() 또는 regulator_bulk_enable() → clk_prepare_enable()
 */
static int slot_pwrctrl_power_on(struct pci_pwrctrl *pwrctrl)
{
	/* [한국어] 프레임워크가 주는 pwrctrl 포인터에서 이 드라이버의 구조체를 되찾는다.
	 * pwrctrl 이 값으로 내장되어 있어 성립하는 관용이다. */
	struct slot_pwrctrl *slot = container_of(pwrctrl,
						struct slot_pwrctrl, pwrctrl);
	/* [한국어] 각 단계 결과. */
	int ret;

	/* [한국어] 전원 시퀀서 방식이면, */
	if (slot->pwrseq) {
		/* [한국어] 시퀀서에 전부 맡긴다. 여러 전원의 순서와 지연이 시퀀서 서술 안에
		 * 이미 들어 있으므로 이 드라이버가 관여할 것이 없다. */
		pwrseq_power_on(slot->pwrseq);
		/* [한국어] 반환값을 확인하지 않고 성공으로 답한다.
		 * [상류 코드 관찰] pwrseq_power_on() 은 int 를 돌려주는데 그 값을 버린다.
		 * 시퀀서가 전원을 못 켜도 PCI 코어는 성공으로 알게 된다. */
		return 0;
	}

	/* [한국어] 레귤레이터 방식이면 DT 에 적힌 전원을 한 번에 켠다. bulk API 를 쓰면
	 * 여러 레귤레이터의 순서와 실패 되감기를 프레임워크가 대신 처리해 준다. */
	ret = regulator_bulk_enable(slot->num_supplies, slot->supplies);
	/* [한국어] 실패 검사. */
	if (ret < 0) {
		/* [한국어] 실패 로그. */
		dev_err(slot->pwrctrl.dev, "Failed to enable slot regulators\n");
		/* [한국어] 오류 전달 — 이 경우 클럭은 켜지 않는다. */
		return ret;
	}

	/* [한국어] 전원이 들어온 뒤에 클럭을 켠다. 순서가 중요하다 — 전원이 없는 상태에서
	 * 클럭을 넣으면 장치가 비정상 동작할 수 있다. 클럭이 없는 슬롯이면
	 * clk 가 NULL 이고 이 호출이 조용히 0 을 돌려준다. */
	return clk_prepare_enable(slot->clk);
}

/* [한국어]
 * slot_pwrctrl_power_off - 슬롯의 전원과 클럭을 차단한다
 *
 * @pwrctrl: 프레임워크가 주는 pwrctrl 객체.
 * @return: 언제나 0.
 *
 * power_on 과 같은 두 갈래 구조다. 시퀀서 방식이면 시퀀서에 맡기고,
 * 레귤레이터 방식이면 전원을 끈 뒤 클럭을 끈다.
 *
 * 언제나 0 을 돌려주는 이유는 regulator_bulk_disable() 과
 * clk_disable_unprepare() 가 둘 다 실패를 의미 있게 알릴 수 없기 때문이다 —
 * 전원 차단은 되돌릴 수 없는 단방향 작업이다.
 *
 * [상류 코드 관찰, 수정하지 않음] power_on 이 전원 → 클럭 순서인데 이쪽도
 * 전원 → 클럭 순서다. 엄밀한 역순이라면 클럭을 먼저 꺼야 한다.
 * 또 시퀀서 갈래가 pwrseq_power_off() 의 반환값을 버리는 것도 power_on 과 같다.
 *
 * 실행 컨텍스트: PCI 코어의 슬롯 전원 제어 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   PCI 코어의 슬롯 전원 차단 요청 → pci_pwrctrl.power_off == [이 함수]
 *     → pwrseq_power_off() 또는 regulator_bulk_disable() → clk_disable_unprepare()
 */
static int slot_pwrctrl_power_off(struct pci_pwrctrl *pwrctrl)
{
	/* [한국어] power_on 과 같은 방식으로 구조체를 되찾는다. */
	struct slot_pwrctrl *slot = container_of(pwrctrl,
						struct slot_pwrctrl, pwrctrl);

	/* [한국어] 시퀀서 방식이면, */
	if (slot->pwrseq) {
		/* [한국어] 시퀀서에 맡긴다. */
		pwrseq_power_off(slot->pwrseq);
		/* [한국어] 역시 반환값을 확인하지 않는다. */
		return 0;
	}

	/* [한국어] 레귤레이터를 끈다. */
	regulator_bulk_disable(slot->num_supplies, slot->supplies);
	/* [한국어] 클럭을 끈다.
	 * [상류 코드 관찰] power_on 은 전원 → 클럭 순서인데 power_off 도
	 * 전원 → 클럭 순서다. 역순이라면 클럭을 먼저 꺼야 하지만 그렇게 하지 않는다. */
	clk_disable_unprepare(slot->clk);

	/* [한국어] 언제나 성공을 돌려준다. 두 해제 호출 모두 실패를 알릴 수 없기 때문이다. */
	return 0;
}

/* [한국어]
 * devm_slot_pwrctrl_release - devm 액션으로 등록되는 레귤레이터 해제 콜백
 *
 * @data: devm_add_action_or_reset() 에 넘긴 불투명 포인터. 실제로는
 *       struct slot_pwrctrl 이다.
 *
 * of_regulator_bulk_get_all() 은 devm 판이 아니라서 잡은 레귤레이터가 자동으로
 * 해제되지 않는다. 그래서 이 콜백을 devm 액션으로 등록해 자동 해제를 얹는다 —
 * 그 덕분에 이 드라이버에 remove 콜백이 아예 없어도 된다.
 *
 * 전원 시퀀서 방식일 때도 이 액션이 등록되지만, num_supplies 가 0 이라
 * regulator_bulk_free() 가 아무 일도 하지 않는다. 분기 없이 항상 등록해
 * 코드를 단순하게 유지하는 선택이다.
 *
 * 실행 컨텍스트: 디바이스 해제 경로(probe 실패 또는 언바인드), 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   드라이버 코어의 devm 정리 → [이 함수] → regulator_bulk_free()
 */
static void devm_slot_pwrctrl_release(void *data)
{
	/* [한국어] devm 액션이 넘겨 준 불투명 포인터를 되돌린다. */
	struct slot_pwrctrl *slot = data;

	/* [한국어] of_regulator_bulk_get_all() 이 잡은 레귤레이터들을 해제한다.
	 * 그 함수가 devm 판이 아니라 자동 해제되지 않으므로, 이 액션으로 얹는다.
	 * 시퀀서 방식이면 num_supplies 가 0 이라 아무 일도 하지 않는다. */
	regulator_bulk_free(slot->num_supplies, slot->supplies);
}

/* [한국어]
 * slot_pwrctrl_probe - 두 가지 전원 관리 방식 중 하나를 준비해 프레임워크에 등록한다
 *
 * @pdev: DT 로 매칭된 플랫폼 디바이스.
 * @return: 0 = 성공. -ENOMEM = 할당 실패. 그 밖의 음수 = 자원 획득 또는 등록 실패
 *       (-EPROBE_DEFER 포함).
 *
 * 이 드라이버의 핵심은 DT 서술을 보고 전원 관리 방식을 스스로 고르는 것이다.
 *
 *   - DT 노드에 그래프(remote-endpoint) 서술이 있으면 전원 시퀀서 방식이다.
 *     그래프가 전원 공급 장치와의 연결을 나타내기 때문이며, 그 경우 "pcie"
 *     시퀀서만 얻고 레귤레이터와 클럭은 아예 건드리지 않는다(skip_resources).
 *   - 없으면 레귤레이터 방식이다. DT 에 적힌 전원을 이름 지정 없이 통째로
 *     가져오고(슬롯마다 필요한 전원 종류가 달라 드라이버가 미리 알 수 없다),
 *     선택적 레퍼런스 클럭을 얻는다.
 *
 * 두 갈래가 skip_resources 라벨에서 합류해 공통 마무리를 한다 — 콜백 두 개를
 * 걸고, 레귤레이터 해제 액션을 등록하고, 프레임워크 객체를 초기화한 뒤
 * devm_pci_pwrctrl_device_set_ready() 로 준비 완료를 알린다. 그 시점부터
 * PCI 코어가 슬롯 전원이 필요할 때 콜백을 부른다.
 *
 * [상류 코드 관찰, 수정하지 않음] 클럭 획득 실패의 로그 문구가
 * "Failed to enable slot clock" 인데, 이 단계는 켜는 것이 아니라 얻는 것이다.
 *
 * 실행 컨텍스트: 드라이버 코어의 probe 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 모든 지점이 곧장 return 한다. 자원이 전부 devm 또는 devm 액션이라
 * 되감기 코드가 필요 없고, 그래서 이 드라이버에는 remove 콜백도 없다.
 *
 * 호출 체인:
 *   DT 매칭 → platform_driver.probe == [이 함수]
 *     → of_graph_is_present() → devm_pwrseq_get()
 *   또는 of_regulator_bulk_get_all() → devm_clk_get_optional()
 *     → devm_add_action_or_reset() → pci_pwrctrl_init()
 *     → devm_pci_pwrctrl_device_set_ready()
 */
static int slot_pwrctrl_probe(struct platform_device *pdev)
{
	/* [한국어] 할당할 드라이버 상태. */
	struct slot_pwrctrl *slot;
	/* [한국어] 로그와 devm 의 기준 디바이스. */
	struct device *dev = &pdev->dev;
	/* [한국어] 각 단계 결과. */
	int ret;

	/* [한국어] 상태 구조체를 0 초기화 할당한다. pwrseq 와 supplies 가 NULL 로,
	 * num_supplies 가 0 으로 시작하는 것이 아래 분기와 정리 로직의 전제다. */
	slot = devm_kzalloc(dev, sizeof(*slot), GFP_KERNEL);
	/* [한국어] 메모리 부족. */
	if (!slot)
		/* [한국어] -ENOMEM. */
		return -ENOMEM;

	/* [한국어] DT 노드에 그래프(remote-endpoint) 서술이 있으면 전원 시퀀서 방식이다.
	 * 그래프가 전원 공급 장치와의 연결을 나타내기 때문이며, 이 유무 하나로
	 * 두 전원 관리 방식이 갈린다. */
	if (of_graph_is_present(dev_of_node(dev))) {
		/* [한국어] "pcie" 이름의 전원 시퀀서를 얻는다. */
		slot->pwrseq = devm_pwrseq_get(dev, "pcie");
		/* [한국어] 획득 실패. */
		if (IS_ERR(slot->pwrseq))
			/* [한국어] dev_err_probe 는 -EPROBE_DEFER 일 때 로그를 억제하고 인자로 받은 errno 를
			 * 그대로 돌려주는 편의 함수다 — 시퀀서가 아직 준비되지 않았을 때
			 * 부팅 로그가 오류로 채워지는 것을 막는다. */
			return dev_err_probe(dev, PTR_ERR(slot->pwrseq),
				     "Failed to get the power sequencer\n");

		/* [한국어] 시퀀서가 전원을 다 관리하므로 레귤레이터와 클럭은 얻지 않는다. */
		goto skip_resources;
	}

	/* [한국어] 레귤레이터 방식이면 DT 에 적힌 전원을 전부 가져온다. 이름을 하나하나
	 * 지정하지 않고 통째로 받는 것은, 슬롯마다 필요한 전원 종류가 달라
	 * 드라이버가 미리 알 수 없기 때문이다. */
	ret = of_regulator_bulk_get_all(dev, dev_of_node(dev),
					&slot->supplies);
	/* [한국어] 실패 검사. 반환값이 개수이므로 음수만 오류다. */
	if (ret < 0)
		/* [한국어] 오류를 로그와 함께 전달한다. */
		return dev_err_probe(dev, ret, "Failed to get slot regulators\n");

	/* [한국어] 얻은 개수를 기록한다. */
	slot->num_supplies = ret;

	/* [한국어] 레퍼런스 클럭을 얻는다. optional 판이라 DT 에 없으면 NULL 이 정상이다. */
	slot->clk = devm_clk_get_optional(dev, NULL);
	/* [한국어] NULL 은 정상이지만 ERR_PTR 은 실제 오류다. */
	if (IS_ERR(slot->clk))
		/* [한국어] [상류 코드 관찰] 로그 문구가 "Failed to enable slot clock" 인데
		 * 이 단계는 켜는 것이 아니라 얻는 것이다. 문구와 동작이 어긋난다. */
		return dev_err_probe(dev, PTR_ERR(slot->clk),
				     "Failed to enable slot clock\n");

/* [한국어] 두 방식이 다시 합류하는 지점. */
skip_resources:
	/* [한국어] 전원 인가 콜백을 건다. */
	slot->pwrctrl.power_on = slot_pwrctrl_power_on;
	/* [한국어] 전원 차단 콜백을 건다. 두 콜백이 각자 pwrseq 유무를 다시 확인하므로,
	 * 여기서는 방식을 구분하지 않고 같은 함수를 건다. */
	slot->pwrctrl.power_off = slot_pwrctrl_power_off;

	/* [한국어] 레귤레이터 해제 액션을 등록한다. 시퀀서 방식일 때도 등록하지만
	 * num_supplies 가 0 이라 아무 일도 하지 않는다. */
	ret = devm_add_action_or_reset(dev, devm_slot_pwrctrl_release, slot);
	/* [한국어] 등록 실패는 _or_reset 판이 이미 콜백을 실행한 뒤라는 뜻이므로, */
	if (ret)
		/* [한국어] 오류만 돌려주면 된다. */
		return ret;

	/* [한국어] 프레임워크 객체를 초기화한다. 이 호출 전에 콜백을 걸어 두어야 한다. */
	pci_pwrctrl_init(&slot->pwrctrl, dev);

	/* [한국어] 이 전원 제어기가 준비되었음을 PCI 코어에 알린다. 이 시점부터 코어가
	 * 슬롯 전원이 필요할 때 위 두 콜백을 부른다. devm 판이라 해제도 자동이다. */
	ret = devm_pci_pwrctrl_device_set_ready(dev, &slot->pwrctrl);
	/* [한국어] 등록 실패. */
	if (ret)
		/* [한국어] 오류를 로그와 함께 전달한다. */
		return dev_err_probe(dev, ret, "Failed to register pwrctrl driver\n");

	/* [한국어] probe 성공. */
	return 0;
}

static const struct of_device_id slot_pwrctrl_of_match[] = {
	{
		/* [한국어] PCI 클래스 코드 0x0604(PCI-to-PCI 브리지)에 매칭된다. compatible 이
		 * "pciclass," 로 시작하는 것은 DT 가 특정 장치가 아니라 클래스 전체를
		 * 가리키는 방식이며, 그래서 이 드라이버가 "generic" 이다. */
		.compatible = "pciclass,0604",
	},
	/* Renesas UPD720201/UPD720202 USB 3.0 xHCI Host Controller */
	{
		/* [한국어] Renesas UPD720201/UPD720202 USB 3.0 xHCI 컨트롤러(위 상류 주석).
		 * 브리지가 아닌 개별 장치인데도 이 드라이버가 필요한 예다. */
		.compatible = "pci1912,0014",
	},
	/* [한국어] 테이블 끝을 알리는 빈 항목. */
	{ }
};
/* [한국어] 모듈 자동 로딩을 위해 매칭 테이블을 내보낸다. */
MODULE_DEVICE_TABLE(of, slot_pwrctrl_of_match);

static struct platform_driver slot_pwrctrl_driver = {
	.driver = {
		/* [한국어] 드라이버 이름. */
		.name = "pci-pwrctrl-slot",
		/* [한국어] 위에서 정의한 DT 매칭 테이블. */
		.of_match_table = slot_pwrctrl_of_match,
	},
	/* [한국어] 장치가 나타났을 때 불릴 진입점. remove 콜백이 없는데, 모든 자원이
	 * devm 또는 devm 액션이라 드라이버 코어가 알아서 되돌리기 때문이다. */
	.probe = slot_pwrctrl_probe,
};
/* [한국어] module_init/module_exit 보일러플레이트. */
module_platform_driver(slot_pwrctrl_driver);

/* [한국어] modinfo 에 표시될 작성자. */
MODULE_AUTHOR("Manivannan Sadhasivam <manivannan.sadhasivam@linaro.org>");
/* [한국어] modinfo 에 표시될 설명. */
MODULE_DESCRIPTION("Generic PCI Power Control driver for PCI Slots");
/* [한국어] 라이선스 선언. */
MODULE_LICENSE("GPL");
