// SPDX-License-Identifier: GPL-2.0
/*
 * Synopsys DesignWare PCIe Endpoint controller driver
 *
 * Copyright (C) 2017 Texas Instruments
 * Author: Kishon Vijay Abraham I <kishon@ti.com>
 */

/*
 * [한국어 설명] Synopsys DesignWare PCIe 엔드포인트(EP) 컨트롤러 드라이버 (pcie-designware-ep.c)
 *
 * === 파일의 역할 ===
 * DesignWare(DWC) PCIe IP 를 "엔드포인트" 로 운전하는 드라이버다. 같은 IP 라도
 * 루트 컴플렉스(RC)로 동작하면 pcie-designware-host.c 가, 엔드포인트로 동작하면
 * 이 파일이 담당한다. 하는 일은 커널 PCI 엔드포인트 프레임워크가 정의한 콜백 묶음
 * (struct pci_epc_ops)을 DWC 레지스터 조작으로 번역하는 것이다 — 즉 "EPF 기능
 * 드라이버가 요청하는 추상 동작" 을 "DBI/DBI2 설정 공간 쓰기 + iATU 주소 변환 창
 * 프로그래밍" 으로 내린다. 구체적으로는 (1) 설정 공간 헤더(벤더/디바이스 ID, 클래스)
 * 기록, (2) BAR 의 크기·종류 설정과 그 BAR 로 들어오는 트랜잭션을 로컬 메모리로
 * 돌리는 인바운드 iATU 매핑, (3) 엔드포인트 로컬 물리주소를 호스트 메모리로 내보내는
 * 아웃바운드 iATU 매핑, (4) MSI/MSI-X 능력 레지스터 설정과 실제 인터럽트 발사,
 * (5) 링크 시작/정지와 초기화·정리를 맡는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 위에서 아래로: configfs(사용자 공간에서 EPF 를 만들고 EPC 에 바인딩)
 *   → EPF 기능 드라이버(drivers/pci/endpoint/functions/pci-epf-test.c 등)
 *   → EPC 코어(drivers/pci/endpoint/pci-epc-core.c 의 pci_epc_set_bar/map_addr/raise_irq)
 *   → [이 파일의 epc_ops 콜백]
 *   → 공용 DWC 하부(pcie-designware.c: iATU 접근자, DBI 접근자, 링크 제어)
 *   → 실제 하드웨어 레지스터.
 * 옆으로는 SoC 별 글루 드라이버(pcie-qcom-ep.c, pcie-dw-rockchip.c, pci-layerscape-ep.c,
 * pcie-rcar-gen4.c, pci-imx6.c, pcie-keembay.c, pcie-artpec6.c, pcie-stm32-ep.c,
 * pci-keystone.c, pcie-designware-plat.c)가 있다. 이들이 probe 에서
 * dw_pcie_ep_init() 과 dw_pcie_ep_init_registers() 를 부르고, ep->ops 에 자기
 * 훅(init/pre_init/get_features/raise_irq)을 걸어 SoC 고유 동작을 끼워 넣는다.
 * 실행 컨텍스트는 전부 커널 모듈의 프로세스 컨텍스트다. EPC 콜백들은 EPC 코어가
 * epc->lock 뮤텍스를 잡은 채 부르므로 이 파일의 콜백 본문은 서로 직렬화된다.
 * 반면 dw_pcie_ep_raise_msi_irq() 계열은 글루 드라이버의 raise_irq 훅에서 불리며
 * 그 경로는 EPC 코어의 pci_epc_raise_irq() 를 거쳐 온다.
 *
 * === 타 모듈과의 연결 ===
 * - 위쪽(호출자): pci-epc-core.c. pci_epc_set_bar() 가 BAR 크기가 2의 거듭제곱인지,
 *   BAR_FIXED 면 고정 크기와 일치하는지, BAR_RESIZABLE 이면 1MB~128TB 범위인지,
 *   64비트 BAR 이 BAR5 가 아닌지 등을 먼저 검사한 뒤에야 epc_ops.set_bar 로 내려온다.
 *   따라서 이 파일은 그 검사들을 되풀이하지 않고 DWC 고유 제약(BAR 쌍 겹침 금지 등)만 본다.
 *   pci_epc_raise_irq() 가 넘기는 interrupt_num 은 1-기반이라, 이 파일은 하드웨어에
 *   쓸 때 일관되게 1 을 뺀다.
 * - 아래쪽(피호출자): pcie-designware.c 의 dw_pcie_prog_ep_inbound_atu()(BAR Match Mode),
 *   dw_pcie_prog_inbound_atu()(Address Match Mode), dw_pcie_prog_outbound_atu(),
 *   dw_pcie_disable_atu(), dw_pcie_setup(), dw_pcie_version_detect(),
 *   dw_pcie_iatu_detect(), dw_pcie_edma_detect()/dw_pcie_edma_remove(),
 *   dw_pcie_start_link()/dw_pcie_stop_link(), 그리고 헤더의 DBI/DBI2 인라인 접근자.
 * - 옆쪽: pcie-designware-debugfs.c. init_registers 끝에서
 *   dwc_pcie_debugfs_init(pci, DW_PCIE_EP_TYPE) 로 RAS DES 디버그 인터페이스를 연다.
 * - 공유 자료구조: struct dw_pcie_ep(헤더 정의) — 인바운드/아웃바운드 창 비트맵
 *   ib_window_map/ob_window_map, 아웃바운드 창별 주소 기록 outbound_addr[],
 *   MSI 발사용으로 예약해 둔 한 페이지 msi_mem/msi_mem_phys, 함수 리스트 func_list.
 *   그 리스트의 원소가 struct dw_pcie_ep_func 로, 함수별 MSI/MSI-X 능력 오프셋과
 *   BAR 별 iATU 인덱스를 들고 있다.
 * - 데이터 흐름: EPF 가 DMA 버퍼를 잡아 그 물리주소를 pci_epf_bar 에 담아 set_bar 를
 *   부르면, 이 파일이 BAR 마스크(크기)를 DBI2 에 쓰고 인바운드 iATU 를 걸어
 *   "호스트가 그 BAR 주소로 보낸 트랜잭션 → EPF 버퍼" 경로를 만든다. 반대 방향은
 *   map_addr 로, "엔드포인트 로컬 물리주소에 쓰기 → 호스트 메모리로 나가는 TLP" 경로다.
 *   MSI 발사가 바로 그 아웃바운드 경로를 쓰는 사례다.
 *
 * === 주요 함수/구조체 요약 ===
 * - epc_ops: 이 파일이 EPC 프레임워크에 등록하는 콜백 테이블. 아래 함수들이 여기 꽂힌다.
 * - dw_pcie_ep_set_bar(): BAR 한 개를 세운다. BAR 종류(FIXED/PROGRAMMABLE/RESIZABLE)에
 *   따라 마스크 기록 방식을 갈라 쓰고, 이어서 인바운드 iATU 를 건다. 이미 세워진 BAR 을
 *   다시 부르면 BAR 레지스터를 건너뛰는데, 그것을 쓰면 호스트가 배정한 주소가 지워지기 때문이다.
 * - dw_pcie_ep_ib_atu_bar() / dw_pcie_ep_ib_atu_addr(): 인바운드 매핑의 두 방식.
 *   앞은 BAR 번호로 매칭(주소를 몰라도 됨), 뒤는 호스트가 배정한 실제 주소를 되읽어
 *   BAR 을 여러 조각(submap)으로 쪼개 각각 다른 물리주소에 붙인다.
 * - dw_pcie_ep_map_addr() / dw_pcie_ep_unmap_addr(): 아웃바운드 창 할당과 해제.
 *   ob_window_map 비트맵으로 빈 창을 찾고 outbound_addr[] 로 되찾는다.
 * - dw_pcie_ep_raise_msi_irq() / _msix_irq() / _msix_irq_doorbell(): 인터럽트 세 경로.
 *   MSI 는 아웃바운드 창을 하나 잡아 두고 재사용, MSI-X 는 매번 잡았다 풀며,
 *   도어벨은 IP 전용 레지스터 한 번 쓰기로 끝낸다.
 * - dw_pcie_ep_init() / _init_registers() / _deinit() / _cleanup(): 수명주기. 앞의 둘이
 *   나뉘어 있는 이유는, 레지스터 초기화는 참조 클럭이 살아 있을 때에만 가능해서다.
 */

/* [한국어] IS_ALIGNED()/ALIGN() 정렬 매크로. submap 조각의 크기·오프셋·물리주소가
 * 컨트롤러의 iATU 정렬 단위(pci->region_align)에 맞는지 검사하는 데 쓴다. */
#include <linux/align.h>
/* [한국어] FIELD_GET()/FIELD_PREP()/FIELD_MODIFY() 비트필드 헬퍼. MSI 의 QSIZE/QMASK,
 * Resizable BAR 의 NBAR/BAR_IDX, 링크 능력의 MLW/SLS 처럼 레지스터 한 워드 안의
 * 부분 필드를 마스크·시프트 손계산 없이 다루려고 쓴다. */
#include <linux/bitfield.h>
/* [한국어] of_property_read_u8(). 디바이스 트리의 "max-functions" 속성을 읽어
 * 이 엔드포인트가 노출할 PCI 함수 개수를 정한다. */
#include <linux/of.h>
/* [한국어] platform_get_resource_byname()/to_platform_device(). 이 컨트롤러는
 * 플랫폼 디바이스로 등록되며, "addr_space" 라는 이름의 MEM 리소스가 곧
 * 아웃바운드 창을 뚫을 로컬 물리 주소 구간이다. */
#include <linux/platform_device.h>

/* [한국어] 같은 디렉토리의 공용 DWC 헤더. struct dw_pcie / dw_pcie_ep 정의,
 * DBI·DBI2 인라인 접근자, iATU 프로그래밍 함수 선언이 전부 여기서 온다. */
#include "pcie-designware.h"
/* [한국어] PCI 엔드포인트 컨트롤러(EPC) 프레임워크. struct pci_epc_ops 의 서명,
 * epc_get_drvdata()/epc_set_drvdata(), pci_epc_mem_* 주소 할당자,
 * pci_epc_linkup()/linkdown() 통지 함수가 여기 있다. */
#include <linux/pci-epc.h>
/* [한국어] PCI 엔드포인트 기능(EPF) 프레임워크. struct pci_epf_bar 와 그 안의
 * submap 배열, struct pci_epf_msix_tbl(MSI-X 테이블 엔트리 레이아웃),
 * enum pci_barno 가 여기서 온다. EPF 드라이버가 채워 넘기는 요청의 모양이다. */
#include <linux/pci-epf.h>

/**
 * dw_pcie_ep_get_func_from_ep - Get the struct dw_pcie_ep_func corresponding to
 *				 the endpoint function
 * @ep: DWC EP device
 * @func_no: Function number of the endpoint device
 *
 * Return: struct dw_pcie_ep_func if success, NULL otherwise.
 */
/* [한국어]
 * dw_pcie_ep_get_func_from_ep - 함수 번호로 per-function 상태 구조체를 찾는다
 *
 * @ep: DWC 엔드포인트 인스턴스.
 * @func_no: 찾을 PCI 함수 번호(0 ~ epc->max_functions-1).
 * @return: 해당 struct dw_pcie_ep_func 포인터. 없으면 NULL.
 *
 * 하나의 DWC 컨트롤러는 여러 PCI 함수(multi-function endpoint)를 노출할 수 있고,
 * 함수마다 MSI/MSI-X 능력 구조체의 오프셋과 BAR 별 iATU 인덱스가 다르다. 그 상태를
 * ep->func_list 연결 리스트에 함수당 하나씩 달아 두고, 이 함수가 번호로 되찾는다.
 * 배열이 아니라 리스트인 이유는 항목이 dw_pcie_ep_init_registers() 에서 하나씩
 * devm_kzalloc 되어 붙기 때문이다.
 *
 * 리스트는 init_registers 에서 구성된 뒤로는 추가만 되고 제거되지 않으며, EPC 콜백
 * 경로는 EPC 코어가 잡아 주는 epc->lock 아래에서 직렬화되므로 잠금 없이 순회한다.
 * 못 찾으면 NULL 을 돌려주고, 호출자는 거의 예외 없이 -EINVAL 로 바꿔 되돌린다.
 *
 * 실행 컨텍스트: 커널 프로세스 컨텍스트. 잠들지 않는다.
 *
 * 호출 체인:
 *   이 파일의 거의 모든 EPC 콜백 → [이 함수] → list_for_each_entry()
 */
struct dw_pcie_ep_func *
dw_pcie_ep_get_func_from_ep(struct dw_pcie_ep *ep, u8 func_no)
{
	struct dw_pcie_ep_func *ep_func;

	/* [한국어] 함수별 상태 리스트를 처음부터 훑는다. 항목 수가 max_functions(대개 1~8)라 선형 탐색으로 충분하다 */
	list_for_each_entry(ep_func, &ep->func_list, list) {
		/* [한국어] 찾는 번호와 일치하는지 확인. func_no 는 리스트에 넣을 때 init_registers 가 채운 값이다 */
		if (ep_func->func_no == func_no)
			/* [한국어] 일치하면 그 항목의 포인터를 바로 돌려준다 — 번호는 유일하므로 더 볼 필요가 없다 */
			return ep_func;
	}

	return NULL;
}

/* [한국어]
 * __dw_pcie_ep_reset_bar - BAR 하나를 완전히 꺼서 호스트에게 안 보이게 만든다
 *
 * @pci: DWC 컨트롤러.
 * @func_no: 대상 PCI 함수 번호.
 * @bar: 끌 BAR 번호(BAR_0 ~ BAR_5).
 * @flags: 원래 이 BAR 이 쓰던 pci_epf_bar 플래그. 64비트 BAR 이었는지 판별용.
 * @return: 없음.
 *
 * BAR 을 "없는 것" 으로 만들려면 마스크(크기) 쪽과 주소 쪽을 모두 0 으로 지워야 한다.
 * DWC 는 같은 오프셋을 두 창으로 노출하는데, DBI 창으로 쓰면 BAR 주소 레지스터이고
 * DBI2 창으로 쓰면 BAR 마스크 레지스터다. 두 값을 한 오프셋에 둘 수 없어 IP 가
 * 창을 나눈 것이므로, 여기서도 dbi2 쓰기와 dbi 쓰기를 짝지어 부른다. 마스크가 0 이면
 * 호스트가 크기를 알아내려고 전부 1 을 써 봐도 0 이 되돌아와 "미구현 BAR" 로 읽힌다.
 *
 * BAR 은 원래 호스트만 쓰는 읽기 전용 필드라, 쓰기 전에 dw_pcie_dbi_ro_wr_en() 으로
 * "읽기 전용 레지스터에 쓰기 허용" 비트를 켜고 끝나면 다시 끈다.
 * 64비트 BAR 이면 상위 절반이 다음 BAR 자리(reg + 4)를 잡아먹으므로 그쪽도 지운다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. clear_bar 경로에서는 epc->lock 아래.
 *
 * 호출 체인:
 *   dw_pcie_ep_reset_bar()/dw_pcie_ep_clear_bar() → [이 함수] → dw_pcie_ep_writel_dbi2()/_dbi()
 */
static void __dw_pcie_ep_reset_bar(struct dw_pcie *pci, u8 func_no,
				   enum pci_barno bar, int flags)
{
	struct dw_pcie_ep *ep = &pci->ep;
	/* [한국어] BAR 레지스터의 설정 공간 오프셋을 담을 변수 */
	u32 reg;

	/* [한국어] BAR n 의 오프셋 = BAR0(0x10) + 4*n. PCI 규약상 BAR 6개가 4바이트 간격으로 연달아 있다 */
	reg = PCI_BASE_ADDRESS_0 + (4 * bar);
	dw_pcie_dbi_ro_wr_en(pci);
	/* [한국어] DBI2 창 쓰기 = BAR 마스크를 0 으로. 마스크가 0 이면 호스트가 크기를 물어도 0 이 나와 '미구현 BAR' 로 읽힌다 */
	dw_pcie_ep_writel_dbi2(ep, func_no, reg, 0x0);
	/* [한국어] DBI 창 쓰기 = BAR 본체(주소/종류 플래그)를 0 으로. 마스크와 본체를 둘 다 지워야 완전히 사라진다 */
	dw_pcie_ep_writel_dbi(ep, func_no, reg, 0x0);
	/* [한국어] 64비트 BAR 이었다면 상위 32비트가 바로 다음 BAR 자리를 쓰고 있었으므로 그쪽도 지워야 한다 */
	if (flags & PCI_BASE_ADDRESS_MEM_TYPE_64) {
		/* [한국어] 다음 자리(reg + 4)의 마스크를 0 으로 */
		dw_pcie_ep_writel_dbi2(ep, func_no, reg + 4, 0x0);
		/* [한국어] 다음 자리의 BAR 본체를 0 으로 */
		dw_pcie_ep_writel_dbi(ep, func_no, reg + 4, 0x0);
	}
	dw_pcie_dbi_ro_wr_dis(pci);
}

/**
 * dw_pcie_ep_reset_bar - Reset endpoint BAR
 * @pci: DWC PCI device
 * @bar: BAR number of the endpoint
 */
/* [한국어]
 * dw_pcie_ep_reset_bar - 모든 함수에 대해 같은 번호의 BAR 을 끈다
 *
 * @pci: DWC 컨트롤러.
 * @bar: 끌 BAR 번호.
 * @return: 없음.
 *
 * __dw_pcie_ep_reset_bar() 를 함수 0 부터 max_functions-1 까지 돌린다. flags 에
 * 0 을 넘기므로 64비트 상위 절반은 건드리지 않는데, 이 경로는 "아직 아무도 쓰지
 * 않은 BAR 을 기본값으로 꺼 두는" 초기화용이라 64비트로 쓰였을 리가 없기 때문이다.
 *
 * EXPORT_SYMBOL_GPL 로 내보내며, 실제 호출자는 SoC 글루 드라이버들이다. 이들은
 * 링크가 끊겼다 붙거나 PERST# 리셋이 왔을 때 BAR 을 초기 상태로 되돌리려고 부른다.
 * 이 파일 안에서는 dw_pcie_ep_disable_bars() 가 부른다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   SoC 글루의 init 훅 / dw_pcie_ep_disable_bars() → [이 함수] → __dw_pcie_ep_reset_bar()
 */
void dw_pcie_ep_reset_bar(struct dw_pcie *pci, enum pci_barno bar)
{
	u8 func_no, funcs;

	/* [한국어] 이 컨트롤러가 노출하는 PCI 함수 개수. dw_pcie_ep_get_resources() 가 DT 의 max-functions 로 정해 둔 값 */
	funcs = pci->ep.epc->max_functions;

	/* [한국어] 함수 0 부터 끝까지 — 같은 번호의 BAR 이라도 함수마다 별개의 레지스터다 */
	for (func_no = 0; func_no < funcs; func_no++)
		/* [한국어] flags 에 0 을 넘긴다. 이 경로는 '아직 아무도 쓰지 않은 BAR 을 꺼 두는' 초기화용이라 64비트로 쓰였을 리가 없다 */
		__dw_pcie_ep_reset_bar(pci, func_no, bar, 0);
}
EXPORT_SYMBOL_GPL(dw_pcie_ep_reset_bar);

/* [한국어]
 * dw_pcie_ep_find_capability - 특정 함수의 표준 capability 오프셋을 찾는다
 *
 * @ep: DWC 엔드포인트 인스턴스.
 * @func_no: 대상 PCI 함수 번호.
 * @cap: 찾을 capability ID(PCI_CAP_ID_MSI, PCI_CAP_ID_MSIX, PCI_CAP_ID_EXP 등).
 * @return: 설정 공간 안의 바이트 오프셋. 없으면 0.
 *
 * 커널의 pci_find_capability() 는 struct pci_dev 를 받는데, 여기서 뒤지려는 것은
 * 아직 어떤 버스에도 열거되지 않은 "우리 자신" 의 설정 공간이다. 그래서 PCI 코어의
 * 탐색 매크로 PCI_FIND_NEXT_CAP() 에 DBI 전용 읽기 함수를 끼워 넣어 같은 순회 논리만
 * 재사용한다. 매크로는 첫 인자를 접두사로 삼아 토큰을 이어 붙이므로,
 * dw_pcie_ep_read_cfg 는 실제로는 dw_pcie_ep_read_cfg_byte/_word/_dword 세 인라인
 * 함수를 가리키고, 뒤의 (ep, func_no) 는 그 함수들의 선행 인자로 흘러 들어간다.
 * PCI_CAPABILITY_LIST(0x34)가 리스트의 머리이고, 매크로는 TTL 48 로 사이클을 막는다.
 *
 * 실행 컨텍스트: 초기화 중 프로세스 컨텍스트. DBI 읽기라 잠들지 않는다.
 *
 * 호출 체인:
 *   dw_pcie_ep_init_registers()/dw_pcie_ep_init_non_sticky_registers()
 *     → [이 함수] → PCI_FIND_NEXT_CAP() → dw_pcie_ep_read_cfg_byte/_word()
 */
static u8 dw_pcie_ep_find_capability(struct dw_pcie_ep *ep, u8 func_no, u8 cap)
{
	return PCI_FIND_NEXT_CAP(dw_pcie_ep_read_cfg, PCI_CAPABILITY_LIST,
				 cap, NULL, ep, func_no);
}

/* [한국어]
 * dw_pcie_ep_find_ext_capability - 특정 함수의 PCIe 확장 capability 오프셋을 찾는다
 *
 * @ep: DWC 엔드포인트 인스턴스.
 * @func_no: 대상 PCI 함수 번호.
 * @cap: 찾을 확장 capability ID(여기서는 PCI_EXT_CAP_ID_REBAR).
 * @return: 설정 공간 오프셋(0x100 이상). 없으면 0.
 *
 * 표준 capability 리스트가 설정 공간 앞쪽 256바이트 안에서 1바이트 오프셋으로
 * 이어지는 것과 달리, PCIe 확장 capability 는 0x100 부터 4096바이트까지 뻗은
 * 확장 영역에 4바이트 헤더로 이어진다. 그래서 전용 매크로 PCI_FIND_NEXT_EXT_CAP()
 * 을 쓴다. start 로 0 을 넘기면 매크로가 알아서 0x100 부터 시작한다.
 * 이 파일에서는 Resizable BAR(PCI_EXT_CAP_ID_REBAR) 하나를 찾는 데만 쓴다.
 *
 * 실행 컨텍스트: 초기화/set_bar 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   dw_pcie_ep_get_rebar_offset()/dw_pcie_ep_init_rebar_registers()
 *     → [이 함수] → PCI_FIND_NEXT_EXT_CAP() → dw_pcie_ep_read_cfg_dword()
 */
static u16 dw_pcie_ep_find_ext_capability(struct dw_pcie_ep *ep,
					  u8 func_no, u8 cap)
{
	return PCI_FIND_NEXT_EXT_CAP(dw_pcie_ep_read_cfg, 0,
				     cap, NULL, ep, func_no);
}

/* [한국어]
 * dw_pcie_ep_write_header - EPF 가 정한 정체성을 설정 공간 헤더에 새긴다
 *
 * @epc: EPC 프레임워크가 넘겨주는 컨트롤러 객체. drvdata 에 dw_pcie_ep 가 들어 있다.
 * @func_no: 대상 물리 함수 번호.
 * @vfunc_no: SR-IOV 가상 함수 번호. 이 드라이버는 쓰지 않는다.
 * @hdr: EPF 드라이버가 채운 벤더 ID/디바이스 ID/클래스 코드 등.
 * @return: 항상 0.
 *
 * 호스트가 이 엔드포인트를 열거할 때 처음 읽는 값들이 여기서 정해진다. 즉 이 함수가
 * "우리는 어떤 장치인가" 를 선언한다 — 호스트의 드라이버 매칭은 여기 쓴 벤더/디바이스
 * ID 로 이뤄지므로, 이 값이 틀리면 호스트 쪽에서 원하는 드라이버가 붙지 않는다.
 *
 * 이 필드들은 PCI 규약상 읽기 전용이라, 앞뒤로 dw_pcie_dbi_ro_wr_en()/dis() 를 감아
 * 잠시 쓰기를 허용한다. PCI_CLASS_DEVICE 는 subclass(하위 바이트)와 base class(상위
 * 바이트)를 한 word 에 합쳐 쓰므로 baseclass_code 를 8비트 올려 OR 한다.
 * 폭이 제각각인 이유는 규약상 필드 폭이 그렇기 때문이다 — revid/progif/cache line/
 * interrupt pin 은 1바이트, 벤더/디바이스/서브시스템 ID 는 2바이트.
 *
 * 실행 컨텍스트: epc->lock 뮤텍스를 쥔 프로세스 컨텍스트(EPC 코어가 잡아 준다).
 * 실패 경로가 없어 항상 0 을 돌려준다.
 *
 * 호출 체인:
 *   EPF 드라이버 bind → pci_epc_write_header() → [이 함수] → dw_pcie_ep_writew_dbi() 등
 */
static int dw_pcie_ep_write_header(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
				   struct pci_epf_header *hdr)
{
	struct dw_pcie_ep *ep = epc_get_drvdata(epc);
	/* [한국어] ep 를 품고 있는 dw_pcie 를 container_of 로 되찾는다. ro_wr_en/dis 가 dw_pcie 를 받기 때문 */
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);

	dw_pcie_dbi_ro_wr_en(pci);
	/* [한국어] 벤더 ID(2바이트). 호스트의 드라이버 매칭이 이 값과 다음 줄의 디바이스 ID 로 이뤄진다 */
	dw_pcie_ep_writew_dbi(ep, func_no, PCI_VENDOR_ID, hdr->vendorid);
	/* [한국어] 디바이스 ID(2바이트) */
	dw_pcie_ep_writew_dbi(ep, func_no, PCI_DEVICE_ID, hdr->deviceid);
	/* [한국어] 리비전 ID(1바이트). 같은 디바이스의 하드웨어 세대를 구분하는 값 */
	dw_pcie_ep_writeb_dbi(ep, func_no, PCI_REVISION_ID, hdr->revid);
	/* [한국어] 프로그래밍 인터페이스 코드(1바이트). 클래스 코드 3바이트 중 최하위 자리 */
	dw_pcie_ep_writeb_dbi(ep, func_no, PCI_CLASS_PROG, hdr->progif_code);
	/* [한국어] 클래스 코드의 나머지 2바이트를 한 번에 쓴다. 하위 바이트가 subclass, 상위 바이트가 base class 라 8비트 올려 OR 한다 */
	dw_pcie_ep_writew_dbi(ep, func_no, PCI_CLASS_DEVICE,
			      hdr->subclass_code | hdr->baseclass_code << 8);
	/* [한국어] 캐시 라인 크기(1바이트). 규약상 헤더에 있는 필드라 EPF 가 준 값을 그대로 옮긴다 */
	dw_pcie_ep_writeb_dbi(ep, func_no, PCI_CACHE_LINE_SIZE,
			      hdr->cache_line_size);
	/* [한국어] 서브시스템 벤더 ID(2바이트). 같은 칩을 여러 보드 벤더가 쓸 때 보드를 구분하는 값 */
	dw_pcie_ep_writew_dbi(ep, func_no, PCI_SUBSYSTEM_VENDOR_ID,
			      hdr->subsys_vendor_id);
	/* [한국어] 서브시스템 ID(2바이트) */
	dw_pcie_ep_writew_dbi(ep, func_no, PCI_SUBSYSTEM_ID, hdr->subsys_id);
	/* [한국어] INTx 핀 번호(1바이트). 0 이면 INTx 를 쓰지 않는다는 뜻 */
	dw_pcie_ep_writeb_dbi(ep, func_no, PCI_INTERRUPT_PIN,
			      hdr->interrupt_pin);
	dw_pcie_dbi_ro_wr_dis(pci);

	return 0;
}

/* BAR Match Mode inbound iATU mapping */
/* [한국어]
 * dw_pcie_ep_ib_atu_bar - BAR Match Mode 로 인바운드 iATU 창을 하나 건다
 *
 * @ep: DWC 엔드포인트 인스턴스.
 * @func_no: 대상 함수 번호.
 * @type: 트랜잭션 종류. PCIE_ATU_TYPE_MEM 또는 PCIE_ATU_TYPE_IO.
 * @parent_bus_addr: 이 BAR 로 들어온 접근을 실제로 떨어뜨릴 로컬(부모 버스) 물리주소.
 * @bar: 대상 BAR 번호.
 * @size: 매핑 크기.
 * @return: 0 성공, -EINVAL 이면 함수가 없거나 남은 인바운드 창이 없음, 그 외 iATU 오류.
 *
 * 엔드포인트의 인바운드 매핑이 호스트 쪽과 근본적으로 다른 점이 여기 있다. 호스트는
 * 자기가 어떤 주소 범위를 볼지 미리 알지만, 엔드포인트의 BAR 주소는 호스트가 열거하며
 * 나중에 정해 주므로 소프트웨어가 미리 알 수 없다. 그래서 주소로 매칭하는 대신
 * "이 BAR 로 온 트랜잭션" 으로 매칭하는 BAR Match Mode 를 쓴다. 실제 비트 조작은
 * dw_pcie_prog_ep_inbound_atu() 안에서 PCIE_ATU_BAR_MODE_ENABLE 로 이뤄진다.
 *
 * 창 선택 규칙: 이 BAR 이 아직 창을 안 잡았으면 ib_window_map 비트맵에서 첫 빈 자리를
 * 찾고, 이미 잡았으면 그 창을 그대로 다시 쓴다. bar_to_atu[] 에는 인덱스를 그대로가
 * 아니라 +1 해서 넣는데, 0 을 "미할당" 표시로 쓰고 있어 창 0 과 구분해야 하기 때문이다.
 * 이 규약은 원문 주석이 명시하고 있고, dw_pcie_ep_clear_ib_maps() 가 -1 로 되돌린다.
 *
 * 실행 컨텍스트: epc->lock 아래의 프로세스 컨텍스트.
 * 에러 경로: 창 부족이나 iATU 프로그래밍 실패면 비트맵을 건드리지 않고 그대로 돌아가,
 * 호출자(dw_pcie_ep_set_bar)가 EPC 코어로 오류를 올린다.
 *
 * 호출 체인:
 *   pci_epc_set_bar() → dw_pcie_ep_set_bar() → [이 함수] → dw_pcie_prog_ep_inbound_atu()
 */
static int dw_pcie_ep_ib_atu_bar(struct dw_pcie_ep *ep, u8 func_no, int type,
				 dma_addr_t parent_bus_addr, enum pci_barno bar,
				 size_t size)
{
	int ret;
	/* [한국어] 쓸 인바운드 창의 인덱스를 담을 변수 */
	u32 free_win;
	/* [한국어] ep 를 품은 dw_pcie 를 되찾는다. 창 개수와 iATU 함수가 이쪽에 있다 */
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	/* [한국어] 이 함수 번호의 상태 구조체. bar_to_atu[] 가 여기 들어 있다 */
	struct dw_pcie_ep_func *ep_func = dw_pcie_ep_get_func_from_ep(ep, func_no);

	/* [한국어] 등록되지 않은 함수 번호면 더 진행할 수 없다 */
	if (!ep_func)
		return -EINVAL;

	/* [한국어] 이 BAR 이 아직 창을 잡은 적이 없으면(0 = 미할당) */
	if (!ep_func->bar_to_atu[bar])
		/* [한국어] 인바운드 창 비트맵에서 첫 빈 자리를 찾는다. num_ib_windows 는 dw_pcie_iatu_detect() 가 탐지한 값 */
		free_win = find_first_zero_bit(ep->ib_window_map, pci->num_ib_windows);
	else
		/* [한국어] 이미 창을 잡았으면 그것을 재사용한다. 저장값이 +1 되어 있으므로 1 을 빼 실제 인덱스로 되돌린다 */
		free_win = ep_func->bar_to_atu[bar] - 1;

	/* [한국어] find_first_zero_bit 은 빈 자리가 없으면 크기를 그대로 돌려준다. 그 경우가 곧 창 고갈이다 */
	if (free_win >= pci->num_ib_windows) {
		/* [한국어] 창이 없으면 이 BAR 을 세울 방법이 없다 */
		dev_err(pci->dev, "No free inbound window\n");
		return -EINVAL;
	}

	/* [한국어] BAR Match Mode 로 창을 건다. 주소가 아니라 bar 번호로 매칭하도록 PCIE_ATU_BAR_MODE_ENABLE 을 세우는 것이 이 함수 안에서 일어난다 */
	ret = dw_pcie_prog_ep_inbound_atu(pci, func_no, free_win, type,
					  parent_bus_addr, bar, size);
	/* [한국어] iATU 프로그래밍 실패(정렬 위반이나 잘못된 인덱스 등) */
	if (ret < 0) {
		/* [한국어] 실패했으므로 비트맵을 건드리지 않고 그대로 돌아간다 — 창은 여전히 비어 있는 상태 */
		dev_err(pci->dev, "Failed to program IB window\n");
		return ret;
	}

	/*
	 * Always increment free_win before assignment, since value 0 is used to identify
	 * unallocated mapping.
	 */
	ep_func->bar_to_atu[bar] = free_win + 1;
	/* [한국어] 이제서야 비트맵에 쓰였다고 표시한다. 프로그래밍이 성공한 뒤에 세워야 실패 시 창이 새지 않는다 */
	set_bit(free_win, ep->ib_window_map);

	return 0;
}

/* [한국어]
 * dw_pcie_ep_clear_ib_maps - 한 BAR 에 걸린 인바운드 iATU 창을 모두 걷어낸다
 *
 * @ep: DWC 엔드포인트 인스턴스.
 * @func_no: 대상 함수 번호.
 * @bar: 대상 BAR 번호.
 * @return: 없음.
 *
 * 인바운드 매핑은 두 방식 중 하나로 걸려 있다 — BAR Match Mode 는 창 한 개,
 * Address Match Mode 는 submap 개수만큼 여러 개. 이 함수가 두 경우를 모두 처리한다.
 * 먼저 bar_to_atu[bar] 가 0 이 아니면 BAR Match 방식이므로 그 창 하나만 끄고 끝낸다
 * (두 방식이 동시에 걸리는 일은 없다). 아니면 ib_atu_indexes[bar] 배열을 돌며 전부 끈다.
 *
 * 배열 포인터와 개수를 먼저 지역 변수로 빼내고 구조체 쪽을 NULL/0 으로 만든 뒤에
 * 순회하는 것이 중요하다. 그래야 중간에 실패해도 이미 해제한 배열을 다시 참조하지
 * 않는다. 배열은 devm_kcalloc 으로 잡았으므로 devm_kfree 로 즉시 돌려준다 —
 * 드라이버 해제까지 기다리면 set_bar 를 반복할 때마다 메모리가 쌓인다.
 *
 * 실행 컨텍스트: epc->lock 아래의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   dw_pcie_ep_clear_bar() / dw_pcie_ep_set_bar()(동적 변경) /
 *   dw_pcie_ep_ib_atu_addr()(실패 되감기) → [이 함수] → dw_pcie_disable_atu()
 */
static void dw_pcie_ep_clear_ib_maps(struct dw_pcie_ep *ep, u8 func_no, enum pci_barno bar)
{
	struct dw_pcie_ep_func *ep_func = dw_pcie_ep_get_func_from_ep(ep, func_no);
	/* [한국어] iATU 해제 함수와 dev 포인터를 얻으려고 dw_pcie 를 되찾는다 */
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	/* [한국어] devm_kfree 에 넘길 디바이스. devm 할당의 주인이 이 디바이스다 */
	struct device *dev = pci->dev;
	/* [한국어] i 는 순회 인덱스, num 은 걷어낼 창의 개수 */
	unsigned int i, num;
	/* [한국어] BAR Match Mode 경로에서 쓸 단일 창 인덱스 */
	u32 atu_index;
	/* [한국어] Address Match Mode 경로에서 쓸 창 인덱스 배열 */
	u32 *indexes;

	/* [한국어] 등록되지 않은 함수면 걷어낼 것도 없다 */
	if (!ep_func)
		return;

	/* Tear down the BAR Match Mode mapping, if any. */
	if (ep_func->bar_to_atu[bar]) {
		/* [한국어] 저장값이 +1 되어 있으므로 1 을 빼 실제 인덱스로 되돌린다 */
		atu_index = ep_func->bar_to_atu[bar] - 1;
		/* [한국어] 그 인바운드 창을 끈다. 이 순간부터 그 BAR 로 온 트랜잭션은 갈 곳이 없다 */
		dw_pcie_disable_atu(pci, PCIE_ATU_REGION_DIR_IB, atu_index);
		/* [한국어] 비트맵 비트를 내려 다른 매핑이 이 창을 쓸 수 있게 한다 */
		clear_bit(atu_index, ep->ib_window_map);
		/* [한국어] 미할당 표시(0)로 되돌린다 */
		ep_func->bar_to_atu[bar] = 0;
		return;
	}

	/* Tear down all Address Match Mode mappings, if any. */
	indexes = ep_func->ib_atu_indexes[bar];
	/* [한국어] 실제로 프로그래밍에 성공한 창의 개수. 중간 실패 시 그만큼만 걷어야 한다 */
	num = ep_func->num_ib_atu_indexes[bar];
	/* [한국어] 구조체 쪽을 먼저 비운다 — 아래에서 실패해도 해제된 배열을 다시 참조하지 않도록 */
	ep_func->ib_atu_indexes[bar] = NULL;
	/* [한국어] 개수도 함께 0 으로 */
	ep_func->num_ib_atu_indexes[bar] = 0;
	/* [한국어] Address Match 매핑을 건 적이 없으면 배열이 NULL 이다 */
	if (!indexes)
		return;
	/* [한국어] 성공한 개수만큼만 돈다. num_submap 전체가 아닌 이유는 중간 실패 가능성 때문 */
	for (i = 0; i < num; i++) {
		/* [한국어] 조각마다 걸었던 인바운드 창을 하나씩 끈다 */
		dw_pcie_disable_atu(pci, PCIE_ATU_REGION_DIR_IB, indexes[i]);
		/* [한국어] 해당 비트맵 비트를 내려 창을 반납한다 */
		clear_bit(indexes[i], ep->ib_window_map);
	}
	devm_kfree(dev, indexes);
}

/* [한국어]
 * dw_pcie_ep_read_bar_assigned - 호스트가 이 BAR 에 배정한 PCI 주소를 되읽는다
 *
 * @ep: DWC 엔드포인트 인스턴스.
 * @func_no: 대상 함수 번호.
 * @bar: 대상 BAR 번호.
 * @flags: 이 BAR 의 종류 플래그. I/O 인지, 64비트인지 판단에 쓴다.
 * @return: 호스트가 배정한 주소. 아직 배정 전이면 0.
 *
 * 엔드포인트 쪽에서 BAR 레지스터를 "읽는" 드문 경우다. 평소에는 소프트웨어가 쓰기만
 * 하지만, 호스트가 열거를 마치면 그 레지스터에 호스트가 정한 주소가 들어 있다.
 * Address Match Mode 로 BAR 을 조각내 매핑하려면 그 실제 주소를 알아야 하므로
 * 여기서 되읽는다.
 *
 * 하위 비트는 주소가 아니라 종류 표시(메모리/IO, 32/64비트, prefetchable)라서
 * 마스크로 걷어내야 한다. I/O BAR 은 하위 2비트, 메모리 BAR 은 하위 4비트가 플래그다.
 * 64비트 BAR 이면 상위 32비트가 다음 자리(reg + 4)에 있으므로 이어 붙인다.
 * 0 이 돌아오면 아직 호스트가 열거하지 않았다는 뜻이고, 호출자는 그때 -EINVAL 로 거절한다.
 *
 * 실행 컨텍스트: epc->lock 아래의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   dw_pcie_ep_ib_atu_addr() → [이 함수] → dw_pcie_ep_readl_dbi()
 */
static u64 dw_pcie_ep_read_bar_assigned(struct dw_pcie_ep *ep, u8 func_no,
					enum pci_barno bar, int flags)
{
	u32 reg = PCI_BASE_ADDRESS_0 + (4 * bar);
	/* [한국어] BAR 본체의 하위 32비트와 상위 32비트를 담을 변수 */
	u32 lo, hi;
	/* [한국어] 두 조각을 합쳐 만든 64비트 주소 */
	u64 addr;

	/* [한국어] BAR 레지스터를 읽는다 — 여기 들어 있는 값은 우리가 쓴 것이 아니라 호스트가 열거하며 배정한 주소다 */
	lo = dw_pcie_ep_readl_dbi(ep, func_no, reg);

	/* [한국어] PCI_BASE_ADDRESS_SPACE 비트가 1 이면 I/O BAR. 메모리 BAR 과 플래그 폭이 다르다 */
	if (flags & PCI_BASE_ADDRESS_SPACE)
		/* [한국어] I/O BAR 은 하위 2비트만 플래그이므로 그것만 걷어낸다 */
		return lo & PCI_BASE_ADDRESS_IO_MASK;

	/* [한국어] 메모리 BAR 은 하위 4비트가 플래그(공간 종류, 32/64비트, prefetchable)라 마스크로 지운다 */
	addr = lo & PCI_BASE_ADDRESS_MEM_MASK;
	/* [한국어] 32비트 BAR 이면 여기서 끝 — 상위 절반이 없다 */
	if (!(flags & PCI_BASE_ADDRESS_MEM_TYPE_64))
		/* [한국어] 하위 32비트가 곧 전체 주소 */
		return addr;

	/* [한국어] 64비트 BAR 의 상위 32비트는 다음 BAR 자리에 그대로(플래그 없이) 들어 있다 */
	hi = dw_pcie_ep_readl_dbi(ep, func_no, reg + 4);
	return addr | ((u64)hi << 32);
}

/* [한국어]
 * dw_pcie_ep_validate_submap - submap 배열이 BAR 을 빈틈없이 덮는지 미리 검사한다
 *
 * @ep: DWC 엔드포인트 인스턴스.
 * @submap: EPF 가 넘긴 조각 배열. 각 원소는 크기와 로컬 물리주소를 갖는다.
 * @num_submap: 조각 개수.
 * @bar_size: 이 BAR 전체 크기.
 * @return: 0 이면 유효, -EINVAL 이면 규칙 위반.
 *
 * submap 배열의 순서가 곧 BAR 안에서의 배치다 — 0번 조각이 오프셋 0 에서 시작하고
 * 그다음 조각이 바로 뒤에 이어 붙는다. 오프셋을 따로 받지 않고 크기를 누적해서
 * 계산하는 구조이므로, 배열이 "겹치지도 비지도 않게 BAR 전체를 정확히 덮는" 분해여야
 * 한다. 이 함수가 그 조건을 하나씩 확인한다: 크기가 0 이 아닐 것, 크기·누적 오프셋·
 * 물리주소가 모두 컨트롤러의 iATU 정렬 단위에 맞을 것, BAR 범위를 넘지 않을 것,
 * 그리고 마지막에 누적 합이 BAR 크기와 정확히 같을 것.
 *
 * 원문 주석이 밝히듯 dw_pcie_prog_inbound_atu() 도 정렬을 다시 검사한다. 그런데도
 * 여기서 미리 보는 이유는, 중간에 실패하면 이미 프로그래밍한 창들을 도로 걷어내야
 * 하는 헛수고가 생기기 때문이다. off > bar_size 와 size > bar_size - off 로 나눠 쓴 것은
 * off + size 로 더했다가 오버플로가 나는 것을 피하려는 형태다.
 *
 * 실행 컨텍스트: epc->lock 아래의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   dw_pcie_ep_set_bar() → dw_pcie_ep_ib_atu_addr() → [이 함수]
 */
static int dw_pcie_ep_validate_submap(struct dw_pcie_ep *ep,
				      const struct pci_epf_bar_submap *submap,
				      unsigned int num_submap, size_t bar_size)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	/* [한국어] 이 컨트롤러의 iATU 정렬 단위. dw_pcie_iatu_detect() 가 실제 창에 값을 써 보며 알아낸 값이다 */
	u32 align = pci->region_align;
	/* [한국어] 지금까지 훑은 조각들의 크기 합 = 다음 조각이 시작할 BAR 내 오프셋 */
	size_t off = 0;
	/* [한국어] 조각 순회 인덱스 */
	unsigned int i;
	/* [한국어] 현재 조각의 크기 */
	size_t size;

	/* [한국어] 정렬 단위를 모르거나(0) BAR 크기 자체가 그 단위에 맞지 않으면 조각으로 나눌 수 없다 */
	if (!align || !IS_ALIGNED(bar_size, align))
		return -EINVAL;

	/*
	 * The submap array order defines the BAR layout (submap[0] starts
	 * at offset 0 and each entry immediately follows the previous
	 * one). Here, validate that it forms a strict, gapless
	 * decomposition of the BAR:
	 *  - each entry has a non-zero size
	 *  - sizes, implicit offsets and phys_addr are aligned to
	 *    pci->region_align
	 *  - each entry lies within the BAR range
	 *  - the entries exactly cover the whole BAR
	 *
	 * Note: dw_pcie_prog_inbound_atu() also checks alignment for the
	 * PCI address and the target phys_addr, but validating up-front
	 * avoids partially programming iATU windows in vain.
	 */
	for (i = 0; i < num_submap; i++) {
		/* [한국어] 이 조각의 크기 */
		size = submap[i].size;

		/* [한국어] 크기 0 인 조각은 배치를 계산할 수 없다 */
		if (!size)
			return -EINVAL;

		/* [한국어] 조각의 크기와 시작 오프셋이 둘 다 정렬 단위에 맞아야 창을 걸 수 있다 */
		if (!IS_ALIGNED(size, align) || !IS_ALIGNED(off, align))
			return -EINVAL;

		/* [한국어] 조각이 붙을 로컬 물리주소도 같은 단위로 정렬돼야 한다 */
		if (!IS_ALIGNED(submap[i].phys_addr, align))
			return -EINVAL;

		/* [한국어] BAR 범위를 벗어나는지 확인. off + size 로 더하지 않는 것은 오버플로를 피하기 위해서다 */
		if (off > bar_size || size > bar_size - off)
			return -EINVAL;

		/* [한국어] 다음 조각의 시작 오프셋으로 누적 */
		off += size;
	}
	/* [한국어] 마지막에 정확히 BAR 크기와 같아야 한다 — 모자라면 구멍이, 넘치면 이미 위에서 걸렸다 */
	if (off != bar_size)
		return -EINVAL;

	return 0;
}

/* Address Match Mode inbound iATU mapping */
/* [한국어]
 * dw_pcie_ep_ib_atu_addr - Address Match Mode 로 BAR 을 여러 조각으로 쪼개 매핑한다
 *
 * @ep: DWC 엔드포인트 인스턴스.
 * @func_no: 대상 함수 번호.
 * @type: PCIE_ATU_TYPE_MEM 또는 PCIE_ATU_TYPE_IO.
 * @epf_bar: EPF 가 채운 BAR 요청. submap 배열과 그 개수가 들어 있다.
 * @return: 0 성공, -EINVAL(인자/미배정 BAR), -ENOMEM(인덱스 배열), -ENOSPC(창 부족), iATU 오류.
 *
 * BAR Match Mode 는 BAR 하나를 연속된 물리 메모리 한 덩어리에만 붙일 수 있다.
 * 그보다 잘게, 예컨대 BAR 앞쪽 절반은 이 버퍼에 뒤쪽 절반은 저 버퍼에 붙이려면
 * 조각마다 창을 따로 걸어야 하고, 그러려면 주소로 매칭해야 하므로 호스트가 배정한
 * BAR 주소를 알아야 한다. 그래서 이 경로는 dw_pcie_ep_read_bar_assigned() 로
 * 실제 주소를 되읽는 것부터 시작한다 — 아직 열거 전이라 0 이면 할 수 있는 일이 없다.
 *
 * 동작: (1) submap 이 BAR 을 빈틈없이 덮는지 검증, (2) 호스트 배정 주소 확보,
 * (3) 조각 수만큼 iATU 인덱스 기록용 배열 할당, (4) 조각마다 빈 인바운드 창을 찾아
 * "BAR 주소 + 누적 오프셋 → 조각의 로컬 물리주소" 로 프로그래밍. 성공한 개수를
 * num_ib_atu_indexes 에 그때그때 갱신하므로, 중간 실패 시 err 로 가서
 * dw_pcie_ep_clear_ib_maps() 가 이미 건 것만 정확히 되돌린다.
 *
 * 실행 컨텍스트: epc->lock 아래의 프로세스 컨텍스트. 인덱스 배열은 devm 이라
 * 드라이버 수명에 묶이지만, clear_ib_maps 가 즉시 devm_kfree 로 돌려준다.
 *
 * 호출 체인:
 *   pci-epf-test 등 EPF → pci_epc_set_bar() → dw_pcie_ep_set_bar()
 *     → [이 함수] → dw_pcie_prog_inbound_atu()
 */
static int dw_pcie_ep_ib_atu_addr(struct dw_pcie_ep *ep, u8 func_no, int type,
				  const struct pci_epf_bar *epf_bar)
{
	struct dw_pcie_ep_func *ep_func = dw_pcie_ep_get_func_from_ep(ep, func_no);
	/* [한국어] EPF 가 준 조각 배열. const 인 것은 이 파일이 읽기만 한다는 뜻이다 */
	const struct pci_epf_bar_submap *submap = epf_bar->submap;
	/* [한국어] iATU 함수와 창 개수를 얻으려고 dw_pcie 를 되찾는다 */
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	/* [한국어] 대상 BAR 번호 */
	enum pci_barno bar = epf_bar->barno;
	/* [한국어] devm_kcalloc 과 dev_err 에 넘길 디바이스 */
	struct device *dev = pci->dev;
	/* [한국어] pci_addr 은 조각이 담당할 호스트 쪽 주소, parent_bus_addr 은 그것이 떨어질 로컬 물리주소 */
	u64 pci_addr, parent_bus_addr;
	/* [한국어] size 는 현재 조각 크기, base 는 호스트가 배정한 BAR 시작 주소, off 는 BAR 안 누적 오프셋 */
	u64 size, base, off = 0;
	/* [한국어] free_win 은 잡은 창 번호(int 인 것은 find_first_zero_bit 결과를 비교하기 위함), ret 는 오류 코드 */
	int free_win, ret;
	/* [한국어] 조각 순회 인덱스 */
	unsigned int i;
	/* [한국어] 조각별로 잡은 창 번호를 기록할 배열. clear_ib_maps 가 이것으로 되돌린다 */
	u32 *indexes;

	/* [한국어] 인자 전부를 한 번에 검사한다 — 함수가 등록돼 있고, 조각이 있고, 배열 포인터가 유효하고, BAR 크기가 0 이 아니어야 한다 */
	if (!ep_func || !epf_bar->num_submap || !submap || !epf_bar->size)
		return -EINVAL;

	/* [한국어] 조각들이 BAR 을 빈틈없이 덮는지 먼저 확인. 창을 걸기 전에 봐야 헛수고를 막는다 */
	ret = dw_pcie_ep_validate_submap(ep, submap, epf_bar->num_submap,
					 epf_bar->size);
	/* [한국어] 검증 실패면 아무것도 건드리지 않고 그대로 올린다 */
	if (ret)
		return ret;

	/* [한국어] 호스트가 이 BAR 에 배정한 실제 주소를 되읽는다. Address Match Mode 는 이 주소를 알아야 성립한다 */
	base = dw_pcie_ep_read_bar_assigned(ep, func_no, bar, epf_bar->flags);
	/* [한국어] 0 이면 호스트가 아직 열거하지 않았다는 뜻 — 조각 매핑의 전제가 깨진다 */
	if (!base) {
		dev_err(dev,
			"BAR%u not assigned, cannot set up sub-range mappings\n",
			bar);
		return -EINVAL;
	}

	/* [한국어] 조각 수만큼 창 번호 기록 배열을 잡는다. devm 이지만 clear_ib_maps 가 즉시 devm_kfree 로 돌려준다 */
	indexes = devm_kcalloc(dev, epf_bar->num_submap, sizeof(*indexes),
			       GFP_KERNEL);
	/* [한국어] 메모리 부족 */
	if (!indexes)
		return -ENOMEM;

	/* [한국어] 배열을 구조체에 먼저 걸어 둔다. 그래야 아래에서 실패했을 때 clear_ib_maps 가 찾아 걷을 수 있다 */
	ep_func->ib_atu_indexes[bar] = indexes;
	/* [한국어] 성공 개수는 0 에서 시작해 조각마다 늘린다 — 중간 실패 시 걷을 범위를 정확히 하기 위함 */
	ep_func->num_ib_atu_indexes[bar] = 0;

	/* [한국어] 조각을 순서대로 처리한다. 배열 순서가 곧 BAR 안의 배치다 */
	for (i = 0; i < epf_bar->num_submap; i++) {
		/* [한국어] 이 조각의 크기 */
		size = submap[i].size;
		/* [한국어] 이 조각이 붙을 로컬 물리주소 */
		parent_bus_addr = submap[i].phys_addr;

		/* [한국어] base + off 가 64비트를 넘치는지 검사. 뺄셈 형태로 비교해 오버플로 자체를 피한다 */
		if (off > (~0ULL) - base) {
			ret = -EINVAL;
			goto err;
		}

		/* [한국어] 이 조각이 담당할 호스트 쪽 주소 = BAR 시작 + 지금까지의 누적 오프셋 */
		pci_addr = base + off;
		/* [한국어] 다음 조각을 위해 오프셋을 누적. pci_addr 를 계산한 뒤에 더해야 순서가 맞는다 */
		off += size;

		/* [한국어] 인바운드 창 비트맵에서 빈 자리를 찾는다 */
		free_win = find_first_zero_bit(ep->ib_window_map,
					       pci->num_ib_windows);
		/* [한국어] 빈 자리가 없으면 크기를 그대로 돌려준다 — 창 고갈 */
		if (free_win >= pci->num_ib_windows) {
			ret = -ENOSPC;
			goto err;
		}

		/* [한국어] Address Match Mode 로 창을 건다. BAR Match 와 달리 pci_addr 범위로 매칭하므로 조각별로 다른 목적지를 줄 수 있다 */
		ret = dw_pcie_prog_inbound_atu(pci, free_win, type,
					       parent_bus_addr, pci_addr, size);
		/* [한국어] 정렬 위반 등으로 실패 */
		if (ret)
			goto err;

		/* [한국어] 프로그래밍이 성공한 뒤에야 창을 점유 표시한다 */
		set_bit(free_win, ep->ib_window_map);
		/* [한국어] 되돌리기용으로 창 번호를 기록 */
		indexes[i] = free_win;
		/* [한국어] 여기까지 성공했음을 개수로 남긴다. 다음 조각에서 실패하면 이 개수만큼만 걷힌다 */
		ep_func->num_ib_atu_indexes[bar] = i + 1;
	}
	return 0;
err:
	dw_pcie_ep_clear_ib_maps(ep, func_no, bar);
	return ret;
}

/* [한국어]
 * dw_pcie_ep_outbound_atu - 빈 아웃바운드 창을 하나 잡아 프로그래밍한다
 *
 * @ep: DWC 엔드포인트 인스턴스.
 * @atu: 호출자가 채운 아웃바운드 창 설정. index 는 이 함수가 채운다.
 * @return: 0 성공, -EINVAL 이면 남은 창 없음, 그 외 iATU 프로그래밍 오류.
 *
 * 아웃바운드는 인바운드와 방향이 반대다. 엔드포인트가 자기 로컬 물리주소에 쓰면
 * 그것이 호스트 메모리로 나가는 TLP 가 되도록 만드는 것이다. base 쪽이
 * parent_bus_addr(우리가 알아볼 로컬 범위), target 쪽이 pci_addr(TLP 가 향할 곳)이다.
 *
 * 창 관리는 ob_window_map 비트맵으로 한다. 빈 자리를 찾아 인덱스를 atu->index 에
 * 넣고 프로그래밍한 뒤에야 비트를 세운다 — 실패했는데 비트만 세워지면 그 창이
 * 영영 새지 않게 되기 때문이다. 성공하면 outbound_addr[index] 에 로컬 주소를
 * 적어 두는데, 나중에 unmap 할 때 주소만 가지고 어느 창인지 되찾기 위한 역인덱스다.
 *
 * 실행 컨텍스트: epc->lock 아래의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   dw_pcie_ep_map_addr() → [이 함수] → dw_pcie_prog_outbound_atu()
 */
static int dw_pcie_ep_outbound_atu(struct dw_pcie_ep *ep,
				   struct dw_pcie_ob_atu_cfg *atu)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	/* [한국어] 잡을 아웃바운드 창의 인덱스 */
	u32 free_win;
	/* [한국어] iATU 프로그래밍 결과 */
	int ret;

	/* [한국어] 아웃바운드 창 비트맵에서 빈 자리를 찾는다. num_ob_windows 는 dw_pcie_iatu_detect() 가 탐지한 값 */
	free_win = find_first_zero_bit(ep->ob_window_map, pci->num_ob_windows);
	/* [한국어] 빈 자리가 없으면 크기가 그대로 나온다 */
	if (free_win >= pci->num_ob_windows) {
		/* [한국어] 창 고갈 — 더 이상 호스트 메모리로 나가는 통로를 만들 수 없다 */
		dev_err(pci->dev, "No free outbound window\n");
		return -EINVAL;
	}

	/* [한국어] 호출자가 채워 준 설정에 인덱스만 이 함수가 채워 넣는다 */
	atu->index = free_win;
	/* [한국어] 실제 창 프로그래밍. base 는 parent_bus_addr(우리가 알아볼 로컬 범위), target 은 pci_addr(TLP 가 향할 곳)이다 */
	ret = dw_pcie_prog_outbound_atu(pci, atu);
	/* [한국어] 프로그래밍 실패면 비트맵을 건드리지 않고 그대로 올린다 — 창은 여전히 비어 있다 */
	if (ret)
		return ret;

	/* [한국어] 성공했으므로 창을 점유 표시 */
	set_bit(free_win, ep->ob_window_map);
	/* [한국어] unmap 때 주소만으로 이 창을 되찾기 위한 역인덱스. dw_pcie_find_index() 가 이 배열을 훑는다 */
	ep->outbound_addr[free_win] = atu->parent_bus_addr;

	return 0;
}

/* [한국어]
 * dw_pcie_ep_clear_bar - BAR 하나를 끄고 거기 걸린 인바운드 매핑을 모두 걷는다
 *
 * @epc: EPC 컨트롤러 객체.
 * @func_no: 대상 함수 번호.
 * @vfunc_no: 가상 함수 번호. 쓰지 않는다.
 * @epf_bar: 끌 BAR. barno 와 flags 를 본다.
 * @return: 없음(EPC 규약상 clear_bar 는 실패를 알리지 않는다).
 *
 * set_bar 의 정확한 역이다. 순서가 중요한데, 먼저 BAR 레지스터와 마스크를 0 으로
 * 지워 호스트에게 "이 BAR 은 없다" 로 보이게 만든 다음 iATU 창을 걷는다. 반대로 하면
 * 창이 사라진 뒤에도 BAR 이 살아 있어, 그 사이 호스트가 보낸 트랜잭션이 갈 곳을 잃는다.
 *
 * ep_func->epf_bar[bar] 가 비어 있으면 애초에 세워진 적이 없는 BAR 이므로 그냥 돌아간다.
 * 마지막에 그 포인터를 NULL 로 되돌려, set_bar 가 "처음 세우는 것" 과 "이미 세워진 것을
 * 동적으로 바꾸는 것" 을 구분하는 근거를 초기화한다.
 *
 * 실행 컨텍스트: epc->lock 아래의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   EPF unbind 등 → pci_epc_clear_bar() → [이 함수]
 *     → __dw_pcie_ep_reset_bar(), dw_pcie_ep_clear_ib_maps()
 */
static void dw_pcie_ep_clear_bar(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
				 struct pci_epf_bar *epf_bar)
{
	struct dw_pcie_ep *ep = epc_get_drvdata(epc);
	/* [한국어] BAR 리셋 함수에 넘길 dw_pcie */
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	/* [한국어] 끌 BAR 번호 */
	enum pci_barno bar = epf_bar->barno;
	/* [한국어] 이 함수 번호의 상태 구조체 */
	struct dw_pcie_ep_func *ep_func = dw_pcie_ep_get_func_from_ep(ep, func_no);

	/* [한국어] 등록되지 않은 함수이거나 애초에 세워진 적 없는 BAR 이면 할 일이 없다 */
	if (!ep_func || !ep_func->epf_bar[bar])
		return;

	/* [한국어] 먼저 BAR 을 호스트에게 안 보이게 만든다. 창을 먼저 걷으면 그 사이 들어온 트랜잭션이 갈 곳을 잃는다 */
	__dw_pcie_ep_reset_bar(pci, func_no, bar, epf_bar->flags);

	/* [한국어] 그다음 이 BAR 에 걸린 인바운드 창을 전부 걷는다(BAR Match 든 Address Match 든) */
	dw_pcie_ep_clear_ib_maps(ep, func_no, bar);

	ep_func->epf_bar[bar] = NULL;
}

/* [한국어]
 * dw_pcie_ep_get_rebar_offset - 특정 BAR 을 담당하는 Resizable BAR 제어 항목을 찾는다
 *
 * @ep: DWC 엔드포인트 인스턴스.
 * @func_no: 대상 함수 번호.
 * @bar: 찾고자 하는 BAR 번호.
 * @return: 그 BAR 의 REBAR CAP/CTRL 쌍이 놓인 설정 공간 오프셋. 없으면 0.
 *
 * Resizable BAR 확장 capability 는 헤더 하나 뒤에 (CAP, CTRL) 쌍이 여러 개 이어지는
 * 구조다. 몇 쌍인지는 첫 CTRL 의 NBAR 필드에 들어 있고, 각 쌍이 어느 BAR 을 담당하는지는
 * 그 쌍의 CTRL 안 BAR_IDX 필드에 들어 있다. 즉 순서와 BAR 번호가 일치한다는 보장이
 * 없어서, 쌍을 하나씩 돌며 BAR_IDX 를 확인해야 한다.
 *
 * 루프가 offset 을 PCI_REBAR_CTRL 씩 늘린다. 이 상수는 원래 "항목 안에서 CTRL 이
 * 놓인 상대 오프셋" 인데, 여기서는 "한 항목의 크기" 로도 쓰인다 — 항목이
 * (CAP, CTRL) 두 워드뿐이라 두 값이 같기 때문이다. 그 정의는 PCI 코어 헤더에 있고
 * 이 트리에는 들어 있지 않아 숫자 자체는 확인하지 못했지만, 코드가 같은 상수로
 * 전진과 CTRL 접근을 모두 하고 있다는 사실이 그 등식을 보여 준다.
 * 첫 바퀴의 offset 은 확장 capability 헤더 자리이고, 거기에 PCI_REBAR_CTRL 을 더한 곳이
 * 첫 항목의 CTRL 이다.
 *
 * 실행 컨텍스트: set_bar 경로의 프로세스 컨텍스트.
 * 0 을 돌려주면 호출자 dw_pcie_ep_set_bar_resizable() 이 -EINVAL 로 거절한다.
 *
 * 호출 체인:
 *   dw_pcie_ep_set_bar_resizable() → [이 함수] → dw_pcie_ep_find_ext_capability()
 */
static unsigned int dw_pcie_ep_get_rebar_offset(struct dw_pcie_ep *ep, u8 func_no,
						enum pci_barno bar)
{
	u32 reg, bar_index;
	/* [한국어] offset 은 순회하며 옮겨 다닐 위치, nbars 는 REBAR 항목 개수 */
	unsigned int offset, nbars;
	/* [한국어] 항목 순회 인덱스 */
	int i;

	/* [한국어] Resizable BAR 확장 capability 를 찾는다. 0x100 부터 시작하는 확장 영역에 있다 */
	offset = dw_pcie_ep_find_ext_capability(ep, func_no, PCI_EXT_CAP_ID_REBAR);
	/* [한국어] REBAR 자체가 없는 IP 설정 */
	if (!offset)
		/* [한국어] 0 을 그대로 돌려준다 — 호출자가 이것을 '없음' 으로 해석한다 */
		return offset;

	/* [한국어] 첫 항목의 CTRL 을 읽는다. offset 은 아직 capability 헤더 자리이고 거기에 PCI_REBAR_CTRL 을 더한 곳이 첫 CTRL 이다 */
	reg = dw_pcie_ep_readl_dbi(ep, func_no, offset + PCI_REBAR_CTRL);
	/* [한국어] NBAR 필드 = 이 capability 가 담고 있는 (CAP, CTRL) 쌍의 개수 */
	nbars = FIELD_GET(PCI_REBAR_CTRL_NBAR_MASK, reg);

	/* [한국어] 쌍을 하나씩 훑는다. 8바이트씩 전진하는 것은 한 쌍이 CAP 4바이트 + CTRL 4바이트이기 때문 */
	for (i = 0; i < nbars; i++, offset += PCI_REBAR_CTRL) {
		/* [한국어] 이번 쌍의 CTRL 을 읽는다 */
		reg = dw_pcie_ep_readl_dbi(ep, func_no, offset + PCI_REBAR_CTRL);
		/* [한국어] BAR_IDX 필드 = 이 쌍이 담당하는 BAR 번호. 항목 순서와 BAR 번호가 일치한다는 보장이 없어 확인해야 한다 */
		bar_index = FIELD_GET(PCI_REBAR_CTRL_BAR_IDX, reg);
		/* [한국어] 찾던 BAR 을 담당하는 쌍인지 */
		if (bar_index == bar)
			/* [한국어] 그 쌍의 시작 오프셋을 돌려준다. 호출자는 여기에 PCI_REBAR_CAP/CTRL 을 더해 접근한다 */
			return offset;
	}

	return 0;
}

/* [한국어]
 * dw_pcie_ep_set_bar_resizable - Resizable BAR 방식으로 BAR 크기를 광고한다
 *
 * @ep: DWC 엔드포인트 인스턴스.
 * @func_no: 대상 함수 번호.
 * @epf_bar: EPF 가 요청한 BAR 번호/크기/플래그.
 * @return: 0 성공, -EINVAL 이면 REBAR 항목이 없거나 크기를 표현할 수 없음.
 *
 * 보통의 BAR 은 마스크 레지스터에 (크기-1) 을 써서 크기를 정한다. Resizable BAR 은
 * 그러지 않는다 — 지원 가능한 크기 목록을 CAP 에 광고하면 컨트롤러가 "선택된 크기"
 * 비트를 갱신하고 마스크를 자동으로 유도한다. 그래서 원문 주석대로 마스크에는 크기를
 * 쓰지 않고 BIT(0) 만 써서 BAR 활성화 비트만 세운다.
 *
 * 순서: (1) 이 BAR 의 REBAR 항목 위치를 찾고, (2) 요청 크기를 CAP 비트로 변환하고,
 * (3) 읽기 전용 쓰기를 허용한 뒤 마스크에 BIT(0)/BAR 에 flags 를 쓰고,
 * (4) CTRL 의 상위 16비트를 지워 256TB~8EB 구간을 광고에서 빼고,
 * (5) CAP 에 지원 크기를 쓴다. 마지막 쓰기가 "선택된 크기" 필드를 자동 갱신하는
 * 부수효과를 노린 것이라 순서를 바꿀 수 없다 — 원문 주석이 DWC EP databook 5.96a 의
 * Figure 3-26 을 근거로 든다.
 *
 * 실행 컨텍스트: epc->lock 아래의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   dw_pcie_ep_set_bar() → [이 함수] → pci_epc_bar_size_to_rebar_cap(), DBI/DBI2 쓰기
 */
static int dw_pcie_ep_set_bar_resizable(struct dw_pcie_ep *ep, u8 func_no,
					struct pci_epf_bar *epf_bar)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	/* [한국어] 세울 BAR 번호 */
	enum pci_barno bar = epf_bar->barno;
	/* [한국어] EPF 가 요청한 크기 */
	size_t size = epf_bar->size;
	/* [한국어] BAR 종류 플래그(메모리/IO, 32/64비트, prefetchable) */
	int flags = epf_bar->flags;
	/* [한국어] 이 BAR 의 설정 공간 오프셋 = BAR0 + 4*bar */
	u32 reg = PCI_BASE_ADDRESS_0 + (4 * bar);
	/* [한국어] 이 BAR 을 담당하는 REBAR 항목의 위치 */
	unsigned int rebar_offset;
	/* [한국어] rebar_cap 은 광고할 지원 크기 비트, rebar_ctrl 은 읽고-고쳐-쓸 제어 워드 */
	u32 rebar_cap, rebar_ctrl;
	/* [한국어] 오류 코드 */
	int ret;

	/* [한국어] 이 BAR 의 REBAR 항목을 찾는다 */
	rebar_offset = dw_pcie_ep_get_rebar_offset(ep, func_no, bar);
	/* [한국어] 이 BAR 을 담당하는 항목이 없으면 Resizable 로 다룰 수 없다 */
	if (!rebar_offset)
		return -EINVAL;

	/* [한국어] 요청 크기를 REBAR CAP 의 비트 위치로 바꾼다. 1MB 가 비트 4 이고 그 위로 2배씩 올라간다 */
	ret = pci_epc_bar_size_to_rebar_cap(size, &rebar_cap);
	/* [한국어] 표현할 수 없는 크기(범위 밖이거나 2의 거듭제곱이 아님) */
	if (ret)
		return ret;

	dw_pcie_dbi_ro_wr_en(pci);

	/*
	 * A BAR mask should not be written for a resizable BAR. The BAR mask
	 * is automatically derived by the controller every time the "selected
	 * size" bits are updated, see "Figure 3-26 Resizable BAR Example for
	 * 32-bit Memory BAR0" in DWC EP databook 5.96a. We simply need to write
	 * BIT(0) to set the BAR enable bit.
	 */
	dw_pcie_ep_writel_dbi2(ep, func_no, reg, BIT(0));
	/* [한국어] BAR 본체에는 종류 플래그를 쓴다. 여기는 보통 BAR 과 같다 */
	dw_pcie_ep_writel_dbi(ep, func_no, reg, flags);

	/* [한국어] 64비트 BAR 이면 상위 절반도 정리해야 한다 */
	if (flags & PCI_BASE_ADDRESS_MEM_TYPE_64) {
		/* [한국어] 상위 절반 마스크는 0 — 크기는 CAP 이 정하므로 마스크에 쓸 것이 없다 */
		dw_pcie_ep_writel_dbi2(ep, func_no, reg + 4, 0);
		/* [한국어] 상위 절반 BAR 본체도 0. 그 자리에는 종류 플래그가 없다 */
		dw_pcie_ep_writel_dbi(ep, func_no, reg + 4, 0);
	}

	/*
	 * Bits 31:0 in PCI_REBAR_CAP define "supported sizes" bits for sizes
	 * 1 MB to 128 TB. Bits 31:16 in PCI_REBAR_CTRL define "supported sizes"
	 * bits for sizes 256 TB to 8 EB. Disallow sizes 256 TB to 8 EB.
	 */
	rebar_ctrl = dw_pcie_ep_readl_dbi(ep, func_no, rebar_offset + PCI_REBAR_CTRL);
	/* [한국어] CTRL 상위 16비트가 256TB~8EB 구간의 지원 크기 비트다. 원문 주석대로 그 범위를 아예 광고에서 뺀다 */
	rebar_ctrl &= ~GENMASK(31, 16);
	/* [한국어] 고친 CTRL 을 되쓴다. 여기까지가 '무엇을 광고하지 않을지' 를 정하는 단계 */
	dw_pcie_ep_writel_dbi(ep, func_no, rebar_offset + PCI_REBAR_CTRL, rebar_ctrl);

	/*
	 * The "selected size" (bits 13:8) in PCI_REBAR_CTRL are automatically
	 * updated when writing PCI_REBAR_CAP, see "Figure 3-26 Resizable BAR
	 * Example for 32-bit Memory BAR0" in DWC EP databook 5.96a.
	 */
	dw_pcie_ep_writel_dbi(ep, func_no, rebar_offset + PCI_REBAR_CAP, rebar_cap);

	dw_pcie_dbi_ro_wr_dis(pci);

	return 0;
}

/* [한국어]
 * dw_pcie_ep_set_bar_programmable - 마스크에 크기를 직접 써서 BAR 을 세운다
 *
 * @ep: DWC 엔드포인트 인스턴스.
 * @func_no: 대상 함수 번호.
 * @epf_bar: EPF 가 요청한 BAR 번호/크기/플래그.
 * @return: 항상 0.
 *
 * BAR 크기 광고의 표준 방식이다. 호스트는 BAR 에 전부 1 을 써 본 뒤 되읽어, 0 으로
 * 남은 하위 비트의 개수로 크기를 알아낸다. 그 "0 으로 남는 자리" 를 정하는 것이
 * 마스크 레지스터이고, 거기에 (크기 - 1) 을 쓴다. 크기가 2의 거듭제곱이어야 하는
 * 이유가 여기 있는데, 그 검사는 이미 EPC 코어의 pci_epc_set_bar() 가 했다.
 *
 * DBI2 창으로 쓰는 것이 마스크, DBI 창으로 쓰는 것이 BAR 본체(여기서는 종류 플래그)다.
 * 64비트 BAR 이면 크기의 상위 32비트가 다음 자리(reg + 4)의 마스크로 가고, 그쪽
 * BAR 본체에는 0 을 쓴다 — 상위 절반에는 종류 플래그가 없기 때문이다.
 * BAR_FIXED 도 이 함수를 쓰는데, 고정 BAR 은 마스크의 해당 비트가 어차피 읽기 전용이라
 * 써도 무해하고 활성화 비트만 서면 되기 때문이다(호출자의 fallthrough 주석 참고).
 *
 * 실행 컨텍스트: epc->lock 아래의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   dw_pcie_ep_set_bar() → [이 함수] → dw_pcie_ep_writel_dbi2()/_dbi()
 */
static int dw_pcie_ep_set_bar_programmable(struct dw_pcie_ep *ep, u8 func_no,
					   struct pci_epf_bar *epf_bar)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	/* [한국어] 세울 BAR 번호 */
	enum pci_barno bar = epf_bar->barno;
	/* [한국어] EPF 가 요청한 크기 */
	size_t size = epf_bar->size;
	/* [한국어] BAR 종류 플래그 */
	int flags = epf_bar->flags;
	/* [한국어] 이 BAR 의 설정 공간 오프셋 */
	u32 reg = PCI_BASE_ADDRESS_0 + (4 * bar);

	dw_pcie_dbi_ro_wr_en(pci);

	/* [한국어] 마스크에 (크기 - 1) 의 하위 32비트를 쓴다. 이 값의 0 비트 개수가 곧 호스트에게 보이는 크기다 */
	dw_pcie_ep_writel_dbi2(ep, func_no, reg, lower_32_bits(size - 1));
	/* [한국어] BAR 본체에는 종류 플래그를 쓴다. 주소 비트는 호스트가 나중에 채운다 */
	dw_pcie_ep_writel_dbi(ep, func_no, reg, flags);

	/* [한국어] 64비트 BAR 이면 크기의 상위 절반이 다음 자리로 간다 */
	if (flags & PCI_BASE_ADDRESS_MEM_TYPE_64) {
		/* [한국어] 상위 32비트 마스크. 4GB 를 넘는 BAR 에서만 0 이 아닌 값이 된다 */
		dw_pcie_ep_writel_dbi2(ep, func_no, reg + 4, upper_32_bits(size - 1));
		/* [한국어] 상위 절반 BAR 본체는 0 — 그 자리에는 종류 플래그가 없다 */
		dw_pcie_ep_writel_dbi(ep, func_no, reg + 4, 0);
	}

	dw_pcie_dbi_ro_wr_dis(pci);

	return 0;
}

/* [한국어]
 * dw_pcie_ep_get_bar_type - 이 BAR 이 어떤 종류인지 SoC 능력표에서 읽는다
 *
 * @ep: DWC 엔드포인트 인스턴스.
 * @bar: 알고 싶은 BAR 번호.
 * @return: BAR_PROGRAMMABLE / BAR_FIXED / BAR_RESIZABLE / BAR_RESERVED 중 하나.
 *
 * BAR 의 성격은 IP 설정과 SoC 배선에 달려 있어 이 파일이 알 수 없다. 그래서 글루
 * 드라이버가 채운 pci_epc_features 표를 본다. get_features 훅 자체가 없는 오래된
 * 글루라면 가장 흔한 형태인 BAR_PROGRAMMABLE 을 가정한다 — 이 기본값 덕에 훅 없는
 * 드라이버도 set_bar 가 동작한다.
 *
 * 두 곳에서 쓰인다. set_bar 는 이 값으로 마스크 기록 방식을 고르고,
 * disable_bars 는 BAR_RESERVED 인 BAR 을 초기화에서 제외하는 데 쓴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   dw_pcie_ep_set_bar() / dw_pcie_ep_disable_bars() → [이 함수] → ep->ops->get_features()
 */
static enum pci_epc_bar_type dw_pcie_ep_get_bar_type(struct dw_pcie_ep *ep,
						     enum pci_barno bar)
{
	const struct pci_epc_features *epc_features;

	/* [한국어] 글루가 능력표를 제공하지 않는 경우 */
	if (!ep->ops->get_features)
		return BAR_PROGRAMMABLE;

	/* [한국어] SoC 글루가 채운 능력표를 가져온다. BAR 별 종류가 여기 들어 있다 */
	epc_features = ep->ops->get_features(ep);

	return epc_features->bar[bar].type;
}

/* [한국어]
 * dw_pcie_ep_set_bar - BAR 하나를 세우고 인바운드 경로를 연결한다(EPC set_bar 콜백)
 *
 * @epc: EPC 컨트롤러 객체.
 * @func_no: 대상 함수 번호.
 * @vfunc_no: 가상 함수 번호. 쓰지 않는다.
 * @epf_bar: EPF 가 채운 요청 — barno, size, flags, phys_addr, 그리고 선택적 submap 배열.
 * @return: 0 성공, -EINVAL 이면 DWC 제약 위반이거나 잘못된 재호출, 그 외 하위 오류.
 *
 * 이 파일의 중심 함수다. 하는 일은 두 겹이다. 첫째, BAR 레지스터 쪽에 크기와 종류를
 * 새겨 호스트가 열거할 때 이 BAR 이 보이게 만든다. 둘째, 그 BAR 로 들어온 트랜잭션이
 * EPF 의 버퍼로 떨어지도록 인바운드 iATU 를 건다. 둘 중 하나만 되면 아무 소용이 없다.
 *
 * EPC 코어가 이미 크기가 2의 거듭제곱인지, BAR_FIXED 면 고정 크기와 맞는지,
 * BAR_RESIZABLE 이면 1MB~128TB 인지, BAR5 에 64비트를 요청하지 않았는지를 걸렀으므로
 * 여기서는 DWC 고유 제약만 본다 — 이 IP 는 BAR 쌍을 겹쳐 64비트 BAR 을 만들 수 없어서
 * 64비트 요청은 짝수 BAR 에만 허용한다.
 *
 * 두 번째 호출(재설정) 처리가 이 함수의 까다로운 부분이다. 어떤 EPF 는 clear_bar 없이
 * set_bar 를 다시 부르는데, 그때 BAR 레지스터를 다시 쓰면 호스트가 배정해 둔 주소가
 * 지워진다. 그래서 이미 세워진 BAR 이면 크기·플래그가 그대로인지만 확인하고,
 * 기존 매핑을 걷은 뒤 config_atu 로 건너뛰어 iATU 만 다시 건다. 반대로 처음 세우는
 * BAR 에는 submap 을 허용하지 않는데, submap 매핑은 호스트가 배정한 주소를 되읽어야
 * 성립하므로 "이미 한 번 세워 열거가 끝난 BAR" 에만 의미가 있기 때문이다.
 *
 * 실행 컨텍스트: epc->lock 아래의 프로세스 컨텍스트.
 * 에러 경로: iATU 프로그래밍이 실패하면 epf_bar 포인터를 기록하지 않고 오류를 올린다.
 *
 * 호출 체인:
 *   EPF(pci-epf-test 등) → pci_epc_set_bar() → [이 함수]
 *     → dw_pcie_ep_set_bar_programmable()/_resizable(),
 *       dw_pcie_ep_ib_atu_bar()/dw_pcie_ep_ib_atu_addr()
 */
static int dw_pcie_ep_set_bar(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
			      struct pci_epf_bar *epf_bar)
{
	struct dw_pcie_ep *ep = epc_get_drvdata(epc);
	/* [한국어] 오류 로그에 쓸 dw_pcie */
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	/* [한국어] 이 함수 번호의 상태 구조체. epf_bar[] 로 '이미 세워진 BAR 인지' 를 판별한다 */
	struct dw_pcie_ep_func *ep_func = dw_pcie_ep_get_func_from_ep(ep, func_no);
	/* [한국어] 세울 BAR 번호 */
	enum pci_barno bar = epf_bar->barno;
	/* [한국어] 요청 크기 */
	size_t size = epf_bar->size;
	/* [한국어] 이 BAR 의 종류(FIXED/PROGRAMMABLE/RESIZABLE/RESERVED) */
	enum pci_epc_bar_type bar_type;
	/* [한국어] 요청 플래그 */
	int flags = epf_bar->flags;
	/* [한국어] ret 는 오류 코드, type 은 iATU 에 넘길 트랜잭션 종류 */
	int ret, type;

	/* [한국어] 등록되지 않은 함수 번호 */
	if (!ep_func)
		return -EINVAL;

	/*
	 * DWC does not allow BAR pairs to overlap, e.g. you cannot combine BARs
	 * 1 and 2 to form a 64-bit BAR.
	 */
	if ((flags & PCI_BASE_ADDRESS_MEM_TYPE_64) && (bar & 1))
		return -EINVAL;

	/*
	 * Certain EPF drivers dynamically change the physical address of a BAR
	 * (i.e. they call set_bar() twice, without ever calling clear_bar(), as
	 * calling clear_bar() would clear the BAR's PCI address assigned by the
	 * host).
	 */
	if (ep_func->epf_bar[bar]) {
		/*
		 * We can only dynamically change a BAR if the new BAR size and
		 * BAR flags do not differ from the existing configuration.
		 *
		 * Note: this safety check only works when the caller uses
		 * a new struct pci_epf_bar in the second set_bar() call.
		 * If the same instance is updated in place and passed in,
		 * we cannot reliably detect invalid barno/size/flags
		 * changes here.
		 */
		if (ep_func->epf_bar[bar]->barno != bar ||
		    ep_func->epf_bar[bar]->size != size ||
		    ep_func->epf_bar[bar]->flags != flags)
			return -EINVAL;

		/*
		 * When dynamically changing a BAR, tear down any existing
		 * mappings before re-programming. This is redundant when
		 * both the old and new mappings are BAR Match Mode, but
		 * required to handle in-place updates and match-mode
		 * changes reliably.
		 */
		dw_pcie_ep_clear_ib_maps(ep, func_no, bar);

		/*
		 * When dynamically changing a BAR, skip writing the BAR reg, as
		 * that would clear the BAR's PCI address assigned by the host.
		 */
		goto config_atu;
	} else {
		/*
		 * Subrange mapping is an update-only operation.  The BAR
		 * must have been configured once without submaps so that
		 * subsequent set_bar() calls can update inbound mappings
		 * without touching the BAR register (and clobbering the
		 * host-assigned address).
		 */
		if (epf_bar->num_submap)
			return -EINVAL;
	}

	/* [한국어] 글루 능력표에서 이 BAR 의 종류를 읽는다. 종류에 따라 마스크 기록 방식이 다르다 */
	bar_type = dw_pcie_ep_get_bar_type(ep, bar);
	/* [한국어] 종류별로 갈라 처리 */
	switch (bar_type) {
	case BAR_FIXED:
		/*
		 * There is no need to write a BAR mask for a fixed BAR (except
		 * to write 1 to the LSB of the BAR mask register, to enable the
		 * BAR). Write the BAR mask regardless. (The fixed bits in the
		 * BAR mask register will be read-only anyway.)
		 */
		fallthrough;
	/* [한국어] 프로그래머블 BAR — 마스크에 크기를 직접 쓰는 표준 방식. FIXED 도 fallthrough 로 여기 온다 */
	case BAR_PROGRAMMABLE:
		ret = dw_pcie_ep_set_bar_programmable(ep, func_no, epf_bar);
		break;
	/* [한국어] Resizable BAR — 마스크 대신 CAP 에 지원 크기를 광고하는 방식 */
	case BAR_RESIZABLE:
		ret = dw_pcie_ep_set_bar_resizable(ep, func_no, epf_bar);
		break;
	default:
		ret = -EINVAL;
		/* [한국어] BAR_RESERVED 등 세울 수 없는 종류 */
		dev_err(pci->dev, "Invalid BAR type\n");
		break;
	}

	/* [한국어] BAR 레지스터 설정이 실패했으면 iATU 는 걸지 않는다 */
	if (ret)
		return ret;

config_atu:
	if (!(flags & PCI_BASE_ADDRESS_SPACE))
		/* [한국어] 메모리 BAR 이면 iATU 도 메모리 트랜잭션으로 매칭해야 한다 */
		type = PCIE_ATU_TYPE_MEM;
	else
		/* [한국어] I/O BAR 이면 I/O 트랜잭션으로 매칭 */
		type = PCIE_ATU_TYPE_IO;

	/* [한국어] 조각 요청이 있으면 Address Match Mode */
	if (epf_bar->num_submap)
		/* [한국어] BAR 을 조각내 각각 다른 물리주소에 붙인다. 호스트 배정 주소를 되읽어야 하므로 재호출 경로에서만 온다 */
		ret = dw_pcie_ep_ib_atu_addr(ep, func_no, type, epf_bar);
	else
		/* [한국어] 조각이 없으면 BAR Match Mode — 주소를 몰라도 되는 기본 방식 */
		ret = dw_pcie_ep_ib_atu_bar(ep, func_no, type,
					    epf_bar->phys_addr, bar, size);

	/* [한국어] iATU 를 못 걸었으면 실패. BAR 레지스터는 이미 써졌지만 EPC 코어가 오류를 EPF 에 올린다 */
	if (ret)
		return ret;

	/* [한국어] 성공 표시. 이 포인터의 존재가 '이 BAR 은 이미 세워졌다' 는 표시이자, 다음 set_bar 의 검증 기준이 된다 */
	ep_func->epf_bar[bar] = epf_bar;

	return 0;
}

/* [한국어]
 * dw_pcie_find_index - 로컬 물리주소로 그 주소를 쓰는 아웃바운드 창 번호를 되찾는다
 *
 * @ep: DWC 엔드포인트 인스턴스.
 * @addr: 찾을 부모 버스 기준 주소(이미 parent_bus_offset 을 뺀 값).
 * @atu_index: 찾은 창 번호를 담아 돌려줄 곳.
 * @return: 0 이면 찾음, -EINVAL 이면 그런 창이 없음.
 *
 * unmap_addr 은 주소만 받는다. 그런데 창을 끄려면 인덱스가 필요하다. 그래서
 * 창을 걸 때 outbound_addr[index] 에 적어 둔 주소를 여기서 선형 탐색으로 되짚는다.
 * 창 개수가 많아야 수십 개라 해시나 트리를 둘 이유가 없다.
 *
 * for_each_set_bit 로 "실제로 쓰이는 창" 만 도는 것이 중요하다. 쓰이지 않는 자리의
 * outbound_addr 은 0 이거나 예전 값이 남아 있을 수 있어, 전체를 돌면 주소 0 을
 * 찾을 때 오탐이 난다.
 *
 * 실행 컨텍스트: epc->lock 아래의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   dw_pcie_ep_unmap_addr() → [이 함수] → for_each_set_bit()
 */
static int dw_pcie_find_index(struct dw_pcie_ep *ep, phys_addr_t addr,
			      u32 *atu_index)
{
	u32 index;
	/* [한국어] 창 개수를 얻으려고 dw_pcie 를 되찾는다 */
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);

	/* [한국어] 실제로 쓰이는 창만 돈다. 빈 자리의 outbound_addr 은 예전 값이 남아 있어 오탐을 부른다 */
	for_each_set_bit(index, ep->ob_window_map, pci->num_ob_windows) {
		/* [한국어] 기록해 둔 주소와 다르면 다음 창으로 */
		if (ep->outbound_addr[index] != addr)
			continue;
		*atu_index = index;
		return 0;
	}

	return -EINVAL;
}

/* [한국어]
 * dw_pcie_ep_align_addr - iATU 정렬에 맞게 주소를 내리고 그 보정값을 돌려준다
 *
 * @epc: EPC 컨트롤러 객체.
 * @pci_addr: 실제로 접근하고 싶은 호스트 쪽 PCI 주소.
 * @pci_size: [in] 필요한 크기, [out] 정렬 보정과 페이지 반올림까지 반영한 크기.
 * @offset: [out] 정렬로 깎여 나간 만큼. 호출자가 창 안에서 이만큼 더 가야 원래 주소다.
 * @return: 정렬된 시작 주소. 이 값을 창의 target 으로 쓴다.
 *
 * iATU 는 아무 주소에나 창을 걸 수 없고 컨트롤러가 정한 단위(pci->region_align)에
 * 맞춰야 한다. 그런데 MSI 메시지 주소처럼 호스트가 정해 준 주소는 그 단위에 맞을
 * 이유가 없다. 그래서 주소를 아래로 내려 창을 걸고, 실제 쓰기는 창 시작에서
 * offset 만큼 떨어진 곳에 하도록 나눈다.
 *
 * 크기는 두 번 늘어난다. 먼저 앞쪽으로 내려간 만큼(ofst)을 더해야 원래 끝까지 덮이고,
 * 그다음 EPC 메모리 할당자의 페이지 단위로 올림한다 — 창에 붙일 로컬 주소가 그
 * 단위로 잡혀 있기 때문이다. EPC 규약상 이 콜백은 map_addr 앞에 호출된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 이 파일 안에서는 raise_msi/msix 경로가 직접 부른다.
 *
 * 호출 체인:
 *   pci_epc_mem_map() 또는 dw_pcie_ep_raise_msi_irq() → [이 함수]
 */
static u64 dw_pcie_ep_align_addr(struct pci_epc *epc, u64 pci_addr,
				 size_t *pci_size, size_t *offset)
{
	struct dw_pcie_ep *ep = epc_get_drvdata(epc);
	/* [한국어] 정렬 단위를 얻으려고 dw_pcie 를 되찾는다 */
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	/* [한국어] 정렬 단위가 2의 거듭제곱이므로 (단위 - 1) 이 곧 '깎아낼 하위 비트' 마스크가 된다 */
	u64 mask = pci->region_align - 1;
	/* [한국어] 정렬로 깎여 나갈 만큼. 창 시작에서 이만큼 떨어진 곳이 원래 주소다 */
	size_t ofst = pci_addr & mask;

	*pci_size = ALIGN(ofst + *pci_size, epc->mem->window.page_size);
	*offset = ofst;

	return pci_addr & ~mask;
}

/* [한국어]
 * dw_pcie_ep_unmap_addr - 아웃바운드 창 하나를 끄고 비트맵에 반납한다
 *
 * @epc: EPC 컨트롤러 객체.
 * @func_no: 함수 번호. 아웃바운드 창은 함수별로 나뉘지 않아 쓰이지 않는다.
 * @vfunc_no: 가상 함수 번호. 쓰지 않는다.
 * @addr: 끌 창의 로컬 물리주소(CPU 가 보는 주소).
 * @return: 없음.
 *
 * 호출자가 주는 addr 은 CPU 관점의 물리주소인데, 창에는 부모 버스 관점의 주소가
 * 적혀 있다. 두 관점이 다른 SoC 가 있어서 pci->parent_bus_offset 을 빼고 찾는다.
 * 이 보정은 map_addr 에서 더한 것이 아니라 뺀 것과 정확히 짝을 이룬다.
 *
 * 못 찾으면 조용히 돌아간다 — EPC 규약상 unmap 은 실패를 알리지 않고, 이미 풀린
 * 창을 다시 푸는 것은 무해하기 때문이다. 찾았으면 기록을 지우고 창을 끈 뒤
 * 비트맵 비트를 내려 다른 매핑이 그 창을 쓸 수 있게 한다.
 *
 * 실행 컨텍스트: epc->lock 아래의 프로세스 컨텍스트. 단, dw_pcie_ep_stop() 과
 * raise_msi 경로에서도 불리므로 EPC 콜백 밖에서 호출될 수 있다.
 *
 * 호출 체인:
 *   pci_epc_unmap_addr() / dw_pcie_ep_stop() / dw_pcie_ep_raise_msi_irq()
 *     → [이 함수] → dw_pcie_find_index(), dw_pcie_disable_atu()
 */
static void dw_pcie_ep_unmap_addr(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
				  phys_addr_t addr)
{
	int ret;
	/* [한국어] 찾은 창 번호를 받을 변수 */
	u32 atu_index;
	/* [한국어] EPC 객체에 걸어 둔 dw_pcie_ep 를 되찾는다 */
	struct dw_pcie_ep *ep = epc_get_drvdata(epc);
	/* [한국어] parent_bus_offset 과 iATU 함수를 얻으려고 dw_pcie 를 되찾는다 */
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);

	/* [한국어] CPU 물리주소를 부모 버스 주소로 바꿔 창을 찾는다. map 때 뺀 것과 정확히 같은 보정이다 */
	ret = dw_pcie_find_index(ep, addr - pci->parent_bus_offset,
				 &atu_index);
	/* [한국어] 그런 창이 없으면 조용히 돌아간다. 이미 풀린 창을 다시 푸는 것은 무해하다 */
	if (ret < 0)
		return;

	/* [한국어] 역인덱스 기록을 지운다 */
	ep->outbound_addr[atu_index] = 0;
	dw_pcie_disable_atu(pci, PCIE_ATU_REGION_DIR_OB, atu_index);
	clear_bit(atu_index, ep->ob_window_map);
}

/* [한국어]
 * dw_pcie_ep_map_addr - 로컬 물리주소를 호스트 PCI 주소에 연결한다(아웃바운드)
 *
 * @epc: EPC 컨트롤러 객체.
 * @func_no: 이 창을 쓸 함수 번호. TLP 의 requester ID 에 반영된다.
 * @vfunc_no: 가상 함수 번호. 쓰지 않는다.
 * @addr: 엔드포인트 로컬 물리주소. 여기에 쓰면 호스트로 나간다.
 * @pci_addr: 그 쓰기가 도달할 호스트 쪽 PCI 주소.
 * @size: 매핑 크기.
 * @return: 0 성공, 실패 시 하위 오류 코드.
 *
 * 엔드포인트가 호스트 메모리에 접근하는 유일한 통로다. EPF 가 호스트 버퍼를 읽고
 * 쓰는 것도, 이 파일이 MSI 를 쏘는 것도 결국 이 창을 통한 메모리 쓰기다.
 * addr 에서 parent_bus_offset 을 빼는 것은, 창에 적어야 하는 값이 CPU 물리주소가
 * 아니라 컨트롤러가 붙어 있는 부모 버스 관점의 주소이기 때문이다.
 *
 * atu 구조체를 { 0 } 으로 초기화하고 필요한 필드만 채우는 이유는, 나머지 필드
 * (routing, code 등 TLP 속성)를 기본값 0 으로 두면 일반 메모리 트랜잭션이 되기 때문이다.
 *
 * 실행 컨텍스트: epc->lock 아래이거나(EPC 콜백), 인터럽트 발사 경로(raise_msi).
 * 에러 경로: 창이 없거나 프로그래밍이 실패하면 로그를 남기고 그대로 올린다.
 *
 * 호출 체인:
 *   pci_epc_map_addr() / dw_pcie_ep_raise_msi_irq() / _msix_irq()
 *     → [이 함수] → dw_pcie_ep_outbound_atu() → dw_pcie_prog_outbound_atu()
 */
static int dw_pcie_ep_map_addr(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
			       phys_addr_t addr, u64 pci_addr, size_t size)
{
	int ret;
	/* [한국어] EPC 객체에 걸어 둔 dw_pcie_ep 를 되찾는다 */
	struct dw_pcie_ep *ep = epc_get_drvdata(epc);
	/* [한국어] parent_bus_offset 과 오류 로그용 dev 를 얻으려고 dw_pcie 를 되찾는다 */
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	/* [한국어] 창 설정을 0 으로 초기화. 채우지 않은 필드(routing, code 등 TLP 속성)가 0 이면 일반 메모리 트랜잭션이 된다 */
	struct dw_pcie_ob_atu_cfg atu = { 0 };

	/* [한국어] 이 창으로 나가는 TLP 의 requester 를 몇 번 함수로 볼지 */
	atu.func_no = func_no;
	/* [한국어] 메모리 트랜잭션. EPF 의 호스트 접근도 MSI 발사도 모두 메모리 쓰기다 */
	atu.type = PCIE_ATU_TYPE_MEM;
	/* [한국어] 창이 알아볼 로컬 범위(base). CPU 물리주소에서 부모 버스 오프셋을 빼야 컨트롤러가 보는 주소가 된다 */
	atu.parent_bus_addr = addr - pci->parent_bus_offset;
	/* [한국어] 그 접근이 향할 호스트 쪽 주소(target) */
	atu.pci_addr = pci_addr;
	/* [한국어] 창 크기 */
	atu.size = size;
	/* [한국어] 빈 창을 찾아 실제로 프로그래밍한다 */
	ret = dw_pcie_ep_outbound_atu(ep, &atu);
	/* [한국어] 창 고갈이거나 프로그래밍 실패 */
	if (ret) {
		/* [한국어] 어느 쪽이든 이 주소로는 호스트에 닿을 수 없다 */
		dev_err(pci->dev, "Failed to enable address\n");
		return ret;
	}

	return 0;
}

/* [한국어]
 * dw_pcie_ep_get_msi - 호스트가 이 함수에 몇 개의 MSI 를 허락했는지 되읽는다
 *
 * @epc: EPC 컨트롤러 객체.
 * @func_no: 대상 함수 번호.
 * @vfunc_no: 가상 함수 번호. 쓰지 않는다.
 * @return: 호스트가 할당한 인터럽트 개수(1, 2, 4 … 32). MSI 가 꺼져 있거나 능력이
 *          없으면 -EINVAL.
 *
 * MSI 능력 구조체의 Message Control 워드는 엔드포인트가 쓰는 필드와 호스트가 쓰는
 * 필드를 함께 담는다. 우리가 "최대 몇 개까지 쓸 수 있다" 고 광고하는 자리(QMASK)와
 * 호스트가 "그중 몇 개를 준다" 고 정해 주는 자리(QSIZE)가 그것이다. 이 함수는 호스트가
 * 정한 QSIZE 를 읽는다. 즉 여기서 나오는 값은 우리 요청이 아니라 호스트의 응답이다.
 *
 * ENABLE 비트가 꺼져 있으면 호스트가 아직 MSI 를 켜지 않은 것이므로 -EINVAL 로 거절한다.
 * QSIZE 는 개수가 아니라 2의 지수라, 1 << val 로 되돌린다. EPF 는 이 값을 보고
 * 자기가 쓸 벡터 수를 정한다.
 *
 * 실행 컨텍스트: epc->lock 아래의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   EPF → pci_epc_get_msi() → [이 함수] → dw_pcie_ep_readw_dbi()
 */
static int dw_pcie_ep_get_msi(struct pci_epc *epc, u8 func_no, u8 vfunc_no)
{
	struct dw_pcie_ep *ep = epc_get_drvdata(epc);
	/* [한국어] 이 함수의 MSI 능력 오프셋을 담고 있는 상태 구조체 */
	struct dw_pcie_ep_func *ep_func;
	/* [한국어] val 은 읽어 온 레지스터 값, reg 는 그 오프셋 */
	u32 val, reg;

	/* [한국어] 함수 번호로 상태를 찾는다 */
	ep_func = dw_pcie_ep_get_func_from_ep(ep, func_no);
	/* [한국어] 등록되지 않은 함수이거나 이 함수에 MSI 능력 구조체가 없으면 답할 것이 없다 */
	if (!ep_func || !ep_func->msi_cap)
		return -EINVAL;

	/* [한국어] MSI 능력의 Message Control 워드 오프셋 */
	reg = ep_func->msi_cap + PCI_MSI_FLAGS;
	/* [한국어] 2바이트 읽기 — Message Control 은 word 필드다 */
	val = dw_pcie_ep_readw_dbi(ep, func_no, reg);
	/* [한국어] ENABLE 은 호스트가 세우는 비트다. 꺼져 있으면 호스트가 아직 MSI 를 켜지 않은 것 */
	if (!(val & PCI_MSI_FLAGS_ENABLE))
		return -EINVAL;

	/* [한국어] QSIZE = 호스트가 '이만큼 준다' 고 정한 값. 우리가 쓴 QMASK 가 아니라 호스트의 응답이다 */
	val = FIELD_GET(PCI_MSI_FLAGS_QSIZE, val);

	return 1 << val;
}

/* [한국어]
 * dw_pcie_ep_set_msi - 이 함수가 지원하는 MSI 개수를 호스트에게 광고한다
 *
 * @epc: EPC 컨트롤러 객체.
 * @func_no: 대상 함수 번호.
 * @vfunc_no: 가상 함수 번호. 쓰지 않는다.
 * @nr_irqs: EPF 가 원하는 인터럽트 개수.
 * @return: 0 성공, -EINVAL 이면 함수나 MSI 능력이 없음.
 *
 * get_msi 와 반대 방향이다. Multiple Message Capable(QMASK) 필드에 "우리는 최대
 * 이만큼 쓸 수 있다" 를 적는다. 이 필드도 개수가 아니라 2의 지수로 인코딩되므로
 * order_base_2() 로 변환한다 — 예컨대 5개를 원하면 8개(지수 3)로 올려 광고된다.
 * 호스트는 이 광고를 보고 자기 사정에 맞춰 QSIZE 를 되쓴다.
 *
 * 이 필드는 호스트 관점에서 읽기 전용이라, 쓰기 전에 dw_pcie_dbi_ro_wr_en() 으로
 * 잠금을 풀어야 한다. 읽고-고치고-쓰기 방식으로 QMASK 만 갈아 끼우는 것은 같은
 * 워드에 있는 ENABLE 이나 64BIT 같은 다른 비트를 뭉개지 않기 위해서다.
 *
 * 실행 컨텍스트: epc->lock 아래의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   EPF bind → pci_epc_set_msi() → [이 함수] → dw_pcie_ep_writew_dbi()
 */
static int dw_pcie_ep_set_msi(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
			      u8 nr_irqs)
{
	struct dw_pcie_ep *ep = epc_get_drvdata(epc);
	/* [한국어] ro_wr_en/dis 에 넘길 dw_pcie */
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	/* [한국어] MSI 능력 오프셋을 담은 상태 구조체 */
	struct dw_pcie_ep_func *ep_func;
	/* [한국어] 개수를 2의 지수로 바꾼다. 5를 요청하면 3(=8개)이 되어 올림 광고된다 */
	u8 mmc = order_base_2(nr_irqs);
	/* [한국어] val 은 고쳐 쓸 레지스터 값, reg 는 오프셋 */
	u32 val, reg;

	/* [한국어] 함수 번호로 상태를 찾는다 */
	ep_func = dw_pcie_ep_get_func_from_ep(ep, func_no);
	/* [한국어] 등록되지 않은 함수이거나 MSI 능력이 없음 */
	if (!ep_func || !ep_func->msi_cap)
		return -EINVAL;

	/* [한국어] Message Control 워드 오프셋 */
	reg = ep_func->msi_cap + PCI_MSI_FLAGS;
	/* [한국어] 읽고-고치고-쓰기의 '읽기'. 같은 워드의 ENABLE 등 다른 비트를 지키기 위함 */
	val = dw_pcie_ep_readw_dbi(ep, func_no, reg);
	/* [한국어] QMASK 필드만 비운다 */
	val &= ~PCI_MSI_FLAGS_QMASK;
	/* [한국어] 거기에 새 지수를 끼워 넣는다. QMASK = 우리가 광고하는 '최대 이만큼 쓸 수 있다' */
	val |= FIELD_PREP(PCI_MSI_FLAGS_QMASK, mmc);
	dw_pcie_dbi_ro_wr_en(pci);
	/* [한국어] 되쓴다. 앞줄에서 ro_wr_en 을 켜 두었기에 호스트 관점 읽기 전용인 이 필드에 쓸 수 있다 */
	dw_pcie_ep_writew_dbi(ep, func_no, reg, val);
	dw_pcie_dbi_ro_wr_dis(pci);

	return 0;
}

/* [한국어]
 * dw_pcie_ep_get_msix - 호스트가 허락한 MSI-X 벡터 개수를 되읽는다
 *
 * @epc: EPC 컨트롤러 객체.
 * @func_no: 대상 함수 번호.
 * @vfunc_no: 가상 함수 번호. 쓰지 않는다.
 * @return: 벡터 개수. MSI-X 가 꺼져 있거나 능력이 없으면 -EINVAL.
 *
 * MSI 와 달리 MSI-X 의 테이블 크기 필드(QSIZE)는 2의 지수가 아니라 "개수 - 1" 을
 * 그대로 담는다. 그래서 마스크로 뽑아낸 값에 1 을 더하면 실제 개수다. 이 인코딩
 * 차이가 MSI 경로와 MSI-X 경로에서 계산이 다른 이유다.
 *
 * MSI-X 는 벡터가 최대 2048개까지 갈 수 있어 능력 구조체 안에 주소/데이터를 담지
 * 못하고, 대신 BAR 안의 테이블에 둔다. 그 테이블 위치는 set_msix 가 정한다.
 * ENABLE 비트가 꺼져 있으면 아직 호스트가 MSI-X 를 켜지 않은 것이다.
 *
 * 실행 컨텍스트: epc->lock 아래의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   EPF → pci_epc_get_msix() → [이 함수] → dw_pcie_ep_readw_dbi()
 */
static int dw_pcie_ep_get_msix(struct pci_epc *epc, u8 func_no, u8 vfunc_no)
{
	struct dw_pcie_ep *ep = epc_get_drvdata(epc);
	/* [한국어] MSI-X 능력 오프셋을 담은 상태 구조체 */
	struct dw_pcie_ep_func *ep_func;
	/* [한국어] val 은 읽은 값, reg 는 오프셋 */
	u32 val, reg;

	/* [한국어] 함수 번호로 상태를 찾는다 */
	ep_func = dw_pcie_ep_get_func_from_ep(ep, func_no);
	/* [한국어] 등록되지 않은 함수이거나 MSI-X 능력이 없음 */
	if (!ep_func || !ep_func->msix_cap)
		return -EINVAL;

	/* [한국어] MSI-X 능력의 Message Control 워드 오프셋 */
	reg = ep_func->msix_cap + PCI_MSIX_FLAGS;
	/* [한국어] 2바이트 읽기 */
	val = dw_pcie_ep_readw_dbi(ep, func_no, reg);
	/* [한국어] 호스트가 MSI-X 를 켰는지 */
	if (!(val & PCI_MSIX_FLAGS_ENABLE))
		return -EINVAL;

	/* [한국어] 테이블 크기 필드를 뽑는다. MSI 와 달리 2의 지수가 아니라 '개수 - 1' 이 그대로 들어 있다 */
	val &= PCI_MSIX_FLAGS_QSIZE;

	return val + 1;
}

/* [한국어]
 * dw_pcie_ep_set_msix - MSI-X 벡터 수와 테이블/PBA 위치를 설정 공간에 새긴다
 *
 * @epc: EPC 컨트롤러 객체.
 * @func_no: 대상 함수 번호.
 * @vfunc_no: 가상 함수 번호. 쓰지 않는다.
 * @nr_irqs: 광고할 벡터 개수.
 * @bir: 테이블과 PBA 를 담을 BAR 번호(BAR Indicator Register).
 * @offset: 그 BAR 안에서 테이블이 시작하는 바이트 오프셋.
 * @return: 0 성공, -EINVAL 이면 함수나 MSI-X 능력이 없음.
 *
 * MSI-X 능력 구조체는 벡터 정보를 직접 담지 않고 "어느 BAR 의 어디를 보라" 만 담는다.
 * 그 지시가 두 개다 — 벡터별 주소/데이터가 놓인 Table, 그리고 대기 중인 인터럽트를
 * 표시하는 Pending Bit Array. 두 레지스터 모두 하위 3비트가 BAR 번호이고 나머지
 * 상위 비트가 오프셋이라, offset | bir 한 번의 OR 로 조립할 수 있다(오프셋이 8바이트
 * 정렬이어야 성립한다).
 *
 * PBA 오프셋을 테이블 바로 뒤로 잡는 것이 이 구현의 배치 규약이다 —
 * offset + nr_irqs * PCI_MSIX_ENTRY_SIZE(엔트리 16바이트). 즉 호출자는 그만큼의
 * 공간을 BAR 안에 비워 두어야 한다. QSIZE 는 "개수 - 1" 인코딩이라 nr_irqs - 1 을 쓴다.
 * 세 레지스터 전부 호스트 관점 읽기 전용이므로 ro_wr_en/dis 로 감싼다.
 *
 * 실행 컨텍스트: epc->lock 아래의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   EPF bind → pci_epc_set_msix() → [이 함수] → dw_pcie_ep_writew_dbi()/_writel_dbi()
 */
static int dw_pcie_ep_set_msix(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
			       u16 nr_irqs, enum pci_barno bir, u32 offset)
{
	struct dw_pcie_ep *ep = epc_get_drvdata(epc);
	/* [한국어] ro_wr_en/dis 에 넘길 dw_pcie */
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	/* [한국어] MSI-X 능력 오프셋을 담은 상태 구조체 */
	struct dw_pcie_ep_func *ep_func;
	/* [한국어] val 은 조립할 값, reg 는 오프셋 */
	u32 val, reg;

	/* [한국어] 함수 번호로 상태를 찾는다 */
	ep_func = dw_pcie_ep_get_func_from_ep(ep, func_no);
	/* [한국어] 등록되지 않은 함수이거나 MSI-X 능력이 없음 */
	if (!ep_func || !ep_func->msix_cap)
		return -EINVAL;

	dw_pcie_dbi_ro_wr_en(pci);

	/* [한국어] Message Control 워드 오프셋 */
	reg = ep_func->msix_cap + PCI_MSIX_FLAGS;
	/* [한국어] 읽고-고치고-쓰기의 '읽기' */
	val = dw_pcie_ep_readw_dbi(ep, func_no, reg);
	/* [한국어] 테이블 크기 필드를 비운다 */
	val &= ~PCI_MSIX_FLAGS_QSIZE;
	/* [한국어] 개수 - 1 을 넣는다. 원문 주석대로 N-1 인코딩이다 */
	val |= nr_irqs - 1; /* encoded as N-1 */
	/* [한국어] 되쓴다 */
	dw_pcie_ep_writew_dbi(ep, func_no, reg, val);

	/* [한국어] Table 오프셋 레지스터. '테이블이 어느 BAR 의 어디에 있는지' 를 담는다 */
	reg = ep_func->msix_cap + PCI_MSIX_TABLE;
	/* [한국어] 하위 3비트가 BAR 번호(BIR), 나머지가 오프셋이라 한 번의 OR 로 조립된다 — 오프셋이 8바이트 정렬이어야 성립한다 */
	val = offset | bir;
	/* [한국어] 4바이트 쓰기. 이 레지스터는 dword 필드다 */
	dw_pcie_ep_writel_dbi(ep, func_no, reg, val);

	/* [한국어] PBA(Pending Bit Array) 오프셋 레지스터. 대기 중인 인터럽트를 표시하는 비트 배열의 위치다 */
	reg = ep_func->msix_cap + PCI_MSIX_PBA;
	/* [한국어] PBA 를 테이블 바로 뒤에 둔다. 벡터 하나가 차지하는 크기가 PCI_MSIX_ENTRY_SIZE 이므로
	 * 벡터 수만큼 곱해 건너뛴다 — 호출자는 BAR 안에 이 공간을 비워 두어야 한다.
	 * 그 상수의 정의는 PCI 코어 헤더에 있고 이 트리에는 들어 있지 않아 값 자체는 확인하지 못했다 */
	val = (offset + (nr_irqs * PCI_MSIX_ENTRY_SIZE)) | bir;
	/* [한국어] 4바이트 쓰기 */
	dw_pcie_ep_writel_dbi(ep, func_no, reg, val);

	dw_pcie_dbi_ro_wr_dis(pci);

	return 0;
}

/* [한국어]
 * dw_pcie_ep_raise_irq - 인터럽트 발사 요청을 SoC 글루 훅으로 넘긴다
 *
 * @epc: EPC 컨트롤러 객체.
 * @func_no: 인터럽트를 낼 함수 번호.
 * @vfunc_no: 가상 함수 번호. 쓰지 않는다.
 * @type: PCI_IRQ_INTX / PCI_IRQ_MSI / PCI_IRQ_MSIX 중 하나.
 * @interrupt_num: 몇 번째 인터럽트인지. EPC 규약상 1-기반이다.
 * @return: 글루 훅의 반환값. 훅이 없으면 -EINVAL.
 *
 * 이 파일이 직접 처리하지 않고 글루로 넘기는 이유는, 같은 DWC IP 라도 SoC 마다
 * 지원하는 인터럽트 종류가 다르고 INTx 는 아예 SoC 전용 회로로 내는 경우가 많기
 * 때문이다. 글루의 raise_irq 훅은 보통 type 으로 갈라 이 파일의
 * dw_pcie_ep_raise_intx_irq() / _msi_irq() / _msix_irq() 를 되부른다.
 * 실제로 pcie-designware-plat.c, pcie-qcom-ep.c, pci-imx6.c, pcie-dw-rockchip.c,
 * pcie-rcar-gen4.c, pci-layerscape-ep.c 등이 그 형태다.
 *
 * interrupt_num 이 1-기반이라는 점이 중요하다. 하드웨어 레지스터는 0-기반이라
 * 아래 발사 함수들이 일관되게 1 을 뺀다.
 *
 * 실행 컨텍스트: epc->lock 아래의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   EPF → pci_epc_raise_irq() → [이 함수] → ep->ops->raise_irq()(SoC 글루)
 *     → dw_pcie_ep_raise_msi_irq() 등
 */
static int dw_pcie_ep_raise_irq(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
				unsigned int type, u16 interrupt_num)
{
	struct dw_pcie_ep *ep = epc_get_drvdata(epc);

	/* [한국어] 글루가 인터럽트 발사 훅을 제공하지 않으면 낼 방법이 없다 */
	if (!ep->ops->raise_irq)
		return -EINVAL;

	return ep->ops->raise_irq(ep, func_no, type, interrupt_num);
}

/* [한국어]
 * dw_pcie_ep_stop - 링크를 내리고 MSI 전용 아웃바운드 창을 반납한다
 *
 * @epc: EPC 컨트롤러 객체.
 * @return: 없음.
 *
 * MSI 발사 경로는 창을 한 번 잡아 두고 계속 재사용하는 최적화를 쓴다. 그래서 여기서
 * 명시적으로 걷지 않으면, 엔드포인트를 멈췄다 다시 시작할 때마다 창이 하나씩 새어
 * 결국 인바운드/아웃바운드 창이 고갈된다. 원문 주석이 정확히 그 누수를 막으려는
 * 것이라고 밝힌다.
 *
 * 순서상 창을 먼저 걷고 링크를 내린다. 링크가 살아 있는 동안 창을 걷는 것이
 * 안전한 이유는, 이 시점에서는 EPF 가 이미 인터럽트를 내지 않는 상태이기 때문이다.
 * 창을 걷은 뒤 msi_iatu_mapped 를 false 로 되돌려, 다음 MSI 때 다시 잡도록 만든다.
 *
 * 실행 컨텍스트: epc->lock 아래의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   configfs 의 start=0 → pci_epc_stop() → [이 함수]
 *     → dw_pcie_ep_unmap_addr(), dw_pcie_stop_link()
 */
static void dw_pcie_ep_stop(struct pci_epc *epc)
{
	struct dw_pcie_ep *ep = epc_get_drvdata(epc);
	/* [한국어] dw_pcie_stop_link() 에 넘길 dw_pcie */
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);

	/*
	 * Tear down the dedicated outbound window used for MSI
	 * generation. This avoids leaking an iATU window across
	 * endpoint stop/start cycles.
	 */
	if (ep->msi_iatu_mapped) {
		/* [한국어] MSI 전용으로 잡아 두었던 아웃바운드 창을 반납한다.
	 * 함수 번호에 0 을 넘겨도 되는 것은 dw_pcie_ep_unmap_addr() 이 그 인자를 쓰지 않고
	 * 주소만으로 창을 되찾기 때문이다 — 창을 잡을 때는 실제 func_no 를 넘겼지만
	 * 그 값은 창 설정에만 반영되고 되찾기에는 관여하지 않는다 */
		dw_pcie_ep_unmap_addr(epc, 0, 0, ep->msi_mem_phys);
		/* [한국어] 다음 MSI 때 다시 잡도록 표시를 내린다 */
		ep->msi_iatu_mapped = false;
	}

	dw_pcie_stop_link(pci);
}

/* [한국어]
 * dw_pcie_ep_start - 링크 트레이닝을 시작해 호스트에게 자신을 보이게 만든다
 *
 * @epc: EPC 컨트롤러 객체.
 * @return: dw_pcie_start_link() 의 결과. 0 이면 시작됨.
 *
 * 엔드포인트 설정(헤더, BAR, MSI)이 모두 끝난 뒤에 불려야 한다. 링크가 올라가면
 * 호스트가 열거를 시작하고 BAR 에 주소를 배정하므로, 그 전에 광고할 것이 다 준비되어
 * 있어야 한다. 실제 링크 시작은 SoC 마다 다른 PHY/레지스터 조작이라
 * dw_pcie_start_link() 가 글루의 ops->start_link 훅으로 넘긴다.
 *
 * 링크가 올라온 뒤의 통지는 별도로, 글루가 인터럽트를 받아 dw_pcie_ep_linkup() 을
 * 부르는 경로로 EPF 에 전달된다.
 *
 * 실행 컨텍스트: epc->lock 아래의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   configfs 의 start=1 → pci_epc_start() → [이 함수] → dw_pcie_start_link()
 */
static int dw_pcie_ep_start(struct pci_epc *epc)
{
	struct dw_pcie_ep *ep = epc_get_drvdata(epc);
	/* [한국어] dw_pcie_start_link() 에 넘길 dw_pcie */
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);

	return dw_pcie_start_link(pci);
}

/* [한국어]
 * dw_pcie_ep_get_features - 이 컨트롤러가 무엇을 지원하는지 EPF 에게 알린다
 *
 * @epc: EPC 프레임워크의 컨트롤러 객체.
 * @func_no: 대상 함수 번호. 이 구현은 무시한다(능력이 함수별로 다르지 않다).
 * @vfunc_no: 가상 함수 번호. 쓰지 않는다.
 * @return: SoC 글루가 제공하는 pci_epc_features. get_features 훅이 없으면 NULL.
 *
 * BAR 별 종류(BAR_FIXED/BAR_PROGRAMMABLE/BAR_RESIZABLE/BAR_RESERVED), 고정 크기,
 * 지원 인터럽트 종류, 정렬 제약 같은 것이 여기 담긴다. EPF 드라이버는 이 표를 보고
 * 어떤 BAR 을 어떤 크기로 요청할지 정하고, EPC 코어의 pci_epc_set_bar() 는 이 표로
 * 요청을 검증한다. 능력은 SoC 마다 다르므로 이 파일이 직접 답하지 않고 글루 훅에 넘긴다.
 *
 * NULL 을 돌려주면 EPC 코어는 "능력 정보 없음" 으로 보고 set_bar 등을 거절한다.
 * 이 파일 안에서도 dw_pcie_ep_get_bar_type() 이 같은 훅을 직접 부르는데, 그쪽은
 * 훅이 없을 때 BAR_PROGRAMMABLE 을 기본값으로 삼는다.
 *
 * 실행 컨텍스트: epc->lock 아래의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_epc_get_features() → [이 함수] → ep->ops->get_features()(SoC 글루)
 */
static const struct pci_epc_features*
dw_pcie_ep_get_features(struct pci_epc *epc, u8 func_no, u8 vfunc_no)
{
	struct dw_pcie_ep *ep = epc_get_drvdata(epc);

	/* [한국어] 글루가 능력표를 제공하지 않으면 답할 것이 없다 */
	if (!ep->ops->get_features)
		return NULL;

	/* [한국어] SoC 글루가 채운 능력표를 그대로 돌려준다 */
	return ep->ops->get_features(ep);
}

/* [한국어] 이 파일이 EPC 프레임워크에 등록하는 콜백 테이블. dw_pcie_ep_init() 이 devm_pci_epc_create() 에 넘기며,
 * 이후 pci_epc_* API 호출은 전부 여기 꽂힌 함수로 들어온다. 없는 항목(예: SR-IOV 관련)은
 * 프레임워크가 미지원으로 처리한다 */
static const struct pci_epc_ops epc_ops = {
	/* [한국어] 설정 공간 헤더 기록 — 벤더/디바이스 ID, 클래스 코드 등 '우리가 어떤 장치인가' */
	.write_header		= dw_pcie_ep_write_header,
	/* [한국어] BAR 세우기 — 크기/종류를 레지스터에 새기고 인바운드 iATU 까지 건다. 이 테이블에서 가장 복잡한 항목 */
	.set_bar		= dw_pcie_ep_set_bar,
	/* [한국어] BAR 끄기 — 레지스터를 0 으로 지우고 그 BAR 에 걸린 인바운드 창을 전부 걷는다 */
	.clear_bar		= dw_pcie_ep_clear_bar,
	/* [한국어] 주소 정렬 — iATU 정렬 단위에 맞게 주소를 내리고 보정값을 돌려준다. 프레임워크가 map_addr 앞에 부른다 */
	.align_addr		= dw_pcie_ep_align_addr,
	/* [한국어] 아웃바운드 창 걸기 — 로컬 물리주소를 호스트 PCI 주소에 연결한다 */
	.map_addr		= dw_pcie_ep_map_addr,
	/* [한국어] 아웃바운드 창 걷기 — 로컬 물리주소로 창을 되찾아 끈다 */
	.unmap_addr		= dw_pcie_ep_unmap_addr,
	/* [한국어] MSI 개수 광고 — 우리가 최대 몇 개를 쓸 수 있는지 QMASK 에 쓴다 */
	.set_msi		= dw_pcie_ep_set_msi,
	/* [한국어] MSI 개수 조회 — 호스트가 실제로 몇 개를 허락했는지 QSIZE 에서 읽는다 */
	.get_msi		= dw_pcie_ep_get_msi,
	/* [한국어] MSI-X 설정 — 벡터 수와 테이블/PBA 위치(어느 BAR 의 어느 오프셋)를 새긴다 */
	.set_msix		= dw_pcie_ep_set_msix,
	/* [한국어] MSI-X 개수 조회 — 호스트가 켠 벡터 수를 읽는다 */
	.get_msix		= dw_pcie_ep_get_msix,
	/* [한국어] 인터럽트 발사 — 종류에 따라 SoC 글루 훅으로 넘긴다 */
	.raise_irq		= dw_pcie_ep_raise_irq,
	/* [한국어] 링크 시작 — 설정이 끝난 뒤 트레이닝을 걸어 호스트에게 보이게 만든다 */
	.start			= dw_pcie_ep_start,
	/* [한국어] 링크 정지 — MSI 전용 창을 반납하고 링크를 내린다 */
	.stop			= dw_pcie_ep_stop,
	/* [한국어] 능력 조회 — BAR 별 종류와 지원 기능을 담은 표를 글루에서 받아 돌려준다 */
	.get_features		= dw_pcie_ep_get_features,
};

/**
 * dw_pcie_ep_raise_intx_irq - Raise INTx IRQ to the host
 * @ep: DWC EP device
 * @func_no: Function number of the endpoint
 *
 * Return: 0 if success, errno otherwise.
 */
/* [한국어]
 * dw_pcie_ep_raise_intx_irq - INTx 는 이 공용 계층에서 낼 수 없음을 알린다
 *
 * @ep: DWC 엔드포인트 인스턴스.
 * @func_no: 함수 번호. 쓰이지 않는다.
 * @return: 언제나 -EINVAL.
 *
 * 이름과 달리 실제로 인터럽트를 내지 않는다. 오류 로그를 남기고 -EINVAL 을 돌려주는
 * 것이 전부다. 이유는 INTx(레거시 가상 와이어 인터럽트) 어서션이 DWC 코어의 공통
 * 레지스터로 되지 않고 SoC 마다 다른 전용 회로를 거치기 때문이다.
 *
 * 그래서 이 함수는 "INTx 를 자체 구현하지 않은 글루" 의 기본 동작 역할을 한다.
 * 글루가 INTx 를 지원한다면 자기 raise_irq 훅에서 이 함수를 부르는 대신 직접
 * 처리한다. EXPORT_SYMBOL_GPL 로 내보내며, 이 트리에서는 pcie-designware-plat.c,
 * pcie-qcom-ep.c, pci-imx6.c, pcie-dw-rockchip.c, pcie-rcar-gen4.c,
 * pci-layerscape-ep.c, pcie-stm32-ep.c 가 부른다.
 *
 * 실행 컨텍스트: 글루의 raise_irq 훅 안, 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_epc_raise_irq() → 글루 raise_irq → [이 함수] → dev_err()
 */
int dw_pcie_ep_raise_intx_irq(struct dw_pcie_ep *ep, u8 func_no)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	/* [한국어] 오류 로그를 남길 디바이스 */
	struct device *dev = pci->dev;

	/* [한국어] INTx 어서션은 DWC 공통 레지스터로 되지 않는다. 그것을 지원하는 글루는 이 함수를 부르지 않고 직접 처리한다 */
	dev_err(dev, "EP cannot raise INTX IRQs\n");

	return -EINVAL;
}
EXPORT_SYMBOL_GPL(dw_pcie_ep_raise_intx_irq);

/**
 * dw_pcie_ep_raise_msi_irq - Raise MSI IRQ to the host
 * @ep: DWC EP device
 * @func_no: Function number of the endpoint
 * @interrupt_num: Interrupt number to be raised
 *
 * Return: 0 if success, errno otherwise.
 */
/* [한국어]
 * dw_pcie_ep_raise_msi_irq - MSI 메시지를 호스트 메모리에 써서 인터럽트를 낸다
 *
 * @ep: DWC 엔드포인트 인스턴스.
 * @func_no: 인터럽트를 낼 함수 번호.
 * @interrupt_num: 몇 번째 인터럽트인지. EPC 규약상 1-기반.
 * @return: 0 성공, -EINVAL 이면 MSI 능력 없음, 그 외 창 매핑 오류.
 *
 * MSI 는 별도의 인터럽트 선이 아니라 "호스트가 지정한 주소에 지정한 값을 쓰는
 * 메모리 트랜잭션" 이다. 그래서 엔드포인트가 할 일은 (1) 호스트가 MSI 능력 구조체에
 * 적어 둔 목적지 주소와 데이터를 읽고, (2) 그 주소로 나가는 아웃바운드 창을 만들고,
 * (3) 창에 대응하는 로컬 주소에 데이터를 한 번 쓰는 것이다. 이것이 PCI Local Bus
 * Specification Revision 3.0, 6.8.1 이 정한 절차이고 원문 주석도 그렇게 밝힌다.
 *
 * 주소가 64비트인지는 Message Control 의 64BIT 비트로 갈린다. 그 비트에 따라 상위
 * 주소 레지스터의 존재 여부가 달라지고, 그 결과 데이터 레지스터의 오프셋까지
 * 달라진다(PCI_MSI_DATA_32 대 PCI_MSI_DATA_64) — 능력 구조체가 가변 길이라서다.
 *
 * 이 함수의 핵심 최적화는 창을 매번 잡았다 푸는 대신 한 번 잡아 두고 재사용하는
 * 것이다. 원문 주석대로 AXI 브리지에 트랜잭션이 떠 있는 동안 iATU 레지스터를 고치는
 * 것이 규약상 지원되지 않기 때문에, 인터럽트마다 매핑/해제를 반복하는 것이 위험하다.
 * 목적지 주소나 필요한 크기가 바뀌었을 때에만 창을 다시 잡는데, 그때조차
 * "떠 있는 트랜잭션이 있는지 알 방법이 없다" 는 것을 원문이 인정하고 있다.
 * 정리는 dw_pcie_ep_stop() 이 맡는다.
 *
 * 마지막 writel 이 실제 발사다. 여러 벡터를 쓸 때 하위 비트가 벡터 번호가 되도록
 * msg_data 에 (interrupt_num - 1) 을 OR 한다 — 1-기반을 0-기반으로 낮추는 것이다.
 *
 * 실행 컨텍스트: 글루의 raise_irq 훅 안, 프로세스 컨텍스트. epc->lock 은 EPC 코어
 * 경로로 들어올 때만 잡혀 있고, ep->msi_iatu_mapped 등의 상태를 보호하는 자체 잠금은
 * 이 코드에 없다.
 *
 * 호출 체인:
 *   pci_epc_raise_irq() → 글루 raise_irq → [이 함수]
 *     → dw_pcie_ep_align_addr(), dw_pcie_ep_map_addr(), writel()
 */
int dw_pcie_ep_raise_msi_irq(struct dw_pcie_ep *ep, u8 func_no,
			     u8 interrupt_num)
{
	u32 msg_addr_lower, msg_addr_upper, reg;
	/* [한국어] MSI 능력 오프셋을 담은 상태 구조체 */
	struct dw_pcie_ep_func *ep_func;
	/* [한국어] 창을 잡고 정렬할 때 넘길 EPC 객체 */
	struct pci_epc *epc = ep->epc;
	/* [한국어] MSI 는 4바이트 쓰기 한 번이다. 이 값이 정렬 보정 뒤 창 크기 계산의 출발점이 된다 */
	size_t map_size = sizeof(u32);
	/* [한국어] 정렬로 깎여 나간 만큼. 실제 쓰기는 창 시작에서 이만큼 떨어진 곳에 한다 */
	size_t offset;
	/* [한국어] msg_ctrl 은 Message Control, msg_data 는 호스트가 정한 메시지 값 */
	u16 msg_ctrl, msg_data;
	/* [한국어] 주소가 64비트인지. 능력 구조체가 가변 길이라 이후 오프셋이 달라진다 */
	bool has_upper;
	/* [한국어] 조립한 64비트 목적지 주소 */
	u64 msg_addr;
	/* [한국어] 창 매핑 결과 */
	int ret;

	/* [한국어] 함수 번호로 상태를 찾는다 */
	ep_func = dw_pcie_ep_get_func_from_ep(ep, func_no);
	/* [한국어] 등록되지 않은 함수이거나 MSI 능력이 없으면 낼 수 없다 */
	if (!ep_func || !ep_func->msi_cap)
		return -EINVAL;

	/* Raise MSI per the PCI Local Bus Specification Revision 3.0, 6.8.1. */
	reg = ep_func->msi_cap + PCI_MSI_FLAGS;
	/* [한국어] Message Control 을 읽는다. 여기 들어 있는 정보로 아래 레지스터들의 위치가 정해진다 */
	msg_ctrl = dw_pcie_ep_readw_dbi(ep, func_no, reg);
	/* [한국어] 64BIT 비트를 bool 로 정규화. !! 는 0 아닌 값을 1 로 만든다 */
	has_upper = !!(msg_ctrl & PCI_MSI_FLAGS_64BIT);
	/* [한국어] 메시지 주소의 하위 32비트 레지스터 */
	reg = ep_func->msi_cap + PCI_MSI_ADDRESS_LO;
	/* [한국어] 호스트가 여기에 '나에게 인터럽트를 주려면 이 주소에 써라' 를 적어 두었다 */
	msg_addr_lower = dw_pcie_ep_readl_dbi(ep, func_no, reg);
	/* [한국어] 64비트 주소를 쓰는 호스트인 경우 */
	if (has_upper) {
		/* [한국어] 상위 32비트 주소 레지스터. 32비트 모드에서는 존재하지 않는다 */
		reg = ep_func->msi_cap + PCI_MSI_ADDRESS_HI;
		/* [한국어] 상위 절반 읽기 */
		msg_addr_upper = dw_pcie_ep_readl_dbi(ep, func_no, reg);
		/* [한국어] 64비트 모드에서의 데이터 레지스터 오프셋. 상위 주소가 끼어들어 32비트 모드보다 4바이트 뒤에 있다 */
		reg = ep_func->msi_cap + PCI_MSI_DATA_64;
		/* [한국어] 호스트가 정한 메시지 값. 이 값을 그 주소에 쓰면 호스트가 인터럽트로 받아들인다 */
		msg_data = dw_pcie_ep_readw_dbi(ep, func_no, reg);
	} else {
		/* [한국어] 32비트 모드에서는 상위 절반이 없다 */
		msg_addr_upper = 0;
		/* [한국어] 32비트 모드에서의 데이터 레지스터 오프셋 */
		reg = ep_func->msi_cap + PCI_MSI_DATA_32;
		/* [한국어] 메시지 값 읽기 */
		msg_data = dw_pcie_ep_readw_dbi(ep, func_no, reg);
	}
	/* [한국어] 두 조각을 합쳐 64비트 목적지 주소를 만든다. 32비트 모드면 상위가 0 이라 그대로 하위 값이 된다 */
	msg_addr = ((u64)msg_addr_upper) << 32 | msg_addr_lower;

	/* [한국어] iATU 정렬 단위에 맞게 주소를 내린다. map_size 와 offset 이 여기서 갱신된다 */
	msg_addr = dw_pcie_ep_align_addr(epc, msg_addr, &map_size, &offset);

	/*
	 * Program the outbound iATU once and keep it enabled.
	 *
	 * The spec warns that updating iATU registers while there are
	 * operations in flight on the AXI bridge interface is not
	 * supported, so we avoid reprogramming the region on every MSI,
	 * specifically unmapping immediately after writel().
	 */
	if (ep->msi_iatu_mapped && (ep->msi_msg_addr != msg_addr ||
				    ep->msi_map_size != map_size)) {
		/*
		 * The host changed the MSI target address or the required
		 * mapping size changed. Reprogramming the iATU when there are
		 * operations in flight is unsafe on this controller. However,
		 * there is no unified way to check if we have operations in
		 * flight, thus we don't know if we should WARN() or not.
		 */
		dw_pcie_ep_unmap_addr(epc, func_no, 0, ep->msi_mem_phys);
		/* [한국어] 창을 걷었으므로 다음 블록이 다시 잡도록 표시를 내린다 */
		ep->msi_iatu_mapped = false;
	}

	/* [한국어] 아직 창이 없거나 방금 걷은 경우 — 새로 잡아야 한다 */
	if (!ep->msi_iatu_mapped) {
		/* [한국어] 미리 예약해 둔 msi_mem 페이지를 목적지 주소에 연결한다. 이 창은 stop 까지 유지된다 */
		ret = dw_pcie_ep_map_addr(epc, func_no, 0,
					  ep->msi_mem_phys, msg_addr,
					  map_size);
		/* [한국어] 창 고갈 등으로 실패하면 인터럽트를 낼 수 없다 */
		if (ret)
			return ret;

		/* [한국어] 창을 잡았다고 기록. 다음 인터럽트부터는 이 블록을 건너뛴다 */
		ep->msi_iatu_mapped = true;
		/* [한국어] 어떤 주소에 걸어 두었는지 — 호스트가 주소를 바꾸면 이 값과 달라져 재프로그래밍 조건이 된다 */
		ep->msi_msg_addr = msg_addr;
		/* [한국어] 어떤 크기로 걸어 두었는지 — 마찬가지로 재프로그래밍 판단에 쓴다 */
		ep->msi_map_size = map_size;
	}

	/* [한국어] 실제 발사. 창을 통해 나가는 이 4바이트 쓰기가 곧 호스트가 보는 MSI 다.
	 * 하위 비트에 (interrupt_num - 1) 을 OR 하는 것은 EPC 규약의 1-기반 번호를 0-기반 벡터로 낮추는 것이다 */
	writel(msg_data | (interrupt_num - 1), ep->msi_mem + offset);

	return 0;
}
EXPORT_SYMBOL_GPL(dw_pcie_ep_raise_msi_irq);

/**
 * dw_pcie_ep_raise_msix_irq_doorbell - Raise MSI-X to the host using Doorbell
 *					method
 * @ep: DWC EP device
 * @func_no: Function number of the endpoint device
 * @interrupt_num: Interrupt number to be raised
 *
 * Return: 0 if success, errno otherwise.
 */
/* [한국어]
 * dw_pcie_ep_raise_msix_irq_doorbell - IP 전용 도어벨 레지스터로 MSI-X 를 낸다
 *
 * @ep: DWC 엔드포인트 인스턴스.
 * @func_no: 인터럽트를 낼 함수 번호.
 * @interrupt_num: 벡터 번호. 1-기반.
 * @return: 0 성공, -EINVAL 이면 MSI-X 능력이 없음.
 *
 * 아웃바운드 창을 잡아 메모리 쓰기를 만들어 내는 정공법 대신, DesignWare IP 가
 * 제공하는 지름길을 쓴다. PCIE_MSIX_DOORBELL 레지스터에 "몇 번 함수의 몇 번 벡터" 를
 * 한 번 쓰면 IP 가 알아서 MSI-X 메시지 TLP 를 만들어 보낸다. 창 할당·프로그래밍·해제가
 * 전부 사라지므로 훨씬 싸고, 창 재프로그래밍의 경합 문제도 없다.
 *
 * 값의 형식은 상위 8비트(비트 31:24)가 함수 번호, 하위가 0-기반 벡터 번호다.
 * 그래서 func_no 를 24비트 올리고 interrupt_num 에서 1 을 뺀다.
 * DBI 접근이 함수 인덱스를 붙이지 않는 dw_pcie_writel_dbi() 인 것에 주의 — 함수
 * 번호가 이미 값 안에 들어 있어서 레지스터 자체는 컨트롤러 단위이기 때문이다.
 *
 * 도어벨은 IP 통합 옵션이라 모든 SoC 에 있지 않다. 그래서 EXPORT 하지 않고,
 * 이 트리에서 이것을 부르는 곳은 pci-layerscape-ep.c 한 곳이다.
 *
 * 실행 컨텍스트: 글루의 raise_irq 훅 안, 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_epc_raise_irq() → ls_pcie_ep_raise_irq() → [이 함수] → dw_pcie_writel_dbi()
 */
int dw_pcie_ep_raise_msix_irq_doorbell(struct dw_pcie_ep *ep, u8 func_no,
				       u16 interrupt_num)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	/* [한국어] MSI-X 능력 오프셋을 담은 상태 구조체 */
	struct dw_pcie_ep_func *ep_func;
	/* [한국어] 도어벨에 쓸 값 */
	u32 msg_data;

	/* [한국어] 함수 번호로 상태를 찾는다 */
	ep_func = dw_pcie_ep_get_func_from_ep(ep, func_no);
	/* [한국어] 등록되지 않은 함수이거나 MSI-X 능력이 없음 */
	if (!ep_func || !ep_func->msix_cap)
		return -EINVAL;

	/* [한국어] 상위 8비트(31:24)에 함수 번호를 놓는다 */
	msg_data = (func_no << PCIE_MSIX_DOORBELL_PF_SHIFT) |
		   /* [한국어] 하위에 0-기반 벡터 번호. EPC 규약의 1-기반을 낮춘 것이다 */
		   (interrupt_num - 1);

	/* [한국어] 도어벨 한 번 쓰기로 IP 가 MSI-X TLP 를 만들어 보낸다. 함수 번호가 값 안에 들어 있어
	 * 함수 인덱스를 붙이는 접근자가 아니라 컨트롤러 단위 dw_pcie_writel_dbi() 를 쓴다 */
	dw_pcie_writel_dbi(pci, PCIE_MSIX_DOORBELL, msg_data);

	return 0;
}

/**
 * dw_pcie_ep_raise_msix_irq - Raise MSI-X to the host
 * @ep: DWC EP device
 * @func_no: Function number of the endpoint device
 * @interrupt_num: Interrupt number to be raised
 *
 * Return: 0 if success, errno otherwise.
 */
/* [한국어]
 * dw_pcie_ep_raise_msix_irq - BAR 안의 MSI-X 테이블을 읽어 해당 벡터를 발사한다
 *
 * @ep: DWC 엔드포인트 인스턴스.
 * @func_no: 인터럽트를 낼 함수 번호.
 * @interrupt_num: 벡터 번호. 1-기반.
 * @return: 0 성공, -EINVAL 이면 능력 없음, -EPERM 이면 그 벡터가 마스크됨, 그 외 매핑 오류.
 *
 * MSI 와 다른 점은 목적지 주소/데이터가 설정 공간이 아니라 BAR 안의 테이블에 있다는
 * 것이다. 그래서 절차가 한 단계 늘어난다 — 먼저 MSI-X 능력의 TABLE 레지스터에서
 * "어느 BAR 의 어느 오프셋" 을 뽑고(하위 3비트 BIR, 나머지 오프셋), 그 BAR 에 대응하는
 * EPF 버퍼의 가상 주소에서 테이블을 읽는다. epf_bar[bir]->addr 이 그 가상 주소다.
 *
 * 테이블 엔트리에는 마스크 비트가 있다. 호스트가 그 벡터를 일시적으로 막아 둔
 * 상태라면 인터럽트를 내면 안 되므로 -EPERM 으로 거절한다. 이것은 오류라기보다
 * 정상적인 흐름 제어이고, 그래서 dev_err 가 아니라 dev_dbg 로 남긴다.
 *
 * MSI 경로와 달리 여기서는 매번 창을 잡았다 푼다. 벡터마다 목적지 주소가 다를 수
 * 있어 재사용이 어렵기 때문이다. writel 뒤의 readl 이 중요한데, PCIe 쓰기는 posted
 * 라서 완료를 기다리지 않고 돌아온다 — 읽기를 한 번 끼워 그 쓰기가 실제로 나갔음을
 * 보장한 뒤에야 창을 걷을 수 있다. 창을 먼저 걷으면 아직 나가지 못한 쓰기가 사라진다.
 *
 * 실행 컨텍스트: 글루의 raise_irq 훅 안, 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_epc_raise_irq() → 글루 raise_irq → [이 함수]
 *     → dw_pcie_ep_align_addr(), dw_pcie_ep_map_addr(), writel(), readl(),
 *       dw_pcie_ep_unmap_addr()
 */
int dw_pcie_ep_raise_msix_irq(struct dw_pcie_ep *ep, u8 func_no,
			      u16 interrupt_num)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	/* [한국어] BAR 안의 MSI-X 테이블을 가리킬 포인터. 엔트리는 (주소, 데이터, 벡터 제어) 구조다 */
	struct pci_epf_msix_tbl *msix_tbl;
	/* [한국어] MSI-X 능력 오프셋과 BAR 목록을 담은 상태 구조체 */
	struct dw_pcie_ep_func *ep_func;
	/* [한국어] 창을 잡고 정렬할 때 넘길 EPC 객체 */
	struct pci_epc *epc = ep->epc;
	/* [한국어] MSI-X 도 4바이트 쓰기 한 번이다 */
	size_t map_size = sizeof(u32);
	/* [한국어] 정렬로 깎여 나간 만큼 */
	size_t offset;
	/* [한국어] reg 는 레지스터 오프셋, msg_data 는 보낼 값, vec_ctrl 은 벡터 마스크 상태 */
	u32 reg, msg_data, vec_ctrl;
	/* [한국어] Table 오프셋 레지스터의 원시 값. BIR 과 오프셋이 섞여 있다 */
	u32 tbl_offset;
	/* [한국어] 이 벡터의 목적지 주소 */
	u64 msg_addr;
	/* [한국어] 창 매핑 결과 */
	int ret;
	/* [한국어] 테이블이 들어 있는 BAR 번호(BAR Indicator Register) */
	u8 bir;

	/* [한국어] 함수 번호로 상태를 찾는다 */
	ep_func = dw_pcie_ep_get_func_from_ep(ep, func_no);
	/* [한국어] 등록되지 않은 함수이거나 MSI-X 능력이 없음 */
	if (!ep_func || !ep_func->msix_cap)
		return -EINVAL;

	/* [한국어] Table 오프셋 레지스터의 위치 */
	reg = ep_func->msix_cap + PCI_MSIX_TABLE;
	/* [한국어] '테이블이 어느 BAR 의 어디에 있는지' 를 읽는다. set_msix 가 써 둔 값이다 */
	tbl_offset = dw_pcie_ep_readl_dbi(ep, func_no, reg);
	/* [한국어] 하위 3비트가 BAR 번호 */
	bir = FIELD_GET(PCI_MSIX_TABLE_BIR, tbl_offset);
	/* [한국어] 나머지 상위 비트가 BAR 안의 오프셋 */
	tbl_offset &= PCI_MSIX_TABLE_OFFSET;

	/* [한국어] 그 BAR 에 대응하는 EPF 버퍼의 가상 주소에 오프셋을 더해 테이블 시작을 얻는다.
	 * epf_bar[bir]->addr 은 EPF 가 잡아 set_bar 로 넘긴 버퍼의 커널 가상 주소다 */
	msix_tbl = ep_func->epf_bar[bir]->addr + tbl_offset;
	/* [한국어] 이 벡터의 목적지 주소. 인덱스를 (interrupt_num - 1) 로 낮춘다 */
	msg_addr = msix_tbl[(interrupt_num - 1)].msg_addr;
	/* [한국어] 이 벡터의 메시지 값 */
	msg_data = msix_tbl[(interrupt_num - 1)].msg_data;
	/* [한국어] 이 벡터의 제어 워드. 마스크 비트가 여기 있다 */
	vec_ctrl = msix_tbl[(interrupt_num - 1)].vector_ctrl;

	/* [한국어] 호스트가 이 벡터를 일시적으로 막아 둔 상태인지 확인 */
	if (vec_ctrl & PCI_MSIX_ENTRY_CTRL_MASKBIT) {
		/* [한국어] 오류가 아니라 정상적인 흐름 제어라 dev_err 가 아니라 dev_dbg 로 남긴다 */
		dev_dbg(pci->dev, "MSI-X entry ctrl set\n");
		return -EPERM;
	}

	/* [한국어] iATU 정렬에 맞게 목적지 주소를 내린다 */
	msg_addr = dw_pcie_ep_align_addr(epc, msg_addr, &map_size, &offset);
	/* [한국어] 미리 예약해 둔 msi_mem 페이지를 이 벡터의 목적지에 연결한다. MSI 와 달리 매번 잡는다 —
	 * 벡터마다 목적지가 달라 재사용이 어렵기 때문 */
	ret = dw_pcie_ep_map_addr(epc, func_no, 0, ep->msi_mem_phys, msg_addr,
				  map_size);
	/* [한국어] 창을 못 잡으면 인터럽트를 낼 수 없다 */
	if (ret)
		return ret;

	/* [한국어] 실제 발사. 이 쓰기가 호스트가 보는 MSI-X 메시지다 */
	writel(msg_data, ep->msi_mem + offset);

	/* flush posted write before unmap */
	readl(ep->msi_mem + offset);

	/* [한국어] 창을 반납한다. 위의 readl 로 쓰기가 실제로 나간 것을 보장한 뒤라야 안전하다 */
	dw_pcie_ep_unmap_addr(epc, func_no, 0, ep->msi_mem_phys);

	return 0;
}
EXPORT_SYMBOL_GPL(dw_pcie_ep_raise_msix_irq);

/**
 * dw_pcie_ep_cleanup - Cleanup DWC EP resources after fundamental reset
 * @ep: DWC EP device
 *
 * Cleans up the DWC EP specific resources like eDMA etc... after fundamental
 * reset like PERST#. Note that this API is only applicable for drivers
 * supporting PERST# or any other methods of fundamental reset.
 */
/* [한국어]
 * dw_pcie_ep_cleanup - 근본적 리셋 뒤에 DWC 고유 자원을 정리한다
 *
 * @ep: DWC 엔드포인트 인스턴스.
 * @return: 없음.
 *
 * PERST# 같은 근본적 리셋이 들어오면 컨트롤러의 상태가 통째로 날아간다. 그때
 * 소프트웨어 쪽에 남아 있는 파생 객체 — debugfs 디렉토리와 eDMA 채널 등록 —
 * 도 같이 걷어 내야 재초기화가 깨끗하게 된다. 원문 주석대로 PERST# 나 그에 준하는
 * 리셋을 지원하는 드라이버만 부르면 되는 API 다.
 *
 * dw_pcie_ep_deinit() 도 이 함수를 부르므로, 정상 종료 경로와 리셋 경로가 같은
 * 정리 코드를 공유한다. EXPORT_SYMBOL_GPL 로 내보내며 SoC 글루가 리셋 처리에서 쓴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(리셋 처리 워크나 remove 경로).
 *
 * 호출 체인:
 *   글루의 PERST# 처리 / dw_pcie_ep_deinit() → [이 함수]
 *     → dwc_pcie_debugfs_deinit(), dw_pcie_edma_remove()
 */
void dw_pcie_ep_cleanup(struct dw_pcie_ep *ep)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);

	dwc_pcie_debugfs_deinit(pci);
	dw_pcie_edma_remove(pci);
}
EXPORT_SYMBOL_GPL(dw_pcie_ep_cleanup);

/**
 * dw_pcie_ep_deinit - Deinitialize the endpoint device
 * @ep: DWC EP device
 *
 * Deinitialize the endpoint device. EPC device is not destroyed since that will
 * be taken care by Devres.
 */
/* [한국어]
 * dw_pcie_ep_deinit - 엔드포인트를 해체한다(dw_pcie_ep_init 의 역)
 *
 * @ep: DWC 엔드포인트 인스턴스.
 * @return: 없음.
 *
 * init 이 잡은 것을 역순으로 되돌린다 — 파생 자원 정리(cleanup), MSI 발사용으로
 * 예약해 둔 한 페이지 반납, 그리고 EPC 메모리 할당자 해제. 정작 EPC 디바이스 자체는
 * 파괴하지 않는데, devm_pci_epc_create() 로 만들어 디바이스 수명에 묶여 있어
 * devres 가 알아서 처리하기 때문이다. 원문 주석이 그 점을 명시한다.
 *
 * msi_mem 반납에 넘기는 크기가 요청 크기가 아니라 epc->mem->window.page_size 인 것은,
 * 할당 때에도 같은 단위로 잡았기 때문이다(dw_pcie_ep_init 참고).
 *
 * 실행 컨텍스트: remove 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   글루 드라이버의 remove → [이 함수]
 *     → dw_pcie_ep_cleanup(), pci_epc_mem_free_addr(), pci_epc_mem_exit()
 */
void dw_pcie_ep_deinit(struct dw_pcie_ep *ep)
{
	struct pci_epc *epc = ep->epc;

	dw_pcie_ep_cleanup(ep);

	/* [한국어] MSI 발사용으로 예약해 둔 페이지를 반납한다. 크기가 요청 크기가 아니라 할당자의
	 * 페이지 단위인 것은 잡을 때도 같은 단위였기 때문이다 */
	pci_epc_mem_free_addr(epc, ep->msi_mem_phys, ep->msi_mem,
			      epc->mem->window.page_size);

	pci_epc_mem_exit(epc);
}
EXPORT_SYMBOL_GPL(dw_pcie_ep_deinit);

/* [한국어]
 * dw_pcie_ep_init_rebar_registers - Resizable BAR 이 광고할 크기 목록을 다시 세운다
 *
 * @ep: DWC 엔드포인트 인스턴스.
 * @func_no: 대상 함수 번호.
 * @return: 없음.
 *
 * REBAR 항목의 "선택된 크기" 필드는 sticky 가 아니어서 링크가 끊기거나 리셋이 오면
 * 지워진다. 반면 지원 크기 목록(CAP)은 sticky 다. 문제는 IP 버전에 따라 sticky 여부가
 * 달라진다는 점이라, 원문 주석은 CAP 을 다시 쓰는 방식으로 통일한다 — CAP 을 쓰면
 * 컨트롤러가 CTRL 의 선택된 크기를 자동으로 갱신해 주기 때문이다.
 *
 * 무엇을 광고할지는 BAR 마다 다르다. 이미 dw_pcie_ep_set_bar() 로 크기가 정해진
 * BAR 이면 그 크기만 광고하고(호스트가 다른 크기를 고르지 못하게), 아직 안 정해진
 * BAR 이면 BIT(4) 를 쓴다. PCIe r6.0 7.8.6.2 가 1MB~512GB 중 최소 하나를 지원하라고
 * 요구하므로 최소한인 1MB 하나만 광고하는 것이고, BIT(4) 가 그 1MB 자리다.
 *
 * 실행 컨텍스트: 초기화/링크다운 처리의 프로세스 컨텍스트. 호출자가 이미
 * dw_pcie_dbi_ro_wr_en() 을 켜 둔 상태에서 불린다.
 *
 * 호출 체인:
 *   dw_pcie_ep_init_non_sticky_registers() → [이 함수]
 *     → dw_pcie_ep_find_ext_capability(), pci_epc_bar_size_to_rebar_cap()
 */
static void dw_pcie_ep_init_rebar_registers(struct dw_pcie_ep *ep, u8 func_no)
{
	struct dw_pcie_ep_func *ep_func = dw_pcie_ep_get_func_from_ep(ep, func_no);
	/* [한국어] offset 은 REBAR 항목을 훑을 위치, nbars 는 항목 개수 */
	unsigned int offset, nbars;
	/* [한국어] 이번 항목이 담당하는 BAR 번호 */
	enum pci_barno bar;
	/* [한국어] reg 는 읽은 CTRL, i 는 순회 인덱스, val 은 읽고 쓸 값 */
	u32 reg, i, val;

	/* [한국어] 등록되지 않은 함수면 광고할 것이 없다 */
	if (!ep_func)
		return;

	/* [한국어] Resizable BAR 확장 capability 를 찾는다 */
	offset = dw_pcie_ep_find_ext_capability(ep, func_no, PCI_EXT_CAP_ID_REBAR);

	/* [한국어] REBAR 이 있는 IP 설정일 때만 진행한다. 없으면 아무것도 하지 않고 끝난다 */
	if (offset) {
		/* [한국어] 첫 항목의 CTRL 을 읽는다 */
		reg = dw_pcie_ep_readl_dbi(ep, func_no, offset + PCI_REBAR_CTRL);
		/* [한국어] 이 capability 가 담고 있는 항목 개수 */
		nbars = FIELD_GET(PCI_REBAR_CTRL_NBAR_MASK, reg);

		/*
		 * PCIe r6.0, sec 7.8.6.2 require us to support at least one
		 * size in the range from 1 MB to 512 GB. Advertise support
		 * for 1 MB BAR size only.
		 *
		 * For a BAR that has been configured via dw_pcie_ep_set_bar(),
		 * advertise support for only that size instead.
		 */
		for (i = 0; i < nbars; i++, offset += PCI_REBAR_CTRL) {
			/*
			 * While the RESBAR_CAP_REG_* fields are sticky, the
			 * RESBAR_CTRL_REG_BAR_SIZE field is non-sticky (it is
			 * sticky in certain versions of DWC PCIe, but not all).
			 *
			 * RESBAR_CTRL_REG_BAR_SIZE is updated automatically by
			 * the controller when RESBAR_CAP_REG is written, which
			 * is why RESBAR_CAP_REG is written here.
			 */
			val = dw_pcie_ep_readl_dbi(ep, func_no, offset + PCI_REBAR_CTRL);
			/* [한국어] 이 항목이 담당하는 BAR 번호 */
			bar = FIELD_GET(PCI_REBAR_CTRL_BAR_IDX, val);
			/* [한국어] 이미 set_bar 로 크기가 정해진 BAR 인지 확인 */
			if (ep_func->epf_bar[bar])
				/* [한국어] 정해졌으면 그 크기 하나만 광고해 호스트가 다른 크기를 고르지 못하게 한다 */
				pci_epc_bar_size_to_rebar_cap(ep_func->epf_bar[bar]->size, &val);
			else
				/* [한국어] 아직 안 정해졌으면 최소한만 광고한다. BIT(4) 가 1MB 자리이고, PCIe r6.0 7.8.6.2 는
				 * 1MB~512GB 중 최소 하나를 지원하라고 요구한다 */
				val = BIT(4);

			/* [한국어] CAP 에 쓴다. 이 쓰기가 CTRL 의 '선택된 크기' 필드를 자동 갱신하는 것이 핵심이다 */
			dw_pcie_ep_writel_dbi(ep, func_no, offset + PCI_REBAR_CAP, val);
		}
	}
}

/* [한국어]
 * dw_pcie_ep_init_non_sticky_registers - 리셋으로 날아가는 레지스터들을 다시 채운다
 *
 * @pci: DWC 컨트롤러.
 * @return: 없음.
 *
 * PCIe 레지스터에는 근본적 리셋을 견디는 sticky 와 그렇지 않은 non-sticky 가 있다.
 * 링크가 내려갔다 올라오는 사이 non-sticky 쪽은 초기값으로 돌아가므로, 링크가 다시
 * 붙기 전에 우리 설정을 되써야 한다. 그래서 이 함수는 초기화 때뿐 아니라
 * dw_pcie_ep_linkdown() 에서도 불린다 — PERST# 를 감지하지 못하는 드라이버에게는
 * 이것이 유일한 복구 기회다.
 *
 * 되쓰는 것은 세 가지다. (1) 함수마다 REBAR 광고, (2) dw_pcie_setup() 이 하는 공통
 * 링크/포트 설정, (3) 다기능 엔드포인트일 때 함수 1 이상의 링크 능력 필드.
 * (3)이 필요한 이유는 PCIe r7.0 7.5.3.6 이 모든 함수가 같은 최대 링크 폭과 속도를
 * 보고해야 한다고 요구하는데 dw_pcie_setup() 은 함수 0 만 건드리기 때문이다.
 * 그래서 함수 0 의 MLW/SLS 를 읽어 나머지 함수에 그대로 복사한다.
 *
 * 전체가 dw_pcie_dbi_ro_wr_en()/dis() 로 감싸여 있다 — 여기서 쓰는 필드는 대부분
 * 호스트 관점 읽기 전용이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(초기화 또는 링크다운 통지 경로).
 *
 * 호출 체인:
 *   dw_pcie_ep_init_registers() / dw_pcie_ep_linkdown() → [이 함수]
 *     → dw_pcie_ep_init_rebar_registers(), dw_pcie_setup(), dw_pcie_find_capability()
 */
static void dw_pcie_ep_init_non_sticky_registers(struct dw_pcie *pci)
{
	struct dw_pcie_ep *ep = &pci->ep;
	/* [한국어] 이 컨트롤러가 노출하는 함수 개수 */
	u8 funcs = ep->epc->max_functions;
	/* [한국어] func0_lnkcap 은 함수 0 에서 뽑은 링크 능력 필드, lnkcap 은 다른 함수의 값 */
	u32 func0_lnkcap, lnkcap;
	/* [한국어] func_no 는 순회 인덱스, offset 은 PCIe 능력 구조체의 위치 */
	u8 func_no, offset;

	dw_pcie_dbi_ro_wr_en(pci);

	/* [한국어] 함수마다 REBAR 광고를 되쓴다 */
	for (func_no = 0; func_no < funcs; func_no++)
		/* [한국어] non-sticky 인 '선택된 크기' 를 CAP 재기록으로 되살리는 경로 */
		dw_pcie_ep_init_rebar_registers(ep, func_no);

	dw_pcie_setup(pci);

	/*
	 * PCIe r7.0, section 7.5.3.6 states that for multi-function
	 * endpoints, max link width and speed fields must report same
	 * values for all functions. However, dw_pcie_setup() programs
	 * these fields only for function 0. Hence, mirror these fields
	 * to all other functions as well.
	 */
	if (funcs > 1) {
		/* [한국어] PCI Express capability 구조체를 찾는다. 인덱스 없는 접근자라 함수 0 을 본다 */
		offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);
		/* [한국어] 함수 0 의 Link Capabilities 레지스터를 읽는다 */
		func0_lnkcap = dw_pcie_readl_dbi(pci, offset + PCI_EXP_LNKCAP);
		/* [한국어] 최대 링크 폭(MLW)과 지원 속도(SLS) 필드만 뽑아낸다 */
		func0_lnkcap = FIELD_GET(PCI_EXP_LNKCAP_MLW |
					 /* [한국어] 두 필드를 한 마스크로 묶어 한 번에 추출 */
					 PCI_EXP_LNKCAP_SLS, func0_lnkcap);

		/* [한국어] 함수 1 부터 — 함수 0 은 dw_pcie_setup() 이 이미 제대로 채웠다 */
		for (func_no = 1; func_no < funcs; func_no++) {
			/* [한국어] 이 함수의 PCIe 능력 구조체 위치. 함수마다 다를 수 있어 매번 찾는다 */
			offset = dw_pcie_ep_find_capability(ep, func_no,
							    PCI_CAP_ID_EXP);
			/* [한국어] 이 함수의 Link Capabilities 를 읽는다 */
			lnkcap = dw_pcie_ep_readl_dbi(ep, func_no,
						      offset + PCI_EXP_LNKCAP);
			/* [한국어] MLW/SLS 자리만 함수 0 의 값으로 갈아 끼운다. 나머지 비트는 건드리지 않는다 */
			FIELD_MODIFY(PCI_EXP_LNKCAP_MLW | PCI_EXP_LNKCAP_SLS,
				     &lnkcap, func0_lnkcap);
			/* [한국어] 되쓴다. PCIe r7.0 7.5.3.6 이 모든 함수가 같은 값을 보고하라고 요구한다 */
			dw_pcie_ep_writel_dbi(ep, func_no,
					      offset + PCI_EXP_LNKCAP, lnkcap);
		}
	}

	dw_pcie_dbi_ro_wr_dis(pci);
}

/* [한국어]
 * dw_pcie_ep_disable_bars - 예약 BAR 을 뺀 모든 BAR 을 꺼진 상태로 초기화한다
 *
 * @ep: DWC 엔드포인트 인스턴스.
 * @return: 없음.
 *
 * EPC 코어의 설계 규약은 "모든 BAR 은 기본적으로 꺼져 있고, EPF 드라이버가 쓰고 싶은
 * BAR 만 켠다" 이다. 초기화 시점에 그 규약에 맞는 상태를 만들어 두는 것이 이 함수다.
 * 그러지 않으면 IP 의 리셋 기본값에 따라 EPF 가 요청하지도 않은 BAR 이 호스트에게
 * 보일 수 있다.
 *
 * 예외가 BAR_RESERVED 다. 이 종류는 SoC 가 다른 용도로 배선해 둔 BAR 이라 소프트웨어가
 * 꺼서는 안 된다. 종류 판정은 글루의 능력표를 보는 dw_pcie_ep_get_bar_type() 이 한다.
 *
 * 실행 컨텍스트: 초기화 중 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   dw_pcie_ep_init_registers() → [이 함수]
 *     → dw_pcie_ep_get_bar_type(), dw_pcie_ep_reset_bar()
 */
static void dw_pcie_ep_disable_bars(struct dw_pcie_ep *ep)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	/* [한국어] 이번 BAR 의 종류 */
	enum pci_epc_bar_type bar_type;
	/* [한국어] BAR 순회 인덱스 */
	enum pci_barno bar;

	/* [한국어] 표준 BAR 6개를 전부 훑는다 */
	for (bar = 0; bar < PCI_STD_NUM_BARS; bar++) {
		/* [한국어] 글루 능력표에서 이 BAR 의 종류를 읽는다 */
		bar_type = dw_pcie_ep_get_bar_type(ep, bar);

		/*
		 * Reserved BARs should not get disabled by default. All other
		 * BAR types are disabled by default.
		 *
		 * This is in line with the current EPC core design, where all
		 * BARs are disabled by default, and then the EPF driver enables
		 * the BARs it wishes to use.
		 */
		if (bar_type != BAR_RESERVED)
			/* [한국어] 모든 함수에 대해 이 BAR 을 끈다 */
			dw_pcie_ep_reset_bar(pci, bar);
	}
}

/**
 * dw_pcie_ep_init_registers - Initialize DWC EP specific registers
 * @ep: DWC EP device
 *
 * Initialize the registers (CSRs) specific to DWC EP. This API should be called
 * only when the endpoint receives an active refclk (either from host or
 * generated locally).
 */
/* [한국어]
 * dw_pcie_ep_init_registers - 참조 클럭이 살아난 뒤 컨트롤러 레지스터를 세운다
 *
 * @ep: DWC 엔드포인트 인스턴스.
 * @return: 0 성공, -EIO 면 IP 가 EP 모드가 아님, -ENOMEM 이면 자원 부족, 그 외 하위 오류.
 *
 * dw_pcie_ep_init() 과 나뉘어 있는 이유가 이 함수의 전부를 설명한다 — 여기서 하는 일은
 * 전부 레지스터 접근이고, 레지스터는 컨트롤러에 참조 클럭이 들어와야 읽고 쓸 수 있다.
 * 클럭을 호스트가 공급하는 SoC 에서는 그 시점이 probe 보다 한참 뒤(PERST# 해제 이후)라,
 * 순수한 소프트웨어 준비(init)와 하드웨어 접근(이 함수)을 갈라 둔 것이다. 그래서
 * PERST# 를 지원하는 드라이버는 리셋이 풀릴 때마다 이 함수를 다시 부른다.
 *
 * 단계: (1) 헤더 타입을 읽어 IP 가 정말 EP 모드로 설정됐는지 확인 — 브리지 헤더가
 * 나오면 RC 모드로 스트랩된 것이라 더 진행할 수 없다. (2) IP 버전과 iATU 구성,
 * eDMA 존재를 탐지. (3) 인바운드/아웃바운드 창 비트맵과 outbound_addr 배열을 할당하되,
 * 이미 있으면 건너뛴다(재호출 대비). (4) 함수마다 dw_pcie_ep_func 를 만들어 MSI/MSI-X
 * 능력 오프셋을 미리 찾아 둔다 — 이 역시 이미 있으면 건너뛴다. (5) 글루의 init 훅,
 * (6) BAR 전부 끄기, (7) PTM 설정, (8) non-sticky 레지스터, (9) debugfs.
 *
 * PTM 처리에 원문 주석 두 개가 붙어 있다. 하나는 PTM 능력 구조체가 함수별이 아니라
 * 컨트롤러 단위라는 것 — PCIe r6.0 7.9.15 가 정확히 한 함수에만 두라고 요구하므로,
 * func_no 를 붙이는 접근자가 아니라 표준 DBI 접근자를 쓴다. 다른 하나는 responder
 * 능력을 끄기 전에 root 능력을 먼저 꺼야 한다는 순서 제약이다. 엔드포인트가 PTM
 * root 를 광고할 이유가 없으므로 그 비트를 내린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. devm_* 할당을 쓰므로 잠들 수 있다.
 * 에러 경로: 자원 할당이 하나라도 실패하면 err_remove_edma 로 가서, 이 함수가 켠
 * eDMA 를 되돌린 뒤 오류를 올린다.
 *
 * 호출 체인:
 *   글루 probe 또는 PERST# 해제 처리 → [이 함수]
 *     → dw_pcie_version_detect(), dw_pcie_iatu_detect(), dw_pcie_edma_detect(),
 *       dw_pcie_ep_find_capability(), ep->ops->init(), dw_pcie_ep_disable_bars(),
 *       dw_pcie_ep_init_non_sticky_registers(), dwc_pcie_debugfs_init()
 */
int dw_pcie_ep_init_registers(struct dw_pcie_ep *ep)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	/* [한국어] 함수마다 새로 만들 상태 구조체를 담을 임시 포인터 */
	struct dw_pcie_ep_func *ep_func;
	/* [한국어] devm 할당의 주인이자 오류 로그를 남길 디바이스 */
	struct device *dev = pci->dev;
	/* [한국어] max_functions 를 읽으려고 EPC 객체를 꺼내 둔다 */
	struct pci_epc *epc = ep->epc;
	/* [한국어] ptm_cap_base 는 PTM 능력 구조체 위치, reg 는 읽고 고칠 값 */
	u32 ptm_cap_base, reg;
	/* [한국어] 설정 공간 헤더 타입. IP 가 EP 모드인지 판별하는 근거 */
	u8 hdr_type;
	/* [한국어] 함수 순회 인덱스 */
	u8 func_no;
	/* [한국어] 할당 결과를 잠시 담을 포인터. 성공한 뒤에야 ep->outbound_addr 에 넣는다 */
	void *addr;
	/* [한국어] 오류 코드 */
	int ret;

	/* [한국어] 헤더 타입 바이트를 읽는다. 이 값이 IP 가 어떤 모드로 스트랩됐는지 알려 준다 */
	hdr_type = dw_pcie_readb_dbi(pci, PCI_HEADER_TYPE) &
		   /* [한국어] 상위 비트는 다기능 여부 표시라 하위 7비트만 남긴다 */
		   PCI_HEADER_TYPE_MASK;
	/* [한국어] NORMAL(0)이 아니면 브리지 헤더 — RC 모드로 설정된 IP 라 엔드포인트로 쓸 수 없다 */
	if (hdr_type != PCI_HEADER_TYPE_NORMAL) {
		dev_err(pci->dev,
			"PCIe controller is not set to EP mode (hdr_type:0x%x)!\n",
			hdr_type);
		return -EIO;
	}

	dw_pcie_version_detect(pci);

	dw_pcie_iatu_detect(pci);

	/* [한국어] 내장 DMA 엔진이 있는지 탐지하고 있으면 dmaengine 에 등록한다 */
	ret = dw_pcie_edma_detect(pci);
	/* [한국어] eDMA 탐지가 실패하면 여기서 중단. 아직 아무것도 잡지 않았으므로 되돌릴 것이 없다 */
	if (ret)
		return ret;

	ret = -ENOMEM;
	/* [한국어] 재호출 대비 — PERST# 해제마다 이 함수가 다시 불릴 수 있어 이미 있으면 다시 잡지 않는다 */
	if (!ep->ib_window_map) {
		/* [한국어] 인바운드 창 점유 비트맵. 크기는 방금 iatu_detect 가 알아낸 실제 창 개수다 */
		ep->ib_window_map = devm_bitmap_zalloc(dev, pci->num_ib_windows,
						       GFP_KERNEL);
		/* [한국어] 메모리 부족 */
		if (!ep->ib_window_map)
			goto err_remove_edma;
	}

	/* [한국어] 아웃바운드 비트맵도 같은 이유로 한 번만 잡는다 */
	if (!ep->ob_window_map) {
		/* [한국어] 아웃바운드 창 점유 비트맵 */
		ep->ob_window_map = devm_bitmap_zalloc(dev, pci->num_ob_windows,
						       GFP_KERNEL);
		/* [한국어] 메모리 부족 */
		if (!ep->ob_window_map)
			goto err_remove_edma;
	}

	/* [한국어] 창별 주소 기록 배열도 한 번만 잡는다 */
	if (!ep->outbound_addr) {
		/* [한국어] 창 개수만큼의 phys_addr_t 배열. unmap 때 주소로 창을 되찾는 역인덱스가 된다 */
		addr = devm_kcalloc(dev, pci->num_ob_windows, sizeof(phys_addr_t),
				    GFP_KERNEL);
		/* [한국어] 메모리 부족 */
		if (!addr)
			goto err_remove_edma;
		/* [한국어] 성공했으므로 구조체에 건다 */
		ep->outbound_addr = addr;
	}

	/* [한국어] 노출할 함수 개수만큼 상태 구조체를 준비한다 */
	for (func_no = 0; func_no < epc->max_functions; func_no++) {

		/* [한국어] 이미 만들어 둔 것이 있는지 확인 */
		ep_func = dw_pcie_ep_get_func_from_ep(ep, func_no);
		/* [한국어] 있으면 건너뛴다 — 재호출이므로 기존 상태(BAR 매핑 기록 등)를 보존해야 한다 */
		if (ep_func)
			continue;

		/* [한국어] 새로 만든다. kzalloc 이라 bar_to_atu[] 등이 전부 0(미할당)으로 시작한다 */
		ep_func = devm_kzalloc(dev, sizeof(*ep_func), GFP_KERNEL);
		/* [한국어] 메모리 부족 */
		if (!ep_func)
			goto err_remove_edma;

		/* [한국어] 이 구조체가 담당할 함수 번호를 기록. 나중에 리스트 탐색의 키가 된다 */
		ep_func->func_no = func_no;
		/* [한국어] 이 함수의 MSI 능력 오프셋을 미리 찾아 둔다. 인터럽트를 낼 때마다 탐색하지 않기 위함 */
		ep_func->msi_cap = dw_pcie_ep_find_capability(ep, func_no,
							      PCI_CAP_ID_MSI);
		/* [한국어] MSI-X 능력 오프셋도 미리 찾아 둔다 */
		ep_func->msix_cap = dw_pcie_ep_find_capability(ep, func_no,
							       PCI_CAP_ID_MSIX);

		/* [한국어] 리스트 끝에 붙인다. 순서대로 붙으므로 함수 번호 순이 된다 */
		list_add_tail(&ep_func->list, &ep->func_list);
	}

	/* [한국어] 글루가 SoC 고유 초기화 훅을 제공하는지 */
	if (ep->ops->init)
		ep->ops->init(ep);

	dw_pcie_ep_disable_bars(ep);

	/*
	 * PCIe r6.0, section 7.9.15 states that for endpoints that support
	 * PTM, this capability structure is required in exactly one
	 * function, which controls the PTM behavior of all PTM capable
	 * functions. This indicates the PTM capability structure
	 * represents controller-level registers rather than per-function
	 * registers.
	 *
	 * Therefore, PTM capability registers are configured using the
	 * standard DBI accessors, instead of func_no indexed per-function
	 * accessors.
	 */
	ptm_cap_base = dw_pcie_find_ext_capability(pci, PCI_EXT_CAP_ID_PTM);

	/*
	 * PTM responder capability can be disabled only after disabling
	 * PTM root capability.
	 */
	if (ptm_cap_base) {
		dw_pcie_dbi_ro_wr_en(pci);
		/* [한국어] PTM 능력 워드를 읽는다. 함수 인덱스를 붙이지 않는 접근자인 것은
		 * 이 구조체가 컨트롤러 단위이기 때문이다 */
		reg = dw_pcie_readl_dbi(pci, ptm_cap_base + PCI_PTM_CAP);
		/* [한국어] ROOT 능력 비트를 내린다. 엔드포인트가 PTM 루트를 광고할 이유가 없다 */
		reg &= ~PCI_PTM_CAP_ROOT;
		/* [한국어] 먼저 이것부터 되쓴다 — responder 를 끄기 전에 root 를 꺼야 한다는 순서 제약 때문 */
		dw_pcie_writel_dbi(pci, ptm_cap_base + PCI_PTM_CAP, reg);

		/* [한국어] 방금 쓴 결과를 다시 읽는다. 하드웨어가 값을 조정했을 수 있어 되읽고 고친다 */
		reg = dw_pcie_readl_dbi(pci, ptm_cap_base + PCI_PTM_CAP);
		/* [한국어] responder 능력과 시간 정밀도 필드를 내린다 */
		reg &= ~(PCI_PTM_CAP_RES | PCI_PTM_GRANULARITY_MASK);
		/* [한국어] 되쓴다. 이로써 이 엔드포인트는 PTM 을 광고하지 않는다 */
		dw_pcie_writel_dbi(pci, ptm_cap_base + PCI_PTM_CAP, reg);
		dw_pcie_dbi_ro_wr_dis(pci);
	}

	dw_pcie_ep_init_non_sticky_registers(pci);

	/* [한국어] RAS DES 통계·에러 주입 인터페이스를 debugfs 로 연다. EP 타입임을 알려
	 * pcie-designware-debugfs.c 가 EP 에 해당하는 파일만 만들게 한다 */
	dwc_pcie_debugfs_init(pci, DW_PCIE_EP_TYPE);

	return 0;

err_remove_edma:
	dw_pcie_edma_remove(pci);

	return ret;
}
EXPORT_SYMBOL_GPL(dw_pcie_ep_init_registers);

/**
 * dw_pcie_ep_linkup - Notify EPF drivers about Link Up event
 * @ep: DWC EP device
 */
/* [한국어]
 * dw_pcie_ep_linkup - 링크가 올라왔음을 EPF 드라이버들에게 알린다
 *
 * @ep: DWC 엔드포인트 인스턴스.
 * @return: 없음.
 *
 * 링크 업 이벤트는 SoC 마다 다른 인터럽트나 폴링으로 감지되므로 글루가 먼저 알고,
 * 그것을 EPF 계층까지 전파하는 통로가 이 함수다. EPF 는 이 통지를 받아야 호스트와
 * 실제 통신을 시작할 수 있다 — 링크 전에 호스트 메모리에 접근해 봐야 갈 곳이 없다.
 *
 * 하는 일은 pci_epc_linkup() 으로 넘기는 것뿐이고, 그쪽이 이 EPC 에 바인딩된 모든
 * EPF 의 linkup 콜백을 부른다. EXPORT_SYMBOL_GPL 로 내보낸다.
 *
 * 실행 컨텍스트: 글루의 링크 이벤트 처리(대개 인터럽트 스레드나 워크) 안.
 *
 * 호출 체인:
 *   글루의 링크업 인터럽트 처리 → [이 함수] → pci_epc_linkup() → EPF 의 linkup 콜백
 */
void dw_pcie_ep_linkup(struct dw_pcie_ep *ep)
{
	struct pci_epc *epc = ep->epc;

	pci_epc_linkup(epc);
}
EXPORT_SYMBOL_GPL(dw_pcie_ep_linkup);

/**
 * dw_pcie_ep_linkdown - Notify EPF drivers about Link Down event
 * @ep: DWC EP device
 *
 * Non-sticky registers are also initialized before sending the notification to
 * the EPF drivers. This is needed since the registers need to be initialized
 * before the link comes back again.
 */
/* [한국어]
 * dw_pcie_ep_linkdown - 링크가 끊겼음을 알리고, 그 전에 레지스터를 되살린다
 *
 * @ep: DWC 엔드포인트 인스턴스.
 * @return: 없음.
 *
 * 통지만 하는 linkup 과 달리 여기에는 일이 하나 더 있다. 링크가 내려가면 non-sticky
 * 레지스터가 초기값으로 돌아가는데, 링크는 우리가 모르는 사이에 다시 올라올 수 있다.
 * 그래서 통지보다 먼저 dw_pcie_ep_init_non_sticky_registers() 로 설정을 되써 둔다.
 * 원문 주석이 밝히듯, PERST# 를 지원하지 않는 드라이버에게는 링크가 돌아오기 전에
 * 레지스터를 되살릴 기회가 이것뿐이다.
 *
 * 순서가 뒤바뀌면 EPF 가 통지를 받고 곧바로 하는 처리와 레지스터 복구가 겹칠 수 있다.
 *
 * 실행 컨텍스트: 글루의 링크 이벤트 처리 안, 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   글루의 링크다운 인터럽트 처리 → [이 함수]
 *     → dw_pcie_ep_init_non_sticky_registers(), pci_epc_linkdown()
 */
void dw_pcie_ep_linkdown(struct dw_pcie_ep *ep)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	/* [한국어] 통지를 넘길 EPC 객체 */
	struct pci_epc *epc = ep->epc;

	/*
	 * Initialize the non-sticky DWC registers as they would've reset post
	 * Link Down. This is specifically needed for drivers not supporting
	 * PERST# as they have no way to reinitialize the registers before the
	 * link comes back again.
	 */
	dw_pcie_ep_init_non_sticky_registers(pci);

	pci_epc_linkdown(epc);
}
EXPORT_SYMBOL_GPL(dw_pcie_ep_linkdown);

/* [한국어]
 * dw_pcie_ep_get_resources - 디바이스 트리에서 주소 공간과 함수 개수를 읽어 온다
 *
 * @ep: DWC 엔드포인트 인스턴스.
 * @return: 0 성공, -EINVAL 이면 "addr_space" 리소스가 없음, 그 외 공통 자원 오류.
 *
 * 엔드포인트가 호스트 메모리로 나가려면 그 통로로 쓸 로컬 물리 주소 구간이 필요하다.
 * 그것이 디바이스 트리에 "addr_space" 라는 이름으로 선언된 MEM 리소스이고, 여기서
 * 시작 주소와 크기를 꺼내 ep->phys_base / ep->addr_size 에 둔다. 나중에
 * pci_epc_mem_init() 이 이 구간을 페이지 단위 할당자로 감싸고, map_addr 이 그
 * 할당자에서 받은 주소에 아웃바운드 창을 건다.
 *
 * dw_pcie_parent_bus_offset() 호출 순서에 원문 주석이 붙어 있다. artpec6 의
 * 주소 보정 훅이 ep->phys_base 를 읽으므로, 그 필드를 채운 다음에 불러야 한다.
 * 이 오프셋은 CPU 물리주소와 컨트롤러가 붙은 부모 버스 주소의 차이이고,
 * map_addr/unmap_addr 이 이 값을 빼서 창에 적을 주소를 만든다.
 *
 * "max-functions" 속성이 없으면 1 로 가정한다 — 대부분의 엔드포인트는 단일 함수다.
 *
 * 실행 컨텍스트: probe 중 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   dw_pcie_ep_init() → [이 함수]
 *     → dw_pcie_get_resources(), platform_get_resource_byname(),
 *       dw_pcie_parent_bus_offset(), of_property_read_u8()
 */
static int dw_pcie_ep_get_resources(struct dw_pcie_ep *ep)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	/* [한국어] 플랫폼 디바이스 변환과 DT 노드 접근에 쓸 디바이스 */
	struct device *dev = pci->dev;
	/* [한국어] 리소스를 이름으로 찾으려면 플랫폼 디바이스 형태가 필요하다 */
	struct platform_device *pdev = to_platform_device(dev);
	/* [한국어] 이 컨트롤러의 디바이스 트리 노드. max-functions 속성이 여기 있다 */
	struct device_node *np = dev->of_node;
	/* [한국어] max_functions 를 채워 넣을 EPC 객체 */
	struct pci_epc *epc = ep->epc;
	/* [한국어] addr_space 리소스를 받을 포인터 */
	struct resource *res;
	/* [한국어] 오류 코드 */
	int ret;

	/* [한국어] DBI/DBI2/iATU 레지스터 창 등 RC/EP 공통 자원을 먼저 확보한다 */
	ret = dw_pcie_get_resources(pci);
	/* [한국어] 공통 자원이 없으면 더 진행할 수 없다 */
	if (ret)
		return ret;

	/* [한국어] 이름이 addr_space 인 MEM 리소스를 찾는다. 이것이 아웃바운드 창을 뚫을 로컬 물리 구간이다 */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "addr_space");
	/* [한국어] 이 구간이 없으면 호스트 메모리로 나갈 통로를 만들 수 없다 */
	if (!res)
		return -EINVAL;

	/* [한국어] 구간 시작 주소 */
	ep->phys_base = res->start;
	/* [한국어] 구간 크기. 이 둘이 곧 EPC 메모리 할당자의 관리 범위가 된다 */
	ep->addr_size = resource_size(res);

	/*
	 * artpec6_pcie_cpu_addr_fixup() uses ep->phys_base, so call
	 * dw_pcie_parent_bus_offset() after setting ep->phys_base.
	 */
	pci->parent_bus_offset = dw_pcie_parent_bus_offset(pci, "addr_space",
							   ep->phys_base);

	/* [한국어] DT 에서 노출할 함수 개수를 읽는다 */
	ret = of_property_read_u8(np, "max-functions", &epc->max_functions);
	/* [한국어] 속성이 없거나 읽기 실패면 */
	if (ret < 0)
		/* [한국어] 단일 함수 엔드포인트로 가정한다. 대부분이 그렇다 */
		epc->max_functions = 1;

	return 0;
}

/**
 * dw_pcie_ep_init - Initialize the endpoint device
 * @ep: DWC EP device
 *
 * Initialize the endpoint device. Allocate resources and create the EPC
 * device with the endpoint framework.
 *
 * Return: 0 if success, errno otherwise.
 */
/* [한국어]
 * dw_pcie_ep_init - EPC 디바이스를 만들고 엔드포인트를 쓸 준비를 한다
 *
 * @ep: 글루 드라이버가 자기 구조체 안에 품고 있는 DWC 엔드포인트 인스턴스.
 * @return: 0 성공, EPC 생성/자원 획득/메모리 초기화 실패 시 그 오류 코드.
 *
 * 엔드포인트 쪽 진입점이다. 여기서 하는 일은 전부 레지스터를 건드리지 않는 준비
 * 작업이고, 실제 하드웨어 초기화는 참조 클럭이 확보된 뒤 dw_pcie_ep_init_registers()
 * 가 맡는다. 이 분리가 이 드라이버 수명주기 설계의 핵심이다.
 *
 * 단계: (1) 함수 리스트를 비우고 MSI 창 재사용 상태를 초기화, (2) devm 으로 EPC
 * 디바이스를 만들며 이 파일의 epc_ops 를 등록 — 이 순간부터 EPF 프레임워크가 이
 * 컨트롤러를 볼 수 있다. (3) drvdata 로 ep 를 걸어 두어 콜백에서 epc_get_drvdata()
 * 로 되찾게 한다. (4) 디바이스 트리 자원 획득, (5) 글루의 pre_init 훅,
 * (6) addr_space 구간을 EPC 메모리 할당자로 등록, (7) 그 할당자에서 한 페이지를
 * 미리 떼어 MSI/MSI-X 발사 전용으로 예약.
 *
 * (7)이 필요한 이유는, 인터럽트를 낼 때마다 주소를 새로 할당하면 실패할 수 있고
 * 그 실패를 인터럽트 경로에서 복구할 방법이 없기 때문이다. 미리 잡아 두면
 * raise_msi 는 창만 프로그래밍하면 된다.
 *
 * 실행 컨텍스트: 글루 probe 안, 프로세스 컨텍스트. devm 할당을 쓰므로 잠들 수 있다.
 * 에러 경로: msi_mem 예약이 실패하면 err_exit_epc_mem 으로 가서 방금 초기화한
 * EPC 메모리 할당자를 되돌린다. 그 앞 단계의 실패는 devm 이 알아서 정리한다.
 *
 * 호출 체인:
 *   글루 드라이버 probe → [이 함수]
 *     → devm_pci_epc_create(), dw_pcie_ep_get_resources(), ep->ops->pre_init(),
 *       pci_epc_mem_init(), pci_epc_mem_alloc_addr()
 */
int dw_pcie_ep_init(struct dw_pcie_ep *ep)
{
	int ret;
	/* [한국어] 만들 EPC 객체를 받을 포인터 */
	struct pci_epc *epc;
	/* [한국어] ep 를 품은 dw_pcie 를 되찾는다 */
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	/* [한국어] devm 할당의 주인 */
	struct device *dev = pci->dev;

	INIT_LIST_HEAD(&ep->func_list);
	/* [한국어] MSI 창 재사용 상태를 초기화. 아직 창을 잡지 않았음을 뜻한다 */
	ep->msi_iatu_mapped = false;
	/* [한국어] 기록해 둔 목적지 주소도 비운다 */
	ep->msi_msg_addr = 0;
	/* [한국어] 기록해 둔 창 크기도 비운다 */
	ep->msi_map_size = 0;

	/* [한국어] EPC 디바이스를 만들며 이 파일의 콜백 테이블을 등록한다. 이 순간부터 EPF 프레임워크가 이 컨트롤러를 볼 수 있다 */
	epc = devm_pci_epc_create(dev, &epc_ops);
	/* [한국어] IS_ERR 로 검사하는 것은 이 API 가 실패를 오류 포인터로 돌려주기 때문 */
	if (IS_ERR(epc)) {
		/* [한국어] EPC 생성 실패 */
		dev_err(dev, "Failed to create epc device\n");
		return PTR_ERR(epc);
	}

	/* [한국어] 만들어진 EPC 를 ep 에 걸어 둔다 — 이 방향은 ep 에서 epc 를 찾는 길 */
	ep->epc = epc;
	/* [한국어] 반대 방향 — 콜백들이 epc_get_drvdata() 로 ep 를 되찾는 길. 두 포인터가 서로를 가리킨다 */
	epc_set_drvdata(epc, ep);

	/* [한국어] DT 에서 주소 공간과 함수 개수를 읽는다. epc->max_functions 를 채우므로 epc 생성 뒤여야 한다 */
	ret = dw_pcie_ep_get_resources(ep);
	/* [한국어] 자원 획득 실패 */
	if (ret)
		return ret;

	/* [한국어] 글루가 레지스터 접근 전에 해 둘 일이 있는지 */
	if (ep->ops->pre_init)
		ep->ops->pre_init(ep);

	/* [한국어] addr_space 구간을 EPC 메모리 할당자로 등록한다. 이후 map_addr 이 쓸 로컬 주소가 여기서 나온다 */
	ret = pci_epc_mem_init(epc, ep->phys_base, ep->addr_size,
			       ep->page_size);
	/* [한국어] 할당자 초기화 실패 */
	if (ret < 0) {
		/* [한국어] 주소 공간을 못 쓰면 호스트 메모리에 접근할 방법이 없다 */
		dev_err(dev, "Failed to initialize address space\n");
		return ret;
	}

	/* [한국어] 인터럽트 발사 전용으로 한 페이지를 미리 떼어 둔다. 인터럽트 경로에서 할당이 실패하면
	 * 복구할 방법이 없으므로 여기서 확보해 두는 것이다 */
	ep->msi_mem = pci_epc_mem_alloc_addr(epc, &ep->msi_mem_phys,
					     epc->mem->window.page_size);
	/* [한국어] 예약 실패 */
	if (!ep->msi_mem) {
		ret = -ENOMEM;
		/* [한국어] MSI/MSI-X 를 낼 수 없는 상태이므로 초기화를 중단한다 */
		dev_err(dev, "Failed to reserve memory for MSI/MSI-X\n");
		goto err_exit_epc_mem;
	}

	return 0;

err_exit_epc_mem:
	pci_epc_mem_exit(epc);

	return ret;
}
EXPORT_SYMBOL_GPL(dw_pcie_ep_init);
