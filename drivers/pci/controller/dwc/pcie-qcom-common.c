// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

/*
 * [한국어 설명] 퀄컴 PCIe 컨트롤러들의 링크 품질 튜닝 (pcie-qcom-common.c)
 *
 * === 파일의 역할 ===
 * 퀄컴은 DesignWare IP 를 호스트(pcie-qcom.c)로도 엔드포인트(pcie-qcom-ep.c)
 * 로도 쓴다. 그 둘이 똑같이 해야 하는 물리 계층 설정이 있어서 이 파일로
 * 뽑아냈다. 200줄 정도의 작은 파일이고 함수도 둘뿐이다.
 *
 * 다루는 것이 이퀄라이제이션(equalization)과 레인 마진(lane margining)이다.
 * 둘 다 신호 품질에 관한 것이다.
 *
 * 이퀄라이제이션은 Gen3 이상에서 필수다. 8GT/s 를 넘으면 기판의 배선이
 * 신호를 뭉개기 때문에, 송신 쪽이 미리 왜곡을 보정해 보내야 수신 쪽에서
 * 제대로 읽힌다. 그 보정 계수를 정하는 과정이 이퀄라이제이션이고,
 * 링크 트레이닝 중에 양쪽이 주고받으며 맞춘다. 이 파일은 그 시작점이 될
 * preset 값을 하드웨어에 넣는다.
 *
 * 레인 마진은 진단 기능이다. 수신 쪽에서 판정 시점이나 전압 기준을 일부러
 * 조금씩 밀어 보며 어디까지 견디는지 재는 것이다. 여유가 얼마나 있는지
 * 알 수 있어서, 링크가 불안정할 때 원인을 좁히는 데 쓴다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * pcie-qcom.c (RC) 또는 pcie-qcom-ep.c (EP) 의 초기화
 *   -> [이 파일] qcom_pcie_common_set_equalization()
 *   -> [이 파일] qcom_pcie_common_set_16gt_lane_margining()
 *      -> pcie-designware.c 의 DBI 접근으로 레지스터에 쓴다
 *
 * 실행 컨텍스트: 초기화 시점의 프로세스 컨텍스트. 링크가 올라오기 전에
 * 설정해야 하므로 순서가 중요하다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pcie-qcom.c, pcie-qcom-ep.c.
 * 아래쪽: pcie-designware.c 의 레지스터 접근 함수.
 * 공유 상태: struct dw_pcie 하나만 받아서 그 DBI 를 통해 쓴다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 부르지 않는다(전수 확인).
 *
 * 다만 관계는 실질적이다. 퀄컴 플랫폼에서 NVMe 를 Gen4 로 붙였는데
 * 링크가 Gen1 으로 떨어지거나 AER 에 수정 가능 오류가 계속 쌓인다면,
 * 물리 계층 설정이 원인일 수 있고 그 설정이 여기서 이뤄진다.
 * 이퀄라이제이션 preset 이 그 보드의 배선에 맞지 않으면 링크가
 * 트레이닝에 실패해 낮은 속도로 물러난다.
 *
 * === 주요 함수/구조체 요약 ===
 * qcom_pcie_common_set_equalization()      : Gen3/Gen4 이퀄라이제이션
 *                          preset 을 하드웨어에 넣는다. 링크 트레이닝 전에.
 * qcom_pcie_common_set_16gt_lane_margining(): Gen4(16GT/s)의 레인 마진
 *                          기능을 켜고 파라미터를 설정한다.
 */

#include <linux/pci.h>

#include "pcie-designware.h"
#include "pcie-qcom-common.h"

/* [한국어]
 * qcom_pcie_common_set_equalization - Gen3 이상 각 속도의 이퀄라이제이션 파라미터를 조정한다
 *
 * @pci: DWC 인스턴스. max_link_speed 와 DBI 접근에 쓰인다.
 * @return: 없음.
 *
 * 8GT/s(Gen3)부터는 링크 학습 중에 송수신 이퀄라이저를 맞추는 단계가 있고,
 * 그 동작을 좌우하는 파라미터가 DWC 의 GEN3_* 레지스터들에 있다. Qualcomm
 * 플랫폼에서 기본값으로는 학습이 불안정해 이 함수가 값을 덮어쓴다.
 *
 * 루프가 8GT/s 부터 max_link_speed 까지 도는 것이 핵심이다. 이 레지스터들은
 * **속도마다 별도의 그림자(shadow) 복사본**을 갖고 있어, 어느 복사본을
 * 건드릴지 GEN3_RELATED_OFF 의 RATE_SHADOW_SEL 로 먼저 고른 뒤 나머지
 * 레지스터를 쓴다. 그래서 속도 하나마다 세 레지스터를 한 벌로 다룬다.
 *   shadow 인덱스 = speed - PCIE_SPEED_8_0GT (8GT/s 가 0)
 *
 * 32GT/s 를 넘는 속도는 이 IP 가 다루지 않으므로 경고 후 루프를 끊는다.
 *
 * 세 레지스터가 하는 일:
 *  - GEN3_RELATED_OFF: ZRXDC_NONCOMPL 를 끄고(비규격 수신 종단 동작 해제)
 *    그림자 선택자를 설정한다.
 *  - GEN3_EQ_FB_MODE_DIR_CHANGE_OFF: 피드백 모드의 평가 횟수와 커서 변경
 *    상한을 정한다(값 0x1/0xd/0x5/0x5).
 *  - GEN3_EQ_CONTROL_OFF: 피드백 모드, phase 2/3 종료 조건, 초기 평가 FOM
 *    사용, 프리셋 요청 벡터를 **모두 지운다** -- 하드웨어 기본 동작으로
 *    되돌린다는 뜻이다.
 *
 * 이 함수는 DBI 쓰기 허용 구간 안에서 불려야 한다. 호출자(pcie-qcom.c,
 * pcie-qcom-ep.c)가 그것을 보장한다.
 *
 * 실행 컨텍스트: 링크가 서기 전, 프로브·재개의 프로세스 문맥.
 *
 * 호출 체인:
 *   qcom_pcie_*_post_init 계열 → [이 함수] → dw_pcie_readl_dbi/writel_dbi
 */
void qcom_pcie_common_set_equalization(struct dw_pcie *pci)
{
	struct device *dev = pci->dev;
	u32 reg;
	/* [한국어] 루프 변수. 열거형 pci_bus_speed 값을 담지만 u16 으로 선언되어 있다(상류 그대로). */
	u16 speed;
	/*
	 * GEN3_RELATED_OFF register is repurposed to apply equalization
	 * settings at various data transmission rates through registers namely
	 * GEN3_EQ_*. The RATE_SHADOW_SEL bit field of GEN3_RELATED_OFF
	 * determines the data rate for which these equalization settings are
	 * applied.
	 */

	for (speed = PCIE_SPEED_8_0GT; speed <= pcie_get_link_speed(pci->max_link_speed); speed++) {
		/* [한국어] 이 IP 가 다루는 그림자 복사본은 32GT/s 까지다. 그 위 속도는 설정할 자리가
		 * 없으므로 루프를 끊는다. */
		if (speed > PCIE_SPEED_32_0GT) {
			/* [한국어] 조용히 건너뛰면 왜 이퀄라이제이션이 적용되지 않았는지 알 수 없으므로 경고한다. */
			dev_warn(dev, "Skipped equalization settings for unsupported data rate\n");
			/* [한국어] 더 높은 속도도 마찬가지이므로 continue 가 아니라 break 다. */
			break;
		}

		reg = dw_pcie_readl_dbi(pci, GEN3_RELATED_OFF);
		/* [한국어] ZRXDC_NONCOMPL 을 끈다 -- 비규격 수신 종단 동작을 쓰지 않겠다는 뜻이다. */
		reg &= ~GEN3_RELATED_OFF_GEN3_ZRXDC_NONCOMPL;
		/* [한국어] 그림자 선택자 필드를 비운다. 아래에서 이번 속도의 인덱스를 넣는다. */
		reg &= ~GEN3_RELATED_OFF_RATE_SHADOW_SEL_MASK;
		/* [한국어] **이 쓰기가 이후 두 레지스터의 대상을 정한다.** GEN3_EQ_* 레지스터는 속도마다
		 * 별도 복사본을 갖고 있어, 어느 것을 건드릴지 여기서 먼저 고른다. */
		reg |= FIELD_PREP(GEN3_RELATED_OFF_RATE_SHADOW_SEL_MASK,
			  /* [한국어] 8GT/s 를 0 으로 하는 인덱스. 그래서 16GT/s 는 1, 32GT/s 는 2 가 된다. */
			  speed - PCIE_SPEED_8_0GT);
		dw_pcie_writel_dbi(pci, GEN3_RELATED_OFF, reg);

		reg = dw_pcie_readl_dbi(pci, GEN3_EQ_FB_MODE_DIR_CHANGE_OFF);
		/* [한국어] 네 필드를 모두 비운다. 기본값이 하드웨어 판본마다 달라 지운 뒤 새로 넣는다. */
		reg &= ~(GEN3_EQ_FMDC_T_MIN_PHASE23 |
			/* [한국어] 평가 횟수 필드. */
			GEN3_EQ_FMDC_N_EVALS |
			/* [한국어] 프리커서 변경 상한 필드. */
			GEN3_EQ_FMDC_MAX_PRE_CURSOR_DELTA |
			/* [한국어] 포스트커서 변경 상한 필드. */
			GEN3_EQ_FMDC_MAX_POST_CURSOR_DELTA);
		reg |= FIELD_PREP(GEN3_EQ_FMDC_T_MIN_PHASE23, 0x1) |
			/* [한국어] 평가 횟수 0xd. 이퀄라이저가 몇 번 시도하고 결정할지를 정한다. */
			FIELD_PREP(GEN3_EQ_FMDC_N_EVALS, 0xd) |
			/* [한국어] 프리커서 변경 상한 0x5 -- 한 번에 계수를 얼마나 크게 바꿀 수 있는지. */
			FIELD_PREP(GEN3_EQ_FMDC_MAX_PRE_CURSOR_DELTA, 0x5) |
			/* [한국어] 포스트커서 변경 상한도 같은 0x5. */
			FIELD_PREP(GEN3_EQ_FMDC_MAX_POST_CURSOR_DELTA, 0x5);
		/* [한국어] 이번 속도의 그림자 복사본에 값이 들어간다. */
		dw_pcie_writel_dbi(pci, GEN3_EQ_FB_MODE_DIR_CHANGE_OFF, reg);

		reg = dw_pcie_readl_dbi(pci, GEN3_EQ_CONTROL_OFF);
		/* [한국어] 피드백 모드 비트를 지운다. */
		reg &= ~(GEN3_EQ_CONTROL_OFF_FB_MODE |
			/* [한국어] phase 2/3 종료 조건 비트를 지운다. */
			GEN3_EQ_CONTROL_OFF_PHASE23_EXIT_MODE |
			/* [한국어] 초기 평가에 FOM(Figure of Merit) 증가분을 쓰는 비트를 지운다. */
			GEN3_EQ_CONTROL_OFF_FOM_INC_INITIAL_EVAL |
			/* [한국어] 프리셋 요청 벡터를 지운다. **네 필드를 모두 지우기만 하고 새 값을 넣지
			 * 않는다** -- 하드웨어 기본 동작으로 되돌린다는 뜻이다. */
			GEN3_EQ_CONTROL_OFF_PSET_REQ_VEC);
		dw_pcie_writel_dbi(pci, GEN3_EQ_CONTROL_OFF, reg);
	/* [한국어] 다음 속도의 그림자 복사본으로 넘어간다. */
	}
}
EXPORT_SYMBOL_GPL(qcom_pcie_common_set_equalization);

/* [한국어]
 * qcom_pcie_common_set_16gt_lane_margining - 16GT/s 레인 마진 측정 능력을 설정한다
 *
 * @pci: DWC 인스턴스. num_lanes 와 DBI 접근에 쓰인다.
 * @return: 없음.
 *
 * 레인 마진(Lane Margining)은 PCIe Gen4 에서 규약이 요구하는 기능으로,
 * 링크를 끊지 않은 채 수신단의 전압·타이밍 여유를 재는 것이다. 호스트가
 * 그 능력을 물으면 여기서 설정한 값이 답으로 나간다.
 *
 * MARGINING_1 레지스터 -- 측정 범위를 알린다:
 *  - MAX_VOLTAGE_OFFSET 0x24, NUM_VOLTAGE_STEPS 0x78: 전압 축의 최대 오프셋과
 *    단계 수.
 *  - MAX_TIMING_OFFSET 0x32, NUM_TIMING_STEPS 0x10: 타이밍 축의 같은 값들.
 *  네 필드를 모두 지운 뒤 새로 쓰는 것은 기본값이 하드웨어마다 달라서다.
 *
 * MARGINING_2 레지스터 -- 어떤 방식을 지원하는지 알린다:
 *  - 켜는 것: 독립 오류 샘플러, 샘플 보고 방식, 좌/우 타이밍 독립 측정,
 *    전압 측정 지원.
 *  - 끄는 것: 상/하 전압 독립 측정(지원 안 함).
 *  - 값으로 채우는 것: MAXLANES 에 **pci->num_lanes** 를 넣어 실제 레인 수를
 *    알리고, 샘플 레이트 두 필드에 0x3f 를 넣는다.
 *
 * MAXLANES 가 num_lanes 에 매여 있으므로, 이 함수는 레인 수가 확정된 뒤에
 * 불려야 한다.
 *
 * 실행 컨텍스트: 프로브·재개의 프로세스 문맥. DBI 쓰기 허용 구간 안이어야 한다.
 *
 * 호출 체인:
 *   qcom_pcie_*_post_init 계열 → [이 함수] → dw_pcie_readl_dbi/writel_dbi
 */
void qcom_pcie_common_set_16gt_lane_margining(struct dw_pcie *pci)
{
	u32 reg;

	reg = dw_pcie_readl_dbi(pci, GEN4_LANE_MARGINING_1_OFF);
	/* [한국어] 네 필드를 모두 비운 뒤 새로 채운다. */
	reg &= ~(MARGINING_MAX_VOLTAGE_OFFSET |
		/* [한국어] 전압 단계 수 필드. */
		MARGINING_NUM_VOLTAGE_STEPS |
		/* [한국어] 최대 타이밍 오프셋 필드. */
		MARGINING_MAX_TIMING_OFFSET |
		/* [한국어] 타이밍 단계 수 필드. */
		MARGINING_NUM_TIMING_STEPS);
	reg |= FIELD_PREP(MARGINING_MAX_VOLTAGE_OFFSET, 0x24) |
		/* [한국어] 전압 축을 0x78 단계로 나눈다. */
		FIELD_PREP(MARGINING_NUM_VOLTAGE_STEPS, 0x78) |
		/* [한국어] 타이밍 최대 오프셋 0x32. */
		FIELD_PREP(MARGINING_MAX_TIMING_OFFSET, 0x32) |
		/* [한국어] 타이밍 축을 0x10 단계로 나눈다. */
		FIELD_PREP(MARGINING_NUM_TIMING_STEPS, 0x10);
	/* [한국어] 측정 범위 선언을 하드웨어에 반영한다. */
	dw_pcie_writel_dbi(pci, GEN4_LANE_MARGINING_1_OFF, reg);

	reg = dw_pcie_readl_dbi(pci, GEN4_LANE_MARGINING_2_OFF);
	/* [한국어] 독립 오류 샘플러를 갖고 있다고 알린다. */
	reg |= MARGINING_IND_ERROR_SAMPLER |
		/* [한국어] 샘플 보고 방식을 지원한다고 알린다. */
		MARGINING_SAMPLE_REPORTING_METHOD |
		/* [한국어] 좌/우 타이밍을 독립적으로 잴 수 있다고 알린다. */
		MARGINING_IND_LEFT_RIGHT_TIMING |
		/* [한국어] 전압 축 측정을 지원한다고 알린다. */
		MARGINING_VOLTAGE_SUPPORTED;
	/* [한국어] 상/하 전압을 독립적으로 재는 것은 **지원하지 않는다**. */
	reg &= ~(MARGINING_IND_UP_DOWN_VOLTAGE |
		/* [한국어] 최대 레인 수 필드를 비운다. 아래에서 실제 값을 넣는다. */
		MARGINING_MAXLANES |
		/* [한국어] 타이밍 샘플 레이트 필드를 비운다. */
		MARGINING_SAMPLE_RATE_TIMING |
		/* [한국어] 전압 샘플 레이트 필드를 비운다. */
		MARGINING_SAMPLE_RATE_VOLTAGE);
	reg |= FIELD_PREP(MARGINING_MAXLANES, pci->num_lanes) |
		/* [한국어] 타이밍 샘플 레이트 0x3f(최대). 더 많이 샘플링할수록 측정이 정확해진다. */
		FIELD_PREP(MARGINING_SAMPLE_RATE_TIMING, 0x3f) |
		/* [한국어] 전압 샘플 레이트도 최대값. */
		FIELD_PREP(MARGINING_SAMPLE_RATE_VOLTAGE, 0x3f);
	/* [한국어] 지원 방식 선언을 하드웨어에 반영한다. 호스트가 마진 능력을 물으면
	 * 이 값들이 답으로 나간다. */
	dw_pcie_writel_dbi(pci, GEN4_LANE_MARGINING_2_OFF, reg);
/* [한국어] MAXLANES 가 pci->num_lanes 에 매여 있으므로, 이 함수는 레인 수가 확정된
 * 뒤에 불려야 한다. */
}
EXPORT_SYMBOL_GPL(qcom_pcie_common_set_16gt_lane_margining);
