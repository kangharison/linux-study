/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2022, Qualcomm Innovation Center, Inc. All rights reserved.
 */

/*
 * [한국어 설명] 퀄컴 SMMU 구현체의 공유 선언 (arm-smmu-qcom.h)
 *
 * === 파일의 역할 ===
 * 퀄컴 SoC 의 SMMU 는 ARM 규격을 따르되 몇 가지가 다르다. 부트로더가
 * 이미 설정해 둔 상태를 커널이 이어받아야 하고, 폴트를 멈춰 세우는
 * 방식이 다르며, 디버그용 레지스터 블록(TBU)이 따로 있다.
 *
 * 그 차이를 다루는 코드가 두 파일에 나뉘어 있어(arm-smmu-qcom.c 와
 * arm-smmu-qcom-debug.c) 공유할 선언을 이 헤더에 모았다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * arm-smmu.c 의 공통 코드 → arm_smmu_impl 갈고리 → arm-smmu-qcom.c
 *   → 이 헤더가 정의한 구조체와 함수
 *
 * === 타 모듈과의 연결 ===
 * arm-smmu.h 의 arm_smmu_device 와 arm_smmu_impl 을 감싼다.
 * 디버그 기능은 설정으로 뺄 수 있어, 꺼진 경우의 빈 함수도 여기 둔다.
 *
 * === 주요 함수/구조체 요약 ===
 * struct qcom_smmu: 퀄컴 확장을 담아 arm_smmu_device 를 감싼 구조체.
 * struct qcom_smmu_match_data: SoC 마다 다른 갈고리표와 레지스터 배치를
 *   장치 트리 매칭으로 고르게 한다.
 * qcom_smmu_context_fault: 퀄컴 전용 문맥 오류 처리기.
 */
#ifndef _ARM_SMMU_QCOM_H	/* [한국어] 중복 포함 방지 가드. */
#define _ARM_SMMU_QCOM_H	/* [한국어] 가드를 세운다. */

/* [한국어] 퀄컴 SMMU. 공통 구조체를 감싸 확장한다.
 *
 * 첫 멤버가 arm_smmu_device 라 container_of 로 오갈 수 있다. 공통 코드는
 * 안쪽만 보고, 퀄컴 코드는 바깥을 되짚어 확장 필드를 쓴다. */
struct qcom_smmu {
	/* [한국어] 공통 SMMU 구조체. 반드시 첫 멤버여야 한다. */
	struct arm_smmu_device smmu;
	/* [한국어] 이 SoC 에 맞는 갈고리표와 레지스터 배치.
	 *  설정자: probe 가 장치 트리 매칭으로 고른다.
	 *  읽는 자: 디버그 레지스터를 읽는 코드 등. */
	const struct qcom_smmu_match_data *data;
	/* [한국어] 우회 스트림을 특별히 다뤄야 하는가.
	 *  설정자: SoC 별 초기화.
	 *  어떤 퀄컴 하드웨어는 우회 항목을 그냥 두면 안 되고, 전용 컨텍스트
	 *  뱅크로 보내야 한다. */
	bool bypass_quirk;
	/* [한국어] 그때 쓸 우회 전용 컨텍스트 뱅크의 번호.
	 *  bypass_quirk 가 참일 때만 뜻이 있다. */
	u8 bypass_cbndx;
	/* [한국어] 폴트를 멈춰 세우기로 한 컨텍스트 뱅크들의 비트맵.
	 *  설정자: 도메인을 만들 때 그 장치가 폴트 처리를 원하면 세운다.
	 *  읽는 자: 오류 처리기가 트랜잭션을 다시 시작시킬지 정한다.
	 *  GPU 처럼 폴트를 스스로 처리하는 장치를 위한 기능이다. */
	u32 stall_enabled;
};

/* [한국어] SoC 마다 자리가 다른 디버그 레지스터의 이름표.
 *
 * 실제 오프셋은 qcom_smmu_config 의 배열에 담겨 있고, 이 값이 그 배열의
 * 첨자가 된다. 그래서 코드는 이름으로 쓰고 값은 SoC 마다 달라진다. */
enum qcom_smmu_impl_reg_offset {
	/* [한국어] TBU(변환 버퍼 유닛)의 전원 상태.
	 *  읽는 자: 무효화가 끝나지 않을 때 원인을 찾는 디버그 코드.
	 *  전원이 꺼진 TBU 는 응답하지 않아 무효화가 멈춘 것처럼 보인다. */
	QCOM_SMMU_TBU_PWR_STATUS,
	/* [한국어] 각 TBU 가 무효화를 받았다고 답했는지.
	 *  어느 TBU 가 응답하지 않는지 가려내는 데 쓴다. */
	QCOM_SMMU_STATS_SYNC_INV_TBU_ACK,
	/* [한국어] SMMU 와 시스템 사이 대기 카운터.
	 *  무효화가 멈춘 원인이 SMMU 안인지 밖인지 가르는 데 쓴다. */
	QCOM_SMMU_MMU2QSS_AND_SAFE_WAIT_CNTR,
};

/* [한국어] SoC 별 디버그 레지스터 배치. */
struct qcom_smmu_config {
	/* [한국어] 위 enum 을 첨자로 하는 오프셋 배열.
	 *  설정자: SoC 마다 정적으로 정의된 표.
	 *  값 범위: NULL 이면 그 SoC 에 디버그 레지스터가 없다는 뜻. */
	const u32 *reg_offset;
};

/* [한국어] 장치 트리 매칭이 고르는 SoC 별 설정 묶음.
 *
 * 퀄컴 SoC 가 워낙 많고 세대마다 달라, 하나의 표로 묶어 두고 compatible
 * 문자열로 고른다. */
struct qcom_smmu_match_data {
	/* [한국어] 디버그 레지스터 배치(없을 수 있다). */
	const struct qcom_smmu_config *cfg;
	/* [한국어] 보통 장치에 쓸 갈고리표.
	 *  읽는 자: qcom_smmu_impl_init. */
	const struct arm_smmu_impl *impl;
	/* [한국어] Adreno GPU 에 쓸 갈고리표.
	 *  GPU 는 자기 주소 공간을 스스로 관리해 항등 매핑이 필요하고,
	 *  폴트도 스스로 처리한다. 그래서 다른 표를 쓴다. */
	const struct arm_smmu_impl *adreno_impl;
	/* [한국어] 이 SMMU 뒤에 붙는 장치들의 매칭표.
	 *  읽는 자: 어떤 장치에 특별 대접을 할지 가리는 코드.
	 *  SMMU 자신이 아니라 그 뒤 장치를 보고 정해야 하는 설정이 있다. */
	const struct of_device_id * const client_match;
};

irqreturn_t qcom_smmu_context_fault(int irq, void *dev);	/* [한국어] 퀄컴 전용 문맥 오류 처리기. arm-smmu-qcom.c 가 구현하고 디버그 쪽도 부른다. */

#ifdef CONFIG_ARM_SMMU_QCOM_DEBUG	/* [한국어] 디버그 기능은 설정으로 뺄 수 있다. */
void qcom_smmu_tlb_sync_debug(struct arm_smmu_device *smmu);	/* [한국어] 무효화가 멈췄을 때 TBU 상태를 찍어 원인을 알린다. */
int qcom_tbu_probe(struct platform_device *pdev);	/* [한국어] TBU 디버그 블록을 별도 플랫폼 장치로 잡는다. */
#else	/* [한국어] 꺼진 빌드에서는 */
static inline void qcom_smmu_tlb_sync_debug(struct arm_smmu_device *smmu) { }	/* [한국어] 아무것도 하지 않는 빈 함수로 대체해, 호출부에 #ifdef 를 뿌리지 않게 한다. */
static inline int qcom_tbu_probe(struct platform_device *pdev) { return -EINVAL; }	/* [한국어] 실패를 돌려주어 그 장치를 잡지 않게 한다. */
#endif	/* [한국어] 디버그 갈래의 끝. */

#endif /* _ARM_SMMU_QCOM_H */	/* [한국어] 포함 가드의 끝. */
