// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe endpoint driver for Renesas R-Car SoCs
 *  Copyright (c) 2020 Renesas Electronics Europe GmbH
 *
 * Author: Lad Prabhakar <prabhakar.mahadev-lad.rj@bp.renesas.com>
 */

/*
 * [한국어 설명] 같은 R-Car PCIe 컨트롤러를 엔드포인트로 모는 드라이버 (pcie-rcar-ep.c)
 *
 * === 파일의 역할 ===
 * pcie-rcar-host.c 와 똑같은 하드웨어를 정반대 역할로 쓴다. 호스트 판이
 * 버스를 만들어 남의 장치를 열거한다면, 이쪽은 R-Car 보드 자신이 남의
 * PCIe 버스에 꽂히는 "장치" 가 되게 한다. 상대편 호스트가 보면 이 보드는
 * BAR 를 가진 평범한 PCIe 엔드포인트로 보인다.
 *
 * 하드웨어를 가르는 것은 단 한 줄이다. rcar_pcie_ep_hw_init() 이 PCIEMSR 에
 * 0 을 쓰고, 호스트 판의 rcar_pcie_hw_init() 은 같은 자리에 1 을 쓴다.
 *
 * 이 드라이버는 커널의 PCI 엔드포인트 프레임워크(drivers/pci/endpoint/)에
 * 붙는다. 그 프레임워크는 "무엇을 흉내 낼지"(EPF, 기능 드라이버)와
 * "어떤 하드웨어로 흉내 낼지"(EPC, 컨트롤러 드라이버)를 분리하는데,
 * 이 파일이 EPC 쪽이다. 그래서 이 파일의 핵심은 struct pci_epc_ops
 * 콜백 열한 개이며, 나머지는 그 콜백을 받쳐 주는 자원 관리다.
 *
 * 엔드포인트가 갖춰야 할 것들을 콜백이 하나씩 담당한다.
 *   - 신원(vendor/device ID, 클래스 코드)      : write_header
 *   - BAR 와 그 뒤의 메모리                     : set_bar / clear_bar
 *   - 호스트 메모리로 나가는 창                  : map_addr / unmap_addr
 *   - 인터럽트를 호스트로 보내기                 : raise_irq, set_msi, get_msi
 *   - 링크 켜고 끄기                            : start / stop
 *   - 이 하드웨어의 제약 알리기                  : get_features
 *
 * === 전체 아키텍처에서의 위치 ===
 * DT 에 "renesas,rcar-gen3-pcie-ep" 같은 compatible 이 있으면 플랫폼 버스가
 * 이 드라이버를 붙인다.
 *
 *   builtin_platform_driver()
 *     -> [이 파일] rcar_pcie_ep_probe()
 *        -> rcar_pcie_ep_get_pdata()   : 레지스터 블록과 바깥 창 자원을 DT 에서
 *        -> devm_pci_epc_create()      : EPC 객체를 만들며 아래 ops 를 등록
 *        -> rcar_pcie_ep_hw_init()     : 엔드포인트 모드로 세우고 config 를 꾸민다
 *        -> pci_epc_multi_mem_init()   : 바깥 창들을 EPC 메모리 할당자에 넘긴다
 *        -> pci_epc_init_notify()      : 준비됐음을 EPF 쪽에 알린다
 *
 * 그 뒤로는 EPC 코어가 콜백을 부른다. 아래 라인 번호는 모두
 * drivers/pci/endpoint/pci-epc-core.c 기준으로 grep 해 확인한 것이다.
 *   write_header  <- pci_epc_write_header():1111  가 :1143 에서 호출
 *   set_bar       <- pci_epc_set_bar():981        가 :1054 에서 호출
 *   clear_bar     <- pci_epc_clear_bar():947      가 :967  에서 호출
 *   set_msi       <- pci_epc_set_msi():601        가 :627  에서 호출
 *   get_msi       <- pci_epc_get_msi():558        가 :578  에서 호출
 *   map_addr      <- pci_epc_map_addr():757       가 :783  에서 호출
 *   unmap_addr    <- pci_epc_unmap_addr():725     가 :741  에서 호출
 *   raise_irq     <- pci_epc_raise_irq():459      가 :484  에서 호출
 *   start         <- pci_epc_start():421          가 :442  에서 호출
 *   stop          <- pci_epc_stop():396           가 :410  에서 호출
 *   get_features  <- pci_epc_get_features():361   가 :381  에서 호출
 *
 * 실행 컨텍스트: probe 와 모든 콜백이 프로세스 컨텍스트다. 안쪽에서
 * rcar_pcie_wait_for_phyrdy() / rcar_pcie_wait_for_dl() 이 잠들 수 있다.
 * 이 드라이버에는 인터럽트 핸들러가 없다 — 호스트 판과 달리 인터럽트를
 * 받는 쪽이 아니라 보내는 쪽이기 때문이다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: drivers/pci/endpoint/pci-epc-core.c 가 이 파일의 ops 를 부르고,
 *   그 위에서 EPF 드라이버(drivers/pci/endpoint/functions/)가 실제로
 *   무엇을 흉내 낼지 정한다.
 * 아래쪽: pcie-rcar.c 의 공통 헬퍼를 호스트 판과 함께 쓴다 -
 *   rcar_pci_read_reg()/write_reg()(pcie-rcar.c:13,18), rcar_rmw32()(:23),
 *   rcar_pcie_wait_for_phyrdy()(:32), rcar_pcie_wait_for_dl()(:44),
 *   rcar_pcie_set_outbound()(:58), rcar_pcie_set_inbound()(:95).
 *   Makefile:12 가 pcie-rcar.o 와 pcie-rcar-ep.o 를 함께 빌드한다.
 * 옆쪽: pcie-rcar-host.c 가 같은 하드웨어의 호스트 판이다. 두 드라이버는
 *   같은 레지스터 정의(pcie-rcar.h)를 공유하지만 동시에 붙을 수는 없다 -
 *   하드웨어가 한 번에 한 모드로만 동작하기 때문이다.
 * 공유 상태: struct rcar_pcie(pcie-rcar.h)를 이 파일의
 *   struct rcar_pcie_endpoint 가 감싸 창 관리 상태를 덧붙인다.
 *
 * === 주요 함수/구조체 요약 ===
 * rcar_pcie_ep_probe()       : 진입점. 자원을 모으고 EPC 를 만들어 등록한다.
 * rcar_pcie_ep_hw_init()     : 엔드포인트 모드로 세우고 config space 를 꾸민다.
 *                              PCIEMSR 에 0 을 쓰는 것이 호스트 판과의 갈림길이다.
 * rcar_pcie_ep_write_header(): 상대 호스트가 볼 신원 정보를 config 에 써 넣는다.
 * rcar_pcie_ep_set_bar()     : BAR 하나를 안쪽 창에 연결한다. 64비트 BAR 만 쓰므로
 *                              창을 두 칸씩 잡는다.
 * rcar_pcie_ep_map_addr()    : 호스트 메모리로 나가는 바깥 창을 연다.
 * rcar_pcie_ep_raise_irq()   : INTx 또는 MSI 를 호스트로 보낸다.
 * rcar_pcie_ep_assert_intx() : INTx 를 1ms 동안 세웠다 내린다.
 * rcar_pcie_ep_assert_msi()  : MSI 벡터 번호를 송신 레지스터에 써서 보낸다.
 * rcar_pcie_epc_ops          : 위 콜백들을 EPC 코어에 노출하는 표.
 * rcar_pcie_epc_features     : 이 하드웨어의 BAR 제약(고정 크기, 64비트 전용).
 * struct rcar_pcie_endpoint  : 창 상태와 BAR-창 대응표를 담은 장치별 상태.
 *
 * === NVMe 관점 ===
 * drivers/nvme/ 에서 이 파일의 심볼을 쓰는 곳은 없다(전수 grep 확인).
 * 관계가 성립할 수 없는 구조다 - drivers/nvme/host/ 는 NVMe 장치를
 * "쓰는" 쪽이고, 이 파일은 R-Car 보드를 아무 장치로나 "보이게" 만드는
 * 쪽이라 방향이 서로 반대다.
 *
 * 이론적으로는 EPF 드라이버가 NVMe 컨트롤러를 흉내 내면 이 하드웨어 위에서
 * NVMe 장치 노릇을 할 수 있겠지만, 이 트리의
 * drivers/pci/endpoint/functions/ 에 그런 기능 드라이버가 있는지는
 * 확인하지 않았으므로 단정하지 않는다. 아래 rcar_pcie_epc_features 가
 * 알리는 BAR 크기(128/256/256바이트)는 NVMe 컨트롤러 레지스터 맵을
 * 담기에는 작다는 점만 사실로 적어 둔다.
 *
 * (주의: 공유 헤더 pcie-rcar.h 에는 레지스터마다 NVMe 를 언급하는 한 줄
 *  주석이 13개 남아 있으나 원본 스냅숏에는 그 자리에 주석이 없다.
 *  앞선 작업이 넣은 주석의 꼬리만 남은 잔재이므로 근거로 삼지 않았고,
 *  이 파일의 레지스터 설명은 pcie-rcar.h 의 #define 값과 pcie-rcar.c 의
 *  실제 사용 코드만을 근거로 적었다.)
 */

/* [한국어] delay.h — usleep_range(). INTx 를 세웠다 내리는 1ms 지연에 쓴다. */
#include <linux/delay.h>
/* [한국어] of_address.h — of_address_to_resource(). DT 0번 자원에서 레지스터 블록을 얻는다. */
#include <linux/of_address.h>
/* [한국어] of_platform.h — DT 기반 플랫폼 장치 헬퍼. */
#include <linux/of_platform.h>
/* [한국어] pci.h — PCI_EXP_* / PCI_HEADER_TYPE_* 등 규격 상수와 enum pci_barno. */
#include <linux/pci.h>
/* [한국어] pci-epc.h — 이 파일의 핵심 인터페이스. struct pci_epc_ops,
 * struct pci_epc_features, epc_get_drvdata(), devm_pci_epc_create(),
 * pci_epc_multi_mem_init(), pci_epc_init_notify() 가 전부 여기서 온다. */
#include <linux/pci-epc.h>
/* [한국어] platform_device.h — platform_driver, platform_get_resource_byname(). */
#include <linux/platform_device.h>
/* [한국어] pm_runtime.h — 런타임 PM. probe 에서 전원과 클럭을 붙잡는다. */
#include <linux/pm_runtime.h>

/* [한국어] pcie-rcar.h — host 판과 공유하는 레지스터 정의와 저수준 헬퍼 선언.
 * MAX_NR_INBOUND_MAPS(6), RCAR_PCI_MAX_RESOURCES(4), MSICAP0_* 가 여기 있다. */
#include "pcie-rcar.h"

/* [한국어] 이 하드웨어가 지원하는 물리 기능 수. DT 의 max-functions 가 이 값을
 * 넘으면 rcar_pcie_ep_get_pdata() 가 여기로 깎는다. */
#define RCAR_EPC_MAX_FUNCTIONS		1

/* Structure representing the PCIe interface */
/* [한국어] 이 드라이버의 장치별 상태 전부. */
struct rcar_pcie_endpoint {
	/* [한국어] host 판과 공유하는 최소 구조체(pcie-rcar.h). dev 와 레지스터 기준 주소를 담는다.
	 * 설정자: rcar_pcie_ep_probe() 가 dev 를, rcar_pcie_ep_get_pdata() 가 base 를 채운다.
	 * 읽는 자: 이 파일의 거의 모든 함수.
	 * 값 범위: base 는 ioremap 된 유효 주소.
	 * 동기화: probe 에서 설정된 뒤 읽기 전용이다.
	 * 첫 필드라 &ep->pcie 와 ep 의 주소가 같다. */
	struct rcar_pcie	pcie;
	/* [한국어] 바깥 창마다 현재 걸려 있는 보드 쪽 물리 주소를 기록하는 배열.
	 * 설정자: rcar_pcie_ep_map_addr() 가 걸고, rcar_pcie_ep_unmap_addr() 가 0 으로 지운다.
	 * 읽는 자: unmap 이 주소로 창 번호를 역조회할 때.
	 * 값 범위: 유효한 물리 주소 또는 0(비어 있음).
	 * 동기화: EPC 코어가 콜백을 직렬화하는 것을 전제하며, 이 드라이버에는 락이 없다. */
	phys_addr_t		*ob_mapped_addr;
	/* [한국어] 바깥 창들의 물리 주소·크기·할당 단위.
	 * 설정자: rcar_pcie_parse_outbound_ranges() 가 DT 에서 채운다.
	 * 읽는 자: rcar_pcie_ep_get_window() 와 pci_epc_multi_mem_init().
	 * 값 범위: RCAR_PCI_MAX_RESOURCES(4)개짜리 배열.
	 * 동기화: probe 에서 채워진 뒤 읽기 전용이다. */
	struct pci_epc_mem_window *ob_window;
	/* [한국어] 이 EPC 가 지원할 물리 기능 수.
	 * 설정자: rcar_pcie_ep_get_pdata() 가 DT 값을 읽어 1 이하로 깎는다.
	 * 읽는 자: probe 가 epc->max_functions 에 옮긴다.
	 * 값 범위: 1(RCAR_EPC_MAX_FUNCTIONS).
	 * 동기화: probe 에서만 설정된다. */
	u8			max_functions;
	/* [한국어] BAR 번호 -> 안쪽 창 번호 대응표.
	 * 설정자: rcar_pcie_ep_set_bar().
	 * 읽는 자: rcar_pcie_ep_clear_bar() 가 창을 되찾을 때.
	 * 값 범위: 0..MAX_NR_INBOUND_MAPS-1.
	 * 동기화: 없다. EPC 코어의 직렬화를 전제한다.
	 * 배열 크기가 BAR 수가 아니라 창 수(6)로 잡혀 있는데, 인덱스가 BAR 번호라
	 * 의미상으로는 BAR 개수가 맞다. 다만 6 이 실제 BAR 번호 범위를 덮으므로
	 * 동작에는 문제가 없다. */
	unsigned int		bar_to_atu[MAX_NR_INBOUND_MAPS];
	/* [한국어] 안쪽 창 사용 여부 비트맵.
	 * 설정자: set_bar 가 두 칸을 세우고, clear_bar 가 두 칸을 비운다.
	 * 읽는 자: set_bar 가 find_first_zero_bit 으로 빈 자리를 찾을 때.
	 * 값 범위: 비트 0..num_ib_windows-1.
	 * 동기화: 없다. EPC 코어의 직렬화를 전제한다. */
	unsigned long		*ib_window_map;
	/* [한국어] 안쪽 창 개수.
	 * 설정자: rcar_pcie_ep_probe() 가 MAX_NR_INBOUND_MAPS(6)로 고정한다.
	 * 읽는 자: set_bar 의 범위 검사와 비트맵 할당 크기.
	 * 값 범위: 6.
	 * 동기화: probe 에서만 설정된다. */
	u32			num_ib_windows;
	/* [한국어] 바깥 창 개수.
	 * 설정자: rcar_pcie_parse_outbound_ranges() 가 실제로 등록한 수로 채운다.
	 * 읽는 자: get_window 의 탐색 범위, unmap 의 탐색 범위, 배열 할당 크기,
	 *   pci_epc_multi_mem_init() 의 인자.
	 * 값 범위: 성공하면 RCAR_PCI_MAX_RESOURCES(4).
	 * 동기화: probe 에서만 설정된다. */
	u32			num_ob_windows;
};

/* [한국어]
 * rcar_pcie_ep_hw_init - 컨트롤러를 엔드포인트 모드로 세우고 config 를 꾸민다
 *
 * @pcie: 컨트롤러.   @return: 없음.
 *
 * 호스트 판 rcar_pcie_hw_init() 의 거울이다. 같은 레지스터를 만지지만
 * 목적이 반대라 값이 달라진다.
 *
 *   1) PCIETCTLR 에 0 - 진행 중인 것을 멈춘다.
 *   2) PCIEMSR 에 0 - 이 한 줄이 엔드포인트 모드로 만든다. 호스트 판이
 *      같은 자리에 1 을 쓰는 것과 정확히 대비되며, 두 드라이버의 유일한
 *      본질적 차이다.
 *   3) PCIe capability 를 만든다 - capability ID, 포트 종류를 Endpoint 로,
 *      헤더 종류를 normal(브리지가 아닌 일반 장치)로. 호스트 판이 각각
 *      Root Port 와 bridge 로 적는 자리다.
 *   4) 물리 슬롯 번호 0.
 *   5) MPSS(최대 페이로드 지원)를 128바이트 고정으로 알린다. EXPCAP(1) 의
 *      하위 3비트를 0 으로 지우는데, PCIe 규격에서 그 필드의 0 이 128바이트를
 *      뜻한다. 상류 주석이 의도를 적어 두었다.
 *   6) 읽기 요청 크기와 페이로드 크기도 128바이트로 못박는다. EXPCAP(2) 의
 *      14:12 와 7:5 비트를 지운다.
 *   7) 목표 링크 속도를 5.0GT/s 로.
 *   8) 완료 타이머 상한 50ms, capability 목록 종료.
 *   9) wmb() 로 앞선 쓰기들이 순서대로 도달하도록 막는다.
 *
 * 호스트 판과 달리 링크 훈련을 시작하지 않는다. 엔드포인트는 링크를 언제
 * 켤지 EPF 쪽이 정하며, 그 지시가 rcar_pcie_ep_start() 로 들어온다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  rcar_pcie_ep_probe() → [이 함수] → rcar_rmw32() / rcar_pci_write_reg()
 */
static void rcar_pcie_ep_hw_init(struct rcar_pcie *pcie)
{
	/* [한국어] EXPCAP 워드를 읽고-고쳐-쓸 임시 변수. */
	u32 val;

	/* [한국어] 진행 중인 것을 멈추고 초기화를 시작한다. */
	rcar_pci_write_reg(pcie, 0, PCIETCTLR);

	/* Set endpoint mode */
	/* [한국어] 이 한 줄이 하드웨어를 엔드포인트로 만든다. host 판이 같은 자리에 1 을
	 * 쓰는 것과 정확히 대비되며, 두 드라이버의 유일한 본질적 차이다. */
	rcar_pci_write_reg(pcie, 0, PCIEMSR);

	/* Initialize default capabilities. */
	/* [한국어] capability ID 를 PCIe 로 적어 capability 목록을 시작한다. */
	rcar_rmw32(pcie, REXPCAP(0), 0xff, PCI_CAP_ID_EXP);
	/* [한국어] 포트 종류 필드를 고친다. */
	rcar_rmw32(pcie, REXPCAP(PCI_EXP_FLAGS),
		   /* [한국어] Endpoint 로 적는다. host 판이 Root Port 를 적는 자리다.
		    * 4비트 미는 것은 PCI_EXP_FLAGS 안에서 Type 필드가 그 자리이기 때문이다. */
		   PCI_EXP_FLAGS_TYPE, PCI_EXP_TYPE_ENDPOINT << 4);
	/* [한국어] 헤더 종류 필드를 고친다. */
	rcar_rmw32(pcie, RCONF(PCI_HEADER_TYPE), PCI_HEADER_TYPE_MASK,
		   /* [한국어] normal(브리지가 아닌 일반 장치)로 적는다. host 판이 bridge 를 적는 자리다. */
		   PCI_HEADER_TYPE_NORMAL);

	/* Write out the physical slot number = 0 */
	/* [한국어] 물리 슬롯 번호를 0 으로. */
	rcar_rmw32(pcie, REXPCAP(PCI_EXP_SLTCAP), PCI_EXP_SLTCAP_PSN, 0);

	/* [한국어] EXPCAP(1) 은 Device Capabilities 워드 자리다. */
	val = rcar_pci_read_reg(pcie, EXPCAP(1));
	/* device supports fixed 128 bytes MPSS */
	/* [한국어] 하위 3비트가 MPSS(최대 페이로드 지원) 필드다. 0 으로 지우면 규격상
	 * 128바이트를 뜻한다 — 상류 주석이 그 의도를 적어 두었다. */
	val &= ~GENMASK(2, 0);
	/* [한국어] 고친 값을 되쓴다. */
	rcar_pci_write_reg(pcie, val, EXPCAP(1));

	/* [한국어] EXPCAP(2) 는 Device Control 워드 자리다. */
	val = rcar_pci_read_reg(pcie, EXPCAP(2));
	/* read requests size 128 bytes */
	/* [한국어] 14:12 가 최대 읽기 요청 크기 필드다. 0 이 128바이트다. */
	val &= ~GENMASK(14, 12);
	/* payload size 128 bytes */
	/* [한국어] 7:5 가 최대 페이로드 크기 필드다. 역시 0 이 128바이트다. */
	val &= ~GENMASK(7, 5);
	/* [한국어] 두 필드를 함께 고친 값을 되쓴다. */
	rcar_pci_write_reg(pcie, val, EXPCAP(2));

	/* Set target link speed to 5.0 GT/s */
	/* [한국어] 목표 링크 속도를 5.0GT/s 로 지정한다. host 판의 force_speedup 과
	 * 같은 레지스터를 쓰지만, 이쪽은 속도 변경을 개시하지 않고 목표만 적어 둔다. */
	rcar_rmw32(pcie, EXPCAP(12), PCI_EXP_LNKSTA_CLS,
		   PCI_EXP_LNKSTA_CLS_5_0GB);

	/* Set the completion timer timeout to the maximum 50ms. */
	/* [한국어] 완료 타이머 상한을 50ms 로. TLCTLR + 1 로 한 바이트 건너뛰는 것은
	 * rcar_rmw32() 가 오프셋 하위 2비트로 바이트 자리를 계산해 주기 때문이다. */
	rcar_rmw32(pcie, TLCTLR + 1, 0x3f, 50);

	/* Terminate list of capabilities (Next Capability Offset=0) */
	/* [한국어] capability 목록을 여기서 끝낸다(Next Capability Offset 을 0 으로). */
	rcar_rmw32(pcie, RVCCAP(0), 0xfff00000, 0);

	/* flush modifications */
	/* [한국어] 앞선 쓰기들이 순서대로 하드웨어에 도달하도록 막는다. 상류 주석대로
	 * "수정 사항 flush" 다. */
	wmb();
}

/* [한국어]
 * rcar_pcie_ep_get_window - 물리 주소로 바깥 창 번호를 찾는다
 *
 * @ep:   엔드포인트 상태.
 * @addr: 찾는 창의 물리 시작 주소.
 * @return: 창 번호(0 이상), 없으면 -EINVAL.
 *
 * 바깥 창들의 물리 주소는 DT 에 고정되어 있고 EPC 코어는 그중 하나를
 * 골라 주소로 넘겨 온다. 그 주소가 몇 번 창인지 되찾는 역조회다.
 *
 * 선형 탐색인데 창이 최대 RCAR_PCI_MAX_RESOURCES(4)개뿐이라 문제가 되지 않는다.
 *
 * phys_base 와 정확히 같은지만 본다. EPC 코어가 창 경계에 맞춰 주소를
 * 넘겨 주는 것을 전제하므로, 창 안쪽의 임의 주소로는 찾지 못한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  rcar_pcie_ep_map_addr() → [이 함수]
 */
static int rcar_pcie_ep_get_window(struct rcar_pcie_endpoint *ep,
				   phys_addr_t addr)
{
	/* [한국어] 창 배열 반복자. */
	int i;

	/* [한국어] 등록된 바깥 창을 처음부터 훑는다. 최대 4개뿐이라 선형 탐색으로 충분하다. */
	for (i = 0; i < ep->num_ob_windows; i++)
		/* [한국어] 물리 시작 주소가 정확히 일치하는 창을 찾는다. EPC 코어가 창 경계에 맞춰
		 * 주소를 넘겨 주는 것을 전제하므로, 창 안쪽의 임의 주소로는 찾지 못한다. */
		if (ep->ob_window[i].phys_base == addr)
			/* [한국어] 창 번호를 돌려준다. */
			return i;

	/* [한국어] 없으면 인자 오류로 답한다. */
	return -EINVAL;
}

/* [한국어]
 * rcar_pcie_parse_outbound_ranges - DT 의 memoryN 자원들을 바깥 창으로 등록한다
 *
 * @ep:   엔드포인트 상태.
 * @pdev: 플랫폼 장치.
 * @return: 0 성공. -EINVAL 은 자원이 없는 경우, -EIO 는 영역 예약 실패.
 *
 * "memory0" 부터 "memory3" 까지 이름으로 자원을 찾아 창 배열을 채운다.
 * RCAR_PCI_MAX_RESOURCES(4)개를 *모두* 요구하며, 하나라도 없으면 실패한다.
 *
 * devm_request_mem_region() 으로 각 구간을 예약하는 것이 중요하다. 이
 * 주소들은 CPU 쪽에서 보이는 창이므로, 다른 드라이버가 같은 물리 주소를
 * 잡으면 충돌한다.
 *
 * page_size 를 창 크기와 같게 두는 대목에 상류 주석이 붙어 있다 - 이
 * 컨트롤러는 한 창에서 여러 번 나눠 할당하는 것을 지원하지 않으므로,
 * 할당 단위를 창 전체로 만들어 EPC 메모리 할당자가 한 창당 하나씩만
 * 내주게 한다.
 *
 * 반환값이 무시된다는 점을 짚어 둔다. 유일한 호출자 rcar_pcie_ep_get_pdata()
 * 가 이 함수를 부르면서 결과를 확인하지 않아, 자원이 없어도 probe 가
 * 계속 진행된다. 그러면 num_ob_windows 가 0 이 되어 뒤의
 * devm_kcalloc(0) 과 pci_epc_multi_mem_init(0) 으로 이어진다.
 * 코드는 고치지 않고 관찰만 적어 둔다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  rcar_pcie_ep_get_pdata() → [이 함수]
 *               → platform_get_resource_byname() → devm_request_mem_region()
 */
static int rcar_pcie_parse_outbound_ranges(struct rcar_pcie_endpoint *ep,
					   struct platform_device *pdev)
{
	/* [한국어] 로그용으로 쓸 공통 구조체. */
	struct rcar_pcie *pcie = &ep->pcie;
	/* [한국어] "memory0" 같은 이름을 조립할 버퍼. 최대 "memory3" 이라 10바이트면 넉넉하다. */
	char outbound_name[10];
	/* [한국어] 찾은 자원. */
	struct resource *res;
	/* [한국어] 창 반복자. 루프 뒤에도 값이 남아 num_ob_windows 에 쓰인다. */
	unsigned int i = 0;

	/* [한국어] 실패 시 0 이 남도록 먼저 지워 둔다. */
	ep->num_ob_windows = 0;
	/* [한국어] RCAR_PCI_MAX_RESOURCES(4)개를 모두 요구한다. */
	for (i = 0; i < RCAR_PCI_MAX_RESOURCES; i++) {
		/* [한국어] 이름으로 찾을 것이므로 인덱스를 문자열에 넣는다. */
		sprintf(outbound_name, "memory%u", i);
		/* [한국어] DT 에서 그 이름의 메모리 자원을 찾는다. */
		res = platform_get_resource_byname(pdev,
						   IORESOURCE_MEM,
						   outbound_name);
		/* [한국어] 하나라도 없으면 */
		if (!res) {
			/* [한국어] 어느 창인지 남기고 */
			dev_err(pcie->dev, "missing outbound window %u\n", i);
			/* [한국어] 인자 오류로 돌린다. */
			return -EINVAL;
		}
		/* [한국어] 그 물리 구간을 예약한다. 이 주소들은 CPU 쪽에서 보이는 창이라, */
		if (!devm_request_mem_region(&pdev->dev, res->start,
					     resource_size(res),
					     /* [한국어] 다른 드라이버가 같은 물리 주소를 잡으면 충돌한다. */
					     res->name)) {
			/* [한국어] 예약 실패를 남기고 */
			dev_err(pcie->dev, "Cannot request memory region %s.\n",
				outbound_name);
			/* [한국어] 입출력 오류로 돌린다. */
			return -EIO;
		}

		/* [한국어] 창의 물리 시작 주소. */
		ep->ob_window[i].phys_base = res->start;
		/* [한국어] 창 크기. */
		ep->ob_window[i].size = resource_size(res);
		/* controller doesn't support multiple allocation
		 * from same window, so set page_size to window size
		 */
		/* [한국어] 할당 단위를 창 크기와 같게 둔다. 상류 주석대로 이 컨트롤러는 한 창에서
		 * 여러 번 나눠 할당하는 것을 지원하지 않으므로, 이렇게 해야 EPC 메모리
		 * 할당자가 한 창당 하나씩만 내준다. */
		ep->ob_window[i].page_size = resource_size(res);
	}
	/* [한국어] 루프를 끝까지 돌았으면 i 가 곧 등록한 창 수다. */
	ep->num_ob_windows = i;

	/* [한국어] 성공. 다만 유일한 호출자가 이 반환값을 확인하지 않는다는 점을
	 * 함수 블록 주석에 적어 두었다. */
	return 0;
}

/* [한국어]
 * rcar_pcie_ep_get_pdata - DT 에서 레지스터 블록과 창, 기능 수를 받아 온다
 *
 * @ep:   엔드포인트 상태.
 * @pdev: 플랫폼 장치.
 * @return: 0 성공, 음수 errno 실패.
 *
 * probe 가 필요한 것을 한 곳에서 모은다.
 *   - 0번 자원을 ioremap 해 pcie->base 로 삼는다. 호스트 판과 같은 방식이다.
 *   - 바깥 창 배열을 RCAR_PCI_MAX_RESOURCES 개만큼 할당하고
 *     rcar_pcie_parse_outbound_ranges() 로 채운다.
 *   - DT 의 "max-functions" 를 읽는다. 읽기에 실패했거나 값이
 *     RCAR_EPC_MAX_FUNCTIONS(1)을 넘으면 1 로 깎는다. 이 하드웨어가
 *     물리 기능을 하나만 지원하기 때문이다.
 *
 * max_functions 의 판정이 한 줄에 두 조건을 묶은 형태라, 속성이 없을 때와
 * 값이 과할 때가 같은 결과로 수렴한다 - 어느 쪽이든 1 이 안전한 기본값이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  rcar_pcie_ep_probe() → [이 함수]
 *               → devm_ioremap_resource() → rcar_pcie_parse_outbound_ranges()
 */
static int rcar_pcie_ep_get_pdata(struct rcar_pcie_endpoint *ep,
				  struct platform_device *pdev)
{
	/* [한국어] 레지스터 접근과 로그에 쓸 공통 구조체. */
	struct rcar_pcie *pcie = &ep->pcie;
	/* [한국어] 아래 devm_kcalloc 의 원소 크기를 구하는 데만 쓰는 선언이다. */
	struct pci_epc_mem_window *window;
	/* [한국어] devm 할당의 주인. */
	struct device *dev = pcie->dev;
	/* [한국어] DT 0번 자원. */
	struct resource res;
	/* [한국어] 하위 호출 결과. */
	int err;

	/* [한국어] 레지스터 블록의 물리 주소를 얻는다. */
	err = of_address_to_resource(dev->of_node, 0, &res);
	/* [한국어] 실패하면 */
	if (err)
		/* [한국어] 진행할 수 없다. */
		return err;
	pcie->base = devm_ioremap_resource(dev, &res);
	if (IS_ERR(pcie->base))
		return PTR_ERR(pcie->base);

	ep->ob_window = devm_kcalloc(dev, RCAR_PCI_MAX_RESOURCES,
				     /* [한국어] 창 배열을 RCAR_PCI_MAX_RESOURCES(4)개만큼 잡는다. window 는 원소 크기를
				      * 구하려고 선언해 둔 변수다. */
				     sizeof(*window), GFP_KERNEL);
	if (!ep->ob_window)
		/* [한국어] 메모리 부족. */
		return -ENOMEM;

	rcar_pcie_parse_outbound_ranges(ep, pdev);
/* [한국어] DT 의 memoryN 자원들로 창 배열을 채운다. 이 호출의 반환값을 확인하지
 * 않으므로 자원이 없어도 probe 가 계속 진행된다 — 함수 블록 주석에 적어 두었다. */

	err = of_property_read_u8(dev->of_node, "max-functions",
				  /* [한국어] DT 의 max-functions 를 읽는다. */
				  &ep->max_functions);
	if (err < 0 || ep->max_functions > RCAR_EPC_MAX_FUNCTIONS)
		/* [한국어] 읽기에 실패했거나 값이 과하면 1 로 깎는다(윗줄의 두 조건).
		 * 이 하드웨어가 물리 기능을 하나만 지원하기 때문이다. */
		ep->max_functions = RCAR_EPC_MAX_FUNCTIONS;

	return 0;
}

/* [한국어]
 * rcar_pcie_ep_write_header - 상대 호스트가 볼 신원 정보를 config space 에 쓴다
 *
 * @epc: EPC 객체.   @fn: 물리 기능 번호.   @vfn: 가상 기능 번호(쓰지 않는다).
 * @hdr: EPF 가 정한 헤더 값들.
 * @return: 0 성공, -EINVAL 은 지원하지 않는 인터럽트 핀.
 *
 * 상대편 호스트가 이 보드를 열거할 때 읽어 갈 값들을 미리 써 두는 콜백이다.
 * EPF 드라이버가 "나는 이런 장치다" 라고 정한 것을 하드웨어에 새긴다.
 *
 * 레지스터가 세 개로 나뉜다.
 *   IDSETR0   - vendor ID(하위 16비트) + device ID(상위 16비트)
 *   IDSETR1   - revision + prog-if + subclass + baseclass 를 바이트별로 쌓는다
 *   SUBIDSETR - subsystem vendor ID + subsystem ID
 *
 * fn 이 0 이 아닐 때 vendor ID 와 subsystem vendor ID 를 새로 쓰지 않고
 * 기존 레지스터 값을 읽어 쓰는 대목이 눈에 띈다. 여러 기능이 vendor ID 를
 * 공유하는 구조를 반영한 것으로 보인다. 다만 이 하드웨어는
 * RCAR_EPC_MAX_FUNCTIONS 가 1 이라 fn 은 사실상 항상 0 이다.
 *
 * 인터럽트 핀은 INTA 까지만 허용한다. 그 이상은 -EINVAL 이며,
 * PCICONF(15) 의 8비트 자리에 써 넣는다.
 *
 * PCICONF(15) 쓰기가 읽고-OR-쓰기라 기존 핀 값을 지우지 않는다. 같은
 * 자리에 다른 값을 두 번 쓰면 비트가 누적된다. 코드는 고치지 않고
 * 관찰만 적어 둔다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  pci_epc_write_header() [pci-epc-core.c:1111] → :1143 → [이 함수]
 */
static int rcar_pcie_ep_write_header(struct pci_epc *epc, u8 fn, u8 vfn,
				     struct pci_epf_header *hdr)
{
	struct rcar_pcie_endpoint *ep = epc_get_drvdata(epc);
	struct rcar_pcie *pcie = &ep->pcie;
	/* [한국어] 레지스터에 조립해 넣을 값. */
	u32 val;

	if (!fn)
		/* [한국어] 기능 0 이면 EPF 가 정한 vendor ID 를 그대로 쓴다. */
		val = hdr->vendorid;
	/* [한국어] 기능 0 이 아니면 */
	else
		val = rcar_pci_read_reg(pcie, IDSETR0);
	/* [한국어] device ID 를 상위 16비트에 얹는다. IDSETR0 이 두 ID 를 한 워드에 담는다. */
	val |= hdr->deviceid << 16;
	/* [한국어] 신원의 첫 워드를 써 넣는다. */
	rcar_pci_write_reg(pcie, val, IDSETR0);

	val = hdr->revid;
	/* [한국어] prog-if 를 8비트 자리에, */
	val |= hdr->progif_code << 8;
	/* [한국어] subclass 를 16비트 자리에, */
	val |= hdr->subclass_code << 16;
	/* [한국어] baseclass 를 24비트 자리에 쌓는다. revision 이 최하위 바이트라
	 * 네 값이 한 워드를 채운다. */
	val |= hdr->baseclass_code << 24;
	/* [한국어] 클래스 코드 워드를 써 넣는다. 호스트가 이 값으로 장치 종류를 판단한다. */
	rcar_pci_write_reg(pcie, val, IDSETR1);

	if (!fn)
		/* [한국어] 기능 0 이면 EPF 가 정한 subsystem vendor ID 를 쓴다. */
		val = hdr->subsys_vendor_id;
	/* [한국어] 아니면 */
	else
		val = rcar_pci_read_reg(pcie, SUBIDSETR);
	/* [한국어] subsystem ID 를 상위 16비트에 얹는다. */
	val |= hdr->subsys_id << 16;
	/* [한국어] subsystem 신원 워드를 써 넣는다. */
	rcar_pci_write_reg(pcie, val, SUBIDSETR);

	if (hdr->interrupt_pin > PCI_INTERRUPT_INTA)
		/* [한국어] INTA 를 넘는 핀은 이 하드웨어가 지원하지 않는다. */
		return -EINVAL;
	val = rcar_pci_read_reg(pcie, PCICONF(15));
	/* [한국어] 핀 번호를 8비트 자리에 얹는다. 읽고-OR-쓰기라 기존 값을 지우지 않아,
	 * 두 번 쓰면 비트가 누적된다 — 함수 블록 주석에 적어 두었다. */
	val |= (hdr->interrupt_pin << 8);
	/* [한국어] 인터럽트 핀을 써 넣는다. */
	rcar_pci_write_reg(pcie, val, PCICONF(15));

	return 0;
}

/* [한국어]
 * rcar_pcie_ep_set_bar - BAR 하나를 안쪽 창에 연결해 호스트에 노출한다
 *
 * @epc: EPC 객체.   @func_no: 기능 번호.   @vfunc_no: 가상 기능 번호(쓰지 않는다).
 * @epf_bar: 노출할 BAR 의 번호·크기·물리 주소.
 * @return: 0 성공. -EINVAL 은 빈 창이 없거나 PHY 가 준비되지 않은 경우.
 *
 * 호스트가 이 BAR 에 접근하면 그 트래픽이 보드의 실제 메모리에 닿아야
 * 한다. 그 통로가 안쪽 창이고, 이 콜백이 둘을 잇는다.
 *
 *   1) ib_window_map 비트맵에서 빈 창을 찾는다. 없으면 실패.
 *   2) I/O 공간 BAR 이면 IO_SPACE 플래그를 더한다.
 *   3) 창을 두 칸(idx, idx+1) 잡는다. 상류 주석대로 64비트 BAR 만 쓰기
 *      때문이며, rcar_pcie_set_inbound() 가 그 한 쌍에 하위/상위 주소를
 *      나눠 쓴다(pcie-rcar.c:95).
 *   4) 크기를 정한다. 호스트 판 rcar_pcie_inbound_ranges() 와 같은 계산이다 -
 *      시작 주소의 정렬(__ffs64)과 하드웨어 상한 4GiB 로 두 번 자르고,
 *      2의 거듭제곱으로 올린 뒤 1 을 빼 마스크를 만들고 하위 4비트를 비운다.
 *   5) 안쪽 창을 쓴다. 마지막 인자 false 가 "엔드포인트 모드" 로,
 *      PCIEPRAR(호스트가 볼 주소)은 건드리지 않는다는 뜻이다 - 그 주소는
 *      상대 호스트가 BAR 에 배정해 주므로 이쪽이 정할 일이 아니다.
 *   6) PHY 준비를 확인한다.
 *
 * 에러 경로에 되돌리기가 없다는 점을 짚어 둔다. PHY 대기가 실패하면
 * -EINVAL 을 돌려주지만 3번에서 세운 두 비트와 bar_to_atu 항목은 그대로
 * 남는다. 코드는 고치지 않고 관찰만 적어 둔다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 안쪽에서 잠들 수 있다.
 *
 * 호출 체인:  pci_epc_set_bar() [pci-epc-core.c:981] → :1054 → [이 함수]
 *               → rcar_pcie_set_inbound() [pcie-rcar.c:95]
 *               → rcar_pcie_wait_for_phyrdy() [pcie-rcar.c:32]
 */
static int rcar_pcie_ep_set_bar(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
				struct pci_epf_bar *epf_bar)
{
	int flags = epf_bar->flags | LAR_ENABLE | LAM_64BIT;
	struct rcar_pcie_endpoint *ep = epc_get_drvdata(epc);
	/* [한국어] 크기를 2의 거듭제곱으로 올린다. fls64(size-1) 이 그 지수를 준다. */
	u64 size = 1ULL << fls64(epf_bar->size - 1);
	/* [한국어] BAR 뒤에 놓일 보드 쪽 메모리의 물리 주소. */
	dma_addr_t cpu_addr = epf_bar->phys_addr;
	/* [한국어] 몇 번 BAR 인지. */
	enum pci_barno bar = epf_bar->barno;
	/* [한국어] 레지스터 접근용 공통 구조체. */
	struct rcar_pcie *pcie = &ep->pcie;
	/* [한국어] 창의 크기 마스크. */
	u32 mask;
	/* [한국어] 잡을 안쪽 창 번호. */
	int idx;
	/* [한국어] PHY 대기 결과. */
	int err;

	idx = find_first_zero_bit(ep->ib_window_map, ep->num_ib_windows);
	/* [한국어] 빈 창이 없으면 */
	if (idx >= ep->num_ib_windows) {
		/* [한국어] 그 사실을 남기고 */
		dev_err(pcie->dev, "no free inbound window\n");
		/* [한국어] 인자 오류로 돌린다. */
		return -EINVAL;
	}

	if ((flags & PCI_BASE_ADDRESS_SPACE) == PCI_BASE_ADDRESS_SPACE_IO)
		/* [한국어] I/O 공간 BAR 이면 그 표시를 더한다(pcie-rcar.h 의 IO_SPACE = BIT(8)). */
		flags |= IO_SPACE;

	ep->bar_to_atu[bar] = idx;
	/* use 64-bit BARs */
	set_bit(idx, ep->ib_window_map);
	set_bit(idx + 1, ep->ib_window_map);
/* [한국어] 창을 두 칸 잡는다(윗줄과 함께). 상류 주석대로 64비트 BAR 만 쓰므로,
 * rcar_pcie_set_inbound() 가 그 한 쌍에 하위/상위 주소를 나눠 쓴다. */

	if (cpu_addr > 0) {
		/* [한국어] 시작 주소 하위의 연속된 0 비트 수가 곧 정렬 크기의 로그다. */
		unsigned long nr_zeros = __ffs64(cpu_addr);
		/* [한국어] 그만큼이 이 주소에서 쓸 수 있는 최대 창 크기다. */
		u64 alignment = 1ULL << nr_zeros;

		size = min(size, alignment);
	/* [한국어] 창 하나는 시작 주소의 정렬보다 클 수 없다. */
	}

	size = min(size, 1ULL << 32);
/* [한국어] 하드웨어 상한 4GiB 로 한 번 더 자른다(윗줄). */

	mask = roundup_pow_of_two(size) - 1;
	/* [한국어] 마스크의 하위 4비트를 비운다. 그 자리에 위 flags 가 들어가기 때문이다. */
	mask &= ~0xf;

	rcar_pcie_set_inbound(pcie, cpu_addr,
			      /* [한국어] PCI 주소로 0 을 넘기고 마지막 인자를 false 로 준다. false 가 "엔드포인트
			       * 모드" 로, PCIEPRAR(호스트가 볼 주소)은 건드리지 않는다는 뜻이다 —
			       * 그 주소는 상대 호스트가 BAR 에 배정해 주므로 이쪽이 정할 일이 아니다. */
			      0x0, mask | flags, idx, false);

	err = rcar_pcie_wait_for_phyrdy(pcie);
	/* [한국어] PHY 가 준비되지 않았으면 */
	if (err) {
		/* [한국어] 그 사실을 남기고 */
		dev_err(pcie->dev, "phy not ready\n");
		/* [한국어] 인자 오류로 돌린다. 다만 위에서 세운 두 비트와 bar_to_atu 항목은
		 * 되돌리지 않는다 — 함수 블록 주석에 적어 두었다. */
		return -EINVAL;
	}

	return 0;
}

/* [한국어]
 * rcar_pcie_ep_clear_bar - BAR 를 내리고 안쪽 창을 반납한다
 *
 * @epc: EPC 객체.   @fn: 기능 번호.   @vfn: 가상 기능 번호(쓰지 않는다).
 * @epf_bar: 내릴 BAR.
 * @return: 없음.
 *
 * set_bar 의 역이다. 창 내용을 0 으로 덮어 통로를 끊고, 비트맵에서 두 칸을
 * 비운다. bar_to_atu[bar] 에 저장해 둔 창 번호가 그 되찾기의 열쇠다.
 *
 * 다만 rcar_pcie_set_inbound() 에 넘기는 인덱스가 atu_index 가 아니라
 * bar 다. 바로 윗줄에서 bar_to_atu[bar] 로 실제 창 번호를 얻어 두고도
 * 하드웨어를 지울 때는 BAR 번호를 인덱스로 쓰고 있어, 둘이 다르면 엉뚱한
 * 창을 지우고 정작 쓰던 창은 남는다. 비트맵 해제는 atu_index 로 하므로
 * 소프트웨어 상태와 하드웨어 상태가 어긋날 수 있다.
 * 코드는 고치지 않고 관찰만 기록한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  pci_epc_clear_bar() [pci-epc-core.c:947] → :967 → [이 함수]
 *               → rcar_pcie_set_inbound() [pcie-rcar.c:95]
 */
static void rcar_pcie_ep_clear_bar(struct pci_epc *epc, u8 fn, u8 vfn,
				   struct pci_epf_bar *epf_bar)
{
	struct rcar_pcie_endpoint *ep = epc_get_drvdata(epc);
	enum pci_barno bar = epf_bar->barno;
	/* [한국어] set_bar 가 기록해 둔 창 번호를 되찾는다. */
	u32 atu_index = ep->bar_to_atu[bar];

	rcar_pcie_set_inbound(&ep->pcie, 0x0, 0x0, 0x0, bar, false);
/* [한국어] 창 내용을 0 으로 덮어 통로를 끊는다. 다만 인덱스로 atu_index 가 아니라
 * bar 를 넘기고 있어, 둘이 다르면 엉뚱한 창을 지운다 — 함수 블록 주석 참조. */

	clear_bit(atu_index, ep->ib_window_map);
	/* [한국어] 두 칸을 모두 비운다(윗줄과 함께). 64비트 BAR 라 한 쌍이었다. */
	clear_bit(atu_index + 1, ep->ib_window_map);
}

/* [한국어]
 * rcar_pcie_ep_set_msi - 이 엔드포인트가 지원하는 MSI 벡터 수를 알린다
 *
 * @epc: EPC 객체.   @fn: 기능 번호.   @vfn: 가상 기능 번호(쓰지 않는다).
 * @nr_irqs: EPF 가 원하는 벡터 수.
 * @return: 항상 0.
 *
 * MSI capability 의 MMC(Multiple Message Capable) 필드에 "나는 2^N 개까지
 * 받을 수 있다" 의 N 을 적는다. 그래서 개수를 그대로 쓰지 않고
 * order_base_2() 로 로그를 취한다.
 *
 * 이 값은 능력 표시일 뿐이고, 실제로 몇 개를 쓸지는 상대 호스트가 MMSE
 * 필드에 적어 준다. 그것을 되읽는 것이 아래 rcar_pcie_ep_get_msi() 다.
 *
 * 읽고-OR-쓰기라 기존 MMC 값을 지우지 않는다. 두 번 부르면 비트가
 * 누적되어 엉뚱한 값이 된다. 코드는 고치지 않고 관찰만 적어 둔다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  pci_epc_set_msi() [pci-epc-core.c:601] → :627 → [이 함수]
 */
static int rcar_pcie_ep_set_msi(struct pci_epc *epc, u8 fn, u8 vfn, u8 nr_irqs)
{
	struct rcar_pcie_endpoint *ep = epc_get_drvdata(epc);
	struct rcar_pcie *pcie = &ep->pcie;
	/* [한국어] MMC 필드에 넣을 로그값. "2^N 개까지 가능" 의 N 이다. */
	u8 mmc = order_base_2(nr_irqs);
	/* [한국어] 읽고-고쳐-쓸 MSI capability 워드. */
	u32 flags;

	flags = rcar_pci_read_reg(pcie, MSICAP(fn));
	/* [한국어] MMC 필드 자리로 밀어 얹는다. 읽고-OR-쓰기라 기존 값을 지우지 않아,
	 * 두 번 부르면 비트가 누적된다 — 함수 블록 주석에 적어 두었다. */
	flags |= mmc << MSICAP0_MMESCAP_OFFSET;
	/* [한국어] 고친 값을 써 넣는다. */
	rcar_pci_write_reg(pcie, flags, MSICAP(fn));

	return 0;
}

/* [한국어]
 * rcar_pcie_ep_get_msi - 상대 호스트가 배정한 MSI 벡터 수를 읽어 온다
 *
 * @epc: EPC 객체.   @fn: 기능 번호.   @vfn: 가상 기능 번호(쓰지 않는다).
 * @return: 배정된 벡터 수(2의 거듭제곱). MSI 가 켜져 있지 않으면 -EINVAL.
 *
 * set_msi 의 짝이다. 이쪽은 쓰는 것이 아니라 읽는다 - 호스트가 이 장치의
 * MSI 를 켜면서 MMSE(Multiple Message Enable) 필드에 실제 허용 개수를
 * 적어 두기 때문이다.
 *
 * 먼저 MSICAP0_MSIE(MSI Enable)를 확인한다. 꺼져 있으면 호스트가 아직
 * MSI 를 켜지 않은 것이라 -EINVAL 이다.
 *
 * 그다음 MMSE 필드를 마스크(GENMASK(22,20))로 떼어 내고 20비트 밀어 N 을
 * 얻은 뒤, 1 << N 으로 실제 개수를 만든다. 로그를 개수로 되돌리는 계산이
 * set_msi 와 정확히 대칭이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  pci_epc_get_msi() [pci-epc-core.c:558] → :578 → [이 함수]
 */
static int rcar_pcie_ep_get_msi(struct pci_epc *epc, u8 fn, u8 vfn)
{
	struct rcar_pcie_endpoint *ep = epc_get_drvdata(epc);
	struct rcar_pcie *pcie = &ep->pcie;
	/* [한국어] 읽어 올 MSI capability 워드. */
	u32 flags;

	flags = rcar_pci_read_reg(pcie, MSICAP(fn));
	/* [한국어] 호스트가 아직 MSI 를 켜지 않았으면 */
	if (!(flags & MSICAP0_MSIE))
		/* [한국어] 쓸 수 없다고 답한다. */
		return -EINVAL;

	return 1 << ((flags & MSICAP0_MMESE_MASK) >> MSICAP0_MMESE_OFFSET);
}

/* [한국어]
 * rcar_pcie_ep_map_addr - 호스트 메모리로 나가는 바깥 창을 연다
 *
 * @epc: EPC 객체.   @fn: 기능 번호.   @vfn: 가상 기능 번호(쓰지 않는다).
 * @addr:     보드 쪽 물리 주소(어느 창을 쓸지 고르는 열쇠).
 * @pci_addr: 그 창이 가리킬 호스트 쪽 PCI 주소.
 * @size:     창 크기.
 * @return: 0 성공. 링크가 없으면 그 errno, 창을 못 찾으면 -EINVAL.
 *
 * 엔드포인트가 호스트 메모리를 읽고 쓰려면 창이 필요하다. 보드의 CPU 가
 * addr 에 접근하면 그 트랜잭션이 호스트의 pci_addr 로 나가게 만드는 것이
 * 이 콜백이다.
 *
 * 먼저 링크를 확인한다. 링크가 없으면 나갈 곳이 없으므로 의미가 없다.
 * 그다음 addr 로 창 번호를 역조회하고, 임시 resource 를 하나 꾸며
 * rcar_pcie_set_outbound() 에 넘긴다.
 *
 * 임시 구조체를 memset 으로 지우고 세 필드만 채우는 방식이 눈에 띈다.
 * rcar_pcie_set_outbound() 가 resource_entry 를 받도록 되어 있어(호스트
 * 판에서는 브리지 창 목록의 항목이 그대로 들어온다) 형식을 맞춰 주는
 * 것이다. win.res 만 연결하고 offset 은 0 으로 두므로, 창의 PCI 주소가
 * 그대로 쓰인다.
 *
 * 마지막으로 어느 창에 어떤 주소를 걸었는지 기록해 둔다. unmap 이 그
 * 기록으로 창을 되찾는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 링크 대기에서 잠들 수 있다.
 *
 * 호출 체인:  pci_epc_map_addr() [pci-epc-core.c:757] → :783 → [이 함수]
 *               → rcar_pcie_wait_for_dl() [pcie-rcar.c:44]
 *               → rcar_pcie_set_outbound() [pcie-rcar.c:58]
 */
static int rcar_pcie_ep_map_addr(struct pci_epc *epc, u8 fn, u8 vfn,
				 phys_addr_t addr, u64 pci_addr, size_t size)
{
	struct rcar_pcie_endpoint *ep = epc_get_drvdata(epc);
	struct rcar_pcie *pcie = &ep->pcie;
	/* [한국어] rcar_pcie_set_outbound() 가 요구하는 형식을 맞추려고 임시로 꾸미는 항목. */
	struct resource_entry win;
	/* [한국어] 그 안에 넣을 자원. */
	struct resource res;
	/* [한국어] 찾은 창 번호. */
	int window;
	/* [한국어] 링크 대기 결과. */
	int err;

	/* check if we have a link. */
	err = rcar_pcie_wait_for_dl(pcie);
	if (err) {
		/* [한국어] 링크가 없으면 나갈 곳이 없다. */
		dev_err(pcie->dev, "link not up\n");
		/* [한국어] 그 errno 를 그대로 전한다. */
		return err;
	}

	window = rcar_pcie_ep_get_window(ep, addr);
	/* [한국어] 주소에 해당하는 창이 없으면 */
	if (window < 0) {
		/* [한국어] 그 사실을 남기고 */
		dev_err(pcie->dev, "failed to get corresponding window\n");
		/* [한국어] 인자 오류로 돌린다. */
		return -EINVAL;
	}

	memset(&win, 0x0, sizeof(win));
	/* [한국어] 임시 구조체를 지운다(윗줄과 함께). 쓰레기값이 남으면 창 설정이 망가진다. */
	memset(&res, 0x0, sizeof(res));
	/* [한국어] 이 창이 가리킬 호스트 쪽 PCI 주소. */
	res.start = pci_addr;
	/* [한국어] 끝 주소(포함). */
	res.end = pci_addr + size - 1;
	/* [한국어] 메모리 창임을 표시한다. IORESOURCE_IO 가 아니므로
	 * rcar_pcie_set_outbound() 가 IO_SPACE 를 붙이지 않는다. */
	res.flags = IORESOURCE_MEM;
	/* [한국어] 항목에 자원을 연결한다. offset 은 memset 으로 0 이라 PCI 주소가 그대로 쓰인다. */
	win.res = &res;

	rcar_pcie_set_outbound(pcie, window, &win);
/* [한국어] PCIEPALR/PAUR/PAMR/PTCTLR 네 레지스터를 채워 창을 연다(pcie-rcar.c:58). */

	ep->ob_mapped_addr[window] = addr;
/* [한국어] 어느 창에 어떤 주소를 걸었는지 기록해 둔다. unmap 이 이 기록으로 창을 되찾는다. */

	return 0;
}

/* [한국어]
 * rcar_pcie_ep_unmap_addr - 열어 둔 바깥 창을 닫는다
 *
 * @epc: EPC 객체.   @fn: 기능 번호.   @vfn: 가상 기능 번호(쓰지 않는다).
 * @addr: 닫을 창에 걸어 두었던 보드 쪽 물리 주소.
 * @return: 없음.
 *
 * map_addr 의 역이다. ob_mapped_addr 기록을 뒤져 창 번호를 찾고, 전부 0 인
 * 빈 resource 를 넘겨 창을 비운다. 찾지 못하면 조용히 돌아간다 - 이미
 * 닫혔거나 애초에 열린 적이 없다는 뜻이다.
 *
 * 창을 "지우는" 방법이 0 으로 채운 resource 를 쓰는 것이라는 점이 요령이다.
 * rcar_pcie_set_outbound() 가 맨 처음 PCIEPTCTLR 에 0 을 써서 창을 끄고,
 * 크기가 0 이라 마스크도 0 이 되며, 마지막에 다시 켜는 값도
 * res->flags 가 비어 있어 PAR_ENABLE 만 남는다.
 *
 * 기록을 0 으로 되돌려 그 자리가 비었음을 표시한다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  pci_epc_unmap_addr() [pci-epc-core.c:725] → :741 → [이 함수]
 *               → rcar_pcie_set_outbound() [pcie-rcar.c:58]
 */
static void rcar_pcie_ep_unmap_addr(struct pci_epc *epc, u8 fn, u8 vfn,
				    phys_addr_t addr)
{
	struct rcar_pcie_endpoint *ep = epc_get_drvdata(epc);
	struct resource_entry win;
	/* [한국어] 빈 자원. 창을 지우는 데 쓴다. */
	struct resource res;
	/* [한국어] 창 반복자. 루프 뒤에도 값이 남아 아래에서 쓰인다. */
	int idx;

	for (idx = 0; idx < ep->num_ob_windows; idx++)
		/* [한국어] 기록해 둔 주소와 일치하는 창을 찾는다. */
		if (ep->ob_mapped_addr[idx] == addr)
			/* [한국어] 찾았으면 그 번호를 idx 에 남긴 채 멈춘다. */
			break;

	if (idx >= ep->num_ob_windows)
		/* [한국어] 끝까지 못 찾았으면 이미 닫혔거나 열린 적이 없다는 뜻이라 조용히 물러난다. */
		return;

	memset(&win, 0x0, sizeof(win));
	/* [한국어] 두 임시 구조체를 모두 0 으로 채운다(윗줄과 함께). */
	memset(&res, 0x0, sizeof(res));
	/* [한국어] 빈 자원을 연결한다. */
	win.res = &res;
	/* [한국어] 전부 0 인 자원으로 창을 덮으면 창이 꺼진다 — set_outbound 가 맨 처음
	 * PCIEPTCTLR 에 0 을 쓰고, 크기가 0 이라 마스크도 0 이 되기 때문이다. */
	rcar_pcie_set_outbound(&ep->pcie, idx, &win);
/* [한국어] 기록을 지워 그 자리가 비었음을 표시한다(다음 줄). */

	ep->ob_mapped_addr[idx] = 0;
}

/* [한국어]
 * rcar_pcie_ep_assert_intx - INTx 인터럽트를 호스트로 보낸다
 *
 * @ep:   엔드포인트 상태.
 * @fn:   기능 번호(쓰지 않는다).
 * @intx: INTx 번호(쓰지 않는다 - 호출자가 항상 0 을 넘긴다).
 * @return: 0 성공. -EINVAL 은 MSI 가 켜져 있거나, INTx 가 막혀 있거나,
 *          이미 assert 중인 경우.
 *
 * INTx 는 레벨 방식이라 "세우고 얼마 뒤 내리는" 동작으로 한 번의 인터럽트를
 * 만든다. 그 전에 보낼 수 있는 상태인지 세 가지를 확인한다.
 *   1) MSI 가 켜져 있으면 안 된다. 둘을 동시에 쓰면 호스트가 인터럽트를
 *      두 번 보는 셈이 된다.
 *   2) config 의 INTDIS(INTx 비활성) 비트가 서 있으면 호스트가 INTx 를
 *      원하지 않는다는 뜻이다.
 *   3) 이미 ASTINTX 가 서 있으면 앞선 assert 가 아직 진행 중이다.
 *
 * 조건이 맞으면 PCIEINTXR 의 ASTINTX 를 세우고 1ms 남짓 기다린 뒤 내린다.
 * usleep_range(1000, 1001) 로 범위를 아주 좁게 준 것은 이 지연이 인터럽트
 * 폭의 의미를 가져 정확해야 하기 때문으로 보인다.
 *
 * 첫 검사에서 PCIEMSITXR 값을 PCI_MSI_FLAGS_ENABLE 과 비교하는데,
 * PCI_MSI_FLAGS_ENABLE 은 config space 의 MSI capability 비트 정의이고
 * PCIEMSITXR 은 컨트롤러 고유 레지스터다. 그 비트 위치가 이 레지스터에서도
 * MSI 활성을 뜻하는지는 pcie-rcar.h 에 정의가 없어 이 트리에서 근거를
 * 확인하지 못했다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. usleep_range 로 잠든다.
 *
 * 호출 체인:  rcar_pcie_ep_raise_irq() → [이 함수] → rcar_pci_write_reg()
 */
static int rcar_pcie_ep_assert_intx(struct rcar_pcie_endpoint *ep,
				    u8 fn, u8 intx)
{
	struct rcar_pcie *pcie = &ep->pcie;
	u32 val;
/* [한국어] MSI 송신 레지스터를 읽는다. */

	val = rcar_pci_read_reg(pcie, PCIEMSITXR);
	/* [한국어] MSI 가 켜져 있으면 INTx 와 동시에 쓸 수 없다. 호스트가 인터럽트를
	 * 두 번 보는 셈이 되기 때문이다. 다만 PCI_MSI_FLAGS_ENABLE 은 config space 의
	 * MSI capability 비트 정의인데 여기서는 컨트롤러 고유 레지스터에 적용하고 있어,
	 * 그 비트 위치가 이 레지스터에서도 같은 뜻인지는 pcie-rcar.h 에 근거가 없다. */
	if ((val & PCI_MSI_FLAGS_ENABLE)) {
		/* [한국어] 그 사실을 남기고 */
		dev_err(pcie->dev, "MSI is enabled, cannot assert INTx\n");
		/* [한국어] 거절한다. */
		return -EINVAL;
	}

	val = rcar_pci_read_reg(pcie, PCICONF(1));
	/* [한국어] config 의 INTDIS 가 서 있으면 호스트가 INTx 를 원하지 않는다는 뜻이다. */
	if ((val & INTDIS)) {
		/* [한국어] 그 사실을 남기고 */
		dev_err(pcie->dev, "INTx message transmission is disabled\n");
		/* [한국어] 거절한다. */
		return -EINVAL;
	}

	val = rcar_pci_read_reg(pcie, PCIEINTXR);
	/* [한국어] 이미 assert 중이면 앞선 인터럽트가 아직 끝나지 않았다. */
	if ((val & ASTINTX)) {
		/* [한국어] 그 사실을 남기고 */
		dev_err(pcie->dev, "INTx is already asserted\n");
		/* [한국어] 거절한다. */
		return -EINVAL;
	}

	val |= ASTINTX;
	/* [한국어] ASTINTX 를 세워 INTx 를 assert 한다(윗줄에서 비트를 얹었다). */
	rcar_pci_write_reg(pcie, val, PCIEINTXR);
	/* [한국어] 약 1ms 유지한다. 범위를 1000~1001 로 아주 좁게 준 것은 이 지연이
	 * 인터럽트 폭의 의미를 가져 정확해야 하기 때문으로 보인다. */
	usleep_range(1000, 1001);
	/* [한국어] 현재 값을 다시 읽어 */
	val = rcar_pci_read_reg(pcie, PCIEINTXR);
	/* [한국어] assert 비트만 내리고 */
	val &= ~ASTINTX;
	/* [한국어] 되쓴다. 이 상승-하강이 한 번의 INTx 인터럽트가 된다. */
	rcar_pci_write_reg(pcie, val, PCIEINTXR);

	return 0;
}

/* [한국어]
 * rcar_pcie_ep_assert_msi - MSI 를 호스트로 보낸다
 *
 * @pcie: 컨트롤러.
 * @fn:   기능 번호(MSICAP 오프셋 계산에 쓴다).
 * @interrupt_num: 보낼 벡터 번호. 1부터 시작한다.
 * @return: 0 성공. -EINVAL 은 MSI 가 꺼져 있거나 번호가 범위를 벗어난 경우.
 *
 * INTx 와 달리 레지스터에 벡터 번호를 쓰면 하드웨어가 알아서 메모리 쓰기를
 * 만들어 보낸다.
 *
 * 먼저 호스트가 MSI 를 켰는지 확인하고(MSICAP0_MSIE), 켰다면 몇 개까지
 * 허용했는지 MMSE 필드에서 읽어 개수로 되돌린다. 그 범위를 벗어나거나
 * 0 이면 거절한다.
 *
 * 인자는 1부터인데 하드웨어는 0부터 세므로 1 을 빼서 쓴다. 이 off-by-one
 * 변환이 이 함수의 실질적 내용이다.
 *
 * PCIEMSITXR 을 읽어 OR 로 쓰는 점을 짚어 둔다. 이전 벡터 번호의 비트가
 * 남아 있으면 두 값이 겹쳐 엉뚱한 번호가 될 수 있다. 코드는 고치지 않고
 * 관찰만 적어 둔다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  rcar_pcie_ep_raise_irq() → [이 함수] → rcar_pci_write_reg()
 */
static int rcar_pcie_ep_assert_msi(struct rcar_pcie *pcie,
				   u8 fn, u8 interrupt_num)
{
	u16 msi_count;
	u32 val;
/* [한국어] MSI capability 워드를 읽는다. */

	/* Check MSI enable bit */
	val = rcar_pci_read_reg(pcie, MSICAP(fn));
	if (!(val & MSICAP0_MSIE))
		/* [한국어] 호스트가 MSI 를 켜지 않았으면 보낼 수 없다. */
		return -EINVAL;

	/* Get MSI numbers from MME */
	msi_count = ((val & MSICAP0_MMESE_MASK) >> MSICAP0_MMESE_OFFSET);
	msi_count = 1 << msi_count;
/* [한국어] MMSE 필드에서 허용 개수를 얻는다(윗줄에서 로그를 개수로 되돌렸다). */

	if (!interrupt_num || interrupt_num > msi_count)
		/* [한국어] 번호가 0 이거나 허용 범위를 넘으면 거절한다. */
		return -EINVAL;

	val = rcar_pci_read_reg(pcie, PCIEMSITXR);
	/* [한국어] 하드웨어는 0부터 세므로 1 을 빼서 쓴다. 다만 읽어 온 값에 OR 하므로
	 * 이전 벡터 번호의 비트가 남아 있으면 겹칠 수 있다 — 함수 블록 주석 참조. */
	rcar_pci_write_reg(pcie, val | (interrupt_num - 1), PCIEMSITXR);

	return 0;
}

/* [한국어]
 * rcar_pcie_ep_raise_irq - 인터럽트 종류에 따라 알맞은 방식으로 보낸다
 *
 * @epc: EPC 객체.   @fn: 기능 번호.   @vfn: 가상 기능 번호(쓰지 않는다).
 * @type: PCI_IRQ_INTX 또는 PCI_IRQ_MSI.
 * @interrupt_num: MSI 벡터 번호(INTx 에서는 쓰이지 않는다).
 * @return: 0 성공, -EINVAL 은 지원하지 않는 종류이거나 하위 함수의 실패.
 *
 * EPF 드라이버가 "호스트를 깨워 달라" 고 할 때 불리는 콜백이다. 종류를
 * 보고 두 구현 중 하나로 넘긴다.
 *
 * MSI-X 갈래가 없다. rcar_pcie_epc_features 가 msi_capable 만 참으로
 * 알리고 msix_capable 은 설정하지 않으므로, EPC 코어가 MSI-X 를 요청하지
 * 않는 것이 전제다.
 *
 * INTx 경로에 interrupt_num 을 넘기지 않고 0 을 넘기는 것은 이 하드웨어가
 * INTA 하나만 지원하기 때문이다 - write_header 도 INTA 를 넘는 핀을 거절한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  pci_epc_raise_irq() [pci-epc-core.c:459] → :484 → [이 함수]
 *               → rcar_pcie_ep_assert_intx() 또는 rcar_pcie_ep_assert_msi()
 */
static int rcar_pcie_ep_raise_irq(struct pci_epc *epc, u8 fn, u8 vfn,
				  unsigned int type, u16 interrupt_num)
{
	struct rcar_pcie_endpoint *ep = epc_get_drvdata(epc);

	switch (type) {
	/* [한국어] 레거시 핀 인터럽트 요청. */
	case PCI_IRQ_INTX:
		/* [한국어] 핀 번호로 0 을 넘긴다. 이 하드웨어는 INTA 하나만 지원한다. */
		return rcar_pcie_ep_assert_intx(ep, fn, 0);

	case PCI_IRQ_MSI:
		/* [한국어] MSI 요청은 벡터 번호를 그대로 넘긴다. MSI-X 갈래가 없는 것은
		 * 아래 features 가 msi_capable 만 알리기 때문이다. */
		return rcar_pcie_ep_assert_msi(&ep->pcie, fn, interrupt_num);

	default:
		return -EINVAL;
	}
}

/* [한국어]
 * rcar_pcie_ep_start - 링크 훈련을 시작해 호스트에 보이게 한다
 *
 * @epc: EPC 객체.   @return: 항상 0.
 *
 * EPF 쪽 준비가 끝나 "이제 호스트에 나타나도 좋다" 는 지시가 왔을 때 불린다.
 * MACCTLR 을 초기값으로 세우고 PCIETCTLR 에 CFINIT 을 써서 링크 훈련을
 * 시작한다. 호스트 판 rcar_pcie_hw_init() 의 마지막 두 줄과 같은 동작이다.
 *
 * 호스트 판과 달리 링크가 설 때까지 기다리지 않는다. 엔드포인트는 언제
 * 호스트가 전원을 넣고 훈련을 시작할지 알 수 없으므로, 준비만 해 두고
 * 기다리지 않는 것이 맞다. 링크가 필요한 map_addr 이 그때 가서 확인한다.
 *
 * 항상 0 을 돌려주므로 이 콜백은 실패하지 않는다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  pci_epc_start() [pci-epc-core.c:421] → :442 → [이 함수]
 */
static int rcar_pcie_ep_start(struct pci_epc *epc)
{
	struct rcar_pcie_endpoint *ep = epc_get_drvdata(epc);

	rcar_pci_write_reg(&ep->pcie, MACCTLR_INIT_VAL, MACCTLR);
	/* [한국어] CFINIT 을 써서 링크 훈련을 시작한다(윗줄에서 MACCTLR 을 초기값으로 세웠다).
	 * 호스트 판과 달리 링크가 설 때까지 기다리지 않는다 — 엔드포인트는 호스트가
	 * 언제 훈련을 시작할지 알 수 없기 때문이다. */
	rcar_pci_write_reg(&ep->pcie, CFINIT, PCIETCTLR);

	return 0;
}

/* [한국어]
 * rcar_pcie_ep_stop - 링크를 내려 호스트에서 사라진다
 *
 * @epc: EPC 객체.   @return: 없음.
 *
 * start 의 역이다. PCIETCTLR 에 0 을 써서 CFINIT 을 지우면 링크 훈련이
 * 멈추고 링크가 내려간다.
 *
 * MACCTLR 은 되돌리지 않는다. 다음에 start 가 불리면 어차피 다시 쓰므로
 * 지울 이유가 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  pci_epc_stop() [pci-epc-core.c:396] → :410 → [이 함수]
 */
static void rcar_pcie_ep_stop(struct pci_epc *epc)
{
	struct rcar_pcie_endpoint *ep = epc_get_drvdata(epc);

	rcar_pci_write_reg(&ep->pcie, 0, PCIETCTLR);
/* [한국어] PCIETCTLR 에 0 을 쓰면 CFINIT 이 지워져 링크가 내려간다(윗줄). */
}

static const struct pci_epc_features rcar_pcie_epc_features = {
	/* [한국어] MSI 를 보낼 수 있다고 알린다. msix_capable 은 설정하지 않으므로
	 * EPC 코어가 MSI-X 를 요청하지 않는다. */
	.msi_capable = true,
	/* use 64-bit BARs so mark BAR[1,3,5] as reserved */
	.bar[BAR_0] = { .type = BAR_FIXED, .fixed_size = 128,
			.only_64bit = true, },
	.bar[BAR_2] = { .type = BAR_FIXED, .fixed_size = 256,
			.only_64bit = true, },
	.bar[BAR_4] = { .type = BAR_FIXED, .fixed_size = 256,
			.only_64bit = true, },
};

static const struct pci_epc_features*
/* [한국어] 기능 번호와 무관하게 같은 구조체를 돌려준다 — 제약이 기능마다 다르지 않고
 * 애초에 기능이 하나뿐이기 때문이다. */
rcar_pcie_ep_get_features(struct pci_epc *epc, u8 func_no, u8 vfunc_no)
{
	return &rcar_pcie_epc_features;
/* [한국어] 위에서 정의한 고정 구조체의 주소를 그대로 준다(윗줄). */
}

static const struct pci_epc_ops rcar_pcie_epc_ops = {
	/* [한국어] 신원 정보를 config 에 쓴다. */
	.write_header	= rcar_pcie_ep_write_header,
	/* [한국어] BAR 를 안쪽 창에 연결한다. 이 표에 없는 콜백(예: set_msix)은 EPC 코어가
	 * "지원하지 않음" 으로 처리한다. */
	.set_bar	= rcar_pcie_ep_set_bar,
	.clear_bar	= rcar_pcie_ep_clear_bar,
	.set_msi	= rcar_pcie_ep_set_msi,
	.get_msi	= rcar_pcie_ep_get_msi,
	.map_addr	= rcar_pcie_ep_map_addr,
	.unmap_addr	= rcar_pcie_ep_unmap_addr,
	.raise_irq	= rcar_pcie_ep_raise_irq,
	.start		= rcar_pcie_ep_start,
	.stop		= rcar_pcie_ep_stop,
	.get_features	= rcar_pcie_ep_get_features,
};

static const struct of_device_id rcar_pcie_ep_of_match[] = {
	/* [한국어] R-Car E3(r8a774c0) 전용 compatible. */
	{ .compatible = "renesas,r8a774c0-pcie-ep", },
	/* [한국어] 3세대 공통 compatible. 이 표에 없는 노드에는 붙지 않는다. */
	{ .compatible = "renesas,rcar-gen3-pcie-ep" },
	{ },
};

/* [한국어]
 * rcar_pcie_ep_probe - 진입점. 자원을 모으고 EPC 를 만들어 등록한다
 *
 * @pdev: 플랫폼 장치.
 * @return: 0 성공, 음수 errno 실패.
 *
 *   1) 장치별 상태를 할당하고 dev 를 채운다.
 *   2) 런타임 PM 을 켜고 참조를 잡아 하드웨어에 전원과 클럭이 들어오게 한다.
 *   3) rcar_pcie_ep_get_pdata() 로 레지스터 블록과 바깥 창을 확보한다.
 *   4) 안쪽 창 비트맵을 만든다. 크기는 MAX_NR_INBOUND_MAPS(6)이며,
 *      set_bar 가 한 BAR 당 두 칸씩 쓰므로 실질적으로 BAR 세 개까지다.
 *      아래 features 가 BAR 0/2/4 세 개만 쓸 수 있다고 알리는 것과 맞는다.
 *   5) 바깥 창별로 현재 걸린 주소를 기록할 배열을 만든다.
 *   6) devm_pci_epc_create() 로 EPC 객체를 만들며 ops 를 등록한다. 이때부터
 *      EPC 코어가 이 드라이버의 콜백을 부를 수 있게 된다.
 *   7) max_functions 를 알리고 drvdata 로 ep 를 걸어 둔다 - 콜백들이
 *      epc_get_drvdata() 로 되찾는 경로다.
 *   8) 하드웨어를 엔드포인트 모드로 세운다.
 *   9) 바깥 창들을 EPC 메모리 할당자에 넘긴다.
 *  10) pci_epc_init_notify() 로 준비 완료를 알린다. 이 시점에 EPF 드라이버가
 *      바인딩되어 write_header/set_bar 같은 콜백이 들어오기 시작한다.
 *
 * 에러 경로가 둘뿐인 것은 나머지가 전부 devm 할당이라 자동으로 반납되기
 * 때문이다. 런타임 PM 참조와 활성화만 손으로 되돌린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(드라이버 바인딩).
 *
 * 호출 체인:  플랫폼 버스 → [이 함수] → rcar_pcie_ep_get_pdata()
 *               → devm_pci_epc_create() → rcar_pcie_ep_hw_init()
 *               → pci_epc_multi_mem_init() → pci_epc_init_notify()
 */
static int rcar_pcie_ep_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rcar_pcie_endpoint *ep;
	/* [한국어] 레지스터 접근용 공통 구조체. */
	struct rcar_pcie *pcie;
	/* [한국어] 만들 EPC 객체. */
	struct pci_epc *epc;
	/* [한국어] 하위 호출 결과. */
	int err;

	ep = devm_kzalloc(dev, sizeof(*ep), GFP_KERNEL);
	/* [한국어] 할당 실패면 */
	if (!ep)
		/* [한국어] 메모리 부족. */
		return -ENOMEM;

	pcie = &ep->pcie;
	/* [한국어] 이후 모든 로그와 devm 할당의 주인이 된다(윗줄에서 pcie 를 가리켰다). */
	pcie->dev = dev;

	pm_runtime_enable(dev);
	err = pm_runtime_resume_and_get(dev);
	/* [한국어] 참조 획득에 실패했으면 */
	if (err < 0) {
		/* [한국어] 그 사실을 남기고 */
		dev_err(dev, "pm_runtime_resume_and_get failed\n");
		/* [한국어] 런타임 PM 을 되돌리는 경로로. */
		goto err_pm_disable;
	}

	err = rcar_pcie_ep_get_pdata(ep, pdev);
	/* [한국어] 자원 확보에 실패했으면 */
	if (err < 0) {
		/* [한국어] 그 사실을 남기고 */
		dev_err(dev, "failed to request resources: %d\n", err);
		/* [한국어] 참조까지 되돌리는 경로로. */
		goto err_pm_put;
	}

	ep->num_ib_windows = MAX_NR_INBOUND_MAPS;
	/* [한국어] 안쪽 창 사용 비트맵을 만든다. */
	ep->ib_window_map =
			/* [한국어] BITS_TO_LONGS 로 6비트를 담을 long 개수를 구한다. set_bar 가 한 BAR 당
			 * 두 칸씩 쓰므로 실질적으로 BAR 세 개까지이며, 아래 features 가 BAR 0/2/4
			 * 세 개만 쓸 수 있다고 알리는 것과 맞는다. */
			devm_kcalloc(dev, BITS_TO_LONGS(ep->num_ib_windows),
				     sizeof(long), GFP_KERNEL);
	if (!ep->ib_window_map) {
		/* [한국어] 할당 실패를 기록하고 */
		err = -ENOMEM;
		dev_err(dev, "failed to allocate memory for inbound map\n");
		/* [한국어] 되돌리는 경로로. */
		goto err_pm_put;
	}

	ep->ob_mapped_addr = devm_kcalloc(dev, ep->num_ob_windows,
					  /* [한국어] 바깥 창마다 걸린 주소를 기록할 배열(윗줄에서 개수를 지정했다). */
					  sizeof(*ep->ob_mapped_addr),
					  GFP_KERNEL);
	if (!ep->ob_mapped_addr) {
		/* [한국어] 할당 실패를 기록하고 */
		err = -ENOMEM;
		dev_err(dev, "failed to allocate memory for outbound memory pointers\n");
		/* [한국어] 되돌리는 경로로. */
		goto err_pm_put;
	}

	epc = devm_pci_epc_create(dev, &rcar_pcie_epc_ops);
	/* [한국어] EPC 객체 생성에 실패했으면 */
	if (IS_ERR(epc)) {
		/* [한국어] 그 사실을 남기고 */
		dev_err(dev, "failed to create epc device\n");
		/* [한국어] 오류를 꺼내 */
		err = PTR_ERR(epc);
		/* [한국어] 되돌리는 경로로. 이 호출이 성공하면 EPC 코어가 이 드라이버의 콜백을
		 * 부를 수 있게 된다. */
		goto err_pm_put;
	}

	epc->max_functions = ep->max_functions;
	/* [한국어] 콜백들이 epc_get_drvdata() 로 되찾을 상태를 걸어 둔다. */
	epc_set_drvdata(epc, ep);

	rcar_pcie_ep_hw_init(pcie);

	err = pci_epc_multi_mem_init(epc, ep->ob_window, ep->num_ob_windows);
	/* [한국어] 바깥 창 등록에 실패했으면 */
	if (err < 0) {
		/* [한국어] 그 사실을 남기고 */
		dev_err(dev, "failed to initialize the epc memory space\n");
		/* [한국어] 되돌리는 경로로. */
		goto err_pm_put;
	}

	pci_epc_init_notify(epc);

	return 0;

err_pm_put:
	pm_runtime_put(dev);

err_pm_disable:
	pm_runtime_disable(dev);

	return err;
}

static struct platform_driver rcar_pcie_ep_driver = {
	/* [한국어] 플랫폼 드라이버 서술자. */
	.driver = {
		/* [한국어] 드라이버 이름. */
		.name = "rcar-pcie-ep",
		/* [한국어] 위에서 정의한 compatible 표. */
		.of_match_table = rcar_pcie_ep_of_match,
		.suppress_bind_attrs = true,
	},
	.probe = rcar_pcie_ep_probe,
};
builtin_platform_driver(rcar_pcie_ep_driver);
