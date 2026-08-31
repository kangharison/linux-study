/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Synopsys DesignWare PCIe host controller driver
 *
 * Copyright (C) 2013 Samsung Electronics Co., Ltd.
 *		https://www.samsung.com
 *
 * Author: Jingoo Han <jg1.han@samsung.com>
 */

/*
 * [한국어 설명] Synopsys DesignWare PCIe 컨트롤러 공용 헤더 (pcie-designware.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 Synopsys 의 DesignWare(이하 DWC) PCIe 컨트롤러 IP 를 다루는
 * 모든 드라이버가 공유하는 단 하나의 계약서다. IP 안에 실제로 존재하는
 * 레지스터 오프셋과 비트 필드(포트 로직, iATU, eDMA, RAS-DES, PTM,
 * Gen3/Gen4 이퀄라이제이션)를 #define 으로 고정하고, 그 위에서 동작하는
 * 세 가지 자료형 — 컨트롤러 전체를 나타내는 struct dw_pcie, 루트 컴플렉스
 * 쪽 상태인 struct dw_pcie_rp, 엔드포인트 쪽 상태인 struct dw_pcie_ep — 을
 * 정의한다. 나아가 SoC 별 접착(glue) 드라이버가 코어에 끼워 넣는 세 개의
 * 콜백 테이블(struct dw_pcie_ops / dw_pcie_host_ops / dw_pcie_ep_ops)을
 * 선언해, "공통 코어가 무엇을 하고 SoC 드라이버가 무엇을 채워야 하는가" 의
 * 경계를 이 파일 한 장으로 규정한다. 마지막으로 DBI(Data Bus Interface)
 * 레지스터 접근을 폭(byte/word/dword)과 대상(dbi / dbi2 / function 별
 * 오프셋)별로 감싸는 static inline 래퍼 무리를 제공한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * DWC PCIe IP 는 SoC 벤더가 가장 널리 라이선스하는 PCIe 컨트롤러다.
 * 이 트리의 drivers/pci/controller/dwc/ 에는 .c 파일이 40개 있고, 그중
 * 5개(pcie-designware.c, -host.c, -ep.c, -debugfs.c, -plat.c)가 공통
 * 코어이며 나머지 35개는 SoC 접착 드라이버다(dwc/Makefile 참조).
 * 계층은 세 단이다.
 *   (1) 맨 아래 pcie-designware.c — IP 버전 판별, iATU 창 개수 탐지,
 *       링크 설정처럼 호스트/엔드포인트 양쪽에 공통인 일.
 *   (2) 가운데 pcie-designware-host.c(루트 컴플렉스 역할)와
 *       pcie-designware-ep.c(엔드포인트 역할) — 둘은 배타적이며 각각
 *       CONFIG_PCIE_DW_HOST / CONFIG_PCIE_DW_EP 로 갈린다. 이 파일의
 *       끝부분에 있는 #ifdef 블록이 그 배타성을 표현한다.
 *   (3) 맨 위 SoC 접착 드라이버 — 클럭/리셋/PHY 를 켜고, struct dw_pcie 를
 *       할당한 뒤 dw_pcie_host_init() 또는 dw_pcie_ep_init() 을 부른다.
 * 이 헤더는 세 단 전부가 #include 하므로, 여기 정의된 구조체 배치가
 * 곧 DWC 드라이버 전체의 데이터 모델이다. 실행 컨텍스트는 커널 모듈이며,
 * 대부분 프로브/PM 같은 프로세스 컨텍스트에서 쓰이나 MSI 수신 경로처럼
 * 인터럽트 컨텍스트에서 만져지는 필드도 섞여 있다(각 필드 주석에 표시).
 *
 * === 타 모듈과의 연결 ===
 * 아래쪽으로는 "../../pci.h" 를 포함해 PCI 코어의 내부 헬퍼
 * (PCI_FIND_NEXT_CAP / PCI_FIND_NEXT_EXT_CAP 매크로 등)를 끌어 쓴다.
 * 호스트 모드에서는 struct pci_host_bridge 와 irq_domain 을 통해 PCI 코어
 * 및 IRQ 서브시스템에 붙고, 엔드포인트 모드에서는 <linux/pci-epc.h> /
 * <linux/pci-epf.h> 를 통해 drivers/pci/endpoint/ 의 EPC(Endpoint
 * Controller) 프레임워크에 붙는다. eDMA 는 <linux/dma/edma.h> 를 통해
 * dmaengine 으로 넘어간다.
 *
 * NVMe 와의 관계는 두 방향이며, 둘 다 이 헤더의 심볼을 직접 부르지는
 * 않는다(drivers/nvme 전체에서 dw_pcie_ 로 시작하는 심볼 호출은 0건임을
 * grep 으로 확인했다).
 *   (a) 호스트 방향: DWC 루트 포트 아래에 NVMe SSD 가 꽂히면, NVMe 드라이버가
 *       보는 config 공간 접근과 MSI-X 벡터는 모두 이 IP 를 통과한다. 그러나
 *       관계는 토폴로지상의 것이지 코드 호출 관계가 아니다.
 *   (b) 엔드포인트 방향: 이쪽이 실제로 코드가 이어지는 유일한 경로다.
 *       drivers/nvme/target/pci-epf.c (NVMe 타깃을 PCI 엔드포인트 함수로
 *       노출하는 드라이버 — 이 저장소는 부분 체크아웃이라 로컬에는 없고
 *       기준 스냅숏 1f0e418bb6 에만 있다) 가 pci_epc_set_bar(),
 *       pci_epc_raise_irq(), pci_epc_mem_map(), pci_epc_set_msi/msix(),
 *       pci_epc_write_header() 를 부른다. EPC 코어가 그 호출을
 *       epc->ops 테이블로 넘기고, SoC 가 DWC 라면 그 테이블이 곧
 *       pcie-designware-ep.c 의 epc_ops 다. 즉 함수 포인터 한 단계를
 *       거치는 간접 호출이며, 직접 호출 체인은 존재하지 않는다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct dw_pcie: 컨트롤러 하나를 통째로 표현한다. DBI/iATU/ELBI 의
 *   가상 주소, iATU 창 개수와 정렬 제약, 클럭·리셋 묶음, 그리고 호스트용
 *   pp 와 엔드포인트용 ep 를 값으로(포인터가 아니라 임베드해서) 품는다.
 *   그래서 to_dw_pcie_from_pp() / to_dw_pcie_from_ep() 가 container_of 로
 *   역참조할 수 있다.
 * - struct dw_pcie_rp: 루트 포트 상태. config 공간 창, IP 내장 MSI 수신기의
 *   벡터 비트맵과 irq_domain, ECAM 사용 여부.
 * - struct dw_pcie_ep: 엔드포인트 상태. EPC 핸들, function 리스트,
 *   inbound/outbound iATU 창 점유 비트맵, MSI 발사에 쓰는 전용 창.
 * - struct dw_pcie_ops / dw_pcie_host_ops / dw_pcie_ep_ops: SoC 접착
 *   드라이버가 채우는 세 콜백 테이블. link_up/start_link 처럼 IP 바깥의
 *   벤더 레지스터를 봐야만 알 수 있는 일들이 여기 모인다.
 * - dw_pcie_readl_dbi() 계열 static inline: DBI 접근의 유일한 통로.
 *   폭을 인자로 받는 dw_pcie_read_dbi() 한 곳으로 모여, SoC 가
 *   ops->read_dbi 를 제공하면 그쪽으로 우회할 수 있게 되어 있다.
 * - dw_pcie_dbi_ro_wr_en()/_dis(): 읽기 전용이어야 할 config 레지스터를
 *   잠시 쓰기 가능하게 여는 잠금 해제 쌍. BAR/LNKCAP 을 드라이버가
 *   직접 써야 하는 자리마다 이 쌍이 등장한다.
 */

#ifndef _PCIE_DESIGNWARE_H
#define _PCIE_DESIGNWARE_H

/* [한국어] FIELD_PREP()/FIELD_GET()/FIELD_MODIFY() 를 쓰기 위해 필요하다.
 * 이 헤더가 정의하는 레지스터 상수 대부분이 GENMASK() 로 만든
 * 비트 필드 마스크이고, 그 마스크에 값을 넣고 빼는 일은 전부
 * 이 매크로들이 한다. 예: PORT_LINK_MODE(n) 은
 * FIELD_PREP(PORT_LINK_MODE_MASK, n) 으로 정의되어 있다. */
#include <linux/bitfield.h>
/* [한국어] GENMASK()/BIT() 로 비트 마스크를 만들고, test_bit()/set_bit() 로
 * dw_pcie.caps 비트맵을 다루기 위해 필요하다. 아래 dw_pcie_cap_is() /
 * dw_pcie_cap_set() 매크로가 직접 test_bit()/set_bit() 를 호출한다. */
#include <linux/bitops.h>
/* [한국어] struct clk_bulk_data 와 clk_bulk_ 계열 API 를 위해 필요하다.
 * dw_pcie 안의 app_clks[] / core_clks[] 배열이 이 타입이며,
 * DWC 코어가 SoC 대신 클럭 묶음을 한꺼번에 켜고 끄는 데 쓴다. */
#include <linux/clk.h>
/* [한국어] dma_addr_t 타입을 위해 필요하다. dw_pcie_rp.msi_data 가 이 타입인데,
 * IP 내장 MSI 수신기가 쓰는 목적지 주소를 장치가 보는 형태(버스 주소)로
 * 담아야 하기 때문이다. */
#include <linux/dma-mapping.h>
/* [한국어] struct dw_edma_chip 을 위해 필요하다. DWC IP 는 컨트롤러 안에
 * eDMA(embedded DMA) 엔진을 선택적으로 품는데, 그 엔진을 커널의
 * dmaengine 서브시스템에 등록하는 서술자가 dw_pcie.edma 필드다. */
#include <linux/dma/edma.h>
/* [한국어] struct gpio_desc 와 gpiod_ 계열 API 를 위해 필요하다.
 * dw_pcie.pe_rst 가 PERST#(PCIe 기본 리셋) 신호를 물고 있는
 * GPIO 서술자이며, 엔드포인트 쪽에서 상대를 리셋할 때 토글한다. */
#include <linux/gpio/consumer.h>
/* [한국어] irq_domain, irq_chip 같은 IRQ 서브시스템 타입을 위해 필요하다.
 * 호스트 모드에서 IP 내장 MSI 수신기를 IRQ 도메인으로 노출해야 하고,
 * dw_pcie_rp.irq_domain / msi_irq_chip 이 그 결과물이다. */
#include <linux/irq.h>
/* [한국어] MSI 코어 정의를 위해 필요하다. 이 헤더 자체는 MSI 타입을 직접
 * 쓰지 않지만, MSI 도메인 관련 선언이 이 경로로 따라 들어온다. */
#include <linux/msi.h>
/* [한국어] struct pci_bus, struct pci_host_bridge, enum pci_barno,
 * PCI_STD_NUM_BARS 등 PCI 코어의 공개 타입 전부를 위해 필요하다.
 * 이 헤더의 구조체 다수가 그 타입을 필드로 갖는다. */
#include <linux/pci.h>
/* [한국어] struct pci_config_window 와 pci_generic_ecam_ops 를 위해 필요하다.
 * 최근 DWC 호스트 코드는 조건이 맞으면 자체 config 접근 대신 표준
 * ECAM 창을 만들어 쓰며(dw_pcie_rp.cfg / ecam_enabled),
 * 그 창의 서술자가 이 헤더에서 온다. */
#include <linux/pci-ecam.h>
/* [한국어] struct reset_control_bulk_data 와 reset_control_bulk_ 계열 API 를
 * 위해 필요하다. dw_pcie 의 app_rsts[] / core_rsts[] 배열이 이 타입으로,
 * 클럭과 마찬가지로 리셋 라인도 묶음 단위로 다룬다. */
#include <linux/reset.h>

/* [한국어] 엔드포인트 컨트롤러(EPC) 프레임워크의 계약을 위해 필요하다.
 * struct pci_epc, struct pci_epc_ops, struct pci_epc_features,
 * enum pci_epc_bar_type 이 여기서 온다. pcie-designware-ep.c 가
 * 이 계약의 구현체이고, dw_pcie_ep.epc 가 그 등록 결과다. */
#include <linux/pci-epc.h>
/* [한국어] 엔드포인트 함수(EPF) 프레임워크를 위해 필요하다.
 * struct pci_epf_bar, struct pci_epf_bar_submap,
 * struct pci_epf_header, struct pci_epf_msix_tbl 이 여기서 온다.
 * 호스트가 아니라 상대편 EPF 드라이버가 채워 주는 요청 서술자들이다. */
#include <linux/pci-epf.h>

/* [한국어] PCI 코어의 **내부** 헤더다. 공개 헤더가 아니라 drivers/pci/ 안에서만
 * 쓰라고 만든 것이어서 상대 경로로 끌어온다. 여기서 필요한 것은
 * PCI_FIND_NEXT_CAP() / PCI_FIND_NEXT_EXT_CAP() 매크로로,
 * config 공간의 capability 연결 리스트를 순회하는 로직을
 * 접근 함수만 갈아 끼워 재사용할 수 있게 만든 것이다.
 * pcie-designware-ep.c 의 dw_pcie_ep_find_capability() 가 이 매크로에
 * DBI 접근 함수를 물려 쓴다. */
#include "../../pci.h"

/* [한국어] IP 코어 버전 상수들. dw_pcie_version_detect() 가
 * PCIE_VERSION_NUMBER(0x8F8) 레지스터를 읽어 dw_pcie.version 에
 * 담는데, IP 가 그 레지스터에 버전을 **ASCII 문자열로** 넣어 둔다.
 * 그래서 상수가 0x3336352a 처럼 보이는 것이다 —
 * 0x33='3', 0x36='6', 0x35='5', 0x2a='*' 로 "365*" 이고
 * 마지막 '*' 가 리비전 문자 a 자리를 대신한다.
 * 상류 주석이 밝히듯 v4.70a 부터 이 코어가 커널에 정식 지원되며,
 * 그 이전 버전(365A/460A)은 예외 처리를 위해서만 남아 있다.
 * 값이 ASCII 라서 숫자 크기 비교가 곧 버전 대소 비교가 된다 —
 * 아래 dw_pcie_ver_is_ge() 가 그 성질에 기대고 있다. */
/* DWC PCIe IP-core versions (native support since v4.70a) */
#define DW_PCIE_VER_365A		0x3336352a	/* [한국어] "365*" — v3.65a. 커널 정식 지원 이전 코어. */
#define DW_PCIE_VER_460A		0x3436302a	/* [한국어] "460*" — v4.60a. iATU 의 UPPER_LIMIT 레지스터(4GB 초과 창)가 이 버전부터 생겨서, 코드 곳곳의 dw_pcie_ver_is_ge(pci, 460A) 분기 기준점이 된다. */
#define DW_PCIE_VER_470A		0x3437302a	/* [한국어] "470*" — v4.70a. 상류 주석대로 커널이 이 IP 를 정식 지원하기 시작한 첫 버전. eDMA 레지스터가 아직 포트 로직 공간(0x970) 안에 있다. */
#define DW_PCIE_VER_480A		0x3438302a	/* [한국어] "480*" — v4.80a. iATU/eDMA 레지스터 공간이 재설계되어 뷰포트(창 선택기) 방식이 unroll(창마다 독립 주소) 방식으로 바뀐 분기점. 아래 iATU 주석 참조. */
#define DW_PCIE_VER_490A		0x3439302a	/* [한국어] "490*" — v4.90a. */
#define DW_PCIE_VER_500A		0x3530302a	/* [한국어] "500*" — v5.00a. */
#define DW_PCIE_VER_520A		0x3532302a	/* [한국어] "520*" — v5.20a. */
#define DW_PCIE_VER_540A		0x3534302a	/* [한국어] "540*" — v5.40a. */
#define DW_PCIE_VER_562A		0x3536322a	/* [한국어] "562*" — v5.62a. 현재 이 헤더가 아는 가장 최신 코어. */

/* [한국어] 버전 비교의 공통 뼈대. _ver 는 365A 처럼 접미사만 받아
 * 토큰 결합(##)으로 DW_PCIE_VER_365A 를 만들고, _op 자리에 비교
 * 연산자를 그대로 끼워 넣는다. 호출자가 상수 이름 전체를 쓰지
 * 않아도 되게 하는 것이 목적이며, 앞의 __ 는 아래 두 매크로만
 * 쓰라는 내부용 표시다. */
#define __dw_pcie_ver_cmp(_pci, _ver, _op) \
	((_pci)->version _op DW_PCIE_VER_ ## _ver)

/* [한국어] IP 버전이 정확히 _ver 인지. 특정 리비전에만 있는 버그를
 * 우회할 때 쓴다. version 필드는 dw_pcie_version_detect() 가
 * 채우므로, 그 호출 이후에만 의미가 있다. */
#define dw_pcie_ver_is(_pci, _ver) __dw_pcie_ver_cmp(_pci, _ver, ==)

/* [한국어] IP 버전이 _ver 이상인지. 새 버전에서 추가된 기능이
 * 있는지 묻는 가장 흔한 형태다. 버전 상수가 ASCII 라서 숫자
 * 크기 비교가 곧 버전 순서 비교가 된다는 점에 기대고 있다
 * (예: "470*" 0x3437302a < "480*" 0x3438302a).
 * 실제 쓰임: pcie-designware.c 가 iATU 상위 한계 레지스터를
 * 건드리기 전에 dw_pcie_ver_is_ge(pci, 460A) 로 확인한다. */
#define dw_pcie_ver_is_ge(_pci, _ver) __dw_pcie_ver_cmp(_pci, _ver, >=)

/* [한국어] 버전과 "타입"을 함께 보는 변형. 타입은
 * PCIE_VERSION_TYPE(0x8FC) 레지스터에서 읽어 dw_pcie.type 에
 * 담기는 값으로, 같은 버전 IP 안에서도 구성이 갈릴 때 쓰라고
 * 마련된 자리다.
 * 주의 — 이 매크로는 DW_PCIE_VER_TYPE_<_type> 이라는 상수로
 * 전개되는데, 그런 이름의 상수는 이 트리 어디에도 정의되어 있지
 * 않고 이 매크로를 부르는 곳도 없다(grep 결과 0건).
 * 즉 현재는 쓰이지 않는 확장 지점이며, 실제로 쓰려면 상수부터
 * 정의해야 한다. */
#define dw_pcie_ver_type_is(_pci, _ver, _type) \
	(__dw_pcie_ver_cmp(_pci, _ver, ==) && \
	 __dw_pcie_ver_cmp(_pci, TYPE_ ## _type, ==))

/* [한국어] 위와 같으나 타입 쪽만 >= 로 비교한다(버전은 여전히 ==).
 * 이름은 _ge 지만 버전 비교는 등호라는 점에 주의.
 * 이것 역시 호출자가 0건이고 TYPE_ 상수도 없는 미사용 확장 지점이다. */
#define dw_pcie_ver_type_is_ge(_pci, _ver, _type) \
	(__dw_pcie_ver_cmp(_pci, _ver, ==) && \
	 __dw_pcie_ver_cmp(_pci, TYPE_ ## _type, >=))

/* DWC PCIe controller capabilities */
/* [한국어] 런타임에 알아낸 컨트롤러 성질을 담는 비트 번호들.
 * 값 자체가 dw_pcie.caps(unsigned long 비트맵)의 비트 인덱스이며,
 * 아래 dw_pcie_cap_is()/dw_pcie_cap_set() 이 test_bit()/set_bit() 의
 * 인자로 그대로 넘긴다. 컴파일 타임 상수가 아니라 탐지 결과라는
 * 점이 중요하다 — 같은 드라이버 바이너리가 여러 SoC 를 지원해야
 * 하기 때문이다. */
#define DW_PCIE_CAP_REQ_RES		0	/* [한국어] 비트 0: 이 컨트롤러가 리소스를 스스로 요청해야 하는지 표시. */
#define DW_PCIE_CAP_IATU_UNROLL		1	/* [한국어] 비트 1: iATU 레지스터가 unroll 방식(창마다 독립 주소)인지. dw_pcie_iatu_detect() 가 PCIE_ATU_VIEWPORT 를 읽어 0xFFFFFFFF(=그 레지스터가 없음)면 이 비트를 세운다. 창을 바꿀 때마다 뷰포트 선택기를 다시 쓰지 않아도 되므로 접근 비용이 크게 준다. */
#define DW_PCIE_CAP_CDM_CHECK		2	/* [한국어] 비트 2: CDM(Configuration Dependent Module) 레지스터 무결성 검사 기능이 있는지. PCIE_PL_CHK_REG_ 계열 레지스터로 config 레지스터가 조용히 손상됐는지 감시하는 안전 기능이다. */

/* [한국어] caps 비트맵에서 성질 하나를 조회한다. _cap 은 IATU_UNROLL
 * 처럼 접미사만 주면 되고 토큰 결합으로 상수 이름이 완성된다.
 * test_bit() 은 원자적 읽기지만 여기서는 프로브 때 한 번 세워지고
 * 이후 읽기만 하는 값이라 동기화 목적은 아니다. */
#define dw_pcie_cap_is(_pci, _cap) \
	test_bit(DW_PCIE_CAP_ ## _cap, &(_pci)->caps)

/* [한국어] caps 비트맵에 성질 하나를 기록한다. 호출 시점은 초기화
 * 경로(주로 dw_pcie_iatu_detect())로 한정되며, 그 뒤로는 읽기 전용처럼
 * 다뤄진다. */
#define dw_pcie_cap_set(_pci, _cap) \
	set_bit(DW_PCIE_CAP_ ## _cap, &(_pci)->caps)

/* Parameters for the waiting for iATU enabled routine */
/* [한국어] iATU 창을 켠 뒤 실제로 켜졌는지 폴링하는 루프의 상수.
 * iATU 활성화는 즉시 반영되지 않을 수 있어, ENABLE 비트를 쓴 다음
 * 다시 읽어 확인해야 한다. 확인 없이 곧바로 그 창으로 접근하면
 * 아직 변환이 걸리지 않은 주소로 TLP 를 쏘게 된다.
 * pcie-designware.c 의 dw_pcie_prog_outbound_atu() /
 * dw_pcie_prog_inbound_atu() / dw_pcie_prog_ep_inbound_atu() 세 곳이
 * 모두 이 쌍을 쓴다. */
#define LINK_WAIT_MAX_IATU_RETRIES	5	/* [한국어] 최대 재시도 횟수. 5회 안에 ENABLE 비트가 서지 않으면 -ETIMEDOUT 으로 포기한다. */
#define LINK_WAIT_IATU			9	/* [한국어] 재시도 사이 대기 시간(밀리초). mdelay(9) 로 바쁜 대기를 한다 — 이 경로가 잠들 수 없는 문맥에서도 불릴 수 있기 때문이다. 최악의 경우 5 x 9 = 45ms 를 소모한다. */

/* Synopsys-specific PCIe configuration registers */
/* [한국어] 여기서부터가 "포트 로직(Port Logic)" 레지스터 영역이다.
 * PCI 규격이 정한 config 공간(0x000~0x0FF)과 확장 config 공간이 끝난
 * 뒤, 0x700 대부터는 Synopsys 가 자기 IP 를 위해 정의한 벤더 전용
 * 레지스터가 놓인다. 표준 lspci 로는 보이지 않고 DBI 를 통해서만
 * 접근할 수 있으며, 오프셋과 비트 배치의 근거는 DWC 데이터북이다.
 * 아래 상수들은 전부 DBI 베이스 기준 바이트 오프셋이다. */
#define PCIE_PORT_FORCE			0x708	/* [한국어] 링크 동작을 강제로 비트는 레지스터. */
#define PORT_FORCE_DO_DESKEW_FOR_SRIS	BIT(23)	/* [한국어] SRIS(Separate Reference clock with Independent Spread) 구성에서 레인 간 도착 시간 차(skew)를 보정하도록 강제한다. 송수신 양쪽이 각자 클럭을 쓰는 구성에서는 레인마다 지연이 달라질 수 있어 별도 보정이 필요하다. */

#define PCIE_PORT_AFR			0x70C	/* [한국어] ACK Frequency and L0-L1 ASPM Control. 이름 그대로 ACK 를 얼마나 자주 보낼지와 저전력 상태(ASPM) 진입 조건을 함께 담는다. dw_pcie_setup() 이 n_fts[0] 이 설정된 SoC 에서 이 레지스터를 손본다. */
#define PORT_AFR_N_FTS_MASK		GENMASK(15, 8)	/* [한국어] N_FTS 필드(비트 15:8). FTS(Fast Training Sequence)는 L0s 저전력에서 깨어날 때 수신기 재동기화를 위해 보내는 훈련 시퀀스이고, 이 값은 그 개수다. 링크 파트너의 수신기가 느릴수록 더 많이 필요하다. */
#define PORT_AFR_N_FTS(n)		FIELD_PREP(PORT_AFR_N_FTS_MASK, n)	/* [한국어] n 을 위 필드 자리로 옮겨 넣는다. FIELD_PREP 은 마스크의 최하위 비트 위치만큼 자동으로 시프트해 준다. */
#define PORT_AFR_CC_N_FTS_MASK		GENMASK(23, 16)	/* [한국어] Common Clock 모드용 N_FTS(비트 23:16). 양쪽이 같은 기준 클럭을 공유하면 재동기화가 빨라 별도 값을 둔다. */
#define PORT_AFR_CC_N_FTS(n)		FIELD_PREP(PORT_AFR_CC_N_FTS_MASK, n)	/* [한국어] Common Clock 쪽 필드에 값을 넣는 짝 매크로. dw_pcie_setup() 은 두 필드에 같은 n_fts[0] 을 쓴다. */
#define PORT_AFR_ENTER_ASPM		BIT(30)	/* [한국어] ASPM(Active State Power Management) 진입을 허용하는 비트. 링크가 유휴일 때 L0s/L1 으로 내려가도 되는지를 결정한다. */
#define PORT_AFR_L0S_ENTRANCE_LAT_SHIFT	24	/* [한국어] L0s 진입 지연 필드의 시프트 값. 아래 마스크와 짝이며, FIELD_PREP 을 쓰지 않는 옛 코드가 직접 시프트할 때 쓰라고 남겨 둔 상수다. */
#define PORT_AFR_L0S_ENTRANCE_LAT_MASK	GENMASK(26, 24)	/* [한국어] L0s 진입 지연 필드(비트 26:24). L0s 는 가장 얕은 저전력 상태로, 복귀가 빨라 지연 허용치가 작다. 주의: 비트 24~26 은 위 CC_N_FTS 마스크(23:16)와 겹치지 않지만 같은 레지스터를 나눠 쓴다. */
#define PORT_AFR_L1_ENTRANCE_LAT_SHIFT	27	/* [한국어] L1 진입 지연 필드의 시프트 값. */
#define PORT_AFR_L1_ENTRANCE_LAT_MASK	GENMASK(29, 27)	/* [한국어] L1 진입 지연 필드(비트 29:27). L1 은 L0s 보다 깊은 저전력 상태여서 복귀 지연이 크고, 진입 판단 기준도 따로 둔다. */

#define PCIE_PORT_LINK_CONTROL		0x710	/* [한국어] 링크 폭과 링크 계층 동작을 정하는 핵심 레지스터. dw_pcie_setup() 과 dw_pcie_link_set_max_link_width() 가 여기를 만진다. */
#define PORT_LINK_DLL_LINK_EN		BIT(5)	/* [한국어] DLL(Data Link Layer) Link Enable. 데이터 링크 계층을 켜서 링크가 실제로 "up" 상태까지 올라갈 수 있게 한다. dw_pcie_setup() 이 항상 세운다. */
#define PORT_LINK_FAST_LINK_MODE	BIT(7)	/* [한국어] Fast Link Mode. LTSSM 타이머를 대폭 줄여 시뮬레이션에서 링크 훈련을 빨리 끝내는 검증용 모드다. 실물에서는 규격 타이밍을 어기므로 켜면 안 되고, dw_pcie_setup() 이 명시적으로 지운다. */
#define PORT_LINK_MODE_MASK		GENMASK(21, 16)	/* [한국어] Link Mode 필드(비트 21:16). 하드웨어가 사용할 레인 수를 지정한다. */
#define PORT_LINK_MODE(n)		FIELD_PREP(PORT_LINK_MODE_MASK, n)	/* [한국어] 레인 수 인코딩 n 을 필드 자리로 옮긴다. */
#define PORT_LINK_MODE_1_LANES		PORT_LINK_MODE(0x1)	/* [한국어] x1. 인코딩이 (레인수 - 1) 을 모두 1 로 채운 비트열이라 1레인은 0b000001 이다. */
#define PORT_LINK_MODE_2_LANES		PORT_LINK_MODE(0x3)	/* [한국어] x2 — 0b000011. */
#define PORT_LINK_MODE_4_LANES		PORT_LINK_MODE(0x7)	/* [한국어] x4 — 0b000111. */
#define PORT_LINK_MODE_8_LANES		PORT_LINK_MODE(0xf)	/* [한국어] x8 — 0b001111. */
#define PORT_LINK_MODE_16_LANES		PORT_LINK_MODE(0x1f)	/* [한국어] x16 — 0b011111. 이 인코딩 규칙 덕에 코드가 표를 두지 않고 (1 << n) - 1 로 계산할 수 있다. */

#define PCIE_PORT_LANE_SKEW		0x714	/* [한국어] 레인별로 의도적인 지연(skew)을 삽입하는 레지스터. 보드 배선 길이 차이를 보정하거나 검증용으로 일부러 틀어 볼 때 쓴다. */
#define PORT_LANE_SKEW_INSERT_MASK	GENMASK(23, 0)	/* [한국어] 삽입할 skew 값 필드(비트 23:0). 24비트를 레인당 3비트씩 8레인으로 나눠 쓰는 배치다. 0 이면 보정 없음. */

#define PCIE_PORT_DEBUG0		0x728	/* [한국어] 링크 상태 관측용 레지스터 0. 하위 6비트에 LTSSM(Link Training and Status State Machine)의 현재 상태가 그대로 노출된다. dw_pcie_get_ltssm() 이 SoC 콜백이 없을 때 여기를 읽는다. */
#define PORT_LOGIC_LTSSM_STATE_MASK	0x3f	/* [한국어] LTSSM 상태 필드(비트 5:0). 값 6비트라 0x00~0x3f 범위이며, 아래 enum dw_pcie_ltssm 의 값들과 1:1 대응한다(그 enum 주석에 "PCIE_PORT_DEBUG0 bits 0:5 와 맞춰야 한다"고 적힌 이유다). */
#define PORT_LOGIC_LTSSM_STATE_L0	0x11	/* [한국어] L0 = 정상 동작 상태의 코드값. 링크가 완전히 훈련되어 TLP 를 주고받을 수 있는 유일한 상태이므로, "링크가 살아 있는가"를 판단하는 기준이 된다. */
#define PCIE_PORT_DEBUG1		0x72C	/* [한국어] 링크 상태 관측용 레지스터 1. LTSSM 코드 대신 요약된 상태 비트를 준다. */
#define PCIE_PORT_DEBUG1_LINK_UP		BIT(4)	/* [한국어] 링크가 up 인지. 기본 dw_pcie_link_up() 은 이 비트가 서 있고 동시에 아래 IN_TRAINING 이 꺼져 있을 때만 up 으로 인정한다 — 훈련 도중에도 이 비트가 잠깐 설 수 있기 때문이다. */
#define PCIE_PORT_DEBUG1_LINK_IN_TRAINING	BIT(29)	/* [한국어] 링크가 재훈련 중인지. up 비트와 함께 봐야 하는 이유가 여기 있다. */

#define PCIE_LINK_WIDTH_SPEED_CONTROL	0x80C	/* [한국어] 링크 폭/속도 전환을 지시하는 레지스터. 위 PCIE_PORT_LINK_CONTROL 이 "몇 레인짜리 포트인가"를 정한다면, 이쪽은 "지금 협상해서 어떤 폭/속도로 갈 것인가"를 지시한다. */
#define PORT_LOGIC_N_FTS_MASK		GENMASK(7, 0)	/* [한국어] 이 레지스터에도 별도의 N_FTS 필드(비트 7:0)가 있다. dw_pcie_setup() 은 n_fts[0] 을 PCIE_PORT_AFR 에, n_fts[1] 을 여기에 쓴다 — 그래서 dw_pcie.n_fts 가 원소 2개짜리 배열이다. */
#define PORT_LOGIC_SPEED_CHANGE		BIT(17)	/* [한국어] Directed Speed Change. 이 비트를 세우면 하드웨어가 링크 속도 재협상(예: Gen1 -> Gen3)을 시작한다. 자기 지움(self-clearing) 성격이라 한 번 쓰면 다시 지울 필요가 없다. */
#define PORT_LOGIC_LINK_WIDTH_MASK	GENMASK(12, 8)	/* [한국어] 목표 링크 폭 필드(비트 12:8). */
#define PORT_LOGIC_LINK_WIDTH(n)	FIELD_PREP(PORT_LOGIC_LINK_WIDTH_MASK, n)	/* [한국어] 폭 값을 필드 자리로 옮긴다. */
#define PORT_LOGIC_LINK_WIDTH_1_LANES	PORT_LOGIC_LINK_WIDTH(0x1)	/* [한국어] x1. 주의 — 여기 인코딩은 위 PORT_LINK_MODE 와 달리 레인 수를 그대로 넣는다(1,2,4,8). 같은 파일 안에 서로 다른 두 인코딩이 있으니 혼동하지 말 것. */
#define PORT_LOGIC_LINK_WIDTH_2_LANES	PORT_LOGIC_LINK_WIDTH(0x2)	/* [한국어] x2 — 값 그대로 2. */
#define PORT_LOGIC_LINK_WIDTH_4_LANES	PORT_LOGIC_LINK_WIDTH(0x4)	/* [한국어] x4 — 값 그대로 4. */
#define PORT_LOGIC_LINK_WIDTH_8_LANES	PORT_LOGIC_LINK_WIDTH(0x8)	/* [한국어] x8 — 값 그대로 8. */

/* [한국어] 여기부터 다섯 개는 IP 에 내장된 MSI 수신기(호스트 모드 전용)의
 * 레지스터다. 보통 MSI 는 엔드포인트가 특정 메모리 주소에 쓰기를 하면
 * CPU 쪽 인터럽트 컨트롤러가 그것을 인터럽트로 바꿔 주는 방식인데,
 * 그런 장치가 없는 SoC 를 위해 DWC IP 가 그 역할을 대신할 수 있다.
 * 그 경우 컨트롤러가 MSI 쓰기를 가로채 자기 상태 레지스터에 비트로
 * 기록하고, 하나의 물리 IRQ 로 CPU 를 깨운다. 그래서 호스트 쪽에
 * chained IRQ 구조(부모 IRQ 하나가 여러 MSI 를 풀어내는 형태)가 생기며,
 * 그 처리는 pcie-designware-host.c 의 dw_chained_msi_isr() 와
 * dw_handle_msi_irq() 가 맡는다.
 * 벡터는 32개 단위로 묶여 "컨트롤 블록"을 이루고, 블록마다 아래 세
 * 레지스터(ENABLE/MASK/STATUS)가 MSI_REG_CTRL_BLOCK_SIZE 간격으로
 * 반복된다. 엔드포인트 모드에서는 쓰이지 않는다. */
#define PCIE_MSI_ADDR_LO		0x820	/* [한국어] IP 가 MSI 목적지로 삼을 주소의 하위 32비트. 이 주소로 오는 쓰기를 가로채 인터럽트로 바꾼다. 값은 dw_pcie_rp.msi_data(장치가 보는 버스 주소)에서 온다. */
#define PCIE_MSI_ADDR_HI		0x824	/* [한국어] 같은 주소의 상위 32비트. 64비트 주소 공간에서 MSI 목적지를 4GB 위에 둘 수 있게 한다. */
#define PCIE_MSI_INTR0_ENABLE		0x828	/* [한국어] 블록 0 의 벡터별 활성화 비트(32개). 비트가 0 이면 그 벡터로 오는 쓰기를 무시한다. */
#define PCIE_MSI_INTR0_MASK		0x82C	/* [한국어] 블록 0 의 벡터별 마스크. ENABLE 과 달리 일시적으로 가리는 용도이며, dw_pcie_rp.irq_mask[] 가 그 소프트웨어 사본이다(인터럽트 문맥에서 갱신되므로 pp->lock 로 보호한다). */
#define PCIE_MSI_INTR0_STATUS		0x830	/* [한국어] 블록 0 의 벡터별 대기 중 인터럽트 비트. dw_handle_msi_irq() 가 이 레지스터를 읽어 어떤 벡터가 왔는지 알아내고, 처리 후 같은 비트를 다시 써서(write-1-to-clear) 지운다. */

/* [한국어] 여기부터는 Gen3(8GT/s) 이상에서 쓰는 링크 이퀄라이제이션
 * 관련 레지스터다. 8GT/s 부터는 신호가 빨라 배선에서 왜곡이 커지므로,
 * 링크 훈련 중에 송신기 계수(pre/post cursor)를 서로 조정해 수신 눈
 * (eye)을 넓히는 절차를 밟는다. 그 절차의 파라미터가 이 레지스터군이다.
 * 중요한 구조 하나 — 이 레지스터들은 속도마다 별개의 "그림자(shadow)
 * 뱅크"를 갖고, 어느 뱅크를 보고 있는지는 아래 RATE_SHADOW_SEL 필드가
 * 정한다. 즉 같은 주소를 읽고 써도 SHADOW_SEL 값에 따라 다른 속도의
 * 설정을 만지게 된다. pcie-qcom-common.c 의
 * qcom_pcie_common_set_equalization() 이 8GT/s 부터 최대 속도까지
 * 돌면서 뱅크를 바꿔 가며 설정하는 것이 이 구조를 그대로 보여 준다. */
#define GEN3_RELATED_OFF			0x890	/* [한국어] Gen3 관련 제어 레지스터의 베이스. 이퀄라이제이션 뱅크 선택기가 여기 있어서, 아래 GEN3_EQ_ 레지스터들을 만지기 전에 항상 먼저 손대야 한다. */
#define GEN3_RELATED_OFF_GEN3_ZRXDC_NONCOMPL	BIT(0)	/* [한국어] 수신 종단 임피던스(ZRX-DC)의 규격 미준수 동작을 허용하는 비트. i.MX6 는 기본값이 문제라 이 비트를 지우는 우회를 넣고(pci-imx6.c), Qualcomm 은 반대로 세운다(pcie-qcom.c) — 즉 SoC 배선 특성에 따라 반대 방향으로 쓰이는 비트다. */
#define GEN3_RELATED_OFF_EQ_PHASE_2_3		BIT(9)	/* [한국어] 이퀄라이제이션 단계 2/3 를 수행할지. 단계 2/3 는 양쪽이 실제로 계수를 주고받으며 수렴시키는 구간이라 시간이 걸리지만, 건너뛰면 마진이 나빠진다. */
#define GEN3_RELATED_OFF_RXEQ_RGRDLESS_RXTS	BIT(13)	/* [한국어] 수신 훈련 시퀀스(RX TS) 도착 여부와 무관하게 수신 이퀄라이제이션을 수행하도록 강제한다. 상대가 TS 를 규격대로 보내지 않는 경우를 위한 우회다. */
#define GEN3_RELATED_OFF_GEN3_EQ_DISABLE	BIT(16)	/* [한국어] Gen3 이퀄라이제이션 자체를 끈다. 훈련이 수렴하지 않아 링크가 아예 서지 않을 때의 마지막 수단. */
#define GEN3_RELATED_OFF_RATE_SHADOW_SEL_SHIFT	24	/* [한국어] 뱅크 선택 필드의 시프트 값(FIELD_PREP 을 쓰지 않는 코드용). */
#define GEN3_RELATED_OFF_RATE_SHADOW_SEL_MASK	GENMASK(25, 24)	/* [한국어] 뱅크 선택 필드(비트 25:24). 값 0 = 8GT/s, 1 = 16GT/s, 2 = 32GT/s 로, qcom_pcie_common_set_equalization() 이 (speed - PCIE_SPEED_8_0GT) 를 그대로 넣는 것이 그 대응이다. */

#define GEN3_EQ_CONTROL_OFF			0x8A8	/* [한국어] 이퀄라이제이션 알고리즘의 동작 방식을 고르는 레지스터. 위 RATE_SHADOW_SEL 이 가리키는 속도 뱅크에 적용된다. */
#define GEN3_EQ_CONTROL_OFF_FB_MODE		GENMASK(3, 0)	/* [한국어] 피드백 모드(비트 3:0). 계수를 어떤 방식으로 되먹임할지를 고른다 — 예컨대 상대에게 프리셋 번호를 요구할지, 계수 값을 직접 지시할지. */
#define GEN3_EQ_CONTROL_OFF_PHASE23_EXIT_MODE	BIT(4)	/* [한국어] 단계 2/3 를 언제 빠져나올지. 수렴을 기다릴지, 정해진 시간이 지나면 나올지를 정한다. */
#define GEN3_EQ_CONTROL_OFF_PSET_REQ_VEC	GENMASK(23, 8)	/* [한국어] 요청할 프리셋 목록을 담는 16비트 벡터(비트 23:8). 각 비트가 프리셋 번호 하나에 대응해, 훈련 중 시험해 볼 후보 집합을 지정한다. 후보를 줄이면 훈련이 빨라지지만 최적점을 놓칠 수 있다. */
#define GEN3_EQ_CONTROL_OFF_FOM_INC_INITIAL_EVAL	BIT(24)	/* [한국어] FOM(Figure Of Merit — 수신 품질 점수) 을 첫 평가에도 포함할지. 초기 상태를 후보에 넣느냐 마느냐의 차이다. */

#define GEN3_EQ_FB_MODE_DIR_CHANGE_OFF		0x8AC	/* [한국어] FMDC(Feedback Mode Direction Change) — 이퀄라이제이션 탐색이 계수를 어느 방향으로 얼마나 움직일지, 언제 멈출지를 정하는 수렴 파라미터 묶음. */
#define GEN3_EQ_FMDC_T_MIN_PHASE23		GENMASK(4, 0)	/* [한국어] 단계 2/3 에 최소한 머무를 시간(비트 4:0). 너무 짧으면 수렴 전에 나와 마진이 나빠지고, 너무 길면 링크 기동이 느려진다. */
#define GEN3_EQ_FMDC_N_EVALS			GENMASK(9, 5)	/* [한국어] 후보 계수를 몇 번 평가할지(비트 9:5). 평가 횟수를 늘리면 잡음에 덜 흔들리지만 그만큼 오래 걸린다. */
#define GEN3_EQ_FMDC_MAX_PRE_CURSOR_DELTA	GENMASK(13, 10)	/* [한국어] pre-cursor 계수를 한 번에 바꿀 수 있는 최대 폭(비트 13:10). pre-cursor 는 현재 비트보다 **앞선** 비트가 만드는 간섭을 상쇄하는 성분이다. */
#define GEN3_EQ_FMDC_MAX_POST_CURSOR_DELTA	GENMASK(17, 14)	/* [한국어] post-cursor 계수의 최대 변화 폭(비트 17:14). post-cursor 는 현재 비트가 **뒤따르는** 비트에 남기는 잔향을 상쇄한다. 두 delta 를 좁게 잡으면 탐색이 안정적이지만 최적점에 도달하기까지 반복이 늘어난다. */

/* [한국어] 코히런시 제어 레지스터군. 컨트롤러가 시스템 쪽(AXI/ACE)으로
 * 내보내는 트랜잭션에 캐시 코히런시 속성을 붙일지 말지를, **주소 구간별로**
 * 정하게 해 준다. 왜 필요한가 — 어떤 SoC 의 인터커넥트는 주변장치 영역으로
 * 가는 코히런트 트랜잭션을 그냥 버린다. 그런 칩에서 MSI 쓰기가 코히런트로
 * 표시되어 주변장치 영역으로 향하면 인터럽트가 통째로 사라진다.
 * 그래서 "이 주소 아래는 주변장치(비코히런트), 위는 메모리(코히런트)" 라는
 * 경계선을 하드웨어에 박아 둔다. 실제 사례가 pcie-nxp-s32g.c 의
 * s32g_pcie_reset_mstr_ace() 로, 상류 주석에 그 사정이 자세히 적혀 있다. */
#define COHERENCY_CONTROL_1_OFF			0x8E0	/* [한국어] 경계 주소의 하위 32비트를 담는다. */
#define CFG_MEMTYPE_BOUNDARY_LOW_ADDR_MASK	GENMASK(31, 2)	/* [한국어] 경계 주소 하위 부분의 유효 비트(31:2). 하위 2비트가 빠진 것은 주소가 4바이트 정렬이라 항상 0 이기 때문이고, 그 자리를 아래 CFG_MEMTYPE_VALUE 같은 플래그가 쓴다. */
#define CFG_MEMTYPE_VALUE			BIT(0)	/* [한국어] 같은 레지스터 비트 0 에 얹힌 메모리 타입 플래그. 경계 위쪽을 어떤 타입으로 볼지 지정한다. */
#define COHERENCY_CONTROL_2_OFF			0x8E4	/* [한국어] 경계 주소의 상위 32비트. s32g 는 여기에 DDR 시작 주소의 upper_32_bits() 를 쓴다. */
#define COHERENCY_CONTROL_3_OFF			0x8E8	/* [한국어] 추가 코히런시 제어. s32g 는 0 으로 지워 기본 상태로 되돌린 뒤 1/2 를 설정한다. */

#define PCIE_PORT_MULTI_LANE_CTRL	0x8C0	/* [한국어] 다중 레인 제어. dw_pcie_upconfig_setup() 이 이 레지스터 하나만 만진다. */
#define PORT_MLTI_UPCFG_SUPPORT		BIT(7)	/* [한국어] "업컨피그(upconfigure)" 지원 비트. 링크가 처음에 좁은 폭(예: x1)으로 섰더라도 나중에 더 넓은 폭(x4)으로 다시 협상할 수 있게 허용한다. 이 비트가 없으면 한 번 좁게 선 링크는 재훈련을 해도 넓어지지 않는다. */

#define PCIE_VERSION_NUMBER		0x8F8	/* [한국어] IP 버전이 ASCII 로 들어 있는 레지스터. dw_pcie_version_detect() 가 읽어 dw_pcie.version 에 담고, 위 DW_PCIE_VER_* 상수와 비교한다. 값이 0 이면 이 레지스터가 없는 옛 IP 라는 뜻이라 그냥 돌아간다. */
#define PCIE_VERSION_TYPE		0x8FC	/* [한국어] IP "타입" 문자열. version 과 같은 방식으로 dw_pcie.type 에 담긴다. 위 dw_pcie_ver_type_is() 가 쓰라고 만든 값이지만, 앞서 적었듯 그 매크로는 현재 이 트리에서 쓰이지 않는다. */

/*
 * iATU inbound and outbound windows CSRs. Before the IP-core v4.80a each
 * iATU region CSRs had been indirectly accessible by means of the dedicated
 * viewport selector. The iATU/eDMA CSRs space was re-designed in DWC PCIe
 * v4.80a in a way so the viewport was unrolled into the directly accessible
 * iATU/eDMA CSRs space.
 */
/* [한국어] iATU(internal Address Translation Unit)는 이 IP 의 심장이다.
 * CPU 가 보는 주소 공간과 PCIe 버스가 보는 주소 공간은 별개인데,
 * 그 둘을 창(window) 단위로 이어 주는 것이 iATU 다. 방향이 둘이다.
 *   - outbound: CPU/AXI 쪽 주소 -> PCIe 주소. 호스트가 엔드포인트의
 *     BAR 을 읽거나 config 접근을 할 때 쓰는 방향.
 *   - inbound: PCIe 주소 -> CPU/AXI 쪽 주소. 상대가 우리 BAR 에 쓴
 *     트랜잭션을 우리 메모리로 떨어뜨리는 방향.
 * 창 하나마다 base/limit(들어오는 주소 범위), target(나가는 주소),
 * ctrl1/ctrl2(TLP 타입과 활성화)를 프로그램한다.
 *
 * 창 개수는 유한하다(하드웨어가 몇 개를 합성했는지는
 * dw_pcie_iatu_detect() 가 실제로 써 보고 세어 num_ob_windows /
 * num_ib_windows 에 담는다). 그래서 창보다 접근 대상이 많으면 창을
 * 돌려 써야 하고, 특히 버스 번호가 다른 config 접근마다 outbound 창을
 * 다시 프로그램해야 하는 구성에서는 그 재설정 비용이 접근마다 붙는다.
 *
 * 상류 주석이 말하는 두 가지 접근 방식이 핵심이다.
 *   - v4.80a 미만: 창 레지스터가 한 벌뿐이고, PCIE_ATU_VIEWPORT 에
 *     "지금 몇 번 창을 볼 것인지"를 먼저 쓴 다음 그 한 벌을 읽고 쓴다.
 *     선택기를 거치므로 접근이 두 단계이고, 선택 상태가 전역이라
 *     동시 접근에 취약하다.
 *   - v4.80a 이상(unroll): 창마다 독립된 주소가 있어 곧바로 접근한다.
 *     이 경우 PCIE_ATU_VIEWPORT 를 읽으면 0xFFFFFFFF 가 나오는데,
 *     dw_pcie_iatu_detect() 는 바로 그것을 보고 unroll 여부를 판정하고
 *     DW_PCIE_CAP_IATU_UNROLL 비트를 세운다. */
#define PCIE_ATU_VIEWPORT		0x900	/* [한국어] 뷰포트 선택기(v4.80a 미만 전용). 어느 창을 볼지 여기 쓴다. unroll IP 에서는 이 주소에 레지스터가 없어 읽으면 0xFFFFFFFF 가 나오고, 그 성질을 탐지에 이용한다. */
#define PCIE_ATU_REGION_DIR_IB		BIT(31)	/* [한국어] 방향 = inbound. 뷰포트 방식에서는 실제로 이 비트를 선택기에 써서 방향을 고르고, unroll 방식에서는 아래 UNROLL_BASE 매크로의 판별 인자로만 쓰인다. */
#define PCIE_ATU_REGION_DIR_OB		0	/* [한국어] 방향 = outbound. 값이 0 이라 "비트를 세우지 않음"이 곧 outbound 다. */
#define PCIE_ATU_VIEWPORT_BASE		0x904	/* [한국어] 뷰포트 방식에서 선택된 창의 레지스터 한 벌이 시작되는 주소. dw_pcie_iatu_detect() 가 뷰포트 IP 를 만나면 atu_base 를 dbi_base + 이 값으로 잡는다. */
#define PCIE_ATU_UNROLL_BASE(dir, index) \
	(((index) << 9) | ((dir == PCIE_ATU_REGION_DIR_IB) ? BIT(8) : 0))	/* [한국어] unroll 방식에서 (방향, 창 번호)로부터 그 창의 레지스터 블록 오프셋을 계산한다. 창 하나가 0x200(512)바이트를 차지하므로 index 를 9비트 왼쪽으로 밀고, 방향은 그 안에서 비트 8(=0x100)로 구분한다. 즉 outbound 0번은 +0x000, inbound 0번은 +0x100, outbound 1번은 +0x200 식으로 배치된다. dw_pcie_iatu_detect() 가 atu_size / 512 로 창 개수 상한을 잡는 것도 이 512바이트 간격 때문이다. */
#define PCIE_ATU_VIEWPORT_SIZE		0x2C	/* [한국어] 뷰포트 방식에서 창 레지스터 한 벌의 크기(44바이트). 아래 CTRL1(0x00)부터 UPPER_LIMIT(0x20)까지를 덮는다. */
/* [한국어] 아래 오프셋들은 "창 레지스터 한 벌 안에서의" 상대 오프셋이다.
 * 뷰포트 방식이면 PCIE_ATU_VIEWPORT_BASE 기준, unroll 방식이면
 * PCIE_ATU_UNROLL_BASE(dir, index) 기준으로 더해서 쓴다. 그 덧셈은
 * pcie-designware.c 의 dw_pcie_select_atu() 안에 숨어 있어서,
 * 호출자는 dw_pcie_writel_atu_ob(pci, index, PCIE_ATU_LOWER_BASE, ...)
 * 처럼 방향과 번호만 넘기면 된다. */
#define PCIE_ATU_REGION_CTRL1		0x000	/* [한국어] 제어 레지스터 1 — 이 창이 만들어 낼 TLP 의 종류(아래 TYPE_ 값)와 function 번호를 담는다. */
#define PCIE_ATU_INCREASE_REGION_SIZE	BIT(13)	/* [한국어] 창 크기를 4GB 넘게 키울 수 있게 하는 비트. 창의 base 와 limit 이 서로 다른 4GB 구간에 걸칠 때만 필요하고, UPPER_LIMIT 레지스터가 있는 v4.60a 이상에서만 세운다 — dw_pcie_prog_outbound_atu()/prog_inbound_atu() 의 조건이 정확히 그 두 가지다. */
#define PCIE_ATU_TYPE_MEM		0x0	/* [한국어] 메모리 TLP. BAR 접근과 DMA 가 모두 이 타입이다. */
#define PCIE_ATU_TYPE_IO		0x2	/* [한국어] I/O 공간 TLP. 옛 x86 유산이라 요즘 장치는 거의 쓰지 않는다. */
#define PCIE_ATU_TYPE_CFG0		0x4	/* [한국어] Type 0 config TLP — 바로 아래 붙은 장치(루트 포트의 직속 자식)에 대한 config 접근. 브리지를 건너지 않는다. */
#define PCIE_ATU_TYPE_CFG1		0x5	/* [한국어] Type 1 config TLP — 하위 버스에 있는 장치에 대한 config 접근. 중간 브리지가 목적지 버스 번호를 보고 계속 전달하다가, 자기 직속 버스에 닿으면 Type 0 으로 바꿔 내려보낸다. 호스트 코드가 대상 버스 번호에 따라 두 타입을 갈라 쓰는 이유다. */
#define PCIE_ATU_TYPE_MSG		0x10	/* [한국어] 메시지 TLP. 데이터가 없는 제어용 TLP 로, 전원 관리 메시지(PME_Turn_Off 등)를 보낼 때 쓴다. dw_pcie_rp.use_atu_msg 를 켠 SoC 는 전용 창 하나를 이 용도로 예약한다. */
#define PCIE_ATU_TD			BIT(8)	/* [한국어] TD(TLP Digest) 비트 — TLP 끝에 ECRC(종단간 CRC)를 붙일지. dw_pcie_enable_ecrc() 가 특정 IP 버전(490A/500A)에서 이 비트를 손본다. */
#define PCIE_ATU_FUNC_NUM(pf)           ((pf) << 20)	/* [한국어] 이 창이 어느 physical function 소속인지(비트 22:20). 다중 function 엔드포인트에서 창을 function 별로 나눠 쓰기 위한 것이다. FIELD_PREP 대신 직접 시프트를 쓴 옛 스타일 매크로. */
#define PCIE_ATU_REGION_CTRL2		0x004	/* [한국어] 제어 레지스터 2 — 창의 활성화와 매칭 방식을 정한다. iATU 프로그래밍의 마지막 단계로 여기에 ENABLE 을 쓰고, 그다음 이 레지스터를 되읽어 실제로 켜졌는지 확인한다. */
#define PCIE_ATU_ENABLE			BIT(31)	/* [한국어] 창 활성화. 이 비트를 지우는 것이 곧 창을 끄는 것이라, dw_pcie_disable_atu() 는 이 레지스터에 0 을 쓰는 한 줄이 전부다. */
#define PCIE_ATU_BAR_MODE_ENABLE	BIT(30)	/* [한국어] BAR Match Mode. 인바운드 창이 "주소 범위"가 아니라 "BAR 번호"로 매칭하게 한다. 엔드포인트에서 중요한데, 호스트가 BAR 주소를 언제 어디로 배정할지 미리 알 수 없기 때문이다. BAR 번호로 매칭하면 호스트가 무슨 주소를 주든 그 BAR 로 온 트랜잭션이 우리 버퍼로 떨어진다. dw_pcie_prog_ep_inbound_atu() 가 (bar << 8) 과 함께 이 비트를 세운다. */
#define PCIE_ATU_CFG_SHIFT_MODE_ENABLE	BIT(28)	/* [한국어] Config Shift Mode. config 접근 주소의 BDF 부분을 하드웨어가 알아서 시프트해 넣게 한다. ECAM 처럼 주소 안에 버스/장치/함수 번호가 박혀 있는 구성을 창 하나로 처리할 수 있게 해 준다. */
#define PCIE_ATU_INHIBIT_PAYLOAD	BIT(22)	/* [한국어] 페이로드 억제. 메시지 TLP 는 데이터가 없어야 하므로, PCIE_ATU_TYPE_MSG 창을 만들 때 dw_pcie_prog_outbound_atu() 가 이 비트를 함께 세운다. */
#define PCIE_ATU_FUNC_NUM_MATCH_EN      BIT(19)	/* [한국어] 매칭 조건에 function 번호까지 넣는다. CTRL1 의 PCIE_ATU_FUNC_NUM() 과 짝으로 쓰여, 다중 function 엔드포인트에서 다른 function 의 트래픽이 이 창에 걸리지 않게 한다. */
#define PCIE_ATU_LOWER_BASE		0x008	/* [한국어] 창이 받아들일 주소 범위의 시작, 하위 32비트. outbound 면 CPU/부모 버스 주소, inbound(Address Match Mode)면 PCIe 주소다 — 방향에 따라 "들어오는 쪽"이 무엇인지가 뒤바뀐다. */
#define PCIE_ATU_UPPER_BASE		0x00C	/* [한국어] 같은 시작 주소의 상위 32비트. */
#define PCIE_ATU_LIMIT			0x010	/* [한국어] 범위의 끝(포함), 하위 32비트. 코드는 항상 base + size - 1 로 계산한다 — 끝 주소가 포함이기 때문이다. */
#define PCIE_ATU_LOWER_TARGET		0x014	/* [한국어] 변환 결과 주소의 시작, 하위 32비트. 즉 base~limit 에 들어온 트랜잭션을 여기서부터 시작하는 주소로 옮긴다. */
/* [한국어] config 접근용 창을 만들 때 target 주소에 넣는 BDF 인코딩.
 * config TLP 의 목적지는 메모리 주소가 아니라 (버스, 장치, 함수) 삼중항이고,
 * iATU 는 그 삼중항을 target 레지스터의 상위 비트에 실어 보낸다.
 * 그래서 세 매크로를 OR 로 합쳐 PCIE_ATU_LOWER_TARGET 에 쓰게 되어 있다. */
#define PCIE_ATU_BUS(x)			FIELD_PREP(GENMASK(31, 24), x)	/* [한국어] 버스 번호(비트 31:24) — 8비트라 0~255. */
#define PCIE_ATU_DEV(x)			FIELD_PREP(GENMASK(23, 19), x)	/* [한국어] 장치 번호(비트 23:19) — 5비트라 0~31. */
#define PCIE_ATU_FUNC(x)		FIELD_PREP(GENMASK(18, 16), x)	/* [한국어] 함수 번호(비트 18:16) — 3비트라 0~7. 세 필드를 합치면 표준 BDF 배치와 정확히 같다. */
#define PCIE_ATU_UPPER_TARGET		0x018	/* [한국어] 변환 결과 주소의 상위 32비트. */
#define PCIE_ATU_UPPER_LIMIT		0x020	/* [한국어] 범위 끝의 상위 32비트. 이 레지스터는 v4.60a 부터 생겼기 때문에, 코드는 항상 dw_pcie_ver_is_ge(pci, 460A) 로 감싸고 쓴다. 없는 IP 에 쓰면 엉뚱한 레지스터를 건드리게 된다. 주소가 0x01C 가 아니라 0x020 인 점도 그대로 하드웨어 배치다. */

#define PCIE_MISC_CONTROL_1_OFF		0x8BC	/* [한국어] 기타 제어 1. 아래 잠금 해제 비트 하나 때문에 이 헤더에서 가장 자주 등장하는 레지스터다. */
#define PCIE_DBI_RO_WR_EN		BIT(0)	/* [한국어] DBI Read-Only Write Enable — config 공간에서 규격상 읽기 전용인 레지스터(Vendor/Device ID, BAR 마스크, LNKCAP 등)를 드라이버가 쓸 수 있게 잠시 여는 비트. 왜 필요한가 — 그 값들은 "장치가 호스트에게 자기를 소개하는 내용"이라 호스트 쪽에서는 읽기 전용이지만, 장치를 만드는 우리 쪽에서는 반드시 초기화해야 하기 때문이다. 아래 dw_pcie_dbi_ro_wr_en()/dis() 가 이 비트를 세우고 지우는 쌍이며, 열어 둔 채로 두면 호스트의 잘못된 쓰기가 그대로 먹히므로 필요한 구간만 감싸는 것이 규칙이다. */
#define PCIE_MSIX_DOORBELL		0x948	/* [한국어] MSI-X 도어벨. 엔드포인트가 MSI-X 인터럽트를 "메모리 쓰기 TLP 를 직접 만들어" 보내는 대신, 이 레지스터에 벡터 번호를 쓰면 IP 가 알아서 TLP 를 생성해 주는 지름길이다. outbound iATU 창을 잡을 필요가 없어 훨씬 싸다. dw_pcie_ep_raise_msix_irq_doorbell() 이 이 방식을 쓰며, 이 트리에서 그것을 부르는 곳은 pci-layerscape-ep.c 하나다. */
#define PCIE_MSIX_DOORBELL_PF_SHIFT	24	/* [한국어] 도어벨 값에서 physical function 번호가 놓이는 자리(비트 31:24). dw_pcie_ep_raise_msix_irq_doorbell() 이 (func_no << 24) | (interrupt_num - 1) 로 조립한다 — 하위 비트가 0-기반 벡터 번호이고, EPC 규약의 interrupt_num 이 1-기반이라 1 을 뺀다. */

/*
 * eDMA CSRs. DW PCIe IP-core v4.70a and older had the eDMA registers accessible
 * over the Port Logic registers space. Afterwards the unrolled mapping was
 * introduced so eDMA and iATU could be accessed via a dedicated registers
 * space.
 */
/* [한국어] eDMA 는 컨트롤러 안에 들어 있는 DMA 엔진이다. 이것이 있으면
 * 큰 데이터를 옮길 때 CPU 가 직접 복사하지 않고 컨트롤러에게 맡길 수 있다.
 * iATU 와 똑같은 역사를 밟았다 — 옛 IP(v4.70a 이하)는 포트 로직 공간
 * 안의 뷰포트를 거쳐 접근했고, 이후 별도 주소 공간으로 분리(unroll)되었다.
 * dw_pcie_edma_detect() 가 채널 수를 세어 dmaengine 에 등록한다. */
#define PCIE_DMA_VIEWPORT_BASE		0x970	/* [한국어] 뷰포트 방식 eDMA 레지스터의 시작 주소(포트 로직 공간 안). */
#define PCIE_DMA_UNROLL_BASE		0x80000	/* [한국어] unroll 방식 eDMA 레지스터 공간의 시작 오프셋(DBI 기준 512KB 지점). 아래 DEFAULT_DBI_DMA_OFFSET 이 이 값을 그대로 재사용한다. */
#define PCIE_DMA_CTRL			0x008	/* [한국어] eDMA 제어/능력 레지스터. 합성된 채널 수가 여기 들어 있어, 드라이버가 읽어서 몇 개를 등록할지 정한다. */
#define PCIE_DMA_NUM_WR_CHAN		GENMASK(3, 0)	/* [한국어] 쓰기 채널 개수(비트 3:0). 쓰기 = 로컬 메모리에서 PCIe 쪽으로 보내는 방향. */
#define PCIE_DMA_NUM_RD_CHAN		GENMASK(19, 16)	/* [한국어] 읽기 채널 개수(비트 19:16). 읽기 = PCIe 쪽에서 로컬 메모리로 가져오는 방향. 읽기와 쓰기 채널이 따로 합성되므로 개수가 다를 수 있다. */

/* [한국어] CDM(Configuration Dependent Module) 레지스터 무결성 검사.
 * 컨트롤러의 config 레지스터가 방사선이나 전기적 결함으로 조용히
 * 뒤집히는 일을 잡아내기 위한 안전 기능이다. 하드웨어가 레지스터
 * 내용을 참조본과 비교하며 순회하고, 어긋나면 오류 비트와 함께
 * 문제가 난 주소를 알려 준다. DW_PCIE_CAP_CDM_CHECK 성질이 있는
 * SoC 에서만 dw_pcie_setup() 이 이 검사를 켠다. */
#define PCIE_PL_CHK_REG_CONTROL_STATUS			0xB20	/* [한국어] 검사 제어/상태 레지스터. 하위 비트로 시작을 지시하고 상위 비트로 결과를 읽는다. */
#define PCIE_PL_CHK_REG_CHK_REG_START			BIT(0)	/* [한국어] 검사 한 회 시작. */
#define PCIE_PL_CHK_REG_CHK_REG_CONTINUOUS		BIT(1)	/* [한국어] 한 번으로 끝내지 않고 계속 반복해서 검사한다. dw_pcie_setup() 은 START 와 이 비트를 함께 세워, 켜 두면 이후로 하드웨어가 알아서 감시하게 만든다. */
#define PCIE_PL_CHK_REG_CHK_REG_COMPARISON_ERROR	BIT(16)	/* [한국어] 비교 결과 불일치 — 레지스터 값이 실제로 손상됐다는 뜻. */
#define PCIE_PL_CHK_REG_CHK_REG_LOGIC_ERROR		BIT(17)	/* [한국어] 검사 로직 자체의 오류 — 값이 아니라 검사 회로 쪽 문제. */
#define PCIE_PL_CHK_REG_CHK_REG_COMPLETE		BIT(18)	/* [한국어] 한 바퀴 순회가 끝났음. */
#define PCIE_PL_CHK_REG_ERR_ADDR			0xB28	/* [한국어] 불일치가 발견된 레지스터의 주소. COMPARISON_ERROR 가 섰을 때만 의미가 있다. */

/*
 * 16.0 GT/s (Gen 4) lane margining register definitions
 */
/* [한국어] 레인 마진 측정(lane margining)은 Gen4(16GT/s)부터 규격이
 * 요구하는 기능이다. 링크를 끊지 않은 채로 수신기의 판정 지점을
 * 전압 축과 시간 축으로 조금씩 밀어 보면서, 어디까지 밀어야 오류가
 * 나기 시작하는지를 재는 것이다. 그 여유분이 곧 신호 품질의 마진이라,
 * 보드 설계 검증과 현장 진단에 쓰인다.
 * 아래 두 레지스터는 "우리 수신기가 어떤 범위를 어떤 해상도로
 * 지원한다"고 상대에게 광고하는 값이며, 실제 측정 절차는 규격 정의
 * 레지스터로 따로 진행된다. 이 트리에서 이 값들을 설정하는 곳은
 * pcie-qcom-common.c 하나다. */
#define GEN4_LANE_MARGINING_1_OFF		0xB80	/* [한국어] 마진 능력 광고 레지스터 1 — 측정 범위와 단계 수. */
#define MARGINING_MAX_VOLTAGE_OFFSET		GENMASK(29, 24)	/* [한국어] 전압 축으로 밀 수 있는 최대 오프셋(비트 29:24). 단위는 규격이 정한 눈금이다. */
#define MARGINING_NUM_VOLTAGE_STEPS		GENMASK(22, 16)	/* [한국어] 전압 축을 몇 단계로 나누는지(비트 22:16). 단계가 많을수록 해상도가 높지만 측정이 오래 걸린다. */
#define MARGINING_MAX_TIMING_OFFSET		GENMASK(13, 8)	/* [한국어] 시간 축으로 밀 수 있는 최대 오프셋(비트 13:8) — 비트 주기(UI)의 비율로 표현된다. */
#define MARGINING_NUM_TIMING_STEPS		GENMASK(5, 0)	/* [한국어] 시간 축 단계 수(비트 5:0). */

#define GEN4_LANE_MARGINING_2_OFF		0xB84	/* [한국어] 마진 능력 광고 레지스터 2 — 어떤 방식의 측정을 지원하는지. */
#define MARGINING_IND_ERROR_SAMPLER		BIT(28)	/* [한국어] 독립적인 오류 샘플러가 있는지. 있으면 실제 데이터 수신을 방해하지 않고 별도 회로로 마진을 잴 수 있어, 링크를 살린 채 측정할 수 있다. */
#define MARGINING_SAMPLE_REPORTING_METHOD	BIT(27)	/* [한국어] 결과를 오류 개수로 보고할지, 샘플 개수로 보고할지. */
#define MARGINING_IND_LEFT_RIGHT_TIMING		BIT(26)	/* [한국어] 시간 축의 왼쪽(이르게)과 오른쪽(늦게)을 독립적으로 잴 수 있는지. 못 하면 양쪽 대칭으로만 측정된다. */
#define MARGINING_IND_UP_DOWN_VOLTAGE		BIT(25)	/* [한국어] 전압 축의 위/아래를 독립적으로 잴 수 있는지. */
#define MARGINING_VOLTAGE_SUPPORTED		BIT(24)	/* [한국어] 전압 축 마진 측정 자체를 지원하는지. 시간 축은 필수지만 전압 축은 선택이라 별도 비트가 있다. */
#define MARGINING_MAXLANES			GENMASK(20, 16)	/* [한국어] 동시에 마진을 잴 수 있는 최대 레인 수(비트 20:16). pcie-qcom-common.c 는 여기에 pci->num_lanes 를 그대로 넣는다. */
#define MARGINING_SAMPLE_RATE_TIMING		GENMASK(13, 8)	/* [한국어] 시간 축 측정의 샘플링 비율(비트 13:8). 값이 클수록 더 많은 비트를 관찰해 통계적으로 정확해진다. */
#define MARGINING_SAMPLE_RATE_VOLTAGE		GENMASK(5, 0)	/* [한국어] 전압 축 측정의 샘플링 비율(비트 5:0). */
/*
 * iATU Unroll-specific register definitions
 * From 4.80 core version the address translation will be made by unroll
 */
/* [한국어] unroll 방식 iATU 창 안에서의 레지스터 오프셋들.
 * 값을 보면 알 수 있듯 위쪽 PCIE_ATU_REGION_CTRL1 계열과 배치가
 * 사실상 같다(CTRL1=0x00, CTRL2=0x04, LOWER_BASE=0x08 ...).
 * 확인 사항 — 이 PCIE_ATU_UNR_ 접두 상수들을 참조하는 코드는
 * 이 트리 어디에도 없다(grep 결과 이 헤더의 정의 8줄이 전부).
 * 현재 코드는 unroll 여부와 무관하게 위쪽 PCIE_ATU_REGION_CTRL1
 * 계열 상수를 쓰고, 베이스 주소 계산만 dw_pcie_select_atu() 안에서
 * 갈라 처리한다. 즉 이 블록은 그 통합 이전에 쓰이던 이름들이
 * 남아 있는 것이다. */
#define PCIE_ATU_UNR_REGION_CTRL1	0x00	/* [한국어] 제어 1 — 위 PCIE_ATU_REGION_CTRL1 과 같은 자리. (현재 미사용) */
#define PCIE_ATU_UNR_REGION_CTRL2	0x04	/* [한국어] 제어 2 — 활성화 비트가 있는 곳. (현재 미사용) */
#define PCIE_ATU_UNR_LOWER_BASE		0x08	/* [한국어] 창 시작 주소 하위 32비트. (현재 미사용) */
#define PCIE_ATU_UNR_UPPER_BASE		0x0C	/* [한국어] 창 시작 주소 상위 32비트. (현재 미사용) */
#define PCIE_ATU_UNR_LOWER_LIMIT	0x10	/* [한국어] 창 끝 주소 하위 32비트. 위쪽 블록에서는 같은 자리를 PCIE_ATU_LIMIT 이라 부른다. (현재 미사용) */
#define PCIE_ATU_UNR_LOWER_TARGET	0x14	/* [한국어] 변환 목적지 하위 32비트. (현재 미사용) */
#define PCIE_ATU_UNR_UPPER_TARGET	0x18	/* [한국어] 변환 목적지 상위 32비트. (현재 미사용) */
#define PCIE_ATU_UNR_UPPER_LIMIT	0x20	/* [한국어] 창 끝 주소 상위 32비트. 0x1C 를 건너뛴 것도 하드웨어 배치 그대로다. (현재 미사용) */

/*
 * RAS-DES register definitions
 */
/* [한국어] RAS-DES(Reliability, Availability, Serviceability -
 * Data Exchange and Statistics)는 Synopsys 가 벤더 전용 확장
 * capability 로 제공하는 진단 블록이다. 링크에서 일어난 일을 종류별로
 * 세는 이벤트 카운터와, 일부러 오류를 만들어 넣는 오류 주입기를 갖는다.
 * 중요한 점 — 여기 오프셋들은 DBI 절대 주소가 아니라 **capability 시작
 * 위치로부터의 상대 오프셋**이다. 그 시작 위치는 런타임에
 * dw_pcie_find_rasdes_capability() 로 찾아야 한다(SoC 마다 다르다).
 *
 * 카운터는 "그룹 번호 + 이벤트 번호"로 하나를 고른 뒤 값을 읽는
 * 간접 방식이라, 고르는 동작과 읽는 동작 사이에 다른 스레드가 끼어들면
 * 엉뚱한 카운터를 읽게 된다. 그래서 pcie-designware-debugfs.c 는
 * 그 두 단계를 reg_event_lock 뮤텍스로 묶는다.
 *
 * 참고 — pcie-designware-debugfs.c 는 같은 레지스터를 자기 파일 안에
 * RAS_DES_EVENT_COUNTER_CTRL_REG(0x8) / _DATA_REG(0xc) 라는 이름으로
 * 다시 정의해 쓴다. 아래 이름들을 쓰는 곳은 pcie-tegra194.c 다. */
#define PCIE_RAS_DES_EVENT_COUNTER_CONTROL	0x8	/* [한국어] 카운터 제어 레지스터(capability 기준 +0x8). 어느 카운터를 볼지 고르고 켜고 끄는 곳. */
#define EVENT_COUNTER_ALL_CLEAR		0x3	/* [한국어] 활성화 필드에 넣으면 모든 카운터를 0 으로 지운다. 측정 시작 전에 쓴다. */
#define EVENT_COUNTER_ENABLE_ALL	0x7	/* [한국어] 활성화 필드에 넣으면 모든 카운터를 센다. */
#define EVENT_COUNTER_ENABLE_SHIFT	2	/* [한국어] 위 두 값이 놓이는 자리(비트 4:2). pcie-tegra194.c 가 val |= EVENT_COUNTER_ENABLE_ALL << EVENT_COUNTER_ENABLE_SHIFT 로 조립한다. */
#define EVENT_COUNTER_EVENT_SEL_MASK	GENMASK(7, 0)	/* [한국어] 이벤트 번호의 값 범위 마스크(8비트). 시프트 전 값에 적용하는 마스크라 GENMASK(7,0) 이다. */
#define EVENT_COUNTER_EVENT_SEL_SHIFT	16	/* [한국어] 이벤트 번호가 레지스터에서 놓이는 자리(비트 23:16). */
#define EVENT_COUNTER_EVENT_Tx_L0S	0x2	/* [한국어] 그룹 5 의 이벤트 2 — 송신 L0s 진입 횟수. */
#define EVENT_COUNTER_EVENT_Rx_L0S	0x3	/* [한국어] 그룹 5 의 이벤트 3 — 수신 L0s 진입 횟수. */
#define EVENT_COUNTER_EVENT_L1		0x5	/* [한국어] 그룹 5 의 이벤트 5 — L1 진입 횟수. */
#define EVENT_COUNTER_EVENT_L1_1	0x7	/* [한국어] 그룹 5 의 이벤트 7 — L1.1 substate 진입 횟수. */
#define EVENT_COUNTER_EVENT_L1_2	0x8	/* [한국어] 그룹 5 의 이벤트 8 — L1.2 substate 진입 횟수. 이 다섯 이벤트가 모두 저전력 상태 관련인 것은, 이 상수들을 쓰는 pcie-tegra194.c 가 ASPM 동작을 확인하려고 이 그룹만 보기 때문이다. */
#define EVENT_COUNTER_GROUP_SEL_SHIFT	24	/* [한국어] 그룹 번호가 놓이는 자리(비트 27:24). */
#define EVENT_COUNTER_GROUP_5		0x5	/* [한국어] 그룹 5 = 전력 상태 전이 이벤트 묶음. 위 이벤트 번호들이 모두 이 그룹 소속이다. */
#define PCIE_RAS_DES_EVENT_COUNTER_DATA		0xc	/* [한국어] 고른 카운터의 현재 값(capability 기준 +0xc). 제어 레지스터로 선택한 뒤 여기를 읽는 2단계 절차라, 그 사이가 원자적이어야 한다. */

/* PTM register definitions */
/* [한국어] PTM(Precision Time Measurement)은 링크 양쪽의 시계를
 * 나노초 수준으로 맞추기 위한 PCIe 규격 기능이다. 원리는 왕복 시간
 * 측정이다 — 요청자(requester)가 t1 에 요청을 보내고, 응답자
 * (responder)가 t2 에 받아 t3 에 답하고, 요청자가 t4 에 받는다.
 * 네 시각을 알면 전파 지연과 시계 차이를 분리해 낼 수 있다.
 * 역할에 따라 볼 수 있는 값이 다르다는 점이 중요하다 — 요청자는
 * 자기가 잰 t1/t4 와 상대 시계(master)를 알고, 응답자는 t2/t3 를 안다.
 * pcie-designware-debugfs.c 의 *_visible() 콜백들이 pci->mode 가
 * EP 인지 RC 인지에 따라 debugfs 파일을 골라 만드는 것이 바로 이
 * 역할 구분을 반영한 것이다.
 * 아래 오프셋 역시 절대 주소가 아니라 PTM 벤더 확장 capability
 * 시작점(dw_pcie.ptm_vsec_offset) 기준 상대 오프셋이다. */
#define PTM_RES_REQ_CTRL		0x8	/* [한국어] 응답자(RES)와 요청자(REQ)가 함께 쓰는 제어 레지스터. 이름이 둘을 붙여 놓은 것도 그래서다. 아래 세 비트가 이 한 레지스터에 얹혀 있다. */
#define PTM_RES_CCONTEXT_VALID		BIT(0)	/* [한국어] 응답자 관점: 내가 들고 있는 시각 컨텍스트가 유효한가. RC 모드에서만 노출된다(dw_pcie_ptm_context_valid_visible() 가 DW_PCIE_RC_TYPE 을 요구한다). */
#define PTM_REQ_AUTO_UPDATE_ENABLED	BIT(0)	/* [한국어] 요청자 관점: 갱신을 하드웨어가 주기적으로 자동 수행할지. 주의 — 위 RES_CCONTEXT_VALID 와 **같은 비트 0** 이다. 모순이 아니라 역할에 따라 같은 비트가 다른 의미를 갖는 것이며, 그래서 두 이름 모두 BIT(0) 이다. */
#define PTM_REQ_START_UPDATE		BIT(1)	/* [한국어] 요청자 관점: 지금 한 번 갱신하라. 자기 지움 비트라, 읽으면 이미 0 이다. dw_pcie_ptm_context_update_read() 가 "AUTO 가 꺼져 있으면 MANUAL 이다"라고 판단하는 근거가 이것이다 — START 비트로는 모드를 되읽을 수 없기 때문이다. */
#define PTM_LOCAL_LSB			0x10	/* [한국어] 로컬 시계 값의 하위 32비트. 64비트 값을 32비트 레지스터 둘로 나눠 읽어야 해서, 읽는 도중 상위가 넘어가면 값이 찢어진다. dw_pcie_ptm_local_clock_read() 가 MSB 를 두 번 읽어 같을 때까지 반복하는 이유다. */
#define PTM_LOCAL_MSB			0x14	/* [한국어] 로컬 시계 값의 상위 32비트. */
#define PTM_T1_T2_LSB			0x18	/* [한국어] t1/t2 시각의 하위 32비트. 하나의 레지스터 쌍을 역할에 따라 t1(요청자) 또는 t2(응답자)로 읽는다 — dw_pcie_ptm_t1_read() 와 dw_pcie_ptm_t2_read() 의 본문이 완전히 같고 노출 조건(EP/RC)만 다른 것이 그 증거다. */
#define PTM_T1_T2_MSB			0x1c	/* [한국어] t1/t2 시각의 상위 32비트. */
#define PTM_T3_T4_LSB			0x28	/* [한국어] t3/t4 시각의 하위 32비트. 위와 같은 방식으로 t3(응답자)/t4(요청자)를 겸한다. 0x20 이 아니라 0x28 인 것은 사이에 다른 레지스터가 있기 때문이다. */
#define PTM_T3_T4_MSB			0x2c	/* [한국어] t3/t4 시각의 상위 32비트. */
#define PTM_MASTER_LSB			0x38	/* [한국어] 상대(master) 시계 값의 하위 32비트. 요청자 쪽에서만 의미가 있어 EP 모드에서만 노출된다. */
#define PTM_MASTER_MSB			0x3c	/* [한국어] 상대 시계 값의 상위 32비트. */

/*
 * The default address offset between dbi_base and atu_base. Root controller
 * drivers are not required to initialize atu_base if the offset matches this
 * default; the driver core automatically derives atu_base from dbi_base using
 * this offset, if atu_base not set.
 */
#define DEFAULT_DBI_ATU_OFFSET (0x3 << 20)	/* [한국어] DBI 베이스에서 iATU 레지스터 공간까지의 기본 거리 = 3MB(0x300000). 상류 주석대로, 디바이스트리가 atu 영역을 따로 알려 주지 않으면 dw_pcie_get_resources() 가 dbi_base 에 이 값을 더해 atu_base 를 만든다. SoC 대부분이 이 배치를 그대로 쓰기 때문에 기본값으로 삼을 수 있다. */
#define DEFAULT_DBI_DMA_OFFSET PCIE_DMA_UNROLL_BASE	/* [한국어] 같은 방식으로 eDMA 레지스터까지의 기본 거리 = 0x80000(512KB). 위에서 정의한 unroll eDMA 베이스를 그대로 재사용하므로, 두 값이 항상 같이 움직인다. */

/* [한국어] IP 내장 MSI 수신기의 규모를 정하는 상수들(호스트 모드 전용).
 * 구조를 알면 값들이 서로 어떻게 묶이는지 보인다 — 하드웨어는 벡터를
 * 32개씩 묶어 "컨트롤 블록" 단위로 관리하고, 블록마다 ENABLE/MASK/STATUS
 * 세 레지스터를 갖는다. 그래서 벡터 번호 하나는
 * (블록 = 번호 / 32, 비트 = 번호 % 32) 로 분해된다.
 * pcie-designware-host.c 의 MSI 경로 전체가 이 분해를 바탕에 깔고 있다. */
#define MAX_MSI_IRQS			256	/* [한국어] 지원하는 MSI 벡터의 절대 상한. dw_pcie_rp.msi_irq_in_use 비트맵의 크기를 이 값으로 선언하므로, 구조체 크기가 여기 묶여 있다. */
#define MAX_MSI_IRQS_PER_CTRL		32	/* [한국어] 컨트롤 블록 하나가 담당하는 벡터 수. 레지스터가 32비트라 자연스럽게 32 다. */
#define MAX_MSI_CTRLS			(MAX_MSI_IRQS / MAX_MSI_IRQS_PER_CTRL)	/* [한국어] 필요한 블록 수 = 256 / 32 = 8. dw_pcie_rp 의 msi_irq[] 와 irq_mask[] 배열 길이가 이 값이다. */
#define MSI_REG_CTRL_BLOCK_SIZE		12	/* [한국어] 블록 하나가 차지하는 레지스터 공간(12바이트 = 4바이트짜리 ENABLE/MASK/STATUS 셋). 그래서 블록 n 의 STATUS 주소는 PCIE_MSI_INTR0_STATUS + n * 12 로 구한다. */
#define MSI_DEF_NUM_VECTORS		32	/* [한국어] 디바이스트리가 벡터 수를 말해 주지 않을 때 쓰는 기본값. 블록 하나 분량이며, dw_pcie_rp.num_vectors 가 0 이면 호스트 초기화 코드가 이 값을 넣는다. */

/* Maximum number of inbound/outbound iATUs */
/* [한국어] iATU 창 개수의 상한. 실제 개수는 하드웨어마다 다르고
 * dw_pcie_iatu_detect() 가 창에 시험 값을 써 보고 되읽어 세어서
 * num_ib_windows / num_ob_windows 에 담는다. 이 상수는 그 탐색의
 * 상한선 역할이다(unroll IP 에서는 atu_size / 512 와 256 중 작은 쪽을
 * 쓴다 — 512 는 창 하나의 레지스터 간격이다). */
#define MAX_IATU_IN			256	/* [한국어] 인바운드 창 개수 상한. */
#define MAX_IATU_OUT			256	/* [한국어] 아웃바운드 창 개수 상한. */

/* Default eDMA LLP memory size */
/* [한국어] eDMA 의 LLP(Linked List Pointer) 서술자를 담을 메모리 크기.
 * eDMA 는 전송할 조각들을 연결 리스트로 받아 순서대로 처리하는데,
 * 그 리스트가 놓일 공간을 한 페이지로 잡는다는 뜻이다. */
#define DMA_LLP_MEM_SIZE		PAGE_SIZE	/* [한국어] 한 페이지(아키텍처에 따라 보통 4KB). 페이지 단위라 DMA 매핑이 단순해진다. */

/* Common struct pci_epc_feature bits among DWC EP glue drivers */
/* [한국어] DWC 기반 엔드포인트 드라이버가 struct pci_epc_features 를
 * 채울 때 공통으로 들어가는 두 항목을 한 덩어리로 묶은 매크로다.
 * SoC 접착 드라이버는 자기 features 구조체 초기화식 안에 이 매크로를
 * 한 줄 적고 나머지 항목만 채우면 된다(이 트리에서 12개 파일이 쓴다).
 * 두 항목 모두 pcie-designware-ep.c 가 실제로 그 능력을 구현하기
 * 때문에 켤 수 있는 것이다.
 *   - dynamic_inbound_mapping: BAR 의 물리 주소를 나중에 바꿔 달 수
 *     있다는 뜻. dw_pcie_ep_set_bar() 가 이미 설정된 BAR 에 대해
 *     BAR 레지스터를 건드리지 않고 인바운드 iATU 만 다시 거는 경로를
 *     갖고 있어서 가능하다(호스트가 배정한 BAR 주소를 지우면 안 되므로).
 *   - subrange_mapping: BAR 하나를 여러 조각으로 쪼개, 조각마다 다른
 *     로컬 물리 주소로 보낼 수 있다는 뜻. dw_pcie_ep_ib_atu_addr() 가
 *     조각마다 인바운드 창을 하나씩 잡아 구현한다. */
#define DWC_EPC_COMMON_FEATURES		.dynamic_inbound_mapping = true, \
					.subrange_mapping = true

/* [한국어] 전방 선언 셋. 아래 콜백 테이블들이 이 세 타입의 포인터를
 * 인자로 받는데, 정작 구조체 본체는 그 콜백 테이블을 필드로 갖는다.
 * 서로를 참조하는 순환이라 어느 한쪽을 먼저 완전히 정의할 수 없어,
 * 포인터로만 쓰는 단계에서는 불완전 타입으로 통과시키는 것이다. */
struct dw_pcie;		/* [한국어] 컨트롤러 하나 전체. 아래에서 정의된다. */
struct dw_pcie_rp;	/* [한국어] 루트 포트(호스트) 측 상태. */
struct dw_pcie_ep;	/* [한국어] 엔드포인트 측 상태. */

/* [한국어] 이 컨트롤러 인스턴스가 어떤 역할로 동작하는지.
 * 같은 IP 가 루트 컴플렉스로도, 엔드포인트로도 합성될 수 있어서
 * 런타임에 구분이 필요하다. dw_pcie.mode 에 담기며,
 * dwc_pcie_debugfs_init(pci, mode) 가 인자로 받아 저장한다.
 * 실제 소비처는 pcie-designware-debugfs.c 의 PTM *_visible()
 * 콜백들로, 역할에 따라 어떤 시각(t1~t4)을 노출할지 가른다. */
enum dw_pcie_device_mode {
	DW_PCIE_UNKNOWN_TYPE,
	/* [한국어] 아직 정해지지 않음(값 0).
	 * 설정자: 구조체를 kzalloc 으로 잡으면 자동으로 이 값이 된다.
	 * 읽는 자: mode 를 보는 코드는 이 값을 EP 도 RC 도 아닌 것으로 취급한다.
	 * 값 범위: 0.
	 * 동기화: 초기화 시점에만 바뀌므로 별도 보호가 없다. */

	DW_PCIE_EP_TYPE,
	/* [한국어] 엔드포인트(장치 쪽) 역할.
	 * 설정자: dw_pcie_ep_init_registers() 가 마지막에
	 *   dwc_pcie_debugfs_init(pci, DW_PCIE_EP_TYPE) 을 부르면서 넘긴다.
	 * 읽는 자: dw_pcie_ptm_context_update_visible(),
	 *   dw_pcie_ptm_master_clock_visible(), dw_pcie_ptm_t1_visible(),
	 *   dw_pcie_ptm_t4_visible() — 즉 PTM 요청자 역할에 해당하는 값들.
	 * 값 범위: 1.
	 * 동기화: 프로브 경로에서 한 번 쓰고 이후 읽기 전용. */

	DW_PCIE_LEG_EP_TYPE,
	/* [한국어] 레거시 엔드포인트 역할.
	 * 설정자/읽는 자: 이 트리에서 이 값을 쓰는 코드를 찾지 못했다.
	 *   enum 자리를 유지하기 위해 남아 있는 것으로 보인다.
	 * 값 범위: 2.
	 * 동기화: 해당 없음. */

	DW_PCIE_RC_TYPE,
	/* [한국어] 루트 컴플렉스(호스트 쪽) 역할.
	 * 설정자: pcie-designware-host.c 가
	 *   dwc_pcie_debugfs_init(pci, DW_PCIE_RC_TYPE) 으로 넘긴다.
	 * 읽는 자: dw_pcie_ptm_context_valid_visible(),
	 *   dw_pcie_ptm_t2_visible(), dw_pcie_ptm_t3_visible() —
	 *   PTM 응답자 역할에 해당하는 값들.
	 * 값 범위: 3.
	 * 동기화: 프로브 경로에서 한 번 쓰고 이후 읽기 전용. */
};

/* [한국어] "애플리케이션" 측 클럭 — 컨트롤러가 SoC 내부 버스(AXI/AHB)에
 * 붙는 쪽의 클럭들이다. PCIe 링크 자체가 아니라 CPU 와 컨트롤러 사이
 * 통신에 필요한 클럭이라는 점에서 아래 core clk 와 갈린다.
 * 이 enum 값들은 dw_pcie.app_clks[] 배열의 **인덱스**이며, 같은 인덱스로
 * pcie-designware.c 의 dw_pcie_app_clks[] 문자열 표에서 디바이스트리
 * 클럭 이름을 꺼내 온다. 그래서 두 배열의 순서가 어긋나면 엉뚱한 클럭을
 * 켜게 되고, 그것을 막으려고 문자열 표는 [DW_PCIE_DBI_CLK] = "dbi" 처럼
 * 지정 초기화자를 쓴다. 전부 선택 사항이라 없는 클럭은 그냥 건너뛴다. */
enum dw_pcie_app_clk {
	DW_PCIE_DBI_CLK,
	/* [한국어] DBI 인터페이스 클럭(디바이스트리 이름 "dbi").
	 * 설정자: dw_pcie_get_resources() 가 app_clks[이 인덱스].id 에
	 *   문자열을 넣고 clk_bulk 로 한꺼번에 받아 온다.
	 * 읽는 자: 클럭을 켜고 끄는 dw_pcie_ 코어 코드.
	 * 값 범위: 0.
	 * 동기화: 프로브/PM 경로에서만 다루므로 별도 락이 없다.
	 * 의미: config 레지스터를 읽고 쓰려면 이 클럭이 살아 있어야 한다.
	 *   즉 이것이 꺼져 있으면 DBI 접근 자체가 불가능하다. */

	DW_PCIE_MSTR_CLK,
	/* [한국어] 마스터 포트 클럭("mstr").
	 * 값 범위: 1.
	 * 의미: 컨트롤러가 **주인 노릇을 하며** 시스템 메모리를 읽고 쓸 때
	 *   쓰는 경로의 클럭이다. 즉 상대 장치의 DMA 가 우리 메모리로
	 *   떨어지는 길이 이 클럭 위에 있다.
	 * 나머지 설정자/읽는 자/동기화는 DBI_CLK 와 같다. */

	DW_PCIE_SLV_CLK,
	/* [한국어] 슬레이브 포트 클럭("slv").
	 * 값 범위: 2.
	 * 의미: CPU 가 주인이 되어 컨트롤러에 접근할 때의 경로다.
	 *   MMIO 와 config 접근이 이 길로 지나간다.
	 * 나머지는 위와 같다. */

	DW_PCIE_NUM_APP_CLKS
	/* [한국어] 개수 표지(값 3). 실제 클럭이 아니라 배열 길이로만 쓴다 —
	 * dw_pcie.app_clks[DW_PCIE_NUM_APP_CLKS] 선언과 순회 루프의 상한이
	 * 모두 이 값이라, 항목을 추가해도 배열과 루프가 자동으로 따라온다. */
};

/* [한국어] "코어" 측 클럭 — PCIe 링크와 PHY 를 실제로 돌리는 클럭들.
 * 위 app clk 와 같은 방식으로 dw_pcie.core_clks[] 의 인덱스이며,
 * pcie-designware.c 의 dw_pcie_core_clks[] 문자열 표와 짝을 이룬다. */
enum dw_pcie_core_clk {
	DW_PCIE_PIPE_CLK,
	/* [한국어] PIPE 인터페이스 클럭("pipe").
	 * 값 범위: 0.
	 * 의미: PIPE(PHY Interface for PCI Express)는 컨트롤러와 PHY 사이의
	 *   표준 인터페이스이고, 이것은 그 구간의 클럭이다.
	 * 설정자/읽는 자/동기화: app clk 와 동일한 clk_bulk 경로. */

	DW_PCIE_CORE_CLK,
	/* [한국어] 컨트롤러 코어 클럭("core").
	 * 값 범위: 1.
	 * 의미: 컨트롤러 내부 로직 전반을 돌린다. */

	DW_PCIE_AUX_CLK,
	/* [한국어] 보조 클럭("aux").
	 * 값 범위: 2.
	 * 의미: 주 클럭이 꺼진 저전력 상태(L1.1/L1.2 같은 substate)에서도
	 *   최소한의 상태를 유지하는 데 쓰인다. 그래서 이 클럭의 유무가
	 *   ASPM L1 substate 지원 여부와 맞물린다. */

	DW_PCIE_REF_CLK,
	/* [한국어] 기준 클럭("ref").
	 * 값 범위: 3.
	 * 의미: PCIe 링크의 기준 주파수(100MHz)를 공급한다. 링크 양쪽이
	 *   같은 기준으로 동작해야 하므로 링크 훈련 전에 안정되어 있어야 한다. */

	DW_PCIE_NUM_CORE_CLKS
	/* [한국어] 개수 표지(값 4). core_clks[] 배열 길이로만 쓴다. */
};

/* [한국어] 애플리케이션 측 리셋 라인. 클럭과 정확히 같은 짜임이다 —
 * dw_pcie.app_rsts[] 의 인덱스이고, pcie-designware.c 의
 * dw_pcie_app_rsts[] 문자열 표와 짝지어 reset_control_bulk API 로
 * 한꺼번에 확보한다. 클럭 이름과 리셋 이름이 같은 것("dbi", "mstr",
 * "slv")은 우연이 아니라 같은 하위 블록을 가리키기 때문이다. */
enum dw_pcie_app_rst {
	DW_PCIE_DBI_RST,
	/* [한국어] DBI 블록 리셋("dbi").
	 * 설정자: dw_pcie_get_resources() 가 이름을 채우고 bulk 로 받는다.
	 * 읽는 자: 리셋을 걸고 푸는 dw_pcie_ 코어 코드.
	 * 값 범위: 0.
	 * 동기화: 프로브/PM 경로 전용이라 별도 락이 없다. */

	DW_PCIE_MSTR_RST,
	/* [한국어] 마스터 포트 리셋("mstr"). 값 범위 1. 나머지는 위와 같다. */

	DW_PCIE_SLV_RST,
	/* [한국어] 슬레이브 포트 리셋("slv"). 값 범위 2. 나머지는 위와 같다. */

	DW_PCIE_NUM_APP_RSTS
	/* [한국어] 개수 표지(값 3). app_rsts[] 배열 길이로만 쓴다. */
};

/* [한국어] 코어 측 리셋 라인. PCIe 는 리셋의 "깊이"가 여러 단계라
 * 종류가 많다. 핵심 구분은 sticky 냐 아니냐다 — sticky 레지스터는
 * 얕은 리셋에서 값이 살아남도록 만들어진 것들로, 링크가 끊겼다 붙어도
 * 진단 정보를 잃지 않기 위한 장치다. 그래서 두 리셋을 따로 둔다.
 * 이 구분은 엔드포인트 코드에도 그대로 나타난다 —
 * dw_pcie_ep_init_non_sticky_registers() 라는 이름이 붙은 이유가,
 * 링크가 내려갔다 오면 non-sticky 레지스터만 날아가므로 그것들만
 * 다시 채워 주면 되기 때문이다. */
enum dw_pcie_core_rst {
	DW_PCIE_NON_STICKY_RST,
	/* [한국어] non-sticky 레지스터를 지우는 리셋("non-sticky").
	 * 설정자: dw_pcie_get_resources() 의 이름 채우기.
	 * 읽는 자: 코어 리셋 시퀀스를 수행하는 dw_pcie_ 코드.
	 * 값 범위: 0.
	 * 동기화: 프로브/PM 경로 전용. */

	DW_PCIE_STICKY_RST,
	/* [한국어] sticky 레지스터까지 지우는 더 깊은 리셋("sticky").
	 * 값 범위: 1. 나머지는 위와 같다. */

	DW_PCIE_CORE_RST,
	/* [한국어] 컨트롤러 코어 로직 리셋("core"). 값 범위 2. */

	DW_PCIE_PIPE_RST,
	/* [한국어] PIPE 인터페이스 리셋("pipe"). 값 범위 3. */

	DW_PCIE_PHY_RST,
	/* [한국어] PHY 리셋("phy"). 값 범위 4. 아날로그 쪽까지 초기화한다. */

	DW_PCIE_HOT_RST,
	/* [한국어] Hot Reset("hot"). 값 범위 5.
	 *   전원을 끊지 않고 링크를 통해 상대에게 전달되는 규격 리셋으로,
	 *   가장 얕은 축에 든다. */

	DW_PCIE_PWR_RST,
	/* [한국어] 전원 리셋("pwr"). 값 범위 6. 가장 깊은 리셋. */

	DW_PCIE_NUM_CORE_RSTS
	/* [한국어] 개수 표지(값 7). core_rsts[] 배열 길이로만 쓴다. */
};

/* [한국어] LTSSM(Link Training and Status State Machine) 상태 목록.
 * PCIe 링크는 전원이 들어온 직후부터 정상 통신에 이르기까지 정해진
 * 상태 기계를 밟아 올라간다. 이 enum 은 그 상태들에 이름을 붙인 것이고,
 * 값은 임의로 정한 것이 아니라 **하드웨어 레지스터에서 읽히는 코드
 * 그대로**다 — 상류 주석이 "PCIE_PORT_DEBUG0 의 비트 0:5 와 맞춰야
 * 한다"고 못 박은 이유가 그것이다. 그래서 dw_pcie_get_ltssm() 은
 * 레지스터에서 6비트를 뽑아 아무 변환 없이 이 타입으로 캐스팅한다.
 *
 * 큰 흐름은 이렇다.
 *   Detect(상대가 있는지 전기적으로 감지) -> Polling(비트 동기와 심볼
 *   경계 확립) -> Configuration(레인 수와 레인 번호 협상) -> L0(정상).
 *   L0 에서 갈라지는 곳이 저전력(L0s/L1/L2)과 Recovery(속도 변경이나
 *   오류 후 재훈련)이며, Recovery 는 다시 L0 로 돌아온다.
 * 링크가 안 서는 문제를 진단할 때는 "어느 상태에서 맴도는가"가 곧
 * 원인을 가리킨다 — Detect 에서 못 나가면 물리적으로 상대가 없거나
 * 전원이 없는 것이고, Polling 에서 맴돌면 신호 품질 문제다.
 *
 * 이 값을 사람이 읽는 문자열로 바꿔 주는 것이
 * dw_pcie_ltssm_status_string() 이고, 그것을 debugfs 의 ltssm_status
 * 파일로 노출하는 것이 pcie-designware-debugfs.c 의
 * ltssm_status_show() 다.
 *
 * 공통 사항 — 아래 모든 값의 설정자는 하드웨어(레지스터를 읽어 온
 * dw_pcie_get_ltssm() 또는 SoC 의 ops->get_ltssm 콜백)이고,
 * 읽는 자는 dw_pcie_ltssm_status_string() 과 SoC 드라이버의 링크
 * 대기 루프다. 순수한 관측값이라 동기화가 필요 없다(읽는 순간의
 * 스냅숏일 뿐이며, 다음 순간 이미 달라져 있을 수 있다). */
enum dw_pcie_ltssm {
	/* Need to align with PCIE_PORT_DEBUG0 bits 0:5 */
	DW_PCIE_LTSSM_DETECT_QUIET = 0x0,	/* [한국어] Detect.Quiet — 리셋 직후의 출발점. 송신기를 전기적 유휴 상태로 두고 기다린다. 여기서 나가지 못하면 상대가 전혀 감지되지 않는다는 뜻이다. */
	DW_PCIE_LTSSM_DETECT_ACT = 0x1,	/* [한국어] Detect.Active — 수신기가 실제로 붙어 있는지 전기적으로 탐지하는 중. */
	DW_PCIE_LTSSM_POLL_ACTIVE = 0x2,	/* [한국어] Polling.Active — TS1 훈련 시퀀스를 주고받으며 비트 동기를 잡는 중. */
	DW_PCIE_LTSSM_POLL_COMPLIANCE = 0x3,	/* [한국어] Polling.Compliance — 규격 준수 시험용 패턴을 내보내는 상태. 계측 장비로 신호를 재려고 일부러 들어가는 곳이며, 정상 부팅 중에 여기 머물면 무언가 잘못된 것이다. */
	DW_PCIE_LTSSM_POLL_CONFIG = 0x4,	/* [한국어] Polling.Configuration — Polling 을 마치고 Configuration 으로 넘어가는 길목. */
	DW_PCIE_LTSSM_PRE_DETECT_QUIET = 0x5,	/* [한국어] Detect.Quiet 로 되돌아가기 직전의 준비 상태. */
	DW_PCIE_LTSSM_DETECT_WAIT = 0x6,	/* [한국어] Detect 단계에서 정해진 시간만큼 기다리는 상태. */
	DW_PCIE_LTSSM_CFG_LINKWD_START = 0x7,	/* [한국어] Configuration.Linkwidth.Start — 링크 폭 협상을 시작한다. 상대에게 "나는 몇 레인까지 된다"고 알린다. */
	DW_PCIE_LTSSM_CFG_LINKWD_ACEPT = 0x8,	/* [한국어] Configuration.Linkwidth.Accept — 상대가 제안한 폭을 받아들이는 단계. 철자 ACEPT 는 6글자 제한에 맞춘 하드웨어 표기 그대로다. */
	DW_PCIE_LTSSM_CFG_LANENUM_WAI = 0x9,	/* [한국어] Configuration.Lanenum.Wait — 레인 번호 배정을 기다린다. 레인마다 번호가 있어야 데이터를 올바른 순서로 재조립할 수 있다. */
	DW_PCIE_LTSSM_CFG_LANENUM_ACEPT = 0xa,	/* [한국어] Configuration.Lanenum.Accept — 레인 번호 배정을 수락. */
	DW_PCIE_LTSSM_CFG_COMPLETE = 0xb,	/* [한국어] Configuration.Complete — 폭과 레인 번호 협상이 끝났다. */
	DW_PCIE_LTSSM_CFG_IDLE = 0xc,	/* [한국어] Configuration.Idle — L0 로 넘어가기 직전의 마지막 정지점. */
	DW_PCIE_LTSSM_RCVRY_LOCK = 0xd,	/* [한국어] Recovery.RcvrLock — 재훈련의 첫 단계로, 비트 동기를 다시 잡는다. 속도를 바꾸거나 오류가 났을 때 L0 에서 여기로 떨어진다. */
	DW_PCIE_LTSSM_RCVRY_SPEED = 0xe,	/* [한국어] Recovery.Speed — 실제로 링크 속도를 바꾸는 단계(예: Gen1 -> Gen3). PORT_LOGIC_SPEED_CHANGE 비트를 세웠을 때 지나가는 곳이다. */
	DW_PCIE_LTSSM_RCVRY_RCVRCFG = 0xf,	/* [한국어] Recovery.RcvrCfg — 재훈련 중 구성을 다시 맞춘다. */
	DW_PCIE_LTSSM_RCVRY_IDLE = 0x10,	/* [한국어] Recovery.Idle — 재훈련을 마치고 L0 로 복귀하기 직전. */
	DW_PCIE_LTSSM_L0 = 0x11,	/* [한국어] L0 — 정상 동작 상태. TLP 를 실제로 주고받을 수 있는 유일한 상태이며, 위 PORT_LOGIC_LTSSM_STATE_L0 상수와 같은 값이다. "링크가 살아 있다"는 말은 곧 여기 있다는 뜻이다. */
	DW_PCIE_LTSSM_L0S = 0x12,	/* [한국어] L0s — 가장 얕은 저전력 상태. 송신만 잠시 쉬며, 복귀가 마이크로초 단위로 빠르다. */
	DW_PCIE_LTSSM_L123_SEND_EIDLE = 0x13,	/* [한국어] L1/L2/L3 로 내려가기 전에 EIOS(Electrical Idle Ordered Set)를 보내는 중. 상대에게 "이제 조용해진다"고 알리는 절차다. */
	DW_PCIE_LTSSM_L1_IDLE = 0x14,	/* [한국어] L1 유휴 상태. L0s 보다 깊어 전력은 더 아끼지만 복귀가 느리다. */
	DW_PCIE_LTSSM_L2_IDLE = 0x15,	/* [한국어] L2 유휴 상태. 주 전원이 꺼지고 보조 전원만 남는 훨씬 깊은 상태로, 시스템 서스펜드에서 쓴다. */
	DW_PCIE_LTSSM_L2_WAKE = 0x16,	/* [한국어] L2 에서 깨어나는 중. */
	DW_PCIE_LTSSM_DISABLED_ENTRY = 0x17,	/* [한국어] Disabled 상태로 들어가는 중. 소프트웨어가 의도적으로 링크를 꺼는 경로다. */
	DW_PCIE_LTSSM_DISABLED_IDLE = 0x18,	/* [한국어] Disabled 로 넘어가는 도중의 유휴 지점. */
	DW_PCIE_LTSSM_DISABLED = 0x19,	/* [한국어] Disabled — 링크가 꺼져 있다. 저전력과 달리 자동 복귀하지 않으며, 소프트웨어가 다시 켜야 한다. */
	DW_PCIE_LTSSM_LPBK_ENTRY = 0x1a,	/* [한국어] Loopback 진입. 루프백은 보낸 신호를 상대가 그대로 되돌려 주게 해 물리 계층만 시험하는 진단 모드다. */
	DW_PCIE_LTSSM_LPBK_ACTIVE = 0x1b,	/* [한국어] Loopback 수행 중. */
	DW_PCIE_LTSSM_LPBK_EXIT = 0x1c,	/* [한국어] Loopback 에서 빠져나오는 중. */
	DW_PCIE_LTSSM_LPBK_EXIT_TIMEOUT = 0x1d,	/* [한국어] Loopback 탈출이 제한 시간 안에 끝나지 않았다. 상대가 루프백을 풀어 주지 않는 고장 상황이다. */
	DW_PCIE_LTSSM_HOT_RESET_ENTRY = 0x1e,	/* [한국어] Hot Reset 진입. 링크를 통해 전달되는 규격 리셋으로, 전원은 유지된다. */
	DW_PCIE_LTSSM_HOT_RESET = 0x1f,	/* [한국어] Hot Reset 수행 중. 여기까지가 6비트(0x00~0x3f)에 담기는 하드웨어 코드다. */
	DW_PCIE_LTSSM_RCVRY_EQ0 = 0x20,	/* [한국어] Recovery.Equalization 단계 0 — Gen3 이상에서만 존재하는 이퀄라이제이션 절차의 첫 구간. 위 GEN3_EQ_ 레지스터들이 이 구간의 동작을 좌우한다. */
	DW_PCIE_LTSSM_RCVRY_EQ1 = 0x21,	/* [한국어] 이퀄라이제이션 단계 1. */
	DW_PCIE_LTSSM_RCVRY_EQ2 = 0x22,	/* [한국어] 이퀄라이제이션 단계 2 — 양쪽이 계수를 주고받기 시작하는 구간. GEN3_EQ_FMDC_T_MIN_PHASE23 이 여기 머무는 최소 시간을 정한다. */
	DW_PCIE_LTSSM_RCVRY_EQ3 = 0x23,	/* [한국어] 이퀄라이제이션 단계 3 — 마지막 수렴 구간. */

	/* Vendor glue drivers provide pseudo L1 substates from get_ltssm() */
	/* [한국어] 아래 둘은 하드웨어 LTSSM 코드가 아니라 **가짜(pseudo) 값**이다.
	 * L1.1 / L1.2 는 규격상 L1 안의 substate 라서 6비트 LTSSM 필드에
	 * 별도 코드가 없다. 그런데 이 둘을 구분할 수 있으면 진단에 유용하므로,
	 * SoC 접착 드라이버가 자기 벤더 레지스터를 추가로 읽어 알아낸 뒤
	 * ops->get_ltssm() 에서 이 값을 대신 돌려준다.
	 * 값이 0x141/0x142 로 6비트 범위(0x00~0x3f) 밖에 있는 것은
	 * 진짜 하드웨어 코드와 절대 충돌하지 않게 하려는 것이다.
	 * 이 트리에서 실제로 이 값을 만들어 내는 곳은 pcie-dw-rockchip.c 다. */
	DW_PCIE_LTSSM_L1_1 = 0x141,	/* [한국어] L1.1 substate — L1 이지만 공통 기준 클럭을 살려 둬 복귀가 비교적 빠른 쪽. */
	DW_PCIE_LTSSM_L1_2 = 0x142,	/* [한국어] L1.2 substate — 기준 클럭까지 끄는 더 깊은 쪽. 전력은 더 아끼지만 복귀에 시간이 걸린다. */

	DW_PCIE_LTSSM_UNKNOWN = 0xFFFFFFFF,	/* [한국어] 알 수 없음. dw_pcie_ltssm_status_string() 의 switch 가 어느 case 에도 걸리지 않을 때 쓰는 기본값이자, SoC 드라이버가 아직 상태를 모를 때 넣는 초기값이다(pcie-dw-rockchip.c 가 prev_val 초기화에 쓴다). */
};

/* [한국어] 아웃바운드 iATU 창 하나를 어떻게 프로그램할지 담는 서술자.
 * 인자를 아홉 개나 늘어놓는 대신 구조체 하나로 묶어
 * dw_pcie_prog_outbound_atu(pci, &atu) 에 넘긴다.
 * 관례상 호출자는 스택에 `= { 0 }` 으로 잡아 필요한 필드만 채우므로,
 * 채우지 않은 필드는 0 이라는 것이 곧 "이 기능을 쓰지 않는다"는 뜻이다. */
struct dw_pcie_ob_atu_cfg {
	int index;
	/* [한국어] 사용할 창 번호.
	 * 설정자: 호출자. 엔드포인트 쪽 dw_pcie_ep_outbound_atu() 는
	 *   ob_window_map 비트맵에서 빈 창을 find_first_zero_bit 으로 찾아
	 *   여기에 넣고, 호스트 쪽은 용도별로 미리 정해 둔 번호를 쓴다
	 *   (예: 메시지 전송 전용 창은 pp->msg_atu_index).
	 * 읽는 자: dw_pcie_prog_outbound_atu() — 맨 먼저
	 *   num_ob_windows 와 비교해 범위를 넘으면 -ENOSPC 로 거절한다.
	 * 값 범위: 0 ~ pci->num_ob_windows - 1.
	 * 동기화: 창 점유 관리는 비트맵 쪽에서 하며, 이 구조체는
	 *   호출자 스택에 잡히는 일회용이라 공유되지 않는다. */

	int type;
	/* [한국어] 이 창이 만들어 낼 TLP 종류.
	 * 설정자: 호출자.
	 * 읽는 자: dw_pcie_prog_outbound_atu() 가 REGION_CTRL1 에 그대로 싣고,
	 *   PCIE_ATU_TYPE_MSG 인지 따로 검사해 CTRL2 에 INHIBIT_PAYLOAD 를 더한다.
	 * 값 범위: PCIE_ATU_TYPE_MEM / _IO / _CFG0 / _CFG1 / _MSG.
	 * 동기화: 위와 같이 일회용. */

	u8 func_no;
	/* [한국어] 이 창을 소유할 physical function 번호.
	 * 설정자: 엔드포인트 경로에서 dw_pcie_ep_map_addr() 등이 넣는다.
	 *   호스트 경로는 채우지 않아 0 이 된다.
	 * 읽는 자: dw_pcie_prog_outbound_atu() 가 PCIE_ATU_FUNC_NUM(func_no) 로
	 *   비트 22:20 에 실어 CTRL1 에 쓴다.
	 * 값 범위: 0 ~ 7(3비트 필드).
	 * 동기화: 일회용. */

	u8 code;
	/* [한국어] 메시지 TLP 의 메시지 코드.
	 * 설정자: type 이 PCIE_ATU_TYPE_MSG 일 때만 의미가 있다. 이 트리에서
	 *   실제로 채우는 곳은 pcie-designware-host.c 한 곳으로,
	 *   PCIE_MSG_CODE_PME_TURN_OFF 를 넣어 서스펜드 직전 상대에게
	 *   전원을 끄라고 알린다.
	 * 읽는 자: dw_pcie_prog_outbound_atu() 가 MSG 타입일 때만 CTRL2 에 OR 한다.
	 * 값 범위: PCIe 규격의 메시지 코드(8비트). MSG 가 아니면 0.
	 * 동기화: 일회용. */

	u8 routing;
	/* [한국어] 메시지 TLP 의 라우팅 방식 — 목적지를 어떻게 정할지.
	 * 설정자: 위와 같이 호스트의 PME_Turn_Off 경로가
	 *   PCIE_MSG_TYPE_R_BC(브로드캐스트, 하위로 전부 뿌림)를 넣는다.
	 * 읽는 자: dw_pcie_prog_outbound_atu() 가 type 과 OR 해 CTRL1 에 쓴다.
	 * 값 범위: PCIe 규격의 라우팅 코드(3비트). MSG 가 아니면 0.
	 * 동기화: 일회용. */

	u32 ctrl2;
	/* [한국어] REGION_CTRL2 에 추가로 얹을 비트들.
	 * 설정자: 호출자가 필요할 때만. 이 트리에서는 호스트의 config 접근용
	 *   창 두 곳이 PCIE_ATU_CFG_SHIFT_MODE_ENABLE 을 넣는다.
	 * 읽는 자: dw_pcie_prog_outbound_atu() 가
	 *   PCIE_ATU_ENABLE | atu->ctrl2 로 합쳐서 쓴다 — 즉 ENABLE 은
	 *   함수가 알아서 붙이므로 호출자가 넣을 필요가 없다.
	 * 값 범위: PCIE_ATU_ 로 시작하는 CTRL2 비트들의 OR. 기본 0.
	 * 동기화: 일회용. */

	u64 parent_bus_addr;
	/* [한국어] 창이 받아들일 주소 범위의 시작 — 컨트롤러 입장에서
	 *   "부모 버스"(CPU/AXI 쪽) 주소다.
	 * 설정자: 호출자. 중요한 점은 CPU 물리 주소를 그대로 넣는 것이 아니라
	 *   pci->parent_bus_offset 을 뺀 값을 넣는다는 것이다
	 *   (dw_pcie_ep_map_addr() 의 addr - pci->parent_bus_offset 이 그 예).
	 *   컨트롤러가 보는 주소와 CPU 가 보는 주소가 다른 SoC 가 있기 때문이다.
	 * 읽는 자: dw_pcie_prog_outbound_atu() 가 LOWER/UPPER_BASE 에 나눠 쓰고,
	 *   size 를 더해 LIMIT 도 계산한다.
	 * 값 범위: pci->region_align 정렬을 만족해야 한다. 어기면 -EINVAL.
	 * 동기화: 일회용. */

	u64 pci_addr;
	/* [한국어] 변환 결과 — PCIe 버스에서의 목적지 주소.
	 * 설정자: 호출자. config 접근용 창이라면 주소가 아니라
	 *   PCIE_ATU_BUS/DEV/FUNC 로 조립한 BDF 가 들어간다.
	 * 읽는 자: dw_pcie_prog_outbound_atu() 가 LOWER/UPPER_TARGET 에 쓴다.
	 * 값 범위: region_align 정렬 필수.
	 * 동기화: 일회용. */

	u64 size;
	/* [한국어] 창의 크기(바이트).
	 * 설정자: 호출자.
	 * 읽는 자: dw_pcie_prog_outbound_atu() 가
	 *   limit = parent_bus_addr + size - 1 로 끝 주소를 만든다.
	 *   0 이면 -EINVAL 로 거절한다.
	 * 값 범위: 1 이상이며, 창이 pci->region_limit 을 넘어가면 안 된다.
	 *   4GB 를 넘는 창은 v4.60a 이상에서 INCREASE_REGION_SIZE 와 함께만 가능.
	 * 동기화: 일회용. */
};

/* [한국어] 호스트(루트 컴플렉스) 모드에서 SoC 접착 드라이버가 채우는
 * 콜백 테이블. 공통 코어가 정해진 순서로 일을 진행하다가, IP 바깥의
 * 벤더 레지스터를 만져야 하는 지점에서 여기 있는 함수를 부른다.
 * 전부 선택 사항이라 NULL 이면 그냥 건너뛴다 — 그래서 호출부는 항상
 * `if (pp->ops && pp->ops->init)` 같은 이중 검사를 한다.
 * 이 테이블 자체는 대개 SoC 드라이버의 static const 이므로 읽기 전용이다. */
struct dw_pcie_host_ops {
	int (*init)(struct dw_pcie_rp *pp);
	/* [한국어] 호스트 초기화 훅.
	 * 설정자: SoC 접착 드라이버의 static const 정의.
	 * 읽는 자: pcie-designware-host.c 의 초기화 경로.
	 * 호출 시점: 공통 코어가 자기 설정을 하기 전, PHY 를 켜고 벤더
	 *   레지스터를 준비해야 하는 자리.
	 * 반환: 0 이면 성공, 음수 errno 면 초기화 전체가 실패한다.
	 * 동기화: 프로브/재개 경로에서만 불리므로 프로세스 컨텍스트이며
	 *   잠들 수 있다. */

	void (*deinit)(struct dw_pcie_rp *pp);
	/* [한국어] init 의 짝. 자원을 되돌린다.
	 * 호출 시점: 호스트를 내릴 때(제거/서스펜드 경로).
	 * 반환값이 없으므로 실패를 알릴 방법이 없다 — 정리는 실패해도
	 *   계속 진행해야 하기 때문이다.
	 * 나머지는 init 과 같다. */

	void (*post_init)(struct dw_pcie_rp *pp);
	/* [한국어] 공통 코어가 자기 설정을 마친 **뒤에** 불리는 훅.
	 * init 과 나뉘어 있는 이유는 순서 때문이다 — 어떤 벤더 설정은
	 *   코어가 레지스터를 건드린 뒤에 덮어써야 살아남는다.
	 * 나머지는 init 과 같다. */

	int (*msi_init)(struct dw_pcie_rp *pp);
	/* [한국어] MSI 수신 설비를 SoC 가 직접 마련하는 훅.
	 * 왜 있는가: 어떤 SoC 는 IP 내장 MSI 수신기 대신 자기 인터럽트
	 *   컨트롤러로 MSI 를 받는다. 그런 경우 이 콜백을 제공하고,
	 *   공통 코어는 내장 수신기 설정을 건너뛴다.
	 * 읽는 자: pcie-designware-host.c 가 이 콜백의 유무로
	 *   pp->use_imsi_rx 를 결정한다 — 이 콜백이 있으면 내장 수신기를
	 *   쓰지 않는다는 뜻이다.
	 * 반환: 0 성공, 음수 errno 실패.
	 * 동기화: 프로브 경로, 프로세스 컨텍스트. */

	void (*pme_turn_off)(struct dw_pcie_rp *pp);
	/* [한국어] 서스펜드 직전 PME_Turn_Off 메시지를 보내는 훅.
	 * 왜 있는가: 이 메시지를 보내는 방법이 SoC 마다 다르다. 공통 코어는
	 *   아웃바운드 iATU 로 메시지 TLP 를 만드는 길(pp->use_atu_msg)을
	 *   갖고 있지만, 벤더 전용 레지스터로 보내야 하는 SoC 는 이 콜백을
	 *   대신 제공한다.
	 * 호출 시점: dw_pcie_suspend_noirq() 경로.
	 * 동기화: PM 콜백이라 프로세스 컨텍스트이며 잠들 수 있다. */
};

/* [한국어] 루트 포트(호스트) 모드의 모든 상태. struct dw_pcie 안에
 * 포인터가 아니라 값으로 박혀 있어서, 이 구조체 주소만 있으면
 * to_dw_pcie_from_pp() 가 container_of 로 바깥 컨트롤러를 되찾는다.
 * 크게 네 덩어리다 — config 공간 접근 창, I/O 공간 창,
 * IP 내장 MSI 수신기 상태, 그리고 PCI 코어와의 접점(bridge/ECAM). */
struct dw_pcie_rp {
	bool			use_imsi_rx:1;
	/* [한국어] IP 내장 MSI 수신기(internal MSI receiver)를 쓸 것인가.
	 * 설정자: pcie-designware-host.c 가 초기화 중에 계산한다 —
	 *   SoC 가 ops->msi_init 을 제공하거나 자체 MSI 도메인이 있으면
	 *   false, 아니면 true 다. 즉 "다른 방법이 없을 때 IP 것을 쓴다".
	 * 읽는 자: MSI 도메인 생성, 마스크/언마스크, 서스펜드/재개 경로가
	 *   전부 이 값으로 갈린다.
	 * 값 범위: 0/1(비트필드 1비트).
	 * 동기화: 프로브 때 한 번 정해지고 이후 읽기 전용. */

	bool			keep_rp_msi_en:1;
	/* [한국어] 재개(resume) 시 루트 포트 자신의 MSI Enable 비트를
	 *   건드리지 말라는 표시.
	 * 설정자: SoC 접착 드라이버. 이 트리에서는 pci-imx6.c 하나가 켠다.
	 * 읽는 자: pcie-designware-host.c 의 재개 경로가
	 *   `use_imsi_rx && !keep_rp_msi_en` 일 때만 그 비트를 다시 만진다.
	 * 값 범위: 0/1.
	 * 동기화: 프로브 때 설정, 이후 읽기 전용. */

	bool			cfg0_io_shared:1;
	/* [한국어] config 접근용 iATU 창과 I/O 공간 창을 **같은 창 하나로
	 *   돌려 쓰는가**. 창이 부족한 SoC 를 위한 절충이다.
	 * 설정자: pcie-designware-host.c 가 창 개수를 세어 보고 모자라면 켠다.
	 * 읽는 자: 같은 파일의 config 접근 경로. 켜져 있으면 config 접근
	 *   전후로 창을 config 용/I/O 용으로 갈아 끼워야 해서, 접근마다
	 *   iATU 재프로그래밍 비용이 붙는다.
	 * 값 범위: 0/1.
	 * 동기화: 프로브 때 설정. 다만 창을 갈아 끼우는 동작 자체는
	 *   config 접근 경로에서 일어난다. */

	u64			cfg0_base;
	/* [한국어] config 공간 창의 CPU 물리 시작 주소.
	 * 설정자: dw_pcie_host_get_resources() 가 디바이스트리의
	 *   "config" reg 영역에서 res->start 를 가져와 넣는다.
	 * 읽는 자: 아웃바운드 iATU 를 config 용으로 프로그램하는 코드.
	 *   또한 MSI 목적지 주소가 따로 없을 때 이 주소를 대신 쓰기도 한다
	 *   (pp->msi_data = pp->cfg0_base).
	 * 값 범위: 유효한 물리 주소.
	 * 동기화: 프로브 때 설정, 이후 읽기 전용. */

	void __iomem		*va_cfg0_base;
	/* [한국어] 같은 config 창을 ioremap 한 가상 주소.
	 * 설정자: ECAM 을 쓰지 않는 경로에서 devm_pci_remap_cfg_resource() 결과.
	 *   ECAM 을 쓰면 대신 아래 cfg 를 채우므로 이쪽은 비어 있다.
	 * 읽는 자: dw_pcie_own_conf_map_bus() / dw_pcie_other_conf_map_bus()
	 *   가 config 읽기/쓰기의 최종 주소를 만들 때.
	 * 값 범위: 유효한 __iomem 포인터 또는 NULL.
	 * 동기화: 프로브 때 설정, 이후 읽기 전용(단, 그 주소로의 접근은
	 *   config 락 아래에서 일어난다). */

	u32			cfg0_size;
	/* [한국어] config 창의 크기(바이트).
	 * 설정자: resource_size(res) — 위 cfg0_base 와 같은 자리에서.
	 * 읽는 자: iATU 창 크기 계산과 ECAM 사용 가능 여부 판정.
	 * 값 범위: 창 하나 분량. ECAM 을 쓰려면 버스 개수 x 1MB 이상이어야 한다.
	 * 동기화: 프로브 때 설정, 이후 읽기 전용. */

	resource_size_t		io_base;
	/* [한국어] PCI I/O 공간 창의 CPU 물리 주소.
	 * 설정자: 디바이스트리 ranges 의 IORESOURCE_IO 항목에서.
	 * 읽는 자: I/O 용 아웃바운드 iATU 프로그래밍.
	 * 값 범위: 유효한 물리 주소. I/O 창이 없으면 사용되지 않는다.
	 * 동기화: 프로브 때 설정, 이후 읽기 전용. */

	phys_addr_t		io_bus_addr;
	/* [한국어] 같은 I/O 창이 **PCI 버스 쪽에서** 보이는 주소.
	 * 왜 따로 두는가: I/O 공간은 PCI 쪽 주소가 대개 0 근처의 작은 값이고
	 *   CPU 쪽 주소는 SoC 메모리 맵 어딘가라, 둘이 전혀 다르다.
	 *   iATU 의 base(CPU 쪽)와 target(PCI 쪽)에 각각 들어간다.
	 * 설정자/읽는 자/동기화: io_base 와 같다. */

	u32			io_size;
	/* [한국어] I/O 창의 크기(바이트).
	 * 설정자/읽는 자/동기화: io_base 와 같다. */

	int			irq;
	/* [한국어] SoC 가 제공하는 보조 인터럽트 번호.
	 * 설정자: SoC 접착 드라이버가 platform_get_irq() 결과를 넣는다
	 *   (예: pci-dra7xx.c, pcie-armada8k.c).
	 * 읽는 자: 그 SoC 드라이버 자신 — 대개
	 *   irq_set_chained_handler_and_data() 로 자기 핸들러를 건다.
	 *   공통 코어는 이 필드를 해석하지 않는다.
	 * 값 범위: 유효한 리눅스 IRQ 번호, 또는 음수 errno.
	 * 동기화: 프로브 때 설정, 이후 읽기 전용. */

	const struct dw_pcie_host_ops *ops;
	/* [한국어] 위에서 정의한 호스트 콜백 테이블.
	 * 설정자: SoC 접착 드라이버가 자기 static const 테이블 주소를 넣는다.
	 * 읽는 자: pcie-designware-host.c 의 각 단계.
	 * 값 범위: 유효한 포인터 또는 NULL(콜백이 전혀 필요 없는 SoC).
	 *   NULL 일 수 있으므로 호출부는 항상 pp->ops 부터 검사한다.
	 * 동기화: 읽기 전용 상수 테이블. */

	int			msi_irq[MAX_MSI_CTRLS];
	/* [한국어] MSI 컨트롤 블록마다 배정된 부모 IRQ 번호(최대 8개).
	 * 왜 배열인가: 벡터 32개 묶음마다 별도의 물리 IRQ 선을 두는 SoC 가
	 *   있어서다. 하나만 두는 SoC 는 [0] 만 쓴다.
	 * 설정자: pcie-designware-host.c 가 디바이스트리에서 "msi0", "msi1"
	 *   식으로 찾아 채운다. 내장 수신기를 쓰지 않는 SoC 는 -ENODEV 를
	 *   넣어 두기도 한다(pci-dra7xx.c).
	 * 읽는 자: 같은 파일이 각 IRQ 에 dw_chained_msi_isr 을 체인 핸들러로
	 *   걸고, 정리할 때 다시 떼어 낸다.
	 * 값 범위: 양수 IRQ 번호, 0(없음), 또는 음수 errno.
	 * 동기화: 프로브 때 설정, 이후 읽기 전용. */

	struct irq_domain	*irq_domain;
	/* [한국어] 이 루트 포트가 만든 MSI IRQ 도메인.
	 * 왜 필요한가: 내장 수신기가 받는 벡터들을 리눅스 IRQ 번호로
	 *   보이게 하려면 도메인이 있어야 한다. 하위 장치가 MSI 를 요청하면
	 *   이 도메인에서 벡터를 하나 떼어 준다.
	 * 설정자: dw_pcie_allocate_domains().
	 * 읽는 자: 벡터 할당/해제와 인터럽트 디스패치 경로.
	 * 값 범위: 유효한 포인터 또는 NULL(내장 수신기를 쓰지 않을 때).
	 * 동기화: 도메인 내부 자료구조는 IRQ 코어가 보호한다. */

	dma_addr_t		msi_data;
	/* [한국어] 내장 MSI 수신기가 감시할 목적지 주소 — **장치가 보는
	 *   버스 주소**라 dma_addr_t 다.
	 * 설정자: pcie-designware-host.c 가 dmam_alloc_coherent() 로 8바이트를
	 *   할당해 그 DMA 주소를 넣거나, 그럴 필요가 없으면 cfg0_base 를 쓴다.
	 *   실제 메모리를 잡는 이유는 그 주소가 시스템에서 유효하고
	 *   충돌하지 않는다는 보장이 필요하기 때문이다.
	 * 읽는 자: PCIE_MSI_ADDR_LO/HI 에 이 값을 쓰고, MSI 메시지를 만들
	 *   때도 같은 주소를 하위 장치에게 알려 준다.
	 * 값 범위: 유효한 DMA 주소.
	 * 동기화: 프로브 때 설정, 이후 읽기 전용. */

	struct irq_chip		*msi_irq_chip;
	/* [한국어] MSI 벡터에 붙일 irq_chip — 마스크/언마스크/ack 구현체.
	 * 설정자: 기본은 pcie-designware-host.c 가
	 *   dw_pci_msi_bottom_irq_chip 을 넣지만, SoC 가 다른 방식으로
	 *   마스킹해야 하면 자기 것으로 갈아 끼운다(pci-keystone.c 가 그 예).
	 * 읽는 자: 도메인이 벡터를 배정할 때 irq_domain_set_info() 로 전달.
	 * 값 범위: 유효한 포인터.
	 * 동기화: 읽기 전용 상수 테이블을 가리킨다. */

	u32			num_vectors;
	/* [한국어] 실제로 쓸 MSI 벡터 개수.
	 * 설정자: 디바이스트리 값이 있으면 그것, 없으면
	 *   MSI_DEF_NUM_VECTORS(32). SoC 가 직접 넣기도 한다
	 *   (pcie-designware-plat.c 는 MAX_MSI_IRQS 를 넣는다).
	 * 읽는 자: 블록 수 계산(num_vectors / 32), 비트맵 탐색 범위,
	 *   도메인 크기 등 MSI 경로 전반.
	 * 값 범위: 32 의 배수이며 MAX_MSI_IRQS(256) 이하.
	 * 동기화: 프로브 때 설정, 이후 읽기 전용. */

	u32			irq_mask[MAX_MSI_CTRLS];
	/* [한국어] PCIE_MSI_INTR0_MASK 레지스터의 소프트웨어 사본(블록마다 하나).
	 * 왜 사본이 필요한가: 마스크/언마스크는 비트 하나만 바꾸는 일인데,
	 *   레지스터를 읽어서 고쳐 쓰면 그사이 다른 CPU 의 변경을 지워 버릴
	 *   수 있다. 그래서 메모리에 사본을 두고 그것을 고친 뒤 통째로 쓴다.
	 * 설정자/읽는 자: dw_pci_bottom_mask()/unmask() 가
	 *   irq_mask[ctrl] |= BIT(bit) 로 고치고 곧바로 레지스터에 쓴다.
	 *   재개 경로도 이 사본으로 레지스터를 복원한다.
	 * 값 범위: 각 원소가 32비트 마스크. 1 이 마스크됨.
	 * 동기화: **반드시 pp->lock 아래에서** 만져야 한다. 마스킹은
	 *   인터럽트 컨텍스트에서도 불리므로 raw_spinlock 을 쓴다. */

	struct pci_host_bridge  *bridge;
	/* [한국어] PCI 코어에 등록할 호스트 브리지 객체.
	 * 왜 필요한가: 리눅스 PCI 코어가 버스를 열거하고 리소스를 배분하는
	 *   출발점이 이 객체다. 여기에 config 접근 ops 와 주소 창 목록을
	 *   달아 코어에 넘기면, 그 아래로 장치들이 발견된다.
	 * 설정자: pcie-designware-host.c 가 devm_pci_alloc_host_bridge() 로
	 *   만들고, ops/child_ops/sysdata 를 채운다.
	 * 읽는 자: 리소스 창 순회, ECAM 판정, 그리고 PCI 코어 자신.
	 * 값 범위: 유효한 포인터.
	 * 동기화: PCI 코어의 규약을 따른다. */

	raw_spinlock_t		lock;
	/* [한국어] MSI 벡터 비트맵과 irq_mask[] 를 지키는 락.
	 * 왜 raw_spinlock 인가: 이 락을 잡는 경로 일부가 인터럽트 컨텍스트
	 *   (마스크/언마스크)라서, RT 커널에서도 잠들면 안 되기 때문이다.
	 * 설정자: 초기화 때 raw_spin_lock_init().
	 * 읽는 자: guard(raw_spinlock)(&pp->lock) 형태로 마스크/언마스크와
	 *   벡터 할당/해제 구간을 감싼다(벡터 할당 쪽은 irq 버전을 쓴다).
	 * 값 범위: 해당 없음.
	 * 동기화: 이 필드 자체가 동기화 장치다. */

	DECLARE_BITMAP(msi_irq_in_use, MAX_MSI_IRQS);
	/* [한국어] 어떤 MSI 벡터가 이미 배정되었는지 표시하는 비트맵(256비트).
	 * 왜 비트맵인가: 여러 벡터를 연속으로 달라는 요청(MSI 는 2의 거듭제곱
	 *   개수를 정렬된 위치에 요구한다)을 bitmap_find_free_region() 한 번으로
	 *   처리할 수 있기 때문이다.
	 * 설정자: dw_pcie_irq_domain_alloc() 이 bitmap_find_free_region() 으로
	 *   자리를 잡고, 해제는 bitmap_release_region().
	 * 읽는 자: 같은 두 함수.
	 * 값 범위: 비트 0 ~ num_vectors-1 만 실제로 쓰인다.
	 * 동기화: pp->lock 아래에서만 만진다. */

	bool			use_atu_msg;
	/* [한국어] PME_Turn_Off 같은 메시지 TLP 를 아웃바운드 iATU 창으로
	 *   만들어 보낼 것인가.
	 * 왜 있는가: 메시지 TLP 를 보내는 표준 방법이 따로 없어서,
	 *   "메시지 타입 창을 하나 잡아 두고 거기에 더미 쓰기를 하면
	 *   하드웨어가 메시지 TLP 로 바꿔 준다"는 기법을 쓴다.
	 *   그 대가로 아웃바운드 창 하나를 영구히 예약해야 한다.
	 * 설정자: SoC 접착 드라이버가 dw_pcie_host_init() **전에** 켜야 한다
	 *   (pcie-designware-host.c 의 상류 주석이 그 순서를 명시한다).
	 *   이 트리에서는 pcie-nxp-s32g.c 와 pci-imx6.c 가 켠다.
	 * 읽는 자: 창을 예약하는 초기화 코드와 실제 전송 코드.
	 * 값 범위: 0/1.
	 * 동기화: 프로브 때 설정, 이후 읽기 전용. */

	int			msg_atu_index;
	/* [한국어] 위 메시지 전송용으로 예약해 둔 아웃바운드 창 번호.
	 * 설정자: 초기화 중 창을 나눠 줄 때 ob_iatu_index++ 로 받아 둔다.
	 * 읽는 자: 메시지를 보낼 때 atu.index 에 넣는다. 보내기 전에
	 *   num_ob_windows 와 비교해 유효한지 먼저 확인한다.
	 * 값 범위: 0 ~ num_ob_windows-1. use_atu_msg 가 꺼져 있으면 무의미.
	 * 동기화: 프로브 때 설정, 이후 읽기 전용. */

	struct resource		*msg_res;
	/* [한국어] 메시지 창이 차지할 주소 영역.
	 * 왜 필요한가: 메시지를 보내려면 그 창에 실제로 더미 쓰기를 할
	 *   CPU 주소가 있어야 하고, 그 주소가 다른 용도와 겹치면 안 된다.
	 * 설정자: 디바이스트리 ranges 를 훑던 초기화 코드가 남는 영역을
	 *   이 용도로 떼어 둔다.
	 * 읽는 자: 창 크기(resource_size)와 시작 주소를 iATU 설정에 쓰고,
	 *   ioremap 해서 더미 쓰기를 수행한다.
	 * 값 범위: 유효한 resource 포인터 또는 NULL(없으면 -ENOSPC).
	 * 동기화: 프로브 때 설정, 이후 읽기 전용. */

	struct pci_eq_presets	presets;
	/* [한국어] 링크 이퀄라이제이션 프리셋 값들을 디바이스트리에서 읽어 둔 것.
	 * 왜 있는가: 보드마다 배선 특성이 달라 최적 프리셋이 다르다.
	 *   그 값을 코드에 박을 수 없으니 디바이스트리가 알려 주게 한다.
	 * 설정자: of_pci_get_equalization_presets(dev, &pp->presets,
	 *   pci->num_lanes) — 레인 수만큼 읽어 온다.
	 * 읽는 자: dw_pcie_program_presets() 가 속도(8/16/32 GT/s)에 따라
	 *   해당 배열을 골라 하드웨어에 쓴다.
	 * 값 범위: 속도별 프리셋 배열. 디바이스트리에 없으면 비어 있다.
	 * 동기화: 프로브 때 설정, 이후 읽기 전용. */

	struct pci_config_window *cfg;
	/* [한국어] ECAM 방식을 쓸 때 만든 config 창 서술자.
	 * 왜 있는가: ECAM 은 config 공간 전체를 메모리 맵에 통째로 펼치는
	 *   표준 방식이다. 조건만 맞으면 이쪽이 훨씬 싸다 — 접근마다
	 *   iATU 를 다시 프로그램할 필요가 없기 때문이다.
	 * 설정자: dw_pcie_create_ecam_window() 가 pci_ecam_create() 결과를
	 *   넣고, cfg->priv 에 pp 를 되꽂아 역참조 길을 만든다.
	 * 읽는 자: PCI 코어가 pp->bridge->sysdata 를 통해 쓴다.
	 *   정리 경로는 이 포인터가 NULL 이 아닐 때만 pci_ecam_free() 한다.
	 * 값 범위: 유효한 포인터 또는 NULL(ECAM 을 쓰지 않을 때).
	 * 동기화: 프로브 때 설정, 이후 읽기 전용. */

	bool			ecam_enabled;
	/* [한국어] 실제로 ECAM 경로를 쓰기로 했는지.
	 * 설정자: dw_pcie_ecam_enabled() 의 판정 결과.
	 *   조건은 세 가지다 — (1) native_ecam 이 꺼져 있고,
	 *   (2) config 영역 시작 주소가 256MB 정렬이며(규격이 요구하는
	 *   2^(n+20) 정렬인데 DWC 는 버스 번호에 8비트를 쓰므로 2^28),
	 *   (3) config 영역이 버스 범위 전체를 덮을 만큼 크다.
	 * 읽는 자: config 접근 ops 선택, 리소스 창 계산, 정리 경로.
	 * 값 범위: 0/1.
	 * 동기화: 프로브 때 설정, 이후 읽기 전용. */

	bool			native_ecam;
	/* [한국어] "이 SoC 는 ECAM 을 하드웨어가 직접 처리한다"는 표시.
	 * 왜 있는가: 그런 SoC 에서는 DWC 공통 코어가 ECAM 창을 또 만들면
	 *   안 된다. 그래서 dw_pcie_ecam_enabled() 는 이 값이 참이면
	 *   곧바로 false 를 돌려준다 — 이름과 반대로 보이지만,
	 *   "코어가 나설 필요 없음"이라는 뜻이다.
	 * 설정자: SoC 접착 드라이버. 이 트리에서는 pcie-al.c 하나가 켠다.
	 * 읽는 자: dw_pcie_ecam_enabled().
	 * 값 범위: 0/1.
	 * 동기화: 프로브 때 설정, 이후 읽기 전용. */

	bool                    skip_l23_ready;
	/* [한국어] 서스펜드할 때 링크가 L2/L3 Ready 로 내려가기를
	 *   기다리지 말라는 표시.
	 * 왜 있는가: 어떤 SoC 는 그 전이가 하드웨어적으로 완료 보고되지
	 *   않아, 기다리면 타임아웃으로 시간만 버린다.
	 * 설정자: SoC 접착 드라이버(pcie-eswin.c, pci-imx6.c).
	 * 읽는 자: dw_pcie_suspend_noirq() 경로의 대기 루프.
	 * 값 범위: 0/1.
	 * 동기화: 프로브 때 설정, 이후 읽기 전용. */
};

/* [한국어] 엔드포인트 모드에서 SoC 접착 드라이버가 채우는 콜백 테이블.
 * 호스트 쪽 dw_pcie_host_ops 와 같은 역할이지만, 여기에는 반드시
 * 채워야 하는 것이 하나 있다 — raise_irq 다. 인터럽트를 실제로
 * 어떻게 쏠지(MSI 인지 MSI-X 인지, 도어벨을 쓸지)를 SoC 가 골라야
 * 하기 때문이며, 공통 코어는 그 선택지를
 * dw_pcie_ep_raise_msi_irq()/msix_irq()/msix_irq_doorbell() 로
 * 준비해 두고 기다린다.
 * 이 테이블 자체는 SoC 드라이버의 static const 라 읽기 전용이다. */
struct dw_pcie_ep_ops {
	void	(*pre_init)(struct dw_pcie_ep *ep);
	/* [한국어] EPC 를 만든 직후, 주소 공간을 잡기 **전에** 불리는 훅.
	 * 왜 나뉘어 있는가: 어떤 SoC 는 pci_epc_mem_init() 이 돌기 전에
	 *   벤더 레지스터를 손봐야 한다.
	 * 설정자: SoC 접착 드라이버. 이 트리에서 제공하는 곳은
	 *   pcie-rcar-gen4.c 하나다.
	 * 읽는 자: dw_pcie_ep_init() 이 `if (ep->ops->pre_init)` 로 확인 후 호출.
	 * 반환값이 없어 실패를 알릴 수 없으므로, 실패할 수 있는 일은
	 *   여기 두지 않는 것이 전제다.
	 * 동기화: 프로브 경로, 프로세스 컨텍스트. */

	void	(*init)(struct dw_pcie_ep *ep);
	/* [한국어] 레지스터 초기화 단계에서 불리는 훅.
	 * 호출 시점: dw_pcie_ep_init_registers() 안에서, function 목록을
	 *   다 만든 뒤 BAR 을 끄기 직전.
	 * 설정자: SoC 접착 드라이버(선택).
	 * 읽는 자: dw_pcie_ep_init_registers().
	 * 동기화: 프로브/리셋 복구 경로, 프로세스 컨텍스트. */

	int	(*raise_irq)(struct dw_pcie_ep *ep, u8 func_no,
			     unsigned int type, u16 interrupt_num);
	/* [한국어] 호스트에게 인터럽트를 쏘는 훅. 사실상 필수다.
	 * @func_no: 어느 function 이 인터럽트를 내는지.
	 * @type: PCI_IRQ_INTX / PCI_IRQ_MSI / PCI_IRQ_MSIX 중 하나.
	 * @interrupt_num: 벡터 번호. **1-기반**이다(EPC 규약).
	 *   그래서 하위 구현들이 하나같이 interrupt_num - 1 로 바꿔 쓴다.
	 * 반환: 0 성공, 음수 errno 실패.
	 * 설정자: 모든 EP SoC 드라이버가 채운다.
	 * 읽는 자: 이 파일의 dw_pcie_ep_raise_irq() 가 그대로 전달한다.
	 *   전형적인 구현은 type 을 switch 해서 공통 코어의
	 *   dw_pcie_ep_raise_msi_irq() 나 _msix_irq() 를 부르는 것이다.
	 * 동기화: 호출 경로가 pci_epc_raise_irq() 이므로 epc->lock 뮤텍스를
	 *   쥔 채로 불린다. 잠들 수 있는 문맥이다. */

	const struct pci_epc_features* (*get_features)(struct dw_pcie_ep *ep);
	/* [한국어] 이 컨트롤러가 어떤 EP 기능을 지원하는지 알려 주는 훅.
	 * 반환: struct pci_epc_features 상수 포인터. BAR 별 타입
	 *   (BAR_FIXED / BAR_PROGRAMMABLE / BAR_RESIZABLE / BAR_RESERVED),
	 *   고정 크기, MSI/MSI-X 지원 여부 등이 들어 있다.
	 * 왜 중요한가: dw_pcie_ep_get_bar_type() 이 이 값을 보고 BAR 을
	 *   어떤 방식으로 설정할지 고르고, dw_pcie_ep_disable_bars() 는
	 *   BAR_RESERVED 만 건드리지 않고 남긴다. 콜백이 없으면
	 *   전부 BAR_PROGRAMMABLE 로 간주한다.
	 * 설정자: SoC 접착 드라이버. 대개 DWC_EPC_COMMON_FEATURES 매크로로
	 *   공통 두 항목을 채운 상수 테이블을 돌려준다.
	 * 읽는 자: dw_pcie_ep_get_features(), dw_pcie_ep_get_bar_type().
	 * 동기화: 상수 반환이라 없음. */
	/*
	 * Provide a method to implement the different func config space
	 * access for different platform, if different func have different
	 * offset, return the offset of func. if use write a register way
	 * return a 0, and implement code in callback function of platform
	 * driver.
	 */
	unsigned int (*get_dbi_offset)(struct dw_pcie_ep *ep, u8 func_no);
	/* [한국어] function 번호로부터 그 function 의 DBI 레지스터가 놓인
	 *   추가 오프셋을 알려 주는 훅. 상류 주석이 위에 그 취지를 적어 두었다.
	 * 왜 필요한가: 다중 function 엔드포인트에서 각 function 의 config
	 *   공간을 어디에 두는지는 SoC 마다 다르다. function 마다 일정
	 *   간격으로 늘어놓는 칩도 있고, 별도 레지스터로 선택하게 하는
	 *   칩도 있다. 후자는 이 콜백에서 0 을 돌려주고 자기 방식대로
	 *   전환을 처리하라는 것이 상류 주석의 지시다.
	 * 반환: 바이트 오프셋. 콜백이 없으면 0(= function 0 만 있는 경우).
	 * 설정자: pcie-rcar-gen4.c, pci-layerscape-ep.c 가 제공한다.
	 * 읽는 자: dw_pcie_ep_get_dbi_offset() 이 감싸고, 그것을
	 *   dw_pcie_ep_read_dbi()/write_dbi() 가 모든 접근마다 부른다.
	 * 동기화: 순수 계산 함수라 없음. */

	unsigned int (*get_dbi2_offset)(struct dw_pcie_ep *ep, u8 func_no);
	/* [한국어] 같은 것을 dbi2(BAR 마스크 전용 창)에 대해 알려 주는 훅.
	 * 왜 따로 있는가: dbi 와 dbi2 의 function 간격이 다른 SoC 가 있어서다.
	 * 설정자: pcie-rcar-gen4.c 만 제공한다.
	 * 읽는 자: dw_pcie_ep_get_dbi2_offset(). 이 콜백이 없으면
	 *   **하위 호환을 위해** get_dbi_offset 을 대신 쓴다 —
	 *   그 함수 안의 "for backward compatibility" 주석이 그 뜻이다.
	 * 동기화: 순수 계산 함수라 없음. */
};

/* [한국어] 엔드포인트 function 하나의 상태. 한 컨트롤러가 여러 PCI
 * function 을 노출할 수 있으므로(디바이스트리 "max-functions"),
 * function 마다 이 구조체를 하나씩 만들어 ep->func_list 에 매단다.
 * 만드는 곳은 dw_pcie_ep_init_registers() 이고, devm_kzalloc 이라
 * 디바이스가 사라질 때 자동으로 해제된다.
 * 찾을 때는 dw_pcie_ep_get_func_from_ep(ep, func_no) 로 리스트를 훑는다. */
struct dw_pcie_ep_func {
	struct list_head	list;
	/* [한국어] ep->func_list 에 매달리기 위한 연결 고리.
	 * 설정자: dw_pcie_ep_init_registers() 의 list_add_tail().
	 * 읽는 자: dw_pcie_ep_get_func_from_ep() 의 list_for_each_entry().
	 * 값 범위: 해당 없음.
	 * 동기화: 리스트를 만드는 것은 초기화 때 한 번뿐이고 이후로는
	 *   순회만 하므로 별도 락이 없다. 항목이 중간에 사라지지 않는다는
	 *   전제 위에 서 있다. */

	u8			func_no;
	/* [한국어] 이 항목이 나타내는 PCI function 번호.
	 * 설정자: dw_pcie_ep_init_registers() 의 루프 인덱스.
	 * 읽는 자: dw_pcie_ep_get_func_from_ep() 의 비교 조건.
	 * 값 범위: 0 ~ epc->max_functions - 1.
	 * 동기화: 초기화 후 불변. */

	u8			msi_cap;	/* MSI capability offset */
	/* [한국어] 이 function 의 config 공간에서 MSI capability 가 있는 위치.
	 * 왜 캐시하는가: MSI 를 쏠 때마다 capability 리스트를 다시 훑으면
	 *   config 접근이 여러 번 붙는다. 위치는 변하지 않으므로 한 번 찾아 둔다.
	 * 설정자: dw_pcie_ep_init_registers() 가
	 *   dw_pcie_ep_find_capability(ep, func_no, PCI_CAP_ID_MSI) 결과를 넣는다.
	 * 읽는 자: dw_pcie_ep_get_msi(), _set_msi(), dw_pcie_ep_raise_msi_irq() —
	 *   셋 다 맨 먼저 이 값이 0 인지 보고 0 이면 -EINVAL 로 거절한다.
	 * 값 범위: 0(= MSI capability 없음) 또는 0x40 이상의 오프셋.
	 * 동기화: 초기화 후 불변. */

	u8			msix_cap;	/* MSI-X capability offset */
	/* [한국어] 같은 방식으로 캐시해 둔 MSI-X capability 오프셋.
	 * 설정자: 위와 같은 자리에서 PCI_CAP_ID_MSIX 로 찾는다.
	 * 읽는 자: dw_pcie_ep_get_msix(), _set_msix(),
	 *   dw_pcie_ep_raise_msix_irq(), _msix_irq_doorbell().
	 * 값 범위: 0(없음) 또는 유효 오프셋.
	 * 동기화: 초기화 후 불변. */

	u8			bar_to_atu[PCI_STD_NUM_BARS];
	/* [한국어] BAR 마다 어떤 인바운드 iATU 창을 쓰고 있는지
	 *   — **BAR Match Mode** 전용 기록이다.
	 * 중요한 규약: 값은 창 번호 + 1 이다. 0 을 "아직 없음"으로 쓰기
	 *   위해서인데, dw_pcie_ep_ib_atu_bar() 의 상류 주석이 그 이유를
	 *   명시하고 있다("value 0 is used to identify unallocated mapping").
	 *   그래서 읽을 때는 항상 -1 을 해야 한다.
	 * 설정자: dw_pcie_ep_ib_atu_bar() 가 free_win + 1 을 넣고,
	 *   dw_pcie_ep_clear_ib_maps() 가 0 으로 되돌린다.
	 * 읽는 자: 같은 두 함수. 창을 다시 잡을 때 기존 창이 있으면
	 *   그것을 재사용한다.
	 * 값 범위: 0(없음) 또는 1 ~ num_ib_windows.
	 * 동기화: set_bar/clear_bar 경로에서만 만지며, 그 경로는
	 *   EPC 코어가 epc->lock 를 쥔 채로 부른다. */

	struct pci_epf_bar	*epf_bar[PCI_STD_NUM_BARS];
	/* [한국어] BAR 마다 현재 걸려 있는 EPF 쪽 요청 서술자.
	 * 왜 보관하는가: 두 가지 쓰임이 있다.
	 *   (1) "이 BAR 이 이미 설정되었는가"의 표시 — NULL 이 아니면 설정됨.
	 *       dw_pcie_ep_set_bar() 는 이것을 보고 재설정 경로로 갈지
	 *       최초 설정 경로로 갈지 가른다.
	 *   (2) MSI-X 테이블이 BAR 안에 있으므로, 인터럽트를 쏠 때
	 *       epf_bar[bir]->addr 로 그 BAR 의 로컬 가상 주소를 찾아
	 *       테이블 항목을 읽는다(dw_pcie_ep_raise_msix_irq()).
	 * 설정자: dw_pcie_ep_set_bar() 가 성공 끝에 넣고,
	 *   dw_pcie_ep_clear_bar() 가 NULL 로 되돌린다.
	 * 읽는 자: 위 두 경로와 dw_pcie_ep_init_rebar_registers()
	 *   (이미 설정된 BAR 은 그 크기만 광고하도록).
	 * 값 범위: 유효한 포인터 또는 NULL. 소유권은 EPF 드라이버에 있고
	 *   여기서는 빌려 쓰는 것이라 해제하지 않는다.
	 * 동기화: epc->lock 아래. */

	/* Only for Address Match Mode inbound iATU */
	u32			*ib_atu_indexes[PCI_STD_NUM_BARS];
	/* [한국어] **Address Match Mode** 에서 BAR 하나를 여러 조각으로
	 *   나눠 매핑했을 때, 그 조각들이 쓰는 창 번호 배열.
	 * 왜 별도인가: BAR Match Mode 는 BAR 당 창 하나라 위 bar_to_atu[]
	 *   한 칸이면 되지만, subrange 매핑은 조각 수만큼 창을 쓰므로
	 *   가변 길이 배열이 필요하다. 두 모드는 배타적이라 —
	 *   dw_pcie_ep_clear_ib_maps() 가 bar_to_atu 를 먼저 보고 있으면
	 *   그것만 처리하고 곧바로 돌아가는 구조가 그 배타성을 보여 준다.
	 * 값 범위 주의: 여기 담기는 것은 창 번호 **그대로**다(위 bar_to_atu 의
	 *   +1 규약이 적용되지 않는다). 배열 포인터가 NULL 인지로
	 *   존재 여부를 판단하기 때문이다.
	 * 설정자: dw_pcie_ep_ib_atu_addr() 가 devm_kcalloc 으로 잡아 채우고,
	 *   dw_pcie_ep_clear_ib_maps() 가 devm_kfree 로 반납하며 NULL 로 되돌린다.
	 * 읽는 자: dw_pcie_ep_clear_ib_maps() 의 해제 루프.
	 * 동기화: epc->lock 아래. */

	unsigned int		num_ib_atu_indexes[PCI_STD_NUM_BARS];
	/* [한국어] 위 배열에서 **실제로 유효한** 항목 수.
	 * 왜 따로 세는가: 매핑 도중 실패하면 일부만 잡힌 상태로 되돌려야
	 *   한다. dw_pcie_ep_ib_atu_addr() 는 창을 하나 잡을 때마다
	 *   이 값을 i + 1 로 올리므로, 중간에 실패해 err 로 뛰어도
	 *   dw_pcie_ep_clear_ib_maps() 가 정확히 잡힌 만큼만 푼다.
	 * 설정자/읽는 자: 위와 같은 두 함수.
	 * 값 범위: 0 ~ epf_bar->num_submap.
	 * 동기화: epc->lock 아래. */
};

/* [한국어] 엔드포인트 모드로 동작하는 DWC 컨트롤러 하나의 상태 전부.
 * struct dw_pcie 안에 값으로 박혀 있어(pci->ep), to_dw_pcie_from_ep() 이
 * container_of 로 역참조한다. SoC 글루 드라이버는 자기 구조체 안에 dw_pcie 를 품고,
 * probe 에서 이 구조체의 ops/page_size 를 채운 뒤 dw_pcie_ep_init() 을 부른다.
 * 나머지 필드는 pcie-designware-ep.c 가 초기화 과정에서 채운다. */
struct dw_pcie_ep {
	struct pci_epc		*epc;
	/* [한국어] 커널 PCI 엔드포인트 프레임워크가 이 컨트롤러를 가리키는 객체.
	 * 이것이 있어야 EPF 기능 드라이버가 configfs 를 통해 이 컨트롤러에 바인딩된다.
	 * 설정자: dw_pcie_ep_init() 이 devm_pci_epc_create() 결과를 넣는다. 같은 줄에서
	 *   epc_set_drvdata(epc, ep) 로 반대 방향 링크도 걸어, 두 포인터가 서로를 가리킨다.
	 * 읽는 자: 인터럽트 발사 경로가 창을 잡을 때(ep->epc), init/deinit 이
	 *   epc->mem 과 epc->max_functions 를 볼 때.
	 * 값 범위: 유효한 포인터. devm 으로 만들어 디바이스 수명에 묶인다.
	 * 동기화: 생성 후 불변. epc 내부 상태는 EPC 코어의 epc->lock 이 지킨다. */

	struct list_head	func_list;
	/* [한국어] 이 컨트롤러가 노출하는 PCI 함수별 상태(struct dw_pcie_ep_func)의 리스트 머리.
	 * 배열이 아니라 리스트인 것은 항목이 초기화 중 하나씩 devm_kzalloc 되어 붙기 때문이다.
	 * 설정자: dw_pcie_ep_init() 의 INIT_LIST_HEAD,
	 *   dw_pcie_ep_init_registers() 의 list_add_tail(함수 개수만큼).
	 * 읽는 자: dw_pcie_ep_get_func_from_ep() 의 순회 — 이 파일의 거의 모든 콜백이 거친다.
	 * 값 범위: 비어 있거나 max_functions 개의 항목. 한 번 붙은 항목은 제거되지 않는다.
	 * 동기화: 초기화 뒤로는 추가만 되고, 콜백 경로는 epc->lock 아래에서 직렬화되므로
	 *   별도 잠금이 없다. */

	const struct dw_pcie_ep_ops *ops;
	/* [한국어] SoC 글루가 제공하는 훅 묶음. 공용 코드가 알 수 없는 SoC 고유 동작
	 * (pre_init/init 초기화, get_features 능력표, raise_irq 인터럽트, get_dbi_offset
	 * 함수별 DBI 오프셋)을 여기로 위임한다.
	 * 설정자: SoC 글루 드라이버가 probe 에서 자기 정적 구조체를 가리키게 한다.
	 * 읽는 자: dw_pcie_ep_init()(pre_init), _init_registers()(init),
	 *   dw_pcie_ep_get_bar_type()/get_features(get_features),
	 *   dw_pcie_ep_raise_irq()(raise_irq), 헤더의 DBI 오프셋 인라인들.
	 * 값 범위: 유효한 포인터. 개별 훅은 NULL 일 수 있어 호출 전마다 검사한다.
	 * 동기화: 정적 const 라 불변. */

	phys_addr_t		phys_base;
	/* [한국어] 아웃바운드 창을 뚫을 로컬 물리 주소 구간의 시작. 디바이스 트리에서
	 * "addr_space" 라는 이름의 MEM 리소스로 온다.
	 * 설정자: dw_pcie_ep_get_resources() 가 res->start 를 넣는다.
	 * 읽는 자: pci_epc_mem_init() 에 넘겨 할당자의 관리 범위를 정한다.
	 *   artpec6 의 cpu_addr_fixup 훅도 이 값을 읽으므로,
	 *   dw_pcie_parent_bus_offset() 호출은 반드시 이 필드를 채운 뒤여야 한다.
	 * 값 범위: SoC 가 배선해 둔 물리 주소.
	 * 동기화: probe 에서 한 번 쓰이고 이후 불변. */

	size_t			addr_size;
	/* [한국어] 그 구간의 크기. phys_base 와 짝을 이룬다.
	 * 설정자: dw_pcie_ep_get_resources() 의 resource_size(res).
	 * 읽는 자: pci_epc_mem_init().
	 * 값 범위: 리소스 크기. 이 크기가 곧 엔드포인트가 동시에 열어 둘 수 있는
	 *   호스트 메모리 창의 총량을 제한한다.
	 * 동기화: probe 에서 한 번 쓰이고 이후 불변. */

	size_t			page_size;
	/* [한국어] 위 구간을 EPC 메모리 할당자가 나눠 줄 때의 단위.
	 * 설정자: SoC 글루가 probe 에서 채운다(공용 코드가 정하지 않는다).
	 * 읽는 자: dw_pcie_ep_init() 이 pci_epc_mem_init() 에 넘긴다. 그 뒤로는
	 *   할당자 쪽 epc->mem->window.page_size 가 실질적인 기준이 되어,
	 *   dw_pcie_ep_align_addr() 과 msi_mem 할당/반납이 그것을 쓴다.
	 * 값 범위: 2의 거듭제곱. 0 이면 할당자가 기본값을 정한다.
	 * 동기화: probe 에서 한 번 쓰이고 이후 불변. */

	phys_addr_t		*outbound_addr;
	/* [한국어] 아웃바운드 창 번호 → 그 창에 걸린 부모 버스 주소의 역인덱스 배열.
	 * 왜 필요한가: unmap_addr 콜백은 주소만 받는데 창을 끄려면 번호가 필요하다.
	 *   창을 걸 때 여기 적어 두고, dw_pcie_find_index() 가 선형 탐색으로 되짚는다.
	 * 설정자: dw_pcie_ep_init_registers() 가 devm_kcalloc 으로 잡고,
	 *   dw_pcie_ep_outbound_atu() 가 창을 걸며 채우며,
	 *   dw_pcie_ep_unmap_addr() 이 0 으로 지운다.
	 * 읽는 자: dw_pcie_find_index() — 단, ob_window_map 의 세워진 비트만 훑는다.
	 *   빈 자리에는 예전 값이 남아 있을 수 있어 전체를 훑으면 오탐이 나기 때문이다.
	 * 값 범위: 원소 num_ob_windows 개.
	 * 동기화: epc->lock 아래이거나 인터럽트 발사 경로. 자체 잠금은 없다. */

	unsigned long		*ib_window_map;
	/* [한국어] 인바운드 iATU 창의 점유 비트맵. 비트 n 이 서 있으면 창 n 이 쓰이는 중.
	 * 설정자: dw_pcie_ep_init_registers() 가 devm_bitmap_zalloc 으로 잡고,
	 *   dw_pcie_ep_ib_atu_bar()/_ib_atu_addr() 이 set_bit,
	 *   dw_pcie_ep_clear_ib_maps() 가 clear_bit 한다.
	 * 읽는 자: 위 두 매핑 함수의 find_first_zero_bit.
	 * 값 범위: num_ib_windows 비트. 크기는 dw_pcie_iatu_detect() 가 알아낸 값이다.
	 * 동기화: set_bar/clear_bar 경로에서만 만지므로 epc->lock 이 직렬화한다.
	 *   비트 조작 자체는 원자적이지만, "빈 자리 찾기 + 세우기" 는 원자적이지 않다. */

	unsigned long		*ob_window_map;
	/* [한국어] 아웃바운드 iATU 창의 점유 비트맵. 인바운드 쪽과 같은 규약이다.
	 * 설정자: dw_pcie_ep_init_registers() 의 할당,
	 *   dw_pcie_ep_outbound_atu() 의 set_bit, dw_pcie_ep_unmap_addr() 의 clear_bit.
	 * 읽는 자: dw_pcie_ep_outbound_atu() 의 find_first_zero_bit,
	 *   dw_pcie_find_index() 의 for_each_set_bit.
	 * 값 범위: num_ob_windows 비트.
	 * 동기화: 인바운드와 달리 EPC 콜백 밖(인터럽트 발사 경로)에서도 만져진다.
	 *   그런데도 자체 잠금이 없어, MSI 발사와 map_addr 이 겹치면 원리상 경합이 있다. */

	void __iomem		*msi_mem;
	/* [한국어] MSI/MSI-X 를 쏘기 위해 미리 예약해 둔 한 페이지의 커널 가상 주소.
	 * 왜 미리 잡는가: 인터럽트를 낼 때마다 주소를 할당하면 실패할 수 있고,
	 *   그 실패를 인터럽트 경로에서 복구할 방법이 없다. 그래서 초기화 때 확보해 둔다.
	 * 설정자: dw_pcie_ep_init() 의 pci_epc_mem_alloc_addr().
	 * 읽는 자: dw_pcie_ep_raise_msi_irq()/_msix_irq() 의 writel/readl 목적지
	 *   (정렬 보정 offset 을 더한 자리에 쓴다).
	 * 값 범위: 유효한 iomem 포인터. 실패 시 init 이 -ENOMEM 으로 중단된다.
	 * 동기화: 포인터 자체는 불변. 이 창이 어디에 연결돼 있는지는 아래 세 필드가 관리한다. */

	phys_addr_t		msi_mem_phys;
	/* [한국어] 위 페이지의 물리 주소. 아웃바운드 창의 로컬 쪽 끝이 된다.
	 * 설정자: pci_epc_mem_alloc_addr() 이 출력 인자로 채운다.
	 * 읽는 자: dw_pcie_ep_map_addr()/unmap_addr() 에 넘기는 addr 인자,
	 *   그리고 dw_pcie_ep_stop() 의 창 반납.
	 * 값 범위: addr_space 구간 안의 페이지 정렬 주소.
	 * 동기화: 불변. */

	/* MSI outbound iATU state */
	bool			msi_iatu_mapped;
	/* [한국어] MSI 용 아웃바운드 창이 지금 걸려 있는지.
	 * 왜 필요한가: 원문 주석대로 AXI 브리지에 트랜잭션이 떠 있는 동안 iATU 를
	 *   재프로그래밍하는 것이 규약상 지원되지 않는다. 그래서 창을 한 번 걸고
	 *   계속 재사용하며, 그 상태를 이 플래그로 기억한다.
	 * 설정자: dw_pcie_ep_init() 이 false 로 초기화,
	 *   dw_pcie_ep_raise_msi_irq() 이 창을 걸며 true / 걷으며 false,
	 *   dw_pcie_ep_stop() 이 창을 반납하며 false.
	 * 읽는 자: 같은 두 함수.
	 * 값 범위: true/false.
	 * 동기화: 없음. 인터럽트 발사가 여러 스레드에서 동시에 들어오면 보호되지 않는다. */

	u64			msi_msg_addr;
	/* [한국어] 지금 걸려 있는 창이 향하는 호스트 쪽 MSI 목적지 주소.
	 * 왜 기억하는가: 호스트가 MSI 주소를 바꿀 수 있기 때문이다. 매번 능력 구조체에서
	 *   읽은 주소를 이 값과 견주어, 달라졌을 때만 창을 다시 건다.
	 * 설정자: dw_pcie_ep_init() 이 0 으로, dw_pcie_ep_raise_msi_irq() 이 창을 걸며 갱신.
	 * 읽는 자: dw_pcie_ep_raise_msi_irq() 의 재프로그래밍 판단.
	 * 값 범위: 정렬 보정을 마친 64비트 주소.
	 * 동기화: msi_iatu_mapped 와 같다 — 자체 잠금 없음. */

	size_t			msi_map_size;
	/* [한국어] 지금 걸려 있는 창의 크기. 목적지 주소와 함께 재프로그래밍 여부를 가른다.
	 * 왜 크기까지 보는가: dw_pcie_ep_align_addr() 이 정렬 보정 때문에 크기를 늘리는데,
	 *   목적지 주소의 정렬 여분이 달라지면 같은 4바이트 쓰기라도 필요한 창 크기가 달라진다.
	 * 설정자: dw_pcie_ep_init() 이 0 으로, dw_pcie_ep_raise_msi_irq() 이 창을 걸며 갱신.
	 * 읽는 자: dw_pcie_ep_raise_msi_irq() 의 재프로그래밍 판단.
	 * 값 범위: 할당자 페이지 단위로 올림된 크기.
	 * 동기화: 위 두 필드와 같다. */
};

/* [한국어] RC/EP 양쪽이 공통으로 쓰는 SoC 글루 훅 묶음. 앞의 dw_pcie_ep_ops 가
 * 엔드포인트 전용이라면 이쪽은 컨트롤러 공통이다. 모든 항목이 선택적이라
 * 공용 코드는 부르기 전에 반드시 NULL 검사를 한다 — 훅이 없으면 기본 동작으로 간다. */
struct dw_pcie_ops {
	u64	(*cpu_addr_fixup)(struct dw_pcie *pcie, u64 cpu_addr);
	/* [한국어] CPU 물리 주소를 컨트롤러가 붙은 부모 버스 주소로 바꾸는 옛 방식의 훅.
	 * 지금은 디바이스 트리의 ranges 로 그 차이를 알아내는 쪽이 정식이고, 이 훅은
	 * 아래 use_parent_dt_ranges 주석대로 "있으면 디바이스 트리 결과와 비교해 보는"
	 * 검증용으로 남았다.
	 * 설정자: 주소 변환이 필요한 SoC 글루(artpec6 등).
	 * 읽는 자: dw_pcie_parent_bus_offset().
	 * 값 범위: NULL 이면 오프셋 0 을 가정한다.
	 * 동기화: 정적 const 라 불변. */

	u32	(*read_dbi)(struct dw_pcie *pcie, void __iomem *base, u32 reg,
			    size_t size);
	/* [한국어] DBI 읽기를 SoC 가 가로채는 훅. 표준 readl 로는 안 되는 IP 통합이 있다 —
	 * 예컨대 DBI 접근 전에 별도 레지스터로 창을 열어야 하거나, 정렬 제약이 다른 경우다.
	 * 설정자: 그런 제약이 있는 SoC 글루.
	 * 읽는 자: dw_pcie_read_dbi()(pcie-designware.c). 훅이 없으면 dw_pcie_read() 로 간다.
	 * 값 범위: NULL 허용. base 로 dbi_base 또는 dbi_base2 가 넘어온다.
	 * 동기화: 정적 const 라 불변. */

	void	(*write_dbi)(struct dw_pcie *pcie, void __iomem *base, u32 reg,
			     size_t size, u32 val);
	/* [한국어] 위 read_dbi 의 쓰기 짝.
	 * 설정자: 같은 SoC 글루.
	 * 읽는 자: dw_pcie_write_dbi().
	 * 값 범위: NULL 허용.
	 * 동기화: 정적 const 라 불변. */

	void    (*write_dbi2)(struct dw_pcie *pcie, void __iomem *base, u32 reg,
			      size_t size, u32 val);
	/* [한국어] DBI2 창(BAR 마스크 쪽) 전용 쓰기 훅. DBI2 가 별도로 존재하는 이유는
	 * BAR 주소와 BAR 마스크를 같은 오프셋에 둘 수 없어 IP 가 같은 레지스터를
	 * 두 창으로 노출하기 때문이다. 읽기 훅이 없는 것은 마스크를 되읽을 일이 없어서다.
	 * 설정자: SoC 글루.
	 * 읽는 자: dw_pcie_write_dbi2(). 훅이 없으면 dbi_base2 에 직접 쓴다.
	 * 값 범위: NULL 허용.
	 * 동기화: 정적 const 라 불변. */

	bool	(*link_up)(struct dw_pcie *pcie);
	/* [한국어] 링크가 올라왔는지 SoC 방식으로 판정하는 훅. 표준 디버그 레지스터가
	 * 아니라 SoC 전용 상태 레지스터를 봐야 하는 경우에 쓴다.
	 * 설정자: SoC 글루.
	 * 읽는 자: dw_pcie_link_up(). 훅이 없으면 LTSSM 상태로 판정한다.
	 * 값 범위: NULL 허용.
	 * 동기화: 정적 const 라 불변. */

	enum dw_pcie_ltssm (*get_ltssm)(struct dw_pcie *pcie);
	/* [한국어] LTSSM 상태를 SoC 방식으로 읽는 훅.
	 * 설정자: SoC 글루.
	 * 읽는 자: 헤더의 dw_pcie_get_ltssm() 인라인. 훅이 없으면
	 *   PCIE_PORT_DEBUG0 에서 상태 필드를 뽑는 기본 구현으로 간다.
	 * 값 범위: NULL 허용.
	 * 동기화: 정적 const 라 불변. */

	int	(*start_link)(struct dw_pcie *pcie);
	/* [한국어] 링크 트레이닝을 시작하는 훅. 실제 동작은 SoC 마다 다른 PHY 제어와
	 * 레지스터 조작이라 공용 코드가 대신할 수 없다.
	 * 설정자: 거의 모든 SoC 글루.
	 * 읽는 자: 헤더의 dw_pcie_start_link() 인라인 → dw_pcie_ep_start() 와 호스트 초기화.
	 * 값 범위: NULL 이면 dw_pcie_start_link() 가 0 을 돌려주어 "이미 올라와 있음" 처럼 동작한다.
	 * 동기화: 정적 const 라 불변. */

	void	(*stop_link)(struct dw_pcie *pcie);
	/* [한국어] 링크를 내리는 훅. start_link 의 역.
	 * 설정자: SoC 글루.
	 * 읽는 자: 헤더의 dw_pcie_stop_link() 인라인 → dw_pcie_ep_stop() 등.
	 * 값 범위: NULL 허용(그 경우 아무것도 하지 않는다).
	 * 동기화: 정적 const 라 불변. */
};

/* [한국어] 이 컨트롤러의 debugfs 계층을 되찾기 위한 최소한의 기록.
 * pcie-designware-debugfs.c 가 만들고 pci->debugfs 에 걸어 둔다. */
struct debugfs_info {
	struct dentry		*debug_dir;
	/* [한국어] dwc_pcie_<디바이스명> 최상위 디렉토리.
	 * 설정자: dwc_pcie_debugfs_init().
	 * 읽는 자: dwc_pcie_debugfs_deinit() 의 debugfs_remove_recursive() — 이 한 번의
	 *   재귀 삭제가 그 아래 수십 개 파일을 통째로 걷으므로, 개별 해제 코드가 필요 없다.
	 * 값 범위: 유효한 dentry. debugfs API 는 실패해도 오류 포인터를 그대로 넘겨
	 *   이후 호출이 무해하게 실패하도록 설계돼 있어 별도 검사가 없다.
	 * 동기화: 초기화 후 불변. */

	void			*rasdes_info;
	/* [한국어] RAS DES 파일들이 공유하는 정보(struct dwc_pcie_rasdes_info)의 포인터.
	 * void * 인 것은 그 구조체가 pcie-designware-debugfs.c 안에만 정의돼 있어
	 * 이 헤더가 타입을 알 수 없기 때문이다 — 헤더를 얇게 유지하려는 선택이다.
	 * 설정자: dwc_pcie_rasdes_debugfs_init().
	 * 읽는 자: 그 파일의 모든 read/write 핸들러가 pci->debugfs->rasdes_info 로 되찾는다.
	 * 값 범위: 유효한 포인터. RAS DES 가 없는 SoC 에서는 NULL 로 남는다.
	 * 동기화: 초기화 후 불변. 그 안의 뮤텍스가 카운터 접근을 지킨다. */
};

/* [한국어] DesignWare PCIe 컨트롤러 하나를 통째로 나타내는 최상위 구조체.
 * RC 로 쓰든 EP 로 쓰든 이것이 뿌리이며, 두 모드의 상태(pp 와 ep)를 값으로 품고 있다.
 * SoC 글루 드라이버는 자기 구조체 안에 이것을 하나 두고 probe 에서 dev/ops/
 * (필요하면) 클럭·리셋 이름 등을 채운 뒤 dw_pcie_host_init() 또는
 * dw_pcie_ep_init() 을 부른다. 나머지 필드는 dw_pcie_get_resources(),
 * dw_pcie_version_detect(), dw_pcie_iatu_detect() 가 하드웨어를 탐지해 채운다. */
struct dw_pcie {
	struct device		*dev;
	/* [한국어] 이 컨트롤러의 커널 디바이스. 모든 devm 할당의 주인이자 로그의 주체이고,
	 * 디바이스 트리 노드를 얻는 통로(dev->of_node)이기도 하다.
	 * 설정자: SoC 글루 probe.
	 * 읽는 자: 사실상 이 드라이버 묶음 전체.
	 * 값 범위: 유효한 포인터. NULL 일 수 없다.
	 * 동기화: probe 에서 한 번 쓰이고 이후 불변. */

	void __iomem		*dbi_base;
	/* [한국어] DBI(Data Bus Interface) 창의 가상 주소. 이 컨트롤러 자신의 PCI 설정
	 * 공간과 포트 논리 레지스터가 여기 매핑돼 있다. 아직 버스에 열거되지 않은
	 * 자기 자신의 설정 공간을 보는 유일한 통로다.
	 * 설정자: dw_pcie_get_resources() 가 "dbi" 리소스를 ioremap 한 결과.
	 * 읽는 자: dw_pcie_read_dbi()/write_dbi() 와 그 위의 모든 접근자.
	 * 값 범위: 유효한 iomem 포인터. 없으면 초기화가 실패한다.
	 * 동기화: 포인터는 불변. 그 너머 레지스터 접근에는 잠금이 없다. */

	resource_size_t		dbi_phys_addr;
	/* [한국어] 위 DBI 창의 물리 주소. 가상 주소만으로는 알 수 없는 값이라 따로 둔다.
	 * 설정자: dw_pcie_get_resources().
	 * 읽는 자: dw_pcie_parent_bus_offset() 이 CPU 주소와 부모 버스 주소의 차이를
	 *   계산할 때 기준으로 삼는다.
	 * 값 범위: SoC 가 배선한 물리 주소.
	 * 동기화: 불변. */

	void __iomem		*dbi_base2;
	/* [한국어] DBI2 창의 가상 주소. DBI 와 같은 오프셋을 다른 의미로 노출한다 —
	 * DBI 로 쓰면 BAR 주소, DBI2 로 쓰면 BAR 마스크(크기)다. 두 값을 한 자리에
	 * 둘 수 없어 IP 가 창을 나눈 결과이며, 엔드포인트의 BAR 설정이 이것을 쓴다.
	 * 설정자: dw_pcie_get_resources() 가 "dbi2" 리소스를 ioremap 하거나,
	 *   별도 리소스가 없으면 dbi_base 에 고정 오프셋을 더해 만든다.
	 * 읽는 자: dw_pcie_write_dbi2() 와 그 위의 dw_pcie_ep_writel_dbi2().
	 * 값 범위: 유효한 iomem 포인터.
	 * 동기화: 불변. */

	void __iomem		*atu_base;
	/* [한국어] iATU(내부 주소 변환 유닛) 레지스터 창의 가상 주소.
	 * 신형 "unrolled" 방식에서는 창마다 독립된 레지스터 묶음이 여기 늘어서 있고,
	 * 구형 뷰포트 방식에서는 이 자리가 DBI 안쪽을 가리킨다.
	 * 설정자: dw_pcie_get_resources() 가 "atu" 리소스를 ioremap 하거나,
	 *   없으면 dbi_base 에 고정 오프셋을 더한다.
	 * 읽는 자: dw_pcie_select_atu() 를 거치는 모든 iATU 프로그래밍.
	 * 값 범위: 유효한 iomem 포인터.
	 * 동기화: 불변. 단 뷰포트 방식은 "뷰포트 선택 + 접근" 두 단계가 원자적이지 않은데
	 *   그것을 지키는 잠금이 이 드라이버에 없다. */

	void __iomem		*elbi_base;
	/* [한국어] ELBI(External Local Bus Interface) 창. DWC 코어 바깥에 SoC 가 덧붙인
	 * 전용 레지스터 블록이라 내용이 SoC 마다 완전히 다르다. 공용 코드는 매핑만 해 주고
	 * 해석하지 않는다.
	 * 설정자: dw_pcie_get_resources() 가 "elbi" 또는 "apb" 리소스를 ioremap.
	 * 읽는 자: SoC 글루(pci-exynos.c 등)가 자기 레지스터를 읽고 쓸 때.
	 * 값 범위: 유효한 iomem 포인터. 그런 리소스가 없는 SoC 에서는 NULL 로 남는다.
	 * 동기화: 불변. 그 너머의 동기화는 글루의 책임. */

	resource_size_t		atu_phys_addr;
	/* [한국어] iATU 창의 물리 주소.
	 * 설정자: dw_pcie_get_resources().
	 * 읽는 자: dw_pcie_parent_bus_offset() 이 "atu" 이름으로 불릴 때의 기준 주소.
	 * 값 범위: SoC 가 배선한 물리 주소. 별도 리소스가 없으면 0 으로 남을 수 있다.
	 * 동기화: 불변. */

	size_t			atu_size;
	/* [한국어] iATU 레지스터 창의 크기. 이 값이 곧 "창이 몇 개까지 있을 수 있는가" 의
	 * 상한을 정한다 — dw_pcie_iatu_detect() 가 창 하나당 512바이트로 나눠
	 * 최대 256개까지로 후보를 좁힌 뒤, 실제로 값을 써 보며 개수를 확정한다.
	 * 설정자: dw_pcie_get_resources() 가 리소스 크기로 채우고, 없으면 4KB 를 가정한다.
	 *   pcie-tegra194.c 처럼 글루가 직접 넣는 경우도 있다.
	 * 읽는 자: dw_pcie_iatu_detect(), eDMA 레지스터 위치 결정.
	 * 값 범위: 4KB 이상.
	 * 동기화: 초기화 후 불변. */

	resource_size_t		parent_bus_offset;
	/* [한국어] CPU 물리 주소와 컨트롤러가 붙어 있는 부모 버스 주소의 차이.
	 * 왜 필요한가: iATU 에 적어야 하는 주소는 CPU 가 보는 주소가 아니라 컨트롤러가
	 *   보는 주소다. 두 관점이 어긋나는 SoC 가 있어 그 차이를 보정해야 한다.
	 * 설정자: dw_pcie_parent_bus_offset() 의 결과를 host/ep 초기화가 넣는다.
	 *   엔드포인트에서는 ep->phys_base 를 채운 뒤여야 하는데, artpec6 의
	 *   cpu_addr_fixup 훅이 그 값을 읽기 때문이다.
	 * 읽는 자: dw_pcie_ep_map_addr()/unmap_addr() 이 빼고, 호스트 쪽 창 설정도 같다.
	 * 값 범위: 대개 0. 0 이 아니면 주소 공간이 어긋난 SoC 다.
	 * 동기화: 초기화 후 불변. */

	u32			num_ib_windows;
	/* [한국어] 이 IP 에 실제로 있는 인바운드 iATU 창의 개수.
	 * 설정자: dw_pcie_iatu_detect() 가 창에 값을 써 보고 되읽는 방식으로 알아낸다 —
	 *   IP 설정마다 다르고 레지스터로 알려 주지 않기 때문이다.
	 * 읽는 자: ib_window_map 비트맵의 크기이자 find_first_zero_bit 의 상한.
	 * 값 범위: 보통 2~16.
	 * 동기화: 탐지 후 불변. */

	u32			num_ob_windows;
	/* [한국어] 아웃바운드 iATU 창의 개수. 인바운드와 같은 방식으로 탐지한다.
	 * 설정자: dw_pcie_iatu_detect().
	 * 읽는 자: ob_window_map 비트맵과 outbound_addr 배열의 크기.
	 * 값 범위: 보통 2~16. 이 값이 곧 동시에 열어 둘 수 있는 호스트 창의 개수다.
	 * 동기화: 탐지 후 불변. */

	u32			region_align;
	/* [한국어] iATU 창의 시작 주소와 크기가 맞아야 하는 정렬 단위.
	 * 설정자: dw_pcie_iatu_detect() 가 창에 값을 써 보며 알아낸다.
	 * 읽는 자: dw_pcie_ep_align_addr() 의 마스크, dw_pcie_ep_validate_submap() 의
	 *   조각 검증, iATU 프로그래밍 함수들의 정렬 확인.
	 * 값 범위: 2의 거듭제곱. 대개 64KB.
	 * 동기화: 탐지 후 불변. */

	u64			region_limit;
	/* [한국어] 창 하나가 덮을 수 있는 주소 범위의 상한 마스크.
	 * 왜 필요한가: iATU 의 limit 레지스터가 32비트뿐인 구형 IP 에서는 창 하나가
	 *   4GB 경계를 넘을 수 없다. 그 제약을 이 값으로 표현해, 창을 걸기 전에
	 *   시작과 끝이 같은 구간 안에 있는지 검사한다.
	 * 설정자: dw_pcie_iatu_detect().
	 * 읽는 자: dw_pcie_prog_outbound_atu()/prog_inbound_atu() 의 경계 검사,
	 *   호스트 쪽 창 분할(pcie-designware-host.c)이 (region_limit + 1) 로 창 크기를 자른다.
	 * 값 범위: dw_pcie_iatu_detect() 가 (상위 limit 레지스터 값 << 32) | (4GB - 1) 로
	 *   계산한다. 상위 limit 레지스터가 없는 구형 IP 에서는 그 값이 0 이라
	 *   정확히 0xffffffff(4GB - 1)가 되고, 지원하는 IP 에서는 그보다 커진다.
	 * 동기화: 탐지 후 불변. */

	struct dw_pcie_rp	pp;
	/* [한국어] 루트 포트(RC 모드) 상태. 값으로 박혀 있어 to_dw_pcie_from_pp() 이
	 * container_of 로 역참조한다.
	 * 설정자: SoC 글루가 probe 에서 일부를 채우고, pcie-designware-host.c 가 나머지를 채운다.
	 * 읽는 자: 호스트 경로 전부.
	 * 값 범위: EP 모드로만 쓰는 컨트롤러에서는 손대지 않은 채 남는다.
	 * 동기화: 그 안의 lock 필드가 설정 공간 접근을 지킨다. */

	struct dw_pcie_ep	ep;
	/* [한국어] 엔드포인트(EP 모드) 상태. 마찬가지로 값으로 박혀 있고
	 * to_dw_pcie_from_ep() 이 역참조한다.
	 * 설정자: SoC 글루와 pcie-designware-ep.c.
	 * 읽는 자: 엔드포인트 경로 전부.
	 * 값 범위: RC 모드로만 쓰는 컨트롤러에서는 손대지 않은 채 남는다.
	 * 동기화: EPC 코어의 epc->lock 이 콜백 경로를 직렬화한다. */

	const struct dw_pcie_ops *ops;
	/* [한국어] 위에서 정의한 공통 SoC 훅 묶음.
	 * 설정자: SoC 글루 probe.
	 * 읽는 자: DBI 접근자, 링크 제어 인라인들. 모든 훅 호출 전에 ops 자체와
	 *   개별 훅의 NULL 을 함께 검사한다 — ops 를 아예 두지 않는 글루도 있기 때문이다.
	 * 값 범위: NULL 허용.
	 * 동기화: 불변. */

	u32			version;
	/* [한국어] DWC IP 의 버전 번호. 같은 IP 라도 세대마다 레지스터 배치와 동작이
	 * 조금씩 달라, 그 차이를 가르는 근거가 된다.
	 * 설정자: dw_pcie_version_detect() 가 전용 레지스터에서 읽는다. 그 레지스터가
	 *   없는 구형에서는 pci-keystone.c 처럼 글루가 미리 넣어 두기도 하는데,
	 *   그때 탐지 결과와 다르면 경고를 낸다.
	 * 읽는 자: iATU 방식 선택 등 버전별 분기.
	 * 값 범위: 0 이면 탐지 실패나 미설정.
	 * 동기화: 탐지 후 불변. */

	u32			type;
	/* [한국어] IP 의 종류 코드. 버전과 함께 같은 레지스터에서 읽는다.
	 * 설정자: dw_pcie_version_detect(). 이미 값이 있는데 다르면 경고를 낸다.
	 * 읽는 자: 버전과 함께 쿼크 판정에 쓰인다.
	 * 값 범위: IP 가 보고하는 코드.
	 * 동기화: 탐지 후 불변. */

	unsigned long		caps;
	/* [한국어] 이 컨트롤러의 성질 비트맵. DW_PCIE_CAP_* 열거값을 비트 번호로 쓴다.
	 * 왜 비트맵인가: 성질이 계속 늘어나는데 bool 을 하나씩 두면 구조체가 커지고,
	 *   dw_pcie_cap_is()/dw_pcie_cap_set() 매크로로 이름을 붙여 다루면 읽기도 쉽다.
	 * 설정자: dw_pcie_iatu_detect() 가 IATU_UNROLL 을,
	 *   SoC 글루가 CDM_CHECK 같은 자기 성질을 dw_pcie_cap_set() 으로 세운다.
	 * 읽는 자: dw_pcie_cap_is() 를 쓰는 모든 분기.
	 * 값 범위: DW_PCIE_CAP_* 비트들의 조합.
	 * 동기화: set_bit/test_bit 이 원자적이지만, 실제로는 초기화 중에만 쓰인다. */

	int			num_lanes;
	/* [한국어] 이 링크가 쓸 레인 수.
	 * 설정자: dw_pcie_get_resources() 가 디바이스 트리의 "num-lanes" 를 읽거나
	 *   글루가 직접 넣는다.
	 * 읽는 자: dw_pcie_setup() 이 링크 폭 레지스터를 프로그래밍할 때.
	 * 값 범위: 1, 2, 4, 8, 16. 0 이면 하드웨어 기본값을 그대로 둔다.
	 * 동기화: 초기화 후 불변. */

	int			max_link_speed;
	/* [한국어] 광고할 최대 링크 속도(PCIe 세대).
	 * 설정자: 디바이스 트리의 "max-link-speed" 또는 글루.
	 * 읽는 자: dw_pcie_setup() 이 링크 능력의 속도 필드를 제한할 때.
	 * 값 범위: 1~6(Gen1~Gen6). 0 이면 제한하지 않는다.
	 * 동기화: 초기화 후 불변. */

	u8			n_fts[2];
	/* [한국어] N_FTS(Number of Fast Training Sequences) — L0s 저전력 상태에서 깨어날 때
	 * 링크를 다시 맞추려고 보내는 훈련 시퀀스의 개수. 값이 작으면 복구가 빠르지만
	 * 신호가 나쁜 보드에서는 실패하고, 크면 안전하지만 지연이 늘어난다.
	 * 두 칸인 것은 속도 구간별로 값을 달리 두기 위해서다 — 글루들이 [0] 과 [1] 에
	 * 서로 다른 값을 넣는다(pcie-intel-gw.c 는 [1] 에 Gen3/Gen4 용 값을 넣는다).
	 * 설정자: SoC 글루(pcie-artpec6.c, pcie-tegra194.c, pcie-intel-gw.c 등).
	 * 읽는 자: dw_pcie_setup().
	 * 값 범위: 0 이면 하드웨어 기본값을 쓴다.
	 * 동기화: 초기화 후 불변. */

	struct dw_edma_chip	edma;
	/* [한국어] IP 에 내장된 DMA 엔진(eDMA)을 커널 dmaengine 하위 시스템에 등록하기 위한 서술자.
	 * 왜 있는가: 엔드포인트가 호스트 메모리와 큰 데이터를 주고받을 때 CPU 복사 대신
	 *   이 엔진을 쓰면 훨씬 빠르다.
	 * 설정자: dw_pcie_get_resources() 가 reg_base 를 정하고(별도 리소스가 없으면
	 *   atu_base 에 고정 오프셋을 더한다), dw_pcie_edma_detect() 가 채널 수 등을 탐지해 등록한다.
	 * 읽는 자: dw_pcie_edma_remove() 와 dmaengine 코어.
	 * 값 범위: reg_base 가 NULL 이면 eDMA 없음.
	 * 동기화: 등록 후에는 dmaengine 코어가 관리한다. */

	bool			l1ss_support;	/* L1 PM Substates support */
	/* [한국어] 이 플랫폼이 L1 PM Substates(L1.1/L1.2 초저전력 상태)를 실제로 지원하는지.
	 * 왜 명시가 필요한가: IP 가 능력을 광고해도 보드의 클럭 요청 배선이나 전원 설계가
	 *   받쳐 주지 않으면 그 상태에서 깨어나지 못한다. 그래서 기본은 미지원으로 두고,
	 *   확인된 플랫폼만 참으로 표시한다. 거짓이면 dw_pcie_hide_unsupported_l1ss() 가
	 *   그 능력을 설정 공간에서 감춰 호스트가 쓰지 못하게 한다.
	 * 설정자: SoC 글루(pcie-tegra194.c, pcie-dw-rockchip.c 가 조건부로 true).
	 * 읽는 자: pcie-designware.c 의 L1SS 처리.
	 * 값 범위: true/false. 기본 false.
	 * 동기화: 초기화 후 불변. */

	struct clk_bulk_data	app_clks[DW_PCIE_NUM_APP_CLKS];
	/* [한국어] 애플리케이션 쪽 클럭들(코어 로직이 아니라 버스 인터페이스 쪽).
	 * bulk API 를 쓰는 것은 이름이 정해진 여러 클럭을 한 번에 얻고 켜고 끄기 위해서다.
	 * 설정자: dw_pcie_get_resources() 가 부르는 정적 헬퍼 dw_pcie_get_clocks() 가
	 *   정해진 이름들을 채우고 일괄 획득한다.
	 * 읽는 자: 서스펜드/리줌과 초기화의 clk_bulk_prepare_enable 계열.
	 * 값 범위: 배열 크기는 고정이고, 없는 클럭은 선택적으로 처리된다.
	 * 동기화: 클럭 프레임워크가 자체 잠금을 갖는다. */

	struct clk_bulk_data	core_clks[DW_PCIE_NUM_CORE_CLKS];
	/* [한국어] 코어 쪽 클럭들(PIPE, 참조 클럭 등).
	 * 설정자/읽는 자: app_clks 와 같다.
	 * 값 범위: 배열 크기 고정.
	 * 동기화: 클럭 프레임워크. */

	struct reset_control_bulk_data	app_rsts[DW_PCIE_NUM_APP_RSTS];
	/* [한국어] 애플리케이션 쪽 리셋 라인들. 클럭과 같은 이유로 bulk API 를 쓴다.
	 * 설정자: dw_pcie_get_resources() → dw_pcie_get_resets().
	 * 읽는 자: 초기화와 리줌에서 리셋을 걸고 푸는 코드.
	 * 값 범위: 배열 크기 고정.
	 * 동기화: 리셋 프레임워크. */

	struct reset_control_bulk_data	core_rsts[DW_PCIE_NUM_CORE_RSTS];
	/* [한국어] 코어 쪽 리셋 라인들.
	 * 설정자/읽는 자: app_rsts 와 같다.
	 * 값 범위: 배열 크기 고정.
	 * 동기화: 리셋 프레임워크. */

	struct gpio_desc		*pe_rst;
	/* [한국어] PERST# 신호를 내보내는 GPIO. 루트 포트가 아래에 붙은 장치를 근본적으로
	 * 리셋하는 선이다. 규약상 리셋을 푼 뒤 100ms 를 기다려야 해서,
	 * 이것을 쓰는 글루(pcie-rcar-gen4.c)가 그 지연을 넣는다.
	 * 설정자: dw_pcie_get_resources() 가 부르는 정적 헬퍼 dw_pcie_get_resets() 안의
	 *   devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH). 이름이 "reset" 인 것은
	 *   디바이스 트리 바인딩이 그렇게 정해져 있어서이고, 초기값 HIGH 는 곧 리셋을
	 *   건 상태로 시작한다는 뜻이다.
	 * 읽는 자: 글루의 gpiod_set_value_cansleep.
	 * 값 범위: NULL 이면 그 보드에 PERST# GPIO 가 없다는 뜻이라 정상이다.
	 * 동기화: GPIO 프레임워크. */

	bool			suspended;
	/* [한국어] 이 컨트롤러가 서스펜드 상태인지.
	 * 왜 필요한가: 리줌 때 무엇을 다시 해야 하는지가 서스펜드 때 무엇을 껐느냐에
	 *   달려 있어, 그 사실을 기억해야 한다.
	 * 설정자: dw_pcie_suspend_noirq() 가 true, dw_pcie_resume_noirq() 가 false.
	 * 읽는 자: 같은 두 함수. (글루의 pcie-qcom.c 는 이 필드가 아니라 자기 구조체의
	 *   동명 필드를 따로 둔다 — 혼동하지 말 것.)
	 * 값 범위: true/false.
	 * 동기화: 전원 관리 코어가 직렬화한다. */

	struct debugfs_info	*debugfs;
	/* [한국어] 이 컨트롤러의 debugfs 계층 기록. 위 struct debugfs_info 참고.
	 * 설정자: dwc_pcie_debugfs_init().
	 * 읽는 자: RAS DES 핸들러 전부, dwc_pcie_debugfs_deinit().
	 * 값 범위: NULL 이면 debugfs 초기화 전이거나 실패한 상태다.
	 *   dwc_pcie_debugfs_deinit() 이 그 경우를 먼저 걸러낸다.
	 * 동기화: 초기화 후 불변. */

	enum			dw_pcie_device_mode mode;
	/* [한국어] 이 컨트롤러가 RC 로 도는지 EP 로 도는지.
	 * 왜 기억하는가: PTM debugfs 파일 중 어떤 것이 의미 있는지가 역할에 따라 갈린다 —
	 *   요청자(EP)에는 t1/t4 와 마스터 시계가, 응답자(RC)에는 t2/t3 와 문맥 유효 표시가
	 *   있어야 한다. 가시성 콜백들이 이 필드 하나만 보고 답한다.
	 * 설정자: dwc_pcie_debugfs_init() 이 호출자가 넘긴 mode 를 넣는다. 반드시
	 *   pcie_ptm_create_debugfs() 호출 앞이어야 한다.
	 * 읽는 자: pcie-designware-debugfs.c 의 여덟 가시성 콜백.
	 * 값 범위: DW_PCIE_RC_TYPE 또는 DW_PCIE_EP_TYPE.
	 * 동기화: 초기화 후 불변. */

	u16			ptm_vsec_offset;
	/* [한국어] PTM 벤더 확장 블록이 설정 공간의 어디에서 시작하는지.
	 * 설정자: dw_pcie_ptm_check_capability() 콜백이 유일하게 채운다.
	 * 읽는 자: pcie-designware-debugfs.c 의 모든 PTM 읽기/쓰기 콜백이 이 값에
	 *   레지스터 오프셋을 더해 접근한다.
	 * 값 범위: 0 이면 PTM 미지원이고, 그때 PTM 계층이 파일을 하나도 만들지 않는다.
	 * 동기화: 초기화 후 불변. */

	struct pci_ptm_debugfs	*ptm_debugfs;
	/* [한국어] PCI 코어의 PTM debugfs 계층이 만들어 돌려준 핸들.
	 * 왜 보관하는가: 그 계층이 별도의 최상위 디렉토리(pcie_ptm_<이름>)를 만들므로,
	 *   우리 디렉토리를 재귀 삭제해도 걷히지 않는다. 해제하려면 이 핸들이 필요하다.
	 * 설정자: dwc_pcie_debugfs_init() 의 pcie_ptm_create_debugfs().
	 * 읽는 자: dwc_pcie_debugfs_deinit() 의 pcie_ptm_destroy_debugfs().
	 * 값 범위: NULL 이면 PTM 이 없거나 생성이 실패한 것이며, destroy 쪽이 그것을 받아 준다.
	 * 동기화: 초기화 후 불변. */

	/*
	 * If iATU input addresses are offset from CPU physical addresses,
	 * we previously required .cpu_addr_fixup() to convert them.  We
	 * now rely on the devicetree instead.  If .cpu_addr_fixup()
	 * exists, we compare its results with devicetree.
	 *
	 * If .cpu_addr_fixup() does not exist, we assume the offset is
	 * zero and warn if devicetree claims otherwise.  If we know all
	 * devicetrees correctly describe the offset, set
	 * use_parent_dt_ranges to true to avoid this warning.
	 */
	bool			use_parent_dt_ranges;
	/* [한국어] 부모 노드의 디바이스 트리 ranges 를 그대로 믿어도 되는 플랫폼인지.
	 * 왜 필요한가: 위 영어 주석이 설명하듯, 예전에는 cpu_addr_fixup 훅으로 주소 차이를
	 *   변환했지만 지금은 디바이스 트리를 정식 근거로 삼는다. 훅이 없는 플랫폼에서
	 *   디바이스 트리가 0 이 아닌 오프셋을 말하면 "정말 맞느냐" 는 경고를 내는데,
	 *   그 서술이 옳다고 확인된 플랫폼은 이 값을 참으로 두어 경고를 끈다.
	 * 설정자: 확인을 마친 SoC 글루(pcie-andes-qilai.c, pci-imx6.c, pcie-intel-gw.c 등).
	 * 읽는 자: dw_pcie_parent_bus_offset() 의 경고 판단.
	 * 값 범위: true/false. 기본 false.
	 * 동기화: probe 에서 한 번 쓰이고 이후 불변. */
};

/* [한국어] 루트 포트 상태에서 그것을 품은 컨트롤러로 거슬러 올라가는 매크로.
 * pp 가 dw_pcie 안에 값으로 박혀 있기에 성립하며, 호스트 쪽 콜백들이
 * struct dw_pcie_rp 만 받으므로 이 역참조가 늘 필요하다. */
#define to_dw_pcie_from_pp(port) container_of((port), struct dw_pcie, pp)

/* [한국어] 엔드포인트 상태에서 컨트롤러로 거슬러 올라가는 매크로.
 * pcie-designware-ep.c 의 거의 모든 함수가 첫 줄에서 이것을 부른다 —
 * EPC 콜백은 dw_pcie_ep 만 넘겨 받는데 DBI 접근과 iATU 는 dw_pcie 를 요구하기 때문이다. */
#define to_dw_pcie_from_ep(endpoint)   \
		container_of((endpoint), struct dw_pcie, ep)

/* [한국어] 디바이스 트리와 플랫폼 리소스에서 RC/EP 공통 자원을 모두 확보한다 —
 * DBI/DBI2/iATU/ELBI 창의 ioremap, 클럭과 리셋 일괄 획득, PERST# GPIO,
 * num-lanes 같은 속성. 호스트와 엔드포인트 초기화가 가장 먼저 부른다. */
int dw_pcie_get_resources(struct dw_pcie *pci);

/* [한국어] IP 의 버전과 종류를 전용 레지스터에서 읽어 pci->version/type 에 넣는다.
 * 글루가 미리 값을 넣어 두었는데 탐지 결과와 다르면 경고를 남긴다. */
void dw_pcie_version_detect(struct dw_pcie *pci);

/* [한국어] 이 컨트롤러 자신의 설정 공간에서 표준 capability 의 오프셋을 찾는다.
 * 커널의 pci_find_capability() 는 struct pci_dev 를 요구하는데, 여기서 보려는 것은
 * 아직 어느 버스에도 열거되지 않은 자기 자신이라 쓸 수 없다. */
u8 dw_pcie_find_capability(struct dw_pcie *pci, u8 cap);
/* [한국어] 같은 이유의 확장 capability 판. 0x100 부터 시작하는 확장 영역을 훑는다. */
u16 dw_pcie_find_ext_capability(struct dw_pcie *pci, u8 cap);
/* [한국어] 표준 capability 하나를 리스트에서 떼어 내 보이지 않게 만든다.
 * 하드웨어가 광고하지만 이 플랫폼에서는 실제로 쓸 수 없는 기능을 감출 때 쓴다.
 * 이 트리의 호출자는 pcie-designware-host.c 로, 루트 포트에서 MSI/MSI-X 능력을 뗀다. */
void dw_pcie_remove_capability(struct dw_pcie *pci, u8 cap);
/* [한국어] 확장 capability 를 리스트에서 떼어 낸다. 이 트리에서 실제로 부르는 곳은
 * pcie-dw-rockchip.c 의 엔드포인트 초기화이며, 지원하지 않는 ATS 능력을 없앤다.
 * 주의: dw_pcie_hide_unsupported_l1ss() 는 이 함수를 쓰지 않는다 —
 * 그쪽은 capability 를 떼지 않고 L1SS CAP 안의 지원 비트만 지운다. */
void dw_pcie_remove_ext_capability(struct dw_pcie *pci, u8 cap);
/* [한국어] RAS DES 벤더 확장의 위치를 찾는다. 벤더마다 VSEC ID 가 달라 표를 넘겨 찾으며,
 * 반환값이 pcie-designware-debugfs.c 의 모든 레지스터 접근의 기준점이 된다. */
u16 dw_pcie_find_rasdes_capability(struct dw_pcie *pci);
/* [한국어] PTM 벤더 확장의 위치를 찾는다. dw_pcie_ptm_check_capability() 콜백이
 * 이것을 불러 pci->ptm_vsec_offset 을 채운다. */
u16 dw_pcie_find_ptm_capability(struct dw_pcie *pci);

/* [한국어] 정렬을 지키며 iomem 을 size 바이트만큼 읽는 저수준 헬퍼.
 * 반환값이 PCIBIOS_* 코드인 것은 PCI 설정 공간 접근의 관례를 따르기 때문이다. */
int dw_pcie_read(void __iomem *addr, int size, u32 *val);
/* [한국어] 위의 쓰기 짝. */
int dw_pcie_write(void __iomem *addr, int size, u32 val);

/* [한국어] DBI 창에서 size 바이트를 읽는다. 글루의 read_dbi 훅이 있으면 그쪽으로,
 * 없으면 dw_pcie_read() 로 간다. 아래 인라인 접근자들이 모두 이것으로 모인다. */
u32 dw_pcie_read_dbi(struct dw_pcie *pci, u32 reg, size_t size);
/* [한국어] DBI 창 쓰기. BAR 주소나 벤더 ID 처럼 '본체' 쪽이 이 경로다. */
void dw_pcie_write_dbi(struct dw_pcie *pci, u32 reg, size_t size, u32 val);
/* [한국어] DBI2 창 쓰기. 같은 오프셋이라도 이쪽으로 쓰면 BAR 마스크(크기)가 된다. */
void dw_pcie_write_dbi2(struct dw_pcie *pci, u32 reg, size_t size, u32 val);
/* [한국어] 링크가 올라와 있는지 판정한다. 글루의 link_up 훅이 있으면 그것을 쓰고,
 * 없으면 LTSSM 상태가 L0 계열인지로 판단한다. */
bool dw_pcie_link_up(struct dw_pcie *pci);
/* [한국어] 링크 폭 자동 재협상(upconfigure)을 켠다. 일부 레인이 죽어 좁게 붙었다가
 * 나중에 넓힐 수 있게 하는 설정이다. */
void dw_pcie_upconfig_setup(struct dw_pcie *pci);
/* [한국어] 링크가 올라올 때까지 폴링하며 기다린다. 시간 안에 안 붙으면 오류를 돌려주고,
 * 그때 LTSSM 상태를 함께 찍어 어느 단계에서 멈췄는지 알려 준다. */
int dw_pcie_wait_for_link(struct dw_pcie *pci);
/* [한국어] 실제로 협상된 최대 링크 폭을 읽는다. */
int dw_pcie_link_get_max_link_width(struct dw_pcie *pci);
/* [한국어] 아웃바운드 창을 프로그래밍한다 — 우리가 로컬 주소에 접근하면 그것이
 * 호스트/버스 쪽으로 나가는 TLP 가 되게 만든다. base 는 알아볼 로컬 범위
 * (atu->parent_bus_addr), target 은 옮겨 갈 곳(atu->pci_addr)이다.
 * 인자가 많아 구조체로 묶어 넘긴다. */
int dw_pcie_prog_outbound_atu(struct dw_pcie *pci,
			      const struct dw_pcie_ob_atu_cfg *atu);
/* [한국어] 인바운드 창을 주소 매칭 방식으로 프로그래밍한다. 아웃바운드와 base/target 이
 * 반대다 — 여기서 알아볼 범위(base)는 들어오는 pci_addr 쪽이고, 옮겨 갈 곳(target)이
 * 로컬 parent_bus_addr 이다. 엔드포인트에서는 호스트가 BAR 주소를 배정한 뒤에만
 * 쓸 수 있어, dw_pcie_ep_ib_atu_addr() 의 조각 매핑 경로가 이것을 쓴다. */
int dw_pcie_prog_inbound_atu(struct dw_pcie *pci, int index, int type,
			     u64 parent_bus_addr, u64 pci_addr, u64 size);
/* [한국어] 인바운드 창을 BAR 매칭 방식으로 프로그래밍한다. 주소 대신 BAR 번호로
 * 매칭하도록 PCIE_ATU_BAR_MODE_ENABLE 을 세우는 것이 위 함수와의 결정적 차이다.
 * 엔드포인트의 BAR 주소는 호스트가 열거하며 정하므로 소프트웨어가 미리 알 수 없는데,
 * BAR 번호로 매칭하면 호스트가 무슨 주소를 주든 그 BAR 로 온 트랜잭션이 우리 버퍼로 떨어진다. */
int dw_pcie_prog_ep_inbound_atu(struct dw_pcie *pci, u8 func_no, int index,
				int type, u64 parent_bus_addr,
				u8 bar, size_t size);
/* [한국어] 창 하나를 끈다. dir 로 인바운드/아웃바운드를 가른다
 * (PCIE_ATU_REGION_DIR_IB / _OB). 창을 끄기만 하고 비트맵 반납은 호출자 몫이다. */
void dw_pcie_disable_atu(struct dw_pcie *pci, u32 dir, int index);
/* [한국어] 보드가 실제로 L1 PM Substates 를 지원하지 않을 때, L1SS 확장 capability 의
 * CAP 레지스터에서 L1.1/L1.2 지원 비트를 지워 그 상태를 광고하지 않게 한다.
 * capability 구조체 자체를 떼지는 않는다는 점이 dw_pcie_remove_ext_capability() 와 다르다.
 * pci->l1ss_support 가 참이면 아무것도 하지 않는다. */
void dw_pcie_hide_unsupported_l1ss(struct dw_pcie *pci);
/* [한국어] RC/EP 공통의 포트 논리 설정 — 링크 폭, 최대 속도, N_FTS 등을 프로그래밍한다.
 * 다기능 엔드포인트에서는 함수 0 만 건드리므로, 나머지 함수의 링크 능력은
 * dw_pcie_ep_init_non_sticky_registers() 가 따로 복사해 맞춘다. */
void dw_pcie_setup(struct dw_pcie *pci);
/* [한국어] iATU 가 구형 뷰포트 방식인지 신형 unrolled 방식인지 판별하고,
 * 인바운드/아웃바운드 창 개수와 정렬 단위·범위 상한을 알아낸다. 레지스터로 알려 주지
 * 않는 값이라 실제로 써 보고 되읽는 방식으로 탐지한다. */
void dw_pcie_iatu_detect(struct dw_pcie *pci);
/* [한국어] 내장 DMA 엔진이 있는지 탐지해 dmaengine 하위 시스템에 등록한다.
 * 없으면 조용히 성공으로 돌아간다. */
int dw_pcie_edma_detect(struct dw_pcie *pci);
/* [한국어] 위 등록을 되돌린다. 근본적 리셋 뒤 정리(dw_pcie_ep_cleanup)와
 * 초기화 실패 되감기에서 불린다. */
void dw_pcie_edma_remove(struct dw_pcie *pci);
/* [한국어] CPU 물리 주소와 컨트롤러가 붙은 부모 버스 주소의 차이를 구한다.
 * reg_name 은 어느 리소스를 기준으로 잴지("dbi", "atu", "addr_space")를 고르는 이름이고,
 * cpu_phy_addr 은 그 리소스의 CPU 쪽 주소다. 결과가 pci->parent_bus_offset 이 되어
 * 이후 모든 창 프로그래밍에서 주소 보정에 쓰인다. */
resource_size_t dw_pcie_parent_bus_offset(struct dw_pcie *pci,
					  const char *reg_name,
					  resource_size_t cpu_phy_addr);

/* [한국어]
 * dw_pcie_writel_dbi - DBI 창에 4바이트를 쓴다
 *
 * @pci: 대상 컨트롤러.
 * @reg: DBI 창 시작에서 잰 레지스터 오프셋.
 * @val: 쓸 값.
 * @return: 없음.
 *
 * 폭별 접근자 중 가장 많이 쓰이는 것. 아래 세 쌍(l/w/b)은 전부 크기 인자만 다른
 * 얇은 포장이며, 실제 분기(글루 훅이 있으면 그쪽으로, 없으면 직접 접근)는
 * dw_pcie_write_dbi() 안에서 한 번만 일어난다. 폭을 이름으로 고정해 두면
 * 호출부에서 크기를 손으로 넘길 일이 없어 실수가 줄어든다.
 *
 * 실행 컨텍스트: 어디서든. 잠들지 않는다.
 *
 * 호출 체인:
 *   DWC 드라이버 전반 → [이 함수] → dw_pcie_write_dbi()
 */
static inline void dw_pcie_writel_dbi(struct dw_pcie *pci, u32 reg, u32 val)
{
	dw_pcie_write_dbi(pci, reg, 0x4, val);
}

/* [한국어]
 * dw_pcie_readl_dbi - DBI 창에서 4바이트를 읽는다
 *
 * @pci: 대상 컨트롤러.
 * @reg: DBI 창 시작에서 잰 레지스터 오프셋.
 * @return: 읽은 값.
 *
 * 읽고-고치고-쓰기 패턴의 앞쪽 절반으로 가장 자주 쓰인다. DBI 는 자기 자신의
 * 설정 공간이라, 이 함수로 읽는 값에는 우리가 쓴 것뿐 아니라 호스트가 열거하며
 * 써 넣은 것(BAR 주소, MSI 목적지 등)도 들어 있다.
 *
 * 실행 컨텍스트: 어디서든. 잠들지 않는다.
 *
 * 호출 체인:
 *   DWC 드라이버 전반 → [이 함수] → dw_pcie_read_dbi()
 */
static inline u32 dw_pcie_readl_dbi(struct dw_pcie *pci, u32 reg)
{
	return dw_pcie_read_dbi(pci, reg, 0x4);
}

/* [한국어]
 * dw_pcie_writew_dbi - DBI 창에 2바이트를 쓴다
 *
 * @pci: 대상 컨트롤러.
 * @reg: 레지스터 오프셋.
 * @val: 쓸 값.
 * @return: 없음.
 *
 * 2바이트 폭이 필요한 것은 PCI 설정 공간의 필드 폭이 규약으로 정해져 있어서다 —
 * 벤더 ID, 디바이스 ID, 서브시스템 ID, MSI 의 Message Control 이 모두 word 다.
 * 옆 필드를 건드리지 않으려면 그 폭 그대로 접근해야 한다.
 *
 * 실행 컨텍스트: 어디서든. 잠들지 않는다.
 *
 * 호출 체인:
 *   dw_pcie_ep_write_header() 등 → [이 함수] → dw_pcie_write_dbi()
 */
static inline void dw_pcie_writew_dbi(struct dw_pcie *pci, u32 reg, u16 val)
{
	dw_pcie_write_dbi(pci, reg, 0x2, val);
}

/* [한국어]
 * dw_pcie_readw_dbi - DBI 창에서 2바이트를 읽는다
 *
 * @pci: 대상 컨트롤러.
 * @reg: 레지스터 오프셋.
 * @return: 읽은 값.
 *
 * MSI/MSI-X 의 Message Control 처럼 word 폭 필드를 읽을 때 쓴다.
 *
 * 실행 컨텍스트: 어디서든. 잠들지 않는다.
 *
 * 호출 체인:
 *   MSI 설정/발사 경로 → [이 함수] → dw_pcie_read_dbi()
 */
static inline u16 dw_pcie_readw_dbi(struct dw_pcie *pci, u32 reg)
{
	return dw_pcie_read_dbi(pci, reg, 0x2);
}

/* [한국어]
 * dw_pcie_writeb_dbi - DBI 창에 1바이트를 쓴다
 *
 * @pci: 대상 컨트롤러.
 * @reg: 레지스터 오프셋.
 * @val: 쓸 값.
 * @return: 없음.
 *
 * 리비전 ID, 클래스 코드의 프로그래밍 인터페이스 바이트, 캐시 라인 크기,
 * INTx 핀 번호처럼 1바이트 필드에 쓴다.
 *
 * 실행 컨텍스트: 어디서든. 잠들지 않는다.
 *
 * 호출 체인:
 *   dw_pcie_ep_write_header() 등 → [이 함수] → dw_pcie_write_dbi()
 */
static inline void dw_pcie_writeb_dbi(struct dw_pcie *pci, u32 reg, u8 val)
{
	dw_pcie_write_dbi(pci, reg, 0x1, val);
}

/* [한국어]
 * dw_pcie_readb_dbi - DBI 창에서 1바이트를 읽는다
 *
 * @pci: 대상 컨트롤러.
 * @reg: 레지스터 오프셋.
 * @return: 읽은 값.
 *
 * 대표적인 쓰임이 헤더 타입 확인이다 — dw_pcie_ep_init_registers() 가 이것으로
 * IP 가 정말 엔드포인트 모드로 스트랩됐는지 확인한다.
 *
 * 실행 컨텍스트: 어디서든. 잠들지 않는다.
 *
 * 호출 체인:
 *   dw_pcie_ep_init_registers() 등 → [이 함수] → dw_pcie_read_dbi()
 */
static inline u8 dw_pcie_readb_dbi(struct dw_pcie *pci, u32 reg)
{
	return dw_pcie_read_dbi(pci, reg, 0x1);
}

/* [한국어]
 * dw_pcie_writel_dbi2 - DBI2 창에 4바이트를 쓴다(BAR 마스크 쪽)
 *
 * @pci: 대상 컨트롤러.
 * @reg: 레지스터 오프셋. DBI 쪽과 같은 값을 쓴다.
 * @val: 쓸 값.
 * @return: 없음.
 *
 * 같은 오프셋인데 DBI 로 쓰면 BAR 주소가, DBI2 로 쓰면 BAR 마스크(크기)가 된다.
 * 두 값을 한 자리에 둘 수 없어 IP 가 같은 레지스터를 두 창으로 노출한 결과다.
 * 읽기 짝이 없는 것은 마스크를 되읽을 일이 없기 때문이다.
 *
 * 실행 컨텍스트: 어디서든. 잠들지 않는다.
 *
 * 호출 체인:
 *   엔드포인트 BAR 설정 → [이 함수] → dw_pcie_write_dbi2()
 */
static inline void dw_pcie_writel_dbi2(struct dw_pcie *pci, u32 reg, u32 val)
{
	dw_pcie_write_dbi2(pci, reg, 0x4, val);
}

/* [한국어]
 * dw_pcie_read_cfg_byte - capability 탐색 매크로에 끼워 넣을 1바이트 읽기 어댑터
 *
 * @pci: 대상 컨트롤러.
 * @where: 설정 공간 오프셋.
 * @val: 읽은 값을 담아 돌려줄 곳.
 * @return: 언제나 PCIBIOS_SUCCESSFUL.
 *
 * PCI 코어의 PCI_FIND_NEXT_CAP() 매크로는 첫 인자를 접두사로 삼아
 * <접두사>_byte / _word / _dword 라는 이름을 토큰 결합으로 만들어 부른다.
 * 그 규약에 맞추려고 존재하는 어댑터다 — 실제 하는 일은 DBI 읽기 하나이고,
 * 서명만 코어가 요구하는 "오프셋을 받고 출력 인자에 담고 상태 코드를 돌려주는" 형태다.
 *
 * DBI 읽기는 실패할 수 없으므로 언제나 성공을 돌려준다. 반면 진짜 설정 공간 접근은
 * 장치가 없으면 실패할 수 있어, 코어 매크로가 그 반환값을 검사하는 것이다.
 *
 * 실행 컨텍스트: 초기화 중. 잠들지 않는다.
 *
 * 호출 체인:
 *   dw_pcie_find_capability() → PCI_FIND_NEXT_CAP() → [이 함수] → dw_pcie_readb_dbi()
 */
static inline int dw_pcie_read_cfg_byte(struct dw_pcie *pci, int where,
					u8 *val)
{
	*val = dw_pcie_readb_dbi(pci, where);
	return PCIBIOS_SUCCESSFUL;
}

/* [한국어]
 * dw_pcie_read_cfg_word - 탐색 매크로용 2바이트 읽기 어댑터
 *
 * @pci: 대상 컨트롤러.
 * @where: 설정 공간 오프셋.
 * @val: 읽은 값을 담아 돌려줄 곳.
 * @return: 언제나 PCIBIOS_SUCCESSFUL.
 *
 * 표준 capability 리스트를 훑을 때 ID(하위 바이트)와 Next(상위 바이트)를 word 로
 * 한 번에 읽는 데 쓰인다 — 두 번 읽을 것을 한 번으로 줄이는 최적화다.
 *
 * 실행 컨텍스트: 초기화 중. 잠들지 않는다.
 *
 * 호출 체인:
 *   dw_pcie_find_capability() → PCI_FIND_NEXT_CAP() → [이 함수] → dw_pcie_readw_dbi()
 */
static inline int dw_pcie_read_cfg_word(struct dw_pcie *pci, int where,
					u16 *val)
{
	*val = dw_pcie_readw_dbi(pci, where);
	return PCIBIOS_SUCCESSFUL;
}

/* [한국어]
 * dw_pcie_read_cfg_dword - 탐색 매크로용 4바이트 읽기 어댑터
 *
 * @pci: 대상 컨트롤러.
 * @where: 설정 공간 오프셋.
 * @val: 읽은 값을 담아 돌려줄 곳.
 * @return: 언제나 PCIBIOS_SUCCESSFUL.
 *
 * 확장 capability 의 헤더가 4바이트라, PCI_FIND_NEXT_EXT_CAP() 은 이 폭만 쓴다.
 *
 * 실행 컨텍스트: 초기화 중. 잠들지 않는다.
 *
 * 호출 체인:
 *   dw_pcie_find_ext_capability() → PCI_FIND_NEXT_EXT_CAP() → [이 함수] → dw_pcie_readl_dbi()
 */
static inline int dw_pcie_read_cfg_dword(struct dw_pcie *pci, int where,
					 u32 *val)
{
	*val = dw_pcie_readl_dbi(pci, where);
	return PCIBIOS_SUCCESSFUL;
}

/* [한국어]
 * dw_pcie_ep_get_dbi_offset - 특정 함수의 DBI 창이 어디서 시작하는지 묻는다
 *
 * @ep: 엔드포인트 인스턴스.
 * @func_no: 대상 PCI 함수 번호.
 * @return: 그 함수의 DBI 기준 오프셋. 훅이 없으면 0.
 *
 * 다기능 엔드포인트에서는 함수마다 별도의 설정 공간이 있고, 그것들이 DBI 창 안에
 * 어떤 간격으로 늘어서는지는 IP 통합 방식에 달려 있다. 공용 코드가 알 수 없어
 * SoC 글루의 훅에 묻는다.
 *
 * 훅이 없으면 0 을 돌려주는데, 그러면 모든 함수가 같은 자리를 보게 된다 —
 * 단일 함수 엔드포인트에서는 그것이 정답이므로 안전한 기본값이다.
 *
 * 실행 컨텍스트: 어디서든. 잠들지 않는다.
 *
 * 호출 체인:
 *   dw_pcie_ep_read_dbi()/write_dbi() → [이 함수] → ep->ops->get_dbi_offset()
 */
static inline unsigned int dw_pcie_ep_get_dbi_offset(struct dw_pcie_ep *ep,
						     u8 func_no)
{
	unsigned int dbi_offset = 0;

	/* [한국어] 글루가 함수별 DBI 간격을 알려 주는 훅을 제공하는지 */
	if (ep->ops->get_dbi_offset)
		/* [한국어] 제공하면 그 값을 쓴다. 없으면 0 이 그대로 남아 모든 함수가 같은 자리를 본다 —
		 * 단일 함수 엔드포인트에서는 그것이 정답이다 */
		dbi_offset = ep->ops->get_dbi_offset(ep, func_no);

	return dbi_offset;
}

/* [한국어]
 * dw_pcie_ep_read_dbi - 함수 번호를 반영해 DBI 를 읽는다
 *
 * @ep: 엔드포인트 인스턴스.
 * @func_no: 대상 함수 번호.
 * @reg: 그 함수의 설정 공간 안에서의 오프셋.
 * @size: 읽을 바이트 수(1/2/4).
 * @return: 읽은 값.
 *
 * 함수별 오프셋을 앞에 더해 주는 것이 dw_pcie_read_dbi() 와의 유일한 차이다.
 * 그 한 단계 덕에 상위 코드는 "함수 3 의 MSI 능력" 처럼 논리적인 주소만 다루면 된다.
 *
 * 함수 인덱스를 붙이지 않는 표준 접근자를 써야 하는 자리도 있다 — PTM 능력처럼
 * 컨트롤러 단위인 레지스터가 그렇고, dw_pcie_ep_init_registers() 가 그것을 명시한다.
 *
 * 실행 컨텍스트: 어디서든. 잠들지 않는다.
 *
 * 호출 체인:
 *   dw_pcie_ep_readl_dbi() 등 → [이 함수] → dw_pcie_ep_get_dbi_offset(), dw_pcie_read_dbi()
 */
static inline u32 dw_pcie_ep_read_dbi(struct dw_pcie_ep *ep, u8 func_no,
				      u32 reg, size_t size)
{
	unsigned int offset = dw_pcie_ep_get_dbi_offset(ep, func_no);
	/* [한국어] DBI 읽기 함수가 dw_pcie 를 요구하므로 container_of 로 되찾는다 */
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);

	return dw_pcie_read_dbi(pci, offset + reg, size);
}

/* [한국어]
 * dw_pcie_ep_write_dbi - 함수 번호를 반영해 DBI 에 쓴다
 *
 * @ep: 엔드포인트 인스턴스.
 * @func_no: 대상 함수 번호.
 * @reg: 그 함수의 설정 공간 안에서의 오프셋.
 * @size: 쓸 바이트 수(1/2/4).
 * @val: 쓸 값.
 * @return: 없음.
 *
 * 읽기 짝과 대칭이다. 여기서 쓰는 필드들은 대부분 호스트 관점 읽기 전용이라,
 * 호출부가 앞뒤로 dw_pcie_dbi_ro_wr_en()/dis() 를 감아 주어야 실제로 값이 들어간다.
 *
 * 실행 컨텍스트: 어디서든. 잠들지 않는다.
 *
 * 호출 체인:
 *   dw_pcie_ep_writel_dbi() 등 → [이 함수] → dw_pcie_ep_get_dbi_offset(), dw_pcie_write_dbi()
 */
static inline void dw_pcie_ep_write_dbi(struct dw_pcie_ep *ep, u8 func_no,
					u32 reg, size_t size, u32 val)
{
	unsigned int offset = dw_pcie_ep_get_dbi_offset(ep, func_no);
	/* [한국어] 쓰기 쪽도 마찬가지로 dw_pcie 를 되찾는다 */
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);

	dw_pcie_write_dbi(pci, offset + reg, size, val);
}

/* [한국어]
 * dw_pcie_ep_writel_dbi - 특정 함수의 설정 공간에 4바이트를 쓴다
 *
 * @ep: 엔드포인트 인스턴스. @func_no: 대상 함수 번호.
 * @reg: 그 함수 설정 공간 안의 오프셋. @val: 쓸 값. @return: 없음.
 *
 * BAR 본체, MSI-X 의 Table/PBA 오프셋, Resizable BAR 의 CAP/CTRL 처럼
 * dword 폭 레지스터에 쓴다. 폭만 고정한 얇은 포장이다.
 *
 * 실행 컨텍스트: 어디서든. 잠들지 않는다.
 *
 * 호출 체인:
 *   pcie-designware-ep.c → [이 함수] → dw_pcie_ep_write_dbi()
 */
static inline void dw_pcie_ep_writel_dbi(struct dw_pcie_ep *ep, u8 func_no,
					 u32 reg, u32 val)
{
	dw_pcie_ep_write_dbi(ep, func_no, reg, 0x4, val);
}

/* [한국어]
 * dw_pcie_ep_readl_dbi - 특정 함수의 설정 공간에서 4바이트를 읽는다
 *
 * @ep: 엔드포인트 인스턴스. @func_no: 대상 함수 번호.
 * @reg: 그 함수 설정 공간 안의 오프셋. @return: 읽은 값.
 *
 * 호스트가 배정한 BAR 주소를 되읽거나, MSI 목적지 주소·MSI-X 테이블 위치·
 * REBAR 제어 워드를 읽을 때 쓴다.
 *
 * 실행 컨텍스트: 어디서든. 잠들지 않는다.
 *
 * 호출 체인:
 *   pcie-designware-ep.c → [이 함수] → dw_pcie_ep_read_dbi()
 */
static inline u32 dw_pcie_ep_readl_dbi(struct dw_pcie_ep *ep, u8 func_no,
				       u32 reg)
{
	return dw_pcie_ep_read_dbi(ep, func_no, reg, 0x4);
}

/* [한국어]
 * dw_pcie_ep_writew_dbi - 특정 함수의 설정 공간에 2바이트를 쓴다
 *
 * @ep: 엔드포인트 인스턴스. @func_no: 대상 함수 번호.
 * @reg: 오프셋. @val: 쓸 값. @return: 없음.
 *
 * 벤더/디바이스 ID, 서브시스템 ID, MSI/MSI-X 의 Message Control 이 word 폭이다.
 * 그 옆 필드를 뭉개지 않으려면 반드시 이 폭으로 접근해야 한다.
 *
 * 실행 컨텍스트: 어디서든. 잠들지 않는다.
 *
 * 호출 체인:
 *   dw_pcie_ep_write_header()/set_msi()/set_msix() → [이 함수] → dw_pcie_ep_write_dbi()
 */
static inline void dw_pcie_ep_writew_dbi(struct dw_pcie_ep *ep, u8 func_no,
					 u32 reg, u16 val)
{
	dw_pcie_ep_write_dbi(ep, func_no, reg, 0x2, val);
}

/* [한국어]
 * dw_pcie_ep_readw_dbi - 특정 함수의 설정 공간에서 2바이트를 읽는다
 *
 * @ep: 엔드포인트 인스턴스. @func_no: 대상 함수 번호.
 * @reg: 오프셋. @return: 읽은 값.
 *
 * MSI/MSI-X 의 Message Control 을 읽어 호스트가 인터럽트를 켰는지, 몇 개를
 * 허락했는지, 주소가 64비트인지를 판단하는 데 쓰인다.
 *
 * 실행 컨텍스트: 어디서든. 잠들지 않는다.
 *
 * 호출 체인:
 *   dw_pcie_ep_get_msi()/raise_msi_irq() 등 → [이 함수] → dw_pcie_ep_read_dbi()
 */
static inline u16 dw_pcie_ep_readw_dbi(struct dw_pcie_ep *ep, u8 func_no,
				       u32 reg)
{
	return dw_pcie_ep_read_dbi(ep, func_no, reg, 0x2);
}

/* [한국어]
 * dw_pcie_ep_writeb_dbi - 특정 함수의 설정 공간에 1바이트를 쓴다
 *
 * @ep: 엔드포인트 인스턴스. @func_no: 대상 함수 번호.
 * @reg: 오프셋. @val: 쓸 값. @return: 없음.
 *
 * 리비전 ID, 클래스 코드의 프로그래밍 인터페이스, 캐시 라인 크기, INTx 핀 번호가
 * 1바이트 필드다.
 *
 * 실행 컨텍스트: 어디서든. 잠들지 않는다.
 *
 * 호출 체인:
 *   dw_pcie_ep_write_header() → [이 함수] → dw_pcie_ep_write_dbi()
 */
static inline void dw_pcie_ep_writeb_dbi(struct dw_pcie_ep *ep, u8 func_no,
					 u32 reg, u8 val)
{
	dw_pcie_ep_write_dbi(ep, func_no, reg, 0x1, val);
}

/* [한국어]
 * dw_pcie_ep_readb_dbi - 특정 함수의 설정 공간에서 1바이트를 읽는다
 *
 * @ep: 엔드포인트 인스턴스. @func_no: 대상 함수 번호.
 * @reg: 오프셋. @return: 읽은 값.
 *
 * 이 파일 안에서는 아래 dw_pcie_ep_read_cfg_byte() 어댑터가 유일한 호출자이고,
 * 그것을 통해 capability 리스트 탐색에 쓰인다.
 *
 * 실행 컨텍스트: 어디서든. 잠들지 않는다.
 *
 * 호출 체인:
 *   dw_pcie_ep_read_cfg_byte() → [이 함수] → dw_pcie_ep_read_dbi()
 */
static inline u8 dw_pcie_ep_readb_dbi(struct dw_pcie_ep *ep, u8 func_no,
				      u32 reg)
{
	return dw_pcie_ep_read_dbi(ep, func_no, reg, 0x1);
}

/* [한국어]
 * dw_pcie_ep_read_cfg_byte - 함수별 capability 탐색용 1바이트 읽기 어댑터
 *
 * @ep: 엔드포인트 인스턴스. @func_no: 대상 함수 번호.
 * @where: 설정 공간 오프셋. @val: 읽은 값을 담아 돌려줄 곳.
 * @return: 언제나 PCIBIOS_SUCCESSFUL.
 *
 * dw_pcie_ep_find_capability() 가 PCI_FIND_NEXT_CAP() 매크로에 dw_pcie_ep_read_cfg 를
 * 접두사로 넘기면, 매크로가 토큰 결합으로 이 함수(와 _word/_dword)를 만들어 부른다.
 * 뒤에 붙는 (ep, func_no) 가 매크로의 args... 로 흘러 이 함수의 앞 두 인자가 된다 —
 * 그래서 함수별 설정 공간을 대상으로 같은 탐색 논리를 그대로 재사용할 수 있다.
 *
 * 실행 컨텍스트: 초기화 중. 잠들지 않는다.
 *
 * 호출 체인:
 *   dw_pcie_ep_find_capability() → PCI_FIND_NEXT_CAP() → [이 함수] → dw_pcie_ep_readb_dbi()
 */
static inline int dw_pcie_ep_read_cfg_byte(struct dw_pcie_ep *ep, u8 func_no,
					   int where, u8 *val)
{
	*val = dw_pcie_ep_readb_dbi(ep, func_no, where);
	return PCIBIOS_SUCCESSFUL;
}

/* [한국어]
 * dw_pcie_ep_read_cfg_word - 함수별 capability 탐색용 2바이트 읽기 어댑터
 *
 * @ep: 엔드포인트 인스턴스. @func_no: 대상 함수 번호.
 * @where: 설정 공간 오프셋. @val: 읽은 값을 담아 돌려줄 곳.
 * @return: 언제나 PCIBIOS_SUCCESSFUL.
 *
 * 표준 capability 항목의 ID 와 Next 를 한 번에 읽는 데 쓰인다.
 *
 * 실행 컨텍스트: 초기화 중. 잠들지 않는다.
 *
 * 호출 체인:
 *   dw_pcie_ep_find_capability() → PCI_FIND_NEXT_CAP() → [이 함수] → dw_pcie_ep_readw_dbi()
 */
static inline int dw_pcie_ep_read_cfg_word(struct dw_pcie_ep *ep, u8 func_no,
					   int where, u16 *val)
{
	*val = dw_pcie_ep_readw_dbi(ep, func_no, where);
	return PCIBIOS_SUCCESSFUL;
}

/* [한국어]
 * dw_pcie_ep_read_cfg_dword - 함수별 확장 capability 탐색용 4바이트 읽기 어댑터
 *
 * @ep: 엔드포인트 인스턴스. @func_no: 대상 함수 번호.
 * @where: 설정 공간 오프셋. @val: 읽은 값을 담아 돌려줄 곳.
 * @return: 언제나 PCIBIOS_SUCCESSFUL.
 *
 * 확장 capability 헤더가 4바이트라 PCI_FIND_NEXT_EXT_CAP() 은 이 폭만 쓴다.
 * 엔드포인트에서는 Resizable BAR 을 찾는 데 쓰인다.
 *
 * 실행 컨텍스트: 초기화/set_bar 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   dw_pcie_ep_find_ext_capability() → PCI_FIND_NEXT_EXT_CAP() → [이 함수]
 *     → dw_pcie_ep_readl_dbi()
 */
static inline int dw_pcie_ep_read_cfg_dword(struct dw_pcie_ep *ep, u8 func_no,
					    int where, u32 *val)
{
	*val = dw_pcie_ep_readl_dbi(ep, func_no, where);
	return PCIBIOS_SUCCESSFUL;
}

/* [한국어]
 * dw_pcie_ep_get_dbi2_offset - 특정 함수의 DBI2 창이 어디서 시작하는지 묻는다
 *
 * @ep: 엔드포인트 인스턴스.
 * @func_no: 대상 함수 번호.
 * @return: 그 함수의 DBI2 기준 오프셋. 어떤 훅도 없으면 0.
 *
 * DBI 쪽과 같은 역할이지만 대체 경로가 하나 더 있다. 전용 get_dbi2_offset 훅이
 * 있으면 그것을 쓰고, 없으면 원문 주석대로 하위 호환을 위해 get_dbi_offset 훅을
 * 대신 쓴다 — 두 창의 함수별 간격이 같은 IP 가 많고, 예전에는 훅이 하나뿐이었기 때문이다.
 * 간격이 다른 IP 를 만나면서 전용 훅이 나중에 생겼고, 기존 글루를 고치지 않아도
 * 동작하도록 이 순서를 둔 것이다.
 *
 * 실행 컨텍스트: 어디서든. 잠들지 않는다.
 *
 * 호출 체인:
 *   dw_pcie_ep_write_dbi2() → [이 함수] → ep->ops->get_dbi2_offset()/get_dbi_offset()
 */
static inline unsigned int dw_pcie_ep_get_dbi2_offset(struct dw_pcie_ep *ep,
						      u8 func_no)
{
	unsigned int dbi2_offset = 0;

	/* [한국어] DBI2 전용 오프셋 훅이 있으면 그것이 우선이다 */
	if (ep->ops->get_dbi2_offset)
		/* [한국어] 두 창의 함수별 간격이 다른 IP 를 위해 나중에 추가된 훅이다 */
		dbi2_offset = ep->ops->get_dbi2_offset(ep, func_no);
	/* [한국어] 전용 훅이 없으면 DBI 쪽 훅으로 물러난다 — 원문 주석대로 하위 호환용이다 */
	else if (ep->ops->get_dbi_offset)     /* for backward compatibility */
		/* [한국어] 두 창의 간격이 같은 IP 에서는 이 대체가 정확히 맞는다 */
		dbi2_offset = ep->ops->get_dbi_offset(ep, func_no);

	return dbi2_offset;
}

/* [한국어]
 * dw_pcie_ep_write_dbi2 - 함수 번호를 반영해 DBI2(BAR 마스크)에 쓴다
 *
 * @ep: 엔드포인트 인스턴스. @func_no: 대상 함수 번호.
 * @reg: 오프셋. @size: 바이트 수. @val: 쓸 값. @return: 없음.
 *
 * DBI 쪽 짝과 구조가 같지만 오프셋 계산이 dw_pcie_ep_get_dbi2_offset() 을 거치고
 * 최종 접근이 dw_pcie_write_dbi2() 로 간다. 읽기 짝이 없는 것은 BAR 마스크를
 * 되읽을 일이 없어서다.
 *
 * 실행 컨텍스트: 어디서든. 잠들지 않는다.
 *
 * 호출 체인:
 *   dw_pcie_ep_writel_dbi2() → [이 함수] → dw_pcie_ep_get_dbi2_offset(), dw_pcie_write_dbi2()
 */
static inline void dw_pcie_ep_write_dbi2(struct dw_pcie_ep *ep, u8 func_no,
					 u32 reg, size_t size, u32 val)
{
	unsigned int offset = dw_pcie_ep_get_dbi2_offset(ep, func_no);
	/* [한국어] DBI2 쓰기 함수가 dw_pcie 를 요구하므로 되찾는다 */
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);

	dw_pcie_write_dbi2(pci, offset + reg, size, val);
}

/* [한국어]
 * dw_pcie_ep_writel_dbi2 - 특정 함수의 BAR 마스크에 4바이트를 쓴다
 *
 * @ep: 엔드포인트 인스턴스. @func_no: 대상 함수 번호.
 * @reg: BAR 레지스터 오프셋(DBI 쪽과 같은 값). @val: 쓸 값. @return: 없음.
 *
 * 엔드포인트 BAR 설정의 핵심 도구다. 여기에 (크기 - 1) 을 쓰면 그 0 비트 개수가
 * 곧 호스트에게 보이는 BAR 크기가 되고, 0 을 쓰면 그 BAR 이 사라진다.
 * Resizable BAR 만은 예외로 BIT(0) 만 써서 활성화 비트만 세운다 — 크기는
 * CAP 레지스터가 정하기 때문이다.
 *
 * 실행 컨텍스트: set_bar/clear_bar 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   dw_pcie_ep_set_bar_programmable()/_resizable()/__dw_pcie_ep_reset_bar()
 *     → [이 함수] → dw_pcie_ep_write_dbi2()
 */
static inline void dw_pcie_ep_writel_dbi2(struct dw_pcie_ep *ep, u8 func_no,
					  u32 reg, u32 val)
{
	dw_pcie_ep_write_dbi2(ep, func_no, reg, 0x4, val);
}

/* [한국어]
 * dw_pcie_dbi_ro_wr_en - 읽기 전용 설정 공간 필드에 잠시 쓸 수 있게 만든다
 *
 * @pci: 대상 컨트롤러.
 * @return: 없음.
 *
 * PCI 규약상 벤더 ID, 클래스 코드, BAR 마스크, 링크 능력 같은 필드는 호스트가
 * 쓸 수 없는 읽기 전용이다. 그런데 그 값을 정하는 것은 장치 쪽 소프트웨어이므로,
 * 우리에게는 쓸 방법이 있어야 한다. DWC 는 그 통로로 잠금 비트를 하나 두었고,
 * 이 함수가 그것을 연다.
 *
 * 반드시 dw_pcie_dbi_ro_wr_dis() 와 짝지어 써야 한다. 열어 둔 채로 두면 실수로
 * 정체성 필드를 덮어쓸 수 있고, 실행 중 호스트가 보는 값이 흔들릴 수 있다.
 * 이 파일의 정책은 필요한 쓰기 묶음만 감싸고 곧바로 닫는 것이다.
 *
 * 잠금 비트 자체가 읽고-고치고-쓰기라 원자적이지 않은데, 중첩 호출을 세는 장치도
 * 없다. 즉 두 경로가 동시에 이것을 쓰면 한쪽이 먼저 닫아 버릴 수 있다 —
 * 실제로는 초기화와 EPC 콜백(epc->lock 아래)에서만 쓰여 문제가 드러나지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 잠들지 않는다.
 *
 * 호출 체인:
 *   BAR/헤더/MSI 설정 코드 → [이 함수] → dw_pcie_readl_dbi(), dw_pcie_writel_dbi()
 */
static inline void dw_pcie_dbi_ro_wr_en(struct dw_pcie *pci)
{
	u32 reg;
	/* [한국어] 읽고 고쳐 쓸 레지스터 값 */
	u32 val;

	/* [한국어] 잠금 비트가 들어 있는 기타 제어 레지스터 */
	reg = PCIE_MISC_CONTROL_1_OFF;
	/* [한국어] 현재 값을 읽는다. 같은 레지스터의 다른 제어 비트를 지키려면 통째로 쓸 수 없다 */
	val = dw_pcie_readl_dbi(pci, reg);
	/* [한국어] 읽기 전용 필드 쓰기 허용 비트를 세운다 */
	val |= PCIE_DBI_RO_WR_EN;
	dw_pcie_writel_dbi(pci, reg, val);
}

/* [한국어]
 * dw_pcie_dbi_ro_wr_dis - 읽기 전용 필드 쓰기 허용을 되돌린다
 *
 * @pci: 대상 컨트롤러.
 * @return: 없음.
 *
 * 위 함수의 정확한 역이며 같은 비트를 내린다. 짝이 맞지 않아 열린 채로 남으면
 * 이후의 실수 있는 쓰기가 그대로 반영되므로, 오류 반환 경로에서도 반드시
 * 이 함수를 거쳐 나가야 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 잠들지 않는다.
 *
 * 호출 체인:
 *   BAR/헤더/MSI 설정 코드 → [이 함수] → dw_pcie_readl_dbi(), dw_pcie_writel_dbi()
 */
static inline void dw_pcie_dbi_ro_wr_dis(struct dw_pcie *pci)
{
	u32 reg;
	/* [한국어] 읽고 고쳐 쓸 레지스터 값 */
	u32 val;

	/* [한국어] 켤 때와 같은 레지스터 */
	reg = PCIE_MISC_CONTROL_1_OFF;
	/* [한국어] 현재 값을 읽는다 */
	val = dw_pcie_readl_dbi(pci, reg);
	/* [한국어] 허용 비트를 내려 다시 잠근다 */
	val &= ~PCIE_DBI_RO_WR_EN;
	dw_pcie_writel_dbi(pci, reg, val);
}

/* [한국어]
 * dw_pcie_start_link - 링크 트레이닝 시작을 SoC 훅에 위임한다
 *
 * @pci: 대상 컨트롤러.
 * @return: 훅의 결과. 훅이 없으면 0.
 *
 * 링크를 올리는 절차는 PHY 제어와 SoC 전용 레지스터가 얽혀 있어 공용 코드가
 * 대신할 수 없다. 그래서 훅으로 넘기고, 여기서는 NULL 검사만 한다.
 *
 * 훅이 없을 때 0(성공)을 돌려주는 것이 중요한 규약이다. 링크가 다른 방법으로
 * 이미 올라오는 플랫폼(부트로더가 올려 두었거나 하드웨어가 자동으로 하는 경우)에서
 * 초기화가 실패로 끝나지 않게 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 훅 안에서 잠들 수 있다.
 *
 * 호출 체인:
 *   dw_pcie_ep_start() / 호스트 초기화 → [이 함수] → pci->ops->start_link()
 */
static inline int dw_pcie_start_link(struct dw_pcie *pci)
{
	if (pci->ops && pci->ops->start_link)
		/* [한국어] 훅이 있으면 링크 시작을 그쪽에 맡긴다. 실제 절차는 PHY 제어와 SoC 전용
		 * 레지스터가 얽혀 있어 공용 코드가 대신할 수 없다 */
		return pci->ops->start_link(pci);

	return 0;
}

/* [한국어]
 * dw_pcie_stop_link - 링크 정지를 SoC 훅에 위임한다
 *
 * @pci: 대상 컨트롤러.
 * @return: 없음.
 *
 * start_link 의 역이며 마찬가지로 NULL 검사만 한다. 훅이 없으면 아무것도 하지
 * 않는데, 링크를 내릴 수단이 없는 플랫폼에서는 그것이 유일하게 옳은 동작이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 훅 안에서 잠들 수 있다.
 *
 * 호출 체인:
 *   dw_pcie_ep_stop() / 호스트 해제 → [이 함수] → pci->ops->stop_link()
 */
static inline void dw_pcie_stop_link(struct dw_pcie *pci)
{
	if (pci->ops && pci->ops->stop_link)
		pci->ops->stop_link(pci);
}

/* [한국어]
 * dw_pcie_get_ltssm - 링크 트레이닝 상태 기계의 현재 상태를 읽는다
 *
 * @pci: 대상 컨트롤러.
 * @return: LTSSM 상태 열거값.
 *
 * 위 두 함수와 달리 기본 구현이 있다. SoC 훅이 있으면 그것을 쓰고, 없으면
 * 표준 포트 디버그 레지스터(PCIE_PORT_DEBUG0)에서 상태 필드를 뽑는다.
 * 훅이 필요한 이유는 일부 IP 통합이 이 레지스터를 노출하지 않거나 다른 자리에
 * 두기 때문이다.
 *
 * 두 쓰임이 있다. 하나는 링크가 올라왔는지 판정하는 dw_pcie_link_up() 의 근거이고,
 * 다른 하나는 debugfs 의 ltssm_status 파일이 사람에게 보여 주는 값이다 —
 * 링크가 안 붙을 때 어느 단계에서 멈췄는지가 원인의 실마리가 된다.
 *
 * 실행 컨텍스트: 어디서든. 잠들지 않는다.
 *
 * 호출 체인:
 *   dw_pcie_link_up() / ltssm_status_show() → [이 함수]
 *     → pci->ops->get_ltssm() 또는 dw_pcie_readl_dbi()
 */
static inline enum dw_pcie_ltssm dw_pcie_get_ltssm(struct dw_pcie *pci)
{
	u32 val;

	/* [한국어] 글루가 LTSSM 읽기 훅을 제공하는지. ops 자체가 NULL 인 글루도 있어 함께 검사한다 */
	if (pci->ops && pci->ops->get_ltssm)
		/* [한국어] 제공하면 그것을 쓴다 — 표준 레지스터를 노출하지 않는 IP 통합이 있기 때문 */
		return pci->ops->get_ltssm(pci);

	/* [한국어] 기본 경로: 표준 포트 디버그 레지스터를 읽는다 */
	val = dw_pcie_readl_dbi(pci, PCIE_PORT_DEBUG0);

	return (enum dw_pcie_ltssm)FIELD_GET(PORT_LOGIC_LTSSM_STATE_MASK, val);
}

/* [한국어] LTSSM 상태 값을 사람이 읽을 이름으로 바꾼다. debugfs 의 ltssm_status 와
 * 링크 대기 실패 로그가 쓰며, 표에 없는 값이면 알 수 없음을 나타내는 문자열을 돌려준다. */
const char *dw_pcie_ltssm_status_string(enum dw_pcie_ltssm ltssm);

/* [한국어] 여기서부터 호스트(RC) 전용 API 다. CONFIG_PCIE_DW_HOST 가 꺼져 있으면
 * 아래 #else 쪽의 빈 인라인이 대신 쓰인다. 이렇게 짝을 두는 이유는, 이 헤더를 쓰는
 * SoC 글루가 RC 와 EP 를 한 파일에서 모두 지원하면서도 #ifdef 로 코드를 갈라 쓰지
 * 않아도 되게 하기 위해서다 — 빌드에서 빠진 쪽은 아무것도 하지 않는 함수가 된다. */
#ifdef CONFIG_PCIE_DW_HOST
/* [한국어] 서스펜드 직전(인터럽트가 꺼진 뒤) 링크를 저전력 상태로 넘기고 상태를 기록한다. */
int dw_pcie_suspend_noirq(struct dw_pcie *pci);
/* [한국어] 리줌 때 링크를 되살린다. 서스펜드 때 무엇을 껐느냐에 따라 할 일이 달라
 * pci->suspended 를 근거로 삼는다. */
int dw_pcie_resume_noirq(struct dw_pcie *pci);
/* [한국어] MSI 인터럽트가 도착했을 때 어느 하위 장치의 것인지 가려 해당 IRQ 로 넘긴다.
 * 루트 포트가 MSI 를 받아 커널 IRQ 로 바꾸는 경로의 핵심이다. */
void dw_handle_msi_irq(struct dw_pcie_rp *pp);
/* [한국어] MSI 수신에 쓸 컨트롤러 쪽 설정 — 메시지를 받을 주소를 하드웨어에 알린다. */
void dw_pcie_msi_init(struct dw_pcie_rp *pp);
/* [한국어] MSI 를 받기 위한 IRQ 도메인과 수신 버퍼를 준비한다. */
int dw_pcie_msi_host_init(struct dw_pcie_rp *pp);
/* [한국어] 위에서 잡은 MSI 자원을 되돌린다. */
void dw_pcie_free_msi(struct dw_pcie_rp *pp);
/* [한국어] 루트 컴플렉스로 동작하기 위한 레지스터 설정 — 브리지 헤더, 버스 번호,
 * 창 설정 등. 링크가 내려갔다 올라올 때 다시 부르기도 한다. */
int dw_pcie_setup_rc(struct dw_pcie_rp *pp);
/* [한국어] 호스트 쪽 진입점. 자원 확보부터 PCI 버스 등록까지를 묶는다.
 * SoC 글루의 probe 가 부른다. */
int dw_pcie_host_init(struct dw_pcie_rp *pp);
/* [한국어] 위의 역. remove 경로에서 버스를 걷고 자원을 되돌린다. */
void dw_pcie_host_deinit(struct dw_pcie_rp *pp);
/* [한국어] 이 루트 포트의 IRQ 도메인을 만든다. MSI 를 커널 IRQ 번호로 잇는 뼈대다. */
int dw_pcie_allocate_domains(struct dw_pcie_rp *pp);
/* [한국어] 루트 포트 "자기 자신" 의 설정 공간 접근을 DBI 주소로 매핑한다.
 * 아래에 붙은 장치의 설정 공간은 iATU 창을 거치지만, 루트 포트 자신은 DBI 로 직접 본다. */
void __iomem *dw_pcie_own_conf_map_bus(struct pci_bus *bus, unsigned int devfn,
				       int where);
#else
/* [한국어] 아래는 CONFIG_PCIE_DW_HOST 가 꺼졌을 때의 빈 구현들이다. 반환값은
 * "아무 일도 없었지만 실패는 아니다" 를 뜻하도록 골랐고, 자원이 정말 필요한
 * dw_pcie_msi_host_init() 만 -ENODEV 로 명확히 거절한다. */
/* [한국어] 호스트 지원이 빠진 빌드에서는 서스펜드에서 할 일이 없다. */
static inline int dw_pcie_suspend_noirq(struct dw_pcie *pci)
{
	return 0;
}

/* [한국어] 리줌도 마찬가지로 성공만 알린다. */
static inline int dw_pcie_resume_noirq(struct dw_pcie *pci)
{
	return 0;
}

/* [한국어] 받을 MSI 자체가 없으므로 본문이 비어 있다. */
static inline void dw_handle_msi_irq(struct dw_pcie_rp *pp) { }

/* [한국어] MSI 수신 설정도 할 일이 없다. */
static inline void dw_pcie_msi_init(struct dw_pcie_rp *pp)
{ }

/* [한국어] 이것만 오류를 돌려준다. 호출자가 MSI 자원을 기대하는 자리라,
 * 조용히 성공을 흉내 내면 뒤에서 NULL 참조가 나기 때문이다. */
static inline int dw_pcie_msi_host_init(struct dw_pcie_rp *pp)
{
	return -ENODEV;
}

/* [한국어] 잡은 것이 없으니 풀 것도 없다. */
static inline void dw_pcie_free_msi(struct dw_pcie_rp *pp)
{ }

/* [한국어] RC 설정을 할 이유가 없으므로 성공만 알린다. */
static inline int dw_pcie_setup_rc(struct dw_pcie_rp *pp)
{
	return 0;
}

/* [한국어] 호스트 초기화가 없어도 글루의 probe 가 실패하지 않도록 0 을 돌려준다. */
static inline int dw_pcie_host_init(struct dw_pcie_rp *pp)
{
	return 0;
}

/* [한국어] 초기화가 없었으니 해제도 없다. */
static inline void dw_pcie_host_deinit(struct dw_pcie_rp *pp)
{
}

/* [한국어] IRQ 도메인을 만들지 않아도 성공으로 처리한다. */
static inline int dw_pcie_allocate_domains(struct dw_pcie_rp *pp)
{
	return 0;
}
/* [한국어] 설정 공간 매핑 요청에 NULL 을 돌려준다. PCI 코어는 NULL 을
 * "접근 불가" 로 해석해 0xffffffff 를 읽은 것처럼 다룬다. */
static inline void __iomem *dw_pcie_own_conf_map_bus(struct pci_bus *bus,
						     unsigned int devfn,
						     int where)
{
	return NULL;
}
#endif

/* [한국어] 여기서부터 엔드포인트(EP) 전용 API 다. 구현은 pcie-designware-ep.c 에 있고,
 * CONFIG_PCIE_DW_EP 가 꺼지면 아래 #else 의 빈 인라인이 대신 쓰인다.
 * 호스트 쪽과 다른 점 하나가 눈에 띈다 — 빈 구현의 인터럽트 발사 함수들이
 * 오류가 아니라 0(성공)을 돌려준다. */
#ifdef CONFIG_PCIE_DW_EP
/* [한국어] 링크가 올라왔음을 EPF 드라이버들에게 알린다. SoC 글루가 링크 이벤트를
 * 감지해 부르며, 그것이 pci_epc_linkup() 으로 전파된다. */
void dw_pcie_ep_linkup(struct dw_pcie_ep *ep);
/* [한국어] 링크가 끊겼음을 알린다. 통지 전에 non-sticky 레지스터를 되살리는 일이
 * 함께 들어 있어, 링크가 돌아왔을 때 설정이 남아 있게 한다. */
void dw_pcie_ep_linkdown(struct dw_pcie_ep *ep);
/* [한국어] 엔드포인트 진입점. EPC 디바이스를 만들고 주소 공간과 MSI 용 페이지를
 * 준비한다. 레지스터는 건드리지 않는다 — 참조 클럭이 없어도 되는 단계다. */
int dw_pcie_ep_init(struct dw_pcie_ep *ep);
/* [한국어] 참조 클럭이 살아난 뒤 레지스터를 세운다. init 과 나뉘어 있는 이유가 그것이며,
 * PERST# 를 지원하는 드라이버는 리셋이 풀릴 때마다 다시 부른다. */
int dw_pcie_ep_init_registers(struct dw_pcie_ep *ep);
/* [한국어] init 의 역. 파생 자원 정리, MSI 페이지 반납, 주소 할당자 해제.
 * EPC 디바이스 자체는 devm 이 처리하므로 파괴하지 않는다. */
void dw_pcie_ep_deinit(struct dw_pcie_ep *ep);
/* [한국어] 근본적 리셋 뒤 debugfs 와 eDMA 만 걷어 내는 가벼운 정리.
 * PERST# 를 지원하는 드라이버가 리셋 처리에서 쓴다. */
void dw_pcie_ep_cleanup(struct dw_pcie_ep *ep);
/* [한국어] INTx 를 내려는 시도. 이 공용 계층에서는 낼 수 없어 로그를 남기고
 * -EINVAL 을 돌려준다 — INTx 어서션이 SoC 전용 회로를 거치기 때문이다. */
int dw_pcie_ep_raise_intx_irq(struct dw_pcie_ep *ep, u8 func_no);
/* [한국어] MSI 발사. 호스트가 지정한 주소로 나가는 아웃바운드 창을 한 번 잡아 두고
 * 재사용하며, 그 창에 대응하는 로컬 주소에 4바이트를 쓴다. interrupt_num 은 1-기반. */
int dw_pcie_ep_raise_msi_irq(struct dw_pcie_ep *ep, u8 func_no,
			     u8 interrupt_num);
/* [한국어] MSI-X 발사. 목적지가 BAR 안의 테이블에 있어 그것을 먼저 읽고,
 * 창은 매번 잡았다 푼다. 벡터가 마스크돼 있으면 -EPERM 으로 거절한다. */
int dw_pcie_ep_raise_msix_irq(struct dw_pcie_ep *ep, u8 func_no,
			     u16 interrupt_num);
/* [한국어] MSI-X 를 IP 전용 도어벨 레지스터 한 번 쓰기로 낸다. 창이 필요 없어
 * 훨씬 싸지만 IP 통합 옵션이라 모든 SoC 에 있지는 않다. */
int dw_pcie_ep_raise_msix_irq_doorbell(struct dw_pcie_ep *ep, u8 func_no,
				       u16 interrupt_num);
/* [한국어] 모든 함수에 대해 같은 번호의 BAR 을 꺼서 호스트에게 안 보이게 만든다. */
void dw_pcie_ep_reset_bar(struct dw_pcie *pci, enum pci_barno bar);
/* [한국어] 함수 번호로 per-function 상태를 찾는다. SoC 글루가 자기 raise_irq 훅에서
 * 능력 오프셋을 보려고 부르기도 해서 외부로 노출된다. */
struct dw_pcie_ep_func *
dw_pcie_ep_get_func_from_ep(struct dw_pcie_ep *ep, u8 func_no);
#else
/* [한국어] 아래는 CONFIG_PCIE_DW_EP 가 꺼졌을 때의 빈 구현들이다. */
/* [한국어] 알릴 EPF 가 없으므로 본문이 비어 있다. */
static inline void dw_pcie_ep_linkup(struct dw_pcie_ep *ep)
{
}

/* [한국어] 링크다운 통지도 마찬가지. */
static inline void dw_pcie_ep_linkdown(struct dw_pcie_ep *ep)
{
}

/* [한국어] 엔드포인트 초기화가 없어도 글루의 probe 가 실패하지 않도록 0 을 돌려준다. */
static inline int dw_pcie_ep_init(struct dw_pcie_ep *ep)
{
	return 0;
}

/* [한국어] 레지스터 초기화도 성공으로 처리한다. */
static inline int dw_pcie_ep_init_registers(struct dw_pcie_ep *ep)
{
	return 0;
}

/* [한국어] 잡은 것이 없으니 풀 것도 없다. */
static inline void dw_pcie_ep_deinit(struct dw_pcie_ep *ep)
{
}

/* [한국어] 정리할 파생 자원도 없다. */
static inline void dw_pcie_ep_cleanup(struct dw_pcie_ep *ep)
{
}

/* [한국어] 실제 구현이 -EINVAL 을 돌려주는 것과 달리 여기서는 0 이다.
 * 이 빌드에는 인터럽트를 기다리는 EPF 자체가 없으므로, 글루의 raise_irq 훅이
 * 그대로 성공을 알려도 아무 일도 일어나지 않기 때문이다. */
static inline int dw_pcie_ep_raise_intx_irq(struct dw_pcie_ep *ep, u8 func_no)
{
	return 0;
}

/* [한국어] MSI 발사도 아무 일 없이 성공으로 처리한다. */
static inline int dw_pcie_ep_raise_msi_irq(struct dw_pcie_ep *ep, u8 func_no,
					   u8 interrupt_num)
{
	return 0;
}

/* [한국어] MSI-X 발사도 마찬가지. */
static inline int dw_pcie_ep_raise_msix_irq(struct dw_pcie_ep *ep, u8 func_no,
					   u16 interrupt_num)
{
	return 0;
}

/* [한국어] 도어벨 방식도 마찬가지. */
static inline int dw_pcie_ep_raise_msix_irq_doorbell(struct dw_pcie_ep *ep,
						     u8 func_no,
						     u16 interrupt_num)
{
	return 0;
}

/* [한국어] 끌 BAR 이 없으므로 본문이 비어 있다. */
static inline void dw_pcie_ep_reset_bar(struct dw_pcie *pci, enum pci_barno bar)
{
}

/* [한국어] 함수 리스트가 아예 없으므로 NULL 을 돌려준다. 실제 구현도 못 찾으면
 * NULL 이고 호출자가 그것을 -EINVAL 로 바꾸므로, 같은 규약이 그대로 성립한다. */
static inline struct dw_pcie_ep_func *
dw_pcie_ep_get_func_from_ep(struct dw_pcie_ep *ep, u8 func_no)
{
	return NULL;
}
#endif

/* [한국어] 마지막으로 debugfs 진단 인터페이스. 구현은 pcie-designware-debugfs.c 에 있다.
 * 이 짝 덕분에 host.c 와 ep.c 가 #ifdef 없이 그냥 부를 수 있다 — 진단 기능이
 * 빠진 커널에서는 호출이 통째로 사라진다. */
#ifdef CONFIG_PCIE_DW_DEBUGFS
/* [한국어] 이 컨트롤러의 debugfs 계층(RAS DES 세 갈래 + ltssm_status + PTM)을 만든다.
 * mode 로 RC 인지 EP 인지 알려 주며, 그 값이 PTM 파일의 가시성을 가른다. */
void dwc_pcie_debugfs_init(struct dw_pcie *pci, enum dw_pcie_device_mode mode);
/* [한국어] 위에서 만든 것을 모두 걷는다. PTM 쪽은 별도 최상위 디렉토리라
 * 따로 파괴해야 한다. */
void dwc_pcie_debugfs_deinit(struct dw_pcie *pci);
#else
/* [한국어] debugfs 지원이 없으면 아무 디렉토리도 만들지 않는다. void 반환이라
 * 호출부가 결과를 볼 일도 없어 완전히 사라진다. */
static inline void dwc_pcie_debugfs_init(struct dw_pcie *pci,
					 enum dw_pcie_device_mode mode)
{
}
/* [한국어] 만든 것이 없으니 걷을 것도 없다. */
static inline void dwc_pcie_debugfs_deinit(struct dw_pcie *pci)
{
}
#endif

#endif /* _PCIE_DESIGNWARE_H */
