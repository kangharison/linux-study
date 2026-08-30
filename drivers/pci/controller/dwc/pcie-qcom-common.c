// SPDX-License-Identifier: GPL-2.0
/* NVMe: Qualcomm DesignWare PCIe 컨트롤러 공통 설정
 * NVMe SSD가 PCIe 링크를 통해 호스트와 통신하기 전,
 * 물리/링크 계층 파라미터(equalization, lane margining)를 tuning 한다.
 * 이 파일의 함수들은 dwc_pcie EP 링크 품질과 직결되며,
 * 링크 품질이 낮으면 NVMe I/O 장애, AER, DMA 오류로 이어질 수 있다.
 */
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/pci.h>           /* NVMe: pci_bus, pci_dev, pci_read_config_xxx 등
                                  * NVMe host driver가 장치를 탐색/바인딩할 때
                                  * 사용하는 핵심 PCI 서브시스템 헤더 */

#include "pcie-designware.h"     /* NVMe: DesignWare PCIe 컨트롤러 구조체 dw_pcie,
                                  * DBI(Data Bus Interface) 접근 함수,
                                  * link speed/lane 관련 정의 포함 */
#include "pcie-qcom-common.h"    /* NVMe: Qualcomm 공통 레지스터/매크로/함수 선언 */

/* NVMe: PCIe link equalization 설정
 * Gen3(8GT/s) 이상에서 신호 왜곡을 보상해 링크 안정성을 확보한다.
 * NVMe SSD 연결 시 link training 단계에서 이 값이 적용되며,
 * 잘못된 equalization은 link retrain, AER correctable error,
 * DMA throughput 저하를 유발할 수 있다.
 */
void qcom_pcie_common_set_equalization(struct dw_pcie *pci)
{
	struct device *dev = pci->dev;  /* NVMe: 컨트롤러 device; dev_warn 등에 사용 */
	u32 reg;                        /* NVMe: DBI register read/write 버퍼 */
	u16 speed;                      /* NVMe: 현재 설정할 PCIe speed (GT/s 단위) */

	/*
	 * GEN3_RELATED_OFF register is repurposed to apply equalization
	 * settings at various data transmission rates through registers namely
	 * GEN3_EQ_*. The RATE_SHADOW_SEL bit field of GEN3_RELATED_OFF
	 * determines the data rate for which these equalization settings are
	 * applied.
	 */

	/* NVMe: Gen3(8GT/s)부터 컨트롤러가 지원하는 최대 link speed까지 반복
	 * pcie_get_link_speed()는 dw_pcie->max_link_speed를 PCIe speed 상수로 변환
	 * 예: Gen3, Gen4, Gen5 등. NVMe SSD의 최대 속도와 맞물려야 한다.
	 */
	for (speed = PCIE_SPEED_8_0GT; speed <= pcie_get_link_speed(pci->max_link_speed); speed++) {
		/* NVMe: 현재 커널이 지원하는 최대 32GT/s(Gen5)를 초과하면
		 * equalization 설정을 중단하고 경고. NVMe SSD가 Gen6 이상을
		 * 요구하더라도 이 코드는 대응하지 않는다.
		 */
		if (speed > PCIE_SPEED_32_0GT) {
			dev_warn(dev, "Skipped equalization settings for unsupported data rate\n");
			break;
		}

		/* NVMe: Gen3 관련 레지스터 읽기
		 * DBI를 통해 DesignWare 내부 레지스터에 접근.
		 * NVMe host가 링크 상태를 변경하기 전 컨트롤러 내부 상태를 읽는
		 * 패턴과 동일하다.
		 */
		reg = dw_pcie_readl_dbi(pci, GEN3_RELATED_OFF);
		/* NVMe: Gen3 non-compliant ZRX-DC 설정 클리어
		 * compliance 관련 비트로, 일반 NVMe 동작 시에는 사용하지 않음
		 */
		reg &= ~GEN3_RELATED_OFF_GEN3_ZRXDC_NONCOMPL;
		/* NVMe: RATE_SHADOW_SEL 필드 클리어
		 * 다음 쓰기에서 speed별 shadow bank를 선택하기 위해 초기화
		 */
		reg &= ~GEN3_RELATED_OFF_RATE_SHADOW_SEL_MASK;
		/* NVMe: speed별 shadow bank 선택
		 * 8GT/s -> 0, 16GT/s -> 1, 32GT/s -> 2 처럼 매핑되어
		 * 각 속도에 맞는 equalization coefficient를 설정
		 */
		reg |= FIELD_PREP(GEN3_RELATED_OFF_RATE_SHADOW_SEL_MASK,
			  speed - PCIE_SPEED_8_0GT);
		/* NVMe: 선택된 speed의 GEN3_RELATED_OFF에 기록
		 * 이 시점에서 NVMe PCIe link의 해당 속도 equalization 설정 공간 활성화
		 */
		dw_pcie_writel_dbi(pci, GEN3_RELATED_OFF, reg);

		/* NVMe: Feedback Mode Direction Change 레지스터 읽기
		 * equalization phase 2/3의 수렴 조건을 제어
		 */
		reg = dw_pcie_readl_dbi(pci, GEN3_EQ_FB_MODE_DIR_CHANGE_OFF);
		/* NVMe: 기존 phase23 시간/평가횟수/pre-post cursor delta 클리어
		 * 이전 값이 남아 있으면 NVMe 링크 training 결과가 달라질 수 있음
		 */
		reg &= ~(GEN3_EQ_FMDC_T_MIN_PHASE23 |
			GEN3_EQ_FMDC_N_EVALS |
			GEN3_EQ_FMDC_MAX_PRE_CURSOR_DELTA |
			GEN3_EQ_FMDC_MAX_POST_CURSOR_DELTA);
		/* NVMe: equalization 수렴 파라미터 설정
		 * phase23 최소 시간, 평가 횟수, pre/post cursor 변화 한도를
		 * Qualcomm 권장값으로 고정. NVMe SSD와의 link training이
		 * 이 값에 따라 수렴하거나 timeout/retrain 될 수 있다.
		 */
		reg |= FIELD_PREP(GEN3_EQ_FMDC_T_MIN_PHASE23, 0x1) |
			FIELD_PREP(GEN3_EQ_FMDC_N_EVALS, 0xd) |
			FIELD_PREP(GEN3_EQ_FMDC_MAX_PRE_CURSOR_DELTA, 0x5) |
			FIELD_PREP(GEN3_EQ_FMDC_MAX_POST_CURSOR_DELTA, 0x5);
		/* NVMe: FMDC 레지스터에 기록
		 * 이 설정은 컨트롤러와 NVMe SSD 간 Tx/Rx equalization 협상에 영향
		 */
		dw_pcie_writel_dbi(pci, GEN3_EQ_FB_MODE_DIR_CHANGE_OFF, reg);

		/* NVMe: Equalization Control 레지스터 읽기
		 * equalization algorithm 동작 모드 선택
		 */
		reg = dw_pcie_readl_dbi(pci, GEN3_EQ_CONTROL_OFF);
		/* NVMe: feedback mode, phase23 exit mode, FOM 증가 초기 평가,
		 * preset request vector 필드 모두 클리어
		 * Qualcomm 플랫폼에서 의도한 기본 동작으로 되돌림
		 */
		reg &= ~(GEN3_EQ_CONTROL_OFF_FB_MODE |
			GEN3_EQ_CONTROL_OFF_PHASE23_EXIT_MODE |
			GEN3_EQ_CONTROL_OFF_FOM_INC_INITIAL_EVAL |
			GEN3_EQ_CONTROL_OFF_PSET_REQ_VEC);
		/* NVMe: equalization control 기록
		 * 이 값이 equalization 결과(FOM, preset)를 결정하며,
		 * 최종적으로 NVMe SSD의 link width/speed 협상에 반영됨
		 */
		dw_pcie_writel_dbi(pci, GEN3_EQ_CONTROL_OFF, reg);
	}
}
EXPORT_SYMBOL_GPL(qcom_pcie_common_set_equalization);
/* NVMe: 위 함수를 모듈 외부(예: Qualcomm SoC별 pcie-qcom-*.c)에서 사용 가능하게 노출
 * NVMe host driver는 직접 호출하지 않고, 컨트롤러 드라이버 초기화 경로에서 호출됨
 */

/* NVMe: 16GT/s(Gen4) lane margining 설정
 * lane margining은 link margin(전압/타이밍 여유) 측정을 가능하게 하며,
 * signal integrity 디버깅과 AER 예측에 사용된다.
 * NVMe SSD가 Gen4 속도로 동작할 때 이 설정이 적용되어
 * link 오류 발생 시 debugfs/dmesg를 통해 margin 정보를 얻을 수 있다.
 */
void qcom_pcie_common_set_16gt_lane_margining(struct dw_pcie *pci)
{
	u32 reg;                        /* NVMe: DBI register 버퍼 */

	/* NVMe: Gen4 Lane Margining 1 레지스터 읽기
	 * 전압/타이밍 offset 및 step 수 설정
	 */
	reg = dw_pcie_readl_dbi(pci, GEN4_LANE_MARGINING_1_OFF);
	/* NVMe: 기존 voltage/timing margin 필드 클리어
	 * 레지스터 값을 Qualcomm 기본값으로 재구성하기 위해 초기화
	 */
	reg &= ~(MARGINING_MAX_VOLTAGE_OFFSET |
		MARGINING_NUM_VOLTAGE_STEPS |
		MARGINING_MAX_TIMING_OFFSET |
		MARGINING_NUM_TIMING_STEPS);
	/* NVMe: lane margining 파라미터 설정
	 * 최대 전압 오프셋, 전압 단계 수, 최대 타이밍 오프셋,
	 * 타이밍 단계 수를 Qualcomm 권장값으로 설정.
	 * 이 값은 NVMe SSD와의 link에서 얼마나 여유 있는 eye를 가지는지
	 * 측정하는 기준이 된다.
	 */
	reg |= FIELD_PREP(MARGINING_MAX_VOLTAGE_OFFSET, 0x24) |
		FIELD_PREP(MARGINING_NUM_VOLTAGE_STEPS, 0x78) |
		FIELD_PREP(MARGINING_MAX_TIMING_OFFSET, 0x32) |
		FIELD_PREP(MARGINING_NUM_TIMING_STEPS, 0x10);
	/* NVMe: Lane Margining 1 레지스터 기록
	 * Gen4 NVMe SSD가 들어왔을 때 margining 측정을 위한 기준 확립
	 */
	dw_pcie_writel_dbi(pci, GEN4_LANE_MARGINING_1_OFF, reg);

	/* NVMe: Gen4 Lane Margining 2 레지스터 읽기
	 * margining 지원 기능 및 lane/sample rate 설정
	 */
	reg = dw_pcie_readl_dbi(pci, GEN4_LANE_MARGINING_2_OFF);
	/* NVMe: margining 보고/샘플링 기능 비트 설정
	 * independent error sampler, sample reporting method,
	 * left/right timing, voltage supported 기능 활성화
	 */
	reg |= MARGINING_IND_ERROR_SAMPLER |
		MARGINING_SAMPLE_REPORTING_METHOD |
		MARGINING_IND_LEFT_RIGHT_TIMING |
		MARGINING_VOLTAGE_SUPPORTED;
	/* NVMe: up/down voltage, max lanes, sample rate 필드 클리어
	 * 뒤에서 Qualcomm 기본값으로 재설정하기 위해 초기화
	 */
	reg &= ~(MARGINING_IND_UP_DOWN_VOLTAGE |
		MARGINING_MAXLANES |
		MARGINING_SAMPLE_RATE_TIMING |
		MARGINING_SAMPLE_RATE_VOLTAGE);
	/* NVMe: 컨트롤러 물리 lane 수 및 샘플링 레이트 설정
	 * pci->num_lanes는 NVMe SSD와 실제 협상된/가능한 lane 수와
	 * 연관되며, lane margining이 모든 활성 lane에 대해 동작하도록 함.
	 * 타이밍/전압 sample rate는 0x3f로 고정.
	 */
	reg |= FIELD_PREP(MARGINING_MAXLANES, pci->num_lanes) |
		FIELD_PREP(MARGINING_SAMPLE_RATE_TIMING, 0x3f) |
		FIELD_PREP(MARGINING_SAMPLE_RATE_VOLTAGE, 0x3f);
	/* NVMe: Lane Margining 2 레지스터 기록
	 * Gen4 NVMe 장치에서 lane margining이 정상 동작하도록 최종 확정
	 */
	dw_pcie_writel_dbi(pci, GEN4_LANE_MARGINING_2_OFF, reg);
}
EXPORT_SYMBOL_GPL(qcom_pcie_common_set_16gt_lane_margining);
/* NVMe: 모듈 외부에서 lane margining 함수를 호출할 수 있도록 낸출
 * NVMe host driver pci.c는 직접 사용하지 않음
 */
