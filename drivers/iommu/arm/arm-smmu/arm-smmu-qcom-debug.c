// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

/*
 * [한국어 설명] 퀄컴 SMMU 의 디버그 블록(TBU)을 다루는 곳 (arm-smmu-qcom-debug.c)
 *
 * === 파일의 역할 ===
 * 퀄컴 SoC 의 SMMU 에는 규격에 없는 디버그 하드웨어가 붙어 있다. TBU
 * (Translation Buffer Unit)라 부르는 그 블록은 스트림 무리마다 하나씩
 * 있고, 두 가지 일에 쓰인다.
 *
 * 첫째, ATOS(주소 변환 연산)를 손수 일으켜 하드웨어가 그 IOVA 를 어떻게
 * 변환하는지 물어본다. 폴트가 났을 때 "표에는 있는데 하드웨어가 못 찾는
 * 것인지, 표에도 없는 것인지"를 가려내는 결정적인 단서가 된다.
 *
 * 둘째, 무효화가 끝나지 않을 때 어느 TBU 가 응답하지 않는지 알려 준다.
 * 그 레지스터는 보안 세계가 쥐고 있어 SCM 호출로 읽는다.
 *
 * 폴트 처리기도 여기서 확장된다. 소프트웨어 표 순회 결과와 하드웨어
 * ATOS 결과를 함께 찍어, 두 값이 다르면 TLB 문제임을 드러낸다.
 * TLB 를 비운 전후로 두 번 물어보는 검증까지 한다.
 *
 * -EBUSY 응답의 처리가 이 파일의 미묘한 부분이다. 클라이언트가 그것을
 * 돌려주면 폴트 상태를 그대로 두어, 그쪽이 디버깅을 마칠 때까지 트랜잭션이
 * 멈춘 채로 남는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * arm-smmu.c 의 오류 인터럽트 → 퀄컴 갈고리 → qcom_smmu_context_fault
 *   → qcom_smmu_verify_fault → TBU 의 ATOS → 하드웨어가 답한 물리 주소
 *
 * arm-smmu.c 의 무효화 대기가 시간을 다하면
 *   → 퀄컴 갈고리 → qcom_smmu_tlb_sync_debug → SCM → 보안 레지스터
 *
 * TBU 는 별도 플랫폼 장치라 qcom_tbu_probe 가 따로 잡는다.
 *
 * === 타 모듈과의 연결 ===
 * 위: arm-smmu-qcom.c 가 갈고리로 이 함수들을 연결한다.
 * 옆: qcom_scm(보안 레지스터 읽기), interconnect(대역폭 확보),
 *   clk(TBU 클럭).
 * 아래: arm-smmu.h 의 레지스터 접근.
 *
 * === 주요 함수/구조체 요약 ===
 * struct qcom_tbu: TBU 하나. 담당하는 스트림 id 범위와 레지스터 창을 든다.
 * qcom_tbu_halt / resume: ATOS 를 하려면 그 TBU 를 멈춰 세워야 한다.
 * qcom_tbu_trigger_atos: 하드웨어에게 주소 변환을 시키고 결과를 읽는다.
 * qcom_smmu_verify_fault: TLB 를 비운 전후로 두 번 물어 값을 견준다.
 * qcom_smmu_context_fault: 확장된 폴트 처리기.
 * qcom_tbu_probe: 장치 트리의 TBU 노드를 잡아 목록에 넣는다.
 */
#include <linux/cleanup.h>	/* [한국어] guard / scoped_guard — 나갈 때 저절로 풀리는 락. */
#include <linux/device.h>	/* [한국어] 플랫폼 장치 모형. */
#include <linux/interconnect.h>	/* [한국어] ATOS 때 연결망 대역폭을 확보한다. */
#include <linux/firmware/qcom/qcom_scm.h>	/* [한국어] 보안 세계가 쥔 레지스터를 SCM 호출로 읽는다. */
#include <linux/iopoll.h>	/* [한국어] readl_poll_timeout — 정지 확인을 기다린다. */
#include <linux/list.h>	/* [한국어] 전역 TBU 목록. */
#include <linux/mod_devicetable.h>	/* [한국어] 장치 트리 매칭 표 타입. */
#include <linux/mutex.h>	/* [한국어] 그 목록을 지키는 뮤텍스. */
#include <linux/platform_device.h>	/* [한국어] TBU 는 별도 플랫폼 장치다. */
#include <linux/ratelimit.h>	/* [한국어] 오류 로그의 속도 제한. */
#include <linux/spinlock.h>	/* [한국어] 정지 카운터와 ATOS 직렬화. */

#include "arm-smmu.h"	/* [한국어] 레지스터 정의와 공통 자료 구조. */
#include "arm-smmu-qcom.h"	/* [한국어] 퀄컴 확장 구조체와 공유 선언. */

#define TBU_DBG_TIMEOUT_US		100	/* [한국어] 정지와 ATOS 를 기다리는 최대 시간(마이크로초). 짧은 이유는 원자적 문맥에서 도는 대기라서다. */
#define DEBUG_AXUSER_REG		0x30	/* [한국어] 버스가 요구하는 부가 식별자 레지스터. */
#define DEBUG_AXUSER_CDMID		GENMASK_ULL(43, 36)	/* [한국어] 그 안의 식별자 필드. */
#define DEBUG_AXUSER_CDMID_VAL		0xff	/* [한국어] 거기에 넣을 값. 하드웨어가 정한 상수다. */
#define DEBUG_PAR_REG			0x28	/* [한국어] ATOS 결과 레지스터. */
#define DEBUG_PAR_FAULT_VAL		BIT(0)	/* [한국어] 변환이 실패했다 — 그 주소에 매핑이 없다는 뜻. */
#define DEBUG_PAR_PA			GENMASK_ULL(47, 12)	/* [한국어] 성공했을 때의 물리 페이지 주소. */
#define DEBUG_SID_HALT_REG		0x0	/* [한국어] 정지 제어이자 스트림 id 지정 레지스터. */
#define DEBUG_SID_HALT_VAL		BIT(16)	/* [한국어] 이 비트를 세우면 그 TBU 가 멈춘다. */
#define DEBUG_SID_HALT_SID		GENMASK(9, 0)	/* [한국어] ATOS 를 할 스트림 id 자리. */
#define DEBUG_SR_HALT_ACK_REG		0x20	/* [한국어] 정지 확인과 ATOS 진행 상태 레지스터. */
#define DEBUG_SR_HALT_ACK_VAL		BIT(1)	/* [한국어] 정지가 실제로 이뤄졌다. */
#define DEBUG_SR_ECATS_RUNNING_VAL	BIT(0)	/* [한국어] ATOS 가 아직 진행 중이다. */
#define DEBUG_TXN_AXCACHE		GENMASK(5, 2)	/* [한국어] ATOS 접근의 캐시 속성. */
#define DEBUG_TXN_AXPROT		GENMASK(8, 6)	/* [한국어] 그 보호 속성. */
#define DEBUG_TXN_AXPROT_PRIV		0x1	/* [한국어] 특권 접근 비트. */
#define DEBUG_TXN_AXPROT_NSEC		0x2	/* [한국어] 비보안 접근 비트. */
#define DEBUG_TXN_TRIGG_REG		0x18	/* [한국어] ATOS 를 시작시키는 레지스터. */
#define DEBUG_TXN_TRIGGER		BIT(0)	/* [한국어] 이 비트를 쓰는 순간 변환이 시작된다. */
#define DEBUG_VA_ADDR_REG		0x8	/* [한국어] 변환할 주소를 넣는 레지스터. */

static LIST_HEAD(tbu_list);	/* [한국어] 등록된 TBU 를 모으는 전역 목록. */
static DEFINE_MUTEX(tbu_list_lock);	/* [한국어] 그 목록을 지킨다. */
static DEFINE_SPINLOCK(atos_lock);	/* [한국어] ATOS 하나만 동시에 돌게 한다 — 디버그 레지스터가 하나뿐이다. */

/*
 * [한국어] TBU 하나. 스트림 id 무리마다 하나씩 있다.
 *
 * SMMU 본체와 별도의 플랫폼 장치라, 자기 클럭과 연결망 경로를 따로 갖는다.
 * 평소에는 꺼져 있고 ATOS 를 할 때만 켠다.
 */
struct qcom_tbu {
	/* [한국어] 이 TBU 의 플랫폼 장치. 로그와 자원 관리에 쓴다. */
	struct device *dev;
	/* [한국어] 이 TBU 가 속한 SMMU 의 트리 노드.
	 *  설정자: probe 가 qcom,stream-id-range 속성에서 읽는다.
	 *  읽는 자: qcom_find_tbu 가 같은 SMMU 인지 견줄 때. 포인터 비교만 하므로
	 *  참조를 들지 않는다. */
	struct device_node *smmu_np;
	/* [한국어] 맡는 스트림 id 범위. [0]이 시작, [1]이 개수다.
	 *  설정자: probe. 읽는 자: qcom_find_tbu.
	 *  TBU 마다 담당 범위가 나뉘어 있어, 어느 TBU 에 물어야 할지 이것으로 정한다. */
	u32 sid_range[2];
	/* [한국어] 전역 TBU 목록에 매다는 고리.
	 *  TBU 와 SMMU 의 probe 순서를 보장할 수 없어 이렇게 모아 두고 나중에 짝짓는다. */
	struct list_head list;
	/* [한국어] 이 TBU 의 클럭(없을 수 있다).
	 *  ATOS 를 할 때만 켠다 — 평소에 켜 두면 전력만 쓴다. */
	struct clk *clk;
	/* [한국어] 연결망 대역폭 경로.
	 *  ATOS 때 대역폭을 확보하지 않으면 그 접근이 굶을 수 있다. */
	struct icc_path	*path;
	/* [한국어] 디버그 레지스터 창의 주소.
	 *  설정자: probe 가 매핑한다. 읽는 자: 정지·ATOS 함수들. */
	void __iomem *base;
	/* [한국어] (위 영어 주석 참고) 세우기와 풀기를 직렬화한다.
	 *  아래 halt_count 도 이 락이 지킨다. */
	spinlock_t halt_lock; /* multiple halt or resume can't execute concurrently */
	/* [한국어] 이 TBU 를 세워 둔 사용자 수.
	 *  설정자·읽는 자: halt / resume.
	 *  폴트 처리와 ATOS 가 겹칠 수 있어 참조로 센다. */
	int halt_count;
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
 * qcom_smmu_tlb_sync_debug - 무효화가 멈췄을 때 TBU 상태를 찍는다
 *
 * @smmu: 대상 SMMU.
 *
 * 공통 코드의 무효화 대기가 1초를 넘기면 불린다. 그 자체로는 고칠 수
 * 없지만, 어느 TBU 가 응답하지 않는지 알면 원인을 좁힐 수 있다.
 *
 * 세 값이 단서다 — TBU 전원이 꺼졌는지, 무효화를 받았다고 답했는지,
 * 그리고 SMMU 밖에서 대기가 걸려 있는지.
 *
 * SCM 호출로 읽는 것에 주의. 이 레지스터들은 보안 세계가 쥐고 있어
 * 커널이 직접 읽을 수 없다.
 *
 * 속도 제한을 두는 이유: 무효화가 멈추면 그 뒤로도 계속 멈춰, 이 함수가
 * 끝없이 불린다.
 */
void qcom_smmu_tlb_sync_debug(struct arm_smmu_device *smmu)
{
	int ret;	/* [한국어] SCM 호출 결과. */
	u32 tbu_pwr_status, sync_inv_ack, sync_inv_progress;	/* [한국어] 읽어 올 세 단서. */
	struct qcom_smmu *qsmmu = container_of(smmu, struct qcom_smmu, smmu);	/* [한국어] 퀄컴 구조체로 되짚는다. */
	const struct qcom_smmu_config *cfg;	/* [한국어] SoC 별 레지스터 배치. */
	static DEFINE_RATELIMIT_STATE(rs, DEFAULT_RATELIMIT_INTERVAL,	/* [한국어] 무효화가 멈추면 이 함수가 끝없이 불려 속도를 제한한다. */
				      DEFAULT_RATELIMIT_BURST);

	if (__ratelimit(&rs)) {	/* [한국어] 제한을 통과했으면 */
		dev_err(smmu->dev, "TLB sync timed out -- SMMU may be deadlocked\n");	/* [한국어] 먼저 그 사실을 알린다. */

		cfg = qsmmu->data->cfg;	/* [한국어] 이 SoC 의 레지스터 배치. */
		if (!cfg)	/* [한국어] 디버그 레지스터가 없는 SoC 면 */
			return;	/* [한국어] 더 알아낼 것이 없다. */

		ret = qcom_scm_io_readl(smmu->ioaddr + cfg->reg_offset[QCOM_SMMU_TBU_PWR_STATUS],	/* [한국어] TBU 전원 상태. 보안 세계가 쥔 레지스터라 SCM 으로 읽는다. */
					&tbu_pwr_status);
		if (ret)	/* [한국어] 읽지 못했으면 */
			dev_err(smmu->dev,	/* [한국어] 그 사실만 알린다. */
				"Failed to read TBU power status: %d\n", ret);	/* [한국어] 전원이 꺼진 TBU 는 응답하지 않아 무효화가 멈춘 것처럼 보인다. */

		ret = qcom_scm_io_readl(smmu->ioaddr + cfg->reg_offset[QCOM_SMMU_STATS_SYNC_INV_TBU_ACK],	/* [한국어] 각 TBU 가 무효화를 받았다고 답했는지. */
					&sync_inv_ack);
		if (ret)	/* [한국어] 실패하면 그 사실만 알리고 나머지 값은 계속 읽는다. */
			dev_err(smmu->dev,
				"Failed to read TBU sync/inv ack status: %d\n", ret);	/* [한국어] 어느 TBU 가 응답하지 않는지 가려낼 수 있다. */

		ret = qcom_scm_io_readl(smmu->ioaddr + cfg->reg_offset[QCOM_SMMU_MMU2QSS_AND_SAFE_WAIT_CNTR],	/* [한국어] SMMU 와 시스템 사이의 대기 카운터. */
					&sync_inv_progress);
		if (ret)	/* [한국어] 실패하면 그 사실만 알리고 나머지 값은 계속 읽는다. */
			dev_err(smmu->dev,
				"Failed to read TCU syn/inv progress: %d\n", ret);	/* [한국어] 멈춘 원인이 SMMU 안인지 밖인지 가른다. */

		dev_err(smmu->dev,	/* [한국어] 세 값을 함께 찍는다. */
			"TBU: power_status %#x sync_inv_ack %#x sync_inv_progress %#x\n",
			tbu_pwr_status, sync_inv_ack, sync_inv_progress);	/* [한국어] 이것이 원인을 좁히는 단서가 된다. */
	}
}

/*
 * [한국어]
 * qcom_find_tbu - 그 스트림 id 를 담당하는 TBU 를 찾는다
 *
 * @qsmmu: 대상 SMMU.
 * @sid: 찾는 스트림 id.
 * @return: 그 TBU, 없으면 NULL.
 *
 * TBU 마다 담당하는 id 범위가 장치 트리에 적혀 있다. 같은 SMMU 에 속하고
 * 그 범위에 드는 것을 찾는다.
 *
 * 목록이 전역인 이유: TBU 는 SMMU 와 별개의 플랫폼 장치라 순서를 보장할
 * 수 없어, 등록된 것을 모두 모아 두고 나중에 짝을 맞춘다.
 */
static struct qcom_tbu *qcom_find_tbu(struct qcom_smmu *qsmmu, u32 sid)
{
	struct qcom_tbu *tbu;	/* [한국어] 훑을 TBU. */
	u32 start, end;	/* [한국어] 그 TBU 가 맡는 id 범위. */

	guard(mutex)(&tbu_list_lock);	/* [한국어] 목록을 지키는 뮤텍스. 나갈 때 저절로 풀린다. */

	if (list_empty(&tbu_list))	/* [한국어] 등록된 TBU 가 하나도 없으면 */
		return NULL;	/* [한국어] 디버그 기능을 쓸 수 없다. */

	list_for_each_entry(tbu, &tbu_list, list) {	/* [한국어] 모든 TBU 를 훑는다. */
		start = tbu->sid_range[0];	/* [한국어] 맡는 범위의 시작. */
		end = start + tbu->sid_range[1];	/* [한국어] 그 개수를 더한 끝. */

		if (qsmmu->smmu.dev->of_node == tbu->smmu_np &&	/* [한국어] 같은 SMMU 에 속하고 */
		    start <= sid && sid < end)	/* [한국어] 그 범위에 들면 */
			return tbu;	/* [한국어] 이 TBU 가 담당한다. */
	}
	dev_err(qsmmu->smmu.dev, "Unable to find TBU for sid 0x%x\n", sid);	/* [한국어] 장치 트리가 그 id 를 어느 TBU 에도 배정하지 않았다. */

	return NULL;	/* [한국어] 찾지 못했다. */
}

/*
 * [한국어]
 * qcom_tbu_halt - ATOS 를 하기 위해 TBU 를 멈춰 세운다
 *
 * @tbu: 대상 TBU.
 * @smmu_domain: 그 도메인.
 * @return: 0 성공, 시간이 다하면 -ETIMEDOUT.
 *
 * 멈춰 세우지 않으면 진행 중인 트랜잭션과 ATOS 가 뒤섞인다.
 *
 * 폴트 중일 때가 까다롭다. 원 주석대로 앞선 트랜잭션(폴트 자체 포함)이
 * 끝나야 정지 요청이 완료되는데, 폴트는 응답을 기다리며 멈춰 있어 끝나지
 * 않는다. 그래서 폴트 인터럽트를 잠시 끄고 그 트랜잭션을 강제로 끝낸다.
 *
 * 참조를 세는 이유: 폴트 처리와 ATOS 가 겹칠 수 있다.
 */
static int qcom_tbu_halt(struct qcom_tbu *tbu, struct arm_smmu_domain *smmu_domain)
{
	struct arm_smmu_device *smmu = smmu_domain->smmu;	/* [한국어] 그 SMMU. */
	int ret = 0, idx = smmu_domain->cfg.cbndx;	/* [한국어] 결과 코드와 뱅크 번호. */
	u32 val, fsr, status;	/* [한국어] 레지스터 값들. */

	guard(spinlock_irqsave)(&tbu->halt_lock);	/* [한국어] 세우기와 풀기가 겹치면 안 된다. */
	if (tbu->halt_count) {	/* [한국어] 이미 세워져 있으면 */
		tbu->halt_count++;	/* [한국어] 참조만 늘리고 */
		return ret;	/* [한국어] 돌아간다. */
	}

	val = readl_relaxed(tbu->base + DEBUG_SID_HALT_REG);	/* [한국어] 정지 레지스터를 읽어 */
	val |= DEBUG_SID_HALT_VAL;	/* [한국어] 정지 비트를 세우고 */
	writel_relaxed(val, tbu->base + DEBUG_SID_HALT_REG);	/* [한국어] 되쓴다. */

	fsr = arm_smmu_cb_read(smmu, idx, ARM_SMMU_CB_FSR);	/* [한국어] 폴트 상태를 본다. */
	if ((fsr & ARM_SMMU_CB_FSR_FAULT) && (fsr & ARM_SMMU_CB_FSR_SS)) {	/* [한국어] 폴트가 났고 트랜잭션이 멈춰 서 있으면 */
		u32 sctlr_orig, sctlr;	/* [한국어] 되돌릴 원래 값과 고친 값. */

		/*
		 * We are in a fault. Our request to halt the bus will not
		 * complete until transactions in front of us (such as the fault
		 * itself) have completed. Disable iommu faults and terminate
		 * any existing transactions.
		 */
		sctlr_orig = arm_smmu_cb_read(smmu, idx, ARM_SMMU_CB_SCTLR);	/* [한국어] 원래 설정을 기억해 두고 */
		sctlr = sctlr_orig & ~(ARM_SMMU_SCTLR_CFCFG | ARM_SMMU_SCTLR_CFIE);	/* [한국어] 멈춰 세우기와 인터럽트를 끈다. */
		arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_SCTLR, sctlr);	/* [한국어] 그렇게 해야 다음 걸음이 통한다. */
		arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_FSR, fsr);	/* [한국어] 폴트 상태를 지우고 */
		arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_RESUME, ARM_SMMU_RESUME_TERMINATE);	/* [한국어] 멈춘 트랜잭션을 끝낸다 — 원 주석대로 그것이 끝나야 정지 요청이 완료된다. */
		arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_SCTLR, sctlr_orig);	/* [한국어] 설정을 되돌린다. */
	}

	if (readl_poll_timeout_atomic(tbu->base + DEBUG_SR_HALT_ACK_REG, status,	/* [한국어] 정지 확인 비트가 설 때까지 기다린다. */
				      (status & DEBUG_SR_HALT_ACK_VAL),	/* [한국어] 100마이크로초까지. */
				      0, TBU_DBG_TIMEOUT_US)) {
		dev_err(tbu->dev, "Timeout while trying to halt TBU!\n");	/* [한국어] 시간이 다했으면 알리고 */
		ret = -ETIMEDOUT;	/* [한국어] 실패로 표시한다. */

		val = readl_relaxed(tbu->base + DEBUG_SID_HALT_REG);	/* [한국어] 정지 요청을 */
		val &= ~DEBUG_SID_HALT_VAL;	/* [한국어] 거둬들여 */
		writel_relaxed(val, tbu->base + DEBUG_SID_HALT_REG);	/* [한국어] 원래 상태로 되돌린다. */

		return ret;	/* [한국어] 실패를 올린다. */
	}

	tbu->halt_count = 1;	/* [한국어] 첫 참조. */

	return ret;	/* [한국어] 성공. */
}

/*
 * [한국어]
 * qcom_tbu_resume - 멈춰 세운 TBU 를 풀어 준다
 *
 * @tbu: 대상 TBU.
 *
 * 마지막 참조가 사라질 때만 실제로 푼다.
 *
 * 세우지 않았는데 풀라는 것은 짝이 맞지 않는 것이라 경고를 남긴다.
 */
static void qcom_tbu_resume(struct qcom_tbu *tbu)
{
	u32 val;	/* [한국어] 레지스터 값. */

	guard(spinlock_irqsave)(&tbu->halt_lock);	/* [한국어] 세우기와 겹치면 안 된다. */
	if (!tbu->halt_count) {	/* [한국어] 세우지 않았는데 풀라는 것은 */
		WARN(1, "%s: halt_count is 0", dev_name(tbu->dev));	/* [한국어] 짝이 맞지 않는 것이라 경고를 남기고 */
		return;	/* [한국어] 아무것도 하지 않는다. */
	}

	if (tbu->halt_count > 1) {	/* [한국어] 아직 다른 사용자가 있으면 */
		tbu->halt_count--;	/* [한국어] 참조만 줄이고 */
		return;	/* [한국어] 세워 둔 채로 둔다. */
	}

	val = readl_relaxed(tbu->base + DEBUG_SID_HALT_REG);	/* [한국어] 정지 레지스터를 읽어 */
	val &= ~DEBUG_SID_HALT_VAL;	/* [한국어] 정지 비트를 지우고 */
	writel_relaxed(val, tbu->base + DEBUG_SID_HALT_REG);	/* [한국어] 되쓴다. */

	tbu->halt_count = 0;	/* [한국어] 풀렸음을 표시한다. */
}

/*
 * [한국어]
 * qcom_tbu_trigger_atos - 하드웨어에게 주소 변환을 시키고 결과를 읽는다
 *
 * @smmu_domain: 대상 도메인(쓰지 않는다).
 * @tbu: 그 TBU.
 * @iova: 물어볼 주소.
 * @sid: 그 주소를 낼 장치의 스트림 id.
 * @return: 물리 주소, 실패하면 0.
 *
 * 규격의 ATS1PR 과 같은 일을 퀄컴 전용 레지스터로 한다. 스트림 id 까지
 * 지정할 수 있어, 특정 장치가 그 주소를 어떻게 보는지 알 수 있다.
 *
 * 접근 속성을 캐시 가능·비보안·특권으로 정해 두는데, 그 조합이 평범한
 * 커널 DMA 와 가장 가까워 실제 상황을 재현한다.
 *
 * 기다림이 세 갈래로 끝난다 — 완료, 폴트, 시간 초과. 폴트가 났다는 것은
 * 그 주소에 매핑이 없다는 뜻이라, 그것도 유용한 답이다.
 *
 * 마지막에 레지스터를 되돌리는 것이 중요하다. 그러지 않으면 다음 ATOS 가
 * 남은 설정에 영향을 받는다.
 */
static phys_addr_t qcom_tbu_trigger_atos(struct arm_smmu_domain *smmu_domain,
					 struct qcom_tbu *tbu, dma_addr_t iova, u32 sid)
{
	bool atos_timedout = false;	/* [한국어] 시간이 다했는지. */
	phys_addr_t phys = 0;	/* [한국어] 결과. 실패하면 0 그대로. */
	ktime_t timeout;	/* [한국어] 기다릴 시각. */
	u64 val;	/* [한국어] 레지스터 값. */

	/* Set address and stream-id */
	val = readq_relaxed(tbu->base + DEBUG_SID_HALT_REG);	/* [한국어] 정지 레지스터에 스트림 id 자리가 함께 있다. */
	val &= ~DEBUG_SID_HALT_SID;	/* [한국어] 그 자리를 비우고 */
	val |= FIELD_PREP(DEBUG_SID_HALT_SID, sid);	/* [한국어] 물어볼 장치의 id 를 넣는다. */
	writeq_relaxed(val, tbu->base + DEBUG_SID_HALT_REG);	/* [한국어] 되쓴다. */
	writeq_relaxed(iova, tbu->base + DEBUG_VA_ADDR_REG);	/* [한국어] 변환할 주소. */
	val = FIELD_PREP(DEBUG_AXUSER_CDMID, DEBUG_AXUSER_CDMID_VAL);	/* [한국어] 버스가 요구하는 부가 식별자. 정해진 값을 그대로 쓴다. */
	writeq_relaxed(val, tbu->base + DEBUG_AXUSER_REG);	/* [한국어] 그 레지스터에 쓴다. */

	/* Write-back read and write-allocate */
	val = FIELD_PREP(DEBUG_TXN_AXCACHE, 0xf);	/* [한국어] 캐시 속성을 되쓰기·할당으로 — 평범한 커널 DMA 와 가장 가깝다. */

	/* Non-secure access */
	val |= FIELD_PREP(DEBUG_TXN_AXPROT, DEBUG_TXN_AXPROT_NSEC);	/* [한국어] 비보안 접근. */

	/* Privileged access */
	val |= FIELD_PREP(DEBUG_TXN_AXPROT, DEBUG_TXN_AXPROT_PRIV);	/* [한국어] 특권 접근. 두 비트가 같은 필드 안에 있어 OR 로 합쳐진다. */

	val |= DEBUG_TXN_TRIGGER;	/* [한국어] 이 비트가 변환을 시작시킨다. */
	writeq_relaxed(val, tbu->base + DEBUG_TXN_TRIGG_REG);	/* [한국어] 쓰는 순간 하드웨어가 변환을 시작한다. */

	timeout = ktime_add_us(ktime_get(), TBU_DBG_TIMEOUT_US);	/* [한국어] 100마이크로초 뒤의 시각. */
	for (;;) {	/* [한국어] 세 갈래로 끝난다. */
		val = readl_relaxed(tbu->base + DEBUG_SR_HALT_ACK_REG);	/* [한국어] 진행 상태를 읽어 */
		if (!(val & DEBUG_SR_ECATS_RUNNING_VAL))	/* [한국어] 끝났으면 */
			break;	/* [한국어] 고리를 나간다. */
		val = readl_relaxed(tbu->base + DEBUG_PAR_REG);	/* [한국어] 결과 레지스터도 본다. */
		if (val & DEBUG_PAR_FAULT_VAL)	/* [한국어] 폴트가 났으면 */
			break;	/* [한국어] 그것도 끝난 것이다. */
		if (ktime_compare(ktime_get(), timeout) > 0) {	/* [한국어] 시간이 다했으면 */
			atos_timedout = true;	/* [한국어] 표시하고 */
			break;	/* [한국어] 나간다. */
		}
	}

	val = readq_relaxed(tbu->base + DEBUG_PAR_REG);	/* [한국어] 결과를 64비트로 읽는다. */
	if (val & DEBUG_PAR_FAULT_VAL)	/* [한국어] 폴트였으면 */
		dev_err(tbu->dev, "ATOS generated a fault interrupt! PAR = %llx, SID=0x%x\n",	/* [한국어] 그 주소에 매핑이 없다는 뜻이다. 그것도 유용한 답이다. */
			val, sid);
	else if (atos_timedout)	/* [한국어] 시간이 다했으면 */
		dev_err_ratelimited(tbu->dev, "ATOS translation timed out!\n");	/* [한국어] 하드웨어가 응답하지 않았다. */
	else
		phys = FIELD_GET(DEBUG_PAR_PA, val);	/* [한국어] 성공했으면 물리 주소를 꺼낸다. */

	/* Reset hardware */
	writeq_relaxed(0, tbu->base + DEBUG_TXN_TRIGG_REG);	/* [한국어] 다음 ATOS 가 남은 설정에 영향받지 않게 */
	writeq_relaxed(0, tbu->base + DEBUG_VA_ADDR_REG);	/* [한국어] 레지스터를 되돌린다. */
	val = readl_relaxed(tbu->base + DEBUG_SID_HALT_REG);	/* [한국어] 정지 레지스터의 id 자리도 */
	val &= ~DEBUG_SID_HALT_SID;	/* [한국어] 비우고 */
	writel_relaxed(val, tbu->base + DEBUG_SID_HALT_REG);	/* [한국어] 되쓴다. 정지 비트는 그대로 둔다. */

	return phys;	/* [한국어] 결과. */
}

/*
 * [한국어]
 * qcom_iova_to_phys - TBU 를 켜고 멈춰 세운 뒤 ATOS 를 한다
 *
 * @smmu_domain: 대상 도메인.
 * @iova: 물어볼 주소.
 * @sid: 그 장치의 스트림 id.
 * @return: 물리 주소, 실패하면 0.
 *
 * ATOS 하나를 위해 준비할 것이 많다. 연결망 대역폭을 확보하고, 클럭을
 * 켜고, TBU 를 멈춰 세우고, 폴트 인터럽트를 잠시 끈다.
 *
 * 원 주석대로 ATOS 자체가 폴트 인터럽트를 일으킬 수 있어, 그것을 끄고
 * 상태를 손수 확인한다.
 *
 * 세 번까지 다시 시도하는 것이 눈에 띈다. 첫 시도가 실패해도 두 번째에
 * 성공하는 경우가 있다 — 하드웨어의 어떤 상태 때문으로 보인다.
 *
 * 전역 스핀락으로 ATOS 하나만 동시에 돌게 한다. TBU 의 디버그 레지스터가
 * 하나뿐이라 겹치면 결과가 뒤섞인다.
 *
 * 마지막의 읽기는 앞선 쓰기들이 실제로 하드웨어에 닿았음을 보장한다.
 */
static phys_addr_t qcom_iova_to_phys(struct arm_smmu_domain *smmu_domain,
				     dma_addr_t iova, u32 sid)
{
	struct arm_smmu_device *smmu = smmu_domain->smmu;	/* [한국어] 그 SMMU. */
	struct qcom_smmu *qsmmu = to_qcom_smmu(smmu);	/* [한국어] 퀄컴 구조체. */
	int idx = smmu_domain->cfg.cbndx;	/* [한국어] 뱅크 번호. */
	struct qcom_tbu *tbu;	/* [한국어] 담당 TBU. */
	u32 sctlr_orig, sctlr;	/* [한국어] 되돌릴 원래 설정과 고친 설정. */
	phys_addr_t phys = 0;	/* [한국어] 결과. 실패하면 0. */
	int attempt = 0;	/* [한국어] 다시 시도한 횟수. */
	int ret;	/* [한국어] 결과 코드. */
	u64 fsr;	/* [한국어] 폴트 상태. */

	tbu = qcom_find_tbu(qsmmu, sid);	/* [한국어] 그 스트림 id 를 맡는 TBU 를 찾는다. */
	if (!tbu)	/* [한국어] 없으면 */
		return 0;	/* [한국어] ATOS 를 할 수 없다. */

	ret = icc_set_bw(tbu->path, 0, UINT_MAX);	/* [한국어] 연결망 대역폭을 확보한다. 그러지 않으면 TBU 접근이 굶을 수 있다. */
	if (ret)	/* [한국어] 실패하면 */
		return ret;	/* [한국어] 포기한다. */

	ret = clk_prepare_enable(tbu->clk);	/* [한국어] TBU 클럭을 켠다. 평소에는 꺼 두어 전력을 아낀다. */
	if (ret)	/* [한국어] 실패하면 그 사실만 알리고 나머지 값은 계속 읽는다. */
		goto disable_icc;	/* [한국어] 실패하면 대역폭을 되돌린다. */

	ret = qcom_tbu_halt(tbu, smmu_domain);	/* [한국어] ATOS 와 진행 중인 트랜잭션이 뒤섞이지 않게 세운다. */
	if (ret)	/* [한국어] 실패하면 그 사실만 알리고 나머지 값은 계속 읽는다. */
		goto disable_clk;	/* [한국어] 실패하면 클럭을 끈다. */

	/*
	 * ATOS/ECATS can trigger the fault interrupt, so disable it temporarily
	 * and check for an interrupt manually.
	 */
	sctlr_orig = arm_smmu_cb_read(smmu, idx, ARM_SMMU_CB_SCTLR);	/* [한국어] 원 주석대로 ATOS 자체가 폴트 인터럽트를 일으킬 수 있어 */
	sctlr = sctlr_orig & ~(ARM_SMMU_SCTLR_CFCFG | ARM_SMMU_SCTLR_CFIE);	/* [한국어] 그것을 잠시 끄고 */
	arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_SCTLR, sctlr);	/* [한국어] 상태를 손수 확인한다. */

	fsr = arm_smmu_cb_read(smmu, idx, ARM_SMMU_CB_FSR);	/* [한국어] 쌓인 폴트가 있는지 본다. */
	if (fsr & ARM_SMMU_CB_FSR_FAULT) {	/* [한국어] 있으면 */
		/* Clear pending interrupts */
		arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_FSR, fsr);	/* [한국어] 지운다 — ATOS 결과와 섞이면 안 된다. */

		/*
		 * TBU halt takes care of resuming any stalled transcation.
		 * Kept it here for completeness sake.
		 */
		if (fsr & ARM_SMMU_CB_FSR_SS)	/* [한국어] 멈춰 선 트랜잭션이 있으면 */
			arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_RESUME,	/* [한국어] 끝낸다. 원 주석대로 정지가 이미 처리하지만 완결성을 위해 남겼다. */
					  ARM_SMMU_RESUME_TERMINATE);
	}

	/* Only one concurrent atos operation */
	scoped_guard(spinlock_irqsave, &atos_lock) {	/* [한국어] TBU 의 디버그 레지스터가 하나뿐이라 ATOS 하나만 동시에 돈다. */
		/*
		 * If the translation fails, attempt the lookup more time."
		 */
		do {	/* [한국어] 원 주석대로 실패하면 몇 번 더 시도한다. */
			phys = qcom_tbu_trigger_atos(smmu_domain, tbu, iova, sid);	/* [한국어] 하드웨어에게 변환을 시킨다. */

			fsr = arm_smmu_cb_read(smmu, idx, ARM_SMMU_CB_FSR);	/* [한국어] 그 과정에서 난 폴트를 본다. */
			if (fsr & ARM_SMMU_CB_FSR_FAULT) {	/* [한국어] 났으면 */
				/* Clear pending interrupts */
				arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_FSR, fsr);	/* [한국어] 지우고 */

				if (fsr & ARM_SMMU_CB_FSR_SS)	/* [한국어] 멈춰 선 것이 있으면 */
					arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_RESUME,	/* [한국어] 끝낸다. */
							  ARM_SMMU_RESUME_TERMINATE);
			}
		} while (!phys && attempt++ < 2);	/* [한국어] 세 번까지. 첫 시도가 실패해도 다음에 성공하는 경우가 있다. */

		arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_SCTLR, sctlr_orig);	/* [한국어] 폴트 인터럽트 설정을 되돌린다. */
	}
	qcom_tbu_resume(tbu);	/* [한국어] TBU 를 풀어 준다. */

	/* Read to complete prior write transcations */
	readl_relaxed(tbu->base + DEBUG_SR_HALT_ACK_REG);	/* [한국어] 원 주석대로 앞선 쓰기들이 실제로 하드웨어에 닿았음을 보장한다. */

disable_clk:	/* [한국어] 정지 실패가 합류한다. */
	clk_disable_unprepare(tbu->clk);	/* [한국어] 클럭을 끈다. */
disable_icc:	/* [한국어] 클럭 실패도 합류한다. */
	icc_set_bw(tbu->path, 0, 0);	/* [한국어] 대역폭 확보를 되돌린다. */

	return phys;	/* [한국어] 결과. */
}

/*
 * [한국어]
 * qcom_smmu_iova_to_phys_hard - 폴트를 낸 장치의 눈으로 주소를 변환한다
 *
 * @smmu_domain: 대상 도메인.
 * @iova: 물어볼 주소.
 * @return: 물리 주소, 실패하면 0.
 *
 * 오류 레지스터에서 스트림 id 를 꺼내 그 장치 기준으로 물어본다.
 * 같은 IOVA 도 장치마다 다르게 변환될 수 있어 이 과정이 필요하다.
 */
static phys_addr_t qcom_smmu_iova_to_phys_hard(struct arm_smmu_domain *smmu_domain, dma_addr_t iova)
{
	struct arm_smmu_device *smmu = smmu_domain->smmu;	/* [한국어] 그 SMMU. */
	int idx = smmu_domain->cfg.cbndx;	/* [한국어] 뱅크 번호. */
	u32 frsynra;	/* [한국어] 오류 부가 정보. */
	u16 sid;	/* [한국어] 폴트를 낸 장치의 스트림 id. */

	frsynra = arm_smmu_gr1_read(smmu, ARM_SMMU_GR1_CBFRSYNRA(idx));	/* [한국어] 그 정보를 읽어 */
	sid = FIELD_GET(ARM_SMMU_CBFRSYNRA_SID, frsynra);	/* [한국어] 스트림 id 를 꺼낸다. */

	return qcom_iova_to_phys(smmu_domain, iova, sid);	/* [한국어] 그 장치 기준으로 물어본다 — 같은 IOVA 도 장치마다 다르게 변환된다. */
}

/*
 * [한국어]
 * qcom_smmu_verify_fault - TLB 를 비운 전후로 두 번 물어 값을 견준다
 *
 * @smmu_domain: 대상 도메인.
 * @iova: 폴트가 난 주소.
 * @fsr: 오류 상태(쓰지 않는다).
 * @return: 물리 주소. 앞이 0 이면 뒤의 값을 준다.
 *
 * 두 값이 다르면 TLB 에 낡은 항목이 남아 있었다는 뜻이다 — 무효화를
 * 빠뜨린 버그를 잡아내는 방법이다.
 *
 * 앞이 0 이고 뒤가 유효하면, 표에는 매핑이 있는데 TLB 가 그것을 보지
 * 못한 것이다. 그 사실 자체가 원인을 말해 준다.
 */
static phys_addr_t qcom_smmu_verify_fault(struct arm_smmu_domain *smmu_domain, dma_addr_t iova, u32 fsr)
{
	struct io_pgtable *iop = io_pgtable_ops_to_pgtable(smmu_domain->pgtbl_ops);	/* [한국어] TLB 를 비우려면 표 객체가 필요하다. */
	struct arm_smmu_device *smmu = smmu_domain->smmu;	/* [한국어] 그 SMMU. */
	phys_addr_t phys_post_tlbiall;	/* [한국어] 비운 뒤의 결과. */
	phys_addr_t phys;	/* [한국어] 비우기 전의 결과. */

	phys = qcom_smmu_iova_to_phys_hard(smmu_domain, iova);	/* [한국어] 먼저 지금 상태로 물어보고 */
	io_pgtable_tlb_flush_all(iop);	/* [한국어] TLB 를 통째로 비운 뒤 */
	phys_post_tlbiall = qcom_smmu_iova_to_phys_hard(smmu_domain, iova);	/* [한국어] 다시 물어본다. */

	if (phys != phys_post_tlbiall) {	/* [한국어] 두 값이 다르면 */
		dev_err(smmu->dev,	/* [한국어] TLB 에 낡은 항목이 남아 있었다는 뜻이다 — 무효화를 빠뜨린 버그를 잡아낸다. */
			"ATOS results differed across TLBIALL... (before: %pa after: %pa)\n",
			&phys, &phys_post_tlbiall);	/* [한국어] 두 값을 함께 남긴다. */
	}

	return (phys == 0 ? phys_post_tlbiall : phys);	/* [한국어] 앞이 0 이면 뒤의 값을 준다. 그 자체가 "표에는 있는데 TLB 가 못 봤다"는 신호다. */
}

/*
 * [한국어]
 * qcom_smmu_context_fault - 퀄컴 전용 문맥 오류 처리기
 *
 * @irq: 인터럽트 번호(쓰지 않는다).
 * @dev: 등록할 때 넘긴 도메인.
 * @return: IRQ_HANDLED, IRQ_NONE 중 하나.
 *
 * TBU 가 없으면 공통 판과 똑같이 동작한다 — 그것이 앞쪽 갈래다.
 *
 * TBU 가 있으면 소프트웨어 표 순회와 하드웨어 ATOS 를 모두 해 함께 찍는다.
 * 둘을 견주면 원인이 드러난다: 소프트웨어도 0 이면 매핑 자체가 없는 것이고,
 * 소프트웨어는 찾는데 하드웨어가 못 찾으면 TLB 문제다.
 *
 * -EBUSY 처리가 이 함수의 핵심이다. 원 주석이 그 절차를 자세히 적어
 * 두었다 — 클라이언트가 디버깅을 마칠 때까지 폴트 상태를 그대로 두어야
 * 트랜잭션이 멈춘 채로 남는다. FSR 을 클라이언트가 마지막에 지워야
 * SCTLR.HUPCF 가 뜻대로 동작한다는 설명까지 붙어 있다.
 *
 * 실행 컨텍스트: 스레드 인터럽트. ATOS 가 클럭을 켜고 잠들 수 있어
 * 보통 인터럽트로는 쓸 수 없다.
 */
irqreturn_t qcom_smmu_context_fault(int irq, void *dev)
{
	struct arm_smmu_domain *smmu_domain = dev;	/* [한국어] 등록할 때 넘긴 도메인. */
	struct io_pgtable_ops *ops = smmu_domain->pgtbl_ops;	/* [한국어] 소프트웨어 표 순회에 쓴다. */
	struct arm_smmu_device *smmu = smmu_domain->smmu;	/* [한국어] 그 SMMU. */
	struct arm_smmu_context_fault_info cfi;	/* [한국어] 읽어 둘 오류 정보. */
	u32 resume = 0;	/* [한국어] 멈춘 트랜잭션을 어떻게 할지. */
	int idx = smmu_domain->cfg.cbndx;	/* [한국어] 뱅크 번호. */
	phys_addr_t phys_soft;	/* [한국어] 소프트웨어 표 순회 결과. */
	int ret, tmp;	/* [한국어] 반환값과 상위 계층의 응답. */

	static DEFINE_RATELIMIT_STATE(_rs,	/* [한국어] 로그 속도 제한. */
				      DEFAULT_RATELIMIT_INTERVAL,
				      DEFAULT_RATELIMIT_BURST);

	arm_smmu_read_context_fault_info(smmu, idx, &cfi);	/* [한국어] 레지스터들을 한 번에 떠 둔다. */

	if (!(cfi.fsr & ARM_SMMU_CB_FSR_FAULT))	/* [한국어] 오류 비트가 없으면 */
		return IRQ_NONE;	/* [한국어] 우리 것이 아니다. */

	if (list_empty(&tbu_list)) {	/* [한국어] TBU 가 없는 SoC 면 */
		ret = report_iommu_fault(&smmu_domain->domain, NULL, cfi.iova,	/* [한국어] 공통 판과 똑같이 동작한다. */
					 cfi.fsynr & ARM_SMMU_CB_FSYNR0_WNR ? IOMMU_FAULT_WRITE : IOMMU_FAULT_READ);	/* [한국어] 방향을 함께 알린다. */

		if (ret == -ENOSYS)	/* [한국어] 아무도 다루지 않으면 */
			arm_smmu_print_context_fault_info(smmu, idx, &cfi);	/* [한국어] 로그로 찍고 */

		arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_FSR, cfi.fsr);	/* [한국어] 오류 상태를 지운다. */

		if (cfi.fsr & ARM_SMMU_CB_FSR_SS) {	/* [한국어] 멈춰 선 트랜잭션이 있으면 */
			arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_RESUME,	/* [한국어] 풀어 준다. */
					  ret == -EAGAIN ? 0 : ARM_SMMU_RESUME_TERMINATE);	/* [한국어] 다시 시도하거나 끝낸다. */
		}

		return IRQ_HANDLED;	/* [한국어] 처리했다. */
	}

	phys_soft = ops->iova_to_phys(ops, cfi.iova);	/* [한국어] 표에는 매핑이 있는지 먼저 본다. */

	tmp = report_iommu_fault(&smmu_domain->domain, NULL, cfi.iova,	/* [한국어] 상위 계층에 알려 본다. */
				 cfi.fsynr & ARM_SMMU_CB_FSYNR0_WNR ? IOMMU_FAULT_WRITE : IOMMU_FAULT_READ);
	if (!tmp || tmp == -EBUSY) {	/* [한국어] 처리했거나 디버깅 중이면 */
		ret = IRQ_HANDLED;	/* [한국어] 우리 인터럽트였다. */
		resume = ARM_SMMU_RESUME_TERMINATE;	/* [한국어] 그 트랜잭션은 끝낸다. */
	} else if (tmp == -EAGAIN) {	/* [한국어] 다시 시도하라면 */
		ret = IRQ_HANDLED;	/* [한국어] 처리한 것으로 보고 */
		resume = 0;	/* [한국어] 트랜잭션을 되살린다. */
	} else {
		phys_addr_t phys_atos = qcom_smmu_verify_fault(smmu_domain, cfi.iova, cfi.fsr);	/* [한국어] 아무도 다루지 않으면 하드웨어에게도 물어본다. */

		if (__ratelimit(&_rs)) {	/* [한국어] 속도 제한을 통과했으면 */
			arm_smmu_print_context_fault_info(smmu, idx, &cfi);	/* [한국어] 오류 내용을 찍고 */

			dev_err(smmu->dev,	/* [한국어] 표 자체에 매핑이 없다 — 장치가 매핑하지 않은 주소를 건드린 것이다. */
				"soft iova-to-phys=%pa\n", &phys_soft);	/* [한국어] 소프트웨어 순회 결과도 찍는다. */
			if (!phys_soft)	/* [한국어] 그것도 0 이면 */
				dev_err(smmu->dev,	/* [한국어] 표 자체에 매핑이 없다는 뜻이다 — 가장 흔한 원인이다. */
					"SOFTWARE TABLE WALK FAILED! Looks like %s accessed an unmapped address!\n",
					dev_name(smmu->dev));
			if (phys_atos)	/* [한국어] 하드웨어가 답했으면 */
				dev_err(smmu->dev, "hard iova-to-phys (ATOS)=%pa\n",	/* [한국어] 그 값도 찍는다. 소프트웨어 값과 다르면 TLB 문제다. */
					&phys_atos);
			else
				dev_err(smmu->dev, "hard iova-to-phys (ATOS) failed\n");	/* [한국어] 답하지 못했으면 그 사실을 남긴다. */
		}
		ret = IRQ_NONE;	/* [한국어] 아무도 다루지 못했다. */
		resume = ARM_SMMU_RESUME_TERMINATE;	/* [한국어] 그래도 트랜잭션은 끝내야 한다. */
	}

	/*
	 * If the client returns -EBUSY, do not clear FSR and do not RESUME
	 * if stalled. This is required to keep the IOMMU client stalled on
	 * the outstanding fault. This gives the client a chance to take any
	 * debug action and then terminate the stalled transaction.
	 * So, the sequence in case of stall on fault should be:
	 * 1) Do not clear FSR or write to RESUME here
	 * 2) Client takes any debug action
	 * 3) Client terminates the stalled transaction and resumes the IOMMU
	 * 4) Client clears FSR. The FSR should only be cleared after 3) and
	 *    not before so that the fault remains outstanding. This ensures
	 *    SCTLR.HUPCF has the desired effect if subsequent transactions also
	 *    need to be terminated.
	 */
	if (tmp != -EBUSY) {	/* [한국어] 원 주석대로 -EBUSY 면 아무것도 건드리지 않는다. */
		/* Clear the faulting FSR */
		arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_FSR, cfi.fsr);	/* [한국어] 오류 상태를 지우고 */

		/* Retry or terminate any stalled transactions */
		if (cfi.fsr & ARM_SMMU_CB_FSR_SS)	/* [한국어] 멈춰 선 것이 있으면 */
			arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_RESUME, resume);	/* [한국어] 위에서 정한 대로 처리한다. */
	}

	return ret;	/* [한국어] 결과. */
}

/*
 * [한국어]
 * qcom_tbu_probe - 장치 트리의 TBU 노드를 잡는다
 *
 * @pdev: 그 플랫폼 장치.
 * @return: 0 성공, 음수면 실패.
 *
 * TBU 는 SMMU 와 별개의 노드라 따로 잡아 전역 목록에 넣는다. 나중에
 * ATOS 가 필요할 때 스트림 id 로 짝을 찾는다.
 *
 * 클럭과 연결망 경로를 미리 얻어 두되 켜지는 않는다 — ATOS 를 할 때만
 * 켜서 평소 전력을 아낀다.
 */
int qcom_tbu_probe(struct platform_device *pdev)
{
	struct of_phandle_args args = { .args_count = 2 };	/* [한국어] 스트림 id 범위는 두 인자(시작, 개수)로 온다. */
	struct device_node *np = pdev->dev.of_node;	/* [한국어] 이 TBU 의 트리 노드. */
	struct device *dev = &pdev->dev;	/* [한국어] 그 장치. */
	struct qcom_tbu *tbu;	/* [한국어] 만들 구조체. */

	tbu = devm_kzalloc(dev, sizeof(*tbu), GFP_KERNEL);	/* [한국어] 구조체를 잡는다. */
	if (!tbu)	/* [한국어] 메모리가 없다. */
		return -ENOMEM;	/* [한국어] 실패. */

	tbu->dev = dev;	/* [한국어] 장치를 기억한다. */
	INIT_LIST_HEAD(&tbu->list);	/* [한국어] 목록 고리를 비운 상태로. */
	spin_lock_init(&tbu->halt_lock);	/* [한국어] 세우기와 풀기를 직렬화할 락. */

	if (of_parse_phandle_with_args(np, "qcom,stream-id-range", "#iommu-cells", 0, &args)) {	/* [한국어] 어느 SMMU 의 어느 id 범위를 맡는지 읽는다. */
		dev_err(dev, "Cannot parse the 'qcom,stream-id-range' DT property\n");	/* [한국어] 없으면 이 TBU 를 쓸 수 없다. */
		return -EINVAL;	/* [한국어] 포기한다. */
	}

	tbu->smmu_np =  args.np;	/* [한국어] 속한 SMMU 의 노드. */
	tbu->sid_range[0] = args.args[0];	/* [한국어] 맡는 범위의 시작. */
	tbu->sid_range[1] = args.args[1];	/* [한국어] 그 개수. */
	of_node_put(args.np);	/* [한국어] 노드 포인터는 비교에만 쓰므로 참조를 놓는다. */

	tbu->base = devm_of_iomap(dev, np, 0, NULL);	/* [한국어] 디버그 레지스터 창을 매핑한다. */
	if (IS_ERR(tbu->base))	/* [한국어] 실패하면 */
		return PTR_ERR(tbu->base);	/* [한국어] 포기한다. */

	tbu->clk = devm_clk_get_optional(dev, NULL);	/* [한국어] 클럭은 없을 수도 있어 optional 판을 쓴다. */
	if (IS_ERR(tbu->clk))	/* [한국어] 오류면 */
		return PTR_ERR(tbu->clk);	/* [한국어] 포기한다. */

	tbu->path = devm_of_icc_get(dev, NULL);	/* [한국어] 연결망 경로. ATOS 때 대역폭을 확보하는 데 쓴다. */
	if (IS_ERR(tbu->path))	/* [한국어] 오류면 */
		return PTR_ERR(tbu->path);	/* [한국어] 포기한다. */

	guard(mutex)(&tbu_list_lock);	/* [한국어] 목록을 지키는 뮤텍스. */
	list_add_tail(&tbu->list, &tbu_list);	/* [한국어] 전역 목록에 넣는다. SMMU 와 순서를 보장할 수 없어 이렇게 모아 둔다. */

	return 0;	/* [한국어] 성공. */
}
