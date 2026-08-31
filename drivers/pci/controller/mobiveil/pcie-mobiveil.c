// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for Mobiveil PCIe Host controller
 *
 * Copyright (c) 2018 Mobiveil Inc.
 * Copyright 2019 NXP
 *
 * Author: Subrahmanya Lingappa <l.subrahmanya@mobiveil.co.in>
 *	   Hou Zhiqiang <Zhiqiang.Hou@nxp.com>
 */

/*
 * [한국어 설명] Mobiveil PCIe IP 의 레지스터 접근과 주소 창 (pcie-mobiveil.c)
 *
 * === 파일의 역할 ===
 * Mobiveil 은 PCIe 컨트롤러 IP 를 파는 회사이고, NXP Layerscape 의 일부
 * 세대가 그 IP 를 쓴다. 이 파일은 그 IP 를 다루는 가장 아래층으로,
 * 두 가지를 담당한다.
 *
 * 1) 페이지 방식 레지스터 접근
 *    이 IP 의 특이점이자 이 파일에서 가장 중요한 부분이다. 레지스터가
 *    많아 주소 공간에 다 펼치지 못하고, 0xc00 이상은 "페이지" 로 나눠
 *    창 하나를 돌려 쓴다.
 *
 *    그래서 그런 레지스터에 접근하려면 두 단계를 거쳐야 한다.
 *      먼저 PAB_CTRL 의 pg_sel 필드에 "몇 번 페이지" 인지를 쓰고,
 *      그다음 그 페이지 안의 상대 위치로 접근한다.
 *    아래 mobiveil_pcie_comp_addr() 이 그 변환을 감춰 주므로, 호출자는
 *    평소처럼 오프셋 하나만 넘기면 된다.
 *
 *    이 방식의 대가는 명확하다. 페이지 레지스터 접근 한 번이 실제로는
 *    쓰기 한 번 + 읽기·쓰기 한 번이 되고, 그 사이가 원자적이지 않다.
 *    두 스레드가 동시에 다른 페이지를 건드리면 어긋날 수 있는데,
 *    이 파일에는 그것을 막는 잠금이 없다 — 상위 계층이 직렬화한다고
 *    전제하는 것으로 보이나, 그 근거를 이 트리에서 확인하지는 못했다.
 *
 * 2) 인바운드·아웃바운드 주소 창(AMAP)
 *    DesignWare 의 iATU, Cadence 의 아웃바운드 영역에 해당한다.
 *    아웃바운드는 CPU 주소를 PCIe 주소로, 인바운드는 그 반대로 옮긴다.
 *    창 크기를 "비트 수" 가 아니라 마스크로 표현하는 점이 앞의 두 IP 와
 *    다르다 — ~(size - 1) 로 만든 값을 그대로 레지스터에 넣는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SoC 별 드라이버(pcie-layerscape-gen4.c, pcie-mobiveil-plat.c)
 *   -> pcie-mobiveil-host.c 의 호스트 초기화
 *      -> [이 파일] mobiveil_csr_read/write 로 레지스터 접근
 *      -> [이 파일] program_ib_windows() / program_ob_windows() 로 창 설정
 *      -> [이 파일] mobiveil_bringup_link() 로 링크 대기
 *
 * 실행 컨텍스트: 초기화 중 프로세스 컨텍스트가 대부분이다. 레지스터
 *   접근 함수는 config 접근 경로에서도 불릴 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pcie-mobiveil-host.c 와 SoC 별 드라이버들.
 * 아래쪽: 커널의 readl/writel MMIO 접근자.
 * 공유 상태: struct mobiveil_pcie — 레지스터 베이스, 창 개수와 사용량,
 *   그리고 SoC 별 콜백 표(ops).
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 부르지 않는다(drivers/nvme 트리
 * 전수 확인 — mobiveil 관련 심볼 호출 0건). 관계는 토폴로지상의 것이다 —
 * 이 IP 를 쓴 보드에 NVMe 를 붙이면 여기서 설정한 주소 창을 통해
 * config 접근과 MMIO 가 오간다.
 *
 * 알아 둘 만한 점은 창 개수의 제약이다. program_ob_windows() 가
 * apio_wins 를 넘으면 오류를 찍고 그냥 돌아가므로, 큰 BAR 를 가진
 * 장치를 여럿 붙이면 창이 모자라 일부가 매핑되지 않을 수 있다.
 *
 * === 주요 함수/구조체 요약 ===
 * mobiveil_pcie_sel_page()  : PAB_CTRL 의 pg_sel 에 페이지 번호를 쓴다.
 * mobiveil_pcie_comp_addr() : 오프셋을 보고 페이지 전환이 필요한지
 *                             판단해 실제 접근 주소를 만든다.
 * mobiveil_pcie_read() / _write() : 크기별 MMIO 접근과 정렬 검사.
 * mobiveil_csr_read() / mobiveil_csr_write() : 위 둘을 묶은 공개 API.
 * mobiveil_pcie_link_up()   : 링크 상태를 확인한다. SoC 콜백이 있으면
 *                             그것을 우선한다.
 * program_ib_windows()      : 인바운드 창(PCIe → 메모리)을 설정한다.
 * program_ob_windows()      : 아웃바운드 창(CPU → PCIe)을 설정한다.
 * mobiveil_bringup_link()   : 링크가 올라오기를 기다린다.
 */

/* [한국어] usleep_range — 링크 대기 루프에서 CPU 를 놓아 준다. */
#include <linux/delay.h>
/* [한국어] 초기화 섹션 매크로. */
#include <linux/init.h>
/* [한국어] 기본 커널 매크로와 비트 연산 헬퍼. */
#include <linux/kernel.h>
/* [한국어] PCIBIOS_SUCCESSFUL / PCIBIOS_BAD_REGISTER_NUMBER 등
 * config 접근 반환 코드. 아래 read/write 가 그것을 쓴다. */
#include <linux/pci.h>
/* [한국어] struct platform_device. 오류 메시지를 낼 때 pcie->pdev->dev 를
 * 쓰므로 필요하다. */
#include <linux/platform_device.h>

/* [한국어] struct mobiveil_pcie 와 PAB_* 레지스터 상수,
 * 그리고 mobiveil_csr_readl 같은 크기별 편의 매크로. */
#include "pcie-mobiveil.h"

/*
 * mobiveil_pcie_sel_page - routine to access paged register
 *
 * Registers whose address greater than PAGED_ADDR_BNDRY (0xc00) are paged,
 * for this scheme to work extracted higher 6 bits of the offset will be
 * written to pg_sel field of PAB_CTRL register and rest of the lower 10
 * bits enabled with PAGED_ADDR_BNDRY are used as offset of the register.
 */
/* [한국어] (위 상류 주석이 페이지 방식의 원리를 설명하고 있다)
 *
 * mobiveil_pcie_sel_page - 접근할 레지스터 페이지를 선택한다
 *
 * @pcie: 대상 컨트롤러.
 * @pg_idx: 선택할 페이지 번호.
 * @return: 없음.
 *
 * PAB_CTRL 레지스터의 pg_sel 필드에 페이지 번호를 쓴다. 이후의 페이지
 * 영역 접근은 그 페이지를 향하게 된다.
 *
 * 이 함수 자체는 단순하지만 그 사정이 중요하다. 페이지 전환과 실제
 * 접근이 두 번의 별개 MMIO 이므로 그 사이가 원자적이지 않다.
 * 이 파일에는 그것을 보호하는 잠금이 없다.
 *
 * 실행 컨텍스트: 레지스터 접근 경로. 상위 계층이 직렬화한다고
 *   전제하는 것으로 보이나 그 근거는 이 트리에서 확인하지 못했다.
 *
 * 호출 체인:
 *   mobiveil_csr_read/write() → mobiveil_pcie_comp_addr() → [이 함수]
 */
static void mobiveil_pcie_sel_page(struct mobiveil_pcie *pcie, u8 pg_idx)
{
	u32 val;

	/* [한국어] PAB_CTRL 은 페이지 영역이 아닌 직접 접근 레지스터라
	 * 여기서는 페이지 전환 없이 readl 로 바로 읽는다 — 그렇지 않으면
	 * 페이지를 고르려고 페이지를 골라야 하는 순환이 된다. */
	val = readl(pcie->csr_axi_slave_base + PAB_CTRL);
	/* [한국어] pg_sel 필드만 지운다. 이 레지스터에 다른 제어 비트도
	 * 함께 있어 통째로 덮어쓰면 안 된다. */
	val &= ~(PAGE_SEL_MASK << PAGE_SEL_SHIFT);
	/* [한국어] 새 페이지 번호를 그 자리에 넣는다. 인자에도 마스크를
	 * 씌워 범위를 넘는 값이 옆 필드를 침범하지 않게 한다. */
	val |= (pg_idx & PAGE_SEL_MASK) << PAGE_SEL_SHIFT;

	/* [한국어] 이 쓰기가 끝나야 다음 접근이 올바른 페이지로 간다. */
	writel(val, pcie->csr_axi_slave_base + PAB_CTRL);
}

/* [한국어]
 * mobiveil_pcie_comp_addr - 오프셋을 실제 접근 주소로 바꾼다
 *
 * @pcie: 대상 컨트롤러.
 * @off: 논리적 레지스터 오프셋. 호출자는 페이지를 신경 쓰지 않는다.
 * @return: 그 레지스터에 실제로 접근할 가상 주소.
 *
 * 이 파일의 페이지 방식을 감추는 함수다. 호출자는 늘 논리 오프셋
 * 하나만 넘기고, 여기서 두 갈래로 나뉜다.
 *   경계(PAGED_ADDR_BNDRY, 0xc00) 미만 — 직접 접근 영역이다.
 *     그대로 더하면 되지만, 그 전에 pg_sel 을 0 으로 되돌린다.
 *   경계 이상 — 페이지 영역이다. 상위 비트를 페이지 번호로 삼아
 *     전환한 뒤, 하위 비트를 페이지 안의 위치로 쓴다.
 *
 * 직접 접근 영역인데도 pg_sel 을 0 으로 쓰는 것이 눈에 띈다.
 * 상류 주석이 그렇게 하라고 밝히고 있으며, 하드웨어가 pg_sel 값에
 * 따라 직접 영역의 해석을 달리하는 것으로 보인다. 다만 그 이유의
 * 근거를 이 트리에서 확인하지는 못했다.
 * 그 결과 모든 레지스터 접근이 PAB_CTRL 쓰기를 한 번씩 동반한다.
 *
 * 실행 컨텍스트: 레지스터 접근 경로. 부수효과(페이지 전환)가 있는
 *   함수이므로 순수 계산이 아니라는 점에 주의해야 한다.
 *
 * 호출 체인:
 *   mobiveil_csr_read() / mobiveil_csr_write() → [이 함수]
 *     → mobiveil_pcie_sel_page()
 */
static void __iomem *mobiveil_pcie_comp_addr(struct mobiveil_pcie *pcie,
					     u32 off)
{
	if (off < PAGED_ADDR_BNDRY) {
		/* For directly accessed registers, clear the pg_sel field */
		/* [한국어] 상류 주석대로 직접 접근 영역에서는 pg_sel 을 0 으로
		 * 되돌린다. */
		mobiveil_pcie_sel_page(pcie, 0);
		/* [한국어] 베이스에 오프셋을 그대로 더하면 된다. */
		return pcie->csr_axi_slave_base + off;
	}

	/* [한국어] 페이지 영역. 오프셋의 상위 6비트가 페이지 번호다. */
	mobiveil_pcie_sel_page(pcie, OFFSET_TO_PAGE_IDX(off));
	/* [한국어] 하위 10비트에 경계값을 씌운 것이 페이지 안의 위치다.
	 * 즉 어느 페이지든 실제 접근은 같은 주소 범위(0xc00~0xfff)에서
	 * 이뤄지고, 그 안의 내용만 pg_sel 에 따라 달라진다. */
	return pcie->csr_axi_slave_base + OFFSET_TO_PAGE_ADDR(off);
}

/* [한국어]
 * mobiveil_pcie_read - 정렬을 확인하고 크기에 맞는 MMIO 읽기를 한다
 *
 * @addr: 이미 페이지 변환이 끝난 실제 접근 주소.
 * @size: 1, 2, 4 중 하나(바이트).
 * @val: 읽은 값을 담을 곳.
 * @return: PCIBIOS_SUCCESSFUL, 또는 정렬·크기가 잘못되면
 *   PCIBIOS_BAD_REGISTER_NUMBER.
 *
 * MMIO 는 정렬되지 않은 접근을 허용하지 않는다. 2바이트 읽기는 짝수
 * 주소, 4바이트 읽기는 4의 배수 주소여야 하며, 어기면 버스 오류가 나거나
 * 조용히 엉뚱한 값을 읽는다. 그래서 하드웨어를 건드리기 전에 검사한다.
 *
 * PCIBIOS_* 반환값을 쓰는 것은 이 함수가 결국 PCI config 접근 경로에
 * 쓰이기 때문이다. 그쪽 규약이 음수 errno 가 아니라 이 상수들이다.
 *
 * 실행 컨텍스트: 레지스터 접근 경로. 순수 MMIO 라 잠들지 않는다.
 *
 * 에러 경로: 실패 시 *val 을 0 으로 만들어 준다. 호출자가 반환값을
 *   확인하지 않고 값을 쓰더라도 쓰레기가 흘러들지 않게 하려는 것이다.
 *
 * 호출 체인:
 *   mobiveil_csr_read() → [이 함수] → readl/readw/readb
 */
static int mobiveil_pcie_read(void __iomem *addr, int size, u32 *val)
{
	/* [한국어] size 가 2의 거듭제곱이므로 (size-1) 마스크로 정렬을
	 * 확인할 수 있다. 예를 들어 4바이트면 하위 2비트가 0 이어야 한다. */
	if ((uintptr_t)addr & (size - 1)) {
		/* [한국어] 실패해도 출력 인자를 정해진 값으로 채워 둔다. */
		*val = 0;
		return PCIBIOS_BAD_REGISTER_NUMBER;
	}

	/* [한국어] 크기에 맞는 접근자를 고른다. readl/readw/readb 는 각각
	 * 4/2/1 바이트 MMIO 읽기이며, 바이트 순서 변환과 컴파일러 최적화
	 * 방지를 함께 처리해 준다. */
	switch (size) {
	case 4:
		*val = readl(addr);
		break;
	case 2:
		*val = readw(addr);
		break;
	case 1:
		*val = readb(addr);
		break;
	default:
		/* [한국어] 1/2/4 가 아닌 크기는 PCI 에 존재하지 않는다.
		 * 호출자의 오류이므로 값을 0 으로 두고 실패로 알린다. */
		*val = 0;
		return PCIBIOS_BAD_REGISTER_NUMBER;
	}

	return PCIBIOS_SUCCESSFUL;
}

/* [한국어]
 * mobiveil_pcie_write - 정렬을 확인하고 크기에 맞는 MMIO 쓰기를 한다
 *
 * @addr: 이미 페이지 변환이 끝난 실제 접근 주소.
 * @size: 1, 2, 4 중 하나(바이트).
 * @val: 쓸 값.
 * @return: PCIBIOS_SUCCESSFUL 또는 PCIBIOS_BAD_REGISTER_NUMBER.
 *
 * 위 read 의 짝이다. 출력 인자가 없어 실패 시 채워 둘 것이 없다는
 * 점만 다르다.
 *
 * 실행 컨텍스트: 레지스터 접근 경로.
 *
 * 호출 체인:
 *   mobiveil_csr_write() → [이 함수] → writel/writew/writeb
 */
static int mobiveil_pcie_write(void __iomem *addr, int size, u32 val)
{
	/* [한국어] read 와 같은 정렬 검사. */
	if ((uintptr_t)addr & (size - 1))
		return PCIBIOS_BAD_REGISTER_NUMBER;

	/* [한국어] read 와 같은 방식으로 크기에 맞는 접근자를 고른다. */
	switch (size) {
	/* [한국어] 4바이트 쓰기. */
	case 4:
		writel(val, addr);
		break;
	/* [한국어] 2바이트 쓰기. */
	case 2:
		writew(val, addr);
		break;
	/* [한국어] 1바이트 쓰기. */
	case 1:
		writeb(val, addr);
		break;
	default:
		return PCIBIOS_BAD_REGISTER_NUMBER;
	}

	return PCIBIOS_SUCCESSFUL;
}

/* [한국어]
 * mobiveil_csr_read - 논리 오프셋으로 컨트롤러 레지스터를 읽는다
 *
 * @pcie: 대상 컨트롤러.
 * @off: 논리 오프셋. 페이지 여부는 이 함수가 알아서 처리한다.
 * @size: 읽을 크기(바이트).
 * @return: 읽은 값. 실패해도 값을 돌려주므로 호출자가 구분할 수 없다.
 *
 * 이 파일의 공개 읽기 API 다. 페이지 전환과 크기별 접근을 감춰 준다.
 *
 * 반환값으로 실패를 알리지 않는 점에 주의해야 한다. 오류가 나면
 * 메시지만 남기고 0(read 가 채워 둔 값)을 돌려주므로, 호출자는
 * 정상적으로 읽은 0 과 실패를 구분할 수 없다.
 * 헤더의 mobiveil_csr_readl 같은 매크로가 이것을 감싸며, 그 사용처들도
 * 반환값을 그대로 쓴다 — 상류 설계가 그렇다.
 *
 * 실행 컨텍스트: 초기화와 config 접근 경로. 페이지 전환이라는 부수효과가
 *   있으므로 동시에 부르면 어긋날 수 있다(파일 상단 주석 참고).
 *
 * 호출 체인:
 *   mobiveil_csr_readl/readw/readb 매크로 → [이 함수]
 *     → mobiveil_pcie_comp_addr() → mobiveil_pcie_read()
 */
u32 mobiveil_csr_read(struct mobiveil_pcie *pcie, u32 off, size_t size)
{
	void __iomem *addr;
	u32 val;
	int ret;

	/* [한국어] 페이지 전환까지 포함해 실제 주소를 얻는다. */
	addr = mobiveil_pcie_comp_addr(pcie, off);

	ret = mobiveil_pcie_read(addr, size, &val);
	if (ret)
		/* [한국어] 정렬이나 크기가 잘못된 것은 호출자의 버그이므로
		 * 메시지를 남긴다. 다만 어느 오프셋에서 났는지는 찍지 않아
		 * 진단이 쉽지는 않다. */
		dev_err(&pcie->pdev->dev, "read CSR address failed\n");

	/* [한국어] 실패했어도 val 은 0 으로 채워져 있다. */
	return val;
}

/* [한국어]
 * mobiveil_csr_write - 논리 오프셋으로 컨트롤러 레지스터에 쓴다
 *
 * @pcie: 대상 컨트롤러.
 * @val: 쓸 값. 인자 순서가 read 와 달라 값이 오프셋보다 앞에 온다.
 * @off: 논리 오프셋.
 * @size: 쓸 크기(바이트).
 * @return: 없음. 실패해도 알리지 않는다.
 *
 * read 의 짝이다. void 라 실패를 전할 방법이 아예 없고 메시지만 남긴다.
 *
 * 실행 컨텍스트: 초기화와 config 접근 경로.
 *
 * 호출 체인:
 *   mobiveil_csr_writel/writew/writeb 매크로 → [이 함수]
 *     → mobiveil_pcie_comp_addr() → mobiveil_pcie_write()
 */
void mobiveil_csr_write(struct mobiveil_pcie *pcie, u32 val, u32 off,
			       size_t size)
{
	void __iomem *addr;
	/* [한국어] write 의 결과. 실패해도 호출자에게 전할 방법이 없다. */
	int ret;

	/* [한국어] 페이지 전환까지 포함해 실제 주소를 얻는다. */
	addr = mobiveil_pcie_comp_addr(pcie, off);

	/* [한국어] 정렬을 확인하고 크기에 맞게 쓴다. */
	ret = mobiveil_pcie_write(addr, size, val);
	/* [한국어] 정렬이나 크기가 잘못된 것은 호출자의 버그다. */
	if (ret)
		dev_err(&pcie->pdev->dev, "write CSR address failed\n");
}

/* [한국어]
 * mobiveil_pcie_link_up - 링크가 올라왔는지 확인한다
 *
 * @pcie: 대상 컨트롤러.
 * @return: 링크가 L0(정상 동작 상태)이면 true.
 *
 * SoC 마다 링크 상태를 읽는 방법이 다를 수 있어 콜백을 먼저 본다.
 * 콜백이 없으면 이 IP 의 표준 방식 — LTSSM 상태가 L0 인지 확인 — 을 쓴다.
 *
 * L0 은 PCIe 링크 상태 기계에서 "정상 동작 중" 을 뜻한다. 트레이닝
 * 중이거나 절전 상태(L0s, L1)면 이 값이 아니므로 false 가 된다.
 *
 * 실행 컨텍스트: 링크 대기 루프와 초기화 경로.
 *
 * 호출 체인:
 *   mobiveil_bringup_link() → [이 함수] → ops->link_up 또는 csr_readl
 */
bool mobiveil_pcie_link_up(struct mobiveil_pcie *pcie)
{
	/* [한국어] SoC 별 구현이 있으면 그것을 우선한다. Layerscape 처럼
	 * 별도 레지스터로 링크를 보는 하드웨어가 있기 때문이다.
	 *
	 * ops 자체가 NULL 인지는 확인하지 않는다. 확인해 보면 mobiveil
	 * 트리에서 pcie->ops 를 대입하는 곳은 pcie-layerscape-gen4.c:217
	 * 하나뿐이고, pcie-mobiveil-plat.c 의 mbvl,gpex40-pcie 경로는
	 * 그것을 채우지 않는다. 브리지 private 영역이 0 으로 초기화되므로
	 * 그 경로에서는 이 줄이 NULL 을 역참조하게 된다.
	 * 이 함수는 mobiveil_bringup_link() 를 거쳐
	 * mobiveil_pcie_host_probe() 에서 도달한다.
	 * 상류 코드 그대로이며 수정하지 않는다 — 의도인지, 그 compatible 이
	 * 실제로 쓰이지 않아 드러나지 않는 것인지는 판단할 근거가 없다. */
	if (pcie->ops->link_up)
		return pcie->ops->link_up(pcie);

	/* [한국어] 기본 방식. LTSSM 상태 필드를 뽑아 L0 과 비교한다. */
	return (mobiveil_csr_readl(pcie, LTSSM_STATUS) &
		LTSSM_STATUS_L0_MASK) == LTSSM_STATUS_L0;
}

/* [한국어]
 * program_ib_windows - 인바운드 주소 창을 설정한다 (PCIe → 시스템 메모리)
 *
 * @pcie: 대상 컨트롤러.
 * @win_num: 몇 번째 창인지.
 * @cpu_addr: 이쪽(AXI) 주소. 들어온 요청이 닿을 곳이다.
 * @pci_addr: 저쪽(PCIe) 주소. 이 범위로 들어오는 요청이 대상이다.
 * @type: 창의 종류(메모리/IO 등). AMAP_CTRL 의 타입 필드에 그대로 실린다.
 * @size: 창 크기.
 * @return: 없음. 창이 모자라면 메시지만 남기고 물러난다.
 *
 * 인바운드는 장치가 내는 DMA 가 시스템 메모리의 어디에 닿을지를 정한다.
 * 아웃바운드(아래 함수)와 방향이 반대다.
 *
 * 크기를 마스크로 표현하는 점이 이 IP 의 특징이다. size64 = ~(size - 1)
 * 은 크기가 2의 거듭제곱일 때 상위 비트만 1 인 마스크가 된다 —
 * 예컨대 크기 4KB(0x1000)면 ~0xFFF = 0xFFFFF000 이다. 하드웨어는 이
 * 마스크로 들어온 주소의 어느 비트까지를 비교할지 정한다.
 * 그래서 크기가 2의 거듭제곱이 아니면 엉뚱한 범위가 열리는데,
 * 이 함수는 그것을 검사하지 않는다 — 호출자의 책임이다.
 *
 * 실행 컨텍스트: 호스트 초기화 중 프로세스 컨텍스트.
 *
 * 에러 경로: 창 번호가 범위를 넘으면 메시지만 남기고 아무것도 하지
 *   않는다. void 라 실패를 전할 방법이 없어, 호출자는 창이 설정되지
 *   않았다는 사실을 알 수 없다.
 *
 * 호출 체인:
 *   mobiveil_host_init() 계열 [pcie-mobiveil-host.c] → [이 함수]
 *     → mobiveil_csr_writel()
 */
void program_ib_windows(struct mobiveil_pcie *pcie, int win_num,
			u64 cpu_addr, u64 pci_addr, u32 type, u64 size)
{
	u32 value;
	/* [한국어] 크기를 비교 마스크로 바꾼다. 위 설명 참고. */
	u64 size64 = ~(size - 1);

	/* [한국어] 하드웨어가 가진 인바운드 창 개수를 넘을 수 없다. */
	if (win_num >= pcie->ppio_wins) {
		dev_err(&pcie->pdev->dev,
			"ERROR: max inbound windows reached !\n");
		return;
	}

	/* [한국어] 제어 레지스터를 읽어 타입과 크기 필드만 고친다. */
	value = mobiveil_csr_readl(pcie, PAB_PEX_AMAP_CTRL(win_num));
	value &= ~(AMAP_CTRL_TYPE_MASK << AMAP_CTRL_TYPE_SHIFT | WIN_SIZE_MASK);
	/* [한국어] 타입, 활성화 비트, 크기 마스크의 하위 절반을 함께 넣는다.
	 * 활성화를 같은 쓰기에 담으므로, 나머지 설정이 아직 안 들어간
	 * 상태에서 창이 켜지는 셈이다 — 아래 주소 쓰기가 이어지기 전까지의
	 * 짧은 구간이 존재한다. 초기화 중이라 그 사이에 요청이 들어올 일이
	 * 없다고 보는 것으로 읽힌다. */
	value |= type << AMAP_CTRL_TYPE_SHIFT | 1 << AMAP_CTRL_EN_SHIFT |
		 (lower_32_bits(size64) & WIN_SIZE_MASK);
	mobiveil_csr_writel(pcie, value, PAB_PEX_AMAP_CTRL(win_num));

	/* [한국어] 크기 마스크의 상위 32비트는 확장 레지스터에 따로 들어간다.
	 * 64비트 주소 공간을 지원하기 위한 구조다. */
	mobiveil_csr_writel(pcie, upper_32_bits(size64),
			    PAB_EXT_PEX_AMAP_SIZEN(win_num));

	/* [한국어] AXI 쪽(이 SoC 쪽) 주소를 상하위로 나눠 쓴다. */
	mobiveil_csr_writel(pcie, lower_32_bits(cpu_addr),
			    PAB_PEX_AMAP_AXI_WIN(win_num));
	mobiveil_csr_writel(pcie, upper_32_bits(cpu_addr),
			    PAB_EXT_PEX_AMAP_AXI_WIN(win_num));

	/* [한국어] PCIe 쪽 주소도 상하위로 나눠 쓴다. 이 범위로 들어오는
	 * 요청이 위 AXI 주소로 옮겨진다. */
	mobiveil_csr_writel(pcie, lower_32_bits(pci_addr),
			    PAB_PEX_AMAP_PEX_WIN_L(win_num));
	mobiveil_csr_writel(pcie, upper_32_bits(pci_addr),
			    PAB_PEX_AMAP_PEX_WIN_H(win_num));

	/* [한국어] 쓴 창 수를 센다. 다만 이 값을 읽어 다음 창 번호를 정하는
	 * 코드는 이 파일에 없다 — 호출자가 번호를 직접 관리한다. */
	pcie->ib_wins_configured++;
}

/*
 * routine to program the outbound windows
 */
/* [한국어]
 * program_ob_windows - 아웃바운드 주소 창을 설정한다 (CPU → PCIe)
 *
 * @pcie: 대상 컨트롤러.
 * @win_num: 몇 번째 창인지.
 * @cpu_addr: 이쪽 CPU 주소. 이 범위에 접근하면 PCIe 로 나간다.
 * @pci_addr: 그 접근이 나갈 PCIe 주소.
 * @type: TLP 종류(메모리/IO/config).
 * @size: 창 크기.
 * @return: 없음.
 *
 * 위 인바운드의 반대 방향이다. DesignWare 의 iATU 아웃바운드,
 * Cadence 의 아웃바운드 영역에 해당한다.
 *
 * 구조는 인바운드와 거의 같지만 레지스터 이름이 PEX_AMAP 이 아니라
 * AXI_AMAP 이고, CPU 주소에 정렬 마스크를 씌우는 점이 다르다.
 *
 * 실행 컨텍스트: 호스트 초기화 중 프로세스 컨텍스트.
 *
 * 에러 경로: 인바운드와 같다. 창이 모자라면 메시지만 남기고 물러나며,
 *   호출자는 실패를 알 수 없다. 큰 BAR 를 가진 장치를 여럿 붙이면
 *   창이 모자라 일부가 매핑되지 않을 수 있는 지점이다.
 *
 * 호출 체인:
 *   mobiveil_host_init() 계열 [pcie-mobiveil-host.c] → [이 함수]
 */
void program_ob_windows(struct mobiveil_pcie *pcie, int win_num,
			u64 cpu_addr, u64 pci_addr, u32 type, u64 size)
{
	u32 value;
	/* [한국어] 인바운드와 같은 방식의 크기 마스크. */
	u64 size64 = ~(size - 1);

	/* [한국어] 아웃바운드 창 개수 상한. 인바운드와 다른 필드(apio_wins)를
	 * 본다 — 하드웨어가 두 방향의 창을 따로 갖기 때문이다. */
	if (win_num >= pcie->apio_wins) {
		dev_err(&pcie->pdev->dev,
			"ERROR: max outbound windows reached !\n");
		return;
	}

	/*
	 * program Enable Bit to 1, Type Bit to (00) base 2, AXI Window Size Bit
	 * to 4 KB in PAB_AXI_AMAP_CTRL register
	 */
	/* [한국어] 제어 레지스터를 읽어 타입과 크기만 고친다.
	 * 상류 주석이 "Enable 을 1, Type 을 0, 크기를 4KB 로" 라고 예를
	 * 들고 있는데, 실제로는 타입과 크기가 인자로 들어오므로 그 주석은
	 * 대표적인 경우를 설명한 것으로 읽힌다. */
	value = mobiveil_csr_readl(pcie, PAB_AXI_AMAP_CTRL(win_num));
	value &= ~(WIN_TYPE_MASK << WIN_TYPE_SHIFT | WIN_SIZE_MASK);
	/* [한국어] 활성화·타입·크기를 한 워드로 합쳐 쓴다. 인바운드와
	 * 마찬가지로 활성화가 주소 설정보다 먼저 들어간다. */
	value |= 1 << WIN_ENABLE_SHIFT | type << WIN_TYPE_SHIFT |
		 (lower_32_bits(size64) & WIN_SIZE_MASK);
	mobiveil_csr_writel(pcie, value, PAB_AXI_AMAP_CTRL(win_num));

	/* [한국어] 크기 마스크의 상위 32비트. */
	mobiveil_csr_writel(pcie, upper_32_bits(size64),
			    PAB_EXT_AXI_AMAP_SIZE(win_num));

	/*
	 * program AXI window base with appropriate value in
	 * PAB_AXI_AMAP_AXI_WIN0 register
	 */
	/* [한국어] CPU 주소를 쓰되 하위 정렬 비트를 잘라 낸다. 그 자리가
	 * 주소가 아니라 다른 용도로 쓰이기 때문이며, 인바운드 쪽에는
	 * 이 마스크가 없다 — 두 방향의 레지스터 배치가 다르다. */
	mobiveil_csr_writel(pcie,
			    lower_32_bits(cpu_addr) & (~AXI_WINDOW_ALIGN_MASK),
			    PAB_AXI_AMAP_AXI_WIN(win_num));
	mobiveil_csr_writel(pcie, upper_32_bits(cpu_addr),
			    PAB_EXT_AXI_AMAP_AXI_WIN(win_num));

	/* [한국어] 나갈 PCIe 주소. 상위 32비트에는 마스크를 씌우지 않는다. */
	mobiveil_csr_writel(pcie, lower_32_bits(pci_addr),
			    PAB_AXI_AMAP_PEX_WIN_L(win_num));
	mobiveil_csr_writel(pcie, upper_32_bits(pci_addr),
			    PAB_AXI_AMAP_PEX_WIN_H(win_num));

	/* [한국어] 쓴 창 수를 센다. 인바운드와 마찬가지로 이 값을 읽어
	 * 다음 번호를 정하는 코드는 이 파일에 없다. */
	pcie->ob_wins_configured++;
}

/* [한국어]
 * mobiveil_bringup_link - 링크가 올라오기를 기다린다
 *
 * @pcie: 대상 컨트롤러.
 * @return: 0 이면 링크가 올라왔다. 재시도 한도를 넘기면 -ETIMEDOUT.
 *
 * 이름이 bringup 이지만 실제로는 기다리기만 한다 — 링크 트레이닝을
 * 시작시키는 코드가 이 함수에 없다. 그 시작은 하드웨어가 자동으로
 * 하거나 SoC 별 드라이버가 따로 처리하는 것으로 보이며, 그 근거를
 * 이 트리에서 확인하지는 못했다.
 *
 * 실행 컨텍스트: 초기화 중 프로세스 컨텍스트. usleep_range 로 잠든다.
 *
 * 에러 경로: 슬롯이 비어 있어도 링크가 안 올라오므로 이 오류가 반드시
 *   고장을 뜻하지는 않는다. 그런데도 dev_err 로 남기는데, 호출자가
 *   그 사정을 알고 판단하도록 맡긴 것으로 읽힌다.
 *
 * 호출 체인:
 *   mobiveil_host_init() 계열 [pcie-mobiveil-host.c] → [이 함수]
 *     → mobiveil_pcie_link_up()
 */
int mobiveil_bringup_link(struct mobiveil_pcie *pcie)
{
	int retries;

	/* check if the link is up or not */
	/* [한국어] 정해진 횟수만큼 반복 확인한다. 상수들은 이 IP 의 헤더에
	 * 정의되어 있다. */
	for (retries = 0; retries < LINK_WAIT_MAX_RETRIES; retries++) {
		if (mobiveil_pcie_link_up(pcie))
			return 0;

		/* [한국어] 다음 확인까지 잔다. 범위로 주면 커널이 다른
		 * 타이머와 묶어 깨울 수 있어 효율적이다. */
		usleep_range(LINK_WAIT_MIN, LINK_WAIT_MAX);
	}

	/* [한국어] 슬롯이 비어 있어도 여기로 오므로 이 메시지가 반드시 고장을 뜻하지는
	 * 않는다. 판단은 호출자에게 맡긴다. */
	dev_err(&pcie->pdev->dev, "link never came up\n");

	return -ETIMEDOUT;
}
