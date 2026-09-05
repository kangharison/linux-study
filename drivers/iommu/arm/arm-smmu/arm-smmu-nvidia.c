// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2019-2020 NVIDIA CORPORATION.  All rights reserved.

/*
 * [한국어 설명] NVIDIA Tegra 의 SMMU 구현체 갈고리 (arm-smmu-nvidia.c)
 *
 * === 파일의 역할 ===
 * 아래 영어 주석이 이 파일의 존재 이유를 밝혀 두었다. Tegra194 에는
 * MMU-500 이 세 개 있고, 그중 둘은 IOVA 접근이 그 사이로 번갈아 나뉘어
 * 가므로 반드시 똑같이 프로그래밍해야 한다.
 *
 * 그래서 이 파일의 핵심은 "쓰기는 모든 인스턴스에, 읽기는 첫 번째에서"
 * 라는 규칙이다. 레지스터 접근 갈고리 넷이 그것을 구현한다.
 *
 * 완료 대기와 오류 처리기도 그에 맞춰 바뀐다. 무효화 완료는 모든
 * 인스턴스가 끝나야 끝난 것이고, 오류는 어느 인스턴스에서든 날 수 있어
 * 모두 살펴야 한다.
 *
 * 메모리 컨트롤러와의 협조도 이 파일이 맡는다. 원 주석대로 그쪽에
 * SID 덮어쓰기를 프로그래밍해야, 펌웨어가 세운 화면 출력 설정을 커널이
 * 매끄럽게 이어받을 수 있다.
 *
 * Tegra194/234 의 하드웨어 결함 우회도 있다 — 큰 페이지 매핑을 아예
 * 금지한다. 아래 init_context 의 주석이 그 이유를 자세히 밝힌다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * arm-smmu.c 의 probe → arm_smmu_impl_init → nvidia_smmu_impl_init
 *   → 이 파일이 갈고리를 단다
 *
 * 이후 arm-smmu.c 의 모든 레지스터 접근이 이 파일의 함수를 거쳐
 * 여러 인스턴스로 퍼진다.
 *
 * === 타 모듈과의 연결 ===
 * 위: arm-smmu-impl.c 가 장치 트리 compatible 을 보고 이리로 보낸다.
 * 옆: soc/tegra/mc — 메모리 컨트롤러 드라이버.
 * 아래: arm-smmu.h 의 레지스터 정의.
 *
 * === 주요 함수/구조체 요약 ===
 * struct nvidia_smmu: 여러 인스턴스의 레지스터 창을 함께 든다.
 * nvidia_smmu_read_reg / write_reg: 읽기는 하나에서, 쓰기는 모두에게.
 * nvidia_smmu_tlb_sync: 모든 인스턴스가 끝날 때까지 기다린다.
 * nvidia_smmu_global_fault / context_fault: 모든 인스턴스를 살핀다.
 * nvidia_smmu_init_context: 결함 우회로 큰 페이지를 금지한다.
 * nvidia_smmu_impl_init: 인스턴스 수를 세어 알맞은 갈고리표를 고른다.
 */
#include <linux/bitfield.h>	/* [한국어] FIELD_GET 등 레지스터 필드 조작. */
#include <linux/delay.h>	/* [한국어] udelay — 무효화 완료를 기다린다. */
#include <linux/of.h>	/* [한국어] 장치 트리 compatible 문자열을 본다. */
#include <linux/platform_device.h>	/* [한국어] 추가 레지스터 창 자원을 얻는다. */
#include <linux/slab.h>	/* [한국어] 구조체를 늘리는 devm_krealloc. */

#include <soc/tegra/mc.h>	/* [한국어] 메모리 컨트롤러 드라이버와 협조하는 API. */

#include "arm-smmu.h"	/* [한국어] 레지스터 정의와 공통 자료 구조. */

/*
 * Tegra194 has three ARM MMU-500 Instances.
 * Two of them are used together and must be programmed identically for
 * interleaved IOVA accesses across them and translates accesses from
 * non-isochronous HW devices.
 * Third one is used for translating accesses from isochronous HW devices.
 *
 * In addition, the SMMU driver needs to coordinate with the memory controller
 * driver to ensure that the right SID override is programmed for any given
 * memory client. This is necessary to allow for use-case such as seamlessly
 * handing over the display controller configuration from the firmware to the
 * kernel.
 *
 * This implementation supports programming of the two instances that must
 * be programmed identically and takes care of invoking the memory controller
 * driver for SID override programming after devices have been attached to an
 * SMMU instance.
 */
#define MAX_SMMU_INSTANCES 2	/* [한국어] 함께 프로그래밍할 인스턴스의 최대 수. 위 주석대로 Tegra194 는 셋 중 둘을 짝으로 쓴다. */

/*
 * [한국어] 여러 MMU-500 인스턴스를 하나처럼 다루는 구조체.
 *
 * 첫 멤버가 공통 구조체라 container_of 로 오갈 수 있다. 공통 코드는
 * SMMU 가 하나뿐이라고 믿고 돌아가며, 이 파일의 갈고리가 그것을 여러
 * 인스턴스로 퍼뜨린다.
 */
struct nvidia_smmu {
	/* [한국어] 공통 SMMU 구조체. 반드시 첫 멤버여야 한다. */
	struct arm_smmu_device smmu;
	/* [한국어] 인스턴스마다의 레지스터 창 주소.
	 *  설정자: nvidia_smmu_impl_init. 첫 칸은 공통 코드가 매핑한 것을 그대로 쓴다.
	 *  읽는 자: nvidia_smmu_page 가 페이지 주소를 만들 때. */
	void __iomem *bases[MAX_SMMU_INSTANCES];
	/* [한국어] 실제로 찾은 인스턴스 수.
	 *  설정자: 장치 트리의 레지스터 창 개수를 세어 정한다.
	 *  값 범위: 1 .. MAX_SMMU_INSTANCES.
	 *  1 이면 공통 코드가 그대로 맞아 가벼운 갈고리표를 쓴다. */
	unsigned int num_instances;
	/* [한국어] 메모리 컨트롤러 드라이버의 손잡이.
	 *  설정자: impl_init 이 찾아 둔다.
	 *  읽는 자: probe_finalize 가 SID 덮어쓰기를 부탁할 때.
	 *  펌웨어가 세운 화면 출력 설정을 이어받으려면 그쪽 협조가 필요하다. */
	struct tegra_mc *mc;
};

/*
 * [한국어]
 * to_nvidia_smmu - 공통 구조체에서 NVIDIA 구조체로 되짚는다
 *
 * @smmu: 공통 구조체.
 * @return: 그것을 품은 NVIDIA 구조체.
 */
static inline struct nvidia_smmu *to_nvidia_smmu(struct arm_smmu_device *smmu)
{
	return container_of(smmu, struct nvidia_smmu, smmu);	/* [한국어] 공통 구조체를 품은 바깥으로 되짚는다. */
}

/*
 * [한국어]
 * nvidia_smmu_page - 지정한 인스턴스의 레지스터 페이지 주소를 구한다
 *
 * @smmu: 공통 구조체.
 * @inst: 인스턴스 번호.
 * @page: 그 안의 페이지 번호.
 * @return: 그 페이지의 가상 주소.
 *
 * 공통 판(arm_smmu_page)과 달리 인스턴스를 고를 수 있다. 그것이 이
 * 파일의 모든 갈고리가 기대는 바탕이다.
 */
static inline void __iomem *nvidia_smmu_page(struct arm_smmu_device *smmu,
					     unsigned int inst, int page)
{
	struct nvidia_smmu *nvidia_smmu;	/* [한국어] 바깥 구조체. */

	nvidia_smmu = container_of(smmu, struct nvidia_smmu, smmu);	/* [한국어] 되짚는다. */
	return nvidia_smmu->bases[inst] + (page << smmu->pgshift);	/* [한국어] 인스턴스마다 레지스터 창이 따로라 그 기준 주소에서 센다. */
}

/*
 * [한국어]
 * nvidia_smmu_read_reg - 첫 인스턴스에서 읽는다
 *
 * @smmu: 공통 구조체.
 * @page: 레지스터 페이지 번호.
 * @offset: 그 안의 오프셋.
 * @return: 읽은 값.
 *
 * 모든 인스턴스를 똑같이 프로그래밍하므로 어느 것을 읽어도 같다.
 * 그래서 첫 번째만 읽는다.
 */
static u32 nvidia_smmu_read_reg(struct arm_smmu_device *smmu,
				int page, int offset)
{
	void __iomem *reg = nvidia_smmu_page(smmu, 0, page) + offset;	/* [한국어] 첫 인스턴스의 주소. */

	return readl_relaxed(reg);	/* [한국어] 모두 똑같이 프로그래밍하므로 어느 것을 읽어도 같다. */
}

/*
 * [한국어]
 * nvidia_smmu_write_reg - 모든 인스턴스에 쓴다
 *
 * @smmu: 공통 구조체.
 * @page: 레지스터 페이지 번호.
 * @offset: 그 안의 오프셋.
 * @val: 쓸 값.
 *
 * IOVA 접근이 인스턴스 사이로 나뉘어 가므로, 설정이 어긋나면 같은
 * 주소가 인스턴스마다 다르게 변환된다.
 */
static void nvidia_smmu_write_reg(struct arm_smmu_device *smmu,
				  int page, int offset, u32 val)
{
	struct nvidia_smmu *nvidia = to_nvidia_smmu(smmu);	/* [한국어] 바깥 구조체. */
	unsigned int i;	/* [한국어] 인스턴스 순회 첨자. */

	for (i = 0; i < nvidia->num_instances; i++) {	/* [한국어] 모든 인스턴스에 */
		void __iomem *reg = nvidia_smmu_page(smmu, i, page) + offset;	/* [한국어] 그 인스턴스의 주소를 구해 */

		writel_relaxed(val, reg);	/* [한국어] 같은 값을 쓴다. 어긋나면 같은 IOVA 가 다르게 변환된다. */
	}
}

/*
 * [한국어]
 * nvidia_smmu_read_reg64 - 첫 인스턴스에서 64비트로 읽는다
 *
 * @smmu: 공통 구조체.
 * @page: 레지스터 페이지 번호.
 * @offset: 그 안의 오프셋.
 * @return: 읽은 값.
 */
static u64 nvidia_smmu_read_reg64(struct arm_smmu_device *smmu,
				  int page, int offset)
{
	void __iomem *reg = nvidia_smmu_page(smmu, 0, page) + offset;	/* [한국어] 첫 인스턴스의 주소. */

	return readq_relaxed(reg);	/* [한국어] 64비트로 한 번에 읽는다. */
}

/*
 * [한국어]
 * nvidia_smmu_write_reg64 - 모든 인스턴스에 64비트로 쓴다
 *
 * @smmu: 공통 구조체.
 * @page: 레지스터 페이지 번호.
 * @offset: 그 안의 오프셋.
 * @val: 쓸 값.
 */
static void nvidia_smmu_write_reg64(struct arm_smmu_device *smmu,
				    int page, int offset, u64 val)
{
	struct nvidia_smmu *nvidia = to_nvidia_smmu(smmu);	/* [한국어] 바깥 구조체. */
	unsigned int i;	/* [한국어] 순회 첨자. */

	for (i = 0; i < nvidia->num_instances; i++) {	/* [한국어] 모든 인스턴스에 */
		void __iomem *reg = nvidia_smmu_page(smmu, i, page) + offset;	/* [한국어] 주소를 구해 */

		writeq_relaxed(val, reg);	/* [한국어] 같은 값을 쓴다. */
	}
}

/*
 * [한국어]
 * nvidia_smmu_tlb_sync - 모든 인스턴스의 무효화가 끝나기를 기다린다
 *
 * @smmu: 공통 구조체.
 * @page: 동기화 레지스터가 있는 페이지.
 * @sync: 동기화 명령 레지스터의 오프셋.
 * @status: 상태 레지스터의 오프셋.
 *
 * 명령은 쓰기 갈고리가 이미 모든 인스턴스에 퍼뜨렸다. 여기서는 상태를
 * 모아서 본다 — 하나라도 진행 중이면 아직 끝난 것이 아니다.
 *
 * 상태 비트를 OR 로 모으는 것이 그 판정 방법이다.
 *
 * 기다리는 구조는 공통 판과 같다 — 먼저 돌고, 그래도 안 끝나면 점점
 * 오래 잔다.
 */
static void nvidia_smmu_tlb_sync(struct arm_smmu_device *smmu, int page,
				 int sync, int status)
{
	struct nvidia_smmu *nvidia = to_nvidia_smmu(smmu);	/* [한국어] 바깥 구조체. */
	unsigned int delay;	/* [한국어] 잠들 시간. 두 배씩 늘려 간다. */

	arm_smmu_writel(smmu, page, sync, 0);	/* [한국어] 동기화를 시작시킨다. 쓰기 갈고리가 모든 인스턴스에 퍼뜨린다. */

	for (delay = 1; delay < TLB_LOOP_TIMEOUT; delay *= 2) {	/* [한국어] 1초까지 기다린다. */
		unsigned int spin_cnt;	/* [한국어] 돌며 기다린 횟수. */

		for (spin_cnt = TLB_SPIN_COUNT; spin_cnt > 0; spin_cnt--) {	/* [한국어] 먼저 돌아 본다 — 대개 곧 끝난다. */
			u32 val = 0;	/* [한국어] 모든 인스턴스의 상태를 모을 값. */
			unsigned int i;	/* [한국어] 인스턴스 순회 첨자. */

			for (i = 0; i < nvidia->num_instances; i++) {	/* [한국어] 인스턴스마다 */
				void __iomem *reg;	/* [한국어] 그 상태 레지스터의 주소. */

				reg = nvidia_smmu_page(smmu, i, page) + status;	/* [한국어] 주소를 구해 */
				val |= readl_relaxed(reg);	/* [한국어] 상태를 모은다. 하나라도 진행 중이면 비트가 선다. */
			}

			if (!(val & ARM_SMMU_sTLBGSTATUS_GSACTIVE))	/* [한국어] 모두 끝났으면 */
				return;	/* [한국어] 돌아간다. */

			cpu_relax();	/* [한국어] 같은 자리를 도는 동안 CPU 에 힌트를 준다. */
		}

		udelay(delay);	/* [한국어] 아직이면 그만큼 잠든다. */
	}

	dev_err_ratelimited(smmu->dev,	/* [한국어] 1초가 지났다. 하드웨어가 멈춘 것이다. */
			    "TLB sync timed out -- SMMU may be deadlocked\n");
}

/*
 * [한국어]
 * nvidia_smmu_reset - 모든 인스턴스의 전역 오류 상태를 지운다
 *
 * @smmu: 공통 구조체.
 * @return: 늘 0.
 *
 * 공통 코드의 reset 은 쓰기 갈고리를 거쳐 이미 모든 인스턴스에 퍼진다.
 * 다만 오류 상태 지우기는 "읽은 값을 되쓰는" 방식이라, 인스턴스마다
 * 값이 다를 수 있어 각자 읽고 각자 지워야 한다.
 */
static int nvidia_smmu_reset(struct arm_smmu_device *smmu)
{
	struct nvidia_smmu *nvidia = to_nvidia_smmu(smmu);	/* [한국어] 바깥 구조체. */
	unsigned int i;	/* [한국어] 순회 첨자. */

	for (i = 0; i < nvidia->num_instances; i++) {	/* [한국어] 인스턴스마다 */
		u32 val;	/* [한국어] 읽어 온 오류 상태. */
		void __iomem *reg = nvidia_smmu_page(smmu, i, ARM_SMMU_GR0) +	/* [한국어] 그 오류 레지스터의 주소. */
				    ARM_SMMU_GR0_sGFSR;

		/* clear global FSR */
		val = readl_relaxed(reg);	/* [한국어] 읽은 값을 */
		writel_relaxed(val, reg);	/* [한국어] 그대로 되써서 지운다. 인스턴스마다 값이 달라 각자 해야 한다. */
	}

	return 0;	/* [한국어] 늘 성공. */
}

/*
 * [한국어]
 * nvidia_smmu_global_fault_inst - 인스턴스 하나의 전역 오류를 다룬다
 *
 * @irq: 인터럽트 번호(쓰지 않는다).
 * @smmu: 공통 구조체.
 * @inst: 살펴볼 인스턴스.
 * @return: IRQ_HANDLED 또는 IRQ_NONE.
 *
 * 공통 판과 하는 일이 같되, 지정한 인스턴스의 레지스터를 직접 읽는다 —
 * 갈고리를 거치면 첫 인스턴스만 보게 되기 때문이다.
 */
static irqreturn_t nvidia_smmu_global_fault_inst(int irq,
						 struct arm_smmu_device *smmu,
						 int inst)
{
	u32 gfsr, gfsynr0, gfsynr1, gfsynr2;	/* [한국어] 오류 상태와 부가 정보. */
	void __iomem *gr0_base = nvidia_smmu_page(smmu, inst, 0);	/* [한국어] 이 인스턴스의 전역 페이지. */

	gfsr = readl_relaxed(gr0_base + ARM_SMMU_GR0_sGFSR);	/* [한국어] 오류 상태. */
	if (!gfsr)	/* [한국어] 이 인스턴스에는 오류가 없으면 */
		return IRQ_NONE;	/* [한국어] 넘어간다. */

	gfsynr0 = readl_relaxed(gr0_base + ARM_SMMU_GR0_sGFSYNR0);	/* [한국어] 부가 정보 0. */
	gfsynr1 = readl_relaxed(gr0_base + ARM_SMMU_GR0_sGFSYNR1);	/* [한국어] 부가 정보 1 — 스트림 id 가 여기 있다. */
	gfsynr2 = readl_relaxed(gr0_base + ARM_SMMU_GR0_sGFSYNR2);	/* [한국어] 부가 정보 2. */

	dev_err_ratelimited(smmu->dev,	/* [한국어] 전역 오류는 원인을 짚기 어려워 심각할 수 있다. */
			    "Unexpected global fault, this could be serious\n");
	dev_err_ratelimited(smmu->dev,	/* [한국어] 1초가 지났다. 인스턴스 중 하나가 응답하지 않는 것이다. */
			    "\tGFSR 0x%08x, GFSYNR0 0x%08x, GFSYNR1 0x%08x, GFSYNR2 0x%08x\n",
			    gfsr, gfsynr0, gfsynr1, gfsynr2);	/* [한국어] 원시 값을 그대로 남긴다. */

	writel_relaxed(gfsr, gr0_base + ARM_SMMU_GR0_sGFSR);	/* [한국어] 읽은 값을 되써서 그것만 지운다. */
	return IRQ_HANDLED;	/* [한국어] 처리했다. */
}

/*
 * [한국어]
 * nvidia_smmu_global_fault - 전역 오류 인터럽트 처리기
 *
 * @irq: 인터럽트 번호.
 * @dev: 등록할 때 넘긴 SMMU.
 * @return: 하나라도 처리했으면 IRQ_HANDLED.
 *
 * 인터럽트 선을 인스턴스들이 나눠 쓰므로 모두 살펴야 한다.
 */
static irqreturn_t nvidia_smmu_global_fault(int irq, void *dev)
{
	unsigned int inst;	/* [한국어] 인스턴스 순회 첨자. */
	irqreturn_t ret = IRQ_NONE;	/* [한국어] 하나도 처리하지 못했을 때의 기본값. */
	struct arm_smmu_device *smmu = dev;	/* [한국어] 등록할 때 넘긴 SMMU. */
	struct nvidia_smmu *nvidia = to_nvidia_smmu(smmu);	/* [한국어] 바깥 구조체. */

	for (inst = 0; inst < nvidia->num_instances; inst++) {	/* [한국어] 인터럽트 선을 나눠 쓰므로 모두 살핀다. */
		irqreturn_t irq_ret;	/* [한국어] 그 인스턴스의 결과. */

		irq_ret = nvidia_smmu_global_fault_inst(irq, smmu, inst);	/* [한국어] 하나씩 처리한다. */
		if (irq_ret == IRQ_HANDLED)	/* [한국어] 하나라도 처리했으면 */
			ret = IRQ_HANDLED;	/* [한국어] 우리 인터럽트였다고 알린다. */
	}

	return ret;	/* [한국어] 결과. */
}

/*
 * [한국어]
 * nvidia_smmu_context_fault_bank - 인스턴스의 뱅크 하나를 살핀다
 *
 * @irq: 인터럽트 번호(쓰지 않는다).
 * @smmu: 공통 구조체.
 * @idx: 뱅크 번호.
 * @inst: 인스턴스 번호.
 * @return: IRQ_HANDLED 또는 IRQ_NONE.
 *
 * 공통 판과 달리 상위 계층에 알리지 않고 곧바로 로그로 찍는다.
 */
static irqreturn_t nvidia_smmu_context_fault_bank(int irq,
						  struct arm_smmu_device *smmu,
						  int idx, int inst)
{
	u32 fsr, fsynr, cbfrsynra;	/* [한국어] 오류 상태와 부가 정보. */
	unsigned long iova;	/* [한국어] 오류가 난 주소. */
	void __iomem *gr1_base = nvidia_smmu_page(smmu, inst, 1);	/* [한국어] 이 인스턴스의 두 번째 전역 페이지. */
	void __iomem *cb_base = nvidia_smmu_page(smmu, inst, smmu->numpage + idx);	/* [한국어] 그 뱅크의 페이지. 전역 페이지들 뒤에 이어진다. */

	fsr = readl_relaxed(cb_base + ARM_SMMU_CB_FSR);	/* [한국어] 오류 상태. */
	if (!(fsr & ARM_SMMU_CB_FSR_FAULT))	/* [한국어] 이 뱅크에는 오류가 없으면 */
		return IRQ_NONE;	/* [한국어] 넘어간다. */

	fsynr = readl_relaxed(cb_base + ARM_SMMU_CB_FSYNR0);	/* [한국어] 부가 정보. */
	iova = readq_relaxed(cb_base + ARM_SMMU_CB_FAR);	/* [한국어] 오류가 난 주소. */
	cbfrsynra = readl_relaxed(gr1_base + ARM_SMMU_GR1_CBFRSYNRA(idx));	/* [한국어] 어느 장치가 냈는지. */

	dev_err_ratelimited(smmu->dev,	/* [한국어] 로그로 찍는다. */
			    "Unhandled context fault: fsr=0x%x, iova=0x%08lx, fsynr=0x%x, cbfrsynra=0x%x, cb=%d\n",
			    fsr, iova, fsynr, cbfrsynra, idx);	/* [한국어] 뱅크 번호까지 붙여 어느 도메인인지 알 수 있게 한다. */

	writel_relaxed(fsr, cb_base + ARM_SMMU_CB_FSR);	/* [한국어] 읽은 값을 되써서 지운다. */
	return IRQ_HANDLED;	/* [한국어] 처리했다. */
}

/*
 * [한국어]
 * nvidia_smmu_context_fault - 문맥 오류 인터럽트 처리기
 *
 * @irq: 인터럽트 번호.
 * @dev: 등록할 때 넘긴 도메인.
 * @return: 하나라도 처리했으면 IRQ_HANDLED.
 *
 * 원 주석대로 인터럽트 선이 모든 문맥에 걸쳐 공유되므로, 어느 뱅크에서
 * 났는지 알 수 없다. 그래서 모든 인스턴스의 모든 뱅크를 훑는다.
 *
 * 그만큼 느리지만, 오류는 드물게만 나므로 문제가 되지 않는다.
 */
static irqreturn_t nvidia_smmu_context_fault(int irq, void *dev)
{
	int idx;	/* [한국어] 뱅크 순회 첨자. */
	unsigned int inst;	/* [한국어] 인스턴스 순회 첨자. */
	irqreturn_t ret = IRQ_NONE;	/* [한국어] 기본값. */
	struct arm_smmu_device *smmu;	/* [한국어] 공통 구조체. */
	struct arm_smmu_domain *smmu_domain = dev;	/* [한국어] 등록할 때 넘긴 도메인. */
	struct nvidia_smmu *nvidia;	/* [한국어] 바깥 구조체. */

	smmu = smmu_domain->smmu;	/* [한국어] 그 도메인이 매인 SMMU. */
	nvidia = to_nvidia_smmu(smmu);	/* [한국어] 바깥으로 되짚는다. */

	for (inst = 0; inst < nvidia->num_instances; inst++) {	/* [한국어] 인스턴스마다 */
		irqreturn_t irq_ret;	/* [한국어] 그 결과. */

		/*
		 * Interrupt line is shared between all contexts.
		 * Check for faults across all contexts.
		 */
		for (idx = 0; idx < smmu->num_context_banks; idx++) {	/* [한국어] 원 주석대로 어느 뱅크에서 났는지 알 수 없어 모두 훑는다. */
			irq_ret = nvidia_smmu_context_fault_bank(irq, smmu,	/* [한국어] 하나씩 살핀다. */
								 idx, inst);
			if (irq_ret == IRQ_HANDLED)	/* [한국어] 하나라도 처리했으면 */
				ret = IRQ_HANDLED;	/* [한국어] 우리 인터럽트였다고 알린다. */
		}
	}

	return ret;	/* [한국어] 결과. */
}

/*
 * [한국어]
 * nvidia_smmu_probe_finalize - 메모리 컨트롤러에 이 장치를 알린다
 *
 * @smmu: 공통 구조체.
 * @dev: 방금 붙은 장치.
 *
 * 원 주석대로 메모리 컨트롤러가 그 장치의 SID 덮어쓰기를 프로그래밍해야
 * 한다. 그래야 펌웨어가 세운 화면 출력 설정을 커널이 매끄럽게 이어받을
 * 수 있다.
 *
 * 실패해도 진행을 막지 않는다 — 그 장치의 일부 기능만 잃는다.
 */
static void nvidia_smmu_probe_finalize(struct arm_smmu_device *smmu, struct device *dev)
{
	struct nvidia_smmu *nvidia = to_nvidia_smmu(smmu);	/* [한국어] 바깥 구조체. */
	int err;	/* [한국어] 결과 코드. */

	err = tegra_mc_probe_device(nvidia->mc, dev);	/* [한국어] 메모리 컨트롤러에 이 장치를 알려 SID 덮어쓰기를 프로그래밍하게 한다. */
	if (err < 0)	/* [한국어] 실패하면 */
		dev_err(smmu->dev, "memory controller probe failed for %s: %d\n",	/* [한국어] 알리되 진행을 막지는 않는다 — 그 장치의 일부 기능만 잃는다. */
			dev_name(dev), err);
}

/*
 * [한국어]
 * (아래 영어 주석과 함께 읽을 것)
 * nvidia_smmu_init_context - 큰 페이지 매핑을 금지한다
 *
 * @smmu_domain: 만들어지는 도메인.
 * @pgtbl_cfg: 페이지 테이블 설정.
 * @dev: 붙는 장치(쓰지 않는다).
 * @return: 늘 0.
 *
 * 원 주석이 결함을 자세히 설명한다 — 순회 캐시 항목이 제대로 무효화되지
 * 않는다. 변환할 때와 무효화할 때 같은 IOVA 로 만들어 내는 캐시 색인이
 * 달라서다.
 *
 * 그 결과 해제할 때 놓인 PMD 항목 자리에 다음 매핑이 새 PTE 표를 넣으면,
 * 순회 캐시에 남은 옛 PMD 항목 때문에 페이지 폴트가 난다.
 *
 * 페이지 크기를 PAGE_SIZE 이하로 제한하면 PMD 항목이 놓이는 일 자체가
 * 없어져 그 경로를 피한다.
 */
static int nvidia_smmu_init_context(struct arm_smmu_domain *smmu_domain,
				    struct io_pgtable_cfg *pgtbl_cfg,
				    struct device *dev)
{
	struct arm_smmu_device *smmu = smmu_domain->smmu;	/* [한국어] 이 도메인이 매인 SMMU. */
	const struct device_node *np = smmu->dev->of_node;	/* [한국어] 그 장치 트리 노드. */

	/*
	 * Tegra194 and Tegra234 SoCs have the erratum that causes walk cache
	 * entries to not be invalidated correctly. The problem is that the walk
	 * cache index generated for IOVA is not same across translation and
	 * invalidation requests. This is leading to page faults when PMD entry
	 * is released during unmap and populated with new PTE table during
	 * subsequent map request. Disabling large page mappings avoids the
	 * release of PMD entry and avoid translations seeing stale PMD entry in
	 * walk cache.
	 * Fix this by limiting the page mappings to PAGE_SIZE on Tegra194 and
	 * Tegra234.
	 */
	if (of_device_is_compatible(np, "nvidia,tegra234-smmu") ||	/* [한국어] 결함이 있는 SoC 인지 본다. */
	    of_device_is_compatible(np, "nvidia,tegra194-smmu")) {	/* [한국어] 두 세대가 같은 결함을 갖는다. */
		smmu->pgsize_bitmap &= GENMASK(PAGE_SHIFT, 0);	/* [한국어] 호스트 페이지 이하 크기만 남긴다 — PMD 항목이 놓이는 일 자체를 없앤다. */
		pgtbl_cfg->pgsize_bitmap = smmu->pgsize_bitmap;	/* [한국어] 표를 만들 설정에도 반영한다. */
	}

	return 0;	/* [한국어] 늘 성공. */
}

/*
 * [한국어] 인스턴스가 여럿일 때의 갈고리표.
 *
 * 레지스터 접근부터 오류 처리까지 거의 모두를 바꾼다.
 */
static const struct arm_smmu_impl nvidia_smmu_impl = {
	.read_reg = nvidia_smmu_read_reg,	/* [한국어] 읽기는 첫 인스턴스에서만. */
	.write_reg = nvidia_smmu_write_reg,	/* [한국어] 쓰기는 모든 인스턴스에. */
	.read_reg64 = nvidia_smmu_read_reg64,	/* [한국어] 64비트 읽기도 첫 인스턴스에서. */
	.write_reg64 = nvidia_smmu_write_reg64,	/* [한국어] 64비트 쓰기도 모두에게. */
	.reset = nvidia_smmu_reset,	/* [한국어] 오류 상태는 인스턴스마다 달라 각자 지운다. */
	.tlb_sync = nvidia_smmu_tlb_sync,	/* [한국어] 모든 인스턴스가 끝나야 끝난 것이다. */
	.global_fault = nvidia_smmu_global_fault,	/* [한국어] 모든 인스턴스를 살핀다. */
	.context_fault = nvidia_smmu_context_fault,	/* [한국어] 인터럽트를 공유해 모든 뱅크를 훑는다. */
	.probe_finalize = nvidia_smmu_probe_finalize,	/* [한국어] 메모리 컨트롤러에 장치를 알린다. */
	.init_context = nvidia_smmu_init_context,	/* [한국어] 결함 우회로 큰 페이지를 금지한다. */
};

/*
 * [한국어] 인스턴스가 하나뿐일 때의 갈고리표.
 *
 * 그때는 공통 코드가 그대로 맞아, 메모리 컨트롤러 협조와 결함 우회만
 * 남긴다.
 */
static const struct arm_smmu_impl nvidia_smmu_single_impl = {
	.probe_finalize = nvidia_smmu_probe_finalize,	/* [한국어] 메모리 컨트롤러에 장치를 알린다. */
	.init_context = nvidia_smmu_init_context,	/* [한국어] 결함 우회로 큰 페이지를 금지한다. */
};

/*
 * [한국어]
 * nvidia_smmu_impl_init - 인스턴스를 세어 갈고리표를 고른다
 *
 * @smmu: 공통 구조체.
 * @return: 늘어난 구조체, 실패하면 오류 포인터.
 *
 * 장치 트리가 레지스터 창을 여러 개 적어 두면 그만큼이 인스턴스다.
 * 첫 번째는 공통 코드가 이미 매핑해 두었으므로 그것을 그대로 쓴다.
 *
 * 인스턴스가 하나면 공통 코드가 그대로 맞아 가벼운 갈고리표를 쓴다.
 */
struct arm_smmu_device *nvidia_smmu_impl_init(struct arm_smmu_device *smmu)
{
	struct resource *res;	/* [한국어] 추가 레지스터 창의 자원 정보. */
	struct device *dev = smmu->dev;	/* [한국어] 이 SMMU 의 장치. */
	struct nvidia_smmu *nvidia_smmu;	/* [한국어] 늘릴 구조체. */
	struct platform_device *pdev = to_platform_device(dev);	/* [한국어] 자원을 얻으려면 플랫폼 장치로 봐야 한다. */
	unsigned int i;	/* [한국어] 순회 첨자. */

	nvidia_smmu = devm_krealloc(dev, smmu, sizeof(*nvidia_smmu), GFP_KERNEL);	/* [한국어] 기존 구조체를 그대로 늘린다. */
	if (!nvidia_smmu)	/* [한국어] 메모리가 없다. */
		return ERR_PTR(-ENOMEM);	/* [한국어] 실패. */

	nvidia_smmu->mc = devm_tegra_memory_controller_get(dev);	/* [한국어] 메모리 컨트롤러를 찾는다. 아직 없으면 나중에 다시 시도하라는 오류가 온다. */
	if (IS_ERR(nvidia_smmu->mc))	/* [한국어] 찾지 못했으면 */
		return ERR_CAST(nvidia_smmu->mc);	/* [한국어] 그대로 올린다. */

	/* Instance 0 is ioremapped by arm-smmu.c. */
	nvidia_smmu->bases[0] = smmu->base;	/* [한국어] 원 주석대로 첫 창은 공통 코드가 이미 매핑해 두었다. */
	nvidia_smmu->num_instances++;	/* [한국어] 그것이 첫 인스턴스다. */

	for (i = 1; i < MAX_SMMU_INSTANCES; i++) {	/* [한국어] 장치 트리가 적어 둔 추가 창마다 */
		res = platform_get_resource(pdev, IORESOURCE_MEM, i);	/* [한국어] 자원을 얻는다. */
		if (!res)	/* [한국어] 더 없으면 */
			break;	/* [한국어] 거기서 끝이다. */

		nvidia_smmu->bases[i] = devm_ioremap_resource(dev, res);	/* [한국어] 그 창을 매핑한다. */
		if (IS_ERR(nvidia_smmu->bases[i]))	/* [한국어] 실패하면 */
			return ERR_CAST(nvidia_smmu->bases[i]);	/* [한국어] 그대로 올린다. */

		nvidia_smmu->num_instances++;	/* [한국어] 인스턴스 수를 늘린다. */
	}

	if (nvidia_smmu->num_instances == 1)	/* [한국어] 하나뿐이면 */
		nvidia_smmu->smmu.impl = &nvidia_smmu_single_impl;	/* [한국어] 공통 코드가 그대로 맞아 가벼운 갈고리표를 쓰고 */
	else
		nvidia_smmu->smmu.impl = &nvidia_smmu_impl;	/* [한국어] 여럿이면 레지스터 접근까지 바꾸는 표를 쓴다. */

	return &nvidia_smmu->smmu;	/* [한국어] 늘어난 구조체의 안쪽을 돌려준다. */
}
