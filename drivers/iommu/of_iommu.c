// SPDX-License-Identifier: GPL-2.0-only
/*
 * OF helpers for IOMMU
 *
 * Copyright (c) 2012, NVIDIA CORPORATION.  All rights reserved.
 */

/*
 * [한국어 설명] 장치 트리 기반 IOMMU 설정 (drivers/iommu/of_iommu.c)
 *
 * === 파일의 역할 ===
 * 장치 트리(Device Tree)가 기술한 "이 장치는 저 IOMMU 아래이고, 하드웨어에는
 * 이런 ID 로 보인다"는 정보를 읽어 fwspec 으로 옮기는 계층이다. ARM/RISC-V
 * 임베디드 시스템에서 장치가 IOMMU 를 만나는 첫 지점이며, x86 에서 ACPI DMAR/IVRS
 * 가 하는 일을 DT 로 하는 셈이다.
 *
 * DT 는 두 가지 방식으로 이 관계를 기술한다.
 *  - iommus = <&smmu 0x1234>: 장치 노드가 직접 자기 IOMMU 와 ID 를 적는다.
 *    플랫폼 장치가 쓰는 방식이다.
 *  - iommu-map: 버스 노드가 "requester id 범위 → IOMMU 와 ID" 매핑을 적는다.
 *    PCI 처럼 장치가 동적으로 나타나는 버스에서 쓴다.
 *
 * ID 를 그대로 쓰지 않고 드라이버의 of_xlate 콜백을 거치는 것이 요점이다. DT 의
 * 인자 개수와 의미는 IOMMU 마다 다르고(SMMU 는 StreamID 하나, 어떤 것은 마스크까지),
 * 그 해석은 드라이버만 할 수 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 흐름: 장치 등록 → bus->dma_configure → of_dma_configure
 *         → [이 파일] of_iommu_configure
 *           → of_iommu_xlate : fwspec 생성 + 드라이버 of_xlate 로 ID 등록
 *         → iommu.c iommu_probe_device : 그 fwspec 으로 드라이버를 찾아 프로브
 *
 * PCI 장치는 한 단계가 더 있다. 장치가 여러 DMA 별칭을 낼 수 있으므로
 * pci_for_each_dma_alias 로 각 별칭마다 ID 를 등록한다 — 그래야 어느 requester id
 * 로 오든 IOMMU 가 같은 장치로 인식한다.
 *
 * === 타 모듈과의 연결 ===
 * - iommu.c: iommu_fwspec_init/add_ids 로 fwspec 을 채우고, iommu_probe_device 로
 *   프로브를 시작한다. iommu_probe_device_lock 을 공유해 경쟁을 막는다.
 * - 벤더 드라이버: of_xlate 콜백에서 DT 인자를 자기 ID 형식으로 해석한다.
 * - reserved-memory: DT 의 memory-region 과 iommu-addresses 로 기술된 예약 구간을
 *   읽어 IOMMU 예약 영역으로 옮긴다.
 *
 * === 주요 함수/구조체 요약 ===
 * - of_iommu_xlate()            : fwspec 을 만들고 드라이버 of_xlate 를 부른다.
 * - of_iommu_configure_dev()    : iommus 속성을 순회하며 ID 를 등록.
 * - of_iommu_configure_dev_id() : iommu-map 으로 ID 를 변환해 등록.
 * - of_iommu_configure()        : 진입점. PCI 면 별칭마다 반복한다.
 * - of_iommu_get_resv_regions() : DT 예약 메모리를 IOMMU 예약 구간으로.
 */
#include <linux/export.h>	/* [한국어] EXPORT_SYMBOL */
#include <linux/iommu.h>	/* [한국어] fwspec API */
#include <linux/limits.h>	/* [한국어] 크기 상한 */
#include <linux/module.h>	/* [한국어] 드라이버 모듈 참조 */
#include <linux/of.h>	/* [한국어] 장치 트리 파싱 */
#include <linux/of_address.h>	/* [한국어] 예약 메모리의 주소 변환 */
#include <linux/of_iommu.h>	/* [한국어] 이 파일의 공개 선언 */
#include <linux/of_pci.h>	/* [한국어] PCI 노드의 iommu-map 처리 */
#include <linux/pci.h>	/* [한국어] DMA 별칭 순회 */
#include <linux/slab.h>	/* [한국어] 예약 구간 객체 할당 */
#include <linux/fsl/mc.h>	/* [한국어] Freescale 관리 복합체 버스 */

#include "iommu-priv.h"	/* [한국어] iommu_probe_device_lock 등 내부 선언 */

/*
 * [한국어]
 * of_iommu_xlate - DT 항목 하나를 fwspec 으로 옮긴다
 *
 * @dev:        설정할 장치
 * @iommu_spec: DT 에서 읽은 IOMMU 노드와 인자들
 * @return:     0 성공, -ENODEV 면 대상이 아님, -EPROBE_DEFER 면 나중에 다시
 *
 * DT 가 기술한 "저 IOMMU, 이런 ID" 를 커널 자료구조로 바꾸는 곳이다. 두 단계로
 * 나뉜다 — fwspec 을 만들어 IOMMU 노드에 묶고(iommu_fwspec_init), 그 다음 드라이버의
 * of_xlate 로 인자를 해석하게 한다.
 *
 * 인자 해석을 드라이버에 맡기는 이유는 형식이 IOMMU 마다 다르기 때문이다. SMMU 는
 * StreamID 하나, 어떤 것은 ID 와 마스크 쌍, 또 어떤 것은 컨텍스트 뱅크 번호까지
 * 받는다. #iommu-cells 속성이 그 개수를 선언하고, 의미는 드라이버만 안다.
 *
 * 모듈 참조를 콜백 동안만 잡는 것에 주의할 것. 이 시점에는 아직 장치가 그
 * 드라이버에 붙지 않았으므로, 장기 참조는 나중에 iommu_init_device 가 잡는다.
 *
 * 실행 컨텍스트: 프로브 경로. iommu_probe_device_lock 아래. 프로세스 문맥.
 *
 * 호출 체인: of_iommu_configure_dev(_id) → [이 함수] → ops->of_xlate
 */
static int of_iommu_xlate(struct device *dev,
			  struct of_phandle_args *iommu_spec)
{
	const struct iommu_ops *ops;	/* [한국어] 이 IOMMU 노드에 해당하는 드라이버 */
	int ret;	/* [한국어] 각 단계의 결과 */

	if (!of_device_is_available(iommu_spec->np))	/* [한국어] DT 에서 status = disabled 로 꺼 둔 IOMMU */
		return -ENODEV;	/* [한국어] 이 장치는 IOMMU 없이 동작한다 */

	ret = iommu_fwspec_init(dev, of_fwnode_handle(iommu_spec->np));	/* [한국어] 이 장치를 그 IOMMU 노드에 묶는다. 드라이버가 아직 등록되지 않았으면 여기서 -EPROBE_DEFER 가 나온다 */
	if (ret)	/* [한국어] 묶기 실패 또는 프로브 연기 */
		return ret;	/* [한국어] 그대로 올린다 */

	ops = iommu_ops_from_fwnode(&iommu_spec->np->fwnode);	/* [한국어] 그 노드를 맡은 드라이버 */
	if (!ops->of_xlate || !try_module_get(ops->owner))	/* [한국어] DT 해석 콜백이 없거나 모듈이 내려가는 중 */
		return -ENODEV;	/* [한국어] ID 를 등록할 수 없다 */

	ret = ops->of_xlate(dev, iommu_spec);	/* [한국어] 드라이버가 DT 인자를 자기 ID 형식으로 해석해 fwspec 에 넣는다. 인자의 개수와 의미가 IOMMU 마다 달라 이 해석은 드라이버만 할 수 있다 */
	module_put(ops->owner);	/* [한국어] 콜백이 끝났으므로 모듈 참조를 놓는다 */
	return ret;	/* [한국어] 0 이면 이 IOMMU 항목이 등록되었다 */
}

/*
 * [한국어]
 * of_iommu_configure_dev_id - iommu-map 으로 ID 를 변환해 등록한다
 *
 * @master_np: 매핑 표를 담은 버스 노드
 * @dev:       설정할 장치
 * @id:        입력 ID (PCI 라면 requester id)
 * @return:    0 성공, 음수 실패
 *
 * PCI 처럼 장치가 동적으로 나타나는 버스를 위한 경로다. 장치 노드가 DT 에 없을 수
 * 있으므로, 버스 노드가 "이 ID 범위는 저 IOMMU 의 이 ID 로" 라는 표를 대신 들고
 * 있고 of_map_id 가 그것을 뒤진다.
 *
 * iommu-map-mask 는 하위 비트를 걸러 여러 함수를 한 항목으로 묶을 때 쓴다 —
 * 다기능 장치의 모든 함수가 같은 IOMMU ID 를 쓰는 흔한 구성이다.
 *
 * 실행 컨텍스트: 프로브 경로. 프로세스 문맥.
 *
 * 호출 체인: of_pci_iommu_init, of_iommu_configure_device → [이 함수]
 */
static int of_iommu_configure_dev_id(struct device_node *master_np,
				     struct device *dev,
				     const u32 *id)
{
	struct of_phandle_args iommu_spec = { .args_count = 1 };	/* [한국어] iommu-map 은 언제나 ID 하나를 낸다 */
	int err;	/* [한국어] 변환 결과 */

	err = of_map_id(master_np, *id, "iommu-map",	/* [한국어] 버스 노드의 매핑 표에서 이 ID 를 찾는다. PCI 라면 requester id 가 입력이다 */
			 "iommu-map-mask", &iommu_spec.np,	/* [한국어] 마스크 속성으로 하위 비트를 걸러 낸 뒤 매핑한다 */
			 iommu_spec.args);	/* [한국어] 변환된 IOMMU 쪽 ID 를 여기에 받는다 */
	if (err)	/* [한국어] 이 ID 에 대한 매핑이 없다 */
		return err;	/* [한국어] IOMMU 아래가 아닌 장치 */

	err = of_iommu_xlate(dev, &iommu_spec);	/* [한국어] 변환된 ID 로 fwspec 을 채운다 */
	of_node_put(iommu_spec.np);	/* [한국어] of_map_id 가 잡은 노드 참조 반납 */
	return err;	/* [한국어] 등록 결과 */
}

/*
 * [한국어]
 * of_iommu_configure_dev - iommus 속성을 순회하며 ID 를 등록한다
 *
 * @master_np: 장치 노드
 * @dev:       설정할 장치
 * @return:    0 성공, -ENODEV 면 iommus 속성이 없다
 *
 * 플랫폼 장치를 위한 경로다. 장치 노드가 DT 에 직접 있으므로 자기 IOMMU 와 ID 를
 * iommus = <&smmu 0x1234> 형태로 적어 둔다.
 *
 * 항목이 여럿일 수 있다는 점이 중요하다. 하나의 SoC 블록이 여러 마스터 포트를
 * 갖거나(디스플레이 컨트롤러의 여러 레이어), 서로 다른 IOMMU 를 동시에 지나는
 * 구성이 있어, 순회하며 모두 등록한다.
 *
 * err 초기값이 -ENODEV 인 것이 계약의 일부다 — 항목이 하나도 없으면 그 값이
 * 그대로 나가 "IOMMU 아래가 아님"을 뜻한다.
 *
 * 실행 컨텍스트: 프로브 경로. 프로세스 문맥.
 *
 * 호출 체인: of_iommu_configure_device → [이 함수] → of_iommu_xlate
 */
static int of_iommu_configure_dev(struct device_node *master_np,
				  struct device *dev)
{
	struct of_phandle_args iommu_spec;	/* [한국어] 순회하며 채울 IOMMU 항목 */
	int err = -ENODEV, idx = 0;	/* [한국어] 기본값은 '없음'. 항목이 하나도 없으면 그대로 나간다 */

	while (!of_parse_phandle_with_args(master_np, "iommus",	/* [한국어] iommus 속성의 항목들을 하나씩 */
					   "#iommu-cells",	/* [한국어] 각 항목의 인자 개수는 IOMMU 노드가 선언한다 */
					   idx, &iommu_spec)) {	/* [한국어] idx 번째 항목 */
		err = of_iommu_xlate(dev, &iommu_spec);	/* [한국어] 그 항목을 fwspec 에 등록 */
		of_node_put(iommu_spec.np);	/* [한국어] 파싱이 잡은 노드 참조 반납 */
		idx++;	/* [한국어] 다음 항목 */
		if (err)	/* [한국어] 등록 실패 또는 프로브 연기 */
			break;	/* [한국어] 중단 — 부분 등록 상태는 호출자가 정리한다 */
	}

	return err;	/* [한국어] 마지막 항목의 결과. 하나도 없었으면 -ENODEV */
}

/*
 * [한국어] PCI 별칭 순회 콜백에 넘길 문맥.
 * pci_for_each_dma_alias 의 콜백 시그니처가 void* 하나뿐이라, 장치와 버스 노드
 * 두 값을 함께 넘기려면 이렇게 묶어야 한다.
 */
struct of_pci_iommu_alias_info {
	struct device *dev;
	/* [한국어] 별칭마다 IOMMU ID 를 등록할 대상 장치.
	 * 설정자: of_iommu_configure() 가 순회를 시작하기 전에 채운다.
	 * 읽는 자: of_pci_iommu_init() 콜백이 찾아낸 ID 를 이 장치에 붙일 때.
	 * 왜 구조체로 묶어 넘기는가: pci_for_each_dma_alias() 의 콜백은 void 포인터
	 *   하나만 받는다. 장치와 아래 버스 노드 두 값을 함께 넘기려면 묶는 수밖에 없다.
	 * 값 범위: NULL 이 아니다. 순회하는 동안 바뀌지 않는다.
	 * 왜 별칭을 순회하는가: PCI-to-PCI 브리지 뒤의 장치나 위상 정보가 없는
	 *   장치는 IOMMU 에 자기 BDF 가 아닌 다른 요청자 id 로 나타난다. 그 모든
	 *   가능한 id 를 등록해야 어느 것으로 오든 이 장치로 인식된다. */
	struct device_node *np;
	/* [한국어] iommu-map 표를 담고 있는 버스(호스트 브리지) 노드.
	 * 설정자: of_iommu_configure() 가 장치의 상위 버스 노드를 찾아 담는다.
	 * 읽는 자: 콜백이 of_map_id() 로 이 노드의 iommu-map 표를 뒤져, 별칭 id 를
	 *   IOMMU 의 id 로 옮길 때.
	 * 왜 장치 노드가 아니라 버스 노드인가: PCI 장치는 대개 장치 트리에 노드가
	 *   없다 — 열거로 발견되기 때문이다. 그래서 대응 표는 호스트 브리지 노드에
	 *   놓이고, 그 표가 요청자 id 범위를 IOMMU id 범위로 옮긴다.
	 * 값 범위: NULL 이 아니다. */
};

/*
 * [한국어]
 * of_pci_iommu_init - 별칭 하나에 대해 IOMMU ID 를 등록한다 (별칭 순회 콜백)
 *
 * @pdev:   순회 중인 PCI 장치 (여기서는 쓰지 않는다)
 * @alias:  이 장치가 낼 수 있는 requester id 중 하나
 * @data:   장치와 버스 노드를 담은 문맥
 * @return: 0 성공, 음수면 순회 중단
 *
 * 하나의 PCI 장치가 여러 requester id 로 DMA 를 낼 수 있다는 사실이 이 콜백의
 * 이유다 — PCIe-PCI 브리지 뒤의 장치는 브리지의 id 로 보이고, 일부 장치는
 * 하드웨어 버그로 남의 id 를 쓴다. IOMMU 가 그 모두를 같은 장치로 인식해야
 * 격리가 성립하므로, 별칭마다 ID 를 등록한다.
 *
 * 실행 컨텍스트: 프로브 경로. 프로세스 문맥.
 *
 * 호출 체인: pci_for_each_dma_alias → [이 함수] → of_iommu_configure_dev_id
 */
static int of_pci_iommu_init(struct pci_dev *pdev, u16 alias, void *data)
{
	struct of_pci_iommu_alias_info *info = data;	/* [한국어] 콜백 시그니처가 void* 하나뿐이라 구조체로 묶어 받는다 */
	u32 input_id = alias;	/* [한국어] 별칭(requester id)이 매핑 표의 입력이다 */

	return of_iommu_configure_dev_id(info->np, info->dev, &input_id);	/* [한국어] 이 별칭에 해당하는 IOMMU ID 를 등록한다. 별칭마다 부르므로 하나의 장치가 여러 ID 를 갖게 된다 */
}

/*
 * [한국어]
 * of_iommu_configure_device - 두 기술 방식 중 하나를 고른다
 *
 * @master_np: DT 노드
 * @dev:       설정할 장치
 * @id:        ID 가 주어졌으면 iommu-map 방식, NULL 이면 iommus 방식
 * @return:    0 성공, 음수 실패
 *
 * DT 가 IOMMU 관계를 기술하는 두 방식의 분기점이다. ID 의 유무가 곧 방식의
 * 차이를 나타낸다 — 버스가 ID 를 알려 주면 매핑 표를, 아니면 장치 노드의 속성을
 * 본다.
 *
 * 실행 컨텍스트: 프로브 경로.
 *
 * 호출 체인: of_iommu_configure → [이 함수]
 */
static int of_iommu_configure_device(struct device_node *master_np,
				     struct device *dev, const u32 *id)
{
	return (id) ? of_iommu_configure_dev_id(master_np, dev, id) :	/* [한국어] ID 가 주어졌으면 iommu-map 방식 */
		      of_iommu_configure_dev(master_np, dev);	/* [한국어] 아니면 장치 노드의 iommus 속성 방식 */
}

/*
 * [한국어]
 * of_pci_check_device_ats - 루트 컴플렉스의 ATS 지원 여부를 fwspec 에 남긴다
 *
 * @dev: 설정 중인 장치
 * @np:  PCI 호스트 브리지 노드
 *
 * ATS(Address Translation Services)는 장치가 번역 결과를 자기 캐시(ATC)에 들고
 * 있게 해 IOMMU 왕복을 줄이는 기능이다. 성능 이득이 크지만 장치만 지원한다고
 * 되는 것이 아니라 루트 컴플렉스가 그 프로토콜을 중계해야 하고, DT 에서는 그
 * 사실을 ats-supported 속성으로 알린다.
 *
 * 여기서 플래그만 남기고 실제 활성화는 드라이버가 한다.
 *
 * 실행 컨텍스트: 프로브 경로.
 *
 * 호출 체인: of_iommu_configure → [이 함수]
 */
static void of_pci_check_device_ats(struct device *dev, struct device_node *np)
{
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);	/* [한국어] 방금 만들어진 fwspec */

	if (fwspec && of_property_read_bool(np, "ats-supported"))	/* [한국어] 루트 컴플렉스가 ATS 를 지원한다고 DT 에 적혀 있으면 */
		fwspec->flags |= IOMMU_FWSPEC_PCI_RC_ATS;	/* [한국어] 드라이버가 이 장치에 ATS 를 켤 수 있음을 알린다. ATS 는 장치가 번역 결과를 자기 캐시에 들고 있게 해 IOMMU 왕복을 줄이지만, 루트 컴플렉스가 그 프로토콜을 지원해야 한다 */
}

/*
 * Returns:
 *  0 on success, an iommu was configured
 *  -ENODEV if the device does not have any IOMMU
 *  -EPROBEDEFER if probing should be tried again
 *  -errno fatal errors
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * of_iommu_configure - DT 정보로 장치의 IOMMU 관계를 설정한다 (진입점)
 *
 * @dev:       설정할 장치
 * @master_np: 관계를 기술한 DT 노드
 * @id:        PCI 라면 requester id, 아니면 NULL
 * @return:    0 성공, -ENODEV 면 IOMMU 아래가 아님, -EPROBE_DEFER 면 나중에 다시
 *
 * 버스의 dma_configure 경로에서 불린다. 장치가 IOMMU 를 만나는 첫 지점이며,
 * 여기서 fwspec 이 채워져야 이후 iommu_probe_device 가 드라이버를 찾을 수 있다.
 *
 * PCI 와 플랫폼 장치의 처리가 갈린다. PCI 는 별칭마다 ID 를 등록해야 하고,
 * 동시에 ACS 요청과 ATS 확인이 필요하다. 위 영어 주석이 말하듯 부모 노드를 거슬러
 * 올라가며 IOMMU 를 찾지는 않는다 — DT 바인딩이 그렇게 정의되어 있다.
 *
 * 되감기가 두 갈래인 것이 이 함수의 미묘한 부분이다. 들어올 때 이미 장치 상태가
 * 있었다면 우리가 만든 fwspec 만 거두고, 없었다면 상태까지 거둔다. 남의 것을
 * 지우지 않기 위한 구분이다.
 *
 * 실행 컨텍스트: 장치 등록 경로. 프로세스 문맥, 뮤텍스를 잡는다.
 *
 * 호출 체인: 버스의 dma_configure → of_dma_configure → [이 함수]
 */
int of_iommu_configure(struct device *dev, struct device_node *master_np,
		       const u32 *id)
{
	bool dev_iommu_present;	/* [한국어] 이 함수에 들어올 때 이미 장치 상태가 있었는지. 실패 시 무엇까지 되돌릴지를 이 값이 정한다 */
	int err;	/* [한국어] 설정 결과 */

	if (!master_np)	/* [한국어] IOMMU 관계를 기술한 노드가 없다 */
		return -ENODEV;	/* [한국어] IOMMU 아래가 아닌 장치 */

	/* Serialise to make dev->iommu stable under our potential fwspec */
	mutex_lock(&iommu_probe_device_lock);	/* [한국어] fwspec 조작을 프로브 경로와 직렬화한다 (위 영어 주석) */
	if (dev_iommu_fwspec_get(dev)) {	/* [한국어] 이미 설정된 장치 — 재생(replay) 호출이거나 중복 */
		mutex_unlock(&iommu_probe_device_lock);	/* [한국어] 락만 놓고 */
		return 0;	/* [한국어] 성공으로 간주한다 */
	}
	dev_iommu_present = dev->iommu;	/* [한국어] 되감기 범위 판단의 기준. 이미 상태가 있었다면 fwspec 만 거두고, 없었다면 상태 자체를 거둔다 */

	/*
	 * We don't currently walk up the tree looking for a parent IOMMU.
	 * See the `Notes:' section of
	 * Documentation/devicetree/bindings/iommu/iommu.txt
	 */
	if (dev_is_pci(dev)) {	/* [한국어] PCI 장치 */
		struct of_pci_iommu_alias_info info = {	/* [한국어] 별칭 콜백에 넘길 문맥 */
			.dev = dev,	/* [한국어] 대상 장치 */
			.np = master_np,	/* [한국어] 매핑 표를 담은 버스 노드 */
		};

		pci_request_acs();	/* [한국어] PCI 코어에 ACS 를 켜 달라고 요청한다. IOMMU 를 쓰는 시스템에서는 장치 간 P2P 를 막아야 격리가 성립하며, 이 호출이 그 요청을 남긴다 */
		err = pci_for_each_dma_alias(to_pci_dev(dev),	/* [한국어] 이 장치가 낼 수 있는 모든 requester id 에 대해 */
					     of_pci_iommu_init, &info);	/* [한국어] 각각 IOMMU ID 를 등록한다. 브리지 뒤의 장치는 브리지의 id 로도 DMA 를 내므로, 그 모두를 IOMMU 가 알아야 한다 */
		of_pci_check_device_ats(dev, master_np);	/* [한국어] ATS 지원 여부를 fwspec 플래그로 남긴다 */
	} else {
		err = of_iommu_configure_device(master_np, dev, id);	/* [한국어] 플랫폼 장치 — 별칭 개념이 없으므로 한 번만 */
	}

	if (err && dev_iommu_present)	/* [한국어] 실패했고, 들어올 때 이미 장치 상태가 있었다면 */
		iommu_fwspec_free(dev);	/* [한국어] 우리가 만든 fwspec 만 거둔다 — 상태 자체는 다른 주인의 것이다 */
	else if (err && dev->iommu)	/* [한국어] 실패했고, 우리가 상태까지 만들었다면 */
		dev_iommu_free(dev);	/* [한국어] 상태를 통째로 거둔다 */
	mutex_unlock(&iommu_probe_device_lock);	/* [한국어] 설정 구간 끝 */

	/*
	 * If we're not on the iommu_probe_device() path (as indicated by the
	 * initial dev->iommu) then try to simulate it. This should no longer
	 * happen unless of_dma_configure() is being misused outside bus code.
	 */
	if (!err && dev->bus && !dev_iommu_present)	/* [한국어] 성공했고, 들어올 때 상태가 없었다 = 프로브 경로에서 불린 것이 아니다 */
		err = iommu_probe_device(dev);	/* [한국어] 프로브를 흉내 낸다. 위 영어 주석대로 이제는 일어나지 않아야 하는 경로이며, 버스 코드 밖에서 of_dma_configure 를 잘못 쓴 경우다 */

	if (err && err != -EPROBE_DEFER)	/* [한국어] 진짜 실패면 (프로브 연기는 정상 상황이다) */
		dev_dbg(dev, "Adding to IOMMU failed: %d\n", err);	/* [한국어] 디버그 수준으로만 남긴다 — IOMMU 없이 동작하는 장치가 많아 경고로 올리면 로그가 시끄러워진다 */

	return err;	/* [한국어] 0 / -ENODEV / -EPROBE_DEFER / 그 외 실패 (위 영어 주석) */
}

/*
 * [한국어]
 * iommu_resv_region_get_type - 예약 구간이 직통 매핑인지 단순 예약인지 판정한다
 *
 * @dev:    대상 장치
 * @phys:   DT 가 기술한 물리 영역 (없으면 start >= end)
 * @start:  IOVA 구간의 시작
 * @length: 길이
 * @return: IOMMU_RESV_DIRECT 또는 IOMMU_RESV_RESERVED
 *
 * 두 종류의 차이가 실질적이다. DIRECT 는 IOVA == 물리인 매핑을 실제로 만들어야
 * 하는 구간이고(펌웨어가 그 주소로 계속 접근하고 있다), RESERVED 는 그냥 그
 * 주소를 내주지 말라는 뜻이다.
 *
 * IOVA 와 물리가 다르게 기술된 경우는 지금 코드가 다루지 못한다 — 그런 변환을
 * 만들려면 임의의 오프셋 매핑을 지원해야 하는데, 경고를 남기고 단순 예약으로
 * 낮춘다.
 *
 * 실행 컨텍스트: 예약 구간 조회. 프로세스 문맥.
 *
 * 호출 체인: of_iommu_get_resv_regions → [이 함수]
 */
static enum iommu_resv_type __maybe_unused
iommu_resv_region_get_type(struct device *dev,
			   struct resource *phys,
			   phys_addr_t start, size_t length)
{
	phys_addr_t end = start + length - 1;	/* [한국어] IOVA 구간의 마지막 주소 */

	/*
	 * IOMMU regions without an associated physical region cannot be
	 * mapped and are simply reservations.
	 */
	if (phys->start >= phys->end)	/* [한국어] 연결된 물리 영역이 없다 (reg 속성이 없는 예약 노드) */
		return IOMMU_RESV_RESERVED;	/* [한국어] 매핑할 대상이 없으므로 그냥 '쓰지 말 것'이다 (위 영어 주석) */

	/* may be IOMMU_RESV_DIRECT_RELAXABLE for certain cases */
	if (start == phys->start && end == phys->end)	/* [한국어] IOVA 구간과 물리 구간이 정확히 같다 */
		return IOMMU_RESV_DIRECT;	/* [한국어] 항등 매핑을 만들어야 한다 — 펌웨어가 그 주소로 계속 접근하고 있다는 뜻 */

	dev_warn(dev, "treating non-direct mapping [%pr] -> [%pap-%pap] as reservation\n", phys,	/* [한국어] IOVA 와 물리가 다르게 기술되었다. 지금 코드는 그런 변환을 만들 수 없다 */
		 &start, &end);	/* [한국어] 문제의 두 구간 */
	return IOMMU_RESV_RESERVED;	/* [한국어] 매핑은 못 만들어도 그 주소를 비워 두기는 한다 */
}

/**
 * of_iommu_get_resv_regions - reserved region driver helper for device tree
 * @dev: device for which to get reserved regions
 * @list: reserved region list
 *
 * IOMMU drivers can use this to implement their .get_resv_regions() callback
 * for memory regions attached to a device tree node. See the reserved-memory
 * device tree bindings on how to use these:
 *
 *   Documentation/devicetree/bindings/reserved-memory/reserved-memory.txt
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * of_iommu_get_resv_regions - DT 예약 메모리를 IOMMU 예약 구간으로 옮긴다
 *
 * @dev:  대상 장치
 * @list: 예약 구간을 매달 목록
 *
 * DT 의 reserved-memory 노드가 "이 장치가 이 주소를 계속 쓰고 있다"고 기술하면,
 * IOMMU 는 그 IOVA 를 다른 용도로 내주면 안 된다. 대표적인 것이 부트로더가 켜
 * 두고 커널로 넘긴 디스플레이 프레임버퍼다 — 커널이 그 매핑을 끊는 순간 화면이
 * 깨진다.
 *
 * 파싱이 두 겹인 이유가 있다. memory-region 은 물리 영역을 가리키고,
 * iommu-addresses 는 "그 영역이 어느 장치의 어느 IOVA 에 대응하는가"를 적는다.
 * 한 예약 영역을 여러 장치가 서로 다른 IOVA 로 볼 수 있어, 이 장치에 해당하는
 * 항목만 골라야 한다.
 *
 * 실행 컨텍스트: 프로브 경로. 프로세스 문맥, 잠들 수 있다.
 *
 * 호출 체인: 벤더 드라이버의 get_resv_regions → iommu_dma_get_resv_regions
 *            → [이 함수] → iommu_alloc_resv_region
 */
void of_iommu_get_resv_regions(struct device *dev, struct list_head *list)
{
#if IS_ENABLED(CONFIG_OF_ADDRESS)	/* [한국어] 주소 변환 지원이 없는 빌드에서는 예약 구간을 해석할 수 없다 */
	struct of_phandle_iterator it;	/* [한국어] memory-region 참조를 순회하는 반복자 */
	int err;	/* [한국어] 파싱 결과 */

	of_for_each_phandle(&it, err, dev->of_node, "memory-region", NULL, 0) {	/* [한국어] 이 장치가 참조하는 예약 메모리 노드들 */
		const __be32 *maps, *end;	/* [한국어] iommu-addresses 속성을 훑을 커서 */
		struct resource phys;	/* [한국어] 그 노드가 기술한 물리 영역 */
		int size;	/* [한국어] 속성의 바이트 크기 */

		memset(&phys, 0, sizeof(phys));	/* [한국어] reg 가 없는 노드를 위해 0 으로 시작 — start >= end 가 되어 '물리 영역 없음'을 뜻한다 */

		/*
		 * The "reg" property is optional and can be omitted by reserved-memory regions
		 * that represent reservations in the IOVA space, which are regions that should
		 * not be mapped.
		 */
		if (of_property_present(it.node, "reg")) {	/* [한국어] 물리 영역이 기술되어 있으면 (위 영어 주석: 선택적이다) */
			err = of_address_to_resource(it.node, 0, &phys);	/* [한국어] 그 주소를 읽는다 */
			if (err < 0) {	/* [한국어] 파싱 실패 */
				dev_err(dev, "failed to parse memory region %pOF: %d\n",	/* [한국어] DT 기술이 잘못되었다 */
					it.node, err);	/* [한국어] 문제의 노드 */
				continue;	/* [한국어] 이 노드는 건너뛴다 */
			}
		}

		maps = of_get_property(it.node, "iommu-addresses", &size);	/* [한국어] 이 예약 영역이 어느 장치의 어느 IOVA 에 대응하는지를 담은 속성 */
		if (!maps)	/* [한국어] IOMMU 와 무관한 예약 메모리 */
			continue;	/* [한국어] 건너뛴다 */

		end = maps + size / sizeof(__be32);	/* [한국어] 속성의 끝 */

		while (maps < end) {	/* [한국어] 항목들을 순회한다 */
			struct device_node *np;	/* [한국어] 이 항목이 가리키는 장치 노드 */
			u32 phandle;	/* [한국어] 그 노드의 참조 번호 */

			phandle = be32_to_cpup(maps++);	/* [한국어] DT 는 빅엔디안이므로 변환하며 읽는다 */
			np = of_find_node_by_phandle(phandle);	/* [한국어] 그 노드를 찾는다 */

			if (np == dev->of_node) {	/* [한국어] 이 항목이 우리 장치에 대한 것이면 */
				int prot = IOMMU_READ | IOMMU_WRITE;	/* [한국어] 예약 구간은 읽기·쓰기 모두 허용한다 */
				struct iommu_resv_region *region;	/* [한국어] 만들 예약 구간 */
				enum iommu_resv_type type;	/* [한국어] 직통 매핑인지 단순 예약인지 */
				phys_addr_t iova;	/* [한국어] 이 구간의 IOVA */
				size_t length;	/* [한국어] 길이 */

				if (of_dma_is_coherent(dev->of_node))	/* [한국어] 이 장치가 캐시 일관성을 갖는다면 */
					prot |= IOMMU_CACHE;	/* [한국어] 캐시 가능으로 매핑한다 */

				maps = of_translate_dma_region(np, maps, &iova, &length);	/* [한국어] DT 셀에서 IOVA 와 길이를 읽고 커서를 전진시킨다 */
				if (length == 0) {	/* [한국어] 길이 0 짜리 구간 */
					dev_warn(dev, "Cannot reserve IOVA region of 0 size\n");	/* [한국어] DT 기술 오류 */
					continue;	/* [한국어] 건너뛴다 */
				}
				type = iommu_resv_region_get_type(dev, &phys, iova, length);	/* [한국어] 물리 영역과 대조해 종류를 정한다 */

				region = iommu_alloc_resv_region(iova, length, prot, type,	/* [한국어] 예약 구간 객체를 만든다 */
								 GFP_KERNEL);	/* [한국어] 프로세스 문맥 */
				if (region)	/* [한국어] 생성에 성공했으면 */
					list_add_tail(&region->list, list);	/* [한국어] 드라이버가 코어에 돌려줄 목록에 넣는다 */
			}
		}
	}
#endif
}
EXPORT_SYMBOL(of_iommu_get_resv_regions);	/* [한국어] 벤더 드라이버가 자기 get_resv_regions 콜백에서 부른다 */
