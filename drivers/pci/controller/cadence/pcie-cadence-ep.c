// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2017 Cadence
// Cadence PCIe endpoint controller driver.
// Author: Cyrille Pitchen <cyrille.pitchen@free-electrons.com>

/*
 * [한국어 설명] Cadence PCIe IP 의 엔드포인트(EP) 구현 (pcie-cadence-ep.c)
 *
 * === 파일의 역할 ===
 * 같은 Cadence PCIe IP 를 "장치 쪽" 으로 동작시키는 코드다. 옆 파일
 * pcie-cadence-host.c 가 호스트 브리지를 만드는 쪽이라면, 이 파일은 그
 * 브리지 아래에 꽂히는 장치처럼 행세하는 쪽이다. SoC 를 PCIe 카드처럼
 * 다른 컴퓨터에 꽂거나, 두 SoC 를 PCIe 로 잇는 NTB 구성에 쓴다.
 *
 * 커널에는 이런 컨트롤러를 위한 프레임워크가 따로 있다 —
 * drivers/pci/endpoint/ 의 EPC(EndPoint Controller) 계층이다. 그 계층이
 * 정한 규약이 struct pci_epc_ops 이고, 이 파일이 하는 일의 대부분은 그
 * 규약의 열세 가지 콜백을 이 IP 의 레지스터 조작으로 옮기는 것이다.
 * 반대편에는 EPF(EndPoint Function) 드라이버가 있어, configfs 로 어떤
 * 장치인 척할지를 사람이 정한다(예: drivers/pci/endpoint/functions 의
 * pci-epf-test.c).
 *
 * 호스트 쪽과 대칭을 이루는 개념이 셋 있다.
 *   config space  — 호스트 모드에서는 아래쪽 장치의 config 를 "읽는" 쪽이었지만,
 *                   여기서는 자기 config space 를 "채우는" 쪽이다
 *                   (cdns_pcie_ep_write_header).
 *   인바운드 BAR  — 호스트가 이 장치의 BAR 에 접근하면 그것을 로컬 메모리
 *                   주소로 바꿔 준다 (cdns_pcie_ep_set_bar).
 *   아웃바운드 창 — 이 장치가 호스트 메모리를 읽고 쓰는 통로다
 *                   (cdns_pcie_ep_map_addr). 인터럽트도 결국 이 창을 통한
 *                   메모리 쓰기다.
 *
 * 이 파일에서 가장 분량이 많은 부분이 인터럽트다. 엔드포인트에는 호스트로
 * 향하는 인터럽트 선이 없으므로, 세 방식 모두가 결국 "호스트 메모리의 어떤
 * 주소에 어떤 값을 쓰는 일" 로 구현된다. MSI 와 MSI-X 는 호스트가 알려 준
 * 주소에 데이터를 쓰는 것이고, 레거시 INTx 는 Assert_INTx / Deassert_INTx
 * 메시지 TLP 를 만들어 보내는 것이다. 그 셋을 위해 아웃바운드 창 0번을
 * 통째로 예약해 두고 매번 목적지만 바꿔 조준한다.
 *
 * SR-IOV(가상 함수)도 다룬다. 이 IP 에서 VF 들은 BAR 설정을 공유하지만
 * 인바운드 주소 변환은 각자 갖는데, 그 비대칭이 set_bar/clear_bar 안의
 * "vfn == 0 || vfn == 1" 분기와 cdns_pcie_get_fn_from_vfn() 로 나타난다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SoC 별 드라이버의 probe (pci-j721e.c / pcie-cadence-plat.c)
 *   -> cdns_pcie_init_phy()        [pcie-cadence.c] PHY 준비
 *   -> cdns_pcie_ep_setup()        [이 파일] EPC 객체 등록까지
 *        -> devm_pci_epc_create(dev, &cdns_pcie_epc_ops)
 *        -> pci_epc_mem_init() / pci_epc_mem_alloc_addr()
 *        -> pci_epc_init_notify()  [EPC 코어] EPF 드라이버에게 알린다
 *
 * 그 뒤로는 EPF 쪽이 주도한다.
 *   사용자가 configfs 로 EPF 를 만들어 이 EPC 에 바인딩
 *     -> EPF 드라이버가 pci_epc_write_header / _set_bar / _set_msi ... 호출
 *        -> EPC 코어가 epc->lock 을 쥐고 이 파일의 콜백을 부른다
 *     -> 마지막에 pci_epc_start() -> cdns_pcie_ep_start() 로 링크를 올린다
 *   호스트가 열거를 마친 뒤, EPF 가 일이 생길 때마다
 *     pci_epc_raise_irq() -> cdns_pcie_ep_raise_irq() 로 인터럽트를 건다.
 *
 * 실행 컨텍스트: 모든 pci_epc_ops 콜백은 EPC 코어가 epc->lock(뮤텍스)을 쥔
 *   프로세스 컨텍스트에서 불린다 — drivers/pci/endpoint/pci-epc-core.c 참고.
 *   따라서 콜백 안에서 잠들어도 되고, 서로 다른 콜백이 동시에 실행되지 않는다.
 *   예외적으로 INTx 경로만 ep->lock 스핀락을 irqsave 로 따로 잡는데,
 *   그것은 호스트와 공유하는 PCI_STATUS 레지스터의 읽고-고쳐-쓰기 구간을
 *   짧게 유지하기 위한 것이다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: drivers/pci/endpoint/pci-epc-core.c 가 이 파일의 ops 표를 부른다.
 *   그 위에 EPF 드라이버(drivers/pci/endpoint/functions/)가 있고,
 *   더 위에 configfs 를 통한 사용자 설정이 있다.
 * 옆쪽: SoC 별 드라이버(pci-j721e.c, pcie-cadence-plat.c)가
 *   cdns_pcie_ep_setup() / cdns_pcie_ep_disable() 을 부른다.
 * 아래쪽: pcie-cadence.c 의 아웃바운드 창 설정(cdns_pcie_set_outbound_region,
 *   _for_normal_msg, cdns_pcie_reset_outbound_region)과 capability 탐색
 *   (cdns_pcie_find_capability / _find_ext_capability), 링크 시작
 *   (cdns_pcie_start_link).
 * 공유 상태: struct cdns_pcie_ep 가 중심이다. ob_region_map 비트맵이 아웃바운드
 *   창 점유 상태, ob_addr[] 이 창별 CPU 주소, epf[] 가 함수별 BAR 장부,
 *   irq_pci_addr / irq_pci_fn 이 0번 창의 현재 조준 상태를 담는다.
 * 데이터 흐름: 호스트 -> BAR -> 인바운드 변환 -> 로컬 메모리 (읽기/쓰기),
 *   그리고 로컬 CPU -> 아웃바운드 창 -> 호스트 메모리 (DMA 와 인터럽트).
 *
 * === NVMe 관점 ===
 * drivers/nvme 에서 cdns_ 로 시작하는 심볼을 부르는 곳은 없다(전수 확인).
 * 이 파일과 NVMe 의 관계는 코드 호출이 아니다.
 *
 * 다만 개념적으로는 NVMe 를 공부하는 독자에게 흥미로운 반대편이다. 보통
 * NVMe 는 호스트가 SSD 컨트롤러를 부리는 관계인데, 이 파일은 그 SSD 쪽
 * 자리에 SoC 를 앉히는 코드다. NVMe 컨트롤러가 하는 일 — config space 헤더를
 * 채워 자기 정체를 밝히고, BAR0 를 열어 doorbell 과 레지스터를 노출하고,
 * 호스트 메모리의 큐를 읽고 쓰고, 완료 시 MSI-X 를 쏘는 일 — 의 하드웨어
 * 쪽 골격이 여기 그대로 있다.
 *   config space 헤더 -> cdns_pcie_ep_write_header()
 *   BAR 노출          -> cdns_pcie_ep_set_bar()
 *   호스트 메모리 접근 -> cdns_pcie_ep_map_addr()
 *   MSI-X 발사        -> cdns_pcie_ep_send_msix_irq()
 * 실제로 이 프레임워크 위에 NVMe 엔드포인트 함수를 얹은 코드가 이 트리에
 * 있는지는 확인하지 못했다.
 *
 * === 주요 함수/구조체 요약 ===
 * cdns_pcie_get_fn_from_vfn() : (PF 번호, VF 번호)를 SR-IOV 라우팅 번호로 바꾼다.
 * cdns_pcie_ep_write_header() : 자기 config space 헤더를 채운다.
 * cdns_pcie_ep_set_bar() / _clear_bar() : 인바운드 BAR 를 열고 닫는다.
 * cdns_pcie_ep_map_addr() / _unmap_addr() : 호스트 메모리로 나가는 창을 관리한다.
 * cdns_pcie_ep_set_msi() / _get_msi() / _set_msix() / _get_msix() :
 *                        인터럽트 능력을 광고하고 호스트의 허락을 읽는다.
 * cdns_pcie_ep_assert_intx() : INTx 메시지 TLP 한 발을 보낸다.
 * cdns_pcie_ep_send_msi_irq() / _send_msix_irq() : 호스트 메모리에 써서
 *                        인터럽트를 건다.
 * cdns_pcie_ep_raise_irq() : 세 방식으로 갈라 주는 규약 진입점.
 * cdns_pcie_ep_start()  : 모든 함수를 활성화하고 링크를 올린다.
 * cdns_pcie_epc_ops     : EPC 규약 구현 표. 이 파일의 목차라고 봐도 좋다.
 * cdns_pcie_epc_features / _vf_features : 이 컨트롤러의 능력 표.
 * cdns_pcie_ep_setup() / _disable() : SoC 드라이버가 부르는 진입점과 정리.
 */

/* [한국어] linux/bitfield.h — FIELD_GET(). MSI 의 Multiple Message Enable,
 * MSI-X 의 Table BIR 같이 레지스터 안에 박힌 비트 필드를 마스크와 시프트
 * 계산 없이 꺼내는 데 쓴다. */
#include <linux/bitfield.h>
/* [한국어] linux/delay.h — mdelay(). INTx 를 어서트한 뒤 해제하기 전에 1ms 를
 * 바쁘게 기다리는 곳이 한 군데 있다(cdns_pcie_ep_send_intx_irq). */
#include <linux/delay.h>
/* [한국어] linux/kernel.h — max_t(), fls64(), ilog2(), order_base_2().
 * BAR 크기를 2의 거듭제곱으로 올림하고 그것을 aperture 코드로 바꾸는 계산,
 * 그리고 MSI 개수를 log2 로 바꾸는 계산에 쓴다. */
#include <linux/kernel.h>
/* [한국어] linux/module.h — EXPORT_SYMBOL_GPL 과 MODULE_ 매크로들.
 * 이 파일은 독립 모듈로 빌드되고 SoC 별 드라이버가 링크해 쓴다. */
#include <linux/module.h>
/* [한국어] linux/of.h — of_property_read_u32 / _u8 / _u8_array.
 * 디바이스 트리에서 cdns,max-outbound-regions, max-functions,
 * max-virtual-functions 를 읽는다. */
#include <linux/of.h>
/* [한국어] linux/pci-epc.h — 이 파일이 구현하는 규약 그 자체다.
 * struct pci_epc, struct pci_epc_ops, struct pci_epc_features,
 * epc_get_drvdata / epc_set_drvdata, devm_pci_epc_create,
 * pci_epc_mem_init / _alloc_addr / _free_addr / _exit,
 * pci_epc_init_notify / _deinit_notify 가 모두 여기서 온다. */
#include <linux/pci-epc.h>
/* [한국어] linux/platform_device.h — to_platform_device(),
 * platform_get_resource_byname(), devm_platform_ioremap_resource_byname().
 * 디바이스 트리가 준 reg/mem 두 자원을 꺼내는 데 필요하다. */
#include <linux/platform_device.h>
/* [한국어] linux/sizes.h — SZ_128K. 인터럽트 전용 아웃바운드 창의 크기로 쓴다.
 * 2^17 이며, 이 IP 의 메시지 전용 창이 17비트로 고정된 것과 정확히 맞는다. */
#include <linux/sizes.h>

/* [한국어] 이 IP 공통 헤더. struct cdns_pcie_ep / cdns_pcie_epf 정의,
 * CDNS_PCIE_ 레지스터 상수, cdns_pcie_ep_fn_readw / writel 같은
 * 엔드포인트 함수별 config space 접근자, 그리고
 * cdns_pcie_set_outbound_region() 선언이 여기서 온다. */
#include "pcie-cadence.h"
/* [한국어] PCI 코어 내부 헤더. PCIE_MSG_TYPE_R_LOCAL 과 PCIE_MSG_CODE_ASSERT_INTA
 * 계열 상수를 쓰기 위해 필요하다 — INTx 를 메시지 TLP 로 흉내 낼 때
 * 그 라우팅과 코드 값을 여기서 가져온다. */
#include "../../pci.h"

/* [한국어] BAR 하나가 가질 수 있는 최소 크기(128바이트).
 * 아래 aperture 계산이 ilog2(sz) - 7 인 것과 짝이다 — 2^7 = 128 이 코드 0 이다.
 * 호스트가 요구한 크기가 이보다 작아도 이 값으로 올려 잡는다. */
#define CDNS_PCIE_EP_MIN_APERTURE		128	/* 128 bytes */
/* [한국어] ep->irq_pci_addr 의 "아직 아무 매핑도 없음" 표시.
 * 실제 매핑 값은 언제나 pci_addr & ~0xff 형태(256의 배수)라
 * 0x1 과는 절대 같아질 수 없다 — 그래서 센티널로 안전하다. */
#define CDNS_PCIE_EP_IRQ_PCI_ADDR_NONE		0x1
/* [한국어] ep->irq_pci_addr 의 "지금 INTx 메시지용으로 매핑되어 있음" 표시.
 * 역시 256의 배수가 아니므로 실제 MSI/MSI-X 주소와 충돌하지 않는다.
 * INTx 는 목적지 주소가 없는 메시지 TLP 라 진짜 주소를 저장할 것이 없어
 * 이런 표식을 쓴다. */
#define CDNS_PCIE_EP_IRQ_PCI_ADDR_LEGACY	0x3

/* [한국어]
 * cdns_pcie_get_fn_from_vfn - (물리 함수, 가상 함수) 쌍을 하드웨어 함수 번호로 바꾼다
 *
 * @pcie: 대상 컨트롤러. SR-IOV capability 를 읽는 데 쓴다.
 * @fn: 물리 함수(PF) 번호.
 * @vfn: 가상 함수(VF) 번호. 0 이면 PF 자신, 1 이상이면 그 PF 의 n번째 VF.
 * @return: 레지스터 색인에 쓸 함수 번호. vfn 이 0 이면 fn 을 그대로 돌려준다.
 *
 * EPC 규약은 대상을 (func_no, vfunc_no) 두 값으로 지정한다. 그런데 하드웨어
 * 레지스터는 그런 쌍을 모르고 하나의 함수 번호만 안다. SR-IOV 규격이 그
 * 변환 규칙을 정해 두었다 — PF 의 SR-IOV capability 안에 VF Offset 과
 * VF Stride 라는 두 필드가 있고, n번째 VF 의 라우팅 번호는
 * PF + VF Offset + (n - 1) * VF Stride 다. 이 함수가 그 식 그대로다.
 *
 * 왜 필요한가: 이 파일의 거의 모든 함수가 레지스터를 만지기 직전에 이것을
 * 부른다. 예를 들어 CDNS_PCIE_AT_IB_EP_FUNC_BAR_ADDR0(fn, bar) 은 fn 을
 * 곱셈에 쓰므로, PF 번호를 그대로 넣으면 VF 의 레지스터가 아니라 다른 PF 의
 * 레지스터를 건드리게 된다.
 *
 * 실행 컨텍스트: EPC 코어가 epc->lock 을 쥔 프로세스 컨텍스트, 또는 인터럽트를
 *   올리는 경로. 레지스터 읽기 두 번뿐이라 잠들지 않는다.
 *
 * 에러 경로: 없다. SR-IOV capability 를 못 찾아 cap 이 0 이어도 그대로
 *   오프셋 0 을 읽어 엉뚱한 값을 쓰게 되는데, 이 트리에서 그것을 막는 코드는
 *   찾지 못했다. vfn > 0 은 SR-IOV 를 지원하는 구성에서만 온다는 전제로 보인다.
 *
 * 호출 체인:
 *   cdns_pcie_ep_set_bar() / _clear_bar() / _map_addr() / _set_msi() /
 *   _get_msi() / _set_msix() / _get_msix() / _send_msi_irq() /
 *   _map_msi_irq() / _send_msix_irq() -> [이 함수]
 *     -> cdns_pcie_find_ext_capability() [pcie-cadence.c]
 *     -> cdns_pcie_ep_fn_readw() [pcie-cadence.h]
 */
static u8 cdns_pcie_get_fn_from_vfn(struct cdns_pcie *pcie, u8 fn, u8 vfn)
{
	/* [한국어] SR-IOV capability 에서 읽어 올 두 값.
	 * first_vf_offset 은 첫 VF 가 PF 로부터 몇 함수 번호만큼 떨어져 있는지,
	 * stride 는 VF 사이의 간격이다(PCIe SR-IOV 규격의 VF Offset / VF Stride). */
	u32 first_vf_offset, stride;
	/* [한국어] SR-IOV extended capability 가 config space 안에서 시작하는 오프셋. */
	u16 cap;

	/* [한국어] vfn 0 은 "가상 함수가 아니라 물리 함수 자신" 이라는 뜻이다.
	 * EPC 규약은 (func_no, vfunc_no) 쌍으로 대상을 지정하고 vfunc_no 0 이 PF 다. */
	if (vfn == 0)
		return fn;

	/* [한국어] SR-IOV 는 확장 capability(오프셋 0x100 이상)라 find_ext_capability 를 쓴다.
	 * 반환값을 검사하지 않는데, SR-IOV 를 지원하지 않는 IP 에서 vfn > 0 으로
	 * 불릴 일이 없다는 전제로 보인다 — 이 트리에서 그 전제를 강제하는 코드는
	 * 찾지 못했다. */
	cap = cdns_pcie_find_ext_capability(pcie, PCI_EXT_CAP_ID_SRIOV);
	/* [한국어] VF Offset 필드(SR-IOV capability + 0x14). PF 번호에 이 값을 더하면
	 * 첫 VF 의 라우팅 번호가 된다. */
	first_vf_offset = cdns_pcie_ep_fn_readw(pcie, fn, cap + PCI_SRIOV_VF_OFFSET);
	/* [한국어] VF Stride 필드(SR-IOV capability + 0x16). VF 들이 함수 번호 공간에서
	 * 몇 칸씩 떨어져 배치되는지를 나타낸다. */
	stride = cdns_pcie_ep_fn_readw(pcie, fn, cap +  PCI_SRIOV_VF_STRIDE);
	/* [한국어] SR-IOV 규격의 VF 라우팅 ID 계산 그대로다.
	 * vfn 은 1부터 세는 번호이므로 (vfn - 1) 을 곱한다 —
	 * 첫 VF(vfn == 1)는 PF + offset 자리에 놓인다.
	 * 이렇게 얻은 번호가 곧 하드웨어 레지스터에서 이 VF 를 가리키는 인덱스다. */
	fn = fn + first_vf_offset + ((vfn - 1) * stride);

	/* [한국어] 물리 함수 번호(vfn == 0) 또는 계산된 VF 라우팅 번호를 돌려준다.
	 * 호출자들은 이 값을 CDNS_PCIE_AT_IB_EP_FUNC_BAR_ADDR0(fn, bar) 처럼
	 * 레지스터 오프셋 계산에 바로 넣는다. */
	return fn;
}

/* [한국어]
 * cdns_pcie_ep_write_header - 자기 config space 헤더를 채운다 (epc_ops.write_header)
 *
 * @epc: EPC 객체. epc_get_drvdata 로 이 드라이버 상태를 꺼낸다.
 * @fn: 물리 함수 번호.
 * @vfn: 가상 함수 번호. 0 이면 PF, 1 이면 VF Device ID 만 갱신, 2 이상은 거절.
 * @hdr: EPF 드라이버가 configfs 로 설정한 헤더 값들
 *       (vendorid, deviceid, revid, class 코드, subsys, interrupt_pin 등).
 * @return: 0 이면 성공. vfn 이 2 이상이면 -EINVAL.
 *
 * 엔드포인트가 "무엇인 척할지" 를 정하는 함수다. 호스트가 이 장치를 열거할 때
 * 가장 먼저 읽는 것이 config space 헤더이고, 그 값에 따라 어떤 드라이버를
 * 붙일지가 정해진다. 그러니 이 함수가 곧 장치의 정체성을 만드는 자리다.
 *
 * 두 가지 특이 사항이 있다.
 *   VF 처리 — SR-IOV 규격상 VF 들은 자기 Device ID 를 따로 갖지 않고, PF 의
 *     SR-IOV capability 안에 있는 VF Device ID 필드 하나를 공유한다. 그래서
 *     VF #1 만 그것을 대표로 설정하고, VF #2 이상은 거절한다.
 *   Vendor ID — 상류 주석대로 함수 0 에서만 바꿀 수 있다. 게다가 이 IP 에서
 *     Vendor ID 는 config space 가 아니라 Local Management 블록의 ID
 *     레지스터로 정해진다. 호스트 모드의 루트 포트 설정이 쓰는 것과 같은
 *     레지스터다.
 *
 * 실행 컨텍스트: EPC 코어가 epc->lock 을 쥔 프로세스 컨텍스트.
 *
 * 에러 경로: vfn 검사 하나뿐이며, 레지스터 쓰기는 실패하지 않는다.
 *
 * 호출 체인:
 *   EPF 드라이버 -> pci_epc_write_header() [pci-epc-core.c] -> [이 함수]
 *     -> cdns_pcie_ep_fn_writeb/writew() / cdns_pcie_writel()
 */
static int cdns_pcie_ep_write_header(struct pci_epc *epc, u8 fn, u8 vfn,
				     struct pci_epf_header *hdr)
{
	/* [한국어] EPC 코어가 devm_pci_epc_create 시점에 저장해 둔 드라이버 private 포인터.
	 * cdns_pcie_ep_setup() 의 epc_set_drvdata(epc, ep) 와 짝을 이룬다. */
	struct cdns_pcie_ep *ep = epc_get_drvdata(epc);
	/* [한국어] 레지스터 접근용 공통 구조체. */
	struct cdns_pcie *pcie = &ep->pcie;
	/* [한국어] VF 의 Device ID 를 쓸 레지스터 오프셋을 담을 변수. */
	u32 reg;
	/* [한국어] SR-IOV capability 오프셋. */
	u16 cap;

	/* [한국어] VF 처리를 위해 먼저 SR-IOV capability 위치를 찾아 둔다. */
	cap = cdns_pcie_find_ext_capability(pcie, PCI_EXT_CAP_ID_SRIOV);
	/* [한국어] 이 IP 는 VF 별로 Device ID 를 따로 두지 않는다. SR-IOV 규격상
	 * VF Device ID 필드는 PF 의 capability 안에 하나뿐이고 모든 VF 가 공유한다.
	 * 그래서 VF #1 만 그 값을 정할 수 있고 그 이상은 거절한다. */
	if (vfn > 1) {
		/* [한국어] 함수 드라이버(EPF)가 잘못 쓴 것이므로 원인을 밝혀 준다. */
		dev_err(&epc->dev, "Only Virtual Function #1 has deviceID\n");
		/* [한국어] EPC 코어를 통해 EPF 드라이버에게 그대로 전달된다. */
		return -EINVAL;
	/* [한국어] VF #1 이면 SR-IOV capability 의 VF Device ID 필드 하나만 갱신하고 끝낸다.
	 * VF 는 자기 config space 헤더의 나머지 필드를 PF 에서 물려받으므로
	 * 아래의 표준 헤더 채우기를 하지 않는다. */
	} else if (vfn == 1) {
		/* [한국어] SR-IOV capability + PCI_SRIOV_VF_DID(0x1A) 가 VF Device ID 필드다. */
		reg = cap + PCI_SRIOV_VF_DID;
		/* [한국어] VF Device ID 를 쓴다. 대상은 PF 의 config space(fn) 이라는 점에 주의 —
		 * VF 자신이 아니라 PF 안의 SR-IOV capability 를 고치는 것이다. */
		cdns_pcie_ep_fn_writew(pcie, fn, reg, hdr->deviceid);
		return 0;
	}

	/* [한국어] 여기부터는 PF 자신의 config space 헤더를 채운다.
	 * EPF 드라이버가 configfs 로 설정한 값들이 hdr 에 담겨 온다. */
	cdns_pcie_ep_fn_writew(pcie, fn, PCI_DEVICE_ID, hdr->deviceid);
	/* [한국어] Revision ID 바이트. */
	cdns_pcie_ep_fn_writeb(pcie, fn, PCI_REVISION_ID, hdr->revid);
	/* [한국어] Class Code 의 최하위 바이트(Programming Interface). */
	cdns_pcie_ep_fn_writeb(pcie, fn, PCI_CLASS_PROG, hdr->progif_code);
	/* [한국어] Class Code 의 상위 두 바이트. subclass 가 하위, base class 가 상위이므로
	 * baseclass 를 8비트 왼쪽으로 밀어 한 워드로 합친다.
	 * 호스트가 이 값을 보고 어떤 드라이버를 붙일지 정한다. */
	cdns_pcie_ep_fn_writew(pcie, fn, PCI_CLASS_DEVICE,
			       hdr->subclass_code | hdr->baseclass_code << 8);
	/* [한국어] Cache Line Size 레지스터. 레거시 PCI 시절의 필드로,
	 * PCIe 에서는 실질적 의미가 없지만 호스트 소프트웨어가 읽을 수 있게 채워 준다. */
	cdns_pcie_ep_fn_writeb(pcie, fn, PCI_CACHE_LINE_SIZE,
			       hdr->cache_line_size);
	/* [한국어] Subsystem ID. Subsystem Vendor ID 와 달리 이것은 함수마다 따로 둘 수 있다. */
	cdns_pcie_ep_fn_writew(pcie, fn, PCI_SUBSYSTEM_ID, hdr->subsys_id);
	/* [한국어] Interrupt Pin 레지스터. 이 함수가 INTA#~INTD# 중 어느 것을 쓰는지를
	 * 호스트에 알린다(1 = INTA). INTx 를 쓰지 않으면 0 이다. */
	cdns_pcie_ep_fn_writeb(pcie, fn, PCI_INTERRUPT_PIN, hdr->interrupt_pin);

	/*
	 * Vendor ID can only be modified from function 0, all other functions
	 * use the same vendor ID as function 0.
	 */
	/* [한국어] 상류 주석이 이유를 밝히고 있다 — Vendor ID 는 함수 0 에서만 바꿀 수 있고
	 * 나머지 함수는 함수 0 의 값을 그대로 쓴다. 하드웨어가 그렇게 만들어져 있다. */
	if (fn == 0) {
		/* Update the vendor IDs. */
		/* [한국어] Vendor ID(하위 16비트)와 Subsystem Vendor ID(상위 16비트)를 한 워드로 합친다.
		 * 호스트 모드의 cdns_pcie_host_init_root_port() 이 쓰는 것과 같은 레지스터다. */
		u32 id = CDNS_PCIE_LM_ID_VENDOR(hdr->vendorid) |
			 CDNS_PCIE_LM_ID_SUBSYS(hdr->subsys_vendor_id);

		/* [한국어] config space 가 아니라 Local Management 블록의 ID 레지스터에 쓴다.
		 * 이 IP 에서 Vendor ID 는 함수별 config space 가 아니라 여기서 정해진다. */
		cdns_pcie_writel(pcie, CDNS_PCIE_LM_ID, id);
	}

	/* [한국어] 여기까지 오면 성공. 실패할 수 있는 것은 위의 vfn 검사뿐이다. */
	return 0;
}

/* [한국어]
 * cdns_pcie_ep_set_bar - 인바운드 BAR 하나를 연다 (epc_ops.set_bar)
 *
 * @epc: EPC 객체.
 * @fn: 물리 함수 번호.
 * @vfn: 가상 함수 번호. 0 이면 PF.
 * @epf_bar: EPF 드라이버가 준비한 BAR 정보. phys_addr 은 이 BAR 뒤에 놓일
 *           로컬 버퍼의 물리 주소, size 는 요청 크기, barno 는 몇 번 BAR 인지,
 *           flags 는 IO/MEM, 32/64비트, prefetchable 여부다.
 * @return: 0 이면 성공. 64비트 BAR 를 홀수 번호에 요청하면 -EINVAL.
 *
 * 호스트에게 메모리 창을 열어 주는 함수다. 호스트가 이 BAR 범위에 읽기/쓰기
 * TLP 를 보내면 컨트롤러가 그것을 epf_bar->phys_addr 로 변환해 로컬 메모리에
 * 닿게 한다. NVMe 로 치면 컨트롤러가 BAR0 를 통해 CAP/CC/doorbell 을
 * 노출하는 것과 같은 자리다.
 *
 * 설정은 두 갈래로 나뉘어 서로 다른 레지스터에 들어간다.
 *   BAR_CFG   — 크기(aperture)와 종류(IO/MEM, 32/64비트, prefetch).
 *               이 IP 에서 VF 들은 이것을 공유하므로 VF #1 만 대표로 쓴다.
 *   AT_IB_..  — 인바운드 주소 변환의 목적지. 이쪽은 VF 마다 따로 있으므로
 *               SR-IOV 라우팅 번호로 각자의 레지스터를 찾아 쓴다.
 * 그 비대칭이 이 함수 중간의 "vfn == 0 || vfn == 1" 분기와 그 뒤의
 * cdns_pcie_get_fn_from_vfn() 호출로 나타난다.
 *
 * 크기 계산도 눈여겨볼 만하다. 요청 크기를 최소 128바이트로 올리고, 2의
 * 거듭제곱으로 올림한 뒤, log2 에서 7 을 빼 하드웨어 코드로 만든다.
 * 상류 주석이 그 대응표(128B -> 0, 256B -> 1, ...)를 적어 두었다.
 *
 * 실행 컨텍스트: EPC 코어가 epc->lock 을 쥔 프로세스 컨텍스트.
 *
 * 에러 경로: 64비트 BAR 정렬 검사 하나뿐이고, 그 경우 아무 레지스터도
 *   건드리지 않은 상태에서 돌아간다.
 *
 * 호출 체인:
 *   EPF 드라이버 -> pci_epc_set_bar() [pci-epc-core.c] -> [이 함수]
 *     -> cdns_pcie_get_fn_from_vfn() -> cdns_pcie_writel()
 */
static int cdns_pcie_ep_set_bar(struct pci_epc *epc, u8 fn, u8 vfn,
				struct pci_epf_bar *epf_bar)
{
	/* [한국어] EPC 코어가 저장해 둔 드라이버 private 포인터. */
	struct cdns_pcie_ep *ep = epc_get_drvdata(epc);
	/* [한국어] 이 물리 함수의 BAR 장부. VF 라면 아래에서 다시 epf->epf[vfn-1] 로 좁힌다. */
	struct cdns_pcie_epf *epf = &ep->epf[fn];
	/* [한국어] 레지스터 접근용 공통 구조체. */
	struct cdns_pcie *pcie = &ep->pcie;
	/* [한국어] 이 BAR 가 가리킬 로컬(엔드포인트 쪽) 물리 주소.
	 * EPF 코어가 dma_alloc_coherent 등으로 잡아 둔 버퍼의 주소이며,
	 * 호스트가 이 BAR 에 쓰면 그 데이터가 여기 도착한다. */
	dma_addr_t bar_phys = epf_bar->phys_addr;
	/* [한국어] 몇 번 BAR 인지(BAR_0 ~ BAR_5). */
	enum pci_barno bar = epf_bar->barno;
	/* [한국어] PCI_BASE_ADDRESS_ 계열 플래그. IO/MEM, 32/64비트, prefetchable 여부를 담는다. */
	int flags = epf_bar->flags;
	/* [한국어] addr0/addr1 은 인바운드 주소 변환에 쓸 로컬 주소, reg 는 BAR 설정 레지스터
	 * 오프셋, cfg 는 그 값, b 는 레지스터 안의 필드 인덱스,
	 * aperture 는 크기 코드, ctrl 은 BAR 종류 코드다. */
	u32 addr0, addr1, reg, cfg, b, aperture, ctrl;
	/* [한국어] 64비트 BAR 를 다루므로 크기 계산은 u64 로 한다. */
	u64 sz;

	/* BAR size is 2^(aperture + 7) */
	/* [한국어] 요청 크기가 이 IP 의 최소 aperture(128바이트)보다 작으면 올려 잡는다.
	 * 더 작은 BAR 는 하드웨어가 표현할 수 없다. */
	sz = max_t(size_t, epf_bar->size, CDNS_PCIE_EP_MIN_APERTURE);
	/*
	 * roundup_pow_of_two() returns an unsigned long, which is not suited
	 * for 64bit values.
	 */
	/* [한국어] 상류 주석대로 roundup_pow_of_two() 를 쓸 수 없다 —
	 * 그 함수는 unsigned long 을 돌려주는데 32비트 아키텍처에서는 32비트라
	 * 64비트 크기를 담지 못한다. fls64(sz - 1) 은 sz 를 덮는 가장 작은
	 * 2의 거듭제곱의 지수를 준다. */
	sz = 1ULL << fls64(sz - 1);
	/* [한국어] 크기를 하드웨어의 aperture 코드로 바꾼다. 상류 주석이 대응표를 적어 두었다 —
	 * 128B 가 0, 256B 가 1 하는 식이므로 log2 에서 7 을 빼면 된다. */
	aperture = ilog2(sz) - 7; /* 128B -> 0, 256B -> 1, 512B -> 2, ... */

	/* [한국어] PCI_BASE_ADDRESS_SPACE 는 BAR 최하위 비트로, 이 BAR 가 I/O 공간인지
	 * 메모리 공간인지를 가른다. I/O BAR 는 종류 코드가 하나뿐이라 바로 정해진다. */
	if ((flags & PCI_BASE_ADDRESS_SPACE) == PCI_BASE_ADDRESS_SPACE_IO) {
		/* [한국어] I/O BAR 코드(0x1). 이 IP 는 32비트 I/O 만 지원한다. */
		ctrl = CDNS_PCIE_LM_BAR_CFG_CTRL_IO_32BITS;
	} else {
		/* [한국어] prefetchable 비트(BAR 비트 3). 호스트가 이 영역을 미리 읽어도 부작용이
		 * 없다는 표시이며, 브리지가 읽기를 합치거나 앞당길 수 있게 한다. */
		bool is_prefetch = !!(flags & PCI_BASE_ADDRESS_MEM_PREFETCH);
		/* [한국어] 64비트 메모리 BAR 인지(BAR 비트 [2:1] 이 0b10).
		 * 64비트 BAR 는 연속한 두 BAR 를 묶어 쓴다. */
		bool is_64bits = !!(flags & PCI_BASE_ADDRESS_MEM_TYPE_64);

		/* [한국어] 64비트 BAR 는 반드시 짝수 번호에서 시작해야 한다.
		 * 홀수 번호는 앞 BAR 의 상위 32비트 자리이므로 그 자리에서 시작할 수 없다.
		 * (bar & 1) 로 홀수인지 검사한다. */
		if (is_64bits && (bar & 1))
			/* [한국어] EPF 드라이버의 설정 오류이므로 거절한다. */
			return -EINVAL;

		/* [한국어] 네 가지 조합을 그대로 코드로 옮긴다. 이 값들은 호스트 모드의
		 * RC BAR 설정과 같은 상수를 쓴다 — 헤더 주석이 "applicable to both
		 * Endpoint Function and Root Complex" 라고 밝히고 있다. */
		if (is_64bits && is_prefetch)
			/* [한국어] prefetchable 64비트 메모리 BAR(0x7). */
			ctrl = CDNS_PCIE_LM_BAR_CFG_CTRL_PREFETCH_MEM_64BITS;
		else if (is_prefetch)
			/* [한국어] prefetchable 32비트 메모리 BAR(0x5). */
			ctrl = CDNS_PCIE_LM_BAR_CFG_CTRL_PREFETCH_MEM_32BITS;
		else if (is_64bits)
			/* [한국어] 일반(non-prefetchable) 64비트 메모리 BAR(0x6). */
			ctrl = CDNS_PCIE_LM_BAR_CFG_CTRL_MEM_64BITS;
		else
			/* [한국어] 일반 32비트 메모리 BAR(0x4). */
			ctrl = CDNS_PCIE_LM_BAR_CFG_CTRL_MEM_32BITS;
	}

	/* [한국어] 인바운드 변환의 목적지 로컬 주소 하위 32비트.
	 * 호스트 모드의 인바운드 설정과 달리 여기서는 크기 필드를 섞지 않는다 —
	 * EP 쪽은 크기를 별도의 BAR_CFG 레지스터 aperture 필드로 표현하기 때문이다. */
	addr0 = lower_32_bits(bar_phys);
	/* [한국어] 상위 32비트. 4GB 위쪽 로컬 메모리를 BAR 뒤에 둘 수 있다. */
	addr1 = upper_32_bits(bar_phys);

	/* [한국어] VF #1 이면 VF 전용 BAR 설정 레지스터를 쓴다.
	 * 이 IP 에서 VF 들은 BAR 설정을 공유하므로(SR-IOV 규격의 VF BAR 는
	 * PF 의 capability 에 한 벌만 존재한다) VF #1 하나만 그것을 대표해 설정한다. */
	if (vfn == 1)
		/* [한국어] VF BAR 설정 레지스터. BAR 번호가 4 미만이면 CFG0, 이상이면 CFG1 이다. */
		reg = CDNS_PCIE_LM_EP_VFUNC_BAR_CFG(bar, fn);
	else
		/* [한국어] PF BAR 설정 레지스터. 역시 BAR_4 를 기준으로 두 레지스터로 나뉜다. */
		reg = CDNS_PCIE_LM_EP_FUNC_BAR_CFG(bar, fn);
	/* [한국어] 레지스터 하나에 BAR 네 개분의 필드가 8비트씩 들어 있으므로,
	 * BAR_4 이상은 두 번째 레지스터의 0번 자리부터 다시 센다. */
	b = (bar < BAR_4) ? bar : bar - BAR_4;

	/* [한국어] vfn 이 0(PF)이거나 1(대표 VF)일 때만 BAR_CFG 를 건드린다.
	 * vfn 이 2 이상이면 이미 VF #1 이 설정해 둔 것을 공유하므로 건너뛰고,
	 * 아래의 주소 변환만 자기 몫으로 설정한다. */
	if (vfn == 0 || vfn == 1) {
		/* [한국어] 한 레지스터에 여러 BAR 의 설정이 들어 있으므로 읽고-고쳐-쓰기를 한다. */
		cfg = cdns_pcie_readl(pcie, reg);
		/* [한국어] 이번 BAR 자리의 aperture(하위 5비트)와 ctrl(상위 3비트) 필드를 지운다.
		 * 두 마스크 모두 (b * 8) 만큼 밀려 있어 BAR 별 8비트 구획에 대응한다. */
		cfg &= ~(CDNS_PCIE_LM_EP_FUNC_BAR_CFG_BAR_APERTURE_MASK(b) |
			 CDNS_PCIE_LM_EP_FUNC_BAR_CFG_BAR_CTRL_MASK(b));
		/* [한국어] 지운 자리에 새 크기 코드와 종류 코드를 넣는다. */
		cfg |= (CDNS_PCIE_LM_EP_FUNC_BAR_CFG_BAR_APERTURE(b, aperture) |
			CDNS_PCIE_LM_EP_FUNC_BAR_CFG_BAR_CTRL(b, ctrl));
		/* [한국어] BAR 설정을 반영한다. 이 시점에 호스트가 이 BAR 를 읽으면
		 * 요청한 크기와 종류로 보인다. */
		cdns_pcie_writel(pcie, reg, cfg);
	}

	/* [한국어] 여기부터는 VF 별로 따로 있는 주소 변환 레지스터를 다룬다.
	 * BAR_CFG 는 VF 들이 공유하지만 인바운드 목적지 주소는 VF 마다 달라야 하므로,
	 * SR-IOV 라우팅 번호로 바꿔 각자의 레지스터를 찾는다. */
	fn = cdns_pcie_get_fn_from_vfn(pcie, fn, vfn);
	/* [한국어] 인바운드 주소 변환 레지스터에 로컬 주소 하위 워드를 쓴다.
	 * 이 순간부터 호스트가 이 BAR 범위에 보낸 메모리 TLP 가
	 * bar_phys 가 가리키는 로컬 메모리에 도착한다. */
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_IB_EP_FUNC_BAR_ADDR0(fn, bar),
			 addr0);
	/* [한국어] 상위 워드까지 쓰면 변환이 완성된다. */
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_IB_EP_FUNC_BAR_ADDR1(fn, bar),
			 addr1);

	/* [한국어] VF 라면 장부도 VF 전용 배열로 좁힌다. ep->epf[fn] 안의 epf 포인터가
	 * cdns_pcie_ep_setup() 에서 VF 개수만큼 할당된 배열을 가리킨다. */
	if (vfn > 0)
		epf = &epf->epf[vfn - 1];
	/* [한국어] 설정한 BAR 를 장부에 기록한다. 이 장부는 나중에
	 * cdns_pcie_ep_send_msix_irq() 가 MSI-X 테이블이 놓인 BAR 의
	 * CPU 쪽 가상 주소(epf_bar->addr)를 찾는 데 쓰인다. */
	epf->epf_bar[bar] = epf_bar;

	/* [한국어] 여기까지 오면 성공. 실패하는 경우는 위의 64비트 BAR 정렬 검사뿐이다. */
	return 0;
}

/* [한국어]
 * cdns_pcie_ep_clear_bar - 인바운드 BAR 를 닫는다 (epc_ops.clear_bar)
 *
 * @epc: EPC 객체.
 * @fn: 물리 함수 번호.
 * @vfn: 가상 함수 번호.
 * @epf_bar: 닫을 BAR 정보. barno 만 실제로 쓴다.
 * @return: 없음. EPC 규약의 clear_bar 는 void 라 실패를 알릴 방법이 없다.
 *
 * cdns_pcie_ep_set_bar() 의 대칭 함수다. 순서까지 그대로 뒤집혀 있다 —
 * BAR_CFG 의 종류 코드를 DISABLED 로 만들어 호스트에게 이 BAR 가 없는
 * 것처럼 보이게 하고, 인바운드 주소 변환 레지스터를 0 으로 지우고,
 * 마지막으로 장부에서 지운다.
 *
 * 왜 필요한가: BAR 를 열어 둔 채로 그 뒤의 로컬 버퍼를 해제하면 호스트가
 * 이미 없는 메모리에 쓰게 된다. EPF 드라이버가 언바인드될 때 반드시 거쳐야
 * 하는 경로다.
 *
 * VF 처리 규칙은 set_bar 과 같다 — BAR_CFG 는 VF #1 이 대표로,
 * 주소 변환은 각 VF 가 자기 것을 지운다.
 *
 * 실행 컨텍스트: EPC 코어가 epc->lock 을 쥔 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   EPF 드라이버 또는 pci_epc_deinit_notify 경로
 *     -> pci_epc_clear_bar() [pci-epc-core.c] -> [이 함수]
 *       -> cdns_pcie_get_fn_from_vfn() -> cdns_pcie_writel()
 */
static void cdns_pcie_ep_clear_bar(struct pci_epc *epc, u8 fn, u8 vfn,
				   struct pci_epf_bar *epf_bar)
{
	/* [한국어] EPC 코어가 저장해 둔 드라이버 private 포인터. */
	struct cdns_pcie_ep *ep = epc_get_drvdata(epc);
	/* [한국어] 이 물리 함수의 BAR 장부. VF 면 아래에서 좁힌다. */
	struct cdns_pcie_epf *epf = &ep->epf[fn];
	/* [한국어] 레지스터 접근용 공통 구조체. */
	struct cdns_pcie *pcie = &ep->pcie;
	/* [한국어] 지울 BAR 번호. */
	enum pci_barno bar = epf_bar->barno;
	/* [한국어] reg 는 BAR 설정 레지스터 오프셋, cfg 는 그 값,
	 * b 는 레지스터 안 필드 인덱스, ctrl 은 넣을 종류 코드(DISABLED)다. */
	u32 reg, cfg, b, ctrl;

	/* [한국어] set_bar 와 같은 규칙 — VF #1 이 VF 들을 대표해 설정을 갖는다. */
	if (vfn == 1)
		/* [한국어] VF BAR 설정 레지스터. */
		reg = CDNS_PCIE_LM_EP_VFUNC_BAR_CFG(bar, fn);
	else
		/* [한국어] PF BAR 설정 레지스터. */
		reg = CDNS_PCIE_LM_EP_FUNC_BAR_CFG(bar, fn);
	/* [한국어] BAR_4 이상은 두 번째 레지스터의 0번 자리부터 센다. */
	b = (bar < BAR_4) ? bar : bar - BAR_4;

	/* [한국어] PF 또는 대표 VF 일 때만 BAR_CFG 를 건드린다. */
	if (vfn == 0 || vfn == 1) {
		/* [한국어] 종류 코드 0x0 = DISABLED. 이 값을 넣으면 호스트에게 이 BAR 가
		 * 구현되지 않은 것으로 보인다(BAR 를 읽으면 0 이 돌아온다). */
		ctrl = CDNS_PCIE_LM_BAR_CFG_CTRL_DISABLED;
		/* [한국어] 다른 BAR 의 설정을 보존해야 하므로 읽고-고쳐-쓰기. */
		cfg = cdns_pcie_readl(pcie, reg);
		/* [한국어] 이번 BAR 자리의 aperture 와 ctrl 필드를 지운다. */
		cfg &= ~(CDNS_PCIE_LM_EP_FUNC_BAR_CFG_BAR_APERTURE_MASK(b) |
			 CDNS_PCIE_LM_EP_FUNC_BAR_CFG_BAR_CTRL_MASK(b));
		/* [한국어] aperture 는 0 인 채로 두고 ctrl 만 DISABLED 로 넣는다.
		 * ctrl 이 DISABLED 면 aperture 값은 의미가 없다. */
		cfg |= CDNS_PCIE_LM_EP_FUNC_BAR_CFG_BAR_CTRL(b, ctrl);
		/* [한국어] BAR 를 끈다. */
		cdns_pcie_writel(pcie, reg, cfg);
	}

	/* [한국어] 주소 변환 레지스터는 VF 마다 따로이므로 라우팅 번호로 바꾼다. */
	fn = cdns_pcie_get_fn_from_vfn(pcie, fn, vfn);
	/* [한국어] 인바운드 변환 목적지 주소를 0 으로 지운다. */
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_IB_EP_FUNC_BAR_ADDR0(fn, bar), 0);
	/* [한국어] 상위 워드까지 지운다. */
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_IB_EP_FUNC_BAR_ADDR1(fn, bar), 0);

	/* [한국어] VF 면 장부도 VF 전용 배열로 좁힌다. */
	if (vfn > 0)
		epf = &epf->epf[vfn - 1];
	/* [한국어] 장부에서 이 BAR 를 지운다. set_bar 의 기록과 대칭이다. */
	epf->epf_bar[bar] = NULL;
}

/* [한국어]
 * cdns_pcie_ep_map_addr - 호스트 메모리로 나가는 아웃바운드 창을 연다 (epc_ops.map_addr)
 *
 * @epc: EPC 객체.
 * @fn: 물리 함수 번호.
 * @vfn: 가상 함수 번호.
 * @addr: 이 창에 대응할 로컬(CPU) 쪽 물리 주소. EPF 드라이버가
 *        pci_epc_mem_alloc_addr 로 잡아 온 주소다.
 * @pci_addr: 그 접근이 도달할 호스트 쪽 PCI 주소.
 * @size: 창 크기.
 * @return: 0 이면 성공. 빈 창이 없으면 -EINVAL.
 *
 * BAR 가 "호스트가 나를 보는 창" 이라면, 이것은 "내가 호스트를 보는 창" 이다.
 * 이 창을 연 뒤 로컬 CPU 가 addr 에 읽고 쓰면 그것이 그대로 호스트 메모리의
 * pci_addr 에 대한 TLP 가 되어 나간다. 엔드포인트가 호스트 메모리의 큐나
 * 버퍼를 직접 만지는 통로가 이것이다.
 *
 * 창 관리는 ob_region_map 비트맵으로 한다. 0번 비트는 cdns_pcie_ep_setup()
 * 이 인터럽트 전용으로 미리 세워 두었으므로 여기서는 잡히지 않는다.
 *
 * 실행 컨텍스트: EPC 코어가 epc->lock(뮤텍스)을 쥔 프로세스 컨텍스트.
 *   find_first_zero_bit 과 set_bit 사이가 원자적이지 않지만, 그 뮤텍스가
 *   두 map_addr 이 겹치는 것을 막아 준다. 다만 이 파일 안의
 *   cdns_pcie_ep_map_msi_irq() 도 이 함수를 직접 부르는데, 그 경로 역시
 *   같은 뮤텍스 아래에서 시작하므로 문제가 되지 않는다.
 *
 * 에러 경로: 창이 모자라면 dev_err 로 남기고 -EINVAL. 레지스터는 손대지 않는다.
 *
 * 호출 체인:
 *   EPF 드라이버 -> pci_epc_map_addr() [pci-epc-core.c] -> [이 함수]
 *   또는 cdns_pcie_ep_map_msi_irq() [이 파일] -> [이 함수]
 *     -> cdns_pcie_get_fn_from_vfn() -> cdns_pcie_set_outbound_region()
 */
static int cdns_pcie_ep_map_addr(struct pci_epc *epc, u8 fn, u8 vfn,
				 phys_addr_t addr, u64 pci_addr, size_t size)
{
	/* [한국어] EPC 코어가 저장해 둔 드라이버 private 포인터. */
	struct cdns_pcie_ep *ep = epc_get_drvdata(epc);
	/* [한국어] 레지스터 접근용 공통 구조체. */
	struct cdns_pcie *pcie = &ep->pcie;
	/* [한국어] 고른 아웃바운드 창 번호. */
	u32 r;

	/* [한국어] ob_region_map 비트맵에서 처음 비어 있는 자리를 찾는다.
	 * 0번 비트는 cdns_pcie_ep_setup() 이 인터럽트 전용으로 미리 세워 두었으므로
	 * 여기서는 1번 이후만 잡힌다. BITS_PER_LONG 을 한계로 주는 것은
	 * 비트맵이 unsigned long 하나이기 때문이다. */
	r = find_first_zero_bit(&ep->ob_region_map, BITS_PER_LONG);
	/* [한국어] 쓸 수 있는 상한 검사. 조건이 max_regions - 1 이므로 실제로 배정되는 번호는
	 * 1 부터 max_regions - 2 까지다. 0번을 인터럽트용으로 뺀 것 말고도
	 * 마지막 한 자리가 더 빠지는 셈인데, 그 이유는 이 트리에서 확인할 수 없다.
	 * (아래 cdns_pcie_ep_unmap_addr 의 탐색 루프도 같은 상한을 쓴다.) */
	if (r >= ep->max_regions - 1) {
		/* [한국어] 창이 모자라면 EPF 드라이버에게 알린다. 아웃바운드 창 개수는
		 * 디바이스 트리의 cdns,max-outbound-regions 로 정해진다. */
		dev_err(&epc->dev, "no free outbound region\n");
		/* [한국어] EPC 코어를 거쳐 EPF 드라이버로 올라간다. */
		return -EINVAL;
	}

	/* [한국어] VF 면 SR-IOV 라우팅 번호로 바꾼다. 나가는 TLP 의 requester ID 에
	 * 어느 함수가 낸 요청인지가 실려야 하기 때문이다. */
	fn = cdns_pcie_get_fn_from_vfn(pcie, fn, vfn);
	/* [한국어] 아웃바운드 창을 연다. 두 번째 인자 busnr 이 0 인 것은 EP 모드에서
	 * 버스 번호를 소프트웨어가 정하지 않기 때문이다 — 호스트가 열거하며
	 * 알려 준 값을 하드웨어가 기억해 두었다가 쓴다(pcie-cadence.c 의
	 * cdns_pcie_set_outbound_region 안 is_rc 분기 참고).
	 * is_io 는 false — 엔드포인트가 호스트 메모리를 읽고 쓰는 창이다. */
	cdns_pcie_set_outbound_region(pcie, 0, fn, r, false, addr, pci_addr, size);

	/* [한국어] 창을 점유로 표시한다. set_bit 은 원자적 연산이지만, 위의
	 * find_first_zero_bit 과 묶여 있지 않아 그 자체로는 경쟁을 막지 못한다.
	 * 실제 직렬화는 EPC 코어가 쥔 epc->lock 뮤텍스가 담당한다
	 * (drivers/pci/endpoint/pci-epc-core.c 의 pci_epc_map_addr). */
	set_bit(r, &ep->ob_region_map);
	/* [한국어] 나중에 unmap 할 때 주소로 창을 되찾을 수 있게 기록해 둔다. */
	ep->ob_addr[r] = addr;

	/* [한국어] 여기까지 오면 성공. */
	return 0;
}

/* [한국어]
 * cdns_pcie_ep_unmap_addr - 아웃바운드 창을 닫는다 (epc_ops.unmap_addr)
 *
 * @epc: EPC 객체.
 * @fn: 물리 함수 번호. 이 함수에서는 쓰이지 않는다 — 창은 함수별이 아니라
 *      컨트롤러 전체에서 공유하는 자원이기 때문이다.
 * @vfn: 가상 함수 번호. 역시 쓰이지 않는다.
 * @addr: 닫을 창의 로컬 쪽 주소. map_addr 에 넘겼던 것과 같은 값이어야 한다.
 * @return: 없음.
 *
 * map_addr 이 ob_addr[] 에 기록해 둔 주소로 창 번호를 되찾아 지운다.
 * 창 개수가 많아야 32개(CDNS_PCIE_MAX_OB)라 선형 탐색으로 충분하다.
 *
 * 정리 순서가 눈여겨볼 만하다 — 레지스터를 먼저 지우고 그 다음에 비트맵을
 * 푼다. 반대로 하면 비트를 푼 직후 다른 경로가 그 창을 잡았을 때 아직
 * 이전 설정이 살아 있는 창을 받게 된다.
 *
 * 실행 컨텍스트: EPC 코어가 epc->lock 을 쥔 프로세스 컨텍스트.
 *
 * 에러 경로: 그런 창이 없으면 조용히 돌아간다. void 반환이라 알릴 방법이 없다.
 *
 * 호출 체인:
 *   EPF 드라이버 -> pci_epc_unmap_addr() [pci-epc-core.c] -> [이 함수]
 *     -> cdns_pcie_reset_outbound_region() [pcie-cadence.c]
 */
static void cdns_pcie_ep_unmap_addr(struct pci_epc *epc, u8 fn, u8 vfn,
				    phys_addr_t addr)
{
	/* [한국어] EPC 코어가 저장해 둔 드라이버 private 포인터. */
	struct cdns_pcie_ep *ep = epc_get_drvdata(epc);
	/* [한국어] 레지스터 접근용 공통 구조체. */
	struct cdns_pcie *pcie = &ep->pcie;
	/* [한국어] 찾은 창 번호. */
	u32 r;

	/* [한국어] 주소로 창을 되찾는 선형 탐색. 창 개수가 많아야 32개(CDNS_PCIE_MAX_OB)라
	 * 선형 탐색으로 충분하다. 0번은 인터럽트 전용이지만 여기서 배제하지 않는데,
	 * ep->ob_addr[0] 에는 아무도 값을 넣지 않으므로 0 이 아닌 addr 와는 맞지 않는다. */
	for (r = 0; r < ep->max_regions - 1; r++)
		/* [한국어] map_addr 이 기록해 둔 CPU 쪽 주소와 일치하는 창을 찾는다. */
		if (ep->ob_addr[r] == addr)
			break;

	/* [한국어] 루프를 끝까지 돌고 나온 경우 — 그런 창이 없다.
	 * 조용히 돌아간다. EPC 규약의 unmap_addr 은 void 라 오류를 알릴 길이 없다. */
	if (r == ep->max_regions - 1)
		return;

	/* [한국어] 창의 여섯 레지스터를 모두 0 으로 지운다(pcie-cadence.c).
	 * 이 순간부터 그 CPU 주소에 접근해도 PCIe 로 나가지 않는다. */
	cdns_pcie_reset_outbound_region(pcie, r);

	/* [한국어] 장부를 지운다. */
	ep->ob_addr[r] = 0;
	/* [한국어] 비트맵에서 점유를 푼다. 레지스터를 먼저 끄고 비트를 나중에 푸는 순서라,
	 * 다른 경로가 이 창을 다시 잡더라도 이미 꺼진 창을 받게 된다. */
	clear_bit(r, &ep->ob_region_map);
}

/* [한국어]
 * cdns_pcie_ep_set_msi - 쓸 MSI 개수를 호스트에게 광고한다 (epc_ops.set_msi)
 *
 * @epc: EPC 객체.
 * @fn: 물리 함수 번호.
 * @vfn: 가상 함수 번호.
 * @nr_irqs: EPF 드라이버가 원하는 인터럽트 개수.
 * @return: 항상 0. 레지스터 쓰기뿐이라 실패할 일이 없다.
 *
 * MSI 협상의 첫 단계다. 흐름은 이렇다.
 *   1. 엔드포인트가 "나는 최대 2^mmc 개를 쓸 수 있다" 고 능력 레지스터에 적는다
 *      — 이 함수.
 *   2. 호스트가 열거하며 그것을 읽고, 실제로 몇 개를 줄지 정해
 *      Multiple Message Enable 필드에 적고 MSI Enable 을 켠다.
 *   3. 엔드포인트가 그 값을 읽어 실제 개수를 안다 — cdns_pcie_ep_get_msi().
 *
 * MSI 는 개수를 2의 거듭제곱으로만 요청할 수 있고 레지스터에는 그 지수를
 * 적는다. order_base_2() 가 올림한 지수를 준다.
 *
 * 함께 하는 두 가지 설정이 더 있다. 64비트 주소 능력을 켜는 것은 호스트가
 * 4GB 위쪽 MSI 목적지를 줄 수 있게 하려는 것이고(아래 send 경로가
 * ADDRESS_HI 를 읽는 것과 짝이다), 벡터별 마스킹 능력을 끄는 것은 이 IP 에
 * 그 레지스터가 없기 때문이다.
 *
 * 실행 컨텍스트: EPC 코어가 epc->lock 을 쥔 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   EPF 드라이버 -> pci_epc_set_msi() [pci-epc-core.c] -> [이 함수]
 *     -> cdns_pcie_find_capability() / cdns_pcie_ep_fn_readw/writew()
 */
static int cdns_pcie_ep_set_msi(struct pci_epc *epc, u8 fn, u8 vfn, u8 nr_irqs)
{
	/* [한국어] EPC 코어가 저장해 둔 드라이버 private 포인터. */
	struct cdns_pcie_ep *ep = epc_get_drvdata(epc);
	/* [한국어] 레지스터 접근용 공통 구조체. */
	struct cdns_pcie *pcie = &ep->pcie;
	/* [한국어] Multiple Message Capable 필드에 넣을 값. MSI 는 개수를 2의 거듭제곱으로만
	 * 요청할 수 있고 레지스터에는 그 지수를 적는다. order_base_2(5) 는 3(=8개)처럼
	 * 올림한 지수를 준다. */
	u8 mmc = order_base_2(nr_irqs);
	/* [한국어] Message Control 레지스터 값. */
	u16 flags;
	/* [한국어] MSI capability 의 config space 오프셋. */
	u8 cap;

	/* [한국어] MSI 는 표준 capability(오프셋 0x40 이후 연결 리스트)라
	 * find_capability 를 쓴다. 확장 capability 인 SR-IOV 와는 탐색 방법이 다르다. */
	cap = cdns_pcie_find_capability(pcie, PCI_CAP_ID_MSI);
	/* [한국어] VF 면 SR-IOV 라우팅 번호로 바꾼다. VF 마다 자기 config space 가 있다. */
	fn = cdns_pcie_get_fn_from_vfn(pcie, fn, vfn);

	/*
	 * Set the Multiple Message Capable bitfield into the Message Control
	 * register.
	 */
	/* [한국어] Message Control 레지스터를 읽는다. 여기에 MSI 의 능력과 활성 상태가
	 * 모두 들어 있다. */
	flags = cdns_pcie_ep_fn_readw(pcie, fn, cap + PCI_MSI_FLAGS);
	/* [한국어] Multiple Message Capable 필드(비트 [3:1])만 갈아 끼운다.
	 * QMASK 로 지우고 mmc 를 1비트 밀어 넣는 것이 그 자리에 정확히 대응한다.
	 * 이 값이 "나는 인터럽트를 최대 2^mmc 개까지 쓸 수 있다" 는 광고다. */
	flags = (flags & ~PCI_MSI_FLAGS_QMASK) | (mmc << 1);
	/* [한국어] 64비트 주소 지원(비트 7)을 켠다. 이것을 켜야 호스트가 4GB 위쪽
	 * MSI 목적지 주소를 줄 수 있고, 아래 send 경로가 ADDRESS_HI 를 읽는 것과 짝이 맞다. */
	flags |= PCI_MSI_FLAGS_64BIT;
	/* [한국어] Per-Vector Masking 지원(비트 8)을 끈다. 이 IP 에는 벡터별 마스크
	 * 레지스터가 없으므로 능력이 없다고 알린다. */
	flags &= ~PCI_MSI_FLAGS_MASKBIT;
	/* [한국어] 고친 Message Control 을 되쓴다. 호스트는 이 값을 보고 실제로 몇 개를
	 * 할당할지 정해 Multiple Message Enable 필드에 적어 준다. */
	cdns_pcie_ep_fn_writew(pcie, fn, cap + PCI_MSI_FLAGS, flags);

	/* [한국어] 레지스터 쓰기뿐이라 실패할 일이 없다. */
	return 0;
}

/* [한국어]
 * cdns_pcie_ep_get_msi - 호스트가 실제로 허락한 MSI 개수를 읽는다 (epc_ops.get_msi)
 *
 * @epc: EPC 객체.
 * @fn: 물리 함수 번호.
 * @vfn: 가상 함수 번호.
 * @return: 허락된 인터럽트 개수(1, 2, 4, ... 32). 호스트가 아직 MSI 를 켜지
 *          않았으면 -EINVAL.
 *
 * MSI 협상의 마지막 단계다. cdns_pcie_ep_set_msi() 가 광고한 능력에 대해
 * 호스트가 실제로 몇 개를 줬는지를 Multiple Message Enable 필드에서 읽는다.
 * 능력보다 적게 줄 수 있으므로 반드시 이쪽을 읽어야 한다.
 *
 * -EINVAL 의 의미가 오류라기보다 "아직 준비 안 됨" 에 가깝다. 호스트가
 * 열거를 마치고 MSI 를 켜기 전에는 이 값을 알 수 없으므로, EPF 드라이버는
 * 보통 이것을 폴링하거나 링크업 통지를 기다린 뒤 부른다.
 *
 * 실행 컨텍스트: EPC 코어가 epc->lock 을 쥔 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   EPF 드라이버 -> pci_epc_get_msi() [pci-epc-core.c] -> [이 함수]
 *     -> cdns_pcie_find_capability() / cdns_pcie_ep_fn_readw()
 */
static int cdns_pcie_ep_get_msi(struct pci_epc *epc, u8 fn, u8 vfn)
{
	/* [한국어] EPC 코어가 저장해 둔 드라이버 private 포인터. */
	struct cdns_pcie_ep *ep = epc_get_drvdata(epc);
	/* [한국어] 레지스터 접근용 공통 구조체. */
	struct cdns_pcie *pcie = &ep->pcie;
	/* [한국어] flags 는 Message Control 값, mme 는 그 안의 Multiple Message Enable 필드. */
	u16 flags, mme;
	/* [한국어] MSI capability 오프셋. */
	u8 cap;

	/* [한국어] MSI capability 를 찾는다. */
	cap = cdns_pcie_find_capability(pcie, PCI_CAP_ID_MSI);
	/* [한국어] VF 면 라우팅 번호로 바꾼다. */
	fn = cdns_pcie_get_fn_from_vfn(pcie, fn, vfn);

	/* Validate that the MSI feature is actually enabled. */
	/* [한국어] Message Control 을 읽는다. */
	flags = cdns_pcie_ep_fn_readw(pcie, fn, cap + PCI_MSI_FLAGS);
	/* [한국어] MSI Enable(비트 0)은 호스트가 세우는 비트다.
	 * 꺼져 있으면 호스트가 아직 MSI 를 켜지 않았다는 뜻이므로 개수를 물어도 의미가 없다. */
	if (!(flags & PCI_MSI_FLAGS_ENABLE))
		/* [한국어] EPF 드라이버는 이 오류를 보고 아직 준비되지 않았다고 판단한다. */
		return -EINVAL;

	/*
	 * Get the Multiple Message Enable bitfield from the Message Control
	 * register.
	 */
	/* [한국어] Multiple Message Enable(비트 [6:4])을 꺼낸다.
	 * 이것은 위 set_msi 가 광고한 능력에 대해 호스트가 실제로 허락한 개수의 지수다.
	 * 능력보다 적게 줄 수 있으므로 반드시 이쪽을 읽어야 한다. */
	mme = FIELD_GET(PCI_MSI_FLAGS_QSIZE, flags);

	/* [한국어] 지수를 개수로 되돌린다. 예: mme 가 2 면 4개.
	 * EPF 드라이버는 이 개수 안에서만 인터럽트 번호를 고를 수 있다. */
	return 1 << mme;
}

/* [한국어]
 * cdns_pcie_ep_get_msix - 호스트가 허락한 MSI-X 개수를 읽는다 (epc_ops.get_msix)
 *
 * @epc: EPC 객체.
 * @func_no: 물리 함수 번호. 다른 함수들이 fn 이라고 쓰는 것과 이름만 다르다.
 * @vfunc_no: 가상 함수 번호.
 * @return: 테이블 크기(= 벡터 개수). 호스트가 MSI-X 를 켜지 않았으면 -EINVAL.
 *
 * MSI 쪽 get_msi 와 대응하지만 세부가 다르다. MSI-X 에는 Multiple Message
 * Enable 같은 별도 필드가 없고, Message Control 의 Table Size 필드가 곧
 * 벡터 개수다. 그 값은 N-1 로 인코딩되므로 1 을 더해야 한다.
 *
 * 또 MSI 와 달리 개수가 2의 거듭제곱일 필요가 없다. MSI-X 는 벡터마다
 * 주소와 데이터를 테이블에 따로 두기 때문에 임의의 개수가 가능하다.
 *
 * 다만 여기서 읽는 Table Size 는 이 엔드포인트가 cdns_pcie_ep_set_msix() 로
 * 적어 둔 값 그대로다. MSI 처럼 호스트가 개수를 깎는 구조가 아니라,
 * 호스트는 Enable 비트만 켜고 테이블을 채운다.
 *
 * 실행 컨텍스트: EPC 코어가 epc->lock 을 쥔 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   EPF 드라이버 -> pci_epc_get_msix() [pci-epc-core.c] -> [이 함수]
 *     -> cdns_pcie_find_capability() / cdns_pcie_ep_fn_readw()
 */
static int cdns_pcie_ep_get_msix(struct pci_epc *epc, u8 func_no, u8 vfunc_no)
{
	/* [한국어] EPC 코어가 저장해 둔 드라이버 private 포인터. */
	struct cdns_pcie_ep *ep = epc_get_drvdata(epc);
	/* [한국어] 레지스터 접근용 공통 구조체. */
	struct cdns_pcie *pcie = &ep->pcie;
	/* [한국어] val 은 Message Control 값, reg 는 그 오프셋. */
	u32 val, reg;
	/* [한국어] MSI-X capability 오프셋. */
	u8 cap;

	/* [한국어] MSI-X 도 표준 capability 다. */
	cap = cdns_pcie_find_capability(pcie, PCI_CAP_ID_MSIX);
	/* [한국어] VF 면 라우팅 번호로 바꾼다. */
	func_no = cdns_pcie_get_fn_from_vfn(pcie, func_no, vfunc_no);

	/* [한국어] MSI-X capability 의 Message Control 레지스터. */
	reg = cap + PCI_MSIX_FLAGS;
	/* [한국어] Message Control 을 읽는다. */
	val = cdns_pcie_ep_fn_readw(pcie, func_no, reg);
	/* [한국어] MSI-X Enable(비트 15)은 호스트가 세운다. 꺼져 있으면 아직 쓸 수 없다. */
	if (!(val & PCI_MSIX_FLAGS_ENABLE))
		/* [한국어] EPF 드라이버에게 아직 준비되지 않았음을 알린다. */
		return -EINVAL;

	/* [한국어] Table Size 필드(비트 [10:0])만 남긴다. */
	val &= PCI_MSIX_FLAGS_QSIZE;

	/* [한국어] Table Size 는 N-1 로 인코딩되므로 1 을 더해야 실제 개수가 된다
	 * (PCIe 규격의 MSI-X Message Control 정의).
	 * MSI 와 달리 MSI-X 는 2의 거듭제곱일 필요가 없다. */
	return val + 1;
}

/* [한국어]
 * cdns_pcie_ep_set_msix - MSI-X 테이블 크기와 위치를 설정한다 (epc_ops.set_msix)
 *
 * @epc: EPC 객체.
 * @fn: 물리 함수 번호.
 * @vfn: 가상 함수 번호.
 * @nr_irqs: 벡터 개수.
 * @bir: MSI-X 테이블과 PBA 가 놓일 BAR 번호(BAR Indicator Register).
 * @offset: 그 BAR 안에서 테이블이 시작할 오프셋.
 * @return: 항상 0. 레지스터 쓰기뿐이다.
 *
 * MSI 와 MSI-X 의 구조적 차이가 이 함수에 드러난다. MSI 는 주소와 데이터를
 * config space 안의 capability 에 직접 두지만, MSI-X 는 BAR 뒤의 메모리에
 * 테이블을 두고 capability 에는 "그 테이블이 몇 번 BAR 의 어느 오프셋에
 * 있는가" 만 적는다. 그래서 벡터 수가 훨씬 많아질 수 있다.
 *
 * 여기서 하는 일이 셋이다.
 *   1. Table Size 를 N-1 로 인코딩해 Message Control 에 적는다.
 *   2. Table Offset/BIR 레지스터에 테이블 위치를 적는다. 오프셋이 8바이트
 *      정렬이라 하위 3비트가 비고, 그 자리에 BIR 을 그대로 OR 한다.
 *   3. PBA(Pending Bit Array) 위치를 테이블 바로 뒤에 적는다. 항목 하나가
 *      16바이트(PCI_MSIX_ENTRY_SIZE)이므로 nr_irqs * 16 만큼 뒤다.
 *      상류 주석이 밝히듯 PBA 의 BAR 는 테이블과 같아야 한다.
 *
 * 실제 테이블 내용은 호스트가 BAR 를 통해 써 넣는다. 엔드포인트는
 * cdns_pcie_ep_send_msix_irq() 에서 그것을 읽어 쓴다.
 *
 * 실행 컨텍스트: EPC 코어가 epc->lock 을 쥔 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   EPF 드라이버 -> pci_epc_set_msix() [pci-epc-core.c] -> [이 함수]
 *     -> cdns_pcie_find_capability() / cdns_pcie_ep_fn_readw/writew/writel()
 */
static int cdns_pcie_ep_set_msix(struct pci_epc *epc, u8 fn, u8 vfn,
				 u16 nr_irqs, enum pci_barno bir, u32 offset)
{
	/* [한국어] EPC 코어가 저장해 둔 드라이버 private 포인터. */
	struct cdns_pcie_ep *ep = epc_get_drvdata(epc);
	/* [한국어] 레지스터 접근용 공통 구조체. */
	struct cdns_pcie *pcie = &ep->pcie;
	/* [한국어] val 은 레지스터 값, reg 는 오프셋. */
	u32 val, reg;
	/* [한국어] MSI-X capability 오프셋. */
	u8 cap;

	/* [한국어] MSI-X capability 를 찾는다. */
	cap = cdns_pcie_find_capability(pcie, PCI_CAP_ID_MSIX);
	/* [한국어] VF 면 라우팅 번호로 바꾼다. */
	fn = cdns_pcie_get_fn_from_vfn(pcie, fn, vfn);

	/* [한국어] Message Control 레지스터. */
	reg = cap + PCI_MSIX_FLAGS;
	/* [한국어] 기존 값을 읽어 Enable 같은 다른 비트를 보존한다. */
	val = cdns_pcie_ep_fn_readw(pcie, fn, reg);
	/* [한국어] Table Size 필드를 지운다. */
	val &= ~PCI_MSIX_FLAGS_QSIZE;
	/* [한국어] N-1 인코딩으로 개수를 넣는다(상류 주석 "encoded as N-1"). */
	val |= nr_irqs - 1; /* encoded as N-1 */
	/* [한국어] Table Size 를 반영한다. */
	cdns_pcie_ep_fn_writew(pcie, fn, reg, val);

	/* Set MSI-X BAR and offset */
	/* [한국어] Table Offset/BIR 레지스터. 상위 29비트가 오프셋, 하위 3비트가 BIR 이다. */
	reg = cap + PCI_MSIX_TABLE;
	/* [한국어] BIR(BAR Indicator Register)은 MSI-X 테이블이 몇 번 BAR 안에 있는지를,
	 * offset 은 그 BAR 안에서의 위치를 가리킨다.
	 * offset 은 8바이트 정렬이라 하위 3비트가 비어 있고, 그 자리에 bir 을 그대로
	 * OR 하면 규격이 요구하는 배치가 된다. */
	val = offset | bir;
	/* [한국어] 테이블 위치를 호스트에 알린다. 호스트는 이 정보를 보고 해당 BAR 의
	 * 그 오프셋에 벡터별 주소와 데이터를 써 넣는다. */
	cdns_pcie_ep_fn_writel(pcie, fn, reg, val);

	/* Set PBA BAR and offset.  BAR must match MSI-X BAR */
	/* [한국어] PBA(Pending Bit Array) Offset/BIR 레지스터. 형식은 테이블 쪽과 같다. */
	reg = cap + PCI_MSIX_PBA;
	/* [한국어] PBA 를 테이블 바로 뒤에 놓는다. PCI_MSIX_ENTRY_SIZE 는 16바이트이므로
	 * nr_irqs * 16 이 테이블 크기이고, 그만큼 뒤가 PBA 시작이다.
	 * 상류 주석대로 BAR 는 테이블과 같아야 하므로 같은 bir 을 OR 한다. */
	val = (offset + (nr_irqs * PCI_MSIX_ENTRY_SIZE)) | bir;
	/* [한국어] PBA 위치를 호스트에 알린다. */
	cdns_pcie_ep_fn_writel(pcie, fn, reg, val);

	/* [한국어] 레지스터 쓰기뿐이라 실패할 일이 없다. */
	return 0;
}

/* [한국어]
 * cdns_pcie_ep_assert_intx - INTx 어서트/해제 메시지 TLP 를 한 발 보낸다
 *
 * @ep: 이 드라이버의 엔드포인트 상태.
 * @fn: 물리 함수 번호. VF 는 INTx 를 쓸 수 없으므로 언제나 PF 다.
 * @intx: 어느 선인지. 0=INTA, 1=INTB, 2=INTC, 3=INTD.
 * @is_asserted: true 면 Assert_INTx, false 면 Deassert_INTx.
 * @return: 없음.
 *
 * PCIe 에는 레거시 PCI 의 INTA#~INTD# 물리 핀이 없다. 대신 Assert_INTx /
 * Deassert_INTx 라는 메시지 TLP 로 그 동작을 흉내 낸다(PCIe r6.0 sec 2.2.8.1).
 * 이 함수가 그 메시지 한 발을 만들어 보낸다.
 *
 * 메시지를 보내는 방법이 독특하다. 데이터를 어딘가에 쓰는 것이 아니라,
 * 메시지 전용으로 설정된 아웃바운드 창 안의 "특정 오프셋" 에 쓰는 것이
 * 곧 메시지 전송이다. 오프셋의 비트 [7:5] 가 Message Routing 이고
 * [15:8] 이 Message Code 다. 그래서 쓰는 값(0)은 의미가 없고 주소만 의미가 있다.
 *
 * 동작 단계:
 *   1. 창 0번이 아직 INTx 메시지용으로 설정되어 있지 않거나 다른 함수용이면
 *      메시지 전용 창으로 다시 설정한다.
 *   2. irq_pending 비트맵을 갱신하고 메시지 코드를 고른다.
 *   3. PCI_STATUS 의 Interrupt Status 비트를 보류 상태와 맞춘다.
 *      이것만 ep->lock 스핀락으로 보호한다.
 *   4. 창 안의 인코딩된 오프셋에 써서 메시지를 내보낸다.
 *
 * 실행 컨텍스트: EPC 코어의 epc->lock 아래에서 시작되지만, ep->lock 을
 *   spin_lock_irqsave 로 잡는 것으로 보아 인터럽트 문맥도 염두에 둔 코드다.
 *   그 락이 보호하는 것은 PCI_STATUS 의 읽고-고쳐-쓰기 구간뿐이며,
 *   ep->irq_pending 갱신은 락 밖에서 이뤄진다.
 *
 * 에러 경로: 없다. void 반환이다.
 *
 * 호출 체인:
 *   cdns_pcie_ep_raise_irq() -> cdns_pcie_ep_send_intx_irq() -> [이 함수]
 *     -> cdns_pcie_set_outbound_region_for_normal_msg() [pcie-cadence.c]
 *     -> cdns_pcie_ep_fn_readw/writew() -> writel()
 */
static void cdns_pcie_ep_assert_intx(struct cdns_pcie_ep *ep, u8 fn, u8 intx,
				     bool is_asserted)
{
	/* [한국어] 레지스터 접근용 공통 구조체. */
	struct cdns_pcie *pcie = &ep->pcie;
	/* [한국어] spin_lock_irqsave 가 저장할 인터럽트 상태. */
	unsigned long flags;
	/* [한국어] 메시지 창 안에서 라우팅과 코드를 인코딩할 오프셋. */
	u32 offset;
	/* [한국어] config space 의 PCI_STATUS 값. */
	u16 status;
	/* [한국어] 보낼 메시지의 Message Code(Assert_INTA 계열). */
	u8 msg_code;

	/* [한국어] INTA~INTD 는 넷뿐이므로 하위 2비트만 쓴다. 상위 비트가 실려 오면
	 * 엉뚱한 메시지 코드가 만들어지므로 여기서 자른다. */
	intx &= 3;

	/* Set the outbound region if needed. */
	/* [한국어] 아웃바운드 0번 창이 지금 INTx 메시지용으로 설정되어 있고 함수도 같다면
	 * 다시 설정할 필요가 없다. 인터럽트 경로는 자주 불리므로 unlikely 로
	 * 분기 예측을 돕는다 — 보통은 이미 설정되어 있다는 뜻이다. */
	if (unlikely(ep->irq_pci_addr != CDNS_PCIE_EP_IRQ_PCI_ADDR_LEGACY ||
		     ep->irq_pci_fn != fn)) {
		/* First region was reserved for IRQ writes. */
		/* [한국어] 0번 창을 메시지 TLP 전용으로 설정한다. busnr 인자가 0 인 것은
		 * EP 모드에서 버스 번호를 하드웨어가 기억해 둔 값으로 채우기 때문이다.
		 * 창 번호도 0 — cdns_pcie_ep_setup() 이 인터럽트 전용으로 예약해 둔 자리다. */
		cdns_pcie_set_outbound_region_for_normal_msg(pcie, 0, fn, 0,
							     ep->irq_phys_addr);
		/* [한국어] 이제 이 창이 INTx 메시지용이라는 표시를 남긴다.
		 * 다음 호출에서 위 조건이 참이 되어 재설정을 건너뛰게 된다. */
		ep->irq_pci_addr = CDNS_PCIE_EP_IRQ_PCI_ADDR_LEGACY;
		/* [한국어] 어느 함수용으로 설정했는지도 기록한다. 다중 함수 장치에서는
		 * 함수가 바뀌면 requester ID 가 달라지므로 창을 다시 설정해야 한다. */
		ep->irq_pci_fn = fn;
	}

	/* [한국어] 어서트인지 해제인지에 따라 보류 비트와 메시지 코드가 갈린다. */
	if (is_asserted) {
		/* [한국어] 이 INTx 선이 어서트되었음을 기록한다. 네 선을 비트로 관리한다. */
		ep->irq_pending |= BIT(intx);
		/* [한국어] Assert_INTA(0x20)에 선 번호를 더해 INTA~INTD 코드를 만든다.
		 * drivers/pci/pci.h 에서 0x20~0x23 이 A~D 로 연속이라 덧셈이 성립한다. */
		msg_code = PCIE_MSG_CODE_ASSERT_INTA + intx;
	} else {
		/* [한국어] 해제. 해당 비트를 내린다. */
		ep->irq_pending &= ~BIT(intx);
		/* [한국어] Deassert_INTA(0x24)에 선 번호를 더한다. 역시 0x24~0x27 이 A~D 로 연속이다.
		 * INTx 는 레벨 트리거라 반드시 assert 와 deassert 가 짝을 이뤄야 한다. */
		msg_code = PCIE_MSG_CODE_DEASSERT_INTA + intx;
	}

	/* [한국어] 여기부터 PCI_STATUS 의 읽고-고쳐-쓰기를 보호한다.
	 * 헤더의 주석대로 이 락의 목적은 그 구간을 짧게 유지하는 것이다 —
	 * PCI_STATUS 는 호스트(원격 RC)와 이 EP 가 모두 접근할 수 있는 레지스터라,
	 * 읽기와 쓰기 사이가 벌어질수록 그 사이에 호스트가 끼어들 여지가 커진다.
	 * irqsave 를 쓰는 것은 이 함수가 인터럽트 문맥에서 불릴 수도 있기 때문이다.
	 * 다만 바로 위의 ep->irq_pending 갱신은 락 밖에서 이뤄진다. */
	spin_lock_irqsave(&ep->lock, flags);
	/* [한국어] 현재 Status 레지스터를 읽는다. */
	status = cdns_pcie_ep_fn_readw(pcie, fn, PCI_STATUS);
	/* [한국어] Interrupt Status 비트(비트 3)와 실제 보류 상태가 어긋났는지를 XOR 로 본다.
	 * 둘 다 참이거나 둘 다 거짓이면 손댈 것이 없고, 어긋났을 때만 뒤집는다.
	 * 불필요한 쓰기를 줄여 호스트와 겹칠 기회를 줄이는 구조다. */
	if (((status & PCI_STATUS_INTERRUPT) != 0) ^ (ep->irq_pending != 0)) {
		/* [한국어] 해당 비트만 뒤집는다. 어긋난 방향이 무엇이든 XOR 한 번이면 맞는다. */
		status ^= PCI_STATUS_INTERRUPT;
		/* [한국어] Status 를 되쓴다. 이 비트는 호스트가 "이 함수가 INTx 를 걸었는가" 를
		 * 확인할 때 읽는 자리다. */
		cdns_pcie_ep_fn_writew(pcie, fn, PCI_STATUS, status);
	}
	/* [한국어] 락을 풀고 인터럽트 상태를 복원한다. */
	spin_unlock_irqrestore(&ep->lock, flags);

	/* [한국어] 메시지 창 안의 오프셋으로 라우팅과 코드를 인코딩한다.
	 * 주소 비트 [7:5] 가 Message Routing, [15:8] 이 Message Code 다.
	 * R_LOCAL(4)은 "Terminate at Receiver" — INTx 메시지는 바로 위 포트에서
	 * 소비되고 더 전달되지 않는다(PCIe r6.0 sec 2.2.8.1).
	 * 코드가 비트 15까지 쓰고 CDNS_PCIE_MSG_NO_DATA 가 비트 16이므로,
	 * 이 창이 17비트(128KB)여야 하는 이유가 여기서 드러난다 —
	 * cdns_pcie_ep_setup() 이 SZ_128K 를 잡는 것과 정확히 맞는다. */
	offset = CDNS_PCIE_NORMAL_MSG_ROUTING(PCIE_MSG_TYPE_R_LOCAL) |
		 CDNS_PCIE_NORMAL_MSG_CODE(msg_code);
	/* [한국어] 창 안의 그 오프셋에 아무 값이나 쓰면 컨트롤러가 해당 메시지 TLP 를
	 * 만들어 보낸다. 데이터는 쓰이지 않으므로 0 을 쓴다 —
	 * 메시지를 실제로 발생시키는 것은 쓰기 동작 자체와 그 주소다. */
	writel(0, ep->irq_cpu_addr + offset);
}

/* [한국어]
 * cdns_pcie_ep_send_intx_irq - 레거시 INTx 인터럽트 한 번을 완성한다
 *
 * @ep: 이 드라이버의 엔드포인트 상태.
 * @fn: 물리 함수 번호.
 * @vfn: 가상 함수 번호. 받기는 하지만 이 함수 안에서 쓰이지 않는다 —
 *       호출자 cdns_pcie_ep_raise_irq() 가 이미 VF 를 걸러 냈기 때문이다.
 * @intx: 어느 선인지. 호출자는 항상 0(INTA)을 넘긴다.
 * @return: 0 이면 보냈다. 호스트가 INTx 를 꺼 두었으면 -EINVAL.
 *
 * INTx 는 레벨 트리거다. 선을 올렸다가 내려야 한 번의 인터럽트가 되므로,
 * assert 와 deassert 를 짝으로 보낸다. 그 사이의 1ms 는 상류 주석대로
 * dra7xx 드라이버에서 가져온 값이며, 호스트가 어서트 상태를 인지할 시간을
 * 준다.
 *
 * 먼저 Command 레지스터의 INTx Disable 비트를 확인한다. 이 비트는 호스트가
 * 세우는 것으로, 세워져 있으면 이 함수는 INTx 메시지를 보내면 안 된다는
 * 뜻이다(PCI 규격). 보통 호스트가 MSI 나 MSI-X 를 켜면서 함께 세운다.
 *
 * 실행 컨텍스트: EPC 코어의 epc->lock 아래에서 불린다. mdelay(1) 은 잠들지
 *   않고 바쁘게 기다리므로 그 1ms 동안 CPU 를 붙잡는다. 인터럽트를 자주
 *   거는 용도로는 부적합하고, 그래서 MSI/MSI-X 가 있으면 그쪽을 쓴다.
 *
 * 에러 경로: INTx Disable 검사 하나뿐이다.
 *
 * 호출 체인:
 *   EPF 드라이버 -> pci_epc_raise_irq() -> cdns_pcie_ep_raise_irq()
 *     -> [이 함수] -> cdns_pcie_ep_assert_intx() 두 번
 */
static int cdns_pcie_ep_send_intx_irq(struct cdns_pcie_ep *ep, u8 fn, u8 vfn,
				      u8 intx)
{
	/* [한국어] config space 의 Command 레지스터 값. */
	u16 cmd;

	/* [한국어] Command 레지스터를 읽는다. 호스트가 이 함수의 인터럽트 동작을
	 * 어떻게 설정해 두었는지 확인해야 한다. */
	cmd = cdns_pcie_ep_fn_readw(&ep->pcie, fn, PCI_COMMAND);
	/* [한국어] INTx Disable(비트 10)은 호스트가 세우는 비트다.
	 * 세워져 있으면 이 함수는 INTx 메시지를 보내면 안 된다(PCI 규격).
	 * 보통 호스트가 MSI/MSI-X 를 켜면서 함께 세운다. */
	if (cmd & PCI_COMMAND_INTX_DISABLE)
		/* [한국어] 규격상 금지된 동작이므로 거절한다. */
		return -EINVAL;

	/* [한국어] INTx 선을 어서트한다 — Assert_INTx 메시지를 보낸다. */
	cdns_pcie_ep_assert_intx(ep, fn, intx, true);
	/*
	 * The mdelay() value was taken from dra7xx_pcie_raise_intx_irq()
	 */
	/* [한국어] 상류 주석대로 dra7xx 드라이버에서 가져온 값이다.
	 * INTx 는 레벨 트리거라 어서트 상태가 호스트에 인지될 만큼 유지되어야 한다.
	 * mdelay 는 바쁘게 기다리는 함수라 이 함수가 잠들 수 없는 문맥에서도
	 * 동작하지만, 그만큼 1ms 동안 CPU 를 붙잡는다. */
	mdelay(1);
	/* [한국어] 해제한다 — Deassert_INTx 메시지를 보낸다. 이 쌍이 곧 한 번의
	 * 레거시 인터럽트 펄스가 된다. */
	cdns_pcie_ep_assert_intx(ep, fn, intx, false);
	/* [한국어] 여기까지 오면 성공. */
	return 0;
}

/* [한국어]
 * cdns_pcie_ep_send_msi_irq - MSI 인터럽트 한 발을 호스트에 쏜다
 *
 * @ep: 이 드라이버의 엔드포인트 상태.
 * @fn: 물리 함수 번호.
 * @vfn: 가상 함수 번호.
 * @interrupt_num: 벡터 번호. 1부터 센다(EPC 규약의 pci_epc_raise_irq 문서).
 * @return: 0 이면 보냈다. 호스트가 MSI 를 켜지 않았거나 번호가 범위를 벗어나면
 *          -EINVAL.
 *
 * MSI 의 실체는 "호스트가 알려 준 주소에 호스트가 알려 준 데이터를 쓰는
 * 메모리 쓰기 TLP" 다. 별도의 인터럽트 신호가 없다. 그래서 이 함수는
 * 결국 아웃바운드 창 하나를 그 주소로 조준하고 writel 한 번을 하는 것이 전부다.
 *
 * 동작 단계:
 *   1. 호스트가 MSI 를 켰는지, 요청한 벡터 번호가 허락된 범위인지 확인한다.
 *   2. 데이터 워드를 만든다. MSI 는 벡터마다 데이터가 따로 있는 것이 아니라,
 *      기준 데이터의 하위 비트에 벡터 번호를 실어 구분한다. 그 하위 비트 수는
 *      허락된 개수에서 나온다.
 *   3. 목적지 주소를 capability 에서 읽는다. 상위/하위 32비트가 따로 있다.
 *   4. 아웃바운드 0번 창을 그 주소로 조준한다. 이미 같은 주소·같은 함수로
 *      맞춰져 있으면 건너뛴다 — 인터럽트는 자주 일어나므로 이 최적화가 크다.
 *   5. 창 안의 오프셋에 데이터를 쓴다. 이것이 전송이다.
 *
 * 창 크기를 256바이트로 잡는 이유는 이 IP 가 그보다 작은 아웃바운드 창을
 * 표현할 수 없기 때문이다(pcie-cadence.c 의 nbits 최소 8 참고). 그래서
 * 목적지 주소를 256바이트 경계로 내려 창을 열고, 나머지 하위 8비트를
 * 창 안의 오프셋으로 쓴다.
 *
 * 실행 컨텍스트: EPC 코어의 epc->lock 아래. 아웃바운드 0번 창을 공유하므로
 *   INTx / MSI-X 경로와 동시에 실행되면 서로의 조준을 덮어쓸 수 있는데,
 *   그 직렬화도 같은 뮤텍스가 맡는다.
 *
 * 에러 경로: 두 검사 모두 레지스터를 건드리기 전에 이뤄진다.
 *
 * 호출 체인:
 *   EPF 드라이버 -> pci_epc_raise_irq() -> cdns_pcie_ep_raise_irq()
 *     -> [이 함수] -> cdns_pcie_set_outbound_region() -> writel()
 */
static int cdns_pcie_ep_send_msi_irq(struct cdns_pcie_ep *ep, u8 fn, u8 vfn,
				     u8 interrupt_num)
{
	/* [한국어] 레지스터 접근용 공통 구조체. */
	struct cdns_pcie *pcie = &ep->pcie;
	/* [한국어] flags 는 Message Control, mme 는 허락된 개수의 지수,
	 * data 는 보낼 데이터 워드, data_mask 는 그 안에서 벡터 번호가 차지하는 비트. */
	u16 flags, mme, data, data_mask;
	/* [한국어] pci_addr 은 호스트가 알려 준 MSI 목적지 주소.
	 * pci_addr_mask 0xff 는 아웃바운드 창의 최소 크기(256바이트)에 맞춘 값이다 —
	 * 이 IP 는 그보다 작은 창을 표현할 수 없다(pcie-cadence.c 참고). */
	u64 pci_addr, pci_addr_mask = 0xff;
	/* [한국어] msi_count 는 실제 허락된 인터럽트 개수, cap 은 MSI capability 오프셋. */
	u8 msi_count, cap;

	/* [한국어] MSI capability 를 찾는다. */
	cap = cdns_pcie_find_capability(pcie, PCI_CAP_ID_MSI);
	/* [한국어] VF 면 라우팅 번호로 바꾼다. */
	fn = cdns_pcie_get_fn_from_vfn(pcie, fn, vfn);

	/* Check whether the MSI feature has been enabled by the PCI host. */
	/* [한국어] Message Control 을 읽는다. */
	flags = cdns_pcie_ep_fn_readw(pcie, fn, cap + PCI_MSI_FLAGS);
	/* [한국어] MSI Enable 이 꺼져 있으면 호스트가 MSI 를 쓰지 않겠다는 뜻이다. */
	if (!(flags & PCI_MSI_FLAGS_ENABLE))
		/* [한국어] 이 상태에서 MSI 를 보내면 규격 위반이므로 거절한다. */
		return -EINVAL;

	/* Get the number of enabled MSIs */
	/* [한국어] 호스트가 허락한 개수의 지수를 꺼낸다. */
	mme = FIELD_GET(PCI_MSI_FLAGS_QSIZE, flags);
	/* [한국어] 지수를 개수로 되돌린다. */
	msi_count = 1 << mme;
	/* [한국어] interrupt_num 은 1부터 세는 번호다(EPC 규약의 pci_epc_raise_irq 문서).
	 * 0 이거나 허락된 개수를 넘으면 잘못된 요청이다. */
	if (!interrupt_num || interrupt_num > msi_count)
		/* [한국어] EPF 드라이버의 오류이므로 거절한다. */
		return -EINVAL;

	/* Compute the data value to be written. */
	/* [한국어] MSI 는 벡터 번호를 데이터 워드의 하위 비트에 실어 구분한다.
	 * msi_count 가 2의 거듭제곱이므로 -1 하면 그 하위 비트들의 마스크가 된다. */
	data_mask = msi_count - 1;
	/* [한국어] 호스트가 써 넣은 Message Data 를 읽는다. PCI_MSI_DATA_64 오프셋을 쓰는 것은
	 * set_msi 에서 64비트 주소 능력을 켰기 때문이다 — 64비트 MSI 는 데이터
	 * 레지스터의 위치가 다르다. */
	data = cdns_pcie_ep_fn_readw(pcie, fn, cap + PCI_MSI_DATA_64);
	/* [한국어] 하위 비트만 벡터 번호로 갈아 끼운다. interrupt_num 이 1부터이므로
	 * 1 을 빼서 0부터 세는 벡터 번호로 바꾼다. */
	data = (data & ~data_mask) | ((interrupt_num - 1) & data_mask);

	/* Get the PCI address where to write the data into. */
	/* [한국어] 목적지 주소 상위 32비트를 먼저 읽는다. */
	pci_addr = cdns_pcie_ep_fn_readl(pcie, fn, cap + PCI_MSI_ADDRESS_HI);
	/* [한국어] 상위로 밀어 올린다. */
	pci_addr <<= 32;
	/* [한국어] 하위 32비트를 합친다. */
	pci_addr |= cdns_pcie_ep_fn_readl(pcie, fn, cap + PCI_MSI_ADDRESS_LO);
	/* [한국어] 하위 2비트를 지운다. MSI 목적지 주소는 DWORD 정렬이라
	 * 그 두 비트는 규격상 0 이다. */
	pci_addr &= GENMASK_ULL(63, 2);

	/* Set the outbound region if needed. */
	/* [한국어] 0번 창이 이미 이 목적지·이 함수로 설정되어 있으면 다시 하지 않는다.
	 * 인터럽트 경로는 뜨거운 경로라 unlikely 로 표시했다 — 대개는 이미 맞다.
	 * 첫 호출에서는 ep->irq_pci_addr 이 센티널 값(0x1)이라 반드시 참이 된다. */
	if (unlikely(ep->irq_pci_addr != (pci_addr & ~pci_addr_mask) ||
		     ep->irq_pci_fn != fn)) {
		/* First region was reserved for IRQ writes. */
		/* [한국어] 0번 창을 MSI 목적지로 조준한다. 창의 시작을 256바이트 경계로 내리고
		 * (pci_addr & ~0xff) 크기를 256바이트로 잡는다.
		 * is_io 는 false — MSI 는 메모리 쓰기 TLP 다. */
		cdns_pcie_set_outbound_region(pcie, 0, fn, 0,
					      false,
					      ep->irq_phys_addr,
					      pci_addr & ~pci_addr_mask,
					      pci_addr_mask + 1);
		/* [한국어] 다음 호출에서 재설정을 건너뛸 수 있도록 지금 설정을 기억한다. */
		ep->irq_pci_addr = (pci_addr & ~pci_addr_mask);
		/* [한국어] 어느 함수용인지도 기록한다. */
		ep->irq_pci_fn = fn;
	}
	/* [한국어] 창 안의 오프셋(주소 하위 8비트)에 데이터를 쓴다.
	 * 이 쓰기가 그대로 호스트 메모리로 가는 메모리 쓰기 TLP 가 되고,
	 * 호스트의 인터럽트 컨트롤러가 그 주소·데이터 조합을 인터럽트로 해석한다.
	 * 이것이 MSI 의 전부다 — 별도의 인터럽트 선이 없다. */
	writel(data, ep->irq_cpu_addr + (pci_addr & pci_addr_mask));

	/* [한국어] 여기까지 오면 성공. */
	return 0;
}

/* [한국어]
 * cdns_pcie_ep_map_msi_irq - MSI 주소를 미리 창에 매핑하고 데이터를 돌려준다
 *                            (epc_ops.map_msi_irq)
 *
 * @epc: EPC 객체.
 * @fn: 물리 함수 번호.
 * @vfn: 가상 함수 번호.
 * @addr: 창들을 열 로컬 쪽 시작 물리 주소. 아웃바운드 영역 안의 주소여야 한다.
 * @interrupt_num: 몇 개의 벡터에 대해 창을 열지. 1부터 센다.
 * @entry_size: 벡터 하나가 차지할 창 크기.
 * @msi_data: [출력] 호출자가 써야 할 데이터 워드의 기준값.
 * @msi_addr_offset: [출력] 창 시작으로부터 실제 MSI 주소까지의 오프셋.
 * @return: 0 이면 성공. 호스트가 MSI 를 켜지 않았거나 번호가 범위를 벗어나거나
 *          창이 모자라면 음수.
 *
 * cdns_pcie_ep_send_msi_irq() 와 목적이 다르다. 그쪽은 지금 인터럽트를 쏘는
 * 함수이고, 이쪽은 "나중에 누군가가 직접 쏠 수 있도록 길을 미리 깔아 두는"
 * 함수다.
 *
 * 왜 그런 것이 필요한가: 상류 kernel-doc(pci_epc_map_msi_irq)이 NTB 의
 * 도어벨을 이유로 든다. 두 시스템을 PCIe 로 이어 놓으면 한쪽이 상대의
 * 아웃바운드 영역 물리 주소에 직접 써서 인터럽트를 걸 수 있는데, 그러려면
 * 그 주소가 미리 상대의 MSI 목적지로 매핑되어 있어야 한다. 매번 창을
 * 조준할 여유가 없는 것이다.
 *
 * 그래서 벡터 개수만큼 창을 연달아 열고, 목적지는 모두 같은 MSI 주소로 두되
 * 로컬 주소만 entry_size 씩 벌린다. 데이터 워드는 벡터 번호 부분을 0 으로
 * 비워 돌려주므로, 호출자가 거기에 (벡터 번호 - 1) 을 얹어 쓰면 된다.
 *
 * 실행 컨텍스트: EPC 코어가 epc->lock 을 쥔 프로세스 컨텍스트.
 *   안에서 cdns_pcie_ep_map_addr() 를 직접 부르는데, 그것 역시 같은 락 아래다.
 *
 * 에러 경로: 중간에 창 잡기가 실패해도 이미 연 창들을 되돌리지 않는다.
 *   호출자가 정리를 책임지는 구조인지는 이 트리에서 확인할 수 없다.
 *
 * 호출 체인:
 *   EPF 드라이버 -> pci_epc_map_msi_irq() [pci-epc-core.c] -> [이 함수]
 *     -> cdns_pcie_ep_map_addr() [이 파일] -> cdns_pcie_set_outbound_region()
 */
static int cdns_pcie_ep_map_msi_irq(struct pci_epc *epc, u8 fn, u8 vfn,
				    phys_addr_t addr, u8 interrupt_num,
				    u32 entry_size, u32 *msi_data,
				    u32 *msi_addr_offset)
{
	/* [한국어] EPC 코어가 저장해 둔 드라이버 private 포인터. */
	struct cdns_pcie_ep *ep = epc_get_drvdata(epc);
	/* [한국어] 레지스터 접근용 공통 구조체. */
	struct cdns_pcie *pcie = &ep->pcie;
	/* [한국어] 호스트가 알려 준 MSI 목적지 주소와 창 크기 마스크. */
	u64 pci_addr, pci_addr_mask = 0xff;
	/* [한국어] Message Control 과 데이터 워드 계산용 변수들. */
	u16 flags, mme, data, data_mask;
	/* [한국어] 허락된 인터럽트 개수와 MSI capability 오프셋. */
	u8 msi_count, cap;
	/* [한국어] cdns_pcie_ep_map_addr 의 반환값. */
	int ret;
	/* [한국어] 창을 여러 개 여는 반복 변수. */
	int i;

	/* [한국어] MSI capability 를 찾는다. */
	cap = cdns_pcie_find_capability(pcie, PCI_CAP_ID_MSI);
	/* [한국어] VF 면 라우팅 번호로 바꾼다. */
	fn = cdns_pcie_get_fn_from_vfn(pcie, fn, vfn);

	/* Check whether the MSI feature has been enabled by the PCI host. */
	/* [한국어] Message Control 을 읽는다. */
	flags = cdns_pcie_ep_fn_readw(pcie, fn, cap + PCI_MSI_FLAGS);
	/* [한국어] 호스트가 MSI 를 켜지 않았으면 목적지 주소가 유효하지 않다. */
	if (!(flags & PCI_MSI_FLAGS_ENABLE))
		/* [한국어] EPF 드라이버에게 아직 준비되지 않았음을 알린다. */
		return -EINVAL;

	/* Get the number of enabled MSIs */
	/* [한국어] 허락된 개수의 지수. */
	mme = FIELD_GET(PCI_MSI_FLAGS_QSIZE, flags);
	/* [한국어] 개수로 되돌린다. */
	msi_count = 1 << mme;
	/* [한국어] 1부터 세는 번호이므로 0 은 잘못이고, 허락된 개수도 넘을 수 없다. */
	if (!interrupt_num || interrupt_num > msi_count)
		/* [한국어] 잘못된 요청을 거절한다. */
		return -EINVAL;

	/* Compute the data value to be written. */
	/* [한국어] 벡터 번호가 들어갈 하위 비트의 마스크. */
	data_mask = msi_count - 1;
	/* [한국어] 호스트가 써 넣은 Message Data 를 읽는다. */
	data = cdns_pcie_ep_fn_readw(pcie, fn, cap + PCI_MSI_DATA_64);
	/* [한국어] send_msi_irq 와 달리 벡터 번호를 넣지 않고 지우기만 한다.
	 * 여기서는 특정 벡터를 지금 쏘는 것이 아니라, 호출자가 나중에 스스로
	 * 번호를 더해 쓸 수 있도록 기준값을 돌려주기 때문이다. */
	data = data & ~data_mask;

	/* Get the PCI address where to write the data into. */
	/* [한국어] 목적지 주소 상위 32비트. */
	pci_addr = cdns_pcie_ep_fn_readl(pcie, fn, cap + PCI_MSI_ADDRESS_HI);
	/* [한국어] 상위로 민다. */
	pci_addr <<= 32;
	/* [한국어] 하위 32비트를 합친다. */
	pci_addr |= cdns_pcie_ep_fn_readl(pcie, fn, cap + PCI_MSI_ADDRESS_LO);
	/* [한국어] DWORD 정렬이라 하위 2비트는 0 이다. */
	pci_addr &= GENMASK_ULL(63, 2);

	/* [한국어] interrupt_num 개만큼 아웃바운드 창을 연달아 연다.
	 * send_msi_irq 가 창 하나를 그때그때 조준해 쓰는 것과 대조적으로,
	 * 여기서는 미리 여러 창을 열어 두고 호출자가 직접 쓰게 한다.
	 * 상류 kernel-doc(pci_epc_map_msi_irq)이 그 용도를 밝히고 있다 — NTB 의
	 * 도어벨이다. 양쪽 EP 가 상대의 아웃바운드 영역 물리 주소에 직접 써서
	 * 인터럽트를 거는 구조라, 매번 창을 조준할 여유가 없다. */
	for (i = 0; i < interrupt_num; i++) {
		/* [한국어] 창 하나를 연다. 목적지는 모두 같은 MSI 주소(256바이트 경계로 내린 값)이고,
		 * CPU 쪽 주소만 entry_size 씩 달라진다. */
		ret = cdns_pcie_ep_map_addr(epc, fn, vfn, addr,
					    pci_addr & ~pci_addr_mask,
					    entry_size);
		/* [한국어] 중간에 실패하면. */
		if (ret)
			/* [한국어] 이미 연 창들을 되돌리지 않고 그대로 돌아간다.
			 * 호출자가 정리를 책임지는 구조인지는 이 트리에서 확인할 수 없다. */
			return ret;
		/* [한국어] 다음 창의 CPU 쪽 주소로 옮긴다. */
		addr = addr + entry_size;
	}

	/* [한국어] 호출자가 쓸 데이터 기준값. 여기에 (벡터 번호 - 1) 을 OR 하면
	 * send_msi_irq 가 만드는 값과 같아진다. */
	*msi_data = data;
	/* [한국어] 창 시작으로부터 실제 MSI 주소까지의 오프셋. 창을 256바이트 경계로
	 * 내려 잡았으므로 그 안에서의 위치를 알려 줘야 한다. */
	*msi_addr_offset = pci_addr & pci_addr_mask;

	/* [한국어] 여기까지 오면 성공. 출력 인자 둘이 채워져 있다. */
	return 0;
}

/* [한국어]
 * cdns_pcie_ep_send_msix_irq - MSI-X 인터럽트 한 발을 호스트에 쏜다
 *
 * @ep: 이 드라이버의 엔드포인트 상태.
 * @fn: 물리 함수 번호.
 * @vfn: 가상 함수 번호.
 * @interrupt_num: 벡터 번호. 1부터 센다.
 * @return: 0 이면 보냈다. 호스트가 MSI-X 를 켜지 않았으면 -EINVAL.
 *
 * MSI 와 최종 동작은 같다 — 호스트 메모리에 값을 하나 쓴다. 다른 것은 그
 * 주소와 데이터를 어디서 얻느냐다.
 *
 * MSI 는 config space 의 capability 에서 읽지만, MSI-X 는 BAR 뒤의 로컬
 * 메모리에 놓인 테이블에서 읽는다. 그 테이블은 호스트가 BAR 를 통해 써
 * 넣은 것이다. 즉 이 함수는 "호스트가 내 메모리에 남겨 둔 지시를 읽어
 * 그대로 실행하는" 셈이다. 테이블 항목 하나가
 * {주소 64비트, 데이터 32비트, 벡터 제어 32비트} 16바이트다.
 *
 * 테이블 위치를 찾는 경로가 이 함수의 핵심이다.
 *   Table Offset/BIR 레지스터 -> BIR(몇 번 BAR)과 오프셋
 *   -> epf->epf_bar[bir]->addr (cdns_pcie_ep_set_bar 이 장부에 남긴
 *      그 BAR 뒤 로컬 버퍼의 CPU 쪽 가상 주소)
 *   -> 거기에 오프셋을 더하면 테이블
 * 그래서 이 함수가 동작하려면 set_bar 가 먼저 그 BAR 를 열어 두었어야 한다.
 *
 * 주의: 항목의 Vector Control 에 있는 마스크 비트를 검사하지 않는다.
 * 규격상 마스크된 벡터는 보류시켜야 하지만 이 구현은 그렇게 하지 않으며,
 * 그 이유는 이 트리에서 확인할 수 없다.
 *
 * 실행 컨텍스트: EPC 코어의 epc->lock 아래. 아웃바운드 0번 창을 INTx / MSI
 *   경로와 공유한다.
 *
 * 에러 경로: MSI-X Enable 검사 하나뿐이다.
 *
 * 호출 체인:
 *   EPF 드라이버 -> pci_epc_raise_irq() -> cdns_pcie_ep_raise_irq()
 *     -> [이 함수] -> cdns_pcie_set_outbound_region() -> writel()
 */
static int cdns_pcie_ep_send_msix_irq(struct cdns_pcie_ep *ep, u8 fn, u8 vfn,
				      u16 interrupt_num)
{
	/* [한국어] tbl_offset 은 MSI-X 테이블의 BAR 안 오프셋, msg_data 는 보낼 데이터,
	 * reg 는 레지스터 오프셋 계산용이다. */
	u32 tbl_offset, msg_data, reg;
	/* [한국어] 레지스터 접근용 공통 구조체. */
	struct cdns_pcie *pcie = &ep->pcie;
	/* [한국어] MSI-X 테이블 항목을 가리킬 포인터. 항목 하나가
	 * {주소 64비트, 데이터 32비트, 벡터 제어 32비트} 16바이트다. */
	struct pci_epf_msix_tbl *msix_tbl;
	/* [한국어] 이 함수의 BAR 장부. 테이블이 놓인 BAR 의 CPU 쪽 주소를 여기서 얻는다. */
	struct cdns_pcie_epf *epf;
	/* [한국어] 아웃바운드 창 크기 마스크. MSI 쪽과 같은 256바이트다. */
	u64 pci_addr_mask = 0xff;
	/* [한국어] 테이블에서 읽어 낼 목적지 주소. */
	u64 msg_addr;
	/* [한국어] bir 은 테이블이 든 BAR 번호, cap 은 MSI-X capability 오프셋. */
	u8 bir, cap;
	/* [한국어] Message Control 값. */
	u16 flags;

	/* [한국어] MSI-X capability 를 찾는다. */
	cap = cdns_pcie_find_capability(pcie, PCI_CAP_ID_MSIX);
	/* [한국어] 이 물리 함수의 BAR 장부. */
	epf = &ep->epf[fn];
	/* [한국어] VF 면 장부를 VF 전용 배열로 좁힌다. */
	if (vfn > 0)
		/* [한국어] vfn 은 1부터이므로 -1 해서 배열 인덱스로 쓴다. */
		epf = &epf->epf[vfn - 1];

	/* [한국어] 여기서부터 fn 은 SR-IOV 라우팅 번호다. 위의 epf 장부 선택을
	 * fn 을 덮어쓰기 전에 끝낸 순서가 중요하다 — 장부는 원래 함수 번호로
	 * 색인하고 레지스터는 라우팅 번호로 색인하기 때문이다. */
	fn = cdns_pcie_get_fn_from_vfn(pcie, fn, vfn);

	/* Check whether the MSI-X feature has been enabled by the PCI host. */
	/* [한국어] MSI-X Message Control 을 읽는다. */
	flags = cdns_pcie_ep_fn_readw(pcie, fn, cap + PCI_MSIX_FLAGS);
	/* [한국어] MSI-X Enable 이 꺼져 있으면 호스트가 쓰지 않겠다는 뜻이다. */
	if (!(flags & PCI_MSIX_FLAGS_ENABLE))
		/* [한국어] 규격 위반이 되므로 거절한다. */
		return -EINVAL;

	/* [한국어] Table Offset/BIR 레지스터. set_msix 가 써 넣은 값을 되읽는다. */
	reg = cap + PCI_MSIX_TABLE;
	/* [한국어] 테이블 위치를 읽는다. */
	tbl_offset = cdns_pcie_ep_fn_readl(pcie, fn, reg);
	/* [한국어] 하위 3비트가 BIR — 테이블이 몇 번 BAR 안에 있는지. */
	bir = FIELD_GET(PCI_MSIX_TABLE_BIR, tbl_offset);
	/* [한국어] 상위 비트가 BAR 안에서의 오프셋. 하위 3비트를 지운다. */
	tbl_offset &= PCI_MSIX_TABLE_OFFSET;

	/* [한국어] 핵심 한 줄이다. epf_bar[bir]->addr 은 그 BAR 뒤에 붙어 있는 로컬 메모리의
	 * CPU 쪽 가상 주소이고(cdns_pcie_ep_set_bar 이 장부에 기록해 둔 것),
	 * 거기에 테이블 오프셋을 더하면 MSI-X 테이블이 나온다.
	 * 그 테이블의 내용은 호스트가 BAR 를 통해 써 넣은 것이다 —
	 * 즉 엔드포인트가 호스트가 남긴 값을 읽어 오는 셈이다. */
	msix_tbl = epf->epf_bar[bir]->addr + tbl_offset;
	/* [한국어] interrupt_num 은 1부터이므로 -1 해서 항목을 고른다.
	 * 목적지 주소는 호스트의 인터럽트 컨트롤러가 지정한 것이다. */
	msg_addr = msix_tbl[(interrupt_num - 1)].msg_addr;
	/* [한국어] 그 항목의 데이터 워드. MSI 와 달리 벡터마다 값이 따로 있으므로
	 * 계산할 것이 없다 — 그대로 쓰면 된다.
	 * (참고: Vector Control 의 마스크 비트는 검사하지 않는다.
	 * 규격상 마스크된 벡터는 보류시켜야 하지만 이 구현은 그렇게 하지 않으며,
	 * 그 이유는 이 트리에서 확인할 수 없다.) */
	msg_data = msix_tbl[(interrupt_num - 1)].msg_data;

	/* Set the outbound region if needed. */
	/* [한국어] 0번 창이 이미 이 목적지·이 함수로 맞춰져 있으면 재설정을 건너뛴다.
	 * MSI 경로와 달리 unlikely 를 쓰지 않았다 — MSI-X 는 벡터마다 목적지가
	 * 다를 수 있어 재설정이 흔하기 때문으로 읽히지만, 근거는 확인할 수 없다. */
	if (ep->irq_pci_addr != (msg_addr & ~pci_addr_mask) ||
	    ep->irq_pci_fn != fn) {
		/* First region was reserved for IRQ writes. */
		/* [한국어] 0번 창을 이 벡터의 목적지로 조준한다. 256바이트 경계로 내리고
		 * 크기도 256바이트로 잡는 것은 MSI 경로와 같다. */
		cdns_pcie_set_outbound_region(pcie, 0, fn, 0,
					      false,
					      ep->irq_phys_addr,
					      msg_addr & ~pci_addr_mask,
					      pci_addr_mask + 1);
		/* [한국어] 지금 설정을 기억해 다음 호출의 재설정을 줄인다. */
		ep->irq_pci_addr = (msg_addr & ~pci_addr_mask);
		/* [한국어] 어느 함수용인지도 기록한다. */
		ep->irq_pci_fn = fn;
	}
	/* [한국어] 창 안의 오프셋에 그 벡터의 데이터를 쓴다. 이 메모리 쓰기 TLP 가
	 * 호스트에 도착하면 해당 인터럽트가 걸린다. */
	writel(msg_data, ep->irq_cpu_addr + (msg_addr & pci_addr_mask));

	/* [한국어] 여기까지 오면 성공. */
	return 0;
}

/* [한국어]
 * cdns_pcie_ep_raise_irq - 호스트에 인터럽트를 건다 (epc_ops.raise_irq)
 *
 * @epc: EPC 객체.
 * @fn: 물리 함수 번호.
 * @vfn: 가상 함수 번호.
 * @type: PCI_IRQ_INTX / PCI_IRQ_MSI / PCI_IRQ_MSIX 중 하나.
 * @interrupt_num: 벡터 번호. 1부터 센다. INTx 에서는 쓰이지 않는다.
 * @return: 0 이면 보냈다. 지원하지 않는 방식이거나 하위 함수가 실패하면 음수.
 *
 * 세 인터럽트 방식으로 갈라 주는 얇은 분배기다. EPF 드라이버는 방식이
 * 무엇이든 pci_epc_raise_irq() 하나만 부르면 되고, 그 차이를 여기서 흡수한다.
 *
 * INTx 만 특별 취급을 받는다. SR-IOV 규격상 가상 함수는 INTx 를 지원하지
 * 않고 MSI/MSI-X 만 쓰므로, vfn 이 0 이 아니면 거절한다. 또 INTA 하나만
 * 지원해 마지막 인자를 상수 0 으로 넘긴다.
 *
 * 참고로 이 파일의 능력 표(cdns_pcie_epc_features)에는 intx_capable 이
 * 없어 false 다. 그 플래그를 보는 EPF 드라이버(pci-epf-test.c)는 이
 * 컨트롤러에서 INTx 를 고르지 않는다 — 즉 이 분기는 그 함수 드라이버
 * 경로로는 닿지 않는다.
 *
 * 실행 컨텍스트: EPC 코어가 epc->lock 을 쥔 프로세스 컨텍스트.
 *
 * 에러 경로: 알 수 없는 type 은 switch 를 빠져나와 -EINVAL 로 끝난다.
 *
 * 호출 체인:
 *   EPF 드라이버 -> pci_epc_raise_irq() [pci-epc-core.c] -> [이 함수]
 *     -> cdns_pcie_ep_send_intx_irq() / _send_msi_irq() / _send_msix_irq()
 */
static int cdns_pcie_ep_raise_irq(struct pci_epc *epc, u8 fn, u8 vfn,
				  unsigned int type, u16 interrupt_num)
{
	/* [한국어] EPC 코어가 저장해 둔 드라이버 private 포인터. */
	struct cdns_pcie_ep *ep = epc_get_drvdata(epc);
	/* [한국어] 레지스터 접근용 공통 구조체. */
	struct cdns_pcie *pcie = &ep->pcie;
	/* [한국어] 오류 메시지를 낼 device. */
	struct device *dev = pcie->dev;

	/* [한국어] EPC 규약이 정한 세 가지 인터럽트 방식 중 무엇인지로 갈라진다. */
	switch (type) {
	/* [한국어] 레거시 INTx — 메시지 TLP 로 흉내 낸다. */
	case PCI_IRQ_INTX:
		/* [한국어] 가상 함수는 INTx 를 쓸 수 없다. SR-IOV 규격상 VF 는 INTx 를 지원하지 않고
		 * MSI/MSI-X 만 쓴다. */
		if (vfn > 0) {
			/* [한국어] EPF 드라이버의 설정 오류이므로 원인을 밝힌다. */
			dev_err(dev, "Cannot raise INTX interrupts for VF\n");
			/* [한국어] 거절한다. */
			return -EINVAL;
		}
		/* [한국어] INTA 하나만 지원한다 — 마지막 인자가 상수 0(=INTA)이다.
		 * cdns_pcie_ep_send_intx_irq 는 vfn 을 받지만 쓰지 않는다. */
		return cdns_pcie_ep_send_intx_irq(ep, fn, vfn, 0);

	/* [한국어] MSI — 호스트가 지정한 주소에 데이터 워드 하나를 쓴다. */
	case PCI_IRQ_MSI:
		/* [한국어] interrupt_num 은 1부터 세는 벡터 번호다. */
		return cdns_pcie_ep_send_msi_irq(ep, fn, vfn, interrupt_num);

	/* [한국어] MSI-X — 벡터마다 주소와 데이터가 따로 있는 방식. */
	case PCI_IRQ_MSIX:
		/* [한국어] 역시 1부터 세는 벡터 번호. */
		return cdns_pcie_ep_send_msix_irq(ep, fn, vfn, interrupt_num);

	/* [한국어] 알 수 없는 방식. */
	default:
		break;
	}

	/* [한국어] 지원하지 않는 방식이면 오류. switch 를 빠져나온 경우도 여기로 온다. */
	return -EINVAL;
}

/* [한국어]
 * cdns_pcie_ep_start - 함수들을 활성화하고 링크를 올린다 (epc_ops.start)
 *
 * @epc: EPC 객체. function_num_map 비트맵에 활성화할 함수들이 담겨 있다.
 * @return: 0 이면 성공. SoC 의 start_link 콜백이 실패하면 그 오류 코드.
 *
 * 엔드포인트 초기화의 마지막 단계다. EPF 드라이버들이 configfs 로 바인딩을
 * 마치고 각자의 설정(헤더, BAR, MSI)을 끝낸 뒤에 불린다. 링크가 올라오면
 * 호스트가 곧바로 열거를 시작하므로, 그 전에 모든 준비가 끝나 있어야 한다.
 *
 * 하는 일이 셋이다.
 *   1. 물리 함수 활성화 — EPF 바인딩 결과인 function_num_map 을 그대로
 *      하드웨어에 쓴다. 비트 0 은 하드웨어에 고정되어 있어 함수 0 은 언제나
 *      켜져 있다(상류 주석).
 *   2. ARI 마무리 — 다중 함수 장치의 함수들은 ARI 규격에 따라 Next Function
 *      Number 로 이어지는데, 마지막 함수의 그 필드는 0 이어야 한다.
 *      그러지 않으면 호스트가 존재하지 않는 함수를 계속 찾는다.
 *   3. FLR quirk — 보드가 Function Level Reset 에 결함이 있으면 능력 비트를
 *      지워 호스트가 시도하지 않게 한다.
 *   그리고 마지막에 링크를 올린다.
 *
 * 호스트 쪽 cdns_pcie_host_link_setup() 과 대조적으로 링크가 올라오기를
 * 기다리지 않는다. 엔드포인트는 호스트가 링크를 걸어 주기를 기다리는 쪽이라
 * 여기서 기다릴 대상이 없다.
 *
 * 실행 컨텍스트: EPC 코어가 epc->lock 을 쥔 프로세스 컨텍스트.
 *
 * 에러 경로: 링크 시작 실패만 오류로 올라간다.
 *
 * 호출 체인:
 *   EPF 드라이버 -> pci_epc_start() [pci-epc-core.c] -> [이 함수]
 *     -> cdns_pcie_find_capability() / cdns_pcie_writel()
 *     -> cdns_pcie_start_link() [pcie-cadence.h -> SoC 별 ops]
 */
static int cdns_pcie_ep_start(struct pci_epc *epc)
{
	/* [한국어] EPC 코어가 저장해 둔 드라이버 private 포인터. */
	struct cdns_pcie_ep *ep = epc_get_drvdata(epc);
	/* [한국어] 레지스터 접근용 공통 구조체. */
	struct cdns_pcie *pcie = &ep->pcie;
	/* [한국어] 오류 메시지를 낼 device. */
	struct device *dev = pcie->dev;
	/* [한국어] 훑을 물리 함수 번호의 상한. function_num_map 이 unsigned long 이므로
	 * 이 식은 곧 BITS_PER_LONG 이다 — 비트맵 한 워드가 담을 수 있는 함수 수. */
	int max_epfs = sizeof(epc->function_num_map) * 8;
	/* [한국어] ret 는 링크 시작 결과, epf 는 함수 반복 변수,
	 * last_fn 은 활성화된 마지막 함수 번호. */
	int ret, epf, last_fn;
	/* [한국어] reg 는 레지스터 오프셋, value 는 그 값(읽고-고쳐-쓰기용). */
	u32 reg, value;
	/* [한국어] PCI Express capability 의 config space 오프셋. */
	u8 cap;

	/* [한국어] PCI Express capability 를 찾는다. 아래 FLR quirk 처리에서
	 * Device Capabilities 레지스터 위치를 계산하는 데 쓴다.
	 * quirk 가 없으면 쓰이지 않지만 미리 구해 둔다. */
	cap = cdns_pcie_find_capability(pcie, PCI_CAP_ID_EXP);
	/*
	 * BIT(0) is hardwired to 1, hence function 0 is always enabled
	 * and can't be disabled anyway.
	 */
	/* [한국어] EPF 드라이버들이 configfs 로 바인딩한 결과가 epc->function_num_map 에
	 * 비트맵으로 모여 있다. 그것을 그대로 하드웨어에 써서 해당 물리 함수들을
	 * 한꺼번에 활성화한다. 상류 주석대로 비트 0 은 하드웨어에 1 로 고정되어
	 * 있어 함수 0 은 언제나 켜져 있다.
	 * cdns_pcie_ep_setup() 이 BIT(0) 만 써 둔 것을 여기서 갈아 끼우는 셈이다. */
	cdns_pcie_writel(pcie, CDNS_PCIE_LM_EP_FUNC_CFG, epc->function_num_map);

	/*
	 * Next function field in ARI_CAP_AND_CTR register for last function
	 * should be 0.  Clear Next Function Number field for the last
	 * function used.
	 */
	/* [한국어] 활성화된 함수 중 번호가 가장 큰 것을 찾는다. */
	last_fn = find_last_bit(&epc->function_num_map, BITS_PER_LONG);
	/* [한국어] 그 함수의 ARI Capability and Control 레지스터 오프셋.
	 * 0x144 + fn * 0x1000 이라, 함수마다 4KB 씩 떨어진 자리에 놓인다.
	 * Local Management 블록과 달리 베이스 오프셋을 더하지 않는 생 주소다. */
	reg     = CDNS_PCIE_CORE_PF_I_ARI_CAP_AND_CTRL(last_fn);
	/* [한국어] 현재 값을 읽는다. 한 필드만 고칠 것이므로 나머지를 보존해야 한다. */
	value  = cdns_pcie_readl(pcie, reg);
	/* [한국어] Next Function Number 필드(비트 [15:8])를 0 으로 지운다.
	 * ARI(Alternative Routing-ID Interpretation)는 다중 함수 장치의 함수들을
	 * 연결 리스트처럼 잇는 구조이고, 마지막 함수는 다음이 없으므로 0 이어야
	 * 한다(상류 주석과 PCIe ARI 규격).
	 * 이 값이 잘못되면 호스트가 존재하지 않는 함수를 찾아 열거를 계속한다. */
	value &= ~CDNS_PCIE_ARI_CAP_NFN_MASK;
	/* [한국어] 고친 값을 되쓴다. */
	cdns_pcie_writel(pcie, reg, value);

	/* [한국어] 일부 보드는 FLR(Function Level Reset) 동작에 결함이 있어,
	 * 호스트가 FLR 을 걸면 복구되지 않는다. SoC 드라이버가 이 플래그를 세워 두면
	 * 능력 자체를 감춰 호스트가 시도하지 않게 한다. */
	if (ep->quirk_disable_flr) {
		/* [한국어] 활성화 여부와 무관하게 가능한 함수 번호를 모두 훑는다. */
		for (epf = 0; epf < max_epfs; epf++) {
			/* [한국어] 비트맵에 없는 함수는 존재하지 않으므로 건너뛴다.
			 * 그 함수의 config space 를 읽으면 의미 없는 값이 나온다. */
			if (!(epc->function_num_map & BIT(epf)))
				continue;

			/* [한국어] 그 함수의 Device Capabilities 레지스터를 읽는다.
			 * 함수마다 config space 가 따로이므로 함수 번호를 함께 넘긴다. */
			value = cdns_pcie_ep_fn_readl(pcie, epf,
						      cap + PCI_EXP_DEVCAP);
			/* [한국어] Function Level Reset Capable 비트(비트 28)를 지운다.
			 * 호스트는 이 비트가 0 이면 FLR 을 걸 수 없다고 판단한다. */
			value &= ~PCI_EXP_DEVCAP_FLR;
			/* [한국어] 고친 능력 값을 되쓴다. 보통 DEVCAP 은 읽기 전용이지만,
			 * 이 IP 는 엔드포인트 config space 를 레지스터로 노출해 쓰기를 허용한다. */
			cdns_pcie_ep_fn_writel(pcie, epf,
					       cap + PCI_EXP_DEVCAP, value);
		}
	}

	/* [한국어] 마지막으로 링크를 올린다. 모든 함수 설정이 끝난 뒤여야 하는데,
	 * 링크가 올라오면 호스트가 곧바로 열거를 시작하기 때문이다.
	 * SoC 별 ops->start_link 콜백이 없으면 0 을 돌려받는다. */
	ret = cdns_pcie_start_link(pcie);
	/* [한국어] 링크 시작 실패. */
	if (ret) {
		/* [한국어] 하드웨어를 켜지 못한 것이므로 오류로 남긴다.
		 * 호스트 쪽 cdns_pcie_host_link_setup() 과 달리 여기서는 링크가 올라오기를
		 * 기다리지 않는다 — 엔드포인트는 호스트가 링크를 걸어 주기를 기다리는
		 * 쪽이라 대기할 대상이 없다. */
		dev_err(dev, "Failed to start link\n");
		return ret;
	}

	/* [한국어] 여기까지 오면 성공. 이제 호스트가 이 장치를 열거할 수 있다. */
	return 0;
}

/* [한국어]
 * cdns_pcie_epc_vf_features - 가상 함수(VF)의 능력 표
 *
 * 역할: EPF 드라이버가 "이 컨트롤러의 VF 로 무엇을 할 수 있는가" 를 묻는
 *   창구다. cdns_pcie_ep_get_features() 가 vfunc_no 가 0 이 아닐 때 이것을
 *   돌려준다.
 * 설정자: 컴파일 시점 고정. 런타임에 바뀌지 않는다.
 * 읽는 자: pci_epc_get_features() 를 거쳐 EPF 드라이버.
 * 값 범위: 아래 세 필드만 채우고 나머지는 0/false 로 남는다.
 * 동기화: const 정적 변수라 락이 필요 없다.
 *
 * 물리 함수 표(cdns_pcie_epc_features)와 다른 점은 align 뿐이다 —
 * 256 이 아니라 65536 이다.
 */
static const struct pci_epc_features cdns_pcie_epc_vf_features = {
	/* [한국어] 역할: 이 컨트롤러의 가상 함수가 MSI 를 쓸 수 있음을 EPF 드라이버에게 알린다.
	 * 설정자: 컴파일 시점 고정.
	 * 읽는 자: pci_epc_get_features 를 거쳐 EPF 드라이버(예: pci-epf-test.c).
	 * 값 범위: true / false. 여기서는 true.
	 * 동기화: 읽기 전용 상수라 락이 필요 없다. */
	.msi_capable = true,
	/* [한국어] 역할: 가상 함수의 MSI-X 지원 여부.
	 * 설정자: 컴파일 시점 고정.
	 * 읽는 자: EPF 드라이버가 인터럽트 방식을 고를 때 본다.
	 * 값 범위: true / false. 여기서는 true.
	 * 동기화: 읽기 전용 상수. */
	.msix_capable = true,
	/* [한국어] 역할: BAR 뒤에 붙일 로컬 버퍼를 잡을 때 요구되는 정렬(64KB).
	 *   물리 함수의 256바이트보다 훨씬 큰데, SR-IOV 의 VF BAR 는 여러 VF 가
	 *   같은 aperture 를 나눠 쓰는 구조라 더 큰 단위로 잘라야 하기 때문으로
	 *   읽힌다. 다만 65536 이라는 값의 근거는 이 트리에서 확인하지 못했다.
	 * 설정자: 컴파일 시점 고정.
	 * 읽는 자: EPF 코어가 pci_epf_alloc_space 에서 버퍼 크기를 올림할 때.
	 * 값 범위: 2의 거듭제곱 바이트 수.
	 * 동기화: 읽기 전용 상수.
	 * 
	 * 참고: 이 표에는 intx_capable 이 없어 false 다. 그래서 이 컨트롤러의
	 * 가상 함수는 EPF 드라이버가 INTx 를 고르지 않는다
	 * (pci-epf-test.c 가 그 플래그를 보고 판단한다).
	 * linkup_notifier 와 dynamic_inbound_mapping 도 설정하지 않아 false 다. */
	.align = 65536,
};

/* [한국어]
 * cdns_pcie_epc_features - 물리 함수(PF)의 능력 표
 *
 * 역할: EPF 드라이버가 이 컨트롤러의 PF 능력을 확인하는 창구다.
 *   MSI 와 MSI-X 를 쓸 수 있고 BAR 버퍼는 256바이트 정렬이 필요하다는
 *   세 가지를 알린다.
 * 설정자: 컴파일 시점 고정.
 * 읽는 자: cdns_pcie_ep_get_features() -> pci_epc_get_features() -> EPF 드라이버.
 * 값 범위: 아래 세 필드만 채운다. bar[] 배열을 비워 두므로 BAR 별 하드웨어
 *   제약(고정 크기, 예약된 BAR 등)은 없다고 알리는 셈이다.
 * 동기화: const 정적 변수.
 *
 * 채우지 않아 false 로 남는 것 중 눈에 띄는 것들:
 *   intx_capable — 이 파일이 INTx 를 구현하는데도 false 다.
 *     그래서 pci-epf-test.c 는 이 컨트롤러에서 INTx 를 고르지 않는다.
 *   linkup_notifier — 링크업을 EPF 에 통지하지 못한다는 뜻이다. 실제로
 *     이 파일에 링크업 인터럽트를 다루는 코드가 없다.
 *   dynamic_inbound_mapping — set_bar 를 다시 부르기 전에 clear_bar 를
 *     먼저 불러야 한다는 뜻이다.
 */
static const struct pci_epc_features cdns_pcie_epc_features = {
	/* [한국어] 역할: 물리 함수의 MSI 지원 여부.
	 * 설정자: 컴파일 시점 고정.
	 * 읽는 자: EPF 드라이버가 인터럽트 방식을 고를 때.
	 * 값 범위: true / false. 여기서는 true.
	 * 동기화: 읽기 전용 상수. */
	.msi_capable = true,
	/* [한국어] 역할: 물리 함수의 MSI-X 지원 여부.
	 * 설정자: 컴파일 시점 고정.
	 * 읽는 자: EPF 드라이버.
	 * 값 범위: true / false. 여기서는 true.
	 * 동기화: 읽기 전용 상수. */
	.msix_capable = true,
	/* [한국어] 역할: 물리 함수 BAR 버퍼의 정렬 요구(256바이트).
	 *   이 IP 의 아웃바운드 창이 256바이트 경계를 요구하는 것과 같은 수이지만,
	 *   두 값이 같은 이유가 하드웨어 제약에서 오는지는 확인하지 못했다.
	 *   BAR 자체의 최소 크기는 CDNS_PCIE_EP_MIN_APERTURE(128)로 이것과 다르다.
	 * 설정자: 컴파일 시점 고정.
	 * 읽는 자: EPF 코어의 버퍼 할당 경로.
	 * 값 범위: 2의 거듭제곱 바이트 수.
	 * 동기화: 읽기 전용 상수.
	 * 
	 * 참고: 이 표에도 intx_capable 이 없다. cdns_pcie_ep_raise_irq 는
	 * PCI_IRQ_INTX 를 처리하지만, 그 플래그를 보는 EPF 드라이버
	 * (pci-epf-test.c)는 이 컨트롤러에서 INTx 를 고르지 않는다. */
	.align = 256,
};

/* [한국어]
 * cdns_pcie_ep_get_features - 이 컨트롤러의 능력 표를 돌려준다 (epc_ops.get_features)
 *
 * @epc: EPC 객체. 이 함수에서는 쓰이지 않는다 — 능력이 컨트롤러 인스턴스와
 *       무관하게 컴파일 시점에 정해져 있기 때문이다.
 * @func_no: 물리 함수 번호. 역시 쓰이지 않는다 — 모든 PF 가 같은 능력을 갖는다.
 * @vfunc_no: 가상 함수 번호. 이것만 본다.
 * @return: PF 면 cdns_pcie_epc_features, VF 면 cdns_pcie_epc_vf_features.
 *          NULL 을 돌려주는 경우는 없다.
 *
 * EPF 드라이버가 자기 함수를 구성하기 전에 "이 컨트롤러로 무엇을 할 수
 * 있는가" 를 묻는 자리다. 인터럽트 방식을 무엇으로 할지, BAR 버퍼를 몇
 * 바이트 정렬로 잡을지가 이 표에서 정해진다.
 *
 * PF 와 VF 를 가르는 이유는 정렬 요구가 다르기 때문이다 — PF 는 256바이트,
 * VF 는 65536바이트다.
 *
 * 실행 컨텍스트: EPC 코어가 epc->lock 을 쥔 프로세스 컨텍스트.
 *   읽기만 하므로 재진입해도 안전하다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   EPF 드라이버 -> pci_epc_get_features() [pci-epc-core.c] -> [이 함수]
 */
static const struct pci_epc_features*
cdns_pcie_ep_get_features(struct pci_epc *epc, u8 func_no, u8 vfunc_no)
{
	/* [한국어] vfunc_no 가 0 이면 물리 함수를 묻는 것이다. */
	if (!vfunc_no)
		/* [한국어] 물리 함수용 표 — 정렬 256바이트. */
		return &cdns_pcie_epc_features;

	/* [한국어] 가상 함수용 표 — 정렬 65536바이트. 이 함수는 실패할 수 없어
	 * 항상 유효한 포인터를 돌려준다. */
	return &cdns_pcie_epc_vf_features;
}

/* [한국어]
 * cdns_pcie_epc_ops - EPC 규약(struct pci_epc_ops) 구현 표
 *
 * 역할: 커널의 엔드포인트 프레임워크와 이 드라이버 사이의 유일한 접점이다.
 *   EPC 코어는 이 표를 통해서만 컨트롤러를 부리고, 그 반대편에 있는 EPF
 *   드라이버는 컨트롤러가 Cadence 인지 DesignWare 인지 알 필요가 없다.
 *   이 파일의 목차라고 봐도 좋다.
 * 설정자: 컴파일 시점 고정.
 * 읽는 자: cdns_pcie_ep_setup() 이 devm_pci_epc_create() 에 넘기고,
 *   그 뒤로는 pci-epc-core.c 의 래퍼 함수들이 이 콜백들을 부른다.
 * 값 범위: 아래 열세 개를 채우고 .stop 과 .align_addr 은 NULL 로 둔다.
 *   EPC 코어는 없는 콜백에 대해 아무 일도 하지 않거나 기본 동작을 쓴다.
 * 동기화: 모든 콜백은 EPC 코어가 epc->lock(뮤텍스)을 쥔 상태에서 불린다.
 *   그래서 이 파일의 콜백들은 서로 겹쳐 실행되지 않으며,
 *   아웃바운드 0번 창처럼 공유 자원을 락 없이 다룰 수 있다.
 */
static const struct pci_epc_ops cdns_pcie_epc_ops = {
	/* [한국어] 역할: 함수의 config space 헤더(vendor/device/class 등)를 채운다.
	 * 설정자: 컴파일 시점 고정.
	 * 읽는 자: pci_epc_write_header 가 epc->lock 을 쥐고 부른다.
	 * 값 범위: NULL 이 아닌 함수 포인터.
	 * 동기화: EPC 코어의 epc->lock 뮤텍스 아래에서 실행된다. */
	.write_header	= cdns_pcie_ep_write_header,
	/* [한국어] 역할: BAR 하나를 설정한다. EPF 드라이버가 잡은 로컬 버퍼를
	 *   호스트에게 BAR 로 노출하는 단계다.
	 * 설정자: 컴파일 시점 고정.
	 * 읽는 자: pci_epc_set_bar.
	 * 값 범위: NULL 이 아닌 함수 포인터.
	 * 동기화: epc->lock 아래. */
	.set_bar	= cdns_pcie_ep_set_bar,
	/* [한국어] 역할: BAR 를 끈다. set_bar 의 대칭.
	 * 설정자: 컴파일 시점 고정.
	 * 읽는 자: pci_epc_clear_bar.
	 * 값 범위: NULL 이 아닌 함수 포인터. 반환값이 없다(void).
	 * 동기화: epc->lock 아래. */
	.clear_bar	= cdns_pcie_ep_clear_bar,
	/* [한국어] 역할: 아웃바운드 창을 연다. 엔드포인트가 호스트 메모리를 읽고 쓰는 통로다.
	 * 설정자: 컴파일 시점 고정.
	 * 읽는 자: pci_epc_map_addr, 그리고 이 파일 안의 map_msi_irq.
	 * 값 범위: NULL 이 아닌 함수 포인터.
	 * 동기화: epc->lock 아래(내부 호출은 이미 락을 쥔 상태에서 이뤄진다). */
	.map_addr	= cdns_pcie_ep_map_addr,
	/* [한국어] 역할: 아웃바운드 창을 닫는다.
	 * 설정자: 컴파일 시점 고정.
	 * 읽는 자: pci_epc_unmap_addr.
	 * 값 범위: NULL 이 아닌 함수 포인터. void 반환.
	 * 동기화: epc->lock 아래. */
	.unmap_addr	= cdns_pcie_ep_unmap_addr,
	/* [한국어] 역할: 쓸 MSI 개수를 능력 레지스터에 광고한다.
	 * 설정자: 컴파일 시점 고정.
	 * 읽는 자: pci_epc_set_msi.
	 * 값 범위: NULL 이 아닌 함수 포인터.
	 * 동기화: epc->lock 아래. */
	.set_msi	= cdns_pcie_ep_set_msi,
	/* [한국어] 역할: 호스트가 실제로 허락한 MSI 개수를 읽는다.
	 * 설정자: 컴파일 시점 고정.
	 * 읽는 자: pci_epc_get_msi.
	 * 값 범위: NULL 이 아닌 함수 포인터.
	 * 동기화: epc->lock 아래. */
	.get_msi	= cdns_pcie_ep_get_msi,
	/* [한국어] 역할: MSI-X 테이블 크기와 테이블/PBA 위치를 설정한다.
	 * 설정자: 컴파일 시점 고정.
	 * 읽는 자: pci_epc_set_msix.
	 * 값 범위: NULL 이 아닌 함수 포인터.
	 * 동기화: epc->lock 아래. */
	.set_msix	= cdns_pcie_ep_set_msix,
	/* [한국어] 역할: 호스트가 허락한 MSI-X 개수를 읽는다.
	 * 설정자: 컴파일 시점 고정.
	 * 읽는 자: pci_epc_get_msix.
	 * 값 범위: NULL 이 아닌 함수 포인터.
	 * 동기화: epc->lock 아래. */
	.get_msix	= cdns_pcie_ep_get_msix,
	/* [한국어] 역할: 호스트에 인터럽트를 건다. INTx / MSI / MSI-X 를 모두 받는다.
	 * 설정자: 컴파일 시점 고정.
	 * 읽는 자: pci_epc_raise_irq.
	 * 값 범위: NULL 이 아닌 함수 포인터.
	 * 동기화: epc->lock 아래. 다만 INTx 경로는 ep->lock 스핀락을 따로 쥔다. */
	.raise_irq	= cdns_pcie_ep_raise_irq,
	/* [한국어] 역할: MSI 주소를 아웃바운드 창에 미리 매핑해 두고 데이터/오프셋을 돌려준다.
	 *   NTB 의 도어벨처럼 상대편이 직접 써서 인터럽트를 거는 구조에 쓴다.
	 * 설정자: 컴파일 시점 고정.
	 * 읽는 자: pci_epc_map_msi_irq.
	 * 값 범위: NULL 이 아닌 함수 포인터. 이것을 제공하지 않는 컨트롤러도 많다.
	 * 동기화: epc->lock 아래. */
	.map_msi_irq	= cdns_pcie_ep_map_msi_irq,
	/* [한국어] 역할: 링크를 올린다. 모든 함수 설정이 끝난 뒤 마지막에 불린다.
	 * 설정자: 컴파일 시점 고정.
	 * 읽는 자: pci_epc_start.
	 * 값 범위: NULL 이 아닌 함수 포인터.
	 * 동기화: epc->lock 아래. */
	.start		= cdns_pcie_ep_start,
	/* [한국어] 역할: 이 컨트롤러가 지원하는 기능 표를 돌려준다.
	 * 설정자: 컴파일 시점 고정.
	 * 읽는 자: pci_epc_get_features, 그리고 그것을 통해 EPF 드라이버.
	 * 값 범위: NULL 이 아닌 함수 포인터.
	 * 동기화: epc->lock 아래.
	 * 
	 * 참고: .stop 과 .align_addr 은 제공하지 않는다(NULL). EPC 코어는
	 * 두 콜백이 없으면 각각 아무 일도 하지 않거나 기본 동작을 쓴다. */
	.get_features	= cdns_pcie_ep_get_features,
};

/* [한국어]
 * cdns_pcie_ep_disable - 엔드포인트를 내린다 (remove 경로)
 *
 * @ep: 이 드라이버의 엔드포인트 상태.
 * @return: 없음.
 *
 * cdns_pcie_ep_setup() 이 한 일을 역순으로 되돌린다. 세 단계다.
 *   1. pci_epc_deinit_notify() — EPF 드라이버들에게 이 컨트롤러가 내려간다고
 *      알린다. 각 함수 드라이버가 자기 BAR 를 끄고 창을 닫을 기회를 갖는다.
 *      가장 먼저 해야 하는 이유가 여기 있다 — 아직 자원이 살아 있는 동안
 *      정리하게 해야 한다.
 *   2. pci_epc_mem_free_addr() — 인터럽트 전용으로 잡아 두었던 128KB 를
 *      돌려준다. 크기를 다시 넘기는 것은 이 할당자가 크기를 기억하지 않기
 *      때문이다.
 *   3. pci_epc_mem_exit() — 할당자 자체를 없앤다. 개별 할당을 모두 돌려준
 *      뒤여야 한다.
 *
 * EPC 객체 자체와 ob_addr / epf 배열은 devm_ 으로 잡았으므로 커널이
 * 알아서 푼다. 그래서 여기서 명시적으로 해제하지 않는다.
 *
 * 링크를 내리는 처리는 없다. 호스트 쪽 cdns_pcie_host_disable() 이
 * cdns_pcie_stop_link() 를 부르는 것과 대조적인데, 엔드포인트는 링크를
 * 거는 주체가 아니어서 그런 것으로 읽히지만 근거는 확인하지 못했다.
 *
 * 실행 컨텍스트: remove 경로의 프로세스 컨텍스트. 1단계가 다른 드라이버의
 *   정리 코드를 부르므로 잠들 수 있다.
 *
 * 호출 체인:
 *   j721e_pcie_remove() [pci-j721e.c] -> [이 함수]
 *     -> pci_epc_deinit_notify() / pci_epc_mem_free_addr() / pci_epc_mem_exit()
 */
void cdns_pcie_ep_disable(struct cdns_pcie_ep *ep)
{
	/* [한국어] SoC 드라이버가 채워 둔 device. 아래에서 EPC 객체를 되찾는 데 쓴다. */
	struct device *dev = ep->pcie.dev;
	/* [한국어] device 로부터 그것을 품은 struct pci_epc 를 얻는다.
	 * cdns_pcie_ep_setup() 의 devm_pci_epc_create(dev, ...) 가 만든 그 객체다. */
	struct pci_epc *epc = to_pci_epc(dev);

	/* [한국어] EPF 드라이버들에게 이 컨트롤러가 내려간다고 알린다.
	 * 각 함수 드라이버가 자기 자원을 정리할 기회를 갖는다 —
	 * BAR 를 끄고 창을 닫는 일이 이 통지 안에서 일어난다. */
	pci_epc_deinit_notify(epc);
	/* [한국어] 인터럽트 전용으로 잡아 두었던 128KB 주소 공간을 돌려준다.
	 * 크기를 다시 넘겨야 하는 것은 이 할당자가 크기를 기억하지 않기 때문이다. */
	pci_epc_mem_free_addr(epc, ep->irq_phys_addr, ep->irq_cpu_addr,
			      SZ_128K);
	/* [한국어] EPC 메모리 할당자를 해제한다. 순서가 중요하다 —
	 * 개별 할당을 모두 돌려준 뒤에 할당자 자체를 없앤다. */
	pci_epc_mem_exit(epc);
}
/* [한국어] SoC 별 드라이버의 remove 경로가 부르기 위해 내보낸다.
 * 실제 사용처: pci-j721e.c. */
EXPORT_SYMBOL_GPL(cdns_pcie_ep_disable);

/* [한국어]
 * cdns_pcie_ep_setup - SoC 드라이버가 부르는 엔드포인트 초기화 진입점
 *
 * @ep: SoC 드라이버가 잡아 둔 구조체. 호출 전에 ep->pcie.dev 가 채워져 있어야
 *      하고, quirk 플래그와 ops 도 미리 설정해 둔다.
 * @return: 0 이면 EPC 등록까지 끝났다는 뜻. 실패하면 음수 오류.
 *
 * 이 파일의 시작점이자 SoC 드라이버와의 경계다. 호스트 쪽
 * cdns_pcie_host_setup() 에 대응하지만, 끝나는 지점이 다르다. 호스트는
 * pci_host_probe() 로 버스 열거까지 마치고 돌아오는 반면, 여기서는 EPC 객체를
 * 등록하고 EPF 드라이버들에게 통지하는 데서 끝난다. 실제로 무엇인 척할지는
 * 사용자가 configfs 로 정하고, 링크는 그 뒤 cdns_pcie_ep_start() 에서 올라간다.
 *
 * 동작 단계:
 *   1. 컨트롤러를 EP 모드로 표시한다 — 아웃바운드 창의 requester ID 처리가
 *      호스트 모드와 갈리는 근거다.
 *   2. "reg"(레지스터 블록)와 "mem"(호스트 접근용 로컬 주소 공간) 자원을 얻는다.
 *   3. 아웃바운드 창 개수를 정하고 창별 주소 장부를 잡는다.
 *   4. 함수 0 만 켜 둔다. 나머지는 EPF 바인딩 뒤 ep_start 에서 켜진다.
 *   5. EPC 객체를 만들고 ops 표를 붙인다. 이 순간 커널의 엔드포인트
 *      프레임워크에 등록되어 configfs 로 보이기 시작한다.
 *   6. 물리 함수 수와 함수별 VF 수를 디바이스 트리에서 읽고 장부를 잡는다.
 *   7. "mem" 자원을 EPC 메모리 할당자에 넘기고, 그중 128KB 를 인터럽트
 *      전용으로 떼어 아웃바운드 0번 창에 예약한다.
 *   8. Detect.Quiet quirk 를 보정하고 스핀락을 초기화한 뒤, EPF 드라이버들에게
 *      준비되었음을 알린다.
 *
 * 8번이 가장 마지막인 것이 중요하다. 그 통지를 받은 EPF 드라이버가 곧바로
 * write_header / set_bar 같은 콜백을 부르기 시작하므로, 그 전에 모든 자원이
 * 준비되어 있어야 한다.
 *
 * 실행 컨텍스트: probe 시점의 프로세스 컨텍스트.
 *
 * 에러 경로: pci_epc_mem_init() 이전의 실패는 그냥 돌아간다 — 그때까지 잡은
 *   것이 모두 devm_ 이라 커널이 푼다. 그 이후의 실패만 free_epc_mem 레이블로
 *   가서 할당자를 되돌린다. 호출자인 SoC 드라이버는 PHY 를 끄고 probe 를 접는다.
 *
 * 호출 체인:
 *   j721e_pcie_probe() / cdns_plat_pcie_probe() -> [이 함수]
 *     -> devm_pci_epc_create() / pci_epc_mem_init() / pci_epc_mem_alloc_addr()
 *     -> pci_epc_init_notify() -> (EPF 드라이버) -> 이 파일의 ops 콜백들
 */
int cdns_pcie_ep_setup(struct cdns_pcie_ep *ep)
{
	/* [한국어] 오류 메시지와 디바이스 트리 노드를 얻을 struct device. */
	struct device *dev = ep->pcie.dev;
	/* [한국어] 플랫폼 자원(reg/mem)을 꺼내기 위해 platform_device 로 되돌린다. */
	struct platform_device *pdev = to_platform_device(dev);
	/* [한국어] cdns,max-outbound-regions 등을 읽을 디바이스 트리 노드. */
	struct device_node *np = dev->of_node;
	/* [한국어] 레지스터 접근용 공통 구조체. */
	struct cdns_pcie *pcie = &ep->pcie;
	/* [한국어] 가상 함수 배열을 할당할 때 쓸 임시 포인터. */
	struct cdns_pcie_epf *epf;
	/* [한국어] "mem" 자원을 담을 임시 포인터. */
	struct resource *res;
	/* [한국어] 이 파일이 만들 EPC 객체. */
	struct pci_epc *epc;
	/* [한국어] 하위 단계들의 오류 코드. */
	int ret;
	/* [한국어] 물리 함수를 훑는 반복 변수. */
	int i;

	/* [한국어] 이 컨트롤러를 엔드포인트 모드로 표시한다.
	 * 이 플래그가 pcie-cadence.c 의 아웃바운드 창 설정에서 갈림길이 된다 —
	 * EP 면 HARDCODED_RID 를 세우지 않아 버스·장치 번호를 하드웨어가
	 * 호스트에게서 받아 기억해 둔 값으로 채우고, 펑션 번호만 소프트웨어가 넣는다. */
	pcie->is_rc = false;

	/* [한국어] "reg" 자원 — 이 IP 의 레지스터 블록. Local Management, 함수별 config
	 * space, Address Translation 이 하나의 창 안에 오프셋으로 나뉘어 들어 있다.
	 * devm_ 이므로 probe 실패나 remove 때 자동 해제된다. */
	pcie->reg_base = devm_platform_ioremap_resource_byname(pdev, "reg");
	/* [한국어] 레지스터 창이 없으면 아무것도 할 수 없다. */
	if (IS_ERR(pcie->reg_base)) {
		/* [한국어] 디바이스 트리에 reg 이름의 자원이 없다는 뜻이므로 원인을 밝힌다. */
		dev_err(dev, "missing \"reg\"\n");
		/* [한국어] IS_ERR 로 감싸인 오류 코드를 꺼내 돌려준다. */
		return PTR_ERR(pcie->reg_base);
	}

	/* [한국어] "mem" 자원 — 아웃바운드 창들이 대응할 로컬 주소 공간.
	 * 엔드포인트가 호스트 메모리를 읽고 쓸 때 CPU 가 접근하는 주소 범위이며,
	 * 아래 pci_epc_mem_init 이 이 범위를 페이지 단위 할당자로 관리한다. */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "mem");
	/* [한국어] 이 자원은 필수다. 없으면 호스트 메모리에 접근할 창을 만들 수 없다. */
	if (!res) {
		/* [한국어] 디바이스 트리 오류이므로 원인을 밝힌다. */
		dev_err(dev, "missing \"mem\"\n");
		/* [한국어] 잘못된 설정이므로 -EINVAL. */
		return -EINVAL;
	}
	/* [한국어] 나중에 pci_epc_mem_init 이 시작 주소와 크기를 꺼내 쓴다. */
	pcie->mem_res = res;

	/* [한국어] 하드웨어 기본값(32개). 디바이스 트리가 덮어쓸 수 있다. */
	ep->max_regions = CDNS_PCIE_MAX_OB;
	/* [한국어] 실제 창 개수가 IP 구성마다 다르므로 디바이스 트리에서 읽는다.
	 * 반환값을 검사하지 않는 것은 속성이 없으면 위 기본값이 그대로 남기 때문이다. */
	of_property_read_u32(np, "cdns,max-outbound-regions", &ep->max_regions);

	/* [한국어] 창마다 그 CPU 쪽 시작 주소를 기억할 배열.
	 * cdns_pcie_ep_unmap_addr 이 주소로 창을 되찾는 데 쓴다.
	 * devm_kcalloc 은 0 으로 초기화하며 remove 때 자동 해제된다. */
	ep->ob_addr = devm_kcalloc(dev,
				   ep->max_regions, sizeof(*ep->ob_addr),
				   GFP_KERNEL);
	/* [한국어] 메모리 부족. */
	if (!ep->ob_addr)
		/* [한국어] probe 를 접는다. 앞서 잡은 것은 모두 devm_ 이라 따로 풀 것이 없다. */
		return -ENOMEM;

	/* Disable all but function 0 (anyway BIT(0) is hardwired to 1). */
	/* [한국어] 상류 주석대로 함수 0 만 켠다. 비트 0 은 하드웨어에 1 로 고정되어 있어
	 * 어차피 끌 수 없다. 나머지 함수는 EPF 드라이버가 configfs 로 바인딩한 뒤
	 * cdns_pcie_ep_start() 에서 한꺼번에 켜진다. */
	cdns_pcie_writel(pcie, CDNS_PCIE_LM_EP_FUNC_CFG, BIT(0));

	/* [한국어] EPC 객체를 만들고 위에서 정의한 ops 표를 붙인다.
	 * 이 순간부터 이 컨트롤러가 커널의 엔드포인트 프레임워크에 등록되어
	 * configfs 를 통해 EPF 드라이버를 붙일 수 있게 된다. */
	epc = devm_pci_epc_create(dev, &cdns_pcie_epc_ops);
	/* [한국어] 생성 실패. */
	if (IS_ERR(epc)) {
		/* [한국어] 원인을 밝힌다. */
		dev_err(dev, "failed to create epc device\n");
		/* [한국어] 오류 코드를 꺼내 돌려준다. */
		return PTR_ERR(epc);
	}

	/* [한국어] EPC 객체에 이 드라이버의 상태를 매단다.
	 * 모든 ops 콜백이 첫 줄에서 epc_get_drvdata(epc) 로 이것을 되찾는다. */
	epc_set_drvdata(epc, ep);

	/* [한국어] 지원할 물리 함수 개수를 디바이스 트리에서 읽는다.
	 * 음수 반환은 속성이 없다는 뜻이다. */
	if (of_property_read_u8(np, "max-functions", &epc->max_functions) < 0)
		/* [한국어] 속성이 없으면 함수 하나짜리 장치로 본다. */
		epc->max_functions = 1;

	/* [한국어] 물리 함수마다 BAR 장부를 하나씩 둔다. */
	ep->epf = devm_kcalloc(dev, epc->max_functions, sizeof(*ep->epf),
			       GFP_KERNEL);
	if (!ep->epf)
		/* [한국어] 메모리 부족. */
		return -ENOMEM;
/* [한국어] probe 를 접는다. */

	/* [한국어] 물리 함수별 최대 VF 개수를 담을 배열. 이 배열의 소유자는 EPC 객체이지만
	 * 채우는 것은 이 드라이버다. */
	epc->max_vfs = devm_kcalloc(dev, epc->max_functions,
				    sizeof(*epc->max_vfs), GFP_KERNEL);
	if (!epc->max_vfs)
		/* [한국어] 메모리 부족. */
		return -ENOMEM;
/* [한국어] probe 를 접는다. */

	/* [한국어] 디바이스 트리에서 함수별 VF 개수를 한 번에 읽는다.
	 * 배열 속성이라 개수를 함께 넘긴다. */
	ret = of_property_read_u8_array(np, "max-virtual-functions",
					epc->max_vfs, epc->max_functions);
	/* [한국어] 속성이 있어 성공적으로 읽었을 때만 VF 장부를 만든다.
	 * 속성이 없으면 SR-IOV 를 쓰지 않는 구성이므로 아무것도 하지 않는다. */
	if (ret == 0) {
		/* [한국어] 물리 함수를 하나씩 본다. */
		for (i = 0; i < epc->max_functions; i++) {
			/* [한국어] 이 물리 함수의 장부. */
			epf = &ep->epf[i];
			/* [한국어] VF 를 두지 않는 함수는 건너뛴다. */
			if (epc->max_vfs[i] == 0)
				continue;
			/* [한국어] 이 물리 함수의 VF 장부 배열을 만든다.
			 * sizeof(*ep->epf) 를 쓰는 것은 원소 타입이 같은 struct cdns_pcie_epf 이기
			 * 때문이다. cdns_pcie_ep_set_bar 이 epf->epf[vfn - 1] 로 여기에 접근한다. */
			epf->epf = devm_kcalloc(dev, epc->max_vfs[i],
						sizeof(*ep->epf), GFP_KERNEL);
			/* [한국어] 메모리 부족. */
			if (!epf->epf)
				/* [한국어] probe 를 접는다. */
				return -ENOMEM;
		}
	}

	/* [한국어] "mem" 자원 전체를 EPC 메모리 할당자에 넘긴다.
	 * EPF 드라이버가 pci_epc_mem_alloc_addr 로 이 공간에서 창을 잡아 쓴다.
	 * PAGE_SIZE 를 할당 단위로 준다. */
	ret = pci_epc_mem_init(epc, pcie->mem_res->start,
			       resource_size(pcie->mem_res), PAGE_SIZE);
	/* [한국어] 할당자 초기화 실패. */
	if (ret < 0) {
		/* [한국어] 원인을 밝힌다. */
		dev_err(dev, "failed to initialize the memory space\n");
		/* [한국어] probe 를 접는다. 이 단계 이후로는 실패 시 되돌릴 것이 생긴다. */
		return ret;
	}

	/* [한국어] 인터럽트 전용 창의 CPU 쪽 주소를 128KB 잡는다.
	 * SZ_128K 는 2^17 이고, 이 IP 의 메시지 전용 아웃바운드 창이 17비트로
	 * 고정된 것과 정확히 맞는다 — INTx 메시지의 라우팅과 코드가 오프셋 비트
	 * [15:5] 에, MSG_NO_DATA 가 비트 16 에 놓이므로 그만큼이 필요하다.
	 * 물리 주소는 출력 인자로 받아 두었다가 아웃바운드 창의 CPU 주소로 쓴다. */
	ep->irq_cpu_addr = pci_epc_mem_alloc_addr(epc, &ep->irq_phys_addr,
						  SZ_128K);
	/* [한국어] 주소 공간이 모자란 경우. */
	if (!ep->irq_cpu_addr) {
		/* [한국어] 원인을 밝힌다. */
		dev_err(dev, "failed to reserve memory space for MSI\n");
		/* [한국어] 메모리 부족으로 보고한다. */
		ret = -ENOMEM;
		/* [한국어] 여기서부터는 pci_epc_mem_init 을 되돌려야 하므로 goto 로 정리 경로로 간다. */
		goto free_epc_mem;
	}
	/* [한국어] "아직 어떤 목적지로도 조준되지 않았음" 센티널.
	 * 첫 인터럽트에서 반드시 창 설정 분기를 타게 만든다. */
	ep->irq_pci_addr = CDNS_PCIE_EP_IRQ_PCI_ADDR_NONE;
	/* Reserve region 0 for IRQs */
	/* [한국어] 아웃바운드 0번 창을 인터럽트 전용으로 예약한다.
	 * cdns_pcie_ep_map_addr 의 find_first_zero_bit 이 이 비트를 건너뛰게 되고,
	 * assert_intx / send_msi_irq / send_msix_irq 는 모두 창 번호 0 을 상수로 쓴다. */
	set_bit(0, &ep->ob_region_map);

	/* [한국어] 일부 IP 판본은 LTSSM 의 Detect.Quiet 최소 지연이 짧아 링크가 불안정하다.
	 * 호스트 쪽 cdns_pcie_host_link_setup() 과 같은 quirk 다. */
	if (ep->quirk_detect_quiet_flag)
		/* [한국어] LTSSM Detect.Quiet 최소 지연을 보정한다(pcie-cadence.c). */
		cdns_pcie_detect_quiet_min_delay_set(&ep->pcie);

	/* [한국어] INTx 어서트 시 PCI_STATUS 를 읽고-고쳐-쓸 때 쓸 스핀락을 초기화한다.
	 * 실제로 쓰는 곳은 cdns_pcie_ep_assert_intx 한 군데뿐이다. */
	spin_lock_init(&ep->lock);

	/* [한국어] 엔드포인트 프레임워크에 초기화가 끝났음을 알린다.
	 * 이 통지를 받은 EPF 드라이버가 자기 함수를 구성하기 시작하고,
	 * 그 끝에서 pci_epc_start 를 거쳐 cdns_pcie_ep_start() 가 불린다.
	 * 이 호출이 마지막인 것은, 그 전에 모든 자원이 준비되어 있어야 하기 때문이다. */
	pci_epc_init_notify(epc);

	/* [한국어] 성공. 링크는 아직 올라가지 않았다 — 그것은 ep_start 의 몫이다. */
	return 0;

 /* [한국어] pci_epc_mem_alloc_addr 실패 전용 정리 경로. */
 free_epc_mem:
	/* [한국어] 할당자를 해제한다. 앞의 devm_ 자원들은 커널이 알아서 푼다. */
	pci_epc_mem_exit(epc);

	/* [한국어] 저장해 둔 오류 코드를 그대로 돌려준다. */
	return ret;
}
/* [한국어] SoC 별 드라이버의 probe 가 부르기 위해 내보낸다.
 * 실제 사용처: pci-j721e.c, pcie-cadence-plat.c. */
EXPORT_SYMBOL_GPL(cdns_pcie_ep_setup);

/* [한국어] GPL 모듈로 선언한다. EXPORT_SYMBOL_GPL 심볼을 쓰려면
 * 사용하는 쪽도 GPL 이어야 한다. */
MODULE_LICENSE("GPL");
/* [한국어] modinfo 에 보이는 모듈 설명. */
MODULE_DESCRIPTION("Cadence PCIe endpoint controller driver");
/* [한국어] 원저자 표기. */
MODULE_AUTHOR("Cyrille Pitchen <cyrille.pitchen@free-electrons.com>");
