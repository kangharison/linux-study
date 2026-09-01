// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015 Broadcom Corporation
 */

/*
 * [한국어 설명] iProc PCIe 컨트롤러의 디바이스 트리 플랫폼 결합 계층 (pcie-iproc-platform.c)
 *
 * === 파일의 역할 ===
 * Broadcom iProc 계열 SoC 의 PCIe 호스트 컨트롤러를 디바이스 트리(DT)로 기술된
 * 플랫폼 버스에 붙여 주는 145줄 미만의 결합(glue) 드라이버다. 컨트롤러를 실제로
 * 다루는 코드 — 레지스터 초기화, 링크 학습 대기, config 공간 접근, 주소 창 설정,
 * MSI 배선 — 는 한 줄도 여기에 없고 전부 이웃 파일 pcie-iproc.c 에 모여 있다.
 *
 * 이 파일이 하는 일은 셋이다.
 *   1) DT 가 말하는 것을 공용 구조체 struct iproc_pcie 의 필드로 옮겨 담는 번역.
 *      compatible 문자열 -> type, reg 속성 -> base/base_addr,
 *      brcm,pcie-ob -> need_ob_cfg 와 ob.axi_offset, dma-ranges -> need_ib_cfg,
 *      pcie-phy -> phy.
 *   2) PAXC 계열에서 레거시 INTx 사상을 끄는 것(다만 아래 관찰 참조).
 *   3) 나머지를 iproc_pcie_setup() 에 통째로 위임하는 것.
 *
 * 같은 컨트롤러를 SoC 내부 BCMA 버스에 붙이는 짝이 pcie-iproc-bcma.c 이며,
 * 두 파일은 공용 코어를 사이에 두고 완전히 대칭이다. 대칭의 축은
 * "같은 하드웨어를 어느 버스가 발견하고, 그 버스가 아는 것을 어떻게 번역하는가" 다.
 *
 *   이 파일(platform)        pcie-iproc-bcma.c
 *   ----------------------   ------------------------------
 *   platform_driver          bcma_driver
 *   DT compatible 로 매칭     제조사/코어 ID 로 매칭
 *   type 을 match data 에서   type 을 IPROC_PCIE_PAXB_BCMA 로 고정
 *   reg 속성 -> ioremap       bdev->io_addr 를 그대로(이미 매핑돼 있다)
 *   메모리 창은 DT ranges     128MB 윈도를 코드로 조립
 *   map_irq 를 세우지 않음    iproc_bcma_pcie_map_irq 를 직접 등록
 *   PHY 선택적 사용           PHY 없음
 *   변종 넷을 지원            PAXB_BCMA 하나만
 *
 * === 전체 아키텍처에서의 위치 ===
 * 계층을 위에서부터 보면 PCI 코어(drivers/pci/probe.c 등) -> 호스트 브리지 추상화
 * (pci_host_bridge) -> iProc 공용 컨트롤러 코어(pcie-iproc.c) -> 버스 결합 계층
 * (이 파일) -> 플랫폼 버스 -> SoC 하드웨어 순이다. 이 파일은 아래에서 두 번째,
 * 하드웨어에 가장 가까운 소프트웨어 층이면서도 정작 하드웨어를 직접 만지지는 않는다.
 *
 * 정방향:
 *   플랫폼 버스가 DT 노드의 compatible 을 아래 매칭 표와 대조
 *     -> [이 파일] iproc_pltfm_pcie_probe()
 *        -> devm_pci_alloc_host_bridge()  브리지 + iproc_pcie private 를 함께 할당
 *        -> DT 속성을 struct iproc_pcie 로 번역
 *        -> iproc_pcie_setup() [pcie-iproc.c:2782]
 *           -> 변종 판정, PERST, 링크 학습, 주소 창, MSI, pci_host_probe()
 *
 * 역방향: 열거 도중 PCI 코어가 config 접근 콜백을 되부르지만, 그 콜백은
 * 이 파일이 아니라 공용 코어(iproc_pcie_ops)가 제공한다. 이 파일이 등록하는
 * 콜백은 하나도 없다 — bcma 판이 map_irq 를 등록하는 것과 대비된다.
 *
 * 해제: iproc_pltfm_pcie_remove() -> iproc_pcie_remove()
 * 전원 차단: iproc_pltfm_pcie_shutdown() -> iproc_pcie_shutdown()
 *
 * 실행 컨텍스트: 전부 프로세스 컨텍스트다. probe 는 링크 학습(최대 100ms)과
 * config 재시도(최대 500ms)를 유발하므로 잠들 수 있고, remove 는 하위 장치의
 * 드라이버 해제를 유발하므로 역시 잠들 수 있다. 이 파일에는 인터럽트 핸들러도
 * 락도 없다.
 *
 * === 타 모듈과의 연결 ===
 * 옆쪽(공용 코어): pcie-iproc.h 의 struct iproc_pcie 와 enum iproc_pcie_type,
 *   그리고 EXPORT 된 iproc_pcie_setup()(pcie-iproc.c:3057 에서 EXPORT),
 *   iproc_pcie_remove()(:3071), iproc_pcie_shutdown()(:1391, GPL 한정) 셋이
 *   이 파일과 공용 코어 사이의 유일한 함수 경계다.
 * 위쪽: linux/pci.h 의 devm_pci_alloc_host_bridge(), pci_host_bridge_priv(),
 *   그리고 ../pci.h 의 devm_pci_remap_cfgspace().
 * 아래쪽: DT 파서(of_device_get_match_data, of_address_to_resource,
 *   of_property_read_bool, of_property_read_u32), PHY 프레임워크
 *   (devm_phy_optional_get), 플랫폼 버스(module_platform_driver).
 * 데이터 흐름: DT 노드 -> match data 로 type, reg 로 base/base_addr,
 *   brcm,pcie-ob-axi-offset 로 ob.axi_offset -> struct iproc_pcie ->
 *   iproc_pcie_setup() -> 하드웨어. 역방향으로는 platform_get_drvdata() 로
 *   같은 포인터를 되찾아 remove/shutdown 에 넘긴다.
 * 공유 상태: platform_set_drvdata() 로 플랫폼 장치에 매달아 두는
 *   struct iproc_pcie 포인터 하나뿐이다. 전역 변수도 정적 상태도 없어
 *   인스턴스마다 독립적이며, 동기화가 필요한 지점 자체가 없다.
 *
 * === 주요 함수/구조체 요약 ===
 * iproc_pcie_of_match_table[] : compatible 문자열과 변종을 잇는 표.
 *                               네 변종만 있고 PAXB_BCMA 가 빠져 있다 —
 *                               그것은 DT 가 아니라 BCMA 버스로 발견되기 때문이다.
 * iproc_pltfm_pcie_probe()    : DT 속성을 struct iproc_pcie 로 번역하고
 *                               iproc_pcie_setup() 에 위임한다. 이 파일의 본체다.
 * iproc_pltfm_pcie_remove()   : drvdata 를 되찾아 iproc_pcie_remove() 에 넘긴다.
 * iproc_pltfm_pcie_shutdown() : 같은 방식으로 iproc_pcie_shutdown() 에 넘긴다.
 * iproc_pltfm_pcie_driver     : 플랫폼 드라이버 서술자.
 * 이 파일에는 구조체 정의가 없다. 다루는 struct iproc_pcie 는 pcie-iproc.h 소유다.
 *
 * === map_irq 에 대한 관찰 (코드를 고치지 않고 기록) ===
 * 아래 probe 의 switch 문은 PAXC 계열에서 pcie->map_irq 를 NULL 로 만드는데,
 * 코드를 따라가 보면 그 대입이 아무것도 바꾸지 않는다.
 *   1) pci_alloc_host_bridge() 가 kzalloc 으로 할당하므로(probe.c) private 영역인
 *      struct iproc_pcie 는 처음부터 0 이고, 따라서 map_irq 는 이미 NULL 이다.
 *   2) 이 파일은 map_irq 에 NULL 아닌 값을 넣는 곳이 한 군데도 없다
 *      (전수 grep: 대입은 :101 의 NULL 하나뿐).
 * 즉 이 switch 는 NULL 을 NULL 로 덮는 no-op 이며, "PAXC 는 레거시 IRQ 를
 * 지원하지 않는다" 는 의도를 코드로 드러내는 문서 역할만 한다.
 *
 * 더 눈여겨볼 것은 그다음이다. devm_pci_alloc_host_bridge() 는 내부에서
 * devm_of_pci_bridge_init() 을 부르고, 그것이 DT 노드가 있으면
 * bridge->map_irq 에 of_irq_parse_and_map_pci 를 심어 둔다(of.c 의 해당 함수).
 * 그런데 iproc_pcie_setup() 이 마지막에 host->map_irq = pcie->map_irq 를
 * 무조건 실행하므로(pcie-iproc.c:3020), 그 값이 NULL 로 덮여 사라진다.
 * 그 결과 pci_assign_irq() 가 hbrg->map_irq 가 NULL 임을 보고
 * "runtime IRQ mapping not provided by arch" 를 남기고 곧바로 돌아가
 * (drivers/pci/irq.c:363~366) 이 드라이버로 붙은 장치에는 INTx 가 배정되지 않는다.
 * PAXC 뿐 아니라 PAXB 계열에서도 마찬가지다.
 *
 * 이것이 의도인지 결함인지는 이 트리의 코드와 주석만으로 확정할 수 없다.
 * 사실만 적어 두며, 코드는 고치지 않는다.
 *
 * === NVMe 관점 ===
 * drivers/nvme/ 에서 이 파일의 심볼을 쓰는 곳은 없다(전수 grep 확인).
 * 이 파일은 버스를 만드는 쪽의 결합 계층이고 NVMe 는 그 위에 열거되는
 * 장치라 계층이 다르다. 다만 위 map_irq 관찰은 NVMe 에도 그대로 적용된다 —
 * 이 컨트롤러에 붙은 NVMe SSD 는 INTx 를 받을 수 없으므로 MSI/MSI-X 가
 * 동작해야만 인터럽트를 쓸 수 있다.
 */

/* [한국어] kernel.h — 기본 매크로. 이 파일에서 직접 쓰는 것은 없지만 다른 헤더의 전제다. */
#include <linux/kernel.h>
/* [한국어] pci.h — devm_pci_alloc_host_bridge(), pci_host_bridge_priv(),
 * struct pci_host_bridge 와 struct pci_dev. 이 파일의 뼈대가 되는 헤더다. */
#include <linux/pci.h>
/* [한국어] clk.h — 클럭 API. 이 파일은 클럭을 직접 다루지 않는다(전수 확인).
 * 공용 코어나 다른 헤더의 전제로 남아 있는 것으로 보인다. */
#include <linux/clk.h>
/* [한국어] module.h — MODULE_DEVICE_TABLE / MODULE_AUTHOR / MODULE_LICENSE 와
 * module_platform_driver(). 이 파일이 로드 가능한 모듈이 되게 한다. */
#include <linux/module.h>
/* [한국어] slab.h — 슬랩 할당자. 이 파일은 직접 할당하지 않고 devm_ 계열만 쓴다. */
#include <linux/slab.h>
/* [한국어] interrupt.h — 인터럽트 정의. 이 파일에는 핸들러가 없다. */
#include <linux/interrupt.h>
/* [한국어] platform_device.h — struct platform_device, platform_driver,
 * platform_set_drvdata / platform_get_drvdata. 이 파일이 플랫폼 드라이버인
 * 근거가 되는 헤더이며, bcma 판이 linux/bcma/bcma.h 를 쓰는 자리에 해당한다. */
#include <linux/platform_device.h>
/* [한국어] of_address.h — of_address_to_resource(). DT 의 reg 속성에서
 * 레지스터 블록의 물리 주소를 얻는다. */
#include <linux/of_address.h>
/* [한국어] of_pci.h — DT PCI 헬퍼 선언. */
#include <linux/of_pci.h>
/* [한국어] of_platform.h — of_device_get_match_data() 와 of_match_ptr().
 * compatible 매칭 결과에서 변종 값을 꺼내는 데 쓴다. */
#include <linux/of_platform.h>
/* [한국어] phy/phy.h — devm_phy_optional_get() 과 struct phy.
 * bcma 판은 PHY 를 쓰지 않으면서도 이 헤더를 포함하는데(구조체 필드 타입 때문),
 * 이 파일은 실제로 PHY 를 받아 쓴다. */
#include <linux/phy/phy.h>

/* [한국어] ../pci.h — PCI 코어 내부 헤더. devm_pci_remap_cfgspace() 가 여기 있다.
 * 외부 공개 API 가 아니라 드라이버 트리 안에서만 쓰는 함수라 상대 경로다. */
#include "../pci.h"
/* [한국어] pcie-iproc.h — 공용 코어 헤더. struct iproc_pcie 정의,
 * enum iproc_pcie_type(아래 매칭 표가 쓰는 값들), 그리고 이 파일이 부르는
 * iproc_pcie_setup()/remove()/shutdown() 선언이 여기 있다. */
#include "pcie-iproc.h"

/* [한국어] compatible 문자열과 하드웨어 변종을 잇는 표. 플랫폼 버스가 DT 노드의
 * compatible 을 이 표와 대조해 probe 를 부르고, probe 는 매칭된 항목의
 * data 를 꺼내 pcie->type 으로 삼는다.
 * 
 * 네 변종만 있고 IPROC_PCIE_PAXB_BCMA 가 빠져 있는 것이 요점이다 —
 * 그 변종은 DT 가 아니라 BCMA 버스로 발견되므로 pcie-iproc-bcma.c 가 맡는다. */
static const struct of_device_id iproc_pcie_of_match_table[] = {
	{
		/* [한국어] PAXB 1세대. NS, NSP, Cygnus, NS2, Pegasus SoC 가 이 이름을 쓴다
		 * (pcie-iproc.h 의 enum 주석 근거). */
		.compatible = "brcm,iproc-pcie",
		/* [한국어] data 는 void * 타입이라 enum 값을 포인터로 캐스팅해 담는다.
		 * (int *) 로 캐스팅하지만 실제로 역참조하지 않고, probe 가 uintptr_t 로
		 * 되돌려 정수로 쓴다. 커널에서 흔한 관용구다. */
		.data = (int *)IPROC_PCIE_PAXB,
	}, {
		/* [한국어] PAXB 2세대. Stingray SoC 용이며, 이 파일이 지원하는 변종 중 기능이 가장 많다. */
		.compatible = "brcm,iproc-pcie-paxb-v2",
		/* [한국어] IPROC_PCIE_PAXB_V2 가 rev_init 에서 바깥 창 4개, 안쪽 영역 5개,
		 * RRS 재시도, MSI 배선을 모두 켜는 변종이다. */
		.data = (int *)IPROC_PCIE_PAXB_V2,
	}, {
		/* [한국어] PAXC 1세대. SoC 내부에 에뮬레이트된 엔드포인트 전용 래퍼다. */
		.compatible = "brcm,iproc-pcie-paxc",
		/* [한국어] 이 값이 rev_init 에서 ep_is_internal 을 세워 PERST 와 링크 학습을
		 * 통째로 건너뛰게 만든다. */
		.data = (int *)IPROC_PCIE_PAXC,
	}, {
		/* [한국어] PAXC 2세대. v1 에 전용 MSI 레지스터가 더해진 형태다. */
		.compatible = "brcm,iproc-pcie-paxc-v2",
		/* [한국어] rev_init 에서 need_msi_steer 까지 세우는 변종이다. */
		.data = (int *)IPROC_PCIE_PAXC_V2,
	},
	/* [한국어] 표의 끝 표시. of_device_id 배열은 compatible 이 빈 항목으로 끝나야 한다. */
	{ /* sentinel */ }
};
/* [한국어] 이 표를 모듈 메타데이터에 넣어, 모듈이 로드되기 전에도 DT 노드를 보고
 * 어떤 모듈을 불러야 하는지 udev/커널이 알 수 있게 한다. */
MODULE_DEVICE_TABLE(of, iproc_pcie_of_match_table);

/* [한국어]
 * iproc_pltfm_pcie_probe - DT 속성을 공용 구조체로 번역하고 코어에 위임한다
 *
 * @pdev: 플랫폼 버스가 compatible 매칭으로 넘긴 장치.
 * @return: 0 성공. -ENOMEM 은 브리지 할당 또는 레지스터 매핑 실패,
 *          그 밖에는 DT 파싱이나 iproc_pcie_setup() 이 돌려준 errno.
 *
 * 이 파일의 본체이자 사실상 전부다. 하드웨어를 직접 만지는 코드는 한 줄도
 * 없고, DT 가 말하는 것을 struct iproc_pcie 의 필드로 옮겨 담은 뒤
 * iproc_pcie_setup() 에 넘기는 것이 일이다.
 *
 * 순서와 각 단계의 뜻:
 *   1) devm_pci_alloc_host_bridge(dev, sizeof(*pcie)) 로 브리지와 이 드라이버의
 *      private 영역을 한 덩어리로 할당한다. kzalloc 기반이라 pcie 의 모든
 *      필드가 0 으로 시작한다 — 아래에서 명시적으로 채우지 않는 플래그들이
 *      자동으로 false 가 되는 것이 이 성질에 기대고 있다.
 *   2) type 을 match data 에서 꺼낸다. compatible 문자열 하나가 곧 변종이며,
 *      이 값이 나중에 iproc_pcie_rev_init() 의 switch 를 가른다.
 *   3) reg 속성의 0번 자원을 config 공간용으로 매핑한다. 평범한 ioremap 이
 *      아니라 devm_pci_remap_cfgspace() 를 쓰는 것이 요점이다 - config 접근은
 *      쓰기 병합(write combining)이 일어나면 안 되기 때문이다.
 *      물리 주소도 base_addr 에 따로 보관하는데, MSI 수신 주소 계산에 쓰인다.
 *   4) brcm,pcie-ob 가 있으면 바깥 창을 쓰겠다는 뜻이므로 axi_offset 을 함께
 *      읽어 need_ob_cfg 를 세운다. 오프셋 속성이 없으면 실패로 다룬다 -
 *      창을 쓰겠다면서 변환 기준을 주지 않은 것은 DT 오류이기 때문이다.
 *   5) dma-ranges 의 존재 여부가 그대로 need_ib_cfg 가 된다. 상류 주석이
 *      그 판단 근거를 적어 두었다.
 *   6) PHY 는 optional 이다. 없으면 NULL 이 오고 그것이 정상이다.
 *   7) PAXC 계열에서 map_irq 를 NULL 로 만든다. 다만 파일 상단 관찰에 적었듯
 *      이 대입은 no-op 이고, 실제로는 모든 변종에서 INTx 가 배정되지 않는다.
 *   8) iproc_pcie_setup() 에 넘긴다. 여기서부터 버스 종류와 무관한 공통 경로다.
 *   9) 성공하면 drvdata 에 pcie 를 걸어 둔다. remove/shutdown 이 이것으로
 *      되찾는다. 실패 경로에서는 걸지 않으므로 정리할 것도 없다.
 *
 * 오류 경로에 정리 코드가 하나도 없는 것이 특징인데, 할당이 전부 devm_ 계열
 * 이라 장치 해제 시 커널이 되돌려 주기 때문이다. bcma 판도 같은 성질을 갖는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(드라이버 바인딩). 안쪽에서 잠든다.
 *
 * 호출 체인:  플랫폼 버스 → [이 함수] → devm_pci_alloc_host_bridge()
 *               → of_device_get_match_data() → devm_pci_remap_cfgspace()
 *               → devm_phy_optional_get() → iproc_pcie_setup() [pcie-iproc.c:2782]
 */
static int iproc_pltfm_pcie_probe(struct platform_device *pdev)
{
	/* [한국어] 로그와 devm 할당의 기준이 되는 device. pdev 안의 것을 꺼내 쓴다. */
	struct device *dev = &pdev->dev;
	/* [한국어] 이 드라이버의 상태 구조체. 아래에서 브리지의 private 영역을 가리키게 된다. */
	struct iproc_pcie *pcie;
	/* [한국어] DT 노드. 아래 속성 읽기들이 모두 이것을 기준으로 한다. */
	struct device_node *np = dev->of_node;
	/* [한국어] reg 속성에서 읽어 올 레지스터 블록 자원. */
	struct resource reg;
	/* [한국어] PCI 호스트 브리지. private 영역에 위 pcie 가 얹힌다. */
	struct pci_host_bridge *bridge;
	/* [한국어] 하위 호출 결과. */
	int ret;

	/* [한국어] 브리지와 private 영역(struct iproc_pcie)을 한 덩어리로 할당한다.
	 * kzalloc 기반이라 pcie 의 모든 필드가 0 으로 시작하며, 아래에서 명시적으로
	 * 채우지 않는 플래그들이 자동으로 false 가 되는 것이 이 성질에 기댄다.
	 * 이 함수는 내부에서 devm_of_pci_bridge_init() 도 불러 DT 의 ranges 를
	 * 브리지 창으로 만들고 map_irq 를 심어 둔다. */
	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*pcie));
	/* [한국어] 할당 실패면 */
	if (!bridge)
		/* [한국어] 메모리 부족. */
		return -ENOMEM;

	/* [한국어] 방금 할당한 덩어리에서 private 부분의 주소를 얻는다.
	 * 브리지 구조체 바로 뒤에 붙어 있어 포인터 산술 한 번으로 구해진다. */
	pcie = pci_host_bridge_priv(bridge);

	/* [한국어] 이후 모든 로그와 devm 할당, DT 조회의 기준이 된다. */
	pcie->dev = dev;
	/* [한국어] 매칭된 표 항목의 data 를 꺼내 변종으로 삼는다. 포인터를 uintptr_t 로
	 * 되돌려 정수로 읽는 것이 위 (int *) 캐스팅과 짝을 이룬다.
	 * 이 한 줄이 이후 rev_init 의 switch 를 가르는 유일한 근거다. */
	pcie->type = (uintptr_t)of_device_get_match_data(dev);

	/* [한국어] DT 의 reg 속성 0번 항목을 자원으로 읽는다. */
	ret = of_address_to_resource(np, 0, &reg);
	/* [한국어] 읽지 못했으면 */
	if (ret < 0) {
		/* [한국어] 그 사실을 남기고 */
		dev_err(dev, "unable to obtain controller resources\n");
		/* [한국어] errno 를 전한다. 아직 아무것도 잡지 않았으므로 되돌릴 것이 없다. */
		return ret;
	}

	/* [한국어] 레지스터 블록을 매핑한다. 평범한 ioremap 이 아니라 config 공간 전용
	 * 함수를 쓰는 것이 요점인데, config 접근에는 쓰기 병합이 일어나면 안 되기
	 * 때문이다. 이 함수는 아키텍처에 따라 적절한 매핑 속성을 골라 준다. */
	pcie->base = devm_pci_remap_cfgspace(dev, reg.start,
					     /* [한국어] 크기는 자원의 길이 그대로. */
					     resource_size(&reg));
	/* [한국어] 매핑 실패면 */
	if (!pcie->base) {
		/* [한국어] 그 사실을 남기고 */
		dev_err(dev, "unable to map controller registers\n");
		/* [한국어] 메모리 부족으로 돌린다. 이 함수는 ERR_PTR 이 아니라 NULL 로 실패를 알린다. */
		return -ENOMEM;
	}
	/* [한국어] 물리 주소도 따로 보관한다. 가상 주소로는 알 수 없는 값이고,
	 * 공용 코어가 MSI 수신 주소를 정할 때 이 물리 주소를 쓴다. */
	pcie->base_addr = reg.start;

	/* [한국어] 바깥 창을 쓰겠다는 DT 표시가 있는가. bcma 판이 128MB 창을 코드로
	 * 조립하는 자리에 해당하며, 이쪽은 DT 가 결정한다. */
	if (of_property_read_bool(np, "brcm,pcie-ob")) {
		/* [한국어] 오프셋 값을 잠시 받을 변수. u32 로 읽어 resource_size_t 필드에 넣는다. */
		u32 val;

		/* [한국어] AXI 주소와 컨트롤러 내부 주소의 차이를 읽는다. */
		ret = of_property_read_u32(np, "brcm,pcie-ob-axi-offset",
					   /* [한국어] 이 값이 iproc_pcie_setup_ob() 에서 axi_addr 에서 빼지는 기준이 된다. */
					   &val);
		/* [한국어] 창을 쓰겠다면서 변환 기준을 주지 않았으면 */
		if (ret) {
			/* [한국어] 어느 속성이 빠졌는지 정확히 남기고 */
			dev_err(dev,
				/* [한국어] DT 오류이므로 */
				"missing brcm,pcie-ob-axi-offset property\n");
			/* [한국어] errno 를 전해 probe 를 실패시킨다. */
			return ret;
		}
		/* [한국어] 읽은 오프셋을 저장한다. */
		pcie->ob.axi_offset = val;
		/* [한국어] 바깥 창 설정이 필요하다고 표시한다. rev_init 이 이 플래그를 보고
		 * 변종별 창 능력 표를 붙일지 정한다. */
		pcie->need_ob_cfg = true;
	}

	/*
	 * DT nodes are not used by all platforms that use the iProc PCIe
	 * core driver. For platforms that require explicit inbound mapping
	 * configuration, "dma-ranges" would have been present in DT
	 */
	/* [한국어] dma-ranges 의 존재 여부가 그대로 안쪽 창 필요 여부가 된다.
	 * 상류 주석이 그 판단 근거를 적어 두었다 — DT 를 쓰지 않는 플랫폼도 있고,
	 * 안쪽 매핑이 필요한 플랫폼이라면 이 속성이 있었을 것이라는 뜻이다. */
	pcie->need_ib_cfg = of_property_read_bool(np, "dma-ranges");

	/* PHY use is optional */
	/* [한국어] PHY 는 선택적이다(상류 주석). 1·2세대처럼 PHY 장치가 따로 없는 구성에서는
	 * NULL 이 오고 그것이 정상이며, 공용 코어의 phy_init/phy_power_on 이
	 * NULL 을 무해하게 처리한다. */
	pcie->phy = devm_phy_optional_get(dev, "pcie-phy");
	/* [한국어] 실제 오류(없는 것과는 다르다)면 */
	if (IS_ERR(pcie->phy))
		/* [한국어] 그 오류를 전한다. */
		return PTR_ERR(pcie->phy);

	/* PAXC doesn't support legacy IRQs, skip mapping */
	/* [한국어] PAXC 계열은 레거시 INTx 를 지원하지 않는다(상류 주석). */
	switch (pcie->type) {
	/* [한국어] PAXC 1세대와 */
	case IPROC_PCIE_PAXC:
	/* [한국어] 2세대가 대상이다. */
	case IPROC_PCIE_PAXC_V2:
		/* [한국어] 다만 이 대입은 no-op 이다 — 위 devm_pci_alloc_host_bridge 가 kzalloc 으로
		 * 할당해 map_irq 가 이미 NULL 이고, 이 파일 어디에서도 NULL 아닌 값을 넣지
		 * 않는다(전수 grep). 의도를 코드로 드러내는 문서 역할만 한다.
		 * 자세한 내용은 파일 상단의 map_irq 관찰 절 참조. */
		pcie->map_irq = NULL;
		/* [한국어] PAXC 갈래 끝. */
		break;
	/* [한국어] 그 밖의 변종은 */
	default:
		/* [한국어] 아무것도 하지 않는다. 결과적으로 모든 변종에서 map_irq 가 NULL 이다. */
		break;
	/* [한국어] switch 끝. */
	}

	/* [한국어] 준비한 구조체와 브리지의 창 목록을 공용 코어에 넘긴다.
	 * 이 시점부터 버스 종류와 무관한 공통 경로이며, 변종 판정·PERST·링크 학습·
	 * 주소 창·MSI·버스 스캔이 모두 그 안에서 일어난다. */
	ret = iproc_pcie_setup(pcie, &bridge->windows);
	/* [한국어] 실패했으면 */
	if (ret) {
		/* [한국어] 그 사실을 남기고 */
		dev_err(dev, "PCIe controller setup failed\n");
		/* [한국어] errno 를 전한다. 되돌릴 것이 없는 이유는 할당이 전부 devm_ 계열이라
		 * 장치 해제 시 커널이 반납하고, 하드웨어 설정은 코어가 자기 에러 경로에서
		 * PHY 를 되돌리기 때문이다. */
		return ret;
	}

	/* [한국어] 성공했으니 remove/shutdown 이 되찾을 수 있게 걸어 둔다.
	 * 실패 경로에서는 걸지 않으므로 정리할 것도 없다. */
	platform_set_drvdata(pdev, pcie);
	/* [한국어] 성공. */
	return 0;
}

/* [한국어]
 * iproc_pltfm_pcie_remove - drvdata 를 되찾아 공용 코어에 제거를 맡긴다
 *
 * @pdev: 제거되는 플랫폼 장치.
 * @return: 없음.
 *
 * probe 가 platform_set_drvdata() 로 걸어 둔 struct iproc_pcie 를 꺼내
 * iproc_pcie_remove() 에 넘기는 두 줄짜리다.
 *
 * 그 함수가 pci_stop_root_bus() 와 pci_remove_root_bus() 로 버스를 해체하고,
 * MSI 컨트롤러를 정리하고, PHY 전원을 내린다(pcie-iproc.c:3060~3070).
 * 이 파일이 devm 으로 잡은 것들은 이 함수가 돌아간 뒤 커널이 자동으로 반납한다.
 *
 * drvdata 가 NULL 인지 확인하지 않는데, probe 가 성공했을 때만 remove 가
 * 불린다는 드라이버 모델의 규약에 기대는 것이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 하위 장치의 드라이버 해제를 유발해 잠들 수 있다.
 *
 * 호출 체인:  플랫폼 버스(언바인딩) → [이 함수] → iproc_pcie_remove() [pcie-iproc.c:3060]
 */
static void iproc_pltfm_pcie_remove(struct platform_device *pdev)
{
	/* [한국어] probe 가 걸어 둔 상태 구조체를 되찾는다. NULL 검사가 없는 것은
	 * probe 가 성공했을 때만 remove 가 불린다는 드라이버 모델의 규약 때문이다. */
	struct iproc_pcie *pcie = platform_get_drvdata(pdev);

	/* [한국어] 버스 해체, MSI 정리, PHY 내리기를 공용 코어가 한다.
	 * 이 파일이 devm 으로 잡은 것들은 이 함수가 돌아간 뒤 커널이 반납한다. */
	iproc_pcie_remove(pcie);
}

/* [한국어]
 * iproc_pltfm_pcie_shutdown - 시스템 종료 시 공용 코어의 종료 절차를 부른다
 *
 * @pdev: 종료 중인 플랫폼 장치.
 * @return: 없음.
 *
 * remove 와 같은 모양이지만 부르는 대상이 다르다. iproc_pcie_shutdown() 은
 * 버스를 해체하지 않고 PERST# 를 assert 한 뒤 500ms 기다리기만 한다
 * (pcie-iproc.c:1384~1390).
 *
 * 종료와 제거를 나누는 이유는 목적이 다르기 때문이다. 제거는 자료구조까지
 * 정리해야 하지만, 종료는 다음 부팅이나 kexec 로 넘어갈 때 엔드포인트가
 * 어중간한 상태로 남지 않게만 하면 된다. 곧 전원이 끊기거나 커널이 바뀌므로
 * 소프트웨어 자료구조를 정리할 이유가 없다.
 *
 * iproc_pcie_shutdown() 은 int 를 돌려주지만 여기서는 무시한다.
 * 그 함수가 항상 0 을 돌려주므로 확인할 것이 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(시스템 종료). msleep 으로 잠든다.
 *
 * 호출 체인:  커널 종료 경로 → [이 함수] → iproc_pcie_shutdown() [pcie-iproc.c:1384]
 */
static void iproc_pltfm_pcie_shutdown(struct platform_device *pdev)
{
	/* [한국어] 같은 방식으로 상태 구조체를 되찾는다. */
	struct iproc_pcie *pcie = platform_get_drvdata(pdev);

	/* [한국어] PERST 를 assert 하고 500ms 기다린다. 버스를 해체하지 않는 것이
	 * remove 와 다른 점이며, 곧 전원이 끊기거나 커널이 바뀌므로 소프트웨어
	 * 자료구조를 정리할 이유가 없기 때문이다. 반환값(항상 0)은 무시한다. */
	iproc_pcie_shutdown(pcie);
}

/* [한국어] 플랫폼 드라이버 서술자. bcma 판의 struct bcma_driver 에 대응한다. */
static struct platform_driver iproc_pltfm_pcie_driver = {
	/* [한국어] 드라이버 공통 부분. */
	.driver = {
		/* [한국어] 드라이버 이름. sysfs 와 로그에 나타난다. */
		.name = "iproc-pcie",
		/* [한국어] 위 매칭 표를 건다. of_match_ptr 로 감싸는 것은 CONFIG_OF 가 꺼진 빌드에서
		 * 표를 NULL 로 만들어 링크 오류를 막는 관용구다. */
		.of_match_table = of_match_ptr(iproc_pcie_of_match_table),
	},
	/* [한국어] 바인딩 시 호출. */
	.probe = iproc_pltfm_pcie_probe,
	/* [한국어] 언바인딩 시 호출. */
	.remove = iproc_pltfm_pcie_remove,
	/* [한국어] 시스템 종료 시 호출. bcma 판에는 없는 콜백인데, 그쪽은 SoC 내장 버스라
	 * 종료 시 PERST 를 다룰 이유가 없기 때문으로 보인다. */
	.shutdown = iproc_pltfm_pcie_shutdown,
};
/* [한국어] 모듈 init/exit 를 자동 생성해 위 드라이버를 플랫폼 버스에 등록한다. */
module_platform_driver(iproc_pltfm_pcie_driver);

/* [한국어] 모듈 메타데이터. 작성자, */
MODULE_AUTHOR("Ray Jui <rjui@broadcom.com>");
/* [한국어] 설명, */
MODULE_DESCRIPTION("Broadcom iPROC PCIe platform driver");
/* [한국어] 라이선스. GPL v2 선언이 있어야 GPL 한정 심볼(iproc_pcie_shutdown 등)을
 * 쓸 수 있다. */
MODULE_LICENSE("GPL v2");
