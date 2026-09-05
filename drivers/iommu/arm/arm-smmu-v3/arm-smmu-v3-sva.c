// SPDX-License-Identifier: GPL-2.0
/*
 * Implementation of the IOMMU SVA API for the ARM SMMUv3
 */

/*
 * [한국어 설명] SMMUv3 의 SVA(공유 가상 주소) 구현 (arm-smmu-v3-sva.c)
 *
 * === 파일의 역할 ===
 * 장치가 CPU 프로세스와 같은 페이지 테이블을 그대로 쓰게 만드는 것이 이
 * 파일의 일이다. 보통의 IOMMU 도메인은 커널이 따로 지은 페이지 테이블을
 * 장치에게 보여 주지만, SVA(Shared Virtual Addressing)는 그런 번역 없이
 * 프로세스의 mm->pgd 를 SMMU 의 문맥 서술자에 그대로 꽂는다. 그러면 장치가
 * 낸 주소와 CPU 가 쓰는 포인터가 같은 뜻을 갖게 되어, 사용자 공간이 포인터를
 * 그대로 가속기에 넘길 수 있다.
 * 그 대가로 두 가지 일이 새로 필요해진다. 첫째, CPU 쪽에서 매핑이 바뀌면
 * SMMU 의 TLB 도 함께 비워야 한다 — mmu_notifier 를 달아 그 일을 한다.
 * 둘째, 장치가 아직 매핑되지 않은 주소를 건드리면 폴트가 나는데, 이때
 * 프로세스가 살아 있는 동안은 페이지를 채워 주고(iopf), 프로세스가 죽으면
 * 더 이상 변환하지 않도록 서술자를 막아야 한다.
 * 이 파일은 그 둘을 모두 다룬다. 문맥 서술자를 SVA 용으로 짓는 일
 * (arm_smmu_make_sva_cd), mmu_notifier 콜백 세 개, 그리고 SVA 도메인의
 * 수명 관리(할당·붙이기·해제)가 전부다. 페이지 테이블을 직접 만들지 않는
 * 것이 다른 도메인 종류와 가장 크게 다른 점이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 사용자 공간이 iommu_sva_bind_device() 를 부르면 아래로 이렇게 내려온다:
 *
 *   사용자 공간 (또는 가속기 드라이버)
 *     → iommu_sva_bind_device()  [drivers/iommu/iommu-sva.c]
 *     → arm_smmu_sva_domain_alloc()   ← 이 파일
 *         → ASID 를 잡고, mmu_notifier 를 등록한다
 *     → iommu_attach_device_pasid()
 *     → arm_smmu_sva_set_dev_pasid()  ← 이 파일
 *         → arm_smmu_make_sva_cd() 로 서술자를 짓고
 *         → arm_smmu_set_pasid() 로 문맥 서술자 표에 써 넣는다
 *
 * 그 뒤 장치가 그 PASID 로 DMA 를 내면, SMMU 는 문맥 서술자의 TTB0 —
 * 곧 프로세스의 pgd — 를 걸어가 주소를 번역한다.
 * 반대 방향, 즉 CPU 쪽 변경이 SMMU 로 전해지는 길은 mmu_notifier 다:
 *
 *   프로세스가 munmap/페이지 회수/COW 등으로 매핑을 바꿈
 *     → mmu_notifier 알림
 *     → arm_smmu_mm_arch_invalidate_secondary_tlbs()   ← 이 파일
 *     → arm_smmu_domain_inv_range() → 명령 큐에 TLB 무효화 명령
 *
 * 실행 컨텍스트는 세 갈래다. 도메인 할당·붙이기는 프로세스 문맥에서 잠들 수
 * 있고, mmu_notifier 콜백은 mm 쪽 락을 쥔 채 불려 잠들면 안 되며, 서술자를
 * 고쳐 쓰는 구간은 devices_lock 스핀락 아래에서 돈다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽으로는 iommu 코어의 SVA 계층(drivers/iommu/iommu-sva.c)과 폴트 처리
 * 계층(io-pgfault.c)에 얹힌다. 코어가 mm 하나당 도메인 하나를 관리하고,
 * 이 파일은 그 도메인을 SMMU 하드웨어에 옮겨 담는 일만 맡는다.
 * 옆으로는 같은 디렉토리의 arm-smmu-v3.c 가 제공하는 도구를 쓴다 —
 * arm_smmu_domain_alloc(), arm_smmu_get_cd_ptr(), arm_smmu_write_cd_entry(),
 * arm_smmu_set_pasid(), arm_smmu_domain_inv_range() 가 그것이다. 이 파일은
 * 하드웨어 레지스터를 직접 건드리지 않고, 모두 그 함수들을 거친다.
 * 아래쪽으로는 커널 메모리 관리와 직접 맞닿는다. mmu_notifier 로 mm 의
 * 변경을 구독하고, mm->pgd 의 물리 주소를 서술자에 꽂고, mmget/mmput 으로
 * mm 의 수명을 붙잡는다. io-pgtable 은 쓰지 않는다 — 페이지 테이블을 짓는
 * 쪽이 커널의 일반 MMU 코드이기 때문이다.
 * 공유 상태로는 전역 arm_smmu_asid_xa (ASID 번호 풀)를 arm-smmu-v3.c 와
 * 함께 쓴다. SVA 도메인도 보통 도메인과 같은 ASID 공간을 나눠 쓰므로,
 * 번호가 겹치지 않게 이 xarray 하나로 관리한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - arm_smmu_make_sva_cd(): 프로세스의 pgd 와 CPU 의 변환 설정(TCR/MAIR)을
 *   그대로 옮겨 SVA 용 문맥 서술자를 짓는다. mm 이 NULL 이면 "모두 폴트"
 *   서술자가 되어, 프로세스가 죽은 뒤의 DMA 를 안전하게 막는다.
 * - arm_smmu_mm_arch_invalidate_secondary_tlbs(): CPU 쪽 매핑이 바뀌었을 때
 *   같은 범위를 SMMU TLB 에서도 지운다.
 * - arm_smmu_mm_release(): 프로세스가 죽을 때 서술자를 "모두 폴트"로 바꾼다.
 *   서술자를 지우지 않고 바꾸는 이유는, 지워 버리면 남은 DMA 가 C_BAD_CD
 *   오류를 쏟아 내기 때문이다.
 * - arm_smmu_sva_supported(): 이 SMMU 가 CPU 와 같은 주소 폭·ASID 폭·페이지
 *   크기를 감당할 수 있는지 검사한다. 하나라도 모자라면 SVA 를 못 켠다.
 * - arm_smmu_sva_domain_alloc(): SVA 도메인을 만들고 ASID 를 잡고
 *   mmu_notifier 를 등록한다.
 * - arm_smmu_sva_domain_free(): ASID 를 돌려주고, 실제 해제는 SRCU 콜백으로
 *   미룬다 — 아직 돌고 있는 알림 콜백이 있을 수 있기 때문이다.
 */

#include <linux/mm.h>	/* [한국어] mm_struct 와 pgd 접근. SVA 의 출발점이 프로세스의 mm 이다. */
#include <linux/mmu_context.h>	/* [한국어] vabits_actual 같은 CPU 쪽 주소 폭 정보. 서술자의 T0SZ 를 정할 때 쓴다. */
#include <linux/mmu_notifier.h>	/* [한국어] CPU 매핑 변경을 구독하는 장치. SVA 의 핵심 뼈대다. */
#include <linux/sched/mm.h>	/* [한국어] mmget_not_zero/mmput — 붙이는 동안 mm 이 사라지지 않게 잡아 둔다. */
#include <linux/slab.h>	/* [한국어] 도메인 구조체 할당. */
#include <kunit/visibility.h>	/* [한국어] VISIBLE_IF_KUNIT — 단위 시험에서 static 함수를 들여다볼 수 있게 한다. */

#include "arm-smmu-v3.h"	/* [한국어] 이 드라이버의 자료 모델과 하드웨어 규격. */
#include "../../io-pgtable-arm.h"	/* [한국어] ARM_LPAE_TCR_* — CPU 와 같은 변환 설정 값을 쓰기 위해 필요하다. */

/*
 * [한국어]
 * arm_smmu_update_s1_domain_cd_entry - 1단계 도메인의 서술자를 모두 다시 쓴다
 *
 * @smmu_domain: 대상 도메인.
 *
 * 한 도메인이 여러 장치·여러 PASID 에 걸쳐 있을 수 있으므로, 도메인의
 * 설정이 바뀌면 그 도메인을 가리키던 문맥 서술자를 전부 새로 써야 한다.
 * 이 함수는 도메인에 매달린 (장치, PASID) 짝을 훑으며 하나씩 고쳐 쓴다.
 *
 * 실행 컨텍스트: devices_lock 스핀락을 인터럽트까지 막고 잡은 채 돈다 —
 * 목록을 훑는 동안 장치가 붙거나 떨어지면 안 되기 때문이다. 그래서
 * 이 안에서 부르는 함수들은 모두 잠들지 않아야 한다.
 *
 * __maybe_unused 가 붙은 이유: 지금 구조에서는 부르는 곳이 없어졌지만,
 * 서술자를 통째로 다시 쓰는 경로가 다시 필요해질 수 있어 남겨 두었다.
 *
 * 호출 체인:
 *   (현재는 호출부 없음) → [이 함수] → arm_smmu_write_cd_entry()
 */
static void __maybe_unused
arm_smmu_update_s1_domain_cd_entry(struct arm_smmu_domain *smmu_domain)
{
	struct arm_smmu_master_domain *master_domain;	/* [한국어] 도메인과 장치를 잇는 고리 하나를 가리킬 반복자. */
	struct arm_smmu_cd target_cd;	/* [한국어] 새로 지을 서술자 값을 담아 둘 지역 버퍼. */
	unsigned long flags;	/* [한국어] 인터럽트 상태를 저장할 자리 — 락을 풀 때 그대로 되돌린다. */

	spin_lock_irqsave(&smmu_domain->devices_lock, flags);	/* [한국어] 장치 목록을 훑는 동안 붙이기·떼기가 끼어들지 못하게 막는다. 인터럽트 문맥에서도 이 목록을 건드리므로 irqsave 가 필요하다. */
	list_for_each_entry(master_domain, &smmu_domain->devices, devices_elm) {	/* [한국어] 이 도메인에 매달린 (장치, PASID) 짝을 하나씩 본다. */
		struct arm_smmu_master *master = master_domain->master;	/* [한국어] 그 짝이 가리키는 장치. */
		struct arm_smmu_cd *cdptr;	/* [한국어] 그 장치의 문맥 서술자 표에서 이 PASID 항목이 놓인 자리. */

		cdptr = arm_smmu_get_cd_ptr(master, master_domain->ssid);	/* [한국어] 표에서 항목의 주소를 얻는다. 이미 붙어 있는 짝이므로 자리는 있어야 한다. */
		if (WARN_ON(!cdptr))	/* [한국어] 없다면 자료 구조가 어긋난 것 — 경고만 남기고 이 짝은 건너뛴다. */
			continue;	/* [한국어] 하나가 어긋났다고 나머지까지 포기하지는 않는다. */

		arm_smmu_make_s1_cd(&target_cd, master, smmu_domain);	/* [한국어] 도메인의 현재 설정으로 서술자 값을 새로 짓는다. */
		arm_smmu_write_cd_entry(master, master_domain->ssid, cdptr,	/* [한국어] 하드웨어가 중간 상태를 보지 못하도록 정해진 순서로 고쳐 쓴다. */
					&target_cd);
	}
	spin_unlock_irqrestore(&smmu_domain->devices_lock, flags);	/* [한국어] 목록 훑기가 끝났으니 락과 인터럽트 상태를 되돌린다. */
}

/*
 * [한국어]
 * page_size_to_cd - 커널의 페이지 크기를 서술자의 알갱이 코드로 바꾼다
 *
 * @return: CTXDESC_CD_0_TCR_TG0 에 넣을 값.
 *
 * SVA 는 CPU 의 페이지 테이블을 그대로 쓰므로, SMMU 에게도 CPU 와 똑같은
 * 알갱이 크기를 알려 줘야 한다. 커널의 PAGE_SIZE 는 빌드 시점에 정해지므로
 * 이 함수는 사실상 상수를 돌려주며, 컴파일러가 통째로 접어 버린다.
 *
 * 실행 컨텍스트: 서술자를 짓는 곳 어디서나. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_make_sva_cd() → [이 함수]
 */
static u64 page_size_to_cd(void)
{
	static_assert(PAGE_SIZE == SZ_4K || PAGE_SIZE == SZ_16K ||	/* [한국어] arm64 가 지원하는 세 가지 페이지 크기 밖이면 빌드를 막는다 — 소리 없이 틀린 코드를 내보내는 것보다 낫다. */
		      PAGE_SIZE == SZ_64K);
	if (PAGE_SIZE == SZ_64K)	/* [한국어] 64K 커널이면 그에 맞는 알갱이 코드. */
		return ARM_LPAE_TCR_TG0_64K;	/* [한국어] LPAE 규격이 정한 값을 그대로 쓴다. */
	if (PAGE_SIZE == SZ_16K)	/* [한국어] 16K 커널. */
		return ARM_LPAE_TCR_TG0_16K;	/* [한국어] 같은 규격의 16K 코드. */
	return ARM_LPAE_TCR_TG0_4K;	/* [한국어] 나머지는 4K — 가장 흔한 설정이다. */
}

/*
 * [한국어]
 * arm_smmu_make_sva_cd - 프로세스 주소 공간을 가리키는 문맥 서술자를 짓는다
 *
 * @target: 만들어진 값을 담을 서술자 버퍼 (아직 표에 쓰지는 않는다).
 * @master: 이 서술자를 쓰게 될 장치.
 * @mm: 공유할 프로세스의 메모리 서술자. NULL 이면 "모두 폴트" 서술자가 된다.
 * @asid: 이 주소 공간에 배정된 ASID.
 *
 * SVA 의 핵심이 이 함수다. 커널이 페이지 테이블을 새로 짓는 대신, CPU 가
 * 이미 쓰고 있는 설정 — 물리 주소 폭(PARANGE), 가상 주소 폭(vabits_actual),
 * 페이지 크기, 캐시 속성(MAIR) — 을 그대로 읽어다 서술자에 옮겨 담는다.
 * 그래야 SMMU 가 CPU 와 똑같이 그 페이지 테이블을 걸어갈 수 있다.
 *
 * mm 이 NULL 인 경우가 중요하다. 프로세스가 죽었는데 장치는 아직 DMA 를
 * 내고 있을 수 있다. 이때 서술자를 지워 버리면 하드웨어가 C_BAD_CD 오류를
 * 쏟아 내므로, 서술자는 유효하게 두되 TTB0 를 못 쓰게(EPD0) 막는다.
 * EPD0 가 켜져 있으면 하드웨어가 TTB0 를 아예 읽지 않기 때문에, 두 상태
 * 사이를 끊김 없이(hitless) 오갈 수 있다는 것이 이 설계의 요점이다.
 *
 * 실행 컨텍스트: 붙이기 경로와 mm 해제 콜백 양쪽에서 불린다. 뒤쪽은
 * 스핀락 아래이므로 잠들면 안 된다 — 이 함수는 메모리를 잡지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_sva_set_dev_pasid()/arm_smmu_mm_release() → [이 함수]
 *     → read_sanitised_ftr_reg(), read_sysreg(mair_el1)
 */
VISIBLE_IF_KUNIT	/* [한국어] 단위 시험이 이 함수를 직접 부를 수 있게 열어 둔다 — 서술자를 제대로 짓는지가 시험의 주제다. */
void arm_smmu_make_sva_cd(struct arm_smmu_cd *target,
			  struct arm_smmu_master *master, struct mm_struct *mm,
			  u16 asid)
{
	u64 par;	/* [한국어] CPU 가 지원하는 물리 주소 폭 코드(PARANGE)를 담을 자리. */

	memset(target, 0, sizeof(*target));	/* [한국어] 남은 쓰레기 비트가 하드웨어에게 엉뚱한 뜻으로 읽히지 않도록 통째로 지우고 시작한다. */

	par = cpuid_feature_extract_unsigned_field(	/* [한국어] 여러 CPU 의 능력을 합쳐 놓은(sanitised) 레지스터에서 물리 주소 폭 필드만 꺼낸다. */
		read_sanitised_ftr_reg(SYS_ID_AA64MMFR0_EL1),	/* [한국어] 개별 CPU 가 아니라 시스템 전체가 보장하는 값을 읽어야 한다 — big.LITTLE 처럼 코어마다 다를 수 있기 때문이다. */
		ID_AA64MMFR0_EL1_PARANGE_SHIFT);	/* [한국어] 그 레지스터 안에서 PARANGE 필드의 자리. */

	target->data[0] = cpu_to_le64(	/* [한국어] 서술자 첫 워드를 짓는다. 하드웨어는 리틀엔디안으로 읽으므로 변환해 담는다. */
		CTXDESC_CD_0_TCR_EPD1 |	/* [한국어] TTBR1(커널 절반)은 쓰지 않는다 — 장치는 사용자 주소만 낸다. */
#ifdef __BIG_ENDIAN	/* [한국어] 커널이 빅엔디안으로 빌드된 경우에만. */
		CTXDESC_CD_0_ENDI |	/* [한국어] 페이지 테이블 항목을 빅엔디안으로 읽으라고 하드웨어에 알린다. CPU 와 해석이 어긋나면 엉뚱한 주소를 걷는다. */
#endif
		CTXDESC_CD_0_V |	/* [한국어] 이 서술자가 유효하다는 표시. 이 비트가 없으면 하드웨어가 나머지를 보지 않는다. */
		FIELD_PREP(CTXDESC_CD_0_TCR_IPS, par) |	/* [한국어] 출력 물리 주소 폭을 CPU 와 같게 맞춘다. */
		CTXDESC_CD_0_AA64 |	/* [한국어] AArch64 형식의 페이지 테이블임을 알린다 — AArch32 는 지원하지 않는다. */
		(master->stall_enabled ? CTXDESC_CD_0_S : 0) |	/* [한국어] 이 장치가 멈춰 서기를 지원하면 폴트 시 트랜잭션을 붙잡아 두게 한다. 그래야 페이지를 채운 뒤 다시 굴릴 수 있다. */
		CTXDESC_CD_0_R |	/* [한국어] 접근 플래그 오류를 폴트로 보고하게 한다. */
		CTXDESC_CD_0_A |	/* [한국어] 하드웨어가 접근 플래그를 스스로 갱신하게 한다 — CPU 와 같은 페이지 테이블을 쓰므로 동작도 같아야 한다. */
		CTXDESC_CD_0_ASET |	/* [한국어] 이 ASID 로는 브로드캐스트 TLB 무효화를 받지 않는다는 표시. 무효화는 명령 큐로만 한다. */
		FIELD_PREP(CTXDESC_CD_0_ASID, asid));	/* [한국어] 이 주소 공간의 번호. TLB 항목에 이 값이 태그로 붙어, 무효화 때 골라 지울 수 있다. */

	/*
	 * If no MM is passed then this creates a SVA entry that faults
	 * everything. arm_smmu_write_cd_entry() can hitlessly go between these
	 * two entries types since TTB0 is ignored by HW when EPD0 is set.
	 */
	/* [한국어] (위 영어 주석 참고) mm 이 없으면 "무엇을 건드려도 폴트"인
	 * 서술자가 된다. EPD0 가 켜지면 하드웨어가 TTB0 를 아예 읽지 않으므로,
	 * 정상 서술자와 이 서술자 사이는 끊김 없이 오갈 수 있다. */
	if (mm) {	/* [한국어] 살아 있는 프로세스에 붙이는 경우 — 실제 변환 설정을 채운다. */
		target->data[0] |= cpu_to_le64(	/* [한국어] 첫 워드에 변환 제어(TCR) 필드들을 더한다. */
			FIELD_PREP(CTXDESC_CD_0_TCR_T0SZ,	/* [한국어] 입력 가상 주소 폭. CPU 가 실제로 쓰는 폭과 정확히 같아야 한다. */
				   64ULL - vabits_actual) |	/* [한국어] T0SZ 는 "64에서 주소 폭을 뺀 값"으로 표현한다 — 규격이 그렇게 정했다. */
			FIELD_PREP(CTXDESC_CD_0_TCR_TG0, page_size_to_cd()) |	/* [한국어] 알갱이 크기를 커널의 PAGE_SIZE 에 맞춘다. */
			FIELD_PREP(CTXDESC_CD_0_TCR_IRGN0,	/* [한국어] 페이지 테이블을 읽을 때의 안쪽 캐시 정책. */
				   ARM_LPAE_TCR_RGN_WBWA) |	/* [한국어] 쓰기 되쓰기·쓰기 할당 — CPU 가 페이지 테이블을 다루는 방식과 같게 맞춘다. */
			FIELD_PREP(CTXDESC_CD_0_TCR_ORGN0,	/* [한국어] 바깥 캐시 정책도 같은 이유로. */
				   ARM_LPAE_TCR_RGN_WBWA) |
			FIELD_PREP(CTXDESC_CD_0_TCR_SH0, ARM_LPAE_TCR_SH_IS));	/* [한국어] 안쪽 공유 영역 — SMMU 와 CPU 가 같은 일관성 영역 안에 있어야 페이지 테이블 변경이 서로 보인다. */

		target->data[1] = cpu_to_le64(virt_to_phys(mm->pgd) &	/* [한국어] 프로세스의 페이지 테이블 뿌리를 물리 주소로 바꿔 TTB0 에 꽂는다. 이 한 줄이 SVA 그 자체다. */
					      CTXDESC_CD_1_TTB0_MASK);	/* [한국어] 규격이 정한 주소 비트만 남긴다 — 아래 정렬 비트는 0 이어야 한다. */
	} else {	/* [한국어] mm 이 없는 경우 — 프로세스가 죽었거나 아직 붙이지 않은 상태다. */
		target->data[0] |= cpu_to_le64(CTXDESC_CD_0_TCR_EPD0);	/* [한국어] TTB0 사용을 막는다. 이제 어떤 주소를 내밀어도 변환이 실패한다. */

		/*
		 * Disable stall and immediately generate an abort if stall
		 * disable is permitted. This speeds up cleanup for an unclean
		 * exit if the device is still doing a lot of DMA.
		 */
		/* [한국어] (위 영어 주석 참고) 이미 죽은 주소 공간에 대한 폴트를
		 * 붙잡아 두어 봐야 답해 줄 사람이 없다. 곧바로 중단시키는 편이
		 * 정리를 훨씬 빨리 끝낸다. */
		if (!(master->smmu->features & ARM_SMMU_FEAT_STALL_FORCE))	/* [한국어] 하드웨어가 멈춰 서기를 강제하지 않을 때만 끌 수 있다 — 강제 설정이면 끄는 것이 허용되지 않는다. */
			target->data[0] &=	/* [한국어] 멈춤과 폴트 보고 비트를 지워 즉시 중단으로 만든다. */
				cpu_to_le64(~(CTXDESC_CD_0_S | CTXDESC_CD_0_R));	/* [한국어] 두 비트의 반전 마스크로 AND — 나머지 설정은 그대로 둔다. */
	}

	/*
	 * MAIR value is pretty much constant and global, so we can just get it
	 * from the current CPU register
	 */
	/* [한국어] (위 영어 주석 참고) 메모리 속성 표는 커널 전체가 같은 값을
	 * 쓰므로, 지금 이 CPU 의 레지스터에서 읽어도 안전하다. 페이지 테이블
	 * 항목의 속성 인덱스가 이 표를 거쳐 실제 캐시 동작으로 풀린다. */
	target->data[3] = cpu_to_le64(read_sysreg(mair_el1));	/* [한국어] CPU 의 MAIR_EL1 을 그대로 옮겨 담아, SMMU 도 같은 캐시 속성으로 접근하게 한다. */

	/*
	 * Note that we don't bother with S1PIE on the SMMU, we just rely on
	 * our default encoding scheme matching direct permissions anyway.
	 * SMMU has no notion of S1POE nor GCS, so make sure that is clear if
	 * either is enabled for CPUs, just in case anyone imagines otherwise.
	 */
	/* [한국어] (위 영어 주석 참고) CPU 에는 권한을 덧씌우는 최신 기능들이
	 * 있지만 SMMU 에는 그런 개념이 없다. 곧 장치가 보는 권한과 CPU 가 보는
	 * 권한이 달라질 수 있다는 뜻이라, 그런 기능이 켜져 있으면 한 번 경고한다. */
	if (system_supports_poe() || system_supports_gcs())	/* [한국어] 권한 덧씌우기(POE)나 보호된 호출 스택(GCS)이 켜진 시스템인가. */
		dev_warn_once(master->smmu->dev, "SVA devices ignore permission overlays and GCS\n");	/* [한국어] 부팅마다 한 번만 알린다 — 매 서술자마다 찍으면 로그가 넘친다. */
}
EXPORT_SYMBOL_IF_KUNIT(arm_smmu_make_sva_cd);	/* [한국어] 단위 시험 모듈이 링크할 수 있도록 내보낸다. 시험을 끄면 아무 일도 하지 않는다. */

/*
 * [한국어]
 * arm_smmu_mm_arch_invalidate_secondary_tlbs - CPU 매핑 변경을 SMMU TLB 에 반영한다
 *
 * @mn: 알림 등록에 쓰인 노드. 이것을 품은 도메인을 되찾는 열쇠다.
 * @mm: 바뀐 주소 공간 (여기서는 쓰지 않는다 — 도메인이 이미 그 mm 에 묶여 있다).
 * @start: 바뀐 구간의 시작 주소.
 * @end: 바뀐 구간의 끝 주소 (그 주소는 포함하지 않는다).
 *
 * SVA 에서 가장 자주 불리는 함수다. 프로세스가 munmap 을 하거나, 페이지가
 * 회수되거나, COW 로 물리 페이지가 바뀌면 CPU TLB 를 비우는 것과 함께
 * 이 콜백이 불려 SMMU 의 TLB 도 비운다. 이 알림을 놓치면 장치가 이미
 * 해제된 물리 페이지를 계속 읽고 쓰는 심각한 사고가 난다.
 *
 * 실행 컨텍스트: mm 쪽 락을 쥔 채 불린다. 잠들면 안 되고, 메모리를 잡아서도
 * 안 된다 — 회수 경로에서 불릴 수 있어 재귀가 생기기 때문이다.
 *
 * 호출 체인:
 *   zap_page_range()/try_to_unmap() 등 → mmu_notifier 배포
 *     → [이 함수] → arm_smmu_domain_inv_range() → 명령 큐
 */
static void arm_smmu_mm_arch_invalidate_secondary_tlbs(struct mmu_notifier *mn,
						struct mm_struct *mm,
						unsigned long start,
						unsigned long end)
{
	struct arm_smmu_domain *smmu_domain =	/* [한국어] 알림 노드를 품은 SVA 도메인을 되찾는다. */
		container_of(mn, struct arm_smmu_domain, mmu_notifier);	/* [한국어] 도메인 안에 알림 노드가 박혀 있어, 그 오프셋만큼 빼면 도메인이 나온다. */
	size_t size;	/* [한국어] 무효화할 구간의 길이. */

	/*
	 * The mm_types defines vm_end as the first byte after the end address,
	 * different from IOMMU subsystem using the last address of an address
	 * range. So do a simple translation here by calculating size correctly.
	 */
	/* [한국어] (위 영어 주석 참고) 커널 mm 쪽은 끝 주소를 "구간 다음 바이트"로
	 * 표현하고, IOMMU 쪽은 길이를 받는다. 두 관례가 다르므로 여기서 길이로
	 * 바꿔 넘긴다 — 이 계산을 틀리면 마지막 페이지를 안 지우거나 한 페이지를
	 * 더 지우게 된다. */
	size = end - start;	/* [한국어] 끝에서 시작을 빼면 그대로 길이가 된다. */

	arm_smmu_domain_inv_range(smmu_domain, start, size, PAGE_SIZE, false);	/* [한국어] 그 구간을 페이지 단위로 무효화한다. leaf=false 인 이유는 중간 단계 표도 바뀌었을 수 있기 때문이다. */
}

/*
 * [한국어]
 * arm_smmu_mm_release - 프로세스가 죽을 때 장치의 변환을 막는다
 *
 * @mn: 알림 노드.
 * @mm: 사라지는 주소 공간.
 *
 * 프로세스가 종료되면 페이지 테이블이 곧 해제된다. 그런데 장치는 그 사실을
 * 모른 채 여전히 DMA 를 내고 있을 수 있다. 그대로 두면 이미 반납된 물리
 * 페이지를 장치가 건드리는 심각한 사고가 나므로, 이 콜백에서 그 도메인을
 * 쓰는 모든 문맥 서술자를 "모두 폴트" 상태로 바꾼다.
 *
 * 서술자를 아예 지우지 않는 것이 요점이다. 지워 버리면 하드웨어가 C_BAD_CD
 * 오류를 이벤트 큐에 쏟아 내 로그가 넘치고 처리 비용도 커진다. 유효하되
 * 아무것도 변환하지 못하는 서술자로 바꾸는 편이 훨씬 깔끔하다.
 *
 * 실행 컨텍스트: mm 해제 경로. 안에서 devices_lock 스핀락을 잡으므로
 * 그 구간에서는 잠들 수 없다.
 *
 * 호출 체인:
 *   exit_mmap() → mmu_notifier release → [이 함수]
 *     → arm_smmu_make_sva_cd(mm=NULL) → arm_smmu_write_cd_entry()
 *     → arm_smmu_domain_inv()
 */
static void arm_smmu_mm_release(struct mmu_notifier *mn, struct mm_struct *mm)
{
	struct arm_smmu_domain *smmu_domain =	/* [한국어] 알림 노드에서 도메인을 되찾는다. */
		container_of(mn, struct arm_smmu_domain, mmu_notifier);
	struct arm_smmu_master_domain *master_domain;	/* [한국어] 이 도메인에 매달린 (장치, PASID) 짝을 훑을 반복자. */
	unsigned long flags;	/* [한국어] 인터럽트 상태 저장 자리. */

	/*
	 * DMA may still be running. Keep the cd valid to avoid C_BAD_CD events,
	 * but disable translation.
	 */
	/* [한국어] (위 영어 주석 참고) 아직 DMA 가 돌고 있을 수 있으므로 서술자는
	 * 유효하게 두되 변환만 막는다. 이것이 "지우기"가 아니라 "바꾸기"인 이유다. */
	spin_lock_irqsave(&smmu_domain->devices_lock, flags);	/* [한국어] 훑는 동안 목록이 바뀌지 못하게 막는다. */
	list_for_each_entry(master_domain, &smmu_domain->devices,	/* [한국어] 이 주소 공간을 쓰던 모든 (장치, PASID) 짝을 본다. */
			    devices_elm) {
		struct arm_smmu_master *master = master_domain->master;	/* [한국어] 그 장치. */
		struct arm_smmu_cd target;	/* [한국어] "모두 폴트" 서술자를 지을 지역 버퍼. */
		struct arm_smmu_cd *cdptr;	/* [한국어] 표에서 그 항목이 놓인 자리. */

		cdptr = arm_smmu_get_cd_ptr(master, master_domain->ssid);	/* [한국어] 고쳐 쓸 항목의 주소를 얻는다. */
		if (WARN_ON(!cdptr))	/* [한국어] 붙어 있는데 항목이 없다면 자료 구조가 어긋난 것이다. */
			continue;	/* [한국어] 경고만 남기고 나머지 짝은 계속 처리한다. */
		arm_smmu_make_sva_cd(&target, master, NULL,	/* [한국어] mm 을 NULL 로 넘겨 EPD0 가 켜진 "모두 폴트" 서술자를 짓는다. */
				     smmu_domain->cd.asid);	/* [한국어] ASID 는 그대로 둔다 — 아직 이 도메인의 TLB 항목을 지워야 하기 때문이다. */
		arm_smmu_write_cd_entry(master, master_domain->ssid, cdptr,	/* [한국어] 끊김 없이 새 서술자로 바꾼다. 하드웨어는 중간의 어긋난 상태를 보지 않는다. */
					&target);
	}
	spin_unlock_irqrestore(&smmu_domain->devices_lock, flags);	/* [한국어] 목록 훑기가 끝났다. */

	arm_smmu_domain_inv(smmu_domain);	/* [한국어] 이미 캐시에 올라간 변환까지 모두 지운다 — 서술자를 바꿨어도 TLB 에 남은 항목은 그대로 쓰이기 때문이다. */
}

/*
 * [한국어]
 * arm_smmu_mmu_notifier_free - 알림이 다 빠져나간 뒤 도메인을 놓는다
 *
 * @mn: 놓을 알림 노드.
 *
 * SVA 도메인의 실제 해제는 여기까지 미뤄진다. 도메인을 놓는 시점에도
 * arch_invalidate_secondary_tlbs 콜백이 다른 CPU 에서 돌고 있을 수 있어,
 * mmu_notifier 계층이 SRCU 유예 기간을 지난 뒤에 이 콜백을 부른다.
 * 그때가 되어야 아무도 이 도메인을 보고 있지 않다고 확신할 수 있다.
 *
 * 실행 컨텍스트: SRCU 콜백. 프로세스 문맥이며 잠들 수 있다.
 *
 * 호출 체인:
 *   mmu_notifier_put() → (SRCU 유예) → [이 함수] → arm_smmu_domain_free()
 */
static void arm_smmu_mmu_notifier_free(struct mmu_notifier *mn)
{
	arm_smmu_domain_free(	/* [한국어] 무효화 배열과 도메인 몸통을 놓는다. */
		container_of(mn, struct arm_smmu_domain, mmu_notifier));	/* [한국어] 알림 노드에서 그것을 품은 도메인을 되찾아 넘긴다. */
}

/* [한국어] mm 쪽 변경을 이 드라이버로 전달하는 콜백표.
 *
 * 세 갈래만 쓴다 — 매핑이 바뀔 때(무효화), 프로세스가 죽을 때(변환 차단),
 * 그리고 유예 기간이 지나 안전해졌을 때(해제). 나머지 알림은 SVA 에
 * 필요하지 않다. */
static const struct mmu_notifier_ops arm_smmu_mmu_notifier_ops = {
	.arch_invalidate_secondary_tlbs	= arm_smmu_mm_arch_invalidate_secondary_tlbs,	/* [한국어] CPU TLB 를 비울 때 SMMU TLB 도 함께 비우게 하는 갈고리. */
	.release			= arm_smmu_mm_release,	/* [한국어] 주소 공간이 사라질 때 변환을 막는 갈고리. */
	.free_notifier			= arm_smmu_mmu_notifier_free,	/* [한국어] 유예 기간이 끝난 뒤 실제로 놓는 갈고리. */
};

/*
 * [한국어]
 * arm_smmu_sva_supported - 이 SMMU 가 CPU 와 주소 공간을 나눌 수 있는가
 *
 * @smmu: 검사할 SMMU.
 * @return: 나눌 수 있으면 참.
 *
 * SVA 는 SMMU 가 CPU 의 페이지 테이블을 "그대로" 걸어갈 수 있어야 성립한다.
 * 그래서 검사할 것이 많다 — 캐시 일관성이 있는가, 가상 주소 폭이 CPU 만큼
 * 넓은가, 커널의 페이지 크기를 지원하는가, 물리 주소 폭이 모자라지 않은가,
 * ASID 폭이 CPU 만큼 넓은가. 하나라도 모자라면 같은 페이지 테이블을 두
 * 장치가 다르게 해석하게 되므로, 그런 하드웨어에서는 SVA 를 아예 켜지 않는다.
 *
 * 실행 컨텍스트: 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_device_probe() → [이 함수] → read_sanitised_ftr_reg()
 */
bool arm_smmu_sva_supported(struct arm_smmu_device *smmu)
{
	unsigned long reg, fld;	/* [한국어] CPU 능력 레지스터 값과, 거기서 꺼낸 필드 하나. */
	unsigned long oas;	/* [한국어] CPU 가 내보내는 물리 주소 폭(비트 수). */
	unsigned long asid_bits;	/* [한국어] CPU 가 쓰는 ASID 폭(비트 수). */
	u32 feat_mask = ARM_SMMU_FEAT_COHERENCY;	/* [한국어] 반드시 있어야 할 기능들. 캐시 일관성은 기본 조건이다 — 없으면 CPU 가 고친 페이지 테이블을 SMMU 가 못 본다. */

	if (vabits_actual == 52) {	/* [한국어] 커널이 52비트 가상 주소를 쓰는 경우. */
		/* We don't support LPA2 */
		/* [한국어] (위 영어 주석 참고) 52비트 주소를 4K/16K 알갱이로 쓰려면
		 * LPA2 라는 별도 확장이 필요한데, 이 드라이버는 아직 지원하지 않는다. */
		if (PAGE_SIZE != SZ_64K)	/* [한국어] 64K 알갱이에서만 LPA2 없이 52비트를 쓸 수 있다. */
			return false;	/* [한국어] 그 밖의 조합은 SVA 를 켤 수 없다. */
		feat_mask |= ARM_SMMU_FEAT_VAX;	/* [한국어] SMMU 도 확장된 가상 주소 폭을 지원해야 한다는 조건을 더한다. */
	}

	if (system_supports_bbml2_noabort())	/* [한국어] CPU 가 블록 매핑을 중단 없이 쪼개고 합칠 수 있는 경우. */
		feat_mask |= ARM_SMMU_FEAT_BBML2;	/* [한국어] 그렇다면 SMMU 도 같은 성질을 가져야 한다 — CPU 만 쪼개고 SMMU 가 못 따라오면 그사이 장치가 잘못된 매핑을 본다. */

	if ((smmu->features & feat_mask) != feat_mask)	/* [한국어] 모아 둔 필수 기능을 하나라도 빠뜨렸는가. */
		return false;	/* [한국어] 빠뜨렸다면 SVA 를 켜지 않는다. */

	if (!(smmu->pgsize_bitmap & PAGE_SIZE))	/* [한국어] SMMU 가 커널의 페이지 크기를 지원하는가. */
		return false;	/* [한국어] 지원하지 않으면 CPU 페이지 테이블을 그대로 걸을 수 없다. */

	/*
	 * Get the smallest PA size of all CPUs (sanitized by cpufeature). We're
	 * not even pretending to support AArch32 here. Abort if the MMU outputs
	 * addresses larger than what we support.
	 */
	/* [한국어] (위 영어 주석 참고) 코어마다 물리 주소 폭이 다를 수 있으므로,
	 * 커널이 모든 코어의 최솟값으로 정리해 둔 레지스터를 읽는다. SMMU 가
	 * 그보다 좁으면 CPU 가 만든 페이지 테이블의 주소를 다 표현하지 못한다. */
	reg = read_sanitised_ftr_reg(SYS_ID_AA64MMFR0_EL1);	/* [한국어] 메모리 모델 능력 레지스터를 시스템 전체 기준으로 읽는다. */
	fld = cpuid_feature_extract_unsigned_field(reg, ID_AA64MMFR0_EL1_PARANGE_SHIFT);	/* [한국어] 물리 주소 폭 코드를 꺼낸다. */
	oas = id_aa64mmfr0_parange_to_phys_shift(fld);	/* [한국어] 코드를 실제 비트 수로 바꾼다 (예: 코드 5 → 48비트). */
	if (smmu->oas < oas)	/* [한국어] SMMU 의 출력 주소 폭이 CPU 보다 좁으면. */
		return false;	/* [한국어] 표현하지 못하는 물리 주소가 생기므로 SVA 를 못 켠다. */

	/* We can support bigger ASIDs than the CPU, but not smaller */
	/* [한국어] (위 영어 주석 참고) ASID 는 SMMU 쪽이 더 넓어도 상관없다 —
	 * CPU 가 쓰는 번호를 모두 담을 수만 있으면 된다. */
	fld = cpuid_feature_extract_unsigned_field(reg, ID_AA64MMFR0_EL1_ASIDBITS_SHIFT);	/* [한국어] CPU 의 ASID 폭 코드를 꺼낸다. */
	asid_bits = fld ? 16 : 8;	/* [한국어] 코드가 0 이면 8비트, 아니면 16비트 — 규격이 두 값만 정의한다. */
	if (smmu->asid_bits < asid_bits)	/* [한국어] SMMU 가 그보다 좁으면 CPU 의 ASID 를 다 담지 못한다. */
		return false;	/* [한국어] 담지 못하면 서로 다른 주소 공간이 같은 태그를 쓰게 되어 위험하다. */

	/*
	 * See max_pinned_asids in arch/arm64/mm/context.c. The following is
	 * generally the maximum number of bindable processes.
	 */
	/* [한국어] (위 영어 주석 참고) SVA 로 묶인 프로세스는 ASID 를 놓지 못하고
	 * 붙잡아 둔다. 그래서 동시에 묶을 수 있는 프로세스 수에 한계가 생기고,
	 * 아래 계산이 그 대략적인 상한이다. */
	if (arm64_kernel_unmapped_at_el0())	/* [한국어] KPTI(커널 페이지 테이블 분리)가 켜져 있으면. */
		asid_bits--;	/* [한국어] ASID 를 커널·사용자 짝으로 나눠 쓰므로 쓸 수 있는 폭이 한 비트 줄어든다. */
	dev_dbg(smmu->dev, "%d shared contexts\n", (1 << asid_bits) -	/* [한국어] 디버그 로그로만 남긴다 — 전체 ASID 수에서. */
		num_possible_cpus() - 2);	/* [한국어] CPU 마다 하나씩 예약된 몫과 예약 번호 둘을 빼면 실제로 묶을 수 있는 수가 나온다. */

	return true;	/* [한국어] 모든 조건을 통과했으니 이 SMMU 는 SVA 를 쓸 수 있다. */
}

/*
 * [한국어]
 * arm_smmu_sva_notifier_synchronize - 남은 알림 콜백이 끝나기를 기다린다
 *
 * 모듈을 내릴 때, mmu_notifier_put() 으로 넘긴 해제 콜백이 아직 SRCU 유예
 * 기간 안에 있을 수 있다. 그 상태로 모듈 코드를 메모리에서 없애면 이미
 * 사라진 함수를 부르게 되므로, 여기서 남은 콜백이 다 빠져나가기를 기다린다.
 *
 * 실행 컨텍스트: 모듈 해제. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_driver_exit() → [이 함수] → mmu_notifier_synchronize()
 */
void arm_smmu_sva_notifier_synchronize(void)
{
	/*
	 * Some MMU notifiers may still be waiting to be freed, using
	 * arm_smmu_mmu_notifier_free(). Wait for them.
	 */
	/* [한국어] (위 영어 주석 참고) 아직 해제를 기다리는 알림이 남아 있을 수
	 * 있다. 그것들이 모두 정리될 때까지 여기서 멈춰 선다. */
	mmu_notifier_synchronize();	/* [한국어] SRCU 유예 기간이 모두 지날 때까지 기다린다 — 이 뒤로는 우리 콜백이 불릴 일이 없다. */
}

/*
 * [한국어]
 * arm_smmu_sva_set_dev_pasid - 그 PASID 를 프로세스 주소 공간에 잇는다
 *
 * @domain: SVA 도메인 (안에 mm 을 품고 있다).
 * @dev: 이을 장치.
 * @id: 이을 PASID 번호.
 * @old: 그 PASID 가 쓰던 이전 도메인.
 * @return: 0 성공, 음수 오류.
 *
 * 장치가 그 PASID 로 DMA 를 내면 프로세스의 페이지 테이블로 번역되도록,
 * 문맥 서술자 표의 그 자리에 SVA 서술자를 써 넣는다. 붙이는 도중에
 * 프로세스가 죽어 arm_smmu_mm_release() 가 끼어들면 방금 쓴 서술자가
 * 그대로 남아 버리므로, mmget_not_zero() 로 mm 을 붙잡아 두고 진행한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 잠들 수 있다.
 *
 * 호출 체인:
 *   iommu_attach_device_pasid() → [이 함수]
 *     → arm_smmu_make_sva_cd() → arm_smmu_set_pasid()
 */
static int arm_smmu_sva_set_dev_pasid(struct iommu_domain *domain,
				      struct device *dev, ioasid_t id,
				      struct iommu_domain *old)
{
	struct arm_smmu_domain *smmu_domain = to_smmu_domain(domain);	/* [한국어] 코어 도메인에서 이 드라이버의 도메인으로 되짚는다. */
	struct arm_smmu_master *master = dev_iommu_priv_get(dev);	/* [한국어] 장치에 붙여 둔 SMMU 쪽 상태를 꺼낸다. */
	struct arm_smmu_cd target;	/* [한국어] 새로 지을 SVA 서술자를 담을 지역 버퍼. */
	int ret;	/* [한국어] 붙이기 결과. */

	if (!(master->smmu->features & ARM_SMMU_FEAT_SVA))	/* [한국어] 이 하드웨어가 SVA 를 못 쓴다면. */
		return -EOPNOTSUPP;	/* [한국어] 코어에게 지원하지 않는다고 알린다. */

	/* Prevent arm_smmu_mm_release from being called while we are attaching */
	/* [한국어] (위 영어 주석 참고) 붙이는 도중에 프로세스가 죽으면, 죽음을
	 * 처리하는 콜백이 먼저 지나간 뒤에 우리가 유효한 서술자를 써 넣는
	 * 역전이 생긴다. 그러면 장치가 이미 사라진 주소 공간을 계속 보게 된다. */
	if (!mmget_not_zero(domain->mm))	/* [한국어] mm 의 참조를 올려 둔다. 이미 0 이면 프로세스가 죽고 있는 중이다. */
		return -EINVAL;	/* [한국어] 죽어 가는 주소 공간에는 붙일 수 없다. */

	/*
	 * This does not need the arm_smmu_asid_lock because SVA domains never
	 * get reassigned
	 */
	/* [한국어] (위 영어 주석 참고) 보통 도메인은 ASID 를 다른 도메인에게
	 * 넘겨줄 수 있어 락이 필요하지만, SVA 도메인의 ASID 는 도메인이 사라질
	 * 때까지 그대로다. 그래서 락 없이 읽어도 안전하다. */
	arm_smmu_make_sva_cd(&target, master, domain->mm, smmu_domain->cd.asid);	/* [한국어] 프로세스의 pgd 를 꽂은 서술자를 짓는다. */
	ret = arm_smmu_set_pasid(master, smmu_domain, id, &target, old);	/* [한국어] 문맥 서술자 표의 그 PASID 자리에 써 넣고, 무효화 배열도 새 도메인 쪽으로 옮긴다. */

	mmput(domain->mm);	/* [한국어] 붙이기가 끝났으니 붙잡아 둔 mm 참조를 놓는다. 이제 죽음 처리가 끼어들어도 순서가 어긋나지 않는다. */
	return ret;	/* [한국어] 코어에게 결과를 그대로 돌려준다. */
}

/*
 * [한국어]
 * arm_smmu_sva_domain_free - SVA 도메인을 놓는다
 *
 * @domain: 놓을 도메인.
 *
 * ASID 를 돌려주기 전에 그 ASID 로 태그된 TLB 항목을 먼저 지운다. 순서가
 * 뒤바뀌면, 번호를 넘겨받은 다음 도메인이 앞 도메인의 낡은 변환을 그대로
 * 쓰게 되는 심각한 사고가 난다.
 *
 * 실제 메모리 해제는 여기서 하지 않고 mmu_notifier_put() 에 맡긴다.
 * 아직 무효화 콜백이 돌고 있을 수 있어, SRCU 유예 기간을 지나야
 * arm_smmu_mmu_notifier_free() 가 불려 도메인을 놓는다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 잠들 수 있다.
 *
 * 호출 체인:
 *   iommu_domain_free() → [이 함수]
 *     → arm_smmu_domain_inv() → xa_erase() → mmu_notifier_put()
 */
static void arm_smmu_sva_domain_free(struct iommu_domain *domain)
{
	struct arm_smmu_domain *smmu_domain = to_smmu_domain(domain);	/* [한국어] 코어 도메인에서 이 드라이버의 도메인으로 되짚는다. */

	/*
	 * Ensure the ASID is empty in the iommu cache before allowing reuse.
	 */
	/* [한국어] (위 영어 주석 참고) 번호를 돌려주기 전에 그 번호로 캐시에 남은
	 * 변환을 반드시 먼저 비워야 한다. 이 순서가 SVA 해제의 안전을 지탱한다. */
	arm_smmu_domain_inv(smmu_domain);	/* [한국어] 이 도메인의 TLB 항목을 모두 지운다. */

	/*
	 * Notice that the arm_smmu_mm_arch_invalidate_secondary_tlbs op can
	 * still be called/running at this point. We allow the ASID to be
	 * reused, and if there is a race then it just suffers harmless
	 * unnecessary invalidation.
	 */
	/* [한국어] (위 영어 주석 참고) 이 시점에도 무효화 콜백이 돌고 있을 수 있다.
	 * 그 경합이 나면 다음 주인이 쓰는 ASID 를 괜히 한 번 더 비우게 되는데,
	 * 성능만 조금 손해일 뿐 정확성은 깨지지 않는다 — 그래서 그냥 허용한다. */
	xa_erase(&arm_smmu_asid_xa, smmu_domain->cd.asid);	/* [한국어] ASID 번호를 전역 풀에 돌려준다. 이 뒤로 다른 도메인이 이 번호를 받을 수 있다. */

	/*
	 * Actual free is defered to the SRCU callback
	 * arm_smmu_mmu_notifier_free()
	 */
	/* [한국어] (위 영어 주석 참고) 도메인 메모리를 여기서 놓으면, 아직 돌고 있는
	 * 알림 콜백이 사라진 구조체를 건드린다. 그래서 해제를 유예 콜백으로 미룬다. */
	mmu_notifier_put(&smmu_domain->mmu_notifier);	/* [한국어] 알림 등록을 풀고, 유예 기간이 지나면 free_notifier 콜백이 불리게 한다. */
}

/* [한국어] SVA 도메인의 연산표.
 *
 * 매우 짧다 — map/unmap 이 없다는 점이 핵심이다. SVA 도메인은 페이지
 * 테이블을 스스로 만들지 않고 프로세스의 것을 빌려 쓰므로, 매핑을 걸고
 * 푸는 일은 커널 MMU 가 하고 이 드라이버는 붙이고 놓는 일만 맡는다. */
static const struct iommu_domain_ops arm_smmu_sva_domain_ops = {
	.set_dev_pasid		= arm_smmu_sva_set_dev_pasid,	/* [한국어] 그 PASID 를 이 주소 공간에 잇는 갈고리. */
	.free			= arm_smmu_sva_domain_free	/* [한국어] 도메인을 놓는 갈고리. */
};

/*
 * [한국어]
 * arm_smmu_sva_domain_alloc - 프로세스 주소 공간을 쓰는 도메인을 만든다
 *
 * @dev: 그 주소 공간을 쓸 장치.
 * @mm: 공유할 프로세스의 메모리 서술자.
 * @return: 만들어진 도메인, 실패하면 ERR_PTR.
 *
 * 페이지 테이블을 만들지 않는다는 점만 빼면 보통 도메인 할당과 비슷하다.
 * 하는 일은 세 가지다 — 도메인 껍데기를 잡고, ASID 를 전역 풀에서 받고,
 * mmu_notifier 를 등록해 CPU 쪽 변경을 구독한다. 알림 등록이 마지막인
 * 이유는, 등록하는 순간부터 콜백이 들어올 수 있어 그 전에 도메인이
 * 완전히 준비되어 있어야 하기 때문이다.
 *
 * 오류 처리는 되감기 순서를 지킨다 — ASID 를 받은 뒤 알림 등록이 실패하면
 * 받은 번호를 먼저 돌려주고, 그다음 도메인을 놓는다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 잠들 수 있다.
 *
 * 호출 체인:
 *   iommu_sva_bind_device() → iommu_paging_domain_alloc → [이 함수]
 *     → arm_smmu_domain_alloc() → xa_alloc() → mmu_notifier_register()
 */
struct iommu_domain *arm_smmu_sva_domain_alloc(struct device *dev,
					       struct mm_struct *mm)
{
	struct arm_smmu_master *master = dev_iommu_priv_get(dev);	/* [한국어] 장치에 붙여 둔 SMMU 쪽 상태. */
	struct arm_smmu_device *smmu = master->smmu;	/* [한국어] 그 장치가 매달린 SMMU. ASID 폭 등을 여기서 본다. */
	struct arm_smmu_domain *smmu_domain;	/* [한국어] 만들어 낼 도메인. */
	u32 asid;	/* [한국어] 전역 풀에서 받아 올 ASID 번호. */
	int ret;	/* [한국어] 중간 단계의 실패를 담을 자리. */

	if (!(master->smmu->features & ARM_SMMU_FEAT_SVA))	/* [한국어] 이 하드웨어가 SVA 를 못 쓴다면. */
		return ERR_PTR(-EOPNOTSUPP);	/* [한국어] 오류 포인터로 알린다 — 코어가 IS_ERR 로 걸러 낸다. */

	smmu_domain = arm_smmu_domain_alloc();	/* [한국어] 도메인 껍데기와 빈 무효화 배열을 잡는다. */
	if (IS_ERR(smmu_domain))	/* [한국어] 메모리가 없으면 여기서 접는다. */
		return ERR_CAST(smmu_domain);	/* [한국어] 오류 코드를 그대로 위로 넘긴다. */
	smmu_domain->domain.type = IOMMU_DOMAIN_SVA;	/* [한국어] 코어에게 이 도메인이 SVA 종류임을 알린다 — 코어의 처리 갈래가 달라진다. */
	smmu_domain->domain.ops = &arm_smmu_sva_domain_ops;	/* [한국어] 위에서 정의한 짧은 연산표를 건다. */

	/*
	 * Choose page_size as the leaf page size for invalidation when
	 * ARM_SMMU_FEAT_RANGE_INV is present
	 */
	/* [한국어] (위 영어 주석 참고) 범위 무효화를 쓸 때 한 걸음의 크기가 될 값이다.
	 * CPU 페이지 테이블을 그대로 쓰므로 커널의 페이지 크기와 같아야 한다. */
	smmu_domain->domain.pgsize_bitmap = PAGE_SIZE;	/* [한국어] 이 도메인이 다루는 알갱이는 커널 페이지 하나뿐이다. */
	smmu_domain->stage = ARM_SMMU_DOMAIN_SVA;	/* [한국어] 드라이버 안에서 이 도메인의 갈래를 구분하는 표시. */
	smmu_domain->smmu = smmu;	/* [한국어] 어느 SMMU 에 속한 도메인인지 기록한다 — 무효화 명령을 어느 큐에 넣을지가 여기서 정해진다. */

	ret = xa_alloc(&arm_smmu_asid_xa, &asid, smmu_domain,	/* [한국어] 전역 ASID 풀에서 빈 번호 하나를 받아 이 도메인에 묶는다. */
		       XA_LIMIT(1, (1 << smmu->asid_bits) - 1), GFP_KERNEL);	/* [한국어] 0번은 쓰지 않는다 — 예약된 뜻이 있어서다. 위쪽 한계는 하드웨어의 ASID 폭이 정한다. */
	if (ret)	/* [한국어] 번호가 동나면 더 이상 프로세스를 묶을 수 없다. */
		goto err_free;	/* [한국어] 방금 잡은 도메인을 놓고 나간다. */

	smmu_domain->cd.asid = asid;	/* [한국어] 받은 번호를 문맥 서술자 설정에 기록한다 — 서술자를 지을 때 이 값이 쓰인다. */
	smmu_domain->mmu_notifier.ops = &arm_smmu_mmu_notifier_ops;	/* [한국어] 알림 콜백표를 걸어 둔다. 등록 전에 채워야 한다. */
	ret = mmu_notifier_register(&smmu_domain->mmu_notifier, mm);	/* [한국어] 그 mm 의 변경을 구독하기 시작한다. 이 줄 이후로 콜백이 들어올 수 있다. */
	if (ret)	/* [한국어] 등록이 실패하면 (메모리 부족 등). */
		goto err_asid;	/* [한국어] 받은 ASID 를 먼저 돌려주고 나간다. */

	return &smmu_domain->domain;	/* [한국어] 코어가 아는 형태로 돌려준다 — 첫 필드가 iommu_domain 이라 그대로 쓸 수 있다. */

err_asid:	/* [한국어] 알림 등록이 실패했을 때의 되감기 지점. */
	xa_erase(&arm_smmu_asid_xa, smmu_domain->cd.asid);	/* [한국어] 받았던 번호를 풀에 돌려준다 — 아직 아무도 쓰지 않았으므로 무효화는 필요 없다. */
err_free:	/* [한국어] ASID 를 받기도 전에 실패했을 때의 되감기 지점. */
	arm_smmu_domain_free(smmu_domain);	/* [한국어] 도메인 껍데기를 놓는다. 알림을 등록하기 전이라 유예 없이 바로 놓아도 된다. */
	return ERR_PTR(ret);	/* [한국어] 실패 이유를 오류 포인터로 감싸 돌려준다. */
}
