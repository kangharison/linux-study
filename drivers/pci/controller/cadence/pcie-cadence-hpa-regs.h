/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Cadence PCIe controller driver.
 *
 * Copyright (c) 2024, Cadence Design Systems
 * Author: Manikandan K Pillai <mpillai@cadence.com>
 */
/*
 * [한국어 설명] Cadence PCIe 컨트롤러 HPA 레지스터 맵 (pcie-cadence-hpa-regs.h)
 *
 * === 파일의 역할 ===
 * Cadence PCIe IP 의 **신형 레지스터 배치(HPA, High Performance Architecture)**
 * 에서 쓰는 레지스터 오프셋과 비트필드를 정의하는 순수 헤더다. 실행 코드는
 * 없고 #define 만 있다. 구형 배치를 담은 pcie-cadence-lga-regs.h 와 같은
 * 개념(로컬 관리, BAR 설정, 주소 변환)을 다루지만 **주소도 비트 자리도 전부
 * 다르다** -- 그래서 파일이 분리되어 있고, 드라이버는 cdns_pcie->is_hpa
 * 같은 판별로 두 경로를 갈라 탄다.
 * 가장 큰 구조적 차이는 주소 지정 방식이다. LGA 는 reg_base 로부터의 절대
 * 오프셋(예: 0x00100000)을 쓰지만, HPA 는 **레지스터 뱅크 상대 오프셋**을
 * 쓴다. 즉 여기 적힌 값(예: 0x1420)은 그 자체로는 주소가 아니고,
 * cdns_reg_bank_to_off() 가 돌려주는 뱅크 기준 주소에 더해져야 완성된다.
 * 뱅크 기준 주소는 SoC 마다 다르므로 struct cdns_pcie_rp_reg_offsets 에
 * 담겨 온다(pcie-cadence.h:219 이하).
 *
 * === 전체 아키텍처에서의 위치 ===
 * PCI 컨트롤러 드라이버 계층 중 Cadence 계열의 최하단이다. pcie-cadence.h 가
 * 이 헤더를 포함해 cdns_pcie_hpa_readl/writel 접근자와 함께 노출하고,
 * pcie-cadence.c(공통), pcie-cadence-host-hpa.c(HPA 루트 컴플렉스),
 * pcie-cadence-ep.c 가 그 접근자를 통해 여기 상수를 쓴다.
 * 이 트리에서 HPA 경로를 실제로 타는 SoC 는 pci-sky1.c 하나다. sky1 은
 * 자기 뱅크 기준 주소를 직접 정의해 채운다(SKY1_IP_REG_BANK = 0x1000 등,
 * pci-sky1.c:156 이하). 실행 컨텍스트는 없다 -- 전처리 단계에서만 존재한다.
 *
 * === 타 모듈과의 연결 ===
 * 포함하는 헤더가 다섯 개인 점이 LGA(bitfield.h 하나)와 다르다:
 * kernel.h, pci.h, pci-epf.h, phy/phy.h, bitfield.h. 이 중 실제로 필요한
 * 것은 BAR_3 열거값(pci.h 또는 pci-epf.h)과 GENMASK/FIELD_PREP(bitfield.h)
 * 이며, phy.h 는 이 파일의 어떤 정의에도 쓰이지 않는다(이 파일 안에서 확인).
 * 데이터 흐름은 LGA 와 같다: 드라이버가 논리 값(BAR 번호, 버스 번호,
 * 창 번호)을 넘기면 함수형 매크로가 레지스터 오프셋이나 비트 패턴으로 바꾸고,
 * 접근자가 그것을 뱅크 기준 주소에 더해 MMIO 로 내보낸다.
 * drivers/nvme 는 이 헤더의 어떤 이름도 참조하지 않는다 -- NVMe 는 열거가
 * 끝난 뒤의 PCI 디바이스만 다루므로 호스트 컨트롤러 레지스터와 닿지 않는다
 * (이 트리에서 확인).
 *
 * === 주요 함수/구조체 요약 ===
 * 함수도 구조체도 없다. 정의는 크게 네 묶음이다.
 *  - 뱅크 기준 주소 후보 (IP_REG_BANK, IP_CFG_CTRL_REG_BANK, AXI_* 등):
 *    **이 트리에서 참조하는 곳이 한 군데도 없다.** 유일한 HPA 소비자인 sky1 이
 *    자기 값(0x1000, 0x4c00, 0x9000 ...)을 따로 정의해 쓰기 때문이다. 여기
 *    값들은 0x01000000 대의 훨씬 큰 주소라, 다른 SoC 를 위한 기본값이거나
 *    문서상의 참조로 남은 것으로 보인다.
 *  - BAR 설정 (LM_RC_BAR_CFG, LM_BAR_CFG_CTRL_*): BAR 하나가 10비트를
 *    차지하고, 종류 값은 4비트에 담긴다. LGA(8비트 슬롯, 3비트 종류)와
 *    자릿수부터 다르다.
 *  - 아웃바운드/인바운드 주소 변환 (AT_OB_REGION_*, AT_IB_*): 창 하나가
 *    0x80 바이트를 차지한다(LGA 는 0x20). 최대 창 수도 15개로 LGA 의 32개보다
 *    적다.
 *  - 물리 계층 (PHY_LAYER_CFG0, PHY_DBG_STS_REG0): 링크 학습 파라미터와
 *    디버그 상태. LGA 에서 LM 영역에 있던 LTSSM 설정이 HPA 에서는 물리 계층
 *    레지스터로 옮겨졌다.
 * 값을 자리에 넣을 때 LGA 는 직접 시프트+마스크를 썼지만, HPA 는 상당수를
 * FIELD_PREP() 으로 바꿔 마스크 하나만 주면 시프트가 자동으로 유도되게 했다.
 */
#ifndef _PCIE_CADENCE_HPA_REGS_H
#define _PCIE_CADENCE_HPA_REGS_H

/* [한국어] 일반 커널 매크로용. 이 파일 자체는 특별히 쓰는 것이 없지만
 * Cadence 가 헤더 묶음을 관례적으로 포함해 두었다. */
#include <linux/kernel.h>
/* [한국어] PCI 공통 정의용. 아래 BAR 번호 비교(BAR_3)가 여기 계통의
 * 열거값에 의존한다. */
#include <linux/pci.h>
/* [한국어] enum pci_barno(BAR_0..BAR_5)를 제공한다. BAR_3 비교가 이것을 쓴다. */
#include <linux/pci-epf.h>
/* [한국어] PHY 프레임워크 헤더. 다만 이 파일 안의 어떤 정의도 phy 타입을
 * 쓰지 않는다 -- Cadence 헤더 묶음을 그대로 가져온 흔적으로 보인다. */
#include <linux/phy/phy.h>
/* [한국어] GENMASK/BIT/FIELD_PREP 를 쓰기 위해 포함한다. HPA 는 LGA 와 달리
 * 필드 채우기를 FIELD_PREP 으로 처리하는 곳이 많아 이 헤더가 특히 중요하다. */
#include <linux/bitfield.h>

/* High Performance Architecture (HPA) PCIe controller registers */
/* [한국어] IP 레지스터 뱅크의 기준 주소 후보. **이 트리에서 참조하는 곳이
 * 없다** -- 유일한 HPA 소비자인 pci-sky1.c 가 자기 값 SKY1_IP_REG_BANK(0x1000)를
 * 정의해 reg_off->ip_reg_bank_offset 에 채워 넣기 때문이다(pci-sky1.c:156, :735).
 * 여기 0x01000000 은 다른 SoC 나 문서상의 기본 배치로 보인다. */
#define CDNS_PCIE_HPA_IP_REG_BANK		0x01000000
/* [한국어] IP 설정·제어 레지스터 뱅크의 기준 주소 후보. 마찬가지로 사용처 0.
 * sky1 은 0x4c00 을 쓴다. */
#define CDNS_PCIE_HPA_IP_CFG_CTRL_REG_BANK	0x01003C00
/* [한국어] AXI 마스터 공통 레지스터 뱅크의 기준 주소 후보. 사용처 0. */
#define CDNS_PCIE_HPA_IP_AXI_MASTER_COMMON	0x02020000

/* Address Translation Registers */
/* [한국어] AXI 슬레이브 뱅크의 기준 주소 후보. 사용처 0. sky1 은 0x9000.
 * 슬레이브는 CPU 가 컨트롤러를 향해 거는 쪽, 즉 아웃바운드 창의 관문이다. */
#define CDNS_PCIE_HPA_AXI_SLAVE                 0x03000000
/* [한국어] AXI 마스터 뱅크의 기준 주소 후보. 사용처 0. sky1 은 0xb000.
 * 마스터는 컨트롤러가 시스템 메모리를 향해 거는 쪽, 즉 인바운드 경로다. */
#define CDNS_PCIE_HPA_AXI_MASTER                0x03002000

/* Root Port register base address */
/* [한국어] 루트 포트 레지스터 뱅크의 기준 오프셋(0). 뱅크 변환표에서
 * REG_BANK_RP 가 항상 0 을 돌려주는 것과 짝을 이룬다 -- 루트 포트 설정공간은
 * reg_base 그 자리에서 시작한다. */
#define CDNS_PCIE_HPA_RP_BASE			0x0

/* [한국어] 벤더/서브시스템 ID 레지스터의 뱅크 상대 오프셋. IP 레지스터 뱅크
 * 기준이므로 sky1 에서는 실제 주소가 reg_base + 0x1000 + 0x420 이 된다.
 * 이 상수 자체를 참조하는 곳은 이 트리에 없다. */
#define CDNS_PCIE_HPA_LM_ID			0x1420

/* Endpoint Function BARs */
/* [한국어] 엔드포인트 함수 fn 의 BAR b 설정 레지스터를 고른다. **경계가
 * BAR_3 인 점이 LGA(BAR_4)와 다르다** -- HPA 는 BAR 하나가 10비트를 쓰므로
 * 32비트 레지스터 하나에 3개(30비트)까지만 들어가기 때문이다. */
#define CDNS_PCIE_HPA_LM_EP_FUNC_BAR_CFG(bar, fn) \
	(((bar) < BAR_3) ? CDNS_PCIE_HPA_LM_EP_FUNC_BAR_CFG0(fn) : \
			CDNS_PCIE_HPA_LM_EP_FUNC_BAR_CFG1(fn))
/* [한국어] BAR 0~2 를 담는 레지스터. 물리 함수마다 0x4000 씩 떨어진다. */
#define CDNS_PCIE_HPA_LM_EP_FUNC_BAR_CFG0(pfn) (0x4000 * (pfn))
/* [한국어] BAR 3~5 를 담는 레지스터. CFG0 바로 다음 4바이트. */
#define CDNS_PCIE_HPA_LM_EP_FUNC_BAR_CFG1(pfn) ((0x4000 * (pfn)) + 0x04)
/* [한국어] SR-IOV 가상 함수용 같은 구조. 역시 BAR_3 을 경계로 나뉜다. */
#define CDNS_PCIE_HPA_LM_EP_VFUNC_BAR_CFG(bar, fn) \
	(((bar) < BAR_3) ? CDNS_PCIE_HPA_LM_EP_VFUNC_BAR_CFG0(fn) : \
			CDNS_PCIE_HPA_LM_EP_VFUNC_BAR_CFG1(fn))
/* [한국어] VF 의 BAR 0~2 설정. 같은 0x4000 블록 안에서 0x08 만큼 뒤에 있다. */
#define CDNS_PCIE_HPA_LM_EP_VFUNC_BAR_CFG0(vfn) ((0x4000 * (vfn)) + 0x08)
/* [한국어] VF 의 BAR 3~5 설정. */
#define CDNS_PCIE_HPA_LM_EP_VFUNC_BAR_CFG1(vfn) ((0x4000 * (vfn)) + 0x0C)
/* [한국어] 레지스터 안에서 BAR f 의 aperture(크기 지수) 자리. BAR 하나가
 * 10비트를 차지하고, 그 슬롯 안에서 4비트를 건너뛴 뒤 6비트가 크기다.
 * LGA(8비트 슬롯, 5비트 크기)보다 슬롯도 크기 폭도 넓다. */
#define CDNS_PCIE_HPA_LM_EP_FUNC_BAR_CFG_BAR_APERTURE_MASK(f) \
	(GENMASK(5, 0) << (0x4 + (f) * 10))
/* [한국어] 크기 지수 a 를 BAR b 의 자리로 옮겨 담는다. 시프트 4 + b * 10 이
 * 위 마스크의 0x4 + f * 10 과 같은 값이다. */
#define CDNS_PCIE_HPA_LM_EP_FUNC_BAR_CFG_BAR_APERTURE(b, a) \
	(((a) << (4 + ((b) * 10))) & (CDNS_PCIE_HPA_LM_EP_FUNC_BAR_CFG_BAR_APERTURE_MASK(b)))
/* [한국어] 같은 10비트 슬롯의 **하위** 4비트가 BAR 종류(control) 자리다.
 * LGA 는 종류가 상위 3비트였는데 HPA 는 하위 4비트로 자리와 폭이 모두 바뀌었다. */
#define CDNS_PCIE_HPA_LM_EP_FUNC_BAR_CFG_BAR_CTRL_MASK(f) \
	(GENMASK(3, 0) << ((f) * 10))
/* [한국어] 종류 값 c 를 BAR b 의 자리로 옮겨 담는다. 종류가 슬롯 맨 아래에서
 * 시작하므로 시프트가 b * 10 뿐이다. */
#define CDNS_PCIE_HPA_LM_EP_FUNC_BAR_CFG_BAR_CTRL(b, c) \
	(((c) << ((b) * 10)) & (CDNS_PCIE_HPA_LM_EP_FUNC_BAR_CFG_BAR_CTRL_MASK(b)))

/* Endpoint Function Configuration Register */
/* [한국어] 엔드포인트가 노출할 물리 함수 개수를 정하는 레지스터.
 * 이 상수를 참조하는 곳은 이 트리에 없다. */
#define CDNS_PCIE_HPA_LM_EP_FUNC_CFG		0x02C0

/* Root Complex BAR Configuration Register */
/* [한국어] 루트 컴플렉스 BAR 설정 레지스터. LGA 의 0x0300 과 달리 뱅크
 * 상대 0x14 로, 훨씬 앞쪽에 놓였다. */
#define CDNS_PCIE_HPA_LM_RC_BAR_CFG                        0x14
/* [한국어] RC BAR0 의 크기 지수 자리(비트 4~9). 종류가 아래 4비트를 차지하고
 * 그 위에 크기 6비트가 오는 배치다. */
#define CDNS_PCIE_HPA_LM_RC_BAR_CFG_BAR0_APERTURE_MASK     GENMASK(9, 4)
/* [한국어] FIELD_PREP 으로 값을 그 자리에 넣는다. 마스크만 주면 시프트가
 * 자동으로 유도되므로, LGA 처럼 시프트를 따로 적을 필요가 없다. */
#define CDNS_PCIE_HPA_LM_RC_BAR_CFG_BAR0_APERTURE(a) \
	FIELD_PREP(CDNS_PCIE_HPA_LM_RC_BAR_CFG_BAR0_APERTURE_MASK, a)
/* [한국어] RC BAR0 의 종류 자리(하위 4비트). */
#define CDNS_PCIE_HPA_LM_RC_BAR_CFG_BAR0_CTRL_MASK         GENMASK(3, 0)
/* [한국어] 종류 값을 FIELD_PREP 으로 그 자리에 넣는다. */
#define CDNS_PCIE_HPA_LM_RC_BAR_CFG_BAR0_CTRL(c) \
	FIELD_PREP(CDNS_PCIE_HPA_LM_RC_BAR_CFG_BAR0_CTRL_MASK, c)
/* [한국어] RC BAR1 의 크기 지수 자리(비트 14~19). BAR0 슬롯(0~9)에서 10비트
 * 건너뛴 다음 슬롯이다. */
#define CDNS_PCIE_HPA_LM_RC_BAR_CFG_BAR1_APERTURE_MASK     GENMASK(19, 14)
/* [한국어] BAR1 크기 지수를 넣는다. */
#define CDNS_PCIE_HPA_LM_RC_BAR_CFG_BAR1_APERTURE(a) \
	FIELD_PREP(CDNS_PCIE_HPA_LM_RC_BAR_CFG_BAR1_APERTURE_MASK, a)
/* [한국어] RC BAR1 의 종류 자리(비트 10~13). */
#define CDNS_PCIE_HPA_LM_RC_BAR_CFG_BAR1_CTRL_MASK         GENMASK(13, 10)
/* [한국어] BAR1 종류 값을 넣는다. */
#define CDNS_PCIE_HPA_LM_RC_BAR_CFG_BAR1_CTRL(c) \
	FIELD_PREP(CDNS_PCIE_HPA_LM_RC_BAR_CFG_BAR1_CTRL_MASK, c)

/* [한국어] 프리페치 가능 메모리 창을 켜는 비트. LGA 는 BIT(17)이었다. */
#define CDNS_PCIE_HPA_LM_RC_BAR_CFG_PREFETCH_MEM_ENABLE BIT(20)
/* [한국어] 그 프리페치 창을 64비트로 쓴다는 표시. */
#define CDNS_PCIE_HPA_LM_RC_BAR_CFG_PREFETCH_MEM_64BITS BIT(21)
/* [한국어] IO 창을 켜는 비트. */
#define CDNS_PCIE_HPA_LM_RC_BAR_CFG_IO_ENABLE           BIT(22)
/* [한국어] IO 창을 32비트 주소로 쓴다는 표시. */
#define CDNS_PCIE_HPA_LM_RC_BAR_CFG_IO_32BITS           BIT(23)

/* BAR control values applicable to both Endpoint Function and Root Complex */
/* [한국어] BAR 를 끈 상태. 아래 값들은 엔드포인트와 RC 양쪽에서 같은 의미다.
 * HPA 의 4비트 인코딩은 비트마다 뜻이 나뉘어 있어 규칙적이다:
 *   비트 0 = 창 사용, 비트 1 = IO(0이면 메모리), 비트 2 = 64비트,
 *   비트 3 = 프리페치 가능.
 * 그래서 MEM_32BITS=0b0001, MEM_64BITS=0b0101, PREF_MEM_64BITS=0b1101 이 된다. */
#define CDNS_PCIE_HPA_LM_BAR_CFG_CTRL_DISABLED              0x0
/* [한국어] 32비트 IO 창 (0b0011 = 사용 + IO). */
#define CDNS_PCIE_HPA_LM_BAR_CFG_CTRL_IO_32BITS             0x3
/* [한국어] 32비트 메모리 창 (0b0001). */
#define CDNS_PCIE_HPA_LM_BAR_CFG_CTRL_MEM_32BITS            0x1
/* [한국어] 32비트 프리페치 가능 메모리 창 (0b1001 = 32비트 메모리 + 프리페치 비트). */
#define CDNS_PCIE_HPA_LM_BAR_CFG_CTRL_PREFETCH_MEM_32BITS   0x9
/* [한국어] 64비트 메모리 창 (0b0101 = 메모리 + 64비트 비트). */
#define CDNS_PCIE_HPA_LM_BAR_CFG_CTRL_MEM_64BITS            0x5
/* [한국어] 64비트 프리페치 가능 메모리 창 (0b1101).
 * 
 * 프리페치가 비트 3 하나로 분리되어 있어, pcie-cadence-host-hpa.c:131-139 처럼
 * 기본값(MEM_64BITS)을 먼저 OR 하고 프리페치일 때만 이 값을 더 OR 하는 코드가
 * 두 상태를 실제로 구분해 낸다. 구형 LGA 는 같은 자리 값을 0x6/0x7 로 잡아
 * 0x6 | 0x7 == 0x7 이 되는 바람에 그 구분이 사라진다
 * (pcie-cadence-lga-regs.h 의 CTRL_PREFETCH_MEM_64BITS 주석 참조). */
#define CDNS_PCIE_HPA_LM_BAR_CFG_CTRL_PREFETCH_MEM_64BITS   0xD

/* [한국어] RC BAR 설정 레지스터에서 bar 번호에 맞는 슬롯으로 '끔' 값을 민다.
 * 시프트가 bar * 10 인 것은 슬롯 폭이 10비트이고 종류가 슬롯 맨 아래에서
 * 시작하기 때문이다. LGA 의 bar * 8 + 6 과 대조된다. */
#define HPA_LM_RC_BAR_CFG_CTRL_DISABLED(bar)                \
		(CDNS_PCIE_HPA_LM_BAR_CFG_CTRL_DISABLED << ((bar) * 10))
/* [한국어] 같은 방식으로 32비트 IO 값을 민다. */
#define HPA_LM_RC_BAR_CFG_CTRL_IO_32BITS(bar)               \
		(CDNS_PCIE_HPA_LM_BAR_CFG_CTRL_IO_32BITS << ((bar) * 10))
/* [한국어] 같은 방식으로 32비트 메모리 값을 민다. */
#define HPA_LM_RC_BAR_CFG_CTRL_MEM_32BITS(bar)              \
		(CDNS_PCIE_HPA_LM_BAR_CFG_CTRL_MEM_32BITS << ((bar) * 10))
/* [한국어] 같은 방식으로 32비트 프리페치 메모리 값을 민다. */
#define HPA_LM_RC_BAR_CFG_CTRL_PREF_MEM_32BITS(bar) \
		(CDNS_PCIE_HPA_LM_BAR_CFG_CTRL_PREFETCH_MEM_32BITS << ((bar) * 10))
/* [한국어] 같은 방식으로 64비트 메모리 값을 민다. */
#define HPA_LM_RC_BAR_CFG_CTRL_MEM_64BITS(bar)              \
		(CDNS_PCIE_HPA_LM_BAR_CFG_CTRL_MEM_64BITS << ((bar) * 10))
/* [한국어] 같은 방식으로 64비트 프리페치 메모리 값을 민다. */
#define HPA_LM_RC_BAR_CFG_CTRL_PREF_MEM_64BITS(bar) \
		(CDNS_PCIE_HPA_LM_BAR_CFG_CTRL_PREFETCH_MEM_64BITS << ((bar) * 10))
/* [한국어] 크기 지수를 BAR 슬롯의 aperture 자리에 넣는다. **빼는 값이 7 인 점이
 * LGA(2)와 다르다** -- HPA 레지스터의 0 이 2^7(128바이트)를 뜻하도록 인코딩이
 * 바뀌었기 때문이다. 시프트에 + 4 가 붙는 것은 슬롯 안에서 종류 4비트를
 * 건너뛰기 때문이다. */
#define HPA_LM_RC_BAR_CFG_APERTURE(bar, aperture)           \
		(((aperture) - 7) << (((bar) * 10) + 4))

/* [한국어] PTM(Precision Time Measurement) 제어 레지스터. 뱅크 상대 0x0520. */
#define CDNS_PCIE_HPA_LM_PTM_CTRL		0x0520
/* [한국어] PTM Responder 를 켜는 비트. LGA 와 같은 BIT(17) 이지만 레지스터
 * 주소는 다르다. 이름의 PTM 철자가 LGA 쪽(TPM)과 달리 바로잡혀 있다. */
#define CDNS_PCIE_HPA_LM_PTM_CTRL_PTMRSEN	BIT(17)

/* Root Port Registers PCI config space for root port function */
/* [한국어] 루트 포트 설정공간에서 PCI Express 능력 구조의 오프셋.
 * 이 상수를 참조하는 곳은 이 트리에 없다. */
#define CDNS_PCIE_HPA_RP_CAP_OFFSET	0xC0

/* Region r Outbound AXI to PCIe Address Translation Register 0 */
/* [한국어] 아웃바운드 창 r 의 PCI 쪽 주소 레지스터 0. **창 하나가 0x80 바이트**
 * 를 차지한다(LGA 는 0x20). (r) & 0x1F 로 잘라 32칸까지 접근 가능한 산술이지만,
 * 실제 상한은 아래 CDNS_PCIE_HPA_MAX_OB(15)이다. */
#define CDNS_PCIE_HPA_AT_OB_REGION_PCI_ADDR0(r)            (0x1010 + ((r) & 0x1F) * 0x0080)
/* [한국어] 이 창이 덮는 주소 폭을 담는 자리(하위 6비트). */
#define CDNS_PCIE_HPA_AT_OB_REGION_PCI_ADDR0_NBITS_MASK    GENMASK(5, 0)
/* [한국어] nbits 에서 1 을 빼서 넣는다. 레지스터의 0 이 '1비트' 를 뜻하기
 * 때문으로, LGA 와 같은 규칙이다. */
#define CDNS_PCIE_HPA_AT_OB_REGION_PCI_ADDR0_NBITS(nbits) \
	(((nbits) - 1) & CDNS_PCIE_HPA_AT_OB_REGION_PCI_ADDR0_NBITS_MASK)
/* [한국어] 설정 트랜잭션의 목적지 devfn 자리(비트 16~23). LGA 는 12~19 였다. */
#define CDNS_PCIE_HPA_AT_OB_REGION_PCI_ADDR0_DEVFN_MASK    GENMASK(23, 16)
/* [한국어] FIELD_PREP 으로 devfn 을 그 자리에 넣는다. */
#define CDNS_PCIE_HPA_AT_OB_REGION_PCI_ADDR0_DEVFN(devfn) \
	FIELD_PREP(CDNS_PCIE_HPA_AT_OB_REGION_PCI_ADDR0_DEVFN_MASK, devfn)
/* [한국어] 목적지 버스 번호 자리(비트 24~31). LGA 는 20~27 이었다. */
#define CDNS_PCIE_HPA_AT_OB_REGION_PCI_ADDR0_BUS_MASK      GENMASK(31, 24)
/* [한국어] FIELD_PREP 으로 버스 번호를 그 자리에 넣는다. */
#define CDNS_PCIE_HPA_AT_OB_REGION_PCI_ADDR0_BUS(bus) \
	FIELD_PREP(CDNS_PCIE_HPA_AT_OB_REGION_PCI_ADDR0_BUS_MASK, bus)

/* Region r Outbound AXI to PCIe Address Translation Register 1 */
/* [한국어] 아웃바운드 창 r 의 PCI 쪽 주소 상위 32비트. ADDR0 바로 다음 4바이트. */
#define CDNS_PCIE_HPA_AT_OB_REGION_PCI_ADDR1(r)            (0x1014 + ((r) & 0x1F) * 0x0080)

/* Region r Outbound PCIe Descriptor Register */
/* [한국어] 아웃바운드 창 r 의 서술자 레지스터 0. 이 창의 접근을 어떤 종류의
 * TLP 로 바꿀지 정한다. **주소가 ADDR0(0x1010)보다 앞인 0x1008 인 점에 유의** --
 * 창 블록 안의 배치가 LGA 와 다르다. */
#define CDNS_PCIE_HPA_AT_OB_REGION_DESC0(r)                (0x1008 + ((r) & 0x1F) * 0x0080)
/* [한국어] TLP 종류 자리(비트 24~28). LGA 는 하위 4비트였는데 HPA 는 상위로
 * 옮기고 폭도 5비트로 늘렸다. */
#define CDNS_PCIE_HPA_AT_OB_REGION_DESC0_TYPE_MASK         GENMASK(28, 24)
/* [한국어] 메모리 트랜잭션. 종류 코드 0x0 -- LGA 의 0x2 와 값 자체가 다르다.
 * 여기서는 마스크와 값이 FIELD_PREP 으로 이미 합쳐져 있어, 드라이버는 이
 * 상수를 그대로 OR 하면 된다. */
#define CDNS_PCIE_HPA_AT_OB_REGION_DESC0_TYPE_MEM  \
	FIELD_PREP(CDNS_PCIE_HPA_AT_OB_REGION_DESC0_TYPE_MASK, 0x0)
/* [한국어] IO 트랜잭션. 종류 코드 0x2. */
#define CDNS_PCIE_HPA_AT_OB_REGION_DESC0_TYPE_IO   \
	FIELD_PREP(CDNS_PCIE_HPA_AT_OB_REGION_DESC0_TYPE_MASK, 0x2)
/* [한국어] Type 0 설정 트랜잭션(링크 바로 건너편). 종류 코드 0x4. */
#define CDNS_PCIE_HPA_AT_OB_REGION_DESC0_TYPE_CONF_TYPE0  \
	FIELD_PREP(CDNS_PCIE_HPA_AT_OB_REGION_DESC0_TYPE_MASK, 0x4)
/* [한국어] Type 1 설정 트랜잭션(스위치 너머). 종류 코드 0x5. */
#define CDNS_PCIE_HPA_AT_OB_REGION_DESC0_TYPE_CONF_TYPE1  \
	FIELD_PREP(CDNS_PCIE_HPA_AT_OB_REGION_DESC0_TYPE_MASK, 0x5)
/* [한국어] 일반 메시지 TLP. 종류 코드 0x10 -- 5비트 필드라서 표현 가능한 값이다.
 * LGA 의 4비트 필드로는 담을 수 없었을 값이라, 필드 확장의 이유를 짐작하게 한다. */
#define CDNS_PCIE_HPA_AT_OB_REGION_DESC0_TYPE_NORMAL_MSG  \
	FIELD_PREP(CDNS_PCIE_HPA_AT_OB_REGION_DESC0_TYPE_MASK, 0x10)

/* Region r Outbound PCIe Descriptor Register */
/* [한국어] 아웃바운드 창 r 의 서술자 레지스터 1. 고정 RID 의 버스/devfn 이
 * 여기 담긴다. */
#define CDNS_PCIE_HPA_AT_OB_REGION_DESC1(r)        (0x100C + ((r) & 0x1F) * 0x0080)
/* [한국어] 고정 RID 의 버스 번호 자리(비트 24~31). */
#define CDNS_PCIE_HPA_AT_OB_REGION_DESC1_BUS_MASK  GENMASK(31, 24)
/* [한국어] FIELD_PREP 으로 버스 번호를 그 자리에 넣는다. */
#define CDNS_PCIE_HPA_AT_OB_REGION_DESC1_BUS(bus) \
	FIELD_PREP(CDNS_PCIE_HPA_AT_OB_REGION_DESC1_BUS_MASK, bus)
/* [한국어] 고정 RID 의 devfn 자리(비트 16~23). PCI_ADDR0 쪽과 같은 자릿수라,
 * HPA 에서는 두 레지스터가 같은 배치를 공유한다. */
#define CDNS_PCIE_HPA_AT_OB_REGION_DESC1_DEVFN_MASK    GENMASK(23, 16)
/* [한국어] FIELD_PREP 으로 devfn 을 그 자리에 넣는다. */
#define CDNS_PCIE_HPA_AT_OB_REGION_DESC1_DEVFN(devfn) \
	FIELD_PREP(CDNS_PCIE_HPA_AT_OB_REGION_DESC1_DEVFN_MASK, devfn)

/* [한국어] 아웃바운드 창 r 의 제어 레지스터 0. LGA 에는 대응물이 없고, HPA 가
 * 새로 둔 레지스터다. 창 블록(0x80) 안의 0x18 자리에 있다. */
#define CDNS_PCIE_HPA_AT_OB_REGION_CTRL0(r)         (0x1018 + ((r) & 0x1F) * 0x0080)
/* [한국어] '이 창이 만드는 TLP 의 버스 번호를 DESC1 에 적어 둔 값으로
 * 공급하라' 는 비트. 이것을 세우지 않으면 하드웨어가 다른 출처의 버스 번호를
 * 쓴다. LGA 의 HARDCODED_RID 비트에 해당하는 역할이다. */
#define CDNS_PCIE_HPA_AT_OB_REGION_CTRL0_SUPPLY_BUS BIT(26)
/* [한국어] 같은 뜻의 devfn 쪽 비트.
 * 
 * 두 비트는 항상 함께 쓰인다(pcie-cadence-hpa.c:275, :359,
 * pcie-cadence-host-hpa.c:222). 참고로 host-hpa.c:585 는 같은 레지스터에
 * 리터럴 0x06000000 을 직접 쓰는데, 이는 BIT(26) | BIT(25) 와 정확히 같은 값이다. */
#define CDNS_PCIE_HPA_AT_OB_REGION_CTRL0_SUPPLY_DEV_FN BIT(25)

/* Region r AXI Region Base Address Register 0 */
/* [한국어] 아웃바운드 창 r 의 AXI(CPU) 쪽 기준 주소 레지스터 0. 창 블록의
 * **맨 앞(0x1000)** 이다 -- LGA 가 CPU 주소를 블록 뒤쪽(0x18)에 둔 것과 반대다. */
#define CDNS_PCIE_HPA_AT_OB_REGION_CPU_ADDR0(r)     (0x1000 + ((r) & 0x1F) * 0x0080)
/* [한국어] CPU 쪽 창이 덮는 주소 폭 자리(하위 6비트). */
#define CDNS_PCIE_HPA_AT_OB_REGION_CPU_ADDR0_NBITS_MASK    GENMASK(5, 0)
/* [한국어] PCI 쪽과 같은 규칙으로 nbits 에서 1 을 뺀다. */
#define CDNS_PCIE_HPA_AT_OB_REGION_CPU_ADDR0_NBITS(nbits) \
	(((nbits) - 1) & CDNS_PCIE_HPA_AT_OB_REGION_CPU_ADDR0_NBITS_MASK)

/* Region r AXI Region Base Address Register 1 */
/* [한국어] AXI 쪽 기준 주소의 상위 32비트. */
#define CDNS_PCIE_HPA_AT_OB_REGION_CPU_ADDR1(r)     (0x1004 + ((r) & 0x1F) * 0x0080)

/* Root Port BAR Inbound PCIe to AXI Address Translation Register */
/* [한국어] 루트 포트 BAR bar 의 인바운드(PCIe→AXI) 주소 변환 레지스터 0.
 * 오프셋이 순수하게 bar * 8 뿐인 것은 이 레지스터들이 전용 뱅크
 * (REG_BANK_AXI_* 계열)의 맨 앞에서 시작하기 때문이다 -- LGA 가 AT_BASE +
 * 0x0800 처럼 큰 오프셋을 쓰던 것과 대조된다. */
#define CDNS_PCIE_HPA_AT_IB_RP_BAR_ADDR0(bar)              (((bar) * 0x0008))
/* [한국어] 인바운드 창이 덮는 주소 폭 자리(하위 6비트). */
#define CDNS_PCIE_HPA_AT_IB_RP_BAR_ADDR0_NBITS_MASK        GENMASK(5, 0)
/* [한국어] 같은 규칙으로 nbits 에서 1 을 뺀다. */
#define CDNS_PCIE_HPA_AT_IB_RP_BAR_ADDR0_NBITS(nbits) \
	(((nbits) - 1) & CDNS_PCIE_HPA_AT_IB_RP_BAR_ADDR0_NBITS_MASK)
/* [한국어] 그 인바운드 창 주소의 상위 32비트. */
#define CDNS_PCIE_HPA_AT_IB_RP_BAR_ADDR1(bar)              (0x04 + ((bar) * 0x0008))

/* AXI link down register */
/* [한국어] AXI 링크 다운 레지스터. 링크가 끊긴 동안의 AXI 접근 처리를 다룬다.
 * 값이 0x04 로 아주 작은 것도 뱅크 상대 오프셋이기 때문이다. */
#define CDNS_PCIE_HPA_AT_LINKDOWN 0x04

/*
 * Physical Layer Configuration Register 0
 * This register contains the parameters required for functional setup
 * of Physical Layer.
 */
/* [한국어] 물리 계층 설정 레지스터 0. 상류 주석대로 물리 계층의 기능 설정에
 * 필요한 파라미터가 모여 있다. LGA 에서 LM 영역(LTSSM_CONTROL_CAP)에 있던
 * 링크 학습 설정이 HPA 에서는 이쪽으로 옮겨졌다. */
#define CDNS_PCIE_HPA_PHY_LAYER_CFG0               0x0400
/* [한국어] Detect.Quiet 상태의 최소 체류 시간 자리(비트 24~26).
 * LGA 는 같은 뜻의 필드를 비트 1~2 에 두었다. */
#define CDNS_PCIE_HPA_DETECT_QUIET_MIN_DELAY_MASK  GENMASK(26, 24)
/* [한국어] FIELD_PREP 으로 delay 를 그 자리에 넣는다. 값을 키우면 링크 학습이
 * 느려지는 대신 신호가 안정될 시간을 더 벌어, 불안정한 보드에서 학습 실패가
 * 줄어든다. */
#define CDNS_PCIE_HPA_DETECT_QUIET_MIN_DELAY(delay) \
	FIELD_PREP(CDNS_PCIE_HPA_DETECT_QUIET_MIN_DELAY_MASK, delay)
/* [한국어] 링크 학습 활성 비트(비트 27). GENMASK(27, 27)로 한 비트를
 * 마스크 형태로 표현했다 -- FIELD_PREP 과 짝을 맞추기 위한 표기다.
 * 이 상수를 참조하는 곳은 이 트리에 없다. */
#define CDNS_PCIE_HPA_LINK_TRNG_EN_MASK  GENMASK(27, 27)

/* [한국어] 물리 계층 디버그 상태 레지스터 0. LTSSM 현재 상태를 읽어
 * 링크가 어디까지 진행됐는지 확인하는 데 쓴다. */
#define CDNS_PCIE_HPA_PHY_DBG_STS_REG0             0x0420

/* [한국어] 루트 포트가 쓸 수 있는 인바운드 창의 최대 번호(3). LGA 와 같다. */
#define CDNS_PCIE_HPA_RP_MAX_IB     0x3
/* [한국어] 아웃바운드 창의 최대 개수(15). **LGA 의 32개보다 적다** -- 창 하나가
 * 0x80 바이트로 커진 대신 개수가 줄었다. 위의 (r) & 0x1F 마스크는 32칸까지
 * 표현하지만 실제 상한은 이 값이다. */
#define CDNS_PCIE_HPA_MAX_OB        15

/* Endpoint Function BAR Inbound PCIe to AXI Address Translation Register */
/* [한국어] 엔드포인트 함수 fn 의 BAR bar 인바운드 주소 변환 레지스터 0.
 * 함수마다 0x80, BAR 마다 0x8 씩 떨어진다 -- 함수당 BAR 16칸을 예약한 배치로,
 * LGA(함수당 0x40, 8칸)보다 넓다. */
#define CDNS_PCIE_HPA_AT_IB_EP_FUNC_BAR_ADDR0(fn, bar) (((fn) * 0x0080) + ((bar) * 0x0008))
/* [한국어] 같은 창 주소의 상위 32비트. */
#define CDNS_PCIE_HPA_AT_IB_EP_FUNC_BAR_ADDR1(fn, bar) (0x4 + ((fn) * 0x0080) + ((bar) * 0x0008))

/* Miscellaneous offsets definitions */
/* [한국어] 태그 관리 레지스터. PCIe 요청에 붙는 태그(완료 패킷을 요청과
 * 짝짓는 번호)의 개수·할당 정책을 다룬다. 뱅크 상대 오프셋이 0 이라
 * 해당 뱅크의 첫 레지스터다. */
#define CDNS_PCIE_HPA_TAG_MANAGEMENT        0x0
/* [한국어] 슬레이브 응답 레지스터. AXI 슬레이브 쪽에서 오류를 어떻게 되돌릴지
 * (SLVERR/DECERR 등) 정한다. 이 상수를 참조하는 곳은 이 트리에 없다. */
#define CDNS_PCIE_HPA_SLAVE_RESP            0x100

/* [한국어] 루트 포트 요청자 ID 레지스터. LGA 의 CDNS_PCIE_LM_RP_RID 에
 * 해당한다. 이름만 Cadence 내부 표기(I_ 접두사)를 그대로 따랐고, 다른
 * 상수들과 달리 CDNS_PCIE_HPA_ 접두사가 없다.
 * 이 상수를 참조하는 곳은 이 트리에 없다. */
#define I_ROOT_PORT_REQ_ID_REG              0x141c
/* [한국어] SBSA(Server Base System Architecture) 관련 제어 레지스터.
 * Arm 서버 규격이 요구하는 동작으로 컨트롤러를 맞출 때 쓴다.
 * 이 상수를 참조하는 곳은 이 트리에 없다. */
#define LM_HAL_SBSA_CTRL                    0x1170

/* [한국어] 루트 포트 설정공간의 버스 번호 레지스터(primary/secondary/
 * subordinate). RP_BASE 가 0 이므로 실질적으로 0x18 이며, 이는 PCI 규약이
 * 정한 브리지 헤더의 버스 번호 오프셋과 일치한다.
 * 이 상수를 참조하는 곳은 이 트리에 없다. */
#define I_PCIE_BUS_NUMBERS                  (CDNS_PCIE_HPA_RP_BASE + 0x18)
/* [한국어] 확장 ROM 기준 주소 레지스터의 오프셋. 위 I_PCIE_BUS_NUMBERS 와
 * 같은 0x18 값이지만 이쪽은 다른 문맥(EROM)에서 쓰인다 -- 같은 숫자가 서로
 * 다른 뱅크 기준으로 해석되기 때문에 충돌하지 않는다. */
#define CDNS_PCIE_EROM                      0x18
#endif /* _PCIE_CADENCE_HPA_REGS_H */
