// SPDX-License-Identifier: GPL-2.0-only
// Miscellaneous Arm SMMU implementation and integration quirks
// Copyright (C) 2019 Arm Limited

/*
 * [한국어 설명] 규격을 벗어난 구현들을 덮는 갈고리 모음 (arm-smmu-impl.c)
 *
 * === 파일의 역할 ===
 * 위 영어 주석대로 "잡다한 구현·통합 예외"를 모아 둔 파일이다. ARM 이
 * 낸 규격을 여러 회사가 조금씩 다르게 구현했고, 그 차이를 본체 코드에
 * 흩뿌리지 않으려고 여기 모았다.
 *
 * 예외의 성격이 셋으로 나뉜다. 첫째는 레지스터가 다른 자리에 있는 경우
 * (Calxeda) — 읽기·쓰기를 가로채 오프셋을 바꾼다. 둘째는 자원이 겹치면
 * 안 되는 경우(Cavium) — ASID/VMID 를 SMMU 마다 다른 범위에서 배정한다.
 * 셋째는 특정 기능을 꺼야 하는 경우(MMU-500, Marvell) — 초기화 때
 * 레지스터를 손보거나 능력 비트를 지운다.
 *
 * 하드웨어 결함(erratum) 번호가 주석에 붙어 있는 것이 이 파일의 특징이다.
 * 각 우회가 어떤 문제를 피하려는 것인지 그 번호로 추적할 수 있다.
 *
 * arm_smmu_impl_init 이 이 파일의 입구다. 모델과 장치 트리를 보고 알맞은
 * 갈고리표를 골라 단다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * arm-smmu.c 의 probe → arm_smmu_impl_init → 이 파일이 갈고리를 단다
 *   → 이후 모든 레지스터 접근과 초기화가 그 갈고리를 거친다
 *
 * 퀄컴과 NVIDIA 는 예외가 많아 별도 파일로 나뉘어 있고, 이 파일이
 * 그쪽 초기화 함수를 불러 준다.
 *
 * === 타 모듈과의 연결 ===
 * 위: arm-smmu.c 가 유일한 호출자다.
 * 옆: arm-smmu-nvidia.c, arm-smmu-qcom.c 로 갈라 보낸다.
 * 아래: arm-smmu.h 의 레지스터 정의와 접근 함수.
 *
 * === 주요 함수/구조체 요약 ===
 * arm_smmu_impl_init: 모델과 장치 트리를 보고 갈고리표를 고른다.
 * arm_smmu_read_ns / write_ns: Calxeda 의 보안 레지스터 배치를 비보안
 *   자리로 옮긴다.
 * cavium_cfg_probe / init_context: SMMU 마다 겹치지 않는 ASID 범위를 준다.
 * arm_mmu500_reset: MMU-500 의 여러 결함을 우회하는 초기화.
 * mrvl_mmu500_readq / writeq: 64비트 접근을 32비트 둘로 쪼갠다.
 */
#define pr_fmt(fmt) "arm-smmu: " fmt

#include <linux/bitfield.h>	/* [한국어] FIELD_GET — 판 번호를 레지스터에서 꺼낸다. */
#include <linux/of.h>	/* [한국어] 장치 트리 속성과 compatible 문자열을 본다. */

#include "arm-smmu.h"	/* [한국어] 레지스터 정의와 공통 자료 구조. */


/*
 * [한국어]
 * arm_smmu_gr0_ns - 보안 레지스터 오프셋을 비보안 자리로 옮긴다
 *
 * @offset: 원래 오프셋.
 * @return: 이 하드웨어에서 실제로 쓸 오프셋.
 *
 * Calxeda(옛 MMU-400 통합)는 보안 세계가 레지스터의 앞쪽 절반을 쥐고
 * 있어, 커널이 쓸 수 있는 것은 0x400 만큼 뒤의 비보안 별칭이다.
 *
 * 이름에 s 가 붙은 레지스터(sCR0 등)만 그 대상이다 — 그것이 곧 "보안
 * 세계와 공유하는 레지스터"라는 표시다.
 */
static int arm_smmu_gr0_ns(int offset)
{
	switch (offset) {	/* [한국어] 옮겨야 하는 레지스터인지 본다. */
	case ARM_SMMU_GR0_sCR0:	/* [한국어] 전역 설정. */
	case ARM_SMMU_GR0_sACR:	/* [한국어] 보조 설정. */
	case ARM_SMMU_GR0_sGFSR:	/* [한국어] 전역 오류 상태. */
	case ARM_SMMU_GR0_sGFSYNR0:	/* [한국어] 오류 부가 정보 0. */
	case ARM_SMMU_GR0_sGFSYNR1:	/* [한국어] 부가 정보 1. */
	case ARM_SMMU_GR0_sGFSYNR2:	/* [한국어] 부가 정보 2. 이름에 s 가 붙은 것이 곧 보안 세계와 공유한다는 표시다. */
		return offset + 0x400;	/* [한국어] 비보안 별칭이 그만큼 뒤에 있다. */
	default:	/* [한국어] 그 밖의 레지스터는 */
		return offset;	/* [한국어] 자리가 같다. */
	}
}

/*
 * [한국어]
 * arm_smmu_read_ns - 오프셋을 옮겨 읽는다
 *
 * @smmu: 대상 SMMU.
 * @page: 레지스터 페이지 번호.
 * @offset: 원래 오프셋.
 * @return: 읽은 값.
 *
 * 전역 페이지 0 의 접근만 옮긴다 — 그 밖의 레지스터는 자리가 같다.
 */
static u32 arm_smmu_read_ns(struct arm_smmu_device *smmu, int page,
			    int offset)
{
	if (page == ARM_SMMU_GR0)	/* [한국어] 전역 페이지 0 의 접근만 옮긴다. */
		offset = arm_smmu_gr0_ns(offset);	/* [한국어] 비보안 자리로 바꾼다. */
	return readl_relaxed(arm_smmu_page(smmu, page) + offset);	/* [한국어] 바뀐 자리에서 읽는다. */
}

/*
 * [한국어]
 * arm_smmu_write_ns - 오프셋을 옮겨 쓴다
 *
 * @smmu: 대상 SMMU.
 * @page: 레지스터 페이지 번호.
 * @offset: 원래 오프셋.
 * @val: 쓸 값.
 *
 * 읽기와 짝이다.
 */
static void arm_smmu_write_ns(struct arm_smmu_device *smmu, int page,
			      int offset, u32 val)
{
	if (page == ARM_SMMU_GR0)	/* [한국어] 같은 조건. */
		offset = arm_smmu_gr0_ns(offset);	/* [한국어] 비보안 자리로. */
	writel_relaxed(val, arm_smmu_page(smmu, page) + offset);	/* [한국어] 그 자리에 쓴다. */
}

/* Since we don't care for sGFAR, we can do without 64-bit accessors */
/*
 * [한국어] Calxeda 통합의 갈고리표.
 *
 * 위 영어 주석대로 sGFAR(전역 오류 주소)은 쓰지 않으므로 64비트 접근을
 * 옮길 필요가 없다 — 그래서 32비트 두 개만 채운다.
 */
static const struct arm_smmu_impl calxeda_impl = {
	.read_reg = arm_smmu_read_ns,	/* [한국어] 32비트 읽기를 가로챈다. */
	.write_reg = arm_smmu_write_ns,	/* [한국어] 32비트 쓰기를 가로챈다. */
};


/*
 * [한국어] Cavium SMMU. 공통 구조체에 SMMU 별 id 기준값을 더한 것이다.
 *
 * 아래 결함 우회를 위해 이 SMMU 가 쓸 ASID/VMID 범위의 시작을 기억한다.
 */
struct cavium_smmu {
	/* [한국어] 공통 SMMU 구조체. 반드시 첫 멤버여야 container_of 로 오갈 수 있다. */
	struct arm_smmu_device smmu;
	/* [한국어] 이 SMMU 가 쓸 ASID/VMID 범위의 시작.
	 *  설정자: cavium_cfg_probe 가 전역 카운터에서 잘라 온다.
	 *  읽는 자: cavium_init_context 가 배정된 id 에 더한다.
	 *  결함 #27704 때문에 SMMU 마다 겹치지 않는 범위가 필요하다. */
	u32 id_base;
};

/*
 * [한국어]
 * (아래 영어 주석과 함께 읽을 것)
 * cavium_cfg_probe - 이 SMMU 가 쓸 id 범위의 시작을 정한다
 *
 * @smmu: 대상 SMMU.
 * @return: 늘 0.
 *
 * 원 주석의 CN88xx 결함 #27704 — 시스템 안의 여러 SMMU 가 같은 ASID 나
 * VMID 를 쓰면 안 된다. 규격대로라면 SMMU 마다 독립된 이름 공간이지만,
 * 이 하드웨어는 그것이 새어 서로 간섭한다.
 *
 * 그래서 전역 카운터에서 뱅크 수만큼 잘라 이 SMMU 의 기준값으로 삼는다.
 * 그러면 SMMU 마다 겹치지 않는 범위를 갖는다.
 *
 * static 카운터를 함수 안에 둔 것은 이 함수만 쓰기 때문이다.
 */
static int cavium_cfg_probe(struct arm_smmu_device *smmu)
{
	static atomic_t context_count = ATOMIC_INIT(0);	/* [한국어] 시스템 전체에서 배정한 id 의 누적. 이 함수만 쓰므로 함수 안에 둔다. */
	struct cavium_smmu *cs = container_of(smmu, struct cavium_smmu, smmu);	/* [한국어] 늘려 둔 구조체로 되짚는다. */
	/*
	 * Cavium CN88xx erratum #27704.
	 * Ensure ASID and VMID allocation is unique across all SMMUs in
	 * the system.
	 */
	cs->id_base = atomic_fetch_add(smmu->num_context_banks, &context_count);	/* [한국어] 뱅크 수만큼 잘라 이 SMMU 의 몫으로 삼는다. 원자 연산이라 여러 SMMU 가 동시에 probe 돼도 겹치지 않는다. */
	dev_notice(smmu->dev, "\tenabling workaround for Cavium erratum 27704\n");	/* [한국어] 어떤 우회가 켜졌는지 로그에 남긴다. 나중에 문제를 추적할 때 단서가 된다. */

	return 0;	/* [한국어] 늘 성공한다. */
}

/*
 * [한국어]
 * cavium_init_context - 배정된 id 에 기준값을 더한다
 *
 * @smmu_domain: 만들어지는 도메인.
 * @pgtbl_cfg: 페이지 테이블 설정(여기서는 쓰지 않는다).
 * @dev: 붙는 장치(여기서는 쓰지 않는다).
 * @return: 늘 0.
 *
 * 공통 코드가 뱅크 번호에서 id 를 만들어 두었으므로, 여기서 기준값만
 * 더하면 SMMU 사이에 겹치지 않게 된다.
 */
static int cavium_init_context(struct arm_smmu_domain *smmu_domain,
		struct io_pgtable_cfg *pgtbl_cfg, struct device *dev)
{
	struct cavium_smmu *cs = container_of(smmu_domain->smmu,	/* [한국어] 이 도메인이 매인 SMMU 의 늘어난 구조체. */
					      struct cavium_smmu, smmu);

	if (smmu_domain->stage == ARM_SMMU_DOMAIN_S2)	/* [한국어] 2단계 도메인이면 */
		smmu_domain->cfg.vmid += cs->id_base;	/* [한국어] VMID 에 기준값을 더한다. */
	else
		smmu_domain->cfg.asid += cs->id_base;	/* [한국어] 1단계면 ASID 에 더한다. 둘은 union 이라 한 자리를 나눠 쓴다. */

	return 0;	/* [한국어] 늘 성공한다. */
}

/*
 * [한국어] Cavium 의 갈고리표. 두 결함 우회 함수만 단다.
 */
static const struct arm_smmu_impl cavium_impl = {
	.cfg_probe = cavium_cfg_probe,	/* [한국어] 능력을 읽은 뒤 id 범위를 정한다. */
	.init_context = cavium_init_context,	/* [한국어] 도메인을 만들 때 그 범위를 반영한다. */
};

/*
 * [한국어]
 * cavium_smmu_impl_init - 구조체를 늘려 Cavium 확장을 담는다
 *
 * @smmu: 공통 구조체.
 * @return: 늘어난 구조체, 실패하면 오류 포인터.
 *
 * devm_krealloc 이 요점이다. 공통 코드가 이미 만들어 둔 구조체를 그대로
 * 늘려, 앞부분의 내용을 옮기지 않고 뒤에 필드를 붙인다.
 *
 * devm 판이라 장치가 사라질 때 저절로 해제된다.
 */
static struct arm_smmu_device *cavium_smmu_impl_init(struct arm_smmu_device *smmu)
{
	struct cavium_smmu *cs;	/* [한국어] 늘린 구조체. */

	cs = devm_krealloc(smmu->dev, smmu, sizeof(*cs), GFP_KERNEL);	/* [한국어] 기존 구조체를 그대로 늘린다 — 앞부분 내용이 보존되고, 장치가 사라질 때 저절로 해제된다. */
	if (!cs)	/* [한국어] 메모리가 없다. */
		return ERR_PTR(-ENOMEM);	/* [한국어] 실패. */

	cs->smmu.impl = &cavium_impl;	/* [한국어] 갈고리를 단다. */

	return &cs->smmu;	/* [한국어] 늘어난 구조체의 안쪽을 돌려준다. 호출자는 이 포인터로 바꿔 써야 한다. */
}


#define ARM_MMU500_ACTLR_CPRE		(1 << 1)	/* [한국어] 뱅크별 보조 제어의 미리 읽기 비트. 알려진 결함 다섯 가지의 원인이라 끈다. */

#define ARM_MMU500_ACR_CACHE_LOCK	(1 << 26)	/* [한국어] 보조 설정의 캐시 잠금. 서 있으면 뱅크별 ACTLR 쓰기가 먹히지 않는다. */
#define ARM_MMU500_ACR_S2CRB_TLBEN	(1 << 10)	/* [한국어] S2CR 우회 항목도 TLB 에 담게 한다. */
#define ARM_MMU500_ACR_SMTNMB_TLBEN	(1 << 8)	/* [한국어] 매칭되지 않은 스트림의 우회 항목도 TLB 에 담게 한다. 둘 다 지연을 줄이는 설정이다. */

/*
 * [한국어]
 * arm_mmu500_reset - MMU-500 의 여러 결함을 우회하는 초기화
 *
 * @smmu: 대상 SMMU.
 * @return: 늘 0.
 *
 * 세 가지를 한다.
 *
 * 첫째, 원 주석대로 r2p0 이후 판에서는 ACR 의 캐시 잠금을 풀어야 뱅크별
 * ACTLR 쓰기가 먹힌다. 보안 세계가 SACR 쪽 잠금도 풀어 주기를 바랄
 * 수밖에 없다는 주석이 그 한계를 솔직히 적어 두었다.
 *
 * 둘째, 매칭되지 않은 스트림도 우회 TLB 항목을 만들게 허용해 지연을
 * 줄인다.
 *
 * 셋째, 원 주석대로 다음 페이지 미리 읽기를 끈다 — 이득이 별로 없는데
 * 알려진 결함이 다섯 가지나 있다. 껐는데도 다시 켜져 있으면 보안 세계가
 * 잠가 둔 것이라, 그 사실을 로그로 알린다.
 */
int arm_mmu500_reset(struct arm_smmu_device *smmu)
{
	u32 reg, major;	/* [한국어] 읽어 온 레지스터 값과 판 번호. */
	/*
	 * On MMU-500 r2p0 onwards we need to clear ACR.CACHE_LOCK before
	 * writes to the context bank ACTLRs will stick. And we just hope that
	 * Secure has also cleared SACR.CACHE_LOCK for this to take effect...
	 */
	reg = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_ID7);	/* [한국어] 판 번호 레지스터. */
	major = FIELD_GET(ARM_SMMU_ID7_MAJOR, reg);	/* [한국어] 주 판 번호를 꺼낸다. */
	reg = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_sACR);	/* [한국어] 보조 설정을 읽어 */
	if (major >= 2)	/* [한국어] r2p0 이후 판이면 */
		reg &= ~ARM_MMU500_ACR_CACHE_LOCK;	/* [한국어] 캐시 잠금을 푼다 — 그래야 뱅크별 ACTLR 쓰기가 먹힌다. */
	/*
	 * Allow unmatched Stream IDs to allocate bypass
	 * TLB entries for reduced latency.
	 */
	reg |= ARM_MMU500_ACR_SMTNMB_TLBEN | ARM_MMU500_ACR_S2CRB_TLBEN;	/* [한국어] 매칭되지 않은 스트림도 우회 TLB 항목을 만들게 해 지연을 줄인다. */
	arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_sACR, reg);	/* [한국어] 고친 값을 되쓴다. */

#ifdef CONFIG_ARM_SMMU_MMU_500_CPRE_ERRATA	/* [한국어] 미리 읽기를 끄는 우회는 설정으로 뺄 수 있다 — 그 결함이 없는 판도 있기 때문이다. */
	/*
	 * Disable MMU-500's not-particularly-beneficial next-page
	 * prefetcher for the sake of at least 5 known errata.
	 */
	for (int i = 0; i < smmu->num_context_banks; ++i) {	/* [한국어] 모든 컨텍스트 뱅크에 대해 */
		reg = arm_smmu_cb_read(smmu, i, ARM_SMMU_CB_ACTLR);	/* [한국어] 보조 제어를 읽어 */
		reg &= ~ARM_MMU500_ACTLR_CPRE;	/* [한국어] 미리 읽기 비트를 지우고 */
		arm_smmu_cb_write(smmu, i, ARM_SMMU_CB_ACTLR, reg);	/* [한국어] 되쓴다. */
		reg = arm_smmu_cb_read(smmu, i, ARM_SMMU_CB_ACTLR);	/* [한국어] 정말 꺼졌는지 되읽어 확인한다. */
		if (reg & ARM_MMU500_ACTLR_CPRE)	/* [한국어] 아직 켜져 있으면 보안 세계가 잠가 둔 것이다. */
			dev_warn_once(smmu->dev, "Failed to disable prefetcher for errata workarounds, check SACR.CACHE_LOCK\n");	/* [한국어] 한 번만 경고한다 — 뱅크마다 같은 메시지를 쏟아 낼 이유가 없다. */
	}
#endif

	return 0;	/* [한국어] 늘 성공한다. */
}

/*
 * [한국어] ARM MMU-500 의 갈고리표. 리셋 절차만 바꾼다.
 */
static const struct arm_smmu_impl arm_mmu500_impl = {
	.reset = arm_mmu500_reset,	/* [한국어] 초기화 뒤 결함 우회를 적용한다. */
};

/*
 * [한국어]
 * (아래 영어 주석과 함께 읽을 것)
 * mrvl_mmu500_readq - 64비트 읽기를 32비트 둘로 쪼갠다
 *
 * @smmu: 대상 SMMU.
 * @page: 레지스터 페이지 번호.
 * @off: 오프셋.
 * @return: 읽은 값.
 *
 * 원 주석의 Armada-AP806 결함 #582743 — 이 연결망은 64비트 접근을
 * 제대로 전달하지 못한다. 그래서 32비트 두 번으로 나눈다.
 *
 * hi_lo 판인 것이 중요하다. 상위를 먼저 읽어야 하드웨어가 값을 잠가
 * 두어, 두 번 읽는 사이에 값이 바뀌어도 앞뒤가 맞는다.
 */
static u64 mrvl_mmu500_readq(struct arm_smmu_device *smmu, int page, int off)
{
	/*
	 * Marvell Armada-AP806 erratum #582743.
	 * Split all the readq to double readl
	 */
	return hi_lo_readq_relaxed(arm_smmu_page(smmu, page) + off);	/* [한국어] 상위를 먼저 읽어야 하드웨어가 값을 잠가 두어, 두 번 읽는 사이의 변화에 흔들리지 않는다. */
}

/*
 * [한국어]
 * mrvl_mmu500_writeq - 64비트 쓰기를 32비트 둘로 쪼갠다
 *
 * @smmu: 대상 SMMU.
 * @page: 레지스터 페이지 번호.
 * @off: 오프셋.
 * @val: 쓸 값.
 *
 * 같은 결함의 쓰기 쪽이다. 상위를 먼저 써야 하드웨어가 하위를 받는
 * 순간 온전한 값으로 반영한다.
 */
static void mrvl_mmu500_writeq(struct arm_smmu_device *smmu, int page, int off,
			       u64 val)
{
	/*
	 * Marvell Armada-AP806 erratum #582743.
	 * Split all the writeq to double writel
	 */
	hi_lo_writeq_relaxed(val, arm_smmu_page(smmu, page) + off);	/* [한국어] 상위를 먼저 쓰면 하위를 쓰는 순간 온전한 값으로 반영된다. */
}

/*
 * [한국어]
 * (아래 영어 주석과 함께 읽을 것)
 * mrvl_mmu500_cfg_probe - 64비트 표 형식을 아예 감춘다
 *
 * @smmu: 대상 SMMU.
 * @return: 늘 0.
 *
 * 원 주석대로 같은 결함을 피하는 다른 각도다. 64비트 표 형식을 쓰면
 * 64비트 레지스터 접근이 필요해지는데, 그 형식을 아예 지원하지 않는 것처럼
 * 만들어 32비트 접근만 쓰게 한다.
 *
 * 능력 비트를 지우는 것만으로 상위 코드가 알아서 다른 형식을 고른다 —
 * 능력 표를 하나 두고 그것만 고치면 되는 구조의 이점이다.
 */
static int mrvl_mmu500_cfg_probe(struct arm_smmu_device *smmu)
{

	/*
	 * Armada-AP806 erratum #582743.
	 * Hide the SMMU_IDR2.PTFSv8 fields to sidestep the AArch64
	 * formats altogether and allow using 32 bits access on the
	 * interconnect.
	 */
	smmu->features &= ~(ARM_SMMU_FEAT_FMT_AARCH64_4K |	/* [한국어] 64비트 표 형식 세 가지를 모두 지운다. */
			    ARM_SMMU_FEAT_FMT_AARCH64_16K |	/* [한국어] 그러면 상위 코드가 32비트 형식을 고르고, */
			    ARM_SMMU_FEAT_FMT_AARCH64_64K);	/* [한국어] 64비트 레지스터 접근이 필요 없어진다. */

	return 0;	/* [한국어] 늘 성공한다. */
}

/*
 * [한국어] Marvell Armada-AP806 의 갈고리표.
 *
 * MMU-500 이므로 그 리셋 절차를 그대로 쓰면서, 64비트 접근만 자기 것으로
 * 바꾼다.
 */
static const struct arm_smmu_impl mrvl_mmu500_impl = {
	.read_reg64 = mrvl_mmu500_readq,	/* [한국어] 64비트 읽기를 32비트 둘로 쪼갠다. */
	.write_reg64 = mrvl_mmu500_writeq,	/* [한국어] 64비트 쓰기도 마찬가지. */
	.cfg_probe = mrvl_mmu500_cfg_probe,	/* [한국어] 64비트 표 형식을 감춰 그런 접근이 아예 생기지 않게 한다. */
	.reset = arm_mmu500_reset,
};


/*
 * [한국어]
 * arm_smmu_impl_init - 이 하드웨어에 맞는 갈고리표를 고른다
 *
 * @smmu: 능력을 아직 읽지 않은 SMMU.
 * @return: 갈고리가 달린 구조체(늘어났을 수 있다), 실패하면 오류 포인터.
 *
 * 이 파일의 입구다. probe 가 가장 먼저 부른다.
 *
 * 원 주석이 순서의 이유를 밝힌다 — 모델별 예외를 먼저 달아야, 통합
 * 단계의 예외가 그것을 물려받아 확장할 수 있다. Marvell 이 MMU-500 의
 * 리셋을 그대로 쓰면서 접근만 바꾸는 것이 그 예다.
 *
 * 구조체를 늘려 돌려주는 경우가 있어(Cavium) 반환값을 반드시 받아야 한다.
 */
struct arm_smmu_device *arm_smmu_impl_init(struct arm_smmu_device *smmu)
{
	const struct device_node *np = smmu->dev->of_node;	/* [한국어] 장치 트리 노드. 통합 단계의 예외를 이것으로 가려낸다. */

	/*
	 * Set the impl for model-specific implementation quirks first,
	 * such that platform integration quirks can pick it up and
	 * inherit from it if necessary.
	 */
	switch (smmu->model) {	/* [한국어] 먼저 모델별 예외를 단다. */
	case ARM_MMU500:	/* [한국어] ARM 의 MMU-500 이면 */
		smmu->impl = &arm_mmu500_impl;	/* [한국어] 그 리셋 절차를 단다. */
		break;	/* [한국어] 통합 단계 검사로 넘어간다. */
	case CAVIUM_SMMUV2:	/* [한국어] Cavium 이면 */
		return cavium_smmu_impl_init(smmu);	/* [한국어] 구조체를 늘려야 해서 곧장 돌아간다. */
	default:	/* [한국어] 그 밖의 모델은 */
		break;	/* [한국어] 모델별 예외가 없다. */
	}

	/* This is implicitly MMU-400 */
	if (of_property_read_bool(np, "calxeda,smmu-secure-config-access"))	/* [한국어] 위 영어 주석대로 이 속성이 곧 MMU-400 임을 뜻한다. */
		smmu->impl = &calxeda_impl;	/* [한국어] 보안 레지스터 배치를 옮기는 갈고리. */

	if (of_device_is_compatible(np, "nvidia,tegra234-smmu") ||	/* [한국어] NVIDIA Tegra 계열이면 */
	    of_device_is_compatible(np, "nvidia,tegra194-smmu") ||	/* [한국어] 여러 세대를 함께 본다. */
	    of_device_is_compatible(np, "nvidia,tegra186-smmu"))	/* [한국어] 가장 오래된 판까지. */
		return nvidia_smmu_impl_init(smmu);	/* [한국어] 그쪽도 구조체를 늘려야 해서 곧장 돌아간다. */

	if (IS_ENABLED(CONFIG_ARM_SMMU_QCOM))	/* [한국어] 퀄컴 지원이 빌드에 들어 있으면 */
		smmu = qcom_smmu_impl_init(smmu);	/* [한국어] 퀄컴인지 그쪽이 판별한다. 아니면 받은 것을 그대로 돌려준다. */

	if (of_device_is_compatible(np, "marvell,ap806-smmu-500"))	/* [한국어] Marvell 통합이면 */
		smmu->impl = &mrvl_mmu500_impl;	/* [한국어] MMU-500 갈고리를 이것으로 덮어쓴다 — 그 표가 리셋을 물려받았다. */

	return smmu;	/* [한국어] 갈고리가 달린 구조체. */
}

/*
 * [한국어]
 * arm_smmu_impl_module_init - 구현체 모듈들의 초기화
 *
 * @return: 0 성공, 음수면 실패.
 *
 * 퀄컴 구현체는 별도 플랫폼 드라이버를 등록해야 해서, 모듈이 올라올 때
 * 한 번 불러 준다.
 */
int __init arm_smmu_impl_module_init(void)
{
	if (IS_ENABLED(CONFIG_ARM_SMMU_QCOM))	/* [한국어] 퀄컴 지원이 있으면 */
		return qcom_smmu_module_init();	/* [한국어] 그 플랫폼 드라이버를 등록한다. */

	return 0;	/* [한국어] 없으면 할 일이 없다. */
}

/*
 * [한국어]
 * arm_smmu_impl_module_exit - 그 정리
 *
 * 등록한 드라이버를 걷는다.
 */
void __exit arm_smmu_impl_module_exit(void)
{
	if (IS_ENABLED(CONFIG_ARM_SMMU_QCOM))	/* [한국어] 등록했으면 */
		qcom_smmu_module_exit();	/* [한국어] 걷어 낸다. */
}
