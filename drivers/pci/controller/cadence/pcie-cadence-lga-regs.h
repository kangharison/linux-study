/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Cadence PCIe controller driver.
 *
 * Copyright (c) 2017 Cadence
 * Author: Cyrille Pitchen <cyrille.pitchen@free-electrons.com>
 */
/*
 * [한국어 설명] Cadence PCIe 컨트롤러 LGA 레지스터 맵 (pcie-cadence-lga-regs.h)
 *
 * === 파일의 역할 ===
 * Cadence PCIe IP 의 **구형 레지스터 배치(LGA, Legacy Gen Architecture)** 에서
 * 쓰는 모든 레지스터 오프셋과 비트필드를 정의하는 순수 헤더다. 실행 코드는
 * 한 줄도 없고, 오직 #define 만으로 하드웨어 문서를 코드로 옮겨 놓았다.
 * Cadence IP 는 세대에 따라 레지스터 주소와 비트 자리가 완전히 달라져서,
 * 같은 드라이버가 두 배치를 모두 지원하려면 맵을 파일 단위로 분리하는 수밖에
 * 없다. 그 짝이 pcie-cadence-hpa-regs.h(신형 HPA 배치)이며, 두 파일은 같은
 * 개념(로컬 관리, 주소 변환, BAR 설정)을 서로 다른 주소·자릿수로 표현한다.
 * 이 파일에는 링크 업 대기 재시도 횟수/간격 같은 드라이버 동작 파라미터도
 * 함께 들어 있다 -- 하드웨어 타이밍에 직접 매인 값이기 때문이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * PCI 서브시스템의 컨트롤러 드라이버 계층, 그중 Cadence IP 계열의 최하단이다.
 * 위로는 pcie-cadence.h 가 이 헤더를 포함해 접근자 인라인 함수
 * (cdns_pcie_readl/writel 계열)와 함께 노출하고, 그것을 다시
 * pcie-cadence.c(공통), pcie-cadence-host.c(루트 컴플렉스),
 * pcie-cadence-ep.c(엔드포인트)가 사용한다. 최종 소비자는 SoC 별 글루
 * 드라이버(pcie-cadence-plat.c, pci-j721e.c, pcie-sg2042.c)다.
 * 실행 컨텍스트는 없다 -- 전처리 단계에서만 존재하는 파일이다. 여기 정의된
 * 값은 모두 커널 프로세스 문맥 또는 인터럽트 문맥에서 MMIO 접근에 쓰인다.
 *
 * === 타 모듈과의 연결 ===
 * 이 파일은 <linux/bitfield.h> 하나만 포함한다. GENMASK 로 비트 범위를
 * 표현하기 위해서다. 그 외에는 아무 데도 의존하지 않는다.
 * 데이터 흐름 관점에서 이 헤더는 "주소를 만들어 내는 쪽" 이다: 드라이버가
 * 버스/장치/BAR 번호 같은 논리 값을 넘기면 여기의 함수형 매크로가 그것을
 * 레지스터 오프셋이나 비트 패턴으로 바꾸고, 접근자가 그 결과를 MMIO 로
 * 내보낸다. 반대 방향(레지스터에서 읽은 값의 해석)에도 같은 마스크가 쓰인다.
 * 공유 자료구조는 없다. 대신 **주소 공간 자체가 공유 상태**다 -- 이 헤더의
 * 상수들은 struct cdns_pcie 의 reg_base 를 기준으로 한 상대 오프셋이며,
 * 그 기준점은 SoC 글루 드라이버가 DT 에서 잡아 채워 준다.
 * 참고로 drivers/nvme 는 이 헤더의 어떤 이름도 참조하지 않는다. NVMe 는
 * 열거가 끝난 뒤의 PCI 디바이스만 다루므로, 호스트 컨트롤러 레지스터 맵과는
 * 직접 닿는 지점이 없다(이 트리에서 확인).
 *
 * === 주요 함수/구조체 요약 ===
 * 함수도 구조체도 없다. 대신 레지스터 묶음이 네 개의 기준 주소로 갈린다.
 *  - CDNS_PCIE_LM_BASE (0x00100000) -- Local Management. 벤더/서브시스템 ID,
 *    루트 포트 RID, 엔드포인트 버스·장치 번호, BAR 설정, PTM, LTSSM 능력.
 *  - CDNS_PCIE_EP_FUNC_BASE(fn) -- 엔드포인트 각 함수의 PCI 설정공간 창.
 *    함수 번호를 12비트 왼쪽으로 밀어 4KiB 단위로 나눈다.
 *  - CDNS_PCIE_RP_BASE (0x00200000) -- 루트 포트 함수의 PCI 설정공간.
 *  - CDNS_PCIE_AT_BASE (0x00400000) -- Address Translation. 아웃바운드
 *    (AXI→PCIe) 창 32개와 인바운드(PCIe→AXI) 창들의 주소·서술자 레지스터.
 * 함수형 매크로는 대개 두 벌로 온다: _MASK 는 비트 범위를, 같은 이름의
 * 함수형은 값을 그 자리로 밀어 넣은 결과를 준다. 그래서 드라이버는
 * "지우기(&= ~MASK) → 넣기(|= 함수형)" 형태로 필드를 갱신한다.
 */
#ifndef _PCIE_CADENCE_LGA_REGS_H
#define _PCIE_CADENCE_LGA_REGS_H

/* [한국어] [한국어] GENMASK/BIT 매크로를 쓰기 위해 포함한다. 이 헤더의 거의 모든
 * 비트필드가 GENMASK(hi, lo) 로 범위를 표현하므로 필수 의존이다.
 * 실행 코드가 없는 파일이라 포함하는 헤더도 이것 하나뿐이다. */
#include <linux/bitfield.h>

/* Parameters for the waiting for link up routine */
/* [한국어] [한국어] 링크 업을 기다릴 때의 최대 재시도 횟수. 아래 USLEEP 값과 곱하면
 * 최악 대기 시간이 나온다: 10 * 100ms = 1초. PCIe 규약이 요구하는 링크
 * 학습 시간보다 넉넉하게 잡아, 느린 엔드포인트도 놓치지 않게 한 값이다. */
#define LINK_WAIT_MAX_RETRIES	10
/* [한국어] [한국어] 재시도 사이 usleep_range 의 하한(90ms). 밀리초 단위로 큰 이유는
 * 링크 학습이 마이크로초가 아니라 수십 밀리초 단위로 진행되기 때문이다. */
#define LINK_WAIT_USLEEP_MIN	90000
/* [한국어] [한국어] 같은 범위의 상한(100ms). usleep_range 에 하한/상한을 주면 커널이
 * 다른 타이머와 묶어 깨울 수 있어 불필요한 인터럽트가 줄어든다. */
#define LINK_WAIT_USLEEP_MAX	100000

/* Local Management Registers */
/* [한국어] [한국어] Local Management 레지스터 블록의 기준 오프셋. IP 내부 설정 --
 * 벤더 ID, BAR 설정, PTM, LTSSM 능력 -- 이 여기 모여 있다. PCI 설정공간이
 * 아니라 컨트롤러 자체의 제어면이라, 호스트에게는 보이지 않는다. */
#define CDNS_PCIE_LM_BASE	0x00100000

/* Vendor ID Register */
/* [한국어] [한국어] 벤더 ID / 서브시스템 ID 레지스터. 이 컨트롤러가 PCI 설정공간에
 * 내보일 신원 값을 소프트웨어가 정해 넣는 자리다. 한 32비트 워드에 두 필드가
 * 아래위로 나뉘어 있다. */
#define CDNS_PCIE_LM_ID		(CDNS_PCIE_LM_BASE + 0x0044)
/* [한국어] [한국어] 하위 16비트가 Vendor ID 자리라는 표시. 갱신 시 이 마스크로 먼저
 * 지우고 아래 함수형 매크로로 새 값을 넣는다. */
#define  CDNS_PCIE_LM_ID_VENDOR_MASK	GENMASK(15, 0)
/* [한국어] [한국어] 그 필드의 시작 비트(0). 마스크와 짝을 이뤄 함수형 매크로에 쓰인다. */
#define  CDNS_PCIE_LM_ID_VENDOR_SHIFT	0
/* [한국어] [한국어] vid 를 Vendor ID 자리로 옮겨 담는다. 시프트 뒤 마스크를 다시
 * 씌우는 이유는, 인자가 16비트를 넘겨도 이웃 필드(SUBSYS)를 침범하지
 * 않게 잘라 내기 위해서다. */
#define  CDNS_PCIE_LM_ID_VENDOR(vid) \
	(((vid) << CDNS_PCIE_LM_ID_VENDOR_SHIFT) & CDNS_PCIE_LM_ID_VENDOR_MASK)
/* [한국어] [한국어] 상위 16비트가 Subsystem ID 자리. */
#define  CDNS_PCIE_LM_ID_SUBSYS_MASK	GENMASK(31, 16)
/* [한국어] [한국어] 그 필드의 시작 비트(16). */
#define  CDNS_PCIE_LM_ID_SUBSYS_SHIFT	16
/* [한국어] [한국어] sub 를 Subsystem ID 자리로 옮겨 담는다. VENDOR 쪽과 같은 이유로
 * 시프트 후 마스크를 씌운다. */
#define  CDNS_PCIE_LM_ID_SUBSYS(sub) \
	(((sub) << CDNS_PCIE_LM_ID_SUBSYS_SHIFT) & CDNS_PCIE_LM_ID_SUBSYS_MASK)

/* Root Port Requester ID Register */
/* [한국어] [한국어] Root Port Requester ID 레지스터. 루트 포트가 자기 이름으로 내보내는
 * 요청의 RID(버스:장치.함수)를 정한다. 이 값이 잘못되면 완료 패킷이 돌아올
 * 주소를 못 찾아 트랜잭션이 타임아웃된다. */
#define  CDNS_PCIE_LM_RP_RID		(CDNS_PCIE_LM_BASE + 0x0228)
/* [한국어] [한국어] RID 는 하위 16비트를 쓴다 -- 버스 8 + 장치 5 + 함수 3 비트. */
#define  CDNS_PCIE_LM_RP_RID_MASK	GENMASK(15, 0)
/* [한국어] [한국어] 그 필드의 시작 비트(0). */
#define  CDNS_PCIE_LM_RP_RID_SHIFT	0
/* [한국어] [한국어] rid 를 RID 자리로 옮겨 담는다. 이름 끝의 밑줄은 위의 상수
 * CDNS_PCIE_LM_RP_RID 와 이름이 겹치는 것을 피하려는 것이다. */
#define  CDNS_PCIE_LM_RP_RID_(rid) \
	(((rid) << CDNS_PCIE_LM_RP_RID_SHIFT) & CDNS_PCIE_LM_RP_RID_MASK)

/* Endpoint Bus and Device Number Register */
/* [한국어] [한국어] 엔드포인트 모드에서 이 컨트롤러가 응답할 버스/장치 번호. 엔드포인트는
 * 호스트가 열거하며 번호를 정해 주지만, Cadence IP 는 그 값을 이 레지스터에
 * 반영해 두어야 내부 라우팅이 맞는다. */
#define  CDNS_PCIE_LM_EP_ID		(CDNS_PCIE_LM_BASE + 0x022C)
/* [한국어] [한국어] 장치 번호는 5비트(0~31). PCI 규약의 장치 번호 폭 그대로다. */
#define  CDNS_PCIE_LM_EP_ID_DEV_MASK	GENMASK(4, 0)
/* [한국어] [한국어] 장치 번호 필드의 시작 비트(0). */
#define  CDNS_PCIE_LM_EP_ID_DEV_SHIFT	0
/* [한국어] [한국어] 버스 번호는 비트 8~15 의 8비트(0~255). */
#define  CDNS_PCIE_LM_EP_ID_BUS_MASK	GENMASK(15, 8)
/* [한국어] [한국어] 버스 번호 필드의 시작 비트(8). */
#define  CDNS_PCIE_LM_EP_ID_BUS_SHIFT	8

/* Endpoint Function f BAR b Configuration Registers */
/* [한국어] [한국어] 엔드포인트 함수 fn 의 BAR b 설정 레지스터를 고른다. BAR 6개가
 * 두 개의 32비트 레지스터에 나뉘어 담기므로, BAR_4 를 경계로 CFG0/CFG1 을
 * 갈라 준다 -- 레지스터 하나가 BAR 4개분(각 8비트)만 담을 수 있기 때문이다. */
#define CDNS_PCIE_LM_EP_FUNC_BAR_CFG(bar, fn) \
	(((bar) < BAR_4) ? CDNS_PCIE_LM_EP_FUNC_BAR_CFG0(fn) : CDNS_PCIE_LM_EP_FUNC_BAR_CFG1(fn))
/* [한국어] [한국어] BAR 0~3 을 담는 레지스터. 함수마다 8바이트씩 떨어져 있다. */
#define CDNS_PCIE_LM_EP_FUNC_BAR_CFG0(fn) \
	(CDNS_PCIE_LM_BASE + 0x0240 + (fn) * 0x0008)
/* [한국어] [한국어] BAR 4~5 를 담는 레지스터. CFG0 바로 다음 4바이트다. */
#define CDNS_PCIE_LM_EP_FUNC_BAR_CFG1(fn) \
	(CDNS_PCIE_LM_BASE + 0x0244 + (fn) * 0x0008)
/* [한국어] [한국어] SR-IOV 가상 함수(VF)용 같은 구조. PF 와 별도의 BAR 설정을 갖는다. */
#define CDNS_PCIE_LM_EP_VFUNC_BAR_CFG(bar, fn) \
	(((bar) < BAR_4) ? CDNS_PCIE_LM_EP_VFUNC_BAR_CFG0(fn) : CDNS_PCIE_LM_EP_VFUNC_BAR_CFG1(fn))
/* [한국어] [한국어] VF 의 BAR 0~3 설정 레지스터. */
#define CDNS_PCIE_LM_EP_VFUNC_BAR_CFG0(fn) \
	(CDNS_PCIE_LM_BASE + 0x0280 + (fn) * 0x0008)
/* [한국어] [한국어] VF 의 BAR 4~5 설정 레지스터. */
#define CDNS_PCIE_LM_EP_VFUNC_BAR_CFG1(fn) \
	(CDNS_PCIE_LM_BASE + 0x0284 + (fn) * 0x0008)
/* [한국어] [한국어] 레지스터 안에서 BAR b 의 aperture(크기 지수) 자리. 한 BAR 이
 * 8비트를 차지하므로 b * 8 만큼 밀고, 그중 하위 5비트가 크기다.
 * 5비트로 2^0 ~ 2^31 까지 표현한다. */
#define  CDNS_PCIE_LM_EP_FUNC_BAR_CFG_BAR_APERTURE_MASK(b) \
	(GENMASK(4, 0) << ((b) * 8))
/* [한국어] [한국어] 크기 지수 a 를 BAR b 의 자리로 옮겨 담는다. */
#define  CDNS_PCIE_LM_EP_FUNC_BAR_CFG_BAR_APERTURE(b, a) \
	(((a) << ((b) * 8)) & CDNS_PCIE_LM_EP_FUNC_BAR_CFG_BAR_APERTURE_MASK(b))
/* [한국어] [한국어] 같은 8비트 안에서 상위 3비트가 BAR 종류(control) 자리다.
 * GENMASK(7,5) 를 b * 8 만큼 민다. */
#define  CDNS_PCIE_LM_EP_FUNC_BAR_CFG_BAR_CTRL_MASK(b) \
	(GENMASK(7, 5) << ((b) * 8))
/* [한국어] [한국어] 종류 값 c 를 BAR b 의 control 자리로 옮겨 담는다. 시프트가
 * b * 8 + 5 인 것은 8비트 슬롯 안에서 다시 5비트를 건너뛰기 때문이다. */
#define  CDNS_PCIE_LM_EP_FUNC_BAR_CFG_BAR_CTRL(b, c) \
	(((c) << ((b) * 8 + 5)) & CDNS_PCIE_LM_EP_FUNC_BAR_CFG_BAR_CTRL_MASK(b))

/* Endpoint Function Configuration Register */
/* [한국어] [한국어] 엔드포인트가 몇 개의 물리 함수를 노출할지 정하는 레지스터. */
#define CDNS_PCIE_LM_EP_FUNC_CFG	(CDNS_PCIE_LM_BASE + 0x02C0)

/* Root Complex BAR Configuration Register */
/* [한국어] [한국어] 루트 컴플렉스 모드의 BAR 설정 레지스터. 루트 포트는 BAR 를
 * 외부에 노출하지 않지만, 인바운드 주소 변환의 창 크기를 정하는 데 같은
 * 구조를 쓴다. */
#define CDNS_PCIE_LM_RC_BAR_CFG	(CDNS_PCIE_LM_BASE + 0x0300)
/* [한국어] [한국어] RC BAR0 의 크기 지수. 엔드포인트 쪽(5비트)과 달리 6비트라
 * 더 큰 창을 표현할 수 있다. */
#define  CDNS_PCIE_LM_RC_BAR_CFG_BAR0_APERTURE_MASK	GENMASK(5, 0)
/* [한국어] [한국어] 크기 지수 a 를 BAR0 자리에 넣는다. */
#define  CDNS_PCIE_LM_RC_BAR_CFG_BAR0_APERTURE(a) \
	(((a) << 0) & CDNS_PCIE_LM_RC_BAR_CFG_BAR0_APERTURE_MASK)
/* [한국어] [한국어] RC BAR0 의 종류(control) 자리, 비트 6~8. */
#define  CDNS_PCIE_LM_RC_BAR_CFG_BAR0_CTRL_MASK		GENMASK(8, 6)
/* [한국어] [한국어] 종류 값 c 를 BAR0 자리에 넣는다. */
#define  CDNS_PCIE_LM_RC_BAR_CFG_BAR0_CTRL(c) \
	(((c) << 6) & CDNS_PCIE_LM_RC_BAR_CFG_BAR0_CTRL_MASK)
/* [한국어] [한국어] RC BAR1 의 크기 지수, 비트 9~13. */
#define  CDNS_PCIE_LM_RC_BAR_CFG_BAR1_APERTURE_MASK	GENMASK(13, 9)
/* [한국어] [한국어] 크기 지수 a 를 BAR1 자리에 넣는다. */
#define  CDNS_PCIE_LM_RC_BAR_CFG_BAR1_APERTURE(a) \
	(((a) << 9) & CDNS_PCIE_LM_RC_BAR_CFG_BAR1_APERTURE_MASK)
/* [한국어] [한국어] RC BAR1 의 종류 자리, 비트 14~16. */
#define  CDNS_PCIE_LM_RC_BAR_CFG_BAR1_CTRL_MASK		GENMASK(16, 14)
/* [한국어] [한국어] 종류 값 c 를 BAR1 자리에 넣는다. */
#define  CDNS_PCIE_LM_RC_BAR_CFG_BAR1_CTRL(c) \
	(((c) << 14) & CDNS_PCIE_LM_RC_BAR_CFG_BAR1_CTRL_MASK)
/* [한국어] [한국어] 프리페치 가능 메모리 창을 켜는 비트. 이것이 꺼져 있으면 아래의
 * 32/64비트 선택은 의미가 없다. */
#define  CDNS_PCIE_LM_RC_BAR_CFG_PREFETCH_MEM_ENABLE	BIT(17)
/* [한국어] [한국어] 프리페치 창을 32비트로 쓰겠다는 표시. 값이 0 이므로 아무것도
 * 세우지 않는 것이 곧 32비트 선택이다 -- 기본값을 0 으로 잡은 설계다. */
#define  CDNS_PCIE_LM_RC_BAR_CFG_PREFETCH_MEM_32BITS	0
/* [한국어] [한국어] 프리페치 창을 64비트로 쓰겠다는 표시(비트 18). */
#define  CDNS_PCIE_LM_RC_BAR_CFG_PREFETCH_MEM_64BITS	BIT(18)
/* [한국어] [한국어] IO 창을 켜는 비트. */
#define  CDNS_PCIE_LM_RC_BAR_CFG_IO_ENABLE		BIT(19)
/* [한국어] [한국어] IO 창을 16비트 주소로 쓰겠다는 표시. 위와 같은 이유로 값이 0 이다. */
#define  CDNS_PCIE_LM_RC_BAR_CFG_IO_16BITS		0
/* [한국어] [한국어] IO 창을 32비트 주소로 쓰겠다는 표시(비트 20). */
#define  CDNS_PCIE_LM_RC_BAR_CFG_IO_32BITS		BIT(20)
/* [한국어] [한국어] 인바운드 주소가 BAR 범위 안인지 하드웨어가 검사하게 하는 비트.
 * 켜 두면 범위를 벗어난 접근이 조용히 엉뚱한 곳에 닿는 대신 걸러진다. */
#define  CDNS_PCIE_LM_RC_BAR_CFG_CHECK_ENABLE		BIT(31)

/* BAR control values applicable to both Endpoint Function and Root Complex */
/* [한국어] [한국어] BAR 를 끈 상태. 아래 값들은 엔드포인트 BAR 설정과 RC BAR 설정
 * 양쪽에서 같은 의미로 쓰인다. */
#define  CDNS_PCIE_LM_BAR_CFG_CTRL_DISABLED		0x0
/* [한국어] [한국어] 32비트 IO 공간 BAR. */
#define  CDNS_PCIE_LM_BAR_CFG_CTRL_IO_32BITS		0x1
/* [한국어] [한국어] 32비트 메모리 BAR (프리페치 불가). */
#define  CDNS_PCIE_LM_BAR_CFG_CTRL_MEM_32BITS		0x4
/* [한국어] [한국어] 32비트 프리페치 가능 메모리 BAR. 0x4 에 비트 0 이 더해진 값이다. */
#define  CDNS_PCIE_LM_BAR_CFG_CTRL_PREFETCH_MEM_32BITS	0x5
/* [한국어] [한국어] 64비트 메모리 BAR (프리페치 불가). */
#define  CDNS_PCIE_LM_BAR_CFG_CTRL_MEM_64BITS		0x6
/* [한국어] [한국어] 64비트 프리페치 가능 메모리 BAR. 0x6 에 비트 0 이 더해진 값이다.
 * 
 * 코드 관찰 (상류 그대로, 수정하지 않음): pcie-cadence-host.c:191-193 의 RC BAR
 * 설정은 프리페치가 아닐 때만 MEM_64BITS(0x6)를 OR 하고, 그 다음 줄에서
 * PREF_MEM_64BITS(0x7)를 조건 없이 OR 한다. 0x6 | 0x7 == 0x7 이므로 두 갈래가
 * 모두 0x7(프리페치 가능)로 끝나 앞의 조건문이 결과에 영향을 주지 않는다.
 * 32비트 쌍(0x4 | 0x5 == 0x5)도 마찬가지다. 신형 HPA 배치는 같은 값들을
 * 0x5/0xD 로 잡아 프리페치를 비트 3 하나로 분리했고, 코드도 기본값을 먼저
 * 넣고 프리페치일 때만 OR 을 더하는 순서라 두 상태가 실제로 구분된다
 * (pcie-cadence-host-hpa.c:131-139). */
#define  CDNS_PCIE_LM_BAR_CFG_CTRL_PREFETCH_MEM_64BITS	0x7

/* [한국어] [한국어] RC BAR 설정 레지스터 안에서 bar 번호에 맞는 자리로 '끔' 값을 민다.
 * bar * 8 + 6 인 것은 BAR 하나가 8비트를 차지하고 그 안에서 control 이
 * 6비트째부터 시작하기 때문이다. */
#define LM_RC_BAR_CFG_CTRL_DISABLED(bar)		\
		(CDNS_PCIE_LM_BAR_CFG_CTRL_DISABLED << (((bar) * 8) + 6))
/* [한국어] [한국어] 같은 방식으로 32비트 IO 값을 민다. */
#define LM_RC_BAR_CFG_CTRL_IO_32BITS(bar)		\
		(CDNS_PCIE_LM_BAR_CFG_CTRL_IO_32BITS << (((bar) * 8) + 6))
/* [한국어] [한국어] 같은 방식으로 32비트 메모리 값을 민다. */
#define LM_RC_BAR_CFG_CTRL_MEM_32BITS(bar)		\
		(CDNS_PCIE_LM_BAR_CFG_CTRL_MEM_32BITS << (((bar) * 8) + 6))
/* [한국어] [한국어] 같은 방식으로 32비트 프리페치 메모리 값을 민다. */
#define LM_RC_BAR_CFG_CTRL_PREF_MEM_32BITS(bar)	\
	(CDNS_PCIE_LM_BAR_CFG_CTRL_PREFETCH_MEM_32BITS << (((bar) * 8) + 6))
/* [한국어] [한국어] 같은 방식으로 64비트 메모리 값을 민다. */
#define LM_RC_BAR_CFG_CTRL_MEM_64BITS(bar)		\
		(CDNS_PCIE_LM_BAR_CFG_CTRL_MEM_64BITS << (((bar) * 8) + 6))
/* [한국어] [한국어] 같은 방식으로 64비트 프리페치 메모리 값을 민다. */
#define LM_RC_BAR_CFG_CTRL_PREF_MEM_64BITS(bar)	\
	(CDNS_PCIE_LM_BAR_CFG_CTRL_PREFETCH_MEM_64BITS << (((bar) * 8) + 6))
/* [한국어] [한국어] 크기 지수를 BAR 자리에 넣는다. aperture 에서 2 를 빼는 것은
 * 레지스터가 표현하는 최소 크기가 4바이트(2^2)라 0 이 곧 2^2 를 뜻하기
 * 때문이다. 여기서는 시프트가 bar * 8 뿐인데, aperture 필드가 8비트 슬롯의
 * 맨 아래에서 시작하기 때문이다. */
#define LM_RC_BAR_CFG_APERTURE(bar, aperture)		\
					(((aperture) - 2) << ((bar) * 8))

/* PTM Control Register */
/* [한국어] [한국어] PTM(Precision Time Measurement) 제어 레지스터. 호스트와 장치가
 * 클록을 나노초 단위로 맞추는 PCIe 기능이다. */
#define CDNS_PCIE_LM_PTM_CTRL		(CDNS_PCIE_LM_BASE + 0x0DA8)
/* [한국어] [한국어] PTM Responder 를 켜는 비트. 매크로 이름의 TPM 은 상류의 오타로
 * 보이지만 이름을 바꾸면 호출부가 깨지므로 그대로 둔다. */
#define CDNS_PCIE_LM_TPM_CTRL_PTMRSEN	BIT(17)

/*
 * Endpoint Function Registers (PCI configuration space for endpoint functions)
 */
/* [한국어] [한국어] 엔드포인트 함수 fn 의 PCI 설정공간 창 시작 오프셋. 함수당 4KiB
 * (1 << 12)씩 떨어지고, GENMASK(19,12) 로 잘라 최대 256개 함수까지만 표현한다. */
#define CDNS_PCIE_EP_FUNC_BASE(fn)	(((fn) << 12) & GENMASK(19, 12))

/* [한국어] [한국어] 그 창 안에서 MSI 능력 구조가 있는 오프셋. Cadence IP 가 합성 시
 * 이 자리에 고정해 둔 값이라 탐색 없이 바로 접근한다. */
#define CDNS_PCIE_EP_FUNC_MSI_CAP_OFFSET	0x90
/* [한국어] [한국어] 같은 창 안의 MSI-X 능력 구조 오프셋. */
#define CDNS_PCIE_EP_FUNC_MSIX_CAP_OFFSET	0xB0
/* [한국어] [한국어] 같은 창 안의 Device Capability(PCI Express 능력) 오프셋. */
#define CDNS_PCIE_EP_FUNC_DEV_CAP_OFFSET	0xC0
/* [한국어] [한국어] 같은 창 안의 SR-IOV 확장 능력 오프셋. 0x100 이상이므로 확장
 * 설정공간(ECS) 영역이다. */
#define CDNS_PCIE_EP_FUNC_SRIOV_CAP_OFFSET	0x200

/* Endpoint PF Registers */
/* [한국어] [한국어] 물리 함수 fn 의 ARI(Alternative Routing-ID) 능력·제어 레지스터.
 * 함수마다 4KiB(0x1000)씩 떨어진다. ARI 는 장치 번호 자리까지 함수 번호로
 * 써서 8개 제한을 256개로 넓히는 기능이다. */
#define CDNS_PCIE_CORE_PF_I_ARI_CAP_AND_CTRL(fn)	(0x144 + (fn) * 0x1000)
/* [한국어] [한국어] 그 레지스터에서 '다음 함수 번호'(Next Function Number) 필드.
 * 함수들을 사슬로 이어 열거하게 하는 값이다. */
#define CDNS_PCIE_ARI_CAP_NFN_MASK			GENMASK(15, 8)

/* Root Port Registers (PCI configuration space for the root port function) */
/* [한국어] [한국어] 루트 포트 함수의 PCI 설정공간 창. LM 블록과 달리 이쪽은 호스트
 * 소프트웨어가 보는 표준 설정공간 구조 그대로다. */
#define CDNS_PCIE_RP_BASE	0x00200000
/* [한국어] [한국어] 그 안에서 PCI Express 능력 구조의 오프셋. 링크 상태/제어를
 * 읽을 때 이 값을 기준으로 삼는다. */
#define CDNS_PCIE_RP_CAP_OFFSET 0xC0

/* Address Translation Registers */
/* [한국어] [한국어] Address Translation 블록의 기준 오프셋. AXI(CPU 쪽)와 PCIe 쪽
 * 주소를 서로 옮기는 창들이 여기 모여 있다. DesignWare 의 iATU 에 해당하는
 * Cadence 쪽 구조다. */
#define CDNS_PCIE_AT_BASE	0x00400000

/* Region r Outbound AXI to PCIe Address Translation Register 0 */
/* [한국어] [한국어] 아웃바운드 창 r 의 PCI 쪽 주소 레지스터 0. 창마다 0x20 바이트씩
 * 떨어지고, (r) & 0x1F 로 잘라 최대 32개(CDNS_PCIE_MAX_OB)까지만 접근한다. */
#define CDNS_PCIE_AT_OB_REGION_PCI_ADDR0(r) \
	(CDNS_PCIE_AT_BASE + 0x0000 + ((r) & 0x1F) * 0x0020)
/* [한국어] [한국어] 이 창이 덮는 주소 폭을 2의 지수로 담는 자리(하위 6비트). */
#define  CDNS_PCIE_AT_OB_REGION_PCI_ADDR0_NBITS_MASK	GENMASK(5, 0)
/* [한국어] [한국어] nbits 에서 1 을 빼서 넣는다. 레지스터가 0 을 '1비트' 로 세므로
 * 지수를 그대로 넣으면 창 크기가 두 배가 된다. */
#define  CDNS_PCIE_AT_OB_REGION_PCI_ADDR0_NBITS(nbits) \
	(((nbits) - 1) & CDNS_PCIE_AT_OB_REGION_PCI_ADDR0_NBITS_MASK)
/* [한국어] [한국어] 설정 트랜잭션을 만들 때 목적지 devfn 을 담는 자리(비트 12~19). */
#define  CDNS_PCIE_AT_OB_REGION_PCI_ADDR0_DEVFN_MASK	GENMASK(19, 12)
/* [한국어] [한국어] devfn 을 그 자리로 옮겨 담는다. */
#define  CDNS_PCIE_AT_OB_REGION_PCI_ADDR0_DEVFN(devfn) \
	(((devfn) << 12) & CDNS_PCIE_AT_OB_REGION_PCI_ADDR0_DEVFN_MASK)
/* [한국어] [한국어] 목적지 버스 번호를 담는 자리(비트 20~27). */
#define  CDNS_PCIE_AT_OB_REGION_PCI_ADDR0_BUS_MASK	GENMASK(27, 20)
/* [한국어] [한국어] 버스 번호를 그 자리로 옮겨 담는다. */
#define  CDNS_PCIE_AT_OB_REGION_PCI_ADDR0_BUS(bus) \
	(((bus) << 20) & CDNS_PCIE_AT_OB_REGION_PCI_ADDR0_BUS_MASK)

/* Region r Outbound AXI to PCIe Address Translation Register 1 */
/* [한국어] [한국어] 아웃바운드 창 r 의 PCI 쪽 주소 상위 32비트. ADDR0 과 짝을 이뤄
 * 64비트 주소를 만든다. */
#define CDNS_PCIE_AT_OB_REGION_PCI_ADDR1(r) \
	(CDNS_PCIE_AT_BASE + 0x0004 + ((r) & 0x1F) * 0x0020)

/* Region r Outbound PCIe Descriptor Register 0 */
/* [한국어] [한국어] 아웃바운드 창 r 의 서술자 레지스터 0. '이 창의 쓰기를 어떤 종류의
 * TLP 로 바꿀 것인가' 를 정하는 자리다. */
#define CDNS_PCIE_AT_OB_REGION_DESC0(r) \
	(CDNS_PCIE_AT_BASE + 0x0008 + ((r) & 0x1F) * 0x0020)
/* [한국어] [한국어] TLP 종류를 담는 하위 4비트. */
#define  CDNS_PCIE_AT_OB_REGION_DESC0_TYPE_MASK		GENMASK(3, 0)
/* [한국어] [한국어] 메모리 트랜잭션. */
#define  CDNS_PCIE_AT_OB_REGION_DESC0_TYPE_MEM		0x2
/* [한국어] [한국어] IO 트랜잭션. */
#define  CDNS_PCIE_AT_OB_REGION_DESC0_TYPE_IO		0x6
/* [한국어] [한국어] Type 0 설정 트랜잭션 -- 링크 바로 건너편 장치를 겨눌 때. */
#define  CDNS_PCIE_AT_OB_REGION_DESC0_TYPE_CONF_TYPE0	0xA
/* [한국어] [한국어] Type 1 설정 트랜잭션 -- 스위치를 거쳐 더 깊은 버스를 겨눌 때. */
#define  CDNS_PCIE_AT_OB_REGION_DESC0_TYPE_CONF_TYPE1	0xB
/* [한국어] [한국어] 일반 메시지 TLP. PME_Turn_Off 같은 규약 메시지를 보낼 때 쓴다. */
#define  CDNS_PCIE_AT_OB_REGION_DESC0_TYPE_NORMAL_MSG	0xC
/* [한국어] [한국어] 벤더 정의 메시지 TLP. 규약이 정하지 않은 신호를 벤더가 임의로
 * 실어 보낼 때 쓴다. */
#define  CDNS_PCIE_AT_OB_REGION_DESC0_TYPE_VENDOR_MSG	0xD
/* Bit 23 MUST be set in RC mode. */
/* [한국어] [한국어] 이 창이 만드는 TLP 의 요청자 ID 를 소프트웨어가 정한 값으로
 * 고정하라는 비트. 상류 주석이 못 박아 두었듯 **RC 모드에서는 반드시 세워야
 * 한다** -- 루트 포트가 자기 RID 로 요청을 내보내야 완료 패킷이 돌아올 길을
 * 찾기 때문이다. */
#define  CDNS_PCIE_AT_OB_REGION_DESC0_HARDCODED_RID	BIT(23)
/* [한국어] [한국어] 그 고정 RID 의 devfn 부분(비트 24~31). */
#define  CDNS_PCIE_AT_OB_REGION_DESC0_DEVFN_MASK	GENMASK(31, 24)
/* [한국어] [한국어] devfn 을 그 자리로 옮겨 담는다. */
#define  CDNS_PCIE_AT_OB_REGION_DESC0_DEVFN(devfn) \
	(((devfn) << 24) & CDNS_PCIE_AT_OB_REGION_DESC0_DEVFN_MASK)

/* Region r Outbound PCIe Descriptor Register 1 */
/* [한국어] [한국어] 아웃바운드 창 r 의 서술자 레지스터 1. DESC0 이 담지 못한 버스
 * 번호가 여기 들어간다. */
#define CDNS_PCIE_AT_OB_REGION_DESC1(r)	\
	(CDNS_PCIE_AT_BASE + 0x000C + ((r) & 0x1F) * 0x0020)
/* [한국어] [한국어] 고정 RID 의 버스 번호 부분(하위 8비트). */
#define  CDNS_PCIE_AT_OB_REGION_DESC1_BUS_MASK	GENMASK(7, 0)
/* [한국어] [한국어] 버스 번호를 그대로 담는다. 시프트가 없는 것은 필드가 비트 0 에서
 * 시작하기 때문이다. */
#define  CDNS_PCIE_AT_OB_REGION_DESC1_BUS(bus) \
	((bus) & CDNS_PCIE_AT_OB_REGION_DESC1_BUS_MASK)

/* Region r AXI Region Base Address Register 0 */
/* [한국어] [한국어] 아웃바운드 창 r 의 AXI(CPU) 쪽 기준 주소 레지스터 0. PCI_ADDR 쪽과
 * 짝을 이뤄 '이 CPU 주소 범위를 저 PCI 주소로 옮긴다' 는 대응을 완성한다.
 * 오프셋 0x18 로 같은 0x20 바이트 창 블록 안에 있다. */
#define CDNS_PCIE_AT_OB_REGION_CPU_ADDR0(r) \
	(CDNS_PCIE_AT_BASE + 0x0018 + ((r) & 0x1F) * 0x0020)
/* [한국어] [한국어] CPU 쪽 창이 덮는 주소 폭을 담는 자리(하위 6비트). */
#define  CDNS_PCIE_AT_OB_REGION_CPU_ADDR0_NBITS_MASK	GENMASK(5, 0)
/* [한국어] [한국어] PCI 쪽과 같은 이유로 nbits 에서 1 을 빼서 넣는다. */
#define  CDNS_PCIE_AT_OB_REGION_CPU_ADDR0_NBITS(nbits) \
	(((nbits) - 1) & CDNS_PCIE_AT_OB_REGION_CPU_ADDR0_NBITS_MASK)

/* Region r AXI Region Base Address Register 1 */
/* [한국어] [한국어] AXI 쪽 기준 주소의 상위 32비트. 64비트 물리 주소를 쓰는 SoC 에서
 * 필요하다. */
#define CDNS_PCIE_AT_OB_REGION_CPU_ADDR1(r) \
	(CDNS_PCIE_AT_BASE + 0x001C + ((r) & 0x1F) * 0x0020)

/* Root Port BAR Inbound PCIe to AXI Address Translation Register */
/* [한국어] [한국어] 루트 포트 BAR bar 의 인바운드(PCIe→AXI) 주소 변환 레지스터 0.
 * 엔드포인트가 DMA 로 보낸 주소를 시스템 메모리 주소로 되돌리는 창이다.
 * BAR 마다 8바이트씩 떨어진다. */
#define CDNS_PCIE_AT_IB_RP_BAR_ADDR0(bar) \
	(CDNS_PCIE_AT_BASE + 0x0800 + (bar) * 0x0008)
/* [한국어] [한국어] 인바운드 창이 덮는 주소 폭 자리(하위 6비트). */
#define  CDNS_PCIE_AT_IB_RP_BAR_ADDR0_NBITS_MASK	GENMASK(5, 0)
/* [한국어] [한국어] 아웃바운드와 같은 규칙으로 nbits 에서 1 을 뺀다. */
#define  CDNS_PCIE_AT_IB_RP_BAR_ADDR0_NBITS(nbits) \
	(((nbits) - 1) & CDNS_PCIE_AT_IB_RP_BAR_ADDR0_NBITS_MASK)
/* [한국어] [한국어] 그 인바운드 창 주소의 상위 32비트. */
#define CDNS_PCIE_AT_IB_RP_BAR_ADDR1(bar) \
	(CDNS_PCIE_AT_BASE + 0x0804 + (bar) * 0x0008)

/* AXI link down register */
/* [한국어] [한국어] AXI 링크 다운 레지스터. 링크가 끊긴 동안 AXI 쪽 접근을 어떻게
 * 처리할지를 다룬다 -- 응답 없이 멈추는 대신 오류로 끝내 시스템 정지를 막는다. */
#define CDNS_PCIE_AT_LINKDOWN (CDNS_PCIE_AT_BASE + 0x0824)

/* LTSSM Capabilities register */
/* [한국어] [한국어] LTSSM(Link Training and Status State Machine) 제어·능력 레지스터.
 * 주소가 AT 가 아니라 LM 기준인 점에 유의 -- 링크 학습은 로컬 관리 영역이다. */
#define CDNS_PCIE_LTSSM_CONTROL_CAP		(CDNS_PCIE_LM_BASE + 0x0054)
/* [한국어] [한국어] Detect.Quiet 상태의 최소 체류 시간을 담는 자리(비트 1~2). */
#define  CDNS_PCIE_DETECT_QUIET_MIN_DELAY_MASK	GENMASK(2, 1)
/* [한국어] [한국어] 그 필드의 시작 비트(1). */
#define  CDNS_PCIE_DETECT_QUIET_MIN_DELAY_SHIFT 1
/* [한국어] [한국어] delay 값을 그 자리로 옮겨 담는다. 이 값을 늘리면 링크 학습이
 * 느려지는 대신, 신호가 안정되기를 더 기다려 불안정한 보드에서 학습
 * 실패가 줄어든다. */
#define  CDNS_PCIE_DETECT_QUIET_MIN_DELAY(delay) \
	 (((delay) << CDNS_PCIE_DETECT_QUIET_MIN_DELAY_SHIFT) & \
	 CDNS_PCIE_DETECT_QUIET_MIN_DELAY_MASK)

/* [한국어] [한국어] 루트 포트가 쓸 수 있는 인바운드 창의 최대 번호(3). BAR 개수에
 * 대응한다. */
#define CDNS_PCIE_RP_MAX_IB	0x3
/* [한국어] [한국어] 아웃바운드 창의 최대 개수(32). 위의 (r) & 0x1F 마스크와 짝이다. */
#define CDNS_PCIE_MAX_OB	32

/* Endpoint Function BAR Inbound PCIe to AXI Address Translation Register */
/* [한국어] [한국어] 엔드포인트 함수 fn 의 BAR bar 에 대한 인바운드 주소 변환 레지스터 0.
 * 함수마다 0x40, BAR 마다 0x8 씩 떨어진다 -- 함수당 BAR 8칸을 예약한 배치다.
 * 엔드포인트 BAR 로 들어온 접근을 로컬 메모리 주소로 되돌린다. */
#define CDNS_PCIE_AT_IB_EP_FUNC_BAR_ADDR0(fn, bar) \
	(CDNS_PCIE_AT_BASE + 0x0840 + (fn) * 0x0040 + (bar) * 0x0008)
/* [한국어] [한국어] 같은 창 주소의 상위 32비트. */
#define CDNS_PCIE_AT_IB_EP_FUNC_BAR_ADDR1(fn, bar) \
	(CDNS_PCIE_AT_BASE + 0x0844 + (fn) * 0x0040 + (bar) * 0x0008)

/* Normal/Vendor specific message access: offset inside some outbound region */
/* [한국어] [한국어] 메시지 TLP 의 라우팅 필드(비트 5~7). 이 값들은 레지스터가 아니라
 * **아웃바운드 창 안의 오프셋**에 실린다 -- 상류 주석대로, 메시지를 보내는
 * 방법이 '메시지용으로 설정한 창의 특정 오프셋에 쓰기' 이기 때문이다.
 * 그래서 라우팅과 코드가 주소 비트로 인코딩된다. */
#define CDNS_PCIE_NORMAL_MSG_ROUTING_MASK	GENMASK(7, 5)
/* [한국어] [한국어] 라우팅 값을 그 자리로 옮겨 담는다. 라우팅은 메시지를 어디까지
 * 전파할지(수신자 지정, 브로드캐스트, 로컬 등)를 정한다. */
#define CDNS_PCIE_NORMAL_MSG_ROUTING(route) \
	(((route) << 5) & CDNS_PCIE_NORMAL_MSG_ROUTING_MASK)
/* [한국어] [한국어] 메시지 코드 필드(비트 8~15). PME_Turn_Off 같은 메시지 종류를 담는다. */
#define CDNS_PCIE_NORMAL_MSG_CODE_MASK		GENMASK(15, 8)
/* [한국어] [한국어] 코드 값을 그 자리로 옮겨 담는다. */
#define CDNS_PCIE_NORMAL_MSG_CODE(code) \
	(((code) << 8) & CDNS_PCIE_NORMAL_MSG_CODE_MASK)
/* [한국어] [한국어] 데이터 없는 메시지라는 표시(비트 16). PME_Turn_Off 처럼 신호만
 * 전하는 메시지는 페이로드가 없으므로 이 비트를 세운다. */
#define CDNS_PCIE_MSG_NO_DATA                   BIT(16)

#endif /* _PCIE_CADENCE_LGA_REGS_H */
