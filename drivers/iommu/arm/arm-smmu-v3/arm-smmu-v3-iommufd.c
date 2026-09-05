// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES
 */

/*
 * [한국어 설명] SMMUv3 를 게스트에게 그대로 내주는 iommufd 연동 (arm-smmu-v3-iommufd.c)
 *
 * === 파일의 역할 ===
 * 가상 머신 안의 게스트 커널이 자기 SMMU 를 직접 다루는 것처럼 보이게 만드는
 * 것이 이 파일의 일이다. 게스트는 자기 스트림 표 항목(vSTE)을 짓고, 자기
 * 문맥 서술자 표를 만들고, 무효화 명령을 낸다. 호스트는 그 요청을 그대로
 * 믿지 않고 — 게스트가 남의 메모리나 남의 VMID 에 손대지 못하도록 — 걸러 낸
 * 뒤 실제 하드웨어에 옮겨 준다. 이 "걸러 내기"가 파일의 절반을 차지한다.
 * 그 바탕에는 SMMUv3 의 중첩 변환(nested translation)이 있다. 게스트가 지은
 * 1단계 변환(게스트 가상 → 게스트 물리)을 호스트가 지은 2단계 변환(게스트
 * 물리 → 실제 물리)이 감싸므로, 게스트가 1단계를 아무렇게 지어도 2단계를
 * 벗어날 수 없다. 그래서 1단계는 게스트에게 통째로 맡길 수 있다.
 * 다만 STE 에는 1단계 설정 말고도 호스트가 쥐고 있어야 할 비트가 섞여 있고
 * (2단계 표 주소, VMID, 캐시 정책 등), 무효화 명령에도 VMID/스트림 번호처럼
 * 게스트가 적어서는 안 되는 자리가 있다. 그 경계를 정의한 것이 헤더의
 * STRTAB_STE_0/1_NESTING_ALLOWED 마스크이고, 이 파일이 그것을 강제한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 사용자 공간(대개 VMM)이 iommufd 로 요청을 내려보내는 길은 이렇다:
 *
 *   VMM (qemu 등)
 *     → IOMMU_VIOMMU_ALLOC ioctl
 *       → arm_smmu_get_viommu_size()   ← 이 파일 (중첩을 켜도 되는지 검사)
 *       → arm_vsmmu_init()             ← 이 파일 (2단계 부모와 VMID 를 잇는다)
 *     → IOMMU_HWPT_ALLOC (nested) ioctl
 *       → arm_vsmmu_alloc_domain_nested()  ← 이 파일 (게스트 vSTE 를 검사)
 *     → 장치 붙이기
 *       → arm_smmu_attach_dev_nested()     ← 이 파일 (실제 STE 를 짓고 쓴다)
 *     → IOMMU_VIOMMU_INVALIDATE ioctl
 *       → arm_vsmmu_cache_invalidate()     ← 이 파일 (명령을 걸러 큐에 넣는다)
 *
 * 반대 방향으로는, 하드웨어가 낸 폴트를 게스트에게 돌려주는 길이 있다:
 *
 *   이벤트 큐 인터럽트 → arm_smmu_evtq_thread()
 *     → arm_vmaster_report_event()   ← 이 파일 (스트림 번호를 게스트 것으로 바꿔 전달)
 *     → iommufd 의 vEVENTQ → VMM → 게스트 커널
 *
 * 실행 컨텍스트는 두 갈래다. 대부분은 사용자 공간 ioctl 이라 프로세스 문맥에서
 * 잠들 수 있고, arm_vmaster_report_event() 만 이벤트 큐 인터럽트 스레드에서
 * streams_mutex 를 쥔 채 불린다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽으로는 iommufd(drivers/iommu/iommufd/)의 viommu 계층에 얹힌다. iommufd 가
 * 객체 수명과 사용자 데이터 복사를 맡고, 이 파일은 그 위에서 SMMUv3 고유의
 * 검사와 변환만 한다. 게스트 장치 번호(vSID)와 실제 스트림 번호(SID)를 잇는
 * 표도 iommufd 쪽(vdevs xarray)이 관리하며, 이 파일은 조회만 한다.
 * 아래쪽으로는 arm-smmu-v3.c 의 도구를 쓴다 — arm_smmu_make_s2_domain_ste(),
 * arm_smmu_make_abort_ste(), arm_smmu_attach_prepare/commit(),
 * arm_smmu_install_ste_for_dev(), arm_smmu_cmdq_issue_cmdlist() 가 그것이다.
 * 하드웨어 레지스터를 직접 건드리는 곳은 arm_smmu_hw_info() 하나뿐이다.
 * 옆으로는 구현체별 확장(impl_ops)에 갈래를 넘긴다. Tegra241 처럼 자기만의
 * 가상 IOMMU 종류를 더한 하드웨어가 있어, 표준 종류가 아니면 그쪽으로 넘긴다.
 * 데이터 흐름으로 보면 게스트가 지은 값(vSTE, 무효화 명령)이 사용자 공간에서
 * 내려와 이 파일에서 검사·치환을 거친 뒤 하드웨어 자료 구조로 흘러가고,
 * 반대로 하드웨어가 낸 이벤트가 번호만 바뀌어 게스트로 올라간다.
 *
 * === 주요 함수/구조체 요약 ===
 * - arm_smmu_hw_info(): 게스트가 자기 SMMU 의 능력을 알 수 있도록 IDR 레지스터
 *   값을 사용자 공간에 그대로 보여 준다.
 * - arm_smmu_validate_vste(): 게스트가 지은 STE 에서 허용된 비트만 남기고
 *   나머지가 섞여 있으면 거부한다. 중첩 보안의 첫 관문이다.
 * - arm_smmu_make_nested_domain_ste(): 게스트의 vSTE 를 실제 STE 로 옮긴다.
 *   게스트가 무효 STE 를 주면 abort STE 로 바꿔, 게스트가 자기 오류를
 *   자기 이벤트 큐에서 보게 만든다.
 * - arm_vsmmu_convert_user_cmd(): 게스트 무효화 명령의 VMID/SID 자리를 호스트가
 *   정한 값으로 강제로 덮어쓴다. 게스트가 남의 캐시를 지우지 못하게 막는 핵심.
 * - arm_vsmmu_cache_invalidate(): 그렇게 걸러 낸 명령을 묶음으로 큐에 넣는다.
 * - arm_vmaster_report_event(): 하드웨어 이벤트의 실제 스트림 번호를 게스트가
 *   아는 번호로 바꿔 게스트에게 올린다.
 * - struct arm_vsmmu_invalidation_cmd: 사용자 명령 원본과 커널 내부 형식(호스트
 *   엔디안)을 같은 자리에 겹쳐 두어, 제자리 변환이 가능하게 한 union.
 */

#include <uapi/linux/iommufd.h>	/* [한국어] 사용자 공간과 주고받는 구조체 정의 — vSTE, 무효화 명령, 이벤트 형식이 여기 있다. */

#include "arm-smmu-v3.h"	/* [한국어] 이 드라이버의 자료 모델과 STE/명령 비트 정의. */

/*
 * [한국어]
 * arm_smmu_hw_info - 이 SMMU 의 능력 레지스터를 사용자 공간에 보여 준다
 *
 * @dev: 어느 장치가 매달린 SMMU 인지 알려 주는 장치.
 * @length: 돌려주는 정보의 크기를 적어 줄 자리.
 * @type: 사용자가 원한 형식이 들어오고, 실제로 준 형식을 적어 돌려준다.
 * @return: 새로 잡은 정보 버퍼 (호출자가 놓는다), 실패하면 ERR_PTR.
 *
 * 게스트 커널이 자기 SMMU 를 다루려면 그 하드웨어가 무엇을 지원하는지 알아야
 * 한다. VMM 이 이 정보를 받아 게스트에게 보여 줄 가상 SMMU 의 능력 레지스터를
 * 짓는다. 그래서 여기서는 IDR0~IDR5 와 IIDR, AIDR 을 그대로 읽어 넘긴다.
 * 능력 레지스터는 읽기 전용이라 그대로 노출해도 위험하지 않다.
 *
 * 표준 형식이 아닌 종류를 물으면 구현체별 확장(impl_ops)에게 넘긴다 —
 * Tegra 처럼 자기만의 정보를 더 보여 주는 하드웨어가 있기 때문이다.
 *
 * 실행 컨텍스트: iommufd ioctl. 잠들 수 있다.
 *
 * 호출 체인:
 *   IOMMU_GET_HW_INFO ioctl → iommufd → [이 함수] → readl_relaxed()
 */
void *arm_smmu_hw_info(struct device *dev, u32 *length,
		       enum iommu_hw_info_type *type)
{
	struct arm_smmu_master *master = dev_iommu_priv_get(dev);	/* [한국어] 장치에 붙여 둔 SMMU 쪽 상태를 꺼낸다. */
	const struct arm_smmu_impl_ops *impl_ops = master->smmu->impl_ops;	/* [한국어] 구현체별 갈고리표 — 없을 수도 있다. */
	struct iommu_hw_info_arm_smmuv3 *info;	/* [한국어] 사용자에게 돌려줄 정보 버퍼. */
	u32 __iomem *base_idr;	/* [한국어] IDR 레지스터들이 늘어선 자리의 시작 주소. __iomem 이라 반드시 접근자 함수로 읽어야 한다. */
	unsigned int i;	/* [한국어] IDR0~IDR5 를 훑을 반복자. */

	if (*type != IOMMU_HW_INFO_TYPE_DEFAULT &&	/* [한국어] 사용자가 "아무거나"를 요청한 것도 아니고. */
	    *type != IOMMU_HW_INFO_TYPE_ARM_SMMUV3) {	/* [한국어] 표준 SMMUv3 형식을 요청한 것도 아니라면. */
		if (!impl_ops || !impl_ops->hw_info)	/* [한국어] 그 종류를 다룰 구현체별 갈고리가 없다면. */
			return ERR_PTR(-EOPNOTSUPP);	/* [한국어] 지원하지 않는 형식이라고 알린다. */
		return impl_ops->hw_info(master->smmu, length, type);	/* [한국어] 있다면 그쪽에게 통째로 넘긴다. */
	}

	info = kzalloc_obj(*info);	/* [한국어] 0 으로 채운 정보 버퍼를 잡는다 — 안 채운 필드가 쓰레기로 새어 나가면 안 된다. */
	if (!info)	/* [한국어] 메모리가 없으면. */
		return ERR_PTR(-ENOMEM);	/* [한국어] 오류 포인터로 알린다. */

	base_idr = master->smmu->base + ARM_SMMU_IDR0;	/* [한국어] MMIO 창의 시작에서 IDR0 오프셋만큼 옮긴 자리. IDR 들은 연속으로 놓여 있다. */
	for (i = 0; i <= 5; i++)	/* [한국어] IDR0 부터 IDR5 까지 여섯 개. */
		info->idr[i] = readl_relaxed(base_idr + i);	/* [한국어] 순서 보장이 필요 없는 읽기라 relaxed 로 읽는다 — 능력 레지스터는 값이 변하지 않는다. */
	info->iidr = readl_relaxed(master->smmu->base + ARM_SMMU_IIDR);	/* [한국어] 구현자·제품·개정 번호. 게스트가 특정 하드웨어의 결함 우회를 켜는 근거가 된다. */
	info->aidr = readl_relaxed(master->smmu->base + ARM_SMMU_AIDR);	/* [한국어] 이 하드웨어가 따르는 SMMUv3 규격 개정 번호. */

	*length = sizeof(*info);	/* [한국어] 얼마나 채웠는지 알려 준다 — 사용자 쪽 구조체가 더 클 수도, 작을 수도 있다. */
	*type = IOMMU_HW_INFO_TYPE_ARM_SMMUV3;	/* [한국어] "아무거나"를 요청했더라도 실제로 준 형식이 무엇인지 정확히 적어 돌려준다. */

	return info;	/* [한국어] iommufd 가 사용자 공간으로 복사한 뒤 이 버퍼를 놓는다. */
}

/*
 * [한국어]
 * arm_smmu_make_nested_cd_table_ste - 게스트의 문맥 표를 가리키는 중첩 STE 를 짓는다
 *
 * @target: 지어 담을 STE 버퍼.
 * @master: 이 STE 를 쓰게 될 장치.
 * @nested_domain: 게스트가 준 vSTE 를 담고 있는 중첩 도메인.
 * @ats_enabled: 이 붙이기에서 ATS 를 켜기로 했는가.
 *
 * 게스트가 "내 문맥 서술자 표를 써라"고 지은 vSTE 를 실제 STE 로 옮긴다.
 * 순서가 중요하다 — 먼저 호스트의 2단계 설정으로 STE 를 통째로 짓고,
 * 그다음 게스트가 준 비트를 얹는다. 그래야 2단계 표 주소·VMID 같은 호스트
 * 몫이 게스트 값에 덮이지 않는다. 게스트 비트는 이미
 * arm_smmu_validate_vste() 에서 허용 마스크로 걸러진 상태다.
 *
 * 실행 컨텍스트: 붙이기 경로. 잠들지 않는다 (값을 짓기만 한다).
 *
 * 호출 체인:
 *   arm_smmu_make_nested_domain_ste() → [이 함수]
 *     → arm_smmu_make_s2_domain_ste()
 */
static void arm_smmu_make_nested_cd_table_ste(
	struct arm_smmu_ste *target, struct arm_smmu_master *master,
	struct arm_smmu_nested_domain *nested_domain, bool ats_enabled)
{
	arm_smmu_make_s2_domain_ste(	/* [한국어] 먼저 호스트의 2단계 설정으로 STE 전체를 짓는다 — 이것이 게스트를 가두는 바깥 울타리다. */
		target, master, nested_domain->vsmmu->s2_parent, ats_enabled);	/* [한국어] 부모 2단계 도메인의 VMID 와 표 주소가 여기서 채워진다. */

	target->data[0] = cpu_to_le64(STRTAB_STE_0_V |	/* [한국어] 첫 워드는 다시 짓는다 — 설정 갈래를 "중첩"으로 바꿔야 하기 때문이다. */
				      FIELD_PREP(STRTAB_STE_0_CFG,	/* [한국어] 1단계와 2단계를 모두 거치는 갈래. */
						 STRTAB_STE_0_CFG_NESTED));
	target->data[0] |= nested_domain->ste[0] &	/* [한국어] 게스트가 지은 첫 워드를 얹는다 — 게스트의 문맥 표 주소와 PASID 폭이 여기 들어 있다. */
			   ~cpu_to_le64(STRTAB_STE_0_CFG);	/* [한국어] 다만 설정 갈래 필드만은 게스트 값을 버린다 — 방금 우리가 정한 NESTED 를 지켜야 한다. */
	target->data[1] |= nested_domain->ste[1];	/* [한국어] 둘째 워드는 얹기만 한다 — 게스트 몫(캐시 정책, 멈춤 허용, ATS)과 호스트 몫이 다른 비트에 있어 겹치지 않는다. */
	/* Merge events for DoS mitigations on eventq */
	/* [한국어] (위 영어 주석 참고) 게스트가 폴트를 마구 낼 수 있으므로, 같은
	 * 원인의 이벤트를 하드웨어가 하나로 합치게 한다. 그러지 않으면 게스트 하나가
	 * 이벤트 큐를 가득 채워 호스트 전체의 폴트 처리를 마비시킬 수 있다. */
	target->data[1] |= cpu_to_le64(STRTAB_STE_1_MEV);	/* [한국어] 이벤트 병합 비트를 켠다 — 게스트가 선택할 수 있는 것이 아니라 호스트가 강제한다. */
}

/*
 * Create a physical STE from the virtual STE that userspace provided when it
 * created the nested domain. Using the vSTE userspace can request:
 * - Non-valid STE
 * - Abort STE
 * - Bypass STE (install the S2, no CD table)
 * - CD table STE (install the S2 and the userspace CD table)
 */
/*
 * [한국어]
 * arm_smmu_make_nested_domain_ste - 게스트가 지은 vSTE 를 실제 STE 로 옮긴다
 *
 * @target: 지어 담을 STE 버퍼.
 * @master: 이 STE 를 쓰게 될 장치.
 * @nested_domain: 게스트의 vSTE 를 담은 중첩 도메인.
 * @ats_enabled: 이번 붙이기에서 ATS 를 켜기로 했는가.
 *
 * (위 영어 주석 참고) 게스트가 vSTE 로 요청할 수 있는 것은 네 가지뿐이다 —
 * 무효, 중단, 우회(2단계만 적용), 그리고 자기 문맥 표를 쓰는 1단계 변환.
 * 그 밖의 값은 이미 arm_smmu_validate_vste() 에서 걸러졌으므로, 여기서는
 * 갈래를 나눠 알맞은 STE 를 짓기만 하면 된다.
 *
 * 무효 STE 를 중단 STE 로 바꾸는 처리가 흥미롭다. 게스트가 유효하지 않은
 * 항목을 만들어 두면, 그 스트림의 접근은 실패해야 하고 그 실패는 게스트가
 * 알아야 한다. 중단 STE 로 만들면 하드웨어가 C_BAD_STE 를 내고, 그것이
 * 게스트의 이벤트 큐로 전달되어 게스트가 자기 실수를 보게 된다.
 *
 * 실행 컨텍스트: 붙이기 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_attach_dev_nested() → [이 함수]
 *     → arm_smmu_make_nested_cd_table_ste() / arm_smmu_make_s2_domain_ste()
 *       / arm_smmu_make_abort_ste()
 */
static void arm_smmu_make_nested_domain_ste(
	struct arm_smmu_ste *target, struct arm_smmu_master *master,
	struct arm_smmu_nested_domain *nested_domain, bool ats_enabled)
{
	unsigned int cfg =	/* [한국어] 게스트가 요청한 설정 갈래를 꺼낸다. */
		FIELD_GET(STRTAB_STE_0_CFG, le64_to_cpu(nested_domain->ste[0]));	/* [한국어] vSTE 는 하드웨어 형식(리틀엔디안)으로 저장되어 있어 호스트 엔디안으로 바꿔 읽는다. */

	/*
	 * Userspace can request a non-valid STE through the nesting interface.
	 * We relay that into an abort physical STE with the intention that
	 * C_BAD_STE for this SID can be generated to userspace.
	 */
	/* [한국어] (위 영어 주석 참고) 게스트가 유효하지 않은 항목을 요청하면,
	 * 실제로는 중단 항목을 심는다. 그러면 하드웨어가 C_BAD_STE 오류를 내고,
	 * 그 오류가 게스트에게 올라가 게스트가 자기 설정 실수를 알게 된다. */
	if (!(nested_domain->ste[0] & cpu_to_le64(STRTAB_STE_0_V)))	/* [한국어] 유효 비트가 꺼져 있는가. */
		cfg = STRTAB_STE_0_CFG_ABORT;	/* [한국어] 갈래를 중단으로 바꿔 아래 switch 가 중단 STE 를 짓게 한다. */

	switch (cfg) {	/* [한국어] 게스트가 요청한 갈래에 따라 실제 STE 를 짓는다. */
	case STRTAB_STE_0_CFG_S1_TRANS:	/* [한국어] 게스트가 자기 문맥 서술자 표를 쓰겠다는 경우 — 진짜 중첩 변환이다. */
		arm_smmu_make_nested_cd_table_ste(target, master, nested_domain,	/* [한국어] 2단계 위에 게스트의 1단계를 얹는다. */
						  ats_enabled);
		break;
	case STRTAB_STE_0_CFG_BYPASS:	/* [한국어] 게스트가 1단계 변환 없이 지나가겠다는 경우. */
		arm_smmu_make_s2_domain_ste(target, master,	/* [한국어] 그래도 호스트의 2단계는 반드시 적용한다 — 그것이 게스트를 가두는 울타리다. */
					    nested_domain->vsmmu->s2_parent,
					    ats_enabled);
		break;
	case STRTAB_STE_0_CFG_ABORT:	/* [한국어] 게스트가 이 스트림을 막겠다는 경우. */
	default:	/* [한국어] 그 밖의 값 — 검증을 통과했다면 여기 올 수 없지만, 안전을 위해 같은 처리로 묶는다. */
		arm_smmu_make_abort_ste(target);	/* [한국어] 모든 접근을 거부하는 항목을 심는다. */
		break;
	}
}

/*
 * [한국어]
 * arm_smmu_attach_prepare_vmaster - 게스트에게 사건을 돌려줄 다리를 미리 만든다
 *
 * @state: 붙이기 상태 묶음. 성공하면 state->vmaster 가 채워진다.
 * @nested_domain: 붙일 중첩 도메인.
 * @return: 0 성공, 음수 오류.
 *
 * 하드웨어가 낸 이벤트에는 실제 스트림 번호가 실려 있는데, 게스트는 그
 * 번호를 모른다. 게스트가 아는 번호(vSID)로 바꿔 줄 대응표가 필요하고,
 * 그 한 칸이 struct arm_smmu_vmaster 다. 이 함수는 그 칸을 잡아 state 에
 * 담아 두기만 한다 — 잡기가 실패할 수 있으므로 prepare 단계의 일이다.
 *
 * 중단·우회 도메인은 예외다. 그런 도메인은 게스트의 1단계 변환을 쓰지
 * 않으므로 무효화나 폴트를 게스트에게 돌려줄 일이 없고, 따라서 vDEVICE 를
 * 먼저 만들지 않았어도 붙일 수 있게 허용한다 (게스트 부팅 초기의 GBPA 상태).
 *
 * 실행 컨텍스트: 붙이기 경로, group mutex 아래. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_attach_prepare() → [이 함수] → iommufd_viommu_get_vdev_id()
 */
int arm_smmu_attach_prepare_vmaster(struct arm_smmu_attach_state *state,
				    struct arm_smmu_nested_domain *nested_domain)
{
	unsigned int cfg =	/* [한국어] 게스트가 요청한 설정 갈래 — 아래에서 예외를 허용할지 판단하는 데 쓴다. */
		FIELD_GET(STRTAB_STE_0_CFG, le64_to_cpu(nested_domain->ste[0]));
	struct arm_smmu_vmaster *vmaster;	/* [한국어] 새로 잡을 다리. */
	unsigned long vsid;	/* [한국어] 게스트가 이 장치에 붙인 번호. */
	int ret;	/* [한국어] 조회 결과. */

	iommu_group_mutex_assert(state->master->dev);	/* [한국어] 이 자료 구조는 group mutex 로 지켜진다 — 잡고 들어왔는지 디버그 빌드에서 확인한다. */

	ret = iommufd_viommu_get_vdev_id(&nested_domain->vsmmu->core,	/* [한국어] iommufd 가 관리하는 vDEVICE 표에서 이 장치의 게스트 번호를 찾는다. */
					 state->master->dev, &vsid);
	/*
	 * Attaching to a translate nested domain must allocate a vDEVICE prior,
	 * as CD/ATS invalidations and vevents require a vSID to work properly.
	 * A abort/bypass domain is allowed to attach w/o vmaster for GBPA case.
	 */
	/* [한국어] (위 영어 주석 참고) 진짜 변환을 하는 중첩 도메인이라면 게스트
	 * 번호가 반드시 있어야 한다 — 무효화 명령의 번호를 바꿔 끼우고 이벤트를
	 * 돌려주는 데 그 번호가 필요하기 때문이다. 반면 중단·우회 도메인은 그런
	 * 일이 없으므로, 게스트가 아직 장치를 등록하기 전이어도 붙일 수 있다. */
	if (ret) {	/* [한국어] 게스트 번호를 못 찾은 경우. */
		if (cfg == STRTAB_STE_0_CFG_ABORT ||	/* [한국어] 중단 도메인이거나. */
		    cfg == STRTAB_STE_0_CFG_BYPASS)	/* [한국어] 우회 도메인이라면. */
			return 0;	/* [한국어] 다리 없이 붙이기를 허용한다. */
		return ret;	/* [한국어] 그 밖에는 실패로 처리한다 — 번호 없이 붙이면 나중에 무효화가 엉뚱한 곳으로 간다. */
	}

	vmaster = kzalloc_obj(*vmaster);	/* [한국어] 다리 한 칸을 잡는다. */
	if (!vmaster)	/* [한국어] 메모리가 없으면. */
		return -ENOMEM;	/* [한국어] 붙이기를 접는다 — 아직 하드웨어를 건드리기 전이라 되돌릴 것이 없다. */
	vmaster->vsmmu = nested_domain->vsmmu;	/* [한국어] 어느 가상 SMMU 에 속한 다리인지 기록한다 — 이벤트를 올릴 목적지가 된다. */
	vmaster->vsid = vsid;	/* [한국어] 게스트가 아는 장치 번호. 이벤트를 올릴 때 실제 번호 대신 이 값을 적는다. */
	state->vmaster = vmaster;	/* [한국어] commit 단계가 실제로 걸 수 있도록 상태 묶음에 담아 둔다. */

	return 0;	/* [한국어] 준비 완료. 이 뒤로 실패할 일은 없다. */
}

/*
 * [한국어]
 * arm_smmu_attach_commit_vmaster - 준비해 둔 다리를 장치에 실제로 건다
 *
 * @state: prepare 가 채워 둔 상태 묶음.
 *
 * 장치에 걸려 있던 옛 다리를 놓고 새 다리를 건다. 이벤트를 게스트에게
 * 올리는 인터럽트 경로가 같은 자리를 읽으므로, 바꾸는 동안 streams_mutex 를
 * 잡는다 — 그 경로도 같은 락 아래에서 이 값을 읽는다.
 *
 * 잡을 것은 이미 다 잡아 두었으므로 이 함수는 실패하지 않는다.
 *
 * 실행 컨텍스트: 붙이기 경로. mutex 를 잡으므로 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_attach_commit() → [이 함수]
 */
void arm_smmu_attach_commit_vmaster(struct arm_smmu_attach_state *state)
{
	struct arm_smmu_master *master = state->master;	/* [한국어] 다리를 걸 대상 장치. */

	mutex_lock(&master->smmu->streams_mutex);	/* [한국어] 이벤트 처리 스레드가 master->vmaster 를 읽는 동안 바뀌면 안 된다. */
	kfree(master->vmaster);	/* [한국어] 옛 다리를 놓는다. 락 아래이므로 지금 이것을 읽고 있는 쪽은 없다. */
	master->vmaster = state->vmaster;	/* [한국어] 새 다리를 건다. NULL 일 수도 있다 — 그러면 이벤트를 게스트에게 올리지 않는다. */
	mutex_unlock(&master->smmu->streams_mutex);	/* [한국어] 이제 이벤트 처리 스레드가 새 값을 보게 된다. */
}

/*
 * [한국어]
 * arm_smmu_master_clear_vmaster - 그 장치의 게스트 다리를 끊는다
 *
 * @master: 대상 장치.
 *
 * 장치를 떼거나 중첩이 아닌 도메인으로 옮길 때 부른다. 빈 상태 묶음을
 * 만들어 commit 을 그대로 재사용하는 것이 요령이다 — state.vmaster 가
 * NULL 이므로 옛 다리를 놓고 NULL 을 거는 동작이 된다.
 *
 * 실행 컨텍스트: 떼기 경로. mutex 를 잡으므로 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_release_device()/붙이기 경로 → [이 함수]
 *     → arm_smmu_attach_commit_vmaster()
 */
void arm_smmu_master_clear_vmaster(struct arm_smmu_master *master)
{
	struct arm_smmu_attach_state state = { .master = master };	/* [한국어] vmaster 가 NULL 인 빈 상태 — 나머지 필드도 0 으로 채워진다. */

	arm_smmu_attach_commit_vmaster(&state);	/* [한국어] 같은 경로를 재사용해 옛 다리를 놓고 NULL 을 건다. */
}

/*
 * [한국어]
 * arm_smmu_attach_dev_nested - 장치를 게스트의 중첩 도메인에 붙인다
 *
 * @domain: 붙일 중첩 도메인.
 * @dev: 붙일 장치.
 * @old_domain: 그 장치가 쓰던 이전 도메인.
 * @return: 0 성공, 음수 오류.
 *
 * 중첩 도메인의 attach_dev 갈고리다. 검사 → 준비 → 하드웨어 쓰기 → 마무리의
 * 네 걸음을 밟는다. 준비와 마무리 사이에서만 하드웨어를 건드리므로, 그
 * 앞에서 실패하면 아무것도 바뀌지 않는다.
 *
 * ATS 처리가 이 함수에서 가장 미묘한 부분이다. 게스트가 무효화를 직접 내므로,
 * 호스트가 게스트 몰래 ATS 를 켜 두면 게스트는 장치 캐시를 비울 생각을 하지
 * 않아 캐시가 낡은 채 남는다. 그래서 게스트의 STE 에 적힌 EATS 값을 그대로
 * 따라간다 — 게스트가 켰다고 적었으면 켜고, 아니면 끈다.
 *
 * 실행 컨텍스트: iommufd/iommu 코어의 붙이기 경로. 잠들 수 있다.
 *
 * 호출 체인:
 *   iommu_attach_device() → [이 함수]
 *     → arm_smmu_attach_prepare() → arm_smmu_install_ste_for_dev()
 *     → arm_smmu_attach_commit()
 */
static int arm_smmu_attach_dev_nested(struct iommu_domain *domain,
				      struct device *dev,
				      struct iommu_domain *old_domain)
{
	struct arm_smmu_nested_domain *nested_domain =	/* [한국어] 코어 도메인에서 중첩 도메인으로 되짚는다. */
		to_smmu_nested_domain(domain);
	struct arm_smmu_master *master = dev_iommu_priv_get(dev);	/* [한국어] 장치에 붙여 둔 SMMU 쪽 상태. */
	struct arm_smmu_attach_state state = {	/* [한국어] 두 단계가 주고받을 상태 묶음을 채운다. */
		.master = master,	/* [한국어] 대상 장치. */
		.old_domain = old_domain,	/* [한국어] 무효화 배열을 옛 도메인에서 걷어 내는 데 필요하다. */
		.ssid = IOMMU_NO_PASID,	/* [한국어] 중첩은 장치 전체에 걸리므로 PASID 없이 붙인다 — 게스트의 PASID 는 게스트의 문맥 표가 관리한다. */
	};
	struct arm_smmu_ste ste;	/* [한국어] 지어서 심을 스트림 표 항목. */
	int ret;	/* [한국어] 준비 단계의 결과. */

	if (nested_domain->vsmmu->smmu != master->smmu)	/* [한국어] 이 장치가 그 가상 SMMU 를 떠받치는 하드웨어에 매달려 있는가. */
		return -EINVAL;	/* [한국어] 다른 SMMU 의 장치를 남의 가상 SMMU 에 붙일 수는 없다. */
	if (arm_smmu_ssids_in_use(&master->cd_table))	/* [한국어] 호스트가 이 장치의 PASID 를 쓰고 있는가 (SVA 등). */
		return -EBUSY;	/* [한국어] 그렇다면 문맥 표의 주인이 둘이 되므로 붙일 수 없다 — 중첩에서는 게스트가 표의 주인이다. */

	mutex_lock(&arm_smmu_asid_lock);	/* [한국어] ASID 배정과 문맥 표 상태를 지키는 전역 락. 붙이기 전 구간을 통째로 감싼다. */
	/*
	 * The VM has to control the actual ATS state at the PCI device because
	 * we forward the invalidations directly from the VM. If the VM doesn't
	 * think ATS is on it will not generate ATC flushes and the ATC will
	 * become incoherent. Since we can't access the actual virtual PCI ATS
	 * config bit here base this off the EATS value in the STE. If the EATS
	 * is set then the VM must generate ATC flushes.
	 */
	/* [한국어] (위 영어 주석 참고) 무효화를 게스트가 직접 내므로, 장치 캐시를
	 * 비울 책임도 게스트에게 있다. 게스트가 ATS 가 꺼져 있다고 믿으면 캐시를
	 * 비우지 않을 것이므로, 호스트가 몰래 켜 두면 캐시가 낡은 채 남는다.
	 * 게스트의 PCI 설정 비트를 여기서 볼 수는 없으니, STE 의 EATS 값을
	 * 게스트의 의사로 삼는다. */
	if (FIELD_GET(STRTAB_STE_0_CFG, le64_to_cpu(nested_domain->ste[0])) ==	/* [한국어] 게스트가 진짜 1단계 변환을 요청한 경우에만 이 판단이 뜻을 가진다. */
	    STRTAB_STE_0_CFG_S1_TRANS)
		state.disable_ats = !nested_domain->enable_ats;	/* [한국어] 게스트가 EATS 를 켜지 않았다면 호스트도 끈다 — 게스트의 뜻을 그대로 따른다. */
	ret = arm_smmu_attach_prepare(&state, domain);	/* [한국어] 실패할 수 있는 일을 모두 여기서 끝낸다 — 무효화 배열, 게스트 다리, ATS 판단. */
	if (ret) {	/* [한국어] 준비가 실패하면. */
		mutex_unlock(&arm_smmu_asid_lock);	/* [한국어] 락을 놓고. */
		return ret;	/* [한국어] 하드웨어는 아직 옛 설정 그대로다 — 되돌릴 것이 없다. */
	}

	arm_smmu_make_nested_domain_ste(&ste, master, nested_domain,	/* [한국어] 게스트 vSTE 와 호스트 2단계를 합쳐 실제 항목을 짓는다. */
					state.ats_enabled);	/* [한국어] 준비 단계가 최종 결정한 ATS 상태를 반영한다. */
	arm_smmu_install_ste_for_dev(master, &ste);	/* [한국어] 이 장치의 모든 스트림 번호에 그 항목을 심는다. 이 순간부터 하드웨어가 새 설정으로 동작한다. */
	arm_smmu_attach_commit(&state);	/* [한국어] 무효화 배열 교체, ATS 켜기, 게스트 다리 걸기 — 실패하지 않는 뒷정리. */
	mutex_unlock(&arm_smmu_asid_lock);	/* [한국어] 붙이기가 끝났다. */
	return 0;	/* [한국어] 여기까지 왔으면 성공이다. */
}

/*
 * [한국어]
 * arm_smmu_domain_nested_free - 중첩 도메인을 놓는다
 *
 * @domain: 놓을 도메인.
 *
 * 중첩 도메인은 페이지 테이블도, ASID 도, 무효화 배열도 갖지 않는다 —
 * 게스트가 지은 STE 두 워드를 담고 있을 뿐이다. 그래서 해제도 단순히
 * 구조체를 놓는 것으로 끝난다. 실제 변환 자원은 부모 2단계 도메인과
 * 게스트의 문맥 표가 각각 관리한다.
 *
 * 실행 컨텍스트: iommufd 객체 해제. 잠들 수 있다.
 *
 * 호출 체인:
 *   iommu_domain_free() → [이 함수] → kfree()
 */
static void arm_smmu_domain_nested_free(struct iommu_domain *domain)
{
	kfree(to_smmu_nested_domain(domain));	/* [한국어] 감싸는 구조체를 되짚어 통째로 놓는다. */
}

/* [한국어] 중첩 도메인의 연산표.
 *
 * 둘뿐이다 — 붙이기와 놓기. map/unmap 이 없는 이유는 이 도메인이 페이지
 * 테이블을 갖지 않기 때문이다. 게스트 물리 → 실제 물리 매핑은 부모 2단계
 * 도메인이 쥐고 있고, 게스트 가상 → 게스트 물리는 게스트가 자기 표에
 * 직접 쓴다. 호스트가 매핑을 걸 자리가 아예 없다. */
static const struct iommu_domain_ops arm_smmu_nested_ops = {
	.attach_dev = arm_smmu_attach_dev_nested,	/* [한국어] 장치를 이 중첩 설정에 붙이는 갈고리. */
	.free = arm_smmu_domain_nested_free,	/* [한국어] 도메인을 놓는 갈고리. */
};

/*
 * [한국어]
 * arm_smmu_validate_vste - 게스트가 지은 STE 가 안전한지 검사한다
 *
 * @arg: 사용자 공간에서 복사해 온 STE 두 워드 (제자리에서 고쳐진다).
 * @enable_ats: 게스트가 ATS 를 켜기를 원하는지 적어 돌려줄 자리.
 * @return: 0 통과, -EIO 거부.
 *
 * 중첩 변환의 보안이 이 함수 하나에 걸려 있다. 게스트가 STE 에 적을 수 있는
 * 것은 자기 1단계 변환에 관한 비트뿐이고, 2단계 표 주소·VMID·메모리 속성
 * 같은 호스트 몫을 건드리면 게스트가 울타리를 넘을 수 있다. 그 경계가
 * STRTAB_STE_0/1_NESTING_ALLOWED 마스크이며, 여기서 마스크 밖 비트가 하나라도
 * 켜져 있으면 통째로 거부한다.
 *
 * 거부에 -EIO 를 쓰는 것은 약속이다 — 다른 오류 코드는 "요청 자체가 잘못됐다"는
 * 뜻으로 쓰이고, -EIO 는 "STE 데이터가 잘못됐다"는 뜻으로 예약해 두었다.
 *
 * 실행 컨텍스트: iommufd ioctl. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_vsmmu_alloc_domain_nested() → [이 함수]
 */
static int arm_smmu_validate_vste(struct iommu_hwpt_arm_smmuv3 *arg,
				  bool *enable_ats)
{
	unsigned int eats;	/* [한국어] 게스트가 적은 ATS 설정 값. */
	unsigned int cfg;	/* [한국어] 게스트가 적은 설정 갈래. */

	if (!(arg->ste[0] & cpu_to_le64(STRTAB_STE_0_V))) {	/* [한국어] 유효 비트가 꺼진 STE 인가. */
		memset(arg->ste, 0, sizeof(arg->ste));	/* [한국어] 나머지 비트는 볼 필요가 없으니 통째로 지운다 — 쓰레기 값을 들고 다니지 않기 위해서다. */
		return 0;	/* [한국어] 무효 STE 는 허용한다. 나중에 중단 STE 로 옮겨진다. */
	}

	/* EIO is reserved for invalid STE data. */
	/* [한국어] (위 영어 주석 참고) 이 아래의 거부는 모두 -EIO 다 — 사용자 공간이
	 * "내가 적은 STE 값이 문제"라고 정확히 알 수 있게 하는 약속이다. */
	if ((arg->ste[0] & ~STRTAB_STE_0_NESTING_ALLOWED) ||	/* [한국어] 첫 워드에 게스트가 만져서는 안 될 비트가 켜져 있는가. */
	    (arg->ste[1] & ~STRTAB_STE_1_NESTING_ALLOWED))	/* [한국어] 둘째 워드도 마찬가지로 검사한다. */
		return -EIO;	/* [한국어] 하나라도 넘어섰으면 통째로 거부한다 — 몰래 지워 주는 것보다 거부가 안전하다. */

	cfg = FIELD_GET(STRTAB_STE_0_CFG, le64_to_cpu(arg->ste[0]));	/* [한국어] 게스트가 요청한 설정 갈래를 꺼낸다. */
	if (cfg != STRTAB_STE_0_CFG_ABORT && cfg != STRTAB_STE_0_CFG_BYPASS &&	/* [한국어] 중단도, 우회도. */
	    cfg != STRTAB_STE_0_CFG_S1_TRANS)	/* [한국어] 1단계 변환도 아니라면. */
		return -EIO;	/* [한국어] 게스트가 쓸 수 있는 갈래는 이 셋뿐이다 — 2단계나 중첩을 게스트가 직접 요청할 수는 없다. */

	/*
	 * Only Full ATS or ATS UR is supported
	 * The EATS field will be set by arm_smmu_make_nested_domain_ste()
	 */
	/* [한국어] (위 영어 주석 참고) EATS 자리는 게스트의 "의사 표시"로만 읽고,
	 * 실제 STE 에 들어갈 값은 호스트가 붙이기 때 다시 다시 정한다. 그래서 여기서는
	 * 값을 꺼내 둔 뒤 원본에서는 지워 버린다. */
	eats = FIELD_GET(STRTAB_STE_1_EATS, le64_to_cpu(arg->ste[1]));	/* [한국어] 게스트가 적은 ATS 설정을 꺼낸다. */
	arg->ste[1] &= ~cpu_to_le64(STRTAB_STE_1_EATS);	/* [한국어] 원본에서는 지운다 — 그대로 두면 게스트 값이 실제 STE 에 그대로 실린다. */
	if (eats != STRTAB_STE_1_EATS_ABT && eats != STRTAB_STE_1_EATS_TRANS)	/* [한국어] 지원하는 두 가지 — 끄기(중단)와 완전 켜기 — 가 아니라면. */
		return -EIO;	/* [한국어] 중간 형태(ATS UR 등 그 밖의 값)는 다루지 않는다. */

	if (cfg == STRTAB_STE_0_CFG_S1_TRANS)	/* [한국어] 진짜 변환을 요청한 경우에만. */
		*enable_ats = (eats == STRTAB_STE_1_EATS_TRANS);	/* [한국어] 게스트가 ATS 를 켜기를 원했는지 호출자에게 알린다. */
	return 0;	/* [한국어] 모든 검사를 통과했다. */
}

/*
 * [한국어]
 * arm_vsmmu_alloc_domain_nested - 게스트의 1단계 설정을 도메인으로 감싼다
 *
 * @viommu: 이 도메인이 속할 가상 SMMU.
 * @flags: 사용자가 준 플래그 (지금은 어떤 값도 지원하지 않는다).
 * @user_data: 게스트가 지은 STE 원본이 담긴 사용자 데이터.
 * @return: 만들어진 중첩 도메인, 실패하면 ERR_PTR.
 *
 * 사용자 공간에서 STE 두 워드를 복사해 오고, 검사를 통과하면 그 값을 담은
 * 작은 도메인을 만든다. 이 도메인이 하는 일은 "게스트가 이렇게 지었다"는
 * 사실을 기억하는 것뿐이며, 실제 하드웨어에 심는 일은 장치를 붙일 때
 * arm_smmu_attach_dev_nested() 가 한다.
 *
 * GFP_KERNEL_ACCOUNT 로 잡는 것이 눈에 띈다 — 사용자 공간이 요청할 때마다
 * 커널 메모리가 늘어나므로, 그 메모리를 요청한 cgroup 의 몫으로 달아
 * 한 컨테이너가 커널 메모리를 무한히 먹지 못하게 막는다.
 *
 * 실행 컨텍스트: iommufd ioctl. 잠들 수 있다.
 *
 * 호출 체인:
 *   IOMMU_HWPT_ALLOC ioctl → iommufd → [이 함수]
 *     → iommu_copy_struct_from_user() → arm_smmu_validate_vste()
 */
struct iommu_domain *
arm_vsmmu_alloc_domain_nested(struct iommufd_viommu *viommu, u32 flags,
			      const struct iommu_user_data *user_data)
{
	struct arm_vsmmu *vsmmu = container_of(viommu, struct arm_vsmmu, core);	/* [한국어] iommufd 객체에서 이 드라이버의 가상 SMMU 로 되짚는다. */
	struct arm_smmu_nested_domain *nested_domain;	/* [한국어] 만들어 낼 중첩 도메인. */
	struct iommu_hwpt_arm_smmuv3 arg;	/* [한국어] 사용자 공간에서 복사해 올 STE 원본. 스택에 둔다 — 16바이트뿐이다. */
	bool enable_ats = false;	/* [한국어] 게스트가 ATS 를 원하는지. 검사 함수가 채워 준다. */
	int ret;	/* [한국어] 중간 단계의 결과. */

	if (flags)	/* [한국어] 아직 정의된 플래그가 없다. */
		return ERR_PTR(-EOPNOTSUPP);	/* [한국어] 모르는 플래그를 조용히 무시하면 나중에 뜻이 생겼을 때 호환이 깨진다 — 그래서 거부한다. */

	ret = iommu_copy_struct_from_user(&arg, user_data,	/* [한국어] 사용자 공간의 구조체를 안전하게 복사해 온다. */
					  IOMMU_HWPT_DATA_ARM_SMMUV3, ste);	/* [한국어] 형식 번호와 마지막 필드 이름을 넘겨, 버전이 다른 구조체도 안전하게 다룬다. */
	if (ret)	/* [한국어] 복사 실패 (잘못된 주소, 크기 불일치 등). */
		return ERR_PTR(ret);	/* [한국어] 그대로 위로 넘긴다. */

	ret = arm_smmu_validate_vste(&arg, &enable_ats);	/* [한국어] 게스트가 만져서는 안 될 비트를 건드렸는지 검사한다. */
	if (ret)	/* [한국어] 거부된 경우. */
		return ERR_PTR(ret);	/* [한국어] -EIO 를 그대로 사용자에게 돌려준다. */

	nested_domain = kzalloc_obj(*nested_domain, GFP_KERNEL_ACCOUNT);	/* [한국어] 사용자 요청으로 늘어나는 메모리라 cgroup 몫으로 달아 잡는다. */
	if (!nested_domain)	/* [한국어] 메모리가 없으면. */
		return ERR_PTR(-ENOMEM);	/* [한국어] 오류 포인터로 알린다. */

	nested_domain->domain.type = IOMMU_DOMAIN_NESTED;	/* [한국어] 코어에게 이 도메인이 중첩 종류임을 알린다. */
	nested_domain->domain.ops = &arm_smmu_nested_ops;	/* [한국어] 붙이기·놓기 두 갈고리만 있는 연산표를 건다. */
	nested_domain->enable_ats = enable_ats;	/* [한국어] 게스트의 ATS 의사를 기억해 둔다 — 붙일 때 이 값을 따른다. */
	nested_domain->vsmmu = vsmmu;	/* [한국어] 어느 가상 SMMU 에 속하는지 — 부모 2단계 도메인과 VMID 가 여기로 이어진다. */
	nested_domain->ste[0] = arg.ste[0];	/* [한국어] 검사를 통과한 첫 워드를 그대로 기억한다. */
	nested_domain->ste[1] = arg.ste[1] & ~cpu_to_le64(STRTAB_STE_1_EATS);	/* [한국어] 둘째 워드에서 EATS 는 한 번 더 지워 둔다 — 실제 값은 붙일 때 호스트가 정한다. */

	return &nested_domain->domain;	/* [한국어] 코어가 아는 형태로 돌려준다. */
}

/*
 * [한국어]
 * arm_vsmmu_vsid_to_sid - 게스트가 아는 장치 번호를 실제 스트림 번호로 바꾼다
 *
 * @vsmmu: 그 가상 SMMU.
 * @vsid: 게스트가 명령에 적은 장치 번호.
 * @sid: 실제 스트림 번호를 적어 줄 자리 (NULL 이면 존재 확인만 한다).
 * @return: 0 성공, -EIO 그런 장치가 없음.
 *
 * 게스트는 자기가 붙인 번호로 장치를 가리키므로, 그 번호를 하드웨어가 아는
 * 번호로 바꿔 줘야 명령이 옳은 곳에 간다. 이 치환이 없으면 게스트가 아무
 * 번호나 적어 다른 게스트나 호스트의 장치 캐시를 지울 수 있다.
 *
 * xa_lock 을 잡는 이유는, 조회한 장치가 조회 직후 사라지지 않도록 하기
 * 위해서다. 락 안에서 스트림 번호까지 복사해 나오면 그 값은 계속 유효하다.
 *
 * 실행 컨텍스트: 무효화 ioctl 처리 중. 스핀락을 잡으므로 그 안에서는 잠들 수 없다.
 *
 * 호출 체인:
 *   arm_vsmmu_convert_user_cmd() → [이 함수] → iommufd_viommu_find_dev()
 */
static int arm_vsmmu_vsid_to_sid(struct arm_vsmmu *vsmmu, u32 vsid, u32 *sid)
{
	struct arm_smmu_master *master;	/* [한국어] 찾아낸 장치의 SMMU 쪽 상태. */
	struct device *dev;	/* [한국어] 그 번호에 해당하는 장치. */
	int ret = 0;	/* [한국어] 기본은 성공 — 못 찾았을 때만 바뀐다. */

	xa_lock(&vsmmu->core.vdevs);	/* [한국어] 게스트 장치 표를 잠근다 — 조회 도중 장치가 빠지면 안 된다. */
	dev = iommufd_viommu_find_dev(&vsmmu->core, (unsigned long)vsid);	/* [한국어] 게스트 번호로 실제 장치를 찾는다. */
	if (!dev) {	/* [한국어] 게스트가 등록하지 않은 번호를 적었다면. */
		ret = -EIO;	/* [한국어] 명령 데이터가 잘못됐다는 뜻으로 -EIO 를 쓴다. */
		goto unlock;	/* [한국어] 락을 풀고 나간다. */
	}
	master = dev_iommu_priv_get(dev);	/* [한국어] 그 장치의 스트림 번호 목록을 꺼내기 위해. */

	/* At this moment, iommufd only supports PCI device that has one SID */
	/* [한국어] (위 영어 주석 참고) 지금은 스트림 번호가 하나뿐인 PCI 장치만
	 * 게스트에 넘길 수 있다. 여러 개인 장치는 게스트가 어느 번호로 명령을
	 * 낼지 정할 방법이 아직 없기 때문이다. */
	if (sid)	/* [한국어] 호출자가 번호를 원한 경우에만 (존재 확인만 하는 호출도 있다). */
		*sid = master->streams[0].id;	/* [한국어] 첫 번째이자 유일한 스트림 번호를 준다. */
unlock:	/* [한국어] 성공·실패 모두 이 자리를 지나 락을 푼다. */
	xa_unlock(&vsmmu->core.vdevs);	/* [한국어] 표를 푼다. */
	return ret;	/* [한국어] 찾았으면 0, 아니면 -EIO. */
}

/* This is basically iommu_viommu_arm_smmuv3_invalidate in u64 for conversion */
/* [한국어] 게스트가 낸 무효화 명령 하나를, 사용자 형식과 커널 내부 형식
 * 양쪽으로 볼 수 있게 겹쳐 둔 자리.
 *
 * (위 영어 주석 참고) 같은 16바이트를 두 이름으로 보는 union 이다. 사용자
 * 공간에서 온 값은 리틀엔디안 le64 이고, 명령 큐에 넣을 때는 호스트 엔디안
 * u64 여야 한다. 두 표현이 같은 자리를 쓰므로 버퍼를 새로 잡지 않고
 * 제자리에서 변환할 수 있다 — 명령이 수천 개씩 올 수 있어 복사를 아끼는 것이
 * 뜻이 있다. */
struct arm_vsmmu_invalidation_cmd {
	/* [한국어] 두 표현을 겹쳐 담는 union.
	 * 설정자: 사용자 복사는 ucmd 쪽으로 들어오고, 변환은 cmd 쪽으로 쓴다.
	 * 읽는 자: arm_smmu_cmdq_issue_cmdlist() 는 cmd 쪽만 본다.
	 * 값 범위: 변환 전에는 le64, 변환 후에는 호스트 엔디안이다 —
	 *         같은 자리를 두 뜻으로 읽으므로 변환 여부를 코드 흐름으로만
	 *         구분한다.
	 * 동기화: ioctl 하나가 자기 배열만 다루므로 공유되지 않는다. */
	union {
		/* [한국어] 명령 큐에 그대로 실을 두 워드 (호스트 엔디안).
		 * 설정자: arm_vsmmu_convert_user_cmd() 가 제자리에서 채운다.
		 * 읽는 자: arm_smmu_cmdq_issue_cmdlist().
		 * 값 범위: 검사와 치환을 마친 안전한 명령이어야 한다.
		 * 동기화: 없음. */
		u64 cmd[2];
		/* [한국어] 사용자 공간에서 온 원본 명령 (리틀엔디안).
		 * 설정자: iommu_copy_struct_from_full_user_array() 가 통째로 복사한다.
		 * 읽는 자: 변환 함수가 첫 줄에서 읽어 cmd 로 옮긴다.
		 * 값 범위: 게스트가 적은 값 그대로 — 아직 아무것도 믿을 수 없다.
		 * 동기화: 없음. */
		struct iommu_viommu_arm_smmuv3_invalidate ucmd;
	};
};

/*
 * Convert, in place, the raw invalidation command into an internal format that
 * can be passed to arm_smmu_cmdq_issue_cmdlist(). Internally commands are
 * stored in CPU endian.
 *
 * Enforce the VMID or SID on the command.
 */
/*
 * [한국어]
 * arm_vsmmu_convert_user_cmd - 게스트 명령을 안전한 커널 명령으로 바꾼다
 *
 * @vsmmu: 그 가상 SMMU (강제로 써 넣을 VMID 를 여기서 가져온다).
 * @cmd: 사용자 원본이 담긴 자리. 제자리에서 고쳐진다.
 * @return: 0 통과, -EIO 거부.
 *
 * (위 영어 주석 참고) 중첩 변환의 두 번째 보안 관문이다. 게스트가 낸 무효화
 * 명령은 두 가지 이유로 그대로 쓸 수 없다. 첫째, 엔디안이 다르다. 둘째,
 * 그리고 훨씬 중요하게, 게스트가 VMID 나 스트림 번호를 남의 것으로 적을 수
 * 있다. 그러면 게스트 하나가 다른 게스트나 호스트의 TLB 를 마음대로 지워
 * 성능을 떨어뜨리거나, 최악에는 낡은 변환을 남겨 두게 만들 수 있다.
 *
 * 그래서 이 함수는 허용된 명령 종류만 통과시키고, 그 명령의 VMID 자리는
 * 이 게스트의 VMID 로, 스트림 번호 자리는 실제 번호로 반드시 덮어쓴다.
 * 게스트가 무엇을 적었든 결과는 자기 몫만 지우는 명령이 된다.
 *
 * TLBI_NSNH_ALL 을 NH_ALL 로 바꾸는 처리가 좋은 예다. 게스트는 "모든 TLB 를
 * 비워라"라고 말하지만, 그대로 실행하면 호스트와 다른 게스트의 항목까지
 * 사라진다. 그래서 "이 VMID 의 항목만 비워라"로 바꿔 뜻을 좁힌다.
 *
 * 실행 컨텍스트: 무효화 ioctl. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_vsmmu_cache_invalidate() → [이 함수] → arm_vsmmu_vsid_to_sid()
 */
static int arm_vsmmu_convert_user_cmd(struct arm_vsmmu *vsmmu,
				      struct arm_vsmmu_invalidation_cmd *cmd)
{
	/* Commands are le64 stored in u64 */
	/* [한국어] (위 영어 주석 참고) 사용자에게서 온 값은 하드웨어 형식(리틀엔디안)
	 * 이므로, 비트를 꺼내 고치려면 먼저 호스트 엔디안으로 바꿔야 한다. */
	cmd->cmd[0] = le64_to_cpu(cmd->ucmd.cmd[0]);	/* [한국어] 첫 워드를 제자리에서 변환한다 — union 이라 같은 자리를 읽고 쓴다. */
	cmd->cmd[1] = le64_to_cpu(cmd->ucmd.cmd[1]);	/* [한국어] 둘째 워드도 마찬가지. */

	switch (cmd->cmd[0] & CMDQ_0_OP) {	/* [한국어] 명령 종류 필드만 남겨 갈래를 나눈다. */
	case CMDQ_OP_TLBI_NSNH_ALL:	/* [한국어] "비보안·비하이퍼바이저 TLB 전부"를 비우라는 명령. */
		/* Convert to NH_ALL */
		/* [한국어] (위 영어 주석 참고) 그대로 두면 다른 게스트와 호스트의 항목까지
		 * 지워진다. 이 게스트의 VMID 로 한정된 명령으로 바꿔 뜻을 좁힌다. */
		cmd->cmd[0] = CMDQ_OP_TLBI_NH_ALL |	/* [한국어] 명령을 통째로 다시 짓는다 — 게스트가 적은 나머지 필드는 버린다. */
			      FIELD_PREP(CMDQ_TLBI_0_VMID, vsmmu->vmid);	/* [한국어] 호스트가 이 게스트에게 배정한 VMID 를 강제로 적는다. */
		cmd->cmd[1] = 0;	/* [한국어] 이 명령은 둘째 워드를 쓰지 않는다 — 게스트가 넣어 둔 값이 남지 않게 지운다. */
		break;
	case CMDQ_OP_TLBI_NH_VA:	/* [한국어] 주소와 ASID 로 비우기. */
	case CMDQ_OP_TLBI_NH_VAA:	/* [한국어] 주소로만 비우기 (모든 ASID). */
	case CMDQ_OP_TLBI_NH_ALL:	/* [한국어] 그 VMID 전체 비우기. */
	case CMDQ_OP_TLBI_NH_ASID:	/* [한국어] 그 ASID 비우기. */
		cmd->cmd[0] &= ~CMDQ_TLBI_0_VMID;	/* [한국어] 게스트가 적은 VMID 를 지운다. */
		cmd->cmd[0] |= FIELD_PREP(CMDQ_TLBI_0_VMID, vsmmu->vmid);	/* [한국어] 호스트가 정한 VMID 로 갈아 끼운다 — 게스트는 자기 몫만 지울 수 있다. ASID 는 게스트가 정해도 안전하다, VMID 안에서만 뜻을 갖기 때문이다. */
		break;
	case CMDQ_OP_ATC_INV:	/* [한국어] 장치 쪽 변환 캐시 비우기. */
	case CMDQ_OP_CFGI_CD:	/* [한국어] 문맥 서술자 하나 무효화. */
	case CMDQ_OP_CFGI_CD_ALL: {	/* [한국어] 그 스트림의 문맥 서술자 전부 무효화. 세 명령 모두 스트림 번호를 인자로 쓴다. */
		u32 sid, vsid = FIELD_GET(CMDQ_CFGI_0_SID, cmd->cmd[0]);	/* [한국어] 게스트가 적은 장치 번호를 꺼낸다. */

		if (arm_vsmmu_vsid_to_sid(vsmmu, vsid, &sid))	/* [한국어] 그 번호가 이 게스트에게 실제로 붙은 장치인지 확인하며 실제 번호를 얻는다. */
			return -EIO;	/* [한국어] 남의 장치를 가리켰거나 없는 번호라면 명령을 버린다. */
		cmd->cmd[0] &= ~CMDQ_CFGI_0_SID;	/* [한국어] 게스트가 적은 번호를 지우고. */
		cmd->cmd[0] |= FIELD_PREP(CMDQ_CFGI_0_SID, sid);	/* [한국어] 실제 하드웨어 번호로 갈아 끼운다. */
		break;
	}
	default:	/* [한국어] 그 밖의 명령 — STE 무효화, 명령 동기화, 페이지 응답 등. */
		return -EIO;	/* [한국어] 게스트에게 허용하지 않는다. 특히 CFGI_STE 를 허용하면 게스트가 남의 스트림 설정을 흔들 수 있다. */
	}
	return 0;	/* [한국어] 안전한 명령으로 바뀌었다. */
}

/*
 * [한국어]
 * arm_vsmmu_cache_invalidate - 게스트가 낸 무효화 명령들을 대신 실행한다
 *
 * @viommu: 그 가상 SMMU.
 * @array: 사용자 공간의 명령 배열. 처리한 개수를 여기에 적어 돌려준다.
 * @return: 0 전부 성공, 음수면 그 지점에서 멈췄다.
 *
 * 게스트가 자기 명령 큐에 넣듯 지은 명령들을 통째로 받아, 하나씩 검사·치환한
 * 뒤 실제 명령 큐에 밀어 넣는다. 명령을 한 개씩 큐에 넣으면 MMIO 쓰기가
 * 명령 수만큼 생기므로, CMDQ_BATCH_ENTRIES 만큼 모아 한 번에 넣는다.
 *
 * 몇 개를 처리했는지 돌려주는 것이 중요하다. 중간에 잘못된 명령이 나오면
 * 거기서 멈추는데, 사용자 공간은 그 개수를 보고 어느 명령이 문제였는지
 * 알아내 고칠 수 있다.
 *
 * 실행 컨텍스트: iommufd ioctl. 잠들 수 있다 (명령 큐 삽입 자체는 잠들지 않는다).
 *
 * 호출 체인:
 *   IOMMU_VIOMMU_INVALIDATE ioctl → iommufd → [이 함수]
 *     → arm_vsmmu_convert_user_cmd() → arm_smmu_cmdq_issue_cmdlist()
 */
int arm_vsmmu_cache_invalidate(struct iommufd_viommu *viommu,
			       struct iommu_user_data_array *array)
{
	struct arm_vsmmu *vsmmu = container_of(viommu, struct arm_vsmmu, core);	/* [한국어] iommufd 객체에서 이 드라이버의 가상 SMMU 로 되짚는다. */
	struct arm_smmu_device *smmu = vsmmu->smmu;	/* [한국어] 명령을 실제로 넣을 하드웨어. */
	struct arm_vsmmu_invalidation_cmd *last;	/* [한국어] 아직 큐에 넣지 않은 묶음의 시작. */
	struct arm_vsmmu_invalidation_cmd *cmds;	/* [한국어] 명령 배열 전체의 시작. */
	struct arm_vsmmu_invalidation_cmd *cur;	/* [한국어] 지금 변환 중인 자리. */
	struct arm_vsmmu_invalidation_cmd *end;	/* [한국어] 배열의 끝 다음 자리. */
	int ret;	/* [한국어] 중간 단계의 결과. */

	cmds = kzalloc_objs(*cmds, array->entry_num);	/* [한국어] 사용자 명령을 통째로 받아 둘 커널 버퍼를 잡는다 — 사용자 메모리를 직접 읽으며 검사하면 검사 후 값이 바뀌는 공격(TOCTOU)이 가능하다. */
	if (!cmds)	/* [한국어] 메모리가 없으면. */
		return -ENOMEM;	/* [한국어] 아무 명령도 처리하지 못했다고 알린다. */
	cur = cmds;	/* [한국어] 처음부터 시작한다. */
	end = cmds + array->entry_num;	/* [한국어] 끝 다음 자리를 미리 계산해 둔다. */

	static_assert(sizeof(*cmds) == 2 * sizeof(u64));	/* [한국어] union 에 패딩이 끼어 크기가 달라지면 사용자 배열과 어긋난다 — 빌드 시점에 막는다. */
	ret = iommu_copy_struct_from_full_user_array(	/* [한국어] 사용자 배열 전체를 한 번에 복사한다. */
		cmds, sizeof(*cmds), array,
		IOMMU_VIOMMU_INVALIDATE_DATA_ARM_SMMUV3);	/* [한국어] 형식 번호를 넘겨, 사용자가 다른 하드웨어용 명령을 보냈으면 걸러 내게 한다. */
	if (ret)	/* [한국어] 복사가 실패하면. */
		goto out;	/* [한국어] 처리 개수 0 으로 나간다. */

	last = cmds;	/* [한국어] 첫 묶음도 처음부터 시작한다. */
	while (cur != end) {	/* [한국어] 배열 끝까지 하나씩 본다. */
		ret = arm_vsmmu_convert_user_cmd(vsmmu, cur);	/* [한국어] 이 명령을 검사하고 안전한 형태로 바꾼다. */
		if (ret)	/* [한국어] 거부된 명령을 만나면. */
			goto out;	/* [한국어] 여기서 멈춘다 — cur 이 문제의 자리를 가리키므로 사용자가 알 수 있다. */

		/* FIXME work in blocks of CMDQ_BATCH_ENTRIES and copy each block? */
		/* [한국어] (위 영어 주석 참고) 지금은 사용자 배열 전체를 한 번에 복사하는데,
		 * 명령이 아주 많으면 그만큼 큰 커널 버퍼가 필요하다. 묶음 단위로 나눠
		 * 복사하는 편이 나을 수 있다는 미완의 과제를 적어 둔 것이다. */
		cur++;	/* [한국어] 다음 자리로 옮긴다. */
		if (cur != end && (cur - last) != CMDQ_BATCH_ENTRIES - 1)	/* [한국어] 배열 끝도 아니고 묶음이 다 차지도 않았다면. */
			continue;	/* [한국어] 아직 큐에 넣지 않고 더 모은다. */

		/* FIXME always uses the main cmdq rather than trying to group by type */
		/* [한국어] (위 영어 주석 참고) Tegra 처럼 보조 큐가 여러 개인 하드웨어에서는
		 * 명령 종류별로 큐를 나눠 넣는 편이 빠르지만, 지금은 주 명령 큐만 쓴다. */
		ret = arm_smmu_cmdq_issue_cmdlist(smmu, &smmu->cmdq, last->cmd,	/* [한국어] 모아 둔 묶음을 한 번에 큐에 넣는다. */
						  cur - last, true);	/* [한국어] 마지막에 완료 대기를 붙여, 돌아왔을 때 무효화가 실제로 끝나 있게 한다 — 게스트는 이 ioctl 이 돌아오면 캐시가 비었다고 믿는다. */
		if (ret) {	/* [한국어] 큐가 막혔거나 시간이 다 됐다면. */
			cur--;	/* [한국어] 이 묶음은 실패했으므로 마지막 명령은 처리되지 않은 것으로 센다. */
			goto out;	/* [한국어] 여기서 멈춘다. */
		}
		last = cur;	/* [한국어] 다음 묶음의 시작을 옮긴다. */
	}
out:	/* [한국어] 성공·실패 모두 이 자리를 지난다. */
	array->entry_num = cur - cmds;	/* [한국어] 실제로 처리한 개수를 사용자에게 알린다 — 어디서 멈췄는지 알려 주는 유일한 단서다. */
	kfree(cmds);	/* [한국어] 커널 버퍼를 놓는다. */
	return ret;	/* [한국어] 마지막 결과를 그대로 돌려준다. */
}

/* [한국어] 가상 SMMU 의 연산표.
 *
 * iommufd 가 사용자 요청을 받아 이 두 갈고리로 내려보낸다 — 게스트가 지은
 * 1단계 설정을 도메인으로 감싸는 일과, 게스트가 낸 무효화를 대신 실행하는 일.
 * 두 갈고리 모두 "게스트 값을 믿지 않고 걸러 낸다"는 같은 원칙 위에 있다. */
static const struct iommufd_viommu_ops arm_vsmmu_ops = {
	.alloc_domain_nested = arm_vsmmu_alloc_domain_nested,	/* [한국어] 게스트 vSTE 를 검사해 중첩 도메인을 만드는 갈고리. */
	.cache_invalidate = arm_vsmmu_cache_invalidate,	/* [한국어] 게스트 무효화 명령을 걸러 실행하는 갈고리. */
};

/*
 * [한국어]
 * arm_smmu_get_viommu_size - 이 장치에 중첩을 켜도 되는지 판단하고 크기를 알린다
 *
 * @dev: 게스트에게 넘길 장치.
 * @viommu_type: 사용자가 요청한 가상 IOMMU 종류.
 * @return: 만들 객체의 바이트 크기, 켤 수 없으면 0.
 *
 * 이름은 크기를 묻는 것 같지만 실제로는 중첩 변환을 허용할지 결정하는
 * 관문이다. 0 을 돌려주면 iommufd 가 요청을 거부하므로, 안전 조건을 여기서
 * 모두 검사한다.
 *
 * 검사 항목이 셋이다. 첫째, 하드웨어가 중첩을 지원해야 한다. 둘째,
 * 명령 큐 강제 동기화 결함이 있는 하드웨어는 제외한다 — 게스트가 낸 명령의
 * 완료 처리가 어떻게 되는지 아직 확인되지 않았기 때문이다. 셋째, 그리고
 * 가장 중요하게, 게스트가 캐시를 우회해 메모리를 건드리지 못해야 한다.
 * VFIO 가 캐시 관리를 하지 않으므로, 장치가 완전히 캐시 일관적이거나
 * (canwbs) 2단계가 캐시 속성을 강제할 수 있어야(S2FWB) 한다.
 *
 * 실행 컨텍스트: iommufd ioctl. 잠들 수 있다.
 *
 * 호출 체인:
 *   IOMMU_VIOMMU_ALLOC ioctl → iommufd → [이 함수]
 */
size_t arm_smmu_get_viommu_size(struct device *dev,
				enum iommu_viommu_type viommu_type)
{
	struct arm_smmu_master *master = dev_iommu_priv_get(dev);	/* [한국어] 장치에 붙여 둔 SMMU 쪽 상태. */
	struct arm_smmu_device *smmu = master->smmu;	/* [한국어] 그 장치가 매달린 하드웨어. */

	if (!(smmu->features & ARM_SMMU_FEAT_NESTING))	/* [한국어] 하드웨어가 1단계와 2단계를 겹쳐 쓸 수 없다면. */
		return 0;	/* [한국어] 중첩 자체가 불가능하므로 거부한다. */

	/*
	 * FORCE_SYNC is not set with FEAT_NESTING. Some study of the exact HW
	 * defect is needed to determine if arm_vsmmu_cache_invalidate() needs
	 * any change to remove this.
	 */
	/* [한국어] (위 영어 주석 참고) 명령마다 완료 대기를 강제해야 하는 결함이 있는
	 * 하드웨어는 중첩과 함께 쓴 전례가 없다. 게스트 명령을 묶음으로 넣는 지금
	 * 방식이 그런 하드웨어에서 안전한지 확인되지 않아, 일단 막아 둔다. */
	if (WARN_ON(smmu->options & ARM_SMMU_OPT_CMDQ_FORCE_SYNC))	/* [한국어] 그런 조합이 나타나면 경고까지 남긴다 — 예상치 못한 하드웨어라는 뜻이다. */
		return 0;	/* [한국어] 거부한다. */

	/*
	 * Must support some way to prevent the VM from bypassing the cache
	 * because VFIO currently does not do any cache maintenance. canwbs
	 * indicates the device is fully coherent and no cache maintenance is
	 * ever required, even for PCI No-Snoop. S2FWB means the S1 can't make
	 * things non-coherent using the memattr, but No-Snoop behavior is not
	 * effected.
	 */
	/* [한국어] (위 영어 주석 참고) 게스트가 캐시를 건너뛰고 메모리를 직접 건드리면
	 * 호스트가 캐시에 들고 있던 값과 어긋난다. VFIO 는 캐시를 씻어 주지 않으므로,
	 * 애초에 그런 일이 불가능한 하드웨어에서만 중첩을 허용한다 — 장치가 완전히
	 * 캐시 일관적이거나, 2단계가 게스트의 메모리 속성을 덮어쓸 수 있어야 한다. */
	if (!arm_smmu_master_canwbs(master) &&	/* [한국어] 장치가 완전히 캐시 일관적이지도 않고. */
	    !(smmu->features & ARM_SMMU_FEAT_S2FWB))	/* [한국어] 2단계가 캐시 속성을 강제하지도 못한다면. */
		return 0;	/* [한국어] 게스트를 믿을 수 없으므로 거부한다. */

	if (viommu_type == IOMMU_VIOMMU_TYPE_ARM_SMMUV3)	/* [한국어] 표준 SMMUv3 가상 IOMMU 를 요청한 경우. */
		return VIOMMU_STRUCT_SIZE(struct arm_vsmmu, core);	/* [한국어] iommufd 가 잡을 크기를 알려 준다. 매크로가 core 필드 위치까지 함께 검사한다. */

	if (!smmu->impl_ops || !smmu->impl_ops->get_viommu_size)	/* [한국어] 그 밖의 종류인데 구현체별 갈고리가 없다면. */
		return 0;	/* [한국어] 모르는 종류이므로 거부한다. */
	return smmu->impl_ops->get_viommu_size(viommu_type);	/* [한국어] 있다면 그쪽에게 판단을 넘긴다 — Tegra 처럼 자기 종류를 더한 하드웨어가 있다. */
}

/*
 * [한국어]
 * arm_vsmmu_init - 가상 SMMU 를 실제 하드웨어와 2단계 도메인에 잇는다
 *
 * @viommu: iommufd 가 이미 잡아 둔 객체 (크기는 위 함수가 알려 준 값).
 * @parent_domain: 게스트를 가둘 2단계 도메인.
 * @user_data: 사용자가 함께 넘긴 설정 (표준 종류에서는 쓰지 않는다).
 * @return: 0 성공, 음수 오류.
 *
 * 부모 2단계 도메인이 정말 같은 하드웨어의 것인지 확인한 뒤, 그 도메인의
 * VMID 를 이어받고 연산표를 건다. 이 VMID 가 앞으로 게스트의 모든 무효화
 * 명령에 강제로 적히는 값이므로, 여기서 정확히 이어받는 것이 중요하다.
 *
 * 실행 컨텍스트: iommufd ioctl. 잠들 수 있다.
 *
 * 호출 체인:
 *   IOMMU_VIOMMU_ALLOC ioctl → iommufd → [이 함수]
 */
int arm_vsmmu_init(struct iommufd_viommu *viommu,
		   struct iommu_domain *parent_domain,
		   const struct iommu_user_data *user_data)
{
	struct arm_vsmmu *vsmmu = container_of(viommu, struct arm_vsmmu, core);	/* [한국어] iommufd 가 잡아 둔 객체에서 이 드라이버의 구조체로 되짚는다. */
	struct arm_smmu_device *smmu =	/* [한국어] iommufd 가 기록해 둔 iommu 장치에서 이 드라이버의 SMMU 로 되짚는다. */
		container_of(viommu->iommu_dev, struct arm_smmu_device, iommu);
	struct arm_smmu_domain *s2_parent = to_smmu_domain(parent_domain);	/* [한국어] 부모 도메인을 이 드라이버의 형태로 되짚는다. */

	if (s2_parent->smmu != smmu)	/* [한국어] 부모 도메인이 다른 SMMU 의 것이라면. */
		return -EINVAL;	/* [한국어] 서로 다른 하드웨어를 엮을 수는 없다. */

	vsmmu->smmu = smmu;	/* [한국어] 명령을 넣을 하드웨어를 기억한다. */
	vsmmu->s2_parent = s2_parent;	/* [한국어] 게스트를 가둘 바깥 울타리를 기억한다. */
	/* FIXME Move VMID allocation from the S2 domain allocation to here */
	/* [한국어] (위 영어 주석 참고) 지금은 2단계 도메인을 만들 때 VMID 가 배정되고
	 * 여기서는 복사만 한다. 가상 IOMMU 단위로 배정하는 편이 개념적으로 맞지만
	 * 아직 옮기지 않았다는 뜻이다. */
	vsmmu->vmid = s2_parent->s2_cfg.vmid;	/* [한국어] 이 값이 앞으로 게스트의 모든 무효화 명령에 강제로 적힌다. */

	if (viommu->type == IOMMU_VIOMMU_TYPE_ARM_SMMUV3) {	/* [한국어] 표준 종류라면. */
		viommu->ops = &arm_vsmmu_ops;	/* [한국어] 위에서 정의한 연산표를 건다. */
		return 0;	/* [한국어] 준비 완료. */
	}

	return smmu->impl_ops->vsmmu_init(vsmmu, user_data);	/* [한국어] 그 밖의 종류는 구현체별 초기화에 넘긴다 — 크기 검사에서 이미 걸러졌으므로 갈고리는 있다고 믿어도 된다. */
}

/*
 * [한국어]
 * arm_vmaster_report_event - 하드웨어 이벤트를 게스트에게 올린다
 *
 * @vmaster: 이 장치의 게스트 쪽 다리 (게스트가 아는 번호를 들고 있다).
 * @evt: 이벤트 큐에서 읽은 원본 워드들.
 * @return: 0 전달 성공, 음수면 전달하지 못했다.
 *
 * 하드웨어가 낸 폴트 기록에는 실제 스트림 번호가 실려 있는데, 게스트는
 * 그 번호를 모른다. 그 자리만 게스트가 아는 번호로 바꿔 끼우고 나머지는
 * 그대로 올린다. 게스트 커널은 자기 이벤트 큐에서 읽은 것처럼 그 기록을
 * 보고, 자기 폴트 처리기를 돌린다.
 *
 * 실제 스트림 번호를 그대로 올리면 안 되는 이유는 두 가지다 — 게스트가
 * 그 번호를 알아볼 수 없고, 호스트의 물리적 배치를 게스트에게 흘리게 된다.
 *
 * 실행 컨텍스트: 이벤트 큐 인터럽트 스레드. streams_mutex 를 쥔 채 불린다 —
 * 그래야 vmaster 가 이 함수가 도는 동안 사라지지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_evtq_thread() → [이 함수] → iommufd_viommu_report_event()
 */
int arm_vmaster_report_event(struct arm_smmu_vmaster *vmaster, u64 *evt)
{
	struct iommu_vevent_arm_smmuv3 vevt;	/* [한국어] 게스트에게 올릴 이벤트 기록. 사용자 공간과 주고받는 형식이다. */
	int i;	/* [한국어] 남은 워드를 옮길 반복자. */

	lockdep_assert_held(&vmaster->vsmmu->smmu->streams_mutex);	/* [한국어] 호출자가 락을 잡고 들어왔는지 확인한다 — 이 락이 vmaster 의 수명을 지킨다. */

	vevt.evt[0] = cpu_to_le64((evt[0] & ~EVTQ_0_SID) |	/* [한국어] 첫 워드에서 실제 스트림 번호를 지우고. */
				  FIELD_PREP(EVTQ_0_SID, vmaster->vsid));	/* [한국어] 게스트가 아는 번호를 대신 적는다. 이 한 줄이 이 함수의 전부다. */
	for (i = 1; i < EVTQ_ENT_DWORDS; i++)	/* [한국어] 나머지 워드들 — 주소, 오류 종류, PASID 등. */
		vevt.evt[i] = cpu_to_le64(evt[i]);	/* [한국어] 손대지 않고 하드웨어 형식으로 옮긴다. 게스트가 자기 하드웨어에서 읽은 것처럼 보여야 한다. */

	return iommufd_viommu_report_event(&vmaster->vsmmu->core,	/* [한국어] iommufd 의 가상 이벤트 큐에 넣는다 — VMM 이 읽어 게스트에게 전달한다. */
					   IOMMU_VEVENTQ_TYPE_ARM_SMMUV3, &vevt,	/* [한국어] 형식 번호를 함께 넘겨, VMM 이 어떻게 해석할지 알게 한다. */
					   sizeof(vevt));
}

MODULE_IMPORT_NS("IOMMUFD");	/* [한국어] iommufd 가 IOMMUFD 이름공간으로 내보낸 심볼을 쓸 수 있게 한다 — 아무 모듈이나 iommufd 내부 함수를 부르지 못하게 막는 장치다. */
