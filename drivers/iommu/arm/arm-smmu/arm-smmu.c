// SPDX-License-Identifier: GPL-2.0-only
/*
 * IOMMU API for ARM architected SMMU implementations.
 *
 * Copyright (C) 2013 ARM Limited
 *
 * Author: Will Deacon <will.deacon@arm.com>
 *
 * This driver currently supports:
 *	- SMMUv1 and v2 implementations
 *	- Stream-matching and stream-indexing
 *	- v7/v8 long-descriptor format
 *	- Non-secure access to the SMMU
 *	- Context fault reporting
 *	- Extended Stream ID (16 bit)
 */

/*
 * [한국어 설명] ARM SMMU v1/v2 드라이버 본체 (arm-smmu.c)
 *
 * === 파일의 역할 ===
 * 위 영어 주석이 지원 범위를 밝혀 두었다. SMMU 1판과 2판, 스트림 매칭과
 * 인덱싱, v7/v8 긴 서술자 형식, 비보안 접근, 문맥 오류 보고, 확장
 * 스트림 id 다.
 *
 * 하는 일은 세 갈래로 나뉜다.
 *
 * 첫째, 하드웨어를 알아내고 세운다. 능력 레지스터를 읽어 뱅크 수와 주소
 * 폭과 지원 형식을 파악하고, 그에 맞춰 자원 배열을 잡는다.
 *
 * 둘째, 장치를 주소 공간에 잇는다. 장치가 쓰는 스트림 id 마다 매칭 항목
 * (SMR)을 배정하고, 그것이 가리키는 S2CR 을 도메인의 컨텍스트 뱅크로
 * 향하게 한다. 같은 항목에 걸리는 장치들은 서로 구별되지 않으므로 반드시
 * 한 그룹이 된다.
 *
 * 셋째, 매핑과 무효화를 한다. 표 자체는 io-pgtable 이 만들고, 이 파일은
 * 그것을 하드웨어에 걸고 TLB 를 비우는 일을 맡는다.
 *
 * TLB 무효화 함수가 여럿인 것이 이 파일의 특징이다. 1단계와 2단계가
 * 다르고, 2단계는 다시 규격 판에 따라 다르다. 주소 단위 무효화가 없는
 * 옛 하드웨어에서는 VMID 통째로 비우는 수밖에 없다.
 *
 * 전원 관리도 얽혀 있다. 어떤 SoC 는 SMMU 를 꺼 두었다가 필요할 때만
 * 켜는데, 매핑 하나마다 켜고 끄면 몹시 느려진다. autosuspend 지연을 두어
 * 그것을 막는다.
 *
 * 옛 장치 트리 결합(mmu-masters)을 다루는 코드가 앞부분에 있다. 지금은
 * iommus 속성을 쓰지만, 옛 트리를 쓰는 기계를 위해 남겨 두었다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 장치 드라이버의 DMA API → iommu 코어 → arm_smmu_ops
 *   → 이 파일 → io-pgtable(ARM LPAE) → 페이지 테이블
 *   → 이 파일의 TLB 무효화 → SMMU 하드웨어
 *
 * probe: 플랫폼 버스 → arm_smmu_device_probe → 능력 조사 → 자원 배정
 *   → iommu 코어에 등록
 *
 * 실행 컨텍스트: 대부분 프로세스 문맥. 오류 처리기만 인터럽트 문맥이고,
 * 무효화 경로는 스핀락 아래에서 돈다.
 *
 * === 타 모듈과의 연결 ===
 * 위: iommu 코어와 dma-iommu.
 * 옆: arm-smmu-impl.c 가 구현체별 갈고리를 달아 준다.
 * 아래: io-pgtable 의 ARM LPAE 구현, 그리고 MMIO.
 *
 * === 주요 함수/구조체 요약 ===
 * arm_smmu_device_probe: 하드웨어를 찾아 세우고 코어에 등록한다.
 * arm_smmu_device_cfg_probe: 능력 레지스터를 읽어 성질을 알아낸다.
 * arm_smmu_init_domain_context: 도메인에 컨텍스트 뱅크를 배정하고
 *   페이지 테이블을 만든다.
 * arm_smmu_write_context_bank: 뱅크 설정을 하드웨어에 실제로 쓴다.
 * arm_smmu_master_alloc_smes: 장치의 스트림 id 에 매칭 항목을 배정한다.
 * arm_smmu_attach_dev: 그 항목들을 도메인의 뱅크로 향하게 한다.
 * arm_smmu_tlb_* : 단계와 판에 따라 갈라지는 TLB 무효화 함수들.
 * arm_smmu_iova_to_phys_hard: 하드웨어에게 주소 변환을 직접 물어본다.
 */
#define pr_fmt(fmt) "arm-smmu: " fmt

#include <linux/acpi.h>	/* [한국어] ACPI 표에서 SMMU 정보를 읽는다. */
#include <linux/acpi_iort.h>	/* [한국어] IORT 표 — ACPI 시스템의 IOMMU 배치를 담는다. */
#include <linux/bitfield.h>	/* [한국어] FIELD_PREP / FIELD_GET — 레지스터 필드를 다룬다. */
#include <linux/delay.h>	/* [한국어] udelay — 무효화 완료를 기다린다. */
#include <linux/dma-mapping.h>	/* [한국어] 표 순회기의 DMA 마스크를 설정한다. */
#include <linux/err.h>	/* [한국어] 오류 포인터 관용구. */
#include <linux/interrupt.h>	/* [한국어] 오류 인터럽트를 걸고 뗀다. */
#include <linux/io.h>	/* [한국어] MMIO 접근. */
#include <linux/iopoll.h>	/* [한국어] readl_poll_timeout — 주소 변환 완료를 기다린다. */
#include <linux/module.h>	/* [한국어] 모듈 파라미터와 메타데이터. */
#include <linux/of.h>	/* [한국어] 장치 트리 속성. */
#include <linux/of_address.h>	/* [한국어] 노드에서 주소를 읽는다. */
#include <linux/pci.h>	/* [한국어] PCI 별칭 순회 — 옛 결합의 스트림 id 를 구한다. */
#include <linux/platform_device.h>	/* [한국어] 플랫폼 드라이버 등록. */
#include <linux/pm_runtime.h>	/* [한국어] 전원 관리. 어떤 SoC 는 SMMU 를 꺼 둔다. */
#include <linux/ratelimit.h>	/* [한국어] 오류 로그의 속도 제한. 잘못된 장치는 초당 수천 번 오류를 낸다. */
#include <linux/slab.h>	/* [한국어] kzalloc / kfree. */
#include <linux/string_choices.h>	/* [한국어] str_plural — 로그 문구의 단수·복수를 맞춘다. */

#include <linux/fsl/mc.h>	/* [한국어] Freescale MC 버스 장치의 그룹 규칙. */

#include "arm-smmu.h"	/* [한국어] 레지스터 정의와 공통 자료 구조. */
#include "../../dma-iommu.h"	/* [한국어] dma-iommu 의 예약 구간 목록. */

/*
 * Apparently, some Qualcomm arm64 platforms which appear to expose their SMMU
 * global register space are still, in fact, using a hypervisor to mediate it
 * by trapping and emulating register accesses. Sadly, some deployed versions
 * of said trapping code have bugs wherein they go horribly wrong for stores
 * using r31 (i.e. XZR/WZR) as the source register.
 */
#define QCOM_DUMMY_VAL -1	/* [한국어] 위 주석대로 어떤 퀄컴 하이퍼바이저가 제로 레지스터를 원본으로 쓰는 저장 명령을 잘못 처리한다. 0 이 아닌 값을 쓰면 그 경로를 피한다. */

#define MSI_IOVA_BASE			0x8000000	/* [한국어] MSI doorbell 을 놓을 IOVA. 커널이 정하고 사용자는 그 자리를 쓸 수 없다. */
#define MSI_IOVA_LENGTH			0x100000	/* [한국어] 그 창의 크기(1MB). */

static int force_stage;	/* [한국어] 디버깅용. 특정 변환 단계만 쓰게 강제한다. */
module_param(force_stage, int, S_IRUGO);	/* [한국어] 읽기만 노출한다 — 부팅 뒤에 바꾸면 이미 만들어진 도메인과 어긋난다. */
MODULE_PARM_DESC(force_stage,	/* [한국어] 1 또는 2 만 뜻이 있고, 그 밖의 값은 무시된다. 특정 단계를 강제하면 중첩 변환은 못 쓴다. */
	"Force SMMU mappings to be installed at a particular stage of translation. A value of '1' or '2' forces the corresponding stage. All other values are ignored (i.e. no stage is forced). Note that selecting a specific stage will disable support for nested translation.");
static bool disable_bypass =	/* [한국어] 기본값을 커널 설정에서 가져온다. 배포판마다 보안 방침이 달라 빌드 때 정하게 했다. */
	IS_ENABLED(CONFIG_ARM_SMMU_DISABLE_BYPASS_BY_DEFAULT);
module_param(disable_bypass, bool, S_IRUGO);	/* [한국어] 역시 읽기만 노출한다. */
MODULE_PARM_DESC(disable_bypass,	/* [한국어] 켜면 등록되지 않은 장치의 DMA 가 오류가 되고, 끄면 SMMU 를 그냥 지나간다. */
	"Disable bypass streams such that incoming transactions from devices that are not attached to an iommu domain will report an abort back to the device and will not be allowed to pass through the SMMU.");

/* [한국어] 매핑 항목의 초기 상태를 짓는 매크로.
 * 우회를 막는 설정이면 오류로, 아니면 통과로 시작한다. 매크로인 이유는
 * disable_bypass 가 실행 중에 정해지는 값이라 상수 초기화로 쓸 수 없기
 * 때문이다. */
#define s2cr_init_val (struct arm_smmu_s2cr){				\
	.type = disable_bypass ? S2CR_TYPE_FAULT : S2CR_TYPE_BYPASS,	\
}

static bool using_legacy_binding, using_generic_binding;	/* [한국어] 한 시스템에서 두 결합을 섞을 수 없어, 먼저 본 쪽으로 고정하는 표시다. */

/*
 * [한국어]
 * arm_smmu_rpm_get - SMMU 의 전원을 켜고 참조를 든다
 *
 * @smmu: 대상 SMMU.
 * @return: 0 성공, 음수면 실패.
 *
 * 어떤 SoC 는 SMMU 를 쓰지 않을 때 꺼 둔다. 레지스터를 건드리기 전에
 * 반드시 깨워야 한다.
 *
 * 전원 관리를 쓰지 않는 하드웨어에서는 아무것도 하지 않고 성공한다 —
 * 호출부가 조건을 따지지 않아도 되게 하려는 것이다.
 */
static inline int arm_smmu_rpm_get(struct arm_smmu_device *smmu)
{
	if (pm_runtime_enabled(smmu->dev))	/* [한국어] 전원 관리를 쓰는 하드웨어면 */
		return pm_runtime_resume_and_get(smmu->dev);	/* [한국어] 깨우고 참조를 든다. 실패하면 참조도 들지 않는다. */

	return 0;	/* [한국어] 쓰지 않으면 늘 켜져 있어 할 일이 없다. */
}

/*
 * [한국어]
 * arm_smmu_rpm_put - 전원 참조를 놓는다
 *
 * @smmu: 대상 SMMU.
 *
 * 곧바로 끄지 않고 마지막 사용 시각을 찍은 뒤 자동 유예에 맡긴다.
 * 아래 use_autosuspend 의 주석이 그 이유를 밝힌다.
 */
static inline void arm_smmu_rpm_put(struct arm_smmu_device *smmu)
{
	if (pm_runtime_enabled(smmu->dev)) {	/* [한국어] 전원 관리를 쓰는 하드웨어면 */
		pm_runtime_mark_last_busy(smmu->dev);	/* [한국어] 마지막 사용 시각을 찍어 유예의 기준으로 삼는다. */
		__pm_runtime_put_autosuspend(smmu->dev);	/* [한국어] 곧바로 끄지 않고 유예 뒤에 끄게 맡긴다. */

	}
}

/*
 * [한국어]
 * (아래 영어 주석과 함께 읽을 것)
 * arm_smmu_rpm_use_autosuspend - 전원 상태가 튀지 않게 유예를 둔다
 *
 * @smmu: 대상 SMMU.
 *
 * 원 주석이 든 예가 이 설정의 이유를 잘 보여 준다 — GPU 를 쓰는 게임이
 * 끝나면 버퍼 수백·수천 개가 한꺼번에 풀리는데, 그때마다 SMMU 를 켰다
 * 껐다 하면 뱅크를 다시 프로그래밍하는 데만 5~10초가 걸린다. 사용자에게는
 * 시스템이 멈춘 것처럼 보인다.
 *
 * 20밀리초의 유예를 두면 그 무리가 한 번의 켜짐 안에 끝난다.
 */
static void arm_smmu_rpm_use_autosuspend(struct arm_smmu_device *smmu)
{
	/*
	 * Setup an autosuspend delay to avoid bouncing runpm state.
	 * Otherwise, if a driver for a suspended consumer device
	 * unmaps buffers, it will runpm resume/suspend for each one.
	 *
	 * For example, when used by a GPU device, when an application
	 * or game exits, it can trigger unmapping 100s or 1000s of
	 * buffers.  With a runpm cycle for each buffer, that adds up
	 * to 5-10sec worth of reprogramming the context bank, while
	 * the system appears to be locked up to the user.
	 */
	pm_runtime_set_autosuspend_delay(smmu->dev, 20);	/* [한국어] 20밀리초. 버퍼 해제가 무리로 몰려와도 한 번의 켜짐 안에 끝난다. */
	pm_runtime_use_autosuspend(smmu->dev);	/* [한국어] 유예 방식을 켠다. */
}

/*
 * [한국어]
 * to_smmu_domain - 코어 도메인에서 이 드라이버의 도메인으로 되짚는다
 *
 * @dom: 코어가 준 도메인.
 * @return: 그것을 품은 SMMU 도메인.
 */
static struct arm_smmu_domain *to_smmu_domain(struct iommu_domain *dom)
{
	return container_of(dom, struct arm_smmu_domain, domain);	/* [한국어] 코어 도메인을 품은 구조체로 되짚는다. */
}

static struct platform_driver arm_smmu_driver;	/* [한국어] 아래에서 정의하는 드라이버의 전방 선언. 옛 결합 코드가 그 장치 목록을 훑기 때문이다. */
static const struct iommu_ops arm_smmu_ops;	/* [한국어] 연산표의 전방 선언. 능력 조회가 그 주소를 쓴다. */

#ifdef CONFIG_ARM_SMMU_LEGACY_DT_BINDINGS	/* [한국어] 옛 결합 지원은 설정으로 뺄 수 있다 — 새 트리만 쓰는 시스템에서는 필요 없다. */
/*
 * [한국어]
 * dev_get_dev_node - 이 장치를 대표하는 장치 트리 노드를 얻는다
 *
 * @dev: 대상 장치.
 * @return: 참조를 든 노드, 없으면 NULL.
 *
 * 옛 결합(mmu-masters)에서만 쓴다. PCI 장치는 장치 트리에 자기 노드가
 * 없으므로, 뿌리 버스까지 거슬러 올라가 그 다리(host bridge)의 부모
 * 노드를 대신 쓴다.
 *
 * 참조를 든 채 돌려주므로 호출자가 of_node_put 해야 한다.
 */
static struct device_node *dev_get_dev_node(struct device *dev)
{
	if (dev_is_pci(dev)) {	/* [한국어] PCI 장치는 장치 트리에 자기 노드가 없다. */
		struct pci_bus *bus = to_pci_dev(dev)->bus;	/* [한국어] 그 장치가 달린 버스. */

		while (!pci_is_root_bus(bus))	/* [한국어] 뿌리 버스까지 */
			bus = bus->parent;	/* [한국어] 거슬러 올라간다. */
		return of_node_get(bus->bridge->parent->of_node);	/* [한국어] 그 다리의 부모가 장치 트리에 있는 노드다. 참조를 들어 돌려준다. */
	}

	return of_node_get(dev->of_node);	/* [한국어] 플랫폼 장치는 자기 노드가 있다. */
}

/*
 * [한국어]
 * __arm_smmu_get_pci_sid - PCI 별칭 순회에서 스트림 id 를 받아 둔다
 *
 * @pdev: PCI 장치(쓰지 않는다).
 * @alias: 이 단계의 요청자 id.
 * @data: 받아 둘 자리.
 * @return: 0 — 계속 순회하라는 뜻(원 주석대로).
 *
 * 0 을 돌려주어 끝까지 도는 것이 요점이다. 마지막에 남는 값이 가장
 * 상위에서 본 요청자 id 이고, 옛 결합은 그것을 스트림 id 로 본다.
 */
static int __arm_smmu_get_pci_sid(struct pci_dev *pdev, u16 alias, void *data)
{
	*((__be32 *)data) = cpu_to_be32(alias);	/* [한국어] 장치 트리 값은 빅 엔디언이라 그 형식으로 담는다. */
	return 0; /* Continue walking */	/* [한국어] 원 주석대로 계속 돈다. 마지막에 남는 값이 가장 상위에서 본 요청자 id 다. */
}

/*
 * [한국어]
 * __find_legacy_master_phandle - 이 장치를 가리키는 SMMU 를 찾는다
 *
 * @dev: 후보 SMMU 장치.
 * @data: 찾는 노드가 담긴 반복자, 찾으면 그 장치를 여기에 쓴다.
 * @return: 1 이면 찾았다(순회를 끝낸다), 0 이면 계속, 음수면 오류.
 *
 * 옛 결합은 방향이 반대다. 장치가 자기 IOMMU 를 가리키는 것이 아니라,
 * SMMU 가 mmu-masters 로 자기 아래 장치들을 나열한다. 그래서 모든 SMMU 를
 * 훑으며 이 장치를 가리키는 것을 찾아야 한다.
 *
 * 반복자를 그대로 둔 채 돌아가는 것이 요령이다 — 찾았을 때 그 반복자에
 * 스트림 id 인자가 담겨 있어, 호출자가 곧바로 읽을 수 있다.
 */
static int __find_legacy_master_phandle(struct device *dev, void *data)
{
	struct of_phandle_iterator *it = *(void **)data;	/* [한국어] 찾는 노드가 담긴 반복자. 찾으면 이 자리에 장치를 대신 넣는다. */
	struct device_node *np = it->node;	/* [한국어] 우리가 찾는 장치의 노드. */
	int err;	/* [한국어] 순회 결과. */

	of_for_each_phandle(it, err, dev->of_node, "mmu-masters",	/* [한국어] 이 SMMU 가 나열한 장치들을 훑는다. */
			    "#stream-id-cells", -1)
		if (it->node == np) {	/* [한국어] 우리 장치를 찾았으면 */
			*(void **)data = dev;	/* [한국어] 이 SMMU 장치를 알려 주고 */
			return 1;	/* [한국어] 순회를 끝낸다. */
		}
	it->node = np;	/* [한국어] 못 찾았으면 반복자를 원래대로 돌려놓는다 — 다음 SMMU 를 볼 때 다시 쓴다. */
	return err == -ENOENT ? 0 : err;	/* [한국어] 속성이 없는 것은 오류가 아니라 이 SMMU 가 옛 결합을 안 쓴다는 뜻이다. */
}

/*
 * [한국어]
 * arm_smmu_register_legacy_master - 옛 결합으로 장치를 등록한다
 *
 * @dev: 등록할 장치.
 * @smmu: 찾아낸 SMMU 를 여기에 쓴다.
 * @return: 0 성공, 음수면 실패.
 *
 * 옛 장치 트리(mmu-masters)를 쓰는 기계를 위한 경로다. 지금 트리는
 * 장치 쪽에 iommus 속성을 두지만, 이미 배포된 트리를 깨지 않으려고
 * 남겨 두었다.
 *
 * 모든 SMMU 를 훑어 이 장치를 가리키는 것을 찾은 뒤, 그 항목에 적힌
 * 스트림 id 들을 fwspec 에 담는다. 그러면 이후 경로가 새 결합과 똑같이
 * 동작한다.
 *
 * PCI 는 원 주석대로 스트림 id 가 곧 요청자 id 라고 가정한다.
 */
static int arm_smmu_register_legacy_master(struct device *dev,
					   struct arm_smmu_device **smmu)
{
	struct device *smmu_dev;	/* [한국어] 찾아낸 SMMU 장치. */
	struct device_node *np;	/* [한국어] 이 장치의 트리 노드. */
	struct of_phandle_iterator it;	/* [한국어] 속성 순회 상태. 찾은 뒤 인자를 읽는 데도 쓴다. */
	void *data = &it;	/* [한국어] 순회 콜백에 넘길 값. 찾으면 여기에 장치가 들어온다. */
	u32 *sids;	/* [한국어] 읽어 낼 스트림 id 배열. */
	__be32 pci_sid;	/* [한국어] PCI 일 때 만들어 낸 id. */
	int err;	/* [한국어] 결과 코드. */

	np = dev_get_dev_node(dev);	/* [한국어] 이 장치를 대표하는 노드. */
	if (!np || !of_property_present(np, "#stream-id-cells")) {	/* [한국어] 노드가 없거나 옛 결합의 표시가 없으면 */
		of_node_put(np);	/* [한국어] 참조를 놓고 */
		return -ENODEV;	/* [한국어] 이 경로가 아니다. */
	}

	it.node = np;	/* [한국어] 찾을 노드를 반복자에 담는다. */
	err = driver_for_each_device(&arm_smmu_driver.driver, NULL, &data,	/* [한국어] 모든 SMMU 장치를 훑으며 이 장치를 가리키는 것을 찾는다. */
				     __find_legacy_master_phandle);
	smmu_dev = data;	/* [한국어] 찾았으면 여기 들어 있다. */
	of_node_put(np);	/* [한국어] 노드 참조를 놓는다. */
	if (err == 0)	/* [한국어] 아무도 이 장치를 가리키지 않았다. */
		return -ENODEV;	/* [한국어] 이 SMMU 아래가 아니다. */
	if (err < 0)	/* [한국어] 순회 중 오류가 났다. */
		return err;	/* [한국어] 그대로 올린다. */

	if (dev_is_pci(dev)) {	/* [한국어] PCI 장치면 */
		/* "mmu-masters" assumes Stream ID == Requester ID */
		pci_for_each_dma_alias(to_pci_dev(dev), __arm_smmu_get_pci_sid,	/* [한국어] 원 주석대로 스트림 id 가 곧 요청자 id 라 가정하고 그것을 구한다. */
				       &pci_sid);
		it.cur = &pci_sid;	/* [한국어] 반복자가 그 값을 가리키게 해 */
		it.cur_count = 1;	/* [한국어] 아래 공통 코드가 그대로 읽게 한다. */
	}

	err = iommu_fwspec_init(dev, NULL);	/* [한국어] 새 결합과 같은 자료 구조를 만든다. 이후 경로가 구별하지 않게 하려는 것이다. */
	if (err)	/* [한국어] 만들지 못했으면 */
		return err;	/* [한국어] 그대로 올린다. */

	sids = kcalloc(it.cur_count, sizeof(*sids), GFP_KERNEL);	/* [한국어] id 배열을 잡는다. */
	if (!sids)	/* [한국어] 메모리가 없다. */
		return -ENOMEM;	/* [한국어] 실패. */

	*smmu = dev_get_drvdata(smmu_dev);	/* [한국어] 찾은 SMMU 의 드라이버 상태를 호출자에게 알린다. */
	of_phandle_iterator_args(&it, sids, it.cur_count);	/* [한국어] 반복자에 남아 있던 인자들을 배열로 읽어 낸다. */
	err = iommu_fwspec_add_ids(dev, sids, it.cur_count);	/* [한국어] 새 결합과 같은 형식으로 담는다. */
	kfree(sids);	/* [한국어] 임시 배열을 해제한다. */
	return err;	/* [한국어] 결과. */
}
#else
/*
 * [한국어]
 * arm_smmu_register_legacy_master - 옛 결합이 꺼진 빌드의 빈 판
 *
 * @dev: 대상 장치.
 * @smmu: 채우지 않는다.
 * @return: 늘 -ENODEV.
 *
 * 실패를 돌려주어 그 경로가 아예 쓰이지 않게 한다. 호출부에 #ifdef 를
 * 뿌리지 않으려는 흔한 방식이다.
 */
static int arm_smmu_register_legacy_master(struct device *dev,
					   struct arm_smmu_device **smmu)
{
	return -ENODEV;	/* [한국어] 옛 결합이 꺼진 빌드에서는 늘 실패한다. */
}
#endif /* CONFIG_ARM_SMMU_LEGACY_DT_BINDINGS */

/*
 * [한국어]
 * __arm_smmu_free_bitmap - 비트맵에서 자리를 놓는다
 *
 * @map: 대상 비트맵.
 * @idx: 놓을 자리.
 *
 * 잡을 때와 달리 경합이 없어 그냥 지운다 — 자기가 잡은 자리를 자기가
 * 놓는 것이라 남이 끼어들 수 없다.
 */
static void __arm_smmu_free_bitmap(unsigned long *map, int idx)
{
	clear_bit(idx, map);	/* [한국어] 자기가 잡은 자리를 놓는 것이라 경합이 없다. */
}

/* Wait for any pending TLB invalidations to complete */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * __arm_smmu_tlb_sync - TLB 무효화가 끝날 때까지 기다린다
 *
 * @smmu: 대상 SMMU.
 * @page: 동기화 레지스터가 있는 페이지.
 * @sync: 동기화 명령 레지스터의 오프셋.
 * @status: 그 상태 레지스터의 오프셋.
 *
 * 무효화 명령은 비동기라, 실제로 끝났는지 따로 확인해야 한다. 끝나기
 * 전에 그 페이지를 재사용하면 장치가 낡은 매핑으로 DMA 한다.
 *
 * 기다림이 두 겹이다. 처음에는 돌면서(spin) 기다리고 — 대개 곧 끝난다 —
 * 그래도 안 끝나면 점점 긴 시간을 잔다. 짧은 경우를 빠르게 처리하면서
 * 긴 경우에 CPU 를 낭비하지 않는 절충이다.
 *
 * 1초가 지나도 끝나지 않으면 하드웨어가 멈춘 것이다. 그 이상 할 수 있는
 * 일이 없어 로그만 남기고 돌아간다.
 *
 * QCOM_DUMMY_VAL 을 쓰는 이유는 파일 위쪽 주석에 있다 — 어떤 퀄컴
 * 하이퍼바이저가 0 쓰기(제로 레지스터를 원본으로 쓰는 명령)를 잘못
 * 처리해, 0 이 아닌 값을 써야 한다.
 */
static void __arm_smmu_tlb_sync(struct arm_smmu_device *smmu, int page,
				int sync, int status)
{
	unsigned int spin_cnt, delay;	/* [한국어] 돌며 기다린 횟수와 잠들 시간. */
	u32 reg;	/* [한국어] 상태 레지스터 값. */

	if (smmu->impl && unlikely(smmu->impl->tlb_sync))	/* [한국어] 구현체가 대기 방식을 바꿨으면 */
		return smmu->impl->tlb_sync(smmu, page, sync, status);	/* [한국어] 그쪽에 맡긴다. */

	arm_smmu_writel(smmu, page, sync, QCOM_DUMMY_VAL);	/* [한국어] 동기화를 시작시킨다. 0 이 아닌 값을 쓰는 이유는 파일 위쪽 주석에 있다. */
	for (delay = 1; delay < TLB_LOOP_TIMEOUT; delay *= 2) {	/* [한국어] 잠드는 시간을 두 배씩 늘려 간다. */
		for (spin_cnt = TLB_SPIN_COUNT; spin_cnt > 0; spin_cnt--) {	/* [한국어] 먼저 돌며 기다린다 — 대개 곧 끝난다. */
			reg = arm_smmu_readl(smmu, page, status);	/* [한국어] 진행 상태를 읽는다. */
			if (!(reg & ARM_SMMU_sTLBGSTATUS_GSACTIVE))	/* [한국어] 활성 비트가 내려갔으면 */
				return;	/* [한국어] 끝났다. */
			cpu_relax();	/* [한국어] 같은 자리를 도는 동안 CPU 에 힌트를 준다. */
		}
		udelay(delay);	/* [한국어] 아직이면 그만큼 잠든다. */
	}
	dev_err_ratelimited(smmu->dev,	/* [한국어] 1초가 지났다. 하드웨어가 멈춘 것이고, 더 할 수 있는 일이 없다. */
			    "TLB sync timed out -- SMMU may be deadlocked\n");
}

/*
 * [한국어]
 * arm_smmu_tlb_sync_global - 전역 무효화의 완료를 기다린다
 *
 * @smmu: 대상 SMMU.
 *
 * 명령과 완료 확인이 한 쌍이라, 겹치면 남의 완료를 자기 것으로 오해한다.
 * 그래서 스핀락으로 직렬화한다.
 *
 * 인터럽트를 막는 판을 쓰는 이유: 무효화가 인터럽트 문맥에서도 불릴 수
 * 있어, 그 안에서 같은 락을 잡으면 교착이 된다.
 */
static void arm_smmu_tlb_sync_global(struct arm_smmu_device *smmu)
{
	unsigned long flags;	/* [한국어] 인터럽트 상태 저장. */

	spin_lock_irqsave(&smmu->global_sync_lock, flags);	/* [한국어] 명령과 완료 확인이 한 쌍이라 직렬화한다. 인터럽트 문맥에서도 불려 막아야 한다. */
	__arm_smmu_tlb_sync(smmu, ARM_SMMU_GR0, ARM_SMMU_GR0_sTLBGSYNC,	/* [한국어] 전역 레지스터로 기다린다. */
			    ARM_SMMU_GR0_sTLBGSTATUS);
	spin_unlock_irqrestore(&smmu->global_sync_lock, flags);	/* [한국어] 락 해제. */
}

/*
 * [한국어]
 * arm_smmu_tlb_sync_context - 이 도메인의 무효화 완료를 기다린다
 *
 * @smmu_domain: 대상 도메인.
 *
 * 뱅크마다 동기화 레지스터가 따로 있어, 도메인 단위로 기다릴 수 있다.
 * 전역보다 훨씬 덜 부딪힌다.
 */
static void arm_smmu_tlb_sync_context(struct arm_smmu_domain *smmu_domain)
{
	struct arm_smmu_device *smmu = smmu_domain->smmu;	/* [한국어] 이 도메인이 매인 SMMU. */
	unsigned long flags;	/* [한국어] 인터럽트 상태 저장. */

	spin_lock_irqsave(&smmu_domain->cb_lock, flags);	/* [한국어] 뱅크 단위 락이라 전역보다 훨씬 덜 부딪힌다. */
	__arm_smmu_tlb_sync(smmu, ARM_SMMU_CB(smmu, smmu_domain->cfg.cbndx),	/* [한국어] 그 뱅크의 동기화 레지스터로 기다린다. */
			    ARM_SMMU_CB_TLBSYNC, ARM_SMMU_CB_TLBSTATUS);
	spin_unlock_irqrestore(&smmu_domain->cb_lock, flags);	/* [한국어] 락 해제. */
}

/*
 * [한국어]
 * arm_smmu_tlb_inv_context_s1 - 1단계 주소 공간 전체를 무효화한다
 *
 * @cookie: io-pgtable 에 등록해 둔 도메인 포인터.
 *
 * ASID 통째로 비운다. 구간이 넓을 때는 하나씩 비우는 것보다 빠르다.
 *
 * 원 주석이 wmb 의 이유를 밝힌다 — 레지스터 쓰기가 relaxed 판이라
 * 순서가 보장되지 않는다. 이 CPU 가 지운 페이지 테이블 항목이 먼저
 * 보여야, 무효화 뒤에 하드웨어가 낡은 항목을 다시 읽지 않는다.
 */
static void arm_smmu_tlb_inv_context_s1(void *cookie)
{
	struct arm_smmu_domain *smmu_domain = cookie;	/* [한국어] io-pgtable 에 등록해 둔 도메인. */
	/*
	 * The TLBI write may be relaxed, so ensure that PTEs cleared by the
	 * current CPU are visible beforehand.
	 */
	wmb();	/* [한국어] 원 주석대로 이 CPU 가 지운 표 항목이 무효화보다 먼저 보여야 한다. */
	arm_smmu_cb_write(smmu_domain->smmu, smmu_domain->cfg.cbndx,	/* [한국어] ASID 통째로 비운다. */
			  ARM_SMMU_CB_S1_TLBIASID, smmu_domain->cfg.asid);
	arm_smmu_tlb_sync_context(smmu_domain);	/* [한국어] 끝날 때까지 기다린다. */
}

/*
 * [한국어]
 * arm_smmu_tlb_inv_context_s2 - 2단계 주소 공간 전체를 무효화한다
 *
 * @cookie: 도메인 포인터.
 *
 * VMID 통째로 비운다. 2단계 무효화는 전역 레지스터로 하므로 전역
 * 동기화를 기다려야 한다.
 */
static void arm_smmu_tlb_inv_context_s2(void *cookie)
{
	struct arm_smmu_domain *smmu_domain = cookie;	/* [한국어] 도메인 포인터. */
	struct arm_smmu_device *smmu = smmu_domain->smmu;	/* [한국어] 그 SMMU. */

	/* See above */
	wmb();	/* [한국어] 위와 같은 이유의 장벽. */
	arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_TLBIVMID, smmu_domain->cfg.vmid);	/* [한국어] VMID 통째로 비운다. 2단계 무효화는 전역 레지스터로 나간다. */
	arm_smmu_tlb_sync_global(smmu);	/* [한국어] 그래서 전역 동기화를 기다린다. */
}

/*
 * [한국어]
 * arm_smmu_tlb_inv_range_s1 - 1단계 TLB 를 구간 단위로 무효화한다
 *
 * @iova: 시작 주소.
 * @size: 길이.
 * @granule: 한 번에 비울 단위(페이지 크기).
 * @cookie: 도메인 포인터.
 * @reg: 쓸 무효화 레지스터(마지막 단계만 비울지에 따라 다르다).
 *
 * 표 형식에 따라 주소를 담는 방식이 다르다. 64비트 형식은 주소를 12비트
 * 내리고 ASID 를 상위 48비트에 얹어 한 번에 쓴다. 32비트 형식은 주소의
 * 하위 12비트 자리에 ASID 를 넣는다 — 페이지 정렬이라 그 자리가 비어 있다.
 *
 * 일관성 있는 표 순회를 하는 하드웨어에서만 wmb 를 넣는 데 주의. 그렇지
 * 않은 하드웨어는 어차피 페이지 테이블을 캐시에서 비우며 장벽을 거친다.
 */
static void arm_smmu_tlb_inv_range_s1(unsigned long iova, size_t size,
				      size_t granule, void *cookie, int reg)
{
	struct arm_smmu_domain *smmu_domain = cookie;	/* [한국어] 도메인 포인터. */
	struct arm_smmu_device *smmu = smmu_domain->smmu;	/* [한국어] 그 SMMU. */
	struct arm_smmu_cfg *cfg = &smmu_domain->cfg;	/* [한국어] 뱅크 설정. */
	int idx = cfg->cbndx;	/* [한국어] 뱅크 번호. */

	if (smmu->features & ARM_SMMU_FEAT_COHERENT_WALK)	/* [한국어] 일관성 있는 표 순회를 하는 하드웨어에서만 */
		wmb();	/* [한국어] 장벽이 필요하다. 아니면 표를 캐시에서 비우며 이미 장벽을 거친다. */

	if (cfg->fmt != ARM_SMMU_CTX_FMT_AARCH64) {	/* [한국어] 32비트 형식이면 */
		iova = (iova >> 12) << 12;	/* [한국어] 하위 12비트를 지워 */
		iova |= cfg->asid;	/* [한국어] 그 자리에 ASID 를 넣는다. 페이지 정렬이라 그 자리가 비어 있다. */
		do {
			arm_smmu_cb_write(smmu, idx, reg, iova);	/* [한국어] 32비트 쓰기로 무효화한다. */
			iova += granule;	/* [한국어] 다음 페이지로. */
		} while (size -= granule);	/* [한국어] 구간을 다 덮을 때까지. */
	} else {
		iova >>= 12;	/* [한국어] 64비트 형식은 주소를 페이지 번호로 만들고 */
		iova |= (u64)cfg->asid << 48;	/* [한국어] ASID 를 상위에 얹는다. */
		do {
			arm_smmu_cb_writeq(smmu, idx, reg, iova);	/* [한국어] 64비트 쓰기로 한 번에 보낸다. */
			iova += granule >> 12;	/* [한국어] 페이지 번호 단위로 나아간다. */
		} while (size -= granule);	/* [한국어] 구간을 다 덮을 때까지. */
	}
}

/*
 * [한국어]
 * arm_smmu_tlb_inv_range_s2 - 2단계 TLB 를 구간 단위로 무효화한다
 *
 * @iova: 시작 중간 물리 주소.
 * @size: 길이.
 * @granule: 한 번에 비울 단위.
 * @cookie: 도메인 포인터.
 * @reg: 쓸 무효화 레지스터.
 *
 * 2단계에는 ASID 가 없어 주소만 쓰면 된다 — 뱅크 자체가 VMID 를 알고
 * 있기 때문이다. 그래서 1단계 판보다 단순하다.
 */
static void arm_smmu_tlb_inv_range_s2(unsigned long iova, size_t size,
				      size_t granule, void *cookie, int reg)
{
	struct arm_smmu_domain *smmu_domain = cookie;	/* [한국어] 도메인 포인터. */
	struct arm_smmu_device *smmu = smmu_domain->smmu;	/* [한국어] 그 SMMU. */
	int idx = smmu_domain->cfg.cbndx;	/* [한국어] 뱅크 번호. */

	if (smmu->features & ARM_SMMU_FEAT_COHERENT_WALK)	/* [한국어] 일관성 있는 순회면 */
		wmb();	/* [한국어] 장벽이 필요하다. */

	iova >>= 12;	/* [한국어] 2단계에는 ASID 가 없어 주소만 담으면 된다. */
	do {
		if (smmu_domain->cfg.fmt == ARM_SMMU_CTX_FMT_AARCH64)	/* [한국어] 64비트 형식이면 */
			arm_smmu_cb_writeq(smmu, idx, reg, iova);	/* [한국어] 한 번에 쓰고 */
		else
			arm_smmu_cb_write(smmu, idx, reg, iova);	/* [한국어] 아니면 32비트로 쓴다. */
		iova += granule >> 12;	/* [한국어] 다음 페이지 번호로. */
	} while (size -= granule);	/* [한국어] 구간을 다 덮을 때까지. */
}

/*
 * [한국어]
 * arm_smmu_tlb_inv_walk_s1 - 표 순회 캐시까지 비우는 1단계 무효화
 *
 * @iova: 시작 주소.
 * @size: 길이.
 * @granule: 비울 단위.
 * @cookie: 도메인 포인터.
 *
 * io-pgtable 이 중간 단계 표를 지운 뒤 부른다. 그 표를 캐시해 둔 항목까지
 * 비워야 하므로 마지막 단계만 비우는 판(TLBIVAL)을 쓰지 않는다.
 *
 * 구현체가 ASID 통째 비우기를 선호한다고 표시했으면 그쪽으로 간다 —
 * 어떤 하드웨어는 구간 무효화가 몹시 느리다.
 */
static void arm_smmu_tlb_inv_walk_s1(unsigned long iova, size_t size,
				     size_t granule, void *cookie)
{
	struct arm_smmu_domain *smmu_domain = cookie;	/* [한국어] 도메인 포인터. */
	struct arm_smmu_cfg *cfg = &smmu_domain->cfg;	/* [한국어] 뱅크 설정. */

	if (cfg->flush_walk_prefer_tlbiasid) {	/* [한국어] 구현체가 ASID 통째 비우기를 선호하면 */
		arm_smmu_tlb_inv_context_s1(cookie);	/* [한국어] 그쪽으로 간다 — 어떤 하드웨어는 구간 무효화가 몹시 느리다. */
	} else {
		arm_smmu_tlb_inv_range_s1(iova, size, granule, cookie,	/* [한국어] 중간 표 캐시까지 비우는 레지스터를 쓴다. */
					  ARM_SMMU_CB_S1_TLBIVA);
		arm_smmu_tlb_sync_context(cookie);	/* [한국어] 끝날 때까지 기다린다. */
	}
}

/*
 * [한국어]
 * arm_smmu_tlb_add_page_s1 - 잎 페이지 하나를 무효화 목록에 더한다
 *
 * @gather: 코어가 모아 두는 무효화 목록(여기서는 쓰지 않는다).
 * @iova: 그 주소.
 * @granule: 그 크기.
 * @cookie: 도메인 포인터.
 *
 * 마지막 단계만 비우는 레지스터(TLBIVAL)를 쓴다 — 잎 항목만 바뀌었으므로
 * 중간 표 캐시는 그대로 두어도 된다.
 *
 * 모으지 않고 곧바로 비우는 데 주의. 완료 대기는 나중에 한 번만 한다.
 */
static void arm_smmu_tlb_add_page_s1(struct iommu_iotlb_gather *gather,
				     unsigned long iova, size_t granule,
				     void *cookie)
{
	arm_smmu_tlb_inv_range_s1(iova, granule, granule, cookie,	/* [한국어] 마지막 단계만 비우는 레지스터. 잎 항목만 바뀌었으므로 중간 표 캐시는 그대로 둔다. */
				  ARM_SMMU_CB_S1_TLBIVAL);
}

/*
 * [한국어]
 * arm_smmu_tlb_inv_walk_s2 - 표 순회 캐시까지 비우는 2단계 무효화
 *
 * @iova: 시작 주소.
 * @size: 길이.
 * @granule: 비울 단위.
 * @cookie: 도메인 포인터.
 */
static void arm_smmu_tlb_inv_walk_s2(unsigned long iova, size_t size,
				     size_t granule, void *cookie)
{
	arm_smmu_tlb_inv_range_s2(iova, size, granule, cookie,	/* [한국어] 중간 표 캐시까지 비운다. */
				  ARM_SMMU_CB_S2_TLBIIPAS2);
	arm_smmu_tlb_sync_context(cookie);	/* [한국어] 끝날 때까지 기다린다. */
}

/*
 * [한국어]
 * arm_smmu_tlb_add_page_s2 - 2단계 잎 페이지 하나를 무효화한다
 *
 * @gather: 무효화 목록(쓰지 않는다).
 * @iova: 그 주소.
 * @granule: 그 크기.
 * @cookie: 도메인 포인터.
 */
static void arm_smmu_tlb_add_page_s2(struct iommu_iotlb_gather *gather,
				     unsigned long iova, size_t granule,
				     void *cookie)
{
	arm_smmu_tlb_inv_range_s2(iova, granule, granule, cookie,	/* [한국어] 마지막 단계만 비우는 레지스터. */
				  ARM_SMMU_CB_S2_TLBIIPAS2L);
}

/*
 * [한국어]
 * arm_smmu_tlb_inv_walk_s2_v1 - 옛 판의 2단계 무효화
 *
 * @iova: 시작 주소(쓰지 않는다).
 * @size: 길이(쓰지 않는다).
 * @granule: 단위(쓰지 않는다).
 * @cookie: 도메인 포인터.
 *
 * 1판 하드웨어에는 2단계 주소 단위 무효화가 없다. VMID 통째로 비우는
 * 수밖에 없어, 인자를 모두 무시한다.
 */
static void arm_smmu_tlb_inv_walk_s2_v1(unsigned long iova, size_t size,
					size_t granule, void *cookie)
{
	arm_smmu_tlb_inv_context_s2(cookie);	/* [한국어] 1판에는 2단계 주소 단위 무효화가 없어 VMID 통째로 비운다. */
}
/*
 * On MMU-401 at least, the cost of firing off multiple TLBIVMIDs appears
 * almost negligible, but the benefit of getting the first one in as far ahead
 * of the sync as possible is significant, hence we don't just make this a
 * no-op and call arm_smmu_tlb_inv_context_s2() from .iotlb_sync as you might
 * think.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * arm_smmu_tlb_add_page_s2_v1 - 옛 판에서 페이지 하나가 바뀌었을 때
 *
 * @gather: 무효화 목록(쓰지 않는다).
 * @iova: 그 주소(쓰지 않는다).
 * @granule: 그 크기(쓰지 않는다).
 * @cookie: 도메인 포인터.
 *
 * 원 주석이 이 함수가 no-op 이 아닌 이유를 설명한다 — MMU-401 에서
 * VMID 무효화를 여러 번 쏘는 비용은 거의 없는데, 첫 번째를 완료 대기보다
 * 최대한 앞당기는 이득은 크다. 그래서 모아 두었다 한 번 하는 대신 그때그때
 * 쏜다.
 */
static void arm_smmu_tlb_add_page_s2_v1(struct iommu_iotlb_gather *gather,
					unsigned long iova, size_t granule,
					void *cookie)
{
	struct arm_smmu_domain *smmu_domain = cookie;	/* [한국어] 도메인 포인터. */
	struct arm_smmu_device *smmu = smmu_domain->smmu;	/* [한국어] 그 SMMU. */

	if (smmu->features & ARM_SMMU_FEAT_COHERENT_WALK)	/* [한국어] 일관성 있는 순회면 */
		wmb();	/* [한국어] 장벽이 필요하다. */

	arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_TLBIVMID, smmu_domain->cfg.vmid);	/* [한국어] 원 주석대로 여러 번 쏘는 비용은 거의 없고, 첫 번째를 앞당기는 이득이 크다. */
}

/*
 * [한국어] 1단계 도메인의 TLB 무효화 함수표.
 *
 * io-pgtable 이 표를 고칠 때마다 이 함수들을 부른다.
 */
static const struct iommu_flush_ops arm_smmu_s1_tlb_ops = {
	.tlb_flush_all	= arm_smmu_tlb_inv_context_s1,	/* [한국어] ASID 통째로 비운다. */
	.tlb_flush_walk	= arm_smmu_tlb_inv_walk_s1,
	.tlb_add_page	= arm_smmu_tlb_add_page_s1,
};

/*
 * [한국어] 2판 하드웨어의 2단계 무효화 함수표.
 *
 * 주소 단위 무효화를 쓸 수 있다.
 */
static const struct iommu_flush_ops arm_smmu_s2_tlb_ops_v2 = {
	.tlb_flush_all	= arm_smmu_tlb_inv_context_s2,	/* [한국어] VMID 통째로 비운다. */
	.tlb_flush_walk	= arm_smmu_tlb_inv_walk_s2,
	.tlb_add_page	= arm_smmu_tlb_add_page_s2,
};

/*
 * [한국어] 1판 하드웨어의 2단계 무효화 함수표.
 *
 * 주소 단위 무효화가 없어 VMID 통째로 비운다.
 */
static const struct iommu_flush_ops arm_smmu_s2_tlb_ops_v1 = {
	.tlb_flush_all	= arm_smmu_tlb_inv_context_s2,	/* [한국어] 1판도 통째 비우기는 같다 — 다른 것은 구간 무효화뿐이다. */
	.tlb_flush_walk	= arm_smmu_tlb_inv_walk_s2_v1,
	.tlb_add_page	= arm_smmu_tlb_add_page_s2_v1,
};


/*
 * [한국어]
 * arm_smmu_read_context_fault_info - 오류 레지스터들을 한 번에 읽는다
 *
 * @smmu: 대상 SMMU.
 * @idx: 오류가 난 컨텍스트 뱅크 번호.
 * @cfi: 읽은 값을 담을 구조체.
 *
 * 네 레지스터를 잇달아 읽어 구조체에 담는다. 나중에 출력할 때 다시 읽으면
 * 그 사이 새 오류가 덮어썼을 수 있어, 한 번에 떠 둔다.
 *
 * 퀄컴 오류 처리기도 이 함수를 쓴다.
 */
void arm_smmu_read_context_fault_info(struct arm_smmu_device *smmu, int idx,
				      struct arm_smmu_context_fault_info *cfi)
{
	cfi->iova = arm_smmu_cb_readq(smmu, idx, ARM_SMMU_CB_FAR);	/* [한국어] 오류가 난 주소. */
	cfi->fsr = arm_smmu_cb_read(smmu, idx, ARM_SMMU_CB_FSR);	/* [한국어] 오류 종류 비트들. */
	cfi->fsynr = arm_smmu_cb_read(smmu, idx, ARM_SMMU_CB_FSYNR0);	/* [한국어] 읽기였는지 쓰기였는지 등 부가 정보. */
	cfi->cbfrsynra = arm_smmu_gr1_read(smmu, ARM_SMMU_GR1_CBFRSYNRA(idx));	/* [한국어] 어느 장치가 냈는지. 원인을 찾는 데 가장 중요한 값이다. */
}

/*
 * [한국어]
 * arm_smmu_print_context_fault_info - 오류 내용을 사람이 읽게 찍는다
 *
 * @smmu: 대상 SMMU.
 * @idx: 뱅크 번호.
 * @cfi: 읽어 둔 오류 정보.
 *
 * 비트 이름을 그대로 풀어 쓴다. 어느 장치가(SID) 어느 주소에서(iova)
 * 무엇을 하다(WNR, IND) 왜 실패했는지(TF, PF, EF)가 한 줄에 담긴다.
 *
 * 이 출력이 SMMU 문제를 다루는 가장 흔한 실마리다.
 */
void arm_smmu_print_context_fault_info(struct arm_smmu_device *smmu, int idx,
				       const struct arm_smmu_context_fault_info *cfi)
{
	dev_err(smmu->dev,	/* [한국어] 한 줄 요약. 뱅크 번호까지 붙여 어느 도메인인지 알 수 있게 한다. */
		"Unhandled context fault: fsr=0x%x, iova=0x%08lx, fsynr=0x%x, cbfrsynra=0x%x, cb=%d\n",
		cfi->fsr, cfi->iova, cfi->fsynr, cfi->cbfrsynra, idx);	/* [한국어] 원시 값들을 그대로 남겨, 나중에 손으로 해석할 수도 있게 한다. */

	dev_err(smmu->dev, "FSR    = %08x [%s%sFormat=%u%s%s%s%s%s%s%s%s], SID=0x%x\n",	/* [한국어] 오류 비트들을 이름으로 풀어 쓴다. */
		cfi->fsr,
		(cfi->fsr & ARM_SMMU_CB_FSR_MULTI)  ? "MULTI " : "",	/* [한국어] 여러 오류가 겹쳤다 — 앞의 것을 지우기 전에 새 오류가 났다. */
		(cfi->fsr & ARM_SMMU_CB_FSR_SS)     ? "SS " : "",	/* [한국어] 트랜잭션이 멈춰 서 있다. RESUME 을 써야 진행된다. */
		(u32)FIELD_GET(ARM_SMMU_CB_FSR_FORMAT, cfi->fsr),
		(cfi->fsr & ARM_SMMU_CB_FSR_UUT)    ? " UUT" : "",	/* [한국어] 예상치 못한 미사용 트랜잭션. */
		(cfi->fsr & ARM_SMMU_CB_FSR_ASF)    ? " ASF" : "",	/* [한국어] 접근 플래그 오류. 표의 AF 비트가 0 이었다. */
		(cfi->fsr & ARM_SMMU_CB_FSR_TLBLKF) ? " TLBLKF" : "",	/* [한국어] TLB 잠금 오류. */
		(cfi->fsr & ARM_SMMU_CB_FSR_TLBMCF) ? " TLBMCF" : "",	/* [한국어] TLB 다중 일치. 무효화를 빠뜨렸을 때 난다. */
		(cfi->fsr & ARM_SMMU_CB_FSR_EF)     ? " EF" : "",	/* [한국어] 외부 오류. 표를 읽다 버스 오류가 났다. */
		(cfi->fsr & ARM_SMMU_CB_FSR_PF)     ? " PF" : "",	/* [한국어] 권한 오류. 매핑은 있지만 그 접근이 허용되지 않았다. */
		(cfi->fsr & ARM_SMMU_CB_FSR_AFF)    ? " AFF" : "",	/* [한국어] 주소 크기 오류. */
		(cfi->fsr & ARM_SMMU_CB_FSR_TF)     ? " TF" : "",	/* [한국어] 변환 오류. 매핑이 아예 없다 — 가장 흔한 원인이다. */
		cfi->cbfrsynra);

	dev_err(smmu->dev, "FSYNR0 = %08x [S1CBNDX=%u%s%s%s%s%s%s PLVL=%u]\n",	/* [한국어] 부가 정보를 풀어 쓴다. */
		cfi->fsynr,
		(u32)FIELD_GET(ARM_SMMU_CB_FSYNR0_S1CBNDX, cfi->fsynr),
		(cfi->fsynr & ARM_SMMU_CB_FSYNR0_AFR) ? " AFR" : "",	/* [한국어] 접근 플래그 관련. */
		(cfi->fsynr & ARM_SMMU_CB_FSYNR0_PTWF) ? " PTWF" : "",	/* [한국어] 표를 순회하다 난 오류 — 장치의 접근이 아니라 표 읽기가 실패했다. */
		(cfi->fsynr & ARM_SMMU_CB_FSYNR0_NSATTR) ? " NSATTR" : "",	/* [한국어] 비보안 속성이었다. */
		(cfi->fsynr & ARM_SMMU_CB_FSYNR0_IND) ? " IND" : "",	/* [한국어] 명령 인출이었다. */
		(cfi->fsynr & ARM_SMMU_CB_FSYNR0_PNU) ? " PNU" : "",	/* [한국어] 특권 없는 접근이었다. */
		(cfi->fsynr & ARM_SMMU_CB_FSYNR0_WNR) ? " WNR" : "",	/* [한국어] 쓰기였다. 없으면 읽기. */
		(u32)FIELD_GET(ARM_SMMU_CB_FSYNR0_PLVL, cfi->fsynr));
}

/*
 * [한국어]
 * arm_smmu_context_fault - 문맥 오류 인터럽트 처리기
 *
 * @irq: 인터럽트 번호(쓰지 않는다).
 * @dev: 등록할 때 넘긴 도메인.
 * @return: IRQ_HANDLED 또는 IRQ_NONE.
 *
 * 오류 비트가 하나도 없으면 우리 것이 아니다 — 인터럽트를 여러 뱅크가
 * 나눠 쓸 수 있어 그 판별이 필요하다.
 *
 * 먼저 상위 계층에 알려 본다. 그것을 다룰 수 있는 곳이 있으면(GPU
 * 드라이버 등) 거기서 처리하고, -ENOSYS 면 아무도 다루지 않는다는 뜻이라
 * 로그로 찍는다.
 *
 * 속도 제한을 두는 이유: 잘못된 장치는 초당 수천 번 오류를 낼 수 있고,
 * 그것을 모두 찍으면 로그가 마비된다.
 *
 * 마지막의 RESUME 이 중요하다. 멈춰 세워진 트랜잭션은 명시적으로 풀어
 * 주어야 한다. -EAGAIN 이면 다시 시도하라는 뜻이고, 아니면 끝내라고
 * 알린다.
 *
 * 실행 컨텍스트: 인터럽트 문맥(구현체에 따라 스레드 인터럽트일 수도).
 */
static irqreturn_t arm_smmu_context_fault(int irq, void *dev)
{
	struct arm_smmu_context_fault_info cfi;	/* [한국어] 읽어 둘 오류 정보. */
	struct arm_smmu_domain *smmu_domain = dev;	/* [한국어] 등록할 때 넘긴 도메인. */
	struct arm_smmu_device *smmu = smmu_domain->smmu;	/* [한국어] 그 SMMU. */
	static DEFINE_RATELIMIT_STATE(rs, DEFAULT_RATELIMIT_INTERVAL,	/* [한국어] 속도 제한 상태. 잘못된 장치는 초당 수천 번 오류를 낸다. */
				      DEFAULT_RATELIMIT_BURST);
	int idx = smmu_domain->cfg.cbndx;	/* [한국어] 뱅크 번호. */
	int ret;	/* [한국어] 상위 계층의 처리 결과. */

	arm_smmu_read_context_fault_info(smmu, idx, &cfi);	/* [한국어] 레지스터들을 한 번에 떠 둔다. */

	if (!(cfi.fsr & ARM_SMMU_CB_FSR_FAULT))	/* [한국어] 오류 비트가 하나도 없으면 */
		return IRQ_NONE;	/* [한국어] 우리 것이 아니다 — 인터럽트를 여러 뱅크가 나눠 쓸 수 있다. */

	ret = report_iommu_fault(&smmu_domain->domain, NULL, cfi.iova,	/* [한국어] 상위 계층에 알려 본다. GPU 드라이버처럼 스스로 처리하는 곳이 있다. */
		cfi.fsynr & ARM_SMMU_CB_FSYNR0_WNR ? IOMMU_FAULT_WRITE : IOMMU_FAULT_READ);	/* [한국어] 방향을 함께 알린다. */

	if (ret == -ENOSYS && __ratelimit(&rs))	/* [한국어] 아무도 다루지 않았고 속도 제한에 걸리지 않았으면 */
		arm_smmu_print_context_fault_info(smmu, idx, &cfi);	/* [한국어] 로그로 찍는다. */

	arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_FSR, cfi.fsr);	/* [한국어] 읽은 값을 되써서 그것만 지운다 — 그 사이 생긴 새 오류를 놓치지 않는다. */

	if (cfi.fsr & ARM_SMMU_CB_FSR_SS) {	/* [한국어] 멈춰 세워진 트랜잭션이 있으면 */
		arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_RESUME,	/* [한국어] 풀어 주어야 한다. 그러지 않으면 장치가 영원히 멈춰 있다. */
				  ret == -EAGAIN ? 0 : ARM_SMMU_RESUME_TERMINATE);	/* [한국어] -EAGAIN 은 다시 시도하라는 뜻이고, 아니면 끝내라고 알린다. */
	}

	return IRQ_HANDLED;	/* [한국어] 처리했다. */
}

/*
 * [한국어]
 * arm_smmu_global_fault - 전역 오류 인터럽트 처리기
 *
 * @irq: 인터럽트 번호(쓰지 않는다).
 * @dev: 등록할 때 넘긴 SMMU.
 * @return: IRQ_HANDLED 또는 IRQ_NONE.
 *
 * 뱅크에 매이지 않은 오류를 다룬다. 가장 흔한 것이 "모르는 스트림 id" —
 * 등록되지 않은 장치가 DMA 를 시도했다는 뜻이다.
 *
 * 그 경우 메시지에 부팅 인자까지 알려 주는 것이 친절한 대목이다. 다만
 * 그것을 켜면 보호가 사라진다는 경고도 함께 붙인다.
 *
 * 오류 상태는 1 을 써서 지운다 — 읽은 값을 그대로 되쓰면 본 것만 지워져,
 * 그 사이 생긴 새 오류를 놓치지 않는다.
 */
static irqreturn_t arm_smmu_global_fault(int irq, void *dev)
{
	u32 gfsr, gfsynr0, gfsynr1, gfsynr2;	/* [한국어] 오류 상태와 부가 정보들. */
	struct arm_smmu_device *smmu = dev;	/* [한국어] 등록할 때 넘긴 SMMU. */
	static DEFINE_RATELIMIT_STATE(rs, DEFAULT_RATELIMIT_INTERVAL,	/* [한국어] 속도 제한 상태. */
				      DEFAULT_RATELIMIT_BURST);

	gfsr = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_sGFSR);	/* [한국어] 오류 상태. */
	gfsynr0 = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_sGFSYNR0);	/* [한국어] 부가 정보 0. */
	gfsynr1 = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_sGFSYNR1);	/* [한국어] 부가 정보 1 — 오류를 낸 스트림 id 가 여기 있다. */
	gfsynr2 = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_sGFSYNR2);	/* [한국어] 부가 정보 2. */

	if (!gfsr)	/* [한국어] 오류 비트가 없으면 */
		return IRQ_NONE;	/* [한국어] 우리 것이 아니다. */

	if (__ratelimit(&rs)) {	/* [한국어] 속도 제한을 통과했으면 */
		if (IS_ENABLED(CONFIG_ARM_SMMU_DISABLE_BYPASS_BY_DEFAULT) &&	/* [한국어] 우회를 막는 설정이고 */
		    (gfsr & ARM_SMMU_sGFSR_USF))	/* [한국어] 모르는 스트림 오류면 */
			dev_err(smmu->dev,	/* [한국어] 가장 흔한 경우다. 등록되지 않은 장치가 DMA 를 시도했다. */
				"Blocked unknown Stream ID 0x%hx; boot with \"arm-smmu.disable_bypass=0\" to allow, but this may have security implications\n",
				(u16)gfsynr1);	/* [한국어] 어느 스트림인지 알려 준다. 부팅 인자도 함께 알리되 보안 경고를 붙인다. */
		else
			dev_err(smmu->dev,	/* [한국어] 그 밖의 전역 오류는 원인을 짚기 어려워 심각할 수 있다고만 알린다. */
				"Unexpected global fault, this could be serious\n");	/* [한국어] 원시 값은 아래에 함께 찍는다. */
		dev_err(smmu->dev,	/* [한국어] 전역 오류의 원시 값들을 함께 남긴다. */
			"\tGFSR 0x%08x, GFSYNR0 0x%08x, GFSYNR1 0x%08x, GFSYNR2 0x%08x\n",
			gfsr, gfsynr0, gfsynr1, gfsynr2);	/* [한국어] 네 레지스터를 그대로 남겨 손으로 해석할 수 있게 한다. */
	}

	arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_sGFSR, gfsr);	/* [한국어] 읽은 값을 되써서 그것만 지운다. */
	return IRQ_HANDLED;	/* [한국어] 처리했다. */
}

/*
 * [한국어]
 * arm_smmu_init_context_bank - 뱅크 설정을 메모리에 만들어 둔다
 *
 * @smmu_domain: 대상 도메인.
 * @pgtbl_cfg: io-pgtable 이 채운 페이지 테이블 설정.
 *
 * 하드웨어에 곧바로 쓰지 않고 그림자 구조체(smmu->cbs)에 담는다. 전원이
 * 꺼졌다 돌아오면 그것을 그대로 다시 쓰면 되기 때문이다.
 *
 * 표 형식마다 값을 가져오는 자리가 다르다. 32비트 짧은 서술자는 io-pgtable
 * 의 v7s 쪽에서, 그 밖에는 lpae 쪽에서 온다.
 *
 * 1단계일 때 TTBR0 과 TTBR1 에 모두 ASID 를 넣는 것이 눈에 띈다. 어느
 * 쪽에서 ASID 를 읽을지 TCR 의 A1 비트가 정하므로, 양쪽에 같은 값을 두면
 * 그 설정과 무관하게 맞는다.
 *
 * MAIR 은 1단계에만 있다 — 2단계는 메모리 속성을 표 항목에 직접 담는다.
 */
static void arm_smmu_init_context_bank(struct arm_smmu_domain *smmu_domain,
				       struct io_pgtable_cfg *pgtbl_cfg)
{
	struct arm_smmu_cfg *cfg = &smmu_domain->cfg;	/* [한국어] 이 도메인의 뱅크 설정. */
	struct arm_smmu_cb *cb = &smmu_domain->smmu->cbs[cfg->cbndx];	/* [한국어] 그 뱅크의 그림자 구조체. 하드웨어에 곧바로 쓰지 않고 여기 담아 둔다. */
	bool stage1 = cfg->cbar != CBAR_TYPE_S2_TRANS;	/* [한국어] 2단계 전용이 아니면 1단계가 있다는 뜻. */

	cb->cfg = cfg;	/* [한국어] 이 뱅크가 쓰이고 있음을 표시한다. NULL 이면 꺼진 뱅크다. */

	/* TCR */
	if (stage1) {	/* [한국어] 1단계면 TCR 을 그 형식에 맞게 만든다. */
		if (cfg->fmt == ARM_SMMU_CTX_FMT_AARCH32_S) {	/* [한국어] 32비트 짧은 서술자 형식이면 */
			cb->tcr[0] = pgtbl_cfg->arm_v7s_cfg.tcr;	/* [한국어] io-pgtable 의 v7s 쪽에서 값을 가져온다. */
		} else {
			cb->tcr[0] = arm_smmu_lpae_tcr(pgtbl_cfg);	/* [한국어] 긴 서술자 형식은 헤더의 변환 함수를 쓴다. */
			cb->tcr[1] = arm_smmu_lpae_tcr2(pgtbl_cfg);	/* [한국어] TCR2 도 함께. */
			if (cfg->fmt == ARM_SMMU_CTX_FMT_AARCH64)	/* [한국어] 64비트 형식이면 */
				cb->tcr[1] |= ARM_SMMU_TCR2_AS;	/* [한국어] 16비트 ASID 를 쓴다고 알린다. */
			else
				cb->tcr[0] |= ARM_SMMU_TCR_EAE;	/* [한국어] 32비트 형식은 확장 주소 비트로 긴 서술자를 고른다. */
		}
	} else {
		cb->tcr[0] = arm_smmu_lpae_vtcr(pgtbl_cfg);	/* [한국어] 2단계는 VTCR 하나뿐이다. */
	}

	/* TTBRs */
	if (stage1) {	/* [한국어] 표 기준 주소도 단계에 따라 다르다. */
		if (cfg->fmt == ARM_SMMU_CTX_FMT_AARCH32_S) {	/* [한국어] 짧은 서술자 형식은 v7s 쪽에서 값을 가져온다. */
			cb->ttbr[0] = pgtbl_cfg->arm_v7s_cfg.ttbr;	/* [한국어] v7s 는 표 주소가 하나다. */
			cb->ttbr[1] = 0;	/* [한국어] 두 번째는 쓰지 않는다. */
		} else {
			cb->ttbr[0] = FIELD_PREP(ARM_SMMU_TTBRn_ASID,	/* [한국어] 양쪽에 같은 ASID 를 넣는다 — TCR 의 A1 비트가 어느 쪽에서 읽을지 정하므로, 둘 다 채우면 그 설정과 무관하게 맞는다. */
						 cfg->asid);
			cb->ttbr[1] = FIELD_PREP(ARM_SMMU_TTBRn_ASID,	/* [한국어] 두 번째도 같은 값. */
						 cfg->asid);

			if (pgtbl_cfg->quirks & IO_PGTABLE_QUIRK_ARM_TTBR1)	/* [한국어] 높은 주소 공간을 쓰는 표면 */
				cb->ttbr[1] |= pgtbl_cfg->arm_lpae_s1_cfg.ttbr;	/* [한국어] TTBR1 에 표 주소를 얹고 */
			else
				cb->ttbr[0] |= pgtbl_cfg->arm_lpae_s1_cfg.ttbr;	/* [한국어] 아니면 TTBR0 에 얹는다. */
		}
	} else {
		cb->ttbr[0] = pgtbl_cfg->arm_lpae_s2_cfg.vttbr;	/* [한국어] 2단계는 전용 표 주소를 쓴다. */
	}

	/* MAIRs (stage-1 only) */
	if (stage1) {	/* [한국어] 메모리 속성 표는 1단계에만 있다. */
		if (cfg->fmt == ARM_SMMU_CTX_FMT_AARCH32_S) {	/* [한국어] 짧은 서술자는 속성 표 이름이 다르다. */
			cb->mair[0] = pgtbl_cfg->arm_v7s_cfg.prrr;	/* [한국어] v7s 는 속성 표 이름이 다르다. */
			cb->mair[1] = pgtbl_cfg->arm_v7s_cfg.nmrr;	/* [한국어] 두 번째 속성 표. */
		} else {
			cb->mair[0] = pgtbl_cfg->arm_lpae_s1_cfg.mair;	/* [한국어] 64비트 값의 하위 절반. */
			cb->mair[1] = pgtbl_cfg->arm_lpae_s1_cfg.mair >> 32;	/* [한국어] 상위 절반. 레지스터가 32비트씩 둘로 나뉘어 있다. */
		}
	}
}

/*
 * [한국어]
 * arm_smmu_write_context_bank - 뱅크 설정을 하드웨어에 실제로 쓴다
 *
 * @smmu: 대상 SMMU.
 * @idx: 뱅크 번호.
 *
 * 그림자 구조체의 내용을 레지스터에 옮긴다. 도메인을 만들 때와 전원이
 * 돌아왔을 때 부른다.
 *
 * cfg 가 NULL 이면 쓰이지 않는 뱅크라 SCTLR 만 0 으로 만들어 끈다.
 *
 * 원 주석이 순서의 이유를 밝힌다 — TCR 을 TTBR 보다 먼저 써야 한다.
 * TCR 의 설정이 TTBR 의 어떤 필드가 어떻게 읽히는지를 정하기 때문이다
 * (특히 ASID 의 상위 8비트).
 *
 * CBAR 에 가장 약한 공유·메모리 속성을 넣는 것도 원 주석이 설명한다 —
 * 그래야 페이지 테이블 항목이 정한 속성이 이기고, 여기서 정한 것에
 * 발목 잡히지 않는다.
 *
 * SCTLR 을 마지막에 쓰는 것이 요점이다. 그 안의 M 비트가 이 뱅크의 변환을
 * 켜므로, 모든 설정이 자리 잡은 뒤여야 한다.
 */
void arm_smmu_write_context_bank(struct arm_smmu_device *smmu, int idx)
{
	u32 reg;	/* [한국어] 만들어 쓸 레지스터 값. */
	bool stage1;	/* [한국어] 1단계가 있는 뱅크인가. */
	struct arm_smmu_cb *cb = &smmu->cbs[idx];	/* [한국어] 그림자 구조체. */
	struct arm_smmu_cfg *cfg = cb->cfg;	/* [한국어] 그것을 쓰는 도메인의 설정. */

	/* Unassigned context banks only need disabling */
	if (!cfg) {	/* [한국어] 쓰이지 않는 뱅크면 */
		arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_SCTLR, 0);	/* [한국어] 제어 레지스터를 0 으로 만들어 끄기만 하면 된다. */
		return;	/* [한국어] 더 할 일이 없다. */
	}

	stage1 = cfg->cbar != CBAR_TYPE_S2_TRANS;	/* [한국어] 2단계 전용이 아니면 1단계가 있다. */

	/* CBA2R */
	if (smmu->version > ARM_SMMU_V1) {	/* [한국어] 2판부터 있는 레지스터다. */
		if (cfg->fmt == ARM_SMMU_CTX_FMT_AARCH64)	/* [한국어] 64비트 형식이면 */
			reg = ARM_SMMU_CBA2R_VA64;	/* [한국어] 그 비트를 세우고 */
		else
			reg = 0;	/* [한국어] 아니면 32비트 형식이다. */
		/* 16-bit VMIDs live in CBA2R */
		if (smmu->features & ARM_SMMU_FEAT_VMID16)	/* [한국어] 16비트 VMID 를 쓰면 */
			reg |= FIELD_PREP(ARM_SMMU_CBA2R_VMID16, cfg->vmid);	/* [한국어] 원 주석대로 그 값은 이 레지스터에 담긴다. */

		arm_smmu_gr1_write(smmu, ARM_SMMU_GR1_CBA2R(idx), reg);	/* [한국어] 두 번째 속성 레지스터를 쓴다. */
	}

	/* CBAR */
	reg = FIELD_PREP(ARM_SMMU_CBAR_TYPE, cfg->cbar);	/* [한국어] 이 뱅크가 하는 변환의 종류. */
	if (smmu->version < ARM_SMMU_V2)	/* [한국어] 1판에서는 */
		reg |= FIELD_PREP(ARM_SMMU_CBAR_IRPTNDX, cfg->irptndx);	/* [한국어] 인터럽트 번호도 이 레지스터에 담는다. 2판은 뱅크마다 인터럽트가 정해져 있다. */

	/*
	 * Use the weakest shareability/memory types, so they are
	 * overridden by the ttbcr/pte.
	 */
	if (stage1) {	/* [한국어] 1단계면 우회 트래픽의 속성을 정하고, */
		reg |= FIELD_PREP(ARM_SMMU_CBAR_S1_BPSHCFG,	/* [한국어] 원 주석대로 가장 약한 공유 속성을 넣어 */
				  ARM_SMMU_CBAR_S1_BPSHCFG_NSH) |	/* [한국어] 표 항목이 정한 속성이 이기게 한다. */
		       FIELD_PREP(ARM_SMMU_CBAR_S1_MEMATTR,	/* [한국어] 메모리 속성도 마찬가지 이유로 */
				  ARM_SMMU_CBAR_S1_MEMATTR_WB);	/* [한국어] 되쓰기 캐시로 둔다. */
	} else if (!(smmu->features & ARM_SMMU_FEAT_VMID16)) {	/* [한국어] 2단계이고 8비트 VMID 를 쓰면 */
		/* 8-bit VMIDs live in CBAR */
		reg |= FIELD_PREP(ARM_SMMU_CBAR_VMID, cfg->vmid);	/* [한국어] 원 주석대로 그 값이 이 레지스터에 담긴다. */
	}
	arm_smmu_gr1_write(smmu, ARM_SMMU_GR1_CBAR(idx), reg);	/* [한국어] 속성 레지스터를 쓴다. */

	/*
	 * TCR
	 * We must write this before the TTBRs, since it determines the
	 * access behaviour of some fields (in particular, ASID[15:8]).
	 */
	if (stage1 && smmu->version > ARM_SMMU_V1)	/* [한국어] 2판의 1단계면 TCR2 가 있다. */
		arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_TCR2, cb->tcr[1]);	/* [한국어] 먼저 쓴다. */
	arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_TCR, cb->tcr[0]);	/* [한국어] 원 주석대로 TCR 을 TTBR 보다 먼저 써야 한다 — 그 설정이 TTBR 의 필드 해석을 정한다. */

	/* TTBRs */
	if (cfg->fmt == ARM_SMMU_CTX_FMT_AARCH32_S) {	/* [한국어] 32비트 짧은 서술자 형식이면 */
		arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_CONTEXTIDR, cfg->asid);	/* [한국어] ASID 가 별도 레지스터에 있다. */
		arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_TTBR0, cb->ttbr[0]);	/* [한국어] 표 주소는 32비트다. */
		arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_TTBR1, cb->ttbr[1]);	/* [한국어] 두 번째도. */
	} else {
		arm_smmu_cb_writeq(smmu, idx, ARM_SMMU_CB_TTBR0, cb->ttbr[0]);	/* [한국어] 긴 서술자 형식은 64비트로 한 번에 쓴다 — 표 주소와 ASID 가 한 레지스터에 있다. */
		if (stage1)	/* [한국어] 1단계면 */
			arm_smmu_cb_writeq(smmu, idx, ARM_SMMU_CB_TTBR1,	/* [한국어] 두 번째 표 주소도 쓴다. 2단계에는 없다. */
					   cb->ttbr[1]);
	}

	/* MAIRs (stage-1 only) */
	if (stage1) {	/* [한국어] 메모리 속성 표를 쓴다. */
		arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_S1_MAIR0, cb->mair[0]);	/* [한국어] 속성 표 하위. */
		arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_S1_MAIR1, cb->mair[1]);	/* [한국어] 상위. 2단계는 속성을 표 항목에 직접 담아 이 레지스터가 없다. */
	}

	/* SCTLR */
	reg = ARM_SMMU_SCTLR_CFIE | ARM_SMMU_SCTLR_CFRE | ARM_SMMU_SCTLR_AFE |	/* [한국어] 오류 보고와 인터럽트를 켜고, 접근 플래그와 TEX 재매핑을 쓰고, */
	      ARM_SMMU_SCTLR_TRE | ARM_SMMU_SCTLR_M;	/* [한국어] 마지막 M 비트가 이 뱅크의 변환을 켠다. */
	if (stage1)	/* [한국어] 1단계면 ASID 개인 이름 공간을 켠다. */
		reg |= ARM_SMMU_SCTLR_S1_ASIDPNE;	/* [한국어] 1단계면 ASID 를 뱅크마다 따로 쓴다. */
	if (IS_ENABLED(CONFIG_CPU_BIG_ENDIAN))	/* [한국어] 빅 엔디언 커널이면 */
		reg |= ARM_SMMU_SCTLR_E;	/* [한국어] 표도 그 순서로 읽게 한다. */

	if (smmu->impl && smmu->impl->write_sctlr)	/* [한국어] 구현체가 이 레지스터를 가로채면 */
		smmu->impl->write_sctlr(smmu, idx, reg);	/* [한국어] 그쪽에 맡긴다. */
	else
		arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_SCTLR, reg);	/* [한국어] 마지막에 쓴다 — 이 순간 뱅크가 살아난다. */
}

/*
 * [한국어]
 * arm_smmu_alloc_context_bank - 컨텍스트 뱅크를 하나 배정받는다
 *
 * @smmu_domain: 대상 도메인.
 * @smmu: 그 SMMU.
 * @dev: 붙는 장치.
 * @start: 찾기 시작할 번호.
 * @return: 배정받은 번호, 없으면 음수.
 *
 * 구현체가 배정 방식을 바꿀 수 있다 — 어떤 하드웨어는 특정 장치가 특정
 * 뱅크만 쓸 수 있다.
 *
 * start 가 있는 이유: 2단계 전용 뱅크가 앞쪽에 몰려 있어, 1단계 도메인은
 * 그 뒤에서 찾아야 한다.
 */
static int arm_smmu_alloc_context_bank(struct arm_smmu_domain *smmu_domain,
				       struct arm_smmu_device *smmu,
				       struct device *dev, unsigned int start)
{
	if (smmu->impl && smmu->impl->alloc_context_bank)	/* [한국어] 구현체가 배정 방식을 바꿨으면 */
		return smmu->impl->alloc_context_bank(smmu_domain, smmu, dev, start);	/* [한국어] 그쪽에 맡긴다. */

	return __arm_smmu_alloc_bitmap(smmu->context_map, start, smmu->num_context_banks);	/* [한국어] 아니면 비트맵에서 빈 자리를 원자적으로 잡는다. */
}

/*
 * [한국어]
 * arm_smmu_init_domain_context - 도메인에 컨텍스트 뱅크와 페이지 테이블을 마련한다
 *
 * @smmu_domain: 세울 도메인.
 * @smmu: 붙을 SMMU.
 * @dev: 첫 장치.
 * @return: 0 성공, 음수면 실패.
 *
 * 이 파일에서 가장 긴 함수이고, 도메인이 실제 하드웨어 자원과 이어지는
 * 자리다. 첫 장치를 붙일 때 한 번만 돈다 — 이미 세워졌으면 곧바로
 * 돌아간다.
 *
 * 단계 고르기가 원 주석의 표에 정리되어 있다. 규격이 1단계와 2단계를
 * 모두 갖추고도 중첩은 못 하는 하드웨어를 허용해, 조합이 복잡해졌다.
 * 사용자가 2단계를 직접 요청할 수는 없다는 마지막 문장이 중요하다.
 *
 * 표 형식 고르기는 더 까다롭다고 원 주석이 인정한다. 지금은 "시스템의
 * 나머지와 가장 가까운 것"을 고르고, 64비트 지원이 32비트 지원을 포함할
 * 것이라 가정한다. 그 판단을 io-pgtable 쪽으로 옮기는 편이 낫다는 말도
 * 함께 적어 두었다.
 *
 * 인터럽트 요청을 마지막에 하는 이유도 원 주석에 있다 — 처리기가 반쯤
 * 세워진 도메인을 보면 안 된다.
 *
 * 실패 경로가 두 갈래다. 뱅크를 잡기 전이면 락만 놓고, 잡은 뒤면
 * 그것까지 놓아야 한다.
 */
static int arm_smmu_init_domain_context(struct arm_smmu_domain *smmu_domain,
					struct arm_smmu_device *smmu,
					struct device *dev)
{
	int irq, start, ret = 0;	/* [한국어] 인터럽트 번호, 뱅크를 찾기 시작할 자리, 결과 코드. */
	unsigned long ias, oas;	/* [한국어] 입력·출력 주소 폭. */
	struct io_pgtable_ops *pgtbl_ops;	/* [한국어] 만들어질 표 조작 함수들. */
	struct io_pgtable_cfg pgtbl_cfg;	/* [한국어] 표를 만들 때 넘길 설정. */
	enum io_pgtable_fmt fmt;	/* [한국어] 고를 표 형식. */
	struct iommu_domain *domain = &smmu_domain->domain;	/* [한국어] 코어가 보는 도메인. */
	struct arm_smmu_cfg *cfg = &smmu_domain->cfg;	/* [한국어] 뱅크 설정. */
	irqreturn_t (*context_fault)(int irq, void *dev);	/* [한국어] 쓸 오류 처리기. */

	mutex_lock(&smmu_domain->init_mutex);	/* [한국어] 첫 붙임을 직렬화한다. */
	if (smmu_domain->smmu)	/* [한국어] 이미 세워졌으면 */
		goto out_unlock;	/* [한국어] 할 일이 없다. */

	/*
	 * Mapping the requested stage onto what we support is surprisingly
	 * complicated, mainly because the spec allows S1+S2 SMMUs without
	 * support for nested translation. That means we end up with the
	 * following table:
	 *
	 * Requested        Supported        Actual
	 *     S1               N              S1
	 *     S1             S1+S2            S1
	 *     S1               S2             S2
	 *     S1               S1             S1
	 *     N                N              N
	 *     N              S1+S2            S2
	 *     N                S2             S2
	 *     N                S1             S1
	 *
	 * Note that you can't actually request stage-2 mappings.
	 */
	if (!(smmu->features & ARM_SMMU_FEAT_TRANS_S1))	/* [한국어] 1단계를 못 하면 */
		smmu_domain->stage = ARM_SMMU_DOMAIN_S2;	/* [한국어] 2단계로 내린다. 원 주석의 표가 이 조합을 정리해 두었다. */
	if (!(smmu->features & ARM_SMMU_FEAT_TRANS_S2))	/* [한국어] 2단계를 못 하면 */
		smmu_domain->stage = ARM_SMMU_DOMAIN_S1;	/* [한국어] 1단계로 올린다. */

	/*
	 * Choosing a suitable context format is even more fiddly. Until we
	 * grow some way for the caller to express a preference, and/or move
	 * the decision into the io-pgtable code where it arguably belongs,
	 * just aim for the closest thing to the rest of the system, and hope
	 * that the hardware isn't esoteric enough that we can't assume AArch64
	 * support to be a superset of AArch32 support...
	 */
	if (smmu->features & ARM_SMMU_FEAT_FMT_AARCH32_L)	/* [한국어] 32비트 긴 서술자를 쓸 수 있으면 */
		cfg->fmt = ARM_SMMU_CTX_FMT_AARCH32_L;	/* [한국어] 일단 후보로 둔다. */
	if (IS_ENABLED(CONFIG_IOMMU_IO_PGTABLE_ARMV7S) &&	/* [한국어] 32비트 커널이고 LPAE 도 안 쓰며 */
	    !IS_ENABLED(CONFIG_64BIT) && !IS_ENABLED(CONFIG_ARM_LPAE) &&	/* [한국어] 하드웨어가 짧은 서술자를 지원하고 */
	    (smmu->features & ARM_SMMU_FEAT_FMT_AARCH32_S) &&	/* [한국어] 1단계 도메인이면 */
	    (smmu_domain->stage == ARM_SMMU_DOMAIN_S1))	/* [한국어] 시스템의 나머지와 가장 가까운 형식이다. */
		cfg->fmt = ARM_SMMU_CTX_FMT_AARCH32_S;	/* [한국어] 짧은 서술자를 고른다. */
	if ((IS_ENABLED(CONFIG_64BIT) || cfg->fmt == ARM_SMMU_CTX_FMT_NONE) &&	/* [한국어] 64비트 커널이거나 아직 아무것도 못 골랐고 */
	    (smmu->features & (ARM_SMMU_FEAT_FMT_AARCH64_64K |	/* [한국어] 64비트 형식 중 하나라도 되면 */
			       ARM_SMMU_FEAT_FMT_AARCH64_16K |
			       ARM_SMMU_FEAT_FMT_AARCH64_4K)))
		cfg->fmt = ARM_SMMU_CTX_FMT_AARCH64;	/* [한국어] 그것을 고른다. 원 주석대로 64비트 지원이 32비트를 포함한다고 가정한다. */

	if (cfg->fmt == ARM_SMMU_CTX_FMT_NONE) {	/* [한국어] 쓸 수 있는 형식이 하나도 없으면 */
		ret = -EINVAL;	/* [한국어] 이 하드웨어를 다룰 수 없다. */
		goto out_unlock;	/* [한국어] 락만 놓고 나간다. */
	}

	switch (smmu_domain->stage) {	/* [한국어] 정해진 단계에 따라 자원과 형식을 고른다. */
	case ARM_SMMU_DOMAIN_S1:	/* [한국어] 1단계 도메인. */
		cfg->cbar = CBAR_TYPE_S1_TRANS_S2_BYPASS;	/* [한국어] 1단계만 변환하고 2단계는 통과. */
		start = smmu->num_s2_context_banks;	/* [한국어] 2단계 전용 뱅크가 앞쪽에 몰려 있어 그 뒤에서 찾는다. */
		ias = smmu->va_size;	/* [한국어] 입력은 장치가 내는 가상 주소. */
		oas = smmu->ipa_size;	/* [한국어] 출력은 중간 물리 주소. */
		if (cfg->fmt == ARM_SMMU_CTX_FMT_AARCH64) {	/* [한국어] 64비트 형식이면 */
			fmt = ARM_64_LPAE_S1;	/* [한국어] 64비트 1단계 표. */
		} else if (cfg->fmt == ARM_SMMU_CTX_FMT_AARCH32_L) {	/* [한국어] 32비트 긴 서술자면 */
			fmt = ARM_32_LPAE_S1;	/* [한국어] 그 형식의 표를 쓰고 */
			ias = min(ias, 32UL);	/* [한국어] 주소 폭을 그 형식의 한계로 좁힌다. */
			oas = min(oas, 40UL);	/* [한국어] 출력은 40비트까지. */
		} else {
			fmt = ARM_V7S;	/* [한국어] 짧은 서술자 형식. */
			ias = min(ias, 32UL);	/* [한국어] 입출력 모두 32비트가 한계다. */
			oas = min(oas, 32UL);	/* [한국어] 같음. */
		}
		smmu_domain->flush_ops = &arm_smmu_s1_tlb_ops;	/* [한국어] 1단계 무효화 함수표. */
		break;
	case ARM_SMMU_DOMAIN_NESTED:	/* [한국어] 중첩 도메인은 */
		/*
		 * We will likely want to change this if/when KVM gets
		 * involved.
		 */
	case ARM_SMMU_DOMAIN_S2:	/* [한국어] 2단계와 같이 다룬다. 원 주석대로 KVM 이 얽히면 바뀔 자리다. */
		cfg->cbar = CBAR_TYPE_S2_TRANS;	/* [한국어] 2단계만 변환한다. */
		start = 0;	/* [한국어] 2단계 뱅크는 앞쪽부터 쓴다. */
		ias = smmu->ipa_size;	/* [한국어] 입력은 중간 물리 주소. */
		oas = smmu->pa_size;	/* [한국어] 출력은 실제 물리 주소. */
		if (cfg->fmt == ARM_SMMU_CTX_FMT_AARCH64) {	/* [한국어] 64비트 형식이면 */
			fmt = ARM_64_LPAE_S2;	/* [한국어] 64비트 2단계 표. */
		} else {
			fmt = ARM_32_LPAE_S2;	/* [한국어] 32비트 2단계 표. */
			ias = min(ias, 40UL);	/* [한국어] 그 형식의 한계. */
			oas = min(oas, 40UL);	/* [한국어] 같음. */
		}
		if (smmu->version == ARM_SMMU_V2)	/* [한국어] 2판이면 */
			smmu_domain->flush_ops = &arm_smmu_s2_tlb_ops_v2;	/* [한국어] 주소 단위 무효화를 쓰고 */
		else
			smmu_domain->flush_ops = &arm_smmu_s2_tlb_ops_v1;	/* [한국어] 1판이면 VMID 통째로 비운다. */
		break;
	default:	/* [한국어] 모르는 단계 값. */
		ret = -EINVAL;	/* [한국어] 모르는 단계. */
		goto out_unlock;	/* [한국어] 락만 놓고 나간다. */
	}

	ret = arm_smmu_alloc_context_bank(smmu_domain, smmu, dev, start);	/* [한국어] 뱅크를 하나 잡는다. */
	if (ret < 0) {	/* [한국어] 자리가 없다. */
		goto out_unlock;	/* [한국어] 락만 놓고 나간다. */
	}

	smmu_domain->smmu = smmu;	/* [한국어] 이 순간 도메인이 이 SMMU 에 매인다. */

	cfg->cbndx = ret;	/* [한국어] 배정받은 뱅크 번호. */
	if (smmu->version < ARM_SMMU_V2) {	/* [한국어] 1판은 인터럽트가 뱅크보다 적을 수 있어 */
		cfg->irptndx = atomic_inc_return(&smmu->irptndx);	/* [한국어] 돌려 가며 배정하고 */
		cfg->irptndx %= smmu->num_context_irqs;	/* [한국어] 개수로 나눈 나머지를 쓴다. */
	} else {
		cfg->irptndx = cfg->cbndx;	/* [한국어] 2판은 뱅크마다 인터럽트가 정해져 있다. */
	}

	if (smmu_domain->stage == ARM_SMMU_DOMAIN_S2)	/* [한국어] 2단계면 */
		cfg->vmid = cfg->cbndx + 1;	/* [한국어] VMID 를 쓴다. 0 은 예약이라 1 을 더한다. */
	else
		cfg->asid = cfg->cbndx;	/* [한국어] 1단계면 ASID 를 쓴다. 뱅크 번호를 그대로 쓰면 겹치지 않는다. */

	pgtbl_cfg = (struct io_pgtable_cfg) {	/* [한국어] 표를 만들 때 넘길 설정을 한 번에 짓는다. */
		.pgsize_bitmap	= smmu->pgsize_bitmap,	/* [한국어] 하드웨어가 지원하는 페이지 크기. */
		.ias		= ias,	/* [한국어] 입력 주소 폭. */
		.oas		= oas,	/* [한국어] 출력 주소 폭. */
		.coherent_walk	= smmu->features & ARM_SMMU_FEAT_COHERENT_WALK,	/* [한국어] 표를 캐시에 두어도 되는가. */
		.tlb		= smmu_domain->flush_ops,	/* [한국어] 표를 고칠 때 부를 무효화 함수들. */
		.iommu_dev	= smmu->dev,	/* [한국어] 표를 담을 메모리를 어느 장치 앞으로 잡을지. */
	};

	if (smmu->impl && smmu->impl->init_context) {	/* [한국어] 구현체가 설정을 손볼 수 있으면 */
		ret = smmu->impl->init_context(smmu_domain, &pgtbl_cfg, dev);	/* [한국어] 그쪽에 맡긴다. */
		if (ret)	/* [한국어] 구현체가 설정을 손보다 실패했다. */
			goto out_clear_smmu;	/* [한국어] 실패하면 뱅크까지 되돌린다. */
	}

	if (smmu_domain->pgtbl_quirks)	/* [한국어] 사용자가 예외 표시를 설정했으면 */
		pgtbl_cfg.quirks |= smmu_domain->pgtbl_quirks;	/* [한국어] 그것도 더한다. */

	pgtbl_ops = alloc_io_pgtable_ops(fmt, &pgtbl_cfg, smmu_domain);	/* [한국어] 실제 페이지 테이블을 만든다. */
	if (!pgtbl_ops) {	/* [한국어] 만들지 못했으면 */
		ret = -ENOMEM;	/* [한국어] 메모리 부족으로 본다. */
		goto out_clear_smmu;	/* [한국어] 뱅크를 되돌린다. */
	}

	/* Update the domain's page sizes to reflect the page table format */
	domain->pgsize_bitmap = pgtbl_cfg.pgsize_bitmap;	/* [한국어] 표가 실제로 쓸 수 있는 크기로 좁혀졌을 수 있어 다시 받는다. */

	if (pgtbl_cfg.quirks & IO_PGTABLE_QUIRK_ARM_TTBR1) {	/* [한국어] 높은 주소 공간을 쓰는 표면 */
		domain->geometry.aperture_start = ~0UL << ias;	/* [한국어] 주소 범위가 위쪽 끝에 붙는다. */
		domain->geometry.aperture_end = ~0UL;	/* [한국어] 끝은 최댓값. */
	} else {
		domain->geometry.aperture_end = (1UL << ias) - 1;	/* [한국어] 아니면 0 부터 그 폭까지. */
	}

	domain->geometry.force_aperture = true;	/* [한국어] 그 범위 밖의 매핑을 코어가 거절하게 한다. */

	/* Initialise the context bank with our page table cfg */
	arm_smmu_init_context_bank(smmu_domain, &pgtbl_cfg);	/* [한국어] 뱅크 설정을 그림자 구조체에 만들고 */
	arm_smmu_write_context_bank(smmu, cfg->cbndx);	/* [한국어] 하드웨어에 쓴다. */

	/*
	 * Request context fault interrupt. Do this last to avoid the
	 * handler seeing a half-initialised domain state.
	 */
	irq = smmu->irqs[cfg->irptndx];	/* [한국어] 이 뱅크가 쓸 인터럽트 번호. */

	if (smmu->impl && smmu->impl->context_fault)	/* [한국어] 구현체가 처리기를 바꿨으면 */
		context_fault = smmu->impl->context_fault;	/* [한국어] 그것을 쓰고 */
	else
		context_fault = arm_smmu_context_fault;	/* [한국어] 아니면 기본 처리기. */

	if (smmu->impl && smmu->impl->context_fault_needs_threaded_irq)	/* [한국어] 처리기가 잠들 수 있으면 */
		ret = devm_request_threaded_irq(smmu->dev, irq, NULL,	/* [한국어] 스레드 인터럽트로 등록한다. */
						context_fault,
						IRQF_ONESHOT | IRQF_SHARED,	/* [한국어] 스레드가 끝날 때까지 다시 울리지 않게 하고, 다른 뱅크와 나눠 쓴다. */
						"arm-smmu-context-fault",
						smmu_domain);
	else
		ret = devm_request_irq(smmu->dev, irq, context_fault, IRQF_SHARED,	/* [한국어] 아니면 보통 인터럽트로 등록한다. */
				       "arm-smmu-context-fault", smmu_domain);

	if (ret < 0) {	/* [한국어] 걸지 못했으면 */
		dev_err(smmu->dev, "failed to request context IRQ %d (%u)\n",	/* [한국어] 알리고 */
			cfg->irptndx, irq);
		cfg->irptndx = ARM_SMMU_INVALID_IRPTNDX;	/* [한국어] 없음으로 표시한다. 오류 보고만 못 할 뿐 변환은 된다. */
	}

	mutex_unlock(&smmu_domain->init_mutex);	/* [한국어] 락을 놓는다. */

	/* Publish page table ops for map/unmap */
	smmu_domain->pgtbl_ops = pgtbl_ops;	/* [한국어] 락 밖에서 공개한다 — 이 포인터가 보이는 순간 매핑 경로가 열린다. */
	return 0;	/* [한국어] 성공. */

out_clear_smmu:	/* [한국어] 뱅크를 잡은 뒤의 실패 경로. */
	__arm_smmu_free_bitmap(smmu->context_map, cfg->cbndx);	/* [한국어] 뱅크를 반납하고 */
	smmu_domain->smmu = NULL;	/* [한국어] 매인 적 없는 상태로 되돌린다. */
out_unlock:	/* [한국어] 모든 경로가 합류한다. */
	mutex_unlock(&smmu_domain->init_mutex);	/* [한국어] 락을 놓고 */
	return ret;	/* [한국어] 결과를 올린다. */
}

/*
 * [한국어]
 * arm_smmu_destroy_domain_context - 도메인의 하드웨어 자원을 놓는다
 *
 * @smmu_domain: 대상 도메인.
 *
 * 순서가 중요하다. 원 주석대로 뱅크를 먼저 꺼야 표를 해제할 수 있다 —
 * 켜져 있는 채로 표를 놓으면 하드웨어가 해제된 메모리를 읽는다.
 *
 * 전원을 켜고 하는 것에 주의. 꺼져 있으면 레지스터를 쓸 수 없다.
 */
static void arm_smmu_destroy_domain_context(struct arm_smmu_domain *smmu_domain)
{
	struct arm_smmu_device *smmu = smmu_domain->smmu;	/* [한국어] 이 도메인이 매인 SMMU. */
	struct arm_smmu_cfg *cfg = &smmu_domain->cfg;	/* [한국어] 뱅크 설정. */
	int ret, irq;	/* [한국어] 결과 코드와 인터럽트 번호. */

	if (!smmu)	/* [한국어] 한 번도 붙지 않은 도메인이면 */
		return;	/* [한국어] 놓을 자원이 없다. */

	ret = arm_smmu_rpm_get(smmu);	/* [한국어] 레지스터를 쓰려면 켜야 한다. */
	if (ret < 0)	/* [한국어] 켜지 못하면 */
		return;	/* [한국어] 뱅크를 끌 방법이 없다. 드문 경우다. */

	/*
	 * Disable the context bank and free the page tables before freeing
	 * it.
	 */
	smmu->cbs[cfg->cbndx].cfg = NULL;	/* [한국어] 쓰이지 않는 뱅크로 표시하고 */
	arm_smmu_write_context_bank(smmu, cfg->cbndx);	/* [한국어] 원 주석대로 표를 놓기 전에 하드웨어를 끈다. */

	if (cfg->irptndx != ARM_SMMU_INVALID_IRPTNDX) {	/* [한국어] 인터럽트를 잡았으면 */
		irq = smmu->irqs[cfg->irptndx];	/* [한국어] 그 번호를 찾아 */
		devm_free_irq(smmu->dev, irq, smmu_domain);	/* [한국어] 처리기를 뗀다. */
	}

	free_io_pgtable_ops(smmu_domain->pgtbl_ops);	/* [한국어] 이제 표를 놓아도 안전하다. */
	__arm_smmu_free_bitmap(smmu->context_map, cfg->cbndx);	/* [한국어] 뱅크를 반납한다. */

	arm_smmu_rpm_put(smmu);	/* [한국어] 전원 참조를 놓는다. */
}

/*
 * [한국어]
 * arm_smmu_domain_alloc_paging - 페이징 도메인을 만든다
 *
 * @dev: 요청한 장치.
 * @return: 만든 도메인, 실패하면 NULL.
 *
 * 원 주석대로 여기서는 거의 아무것도 하지 않는다. 실제 자원은 첫 장치를
 * 붙일 때 잡는다 — 그전에는 어느 SMMU 에 매일지도 모르기 때문이다.
 *
 * 페이지 크기 비트맵만 미리 채워 둔다. 코어가 그것을 보고 매핑을 쪼개므로,
 * 붙기 전에도 있어야 한다.
 */
static struct iommu_domain *arm_smmu_domain_alloc_paging(struct device *dev)
{
	struct arm_smmu_domain *smmu_domain;	/* [한국어] 만들 도메인. */
	struct arm_smmu_master_cfg *cfg = dev_iommu_priv_get(dev);	/* [한국어] 요청한 장치의 매핑 상태. */
	struct arm_smmu_device *smmu = cfg->smmu;	/* [한국어] 그 장치가 매인 SMMU. */

	/*
	 * Allocate the domain and initialise some of its data structures.
	 * We can't really do anything meaningful until we've added a
	 * master.
	 */
	smmu_domain = kzalloc_obj(*smmu_domain);	/* [한국어] 도메인 구조체. */
	if (!smmu_domain)	/* [한국어] 메모리가 없다. */
		return NULL;	/* [한국어] 실패. */

	mutex_init(&smmu_domain->init_mutex);	/* [한국어] 첫 붙임을 직렬화할 뮤텍스. */
	spin_lock_init(&smmu_domain->cb_lock);	/* [한국어] 주소 변환과 동기화를 직렬화할 스핀락. */
	smmu_domain->domain.pgsize_bitmap = smmu->pgsize_bitmap;	/* [한국어] 코어가 매핑을 쪼갤 단위를 알아야 하므로 미리 채운다. */

	return &smmu_domain->domain;	/* [한국어] 코어가 보는 도메인. */
}

/*
 * [한국어]
 * arm_smmu_domain_free - 도메인을 해제한다
 *
 * @domain: 해제할 도메인.
 *
 * 원 주석대로 이 시점에 모든 장치가 이미 떨어져 있다고 가정한다 —
 * 코어가 그것을 보장한다.
 */
static void arm_smmu_domain_free(struct iommu_domain *domain)
{
	struct arm_smmu_domain *smmu_domain = to_smmu_domain(domain);	/* [한국어] 이 드라이버의 도메인으로 되짚는다. */

	/*
	 * Free the domain resources. We assume that all devices have
	 * already been detached.
	 */
	arm_smmu_destroy_domain_context(smmu_domain);	/* [한국어] 하드웨어 자원을 놓는다. */
	kfree(smmu_domain);	/* [한국어] 구조체를 해제한다. */
}

/*
 * [한국어]
 * arm_smmu_write_smr - 스트림 매칭 레지스터 하나를 쓴다
 *
 * @smmu: 대상 SMMU.
 * @idx: 항목 번호.
 *
 * 확장 스트림 id 모드에서는 유효 비트가 이 레지스터에 없다 — 그 자리가
 * id 의 상위 비트로 쓰이기 때문이다. 그때는 S2CR 쪽의 EXIDVALID 가 그
 * 역할을 한다.
 */
static void arm_smmu_write_smr(struct arm_smmu_device *smmu, int idx)
{
	struct arm_smmu_smr *smr = smmu->smrs + idx;	/* [한국어] 그림자 상태. */
	u32 reg = FIELD_PREP(ARM_SMMU_SMR_ID, smr->id) |	/* [한국어] 받아들일 id 와 */
		  FIELD_PREP(ARM_SMMU_SMR_MASK, smr->mask);	/* [한국어] 무시할 비트들. */

	if (!(smmu->features & ARM_SMMU_FEAT_EXIDS) && smr->valid)	/* [한국어] 확장 id 모드가 아닐 때만 여기에 유효 비트를 세운다 — 그 모드에서는 이 자리가 id 의 상위 비트다. */
		reg |= ARM_SMMU_SMR_VALID;	/* [한국어] 유효 표시. */
	arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_SMR(idx), reg);	/* [한국어] 레지스터에 쓴다. */
}

/*
 * [한국어]
 * arm_smmu_write_s2cr - 스트림-컨텍스트 레지스터 하나를 쓴다
 *
 * @smmu: 대상 SMMU.
 * @idx: 항목 번호.
 *
 * 이 항목에 걸린 스트림을 어디로 보낼지 정한다 — 변환할지, 그냥
 * 통과시킬지, 오류로 만들지.
 *
 * 확장 id 모드에서 유효 비트를 여기에 세운다. SMR 쪽 자리를 id 가
 * 차지했기 때문이다.
 *
 * 구현체가 이 레지스터를 가로챌 수 있다 — 퀄컴은 규격에 없는 비트를
 * 더 쓴다.
 */
static void arm_smmu_write_s2cr(struct arm_smmu_device *smmu, int idx)
{
	struct arm_smmu_s2cr *s2cr = smmu->s2crs + idx;	/* [한국어] 그림자 상태. */
	u32 reg;	/* [한국어] 만들어 쓸 값. */

	if (smmu->impl && smmu->impl->write_s2cr) {	/* [한국어] 구현체가 가로채면 */
		smmu->impl->write_s2cr(smmu, idx);	/* [한국어] 그쪽에 맡기고 */
		return;	/* [한국어] 돌아간다. */
	}

	reg = FIELD_PREP(ARM_SMMU_S2CR_TYPE, s2cr->type) |	/* [한국어] 변환·우회·오류 중 무엇으로 다룰지, */
	      FIELD_PREP(ARM_SMMU_S2CR_CBNDX, s2cr->cbndx) |	/* [한국어] 변환이면 어느 뱅크로 보낼지, */
	      FIELD_PREP(ARM_SMMU_S2CR_PRIVCFG, s2cr->privcfg);	/* [한국어] 특권 속성을 어떻게 다룰지. */

	if (smmu->features & ARM_SMMU_FEAT_EXIDS && smmu->smrs &&	/* [한국어] 확장 id 모드면 유효 비트가 이쪽에 있다. */
	    smmu->smrs[idx].valid)	/* [한국어] 짝이 되는 SMR 이 유효할 때만 세운다. */
		reg |= ARM_SMMU_S2CR_EXIDVALID;	/* [한국어] 확장 모드의 유효 표시. */
	arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_S2CR(idx), reg);	/* [한국어] 레지스터에 쓴다. */
}

/*
 * [한국어]
 * arm_smmu_write_sme - 매핑 항목 하나를 통째로 쓴다
 *
 * @smmu: 대상 SMMU.
 * @idx: 항목 번호.
 *
 * S2CR 을 먼저 쓰는 순서가 중요하다. SMR 을 유효하게 만드는 순간부터
 * 그 스트림이 매칭되므로, 갈 곳이 먼저 정해져 있어야 한다.
 *
 * 스트림 매칭이 없는 하드웨어에서는 SMR 이 아예 없다.
 */
static void arm_smmu_write_sme(struct arm_smmu_device *smmu, int idx)
{
	arm_smmu_write_s2cr(smmu, idx);	/* [한국어] 갈 곳을 먼저 정한다. */
	if (smmu->smrs)	/* [한국어] 스트림 매칭이 있는 하드웨어면 */
		arm_smmu_write_smr(smmu, idx);	/* [한국어] 그 뒤에 매칭을 켠다. 순서가 반대면 갈 곳이 정해지기 전에 스트림이 걸린다. */
}

/*
 * The width of SMR's mask field depends on sCR0_EXIDENABLE, so this function
 * should be called after sCR0 is written.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * arm_smmu_test_smr_masks - 실제로 쓸 수 있는 id·마스크 폭을 실험으로 알아낸다
 *
 * @smmu: 대상 SMMU.
 *
 * 규격이 알려 주지 않는 값을 알아내는 방법이다 — 전부 1 을 써 보고
 * 되읽어, 남은 비트가 곧 구현된 폭이다.
 *
 * 원 주석대로 sCR0 를 쓴 뒤에 불러야 한다. 확장 id 설정이 마스크 필드의
 * 폭을 바꾸기 때문이다.
 *
 * 빈 항목을 찾아 쓰는 것이 요점이다. 원 주석이 재미있는 지적을 한다 —
 * 빈 항목이 하나도 없다면 이 실험 없이도 된다. 그때 쓸 수 있는 것은
 * 이미 믿고 있는 id·마스크 값뿐이기 때문이다.
 *
 * id 와 마스크를 따로 시험하는 이유도 원 주석에 있다: 마스크 비트가 서
 * 있으면 대응하는 id 비트가 보존되지 않을 수 있다.
 */
static void arm_smmu_test_smr_masks(struct arm_smmu_device *smmu)
{
	u32 smr;	/* [한국어] 시험용으로 쓰고 되읽을 값. */
	int i;	/* [한국어] 빈 항목을 찾는 첨자. */

	if (!smmu->smrs)	/* [한국어] 스트림 매칭이 없으면 */
		return;	/* [한국어] 시험할 것이 없다. */
	/*
	 * If we've had to accommodate firmware memory regions, we may
	 * have live SMRs by now; tread carefully...
	 *
	 * Somewhat perversely, not having a free SMR for this test implies we
	 * can get away without it anyway, as we'll only be able to 'allocate'
	 * these SMRs for the ID/mask values we're already trusting to be OK.
	 */
	for (i = 0; i < smmu->num_mapping_groups; i++)	/* [한국어] 빈 항목을 찾는다. */
		if (!smmu->smrs[i].valid)	/* [한국어] 비어 있는 것을 찾으면 */
			goto smr_ok;	/* [한국어] 그것으로 시험한다. */
	return;	/* [한국어] 원 주석대로 빈 항목이 없으면 이 시험 없이도 된다. */
smr_ok:	/* [한국어] 시험에 쓸 항목을 찾았다. */
	/*
	 * SMR.ID bits may not be preserved if the corresponding MASK
	 * bits are set, so check each one separately. We can reject
	 * masters later if they try to claim IDs outside these masks.
	 */
	smr = FIELD_PREP(ARM_SMMU_SMR_ID, smmu->streamid_mask);	/* [한국어] id 자리에 전부 1 을 써 보고 */
	arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_SMR(i), smr);	/* [한국어] 레지스터에 쓴 뒤 */
	smr = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_SMR(i));	/* [한국어] 되읽는다. */
	smmu->streamid_mask = FIELD_GET(ARM_SMMU_SMR_ID, smr);	/* [한국어] 남은 비트가 곧 구현된 폭이다. */

	smr = FIELD_PREP(ARM_SMMU_SMR_MASK, smmu->streamid_mask);	/* [한국어] 마스크 자리도 같은 방법으로. 원 주석대로 따로 시험해야 한다. */
	arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_SMR(i), smr);	/* [한국어] 써 보고 */
	smr = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_SMR(i));	/* [한국어] 되읽어 */
	smmu->smr_mask_mask = FIELD_GET(ARM_SMMU_SMR_MASK, smr);	/* [한국어] 실제 마스크 폭을 알아낸다. */
}

/*
 * [한국어]
 * arm_smmu_find_sme - 이 id·마스크에 맞는 매핑 항목을 찾거나 빈 자리를 고른다
 *
 * @smmu: 대상 SMMU.
 * @id: 스트림 id.
 * @mask: 그 마스크.
 * @return: 쓸 항목 번호, 충돌하면 -EINVAL, 자리가 없으면 -ENOSPC.
 *
 * 스트림 인덱싱 하드웨어는 id 가 곧 항목 번호라 고민할 것이 없다 —
 * 원 주석의 "blissfully easy".
 *
 * 매칭 하드웨어는 세 갈래로 판단한다.
 *
 * 기존 항목이 새 항목을 온전히 덮으면 그것을 함께 쓴다. 원 주석대로 그
 * 경우 뒤에 충돌하는 항목이 있을 수 없다는 보장이 따라온다.
 *
 * 온전히 덮지 않으면서 겹치면 거절한다. 원 주석대로 그런 겹침이 있으면
 * 두 항목 모두에 걸리는 스트림 id 가 반드시 존재하고, 그 위험을 허용할
 * 수 없다.
 *
 * 아무것도 걸리지 않으면 처음 만난 빈 자리를 쓴다.
 */
static int arm_smmu_find_sme(struct arm_smmu_device *smmu, u16 id, u16 mask)
{
	struct arm_smmu_smr *smrs = smmu->smrs;	/* [한국어] 매칭 항목 배열. */
	int i, free_idx = -ENOSPC;	/* [한국어] 순회 첨자와, 처음 만난 빈 자리. */

	/* Stream indexing is blissfully easy */
	if (!smrs)	/* [한국어] 스트림 인덱싱 하드웨어면 */
		return id;	/* [한국어] id 가 곧 항목 번호다. */

	/* Validating SMRs is... less so */
	for (i = 0; i < smmu->num_mapping_groups; ++i) {	/* [한국어] 모든 항목을 훑는다. */
		if (!smrs[i].valid) {	/* [한국어] 비어 있으면 */
			/*
			 * Note the first free entry we come across, which
			 * we'll claim in the end if nothing else matches.
			 */
			if (free_idx < 0)	/* [한국어] 아직 빈 자리를 못 봤을 때만 */
				free_idx = i;	/* [한국어] 기억해 둔다. 원 주석대로 아무것도 맞지 않으면 이것을 쓴다. */
			continue;	/* [한국어] 다음 항목으로. */
		}
		/*
		 * If the new entry is _entirely_ matched by an existing entry,
		 * then reuse that, with the guarantee that there also cannot
		 * be any subsequent conflicting entries. In normal use we'd
		 * expect simply identical entries for this case, but there's
		 * no harm in accommodating the generalisation.
		 */
		if ((mask & smrs[i].mask) == mask &&	/* [한국어] 기존 항목이 새 것을 온전히 덮고 */
		    !((id ^ smrs[i].id) & ~smrs[i].mask))	/* [한국어] id 도 그 마스크 아래에서 같으면 */
			return i;	/* [한국어] 함께 쓴다. 원 주석대로 그러면 뒤에 충돌이 없다는 보장이 따라온다. */
		/*
		 * If the new entry has any other overlap with an existing one,
		 * though, then there always exists at least one stream ID
		 * which would cause a conflict, and we can't allow that risk.
		 */
		if (!((id ^ smrs[i].id) & ~(smrs[i].mask | mask)))	/* [한국어] 온전히 덮지 않으면서 겹치면 */
			return -EINVAL;	/* [한국어] 거절한다. 두 항목 모두에 걸리는 id 가 반드시 있다. */
	}

	return free_idx;	/* [한국어] 아무것도 걸리지 않았으면 빈 자리를 쓴다. */
}

/*
 * [한국어]
 * arm_smmu_free_sme - 매핑 항목의 참조를 하나 놓는다
 *
 * @smmu: 대상 SMMU.
 * @idx: 항목 번호.
 * @return: 실제로 비워졌으면 참.
 *
 * 여러 장치가 한 항목을 나눠 쓸 수 있어 세어 둔다. 마지막이 떠날 때만
 * 초기값으로 되돌린다.
 *
 * 참을 돌려주면 호출자가 하드웨어에 그 변화를 써야 한다.
 */
static bool arm_smmu_free_sme(struct arm_smmu_device *smmu, int idx)
{
	if (--smmu->s2crs[idx].count)	/* [한국어] 아직 쓰는 장치가 남았으면 */
		return false;	/* [한국어] 그대로 둔다. */

	smmu->s2crs[idx] = s2cr_init_val;	/* [한국어] 초기값으로 되돌린다 — 설정에 따라 우회이거나 오류다. */
	if (smmu->smrs)	/* [한국어] 스트림 매칭이 있으면 */
		smmu->smrs[idx].valid = false;	/* [한국어] 매칭도 끈다. */

	return true;	/* [한국어] 하드웨어에 써야 한다고 알린다. */
}

/*
 * [한국어]
 * arm_smmu_master_alloc_smes - 장치의 스트림 id 마다 매핑 항목을 배정한다
 *
 * @dev: 대상 장치.
 * @return: 0 성공, 음수면 실패.
 *
 * 두 걸음으로 나뉜다. 먼저 모든 id 에 대해 자리를 정하고 그림자 상태를
 * 채운 뒤, 그것이 모두 성공했을 때만 하드웨어를 건드린다.
 *
 * 그렇게 나눈 이유: 중간에 실패하면 아무것도 하지 않은 상태로 되돌려야
 * 하는데, 하드웨어를 이미 건드렸으면 그 사이 장치가 DMA 를 낼 수 있다.
 *
 * 이미 배정된 id 를 다시 배정하려 하면 -EEXIST 다 — 같은 장치를 두 번
 * 등록하는 것은 버그다.
 */
static int arm_smmu_master_alloc_smes(struct device *dev)
{
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);	/* [한국어] 이 장치의 스트림 id 목록. */
	struct arm_smmu_master_cfg *cfg = dev_iommu_priv_get(dev);	/* [한국어] 그 장치의 매핑 상태. */
	struct arm_smmu_device *smmu = cfg->smmu;	/* [한국어] 매인 SMMU. */
	struct arm_smmu_smr *smrs = smmu->smrs;	/* [한국어] 매칭 항목 배열(없을 수 있다). */
	int i, idx, ret;	/* [한국어] 순회 첨자, 항목 번호, 결과. */

	mutex_lock(&smmu->stream_map_mutex);	/* [한국어] 항목 배정을 직렬화한다. */
	/* Figure out a viable stream map entry allocation */
	for_each_cfg_sme(cfg, fwspec, i, idx) {	/* [한국어] 이 장치의 스트림 id 마다. */
		u16 sid = FIELD_GET(ARM_SMMU_SMR_ID, fwspec->ids[i]);	/* [한국어] 한 값에 담긴 id 를 꺼낸다. */
		u16 mask = FIELD_GET(ARM_SMMU_SMR_MASK, fwspec->ids[i]);	/* [한국어] 마스크도 함께. */

		if (idx != INVALID_SMENDX) {	/* [한국어] 이미 배정돼 있으면 */
			ret = -EEXIST;	/* [한국어] 같은 장치를 두 번 등록하는 버그다. */
			goto out_err;	/* [한국어] 되돌린다. */
		}

		ret = arm_smmu_find_sme(smmu, sid, mask);	/* [한국어] 쓸 항목을 찾는다. */
		if (ret < 0)	/* [한국어] 충돌하거나 자리가 없다. */
			goto out_err;	/* [한국어] 되돌린다. */

		idx = ret;	/* [한국어] 찾은 항목 번호. */
		if (smrs && smmu->s2crs[idx].count == 0) {	/* [한국어] 그 항목의 첫 사용자면 */
			smrs[idx].id = sid;	/* [한국어] id 와 */
			smrs[idx].mask = mask;	/* [한국어] 마스크를 채우고 */
			smrs[idx].valid = true;	/* [한국어] 유효로 표시한다. 두 번째 사용자는 이미 채워진 것을 쓴다. */
		}
		smmu->s2crs[idx].count++;	/* [한국어] 사용자 수를 늘린다. */
		cfg->smendx[i] = (s16)idx;	/* [한국어] 이 id 가 쓰는 항목을 기억한다. */
	}

	/* It worked! Now, poke the actual hardware */
	for_each_cfg_sme(cfg, fwspec, i, idx)	/* [한국어] 모두 성공했으니 */
		arm_smmu_write_sme(smmu, idx);	/* [한국어] 이제야 하드웨어를 건드린다. */

	mutex_unlock(&smmu->stream_map_mutex);	/* [한국어] 락을 놓는다. */
	return 0;	/* [한국어] 성공. */

out_err:	/* [한국어] 실패 경로. */
	while (i--) {	/* [한국어] 앞서 배정한 것들을 */
		arm_smmu_free_sme(smmu, cfg->smendx[i]);	/* [한국어] 되돌리고 */
		cfg->smendx[i] = INVALID_SMENDX;	/* [한국어] 표시도 지운다. 하드웨어는 아직 건드리지 않았으므로 이것으로 충분하다. */
	}
	mutex_unlock(&smmu->stream_map_mutex);	/* [한국어] 락을 놓고 */
	return ret;	/* [한국어] 실패를 올린다. */
}

/*
 * [한국어]
 * arm_smmu_master_free_smes - 장치가 쓰던 매핑 항목들을 놓는다
 *
 * @cfg: 그 장치의 매핑 상태.
 * @fwspec: 그 장치의 스트림 id 목록.
 *
 * 마지막 참조가 사라진 항목만 하드웨어에서 지운다.
 */
static void arm_smmu_master_free_smes(struct arm_smmu_master_cfg *cfg,
				      struct iommu_fwspec *fwspec)
{
	struct arm_smmu_device *smmu = cfg->smmu;	/* [한국어] 매인 SMMU. */
	int i, idx;	/* [한국어] 순회 첨자와 항목 번호. */

	mutex_lock(&smmu->stream_map_mutex);	/* [한국어] 항목 상태를 지키는 락. */
	for_each_cfg_sme(cfg, fwspec, i, idx) {	/* [한국어] 이 장치가 쓰던 항목마다. */
		if (arm_smmu_free_sme(smmu, idx))	/* [한국어] 마지막 사용자였으면 */
			arm_smmu_write_sme(smmu, idx);	/* [한국어] 하드웨어에서도 지운다. */
		cfg->smendx[i] = INVALID_SMENDX;	/* [한국어] 배정 표시를 지운다. */
	}
	mutex_unlock(&smmu->stream_map_mutex);	/* [한국어] 락을 놓는다. */
}

/*
 * [한국어]
 * arm_smmu_master_install_s2crs - 장치의 항목들을 어디로 보낼지 정한다
 *
 * @cfg: 그 장치의 매핑 상태.
 * @type: 변환·우회·오류 중 무엇으로 할지.
 * @cbndx: 변환일 때 보낼 컨텍스트 뱅크 번호.
 * @fwspec: 스트림 id 목록.
 *
 * 이미 그 상태인 항목은 건너뛴다 — 같은 값을 다시 쓰면 하드웨어가
 * 잠시 흔들릴 수 있고, 쓸 이유도 없다.
 *
 * 이 함수 하나가 붙이기·항등·차단을 모두 처리한다. 셋의 차이는 type 과
 * cbndx 뿐이다.
 */
static void arm_smmu_master_install_s2crs(struct arm_smmu_master_cfg *cfg,
					  enum arm_smmu_s2cr_type type,
					  u8 cbndx, struct iommu_fwspec *fwspec)
{
	struct arm_smmu_device *smmu = cfg->smmu;	/* [한국어] 매인 SMMU. */
	struct arm_smmu_s2cr *s2cr = smmu->s2crs;	/* [한국어] 항목 배열. */
	int i, idx;	/* [한국어] 순회 첨자와 항목 번호. */

	for_each_cfg_sme(cfg, fwspec, i, idx) {	/* [한국어] 이 장치의 항목마다. */
		if (type == s2cr[idx].type && cbndx == s2cr[idx].cbndx)	/* [한국어] 이미 그 상태면 */
			continue;	/* [한국어] 건드리지 않는다 — 같은 값을 다시 쓰면 하드웨어가 잠시 흔들릴 수 있다. */

		s2cr[idx].type = type;	/* [한국어] 변환·우회·오류 중 무엇으로 다룰지. */
		s2cr[idx].privcfg = S2CR_PRIVCFG_DEFAULT;	/* [한국어] 특권 속성은 장치가 보낸 것을 그대로 쓴다. */
		s2cr[idx].cbndx = cbndx;	/* [한국어] 변환이면 보낼 뱅크. */
		arm_smmu_write_s2cr(smmu, idx);	/* [한국어] 하드웨어에 쓴다. */
	}
}

/*
 * [한국어]
 * arm_smmu_attach_dev - 장치를 도메인에 붙인다
 *
 * @domain: 붙일 도메인.
 * @dev: 붙는 장치.
 * @old: 붙어 있던 도메인(여기서는 쓰지 않는다).
 * @return: 0 성공, 음수면 실패.
 *
 * 원 주석의 FIXME 가 cfg 검사의 이유를 밝힌다 — arch/arm 의 옛 DMA API
 * 코드가 of_xlate 와 probe_device 사이에 자기 도메인을 붙이려 든다. 그것을
 * 다룰 방법이 없어, NULL 을 역참조하는 대신 정중히 거절한다.
 *
 * 도메인이 아직 세워지지 않았으면 여기서 세운다 — 첫 장치가 붙는 순간이
 * 곧 그 도메인이 어느 SMMU 에 매이는 순간이다.
 *
 * 서로 다른 SMMU 에 걸친 도메인은 지원하지 않는다. 페이지 테이블은 나눌
 * 수 있어도 컨텍스트 뱅크는 하드웨어마다 따로이기 때문이다.
 */
static int arm_smmu_attach_dev(struct iommu_domain *domain, struct device *dev,
			       struct iommu_domain *old)
{
	struct arm_smmu_domain *smmu_domain = to_smmu_domain(domain);	/* [한국어] 이 드라이버의 도메인. */
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);	/* [한국어] 장치의 스트림 id 목록. */
	struct arm_smmu_master_cfg *cfg;	/* [한국어] 그 장치의 매핑 상태. */
	struct arm_smmu_device *smmu;	/* [한국어] 매인 SMMU. */
	int ret;	/* [한국어] 결과 코드. */

	/*
	 * FIXME: The arch/arm DMA API code tries to attach devices to its own
	 * domains between of_xlate() and probe_device() - we have no way to cope
	 * with that, so until ARM gets converted to rely on groups and default
	 * domains, just say no (but more politely than by dereferencing NULL).
	 * This should be at least a WARN_ON once that's sorted.
	 */
	cfg = dev_iommu_priv_get(dev);	/* [한국어] 원 주석의 FIXME 대로, 아직 probe 되지 않은 장치가 올 수 있다. */
	if (!cfg)	/* [한국어] 그러면 다룰 방법이 없다. */
		return -ENODEV;	/* [한국어] 정중히 거절한다. */

	smmu = cfg->smmu;	/* [한국어] 매인 SMMU. */

	ret = arm_smmu_rpm_get(smmu);	/* [한국어] 레지스터를 쓰려면 켜야 한다. */
	if (ret < 0)	/* [한국어] 켜지 못하면 */
		return ret;	/* [한국어] 붙일 수 없다. */

	/* Ensure that the domain is finalised */
	ret = arm_smmu_init_domain_context(smmu_domain, smmu, dev);	/* [한국어] 아직 세워지지 않았으면 여기서 세운다. */
	if (ret < 0)	/* [한국어] 세우지 못하면 */
		goto rpm_put;	/* [한국어] 전원 참조를 놓고 나간다. */

	/*
	 * Sanity check the domain. We don't support domains across
	 * different SMMUs.
	 */
	if (smmu_domain->smmu != smmu) {	/* [한국어] 다른 SMMU 에 매인 도메인이면 */
		ret = -EINVAL;	/* [한국어] 거절한다. 컨텍스트 뱅크는 하드웨어마다 따로다. */
		goto rpm_put;	/* [한국어] 나간다. */
	}

	/* Looks ok, so add the device to the domain */
	arm_smmu_master_install_s2crs(cfg, S2CR_TYPE_TRANS,	/* [한국어] 이 장치의 항목들을 그 뱅크로 향하게 한다. */
				      smmu_domain->cfg.cbndx, fwspec);
rpm_put:	/* [한국어] 성공과 실패가 함께 지나는 지점. */
	arm_smmu_rpm_put(smmu);	/* [한국어] 전원 참조를 놓는다. */
	return ret;	/* [한국어] 결과. */
}

/*
 * [한국어]
 * arm_smmu_attach_dev_type - 항등·차단 도메인에 붙이는 공통 몸통
 *
 * @dev: 붙는 장치.
 * @type: S2CR 종류(우회 또는 오류).
 * @return: 0 성공, 음수면 실패.
 *
 * 두 도메인 모두 컨텍스트 뱅크가 필요 없어, 항목의 종류만 바꾸면 된다.
 * 그래서 뱅크 번호로 0 을 넘긴다 — 쓰이지 않는 값이다.
 */
static int arm_smmu_attach_dev_type(struct device *dev,
				    enum arm_smmu_s2cr_type type)
{
	struct arm_smmu_master_cfg *cfg = dev_iommu_priv_get(dev);	/* [한국어] 장치의 매핑 상태. */
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);	/* [한국어] 스트림 id 목록. */
	struct arm_smmu_device *smmu;	/* [한국어] 매인 SMMU. */
	int ret;	/* [한국어] 결과 코드. */

	if (!cfg)	/* [한국어] 아직 probe 되지 않았으면 */
		return -ENODEV;	/* [한국어] 다룰 수 없다. */
	smmu = cfg->smmu;	/* [한국어] 매인 SMMU. */

	ret = arm_smmu_rpm_get(smmu);	/* [한국어] 레지스터를 쓰려면 켜야 한다. */
	if (ret < 0)	/* [한국어] 켜지 못하면 */
		return ret;	/* [한국어] 붙일 수 없다. */

	arm_smmu_master_install_s2crs(cfg, type, 0, fwspec);	/* [한국어] 뱅크가 필요 없으므로 번호로 0 을 넘긴다 — 쓰이지 않는 값이다. */
	arm_smmu_rpm_put(smmu);	/* [한국어] 전원 참조를 놓는다. */
	return 0;	/* [한국어] 늘 성공한다. */
}

/*
 * [한국어]
 * arm_smmu_attach_dev_identity - 항등 도메인에 붙인다
 *
 * @domain: 항등 도메인(쓰지 않는다).
 * @dev: 붙는 장치.
 * @old: 옛 도메인(쓰지 않는다).
 * @return: 0 성공, 음수면 실패.
 *
 * 우회로 만들어 변환 없이 통과시킨다. 그 장치는 물리 주소를 직접 쓰게
 * 되므로 보호받지 못한다.
 */
static int arm_smmu_attach_dev_identity(struct iommu_domain *domain,
					struct device *dev,
					struct iommu_domain *old)
{
	return arm_smmu_attach_dev_type(dev, S2CR_TYPE_BYPASS);	/* [한국어] 우회로 만들어 변환 없이 통과시킨다. */
}

/*
 * [한국어] 항등 도메인의 연산표. 붙이기만 있으면 된다 — 매핑이 없다.
 */
static const struct iommu_domain_ops arm_smmu_identity_ops = {
	.attach_dev = arm_smmu_attach_dev_identity,	/* [한국어] 붙이기만 있으면 된다. */
};

/*
 * [한국어] 하나뿐인 항등 도메인.
 *
 * 정적으로 두는 이유: 내용이 없어 하드웨어마다 만들 이유가 없고,
 * 할당이 없으니 실패하지 않는다.
 */
static struct iommu_domain arm_smmu_identity_domain = {
	.type = IOMMU_DOMAIN_IDENTITY,	/* [한국어] 항등 종류. 변환 없이 통과시킨다. */
	.ops = &arm_smmu_identity_ops,
};

/*
 * [한국어]
 * arm_smmu_attach_dev_blocked - 차단 도메인에 붙인다
 *
 * @domain: 차단 도메인(쓰지 않는다).
 * @dev: 붙는 장치.
 * @old: 옛 도메인(쓰지 않는다).
 * @return: 0 성공, 음수면 실패.
 *
 * 그 장치의 모든 접근을 오류로 만든다.
 */
static int arm_smmu_attach_dev_blocked(struct iommu_domain *domain,
				       struct device *dev,
				       struct iommu_domain *old)
{
	return arm_smmu_attach_dev_type(dev, S2CR_TYPE_FAULT);	/* [한국어] 모든 접근을 오류로 만든다. */
}

/*
 * [한국어] 차단 도메인의 연산표.
 */
static const struct iommu_domain_ops arm_smmu_blocked_ops = {
	.attach_dev = arm_smmu_attach_dev_blocked,	/* [한국어] 붙이기만 있으면 된다. */
};

/*
 * [한국어] 하나뿐인 차단 도메인.
 *
 * DMA 를 막는 일이 메모리 부족으로 실패하면 안 되므로 정적으로 둔다.
 */
static struct iommu_domain arm_smmu_blocked_domain = {
	.type = IOMMU_DOMAIN_BLOCKED,	/* [한국어] 차단 종류. 모든 접근이 오류가 된다. */
	.ops = &arm_smmu_blocked_ops,
};

/*
 * [한국어]
 * arm_smmu_map_pages - 매핑을 만든다
 *
 * @domain: 대상 도메인.
 * @iova: 시작 주소.
 * @paddr: 물리 주소.
 * @pgsize: 페이지 크기.
 * @pgcount: 그 개수.
 * @prot: 권한.
 * @gfp: 할당 플래그.
 * @mapped: 실제로 매핑한 바이트 수를 여기에 쓴다.
 * @return: 0 성공, 음수면 실패.
 *
 * 표를 고치는 일은 io-pgtable 이 한다. 이 함수가 하는 일은 그 전후로
 * 전원을 켜고 끄는 것뿐이다 — 표 순회에 필요한 캐시 유지 동작이
 * 하드웨어를 건드릴 수 있기 때문이다.
 *
 * ops 가 없으면 아직 어느 SMMU 에도 붙지 않은 도메인이다.
 */
static int arm_smmu_map_pages(struct iommu_domain *domain, unsigned long iova,
			      phys_addr_t paddr, size_t pgsize, size_t pgcount,
			      int prot, gfp_t gfp, size_t *mapped)
{
	struct io_pgtable_ops *ops = to_smmu_domain(domain)->pgtbl_ops;	/* [한국어] 표 조작 함수들. */
	struct arm_smmu_device *smmu = to_smmu_domain(domain)->smmu;	/* [한국어] 매인 SMMU. */
	int ret;	/* [한국어] 결과 코드. */

	if (!ops)	/* [한국어] 아직 어느 SMMU 에도 붙지 않은 도메인이면 */
		return -ENODEV;	/* [한국어] 매핑할 곳이 없다. */

	arm_smmu_rpm_get(smmu);	/* [한국어] 표 순회 캐시 유지가 하드웨어를 건드릴 수 있어 켜 둔다. */
	ret = ops->map_pages(ops, iova, paddr, pgsize, pgcount, prot, gfp, mapped);	/* [한국어] 실제 매핑은 io-pgtable 이 한다. */
	arm_smmu_rpm_put(smmu);	/* [한국어] 전원 참조를 놓는다. */

	return ret;	/* [한국어] 결과. */
}

/*
 * [한국어]
 * arm_smmu_unmap_pages - 매핑을 푼다
 *
 * @domain: 대상 도메인.
 * @iova: 시작 주소.
 * @pgsize: 페이지 크기.
 * @pgcount: 그 개수.
 * @iotlb_gather: 코어가 모으는 무효화 목록.
 * @return: 실제로 푼 바이트 수.
 *
 * 매핑과 같은 구조다. 무효화는 io-pgtable 이 이 파일의 flush_ops 를
 * 불러 처리한다.
 */
static size_t arm_smmu_unmap_pages(struct iommu_domain *domain, unsigned long iova,
				   size_t pgsize, size_t pgcount,
				   struct iommu_iotlb_gather *iotlb_gather)
{
	struct io_pgtable_ops *ops = to_smmu_domain(domain)->pgtbl_ops;	/* [한국어] 표 조작 함수들. */
	struct arm_smmu_device *smmu = to_smmu_domain(domain)->smmu;	/* [한국어] 매인 SMMU. */
	size_t ret;	/* [한국어] 실제로 푼 바이트 수. */

	if (!ops)	/* [한국어] 붙지 않은 도메인이면 */
		return 0;	/* [한국어] 푼 것이 없다. */

	arm_smmu_rpm_get(smmu);	/* [한국어] 무효화가 하드웨어를 건드리므로 켜 둔다. */
	ret = ops->unmap_pages(ops, iova, pgsize, pgcount, iotlb_gather);	/* [한국어] 해제와 무효화는 io-pgtable 이 이 파일의 함수를 불러 처리한다. */
	arm_smmu_rpm_put(smmu);	/* [한국어] 전원 참조를 놓는다. */

	return ret;	/* [한국어] 푼 크기. */
}

/*
 * [한국어]
 * arm_smmu_flush_iotlb_all - 이 도메인의 TLB 를 통째로 비운다
 *
 * @domain: 대상 도메인.
 *
 * 코어가 여러 해제를 모았다가 한 번에 비울 때 부른다.
 *
 * flush_ops 가 없으면 아직 세워지지 않은 도메인이라 비울 것도 없다.
 */
static void arm_smmu_flush_iotlb_all(struct iommu_domain *domain)
{
	struct arm_smmu_domain *smmu_domain = to_smmu_domain(domain);	/* [한국어] 이 드라이버의 도메인. */
	struct arm_smmu_device *smmu = smmu_domain->smmu;	/* [한국어] 매인 SMMU. */

	if (smmu_domain->flush_ops) {	/* [한국어] 세워진 도메인이면 */
		arm_smmu_rpm_get(smmu);	/* [한국어] 켜고 */
		smmu_domain->flush_ops->tlb_flush_all(smmu_domain);	/* [한국어] 단계에 맞는 통째 비우기를 부른다. */
		arm_smmu_rpm_put(smmu);	/* [한국어] 전원 참조를 놓는다. */
	}
}

/*
 * [한국어]
 * arm_smmu_iotlb_sync - 모아 둔 무효화의 완료를 기다린다
 *
 * @domain: 대상 도메인.
 * @gather: 무효화 목록(여기서는 쓰지 않는다).
 *
 * 무효화 명령 자체는 이미 나갔고, 여기서는 끝나기를 기다린다.
 *
 * 2판이거나 1단계면 뱅크 단위로 기다릴 수 있다. 1판의 2단계는 무효화가
 * 전역 레지스터로 나가므로 전역 동기화를 기다려야 한다.
 */
static void arm_smmu_iotlb_sync(struct iommu_domain *domain,
				struct iommu_iotlb_gather *gather)
{
	struct arm_smmu_domain *smmu_domain = to_smmu_domain(domain);	/* [한국어] 이 드라이버의 도메인. */
	struct arm_smmu_device *smmu = smmu_domain->smmu;	/* [한국어] 매인 SMMU. */

	if (!smmu)	/* [한국어] 붙지 않은 도메인이면 */
		return;	/* [한국어] 기다릴 것이 없다. */

	arm_smmu_rpm_get(smmu);	/* [한국어] 레지스터를 읽어야 하므로 켠다. */
	if (smmu->version == ARM_SMMU_V2 ||	/* [한국어] 2판이거나 */
	    smmu_domain->stage == ARM_SMMU_DOMAIN_S1)	/* [한국어] 1단계면 */
		arm_smmu_tlb_sync_context(smmu_domain);	/* [한국어] 뱅크 단위로 기다릴 수 있다. */
	else
		arm_smmu_tlb_sync_global(smmu);	/* [한국어] 1판의 2단계는 무효화가 전역으로 나가 전역 동기화를 기다린다. */
	arm_smmu_rpm_put(smmu);	/* [한국어] 전원 참조를 놓는다. */
}

/*
 * [한국어]
 * arm_smmu_iova_to_phys_hard - 하드웨어에게 주소 변환을 직접 물어본다
 *
 * @domain: 대상 도메인.
 * @iova: 물어볼 주소.
 * @return: 그 물리 주소, 실패하면 0.
 *
 * 소프트웨어로 표를 따라가는 대신 SMMU 에게 시킨다. TLB 와 표를 모두
 * 반영한 결과가 나와, 실제 DMA 가 어디로 갈지를 정확히 알려 준다.
 *
 * 명령을 쓰고 결과를 읽는 두 걸음이라 스핀락으로 직렬화한다 — 겹치면
 * 남의 결과를 자기 것으로 읽는다.
 *
 * 시간이 다하면 소프트웨어 순회로 물러선다. 하드웨어가 멈춰도 답은
 * 줄 수 있어야 하기 때문이다.
 *
 * 마지막에 하위 12비트를 되붙이는 것에 주의 — 변환은 페이지 단위라
 * 페이지 안의 오프셋은 그대로 이어진다.
 */
static phys_addr_t arm_smmu_iova_to_phys_hard(struct iommu_domain *domain,
					      dma_addr_t iova)
{
	struct arm_smmu_domain *smmu_domain = to_smmu_domain(domain);	/* [한국어] 이 드라이버의 도메인. */
	struct arm_smmu_device *smmu = smmu_domain->smmu;	/* [한국어] 매인 SMMU. */
	struct arm_smmu_cfg *cfg = &smmu_domain->cfg;	/* [한국어] 뱅크 설정. */
	struct io_pgtable_ops *ops= smmu_domain->pgtbl_ops;	/* [한국어] 물러설 때 쓸 소프트웨어 순회. */
	struct device *dev = smmu->dev;	/* [한국어] 로그용. */
	void __iomem *reg;	/* [한국어] 상태 레지스터의 주소. */
	u32 tmp;	/* [한국어] 폴링에서 읽은 값. */
	u64 phys;	/* [한국어] 하드웨어가 답한 결과. */
	unsigned long va, flags;	/* [한국어] 정렬한 주소와 인터럽트 상태. */
	int ret, idx = cfg->cbndx;	/* [한국어] 결과 코드와 뱅크 번호. */
	phys_addr_t addr = 0;	/* [한국어] 돌려줄 물리 주소. 실패하면 0 그대로. */

	ret = arm_smmu_rpm_get(smmu);	/* [한국어] 레지스터를 쓰려면 켜야 한다. */
	if (ret < 0)	/* [한국어] 켜지 못하면 */
		return 0;	/* [한국어] 물어볼 수 없다. */

	spin_lock_irqsave(&smmu_domain->cb_lock, flags);	/* [한국어] 명령과 결과 읽기가 한 쌍이라 직렬화한다. */
	va = iova & ~0xfffUL;	/* [한국어] 페이지 경계로 내린다 — 변환은 페이지 단위다. */
	if (cfg->fmt == ARM_SMMU_CTX_FMT_AARCH64)	/* [한국어] 64비트 형식이면 */
		arm_smmu_cb_writeq(smmu, idx, ARM_SMMU_CB_ATS1PR, va);	/* [한국어] 64비트로 한 번에 쓰고 */
	else
		arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_ATS1PR, va);	/* [한국어] 아니면 32비트로 쓴다. */

	reg = arm_smmu_page(smmu, ARM_SMMU_CB(smmu, idx)) + ARM_SMMU_CB_ATSR;	/* [한국어] 상태 레지스터의 주소를 미리 구해 둔다. */
	if (readl_poll_timeout_atomic(reg, tmp, !(tmp & ARM_SMMU_CB_ATSR_ACTIVE),	/* [한국어] 5마이크로초 간격으로 50마이크로초까지 기다린다. 원자적 판이라 잠들지 않는다. */
				      5, 50)) {
		spin_unlock_irqrestore(&smmu_domain->cb_lock, flags);	/* [한국어] 시간이 다했으면 락을 놓고 */
		dev_err(dev,	/* [한국어] 알린 뒤 */
			"iova to phys timed out on %pad. Falling back to software table walk.\n",
			&iova);
		arm_smmu_rpm_put(smmu);	/* [한국어] 전원 참조를 놓고 */
		return ops->iova_to_phys(ops, iova);	/* [한국어] 소프트웨어 순회로 물러선다. 하드웨어가 멈춰도 답은 줄 수 있어야 한다. */
	}

	phys = arm_smmu_cb_readq(smmu, idx, ARM_SMMU_CB_PAR);	/* [한국어] 결과 레지스터를 읽는다. */
	spin_unlock_irqrestore(&smmu_domain->cb_lock, flags);	/* [한국어] 읽었으니 락을 놓는다. */
	if (phys & ARM_SMMU_CB_PAR_F) {	/* [한국어] 변환이 실패했으면 */
		dev_err(dev, "translation fault!\n");	/* [한국어] 알리고 */
		dev_err(dev, "PAR = 0x%llx\n", phys);	/* [한국어] 원시 값도 남긴다 — 실패 이유가 그 안에 담겨 있다. */
		goto out;	/* [한국어] 0 을 돌려준다. */
	}

	addr = (phys & GENMASK_ULL(39, 12)) | (iova & 0xfff);	/* [한국어] 물리 페이지 주소에 페이지 안 오프셋을 되붙인다. */
out:	/* [한국어] 성공과 실패가 함께 지나는 지점. */
	arm_smmu_rpm_put(smmu);	/* [한국어] 전원 참조를 놓는다. */

	return addr;	/* [한국어] 결과. */
}

/*
 * [한국어]
 * arm_smmu_iova_to_phys - IOVA 를 물리 주소로 옮긴다
 *
 * @domain: 대상 도메인.
 * @iova: 물어볼 주소.
 * @return: 그 물리 주소, 없으면 0.
 *
 * 하드웨어에 물어보는 길이 있고 1단계 도메인이면 그쪽을 쓴다. 그 편이
 * 실제 하드웨어가 보는 것과 정확히 같기 때문이다.
 *
 * 2단계에는 그 연산이 없어 늘 소프트웨어로 따라간다.
 */
static phys_addr_t arm_smmu_iova_to_phys(struct iommu_domain *domain,
					dma_addr_t iova)
{
	struct arm_smmu_domain *smmu_domain = to_smmu_domain(domain);	/* [한국어] 이 드라이버의 도메인. */
	struct io_pgtable_ops *ops = smmu_domain->pgtbl_ops;	/* [한국어] 표 조작 함수들. */

	if (!ops)	/* [한국어] 붙지 않은 도메인이면 */
		return 0;	/* [한국어] 매핑이 없다. */

	if (smmu_domain->smmu->features & ARM_SMMU_FEAT_TRANS_OPS &&	/* [한국어] 하드웨어에 물어볼 수 있고 */
			smmu_domain->stage == ARM_SMMU_DOMAIN_S1)	/* [한국어] 1단계면 */
		return arm_smmu_iova_to_phys_hard(domain, iova);	/* [한국어] 그쪽이 실제 하드웨어가 보는 것과 같다. */

	return ops->iova_to_phys(ops, iova);	/* [한국어] 아니면 소프트웨어로 표를 따라간다. */
}

/*
 * [한국어]
 * arm_smmu_capable - 이 장치가 그 능력을 갖췄는지 답한다
 *
 * @dev: 물어보는 장치.
 * @cap: 물어보는 능력.
 * @return: 갖췄으면 참.
 *
 * 캐시 일관성 판정의 근거를 원 주석이 밝힌다 — 표 순회가 일관성 있는
 * 연결망에 붙어 있으면 변환 인터페이스도 대개 그렇고, 장치 자신이
 * 일관성이 있으면 그 변환 인터페이스도 반드시 그렇다.
 *
 * 실행 금지(NOEXEC)와 지연된 비우기는 늘 지원한다.
 */
static bool arm_smmu_capable(struct device *dev, enum iommu_cap cap)
{
	struct arm_smmu_master_cfg *cfg = dev_iommu_priv_get(dev);	/* [한국어] 장치의 매핑 상태. */

	switch (cap) {	/* [한국어] 물어보는 능력. */
	case IOMMU_CAP_CACHE_COHERENCY:	/* [한국어] 캐시 일관성. */
		/*
		 * It's overwhelmingly the case in practice that when the pagetable
		 * walk interface is connected to a coherent interconnect, all the
		 * translation interfaces are too. Furthermore if the device is
		 * natively coherent, then its translation interface must also be.
		 */
		return cfg->smmu->features & ARM_SMMU_FEAT_COHERENT_WALK ||	/* [한국어] 원 주석대로 표 순회가 일관성이 있으면 변환 인터페이스도 대개 그렇고, */
			device_get_dma_attr(dev) == DEV_DMA_COHERENT;	/* [한국어] 장치 자신이 일관성이 있으면 반드시 그렇다. */
	case IOMMU_CAP_NOEXEC:	/* [한국어] 실행 금지 매핑. */
	case IOMMU_CAP_DEFERRED_FLUSH:	/* [한국어] 지연된 비우기. 둘 다 늘 지원한다. */
		return true;	/* [한국어] 참. */
	default:	/* [한국어] 모르는 능력. */
		return false;	/* [한국어] 아니다. */
	}
}

static	/* [한국어] 반환형이 다음 줄의 함수 이름과 나뉘어 있다. */
/*
 * [한국어]
 * arm_smmu_get_by_fwnode - 펌웨어 노드로 SMMU 를 찾는다
 *
 * @fwnode: 장치 트리나 ACPI 의 노드.
 * @return: 그 SMMU, 없으면 NULL.
 *
 * 장치가 자기 IOMMU 를 노드로 가리키므로, 그것을 실제 드라이버 상태로
 * 옮기는 자리다.
 *
 * 참조를 곧바로 놓는 것에 주의. 그 장치는 플랫폼 버스가 들고 있어
 * 사라지지 않고, 여기서 참조를 유지하면 오히려 정리를 막는다.
 */
struct arm_smmu_device *arm_smmu_get_by_fwnode(struct fwnode_handle *fwnode)
{
	struct device *dev = bus_find_device_by_fwnode(&platform_bus_type, fwnode);	/* [한국어] 그 노드를 가진 플랫폼 장치를 찾는다. */

	put_device(dev);	/* [한국어] 곧바로 참조를 놓는다 — 플랫폼 버스가 들고 있어 사라지지 않고, 여기서 유지하면 정리를 막는다. */
	return dev ? dev_get_drvdata(dev) : NULL;	/* [한국어] 그 장치의 드라이버 상태가 곧 SMMU 다. */
}

/*
 * [한국어]
 * arm_smmu_probe_device - 이 장치를 맡아 매핑 항목을 배정한다
 *
 * @dev: 검사할 장치.
 * @return: 이 장치를 맡을 SMMU 의 손잡이, 실패하면 오류 포인터.
 *
 * 코어가 새 장치를 볼 때마다 부른다. 옛 결합이면 SMMU 를 거꾸로 찾고,
 * 새 결합이면 fwspec 에 이미 담긴 노드로 찾는다.
 *
 * 스트림 id 와 마스크가 하드웨어 폭 안에 있는지 먼저 확인한다. 넘으면
 * 그 장치를 다룰 수 없으므로, 자원을 잡기 전에 거절한다.
 *
 * cfg 를 가변 길이로 잡는 것이 눈에 띈다 — 이 장치의 id 개수만큼만
 * 배열을 붙인다.
 *
 * 마지막의 장치 링크가 중요하다. 그 장치를 쓰는 동안 SMMU 가 잠들지
 * 않게 하고, 장치가 사라지면 링크도 저절로 걷힌다.
 */
static struct iommu_device *arm_smmu_probe_device(struct device *dev)
{
	struct arm_smmu_device *smmu = NULL;	/* [한국어] 이 장치를 맡을 SMMU. */
	struct arm_smmu_master_cfg *cfg;	/* [한국어] 만들 매핑 상태. */
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);	/* [한국어] 펌웨어가 알려 준 스트림 id 목록. */
	int i, ret;	/* [한국어] 순회 첨자와 결과 코드. */

	if (using_legacy_binding) {	/* [한국어] 옛 결합을 쓰는 시스템이면 */
		ret = arm_smmu_register_legacy_master(dev, &smmu);	/* [한국어] SMMU 를 거꾸로 찾아 fwspec 을 만든다. */

		/*
		 * If dev->iommu_fwspec is initally NULL, arm_smmu_register_legacy_master()
		 * will allocate/initialise a new one. Thus we need to update fwspec for
		 * later use.
		 */
		fwspec = dev_iommu_fwspec_get(dev);	/* [한국어] 원 주석대로 그 함수가 fwspec 을 새로 만들 수 있어 다시 읽는다. */
		if (ret)	/* [한국어] 찾지 못했으면 */
			goto out_free;	/* [한국어] 오류를 올린다. */
	} else {
		smmu = arm_smmu_get_by_fwnode(fwspec->iommu_fwnode);	/* [한국어] 새 결합은 장치가 이미 자기 IOMMU 를 가리킨다. */
	}

	ret = -EINVAL;	/* [한국어] 아래 검사에 걸렸을 때의 기본 오류. */
	for (i = 0; i < fwspec->num_ids; i++) {	/* [한국어] 스트림 id 마다 */
		u16 sid = FIELD_GET(ARM_SMMU_SMR_ID, fwspec->ids[i]);	/* [한국어] id 를 꺼내고 */
		u16 mask = FIELD_GET(ARM_SMMU_SMR_MASK, fwspec->ids[i]);	/* [한국어] 마스크도 꺼낸다. */

		if (sid & ~smmu->streamid_mask) {	/* [한국어] 하드웨어가 다룰 수 있는 폭을 넘으면 */
			dev_err(dev, "stream ID 0x%x out of range for SMMU (0x%x)\n",	/* [한국어] 알리고 */
				sid, smmu->streamid_mask);
			goto out_free;	/* [한국어] 거절한다. 자원을 잡기 전에 걸러 낸다. */
		}
		if (mask & ~smmu->smr_mask_mask) {	/* [한국어] 마스크도 마찬가지. */
			dev_err(dev, "SMR mask 0x%x out of range for SMMU (0x%x)\n",	/* [한국어] 알리고 */
				mask, smmu->smr_mask_mask);
			goto out_free;	/* [한국어] 거절한다. */
		}
	}

	ret = -ENOMEM;	/* [한국어] 아래 할당에 걸렸을 때의 오류. */
	cfg = kzalloc(offsetof(struct arm_smmu_master_cfg, smendx[i]),	/* [한국어] 이 장치의 id 개수만큼만 배열을 붙여 잡는다. */
		      GFP_KERNEL);
	if (!cfg)	/* [한국어] 메모리가 없다. */
		goto out_free;	/* [한국어] 실패. */

	cfg->smmu = smmu;	/* [한국어] 매인 SMMU 를 기억한다. */
	dev_iommu_priv_set(dev, cfg);	/* [한국어] 장치에 매달아 둔다. 이후 모든 경로가 이것으로 되짚는다. */
	while (i--)	/* [한국어] 모든 자리를 */
		cfg->smendx[i] = INVALID_SMENDX;	/* [한국어] 아직 배정되지 않음으로 표시한다. */

	ret = arm_smmu_rpm_get(smmu);	/* [한국어] 레지스터를 쓰려면 켜야 한다. */
	if (ret < 0)	/* [한국어] 켜지 못하면 */
		goto out_cfg_free;	/* [한국어] 만든 것을 되돌린다. */

	ret = arm_smmu_master_alloc_smes(dev);	/* [한국어] 스트림 id 마다 매칭 항목을 배정한다. */
	arm_smmu_rpm_put(smmu);	/* [한국어] 전원 참조를 놓는다. */

	if (ret)	/* [한국어] 배정하지 못했으면 */
		goto out_cfg_free;	/* [한국어] 되돌린다. */

	device_link_add(dev, smmu->dev,	/* [한국어] 이 장치를 쓰는 동안 SMMU 가 잠들지 않게 하고, */
			DL_FLAG_PM_RUNTIME | DL_FLAG_AUTOREMOVE_SUPPLIER);	/* [한국어] 장치가 사라지면 링크도 저절로 걷힌다. */

	return &smmu->iommu;	/* [한국어] 이 장치를 맡을 IOMMU 의 손잡이. */

out_cfg_free:	/* [한국어] 매핑 상태를 만든 뒤의 실패. */
	kfree(cfg);	/* [한국어] 해제한다. */
out_free:	/* [한국어] 모든 실패가 합류한다. */
	return ERR_PTR(ret);	/* [한국어] 오류 포인터로 알린다. */
}

/*
 * [한국어]
 * arm_smmu_release_device - 장치가 떠날 때 매핑 항목을 놓는다
 *
 * @dev: 떠나는 장치.
 *
 * 전원을 켜지 못하면 그냥 돌아간다 — 레지스터를 쓸 수 없으면 항목을
 * 지울 방법이 없다. 드문 경우이고, 그때는 SMMU 자체가 문제 상태다.
 */
static void arm_smmu_release_device(struct device *dev)
{
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);	/* [한국어] 스트림 id 목록. */
	struct arm_smmu_master_cfg *cfg = dev_iommu_priv_get(dev);	/* [한국어] 매핑 상태. */
	int ret;	/* [한국어] 결과 코드. */

	ret = arm_smmu_rpm_get(cfg->smmu);	/* [한국어] 레지스터를 쓰려면 켜야 한다. */
	if (ret < 0)	/* [한국어] 켜지 못하면 항목을 지울 방법이 없다. */
		return;	/* [한국어] 드문 경우이고, 그때는 SMMU 자체가 문제 상태다. */

	arm_smmu_master_free_smes(cfg, fwspec);	/* [한국어] 매칭 항목들을 놓는다. */

	arm_smmu_rpm_put(cfg->smmu);	/* [한국어] 전원 참조를 놓는다. */

	kfree(cfg);	/* [한국어] 매핑 상태를 해제한다. */
}

/*
 * [한국어]
 * arm_smmu_probe_finalize - 장치 probe 의 마지막 갈고리
 *
 * @dev: 그 장치.
 *
 * 구현체가 그 장치에 필요한 마무리 설정을 하는 자리다. 퀄컴은 여기서
 * 전원 관리 유예를 켠다.
 */
static void arm_smmu_probe_finalize(struct device *dev)
{
	struct arm_smmu_master_cfg *cfg;	/* [한국어] 매핑 상태. */
	struct arm_smmu_device *smmu;	/* [한국어] 매인 SMMU. */

	cfg = dev_iommu_priv_get(dev);	/* [한국어] 장치에 매달아 둔 상태. */
	smmu = cfg->smmu;	/* [한국어] 그 SMMU. */

	if (smmu->impl && smmu->impl->probe_finalize)	/* [한국어] 구현체가 마무리할 것이 있으면 */
		smmu->impl->probe_finalize(smmu, dev);	/* [한국어] 그쪽에 맡긴다. */
}

/*
 * [한국어]
 * arm_smmu_device_group - 이 장치가 속할 그룹을 정한다
 *
 * @dev: 대상 장치.
 * @return: 그 그룹, 실패하면 오류 포인터.
 *
 * 같은 매핑 항목에 걸리는 장치들은 SMMU 가 구별하지 못하므로 반드시 한
 * 그룹이어야 한다. 그래서 이 장치의 항목들에 이미 그룹이 붙어 있으면
 * 그것을 쓴다.
 *
 * 서로 다른 그룹이 걸려 있으면 모순이라 거절한다 — 한 장치가 두 그룹에
 * 속할 수는 없다.
 *
 * 아무것도 없으면 버스별 기본 규칙(PCI 는 별칭까지 묶는다)으로 만들고,
 * 그것을 항목에 적어 두어 다음 장치가 빠르게 찾게 한다.
 */
static struct iommu_group *arm_smmu_device_group(struct device *dev)
{
	struct arm_smmu_master_cfg *cfg = dev_iommu_priv_get(dev);	/* [한국어] 매핑 상태. */
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);	/* [한국어] 스트림 id 목록. */
	struct arm_smmu_device *smmu = cfg->smmu;	/* [한국어] 매인 SMMU. */
	struct iommu_group *group = NULL;	/* [한국어] 찾거나 만들 그룹. */
	int i, idx;	/* [한국어] 순회 첨자와 항목 번호. */

	mutex_lock(&smmu->stream_map_mutex);	/* [한국어] 항목의 그룹 정보를 지키는 락. */
	for_each_cfg_sme(cfg, fwspec, i, idx) {	/* [한국어] 이 장치가 쓰는 항목마다 */
		if (group && smmu->s2crs[idx].group &&	/* [한국어] 이미 다른 그룹을 봤는데 */
		    group != smmu->s2crs[idx].group) {	/* [한국어] 또 다른 그룹이 나오면 */
			mutex_unlock(&smmu->stream_map_mutex);	/* [한국어] 락을 놓고 */
			return ERR_PTR(-EINVAL);	/* [한국어] 거절한다. 한 장치가 두 그룹에 속할 수는 없다. */
		}

		group = smmu->s2crs[idx].group;	/* [한국어] 그 항목에 붙은 그룹을 기억한다. */
	}

	if (group) {	/* [한국어] 이미 그룹이 있으면 */
		mutex_unlock(&smmu->stream_map_mutex);	/* [한국어] 락을 놓고 */
		return iommu_group_ref_get(group);	/* [한국어] 참조를 들어 돌려준다. */
	}

	if (dev_is_pci(dev))	/* [한국어] PCI 면 */
		group = pci_device_group(dev);	/* [한국어] 별칭까지 묶는 규칙을 쓰고 */
	else if (dev_is_fsl_mc(dev))	/* [한국어] Freescale MC 버스면 */
		group = fsl_mc_device_group(dev);	/* [한국어] 그 규칙을, */
	else
		group = generic_device_group(dev);	/* [한국어] 그 밖에는 장치마다 하나씩 만든다. */

	/* Remember group for faster lookups */
	if (!IS_ERR(group))	/* [한국어] 만들었으면 */
		for_each_cfg_sme(cfg, fwspec, i, idx)	/* [한국어] 이 장치의 항목마다 */
			smmu->s2crs[idx].group = group;	/* [한국어] 그룹을 적어 둔다 — 같은 항목에 걸리는 다음 장치가 이것을 찾는다. */

	mutex_unlock(&smmu->stream_map_mutex);	/* [한국어] 락을 놓는다. */
	return group;	/* [한국어] 그룹. */
}

/*
 * [한국어]
 * arm_smmu_set_pgtable_quirks - 페이지 테이블 예외 표시를 설정한다
 *
 * @domain: 대상 도메인.
 * @quirks: 설정할 표시.
 * @return: 0 성공, 이미 세워진 도메인이면 -EPERM.
 *
 * 표가 만들어진 뒤에는 바꿀 수 없다 — 그 표시가 표의 모양을 정하기
 * 때문이다. 그래서 첫 장치가 붙기 전에만 허용한다.
 */
static int arm_smmu_set_pgtable_quirks(struct iommu_domain *domain,
		unsigned long quirks)
{
	struct arm_smmu_domain *smmu_domain = to_smmu_domain(domain);	/* [한국어] 이 드라이버의 도메인. */
	int ret = 0;	/* [한국어] 결과 코드. */

	mutex_lock(&smmu_domain->init_mutex);	/* [한국어] 세워짐 여부를 확인하는 동안 바뀌지 않게 한다. */
	if (smmu_domain->smmu)	/* [한국어] 이미 표가 만들어졌으면 */
		ret = -EPERM;	/* [한국어] 바꿀 수 없다 — 그 표시가 표의 모양을 정한다. */
	else
		smmu_domain->pgtbl_quirks = quirks;	/* [한국어] 아직이면 기억해 두었다 표를 만들 때 쓴다. */
	mutex_unlock(&smmu_domain->init_mutex);	/* [한국어] 락을 놓는다. */

	return ret;	/* [한국어] 결과. */
}

/*
 * [한국어]
 * arm_smmu_of_xlate - 장치 트리의 iommus 인자를 스트림 id 로 옮긴다
 *
 * @dev: 대상 장치.
 * @args: iommus 속성의 인자들.
 * @return: 0 성공, 음수면 실패.
 *
 * 인자가 하나면 id 만, 둘이면 마스크까지 온다. 마스크를 주지 않았으면
 * SMMU 노드의 stream-match-mask 속성을 대신 본다 — 그 SMMU 아래 모든
 * 장치가 같은 마스크를 쓰는 경우를 위한 지름길이다.
 *
 * id 와 마스크를 한 32비트 값에 담아 fwspec 에 넣는다. 그 배치가 SMR
 * 레지스터와 같아, 나중에 그대로 꺼내 쓸 수 있다.
 */
static int arm_smmu_of_xlate(struct device *dev,
			     const struct of_phandle_args *args)
{
	u32 mask, fwid = 0;	/* [한국어] 노드에서 읽을 마스크와, 만들어 낼 값. */

	if (args->args_count > 0)	/* [한국어] 첫 인자가 있으면 */
		fwid |= FIELD_PREP(ARM_SMMU_SMR_ID, args->args[0]);	/* [한국어] 그것이 스트림 id 다. */

	if (args->args_count > 1)	/* [한국어] 두 번째 인자가 있으면 */
		fwid |= FIELD_PREP(ARM_SMMU_SMR_MASK, args->args[1]);	/* [한국어] 그것이 마스크다. */
	else if (!of_property_read_u32(args->np, "stream-match-mask", &mask))	/* [한국어] 없으면 SMMU 노드의 공통 마스크를 본다. */
		fwid |= FIELD_PREP(ARM_SMMU_SMR_MASK, mask);	/* [한국어] 그 SMMU 아래 모든 장치가 같은 마스크를 쓰는 경우의 지름길이다. */

	return iommu_fwspec_add_ids(dev, &fwid, 1);	/* [한국어] SMR 레지스터와 같은 배치로 담아 두어, 나중에 그대로 꺼내 쓴다. */
}

/*
 * [한국어]
 * arm_smmu_get_resv_regions - 이 장치가 쓰면 안 되는 IOVA 구간을 알린다
 *
 * @dev: 대상 장치.
 * @head: 구간 목록을 이어 붙일 자리.
 *
 * MSI 창을 예약한다. ARM 에서는 인터럽트 doorbell 이 IOVA 공간 안에
 * 있어야 하고, 커널이 그 주소를 정해 매핑해 둔다. 사용자가 그 자리에
 * 다른 매핑을 만들면 인터럽트가 엉뚱한 메모리를 건드린다.
 *
 * 그 뒤 dma-iommu 의 일반 예약 구간도 더한다.
 */
static void arm_smmu_get_resv_regions(struct device *dev,
				      struct list_head *head)
{
	struct iommu_resv_region *region;	/* [한국어] 만들 예약 구간. */
	int prot = IOMMU_WRITE | IOMMU_NOEXEC | IOMMU_MMIO;	/* [한국어] MSI doorbell 은 쓰기 전용 장치 레지스터다. */

	region = iommu_alloc_resv_region(MSI_IOVA_BASE, MSI_IOVA_LENGTH,	/* [한국어] 커널이 정한 MSI 창을 예약한다. */
					 prot, IOMMU_RESV_SW_MSI, GFP_KERNEL);	/* [한국어] 소프트웨어 MSI 종류로 표시해, 코어가 그 자리에 실제 매핑을 만들게 한다. */
	if (!region)	/* [한국어] 메모리가 없으면 */
		return;	/* [한국어] 예약 없이 넘어간다. */

	list_add_tail(&region->list, head);	/* [한국어] 목록에 더한다. */

	iommu_dma_get_resv_regions(dev, head);	/* [한국어] dma-iommu 의 일반 예약 구간도 더한다. */
}

/*
 * [한국어]
 * arm_smmu_def_domain_type - 이 장치에 어떤 기본 도메인을 줄지 정한다
 *
 * @dev: 대상 장치.
 * @return: 도메인 종류, 0 이면 코어의 기본값을 쓴다.
 *
 * 옛 결합에서는 늘 항등 도메인이다 — 그 시절 DMA API 가 자기 매핑을
 * 직접 관리했기 때문이다.
 *
 * 그 밖에는 구현체에 맡긴다. GPU 처럼 항등 매핑이 필요한 장치를 그쪽이
 * 알고 있다.
 */
static int arm_smmu_def_domain_type(struct device *dev)
{
	struct arm_smmu_master_cfg *cfg = dev_iommu_priv_get(dev);	/* [한국어] 매핑 상태. */
	const struct arm_smmu_impl *impl = cfg->smmu->impl;	/* [한국어] 구현체 갈고리표. */

	if (using_legacy_binding)	/* [한국어] 옛 결합이면 */
		return IOMMU_DOMAIN_IDENTITY;	/* [한국어] 늘 항등 도메인이다 — 그 시절 DMA API 가 매핑을 직접 관리했다. */

	if (impl && impl->def_domain_type)	/* [한국어] 구현체가 정하는 것이 있으면 */
		return impl->def_domain_type(dev);	/* [한국어] 그쪽에 맡긴다. */

	return 0;	/* [한국어] 코어의 기본값을 쓴다. */
}

/*
 * [한국어] iommu 코어에 등록하는 연산표.
 *
 * 앞쪽은 장치와 도메인의 생애를 다루는 함수들이고, default_domain_ops 는
 * 그 도메인 위에서 실제 매핑을 다루는 함수들이다.
 *
 * 항등·차단 도메인을 정적으로 두는 데 주의 — 그 둘은 만들기가 실패하면
 * 안 된다.
 */
static const struct iommu_ops arm_smmu_ops = {
	.identity_domain	= &arm_smmu_identity_domain,	/* [한국어] 정적으로 둔 항등 도메인. */
	.blocked_domain		= &arm_smmu_blocked_domain,
	.capable		= arm_smmu_capable,
	.domain_alloc_paging	= arm_smmu_domain_alloc_paging,
	.probe_device		= arm_smmu_probe_device,
	.release_device		= arm_smmu_release_device,
	.probe_finalize		= arm_smmu_probe_finalize,
	.device_group		= arm_smmu_device_group,
	.of_xlate		= arm_smmu_of_xlate,
	.get_resv_regions	= arm_smmu_get_resv_regions,
	.def_domain_type	= arm_smmu_def_domain_type,
	.owner			= THIS_MODULE,
	.default_domain_ops = &(const struct iommu_domain_ops) {
		.attach_dev		= arm_smmu_attach_dev,	/* [한국어] 도메인 위에서 실제 매핑을 다루는 함수들이 여기 모인다. */
		.map_pages		= arm_smmu_map_pages,
		.unmap_pages		= arm_smmu_unmap_pages,
		.flush_iotlb_all	= arm_smmu_flush_iotlb_all,
		.iotlb_sync		= arm_smmu_iotlb_sync,
		.iova_to_phys		= arm_smmu_iova_to_phys,
		.set_pgtable_quirks	= arm_smmu_set_pgtable_quirks,
		.free			= arm_smmu_domain_free,
	}
};

/*
 * [한국어]
 * arm_smmu_device_reset - 하드웨어를 알려진 상태로 세운다
 *
 * @smmu: 대상 SMMU.
 *
 * probe 와 전원 복귀 때 부른다. 부트로더가 남긴 설정을 지우고 커널이
 * 아는 상태로 만든다.
 *
 * 순서가 중요하다. 매핑 항목과 컨텍스트 뱅크를 먼저 정리하고, TLB 를
 * 비운 뒤, 마지막에 sCR0 를 써서 SMMU 를 켠다. 원 주석의 "Push the
 * button" 이 그 마지막 걸음이다.
 *
 * CLIENTPD 를 지우는 것이 곧 켜는 동작이다. 그전까지는 모든 트래픽이
 * 그냥 통과한다.
 *
 * USFCFG 가 보안의 핵심이다. 켜면 등록되지 않은 스트림이 오류가 되고,
 * 끄면 그냥 통과한다 — 후자는 그 장치가 물리 메모리를 자유롭게 건드릴
 * 수 있다는 뜻이다.
 *
 * 브로드캐스트를 끄는 이유: 이 드라이버는 무효화를 손수 하므로,
 * CPU 의 무효화가 새어 들어오면 오히려 방해가 된다.
 */
static void arm_smmu_device_reset(struct arm_smmu_device *smmu)
{
	int i;	/* [한국어] 순회 첨자. */
	u32 reg;	/* [한국어] 읽고 쓸 레지스터 값. */

	/* clear global FSR */
	reg = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_sGFSR);	/* [한국어] 전역 오류 상태를 읽어 */
	arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_sGFSR, reg);	/* [한국어] 그대로 되써서 지운다. 부팅 전에 쌓인 오류를 치운다. */

	/*
	 * Reset stream mapping groups: Initial values mark all SMRn as
	 * invalid and all S2CRn as bypass unless overridden.
	 */
	for (i = 0; i < smmu->num_mapping_groups; ++i)	/* [한국어] 모든 매핑 항목을 */
		arm_smmu_write_sme(smmu, i);	/* [한국어] 그림자 상태대로 쓴다. 원 주석대로 초기값은 무효·우회다. */

	/* Make sure all context banks are disabled and clear CB_FSR  */
	for (i = 0; i < smmu->num_context_banks; ++i) {	/* [한국어] 모든 컨텍스트 뱅크를 */
		arm_smmu_write_context_bank(smmu, i);	/* [한국어] 그림자 상태대로 쓰고 */
		arm_smmu_cb_write(smmu, i, ARM_SMMU_CB_FSR, ARM_SMMU_CB_FSR_FAULT);	/* [한국어] 쌓인 오류 상태도 지운다. */
	}

	/* Invalidate the TLB, just in case */
	arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_TLBIALLH, QCOM_DUMMY_VAL);	/* [한국어] 하이퍼바이저 TLB 를 비운다. */
	arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_TLBIALLNSNH, QCOM_DUMMY_VAL);	/* [한국어] 비보안 TLB 도 비운다. 부팅 전 항목이 남아 있을 수 있다. */

	reg = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_sCR0);	/* [한국어] 전역 설정을 읽어 고쳐 나간다. */

	/* Enable fault reporting */
	reg |= (ARM_SMMU_sCR0_GFRE | ARM_SMMU_sCR0_GFIE |	/* [한국어] 오류 보고와 인터럽트를 켜고 */
		ARM_SMMU_sCR0_GCFGFRE | ARM_SMMU_sCR0_GCFGFIE);	/* [한국어] 설정 오류 쪽도 켠다. */

	/* Disable TLB broadcasting. */
	reg |= (ARM_SMMU_sCR0_VMIDPNE | ARM_SMMU_sCR0_PTM);	/* [한국어] 브로드캐스트 무효화를 막는다 — 이 드라이버는 손수 비우므로 CPU 쪽 무효화가 새어 들면 방해가 된다. */

	/* Enable client access, handling unmatched streams as appropriate */
	reg &= ~ARM_SMMU_sCR0_CLIENTPD;	/* [한국어] 이 비트를 지우는 것이 곧 SMMU 를 켜는 동작이다. */
	if (disable_bypass)	/* [한국어] 우회를 막는 설정이면 */
		reg |= ARM_SMMU_sCR0_USFCFG;	/* [한국어] 모르는 스트림을 오류로 만든다 — 보안의 핵심이다. */
	else
		reg &= ~ARM_SMMU_sCR0_USFCFG;	/* [한국어] 아니면 그냥 통과시킨다. 그 장치는 물리 메모리를 자유롭게 건드린다. */

	/* Disable forced broadcasting */
	reg &= ~ARM_SMMU_sCR0_FB;	/* [한국어] 강제 브로드캐스트를 끈다. */

	/* Don't upgrade barriers */
	reg &= ~(ARM_SMMU_sCR0_BSU);	/* [한국어] 장벽을 강화하지 않는다 — 그것은 성능만 깎는다. */

	if (smmu->features & ARM_SMMU_FEAT_VMID16)	/* [한국어] 16비트 VMID 를 쓸 수 있으면 */
		reg |= ARM_SMMU_sCR0_VMID16EN;	/* [한국어] 켠다. 구별할 수 있는 가상 머신 수가 늘어난다. */

	if (smmu->features & ARM_SMMU_FEAT_EXIDS)	/* [한국어] 확장 스트림 id 를 쓸 수 있으면 */
		reg |= ARM_SMMU_sCR0_EXIDENABLE;	/* [한국어] 켠다. id 공간이 넓어지는 대신 마스크를 쓸 수 없다. */

	if (smmu->impl && smmu->impl->reset)	/* [한국어] 구현체가 손볼 것이 있으면 */
		smmu->impl->reset(smmu);	/* [한국어] 그것을 먼저 한다 — 결함 우회가 켜기 전에 자리 잡아야 한다. */

	/* Push the button */
	arm_smmu_tlb_sync_global(smmu);	/* [한국어] 앞선 무효화가 끝나기를 기다린 뒤 */
	arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_sCR0, reg);	/* [한국어] 원 주석의 "Push the button" — 이 쓰기로 SMMU 가 살아난다. */
}

/*
 * [한국어]
 * arm_smmu_id_size_to_bits - 능력 레지스터의 크기 부호를 비트 수로 옮긴다
 *
 * @size: ID2 레지스터의 크기 필드 값.
 * @return: 그 주소 폭(비트).
 *
 * 규격이 정한 표를 그대로 옮긴 것이다. 값이 연속하지 않아(32, 36, 40,
 * 42, 44, 48) 계산이 아니라 표로 만들었다.
 */
static int arm_smmu_id_size_to_bits(int size)
{
	switch (size) {	/* [한국어] 규격이 정한 부호. */
	case 0:	/* [한국어] 부호 0 은 */
		return 32;	/* [한국어] 32비트. */
	case 1:	/* [한국어] 부호 1 은 */
		return 36;	/* [한국어] 36비트. */
	case 2:	/* [한국어] 부호 2 는 */
		return 40;	/* [한국어] 40비트. */
	case 3:	/* [한국어] 부호 3 은 */
		return 42;	/* [한국어] 42비트. */
	case 4:	/* [한국어] 부호 4 는 */
		return 44;	/* [한국어] 44비트. */
	case 5:	/* [한국어] 부호 5 와 */
	default:	/* [한국어] 정의되지 않은 값은 */
		return 48;	/* [한국어] 48비트로 본다 — 가장 넓은 값이라 안전한 쪽이다. */
	}
}

/*
 * [한국어]
 * arm_smmu_device_cfg_probe - 능력 레지스터를 읽어 하드웨어의 성질을 알아낸다
 *
 * @smmu: 대상 SMMU.
 * @return: 0 성공, 음수면 실패.
 *
 * ID0~ID2 를 읽어 features 비트와 자원 개수를 채우고, 그에 맞춰 배열을
 * 잡는다. 이 함수가 끝나야 도메인을 만들 수 있다.
 *
 * force_stage 모듈 파라미터가 여기서 능력을 깎는다 — 하드웨어가 할 수
 * 있어도 못 하는 것처럼 만들어, 특정 단계만 쓰게 강제한다. 디버깅용이다.
 *
 * 표 순회 일관성을 두 곳에서 알아내는 것이 눈에 띈다 — 펌웨어가 알려 준
 * 값과 레지스터가 말하는 값이 다르면 그 사실을 로그에 남기고 펌웨어 쪽을
 * 믿는다. 통합 방식이 하드웨어 자신보다 그것을 더 잘 알기 때문이다.
 *
 * 진행하며 알아낸 것을 로그로 찍는다. SMMU 문제를 다룰 때 그 출력이
 * 하드웨어의 성질을 알려 주는 첫 단서가 된다.
 */
static int arm_smmu_device_cfg_probe(struct arm_smmu_device *smmu)
{
	unsigned int size;	/* [한국어] 자원 개수와 주소 폭을 담을 임시 변수. */
	u32 id;	/* [한국어] 읽어 온 능력 레지스터 값. */
	bool cttw_reg, cttw_fw = smmu->features & ARM_SMMU_FEAT_COHERENT_WALK;	/* [한국어] 레지스터가 말하는 것과 펌웨어가 말하는 것. */
	int i, ret;	/* [한국어] 순회 첨자와 결과 코드. */

	dev_notice(smmu->dev, "probing hardware configuration...\n");	/* [한국어] 아래 로그가 이 하드웨어의 성질을 알려 주는 첫 단서가 된다. */
	dev_notice(smmu->dev, "SMMUv%d with:\n",	/* [한국어] 판 번호를 먼저 찍는다. */
			smmu->version == ARM_SMMU_V2 ? 2 : 1);

	/* ID0 */
	id = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_ID0);	/* [한국어] 첫 능력 레지스터. */

	/* Restrict available stages based on module parameter */
	if (force_stage == 1)	/* [한국어] 모듈 파라미터로 1단계를 강제하면 */
		id &= ~(ARM_SMMU_ID0_S2TS | ARM_SMMU_ID0_NTS);	/* [한국어] 2단계와 중첩을 못 하는 것처럼 만든다. */
	else if (force_stage == 2)	/* [한국어] 2단계를 강제하면 */
		id &= ~(ARM_SMMU_ID0_S1TS | ARM_SMMU_ID0_NTS);	/* [한국어] 1단계와 중첩을 감춘다. 디버깅용 장치다. */

	if (id & ARM_SMMU_ID0_S1TS) {	/* [한국어] 1단계를 지원하면 */
		smmu->features |= ARM_SMMU_FEAT_TRANS_S1;	/* [한국어] 능력 비트를 세우고 */
		dev_notice(smmu->dev, "\tstage 1 translation\n");	/* [한국어] 로그에 남긴다. */
	}

	if (id & ARM_SMMU_ID0_S2TS) {	/* [한국어] 2단계를 지원하면 */
		smmu->features |= ARM_SMMU_FEAT_TRANS_S2;	/* [한국어] 같은 방식으로 기록한다. */
		dev_notice(smmu->dev, "\tstage 2 translation\n");	/* [한국어] 로그에 남긴다. */
	}

	if (id & ARM_SMMU_ID0_NTS) {	/* [한국어] 중첩 변환을 지원하면 */
		smmu->features |= ARM_SMMU_FEAT_TRANS_NESTED;	/* [한국어] 기록한다. */
		dev_notice(smmu->dev, "\tnested translation\n");	/* [한국어] 로그에 남긴다. */
	}

	if (!(smmu->features &	/* [한국어] 두 단계 중 아무것도 못 하면 */
		(ARM_SMMU_FEAT_TRANS_S1 | ARM_SMMU_FEAT_TRANS_S2))) {
		dev_err(smmu->dev, "\tno translation support!\n");	/* [한국어] 쓸 수 없는 하드웨어다. */
		return -ENODEV;	/* [한국어] 포기한다. */
	}

	if ((id & ARM_SMMU_ID0_S1TS) &&	/* [한국어] 1단계가 있고 */
	    ((smmu->version < ARM_SMMU_V2) || !(id & ARM_SMMU_ID0_ATOSNS))) {	/* [한국어] 1판이거나 ATOS 를 지원하면(비트가 부정형이라 0 이 지원이다) */
		smmu->features |= ARM_SMMU_FEAT_TRANS_OPS;	/* [한국어] 하드웨어에 주소 변환을 물어볼 수 있다. */
		dev_notice(smmu->dev, "\taddress translation ops\n");	/* [한국어] 로그에 남긴다. */
	}

	/*
	 * In order for DMA API calls to work properly, we must defer to what
	 * the FW says about coherency, regardless of what the hardware claims.
	 * Fortunately, this also opens up a workaround for systems where the
	 * ID register value has ended up configured incorrectly.
	 */
	cttw_reg = !!(id & ARM_SMMU_ID0_CTTW);	/* [한국어] 레지스터가 말하는 표 순회 일관성. */
	if (cttw_fw || cttw_reg)	/* [한국어] 둘 중 하나라도 있으면 */
		dev_notice(smmu->dev, "\t%scoherent table walk\n",	/* [한국어] 로그에 남긴다. */
			   cttw_fw ? "" : "non-");
	if (cttw_fw != cttw_reg)	/* [한국어] 둘이 어긋나면 */
		dev_notice(smmu->dev,	/* [한국어] 원 주석대로 펌웨어 쪽을 믿는다 — 통합 방식은 하드웨어가 스스로 알 수 없고, 잘못 설정된 ID 값을 우회하는 길도 된다. */
			   "\t(IDR0.CTTW overridden by FW configuration)\n");

	/* Max. number of entries we have for stream matching/indexing */
	if (smmu->version == ARM_SMMU_V2 && id & ARM_SMMU_ID0_EXIDS) {	/* [한국어] 2판이고 확장 id 를 지원하면 */
		smmu->features |= ARM_SMMU_FEAT_EXIDS;	/* [한국어] 그 능력을 기록하고 */
		size = 1 << 16;	/* [한국어] id 공간이 16비트로 늘어난다. */
	} else {
		size = 1 << FIELD_GET(ARM_SMMU_ID0_NUMSIDB, id);	/* [한국어] 아니면 레지스터가 알려 준 폭. */
	}
	smmu->streamid_mask = size - 1;	/* [한국어] 유효한 id 비트의 마스크. */
	if (id & ARM_SMMU_ID0_SMS) {	/* [한국어] 스트림 매칭을 지원하면 */
		smmu->features |= ARM_SMMU_FEAT_STREAM_MATCH;	/* [한국어] 기록하고 */
		size = FIELD_GET(ARM_SMMU_ID0_NUMSMRG, id);	/* [한국어] 매칭 항목 개수를 읽는다. */
		if (size == 0) {	/* [한국어] 지원한다면서 항목이 없으면 */
			dev_err(smmu->dev,	/* [한국어] 앞뒤가 맞지 않는다. */
				"stream-matching supported, but no SMRs present!\n");
			return -ENODEV;	/* [한국어] 쓸 수 없다. */
		}

		/* Zero-initialised to mark as invalid */
		smmu->smrs = devm_kcalloc(smmu->dev, size, sizeof(*smmu->smrs),	/* [한국어] 원 주석대로 0 으로 채워 잡아 모두 무효 상태로 시작한다. */
					  GFP_KERNEL);
		if (!smmu->smrs)	/* [한국어] 메모리가 없다. */
			return -ENOMEM;	/* [한국어] 실패. */

		dev_notice(smmu->dev,	/* [한국어] 항목 개수를 로그에 남긴다. */
			   "\tstream matching with %u register groups", size);
	}
	/* s2cr->type == 0 means translation, so initialise explicitly */
	smmu->s2crs = devm_kmalloc_array(smmu->dev, size, sizeof(*smmu->s2crs),	/* [한국어] 원 주석대로 종류 0 이 "변환"이라 0 으로 채우면 안 되고, */
					 GFP_KERNEL);
	if (!smmu->s2crs)	/* [한국어] 메모리가 없다. */
		return -ENOMEM;	/* [한국어] 실패. */
	for (i = 0; i < size; i++)	/* [한국어] 손수 */
		smmu->s2crs[i] = s2cr_init_val;	/* [한국어] 초기값(우회 또는 오류)으로 채운다. */

	smmu->num_mapping_groups = size;	/* [한국어] 항목 개수를 기억한다. */
	mutex_init(&smmu->stream_map_mutex);	/* [한국어] 그 배정을 지킬 뮤텍스. */
	spin_lock_init(&smmu->global_sync_lock);	/* [한국어] 전역 무효화를 직렬화할 스핀락. */

	if (smmu->version < ARM_SMMU_V2 ||	/* [한국어] 1판이거나 */
	    !(id & ARM_SMMU_ID0_PTFS_NO_AARCH32)) {	/* [한국어] 32비트 형식을 막지 않았으면(부정형 비트) */
		smmu->features |= ARM_SMMU_FEAT_FMT_AARCH32_L;	/* [한국어] 긴 서술자를 쓸 수 있고 */
		if (!(id & ARM_SMMU_ID0_PTFS_NO_AARCH32S))	/* [한국어] 짧은 서술자도 막지 않았으면 */
			smmu->features |= ARM_SMMU_FEAT_FMT_AARCH32_S;	/* [한국어] 그것도 쓸 수 있다. */
	}

	/* ID1 */
	id = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_ID1);	/* [한국어] 두 번째 능력 레지스터. */
	smmu->pgshift = (id & ARM_SMMU_ID1_PAGESIZE) ? 16 : 12;	/* [한국어] 레지스터 페이지가 64KB 인지 4KB 인지. */

	/* Check for size mismatch of SMMU address space from mapped region */
	size = 1 << (FIELD_GET(ARM_SMMU_ID1_NUMPAGENDXB, id) + 1);	/* [한국어] 하드웨어가 말하는 전역 페이지 수. */
	if (smmu->numpage != 2 * size << smmu->pgshift)	/* [한국어] 실제로 매핑한 창의 크기와 다르면 */
		dev_warn(smmu->dev,	/* [한국어] 알린다 — 장치 트리가 잘못됐을 수 있다. 두 배인 것은 전역과 뱅크 영역이 같은 크기이기 때문이다. */
			"SMMU address space size (0x%x) differs from mapped region size (0x%x)!\n",
			2 * size << smmu->pgshift, smmu->numpage);
	/* Now properly encode NUMPAGE to subsequently derive SMMU_CB_BASE */
	smmu->numpage = size;	/* [한국어] 이제 페이지 수로 다시 담는다. 위에서는 창 크기를 임시로 담아 두었다. */

	smmu->num_s2_context_banks = FIELD_GET(ARM_SMMU_ID1_NUMS2CB, id);	/* [한국어] 2단계 전용 뱅크 개수. */
	smmu->num_context_banks = FIELD_GET(ARM_SMMU_ID1_NUMCB, id);	/* [한국어] 전체 뱅크 개수. */
	if (smmu->num_s2_context_banks > smmu->num_context_banks) {	/* [한국어] 전용이 전체보다 많으면 */
		dev_err(smmu->dev, "impossible number of S2 context banks!\n");	/* [한국어] 있을 수 없는 값이다. */
		return -ENODEV;	/* [한국어] 쓸 수 없다. */
	}
	dev_notice(smmu->dev, "\t%u context banks (%u stage-2 only)\n",	/* [한국어] 뱅크 수를 로그에 남긴다 — 동시에 쓸 수 있는 도메인 수다. */
		   smmu->num_context_banks, smmu->num_s2_context_banks);
	smmu->cbs = devm_kcalloc(smmu->dev, smmu->num_context_banks,	/* [한국어] 뱅크 그림자 배열을 잡는다. */
				 sizeof(*smmu->cbs), GFP_KERNEL);
	if (!smmu->cbs)	/* [한국어] 메모리가 없다. */
		return -ENOMEM;	/* [한국어] 실패. */

	/* ID2 */
	id = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_ID2);	/* [한국어] 세 번째 능력 레지스터. */
	size = arm_smmu_id_size_to_bits(FIELD_GET(ARM_SMMU_ID2_IAS, id));	/* [한국어] 입력 주소 폭. */
	smmu->ipa_size = size;	/* [한국어] 중간 물리 주소의 폭이기도 하다. */

	/* The output mask is also applied for bypass */
	size = arm_smmu_id_size_to_bits(FIELD_GET(ARM_SMMU_ID2_OAS, id));	/* [한국어] 출력 주소 폭. */
	smmu->pa_size = size;	/* [한국어] 원 주석대로 우회 트래픽에도 이 마스크가 적용된다. */

	if (id & ARM_SMMU_ID2_VMID16)	/* [한국어] 16비트 VMID 를 지원하면 */
		smmu->features |= ARM_SMMU_FEAT_VMID16;	/* [한국어] 기록한다. */

	/*
	 * What the page table walker can address actually depends on which
	 * descriptor format is in use, but since a) we don't know that yet,
	 * and b) it can vary per context bank, this will have to do...
	 */
	if (dma_set_mask_and_coherent(smmu->dev, DMA_BIT_MASK(size)))	/* [한국어] 표 순회기가 닿을 수 있는 주소 범위를 알린다. */
		dev_warn(smmu->dev,	/* [한국어] 원 주석대로 실제 폭은 표 형식에 따라 다르고 뱅크마다도 다를 수 있지만, 아직 그것을 모르므로 이 값으로 만족한다. */
			 "failed to set DMA mask for table walker\n");

	if (smmu->version < ARM_SMMU_V2) {	/* [한국어] 1판이면 */
		smmu->va_size = smmu->ipa_size;	/* [한국어] 입력 폭이 곧 가상 주소 폭이다. */
		if (smmu->version == ARM_SMMU_V1_64K)	/* [한국어] 64KB 페이지 변종이면 */
			smmu->features |= ARM_SMMU_FEAT_FMT_AARCH64_64K;	/* [한국어] 그 형식만 쓸 수 있다. */
	} else {
		size = FIELD_GET(ARM_SMMU_ID2_UBS, id);	/* [한국어] 2판은 상위 주소 공간 크기를 따로 알려 준다. */
		smmu->va_size = arm_smmu_id_size_to_bits(size);	/* [한국어] 그것이 가상 주소 폭이다. */
		if (id & ARM_SMMU_ID2_PTFS_4K)	/* [한국어] 4KB 형식을 지원하면 */
			smmu->features |= ARM_SMMU_FEAT_FMT_AARCH64_4K;	/* [한국어] 기록한다. */
		if (id & ARM_SMMU_ID2_PTFS_16K)	/* [한국어] 16KB 형식. */
			smmu->features |= ARM_SMMU_FEAT_FMT_AARCH64_16K;	/* [한국어] 기록한다. */
		if (id & ARM_SMMU_ID2_PTFS_64K)	/* [한국어] 64KB 형식. */
			smmu->features |= ARM_SMMU_FEAT_FMT_AARCH64_64K;	/* [한국어] 기록한다. */
	}

	if (smmu->impl && smmu->impl->cfg_probe) {	/* [한국어] 구현체가 능력을 손보면 */
		ret = smmu->impl->cfg_probe(smmu);	/* [한국어] 그쪽에 맡긴다 — 잘못 알려 주는 값을 바로잡거나 감춘다. */
		if (ret)	/* [한국어] 실패하면 */
			return ret;	/* [한국어] 그대로 올린다. */
	}

	/* Now we've corralled the various formats, what'll it do? */
	if (smmu->features & ARM_SMMU_FEAT_FMT_AARCH32_S)	/* [한국어] 짧은 서술자 형식이면 */
		smmu->pgsize_bitmap |= SZ_4K | SZ_64K | SZ_1M | SZ_16M;	/* [한국어] 그 형식이 쓸 수 있는 크기들. */
	if (smmu->features &	/* [한국어] 긴 서술자나 64비트 4KB 형식이면 */
	    (ARM_SMMU_FEAT_FMT_AARCH32_L | ARM_SMMU_FEAT_FMT_AARCH64_4K))	/* [한국어] 긴 서술자나 64비트 4KB 면 */
		smmu->pgsize_bitmap |= SZ_4K | SZ_2M | SZ_1G;	/* [한국어] 4KB 단계 구조의 크기들. */
	if (smmu->features & ARM_SMMU_FEAT_FMT_AARCH64_16K)	/* [한국어] 16KB 형식이면 */
		smmu->pgsize_bitmap |= SZ_16K | SZ_32M;	/* [한국어] 그 단계 구조의 크기들. */
	if (smmu->features & ARM_SMMU_FEAT_FMT_AARCH64_64K)	/* [한국어] 64KB 형식이면 */
		smmu->pgsize_bitmap |= SZ_64K | SZ_512M;	/* [한국어] 그 단계 구조의 크기들. */

	dev_notice(smmu->dev, "\tSupported page sizes: 0x%08lx\n",	/* [한국어] 쓸 수 있는 크기를 로그에 남긴다. */
		   smmu->pgsize_bitmap);


	if (smmu->features & ARM_SMMU_FEAT_TRANS_S1)	/* [한국어] 1단계를 하면 */
		dev_notice(smmu->dev, "\tStage-1: %lu-bit VA -> %lu-bit IPA\n",	/* [한국어] 그 주소 변환 폭을 남긴다. */
			   smmu->va_size, smmu->ipa_size);

	if (smmu->features & ARM_SMMU_FEAT_TRANS_S2)	/* [한국어] 2단계를 하면 */
		dev_notice(smmu->dev, "\tStage-2: %lu-bit IPA -> %lu-bit PA\n",	/* [한국어] 그쪽 폭도 남긴다. */
			   smmu->ipa_size, smmu->pa_size);

	return 0;	/* [한국어] 성공. */
}

struct arm_smmu_match_data {	/* [한국어] compatible 문자열이 가리키는 판·구현체 짝. */
	enum arm_smmu_arch_version version;	/* [한국어] 규격 판. 설정자는 아래 매크로, 읽는 자는 dt_probe 다. */
	enum arm_smmu_implementation model;	/* [한국어] 구현체 종류. 갈고리를 고르는 데 쓴다. */
};

#define ARM_SMMU_MATCH_DATA(name, ver, imp)	\
static const struct arm_smmu_match_data name = { .version = ver, .model = imp }	/* [한국어] 같은 모양의 표를 여럿 만들어야 해서 매크로로 줄였다. */

ARM_SMMU_MATCH_DATA(smmu_generic_v1, ARM_SMMU_V1, GENERIC_SMMU);	/* [한국어] 규격 그대로인 1판. */
ARM_SMMU_MATCH_DATA(smmu_generic_v2, ARM_SMMU_V2, GENERIC_SMMU);	/* [한국어] 규격 그대로인 2판. */
ARM_SMMU_MATCH_DATA(arm_mmu401, ARM_SMMU_V1_64K, GENERIC_SMMU);	/* [한국어] MMU-401 은 64KB 페이지를 쓰는 1판 변종. */
ARM_SMMU_MATCH_DATA(arm_mmu500, ARM_SMMU_V2, ARM_MMU500);	/* [한국어] MMU-500 은 결함 우회가 필요한 2판. */
ARM_SMMU_MATCH_DATA(cavium_smmuv2, ARM_SMMU_V2, CAVIUM_SMMUV2);	/* [한국어] Cavium 은 id 범위를 나눠야 한다. */
ARM_SMMU_MATCH_DATA(qcom_smmuv2, ARM_SMMU_V2, QCOM_SMMUV2);	/* [한국어] 퀄컴은 펌웨어 상태를 이어받아야 한다. */

static const struct of_device_id arm_smmu_of_match[] = {	/* [한국어] 장치 트리 매칭표. */
	{ .compatible = "arm,smmu-v1", .data = &smmu_generic_v1 },	/* [한국어] 규격 이름으로 쓴 노드. */
	{ .compatible = "arm,smmu-v2", .data = &smmu_generic_v2 },
	{ .compatible = "arm,mmu-400", .data = &smmu_generic_v1 },
	{ .compatible = "arm,mmu-401", .data = &arm_mmu401 },
	{ .compatible = "arm,mmu-500", .data = &arm_mmu500 },
	{ .compatible = "cavium,smmu-v2", .data = &cavium_smmuv2 },
	{ .compatible = "nvidia,smmu-500", .data = &arm_mmu500 },
	{ .compatible = "qcom,smmu-v2", .data = &qcom_smmuv2 },
	{ },
};
MODULE_DEVICE_TABLE(of, arm_smmu_of_match);	/* [한국어] 모듈 자동 적재를 위해 이 표를 내보낸다. */

#ifdef CONFIG_ACPI	/* [한국어] ACPI 지원은 설정으로 뺄 수 있다. */
/*
 * [한국어]
 * acpi_smmu_get_data - ACPI 모델 번호를 판·구현체로 옮긴다
 *
 * @model: IORT 표가 알려 준 모델 번호.
 * @smmu: 채울 구조체.
 * @return: 0 성공, 모르는 모델이면 -ENODEV.
 *
 * 장치 트리의 compatible 문자열이 하는 일을 ACPI 에서는 이 번호가 한다.
 */
static int acpi_smmu_get_data(u32 model, struct arm_smmu_device *smmu)
{
	int ret = 0;	/* [한국어] 결과 코드. */

	switch (model) {	/* [한국어] IORT 표의 모델 번호. */
	case ACPI_IORT_SMMU_V1:	/* [한국어] 1판 규격. */
	case ACPI_IORT_SMMU_CORELINK_MMU400:	/* [한국어] MMU-400 도 1판이다. */
		smmu->version = ARM_SMMU_V1;	/* [한국어] 판을 정하고 */
		smmu->model = GENERIC_SMMU;	/* [한국어] 규격 그대로인 구현으로 본다. */
		break;	/* [한국어] 다음으로. */
	case ACPI_IORT_SMMU_CORELINK_MMU401:	/* [한국어] MMU-401 은 */
		smmu->version = ARM_SMMU_V1_64K;	/* [한국어] 64KB 페이지를 쓰는 1판 변종이다. */
		smmu->model = GENERIC_SMMU;	/* [한국어] 구현은 규격대로. */
		break;	/* [한국어] 다음으로. */
	case ACPI_IORT_SMMU_V2:	/* [한국어] 2판 규격. */
		smmu->version = ARM_SMMU_V2;	/* [한국어] 판을 정한다. */
		smmu->model = GENERIC_SMMU;	/* [한국어] 규격 그대로. */
		break;	/* [한국어] 다음으로. */
	case ACPI_IORT_SMMU_CORELINK_MMU500:	/* [한국어] MMU-500 은 */
		smmu->version = ARM_SMMU_V2;	/* [한국어] 2판이되 */
		smmu->model = ARM_MMU500;	/* [한국어] 결함 우회가 필요한 구현이다. */
		break;	/* [한국어] 다음으로. */
	case ACPI_IORT_SMMU_CAVIUM_THUNDERX:	/* [한국어] Cavium ThunderX 는 */
		smmu->version = ARM_SMMU_V2;	/* [한국어] 2판이되 */
		smmu->model = CAVIUM_SMMUV2;	/* [한국어] id 범위를 나눠야 하는 구현이다. */
		break;	/* [한국어] 다음으로. */
	default:	/* [한국어] 모르는 모델. */
		ret = -ENODEV;	/* [한국어] 다룰 수 없다. */
	}

	return ret;	/* [한국어] 결과. */
}

/*
 * [한국어]
 * arm_smmu_device_acpi_probe - ACPI 표에서 SMMU 정보를 읽는다
 *
 * @smmu: 채울 구조체.
 * @global_irqs: 전역 인터럽트 개수를 여기에 쓴다.
 * @pmu_irqs: 성능 카운터 인터럽트 개수를 여기에 쓴다.
 * @return: 0 성공, 음수면 실패.
 *
 * 원 주석대로 설정 접근 인터럽트는 세지 않아, 전역 인터럽트를 늘 1 로
 * 둔다.
 *
 * IORT 표가 표 순회 일관성을 알려 주므로 그것을 features 에 반영한다 —
 * 통합 방식은 하드웨어가 스스로 알 수 없는 정보다.
 */
static int arm_smmu_device_acpi_probe(struct arm_smmu_device *smmu,
				      u32 *global_irqs, u32 *pmu_irqs)
{
	struct device *dev = smmu->dev;	/* [한국어] 이 SMMU 의 장치. */
	struct acpi_iort_node *node =	/* [한국어] 플랫폼 데이터로 넘어온 IORT 노드. */
		*(struct acpi_iort_node **)dev_get_platdata(dev);	/* [한국어] 포인터의 포인터라 한 번 벗긴다. */
	struct acpi_iort_smmu *iort_smmu;	/* [한국어] 그 안의 SMMU 전용 부분. */
	int ret;	/* [한국어] 결과 코드. */

	/* Retrieve SMMU1/2 specific data */
	iort_smmu = (struct acpi_iort_smmu *)node->node_data;	/* [한국어] 노드 뒤에 이어 붙은 데이터. */

	ret = acpi_smmu_get_data(iort_smmu->model, smmu);	/* [한국어] 모델 번호를 판·구현체로 옮긴다. */
	if (ret < 0)	/* [한국어] 모르는 모델이면 */
		return ret;	/* [한국어] 다룰 수 없다. */

	/* Ignore the configuration access interrupt */
	*global_irqs = 1;	/* [한국어] 원 주석대로 설정 접근 인터럽트는 세지 않아 늘 1 이다. */
	*pmu_irqs = 0;	/* [한국어] ACPI 경로에는 성능 카운터 인터럽트가 없다. */

	if (iort_smmu->flags & ACPI_IORT_SMMU_COHERENT_WALK)	/* [한국어] 표가 일관성 있는 순회를 알리면 */
		smmu->features |= ARM_SMMU_FEAT_COHERENT_WALK;	/* [한국어] 기록한다 — 통합 방식은 하드웨어가 스스로 알 수 없다. */

	return 0;	/* [한국어] 성공. */
}
#else
/*
 * [한국어]
 * arm_smmu_device_acpi_probe - ACPI 가 꺼진 빌드의 빈 판
 *
 * @smmu: 채우지 않는다.
 * @global_irqs: 채우지 않는다.
 * @pmu_irqs: 채우지 않는다.
 * @return: 늘 -ENODEV.
 *
 * 같은 이유의 빈 함수다. 이 경우 장치 트리 경로만 남는다.
 */
static inline int arm_smmu_device_acpi_probe(struct arm_smmu_device *smmu,
					     u32 *global_irqs, u32 *pmu_irqs)
{
	return -ENODEV;	/* [한국어] ACPI 가 꺼진 빌드에서는 늘 실패한다. */
}
#endif

/*
 * [한국어]
 * arm_smmu_device_dt_probe - 장치 트리에서 SMMU 정보를 읽는다
 *
 * @smmu: 채울 구조체.
 * @global_irqs: 전역 인터럽트 개수를 여기에 쓴다.
 * @pmu_irqs: 성능 카운터 인터럽트 개수(장치 트리에는 없어 늘 0).
 * @return: 0 성공, 음수면 실패.
 *
 * 옛 결합과 새 결합을 가리는 곳이기도 하다. 한 시스템에서 두 방식을
 * 섞을 수 없어, 먼저 본 쪽으로 고정하고 다른 쪽이 나타나면 거절한다.
 *
 * 옛 결합이면 경고를 남긴다. 커널 설정에 따라 DMA API 가 아예 안 되거나
 * SMMU 자체가 안 되므로, 무엇을 잃는지 알려 준다.
 */
static int arm_smmu_device_dt_probe(struct arm_smmu_device *smmu,
				    u32 *global_irqs, u32 *pmu_irqs)
{
	const struct arm_smmu_match_data *data;	/* [한국어] compatible 이 가리키는 판·구현체. */
	struct device *dev = smmu->dev;	/* [한국어] 이 SMMU 의 장치. */
	bool legacy_binding;	/* [한국어] 이 노드가 옛 결합을 쓰는가. */

	if (of_property_read_u32(dev->of_node, "#global-interrupts", global_irqs))	/* [한국어] 필수 속성이 없으면 */
		return dev_err_probe(dev, -ENODEV,	/* [한국어] 다룰 수 없다. */
				     "missing #global-interrupts property\n");
	*pmu_irqs = 0;	/* [한국어] 장치 트리에는 성능 카운터 인터럽트를 따로 적지 않는다. */

	data = of_device_get_match_data(dev);	/* [한국어] compatible 이 가리키는 표. */
	smmu->version = data->version;	/* [한국어] 판을 정하고 */
	smmu->model = data->model;	/* [한국어] 구현체 종류도 정한다. */

	legacy_binding = of_find_property(dev->of_node, "mmu-masters", NULL);	/* [한국어] 옛 결합의 표시. */
	if (legacy_binding && !using_generic_binding) {	/* [한국어] 옛 결합이고 아직 새 결합을 본 적 없으면 */
		if (!using_legacy_binding) {	/* [한국어] 처음 보는 것이면 */
			pr_notice("deprecated \"mmu-masters\" DT property in use; %s support unavailable\n",	/* [한국어] 무엇을 잃는지 알린다. 커널 설정에 따라 DMA API 가 안 되거나 SMMU 자체가 안 된다. */
				  IS_ENABLED(CONFIG_ARM_SMMU_LEGACY_DT_BINDINGS) ? "DMA API" : "SMMU");
		}
		using_legacy_binding = true;	/* [한국어] 이후 그 방식으로 고정한다. */
	} else if (!legacy_binding && !using_legacy_binding) {	/* [한국어] 새 결합이고 옛 결합을 본 적 없으면 */
		using_generic_binding = true;	/* [한국어] 그쪽으로 고정한다. */
	} else {
		dev_err(dev, "not probing due to mismatched DT properties\n");	/* [한국어] 두 방식이 섞였다 — 한 시스템에서 함께 쓸 수 없다. */
		return -ENODEV;	/* [한국어] 이 SMMU 는 다루지 않는다. */
	}

	if (of_dma_is_coherent(dev->of_node))	/* [한국어] 노드가 일관성을 알리면 */
		smmu->features |= ARM_SMMU_FEAT_COHERENT_WALK;	/* [한국어] 기록한다. */

	return 0;	/* [한국어] 성공. */
}

/*
 * [한국어]
 * arm_smmu_rmr_install_bypass_smr - 부팅 때부터 살아 있던 매핑을 이어받는다
 *
 * @smmu: 대상 SMMU.
 *
 * RMR(예약 메모리 구간)은 펌웨어가 "이 장치는 부팅 중에도 계속 DMA 하니
 * 건드리지 말라"고 알려 주는 표다. 화면 출력이 대표적이다.
 *
 * 원 주석이 방식을 밝힌다 — 펌웨어가 세운 매핑을 하나하나 살펴 맞지 않는
 * 것을 지우는 대신, SMMU 를 통째로 꺼 두었다가 reset 에서 다시 켠다.
 * 그 사이에는 모든 트래픽이 통과하므로 그 장치도 계속 돌아간다.
 *
 * 해당 스트림의 항목을 우회로 잡아 두면, reset 이 그 상태를 그대로
 * 하드웨어에 쓴다. 나중에 진짜 드라이버가 그 장치를 잡으면 참조를 놓아
 * 정상 상태로 돌아간다.
 */
static void arm_smmu_rmr_install_bypass_smr(struct arm_smmu_device *smmu)
{
	struct list_head rmr_list;	/* [한국어] 펌웨어가 알려 준 예약 구간 목록. */
	struct iommu_resv_region *e;	/* [한국어] 그 목록의 항목. */
	int idx, cnt = 0;	/* [한국어] 항목 번호와 이어받은 개수. */
	u32 reg;	/* [한국어] 전역 설정 값. */

	INIT_LIST_HEAD(&rmr_list);	/* [한국어] 목록을 비운 상태로 시작한다. */
	iort_get_rmr_sids(dev_fwnode(smmu->dev), &rmr_list);	/* [한국어] 이 SMMU 아래의 RMR 항목들을 얻는다. */

	/*
	 * Rather than trying to look at existing mappings that
	 * are setup by the firmware and then invalidate the ones
	 * that do no have matching RMR entries, just disable the
	 * SMMU until it gets enabled again in the reset routine.
	 */
	reg = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_sCR0);	/* [한국어] 전역 설정을 읽어 */
	reg |= ARM_SMMU_sCR0_CLIENTPD;	/* [한국어] 원 주석대로 SMMU 를 통째로 꺼 둔다 — 펌웨어가 세운 매핑을 하나하나 살피는 대신 이 방법을 쓴다. */
	arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_sCR0, reg);	/* [한국어] 그 사이 모든 트래픽이 통과해 부팅 중인 장치가 계속 돌아간다. */

	list_for_each_entry(e, &rmr_list, list) {	/* [한국어] RMR 항목마다 */
		struct iommu_iort_rmr_data *rmr;	/* [한국어] 그 안의 스트림 id 목록. */
		int i;	/* [한국어] 순회 첨자. */

		rmr = container_of(e, struct iommu_iort_rmr_data, rr);	/* [한국어] 예약 구간에서 RMR 데이터로 되짚는다. */
		for (i = 0; i < rmr->num_sids; i++) {	/* [한국어] 그 스트림 id 마다 */
			idx = arm_smmu_find_sme(smmu, rmr->sids[i], ~0);	/* [한국어] 매칭 항목을 찾는다. 마스크를 모두 세워 정확히 그 id 만 받게 한다. */
			if (idx < 0)	/* [한국어] 자리가 없거나 충돌하면 */
				continue;	/* [한국어] 그 id 는 포기한다. */

			if (smmu->s2crs[idx].count == 0) {	/* [한국어] 첫 사용자면 */
				smmu->smrs[idx].id = rmr->sids[i];	/* [한국어] 그 id 를 담고 */
				smmu->smrs[idx].mask = 0;	/* [한국어] 마스크 없이 */
				smmu->smrs[idx].valid = true;	/* [한국어] 유효로 표시한다. */
			}
			smmu->s2crs[idx].count++;	/* [한국어] 사용자 수를 늘린다 — 나중에 진짜 드라이버가 이 참조를 놓는다. */
			smmu->s2crs[idx].type = S2CR_TYPE_BYPASS;	/* [한국어] 우회로 두어 그 장치가 계속 DMA 하게 한다. */
			smmu->s2crs[idx].privcfg = S2CR_PRIVCFG_DEFAULT;	/* [한국어] 특권 속성은 장치가 보낸 대로. */

			cnt++;	/* [한국어] 이어받은 개수를 센다. */
		}
	}

	dev_notice(smmu->dev, "\tpreserved %d boot mapping%s\n", cnt,	/* [한국어] 몇 개를 이어받았는지 알린다. */
		   str_plural(cnt));
	iort_put_rmr_sids(dev_fwnode(smmu->dev), &rmr_list);	/* [한국어] 목록을 놓는다. */
}

/*
 * [한국어]
 * arm_smmu_device_probe - SMMU 하드웨어를 찾아 세우고 코어에 등록한다
 *
 * @pdev: 플랫폼 장치.
 * @return: 0 성공, 음수면 실패.
 *
 * 이 파일의 시작점이다. 순서가 이렇다.
 *
 * 펌웨어(장치 트리 또는 ACPI)에서 판과 모델을 읽고, 구현체 갈고리를 달고,
 * 레지스터 창과 인터럽트와 클럭을 잡는다. 그 뒤 능력 레지스터를 읽어
 * 자원 배열을 마련하고, 인터럽트 처리기를 걸고, iommu 코어에 등록한다.
 *
 * 구현체 갈고리를 능력 조사보다 먼저 다는 것이 중요하다 — 그 갈고리가
 * 레지스터 읽기를 가로챌 수 있기 때문이다.
 *
 * 전원 관리를 늦게 켜는 것에도 이유가 있다. 그전에는 하드웨어가 늘 켜져
 * 있어야 초기화를 할 수 있다.
 *
 * 등록을 마지막에 하는 이유: 등록하는 순간 코어가 장치를 붙이기 시작하므로,
 * 모든 준비가 끝나 있어야 한다.
 */
static int arm_smmu_device_probe(struct platform_device *pdev)
{
	struct resource *res;	/* [한국어] 레지스터 창의 자원 정보. */
	struct arm_smmu_device *smmu;	/* [한국어] 만들 SMMU 구조체. */
	struct device *dev = &pdev->dev;	/* [한국어] 플랫폼 장치. */
	int num_irqs, i, err;	/* [한국어] 인터럽트 총수, 순회 첨자, 결과. */
	u32 global_irqs, pmu_irqs;	/* [한국어] 전역·성능 카운터 인터럽트 개수. */
	irqreturn_t (*global_fault)(int irq, void *dev);	/* [한국어] 쓸 전역 오류 처리기. */

	smmu = devm_kzalloc(dev, sizeof(*smmu), GFP_KERNEL);	/* [한국어] 구조체를 잡는다. devm 이라 장치가 사라질 때 저절로 해제된다. */
	if (!smmu) {	/* [한국어] 메모리가 없다. */
		dev_err(dev, "failed to allocate arm_smmu_device\n");	/* [한국어] 알리고 */
		return -ENOMEM;	/* [한국어] 실패. */
	}
	smmu->dev = dev;	/* [한국어] 장치를 기억한다. */

	if (dev->of_node)	/* [한국어] 장치 트리로 왔으면 */
		err = arm_smmu_device_dt_probe(smmu, &global_irqs, &pmu_irqs);	/* [한국어] 그쪽에서 정보를 읽고 */
	else
		err = arm_smmu_device_acpi_probe(smmu, &global_irqs, &pmu_irqs);	/* [한국어] 아니면 ACPI 표에서 읽는다. */
	if (err)	/* [한국어] 읽지 못했으면 */
		return err;	/* [한국어] 다룰 수 없다. */

	smmu->base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);	/* [한국어] 레지스터 창을 매핑한다. */
	if (IS_ERR(smmu->base))	/* [한국어] 실패하면 */
		return PTR_ERR(smmu->base);	/* [한국어] 그대로 올린다. */
	smmu->ioaddr = res->start;	/* [한국어] 물리 주소는 sysfs 이름을 짓는 데 쓴다. */

	/*
	 * The resource size should effectively match the value of SMMU_TOP;
	 * stash that temporarily until we know PAGESIZE to validate it with.
	 */
	smmu->numpage = resource_size(res);	/* [한국어] 원 주석대로 페이지 크기를 알기 전까지 창 크기를 여기 임시로 담아 둔다. */

	smmu = arm_smmu_impl_init(smmu);	/* [한국어] 구현체 갈고리를 단다. 구조체가 늘어날 수 있어 반환값을 받는다. */
	if (IS_ERR(smmu))	/* [한국어] 실패하면 */
		return PTR_ERR(smmu);	/* [한국어] 그대로 올린다. */

	num_irqs = platform_irq_count(pdev);	/* [한국어] 이 장치에 달린 인터럽트 총수. */

	smmu->num_context_irqs = num_irqs - global_irqs - pmu_irqs;	/* [한국어] 나머지가 문맥 오류용이다. */
	if (smmu->num_context_irqs <= 0)	/* [한국어] 하나도 없으면 */
		return dev_err_probe(dev, -ENODEV,	/* [한국어] 오류를 보고할 길이 없어 쓸 수 없다. */
				"found %d interrupts but expected at least %d\n",
				num_irqs, global_irqs + pmu_irqs + 1);

	smmu->irqs = devm_kcalloc(dev, smmu->num_context_irqs,	/* [한국어] 인터럽트 번호 배열을 잡는다. */
				  sizeof(*smmu->irqs), GFP_KERNEL);
	if (!smmu->irqs)	/* [한국어] 메모리가 없다. */
		return dev_err_probe(dev, -ENOMEM, "failed to allocate %d irqs\n",	/* [한국어] 실패. */
				     smmu->num_context_irqs);

	for (i = 0; i < smmu->num_context_irqs; i++) {	/* [한국어] 문맥 인터럽트마다 */
		int irq = platform_get_irq(pdev, global_irqs + pmu_irqs + i);	/* [한국어] 전역과 성능 카운터 뒤에서 세어 가져온다. */

		if (irq < 0)	/* [한국어] 얻지 못하면 */
			return irq;	/* [한국어] 그대로 올린다. */
		smmu->irqs[i] = irq;	/* [한국어] 배열에 담는다. */
	}

	err = devm_clk_bulk_get_all(dev, &smmu->clks);	/* [한국어] 이 SMMU 가 받는 클럭을 모두 얻는다. */
	if (err < 0) {	/* [한국어] 실패하면 */
		dev_err(dev, "failed to get clocks %d\n", err);	/* [한국어] 알리고 */
		return err;	/* [한국어] 포기한다. */
	}
	smmu->num_clks = err;	/* [한국어] 반환값이 곧 개수다. */

	err = clk_bulk_prepare_enable(smmu->num_clks, smmu->clks);	/* [한국어] 레지스터를 읽으려면 클럭이 돌아야 한다. */
	if (err)	/* [한국어] 켜지 못하면 */
		return err;	/* [한국어] 포기한다. */

	err = arm_smmu_device_cfg_probe(smmu);	/* [한국어] 능력을 읽고 자원 배열을 마련한다. */
	if (err)	/* [한국어] 실패하면 */
		return err;	/* [한국어] 포기한다. */

	if (smmu->version == ARM_SMMU_V2) {	/* [한국어] 2판은 뱅크마다 인터럽트가 정해져 있어 */
		if (smmu->num_context_banks > smmu->num_context_irqs) {	/* [한국어] 인터럽트가 모자라면 */
			dev_err(dev,	/* [한국어] 알리고 */
			      "found only %d context irq(s) but %d required\n",
			      smmu->num_context_irqs, smmu->num_context_banks);
			return -ENODEV;	/* [한국어] 쓸 수 없다. */
		}

		/* Ignore superfluous interrupts */
		smmu->num_context_irqs = smmu->num_context_banks;	/* [한국어] 원 주석대로 남는 인터럽트는 무시한다. */
	}

	if (smmu->impl && smmu->impl->global_fault)	/* [한국어] 구현체가 처리기를 바꿨으면 */
		global_fault = smmu->impl->global_fault;	/* [한국어] 그것을 쓰고 */
	else
		global_fault = arm_smmu_global_fault;	/* [한국어] 아니면 기본 처리기. */

	for (i = 0; i < global_irqs; i++) {	/* [한국어] 전역 인터럽트마다 */
		int irq = platform_get_irq(pdev, i);	/* [한국어] 번호를 얻어 */

		if (irq < 0)	/* [한국어] 얻지 못하면 */
			return irq;	/* [한국어] 포기한다. */

		err = devm_request_irq(dev, irq, global_fault, IRQF_SHARED,	/* [한국어] 처리기를 건다. 다른 장치와 나눠 쓸 수 있다. */
				       "arm-smmu global fault", smmu);
		if (err)	/* [한국어] 걸지 못하면 */
			return dev_err_probe(dev, err,	/* [한국어] 오류를 보고할 길이 없어 포기한다. */
					"failed to request global IRQ %d (%u)\n",
					i, irq);
	}

	platform_set_drvdata(pdev, smmu);	/* [한국어] 장치에서 이 구조체를 찾을 수 있게 한다. fwnode 조회가 이것을 쓴다. */

	/* Check for RMRs and install bypass SMRs if any */
	arm_smmu_rmr_install_bypass_smr(smmu);	/* [한국어] 부팅 때부터 살아 있던 매핑을 이어받는다. */

	arm_smmu_device_reset(smmu);	/* [한국어] 하드웨어를 알려진 상태로 세우고 켠다. */
	arm_smmu_test_smr_masks(smmu);	/* [한국어] 켠 뒤에야 마스크 폭을 시험할 수 있다 — 확장 id 설정이 그 폭을 바꾼다. */

	err = iommu_device_sysfs_add(&smmu->iommu, smmu->dev, NULL,	/* [한국어] sysfs 항목을 만든다. 물리 주소를 이름에 넣어 여러 SMMU 를 구별한다. */
				     "smmu.%pa", &smmu->ioaddr);
	if (err)	/* [한국어] 실패하면 */
		return dev_err_probe(dev, err, "Failed to register iommu in sysfs\n");	/* [한국어] 포기한다. */

	err = iommu_device_register(&smmu->iommu, &arm_smmu_ops,	/* [한국어] 코어에 등록한다. 이 순간부터 장치가 붙기 시작한다. */
				    using_legacy_binding ? NULL : dev);	/* [한국어] 옛 결합은 장치가 자기 IOMMU 를 가리키지 않아 이 연결을 만들 수 없다. */
	if (err) {	/* [한국어] 등록하지 못했으면 */
		iommu_device_sysfs_remove(&smmu->iommu);	/* [한국어] sysfs 를 걷고 */
		return dev_err_probe(dev, err, "Failed to register iommu\n");	/* [한국어] 포기한다. */
	}

	/*
	 * We want to avoid touching dev->power.lock in fastpaths unless
	 * it's really going to do something useful - pm_runtime_enabled()
	 * can serve as an ideal proxy for that decision. So, conditionally
	 * enable pm_runtime.
	 */
	if (dev->pm_domain) {	/* [한국어] 원 주석대로 전원 도메인이 있을 때만 전원 관리를 켠다 — */
		pm_runtime_set_active(dev);	/* [한국어] 그러지 않으면 빠른 경로에서 락을 잡는 값만 치른다. */
		pm_runtime_enable(dev);	/* [한국어] 전원 관리를 켜고 */
		arm_smmu_rpm_use_autosuspend(smmu);	/* [한국어] 유예를 두어 상태가 튀지 않게 한다. */
	}

	return 0;	/* [한국어] 성공. */
}

/*
 * [한국어]
 * arm_smmu_device_shutdown - 시스템이 꺼질 때 SMMU 를 끈다
 *
 * @pdev: 플랫폼 장치.
 *
 * CLIENTPD 를 세워 모든 트래픽을 그냥 통과시킨다. 다음 커널(kexec 등)이
 * 자기 설정을 세울 때까지 장치들이 멈추지 않게 하려는 것이다.
 *
 * 아직 쓰이는 뱅크가 있으면 로그를 남긴다 — 그 장치들의 DMA 가 보호
 * 없이 나가게 된다는 뜻이라, 알려 두는 편이 낫다.
 */
static void arm_smmu_device_shutdown(struct platform_device *pdev)
{
	struct arm_smmu_device *smmu = platform_get_drvdata(pdev);	/* [한국어] 이 장치의 SMMU. */

	if (!bitmap_empty(smmu->context_map, ARM_SMMU_MAX_CBS))	/* [한국어] 아직 쓰이는 뱅크가 있으면 */
		dev_notice(&pdev->dev, "disabling translation\n");	/* [한국어] 그 장치들의 DMA 가 보호 없이 나가게 된다고 알린다. */

	arm_smmu_rpm_get(smmu);	/* [한국어] 레지스터를 쓰려면 켜야 한다. */
	/* Turn the thing off */
	arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_sCR0, ARM_SMMU_sCR0_CLIENTPD);	/* [한국어] 모든 트래픽을 통과시킨다 — 다음 커널이 자기 설정을 세울 때까지 장치가 멈추지 않게. */
	arm_smmu_rpm_put(smmu);	/* [한국어] 전원 참조를 놓는다. */

	if (pm_runtime_enabled(smmu->dev))	/* [한국어] 전원 관리를 쓰면 */
		pm_runtime_force_suspend(smmu->dev);	/* [한국어] 강제로 재운다. */
	else
		clk_bulk_disable(smmu->num_clks, smmu->clks);	/* [한국어] 아니면 클럭만 끈다. */

	clk_bulk_unprepare(smmu->num_clks, smmu->clks);	/* [한국어] 클럭 준비까지 푼다. */
}

/*
 * [한국어]
 * arm_smmu_device_remove - 드라이버를 걷어 낸다
 *
 * @pdev: 플랫폼 장치.
 *
 * 코어에서 등록을 푼 뒤 하드웨어를 끈다. 순서가 반대면 등록이 살아 있는
 * 동안 꺼진 하드웨어를 건드리게 된다.
 */
static void arm_smmu_device_remove(struct platform_device *pdev)
{
	struct arm_smmu_device *smmu = platform_get_drvdata(pdev);	/* [한국어] 이 장치의 SMMU. */

	iommu_device_unregister(&smmu->iommu);	/* [한국어] 코어에서 등록을 푼다 — 먼저 해야 꺼진 하드웨어를 건드리지 않는다. */
	iommu_device_sysfs_remove(&smmu->iommu);	/* [한국어] sysfs 항목을 걷는다. */

	arm_smmu_device_shutdown(pdev);	/* [한국어] 하드웨어를 끄는 부분은 같은 절차다. */
}

/*
 * [한국어]
 * arm_smmu_runtime_resume - 전원이 돌아왔을 때
 *
 * @dev: SMMU 장치.
 * @return: 0 성공, 음수면 실패.
 *
 * 클럭을 켜고 하드웨어를 다시 세운다. 레지스터 내용이 전원과 함께
 * 사라지므로, 그림자 구조체의 내용을 통째로 다시 써야 한다.
 */
static int __maybe_unused arm_smmu_runtime_resume(struct device *dev)
{
	struct arm_smmu_device *smmu = dev_get_drvdata(dev);	/* [한국어] 이 장치의 SMMU. */
	int ret;	/* [한국어] 결과 코드. */

	ret = clk_bulk_enable(smmu->num_clks, smmu->clks);	/* [한국어] 클럭을 켠다. */
	if (ret)	/* [한국어] 켜지 못하면 */
		return ret;	/* [한국어] 실패. */

	arm_smmu_device_reset(smmu);	/* [한국어] 레지스터가 전원과 함께 사라졌으므로 그림자 상태를 통째로 다시 쓴다. */

	return 0;	/* [한국어] 성공. */
}

/*
 * [한국어]
 * arm_smmu_runtime_suspend - 전원을 끌 때
 *
 * @dev: SMMU 장치.
 * @return: 늘 0.
 *
 * 클럭만 끄면 된다. 상태는 그림자 구조체에 남아 있다.
 */
static int __maybe_unused arm_smmu_runtime_suspend(struct device *dev)
{
	struct arm_smmu_device *smmu = dev_get_drvdata(dev);	/* [한국어] 이 장치의 SMMU. */

	clk_bulk_disable(smmu->num_clks, smmu->clks);	/* [한국어] 상태는 그림자 구조체에 남아 있어 클럭만 끄면 된다. */

	return 0;	/* [한국어] 늘 성공. */
}

/*
 * [한국어]
 * arm_smmu_pm_resume - 시스템 절전에서 깨어날 때
 *
 * @dev: SMMU 장치.
 * @return: 0 성공, 음수면 실패.
 *
 * 클럭 준비는 늘 하되, 실제로 켜는 것은 그 장치가 잠들어 있지 않을
 * 때만 한다 — 잠든 채로 깨어났으면 그대로 두어야 한다.
 *
 * 준비와 켜기가 나뉜 이유: 준비는 잠들 수 있는 무거운 연산이고, 켜기는
 * 원자적 문맥에서도 되는 가벼운 연산이다.
 */
static int __maybe_unused arm_smmu_pm_resume(struct device *dev)
{
	int ret;	/* [한국어] 결과 코드. */
	struct arm_smmu_device *smmu = dev_get_drvdata(dev);	/* [한국어] 이 장치의 SMMU. */

	ret = clk_bulk_prepare(smmu->num_clks, smmu->clks);	/* [한국어] 준비는 잠들 수 있는 무거운 연산이라 여기서 한다. */
	if (ret)	/* [한국어] 실패하면 */
		return ret;	/* [한국어] 포기한다. */

	if (pm_runtime_suspended(dev))	/* [한국어] 실행 중 전원 관리로 잠들어 있었으면 */
		return 0;	/* [한국어] 그대로 두어야 한다. */

	ret = arm_smmu_runtime_resume(dev);	/* [한국어] 아니면 실제로 깨운다. */
	if (ret)	/* [한국어] 실패하면 */
		clk_bulk_unprepare(smmu->num_clks, smmu->clks);	/* [한국어] 준비도 되돌린다. */

	return ret;	/* [한국어] 결과. */
}

/*
 * [한국어]
 * arm_smmu_pm_suspend - 시스템 절전에 들어갈 때
 *
 * @dev: SMMU 장치.
 * @return: 0 성공, 음수면 실패.
 *
 * 이미 잠들어 있으면 클럭 준비만 풀면 된다. 아니면 먼저 끈 뒤 푼다.
 */
static int __maybe_unused arm_smmu_pm_suspend(struct device *dev)
{
	int ret = 0;	/* [한국어] 결과 코드. */
	struct arm_smmu_device *smmu = dev_get_drvdata(dev);	/* [한국어] 이 장치의 SMMU. */

	if (pm_runtime_suspended(dev))	/* [한국어] 이미 잠들어 있으면 */
		goto clk_unprepare;	/* [한국어] 준비만 풀면 된다. */

	ret = arm_smmu_runtime_suspend(dev);	/* [한국어] 아니면 먼저 끈다. */
	if (ret)	/* [한국어] 끄지 못하면 */
		return ret;	/* [한국어] 준비를 풀지 않고 나간다. */

clk_unprepare:	/* [한국어] 두 경로가 합류한다. */
	clk_bulk_unprepare(smmu->num_clks, smmu->clks);	/* [한국어] 클럭 준비를 푼다. */
	return ret;	/* [한국어] 결과. */
}

/*
 * [한국어] 전원 관리 연산표.
 *
 * 시스템 절전과 실행 중 전원 관리를 모두 다룬다. 마지막 NULL 은 유휴
 * 콜백을 두지 않는다는 뜻이다.
 */
static const struct dev_pm_ops arm_smmu_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(arm_smmu_pm_suspend, arm_smmu_pm_resume)	/* [한국어] 시스템 절전용 콜백. */
	SET_RUNTIME_PM_OPS(arm_smmu_runtime_suspend,
			   arm_smmu_runtime_resume, NULL)
};

/*
 * [한국어] 플랫폼 드라이버 등록 정보.
 *
 * suppress_bind_attrs 가 참인 것이 중요하다 — sysfs 로 이 드라이버를
 * 손수 뗄 수 없게 막는다. 돌고 있는 장치의 IOMMU 를 걷어 내면 시스템이
 * 무너진다.
 */
static struct platform_driver arm_smmu_driver = {
	.driver	= {	/* [한국어] 드라이버 코어에 알릴 정보. */
		.name			= "arm-smmu",	/* [한국어] sysfs 와 로그에 보이는 이름. */
		.of_match_table		= arm_smmu_of_match,
		.pm			= &arm_smmu_pm_ops,
		.suppress_bind_attrs    = true,
	},
	.probe	= arm_smmu_device_probe,
	.remove = arm_smmu_device_remove,
	.shutdown = arm_smmu_device_shutdown,
};

/*
 * [한국어]
 * arm_smmu_init - 모듈 초기화
 *
 * @return: 0 성공, 음수면 실패.
 *
 * 본체 드라이버를 먼저 등록해야 구현체 모듈이 그것을 찾을 수 있다.
 * 구현체 등록에 실패하면 본체도 되돌린다.
 */
static int __init arm_smmu_init(void)
{
	int ret;	/* [한국어] 결과 코드. */

	ret = platform_driver_register(&arm_smmu_driver);	/* [한국어] 본체를 먼저 등록해야 구현체 모듈이 그것을 찾을 수 있다. */
	if (ret)	/* [한국어] 실패하면 */
		return ret;	/* [한국어] 포기한다. */

	ret = arm_smmu_impl_module_init();	/* [한국어] 구현체 모듈들을 초기화한다. */
	if (ret)	/* [한국어] 실패하면 */
		platform_driver_unregister(&arm_smmu_driver);	/* [한국어] 본체도 되돌린다. */

	return ret;	/* [한국어] 결과. */
}
module_init(arm_smmu_init);	/* [한국어] 모듈이 올라올 때 부를 함수. */

/*
 * [한국어]
 * arm_smmu_exit - 모듈 정리
 *
 * 등록의 역순으로 걷는다.
 */
static void __exit arm_smmu_exit(void)
{
	arm_smmu_impl_module_exit();	/* [한국어] 구현체를 먼저 걷고 */
	platform_driver_unregister(&arm_smmu_driver);	/* [한국어] 본체를 걷는다. 등록의 역순이다. */
}
module_exit(arm_smmu_exit);	/* [한국어] 내릴 때 부를 함수. */

MODULE_DESCRIPTION("IOMMU API for ARM architected SMMU implementations");	/* [한국어] modinfo 에 보이는 설명. */
MODULE_AUTHOR("Will Deacon <will@kernel.org>");	/* [한국어] 만든 사람. */
MODULE_ALIAS("platform:arm-smmu");	/* [한국어] 이 이름의 플랫폼 장치가 나타나면 자동으로 적재되게 한다. */
MODULE_LICENSE("GPL v2");	/* [한국어] 라이선스. GPL 전용 심볼을 쓰려면 필요하다. */
