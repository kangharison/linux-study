// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2019, The Linux Foundation. All rights reserved.
 */

/*
 * [한국어 설명] 퀄컴 SoC 의 SMMU 구현체 갈고리 (arm-smmu-qcom.c)
 *
 * === 파일의 역할 ===
 * 퀄컴 SMMU 는 ARM 규격을 따르되 다른 곳이 많다. 그 차이를 갈고리로
 * 덮는 것이 이 파일의 일이고, 크게 네 갈래다.
 *
 * 첫째, 펌웨어가 남긴 상태를 이어받는다. 부팅 중에 화면이 계속 나와야
 * 하므로, 이미 설정된 스트림 매핑을 지우지 않고 우회로 이어 둔다.
 *
 * 둘째, 펌웨어의 버릇을 우회한다. 어떤 펌웨어 판은 S2CR 에 "오류" 쓰기를
 * 무시하고 "우회" 쓰기를 오류로 바꿔 버린다. 그래서 실제로 써 보고
 * 되읽어 그 버릇을 알아낸 뒤, 뜻을 뒤집어 쓰는 방식으로 우회한다.
 *
 * 셋째, Adreno GPU 를 특별히 대접한다. GPU 는 프로세스마다 다른 페이지
 * 테이블을 쓰고 그것을 스스로 갈아 끼운다. 그러려면 상위 주소 공간
 * (TTBR1)은 커널이 쥐고 하위(TTBR0)는 GPU 가 바꿀 수 있어야 한다.
 * 그 인터페이스가 adreno_smmu_priv 다.
 *
 * 넷째, SoC 마다 다른 성능 설정(ACTLR)을 장치별로 심는다. 미리 읽기
 * 깊이와 캐시 방식이 장치 성격에 따라 달라야 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * arm-smmu.c 의 probe → arm_smmu_impl_init → qcom_smmu_impl_init
 *   → 장치 트리 매칭으로 SoC 별 갈고리표를 고른다
 *
 * GPU 드라이버 → adreno_smmu_priv 의 콜백 → 이 파일 → 컨텍스트 뱅크
 *
 * TBU 디버그 장치는 별도 플랫폼 드라이버로 등록한다.
 *
 * === 타 모듈과의 연결 ===
 * 위: arm-smmu-impl.c 가 이리로 보낸다.
 * 옆: arm-smmu-qcom-debug.c(ATOS 와 TBU), qcom_scm(보안 호출),
 *   drm/msm 의 GPU 드라이버(adreno_smmu_priv).
 * 아래: arm-smmu.h 의 레지스터 접근.
 *
 * === 주요 함수/구조체 요약 ===
 * qcom_smmu_cfg_probe: 펌웨어 버릇을 알아내고 남은 매핑을 이어받는다.
 * qcom_smmu_write_s2cr: 그 버릇을 뜻을 뒤집어 우회한다.
 * qcom_adreno_smmu_init_context: GPU 에 분리 페이지 테이블을 열어 준다.
 * qcom_adreno_smmu_set_ttbr0_cfg: GPU 가 하위 표를 갈아 끼우는 통로.
 * qcom_smmu_set_actlr_dev: 장치 성격에 맞는 성능 설정을 심는다.
 * qcom_smmu_impl_init: SoC 를 가려 알맞은 갈고리표를 단다.
 */
#include <linux/acpi.h>	/* [한국어] ACPI 부팅에서 기계 이름으로 가려내는 데 쓴다. */
#include <linux/adreno-smmu-priv.h>	/* [한국어] GPU 드라이버와 주고받는 인터페이스 정의. */
#include <linux/delay.h>	/* [한국어] udelay — 무효화 완료를 기다린다. */
#include <linux/of_device.h>	/* [한국어] 장치 트리 매칭. */
#include <linux/firmware/qcom/qcom_scm.h>	/* [한국어] 보안 세계 호출. 여러 우회가 이것을 쓴다. */
#include <linux/platform_device.h>	/* [한국어] TBU 를 별도 플랫폼 드라이버로 등록한다. */
#include <linux/pm_runtime.h>	/* [한국어] 레지스터를 건드리기 전에 전원을 켠다. */

#include "arm-smmu.h"	/* [한국어] 레지스터 정의와 공통 자료 구조. */
#include "arm-smmu-qcom.h"	/* [한국어] 퀄컴 확장 구조체와 공유 선언. */

#define QCOM_DUMMY_VAL	-1	/* [한국어] 0 쓰기를 잘못 처리하는 하이퍼바이저를 피하려는 값. 본체 파일과 같은 이유다. */

/*
 * SMMU-500 TRM defines BIT(0) as CMTLB (Enable context caching in the
 * macro TLB) and BIT(1) as CPRE (Enable context caching in the prefetch
 * buffer). The remaining bits are implementation defined and vary across
 * SoCs.
 */

#define CPRE			(1 << 1)	/* [한국어] (위 주석 참고) 미리 읽기 버퍼에 문맥을 캐시한다. */
#define CMTLB			(1 << 0)	/* [한국어] 매크로 TLB 에 문맥을 캐시한다. */
#define PREFETCH_SHIFT		8	/* [한국어] 미리 읽기 깊이 필드의 자리. 그 위 비트들은 구현 정의라 SoC 마다 다르다. */
#define PREFETCH_DEFAULT	0	/* [한국어] 하드웨어 기본 깊이. */
#define PREFETCH_SHALLOW	(1 << PREFETCH_SHIFT)	/* [한국어] 얕게. 대역폭이 일정한 장치에 알맞다. */
#define PREFETCH_MODERATE	(2 << PREFETCH_SHIFT)	/* [한국어] 중간. */
#define PREFETCH_DEEP		(3 << PREFETCH_SHIFT)	/* [한국어] 깊게. 접근이 이어지는 GPU 등에 알맞다. */
#define GFX_ACTLR_PRR          (1 << 5)	/* [한국어] GPU 전용 PRR 기능 비트. 규격에 없는 구현 정의 자리다. */

/*
 * [한국어] 장치별 ACTLR(보조 제어) 설정 표.
 *
 * 미리 읽기 깊이와 캐시 방식을 장치 성격에 맞춘다. GPU 처럼 접근이
 * 이어지는 장치는 깊게 미리 읽는 것이 이롭고, 화면 출력처럼 대역폭이
 * 일정한 장치는 얕게 두는 편이 낫다.
 *
 * 같은 종류의 장치라도 SoC 세대마다 값이 달라, compatible 문자열마다
 * 따로 적었다.
 */
static const struct of_device_id qcom_smmu_actlr_client_of_match[] = {
	{ .compatible = "qcom,adreno",	/* [한국어] GPU 는 접근이 이어져 깊게 미리 읽는 것이 이롭다. 아래 항목들은 SoC·장치마다의 값이다. */
			.data = (const void *) (PREFETCH_DEEP | CPRE | CMTLB) },
	{ .compatible = "qcom,adreno-gmu",
			.data = (const void *) (PREFETCH_DEEP | CPRE | CMTLB) },
	{ .compatible = "qcom,adreno-smmu",
			.data = (const void *) (PREFETCH_DEEP | CPRE | CMTLB) },
	{ .compatible = "qcom,fastrpc",
			.data = (const void *) (PREFETCH_DEEP | CPRE | CMTLB) },
	{ .compatible = "qcom,qcm2290-mdss",
			.data = (const void *) (PREFETCH_SHALLOW | CPRE | CMTLB) },
	{ .compatible = "qcom,sa8775p-mdss",
			.data = (const void *) (PREFETCH_DEFAULT | CMTLB) },
	{ .compatible = "qcom,sc7280-mdss",
			.data = (const void *) (PREFETCH_SHALLOW | CPRE | CMTLB) },
	{ .compatible = "qcom,sc7280-venus",
			.data = (const void *) (PREFETCH_SHALLOW | CPRE | CMTLB) },
	{ .compatible = "qcom,sc8180x-mdss",
			.data = (const void *) (PREFETCH_SHALLOW | CPRE | CMTLB) },
	{ .compatible = "qcom,sc8280xp-mdss",
			.data = (const void *) (PREFETCH_SHALLOW | CPRE | CMTLB) },
	{ .compatible = "qcom,sm6115-mdss",
			.data = (const void *) (PREFETCH_SHALLOW | CPRE | CMTLB) },
	{ .compatible = "qcom,sm6125-mdss",
			.data = (const void *) (PREFETCH_SHALLOW | CPRE | CMTLB) },
	{ .compatible = "qcom,sm6350-mdss",
			.data = (const void *) (PREFETCH_SHALLOW | CPRE | CMTLB) },
	{ .compatible = "qcom,sm8150-mdss",
			.data = (const void *) (PREFETCH_SHALLOW | CPRE | CMTLB) },
	{ .compatible = "qcom,sm8250-mdss",
			.data = (const void *) (PREFETCH_SHALLOW | CPRE | CMTLB) },
	{ .compatible = "qcom,sm8350-mdss",
			.data = (const void *) (PREFETCH_SHALLOW | CPRE | CMTLB) },
	{ .compatible = "qcom,sm8450-mdss",
			.data = (const void *) (PREFETCH_DEFAULT | CMTLB) },
	{ .compatible = "qcom,sm8550-mdss",
			.data = (const void *) (PREFETCH_DEFAULT | CMTLB) },
	{ .compatible = "qcom,sm8650-mdss",
			.data = (const void *) (PREFETCH_DEFAULT | CMTLB) },
	{ .compatible = "qcom,sm8750-mdss",
			.data = (const void *) (PREFETCH_DEFAULT | CMTLB) },
	{ .compatible = "qcom,x1e80100-mdss",
			.data = (const void *) (PREFETCH_DEFAULT | CMTLB) },
	{ }
};

/*
 * [한국어]
 * to_qcom_smmu - 공통 구조체에서 퀄컴 구조체로 되짚는다
 *
 * @smmu: 공통 구조체.
 * @return: 그것을 품은 퀄컴 구조체.
 */
static struct qcom_smmu *to_qcom_smmu(struct arm_smmu_device *smmu)
{
	return container_of(smmu, struct qcom_smmu, smmu);	/* [한국어] 공통 구조체를 품은 바깥으로 되짚는다. */
}

/*
 * [한국어]
 * qcom_smmu_tlb_sync - 무효화 완료를 기다리되 실패하면 원인을 찍는다
 *
 * @smmu: 대상 SMMU.
 * @page: 동기화 레지스터가 있는 페이지.
 * @sync: 동기화 명령 레지스터의 오프셋.
 * @status: 상태 레지스터의 오프셋.
 *
 * 기다리는 구조는 공통 판과 같다. 다른 것은 시간이 다했을 때다 —
 * 공통 판은 로그 한 줄로 끝내지만, 여기서는 TBU 상태를 읽어 어느
 * 블록이 응답하지 않는지까지 알려 준다.
 */
static void qcom_smmu_tlb_sync(struct arm_smmu_device *smmu, int page,
				int sync, int status)
{
	unsigned int spin_cnt, delay;	/* [한국어] 돌며 기다린 횟수와 잠들 시간. */
	u32 reg;	/* [한국어] 상태 레지스터 값. */

	arm_smmu_writel(smmu, page, sync, QCOM_DUMMY_VAL);	/* [한국어] 동기화를 시작시킨다. 0 이 아닌 값을 쓰는 이유는 하이퍼바이저 버그 때문이다. */
	for (delay = 1; delay < TLB_LOOP_TIMEOUT; delay *= 2) {	/* [한국어] 1초까지 기다린다. */
		for (spin_cnt = TLB_SPIN_COUNT; spin_cnt > 0; spin_cnt--) {	/* [한국어] 먼저 돌아 본다. */
			reg = arm_smmu_readl(smmu, page, status);	/* [한국어] 진행 상태를 읽어 */
			if (!(reg & ARM_SMMU_sTLBGSTATUS_GSACTIVE))	/* [한국어] 끝났으면 */
				return;	/* [한국어] 돌아간다. */
			cpu_relax();	/* [한국어] 도는 동안 CPU 에 힌트를 준다. */
		}
		udelay(delay);	/* [한국어] 아직이면 그만큼 잔다. */
	}

	qcom_smmu_tlb_sync_debug(smmu);	/* [한국어] 공통 판과 다른 점 — 어느 TBU 가 응답하지 않는지까지 찍는다. */
}

/*
 * [한국어]
 * (아래 영어 주석과 함께 읽을 것)
 * qcom_adreno_smmu_write_sctlr - GPU 뱅크의 제어 레지스터를 쓴다
 *
 * @smmu: 대상 SMMU.
 * @idx: 뱅크 번호.
 * @reg: 공통 코드가 만든 값.
 *
 * 원 주석대로 GPU 는 폴트 뒤에도 다음 트랜잭션을 계속 처리해야 한다 —
 * 그러지 않으면 GPU 가 멈춘다. HUPCF 가 그것을 가능하게 한다.
 *
 * 멈춰 세우기(CFCFG)는 GPU 드라이버가 켜고 끌 수 있게 해 두었다.
 * 그 드라이버가 폴트를 직접 다루고 싶을 때만 켠다.
 */
static void qcom_adreno_smmu_write_sctlr(struct arm_smmu_device *smmu, int idx,
		u32 reg)
{
	struct qcom_smmu *qsmmu = to_qcom_smmu(smmu);	/* [한국어] 퀄컴 구조체. */

	/*
	 * On the GPU device we want to process subsequent transactions after a
	 * fault to keep the GPU from hanging
	 */
	reg |= ARM_SMMU_SCTLR_HUPCF;	/* [한국어] 원 주석대로 폴트 뒤에도 다음 트랜잭션을 계속 처리해 GPU 가 멈추지 않게 한다. */

	if (qsmmu->stall_enabled & BIT(idx))	/* [한국어] GPU 드라이버가 멈춰 세우기를 켰으면 */
		reg |= ARM_SMMU_SCTLR_CFCFG;	/* [한국어] 그 비트도 세운다. */

	arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_SCTLR, reg);	/* [한국어] 레지스터에 쓴다. */
}

/*
 * [한국어]
 * qcom_adreno_smmu_get_fault_info - GPU 드라이버에게 폴트 정보를 넘긴다
 *
 * @cookie: 이 도메인.
 * @info: 채울 구조체.
 *
 * GPU 드라이버가 자기 방식으로 폴트를 분석하려면 원시 레지스터 값이
 * 필요하다. TTBR0 와 CONTEXTIDR 까지 주는 것이 요점 — 그 값으로 어느
 * 프로세스의 표에서 난 폴트인지 알 수 있다.
 */
static void qcom_adreno_smmu_get_fault_info(const void *cookie,
		struct adreno_smmu_fault_info *info)
{
	struct arm_smmu_domain *smmu_domain = (void *)cookie;	/* [한국어] 넘겨 둔 도메인. */
	struct arm_smmu_cfg *cfg = &smmu_domain->cfg;	/* [한국어] 뱅크 설정. */
	struct arm_smmu_device *smmu = smmu_domain->smmu;	/* [한국어] 그 SMMU. */

	info->fsr = arm_smmu_cb_read(smmu, cfg->cbndx, ARM_SMMU_CB_FSR);	/* [한국어] 오류 종류. */
	info->fsynr0 = arm_smmu_cb_read(smmu, cfg->cbndx, ARM_SMMU_CB_FSYNR0);	/* [한국어] 부가 정보 0. */
	info->fsynr1 = arm_smmu_cb_read(smmu, cfg->cbndx, ARM_SMMU_CB_FSYNR1);	/* [한국어] 부가 정보 1 — 퀄컴은 여기에 자기 정보를 더 담는다. */
	info->far = arm_smmu_cb_readq(smmu, cfg->cbndx, ARM_SMMU_CB_FAR);	/* [한국어] 오류가 난 주소. */
	info->cbfrsynra = arm_smmu_gr1_read(smmu, ARM_SMMU_GR1_CBFRSYNRA(cfg->cbndx));	/* [한국어] 어느 장치가 냈는지. */
	info->ttbr0 = arm_smmu_cb_readq(smmu, cfg->cbndx, ARM_SMMU_CB_TTBR0);	/* [한국어] 그때 걸려 있던 하위 표 — 어느 프로세스인지 알 수 있다. */
	info->contextidr = arm_smmu_cb_read(smmu, cfg->cbndx, ARM_SMMU_CB_CONTEXTIDR);	/* [한국어] GPU 드라이버가 심어 둔 문맥 식별자. */
}

/*
 * [한국어]
 * qcom_adreno_smmu_set_stall - 폴트 때 트랜잭션을 멈춰 세울지 정한다
 *
 * @cookie: 이 도메인.
 * @enabled: 멈춰 세울 것인가.
 *
 * GPU 드라이버가 폴트를 직접 다루고 싶을 때 켠다. 멈춰 두면 그 상태를
 * 들여다볼 시간이 생긴다.
 *
 * 원 주석이 TLB 무효화 없이 바꾸어도 되는 근거를 밝힌다 — 규격의 의사
 * 코드에 따르면 CFCFG 는 폴트가 날 때마다 다시 읽히고, 그것을 TLB 에
 * 캐시하는 구현은 없다고 본다.
 *
 * 전원이 켜져 있을 때만 레지스터를 건드린다. 꺼져 있으면 다음에 켜질 때
 * 그림자 상태에서 반영된다.
 */
static void qcom_adreno_smmu_set_stall(const void *cookie, bool enabled)
{
	struct arm_smmu_domain *smmu_domain = (void *)cookie;	/* [한국어] 넘겨 둔 도메인. */
	struct arm_smmu_cfg *cfg = &smmu_domain->cfg;	/* [한국어] 뱅크 설정. */
	struct arm_smmu_device *smmu = smmu_domain->smmu;	/* [한국어] 그 SMMU. */
	struct qcom_smmu *qsmmu = to_qcom_smmu(smmu);	/* [한국어] 퀄컴 구조체. */
	u32 mask = BIT(cfg->cbndx);	/* [한국어] 이 뱅크의 비트. */
	bool stall_changed = !!(qsmmu->stall_enabled & mask) != enabled;	/* [한국어] 값이 실제로 바뀌는가. */
	unsigned long flags;	/* [한국어] 인터럽트 상태 저장. */

	if (enabled)	/* [한국어] 켜라면 */
		qsmmu->stall_enabled |= mask;	/* [한국어] 비트를 세우고 */
	else
		qsmmu->stall_enabled &= ~mask;	/* [한국어] 아니면 지운다. 이 상태는 다음 뱅크 쓰기에도 반영된다. */

	/*
	 * If the device is on and we changed the setting, update the register.
	 * The spec pseudocode says that CFCFG is resampled after a fault, and
	 * we believe that no implementations cache it in the TLB, so it should
	 * be safe to change it without a TLB invalidation.
	 */
	if (stall_changed && pm_runtime_get_if_active(smmu->dev) > 0) {	/* [한국어] 바뀌었고 전원이 켜져 있을 때만 레지스터를 건드린다. */
		u32 reg;	/* [한국어] 읽고 고칠 값. */

		spin_lock_irqsave(&smmu_domain->cb_lock, flags);	/* [한국어] 읽고 고쳐 쓰는 동안 겹치면 안 된다. */
		reg = arm_smmu_cb_read(smmu, cfg->cbndx, ARM_SMMU_CB_SCTLR);	/* [한국어] 지금 값을 읽어 */

		if (enabled)	/* [한국어] 켜라면 */
			reg |= ARM_SMMU_SCTLR_CFCFG;	/* [한국어] 비트를 세우고 */
		else
			reg &= ~ARM_SMMU_SCTLR_CFCFG;	/* [한국어] 아니면 지운다. */

		arm_smmu_cb_write(smmu, cfg->cbndx, ARM_SMMU_CB_SCTLR, reg);	/* [한국어] 원 주석대로 TLB 무효화 없이 바꾸어도 된다. */
		spin_unlock_irqrestore(&smmu_domain->cb_lock, flags);	/* [한국어] 락 해제. */

		pm_runtime_put_autosuspend(smmu->dev);	/* [한국어] 전원 참조를 놓는다. */
	}
}

/*
 * [한국어]
 * qcom_adreno_smmu_set_prr_bit - 부분 상주 구간(PRR) 기능을 켜고 끈다
 *
 * @cookie: 이 도메인.
 * @set: 켤 것인가.
 *
 * PRR 은 매핑되지 않은 접근을 오류로 만들지 않고 미리 정해 둔 페이지로
 * 보내는 기능이다. GPU 가 성기게 할당된 자원(sparse texture)을 다룰 때
 * 쓴다 — 없는 부분을 건드려도 GPU 가 멈추지 않는다.
 *
 * 규격에 없는 퀄컴 전용 기능이라 ACTLR 의 구현 정의 비트를 쓴다.
 */
static void qcom_adreno_smmu_set_prr_bit(const void *cookie, bool set)
{
	struct arm_smmu_domain *smmu_domain = (void *)cookie;	/* [한국어] 넘겨 둔 도메인. */
	struct arm_smmu_device *smmu = smmu_domain->smmu;	/* [한국어] 그 SMMU. */
	struct arm_smmu_cfg *cfg = &smmu_domain->cfg;	/* [한국어] 뱅크 설정. */
	u32 reg = 0;	/* [한국어] 읽고 고칠 값. */
	int ret;	/* [한국어] 전원 결과. */

	ret = pm_runtime_resume_and_get(smmu->dev);	/* [한국어] 레지스터를 쓰려면 켜야 한다. */
	if (ret < 0) {	/* [한국어] 켜지 못했으면 */
		dev_err(smmu->dev, "failed to get runtime PM: %d\n", ret);	/* [한국어] 알리고 */
		return;	/* [한국어] 포기한다. */
	}

	reg =  arm_smmu_cb_read(smmu, cfg->cbndx, ARM_SMMU_CB_ACTLR);	/* [한국어] 보조 제어를 읽어 */
	reg &= ~GFX_ACTLR_PRR;	/* [한국어] 그 비트를 지우고 */
	if (set)	/* [한국어] 켜라면 */
		reg |= FIELD_PREP(GFX_ACTLR_PRR, 1);	/* [한국어] 다시 세운다. */
	arm_smmu_cb_write(smmu, cfg->cbndx, ARM_SMMU_CB_ACTLR, reg);	/* [한국어] 되쓴다. */
	pm_runtime_put_autosuspend(smmu->dev);	/* [한국어] 전원 참조를 놓는다. */
}

/*
 * [한국어]
 * qcom_adreno_smmu_set_prr_addr - PRR 이 가리킬 페이지를 정한다
 *
 * @cookie: 이 도메인.
 * @page_addr: 그 물리 주소.
 *
 * 매핑되지 않은 접근이 모두 이 페이지로 간다. 읽으면 정해진 값이 나오고
 * 쓰면 버려진다.
 *
 * 전역 레지스터라 뱅크와 무관하게 하나뿐이다.
 */
static void qcom_adreno_smmu_set_prr_addr(const void *cookie, phys_addr_t page_addr)
{
	struct arm_smmu_domain *smmu_domain = (void *)cookie;	/* [한국어] 넘겨 둔 도메인. */
	struct arm_smmu_device *smmu = smmu_domain->smmu;	/* [한국어] 그 SMMU. */
	int ret;	/* [한국어] 전원 결과. */

	ret = pm_runtime_resume_and_get(smmu->dev);	/* [한국어] 레지스터를 쓰려면 켜야 한다. */
	if (ret < 0) {	/* [한국어] 켜지 못했으면 */
		dev_err(smmu->dev, "failed to get runtime PM: %d\n", ret);	/* [한국어] 알리고 */
		return;	/* [한국어] 포기한다. */
	}

	writel_relaxed(lower_32_bits(page_addr),	/* [한국어] 물리 주소의 하위 절반과 */
				smmu->base + ARM_SMMU_GFX_PRR_CFG_LADDR);	/* [한국어] 그 레지스터. */
	writel_relaxed(upper_32_bits(page_addr),	/* [한국어] 상위 절반을 */
				smmu->base + ARM_SMMU_GFX_PRR_CFG_UADDR);	/* [한국어] 나눠 쓴다. 전역 레지스터라 뱅크와 무관하다. */
	pm_runtime_put_autosuspend(smmu->dev);	/* [한국어] 전원 참조를 놓는다. */
}

#define QCOM_ADRENO_SMMU_GPU_SID 0	/* [한국어] GPU 가 늘 쓰는 스트림 id. 이 값이 GPU 를 가려내는 판별 기준이 된다. */

/*
 * [한국어]
 * (위 영어 주석에 이어)
 * qcom_adreno_smmu_is_gpu_device - 이 장치가 GPU 인가
 *
 * @dev: 물어보는 장치.
 * @return: GPU 면 참.
 *
 * 원 주석대로 GPU 는 늘 스트림 id 0 을 쓴다. 그것이 GPU 를 가려내고
 * 프로세스별 페이지 테이블을 열어 주는 판별 기준이 된다.
 */
static bool qcom_adreno_smmu_is_gpu_device(struct device *dev)
{
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);	/* [한국어] 이 장치의 스트림 id 목록. */
	int i;	/* [한국어] 순회 첨자. */

	/*
	 * The GPU will always use SID 0 so that is a handy way to uniquely
	 * identify it and configure it for per-instance pagetables
	 */
	for (i = 0; i < fwspec->num_ids; i++) {	/* [한국어] id 마다 */
		u16 sid = FIELD_GET(ARM_SMMU_SMR_ID, fwspec->ids[i]);	/* [한국어] 값을 꺼내 */

		if (sid == QCOM_ADRENO_SMMU_GPU_SID)	/* [한국어] 0 이면 */
			return true;	/* [한국어] GPU 다. */
	}

	return false;	/* [한국어] 아니다. */
}

/*
 * [한국어]
 * qcom_adreno_smmu_get_ttbr1_cfg - 커널이 쥔 상위 표의 설정을 알려 준다
 *
 * @cookie: 이 도메인.
 * @return: 그 표의 설정.
 *
 * GPU 드라이버가 하위 표를 만들 때 상위 표와 같은 형식·같은 페이지
 * 크기를 써야 한다. 그 값을 여기서 얻는다.
 */
static const struct io_pgtable_cfg *qcom_adreno_smmu_get_ttbr1_cfg(
		const void *cookie)
{
	struct arm_smmu_domain *smmu_domain = (void *)cookie;	/* [한국어] 넘겨 둔 도메인. */
	struct io_pgtable *pgtable =	/* [한국어] 커널이 쥔 상위 표. */
		io_pgtable_ops_to_pgtable(smmu_domain->pgtbl_ops);	/* [한국어] 조작 함수에서 표 객체로 되짚는다. */
	return &pgtable->cfg;	/* [한국어] GPU 가 하위 표를 같은 형식으로 만들 수 있게 설정을 넘긴다. */
}

/*
 * Local implementation to configure TTBR0 with the specified pagetable config.
 * The GPU driver will call this to enable TTBR0 when per-instance pagetables
 * are active
 */

/*
 * [한국어]
 * (위 영어 주석에 이어)
 * qcom_adreno_smmu_set_ttbr0_cfg - GPU 가 하위 표를 갈아 끼운다
 *
 * @cookie: 이 도메인.
 * @pgtbl_cfg: 새 표의 설정. NULL 이면 하위 표를 끈다.
 * @return: 0 성공, 음수면 실패.
 *
 * 프로세스별 페이지 테이블의 핵심 통로다. GPU 드라이버가 프로세스를
 * 바꿀 때마다 이것을 불러 하위 표를 갈아 끼운다.
 *
 * 상위 표(커널이 쥔 것)는 그대로 두어, GPU 자신의 자료 구조가 계속
 * 보이게 한다. 그래서 분리 페이지 테이블이 먼저 켜져 있어야 한다.
 *
 * 이미 그 상태면 거절하는 것이 눈에 띈다 — 같은 요청을 두 번 하면
 * 설정이 어긋날 수 있다.
 */
static int qcom_adreno_smmu_set_ttbr0_cfg(const void *cookie,
		const struct io_pgtable_cfg *pgtbl_cfg)
{
	struct arm_smmu_domain *smmu_domain = (void *)cookie;	/* [한국어] 넘겨 둔 도메인. */
	struct io_pgtable *pgtable = io_pgtable_ops_to_pgtable(smmu_domain->pgtbl_ops);	/* [한국어] 상위 표. 되돌릴 때 그 설정을 쓴다. */
	struct arm_smmu_cfg *cfg = &smmu_domain->cfg;	/* [한국어] 뱅크 설정. */
	struct arm_smmu_cb *cb = &smmu_domain->smmu->cbs[cfg->cbndx];	/* [한국어] 그 뱅크의 그림자 구조체. */

	/* The domain must have split pagetables already enabled */
	if (cb->tcr[0] & ARM_SMMU_TCR_EPD1)	/* [한국어] 상위 표가 꺼져 있으면 */
		return -EINVAL;	/* [한국어] 분리 페이지 테이블이 아니라 이 통로를 쓸 수 없다. */

	/* If the pagetable config is NULL, disable TTBR0 */
	if (!pgtbl_cfg) {	/* [한국어] NULL 이면 하위 표를 끄라는 뜻이다. */
		/* Do nothing if it is already disabled */
		if ((cb->tcr[0] & ARM_SMMU_TCR_EPD0))	/* [한국어] 이미 꺼져 있으면 */
			return -EINVAL;	/* [한국어] 두 번 끄는 것은 버그다. */

		/* Set TCR to the original configuration */
		cb->tcr[0] = arm_smmu_lpae_tcr(&pgtable->cfg);	/* [한국어] 상위 표만 쓰는 설정으로 되돌린다. */
		cb->ttbr[0] = FIELD_PREP(ARM_SMMU_TTBRn_ASID, cb->cfg->asid);	/* [한국어] 표 주소는 지우고 ASID 만 남긴다. */
	} else {
		u32 tcr = cb->tcr[0];	/* [한국어] 지금 설정에서 시작한다. */

		/* Don't call this again if TTBR0 is already enabled */
		if (!(cb->tcr[0] & ARM_SMMU_TCR_EPD0))	/* [한국어] 이미 켜져 있으면 */
			return -EINVAL;	/* [한국어] 두 번 켜면 설정이 어긋난다. */

		tcr |= arm_smmu_lpae_tcr(pgtbl_cfg);	/* [한국어] 새 표의 설정을 얹고 */
		tcr &= ~(ARM_SMMU_TCR_EPD0 | ARM_SMMU_TCR_EPD1);	/* [한국어] 두 표 순회를 모두 켠다. */

		cb->tcr[0] = tcr;	/* [한국어] 그림자에 담고 */
		cb->ttbr[0] = pgtbl_cfg->arm_lpae_s1_cfg.ttbr;	/* [한국어] 새 표의 주소와 */
		cb->ttbr[0] |= FIELD_PREP(ARM_SMMU_TTBRn_ASID, cb->cfg->asid);	/* [한국어] ASID 를 함께 담는다. */
	}

	arm_smmu_write_context_bank(smmu_domain->smmu, cb->cfg->cbndx);	/* [한국어] 하드웨어에 반영한다. 이 순간 GPU 가 새 표를 쓰기 시작한다. */

	return 0;	/* [한국어] 성공. */
}

/*
 * [한국어]
 * qcom_adreno_smmu_alloc_context_bank - GPU 에게 0번 뱅크를 준다
 *
 * @smmu_domain: 대상 도메인.
 * @smmu: 그 SMMU.
 * @dev: 붙는 장치.
 * @start: 공통 코드가 제안한 시작 번호(무시한다).
 * @return: 배정받은 번호, 없으면 음수.
 *
 * 원 주석대로 GPU 하드웨어가 페이지 테이블을 스스로 갈아 끼우려면
 * 0번 뱅크여야 한다 — 그 번호가 GPU 레지스터에 박혀 있다.
 *
 * 다른 장치는 1번부터 쓰게 해 그 자리를 비워 둔다.
 */
static int qcom_adreno_smmu_alloc_context_bank(struct arm_smmu_domain *smmu_domain,
					       struct arm_smmu_device *smmu,
					       struct device *dev, int start)
{
	int count;	/* [한국어] 찾을 범위의 크기. */

	/*
	 * Assign context bank 0 to the GPU device so the GPU hardware can
	 * switch pagetables
	 */
	if (qcom_adreno_smmu_is_gpu_device(dev)) {	/* [한국어] GPU 면 */
		start = 0;	/* [한국어] 원 주석대로 0번 뱅크여야 하드웨어가 표를 스스로 갈아 끼울 수 있다. */
		count = 1;	/* [한국어] 그 하나만 본다. */
	} else {
		start = 1;	/* [한국어] 다른 장치는 1번부터 */
		count = smmu->num_context_banks;	/* [한국어] 끝까지. */
	}

	return __arm_smmu_alloc_bitmap(smmu->context_map, start, count);	/* [한국어] 그 범위에서 빈 자리를 잡는다. */
}

/*
 * [한국어]
 * qcom_adreno_can_do_ttbr1 - 이 SoC 가 분리 페이지 테이블을 쓸 수 있는가
 *
 * @smmu: 대상 SMMU.
 * @return: 쓸 수 있으면 참.
 *
 * msm8996 은 그 기능이 온전하지 않아 제외한다. 그 밖에는 모두 된다고
 * 본다 — 부정 목록이 긍정 목록보다 짧기 때문이다.
 */
static bool qcom_adreno_can_do_ttbr1(struct arm_smmu_device *smmu)
{
	const struct device_node *np = smmu->dev->of_node;	/* [한국어] 장치 트리 노드. */

	if (of_device_is_compatible(np, "qcom,msm8996-smmu-v2"))	/* [한국어] 이 SoC 는 분리 표가 온전하지 않다. */
		return false;	/* [한국어] 쓰지 않는다. */

	return true;	/* [한국어] 그 밖에는 모두 된다고 본다. */
}

/*
 * [한국어]
 * qcom_smmu_set_actlr_dev - 이 장치에 맞는 성능 설정을 심는다
 *
 * @dev: 붙는 장치.
 * @smmu: 그 SMMU.
 * @cbndx: 그 뱅크 번호.
 * @client_match: SoC 별 장치 매칭 표.
 *
 * 미리 읽기 깊이와 캐시 방식은 장치 성격에 따라 달라야 한다. 표에 없는
 * 장치는 하드웨어 기본값을 쓴다.
 */
static void qcom_smmu_set_actlr_dev(struct device *dev, struct arm_smmu_device *smmu, int cbndx,
		const struct of_device_id *client_match)
{
	const struct of_device_id *match =	/* [한국어] 이 장치가 표에 있는지 본다. */
			of_match_device(client_match, dev);	/* [한국어] compatible 로 찾는다. */

	if (!match) {	/* [한국어] 없으면 */
		dev_dbg(dev, "no ACTLR settings present\n");	/* [한국어] 하드웨어 기본값을 쓴다. */
		return;	/* [한국어] 건드리지 않는다. */
	}

	arm_smmu_cb_write(smmu, cbndx, ARM_SMMU_CB_ACTLR, (unsigned long)match->data);	/* [한국어] 표에 적힌 값을 그대로 쓴다 — 미리 읽기 깊이와 캐시 방식이 담겨 있다. */
}

/*
 * [한국어]
 * qcom_adreno_smmu_init_context - GPU 도메인을 세운다
 *
 * @smmu_domain: 만들어지는 도메인.
 * @pgtbl_cfg: 표 설정.
 * @dev: 붙는 장치.
 * @return: 늘 0.
 *
 * GPU 가 아니면 성능 설정만 심고 끝난다.
 *
 * GPU 면 원 주석대로 분리 페이지 테이블을 켠다. 그때 상위 표를 커널이
 * 쓰게 되어, 하위는 GPU 가 프로세스마다 갈아 끼울 수 있다.
 *
 * 원 주석이 확인의 이유를 밝힌다 — qcom,adreno-smmu 를 쓰는 대상은
 * 모두 64비트 1단계여야 하지만, arm-smmu 코드가 TTBR1 예외를 켤 때
 * 그것을 전제하므로 다시 확인한다.
 *
 * 마지막에 GPU 드라이버와의 인터페이스를 채운다. PRR 콜백은 그 기능이
 * 있는 SoC 에서만 단다.
 *
 * ASID 통째 무효화를 선호한다고 표시하는 것도 눈에 띈다 — 퀄컴 하드웨어는
 * 구간 무효화가 느리다.
 */
static int qcom_adreno_smmu_init_context(struct arm_smmu_domain *smmu_domain,
		struct io_pgtable_cfg *pgtbl_cfg, struct device *dev)
{
	const struct device_node *np = smmu_domain->smmu->dev->of_node;	/* [한국어] SMMU 의 트리 노드. */
	struct arm_smmu_device *smmu = smmu_domain->smmu;	/* [한국어] 그 SMMU. */
	struct qcom_smmu *qsmmu = to_qcom_smmu(smmu);	/* [한국어] 퀄컴 구조체. */
	const struct of_device_id *client_match;	/* [한국어] 장치별 성능 설정 표. */
	int cbndx = smmu_domain->cfg.cbndx;	/* [한국어] 뱅크 번호. */
	struct adreno_smmu_priv *priv;	/* [한국어] GPU 드라이버와의 인터페이스. */

	smmu_domain->cfg.flush_walk_prefer_tlbiasid = true;	/* [한국어] 퀄컴 하드웨어는 구간 무효화가 느려 ASID 통째 비우기를 선호한다. */

	client_match = qsmmu->data->client_match;	/* [한국어] 이 SoC 의 장치 표. */

	if (client_match)	/* [한국어] 있으면 */
		qcom_smmu_set_actlr_dev(dev, smmu, cbndx, client_match);	/* [한국어] 성능 설정을 심는다. */

	/* Only enable split pagetables for the GPU device (SID 0) */
	if (!qcom_adreno_smmu_is_gpu_device(dev))	/* [한국어] GPU 가 아니면 */
		return 0;	/* [한국어] 여기서 끝이다. */

	/*
	 * All targets that use the qcom,adreno-smmu compatible string *should*
	 * be AARCH64 stage 1 but double check because the arm-smmu code assumes
	 * that is the case when the TTBR1 quirk is enabled
	 */
	if (qcom_adreno_can_do_ttbr1(smmu_domain->smmu) &&	/* [한국어] 분리 표를 쓸 수 있는 SoC 이고 */
	    (smmu_domain->stage == ARM_SMMU_DOMAIN_S1) &&	/* [한국어] 1단계이며 */
	    (smmu_domain->cfg.fmt == ARM_SMMU_CTX_FMT_AARCH64))	/* [한국어] 64비트 형식이면 */
		pgtbl_cfg->quirks |= IO_PGTABLE_QUIRK_ARM_TTBR1;	/* [한국어] 상위 표를 커널이 쓰게 해, 하위를 GPU 가 갈아 끼울 수 있게 한다. */

	/*
	 * Initialize private interface with GPU:
	 */

	priv = dev_get_drvdata(dev);	/* [한국어] GPU 드라이버가 미리 마련해 둔 인터페이스 구조체. */
	priv->cookie = smmu_domain;	/* [한국어] 콜백에 되돌려 줄 값. */
	priv->get_ttbr1_cfg = qcom_adreno_smmu_get_ttbr1_cfg;	/* [한국어] 상위 표 설정을 알려 주는 통로. */
	priv->set_ttbr0_cfg = qcom_adreno_smmu_set_ttbr0_cfg;	/* [한국어] 하위 표를 갈아 끼우는 통로. */
	priv->get_fault_info = qcom_adreno_smmu_get_fault_info;	/* [한국어] 폴트 정보를 넘기는 통로. */
	priv->set_stall = qcom_adreno_smmu_set_stall;	/* [한국어] 멈춰 세우기를 켜고 끄는 통로. */
	priv->set_prr_bit = NULL;	/* [한국어] PRR 은 그 기능이 있는 SoC 에서만 단다. */
	priv->set_prr_addr = NULL;	/* [한국어] 같은 이유로 비워 둔다. */

	if (of_device_is_compatible(np, "qcom,smmu-500") &&	/* [한국어] MMU-500 기반이고 */
	    !of_device_is_compatible(np, "qcom,sm8250-smmu-500") &&	/* [한국어] sm8250 은 제외하며 */
	    of_device_is_compatible(np, "qcom,adreno-smmu")) {	/* [한국어] GPU SMMU 이면 */
		priv->set_prr_bit = qcom_adreno_smmu_set_prr_bit;	/* [한국어] PRR 콜백을 단다. */
		priv->set_prr_addr = qcom_adreno_smmu_set_prr_addr;	/* [한국어] 주소 설정 콜백도. */
	}

	return 0;	/* [한국어] 성공. */
}

/*
 * [한국어] 항등 도메인을 써야 하는 장치들의 표.
 *
 * 화면 출력처럼 부팅 때부터 DMA 하고 있는 장치는 커널이 매핑을 새로
 * 만들 수 없다. 그래서 물리 주소를 그대로 쓰는 항등 도메인을 준다.
 *
 * GPU 도 여기 있는데, 그쪽은 자기 페이지 테이블을 스스로 관리하기 때문이다.
 */
static const struct of_device_id qcom_smmu_client_of_match[] __maybe_unused = {
	{ .compatible = "qcom,adreno" },	/* [한국어] GPU 는 자기 페이지 테이블을 스스로 관리해 항등 도메인을 준다. 아래는 부팅 때부터 DMA 하는 장치들이다. */
	{ .compatible = "qcom,adreno-gmu" },
	{ .compatible = "qcom,glymur-mdss" },
	{ .compatible = "qcom,mdp4" },
	{ .compatible = "qcom,mdss" },
	{ .compatible = "qcom,qcm2290-mdss" },
	{ .compatible = "qcom,sar2130p-mdss" },
	{ .compatible = "qcom,sc7180-mdss" },
	{ .compatible = "qcom,sc7180-mss-pil" },
	{ .compatible = "qcom,sc7280-mdss" },
	{ .compatible = "qcom,sc7280-mss-pil" },
	{ .compatible = "qcom,sc8180x-mdss" },
	{ .compatible = "qcom,sc8280xp-mdss" },
	{ .compatible = "qcom,sdm670-mdss" },
	{ .compatible = "qcom,sdm845-mdss" },
	{ .compatible = "qcom,sdm845-mss-pil" },
	{ .compatible = "qcom,sm6115-mdss" },
	{ .compatible = "qcom,sm6350-mdss" },
	{ .compatible = "qcom,sm6375-mdss" },
	{ .compatible = "qcom,sm8150-mdss" },
	{ .compatible = "qcom,sm8250-mdss" },
	{ .compatible = "qcom,x1e80100-mdss" },
	{ }
};

/*
 * [한국어]
 * qcom_smmu_init_context - 보통 장치의 도메인을 세운다
 *
 * @smmu_domain: 만들어지는 도메인.
 * @pgtbl_cfg: 표 설정(쓰지 않는다).
 * @dev: 붙는 장치.
 * @return: 늘 0.
 *
 * GPU 판에서 분리 페이지 테이블 부분을 뺀 것이다. 성능 설정을 심고
 * ASID 통째 무효화를 선호한다고 표시한다.
 */
static int qcom_smmu_init_context(struct arm_smmu_domain *smmu_domain,
		struct io_pgtable_cfg *pgtbl_cfg, struct device *dev)
{
	struct arm_smmu_device *smmu = smmu_domain->smmu;	/* [한국어] 그 SMMU. */
	struct qcom_smmu *qsmmu = to_qcom_smmu(smmu);	/* [한국어] 퀄컴 구조체. */
	const struct of_device_id *client_match;	/* [한국어] 장치별 성능 설정 표. */
	int cbndx = smmu_domain->cfg.cbndx;	/* [한국어] 뱅크 번호. */

	smmu_domain->cfg.flush_walk_prefer_tlbiasid = true;	/* [한국어] 구간 무효화가 느려 ASID 통째 비우기를 선호한다. */

	client_match = qsmmu->data->client_match;	/* [한국어] 이 SoC 의 장치 표. */

	if (client_match)	/* [한국어] 있으면 */
		qcom_smmu_set_actlr_dev(dev, smmu, cbndx, client_match);	/* [한국어] 성능 설정을 심는다. */

	return 0;	/* [한국어] 성공. */
}

/*
 * [한국어]
 * qcom_smmu_cfg_probe - 펌웨어의 버릇을 알아내고 남은 매핑을 이어받는다
 *
 * @smmu: 대상 SMMU.
 * @return: 늘 0.
 *
 * 이 파일에서 가장 중요한 함수다. 세 가지를 한다.
 *
 * 첫째, SoC 마다 잘못 알려 주는 뱅크 개수를 바로잡는다. 원 주석대로
 * MSM8998 의 LPASS SMMU 는 13개라고 답하지만 마지막 뱅크를 건드리면
 * 시스템이 죽는다.
 *
 * 둘째, 펌웨어의 S2CR 버릇을 실험으로 알아낸다. 원 주석대로 어떤 판은
 * "오류" 쓰기를 무시하고 "우회" 쓰기를 오류로 바꾼다. 우회를 써 보고
 * 되읽어 그것이 유지되는지 본다. 버릇이 있으면 뱅크 하나를 예약해
 * 우회를 흉내 내는 데 쓴다.
 *
 * 그 위에 128개를 넘는 매핑 그룹의 문제도 겹친다 — 원 주석대로 규격
 * 최대치를 넘는 추가 레지스터를 하이퍼바이저가 제대로 다루지 못하는
 * 판이 있어, 마지막 믿을 만한 그룹으로 시험한다.
 *
 * 셋째, 펌웨어가 이미 세워 둔 매핑을 읽어 그림자 상태에 담는다. 그것을
 * 우회로 표시해 두면 reset 이 그대로 하드웨어에 써, 화면이 계속 나온다.
 */
static int qcom_smmu_cfg_probe(struct arm_smmu_device *smmu)
{
	struct qcom_smmu *qsmmu = to_qcom_smmu(smmu);	/* [한국어] 퀄컴 구조체. */
	unsigned int last_s2cr;	/* [한국어] 시험에 쓸 마지막 항목의 오프셋. */
	u32 reg;	/* [한국어] 읽고 쓸 레지스터 값. */
	u32 smr;	/* [한국어] 읽어 온 매칭 레지스터. */
	int i;	/* [한국어] 순회 첨자. */

	/*
	 * MSM8998 LPASS SMMU reports 13 context banks, but accessing
	 * the last context bank crashes the system.
	 */
	if (of_device_is_compatible(smmu->dev->of_node, "qcom,msm8998-smmu-v2") &&	/* [한국어] 원 주석대로 MSM8998 의 LPASS SMMU 는 */
	    smmu->num_context_banks == 13) {	/* [한국어] 13개라고 답하지만 */
		smmu->num_context_banks = 12;	/* [한국어] 마지막을 건드리면 시스템이 죽어 하나를 줄인다. */
	} else if (of_device_is_compatible(smmu->dev->of_node, "qcom,sdm630-smmu-v2")) {	/* [한국어] SDM630 계열은 */
		if (smmu->num_context_banks == 21) /* SDM630 / SDM660 A2NOC SMMU */	/* [한국어] A2NOC SMMU 면 */
			smmu->num_context_banks = 7;	/* [한국어] 7개만 쓸 수 있고 */
		else if (smmu->num_context_banks == 14) /* SDM630 / SDM660 LPASS SMMU */	/* [한국어] LPASS SMMU 면 */
			smmu->num_context_banks = 13;	/* [한국어] 13개다. 개수로 어느 SMMU 인지 가려낸다. */
	}

	/*
	 * Some platforms support more than the Arm SMMU architected maximum of
	 * 128 stream matching groups. The additional registers appear to have
	 * the same behavior as the architected registers in the hardware.
	 * However, on some firmware versions, the hypervisor does not
	 * correctly trap and emulate accesses to the additional registers,
	 * resulting in unexpected behavior.
	 *
	 * If there are more than 128 groups, use the last reliable group to
	 * detect if we need to apply the bypass quirk.
	 */
	if (smmu->num_mapping_groups > 128)	/* [한국어] 원 주석대로 규격 최대치를 넘으면 */
		last_s2cr = ARM_SMMU_GR0_S2CR(127);	/* [한국어] 하이퍼바이저가 제대로 다루는 마지막 항목으로 시험하고 */
	else
		last_s2cr = ARM_SMMU_GR0_S2CR(smmu->num_mapping_groups - 1);	/* [한국어] 아니면 진짜 마지막 항목을 쓴다. */

	/*
	 * With some firmware versions writes to S2CR of type FAULT are
	 * ignored, and writing BYPASS will end up written as FAULT in the
	 * register. Perform a write to S2CR to detect if this is the case and
	 * if so reserve a context bank to emulate bypass streams.
	 */
	reg = FIELD_PREP(ARM_SMMU_S2CR_TYPE, S2CR_TYPE_BYPASS) |	/* [한국어] 우회를 써 보고 */
	      FIELD_PREP(ARM_SMMU_S2CR_CBNDX, 0xff) |	/* [한국어] 뱅크 번호는 뜻이 없는 값으로, */
	      FIELD_PREP(ARM_SMMU_S2CR_PRIVCFG, S2CR_PRIVCFG_DEFAULT);	/* [한국어] 특권 속성은 기본으로 둔다. */
	arm_smmu_gr0_write(smmu, last_s2cr, reg);	/* [한국어] 쓴 뒤 */
	reg = arm_smmu_gr0_read(smmu, last_s2cr);	/* [한국어] 되읽는다. */
	if (FIELD_GET(ARM_SMMU_S2CR_TYPE, reg) != S2CR_TYPE_BYPASS) {	/* [한국어] 우회가 유지되지 않았으면 펌웨어에 그 버릇이 있다. */
		qsmmu->bypass_quirk = true;	/* [한국어] 표시해 두고 */
		qsmmu->bypass_cbndx = smmu->num_context_banks - 1;	/* [한국어] 마지막 뱅크를 우회 흉내용으로 예약한다. */

		set_bit(qsmmu->bypass_cbndx, smmu->context_map);	/* [한국어] 다른 도메인이 그것을 잡지 못하게 막는다. */

		arm_smmu_cb_write(smmu, qsmmu->bypass_cbndx, ARM_SMMU_CB_SCTLR, 0);	/* [한국어] 그 뱅크는 변환을 켜지 않아, 보내진 스트림이 그냥 통과한다. */

		reg = FIELD_PREP(ARM_SMMU_CBAR_TYPE, CBAR_TYPE_S1_TRANS_S2_BYPASS);	/* [한국어] 1단계 변환·2단계 통과로 설정해 두고 */
		arm_smmu_gr1_write(smmu, ARM_SMMU_GR1_CBAR(qsmmu->bypass_cbndx), reg);	/* [한국어] 쓴다. */

		if (smmu->num_mapping_groups > 128) {	/* [한국어] 128개를 넘는 항목은 */
			dev_notice(smmu->dev, "\tLimiting the stream matching groups to 128\n");	/* [한국어] 하이퍼바이저가 제대로 다루지 못하므로 */
			smmu->num_mapping_groups = 128;	/* [한국어] 거기까지만 쓴다. */
		}
	}

	for (i = 0; i < smmu->num_mapping_groups; i++) {	/* [한국어] 모든 매핑 항목을 훑으며 */
		smr = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_SMR(i));	/* [한국어] 펌웨어가 세워 둔 값을 읽는다. */

		if (FIELD_GET(ARM_SMMU_SMR_VALID, smr)) {	/* [한국어] 유효한 항목이면 */
			/* Ignore valid bit for SMR mask extraction. */
			smr &= ~ARM_SMMU_SMR_VALID;	/* [한국어] 원 주석대로 마스크를 꺼낼 때 유효 비트가 섞이지 않게 지운다. */
			smmu->smrs[i].id = FIELD_GET(ARM_SMMU_SMR_ID, smr);	/* [한국어] id 와 */
			smmu->smrs[i].mask = FIELD_GET(ARM_SMMU_SMR_MASK, smr);	/* [한국어] 마스크를 그림자에 담고 */
			smmu->smrs[i].valid = true;	/* [한국어] 유효로 표시한다. */

			smmu->s2crs[i].type = S2CR_TYPE_BYPASS;	/* [한국어] 우회로 두어 그 장치가 계속 DMA 하게 한다. */
			smmu->s2crs[i].privcfg = S2CR_PRIVCFG_DEFAULT;	/* [한국어] 특권 속성은 기본으로. */
			smmu->s2crs[i].cbndx = 0xff;	/* [한국어] 우회에는 뱅크가 필요 없어 뜻 없는 값을 둔다. */
		}
	}

	return 0;	/* [한국어] 늘 성공. */
}

/*
 * [한국어]
 * (위 영어 주석과 함께 읽을 것)
 * qcom_adreno_smmuv2_cfg_probe - GPU SMMU 의 능력을 바로잡는다
 *
 * @smmu: 대상 SMMU.
 * @return: 늘 0.
 *
 * 원 주석대로 16KB 페이지를 지원한다고 알리지만 실제로는 동작하지
 * 않아 그 능력을 지운다.
 *
 * SDM630 은 보안 세계가 마지막 뱅크들을 쥐고 있어 리눅스에서 감춘다.
 */
static int qcom_adreno_smmuv2_cfg_probe(struct arm_smmu_device *smmu)
{
	/* Support for 16K pages is advertised on some SoCs, but it doesn't seem to work */
	smmu->features &= ~ARM_SMMU_FEAT_FMT_AARCH64_16K;	/* [한국어] 원 주석대로 지원한다고 알리지만 실제로는 동작하지 않는다. */

	/* TZ protects several last context banks, hide them from Linux */
	if (of_device_is_compatible(smmu->dev->of_node, "qcom,sdm630-smmu-v2") &&	/* [한국어] SDM630 이고 */
	    smmu->num_context_banks == 5)	/* [한국어] 뱅크가 5개면 */
		smmu->num_context_banks = 2;	/* [한국어] 원 주석대로 보안 세계가 뒤쪽을 쥐고 있어 감춘다. */

	return 0;	/* [한국어] 늘 성공. */
}

/*
 * [한국어]
 * qcom_smmu_write_s2cr - 펌웨어 버릇을 뜻을 뒤집어 우회한다
 *
 * @smmu: 대상 SMMU.
 * @idx: 항목 번호.
 *
 * 버릇이 없으면 규격대로 쓴다. 있으면 두 가지를 뒤집는다.
 *
 * 원 주석대로 "우회"를 쓰면 펌웨어가 "오류"로 바꿔 버리므로, 대신
 * 예약해 둔 뱅크로 보내는 "변환"을 쓴다. 그 뱅크는 아무 매핑도 없이
 * 통과만 시키게 설정해 두었다.
 *
 * 반대로 "오류"를 쓰면 펌웨어가 무시하므로, "우회"를 써서 펌웨어가
 * 그것을 오류로 바꾸게 한다 — 버릇을 거꾸로 이용하는 것이다.
 */
static void qcom_smmu_write_s2cr(struct arm_smmu_device *smmu, int idx)
{
	struct arm_smmu_s2cr *s2cr = smmu->s2crs + idx;	/* [한국어] 그림자 상태. */
	struct qcom_smmu *qsmmu = to_qcom_smmu(smmu);	/* [한국어] 퀄컴 구조체. */
	u32 cbndx = s2cr->cbndx;	/* [한국어] 보낼 뱅크 번호. */
	u32 type = s2cr->type;	/* [한국어] 다룰 방식. */
	u32 reg;	/* [한국어] 만들어 쓸 값. */

	if (qsmmu->bypass_quirk) {	/* [한국어] 펌웨어에 그 버릇이 있으면 */
		if (type == S2CR_TYPE_BYPASS) {	/* [한국어] 우회를 쓰려는 경우 */
			/*
			 * Firmware with quirky S2CR handling will substitute
			 * BYPASS writes with FAULT, so point the stream to the
			 * reserved context bank and ask for translation on the
			 * stream
			 */
			type = S2CR_TYPE_TRANS;	/* [한국어] 원 주석대로 변환으로 바꾸고 */
			cbndx = qsmmu->bypass_cbndx;	/* [한국어] 예약해 둔 뱅크로 보낸다. 그 뱅크가 변환을 켜지 않아 결과가 통과와 같다. */
		} else if (type == S2CR_TYPE_FAULT) {	/* [한국어] 오류를 쓰려는 경우 */
			/*
			 * Firmware with quirky S2CR handling will ignore FAULT
			 * writes, so trick it to write FAULT by asking for a
			 * BYPASS.
			 */
			type = S2CR_TYPE_BYPASS;	/* [한국어] 원 주석대로 우회를 쓴다 — 펌웨어가 그것을 오류로 바꿔 준다. */
			cbndx = 0xff;	/* [한국어] 뜻 없는 뱅크 번호. */
		}
	}

	reg = FIELD_PREP(ARM_SMMU_S2CR_TYPE, type) |	/* [한국어] 뒤집힌 뜻으로 값을 만들어 */
	      FIELD_PREP(ARM_SMMU_S2CR_CBNDX, cbndx) |	/* [한국어] 뱅크 번호와 */
	      FIELD_PREP(ARM_SMMU_S2CR_PRIVCFG, s2cr->privcfg);	/* [한국어] 특권 속성을 담는다. */
	arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_S2CR(idx), reg);	/* [한국어] 레지스터에 쓴다. */
}

/*
 * [한국어]
 * qcom_smmu_def_domain_type - 이 장치에 항등 도메인이 필요한가
 *
 * @dev: 대상 장치.
 * @return: 필요하면 IOMMU_DOMAIN_IDENTITY, 아니면 0.
 *
 * 위 표에 있는 장치는 물리 주소를 그대로 써야 한다.
 */
static int qcom_smmu_def_domain_type(struct device *dev)
{
	const struct of_device_id *match =	/* [한국어] 이 장치가 표에 있는지 본다. */
		of_match_device(qcom_smmu_client_of_match, dev);	/* [한국어] compatible 로 찾는다. */

	return match ? IOMMU_DOMAIN_IDENTITY : 0;	/* [한국어] 있으면 항등 도메인, 없으면 코어의 기본값. */
}

/*
 * [한국어]
 * qcom_sdm845_smmu500_reset - sdm845 전용 초기화
 *
 * @smmu: 대상 SMMU.
 * @return: 0 성공, 음수면 실패.
 *
 * 원 주석대로 wait-for-safe 논리를 끈다. 그것은 실시간 클라이언트를
 * 우선하는 장치인데, USB 나 UFS 처럼 실시간이 아닌 장치의 성능을
 * 떨어뜨린다.
 *
 * 그 논리는 보안 세계가 쥐고 있어 SCM 호출로 끈다.
 */
static int qcom_sdm845_smmu500_reset(struct arm_smmu_device *smmu)
{
	int ret;	/* [한국어] SCM 호출 결과. */

	arm_mmu500_reset(smmu);	/* [한국어] MMU-500 의 공통 결함 우회를 먼저 한다. */

	/*
	 * To address performance degradation in non-real time clients,
	 * such as USB and UFS, turn off wait-for-safe on sdm845 based boards,
	 * such as MTP and db845, whose firmwares implement secure monitor
	 * call handlers to turn on/off the wait-for-safe logic.
	 */
	ret = qcom_scm_qsmmu500_wait_safe_toggle(0);	/* [한국어] 원 주석대로 실시간이 아닌 장치의 성능을 떨어뜨리는 논리를 끈다. 보안 세계가 쥐고 있어 SCM 호출을 쓴다. */
	if (ret)	/* [한국어] 끄지 못했으면 */
		dev_warn(smmu->dev, "Failed to turn off SAFE logic\n");	/* [한국어] 성능만 떨어질 뿐이라 경고로 그친다. */

	return ret;	/* [한국어] 결과. */
}

/*
 * [한국어] 2판 퀄컴 SMMU 의 갈고리표.
 *
 * MMU-500 이 아니라 리셋 갈고리가 없다.
 */
static const struct arm_smmu_impl qcom_smmu_v2_impl = {
	.init_context = qcom_smmu_init_context,	/* [한국어] 성능 설정을 심고 ASID 통째 무효화를 선호한다고 표시한다. */
	.cfg_probe = qcom_smmu_cfg_probe,
	.def_domain_type = qcom_smmu_def_domain_type,
	.write_s2cr = qcom_smmu_write_s2cr,
	.tlb_sync = qcom_smmu_tlb_sync,
};

/*
 * [한국어] MMU-500 기반 퀄컴 SMMU 의 갈고리표.
 *
 * 디버그 기능이 빌드에 들어 있으면 확장된 폴트 처리기를 단다. 그것은
 * 클럭을 켜고 잠들 수 있어 스레드 인터럽트를 요구한다.
 */
static const struct arm_smmu_impl qcom_smmu_500_impl = {
	.init_context = qcom_smmu_init_context,	/* [한국어] 같음. */
	.cfg_probe = qcom_smmu_cfg_probe,
	.def_domain_type = qcom_smmu_def_domain_type,
	.reset = arm_mmu500_reset,
	.write_s2cr = qcom_smmu_write_s2cr,
	.tlb_sync = qcom_smmu_tlb_sync,
#ifdef CONFIG_ARM_SMMU_QCOM_DEBUG
	.context_fault = qcom_smmu_context_fault,
	.context_fault_needs_threaded_irq = true,
#endif
};

/*
 * [한국어] sdm845 전용 갈고리표.
 *
 * 위와 같되 리셋에서 wait-for-safe 를 끈다.
 */
static const struct arm_smmu_impl sdm845_smmu_500_impl = {
	.init_context = qcom_smmu_init_context,	/* [한국어] 같음. */
	.cfg_probe = qcom_smmu_cfg_probe,
	.def_domain_type = qcom_smmu_def_domain_type,
	.reset = qcom_sdm845_smmu500_reset,
	.write_s2cr = qcom_smmu_write_s2cr,
	.tlb_sync = qcom_smmu_tlb_sync,
#ifdef CONFIG_ARM_SMMU_QCOM_DEBUG
	.context_fault = qcom_smmu_context_fault,
	.context_fault_needs_threaded_irq = true,
#endif
};

/*
 * [한국어] 2판 GPU SMMU 의 갈고리표.
 *
 * 0번 뱅크 배정과 제어 레지스터 조작이 더해진다.
 */
static const struct arm_smmu_impl qcom_adreno_smmu_v2_impl = {
	.init_context = qcom_adreno_smmu_init_context,	/* [한국어] GPU 판 — 분리 페이지 테이블까지 연다. */
	.cfg_probe = qcom_adreno_smmuv2_cfg_probe,
	.def_domain_type = qcom_smmu_def_domain_type,
	.alloc_context_bank = qcom_adreno_smmu_alloc_context_bank,
	.write_sctlr = qcom_adreno_smmu_write_sctlr,
	.tlb_sync = qcom_smmu_tlb_sync,
	.context_fault_needs_threaded_irq = true,
};

/*
 * [한국어] MMU-500 기반 GPU SMMU 의 갈고리표.
 *
 * cfg_probe 가 없는 데 주의 — 500 판은 능력 값이 바르게 나온다.
 */
static const struct arm_smmu_impl qcom_adreno_smmu_500_impl = {
	.init_context = qcom_adreno_smmu_init_context,	/* [한국어] 같음. */
	.def_domain_type = qcom_smmu_def_domain_type,
	.reset = arm_mmu500_reset,
	.alloc_context_bank = qcom_adreno_smmu_alloc_context_bank,
	.write_sctlr = qcom_adreno_smmu_write_sctlr,
	.tlb_sync = qcom_smmu_tlb_sync,
	.context_fault_needs_threaded_irq = true,
};

/*
 * [한국어]
 * qcom_smmu_create - 구조체를 늘려 퀄컴 확장을 담는다
 *
 * @smmu: 공통 구조체.
 * @data: SoC 별 설정 묶음.
 * @return: 늘어난 구조체, 갈고리가 없으면 받은 것 그대로.
 *
 * GPU SMMU 노드면 GPU 용 갈고리표를 고른다 — 같은 SoC 라도 SMMU 마다
 * 성격이 다르다.
 *
 * SCM 이 아직 준비되지 않았으면 나중에 다시 시도하라고 알린다. 이
 * 파일의 여러 우회가 SCM 호출을 쓰기 때문이다.
 */
static struct arm_smmu_device *qcom_smmu_create(struct arm_smmu_device *smmu,
		const struct qcom_smmu_match_data *data)
{
	const struct device_node *np = smmu->dev->of_node;	/* [한국어] 장치 트리 노드. */
	const struct arm_smmu_impl *impl;	/* [한국어] 고를 갈고리표. */
	struct qcom_smmu *qsmmu;	/* [한국어] 늘릴 구조체. */

	if (!data)	/* [한국어] 설정이 없으면 */
		return ERR_PTR(-EINVAL);	/* [한국어] 다룰 수 없다. */

	if (np && of_device_is_compatible(np, "qcom,adreno-smmu"))	/* [한국어] GPU SMMU 노드면 */
		impl = data->adreno_impl;	/* [한국어] GPU 용 갈고리표를 고르고 */
	else
		impl = data->impl;	/* [한국어] 아니면 보통 표를 고른다. */

	if (!impl)	/* [한국어] 그 표가 없으면(msm8996 의 보통 장치 등) */
		return smmu;	/* [한국어] 갈고리 없이 그대로 돌려준다. */

	/* Check to make sure qcom_scm has finished probing */
	if (!qcom_scm_is_available())	/* [한국어] SCM 이 아직 준비되지 않았으면 */
		return ERR_PTR(dev_err_probe(smmu->dev, -EPROBE_DEFER,	/* [한국어] 나중에 다시 시도하라고 알린다. 여러 우회가 SCM 호출을 쓴다. */
			"qcom_scm not ready\n"));

	qsmmu = devm_krealloc(smmu->dev, smmu, sizeof(*qsmmu), GFP_KERNEL);	/* [한국어] 기존 구조체를 그대로 늘린다. */
	if (!qsmmu)	/* [한국어] 메모리가 없다. */
		return ERR_PTR(-ENOMEM);	/* [한국어] 실패. */

	qsmmu->smmu.impl = impl;	/* [한국어] 갈고리표를 달고 */
	qsmmu->data = data;	/* [한국어] SoC 별 설정도 기억한다. */

	return &qsmmu->smmu;	/* [한국어] 늘어난 구조체의 안쪽을 돌려준다. */
}

/* Implementation Defined Register Space 0 register offsets */
/*
 * [한국어] (위 영어 주석에 이어) 구현 정의 레지스터 0번 공간의 오프셋들.
 *
 * 디버그용 TBU 상태 레지스터의 자리다. SoC 세대마다 달라 표로 둔다.
 */
static const u32 qcom_smmu_impl0_reg_offset[] = {
	[QCOM_SMMU_TBU_PWR_STATUS]		= 0x2204,	/* [한국어] TBU 전원 상태 레지스터의 오프셋. */
	[QCOM_SMMU_STATS_SYNC_INV_TBU_ACK]	= 0x25dc,
	[QCOM_SMMU_MMU2QSS_AND_SAFE_WAIT_CNTR]	= 0x2670,
};

/*
 * [한국어] 그 오프셋 표를 담은 설정.
 */
static const struct qcom_smmu_config qcom_smmu_impl0_cfg = {
	.reg_offset = qcom_smmu_impl0_reg_offset,	/* [한국어] 위 오프셋 표를 가리킨다. */
};

/*
 * It is not yet possible to use MDP SMMU with the bypass quirk on the msm8996,
 * there are not enough context banks.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어) msm8996 의 설정.
 *
 * 원 주석대로 컨텍스트 뱅크가 모자라 우회 흉내를 쓸 수 없다. 그래서
 * 보통 장치용 갈고리를 아예 달지 않고 GPU 용만 둔다.
 */
static const struct qcom_smmu_match_data msm8996_smmu_data = {
	.impl = NULL,	/* [한국어] 보통 장치용 갈고리를 달지 않는다 — 뱅크가 모자라 우회 흉내를 쓸 수 없다. */
	.adreno_impl = &qcom_adreno_smmu_v2_impl,
};

/*
 * [한국어] 2판 퀄컴 SMMU 의 기본 설정.
 */
static const struct qcom_smmu_match_data qcom_smmu_v2_data = {
	.impl = &qcom_smmu_v2_impl,	/* [한국어] 2판 갈고리표. */
	.adreno_impl = &qcom_adreno_smmu_v2_impl,
};

/*
 * [한국어] sdm845 의 설정.
 *
 * 원 주석대로 GPU 는 별도의 sdm845-smmu-v2 장치가 맡아 여기 GPU 갈고리가
 * 없고, 디버그 설정도 없다.
 */
static const struct qcom_smmu_match_data sdm845_smmu_500_data = {
	.impl = &sdm845_smmu_500_impl,	/* [한국어] wait-for-safe 를 끄는 리셋이 들어 있다. */
	/*
	 * No need for adreno impl here. On sdm845 the Adreno SMMU is handled
	 * by the separate sdm845-smmu-v2 device.
	 */
	/* Also no debug configuration. */
};

/*
 * [한국어] MMU-500 기반 퀄컴 SMMU 의 기본 설정.
 *
 * 오늘날 대부분의 SoC 가 이것을 쓴다.
 */
static const struct qcom_smmu_match_data qcom_smmu_500_impl0_data = {
	.impl = &qcom_smmu_500_impl,	/* [한국어] MMU-500 기반 기본 갈고리표. */
	.adreno_impl = &qcom_adreno_smmu_500_impl,
	.cfg = &qcom_smmu_impl0_cfg,
	.client_match = qcom_smmu_actlr_client_of_match,
};

/*
 * Do not add any more qcom,SOC-smmu-500 entries to this list, unless they need
 * special handling and can not be covered by the qcom,smmu-500 entry.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어) SoC 별 매칭 표.
 *
 * 원 주석의 당부가 중요하다 — 특별한 처리가 필요하지 않다면 새 항목을
 * 더하지 말고 qcom,smmu-500 항목이 덮게 두라는 것이다. SoC 마다 줄을
 * 더하면 표가 끝없이 길어진다.
 */
static const struct of_device_id __maybe_unused qcom_smmu_impl_of_match[] = {
	{ .compatible = "qcom,msm8996-smmu-v2", .data = &msm8996_smmu_data },	/* [한국어] SoC 마다 어느 설정을 쓸지 적어 둔다. 아래 항목들은 대부분 같은 기본 설정을 가리킨다. */
	{ .compatible = "qcom,msm8998-smmu-v2", .data = &qcom_smmu_v2_data },
	{ .compatible = "qcom,qcm2290-smmu-500", .data = &qcom_smmu_500_impl0_data },
	{ .compatible = "qcom,qdu1000-smmu-500", .data = &qcom_smmu_500_impl0_data  },
	{ .compatible = "qcom,sc7180-smmu-500", .data = &qcom_smmu_500_impl0_data },
	{ .compatible = "qcom,sc7180-smmu-v2", .data = &qcom_smmu_v2_data },
	{ .compatible = "qcom,sc7280-smmu-500", .data = &qcom_smmu_500_impl0_data },
	{ .compatible = "qcom,sc8180x-smmu-500", .data = &qcom_smmu_500_impl0_data },
	{ .compatible = "qcom,sc8280xp-smmu-500", .data = &qcom_smmu_500_impl0_data },
	{ .compatible = "qcom,sdm630-smmu-v2", .data = &qcom_smmu_v2_data },
	{ .compatible = "qcom,sdm670-smmu-v2", .data = &qcom_smmu_v2_data },
	{ .compatible = "qcom,sdm845-smmu-v2", .data = &qcom_smmu_v2_data },
	{ .compatible = "qcom,sdm845-smmu-500", .data = &sdm845_smmu_500_data },
	{ .compatible = "qcom,sm6115-smmu-500", .data = &qcom_smmu_500_impl0_data},
	{ .compatible = "qcom,sm6125-smmu-500", .data = &qcom_smmu_500_impl0_data },
	{ .compatible = "qcom,sm6350-smmu-v2", .data = &qcom_smmu_v2_data },
	{ .compatible = "qcom,sm6350-smmu-500", .data = &qcom_smmu_500_impl0_data },
	{ .compatible = "qcom,sm6375-smmu-v2", .data = &qcom_smmu_v2_data },
	{ .compatible = "qcom,sm6375-smmu-500", .data = &qcom_smmu_500_impl0_data },
	{ .compatible = "qcom,sm7150-smmu-v2", .data = &qcom_smmu_v2_data },
	{ .compatible = "qcom,sm8150-smmu-500", .data = &qcom_smmu_500_impl0_data },
	{ .compatible = "qcom,sm8250-smmu-500", .data = &qcom_smmu_500_impl0_data },
	{ .compatible = "qcom,sm8350-smmu-500", .data = &qcom_smmu_500_impl0_data },
	{ .compatible = "qcom,sm8450-smmu-500", .data = &qcom_smmu_500_impl0_data },
	{ .compatible = "qcom,smmu-500", .data = &qcom_smmu_500_impl0_data },
	{ }
};

#ifdef CONFIG_ACPI	/* [한국어] ACPI 부팅 지원은 설정으로 뺄 수 있다. */
/*
 * [한국어] ACPI 로 부팅하는 퀄컴 기계들의 표.
 *
 * ACPI 에는 compatible 문자열이 없어, 제조사와 모델 이름으로 가려낸다.
 */
static struct acpi_platform_list qcom_acpi_platlist[] = {
	{ "LENOVO", "CB-01   ", 0x8180, ACPI_SIG_IORT, equal, "QCOM SMMU" },	/* [한국어] ACPI 에는 compatible 이 없어 제조사·모델 이름으로 가려낸다. */
	{ "QCOM  ", "QCOMEDK2", 0x8180, ACPI_SIG_IORT, equal, "QCOM SMMU" },
	{ }
};
#endif

/*
 * [한국어]
 * qcom_smmu_tbu_probe - TBU 장치를 잡는다
 *
 * @pdev: 그 플랫폼 장치.
 * @return: 0 성공, 음수면 실패.
 *
 * 디버그 기능이 꺼진 빌드에서도 이 장치를 잡아야 한다 — 그러지 않으면
 * 전원 도메인이 켜진 채로 남는다.
 */
static int qcom_smmu_tbu_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;	/* [한국어] 그 장치. */
	int ret;	/* [한국어] 결과 코드. */

	if (IS_ENABLED(CONFIG_ARM_SMMU_QCOM_DEBUG)) {	/* [한국어] 디버그 기능이 빌드에 들어 있으면 */
		ret = qcom_tbu_probe(pdev);	/* [한국어] TBU 를 목록에 등록한다. */
		if (ret)	/* [한국어] 실패하면 */
			return ret;	/* [한국어] 포기한다. */
	}

	if (dev->pm_domain) {	/* [한국어] 전원 도메인이 있으면 */
		pm_runtime_set_active(dev);	/* [한국어] 지금 켜져 있다고 알리고 */
		pm_runtime_enable(dev);	/* [한국어] 전원 관리를 켠다 — 그러지 않으면 켜진 채로 남는다. */
	}

	return 0;	/* [한국어] 성공. */
}

/*
 * [한국어] TBU 노드의 매칭 표.
 */
static const struct of_device_id qcom_smmu_tbu_of_match[] = {
	{ .compatible = "qcom,sc7280-tbu" },	/* [한국어] TBU 노드의 이름들. */
	{ .compatible = "qcom,sdm845-tbu" },
	{ }
};

/*
 * [한국어] TBU 를 잡는 플랫폼 드라이버.
 *
 * SMMU 본체와 별개로 등록한다 — 장치 트리에서 따로 있는 노드다.
 */
static struct platform_driver qcom_smmu_tbu_driver = {
	.driver = {	/* [한국어] 드라이버 코어에 알릴 정보. */
		.name           = "qcom_tbu",	/* [한국어] sysfs 와 로그에 보이는 이름. */
		.of_match_table = qcom_smmu_tbu_of_match,
	},
	.probe = qcom_smmu_tbu_probe,
};

/*
 * [한국어]
 * qcom_smmu_impl_init - 이 SMMU 가 퀄컴 것인지 가려 갈고리를 단다
 *
 * @smmu: 공통 구조체.
 * @return: 갈고리가 달린 구조체, 퀄컴이 아니면 받은 것 그대로.
 *
 * ACPI 로 부팅했으면 기계 이름으로, 장치 트리면 compatible 로 가린다.
 *
 * 마지막 WARN 이 친절하다 — GPU SMMU 인데 표에 없으면 프로세스별
 * 페이지 테이블이 깨진다는 것을 그 자리에서 알려 준다.
 */
struct arm_smmu_device *qcom_smmu_impl_init(struct arm_smmu_device *smmu)
{
	const struct device_node *np = smmu->dev->of_node;	/* [한국어] 장치 트리 노드. ACPI 부팅이면 NULL 이다. */
	const struct of_device_id *match;	/* [한국어] 매칭 결과. */

#ifdef CONFIG_ACPI	/* [한국어] ACPI 매칭은 그 지원이 있을 때만 한다. */
	if (np == NULL) {	/* [한국어] 장치 트리가 없으면 ACPI 부팅이다. */
		/* Match platform for ACPI boot */
		if (acpi_match_platform_list(qcom_acpi_platlist) >= 0)	/* [한국어] 기계 이름으로 가려 */
			return qcom_smmu_create(smmu, &qcom_smmu_500_impl0_data);	/* [한국어] 기본 설정을 단다. */
	}
#endif

	match = of_match_node(qcom_smmu_impl_of_match, np);	/* [한국어] 장치 트리면 compatible 로 찾는다. */
	if (match)	/* [한국어] 찾았으면 */
		return qcom_smmu_create(smmu, match->data);	/* [한국어] 그 설정으로 갈고리를 단다. */

	/*
	 * If you hit this WARN_ON() you are missing an entry in the
	 * qcom_smmu_impl_of_match[] table, and GPU per-process page-
	 * tables will be broken.
	 */
	WARN(of_device_is_compatible(np, "qcom,adreno-smmu"),	/* [한국어] 원 주석대로 GPU SMMU 인데 표에 없으면 */
	     "Missing qcom_smmu_impl_of_match entry for: %s",	/* [한국어] 프로세스별 페이지 테이블이 깨진다. */
	     dev_name(smmu->dev));	/* [한국어] 어느 장치인지 함께 알린다. */

	return smmu;	/* [한국어] 퀄컴이 아니면 받은 것 그대로. */
}

/*
 * [한국어]
 * qcom_smmu_module_init - TBU 드라이버를 등록한다
 *
 * @return: 0 성공, 음수면 실패.
 */
int __init qcom_smmu_module_init(void)
{
	return platform_driver_register(&qcom_smmu_tbu_driver);	/* [한국어] TBU 를 잡을 드라이버를 등록한다. */
}

/*
 * [한국어]
 * qcom_smmu_module_exit - 그것을 걷어 낸다
 */
void __exit qcom_smmu_module_exit(void)
{
	platform_driver_unregister(&qcom_smmu_tbu_driver);	/* [한국어] 그것을 걷어 낸다. */
}
