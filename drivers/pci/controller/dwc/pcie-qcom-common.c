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

void qcom_pcie_common_set_equalization(struct dw_pcie *pci)
{
	struct device *dev = pci->dev;
	u32 reg;
	u16 speed;
	/*
	 * GEN3_RELATED_OFF register is repurposed to apply equalization
	 * settings at various data transmission rates through registers namely
	 * GEN3_EQ_*. The RATE_SHADOW_SEL bit field of GEN3_RELATED_OFF
	 * determines the data rate for which these equalization settings are
	 * applied.
	 */

	for (speed = PCIE_SPEED_8_0GT; speed <= pcie_get_link_speed(pci->max_link_speed); speed++) {
		if (speed > PCIE_SPEED_32_0GT) {
			dev_warn(dev, "Skipped equalization settings for unsupported data rate\n");
			break;
		}

		reg = dw_pcie_readl_dbi(pci, GEN3_RELATED_OFF);
		reg &= ~GEN3_RELATED_OFF_GEN3_ZRXDC_NONCOMPL;
		reg &= ~GEN3_RELATED_OFF_RATE_SHADOW_SEL_MASK;
		reg |= FIELD_PREP(GEN3_RELATED_OFF_RATE_SHADOW_SEL_MASK,
			  speed - PCIE_SPEED_8_0GT);
		dw_pcie_writel_dbi(pci, GEN3_RELATED_OFF, reg);

		reg = dw_pcie_readl_dbi(pci, GEN3_EQ_FB_MODE_DIR_CHANGE_OFF);
		reg &= ~(GEN3_EQ_FMDC_T_MIN_PHASE23 |
			GEN3_EQ_FMDC_N_EVALS |
			GEN3_EQ_FMDC_MAX_PRE_CURSOR_DELTA |
			GEN3_EQ_FMDC_MAX_POST_CURSOR_DELTA);
		reg |= FIELD_PREP(GEN3_EQ_FMDC_T_MIN_PHASE23, 0x1) |
			FIELD_PREP(GEN3_EQ_FMDC_N_EVALS, 0xd) |
			FIELD_PREP(GEN3_EQ_FMDC_MAX_PRE_CURSOR_DELTA, 0x5) |
			FIELD_PREP(GEN3_EQ_FMDC_MAX_POST_CURSOR_DELTA, 0x5);
		dw_pcie_writel_dbi(pci, GEN3_EQ_FB_MODE_DIR_CHANGE_OFF, reg);

		reg = dw_pcie_readl_dbi(pci, GEN3_EQ_CONTROL_OFF);
		reg &= ~(GEN3_EQ_CONTROL_OFF_FB_MODE |
			GEN3_EQ_CONTROL_OFF_PHASE23_EXIT_MODE |
			GEN3_EQ_CONTROL_OFF_FOM_INC_INITIAL_EVAL |
			GEN3_EQ_CONTROL_OFF_PSET_REQ_VEC);
		dw_pcie_writel_dbi(pci, GEN3_EQ_CONTROL_OFF, reg);
	}
}
EXPORT_SYMBOL_GPL(qcom_pcie_common_set_equalization);

void qcom_pcie_common_set_16gt_lane_margining(struct dw_pcie *pci)
{
	u32 reg;

	reg = dw_pcie_readl_dbi(pci, GEN4_LANE_MARGINING_1_OFF);
	reg &= ~(MARGINING_MAX_VOLTAGE_OFFSET |
		MARGINING_NUM_VOLTAGE_STEPS |
		MARGINING_MAX_TIMING_OFFSET |
		MARGINING_NUM_TIMING_STEPS);
	reg |= FIELD_PREP(MARGINING_MAX_VOLTAGE_OFFSET, 0x24) |
		FIELD_PREP(MARGINING_NUM_VOLTAGE_STEPS, 0x78) |
		FIELD_PREP(MARGINING_MAX_TIMING_OFFSET, 0x32) |
		FIELD_PREP(MARGINING_NUM_TIMING_STEPS, 0x10);
	dw_pcie_writel_dbi(pci, GEN4_LANE_MARGINING_1_OFF, reg);

	reg = dw_pcie_readl_dbi(pci, GEN4_LANE_MARGINING_2_OFF);
	reg |= MARGINING_IND_ERROR_SAMPLER |
		MARGINING_SAMPLE_REPORTING_METHOD |
		MARGINING_IND_LEFT_RIGHT_TIMING |
		MARGINING_VOLTAGE_SUPPORTED;
	reg &= ~(MARGINING_IND_UP_DOWN_VOLTAGE |
		MARGINING_MAXLANES |
		MARGINING_SAMPLE_RATE_TIMING |
		MARGINING_SAMPLE_RATE_VOLTAGE);
	reg |= FIELD_PREP(MARGINING_MAXLANES, pci->num_lanes) |
		FIELD_PREP(MARGINING_SAMPLE_RATE_TIMING, 0x3f) |
		FIELD_PREP(MARGINING_SAMPLE_RATE_VOLTAGE, 0x3f);
	dw_pcie_writel_dbi(pci, GEN4_LANE_MARGINING_2_OFF, reg);
}
EXPORT_SYMBOL_GPL(qcom_pcie_common_set_16gt_lane_margining);
