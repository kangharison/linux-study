// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright Altera Corporation (C) 2013-2015. All rights reserved
 *
 * Author: Ley Foon Tan <lftan@altera.com>
 * Description: Altera PCIe host controller driver
 */

/*
 * [한국어 설명] TLP 를 손으로 조립해 config 접근을 흉내 내는 호스트 컨트롤러 (pcie-altera.c)
 *
 * === 파일의 역할 ===
 * Altera(현 Intel) FPGA 에 얹힌 PCIe Root Port 하드웨어의 호스트 컨트롤러
 * 드라이버다. 커널의 PCI 코어가 요구하는 것은 struct pci_ops 의 read/write
 * 두 콜백뿐이고, 이 파일은 그 둘을 이 하드웨어에 맞게 구현한다.
 *
 * 이 드라이버가 다른 호스트 컨트롤러와 결정적으로 다른 점은, 세대 1과 2에서
 * config 접근을 **소프트웨어로 TLP 를 조립해** 수행한다는 것이다. 보통의
 * 컨트롤러는 "이 주소에 쓰면 config 사이클이 나간다" 는 창(ECAM 등)을
 * 제공하지만, 이 하드웨어는 그 대신 TLP 를 통째로 넣고 빼는 레지스터 창구를
 * 준다. 그래서 이 파일이 PCIe 스펙의 Configuration Request TLP 헤더 세
 * DWORD 를 직접 만들어 밀어 넣고, 돌아온 Completion TLP 를 직접 해석한다.
 * TLP_CFG_DW0/DW1/DW2 매크로와 get_tlp_header() 가 그 조립부이고,
 * tlp_write_packet() / tlp_read_packet() 이 송수신부다.
 *
 * 지원하는 하드웨어 세대가 셋이며 접근 방식이 각각 다르다.
 *   V1 (altr,pcie-root-port-1.0)
 *       Root Port 자신과 하위 장치 모두 TLP 조립으로 접근한다.
 *       링크 상태는 LTSSM 레지스터를 직접 읽어 판정한다.
 *   V2 (root-port-2.0, Stratix 10)
 *       하위 장치는 여전히 TLP 조립. 다만 Root Port 자신의 config 는
 *       "Hip" 창에 직접 매핑되어 있어 readb/writeb 로 접근한다
 *       (s10_rp_read_cfg / s10_rp_write_cfg). TX/RX 레지스터 배치도 다르다.
 *   V3 (root-port-3.0-{f,p,r}-tile, Agilex)
 *       TLP 조립이 아예 없다. Root Port 도 하위 장치도 창에 매핑되어 있고,
 *       하위 장치는 BDF 레지스터에 대상을 써 둔 뒤 "Cra" 창으로 접근한다
 *       (aglx_ep_read_cfg / aglx_ep_write_cfg).
 * 세 방식의 차이를 struct altera_pcie_ops 콜백 표로 감싸, 위쪽 코드
 * (_altera_pcie_cfg_read / _altera_pcie_cfg_write)는 세대를 몰라도 되게 했다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 등록: 장치 트리에 altr,pcie-root-port-* 노드
 *         -> 플랫폼 버스가 altera_pcie_probe() 를 부른다
 *            -> of_device_get_match_data() 로 세대별 altera_pcie_data 를 고르고
 *            -> altera_pcie_parse_dt() 로 "Cra"/"Hip" 창을 매핑하고 IRQ 를 건다
 *            -> altera_pcie_init_irq_domain() 으로 INTx 도메인을 만들고
 *            -> V1/V2 면 P2A 인터럽트를 켜고 링크 재훈련(altera_pcie_retrain),
 *               V3 면 AER 인터럽트만 켠다
 *            -> pci_host_probe() 로 PCI 코어에 넘긴다
 *
 * config 접근: PCI 코어의 열거·드라이버
 *         -> pci_ops.read/write -> altera_pcie_cfg_read/write
 *            -> altera_pcie_hide_rc_bar() 로 RC 의 BAR0 를 숨기고
 *            -> altera_pcie_valid_device() 로 링크와 슬롯을 확인한 뒤
 *            -> _altera_pcie_cfg_read/write
 *               -> (세대별) rp_read_cfg / ep_read_cfg 콜백,
 *                  또는 tlp_cfg_dword_read/write -> get_tlp_header()
 *                  -> tlp_write_pkt -> tlp_read_pkt
 *
 * 인터럽트: 하위 장치의 INTx
 *         -> 상위 인터럽트 컨트롤러 -> altera_pcie_isr()(체인 핸들러)
 *            -> generic_handle_domain_irq() -> 장치 드라이버의 핸들러
 *         V3 에서는 aglx_isr() 이 AER 인터럽트 하나만 다룬다.
 *
 * 실행 컨텍스트: probe 와 config 접근은 프로세스 컨텍스트다. 다만 config
 * 접근은 PCI 코어가 스핀락(pci_lock)을 쥔 채 부르므로 잠들 수 없고,
 * 그래서 TLP 폴링이 msleep 이 아니라 udelay 로 되어 있다.
 * altera_pcie_isr()/aglx_isr() 은 인터럽트 컨텍스트의 체인 핸들러다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: drivers/pci/probe.c 를 비롯한 PCI 코어 전체가 이 파일의
 *   altera_pcie_ops(pci_ops) 를 통해서만 이 하드웨어에 닿는다.
 *   plaform_driver 로 등록되므로 장치 트리와 플랫폼 버스가 진입점이다.
 * 아래쪽: readl/writel 계열 MMIO 접근(두 창 "Cra" 와 "Hip"),
 *   irqdomain 과 chained_irq(하위 INTx 를 커널 IRQ 로 옮기는 부분),
 *   drivers/pci/pci.h(이 파일은 "../pci.h" 로 그것을 끌어온다).
 * 공유 상태: struct altera_pcie 하나뿐이고, pci_host_bridge 의 private
 *   영역에 얹혀 있다(devm_pci_alloc_host_bridge + pci_host_bridge_priv).
 *   그래서 별도 할당과 해제가 없다.
 *
 * === 주요 함수/구조체 요약 ===
 * altera_pcie_probe() / altera_pcie_remove()
 *                        : 플랫폼 드라이버 진입점과 정리.
 * altera_pcie_parse_dt() : "Cra"/"Hip" 창을 매핑하고 IRQ 를 체인으로 건다.
 * altera_pcie_cfg_read() / altera_pcie_cfg_write()
 *                        : pci_ops 콜백. 유효성 검사 뒤 아래로 넘긴다.
 * _altera_pcie_cfg_read() / _altera_pcie_cfg_write()
 *                        : 세대별 경로를 고르고, TLP 경로일 때는 크기에 맞는
 *                          byte enable 을 만들어 DWORD 단위로 잘라 붙인다.
 * get_tlp_header()       : Configuration Request TLP 의 헤더 세 DWORD 를 만든다.
 *                          이 파일의 핵심이며 아래 별도 절에서 설명한다.
 * tlp_cfg_dword_read() / tlp_cfg_dword_write()
 *                        : 헤더를 만들어 보내고 Completion 을 받는 한 벌.
 * tlp_write_packet() / s10_tlp_write_packet()
 *                        : 조립된 헤더와 데이터를 SOP/EOP 표시와 함께 밀어 넣는다.
 * tlp_read_packet() / s10_tlp_read_packet()
 *                        : Completion TLP 를 폴링으로 받아 상태와 데이터를 꺼낸다.
 * s10_rp_read_cfg() / s10_rp_write_cfg() / aglx_rp_read_cfg() / aglx_rp_write_cfg()
 *                        : Root Port 자신의 config 를 창에 직접 접근한다.
 * aglx_ep_read_cfg() / aglx_ep_write_cfg()
 *                        : V3 의 하위 장치 접근. BDF 레지스터에 대상을 써 둔 뒤
 *                          Type 0/1 을 오프셋 비트에 실어 "Cra" 창으로 접근한다.
 * altera_pcie_link_up() / s10_altera_pcie_link_up() / aglx_altera_pcie_link_up()
 *                        : 세대별 링크 상태 판정.
 * altera_pcie_retrain() / altera_wait_link_retrain()
 *                        : 2.5GT/s 로 붙은 링크를 더 빠른 속도로 재훈련시킨다.
 * altera_pcie_isr() / aglx_isr()
 *                        : 체인 IRQ 핸들러. 전자는 INTx 넷, 후자는 AER 하나.
 * altera_pcie_hide_rc_bar()
 *                        : RC 의 BAR0 를 열거에서 감춘다(위 원문 주석이 이유를 밝힌다).
 *
 * struct altera_pcie     : 이 컨트롤러 하나의 상태 전부.
 * struct altera_pcie_ops : 세대별 동작을 감싸는 콜백 표.
 * struct altera_pcie_data: 세대별 상수 묶음(TLP fmt/type 값, capability
 *                          오프셋, 포트 설정 레지스터 오프셋).
 * struct tlp_rp_regpair_t: V1 의 TX 레지스터 세 개를 한 번에 나르는 묶음.
 * enum altera_pcie_version: V1/V2/V3 구분.
 *
 * === TLP 조립이 이 파일의 핵심인 이유와 그 인코딩 ===
 * PCIe 의 Configuration Request 는 헤더가 세 DWORD 다. 이 파일은 그 셋을
 * 매크로로 만든다.
 *
 *   TLP_CFG_DW0(pcie, cfg) = (cfg << 24) | TLP_PAYLOAD_SIZE
 *     상위 바이트가 fmt/type 이고, 하위가 Length 필드다. Length 는 언제나
 *     1 DWORD(TLP_PAYLOAD_SIZE = 0x01) — config 접근은 한 번에 최대
 *     한 DWORD 이기 때문이다.
 *     fmt/type 값은 파일 위쪽 원문 영어 주석이 붙은 네 상수에서 온다.
 *     0x04 = Configuration Read Type 0, 0x44 = Configuration Write Type 0,
 *     0x05 = Configuration Read Type 1, 0x45 = Configuration Write Type 1.
 *     읽기와 쓰기가 0x40 만큼 차이 나는 것은 그 비트가 fmt 의 "데이터가
 *     따라오는가" 를 뜻하기 때문이고, Type 0 과 Type 1 이 1 만큼 차이 나는
 *     것은 type 필드의 최하위 비트가 그 구분이기 때문이다.
 *     Type 0 은 "이 링크 바로 아래 장치", Type 1 은 "더 아래 버스로 넘겨야
 *     하는 접근" 을 뜻한다 — 그 판정이 get_tlp_header() 의 첫 분기다.
 *
 *   TLP_CFG_DW1(pcie, tag, be) =
 *       (PCI_DEVID(root_bus_nr, RP_DEVFN) << 16) | (tag << 8) | be
 *     상위 절반이 Requester ID(요청을 낸 주체 = 이 Root Port 자신),
 *     그 다음 바이트가 Tag, 최하위 바이트가 byte enable 이다.
 *     Tag 는 읽기와 쓰기에 서로 다른 상수(TLP_READ_TAG 0x1d,
 *     TLP_WRITE_TAG 0x10)를 쓴다. 이 드라이버는 한 번에 하나의 요청만
 *     내고 완료를 기다리므로 태그를 관리할 필요가 없고, 두 값이 다르기만
 *     하면 된다.
 *     byte enable 은 호출자가 접근 크기와 오프셋으로 계산한다 —
 *     1바이트면 1 << (offset & 3), 2바이트면 3 << (offset & 3),
 *     4바이트면 0xf. config 접근이 언제나 DWORD 단위로 나가므로,
 *     그 안에서 어느 바이트가 유효한지 이 필드가 알린다.
 *
 *   TLP_CFG_DW2(bus, devfn, offset) =
 *       (bus << 24) | (devfn << 16) | offset
 *     대상 장치의 BDF 와 레지스터 오프셋이다.
 *
 * 완료 쪽 해석도 매크로 둘로 한다.
 *   TLP_COMP_STATUS(s) = (s >> 13) & 7  — Completion Status. 0 이 성공이고,
 *     그 밖이면 호출자가 PCIBIOS_DEVICE_NOT_FOUND 로 바꾼다. 없는 장치를
 *     찔렀을 때 Unsupported Request 가 돌아오는 것이 이 경로다.
 *   TLP_BYTE_COUNT(s) = s & 0xfff — 돌아온 바이트 수.
 *
 * === 하드웨어 레지스터 값의 근거 ===
 * 이 파일의 레지스터 오프셋(RP_TX_REG0 0x2000, P2A_INT_STATUS 0x3060,
 * RP_LTSSM 0x3c64 등)과 비트 값(LTSSM_L0 0xf, P2A_INT_STS_ALL 0xf,
 * CFG_AER BIT(4) 등)은 모두 이 파일 안에 정의되어 있으나, 그 근거가 되는
 * Altera 하드웨어 문서는 이 트리에 없다. 따라서 아래 주석들은 "그 값이
 * 무엇을 뜻하는지" 를 하드웨어 문서로 확인하지 않고, **코드가 그 상수를
 * 어떻게 쓰는지**(어느 레지스터의 마스크인지, 폴링 조건인지, RW1C 인지)로
 * 설명한다. 값의 의미를 단정하지 않은 곳은 그렇게 밝혀 두었다.
 * 반면 PCI_EXP_LNKSTA_DLLLA 나 PCI_EXP_LNKCTL_RL 같은 PCIe 표준 상수는
 * include/linux/pci_regs.h 에 있어야 하는데 그 헤더도 이 스파스 체크아웃에
 * 없다 — 값은 확인하지 못했고, 코드의 사용 방식으로만 설명한다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일과 아무 접점이 없다(drivers/nvme 전수 grep 0건).
 * 그럴 수밖에 없는 것이, 이 파일은 특정 FPGA 호스트 컨트롤러의 드라이버라
 * 장치 종류와 무관하게 "PCI 버스를 제공" 하는 쪽이기 때문이다.
 *
 * 다만 이 파일을 읽는 것이 NVMe 학습에 주는 것이 하나 있다 — NVMe 컨트롤러의
 * config space 를 읽는다는 일이 실제로 어떤 물리적 동작인지 보여 준다.
 * lspci 가 NVMe 의 vendor ID 를 읽을 때, 이런 하드웨어에서는 이 파일이
 * Configuration Read TLP 를 손으로 조립해 링크로 내보내고 Completion 이
 * 돌아오기를 폴링으로 기다린다. NVMe 드라이버가 pci_read_config_dword() 한
 * 줄로 쓰는 것 아래에 이런 계층이 있다.
 */

/* [한국어] FIELD_PREP 과 GENMASK — V3 가 config 오프셋의 상위 비트에 Type 0/1 표시를
 * 실을 때 쓴다(AGLX_CFG_TARGET) */
#include <linux/bitfield.h>
/* [한국어] udelay — TLP 완료 폴링과 링크 재훈련 대기의 간격을 만든다.
 * config 접근이 스핀락 아래에서 도므로 msleep 을 쓸 수 없다 */
#include <linux/delay.h>
/* [한국어] irqreturn_t 와 인터럽트 관련 타입. 이 파일의 두 ISR 은 체인 핸들러라
 * irq_desc 를 받는다 */
#include <linux/interrupt.h>
/* [한국어] chained_irq_enter/exit — 체인 핸들러가 상위 컨트롤러의 마스킹과
 * 확인응답을 처리하게 해 주는 한 쌍 */
#include <linux/irqchip/chained_irq.h>
/* [한국어] irq_domain, irq_domain_create_linear, generic_handle_domain_irq,
 * dummy_irq_chip, handle_simple_irq — 하위 INTx 를 커널 IRQ 로 옮기는 계층 */
#include <linux/irqdomain.h>
/* [한국어] 이 파일에서 직접 쓰는 심볼은 확인되지 않았다. 상류가 달고 있는 포함 문이며
 * 코드는 고치지 않는다 */
#include <linux/init.h>
/* [한국어] MODULE_DEVICE_TABLE / MODULE_DESCRIPTION / MODULE_LICENSE 와
 * module_platform_driver — 파일 맨 끝의 모듈 등록부 */
#include <linux/module.h>
/* [한국어] of_device_get_match_data — compatible 문자열로 매칭된 세대별 상수 묶음을
 * 꺼낸다. 이 드라이버의 세대 분기가 전부 그 한 줄에서 시작한다 */
#include <linux/of.h>
/* [한국어] 이 파일에서 직접 쓰는 심볼은 확인되지 않았다. 상류 그대로 둔다 */
#include <linux/of_pci.h>
/* [한국어] struct pci_bus, struct pci_ops, PCIBIOS_* 반환 코드, PCI_SLOT,
 * PCI_DEVID, PCI_NUM_INTX, PCI_EXP_* 상수 등 PCI 코어 API */
#include <linux/pci.h>
/* [한국어] struct platform_device, platform_get_irq, platform_set_drvdata,
 * devm_platform_ioremap_resource_byname — 플랫폼 드라이버 뼈대 */
#include <linux/platform_device.h>
/* [한국어] 이 파일에서 직접 쓰는 할당 함수는 확인되지 않았다(구조체는 devm 판
 * 호스트 브리지 할당에 얹혀 온다). 상류 그대로 둔다 */
#include <linux/slab.h>

/* [한국어] PCI 코어 내부 헤더. devm_pci_alloc_host_bridge(), pci_host_bridge_priv(),
 * pci_host_probe(), pci_irqd_intx_xlate() 등이 여기서 온다 */
#include "../pci.h"

/* [한국어] V1 의 TLP 송신 데이터 레지스터 0. 한 번의 송신이 이 레지스터와 REG1 에
 * 두 DWORD 를 싣는다.
 * 읽는 자/쓰는 자: tlp_write_tx() 와 s10_tlp_write_tx() 가 쓴다.
 * 오프셋 값의 근거가 되는 Altera 문서는 이 트리에 없다 */
#define RP_TX_REG0			0x2000
/* [한국어] V1 의 TLP 송신 데이터 레지스터 1. V2 에서는 같은 오프셋이
 * S10_RP_TX_CNTRL(제어)로 쓰인다 — 두 세대의 레지스터 배치가 다르다 */
#define RP_TX_REG1			0x2004
/* [한국어] V1 의 TLP 송신 제어 레지스터. 여기에 쓰는 것이 "보내라" 신호이므로
 * 데이터 두 개를 먼저 채운 뒤 마지막에 쓴다 */
#define RP_TX_CNTRL			0x2008
/* [한국어] 송신 제어의 "패킷 끝"(End Of Packet) 표시. 마지막 묶음에만 붙인다 */
#define RP_TX_EOP			0x2
/* [한국어] 송신 제어의 "패킷 시작"(Start Of Packet) 표시. 첫 묶음에만 붙인다.
 * 둘 다 아닌 0 은 패킷 중간을 뜻한다 */
#define RP_TX_SOP			0x1
/* [한국어] V1 의 완료(Completion) 수신 상태 레지스터. tlp_read_packet() 이 이것을
 * 폴링하며 SOP/EOP 를 기다린다 */
#define RP_RXCPL_STATUS			0x2010
/* [한국어] 수신 상태의 "패킷 끝" 비트. 이것을 본 바퀴에서 결론을 낸다 */
#define RP_RXCPL_EOP			0x2
/* [한국어] 수신 상태의 "패킷 시작" 비트. 이것을 본 바퀴에서 Completion Status 를 뽑는다.
 * 송신 쪽 상수와 값이 같지만 다른 레지스터의 비트라 별도로 정의되어 있다 */
#define RP_RXCPL_SOP			0x1
/* [한국어] V1 의 완료 수신 데이터 레지스터 0. 데이터 DWORD 가 여기 실려 온다 */
#define RP_RXCPL_REG0			0x2014
/* [한국어] V1 의 완료 수신 데이터 레지스터 1. Completion Status 와 Byte Count 가
 * 여기 실려 온다 — TLP_COMP_STATUS/TLP_BYTE_COUNT 가 이 값을 해석한다 */
#define RP_RXCPL_REG1			0x2018
/* [한국어] P2A(Peripheral to Avalon) 인터럽트 상태 레지스터. 하위 장치의 INTx 가
 * 여기 모인다. altera_pcie_isr() 이 읽고, 같은 자리에 1 을 써서 지운다(RW1C
 * 로 보이는 사용 방식) */
#define P2A_INT_STATUS			0x3060
/* [한국어] 그 상태 레지스터에서 INTx 넷에 해당하는 비트 묶음. 0xf 이므로 하위 네
 * 비트다. 다른 비트가 있을 수 있어 마스킹하는 것으로 보이나, 근거가 되는
 * 문서는 이 트리에 없다 */
#define P2A_INT_STS_ALL			0xf
/* [한국어] P2A 인터럽트 활성화 레지스터. probe 에서 한 번 켜고 이후 건드리지 않는다 */
#define P2A_INT_ENABLE			0x3070
/* [한국어] 활성화할 비트 묶음. 상태 쪽과 같은 0xf 로, INTx 넷을 한꺼번에 켠다.
 * 개별 INTx 를 끄고 켤 수단이 없다는 뜻이며, 그래서 IRQ chip 이
 * dummy_irq_chip 이다 */
#define P2A_INT_ENA_ALL			0xf
/* [한국어] V1 의 LTSSM(Link Training and Status State Machine) 상태 레지스터 */
#define RP_LTSSM			0x3c64
/* [한국어] 그 레지스터에서 상태 번호가 들어 있는 필드의 마스크(하위 5비트) */
#define RP_LTSSM_MASK			0x1f
/* [한국어] LTSSM 이 L0(정상 동작) 상태일 때의 값. altera_pcie_link_up() 이
 * 마스킹 뒤 이 값과 동등 비교한다 — 비트 검사가 아니라 상태 번호 비교다 */
#define LTSSM_L0			0xf

/* [한국어] V2 의 TLP 송신 제어 레지스터. V1 의 데이터 REG1 과 같은 오프셋이다 */
#define S10_RP_TX_CNTRL			0x2004
/* [한국어] V2 의 완료 수신 데이터 레지스터. 한 번에 한 DWORD 씩 나온다 */
#define S10_RP_RXCPL_REG		0x2008
/* [한국어] V2 의 완료 수신 상태 레지스터 */
#define S10_RP_RXCPL_STATUS		0x200C
/* [한국어] V2 에서 Root Port 자신의 config space 가 보이는 주소를 계산한다.
 * "Hip" 창 시작점에 레지스터 오프셋과 (1 << 20) 을 더한다.
 * 그 1MB 오프셋의 근거가 되는 Altera 문서는 이 트리에 없다 — 코드가
 * 그것을 "Root Port config 가 보이는 창" 으로 쓴다는 것만 알 수 있다 */
#define S10_RP_CFG_ADDR(pcie, reg) \
	(((pcie)->hip_base) + (reg) + (1 << 20))
/* [한국어] V2 에서 Root Port 의 Secondary Bus 번호를 창에서 직접 읽는다.
 * get_tlp_header() 가 Type 0/Type 1 을 가를 때 쓴다 — 커널이 기억하는
 * 값이 아니라 하드웨어에 되묻는 방식이다 */
#define S10_RP_SECONDARY(pcie) \
	readb(S10_RP_CFG_ADDR(pcie, PCI_SECONDARY_BUS))

/* TLP configuration type 0 and 1 */
#define TLP_FMTTYPE_CFGRD0		0x04	/* Configuration Read Type 0 */
#define TLP_FMTTYPE_CFGWR0		0x44	/* Configuration Write Type 0 */
#define TLP_FMTTYPE_CFGRD1		0x05	/* Configuration Read Type 1 */
#define TLP_FMTTYPE_CFGWR1		0x45	/* Configuration Write Type 1 */
/* [한국어] Configuration Request TLP 의 Length 필드에 넣을 값. config 접근은
 * 언제나 한 DWORD 이므로 1 로 고정이다 */
#define TLP_PAYLOAD_SIZE		0x01
/* [한국어] 읽기 요청의 Tag. 이 드라이버는 한 번에 하나의 요청만 내고 완료를
 * 기다리므로 태그를 관리할 필요가 없고, 쓰기 태그와 다르기만 하면 된다 */
#define TLP_READ_TAG			0x1d
/* [한국어] 쓰기 요청의 Tag. 위와 같은 이유로 임의의 다른 값이다 */
#define TLP_WRITE_TAG			0x10
/* [한국어] Requester ID 를 만들 때 쓰는 Root Port 자신의 devfn. Root Port 는
 * 언제나 장치 0 기능 0 이다 */
#define RP_DEVFN			0
/* [한국어] TLP 헤더 첫 DWORD. 상위 바이트가 fmt/type, 하위가 Length 다.
 * 인자 pcie 는 쓰이지 않는다 — 세 DW 매크로의 모양을 맞춘 것으로 보이며,
 * 코드는 고치지 않고 이 사실만 적어 둔다 */
#define TLP_CFG_DW0(pcie, cfg) \
			(((cfg) << 24) |	\
			  TLP_PAYLOAD_SIZE)
/* [한국어] TLP 헤더 둘째 DWORD. 상위 16비트가 Requester ID(이 Root Port 자신),
 * 다음 바이트가 Tag, 최하위 바이트가 byte enable 이다.
 * PCI_DEVID(bus, devfn) 이 버스와 devfn 을 16비트 ID 로 합친다 */
#define TLP_CFG_DW1(pcie, tag, be) \
	(((PCI_DEVID(pcie->root_bus_nr,  RP_DEVFN)) << 16) | (tag << 8) | (be))
/* [한국어] TLP 헤더 셋째 DWORD. 대상 장치의 버스/devfn 과 레지스터 오프셋이다 */
#define TLP_CFG_DW2(bus, devfn, offset) \
				(((bus) << 24) | ((devfn) << 16) | (offset))
/* [한국어] 완료 TLP 에서 Completion Status 를 뽑는다. 0 이 성공이고, 그 밖이면
 * 호출자가 PCIBIOS_DEVICE_NOT_FOUND 로 바꾼다 — 없는 장치를 찔렀을 때
 * Unsupported Request 가 돌아오는 것이 이 경로다 */
#define TLP_COMP_STATUS(s)		(((s) >> 13) & 7)
/* [한국어] 완료 TLP 에서 Byte Count 를 뽑는다. s10_tlp_read_packet() 이 이것으로
 * "정말 4바이트가 왔는가" 를 확인한다.
 * (((s) >> 0) & 0xfff) 의 >> 0 은 값을 바꾸지 않으며, 위 매크로와 모양을
 * 맞추려는 것으로 보인다 */
#define TLP_BYTE_COUNT(s)		(((s) >> 0) & 0xfff)
/* [한국어] Configuration Request 헤더의 DWORD 개수. get_tlp_header() 가 채우는
 * 배열의 크기이기도 하다 */
#define TLP_HDR_SIZE			3
/* [한국어] TLP 완료를 기다리는 폴링 횟수의 상한. 한 바퀴에 5마이크로초씩 쉬므로
 * 최악 2.5밀리초를 기다린다. 없는 장치를 찌를 때마다 이만큼 걸리므로,
 * altera_pcie_valid_device() 가 미리 걸러 주는 것이 부팅 시간에 직접
 * 영향을 준다 */
#define TLP_LOOP			500

/* [한국어] 링크가 서기를 기다리는 상한. HZ 이므로 1초다 */
#define LINK_UP_TIMEOUT			HZ
/* [한국어] 링크 재훈련이 끝나기를 기다리는 상한. 역시 1초다 */
#define LINK_RETRAIN_TIMEOUT		HZ

/* [한국어] config 오프셋을 DWORD 경계로 내릴 때 쓰는 마스크.
 * (where & ~DWORD_MASK) 형태로 하위 두 비트를 지운다 */
#define DWORD_MASK			3

/* [한국어] V2 의 Configuration Read Type 0 fmt/type 값.
 * V1 의 TLP_FMTTYPE_CFGRD0(0x04)과 값이 다르다 — V2 에서는 0x05 다.
 * get_tlp_header() 의 Type 0/1 판정 방향도 V1 과 반대라 결과가 맞아떨어진다.
 * 그 배치의 근거가 되는 Altera 문서는 이 트리에 없어, 코드가 그렇게
 * 되어 있다는 사실만 적는다 */
#define S10_TLP_FMTTYPE_CFGRD0		0x05
/* [한국어] V2 의 Configuration Read Type 1 fmt/type 값. 역시 V1 과 뒤바뀌어 있다 */
#define S10_TLP_FMTTYPE_CFGRD1		0x04
/* [한국어] V2 의 Configuration Write Type 0 fmt/type 값. 읽기 값에 0x40 이 더해진
 * 형태이며, 그 비트가 "데이터가 따라온다" 는 fmt 표시다 */
#define S10_TLP_FMTTYPE_CFGWR0		0x45
/* [한국어] V2 의 Configuration Write Type 1 fmt/type 값 */
#define S10_TLP_FMTTYPE_CFGWR1		0x44

/* [한국어] V3 에서 Root Port 자신의 config space 주소를 계산한다.
 * V2 와 달리 (1 << 20) 을 더하지 않는다 — 그 차이의 근거가 되는 문서는
 * 이 트리에 없다 */
#define AGLX_RP_CFG_ADDR(pcie, reg)	(((pcie)->hip_base) + (reg))
/* [한국어] V3 에서 Root Port 의 Secondary Bus 번호를 창에서 직접 읽는다.
 * aglx_ep_read_cfg()/aglx_ep_write_cfg() 가 Type 0/Type 1 을 가를 때 쓴다 */
#define AGLX_RP_SECONDARY(pcie) \
	readb(AGLX_RP_CFG_ADDR(pcie, PCI_SECONDARY_BUS))

/* [한국어] V3 에서 접근 대상 BDF 를 지정하는 레지스터("Cra" 창 안).
 * 여기에 대상을 써 둔 뒤 창을 두드리는 간접 접근 방식이다 */
#define AGLX_BDF_REG			0x00002004
/* [한국어] V3 의 포트 인터럽트 상태 레지스터 오프셋. 포트 설정 영역
 * (port_conf_offset)에 상대적이며, 그 영역의 위치는 타일 종류마다 다르다 */
#define AGLX_ROOT_PORT_IRQ_STATUS	0x14c
/* [한국어] V3 의 포트 인터럽트 활성화 레지스터 오프셋. probe 에서 CFG_AER 만 켠다 */
#define AGLX_ROOT_PORT_IRQ_ENABLE	0x150
/* [한국어] 위 두 레지스터에서 AER 인터럽트를 뜻하는 비트.
 * aglx_isr() 이 상태에서 이 비트를 보고, 같은 비트를 되써서 지운다
 * (RW1C 로 보이는 사용 방식). 비트 번호 4 의 근거가 되는 문서는
 * 이 트리에 없다 */
#define CFG_AER				BIT(4)

/* [한국어] V3 에서 config 오프셋의 상위 비트 두 자리를 "어느 대상 공간인가" 로 쓴다.
 * GENMASK(13, 12) 이므로 비트 13-12 다. 실제 config 오프셋이 12비트 안에
 * 들어가므로 그 위 두 비트를 이 용도로 겹쳐 쓸 수 있다 */
#define AGLX_CFG_TARGET			GENMASK(13, 12)
/* [한국어] 그 필드에 넣을 값 — Type 0 config 접근(바로 아래 버스의 장치).
 * 코드에서는 기본값이라 명시적으로 쓰이지 않는다 */
#define AGLX_CFG_TARGET_TYPE0		0
/* [한국어] Type 1 config 접근(더 아래 버스로 넘겨야 하는 접근).
 * aglx_ep_read_cfg()/aglx_ep_write_cfg() 가 FIELD_PREP 으로 얹는다 */
#define AGLX_CFG_TARGET_TYPE1		1
/* [한국어] 로컬 0x2000 영역 접근. 이 파일의 어느 코드도 이 값을 쓰지 않는다 —
 * 하드웨어가 제공하는 대상 종류를 기록해 둔 것으로 보인다 */
#define AGLX_CFG_TARGET_LOCAL_2000	2
/* [한국어] 로컬 0x3000 영역 접근. 역시 이 파일에서 쓰이지 않는다 */
#define AGLX_CFG_TARGET_LOCAL_3000	3

/* [한국어] 지원하는 하드웨어 세대. of_device_get_match_data() 가 고른
 * altera_pcie_data 안에 담겨 오며, 이 파일의 세대별 분기가 이 값을 본다.
 * 읽는 자: get_tlp_header()(Type 0/1 판정 방향),
 *   altera_pcie_parse_dt()("Hip" 창을 매핑할지),
 *   altera_pcie_probe()(초기화 방식).
 * 동기화: probe 에서 한 번 정해지고 이후 읽기 전용 */
enum altera_pcie_version {
	/* [한국어] V1 — altr,pcie-root-port-1.0. Root Port 와 하위 장치 모두 TLP 조립으로
	 * 접근하고, 링크 상태는 LTSSM 레지스터로 판정한다.
	 * 명시적으로 0 을 주어 값이 배열 인덱스처럼 쓰일 수 있게 해 두었다 */
	ALTERA_PCIE_V1 = 0,
	/* [한국어] V2 — altr,pcie-root-port-2.0(Stratix 10). Root Port 는 창에 직접,
	 * 하위 장치는 여전히 TLP 조립으로 접근한다 */
	ALTERA_PCIE_V2,
	/* [한국어] V3 — altr,pcie-root-port-3.0-{f,p,r}-tile(Agilex). TLP 조립이 없고,
	 * Root Port 도 하위 장치도 창에 매핑되어 있다 */
	ALTERA_PCIE_V3,
};

struct altera_pcie {
	/* [한국어] 이 컨트롤러의 플랫폼 장치.
	 * 설정자: altera_pcie_probe().
	 * 읽는 자: altera_pcie_parse_dt() 가 리소스와 IRQ 를 얻을 때,
	 *   그리고 여러 곳이 &pcie->pdev->dev 로 로그 출력 대상을 얻을 때.
	 * 동기화: probe 에서 한 번 쓰고 이후 읽기 전용 */
	struct platform_device	*pdev;
	/* [한국어] "Cra" 창의 커널 가상 주소. TLP 송수신 레지스터, 인터럽트 레지스터,
	 * LTSSM, V3 의 하위 장치 config 창이 모두 이 안에 있다.
	 * 설정자: altera_pcie_parse_dt() 의 devm ioremap.
	 * 읽는 자: cra_readl/writel 계열 여섯 함수 전부.
	 * 값 범위: 모든 세대에서 반드시 유효하다.
	 * 동기화: probe 뒤 읽기 전용. 해제는 devm 이 맡는다 */
	void __iomem		*cra_base;
	/* [한국어] "Hip" 창의 커널 가상 주소. Root Port 자신의 config space 와, V3 의
	 * 포트 설정/인터럽트 레지스터가 여기 있다.
	 * 설정자: altera_pcie_parse_dt() — V2/V3 일 때만 매핑한다.
	 * 읽는 자: S10_RP_CFG_ADDR / AGLX_RP_CFG_ADDR 매크로와 aglx_isr().
	 * 값 범위: V1 에서는 매핑되지 않아 유효하지 않다. 그 세대의 코드 경로가
	 *   이 필드를 건드리지 않으므로 문제가 되지 않는다.
	 * 동기화: probe 뒤 읽기 전용 */
	void __iomem		*hip_base;
	/* [한국어] 이 컨트롤러가 상위 인터럽트 컨트롤러에서 받은 IRQ 번호.
	 * 설정자: altera_pcie_parse_dt() 의 platform_get_irq().
	 * 읽는 자: 같은 함수가 체인 핸들러를 걸 때, altera_pcie_irq_teardown() 이
	 *   풀 때, aglx_isr() 이 오류 로그에 번호를 찍을 때.
	 * 값 범위: 음수면 획득 실패이며 그 경우 probe 가 중단된다 */
	int			irq;
	/* [한국어] 이 Root Port 의 primary 버스 번호(커널이 기억하는 사본).
	 * 설정자: 세 곳이 config 쓰기를 엿보아 갱신한다 —
	 *   tlp_cfg_dword_write(), s10_rp_write_cfg(), aglx_rp_write_cfg().
	 *   PCI 코어가 Root Port 의 PCI_PRIMARY_BUS 에 쓰는 것을 가로채는 방식이다.
	 * 읽는 자: TLP_CFG_DW1 의 Requester ID, get_tlp_header() 의 V1 Type 판정,
	 *   altera_pcie_valid_device(), _altera_pcie_cfg_read/write 의 경로 선택.
	 * 값 범위: probe 시점에는 0(할당 시 0 초기화). 열거가 진행되며 갱신된다.
	 * 동기화: config 접근이 PCI 코어의 pci_lock 으로 직렬화되므로 별도 락이 없다 */
	u8			root_bus_nr;
	/* [한국어] 하위 장치의 INTx 를 커널 IRQ 로 옮기는 도메인.
	 * 설정자: altera_pcie_init_irq_domain().
	 * 읽는 자: altera_pcie_isr()/aglx_isr() 의 generic_handle_domain_irq(),
	 *   altera_pcie_irq_teardown() 의 제거.
	 * 값 범위: 생성 실패 시 NULL 이며 그 경우 probe 가 중단된다 */
	struct irq_domain	*irq_domain;
	/* [한국어] 이 파일의 어느 코드도 이 필드를 읽거나 쓰지 않는다(전수 확인).
	 * 버스 범위를 담으려던 자리로 보이나 지금은 쓰이지 않으며, 코드는
	 * 고치지 않고 이 사실만 적어 둔다 */
	struct resource		bus_range;
	/* [한국어] of_device_get_match_data() 가 고른 세대별 상수 묶음.
	 * 설정자: altera_pcie_probe().
	 * 읽는 자: 이 파일의 거의 모든 함수 — ops 콜백, TLP fmt/type 값,
	 *   capability 오프셋, 포트 레지스터 오프셋이 전부 여기서 나온다.
	 * 값 범위: 다섯 정적 구조체 중 하나를 가리키며 NULL 이면 probe 가 -ENODEV.
	 * 동기화: const 를 가리키는 포인터라 내용이 바뀌지 않는다 */
	const struct altera_pcie_data	*pcie_data;
};

struct altera_pcie_ops {
	/* [한국어] 완료 TLP 를 받는 콜백.
	 * 설정자: altera_pcie_ops_1_0(tlp_read_packet) 과
	 *   altera_pcie_ops_2_0(s10_tlp_read_packet).
	 * 읽는 자: tlp_cfg_dword_read()/tlp_cfg_dword_write().
	 * 값 범위: V3 의 ops 에는 이 필드가 없어 NULL 이다 — 그 세대는 TLP 를
	 *   조립하지 않으므로 그 경로에 닿지 않는다 */
	int (*tlp_read_pkt)(struct altera_pcie *pcie, u32 *value);
	/* [한국어] 조립된 TLP 를 보내는 콜백.
	 * 설정자: V1/V2 의 ops. V3 는 NULL 이다.
	 * 읽는 자: tlp_cfg_dword_read()/tlp_cfg_dword_write().
	 * 인자 align 은 V1 판만 쓰고 V2 판은 무시한다 */
	void (*tlp_write_pkt)(struct altera_pcie *pcie, u32 *headers,
			      u32 data, bool align);
	/* [한국어] 링크 상태를 판정하는 콜백. 세 세대 모두 채운다.
	 * 읽는 자: altera_pcie_valid_device(), altera_pcie_retrain(),
	 *   altera_wait_link_retrain().
	 * 값 범위: 세 구현이 각각 LTSSM, Link Status(readw), Link Status
	 *   (readw_relaxed)를 본다 */
	bool (*get_link_status)(struct altera_pcie *pcie);
	/* [한국어] Root Port 자신의 config 를 읽는 콜백.
	 * 설정자: V2(s10_rp_read_cfg)와 V3(aglx_rp_read_cfg).
	 * 값 범위: V1 은 NULL 이다 — 그 세대는 Root Port 에도 TLP 를 보낸다.
	 * 읽는 자: _altera_pcie_cfg_read() 가 NULL 여부로 경로를 가른다 */
	int (*rp_read_cfg)(struct altera_pcie *pcie, int where,
			   int size, u32 *value);
	/* [한국어] Root Port 자신의 config 를 쓰는 콜백. 읽기 판과 달리 busno 를 받는데,
	 * root_bus_nr 갱신 판정에 필요하기 때문이다.
	 * 값 범위: V1 은 NULL */
	int (*rp_write_cfg)(struct altera_pcie *pcie, u8 busno,
			    int where, int size, u32 value);
	/* [한국어] 하위 장치의 config 를 읽는 콜백.
	 * 설정자: V3(aglx_ep_read_cfg)만 채운다.
	 * 값 범위: V1/V2 는 NULL 이며, 그 경우 _altera_pcie_cfg_read() 가
	 *   TLP 조립 경로로 내려간다 */
	int (*ep_read_cfg)(struct altera_pcie *pcie, u8 busno,
			   unsigned int devfn, int where, int size, u32 *value);
	/* [한국어] 하위 장치의 config 를 쓰는 콜백.
	 * 설정자: V3(aglx_ep_write_cfg)만 채운다.
	 * 값 범위: V1/V2 는 NULL */
	int (*ep_write_cfg)(struct altera_pcie *pcie, u8 busno,
			    unsigned int devfn, int where, int size, u32 value);
	/* [한국어] 체인 IRQ 핸들러.
	 * 설정자: V1/V2 는 altera_pcie_isr(INTx 넷), V3 는 aglx_isr(AER 하나).
	 * 읽는 자: altera_pcie_parse_dt() 가 irq_set_chained_handler_and_data 로
	 *   건다.
	 * 값 범위: 세 세대 모두 채워져 있다 */
	void (*rp_isr)(struct irq_desc *desc);
};

struct altera_pcie_data {
	/* [한국어] 이 세대의 콜백 표.
	 * 설정자: 다섯 정적 altera_pcie_data 초기화자.
	 * 읽는 자: 이 파일 전체가 pcie->pcie_data->ops-> 형태로 부른다 */
	const struct altera_pcie_ops *ops;
	enum altera_pcie_version version;
	u32 cap_offset;		/* PCIe capability structure register offset */
	/* [한국어] Configuration Read Type 0 의 fmt/type 값.
	 * 읽는 자: get_tlp_header().
	 * 값 범위: V1 은 0x04, V2 는 0x05. V3 는 TLP 를 쓰지 않아 0 이다 */
	u32 cfgrd0;
	/* [한국어] Configuration Read Type 1 의 fmt/type 값.
	 * 값 범위: V1 은 0x05, V2 는 0x04 — 두 세대에서 뒤바뀌어 있다 */
	u32 cfgrd1;
	/* [한국어] Configuration Write Type 0 의 fmt/type 값.
	 * 값 범위: V1 은 0x44, V2 는 0x45 */
	u32 cfgwr0;
	/* [한국어] Configuration Write Type 1 의 fmt/type 값.
	 * 값 범위: V1 은 0x45, V2 는 0x44 */
	u32 cfgwr1;
	/* [한국어] V3 의 포트 설정 영역이 "Hip" 창 안에서 시작하는 오프셋.
	 * 값 범위: 타일 종류마다 다르다 — f-tile 0x14000, p-tile 0x104000,
	 *   r-tile 0x1300. V1/V2 는 0 이며 쓰이지 않는다.
	 * 읽는 자: aglx_isr() 과 altera_pcie_probe() 의 V3 갈래 */
	u32 port_conf_offset;
	/* [한국어] 그 영역 안에서 인터럽트 상태 레지스터의 오프셋.
	 * 값 범위: f/p-tile 은 AGLX_ROOT_PORT_IRQ_STATUS(0x14c), r-tile 은 0x0.
	 * 읽는 자: aglx_isr() */
	u32 port_irq_status_offset;
	/* [한국어] 그 영역 안에서 인터럽트 활성화 레지스터의 오프셋.
	 * 값 범위: f/p-tile 은 AGLX_ROOT_PORT_IRQ_ENABLE(0x150), r-tile 은 0x4.
	 * 읽는 자: altera_pcie_probe() 의 V3 갈래 */
	u32 port_irq_enable_offset;
};

/* [한국어] V1 의 TX 레지스터 세 개에 쓸 값을 한 번에 나르는 묶음.
 * 왜 필요한가: V1 은 한 번의 송신이 데이터 둘과 제어 하나를 요구하므로,
 *   세 값을 함께 넘기는 편이 인자 셋을 늘어놓는 것보다 읽기 쉽다.
 * 수명: tlp_write_packet() 의 스택 지역 변수로만 쓰이며 공유되지 않는다 */
struct tlp_rp_regpair_t {
	/* [한국어] 송신 제어 값. RP_TX_SOP(패킷 시작), RP_TX_EOP(패킷 끝), 또는 0(중간).
	 * 설정자/읽는 자: tlp_write_packet() 이 채우고 tlp_write_tx() 가 쓴다.
	 * 필드 순서가 ctrl 이 먼저이지만 실제로 쓰는 순서는 reg0, reg1, ctrl 이다 —
	 * 제어 쓰기가 "보내라" 신호이므로 마지막이어야 한다 */
	u32 ctrl;
	/* [한국어] 송신 데이터 DWORD 0 */
	u32 reg0;
	/* [한국어] 송신 데이터 DWORD 1. 정렬 보정 경로에서는 0 이 들어가기도 한다
	 * (tlp_write_packet 의 align 갈래 참조) */
	u32 reg1;
};

/* [한국어]
 * cra_writel - "Cra" 창의 레지스터에 32비트를 쓴다
 *
 * @pcie: 이 컨트롤러.  @value: 쓸 값.  @reg: 창 시작점으로부터의 오프셋
 * @return: 없음
 *
 * "Cra"(Control Register Access) 창은 altera_pcie_parse_dt() 가 장치 트리의
 * 같은 이름 리소스를 ioremap 해 둔 MMIO 영역이다. 이 파일의 TLP 송수신
 * 레지스터(RP_TX_*, RP_RXCPL_*), 인터럽트 레지스터(P2A_*), LTSSM 레지스터가
 * 모두 이 창 안에 있다.
 *
 * writel_relaxed 를 쓰는 점이 요점이다. _relaxed 판은 메모리 배리어를 걸지
 * 않아 더 빠른 대신, 이 쓰기와 다른 메모리 접근 사이의 순서를 보장하지
 * 않는다. 다만 같은 장치의 같은 창에 대한 접근끼리는 순서가 유지되므로,
 * "레지스터 셋을 순서대로 쓰고 마지막에 제어 레지스터를 친다" 는 이 파일의
 * 쓰임에는 충분하다.
 *
 * 실행 컨텍스트: 제한 없음. MMIO 쓰기 한 번이라 잠들지 않는다.
 *
 * 호출 체인:
 *   tlp_write_tx() / s10_tlp_write_tx() / aglx_ep_write_cfg() /
 *   altera_pcie_isr() / altera_pcie_probe() → [이 함수] → writel_relaxed()
 */
static inline void cra_writel(struct altera_pcie *pcie, const u32 value,
			      const u32 reg)
{
	writel_relaxed(value, pcie->cra_base + reg);
}

/* [한국어]
 * cra_readl - "Cra" 창의 레지스터에서 32비트를 읽는다
 *
 * @pcie: 이 컨트롤러.  @reg: 창 시작점으로부터의 오프셋
 * @return: 읽은 값
 *
 * cra_writel() 의 짝이며, readl_relaxed 를 쓰는 이유도 같다.
 *
 * 이 파일에서 가장 많이 불리는 함수다 — TLP 수신 폴링(tlp_read_packet 과
 * s10_tlp_read_packet)이 최대 500번까지 이것을 반복하고, 링크 상태 판정
 * (altera_pcie_link_up)과 인터럽트 처리도 이것으로 시작한다.
 *
 * 실행 컨텍스트: 제한 없음. 다만 폴링 루프 안에서 불릴 때는 그 루프가
 * udelay 로 간격을 두므로, 버스를 독점하지는 않는다.
 *
 * 호출 체인:
 *   altera_pcie_link_up() / tlp_read_packet() / s10_tlp_read_packet() /
 *   aglx_ep_read_cfg() / altera_pcie_isr() → [이 함수] → readl_relaxed()
 */
static inline u32 cra_readl(struct altera_pcie *pcie, const u32 reg)
{
	return readl_relaxed(pcie->cra_base + reg);
}

/* [한국어]
 * cra_writew - "Cra" 창에 16비트를 쓴다
 *
 * @pcie: 이 컨트롤러.  @value: 쓸 값(하위 16비트만 쓰인다).  @reg: 오프셋
 * @return: 없음
 *
 * V3(Agilex) 전용이다. aglx_ep_write_cfg() 가 크기 2 인 config 쓰기를
 * 처리할 때만 부른다 — 그 세대에서는 하위 장치의 config 가 "Cra" 창에
 * 직접 매핑되어 있어, 접근 크기를 그대로 MMIO 크기로 옮길 수 있기 때문이다.
 *
 * 인자 타입이 u32 인데 16비트만 쓴다. 세 writeX 래퍼의 서명을 맞추려는
 * 것으로 보이며, 코드는 고치지 않고 이 사실만 적어 둔다.
 *
 * 실행 컨텍스트: 제한 없음.
 *
 * 호출 체인:
 *   aglx_ep_write_cfg() → [이 함수] → writew_relaxed()
 */
static inline void cra_writew(struct altera_pcie *pcie, const u32 value,
			      const u32 reg)
{
	writew_relaxed(value, pcie->cra_base + reg);
}

/* [한국어]
 * cra_readw - "Cra" 창에서 16비트를 읽는다
 *
 * @pcie: 이 컨트롤러.  @reg: 오프셋.  @return: 읽은 16비트 값
 *
 * cra_writew() 의 짝이고, V3 의 aglx_ep_read_cfg() 가 크기 2 인 config
 * 읽기에 쓴다.
 *
 * 반환형이 u32 이지만 readw_relaxed 가 돌려주는 것은 16비트라, 상위
 * 16비트는 0 으로 채워진다. 호출자가 그 값을 *value 에 그대로 넣으므로
 * PCI 코어가 기대하는 "상위는 0" 규약과 맞는다.
 *
 * 실행 컨텍스트: 제한 없음.
 *
 * 호출 체인:
 *   aglx_ep_read_cfg() → [이 함수] → readw_relaxed()
 */
static inline u32 cra_readw(struct altera_pcie *pcie, const u32 reg)
{
	return readw_relaxed(pcie->cra_base + reg);
}

/* [한국어]
 * cra_writeb - "Cra" 창에 8비트를 쓴다
 *
 * @pcie: 이 컨트롤러.  @value: 쓸 값(하위 8비트만).  @reg: 오프셋
 * @return: 없음
 *
 * V3 의 aglx_ep_write_cfg() 가 크기 1 인 config 쓰기에 쓴다.
 *
 * 바이트 단위 접근이 그대로 통하는 것이 이 세대의 이점이다. TLP 를
 * 조립하는 V1/V2 경로에서는 같은 일을 하려면 DWORD 로 읽어 해당 바이트만
 * 바꿔 쓰는 대신 byte enable 을 계산해 넘겨야 한다.
 *
 * 실행 컨텍스트: 제한 없음.
 *
 * 호출 체인:
 *   aglx_ep_write_cfg() → [이 함수] → writeb_relaxed()
 */
static inline void cra_writeb(struct altera_pcie *pcie, const u32 value,
			      const u32 reg)
{
	writeb_relaxed(value, pcie->cra_base + reg);
}

/* [한국어]
 * cra_readb - "Cra" 창에서 8비트를 읽는다
 *
 * @pcie: 이 컨트롤러.  @reg: 오프셋.  @return: 읽은 8비트 값
 *
 * V3 의 aglx_ep_read_cfg() 가 크기 1 인 config 읽기에 쓴다.
 * cra_readw() 와 마찬가지로 상위 비트는 0 으로 채워진다.
 *
 * 실행 컨텍스트: 제한 없음.
 *
 * 호출 체인:
 *   aglx_ep_read_cfg() → [이 함수] → readb_relaxed()
 */
static inline u32 cra_readb(struct altera_pcie *pcie, const u32 reg)
{
	return readb_relaxed(pcie->cra_base + reg);
}

/* [한국어]
 * altera_pcie_link_up - V1 세대의 링크 상태를 판정한다
 *
 * @pcie: 이 컨트롤러.  @return: true 면 링크가 서 있다
 *
 * LTSSM(Link Training and Status State Machine) 레지스터를 읽어 현재 상태가
 * L0 인지 본다. L0 은 PCIe 링크가 정상 동작 중인 상태를 뜻한다.
 *
 * RP_LTSSM_MASK 로 하위 비트만 남기고 LTSSM_L0 과 같은지 비교한다.
 * 같은지(==)를 보는 것이지 비트가 서 있는지를 보는 것이 아니다 — LTSSM 은
 * 상태 번호를 담는 필드이지 비트 묶음이 아니기 때문이다.
 * 두 상수의 근거가 되는 Altera 하드웨어 문서는 이 트리에 없어, 값의 의미는
 * 코드의 사용 방식(마스크 뒤 동등 비교)으로만 설명한다.
 *
 * !!(...) 는 이미 bool 인 비교 결과에 다시 붙은 것이라 값이 바뀌지 않는다.
 * 코드는 고치지 않고 이 관찰만 적어 둔다.
 *
 * altera_pcie_ops_1_0 의 get_link_status 콜백으로 등록되며,
 * altera_pcie_valid_device() 와 altera_pcie_retrain() 이 이것을 통해 부른다.
 *
 * 실행 컨텍스트: config 접근 경로(스핀락 보유)에서도 불리므로 잠들지 않는다.
 *
 * 호출 체인:
 *   altera_pcie_valid_device() / altera_wait_link_retrain() → (ops 콜백)
 *     → [이 함수] → cra_readl()
 */
static bool altera_pcie_link_up(struct altera_pcie *pcie)
{
	return !!((cra_readl(pcie, RP_LTSSM) & RP_LTSSM_MASK) == LTSSM_L0);
}

/* [한국어]
 * s10_altera_pcie_link_up - V2(Stratix 10) 세대의 링크 상태를 판정한다
 *
 * @pcie: 이 컨트롤러.  @return: true 면 데이터 링크 계층이 활성이다
 *
 * V1 과 달리 LTSSM 레지스터를 보지 않고, Root Port 자신의 PCIe Capability
 * 안 Link Status 레지스터에서 DLLLA(Data Link Layer Link Active) 비트를 본다.
 * 표준 레지스터를 쓰므로 하드웨어 고유 상수가 필요 없다는 것이 이 방식의
 * 이점이다.
 *
 * 주소 계산이 S10_RP_CFG_ADDR(pcie, cap_offset + PCI_EXP_LNKSTA) 다.
 * 그 매크로가 "Hip" 창 시작점에 (1 << 20) 을 더하는데, 그 오프셋이 왜
 * 1MB 인지는 Altera 하드웨어 문서 소관이라 이 트리에서 확인하지 못했다 —
 * 코드가 그것을 "Root Port 의 config space 가 보이는 창" 으로 쓴다는 것만
 * 알 수 있다.
 * cap_offset 은 altera_pcie_2_0_data 가 0x70 으로 지정한 값이다.
 *
 * readw(_relaxed 아님)를 쓰는 점이 V3 판과 다르다. 코드는 고치지 않고
 * 이 차이만 적어 둔다.
 *
 * 실행 컨텍스트: config 접근 경로에서도 불리므로 잠들지 않는다.
 *
 * 호출 체인:
 *   altera_pcie_valid_device() / altera_wait_link_retrain() → (ops 콜백) → [이 함수]
 */
static bool s10_altera_pcie_link_up(struct altera_pcie *pcie)
{
	void __iomem *addr = S10_RP_CFG_ADDR(pcie,
				   pcie->pcie_data->cap_offset +
				   PCI_EXP_LNKSTA);

	return !!(readw(addr) & PCI_EXP_LNKSTA_DLLLA);
}

/* [한국어]
 * aglx_altera_pcie_link_up - V3(Agilex) 세대의 링크 상태를 판정한다
 *
 * @pcie: 이 컨트롤러.  @return: 0 이 아니면 데이터 링크 계층이 활성이다
 *
 * V2 판과 보는 비트가 같다(Link Status 의 DLLLA). 다른 것은 주소 계산뿐 —
 * AGLX_RP_CFG_ADDR 은 "Hip" 창 시작점에 오프셋을 그대로 더한다.
 * V2 의 (1 << 20) 같은 추가 오프셋이 없는데, 그 차이의 근거가 되는
 * 하드웨어 문서는 이 트리에 없다.
 *
 * readw_relaxed 를 쓰는 것도 V2 판과 다르다.
 *
 * 반환형이 bool 인데 마스킹 결과를 그대로 돌려준다. C 의 bool 변환이
 * 0 이 아닌 값을 true 로 만들므로 동작에는 문제가 없다.
 *
 * 실행 컨텍스트: config 접근 경로에서도 불리므로 잠들지 않는다.
 *
 * 호출 체인:
 *   altera_pcie_valid_device() / altera_wait_link_retrain() → (ops 콜백) → [이 함수]
 */
static bool aglx_altera_pcie_link_up(struct altera_pcie *pcie)
{
	void __iomem *addr = AGLX_RP_CFG_ADDR(pcie,
				   pcie->pcie_data->cap_offset +
				   PCI_EXP_LNKSTA);

	/* [한국어] Link Status 의 DLLLA(Data Link Layer Link Active) 비트를 본다.
	 * V2 판과 같은 판정이지만 _relaxed 판을 쓰고 주소 계산에 추가 오프셋이 없다 */
	return (readw_relaxed(addr) & PCI_EXP_LNKSTA_DLLLA);
}

/*
 * Altera PCIe port uses BAR0 of RC's configuration space as the translation
 * from PCI bus to native BUS.  Entire DDR region is mapped into PCIe space
 * using these registers, so it can be reached by DMA from EP devices.
 * This BAR0 will also access to MSI vector when receiving MSI/MSI-X interrupt
 * from EP devices, eventually trigger interrupt to GIC.  The BAR0 of bridge
 * should be hidden during enumeration to avoid the sizing and resource
 * allocation by PCIe core.
 */
/* [한국어]
 * altera_pcie_hide_rc_bar - Root Complex 의 BAR0 를 열거에서 감출지 판정한다
 *
 * @bus: 접근 대상 버스.  @devfn: 장치/기능 번호.  @offset: config 오프셋
 * @return: true 면 이 접근을 막아야 한다
 *
 * 바로 위 원문 영어 주석이 이유를 온전히 설명한다. 요약하면 — 이 하드웨어는
 * RC config space 의 BAR0 를 "PCI 버스 주소에서 네이티브 버스 주소로의 변환"
 * 설정에 쓴다. DDR 전체가 그 레지스터들을 통해 PCIe 공간에 매핑되어 EP 의
 * DMA 가 닿을 수 있게 되고, EP 가 보낸 MSI/MSI-X 벡터도 이 BAR0 를 거쳐
 * GIC 인터럽트가 된다. 즉 그 자리는 이미 다른 용도로 쓰이고 있다.
 *
 * 그런데 PCI 코어는 열거 중에 모든 BAR 에 0xffffffff 를 써 크기를 알아내고
 * 자원을 배정한다. 그 동작이 이 설정을 망가뜨리므로, 아예 접근 자체를
 * 막아야 한다.
 *
 * 판정은 셋의 논리곱이다 — 루트 버스이고, devfn 이 0(즉 RC 자신)이고,
 * 오프셋이 BAR0 자리일 때만 참이다. 그 밖의 모든 접근은 정상적으로 통과한다.
 *
 * 호출자 두 곳(altera_pcie_cfg_read/write)은 참일 때
 * PCIBIOS_BAD_REGISTER_NUMBER 를 돌려준다 — 읽기에서는 0xffffffff 처럼
 * 보이게 되어 PCI 코어가 "구현되지 않은 BAR" 로 판단하고 넘어간다.
 *
 * 실행 컨텍스트: config 접근 경로. 순수 판정이라 부작용이 없다.
 *
 * 호출 체인:
 *   altera_pcie_cfg_read() / altera_pcie_cfg_write() → [이 함수]
 */
static bool altera_pcie_hide_rc_bar(struct pci_bus *bus, unsigned int  devfn,
				    int offset)
{
	if (pci_is_root_bus(bus) && (devfn == 0) &&
	    (offset == PCI_BASE_ADDRESS_0))
		return true;

	return false;
}

/* [한국어]
 * tlp_write_tx - V1 의 TX 레지스터 세 개에 한 묶음을 밀어 넣는다
 *
 * @pcie: 이 컨트롤러.  @tlp_rp_regdata: 쓸 세 값(reg0, reg1, ctrl)
 * @return: 없음
 *
 * V1 하드웨어의 TLP 송신 창구는 레지스터 셋이다 — 데이터 두 개(REG0, REG1)와
 * 제어 하나(CNTRL). 한 번의 쓰기가 TLP 의 두 DWORD 를 실어 보낸다.
 *
 * 쓰는 순서가 규약이다. 데이터 둘을 먼저 채우고 마지막에 제어를 쓴다 —
 * 제어 레지스터 쓰기가 "이제 보내라" 는 신호이므로, 그보다 먼저 데이터가
 * 자리 잡고 있어야 한다. cra_writel 이 _relaxed 판이지만 같은 장치의 같은
 * 창에 대한 접근끼리는 순서가 유지되므로 이 규약이 성립한다.
 *
 * ctrl 에는 RP_TX_SOP(패킷 시작) 또는 RP_TX_EOP(패킷 끝), 또는 둘 다
 * 아닌 0(중간)이 들어간다. 그 세 상수의 근거가 되는 하드웨어 문서는
 * 이 트리에 없으나, tlp_write_packet() 이 첫 묶음에 SOP 를, 마지막 묶음에
 * EOP 를 붙이는 것으로 그 역할을 코드에서 읽을 수 있다.
 *
 * 실행 컨텍스트: config 접근 경로(스핀락 보유). 잠들지 않는다.
 *
 * 호출 체인:
 *   tlp_write_packet() → [이 함수] → cra_writel()
 */
static void tlp_write_tx(struct altera_pcie *pcie,
			 struct tlp_rp_regpair_t *tlp_rp_regdata)
{
	cra_writel(pcie, tlp_rp_regdata->reg0, RP_TX_REG0);
	/* [한국어] 데이터 두 번째. 아래 제어 쓰기보다 먼저여야 한다 */
	cra_writel(pcie, tlp_rp_regdata->reg1, RP_TX_REG1);
	cra_writel(pcie, tlp_rp_regdata->ctrl, RP_TX_CNTRL);
}

/* [한국어]
 * s10_tlp_write_tx - V2 의 TX 레지스터 두 개에 한 DWORD 를 밀어 넣는다
 *
 * @pcie: 이 컨트롤러.  @reg0: 보낼 DWORD.  @ctrl: SOP/EOP 표시
 * @return: 없음
 *
 * V1 판과 두 가지가 다르다.
 *   - 한 번에 한 DWORD 만 보낸다(V1 은 두 개). 그래서 헤더 세 DWORD 와
 *     데이터 하나를 보내려면 네 번 불러야 한다.
 *   - 제어 레지스터의 오프셋이 다르다. V1 은 RP_TX_CNTRL(0x2008),
 *     V2 는 S10_RP_TX_CNTRL(0x2004) 이다. 흥미롭게도 V2 의 제어 오프셋이
 *     V1 의 데이터 레지스터 REG1 자리와 같은데, 그것은 두 세대의 레지스터
 *     배치가 아예 다르다는 뜻이다. 근거가 되는 하드웨어 문서는 이 트리에
 *     없어 그 이상은 확인하지 못했다.
 *
 * 데이터를 먼저 쓰고 제어를 나중에 쓰는 순서 규약은 V1 과 같다.
 *
 * 실행 컨텍스트: config 접근 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   s10_tlp_write_packet() → [이 함수] → cra_writel()
 */
static void s10_tlp_write_tx(struct altera_pcie *pcie, u32 reg0, u32 ctrl)
{
	cra_writel(pcie, reg0, RP_TX_REG0);
	cra_writel(pcie, ctrl, S10_RP_TX_CNTRL);
}

/* [한국어]
 * altera_pcie_valid_device - 이 (버스, 장치) 조합에 접근해도 되는지 판정한다
 *
 * @pcie: 이 컨트롤러.  @bus: 대상 버스.  @dev: 대상 장치 번호(PCI_SLOT 결과)
 * @return: true 면 접근해도 된다
 *
 * 없는 장치를 찌르면 TLP 폴링이 500번 * 5마이크로초 = 최대 2.5밀리초를
 * 헛되이 기다린다. 열거 중에는 그런 접근이 수없이 일어나므로, 미리 걸러
 * 내는 이 판정이 부팅 시간에 직접 영향을 준다.
 *
 * 두 가지를 본다.
 *   1) 루트 버스가 아닌 곳을 찌르는데 링크가 서 있지 않으면, 그 아래에는
 *      아무 장치도 없다. 원문 주석이 그대로 적고 있다.
 *      링크 판정은 세대별 콜백에 위임한다.
 *   2) 루트 버스에서는 장치 0 만 유효하다. 원문 주석대로 Root Port 하나에
 *      슬롯 하나만 있기 때문이다. PCIe 는 point-to-point 라 한 링크에
 *      장치가 하나뿐이며, 그래서 dev > 0 인 접근은 볼 것도 없이 거짓이다.
 *
 * 루트 버스에 대해서는 링크를 보지 않는 점에 주의한다. Root Port 자신의
 * config 는 링크가 서지 않아도 읽을 수 있어야 하기 때문이다.
 *
 * 실행 컨텍스트: config 접근 경로(스핀락 보유). 잠들지 않는다.
 *
 * 호출 체인:
 *   altera_pcie_cfg_read() / altera_pcie_cfg_write() → [이 함수]
 *     → (ops 콜백) get_link_status
 */
static bool altera_pcie_valid_device(struct altera_pcie *pcie,
				     struct pci_bus *bus, int dev)
{
	/* If there is no link, then there is no device */
	if (bus->number != pcie->root_bus_nr) {
		/* [한국어] 루트 버스가 아닌 곳을 찌르는데 링크가 없다면 그 아래에 장치가 없다.
		 * 세대별 콜백에 판정을 위임한다 */
		if (!pcie->pcie_data->ops->get_link_status(pcie))
			return false;
	}

	/* access only one slot on each root port */
	if (bus->number == pcie->root_bus_nr && dev > 0)
		return false;

	return true;
}

/* [한국어]
 * tlp_read_packet - V1 에서 Completion TLP 를 받아 상태와 데이터를 꺼낸다
 *
 * @pcie: 이 컨트롤러.  @value: 읽은 DWORD 를 담을 곳(쓰기 완료 확인이면 NULL)
 * @return: PCIBIOS_SUCCESSFUL, 또는 PCIBIOS_DEVICE_NOT_FOUND
 *
 * config 요청을 내보낸 뒤 그 응답(Completion TLP)을 기다리는 부분이다.
 * 인터럽트가 아니라 폴링으로 기다린다 — 이 함수가 PCI 코어의 스핀락을
 * 쥔 채 불리므로 잠들 수 없기 때문이다.
 *
 * 폴링 구조: 최대 TLP_LOOP(500)번, 매 바퀴 5마이크로초 쉰다. 즉 최악
 * 2.5밀리초를 기다린다. 바로 위 원문 영어 주석이 왜 여러 바퀴가 필요한지
 * 밝힌다 — 헤더를 읽는 데 최소 두 바퀴, 데이터 페이로드에 한 바퀴다.
 *
 * 한 바퀴에서 하는 일:
 *   상태 레지스터를 읽어 SOP(패킷 시작) 또는 EOP(패킷 끝) 표시가 있거나,
 *   이미 SOP 를 본 뒤(sop 플래그)라면 데이터 레지스터 둘을 읽는다.
 *   - SOP 를 본 바퀴에서는 reg1 에서 Completion Status 를 뽑아 둔다.
 *     그 필드가 첫 헤더 DWORD 에 실려 오기 때문이다.
 *   - EOP 를 본 바퀴에서 결론을 낸다. Completion Status 가 0 이 아니면
 *     요청이 거부된 것이므로 PCIBIOS_DEVICE_NOT_FOUND — 없는 장치를 찔렀을
 *     때 Unsupported Request 가 돌아오는 것이 이 경로다. 0 이면 reg0 을
 *     데이터로 넘긴다.
 *
 * comp_status 를 1 로 초기화하는 것이 방어다. SOP 없이 EOP 만 보는
 * 이상한 경우에도 "실패" 로 떨어지게 한다.
 *
 * value 가 NULL 일 수 있는 이유: 쓰기 경로(tlp_cfg_dword_write)도 완료를
 * 기다려야 하는데, 그때는 돌아온 데이터가 필요 없다.
 *
 * 에러 경로: 500바퀴를 다 돌도록 EOP 를 못 보면 PCIBIOS_DEVICE_NOT_FOUND.
 *
 * 실행 컨텍스트: config 접근 경로(스핀락 보유). udelay 로만 쉬므로
 * 잠들지 않는다.
 *
 * 호출 체인:
 *   tlp_cfg_dword_read() / tlp_cfg_dword_write() → (ops 콜백)
 *     → [이 함수] → cra_readl()
 */
static int tlp_read_packet(struct altera_pcie *pcie, u32 *value)
{
	int i;
	/* [한국어] SOP 를 이미 보았는가. 한 번 보면 EOP 를 만날 때까지 매 바퀴 데이터를 읽는다 */
	bool sop = false;
	/* [한국어] 이번 바퀴에 읽은 수신 상태 값 */
	u32 ctrl;
	/* [한국어] 이번 바퀴에 읽은 데이터 두 DWORD */
	u32 reg0, reg1;
	/* [한국어] Completion Status. 1 로 초기화하는 것이 방어다 — SOP 없이 EOP 만 보는
	 * 이상한 경우에도 "실패" 로 떨어지게 한다 */
	u32 comp_status = 1;

	/*
	 * Minimum 2 loops to read TLP headers and 1 loop to read data
	 * payload.
	 */
	for (i = 0; i < TLP_LOOP; i++) {
		/* [한국어] 수신 상태를 읽는다 */
		ctrl = cra_readl(pcie, RP_RXCPL_STATUS);
		/* [한국어] SOP 나 EOP 표시가 있거나, 이미 SOP 를 본 뒤라면 데이터를 읽을 차례다 */
		if ((ctrl & RP_RXCPL_SOP) || (ctrl & RP_RXCPL_EOP) || sop) {
			/* [한국어] 데이터 DWORD 0. 완료 데이터가 여기 실려 온다 */
			reg0 = cra_readl(pcie, RP_RXCPL_REG0);
			/* [한국어] 데이터 DWORD 1. Completion Status 와 Byte Count 가 여기 실려 온다 */
			reg1 = cra_readl(pcie, RP_RXCPL_REG1);

			/* [한국어] 패킷의 시작 바퀴라면 */
			if (ctrl & RP_RXCPL_SOP) {
				/* [한국어] 이후 바퀴에서도 데이터를 계속 읽도록 표시해 둔다 */
				sop = true;
				/* [한국어] Completion Status 를 지금 뽑아 둔다. 그 필드가 첫 헤더 DWORD 에
				 * 실려 오므로 이 바퀴를 놓치면 값을 잃는다 */
				comp_status = TLP_COMP_STATUS(reg1);
			}

			/* [한국어] 패킷의 끝 바퀴라면 여기서 결론을 낸다 */
			if (ctrl & RP_RXCPL_EOP) {
				/* [한국어] Status 가 0 이 아니면 요청이 거부된 것이다 — 없는 장치를 찔렀을 때
				 * Unsupported Request 가 돌아오는 경로다 */
				if (comp_status)
					return PCIBIOS_DEVICE_NOT_FOUND;

				if (value)
					*value = reg0;

				return PCIBIOS_SUCCESSFUL;
			}
		}
		udelay(5);
	}

	return PCIBIOS_DEVICE_NOT_FOUND;
}

/* [한국어]
 * s10_tlp_read_packet - V2 에서 Completion TLP 를 받아 상태와 데이터를 꺼낸다
 *
 * @pcie: 이 컨트롤러.  @value: 읽은 DWORD 를 담을 곳(NULL 가능)
 * @return: PCIBIOS_SUCCESSFUL, 또는 PCIBIOS_DEVICE_NOT_FOUND
 *
 * V1 판과 목적은 같으나 하드웨어 창구가 달라 구조가 다르다. V2 는 한 번에
 * 한 DWORD 씩 나오므로, 받은 것을 dw[] 배열에 순서대로 쌓는다.
 *
 * 두 단계로 나뉜다.
 *   1) SOP 찾기. 최대 500바퀴 폴링하며 상태 레지스터에 SOP 가 설 때까지
 *      기다린다. 서면 첫 DWORD 를 읽고 루프를 빠져나온다(원문 주석
 *      "Read first DW"). 500바퀴를 다 돌면 원문 주석대로 SOP 검출 실패이며
 *      PCIBIOS_DEVICE_NOT_FOUND 다.
 *   2) EOP 찾기(원문 주석 "Poll for EOP"). 배열이 찰 때까지 계속 읽으며
 *      EOP 를 기다린다. 이 루프에는 udelay 가 없는데, SOP 를 이미 본
 *      시점이면 나머지 DWORD 가 곧바로 이어져 나온다는 전제로 보인다 —
 *      근거가 되는 하드웨어 문서는 이 트리에 없다.
 *
 * EOP 를 본 시점의 판정:
 *   - dw[1] 에서 Completion Status 를 뽑는다. 0 이 아니면 실패.
 *   - 데이터를 넘기는 조건이 셋이다 — value 가 NULL 이 아니고, 돌아온
 *     바이트 수가 정확히 4 이고, 받은 DWORD 가 4개일 것. 그 조건이 맞을
 *     때만 dw[3] 을 넘긴다. Completion 헤더가 3 DWORD 이므로 네 번째가
 *     데이터라는 계산이다. 조건이 안 맞으면 값을 쓰지 않고 성공을
 *     돌려주는데, 코드는 고치지 않고 이 관찰만 적어 둔다.
 *
 * 배열이 찼는데도 EOP 를 못 보면 "Malformed TLP packet" 을 남기고 실패한다.
 *
 * 실행 컨텍스트: config 접근 경로(스핀락 보유). 잠들지 않는다.
 *
 * 호출 체인:
 *   tlp_cfg_dword_read() / tlp_cfg_dword_write() → (ops 콜백)
 *     → [이 함수] → cra_readl()
 */
static int s10_tlp_read_packet(struct altera_pcie *pcie, u32 *value)
{
	u32 ctrl;
	/* [한국어] 완료 상태 코드 */
	u32 comp_status;
	/* [한국어] 받은 DWORD 를 순서대로 쌓을 배열. Completion 헤더 3 개와 데이터 1 개를
	 * 담을 수 있는 크기다 */
	u32 dw[4];
	/* [한국어] 폴링 횟수이자 배열 인덱스로 두 단계에 걸쳐 쓰인다 */
	u32 count;
	/* [한국어] "Malformed TLP packet" 경고를 찍을 대상 */
	struct device *dev = &pcie->pdev->dev;

	/* [한국어] 1단계 — SOP 를 찾을 때까지 최대 500바퀴 폴링한다 */
	for (count = 0; count < TLP_LOOP; count++) {
		/* [한국어] 수신 상태를 읽는다 */
		ctrl = cra_readl(pcie, S10_RP_RXCPL_STATUS);
		if (ctrl & RP_RXCPL_SOP) {
			/* Read first DW */
			dw[0] = cra_readl(pcie, S10_RP_RXCPL_REG);
			break;
		}

		udelay(5);
	}

	/* SOP detection failed, return error */
	if (count == TLP_LOOP)
		return PCIBIOS_DEVICE_NOT_FOUND;

	/* [한국어] 2단계 준비. 첫 DWORD 는 이미 읽었으므로 1 부터 채운다 */
	count = 1;

	/* Poll for EOP */
	while (count < ARRAY_SIZE(dw)) {
		/* [한국어] 수신 상태를 다시 읽는다. 이 루프에는 udelay 가 없는데, SOP 를 본 뒤에는
		 * 나머지 DWORD 가 곧바로 이어져 나온다는 전제로 보인다 — 근거가 되는
		 * 하드웨어 문서는 이 트리에 없다 */
		ctrl = cra_readl(pcie, S10_RP_RXCPL_STATUS);
		/* [한국어] 상태를 읽은 뒤 데이터를 읽어 배열에 쌓는다 */
		dw[count++] = cra_readl(pcie, S10_RP_RXCPL_REG);
		/* [한국어] EOP 를 본 바퀴에서 결론을 낸다 */
		if (ctrl & RP_RXCPL_EOP) {
			/* [한국어] Completion 헤더의 둘째 DWORD 에서 상태를 뽑는다 */
			comp_status = TLP_COMP_STATUS(dw[1]);
			/* [한국어] 0 이 아니면 요청이 거부된 것이다 */
			if (comp_status)
				return PCIBIOS_DEVICE_NOT_FOUND;

			/* [한국어] 데이터를 넘기는 조건이 셋이다 — 담을 곳이 있고, 돌아온 바이트 수가
			 * 정확히 4 이고, 받은 DWORD 가 4개일 것. 조건이 안 맞으면 값을 쓰지 않고
			 * 성공을 돌려주는데, 코드는 고치지 않고 이 관찰만 적어 둔다 */
			if (value && TLP_BYTE_COUNT(dw[1]) == sizeof(u32) &&
			    count == 4)
				*value = dw[3];

			return PCIBIOS_SUCCESSFUL;
		}
	}

	/* [한국어] 배열이 찼는데도 EOP 를 못 봤다. 하드웨어가 규격에 맞지 않는 패킷을
	 * 돌려준 경우이며, 흔치 않으므로 ratelimit 없이 경고를 남긴다 */
	dev_warn(dev, "Malformed TLP packet\n");

	return PCIBIOS_DEVICE_NOT_FOUND;
}

/* [한국어]
 * tlp_write_packet - V1 에서 조립된 TLP 를 링크로 밀어 넣는다
 *
 * @pcie: 이 컨트롤러.  @headers: get_tlp_header() 가 만든 세 DWORD
 * @data: 쓰기일 때의 페이로드(읽기면 0).  @align: 정렬 보정을 넣을 것인가
 * @return: 없음
 *
 * V1 의 TX 창구는 한 번에 두 DWORD 를 받는다. 헤더 셋과 데이터 하나,
 * 합쳐 네 DWORD 를 보내야 하므로 홀짝이 맞지 않는다. 그 처리가 이 함수의
 * 핵심이고 @align 이 그 스위치다.
 *
 *   align 이 거짓: 두 번에 나눠 보낸다.
 *       (헤더0, 헤더1) 에 SOP → (헤더2, 데이터) 에 EOP
 *       네 DWORD 가 두 묶음에 정확히 들어간다.
 *
 *   align 이 참: 세 번에 나눠 보낸다.
 *       (헤더0, 헤더1) 에 SOP → (헤더2, 0) 에 표시 없음
 *       → (데이터, 0) 에 EOP
 *       즉 헤더2 뒤에 빈 자리를 하나 끼워 데이터를 다음 묶음의 첫 자리로
 *       민다. 호출자(tlp_cfg_dword_write)가 대상 오프셋이 8의 배수일 때
 *       이 경로를 고르는 것으로 보아, 데이터가 QWORD 경계의 어느 쪽에
 *       놓이는지를 맞추기 위한 보정이다. 그 정렬을 요구하는 근거가 되는
 *       하드웨어 문서는 이 트리에 없다.
 *
 * 어느 경로든 첫 묶음에 SOP, 마지막 묶음에 EOP 를 붙인다. 그 사이 묶음은
 * ctrl 이 0 이다.
 *
 * 읽기 경로에서도 이 함수를 부른다(data = 0, align = false). Configuration
 * Read TLP 에는 페이로드가 없으므로 그 0 은 하드웨어가 무시하는 자리다.
 *
 * 실행 컨텍스트: config 접근 경로(스핀락 보유). 잠들지 않는다.
 *
 * 호출 체인:
 *   tlp_cfg_dword_read() / tlp_cfg_dword_write() → (ops 콜백)
 *     → [이 함수] → tlp_write_tx()
 */
static void tlp_write_packet(struct altera_pcie *pcie, u32 *headers,
			     u32 data, bool align)
{
	struct tlp_rp_regpair_t tlp_rp_regdata;

	/* [한국어] 첫 묶음의 데이터 0 — 헤더 DWORD 0 */
	tlp_rp_regdata.reg0 = headers[0];
	/* [한국어] 첫 묶음의 데이터 1 — 헤더 DWORD 1 */
	tlp_rp_regdata.reg1 = headers[1];
	/* [한국어] 첫 묶음에 패킷 시작 표시를 붙인다 */
	tlp_rp_regdata.ctrl = RP_TX_SOP;
	/* [한국어] 첫 묶음을 내보낸다 */
	tlp_write_tx(pcie, &tlp_rp_regdata);

	/* [한국어] 정렬 보정이 필요한 경우 — 호출자가 대상 오프셋이 8의 배수일 때 참을 준다 */
	if (align) {
		/* [한국어] 둘째 묶음의 데이터 0 — 헤더 DWORD 2 */
		tlp_rp_regdata.reg0 = headers[2];
		/* [한국어] 둘째 묶음의 데이터 1 은 비워 둔다. 이 빈 자리가 데이터를 다음 묶음의
		 * 첫 자리로 미는 보정이다 */
		tlp_rp_regdata.reg1 = 0;
		/* [한국어] 패킷 중간이므로 표시가 없다 */
		tlp_rp_regdata.ctrl = 0;
		/* [한국어] 둘째 묶음을 내보낸다 */
		tlp_write_tx(pcie, &tlp_rp_regdata);

		/* [한국어] 셋째 묶음의 데이터 0 에 페이로드를 싣는다 */
		tlp_rp_regdata.reg0 = data;
		/* [한국어] 그 뒤는 비어 있다 */
		tlp_rp_regdata.reg1 = 0;
	} else {
		/* [한국어] 보정이 필요 없는 경우 — 헤더 2 와 데이터를 한 묶음에 넣는다.
		 * 네 DWORD 가 두 묶음에 정확히 들어간다 */
		tlp_rp_regdata.reg0 = headers[2];
		/* [한국어] 같은 묶음의 둘째 자리에 페이로드 */
		tlp_rp_regdata.reg1 = data;
	}

	/* [한국어] 마지막 묶음에 패킷 끝 표시를 붙인다. 두 갈래가 여기서 합류한다 */
	tlp_rp_regdata.ctrl = RP_TX_EOP;
	tlp_write_tx(pcie, &tlp_rp_regdata);
}

/* [한국어]
 * s10_tlp_write_packet - V2 에서 조립된 TLP 를 링크로 밀어 넣는다
 *
 * @pcie: 이 컨트롤러.  @headers: 헤더 세 DWORD.  @data: 페이로드
 * @dummy: 쓰이지 않는다(V1 판과 서명을 맞추기 위한 자리)
 * @return: 없음
 *
 * V2 의 TX 창구는 한 번에 한 DWORD 를 받으므로 네 번 부르면 끝이다.
 * V1 판의 align 분기가 필요 없는 이유가 여기 있다 — 홀짝을 맞출 일이
 * 없기 때문이다. 그래서 @dummy 인자는 받기만 하고 쓰지 않는다.
 * struct altera_pcie_ops 의 tlp_write_pkt 서명을 두 세대가 공유해야 해서
 * 남아 있는 자리이며, 코드는 고치지 않고 이 사실만 적어 둔다.
 *
 * SOP 는 첫 DWORD 에, EOP 는 마지막(데이터)에 붙이고 가운데 둘은 0 이다.
 *
 * 실행 컨텍스트: config 접근 경로(스핀락 보유). 잠들지 않는다.
 *
 * 호출 체인:
 *   tlp_cfg_dword_read() / tlp_cfg_dword_write() → (ops 콜백)
 *     → [이 함수] → s10_tlp_write_tx()
 */
static void s10_tlp_write_packet(struct altera_pcie *pcie, u32 *headers,
				 u32 data, bool dummy)
{
	s10_tlp_write_tx(pcie, headers[0], RP_TX_SOP);
	/* [한국어] 헤더 DWORD 1. 중간이므로 표시가 없다 */
	s10_tlp_write_tx(pcie, headers[1], 0);
	/* [한국어] 헤더 DWORD 2 */
	s10_tlp_write_tx(pcie, headers[2], 0);
	s10_tlp_write_tx(pcie, data, RP_TX_EOP);
}

/* [한국어]
 * get_tlp_header - Configuration Request TLP 의 헤더 세 DWORD 를 조립한다
 *
 * @pcie: 이 컨트롤러.  @bus: 대상 버스 번호.  @devfn: 대상 장치/기능
 * @where: 대상 config 오프셋(DWORD 정렬된 값이 넘어온다)
 * @byte_en: 이 DWORD 안에서 유효한 바이트를 나타내는 4비트 마스크
 * @read: 참이면 읽기 요청, 거짓이면 쓰기 요청
 * @headers: 조립 결과를 담을 세 칸 배열(출력)
 * @return: 없음
 *
 * 이 파일의 심장이다. 파일 상단의 "TLP 조립이 이 파일의 핵심인 이유와 그
 * 인코딩" 절이 각 DWORD 의 비트 배치를 설명하므로 함께 읽어야 한다.
 *
 * 하는 일은 셋이다.
 *
 *   1) fmt/type 고르기. 읽기/쓰기와 Type 0/Type 1 의 네 조합 중 하나다.
 *      읽기/쓰기는 @read 로 갈리고, Type 0/1 은 대상 버스로 갈린다.
 *      그 판정이 세대마다 다르다.
 *        V1  : bus == root_bus_nr 이면 cfg0, 아니면 cfg1
 *        V2/V3: bus > S10_RP_SECONDARY(pcie) 이면 cfg0, 아니면 cfg1
 *      두 판정의 방향이 반대인 것처럼 보이지만, 세대별 상수도 함께 뒤집혀
 *      있어 결과가 맞아떨어진다 — V1 은 cfgrd0=0x04(Type 0)/cfgrd1=0x05
 *      (Type 1) 인데 V2 는 cfgrd0=0x05/cfgrd1=0x04 로 서로 바뀌어 있다.
 *      즉 두 세대에서 "cfg0" 이라는 이름이 가리키는 TLP 타입이 다르다.
 *      그 배치의 근거가 되는 하드웨어 문서는 이 트리에 없어, 여기서는
 *      코드가 그렇게 되어 있다는 사실만 적는다.
 *      S10_RP_SECONDARY 는 Root Port 의 Secondary Bus 레지스터를 창에서
 *      직접 읽는 매크로다 — 즉 "이 접근이 내 바로 아래 버스를 향하는가" 를
 *      하드웨어에 되물어 판정하는 셈이다.
 *
 *   2) 태그 고르기. 읽기면 TLP_READ_TAG, 쓰기면 TLP_WRITE_TAG.
 *      이 드라이버는 한 번에 하나의 요청만 내고 완료를 기다리므로 태그를
 *      관리할 필요가 없고, 두 값이 서로 다르기만 하면 된다.
 *
 *   3) 세 DWORD 를 매크로로 만든다. DW0 은 fmt/type 과 길이,
 *      DW1 은 Requester ID 와 태그와 byte enable, DW2 는 대상 BDF 와 오프셋.
 *      Requester ID 에 root_bus_nr 과 RP_DEVFN(0)이 들어가는 것은
 *      "이 요청을 낸 것은 이 Root Port 자신" 이라는 뜻이다.
 *
 * 실행 컨텍스트: config 접근 경로(스핀락 보유). 순수 계산이지만
 * S10_RP_SECONDARY 가 MMIO 읽기를 한다.
 *
 * 호출 체인:
 *   tlp_cfg_dword_read() / tlp_cfg_dword_write() → [이 함수]
 */
static void get_tlp_header(struct altera_pcie *pcie, u8 bus, u32 devfn,
			   int where, u8 byte_en, bool read, u32 *headers)
{
	u8 cfg;
	/* [한국어] Type 0 에 쓸 fmt/type 값. 읽기냐 쓰기냐로 갈린다 */
	u8 cfg0 = read ? pcie->pcie_data->cfgrd0 : pcie->pcie_data->cfgwr0;
	/* [한국어] Type 1 에 쓸 fmt/type 값 */
	u8 cfg1 = read ? pcie->pcie_data->cfgrd1 : pcie->pcie_data->cfgwr1;
	/* [한국어] 요청 태그. 읽기와 쓰기에 서로 다른 상수를 쓰지만, 한 번에 하나의
	 * 요청만 내므로 값 자체에 의미는 없다 */
	u8 tag = read ? TLP_READ_TAG : TLP_WRITE_TAG;

	/* [한국어] 세대에 따라 Type 0/1 판정 방식이 다르다 */
	if (pcie->pcie_data->version == ALTERA_PCIE_V1)
		/* [한국어] V1 — 대상이 root_bus_nr 과 같으면 cfg0. 그 세대에서는 cfg0 이
		 * Type 0(0x04/0x44)이다 */
		cfg = (bus == pcie->root_bus_nr) ? cfg0 : cfg1;
	else
		/* [한국어] V2/V3 — Root Port 의 Secondary Bus 보다 먼 버스면 cfg0.
		 * 판정 방향이 V1 과 반대로 보이지만 그 세대의 cfg0/cfg1 상수도 함께
		 * 뒤바뀌어 있어 결과가 맞아떨어진다. S10_RP_SECONDARY 는 커널이 기억하는
		 * 값이 아니라 하드웨어에 되묻는 방식이다 */
		cfg = (bus > S10_RP_SECONDARY(pcie)) ? cfg0 : cfg1;

	/* [한국어] 헤더 DWORD 0 — fmt/type 과 Length(=1) */
	headers[0] = TLP_CFG_DW0(pcie, cfg);
	/* [한국어] 헤더 DWORD 1 — Requester ID(이 Root Port 자신), Tag, byte enable */
	headers[1] = TLP_CFG_DW1(pcie, tag, byte_en);
	headers[2] = TLP_CFG_DW2(bus, devfn, where);
}

/* [한국어]
 * tlp_cfg_dword_read - TLP 를 조립해 config DWORD 하나를 읽는다
 *
 * @pcie: 이 컨트롤러.  @bus: 대상 버스.  @devfn: 대상 장치/기능
 * @where: DWORD 정렬된 config 오프셋.  @byte_en: 유효 바이트 마스크
 * @value: 읽은 DWORD 를 담을 곳
 * @return: PCIBIOS_SUCCESSFUL, 또는 PCIBIOS_DEVICE_NOT_FOUND
 *
 * "헤더를 만들고, 보내고, 완료를 받는" 세 줄이다. 세 단계 모두 세대별
 * 차이를 아래(get_tlp_header 의 분기)나 옆(ops 콜백)으로 밀어 두어,
 * 이 함수 자체는 세대를 모른다.
 *
 * 읽기 요청에는 페이로드가 없으므로 tlp_write_pkt 에 data = 0, align =
 * false 를 넘긴다.
 *
 * 에러 경로: tlp_read_pkt 이 돌려준 값을 그대로 올린다. 없는 장치를
 * 찔렀거나 폴링이 시간 초과된 경우가 모두 PCIBIOS_DEVICE_NOT_FOUND 로
 * 합쳐진다.
 *
 * 실행 컨텍스트: config 접근 경로(스핀락 보유). 최악 2.5밀리초 폴링한다.
 *
 * 호출 체인:
 *   _altera_pcie_cfg_read() → [이 함수]
 *     → get_tlp_header() → (ops) tlp_write_pkt → (ops) tlp_read_pkt
 */
static int tlp_cfg_dword_read(struct altera_pcie *pcie, u8 bus, u32 devfn,
			      int where, u8 byte_en, u32 *value)
{
	u32 headers[TLP_HDR_SIZE];

	/* [한국어] 읽기 요청이므로 read 인자에 true 를 준다 */
	get_tlp_header(pcie, bus, devfn, where, byte_en, true,
		       headers);

	/* [한국어] 페이로드가 없으므로 data 는 0, 정렬 보정도 필요 없다 */
	pcie->pcie_data->ops->tlp_write_pkt(pcie, headers, 0, false);

	return pcie->pcie_data->ops->tlp_read_pkt(pcie, value);
}

/* [한국어]
 * tlp_cfg_dword_write - TLP 를 조립해 config DWORD 하나를 쓴다
 *
 * @pcie: 이 컨트롤러.  @bus: 대상 버스.  @devfn: 대상 장치/기능
 * @where: DWORD 정렬된 config 오프셋.  @byte_en: 유효 바이트 마스크
 * @value: 쓸 DWORD(호출자가 이미 자리에 맞춰 밀어 둔 값)
 * @return: PCIBIOS_SUCCESSFUL, 또는 PCIBIOS_DEVICE_NOT_FOUND
 *
 * 읽기 판과 셋이 다르다.
 *
 *   1) 정렬 보정. 원문 주석 "check alignment to Qword" 대로 대상 오프셋이
 *      8의 배수인지 보고 tlp_write_pkt 의 align 인자를 정한다. V1 에서는
 *      그 값에 따라 데이터가 놓이는 자리가 달라진다(tlp_write_packet 참조).
 *      V2 판은 이 인자를 무시한다.
 *
 *   2) 쓰기인데도 완료를 기다린다. Configuration Write 는 Non-Posted 라
 *      Completion 이 반드시 돌아오며, 그것을 받아야 다음 요청을 낼 수 있다.
 *      받은 데이터는 필요 없으므로 tlp_read_pkt 에 NULL 을 넘긴다.
 *
 *   3) 마지막의 root_bus_nr 갱신. 원문 영어 주석이 이유를 밝힌다 —
 *      Root Port 의 PCI_PRIMARY_BUS 레지스터가 바뀌는 것을 지켜보다가
 *      지역 사본을 맞춰 둔다.
 *      왜 필요한가: 이 파일은 root_bus_nr 을 두 곳에서 쓴다. TLP 의
 *      Requester ID 를 만들 때(TLP_CFG_DW1)와, Type 0/1 을 가를 때
 *      (get_tlp_header 의 V1 분기). PCI 코어가 버스 번호를 재배정하면
 *      그 값이 달라지는데, 이 하드웨어는 그것을 알려 주지 않으므로
 *      "코어가 쓰는 것을 엿보는" 방식으로 따라간다.
 *      같은 처리가 s10_rp_write_cfg() 와 aglx_rp_write_cfg() 에도 있다 —
 *      세 경로 모두 자기 방식으로 Root Port 에 쓰기 때문이다.
 *
 * 실행 컨텍스트: config 접근 경로(스핀락 보유). 최악 2.5밀리초 폴링한다.
 *
 * 호출 체인:
 *   _altera_pcie_cfg_write() → [이 함수]
 *     → get_tlp_header() → (ops) tlp_write_pkt → (ops) tlp_read_pkt
 */
static int tlp_cfg_dword_write(struct altera_pcie *pcie, u8 bus, u32 devfn,
			       int where, u8 byte_en, u32 value)
{
	u32 headers[TLP_HDR_SIZE];
	/* [한국어] 완료 수신 결과 */
	int ret;

	/* [한국어] 쓰기 요청이므로 read 인자에 false 를 준다 */
	get_tlp_header(pcie, bus, devfn, where, byte_en, false,
		       headers);

	/* check alignment to Qword */
	if ((where & 0x7) == 0)
		/* [한국어] 대상 오프셋이 8의 배수일 때의 경로 — 정렬 보정을 넣는다.
		 * V1 에서는 그 값에 따라 데이터가 놓이는 자리가 달라진다 */
		pcie->pcie_data->ops->tlp_write_pkt(pcie, headers,
					    value, true);
	else
		/* [한국어] 그 밖에는 보정 없이 두 묶음으로 보낸다 */
		pcie->pcie_data->ops->tlp_write_pkt(pcie, headers,
					    value, false);

	/* [한국어] 쓰기도 완료를 기다린다. Configuration Write 는 Non-Posted 라
	 * Completion 이 반드시 돌아오며, 그것을 받아야 다음 요청을 낼 수 있다.
	 * 돌아온 데이터는 필요 없으므로 NULL 을 넘긴다 */
	ret = pcie->pcie_data->ops->tlp_read_pkt(pcie, NULL);
	/* [한국어] 완료가 실패면 그대로 올린다 */
	if (ret != PCIBIOS_SUCCESSFUL)
		return ret;

	/*
	 * Monitor changes to PCI_PRIMARY_BUS register on root port
	 * and update local copy of root bus number accordingly.
	 */
	if ((bus == pcie->root_bus_nr) && (where == PCI_PRIMARY_BUS))
		/* [한국어] PCI 코어가 Root Port 의 primary 버스 번호를 바꾼 것을 엿보아
		 * 지역 사본을 맞춘다. 하드웨어가 알려 주지 않으므로 이 방법뿐이다 */
		pcie->root_bus_nr = (u8)(value);

	return PCIBIOS_SUCCESSFUL;
}

/* [한국어]
 * s10_rp_read_cfg - V2 에서 Root Port 자신의 config 를 창으로 직접 읽는다
 *
 * @pcie: 이 컨트롤러.  @where: config 오프셋.  @size: 1/2/4 바이트
 * @value: 읽은 값을 담을 곳.  @return: 언제나 PCIBIOS_SUCCESSFUL
 *
 * V2 부터는 Root Port 자신의 config space 가 "Hip" 창에 그대로 매핑되어
 * 있어, TLP 를 조립할 필요 없이 MMIO 로 읽으면 된다. 자기 자신에게 TLP 를
 * 보내는 것이 우스운 일이기도 하고, 훨씬 빠르기도 하다.
 *
 * 크기별로 readb/readw/readl 을 골라 접근 폭을 그대로 맞춘다. TLP 경로가
 * 언제나 DWORD 로 읽어 바이트를 잘라 내야 하는 것과 대조된다.
 *
 * default 갈래가 4바이트를 맡는다 — size 가 3 같은 값으로 올 일이 없다는
 * 전제이며, PCI 코어가 1/2/4 만 넘기므로 성립한다.
 *
 * 주소는 S10_RP_CFG_ADDR 이 계산한다. "Hip" 창 시작점에 (1 << 20) 을
 * 더하는데, 그 오프셋의 근거가 되는 하드웨어 문서는 이 트리에 없다.
 *
 * 언제나 성공을 돌려준다. MMIO 읽기는 실패를 알릴 방법이 없기 때문이다.
 *
 * altera_pcie_ops_2_0 의 rp_read_cfg 콜백으로 등록되며,
 * _altera_pcie_cfg_read() 가 대상이 루트 버스일 때 이것을 고른다.
 *
 * 실행 컨텍스트: config 접근 경로(스핀락 보유). 잠들지 않는다.
 *
 * 호출 체인:
 *   _altera_pcie_cfg_read() → (ops 콜백) → [이 함수] → readb/readw/readl
 */
static int s10_rp_read_cfg(struct altera_pcie *pcie, int where,
			   int size, u32 *value)
{
	void __iomem *addr = S10_RP_CFG_ADDR(pcie, where);

	/* [한국어] 접근 크기를 그대로 MMIO 폭으로 옮긴다. TLP 경로가 언제나 DWORD 로
	 * 읽어 잘라 내야 하는 것과 대조된다 */
	switch (size) {
	case 1:
		*value = readb(addr);
		break;
	case 2:
		*value = readw(addr);
		break;
	default:
		*value = readl(addr);
		break;
	}

	return PCIBIOS_SUCCESSFUL;
}

/* [한국어]
 * s10_rp_write_cfg - V2 에서 Root Port 자신의 config 를 창으로 직접 쓴다
 *
 * @pcie: 이 컨트롤러.  @busno: 쓰기 대상 버스(root_bus_nr 갱신 판정에 쓰인다)
 * @where: config 오프셋.  @size: 1/2/4 바이트.  @value: 쓸 값
 * @return: 언제나 PCIBIOS_SUCCESSFUL
 *
 * s10_rp_read_cfg() 의 짝이며, 크기별로 writeb/writew/writel 을 고른다.
 *
 * 마지막의 root_bus_nr 갱신이 중요하다. 원문 영어 주석이
 * tlp_cfg_dword_write() 의 것과 같은 내용을 적고 있다 — Root Port 의
 * PCI_PRIMARY_BUS 레지스터가 바뀌면 지역 사본을 맞춘다.
 * 그 값은 이 세대에서 두 곳에 쓰인다: get_tlp_header() 의 Requester ID
 * (V2 도 하위 장치는 여전히 TLP 로 접근한다)와, _altera_pcie_cfg_read/write
 * 가 "이 접근이 Root Port 자신을 향하는가" 를 가르는 비교다.
 *
 * 읽기 판과 달리 & 0xff 로 하위 바이트만 취한다. PCI_PRIMARY_BUS 는
 * 바이트 필드인데 size 가 4 로 넘어오면 인접 필드(secondary, subordinate)가
 * 함께 실려 오기 때문이다.
 *
 * @busno 를 받는 이유가 여기 있다 — 읽기 판에는 없는 인자다.
 *
 * 실행 컨텍스트: config 접근 경로(스핀락 보유). 잠들지 않는다.
 *
 * 호출 체인:
 *   _altera_pcie_cfg_write() → (ops 콜백) → [이 함수] → writeb/writew/writel
 */
static int s10_rp_write_cfg(struct altera_pcie *pcie, u8 busno,
			    int where, int size, u32 value)
{
	void __iomem *addr = S10_RP_CFG_ADDR(pcie, where);

	/* [한국어] 크기별로 쓰기 폭을 고른다 */
	switch (size) {
	/* [한국어] 1바이트 쓰기 */
	case 1:
		writeb(value, addr);
		break;
	/* [한국어] 2바이트 쓰기 */
	case 2:
		writew(value, addr);
		break;
	default:
		writel(value, addr);
		break;
	}

	/*
	 * Monitor changes to PCI_PRIMARY_BUS register on root port
	 * and update local copy of root bus number accordingly.
	 */
	if (busno == pcie->root_bus_nr && where == PCI_PRIMARY_BUS)
		/* [한국어] 하위 바이트만 취한다. PCI_PRIMARY_BUS 는 바이트 필드인데 size 가 4 로
		 * 넘어오면 인접 필드(secondary, subordinate)가 함께 실려 오기 때문이다 */
		pcie->root_bus_nr = value & 0xff;

	return PCIBIOS_SUCCESSFUL;
}

/* [한국어]
 * aglx_rp_read_cfg - V3 에서 Root Port 자신의 config 를 창으로 직접 읽는다
 *
 * @pcie: 이 컨트롤러.  @where: config 오프셋.  @size: 1/2/4 바이트
 * @value: 읽은 값을 담을 곳.  @return: 언제나 PCIBIOS_SUCCESSFUL
 *
 * V2 판과 크게 둘이 다르다.
 *   - 주소 계산에 (1 << 20) 이 없다(AGLX_RP_CFG_ADDR 참조).
 *   - readb/w/l 대신 _relaxed 판을 쓴다.
 *   - 그리고 아래의 값 보정이 있다.
 *
 * 값 보정이 이 함수의 특징이다. 원문 영어 주석이 첫 갈래의 이유를 밝힌다 —
 * Interrupt Pin 이 하드웨어에 프로그램되어 있지 않아 0 으로 읽히는데,
 * 그러면 PCI 코어가 "이 장치는 INTx 를 쓰지 않는다" 로 판단해 인터럽트를
 * 연결하지 않는다. 그래서 INTA(0x01)로 보이게 고쳐 준다.
 * 둘째 갈래는 Interrupt Line 자리에 0x0100 을 얹는다. 두 필드가 인접해
 * 있어 size 가 2 이상이면 한 번에 읽히는데, 그때도 Pin 자리가 채워지도록
 * 하는 처리로 보인다 — 두 갈래가 else-if 로 묶여 있어 한 번의 읽기에
 * 하나만 적용된다.
 *
 * 이 보정 때문에 이 함수는 "하드웨어를 그대로 보여 주는" 함수가 아니다.
 * 값의 근거가 되는 하드웨어 문서는 이 트리에 없어, 코드가 무엇을 하는지만
 * 적어 둔다.
 *
 * 실행 컨텍스트: config 접근 경로(스핀락 보유). 잠들지 않는다.
 *
 * 호출 체인:
 *   _altera_pcie_cfg_read() → (ops 콜백) → [이 함수]
 */
static int aglx_rp_read_cfg(struct altera_pcie *pcie, int where,
			    int size, u32 *value)
{
	void __iomem *addr = AGLX_RP_CFG_ADDR(pcie, where);

	/* [한국어] 크기별로 읽기 폭을 고른다. V2 판과 달리 _relaxed 를 쓴다 */
	switch (size) {
	case 1:
		*value = readb_relaxed(addr);
		break;
	case 2:
		*value = readw_relaxed(addr);
		break;
	default:
		*value = readl_relaxed(addr);
		break;
	}

	/* Interrupt PIN not programmed in hardware, set to INTA. */
	if (where == PCI_INTERRUPT_PIN && size == 1 && !(*value))
		*value = 0x01;
	else if (where == PCI_INTERRUPT_LINE && !(*value & 0xff00))
		*value |= 0x0100;

	return PCIBIOS_SUCCESSFUL;
}

/* [한국어]
 * aglx_rp_write_cfg - V3 에서 Root Port 자신의 config 를 창으로 직접 쓴다
 *
 * @pcie: 이 컨트롤러.  @busno: root_bus_nr 갱신 판정에 쓰이는 버스 번호
 * @where: config 오프셋.  @size: 1/2/4 바이트.  @value: 쓸 값
 * @return: 언제나 PCIBIOS_SUCCESSFUL
 *
 * s10_rp_write_cfg() 와 구조가 같다. 다른 것은 주소 계산(추가 오프셋 없음)과
 * _relaxed 판 사용뿐이다.
 *
 * 마지막의 root_bus_nr 갱신도 같은 이유로 있다(원문 영어 주석 참조).
 * 다만 V3 에서 그 값이 쓰이는 곳은 V1/V2 와 다르다 — 이 세대는 TLP 를
 * 조립하지 않으므로 Requester ID 를 만들 일이 없고, 오직
 * _altera_pcie_cfg_read/write 의 "Root Port 자신인가" 판정과
 * altera_pcie_valid_device() 의 슬롯 검사에만 쓰인다.
 * 하위 장치의 Type 0/1 판정은 root_bus_nr 이 아니라
 * AGLX_RP_SECONDARY() 로 하드웨어에 되묻는다.
 *
 * 읽기 판의 Interrupt Pin 보정에 대응하는 처리가 쓰기 쪽에는 없다.
 * 코드는 고치지 않고 이 비대칭만 적어 둔다.
 *
 * 실행 컨텍스트: config 접근 경로(스핀락 보유). 잠들지 않는다.
 *
 * 호출 체인:
 *   _altera_pcie_cfg_write() → (ops 콜백) → [이 함수]
 */
static int aglx_rp_write_cfg(struct altera_pcie *pcie, u8 busno,
			     int where, int size, u32 value)
{
	void __iomem *addr = AGLX_RP_CFG_ADDR(pcie, where);

	/* [한국어] 크기별로 쓰기 폭을 고른다 */
	switch (size) {
	/* [한국어] 1바이트 쓰기 */
	case 1:
		writeb_relaxed(value, addr);
		break;
	/* [한국어] 2바이트 쓰기 */
	case 2:
		writew_relaxed(value, addr);
		break;
	default:
		writel_relaxed(value, addr);
		break;
	}

	/*
	 * Monitor changes to PCI_PRIMARY_BUS register on Root Port
	 * and update local copy of root bus number accordingly.
	 */
	if (busno == pcie->root_bus_nr && where == PCI_PRIMARY_BUS)
		/* [한국어] V2 판과 같은 이유로 하위 바이트만 취한다 */
		pcie->root_bus_nr = value & 0xff;

	return PCIBIOS_SUCCESSFUL;
}

/* [한국어]
 * aglx_ep_write_cfg - V3 에서 하위 장치의 config 를 쓴다
 *
 * @pcie: 이 컨트롤러.  @busno: 대상 버스.  @devfn: 대상 장치/기능
 * @where: config 오프셋.  @size: 1/2/4 바이트.  @value: 쓸 값
 * @return: 언제나 PCIBIOS_SUCCESSFUL
 *
 * V3 는 TLP 를 조립하지 않는다. 대신 두 단계로 접근한다.
 *
 *   1) 대상 BDF 를 AGLX_BDF_REG 레지스터에 먼저 써 둔다.
 *      ((busno << 8) | devfn) 형태이며, 이후의 창 접근이 그 대상을 향하게
 *      된다. 즉 "주소를 먼저 세팅하고 데이터 창을 두드리는" 고전적인
 *      간접 접근 방식이다.
 *
 *   2) 오프셋에 Type 0/Type 1 구분을 실어 "Cra" 창에 접근한다.
 *      대상 버스가 Root Port 의 Secondary Bus 보다 크면 — 즉 바로 아래
 *      버스가 아니라 더 먼 곳이면 — AGLX_CFG_TARGET 필드에
 *      AGLX_CFG_TARGET_TYPE1 을 FIELD_PREP 으로 얹는다. 그러면
 *      하드웨어가 Type 1 config 요청으로 내보낸다.
 *      Secondary Bus 이하면 아무것도 얹지 않아 기본값(Type 0)이 된다.
 *      AGLX_CFG_TARGET 은 GENMASK(13, 12) 이므로 오프셋의 상위 비트 두
 *      자리를 "어느 대상 공간인가" 로 쓰는 셈이고, 그래서 실제 config
 *      오프셋과 이 표시가 한 값에 공존할 수 있다.
 *
 *   3) 크기에 맞는 cra_writeb/w/l 로 쓴다.
 *
 * 이 방식에는 경합 위험이 내재한다 — BDF 레지스터를 쓰고 창에 접근하는
 * 사이에 다른 CPU 가 끼어들면 엉뚱한 장치에 쓰게 된다. 이 파일에는 그것을
 * 막는 락이 없는데, PCI 코어가 config 접근 전체를 pci_lock 으로 직렬화하기
 * 때문에 성립하는 것으로 보인다. 코드는 고치지 않고 이 관찰만 적어 둔다.
 *
 * default 갈래의 break 들여쓰기가 다른 갈래와 다르다. 동작에는 영향이 없다.
 *
 * 실행 컨텍스트: config 접근 경로(스핀락 보유). 잠들지 않는다.
 *
 * 호출 체인:
 *   _altera_pcie_cfg_write() → (ops 콜백) → [이 함수] → cra_writel/w/b
 */
static int aglx_ep_write_cfg(struct altera_pcie *pcie, u8 busno,
			     unsigned int devfn, int where, int size, u32 value)
{
	cra_writel(pcie, ((busno << 8) | devfn), AGLX_BDF_REG);
	/* [한국어] 대상이 Root Port 의 Secondary Bus 보다 먼 버스인가 */
	if (busno > AGLX_RP_SECONDARY(pcie))
		/* [한국어] 그렇다면 오프셋 상위 비트에 Type 1 표시를 얹는다. 실제 config 오프셋이
		 * 12비트 안에 들어가므로 비트 13-12 를 이 용도로 겹쳐 쓸 수 있다 */
		where |= FIELD_PREP(AGLX_CFG_TARGET, AGLX_CFG_TARGET_TYPE1);

	/* [한국어] 크기별로 쓰기 폭을 고른다. 하위 장치 config 가 창에 매핑되어 있어
	 * 접근 크기를 그대로 옮길 수 있다 */
	switch (size) {
	/* [한국어] 1바이트 쓰기 */
	case 1:
		cra_writeb(pcie, value, where);
		break;
	/* [한국어] 2바이트 쓰기 */
	case 2:
		cra_writew(pcie, value, where);
		break;
	default:
		cra_writel(pcie, value, where);
			break;
	}

	return PCIBIOS_SUCCESSFUL;
}

/* [한국어]
 * aglx_ep_read_cfg - V3 에서 하위 장치의 config 를 읽는다
 *
 * @pcie: 이 컨트롤러.  @busno: 대상 버스.  @devfn: 대상 장치/기능
 * @where: config 오프셋.  @size: 1/2/4 바이트.  @value: 읽은 값을 담을 곳
 * @return: 언제나 PCIBIOS_SUCCESSFUL
 *
 * aglx_ep_write_cfg() 의 정확한 거울상이다. BDF 레지스터에 대상을 써 두고,
 * Secondary Bus 와 비교해 Type 1 표시를 오프셋에 얹은 뒤, 크기에 맞는
 * cra_readb/w/l 로 읽는다.
 *
 * 없는 장치를 찔렀을 때의 처리가 TLP 경로와 다르다는 점에 주의한다.
 * TLP 경로는 Completion Status 로 실패를 알 수 있어
 * PCIBIOS_DEVICE_NOT_FOUND 를 돌려주지만, 이 경로는 언제나 성공을
 * 돌려주고 값으로 0xffffffff 가 읽힌다. PCI 코어는 그 값을 "장치 없음"
 * 으로 해석하므로 결과적으로는 같은 판단에 이른다.
 * 그 앞에서 altera_pcie_valid_device() 가 링크 유무를 먼저 걸러 주기도 한다.
 *
 * 실행 컨텍스트: config 접근 경로(스핀락 보유). 잠들지 않는다.
 *
 * 호출 체인:
 *   _altera_pcie_cfg_read() → (ops 콜백) → [이 함수] → cra_readl/w/b
 */
static int aglx_ep_read_cfg(struct altera_pcie *pcie, u8 busno,
			    unsigned int devfn, int where, int size, u32 *value)
{
	cra_writel(pcie, ((busno << 8) | devfn), AGLX_BDF_REG);
	/* [한국어] 읽기 쪽도 같은 Type 판정을 한다 */
	if (busno > AGLX_RP_SECONDARY(pcie))
		/* [한국어] Type 1 표시를 오프셋에 얹는다 */
		where |= FIELD_PREP(AGLX_CFG_TARGET, AGLX_CFG_TARGET_TYPE1);

	/* [한국어] 크기별로 읽기 폭을 고른다 */
	switch (size) {
	case 1:
		*value = cra_readb(pcie, where);
		break;
	case 2:
		*value = cra_readw(pcie, where);
		break;
	default:
		*value = cra_readl(pcie, where);
		break;
	}

	return PCIBIOS_SUCCESSFUL;
}

/* [한국어]
 * _altera_pcie_cfg_read - 세대별 경로를 골라 config 를 읽는다
 *
 * @pcie: 이 컨트롤러.  @busno: 대상 버스.  @devfn: 대상 장치/기능
 * @where: config 오프셋(정렬 안 된 값도 온다).  @size: 1/2/4 바이트
 * @value: 읽은 값을 담을 곳
 * @return: PCIBIOS_SUCCESSFUL, 또는 아래 경로가 돌려준 오류
 *
 * 세 갈래로 나뉜다. 앞의 둘은 콜백이 있으면 그대로 위임하고, 셋째가
 * V1/V2 의 TLP 경로다.
 *
 *   1) 대상이 루트 버스이고 rp_read_cfg 콜백이 있으면(V2/V3) 그쪽으로.
 *   2) ep_read_cfg 콜백이 있으면(V3) 그쪽으로.
 *   3) 둘 다 아니면 TLP 를 조립한다(V1 전체, V2 의 하위 장치).
 *
 * TLP 경로에서 하는 일이 이 함수의 본체다. PCIe config 요청은 언제나
 * DWORD 단위로 나가므로, 임의 크기·임의 오프셋 요청을 그 형태로 옮기고
 * 돌아온 DWORD 에서 원하는 바이트를 잘라 내야 한다.
 *
 *   byte enable 계산:
 *       1바이트 → 1 << (where & 3)   해당 바이트 한 자리만
 *       2바이트 → 3 << (where & 3)   인접 두 자리
 *       4바이트 → 0xf                네 자리 전부
 *     (where & 3) 이 DWORD 안에서의 바이트 위치다.
 *
 *   요청은 (where & ~DWORD_MASK) 로 DWORD 경계에 맞춰 보낸다.
 *
 *   잘라 내기:
 *       1바이트 → (data >> (8 * (where & 0x3))) & 0xff
 *       2바이트 → (data >> (8 * (where & 0x2))) & 0xffff
 *       4바이트 → 그대로
 *     2바이트 갈래의 마스크가 0x2 인 점에 주의한다 — 0x3 이 아니다.
 *     16비트 접근은 오프셋이 2의 배수라 하위 비트가 0 이므로, 0x2 로
 *     걸러도 결과가 같고 시프트가 0 또는 16 만 나오게 된다.
 *
 * 에러 경로: TLP 읽기가 실패하면 그 값을 그대로 올린다. 이때 *value 는
 * 건드리지 않는다 — PCI 코어가 실패 시 0xffffffff 를 채워 넣는다.
 *
 * 실행 컨텍스트: config 접근 경로(스핀락 보유). 잠들지 않는다.
 *
 * 호출 체인:
 *   altera_pcie_cfg_read() / altera_read_cap_word() → [이 함수]
 *     → (ops) rp_read_cfg / ep_read_cfg, 또는 tlp_cfg_dword_read()
 */
static int _altera_pcie_cfg_read(struct altera_pcie *pcie, u8 busno,
				 unsigned int devfn, int where, int size,
				 u32 *value)
{
	int ret;
	/* [한국어] TLP 경로에서 받아 올 DWORD */
	u32 data;
	/* [한국어] 이 DWORD 안에서 어느 바이트가 유효한지 알리는 4비트 마스크 */
	u8 byte_en;

	/* [한국어] 대상이 Root Port 자신이고 그 전용 콜백이 있으면(V2/V3) */
	if (busno == pcie->root_bus_nr && pcie->pcie_data->ops->rp_read_cfg)
		/* [한국어] 창 직접 접근으로 위임한다 */
		return pcie->pcie_data->ops->rp_read_cfg(pcie, where,
							 size, value);

	/* [한국어] 하위 장치 전용 콜백이 있으면(V3) */
	if (pcie->pcie_data->ops->ep_read_cfg)
		/* [한국어] 그쪽으로 위임한다. 여기까지 오지 않으면 아래 TLP 경로다 */
		return pcie->pcie_data->ops->ep_read_cfg(pcie, busno, devfn,
							where, size, value);

	/* [한국어] 접근 크기에 맞는 byte enable 을 만든다 */
	switch (size) {
	/* [한국어] 1바이트 */
	case 1:
		byte_en = 1 << (where & 3);
		break;
	/* [한국어] 2바이트 */
	case 2:
		byte_en = 3 << (where & 3);
		break;
	default:
		byte_en = 0xf;
		break;
	}

	/* [한국어] DWORD 경계로 내린 오프셋으로 요청을 보낸다 */
	ret = tlp_cfg_dword_read(pcie, busno, devfn,
				 (where & ~DWORD_MASK), byte_en, &data);
	/* [한국어] 요청이 실패하면 그대로 올린다. *value 는 건드리지 않는다 —
	 * PCI 코어가 실패 시 0xffffffff 를 채워 넣는다 */
	if (ret != PCIBIOS_SUCCESSFUL)
		return ret;

	/* [한국어] 돌아온 DWORD 에서 원하는 바이트를 잘라 낸다 */
	switch (size) {
	case 1:
		*value = (data >> (8 * (where & 0x3))) & 0xff;
		break;
	case 2:
		*value = (data >> (8 * (where & 0x2))) & 0xffff;
		break;
	default:
		*value = data;
		break;
	}

	return PCIBIOS_SUCCESSFUL;
}

/* [한국어]
 * _altera_pcie_cfg_write - 세대별 경로를 골라 config 를 쓴다
 *
 * @pcie: 이 컨트롤러.  @busno: 대상 버스.  @devfn: 대상 장치/기능
 * @where: config 오프셋.  @size: 1/2/4 바이트.  @value: 쓸 값
 * @return: PCIBIOS_SUCCESSFUL, 또는 아래 경로가 돌려준 오류
 *
 * _altera_pcie_cfg_read() 의 거울상이다. 갈래 셋(rp_write_cfg /
 * ep_write_cfg / TLP)의 구조가 같다.
 *
 * 읽기와 다른 점은 값을 자리에 맞춰 **미리 밀어 둔다**는 것이다.
 *   shift = 8 * (where & 3) 로 DWORD 안에서의 바이트 위치를 구하고,
 *   1바이트 → (value & 0xff) << shift
 *   2바이트 → (value & 0xffff) << shift
 *   4바이트 → 그대로
 * 읽기 쪽이 받은 값을 오른쪽으로 밀어 잘라 내는 것과 반대 방향이다.
 *
 * byte enable 계산은 읽기와 완전히 같다. 그 마스크가 하드웨어에게
 * "이 DWORD 중 이 바이트만 실제로 써라" 를 알리므로, 나머지 바이트에
 * 어떤 값이 실려 있든 무시된다. 읽고-고치고-쓰기를 하지 않아도 되는
 * 이유가 이것이다 — config 접근에서 인접 필드를 건드리지 않는 것이
 * 중요한데(예: PCI_COMMAND 옆의 PCI_STATUS 는 RW1C 다) byte enable 이
 * 그것을 하드웨어 수준에서 보장한다.
 *
 * shift 를 함수 앞에서 미리 계산하는 점이 읽기 쪽과 다르다. 4바이트
 * 갈래에서는 쓰이지 않는 값이 되지만, 코드는 고치지 않고 이 사실만 적어 둔다.
 *
 * 실행 컨텍스트: config 접근 경로(스핀락 보유). 잠들지 않는다.
 *
 * 호출 체인:
 *   altera_pcie_cfg_write() / altera_write_cap_word() → [이 함수]
 *     → (ops) rp_write_cfg / ep_write_cfg, 또는 tlp_cfg_dword_write()
 */
static int _altera_pcie_cfg_write(struct altera_pcie *pcie, u8 busno,
				  unsigned int devfn, int where, int size,
				  u32 value)
{
	u32 data32;
	/* [한국어] DWORD 안에서의 바이트 위치. 값을 그 자리로 밀어 올릴 때 쓴다.
	 * 4바이트 갈래에서는 쓰이지 않는 값이 되지만, 코드는 고치지 않고
	 * 이 사실만 적어 둔다 */
	u32 shift = 8 * (where & 3);
	/* [한국어] 쓰기 대상 바이트를 알리는 마스크 */
	u8 byte_en;

	/* [한국어] 대상이 Root Port 자신이고 그 전용 콜백이 있으면(V2/V3) */
	if (busno == pcie->root_bus_nr && pcie->pcie_data->ops->rp_write_cfg)
		/* [한국어] 창 직접 접근으로 위임한다 */
		return pcie->pcie_data->ops->rp_write_cfg(pcie, busno,
							  where, size, value);

	/* [한국어] 하위 장치 전용 콜백이 있으면(V3) */
	if (pcie->pcie_data->ops->ep_write_cfg)
		/* [한국어] 그쪽으로 위임한다 */
		return pcie->pcie_data->ops->ep_write_cfg(pcie, busno, devfn,
							 where, size, value);

	/* [한국어] 크기에 따라 값을 자리에 맞춰 밀고 byte enable 을 만든다 */
	switch (size) {
	/* [한국어] 1바이트 쓰기 */
	case 1:
		data32 = (value & 0xff) << shift;
		/* [한국어] 해당 바이트 한 자리만 유효하다고 알린다 */
		byte_en = 1 << (where & 3);
		break;
	/* [한국어] 2바이트 쓰기 */
	case 2:
		data32 = (value & 0xffff) << shift;
		/* [한국어] 인접 두 자리가 유효하다고 알린다 */
		byte_en = 3 << (where & 3);
		break;
	default:
		data32 = value;
		/* [한국어] 4바이트면 네 자리 전부 */
		byte_en = 0xf;
		break;
	}

	/* [한국어] DWORD 경계로 내린 오프셋과 함께 보낸다. byte enable 이 있으므로
	 * 읽고-고치고-쓰기 없이도 인접 필드를 건드리지 않는다 */
	return tlp_cfg_dword_write(pcie, busno, devfn, (where & ~DWORD_MASK),
				   byte_en, data32);
}

/* [한국어]
 * altera_pcie_cfg_read - PCI 코어가 부르는 config 읽기 진입점
 *
 * @bus: 대상 버스.  @devfn: 대상 장치/기능.  @where: config 오프셋
 * @size: 1/2/4 바이트.  @value: 읽은 값을 담을 곳
 * @return: PCIBIOS_SUCCESSFUL / PCIBIOS_BAD_REGISTER_NUMBER /
 *          PCIBIOS_DEVICE_NOT_FOUND
 *
 * struct pci_ops 의 .read 콜백이다. 이 파일이 PCI 코어에 노출하는 두
 * 진입점 중 하나이며, lspci 부터 드라이버의 pci_read_config_dword() 까지
 * 모든 config 읽기가 결국 여기로 온다.
 *
 * bus->sysdata 에서 컨트롤러 상태를 되찾는 것이 첫 동작이다.
 * altera_pcie_probe() 가 bridge->sysdata 에 넣어 둔 값이다.
 *
 * 두 검사를 거친다.
 *   1) altera_pcie_hide_rc_bar() — RC 의 BAR0 는 이 하드웨어에서 주소 변환
 *      설정에 쓰이므로 열거에서 감춘다. PCIBIOS_BAD_REGISTER_NUMBER 를
 *      돌려주면 PCI 코어가 그 자리를 구현되지 않은 것으로 본다.
 *   2) altera_pcie_valid_device() — 링크가 없거나 슬롯 번호가 0 이 아니면
 *      없는 장치다. 이 검사가 없으면 TLP 폴링이 최악 2.5밀리초를 헛되이
 *      기다리게 되고, 열거 중 그런 접근이 수없이 일어난다.
 *
 * 실제 읽기는 _altera_pcie_cfg_read() 에 넘긴다.
 *
 * 실행 컨텍스트: PCI 코어가 pci_lock 스핀락을 쥔 채 부른다. 잠들 수 없고,
 * 그래서 아래 경로가 전부 udelay 폴링으로 되어 있다.
 *
 * 호출 체인:
 *   (PCI 코어의 config 읽기) → pci_ops.read → [이 함수]
 *     → altera_pcie_hide_rc_bar() → altera_pcie_valid_device()
 *     → _altera_pcie_cfg_read()
 */
static int altera_pcie_cfg_read(struct pci_bus *bus, unsigned int devfn,
				int where, int size, u32 *value)
{
	struct altera_pcie *pcie = bus->sysdata;

	/* [한국어] RC 의 BAR0 는 이 하드웨어에서 주소 변환 설정에 쓰이므로 감춘다 */
	if (altera_pcie_hide_rc_bar(bus, devfn, where))
		return PCIBIOS_BAD_REGISTER_NUMBER;

	/* [한국어] 링크가 없거나 슬롯이 0 이 아니면 없는 장치다. 이 검사가 없으면
	 * TLP 폴링이 최악 2.5밀리초를 헛되이 기다린다 */
	if (!altera_pcie_valid_device(pcie, bus, PCI_SLOT(devfn)))
		return PCIBIOS_DEVICE_NOT_FOUND;

	/* [한국어] 실제 읽기는 아래 계층에 넘긴다 */
	return _altera_pcie_cfg_read(pcie, bus->number, devfn, where, size,
				     value);
}

/* [한국어]
 * altera_pcie_cfg_write - PCI 코어가 부르는 config 쓰기 진입점
 *
 * @bus: 대상 버스.  @devfn: 대상 장치/기능.  @where: config 오프셋
 * @size: 1/2/4 바이트.  @value: 쓸 값
 * @return: PCIBIOS_SUCCESSFUL / PCIBIOS_BAD_REGISTER_NUMBER /
 *          PCIBIOS_DEVICE_NOT_FOUND
 *
 * struct pci_ops 의 .write 콜백이며, 읽기 판과 같은 두 검사를 거친 뒤
 * _altera_pcie_cfg_write() 에 넘긴다.
 *
 * BAR0 감추기가 쓰기에서 더 중요하다. PCI 코어는 BAR 크기를 알아내려고
 * 0xffffffff 를 써 보는데, 그 쓰기가 실제로 나가면 이 하드웨어의 주소 변환
 * 설정이 깨져 EP 의 DMA 와 MSI 가 모두 망가진다. 이 검사가 그것을 막는다.
 *
 * 실행 컨텍스트: PCI 코어가 pci_lock 스핀락을 쥔 채 부른다. 잠들 수 없다.
 *
 * 호출 체인:
 *   (PCI 코어의 config 쓰기) → pci_ops.write → [이 함수]
 *     → altera_pcie_hide_rc_bar() → altera_pcie_valid_device()
 *     → _altera_pcie_cfg_write()
 */
static int altera_pcie_cfg_write(struct pci_bus *bus, unsigned int devfn,
				 int where, int size, u32 value)
{
	struct altera_pcie *pcie = bus->sysdata;

	/* [한국어] 쓰기에서 BAR0 감추기가 더 중요하다. PCI 코어가 크기를 알아내려고
	 * 0xffffffff 를 쓰는데, 그것이 실제로 나가면 주소 변환 설정이 깨져
	 * EP 의 DMA 와 MSI 가 모두 망가진다 */
	if (altera_pcie_hide_rc_bar(bus, devfn, where))
		return PCIBIOS_BAD_REGISTER_NUMBER;

	/* [한국어] 없는 장치에 대한 쓰기를 막는다 */
	if (!altera_pcie_valid_device(pcie, bus, PCI_SLOT(devfn)))
		return PCIBIOS_DEVICE_NOT_FOUND;

	/* [한국어] 실제 쓰기는 아래 계층에 넘긴다 */
	return _altera_pcie_cfg_write(pcie, bus->number, devfn, where, size,
				      value);
}

/* [한국어] PCI 코어에 노출하는 연산 표. 이 두 콜백이 이 파일과 코어의 유일한 접점이다 */
static struct pci_ops altera_pcie_ops = {
	/* [한국어] config 읽기 진입점 */
	.read = altera_pcie_cfg_read,
	.write = altera_pcie_cfg_write,
};

/* [한국어]
 * altera_read_cap_word - PCIe Capability 안의 16비트 레지스터를 읽는다
 *
 * @pcie: 이 컨트롤러.  @busno: 대상 버스.  @devfn: 대상 장치/기능
 * @offset: PCIe Capability 시작점으로부터의 오프셋(PCI_EXP_LNKSTA 등)
 * @value: 읽은 16비트를 담을 곳
 * @return: 아래 읽기가 돌려준 값
 *
 * 링크 재훈련 코드가 Link Capability/Status/Control 을 읽을 때 쓰는 헬퍼다.
 * 매번 cap_offset 을 더하는 것을 한 곳에 모아 둔 것이며, 그 값은 세대별
 * altera_pcie_data 가 정한다(V1 은 0x80, V2/V3 은 0x70).
 *
 * pci_ops 를 거치지 않고 _altera_pcie_cfg_read() 를 직접 부른다. 이 시점은
 * PCI 코어가 버스를 만들기 전(altera_pcie_probe 안)이라 struct pci_bus 가
 * 아직 없기 때문이다. 그래서 hide_rc_bar 나 valid_device 검사도 지나가지
 * 않는데, 대상이 언제나 Root Port 자신이라 문제가 되지 않는다.
 *
 * u32 로 받아 u16 에 대입해 상위를 버린다. 크기 인자로 sizeof(*value) 를
 * 넘겨 하드웨어에는 2바이트 접근으로 나간다.
 *
 * 반환값을 확인하지 않고 *value 에 대입한다. 실패 시 data 가 초기화되지
 * 않은 값일 수 있는데, 호출자들이 반환값을 보지 않으므로 그 값이 그대로
 * 쓰인다. 코드는 고치지 않고 이 관찰만 적어 둔다.
 *
 * 실행 컨텍스트: probe 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   altera_wait_link_retrain() / altera_pcie_retrain() → [이 함수]
 *     → _altera_pcie_cfg_read()
 */
static int altera_read_cap_word(struct altera_pcie *pcie, u8 busno,
				unsigned int devfn, int offset, u16 *value)
{
	u32 data;
	/* [한국어] 읽기 결과 */
	int ret;

	/* [한국어] pci_ops 를 거치지 않고 아래 계층을 직접 부른다. 이 시점은 PCI 코어가
	 * 버스를 만들기 전이라 struct pci_bus 가 아직 없기 때문이다 */
	ret = _altera_pcie_cfg_read(pcie, busno, devfn,
				    pcie->pcie_data->cap_offset + offset,
				    sizeof(*value),
				    &data);
	*value = data;
	return ret;
}

/* [한국어]
 * altera_write_cap_word - PCIe Capability 안의 16비트 레지스터를 쓴다
 *
 * @pcie: 이 컨트롤러.  @busno: 대상 버스.  @devfn: 대상 장치/기능
 * @offset: PCIe Capability 시작점으로부터의 오프셋
 * @value: 쓸 16비트 값
 * @return: 아래 쓰기가 돌려준 값
 *
 * altera_read_cap_word() 의 짝이다. 이 파일에서 쓰이는 곳은 한 군데 —
 * altera_pcie_retrain() 이 Link Control 의 재훈련 비트를 세울 때다.
 *
 * 읽기 판과 마찬가지로 pci_ops 를 거치지 않고 아래 계층을 직접 부른다.
 *
 * 실행 컨텍스트: probe 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   altera_pcie_retrain() → [이 함수] → _altera_pcie_cfg_write()
 */
static int altera_write_cap_word(struct altera_pcie *pcie, u8 busno,
				 unsigned int devfn, int offset, u16 value)
{
	return _altera_pcie_cfg_write(pcie, busno, devfn,
				      pcie->pcie_data->cap_offset + offset,
				      sizeof(value),
				      value);
}

/* [한국어]
 * altera_wait_link_retrain - 링크 재훈련이 끝나고 링크가 다시 설 때까지 기다린다
 *
 * @pcie: 이 컨트롤러.  @return: 없음
 *
 * altera_pcie_retrain() 이 재훈련 비트를 세운 뒤 부른다. 기다림이 두
 * 단계이고, 각각 원문 영어 주석이 붙어 있다.
 *
 *   1) "Wait for link training end." — Link Status 의 Link Training 비트가
 *      내려가기를 기다린다. 그 비트가 서 있는 동안은 하드웨어가 링크 속도와
 *      폭을 협상하는 중이다.
 *   2) "Wait for link is up" — 그 다음 실제로 링크가 섰는지를 세대별
 *      get_link_status 콜백으로 확인한다. 훈련이 끝났다고 반드시 링크가
 *      선 것은 아니므로 두 단계가 따로 필요하다.
 *
 * 두 단계 모두 같은 형태다 — jiffies 로 시작 시각을 재고, 100마이크로초씩
 * 쉬며 폴링하고, 상한(각각 LINK_RETRAIN_TIMEOUT 과 LINK_UP_TIMEOUT, 둘 다
 * HZ 이므로 1초)을 넘으면 오류를 남기고 포기한다.
 *
 * 포기해도 반환값이 없다. 재훈련은 성능 최적화이지 필수가 아니므로,
 * 실패해도 probe 를 계속 진행한다 — 링크는 이미 2.5GT/s 로 서 있다.
 *
 * time_after() 를 쓰는 것은 jiffies 넘침(wrap) 상황에서도 비교가 올바르게
 * 되도록 하기 위한 커널 관용구다.
 *
 * 실행 컨텍스트: probe 경로의 프로세스 컨텍스트. udelay 로만 쉬므로
 * CPU 를 붙들고 최대 2초까지 돌 수 있다.
 *
 * 호출 체인:
 *   altera_pcie_retrain() → [이 함수]
 *     → altera_read_cap_word() → (ops) get_link_status
 */
static void altera_wait_link_retrain(struct altera_pcie *pcie)
{
	struct device *dev = &pcie->pdev->dev;
	/* [한국어] 읽어 올 Link Status 값 */
	u16 reg16;
	/* [한국어] 시한을 재는 기준 시각 */
	unsigned long start_jiffies;

	/* Wait for link training end. */
	start_jiffies = jiffies;
	/* [한국어] 1단계 — 링크 훈련이 끝나기를 기다린다 */
	for (;;) {
		/* [한국어] Link Status 를 읽는다 */
		altera_read_cap_word(pcie, pcie->root_bus_nr, RP_DEVFN,
				     PCI_EXP_LNKSTA, &reg16);
		/* [한국어] Link Training 비트가 내려갔으면 훈련이 끝난 것이다 */
		if (!(reg16 & PCI_EXP_LNKSTA_LT))
			break;

		/* [한국어] 1초를 넘겼는지 본다. time_after 는 jiffies 넘침 상황에서도 비교가
		 * 올바르게 되도록 하는 커널 관용구다 */
		if (time_after(jiffies, start_jiffies + LINK_RETRAIN_TIMEOUT)) {
			/* [한국어] 포기하고 로그만 남긴다. 재훈련은 성능 최적화이지 필수가 아니므로
			 * probe 를 계속 진행한다 */
			dev_err(dev, "link retrain timeout\n");
			break;
		}
		udelay(100);
	}

	/* Wait for link is up */
	start_jiffies = jiffies;
	/* [한국어] 2단계 — 링크가 실제로 서기를 기다린다. 훈련이 끝났다고 반드시
	 * 링크가 선 것은 아니라 두 단계가 따로 필요하다 */
	for (;;) {
		/* [한국어] 세대별 판정에 위임한다 */
		if (pcie->pcie_data->ops->get_link_status(pcie))
			break;

		/* [한국어] 역시 1초 상한 */
		if (time_after(jiffies, start_jiffies + LINK_UP_TIMEOUT)) {
			/* [한국어] 포기하고 로그만 남긴다 */
			dev_err(dev, "link up timeout\n");
			break;
		}
		udelay(100);
	}
}

/* [한국어]
 * altera_pcie_retrain - 2.5GT/s 로 붙은 링크를 더 빠른 속도로 재훈련시킨다
 *
 * @pcie: 이 컨트롤러.  @return: 없음
 *
 * 바로 안쪽 원문 영어 주석이 조건을 밝힌다 — Root Port 가 2.5GB/s 보다
 * 빠른 속도를 지원하는데 현재 속도가 2.5GB/s 이면 재훈련 비트를 세운다.
 *
 * 왜 필요한가: PCIe 링크는 처음에 가장 느린 속도(2.5GT/s, Gen1)로 훈련을
 * 시작한다. 보통은 하드웨어가 알아서 더 빠른 속도로 올라가지만, 그렇지
 * 않은 경우가 있어 소프트웨어가 한 번 밀어 준다.
 *
 * 판정이 두 단계다.
 *   1) Link Capability 의 Supported Link Speeds 필드가 2.5GB/s 이하이면
 *      올릴 여지가 없으므로 돌아선다.
 *   2) Link Status 의 Current Link Speed 가 2.5GB/s 일 때만 재훈련한다.
 *      이미 빠른 속도로 붙었으면 건드리지 않는다.
 *
 * 재훈련은 Link Control 의 Retrain Link 비트(PCI_EXP_LNKCTL_RL)를 세우는
 * 것으로 시작된다. 읽고-OR-쓰기라 다른 설정은 보존된다. 그 뒤
 * altera_wait_link_retrain() 이 결과를 기다린다.
 *
 * 맨 앞에서 링크가 서 있지 않으면 곧바로 돌아선다 — 링크가 없으면 읽을
 * Capability 도 없고 재훈련할 대상도 없다.
 *
 * 세 표준 상수(PCI_EXP_LNKCAP_SLS, PCI_EXP_LNKSTA_CLS,
 * PCI_EXP_LNKCTL_RL)의 값은 include/linux/pci_regs.h 가 이 스파스
 * 체크아웃에 없어 확인하지 못했다. 코드가 앞의 둘을 마스크로,
 * 셋째를 세울 비트로 쓴다는 것만 알 수 있다.
 *
 * 실행 컨텍스트: probe 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   altera_pcie_probe() → altera_pcie_host_init() → [이 함수]
 *     → altera_read_cap_word() → altera_write_cap_word()
 *     → altera_wait_link_retrain()
 */
static void altera_pcie_retrain(struct altera_pcie *pcie)
{
	u16 linkcap, linkstat, linkctl;

	/* [한국어] 링크가 없으면 읽을 Capability 도 재훈련할 대상도 없다 */
	if (!pcie->pcie_data->ops->get_link_status(pcie))
		return;

	/*
	 * Set the retrain bit if the PCIe rootport support > 2.5GB/s, but
	 * current speed is 2.5 GB/s.
	 */
	altera_read_cap_word(pcie, pcie->root_bus_nr, RP_DEVFN, PCI_EXP_LNKCAP,
			     &linkcap);
	/* [한국어] 지원 속도가 2.5GB/s 이하면 올릴 여지가 없다 */
	if ((linkcap & PCI_EXP_LNKCAP_SLS) <= PCI_EXP_LNKCAP_SLS_2_5GB)
		return;

	/* [한국어] 현재 속도를 확인한다 */
	altera_read_cap_word(pcie, pcie->root_bus_nr, RP_DEVFN, PCI_EXP_LNKSTA,
			     &linkstat);
	/* [한국어] 이미 빠른 속도로 붙었으면 건드리지 않는다. 2.5GB/s 일 때만 재훈련한다 */
	if ((linkstat & PCI_EXP_LNKSTA_CLS) == PCI_EXP_LNKSTA_CLS_2_5GB) {
		/* [한국어] Link Control 을 읽어 온다 */
		altera_read_cap_word(pcie, pcie->root_bus_nr, RP_DEVFN,
				     PCI_EXP_LNKCTL, &linkctl);
		/* [한국어] Retrain Link 비트를 세운다. 읽고-OR-쓰기라 다른 설정은 보존된다 */
		linkctl |= PCI_EXP_LNKCTL_RL;
		/* [한국어] 되쓰는 순간 하드웨어가 재훈련을 시작한다 */
		altera_write_cap_word(pcie, pcie->root_bus_nr, RP_DEVFN,
				      PCI_EXP_LNKCTL, linkctl);

		altera_wait_link_retrain(pcie);
	}
}

/* [한국어]
 * altera_pcie_intx_map - INTx 가상 IRQ 하나를 설정한다
 *
 * @domain: 이 컨트롤러의 IRQ 도메인.  @irq: 배정된 커널 가상 IRQ 번호
 * @hwirq: 하드웨어 IRQ 번호(0~3, INTA~INTD).  @return: 항상 0
 *
 * irq_domain_ops 의 .map 콜백이다. 하위 장치의 INTx 를 커널 IRQ 로
 * 쓰려면 가상 IRQ 를 하나 만들어야 하고, 그때 이 함수가 그 IRQ 의
 * chip 과 handler 를 정한다.
 *
 * dummy_irq_chip 을 쓰는 것이 요점이다. 보통의 irq_chip 은 mask/unmask
 * 같은 콜백으로 인터럽트를 개별 제어하지만, 이 하드웨어에는 INTx 를
 * 하나씩 끄고 켜는 수단이 없다(P2A_INT_ENABLE 은 넷을 한꺼번에 다룬다).
 * 그래서 아무 일도 하지 않는 더미 chip 을 쓰고, 실제 처리는 체인 핸들러인
 * altera_pcie_isr() 이 전담한다.
 *
 * handle_simple_irq 는 하드웨어 확인응답(ack)이 필요 없는 핸들러다.
 * 확인응답은 altera_pcie_isr() 이 상태 레지스터에 직접 써서 한다.
 *
 * irq_set_chip_data 로 domain->host_data(즉 struct altera_pcie)를 붙여
 * 두지만, 이 파일에서 그것을 다시 꺼내 쓰는 곳은 없다. 코드는 고치지 않고
 * 이 관찰만 적어 둔다.
 *
 * 실행 컨텍스트: IRQ 매핑 경로의 프로세스 컨텍스트. 하위 장치가
 * 처음 인터럽트를 요청할 때 불린다.
 *
 * 호출 체인:
 *   (irq_domain 코어) → irq_domain_ops.map → [이 함수]
 */
static int altera_pcie_intx_map(struct irq_domain *domain, unsigned int irq,
				irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &dummy_irq_chip, handle_simple_irq);
	/* [한국어] domain->host_data(struct altera_pcie)를 이 IRQ 에 붙여 둔다.
	 * 다만 이 파일에서 그것을 다시 꺼내 쓰는 곳은 없다 */
	irq_set_chip_data(irq, domain->host_data);
	return 0;
}

/* [한국어] INTx 도메인의 연산 표 */
static const struct irq_domain_ops intx_domain_ops = {
	/* [한국어] 가상 IRQ 하나를 설정하는 콜백 */
	.map = altera_pcie_intx_map,
	.xlate = pci_irqd_intx_xlate,
};

/* [한국어]
 * altera_pcie_isr - V1/V2 의 INTx 체인 인터럽트 핸들러
 *
 * @desc: 상위 인터럽트 컨트롤러가 넘겨준 irq_desc.  @return: 없음
 *
 * 하위 장치들의 INTA~INTD 가 이 컨트롤러의 인터럽트 하나로 모여 들어온다.
 * 그것을 풀어 각 장치의 핸들러로 나눠 주는 것이 이 함수다.
 *
 * 체인 핸들러라 chained_irq_enter/exit 로 감싼다. 그 두 함수가 상위
 * 컨트롤러 쪽의 마스킹과 확인응답을 처리해, 이 안에서 도는 동안 같은
 * 인터럽트가 다시 들어오지 않게 한다.
 *
 * 바깥 while 루프가 있는 이유가 중요하다. 안쪽 for 루프를 도는 동안 새
 * 인터럽트가 들어올 수 있으므로, 상태 레지스터가 완전히 빌 때까지 다시
 * 읽는다. 이것이 없으면 그 사이에 도착한 인터럽트를 놓치고, 레벨 트리거
 * 방식이라면 인터럽트가 계속 걸리는 상태로 남는다.
 *
 * 안쪽에서 하는 일:
 *   - 먼저 상태 비트를 지운다(원문 주석 "clear interrupts"). 핸들러를
 *     부르기 전에 지우는 순서라, 핸들러가 도는 동안 도착한 인터럽트를
 *     잃지 않는다.
 *     1 << bit 를 쓰는 것으로 보아 이 레지스터는 RW1C 다 — 지우려는
 *     비트에 1 을 쓴다. 근거가 되는 하드웨어 문서는 이 트리에 없으나,
 *     코드가 그렇게 쓰고 있다.
 *   - generic_handle_domain_irq() 로 그 hwirq 에 매핑된 장치 핸들러를
 *     부른다. 매핑이 없으면(아무도 그 INTx 를 요청하지 않았으면) 오류가
 *     돌아오고, ratelimit 을 걸어 로그를 남긴다.
 *
 * P2A_INT_STS_ALL 로 마스킹하는 것은 이 레지스터에 INTx 넷 말고 다른
 * 비트가 있을 수 있어서로 보이나, 근거가 되는 문서는 이 트리에 없다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트. 잠들 수 없다.
 *
 * 호출 체인:
 *   (상위 인터럽트 컨트롤러) → [이 함수]
 *     → cra_readl() → cra_writel() → generic_handle_domain_irq()
 */
static void altera_pcie_isr(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	/* [한국어] 체인 데이터에서 되찾을 컨트롤러 */
	struct altera_pcie *pcie;
	/* [한국어] 오류 로그를 찍을 대상 */
	struct device *dev;
	/* [한국어] for_each_set_bit 이 unsigned long 을 요구하므로 그 타입으로 받는다 */
	unsigned long status;
	/* [한국어] 서 있는 비트 번호(0~3 = INTA~INTD) */
	u32 bit;
	/* [한국어] 핸들러 호출 결과 */
	int ret;

	/* [한국어] 상위 컨트롤러 쪽 마스킹과 확인응답을 처리한다. 이 안에서 도는 동안
	 * 같은 인터럽트가 다시 들어오지 않는다 */
	chained_irq_enter(chip, desc);
	/* [한국어] altera_pcie_parse_dt() 가 붙여 둔 컨트롤러를 되찾는다 */
	pcie = irq_desc_get_handler_data(desc);
	/* [한국어] 로그 대상 */
	dev = &pcie->pdev->dev;

	/* [한국어] 상태가 완전히 빌 때까지 다시 읽는다. 안쪽 루프를 도는 동안 새
	 * 인터럽트가 들어올 수 있으므로, 이것이 없으면 그것을 놓치고 레벨
	 * 트리거 방식이라면 인터럽트가 계속 걸린 상태로 남는다 */
	while ((status = cra_readl(pcie, P2A_INT_STATUS)
		/* [한국어] INTx 넷에 해당하는 비트만 남긴다 */
		& P2A_INT_STS_ALL) != 0) {
		for_each_set_bit(bit, &status, PCI_NUM_INTX) {
			/* clear interrupts */
			cra_writel(pcie, 1 << bit, P2A_INT_STATUS);

			/* [한국어] 그 hwirq 에 매핑된 장치 핸들러를 부른다 */
			ret = generic_handle_domain_irq(pcie->irq_domain, bit);
			/* [한국어] 매핑이 없으면 — 아무도 그 INTx 를 요청하지 않았으면 — 오류가 돌아온다 */
			if (ret)
				/* [한국어] ratelimit 을 걸어 로그를 남긴다. 매핑 없는 인터럽트가 반복되면
				 * 로그가 폭주하기 때문이다 */
				dev_err_ratelimited(dev, "unexpected IRQ, INT%d\n", bit);
		}
	}
	chained_irq_exit(chip, desc);
}

/* [한국어]
 * aglx_isr - V3 의 체인 인터럽트 핸들러(AER 전용)
 *
 * @desc: 상위 인터럽트 컨트롤러가 넘겨준 irq_desc.  @return: 없음
 *
 * V1/V2 의 altera_pcie_isr() 과 역할이 다르다. 저쪽은 하위 장치의 INTx
 * 넷을 나눠 주지만, 이쪽은 AER(오류 보고) 인터럽트 하나만 다룬다.
 *
 * 상태 레지스터의 주소 계산이 세 값의 합이다 —
 * "Hip" 창 + port_conf_offset + port_irq_status_offset.
 * port_conf_offset 이 타일 종류마다 다른 것이 이 세대의 특징이다
 * (f-tile 0x14000, p-tile 0x104000, r-tile 0x1300). 같은 드라이버가
 * 서로 다른 FPGA 타일을 지원하기 위한 구조이며, 그 값들의 근거가 되는
 * 하드웨어 문서는 이 트리에 없다.
 *
 * CFG_AER 비트가 서 있을 때만 처리한다.
 *   - 먼저 같은 비트를 되써서 지운다(RW1C 로 보이는 사용 방식).
 *   - generic_handle_domain_irq(domain, 0) — hwirq 0 하나에 매핑된
 *     핸들러를 부른다. INTx 넷을 쓰는 저쪽과 달리 이 세대는 0 번 하나만
 *     쓰는 셈이다.
 *
 * altera_pcie_init_irq_domain() 은 세대와 무관하게 PCI_NUM_INTX(4)개짜리
 * 도메인을 만드는데, 이 세대는 그중 0 번만 쓴다. 코드는 고치지 않고
 * 이 관찰만 적어 둔다.
 *
 * 바깥 while 루프가 없는 점도 저쪽과 다르다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트. 잠들 수 없다.
 *
 * 호출 체인:
 *   (상위 인터럽트 컨트롤러) → [이 함수]
 *     → readl() → writel() → generic_handle_domain_irq()
 */
static void aglx_isr(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	/* [한국어] 체인 데이터에서 되찾을 컨트롤러 */
	struct altera_pcie *pcie;
	/* [한국어] 오류 로그를 찍을 대상 */
	struct device *dev;
	/* [한국어] 포트 인터럽트 상태 */
	u32 status;
	/* [한국어] 핸들러 호출 결과 */
	int ret;

	/* [한국어] 상위 컨트롤러 쪽 처리를 맡긴다 */
	chained_irq_enter(chip, desc);
	/* [한국어] 컨트롤러를 되찾는다 */
	pcie = irq_desc_get_handler_data(desc);
	/* [한국어] 로그 대상 */
	dev = &pcie->pdev->dev;

	/* [한국어] 포트 설정 영역의 인터럽트 상태를 읽는다. 주소가 세 값의 합인 것은
	 * 포트 설정 영역의 위치가 타일 종류마다 다르기 때문이다 */
	status = readl(pcie->hip_base + pcie->pcie_data->port_conf_offset +
		       pcie->pcie_data->port_irq_status_offset);

	/* [한국어] AER 인터럽트인가. 이 세대는 이 비트 하나만 다룬다 */
	if (status & CFG_AER) {
		/* [한국어] 같은 비트를 되써서 지운다(RW1C 로 보이는 사용 방식).
		 * 핸들러를 부르기 전에 지우는 순서라, 처리 중 도착한 인터럽트를 잃지 않는다 */
		writel(CFG_AER, (pcie->hip_base + pcie->pcie_data->port_conf_offset +
				 pcie->pcie_data->port_irq_status_offset));

		/* [한국어] hwirq 0 에 매핑된 핸들러를 부른다. INTx 넷을 쓰는 V1/V2 와 달리
		 * 이 세대는 0 번 하나만 쓴다 */
		ret = generic_handle_domain_irq(pcie->irq_domain, 0);
		/* [한국어] 매핑이 없으면 오류가 돌아온다 */
		if (ret)
			/* [한국어] ratelimit 을 걸어 로그를 남긴다 */
			dev_err_ratelimited(dev, "unexpected IRQ %d\n", pcie->irq);
	}
	chained_irq_exit(chip, desc);
}

/* [한국어]
 * altera_pcie_init_irq_domain - 하위 장치의 INTx 를 받을 IRQ 도메인을 만든다
 *
 * @pcie: 이 컨트롤러.  @return: 0 = 성공, -ENOMEM = 도메인 생성 실패
 *
 * 원문 주석 "Setup INTx" 대로 INTx 용 선형 도메인을 만든다. 크기는
 * PCI_NUM_INTX(4) — INTA/INTB/INTC/INTD 넷이다.
 *
 * 선형(linear) 도메인은 hwirq 번호를 배열 인덱스로 쓰는 가장 단순한
 * 방식이다. hwirq 가 0~3 으로 작고 조밀하므로 해시나 트리가 필요 없다.
 *
 * dev_fwnode(dev) 로 장치 트리 노드를 도메인에 연결하는 것이 중요하다.
 * 하위 장치의 장치 트리 노드가 interrupt-map 으로 이 컨트롤러를 가리킬 때,
 * 그 연결을 따라 이 도메인을 찾게 된다.
 *
 * 마지막 인자 pcie 가 domain->host_data 가 되어 altera_pcie_intx_map() 에
 * 전달된다.
 *
 * V3 도 같은 크기의 도메인을 만들지만 실제로는 0 번 하나만 쓴다
 * (aglx_isr 참조). 세대별로 나누지 않고 공통으로 둔 것이며, 코드는
 * 고치지 않고 이 관찰만 적어 둔다.
 *
 * 에러 경로: 실패하면 -ENOMEM 을 올리고 altera_pcie_probe() 가 그대로
 * 반환한다. 이미 건 IRQ 체인 핸들러는 devm 이 아니라 손으로 건 것이지만,
 * probe 실패 경로에서 그것을 푸는 코드가 없다. 코드는 고치지 않고
 * 이 관찰만 적어 둔다.
 *
 * 실행 컨텍스트: probe 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   altera_pcie_probe() → [이 함수] → irq_domain_create_linear()
 */
static int altera_pcie_init_irq_domain(struct altera_pcie *pcie)
{
	struct device *dev = &pcie->pdev->dev;

	/* Setup INTx */
	pcie->irq_domain = irq_domain_create_linear(dev_fwnode(dev), PCI_NUM_INTX,
					&intx_domain_ops, pcie);
	/* [한국어] 도메인 생성 실패 */
	if (!pcie->irq_domain) {
		/* [한국어] 무엇이 실패했는지 남긴다 */
		dev_err(dev, "Failed to get a INTx IRQ domain\n");
		return -ENOMEM;
	}

	return 0;
}

/* [한국어]
 * altera_pcie_irq_teardown - IRQ 도메인과 체인 핸들러를 정리한다
 *
 * @pcie: 이 컨트롤러.  @return: 없음
 *
 * altera_pcie_remove() 가 부른다. 순서가 중요하다.
 *
 *   1) irq_set_chained_handler_and_data(irq, NULL, NULL) 로 체인 핸들러를
 *      먼저 뗀다. 이것을 나중에 하면 도메인이 사라진 뒤에도 인터럽트가
 *      들어와 해제된 메모리를 참조하게 된다.
 *   2) irq_domain_remove() 로 도메인을 없앤다.
 *   3) irq_dispose_mapping() 으로 이 컨트롤러 자신의 IRQ 매핑을 푼다.
 *
 * 3)의 대상이 pcie->irq 인 점에 주의한다 — 하위 장치의 INTx 가 아니라
 * 이 컨트롤러가 상위 컨트롤러에서 받은 IRQ 다.
 *
 * 실행 컨텍스트: 드라이버 제거 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   altera_pcie_remove() → [이 함수]
 */
static void altera_pcie_irq_teardown(struct altera_pcie *pcie)
{
	irq_set_chained_handler_and_data(pcie->irq, NULL, NULL);
	irq_domain_remove(pcie->irq_domain);
	irq_dispose_mapping(pcie->irq);
}

/* [한국어]
 * altera_pcie_parse_dt - 장치 트리에서 MMIO 창과 IRQ 를 가져온다
 *
 * @pcie: 이 컨트롤러.  @return: 0 = 성공, 음수 errno = 실패
 *
 * 이 드라이버가 하드웨어에 닿는 통로를 여는 함수다. 셋을 얻는다.
 *
 *   1) "Cra" 창 — 이름으로 찾아 devm 으로 ioremap 한다. TLP 송수신
 *      레지스터, 인터럽트 레지스터, LTSSM, 그리고 V3 의 하위 장치 config
 *      창이 모두 여기 있다. 모든 세대에 필수다.
 *
 *   2) "Hip" 창 — V2 와 V3 에만 있다. Root Port 자신의 config space 가
 *      여기 매핑되어 있고, V3 에서는 포트 설정/인터럽트 레지스터도 여기
 *      있다. V1 은 이 창이 없어 Root Port 자신에게도 TLP 를 보낸다.
 *
 *   3) IRQ — platform_get_irq() 로 얻고, 곧바로 세대별 rp_isr 콜백을
 *      체인 핸들러로 건다. devm 판이 아니라서 실패 경로와 remove 에서
 *      손으로 풀어야 하고, 그것을 altera_pcie_irq_teardown() 이 한다.
 *
 * devm_platform_ioremap_resource_byname 을 쓰므로 매핑 해제는 devm 이
 * 알아서 한다 — 그래서 이 함수에는 실패 시 되돌리는 코드가 없다.
 *
 * IRQ 를 여기서 곧바로 거는 것에 주의한다. altera_pcie_init_irq_domain()
 * 보다 먼저이므로, 이 시점과 도메인 생성 사이에 인터럽트가 들어오면
 * 핸들러가 아직 NULL 인 pcie->irq_domain 을 쓰게 된다. 실제로는 probe
 * 중에 하위 장치가 인터럽트를 낼 일이 없고 P2A 인터럽트도 아직 켜지지
 * 않았으므로 문제가 되지 않는 것으로 보인다. 코드는 고치지 않고
 * 이 관찰만 적어 둔다.
 *
 * 실행 컨텍스트: probe 경로의 프로세스 컨텍스트. ioremap 이 잠들 수 있다.
 *
 * 호출 체인:
 *   altera_pcie_probe() → [이 함수]
 *     → devm_platform_ioremap_resource_byname() → platform_get_irq()
 *     → irq_set_chained_handler_and_data()
 */
static int altera_pcie_parse_dt(struct altera_pcie *pcie)
{
	struct platform_device *pdev = pcie->pdev;

	/* [한국어] "Cra" 창을 이름으로 찾아 devm 으로 매핑한다. 모든 세대에 필수다 */
	pcie->cra_base = devm_platform_ioremap_resource_byname(pdev, "Cra");
	/* [한국어] 매핑 실패 */
	if (IS_ERR(pcie->cra_base))
		/* [한국어] 오류 포인터에서 errno 를 꺼내 올린다 */
		return PTR_ERR(pcie->cra_base);

	/* [한국어] V2 와 V3 에만 "Hip" 창이 있다 */
	if (pcie->pcie_data->version == ALTERA_PCIE_V2 ||
	    pcie->pcie_data->version == ALTERA_PCIE_V3) {
		/* [한국어] Root Port 자신의 config space 와 V3 의 포트 레지스터가 여기 있다 */
		pcie->hip_base = devm_platform_ioremap_resource_byname(pdev, "Hip");
		/* [한국어] 매핑 실패 */
		if (IS_ERR(pcie->hip_base))
			/* [한국어] 오류를 올린다 */
			return PTR_ERR(pcie->hip_base);
	}

	/* setup IRQ */
	pcie->irq = platform_get_irq(pdev, 0);
	/* [한국어] IRQ 획득 실패(음수 errno) */
	if (pcie->irq < 0)
		/* [한국어] 그대로 올린다 */
		return pcie->irq;

	/* [한국어] 세대별 ISR 을 체인 핸들러로 건다. devm 판이 아니라서 실패 경로와
	 * remove 에서 손으로 풀어야 하고, 그것을 altera_pcie_irq_teardown() 이 한다.
	 * 이 시점이 IRQ 도메인 생성보다 앞선 점에 주의 — probe 중에 인터럽트가
	 * 들어올 일이 없어 문제가 되지 않는 것으로 보인다 */
	irq_set_chained_handler_and_data(pcie->irq, pcie->pcie_data->ops->rp_isr, pcie);
	return 0;
}

/* [한국어]
 * altera_pcie_host_init - V1/V2 의 호스트 초기화(링크 재훈련)
 *
 * @pcie: 이 컨트롤러.  @return: 없음
 *
 * 지금은 altera_pcie_retrain() 한 줄을 감싸는 것이 전부다. 이름과 달리
 * "호스트 초기화" 라 부를 만한 다른 일은 하지 않는다.
 *
 * 호출자가 하나(altera_pcie_probe 의 V1/V2 갈래)뿐이라 함수로 나눌 이유가
 * 크지 않아 보이지만, 세대가 늘어날 때 초기화 절차를 여기 모으기 위한
 * 자리로 남겨 둔 것으로 보인다. 코드는 고치지 않고 이 관찰만 적어 둔다.
 *
 * V3 는 이 함수를 부르지 않는다. 그 세대의 초기화는 probe 안에서
 * AER 인터럽트를 켜는 writel 한 줄이다.
 *
 * 실행 컨텍스트: probe 경로의 프로세스 컨텍스트. 최대 2초 걸릴 수 있다.
 *
 * 호출 체인:
 *   altera_pcie_probe() → [이 함수] → altera_pcie_retrain()
 */
static void altera_pcie_host_init(struct altera_pcie *pcie)
{
	altera_pcie_retrain(pcie);
}

/* [한국어] V1 의 콜백 표. TLP 조립 경로를 쓰고 Root Port 전용 콜백이 없다 —
 * 그 세대는 Root Port 에도 TLP 를 보낸다 */
static const struct altera_pcie_ops altera_pcie_ops_1_0 = {
	/* [한국어] V1 의 완료 수신 구현 */
	.tlp_read_pkt = tlp_read_packet,
	.tlp_write_pkt = tlp_write_packet,
	.get_link_status = altera_pcie_link_up,
	.rp_isr = altera_pcie_isr,
};

/* [한국어] V2 의 콜백 표. 하위 장치는 TLP, Root Port 는 창 직접 접근이다 */
static const struct altera_pcie_ops altera_pcie_ops_2_0 = {
	/* [한국어] V2 의 완료 수신 구현(한 DWORD 씩 받는다) */
	.tlp_read_pkt = s10_tlp_read_packet,
	.tlp_write_pkt = s10_tlp_write_packet,
	.get_link_status = s10_altera_pcie_link_up,
	.rp_read_cfg = s10_rp_read_cfg,
	.rp_write_cfg = s10_rp_write_cfg,
	.rp_isr = altera_pcie_isr,
};

/* [한국어] V3 의 콜백 표. TLP 관련 콜백이 아예 없고, Root Port 와 하위 장치
 * 모두 창 직접 접근이다 */
static const struct altera_pcie_ops altera_pcie_ops_3_0 = {
	/* [한국어] V3 의 Root Port config 읽기 */
	.rp_read_cfg = aglx_rp_read_cfg,
	.rp_write_cfg = aglx_rp_write_cfg,
	.get_link_status = aglx_altera_pcie_link_up,
	.ep_read_cfg = aglx_ep_read_cfg,
	.ep_write_cfg = aglx_ep_write_cfg,
	.rp_isr = aglx_isr,
};

/* [한국어] V1(altr,pcie-root-port-1.0)의 상수 묶음 */
static const struct altera_pcie_data altera_pcie_1_0_data = {
	/* [한국어] V1 의 콜백 표를 연결한다 */
	.ops = &altera_pcie_ops_1_0,
	.cap_offset = 0x80,
	.version = ALTERA_PCIE_V1,
	.cfgrd0 = TLP_FMTTYPE_CFGRD0,
	.cfgrd1 = TLP_FMTTYPE_CFGRD1,
	.cfgwr0 = TLP_FMTTYPE_CFGWR0,
	.cfgwr1 = TLP_FMTTYPE_CFGWR1,
};

/* [한국어] V2(altr,pcie-root-port-2.0, Stratix 10)의 상수 묶음 */
static const struct altera_pcie_data altera_pcie_2_0_data = {
	/* [한국어] V2 의 콜백 표를 연결한다 */
	.ops = &altera_pcie_ops_2_0,
	.version = ALTERA_PCIE_V2,
	.cap_offset = 0x70,
	.cfgrd0 = S10_TLP_FMTTYPE_CFGRD0,
	.cfgrd1 = S10_TLP_FMTTYPE_CFGRD1,
	.cfgwr0 = S10_TLP_FMTTYPE_CFGWR0,
	.cfgwr1 = S10_TLP_FMTTYPE_CFGWR1,
};

/* [한국어] V3 의 f-tile 변종. 세 V3 변종이 같은 콜백 표를 공유하고
 * 포트 설정 영역의 오프셋만 다르다 */
static const struct altera_pcie_data altera_pcie_3_0_f_tile_data = {
	/* [한국어] V3 의 콜백 표를 연결한다 */
	.ops = &altera_pcie_ops_3_0,
	.version = ALTERA_PCIE_V3,
	.cap_offset = 0x70,
	.port_conf_offset = 0x14000,
	.port_irq_status_offset = AGLX_ROOT_PORT_IRQ_STATUS,
	.port_irq_enable_offset = AGLX_ROOT_PORT_IRQ_ENABLE,
};

/* [한국어] V3 의 p-tile 변종. 포트 설정 영역이 0x104000 으로 f-tile 과 다르다 */
static const struct altera_pcie_data altera_pcie_3_0_p_tile_data = {
	/* [한국어] 같은 V3 콜백 표 */
	.ops = &altera_pcie_ops_3_0,
	.version = ALTERA_PCIE_V3,
	.cap_offset = 0x70,
	.port_conf_offset = 0x104000,
	.port_irq_status_offset = AGLX_ROOT_PORT_IRQ_STATUS,
	.port_irq_enable_offset = AGLX_ROOT_PORT_IRQ_ENABLE,
};

/* [한국어] V3 의 r-tile 변종. 포트 설정 영역이 0x1300 이고, 그 안의 상태/활성화
 * 오프셋도 0x0/0x4 로 다른 두 변종과 다르다 */
static const struct altera_pcie_data altera_pcie_3_0_r_tile_data = {
	/* [한국어] 같은 V3 콜백 표 */
	.ops = &altera_pcie_ops_3_0,
	.version = ALTERA_PCIE_V3,
	.cap_offset = 0x70,
	.port_conf_offset = 0x1300,
	.port_irq_status_offset = 0x0,
	.port_irq_enable_offset = 0x4,
};

/* [한국어] 장치 트리 compatible 문자열과 상수 묶음의 대응표.
 * altera_pcie_probe() 의 of_device_get_match_data() 가 이 표를 뒤진다 */
static const struct of_device_id altera_pcie_of_match[] = {
	/* [한국어] V1 하드웨어 */
	{.compatible = "altr,pcie-root-port-1.0",
	 .data = &altera_pcie_1_0_data },
	{.compatible = "altr,pcie-root-port-2.0",
	 .data = &altera_pcie_2_0_data },
	{.compatible = "altr,pcie-root-port-3.0-f-tile",
	 .data = &altera_pcie_3_0_f_tile_data },
	{.compatible = "altr,pcie-root-port-3.0-p-tile",
	 .data = &altera_pcie_3_0_p_tile_data },
	{.compatible = "altr,pcie-root-port-3.0-r-tile",
	 .data = &altera_pcie_3_0_r_tile_data },
	{},
};

/* [한국어]
 * altera_pcie_probe - 플랫폼 장치를 잡아 PCI 호스트 브리지를 세운다
 *
 * @pdev: 장치 트리가 만든 플랫폼 장치
 * @return: 0 = 성공, -ENOMEM / -ENODEV, 또는 아래 단계가 돌려준 오류
 *
 * 이 드라이버의 진입점이다. 장치 트리에 altr,pcie-root-port-* 노드가 있으면
 * 플랫폼 버스가 여기로 온다.
 *
 * 절차:
 *   1) devm_pci_alloc_host_bridge(dev, sizeof(*pcie)) — 호스트 브리지와
 *      이 드라이버의 private 영역을 한 번에 잡는다. struct altera_pcie 를
 *      따로 kzalloc 하지 않는 이유가 이것이며, 해제도 devm 이 맡는다.
 *      pci_host_bridge_priv() 로 그 private 영역을 얻는다.
 *
 *   2) of_device_get_match_data() 로 세대별 상수 묶음을 고른다. 어느
 *      compatible 문자열로 매칭됐느냐에 따라 altera_pcie_1_0_data 부터
 *      altera_pcie_3_0_r_tile_data 까지 다섯 중 하나가 온다. 이 한 줄이
 *      이후의 모든 세대별 분기(ops 콜백, TLP fmt/type 값, capability
 *      오프셋, 포트 레지스터 오프셋)를 결정한다.
 *
 *   3) altera_pcie_parse_dt() 로 MMIO 창과 IRQ 를 얻는다.
 *   4) altera_pcie_init_irq_domain() 으로 INTx 도메인을 만든다.
 *
 *   5) 세대별 하드웨어 초기화.
 *      V1/V2: P2A 인터럽트 상태를 전부 지우고(원문 주석 "clear all
 *        interrupts") 전부 켠 다음(원문 주석 "enable all interrupts")
 *        링크를 재훈련한다. 지우기를 켜기보다 먼저 하는 순서가 중요하다 —
 *        반대로 하면 부팅 전에 남아 있던 상태 때문에 켜자마자 가짜
 *        인터럽트가 들어온다.
 *      V3: 포트 설정 영역의 인터럽트 활성화 레지스터에 CFG_AER 만 켠다.
 *        이 세대는 INTx 를 이 방식으로 다루지 않는다.
 *
 *   6) bridge 에 sysdata(이 컨트롤러), busnr, ops 를 채우고
 *      pci_host_probe() 로 PCI 코어에 넘긴다. 그때부터 코어가 버스를
 *      열거하며 altera_pcie_cfg_read/write 를 부르기 시작한다.
 *
 * bridge->busnr 에 pcie->root_bus_nr 을 넣는데, 이 시점에는 그 값이
 * 0 이다(kzalloc 된 뒤 아무도 세우지 않았다). 이후 PCI 코어가 Root Port 의
 * PCI_PRIMARY_BUS 에 쓸 때 config 쓰기 경로가 그것을 엿보아 갱신한다.
 *
 * 에러 경로: 각 단계가 실패하면 로그를 남기고 그대로 반환한다. devm 이
 * 할당과 매핑을 되돌리지만, altera_pcie_parse_dt() 가 건 체인 IRQ 핸들러는
 * 되돌리지 않는다. 코드는 고치지 않고 이 관찰만 적어 둔다.
 *
 * 실행 컨텍스트: 드라이버 바인드 경로의 프로세스 컨텍스트. 잠들 수 있다.
 *
 * 호출 체인:
 *   (플랫폼 버스 바인드) → [이 함수]
 *     → altera_pcie_parse_dt() → altera_pcie_init_irq_domain()
 *     → altera_pcie_host_init() → pci_host_probe()
 */
static int altera_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	/* [한국어] 이 컨트롤러의 상태. 아래 host bridge 의 private 영역에 얹힌다 */
	struct altera_pcie *pcie;
	/* [한국어] PCI 코어에 넘길 호스트 브리지 */
	struct pci_host_bridge *bridge;
	/* [한국어] 각 단계의 결과 */
	int ret;
	/* [한국어] 세대별 상수 묶음 */
	const struct altera_pcie_data *data;

	/* [한국어] 호스트 브리지와 이 드라이버의 private 영역을 한 번에 잡는다.
	 * struct altera_pcie 를 따로 할당하지 않는 이유이며, 해제도 devm 이 맡는다 */
	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*pcie));
	/* [한국어] 메모리 부족 */
	if (!bridge)
		return -ENOMEM;

	/* [한국어] 방금 잡은 private 영역을 얻는다 */
	pcie = pci_host_bridge_priv(bridge);
	/* [한국어] 이후 로그 출력과 리소스 획득에 쓴다 */
	pcie->pdev = pdev;
	/* [한국어] remove 에서 되찾을 수 있도록 매달아 둔다 */
	platform_set_drvdata(pdev, pcie);

	/* [한국어] 어느 compatible 로 매칭됐는지에 따라 다섯 상수 묶음 중 하나를 얻는다.
	 * 이 한 줄이 이후의 모든 세대별 분기를 결정한다 */
	data = of_device_get_match_data(&pdev->dev);
	/* [한국어] 매칭 데이터가 없으면 지원하지 않는 하드웨어다 */
	if (!data)
		return -ENODEV;

	/* [한국어] 이후 이 파일 전체가 pcie->pcie_data 를 통해 세대별 값을 본다 */
	pcie->pcie_data = data;

	/* [한국어] MMIO 창과 IRQ 를 얻는다 */
	ret = altera_pcie_parse_dt(pcie);
	/* [한국어] 실패 */
	if (ret) {
		/* [한국어] 무엇이 실패했는지 남긴다 */
		dev_err(dev, "Parsing DT failed\n");
		return ret;
	}

	/* [한국어] INTx 도메인을 만든다 */
	ret = altera_pcie_init_irq_domain(pcie);
	/* [한국어] 실패 */
	if (ret) {
		/* [한국어] 무엇이 실패했는지 남긴다 */
		dev_err(dev, "Failed creating IRQ Domain\n");
		return ret;
	}

	/* [한국어] V1/V2 의 하드웨어 초기화 갈래 */
	if (pcie->pcie_data->version == ALTERA_PCIE_V1 ||
	    pcie->pcie_data->version == ALTERA_PCIE_V2) {
		/* clear all interrupts */
		cra_writel(pcie, P2A_INT_STS_ALL, P2A_INT_STATUS);
		/* enable all interrupts */
		cra_writel(pcie, P2A_INT_ENA_ALL, P2A_INT_ENABLE);
		altera_pcie_host_init(pcie);
	/* [한국어] V3 의 초기화 갈래 — AER 인터럽트만 켠다 */
	} else if (pcie->pcie_data->version == ALTERA_PCIE_V3) {
		writel(CFG_AER,
		       pcie->hip_base + pcie->pcie_data->port_conf_offset +
		       pcie->pcie_data->port_irq_enable_offset);
	}

	/* [한국어] config 콜백들이 bus->sysdata 로 이 값을 되찾는다 */
	bridge->sysdata = pcie;
	/* [한국어] 이 시점의 root_bus_nr 은 0 이다(할당 시 0 초기화). 이후 PCI 코어가
	 * Root Port 의 PCI_PRIMARY_BUS 에 쓸 때 config 쓰기 경로가 엿보아 갱신한다 */
	bridge->busnr = pcie->root_bus_nr;
	/* [한국어] 이 파일이 코어에 노출하는 연산 표를 건다 */
	bridge->ops = &altera_pcie_ops;

	return pci_host_probe(bridge);
}

/* [한국어]
 * altera_pcie_remove - 이 컨트롤러를 떼어 낸다
 *
 * @pdev: 제거되는 플랫폼 장치.  @return: 없음
 *
 * 순서가 이 함수의 전부다.
 *
 *   1) pci_stop_root_bus() — 버스 아래 장치들의 드라이버를 떼어 낸다.
 *      이 시점까지는 config 접근이 여전히 동작해야 한다.
 *   2) pci_remove_root_bus() — 장치와 버스 객체를 없앤다.
 *   3) altera_pcie_irq_teardown() — 그제야 IRQ 를 정리한다.
 *
 * IRQ 정리가 마지막인 이유가 중요하다. 1)과 2)가 도는 동안 하위 장치가
 * 아직 인터럽트를 낼 수 있으므로, 그것을 받을 도메인과 핸들러가 살아
 * 있어야 한다.
 *
 * MMIO 매핑과 struct altera_pcie 자체는 devm 이 이 함수 뒤에 정리한다.
 *
 * 실행 컨텍스트: 드라이버 언바인드 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   (플랫폼 버스 언바인드) → [이 함수]
 *     → pci_stop_root_bus() → pci_remove_root_bus() → altera_pcie_irq_teardown()
 */
static void altera_pcie_remove(struct platform_device *pdev)
{
	struct altera_pcie *pcie = platform_get_drvdata(pdev);
	/* [한국어] private 영역에서 호스트 브리지로 되돌아간다. probe 의
	 * pci_host_bridge_priv() 와 짝이다 */
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(pcie);

	pci_stop_root_bus(bridge->bus);
	pci_remove_root_bus(bridge->bus);
	altera_pcie_irq_teardown(pcie);
}

/* [한국어] 플랫폼 드라이버 서술자. 이 구조체가 이 파일과 플랫폼 버스를 잇는다 */
static struct platform_driver altera_pcie_driver = {
	/* [한국어] 바인드 시 진입점 */
	.probe = altera_pcie_probe,
	.remove = altera_pcie_remove,
	.driver = {
		/* [한국어] sysfs 와 로그에 보이는 드라이버 이름 */
		.name = "altera-pcie",
		.of_match_table = altera_pcie_of_match,
	},
};

MODULE_DEVICE_TABLE(of, altera_pcie_of_match);
module_platform_driver(altera_pcie_driver);
MODULE_DESCRIPTION("Altera PCIe host controller driver");
MODULE_LICENSE("GPL v2");
