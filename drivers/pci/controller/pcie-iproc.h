/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2014-2015 Broadcom Corporation
 */

/*
 * [한국어 설명] iProc PCIe 드라이버 네 파일이 공유하는 계약 (pcie-iproc.h)
 *
 * === 파일의 역할 ===
 * Broadcom iProc PCIe 드라이버는 한 파일이 아니라 넷으로 나뉘어 있고,
 * 이 헤더가 그 넷을 잇는 유일한 계약이다. 코드는 인라인 더미 둘 말고는 없고
 * 전부 타입 정의와 함수 선언이다.
 *
 * 담고 있는 것이 셋이다.
 *   1) enum iproc_pcie_type - 하드웨어 변종 다섯 가지. 결합 드라이버가
 *      이 값을 정해 넘기면 공용 코어의 iproc_pcie_rev_init() 이 그것으로
 *      레지스터 표와 능력 플래그를 고른다. 이 enum 하나가 다섯 하드웨어의
 *      차이를 흡수하는 출발점이다.
 *   2) struct iproc_pcie - 컨트롤러 하나의 모든 상태. 결합 드라이버가 일부를
 *      채워 넘기고 공용 코어가 나머지를 채우는 "반씩 나눠 쓰는" 구조체다.
 *   3) 파일 간 함수 경계 - setup/remove/shutdown 셋과 MSI 쪽 init/exit 둘.
 *
 * 이 헤더를 읽는 요령은 struct iproc_pcie 의 필드를 "누가 채우는가" 로
 * 나눠 보는 것이다. 결합 드라이버가 채우는 것(dev, type, base, base_addr,
 * mem, phy, map_irq, need_ob_cfg, need_ib_cfg, ob.axi_offset)과 공용 코어가
 * 채우는 것(reg_offsets, 각종 능력 플래그, ob_map/ib_map, 창 개수)이
 * 뚜렷이 갈린다. 그 경계가 곧 이 드라이버의 계층 경계다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 헤더 자체는 실행되지 않는다. 네 .c 파일이 모두 이것을 include 하며,
 * 그 넷의 관계는 이렇다.
 *
 *   pcie-iproc.c            공용 코어. 하드웨어를 실제로 다루는 본체이며,
 *                           아래 setup/remove/shutdown 셋을 정의한다.
 *   pcie-iproc-platform.c   DT 플랫폼 결합. 변종 넷을 지원한다.
 *   pcie-iproc-bcma.c       BCMA 버스 결합. IPROC_PCIE_PAXB_BCMA 하나만.
 *   pcie-iproc-msi.c        선택적 MSI 컨트롤러. init/exit 둘을 정의한다.
 *
 * Kconfig 조합으로 보면 앞의 세 파일은 결합 드라이버 선택에 따라 빌드되고,
 * 마지막 하나만 CONFIG_PCIE_IPROC_MSI 로 따로 켜고 끌 수 있다. 그래서 이
 * 헤더의 아래쪽에 #ifdef 로 감싼 인라인 더미가 있다 - MSI 파일이 빠져도
 * 공용 코어가 컴파일되게 하려는 장치다.
 *
 * 실행 컨텍스트: 헤더라 해당 없음. 다만 여기 선언된 함수는 전부 프로세스
 * 컨텍스트 전용이다. setup 은 링크 학습에서, shutdown 은 500ms 대기에서
 * 잠들고, MSI 쪽 init/exit 도 뮤텍스와 DMA 할당을 다룬다.
 *
 * === 타 모듈과의 연결 ===
 * 이 헤더가 의존하는 것: struct device, struct pci_dev, struct device_node,
 * struct resource, struct phy, resource_size_t, phys_addr_t. 다만 그 정의를
 * 직접 include 하지 않고 이 헤더를 포함하는 .c 파일이 먼저 linux/pci.h 와
 * linux/phy/phy.h 등을 include 해 두는 것을 전제한다. 가드 안에 #include 가
 * 하나도 없는 것이 그 사실을 보여 준다 - 그래서 pcie-iproc-bcma.c 는
 * PHY 를 쓰지 않으면서도 linux/phy/phy.h 를 포함한다.
 * 이 헤더에 의존하는 것: 위 네 .c 파일뿐이다. drivers/ 어디에서도
 * 이 경로를 include 하지 않는다(전수 grep 확인).
 * 앞선 선언(struct iproc_pcie_ob_map / _ib_map / iproc_msi)이 셋 있는데,
 * 그 실체가 각각 pcie-iproc.c 와 pcie-iproc-msi.c 안에 숨어 있기 때문이다.
 * 포인터로만 들고 있으면 되므로 정의를 공개할 필요가 없다는 뜻이다.
 *
 * === 주요 함수/구조체 요약 ===
 * enum iproc_pcie_type   : 변종 다섯. rev_init 의 switch 와 일대일 대응한다.
 * struct iproc_pcie_ob   : 바깥 창 관련 값 둘(AXI 오프셋, 창 개수).
 * struct iproc_pcie_ib   : 안쪽 영역 관련 값 하나(영역 개수).
 * struct iproc_pcie      : 컨트롤러 하나의 모든 상태. 이 헤더의 중심이다.
 * iproc_pcie_setup()     : 결합 드라이버가 채운 구조체를 받아 컨트롤러를
 *                          세우고 PCI 버스 스캔까지 진행한다.
 * iproc_pcie_remove()    : 버스를 해체하고 MSI 와 PHY 를 정리한다.
 * iproc_pcie_shutdown()  : PERST 를 걸어 둔다. 버스는 해체하지 않는다.
 * iproc_msi_init()/exit(): MSI 컨트롤러를 띄우고 내린다. CONFIG 가 꺼지면
 *                          아래 인라인 더미로 대체된다.
 *
 * === NVMe 관점 ===
 * drivers/nvme/ 에서 이 헤더의 심볼을 쓰는 곳은 없다(전수 grep 확인).
 * 이 헤더는 호스트 컨트롤러 드라이버 내부의 계약이고, NVMe 는 그 컨트롤러가
 * 만든 버스 위에 열거되는 장치라 계층이 다르다.
 */

/* [한국어] 헤더 가드. 네 .c 파일이 모두 이것을 include 한다. #include 가 하나도 없어
 * struct device / pci_dev / phy 등의 정의는 포함하는 쪽이 먼저 갖춰야 한다. */
#ifndef _PCIE_IPROC_H
#define _PCIE_IPROC_H

/**
 * enum iproc_pcie_type - iProc PCIe interface type
 * @IPROC_PCIE_PAXB_BCMA: BCMA-based host controllers
 * @IPROC_PCIE_PAXB:	  PAXB-based host controllers for
 *			  NS, NSP, Cygnus, NS2, and Pegasus SOCs
 * @IPROC_PCIE_PAXB_V2:   PAXB-based host controllers for Stingray SoCs
 * @IPROC_PCIE_PAXC:	  PAXC-based host controllers
 * @IPROC_PCIE_PAXC_V2:   PAXC-based host controllers (second generation)
 *
 * PAXB is the wrapper used in root complex that can be connected to an
 * external endpoint device.
 *
 * PAXC is the wrapper used in root complex dedicated for internal emulated
 * endpoint devices.
 */
/* [한국어] 하드웨어 변종 다섯. 이 값 하나가 pcie-iproc.c 의 iproc_pcie_rev_init()
 * switch 를 가르고, 거기서 레지스터 표와 능력 플래그가 통째로 정해진다. */
enum iproc_pcie_type {
	/* [한국어] BCMA 버스로 발견되는 PAXB(0). 결합 드라이버가 pcie-iproc-bcma.c 이며,
	 * DT 매칭 표에는 없다.
	 * 설정자: pcie-iproc-bcma.c 가 상수로 고정해 넣는다.
	 * 읽는 자: rev_init 의 switch.
	 * 값 범위: 0.
	 * rev_init 대응: 오프셋 표(iproc_pcie_reg_paxb_bcma)만 고르고 끝난다 —
	 *   창도 MSI 배선도 없는 가장 단순한 변종이다. */
	IPROC_PCIE_PAXB_BCMA = 0,
	/* [한국어] DT 로 발견되는 PAXB 1세대. NS/NSP/Cygnus/NS2/Pegasus SoC 가 쓴다
	 * (위 상류 주석 근거).
	 * 설정자: pcie-iproc-platform.c 의 매칭 표에서 "brcm,iproc-pcie".
	 * 읽는 자: rev_init 의 switch.
	 * 값 범위: 1.
	 * rev_init 대응: has_apb_err_disable 을 세우고, need_ob_cfg 가 서 있으면
	 *   바깥 창 표(paxb_ob_map, 창 2개)를 붙인다. */
	IPROC_PCIE_PAXB,
	/* [한국어] PAXB 2세대. Stingray SoC 용이며 이 드라이버가 지원하는 변종 중 기능이 가장 많다.
	 * 설정자: 매칭 표의 "brcm,iproc-pcie-paxb-v2".
	 * 읽는 자: rev_init 의 switch.
	 * 값 범위: 2.
	 * rev_init 대응: iproc_cfg_read(RRS 재시도 경로), has_apb_err_disable,
	 *   바깥 창 표(창 4개), 안쪽 영역 표(영역 5개), need_msi_steer 를 모두 켜고,
	 *   CFG_RETRY_STATUS 값을 읽으면 잘못된 데이터가 온다는 경고를 부팅 로그에 남긴다. */
	IPROC_PCIE_PAXB_V2,
	/* [한국어] PAXC 1세대. SoC 내부에 에뮬레이트된 엔드포인트 전용 래퍼다(위 상류 주석).
	 * 설정자: 매칭 표의 "brcm,iproc-pcie-paxc".
	 * 읽는 자: rev_init 의 switch.
	 * 값 범위: 3.
	 * rev_init 대응: ep_is_internal(PERST 와 링크 학습을 통째로 건너뛴다),
	 *   iproc_cfg_read, rej_unconfig_pf 를 세운다. 창은 하나도 없다 —
	 *   내부 직결이라 주소 변환이 필요 없기 때문이다. */
	IPROC_PCIE_PAXC,
	/* [한국어] PAXC 2세대. v1 에 전용 MSI 레지스터가 더해진 형태다.
	 * 설정자: 매칭 표의 "brcm,iproc-pcie-paxc-v2".
	 * 읽는 자: rev_init 의 switch.
	 * 값 범위: 4.
	 * rev_init 대응: v1 의 셋에 need_msi_steer 가 추가된다. 또 이 변종만
	 *   IPROC_PCIE_CLK_CTRL 이 없어, rev_init 이 오프셋 표의 0번 항목을
	 *   따로 IPROC_PCIE_REG_INVALID 로 지정해 준다. */
	IPROC_PCIE_PAXC_V2,
/* [한국어] enum 끝. 다섯 값이 rev_init 의 다섯 case 와 일대일로 대응한다. */
};

/**
 * struct iproc_pcie_ob - iProc PCIe outbound mapping
 * @axi_offset: offset from the AXI address to the internal address used by
 * the iProc PCIe core
 * @nr_windows: total number of supported outbound mapping windows
 */
/* [한국어] 바깥 방향(CPU -> PCI) 창 관련 값 묶음. 두 필드의 출처가 서로 다르다는 점이
 * 이 구조체를 읽는 요령이다 — 하나는 DT 가, 하나는 코어가 정한다. */
struct iproc_pcie_ob {
	/* [한국어] AXI 주소와 컨트롤러 내부 주소의 차이(상류 주석).
	 * 설정자: 결합 드라이버. platform.c 가 DT 의 brcm,pcie-ob-axi-offset 에서 읽고,
	 *   bcma.c 는 이 값을 건드리지 않아 0 으로 남는다.
	 * 읽는 자: pcie-iproc.c 의 iproc_pcie_setup_ob() 이 axi_addr 에서 이 값을 뺀다.
	 *   요청 주소가 이 값보다 작으면 -EINVAL 로 거절한다.
	 * 값 범위: 0 이상의 물리 주소 오프셋.
	 * 동기화: probe 에서 한 번 설정된 뒤 읽기 전용이다. */
	resource_size_t axi_offset;
	/* [한국어] 이 변종이 지원하는 바깥 창 개수(상류 주석).
	 * 설정자: 공용 코어의 rev_init 이 변종별 표 크기에서 가져온다
	 *   (paxb 는 2, paxb_v2 는 4). 결합 드라이버는 건드리지 않는다.
	 * 읽는 자: iproc_pcie_setup_ob() 의 루프 상한,
	 *   iproc_pcie_invalidate_mapping() 의 순회 범위.
	 * 값 범위: 0(need_ob_cfg 가 거짓이면 그대로), 2, 4.
	 * 동기화: probe 에서만 설정된다. */
	unsigned int nr_windows;
};

/**
 * struct iproc_pcie_ib - iProc PCIe inbound mapping
 * @nr_regions: total number of supported inbound mapping regions
 */
/* [한국어] 안쪽 방향(PCI -> 메모리) 영역 관련 값. 필드가 하나뿐인데,
 * 바깥 창의 axi_offset 에 해당하는 변환 오프셋이 안쪽에는 없기 때문이다 —
 * DT 의 dma-ranges 항목이 자기 offset 을 직접 들고 온다. */
struct iproc_pcie_ib {
	/* [한국어] 이 변종이 지원하는 안쪽 영역 개수(상류 주석).
	 * 설정자: rev_init 이 paxb_v2 에서만 표 크기(5)로 채운다. 다른 변종은 0.
	 * 읽는 자: iproc_pcie_setup_ib() 의 루프 상한,
	 *   iproc_pcie_invalidate_mapping() 의 순회 범위.
	 * 값 범위: 0 또는 5.
	 * 동기화: probe 에서만 설정된다. */
	unsigned int nr_regions;
};

/* [한국어] 앞선 선언 셋. 실체가 pcie-iproc.c 안의 static 정의라 여기서는 이름만 알린다.
 * 포인터로만 들고 있으면 되므로 정의를 공개할 이유가 없다. */
struct iproc_pcie_ob_map;
/* [한국어] 안쪽 영역 능력 표의 타입. 역시 pcie-iproc.c 안에 있다. */
struct iproc_pcie_ib_map;
/* [한국어] MSI 컨트롤러 상태. 이쪽은 pcie-iproc-msi.c 안에 정의가 있어,
 * 공용 코어는 포인터만 들고 다니며 내용을 들여다보지 않는다. */
struct iproc_msi;

/**
 * struct iproc_pcie - iProc PCIe device
 * @dev: pointer to device data structure
 * @type: iProc PCIe interface type
 * @reg_offsets: register offsets
 * @base: PCIe host controller I/O register base
 * @base_addr: PCIe host controller register base physical address
 * @mem: host bridge memory window resource
 * @phy: optional PHY device that controls the Serdes
 * @map_irq: function callback to map interrupts
 * @ep_is_internal: indicates an internal emulated endpoint device is connected
 * @iproc_cfg_read: indicates the iProc config read function should be used
 * @rej_unconfig_pf: indicates the root complex needs to detect and reject
 * enumeration against unconfigured physical functions emulated in the ASIC
 * @has_apb_err_disable: indicates the controller can be configured to prevent
 * unsupported request from being forwarded as an APB bus error
 * @fix_paxc_cap: indicates the controller has corrupted capability list in its
 * config space registers and requires SW based fixup
 *
 * @need_ob_cfg: indicates SW needs to configure the outbound mapping window
 * @ob: outbound mapping related parameters
 * @ob_map: outbound mapping related parameters specific to the controller
 *
 * @need_ib_cfg: indicates SW needs to configure the inbound mapping window
 * @ib: inbound mapping related parameters
 * @ib_map: outbound mapping region related parameters
 *
 * @need_msi_steer: indicates additional configuration of the iProc PCIe
 * controller is required to steer MSI writes to external interrupt controller
 * @msi: MSI data
 */
/* [한국어] 컨트롤러 하나의 모든 상태. 이 헤더의 중심이며, 네 .c 파일이 이 구조체를
 * 통해서만 서로를 안다.
 * 
 * 필드를 "누가 채우는가" 로 나눠 보면 계층 경계가 드러난다.
 *   결합 드라이버가 채움: dev, type, base, base_addr, mem, phy, map_irq,
 *                         need_ob_cfg, need_ib_cfg, ob.axi_offset
 *   공용 코어가 채움:     reg_offsets, 능력 플래그 다섯, ob_map/ib_map,
 *                         ob.nr_windows, ib.nr_regions, need_msi_steer
 *   MSI 파일이 채움:      msi */
struct iproc_pcie {
	/* [한국어] 로그, devm 할당, DT 조회의 기준이 되는 device(상류 주석).
	 * 설정자: 결합 드라이버의 probe(platform.c 와 bcma.c 둘 다 가장 먼저 채운다).
	 * 읽는 자: 거의 모든 함수. 공용 코어에서만 11회, 결합·MSI 파일에서 20회 쓰인다.
	 * 값 범위: 유효한 device 포인터. NULL 이 되는 경로는 없다.
	 * 동기화: probe 에서 한 번 설정된 뒤 읽기 전용이다. */
	struct device *dev;
	/* [한국어] 하드웨어 변종(상류 주석).
	 * 설정자: platform.c 는 of_device_get_match_data() 로, bcma.c 는 상수로 넣는다.
	 * 읽는 자: rev_init 의 switch 가 주 사용처이고, platform.c 도 PAXC 판정에 한 번 쓴다.
	 * 값 범위: 위 enum 의 다섯 값 중 하나.
	 * 동기화: probe 에서 한 번 설정된 뒤 읽기 전용이다. */
	enum iproc_pcie_type type;
	/* [한국어] 레지스터 이름 -> 이 변종의 오프셋 표(상류 주석).
	 * 설정자: rev_init 이 devm_kcalloc 으로 사본을 만들어 채운다. 정적 표를 그대로
	 *   쓰지 않는 이유는 값이 0 인 항목을 IPROC_PCIE_REG_INVALID 로 바꿔 넣어야
	 *   "이 변종에 없는 레지스터" 를 구분할 수 있기 때문이다.
	 * 읽는 자: iproc_pcie_reg_offset() 을 통해 사실상 모든 레지스터 접근.
	 * 값 범위: IPROC_PCIE_MAX_NUM_REG 개짜리 배열. 각 항목은 오프셋 또는 0xffff.
	 * 동기화: rev_init 이후 읽기 전용이다. */
	u16 *reg_offsets;
	/* [한국어] ioremap 된 레지스터 블록의 기준 가상 주소(상류 주석).
	 * 설정자: platform.c 는 devm_pci_remap_cfgspace() 로 매핑해 넣고,
	 *   bcma.c 는 BCMA 가 이미 매핑해 둔 bdev->io_addr 을 그대로 쓴다.
	 * 읽는 자: 공용 코어에서 21회, MSI 파일에서도 쓴다. 이 헤더가 가리키는
	 *   모든 오프셋이 이 주소에 더해져 실제 접근이 된다.
	 * 값 범위: 유효한 __iomem 포인터.
	 * 동기화: probe 에서 한 번 설정된 뒤 읽기 전용이다. */
	void __iomem *base;
	/* [한국어] 같은 레지스터 블록의 물리 주소(상류 주석).
	 * 설정자: platform.c 가 DT 자원의 start 를, bcma.c 가 bdev->addr 을 넣는다.
	 * 읽는 자: 공용 코어는 쓰지 않고(사용 0회), pcie-iproc-msi.c 가
	 *   msi->msi_addr 의 기준으로 쓴다 — MSI 수신 주소가 곧 이 레지스터 창의
	 *   물리 주소이기 때문이다.
	 * 값 범위: 유효한 물리 주소.
	 * 동기화: probe 에서 한 번 설정된 뒤 읽기 전용이다. */
	phys_addr_t base_addr;
	/* [한국어] 호스트 브리지의 메모리 창 자원(상류 주석).
	 * 설정자: pcie-iproc-bcma.c 만 쓴다 — addr_s[0] 부터 128MB 를 코드로 조립해
	 *   브리지 창 목록에 넣는다(bcma.c:306~319).
	 * 읽는 자: 없다. 넣은 뒤에는 브리지 창 목록이 소유한다.
	 * 값 범위: bcma 경로에서만 유효. platform.c 는 이 필드를 건드리지 않는데,
	 *   DT 의 ranges 가 창을 제공하므로 손으로 조립할 이유가 없기 때문이다.
	 * 동기화: probe 에서만 설정된다. */
	struct resource mem;
	/* [한국어] Serdes 를 제어하는 선택적 PHY 장치(상류 주석).
	 * 설정자: platform.c 가 devm_phy_optional_get(dev, "pcie-phy") 로 받는다.
	 *   bcma.c 는 채우지 않아 NULL 로 남는다(BCMA 계열은 PHY 를 쓰지 않는다).
	 * 읽는 자: iproc_pcie_setup() 이 phy_init/phy_power_on 을,
	 *   iproc_pcie_remove() 가 phy_power_off/phy_exit 을 부른다.
	 * 값 범위: 유효한 포인터 또는 NULL. PHY API 가 NULL 을 무해하게 처리하므로
	 *   두 경로 모두 별도 분기가 없다.
	 * 동기화: probe 에서 한 번 설정된 뒤 읽기 전용이다. */
	struct phy *phy;
	/* [한국어] 레거시 INTx 를 IRQ 번호로 사상하는 콜백(상류 주석).
	 * 설정자: bcma.c 만 실제 함수를 넣는다(iproc_bcma_pcie_map_irq, bcma.c:331).
	 *   platform.c 는 PAXC 에서 NULL 을 대입하지만 이미 NULL 이라 no-op 이다.
	 * 읽는 자: iproc_pcie_setup() 이 host->map_irq 에 무조건 복사한다
	 *   (pcie-iproc.c:3020).
	 * 값 범위: 유효한 함수 포인터(bcma) 또는 NULL(platform 전부).
	 * 동기화: probe 에서 한 번 설정된 뒤 읽기 전용이다.
	 * 주의: 그 무조건 복사 때문에 DT 경로에서는 devm_of_pci_bridge_init() 이
	 *   심어 둔 of_irq_parse_and_map_pci 가 NULL 로 덮인다. 결과적으로
	 *   pci_assign_irq() 가 일찍 돌아가 INTx 가 배정되지 않는다
	 *   (drivers/pci/irq.c:363~366). 자세한 내용은 pcie-iproc-platform.c 의
	 *   상단 관찰 절에 적어 두었다. */
	int (*map_irq)(const struct pci_dev *, u8, u8);
	/* [한국어] 내부 에뮬레이트 엔드포인트가 연결되어 있는가(상류 주석).
	 * 설정자: rev_init 이 PAXC 와 PAXC_V2 에만 세운다.
	 * 읽는 자: iproc_pcie_perst_ctrl() 이 참이면 리셋을 통째로 건너뛰고,
	 *   iproc_pcie_check_link() 가 참이면 링크 확인 없이 0 을 돌려준다.
	 * 값 범위: true(PAXC 계열) / false(PAXB 계열).
	 * 동기화: rev_init 이후 읽기 전용이다. */
	bool ep_is_internal;
	/* [한국어] iProc 전용 config 읽기 함수를 써야 하는가(상류 주석).
	 * 설정자: rev_init 이 PAXB_V2, PAXC, PAXC_V2 에 세운다.
	 * 읽는 자: iproc_pcie_config_read32() 가 이 값으로 전용 경로와
	 *   커널 일반 함수를 가른다. 전용 경로에는 RRS 재시도와 capability 고치기가 있다.
	 * 값 범위: true / false.
	 * 동기화: rev_init 이후 읽기 전용이다. */
	bool iproc_cfg_read;
	/* [한국어] 설정되지 않은 물리 기능(PF)에 대한 열거를 거부해야 하는가(상류 주석).
	 * 설정자: rev_init 이 PAXC 와 PAXC_V2 에 세운다.
	 * 읽는 자: iproc_pcie_config_read() 가 Vendor ID 를 읽는 시점에 이 값을 보고,
	 *   device ID 가 0x168e 면 PCIBIOS_FUNC_NOT_SUPPORTED 로 답한다.
	 * 값 범위: true(PAXC 계열) / false.
	 * 동기화: rev_init 이후 읽기 전용이다.
	 * 필요한 이유는 상류 주석이 밝히듯 ASIC 결함 때문이다 — 그런 PF 에 쓰기를 하면
	 *   내장 프로세서가 멈춘다. */
	bool rej_unconfig_pf;
	/* [한국어] Unsupported Request 가 APB 버스 오류로 전달되는 것을 막을 수 있는가(상류 주석).
	 * 설정자: rev_init 이 PAXB 와 PAXB_V2 에 세운다.
	 * 읽는 자: iproc_pcie_apb_err_disable() 이 이 값과 버스 번호를 함께 보고,
	 *   config 접근 전후로 APB 오류 전달을 껐다 켠다.
	 * 값 범위: true(PAXB 계열) / false.
	 * 동기화: rev_init 이후 읽기 전용이다. */
	bool has_apb_err_disable;
	/* [한국어] config space 의 capability 목록이 망가져 SW 교정이 필요한가(상류 주석).
	 * 설정자: 다른 플래그와 달리 rev_init 이 아니라 iproc_pcie_fix_cap() 이
	 *   런타임에 세운다 — Vendor ID 를 읽는 순간 device ID 를
	 *   iproc_pcie_corrupt_cap_did[] 목록과 대조해 결정한다.
	 * 읽는 자: 같은 함수의 PM/EXP capability 갈래가 이 값을 보고 값을 고친다.
	 * 값 범위: true / false. 초기값은 kzalloc 덕분에 false 다.
	 * 동기화: config 접근 경로에서 설정·판독되며 pci_lock 이 직렬화한다. */
	bool fix_paxc_cap;

	/* [한국어] 바깥 창을 SW 가 설정해야 하는가(상류 주석).
	 * 설정자: 결합 드라이버. platform.c 는 DT 의 brcm,pcie-ob 유무로,
	 *   bcma.c 는 채우지 않아 false 로 남는다.
	 * 읽는 자: rev_init 이 창 능력 표를 붙일지 판단하고,
	 *   iproc_pcie_setup() 이 iproc_pcie_map_ranges() 를 부를지 정한다.
	 * 값 범위: true / false.
	 * 동기화: probe 에서 한 번 설정된 뒤 읽기 전용이다. */
	bool need_ob_cfg;
	/* [한국어] 위 struct iproc_pcie_ob. 값으로 박아 두어 별도 할당이 필요 없다.
	 * 설정자/읽는 자/값 범위/동기화는 그 구조체의 두 필드 주석 참조. */
	struct iproc_pcie_ob ob;
	/* [한국어] 이 변종의 바깥 창 능력 표(상류 주석).
	 * 설정자: rev_init 이 need_ob_cfg 가 참일 때만 paxb_ob_map 또는
	 *   paxb_v2_ob_map 을 가리키게 한다.
	 * 읽는 자: iproc_pcie_setup_ob() 이 창마다 지원 크기 목록을 꺼내 쓴다.
	 * 값 범위: 정적 const 배열의 주소 또는 NULL.
	 * 동기화: rev_init 이후 읽기 전용이다. */
	const struct iproc_pcie_ob_map *ob_map;

	/* [한국어] 안쪽 영역을 SW 가 설정해야 하는가(상류 주석).
	 * 설정자: 결합 드라이버. platform.c 는 DT 의 dma-ranges 유무를 그대로 넣고,
	 *   bcma.c 는 채우지 않아 false 로 남는다.
	 * 읽는 자: iproc_pcie_setup() 이 iproc_pcie_map_dma_ranges() 를 부를지 정하고,
	 *   iproc_pcie_invalidate_mapping() 도 이 값을 본다.
	 * 값 범위: true / false.
	 * 동기화: probe 에서 한 번 설정된 뒤 읽기 전용이다. */
	bool need_ib_cfg;
	/* [한국어] 위 struct iproc_pcie_ib. 역시 값으로 박아 둔다.
	 * 설정자/읽는 자/값 범위/동기화는 그 구조체의 필드 주석 참조. */
	struct iproc_pcie_ib ib;
	/* [한국어] 이 변종의 안쪽 영역 능력 표.
	 * 설정자: rev_init 이 PAXB_V2 에서만 paxb_v2_ib_map 을 가리키게 한다.
	 *   need_ib_cfg 와 무관하게 무조건 붙이는 점이 바깥 창과 다르다.
	 * 읽는 자: iproc_pcie_setup_ib() 과 iproc_pcie_ib_is_in_use().
	 * 값 범위: 정적 const 배열의 주소 또는 NULL.
	 * 동기화: rev_init 이후 읽기 전용이다.
	 * (상류 주석이 이 필드를 "outbound mapping region" 이라 적었으나 안쪽 표다 —
	 *   ib_map 이라는 이름과 실제 사용처가 모두 안쪽을 가리킨다.) */
	const struct iproc_pcie_ib_map *ib_map;

	/* [한국어] MSI 쓰기를 외부 인터럽트 컨트롤러로 배선하는 추가 설정이 필요한가(상류 주석).
	 * 설정자: rev_init 이 PAXB_V2 와 PAXC_V2 에 세운다.
	 * 읽는 자: iproc_pcie_msi_enable() 이 참이면 iproc_pcie_msi_steer() 를 먼저 부른다.
	 * 값 범위: true / false.
	 * 동기화: rev_init 이후 읽기 전용이다. */
	bool need_msi_steer;
	/* [한국어] MSI 컨트롤러 상태(상류 주석).
	 * 설정자: pcie-iproc-msi.c 의 iproc_msi_init() 이 채우고(:1550),
	 *   iproc_msi_exit() 이 NULL 로 되돌린다(:1744).
	 * 읽는 자: 거의 전부 pcie-iproc-msi.c 안이다(18회). 공용 코어는 이 포인터를
	 *   들고만 있고 내용을 들여다보지 않는다 — struct iproc_msi 의 정의가
	 *   그 파일 안에 숨어 있기 때문이다.
	 * 값 범위: 유효한 포인터 또는 NULL(MSI 미사용 또는 해제 후).
	 * 동기화: probe/remove 경로에서만 바뀌므로 별도 락이 없다. */
	struct iproc_msi *msi;
};

/* [한국어] 공용 코어의 진입점. 결합 드라이버가 채운 구조체와 브리지 창 목록을 받아
 * 변종 판정부터 PCI 버스 스캔까지 진행한다. pcie-iproc.c:2782 에 정의되고
 * :3057 에서 EXPORT_SYMBOL 로 공개된다.
 * 호출자: platform.c:107 과 bcma.c:340. */
int iproc_pcie_setup(struct iproc_pcie *pcie, struct list_head *res);
/* [한국어] 버스를 해체하고 MSI 와 PHY 를 정리한다. pcie-iproc.c:3060 정의,
 * :3071 에서 EXPORT_SYMBOL.
 * 호출자: platform.c:121 과 bcma.c:377. */
void iproc_pcie_remove(struct iproc_pcie *pcie);
/* [한국어] PERST 를 assert 하고 500ms 기다린다. 버스는 해체하지 않는다.
 * pcie-iproc.c:1384 정의, :1391 에서 EXPORT_SYMBOL_GPL(위 둘과 달리 GPL 한정).
 * 호출자: platform.c:128 하나뿐이다 — bcma 판에는 shutdown 콜백이 없다. */
int iproc_pcie_shutdown(struct iproc_pcie *pcie);

/* [한국어] MSI 컨트롤러 파일이 빌드에 포함될 때만 진짜 선언을 쓴다. */
#ifdef CONFIG_PCIE_IPROC_MSI
/* [한국어] pcie-iproc-msi.c 가 정의한다. EXPORT_SYMBOL 로 공개되어 있어 두 파일이
 * 별도 모듈로 빌드되어도 링크된다.
 * 호출자: iproc_pcie_msi_enable() [pcie-iproc.c:1367]. */
int iproc_msi_init(struct iproc_pcie *pcie, struct device_node *node);
/* [한국어] 짝이 되는 해제 함수.
 * 호출자: iproc_pcie_msi_disable() [pcie-iproc.c:1376]. */
void iproc_msi_exit(struct iproc_pcie *pcie);
/* [한국어] CONFIG_PCIE_IPROC_MSI 가 꺼진 빌드. */
#else
/* [한국어]
 * iproc_msi_init - MSI 파일이 빠진 빌드에서 쓰이는 인라인 더미
 *
 * @pcie: 컨트롤러. 쓰지 않는다.
 * @node: msi-parent 가 가리키는 DT 노드. 쓰지 않는다.
 * @return: 항상 -ENODEV.
 *
 * CONFIG_PCIE_IPROC_MSI 가 꺼져 pcie-iproc-msi.c 가 빌드되지 않을 때
 * 링크 오류 대신 이 인라인이 대체한다.
 *
 * -ENODEV 를 고른 것이 요점이다. 호출자인 iproc_pcie_msi_enable() 은 이
 * 실패를 그대로 위로 전하고, 그 위의 iproc_pcie_setup() 은 상류 코드대로
 * 오류로 다루지 않고 "not using iProc MSI" 라는 정보 수준 로그만 남기고
 * 진행한다(pcie-iproc.c 의 해당 블록). 즉 MSI 없이도 컨트롤러는 정상
 * 동작하며, 장치들은 INTx 를 쓰거나 다른 MSI 컨트롤러를 쓰게 된다.
 *
 * 이 방식 덕분에 공용 코어의 호출부에는 #ifdef 가 하나도 없다.
 * 설정에 따라 달라지는 부분을 헤더 한 곳에 몰아 둔 셈이다.
 *
 * 실행 컨텍스트: 호출자를 따른다(프로세스 컨텍스트). 아무 일도 하지 않는다.
 *
 * 호출 체인:  iproc_pcie_msi_enable() [pcie-iproc.c:1367] → [이 더미] → -ENODEV
 */
static inline int iproc_msi_init(struct iproc_pcie *pcie,
				 struct device_node *node)
{
	return -ENODEV;
}
/* [한국어]
 * iproc_msi_exit - MSI 파일이 빠진 빌드에서 쓰이는 인라인 더미(해제 쪽)
 *
 * @pcie: 컨트롤러. 쓰지 않는다.
 * @return: 없음.
 *
 * 위 init 더미의 짝이며 본문이 비어 있다. init 이 항상 실패했으므로
 * 해제할 것도 없기 때문이다.
 *
 * 빈 함수라도 있어야 하는 이유는 호출자 iproc_pcie_msi_disable() 이
 * 조건 없이 이것을 부르기 때문이다. 그쪽에 #ifdef 를 두지 않으려는
 * 설계이며, 컴파일러가 인라인으로 지워 버리므로 실행 비용도 없다.
 *
 * 실행 컨텍스트: 호출자를 따른다(프로세스 컨텍스트).
 *
 * 호출 체인:  iproc_pcie_msi_disable() [pcie-iproc.c:1376] → [이 더미] → (아무 일 없음)
 */
static inline void iproc_msi_exit(struct iproc_pcie *pcie)
{
}
/* [한국어] #ifdef 블록 끝. 이 구조 덕분에 공용 코어는 MSI 파일의 존재 여부와 무관하게
 * 같은 코드로 컴파일된다 — 호출부에 #ifdef 가 하나도 없다. */
#endif

/* [한국어] 헤더 가드 끝. */
#endif /* _PCIE_IPROC_H */
