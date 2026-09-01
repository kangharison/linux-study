/* SPDX-License-Identifier: GPL-2.0 */
/*
 * PCIe driver for Renesas R-Car SoCs
 *  Copyright (C) 2014-2020 Renesas Electronics Europe Ltd
 *
 * Author: Phil Edworthy <phil.edworthy@renesas.com>
 */

/*
 * [한국어 설명] R-Car PCIe 컨트롤러의 레지스터 지도와 공용 선언 (pcie-rcar.h)
 *
 * === 파일의 역할 ===
 * R-Car SoC 안의 PCIe 컨트롤러가 가진 레지스터들의 오프셋과 비트 정의를
 * 한곳에 모아 둔 사설 헤더다. 코드는 한 줄도 없고 전부 #define 과 선언이다.
 *
 * 이 헤더가 따로 존재하는 이유는 같은 하드웨어를 두 드라이버가 정반대
 * 역할로 쓰기 때문이다. pcie-rcar-host.c 는 이 컨트롤러를 루트 컴플렉스로
 * 몰아 남의 장치를 열거하고, pcie-rcar-ep.c 는 같은 컨트롤러를 엔드포인트로
 * 만들어 남의 버스에 꽂힌다. 둘이 만지는 레지스터는 거의 같고 쓰는 값만
 * 다르므로, 정의를 공유하고 정책만 각자 갖는 구조가 된다.
 *
 * 레지스터가 오프셋 대역별로 뚜렷이 묶여 있다는 점을 알아 두면 읽기 쉽다.
 *   0x000xxx  config 접근 창, 모드 설정, PHY 상태, INTx, MSI 송신
 *   0x002xxx  전송 제어와 주소 창(안쪽 방향 포함), MSI 수신
 *   0x003xxx  바깥 방향 주소 창
 *   0x010xxx  이 컨트롤러 자신의 config space (PCI 표준 레이아웃을 그대로 흉내)
 *   0x011xxx  링크 계층 — 신원 설정, 링크 속도, 전원 관리
 *   0x04xxxx  R-Car H1 전용 PHY 창
 *   0x0007xx  R-Car Gen2 전용 PHY 창
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 헤더 자체는 실행되지 않는다. 같은 디렉터리의 세 .c 파일이 모두
 * 이것을 include 하며, 그 셋의 관계는 이렇다.
 *
 *   pcie-rcar.c        공용 저수준 헬퍼. 레지스터 읽기/쓰기, 링크·PHY 대기,
 *                      주소 창 설정. 아래 선언부의 일곱 함수가 여기 정의된다.
 *   pcie-rcar-host.c   루트 컴플렉스 정책. PCIEMSR 에 1 을 쓴다.
 *   pcie-rcar-ep.c     엔드포인트 정책. PCIEMSR 에 0 을 쓴다.
 *
 * Makefile 이 그 조합을 정한다 — :11 이 CONFIG_PCIE_RCAR_HOST 에
 * pcie-rcar.o + pcie-rcar-host.o 를, :12 가 CONFIG_PCIE_RCAR_EP 에
 * pcie-rcar.o + pcie-rcar-ep.o 를 묶는다. 공용 부분이 양쪽에 함께 들어간다.
 *
 * 실행 컨텍스트: 헤더라 해당 없음. 다만 여기 선언된 함수 중
 * rcar_pcie_wait_for_phyrdy() 와 rcar_pcie_wait_for_dl() 은 잠들 수 있어
 * 프로세스 컨텍스트에서만 부를 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * 이 헤더가 의존하는 것: BIT() 와 GENMASK() 매크로, struct device,
 * struct resource_entry. 다만 그 정의를 직접 include 하지 않고 이 헤더를
 * 포함하는 .c 파일이 먼저 linux/pci.h 등을 include 해 두는 것을 전제한다.
 * 헤더 가드 안에 #include 가 하나도 없는 것이 그 사실을 보여 준다.
 * 이 헤더에 의존하는 것: 위 세 .c 파일뿐이다. drivers/ 어디에서도
 * 이 경로를 include 하지 않는다.
 * 공유 상태: 아래 struct rcar_pcie 하나. 세 파일이 이 구조체를 통해
 * 레지스터 기준 주소를 주고받으며, host 판과 ep 판은 각자 이것을 감싼
 * 더 큰 구조체(rcar_pcie_host / rcar_pcie_endpoint)를 따로 둔다.
 *
 * === 주요 함수/구조체 요약 ===
 * struct rcar_pcie      : dev 와 base 만 담은 최소 구조체. 두 드라이버가
 *                         각자의 상태 구조체 첫 필드로 박아 둔다.
 * enum RCAR_PCI_ACCESS_*: config 접근 방향을 나타내는 두 값.
 *                         host 판의 rcar_pcie_config_access() 만 쓴다.
 * rcar_pci_read_reg() / rcar_pci_write_reg() : readl/writel 을 base 기준으로 감싼다.
 * rcar_rmw32()          : 바이트 오프셋 단위 읽고-고쳐-쓰기. 4의 배수가 아닌
 *                         오프셋도 받아 하위 2비트로 시프트를 계산한다.
 * rcar_pcie_wait_for_phyrdy() / rcar_pcie_wait_for_dl() : PHY 준비와 링크
 *                         확립을 폴링으로 기다린다.
 * rcar_pcie_set_outbound() : CPU -> PCI 방향 창 하나를 연다.
 * rcar_pcie_set_inbound()  : PCI -> 메모리 방향 창 한 쌍을 연다.
 *                         마지막 인자 host 가 두 드라이버의 차이를 흡수한다.
 *
 * === 값의 근거에 대하여 ===
 * 아래 주석은 pcie-rcar.c / pcie-rcar-host.c / pcie-rcar-ep.c 에서 각 매크로가
 * 실제로 어떻게 쓰이는지를 근거로 적었다. 사용처가 없거나 코드만으로 뜻을
 * 확정할 수 없는 것은 추측하지 않고 그렇게 밝혀 두었다. Renesas 의 하드웨어
 * 매뉴얼은 이 트리에 없다.
 *
 * === NVMe 관점 ===
 * drivers/nvme/ 에서 이 헤더의 심볼을 쓰는 곳은 없다(전수 grep 확인).
 * 이 헤더는 호스트 컨트롤러의 내부 레지스터 지도이고, NVMe 드라이버는
 * 그 컨트롤러가 만든 버스 위에 열거되는 장치를 다룰 뿐이라 계층이 다르다.
 *
 * (이 파일에는 이력이 하나 있다. 앞선 작업이 레지스터 정의마다 NVMe 를
 *  끌어들인 여러 줄 주석을 붙였고, 그것을 걷어내는 과정에서 첫 줄만
 *  지워져 "addresses when reading/writing NVMe endpoint config space." 같은
 *  꼬리 13줄이 남아 있었다. 원본 스냅숏에는 그 자리에 주석이 없었으며,
 *  현재는 원본으로 되돌린 뒤 이 주석을 새로 쓴 것이다.)
 */

/* [한국어] 헤더 가드. 세 .c 파일이 모두 이 헤더를 include 하므로 중복 포함을 막는다.
 * #include 가 하나도 없다는 점이 특징인데, BIT()/GENMASK()/struct device 등의
 * 정의를 포함하는 쪽이 먼저 갖춰 두는 것을 전제하기 때문이다. */
#ifndef _PCIE_RCAR_H
#define _PCIE_RCAR_H

/* [한국어] config 접근 창의 주소 레지스터. 대상 BDF 와 오프셋을 한 워드로 조립해 여기
 * 써 넣는다. host 판 rcar_pcie_config_access() 가 아래 PCIE_CONF_BUS/DEV/FUNC
 * 매크로로 만든 값을 쓴다. 이 컨트롤러에는 ECAM 이 없어 이런 간접 창을 쓴다. */
#define PCIECAR			0x000010
/* [한국어] config 접근 제어 레지스터. 주소를 세운 뒤 여기에 활성 비트와 접근 종류를
 * 써서 창을 열고, 데이터를 주고받은 뒤 0 을 써서 닫는다. */
#define PCIECCTLR		0x000018
/* [한국어] config 접근 활성 비트(31번). 이 비트가 서 있는 동안만 PCIECDR 로 데이터가
 * 오간다. 닫지 않고 두면 다음 접근이 엉킨다. */
#define  PCIECCTLR_CCIE		BIT(31)
/* [한국어] type 0 config 접근. 바로 아래에 붙은 장치를 직접 지목한다.
 * 값이 (0 << 8) 이라 실제로는 0 이며, TYPE1 과 짝을 이뤄 의도를 드러내려고
 * 0 을 명시적으로 시프트한 형태로 적어 두었다. */
#define  TYPE0			(0 << 8)
/* [한국어] type 1 config 접근(8번 비트). 브리지 너머의 장치를 지목하며, 중간 브리지가
 * 목적지까지 전달한다. host 판이 대상 버스의 부모가 루트인지로 둘을 가른다. */
#define  TYPE1			BIT(8)
/* [한국어] config 접근 창의 데이터 레지스터. 창이 열린 동안 이 레지스터를 읽고 쓰면
 * 대상 장치의 config space 에 닿는다. ARM 에서는 없는 장치 접근이 외부 abort 를
 * 일으켜, host 판이 예외를 잡는 인라인 어셈블리로 이 레지스터를 다룬다. */
#define PCIECDR			0x000020
/* [한국어] 동작 모드 레지스터. 이 한 자리가 호스트와 엔드포인트를 가른다 —
 * host 판 rcar_pcie_hw_init() 은 1 을, ep 판 rcar_pcie_ep_hw_init() 은 0 을 쓴다.
 * 두 드라이버의 유일한 본질적 차이다. */
#define PCIEMSR			0x000028
/* [한국어] INTx 제어/상태 레지스터. host 판은 초기화 끝에 8번 비트부터 4비트를 세워
 * INTx 수신을 켜고, ep 판은 아래 ASTINTX 로 INTx 를 직접 발생시킨다.
 * host 판이 쓰는 그 4비트 필드의 의미는 코드만으로 확정할 수 없어 확인 못 함. */
#define PCIEINTXR		0x000400
/* [한국어] INTx assert 비트(16번). ep 판 rcar_pcie_ep_assert_intx() 가 이 비트를 세우고
 * 1ms 뒤 내려 한 번의 INTx 인터럽트를 만든다. 이미 서 있으면 앞선 assert 가
 * 진행 중이라는 뜻이라 거절한다. */
#define  ASTINTX		BIT(16)
/* [한국어] PHY 상태 레지스터. */
#define PCIEPHYSR		0x0007f0
/* [한국어] PHY 준비 완료 비트(0번). pcie-rcar.c 의 rcar_pcie_wait_for_phyrdy() 가
 * 5ms 간격으로 최대 10회 이 비트를 폴링한다. 두 드라이버 모두 이것이 서기
 * 전에는 링크 훈련으로 넘어가지 않는다. */
#define  PHYRDY			BIT(0)
/* [한국어] MSI 송신 레지스터. ep 판이 호스트로 MSI 를 보낼 때 벡터 번호를 여기 쓴다
 * (rcar_pcie_ep_assert_msi). host 판도 초기화 때 이 레지스터에 0x801f0000 을
 * 쓰지만 그 상수의 비트별 의미는 이 헤더에 정의가 없어 확인 못 함. */
#define PCIEMSITXR		0x000840

/* [한국어] 아래는 전송 제어 계열(0x02000 대역)이다. 링크를 켜고 끄는 일, 링크 상태
 * 확인, 오류 플래그, 그리고 MSI 수신이 이 대역에 모여 있다. */
/* Transfer control */
/* [한국어] 전송 제어 레지스터. 초기화 시작(0 쓰기)과 링크 훈련 개시(CFINIT 쓰기)에
 * 모두 쓰인다. 이 파일에서 가장 자주 등장하는 레지스터다. */
#define PCIETCTLR		0x02000
/* [한국어] 데이터 링크 다운 비트(3번). host 판 rcar_pcie_resume_noirq() 가 재개 후
 * 링크를 다시 세울지 판단하는 데 쓴다. */
#define  DL_DOWN		BIT(3)
/* [한국어] config 초기화/링크 훈련 개시 비트(0번). 이 비트를 쓰면 LTSSM 이 돌아
 * 상대와 링크를 맺는다. host 판은 쓴 뒤 링크가 설 때까지 기다리고,
 * ep 판 rcar_pcie_ep_start() 는 쓰기만 하고 기다리지 않는다 — 엔드포인트는
 * 호스트가 언제 훈련을 시작할지 알 수 없기 때문이다. */
#define  CFINIT			BIT(0)
/* [한국어] 전송 상태 레지스터. */
#define PCIETSTR		0x02004
/* [한국어] 데이터 링크 활성 비트(0번). pcie-rcar.c 의 rcar_pcie_wait_for_dl() 이
 * 5us 간격으로 최대 10000회 폴링한다. 상한이 50ms 로 PHY 대기보다 짧은 간격에
 * 훨씬 많은 횟수인 것은 링크 확립이 더 빠르게 끝날 수 있어서다. */
#define  DATA_LINK_ACTIVE	BIT(0)
/* [한국어] 오류 플래그 레지스터. host 판은 config 접근 전에 읽은 값을 그대로 되써서
 * 지우는데, 이 하드웨어의 상태 비트가 1 을 쓰면 지워지는 방식이기 때문이다. */
#define PCIEERRFR		0x02020
/* [한국어] Unsupported Request 비트(4번). config 접근 후 이 비트가 서 있으면 그 자리에
 * 장치가 없다는 뜻이라, host 판이 PCIBIOS_DEVICE_NOT_FOUND 로 답한다. */
#define  UNSUPPORTED_REQUEST	BIT(4)
/* [한국어] MSI 대기 플래그 레지스터. 벡터마다 비트 하나씩(총 32개)이며, MSI 가 도착하면
 * 해당 비트가 선다. host 판 rcar_pcie_msi_irq() 가 이 비트들을 훑어 처리하고,
 * irq_chip 의 ack 콜백이 1 을 써서 지운다. */
#define PCIEMSIFR		0x02044
/* [한국어] MSI 수신 주소 하위 워드. 장치가 이 주소로 메모리 쓰기를 보내면 컨트롤러가
 * MSI 로 해석한다. host 판은 레지스터 블록 자신의 물리 주소를 여기 쓴다 —
 * 상류 주석대로 R-Car 하드웨어에서 그 주소가 항상 하위 32비트 안에 있기 때문이다. */
#define PCIEMSIALR		0x02048
/* [한국어] MSI 주소 디코딩 활성 비트(0번). 주소의 일부가 아니라 플래그이므로,
 * host 판 rcar_compose_msi_msg() 는 장치에 알려 줄 주소를 만들 때 이 비트를
 * 지운다. teardown 은 이 레지스터를 통째로 0 으로 만들어 디코딩을 끈다. */
#define  MSIFE			BIT(0)
/* [한국어] MSI 수신 주소 상위 워드. 하위와 달리 활성 비트가 섞여 있지 않아 그대로 쓴다. */
#define PCIEMSIAUR		0x0204c
/* [한국어] MSI 인터럽트 활성 레지스터. 벡터마다 비트 하나씩이며, host 판의
 * irq_chip mask/unmask 콜백이 이 워드를 읽고-고쳐-쓴다. 32개 벡터가 한 워드를
 * 공유하기 때문에 그 갱신을 raw spinlock 으로 보호해야 한다. */
#define PCIEMSIIER		0x02050

/* [한국어] 아래는 안쪽 방향 창에서 "장치가 보게 될 PCI 주소" 를 담는 레지스터다. */
/* root port address */
/* [한국어] 루트 포트 주소 레지스터. 창 번호마다 4바이트씩 떨어져 있다.
 * pcie-rcar.c 의 rcar_pcie_set_inbound() 가 host 모드일 때만 이 레지스터를
 * 쓰고(idx 와 idx+1 에 하위·상위), ep 모드에서는 건드리지 않는다 —
 * 엔드포인트에서는 그 주소를 상대 호스트가 BAR 에 배정해 주기 때문이다. */
#define PCIEPRAR(x)		(0x02080 + ((x) * 0x4))

/* [한국어] 아래는 안쪽 방향 창의 "보드 쪽 실제 주소와 크기" 를 담는 레지스터다.
 * 창 하나가 0x20 바이트 간격이며, 64비트 주소를 위해 두 칸을 한 쌍으로 쓴다. */
/* local address reg & mask */
/* [한국어] 로컬 주소 레지스터. 장치의 DMA 가 실제로 닿을 보드 쪽 물리 주소다.
 * rcar_pcie_set_inbound() 가 idx 에 하위 32비트, idx+1 에 상위 32비트를 쓴다. */
#define PCIELAR(x)		(0x02200 + ((x) * 0x20))
/* [한국어] 로컬 주소 마스크 레지스터. 창 크기를 마스크 형태로 담으며, 하위 4비트에는
 * 크기가 아니라 아래 세 플래그가 들어간다. 그래서 크기를 계산하는 두 곳
 * (host 판 rcar_pcie_inbound_ranges, ep 판 rcar_pcie_ep_set_bar)이 마스크를
 * 만든 뒤 mask &= ~0xf 로 그 자리를 비운다. */
#define PCIELAMR(x)		(0x02208 + ((x) * 0x20))
/* [한국어] prefetch 가능 표시(3번 비트). host 판이 DT 의 dma-ranges 항목에
 * IORESOURCE_PREFETCH 가 있을 때만 더한다. */
#define  LAM_PREFETCH		BIT(3)
/* [한국어] 64비트 주소 창 표시(2번 비트). 두 드라이버 모두 항상 세운다 —
 * 창을 두 칸씩 쌍으로 쓰는 이유가 이것이다. */
#define  LAM_64BIT		BIT(2)
/* [한국어] 창 활성 비트(1번). 이것이 없으면 주소를 채워도 창이 동작하지 않는다.
 * 반대로 ep 판 clear_bar 는 이 레지스터를 0 으로 덮어 창을 끈다. */
#define  LAR_ENABLE		BIT(1)

/* [한국어] 아래는 바깥 방향 창(CPU -> PCI)이다. 안쪽 창과 마찬가지로 0x20 간격이지만,
 * 이쪽은 창 하나가 네 레지스터(하위/상위/마스크/제어)를 한 묶음으로 쓴다. */
/* PCIe address reg & mask */
/* [한국어] 바깥 창의 PCI 주소 하위 워드. pcie-rcar.c 의 rcar_pcie_set_outbound() 가
 * 하위 7비트를 지우고 쓰는데, 창 정렬이 128바이트 단위이기 때문이다. */
#define PCIEPALR(x)		(0x03400 + ((x) * 0x20))
/* [한국어] 바깥 창의 PCI 주소 상위 워드. */
#define PCIEPAUR(x)		(0x03404 + ((x) * 0x20))
/* [한국어] 바깥 창의 크기 마스크. set_outbound() 가 크기를 128 로 나눈 값에서 1 을 뺀
 * 뒤 7비트 왼쪽으로 밀어 쓴다. 크기가 128 이하면 마스크가 0 이다. */
#define PCIEPAMR(x)		(0x03408 + ((x) * 0x20))
/* [한국어] 바깥 창의 제어 레지스터. set_outbound() 가 맨 처음 0 을 써서 창을 끄고,
 * 주소와 마스크를 채운 뒤 마지막에 활성 비트를 써서 켠다. 이 순서 덕분에
 * 반쯤 설정된 창이 동작하는 일이 없다. */
#define PCIEPTCTLR(x)		(0x0340c + ((x) * 0x20))
/* [한국어] 바깥 창 활성 비트(31번). */
#define  PAR_ENABLE		BIT(31)
/* [한국어] I/O 공간 창 표시(8번 비트). 메모리 창이 아니라 I/O 창임을 알린다.
 * set_outbound() 가 자원 플래그를 보고 붙이며, ep 판 set_bar 도 I/O BAR 에
 * 같은 상수를 쓴다 — 안쪽/바깥 창이 같은 비트 위치를 공유하는 셈이다. */
#define  IO_SPACE		BIT(8)

/* [한국어] 아래는 이 컨트롤러 자신의 config space 다(0x010000 대역). PCI 표준
 * 레이아웃을 그대로 흉내 내어, 오프셋 x 를 워드 번호로 주면 표준 config 의
 * 같은 자리에 대응한다. host 판이 루트 버스 접근을 간접 창 대신 여기로
 * 돌리는 이유는, 이 컨트롤러가 자기 자신을 type 0/1 로 지목할 수 없어서다. */
/* Configuration */
/* [한국어] config space 워드 접근. 인자가 바이트 오프셋이 아니라 워드 번호다. */
#define PCICONF(x)		(0x010000 + ((x) * 0x4))
/* [한국어] INTx 비활성 비트(10번). 표준 config 의 Command 레지스터에 있는
 * INTx Disable 에 해당한다. ep 판 assert_intx() 가 이 비트를 보고,
 * 서 있으면 호스트가 INTx 를 원하지 않는 것이라 판단해 거절한다. */
#define  INTDIS			BIT(10)
/* [한국어] 전원 관리 capability 창. 이 헤더에 정의는 있으나 세 .c 파일 어디에서도
 * 직접 쓰지 않으며, 아래 RPMCAP() 도 사용처가 없다(전수 grep 확인). */
#define PMCAP(x)		(0x010040 + ((x) * 0x4))
/* [한국어] MSI capability 창. ep 판이 MSI 벡터 수를 알리고 읽는 데 쓴다. */
#define MSICAP(x)		(0x010050 + ((x) * 0x4))
/* [한국어] MSI Enable 비트(16번). 상대 호스트가 이 장치의 MSI 를 켜면 선다.
 * ep 판 get_msi() 와 assert_msi() 가 먼저 이 비트를 확인해, 꺼져 있으면
 * -EINVAL 로 답한다. */
#define  MSICAP0_MSIE		BIT(16)
/* [한국어] MMC(Multiple Message Capable) 필드의 시작 비트(17). ep 판 set_msi() 가
 * "2^N 개까지 가능" 의 N 을 이 자리로 밀어 넣는다. 능력을 알리는 쪽이다. */
#define  MSICAP0_MMESCAP_OFFSET	17
/* [한국어] MMSE(Multiple Message Enable) 필드의 시작 비트(20). 호스트가 실제로 허용한
 * 개수의 로그가 여기 담긴다. 능력이 아니라 결정을 담는 쪽이다. */
#define  MSICAP0_MMESE_OFFSET	20
/* [한국어] 그 MMSE 필드의 마스크(22:20, 3비트). ep 판 get_msi() 와 assert_msi() 가
 * 이 마스크로 떼어 내고 위 오프셋만큼 밀어 N 을 얻은 뒤 1 << N 으로 개수를 만든다. */
#define  MSICAP0_MMESE_MASK	GENMASK(22, 20)
/* [한국어] PCIe capability 창. 두 드라이버가 포트 종류·링크 속도·슬롯 정보를 여기 쓴다.
 * ep 판은 EXPCAP(1)/EXPCAP(2) 로 페이로드 크기까지 못박는다. */
#define EXPCAP(x)		(0x010070 + ((x) * 0x4))
/* [한국어] Virtual Channel capability 창. 직접 쓰이지는 않고 아래 RVCCAP() 를 통해서만
 * 쓰이며, 두 드라이버가 capability 목록을 끝내는 데 쓴다. */
#define VCCAP(x)		(0x010100 + ((x) * 0x4))

/* [한국어] 아래는 링크 계층 제어(0x011000 대역)다. 장치 신원, 링크 속도 협상,
 * 전원 관리 상태가 여기 모여 있다. */
/* link layer */
/* [한국어] 장치 ID 설정 레지스터 0. ep 판 write_header() 가 vendor ID(하위 16비트)와
 * device ID(상위 16비트)를 한 워드로 조립해 쓴다. 상대 호스트가 열거할 때
 * 읽어 갈 값이다. */
#define IDSETR0			0x011000
/* [한국어] 장치 ID 설정 레지스터 1. 클래스 코드 계열을 담는다. host 판은 자기를
 * PCI-to-PCI 브리지로 보이게 하려고 클래스만 쓰고, ep 판은 revision·prog-if·
 * subclass·baseclass 네 바이트를 모두 쌓아 쓴다. */
#define IDSETR1			0x011004
/* [한국어] 서브시스템 ID 설정 레지스터. ep 판만 쓴다 — 호스트 판은 브리지라
 * 서브시스템 신원을 꾸밀 이유가 없다. */
#define SUBIDSETR		0x011024
/* [한국어] 트랜잭션 계층 제어 레지스터. 두 드라이버 모두 TLCTLR + 1 위치에 50 을 써서
 * 완료 타이머 상한을 50ms 로 잡는다. +1 로 바이트를 건너뛸 수 있는 것은
 * rcar_rmw32() 가 오프셋 하위 2비트로 바이트 자리를 계산해 주기 때문이다. */
#define TLCTLR			0x011048
/* [한국어] MAC 상태 레지스터. 현재 링크 속도와 속도 변경 결과를 담는다. */
#define MACSR			0x011054
/* [한국어] 속도 변경 완료 비트(4번). host 판 rcar_pcie_force_speedup() 이 1ms 간격으로
 * 최대 1000회 이 비트를 기다린다. */
#define  SPCHGFIN		BIT(4)
/* [한국어] 속도 변경 실패 비트(6번). 완료 비트와 함께 서면 변경이 실패한 것이다.
 * 다만 host 판은 그것을 오류로 전하지 않는다 — 2.5GT/s 로도 동작하기 때문이다. */
#define  SPCHGFAIL		BIT(6)
/* [한국어] 속도 변경 성공 비트(7번). 코드는 이 비트를 직접 판정에 쓰지 않고,
 * 이전 결과를 지울 때 다른 두 비트와 함께 묶어서만 참조한다. */
#define  SPCHGSUC		BIT(7)
/* [한국어] 링크 속도 필드 마스크(19:16). 아래 두 상수와 비교해 현재 속도를 알아낸다. */
#define  LINK_SPEED		(0xf << 16)
/* [한국어] 2.5GT/s(Gen1) 값. 이 헤더에 정의는 있으나 세 .c 파일 어디에서도 쓰지 않는다
 * (전수 grep 확인). 코드는 5.0GT/s 인지만 확인하고 아니면 2.5GT/s 로 간주한다. */
#define  LINK_SPEED_2_5GTS	(1 << 16)
/* [한국어] 5.0GT/s(Gen2) 값. host 판이 현재 속도 확인과 목표 속도 지정에 모두 쓴다. */
#define  LINK_SPEED_5_0GTS	(2 << 16)
/* [한국어] MAC 제어 레지스터. 링크 훈련 동작을 조절한다. */
#define MACCTLR			0x011058
/* [한국어] NFTS(N_FTS, Fast Training Sequence 개수) 필드 마스크(23:16).
 * 상류 주석이 이름의 출처가 SH7786 이라고 밝혀 두었다. 아래 초기값에
 * 마스크 전체가 그대로 들어가므로 필드를 최대값으로 채우는 셈이다. */
#define  MACCTLR_NFTS_MASK	GENMASK(23, 16)	/* The name is from SH7786 */
/* [한국어] 속도 변경 개시 비트(24번). host 판이 이 비트를 세워 링크 속도 협상을
 * 시작하고, 시작 전에는 다른 주체가 진행 중인지 이 비트로 확인한다. */
#define  SPEED_CHANGE		BIT(24)
/* [한국어] 스크램블 비활성 비트(27번). 이 헤더에 정의는 있으나 세 .c 파일 어디에서도
 * 쓰지 않는다(전수 grep 확인). 보통 디버그·시험용 설정이다. */
#define  SCRAMBLE_DISABLE	BIT(27)
/* [한국어] LTSSM 비활성 비트(31번). 아래 초기값에 포함된다. */
#define  LTSMDIS		BIT(31)
/* [한국어] MACCTLR 초기값. LTSMDIS 와 NFTS 마스크 전체를 합친 값이다.
 * host 판은 초기화와 재개 시, ep 판은 start 시 이 값을 그대로 쓴다.
 * LTSMDIS 를 세운 채로 두는 이유는 코드만으로 확정할 수 없어 확인 못 함. */
#define  MACCTLR_INIT_VAL	(LTSMDIS | MACCTLR_NFTS_MASK)
/* [한국어] 전원 관리 상태 레지스터. L1 절전 진입 상태와 관련 이벤트를 담는다. */
#define PMSR			0x01105c
/* [한국어] L1 진입 완료 비트(31번). host 판 rcar_pcie_wakeup() 이 L1 진입을 지시한 뒤
 * 이 비트를 폴링하고, 끝나면 1 을 써서 지운다. */
#define  L1FAEG			BIT(31)
/* [한국어] PM_ENTER_L1 수신 비트(23번). 상대가 L1 진입 요청 DLLP 를 보냈음을 뜻한다.
 * 이 비트가 서 있는데 아직 L1 상태가 아니면 어중간한 상태라, wakeup() 이
 * 그 조합을 우회 조건으로 삼는다. */
#define  PMEL1RX		BIT(23)
/* [한국어] 전원 상태 필드 마스크(18:16). */
#define  PMSTATE		GENMASK(18, 16)
/* [한국어] L1 상태 값(3). 위 마스크로 떼어 낸 값이 이것과 같으면 이미 L1 이다. */
#define  PMSTATE_L1		(3 << 16)
/* [한국어] 전원 관리 제어 레지스터. */
#define PMCTLR			0x011060
/* [한국어] L1 진입 지시 비트(31번). wakeup() 이 어중간한 상태를 정리하려고 이 비트를
 * 써서 강제로 L1 에 들어가게 한다. 그러면 하드웨어가 스스로 L0s/L0 로 돌아온다. */
#define  L1IATN			BIT(31)

/* [한국어] MAC 상태 레지스터 2. host 판은 두 가지로 쓴다 — 링크 속도 능력 확인
 * (LINK_SPEED 필드)과 링크 폭 로그((data >> 20) & 0x3f). 후자의 필드 위치는
 * 이 헤더에 정의가 없어 확인 못 함. */
#define MACS2R			0x011078
/* [한국어] 클럭 소스 설정 레지스터. */
#define MACCGSPSETR		0x011084
/* [한국어] 속도 변경 사유 비트(31번). host 판이 이 비트를 0 으로 만들어 변경 사유를
 * "의도된 것" 으로 표시한다. */
#define  SPCNGRSN		BIT(31)

/* [한국어] 아래는 R-Car H1(1세대) 전용 PHY 창이다. 이 세대는 PHY 가 별도 드라이버가
 * 아니라 컨트롤러 레지스터로 직접 제어된다. */
/* R-Car H1 PHY */
/* [한국어] H1 PHY 주소 레지스터. 명령·레이트·레인·주소를 비트 필드로 조립해 쓴다.
 * ack 비트도 같은 레지스터에서 읽는다. */
#define H1_PCIEPHYADRR		0x04000c
/* [한국어] 쓰기 명령 비트(16번). host 판 phy_write_reg() 가 항상 세운다 —
 * 읽기 경로는 이 드라이버에 없다. */
#define  WRITE_CMD		BIT(16)
/* [한국어] PHY 응답 비트(24번). phy_wait_for_ack() 이 100us 간격으로 최대 100회 기다린다.
 * 실패해도 무시하는데, 상류 주석대로 오류는 데이터 링크가 내려가면 드러나기 때문이다. */
#define  PHY_ACK		BIT(24)
/* [한국어] 데이터 레이트 필드의 시작 비트(12). */
#define  RATE_POS		12
/* [한국어] 레인 번호 필드의 시작 비트(8). */
#define  LANE_POS		8
/* [한국어] PHY 레지스터 주소 필드의 시작 비트(0). 값이 0 이라 시프트가 무의미하지만,
 * 위 두 상수와 형식을 맞추려고 명시해 두었다. */
#define  ADR_POS		0
/* [한국어] H1 PHY 데이터 출력 레지스터. 주소를 쓰기 전에 여기에 값을 먼저 넣고,
 * 명령을 거둘 때 0 으로 되돌린다. */
#define H1_PCIEPHYDOUTR		0x040014

/* [한국어] 아래는 R-Car Gen2(2세대) 전용 PHY 창이다. H1 과 달리 주소·데이터·제어가
 * 세 레지스터로 분리되어 있고, 오프셋 대역도 완전히 다르다. */
/* R-Car Gen2 PHY */
/* [한국어] Gen2 PHY 주소 레지스터. */
#define GEN2_PCIEPHYADDR	0x780
/* [한국어] Gen2 PHY 데이터 레지스터. */
#define GEN2_PCIEPHYDATA	0x784
/* [한국어] Gen2 PHY 제어 레지스터. host 판이 1 을 썼다가 6 을 쓰는 것이 쓰기 절차인데,
 * 두 값의 의미는 이 헤더에 정의가 없어 확인 못 함. */
#define GEN2_PCIEPHYCTRL	0x78c

/* [한국어] 이 컨트롤러가 지원하는 MSI 벡터 수. host 판이 비트맵 크기와 IRQ 도메인
 * 크기로 쓰며, PCIEMSIFR / PCIEMSIIER 가 32비트 레지스터인 것과 맞아떨어진다. */
#define INT_PCI_MSI_NR		32

/* [한국어] 자기 config space 의 바이트 오프셋 접근. PCICONF(0) 이 시작 주소이고
 * 거기에 바이트 오프셋을 더하는 형태라, PCI_STATUS 같은 표준 상수를 그대로
 * 넣을 수 있다. rcar_rmw32() 가 하위 2비트로 바이트 자리를 계산해 준다. */
#define RCONF(x)		(PCICONF(0) + (x))
/* [한국어] 전원 관리 capability 의 바이트 오프셋 접근. 세 .c 파일 어디에서도 쓰지
 * 않는다(전수 grep 확인). */
#define RPMCAP(x)		(PMCAP(0) + (x))
/* [한국어] PCIe capability 의 바이트 오프셋 접근. 두 드라이버가 포트 종류, 링크 능력,
 * 슬롯 정보를 표준 상수로 지정할 때 쓴다. 이 헤더에서 가장 많이 쓰이는
 * 접근 매크로다. */
#define REXPCAP(x)		(EXPCAP(0) + (x))
/* [한국어] Virtual Channel capability 의 바이트 오프셋 접근. 두 드라이버 모두
 * RVCCAP(0) 의 상위 20비트를 0 으로 만들어 capability 목록을 끝낸다. */
#define RVCCAP(x)		(VCCAP(0) + (x))

/* [한국어] 버스 번호를 config 주소의 31:24 자리로 민다. 0xff 로 먼저 자르는 것은
 * 인자가 8비트를 넘겨도 이웃 필드를 침범하지 않게 하려는 것이다. */
#define PCIE_CONF_BUS(b)	(((b) & 0xff) << 24)
/* [한국어] 장치 번호를 23:19 자리로 민다. PCI 의 장치 번호가 5비트라 0x1f 로 자른다. */
#define PCIE_CONF_DEV(d)	(((d) & 0x1f) << 19)
/* [한국어] 기능 번호를 18:16 자리로 민다. 기능 번호는 3비트다.
 * 위 셋을 OR 하고 레지스터 오프셋을 더한 값이 PCIECAR 에 들어간다. */
#define PCIE_CONF_FUNC(f)	(((f) & 0x7) << 16)

/* [한국어] 바깥 방향 창의 개수. host 판이 DT 의 창 목록을 이 개수만큼 하드웨어에
 * 반영하고, ep 판은 DT 에서 memory0..memory3 네 자원을 모두 요구한다. */
#define RCAR_PCI_MAX_RESOURCES	4
/* [한국어] 안쪽 방향 창의 개수. 다만 두 드라이버 모두 창을 두 칸씩 쌍으로 쓰므로
 * 실제로 표현할 수 있는 구간은 세 개다. ep 판이 BAR 0/2/4 세 개만 지원한다고
 * 알리는 것이 그 결과다. */
#define MAX_NR_INBOUND_MAPS	6

/* [한국어] host 판과 ep 판이 공유하는 최소 상태. 두 드라이버 모두 자기 상태 구조체의
 * 첫 필드로 이것을 박아 두어, 주소가 같다는 성질로 두 타입 사이를 오간다. */
struct rcar_pcie {
	/* [한국어] 로그와 devm 할당, DT 조회의 기준이 되는 device.
	 * 설정자: 각 드라이버의 probe 가 플랫폼 device 를 넣는다.
	 * 읽는 자: pcie-rcar.c 의 헬퍼들과 두 드라이버 전반.
	 * 값 범위: 유효한 device 포인터. NULL 이 되는 경로는 없다.
	 * 동기화: probe 에서 한 번 설정된 뒤 읽기 전용이다. */
	struct device		*dev;
	/* [한국어] ioremap 된 레지스터 블록의 기준 가상 주소. 이 헤더의 모든 오프셋이
	 * 이 주소에 더해져 실제 접근이 된다.
	 * 설정자: host 판 rcar_pcie_get_resources(), ep 판 rcar_pcie_ep_get_pdata().
	 * 읽는 자: rcar_pci_read_reg()/write_reg() 를 통해 사실상 모든 코드.
	 * 값 범위: 유효한 __iomem 포인터. host 판은 이 물리 주소를 MSI 수신
	 *   주소로도 쓴다.
	 * 동기화: probe 에서 한 번 설정된 뒤 읽기 전용이다. */
	void __iomem		*base;
};

/* [한국어] config 접근 방향을 나타내는 익명 enum. 이름이 없는 것은 타입으로 쓰지 않고
 * 값만 쓰기 때문이며, 실제로 host 판 rcar_pcie_config_access() 는 이 값을
 * unsigned char 로 받는다. */
enum {
	/* [한국어] 읽기 접근(0). host 판의 read_conf 가 이 값으로 부르고, write_conf 도
	 * 부분 쓰기를 위해 먼저 이 값으로 한 번 읽는다.
	 * 설정자/읽는 자: rcar_pcie_config_access() 의 access_type 인자.
	 * 값 범위: 0.
	 * 동기화: 상수라 해당 없음. */
	RCAR_PCI_ACCESS_READ,
	/* [한국어] 쓰기 접근(1). write_conf 가 읽어서 고친 워드를 되쓸 때만 쓴다.
	 * 설정자/읽는 자: 위와 동일.
	 * 값 범위: 1.
	 * 동기화: 상수라 해당 없음. */
	RCAR_PCI_ACCESS_WRITE,
};

/* [한국어] 레지스터 쓰기. base + reg 에 writel 하는 한 줄이다(pcie-rcar.c:13).
 * 인자 순서가 (값, 오프셋)이라 writel 과 같고 readl 계열과 반대라는 점에 주의. */
void rcar_pci_write_reg(struct rcar_pcie *pcie, u32 val, unsigned int reg);
/* [한국어] 레지스터 읽기. base + reg 에서 readl 한다(pcie-rcar.c:18). */
u32 rcar_pci_read_reg(struct rcar_pcie *pcie, unsigned int reg);
/* [한국어] 바이트 오프셋 단위 읽고-고쳐-쓰기(pcie-rcar.c:23). where 의 하위 2비트로
 * 시프트를 구해 mask 를 그 자리에서 지우고 data 를 얹는다. 덕분에 호출자가
 * PCI_STATUS 같은 표준 바이트 오프셋을 그대로 넘길 수 있다. */
void rcar_rmw32(struct rcar_pcie *pcie, int where, u32 mask, u32 data);
/* [한국어] PHY 준비 대기(pcie-rcar.c:32). PCIEPHYSR 의 PHYRDY 를 5ms 간격 최대 10회
 * 폴링한다. 실패 시 -ETIMEDOUT. msleep 을 쓰므로 프로세스 컨텍스트 전용이다. */
int rcar_pcie_wait_for_phyrdy(struct rcar_pcie *pcie);
/* [한국어] 데이터 링크 확립 대기(pcie-rcar.c:44). PCIETSTR 의 DATA_LINK_ACTIVE 를
 * 5us 간격 최대 10000회 폴링한다. udelay + cpu_relax 라 바쁘게 기다린다. */
int rcar_pcie_wait_for_dl(struct rcar_pcie *pcie);
/* [한국어] 바깥 방향 창 하나를 연다(pcie-rcar.c:58). 창을 끄고 → 마스크·주소를 채우고
 * → 다시 켜는 순서라 반쯤 설정된 창이 동작하지 않는다. */
void rcar_pcie_set_outbound(struct rcar_pcie *pcie, int win,
			    /* [한국어] resource_entry 를 받는 이유는 host 판에서 브리지 창 목록 항목이 그대로
			     * 들어오기 때문이다. ep 판은 그 형식에 맞춰 임시 구조체를 꾸며 넘긴다. */
			    struct resource_entry *window);
/* [한국어] 안쪽 방향 창 한 쌍을 연다(pcie-rcar.c:95). idx 와 idx+1 에 64비트 주소의
 * 하위·상위를 나눠 쓴다. */
void rcar_pcie_set_inbound(struct rcar_pcie *pcie, u64 cpu_addr,
			   /* [한국어] 마지막 인자 host 가 두 드라이버의 차이를 흡수한다. true 면 PCIEPRAR(장치가
			    * 볼 PCI 주소)까지 쓰고, false 면 건드리지 않는다 — 엔드포인트에서는 그
			    * 주소를 상대 호스트가 BAR 에 배정해 주기 때문이다. */
			   u64 pci_addr, u64 flags, int idx, bool host);

/* [한국어] 헤더 가드 끝. */
#endif
