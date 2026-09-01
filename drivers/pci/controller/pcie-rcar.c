// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe driver for Renesas R-Car SoCs
 *  Copyright (C) 2014-2020 Renesas Electronics Europe Ltd
 *
 * Author: Phil Edworthy <phil.edworthy@renesas.com>
 */

/* [한국어] msleep()/udelay() 선언. 아래 두 대기 함수가 서로 다른 방식을 쓴다. */
/*
 * [한국어 설명] R-Car PCIe 의 host/EP 공용 레지스터 접근과 창 설정 (pcie-rcar.c)
 *
 * === 파일의 역할 ===
 * Renesas R-Car SoC 의 PCIe 컨트롤러를 다루는 두 드라이버 —
 * 루트 컴플렉스인 pcie-rcar-host.c 와 엔드포인트인 pcie-rcar-ep.c — 가
 * 공유하는 최소한의 공통 코드다. 담고 있는 것은 일곱 함수뿐이고, 크게
 * 세 묶음으로 나뉜다. (1) 레지스터 접근자 셋(read/write/rmw32), (2) 하드웨어
 * 준비를 기다리는 폴링 함수 둘(PHY 준비, 데이터 링크 활성), (3) 주소 변환
 * 창 설정 둘(아웃바운드, 인바운드).
 * 드라이버라기보다 라이브러리에 가깝다 — probe 도 없고 모듈 진입점도 없으며,
 * 자기 상태를 갖지 않고 호출자가 넘긴 struct rcar_pcie 만 다룬다.
 * 두 대기 함수가 정반대 방식을 쓰는 것이 이 파일에서 가장 눈에 띄는 대비다.
 * PHY 대기는 5ms × 10회로 잠들며 기다리고, 데이터 링크 대기는 5us × 10000회로
 * 바쁘게 기다린다. 총 시간은 둘 다 50ms 로 같은데, 기다리는 사건의 시간
 * 규모가 밀리초와 마이크로초로 달라 그에 맞춘 것이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * R-Car PCIe 스택은 네 파일이다. 레지스터 지도와 struct rcar_pcie 를 정의하는
 * pcie-rcar.h, 그 위의 공통 코드인 이 파일, 그리고 서로 반대 역할인
 * pcie-rcar-host.c 와 pcie-rcar-ep.c. 두 드라이버가 같은 IP 를 각각 RC 와 EP 로
 * 몰기 때문에 레지스터 접근과 창 설정 절차가 거의 같고, 그 겹치는 부분만
 * 여기로 뽑아냈다.
 * 이 파일에는 진입점이 없고 모든 함수가 EXPORT 없이 두 드라이버에서 직접
 * 불린다(같은 모듈로 링크되기 때문이다). 호출 방향은 언제나 위에서 아래로
 * 한 방향이며, 콜백으로 되불리는 일이 없다.
 * 실행 컨텍스트는 전부 프로세스 컨텍스트다. probe 와 재개(resume) 경로에서
 * 불리며, wait_for_phyrdy() 는 최대 50ms 잠들고 wait_for_dl() 은 같은 시간
 * 동안 CPU 를 점유한다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pcie-rcar-host.c 와 pcie-rcar-ep.c 가 이 파일의 일곱 함수를 모두 쓴다.
 * 특히 set_inbound() 의 host 인자가 두 드라이버를 가르는 지점으로,
 * RC 는 true 를 넘겨 PCI 주소 레지스터까지 채우고 EP 는 false 를 넘겨 건너뛴다.
 * 옆쪽: pcie-rcar.h 의 struct rcar_pcie(base 포인터가 전부다)와 레지스터
 * 오프셋 매크로 — PCIEPHYSR/PHYRDY, PCIETSTR/DATA_LINK_ACTIVE,
 * PCIEPTCTLR/PCIEPAMR/PCIEPAUR/PCIEPALR(아웃바운드),
 * PCIEPRAR/PCIELAR/PCIELAMR(인바운드), 그리고 PAR_ENABLE 과 IO_SPACE 비트.
 * 아래쪽: readl/writel 과 msleep/udelay/cpu_relax, 그리고 PCI 코어의
 * pci_pio_to_address() — I/O 자원의 논리 포트 번호를 물리 주소로 바꾸는 데 쓴다.
 * 데이터 흐름: DT 의 ranges 항목 → struct resource_entry → set_outbound() 이
 * 창 레지스터로 변환 → CPU 주소가 PCI 주소로 나간다. 반대로 인바운드는
 * 엔드포인트의 DMA 주소를 시스템 메모리 주소로 되돌린다.
 * 공유 상태: 없다. 전역 변수도 static 변수도 두지 않고, 상태는 전부 호출자가
 * 소유한 struct rcar_pcie 안에 있다.
 *
 * === 주요 함수/구조체 요약 ===
 * - rcar_pci_write_reg() / rcar_pci_read_reg(): 레지스터 창 접근자.
 *   _relaxed 가 아닌 판을 쓰는 이유는 같은 파일의 폴링 루프가 이 접근자로
 *   상태를 읽기 때문이다 — 배리어가 없으면 낡은 값을 재사용해 무한 루프가 된다.
 * - rcar_rmw32(): 32비트 접근만 지원하는 하드웨어에서 필드 단위 갱신을
 *   흉내 낸다. mask 와 data 를 시프트 전 값으로 받아 바이트 위치 계산을
 *   함수 안으로 숨긴다.
 * - rcar_pcie_wait_for_phyrdy(): 5ms × 10회, msleep 으로 잠들며 대기.
 * - rcar_pcie_wait_for_dl(): 5us × 10000회, udelay + cpu_relax 로 바쁜 대기.
 *   같은 50ms 를 두 방식으로 나눈 이유가 이 파일의 작은 볼거리다.
 * - rcar_pcie_set_outbound(): 창을 끄고 → 주소·크기를 설정하고 → 다시 켜는
 *   순서가 핵심이다. 크기는 128바이트 단위 마스크로 표현하며, I/O 자원만
 *   pci_pio_to_address() 로 한 단계 더 변환한다.
 * - rcar_pcie_set_inbound(): 언제나 64비트 창으로 설정한다. DT range 파서가
 *   32비트와 64비트를 구분하지 않기 때문이며, 그래서 idx 와 idx+1 을 한 쌍으로
 *   쓰고 상위 칸의 플래그 레지스터는 0 으로 비운다.
 * - 이 파일에는 구조체 정의도 전역 변수도 없다. struct rcar_pcie 는
 *   pcie-rcar.h 소유이고, 이 파일은 그것을 읽기만 한다.
 */

#include <linux/delay.h>
/* [한국어] PCI 코어 공개 API — struct resource_entry, pci_pio_to_address(),
 * IORESOURCE_IO 플래그. */
#include <linux/pci.h>
/* [한국어] R-Car PCIe 공용 헤더. struct rcar_pcie 와 레지스터 오프셋 매크로
 * (PCIEPTCTLR/PCIEPAMR/PCIEPAUR/PCIEPALR/PCIEPRAR/PCIELAR/PCIELAMR)가 여기서 온다. */
#include "pcie-rcar.h"

/* [한국어]
 * rcar_pci_write_reg - 컨트롤러 레지스터 창에 32비트를 쓴다
 *
 * @pcie: 컨트롤러 객체. pcie->base 가 ioremap 된 창의 시작이다.
 * @val: 쓸 값.
 * @reg: 창 안에서의 바이트 오프셋.
 *
 * 이 파일은 R-Car PCIe 의 host 판(pcie-rcar-host.c)과 EP 판(pcie-rcar-ep.c)이
 * 공유하는 공통 코드다. 두 드라이버가 같은 레지스터 창을 쓰므로 접근자와
 * 기본 시퀀스를 여기 모아 두었다.
 *
 * 배리어를 포함한 writel 을 쓰는 이유는 이 창을 폴링하는 대기 함수들이
 * 같은 파일에 있기 때문이다 — _relaxed 판이면 컴파일러나 CPU 가 접근 순서를
 * 바꿔 폴링이 낡은 값을 볼 수 있다.
 *
 * 실행 컨텍스트: probe 와 링크 관리 경로. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pcie-rcar-host.c / pcie-rcar-ep.c 의 모든 레지스터 쓰기, 그리고 이 파일의
 *   rcar_rmw32() / set_outbound() / set_inbound() → [이 함수] → writel()
 */
void rcar_pci_write_reg(struct rcar_pcie *pcie, u32 val, unsigned int reg)
{

	/* [한국어] 컨트롤러 레지스터 창에 32비트를 쓴다. 배리어를 포함한 writel 을 쓰는 이유는
	 * 아래 대기 함수들이 이 창을 폴링하기 때문이다 — 배리어가 없으면 낡은 값을
	 * 재사용해 무한 루프가 될 수 있다. */
	writel(val, pcie->base + reg);
}
/* [한국어]
 * rcar_pci_read_reg - 컨트롤러 레지스터 창에서 32비트를 읽는다
 *
 * @pcie: 컨트롤러 객체.
 * @reg: 창 안에서의 바이트 오프셋.
 * @return: 읽은 32비트 값.
 *
 * 쓰기 쪽과 대칭이며 같은 이유로 배리어를 포함한 readl 을 쓴다.
 * 아래 두 대기 함수가 이 접근자로 상태 레지스터를 폴링하므로,
 * 배리어가 없으면 무한 루프가 될 수 있다.
 *
 * 실행 컨텍스트: probe, 링크 관리, 그리고 폴링 루프 안.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   host/ep 드라이버와 이 파일의 rmw32() / wait_for_phyrdy() / wait_for_dl()
 *     → [이 함수] → readl()
 */
u32 rcar_pci_read_reg(struct rcar_pcie *pcie, unsigned int reg)
{

	/* [한국어] 같은 창에서 32비트를 읽는다. */
	return readl(pcie->base + reg);
}
/* [한국어]
 * rcar_rmw32 - 레지스터의 일부 필드만 읽기-수정-쓰기로 갱신한다
 *
 * @pcie: 컨트롤러 객체.
 * @where: 바이트 오프셋. 하위 2비트가 워드 안의 바이트 위치를 뜻한다.
 * @mask: 바꿀 필드의 폭(시프트 전 기준).
 * @data: 넣을 값(역시 시프트 전 기준).
 *
 * 이 하드웨어가 32비트 접근만 지원하므로, 워드 안의 특정 바이트나 필드만
 * 바꾸려면 워드 전체를 읽어 고친 뒤 되써야 한다.
 *
 * mask 와 data 를 시프트 전 값으로 받는 것이 이 함수의 설계다 —
 * 호출자는 "오프셋 where 에 mask 폭으로 data 를 쓴다" 고만 생각하면 되고,
 * 바이트 위치 계산은 이 함수가 처리한다.
 *
 * [상류 코드 관찰] 읽기와 쓰기 사이에 락이 없다. 이 함수를 부르는 경로가
 * probe 와 링크 관리로 직렬화되어 있다는 전제 위에 있다.
 *
 * 실행 컨텍스트: probe 와 링크 관리 경로.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   host/ep 드라이버의 config 헤더 조작 → [이 함수]
 *     → rcar_pci_read_reg() → rcar_pci_write_reg()
 */
void rcar_rmw32(struct rcar_pcie *pcie, int where, u32 mask, u32 data)
{
	/* [한국어] 바이트 오프셋을 비트 시프트로 바꾼다. BITS_PER_BYTE(8)를 곱하는 것이라
	 * 오프셋 하위 2비트가 워드 안의 바이트 위치를 뜻한다. */
	unsigned int shift = BITS_PER_BYTE * (where & 3);
	/* [한국어] 워드 경계로 내린 주소에서 현재 값을 읽는다. 이 하드웨어가 32비트 접근만
	 * 지원하므로 부분 갱신도 워드 단위 읽기-수정-쓰기로 해야 한다. */
	u32 val = rcar_pci_read_reg(pcie, where & ~3);

	/* [한국어] 바꿀 자리의 비트를 지운다. mask 는 폭이고 shift 가 위치라, 둘을 조합해
	 * 정확히 그 필드만 비운다. */
	val &= ~(mask << shift);
	/* [한국어] 새 값을 같은 자리에 넣는다. */
	val |= data << shift;
	/* [한국어] 합친 워드를 되쓴다. */
	rcar_pci_write_reg(pcie, val, where & ~3);
}
/* [한국어]
 * rcar_pcie_wait_for_phyrdy - PHY 가 준비될 때까지 기다린다
 *
 * @pcie: 컨트롤러 객체.
 * @return: 0 = PHY 준비 완료, -ETIMEDOUT = 50ms 안에 준비되지 않음.
 *
 * PHY 상태 레지스터의 준비 비트를 5ms 간격으로 최대 10회 확인한다.
 *
 * msleep 을 쓰는 것이 아래 wait_for_dl() 과 대비되는 점이다. PHY 초기화는
 * 밀리초 단위로 오래 걸려 그동안 CPU 를 붙잡고 있을 이유가 없고,
 * 이 함수가 프로세스 컨텍스트에서만 불리므로 잠들어도 안전하다.
 *
 * 실행 컨텍스트: probe 와 재개 경로, 프로세스 컨텍스트. 최대 50ms 잠든다.
 *
 * 에러 경로: 시간 초과만 오류다. 호출자는 그것을 보고 초기화를 중단한다.
 *
 * 호출 체인:
 *   pcie-rcar-host.c / pcie-rcar-ep.c 의 PHY 초기화 → [이 함수]
 *     → rcar_pci_read_reg(PCIEPHYSR) → msleep()
 */
int rcar_pcie_wait_for_phyrdy(struct rcar_pcie *pcie)
{
	/* [한국어] 최대 10회 시도. 아래 5ms 대기와 곱하면 최대 50ms 다. */
	unsigned int timeout = 10;

	/* [한국어] 남은 횟수가 있는 동안 반복한다. timeout-- 가 조건 안에 있어
	 * 0 이 되는 순간 루프가 끝난다. */
	while (timeout--) {
		/* [한국어] PHY 상태 레지스터에서 준비 완료 비트를 확인한다. */
		if (rcar_pci_read_reg(pcie, PCIEPHYSR) & PHYRDY)
			/* [한국어] 준비됐으면 성공. */
			return 0;
		/* [한국어] 5ms 잠든다. msleep 을 쓰는 것은 PHY 초기화가 밀리초 단위로 오래 걸리고
		 * 이 함수가 프로세스 컨텍스트에서만 불리기 때문이다. */
		msleep(5);
	}
	/* [한국어] 10회를 다 써도 준비되지 않으면 시간 초과. */
	return -ETIMEDOUT;
}

/* [한국어]
 * rcar_pcie_wait_for_dl - 데이터 링크가 활성화될 때까지 기다린다
 *
 * @pcie: 컨트롤러 객체.
 * @return: 0 = 링크 활성, -ETIMEDOUT = 50ms 안에 올라오지 않음.
 *
 * 전송 상태 레지스터의 데이터 링크 활성 비트를 5us 간격으로 최대 10000회
 * 확인한다. 총 대기 시간은 phyrdy 쪽과 같은 50ms 지만 방식이 정반대다.
 *
 * udelay 로 바쁜 대기를 하는 이유: 링크 훈련은 마이크로초 단위로 진행되므로,
 * 잠들었다 깨는 비용이 대기 시간 자체보다 커진다. 그래서 횟수를 1000배 늘리고
 * 한 번의 대기를 1000배 줄였다. cpu_relax() 는 그 바쁜 대기가 하이퍼스레딩
 * 환경에서 형제 스레드를 굶기지 않게 하고, 컴파일러가 루프를 없애지 못하게 한다.
 *
 * 실행 컨텍스트: 링크 훈련 직후 경로. 최대 50ms 동안 CPU 를 점유한다.
 *
 * 에러 경로: 시간 초과만 오류다.
 *
 * 호출 체인:
 *   host/ep 드라이버의 링크 훈련 → [이 함수]
 *     → rcar_pci_read_reg(PCIETSTR) → udelay() → cpu_relax()
 */
int rcar_pcie_wait_for_dl(struct rcar_pcie *pcie)
{
	/* [한국어] 최대 10000회. 아래 5us 대기와 곱하면 최대 50ms 로, PHY 대기와 총 시간은 같다. */
	unsigned int timeout = 10000;

	/* [한국어] 남은 횟수가 있는 동안 반복. */
	while (timeout--) {
		/* [한국어] 전송 상태 레지스터에서 데이터 링크 활성 비트를 확인한다. */
		if ((rcar_pci_read_reg(pcie, PCIETSTR) & DATA_LINK_ACTIVE))
			/* [한국어] 활성이면 성공. */
			return 0;
		/* [한국어] 5us 만 기다린다. PHY 대기와 달리 udelay(바쁜 대기)를 쓰는 이유는
		 * 링크 훈련이 마이크로초 단위로 진행되어 잠들었다 깨는 비용이 대기 시간보다
		 * 커지기 때문이다. 그래서 횟수가 1000배 많고 한 번의 대기는 1000배 짧다. */
		udelay(5);
		/* [한국어] 바쁜 대기 루프임을 CPU 에 알린다. 하이퍼스레딩 환경에서 형제 스레드에
		 * 실행 자원을 양보하고, 컴파일러가 루프를 최적화로 없애지 못하게 한다. */
		cpu_relax();
	}

	/* [한국어] 시간 초과. */
	return -ETIMEDOUT;
}

/* [한국어]
 * rcar_pcie_set_outbound - 아웃바운드 주소 변환 창 하나를 설정한다
 *
 * @pcie: 컨트롤러 객체.
 * @win: 창 번호.
 * @window: DT ranges 에서 파싱된 자원 항목. res 와 offset 을 쓴다.
 *
 * CPU 가 낸 주소를 PCI 버스 주소로 바꿔 링크 너머로 내보내는 창을 연다.
 *
 * 순서가 이 함수의 핵심이다. 먼저 제어 레지스터를 0 으로 써서 창을 끄고,
 * 주소와 크기를 모두 설정한 뒤, 마지막에 다시 제어 레지스터를 써서 켠다.
 * 중간에 창이 살아 있으면 엉뚱한 주소 구간이 잠깐 열리기 때문이다.
 *
 * 크기 계산은 위 영어 주석이 밝히듯 128바이트 단위다. 2의 거듭제곱으로 올린
 * 크기를 128 로 나누고 1 을 빼면 그 크기를 덮는 비트 마스크가 되고,
 * 레지스터에 넣을 때 7비트 왼쪽으로 밀어 바이트 단위로 되돌린다.
 * 128바이트 이하면 마스크가 0 인 최소 창이다.
 *
 * I/O 자원은 주소 변환이 한 단계 더 필요하다. pci_pio_to_address() 로
 * 리눅스의 논리 포트 번호를 실제 물리 주소로 바꾼 뒤 오프셋을 뺀다 —
 * I/O 포트가 메모리에 매핑된 아키텍처에서 필요한 처리다.
 *
 * 실행 컨텍스트: probe 와 재개 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 반환값도 없고 유효성 검사도 하지 않는다 —
 * 호출자가 창 번호와 자원을 이미 검증했다는 전제다.
 *
 * 호출 체인:
 *   pcie-rcar-host.c 의 자원 설정 경로 → [이 함수] → rcar_pci_write_reg() ×5
 */
void rcar_pcie_set_outbound(struct rcar_pcie *pcie, int win,
			    struct resource_entry *window)
{
	/* Setup PCIe address space mappings for each resource */
	/* [한국어] 이 창이 대응할 자원. */
	struct resource *res = window->res;
	/* [한국어] 변환된 시작 주소. */
	resource_size_t res_start;
	/* [한국어] 자원 크기. */
	resource_size_t size;
	/* [한국어] PAMR 에 넣을 마스크. */
	u32 mask;
	/* [한국어] 먼저 창 제어 레지스터를 0 으로 써서 창을 끈다. 주소를 바꾸는 도중
	 * 창이 살아 있으면 엉뚱한 구간이 잠깐 열리기 때문이다. */
	rcar_pci_write_reg(pcie, 0x00000000, PCIEPTCTLR(win));

	/*
	 * The PAMR mask is calculated in units of 128Bytes, which
	 * keeps things pretty simple.
	 */
	/* [한국어] 자원 크기를 얻는다. */
	size = resource_size(res);
	/* [한국어] 128바이트를 넘으면, */
	if (size > 128)
		/* [한국어] 위 영어 주석대로 마스크를 128바이트 단위로 계산한다. 2의 거듭제곱으로
		 * 올린 크기를 128 로 나누고 1 을 빼면, 그 크기를 덮는 비트 마스크가 된다. */
		mask = (roundup_pow_of_two(size) / SZ_128) - 1;

	else
		/* [한국어] 128바이트 이하면 마스크가 0 — 최소 단위 하나짜리 창이다. */
		mask = 0x0;
	/* [한국어] 마스크를 7비트 왼쪽으로 밀어 쓴다. 128 = 2^7 이므로, 128바이트 단위 값을
	 * 바이트 단위 마스크로 되돌리는 시프트다. */
	rcar_pci_write_reg(pcie, mask << 7, PCIEPAMR(win));
	/* [한국어] I/O 자원이면, */
	if (res->flags & IORESOURCE_IO)
		/* [한국어] pci_pio_to_address() 로 리눅스의 논리 포트 번호를 실제 물리 주소로 바꾼 뒤
		 * 버스 주소로 변환한다. I/O 포트가 메모리에 매핑된 아키텍처에서 필요한 단계다. */
		res_start = pci_pio_to_address(res->start) - window->offset;
	else
		/* [한국어] 메모리 자원이면 CPU 주소에서 오프셋만 빼면 된다. */
		res_start = res->start - window->offset;

	/* [한국어] 버스 주소의 상위 32비트를 쓴다. */
	rcar_pci_write_reg(pcie, upper_32_bits(res_start), PCIEPAUR(win));

	/* [한국어] 하위 32비트를 쓰되 하위 7비트를 지운다 — 창이 128바이트 정렬이라
	 * 그 아래 비트는 의미가 없다. */
	rcar_pci_write_reg(pcie, lower_32_bits(res_start) & ~0x7F,
			   PCIEPALR(win));
	/* First resource is for IO */
	/* [한국어] 창 활성화 비트를 세운다. */
	mask = PAR_ENABLE;
	/* [한국어] I/O 자원이면(옆의 상류 주석은 첫 자원이 IO 라고 적었으나, 실제 판정은
	 * 순서가 아니라 플래그로 한다), */
	if (res->flags & IORESOURCE_IO)
		/* [한국어] I/O 공간 표시 비트를 함께 세운다. */
		mask |= IO_SPACE;
	/* [한국어] 제어 레지스터에 써서 창을 켠다. 주소를 모두 설정한 뒤 마지막에 켜는
	 * 순서가 66번 줄의 끄기와 짝을 이룬다. */
	rcar_pci_write_reg(pcie, mask, PCIEPTCTLR(win));

}
/* [한국어]
 * rcar_pcie_set_inbound - 인바운드 주소 변환 창 한 쌍을 설정한다
 *
 * @pcie: 컨트롤러 객체.
 * @cpu_addr: 이 창이 향할 CPU(로컬) 주소.
 * @pci_addr: 호스트가 볼 PCI 주소. EP 모드에서는 쓰지 않는다.
 * @flags: 크기와 활성화 비트를 담은 변환 플래그.
 * @idx: 창 번호. 이 함수는 idx 와 idx+1 두 칸을 한 쌍으로 쓴다.
 * @host: true = 루트 컴플렉스 모드, false = 엔드포인트 모드.
 *
 * 엔드포인트가 DMA 로 낸 주소를 시스템 메모리 주소로 바꾸는 창이다.
 *
 * 두 가지가 이 함수의 특징이다.
 * 첫째, 위 영어 주석대로 언제나 64비트 창으로 설정한다. DT range 파서가
 * 32비트와 64비트 항목을 구분하지 않아 어느 쪽인지 알 수 없기 때문이며,
 * 그래서 idx 와 idx+1 두 칸을 한 쌍으로 쓰고 상위 칸의 플래그는 비워 둔다.
 * 둘째, host 인자로 PCI 주소 레지스터를 쓸지 가른다. EP 모드에서는 호스트가
 * BAR 로 주소를 정하므로 이쪽에서 쓸 값이 없다 — 같은 IP 를 두 방향으로 쓰는
 * 이 드라이버 구조가 인자 하나로 드러나는 지점이다.
 *
 * 실행 컨텍스트: probe 와 재개 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pcie-rcar-host.c 의 인바운드 설정 / pcie-rcar-ep.c 의 set_bar
 *     → [이 함수] → rcar_pci_write_reg() ×4~6
 */
void rcar_pcie_set_inbound(struct rcar_pcie *pcie, u64 cpu_addr,
			   u64 pci_addr, u64 flags, int idx, bool host)
{
	/*
	 * Set up 64-bit inbound regions as the range parser doesn't
	 * distinguish between 32 and 64-bit types.
	 */
	/* [한국어] 호스트(RC) 모드에서만 PCI 주소 레지스터를 쓴다. EP 모드에서는 호스트가
	 * BAR 로 주소를 정하므로 이쪽에서 쓸 값이 없다 — 이 함수가 host 인자를
	 * 받는 이유가 그것이다. */
	if (host)
		rcar_pci_write_reg(pcie, lower_32_bits(pci_addr),
				   PCIEPRAR(idx));
	/* [한국어] CPU(로컬) 주소는 두 모드 모두 쓴다. */
	rcar_pci_write_reg(pcie, lower_32_bits(cpu_addr), PCIELAR(idx));

	/* [한국어] 변환 플래그(크기와 활성화 비트)를 쓴다. */
	rcar_pci_write_reg(pcie, flags, PCIELAMR(idx));
	/* [한국어] 호스트 모드면, */
	if (host)
		/* [한국어] PCI 주소의 상위 32비트를 다음 레지스터에 쓴다. */
		rcar_pci_write_reg(pcie, upper_32_bits(pci_addr),
				   PCIEPRAR(idx + 1));

	/* [한국어] CPU 주소의 상위 32비트도 다음 레지스터에. */
	rcar_pci_write_reg(pcie, upper_32_bits(cpu_addr), PCIELAR(idx + 1));
	/* [한국어] 상위 쪽 플래그 레지스터는 0 으로 비운다. 위 영어 주석대로 이 코드는
	 * 언제나 64비트 인바운드 창을 설정하는데, 그 이유는 DT range 파서가
	 * 32비트와 64비트를 구분하지 않기 때문이다. 그래서 idx 와 idx+1 두 칸을
	 * 한 쌍으로 쓰고, 상위 칸의 플래그는 비워 둔다. */
	rcar_pci_write_reg(pcie, 0, PCIELAMR(idx + 1));

}