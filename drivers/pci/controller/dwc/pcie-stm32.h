/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ST PCIe driver definitions for STM32-MP25 SoC
 *
 * Copyright (C) 2025 STMicroelectronics - All Rights Reserved
 * Author: Christian Bruel <christian.bruel@foss.st.com>
 */

/*
 * [한국어 설명] STM32MP25 PCIe 드라이버가 공유하는 정의 (pcie-stm32.h)
 *
 * === 파일의 역할 ===
 * STMicroelectronics STM32MP25 SoC 의 PCIe 컨트롤러 드라이버 두 개가
 * 함께 쓰는 매크로만 담은 아주 작은 헤더다. 실행 코드도 구조체 정의도
 * 없고, 인스턴스 변환 매크로 하나와 SYSCFG 레지스터 상수 네 개가 전부다.
 * 이 SoC 는 같은 PCIe IP 를 루트 컴플렉스로도 엔드포인트로도 쓸 수 있는데,
 * 어느 쪽으로 동작할지는 DesignWare IP 바깥의 **시스템 설정(SYSCFG)
 * 레지스터**가 정한다. 그 레지스터를 두 드라이버가 똑같이 건드려야 하므로
 * 정의를 한곳에 모아 둔 것이다.
 * struct stm32_pcie 자체는 여기 없고 각 .c 파일이 따로 정의한다 -- 그래서
 * 아래 to_stm32_pcie() 매크로는 타입을 모른 채 drvdata 만 꺼내 준다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * PCI 컨트롤러 드라이버 계층의 SoC 글루 단, 그중에서도 두 드라이버가
 * 공유하는 최하단 정의다. 이 헤더를 포함하는 것은
 * pcie-stm32.c(루트 컴플렉스)와 pcie-stm32-ep.c(엔드포인트) 둘뿐이며,
 * 그 둘은 다시 DesignWare 코어(pcie-designware.h)에 기대어 동작한다.
 * 실행 컨텍스트는 없다 -- 전처리 단계에서만 존재하는 파일이다.
 *
 * === 타 모듈과의 연결 ===
 * 포함하는 헤더는 둘이다. <linux/bits.h> 는 아래 GENMASK/BIT 를 위한 것이고,
 * <linux/device.h> 는 to_stm32_pcie() 가 쓰는 dev_get_drvdata() 를 위한 것이다.
 * 데이터 흐름 관점에서 이 헤더는 "SYSCFG 레지스터의 좌표" 만 제공한다.
 * 실제 접근은 두 드라이버가 각자 들고 있는 regmap 핸들(stm32_pcie->regmap)로
 * 이루어지며, 그 regmap 은 DT 의 syscon phandle 에서 온다.
 * drivers/nvme 는 이 헤더의 어떤 이름도 참조하지 않는다 -- NVMe 는 열거가
 * 끝난 뒤의 PCI 디바이스만 다루므로 SoC 시스템 설정 레지스터와 닿지 않는다.
 *
 * === 주요 함수/구조체 요약 ===
 * 함수도 구조체도 없다. 정의는 다섯 개뿐이다.
 *  - to_stm32_pcie(x): dw_pcie 포인터에서 이 드라이버 인스턴스를 되찾는다.
 *  - STM32MP25_PCIECR_TYPE_MASK / _EP / _RC: 컨트롤러의 동작 모드를 고르는
 *    필드와 그 값. EP 가 0, RC 가 BIT(10) 이다.
 *  - STM32MP25_PCIECR_LTSSM_EN: 링크 학습을 시작시키는 비트.
 *  - SYSCFG_PCIECR: 위 비트들이 들어 있는 SYSCFG 안의 레지스터 오프셋.
 */

/* [한국어] GENMASK 와 BIT 매크로를 위해 포함한다. 아래 네 상수가 모두
 * 이 둘로 만들어지므로 필수 의존이다. */
#include <linux/bits.h>
/* [한국어] dev_get_drvdata() 를 위해 포함한다. 바로 아래 to_stm32_pcie()
 * 매크로가 그것을 쓴다. */
#include <linux/device.h>

/* [한국어] dw_pcie 포인터 x 에서 이 드라이버의 인스턴스를 되찾는다.
 * container_of 가 아니라 dev_get_drvdata 인 점이 특징이다 -- 두 드라이버의
 * struct stm32_pcie 정의가 서로 다른 .c 파일에 있어, 이 헤더는 그 타입을
 * 알지 못하기 때문이다. 그래서 타입을 모르고도 쓸 수 있는 drvdata 방식을
 * 택했고, 반환값은 void * 라 대입하는 쪽에서 타입이 정해진다.
 * 전제: 각 드라이버의 probe 가 platform_set_drvdata 로 인스턴스를 미리
 * 심어 두어야 한다. */
#define to_stm32_pcie(x)	dev_get_drvdata((x)->dev)

/* [한국어] SYSCFG_PCIECR 안에서 컨트롤러의 동작 모드를 담는 필드(비트 8~11).
 * 모드를 바꿀 때 이 마스크로 먼저 지우고 아래 값 중 하나를 넣는다
 * (pcie-stm32.c:172 의 regmap_update_bits 참조). */
#define STM32MP25_PCIECR_TYPE_MASK	GENMASK(11, 8)
/* [한국어] 엔드포인트 모드 값. **0 이라는 점이 중요하다** -- 필드를 지우는
 * 것만으로 EP 가 되므로, 별도의 비트를 세우지 않는다. */
#define STM32MP25_PCIECR_EP		0
/* [한국어] LTSSM(Link Training and Status State Machine) 활성 비트.
 * 이 비트를 세워야 하드웨어가 링크 학습을 시작한다. 위 TYPE 필드(비트 8~11)
 * 밖에 있어 모드와 독립적으로 켜고 끌 수 있다 -- 그래서 두 드라이버가
 * 설정을 마친 뒤 마지막에 이것만 따로 세운다. */
#define STM32MP25_PCIECR_LTSSM_EN	BIT(2)
/* [한국어] 루트 컴플렉스 모드 값. TYPE_MASK(비트 8~11) 안의 비트 10 이라,
 * 마스크로 지운 뒤 이 값을 OR 하면 RC 가 된다. */
#define STM32MP25_PCIECR_RC		BIT(10)

/* [한국어] SYSCFG 블록 안에서 위 비트들이 들어 있는 레지스터의 오프셋.
 * DesignWare IP 의 DBI 창이 아니라 **SoC 시스템 설정 블록**에 있으므로,
 * 두 드라이버는 DT 의 syscon phandle 로 얻은 regmap 을 통해 접근한다
 * (regmap_update_bits(stm32_pcie->regmap, SYSCFG_PCIECR, ...)). */
#define SYSCFG_PCIECR			0x6000
